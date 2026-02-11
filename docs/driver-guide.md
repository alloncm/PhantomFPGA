# PhantomFPGA driver development guide

> "The best time to write a kernel driver was 10 years ago. The second best time is now, with this guide in front of you."

Welcome to the fun part! This guide walks you through implementing your very own Linux kernel driver. By the end, you'll have a working driver that talks to our fake FPGA, and you'll understand why kernel developers drink so much coffee.

Don't worry if you get stuck - that's normal. Kernel development has a learning curve shaped like a brick wall. We'll help you climb it.

## Prerequisites

Before starting, make sure you:

1. Have read [architecture.md](architecture.md) for system overview
2. Have read [phantomfpga-datasheet.md](phantomfpga-datasheet.md) for device operation and registers
3. Have a working build environment (QEMU + guest VM)
4. Understand basic C programming and Linux kernel concepts
5. Got 5 liters of coffee handy

**Recommended background reading:**
- Linux Device Drivers, 3rd Edition (freely available online)
- kernel.org documentation on PCI, DMA, and interrupts
- `Documentation/driver-api/` in the kernel source
- Come on, at least pretend you will look into those...

## The skeleton driver

The skeleton driver at `driver/phantomfpga_drv.c` provides:

- Complete module structure (init/exit, probe/remove)
- Data structure definitions
- Function signatures with detailed TODO comments
- Working parts (PCI enable, BAR mapping, device verification, chardev creation)

Your job is to complete the TODOs to make it fully functional. Each TODO has step-by-step instructions -- this guide explains the concepts behind those steps.

## Development workflow

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

**Pro tip:** Keep a second terminal with `dmesg -w` running to see kernel messages in real-time.

### One step at a time

Kernel development rewards patience and punishes ambition. Don't implement three parts at once and then wonder which one broke things. The cycle is:

1. Implement one thing
2. Build, load, verify it works
3. Celebrate (optional but recommended)
4. Move to the next thing

If step 2 fails, you know exactly where to look. If you skipped ahead and implemented five things, well... enjoy your afternoon.

> [!NOTE]
> **On the eternal tabs vs spaces debate:** This is kernel code. Tabs won. The Linux kernel coding style mandates hard tabs for indentation, and arguing about it is about as productive as explaining to your cat why it shouldn't sit on your keyboard. If seeing tabs in source files makes you uncomfortable, consider it exposure therapy. You'll survive. Probably.

---

## Part 1: Descriptor ring and buffer allocation

**Goal:** Allocate coherent DMA memory for the SG-DMA descriptor ring and per-descriptor frame buffers.

**Skeleton function:** `pfpga_alloc_descriptors()`

### Background

The v3 device uses scatter-gather DMA. Instead of one big contiguous buffer, you provide a ring of descriptors, each pointing to its own buffer. The device reads descriptors to find out where to write frames.

You need to allocate two types of DMA memory:

1. **The descriptor ring** -- an array of `struct phantomfpga_sg_desc` entries. The device reads these to find buffer addresses.
2. **Per-descriptor buffers** -- one buffer per descriptor where the device writes frame data + completion status.

Both need to be allocated with `dma_alloc_coherent()` because the device accesses them via DMA. This gives you both a kernel virtual address (for your code) and a DMA/physical address (for the device).

### Things to think about

- Each buffer must fit a full frame (5120 bytes) plus the completion writeback (16 bytes).
- The descriptor ring is a contiguous array -- one `dma_alloc_coherent()` call.
- The per-descriptor buffers are individual allocations -- loop over `desc_count`.
- If any allocation fails midway, you need to clean up everything you already allocated. The classic goto-based error unwinding pattern is your friend here.
- Don't forget the corresponding `pfpga_free_descriptors()` -- balance every alloc with a free.

### Verification

After loading the driver, you should see allocation messages in dmesg:
```bash
dmesg | grep phantomfpga
```

Also `rmmod` and re-`insmod` a couple of times. Watch dmesg for any warnings about leaked memory or double frees. Getting alloc/free right before moving on saves you from mysterious crashes later.

---

## Part 2: Descriptor ring setup

**Goal:** Tell the device where the descriptor ring lives in memory and initialize the descriptors.

**Skeleton functions:** `pfpga_configure_desc_ring()`, `pfpga_init_descriptors()`

### Background

The device needs three things from you before it can use the descriptor ring:

1. The physical address of the ring (64-bit, split across two 32-bit registers)
2. The ring size (number of descriptors)
3. Each descriptor populated with its buffer's DMA address and size

After initialization, you submit the descriptors by writing the HEAD register. This tells the device "I've prepared descriptors up to this index, go ahead and use them."

### Things to think about

- The ring address is 64 bits but the registers are 32 bits each. Use `lower_32_bits()` and `upper_32_bits()`.
- When initializing descriptors, clear the control field (especially the COMPLETED flag) and set the buffer address and length.
- Leave one descriptor slot empty -- it's how you distinguish a full ring from an empty one.
- The `pfpga_submit_descriptors()` function updates HEAD. You'll call it here and again later when recycling completed descriptors.

### Verify before moving on

After programming the ring, read back the registers with `devmem` to confirm the device actually has the right address and size. A wrong address here means silent DMA corruption later -- much harder to debug.

```bash
devmem $((0x${BAR0} + 0x020))   # DESC_RING_LO
devmem $((0x${BAR0} + 0x024))   # DESC_RING_HI
devmem $((0x${BAR0} + 0x028))   # DESC_RING_SIZE
```

---

## Part 3: Configuration and start/stop

**Goal:** Apply frame streaming configuration and control transmission.

**Skeleton functions:** `pfpga_apply_config()`, `pfpga_start_streaming()`, `pfpga_stop_streaming()`

### Background

The device has configurable frame rate and IRQ coalescing. Configuration happens via the SET_CFG ioctl, which validates parameters, allocates resources, and writes registers.

Starting the device means setting the CTRL.RUN and CTRL.IRQ_EN bits. Stopping means clearing CTRL.RUN.

### Things to think about

- Frame size is fixed at 5120 bytes (read-only register). Don't try to configure it.
- Frame rate is the main knob: 1-60 fps.
- IRQ coalescing is a packed register: count in the low 16 bits, timeout in the high 16 bits. There's a helper function `phantomfpga_irq_coalesce_pack()` in the register header.
- Before starting, clear any stale IRQ_STATUS bits and reset the ring indices.
- When stopping, wake up any processes sleeping in poll/read -- they need to know streaming ended.
- The `pfpga_soft_reset()` function is similar but more aggressive: it resets everything back to defaults. Remember that reset clears the descriptor ring address too, so you'd need to reprogram it.

### Verify before moving on

Test start/stop in isolation before adding interrupts. After `pfpga_start_streaming()`, read the STATUS register -- it should show the device as running. After stop, it should be idle again. If this doesn't work, interrupts won't either, and you'd be debugging the wrong layer.

---

## Part 4: MSI-X interrupt setup

**Goal:** Allocate and connect the three MSI-X interrupt vectors.

**Skeleton function:** `pfpga_setup_msix()`, `pfpga_teardown_msix()`

### Background

The device has 3 MSI-X vectors:

| Vector | Handler | Purpose |
|--------|---------|---------|
| 0 | Complete | Descriptors completed (coalescing threshold met) |
| 1 | Error | DMA or device error |
| 2 | No_desc | No descriptors available, frame dropped |

MSI-X gives each interrupt its own vector, so the handler immediately knows *why* it was called without reading a status register. Though you should still read IRQ_STATUS to confirm and clear the interrupt (W1C).

### Things to think about

- Request all 3 vectors with `pci_alloc_irq_vectors()`. If MSI-X isn't available, fall back to MSI or legacy (fewer vectors, shared handler).
- Use `pci_irq_vector()` to get the actual IRQ number for each vector.
- Each vector gets its own `request_irq()` call with its own handler function.
- Error paths are tricky here: if the third `request_irq()` fails, you need to `free_irq()` the first two and `pci_free_irq_vectors()`. The skeleton's error labels handle this.
- Teardown is the reverse: free IRQs first, then free vectors.

### Verify before moving on

After setup, check `/proc/interrupts` -- you should see three `phantomfpga` entries. If you see zero, MSI-X allocation failed silently. If you see one, you got legacy mode. Sort this out before writing the actual handlers.

---

## Part 5: Interrupt handlers

**Goal:** Handle completion, error, and no-descriptor interrupts.

**Skeleton functions:** `pfpga_irq_complete()`, `pfpga_irq_error()`, `pfpga_irq_no_desc()`

### Background

The completion handler is the hot path -- it runs every time the coalescing threshold triggers. It needs to:

1. Confirm the interrupt (read IRQ_STATUS)
2. Clear it (write back to IRQ_STATUS -- W1C semantics)
3. Update the driver's view of which descriptors are done
4. Wake up anyone waiting for data

The error and no_desc handlers are simpler -- log, clear, wake waiters.

### Things to think about

- You're in hard IRQ context. No sleeping, no `mutex_lock`, no `copy_to_user`. Keep it fast.
- Use `spin_lock()` (not `spin_lock_irqsave()`) in the IRQ handler -- you're already interrupt-disabled.
- The device updates DESC_TAIL when it completes descriptors. Read it and store it in `shadow_tail` so the process context (read/poll) can see what's available.
- Return `IRQ_HANDLED` if you processed the interrupt, `IRQ_NONE` if it wasn't yours.
- `dev_warn_ratelimited()` is your friend for the error/no_desc handlers -- don't spam the log.

### Verify before moving on

Start with minimal handlers that just log and clear. Start the device, let it run for a second, stop it. You should see interrupt counts going up in `/proc/interrupts` and your log messages in dmesg. If interrupts arrive but your handler doesn't run, something went wrong with `request_irq()`. If the handler runs but the device keeps re-interrupting, you're not clearing IRQ_STATUS properly.

---

## Part 6: Read and poll

**Goal:** Let userspace consume frames via read() and wait for data via poll().

**Skeleton functions:** `pfpga_read()`, `pfpga_poll()`

### Background

The read path is the main way userspace gets frame data. It needs to:

1. Wait for completed descriptors (if blocking mode)
2. Find the next completed descriptor
3. Check the completion status
4. Optionally validate the frame CRC
5. Copy frame data to userspace
6. Reset the descriptor and resubmit it for reuse

The poll path just reports whether there's data available -- it doesn't consume anything.

### Things to think about

- The `consumer` index tracks which descriptor the driver will consume next. It's separate from `desc_tail`/`shadow_tail` (which track what the device has completed).
- For blocking reads, use `wait_event_interruptible()`. The condition should check if consumer has fallen behind shadow_tail.
- Check `file->f_flags & O_NONBLOCK` -- return -EAGAIN if non-blocking and no data.
- After consuming a frame, clear the descriptor's COMPLETED flag and resubmit it by advancing HEAD. This keeps the pipeline full.
- The completion writeback lives at the end of the buffer. The helper `phantomfpga_completion_ptr()` calculates its location.
- CRC validation: the helper functions are in the register header. Using them is optional but recommended -- how else will you know if fault injection is working?
- For poll: `poll_wait()` registers your wait queue, then return EPOLLIN if data is available, EPOLLHUP if not streaming.

### Verify before moving on

Get `read()` working before touching `poll()`. Write a tiny test program (or use the app) that opens the device, starts streaming, and calls `read()` in a loop. Print the first few bytes of each frame. If you see data that looks reasonable, you're in great shape. If you see garbage or zeros, check the completion status field first -- it tells you what the device thinks happened.

---

## Part 7: Memory mapping

**Goal:** Allow userspace to mmap descriptor buffers for zero-copy access.

**Skeleton function:** `pfpga_mmap()`

### Background

When using mmap mode, userspace reads frame data directly from the DMA buffers instead of going through `read()`. It then calls `PHANTOMFPGA_IOCTL_CONSUME_FRAME` to signal that it's done with a frame.

### Things to think about

- Each descriptor buffer was allocated with `dma_alloc_coherent()`, which returns page-aligned memory. You can map them into userspace with `remap_pfn_range()` or `dma_mmap_coherent()`.
- Since you have per-descriptor buffers (not one big contiguous allocation), mapping all of them into a single VMA requires a loop. Think about the stride between buffers in the virtual address space.
- Use `pgprot_noncached()` for the page protection -- CPU must see DMA writes immediately.
- Set `VM_IO | VM_DONTEXPAND | VM_DONTDUMP` flags on the VMA.
- Validate the request: is the device configured? Is the size reasonable? Is the offset zero?

### Verify before moving on

mmap is tricky -- if it works, great. If it doesn't, you might get a SIGBUS or silent data corruption. Test with the viewer or a small program that mmaps, reads a frame, and prints it. Compare the output with what `read()` gives you -- they should be identical.

---

## Part 8: IOCTL implementation

**Goal:** Wire up the control interface.

**Skeleton function:** `pfpga_ioctl()` -- the switch/case structure is already there.

### Background

The ioctl interface is how userspace configures and controls the device. The switch structure and most of the boilerplate are already in the skeleton. The main one to implement is SET_CFG -- the rest are either done (GET_CFG, GET_STATS, RESET_STATS, GET_BUFFER_INFO) or straightforward (START/STOP call existing functions, SET_FAULT writes two registers).

### SET_CFG -- the meaty one

This is where everything comes together. You need to:

1. Validate the request (not streaming, parameters in range, desc_count is power of 2)
2. Allocate/reallocate the descriptor ring and buffers
3. Apply configuration to device registers
4. Program the descriptor ring address
5. Initialize and submit descriptors
6. Mark the device as configured

The validation ranges are defined as constants in the register header (`MIN_FRAME_RATE`, `MAX_FRAME_RATE`, etc.).

### CONSUME_FRAME

Used in mmap mode: advance the consumer index, reset the descriptor, resubmit. Similar to what read() does after copying data, but without the copy.

### SET_FAULT

Write the fault flags and rate to the two fault registers. Straightforward -- but think about whether you want to allow this while streaming.

---

## Common pitfalls

### 1. Forgetting locking

The IRQ handler updates `shadow_tail` while process context reads it. Without a spinlock, you get torn reads on 32-bit platforms or stale values from CPU caches.

Rule of thumb: any field touched by both IRQ and process context needs `spin_lock_irqsave()` in process context and `spin_lock()` in IRQ context.

### 2. Holding spinlock too long

`copy_to_user()` can sleep (page fault). Sleeping with a spinlock held = deadlock. Copy what you need into a local variable under the lock, release, then copy to user.

### 3. Not checking return values

`dma_alloc_coherent()` returns NULL on failure. `copy_from_user()` returns the number of bytes NOT copied (zero = success). `request_irq()` returns negative errno. Check everything.

### 4. Wrong barrier usage

Memory barriers enforce ordering of memory operations. You usually don't need them between `ioread/iowrite` calls (those have implicit barriers), but you DO need one when mixing regular memory writes with MMIO register writes.

The classic case: you write descriptor fields into coherent memory, then update a device register to tell the hardware about them. Without a barrier, the CPU might reorder those writes, and the device would see the updated register before the descriptor data is actually in memory.

```c
/* Without wmb(): device might see new head before descriptor data */
pfdev->desc_ring[i].dst_addr = buffer_dma;  /* regular memory write */
pfdev->desc_ring[i].length = size;          /* regular memory write */
wmb();  /* Ensure descriptor fields hit memory before head update */
pfpga_write32(pfdev, REG_DESC_HEAD, new_head);  /* MMIO write */
```

If you're only doing register-to-register writes, `iowrite32` handles ordering for you -- no explicit barrier needed.

See the Glossary entry for Memory Barrier for more context.

### 5. Interrupt handler doing too much

Keep IRQ handlers minimal - just acknowledge the interrupt, update `shadow_tail`, and wake waiters. Heavy processing (CRC checks, copy_to_user, etc.) belongs in process context.

---

## Debugging tips

### Using dmesg

```bash
dmesg -w                        # Watch messages in real-time
dmesg | grep phantomfpga        # Filter for our driver
```

### Kernel prints -- your best debugging tool

Seriously. GDB and fancy tracers have their place, but for kernel driver development, `dev_info()` and friends will get you through 90% of your bugs. Don't be shy about adding them -- you can always remove them later.

**The `dev_*` family** -- always prefer these over raw `printk` or `pr_*`. They automatically prefix messages with the device name, so you know which device is talking:

```c
dev_dbg(&pfdev->pdev->dev, "starting streaming\n");    /* Debug level */
dev_info(&pfdev->pdev->dev, "device ready\n");         /* Info level */
dev_warn(&pfdev->pdev->dev, "buffer nearly full\n");   /* Warning */
dev_err(&pfdev->pdev->dev, "DMA failed\n");            /* Error */
```

**Print early, print often.** When implementing a new function, start by adding a print at the entry point. This alone tells you whether your code path is even being reached:

```c
static int pfpga_start_streaming(struct phantomfpga_dev *pfdev)
{
    dev_info(&pfdev->pdev->dev, "start_streaming called\n");
    /* ... */
}
```

**Print register values.** When writing to hardware registers, print what you're writing. When reading status, print what you got. This is the fastest way to find "I thought I wrote X but the device sees Y" bugs:

```c
dev_info(&pfdev->pdev->dev, "ring addr lo=0x%08x hi=0x%08x size=%u\n",
         lower_32_bits(ring_dma), upper_32_bits(ring_dma), desc_count);
```

**Print indices.** For ring buffer debugging, dump the index state. Most ring bugs show up as indices that don't move or that overtake each other:

```c
dev_dbg(&pfdev->pdev->dev,
        "head=%u tail=%u shadow=%u consumer=%u\n",
        pfdev->desc_head, pfdev->desc_tail,
        pfdev->shadow_tail, pfdev->consumer);
```

**Rate-limited prints** for hot paths. In the IRQ handler or read path, a print per invocation will flood your log and slow everything to a crawl. Use `dev_dbg` (which is compiled out unless you enable it) or `dev_info_ratelimited()`:

```c
/* In the IRQ handler -- fires often, don't spam */
dev_dbg(&pfdev->pdev->dev, "complete IRQ, shadow_tail now %u\n",
        pfdev->shadow_tail);
```

**Enable debug messages at runtime:**
```bash
echo 8 > /proc/sys/kernel/printk           # Show all levels in console
echo "module phantomfpga +p" > /sys/kernel/debug/dynamic_debug/control
```

Or at boot via kernel command line:
```
dyndbg="module phantomfpga +p"
```

**When to remove them:** Once a function works reliably, downgrade `dev_info` to `dev_dbg` rather than deleting them. That way they're still there if you need them, but silent by default. Keep `dev_err` and `dev_warn` in error paths permanently -- those are part of proper driver behavior, not just debugging aids.

### Checking device state

```bash
# View PCI device
lspci -v -s $(lspci | grep 0dad | cut -d' ' -f1)

# View allocated resources
cat /proc/iomem | grep phantomfpga

# View interrupts
cat /proc/interrupts | grep phantomfpga

# Read registers directly (useful before driver is working)
BAR0=$(lspci -v -s 00:01.0 | grep "Memory at" | awk '{print $3}')
devmem $((0x${BAR0} + 0x000))   # DEV_ID -- should be 0xF00DFACE
devmem $((0x${BAR0} + 0x00C))   # STATUS
devmem $((0x${BAR0} + 0x040))   # STAT_FRAMES_TX
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

## Testing your implementation

### Basic functionality

```bash
# Load driver
insmod phantomfpga.ko
dmesg | tail

# Verify device node
ls -la /dev/phantomfpga0

# Build and run app
cd /mnt/app
make
./phantomfpga_app
```

### Integration tests

```bash
cd /mnt/tests/integration
./run_all.sh
```

### Fault injection

Once streaming works, test your error handling:
```bash
# In the guest, via the app's SET_FAULT ioctl, or directly:
devmem $((0x${BAR0} + 0x58)) w 0x02   # CORRUPT_CRC
devmem $((0x${BAR0} + 0x5C)) w 10     # Every ~10 frames

# Your driver/app should detect bad CRCs and report them
```

---

## Suggested implementation order

The parts above are numbered for reading order, but here's a practical build order -- each step gives you something testable. Do NOT skip ahead. Each step builds on the previous one, and each has a clear "it works" signal. If a step doesn't work, fix it before moving on. Debugging step 8 when step 3 is broken is not a good time.

1. **Soft reset** (Part 3) -- simple, and probe already calls it
2. **Descriptor allocation** (Part 1) -- verify with dmesg, check for leaks on rmmod
3. **Descriptor ring setup** (Part 2) -- program registers, verify with devmem
4. **MSI-X setup** (Part 4) -- check `/proc/interrupts`
5. **Interrupt handlers** (Part 5) -- start simple, just clear and log
6. **Configuration + start/stop** (Part 3) -- now you can start the device
7. **Poll** (Part 6) -- get async notification working
8. **Read** (Part 6) -- the moment of truth: data flows to userspace
9. **Mmap** (Part 7) -- zero-copy path
10. **IOCTLs** (Part 8) -- SET_CFG, CONSUME_FRAME, SET_FAULT

---

## Next steps

Once your driver is working:

1. **Implement the userspace app** - Complete the TODOs in `app/phantomfpga_app_impl.cpp`
2. **Run the viewer** - Complete `viewer/phantomfpga_view_impl.cpp` and see what the device has been hiding
3. **Test with fault injection** - Break things on purpose and watch your error handling cope (or not)

Good luck, and may your descriptors never run empty!
