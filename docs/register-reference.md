# PhantomFPGA Register Reference

> "This is where the rubber meets the road. Or rather, where your code meets the hardware."

This document describes every register, every bit, and every magic number you need
to talk to the PhantomFPGA device. Keep this open while you're coding - you'll
refer to it approximately 47 times per implementation session.

(That number is made up. The real number is probably higher.)

## PCI Configuration

| Field | Value | Notes |
|-------|-------|-------|
| Vendor ID | 0x1DAD | "Dad" - because this device is here to teach you |
| Device ID | 0xF00D | "Food" - frame data to consume |
| Subsystem Vendor | 0x1DAD | |
| Subsystem ID | 0x0001 | |
| Revision | 0x01 | Version 1 |
| Class | 0xFF0000 | "Other" device class |

## BARs

| BAR | Type | Size | Content |
|-----|------|------|---------|
| BAR0 | Memory, 32-bit | 4 KB | Device registers |
| BAR1 | (unused) | - | - |

MSI-X table and PBA are located within BAR0:
- MSI-X Table: offset 0x800
- MSI-X PBA: offset 0xC00

## Register Map

All registers are 32-bit and must be accessed with 32-bit aligned reads/writes.

| Offset | Name | Access | Reset Value | Description |
|--------|------|--------|-------------|-------------|
| 0x000 | DEV_ID | R | 0xF00DFACE | Device identification |
| 0x004 | DEV_VER | R | 0x00010000 | Device version (1.0.0) |
| 0x008 | CTRL | R/W | 0x00000000 | Control register |
| 0x00C | STATUS | R | 0x00000000 | Status register |
| 0x010 | FRAME_SIZE | R/W | 0x00001000 | Frame size in bytes (4096) |
| 0x014 | FRAME_RATE | R/W | 0x000003E8 | Frame rate in Hz (1000) |
| 0x018 | WATERMARK | R/W | 0x00000040 | IRQ threshold (64) |
| 0x01C | RING_SIZE | R/W | 0x00000100 | Ring entries (256) |
| 0x020 | DMA_ADDR_LO | R/W | 0x00000000 | DMA base address [31:0] |
| 0x024 | DMA_ADDR_HI | R/W | 0x00000000 | DMA base address [63:32] |
| 0x028 | DMA_SIZE | R/W | 0x00000000 | Total DMA buffer size |
| 0x02C | PROD_IDX | R | 0x00000000 | Producer index |
| 0x030 | CONS_IDX | R/W | 0x00000000 | Consumer index |
| 0x034 | IRQ_STATUS | R/W | 0x00000000 | Interrupt status (W1C) |
| 0x038 | IRQ_MASK | R/W | 0x00000000 | Interrupt enable mask |
| 0x03C | STAT_FRAMES | R | 0x00000000 | Total frames produced |
| 0x040 | STAT_ERRORS | R | 0x00000000 | Error count |
| 0x044 | STAT_OVERRUNS | R | 0x00000000 | Overrun count |
| 0x048 | FAULT_INJECT | R/W | 0x00000000 | Fault injection control |

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

Current value: `0x00010000` = version 1.0.0

| Field | Bits | Description |
|-------|------|-------------|
| Patch | 7:0 | Patch version |
| Minor | 15:8 | Minor version |
| Major | 23:16 | Major version |
| Reserved | 31:24 | Always 0 |

---

### CTRL (0x008) - Control Register

**Read/Write**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |IRQ|RST|STR|
+----------------------------------+---+---+---+
```

| Bit | Name | Access | Description |
|-----|------|--------|-------------|
| 0 | START | R/W | Enable frame production |
| 1 | RESET | W | Soft reset (self-clearing) |
| 2 | IRQ_EN | R/W | Global interrupt enable |
| 31:3 | Reserved | - | Always 0 |

**Bit Definitions:**

- **START (bit 0)**: When set, the device starts producing frames at the
  configured rate. Clear to stop production. The STATUS.RUNNING flag reflects
  the actual running state.

- **RESET (bit 1)**: Writing 1 triggers a soft reset. This bit auto-clears.
  Reset stops frame production, clears all indices and counters, and restores
  default configuration values.

- **IRQ_EN (bit 2)**: Global interrupt enable. When clear, no MSI-X interrupts
  are delivered regardless of IRQ_STATUS and IRQ_MASK.

**Example - Starting the device:**
```c
// Enable interrupts and start
pfpga_write32(pfdev, PHANTOMFPGA_REG_CTRL,
              PHANTOMFPGA_CTRL_START | PHANTOMFPGA_CTRL_IRQ_EN);
```

---

### STATUS (0x00C) - Status Register

**Read-only**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |ERR|OVR|RUN|
+----------------------------------+---+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | RUNNING | Device is actively producing frames |
| 1 | OVERRUN | Ring buffer full (cleared when space available) |
| 2 | ERROR | Error condition (DMA failure, config error) |
| 31:3 | Reserved | Always 0 |

---

### FRAME_SIZE (0x010) - Frame Size

**Read/Write**

```
 31                               0
+----------------------------------+
|          Frame size (bytes)      |
+----------------------------------+
```

Size of each frame slot in the ring buffer, in bytes.

| Limit | Value |
|-------|-------|
| Minimum | 64 bytes |
| Maximum | 65536 bytes (64 KB) |
| Default | 4096 bytes (4 KB) |

The frame size includes the 24-byte header. Payload size = frame_size - 24.

Values below minimum are clamped to minimum. Values above maximum are
clamped to maximum. The device logs a warning when clamping occurs.

**Note:** Only configure when device is stopped.

---

### FRAME_RATE (0x014) - Frame Rate

**Read/Write**

```
 31                               0
+----------------------------------+
|          Frame rate (Hz)         |
+----------------------------------+
```

Target frame production rate in frames per second.

| Limit | Value |
|-------|-------|
| Minimum | 1 Hz |
| Maximum | 100000 Hz (100 kHz) |
| Default | 1000 Hz (1 kHz) |

Can be changed while running - takes effect on next timer tick.

---

### WATERMARK (0x018) - IRQ Watermark Threshold

**Read/Write**

```
 31                               0
+----------------------------------+
|         Watermark (frames)       |
+----------------------------------+
```

Number of pending frames that triggers a watermark interrupt.

| Limit | Value |
|-------|-------|
| Minimum | 1 |
| Maximum | ring_size - 1 |
| Default | 64 |

When `pending_frames >= watermark`, the device sets IRQ_WATERMARK.

Lower values = more interrupts, lower latency.
Higher values = fewer interrupts, better batching.

---

### RING_SIZE (0x01C) - Ring Buffer Size

**Read/Write**

```
 31                               0
+----------------------------------+
|       Ring size (entries)        |
+----------------------------------+
```

Number of frame slots in the ring buffer.

| Limit | Value |
|-------|-------|
| Minimum | 4 |
| Maximum | 4096 |
| Default | 256 |

**Must be a power of 2** for efficient index wrapping. Non-power-of-2 values
are rounded down to the nearest power of 2.

The actual DMA buffer size required is: `ring_size * frame_size`

**Note:** Only configure when device is stopped.

---

### DMA_ADDR_LO (0x020) - DMA Address Low

**Read/Write**

```
 31                               0
+----------------------------------+
|       DMA base address [31:0]    |
+----------------------------------+
```

Lower 32 bits of the DMA buffer physical address.

---

### DMA_ADDR_HI (0x024) - DMA Address High

**Read/Write**

```
 31                               0
+----------------------------------+
|       DMA base address [63:32]   |
+----------------------------------+
```

Upper 32 bits of the DMA buffer physical address.

**Setting the DMA address (64-bit example):**
```c
dma_addr_t addr = pfdev->dma_handle;
pfpga_write32(pfdev, PHANTOMFPGA_REG_DMA_ADDR_LO, lower_32_bits(addr));
pfpga_write32(pfdev, PHANTOMFPGA_REG_DMA_ADDR_HI, upper_32_bits(addr));
```

---

### DMA_SIZE (0x028) - DMA Buffer Size

**Read/Write**

```
 31                               0
+----------------------------------+
|      Total buffer size (bytes)   |
+----------------------------------+
```

Total size of the DMA buffer in bytes. Should equal `ring_size * frame_size`.

The device uses this for internal validation and does not read beyond this size.

---

### PROD_IDX (0x02C) - Producer Index

**Read-only**

```
 31                               0
+----------------------------------+
|          Producer index          |
+----------------------------------+
```

Current write position in the ring buffer. Managed by the device.

Range: 0 to ring_size - 1

The device increments this after writing each frame.

---

### CONS_IDX (0x030) - Consumer Index

**Read/Write**

```
 31                               0
+----------------------------------+
|          Consumer index          |
+----------------------------------+
```

Current read position in the ring buffer. Managed by the driver.

Range: 0 to ring_size - 1

The driver must advance this after consuming each frame to free buffer space.

**Warning:** Values >= ring_size are masked to valid range.

---

### IRQ_STATUS (0x034) - Interrupt Status

**Read/Write (Write-1-to-Clear)**

```
 31                                     2   1   0
+--------------------------------------+---+---+
|              Reserved                |OVR|WM |
+--------------------------------------+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | WATERMARK | Pending frames reached watermark threshold |
| 1 | OVERRUN | Ring buffer overflowed (frame dropped) |
| 31:2 | Reserved | Always 0 |

**Write-1-to-Clear (W1C):** Write a 1 to a bit position to clear that bit.
Writing 0 has no effect. This is the standard pattern for interrupt status
registers.

**Example - Clearing watermark interrupt:**
```c
// Read status
u32 status = pfpga_read32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS);

// Clear watermark bit by writing 1 to it
if (status & PHANTOMFPGA_IRQ_WATERMARK) {
    pfpga_write32(pfdev, PHANTOMFPGA_REG_IRQ_STATUS, PHANTOMFPGA_IRQ_WATERMARK);
}
```

---

### IRQ_MASK (0x038) - Interrupt Mask

**Read/Write**

```
 31                                     2   1   0
+--------------------------------------+---+---+
|              Reserved                |OVR|WM |
+--------------------------------------+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | WATERMARK | Enable watermark interrupt |
| 1 | OVERRUN | Enable overrun interrupt |
| 31:2 | Reserved | Always 0 |

An interrupt is delivered only when:
1. The corresponding IRQ_STATUS bit is set
2. The corresponding IRQ_MASK bit is set
3. CTRL.IRQ_EN is set

**MSI-X Vector Mapping:**
| Condition | Vector |
|-----------|--------|
| WATERMARK | 0 |
| OVERRUN | 1 |

---

### STAT_FRAMES (0x03C) - Frames Produced Counter

**Read-only**

```
 31                               0
+----------------------------------+
|       Total frames produced      |
+----------------------------------+
```

Running count of successfully produced frames since last reset.

---

### STAT_ERRORS (0x040) - Error Counter

**Read-only**

```
 31                               0
+----------------------------------+
|          Error count             |
+----------------------------------+
```

Count of DMA errors (failed writes, invalid addresses).

---

### STAT_OVERRUNS (0x044) - Overrun Counter

**Read-only**

```
 31                               0
+----------------------------------+
|          Overrun count           |
+----------------------------------+
```

Count of dropped frames due to ring buffer full.

---

### FAULT_INJECT (0x048) - Fault Injection Control

**Read/Write**

```
 31                                 3   2   1   0
+----------------------------------+---+---+---+
|             Reserved             |DLY|COR|DRP|
+----------------------------------+---+---+---+
```

| Bit | Name | Description |
|-----|------|-------------|
| 0 | DROP_FRAMES | Randomly drop ~10% of frames |
| 1 | CORRUPT_DATA | Corrupt payload and set CORRUPTED flag |
| 2 | DELAY_IRQ | Suppress MSI-X interrupts |
| 31:3 | Reserved | Always 0 |

Use these bits to test error handling in your driver:

- **DROP_FRAMES**: Tests sequence number gap detection
- **CORRUPT_DATA**: Tests payload validation (if implemented)
- **DELAY_IRQ**: Tests polling fallback (if implemented)

---

## Frame Format

Each frame in the DMA buffer has the following structure:

### Frame Header (24 bytes)

```
Offset  Size   Field         Description
------  ----   -----         -----------
0x00    4      magic         0xABCD1234
0x04    4      seq           Sequence number (monotonic)
0x08    8      ts_ns         Timestamp (nanoseconds, CLOCK_MONOTONIC)
0x10    4      payload_len   Payload size in bytes
0x14    4      flags         Frame flags
0x18    N      payload       Payload data (payload_len bytes)
```

### C Structure

```c
struct phantomfpga_frame_header {
    __le32 magic;           /* 0xABCD1234 */
    __le32 seq;             /* Sequence number */
    __le64 ts_ns;           /* Timestamp in nanoseconds */
    __le32 payload_len;     /* Payload length (excludes header) */
    __le32 flags;           /* Frame flags */
} __packed;

#define PHANTOMFPGA_FRAME_HEADER_SIZE  24
#define PHANTOMFPGA_FRAME_MAGIC        0xABCD1234
```

### Frame Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | CORRUPTED | Payload was intentionally corrupted (fault injection) |
| 31:1 | Reserved | Always 0 |

### Memory Layout

```
DMA Buffer:
+---------------+---------------+---------------+-----+---------------+
|   Frame 0     |   Frame 1     |   Frame 2     | ... |   Frame N-1   |
+---------------+---------------+---------------+-----+---------------+
|<- frame_size->|

Each Frame:
+------------------+------------------------+
| Header (24 bytes)| Payload (variable)     |
+------------------+------------------------+
```

### Payload Generation

The device generates pseudo-random payload using Xorshift32 PRNG seeded
with the sequence number XORed with 0xDEADBEEF. This provides:

1. Reproducible data (same sequence = same payload)
2. Good distribution for testing
3. Easy verification in tests

---

## IOCTL Interface

The driver exposes these ioctls to userspace. Magic number: 'P' (0x50).

### PHANTOMFPGA_IOCTL_SET_CFG (0x01)

**Direction:** Write (user to kernel)

Configure device parameters. Must be called before START.

```c
struct phantomfpga_config {
    __u32 frame_size;       /* Frame size in bytes */
    __u32 frame_rate;       /* Frame rate in Hz */
    __u32 ring_size;        /* Ring buffer entries */
    __u32 watermark;        /* IRQ threshold */
    __u32 reserved[4];      /* Must be zero */
};

#define PHANTOMFPGA_IOCTL_SET_CFG  _IOW('P', 1, struct phantomfpga_config)
```

**Returns:**
- 0 on success
- -EINVAL for invalid parameters
- -EBUSY if device is streaming

---

### PHANTOMFPGA_IOCTL_GET_CFG (0x02)

**Direction:** Read (kernel to user)

Get current configuration.

```c
#define PHANTOMFPGA_IOCTL_GET_CFG  _IOR('P', 2, struct phantomfpga_config)
```

**Returns:** 0 on success

---

### PHANTOMFPGA_IOCTL_START (0x03)

**Direction:** None

Start frame production.

```c
#define PHANTOMFPGA_IOCTL_START  _IO('P', 3)
```

**Returns:**
- 0 on success
- -EINVAL if not configured
- -EBUSY if already streaming

---

### PHANTOMFPGA_IOCTL_STOP (0x04)

**Direction:** None

Stop frame production.

```c
#define PHANTOMFPGA_IOCTL_STOP  _IO('P', 4)
```

**Returns:** 0 on success (always succeeds)

---

### PHANTOMFPGA_IOCTL_GET_STATS (0x05)

**Direction:** Read (kernel to user)

Get device and driver statistics.

```c
struct phantomfpga_stats {
    __u64 frames_produced;  /* Device: total frames produced */
    __u64 frames_consumed;  /* Driver: total frames consumed by app */
    __u32 errors;           /* Device: error count */
    __u32 overruns;         /* Device: overrun count */
    __u32 irq_count;        /* Driver: total interrupts received */
    __u32 prod_idx;         /* Current producer index */
    __u32 cons_idx;         /* Current consumer index */
    __u32 status;           /* Device status register value */
    __u32 reserved[4];      /* Reserved */
};

#define PHANTOMFPGA_IOCTL_GET_STATS  _IOR('P', 5, struct phantomfpga_stats)
```

**Returns:** 0 on success

---

### PHANTOMFPGA_IOCTL_RESET_STATS (0x06)

**Direction:** None

Reset driver-side statistics (frames_consumed, irq_count).
Device counters are reset via soft reset (CTRL.RESET).

```c
#define PHANTOMFPGA_IOCTL_RESET_STATS  _IO('P', 6)
```

**Returns:** 0 on success

---

### PHANTOMFPGA_IOCTL_GET_BUFFER_INFO (0x07)

**Direction:** Read (kernel to user)

Get buffer information for mmap.

```c
struct phantomfpga_buffer_info {
    __u64 buffer_size;      /* Total DMA buffer size */
    __u32 frame_size;       /* Size of each frame slot */
    __u32 ring_size;        /* Number of ring entries */
    __u64 mmap_offset;      /* Offset for mmap (always 0) */
    __u32 reserved[4];      /* Reserved */
};

#define PHANTOMFPGA_IOCTL_GET_BUFFER_INFO  _IOR('P', 7, struct phantomfpga_buffer_info)
```

**Returns:**
- 0 on success
- -EINVAL if not configured

---

### PHANTOMFPGA_IOCTL_CONSUME_FRAME (0x08)

**Direction:** None

Advance consumer index by one frame (for mmap access pattern).

```c
#define PHANTOMFPGA_IOCTL_CONSUME_FRAME  _IO('P', 8)
```

**Returns:**
- 0 on success
- -EAGAIN if no frames available

---

## Error Codes

Standard errno values used by the driver:

| Error | Value | Meaning |
|-------|-------|---------|
| EINVAL | 22 | Invalid parameter |
| EBUSY | 16 | Device busy (streaming or config during stream) |
| EAGAIN | 11 | No data available (poll or non-blocking read) |
| ENOMEM | 12 | Memory allocation failed |
| EIO | 5 | Hardware communication error |
| ENODEV | 19 | Device not found |
| ENOTTY | 25 | Invalid ioctl command |
| EPERM | 1 | Operation not permitted (e.g., write on read-only device) |

---

## Programming Sequence

### Initialization

```
1. Driver probes device (PCI subsystem)
2. Enable PCI device: pci_enable_device()
3. Request BAR0: pci_request_region()
4. Map BAR0: pci_iomap()
5. Verify device ID: read DEV_ID, check == 0xF00DFACE
6. Enable bus mastering: pci_set_master()
7. Set DMA mask: dma_set_mask_and_coherent()
8. Setup MSI-X: pci_alloc_irq_vectors(), request_irq()
9. Allocate DMA buffer: dma_alloc_coherent()
10. Perform soft reset: write CTRL_RESET
11. Create char device: cdev_add(), device_create()
```

### Configuration (from userspace)

```
1. Open device: open("/dev/phantomfpga0", O_RDWR)
2. Set config: ioctl(fd, SET_CFG, &config)
   - Driver validates parameters
   - Driver resizes DMA buffer if needed
   - Driver writes FRAME_SIZE, FRAME_RATE, WATERMARK, RING_SIZE
   - Driver writes DMA_ADDR_LO, DMA_ADDR_HI, DMA_SIZE
3. Get buffer info: ioctl(fd, GET_BUFFER_INFO, &info)
4. Map buffer: mmap(NULL, info.buffer_size, PROT_READ, MAP_SHARED, fd, 0)
```

### Streaming

```
1. Start streaming: ioctl(fd, START)
   - Driver clears indices (CONS_IDX = 0)
   - Driver clears IRQ_STATUS
   - Driver sets IRQ_MASK = WATERMARK | OVERRUN
   - Driver sets CTRL = START | IRQ_EN

2. Wait for data: poll(fd, POLLIN) or blocking read()

3. Process frames (repeat):
   a. Get stats: ioctl(fd, GET_STATS, &stats)
   b. Calculate pending: (prod_idx - cons_idx) & (ring_size - 1)
   c. For each pending frame:
      - Access frame at: buffer + (cons_idx * frame_size)
      - Validate: check magic, sequence, etc.
      - Process payload
      - Consume: ioctl(fd, CONSUME_FRAME)

4. Stop streaming: ioctl(fd, STOP)
   - Driver clears CTRL.START
```

### Cleanup

```
1. Close device: close(fd)
   - Optionally: driver auto-stops streaming

Driver remove:
1. Stop streaming
2. Destroy char device
3. Free DMA buffer
4. Free IRQs and MSI-X vectors
5. Unmap BAR0
6. Release region
7. Disable device
```

---

## Quick Reference Card

### Registers (Offset from BAR0)

```
0x000 DEV_ID      R     Device ID (0xF00DFACE)
0x004 DEV_VER     R     Version (0x00010000 = v1.0.0)
0x008 CTRL        R/W   [2]=IRQ_EN [1]=RESET [0]=START
0x00C STATUS      R     [2]=ERROR [1]=OVERRUN [0]=RUNNING
0x010 FRAME_SIZE  R/W   Frame size in bytes
0x014 FRAME_RATE  R/W   Frame rate in Hz
0x018 WATERMARK   R/W   IRQ threshold
0x01C RING_SIZE   R/W   Ring entries (power of 2)
0x020 DMA_ADDR_LO R/W   DMA address [31:0]
0x024 DMA_ADDR_HI R/W   DMA address [63:32]
0x028 DMA_SIZE    R/W   Total DMA buffer size
0x02C PROD_IDX    R     Producer index
0x030 CONS_IDX    R/W   Consumer index
0x034 IRQ_STATUS  R/W1C [1]=OVERRUN [0]=WATERMARK
0x038 IRQ_MASK    R/W   [1]=OVERRUN [0]=WATERMARK
0x03C STAT_FRAMES R     Frames produced
0x040 STAT_ERRORS R     Error count
0x044 STAT_OVERRUNS R   Overrun count
0x048 FAULT_INJECT R/W  [2]=DELAY_IRQ [1]=CORRUPT [0]=DROP
```

### Frame Header (24 bytes at each frame slot)

```
+00: magic       u32   0xABCD1234
+04: seq         u32   Sequence number
+08: ts_ns       u64   Timestamp (ns)
+10: payload_len u32   Payload size
+14: flags       u32   [0]=CORRUPTED
+18: payload...        Variable length
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
```
