/*
 * QTest Unit Tests for PhantomFPGA Device v2.0
 *
 * Tests the PhantomFPGA virtual FPGA device's register behavior,
 * scatter-gather DMA configuration, packet generation, and CRC logic.
 *
 * Run these tests to verify the device implementation before
 * letting trainees loose on driver development. Because nothing
 * says "professional training environment" like untested hardware.
 *
 * v2.0 brings scatter-gather descriptors, multiple header profiles,
 * and enough configuration options to confuse everyone equally.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/pci.h"
#include "libqos/pci-pc.h"
#include "libqos/malloc-pc.h"
#include "qemu/module.h"

/* ------------------------------------------------------------------------ */
/* Device Constants (mirrored from phantomfpga.h)                           */
/* The source of truth is the QEMU device, but we need these here too       */
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x1DAD
#define PHANTOMFPGA_DEVICE_ID       0xF00D

#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00020000  /* v2.0.0 - SG-DMA edition */

/* Register offsets - the new hotness */
#define REG_DEV_ID              0x000
#define REG_DEV_VER             0x004
#define REG_CTRL                0x008
#define REG_STATUS              0x00C

#define REG_PKT_SIZE_MODE       0x010
#define REG_PKT_SIZE            0x014
#define REG_PKT_SIZE_MAX        0x018
#define REG_HDR_PROFILE         0x01C
#define REG_PKT_RATE            0x020

#define REG_DESC_RING_LO        0x024
#define REG_DESC_RING_HI        0x028
#define REG_DESC_RING_SIZE      0x02C
#define REG_DESC_HEAD           0x030
#define REG_DESC_TAIL           0x034

#define REG_IRQ_STATUS          0x038
#define REG_IRQ_MASK            0x03C
#define REG_IRQ_COALESCE        0x040

#define REG_STAT_PACKETS        0x044
#define REG_STAT_BYTES_LO       0x048
#define REG_STAT_BYTES_HI       0x04C
#define REG_STAT_ERRORS         0x050
#define REG_STAT_DESC_COMPL     0x054

#define REG_FAULT_INJECT        0x058
#define REG_FAULT_RATE          0x05C

/* Control register bits - same as v1.0, if it ain't broke... */
#define CTRL_RUN                (1 << 0)
#define CTRL_RESET              (1 << 1)
#define CTRL_IRQ_EN             (1 << 2)

/* Status register bits */
#define STATUS_RUNNING          (1 << 0)
#define STATUS_DESC_EMPTY       (1 << 1)
#define STATUS_ERROR            (1 << 2)

/* IRQ bits - now we've got three flavors of interrupts */
#define IRQ_COMPLETE            (1 << 0)
#define IRQ_ERROR               (1 << 1)
#define IRQ_NO_DESC             (1 << 2)
#define IRQ_ALL_BITS            (IRQ_COMPLETE | IRQ_ERROR | IRQ_NO_DESC)

/* Fault injection bits - more ways to break things */
#define FAULT_DROP_PACKET       (1 << 0)
#define FAULT_CORRUPT_HDR_CRC   (1 << 1)
#define FAULT_CORRUPT_PAY_CRC   (1 << 2)
#define FAULT_CORRUPT_PAYLOAD   (1 << 3)
#define FAULT_CORRUPT_SEQ       (1 << 4)
#define FAULT_DELAY_IRQ         (1 << 5)
#define FAULT_ALL_BITS          0x3F

/* Header profile constants */
#define HDR_PROFILE_SIMPLE      0
#define HDR_PROFILE_STANDARD    1
#define HDR_PROFILE_FULL        2
#define HDR_PROFILE_MAX         2

/* Header sizes (bytes) */
#define HDR_SIZE_SIMPLE         16
#define HDR_SIZE_STANDARD       32
#define HDR_SIZE_FULL           64

/* Default values - sane choices for the indecisive */
#define DEFAULT_PKT_SIZE        256     /* 256 * 8 = 2048 bytes */
#define DEFAULT_PKT_SIZE_MAX    512     /* 512 * 8 = 4096 bytes */
#define DEFAULT_PKT_RATE        1000    /* 1 kHz - nice round number */
#define DEFAULT_DESC_RING_SIZE  256     /* Enough for most use cases */
#define DEFAULT_IRQ_COALESCE    ((1000 << 16) | 16)  /* 16 packets or 1ms */
#define DEFAULT_FAULT_RATE      1000    /* ~0.1% fault probability */

/* Limits - because infinite is not a valid configuration */
#define MIN_PKT_SIZE            8       /* 8 * 8 = 64 bytes min */
#define MAX_PKT_SIZE            8192    /* 8192 * 8 = 64KB max */
#define MIN_DESC_RING_SIZE      4       /* At least 4 descriptors */
#define MAX_DESC_RING_SIZE      4096    /* That's a lot of descriptors */
#define MIN_PKT_RATE            1       /* One packet per second */
#define MAX_PKT_RATE            100000  /* 100 kHz should be plenty */

/* Packet magic - unchanged because tradition */
#define PACKET_MAGIC            0xABCD1234

/* ------------------------------------------------------------------------ */
/* Test Fixture                                                             */
/* Same setup, different device                                             */
/* ------------------------------------------------------------------------ */

typedef struct {
    QTestState *qts;
    QPCIBus *pcibus;
    QPCIDevice *dev;
    QPCIBar bar0;
    uint64_t bar0_addr;
} PhantomFPGATestState;

static PhantomFPGATestState *test_state;

/*
 * Read a 32-bit register from BAR0
 * The bread and butter of device testing.
 */
static uint32_t reg_read(PhantomFPGATestState *s, uint32_t offset)
{
    return qpci_io_readl(s->dev, s->bar0, offset);
}

/*
 * Write a 32-bit value to BAR0 register
 * What could possibly go wrong?
 */
static void reg_write(PhantomFPGATestState *s, uint32_t offset, uint32_t val)
{
    qpci_io_writel(s->dev, s->bar0, offset, val);
}

/*
 * Set up the test environment: start QEMU, find device, map BAR0
 *
 * This is the ceremony we must perform before each test.
 * If it fails, we blame QEMU. If it works, we take the credit.
 */
static PhantomFPGATestState *phantomfpga_test_start(void)
{
    PhantomFPGATestState *s;

    s = g_new0(PhantomFPGATestState, 1);

    /* Start QEMU with PhantomFPGA device */
    s->qts = qtest_init("-device phantomfpga");

    /* Get PCI bus */
    s->pcibus = qpci_new_pc(s->qts, NULL);
    g_assert(s->pcibus != NULL);

    /* Find our device - it could be anywhere */
    s->dev = qpci_device_find(s->pcibus, QPCI_DEVFN(0x04, 0));
    if (!s->dev) {
        /* Try different slots - device may be elsewhere */
        for (int slot = 0; slot < 32; slot++) {
            s->dev = qpci_device_find(s->pcibus, QPCI_DEVFN(slot, 0));
            if (s->dev) {
                uint16_t vendor = qpci_config_readw(s->dev, PCI_VENDOR_ID);
                uint16_t device = qpci_config_readw(s->dev, PCI_DEVICE_ID);
                if (vendor == PHANTOMFPGA_VENDOR_ID &&
                    device == PHANTOMFPGA_DEVICE_ID) {
                    break;
                }
                qpci_device_free(s->dev);
                s->dev = NULL;
            }
        }
    }
    g_assert(s->dev != NULL);

    /* Enable device */
    qpci_device_enable(s->dev);

    /* Map BAR0 */
    s->bar0 = qpci_iomap(s->dev, 0, &s->bar0_addr);

    return s;
}

/*
 * Tear down the test environment
 * Clean up after ourselves like responsible adults.
 */
static void phantomfpga_test_stop(PhantomFPGATestState *s)
{
    qpci_iounmap(s->dev, s->bar0);
    qpci_device_free(s->dev);
    qpci_free_pc(s->pcibus);
    qtest_quit(s->qts);
    g_free(s);
}

/* ------------------------------------------------------------------------ */
/* Identification Register Tests                                            */
/* The "are you who you say you are?" tests                                 */
/* ------------------------------------------------------------------------ */

/*
 * Test DEV_ID register returns the magic value 0xF00DFACE
 * This is the first thing any driver should check.
 * If this fails, you've got bigger problems.
 */
static void test_dev_id(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DEV_ID);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_ID_VAL);

    phantomfpga_test_stop(s);
}

/*
 * Test DEV_VER register returns v2.0.0
 * Make sure we're testing the right version of the device.
 */
static void test_dev_ver(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_DEV_VER);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_VER);

    phantomfpga_test_stop(s);
}

/*
 * Test that DEV_ID is read-only (writes are ignored)
 * No, you can't change who the device is.
 */
static void test_dev_id_readonly(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Try to write garbage */
    reg_write(s, REG_DEV_ID, 0xDEADBEEF);

    /* Should still read the magic value */
    val = reg_read(s, REG_DEV_ID);
    g_assert_cmpuint(val, ==, PHANTOMFPGA_DEV_ID_VAL);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Control Register Tests                                                   */
/* The "start, stop, reset" dance                                           */
/* ------------------------------------------------------------------------ */

/*
 * Test CTRL register write and read back
 */
static void test_ctrl_write_read(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially should be 0 */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    /* Write IRQ enable bit */
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_IRQ_EN);

    /* Write RUN bit (device should start) */
    reg_write(s, REG_CTRL, CTRL_RUN | CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_RUN | CTRL_IRQ_EN);

    /* Clear RUN bit */
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_IRQ_EN);

    phantomfpga_test_stop(s);
}

/*
 * Test that RESET bit is self-clearing and resets the device
 * The nuclear option for when things go sideways.
 */
static void test_ctrl_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure some non-default values */
    reg_write(s, REG_PKT_SIZE, 512);
    reg_write(s, REG_PKT_RATE, 500);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* CTRL should be 0 (RESET bit self-clears) */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    /* Configuration should be back to defaults */
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_SIZE);

    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_RATE);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Packet Size Configuration Tests                                          */
/* Because size matters (in bytes, that is)                                 */
/* ------------------------------------------------------------------------ */

/*
 * Test PKT_SIZE register write/read with valid values
 * Sizes are in 64-bit words because that's how FPGAs think.
 */
static void test_pkt_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_SIZE);

    /* Set valid value */
    reg_write(s, REG_PKT_SIZE, 128);
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, 128);

    /* Set minimum valid value */
    reg_write(s, REG_PKT_SIZE, MIN_PKT_SIZE);
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, MIN_PKT_SIZE);

    /* Set maximum valid value */
    reg_write(s, REG_PKT_SIZE, MAX_PKT_SIZE);
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, MAX_PKT_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test PKT_SIZE clamping for out-of-range values
 * The device is smarter than your configuration mistakes.
 */
static void test_pkt_size_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum should be clamped */
    reg_write(s, REG_PKT_SIZE, 1);
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, MIN_PKT_SIZE);

    /* Value above maximum should be clamped */
    reg_write(s, REG_PKT_SIZE, MAX_PKT_SIZE + 1000);
    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, MAX_PKT_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test PKT_SIZE_MODE register (fixed vs variable)
 */
static void test_pkt_size_mode(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default is fixed mode (0) */
    val = reg_read(s, REG_PKT_SIZE_MODE);
    g_assert_cmpuint(val, ==, 0);

    /* Set variable mode */
    reg_write(s, REG_PKT_SIZE_MODE, 1);
    val = reg_read(s, REG_PKT_SIZE_MODE);
    g_assert_cmpuint(val, ==, 1);

    /* Any non-zero value should be treated as 1 */
    reg_write(s, REG_PKT_SIZE_MODE, 0xFF);
    val = reg_read(s, REG_PKT_SIZE_MODE);
    g_assert_cmpuint(val, ==, 1);

    /* Back to fixed */
    reg_write(s, REG_PKT_SIZE_MODE, 0);
    val = reg_read(s, REG_PKT_SIZE_MODE);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test PKT_SIZE_MAX register for variable mode
 */
static void test_pkt_size_max(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_PKT_SIZE_MAX);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_SIZE_MAX);

    /* Set valid value */
    reg_write(s, REG_PKT_SIZE_MAX, 1024);
    val = reg_read(s, REG_PKT_SIZE_MAX);
    g_assert_cmpuint(val, ==, 1024);

    /* Set maximum */
    reg_write(s, REG_PKT_SIZE_MAX, MAX_PKT_SIZE);
    val = reg_read(s, REG_PKT_SIZE_MAX);
    g_assert_cmpuint(val, ==, MAX_PKT_SIZE);

    /* Value above maximum should be clamped */
    reg_write(s, REG_PKT_SIZE_MAX, MAX_PKT_SIZE + 1000);
    val = reg_read(s, REG_PKT_SIZE_MAX);
    g_assert_cmpuint(val, ==, MAX_PKT_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test PKT_RATE register write/read
 */
static void test_pkt_rate(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_RATE);

    /* Set valid value */
    reg_write(s, REG_PKT_RATE, 500);
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, 500);

    /* Minimum */
    reg_write(s, REG_PKT_RATE, MIN_PKT_RATE);
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, MIN_PKT_RATE);

    /* Maximum */
    reg_write(s, REG_PKT_RATE, MAX_PKT_RATE);
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, MAX_PKT_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test PKT_RATE clamping
 */
static void test_pkt_rate_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum */
    reg_write(s, REG_PKT_RATE, 0);
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, MIN_PKT_RATE);

    /* Value above maximum */
    reg_write(s, REG_PKT_RATE, MAX_PKT_RATE + 1000);
    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, MAX_PKT_RATE);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Header Profile Tests                                                     */
/* Pick your level of complexity                                            */
/* ------------------------------------------------------------------------ */

/*
 * Test HDR_PROFILE register
 */
static void test_hdr_profile(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default is Simple (0) */
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_SIMPLE);

    /* Set Standard */
    reg_write(s, REG_HDR_PROFILE, HDR_PROFILE_STANDARD);
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_STANDARD);

    /* Set Full */
    reg_write(s, REG_HDR_PROFILE, HDR_PROFILE_FULL);
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_FULL);

    /* Back to Simple */
    reg_write(s, REG_HDR_PROFILE, HDR_PROFILE_SIMPLE);
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_SIMPLE);

    phantomfpga_test_stop(s);
}

/*
 * Test HDR_PROFILE clamping for invalid values
 */
static void test_hdr_profile_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value above maximum should be clamped */
    reg_write(s, REG_HDR_PROFILE, HDR_PROFILE_MAX + 1);
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_MAX);

    /* Way above maximum */
    reg_write(s, REG_HDR_PROFILE, 0xFF);
    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_MAX);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Descriptor Ring Configuration Tests                                      */
/* The heart of the SG-DMA system                                           */
/* ------------------------------------------------------------------------ */

/*
 * Test descriptor ring address configuration (low and high parts)
 */
static void test_desc_ring_addr(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t lo, hi;

    /* Initially should be 0 */
    lo = reg_read(s, REG_DESC_RING_LO);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(lo, ==, 0);
    g_assert_cmpuint(hi, ==, 0);

    /* Set low part */
    reg_write(s, REG_DESC_RING_LO, 0xDEADBEEF);
    lo = reg_read(s, REG_DESC_RING_LO);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);

    /* Set high part */
    reg_write(s, REG_DESC_RING_HI, 0xCAFEBABE);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    /* Both should be preserved */
    lo = reg_read(s, REG_DESC_RING_LO);
    hi = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    phantomfpga_test_stop(s);
}

/*
 * Test DESC_RING_SIZE register and power-of-2 enforcement
 */
static void test_desc_ring_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_DESC_RING_SIZE);

    /* Set power of 2 */
    reg_write(s, REG_DESC_RING_SIZE, 128);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);

    /* Non-power-of-2 should be rounded down */
    reg_write(s, REG_DESC_RING_SIZE, 100);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 64);  /* Rounded down to 64 */

    reg_write(s, REG_DESC_RING_SIZE, 200);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);  /* Rounded down to 128 */

    phantomfpga_test_stop(s);
}

/*
 * Test DESC_RING_SIZE clamping
 */
static void test_desc_ring_size_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum */
    reg_write(s, REG_DESC_RING_SIZE, 1);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, MIN_DESC_RING_SIZE);

    /* Value above maximum */
    reg_write(s, REG_DESC_RING_SIZE, MAX_DESC_RING_SIZE + 1000);
    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, MAX_DESC_RING_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test DESC_HEAD register (driver writes to submit descriptors)
 */
static void test_desc_head(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 0);

    /* Set a value */
    reg_write(s, REG_DESC_HEAD, 10);
    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 10);

    phantomfpga_test_stop(s);
}

/*
 * Test DESC_HEAD wrapping (value >= ring_size is masked)
 */
static void test_desc_head_wrapping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val, ring_size;

    ring_size = reg_read(s, REG_DESC_RING_SIZE);

    /* Set value >= ring_size (should be masked) */
    reg_write(s, REG_DESC_HEAD, ring_size + 5);
    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 5);  /* Masked to (ring_size + 5) & (ring_size - 1) */

    phantomfpga_test_stop(s);
}

/*
 * Test DESC_TAIL is initially 0 and read-only
 * The device updates this, not you.
 */
static void test_desc_tail(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    /* Try to write (should be ignored - read only) */
    reg_write(s, REG_DESC_TAIL, 42);
    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* IRQ Register Tests                                                       */
/* The "please tell me something happened" tests                            */
/* ------------------------------------------------------------------------ */

/*
 * Test IRQ mask register
 */
static void test_irq_mask(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, 0);

    /* Enable complete IRQ */
    reg_write(s, REG_IRQ_MASK, IRQ_COMPLETE);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_COMPLETE);

    /* Enable all */
    reg_write(s, REG_IRQ_MASK, IRQ_ALL_BITS);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_ALL_BITS);

    /* Only valid bits should be written */
    reg_write(s, REG_IRQ_MASK, 0xFFFFFFFF);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_ALL_BITS);

    phantomfpga_test_stop(s);
}

/*
 * Test IRQ status is initially 0
 */
static void test_irq_status_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test IRQ status write-1-to-clear behavior
 */
static void test_irq_status_w1c(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /*
     * We cannot directly set IRQ status (it's set by hardware),
     * but we can test that write-1-to-clear does nothing when
     * status is already 0 (should stay 0).
     */
    reg_write(s, REG_IRQ_STATUS, IRQ_COMPLETE);
    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test IRQ_COALESCE register
 * Lower 16 bits = packet count threshold
 * Upper 16 bits = timeout in microseconds
 */
static void test_irq_coalesce(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, DEFAULT_IRQ_COALESCE);

    /* Set custom value: 32 packets, 500us timeout */
    reg_write(s, REG_IRQ_COALESCE, (500 << 16) | 32);
    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, (500 << 16) | 32);

    /* Set max values */
    reg_write(s, REG_IRQ_COALESCE, 0xFFFFFFFF);
    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, 0xFFFFFFFF);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Statistics Register Tests                                                */
/* The "how are we doing?" counters                                         */
/* ------------------------------------------------------------------------ */

/*
 * Test statistics registers are initially 0 and read-only
 */
static void test_stats_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* All should be 0 initially */
    val = reg_read(s, REG_STAT_PACKETS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_BYTES_LO);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_BYTES_HI);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test statistics registers are read-only
 */
static void test_stats_readonly(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Try to write (should be ignored) */
    reg_write(s, REG_STAT_PACKETS, 999);
    val = reg_read(s, REG_STAT_PACKETS);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_ERRORS, 999);
    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_DESC_COMPL, 999);
    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Fault Injection Tests                                                    */
/* For when you want to test your error handling                            */
/* ------------------------------------------------------------------------ */

/*
 * Test FAULT_INJECT register
 */
static void test_fault_inject(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

    /* Set drop packets fault */
    reg_write(s, REG_FAULT_INJECT, FAULT_DROP_PACKET);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_DROP_PACKET);

    /* Set CRC corruption faults */
    reg_write(s, REG_FAULT_INJECT, FAULT_CORRUPT_HDR_CRC | FAULT_CORRUPT_PAY_CRC);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_CORRUPT_HDR_CRC | FAULT_CORRUPT_PAY_CRC);

    /* Set all faults */
    reg_write(s, REG_FAULT_INJECT, FAULT_ALL_BITS);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_ALL_BITS);

    /* Only valid bits should be written */
    reg_write(s, REG_FAULT_INJECT, 0xFFFFFFFF);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_ALL_BITS);

    phantomfpga_test_stop(s);
}

/*
 * Test FAULT_RATE register
 * Controls probability: ~1 in N packets affected.
 */
static void test_fault_rate(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FAULT_RATE);

    /* Set aggressive rate (10% of packets) */
    reg_write(s, REG_FAULT_RATE, 10);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 10);

    /* Set conservative rate (0.01% of packets) */
    reg_write(s, REG_FAULT_RATE, 10000);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 10000);

    /* Set to 0 (effectively disables faults) */
    reg_write(s, REG_FAULT_RATE, 0);
    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Status Register Tests                                                    */
/* The "what's the device doing?" indicator                                 */
/* ------------------------------------------------------------------------ */

/*
 * Test STATUS register is initially 0 and read-only
 */
static void test_status_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val, ==, 0);

    /* Try to write (should be ignored) */
    reg_write(s, REG_STATUS, 0xFF);
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test that STATUS shows RUNNING when started
 */
static void test_status_running(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Not running initially */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Should now be running */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, STATUS_RUNNING);

    /* Stop the device */
    reg_write(s, REG_CTRL, 0);

    /* Should no longer be running */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test that STATUS shows DESC_EMPTY when no descriptors submitted
 */
static void test_status_desc_empty(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure ring but don't submit any descriptors */
    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_HI, 0);
    reg_write(s, REG_DESC_RING_SIZE, 256);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Should show DESC_EMPTY since HEAD == TAIL */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_DESC_EMPTY, ==, STATUS_DESC_EMPTY);

    /* Stop the device */
    reg_write(s, REG_CTRL, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Packet Production Tests                                                  */
/* The "does it actually do anything?" tests                                */
/* ------------------------------------------------------------------------ */

/*
 * Test that descriptor tail increments when device is running with descriptors
 * Note: This requires the virtual clock to advance
 */
static void test_packet_production(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t tail_before, tail_after;
    uint32_t stat_packets_before, stat_packets_after;

    /*
     * Configure descriptor ring.
     * Using a fake address since we're just testing index updates.
     */
    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_HI, 0);
    reg_write(s, REG_DESC_RING_SIZE, 256);

    /* Submit some descriptors by advancing HEAD */
    reg_write(s, REG_DESC_HEAD, 100);

    /* Set a low packet rate for predictable timing */
    reg_write(s, REG_PKT_RATE, 10);  /* 10 Hz */

    tail_before = reg_read(s, REG_DESC_TAIL);
    stat_packets_before = reg_read(s, REG_STAT_PACKETS);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Advance the clock by 200ms (should produce ~2 packets at 10 Hz) */
    qtest_clock_step(s->qts, 200 * 1000 * 1000);  /* 200ms in ns */

    tail_after = reg_read(s, REG_DESC_TAIL);
    stat_packets_after = reg_read(s, REG_STAT_PACKETS);

    /* Descriptor tail should have advanced */
    g_assert_cmpuint(tail_after, >, tail_before);

    /* Packet counter should have increased */
    g_assert_cmpuint(stat_packets_after, >, stat_packets_before);

    /* Stop the device */
    reg_write(s, REG_CTRL, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test that tail does not advance without descriptors submitted
 */
static void test_no_packets_without_descriptors(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t tail_before, tail_after;

    /* Configure ring but don't submit descriptors (HEAD stays at 0) */
    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_HI, 0);
    reg_write(s, REG_DESC_RING_SIZE, 256);

    tail_before = reg_read(s, REG_DESC_TAIL);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Advance clock */
    qtest_clock_step(s->qts, 100 * 1000 * 1000);  /* 100ms */

    tail_after = reg_read(s, REG_DESC_TAIL);

    /* Tail should NOT have advanced (no descriptors) */
    g_assert_cmpuint(tail_after, ==, tail_before);

    /* DESC_EMPTY status should be set */
    uint32_t status = reg_read(s, REG_STATUS);
    g_assert_cmpuint(status & STATUS_DESC_EMPTY, ==, STATUS_DESC_EMPTY);

    /* Stop and reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Reset Behavior Tests                                                     */
/* The "big red button" tests                                               */
/* ------------------------------------------------------------------------ */

/*
 * Test full device reset clears all state
 * Sometimes you just want to start over.
 */
static void test_full_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure everything to non-default values */
    reg_write(s, REG_PKT_SIZE, 512);
    reg_write(s, REG_PKT_SIZE_MAX, 1024);
    reg_write(s, REG_PKT_SIZE_MODE, 1);
    reg_write(s, REG_PKT_RATE, 500);
    reg_write(s, REG_HDR_PROFILE, HDR_PROFILE_FULL);
    reg_write(s, REG_DESC_RING_LO, 0xAAAAAAAA);
    reg_write(s, REG_DESC_RING_HI, 0xBBBBBBBB);
    reg_write(s, REG_DESC_RING_SIZE, 512);
    reg_write(s, REG_DESC_HEAD, 50);
    reg_write(s, REG_IRQ_MASK, IRQ_ALL_BITS);
    reg_write(s, REG_IRQ_COALESCE, (2000 << 16) | 64);
    reg_write(s, REG_FAULT_INJECT, FAULT_ALL_BITS);
    reg_write(s, REG_FAULT_RATE, 10);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* Everything should be back to defaults */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_PKT_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_SIZE);

    val = reg_read(s, REG_PKT_SIZE_MAX);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_SIZE_MAX);

    val = reg_read(s, REG_PKT_SIZE_MODE);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_PKT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_PKT_RATE);

    val = reg_read(s, REG_HDR_PROFILE);
    g_assert_cmpuint(val, ==, HDR_PROFILE_SIMPLE);

    val = reg_read(s, REG_DESC_RING_LO);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_RING_HI);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_DESC_RING_SIZE);

    val = reg_read(s, REG_DESC_HEAD);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DESC_TAIL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_COALESCE);
    g_assert_cmpuint(val, ==, DEFAULT_IRQ_COALESCE);

    val = reg_read(s, REG_STAT_PACKETS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_DESC_COMPL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FAULT_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FAULT_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test reset stops running device
 */
static void test_reset_stops_device(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure and start */
    reg_write(s, REG_DESC_RING_LO, 0x10000000);
    reg_write(s, REG_DESC_RING_SIZE, 256);
    reg_write(s, REG_DESC_HEAD, 10);
    reg_write(s, REG_CTRL, CTRL_RUN);

    /* Verify running */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, STATUS_RUNNING);

    /* Reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* Should no longer be running */
    val = reg_read(s, REG_STATUS);
    g_assert_cmpuint(val & STATUS_RUNNING, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* PCI Configuration Tests                                                  */
/* The "are you plugged in correctly?" tests                                */
/* ------------------------------------------------------------------------ */

/*
 * Test PCI vendor and device IDs
 */
static void test_pci_ids(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint16_t vendor, device;

    vendor = qpci_config_readw(s->dev, PCI_VENDOR_ID);
    device = qpci_config_readw(s->dev, PCI_DEVICE_ID);

    g_assert_cmpuint(vendor, ==, PHANTOMFPGA_VENDOR_ID);
    g_assert_cmpuint(device, ==, PHANTOMFPGA_DEVICE_ID);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Test Registration                                                        */
/* All the tests, in one convenient list                                    */
/* ------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);

    /* Identification tests */
    qtest_add_func("/phantomfpga/dev_id", test_dev_id);
    qtest_add_func("/phantomfpga/dev_ver", test_dev_ver);
    qtest_add_func("/phantomfpga/dev_id_readonly", test_dev_id_readonly);
    qtest_add_func("/phantomfpga/pci_ids", test_pci_ids);

    /* Control register tests */
    qtest_add_func("/phantomfpga/ctrl_write_read", test_ctrl_write_read);
    qtest_add_func("/phantomfpga/ctrl_reset", test_ctrl_reset);

    /* Packet size configuration tests */
    qtest_add_func("/phantomfpga/pkt_size", test_pkt_size);
    qtest_add_func("/phantomfpga/pkt_size_clamping", test_pkt_size_clamping);
    qtest_add_func("/phantomfpga/pkt_size_mode", test_pkt_size_mode);
    qtest_add_func("/phantomfpga/pkt_size_max", test_pkt_size_max);
    qtest_add_func("/phantomfpga/pkt_rate", test_pkt_rate);
    qtest_add_func("/phantomfpga/pkt_rate_clamping", test_pkt_rate_clamping);

    /* Header profile tests */
    qtest_add_func("/phantomfpga/hdr_profile", test_hdr_profile);
    qtest_add_func("/phantomfpga/hdr_profile_clamping", test_hdr_profile_clamping);

    /* Descriptor ring configuration tests */
    qtest_add_func("/phantomfpga/desc_ring_addr", test_desc_ring_addr);
    qtest_add_func("/phantomfpga/desc_ring_size", test_desc_ring_size);
    qtest_add_func("/phantomfpga/desc_ring_size_clamping", test_desc_ring_size_clamping);
    qtest_add_func("/phantomfpga/desc_head", test_desc_head);
    qtest_add_func("/phantomfpga/desc_head_wrapping", test_desc_head_wrapping);
    qtest_add_func("/phantomfpga/desc_tail", test_desc_tail);

    /* IRQ tests */
    qtest_add_func("/phantomfpga/irq_mask", test_irq_mask);
    qtest_add_func("/phantomfpga/irq_status_initial", test_irq_status_initial);
    qtest_add_func("/phantomfpga/irq_status_w1c", test_irq_status_w1c);
    qtest_add_func("/phantomfpga/irq_coalesce", test_irq_coalesce);

    /* Statistics tests */
    qtest_add_func("/phantomfpga/stats_initial", test_stats_initial);
    qtest_add_func("/phantomfpga/stats_readonly", test_stats_readonly);

    /* Fault injection tests */
    qtest_add_func("/phantomfpga/fault_inject", test_fault_inject);
    qtest_add_func("/phantomfpga/fault_rate", test_fault_rate);

    /* Status tests */
    qtest_add_func("/phantomfpga/status_initial", test_status_initial);
    qtest_add_func("/phantomfpga/status_running", test_status_running);
    qtest_add_func("/phantomfpga/status_desc_empty", test_status_desc_empty);

    /* Packet production tests */
    qtest_add_func("/phantomfpga/packet_production", test_packet_production);
    qtest_add_func("/phantomfpga/no_packets_without_descriptors",
                   test_no_packets_without_descriptors);

    /* Reset tests */
    qtest_add_func("/phantomfpga/full_reset", test_full_reset);
    qtest_add_func("/phantomfpga/reset_stops_device", test_reset_stops_device);

    return g_test_run();
}
