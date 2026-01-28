/*
 * PhantomFPGA QEMU Device Definitions - v2.0 Scatter-Gather Edition
 *
 * A virtual FPGA device for testing scatter-gather DMA drivers.
 * Now with 100% more descriptors and 200% more CRCs!
 *
 * "Because simple ring buffers are so 2023."
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
#define PHANTOMFPGA_REVISION        0x02     /* Bumped for v2.0 */

/* ------------------------------------------------------------------------ */
/* Device Constants                                                         */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_BAR0_SIZE       4096
#define PHANTOMFPGA_PACKET_MAGIC    0xABCD1234
#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00020000  /* v2.0.0 - SG-DMA edition */
#define PHANTOMFPGA_MSIX_VECTORS    3           /* Complete, error, no_desc */

/* Default values - all sizes in 64-bit words where noted */
#define PHANTOMFPGA_DEFAULT_PKT_SIZE    256     /* 256 * 8 = 2KB packets */
#define PHANTOMFPGA_DEFAULT_PKT_SIZE_MAX 512    /* 512 * 8 = 4KB max */
#define PHANTOMFPGA_DEFAULT_PKT_RATE    1000    /* Hz */
#define PHANTOMFPGA_DEFAULT_DESC_COUNT  256     /* descriptors */
#define PHANTOMFPGA_DEFAULT_IRQ_COUNT   16      /* packets before IRQ */
#define PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT 1000    /* microseconds */
#define PHANTOMFPGA_DEFAULT_FAULT_RATE  1000    /* ~0.1% fault probability */

/* Limits - because users WILL try to break things */
#define PHANTOMFPGA_MIN_PKT_SIZE        8       /* 64 bytes - fits largest header */
#define PHANTOMFPGA_MAX_PKT_SIZE        8192    /* 64KB in 64-bit words */
#define PHANTOMFPGA_MIN_DESC_COUNT      4
#define PHANTOMFPGA_MAX_DESC_COUNT      4096
#define PHANTOMFPGA_MIN_PKT_RATE        1
#define PHANTOMFPGA_MAX_PKT_RATE        100000

/* Header profile sizes in bytes */
#define PHANTOMFPGA_HDR_SIMPLE_SIZE     16
#define PHANTOMFPGA_HDR_STANDARD_SIZE   32
#define PHANTOMFPGA_HDR_FULL_SIZE       64

/* ------------------------------------------------------------------------ */
/* Register Offsets - The New World Order                                   */
/* ------------------------------------------------------------------------ */

/* Identification & Control */
#define PHANTOMFPGA_REG_DEV_ID          0x000   /* R   - Device ID */
#define PHANTOMFPGA_REG_DEV_VER         0x004   /* R   - Device Version */
#define PHANTOMFPGA_REG_CTRL            0x008   /* R/W - Control Register */
#define PHANTOMFPGA_REG_STATUS          0x00C   /* R   - Status Register */

/* Packet Configuration */
#define PHANTOMFPGA_REG_PKT_SIZE_MODE   0x010   /* R/W - Variable size mode */
#define PHANTOMFPGA_REG_PKT_SIZE        0x014   /* R/W - Packet size (64-bit words) */
#define PHANTOMFPGA_REG_PKT_SIZE_MAX    0x018   /* R/W - Max size for variable mode */
#define PHANTOMFPGA_REG_HEADER_PROFILE  0x01C   /* R/W - Header profile select */
#define PHANTOMFPGA_REG_PACKET_RATE     0x020   /* R/W - Packets per second */

/* Descriptor Ring Configuration */
#define PHANTOMFPGA_REG_DESC_RING_LO    0x024   /* R/W - Descriptor ring base [31:0] */
#define PHANTOMFPGA_REG_DESC_RING_HI    0x028   /* R/W - Descriptor ring base [63:32] */
#define PHANTOMFPGA_REG_DESC_RING_SIZE  0x02C   /* R/W - Number of descriptors */
#define PHANTOMFPGA_REG_DESC_HEAD       0x030   /* R/W - Head (driver submits) */
#define PHANTOMFPGA_REG_DESC_TAIL       0x034   /* R   - Tail (device completes) */

/* Interrupt Configuration */
#define PHANTOMFPGA_REG_IRQ_STATUS      0x038   /* R/W - IRQ status (W1C) */
#define PHANTOMFPGA_REG_IRQ_MASK        0x03C   /* R/W - IRQ enable bits */
#define PHANTOMFPGA_REG_IRQ_COALESCE    0x040   /* R/W - Coalesce settings */

/* Statistics */
#define PHANTOMFPGA_REG_STAT_PACKETS    0x044   /* R   - Packets produced */
#define PHANTOMFPGA_REG_STAT_BYTES_LO   0x048   /* R   - Total bytes [31:0] */
#define PHANTOMFPGA_REG_STAT_BYTES_HI   0x04C   /* R   - Total bytes [63:32] */
#define PHANTOMFPGA_REG_STAT_ERRORS     0x050   /* R   - Error count */
#define PHANTOMFPGA_REG_STAT_DESC_COMPL 0x054   /* R   - Descriptors completed */

/* Fault Injection */
#define PHANTOMFPGA_REG_FAULT_INJECT    0x058   /* R/W - Fault injection control */
#define PHANTOMFPGA_REG_FAULT_RATE      0x05C   /* R/W - Fault probability (1/N) */

#define PHANTOMFPGA_REG_MAX             0x060   /* First invalid register offset */

/* ------------------------------------------------------------------------ */
/* Control Register (CTRL) Bits                                             */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_CTRL_RUN            (1 << 0)  /* Enable packet production */
#define PHANTOMFPGA_CTRL_RESET          (1 << 1)  /* Soft reset (self-clearing) */
#define PHANTOMFPGA_CTRL_IRQ_EN         (1 << 2)  /* Global interrupt enable */

#define PHANTOMFPGA_CTRL_WRITE_MASK     (PHANTOMFPGA_CTRL_RUN | \
                                         PHANTOMFPGA_CTRL_RESET | \
                                         PHANTOMFPGA_CTRL_IRQ_EN)

/* ------------------------------------------------------------------------ */
/* Status Register (STATUS) Bits                                            */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_STATUS_RUNNING      (1 << 0)  /* Device is producing packets */
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
/* Packet Size Mode Register                                                */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_PKT_SIZE_FIXED      0    /* Use PKT_SIZE for all packets */
#define PHANTOMFPGA_PKT_SIZE_VARIABLE   1    /* Random size in [PKT_SIZE, PKT_SIZE_MAX] */

/* ------------------------------------------------------------------------ */
/* Header Profiles                                                          */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_HDR_PROFILE_SIMPLE      0   /* 16 bytes - just the basics */
#define PHANTOMFPGA_HDR_PROFILE_STANDARD    1   /* 32 bytes - with CRC */
#define PHANTOMFPGA_HDR_PROFILE_FULL        2   /* 64 bytes - the whole enchilada */

/* ------------------------------------------------------------------------ */
/* Fault Injection Bits - For When Things Are Going Too Well               */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_FAULT_DROP_PACKET       (1 << 0)  /* Drop packets randomly */
#define PHANTOMFPGA_FAULT_CORRUPT_HDR_CRC   (1 << 1)  /* Corrupt header CRC32 */
#define PHANTOMFPGA_FAULT_CORRUPT_PAY_CRC   (1 << 2)  /* Corrupt payload CRC32 */
#define PHANTOMFPGA_FAULT_CORRUPT_PAYLOAD   (1 << 3)  /* Flip bits in payload */
#define PHANTOMFPGA_FAULT_CORRUPT_SEQUENCE  (1 << 4)  /* Skip sequence numbers */
#define PHANTOMFPGA_FAULT_DELAY_IRQ         (1 << 5)  /* Suppress interrupts */

#define PHANTOMFPGA_FAULT_ALL               (PHANTOMFPGA_FAULT_DROP_PACKET | \
                                             PHANTOMFPGA_FAULT_CORRUPT_HDR_CRC | \
                                             PHANTOMFPGA_FAULT_CORRUPT_PAY_CRC | \
                                             PHANTOMFPGA_FAULT_CORRUPT_PAYLOAD | \
                                             PHANTOMFPGA_FAULT_CORRUPT_SEQUENCE | \
                                             PHANTOMFPGA_FAULT_DELAY_IRQ)

/* ------------------------------------------------------------------------ */
/* Scatter-Gather Descriptor Structure (32 bytes)                           */
/*                                                                          */
/* Inspired by Xilinx XDMA because imitation is the sincerest form of       */
/* "we needed something that real drivers understand."                      */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_DESC_CTRL_COMPLETED (1 << 0)  /* Device sets when done */
#define PHANTOMFPGA_DESC_CTRL_EOP       (1 << 1)  /* End of packet */
#define PHANTOMFPGA_DESC_CTRL_SOP       (1 << 2)  /* Start of packet */
#define PHANTOMFPGA_DESC_CTRL_IRQ       (1 << 3)  /* Generate IRQ on completion */
#define PHANTOMFPGA_DESC_CTRL_STOP      (1 << 4)  /* Stop after this descriptor */

/*
 * The descriptor lives in host memory. Device fetches it, uses it,
 * and sets COMPLETED when done. It's like a restaurant ticket system,
 * except the food is data and the kitchen is a virtual FPGA.
 */
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
/*                                                                          */
/* Written at the end of each buffer so the driver knows what happened.     */
/* Because "trust but verify" is good life advice and even better DMA       */
/* design philosophy.                                                       */
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
/* Packet Header Structures                                                 */
/*                                                                          */
/* Three profiles, from "I just want data" to "I want ALL the metadata."    */
/* Pick your poison. Or let your trainees pick theirs.                      */
/* ------------------------------------------------------------------------ */

/* Profile 0: Simple (16 bytes) - For minimalists */
typedef struct PhantomFPGAHdrSimple {
    uint32_t magic;         /* 0xABCD1234 - the universal "hello" */
    uint32_t sequence;      /* Packet sequence number */
    uint32_t size;          /* Total packet size */
    uint32_t reserved;      /* Padding to 16 bytes */
} QEMU_PACKED PhantomFPGAHdrSimple;

/* Profile 1: Standard (32 bytes) - The sensible middle ground */
typedef struct PhantomFPGAHdrStandard {
    uint32_t magic;         /* 0xABCD1234 */
    uint32_t sequence;      /* Packet sequence number */
    uint64_t timestamp;     /* Nanosecond timestamp */
    uint32_t size;          /* Total packet size */
    uint32_t counter;       /* Running counter (device uptime indicator) */
    uint32_t hdr_crc32;     /* CRC32 of bytes [0x00-0x17] */
    uint32_t reserved;      /* Padding to 32 bytes */
} QEMU_PACKED PhantomFPGAHdrStandard;

/* Profile 2: Full (64 bytes) - For those who really like their metadata */
typedef struct PhantomFPGAHdrFull {
    uint32_t magic;         /* 0xABCD1234 */
    uint32_t version;       /* Header version (0x00020000) */
    uint32_t sequence;      /* Packet sequence number */
    uint32_t flags;         /* Packet flags (reserved for now) */
    uint64_t timestamp;     /* Nanosecond timestamp */
    uint64_t mono_counter;  /* 64-bit monotonic counter */
    uint32_t size;          /* Total packet size */
    uint32_t payload_size;  /* Payload size (size - header) */
    uint32_t hdr_crc32;     /* CRC32 of bytes [0x00-0x27] */
    uint32_t payload_crc32; /* CRC32 of payload bytes */
    uint64_t channel;       /* Source channel ID */
    uint64_t reserved;      /* Future use */
} QEMU_PACKED PhantomFPGAHdrFull;

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

    /* Packet production timer */
    QEMUTimer *packet_timer;
    int64_t packet_interval_ns;

    /* Descriptor ring state */
    uint64_t desc_ring_addr;    /* Physical address of descriptor ring */
    uint32_t desc_ring_size;    /* Number of descriptors (power of 2) */
    uint32_t desc_head;         /* Head - driver submits here */
    uint32_t desc_tail;         /* Tail - device completes here */

    /* Packet configuration */
    uint32_t pkt_size_mode;     /* 0 = fixed, 1 = variable */
    uint32_t pkt_size;          /* Size in 64-bit words (or min for variable) */
    uint32_t pkt_size_max;      /* Max size for variable mode */
    uint32_t header_profile;    /* 0 = simple, 1 = standard, 2 = full */
    uint32_t packet_rate;       /* Packets per second */

    /* Packet generation state */
    uint32_t sequence;          /* Next sequence number */
    uint64_t mono_counter;      /* Monotonic counter (never resets) */
    uint32_t prng_state;        /* PRNG state for payload generation */

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
    uint32_t stat_packets;      /* Total packets produced */
    uint64_t stat_bytes;        /* Total bytes transferred */
    uint32_t stat_errors;       /* Total errors */
    uint32_t stat_desc_compl;   /* Descriptors completed */

    /* Fault injection */
    uint32_t fault_inject;      /* Fault injection flags */
    uint32_t fault_rate;        /* Fault probability: ~1/N packets affected */
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

/* Calculate packet size in bytes for current configuration */
static inline uint32_t phantomfpga_get_header_size(PhantomFPGAState *s)
{
    switch (s->header_profile) {
    case PHANTOMFPGA_HDR_PROFILE_SIMPLE:
        return PHANTOMFPGA_HDR_SIMPLE_SIZE;
    case PHANTOMFPGA_HDR_PROFILE_STANDARD:
        return PHANTOMFPGA_HDR_STANDARD_SIZE;
    case PHANTOMFPGA_HDR_PROFILE_FULL:
        return PHANTOMFPGA_HDR_FULL_SIZE;
    default:
        return PHANTOMFPGA_HDR_SIMPLE_SIZE;
    }
}

/* Should a fault be triggered this packet? (probabilistic) */
static inline bool phantomfpga_should_fault(PhantomFPGAState *s)
{
    if (s->fault_rate == 0) {
        return false;
    }
    s->fault_counter++;
    return (s->fault_counter % s->fault_rate) == 0;
}

#endif /* HW_MISC_PHANTOMFPGA_H */
