# PhantomFPGA

> A fake FPGA that's more real than your excuses for not learning kernel development.

> [!TIP]
> Reading this as raw text? You're missing out on the pretty formatting. Open this file in a markdown viewer - GitHub renders it nicely, or use VSCode's preview (Ctrl+Shift+V on Linux/Windows, Cmd+Shift+V on Mac). Your eyes will thank you.

**PhantomFPGA** is a training platform for learning Linux kernel driver development. It gives you a virtual PCIe device (running in QEMU) that behaves like a real streaming FPGA - complete with scatter-gather DMA, interrupts, descriptor rings, and all the fun stuff that makes embedded developers lose sleep.

No expensive hardware needed. No wiring and HW setup time. Just pure learning.

And when you're done? Well. Let's just say the device is trying to tell you something. At 25 frames per second. What is it? You'll find out when your driver works.

```
+------------- Host Machine -------------+
|                                        |
|   +------------------------------+     |
|   |       phantomfpga_view       |     |
|   |       (Terminal Viewer)      | <-- The big reveal
|   +------------------------------+     |
|                  ^                     |
+------------------|----- TCP :5000 -----+
                   |
+------------------|----- QEMU VM -------+
|                  |                     |
|   +------------------------------+     |
|   |       phantomfpga_app        | <-- Streams over TCP
|   +------------------------------+     |
|                  |                     |
|   +------------------------------+     |
|   |    Your Kernel Driver        | <-- This is where the magic happens
|   |    (phantomfpga_drv.ko)      |     |
|   +------------------------------+     |
|                  |                     |
|   +------------------------------+     |
|   |        Linux Kernel          | <-- The scary part (we'll help)
|   +------------------------------+     |
|                  |                     |
|   +------------------------------+     |
|   |     PhantomFPGA Device       | <-- Hiding something...
|   |     (Emulated PCIe FPGA)     |     |
|   +------------------------------+     |
|                                        |
+----------------------------------------+
```

## What is this thing?

You know how learning to drive is easier in a simulator before you crash a real car? Same idea here, but for kernel drivers.

PhantomFPGA gives you:
1. A virtual PCIe device that streams... something... via scatter-gather DMA
2. A kernel driver skeleton with detailed TODOs showing you exactly what to implement
3. A userspace server app to stream frames over TCP
4. A viewer skeleton that displays... whatever it is
5. A safe environment where crashing the kernel just means rebooting a VM

The device has 250 frames of data. Each frame is exactly 5120 bytes. They loop forever at 25 fps. What's in them? That's between you and your working driver.

**Who is this for?**
- Junior embedded developers who want to learn kernel programming
- Anyone curious about how drivers actually work
- People who learn best by doing (and breaking things safely)
- Engineers whose boss said "we need a driver for this thing by Friday"

**Who is this NOT for?**
- Chuck Norris (he doesn't need drivers, hardware just obeys him directly)

## Features

- Full PCIe device model in QEMU with vendor ID 0x0DAD and device ID 0xF00D (because every project needs dad jokes in the PCI IDs)
- Real descriptor-based SG-DMA transfers, just like actual hardware
- Three vectors of MSI-X interrupts - one for "hey, data's ready", one for "oops", and one for "you're not keeping up"
- On-demand corrupt CRCs, skip sequences, generally make life difficult (great for testing your error handling)
- Works on x86_64 and aarch64 (ARM64)
- Driver and apps with extensive TODO comments guiding you step by step

## Quick start

Let's get you up and running. This will take about 30-60 minutes depending on your internet speed and how many times you need to re-read the instructions (no judgment, we've all been there).

### Prerequisites

You need **Linux**. This project won't run directly on macOS or Windows.

| Your computer | What to do |
|---------------|------------|
| Linux PC/laptop | You're good to go |
| Mac (M1/M2/M3) | Install [Lima](https://lima-vm.io/) or [UTM](https://mac.getutm.app/) with ARM64 Linux (e.g., Debian arm64) |
| Mac (Intel) | Install [Lima](https://lima-vm.io/) or [UTM](https://mac.getutm.app/) with x86_64 Linux |
| Windows | Use [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) with Ubuntu |

> [!IMPORTANT]
> **VM users (not native Linux laptop/PC):** Give your VM at least **8GB RAM** and **4 CPUs** -- the build compiles a lot of code in parallel. For Lima: `limactl edit <instance>` and add `cpus: 4` and `memory: "8GiB"`.

**Disk space:** About 15-20 GB. Yeah, I know. QEMU and Buildroot are hungry beasts.

Once you have Linux running, install these packages:

```bash
# Ubuntu/Debian - copy-paste this whole block, others - I guess you know what you're doing anyway.
sudo apt-get update
sudo apt-get install -y \
    git build-essential cmake ninja-build meson pkg-config \
    python3 python3-pip \
    libglib2.0-dev libpixman-1-dev libslirp-dev \
    libelf-dev libssl-dev flex bison \
    rsync bc cpio unzip wget
```

### Step 1: Clone this repository (unless you already did)

```bash
git clone https://github.com/walruscraft/PhantomFPGA.git
cd PhantomFPGA
```

### Step 2: Build QEMU with PhantomFPGA device

This builds a custom QEMU that includes our virtual device.

```bash
cd platform/qemu
./setup.sh
make build
cd ../..
```

This will:
- Download QEMU 10.2.0 source code
- Copy our device files into the QEMU tree
- Build QEMU for both x86_64 and aarch64 targets

**What you should see at the end:**
```
Build complete! Binary at: /path/to/PhantomFPGA/platform/qemu/build/qemu-system-x86_64
To verify PhantomFPGA device:
  ./qemu-system-x86_64 -device help 2>&1 | grep -i phantom
```

Let's verify the device is there:
```bash
./platform/qemu/build/qemu-system-x86_64 -device help 2>&1 | grep -i phantom
```

**Expected output:**
```
name "phantomfpga", bus PCI, desc "PhantomFPGA SG-DMA Training Device v3.0"
```

If you see this - congratulations! You built a fake FPGA. Your parents would be... confused but probably supportive.

### Step 3: Build the guest Linux image

This builds a minimal Linux system that runs inside QEMU.

```bash
cd platform/buildroot
make
cd ../..
```

> [!NOTE]
> This takes 20-40 minutes, YMMV. The Makefile automatically picks the right architecture for your computer.

**What you should see at the end** (don't move to Step 4 until you see this!):
```
==> x86_64 build complete!
    Kernel: platform/images/bzImage
    Rootfs: platform/images/rootfs.ext4

Tip: Run './platform/run_qemu.sh' to boot the VM.
```

If the command finishes but you don't see this message, something went wrong. Check the troubleshooting section.

### Step 4: Boot the VM

The moment of truth:

```bash
./platform/run_qemu.sh
```

**What you should see:**
```
[*] PhantomFPGA VM Launcher
[*] =======================
[*] Target: x86_64
[*] Configuration:
[*]   Arch:    x86_64 (q35)
[*]   Memory:  512M, CPUs: 2
[*]   SSH:     ssh -p 2222 root@localhost (password: root)
[*]
[*] Starting QEMU...
[*]   To exit the VM: press Ctrl-A then X

Welcome to PhantomFPGA Training Environment!
phantomfpga login:
```

Login as `root` with password `root`.

> [!TIP]
> **Working with the VM like a pro:**
> - Open the VM in a **separate terminal tab or window**. You'll be switching between your host (for editing code, building) and the VM (for testing) constantly. Having them side by side is a game changer.
> - If you're not a terminal ninja yet, try `mc` (Midnight Commander) - it's a file manager that runs in the terminal. Already installed in the VM. Type `mc` to launch it, `exit` to quit. Pro tip: `Ctrl+O` toggles between the `mc` panels and a regular terminal - super handy for running commands without leaving `mc`.

**Passwordless login:**

In case you `ssh` a lot into the VM, you may want to stop entring the password every time. You can do that following these steps on the host (make sure the QEMU VM is running):
```bash
ssh-keygen -t rsa -f ~/.ssh/id_rsa_dropbear   # Create an SSH key for this target
cat ~/.ssh/id_rsa_dropbear.pub | ssh root@localhost -p 2222 'mkdir -p ~/.ssh && cat >> ~/.ssh/authorized_keys'
ssh root@localhost -p 2222 'chmod 700 ~/.ssh && chmod 600 ~/.ssh/authorized_keys'

# From now on, you'll ssh into the VM passwordless with:
ssh -i ~/.ssh/id_rsa_dropbear root@localhost -p 2222
```

### Step 5: Verify the device is there

Inside the VM, run:

```bash
lspci -nn | grep -i 0dad
```

**Expected output (slot may vary):**
```
00:01.0 Unclassified device [00ff]: Device [0dad:f00d] (rev 03)
```

There it is! Device 0dad:f00d. That's our "DAD" serving "FOOD" in hex. I'm not sorry.

> [!NOTE]
> The PCI slot (00:01.0 in this example) can vary depending on lots of stuff. Use whatever slot `lspci` shows you.

Let's poke the device registers to make sure it's really ours. Since no driver is loaded yet, the device is disabled by default - we need to enable it first:

```bash
# Enable the device (use YOUR slot from lspci above)
echo 1 > /sys/bus/pci/devices/0000:00:01.0/enable

# Check the BAR0 address - it shouldn't say [disabled]
lspci -v -s 00:01.0 | grep "Memory at"
# Example output: Memory at 10040000 (32-bit, non-prefetchable) [size=4K]

# Read the device ID register (at offset 0x000)
# Use the address from the output above, with 0x prefix!
devmem 0x10040000 w
```

**Expected output:**
```
0xF00DFACE
```

`0xF00DFACE` - that's the device's way of saying hello. If you see this, your fake FPGA is alive and... holding its secrets until you write a proper driver for it.

## Now what? The fun part!

You've got the environment running. Now comes the actual learning:

1. **Complete the driver** in `driver/phantomfpga_drv.c`
   - Look for `/* TODO: ... */` comments - they tell you exactly what to implement
   - Follow the detailed guide in `docs/driver-guide.md`

2. **Complete the server app** in `app/phantomfpga_app_impl.cpp`
   - Same deal - find the TODOs, implement them
   - This one streams frames over TCP to whoever wants them

3. **Complete the viewer** in `viewer/phantomfpga_view_impl.cpp`
   - Connects to the server, displays... whatever it is
   - The final piece of the puzzle

4. **Test your work**
   - Load driver, run server, connect viewer
   - Use `--record stream.bin` to save frames, then validate:
     `python3 tools/validate_stream.py stream.bin -v`
   - If you did everything right, you'll know

## C++ crash course (just enough to survive)

The app and viewer are written in C++17. If you're coming from C (or if your C++
knowledge stopped at `cout << "hello"` in college), here's what you actually need
to know. This isn't a C++ textbook -- just the concepts you'll use in the TODO
methods.

### Classes and inheritance

The codebase uses a base class + derived class pattern. The base class (provided)
defines pure virtual methods. You implement them in the derived class (your file).

```cpp
class PhantomFpgaApp {                 // Base class -- don't touch
    virtual int open_device() = 0;     // "= 0" means you MUST implement this
};

class PhantomFpgaAppImpl : public PhantomFpgaApp {  // Your class
    int open_device() override {       // "override" = compiler checks you got the
        /* your code */                //   signature right. Use it. Always.
    }
};
```

You access base class members with the usual dot syntax: `config_.frame_rate`,
`stats_.frames_received`, `dev_fd_.get()`. They're `protected`, which means
your derived class can use them but nobody else can.

### RAII (resource acquisition is initialization)

The single most important C++ concept you'll use. RAII means: when you create an
object, it acquires a resource. When the object goes out of scope, it releases it.
No `goto cleanup`. No `free()` you might forget.

```cpp
// C way -- hope you remember to close() on every error path:
int fd = open("/dev/phantomfpga0", O_RDWR);
// ... 50 lines of code with 4 error paths ...
close(fd);  // did you close() in all the error paths? really?

// C++ way -- FileDescriptor closes automatically when it's destroyed:
dev_fd_ = FileDescriptor(fd);
// When dev_fd_ goes out of scope (or gets reassigned), close() happens.
// Even if an error occurs. Even if you forget. The destructor has your back.
```

The codebase has two RAII wrappers:
- **`FileDescriptor`** -- wraps a file descriptor, calls `close()` on destruction
- **`MappedMemory`** -- wraps an `mmap()` region, calls `munmap()` on destruction

### Move semantics

RAII wrappers are *move-only*. You can't copy a file descriptor (that would mean
two owners trying to close the same fd). You transfer ownership instead:

```cpp
int fd = ::open(DEVICE_PATH, O_RDWR);
dev_fd_ = FileDescriptor(fd);   // fd is now owned by dev_fd_
// Don't use the raw fd anymore -- dev_fd_ owns it now.

void* addr = mmap(...);
buffer_pool_ = MappedMemory(addr, size);  // same idea
```

The `=` here triggers a *move assignment* -- ownership transfers from the
temporary object on the right to the member on the left. The old value (if any)
gets cleaned up automatically.

### Smart pointers (std::unique_ptr)

`std::unique_ptr` is RAII for heap-allocated objects. It owns the pointer and
deletes it when it goes out of scope. You'll see it for the TCP server:

```cpp
std::unique_ptr<TcpServer> tcp_server_;  // might be nullptr

// Use it like a regular pointer, but check for null first:
if (tcp_server_)                          // null check (same as != nullptr)
    tcp_server_->try_accept();            // arrow operator, just like C pointers
```

You don't need to create or delete these yourself -- just use the ones the
base class gives you.

### std::array

`std::array<uint8_t, 5120>` is a fixed-size array that knows its own size and
doesn't decay to a pointer when you sneeze at it.

```cpp
std::array<uint8_t, frame::SIZE> frame_buffer_;

frame_buffer_.data()    // raw uint8_t* pointer (like &arr[0] in C)
frame_buffer_.size()    // 5120 (always, unlike C arrays in function params)
frame_buffer_[42]       // element access, same as C
```

### Namespaces and constexpr

Constants are organized in namespaces instead of `#define` macros:

```cpp
namespace frame {
    constexpr uint32_t MAGIC = 0xF00DFACE;  // compile-time constant
    constexpr size_t   SIZE  = 5120;
}

// Use them with the :: scope operator:
if (hdr->magic == frame::MAGIC) { ... }
```

`constexpr` means "evaluate at compile time" -- like `#define` but type-safe
and debugger-friendly.

### Static methods

`CRC32::compute()` is a static method -- you call it on the class, not on an
instance. Think of it as a namespaced function:

```cpp
uint32_t crc = CRC32::compute(data, len);  // no CRC32 object needed
```

### Calling C functions from C++

Sometimes you need to call plain C functions like `open()`, `ioctl()`, or
`mmap()`. Use the `::` prefix to call the global (C) version explicitly:

```cpp
int fd = ::open(DEVICE_PATH, O_RDWR);  // :: = global scope
```

The `::` isn't always required, but it makes it clear you're calling the C
library function, not some method on the current class.

### Pointer casting

To interpret raw bytes as a struct (like reading a frame header):

```cpp
auto* hdr = reinterpret_cast<const FrameHeader*>(frame_buffer_.data());
// Or the C way -- still works, still fine for this:
auto* hdr = (const FrameHeader*)frame_buffer_.data();
```

Both work. The C++ cast is more explicit about what it's doing. Use whichever
doesn't make your eyes bleed.

### Zero-initialization

C++ structs can be zero-initialized with `= {}`:

```cpp
struct phantomfpga_config cfg = {};  // all fields zeroed
cfg.desc_count = config_.desc_count;
cfg.frame_rate = config_.frame_rate;
```

### That's it

If you know the above, you can complete every TODO in the codebase. You
just fill in the methods with straightforward logic -- mostly the same
POSIX calls you'd write in C, wrapped in slightly nicer containers.

That said -- don't treat the base classes as black boxes. Read the headers
(`phantomfpga_app.h`, `phantomfpga_view.h`) and the `.cpp` files that go
with them. They're full of the patterns listed above, and seeing how RAII
wrappers, move semantics, and class design work in real code is worth more
than any tutorial. The best way to learn C++ is to read C++ that actually
does something useful.

## Documentation

This is a good time to read the docs before you begin breaking stuff. Start here and work your way through:

1. **[Architecture Overview](docs/architecture.md)** - Understand how all the pieces fit together
2. **[Device Datasheet](docs/phantomfpga-datasheet.md)** - How the device works and the complete register reference
3. **[Driver Implementation Guide](docs/driver-guide.md)** - Step-by-step instructions for completing the driver
4. **[Glossary](docs/glossary.md)** - Quick reference for PCIe, DMA, MSI-X, and other jargon

### Building the driver

Build on your host machine, test inside the VM. The `driver/` and `app/` directories on your host are automatically shared with the VM, so anything you build shows up inside the VM instantly. You can treat the `/mnt/driver` and `/mnt/app` directories **in the VM** as a window into your host - everything you in those on any side will be reflected on the other in real time.

```bash
# On your host:
cd driver
make
```

```bash
# Inside the VM:
cd /mnt/driver
insmod phantomfpga.ko
dmesg | tail -20
```

You should see messages about the driver loading and finding the device.

### Building the apps

```bash
# On your host:
cd app
make

cd ../viewer
make
```

```bash
# Inside the VM - run the server:
/mnt/app/phantomfpga_app --tcp-server

# On your host - connect the viewer:
./viewer/phantomfpga_view localhost 5000

# Or record the stream for validation:
./viewer/phantomfpga_view localhost 5000 --record stream.bin
python3 tools/validate_stream.py stream.bin -v
```

And then... well. You'll see what you'll see. Or you won't, if something's broken. The device knows what it wants to show you. Your job is to let it.

> [!NOTE]
> Make sure your terminal is at least 110 columns wide and 45 rows tall. Just trust me on this one.

## Building for a different architecture

The Makefile auto-detects your architecture, but you can explicitly build for a specific one:

```bash
cd platform/buildroot
make x86_64     # Force x86_64 build
make aarch64    # Force aarch64 build
make all-arches # Build both
cd ../..

# Run a specific architecture
./platform/run_qemu.sh --arch aarch64
```

Same device, same driver code, different architecture. That's the beauty of proper abstractions.

## VM options

```bash
# See kernel boot messages (hidden by default)
./platform/run_qemu.sh --verbose-boot

# Debug mode (starts GDB server on port 1234)
./platform/run_qemu.sh --debug

# More resources
./platform/run_qemu.sh --memory 4G --cpus 4

# SSH access (from your host machine, password: root)
ssh -p 2222 root@localhost
```

> **To exit the VM**: press `Ctrl-A` then `X`. This is QEMU's escape sequence -- the `Ctrl-A` is like a "hey QEMU, this next key is for you, not the guest".

## Troubleshooting

### "KVM not available"

QEMU will work without KVM, just slower. If you want KVM:
```bash
# Check if KVM module is loaded
lsmod | grep kvm

# Load it if not (Intel CPU)
sudo modprobe kvm_intel
# Or for AMD:
sudo modprobe kvm_amd
```

### "QEMU binary not found"

Did you build QEMU?
```bash
cd platform/qemu && ./setup.sh && make build
```

### "Kernel image not found"

Did you build Buildroot? Make sure you wait for it to complete - it takes 20-40 minutes or even more, based on your HW.
```bash
cd platform/buildroot && make
```

**Important**: The build isn't done until you see this message:
```
==> x86_64 build complete!
    Kernel: platform/images/bzImage
    Rootfs: platform/images/rootfs.ext4
```

If you don't see this, the build either failed or is still running.

### "Buildroot download/extraction failed"

If you see errors like "gzip: unexpected end of file" or "tar: Error is not recoverable", you might have a corrupted download. This can happen if a previous download was interrupted.

Check for zero-byte tarballs:
```bash
ls -la platform/buildroot/dl/
# Look for any files with size 0
```

If you find empty or corrupted tarballs, delete them and the stamps, then retry:
```bash
cd platform/buildroot
rm -rf .stamps dl/*.tar.gz
make
```

Also verify you have network connectivity - buildroot needs to download packages:
```bash
wget -q --spider https://buildroot.org && echo "OK" || echo "Network issue"
```

### "Device not showing up in lspci"

Make sure you're using OUR QEMU, not the system QEMU. The difference is subtle but crucial:
```bash
# CORRECT - uses our custom QEMU that includes the PhantomFPGA device:
./platform/run_qemu.sh
./platform/qemu/build/qemu-system-x86_64 ...

# WRONG - uses system QEMU from /usr/bin which doesn't have our device:
qemu-system-x86_64 ...
```

The path matters! When you run `qemu-system-x86_64` without a path, your shell finds it in `/usr/bin/` (the system-installed version). Our custom QEMU with the PhantomFPGA device is in `platform/qemu/build/`.

### "My driver crashed the kernel"

Welcome to kernel development! Check `dmesg` for clues. The VM will reboot. No real hardware was harmed. This is literally why we built this training platform.

### "I really messed things up"

Just reboot the VM. Or rebuild everything:
```bash
cd platform/buildroot && make clean && make
```

### "The viewer shows garbage / nothing"

A few things to check:
- Is your terminal big enough? You need at least 110x45.
- Did you implement the TODOs in the viewer? The skeleton doesn't display anything by itself.
- Is the CRC validation working? Bad CRCs mean corrupt frames.
- Are you keeping up? Check the `frames_dropped` counter.

When it works, you'll know. It's not subtle.

## License

MIT License - see [LICENSE](LICENSE) for the full text.

The QEMU device code is also compatible with GPL-2.0-or-later (to play nice with QEMU).
The kernel driver skeleton is GPL-2.0 (because that's what Linux requires).

Use it, learn from it, break it, fix it, share it. Just don't blame me when you accidentally `rm -rf /` inside the VM (ask me how I know this is a concern).

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines.

Found a bug? Open an issue.
Fixed a bug? Open a PR.
Have a better dad joke for the code comments? I'm genuinely interested.

## Acknowledgments

- The QEMU project for making device emulation accessible
- Buildroot for the guest Linux environment
- The Linux kernel community for comprehensive documentation
- Coffee, for everything else
- You, for wanting to learn this stuff

---

*"The best way to learn kernel programming is to write a driver for hardware that doesn't exist yet."*
*- Ancient Embedded Proverb (that I just made up)*

*"Chuck Norris can write kernel drivers in JavaScript. The kernel just accepts it."*
*- Also me*

---

**P.S.** - The device magic number is `0xF00DFACE`. We're not very creative with magic numbers around here, but at least they're memorable. And delicious-sounding.
