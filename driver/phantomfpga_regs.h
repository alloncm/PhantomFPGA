/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PhantomFPGA Register Definitions v2.0 - Scatter-Gather DMA Edition
 *
 * This header mirrors the QEMU device register layout. The trainee driver
 * uses these definitions to communicate with the virtual FPGA device.
 *
 * v2.0 brings scatter-gather DMA descriptors, multiple header profiles,
 * variable packet sizes, and dual CRCs. It's basically XDMA-lite with
 * training wheels... and a sense of humor.
 *
 * Matches: platform/qemu/src/hw/misc/phantomfpga.h
 */

#ifndef PHANTOMFPGA_REGS_H
#define PHANTOMFPGA_REGS_H

#include <linux/types.h>

/* ------------------------------------------------------------------------ */
/* PCI Device Identification                                                */
/* "Hello, my name is..."                                                   */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x1DAD   /* One DAD to rule them all */
#define PHANTOMFPGA_DEVICE_ID       0xF00D   /* What every programmer needs */
#define PHANTOMFPGA_SUBSYS_VENDOR   0x1DAD
#define PHANTOMFPGA_SUBSYS_ID       0x0002   /* SG-DMA edition */
#define PHANTOMFPGA_REVISION        0x02

/* ------------------------------------------------------------------------ */
/* Device Constants                                                         */
/* The numbers that make everything tick                                    */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_BAR0_SIZE       4096
#define PHANTOMFPGA_PACKET_MAGIC    0xABCD1234
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE  /* Expected in DEV_ID register */
#define PHANTOMFPGA_DEV_VER         0x00020000  /* v2.0.0 - SG-DMA edition */
#define PHANTOMFPGA_MSIX_VECTORS    2

/*
 * Default values - sensible starting points for the indecisive
 */
#define PHANTOMFPGA_DEFAULT_PKT_SIZE     256   /* In 64-bit words = 2KB */
#define PHANTOMFPGA_DEFAULT_PKT_SIZE_MAX 512   /* In 64-bit words = 4KB */
#define PHANTOMFPGA_DEFAULT_PKT_RATE     1000  /* Hz - frames per second */
#define PHANTOMFPGA_DEFAULT_DESC_COUNT   256   /* Descriptor ring entries */
#define PHANTOMFPGA_DEFAULT_IRQ_COUNT    16    /* IRQ after N completions */
#define PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT  1000  /* IRQ timeout in microseconds */
#define PHANTOMFPGA_DEFAULT_FAULT_RATE   1000  /* 1 in N packets affected */

/*
 * Limits - because even virtual hardware has boundaries
 */
#define PHANTOMFPGA_MIN_PKT_SIZE         8     /* 64 bytes - room for largest header */
#define PHANTOMFPGA_MAX_PKT_SIZE         8192  /* 64KB - let's not get crazy */
#define PHANTOMFPGA_MIN_DESC_COUNT       4
#define PHANTOMFPGA_MAX_DESC_COUNT       4096
#define PHANTOMFPGA_MIN_PKT_RATE         1
#define PHANTOMFPGA_MAX_PKT_RATE         100000

/* ------------------------------------------------------------------------ */
/* Header Profiles                                                          */
/* Pick your level of complexity (and debugging difficulty)                 */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_HDR_PROFILE_SIMPLE   0  /* 16 bytes - training wheels */
#define PHANTOMFPGA_HDR_PROFILE_STANDARD 1  /* 32 bytes - moderate challenge */
#define PHANTOMFPGA_HDR_PROFILE_FULL     2  /* 64 bytes - boss level */

#define PHANTOMFPGA_HDR_SIZE_SIMPLE      16
#define PHANTOMFPGA_HDR_SIZE_STANDARD    32
#define PHANTOMFPGA_HDR_SIZE_FULL        64

/* ------------------------------------------------------------------------ */
/* Register Offsets (from BAR0 base)                                        */
/* The sacred addresses - handle with care                                  */
/* ------------------------------------------------------------------------ */

/* Identification */
#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID (0xF00DFACE) */
#define PHANTOMFPGA_REG_DEV_VER         0x004   /* R   - Device Version */

/* Control */
#define PHANTOMFPGA_REG_CTRL            0x008   /* R/W - Control Register */
#define PHANTOMFPGA_REG_STATUS          0x00C   /* R   - Status Register */

/* Packet Configuration */
#define PHANTOMFPGA_REG_PKT_SIZE_MODE   0x010   /* R/W - 0=fixed, 1=variable */
#define PHANTOMFPGA_REG_PKT_SIZE        0x014   /* R/W - Size (or min) in 64-bit words */
#define PHANTOMFPGA_REG_PKT_SIZE_MAX    0x018   /* R/W - Max size for variable mode */
#define PHANTOMFPGA_REG_HDR_PROFILE     0x01C   /* R/W - Header profile (0-2) */
#define PHANTOMFPGA_REG_PKT_RATE        0x020   /* R/W - Packets per second */

/* Descriptor Ring Configuration */
#define PHANTOMFPGA_REG_DESC_RING_LO    0x024   /* R/W - Descriptor ring base [31:0] */
#define PHANTOMFPGA_REG_DESC_RING_HI    0x028   /* R/W - Descriptor ring base [63:32] */
#define PHANTOMFPGA_REG_DESC_RING_SIZE  0x02C   /* R/W - Number of descriptors (power of 2) */
#define PHANTOMFPGA_REG_DESC_HEAD       0x030   /* R/W - Head: driver writes to submit */
#define PHANTOMFPGA_REG_DESC_TAIL       0x034   /* R   - Tail: device updates on completion */

/* Interrupts */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x038   /* R/W1C - IRQ status */
#define PHANTOMFPGA_REG_IRQ_MASK        0x03C   /* R/W   - IRQ enable mask */
#define PHANTOMFPGA_REG_IRQ_COALESCE    0x040   /* R/W   - [15:0]=count [31:16]=timeout_us */

/* Statistics - for the curious and the debugging */
#define PHANTOMFPGA_REG_STAT_PACKETS    0x044   /* R   - Packets produced */
#define PHANTOMFPGA_REG_STAT_BYTES_LO   0x048   /* R   - Total bytes [31:0] */
#define PHANTOMFPGA_REG_STAT_BYTES_HI   0x04C   /* R   - Total bytes [63:32] */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x050   /* R   - Error count */
#define PHANTOMFPGA_REG_STAT_DESC_COMPL 0x054   /* R   - Descriptors completed */

/* Fault Injection - for masochists and thorough testers */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x058   /* R/W - Fault injection control */
#define PHANTOMFPGA_REG_FAULT_RATE      0x05C   /* R/W - ~1/N packets affected */

#define PHANTOMFPGA_REG_MAX             0x060   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL @ 0x008) Bits                                     */
/* The big red buttons                                                      */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_RUN            BIT(0)  /* Enable packet production */
#define PHANTOMFPGA_CTRL_RESET          BIT(1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         BIT(2)  /* Global interrupt enable */

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS @ 0x00C) Bits                                    */
/* What's going on in there?                                                */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      BIT(0)  /* Device is producing packets */
#define PHANTOMFPGA_STATUS_DESC_EMPTY   BIT(1)  /* No descriptors available */
#define PHANTOMFPGA_STATUS_ERROR        BIT(2)  /* Something went wrong */

/* ------------------------------------------------------------------------ */
/* IRQ Status/Mask (@ 0x038, 0x03C) Bits                                    */
/* Why did it interrupt me?                                                 */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_COMPLETE        BIT(0)  /* Descriptor(s) completed */
#define PHANTOMFPGA_IRQ_ERROR           BIT(1)  /* Error occurred */
#define PHANTOMFPGA_IRQ_NO_DESC         BIT(2)  /* Ran out of descriptors */

#define PHANTOMFPGA_IRQ_ALL             (PHANTOMFPGA_IRQ_COMPLETE | \
                                         PHANTOMFPGA_IRQ_ERROR | \
                                         PHANTOMFPGA_IRQ_NO_DESC)

/* MSI-X vector assignments */
#define PHANTOMFPGA_MSIX_VEC_COMPLETE   0
#define PHANTOMFPGA_MSIX_VEC_ERROR      1

/* ------------------------------------------------------------------------ */
/* IRQ Coalesce Register (@ 0x040) Fields                                   */
/* Batch those interrupts like a pro                                        */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_COAL_COUNT_MASK  0x0000FFFF
#define PHANTOMFPGA_IRQ_COAL_COUNT_SHIFT 0
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_MASK  0xFFFF0000
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_SHIFT 16

static inline u32 phantomfpga_irq_coalesce_pack(u16 count, u16 timeout_us)
{
	return ((u32)timeout_us << 16) | count;
}

static inline void phantomfpga_irq_coalesce_unpack(u32 val, u16 *count, u16 *timeout_us)
{
	*count = val & 0xFFFF;
	*timeout_us = val >> 16;
}

/* ------------------------------------------------------------------------ */
/* Fault Injection (@ 0x058) Bits                                           */
/* For when you want to see the world burn (in a controlled way)            */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_DROP_PACKET     BIT(0)  /* Drop packets randomly */
#define PHANTOMFPGA_FAULT_CORRUPT_HDR_CRC BIT(1)  /* Corrupt header CRC32 */
#define PHANTOMFPGA_FAULT_CORRUPT_PAY_CRC BIT(2)  /* Corrupt payload CRC32 */
#define PHANTOMFPGA_FAULT_CORRUPT_PAYLOAD BIT(3)  /* Flip bits in payload */
#define PHANTOMFPGA_FAULT_CORRUPT_SEQ     BIT(4)  /* Skip sequence numbers */
#define PHANTOMFPGA_FAULT_DELAY_IRQ       BIT(5)  /* Suppress interrupts */

/* ------------------------------------------------------------------------ */
/* Scatter-Gather Descriptor (32 bytes)                                     */
/* The workhorse of DMA - Xilinx XDMA would be proud (well, almost)         */
/* ------------------------------------------------------------------------ */

/*
 * Descriptor control flags - what should happen with this buffer
 */
#define PHANTOMFPGA_DESC_CTRL_COMPLETED  BIT(0)  /* Device sets when done */
#define PHANTOMFPGA_DESC_CTRL_EOP        BIT(1)  /* End of packet */
#define PHANTOMFPGA_DESC_CTRL_SOP        BIT(2)  /* Start of packet */
#define PHANTOMFPGA_DESC_CTRL_IRQ        BIT(3)  /* Generate IRQ on completion */
#define PHANTOMFPGA_DESC_CTRL_STOP       BIT(4)  /* Stop after this descriptor */

/*
 * Scatter-gather descriptor structure.
 *
 * The driver allocates a ring of these descriptors and per-descriptor
 * buffers. The device fetches descriptors, writes packet data to dst_addr,
 * and sets COMPLETED when done.
 *
 * Must match the QEMU device's PhantomFPGASGDesc structure exactly.
 */
struct phantomfpga_sg_desc {
	__le32 control;        /* Flags: COMPLETED, EOP, SOP, IRQ, STOP */
	__le32 length;         /* Buffer length in bytes */
	__le64 dst_addr;       /* Host destination address */
	__le64 next_desc;      /* Next descriptor address (0 = end of chain) */
	__le64 reserved;       /* Alignment / future use */
} __packed;

#define PHANTOMFPGA_DESC_SIZE   sizeof(struct phantomfpga_sg_desc)

/* ------------------------------------------------------------------------ */
/* Completion Writeback (16 bytes)                                          */
/* Written at the end of each buffer when the descriptor completes          */
/* ------------------------------------------------------------------------ */

/*
 * Completion status codes - what happened during the transfer
 */
#define PHANTOMFPGA_COMPL_OK             0
#define PHANTOMFPGA_COMPL_ERR_DMA        1
#define PHANTOMFPGA_COMPL_ERR_SIZE       2

/*
 * Completion structure written at (buffer_end - 16) when descriptor
 * completes. The driver reads this to verify the transfer and get
 * the actual length written.
 */
struct phantomfpga_completion {
	__le32 status;         /* 0=OK, else error code */
	__le32 actual_length;  /* Bytes actually transferred */
	__le64 timestamp;      /* Device timestamp (ns since start) */
} __packed;

#define PHANTOMFPGA_COMPL_SIZE  sizeof(struct phantomfpga_completion)

/* ------------------------------------------------------------------------ */
/* Packet Headers                                                           */
/* Three flavors of metadata - pick your poison                             */
/* ------------------------------------------------------------------------ */

/*
 * Profile 0: Simple Header (16 bytes)
 * The "Hello World" of packet headers. Just the basics.
 */
struct phantomfpga_hdr_simple {
	__le32 magic;          /* 0xABCD1234 */
	__le32 sequence;       /* Packet sequence number */
	__le32 size;           /* Total packet size */
	__le32 reserved;       /* Padding for alignment */
} __packed;

/*
 * Profile 1: Standard Header (32 bytes)
 * Now with timestamps and CRC. Things are getting serious.
 */
struct phantomfpga_hdr_standard {
	__le32 magic;          /* 0xABCD1234 */
	__le32 sequence;       /* Packet sequence number */
	__le64 timestamp;      /* Nanosecond timestamp */
	__le32 size;           /* Total packet size */
	__le32 counter;        /* Running counter */
	__le32 hdr_crc32;      /* CRC32 of bytes [0x00-0x17] (first 24 bytes) */
	__le32 reserved;       /* Padding */
} __packed;

/*
 * Profile 2: Full Header (64 bytes)
 * The kitchen sink edition. Everything but your mother's recipe.
 *
 * If you've made it to implementing this profile, congratulations -
 * you're either very thorough or very lost. Either way, we respect it.
 */
struct phantomfpga_hdr_full {
	__le32 magic;          /* 0xABCD1234 */
	__le32 version;        /* Header version (0x00020000) */
	__le32 sequence;       /* Packet sequence number */
	__le32 flags;          /* Packet flags */
	__le64 timestamp;      /* Nanosecond timestamp */
	__le64 mono_counter;   /* 64-bit monotonic counter */
	__le32 size;           /* Total packet size */
	__le32 payload_size;   /* Payload size (size - header) */
	__le32 hdr_crc32;      /* CRC32 of bytes [0x00-0x27] (first 40 bytes) */
	__le32 payload_crc32;  /* CRC32 of payload bytes */
	__le64 channel;        /* Source channel ID */
	__le64 reserved;       /* Future use */
} __packed;

/* Full header flags */
#define PHANTOMFPGA_PKT_FLAG_CORRUPTED   BIT(0)  /* Intentionally corrupted (fault injection) */
#define PHANTOMFPGA_PKT_FLAG_VARIABLE    BIT(1)  /* Variable-size packet */

/* ------------------------------------------------------------------------ */
/* Helper Functions and Macros                                              */
/* Making life slightly less painful                                        */
/* ------------------------------------------------------------------------ */

/*
 * Convert packet size from 64-bit words to bytes.
 * Because FPGA folks think in bus widths, not bytes.
 */
static inline u32 phantomfpga_words_to_bytes(u32 words)
{
	return words * 8;
}

/*
 * Convert bytes to 64-bit words (rounded up).
 */
static inline u32 phantomfpga_bytes_to_words(u32 bytes)
{
	return (bytes + 7) / 8;
}

/*
 * Calculate number of available descriptors in the ring.
 * Ring size must be a power of 2.
 */
static inline u32 phantomfpga_desc_pending(u32 head, u32 tail, u32 ring_size)
{
	return (head - tail) & (ring_size - 1);
}

/*
 * Calculate number of free descriptor slots.
 * We keep one slot empty to distinguish full from empty.
 */
static inline u32 phantomfpga_desc_free(u32 head, u32 tail, u32 ring_size)
{
	return ring_size - 1 - phantomfpga_desc_pending(head, tail, ring_size);
}

/*
 * Check if descriptor ring is empty.
 */
static inline bool phantomfpga_desc_ring_empty(u32 head, u32 tail)
{
	return head == tail;
}

/*
 * Check if descriptor ring is full.
 */
static inline bool phantomfpga_desc_ring_full(u32 head, u32 tail, u32 ring_size)
{
	u32 next_head = (head + 1) & (ring_size - 1);
	return next_head == tail;
}

/*
 * Get header size for a given profile.
 * Returns 0 for invalid profile (caller should validate).
 */
static inline u32 phantomfpga_header_size(u32 profile)
{
	switch (profile) {
	case PHANTOMFPGA_HDR_PROFILE_SIMPLE:
		return PHANTOMFPGA_HDR_SIZE_SIMPLE;
	case PHANTOMFPGA_HDR_PROFILE_STANDARD:
		return PHANTOMFPGA_HDR_SIZE_STANDARD;
	case PHANTOMFPGA_HDR_PROFILE_FULL:
		return PHANTOMFPGA_HDR_SIZE_FULL;
	default:
		return 0;
	}
}

/*
 * Get completion struct location within a buffer.
 * It lives at the end of the buffer, not the end of the data.
 */
static inline void *phantomfpga_completion_ptr(void *buffer, u32 buffer_size)
{
	return (u8 *)buffer + buffer_size - PHANTOMFPGA_COMPL_SIZE;
}

/*
 * CRC32 polynomial for verification.
 * IEEE 802.3 (Ethernet) - same as the device uses.
 */
#define PHANTOMFPGA_CRC32_POLY  0xEDB88320

#endif /* PHANTOMFPGA_REGS_H */
