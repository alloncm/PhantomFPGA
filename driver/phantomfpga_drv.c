// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA PCIe Driver v3.0
 *
 * A Linux kernel driver for the PhantomFPGA virtual PCI device.
 * This skeleton provides the structure - you fill in the TODOs!
 *
 * v3.0 streams pre-built data frames via scatter-gather DMA.
 * Build a driver, stream frames over TCP, display them on the host.
 *
 * Learning objectives:
 *   - PCI device probing and BAR mapping
 *   - Scatter-gather DMA with descriptor rings
 *   - Coherent DMA buffer management
 *   - MSI-X interrupt handling with coalescing
 *   - Character device file operations
 *   - IOCTL interface design
 *   - Memory mapping with mmap()
 *   - CRC32 validation
 *
 * Copyright (C) 2026 PhantomFPGA Project
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/pci.h>
#include <linux/interrupt.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/crc32.h>
#include <linux/delay.h>

#include "phantomfpga_regs.h"
#include "phantomfpga_uapi.h"

/* Module metadata */
MODULE_AUTHOR("Me");
MODULE_DESCRIPTION("PhantomFPGA v3.0 Frame Streaming Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("3.0");

/* Driver constants */
#define DRIVER_NAME             "phantomfpga"
#define PHANTOMFPGA_MAX_DEVICES 4

/* Buffer size for frames (frame size + completion writeback) */
#define PHANTOMFPGA_BUFFER_SIZE (PHANTOMFPGA_FRAME_SIZE + PHANTOMFPGA_COMPL_SIZE)

/* ------------------------------------------------------------------------ */
/* Descriptor Buffer Entry                                                  */
/* One of these per descriptor in the ring                                  */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_buffer - Per-descriptor buffer tracking
 *
 * Each descriptor needs a DMA buffer for the device to write frame data.
 * We track both the virtual address (for driver/userspace) and DMA address
 * (for the device).
 */
struct phantomfpga_buffer {
	void *vaddr;              /* Kernel virtual address */
	dma_addr_t dma_addr;      /* DMA address for device */
	size_t size;              /* Buffer size in bytes */
};

/* ------------------------------------------------------------------------ */
/* Device Private Data Structure                                            */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_dev - Per-device private data
 *
 * This structure holds all state for a single PhantomFPGA device.
 * One instance is allocated per PCI device in probe().
 *
 * v3.0 changes: Simplified for fixed-size frame streaming.
 * No more variable packet sizes or header profiles.
 */
struct phantomfpga_dev {
	/* PCI device reference */
	struct pci_dev *pdev;

	/* BAR0 register mapping */
	void __iomem *regs;         /* Kernel virtual address of BAR0 */
	resource_size_t regs_start; /* Physical address of BAR0 */
	resource_size_t regs_len;   /* Length of BAR0 region */

	/* Descriptor ring (SG-DMA) */
	struct phantomfpga_sg_desc *desc_ring;  /* Descriptor ring virtual addr */
	dma_addr_t desc_ring_dma;               /* Descriptor ring DMA addr */
	u32 desc_count;                         /* Number of descriptors */

	/* Per-descriptor buffers */
	struct phantomfpga_buffer *buffers;     /* Array of buffer tracking */
	size_t buffer_size;                     /* Size of each buffer */

	/* Configuration */
	u32 frame_rate;             /* Frames per second (1-60) */
	u16 irq_coalesce_count;     /* IRQ after N completions */
	u16 irq_coalesce_timeout;   /* IRQ timeout in microseconds */
	bool configured;            /* Has SET_CFG been called? */
	bool streaming;             /* Is device currently streaming? */

	/* Ring indices (driver-side shadow) */
	u32 desc_head;              /* Head: driver writes to submit */
	u32 desc_tail;              /* Tail: device updates on completion */
	u32 shadow_tail;            /* Completion pointer (set by IRQ handler) */
	u32 consumer;               /* Consumer pointer (advanced by read/ioctl) */

	/* Statistics (driver-side) */
	u64 frames_consumed;
	u64 bytes_consumed;
	u32 irq_count;
	u32 crc_errors;

	/* Synchronization */
	spinlock_t lock;            /* Protects indices and state */
	struct mutex ioctl_lock;    /* Serializes ioctl operations */
	wait_queue_head_t wait_queue; /* For poll/blocking read */

	/* Character device */
	struct cdev cdev;
	dev_t devno;
	struct device *dev;         /* sysfs device */
	int minor;

	/* MSI-X vectors */
	int num_vectors;
	int irq_complete;           /* IRQ number for completion vector */
	int irq_error;              /* IRQ number for error vector */
	int irq_no_desc;            /* IRQ number for no-descriptor vector */
};

/* ------------------------------------------------------------------------ */
/* Global Driver State                                                      */
/* ------------------------------------------------------------------------ */

static struct class *phantomfpga_class;
static dev_t phantomfpga_devno;
static DEFINE_IDA(phantomfpga_ida);  /* Minor number allocator */

/* PCI device ID table */
static const struct pci_device_id phantomfpga_pci_ids[] = {
	{ PCI_DEVICE(PHANTOMFPGA_VENDOR_ID, PHANTOMFPGA_DEVICE_ID) },
	{ 0, }  /* Terminator */
};
MODULE_DEVICE_TABLE(pci, phantomfpga_pci_ids);

/* ------------------------------------------------------------------------ */
/* Register Access Helpers                                                  */
/* These helpers provide type-safe register access                          */
/* ------------------------------------------------------------------------ */

static inline u32 pfpga_read32(struct phantomfpga_dev *pfdev, u32 offset)
{
	return ioread32(pfdev->regs + offset);
}

static inline void pfpga_write32(struct phantomfpga_dev *pfdev, u32 offset, u32 val)
{
	iowrite32(val, pfdev->regs + offset);
}

/* ------------------------------------------------------------------------ */
/* CRC32 Verification                                                       */
/* Trust but verify - validate frame CRCs                                   */
/* ------------------------------------------------------------------------ */

/*
 * Compute CRC32 for a data buffer.
 *
 * Uses the Linux kernel's crc32_le() function with the IEEE 802.3 polynomial.
 * The device uses the same polynomial, so results should match.
 */
static inline u32 pfpga_compute_crc32(const void *data, size_t len)
{
	return crc32_le(~0, data, len) ^ ~0;
}

/*
 * Validate CRC32 of a received frame.
 * Returns true if CRC matches, false otherwise.
 */
static inline bool pfpga_validate_frame_crc(const void *frame)
{
	u32 computed_crc = pfpga_compute_crc32(frame, PHANTOMFPGA_FRAME_CRC_OFFSET);
	u32 stored_crc = le32_to_cpu(*phantomfpga_frame_crc_ptr((void *)frame));
	return computed_crc == stored_crc;
}

/* ------------------------------------------------------------------------ */
/* Hardware Operations - SG-DMA for Frame Streaming                         */
/* ------------------------------------------------------------------------ */

/*
 * Configure the descriptor ring address in the device.
 * Called after descriptor ring allocation.
 */
static void __maybe_unused pfpga_configure_desc_ring(struct phantomfpga_dev *pfdev)
{
	wmb();
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_LO, lower_32_bits(pfdev->desc_ring_dma));
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_HI, upper_32_bits(pfdev->desc_ring_dma));
}

/*
 * Apply frame streaming configuration to device registers.
 * Called from SET_CFG ioctl after validation.
 */
static void __maybe_unused pfpga_apply_config(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Write configuration to device registers
	 *
	 * Steps:
	 *   1. Write pfdev->frame_rate to PHANTOMFPGA_REG_FRAME_RATE
	 *   2. Write IRQ coalesce settings to PHANTOMFPGA_REG_IRQ_COALESCE:
	 *      Use phantomfpga_irq_coalesce_pack(count, timeout)
	 *   3. Enable all interrupts in PHANTOMFPGA_REG_IRQ_MASK:
	 *      PHANTOMFPGA_IRQ_ALL
	 *
	 * Note: Frame size is fixed at 5120 bytes, no configuration needed.
	 */
}

/*
 * Submit descriptors to the device.
 *
 * After populating descriptors with buffer addresses, write the new
 * head index to tell the device how many are available.
 */
static void __maybe_unused pfpga_submit_descriptors(struct phantomfpga_dev *pfdev, u32 count)
{
	// Ensures all the descriptors are up to date
	wmb();

	pfdev->desc_head = (pfdev->desc_head + count) & (pfdev->desc_count - 1);
	pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_HEAD, pfdev->desc_head);
}

/*
 * Initialize all descriptors with buffer addresses.
 * Called once after buffer allocation.
 */
static void __maybe_unused pfpga_init_descriptors(struct phantomfpga_dev *pfdev)
{
	for (int i = 0; i < pfdev->desc_count - 1; i++) {
		struct phantomfpga_sg_desc *desc = pfdev->desc_ring + i;
		struct phantomfpga_buffer *buffer = pfdev->buffers + i;

		desc->dst_addr = buffer->dma_addr;
		desc->length = buffer->size;
		desc->control = 0;
		desc->next_desc = 0;
		desc->reserved = 0;
	}

	pfdev->desc_head = pfdev->desc_count - 1;
	pfdev->desc_tail = 0; 

	pfpga_submit_descriptors(pfdev, pfdev->desc_count - 1);
}

/*
 * Start frame streaming.
 */
static int pfpga_start_streaming(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Start the device streaming
	 *
	 * Steps:
	 *   1. Check pfdev->configured - return -EINVAL if not configured
	 *   2. Check pfdev->streaming - return -EBUSY if already streaming
	 *   3. Reset indices: desc_head = desc_tail = shadow_tail = consumer = 0
	 *   4. Write 0 to PHANTOMFPGA_REG_DESC_HEAD and DESC_TAIL
	 *   5. Re-initialize descriptors (clear COMPLETED flags)
	 *   6. Submit all available descriptors
	 *   7. Clear any pending IRQs: write PHANTOMFPGA_IRQ_ALL to IRQ_STATUS
	 *   8. Write CTRL register:
	 *      PHANTOMFPGA_CTRL_RUN | PHANTOMFPGA_CTRL_IRQ_EN
	 *   9. Set pfdev->streaming = true
	 *  10. Return 0
	 *
	 * Locking: Called with ioctl_lock held
	 */
	return -ENOTSUPP;  /* Remove this when implemented */
}

/*
 * Stop frame streaming.
 */
static int pfpga_stop_streaming(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Stop the device streaming
	 *
	 * Steps:
	 *   1. Read current CTRL register value
	 *   2. Clear PHANTOMFPGA_CTRL_RUN bit
	 *   3. Write back to CTRL register
	 *   4. Set pfdev->streaming = false
	 *   5. Wake up any waiters (they'll get EOF or EAGAIN)
	 *   6. Return 0
	 *
	 * Note: It's safe to call stop even if not streaming
	 *
	 * Locking: Called with ioctl_lock held
	 */
	return 0;
}

/*
 * Perform soft reset of the device.
 */
static void pfpga_soft_reset(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Trigger soft reset
	 *
	 * Steps:
	 *   1. Write PHANTOMFPGA_CTRL_RESET to CTRL register
	 *   2. The reset bit is self-clearing - wait briefly (udelay(10))
	 *   3. Reset local state: streaming=false, all indices=0 (including consumer)
	 *
	 * Note: Reset clears all device state including statistics
	 */
}

/* ------------------------------------------------------------------------ */
/* Interrupt Handlers                                                       */
/* The device is trying to tell you something                               */
/* ------------------------------------------------------------------------ */

/*
 * MSI-X interrupt handler for completion (vector 0).
 *
 * Called when descriptors complete (IRQ coalescing thresholds met).
 * The driver should process completed descriptors and wake up waiters.
 */
static irqreturn_t __maybe_unused pfpga_irq_complete(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;
	u32 irq_status;

	/*
	 * TODO: Handle completion interrupt
	 *
	 * Steps:
	 *   1. Read IRQ_STATUS register
	 *   2. Check if PHANTOMFPGA_IRQ_COMPLETE bit is set
	 *   3. Clear the interrupt by writing back (write-1-to-clear)
	 *   4. Read new DESC_TAIL from device (completed descriptors)
	 *   5. Update pfdev->shadow_tail under spinlock
	 *   6. Increment pfdev->irq_count
	 *   7. Wake up poll waiters
	 *   8. Return IRQ_HANDLED
	 *
	 * Pattern:
	 *   spin_lock(&pfdev->lock);
	 *   pfdev->shadow_tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL);
	 *   pfdev->irq_count++;
	 *   spin_unlock(&pfdev->lock);
	 *   wake_up_interruptible(&pfdev->wait_queue);
	 */

	(void)irq_status;
	(void)pfdev;
	return IRQ_NONE;  /* Change to IRQ_HANDLED when implemented */
}

/*
 * MSI-X interrupt handler for errors (vector 1).
 *
 * Called on error conditions (DMA error, device error).
 */
static irqreturn_t __maybe_unused pfpga_irq_error(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;

	/*
	 * TODO: Handle error interrupt
	 *
	 * Steps:
	 *   1. Read and clear IRQ_STATUS
	 *   2. Check PHANTOMFPGA_IRQ_ERROR bit
	 *   3. Log warning:
	 *      dev_warn(&pfdev->pdev->dev, "error interrupt: status=0x%x\n", status);
	 *   4. Wake up waiters so they can handle the condition
	 *   5. Return IRQ_HANDLED
	 */

	(void)pfdev;
	return IRQ_NONE;
}

/*
 * MSI-X interrupt handler for no-descriptor condition (vector 2).
 *
 * Called when device has frames to send but no descriptors available.
 * This means backpressure - consumer isn't keeping up with frame rate.
 */
static irqreturn_t __maybe_unused pfpga_irq_no_desc(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;

	/*
	 * TODO: Handle no-descriptor interrupt
	 *
	 * Steps:
	 *   1. Read and clear IRQ_STATUS (PHANTOMFPGA_IRQ_NO_DESC bit)
	 *   2. Log debug (this is expected under load):
	 *      dev_dbg(&pfdev->pdev->dev, "no descriptors available\n");
	 *   3. Wake up waiters to potentially free descriptors
	 *   4. Return IRQ_HANDLED
	 *
	 * Note: If you see this often, either increase descriptor count
	 * or reduce frame rate. Check STAT_FRAMES_DROP for total drops.
	 */

	(void)pfdev;
	return IRQ_NONE;
}

/* ------------------------------------------------------------------------ */
/* File Operations                                                          */
/* The gateway between userspace dreams and kernel reality                  */
/* ------------------------------------------------------------------------ */

/*
 * Open the device file.
 */
static int pfpga_open(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev;

	pfdev = container_of(inode->i_cdev, struct phantomfpga_dev, cdev);
	file->private_data = pfdev;

	dev_dbg(&pfdev->pdev->dev, "device opened\n");
	return 0;
}

/*
 * Close the device file.
 */
static int pfpga_release(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev = file->private_data;

	/*
	 * TODO (optional): Decide cleanup policy
	 *
	 * Options:
	 *   A) Stop streaming on close (safer for cleanup)
	 *   B) Keep streaming until explicit stop (allows multiple readers)
	 *
	 * For now, we don't auto-stop. You can decide.
	 */

	dev_dbg(&pfdev->pdev->dev, "device closed\n");
	return 0;
}

/*
 * Read frames from the device.
 *
 * In SG-DMA mode, this reads completed frame data from descriptor buffers
 * and copies to userspace.
 */
static ssize_t pfpga_read(struct file *file, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	unsigned long flags;
	u32 head, tail, pending;
	struct phantomfpga_sg_desc *desc;
	struct phantomfpga_completion *compl;
	void *buffer;
	size_t to_copy;
	int ret;

	/*
	 * TODO: Implement frame reading with SG-DMA
	 *
	 * Steps:
	 *   1. Check if device is streaming - return -EINVAL if not
	 *
	 *   2. Wait for completed descriptors if blocking:
	 *      if (!(file->f_flags & O_NONBLOCK)) {
	 *          ret = wait_event_interruptible(pfdev->wait_queue,
	 *              consumer != shadow_tail || !streaming);
	 *          // consumer != shadow_tail means IRQ handler advanced shadow_tail
	 *          if (ret) return ret;
	 *          if (!streaming) return 0;  // EOF
	 *      }
	 *
	 *   3. Check for completed-but-not-consumed frames under lock:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      cons = pfdev->consumer;
	 *      compl_tail = pfdev->shadow_tail;
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *
	 *   4. If cons == compl_tail (nothing to consume), return -EAGAIN
	 *
	 *   5. Get the next completed descriptor:
	 *      desc = &pfdev->desc_ring[cons];
	 *      if (!(desc->control & PHANTOMFPGA_DESC_CTRL_COMPLETED))
	 *          return -EAGAIN;  // Not actually complete yet
	 *
	 *   6. Read completion status from buffer end:
	 *      buffer = pfdev->buffers[cons].vaddr;
	 *      compl = phantomfpga_completion_ptr(buffer, pfdev->buffer_size);
	 *      if (compl->status != PHANTOMFPGA_COMPL_OK)
	 *          dev_warn(...);  // Handle error
	 *
	 *   7. Validate frame CRC (optional but recommended):
	 *      if (!pfpga_validate_frame_crc(buffer)) {
	 *          pfdev->crc_errors++;
	 *          // Decide: drop frame or return anyway?
	 *      }
	 *
	 *   8. Copy frame data to userspace:
	 *      to_copy = min(count, (size_t)le32_to_cpu(compl->actual_length));
	 *      if (copy_to_user(buf, buffer, to_copy))
	 *          return -EFAULT;
	 *
	 *   9. Reset descriptor for reuse:
	 *      desc->control = 0;  // Clear COMPLETED
	 *
	 *  10. Advance consumer and resubmit:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      pfdev->consumer = (cons + 1) & (pfdev->desc_count - 1);
	 *      pfdev->frames_consumed++;
	 *      pfdev->bytes_consumed += to_copy;
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *      // Resubmit one descriptor
	 *      pfpga_submit_descriptors(pfdev, 1);
	 *
	 *  11. Return bytes copied (should be PHANTOMFPGA_FRAME_SIZE on success)
	 */

	/* Stub implementation */
	(void)pfdev;
	(void)flags;
	(void)head;
	(void)tail;
	(void)pending;
	(void)desc;
	(void)compl;
	(void)buffer;
	(void)to_copy;
	(void)ret;

	return -ENOTSUPP;
}

/*
 * Write to the device - not supported.
 * The device produces frames, it doesn't consume them.
 */
static ssize_t pfpga_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	return -EPERM;
}

/*
 * Poll for readable data.
 */
static __poll_t pfpga_poll(struct file *file, poll_table *wait)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;
	u32 head, tail;

	/*
	 * TODO: Implement poll support for SG-DMA
	 *
	 * Steps:
	 *   1. Register with poll subsystem:
	 *      poll_wait(file, &pfdev->wait_queue, wait);
	 *
	 *   2. Check for completed-but-not-consumed frames:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      cons = pfdev->consumer;
	 *      compl_tail = pfdev->shadow_tail;
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *
	 *   3. Set return mask:
	 *      if (cons != compl_tail)
	 *          mask |= EPOLLIN | EPOLLRDNORM;
	 *      if (!pfdev->streaming)
	 *          mask |= EPOLLHUP;
	 *
	 *   4. Return mask
	 */

	(void)flags;
	(void)head;
	(void)tail;
	poll_wait(file, &pfdev->wait_queue, wait);

	return mask;
}

/*
 * Memory map descriptor buffers to userspace.
 *
 * This allows zero-copy access - userspace reads frames directly
 * from the DMA buffers without kernel copying.
 */
static int pfpga_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;
	unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;

	/*
	 * TODO: Implement mmap support for SG-DMA buffers
	 *
	 * The mmap layout allows mapping individual descriptor buffers
	 * or the entire buffer pool. Offset determines which buffer(s).
	 *
	 * Simple approach: Map all buffers as one contiguous region
	 *
	 * Steps:
	 *   1. Validate request:
	 *      - Check pfdev->configured is true
	 *      - Check offset == 0 (we only support mapping from start)
	 *      - Check size <= desc_count * buffer_size
	 *
	 *   2. Set page protection:
	 *      vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	 *
	 *   3. Map each buffer page:
	 *      For each buffer, use remap_pfn_range() or dma_mmap_coherent()
	 *
	 *   4. Set VM flags:
	 *      vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	 *
	 *   5. Return 0 on success
	 *
	 * Note: For simplicity, the skeleton allocates buffers as one
	 * large coherent region. Mapping is straightforward in that case.
	 */

	(void)size;
	(void)offset;
	dev_dbg(&pfdev->pdev->dev, "mmap request: size=%zu offset=%lu\n",
		size, offset);

	return -ENOTSUPP;
}

/*
 * IOCTL handler - the control center for device configuration.
 */
static long pfpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret = 0;

	/*
	 * The v3.0 ioctl interface provides:
	 *   - PHANTOMFPGA_IOCTL_SET_CFG: Configure frame rate, descriptors
	 *   - PHANTOMFPGA_IOCTL_GET_CFG: Read current configuration
	 *   - PHANTOMFPGA_IOCTL_START: Start frame streaming
	 *   - PHANTOMFPGA_IOCTL_STOP: Stop frame streaming
	 *   - PHANTOMFPGA_IOCTL_GET_STATS: Get statistics
	 *   - PHANTOMFPGA_IOCTL_RESET_STATS: Reset driver statistics
	 *   - PHANTOMFPGA_IOCTL_GET_BUFFER_INFO: Get mmap info
	 *   - PHANTOMFPGA_IOCTL_CONSUME_FRAME: Mark frame consumed (mmap mode)
	 *   - PHANTOMFPGA_IOCTL_SET_FAULT: Configure fault injection
	 */

	if (_IOC_TYPE(cmd) != PHANTOMFPGA_IOC_MAGIC)
		return -ENOTTY;

	if (_IOC_NR(cmd) > PHANTOMFPGA_IOC_MAXNR)
		return -ENOTTY;

	mutex_lock(&pfdev->ioctl_lock);

	switch (cmd) {
	case PHANTOMFPGA_IOCTL_SET_CFG:
		{
			struct phantomfpga_config cfg;

			/*
			 * TODO: Handle SET_CFG for frame streaming
			 *
			 * Steps:
			 *   1. Check !pfdev->streaming (return -EBUSY if streaming)
			 *   2. copy_from_user(&cfg, argp, sizeof(cfg))
			 *   3. Validate parameters:
			 *      - desc_count in [MIN_DESC_COUNT, MAX_DESC_COUNT]
			 *      - desc_count is power of 2
			 *      - frame_rate in [MIN_FRAME_RATE, MAX_FRAME_RATE]
			 *   4. Calculate buffer size:
			 *      buffer_size = PHANTOMFPGA_FRAME_SIZE + PHANTOMFPGA_COMPL_SIZE
			 *   5. Allocate/reallocate descriptor ring and buffers if needed
			 *   6. Store configuration in pfdev
			 *   7. Call pfpga_apply_config()
			 *   8. Call pfpga_configure_desc_ring()
			 *   9. Call pfpga_init_descriptors()
			 *  10. Set pfdev->configured = true
			 *  11. Return 0
			 */
			(void)cfg;
			ret = -ENOTSUPP;
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_CFG:
		{
			struct phantomfpga_config cfg = {
				.desc_count = pfdev->desc_count,
				.frame_rate = pfdev->frame_rate,
				.irq_coalesce_count = pfdev->irq_coalesce_count,
				.irq_coalesce_timeout = pfdev->irq_coalesce_timeout,
			};

			if (copy_to_user(argp, &cfg, sizeof(cfg)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_START:
		ret = pfpga_start_streaming(pfdev);
		break;

	case PHANTOMFPGA_IOCTL_STOP:
		ret = pfpga_stop_streaming(pfdev);
		break;

	case PHANTOMFPGA_IOCTL_GET_STATS:
		{
			struct phantomfpga_stats stats;
			unsigned long flags;

			/*
			 * TODO: Get statistics
			 *
			 * Steps:
			 *   1. Read device registers:
			 *      - PHANTOMFPGA_REG_STAT_FRAMES_TX
			 *      - PHANTOMFPGA_REG_STAT_FRAMES_DROP
			 *      - PHANTOMFPGA_REG_STAT_BYTES_LO/HI
			 *      - PHANTOMFPGA_REG_STAT_ERRORS
			 *      - PHANTOMFPGA_REG_STAT_DESC_COMPL
			 *      - PHANTOMFPGA_REG_CURRENT_FRAME
			 *      - PHANTOMFPGA_REG_DESC_HEAD/TAIL
			 *      - PHANTOMFPGA_REG_STATUS
			 *   2. Fill stats structure
			 *   3. Add driver-side stats under lock
			 *   4. copy_to_user
			 */
			memset(&stats, 0, sizeof(stats));

			spin_lock_irqsave(&pfdev->lock, flags);
			stats.frames_consumed = pfdev->frames_consumed;
			stats.bytes_consumed = pfdev->bytes_consumed;
			stats.irq_count = pfdev->irq_count;
			stats.crc_errors = pfdev->crc_errors;
			spin_unlock_irqrestore(&pfdev->lock, flags);

			/* Read device stats */
			stats.frames_produced = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_FRAMES_TX);
			stats.frames_dropped = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_FRAMES_DROP);
			stats.desc_completed = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_DESC_COMPL);
			stats.errors = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_ERRORS);
			stats.current_frame = pfpga_read32(pfdev, PHANTOMFPGA_REG_CURRENT_FRAME);
			stats.status = pfpga_read32(pfdev, PHANTOMFPGA_REG_STATUS);

			if (copy_to_user(argp, &stats, sizeof(stats)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_RESET_STATS:
		{
			unsigned long flags;

			spin_lock_irqsave(&pfdev->lock, flags);
			pfdev->frames_consumed = 0;
			pfdev->bytes_consumed = 0;
			pfdev->irq_count = 0;
			pfdev->crc_errors = 0;
			spin_unlock_irqrestore(&pfdev->lock, flags);
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_BUFFER_INFO:
		{
			struct phantomfpga_buffer_info info;

			if (!pfdev->configured) {
				ret = -EINVAL;
				break;
			}

			memset(&info, 0, sizeof(info));
			info.buffer_size = pfdev->buffer_size;
			info.buffer_count = pfdev->desc_count;
			info.total_size = pfdev->buffer_size * pfdev->desc_count;
			info.frame_size = PHANTOMFPGA_FRAME_SIZE;

			if (copy_to_user(argp, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_CONSUME_FRAME:
		{
			unsigned long flags;

			/*
			 * TODO: Mark frame consumed (mmap mode)
			 *
			 * Used when userspace reads directly from mmap'd buffer
			 * and signals completion via ioctl instead of read().
			 *
			 * Steps:
			 *   1. spin_lock_irqsave
			 *   2. Check if there are completed descriptors
			 *   3. Reset descriptor for reuse
			 *   4. Advance consumer
			 *   5. Increment frames_consumed
			 *   6. spin_unlock_irqrestore
			 *   7. Resubmit one descriptor
			 *   8. Return 0
			 */
			(void)flags;
			ret = -ENOTSUPP;
		}
		break;

	case PHANTOMFPGA_IOCTL_SET_FAULT:
		{
			struct phantomfpga_fault_cfg fault;

			/*
			 * TODO: Configure fault injection
			 *
			 * Steps:
			 *   1. copy_from_user(&fault, argp, sizeof(fault))
			 *   2. Write fault.inject_flags to PHANTOMFPGA_REG_FAULT_INJECT
			 *   3. Write fault.fault_rate to PHANTOMFPGA_REG_FAULT_RATE
			 *   4. Return 0
			 *
			 * This is for testing - lets you simulate CRC corruption,
			 * dropped frames, and sequence number skips.
			 */
			(void)fault;
			ret = -ENOTSUPP;
		}
		break;

	default:
		ret = -ENOTTY;
	}

	mutex_unlock(&pfdev->ioctl_lock);
	return ret;
}

/* File operations structure */
static const struct file_operations phantomfpga_fops = {
	.owner          = THIS_MODULE,
	.open           = pfpga_open,
	.release        = pfpga_release,
	.read           = pfpga_read,
	.write          = pfpga_write,
	.poll           = pfpga_poll,
	.mmap           = pfpga_mmap,
	.unlocked_ioctl = pfpga_ioctl,
	.compat_ioctl   = compat_ptr_ioctl,
};

/* ------------------------------------------------------------------------ */
/* PCI Device Initialization                                                */
/* ------------------------------------------------------------------------ */

/*
 * Setup MSI-X interrupts.
 */
static int pfpga_setup_msix(struct phantomfpga_dev *pfdev)
{
	struct pci_dev *pdev = pfdev->pdev;
	int ret;

	/*
	 * TODO: Setup MSI-X interrupts
	 *
	 * Steps:
	 *   1. Allocate MSI-X vectors (v3.0 has 3 vectors):
	 *      ret = pci_alloc_irq_vectors(pdev, PHANTOMFPGA_MSIX_VECTORS,
	 *                                  PHANTOMFPGA_MSIX_VECTORS, PCI_IRQ_MSIX);
	 *      if (ret < 0) {
	 *          // Fallback to MSI or legacy
	 *          ret = pci_alloc_irq_vectors(pdev, 1, 1,
	 *                                      PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	 *          if (ret < 0) return ret;
	 *      }
	 *      pfdev->num_vectors = ret;
	 *
	 *   2. Get IRQ numbers:
	 *      pfdev->irq_complete = pci_irq_vector(pdev, PHANTOMFPGA_MSIX_VEC_COMPLETE);
	 *      if (pfdev->num_vectors > 1)
	 *          pfdev->irq_error = pci_irq_vector(pdev, PHANTOMFPGA_MSIX_VEC_ERROR);
	 *      if (pfdev->num_vectors > 2)
	 *          pfdev->irq_no_desc = pci_irq_vector(pdev, PHANTOMFPGA_MSIX_VEC_NO_DESC);
	 *
	 *   3. Request IRQs:
	 *      ret = request_irq(pfdev->irq_complete, pfpga_irq_complete,
	 *                        0, DRIVER_NAME "-complete", pfdev);
	 *      // Similar for error and no_desc vectors
	 *
	 *   4. Return 0 on success
	 */

	(void)pdev;
	(void)ret;
	pfdev->num_vectors = 0;
	pfdev->irq_complete = -1;
	pfdev->irq_error = -1;
	pfdev->irq_no_desc = -1;

	dev_info(&pdev->dev, "MSI-X setup skipped (TODO)\n");
	return 0;
}

/*
 * Cleanup MSI-X interrupts.
 */
static void pfpga_teardown_msix(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Release MSI-X resources
	 *
	 * Steps:
	 *   1. Free IRQs:
	 *      if (pfdev->irq_complete >= 0)
	 *          free_irq(pfdev->irq_complete, pfdev);
	 *      if (pfdev->irq_error >= 0)
	 *          free_irq(pfdev->irq_error, pfdev);
	 *      if (pfdev->irq_no_desc >= 0)
	 *          free_irq(pfdev->irq_no_desc, pfdev);
	 *
	 *   2. Free vectors:
	 *      if (pfdev->num_vectors > 0)
	 *          pci_free_irq_vectors(pfdev->pdev);
	 */
}

/*
 * Free descriptor ring and buffers.
 */
static void pfpga_free_descriptors(struct phantomfpga_dev *pfdev)
{
	struct device *dev = &pfdev->pdev->dev;

	if (pfdev->buffers != NULL) {
		for (int i = 0; i < pfdev->desc_count; i++) {
			struct phantomfpga_buffer *buffer = pfdev->buffers + i;
			if (buffer->vaddr != NULL) {
				dma_free_coherent(dev, buffer->size, buffer->vaddr, buffer->dma_addr);
			}
		}

		kfree(pfdev->buffers);
		dev_info(dev, "Succesfully freed dma buffers");
	}

	if (pfdev->desc_ring != NULL) {
		dma_free_coherent(dev, pfdev->desc_count * sizeof(struct phantomfpga_sg_desc), pfdev->desc_ring, pfdev->desc_ring_dma);
		dev_info(dev, "Succesfully freed desc ring");
	}
}

/*
 * Allocate descriptor ring and per-descriptor buffers.
 */
static int pfpga_alloc_descriptors(struct phantomfpga_dev *pfdev,
				   u32 desc_count, size_t buffer_size)
{
	struct device *dev = &pfdev->pdev->dev;

	// Allocate the ring itself
	pfdev->desc_ring = dma_alloc_coherent(dev, sizeof(struct phantomfpga_sg_desc) * desc_count, &pfdev->desc_ring_dma, GFP_KERNEL);
	if (pfdev->desc_ring == NULL) {
		goto alloc_err;
	}

	// Allocate buffers
	pfdev->buffers = kzalloc(desc_count * sizeof(struct phantomfpga_buffer), GFP_KERNEL);
	if (pfdev->buffers == NULL) {
		goto alloc_err;
	}

	size_t complete_buffer_size = buffer_size + sizeof(struct phantomfpga_completion);
	for (int i = 0; i < desc_count; i++) {
		struct phantomfpga_buffer *buffer = pfdev->buffers + i;
		buffer->vaddr = dma_alloc_coherent(dev, complete_buffer_size, &buffer->dma_addr, GFP_KERNEL);
		if (buffer->vaddr == NULL) {
			goto alloc_err;
		}
		// TODO: Check if this should include the completion size too
		buffer->size = complete_buffer_size;
	}

	dev_info(dev, "succesfully allocated descriptors, dma_ring at physical address: 0x%llx\n", pfdev->desc_ring_dma);
	return 0;
alloc_err:
	pfpga_free_descriptors(pfdev);
	return -ENOMEM;
}

/*
 * Create character device.
 */
static int pfpga_create_cdev(struct phantomfpga_dev *pfdev)
{
	int minor;
	int ret;

	minor = ida_alloc_max(&phantomfpga_ida, PHANTOMFPGA_MAX_DEVICES - 1,
			      GFP_KERNEL);
	if (minor < 0)
		return minor;

	pfdev->minor = minor;
	pfdev->devno = MKDEV(MAJOR(phantomfpga_devno), minor);

	cdev_init(&pfdev->cdev, &phantomfpga_fops);
	pfdev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&pfdev->cdev, pfdev->devno, 1);
	if (ret)
		goto err_ida;

	pfdev->dev = device_create(phantomfpga_class, &pfdev->pdev->dev,
				   pfdev->devno, pfdev, DRIVER_NAME "%d", minor);
	if (IS_ERR(pfdev->dev)) {
		ret = PTR_ERR(pfdev->dev);
		goto err_cdev;
	}

	dev_info(&pfdev->pdev->dev, "created /dev/%s%d\n", DRIVER_NAME, minor);
	return 0;

err_cdev:
	cdev_del(&pfdev->cdev);
err_ida:
	ida_free(&phantomfpga_ida, minor);
	return ret;
}

/*
 * Destroy character device.
 */
static void pfpga_destroy_cdev(struct phantomfpga_dev *pfdev)
{
	device_destroy(phantomfpga_class, pfdev->devno);
	cdev_del(&pfdev->cdev);
	ida_free(&phantomfpga_ida, pfdev->minor);
}

/*
 * PCI probe function - called when kernel finds matching device.
 */
static int phantomfpga_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct phantomfpga_dev *pfdev;
	u32 dev_id, dev_ver;
	int ret;

	dev_info(&pdev->dev, "probing PhantomFPGA v3.0 device\n");

	/* Allocate device private data */
	pfdev = kzalloc(sizeof(*pfdev), GFP_KERNEL);
	if (!pfdev)
		return -ENOMEM;

	pfdev->pdev = pdev;
	spin_lock_init(&pfdev->lock);
	mutex_init(&pfdev->ioctl_lock);
	init_waitqueue_head(&pfdev->wait_queue);

	pci_set_drvdata(pdev, pfdev);

	/* Enable PCI device */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device\n");
		goto err_free;
	}

	/* Request BAR0 region */
	ret = pci_request_region(pdev, 0, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR0\n");
		goto err_disable;
	}

	/* Map BAR0 */
	pfdev->regs_start = pci_resource_start(pdev, 0);
	pfdev->regs_len = pci_resource_len(pdev, 0);
	pfdev->regs = pci_iomap(pdev, 0, pfdev->regs_len);
	if (!pfdev->regs) {
		dev_err(&pdev->dev, "failed to map BAR0\n");
		ret = -ENOMEM;
		goto err_release;
	}

	dev_info(&pdev->dev, "BAR0 mapped: phys=0x%llx len=%llu virt=%p\n",
		 (unsigned long long)pfdev->regs_start,
		 (unsigned long long)pfdev->regs_len,
		 pfdev->regs);

	/* Verify device identity */
	dev_id = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_ID);
	if (dev_id != PHANTOMFPGA_DEV_ID_VAL) {
		dev_err(&pdev->dev, "unexpected device ID: 0x%08x (expected 0x%08x)\n",
			dev_id, PHANTOMFPGA_DEV_ID_VAL);
		ret = -ENODEV;
		goto err_unmap;
	}

	/* Check version */
	dev_ver = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_VER);
	dev_info(&pdev->dev, "device ID: 0x%08x version: 0x%08x\n", dev_id, dev_ver);

	if (dev_ver < PHANTOMFPGA_DEV_VER) {
		dev_warn(&pdev->dev, "device version older than driver expects, "
			 "things might get interesting\n");
	}

	/* Enable bus mastering for DMA */
	pci_set_master(pdev);

	/* Set DMA mask - try 64-bit, fall back to 32-bit */
	ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (ret) {
		ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (ret) {
			dev_err(&pdev->dev, "failed to set DMA mask\n");
			goto err_unmap;
		}
		dev_info(&pdev->dev, "using 32-bit DMA\n");
	} else {
		dev_info(&pdev->dev, "using 64-bit DMA\n");
	}

	/* Setup MSI-X interrupts */
	ret = pfpga_setup_msix(pfdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to setup MSI-X: %d\n", ret);
		goto err_unmap;
	}

	/* Allocate default descriptors and buffers */
	ret = pfpga_alloc_descriptors(pfdev,
				      PHANTOMFPGA_DEFAULT_DESC_COUNT,
				      PHANTOMFPGA_BUFFER_SIZE);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate descriptors: %d\n", ret);
		goto err_msix;
	}

	/* Perform soft reset */
	pfpga_soft_reset(pfdev);

	/* Create character device */
	ret = pfpga_create_cdev(pfdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to create char device: %d\n", ret);
		goto err_desc;
	}

	/* Set default configuration (not yet applied) */
	pfdev->desc_count = PHANTOMFPGA_DEFAULT_DESC_COUNT;
	pfdev->frame_rate = PHANTOMFPGA_DEFAULT_FRAME_RATE;
	pfdev->irq_coalesce_count = PHANTOMFPGA_DEFAULT_IRQ_COUNT;
	pfdev->irq_coalesce_timeout = PHANTOMFPGA_DEFAULT_IRQ_TIMEOUT;
	pfdev->buffer_size = PHANTOMFPGA_BUFFER_SIZE;
	pfdev->configured = false;
	pfdev->streaming = false;

	pfpga_init_descriptors(pfdev);
	pfpga_configure_desc_ring(pfdev);

	dev_info(&pdev->dev, "PhantomFPGA v3.0 driver loaded\n");
	return 0;

err_desc:
	pfpga_free_descriptors(pfdev);
err_msix:
	pfpga_teardown_msix(pfdev);
err_unmap:
	pci_iounmap(pdev, pfdev->regs);
err_release:
	pci_release_region(pdev, 0);
err_disable:
	pci_disable_device(pdev);
err_free:
	kfree(pfdev);
	return ret;
}

/*
 * PCI remove function - called when device is removed or driver unloaded.
 */
static void phantomfpga_remove(struct pci_dev *pdev)
{
	struct phantomfpga_dev *pfdev = pci_get_drvdata(pdev);

	dev_info(&pdev->dev, "removing PhantomFPGA device\n");

	/* Stop streaming if active */
	if (pfdev->streaming) {
		mutex_lock(&pfdev->ioctl_lock);
		pfpga_stop_streaming(pfdev);
		mutex_unlock(&pfdev->ioctl_lock);
	}

	/* Destroy character device */
	pfpga_destroy_cdev(pfdev);

	/* Free descriptors and buffers */
	pfpga_free_descriptors(pfdev);

	/* Release interrupts */
	pfpga_teardown_msix(pfdev);

	/* Unmap and release BAR0 */
	if (pfdev->regs) {
		pci_iounmap(pdev, pfdev->regs);
		pfdev->regs = NULL;
	}
	pci_release_region(pdev, 0);

	/* Disable device */
	pci_disable_device(pdev);

	/* Free private data */
	kfree(pfdev);

	dev_info(&pdev->dev, "PhantomFPGA driver unloaded\n");
}

/* PCI driver structure */
static struct pci_driver phantomfpga_pci_driver = {
	.name     = DRIVER_NAME,
	.id_table = phantomfpga_pci_ids,
	.probe    = phantomfpga_probe,
	.remove   = phantomfpga_remove,
};

/* ------------------------------------------------------------------------ */
/* Module Init/Exit                                                         */
/* ------------------------------------------------------------------------ */

static int __init phantomfpga_init(void)
{
	int ret;

	pr_info("PhantomFPGA v3.0 driver initializing\n");

	ret = alloc_chrdev_region(&phantomfpga_devno, 0, PHANTOMFPGA_MAX_DEVICES,
				  DRIVER_NAME);
	if (ret) {
		pr_err("failed to allocate chrdev region: %d\n", ret);
		return ret;
	}

	phantomfpga_class = class_create(DRIVER_NAME);
	if (IS_ERR(phantomfpga_class)) {
		ret = PTR_ERR(phantomfpga_class);
		pr_err("failed to create device class: %d\n", ret);
		goto err_chrdev;
	}

	ret = pci_register_driver(&phantomfpga_pci_driver);
	if (ret) {
		pr_err("failed to register PCI driver: %d\n", ret);
		goto err_class;
	}

	pr_info("PhantomFPGA v3.0 driver initialized (major=%d)\n",
		MAJOR(phantomfpga_devno));
	return 0;

err_class:
	class_destroy(phantomfpga_class);
err_chrdev:
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);
	return ret;
}

static void __exit phantomfpga_exit(void)
{
	pr_info("PhantomFPGA v3.0 driver exiting\n");

	pci_unregister_driver(&phantomfpga_pci_driver);
	class_destroy(phantomfpga_class);
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);

	pr_info("PhantomFPGA v3.0 driver unloaded\n");
}

module_init(phantomfpga_init);
module_exit(phantomfpga_exit);
