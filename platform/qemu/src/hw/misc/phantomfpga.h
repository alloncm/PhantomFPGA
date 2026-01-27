/*
 * PhantomFPGA QEMU Device Definitions
 *
 * A virtual FPGA device for testing DMA ring buffer drivers.
 * Simulates a frame producer that writes data to guest memory via DMA.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_PHANTOMFPGA_H
#define HW_MISC_PHANTOMFPGA_H

#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "qemu/timer.h"
#include "qom/object.h"

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
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00010000  /* v1.0.0 */
#define PHANTOMFPGA_MSIX_VECTORS    2

/* Default values */
#define PHANTOMFPGA_DEFAULT_FRAME_SIZE  4096
#define PHANTOMFPGA_DEFAULT_FRAME_RATE  1000    /* Hz */
#define PHANTOMFPGA_DEFAULT_RING_SIZE   256     /* entries */
#define PHANTOMFPGA_DEFAULT_WATERMARK   64      /* frames */

/* Limits */
#define PHANTOMFPGA_MIN_FRAME_SIZE      64
#define PHANTOMFPGA_MAX_FRAME_SIZE      (64 * 1024)
#define PHANTOMFPGA_MIN_RING_SIZE       4
#define PHANTOMFPGA_MAX_RING_SIZE       4096
#define PHANTOMFPGA_MIN_FRAME_RATE      1
#define PHANTOMFPGA_MAX_FRAME_RATE      100000

/* ------------------------------------------------------------------------ */
/* Register Offsets                                                         */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID */
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
#define PHANTOMFPGA_REG_PROD_IDX        0x02C   /* R   - Producer index */
#define PHANTOMFPGA_REG_CONS_IDX        0x030   /* R/W - Consumer index */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x034   /* R/W - IRQ status (W1C) */
#define PHANTOMFPGA_REG_IRQ_MASK        0x038   /* R/W - IRQ mask */
#define PHANTOMFPGA_REG_STAT_FRAMES     0x03C   /* R   - Frames produced counter */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x040   /* R   - Error counter */
#define PHANTOMFPGA_REG_STAT_OVERRUNS   0x044   /* R   - Overrun counter */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x048   /* R/W - Fault injection control */

#define PHANTOMFPGA_REG_MAX             0x04C   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL) Bits                                             */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_START          (1 << 0)  /* Enable frame production */
#define PHANTOMFPGA_CTRL_RESET          (1 << 1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         (1 << 2)  /* Global interrupt enable */

#define PHANTOMFPGA_CTRL_WRITE_MASK     (PHANTOMFPGA_CTRL_START | \
                                         PHANTOMFPGA_CTRL_RESET | \
                                         PHANTOMFPGA_CTRL_IRQ_EN)

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS) Bits                                            */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      (1 << 0)  /* Device is producing frames */
#define PHANTOMFPGA_STATUS_OVERRUN      (1 << 1)  /* Ring buffer full */
#define PHANTOMFPGA_STATUS_ERROR        (1 << 2)  /* Error condition */

/* ------------------------------------------------------------------------ */
/* IRQ Status/Mask Bits                                                     */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_WATERMARK       (1 << 0)  /* Watermark threshold reached */
#define PHANTOMFPGA_IRQ_OVERRUN         (1 << 1)  /* Buffer overrun occurred */

#define PHANTOMFPGA_IRQ_ALL             (PHANTOMFPGA_IRQ_WATERMARK | \
                                         PHANTOMFPGA_IRQ_OVERRUN)

/* MSI-X vector assignments */
#define PHANTOMFPGA_MSIX_VEC_WATERMARK  0
#define PHANTOMFPGA_MSIX_VEC_OVERRUN    1

/* ------------------------------------------------------------------------ */
/* Fault Injection Bits                                                     */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_DROP_FRAMES   (1 << 0)  /* Randomly drop frames */
#define PHANTOMFPGA_FAULT_CORRUPT_DATA  (1 << 1)  /* Corrupt frame payload */
#define PHANTOMFPGA_FAULT_DELAY_IRQ     (1 << 2)  /* Delay interrupt delivery */

#define PHANTOMFPGA_FAULT_ALL           (PHANTOMFPGA_FAULT_DROP_FRAMES | \
                                         PHANTOMFPGA_FAULT_CORRUPT_DATA | \
                                         PHANTOMFPGA_FAULT_DELAY_IRQ)

/* ------------------------------------------------------------------------ */
/* Frame Header Structure                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Frame header written at the start of each DMA frame.
 * The driver uses this to validate and sequence frames.
 */
typedef struct PhantomFPGAFrameHeader {
    uint32_t magic;         /* PHANTOMFPGA_FRAME_MAGIC (0xABCD1234) */
    uint32_t seq;           /* Sequence number, monotonically increasing */
    uint64_t ts_ns;         /* Timestamp in nanoseconds (CLOCK_MONOTONIC) */
    uint32_t payload_len;   /* Payload length in bytes (excludes header) */
    uint32_t flags;         /* Frame flags (reserved, set to 0) */
} QEMU_PACKED PhantomFPGAFrameHeader;

#define PHANTOMFPGA_FRAME_HEADER_SIZE   sizeof(PhantomFPGAFrameHeader)

/* Frame flags (for future use) */
#define PHANTOMFPGA_FRAME_FLAG_CORRUPTED    (1 << 0)  /* Intentionally corrupted */

/* ------------------------------------------------------------------------ */
/* QOM Type Definitions                                                     */
/* ------------------------------------------------------------------------ */

#define TYPE_PHANTOMFPGA "phantomfpga"

OBJECT_DECLARE_SIMPLE_TYPE(PhantomFPGAState, PHANTOMFPGA)

/* ------------------------------------------------------------------------ */
/* Device State Structure                                                   */
/* ------------------------------------------------------------------------ */

typedef struct PhantomFPGAState {
    /* Parent object - must be first */
    PCIDevice parent_obj;

    /* Memory region for BAR0 (registers) */
    MemoryRegion bar0;

    /* MSI-X configuration */
    bool msix_enabled;

    /* Frame production timer */
    QEMUTimer *frame_timer;
    int64_t frame_interval_ns;  /* Nanoseconds between frames */

    /* Ring buffer state */
    uint32_t prod_idx;          /* Producer index (device writes) */
    uint32_t cons_idx;          /* Consumer index (driver writes) */
    uint32_t ring_size;         /* Number of ring entries */

    /* DMA configuration */
    uint64_t dma_addr;          /* DMA base address (physical) */
    uint32_t dma_size;          /* Total DMA buffer size */
    uint32_t frame_size;        /* Size of each frame slot */

    /* Frame generation */
    uint32_t frame_rate;        /* Target frame rate in Hz */
    uint32_t watermark;         /* IRQ watermark threshold */
    uint32_t sequence;          /* Next sequence number */

    /* Control and status */
    uint32_t ctrl;              /* Control register value */
    uint32_t status;            /* Status register value */
    uint32_t irq_status;        /* Pending IRQ flags */
    uint32_t irq_mask;          /* IRQ enable mask */

    /* Statistics counters */
    uint32_t stat_frames;       /* Total frames produced */
    uint32_t stat_errors;       /* Total errors */
    uint32_t stat_overruns;     /* Total overruns */

    /* Fault injection */
    uint32_t fault_inject;      /* Fault injection flags */
    uint32_t fault_drop_counter; /* Internal: for random drop logic */

} PhantomFPGAState;

/* ------------------------------------------------------------------------ */
/* Helper Macros                                                            */
/* ------------------------------------------------------------------------ */

/* Calculate number of frames pending in ring buffer */
static inline uint32_t phantomfpga_ring_pending(PhantomFPGAState *s)
{
    return (s->prod_idx - s->cons_idx) & (s->ring_size - 1);
}

/* Check if ring buffer is full */
static inline bool phantomfpga_ring_full(PhantomFPGAState *s)
{
    /*
     * Ring is full when producer would overwrite unread data.
     * We keep one slot empty to distinguish full from empty.
     */
    uint32_t next_prod = (s->prod_idx + 1) & (s->ring_size - 1);
    return next_prod == s->cons_idx;
}

/* Check if ring buffer is empty */
static inline bool phantomfpga_ring_empty(PhantomFPGAState *s)
{
    return s->prod_idx == s->cons_idx;
}

/* Calculate frame slot address in DMA buffer */
static inline uint64_t phantomfpga_frame_addr(PhantomFPGAState *s, uint32_t idx)
{
    return s->dma_addr + ((uint64_t)idx * s->frame_size);
}

#endif /* HW_MISC_PHANTOMFPGA_H */
