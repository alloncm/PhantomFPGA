/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * PhantomFPGA IOCTL Interface v2.0 - Scatter-Gather DMA Edition
 *
 * This header defines the interface between userspace applications
 * and the PhantomFPGA kernel driver. Include this in your application
 * to communicate with the /dev/phantomfpga device.
 *
 * v2.0 brings scatter-gather DMA descriptors, multiple header profiles,
 * variable packet sizes, and CRC validation. Everything you need to
 * pretend you're working with real FPGA hardware.
 *
 * Usage:
 *   #include "phantomfpga_uapi.h"
 *   int fd = open("/dev/phantomfpga0", O_RDWR);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_SET_CFG, &config);
 *   ioctl(fd, PHANTOMFPGA_IOCTL_START);
 *   // Read packets or mmap the buffer
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
/* Header Profile Constants                                                 */
/* Pick your level of complexity                                            */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_HDR_SIMPLE   0  /* 16 bytes - just the essentials */
#define PHANTOMFPGA_HDR_STANDARD 1  /* 32 bytes - timestamp + CRC */
#define PHANTOMFPGA_HDR_FULL     2  /* 64 bytes - the kitchen sink */

/* Header sizes for each profile (in bytes) */
#define PHANTOMFPGA_HDR_SIZE_SIMPLE   16
#define PHANTOMFPGA_HDR_SIZE_STANDARD 32
#define PHANTOMFPGA_HDR_SIZE_FULL     64

/* ------------------------------------------------------------------------ */
/* SG-DMA Configuration Structure                                           */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_sg_config - Scatter-gather DMA configuration
 *
 * @desc_count:            Number of descriptors in the ring (power of 2)
 * @pkt_size_mode:         0 = fixed size, 1 = variable random size
 * @pkt_size:              Packet size in 64-bit words (or min size if variable)
 * @pkt_size_max:          Max packet size in 64-bit words (for variable mode)
 * @header_profile:        Header format (SIMPLE, STANDARD, FULL)
 * @pkt_rate:              Packet production rate in Hz
 * @irq_coalesce_count:    Generate IRQ after N completions
 * @irq_coalesce_timeout:  Generate IRQ after N microseconds
 * @reserved:              Reserved for future use, must be zero
 *
 * Packet sizes are in 64-bit words because that's how FPGAs think.
 * Multiply by 8 to get bytes if your brain works in bytes.
 *
 * Use with PHANTOMFPGA_IOCTL_SET_CFG before starting streaming.
 */
struct phantomfpga_sg_config {
	__u32 desc_count;           /* Descriptor count (power of 2, 4-4096) */
	__u32 pkt_size_mode;        /* 0=fixed, 1=variable */
	__u32 pkt_size;             /* Size in 64-bit words (or min for variable) */
	__u32 pkt_size_max;         /* Max size in 64-bit words (for variable) */
	__u32 header_profile;       /* 0=simple, 1=standard, 2=full */
	__u32 pkt_rate;             /* Packets per second (1-100000 Hz) */
	__u16 irq_coalesce_count;   /* IRQ after N completions */
	__u16 irq_coalesce_timeout; /* IRQ timeout in microseconds */
	__u32 reserved[4];          /* Reserved, must be zero */
};

/* Keep the old name as an alias for compatibility during transition */
#define phantomfpga_config phantomfpga_sg_config

/* ------------------------------------------------------------------------ */
/* Statistics Structure                                                     */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_sg_stats - Scatter-gather DMA statistics
 *
 * @packets_produced:   Total packets produced by device
 * @packets_consumed:   Total packets consumed by driver/userspace
 * @bytes_produced:     Total bytes produced (useful for bandwidth calc)
 * @bytes_consumed:     Total bytes consumed
 * @desc_completed:     Descriptors completed
 * @errors:             Error counter (DMA errors, device errors)
 * @irq_count:          Total interrupt count
 * @crc_errors:         CRC validation failures (driver-side)
 * @desc_head:          Current descriptor head (submitted)
 * @desc_tail:          Current descriptor tail (completed)
 * @status:             Device status register value
 * @reserved:           Reserved for future use
 *
 * Use with PHANTOMFPGA_IOCTL_GET_STATS to see what's going on.
 * High error counts or crc_errors mean something's wrong.
 */
struct phantomfpga_sg_stats {
	__u64 packets_produced;   /* Total packets produced */
	__u64 packets_consumed;   /* Total packets consumed */
	__u64 bytes_produced;     /* Total bytes produced */
	__u64 bytes_consumed;     /* Total bytes consumed */
	__u32 desc_completed;     /* Descriptors completed */
	__u32 errors;             /* Error counter */
	__u32 irq_count;          /* Total IRQ count */
	__u32 crc_errors;         /* CRC validation failures */
	__u32 desc_head;          /* Current head index */
	__u32 desc_tail;          /* Current tail index */
	__u32 status;             /* Device status register */
	__u32 reserved[5];        /* Reserved for future use */
};

/* Keep the old name as an alias */
#define phantomfpga_stats phantomfpga_sg_stats

/* ------------------------------------------------------------------------ */
/* Buffer Info Structure                                                    */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_buffer_info - Buffer information for mmap
 *
 * @buffer_size:    Size of each descriptor buffer in bytes
 * @buffer_count:   Number of buffers (same as descriptor count)
 * @total_size:     Total mappable size (buffer_size * buffer_count)
 * @header_size:    Size of packet header for current profile
 * @mmap_offset:    Offset to use with mmap() - currently always 0
 * @reserved:       Reserved for future use
 *
 * Use with PHANTOMFPGA_IOCTL_GET_BUFFER_INFO before mmap().
 * The mmap maps all descriptor buffers as one contiguous region.
 */
struct phantomfpga_buffer_info {
	__u64 buffer_size;     /* Size of each buffer */
	__u64 buffer_count;    /* Number of buffers */
	__u64 total_size;      /* Total mappable size */
	__u32 header_size;     /* Header size for current profile */
	__u32 reserved[5];     /* Reserved for future use */
};

/* ------------------------------------------------------------------------ */
/* Fault Injection Configuration                                            */
/* For when you want to test your error handling                            */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_fault_cfg - Fault injection configuration
 *
 * @inject_flags:   Which faults to enable (bitmask)
 * @fault_rate:     Probability: ~1 in N packets affected (0 = disabled)
 * @reserved:       Reserved for future use
 *
 * Fault flags:
 *   Bit 0: DROP_PACKET      - Drop packets randomly
 *   Bit 1: CORRUPT_HDR_CRC  - Corrupt header CRC32
 *   Bit 2: CORRUPT_PAY_CRC  - Corrupt payload CRC32
 *   Bit 3: CORRUPT_PAYLOAD  - Flip bits in payload data
 *   Bit 4: CORRUPT_SEQUENCE - Skip sequence numbers
 *   Bit 5: DELAY_IRQ        - Suppress interrupt delivery
 *
 * Use fault_rate to control probability:
 *   1000 = ~0.1% of packets affected (good for long tests)
 *   100  = ~1% affected
 *   10   = ~10% affected (aggressive testing)
 *
 * Use with PHANTOMFPGA_IOCTL_SET_FAULT. Set inject_flags=0 to disable.
 */
struct phantomfpga_fault_cfg {
	__u32 inject_flags;   /* Fault enable bitmask */
	__u32 fault_rate;     /* ~1 in N packets affected */
	__u32 reserved[4];    /* Reserved for future use */
};

/* Fault injection flag bits (guarded - may be defined in regs.h with BIT()) */
#ifndef PHANTOMFPGA_FAULT_DROP_PACKET
#define PHANTOMFPGA_FAULT_DROP_PACKET     (1 << 0)
#define PHANTOMFPGA_FAULT_CORRUPT_HDR_CRC (1 << 1)
#define PHANTOMFPGA_FAULT_CORRUPT_PAY_CRC (1 << 2)
#define PHANTOMFPGA_FAULT_CORRUPT_PAYLOAD (1 << 3)
#define PHANTOMFPGA_FAULT_CORRUPT_SEQ     (1 << 4)
#define PHANTOMFPGA_FAULT_DELAY_IRQ       (1 << 5)
#endif

/* ------------------------------------------------------------------------ */
/* IOCTL Commands                                                           */
/* ------------------------------------------------------------------------ */

/*
 * PHANTOMFPGA_IOCTL_SET_CFG - Configure device parameters
 *
 * Must be called before PHANTOMFPGA_IOCTL_START.
 * Cannot be called while device is streaming.
 *
 * Input:  struct phantomfpga_sg_config
 * Returns: 0 on success, -EINVAL for invalid params, -EBUSY if streaming
 */
#define PHANTOMFPGA_IOCTL_SET_CFG       _IOW(PHANTOMFPGA_IOC_MAGIC, 1, \
                                             struct phantomfpga_sg_config)

/*
 * PHANTOMFPGA_IOCTL_GET_CFG - Get current device configuration
 *
 * Output: struct phantomfpga_sg_config
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_CFG       _IOR(PHANTOMFPGA_IOC_MAGIC, 2, \
                                             struct phantomfpga_sg_config)

/*
 * PHANTOMFPGA_IOCTL_START - Start packet production
 *
 * Enables packet production. Device will start generating packets
 * at the configured rate and writing them to descriptor buffers.
 *
 * Returns: 0 on success, -EINVAL if not configured, -EBUSY if already started
 */
#define PHANTOMFPGA_IOCTL_START         _IO(PHANTOMFPGA_IOC_MAGIC, 3)

/*
 * PHANTOMFPGA_IOCTL_STOP - Stop packet production
 *
 * Disables packet production. Pending packets in buffers remain valid.
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_STOP          _IO(PHANTOMFPGA_IOC_MAGIC, 4)

/*
 * PHANTOMFPGA_IOCTL_GET_STATS - Get device statistics
 *
 * Output: struct phantomfpga_sg_stats
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_GET_STATS     _IOR(PHANTOMFPGA_IOC_MAGIC, 5, \
                                             struct phantomfpga_sg_stats)

/*
 * PHANTOMFPGA_IOCTL_RESET_STATS - Reset statistics counters
 *
 * Resets driver-side counters (packets_consumed, irq_count, crc_errors).
 * Device counters are reset via soft reset (not implemented here).
 *
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_RESET_STATS   _IO(PHANTOMFPGA_IOC_MAGIC, 6)

/*
 * PHANTOMFPGA_IOCTL_GET_BUFFER_INFO - Get buffer information for mmap
 *
 * Call this to get buffer parameters before mmap().
 *
 * Output: struct phantomfpga_buffer_info
 * Returns: 0 on success, -EINVAL if not configured
 */
#define PHANTOMFPGA_IOCTL_GET_BUFFER_INFO _IOR(PHANTOMFPGA_IOC_MAGIC, 7, \
                                               struct phantomfpga_buffer_info)

/*
 * PHANTOMFPGA_IOCTL_CONSUME_PKT - Mark one packet as consumed
 *
 * Advances the consumer index by one and resubmits the descriptor.
 * Use this after processing a packet when using mmap() access pattern.
 *
 * Returns: 0 on success, -EAGAIN if no packets available
 */
#define PHANTOMFPGA_IOCTL_CONSUME_PKT   _IO(PHANTOMFPGA_IOC_MAGIC, 8)

/* Alias for backwards compatibility */
#define PHANTOMFPGA_IOCTL_CONSUME_FRAME PHANTOMFPGA_IOCTL_CONSUME_PKT

/*
 * PHANTOMFPGA_IOCTL_SET_FAULT - Configure fault injection
 *
 * Enables fault injection for testing error handling code.
 * Set inject_flags=0 to disable all faults.
 *
 * Input: struct phantomfpga_fault_cfg
 * Returns: 0 on success
 */
#define PHANTOMFPGA_IOCTL_SET_FAULT     _IOW(PHANTOMFPGA_IOC_MAGIC, 9, \
                                             struct phantomfpga_fault_cfg)

/* Maximum ioctl command number for validation */
#define PHANTOMFPGA_IOC_MAXNR           9

/* ------------------------------------------------------------------------ */
/* Return Codes                                                             */
/* Standard errno values - nothing fancy here                               */
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
 * ENOTSUPP - Operation not supported (probably a TODO in the driver)
 */

/* ------------------------------------------------------------------------ */
/* Packet Header Structures (for userspace parsing)                         */
/* Note: If kernel regs.h was included first, skip these (it has __packed)  */
/* ------------------------------------------------------------------------ */

#ifndef PHANTOMFPGA_REGS_H  /* Only define if regs.h wasn't included first */

/*
 * These structures match what the device writes at the start of each packet.
 * Use them to parse packet headers after reading from the device.
 *
 * All fields are little-endian. Use le32toh()/le64toh() on big-endian systems.
 */

/* Magic value at start of every packet */
#ifndef PHANTOMFPGA_PACKET_MAGIC
#define PHANTOMFPGA_PACKET_MAGIC  0xABCD1234
#endif

/*
 * Simple header (16 bytes) - Profile 0
 */
struct phantomfpga_hdr_simple {
	__le32 magic;           /* 0xABCD1234 */
	__le32 sequence;        /* Packet sequence number */
	__le32 size;            /* Total packet size in bytes */
	__le32 reserved;        /* Padding */
};

/*
 * Standard header (32 bytes) - Profile 1
 * Includes timestamp and header CRC
 */
struct phantomfpga_hdr_standard {
	__le32 magic;           /* 0xABCD1234 */
	__le32 sequence;        /* Packet sequence number */
	__le64 timestamp;       /* Nanosecond timestamp */
	__le32 size;            /* Total packet size in bytes */
	__le32 counter;         /* Running counter */
	__le32 hdr_crc32;       /* CRC32 of bytes [0x00-0x17] (first 24 bytes) */
	__le32 reserved;        /* Padding */
};

/*
 * Full header (64 bytes) - Profile 2
 * Everything including payload CRC
 */
struct phantomfpga_hdr_full {
	__le32 magic;           /* 0xABCD1234 */
	__le32 version;         /* Header version (0x00020000) */
	__le32 sequence;        /* Packet sequence number */
	__le32 flags;           /* Packet flags */
	__le64 timestamp;       /* Nanosecond timestamp */
	__le64 mono_counter;    /* 64-bit monotonic counter */
	__le32 size;            /* Total packet size in bytes */
	__le32 payload_size;    /* Payload size (size - header) */
	__le32 hdr_crc32;       /* CRC32 of bytes [0x00-0x27] (first 40 bytes) */
	__le32 payload_crc32;   /* CRC32 of payload bytes */
	__le64 channel;         /* Source channel ID */
	__le64 reserved;        /* Future use */
};

/* Flags in full header */
#define PHANTOMFPGA_PKT_FLAG_CORRUPTED  (1 << 0)  /* Intentionally corrupted */
#define PHANTOMFPGA_PKT_FLAG_VARIABLE   (1 << 1)  /* Variable-size packet */

/*
 * Completion structure (16 bytes)
 * Written at (buffer_end - 16) when a descriptor completes
 */
struct phantomfpga_completion {
	__le32 status;          /* 0=OK, else error code */
	__le32 actual_length;   /* Bytes actually transferred */
	__le64 timestamp;       /* Device timestamp */
};

/* Completion status codes */
#define PHANTOMFPGA_COMPL_OK        0
#define PHANTOMFPGA_COMPL_ERR_DMA   1
#define PHANTOMFPGA_COMPL_ERR_SIZE  2

#endif /* PHANTOMFPGA_REGS_H */

#endif /* PHANTOMFPGA_UAPI_H */
