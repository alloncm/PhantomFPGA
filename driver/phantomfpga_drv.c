// SPDX-License-Identifier: GPL-2.0
/*
 * PhantomFPGA PCIe Driver - Trainee Skeleton
 *
 * A Linux kernel driver for the PhantomFPGA virtual PCI device.
 * This skeleton provides the structure - you fill in the TODOs!
 *
 * The device produces frames at a configurable rate and writes them
 * to a shared DMA ring buffer. The driver exposes a char device for
 * userspace to read frames via read() or mmap().
 *
 * Learning objectives:
 *   - PCI device probing and BAR mapping
 *   - DMA buffer allocation with dma_alloc_coherent()
 *   - MSI-X interrupt handling
 *   - Ring buffer producer/consumer patterns
 *   - Character device file operations
 *   - IOCTL interface design
 *   - Memory mapping with mmap()
 *
 * Copyright (C) 2024 PhantomFPGA Project
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

#include "phantomfpga_regs.h"
#include "phantomfpga_uapi.h"

/* Module metadata */
MODULE_AUTHOR("Trainee");
MODULE_DESCRIPTION("PhantomFPGA Virtual DMA Device Driver");
MODULE_LICENSE("GPL");
MODULE_VERSION("0.1");

/* Driver constants */
#define DRIVER_NAME             "phantomfpga"
#define PHANTOMFPGA_MAX_DEVICES 4

/* ------------------------------------------------------------------------ */
/* Device Private Data Structure                                            */
/* ------------------------------------------------------------------------ */

/*
 * struct phantomfpga_dev - Per-device private data
 *
 * This structure holds all state for a single PhantomFPGA device.
 * One instance is allocated per PCI device in probe().
 */
struct phantomfpga_dev {
	/* PCI device reference */
	struct pci_dev *pdev;

	/* BAR0 register mapping */
	void __iomem *regs;         /* Kernel virtual address of BAR0 */
	resource_size_t regs_start; /* Physical address of BAR0 */
	resource_size_t regs_len;   /* Length of BAR0 region */

	/* DMA buffer */
	void *dma_buf;              /* Kernel virtual address of DMA buffer */
	dma_addr_t dma_handle;      /* Physical/bus address for device */
	size_t dma_size;            /* Total DMA buffer size */

	/* Configuration (mirrors device registers) */
	u32 frame_size;
	u32 frame_rate;
	u32 ring_size;
	u32 watermark;
	bool configured;            /* Has SET_CFG been called? */
	bool streaming;             /* Is device currently streaming? */

	/* Ring buffer indices */
	u32 prod_idx;               /* Cached producer index */
	u32 cons_idx;               /* Consumer index (driver managed) */

	/* Statistics (driver-side) */
	u64 frames_consumed;
	u32 irq_count;

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
	int irq_watermark;          /* IRQ number for watermark vector */
	int irq_overrun;            /* IRQ number for overrun vector */
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
/* ------------------------------------------------------------------------ */

/*
 * These helpers provide type-safe register access and can be extended
 * for debugging (e.g., logging all register accesses).
 */

static inline u32 pfpga_read32(struct phantomfpga_dev *pfdev, u32 offset)
{
	return ioread32(pfdev->regs + offset);
}

static inline void pfpga_write32(struct phantomfpga_dev *pfdev, u32 offset, u32 val)
{
	iowrite32(val, pfdev->regs + offset);
}

/* ------------------------------------------------------------------------ */
/* Hardware Operations                                                      */
/* ------------------------------------------------------------------------ */

/*
 * Configure device DMA address.
 * Called after DMA buffer allocation to tell device where to write frames.
 */
static void pfpga_configure_dma(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Write the DMA buffer physical address to the device
	 *
	 * Steps:
	 *   1. Split pfdev->dma_handle into low and high 32-bit parts
	 *   2. Write low 32 bits to PHANTOMFPGA_REG_DMA_ADDR_LO
	 *   3. Write high 32 bits to PHANTOMFPGA_REG_DMA_ADDR_HI
	 *   4. Write total buffer size to PHANTOMFPGA_REG_DMA_SIZE
	 *
	 * Hint: Use lower_32_bits() and upper_32_bits() macros
	 */
}

/*
 * Apply configuration parameters to device registers.
 * Called from SET_CFG ioctl.
 */
static void pfpga_apply_config(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Write configuration to device registers
	 *
	 * Steps:
	 *   1. Write pfdev->frame_size to PHANTOMFPGA_REG_FRAME_SIZE
	 *   2. Write pfdev->frame_rate to PHANTOMFPGA_REG_FRAME_RATE
	 *   3. Write pfdev->watermark to PHANTOMFPGA_REG_WATERMARK
	 *   4. Write pfdev->ring_size to PHANTOMFPGA_REG_RING_SIZE
	 *   5. Reset consumer index: write 0 to PHANTOMFPGA_REG_CONS_IDX
	 *   6. Enable watermark interrupt in PHANTOMFPGA_REG_IRQ_MASK
	 *
	 * Remember: Configuration should only be applied when not streaming!
	 */
}

/*
 * Start frame production.
 */
static int pfpga_start_streaming(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Start the device streaming
	 *
	 * Steps:
	 *   1. Check pfdev->configured - return -EINVAL if not configured
	 *   2. Check pfdev->streaming - return -EBUSY if already streaming
	 *   3. Reset indices: pfdev->prod_idx = pfdev->cons_idx = 0
	 *   4. Write 0 to PHANTOMFPGA_REG_CONS_IDX (sync with device)
	 *   5. Clear any pending IRQs: write PHANTOMFPGA_IRQ_ALL to IRQ_STATUS
	 *   6. Read and set CTRL register:
	 *      - Set PHANTOMFPGA_CTRL_START bit
	 *      - Set PHANTOMFPGA_CTRL_IRQ_EN bit
	 *   7. Set pfdev->streaming = true
	 *   8. Return 0
	 *
	 * Locking: Called with ioctl_lock held
	 */
	return -ENOTSUPP;  /* Remove this when implemented */
}

/*
 * Stop frame production.
 */
static int pfpga_stop_streaming(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Stop the device streaming
	 *
	 * Steps:
	 *   1. Read current CTRL register value
	 *   2. Clear PHANTOMFPGA_CTRL_START bit
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
	 *   3. Reset local state: streaming=false, prod_idx=cons_idx=0
	 *
	 * Note: Reset clears all device state including statistics
	 */
}

/* ------------------------------------------------------------------------ */
/* Interrupt Handler                                                        */
/* ------------------------------------------------------------------------ */

/*
 * MSI-X interrupt handler for watermark (vector 0).
 *
 * Called when the number of pending frames reaches the watermark threshold.
 * The driver should wake up any processes waiting to read data.
 */
static irqreturn_t pfpga_irq_watermark(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;
	u32 irq_status;

	/*
	 * TODO: Handle watermark interrupt
	 *
	 * Steps:
	 *   1. Read IRQ_STATUS register to confirm interrupt source
	 *   2. Check if PHANTOMFPGA_IRQ_WATERMARK bit is set
	 *   3. Clear the interrupt by writing back the status (write-1-to-clear)
	 *   4. Update cached producer index from PHANTOMFPGA_REG_PROD_IDX
	 *   5. Increment pfdev->irq_count
	 *   6. Wake up poll waiters with wake_up_interruptible(&pfdev->wait_queue)
	 *   7. Return IRQ_HANDLED
	 *
	 * Locking: Use spin_lock(&pfdev->lock) to protect shared state
	 *
	 * Example pattern:
	 *   spin_lock(&pfdev->lock);
	 *   ... update state ...
	 *   spin_unlock(&pfdev->lock);
	 *   wake_up_interruptible(&pfdev->wait_queue);
	 *   return IRQ_HANDLED;
	 */

	/* Stub: just acknowledge and return */
	(void)irq_status;
	(void)pfdev;
	return IRQ_NONE;  /* Change to IRQ_HANDLED when implemented */
}

/*
 * MSI-X interrupt handler for overrun (vector 1).
 *
 * Called when the ring buffer overflows (producer catches up to consumer).
 * This indicates the userspace application is too slow.
 */
static irqreturn_t pfpga_irq_overrun(int irq, void *data)
{
	struct phantomfpga_dev *pfdev = data;

	/*
	 * TODO: Handle overrun interrupt
	 *
	 * Steps:
	 *   1. Read and clear IRQ_STATUS (PHANTOMFPGA_IRQ_OVERRUN bit)
	 *   2. Log a warning: dev_warn(&pfdev->pdev->dev, "ring buffer overrun!\n")
	 *   3. Increment irq_count
	 *   4. Wake up waiters (they should handle the overrun condition)
	 *   5. Return IRQ_HANDLED
	 *
	 * Note: Overrun means frames were lost. The producer will continue
	 * from where it was, potentially overwriting unread frames.
	 */

	(void)pfdev;
	return IRQ_NONE;
}

/* ------------------------------------------------------------------------ */
/* File Operations                                                          */
/* ------------------------------------------------------------------------ */

/*
 * Open the device file.
 *
 * Called when userspace opens /dev/phantomfpga.
 * Store device pointer in file->private_data for later operations.
 */
static int pfpga_open(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev;

	/*
	 * TODO: Handle file open
	 *
	 * Steps:
	 *   1. Get device pointer from inode:
	 *      pfdev = container_of(inode->i_cdev, struct phantomfpga_dev, cdev);
	 *   2. Store in file->private_data for use in other fops
	 *   3. Optionally: implement exclusive access (only one opener)
	 *      - Use atomic flag or mutex
	 *      - Return -EBUSY if already open
	 *   4. Return 0 on success
	 *
	 * Note: For this skeleton, multiple opens are allowed.
	 *       A production driver might want exclusive access.
	 */

	pfdev = container_of(inode->i_cdev, struct phantomfpga_dev, cdev);
	file->private_data = pfdev;

	dev_dbg(&pfdev->pdev->dev, "device opened\n");
	return 0;
}

/*
 * Close the device file.
 *
 * Called when userspace closes the file descriptor.
 * Stop streaming if this was the last reference.
 */
static int pfpga_release(struct inode *inode, struct file *file)
{
	struct phantomfpga_dev *pfdev = file->private_data;

	/*
	 * TODO: Handle file close
	 *
	 * Steps:
	 *   1. Consider: should closing the file stop streaming?
	 *      - Option A: Always stop on close (safer for cleanup)
	 *      - Option B: Keep streaming until explicit stop
	 *   2. If implementing exclusive access, release the lock here
	 *   3. Return 0
	 *
	 * For this skeleton, we don't auto-stop on close.
	 * The trainee can decide the best policy.
	 */

	dev_dbg(&pfdev->pdev->dev, "device closed\n");
	return 0;
}

/*
 * Read frames from the device.
 *
 * Copies frame data from the DMA ring buffer to userspace.
 * Blocking behavior depends on O_NONBLOCK flag.
 */
static ssize_t pfpga_read(struct file *file, char __user *buf,
			  size_t count, loff_t *ppos)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	unsigned long flags;
	u32 prod_idx, cons_idx, pending;
	size_t frame_offset, to_copy;
	void *frame_ptr;
	int ret;

	/*
	 * TODO: Implement frame reading
	 *
	 * Steps:
	 *   1. Check if device is streaming - return -EINVAL if not
	 *
	 *   2. Wait for data if blocking:
	 *      if (!(file->f_flags & O_NONBLOCK)) {
	 *          ret = wait_event_interruptible(pfdev->wait_queue,
	 *              !phantomfpga_ring_empty(prod_idx, cons_idx) || !streaming);
	 *          if (ret) return ret;  // interrupted by signal
	 *          if (!streaming) return 0;  // EOF on stop
	 *      }
	 *
	 *   3. Check for available frames under lock:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      prod_idx = pfdev->prod_idx;  // or read from device
	 *      cons_idx = pfdev->cons_idx;
	 *      pending = phantomfpga_ring_pending(prod_idx, cons_idx, ring_size);
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *
	 *   4. If no frames and non-blocking, return -EAGAIN
	 *
	 *   5. Calculate frame address in DMA buffer:
	 *      frame_offset = phantomfpga_frame_offset(cons_idx, frame_size);
	 *      frame_ptr = pfdev->dma_buf + frame_offset;
	 *
	 *   6. Copy to userspace (limit by count and frame_size):
	 *      to_copy = min(count, (size_t)pfdev->frame_size);
	 *      if (copy_to_user(buf, frame_ptr, to_copy))
	 *          return -EFAULT;
	 *
	 *   7. Advance consumer index:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      pfdev->cons_idx = (cons_idx + 1) & (ring_size - 1);
	 *      pfpga_write32(pfdev, PHANTOMFPGA_REG_CONS_IDX, pfdev->cons_idx);
	 *      pfdev->frames_consumed++;
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *
	 *   8. Return number of bytes copied
	 *
	 * Note: This implementation reads one frame per read() call.
	 *       The trainee may optimize to read multiple frames.
	 */

	/* Stub implementation - always returns "not implemented" */
	(void)pfdev;
	(void)flags;
	(void)prod_idx;
	(void)cons_idx;
	(void)pending;
	(void)frame_offset;
	(void)to_copy;
	(void)frame_ptr;
	(void)ret;

	return -ENOTSUPP;
}

/*
 * Write to the device.
 *
 * Not supported - the device is a frame producer, not consumer.
 */
static ssize_t pfpga_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *ppos)
{
	/* Device is read-only (it produces frames, doesn't consume them) */
	return -EPERM;
}

/*
 * Poll for readable data.
 *
 * Used by select()/poll()/epoll() to wait for frames.
 */
static __poll_t pfpga_poll(struct file *file, poll_table *wait)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	__poll_t mask = 0;
	unsigned long flags;

	/*
	 * TODO: Implement poll support
	 *
	 * Steps:
	 *   1. Register with poll subsystem:
	 *      poll_wait(file, &pfdev->wait_queue, wait);
	 *
	 *   2. Check current state under lock:
	 *      spin_lock_irqsave(&pfdev->lock, flags);
	 *      prod_idx = pfdev->prod_idx;  // or read from device
	 *      cons_idx = pfdev->cons_idx;
	 *      spin_unlock_irqrestore(&pfdev->lock, flags);
	 *
	 *   3. Set return mask:
	 *      - EPOLLIN | EPOLLRDNORM if frames available
	 *      - EPOLLHUP if device stopped/error
	 *      - EPOLLERR if error condition
	 *
	 *   Example:
	 *      if (!phantomfpga_ring_empty(prod_idx, cons_idx))
	 *          mask |= EPOLLIN | EPOLLRDNORM;
	 *
	 *   4. Return mask
	 */

	(void)flags;
	poll_wait(file, &pfdev->wait_queue, wait);

	/* Stub: always report nothing available */
	return mask;
}

/*
 * Memory map the DMA buffer to userspace.
 *
 * Allows zero-copy access to frames directly in the DMA buffer.
 */
static int pfpga_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	size_t size = vma->vm_end - vma->vm_start;

	/*
	 * TODO: Implement mmap support
	 *
	 * Steps:
	 *   1. Validate request:
	 *      - Check pfdev->configured is true
	 *      - Check size <= pfdev->dma_size
	 *      - Check vma->vm_pgoff == 0 (we only support offset 0)
	 *
	 *   2. Set appropriate page protection:
	 *      vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
	 *      (Ensures CPU reads fresh data from DMA buffer)
	 *
	 *   3. Map the DMA buffer:
	 *      Option A - Using dma_mmap_coherent (preferred):
	 *        ret = dma_mmap_coherent(&pfdev->pdev->dev, vma,
	 *                                pfdev->dma_buf, pfdev->dma_handle,
	 *                                pfdev->dma_size);
	 *
	 *      Option B - Using remap_pfn_range:
	 *        unsigned long pfn = virt_to_phys(pfdev->dma_buf) >> PAGE_SHIFT;
	 *        ret = remap_pfn_range(vma, vma->vm_start, pfn, size,
	 *                              vma->vm_page_prot);
	 *
	 *   4. Set VM flags:
	 *      vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);
	 *
	 *   5. Return result (0 on success, negative errno on failure)
	 *
	 * Security note: The userspace can read the buffer but should not
	 * modify producer index or write arbitrary data.
	 */

	(void)size;
	dev_dbg(&pfdev->pdev->dev, "mmap request: size=%zu\n", size);

	return -ENOTSUPP;
}

/*
 * IOCTL handler - main control interface.
 */
static long pfpga_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct phantomfpga_dev *pfdev = file->private_data;
	void __user *argp = (void __user *)arg;
	int ret = 0;

	/*
	 * TODO: Implement IOCTL handling
	 *
	 * The ioctl interface provides:
	 *   - PHANTOMFPGA_IOCTL_SET_CFG: Configure frame size, rate, etc.
	 *   - PHANTOMFPGA_IOCTL_GET_CFG: Read current configuration
	 *   - PHANTOMFPGA_IOCTL_START: Start frame production
	 *   - PHANTOMFPGA_IOCTL_STOP: Stop frame production
	 *   - PHANTOMFPGA_IOCTL_GET_STATS: Get statistics
	 *   - PHANTOMFPGA_IOCTL_RESET_STATS: Reset driver statistics
	 *   - PHANTOMFPGA_IOCTL_GET_BUFFER_INFO: Get mmap info
	 *   - PHANTOMFPGA_IOCTL_CONSUME_FRAME: Advance consumer index
	 *
	 * Pattern for each command:
	 *   1. mutex_lock(&pfdev->ioctl_lock)
	 *   2. Process command
	 *   3. mutex_unlock(&pfdev->ioctl_lock)
	 *
	 * Use copy_from_user()/copy_to_user() for data transfer.
	 */

	/* Validate ioctl magic number */
	if (_IOC_TYPE(cmd) != PHANTOMFPGA_IOC_MAGIC)
		return -ENOTTY;

	/* Validate ioctl command number */
	if (_IOC_NR(cmd) > PHANTOMFPGA_IOC_MAXNR)
		return -ENOTTY;

	mutex_lock(&pfdev->ioctl_lock);

	switch (cmd) {
	case PHANTOMFPGA_IOCTL_SET_CFG:
		{
			struct phantomfpga_config cfg;

			/*
			 * TODO: Handle SET_CFG
			 *
			 * Steps:
			 *   1. Check !pfdev->streaming (return -EBUSY if streaming)
			 *   2. copy_from_user(&cfg, argp, sizeof(cfg))
			 *   3. Validate parameters:
			 *      - frame_size in [MIN_FRAME_SIZE, MAX_FRAME_SIZE]
			 *      - frame_rate in [MIN_FRAME_RATE, MAX_FRAME_RATE]
			 *      - ring_size in [MIN_RING_SIZE, MAX_RING_SIZE]
			 *      - ring_size is power of 2: (ring_size & (ring_size-1)) == 0
			 *      - watermark > 0 && watermark < ring_size
			 *      - reserved[] all zero
			 *   4. Calculate and allocate DMA buffer if needed:
			 *      - dma_size = frame_size * ring_size
			 *      - If existing buffer too small, free and reallocate
			 *   5. Store in pfdev: frame_size, frame_rate, ring_size, watermark
			 *   6. Call pfpga_apply_config(pfdev)
			 *   7. Call pfpga_configure_dma(pfdev)
			 *   8. Set pfdev->configured = true
			 *   9. Return 0
			 */
			(void)cfg;
			ret = -ENOTSUPP;
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_CFG:
		{
			struct phantomfpga_config cfg = {
				.frame_size = pfdev->frame_size,
				.frame_rate = pfdev->frame_rate,
				.ring_size = pfdev->ring_size,
				.watermark = pfdev->watermark,
			};

			if (copy_to_user(argp, &cfg, sizeof(cfg)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_START:
		/*
		 * TODO: Start streaming
		 *
		 * Just call pfpga_start_streaming(pfdev) and return result
		 */
		ret = -ENOTSUPP;
		break;

	case PHANTOMFPGA_IOCTL_STOP:
		/*
		 * TODO: Stop streaming
		 *
		 * Just call pfpga_stop_streaming(pfdev) and return result
		 */
		ret = -ENOTSUPP;
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
			 *      - PHANTOMFPGA_REG_STAT_FRAMES
			 *      - PHANTOMFPGA_REG_STAT_ERRORS
			 *      - PHANTOMFPGA_REG_STAT_OVERRUNS
			 *      - PHANTOMFPGA_REG_PROD_IDX
			 *      - PHANTOMFPGA_REG_CONS_IDX
			 *      - PHANTOMFPGA_REG_STATUS
			 *   2. Fill stats structure with register values
			 *   3. Add driver-side stats under lock:
			 *      spin_lock_irqsave(&pfdev->lock, flags);
			 *      stats.frames_consumed = pfdev->frames_consumed;
			 *      stats.irq_count = pfdev->irq_count;
			 *      spin_unlock_irqrestore(&pfdev->lock, flags);
			 *   4. copy_to_user(argp, &stats, sizeof(stats))
			 */
			memset(&stats, 0, sizeof(stats));
			(void)flags;

			if (copy_to_user(argp, &stats, sizeof(stats)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_RESET_STATS:
		{
			unsigned long flags;

			/*
			 * TODO: Reset driver-side statistics
			 *
			 * Steps:
			 *   spin_lock_irqsave(&pfdev->lock, flags);
			 *   pfdev->frames_consumed = 0;
			 *   pfdev->irq_count = 0;
			 *   spin_unlock_irqrestore(&pfdev->lock, flags);
			 *
			 * Note: Device-side counters are reset via soft reset
			 */
			(void)flags;
		}
		break;

	case PHANTOMFPGA_IOCTL_GET_BUFFER_INFO:
		{
			struct phantomfpga_buffer_info info;

			/*
			 * TODO: Return buffer information
			 *
			 * Steps:
			 *   1. Check pfdev->configured (return -EINVAL if not)
			 *   2. Fill info structure:
			 *      info.buffer_size = pfdev->dma_size;
			 *      info.frame_size = pfdev->frame_size;
			 *      info.ring_size = pfdev->ring_size;
			 *      info.mmap_offset = 0;
			 *   3. copy_to_user(argp, &info, sizeof(info))
			 */
			memset(&info, 0, sizeof(info));

			if (!pfdev->configured) {
				ret = -EINVAL;
				break;
			}

			info.buffer_size = pfdev->dma_size;
			info.frame_size = pfdev->frame_size;
			info.ring_size = pfdev->ring_size;
			info.mmap_offset = 0;

			if (copy_to_user(argp, &info, sizeof(info)))
				ret = -EFAULT;
		}
		break;

	case PHANTOMFPGA_IOCTL_CONSUME_FRAME:
		{
			unsigned long flags;

			/*
			 * TODO: Advance consumer index by one frame
			 *
			 * Used with mmap() access pattern where userspace reads
			 * directly from the mapped buffer and signals completion.
			 *
			 * Steps:
			 *   1. spin_lock_irqsave(&pfdev->lock, flags);
			 *   2. Check if frames available (prod_idx != cons_idx)
			 *      - If not, unlock and return -EAGAIN
			 *   3. Advance: cons_idx = (cons_idx + 1) & (ring_size - 1)
			 *   4. Write new cons_idx to device register
			 *   5. Increment frames_consumed
			 *   6. spin_unlock_irqrestore(&pfdev->lock, flags);
			 *   7. Return 0
			 */
			(void)flags;
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
	.compat_ioctl   = compat_ptr_ioctl,  /* 32-bit compat */
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
	 *   1. Allocate MSI-X vectors:
	 *      ret = pci_alloc_irq_vectors(pdev, PHANTOMFPGA_MSIX_VECTORS,
	 *                                  PHANTOMFPGA_MSIX_VECTORS, PCI_IRQ_MSIX);
	 *      if (ret < 0) {
	 *          // Fallback: try single MSI or legacy interrupt
	 *          ret = pci_alloc_irq_vectors(pdev, 1, 1,
	 *                                      PCI_IRQ_MSI | PCI_IRQ_LEGACY);
	 *          if (ret < 0) return ret;
	 *      }
	 *      pfdev->num_vectors = ret;
	 *
	 *   2. Get IRQ numbers:
	 *      pfdev->irq_watermark = pci_irq_vector(pdev, 0);
	 *      if (pfdev->num_vectors > 1)
	 *          pfdev->irq_overrun = pci_irq_vector(pdev, 1);
	 *
	 *   3. Request IRQs:
	 *      ret = request_irq(pfdev->irq_watermark, pfpga_irq_watermark,
	 *                        0, DRIVER_NAME "-watermark", pfdev);
	 *      if (ret) goto err_free_vectors;
	 *
	 *      if (pfdev->num_vectors > 1) {
	 *          ret = request_irq(pfdev->irq_overrun, pfpga_irq_overrun,
	 *                            0, DRIVER_NAME "-overrun", pfdev);
	 *          if (ret) goto err_free_watermark_irq;
	 *      }
	 *
	 *   4. Return 0 on success
	 *
	 * Note: Error handling should clean up partial allocations!
	 */

	(void)pdev;
	(void)ret;
	pfdev->num_vectors = 0;
	pfdev->irq_watermark = -1;
	pfdev->irq_overrun = -1;

	dev_info(&pdev->dev, "MSI-X setup skipped (TODO)\n");
	return 0;  /* Succeed for now so probe completes */
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
	 *      if (pfdev->irq_watermark >= 0)
	 *          free_irq(pfdev->irq_watermark, pfdev);
	 *      if (pfdev->irq_overrun >= 0)
	 *          free_irq(pfdev->irq_overrun, pfdev);
	 *
	 *   2. Free vectors:
	 *      if (pfdev->num_vectors > 0)
	 *          pci_free_irq_vectors(pfdev->pdev);
	 */
}

/*
 * Allocate DMA buffer.
 * Called during probe with default size, resized in SET_CFG if needed.
 */
static int pfpga_alloc_dma_buffer(struct phantomfpga_dev *pfdev, size_t size)
{
	/*
	 * TODO: Allocate coherent DMA buffer
	 *
	 * Steps:
	 *   1. Free existing buffer if any:
	 *      if (pfdev->dma_buf) {
	 *          dma_free_coherent(&pfdev->pdev->dev, pfdev->dma_size,
	 *                            pfdev->dma_buf, pfdev->dma_handle);
	 *      }
	 *
	 *   2. Allocate new buffer:
	 *      pfdev->dma_buf = dma_alloc_coherent(&pfdev->pdev->dev, size,
	 *                                          &pfdev->dma_handle, GFP_KERNEL);
	 *      if (!pfdev->dma_buf)
	 *          return -ENOMEM;
	 *
	 *   3. Store size and return:
	 *      pfdev->dma_size = size;
	 *      return 0;
	 *
	 * Note: dma_alloc_coherent returns:
	 *   - Virtual address (for driver access)
	 *   - DMA handle (physical address for device)
	 */

	(void)size;
	pfdev->dma_buf = NULL;
	pfdev->dma_handle = 0;
	pfdev->dma_size = 0;

	dev_info(&pfdev->pdev->dev, "DMA buffer allocation skipped (TODO)\n");
	return 0;
}

/*
 * Free DMA buffer.
 */
static void pfpga_free_dma_buffer(struct phantomfpga_dev *pfdev)
{
	/*
	 * TODO: Free DMA buffer
	 *
	 * if (pfdev->dma_buf) {
	 *     dma_free_coherent(&pfdev->pdev->dev, pfdev->dma_size,
	 *                       pfdev->dma_buf, pfdev->dma_handle);
	 *     pfdev->dma_buf = NULL;
	 *     pfdev->dma_handle = 0;
	 *     pfdev->dma_size = 0;
	 * }
	 */
}

/*
 * Create character device.
 */
static int pfpga_create_cdev(struct phantomfpga_dev *pfdev)
{
	int minor;
	int ret;

	/* Allocate minor number */
	minor = ida_alloc_max(&phantomfpga_ida, PHANTOMFPGA_MAX_DEVICES - 1,
			      GFP_KERNEL);
	if (minor < 0)
		return minor;

	pfdev->minor = minor;
	pfdev->devno = MKDEV(MAJOR(phantomfpga_devno), minor);

	/* Initialize and add cdev */
	cdev_init(&pfdev->cdev, &phantomfpga_fops);
	pfdev->cdev.owner = THIS_MODULE;

	ret = cdev_add(&pfdev->cdev, pfdev->devno, 1);
	if (ret)
		goto err_ida;

	/* Create device node in /dev */
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
 *
 * This is where all device initialization happens.
 */
static int phantomfpga_probe(struct pci_dev *pdev,
			     const struct pci_device_id *id)
{
	struct phantomfpga_dev *pfdev;
	u32 dev_id;
	int ret;

	dev_info(&pdev->dev, "probing PhantomFPGA device\n");

	/* Allocate device private data */
	pfdev = kzalloc(sizeof(*pfdev), GFP_KERNEL);
	if (!pfdev)
		return -ENOMEM;

	pfdev->pdev = pdev;
	spin_lock_init(&pfdev->lock);
	mutex_init(&pfdev->ioctl_lock);
	init_waitqueue_head(&pfdev->wait_queue);

	/* Store in PCI device for later retrieval */
	pci_set_drvdata(pdev, pfdev);

	/*
	 * TODO: Enable PCI device
	 *
	 * Steps:
	 *   ret = pci_enable_device(pdev);
	 *   if (ret) {
	 *       dev_err(&pdev->dev, "failed to enable PCI device\n");
	 *       goto err_free;
	 *   }
	 *
	 * This wakes up the device and enables I/O and memory access.
	 */
	ret = pci_enable_device(pdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to enable PCI device\n");
		goto err_free;
	}

	/*
	 * TODO: Request BAR0 region
	 *
	 * Steps:
	 *   ret = pci_request_region(pdev, 0, DRIVER_NAME);
	 *   if (ret) {
	 *       dev_err(&pdev->dev, "failed to request BAR0\n");
	 *       goto err_disable;
	 *   }
	 *
	 * This reserves the memory region so other drivers don't use it.
	 */
	ret = pci_request_region(pdev, 0, DRIVER_NAME);
	if (ret) {
		dev_err(&pdev->dev, "failed to request BAR0\n");
		goto err_disable;
	}

	/*
	 * TODO: Map BAR0 to kernel virtual address
	 *
	 * Steps:
	 *   pfdev->regs_start = pci_resource_start(pdev, 0);
	 *   pfdev->regs_len = pci_resource_len(pdev, 0);
	 *   pfdev->regs = pci_iomap(pdev, 0, pfdev->regs_len);
	 *   if (!pfdev->regs) {
	 *       dev_err(&pdev->dev, "failed to map BAR0\n");
	 *       ret = -ENOMEM;
	 *       goto err_release;
	 *   }
	 *
	 * Now we can access device registers via pfdev->regs!
	 */
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

	/*
	 * TODO: Verify device identity
	 *
	 * Steps:
	 *   dev_id = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_ID);
	 *   if (dev_id != PHANTOMFPGA_DEV_ID_VAL) {
	 *       dev_err(&pdev->dev, "unexpected device ID: 0x%08x\n", dev_id);
	 *       ret = -ENODEV;
	 *       goto err_unmap;
	 *   }
	 *   dev_info(&pdev->dev, "device ID verified: 0x%08x\n", dev_id);
	 *
	 * This confirms we're talking to a real PhantomFPGA device.
	 */
	dev_id = pfpga_read32(pfdev, PHANTOMFPGA_REG_DEV_ID);
	if (dev_id != PHANTOMFPGA_DEV_ID_VAL) {
		dev_err(&pdev->dev, "unexpected device ID: 0x%08x (expected 0x%08x)\n",
			dev_id, PHANTOMFPGA_DEV_ID_VAL);
		ret = -ENODEV;
		goto err_unmap;
	}
	dev_info(&pdev->dev, "device ID verified: 0x%08x\n", dev_id);

	/*
	 * TODO: Enable bus mastering for DMA
	 *
	 * Steps:
	 *   pci_set_master(pdev);
	 *
	 * Required for device to perform DMA writes to system memory.
	 */
	pci_set_master(pdev);

	/*
	 * TODO: Set DMA mask
	 *
	 * Steps:
	 *   ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	 *   if (ret) {
	 *       ret = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
	 *       if (ret) {
	 *           dev_err(&pdev->dev, "failed to set DMA mask\n");
	 *           goto err_unmap;
	 *       }
	 *   }
	 *
	 * Try 64-bit DMA first, fall back to 32-bit.
	 */
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

	/* Allocate initial DMA buffer (will be resized on SET_CFG) */
	ret = pfpga_alloc_dma_buffer(pfdev,
				     PHANTOMFPGA_DEFAULT_FRAME_SIZE *
				     PHANTOMFPGA_DEFAULT_RING_SIZE);
	if (ret) {
		dev_err(&pdev->dev, "failed to allocate DMA buffer: %d\n", ret);
		goto err_msix;
	}

	/* Perform soft reset to put device in known state */
	pfpga_soft_reset(pfdev);

	/* Create character device */
	ret = pfpga_create_cdev(pfdev);
	if (ret) {
		dev_err(&pdev->dev, "failed to create char device: %d\n", ret);
		goto err_dma;
	}

	/* Set default configuration (not yet applied to device) */
	pfdev->frame_size = PHANTOMFPGA_DEFAULT_FRAME_SIZE;
	pfdev->frame_rate = PHANTOMFPGA_DEFAULT_FRAME_RATE;
	pfdev->ring_size = PHANTOMFPGA_DEFAULT_RING_SIZE;
	pfdev->watermark = PHANTOMFPGA_DEFAULT_WATERMARK;
	pfdev->configured = false;
	pfdev->streaming = false;

	dev_info(&pdev->dev, "PhantomFPGA driver loaded successfully\n");
	return 0;

err_dma:
	pfpga_free_dma_buffer(pfdev);
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
 *
 * Must release all resources in reverse order of acquisition.
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

	/* Free DMA buffer */
	pfpga_free_dma_buffer(pfdev);

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

/*
 * Module initialization.
 *
 * Called when driver is loaded (insmod/modprobe).
 */
static int __init phantomfpga_init(void)
{
	int ret;

	pr_info("PhantomFPGA driver initializing\n");

	/* Allocate character device numbers */
	ret = alloc_chrdev_region(&phantomfpga_devno, 0, PHANTOMFPGA_MAX_DEVICES,
				  DRIVER_NAME);
	if (ret) {
		pr_err("failed to allocate chrdev region: %d\n", ret);
		return ret;
	}

	/* Create device class for udev */
	phantomfpga_class = class_create(DRIVER_NAME);
	if (IS_ERR(phantomfpga_class)) {
		ret = PTR_ERR(phantomfpga_class);
		pr_err("failed to create device class: %d\n", ret);
		goto err_chrdev;
	}

	/* Register PCI driver */
	ret = pci_register_driver(&phantomfpga_pci_driver);
	if (ret) {
		pr_err("failed to register PCI driver: %d\n", ret);
		goto err_class;
	}

	pr_info("PhantomFPGA driver initialized (major=%d)\n",
		MAJOR(phantomfpga_devno));
	return 0;

err_class:
	class_destroy(phantomfpga_class);
err_chrdev:
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);
	return ret;
}

/*
 * Module cleanup.
 *
 * Called when driver is unloaded (rmmod).
 */
static void __exit phantomfpga_exit(void)
{
	pr_info("PhantomFPGA driver exiting\n");

	pci_unregister_driver(&phantomfpga_pci_driver);
	class_destroy(phantomfpga_class);
	unregister_chrdev_region(phantomfpga_devno, PHANTOMFPGA_MAX_DEVICES);

	pr_info("PhantomFPGA driver unloaded\n");
}

module_init(phantomfpga_init);
module_exit(phantomfpga_exit);
