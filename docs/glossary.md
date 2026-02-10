# Glossary

A quick reference for the jargon you'll encounter while working on PhantomFPGA.
Don't worry if some of these seem intimidating - by the time you finish
this project, you'll be casually dropping terms like "scatter-gather DMA"
in conversation and watching people's eyes glaze over.

---

## Hardware & Bus Concepts

### PCIe (PCI Express)
The high-speed bus that connects peripherals to your CPU. Think of it as a
highway for data between your processor and devices like GPUs, network cards,
and (in our case) FPGAs. PCIe replaced the older PCI bus because apparently
"express" makes everything better.

### BAR (Base Address Register)
A region of memory that a PCIe device exposes to the CPU. When you hear "BAR0",
that's the first (and often only) memory region a device uses. The CPU reads
and writes to this memory to talk to the device. PhantomFPGA uses BAR0 for its
control registers.

### MMIO (Memory-Mapped I/O)
A technique where device registers appear as regular memory addresses. Instead
of special I/O instructions, you just read/write to memory locations. The CPU
thinks it's accessing RAM, but it's actually poking the device. Sneaky, but
efficient.

### DMA (Direct Memory Access)
Lets devices read/write system memory without bothering the CPU for every byte.
The CPU sets up a transfer ("here's where to put the data"), then the device
does the heavy lifting. Essential for high-bandwidth devices - imagine if the
CPU had to copy every network packet byte by byte. DMA is the intern that does
the boring work so the CPU can focus on important things. Chuck Norris doesn't
need DMA - data copies itself out of respect.

### Scatter-Gather DMA
Regular DMA needs contiguous memory. Scatter-gather DMA can handle fragmented
buffers - it follows a list of memory chunks (descriptors) and assembles them
into one logical transfer. More flexible, more complex, more fun to debug.

### Descriptor Ring
A circular buffer of DMA descriptors. The device walks through the ring,
processing each descriptor (which points to a memory buffer), and wraps around
when it reaches the end. Think of it as a conveyor belt of work items, or, if
a programming analogy is better - this is exactly a linked list in a closed
loop.

### MSI-X (Message Signaled Interrupts - Extended)
A modern interrupt mechanism where devices write to a special memory address
to signal the CPU, instead of toggling a physical interrupt line. Supports
multiple interrupt vectors (one per event type), which is why PhantomFPGA can
have separate interrupts for "frame complete", "error", and "no descriptors".

### Interrupt Coalescing
Instead of firing an interrupt for every single event, the device batches them.
"Tell me after 8 frames complete OR after 40ms, whichever comes first." Reduces
CPU overhead (interrupts cause function callbacks) at the cost of slightly
higher latency - imagine you receive 8 buffers at a time instead of one.

### Watermark
The threshold that triggers an interrupt when doing interrupt coalescing. In
PhantomFPGA, it's the number of frames that need to accumulate in the ring
buffer before the device fires an interrupt. Set it to 1 for immediate
notification, higher for batching. The name comes from the paper industry -
a watermark shows when you've reached a certain level. A fantastic trivia
detail.

### FPGA (Field-Programmable Gate Array)
Reconfigurable hardware that can be programmed to act like custom circuits.
Real FPGAs are expensive and require HDL knowledge. PhantomFPGA is none of
those things - it's software pretending to be hardware pretending to be
software. Very meta.

---

## Linux Kernel Concepts

### Kernel Module
A piece of code that can be loaded into the running kernel without rebooting.
Drivers are typically modules. You load them with `insmod`, remove them with
`rmmod`, and pray they don't crash your system with `dmesg`.

### Character Device (Char Device)
A type of device that handles data as a stream of bytes. Accessed through
filesystem like `/dev/phantomfpga0`. You can `open()`, `read()`, `write()`,
and `ioctl()` them just like regular files. The kernel routes these calls to
your driver.

### ioctl (I/O Control)
A catch-all system call for device-specific operations that don't fit into
read/write. Need to configure the device? `ioctl()`. Want statistics?
`ioctl()`. It's the "miscellaneous" drawer of the kernel API.

### mmap (Memory Map)
Maps a file or device memory directly into your process's address space.
Instead of copying data with `read()`, you access it directly. Zero-copy
performance, but you need to be careful about synchronization. PhantomFPGA
uses mmap to let userspace access DMA buffers directly.

### Coherent DMA Memory
Memory where CPU and device always see the same data without manual
synchronization. The hardware handles cache coherency automatically.
Allocated with `dma_alloc_coherent()`. The alternative is "streaming DMA"
which can be faster but requires explicit cache flushes before transfers.
PhantomFPGA uses coherent memory because the ring buffer is accessed
frequently by both sides - the simplicity is worth it.

### Critical Section
A piece of code that accesses shared resources (memory, hardware registers, etc.)
and must not be executed by more than one thread at a time. If two threads enter
the same critical section simultaneously, you get a race condition - and race
conditions are like gremlins: hard to reproduce, impossible to explain to your
manager, and they always show up in production. Spinlocks and mutexes are used
to protect critical sections.

### Spinlock
A lock that busy-waits (spins) until it's available. Used in contexts where
you can't sleep (like interrupt handlers). If you hold a spinlock too long,
you'll make the whole system unresponsive. Keep it short.

### Mutex
A lock that puts the waiting thread to sleep instead of spinning. More efficient
than spinlocks when the critical section might take a while, but you can only
use them in contexts where sleeping is allowed (not in interrupt handlers).
The name comes from "mutual exclusion" - only one thread can hold it at a time.

### Wait Queue
A mechanism for processes to sleep until a condition is met. The driver puts
the process to sleep, and later wakes it up when data is available. More
efficient than polling (checking for a condition) in a loop.

### Memory Barrier
A CPU instruction that enforces ordering of memory operations. Modern CPUs and
compilers can reorder reads and writes for performance, which is usually fine --
until you're talking to a DMA device that reads your memory too. If the CPU
reorders a descriptor write *after* the head pointer update, the device sees
the new head but reads stale descriptor data. Bad things happen.

In PhantomFPGA, `wmb()` (write memory barrier) appears in descriptor
submission: we write descriptor fields, then `wmb()`, then update the head
register. The barrier guarantees the device sees fully-populated descriptors
before it starts processing them. Think of it as "flush all my writes to
memory before continuing" -- because the DMA engine doesn't have the luxury
of CPU cache coherency for ordering.

There are three main flavors in the kernel:
- `wmb()` -- orders writes (our case: descriptors before head update)
- `rmb()` -- orders reads (useful when reading device status after data)
- `mb()` -- orders both (the sledgehammer approach)

Note: `ioread32`/`iowrite32` (which `pfpga_write32` wraps) already include
implicit barriers, so you don't need `wmb()` between register writes. You DO
need it between regular memory writes (like descriptor fields) and register
writes that signal the device.

### IRQ (Interrupt Request)
A signal that tells the CPU "stop what you're doing, something needs attention".
The kernel dispatches IRQs to registered handlers. In PhantomFPGA, the device
raises IRQs when frames complete or errors occur. This is an integral part of
the DMA process - the CPU is kicked only when the buffer is already full.

### Interrupt Vector
In the context of MSI-X, a "vector" is simply an independently-routable
interrupt signal with its own ID number. Each vector can be assigned to a
different CPU core and trigger a different handler function.

The word comes from the original interrupt mechanism where the CPU would look
up a handler address in an "interrupt vector table" -- an array indexed by
interrupt number. So "vector 0" means "index 0 in that table". With MSI-X,
it works the same way conceptually: the device writes a message to a specific
address, and the interrupt controller routes it by vector number.

PhantomFPGA uses 3 vectors:
- Vector 0: frame completion (the happy path)
- Vector 1: device error (something went wrong)
- Vector 2: no descriptors available (ring buffer full)

Having separate vectors means the kernel can dispatch each to a dedicated
handler without checking status registers to figure out what happened.

---

## Userspace Concepts

### System Call (Syscall)
The interface between userspace and the kernel. When you call `open()` or
`read()`, you're actually asking the kernel to do something on your behalf.
The kernel checks permissions, does the work, and returns the result.

### File Descriptor
An integer that represents an open file, socket, or device. When you `open()`
something, you get a file descriptor. You pass it to `read()`, `write()`,
`ioctl()`, etc. It's like a ticket number at a deli counter.

### poll() / select()
System calls that let you wait for events on multiple file descriptors at once.
"Wake me up when any of these have data." Essential for servers handling many
connections or devices.

---

## Networking Concepts

### TCP (Transmission Control Protocol)
A reliable, connection-oriented protocol. Data arrives in order, and lost
packets are automatically retransmitted. PhantomFPGA uses TCP to stream
frames from the guest VM to the host viewer.

### Socket
An endpoint for network communication. You create a socket, connect it to
an address, and then read/write data. Works similar to files (because Unix
philosophy says everything should be a file).

### Port
A 16-bit number that identifies a specific service on a machine. Essentially,
ports are there to allow multiple connections to a single computer - in our
case, SSH over port 22 and our custom streaming on 5000, at the same time.

---

## QEMU / Virtualization Concepts

### QEMU
An open-source machine emulator. It can emulate entire systems, including
CPUs, memory, and devices. PhantomFPGA runs as a virtual device inside QEMU,
which is how we can have a "hardware" device without actual hardware.

### Virtual Device
A software implementation of a hardware device. QEMU provides the framework,
we provide the behavior. The guest OS thinks it's talking to real hardware.

### Guest / Host
The **host** is your real machine running QEMU. The **guest** is the virtual
machine running inside QEMU. The driver and the server run in the guest, the
viewer client runs on the host, which, in this case, represents any remote
machine.

### 9p / virtfs
A network filesystem protocol originally from Plan 9 (yes, that Plan 9 - the
operating system from Bell Labs that was too ahead of its time). QEMU uses it
to share directories between host and guest without copying files. Changes on
one side appear instantly on the other. PhantomFPGA uses this so you can edit
code on your host and test it in the VM without manual syncing.

---

## Data Formats

### Little-Endian
The byte order where the least significant byte comes first. x86 and ARM
(in most configurations) are little-endian. If you store 0x12345678, the
memory contains: 78 56 34 12. The kernel provides `le32_to_cpu()` and friends
to handle conversions.

### CRC32 (Cyclic Redundancy Check)
A checksum algorithm that detects data corruption. PhantomFPGA uses IEEE
802.3 (Ethernet) CRC32. If the computed CRC doesn't match the stored CRC,
something went wrong during transmission.

### Magic Number
A constant value used to identify a data format or validate data integrity.
PhantomFPGA uses 0xF00DFACE. If you see a different magic number, you're
probably reading garbage (or someone is hungry).

---

## Abbreviations Reference

| Abbreviation | Meaning |
|--------------|---------|
| BAR | Base Address Register |
| CRC | Cyclic Redundancy Check |
| DMA | Direct Memory Access |
| FPGA | Field-Programmable Gate Array |
| IRQ | Interrupt Request |
| MMIO | Memory-Mapped I/O |
| MSI-X | Message Signaled Interrupts - Extended |
| PCIe | PCI Express |
| SG | Scatter-Gather |
| TCP | Transmission Control Protocol |
| WTF | The first thing you say when the code misbehaves |

---

## Still Confused?

That's normal. These concepts take time to internalize. The good news is that
PhantomFPGA lets you experiment with all of them in a safe environment where
the worst that can happen is a VM crash - not a fried $500 FPGA board. Chuck
Norris writes kernel drivers without reading the datasheet. They work on the
first try. You are not Chuck Norris. Read the datasheet.

Keep this glossary handy, and refer back to it as you work through the driver.
By the end, these terms will feel like old friends. Weird, technical friends
who only talk about memory addresses and interrupt handlers.

---

*"I know what all these terms mean!"*
*- Future you, hopefully*
