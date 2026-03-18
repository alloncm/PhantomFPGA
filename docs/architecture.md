# PhantomFPGA architecture

> "Any sufficiently advanced emulation is indistinguishable from real hardware bugs."
> - Somebody, probably

This document explains how all the pieces of PhantomFPGA fit together. If you're the type who likes to understand the full picture before diving in, this is for you. If you prefer to just start coding and figure it out later... well, you'll probably end up here eventually anyway.

> **New to kernel development?** Check the **[Glossary](glossary.md)** for quick explanations of terms like PCIe, DMA, BAR, MSI-X, and other jargon.

## System overview

```
+-------------------------------------------------------------------------+
|                              HOST SYSTEM                                |
|                                                                         |
|  +----------+                                                           |
|  |  Viewer  |  (phantomfpga_view - client, runs on host terminal)       |
|  +----+-----+                                                           |
|       |                                                                 |
|       | TCP :5000                                                       |
|       v                                                                 |
|  +-------------------------------------------------------------------+  |
|  |                            QEMU                                   |  |
|  |                                                                   |  |
|  |  +---------------------------------------------------------+      |  |
|  |  |                       GUEST VM                          |      |  |
|  |  |                                                         |      |  |
|  |  |  +---------------+      +------------------------+      |      |  |
|  |  |  |   Userspace   |      |    Linux Kernel        |      |      |  |
|  |  |  |               |      |                        |      |      |  |
|  |  |  | +-----------+ | ioctl| +--------------------+ |      |      |  |
|  |  |  | | App       |<------>| | phantomfpga_drv.ko | |      |      |  |
|  |  |  | | (TCP srv) | | mmap | +--------------------+ |      |      |  |
|  |  |  | +-----------+ |      |          |             |      |      |  |
|  |  |  |       |       |      +----------|-------------+      |      |  |
|  |  |  +-------|-------+                 |                    |      |  |
|  |  |          |                         |                    |      |  |
|  |  |          |   DMA Buffer            | MMIO + MSI-X       |      |  |
|  |  |          v   (shared)              v                    |      |  |
|  |  |  +---------------------------------------------+        |      |  |
|  |  |  |               Guest Memory                  |        |      |  |
|  |  |  |  +---------------------------------------+  |        |      |  |
|  |  |  |  |         Ring Buffer (DMA)             |  |        |      |  |
|  |  |  |  |  [Frame0][Frame1][Frame2]...[FrameN]  |  |        |      |  |
|  |  |  |  +---------------------------------------+  |        |      |  |
|  |  |  +---------------------------------------------+        |      |  |
|  |  +---------------------------------------------------------+      |  |
|  |                            ^                                      |  |
|  |                            | DMA Write                            |  |
|  |                            |                                      |  |
|  |  +---------------------------------------------------------+      |  |
|  |  |                 PhantomFPGA Device                      |      |  |
|  |  |                                                         |      |  |
|  |  |  +------------+  +----------+  +---------------------+  |      |  |
|  |  |  | Registers  |  | Timer    |  | Frame Generator     |  |      |  |
|  |  |  | (BAR0)     |  | (25 Hz)  |  | (embedded frames)   |  |      |  |
|  |  |  +------------+  +----------+  +---------------------+  |      |  |
|  |  |                                                         |      |  |
|  |  +---------------------------------------------------------+      |  |
|  +-------------------------------------------------------------------+  |
+-------------------------------------------------------------------------+
```

## Components

### 1. PhantomFPGA QEMU device

**Location:** `platform/qemu/src/hw/misc/phantomfpga.c`

> **Already implemented.** This component is complete and ready to use. You don't need to modify it - just understand how it works so you can write the driver.

The virtual PCIe device that simulates an FPGA frame producer. Key features:

| Feature | Description |
|---------|-------------|
| PCI ID | Vendor 0x0DAD, Device 0xF00D |
| BAR0 | 4KB MMIO register space |
| MSI-X | 3 vectors (complete, error, no_desc) |
| DMA | Bus-master writes to guest memory |

**Device State Machine:**

```
    +-------+     CTRL_RUN      +---------+
    | IDLE  |----------------->| RUNNING |
    +-------+                  +---------+
        ^                          |
        |      CTRL_RESET or       |
        |       CTRL_RUN=0         |
        +--------------------------+
```

When running, the device:
1. Fires a timer at the configured frame rate
2. Assembles a frame with header + payload (embedded data) + CRC
3. Writes the frame to the ring buffer via scatter-gather DMA
4. Advances the descriptor tail index
5. Fires MSI-X interrupt (based on coalescing: count threshold or timeout)

### 2. Linux kernel driver

**Location:** `driver/phantomfpga_drv.c`

> **Your job.** The skeleton is there with detailed TODOs. See the [Driver Guide](driver-guide.md) for step-by-step instructions.

The kernel module that interfaces with the device. Responsibilities:

| Component | Purpose |
|-----------|---------|
| PCI probe/remove | Device lifecycle management |
| BAR mapping | Register access via `ioread32`/`iowrite32` |
| DMA allocation | Descriptor ring + per-descriptor buffers |
| MSI-X handlers | Interrupt handling |
| Char device | `/dev/phantomfpga0` for userspace |
| File operations | open, read, poll, mmap, ioctl |

**Driver Architecture:**

```
+-------------------------------------------------------------------+
|                      phantomfpga_drv.ko                           |
|                                                                   |
|  +----------------+     +-----------------+     +---------------+ |
|  | PCI Subsystem  |     | Char Device     |     | IRQ Handlers  | |
|  |                |     |                 |     |               | |
|  | - probe()      |     | - open()        |     | - complete()  | |
|  | - remove()     |     | - release()     |     | - error()     | |
|  | - BAR mapping  |     | - read()        |     | - no_desc()   | |
|  | - MSI-X setup  |     | - poll()        |     |               | |
|  | - DMA alloc    |     | - mmap()        |     | Updates:      | |
|  +----------------+     | - ioctl()       |     | - shadow_tail | |
|         |               +-----------------+     +---------------+ |
|         |                      |                       |          |
|         v                      v                       v          |
|  +---------------------------------------------------------------+|
|  |                    struct phantomfpga_dev                       ||
|  |  - pdev (PCI device)        - cdev (char device)              ||
|  |  - regs (BAR0 mapping)      - desc_ring (SG descriptors)     ||
|  |  - lock (spinlock)          - buffers (DMA buffers)           ||
|  |  - desc_head, desc_tail     - wait_queue                      ||
|  +---------------------------------------------------------------+|
+-------------------------------------------------------------------+
```

### 3. Userspace application (TCP server)

**Location:** `app/phantomfpga_app_impl.cpp`

> **Your job.** The skeleton handles argument parsing, TCP setup, and the main loop structure. You implement the frame processing and validation.

The server application that bridges the driver and external viewers:

1. Opens `/dev/phantomfpga0`
2. Configures device via `PHANTOMFPGA_IOCTL_SET_CFG`
3. Maps the DMA buffer via `mmap()`
4. Starts streaming via `PHANTOMFPGA_IOCTL_START`
5. Polls for new frames
6. Validates frames (magic, sequence, CRC)
7. **Streams frames over TCP to connected clients**
8. Marks frames consumed via `PHANTOMFPGA_IOCTL_CONSUME_FRAME`

The app runs inside the guest VM and listens on port 5000 for viewer connections.

### 4. Terminal viewer (TCP client)

**Location:** `viewer/phantomfpga_view_impl.cpp`

> **Your job.** The skeleton handles networking and terminal setup. You implement the frame validation and display logic. This is the final piece of the puzzle.

A terminal-based client that runs on the host machine:

1. Connects to the app's TCP server (default: `localhost:5000`)
2. Receives frame data over the network
3. Validates frame integrity (CRC check)
4. Displays frame data in the terminal
5. Tracks statistics (frame rate, dropped frames, etc.)
6. Optionally records raw frames to disk (`--record`) for offline validation

The viewer is the final piece of the puzzle - when everything works, you'll see what the device has been hiding all along. Use `--record stream.bin` to save the stream, then validate it with `tools/validate_stream.py`.

## Data flow

### Control plane (configuration)

```
Userspace                    Kernel                      Device
   |                           |                           |
   |  ioctl(SET_CFG)           |                           |
   |-------------------------->|                           |
   |                           |  write registers          |
   |                           |-------------------------->|
   |                           |                           |
   |  ioctl(START)             |                           |
   |-------------------------->|                           |
   |                           |  CTRL |= RUN | IRQ_EN     |
   |                           |-------------------------->|
   |                           |                           |
   |                           |         Timer starts      |
   |                           |           running         |
```

### Data plane (frame production)

```
Device                        Memory                      Driver
   |                           |                           |
   |  Timer fires              |                           |
   |                           |                           |
   |  Read descriptor at tail  |                           |
   |  DMA write frame to buf   |                           |
   |-------------------------->| Descriptor buffer         |
   |                           |  +------------------+     |
   |  Set COMPLETED flag       |  | magic: 0xF00DFACE|     |
   |  Write completion status  |  | seq: N           |     |
   |  Advance desc_tail        |  | [payload bytes]  |     |
   |                           |  | crc32: ...       |     |
   |  Coalesce threshold met?  |  +------------------+     |
   |  (count or timeout)       |                           |
   |                           |                           |
   |  Yes: set IRQ_COMPLETE    |                           |
   |                           |                           |
   |  MSI-X vector 0           |                           |
   |------------------------------------------------------>|
   |                           |                           |
   |                           |     Read IRQ_STATUS       |
   |                           |<--------------------------|
   |                           |                           |
   |                           |     Clear IRQ (W1C)       |
   |                           |<--------------------------|
   |                           |                           |
   |                           |     Read DESC_TAIL        |
   |                           |<--------------------------|
   |                           |                           |
   |                           |   wake_up_interruptible   |
   |                           |           |               |
   |                           |           v               |
   |                           |    Userspace unblocks     |
```

### Data plane (frame consumption)

```
Userspace                    Kernel                      Device
   |                           |                           |
   |  poll() returns POLLIN    |                           |
   |<--------------------------|                           |
   |                           |                           |
   |  Access mmap'd buffer     |                           |
   |  for current descriptor   |                           |
   |                           |                           |
   |  Validate frame:          |                           |
   |   - Check magic           |                           |
   |   - Check sequence        |                           |
   |   - Verify CRC            |                           |
   |   - Process payload       |                           |
   |                           |                           |
   |  ioctl(CONSUME_FRAME)     |                           |
   |-------------------------->|                           |
   |                           |  consumer++               |
   |                           |  Reset descriptor         |
   |                           |  Resubmit (desc_head++)   |
   |                           |  write DESC_HEAD register |
   |                           |-------------------------->|
   |                           |                           |
   |                           |    Descriptor available   |
```

## Ring buffer protocol

The ring buffer is a circular queue with power-of-2 size for efficient index wrapping. If you've ever dealt with a circular parking garage, it's the same idea - except the cars are frames and nobody's honking.

### Memory layout

```
Descriptor Ring (coherent DMA)
|
v
+----------+----------+----------+-----+----------+
|  Desc 0  |  Desc 1  |  Desc 2  | ... |  Desc N  |  (32 bytes each)
+----------+----------+----------+-----+----------+
     |           |          |                |
     v           v          v                v
+---------+ +---------+ +---------+    +---------+
| Buffer0 | | Buffer1 | | Buffer2 |   | BufferN |  (5136 bytes each)
| [frame] | | [frame] | | [frame] |   | [frame] |
| [compl] | | [compl] | | [compl] |   | [compl] |
+---------+ +---------+ +---------+    +---------+

Each buffer = frame (5120 bytes) + completion writeback (16 bytes)
```

### Index management

The descriptor ring has three pointers. Think of it as a two-stage pipeline:

```
Ring with 8 descriptors (desc_count = 8):

         desc_tail     desc_head
          (device)      (driver)
              |               |
              v               v
        +---+---+---+---+---+---+---+---+
Slot:   | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
        +---+---+---+---+---+---+---+---+
              ^           ^
              | Available |
              |  to device|
              +-----------+

desc_head = 5  (driver submits up to here)
desc_tail = 1  (device has completed up to here)
pending = (desc_head - desc_tail) & (desc_count - 1) = 4
```

The driver also tracks a `consumer` index for completed-but-not-yet-consumed descriptors, and a `shadow_tail` updated by the IRQ handler.

### Full vs empty

The ring keeps one slot empty to distinguish full from empty. Yes, we sacrifice one slot. It's a classic tradeoff - simpler code or more space. Simpler code won.

| Condition | Formula |
|-----------|---------|
| Empty | `desc_head == desc_tail` |
| Full | `((desc_head + 1) & (desc_count - 1)) == desc_tail` |
| Pending | `(desc_head - desc_tail) & (desc_count - 1)` |

### No-descriptor handling

When no descriptors are available and the device tries to produce a new frame, things get ugly (but predictable):

1. Device sets `STATUS_DESC_EMPTY` flag
2. Device increments `stat_frames_drop` counter
3. Device fires `IRQ_NO_DESC` interrupt (MSI-X vector 2)
4. Frame is **not** written (dropped)
5. Descriptor tail is **not** advanced

The driver must consume frames and resubmit descriptors fast enough to keep up. The device will not wait for you. It has places to be.

## MSI-X interrupt flow

MSI-X is how the device taps the CPU on the shoulder without being rude. Three vectors, three reasons to interrupt your day.

### Vector assignment

| Vector | Interrupt | Purpose |
|--------|-----------|---------|
| 0 | Complete | Descriptor(s) completed (coalescing threshold met) |
| 1 | Error | DMA or device error |
| 2 | No_desc | No descriptors available, frame dropped |

### Interrupt lifecycle

```
1. Device sets IRQ_STATUS bit
2. If (IRQ_MASK & IRQ_STATUS) and CTRL_IRQ_EN:
   Device fires MSI-X to corresponding vector
3. Driver reads IRQ_STATUS
4. Driver clears interrupt by writing back to IRQ_STATUS (W1C)
5. Driver processes condition (wake waiters, log error, etc.)
```

### IRQ coalescing

The IRQ_COALESCE register (0x03C) controls when completion interrupts fire. Two thresholds, whichever hits first wins:

| Setting | Field | Behavior |
|---------|-------|----------|
| Count [15:0] | irq_coalesce_count | Interrupt after N completions |
| Timeout [31:16] | irq_coalesce_timeout | Interrupt after N microseconds |

Examples with default 25 fps:

| Count | Timeout | Behavior |
|-------|---------|----------|
| 1 | 40000 us | Interrupt on every frame (high CPU, low latency) |
| 8 | 40000 us | Interrupt every 8 frames or 40ms (default, balanced) |
| 64 | 100000 us | Batch heavily (lower CPU, higher latency) |

## Fault injection

The device supports testing error handling via the `FAULT_INJECT` register:

| Bit | Fault | Effect |
|-----|-------|--------|
| 0 | DROP_FRAME | Drop frames randomly (probability ~1/N via FAULT_RATE) |
| 1 | CORRUPT_CRC | Write wrong CRC value |
| 2 | CORRUPT_DATA | Flip bits in frame data |
| 3 | SKIP_SEQUENCE | Skip sequence numbers |

The `FAULT_RATE` register (0x05C) controls probability: roughly 1 in N frames will be affected (default 1000).

Enable in the guest (before loading the driver):
```bash
# Get BAR0 address from lspci, enable device, then poke the FAULT_INJECT register.
# 0x58 is the register offset - see the datasheet for the full register map.
echo 1 > /sys/bus/pci/devices/0000:00:01.0/enable
BAR0=$(lspci -v -s 00:01.0 | grep "Memory at" | awk '{print $3}')
devmem $((0x${BAR0} + 0x58)) w 0x01   # Enable DROP_FRAME (bit 0)
devmem $((0x${BAR0} + 0x58)) w 0x06   # Enable CORRUPT_CRC + CORRUPT_DATA (bits 1,2)
devmem $((0x${BAR0} + 0x5C)) w 100    # Set fault rate to ~1 in 100 frames

# No output means success. Read back to verify:
devmem $((0x${BAR0} + 0x58))          # Should print 0x00000006
```

## Memory considerations

### DMA buffer sizing

Each descriptor needs its own buffer (frame + completion writeback), plus the descriptor ring itself:

```
per_buffer = frame_size + completion_size = 5120 + 16 = 5136 bytes
ring_mem   = desc_count * descriptor_size = desc_count * 32 bytes
total      = (per_buffer * desc_count) + ring_mem

Example with 256 descriptors (default):
  5136 * 256 = ~1.3 MB for buffers
  32 * 256   = 8 KB for descriptor ring
```

The driver should allocate DMA memory using `dma_alloc_coherent()` which:
- Returns physically contiguous memory
- Is cache-coherent (no explicit cache management needed)
- Provides both virtual (for driver) and physical (for device) addresses

### Cache coherency

With coherent DMA (`dma_alloc_coherent`):
- Device writes are immediately visible to CPU
- No cache flush/invalidate needed
- Slightly slower than streaming DMA but simpler

For mmap to userspace, use `dma_mmap_coherent()` or ensure `pgprot_noncached()` is applied.

## Concurrency model

Welcome to the fun part - making sure nothing explodes when multiple things happen at once. This is where bugs go to hide.

### Device-side

The QEMU device runs in a single QEMU thread (main loop or dedicated vcpu). All register access is serialized by QEMU's memory region locking.

### Driver-side

```
+------------------+     +------------------+     +------------------+
|  ioctl context   |     |  IRQ context     |     |  tasklet/work    |
|  (process)       |     |  (hard IRQ)      |     |    (if used)     |
+--------+---------+     +--------+---------+     +--------+---------+
         |                        |                        |
         v                        v                        v
    ioctl_lock (mutex)        spinlock                 spinlock
         |                        |                        |
         v                        v                        v
    +----------------------------------------------------------+
    |                    phantomfpga_dev                       |
    +----------------------------------------------------------+
```

| Lock | Type | Protects |
|------|------|----------|
| `ioctl_lock` | Mutex | Configuration changes, start/stop |
| `lock` | Spinlock | Indices, statistics, IRQ state |
| `wait_queue` | Wait queue | Sleeping in read/poll |

### Ordering rules

These rules exist because someone, somewhere, learned them the hard way:

1. Never hold spinlock when calling `mutex_lock` (deadlock city)
2. Use `spin_lock_irqsave` in process context (can be interrupted)
3. Use `spin_lock` in IRQ context (already interrupt-disabled)
4. Keep critical sections short (the system is waiting on you)

## Guest-host interface

### Shared directories (9p virtfs)

> See [Glossary: 9p/virtfs](glossary.md#9p--virtfs) for background on this protocol.

```
Host                          Guest
driver/  <--- 9p mount --->  /mnt/driver
app/     <--- 9p mount --->  /mnt/app
```

Changes on the host are immediately visible in the guest (and vice versa). This allows editing code on the host and building in the guest.

### SSH access

```
Host port 2222 --> Guest port 22

ssh -p 2222 root@localhost
```

### GDB debugging

With `--debug` flag:
```
Host port 1234 --> QEMU GDB stub --> Guest kernel

gdb vmlinux
(gdb) target remote :1234
(gdb) break phantomfpga_probe
(gdb) continue
```

## Performance characteristics

Or: "Why is my frame rate not 10,000 fps?" - You, probably not.

### Practical considerations

- QEMU virtual timer resolution limits high rates
- DMA bandwidth limited by QEMU's memory subsystem
- MSI-X delivery adds latency vs polling
- Guest CPU scheduling affects consumption rate

PhantomFPGA defaults to 25 fps with 5120-byte frames - plenty for what it's trying to show you. You're learning, not trying to break speed records. Save that for when you have real hardware and a deadline.

## Still with me?

If you made it this far, you now understand more about virtual device architecture than most people learn in their first year of embedded work. Give yourself a pat on the back. Or a coffee. Or both, you deserve it.

## Next steps

- **[Glossary](glossary.md)** - Quick reference for PCIe, DMA, MSI-X, and other jargon
- **[Device Datasheet](phantomfpga-datasheet.md)** - How the device works and the complete register reference
- **[Driver Guide](driver-guide.md)** - Time to write some code

---

*"I understood everything in this document on the first read."*
*- Nobody, ever*
