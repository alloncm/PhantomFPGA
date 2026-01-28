/*
 * PhantomFPGA QEMU PCIe Device Implementation - v2.0 Scatter-Gather Edition
 *
 * A virtual FPGA device for training scatter-gather DMA driver development.
 * Now featuring descriptor rings, CRC32 validation, and enough configuration
 * options to make your head spin (in a good way).
 *
 * "In the land of QEMU where the shadows lie,
 *  one descriptor ring to bind them all."
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

/* Debug macro - uncomment to enable verbose logging */
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
/* CRC32 Implementation (IEEE 802.3 polynomial)                             */
/*                                                                          */
/* Because no self-respecting DMA device would ship without CRC support.    */
/* And yes, we could use a library, but where's the fun in that?            */
/* ------------------------------------------------------------------------ */

static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xee0e612c, 0x990951ba, 0x076dc419, 0x706af48f,
    0xe963a535, 0x9e6495a3, 0x0edb8832, 0x79dcb8a4, 0xe0d5e91e, 0x97d2d988,
    0x09b64c2b, 0x7eb17cbd, 0xe7b82d07, 0x90bf1d91, 0x1db71064, 0x6ab020f2,
    0xf3b97148, 0x84be41de, 0x1adad47d, 0x6ddde4eb, 0xf4d4b551, 0x83d385c7,
    0x136c9856, 0x646ba8c0, 0xfd62f97a, 0x8a65c9ec, 0x14015c4f, 0x63066cd9,
    0xfa0f3d63, 0x8d080df5, 0x3b6e20c8, 0x4c69105e, 0xd56041e4, 0xa2677172,
    0x3c03e4d1, 0x4b04d447, 0xd20d85fd, 0xa50ab56b, 0x35b5a8fa, 0x42b2986c,
    0xdbbbc9d6, 0xacbcf940, 0x32d86ce3, 0x45df5c75, 0xdcd60dcf, 0xabd13d59,
    0x26d930ac, 0x51de003a, 0xc8d75180, 0xbfd06116, 0x21b4f4b5, 0x56b3c423,
    0xcfba9599, 0xb8bda50f, 0x2802b89e, 0x5f058808, 0xc60cd9b2, 0xb10be924,
    0x2f6f7c87, 0x58684c11, 0xc1611dab, 0xb6662d3d, 0x76dc4190, 0x01db7106,
    0x98d220bc, 0xefd5102a, 0x71b18589, 0x06b6b51f, 0x9fbfe4a5, 0xe8b8d433,
    0x7807c9a2, 0x0f00f934, 0x9609a88e, 0xe10e9818, 0x7f6a0dbb, 0x086d3d2d,
    0x91646c97, 0xe6635c01, 0x6b6b51f4, 0x1c6c6162, 0x856530d8, 0xf262004e,
    0x6c0695ed, 0x1b01a57b, 0x8208f4c1, 0xf50fc457, 0x65b0d9c6, 0x12b7e950,
    0x8bbeb8ea, 0xfcb9887c, 0x62dd1ddf, 0x15da2d49, 0x8cd37cf3, 0xfbd44c65,
    0x4db26158, 0x3ab551ce, 0xa3bc0074, 0xd4bb30e2, 0x4adfa541, 0x3dd895d7,
    0xa4d1c46d, 0xd3d6f4fb, 0x4369e96a, 0x346ed9fc, 0xad678846, 0xda60b8d0,
    0x44042d73, 0x33031de5, 0xaa0a4c5f, 0xdd0d7cc9, 0x5005713c, 0x270241aa,
    0xbe0b1010, 0xc90c2086, 0x5768b525, 0x206f85b3, 0xb966d409, 0xce61e49f,
    0x5edef90e, 0x29d9c998, 0xb0d09822, 0xc7d7a8b4, 0x59b33d17, 0x2eb40d81,
    0xb7bd5c3b, 0xc0ba6cad, 0xedb88320, 0x9abfb3b6, 0x03b6e20c, 0x74b1d29a,
    0xead54739, 0x9dd277af, 0x04db2615, 0x73dc1683, 0xe3630b12, 0x94643b84,
    0x0d6d6a3e, 0x7a6a5aa8, 0xe40ecf0b, 0x9309ff9d, 0x0a00ae27, 0x7d079eb1,
    0xf00f9344, 0x8708a3d2, 0x1e01f268, 0x6906c2fe, 0xf762575d, 0x806567cb,
    0x196c3671, 0x6e6b06e7, 0xfed41b76, 0x89d32be0, 0x10da7a5a, 0x67dd4acc,
    0xf9b9df6f, 0x8ebeeff9, 0x17b7be43, 0x60b08ed5, 0xd6d6a3e8, 0xa1d1937e,
    0x38d8c2c4, 0x4fdff252, 0xd1bb67f1, 0xa6bc5767, 0x3fb506dd, 0x48b2364b,
    0xd80d2bda, 0xaf0a1b4c, 0x36034af6, 0x41047a60, 0xdf60efc3, 0xa867df55,
    0x316e8eef, 0x4669be79, 0xcb61b38c, 0xbc66831a, 0x256fd2a0, 0x5268e236,
    0xcc0c7795, 0xbb0b4703, 0x220216b9, 0x5505262f, 0xc5ba3bbe, 0xb2bd0b28,
    0x2bb45a92, 0x5cb36a04, 0xc2d7ffa7, 0xb5d0cf31, 0x2cd99e8b, 0x5bdeae1d,
    0x9b64c2b0, 0xec63f226, 0x756aa39c, 0x026d930a, 0x9c0906a9, 0xeb0e363f,
    0x72076785, 0x05005713, 0x95bf4a82, 0xe2b87a14, 0x7bb12bae, 0x0cb61b38,
    0x92d28e9b, 0xe5d5be0d, 0x7cdcefb7, 0x0bdbdf21, 0x86d3d2d4, 0xf1d4e242,
    0x68ddb3f8, 0x1fda836e, 0x81be16cd, 0xf6b9265b, 0x6fb077e1, 0x18b74777,
    0x88085ae6, 0xff0f6a70, 0x66063bca, 0x11010b5c, 0x8f659eff, 0xf862ae69,
    0x616bffd3, 0x166ccf45, 0xa00ae278, 0xd70dd2ee, 0x4e048354, 0x3903b3c2,
    0xa7672661, 0xd06016f7, 0x4969474d, 0x3e6e77db, 0xaed16a4a, 0xd9d65adc,
    0x40df0b66, 0x37d83bf0, 0xa9bcae53, 0xdebb9ec5, 0x47b2cf7f, 0x30b5ffe9,
    0xbdbdf21c, 0xcabac28a, 0x53b39330, 0x24b4a3a6, 0xbad03605, 0xcdd706b3,
    0x54de5729, 0x23d967bf, 0xb3667a2e, 0xc4614ab8, 0x5d681b02, 0x2a6f2b94,
    0xb40bbe37, 0xc30c8ea1, 0x5a05df1b, 0x2d02ef8d
};

static uint32_t crc32_compute(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;

    for (size_t i = 0; i < len; i++) {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }

    return crc ^ 0xFFFFFFFF;
}

/* ------------------------------------------------------------------------ */
/* Simple PRNG for reproducible pseudo-random payload generation            */
/* ------------------------------------------------------------------------ */

/*
 * Xorshift32 - Fast, decent quality PRNG.
 * We use this for payload and variable size generation.
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

static void phantomfpga_packet_timer_cb(void *opaque);
static void phantomfpga_update_irq(PhantomFPGAState *s);
static void phantomfpga_do_reset(PhantomFPGAState *s);

/* ------------------------------------------------------------------------ */
/* Packet Size Calculation                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Calculate the packet size for the current packet.
 * In fixed mode, returns pkt_size * 8.
 * In variable mode, returns a random size in [pkt_size, pkt_size_max] * 8.
 */
static uint32_t phantomfpga_calc_packet_size(PhantomFPGAState *s)
{
    uint32_t size_words;

    if (s->pkt_size_mode == PHANTOMFPGA_PKT_SIZE_FIXED) {
        size_words = s->pkt_size;
    } else {
        /* Variable mode: random size between min and max */
        uint32_t range = s->pkt_size_max - s->pkt_size + 1;
        uint32_t rand_val = prng_xorshift32(&s->prng_state);
        size_words = s->pkt_size + (rand_val % range);
    }

    /* Convert to bytes (64-bit words -> bytes) */
    return size_words * 8;
}

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

    case PHANTOMFPGA_REG_PKT_SIZE_MODE:
        val = s->pkt_size_mode;
        break;

    case PHANTOMFPGA_REG_PKT_SIZE:
        val = s->pkt_size;
        break;

    case PHANTOMFPGA_REG_PKT_SIZE_MAX:
        val = s->pkt_size_max;
        break;

    case PHANTOMFPGA_REG_HEADER_PROFILE:
        val = s->header_profile;
        break;

    case PHANTOMFPGA_REG_PACKET_RATE:
        val = s->packet_rate;
        break;

    case PHANTOMFPGA_REG_DESC_RING_LO:
        val = (uint32_t)(s->desc_ring_addr & 0xFFFFFFFF);
        break;

    case PHANTOMFPGA_REG_DESC_RING_HI:
        val = (uint32_t)(s->desc_ring_addr >> 32);
        break;

    case PHANTOMFPGA_REG_DESC_RING_SIZE:
        val = s->desc_ring_size;
        break;

    case PHANTOMFPGA_REG_DESC_HEAD:
        val = s->desc_head;
        break;

    case PHANTOMFPGA_REG_DESC_TAIL:
        val = s->desc_tail;
        break;

    case PHANTOMFPGA_REG_IRQ_STATUS:
        val = s->irq_status;
        break;

    case PHANTOMFPGA_REG_IRQ_MASK:
        val = s->irq_mask;
        break;

    case PHANTOMFPGA_REG_IRQ_COALESCE:
        val = s->irq_coalesce;
        break;

    case PHANTOMFPGA_REG_STAT_PACKETS:
        val = s->stat_packets;
        break;

    case PHANTOMFPGA_REG_STAT_BYTES_LO:
        val = (uint32_t)(s->stat_bytes & 0xFFFFFFFF);
        break;

    case PHANTOMFPGA_REG_STAT_BYTES_HI:
        val = (uint32_t)(s->stat_bytes >> 32);
        break;

    case PHANTOMFPGA_REG_STAT_ERRORS:
        val = s->stat_errors;
        break;

    case PHANTOMFPGA_REG_STAT_DESC_COMPL:
        val = s->stat_desc_compl;
        break;

    case PHANTOMFPGA_REG_FAULT_INJECT:
        val = s->fault_inject;
        break;

    case PHANTOMFPGA_REG_FAULT_RATE:
        val = s->fault_rate;
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
    /* Read-only registers - silently ignore writes */
    case PHANTOMFPGA_REG_DEV_ID:
    case PHANTOMFPGA_REG_DEV_VER:
    case PHANTOMFPGA_REG_STATUS:
    case PHANTOMFPGA_REG_DESC_TAIL:
    case PHANTOMFPGA_REG_STAT_PACKETS:
    case PHANTOMFPGA_REG_STAT_BYTES_LO:
    case PHANTOMFPGA_REG_STAT_BYTES_HI:
    case PHANTOMFPGA_REG_STAT_ERRORS:
    case PHANTOMFPGA_REG_STAT_DESC_COMPL:
        DPRINTF("write to read-only register 0x%lx ignored\n",
                (unsigned long)addr);
        break;

    case PHANTOMFPGA_REG_CTRL:
        was_running = (s->ctrl & PHANTOMFPGA_CTRL_RUN) != 0;

        /* Handle reset first (self-clearing) */
        if (val32 & PHANTOMFPGA_CTRL_RESET) {
            DPRINTF("software reset triggered\n");
            phantomfpga_do_reset(s);
            break;  /* Reset clears all other bits */
        }

        /* Apply writable bits only */
        s->ctrl = val32 & PHANTOMFPGA_CTRL_WRITE_MASK;

        /* Handle RUN transition */
        if ((s->ctrl & PHANTOMFPGA_CTRL_RUN) && !was_running) {
            /* Starting packet production */
            DPRINTF("starting packet production at %u Hz\n", s->packet_rate);
            s->status |= PHANTOMFPGA_STATUS_RUNNING;
            s->status &= ~PHANTOMFPGA_STATUS_DESC_EMPTY;

            /* Calculate packet interval and start timer */
            if (s->packet_rate > 0) {
                s->packet_interval_ns = NANOSECONDS_PER_SECOND / s->packet_rate;
            } else {
                s->packet_interval_ns = NANOSECONDS_PER_SECOND;  /* 1 Hz fallback */
            }

            timer_mod(s->packet_timer,
                      qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      s->packet_interval_ns);
        } else if (!(s->ctrl & PHANTOMFPGA_CTRL_RUN) && was_running) {
            /* Stopping packet production */
            DPRINTF("stopping packet production\n");
            timer_del(s->packet_timer);
            s->status &= ~PHANTOMFPGA_STATUS_RUNNING;
        }
        break;

    case PHANTOMFPGA_REG_PKT_SIZE_MODE:
        s->pkt_size_mode = val32 & 0x1;  /* Only bit 0 matters */
        break;

    case PHANTOMFPGA_REG_PKT_SIZE:
        if (val32 < PHANTOMFPGA_MIN_PKT_SIZE) {
            val32 = PHANTOMFPGA_MIN_PKT_SIZE;
            WARN("pkt_size clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_PKT_SIZE) {
            val32 = PHANTOMFPGA_MAX_PKT_SIZE;
            WARN("pkt_size clamped to maximum %u\n", val32);
        }
        s->pkt_size = val32;
        break;

    case PHANTOMFPGA_REG_PKT_SIZE_MAX:
        if (val32 < PHANTOMFPGA_MIN_PKT_SIZE) {
            val32 = PHANTOMFPGA_MIN_PKT_SIZE;
            WARN("pkt_size_max clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_PKT_SIZE) {
            val32 = PHANTOMFPGA_MAX_PKT_SIZE;
            WARN("pkt_size_max clamped to maximum %u\n", val32);
        }
        s->pkt_size_max = val32;
        break;

    case PHANTOMFPGA_REG_HEADER_PROFILE:
        if (val32 > PHANTOMFPGA_HDR_PROFILE_FULL) {
            val32 = PHANTOMFPGA_HDR_PROFILE_SIMPLE;
            WARN("header_profile invalid, defaulting to simple\n");
        }
        s->header_profile = val32;
        break;

    case PHANTOMFPGA_REG_PACKET_RATE:
        if (val32 < PHANTOMFPGA_MIN_PKT_RATE) {
            val32 = PHANTOMFPGA_MIN_PKT_RATE;
            WARN("packet_rate clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_PKT_RATE) {
            val32 = PHANTOMFPGA_MAX_PKT_RATE;
            WARN("packet_rate clamped to maximum %u\n", val32);
        }
        s->packet_rate = val32;

        /* Update timer interval if running */
        if (s->status & PHANTOMFPGA_STATUS_RUNNING) {
            s->packet_interval_ns = NANOSECONDS_PER_SECOND / s->packet_rate;
        }
        break;

    case PHANTOMFPGA_REG_DESC_RING_LO:
        s->desc_ring_addr = (s->desc_ring_addr & 0xFFFFFFFF00000000ULL) | val32;
        DPRINTF("desc_ring base low: 0x%08x (full: 0x%016lx)\n",
                val32, (unsigned long)s->desc_ring_addr);
        break;

    case PHANTOMFPGA_REG_DESC_RING_HI:
        s->desc_ring_addr = (s->desc_ring_addr & 0x00000000FFFFFFFFULL) |
                            ((uint64_t)val32 << 32);
        DPRINTF("desc_ring base high: 0x%08x (full: 0x%016lx)\n",
                val32, (unsigned long)s->desc_ring_addr);
        break;

    case PHANTOMFPGA_REG_DESC_RING_SIZE:
        /* Ring size must be power of 2 for efficient modulo */
        if (val32 < PHANTOMFPGA_MIN_DESC_COUNT) {
            val32 = PHANTOMFPGA_MIN_DESC_COUNT;
            WARN("desc_ring_size clamped to minimum %u\n", val32);
        } else if (val32 > PHANTOMFPGA_MAX_DESC_COUNT) {
            val32 = PHANTOMFPGA_MAX_DESC_COUNT;
            WARN("desc_ring_size clamped to maximum %u\n", val32);
        }
        /* Round down to nearest power of 2 */
        val32 = 1 << (31 - __builtin_clz(val32));
        s->desc_ring_size = val32;
        break;

    case PHANTOMFPGA_REG_DESC_HEAD:
        /* Driver advances head to submit new descriptors */
        if (val32 >= s->desc_ring_size && s->desc_ring_size > 0) {
            WARN("desc_head %u out of range (size=%u)\n",
                 val32, s->desc_ring_size);
            val32 = val32 & (s->desc_ring_size - 1);
        }
        s->desc_head = val32;

        /* Clear DESC_EMPTY status if we now have descriptors */
        if (phantomfpga_has_descriptors(s)) {
            s->status &= ~PHANTOMFPGA_STATUS_DESC_EMPTY;
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

    case PHANTOMFPGA_REG_IRQ_COALESCE:
        s->irq_coalesce = val32;
        break;

    case PHANTOMFPGA_REG_FAULT_INJECT:
        s->fault_inject = val32 & PHANTOMFPGA_FAULT_ALL;
        DPRINTF("fault injection set to 0x%x\n", s->fault_inject);
        break;

    case PHANTOMFPGA_REG_FAULT_RATE:
        s->fault_rate = val32;
        if (val32 == 0) {
            DPRINTF("fault injection disabled (rate=0)\n");
        } else {
            DPRINTF("fault rate set to 1/%u\n", val32);
        }
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

static void phantomfpga_fire_irq(PhantomFPGAState *s, int vector)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);

    /* Check for fault injection: delay IRQ */
    if (s->fault_inject & PHANTOMFPGA_FAULT_DELAY_IRQ) {
        if (phantomfpga_should_fault(s)) {
            DPRINTF("suppressing IRQ vector %d (fault injection)\n", vector);
            return;
        }
    }

    if (s->msix_enabled && msix_enabled(pci_dev)) {
        DPRINTF("firing MSI-X vector %d\n", vector);
        msix_notify(pci_dev, vector);
    }
}

static void phantomfpga_update_irq(PhantomFPGAState *s)
{
    /* Only fire interrupts if globally enabled */
    if (!(s->ctrl & PHANTOMFPGA_CTRL_IRQ_EN)) {
        return;
    }

    /* Check each interrupt condition */
    if ((s->irq_status & PHANTOMFPGA_IRQ_COMPLETE) &&
        (s->irq_mask & PHANTOMFPGA_IRQ_COMPLETE)) {
        phantomfpga_fire_irq(s, PHANTOMFPGA_MSIX_VEC_COMPLETE);
    }

    if ((s->irq_status & PHANTOMFPGA_IRQ_ERROR) &&
        (s->irq_mask & PHANTOMFPGA_IRQ_ERROR)) {
        phantomfpga_fire_irq(s, PHANTOMFPGA_MSIX_VEC_ERROR);
    }

    if ((s->irq_status & PHANTOMFPGA_IRQ_NO_DESC) &&
        (s->irq_mask & PHANTOMFPGA_IRQ_NO_DESC)) {
        phantomfpga_fire_irq(s, PHANTOMFPGA_MSIX_VEC_NO_DESC);
    }
}

/*
 * Check if we should fire an IRQ based on coalescing settings.
 * Returns true if IRQ should be fired, false if we should wait.
 */
static bool phantomfpga_check_irq_coalesce(PhantomFPGAState *s)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t count_threshold = s->irq_coalesce & PHANTOMFPGA_IRQ_COAL_COUNT_MASK;
    uint32_t timeout_us = (s->irq_coalesce >> PHANTOMFPGA_IRQ_COAL_TIMEOUT_SHIFT) &
                          0xFFFF;
    int64_t timeout_ns = timeout_us * 1000LL;

    s->irq_pending_count++;

    /* Fire if count threshold reached */
    if (count_threshold > 0 && s->irq_pending_count >= count_threshold) {
        s->irq_pending_count = 0;
        s->irq_last_time_ns = now_ns;
        return true;
    }

    /* Fire if timeout elapsed since last IRQ */
    if (timeout_ns > 0 && (now_ns - s->irq_last_time_ns) >= timeout_ns) {
        s->irq_pending_count = 0;
        s->irq_last_time_ns = now_ns;
        return true;
    }

    /* If no coalescing configured, fire immediately */
    if (count_threshold == 0 && timeout_us == 0) {
        return true;
    }

    return false;
}

/* ------------------------------------------------------------------------ */
/* Packet Production - The Main Event                                       */
/* ------------------------------------------------------------------------ */

/*
 * Build the packet header based on the selected profile.
 * Returns the header size in bytes.
 */
static uint32_t phantomfpga_build_header(PhantomFPGAState *s,
                                          uint8_t *buf,
                                          uint32_t packet_size,
                                          uint32_t payload_size,
                                          uint32_t payload_crc,
                                          bool corrupt_hdr_crc)
{
    uint64_t timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    uint32_t header_size;
    uint32_t hdr_crc;

    switch (s->header_profile) {
    case PHANTOMFPGA_HDR_PROFILE_SIMPLE:
        {
            PhantomFPGAHdrSimple *hdr = (PhantomFPGAHdrSimple *)buf;
            hdr->magic = PHANTOMFPGA_PACKET_MAGIC;
            hdr->sequence = s->sequence;
            hdr->size = packet_size;
            hdr->reserved = 0;
            header_size = sizeof(PhantomFPGAHdrSimple);
        }
        break;

    case PHANTOMFPGA_HDR_PROFILE_STANDARD:
        {
            PhantomFPGAHdrStandard *hdr = (PhantomFPGAHdrStandard *)buf;
            hdr->magic = PHANTOMFPGA_PACKET_MAGIC;
            hdr->sequence = s->sequence;
            hdr->timestamp = timestamp;
            hdr->size = packet_size;
            hdr->counter = (uint32_t)s->mono_counter;
            hdr->reserved = 0;

            /* Calculate CRC over first 24 bytes (0x00-0x17) */
            hdr_crc = crc32_compute(buf, 24);
            if (corrupt_hdr_crc) {
                hdr_crc ^= 0xDEADBEEF;  /* Corrupt it! */
            }
            hdr->hdr_crc32 = hdr_crc;

            header_size = sizeof(PhantomFPGAHdrStandard);
        }
        break;

    case PHANTOMFPGA_HDR_PROFILE_FULL:
        {
            PhantomFPGAHdrFull *hdr = (PhantomFPGAHdrFull *)buf;
            hdr->magic = PHANTOMFPGA_PACKET_MAGIC;
            hdr->version = PHANTOMFPGA_DEV_VER;
            hdr->sequence = s->sequence;
            hdr->flags = 0;
            hdr->timestamp = timestamp;
            hdr->mono_counter = s->mono_counter;
            hdr->size = packet_size;
            hdr->payload_size = payload_size;
            hdr->channel = 0;
            hdr->reserved = 0;

            /* Calculate CRC over first 40 bytes (0x00-0x27) */
            hdr_crc = crc32_compute(buf, 40);
            if (corrupt_hdr_crc) {
                hdr_crc ^= 0xDEADBEEF;
            }
            hdr->hdr_crc32 = hdr_crc;

            /* Payload CRC (may also be corrupted) */
            hdr->payload_crc32 = payload_crc;

            header_size = sizeof(PhantomFPGAHdrFull);
        }
        break;

    default:
        /* Shouldn't happen, but default to simple */
        {
            PhantomFPGAHdrSimple *hdr = (PhantomFPGAHdrSimple *)buf;
            hdr->magic = PHANTOMFPGA_PACKET_MAGIC;
            hdr->sequence = s->sequence;
            hdr->size = packet_size;
            hdr->reserved = 0;
            header_size = sizeof(PhantomFPGAHdrSimple);
        }
        break;
    }

    return header_size;
}

/*
 * Produce a single packet using the next available descriptor.
 */
static void phantomfpga_produce_packet(PhantomFPGAState *s)
{
    PCIDevice *pci_dev = PCI_DEVICE(s);
    PhantomFPGASGDesc desc;
    PhantomFPGACompletion compl;
    uint64_t desc_addr;
    uint32_t packet_size, header_size, payload_size;
    uint8_t *packet_buf;
    uint8_t *payload_ptr;
    uint32_t payload_crc = 0;
    bool do_fault = phantomfpga_should_fault(s);
    bool corrupt_hdr_crc = false;
    bool corrupt_pay_crc = false;
    bool corrupt_payload = false;
    bool skip_sequence = false;
    int ret;

    /* Check if device is running */
    if (!(s->status & PHANTOMFPGA_STATUS_RUNNING)) {
        return;
    }

    /* Check for fault injection: random packet drop */
    if ((s->fault_inject & PHANTOMFPGA_FAULT_DROP_PACKET) && do_fault) {
        DPRINTF("dropping packet %u (fault injection)\n", s->sequence);
        s->sequence++;  /* Still increment sequence to show gap */
        s->mono_counter++;
        return;
    }

    /* Check for fault injection: corrupt sequence */
    if ((s->fault_inject & PHANTOMFPGA_FAULT_CORRUPT_SEQUENCE) && do_fault) {
        skip_sequence = true;
    }

    /* Check for fault injection: CRC corruption */
    if ((s->fault_inject & PHANTOMFPGA_FAULT_CORRUPT_HDR_CRC) && do_fault) {
        corrupt_hdr_crc = true;
    }
    if ((s->fault_inject & PHANTOMFPGA_FAULT_CORRUPT_PAY_CRC) && do_fault) {
        corrupt_pay_crc = true;
    }
    if ((s->fault_inject & PHANTOMFPGA_FAULT_CORRUPT_PAYLOAD) && do_fault) {
        corrupt_payload = true;
    }

    /* Check if descriptors are available */
    if (!phantomfpga_has_descriptors(s)) {
        s->status |= PHANTOMFPGA_STATUS_DESC_EMPTY;
        s->irq_status |= PHANTOMFPGA_IRQ_NO_DESC;
        DPRINTF("no descriptors available (head=%u tail=%u)\n",
                s->desc_head, s->desc_tail);
        phantomfpga_update_irq(s);
        return;
    }

    /* Validate descriptor ring address */
    if (s->desc_ring_addr == 0) {
        WARN("descriptor ring address not configured\n");
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;
        return;
    }

    /* Fetch descriptor at tail position */
    desc_addr = phantomfpga_desc_addr(s, s->desc_tail);
    ret = pci_dma_read(pci_dev, desc_addr, &desc, sizeof(desc));
    if (ret != 0) {
        WARN("failed to read descriptor at 0x%lx\n", (unsigned long)desc_addr);
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;
        return;
    }

    /* Calculate packet size */
    packet_size = phantomfpga_calc_packet_size(s);
    header_size = phantomfpga_get_header_size(s);

    /* Validate buffer is large enough */
    if (desc.length < packet_size + PHANTOMFPGA_COMPL_SIZE) {
        WARN("descriptor buffer too small: %u < %u + %lu\n",
             desc.length, packet_size, (unsigned long)PHANTOMFPGA_COMPL_SIZE);
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;

        /* Write error completion */
        compl.status = PHANTOMFPGA_COMPL_STATUS_OVERFLOW;
        compl.actual_length = 0;
        compl.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        pci_dma_write(pci_dev, desc.dst_addr + desc.length - PHANTOMFPGA_COMPL_SIZE,
                     &compl, sizeof(compl));
        goto complete_desc;
    }

    /* Allocate temporary packet buffer */
    payload_size = packet_size - header_size;
    packet_buf = g_malloc(packet_size);
    payload_ptr = packet_buf + header_size;

    /* Generate pseudo-random payload using sequence-based seed */
    {
        uint32_t prng = s->sequence ^ 0xDEADBEEF;
        for (uint32_t i = 0; i < payload_size; i += 4) {
            uint32_t rnd = prng_xorshift32(&prng);
            uint32_t remaining = payload_size - i;
            if (remaining >= 4) {
                memcpy(payload_ptr + i, &rnd, 4);
            } else {
                memcpy(payload_ptr + i, &rnd, remaining);
            }
        }
    }

    /* Apply payload corruption if needed */
    if (corrupt_payload && payload_size >= 64) {
        payload_ptr[payload_size / 2] ^= 0xFF;
        payload_ptr[payload_size / 2 + 1] ^= 0xAA;
        DPRINTF("corrupted payload at offset %u (fault injection)\n",
                payload_size / 2);
    }

    /* Calculate payload CRC (for profiles that use it) */
    if (s->header_profile >= PHANTOMFPGA_HDR_PROFILE_FULL) {
        payload_crc = crc32_compute(payload_ptr, payload_size);
        if (corrupt_pay_crc) {
            payload_crc ^= 0xCAFEBABE;
            DPRINTF("corrupted payload CRC (fault injection)\n");
        }
    }

    /* Build header */
    phantomfpga_build_header(s, packet_buf, packet_size, payload_size,
                             payload_crc, corrupt_hdr_crc);

    /* Write packet data via DMA */
    ret = pci_dma_write(pci_dev, desc.dst_addr, packet_buf, packet_size);
    g_free(packet_buf);

    if (ret != 0) {
        WARN("DMA write failed for packet at 0x%lx\n",
             (unsigned long)desc.dst_addr);
        s->stat_errors++;
        s->status |= PHANTOMFPGA_STATUS_ERROR;

        compl.status = PHANTOMFPGA_COMPL_STATUS_DMA_ERROR;
        compl.actual_length = 0;
    } else {
        compl.status = PHANTOMFPGA_COMPL_STATUS_OK;
        compl.actual_length = packet_size;

        DPRINTF("produced packet seq=%u size=%u at addr=0x%lx\n",
                s->sequence, packet_size, (unsigned long)desc.dst_addr);
    }

    /* Write completion structure at end of buffer */
    compl.timestamp = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    pci_dma_write(pci_dev, desc.dst_addr + desc.length - PHANTOMFPGA_COMPL_SIZE,
                 &compl, sizeof(compl));

    /* Update statistics */
    s->stat_packets++;
    s->stat_bytes += packet_size;

complete_desc:
    /* Mark descriptor as completed */
    desc.control |= PHANTOMFPGA_DESC_CTRL_COMPLETED;
    desc.control |= PHANTOMFPGA_DESC_CTRL_SOP | PHANTOMFPGA_DESC_CTRL_EOP;
    pci_dma_write(pci_dev, desc_addr, &desc.control, sizeof(desc.control));

    /* Advance tail index */
    s->desc_tail = (s->desc_tail + 1) & (s->desc_ring_size - 1);
    s->stat_desc_compl++;

    /* Update sequence number */
    if (skip_sequence) {
        s->sequence += 2;  /* Skip one to create a gap */
    } else {
        s->sequence++;
    }
    s->mono_counter++;

    /* Handle interrupt coalescing and delivery */
    if (phantomfpga_check_irq_coalesce(s)) {
        s->irq_status |= PHANTOMFPGA_IRQ_COMPLETE;
        phantomfpga_update_irq(s);
    }

    /* Check for stop flag on descriptor */
    if (desc.control & PHANTOMFPGA_DESC_CTRL_STOP) {
        DPRINTF("stop flag on descriptor, stopping production\n");
        s->ctrl &= ~PHANTOMFPGA_CTRL_RUN;
        s->status &= ~PHANTOMFPGA_STATUS_RUNNING;
        timer_del(s->packet_timer);
    }
}

/* Packet timer callback */
static void phantomfpga_packet_timer_cb(void *opaque)
{
    PhantomFPGAState *s = PHANTOMFPGA(opaque);

    /* Produce a packet */
    phantomfpga_produce_packet(s);

    /* Reschedule timer if still running */
    if (s->status & PHANTOMFPGA_STATUS_RUNNING) {
        timer_mod(s->packet_timer,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  s->packet_interval_ns);
    }
}

/* ------------------------------------------------------------------------ */
/* Device Reset                                                             */
/* ------------------------------------------------------------------------ */

static void phantomfpga_do_reset(PhantomFPGAState *s)
{
    DPRINTF("resetting device state\n");

    /* Stop packet timer */
    timer_del(s->packet_timer);

    /* Reset control and status */
    s->ctrl = 0;
    s->status = 0;
    s->irq_status = 0;
    s->irq_mask = 0;
    s->irq_coalesce = (PHANTOMFPGA_DEFAULT_IRQ_COUNT) |
                      (PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT << 16);

    /* Reset descriptor ring state */
    s->desc_ring_addr = 0;
    s->desc_ring_size = PHANTOMFPGA_DEFAULT_DESC_COUNT;
    s->desc_head = 0;
    s->desc_tail = 0;

    /* Reset packet configuration to defaults */
    s->pkt_size_mode = PHANTOMFPGA_PKT_SIZE_FIXED;
    s->pkt_size = PHANTOMFPGA_DEFAULT_PKT_SIZE;
    s->pkt_size_max = PHANTOMFPGA_DEFAULT_PKT_SIZE_MAX;
    s->header_profile = PHANTOMFPGA_HDR_PROFILE_SIMPLE;
    s->packet_rate = PHANTOMFPGA_DEFAULT_PKT_RATE;
    s->packet_interval_ns = NANOSECONDS_PER_SECOND / s->packet_rate;

    /* Reset packet generation state */
    s->sequence = 0;
    /* Note: mono_counter intentionally NOT reset - it's monotonic! */
    s->prng_state = 0x12345678;

    /* Reset IRQ coalescing state */
    s->irq_pending_count = 0;
    s->irq_last_time_ns = 0;

    /* Reset statistics */
    s->stat_packets = 0;
    s->stat_bytes = 0;
    s->stat_errors = 0;
    s->stat_desc_compl = 0;

    /* Reset fault injection */
    s->fault_inject = 0;
    s->fault_rate = PHANTOMFPGA_DEFAULT_FAULT_RATE;
    s->fault_counter = 0;
}

/* PCI reset handler */
static void phantomfpga_reset(DeviceState *dev)
{
    PhantomFPGAState *s = PHANTOMFPGA(dev);
    phantomfpga_do_reset(s);
    /* Full reset also clears the mono counter */
    s->mono_counter = 0;
}

/* ------------------------------------------------------------------------ */
/* Device Realize/Unrealize                                                 */
/* ------------------------------------------------------------------------ */

static void phantomfpga_realize(PCIDevice *pci_dev, Error **errp)
{
    PhantomFPGAState *s = PHANTOMFPGA(pci_dev);
    int ret;

    DPRINTF("realizing device (v2.0 SG-DMA edition)\n");

    /* Initialize BAR0 for MMIO registers */
    memory_region_init_io(&s->bar0, OBJECT(s), &phantomfpga_mmio_ops, s,
                          "phantomfpga-bar0", PHANTOMFPGA_BAR0_SIZE);
    pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* Initialize MSI-X with 3 vectors
     * BAR0 is used for MSI-X table and PBA */
    ret = msix_init(pci_dev, PHANTOMFPGA_MSIX_VECTORS,
                    &s->bar0, 0, 0x800,  /* Table in BAR0 at offset 0x800 */
                    &s->bar0, 0, 0xC00,  /* PBA in BAR0 at offset 0xC00 */
                    0, errp);
    if (ret < 0) {
        /* MSI-X init failed - device works but without MSI-X */
        WARN("MSI-X initialization failed, continuing without MSI-X\n");
        s->msix_enabled = false;
    } else {
        s->msix_enabled = true;
        DPRINTF("MSI-X initialized with %d vectors\n", PHANTOMFPGA_MSIX_VECTORS);
    }

    /* Create packet production timer */
    s->packet_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                   phantomfpga_packet_timer_cb, s);

    /* Set default values */
    s->pkt_size_mode = PHANTOMFPGA_PKT_SIZE_FIXED;
    s->pkt_size = PHANTOMFPGA_DEFAULT_PKT_SIZE;
    s->pkt_size_max = PHANTOMFPGA_DEFAULT_PKT_SIZE_MAX;
    s->header_profile = PHANTOMFPGA_HDR_PROFILE_SIMPLE;
    s->packet_rate = PHANTOMFPGA_DEFAULT_PKT_RATE;
    s->packet_interval_ns = NANOSECONDS_PER_SECOND / s->packet_rate;
    s->desc_ring_size = PHANTOMFPGA_DEFAULT_DESC_COUNT;
    s->irq_coalesce = (PHANTOMFPGA_DEFAULT_IRQ_COUNT) |
                      (PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT << 16);
    s->fault_rate = PHANTOMFPGA_DEFAULT_FAULT_RATE;
    s->prng_state = 0x12345678;
}

static void phantomfpga_exit(PCIDevice *pci_dev)
{
    PhantomFPGAState *s = PHANTOMFPGA(pci_dev);

    DPRINTF("unrealizing device\n");

    /* Stop and free timer */
    timer_del(s->packet_timer);
    timer_free(s->packet_timer);

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
    .version_id = 2,  /* Bumped for v2.0 */
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        /* Descriptor ring state */
        VMSTATE_UINT64(desc_ring_addr, PhantomFPGAState),
        VMSTATE_UINT32(desc_ring_size, PhantomFPGAState),
        VMSTATE_UINT32(desc_head, PhantomFPGAState),
        VMSTATE_UINT32(desc_tail, PhantomFPGAState),

        /* Packet configuration */
        VMSTATE_UINT32(pkt_size_mode, PhantomFPGAState),
        VMSTATE_UINT32(pkt_size, PhantomFPGAState),
        VMSTATE_UINT32(pkt_size_max, PhantomFPGAState),
        VMSTATE_UINT32(header_profile, PhantomFPGAState),
        VMSTATE_UINT32(packet_rate, PhantomFPGAState),

        /* Packet generation state */
        VMSTATE_UINT32(sequence, PhantomFPGAState),
        VMSTATE_UINT64(mono_counter, PhantomFPGAState),
        VMSTATE_UINT32(prng_state, PhantomFPGAState),
        VMSTATE_INT64(packet_interval_ns, PhantomFPGAState),

        /* Control and status */
        VMSTATE_UINT32(ctrl, PhantomFPGAState),
        VMSTATE_UINT32(status, PhantomFPGAState),
        VMSTATE_UINT32(irq_status, PhantomFPGAState),
        VMSTATE_UINT32(irq_mask, PhantomFPGAState),
        VMSTATE_UINT32(irq_coalesce, PhantomFPGAState),

        /* IRQ coalescing state */
        VMSTATE_UINT32(irq_pending_count, PhantomFPGAState),
        VMSTATE_INT64(irq_last_time_ns, PhantomFPGAState),

        /* Statistics */
        VMSTATE_UINT32(stat_packets, PhantomFPGAState),
        VMSTATE_UINT64(stat_bytes, PhantomFPGAState),
        VMSTATE_UINT32(stat_errors, PhantomFPGAState),
        VMSTATE_UINT32(stat_desc_compl, PhantomFPGAState),

        /* Fault injection */
        VMSTATE_UINT32(fault_inject, PhantomFPGAState),
        VMSTATE_UINT32(fault_rate, PhantomFPGAState),
        VMSTATE_UINT32(fault_counter, PhantomFPGAState),

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

    dc->desc = "PhantomFPGA SG-DMA Training Device v2.0";
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
