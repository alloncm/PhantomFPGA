/*
 * PhantomFPGA Viewer - ASCII Animation Terminal Client
 *
 * Connects to the phantomfpga_app TCP server and displays
 * streamed ASCII animation frames in the terminal.
 *
 * Build a driver, stream frames, watch a cartoon. That's the goal.
 *
 * Usage: phantomfpga_view [host] [port]
 *        Default: localhost 5000
 *
 * TRAINEE EXERCISE: Complete the TODOs to make the animation play.
 *
 * What works already:
 *   - TCP client connection
 *   - Command-line parsing
 *   - Signal handling (Ctrl+C exits cleanly)
 *   - CRC32 table and function prototype
 *   - Frame constants
 *
 * Your job (the TODOs):
 *   1. Read a length-prefixed frame from the socket
 *   2. Validate the magic number (0xF00DFACE)
 *   3. Compute and validate CRC32
 *   4. Track sequence numbers, detect dropped frames
 *   5. Display the frame (clear screen, print ASCII art)
 *   6. Control frame rate with sleep
 *   7. Print statistics on exit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>

/* ------------------------------------------------------------------------ */
/* Frame Constants - Must match the device/driver                           */
/* ------------------------------------------------------------------------ */

#define FRAME_MAGIC         0xF00DFACE
#define FRAME_SIZE          5120        /* Total frame size in bytes */
#define FRAME_DATA_SIZE     4995        /* ASCII art portion */
#define FRAME_ROWS          45          /* Terminal rows needed */
#define FRAME_COLS          110         /* Terminal columns needed */
#define FRAME_COUNT         250         /* Total frames in animation */
#define FRAME_CRC_OFFSET    5116        /* CRC32 at end of frame */

#define DEFAULT_HOST        "localhost"
#define DEFAULT_PORT        5000
#define DEFAULT_FRAME_RATE  25          /* Expected fps */

/* ------------------------------------------------------------------------ */
/* Frame Header Structure                                                   */
/* ------------------------------------------------------------------------ */

struct frame_header {
    uint32_t magic;         /* 0xF00DFACE */
    uint32_t sequence;      /* Frame sequence number (0-249, wraps) */
    uint64_t reserved;      /* Reserved (must be 0) */
} __attribute__((packed));

#define FRAME_HEADER_SIZE   sizeof(struct frame_header)
#define FRAME_DATA_OFFSET   FRAME_HEADER_SIZE

/* ------------------------------------------------------------------------ */
/* CRC32 Implementation (IEEE 802.3 polynomial)                             */
/* ------------------------------------------------------------------------ */

/*
 * Pre-computed CRC32 lookup table.
 * Polynomial: 0xEDB88320 (IEEE 802.3, same as Ethernet)
 */
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba,
    0x076dc419, 0x706af48f, 0xe963a535, 0x9e6495a3,
    0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d09, 0x90bf1d91,
    0x1db71064, 0x6ab020f2, 0xf3b97148, 0x84be41de,
    0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec,
    0x14015c4f, 0x63066cd9, 0xfa0f3d63, 0x8d080df5,
    0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b,
    0x35b5a8fa, 0x42b2986c, 0xdede5fd6, 0xa9d9cf40,
    0x3f44d2e3, 0x4843e275, 0xd14aae9f, 0xa64dbe09,
    0x2d02ef8c, 0x5a05df1a, 0xc30c8e2e, 0xb40bbe98,
    0x2a6f2b3b, 0x5d681bad, 0xc4614ab7, 0xb3667a0f,
    0x34d62d3e, 0x43d15ca8, 0xdad60d12, 0xadd66d84,
    0x34d2d02f, 0x43d503b9, 0xdadc5003, 0xaddb6095,
    0x76dc4190, 0x01db7106, 0x98d220bc, 0xefd5102a,
    0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818,
    0x7f6a0dbb, 0x086d3d2d, 0x91646c97, 0xe6635c01,
    0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457,
    0x65b0d9c6, 0x12b7e950, 0x8bbeb8ea, 0xfcb9887c,
    0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2,
    0x4adfa541, 0x3dd895d7, 0xa4d1c46d, 0xd3d6f4fb,
    0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7a4b,
    0x5005713c, 0x270241aa, 0xbe0b1010, 0xc90c2086,
    0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4,
    0x59b33d17, 0x2eb40d81, 0xb7bd5c3b, 0xc0ba6cad,
    0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683,
    0xe3630b12, 0x94643b84, 0x0d6d6a3e, 0x7a6a5aa8,
    0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe,
    0xf762575d, 0x806567cb, 0x196c3671, 0x6e6b06e7,
    0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5,
    0xd6d6a3e8, 0xa1d1937e, 0x38d8c2c4, 0x4fdff252,
    0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60,
    0xdf60efc3, 0xa867df55, 0x316e8eef, 0x4669be79,
    0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f,
    0xc5ba3bbe, 0xb2bd0b28, 0x2bb45a92, 0x5cb36a04,
    0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a,
    0x9c0906a9, 0xeb0e363f, 0x72076785, 0x05005713,
    0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21,
    0x86d3d2d4, 0xf1d4e242, 0x68ddb3f8, 0x1fda836e,
    0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c,
    0x8f659eff, 0xf862ae69, 0x616bffd3, 0x166ccf45,
    0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db,
    0xaed16a4a, 0xd9d65adc, 0x40df0b66, 0x37d83bf0,
    0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6,
    0xbad03605, 0xcdb30693, 0x54ba5741, 0x23bd67d7,
    0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d,
};

/*
 * compute_crc32 - Calculate CRC32 of a buffer
 * @data: Pointer to data buffer
 * @len: Length of data in bytes
 *
 * Returns: CRC32 value (same algorithm as Ethernet/FPGA uses)
 *
 * This is provided for you - use it to validate frames.
 */
static uint32_t compute_crc32(const void *data, size_t len)
{
    const uint8_t *buf = data;
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------ */
/* Global State                                                             */
/* ------------------------------------------------------------------------ */

static volatile sig_atomic_t running = 1;
static int sock_fd = -1;

/* Statistics - you'll update these in your code */
static uint64_t frames_received = 0;
static uint64_t frames_dropped = 0;      /* Sequence gaps detected */
static uint64_t crc_errors = 0;
static uint64_t magic_errors = 0;
static int32_t last_sequence = -1;       /* -1 = no frame received yet */

/* ------------------------------------------------------------------------ */
/* Signal Handler                                                           */
/* ------------------------------------------------------------------------ */

static void signal_handler(int sig)
{
    (void)sig;
    running = 0;
}

/* ------------------------------------------------------------------------ */
/* Helper: Read exactly N bytes from socket                                 */
/* ------------------------------------------------------------------------ */

/*
 * read_exact - Read exactly 'len' bytes from socket
 * @fd: Socket file descriptor
 * @buf: Buffer to read into
 * @len: Number of bytes to read
 *
 * Returns: 0 on success, -1 on error or EOF
 *
 * This handles partial reads for you. Use it to read frames.
 */
static int read_exact(int fd, void *buf, size_t len)
{
    uint8_t *ptr = buf;
    size_t remaining = len;

    while (remaining > 0 && running) {
        ssize_t n = read(fd, ptr, remaining);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            /* EOF - server closed connection */
            return -1;
        }
        ptr += n;
        remaining -= n;
    }

    return running ? 0 : -1;
}

/* ------------------------------------------------------------------------ */
/* ANSI Terminal Helpers (provided)                                         */
/* ------------------------------------------------------------------------ */

/* Clear screen and move cursor to top-left */
static void clear_screen(void)
{
    printf("\033[2J\033[H");
}

/* Hide cursor for cleaner animation */
static void hide_cursor(void)
{
    printf("\033[?25l");
    fflush(stdout);
}

/* Show cursor again */
static void show_cursor(void)
{
    printf("\033[?25h");
    fflush(stdout);
}

/* Move cursor to top-left without clearing (faster redraw) */
static void cursor_home(void)
{
    printf("\033[H");
}

/* ------------------------------------------------------------------------ */
/* Frame Buffer                                                             */
/* ------------------------------------------------------------------------ */

static uint8_t frame_buffer[FRAME_SIZE];

/* ------------------------------------------------------------------------ */
/* TODO: Implement these functions                                          */
/* ------------------------------------------------------------------------ */

/*
 * TODO 1: receive_frame - Read one frame from the server
 *
 * The server sends frames with a 4-byte length prefix (network byte order).
 * You need to:
 *   1. Read 4 bytes for the length (use read_exact)
 *   2. Convert from network byte order (ntohl)
 *   3. Verify length == FRAME_SIZE
 *   4. Read FRAME_SIZE bytes into frame_buffer (use read_exact)
 *
 * Returns: 0 on success, -1 on error
 *
 * Hint: The length prefix is uint32_t in network byte order.
 */
static int receive_frame(void)
{
    /* TODO: Implement frame reception */
    (void)frame_buffer;  /* Remove when implemented */

    fprintf(stderr, "TODO: Implement receive_frame()\n");
    return -1;
}

/*
 * TODO 2: validate_frame - Check magic and CRC
 *
 * You need to:
 *   1. Check magic number at start of frame (should be FRAME_MAGIC)
 *   2. Compute CRC32 of first FRAME_CRC_OFFSET bytes
 *   3. Compare with stored CRC32 at offset FRAME_CRC_OFFSET
 *   4. Update magic_errors or crc_errors counters on failure
 *
 * Returns: true if frame is valid, false otherwise
 *
 * Hints:
 *   - Cast frame_buffer to struct frame_header* to read magic
 *   - CRC is stored as little-endian uint32_t
 *   - Use compute_crc32() to calculate CRC
 */
static bool validate_frame(void)
{
    /* TODO: Implement frame validation */

    fprintf(stderr, "TODO: Implement validate_frame()\n");
    return false;
}

/*
 * TODO 3: check_sequence - Detect dropped frames
 *
 * You need to:
 *   1. Extract sequence number from frame header
 *   2. If this isn't the first frame (last_sequence != -1):
 *      - Calculate expected sequence: (last_sequence + 1) % FRAME_COUNT
 *      - If actual != expected, frames were dropped
 *      - Count how many were dropped and add to frames_dropped
 *   3. Update last_sequence
 *
 * Hint: Sequence numbers wrap around at FRAME_COUNT (250).
 *       If last=248 and current=1, that's 2 frames dropped (249, 0).
 */
static void check_sequence(void)
{
    /* TODO: Implement sequence checking */

    fprintf(stderr, "TODO: Implement check_sequence()\n");
}

/*
 * TODO 4: display_frame - Show the ASCII art
 *
 * You need to:
 *   1. Move cursor to home position (use cursor_home())
 *   2. Write the ASCII data portion of the frame to stdout
 *      - Data starts at offset FRAME_DATA_OFFSET (16 bytes)
 *      - Data is FRAME_DATA_SIZE bytes (4995)
 *   3. Flush stdout
 *
 * Hint: The frame data already contains newlines for row separation.
 *       Just write it as-is with fwrite().
 */
static void display_frame(void)
{
    /* TODO: Implement frame display */

    fprintf(stderr, "TODO: Implement display_frame()\n");
}

/*
 * TODO 5: frame_delay - Sleep to maintain frame rate
 *
 * You need to:
 *   1. Calculate how long to sleep for target frame rate
 *   2. Use nanosleep() to sleep
 *
 * For 25 fps: sleep ~40ms between frames (1000000000 / 25 = 40000000 ns)
 *
 * Hint: struct timespec has tv_sec and tv_nsec fields.
 */
static void frame_delay(void)
{
    /* TODO: Implement frame rate limiting */

    /* For now, just a placeholder delay */
    usleep(40000);  /* 40ms = 25fps, replace with proper implementation */
}

/*
 * TODO 6: print_stats - Print statistics on exit
 *
 * Print a summary like:
 *   Frames received: 1234
 *   Frames dropped:  5 (sequence gaps)
 *   CRC errors:      2
 *   Magic errors:    0
 */
static void print_stats(void)
{
    /* TODO: Implement statistics printing */

    printf("\n--- Statistics ---\n");
    printf("TODO: Print your statistics here\n");
}

/* ------------------------------------------------------------------------ */
/* Main Loop (provided, calls your functions)                               */
/* ------------------------------------------------------------------------ */

static int run_viewer(void)
{
    clear_screen();
    hide_cursor();

    printf("Waiting for frames from server...\n");
    fflush(stdout);

    while (running) {
        /* Receive a frame */
        if (receive_frame() < 0) {
            if (running) {
                fprintf(stderr, "\nConnection lost\n");
            }
            break;
        }

        frames_received++;

        /* Validate the frame */
        if (!validate_frame()) {
            /* Bad frame - skip display but keep going */
            continue;
        }

        /* Check for dropped frames */
        check_sequence();

        /* Display the frame */
        display_frame();

        /* Wait for next frame */
        frame_delay();
    }

    show_cursor();
    print_stats();

    return 0;
}

/* ------------------------------------------------------------------------ */
/* Connection Setup (provided)                                              */
/* ------------------------------------------------------------------------ */

static int connect_to_server(const char *host, int port)
{
    struct addrinfo hints, *result, *rp;
    char port_str[16];
    int ret;

    snprintf(port_str, sizeof(port_str), "%d", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* TCP */

    ret = getaddrinfo(host, port_str, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        return -1;
    }

    /* Try each address until we connect */
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        sock_fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock_fd < 0)
            continue;

        if (connect(sock_fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;  /* Success */

        close(sock_fd);
        sock_fd = -1;
    }

    freeaddrinfo(result);

    if (sock_fd < 0) {
        fprintf(stderr, "Could not connect to %s:%d\n", host, port);
        return -1;
    }

    printf("Connected to %s:%d\n", host, port);
    return 0;
}

/* ------------------------------------------------------------------------ */
/* Main                                                                     */
/* ------------------------------------------------------------------------ */

static void usage(const char *prog)
{
    fprintf(stderr, "PhantomFPGA Viewer - ASCII Animation Client\n\n");
    fprintf(stderr, "Usage: %s [host] [port]\n", prog);
    fprintf(stderr, "  host  Server hostname (default: %s)\n", DEFAULT_HOST);
    fprintf(stderr, "  port  Server port (default: %d)\n", DEFAULT_PORT);
    fprintf(stderr, "\nConnect to phantomfpga_app server and display animation.\n");
    fprintf(stderr, "Press Ctrl+C to exit.\n");
}

int main(int argc, char *argv[])
{
    const char *host = DEFAULT_HOST;
    int port = DEFAULT_PORT;
    int ret;

    /* Parse arguments */
    if (argc > 1) {
        if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        host = argv[1];
    }
    if (argc > 2) {
        port = atoi(argv[2]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port: %s\n", argv[2]);
            return 1;
        }
    }

    /* Set up signal handlers */
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    /* Check terminal size */
    printf("PhantomFPGA Viewer\n");
    printf("Animation requires %dx%d terminal\n", FRAME_COLS, FRAME_ROWS);
    printf("Connecting to %s:%d...\n", host, port);

    /* Connect to server */
    ret = connect_to_server(host, port);
    if (ret < 0) {
        return 1;
    }

    /* Run the viewer */
    ret = run_viewer();

    /* Cleanup */
    if (sock_fd >= 0) {
        close(sock_fd);
    }

    return ret;
}
