# PhantomFPGA Driver Development Guide

> "The best time to write a kernel driver was 10 years ago.
> The second best time is now, with this guide in front of you."

Welcome to the fun part! This guide walks you through implementing your very own
Linux kernel driver. By the end, you'll have a working driver that talks to our
fake FPGA, and you'll understand why kernel developers drink so much coffee.

Don't worry if you get stuck - that's normal. Kernel development has a learning
curve shaped like a brick wall. We'll help you climb it.

## Prerequisites

Before starting, make sure you:

1. Have read [architecture.md](architecture.md) for system overview
2. Have read [phantomfpga-datasheet.md](phantomfpga-datasheet.md) for device operation and registers
3. Have a working build environment (QEMU + guest VM)
4. Understand basic C programming and Linux kernel concepts

**Recommended background reading:**
- Linux Device Drivers, 3rd Edition (freely available online)
- kernel.org documentation on PCI, DMA, and interrupts
- `Documentation/driver-api/` in the kernel source

## The Skeleton Driver

The skeleton driver at `driver/phantomfpga_drv.c` provides:

- Complete module structure (init/exit, probe/remove)
- Data structure definitions
- Function signatures with TODO comments
- Working parts (PCI enable, BAR mapping, device verification)

Your job is to complete the TODOs to make it fully functional.

## Development Workflow

1. **Build the driver** in the guest:
   ```bash
   cd /mnt/driver
   make
   ```

2. **Load the driver**:
   ```bash
   insmod phantomfpga.ko
   dmesg | tail -20
   ```

3. **Test your changes** with the app or manually

4. **Unload and iterate**:
   ```bash
   rmmod phantomfpga
   make clean && make
   insmod phantomfpga.ko
   ```

**Pro tip:** Keep a second terminal with `dmesg -w` running to see kernel
messages in real-time.

---

## Part 1: DMA Buffer Allocation

**Goal:** Allocate coherent DMA memory for the ring buffer.

### Background

DMA (Direct Memory Access) allows the device to read/write system memory
without CPU involvement. The PhantomFPGA device writes frames directly to
a ring buffer in guest memory.

For DMA to work, you need:
1. **Physically contiguous memory** - the device sees physical addresses
2. **Cache coherency** - CPU and device see the same data
3. **Both addresses** - virtual (for driver) and physical (for device)

### Implementation

Find `pfpga_alloc_dma_buffer()` in the skeleton and implement:

```c
static int pfpga_alloc_dma_buffer(struct phantomfpga_dev *pfdev, size_t size)
{
    /* Free existing buffer if any */
    if (pfdev->dma_buf) {
        dma_free_coherent(&pfdev->pdev->dev, pfdev->dma_size,
                          pfdev->dma_buf, pfdev->dma_handle);
    }

    /* Allocate new coherent DMA buffer */
    pfdev->dma_buf = dma_alloc_coherent(&pfdev->pdev->dev, size,
                                        &pfdev->dma_handle, GFP_KERNEL);
    if (!pfdev->dma_buf) {
        dev_err(&pfdev->pdev->dev, "failed to allocate %zu bytes DMA buffer\n",
                size);
        return -ENOMEM;
    }

    pfdev->dma_size = size;

    dev_info(&pfdev->pdev->dev, "DMA buffer allocated: %zu bytes at phys 0x%llx\n",
             size, (unsigned long long)pfdev->dma_handle);

    return 0;
}
```

Also implement `pfpga_free_dma_buffer()`:

```c
static void pfpga_free_dma_buffer(struct phantomfpga_dev *pfdev)
{
    if (pfdev->dma_buf) {
        dma_free_coherent(&pfdev->pdev->dev, pfdev->dma_size,
                          pfdev->dma_buf, pfdev->dma_handle);
        pfdev->dma_buf = NULL;
        pfdev->dma_handle = 0;
        pfdev->dma_size = 0;
    }
}
```

### Key Points

- `dma_alloc_coherent()` returns a virtual address for driver use
- `pfdev->dma_handle` is the physical/bus address for the device
- Use `GFP_KERNEL` - we can sleep during allocation
- Always check for NULL return (allocation failure)
- Balance every alloc with a free on error paths and remove

### Verification

After loading the driver:
```bash
dmesg | grep "DMA buffer"
# Should show allocation message with address
```

---

## Part 2: Tell the Device About DMA

**Goal:** Program the device's DMA address registers.

### Implementation

Find `pfpga_configure_dma()` and implement:

```c
static void pfpga_configure_dma(struct phantomfpga_dev *pfdev)
{
    /* Split 64-bit DMA address into two 32-bit writes */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_DMA_ADDR_LO,
                  lower_32_bits(pfdev->dma_handle));
    pfpga_write32(pfdev, PHANTOMFPGA_REG_DMA_ADDR_HI,
                  upper_32_bits(pfdev->dma_handle));

    /* Tell device the total buffer size */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_DMA_SIZE, pfdev->dma_size);

    dev_dbg(&pfdev->pdev->dev, "DMA configured: addr=0x%llx size=%u\n",
            (unsigned long long)pfdev->dma_handle, pfdev->dma_size);
}
```

### Key Points

- 64-bit addresses need two 32-bit writes (many older devices work this way)
- `lower_32_bits()` and `upper_32_bits()` are kernel helpers
- Call this after allocating the DMA buffer

---

## Part 3: Apply Configuration

**Goal:** Write frame parameters to device registers.

### Implementation

Find `pfpga_apply_config()` and implement:

```c
static void pfpga_apply_config(struct phantomfpga_dev *pfdev)
{
    /* Write configuration registers */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_FRAME_SIZE, pfdev->frame_size);
    pfpga_write32(pfdev, PHANTOMFPGA_REG_FRAME_RATE, pfdev->frame_rate);
    pfpga_write32(pfdev, PHANTOMFPGA_REG_WATERMARK, pfdev->watermark);
    pfpga_write32(pfdev, PHANTOMFPGA_REG_RING_SIZE, pfdev->ring_size);

    /* Reset consumer index */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CONS_IDX, 0);

    /* Enable watermark interrupt */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_MASK, PHANTOMFPGA_IRQ_WATERMARK);
}
```

### Key Points

- Only apply config when device is stopped
- Reset consumer index to sync with device
- Enable the interrupts you want to receive

---

## Part 4: Soft Reset

**Goal:** Implement device reset for clean initialization.

### Implementation

Find `pfpga_soft_reset()` and implement:

```c
static void pfpga_soft_reset(struct phantomfpga_dev *pfdev)
{
    /* Trigger soft reset */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL, PHANTOMFPGA_CTRL_RESET);

    /* Reset bit is self-clearing, wait briefly */
    udelay(10);

    /* Reset local state */
    pfdev->streaming = false;
    pfdev->prod_idx = 0;
    pfdev->cons_idx = 0;

    dev_dbg(&pfdev->pdev->dev, "device reset complete\n");
}
```

### Key Points

- The RESET bit clears itself after the reset completes
- A small delay ensures the reset has time to complete
- Reset your driver's cached state too

---

## Part 5: Start/Stop Streaming

**Goal:** Control frame production.

### Start Implementation

Find `pfpga_start_streaming()`:

```c
static int pfpga_start_streaming(struct phantomfpga_dev *pfdev)
{
    u32 ctrl;

    /* Validate state */
    if (!pfdev->configured) {
        dev_err(&pfdev->pdev->dev, "cannot start: not configured\n");
        return -EINVAL;
    }

    if (pfdev->streaming) {
        dev_warn(&pfdev->pdev->dev, "already streaming\n");
        return -EBUSY;
    }

    /* Reset indices */
    pfdev->prod_idx = 0;
    pfdev->cons_idx = 0;
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CONS_IDX, 0);

    /* Clear any pending interrupts */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS, PHANTOMFPGA_IRQ_ALL);

    /* Start device with interrupts enabled */
    ctrl = PHANTOMFPGA_CTRL_START | PHANTOMFPGA_CTRL_IRQ_EN;
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL, ctrl);

    pfdev->streaming = true;

    dev_info(&pfdev->pdev->dev, "streaming started\n");
    return 0;
}
```

### Stop Implementation

Find `pfpga_stop_streaming()`:

```c
static int pfpga_stop_streaming(struct phantomfpga_dev *pfdev)
{
    u32 ctrl;

    /* Read current control value and clear START bit */
    ctrl = pfpga_read32(pfdev, PHANTOMFPGA_REG_CTRL);
    ctrl &= ~PHANTOMFPGA_CTRL_START;
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL, ctrl);

    pfdev->streaming = false;

    /* Wake up any waiters - they'll see streaming=false */
    wake_up_interruptible(&pfdev->wait_queue);

    dev_info(&pfdev->pdev->dev, "streaming stopped\n");
    return 0;
}
```

### Key Points

- Validate state before starting
- Clear pending IRQs before enabling new ones
- Wake waiters on stop so they can exit gracefully

---

## Part 6: MSI-X Interrupt Setup

**Goal:** Set up interrupt handlers for device notifications.

### Implementation

Find `pfpga_setup_msix()`:

```c
static int pfpga_setup_msix(struct phantomfpga_dev *pfdev)
{
    struct pci_dev *pdev = pfdev->pdev;
    int ret;

    /* Request MSI-X vectors */
    ret = pci_alloc_irq_vectors(pdev, PHANTOMFPGA_MSIX_VECTORS,
                                PHANTOMFPGA_MSIX_VECTORS, PCI_IRQ_MSIX);
    if (ret < 0) {
        /* Fall back to single MSI or legacy */
        dev_warn(&pdev->dev, "MSI-X not available, trying MSI\n");
        ret = pci_alloc_irq_vectors(pdev, 1, 1,
                                    PCI_IRQ_MSI | PCI_IRQ_LEGACY);
        if (ret < 0) {
            dev_err(&pdev->dev, "failed to allocate IRQ vectors: %d\n", ret);
            return ret;
        }
    }

    pfdev->num_vectors = ret;

    /* Get IRQ numbers */
    pfdev->irq_watermark = pci_irq_vector(pdev, 0);
    if (pfdev->num_vectors > 1) {
        pfdev->irq_overrun = pci_irq_vector(pdev, 1);
    } else {
        pfdev->irq_overrun = -1;  /* Shared with watermark */
    }

    /* Request watermark IRQ */
    ret = request_irq(pfdev->irq_watermark, pfpga_irq_watermark,
                      0, DRIVER_NAME "-watermark", pfdev);
    if (ret) {
        dev_err(&pdev->dev, "failed to request watermark IRQ: %d\n", ret);
        goto err_free_vectors;
    }

    /* Request overrun IRQ if separate */
    if (pfdev->irq_overrun >= 0) {
        ret = request_irq(pfdev->irq_overrun, pfpga_irq_overrun,
                          0, DRIVER_NAME "-overrun", pfdev);
        if (ret) {
            dev_err(&pdev->dev, "failed to request overrun IRQ: %d\n", ret);
            goto err_free_watermark;
        }
    }

    dev_info(&pdev->dev, "MSI-X setup complete: %d vectors\n",
             pfdev->num_vectors);
    return 0;

err_free_watermark:
    free_irq(pfdev->irq_watermark, pfdev);
err_free_vectors:
    pci_free_irq_vectors(pdev);
    pfdev->num_vectors = 0;
    return ret;
}
```

Don't forget `pfpga_teardown_msix()`:

```c
static void pfpga_teardown_msix(struct phantomfpga_dev *pfdev)
{
    if (pfdev->irq_overrun >= 0)
        free_irq(pfdev->irq_overrun, pfdev);

    if (pfdev->irq_watermark >= 0)
        free_irq(pfdev->irq_watermark, pfdev);

    if (pfdev->num_vectors > 0)
        pci_free_irq_vectors(pfdev->pdev);

    pfdev->num_vectors = 0;
    pfdev->irq_watermark = -1;
    pfdev->irq_overrun = -1;
}
```

### Key Points

- Try MSI-X first, fall back to MSI or legacy
- Each vector gets its own handler
- Match every `request_irq()` with `free_irq()`
- Error paths must clean up partial allocations

---

## Part 7: Interrupt Handlers

**Goal:** Handle device interrupts to wake waiting processes.

### Watermark Handler

Find `pfpga_irq_watermark()`:

```c
static irqreturn_t pfpga_irq_watermark(int irq, void *data)
{
    struct phantomfpga_dev *pfdev = data;
    u32 irq_status;
    unsigned long flags;

    /* Read and check interrupt status */
    irq_status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

    if (!(irq_status & PHANTOMFPGA_IRQ_WATERMARK))
        return IRQ_NONE;  /* Not our interrupt */

    /* Clear the interrupt (write-1-to-clear) */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS, PHANTOMFPGA_IRQ_WATERMARK);

    /* Update state under lock */
    spin_lock_irqsave(&pfdev->lock, flags);
    pfdev->prod_idx = pfpga_read32(pfdev, PHANTOMFPGA_REG_PROD_IDX);
    pfdev->irq_count++;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    /* Wake up poll/read waiters */
    wake_up_interruptible(&pfdev->wait_queue);

    return IRQ_HANDLED;
}
```

### Overrun Handler

Find `pfpga_irq_overrun()`:

```c
static irqreturn_t pfpga_irq_overrun(int irq, void *data)
{
    struct phantomfpga_dev *pfdev = data;
    u32 irq_status;
    unsigned long flags;

    irq_status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

    if (!(irq_status & PHANTOMFPGA_IRQ_OVERRUN))
        return IRQ_NONE;

    /* Clear the interrupt */
    pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS, PHANTOMFPGA_IRQ_OVERRUN);

    /* Log warning - this means userspace is too slow! */
    dev_warn_ratelimited(&pfdev->pdev->dev, "ring buffer overrun!\n");

    spin_lock_irqsave(&pfdev->lock, flags);
    pfdev->irq_count++;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    /* Still wake waiters - they should check for overrun */
    wake_up_interruptible(&pfdev->wait_queue);

    return IRQ_HANDLED;
}
```

### Key Points

- Check IRQ_STATUS to confirm it's your interrupt
- Clear the interrupt before doing work (W1C semantics)
- Use spinlock to protect shared state
- `IRQ_NONE` if not your interrupt, `IRQ_HANDLED` otherwise
- `dev_warn_ratelimited` prevents log spam

---

## Part 8: Read Operation

**Goal:** Implement frame reading for userspace.

### Implementation

Find `pfpga_read()`:

```c
static ssize_t pfpga_read(struct file *file, char __user *buf,
                          size_t count, loff_t *ppos)
{
    struct phantomfpga_dev *pfdev = file->private_data;
    unsigned long flags;
    u32 prod_idx, cons_idx, pending;
    size_t frame_offset, to_copy;
    void *frame_ptr;
    int ret;

    /* Must be streaming */
    if (!pfdev->streaming)
        return -EINVAL;

    /* Wait for data if blocking */
    if (!(file->f_flags & O_NONBLOCK)) {
        ret = wait_event_interruptible(pfdev->wait_queue,
            !phantomfpga_ring_empty(pfdev->prod_idx, pfdev->cons_idx) ||
            !pfdev->streaming);

        if (ret)
            return ret;  /* Interrupted by signal */

        if (!pfdev->streaming)
            return 0;  /* EOF - device stopped */
    }

    /* Check for available frames */
    spin_lock_irqsave(&pfdev->lock, flags);
    prod_idx = pfdev->prod_idx;
    cons_idx = pfdev->cons_idx;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    pending = phantomfpga_ring_pending(prod_idx, cons_idx, pfdev->ring_size);
    if (pending == 0)
        return -EAGAIN;  /* No data available */

    /* Calculate frame address */
    frame_offset = phantomfpga_frame_offset(cons_idx, pfdev->frame_size);
    frame_ptr = pfdev->dma_buf + frame_offset;

    /* Copy one frame to userspace */
    to_copy = min(count, (size_t)pfdev->frame_size);
    if (copy_to_user(buf, frame_ptr, to_copy))
        return -EFAULT;

    /* Advance consumer index */
    spin_lock_irqsave(&pfdev->lock, flags);
    pfdev->cons_idx = (cons_idx + 1) & (pfdev->ring_size - 1);
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CONS_IDX, pfdev->cons_idx);
    pfdev->frames_consumed++;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    return to_copy;
}
```

### Key Points

- Handle both blocking and non-blocking modes
- `wait_event_interruptible` sleeps until condition is true
- Return -EAGAIN for non-blocking when no data
- `copy_to_user` handles user/kernel memory boundary
- Advance consumer index after successful read

---

## Part 9: Poll Operation

**Goal:** Support select()/poll()/epoll() for async I/O.

### Implementation

Find `pfpga_poll()`:

```c
static __poll_t pfpga_poll(struct file *file, poll_table *wait)
{
    struct phantomfpga_dev *pfdev = file->private_data;
    __poll_t mask = 0;
    unsigned long flags;
    u32 prod_idx, cons_idx;

    /* Register with poll subsystem */
    poll_wait(file, &pfdev->wait_queue, wait);

    /* Check current state */
    spin_lock_irqsave(&pfdev->lock, flags);
    prod_idx = pfdev->prod_idx;
    cons_idx = pfdev->cons_idx;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    /* Data available? */
    if (!phantomfpga_ring_empty(prod_idx, cons_idx))
        mask |= EPOLLIN | EPOLLRDNORM;

    /* Error or stopped? */
    if (!pfdev->streaming)
        mask |= EPOLLHUP;

    return mask;
}
```

### Key Points

- `poll_wait` registers the wait queue - doesn't block!
- Return current state immediately
- EPOLLIN = readable, EPOLLHUP = hung up

---

## Part 10: Memory Mapping

**Goal:** Allow userspace to mmap the DMA buffer for zero-copy access.

### Implementation

Find `pfpga_mmap()`:

```c
static int pfpga_mmap(struct file *file, struct vm_area_struct *vma)
{
    struct phantomfpga_dev *pfdev = file->private_data;
    size_t size = vma->vm_end - vma->vm_start;
    int ret;

    /* Must be configured */
    if (!pfdev->configured) {
        dev_err(&pfdev->pdev->dev, "mmap: device not configured\n");
        return -EINVAL;
    }

    /* Validate size */
    if (size > pfdev->dma_size) {
        dev_err(&pfdev->pdev->dev, "mmap: size %zu > buffer %zu\n",
                size, pfdev->dma_size);
        return -EINVAL;
    }

    /* Only offset 0 is supported */
    if (vma->vm_pgoff != 0) {
        dev_err(&pfdev->pdev->dev, "mmap: non-zero offset not supported\n");
        return -EINVAL;
    }

    /* Set memory type for DMA buffer */
    vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

    /* Map the DMA buffer */
    ret = dma_mmap_coherent(&pfdev->pdev->dev, vma,
                            pfdev->dma_buf, pfdev->dma_handle,
                            pfdev->dma_size);
    if (ret) {
        dev_err(&pfdev->pdev->dev, "dma_mmap_coherent failed: %d\n", ret);
        return ret;
    }

    /* Set flags */
    vm_flags_set(vma, VM_IO | VM_DONTEXPAND | VM_DONTDUMP);

    dev_dbg(&pfdev->pdev->dev, "mmap: %zu bytes mapped\n", size);
    return 0;
}
```

### Key Points

- Validate before mapping (configured, size, offset)
- `pgprot_noncached` ensures CPU sees DMA writes immediately
- `dma_mmap_coherent` is the right way to map DMA buffers
- VM_IO marks as I/O memory, VM_DONTDUMP excludes from core dumps

---

## Part 11: IOCTL Implementation

**Goal:** Implement the control interface.

The skeleton already has the switch structure. Complete each case:

### SET_CFG

```c
case PHANTOMFPGA_IOCTL_SET_CFG:
{
    struct phantomfpga_config cfg;
    size_t required_size;

    /* Cannot configure while streaming */
    if (pfdev->streaming) {
        ret = -EBUSY;
        break;
    }

    /* Copy from userspace */
    if (copy_from_user(&cfg, argp, sizeof(cfg))) {
        ret = -EFAULT;
        break;
    }

    /* Validate parameters */
    if (cfg.frame_size < PHANTOMFPGA_MIN_FRAME_SIZE ||
        cfg.frame_size > PHANTOMFPGA_MAX_FRAME_SIZE) {
        ret = -EINVAL;
        break;
    }
    if (cfg.frame_rate < PHANTOMFPGA_MIN_FRAME_RATE ||
        cfg.frame_rate > PHANTOMFPGA_MAX_FRAME_RATE) {
        ret = -EINVAL;
        break;
    }
    if (cfg.ring_size < PHANTOMFPGA_MIN_RING_SIZE ||
        cfg.ring_size > PHANTOMFPGA_MAX_RING_SIZE) {
        ret = -EINVAL;
        break;
    }
    /* Ring size must be power of 2 */
    if (cfg.ring_size & (cfg.ring_size - 1)) {
        ret = -EINVAL;
        break;
    }
    if (cfg.watermark == 0 || cfg.watermark >= cfg.ring_size) {
        ret = -EINVAL;
        break;
    }

    /* Calculate required buffer size */
    required_size = (size_t)cfg.frame_size * cfg.ring_size;

    /* Reallocate DMA buffer if needed */
    if (required_size != pfdev->dma_size) {
        ret = pfpga_alloc_dma_buffer(pfdev, required_size);
        if (ret)
            break;
    }

    /* Store configuration */
    pfdev->frame_size = cfg.frame_size;
    pfdev->frame_rate = cfg.frame_rate;
    pfdev->ring_size = cfg.ring_size;
    pfdev->watermark = cfg.watermark;

    /* Apply to device */
    pfpga_apply_config(pfdev);
    pfpga_configure_dma(pfdev);

    pfdev->configured = true;
    ret = 0;
}
break;
```

### START and STOP

```c
case PHANTOMFPGA_IOCTL_START:
    ret = pfpga_start_streaming(pfdev);
    break;

case PHANTOMFPGA_IOCTL_STOP:
    ret = pfpga_stop_streaming(pfdev);
    break;
```

### GET_STATS

```c
case PHANTOMFPGA_IOCTL_GET_STATS:
{
    struct phantomfpga_stats stats = {0};
    unsigned long flags;

    /* Read device counters */
    stats.frames_produced = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_FRAMES);
    stats.errors = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_ERRORS);
    stats.overruns = pfpga_read32(pfdev, PHANTOMFPGA_REG_STAT_OVERRUNS);
    stats.prod_idx = pfpga_read32(pfdev, PHANTOMFPGA_REG_PROD_IDX);
    stats.cons_idx = pfpga_read32(pfdev, PHANTOMFPGA_REG_CONS_IDX);
    stats.status = pfpga_read32(pfdev, PHANTOMFPGA_REG_STATUS);

    /* Add driver-side stats */
    spin_lock_irqsave(&pfdev->lock, flags);
    stats.frames_consumed = pfdev->frames_consumed;
    stats.irq_count = pfdev->irq_count;
    spin_unlock_irqrestore(&pfdev->lock, flags);

    if (copy_to_user(argp, &stats, sizeof(stats)))
        ret = -EFAULT;
}
break;
```

### CONSUME_FRAME

```c
case PHANTOMFPGA_IOCTL_CONSUME_FRAME:
{
    unsigned long flags;

    spin_lock_irqsave(&pfdev->lock, flags);

    /* Check if frames available */
    if (phantomfpga_ring_empty(pfdev->prod_idx, pfdev->cons_idx)) {
        spin_unlock_irqrestore(&pfdev->lock, flags);
        ret = -EAGAIN;
        break;
    }

    /* Advance consumer index */
    pfdev->cons_idx = (pfdev->cons_idx + 1) & (pfdev->ring_size - 1);
    pfpga_write32(pfdev, PHANTOMFPGA_REG_CONS_IDX, pfdev->cons_idx);
    pfdev->frames_consumed++;

    spin_unlock_irqrestore(&pfdev->lock, flags);
    ret = 0;
}
break;
```

---

## Common Pitfalls

### 1. Forgetting Locking

**Wrong:**
```c
pfdev->cons_idx++;  /* Unsafe! IRQ can run here */
pfpga_write32(pfdev, REG_CONS_IDX, pfdev->cons_idx);
```

**Right:**
```c
spin_lock_irqsave(&pfdev->lock, flags);
pfdev->cons_idx++;
pfpga_write32(pfdev, REG_CONS_IDX, pfdev->cons_idx);
spin_unlock_irqrestore(&pfdev->lock, flags);
```

### 2. Holding Spinlock Too Long

**Wrong:**
```c
spin_lock(&pfdev->lock);
copy_to_user(buf, data, len);  /* Can sleep! */
spin_unlock(&pfdev->lock);
```

**Right:**
```c
spin_lock_irqsave(&pfdev->lock, flags);
memcpy(local_buf, data, len);  /* Copy to local buffer */
spin_unlock_irqrestore(&pfdev->lock, flags);
copy_to_user(buf, local_buf, len);  /* Now safe to sleep */
```

### 3. Not Checking Return Values

**Wrong:**
```c
dma_alloc_coherent(...);  /* Might return NULL! */
pfdev->dma_buf[0] = 42;   /* Oops, NULL dereference */
```

**Right:**
```c
pfdev->dma_buf = dma_alloc_coherent(...);
if (!pfdev->dma_buf)
    return -ENOMEM;
```

### 4. Wrong Barrier Usage

Memory barriers are usually not needed with coherent DMA and `ioread/iowrite`,
but understand when they might be:

```c
/* Write multiple values that must be seen in order */
pfpga_write32(pfdev, REG_SIZE, size);
wmb();  /* Ensure SIZE is written before ADDR */
pfpga_write32(pfdev, REG_ADDR, addr);
```

### 5. Interrupt Handler Doing Too Much

Keep IRQ handlers minimal - just acknowledge the interrupt and wake waiters.
Heavy processing should be in tasklets, workqueues, or userspace.

---

## Debugging Tips

### Using dmesg

```bash
dmesg -w                        # Watch messages in real-time
dmesg | grep phantomfpga        # Filter for our driver
```

### Adding Debug Prints

```c
dev_dbg(&pfdev->pdev->dev, "starting streaming\n");    /* Debug level */
dev_info(&pfdev->pdev->dev, "device ready\n");         /* Info level */
dev_warn(&pfdev->pdev->dev, "buffer nearly full\n");   /* Warning */
dev_err(&pfdev->pdev->dev, "DMA failed\n");            /* Error */
```

Enable debug messages:
```bash
echo 8 > /proc/sys/kernel/printk  # Show all messages
# Or: add 'dyndbg="module phantomfpga +p"' to kernel command line
```

### Checking Device State

```bash
# View PCI device
lspci -v -s $(lspci | grep 0dad | cut -d' ' -f1)

# View allocated resources
cat /proc/iomem | grep phantomfpga

# View interrupts
cat /proc/interrupts | grep phantomfpga
```

### Using GDB

With `run_qemu.sh --debug`:
```bash
# On host
gdb vmlinux
(gdb) target remote :1234
(gdb) break phantomfpga_probe
(gdb) continue
```

---

## Testing Your Implementation

### Basic Functionality

```bash
# Load driver
insmod phantomfpga.ko
dmesg | tail

# Verify device node
ls -la /dev/phantomfpga0

# Build and run test app
cd /mnt/app/build && cmake .. && make
./phantomfpga_app --rate 1000 --size 256 --watermark 16
```

### Stress Testing

```bash
# High rate
./phantomfpga_app --rate 10000 --size 64 --watermark 8

# Large frames
./phantomfpga_app --rate 100 --size 65536 --watermark 4

# Watch for overruns
dmesg | grep overrun
```

### Integration Tests

```bash
cd /mnt/tests/integration
./run_all.sh
```

---

## Next Steps

Once your driver is working:

1. **Implement the userspace app** - Complete the TODOs in `app/phantomfpga_app.c`
2. **Add error handling** - Test with fault injection
3. **Optimize performance** - Profile with `perf`
4. **Add sysfs interface** - Expose configuration via /sys

Good luck, and may your buffers never overflow!
