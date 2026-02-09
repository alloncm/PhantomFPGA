# PhantomFPGA Device Datasheet v3.0

> "This is where the rubber meets the road. Or rather, where your code meets the hardware."

This document is your complete reference for the PhantomFPGA virtual PCIe device.
It covers how the device works, what it does, and every register you need to
control it. Keep this open while you're coding - you'll refer to it approximately
47 times per implementation session.

(That number is made up. The real number is probably higher.)

---

## Device Overview

PhantomFPGA is a PCIe device that streams fixed-size data frames to host memory
via scatter-gather DMA. Think of it as a data firehose: the device has data it
wants to send you, and your job is to catch it.

### Block Diagram

```
+------------------------------------------------------------------+
|                        PhantomFPGA Device                        |
|                                                                  |
|  +------------------+     +------------------+     +-----------+ |
|  |   Frame Store    |---->|  Frame Engine    |---->|  SG-DMA   | |
|  |   (250 frames)   |     |  (timing/seq)    |     |  Engine   | |
|  +------------------+     +------------------+     +-----+-----+ |
|                                                          |       |
|  +------------------+     +------------------+           |       |
|  |  Fault Injection |---->|  CRC Generator   |-----------+       |
|  +------------------+     +------------------+           |       |
|                                                          v       |
|  +------------------+     +------------------+     +-----------+ |
|  |   IRQ Controller |<----|  Desc Manager    |<----|  PCIe     | |
|  |   (MSI-X x3)     |     |  (ring/head/tail)|     |  Interface| |
|  +------------------+     +------------------+     +-----------+ |
+------------------------------------------------------------------+
                                   |
                                   | PCIe Bus
                                   v
+------------------------------------------------------------------+
|                          Host System                             |
|  +------------------+     +------------------+                   |
|  |  Your Driver     |---->|  Frame Buffers   |                   |
|  |  (you write it)  |     |  (DMA memory)    |                   |
|  +------------------+     +------------------+                   |
+------------------------------------------------------------------+
```

### Key Characteristics

| Parameter | Value | Notes |
|-----------|-------|-------|
| Frame size | 5120 bytes | Fixed, includes header and CRC |
| Frame count | 250 | Loops continuously |
| Frame rate | 1-60 fps | Configurable, default 25 |
| DMA type | Scatter-gather | Descriptor ring-based |
| Interrupts | MSI-X (3 vectors) | Complete, Error, No-descriptor |
| Register space | 4 KB (BAR0) | Memory-mapped I/O |

---

## How the Device Works

### Frame Transmission

The device contains 250 pre-loaded data frames, each exactly 5120 bytes. When
you start the device (set CTRL.RUN), it begins transmitting these frames in
order, looping back to frame 0 after frame 249.

**Transmission timing:**

```
Frame rate = 25 fps (default)
Frame interval = 1000ms / 25 = 40ms

Time:  0ms     40ms    80ms    120ms   160ms   ...
       |       |       |       |       |
Frame: [0]     [1]     [2]     [3]     [4]     ...
```

Each frame is transmitted atomically - the device doesn't send partial frames.
If a descriptor isn't available when it's time to send, the frame is dropped
(not queued).

### The Descriptor Ring

Instead of giving the device one big buffer, you give it a list of smaller
buffers via a "descriptor ring". Each descriptor says: "here's a buffer at
address X with size Y, put a frame there."

The ring is managed with two indices:
- **HEAD** (driver writes): "I've prepared descriptors up to here"
- **TAIL** (device writes): "I've completed descriptors up to here"

```
Ring state:  [completed] [completed] [pending] [pending] [free] [free]
              ^                       ^                          ^
              |                       |                          |
            TAIL                    HEAD                    (next HEAD)
```

**Pending** = descriptors submitted but not yet used
**Free** = slots you can fill with new descriptors

### Backpressure and Drops

Real hardware doesn't wait. If the device has a frame ready and you haven't
given it a descriptor to use, the frame is dropped. This mimics real streaming
devices (cameras, network cards, sensors) where data keeps coming whether
you're ready or not.

```
Device: "Frame 42 ready... any descriptors? No? *drops* Frame 43 ready..."

STAT_FRAMES_DROP counter increments for each dropped frame.
IRQ_STATUS.NO_DESC is set (triggers interrupt if enabled).
```

**What to do about drops:**
1. Use a larger descriptor ring (more buffers in flight)
2. Lower the frame rate (give yourself more time)
3. Fix your driver's latency (process completions faster)

### Interrupt Coalescing

Without coalescing: 25 fps = 25 interrupts/second. That's fine for a demo.

With coalescing: batch completions before firing. "Tell me after 8 frames OR
40ms, whichever comes first." Reduces interrupt overhead significantly.

```
Coalescing disabled:    IRQ IRQ IRQ IRQ IRQ IRQ IRQ IRQ  (8 IRQs)
Coalescing (count=4):   ....IRQ....IRQ                   (2 IRQs)
Coalescing (timeout):   ........IRQ                      (1 IRQ after timeout)
```

### Reset Behavior

Writing CTRL.RESET performs a soft reset:
- Stops transmission immediately
- Clears HEAD and TAIL indices to 0
- Resets all statistics counters
- Restores FRAME_RATE to default (25)
- Preserves descriptor ring address (no need to reprogram)
- RESET bit auto-clears when complete

After reset, you need to resubmit descriptors and restart.

---

## PCI Configuration

| Field | Value | Notes |
|-------|-------|-------|
| Vendor ID | 0x0DAD | "Dad" - because this device is here to teach you |
| Device ID | 0xF00D | "Food" - data to consume |
| Subsystem Vendor | 0x0DAD | |
| Subsystem ID | 0x0003 | v3.0 - SG-DMA Edition |
| Revision | 0x03 | Version 3 |
| Class | 0xFF0000 | "Other" device class |

## BARs

| BAR | Type | Size | Content |
|-----|------|------|---------|
| BAR0 | Memory, 32-bit | 4 KB | Device registers |
| BAR1 | (unused) | - | - |

MSI-X table and PBA are located within BAR0:
- MSI-X Table: offset 0x800
- MSI-X PBA: offset 0xC00

---

## The Big Picture: Scatter-Gather DMA

Before we dive into registers, let's understand what we're building.

Traditional DMA: "Here's a big contiguous buffer, write everything there."
Scatter-Gather DMA: "Here's a list of buffers, figure it out."

Why is SG-DMA better? Because real kernels can't always give you giant contiguous memory chunks. With SG-DMA, you allocate many smaller buffers (often just single pages) and tell the device where they all are.

### How It Works

```
Driver (you)                         Device
-----------                         ------
1. Allocate frame buffers
   (one per descriptor)

2. Build descriptor ring:
   desc[0].dst_addr = buffer_0
   desc[1].dst_addr = buffer_1
   ...

3. Tell device where ring is:
   DESC_RING_LO/HI = ring_addr

4. Submit descriptors:
   DESC_HEAD = N
   "I've prepared N buffers"
                                   5. Device sees HEAD moved
                                      For each pending descriptor:
                                        - DMA frame to dst_addr
                                        - Write completion info
                                        - Advance TAIL
                                        - Maybe fire IRQ

6. IRQ arrives (or poll)
   Read TAIL, process completed
   frames, resubmit descriptors

7. Goto 6 forever
```

The key insight: HEAD is how you "submit" work to the device. TAIL is how the device tells you it finished. The gap between them is your pending work.

### The Descriptor Ring

```
                    +---+---+---+---+---+---+---+---+
Descriptor Ring:    | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 |  (ring_size = 8)
                    +---+---+---+---+---+---+---+---+
                          ^               ^
                          |               |
                        TAIL            HEAD
                      (device)         (driver)
                    "I'm done"      "Work to do"

Pending = (HEAD - TAIL) mod ring_size = 3 descriptors
Free    = ring_size - 1 - pending = 4 slots available
```

Why "ring_size - 1"? We keep one slot empty to distinguish "full" from "empty" (otherwise HEAD == TAIL means both).

---

## Register Map

All registers are 32-bit and must be accessed with 32-bit aligned reads/writes.

| Offset | Name | Access | Reset Value | Description |
|--------|------|--------|-------------|-------------|
| 0x000 | DEV_ID | R | 0xF00DFACE | Device identification |
| 0x004 | DEV_VER | R | 0x00030000 | Device version (3.0.0) |
| 0x008 | CTRL | R/W | 0x00000000 | Control register |
| 0x00C | STATUS | R | 0x00000000 | Status register |
| 0x010 | FRAME_SIZE | R | 0x00001400 | Frame size: 5120 bytes |
| 0x014 | FRAME_COUNT | R | 0x000000FA | Total frames: 250 |
| 0x018 | FRAME_RATE | R/W | 0x00000019 | Frames per second (25) |
| 0x01C | CURRENT_FRAME | R | 0x00000000 | Current frame index (0-249) |
| 0x020 | DESC_RING_LO | R/W | 0x00000000 | Descriptor ring addr [31:0] |
| 0x024 | DESC_RING_HI | R/W | 0x00000000 | Descriptor ring addr [63:32] |
| 0x028 | DESC_RING_SIZE | R/W | 0x00000100 | Descriptor count (256) |
| 0x02C | DESC_HEAD | R/W | 0x00000000 | Head - driver submits |
| 0x030 | DESC_TAIL | R | 0x00000000 | Tail - device completes |
| 0x034 | IRQ_STATUS | R/W1C | 0x00000000 | Interrupt status |
| 0x038 | IRQ_MASK | R/W | 0x00000000 | Interrupt enable mask |
| 0x03C | IRQ_COALESCE | R/W | 0x00000000 | Coalesce settings |
| 0x040 | STAT_FRAMES_TX | R | 0x00000000 | Frames transmitted |
| 0x044 | STAT_FRAMES_DROP | R | 0x00000000 | Frames dropped |
| 0x048 | STAT_BYTES_LO | R | 0x00000000 | Total bytes [31:0] |
| 0x04C | STAT_BYTES_HI | R | 0x00000000 | Total bytes [63:32] |
| 0x050 | STAT_DESC_COMPL | R | 0x00000000 | Descriptors completed |
| 0x054 | STAT_ERRORS | R | 0x00000000 | Error count |
| 0x058 | FAULT_INJECT | R/W | 0x00000000 | Fault injection control |
| 0x05C | FAULT_RATE | R/W | 0x000003E8 | Fault rate (1/1000) |

---

## Register Details

### DEV_ID (0x000) - Device Identification

**Read-only**

```
 31                               0
+----------------------------------+
|           0xF00DFACE             |
+----------------------------------+
```

Always returns `0xF00DFACE`. Use this to verify the device is present
and responding correctly during probe.

---

### DEV_VER (0x004) - Device Version

**Read-only**

```
 31        24 23        16 15         8 7          0
+------------+------------+------------+------------+
|   Reserved |    Major   |    Minor   |   Patch    |
+------------+------------+------------+------------+
```

Current value: `0x00030000` = version 3.0.0

| Field | Bits | Description |
|-------|------|-------------|
| Patch | 7:0 | Patch version (0) |
| Minor | 15:8 | Minor version (0) |
| Major | 23:16 | Major version (3) |
| Reserved | 31:24 | Always 0 |

---

### CTRL (0x008) - Control Register

**Read/Write**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |IRQ|RST|RUN|
+----------------------------------+---+---+---+
```

| Bit | Name | Access | Description |
|-----|------|--------|-------------|
| 0 | RUN | R/W | Enable transmission |
| 1 | RESET | W | Soft reset (self-clearing) |
| 2 | IRQ_EN | R/W | Global interrupt enable |
| 31:3 | Reserved | - | Always 0 |

**RUN (bit 0):** Setting this starts the transmission engine. The device will
begin sending frames at the configured rate, using whatever descriptors are
available. Clear to stop. STATUS.RUNNING reflects actual state.

**RESET (bit 1):** Writing 1 triggers a soft reset. This bit auto-clears.
Reset stops transmission, clears HEAD/TAIL indices, resets all counters,
and restores default configuration. The descriptor ring address is preserved.

**IRQ_EN (bit 2):** Global interrupt enable. When clear, no MSI-X interrupts
are delivered regardless of IRQ_STATUS and IRQ_MASK settings.

---

### STATUS (0x00C) - Status Register

**Read-only**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |ERR|EMP|RUN|
+----------------------------------+---+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | RUNNING | Device is actively transmitting |
| 1 | DESC_EMPTY | No descriptors available (HEAD == TAIL) |
| 2 | ERROR | Error condition (DMA failure, config error) |
| 31:3 | Reserved | Always 0 |

**DESC_EMPTY** means the device has more frames to send but you haven't given
it anywhere to put them. If this stays set while RUNNING, you're dropping
frames - check STAT_FRAMES_DROP.

---

### FRAME_SIZE (0x010) - Frame Size

**Read-only**

```
 31                               0
+----------------------------------+
|              5120                |
+----------------------------------+
```

Fixed at 5120 bytes. This is the size of each frame the device transmits.
Read-only because you can't change reality.

---

### FRAME_COUNT (0x014) - Frame Count

**Read-only**

```
 31                               0
+----------------------------------+
|               250                |
+----------------------------------+
```

Total number of frames available in the device. The device loops through
these continuously - after frame 249 comes frame 0 again.

---

### FRAME_RATE (0x018) - Frame Rate

**Read/Write**

```
 31                               0
+----------------------------------+
|          Frame rate (fps)        |
+----------------------------------+
```

Target frame transmission rate in frames per second.

| Limit | Value |
|-------|-------|
| Minimum | 1 fps |
| Maximum | 60 fps |
| Default | 25 fps |

Can be changed while running - takes effect on next frame interval.

**Pro tip:** If you're seeing dropped frames, lowering this might help. Unless
your driver is the problem, in which case... fix your driver.

---

### CURRENT_FRAME (0x01C) - Current Frame Index

**Read-only**

```
 31                               0
+----------------------------------+
|        Current frame (0-249)     |
+----------------------------------+
```

The frame index currently being transmitted. Wraps from 249 back to 0.
Useful for debugging "is this thing even running?"

---

### DESC_RING_LO (0x020) - Descriptor Ring Address Low

**Read/Write**

```
 31                               0
+----------------------------------+
|      Ring base address [31:0]    |
+----------------------------------+
```

Lower 32 bits of the descriptor ring physical address. Must be 32-byte aligned
(descriptors are 32 bytes each).

---

### DESC_RING_HI (0x024) - Descriptor Ring Address High

**Read/Write**

```
 31                               0
+----------------------------------+
|      Ring base address [63:32]   |
+----------------------------------+
```

Upper 32 bits for 64-bit physical addresses. Set to 0 if your addresses fit
in 32 bits.

**Setting the ring address:**
```c
dma_addr_t ring_dma = pfdev->desc_ring_dma;
pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_LO, lower_32_bits(ring_dma));
pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_RING_HI, upper_32_bits(ring_dma));
```

---

### DESC_RING_SIZE (0x028) - Descriptor Ring Size

**Read/Write**

```
 31                               0
+----------------------------------+
|      Descriptor count            |
+----------------------------------+
```

Number of descriptors in the ring.

| Limit | Value |
|-------|-------|
| Minimum | 4 |
| Maximum | 4096 |
| Default | 256 |

**Must be a power of 2** for efficient index wrapping. Non-power-of-2 values
are rounded down to the nearest power of 2.

Set this before starting transmission. The ring must have at least this many
valid descriptor entries, each pointing to a buffer >= FRAME_SIZE bytes.

---

### DESC_HEAD (0x02C) - Descriptor Head Index

**Read/Write**

```
 31                               0
+----------------------------------+
|          Head index              |
+----------------------------------+
```

Write this to submit descriptors to the device. When you've prepared
descriptors N through M-1, write M here. The device will process all
descriptors from TAIL up to (but not including) HEAD.

Range: 0 to ring_size - 1 (wraps automatically via modulo)

**Submitting work:**
```c
// Prepare descriptor at index 'next'
pfdev->desc_ring[next].dst_addr = cpu_to_le64(buffer_dma);
pfdev->desc_ring[next].length = cpu_to_le32(buffer_size);
pfdev->desc_ring[next].control = cpu_to_le32(0);  // Clear completed flag
wmb();  // Ensure descriptor visible before HEAD update
pfpga_write32(pfdev, PHANTOMFPGA_REG_DESC_HEAD, (next + 1) & (ring_size - 1));
```

---

### DESC_TAIL (0x030) - Descriptor Tail Index

**Read-only**

```
 31                               0
+----------------------------------+
|          Tail index              |
+----------------------------------+
```

The device updates this after completing each descriptor. When TAIL advances,
it means frames have been written to your buffers.

**Processing completions:**
```c
u32 tail = pfpga_read32(pfdev, PHANTOMFPGA_REG_DESC_TAIL);
while (pfdev->last_tail != tail) {
    struct phantomfpga_sg_desc *desc = &pfdev->desc_ring[pfdev->last_tail];
    void *buffer = pfdev->buffers[pfdev->last_tail];
    // Process the completed frame in 'buffer'
    pfdev->last_tail = (pfdev->last_tail + 1) & (ring_size - 1);
}
```

---

### IRQ_STATUS (0x034) - Interrupt Status

**Read/Write (Write-1-to-Clear)**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |NDS|ERR|CMP|
+----------------------------------+---+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | COMPLETE | Descriptor(s) completed |
| 1 | ERROR | Error occurred |
| 2 | NO_DESC | Frame dropped - no descriptor available |
| 31:3 | Reserved | Always 0 |

**Write-1-to-Clear (W1C):** Write a 1 to a bit position to clear that bit.
Writing 0 has no effect.

---

### IRQ_MASK (0x038) - Interrupt Mask

**Read/Write**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |NDS|ERR|CMP|
+----------------------------------+---+---+---+
```

Same bit layout as IRQ_STATUS. Set bits to enable corresponding interrupts.

An interrupt is delivered only when:
1. The corresponding IRQ_STATUS bit is set
2. The corresponding IRQ_MASK bit is set
3. CTRL.IRQ_EN is set

**MSI-X Vector Mapping:**
| Condition | Vector |
|-----------|--------|
| COMPLETE | 0 |
| ERROR | 1 |
| NO_DESC | 2 |

---

### IRQ_COALESCE (0x03C) - Interrupt Coalescing

**Read/Write**

```
 31              16 15               0
+-----------------+------------------+
| Timeout (us)    | Count threshold  |
+-----------------+------------------+
```

| Field | Bits | Description |
|-------|------|-------------|
| Count | 15:0 | Fire IRQ after N descriptor completions |
| Timeout | 31:16 | Fire IRQ after N microseconds if any pending |

**How coalescing works:**

Without coalescing, you get an interrupt for every completed descriptor.
At 25 fps, that's 25 interrupts per second - fine for a demo, brutal in
production.

With coalescing, the device batches completions:
- **Count mode:** Wait until N descriptors complete, then fire
- **Timeout mode:** Even if count not reached, fire after N microseconds

Set both: "fire after 8 completions OR 40ms, whichever comes first."

**Example - reasonable defaults for 25fps:**
```c
u32 coalesce = phantomfpga_irq_coalesce_pack(8, 40000);  // 8 frames or 40ms
pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_COALESCE, coalesce);
```

---

### STAT_FRAMES_TX (0x040) - Frames Transmitted

**Read-only**

Running count of frames successfully DMA'd to host memory since last reset.

---

### STAT_FRAMES_DROP (0x044) - Frames Dropped

**Read-only**

Frames the device wanted to send but couldn't because no descriptors were
available. If this number is growing, you're not submitting descriptors fast
enough. Either increase your ring size, lower the frame rate, or fix your
driver's latency.

---

### STAT_BYTES_LO/HI (0x048/0x04C) - Total Bytes

**Read-only**

64-bit counter of total bytes transferred. At 5120 bytes/frame and 25fps,
this overflows a 32-bit counter in about 9 hours. Hence the 64-bit version.

---

### STAT_DESC_COMPL (0x050) - Descriptors Completed

**Read-only**

Total number of descriptors the device has completed. Should equal
STAT_FRAMES_TX (one descriptor per frame in normal operation).

---

### STAT_ERRORS (0x054) - Error Count

**Read-only**

DMA errors, timeout errors, anything that went wrong. Non-zero here means
check your buffer addresses and sizes.

---

### FAULT_INJECT (0x058) - Fault Injection Control

**Read/Write**

```
 31                                 4   3   2   1   0
+----------------------------------+---+---+---+---+
|             Reserved             |SKP|DAT|CRC|DRP|
+----------------------------------+---+---+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | DROP_FRAME | Randomly drop frames |
| 1 | CORRUPT_CRC | Write incorrect CRC values |
| 2 | CORRUPT_DATA | Flip bits in frame data |
| 3 | SKIP_SEQUENCE | Skip sequence numbers randomly |
| 31:4 | Reserved | Always 0 |

Enable these to test your error handling:
- **DROP_FRAME:** Tests gap detection in your sequence tracking
- **CORRUPT_CRC:** Tests CRC validation (you ARE validating CRCs, right?)
- **CORRUPT_DATA:** Tests... well, what happens when data is garbage
- **SKIP_SEQUENCE:** Tests sequence number discontinuity handling

---

### FAULT_RATE (0x05C) - Fault Probability

**Read/Write**

```
 31                               0
+----------------------------------+
|          Rate (1 in N)           |
+----------------------------------+
```

Probability of fault injection: approximately 1 in N frames affected.
Default 1000 = roughly 0.1% of frames corrupted when faults enabled.

Set to lower values for more aggressive testing. Set to 0 to disable
probability check (every frame affected - chaotic mode).

---

## Descriptor Format

Each descriptor is 32 bytes. The ring is a contiguous array of these.

```
Offset  Size   Field        Description
------  ----   -----        -----------
0x00    4      control      Flags: COMPLETED, EOP, SOP, IRQ, STOP
0x04    4      length       Buffer size in bytes
0x08    8      dst_addr     Host destination address (physical)
0x10    8      next_desc    Unused in ring mode (reserved)
0x18    8      reserved     Alignment padding
------  ----
        32     TOTAL
```

### Control Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | COMPLETED | Device sets when DMA complete |
| 1 | EOP | End of packet (device sets) |
| 2 | SOP | Start of packet (device sets) |
| 3 | IRQ | Request interrupt on completion |
| 4 | STOP | Stop after this descriptor |

**COMPLETED:** You clear this when setting up the descriptor. The device sets
it when done. Check this to confirm completion (in addition to TAIL movement).

**IRQ:** Set this if you want an interrupt specifically for this descriptor.
Combined with coalescing, lets you control exactly when you wake up.

### C Structure

```c
struct phantomfpga_sg_desc {
    __le32 control;      /* Flags */
    __le32 length;       /* Buffer length in bytes */
    __le64 dst_addr;     /* Host destination (physical) address */
    __le64 next_desc;    /* Reserved (unused in ring mode) */
    __le64 reserved;     /* Alignment padding */
} __packed;
```

---

## Completion Writeback

When a descriptor completes, the device writes a 16-byte completion structure
at the END of the destination buffer (buffer + length - 16):

```
Offset  Size   Field          Description
------  ----   -----          -----------
0x00    4      status         0=OK, 1=DMA_ERROR, 2=OVERFLOW
0x04    4      actual_length  Bytes actually transferred
0x08    8      timestamp      Device timestamp (nanoseconds)
------  ----
        16     TOTAL
```

### C Structure

```c
struct phantomfpga_completion {
    __le32 status;         /* 0=OK, else error code */
    __le32 actual_length;  /* Bytes transferred */
    __le64 timestamp;      /* Nanoseconds since device start */
} __packed;
```

**Buffer size:** Your buffers must be at least frame_size + 16 bytes to
accommodate both the frame data and the completion writeback. Or allocate
frame_size and read completion from descriptor ring (device updates control
field with completion status).

---

## Frame Format

Each frame transmitted by the device is exactly 5120 bytes:

```
Offset  Size   Field        Description
------  ----   -----        -----------
0x0000  4      magic        0xF00DFACE
0x0004  4      sequence     Frame index (0-249, wraps)
0x0008  8      reserved     Reserved (must be 0)
0x0010  4995   data         The actual payload data
0x1393  105    padding      Zero bytes
0x13FC  4      crc32        CRC-32 of bytes 0x0000-0x13FB
------  ----
        5120   TOTAL
```

### Frame Header (16 bytes)

```c
struct phantomfpga_frame_header {
    __le32 magic;       /* 0xF00DFACE - check this first! */
    __le32 sequence;    /* 0-249, loops forever */
    __le64 reserved;    /* Reserved (must be 0) */
} __packed;
```

### CRC-32

The device uses IEEE 802.3 (Ethernet) CRC-32, polynomial 0xEDB88320.
The CRC covers bytes 0 through 5115 (everything except the CRC itself).

**Validation pseudocode:**
```c
uint32_t expected_crc = le32_to_cpu(*(__le32 *)(frame + 5116));
uint32_t computed_crc = crc32(0, frame, 5116);
if (expected_crc != computed_crc) {
    // Frame is corrupt - handle accordingly
}
```

---

## Programming Sequence

### Driver Initialization

```
1. Probe: pci_enable_device()
2. Request BAR0: pci_request_region()
3. Map BAR0: pci_iomap()
4. Verify: read DEV_ID, check == 0xF00DFACE
5. Bus mastering: pci_set_master()
6. DMA mask: dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))
7. MSI-X: pci_alloc_irq_vectors(3 vectors), request_irq() for each
8. Allocate descriptor ring: dma_alloc_coherent()
9. Allocate frame buffers: N x dma_alloc_coherent() where N = ring_size
10. Soft reset: write CTRL_RESET, wait for STATUS.RUNNING == 0
11. Set ring address: DESC_RING_LO/HI = ring_dma
12. Set ring size: DESC_RING_SIZE = N
13. Create char device: cdev_add(), device_create()
```

### Starting Transmission

```
1. Initialize all descriptors in ring
   For i = 0 to ring_size-1:
     desc[i].dst_addr = buffer_dma[i]
     desc[i].length = FRAME_SIZE
     desc[i].control = 0

2. Submit all descriptors: DESC_HEAD = ring_size - 1

3. Configure interrupts:
   IRQ_MASK = COMPLETE | ERROR | NO_DESC
   IRQ_COALESCE = (8 << 0) | (40000 << 16)

4. Start: CTRL = RUN | IRQ_EN
```

### Interrupt Handler

```
1. Read IRQ_STATUS
2. If COMPLETE:
   - Read DESC_TAIL
   - For each completed descriptor (last_tail to tail):
     - Read completion status
     - Process frame if valid
     - Mark descriptor for reuse
   - Resubmit descriptors: update DESC_HEAD
   - Clear: write COMPLETE to IRQ_STATUS
3. If NO_DESC:
   - You're falling behind - consider logging
   - Clear: write NO_DESC to IRQ_STATUS
4. If ERROR:
   - Check STAT_ERRORS
   - Handle appropriately (maybe reset device)
   - Clear: write ERROR to IRQ_STATUS
```

### Shutdown

```
1. Stop: clear CTRL.RUN
2. Wait: spin until STATUS.RUNNING == 0
3. Disable IRQs: clear CTRL.IRQ_EN
4. Free IRQs: free_irq() for each vector
5. Release MSI-X: pci_free_irq_vectors()
6. Free buffers: dma_free_coherent() for each
7. Free descriptor ring: dma_free_coherent()
8. Unmap BAR0: pci_iounmap()
9. Release region: pci_release_region()
10. Disable device: pci_disable_device()
```

---

## Quick Reference Card

### Registers (Offset from BAR0)

```
0x000 DEV_ID          R     Device ID (0xF00DFACE)
0x004 DEV_VER         R     Version (0x00030000 = v3.0.0)
0x008 CTRL            R/W   [2]=IRQ_EN [1]=RESET [0]=RUN
0x00C STATUS          R     [2]=ERROR [1]=DESC_EMPTY [0]=RUNNING
0x010 FRAME_SIZE      R     5120 bytes per frame
0x014 FRAME_COUNT     R     250 frames total
0x018 FRAME_RATE      R/W   Frames per second (1-60, default 25)
0x01C CURRENT_FRAME   R     Frame index being sent (0-249)
0x020 DESC_RING_LO    R/W   Descriptor ring address [31:0]
0x024 DESC_RING_HI    R/W   Descriptor ring address [63:32]
0x028 DESC_RING_SIZE  R/W   Number of descriptors (power of 2)
0x02C DESC_HEAD       R/W   Head index (driver submits)
0x030 DESC_TAIL       R     Tail index (device completes)
0x034 IRQ_STATUS      R/W1C [2]=NO_DESC [1]=ERROR [0]=COMPLETE
0x038 IRQ_MASK        R/W   Same bits - enable interrupts
0x03C IRQ_COALESCE    R/W   [31:16]=timeout_us [15:0]=count
0x040 STAT_FRAMES_TX  R     Frames transmitted
0x044 STAT_FRAMES_DROP R    Frames dropped (no descriptor)
0x048 STAT_BYTES_LO   R     Total bytes [31:0]
0x04C STAT_BYTES_HI   R     Total bytes [63:32]
0x050 STAT_DESC_COMPL R     Descriptors completed
0x054 STAT_ERRORS     R     Error count
0x058 FAULT_INJECT    R/W   [3]=SKIP_SEQ [2]=DATA [1]=CRC [0]=DROP
0x05C FAULT_RATE      R/W   Fault probability (1/N, default 1000)
```

### Descriptor (32 bytes)

```
+00: control   u32   [4]=STOP [3]=IRQ [2]=SOP [1]=EOP [0]=COMPLETED
+04: length    u32   Buffer size in bytes
+08: dst_addr  u64   Physical address of buffer
+10: next_desc u64   (unused in ring mode)
+18: reserved  u64   Alignment padding
```

### Frame Header (16 bytes)

```
+00: magic     u32   0xF00DFACE
+04: sequence  u32   Frame index (0-249)
+08: reserved  u64   Reserved (must be 0)
+10: data...         5104 bytes (4995 data + 105 padding + 4 CRC)
```

### IOCTLs (Magic 'P')

```
SET_CFG          _IOW('P', 1, config)   Configure device
GET_CFG          _IOR('P', 2, config)   Get configuration
START            _IO('P', 3)            Start streaming
STOP             _IO('P', 4)            Stop streaming
GET_STATS        _IOR('P', 5, stats)    Get statistics
RESET_STATS      _IO('P', 6)            Reset driver stats
GET_BUFFER_INFO  _IOR('P', 7, info)     Get mmap info
CONSUME_FRAME    _IO('P', 8)            Consume one frame
SET_FAULT        _IOW('P', 9, fault)    Configure fault injection
```

---

## Error Codes

Standard errno values:

| Error | Value | Meaning |
|-------|-------|---------|
| EINVAL | 22 | Invalid parameter |
| EBUSY | 16 | Device busy (streaming when config attempted) |
| EAGAIN | 11 | No data available (poll or non-blocking read) |
| ENOMEM | 12 | Memory allocation failed |
| EIO | 5 | Hardware communication error |
| ENODEV | 19 | Device not found |

---

*"The difference between theory and practice is smaller in theory than in practice."*
*- Someone who definitely debugged a DMA driver at 3 AM*
