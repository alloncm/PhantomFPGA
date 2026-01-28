// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA Userspace Application v2.0 - Scatter-Gather DMA Edition
 *
 * This application demonstrates how to interact with the PhantomFPGA
 * kernel driver. It configures the device, mmaps the descriptor buffers,
 * and polls for incoming packets with full validation.
 *
 * v2.0 brings multiple header profiles, CRC validation, variable packet
 * sizes, and fault injection testing. It's basically what you'd write
 * for a real FPGA data acquisition system... minus the real hardware.
 *
 * YOUR MISSION (should you choose to accept it):
 * Complete all the TODO sections to build a working packet receiver.
 * The driver is your partner - make sure you understand the UAPI header
 * before diving in.
 *
 * Usage:
 *   ./phantomfpga_app --rate 1000 --size 256 --profile standard
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

/* Include the driver UAPI header for ioctl definitions */
#include "phantomfpga_uapi.h"

/* ------------------------------------------------------------------------ */
/* Constants and Defaults                                                   */
/* ------------------------------------------------------------------------ */

#define APP_NAME            "phantomfpga_app"
#define APP_VERSION         "2.0.0"

/* Device node - matches driver */
#define DEVICE_PATH         "/dev/phantomfpga0"

/* Default configuration values (in 64-bit words for sizes) */
#define DEFAULT_PKT_SIZE        256     /* 2KB in bytes (256 * 8) */
#define DEFAULT_PKT_SIZE_MAX    512     /* 4KB in bytes (512 * 8) */
#define DEFAULT_PKT_RATE        1000    /* Hz */
#define DEFAULT_DESC_COUNT      256     /* Must be power of 2 */
#define DEFAULT_HDR_PROFILE     PHANTOMFPGA_HDR_SIMPLE
#define DEFAULT_IRQ_COUNT       16
#define DEFAULT_IRQ_TIMEOUT     1000    /* microseconds */

/* Statistics update interval in seconds */
#define STATS_INTERVAL_SEC  1

/* CRC32 polynomial (IEEE 802.3) */
#define CRC32_POLY          0xEDB88320

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
    uint32_t pkt_size_mode;     /* 0=fixed, 1=variable */
    uint32_t pkt_size;          /* In 64-bit words */
    uint32_t pkt_size_max;      /* In 64-bit words */
    uint32_t header_profile;
    uint32_t pkt_rate;
    uint32_t buffer_size;       /* Per-buffer size in bytes */

    /* Runtime state */
    uint32_t last_seq;          /* Last seen sequence number */
    bool seq_initialized;       /* Have we seen the first packet? */
    uint64_t last_counter;      /* Last monotonic counter (full profile) */

    /* Statistics */
    uint64_t packets_received;  /* Total packets we processed */
    uint64_t packets_valid;     /* Packets that passed validation */
    uint64_t seq_errors;        /* Sequence discontinuities */
    uint64_t magic_errors;      /* Invalid magic number */
    uint64_t hdr_crc_errors;    /* Header CRC failures */
    uint64_t pay_crc_errors;    /* Payload CRC failures */
    uint64_t corrupted_flags;   /* Packets with CORRUPTED flag */
    struct timespec start_time; /* When we started receiving */

    /* Flags */
    bool verbose;
    bool stats_only;
    bool validate_crc;          /* Enable CRC validation */
    bool running;
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
static int process_packet(struct app_context *ctx, const void *buffer, uint32_t actual_len);
static bool validate_packet(struct app_context *ctx, const void *packet, uint32_t pkt_size);
static void print_statistics(struct app_context *ctx);
static void cleanup(struct app_context *ctx);
static void signal_handler(int sig);

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
    ctx.pkt_size_mode = 0;  /* Fixed size */
    ctx.pkt_size = DEFAULT_PKT_SIZE;
    ctx.pkt_size_max = DEFAULT_PKT_SIZE_MAX;
    ctx.header_profile = DEFAULT_HDR_PROFILE;
    ctx.pkt_rate = DEFAULT_PKT_RATE;
    ctx.validate_crc = true;
    ctx.running = true;

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

    /* Set up mmap for zero-copy packet access */
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

    printf("PhantomFPGA v2.0 streaming started.\n");
    printf("  Descriptor count: %u\n", ctx.desc_count);
    printf("  Packet size:      %u bytes (%u 64-bit words)\n",
           ctx.pkt_size * 8, ctx.pkt_size);
    if (ctx.pkt_size_mode) {
        printf("  Packet size max:  %u bytes (%u 64-bit words)\n",
               ctx.pkt_size_max * 8, ctx.pkt_size_max);
    }
    printf("  Header profile:   %s\n",
           ctx.header_profile == PHANTOMFPGA_HDR_SIMPLE ? "simple (16B)" :
           ctx.header_profile == PHANTOMFPGA_HDR_STANDARD ? "standard (32B)" :
           "full (64B)");
    printf("  Packet rate:      %u Hz\n", ctx.pkt_rate);
    printf("  CRC validation:   %s\n", ctx.validate_crc ? "enabled" : "disabled");
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
    printf("PhantomFPGA v2.0 userspace application - receives and validates packets\n");
    printf("from the PhantomFPGA SG-DMA device.\n");
    printf("\n");
    printf("Options:\n");
    printf("  -r, --rate RATE       Packet rate in Hz (default: %d)\n", DEFAULT_PKT_RATE);
    printf("  -s, --size SIZE       Packet size in 64-bit words (default: %d = %d bytes)\n",
           DEFAULT_PKT_SIZE, DEFAULT_PKT_SIZE * 8);
    printf("  -m, --size-max SIZE   Max packet size for variable mode (default: %d)\n",
           DEFAULT_PKT_SIZE_MAX);
    printf("  -v, --variable        Enable variable packet size mode\n");
    printf("  -n, --desc-count N    Descriptor count, power of 2 (default: %d)\n",
           DEFAULT_DESC_COUNT);
    printf("  -p, --profile PROF    Header profile: simple, standard, full (default: simple)\n");
    printf("  -c, --no-crc          Disable CRC validation\n");
    printf("  -S, --stats           Print device statistics and exit\n");
    printf("  -V, --verbose         Enable verbose output\n");
    printf("  -h, --help            Show this help message\n");
    printf("  --version             Show version information\n");
    printf("\n");
    printf("Header Profiles:\n");
    printf("  simple   - 16 bytes: magic, sequence, size\n");
    printf("  standard - 32 bytes: + timestamp, counter, header CRC\n");
    printf("  full     - 64 bytes: + version, flags, mono_counter, payload CRC, channel\n");
    printf("\n");
    printf("Examples:\n");
    printf("  %s --rate 1000 --size 256 --profile standard\n", prog_name);
    printf("  %s --rate 10000 --size 64 --variable --size-max 512 --profile full\n", prog_name);
    printf("  %s --stats\n", prog_name);
    printf("\n");
}

static int parse_arguments(int argc, char *argv[], struct app_context *ctx)
{
    static struct option long_options[] = {
        {"rate",       required_argument, 0, 'r'},
        {"size",       required_argument, 0, 's'},
        {"size-max",   required_argument, 0, 'm'},
        {"variable",   no_argument,       0, 'v'},
        {"desc-count", required_argument, 0, 'n'},
        {"profile",    required_argument, 0, 'p'},
        {"no-crc",     no_argument,       0, 'c'},
        {"stats",      no_argument,       0, 'S'},
        {"verbose",    no_argument,       0, 'V'},
        {"help",       no_argument,       0, 'h'},
        {"version",    no_argument,       0, 0x100},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;

    while ((opt = getopt_long(argc, argv, "r:s:m:vn:p:cSVh",
                              long_options, &option_index)) != -1) {
        switch (opt) {
        case 'r':
            ctx->pkt_rate = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 's':
            ctx->pkt_size = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'm':
            ctx->pkt_size_max = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'v':
            ctx->pkt_size_mode = 1;
            break;
        case 'n':
            ctx->desc_count = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'p':
            if (strcmp(optarg, "simple") == 0)
                ctx->header_profile = PHANTOMFPGA_HDR_SIMPLE;
            else if (strcmp(optarg, "standard") == 0)
                ctx->header_profile = PHANTOMFPGA_HDR_STANDARD;
            else if (strcmp(optarg, "full") == 0)
                ctx->header_profile = PHANTOMFPGA_HDR_FULL;
            else {
                fprintf(stderr, "Unknown profile: %s (use simple, standard, or full)\n", optarg);
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
        default:
            print_usage(argv[0]);
            return -1;
        }
    }

    /*
     * TODO: Add validation for configuration parameters
     *
     * HINTS:
     * - pkt_size should be >= 8 (64 bytes min) and <= 8192 (64KB)
     * - desc_count must be a power of 2: (n & (n-1)) == 0
     * - pkt_rate should be >= 1 and <= 100000
     * - If variable mode, pkt_size_max >= pkt_size
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
     * 1. Create a struct phantomfpga_sg_config and fill it
     * 2. Zero out the reserved fields
     * 3. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_SET_CFG, &config)
     * 4. Check for errors
     *
     * STRUCTURE (from phantomfpga_uapi.h):
     *   struct phantomfpga_sg_config {
     *       __u32 desc_count;
     *       __u32 pkt_size_mode;
     *       __u32 pkt_size;
     *       __u32 pkt_size_max;
     *       __u32 header_profile;
     *       __u32 pkt_rate;
     *       __u16 irq_coalesce_count;
     *       __u16 irq_coalesce_timeout;
     *       __u32 reserved[4];
     *   };
     *
     * EXAMPLE:
     *   struct phantomfpga_sg_config config = {0};
     *   config.desc_count = ctx->desc_count;
     *   config.pkt_size_mode = ctx->pkt_size_mode;
     *   config.pkt_size = ctx->pkt_size;
     *   config.pkt_size_max = ctx->pkt_size_max;
     *   config.header_profile = ctx->header_profile;
     *   config.pkt_rate = ctx->pkt_rate;
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
     *   Each buffer contains: [packet data][completion struct at end]
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
     * TODO: Start packet streaming
     *
     * STEPS:
     * 1. Call ioctl(ctx->fd, PHANTOMFPGA_IOCTL_START)
     * 2. Check for errors
     *
     * After this call, the device will start producing packets.
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
     * TODO: Stop packet streaming
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
     * 2. When data is available, read packets from completed buffers
     * 3. Process and validate each packet
     * 4. Mark packets as consumed via PHANTOMFPGA_IOCTL_CONSUME_PKT
     * 5. Periodically print statistics
     * 6. Exit when ctx->running becomes false
     *
     * POLL SETUP:
     *   struct pollfd pfd = {
     *       .fd = ctx->fd,
     *       .events = POLLIN
     *   };
     *
     * PROCESSING LOOP OUTLINE:
     *   while (ctx->running) {
     *       int ret = poll(&pfd, 1, 1000);  // 1 second timeout
     *       if (ret < 0) {
     *           if (errno == EINTR) continue;
     *           break;
     *       }
     *       if (ret == 0) {
     *           print_statistics(ctx);
     *           continue;
     *       }
     *       if (pfd.revents & POLLIN) {
     *           // Process completed packets
     *           // Use read() or mmap to access packet data
     *           // Then ioctl(fd, PHANTOMFPGA_IOCTL_CONSUME_PKT) to advance
     *       }
     *   }
     *
     * READING PACKETS WITH MMAP:
     * - Get buffer address: buffer_pool + (desc_idx * buffer_size)
     * - Read completion struct at end: buffer + buffer_size - 16
     * - Completion tells you actual_length and status
     * - Packet data is at start of buffer
     *
     * BONUS: Use read() instead of mmap for simpler code
     * The driver can copy packet data to userspace buffer.
     */

    /* --- YOUR CODE HERE --- */
    struct pollfd pfd;
    time_t last_stats_time = time(NULL);

    (void)pfd;
    (void)last_stats_time;

    fprintf(stderr, "TODO: Implement main_loop()\n");

    /* Placeholder loop */
    while (ctx->running) {
        sleep(1);
        if (ctx->verbose) {
            print_statistics(ctx);
        }
    }
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Packet Processing                                                        */
/* ------------------------------------------------------------------------ */

static int process_packet(struct app_context *ctx, const void *buffer, uint32_t actual_len)
{
    /*
     * TODO: Process a single packet from a buffer
     *
     * STEPS:
     * 1. Validate the packet using validate_packet()
     * 2. Update statistics
     * 3. If verbose, print packet info
     *
     * EXAMPLE:
     *   ctx->packets_received++;
     *
     *   if (validate_packet(ctx, buffer, actual_len)) {
     *       ctx->packets_valid++;
     *   }
     *
     *   if (ctx->verbose) {
     *       // Print based on header profile
     *       const struct phantomfpga_hdr_simple *hdr = buffer;
     *       printf("Packet %u: size=%u\n", hdr->sequence, hdr->size);
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)buffer;
    (void)actual_len;
    fprintf(stderr, "TODO: Implement process_packet()\n");
    return 0;
    /* --- END YOUR CODE --- */
}

static bool validate_packet(struct app_context *ctx, const void *packet, uint32_t pkt_size)
{
    /*
     * TODO: Validate packet integrity based on header profile
     *
     * COMMON CHECKS (all profiles):
     * 1. Check magic number is PHANTOMFPGA_PACKET_MAGIC (0xABCD1234)
     * 2. Check sequence continuity
     *
     * STANDARD PROFILE CHECKS:
     * 3. Validate header CRC32 (first 24 bytes)
     *
     * FULL PROFILE CHECKS:
     * 4. Validate header CRC32 (first 40 bytes)
     * 5. Validate payload CRC32
     * 6. Check for CORRUPTED flag (fault injection)
     *
     * MAGIC CHECK:
     *   const struct phantomfpga_hdr_simple *hdr = packet;
     *   if (hdr->magic != PHANTOMFPGA_PACKET_MAGIC) {
     *       ctx->magic_errors++;
     *       return false;
     *   }
     *
     * SEQUENCE CHECK:
     *   if (!ctx->seq_initialized) {
     *       ctx->last_seq = hdr->sequence;
     *       ctx->seq_initialized = true;
     *   } else {
     *       uint32_t expected = ctx->last_seq + 1;
     *       if (hdr->sequence != expected) {
     *           ctx->seq_errors++;
     *       }
     *       ctx->last_seq = hdr->sequence;
     *   }
     *
     * HEADER CRC CHECK (standard/full profiles):
     *   if (ctx->header_profile >= PHANTOMFPGA_HDR_STANDARD) {
     *       const struct phantomfpga_hdr_standard *shdr = packet;
     *       uint32_t expected_crc = compute_crc32(packet, 24);  // First 24 bytes
     *       if (shdr->hdr_crc32 != expected_crc) {
     *           ctx->hdr_crc_errors++;
     *           return false;
     *       }
     *   }
     *
     * PAYLOAD CRC CHECK (full profile only):
     *   if (ctx->header_profile == PHANTOMFPGA_HDR_FULL) {
     *       const struct phantomfpga_hdr_full *fhdr = packet;
     *       const uint8_t *payload = (const uint8_t *)packet + 64;
     *       uint32_t expected_crc = compute_crc32(payload, fhdr->payload_size);
     *       if (fhdr->payload_crc32 != expected_crc) {
     *           ctx->pay_crc_errors++;
     *           return false;
     *       }
     *       if (fhdr->flags & PHANTOMFPGA_PKT_FLAG_CORRUPTED) {
     *           ctx->corrupted_flags++;
     *       }
     *   }
     */

    /* --- YOUR CODE HERE --- */
    (void)ctx;
    (void)packet;
    (void)pkt_size;
    fprintf(stderr, "TODO: Implement validate_packet()\n");
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
     * 3. Calculate packet rate
     * 4. Print stats
     *
     * GET DEVICE STATS:
     *   struct phantomfpga_sg_stats stats;
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
     *   printf("Device produced:   %lu packets\n", stats.packets_produced);
     *   printf("Device completed:  %u descriptors\n", stats.desc_completed);
     *   printf("Device errors:     %u\n", stats.errors);
     *   printf("App received:      %lu packets\n", ctx->packets_received);
     *   printf("App valid:         %lu packets\n", ctx->packets_valid);
     *   printf("Sequence errors:   %lu\n", ctx->seq_errors);
     *   printf("Magic errors:      %lu\n", ctx->magic_errors);
     *   printf("Header CRC errors: %lu\n", ctx->hdr_crc_errors);
     *   printf("Payload CRC errors:%lu\n", ctx->pay_crc_errors);
     *   printf("Corrupted flags:   %lu\n", ctx->corrupted_flags);
     */

    /* --- YOUR CODE HERE --- */
    printf("=== Statistics (TODO: implement me) ===\n");
    printf("Packets received:    %lu\n", (unsigned long)ctx->packets_received);
    printf("Packets valid:       %lu\n", (unsigned long)ctx->packets_valid);
    printf("Sequence errors:     %lu\n", (unsigned long)ctx->seq_errors);
    printf("Magic errors:        %lu\n", (unsigned long)ctx->magic_errors);
    printf("Header CRC errors:   %lu\n", (unsigned long)ctx->hdr_crc_errors);
    printf("Payload CRC errors:  %lu\n", (unsigned long)ctx->pay_crc_errors);
    /* --- END YOUR CODE --- */
}

/* ------------------------------------------------------------------------ */
/* Cleanup and Signal Handling                                              */
/* ------------------------------------------------------------------------ */

static void cleanup(struct app_context *ctx)
{
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
