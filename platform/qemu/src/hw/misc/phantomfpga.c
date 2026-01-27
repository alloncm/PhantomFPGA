/*
 * PhantomFPGA QEMU PCIe Device Implementation
 *
 * A virtual FPGA device for training DMA ring buffer driver development.
 * Simulates a frame producer that writes data to guest memory via DMA,
 * complete with MSI-X interrupts, configurable frame rates, and fault
 * injection for testing error handling paths.
 *
 * "In the land of QEMU where the shadows lie, one ring buffer to rule them all."
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/timer.h"
#include "sysemu/dma.h"

#include "phantomfpga.h"

/* Debug macro - comment out to disable verbose logging */
/* #define PHANTOMFPGA_DEBUG */

#ifdef PHANTOMFPGA_DEBUG
#define DPRINTF(fmt, ...) \
    qemu_log("phantomfpga: " fmt, ## __VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

/* Warning macro for unexpected conditions */
#define WARN(fmt, ...) \
    qemu_log_mask(LOG_GUEST_ERROR, "phantomfpga: " fmt, ## __VA_ARGS__)

/* ------------------------------------------------------------------------ */
/* Simple PRNG for reproducible pseudo-random payload generation            */
/* ------------------------------------------------------------------------ */

/*
 * Xorshift32 - Fast, decent quality PRNG.
 * Uses the frame sequence number as seed for reproducibility.
 */
static uint32_t prng_xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* ------------------------------------------------------------------------ */
/* Forward Declarations                                                     */
/* ------------------------------------------------------------------------ */

static void phantomfpga_frame_timer_cb(void *opaque);
static void phantomfpga_update_irq(PhantomFPGAState *s);
static void phantomfpga_do_reset(PhantomFPGAState *s);

/* ------------------------------------------------------------------------ */
/* Register Read Handler                                                    */
/* ------------------------------------------------------------------------ */

static uint64_t phantomfpga_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    PhantomFPGAState *s = PHANTOMFPGA(opaque);
    uint64_t val = 0xFFFFFFFF;  /* Default for unknown registers */

    /* Only support 32-bit aligned reads */
    if (size != 4 || (addr & 0x3)) {
        WARN("unaligned read: addr=0x%lx size=%u\n",
             (unsigned long)addr, size);
        return 0xFFFFFFFF;
    }

    switch (addr) {
    case PHANTOMFPGA_REG_DEV_ID:
        val = PHANTOMFPGA_DEV_ID_VAL;
        break;

    case PHANTOMFPGA_REG_DEV_VER:
        val = PHANTOMFPGA_DEV_VER;
        break;

    case PHANTOMFPGA_REG_CTRL:
        val = s->ctrl;
        break;

    case PHANTOMFPGA_REG_STATUS:
        val = s->status;
        break;

    case PHANTOMFPGA_REG_FRAME_SIZE:
        val = s->frame_size;
        break;

    case PHANTOMFPGA_REG_FRAME_RATE:
        val = s->frame_rate;
        break;

    case PHANTOMFPGA_REG_WATERMARK:
        val = s->watermark;
        break;

    case PHANTOMFPGA_REG_RING_SIZE:
        val = s->ring_size;
        break;

    case PHANTOMFPGA_REG_DMA_ADDR_LO:
        val = (uint32_t)(s->dma_addr & 0xFFFFFFFF);
        break;

    case PHANTOMFPGA_REG_DMA_ADDR_HI:
        val = (uint32_t)(s->dma_addr >> 32);
        break;

    case PHANTOMFPGA_REG_DMA_SIZE:
        val = s->dma_size;
        break;

    case PHANTOMFPGA_REG_PROD_IDX:
        val = s->prod_idx;
        break;

    case PHANTOMFPGA_REG_CONS_IDX:
        val = s->cons_idx;
        break;

    case PHANTOMFPGA_REG_IRQ_STATUS:
        val = s->irq_status;
        break;

    case PHANTOMFPGA_REG_IRQ_MASK:
        val = s->irq_mask;
        break;

    case PHANTOMFPGA_REG_STAT_FRAMES:
        val = s->stat_frames;
        break;

    case PHANTOMFPGA_REG_STAT_ERRORS:
        val = s->stat_errors;
        break;

    case PHANTOMFPGA_REG_STAT_OVERRUNS:
        val = s->stat_overruns;
        break;

    case PHANTOMFPGA_REG_FAULT_INJECT:
        val = s->fault_inject;
        break;

    default:
        WARN("read from unknown register 0x%lx\n", (unsigned long)addr);
        val = 0xFFFFFFFF;
        break;
    }

    DPRINTF("read: addr=0x%03lx val=0x%08lx\n",
            (unsigned long)addr, (unsigned long)val);
    return val;
}

/* ------------------------------------------------------------------------ */
/* Register Write Handler                                                   */
/* ------------------------------------------------------------------------ */

static void phantomfpga_mmio_write(void *opaque, hwaddr addr,
                                   uint64_t val, unsigned size)
{
    PhantomFPGAState *s = PHANTOMFPGA(opaque);
    uint32_t val32 = (uint32_t)val;
    bool was_running;

    /* Only support 32-bit aligned writes */
    if (size != 4 || (addr & 0x3)) {
        WARN("unaligned write: addr=0x%lx size=%u val=0x%lx\n",
             (unsigned long)addr, size, (unsigned long)val);
        return;
    }

    DPRINTF("write: addr=0x%03lx val=0x%08x\n",
            (unsigned long)addr, val32);

    switch (addr) {
    case PHANTOMFPGA_REG_DEV_ID:
    case PHANTOMFPGA_REG_DEV_VER:
    case PHANTOMFPGA_REG_STATUS:
    case PHANTOMFPGA_REG_PROD_IDX:
    case PHANTOMFPGA_REG_STAT_FRAMES:
    case PHANTOMFPGA_REG_STAT_ERRORS:
    case PHANTOMFPGA_REG_STAT_OVERRUNS:
        /* Read-only registers - silently ignore writes */
        DPRINTF("write to read-only register 0x%lx ignored\n",
                (unsigned long)addr);
        break;

    case PHANTOMFPGA_REG_CTRL:
        was_running = (s->ctrl & PHANTOMFPGA_CTRL_START) != 0;

        /* Handle reset first (self-clearing) */
        if (val32 & PHANTOMFPGA_CTRL_RESET) {
            DPRINTF("software reset triggered\n");
            phantomfpga_do_reset(s);
            break;  /* Reset clears all other bits */
        }

        /* Apply writable bits only */
        s->ctrl = val32 & PHANTOMFPGA_CTRL_WRITE_MASK;

        /* Handle START transition */
        if ((s->ctrl & PHANTOMFPGA_CTRL_START) && !was_running) {
            /* Starting frame production */
            DPRINTF("starting frame production at %u Hz\n", s->frame_rate);
            s->status |= PHANTOMFPGA_STATUS_RUNNING;
            s->status &= ~PHANTOMFPGA_STATUS_OVERRUN;

            /* Calculate frame interval and start timer */
            if (s->frame_rate > 0) {
                s->frame_interval_ns = NANOSECONDS_PER_SECOND / s->frame_rate;
            } else {
                s->frame_interval_ns = NANOSECONDS_PER_SECOND;  /* 1 Hz fallback */
            }

            timer_mod(s->frame_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->frame_interval_ns);
        } else if (!(s->ctrl & PHANTOMFPGA_CTRL_START) && was_running) {
            /* Stopping frame production */
            DPRINTF("stopping frame production\n");
            timer_del(s->frame_timer);
            s->status &= ~PHANTOMFPGA_STATUS_RUNNING;
        }
        break;

    case PHANTOMFPGA_REG_FRAME_SIZE:
        /* Validate and clamp to allowed range */
        if (val32 < PHANTOMFPGA_MIN_FRAME_SIZE) {
            val32 = PHANTOMFPGA_MIN_FRAME_SIZE;
            WARN("frame_size clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_FRAME_SIZE) {
            val32 = PHANTOMFPGA_MAX_FRAME_SIZE;
            WARN("frame_size clamped to maximum %u\n", val32);
        }
        /* Ensure frame_size is at least large enough for the header */
        if (val32 < sizeof(PhantomFPGAFrameHeader)) {
            val32 = sizeof(PhantomFPGAFrameHeader);
            WARN("frame_size clamped to header size %u\n", val32);
        }
        s->frame_size = val32;
        break;

    case PHANTOMFPGA_REG_FRAME_RATE:
        if (val32 < PHANTOMFPGA_MIN_FRAME_RATE) {
            val32 = PHANTOMFPGA_MIN_FRAME_RATE;
            WARN("frame_rate clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_FRAME_RATE) {
            val32 = PHANTOMFPGA_MAX_FRAME_RATE;
            WARN("frame_rate clamped to maximum %u\n", val32);
        }
        s->frame_rate = val32;

        /* Update timer interval if running */
        if (s->status & PHANTOMFPGA_STATUS_RUNNING) {
            s->frame_interval_ns = NANOSECONDS_PER_SECOND / s->frame_rate;
        }
        break;

    case PHANTOMFPGA_REG_WATERMARK:
        /* Watermark must be less than ring size */
        if (val32 >= s->ring_size) {
            val32 = s->ring_size - 1;
            WARN("watermark clamped to ring_size-1 (%u)\n", val32);
        }
        if (val32 == 0) {
            val32 = 1;
            WARN("watermark clamped to minimum 1\n");
        }
        s->watermark = val32;
        break;

    case PHANTOMFPGA_REG_RING_SIZE:
        /* Ring size must be power of 2 for efficient modulo */
        if (val32 < PHANTOMFPGA_MIN_RING_SIZE) {
            val32 = PHANTOMFPGA_MIN_RING_SIZE;
            WARN("ring_size clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_RING_SIZE) {
            val32 = PHANTOMFPGA_MAX_RING_SIZE;
            WARN("ring_size clamped to maximum %u\n", val32);
        }
        /* Round down to nearest power of 2 */
        val32 = 1 << (31 - __builtin_clz(val32));
        s->ring_size = val32;

        /* Adjust watermark if needed */
        if (s->watermark >= s->ring_size) {
            s->watermark = s->ring_size / 4;
            if (s->watermark == 0) {
                s->watermark = 1;
            }
        }
        break;

    case PHANTOMFPGA_REG_DMA_ADDR_LO:
        s->dma_addr = (s->dma_addr & 0xFFFFFFFF00000000ULL) | val32;
        DPRINTF("DMA base address low: 0x%08x (full: 0x%016lx)\n",
                val32, (unsigned long)s->dma_addr);
        break;

    case PHANTOMFPGA_REG_DMA_ADDR_HI:
        s->dma_addr = (s->dma_addr & 0x00000000FFFFFFFFULL) |
                      ((uint64_t)val32 << 32);
        DPRINTF("DMA base address high: 0x%08x (full: 0x%016lx)\n",
                val32, (unsigned long)s->dma_addr);
        break;

    case PHANTOMFPGA_REG_DMA_SIZE:
        s->dma_size = val32;
        break;

    case PHANTOMFPGA_REG_CONS_IDX:
        /* Driver advances consumer index after processing frames */
        if (val32 >= s->ring_size) {
            WARN("cons_idx %u out of range (ring_size=%u)\n",
                 val32, s->ring_size);
            val32 = val32 & (s->ring_size - 1);
        }
        s->cons_idx = val32;

        /* Clear overrun if buffer is no longer full */
        if (!phantomfpga_ring_full(s)) {
            s->status &= ~PHANTOMFPGA_STATUS_OVERRUN;
        }
        break;

    case PHANTOMFPGA_REG_IRQ_STATUS:
        /* Write-1-to-clear semantics */
        s->irq_status &= ~(val32 & PHANTOMFPGA_IRQ_ALL);
        phantomfpga_update_irq(s);
        break;

    case PHANTOMFPGA_REG_IRQ_MASK:
        s->irq_mask = val32 & PHANTOMFPGA_IRQ_ALL;
        phantomfpga_update_irq(s);
        break;

    case PHANTOMFPGA_REG_FAULT_INJECT:
        s->fault_inject = val32 & PHANTOMFPGA_FAULT_ALL;
        DPRINTF("fault injection set to 0x%x\n", s->fault_inject);
        break;

    default:
        WARN("write to unknown register 0x%lx (val=0x%x)\n",
             (unsigned long)addr, val32);
        break;
    }
}

/* MMIO operations structure */
static const MemoryRegionOps phantomfpga_mmio_ops = {
    .read = phantomfpga_mmio_read,
    .write = phantomfpga_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

/* ------------------------------------------------------------------------ */
/* Interrupt Handling                                                       */
/* ------------------------------------------------------------------------ */

static void phantomfpga_update_irq(PhantomFPGAState *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);

    /* Only fire interrupts if globally enabled */
    if (!(s->ctrl & PHANTOMFPGA_CTRL_IRQ_EN)) {
        return;
    }

    /* Check watermark interrupt */
    if ((s->irq_status & PHANTOMFPGA_IRQ_WATERMARK) &&
        (s->irq_mask & PHANTOMFPGA_IRQ_WATERMARK)) {
        if (s->msix_enabled && msix_enabled(pci_dev)) {
            /* Check for delayed IRQ fault injection */
            if (!(s->fault_inject & PHANTOMFPGA_FAULT_DELAY_IRQ)) {
                DPRINTF("firing MSI-X watermark interrupt\n");
                msix_notify(pci_dev, PHANTOMFPGA_MSIX_VEC_WATERMARK);
            } else {
                DPRINTF("delaying MSI-X watermark interrupt (fault injection)\n");
            }
        }
    }

    /* Check overrun interrupt */
    if ((s->irq_status & PHANTOMFPGA_IRQ_OVERRUN) &&
        (s->irq_mask & PHANTOMFPGA_IRQ_OVERRUN)) {
        if (s->msix_enabled && msix_enabled(pci_dev)) {
            if (!(s->fault_inject & PHANTOMFPGA_FAULT_DELAY_IRQ)) {
                DPRINTF("firing MSI-X overrun interrupt\n");
                msix_notify(pci_dev, PHANTOMFPGA_MSIX_VEC_OVERRUN);
            }
        }
    }
}

/* ------------------------------------------------------------------------ */
/* Frame Production                                                         */
/* ------------------------------------------------------------------------ */

static void phantomfpga_produce_frame(PhantomFPGAState *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);
    PhantomFPGAFrameHeader header;
    uint64_t frame_addr;
    uint32_t payload_len;
    uint32_t prng_state;
    uint8_t *payload;
    uint32_t pending;
    int ret;

    /* Check if device is running */
    if (!(s->status & PHANTOMFPGA_STATUS_RUNNING)) {
        return;
    }

    /* Check for fault injection: random frame drop */
    if (s->fault_inject & PHANTOMFPGA_FAULT_DROP_FRAMES) {
        s->fault_drop_counter++;
        /* Drop roughly 10% of frames using simple modulo check */
        if ((s->fault_drop_counter % 10) == 0) {
            DPRINTF("dropping frame %u (fault injection)\n", s->sequence);
            s->sequence++;  /* Still increment sequence to show gap */
            return;
        }
    }

    /* Check if ring buffer is full */
    if (phantomfpga_ring_full(s)) {
        s->status |= PHANTOMFPGA_STATUS_OVERRUN;
        s->stat_overruns++;
        s->irq_status |= PHANTOMFPGA_IRQ_OVERRUN;
        DPRINTF("ring buffer overrun (prod=%u cons=%u)\n",
                s->prod_idx, s->cons_idx);
        phantomfpga_update_irq(s);
        return;
    }

    /* Validate DMA address */
    if (s->dma_addr == 0) {
        WARN("DMA address not configured, cannot produce frame\n");
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;
        return;
    }

    /* Build frame header */
    header.magic = PHANTOMFPGA_FRAME_MAGIC;
    header.seq = s->sequence;
    header.ts_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    /* Calculate payload length (defensive check for underflow) */
    if (s->frame_size > sizeof(PhantomFPGAFrameHeader)) {
        payload_len = s->frame_size - sizeof(PhantomFPGAFrameHeader);
    } else {
        payload_len = 0;
    }
    header.payload_len = payload_len;
    header.flags = 0;

    /* Calculate frame address in ring buffer */
    frame_addr = phantomfpga_frame_addr(s, s->prod_idx);

    DPRINTF("producing frame seq=%u at addr=0x%lx (slot %u)\n",
            s->sequence, (unsigned long)frame_addr, s->prod_idx);

    /* Write frame header via DMA */
    ret = pci_dma_write(pci_dev, frame_addr, &header, sizeof(header));
    if (ret != 0) {
        WARN("DMA write failed for frame header at 0x%lx\n",
             (unsigned long)frame_addr);
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;
        return;
    }

    /* Generate and write payload */
    if (payload_len > 0) {
        payload = g_malloc(payload_len);

        /* Use sequence number as PRNG seed for reproducibility */
        prng_state = s->sequence ^ 0xDEADBEEF;

        /* Fill payload with pseudo-random data */
        for (uint32_t i = 0; i < payload_len; i += 4) {
            uint32_t rnd = prng_xorshift32(&prng_state);
            uint32_t remaining = payload_len - i;
            if (remaining >= 4) {
                memcpy(payload + i, &rnd, 4);
            } else {
                memcpy(payload + i, &rnd, remaining);
            }
        }

        /* Apply corruption fault injection */
        if (s->fault_inject & PHANTOMFPGA_FAULT_CORRUPT_DATA) {
            /* Flip some bits in the middle of the payload */
            if (payload_len >= 64) {
                payload[payload_len / 2] ^= 0xFF;
                payload[payload_len / 2 + 1] ^= 0xAA;
            }
            /* Update header to indicate corruption (re-write) */
            header.flags |= PHANTOMFPGA_FRAME_FLAG_CORRUPTED;
            pci_dma_write(pci_dev, frame_addr, &header, sizeof(header));
        }

        /* Write payload */
        ret = pci_dma_write(pci_dev, frame_addr + sizeof(header),
                           payload, payload_len);
        g_free(payload);

        if (ret != 0) {
            WARN("DMA write failed for frame payload at 0x%lx\n",
                 (unsigned long)(frame_addr + sizeof(header)));
            s->stat_errors++;
            s->status |= PHANTOMFPGA_STATUS_ERROR;
            return;
        }
    }

    /* Update producer index (wrap around using mask) */
    s->prod_idx = (s->prod_idx + 1) & (s->ring_size - 1);
    s->sequence++;
    s->stat_frames++;

    /* Check watermark threshold */
    pending = phantomfpga_ring_pending(s);
    if (pending >= s->watermark) {
        s->irq_status |= PHANTOMFPGA_IRQ_WATERMARK;
        phantomfpga_update_irq(s);
    }
}

/* Frame timer callback */
static void phantomfpga_frame_timer_cb(void *opaque)
{
    PhantomFPGAState *s = PHANTOMFPGA(opaque);

    /* Produce a frame */
    phantomfpga_produce_frame(s);

    /* Reschedule timer if still running */
    if (s->status & PHANTOMFPGA_STATUS_RUNNING) {
        timer_mod(s->frame_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->frame_interval_ns);
    }
}

/* ------------------------------------------------------------------------ */
/* Device Reset                                                             */
/* ------------------------------------------------------------------------ */

static void phantomfpga_do_reset(PhantomFPGAState *s)
{
    DPRINTF("resetting device state\n");

    /* Stop frame timer */
    timer_del(s->frame_timer);

    /* Reset control and status */
    s->ctrl = 0;
    s->status = 0;
    s->irq_status = 0;
    s->irq_mask = 0;

    /* Reset ring buffer state */
    s->prod_idx = 0;
    s->cons_idx = 0;
    s->sequence = 0;

    /* Reset configuration to defaults */
    s->frame_size = PHANTOMFPGA_DEFAULT_FRAME_SIZE;
    s->frame_rate = PHANTOMFPGA_DEFAULT_FRAME_RATE;
    s->ring_size = PHANTOMFPGA_DEFAULT_RING_SIZE;
    s->watermark = PHANTOMFPGA_DEFAULT_WATERMARK;
    s->frame_interval_ns = NANOSECONDS_PER_SECOND / s->frame_rate;

    /* Clear DMA configuration */
    s->dma_addr = 0;
    s->dma_size = 0;

    /* Reset statistics */
    s->stat_frames = 0;
    s->stat_errors = 0;
    s->stat_overruns = 0;

    /* Clear fault injection */
    s->fault_inject = 0;
    s->fault_drop_counter = 0;
}

/* PCI reset handler */
static void phantomfpga_reset(DeviceState *dev)
{
    PhantomFPGAState *s = PHANTOMFPGA(dev);
    phantomfpga_do_reset(s);
}

/* ------------------------------------------------------------------------ */
/* Device Realize/Unrealize                                                 */
/* ------------------------------------------------------------------------ */

static void phantomfpga_realize(PCIDevice *pci_dev, Error **errp)
{
    PhantomFPGAState *s = PHANTOMFPGA(pci_dev);
    int ret;

    DPRINTF("realizing device\n");

    /* Initialize BAR0 for MMIO registers */
    memory_region_init_io(&s->bar0, OBJECT(s), &phantomfpga_mmio_ops, s,
                          "phantomfpga-bar0", PHANTOMFPGA_BAR0_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* Initialize MSI-X with 2 vectors
     * BAR1 is used for MSI-X table and PBA */
    ret = msix_init(pci_dev, PHANTOMFPGA_MSIX_VECTORS,
                    &s->bar0, 0, 0x800,  /* Table in BAR0 at offset 0x800 */
                    &s->bar0, 0, 0xC00,  /* PBA in BAR0 at offset 0xC00 */
                    0, errp);
    if (ret < 0) {
        /* MSI-X init failed - device works but without MSI-X */
        WARN("MSI-X initialization failed, continuing without MSI-X\n");
        s->msix_enabled = false;
        /* Don't propagate error - device can still work with polling */
    } else {
        s->msix_enabled = true;
        DPRINTF("MSI-X initialized with %d vectors\n", PHANTOMFPGA_MSIX_VECTORS);
    }

    /* Create frame production timer */
    s->frame_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  phantomfpga_frame_timer_cb, s);

    /* Set default values */
    s->frame_size = PHANTOMFPGA_DEFAULT_FRAME_SIZE;
    s->frame_rate = PHANTOMFPGA_DEFAULT_FRAME_RATE;
    s->ring_size = PHANTOMFPGA_DEFAULT_RING_SIZE;
    s->watermark = PHANTOMFPGA_DEFAULT_WATERMARK;
    s->frame_interval_ns = NANOSECONDS_PER_SECOND / s->frame_rate;
}

static void phantomfpga_exit(PCIDevice *pci_dev)
{
    PhantomFPGAState *s = PHANTOMFPGA(pci_dev);

    DPRINTF("unrealizing device\n");

    /* Stop and free timer */
    timer_del(s->frame_timer);
    timer_free(s->frame_timer);

    /* Cleanup MSI-X if initialized */
    if (s->msix_enabled) {
        msix_uninit(pci_dev, &s->bar0, &s->bar0);
    }
}

/* ------------------------------------------------------------------------ */
/* VMState for Migration                                                    */
/* ------------------------------------------------------------------------ */

static const VMStateDescription vmstate_phantomfpga = {
    .name = "phantomfpga",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        /* Ring buffer state */
        VMSTATE_UINT32(prod_idx, PhantomFPGAState),
        VMSTATE_UINT32(cons_idx, PhantomFPGAState),
        VMSTATE_UINT32(ring_size, PhantomFPGAState),
        VMSTATE_UINT32(sequence, PhantomFPGAState),

        /* DMA configuration */
        VMSTATE_UINT64(dma_addr, PhantomFPGAState),
        VMSTATE_UINT32(dma_size, PhantomFPGAState),
        VMSTATE_UINT32(frame_size, PhantomFPGAState),

        /* Frame generation */
        VMSTATE_UINT32(frame_rate, PhantomFPGAState),
        VMSTATE_UINT32(watermark, PhantomFPGAState),
        VMSTATE_INT64(frame_interval_ns, PhantomFPGAState),

        /* Control and status */
        VMSTATE_UINT32(ctrl, PhantomFPGAState),
        VMSTATE_UINT32(status, PhantomFPGAState),
        VMSTATE_UINT32(irq_status, PhantomFPGAState),
        VMSTATE_UINT32(irq_mask, PhantomFPGAState),

        /* Statistics */
        VMSTATE_UINT32(stat_frames, PhantomFPGAState),
        VMSTATE_UINT32(stat_errors, PhantomFPGAState),
        VMSTATE_UINT32(stat_overruns, PhantomFPGAState),

        /* Fault injection */
        VMSTATE_UINT32(fault_inject, PhantomFPGAState),
        VMSTATE_UINT32(fault_drop_counter, PhantomFPGAState),

        VMSTATE_END_OF_LIST()
    }
};

/* ------------------------------------------------------------------------ */
/* Device Class Initialization                                              */
/* ------------------------------------------------------------------------ */

static void phantomfpga_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = phantomfpga_realize;
    k->exit = phantomfpga_exit;
    k->vendor_id = PHANTOMFPGA_VENDOR_ID;
    k->device_id = PHANTOMFPGA_DEVICE_ID;
    k->subsystem_vendor_id = PHANTOMFPGA_SUBSYS_VENDOR;
    k->subsystem_id = PHANTOMFPGA_SUBSYS_ID;
    k->revision = PHANTOMFPGA_REVISION;
    k->class_id = PCI_CLASS_OTHERS;  /* 0xFF0000 */

    dc->desc = "PhantomFPGA DMA Training Device";
    dc->reset = phantomfpga_reset;
    dc->vmsd = &vmstate_phantomfpga;

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo phantomfpga_info = {
    .name = TYPE_PHANTOMFPGA,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(PhantomFPGAState),
    .class_init = phantomfpga_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { }
    },
};

static void phantomfpga_register_types(void)
{
    type_register_static(&phantomfpga_info);
}

type_init(phantomfpga_register_types)
