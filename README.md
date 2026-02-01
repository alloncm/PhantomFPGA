# PhantomFPGA

> A fake FPGA that's more real than your excuses for not learning kernel development.

**PhantomFPGA** is a training platform for learning Linux kernel driver development. It gives you a virtual PCIe device (running in QEMU) that behaves like a real streaming FPGA - complete with DMA, interrupts, ring buffers, and all the fun stuff that makes embedded developers lose sleep.

No expensive hardware needed. No magic smoke released. Just pure learning.

```
    +------------------+
    |   Your Code      |  <-- This is where the magic happens
    +------------------+
           |
    +------------------+
    |  Linux Kernel    |  <-- The scary part (we'll help you through it)
    +------------------+
           |
    +------------------+
    |  PhantomFPGA     |  <-- Our fake FPGA (real enough to learn from)
    |  (QEMU Device)   |
    +------------------+
```

## What Is This Thing?

You know how learning to drive is easier in a simulator before you crash a real car? Same idea here, but for kernel drivers.

PhantomFPGA gives you:
1. A virtual PCIe device that produces streaming data (like a real FPGA would)
2. A kernel driver skeleton with detailed TODOs showing you exactly what to implement
3. A userspace app skeleton to consume the data
4. A safe environment where crashing the kernel just means rebooting a VM

The device simulates an FPGA that captures frames at configurable rates and DMAs them to a ring buffer in system memory. Your job is to write the driver that sets this up and the app that reads the frames.

**Who is this for?**
- Junior embedded developers who want to learn kernel programming
- Anyone curious about how drivers actually work
- People who learn best by doing (and breaking things safely)
- Engineers whose boss said "we need a driver for this thing by Friday"

**Who is this NOT for?**
- Chuck Norris (he doesn't need drivers, hardware just obeys him directly)

## Features

- **Virtual PCIe Device**: Full PCIe device model in QEMU with vendor ID 0x0DAD and device ID 0xF00D (because every project needs dad jokes in the PCI IDs)
- **DMA Ring Buffer**: Real DMA transfers to guest memory, just like actual hardware
- **MSI-X Interrupts**: Two vectors - one for "hey, you have data" and one for "oops, buffer overrun"
- **Configurable Everything**: Frame size, rate, ring depth, watermark thresholds
- **Fault Injection**: Make the device misbehave on purpose (great for testing your error handling)
- **Multi-Architecture**: Works on x86_64 and aarch64 (ARM64)
- **Detailed Skeleton Code**: Driver and app with extensive TODO comments guiding you step by step

## Quick Start

Let's get you up and running. This will take about 30-60 minutes depending on your internet speed and how many times you need to re-read the instructions (no judgment, we've all been there).

### Prerequisites

You need a Linux machine (real or VM) with these packages:

```bash
# Ubuntu/Debian - copy-paste this whole block
sudo apt-get update
sudo apt-get install -y \
    git build-essential ninja-build meson pkg-config \
    python3 python3-pip \
    libglib2.0-dev libpixman-1-dev libslirp-dev \
    libelf-dev libssl-dev flex bison \
    rsync bc cpio unzip wget

# Optional: check if KVM is available (makes things faster)
ls -la /dev/kvm
# If this shows the file, great! If not, things will work but slower.
```

**Disk space needed**: About 15-20 GB. Yeah, I know. QEMU and Buildroot are hungry beasts.

### Step 1: Clone the Repository

```bash
git clone https://github.com/walruscraft/PhantomFPGA.git
cd PhantomFPGA
```

### Step 2: Build QEMU with PhantomFPGA Device

This builds a custom QEMU that includes our virtual device.

```bash
cd platform/qemu
./setup.sh
make build
cd ../..
```

This will:
- Download QEMU 8.2.2 source code
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
name "phantomfpga", bus PCI, desc "PhantomFPGA SG-DMA Training Device v2.0"
```

If you see this - congratulations! You built a fake FPGA. Your parents would be... confused but probably supportive.

### Step 3: Build the Guest Linux Image

This builds a minimal Linux system that runs inside QEMU.

```bash
cd platform/buildroot
make            # Builds x86_64 image
make aarch64    # Builds aarch64 image (optional, see note below)
cd ../..
```

**Which one do I need?** Short version: build the one that matches your computer's CPU. If you're on a regular x86 PC or laptop, `make` is all you need. If you're on an ARM64 machine (like a Mac with M1 and later chip running Linux through something like Lima or UTM, or any native ARM64 Linux box/laptop), build the aarch64 image, since it'll run *much* faster as QEMU can use hardware virtualization instead of emulating everything in software. You can also build both and try them, nothing will break.

**Fair warning**: Each image takes a while. Like, 20-40 minutes. Good time for coffee, lunch, or contemplating the meaning of life. Buildroot is downloading and compiling an entire Linux distribution. Make sure your build machine has enough CPUs and RAM available. For instance, Lima VM on a Mac configures only 4GB of RAM for the VM by default, and you'd probably need at least 8GB, if not more. Getting there is quite easy - just run something like `limactl edit debian` or whatever Linux you use, and insert stuff like `cpus: 4` and `memory: "8GiB"`.

**What you should see at the end:**
```
# x86_64
Kernel: platform/images/bzImage
Rootfs: platform/images/rootfs.ext4

# aarch64
Kernel: platform/images/Image
Rootfs: platform/images/rootfs-aarch64.ext4
```

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
[*]   Arch:      x86_64
[*]   Machine:   q35
[*]   Memory:    2G
[*]   CPUs:      2
[*]   KVM:       enabled
[*]   SSH:       localhost:2222
[*]
[*] Starting QEMU...

... (a bunch of Linux boot messages) ...

Welcome to PhantomFPGA Training Environment!
phantomfpga login:
```

Login as `root` with password `root`.

### Step 5: Verify the Device is There

Inside the VM, run:

```bash
lspci -nn | grep -i 0dad
```

**Expected output (slot may vary):**
```
00:02.0 Unclassified device [00ff]: Device [0dad:f00d] (rev 02)
```

There it is! Device 0dad:f00d. That's our "DAD" serving "FOOD" in hex. I'm not sorry.

**Note:** The PCI slot (00:02.0 in this example) can vary depending on your QEMU version and what other devices are configured. Use whatever slot `lspci` shows you.

Let's read the device ID register to make sure it's really our device:

```bash
# Find the BAR0 address (use YOUR slot from lspci above)
lspci -v -s 00:02.0 | grep "Memory at"
# Example output: Memory at febd1000 (32-bit, non-prefetchable) [size=4K]

# Read the device ID register (at offset 0x000)
# Use the address from the output above
devmem 0xfebd1000 w
```

**Expected output:**
```
0xF00DFACE
```

`0xF00DFACE` - that's the device's way of saying hello. If you see this, your fake FPGA is alive and happy!

**Note:** We use `devmem` (busybox's memory access tool) rather than `devmem2`. Same idea, slightly different output format.

## Now What? The Fun Part!

You've got the environment running. Now comes the actual learning:

1. **Complete the driver** in `driver/phantomfpga_drv.c`
   - Look for `/* TODO: ... */` comments - they tell you exactly what to implement
   - Follow the detailed guide in `docs/driver-guide.md`

2. **Complete the app** in `app/phantomfpga_app.c`
   - Same deal - find the TODOs, implement them

3. **Test your work**
   - Load driver, run app, watch data flow, feel accomplished

### Building the Driver (Inside the VM)

```bash
cd /mnt/driver    # This is your host's driver/ directory, shared with the VM
make
sudo insmod phantomfpga.ko
dmesg | tail -20
```

You should see messages about the driver loading and finding the device.

### Building the App (Inside the VM)

```bash
cd /mnt/app
mkdir -p build && cd build
cmake ..
make
./phantomfpga_app --help
```

## Documentation

Start here and work your way through:

1. **[Architecture Overview](docs/architecture.md)** - Understand how all the pieces fit together
2. **[Register Reference](docs/register-reference.md)** - The hardware interface you're programming against
3. **[Driver Implementation Guide](docs/driver-guide.md)** - Step-by-step instructions for completing the driver

## Running on ARM64

Want to try aarch64 instead of x86_64? Sure thing:

```bash
# Build the aarch64 guest image (if you haven't already in Step 3)
cd platform/buildroot
make aarch64
cd ../..

# Run the ARM64 VM
./platform/run_qemu.sh --arch aarch64
```

Same device, same driver code, different architecture. That's the beauty of proper abstractions.

**Which is faster?** The QEMU build (Step 2) already produces binaries for both architectures. The trick is: when your VM's architecture matches your host machine, QEMU can use hardware virtualization (KVM) and things run almost at native speed. When they don't match, QEMU has to emulate every instruction in software, which works but is noticeably slower. So if you're on a Mac with an M-series chip, aarch64 is your friend. On a regular PC, stick with x86_64.

## VM Options

```bash
# Headless mode (for CI or when you don't need graphics)
./platform/run_qemu.sh --headless

# Debug mode (starts GDB server on port 1234)
./platform/run_qemu.sh --debug

# More resources
./platform/run_qemu.sh --memory 4G --cpus 4

# Without KVM (slower but works anywhere)
./platform/run_qemu.sh --no-kvm

# SSH access (from your host machine, password: root)
ssh -p 2222 root@localhost
```

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

Did you build Buildroot?
```bash
cd platform/buildroot && make
```

### "Device not showing up in lspci"

Make sure you're using OUR QEMU, not the system QEMU:
```bash
./platform/run_qemu.sh          # Correct - uses our custom QEMU
qemu-system-x86_64 ...          # Wrong - uses system QEMU without our device
```

### "My driver crashed the kernel"

Welcome to kernel development! Check `dmesg` for clues. The VM will reboot. No real hardware was harmed. This is literally why we built this training platform.

### "I really messed things up"

Just reboot the VM. Or rebuild everything:
```bash
cd platform/buildroot && make clean && make
```

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
