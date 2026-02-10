# PhantomFPGA Architecture

> "Any sufficiently advanced emulation is indistinguishable from real hardware bugs."
> - Somebody, probably

This document explains how all the pieces of PhantomFPGA fit together. If you're the type who likes to understand the full picture before diving in, this is for you. If you prefer to just start coding and figure it out later... well, you'll probably end up here eventually anyway.

> **New to kernel development?** Check the **[Glossary](glossary.md)** for quick explanations of terms like PCIe, DMA, BAR, MSI-X, and other jargon.

## System Overview

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

### 1. PhantomFPGA QEMU Device

**Location:** `platform/qemu/src/hw/misc/phantomfpga.c`

> **Already implemented.** This component is complete and ready to use. You don't need to modify it - just understand how it works so you can write the driver.

The virtual PCIe device that simulates an FPGA frame producer. Key features:

| Feature | Description |
|---------|-------------|
| PCI ID | Vendor 0x0DAD, Device 0xF00D |
| BAR0 | 4KB MMIO register space |
| MSI-X | 2 vectors (watermark, overrun) |
| DMA | Bus-master writes to guest memory |

**Device State Machine:**

```
    +-------+    CTRL_START    +---------+
    | IDLE  |----------------->| RUNNING |
    +-------+                  +---------+
        ^                          |
        |      CTRL_RESET or       |
        |      CTRL_START=0        |
        +--------------------------+
```

When running, the device:
1. Fires a timer at the configured frame rate
2. Assembles a frame with header + payload (embedded data) + CRC
3. Writes the frame to the ring buffer via scatter-gather DMA
4. Advances the producer index
5. Fires MSI-X interrupt when watermark is reached

### 2. Linux Kernel Driver

**Location:** `driver/phantomfpga_drv.c`

> **Your job.** The skeleton is there with detailed TODOs. See the [Driver Guide](driver-guide.md) for step-by-step instructions.

The kernel module that interfaces with the device. Responsibilities:

| Component | Purpose |
|-----------|---------|
| PCI probe/remove | Device lifecycle management |
| BAR mapping | Register access via `ioread32`/`iowrite32` |
| DMA allocation | Coherent buffer for ring buffer |
| MSI-X handlers | Interrupt handling |
| Char device | `/dev/phantomfpga0` for userspace |
| File operations | open, read, poll, mmap, ioctl |

**Driver Architecture:**

```
+-------------------------------------------------------------------+
|                      phantomfpga_drv.ko                           |
|                                                                   |
|  +----------------+     +-----------------+     +---------------+ |
|  | PCI Subsystem  |     | Char Device     |     | IRQ Handler   | |
|  |                |     |                 |     |               | |
|  | - probe()      |     | - open()        |     | - watermark() | |
|  | - remove()     |     | - release()     |     | - overrun()   | |
|  | - BAR mapping  |     | - read()        |     |               | |
|  | - MSI-X setup  |     | - poll()        |     | Updates:      | |
|  | - DMA alloc    |     | - mmap()        |     | - prod_idx    | |
|  +----------------+     | - ioctl()       |     | - wake_up()   | |
|         |               +-----------------+     +---------------+ |
|         |                      |                       |          |
|         v                      v                       v          |
|  +---------------------------------------------------------------+|
|  |                    struct phantomfpga_dev                       ||
|  |  - pdev (PCI device)        - cdev (char device)              ||
|  |  - regs (BAR0 mapping)      - dma_buf (DMA buffer)            ||
|  |  - lock (spinlock)          - wait_queue                      ||
|  |  - prod_idx, cons_idx       - frame_size, ring_size           ||
|  +---------------------------------------------------------------+|
+-------------------------------------------------------------------+
```

### 3. Userspace Application (TCP Server)

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

### 4. Terminal Viewer (TCP Client)

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

## Data Flow

### Control Plane (Configuration)

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
   |                           |  CTRL |= START | IRQ_EN   |
   |                           |-------------------------->|
   |                           |                           |
   |                           |         Timer starts      |
   |                           |           running         |
```

### Data Plane (Frame Production)

```
Device                        Memory                      Driver
   |                           |                           |
   |  Timer fires              |                           |
   |                           |                           |
   |  DMA write frame          |                           |
   |-------------------------->| Frame N                   |
   |                           |  +------------------+     |
   |                           |  | magic: 0xF00DFACE|     |
   |                           |  | seq: N           |     |
   |  prod_idx++               |  | [payload bytes]  |     |
   |                           |  | crc32: ...       |     |
   |  pending >= watermark?    |  +------------------+     |
   |                           |                           |
   |  Yes: set IRQ_WATERMARK   |                           |
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
   |                           |     Read PROD_IDX         |
   |                           |<--------------------------|
   |                           |                           |
   |                           |   wake_up_interruptible   |
   |                           |           |               |
   |                           |           v               |
   |                           |    Userspace unblocks     |
```

### Data Plane (Frame Consumption)

```
Userspace                    Kernel                      Device
   |                           |                           |
   |  poll() returns POLLIN    |                           |
   |<--------------------------|                           |
   |                           |                           |
   |  Access mmap'd buffer     |                           |
   |  at cons_idx * frame_size |                           |
   |                           |                           |
   |  Validate frame:          |                           |
   |   - Check magic           |                           |
   |   - Check sequence        |                           |
   |   - Process payload       |                           |
   |                           |                           |
   |  ioctl(CONSUME_FRAME)     |                           |
   |-------------------------->|                           |
   |                           |  cons_idx++               |
   |                           |  write CONS_IDX register  |
   |                           |-------------------------->|
   |                           |                           |
   |                           |     Ring space freed      |
```

## Ring Buffer Protocol

The ring buffer is a circular queue with power-of-2 size for efficient index wrapping. If you've ever dealt with a circular parking garage, it's the same idea - except the cars are frames and nobody's honking.

### Memory Layout

```
DMA Buffer Base (dma_addr)
|
v
+--------------+--------------+--------------+-----+--------------+
|    Frame0    |    Frame1    |    Frame2    | ... |    FrameN    |
+--------------+--------------+--------------+-----+--------------+
|<-frame_size->|

Total size = frame_size * ring_size
```

### Index Management

```
Ring with 8 entries (ring_size = 8):

          Consumer        Producer
              |               |
              v               v
        +---+---+---+---+---+---+---+---+
Slot:   | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
        +---+---+---+---+---+---+---+---+
              ^           ^
              |  Pending  |
              |  (1,2,3,4)|
              +-----------+

cons_idx = 1  (next frame to consume)
prod_idx = 5  (next slot device will write to)
pending = (prod_idx - cons_idx) & (ring_size - 1) = 4

After consuming one frame:
cons_idx = 2
pending = 3
```

### Full vs Empty

The ring keeps one slot empty to distinguish full from empty. Yes, we sacrifice one slot. It's a classic tradeoff - simpler code or more space. Simpler code won.

| Condition | Formula |
|-----------|---------|
| Empty | `prod_idx == cons_idx` |
| Full | `((prod_idx + 1) & (ring_size - 1)) == cons_idx` |
| Pending | `(prod_idx - cons_idx) & (ring_size - 1)` |

### Overrun Handling

When the ring is full and the device tries to produce a new frame, things get ugly (but predictable):

1. Device sets `STATUS_OVERRUN` flag
2. Device increments `stat_overruns` counter
3. Device fires `IRQ_OVERRUN` interrupt (MSI-X vector 1)
4. Frame is **not** written (dropped)
5. Producer index is **not** advanced

The driver must consume frames faster to prevent overruns. The device will not wait for you. It has places to be.

## MSI-X Interrupt Flow

MSI-X is how the device taps the CPU on the shoulder without being rude. Two vectors, two reasons to interrupt your day.

### Vector Assignment

| Vector | Interrupt | Purpose |
|--------|-----------|---------|
| 0 | Watermark | Pending frames >= watermark threshold |
| 1 | Overrun | Ring buffer full, frame dropped |

### Interrupt Lifecycle

```
1. Device sets IRQ_STATUS bit
2. If (IRQ_MASK & IRQ_STATUS) and CTRL_IRQ_EN:
   Device fires MSI-X to corresponding vector
3. Driver reads IRQ_STATUS
4. Driver clears interrupt by writing back to IRQ_STATUS (W1C)
5. Driver processes condition (wake waiters, log error, etc.)
```

### Watermark Threshold

The watermark controls interrupt coalescing, examples:

| Watermark | Behavior |
|-----------|----------|
| 1 | Interrupt on every frame (high CPU, low latency) |
| 64 | Interrupt every 64 frames (lower CPU, higher latency) |
| ring_size/2 | Balanced approach |

## Fault Injection

The device supports testing error handling via the `FAULT_INJECT` register:

| Bit | Fault | Effect |
|-----|-------|--------|
| 0 | DROP_FRAMES | Randomly drop ~10% of frames |
| 1 | CORRUPT_DATA | Flip bits in payload, set CORRUPTED flag |
| 2 | DELAY_IRQ | Suppress MSI-X (test polling fallback) |

Enable in the guest (before loading the driver):
```bash
# Get BAR0 address from lspci, enable device, then poke the FAULT_INJECT register.
# 0x58 is the register offset - see the datasheet for the full register map.
echo 1 > /sys/bus/pci/devices/0000:00:01.0/enable
BAR0=$(lspci -v -s 00:01.0 | grep "Memory at" | awk '{print $3}')
devmem $((0x${BAR0} + 0x58)) w 0x01   # Enable DROP_FRAMES (bit 0)
devmem $((0x${BAR0} + 0x58)) w 0x03   # Enable DROP + CORRUPT (bits 0,1)

# No output means success. Read back to verify:
devmem $((0x${BAR0} + 0x58))          # Should print 0x00000003
```

## Memory Considerations

### DMA Buffer Sizing

```
buffer_size = frame_size * ring_size

Examples:
  4KB frames x 256 entries = 1MB buffer
  64KB frames x 64 entries = 4MB buffer
```

The driver should allocate DMA memory using `dma_alloc_coherent()` which:
- Returns physically contiguous memory
- Is cache-coherent (no explicit cache management needed)
- Provides both virtual (for driver) and physical (for device) addresses

### Cache Coherency

With coherent DMA (`dma_alloc_coherent`):
- Device writes are immediately visible to CPU
- No cache flush/invalidate needed
- Slightly slower than streaming DMA but simpler

For mmap to userspace, use `dma_mmap_coherent()` or ensure `pgprot_noncached()` is applied.

## Concurrency Model

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

### Ordering Rules

These rules exist because someone, somewhere, learned them the hard way:

1. Never hold spinlock when calling `mutex_lock` (deadlock city)
2. Use `spin_lock_irqsave` in process context (can be interrupted)
3. Use `spin_lock` in IRQ context (already interrupt-disabled)
4. Keep critical sections short (the system is waiting on you)

## Guest-Host Interface

### Shared Directories (9p virtfs)

> See [Glossary: 9p/virtfs](glossary.md#9p--virtfs) for background on this protocol.

```
Host                          Guest
driver/  <--- 9p mount --->  /mnt/driver
app/     <--- 9p mount --->  /mnt/app
```

Changes on the host are immediately visible in the guest (and vice versa). This allows editing code on the host and building in the guest.

### SSH Access

```
Host port 2222 --> Guest port 22

ssh -p 2222 root@localhost
```

### GDB Debugging

With `--debug` flag:
```
Host port 1234 --> QEMU GDB stub --> Guest kernel

gdb vmlinux
(gdb) target remote :1234
(gdb) break phantomfpga_probe
(gdb) continue
```

## Performance Characteristics

Or: "Why is my frame rate not 10,000 fps?" - You, probably not.

### Practical Considerations

- QEMU virtual timer resolution limits high rates
- DMA bandwidth limited by QEMU's memory subsystem
- MSI-X delivery adds latency vs polling
- Guest CPU scheduling affects consumption rate

PhantomFPGA defaults to 25 fps with 5120-byte frames - plenty for what it's trying to show you. You're learning, not trying to break speed records. Save that for when you have real hardware and a deadline.

## Still With Me?

If you made it this far, you now understand more about virtual device architecture than most people learn in their first year of embedded work. Give yourself a pat on the back. Or a coffee. Or both, you deserve it.

## Next Steps

- **[Glossary](glossary.md)** - Quick reference for PCIe, DMA, MSI-X, and other jargon
- **[Device Datasheet](phantomfpga-datasheet.md)** - How the device works and the complete register reference
- **[Driver Guide](driver-guide.md)** - Time to write some code

---

*"I understood everything in this document on the first read."*
*- Nobody, ever*
