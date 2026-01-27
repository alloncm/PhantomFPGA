/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * PhantomFPGA IOCTL Interface (Userspace API)
 *
 * This header defines the interface between userspace applications
 * and the PhantomFPGA kernel driver. Include this in your application
 * to communicate with the /dev/phantomfpga device.
 *
 * Usage:
 *   #include "phantomfpga_uapi.h"
 *   int fd = open("/dev/phantomfpga", O_RDWR);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_SET_CFG, &config);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_START);
 */

#ifndef PHANTOMFPGA_UAPI_H
#define PHANTOMFPGA_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* Device node path */
#define PHANTOMFPGA_DEV_NAME    "phantomfpga"
#define PHANTOMFPGA_DEV_PATH    "/dev/phantomfpga"

/* Magic number for ioctl commands - using 'P' for Phantom */
#define PHANTOMFPGA_IOC_MAGIC   'P'

/* ------------------------------------------------------------------------ */
/* Configuration Structure                                                  */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_config - Device configuration parameters
 *
 * @frame_size:   Size of each frame in bytes (64 - 65536)
 *                Includes header (24 bytes) + payload
 * @frame_rate:   Target frame production rate in Hz (1 - 100000)
 * @ring_size:    Number of ring buffer entries, must be power of 2 (4 - 4096)
 * @watermark:    IRQ threshold - interrupt when this many frames pending
 * @reserved:     Reserved for future use, must be zero
 *
 * Use with PHANTOMFPGA_IOCTL_SET_CFG before starting streaming.
 * Invalid values will return -EINVAL.
 */
struct phantomfpga_config {
	__u32 frame_size;       /* Frame size in bytes (min 64, max 64K) */
	__u32 frame_rate;       /* Frame rate in Hz */
	__u32 ring_size;        /* Ring buffer entries (power of 2) */
	__u32 watermark;        /* IRQ watermark threshold */
	__u32 reserved[4];      /* Reserved, must be zero */
};

/* ------------------------------------------------------------------------ */
/* Statistics Structure                                                     */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_stats - Device statistics
 *
 * @frames_produced:  Total frames produced by device since last reset
 * @frames_consumed:  Total frames consumed by driver/userspace
 * @errors:           Total error count
 * @overruns:         Buffer overrun count (frames dropped due to full buffer)
 * @irq_count:        Total interrupt count
 * @prod_idx:         Current producer index (device write position)
 * @cons_idx:         Current consumer index (driver read position)
 * @status:           Device status register value
 * @reserved:         Reserved for future use
 *
 * Use with PHANTOMFPGA_IOCTL_GET_STATS to retrieve current statistics.
 */
struct phantomfpga_stats {
	__u64 frames_produced;  /* Total frames produced */
	__u64 frames_consumed;  /* Total frames consumed by userspace */
	__u32 errors;           /* Error counter */
	__u32 overruns;         /* Overrun counter */
	__u32 irq_count;        /* Total IRQ count (watermark + overrun) */
	__u32 prod_idx;         /* Current producer index */
	__u32 cons_idx;         /* Current consumer index */
	__u32 status;           /* Device status register */
	__u32 reserved[4];      /* Reserved for future use */
};

/* ------------------------------------------------------------------------ */
/* Buffer Info Structure                                                    */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_buffer_info - DMA buffer information for mmap
 *
 * @buffer_size:  Total DMA buffer size in bytes
 * @frame_size:   Size of each frame slot
 * @ring_size:    Number of entries in ring buffer
 * @mmap_offset:  Offset to use with mmap() - currently always 0
 *
 * Use with PHANTOMFPGA_IOCTL_GET_BUFFER_INFO before mmap().
 */
struct phantomfpga_buffer_info {
	__u64 buffer_size;      /* Total DMA buffer size */
	__u32 frame_size;       /* Frame slot size */
	__u32 ring_size;        /* Ring buffer entries */
	__u64 mmap_offset;      /* Offset for mmap (always 0) */
	__u32 reserved[4];      /* Reserved for future use */
};

/* ------------------------------------------------------------------------ */
/* IOCTL Commands                                                           */
/* ------------------------------------------------------------------------ */

/*
 * PHANTOMFPGA_IOCTL_SET_CFG - Configure device parameters
 *
 * Must be called before PHANTOMFPGA_IOCTL_START.
 * Cannot be called while device is streaming.
 *
 * Input:  struct phantomfpga_config
 * Returns: 0 on success, -EINVAL for invalid params, -EBUSY if streaming
 */
#define PHANTOMFPGA_IOCTL_SET_CFG       _IOW(PHANTOMFPGA_IOC_MAGIC, 1, \
                                             struct phantomfpga_config)

/*
 * PHANTOMFPGA_IOCTL_GET_CFG - Get current device configuration
 *
 * Output: struct phantomfpga_config
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_CFG       _IOR(PHANTOMFPGA_IOC_MAGIC, 2, \
                                             struct phantomfpga_config)

/*
 * PHANTOMFPGA_IOCTL_START - Start streaming
 *
 * Enables frame production. Device will start generating frames
 * at the configured rate and writing them to the DMA buffer.
 *
 * Returns: 0 on success, -EINVAL if not configured, -EBUSY if already started
 */
#define PHANTOMFPGA_IOCTL_START         _IO(PHANTOMFPGA_IOC_MAGIC, 3)

/*
 * PHANTOMFPGA_IOCTL_STOP - Stop streaming
 *
 * Disables frame production. Pending frames in the buffer remain valid.
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_STOP          _IO(PHANTOMFPGA_IOC_MAGIC, 4)

/*
 * PHANTOMFPGA_IOCTL_GET_STATS - Get device statistics
 *
 * Output: struct phantomfpga_stats
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_STATS     _IOR(PHANTOMFPGA_IOC_MAGIC, 5, \
                                             struct phantomfpga_stats)

/*
 * PHANTOMFPGA_IOCTL_RESET_STATS - Reset statistics counters
 *
 * Resets frames_consumed, irq_count to zero.
 * Device counters (frames_produced, errors, overruns) are reset via soft reset.
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_RESET_STATS   _IO(PHANTOMFPGA_IOC_MAGIC, 6)

/*
 * PHANTOMFPGA_IOCTL_GET_BUFFER_INFO - Get DMA buffer information
 *
 * Call this to get buffer parameters before mmap().
 *
 * Output: struct phantomfpga_buffer_info
 * Returns: 0 on success, -EINVAL if not configured
 */
#define PHANTOMFPGA_IOCTL_GET_BUFFER_INFO _IOR(PHANTOMFPGA_IOC_MAGIC, 7, \
                                               struct phantomfpga_buffer_info)

/*
 * PHANTOMFPGA_IOCTL_CONSUME_FRAME - Mark one frame as consumed
 *
 * Advances the consumer index by one. Use this after processing a frame
 * when using mmap() access pattern.
 *
 * Returns: 0 on success, -EAGAIN if no frames available
 */
#define PHANTOMFPGA_IOCTL_CONSUME_FRAME _IO(PHANTOMFPGA_IOC_MAGIC, 8)

/* Maximum ioctl command number for validation */
#define PHANTOMFPGA_IOC_MAXNR           8

/* ------------------------------------------------------------------------ */
/* Return Codes                                                             */
/* ------------------------------------------------------------------------ */

/*
 * Standard errno values are used:
 *
 * EINVAL   - Invalid parameter value
 * EBUSY    - Device busy (already streaming, or config during stream)
 * EAGAIN   - No data available (poll or non-blocking read)
 * ENOMEM   - Memory allocation failed
 * EIO      - Hardware communication error
 * ENODEV   - Device not found
 */

#endif /* PHANTOMFPGA_UAPI_H */
