/*
 * PhantomFPGA QEMU Device Definitions - v3.0 ASCII Animation Edition
 *
 * A virtual FPGA device that streams pre-built ASCII animation frames.
 * The trainee's mission: build a driver, stream frames over TCP,
 * watch a cartoon play in the terminal.
 *
 * "Because nothing says 'I learned DMA' like ASCII art."
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

#define PHANTOMFPGA_VENDOR_ID       0x0DAD
#define PHANTOMFPGA_DEVICE_ID       0xF00D
#define PHANTOMFPGA_SUBSYS_VENDOR   0x0DAD
#define PHANTOMFPGA_SUBSYS_ID       0x0003       /* ASCII Animation edition */
#define PHANTOMFPGA_REVISION        0x03         /* v3.0 */

/* ------------------------------------------------------------------------ */
/* Device Constants                                                         */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_BAR0_SIZE       4096
#define PHANTOMFPGA_FRAME_MAGIC     0xF00DFACE   /* New magic for frames */
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00030000   /* v3.0.0 - ASCII Animation */
#define PHANTOMFPGA_MSIX_VECTORS    3            /* Complete, error, no_desc */

/* Frame constants - from frames_data.h */
#define PHANTOMFPGA_FRAME_SIZE      5120         /* Bytes per frame */
#define PHANTOMFPGA_FRAME_COUNT     250          /* Total frames (10 sec @ 25fps) */
#define PHANTOMFPGA_FRAME_DATA_SIZE 4995         /* ASCII data portion */
#define PHANTOMFPGA_FRAME_ROWS      45
#define PHANTOMFPGA_FRAME_COLS      110

/* Default values */
#define PHANTOMFPGA_DEFAULT_FRAME_RATE  25       /* 25 fps - smooth animation */
#define PHANTOMFPGA_DEFAULT_DESC_COUNT  256
#define PHANTOMFPGA_DEFAULT_IRQ_COUNT   8
#define PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT 40000    /* 40ms = 1 frame at 25fps */
#define PHANTOMFPGA_DEFAULT_FAULT_RATE  1000

/* Limits */
#define PHANTOMFPGA_MIN_FRAME_RATE      1        /* 1 fps */
#define PHANTOMFPGA_MAX_FRAME_RATE      60       /* 60 fps */
#define PHANTOMFPGA_MIN_DESC_COUNT      4
#define PHANTOMFPGA_MAX_DESC_COUNT      4096

/* ------------------------------------------------------------------------ */
/* Register Offsets - Simplified for Animation                              */
/* ------------------------------------------------------------------------ */

/* Identification & Control */
#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID (0xF00DFACE) */
#define PHANTOMFPGA_REG_DEV_VER         0x004   /* R   - Device Version */
#define PHANTOMFPGA_REG_CTRL            0x008   /* R/W - Control Register */
#define PHANTOMFPGA_REG_STATUS          0x00C   /* R   - Status Register */

/* Frame Configuration */
#define PHANTOMFPGA_REG_FRAME_SIZE      0x010   /* R   - Frame size in bytes (5120) */
#define PHANTOMFPGA_REG_FRAME_COUNT     0x014   /* R   - Total frames (250) */
#define PHANTOMFPGA_REG_FRAME_RATE      0x018   /* R/W - Frames per second (1-60) */
#define PHANTOMFPGA_REG_CURRENT_FRAME   0x01C   /* R   - Current frame index */

/* Descriptor Ring Configuration */
#define PHANTOMFPGA_REG_DESC_RING_LO    0x020   /* R/W - Descriptor ring base [31:0] */
#define PHANTOMFPGA_REG_DESC_RING_HI    0x024   /* R/W - Descriptor ring base [63:32] */
#define PHANTOMFPGA_REG_DESC_RING_SIZE  0x028   /* R/W - Number of descriptors */
#define PHANTOMFPGA_REG_DESC_HEAD       0x02C   /* R/W - Head (driver submits) */
#define PHANTOMFPGA_REG_DESC_TAIL       0x030   /* R   - Tail (device completes) */

/* Interrupt Configuration */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x034   /* R/W - IRQ status (W1C) */
#define PHANTOMFPGA_REG_IRQ_MASK        0x038   /* R/W - IRQ enable bits */
#define PHANTOMFPGA_REG_IRQ_COALESCE    0x03C   /* R/W - Coalesce settings */

/* Statistics */
#define PHANTOMFPGA_REG_STAT_FRAMES_TX  0x040   /* R   - Frames transmitted */
#define PHANTOMFPGA_REG_STAT_FRAMES_DROP 0x044  /* R   - Frames dropped (backpressure) */
#define PHANTOMFPGA_REG_STAT_BYTES_LO   0x048   /* R   - Total bytes [31:0] */
#define PHANTOMFPGA_REG_STAT_BYTES_HI   0x04C   /* R   - Total bytes [63:32] */
#define PHANTOMFPGA_REG_STAT_DESC_COMPL 0x050   /* R   - Descriptors completed */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x054   /* R   - Error count */

/* Fault Injection */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x058   /* R/W - Fault injection control */
#define PHANTOMFPGA_REG_FAULT_RATE      0x05C   /* R/W - Fault probability (1/N) */

#define PHANTOMFPGA_REG_MAX             0x060   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL) Bits                                             */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_RUN            (1 << 0)  /* Enable frame transmission */
#define PHANTOMFPGA_CTRL_RESET          (1 << 1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         (1 << 2)  /* Global interrupt enable */

#define PHANTOMFPGA_CTRL_WRITE_MASK     (PHANTOMFPGA_CTRL_RUN | \
                                         PHANTOMFPGA_CTRL_RESET | \
                                         PHANTOMFPGA_CTRL_IRQ_EN)

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS) Bits                                            */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      (1 << 0)  /* Device is transmitting */
#define PHANTOMFPGA_STATUS_DESC_EMPTY   (1 << 1)  /* No descriptors available */
#define PHANTOMFPGA_STATUS_ERROR        (1 << 2)  /* Error condition */

/* ------------------------------------------------------------------------ */
/* IRQ Status/Mask Bits                                                     */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_IRQ_COMPLETE        (1 << 0)  /* Descriptor(s) completed */
#define PHANTOMFPGA_IRQ_ERROR           (1 << 1)  /* Error occurred */
#define PHANTOMFPGA_IRQ_NO_DESC         (1 << 2)  /* No descriptors available */

#define PHANTOMFPGA_IRQ_ALL             (PHANTOMFPGA_IRQ_COMPLETE | \
                                         PHANTOMFPGA_IRQ_ERROR | \
                                         PHANTOMFPGA_IRQ_NO_DESC)

/* MSI-X vector assignments */
#define PHANTOMFPGA_MSIX_VEC_COMPLETE   0
#define PHANTOMFPGA_MSIX_VEC_ERROR      1
#define PHANTOMFPGA_MSIX_VEC_NO_DESC    2

/* ------------------------------------------------------------------------ */
/* IRQ Coalesce Register Layout                                             */
/* ------------------------------------------------------------------------ */

/* [15:0] = count threshold, [31:16] = timeout in microseconds */
#define PHANTOMFPGA_IRQ_COAL_COUNT_MASK     0x0000FFFF
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_SHIFT  16
#define PHANTOMFPGA_IRQ_COAL_TIMEOUT_MASK   0xFFFF0000

/* ------------------------------------------------------------------------ */
/* Fault Injection Bits - Simplified for Frames                             */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_DROP_FRAME        (1 << 0)  /* Drop frames randomly */
#define PHANTOMFPGA_FAULT_CORRUPT_CRC       (1 << 1)  /* Write wrong CRC value */
#define PHANTOMFPGA_FAULT_CORRUPT_DATA      (1 << 2)  /* Flip bits in frame data */
#define PHANTOMFPGA_FAULT_SKIP_SEQUENCE     (1 << 3)  /* Skip sequence numbers */

#define PHANTOMFPGA_FAULT_ALL               (PHANTOMFPGA_FAULT_DROP_FRAME | \
                                             PHANTOMFPGA_FAULT_CORRUPT_CRC | \
                                             PHANTOMFPGA_FAULT_CORRUPT_DATA | \
                                             PHANTOMFPGA_FAULT_SKIP_SEQUENCE)

/* ------------------------------------------------------------------------ */
/* Scatter-Gather Descriptor Structure (32 bytes)                           */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_DESC_CTRL_COMPLETED (1 << 0)  /* Device sets when done */
#define PHANTOMFPGA_DESC_CTRL_EOP       (1 << 1)  /* End of packet */
#define PHANTOMFPGA_DESC_CTRL_SOP       (1 << 2)  /* Start of packet */
#define PHANTOMFPGA_DESC_CTRL_IRQ       (1 << 3)  /* Generate IRQ on completion */
#define PHANTOMFPGA_DESC_CTRL_STOP      (1 << 4)  /* Stop after this descriptor */

typedef struct PhantomFPGASGDesc {
    uint32_t control;       /* Flags: COMPLETED, EOP, SOP, IRQ, STOP */
    uint32_t length;        /* Buffer length in bytes */
    uint64_t dst_addr;      /* Host destination address */
    uint64_t next_desc;     /* Next descriptor address (0 = end of chain) */
    uint64_t reserved;      /* Alignment / future use */
} QEMU_PACKED PhantomFPGASGDesc;

#define PHANTOMFPGA_DESC_SIZE   sizeof(PhantomFPGASGDesc)  /* 32 bytes */

/* ------------------------------------------------------------------------ */
/* Completion Writeback Structure (16 bytes)                                */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_COMPL_STATUS_OK         0
#define PHANTOMFPGA_COMPL_STATUS_DMA_ERROR  1
#define PHANTOMFPGA_COMPL_STATUS_OVERFLOW   2

typedef struct PhantomFPGACompletion {
    uint32_t status;        /* 0 = OK, else error code */
    uint32_t actual_length; /* Bytes actually transferred */
    uint64_t timestamp;     /* Device timestamp */
} QEMU_PACKED PhantomFPGACompletion;

#define PHANTOMFPGA_COMPL_SIZE  sizeof(PhantomFPGACompletion)  /* 16 bytes */

/* ------------------------------------------------------------------------ */
/* Frame Header Structure (16 bytes)                                        */
/* Matches what's in each pre-built frame                                   */
/* ------------------------------------------------------------------------ */

typedef struct PhantomFPGAFrameHeader {
    uint32_t magic;         /* 0xF00DFACE */
    uint32_t sequence;      /* Frame sequence number (0-249, wraps) */
    uint64_t timestamp;     /* Nanoseconds since device start */
} QEMU_PACKED PhantomFPGAFrameHeader;

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

    /* Frame transmission timer */
    QEMUTimer *frame_timer;
    int64_t frame_interval_ns;

    /* Descriptor ring state */
    uint64_t desc_ring_addr;    /* Physical address of descriptor ring */
    uint32_t desc_ring_size;    /* Number of descriptors (power of 2) */
    uint32_t desc_head;         /* Head - driver submits here */
    uint32_t desc_tail;         /* Tail - device completes here */

    /* Frame configuration */
    uint32_t frame_rate;        /* Frames per second */
    uint32_t current_frame;     /* Current frame index (0-249) */
    uint32_t sequence;          /* Sequence number for transmitted frames */

    /* Control and status */
    uint32_t ctrl;              /* Control register value */
    uint32_t status;            /* Status register value */
    uint32_t irq_status;        /* Pending IRQ flags */
    uint32_t irq_mask;          /* IRQ enable mask */
    uint32_t irq_coalesce;      /* Coalesce settings */

    /* IRQ coalescing state */
    uint32_t irq_pending_count; /* Completions since last IRQ */
    int64_t irq_last_time_ns;   /* Last IRQ timestamp */

    /* Statistics counters */
    uint32_t stat_frames_tx;    /* Frames transmitted */
    uint32_t stat_frames_drop;  /* Frames dropped (backpressure) */
    uint64_t stat_bytes;        /* Total bytes transferred */
    uint32_t stat_errors;       /* Total errors */
    uint32_t stat_desc_compl;   /* Descriptors completed */

    /* Fault injection */
    uint32_t fault_inject;      /* Fault injection flags */
    uint32_t fault_rate;        /* Fault probability: ~1/N frames affected */
    uint32_t fault_counter;     /* Internal counter for fault timing */

} PhantomFPGAState;

/* ------------------------------------------------------------------------ */
/* Helper Macros                                                            */
/* ------------------------------------------------------------------------ */

/* Calculate number of available descriptors */
static inline uint32_t phantomfpga_desc_available(PhantomFPGAState *s)
{
    return (s->desc_head - s->desc_tail) & (s->desc_ring_size - 1);
}

/* Check if any descriptors are available */
static inline bool phantomfpga_has_descriptors(PhantomFPGAState *s)
{
    return s->desc_head != s->desc_tail;
}

/* Get descriptor ring address for a given index */
static inline uint64_t phantomfpga_desc_addr(PhantomFPGAState *s, uint32_t idx)
{
    return s->desc_ring_addr + ((uint64_t)idx * PHANTOMFPGA_DESC_SIZE);
}

/* Should a fault be triggered this frame? (probabilistic) */
static inline bool phantomfpga_should_fault(PhantomFPGAState *s)
{
    if (s->fault_rate == 0) {
        return false;
    }
    s->fault_counter++;
    return (s->fault_counter % s->fault_rate) == 0;
}

#endif /* HW_MISC_PHANTOMFPGA_H */
