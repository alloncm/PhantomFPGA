/*
 * QTest Unit Tests for PhantomFPGA Device
 *
 * Tests the PhantomFPGA virtual FPGA device's register behavior,
 * configuration, DMA setup, and frame production logic.
 *
 * Run these tests to verify the device implementation before
 * letting trainees loose on driver development. Because nothing
 * says "professional training environment" like untested hardware.
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
/* ------------------------------------------------------------------------ */

#define PHANTOMFPGA_VENDOR_ID       0x1DAD
#define PHANTOMFPGA_DEVICE_ID       0xF00D

#define PHANTOMFPGA_DEV_ID_VAL      0xF00DFACE
#define PHANTOMFPGA_DEV_VER         0x00010000

/* Register offsets */
#define REG_DEV_ID          0x000
#define REG_DEV_VER         0x004
#define REG_CTRL            0x008
#define REG_STATUS          0x00C
#define REG_FRAME_SIZE      0x010
#define REG_FRAME_RATE      0x014
#define REG_WATERMARK       0x018
#define REG_RING_SIZE       0x01C
#define REG_DMA_ADDR_LO     0x020
#define REG_DMA_ADDR_HI     0x024
#define REG_DMA_SIZE        0x028
#define REG_PROD_IDX        0x02C
#define REG_CONS_IDX        0x030
#define REG_IRQ_STATUS      0x034
#define REG_IRQ_MASK        0x038
#define REG_STAT_FRAMES     0x03C
#define REG_STAT_ERRORS     0x040
#define REG_STAT_OVERRUNS   0x044
#define REG_FAULT_INJECT    0x048

/* Control register bits */
#define CTRL_START          (1 << 0)
#define CTRL_RESET          (1 << 1)
#define CTRL_IRQ_EN         (1 << 2)

/* Status register bits */
#define STATUS_RUNNING      (1 << 0)
#define STATUS_OVERRUN      (1 << 1)
#define STATUS_ERROR        (1 << 2)

/* IRQ bits */
#define IRQ_WATERMARK       (1 << 0)
#define IRQ_OVERRUN         (1 << 1)

/* Fault injection bits */
#define FAULT_DROP_FRAMES   (1 << 0)
#define FAULT_CORRUPT_DATA  (1 << 1)
#define FAULT_DELAY_IRQ     (1 << 2)

/* Default values */
#define DEFAULT_FRAME_SIZE  4096
#define DEFAULT_FRAME_RATE  1000
#define DEFAULT_RING_SIZE   256
#define DEFAULT_WATERMARK   64

/* Limits */
#define MIN_FRAME_SIZE      64
#define MAX_FRAME_SIZE      (64 * 1024)
#define MIN_RING_SIZE       4
#define MAX_RING_SIZE       4096
#define MIN_FRAME_RATE      1
#define MAX_FRAME_RATE      100000

/* Frame magic */
#define FRAME_MAGIC         0xABCD1234

/* ------------------------------------------------------------------------ */
/* Test Fixture                                                             */
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
 */
static uint32_t reg_read(PhantomFPGATestState *s, uint32_t offset)
{
    return qpci_io_readl(s->dev, s->bar0, offset);
}

/*
 * Write a 32-bit value to BAR0 register
 */
static void reg_write(PhantomFPGATestState *s, uint32_t offset, uint32_t val)
{
    qpci_io_writel(s->dev, s->bar0, offset, val);
}

/*
 * Set up the test environment: start QEMU, find device, map BAR0
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

    /* Find our device */
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
/* ------------------------------------------------------------------------ */

/*
 * Test DEV_ID register returns the magic value 0xF00DFACE
 * This is the first thing any driver should check.
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
 * Test DEV_VER register is readable and returns expected version
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

    /* Write START bit (device should start) */
    reg_write(s, REG_CTRL, CTRL_START | CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_START | CTRL_IRQ_EN);

    /* Clear START bit */
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, CTRL_IRQ_EN);

    phantomfpga_test_stop(s);
}

/*
 * Test that RESET bit is self-clearing and resets the device
 */
static void test_ctrl_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure some non-default values */
    reg_write(s, REG_FRAME_SIZE, 8192);
    reg_write(s, REG_FRAME_RATE, 500);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* CTRL should be 0 (RESET bit self-clears) */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    /* Configuration should be back to defaults */
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_SIZE);

    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Configuration Register Tests                                             */
/* ------------------------------------------------------------------------ */

/*
 * Test FRAME_SIZE register write/read with valid values
 */
static void test_frame_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_SIZE);

    /* Set valid value */
    reg_write(s, REG_FRAME_SIZE, 8192);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, 8192);

    /* Set minimum valid value */
    reg_write(s, REG_FRAME_SIZE, MIN_FRAME_SIZE);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, MIN_FRAME_SIZE);

    /* Set maximum valid value */
    reg_write(s, REG_FRAME_SIZE, MAX_FRAME_SIZE);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, MAX_FRAME_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_SIZE clamping for out-of-range values
 */
static void test_frame_size_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum should be clamped */
    reg_write(s, REG_FRAME_SIZE, 1);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, MIN_FRAME_SIZE);

    /* Value above maximum should be clamped */
    reg_write(s, REG_FRAME_SIZE, MAX_FRAME_SIZE + 1000);
    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, MAX_FRAME_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_RATE register write/read
 */
static void test_frame_rate(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    /* Set valid value */
    reg_write(s, REG_FRAME_RATE, 500);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, 500);

    /* Minimum */
    reg_write(s, REG_FRAME_RATE, MIN_FRAME_RATE);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MIN_FRAME_RATE);

    /* Maximum */
    reg_write(s, REG_FRAME_RATE, MAX_FRAME_RATE);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MAX_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test FRAME_RATE clamping
 */
static void test_frame_rate_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum */
    reg_write(s, REG_FRAME_RATE, 0);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MIN_FRAME_RATE);

    /* Value above maximum */
    reg_write(s, REG_FRAME_RATE, MAX_FRAME_RATE + 1000);
    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, MAX_FRAME_RATE);

    phantomfpga_test_stop(s);
}

/*
 * Test RING_SIZE register and power-of-2 rounding
 */
static void test_ring_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_RING_SIZE);

    /* Set power of 2 */
    reg_write(s, REG_RING_SIZE, 128);
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);

    /* Non-power-of-2 should be rounded down */
    reg_write(s, REG_RING_SIZE, 100);
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, 64);  /* Rounded down to 64 */

    reg_write(s, REG_RING_SIZE, 200);
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, 128);  /* Rounded down to 128 */

    phantomfpga_test_stop(s);
}

/*
 * Test RING_SIZE clamping
 */
static void test_ring_size_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Value below minimum */
    reg_write(s, REG_RING_SIZE, 1);
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, MIN_RING_SIZE);

    /* Value above maximum */
    reg_write(s, REG_RING_SIZE, MAX_RING_SIZE + 1000);
    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, MAX_RING_SIZE);

    phantomfpga_test_stop(s);
}

/*
 * Test WATERMARK register
 */
static void test_watermark(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Default value */
    val = reg_read(s, REG_WATERMARK);
    g_assert_cmpuint(val, ==, DEFAULT_WATERMARK);

    /* Set valid value */
    reg_write(s, REG_WATERMARK, 32);
    val = reg_read(s, REG_WATERMARK);
    g_assert_cmpuint(val, ==, 32);

    phantomfpga_test_stop(s);
}

/*
 * Test WATERMARK clamping (must be < ring_size)
 */
static void test_watermark_clamping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val, ring_size;

    /* Get current ring size */
    ring_size = reg_read(s, REG_RING_SIZE);

    /* Try to set watermark >= ring_size */
    reg_write(s, REG_WATERMARK, ring_size);
    val = reg_read(s, REG_WATERMARK);
    g_assert_cmpuint(val, ==, ring_size - 1);

    /* Try to set watermark to 0 */
    reg_write(s, REG_WATERMARK, 0);
    val = reg_read(s, REG_WATERMARK);
    g_assert_cmpuint(val, ==, 1);  /* Minimum is 1 */

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* DMA Configuration Tests                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Test DMA address configuration (low and high parts)
 */
static void test_dma_addr(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t lo, hi;

    /* Initially should be 0 */
    lo = reg_read(s, REG_DMA_ADDR_LO);
    hi = reg_read(s, REG_DMA_ADDR_HI);
    g_assert_cmpuint(lo, ==, 0);
    g_assert_cmpuint(hi, ==, 0);

    /* Set low part */
    reg_write(s, REG_DMA_ADDR_LO, 0xDEADBEEF);
    lo = reg_read(s, REG_DMA_ADDR_LO);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);

    /* Set high part */
    reg_write(s, REG_DMA_ADDR_HI, 0xCAFEBABE);
    hi = reg_read(s, REG_DMA_ADDR_HI);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    /* Both should be preserved */
    lo = reg_read(s, REG_DMA_ADDR_LO);
    hi = reg_read(s, REG_DMA_ADDR_HI);
    g_assert_cmpuint(lo, ==, 0xDEADBEEF);
    g_assert_cmpuint(hi, ==, 0xCAFEBABE);

    phantomfpga_test_stop(s);
}

/*
 * Test DMA size register
 */
static void test_dma_size(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially should be 0 */
    val = reg_read(s, REG_DMA_SIZE);
    g_assert_cmpuint(val, ==, 0);

    /* Set a size */
    reg_write(s, REG_DMA_SIZE, 0x100000);  /* 1 MB */
    val = reg_read(s, REG_DMA_SIZE);
    g_assert_cmpuint(val, ==, 0x100000);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Ring Buffer Index Tests                                                  */
/* ------------------------------------------------------------------------ */

/*
 * Test producer index is initially 0 and read-only
 */
static void test_prod_idx(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_PROD_IDX);
    g_assert_cmpuint(val, ==, 0);

    /* Try to write (should be ignored - read only) */
    reg_write(s, REG_PROD_IDX, 42);
    val = reg_read(s, REG_PROD_IDX);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test consumer index write/read
 */
static void test_cons_idx(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_CONS_IDX);
    g_assert_cmpuint(val, ==, 0);

    /* Set a value */
    reg_write(s, REG_CONS_IDX, 10);
    val = reg_read(s, REG_CONS_IDX);
    g_assert_cmpuint(val, ==, 10);

    phantomfpga_test_stop(s);
}

/*
 * Test consumer index wrapping (value >= ring_size is masked)
 */
static void test_cons_idx_wrapping(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val, ring_size;

    ring_size = reg_read(s, REG_RING_SIZE);

    /* Set value >= ring_size (should be masked) */
    reg_write(s, REG_CONS_IDX, ring_size + 5);
    val = reg_read(s, REG_CONS_IDX);
    g_assert_cmpuint(val, ==, 5);  /* Masked to (ring_size + 5) & (ring_size - 1) */

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* IRQ Register Tests                                                       */
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

    /* Enable watermark IRQ */
    reg_write(s, REG_IRQ_MASK, IRQ_WATERMARK);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_WATERMARK);

    /* Enable both */
    reg_write(s, REG_IRQ_MASK, IRQ_WATERMARK | IRQ_OVERRUN);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_WATERMARK | IRQ_OVERRUN);

    /* Only valid bits should be written */
    reg_write(s, REG_IRQ_MASK, 0xFFFFFFFF);
    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, IRQ_WATERMARK | IRQ_OVERRUN);

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
    reg_write(s, REG_IRQ_STATUS, IRQ_WATERMARK);
    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Statistics Register Tests                                                */
/* ------------------------------------------------------------------------ */

/*
 * Test statistics registers are initially 0 and read-only
 */
static void test_stats_initial(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* All should be 0 initially */
    val = reg_read(s, REG_STAT_FRAMES);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_OVERRUNS);
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
    reg_write(s, REG_STAT_FRAMES, 999);
    val = reg_read(s, REG_STAT_FRAMES);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_ERRORS, 999);
    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    reg_write(s, REG_STAT_OVERRUNS, 999);
    val = reg_read(s, REG_STAT_OVERRUNS);
    g_assert_cmpuint(val, ==, 0);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Fault Injection Tests                                                    */
/* ------------------------------------------------------------------------ */

/*
 * Test fault injection register
 */
static void test_fault_inject(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Initially 0 */
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

    /* Set drop frames fault */
    reg_write(s, REG_FAULT_INJECT, FAULT_DROP_FRAMES);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, FAULT_DROP_FRAMES);

    /* Set all faults */
    reg_write(s, REG_FAULT_INJECT,
              FAULT_DROP_FRAMES | FAULT_CORRUPT_DATA | FAULT_DELAY_IRQ);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==,
                     FAULT_DROP_FRAMES | FAULT_CORRUPT_DATA | FAULT_DELAY_IRQ);

    /* Only valid bits should be written */
    reg_write(s, REG_FAULT_INJECT, 0xFFFFFFFF);
    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==,
                     FAULT_DROP_FRAMES | FAULT_CORRUPT_DATA | FAULT_DELAY_IRQ);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Status Register Tests                                                    */
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
    reg_write(s, REG_CTRL, CTRL_START);

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

/* ------------------------------------------------------------------------ */
/* Frame Production Tests                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Test that producer index increments when device is running
 * Note: This requires the virtual clock to advance
 */
static void test_frame_production(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t prod_idx_before, prod_idx_after;
    uint32_t stat_frames_before, stat_frames_after;

    /*
     * Configure DMA address (required for frame production).
     * Using a fake address since we're just testing index updates.
     * The device needs a valid DMA address or it will set ERROR status.
     */
    reg_write(s, REG_DMA_ADDR_LO, 0x10000000);
    reg_write(s, REG_DMA_ADDR_HI, 0);
    reg_write(s, REG_DMA_SIZE, DEFAULT_RING_SIZE * DEFAULT_FRAME_SIZE);

    /* Set a low frame rate for predictable timing */
    reg_write(s, REG_FRAME_RATE, 10);  /* 10 Hz */

    prod_idx_before = reg_read(s, REG_PROD_IDX);
    stat_frames_before = reg_read(s, REG_STAT_FRAMES);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_START);

    /* Advance the clock by 200ms (should produce ~2 frames at 10 Hz) */
    qtest_clock_step(s->qts, 200 * 1000 * 1000);  /* 200ms in ns */

    prod_idx_after = reg_read(s, REG_PROD_IDX);
    stat_frames_after = reg_read(s, REG_STAT_FRAMES);

    /* Producer index should have advanced */
    g_assert_cmpuint(prod_idx_after, >, prod_idx_before);

    /* Frame counter should have increased */
    g_assert_cmpuint(stat_frames_after, >, stat_frames_before);

    /* Stop the device */
    reg_write(s, REG_CTRL, 0);

    phantomfpga_test_stop(s);
}

/*
 * Test that producer index does not advance without DMA configured
 */
static void test_no_frames_without_dma(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t prod_idx_before, prod_idx_after;

    /* Do NOT configure DMA address */
    prod_idx_before = reg_read(s, REG_PROD_IDX);

    /* Start the device */
    reg_write(s, REG_CTRL, CTRL_START);

    /* Advance clock */
    qtest_clock_step(s->qts, 100 * 1000 * 1000);  /* 100ms */

    prod_idx_after = reg_read(s, REG_PROD_IDX);

    /* Producer index should NOT have advanced (no DMA configured) */
    g_assert_cmpuint(prod_idx_after, ==, prod_idx_before);

    /* Error status might be set */
    /* (We don't strictly require this, but the device should handle it) */

    /* Stop and reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    phantomfpga_test_stop(s);
}

/* ------------------------------------------------------------------------ */
/* Reset Behavior Tests                                                     */
/* ------------------------------------------------------------------------ */

/*
 * Test full device reset clears all state
 */
static void test_full_reset(void)
{
    PhantomFPGATestState *s = phantomfpga_test_start();
    uint32_t val;

    /* Configure everything to non-default values */
    reg_write(s, REG_FRAME_SIZE, 8192);
    reg_write(s, REG_FRAME_RATE, 500);
    reg_write(s, REG_RING_SIZE, 512);
    reg_write(s, REG_WATERMARK, 100);
    reg_write(s, REG_DMA_ADDR_LO, 0xAAAAAAAA);
    reg_write(s, REG_DMA_ADDR_HI, 0xBBBBBBBB);
    reg_write(s, REG_DMA_SIZE, 0x200000);
    reg_write(s, REG_CONS_IDX, 50);
    reg_write(s, REG_IRQ_MASK, IRQ_WATERMARK | IRQ_OVERRUN);
    reg_write(s, REG_FAULT_INJECT, FAULT_DROP_FRAMES);
    reg_write(s, REG_CTRL, CTRL_IRQ_EN);

    /* Trigger reset */
    reg_write(s, REG_CTRL, CTRL_RESET);

    /* Everything should be back to defaults */
    val = reg_read(s, REG_CTRL);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FRAME_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_SIZE);

    val = reg_read(s, REG_FRAME_RATE);
    g_assert_cmpuint(val, ==, DEFAULT_FRAME_RATE);

    val = reg_read(s, REG_RING_SIZE);
    g_assert_cmpuint(val, ==, DEFAULT_RING_SIZE);

    val = reg_read(s, REG_WATERMARK);
    g_assert_cmpuint(val, ==, DEFAULT_WATERMARK);

    val = reg_read(s, REG_DMA_ADDR_LO);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DMA_ADDR_HI);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_DMA_SIZE);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_PROD_IDX);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_CONS_IDX);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_STATUS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_IRQ_MASK);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_FRAMES);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_ERRORS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_STAT_OVERRUNS);
    g_assert_cmpuint(val, ==, 0);

    val = reg_read(s, REG_FAULT_INJECT);
    g_assert_cmpuint(val, ==, 0);

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
    reg_write(s, REG_DMA_ADDR_LO, 0x10000000);
    reg_write(s, REG_DMA_SIZE, 0x100000);
    reg_write(s, REG_CTRL, CTRL_START);

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

    /* Configuration register tests */
    qtest_add_func("/phantomfpga/frame_size", test_frame_size);
    qtest_add_func("/phantomfpga/frame_size_clamping", test_frame_size_clamping);
    qtest_add_func("/phantomfpga/frame_rate", test_frame_rate);
    qtest_add_func("/phantomfpga/frame_rate_clamping", test_frame_rate_clamping);
    qtest_add_func("/phantomfpga/ring_size", test_ring_size);
    qtest_add_func("/phantomfpga/ring_size_clamping", test_ring_size_clamping);
    qtest_add_func("/phantomfpga/watermark", test_watermark);
    qtest_add_func("/phantomfpga/watermark_clamping", test_watermark_clamping);

    /* DMA configuration tests */
    qtest_add_func("/phantomfpga/dma_addr", test_dma_addr);
    qtest_add_func("/phantomfpga/dma_size", test_dma_size);

    /* Ring buffer tests */
    qtest_add_func("/phantomfpga/prod_idx", test_prod_idx);
    qtest_add_func("/phantomfpga/cons_idx", test_cons_idx);
    qtest_add_func("/phantomfpga/cons_idx_wrapping", test_cons_idx_wrapping);

    /* IRQ tests */
    qtest_add_func("/phantomfpga/irq_mask", test_irq_mask);
    qtest_add_func("/phantomfpga/irq_status_initial", test_irq_status_initial);
    qtest_add_func("/phantomfpga/irq_status_w1c", test_irq_status_w1c);

    /* Statistics tests */
    qtest_add_func("/phantomfpga/stats_initial", test_stats_initial);
    qtest_add_func("/phantomfpga/stats_readonly", test_stats_readonly);

    /* Fault injection tests */
    qtest_add_func("/phantomfpga/fault_inject", test_fault_inject);

    /* Status tests */
    qtest_add_func("/phantomfpga/status_initial", test_status_initial);
    qtest_add_func("/phantomfpga/status_running", test_status_running);

    /* Frame production tests */
    qtest_add_func("/phantomfpga/frame_production", test_frame_production);
    qtest_add_func("/phantomfpga/no_frames_without_dma", test_no_frames_without_dma);

    /* Reset tests */
    qtest_add_func("/phantomfpga/full_reset", test_full_reset);
    qtest_add_func("/phantomfpga/reset_stops_device", test_reset_stops_device);

    return g_test_run();
}
