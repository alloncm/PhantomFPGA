/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PhantomFPGA Register Definitions (Kernel Driver)
 *
 * This header mirrors the QEMU device register layout. The trainee driver
 * uses these definitions to communicate with the virtual FPGA device.
 *
 * Matches: platform/qemu/src/hw/misc/phantomfpga.h
 */

#ifndef PHANTOMFPGA_REGS_H
#define PHANTOMFPGA_REGS_H

#include <linux/types.h>

/* ------------------------------------------------------------------------ */
/* PCI Device Identification                                                */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x1DAD
#define PHANTOMFPGA_DEVICE_ID       0xF00D
#define PHANTOMFPGA_SUBSYS_VENDOR   0x1DAD
#define PHANTOMFPGA_SUBSYS_ID       0x0001
#define PHANTOMFPGA_REVISION        0x01

/* ------------------------------------------------------------------------ */
/* Device Constants                                                         */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_BAR0_SIZE       4096
#define PHANTOMFPGA_FRAME_MAGIC     0xABCD1234
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE  /* Expected value in DEV_ID register */
#define PHANTOMFPGA_DEV_VER         0x00010000  /* v1.0.0 */
#define PHANTOMFPGA_MSIX_VECTORS    2

/* Default values (used if driver doesn't configure) */
#define PHANTOMFPGA_DEFAULT_FRAME_SIZE  4096
#define PHANTOMFPGA_DEFAULT_FRAME_RATE  1000    /* Hz */
#define PHANTOMFPGA_DEFAULT_RING_SIZE   256     /* entries */
#define PHANTOMFPGA_DEFAULT_WATERMARK   64      /* frames */

/* Limits (driver must respect these) */
#define PHANTOMFPGA_MIN_FRAME_SIZE      64
#define PHANTOMFPGA_MAX_FRAME_SIZE      (64 * 1024)
#define PHANTOMFPGA_MIN_RING_SIZE       4
#define PHANTOMFPGA_MAX_RING_SIZE       4096
#define PHANTOMFPGA_MIN_FRAME_RATE      1
#define PHANTOMFPGA_MAX_FRAME_RATE      100000

/* ------------------------------------------------------------------------ */
/* Register Offsets (from BAR0 base)                                        */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID (reads 0xF00DFACE) */
#define PHANTOMFPGA_REG_DEV_VER         0x004   /* R   - Device Version */
#define PHANTOMFPGA_REG_CTRL            0x008   /* R/W - Control Register */
#define PHANTOMFPGA_REG_STATUS          0x00C   /* R   - Status Register */
#define PHANTOMFPGA_REG_FRAME_SIZE      0x010   /* R/W - Frame size in bytes */
#define PHANTOMFPGA_REG_FRAME_RATE      0x014   /* R/W - Frame rate in Hz */
#define PHANTOMFPGA_REG_WATERMARK       0x018   /* R/W - IRQ watermark threshold */
#define PHANTOMFPGA_REG_RING_SIZE       0x01C   /* R/W - Ring buffer entry count */
#define PHANTOMFPGA_REG_DMA_ADDR_LO     0x020   /* R/W - DMA base address low 32 bits */
#define PHANTOMFPGA_REG_DMA_ADDR_HI     0x024   /* R/W - DMA base address high 32 bits */
#define PHANTOMFPGA_REG_DMA_SIZE        0x028   /* R/W - DMA buffer total size */
#define PHANTOMFPGA_REG_PROD_IDX        0x02C   /* R   - Producer index (device writes) */
#define PHANTOMFPGA_REG_CONS_IDX        0x030   /* R/W - Consumer index (driver writes) */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x034   /* R/W - IRQ status (write 1 to clear) */
#define PHANTOMFPGA_REG_IRQ_MASK        0x038   /* R/W - IRQ mask (1 = enabled) */
#define PHANTOMFPGA_REG_STAT_FRAMES     0x03C   /* R   - Frames produced counter */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x040   /* R   - Error counter */
#define PHANTOMFPGA_REG_STAT_OVERRUNS   0x044   /* R   - Overrun counter */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x048   /* R/W - Fault injection control */

#define PHANTOMFPGA_REG_MAX             0x04C   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL @ 0x008) Bits                                     */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_START          BIT(0)  /* Enable frame production */
#define PHANTOMFPGA_CTRL_RESET          BIT(1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         BIT(2)  /* Global interrupt enable */

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS @ 0x00C) Bits                                    */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      BIT(0)  /* Device is producing frames */
#define PHANTOMFPGA_STATUS_OVERRUN      BIT(1)  /* Ring buffer full */
#define PHANTOMFPGA_STATUS_ERROR        BIT(2)  /* Error condition */

/* ------------------------------------------------------------------------ */
/* IRQ Status/Mask (@ 0x034, 0x038) Bits                                    */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_WATERMARK       BIT(0)  /* Watermark threshold reached */
#define PHANTOMFPGA_IRQ_OVERRUN         BIT(1)  /* Buffer overrun occurred */

#define PHANTOMFPGA_IRQ_ALL             (PHANTOMFPGA_IRQ_WATERMARK | \
                                         PHANTOMFPGA_IRQ_OVERRUN)

/* MSI-X vector assignments */
#define PHANTOMFPGA_MSIX_VEC_WATERMARK  0
#define PHANTOMFPGA_MSIX_VEC_OVERRUN    1

/* ------------------------------------------------------------------------ */
/* Fault Injection (@ 0x048) Bits                                           */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_DROP_FRAMES   BIT(0)  /* Randomly drop frames */
#define PHANTOMFPGA_FAULT_CORRUPT_DATA  BIT(1)  /* Corrupt frame payload */
#define PHANTOMFPGA_FAULT_DELAY_IRQ     BIT(2)  /* Delay interrupt delivery */

/* ------------------------------------------------------------------------ */
/* Frame Header Structure                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Frame header written at the start of each DMA frame by the device.
 * The driver uses this to validate and sequence frames.
 *
 * Layout in DMA buffer:
 *   +00: magic       (u32) - 0xABCD1234
 *   +04: seq         (u32) - Sequence number
 *   +08: ts_ns       (u64) - Timestamp in nanoseconds
 *   +16: payload_len (u32) - Payload length (excludes header)
 *   +20: flags       (u32) - Frame flags
 *   +24: [payload data...]
 */
struct phantomfpga_frame_header {
	__le32 magic;           /* PHANTOMFPGA_FRAME_MAGIC */
	__le32 seq;             /* Sequence number, monotonically increasing */
	__le64 ts_ns;           /* Timestamp in nanoseconds */
	__le32 payload_len;     /* Payload length in bytes */
	__le32 flags;           /* Frame flags (see below) */
} __packed;

#define PHANTOMFPGA_FRAME_HEADER_SIZE   sizeof(struct phantomfpga_frame_header)

/* Frame flags */
#define PHANTOMFPGA_FRAME_FLAG_CORRUPTED    BIT(0)  /* Intentionally corrupted */

/* ------------------------------------------------------------------------ */
/* Helper Macros for Ring Buffer Calculations                              */
/* ------------------------------------------------------------------------ */

/*
 * Calculate number of frames pending in ring buffer.
 * Ring size must be a power of 2.
 */
static inline u32 phantomfpga_ring_pending(u32 prod_idx, u32 cons_idx, u32 ring_size)
{
	return (prod_idx - cons_idx) & (ring_size - 1);
}

/*
 * Check if ring buffer is full.
 * We keep one slot empty to distinguish full from empty.
 */
static inline bool phantomfpga_ring_full(u32 prod_idx, u32 cons_idx, u32 ring_size)
{
	u32 next_prod = (prod_idx + 1) & (ring_size - 1);

	return next_prod == cons_idx;
}

/*
 * Check if ring buffer is empty.
 */
static inline bool phantomfpga_ring_empty(u32 prod_idx, u32 cons_idx)
{
	return prod_idx == cons_idx;
}

/*
 * Calculate frame slot address offset in DMA buffer.
 */
static inline u64 phantomfpga_frame_offset(u32 idx, u32 frame_size)
{
	return (u64)idx * frame_size;
}

#endif /* PHANTOMFPGA_REGS_H */
