// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA Userspace Application - Trainee Skeleton
 *
 * This application demonstrates how to interact with the PhantomFPGA
 * kernel driver. It opens the device, configures streaming parameters,
 * mmaps the ring buffer, and polls for incoming frames.
 *
 * YOUR MISSION (should you choose to accept it):
 * Complete all the TODO sections to build a working frame receiver.
 * The driver is your partner in crime - make sure you understand the
 * UAPI header before diving in.
 *
 * Usage:
 *   ./phantomfpga_app --rate 1000 --size 256 --watermark 16
 *   ./phantomfpga_app --help
 *   ./phantomfpga_app --stats
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

/* Include the driver UAPI header for ioctl definitions */
#include "phantomfpga_uapi.h"

/* ------------------------------------------------------------------------ */
/* Constants and Defaults                                                   */
/* ------------------------------------------------------------------------ */

#define APP_NAME            "phantomfpga_app"
#define APP_VERSION         "1.0.0"

/* Device node - matches driver */
#define DEVICE_PATH         "/dev/phantomfpga0"

/* Default configuration values */
#define DEFAULT_FRAME_SIZE  4096
#define DEFAULT_FRAME_RATE  1000    /* Hz */
#define DEFAULT_RING_SIZE   256     /* Must be power of 2 */
#define DEFAULT_WATERMARK   64

/* Frame validation - must match driver/device */
#define FRAME_MAGIC         0xABCD1234
#define FRAME_HEADER_SIZE   24      /* See phantomfpga_regs.h */

/* Telemetry defaults (stubs) */
#define DEFAULT_UDP_PORT    5000
#define DEFAULT_TCP_PORT    5001

/* Statistics update interval in seconds */
#define STATS_INTERVAL_SEC  1

/* ------------------------------------------------------------------------ */
/* Frame Header Structure (must match device format)                        */
/* ------------------------------------------------------------------------ */

/*
 * This structure MUST match struct phantomfpga_frame_header in phantomfpga_regs.h
 * We redefine it here for userspace since phantomfpga_regs.h uses kernel types.
 */
struct frame_header {
    uint32_t magic;         /* Should be FRAME_MAGIC (0xABCD1234) */
    uint32_t seq;           /* Sequence number, should increment */
    uint64_t ts_ns;         /* Timestamp in nanoseconds */
    uint32_t payload_len;   /* Payload length (frame_size - header) */
    uint32_t flags;         /* Frame flags */
} __attribute__((packed));

/* Frame flags */
#define FRAME_FLAG_CORRUPTED    (1 << 0)

/* ------------------------------------------------------------------------ */
/* Application Context                                                      */
/* ------------------------------------------------------------------------ */

struct app_context {
    /* Device state */
    int fd;                     /* Device file descriptor */
    void *ring_buffer;          /* mmap'd ring buffer */
    size_t ring_buffer_size;    /* Size of mmap'd region */

    /* Configuration */
    uint32_t frame_size;
    uint32_t frame_rate;
    uint32_t ring_size;
    uint32_t watermark;

    /* Runtime state */
    uint32_t last_seq;          /* Last seen sequence number */
    bool seq_initialized;       /* Have we seen the first frame? */

    /* Statistics */
    uint64_t frames_received;   /* Total frames we processed */
    uint64_t frames_valid;      /* Frames that passed validation */
    uint64_t seq_errors;        /* Sequence discontinuities */
    uint64_t magic_errors;      /* Invalid magic number */
    uint64_t crc_errors;        /* CRC/checksum failures */
    struct timespec start_time; /* When we started receiving */

    /* Telemetry (stubs) */
    int udp_sock;
    int tcp_sock;
    struct sockaddr_in udp_dest;

    /* Flags */
    bool verbose;
    bool stats_only;
    bool running;
};

/* Global context for signal handler */
static struct app_context *g_ctx = NULL;

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
static int process_frame(struct app_context *ctx, const void *frame);
static bool validate_frame(struct app_context *ctx, const struct frame_header *hdr,
                           const void *payload, size_t payload_len);
static void print_statistics(struct app_context *ctx);
static void cleanup(struct app_context *ctx);
static void signal_handler(int sig);

/* Telemetry stubs */
static int setup_udp_telemetry(struct app_context *ctx, const char *dest_ip, int port);
static int setup_tcp_control(struct app_context *ctx, int port);
static void send_udp_telemetry(struct app_context *ctx, const void *data, size_t len);
static void handle_tcp_control(struct app_context *ctx);

/* ------------------------------------------------------------------------ */
/* Main Entry Point                                                         */
/* ------------------------------------------------------------------------ */

int main(int argc, char *argv[])
{
    struct app_context ctx = {0};
    int ret;

    /* Initialize context with defaults */
    ctx.fd = -1;
    ctx.udp_sock = -1;
    ctx.tcp_sock = -1;
    ctx.frame_size = DEFAULT_FRAME_SIZE;
    ctx.frame_rate = DEFAULT_FRAME_RATE;
    ctx.ring_size = DEFAULT_RING_SIZE;
    ctx.watermark = DEFAULT_WATERMARK;
    ctx.running = true;

    /* Set up global context for signal handler */
    g_ctx = &ctx;

    /* Parse command line arguments */
    ret = parse_arguments(argc, argv, &ctx);
    if (ret != 0) {
        return (ret > 0) ? 0 : 1;  /* ret > 0 means --help was shown */
    }

    /* Install signal handlers for graceful shutdown */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Open the device */
    ret = open_device(&ctx);
    if (ret < 0) {
        fprintf(stderr, "Failed to open device: %s\n", strerror(-ret));
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
        fprintf(stderr, "Failed to mmap ring buffer: %s\n", strerror(-ret));
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

    printf("PhantomFPGA streaming started.\n");
    printf("  Frame size: %u bytes\n", ctx.frame_size);
    printf("  Frame rate: %u Hz\n", ctx.frame_rate);
    printf("  Ring size:  %u entries\n", ctx.ring_size);
    printf("  Watermark:  %u frames\n", ctx.watermark);
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
    printf("PhantomFPGA userspace application - receives and validates frames\n");
    printf("from the PhantomFPGA device.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -r, --rate RATE       Frame rate in Hz (default: %d)\n", DEFAULT_FRAME_RATE);
    printf("  -s, --size SIZE       Frame size in bytes (default: %d)\n", DEFAULT_FRAME_SIZE);
    printf("  -n, --ring-size N     Ring buffer entries, power of 2 (default: %d)\n", DEFAULT_RING_SIZE);
    printf("  -w, --watermark N     IRQ watermark threshold (default: %d)\n", DEFAULT_WATERMARK);
    printf("  -S, --stats           Print device statistics and exit\n");
    printf("  -v, --verbose         Enable verbose output\n");
    printf("  -h, --help            Show this help message\n");
    printf("  -V, --version         Show version information\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --rate 1000 --size 256 --watermark 16\n", prog_name);
    printf("  %s --stats\n", prog_name);
    printf("\n");
}

static int parse_arguments(int argc, char *argv[], struct app_context *ctx)
{
    static struct option long_options[] = {
        {"rate",      required_argument, 0, 'r'},
        {"size",      required_argument, 0, 's'},
        {"ring-size", required_argument, 0, 'n'},
        {"watermark", required_argument, 0, 'w'},
        {"stats",     no_argument,       0, 'S'},
        {"verbose",   no_argument,       0, 'v'},
        {"help",      no_argument,       0, 'h'},
        {"version",   no_argument,       0, 'V'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "r:s:n:w:SvhV", long_options, &option_index)) != -1) {
        switch (opt) {
        case 'r':
            ctx->frame_rate = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 's':
            ctx->frame_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'n':
            ctx->ring_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'w':
            ctx->watermark = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'S':
            ctx->stats_only = true;
            break;
        case 'v':
            ctx->verbose = true;
            break;
        case 'h':
            print_usage(argv[0]);
            return 1;  /* Positive means help shown, exit cleanly */
        case 'V':
            printf("%s version %s\n", APP_NAME, APP_VERSION);
            return 1;
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    /* Validate configuration */
    /* TODO: Add validation for frame_size, frame_rate, ring_size, watermark
     *
     * HINTS:
     * - frame_size should be >= 64 and <= 65536
     * - ring_size must be a power of 2 (use: (n & (n-1)) == 0)
     * - watermark should be < ring_size
     * - frame_rate should be >= 1 and <= 100000
     */

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
     * HINT: The device path is "/dev/phantomfpga0"
     *
     * EXAMPLE:
     *   ctx->fd = open(DEVICE_PATH, O_RDWR);
     *   if (ctx->fd < 0) {
     *       return -errno;
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;  /* Remove this line when implementing */
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
     * 1. Create a struct phantomfpga_config and fill it with ctx values
     * 2. Zero out the reserved fields (use memset or initializer)
     * 3. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_SET_CFG, &config)
     * 4. Check for errors (-1 return means failure, errno has reason)
     *
     * STRUCTURE (from phantomfpga_uapi.h):
     *   struct phantomfpga_config {
     *       __u32 frame_size;
     *       __u32 frame_rate;
     *       __u32 ring_size;
     *       __u32 watermark;
     *       __u32 reserved[4];
     *   };
     *
     * EXAMPLE:
     *   struct phantomfpga_config config = {0};
     *   config.frame_size = ctx->frame_size;
     *   config.frame_rate = ctx->frame_rate;
     *   config.ring_size = ctx->ring_size;
     *   config.watermark = ctx->watermark;
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
     * TODO: Map the DMA ring buffer into userspace
     *
     * STEPS:
     * 1. Get buffer info via PHANTOMFPGA_IOCTL_GET_BUFFER_INFO
     * 2. Call mmap() with the buffer size from the ioctl
     * 3. Store the mapped address in ctx->ring_buffer
     * 4. Store the size in ctx->ring_buffer_size
     *
     * GET BUFFER INFO:
     *   struct phantomfpga_buffer_info info;
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_GET_BUFFER_INFO, &info) < 0)
     *       return -errno;
     *
     * MMAP PARAMETERS:
     *   - addr:   NULL (let kernel choose)
     *   - length: info.buffer_size
     *   - prot:   PROT_READ (we only read frames)
     *   - flags:  MAP_SHARED (share with driver)
     *   - fd:     ctx->fd
     *   - offset: info.mmap_offset (usually 0)
     *
     * EXAMPLE:
     *   ctx->ring_buffer = mmap(NULL, info.buffer_size,
     *                           PROT_READ, MAP_SHARED,
     *                           ctx->fd, info.mmap_offset);
     *   if (ctx->ring_buffer == MAP_FAILED) {
     *       return -errno;
     *   }
     *   ctx->ring_buffer_size = info.buffer_size;
     *
     * NOTE: After successful mmap, you can access frames at:
     *   frame_ptr = ctx->ring_buffer + (frame_index * ctx->frame_size)
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
     * EXAMPLE:
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_START) < 0) {
     *       return -errno;
     *   }
     *
     * NOTE: After this call, the device will start producing frames
     * at the configured rate. Make sure mmap is set up first!
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
     * 2. Check for errors (but don't fail if already stopped)
     *
     * EXAMPLE:
     *   if (ioctl(ctx->fd, PHANTOMFPGA_IOCTL_STOP) < 0) {
     *       if (errno != EALREADY)  // Already stopped is OK
     *           return -errno;
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    fprintf(stderr, "TODO: Implement stop_streaming()\n");
    return 0;  /* Best effort - don't fail cleanup */
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
     * This is the heart of the application. The loop should:
     * 1. Use poll() to wait for new frames (POLLIN on ctx->fd)
     * 2. When frames are available, get stats to find producer index
     * 3. Process all available frames
     * 4. Mark each frame as consumed via PHANTOMFPGA_IOCTL_CONSUME_FRAME
     * 5. Periodically print statistics
     * 6. Exit when ctx->running becomes false (signal handler sets this)
     *
     * POLL SETUP:
     *   struct pollfd pfd = {
     *       .fd = ctx->fd,
     *       .events = POLLIN
     *   };
     *
     * POLL LOOP:
     *   while (ctx->running) {
     *       int ret = poll(&pfd, 1, 1000);  // 1 second timeout
     *       if (ret < 0) {
     *           if (errno == EINTR) continue;  // Signal received
     *           break;  // Real error
     *       }
     *       if (ret == 0) {
     *           // Timeout - print stats, continue
     *           print_statistics(ctx);
     *           continue;
     *       }
     *       if (pfd.revents & POLLIN) {
     *           // Frames available - process them
     *           // ...
     *       }
     *   }
     *
     * PROCESSING FRAMES:
     *   - Get current stats to find prod_idx and cons_idx
     *   - Calculate number of pending frames
     *   - For each pending frame:
     *     a) Calculate frame address: ring_buffer + (cons_idx * frame_size)
     *     b) Call process_frame(ctx, frame_ptr)
     *     c) Call ioctl(fd, PHANTOMFPGA_IOCTL_CONSUME_FRAME) to advance cons_idx
     *
     * CALCULATING PENDING FRAMES:
     *   uint32_t pending = (prod_idx - cons_idx) & (ring_size - 1);
     *
     * BONUS CHALLENGE:
     * - Use epoll instead of poll for better scalability
     * - Add timerfd for precise statistics intervals
     */

    /* --- YOUR CODE HERE --- */
    struct pollfd pfd;
    time_t last_stats_time = time(NULL);

    (void)pfd;
    (void)last_stats_time;

    fprintf(stderr, "TODO: Implement main_loop()\n");

    /* Placeholder loop - remove when implementing */
    while (ctx->running) {
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

static int process_frame(struct app_context *ctx, const void *frame)
{
    /*
     * TODO: Process a single frame
     *
     * STEPS:
     * 1. Cast frame to struct frame_header*
     * 2. Extract payload pointer (frame + FRAME_HEADER_SIZE)
     * 3. Get payload length from header
     * 4. Validate the frame using validate_frame()
     * 5. Update statistics (frames_received, frames_valid)
     * 6. If verbose mode, print frame info
     * 7. Optionally send telemetry
     *
     * FRAME LAYOUT:
     *   [frame_header (24 bytes)][payload (payload_len bytes)][padding...]
     *
     * EXAMPLE:
     *   const struct frame_header *hdr = (const struct frame_header *)frame;
     *   const void *payload = (const uint8_t *)frame + FRAME_HEADER_SIZE;
     *
     *   ctx->frames_received++;
     *
     *   if (validate_frame(ctx, hdr, payload, hdr->payload_len)) {
     *       ctx->frames_valid++;
     *       // Process valid frame data here
     *   }
     *
     *   if (ctx->verbose) {
     *       printf("Frame %u: ts=%lu len=%u flags=0x%x\n",
     *              hdr->seq, hdr->ts_ns, hdr->payload_len, hdr->flags);
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)frame;
    fprintf(stderr, "TODO: Implement process_frame()\n");
    return 0;
    /* --- END YOUR CODE --- */
}

static bool validate_frame(struct app_context *ctx, const struct frame_header *hdr,
                           const void *payload, size_t payload_len)
{
    /*
     * TODO: Validate frame integrity
     *
     * CHECKS TO PERFORM:
     * 1. Magic number: hdr->magic should be FRAME_MAGIC (0xABCD1234)
     * 2. Sequence continuity: hdr->seq should be last_seq + 1
     *    (handle wrap-around and first frame case)
     * 3. Payload length: should be reasonable (> 0, < frame_size - header)
     * 4. CRC/checksum: validate payload integrity (see BONUS below)
     *
     * MAGIC CHECK:
     *   if (hdr->magic != FRAME_MAGIC) {
     *       ctx->magic_errors++;
     *       return false;
     *   }
     *
     * SEQUENCE CHECK:
     *   if (!ctx->seq_initialized) {
     *       ctx->last_seq = hdr->seq;
     *       ctx->seq_initialized = true;
     *   } else {
     *       uint32_t expected = ctx->last_seq + 1;
     *       if (hdr->seq != expected) {
     *           ctx->seq_errors++;
     *           // Log gap: printf("Sequence gap: expected %u, got %u\n", ...);
     *       }
     *       ctx->last_seq = hdr->seq;
     *   }
     *
     * FLAGS CHECK:
     *   if (hdr->flags & FRAME_FLAG_CORRUPTED) {
     *       // Frame was intentionally corrupted by fault injection
     *       return false;
     *   }
     *
     * BONUS - CRC VALIDATION:
     * The device doesn't include a CRC, but you could implement a simple
     * checksum validation or add CRC support:
     *
     *   static uint32_t simple_checksum(const void *data, size_t len) {
     *       uint32_t sum = 0;
     *       const uint8_t *p = data;
     *       for (size_t i = 0; i < len; i++)
     *           sum += p[i];
     *       return sum;
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)hdr;
    (void)payload;
    (void)payload_len;
    fprintf(stderr, "TODO: Implement validate_frame()\n");
    return true;  /* Placeholder - always valid */
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
     * 3. Calculate frame rate (frames/second)
     * 4. Print both device and application stats
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
     * CALCULATE RATE:
     *   double rate = (duration > 0) ? ctx->frames_received / duration : 0;
     *
     * EXAMPLE OUTPUT:
     *   printf("=== Statistics ===\n");
     *   printf("Runtime:          %.2f seconds\n", duration);
     *   printf("Device produced:  %lu frames\n", stats.frames_produced);
     *   printf("Device consumed:  %lu frames\n", stats.frames_consumed);
     *   printf("Device overruns:  %u\n", stats.overruns);
     *   printf("Device errors:    %u\n", stats.errors);
     *   printf("App received:     %lu frames\n", ctx->frames_received);
     *   printf("App valid:        %lu frames\n", ctx->frames_valid);
     *   printf("Sequence errors:  %lu\n", ctx->seq_errors);
     *   printf("Magic errors:     %lu\n", ctx->magic_errors);
     *   printf("Effective rate:   %.2f frames/sec\n", rate);
     */

    /* --- YOUR CODE HERE --- */
    printf("=== Statistics (TODO: implement me) ===\n");
    printf("Frames received: %lu\n", (unsigned long)ctx->frames_received);
    printf("Frames valid:    %lu\n", (unsigned long)ctx->frames_valid);
    printf("Sequence errors: %lu\n", (unsigned long)ctx->seq_errors);
    printf("Magic errors:    %lu\n", (unsigned long)ctx->magic_errors);
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Cleanup and Signal Handling                                              */
/* ------------------------------------------------------------------------ */

static void cleanup(struct app_context *ctx)
{
    /*
     * TODO: Clean up all resources
     *
     * STEPS (in reverse order of allocation):
     * 1. Unmap the ring buffer if mapped
     * 2. Close UDP socket if open
     * 3. Close TCP socket if open
     * 4. Close device file descriptor if open
     *
     * MUNMAP:
     *   if (ctx->ring_buffer && ctx->ring_buffer != MAP_FAILED) {
     *       munmap(ctx->ring_buffer, ctx->ring_buffer_size);
     *       ctx->ring_buffer = NULL;
     *   }
     *
     * CLOSE SOCKETS:
     *   if (ctx->udp_sock >= 0) {
     *       close(ctx->udp_sock);
     *       ctx->udp_sock = -1;
     *   }
     *
     * CLOSE DEVICE:
     *   if (ctx->fd >= 0) {
     *       close(ctx->fd);
     *       ctx->fd = -1;
     *   }
     */

    /* --- YOUR CODE HERE --- */
    if (ctx->ring_buffer && ctx->ring_buffer != MAP_FAILED) {
        munmap(ctx->ring_buffer, ctx->ring_buffer_size);
        ctx->ring_buffer = NULL;
    }

    if (ctx->udp_sock >= 0) {
        close(ctx->udp_sock);
        ctx->udp_sock = -1;
    }

    if (ctx->tcp_sock >= 0) {
        close(ctx->tcp_sock);
        ctx->tcp_sock = -1;
    }

    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    /* --- END YOUR CODE --- */
}

static void signal_handler(int sig)
{
    /*
     * Signal handler for graceful shutdown.
     * Sets the running flag to false so main_loop exits cleanly.
     *
     * NOTE: This is already implemented - signals are tricky!
     * Only use async-signal-safe functions here.
     */
    (void)sig;
    if (g_ctx) {
        g_ctx->running = false;
    }
}

/* ------------------------------------------------------------------------ */
/* Telemetry Stubs                                                          */
/* ------------------------------------------------------------------------ */

/*
 * These are stub functions for optional telemetry features.
 * The trainee can implement these as bonus exercises.
 */

static int setup_udp_telemetry(struct app_context *ctx, const char *dest_ip, int port)
{
    /*
     * STUB: Set up UDP socket for telemetry output
     *
     * TODO (BONUS):
     * 1. Create UDP socket: socket(AF_INET, SOCK_DGRAM, 0)
     * 2. Set up destination address in ctx->udp_dest
     * 3. Store socket in ctx->udp_sock
     *
     * This would allow streaming frame metadata to a monitoring system.
     *
     * EXAMPLE:
     *   ctx->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
     *   if (ctx->udp_sock < 0) return -errno;
     *
     *   memset(&ctx->udp_dest, 0, sizeof(ctx->udp_dest));
     *   ctx->udp_dest.sin_family = AF_INET;
     *   ctx->udp_dest.sin_port = htons(port);
     *   inet_pton(AF_INET, dest_ip, &ctx->udp_dest.sin_addr);
     */
    (void)ctx;
    (void)dest_ip;
    (void)port;
    return -ENOSYS;  /* Not implemented */
}

static int setup_tcp_control(struct app_context *ctx, int port)
{
    /*
     * STUB: Set up TCP socket for control interface
     *
     * TODO (BONUS):
     * 1. Create TCP socket: socket(AF_INET, SOCK_STREAM, 0)
     * 2. Set SO_REUSEADDR option
     * 3. Bind to port
     * 4. Listen for connections
     * 5. Store socket in ctx->tcp_sock
     *
     * This would allow remote control (start/stop, config changes).
     *
     * Commands could include:
     *   - "START" - start streaming
     *   - "STOP"  - stop streaming
     *   - "STATS" - get statistics
     *   - "CONFIG rate=1000 size=256" - reconfigure
     */
    (void)ctx;
    (void)port;
    return -ENOSYS;  /* Not implemented */
}

static void send_udp_telemetry(struct app_context *ctx, const void *data, size_t len)
{
    /*
     * STUB: Send telemetry packet over UDP
     *
     * TODO (BONUS):
     *   if (ctx->udp_sock >= 0) {
     *       sendto(ctx->udp_sock, data, len, 0,
     *              (struct sockaddr *)&ctx->udp_dest,
     *              sizeof(ctx->udp_dest));
     *   }
     *
     * Telemetry format could be JSON or a binary protocol:
     *   {"seq": 12345, "ts_ns": 1234567890, "valid": true}
     */
    (void)ctx;
    (void)data;
    (void)len;
}

static void handle_tcp_control(struct app_context *ctx)
{
    /*
     * STUB: Handle incoming TCP control commands
     *
     * TODO (BONUS):
     * 1. Accept new connections if pending
     * 2. Read commands from connected clients
     * 3. Parse and execute commands
     * 4. Send responses
     *
     * This would be called from main_loop when tcp_sock has activity.
     */
    (void)ctx;
}

/* ------------------------------------------------------------------------ */
/* End of phantomfpga_app.c                                                 */
/* ------------------------------------------------------------------------ */
