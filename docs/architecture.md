# PhantomFPGA Architecture

> "Any sufficiently advanced emulation is indistinguishable from real hardware bugs."
> - Somebody, probably

This document explains how all the pieces of PhantomFPGA fit together. If you're
the type who likes to understand the full picture before diving in, this is for you.
If you prefer to just start coding and figure it out later... well, you'll probably
end up here eventually anyway.

## System Overview

```
+------------------------------------------------------------------+
|                          HOST SYSTEM                             |
|                                                                  |
|  +------------------------------------------------------------+  |
|  |                         QEMU                               |  |
|  |                                                            |  |
|  |  +------------------------------------------------------+  |  |
|  |  |                    GUEST VM                          |  |  |
|  |  |                                                      |  |  |
|  |  |  +-------------+      +------------------------+     |  |  |
|  |  |  |  Userspace  |      |    Linux Kernel        |     |  |  |
|  |  |  |             |      |                        |     |  |  |
|  |  |  | +---------+ | ioctl| +--------------------+ |     |  |  |
|  |  |  | | App     |<------>| | phantomfpga_drv.ko | |     |  |  |
|  |  |  | +---------+ | mmap | +--------------------+ |     |  |  |
|  |  |  |      |      |      |          |             |     |  |  |
|  |  |  +------|------+      +----------|-------------+     |  |  |
|  |  |         |                        |                   |  |  |
|  |  |         |   DMA Buffer           | MMIO + MSI-X      |  |  |
|  |  |         v   (shared)             v                   |  |  |
|  |  |  +------------------------------------------+        |  |  |
|  |  |  |              Guest Memory                |        |  |  |
|  |  |  |  +------------------------------------+  |        |  |  |
|  |  |  |  |        Ring Buffer (DMA)           |  |        |  |  |
|  |  |  |  |  [Frame0][Frame1][Frame2]...[FrameN]  |        |  |  |
|  |  |  |  +------------------------------------+  |        |  |  |
|  |  |  +------------------------------------------+        |  |  |
|  |  +------------------------------------------------------+  |  |
|  |                         ^                                  |  |
|  |                         | DMA Write                        |  |
|  |                         |                                  |  |
|  |  +------------------------------------------------------+  |  |
|  |  |              PhantomFPGA Device                      |  |  |
|  |  |                                                      |  |  |
|  |  |  +------------+  +----------+  +------------------+  |  |  |
|  |  |  | Registers  |  | Timer    |  | Frame Generator  |  |  |  |
|  |  |  | (BAR0)     |  | (1kHz)   |  | (PRNG payload)   |  |  |  |
|  |  |  +------------+  +----------+  +------------------+  |  |  |
|  |  |                                                      |  |  |
|  |  +------------------------------------------------------+  |  |
|  +------------------------------------------------------------+  |
+------------------------------------------------------------------+
```

## Components

### 1. PhantomFPGA QEMU Device

**Location:** `platform/qemu/src/hw/misc/phantomfpga.c`

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
2. Generates a frame with header + pseudo-random payload
3. Writes the frame to the ring buffer via DMA
4. Advances the producer index
5. Fires MSI-X interrupt when watermark is reached

### 2. Linux Kernel Driver

**Location:** `driver/phantomfpga_drv.c`

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
|  |                  struct phantomfpga_dev                       ||
|  |  - pdev (PCI device)        - cdev (char device)              ||
|  |  - regs (BAR0 mapping)      - dma_buf (DMA buffer)            ||
|  |  - lock (spinlock)          - wait_queue                      ||
|  |  - prod_idx, cons_idx       - frame_size, ring_size           ||
|  +---------------------------------------------------------------+|
+-------------------------------------------------------------------+
```

### 3. Userspace Application

**Location:** `app/phantomfpga_app.c`

A test application demonstrating driver interaction:

1. Opens `/dev/phantomfpga0`
2. Configures device via `PHANTOMFPGA_IOCTL_SET_CFG`
3. Maps the DMA buffer via `mmap()`
4. Starts streaming via `PHANTOMFPGA_IOCTL_START`
5. Polls for new frames
6. Processes and validates frames
7. Marks frames consumed via `PHANTOMFPGA_IOCTL_CONSUME_FRAME`

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
   |  DMA write header         |                           |
   |-------------------------->| Frame N                   |
   |  DMA write payload        |  +------------------+     |
   |-------------------------->|  | magic: 0xABCD1234|     |
   |                           |  | seq: N           |     |
   |  prod_idx++               |  | ts_ns: ...       |     |
   |                           |  | payload_len: ... |     |
   |  pending >= watermark?    |  | [payload bytes]  |     |
   |                           |  +------------------+     |
   |  Yes: set IRQ_WATERMARK   |                           |
   |                           |                           |
   |  MSI-X vector 0           |                           |
   |---------------------------------------------->|       |
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
   |                           |     wake_up_interruptible |
   |                           |           |               |
   |                           |           v               |
   |                           |     Userspace unblocks    |
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

The ring buffer is a circular queue with power-of-2 size for efficient
index wrapping.

### Memory Layout

```
DMA Buffer Base (dma_addr)
|
v
+--------+--------+--------+-----+--------+
| Frame0 | Frame1 | Frame2 | ... | FrameN |
+--------+--------+--------+-----+--------+
|<------- frame_size ----->|

Total size = frame_size * ring_size
```

### Index Management

```
Ring with 8 entries (ring_size = 8):

                     Consumer                Producer
                        |                       |
                        v                       v
        +---+---+---+---+---+---+---+---+
Slot:   | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |
        +---+---+---+---+---+---+---+---+
              ^   ^   ^   ^
              |   +---+   |
              |  Pending  |
              |  Frames   |
              +-----------+

cons_idx = 1
prod_idx = 5
pending = (prod_idx - cons_idx) & (ring_size - 1) = 4

After consuming one frame:
cons_idx = 2
pending = 3
```

### Full vs Empty

The ring keeps one slot empty to distinguish full from empty:

| Condition | Formula |
|-----------|---------|
| Empty | `prod_idx == cons_idx` |
| Full | `((prod_idx + 1) & (ring_size - 1)) == cons_idx` |
| Pending | `(prod_idx - cons_idx) & (ring_size - 1)` |

### Overrun Handling

When the ring is full and the device tries to produce a new frame:

1. Device sets `STATUS_OVERRUN` flag
2. Device increments `stat_overruns` counter
3. Device fires `IRQ_OVERRUN` interrupt (MSI-X vector 1)
4. Frame is **not** written (dropped)
5. Producer index is **not** advanced

The driver must consume frames faster to prevent overruns.

## MSI-X Interrupt Flow

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

The watermark controls interrupt coalescing:

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

Enable in the guest:
```bash
# Write directly to device register (requires root, mapped BAR)
# Or via driver debug interface if implemented
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

For mmap to userspace, use `dma_mmap_coherent()` or ensure
`pgprot_noncached()` is applied.

## Concurrency Model

### Device-side

The QEMU device runs in a single QEMU thread (main loop or dedicated vcpu).
All register access is serialized by QEMU's memory region locking.

### Driver-side

```
+------------------+     +------------------+     +------------------+
|  ioctl context   |     |  IRQ context     |     |  tasklet/work    |
|  (process)       |     |  (hard IRQ)      |     |  (if used)       |
+--------+---------+     +--------+---------+     +--------+---------+
         |                        |                        |
         v                        v                        v
    ioctl_lock (mutex)       spinlock                 spinlock
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

1. Never hold spinlock when calling `mutex_lock`
2. Use `spin_lock_irqsave` in process context (can be interrupted)
3. Use `spin_lock` in IRQ context (already interrupt-disabled)
4. Keep critical sections short

## Guest-Host Interface

### Shared Directories (9p virtfs)

```
Host                          Guest
driver/  <--- 9p mount --->  /mnt/driver
app/     <--- 9p mount --->  /mnt/app
```

Changes on the host are immediately visible in the guest (and vice versa).
This allows editing code on the host and building in the guest.

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

### Theoretical Limits

| Frame Rate | Frame Size | Bandwidth | Latency (@1kHz) |
|------------|------------|-----------|-----------------|
| 1 kHz | 4 KB | 4 MB/s | 1 ms |
| 10 kHz | 4 KB | 40 MB/s | 0.1 ms |
| 100 kHz | 256 B | 25 MB/s | 0.01 ms |

### Practical Considerations

- QEMU virtual timer resolution limits high rates
- DMA bandwidth limited by QEMU's memory subsystem
- MSI-X delivery adds latency vs polling
- Guest CPU scheduling affects consumption rate

For training purposes, 1 kHz with 4 KB frames is a reasonable default. You're
learning, not trying to break speed records. Save that for when you have real
hardware and a deadline.

## Still With Me?

If you made it this far, you now understand more about virtual device architecture
than most people learn in their first year of embedded work. Give yourself a pat
on the back. Or a coffee. Or both.

## Next Steps

- **[Register Reference](register-reference.md)** - The actual hardware interface
- **[Driver Guide](driver-guide.md)** - Time to write some code

---

*"I understood everything in this document on the first read."*
*- Nobody, ever*
