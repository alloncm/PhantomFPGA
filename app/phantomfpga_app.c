// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA Userspace Application v3.0 - ASCII Animation Edition
 *
 * This application demonstrates how to interact with the PhantomFPGA
 * kernel driver. It configures the device, mmaps the descriptor buffers,
 * and polls for incoming frames with validation.
 *
 * v3.0 streams pre-built ASCII animation frames (fixed 5120 bytes each).
 * Build a driver, stream frames over TCP, watch a cartoon in your terminal.
 *
 * "Because nothing says 'I learned DMA' like ASCII art."
 *
 * YOUR MISSION (should you choose to accept it):
 * Complete all the TODO sections to build a working frame receiver.
 * The driver is your partner - make sure you understand the UAPI header
 * before diving in.
 *
 * Usage:
 *   ./phantomfpga_app --rate 25 --tcp-server 5000
 *   ./phantomfpga_app --help
 *   ./phantomfpga_app --stats
 *
 * Then from your host: phantomfpga_view localhost:5000
 *
 * Author: PhantomFPGA Training Team
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* Include the driver UAPI header for ioctl definitions */
#include "phantomfpga_uapi.h"

/* ------------------------------------------------------------------------ */
/* Constants and Defaults                                                   */
/* ------------------------------------------------------------------------ */

#define APP_NAME            "phantomfpga_app"
#define APP_VERSION         "3.0.0"

/* Device node - matches driver */
#define DEVICE_PATH         "/dev/phantomfpga0"

/* Default configuration values */
#define DEFAULT_FRAME_RATE      25      /* 25 fps for smooth animation */
#define DEFAULT_DESC_COUNT      256     /* Must be power of 2 */
#define DEFAULT_IRQ_COUNT       8
#define DEFAULT_IRQ_TIMEOUT     40000   /* 40ms = 1 frame at 25fps */

/* Statistics update interval in seconds */
#define STATS_INTERVAL_SEC  1

/* CRC32 polynomial (IEEE 802.3) */
#define CRC32_POLY          0xEDB88320

/* Frame CRC offset */
#define FRAME_CRC_OFFSET    (PHANTOMFPGA_FRAME_SIZE - 4)

/* ------------------------------------------------------------------------ */
/* Application Context                                                      */
/* ------------------------------------------------------------------------ */

struct app_context {
    /* Device state */
    int fd;                     /* Device file descriptor */
    void *buffer_pool;          /* mmap'd buffer pool */
    size_t buffer_pool_size;    /* Size of mmap'd region */

    /* Configuration */
    uint32_t desc_count;
    uint32_t frame_rate;        /* Frames per second (1-60) */
    uint32_t buffer_size;       /* Per-buffer size in bytes */

    /* Runtime state */
    uint32_t last_seq;          /* Last seen sequence number */
    bool seq_initialized;       /* Have we seen the first frame? */

    /* Statistics */
    uint64_t frames_received;   /* Total frames we processed */
    uint64_t frames_valid;      /* Frames that passed validation */
    uint64_t seq_errors;        /* Sequence discontinuities */
    uint64_t magic_errors;      /* Invalid magic number */
    uint64_t crc_errors;        /* CRC validation failures */
    struct timespec start_time; /* When we started receiving */

    /* Flags */
    bool verbose;
    bool stats_only;
    bool validate_crc;          /* Enable CRC validation */
    bool running;

    /* Network streaming */
    int tcp_server_sock;        /* TCP server listen socket (-1 if disabled) */
    int tcp_client_fd;          /* Connected TCP client fd (-1 if none) */
    uint16_t tcp_server_port;   /* TCP server listen port */

    uint64_t net_frames_sent;   /* Frames sent over network */
    uint64_t net_bytes_sent;    /* Bytes sent over network */
    uint64_t net_send_errors;   /* Network send errors */
};

/* Global context for signal handler */
static struct app_context *g_ctx = NULL;

/* CRC32 lookup table - initialized once */
static uint32_t crc32_table[256];
static bool crc32_table_initialized = false;

/* ------------------------------------------------------------------------ */
/* Forward Declarations                                                     */
/* ------------------------------------------------------------------------ */

static void print_usage(const char *prog_name);
static int parse_arguments(int argc, char *argv[], struct app_context *ctx);
static int open_device(struct app_context *ctx);
static int configure_device(struct app_context *ctx);
static int setup_mmap(struct app_context *ctx);
static int start_streaming(struct app_context *ctx);
static int stop_streaming(struct app_context *ctx);
static void main_loop(struct app_context *ctx);
static int process_frame(struct app_context *ctx, const void *buffer, uint32_t actual_len);
static bool validate_frame(struct app_context *ctx, const void *frame, uint32_t frame_size);
static void print_statistics(struct app_context *ctx);
static void cleanup(struct app_context *ctx);
static void signal_handler(int sig);

/* Network streaming functions */
static int setup_tcp_server(struct app_context *ctx, uint16_t port);
static void accept_tcp_client(struct app_context *ctx);
static int stream_frame(struct app_context *ctx, const void *data, size_t len);
static void cleanup_network(struct app_context *ctx);

/* CRC32 functions */
static void init_crc32_table(void);
static uint32_t compute_crc32(const void *data, size_t len);

/* ------------------------------------------------------------------------ */
/* CRC32 Implementation                                                     */
/* IEEE 802.3 polynomial - same as the device uses                          */
/* ------------------------------------------------------------------------ */

static void init_crc32_table(void)
{
    if (crc32_table_initialized)
        return;

    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ CRC32_POLY;
            else
                crc >>= 1;
        }
        crc32_table[i] = crc;
    }
    crc32_table_initialized = true;
}

static uint32_t compute_crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------ */
/* Network Streaming Implementation                                         */
/* ------------------------------------------------------------------------ */

/*
 * Set up TCP server socket for accepting viewer connections
 */
static int setup_tcp_server(struct app_context *ctx, uint16_t port)
{
    struct sockaddr_in addr;
    int opt = 1;

    ctx->tcp_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (ctx->tcp_server_sock < 0) {
        perror("socket(TCP server)");
        return -1;
    }

    /* Allow address reuse */
    if (setsockopt(ctx->tcp_server_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt(SO_REUSEADDR)");
        close(ctx->tcp_server_sock);
        ctx->tcp_server_sock = -1;
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(ctx->tcp_server_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind(TCP server)");
        close(ctx->tcp_server_sock);
        ctx->tcp_server_sock = -1;
        return -1;
    }

    if (listen(ctx->tcp_server_sock, 1) < 0) {
        perror("listen(TCP server)");
        close(ctx->tcp_server_sock);
        ctx->tcp_server_sock = -1;
        return -1;
    }

    /* Set non-blocking for accept */
    fcntl(ctx->tcp_server_sock, F_SETFL, O_NONBLOCK);

    printf("[NET] TCP server listening on port %u\n", port);
    printf("[NET] From host: phantomfpga_view localhost:%u\n", port);
    return 0;
}

/*
 * Accept a pending TCP client connection (non-blocking)
 */
static void accept_tcp_client(struct app_context *ctx)
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int fd;

    if (ctx->tcp_server_sock < 0)
        return;

    /* Already have a client? */
    if (ctx->tcp_client_fd >= 0)
        return;

    fd = accept(ctx->tcp_server_sock, (struct sockaddr *)&client_addr, &client_len);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            perror("accept");
        return;
    }

    ctx->tcp_client_fd = fd;
    printf("[NET] Viewer connected from %s:%u\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
}

/*
 * Stream a frame to connected TCP client
 * Frame format: 4-byte length prefix (network order) + frame data
 */
static int stream_frame(struct app_context *ctx, const void *data, size_t len)
{
    ssize_t ret;

    /* TCP server - send to connected viewer */
    if (ctx->tcp_client_fd >= 0) {
        /* Send length prefix (4 bytes) + data for framing */
        uint32_t frame_len = htonl((uint32_t)len);
        ret = send(ctx->tcp_client_fd, &frame_len, sizeof(frame_len), MSG_NOSIGNAL);
        if (ret > 0) {
            ret = send(ctx->tcp_client_fd, data, len, MSG_NOSIGNAL);
        }
        if (ret < 0) {
            if (errno == EPIPE || errno == ECONNRESET) {
                printf("[NET] Viewer disconnected\n");
                close(ctx->tcp_client_fd);
                ctx->tcp_client_fd = -1;
            } else {
                if (ctx->verbose)
                    perror("send(TCP)");
                ctx->net_send_errors++;
            }
            return 0;
        } else {
            ctx->net_frames_sent++;
            ctx->net_bytes_sent += len;
            return 1;
        }
    }

    return 0;
}

/*
 * Clean up all network resources
 */
static void cleanup_network(struct app_context *ctx)
{
    if (ctx->tcp_client_fd >= 0) {
        close(ctx->tcp_client_fd);
        ctx->tcp_client_fd = -1;
    }
    if (ctx->tcp_server_sock >= 0) {
        close(ctx->tcp_server_sock);
        ctx->tcp_server_sock = -1;
    }
}

/* ------------------------------------------------------------------------ */
/* Main Entry Point                                                         */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct app_context ctx = {0};
    int ret;

    /* Initialize CRC table */
    init_crc32_table();

    /* Initialize context with defaults */
    ctx.fd = -1;
    ctx.desc_count = DEFAULT_DESC_COUNT;
    ctx.frame_rate = DEFAULT_FRAME_RATE;
    ctx.validate_crc = true;
    ctx.running = true;

    /* Initialize network socket fds to disabled */
    ctx.tcp_server_sock = -1;
    ctx.tcp_client_fd = -1;

    /* Set up global context for signal handler */
    g_ctx = &ctx;

    /* Parse command line arguments */
    ret = parse_arguments(argc, argv, &ctx);
    if (ret != 0) {
        return (ret > 0) ? 0 : 1;
    }

    /* Install signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Set up network streaming if requested */
    if (ctx.tcp_server_port > 0) {
        if (setup_tcp_server(&ctx, ctx.tcp_server_port) < 0) {
            return 1;
        }
    }

    /* Open the device */
    ret = open_device(&ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to open device: %s\n", strerror(-ret));
        cleanup_network(&ctx);
        return 1;
    }

    /* If stats-only mode, just print stats and exit */
    if (ctx.stats_only) {
        print_statistics(&ctx);
        cleanup(&ctx);
        return 0;
    }

    /* Configure the device */
    ret = configure_device(&ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to configure device: %s\n", strerror(-ret));
        cleanup(&ctx);
        return 1;
    }

    /* Set up mmap for zero-copy frame access */
    ret = setup_mmap(&ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to mmap buffer pool: %s\n", strerror(-ret));
        cleanup(&ctx);
        return 1;
    }

    /* Start streaming */
    ret = start_streaming(&ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to start streaming: %s\n", strerror(-ret));
        cleanup(&ctx);
        return 1;
    }

    printf("PhantomFPGA v3.0 ASCII Animation streaming started.\n");
    printf("  Descriptor count: %u\n", ctx.desc_count);
    printf("  Frame size:       %u bytes (fixed)\n", PHANTOMFPGA_FRAME_SIZE);
    printf("  Frame rate:       %u fps\n", ctx.frame_rate);
    printf("  Total frames:     %u (10 sec loop)\n", PHANTOMFPGA_FRAME_COUNT);
    printf("  CRC validation:   %s\n", ctx.validate_crc ? "enabled" : "disabled");
    if (ctx.tcp_server_sock >= 0) {
        printf("  TCP server:       port %u (waiting for viewer)\n", ctx.tcp_server_port);
    }
    printf("Press Ctrl+C to stop.\n\n");

    /* Record start time for rate calculations */
    clock_gettime(CLOCK_MONOTONIC, &ctx.start_time);

    /* Enter main processing loop */
    main_loop(&ctx);

    /* Stop streaming */
    stop_streaming(&ctx);

    /* Final statistics */
    printf("\n--- Final Statistics ---\n");
    print_statistics(&ctx);

    /* Clean up */
    cleanup(&ctx);

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Command Line Parsing                                                     */
/* ------------------------------------------------------------------------ */

static void print_usage(const char *prog_name)
{
    printf("Usage: %s [OPTIONS]\n", prog_name);
    printf("\n");
    printf("PhantomFPGA v3.0 userspace application - receives and streams ASCII\n");
    printf("animation frames from the PhantomFPGA SG-DMA device.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -r, --rate RATE       Frame rate 1-60 fps (default: %d)\n", DEFAULT_FRAME_RATE);
    printf("  -n, --desc-count N    Descriptor count, power of 2 (default: %d)\n",
           DEFAULT_DESC_COUNT);
    printf("  -c, --no-crc          Disable CRC validation\n");
    printf("  -S, --stats           Print device statistics and exit\n");
    printf("  -V, --verbose         Enable verbose output\n");
    printf("  -h, --help            Show this help message\n");
    printf("  --version             Show version information\n");
    printf("\n");
    printf("Network Streaming:\n");
    printf("  --tcp-server PORT     Listen for viewer connections on PORT\n");
    printf("\n");
    printf("  QEMU: Host is at 10.0.2.2, port 5000 forwarded.\n");
    printf("\n");
    printf("Frame Format (5120 bytes each):\n");
    printf("  0x0000: Header (16 bytes) - [magic], [sequence], [reserved]\n");
    printf("  0x0010: Payload: 2D array (4995 bytes) - 110 cols x 45 rows\n");
    printf("  0x1393: Padding (105 bytes)\n");
    printf("  0x13FC: CRC32 (4 bytes)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  # Start streaming, wait for viewer on port 5000\n");
    printf("  %s --tcp-server 5000\n", prog_name);
    printf("\n");
    printf("  # From host, view the animation:\n");
    printf("  phantomfpga_view localhost:5000\n");
    printf("\n");
}

static int parse_arguments(int argc, char *argv[], struct app_context *ctx)
{
    static struct option long_options[] = {
        {"rate",       required_argument, 0, 'r'},
        {"desc-count", required_argument, 0, 'n'},
        {"no-crc",     no_argument,       0, 'c'},
        {"stats",      no_argument,       0, 'S'},
        {"verbose",    no_argument,       0, 'V'},
        {"help",       no_argument,       0, 'h'},
        {"version",    no_argument,       0, 0x100},
        {"tcp-server", required_argument, 0, 0x102},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "r:n:cSVh",
                              long_options, &option_index)) != -1) {
        switch (opt) {
        case 'r':
            ctx->frame_rate = (uint32_t)strtoul(optarg, NULL, 10);
            if (ctx->frame_rate < 1 || ctx->frame_rate > 60) {
                fprintf(stderr, "Frame rate must be 1-60 fps\n");
                return -1;
            }
            break;
        case 'n':
            ctx->desc_count = (uint32_t)strtoul(optarg, NULL, 10);
            if ((ctx->desc_count & (ctx->desc_count - 1)) != 0 ||
                ctx->desc_count < 4 || ctx->desc_count > 4096) {
                fprintf(stderr, "Descriptor count must be power of 2, 4-4096\n");
                return -1;
            }
            break;
        case 'c':
            ctx->validate_crc = false;
            break;
        case 'S':
            ctx->stats_only = true;
            break;
        case 'V':
            ctx->verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;
        case 0x100:  /* --version */
            printf("%s version %s\n", APP_NAME, APP_VERSION);
            return 1;
        case 0x102:  /* --tcp-server PORT */
            ctx->tcp_server_port = (uint16_t)strtoul(optarg, NULL, 10);
            if (ctx->tcp_server_port == 0) {
                fprintf(stderr, "Invalid TCP server port: %s\n", optarg);
                return -1;
            }
            break;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Device Operations                                                        */
/* ------------------------------------------------------------------------ */

static int open_device(struct app_context *ctx)
{
    /*
     * TODO: Open the PhantomFPGA device node
     *
     * STEPS:
     * 1. Use open() with O_RDWR flag to open DEVICE_PATH
     * 2. Check for errors (returns -1 on failure)
     * 3. Store the file descriptor in ctx->fd
     *
     * EXAMPLE:
     *   ctx->fd = open(DEVICE_PATH, O_RDWR);
     *   if (ctx->fd < 0) {
     *       return -errno;
     *   }
     *   return 0;
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement open_device()\n");
    return -ENODEV;
    /* --- END YOUR CODE --- */
}

static int configure_device(struct app_context *ctx)
{
    /*
     * TODO: Configure the PhantomFPGA device via IOCTL
     *
     * STEPS:
     * 1. Create a struct phantomfpga_config and fill it
     * 2. Zero out the reserved fields
     * 3. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_SET_CFG, &config)
     * 4. Check for errors
     *
     * STRUCTURE (from phantomfpga_uapi.h):
     *   struct phantomfpga_config {
     *       __u32 desc_count;           // Descriptor count (power of 2, 4-4096)
     *       __u32 frame_rate;           // Frames per second (1-60)
     *       __u16 irq_coalesce_count;   // IRQ after N completions
     *       __u16 irq_coalesce_timeout; // IRQ timeout in microseconds
     *       __u32 reserved[4];          // Must be zero
     *   };
     *
     * EXAMPLE:
     *   struct phantomfpga_config config = {0};
     *   config.desc_count = ctx->desc_count;
     *   config.frame_rate = ctx->frame_rate;
     *   config.irq_coalesce_count = DEFAULT_IRQ_COUNT;
     *   config.irq_coalesce_timeout = DEFAULT_IRQ_TIMEOUT;
     *
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_SET_CFG, &config) < 0) {
     *       return -errno;
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement configure_device()\n");
    return -ENOSYS;
    /* --- END YOUR CODE --- */
}

static int setup_mmap(struct app_context *ctx)
{
    /*
     * TODO: Map the descriptor buffer pool into userspace
     *
     * STEPS:
     * 1. Get buffer info via PHANTOMFPGA_IOCTL_GET_BUFFER_INFO
     * 2. Store buffer_size for later use
     * 3. Call mmap() with total_size from the ioctl
     * 4. Store the mapped address in ctx->buffer_pool
     *
     * GET BUFFER INFO:
     *   struct phantomfpga_buffer_info info;
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_GET_BUFFER_INFO, &info) < 0)
     *       return -errno;
     *   ctx->buffer_size = info.buffer_size;
     *
     * MMAP:
     *   ctx->buffer_pool = mmap(NULL, info.total_size,
     *                           PROT_READ, MAP_SHARED,
     *                           ctx->fd, 0);
     *   if (ctx->buffer_pool == MAP_FAILED) {
     *       return -errno;
     *   }
     *   ctx->buffer_pool_size = info.total_size;
     *
     * NOTE: Buffer layout is:
     *   buffer_pool + (desc_index * buffer_size) = start of buffer for descriptor
     *   Each buffer contains: [frame data][completion struct at end]
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement setup_mmap()\n");
    return -ENOSYS;
    /* --- END YOUR CODE --- */
}

static int start_streaming(struct app_context *ctx)
{
    /*
     * TODO: Start frame streaming
     *
     * STEPS:
     * 1. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_START)
     * 2. Check for errors
     *
     * After this call, the device will start transmitting frames.
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement start_streaming()\n");
    return -ENOSYS;
    /* --- END YOUR CODE --- */
}

static int stop_streaming(struct app_context *ctx)
{
    /*
     * TODO: Stop frame streaming
     *
     * STEPS:
     * 1. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_STOP)
     * 2. Errors are OK (might already be stopped)
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement stop_streaming()\n");
    return 0;
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Main Processing Loop                                                     */
/* ------------------------------------------------------------------------ */

static void main_loop(struct app_context *ctx)
{
    /*
     * TODO: Implement the main poll/processing loop
     *
     * The loop should:
     * 1. Use poll() to wait for completed descriptors (POLLIN on ctx->fd)
     * 2. When data is available, read frames from completed buffers
     * 3. Process and validate each frame
     * 4. Mark frames as consumed via PHANTOMFPGA_IOCTL_CONSUME_FRAME
     * 5. Stream frames to connected viewer
     * 6. Periodically print statistics
     * 7. Exit when ctx->running becomes false
     *
     * POLL SETUP:
     *   struct pollfd pfd = {
     *       .fd = ctx->fd,
     *       .events = POLLIN
     *   };
     *
     * PROCESSING LOOP OUTLINE:
     *   while (ctx->running) {
     *       accept_tcp_client(ctx);  // Check for viewer connections
     *
     *       int ret = poll(&pfd, 1, 100);  // 100ms timeout
     *       if (ret < 0) {
     *           if (errno == EINTR) continue;
     *           break;
     *       }
     *       if (pfd.revents & POLLIN) {
     *           // Process completed frames
     *           // Use read() to get frame data
     *           // Call process_frame() to validate and stream
     *           // Frames are auto-consumed after read()
     *       }
     *   }
     *
     * READING FRAMES WITH read():
     *   uint8_t frame_buffer[PHANTOMFPGA_FRAME_SIZE];
     *   ssize_t n = read(ctx->fd, frame_buffer, sizeof(frame_buffer));
     *   if (n == PHANTOMFPGA_FRAME_SIZE) {
     *       process_frame(ctx, frame_buffer, n);
     *   }
     */

    /* --- YOUR CODE HERE --- */
    struct pollfd pfd;
    time_t last_stats_time = time(NULL);

    (void)pfd;
    (void)last_stats_time;

    fprintf(stderr, "TODO: Implement main_loop()\n");

    /* Placeholder loop - trainees will implement proper poll-based processing */
    while (ctx->running) {
        /* Accept pending TCP clients (non-blocking) */
        accept_tcp_client(ctx);

        sleep(1);
        if (ctx->verbose) {
            print_statistics(ctx);
        }
    }
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Frame Processing                                                         */
/* ------------------------------------------------------------------------ */

/* Unused in skeleton - trainees will call this from main loop */
__attribute__((unused))
static int process_frame(struct app_context *ctx, const void *buffer, uint32_t actual_len)
{
    /*
     * TODO: Process a single frame from a buffer
     *
     * STEPS:
     * 1. Validate the frame using validate_frame()
     * 2. Update statistics
     * 3. Stream frame to connected viewer
     * 4. If verbose, print frame info
     *
     * EXAMPLE:
     *   ctx->frames_received++;
     *
     *   if (validate_frame(ctx, buffer, actual_len)) {
     *       ctx->frames_valid++;
     *   }
     *
     *   // Stream to viewer
     *   stream_frame(ctx, buffer, actual_len);
     *
     *   if (ctx->verbose) {
     *       const struct phantomfpga_frame_header *hdr = buffer;
     *       printf("Frame %u: seq=%u\n",
     *              ctx->frames_received, hdr->sequence);
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)buffer;
    (void)actual_len;
    fprintf(stderr, "TODO: Implement process_frame()\n");

    /* Stream frame to viewer even without full implementation */
    stream_frame(ctx, buffer, actual_len);

    return 0;
    /* --- END YOUR CODE --- */
}

/* Unused in skeleton - trainees will call this from process_frame */
__attribute__((unused))
static bool validate_frame(struct app_context *ctx, const void *frame, uint32_t frame_size)
{
    /*
     * TODO: Validate frame integrity
     *
     * CHECKS:
     * 1. Check magic number is PHANTOMFPGA_FRAME_MAGIC (0xF00DFACE)
     * 2. Check sequence continuity
     * 3. Validate CRC32 if enabled
     *
     * MAGIC CHECK:
     *   const struct phantomfpga_frame_header *hdr = frame;
     *   uint32_t magic = le32toh(hdr->magic);
     *   if (magic != PHANTOMFPGA_FRAME_MAGIC) {
     *       ctx->magic_errors++;
     *       return false;
     *   }
     *
     * SEQUENCE CHECK:
     *   uint32_t seq = le32toh(hdr->sequence);
     *   if (!ctx->seq_initialized) {
     *       ctx->last_seq = seq;
     *       ctx->seq_initialized = true;
     *   } else {
     *       uint32_t expected = (ctx->last_seq + 1) % PHANTOMFPGA_FRAME_COUNT;
     *       if (seq != expected) {
     *           ctx->seq_errors++;
     *       }
     *       ctx->last_seq = seq;
     *   }
     *
     * CRC CHECK:
     *   if (ctx->validate_crc) {
     *       uint32_t computed = compute_crc32(frame, FRAME_CRC_OFFSET);
     *       const uint8_t *p = (const uint8_t *)frame + FRAME_CRC_OFFSET;
     *       uint32_t stored = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
     *       if (computed != stored) {
     *           ctx->crc_errors++;
     *           return false;
     *       }
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)frame;
    (void)frame_size;
    fprintf(stderr, "TODO: Implement validate_frame()\n");
    return true;
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Statistics                                                               */
/* ------------------------------------------------------------------------ */

static void print_statistics(struct app_context *ctx)
{
    /*
     * TODO: Print device and application statistics
     *
     * STEPS:
     * 1. Get device statistics via PHANTOMFPGA_IOCTL_GET_STATS
     * 2. Calculate runtime duration
     * 3. Calculate frame rate
     * 4. Print stats
     *
     * GET DEVICE STATS:
     *   struct phantomfpga_stats stats;
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_GET_STATS, &stats) < 0) {
     *       perror("Failed to get stats");
     *       return;
     *   }
     *
     * CALCULATE DURATION:
     *   struct timespec now;
     *   clock_gettime(CLOCK_MONOTONIC, &now);
     *   double duration = (now.tv_sec - ctx->start_time.tv_sec) +
     *                     (now.tv_nsec - ctx->start_time.tv_nsec) / 1e9;
     *
     * EXAMPLE OUTPUT:
     *   printf("=== Statistics ===\n");
     *   printf("Runtime:           %.2f seconds\n", duration);
     *   printf("Device transmitted: %lu frames\n", stats.frames_produced);
     *   printf("Device dropped:    %lu frames\n", stats.frames_dropped);
     *   printf("Current frame:     %u / %u\n", stats.current_frame, PHANTOMFPGA_FRAME_COUNT);
     *   printf("App received:      %lu frames\n", ctx->frames_received);
     *   printf("App valid:         %lu frames\n", ctx->frames_valid);
     *   printf("Sequence errors:   %lu\n", ctx->seq_errors);
     *   printf("Magic errors:      %lu\n", ctx->magic_errors);
     *   printf("CRC errors:        %lu\n", ctx->crc_errors);
     */

    /* --- YOUR CODE HERE --- */
    printf("=== Statistics (TODO: implement me) ===\n");
    printf("Frames received:     %lu\n", (unsigned long)ctx->frames_received);
    printf("Frames valid:        %lu\n", (unsigned long)ctx->frames_valid);
    printf("Sequence errors:     %lu\n", (unsigned long)ctx->seq_errors);
    printf("Magic errors:        %lu\n", (unsigned long)ctx->magic_errors);
    printf("CRC errors:          %lu\n", (unsigned long)ctx->crc_errors);

    /* Network statistics */
    if (ctx->tcp_server_sock >= 0) {
        printf("--- Network ---\n");
        printf("Frames sent:         %lu\n", (unsigned long)ctx->net_frames_sent);
        printf("Bytes sent:          %lu\n", (unsigned long)ctx->net_bytes_sent);
        printf("Send errors:         %lu\n", (unsigned long)ctx->net_send_errors);
        printf("Viewer:              %s\n",
               ctx->tcp_client_fd >= 0 ? "connected" : "waiting");
    }
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Cleanup and Signal Handling                                              */
/* ------------------------------------------------------------------------ */

static void cleanup(struct app_context *ctx)
{
    /* Clean up network connections */
    cleanup_network(ctx);

    /* Unmap the buffer pool */
    if (ctx->buffer_pool && ctx->buffer_pool != MAP_FAILED) {
        munmap(ctx->buffer_pool, ctx->buffer_pool_size);
        ctx->buffer_pool = NULL;
    }

    /* Close device */
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}

static void signal_handler(int sig)
{
    (void)sig;
    if (g_ctx) {
        g_ctx->running = false;
    }
}

/* ------------------------------------------------------------------------ */
/* End of phantomfpga_app.c                                                 */
/* ------------------------------------------------------------------------ */
