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

You need **Linux**. This project won't run directly on macOS or Windows.

| Your computer | What to do |
|---------------|------------|
| Linux PC/laptop | You're good to go |
| Mac (M1/M2/M3) | Install [Lima](https://lima-vm.io/) or [UTM](https://mac.getutm.app/) with ARM64 Linux (e.g., Debian arm64) |
| Mac (Intel) | Install [Lima](https://lima-vm.io/) or [UTM](https://mac.getutm.app/) with x86_64 Linux |
| Windows | Use [WSL2](https://learn.microsoft.com/en-us/windows/wsl/install) with Ubuntu |

> [!IMPORTANT]
> **VM users:** Give your VM at least **8GB RAM** and **4 CPUs** -- the build compiles a lot of code in parallel. For Lima: `limactl edit <instance>` and add `cpus: 4` and `memory: "8GiB"`.

**Disk space:** About 15-20 GB.

Once you have Linux running, install these packages:

```bash
# Ubuntu/Debian - copy-paste this whole block
sudo apt-get update
sudo apt-get install -y \
    git build-essential ninja-build meson pkg-config \
    python3 python3-pip \
    libglib2.0-dev libpixman-1-dev libslirp-dev \
    libelf-dev libssl-dev flex bison \
    rsync bc cpio unzip wget
```

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
name "phantomfpga", bus PCI, desc "PhantomFPGA SG-DMA Training Device v2.0"
```

If you see this - congratulations! You built a fake FPGA. Your parents would be... confused but probably supportive.

### Step 3: Build the Guest Linux Image

This builds a minimal Linux system that runs inside QEMU.

```bash
cd platform/buildroot
make
cd ../..
```

> [!NOTE]
> This takes 20-40 minutes. The Makefile automatically picks the right architecture for your computer.

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

### Step 5: Verify the Device is There

Inside the VM, run:

```bash
lspci -nn | grep -i 0dad
```

**Expected output (slot may vary):**
```
00:01.0 Unclassified device [00ff]: Device [0dad:f00d] (rev 02)
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

`0xF00DFACE` - that's the device's way of saying hello. If you see this, your fake FPGA is alive and happy!

## Now What? The Fun Part!

You've got the environment running. Now comes the actual learning:

1. **Complete the driver** in `driver/phantomfpga_drv.c`
   - Look for `/* TODO: ... */` comments - they tell you exactly what to implement
   - Follow the detailed guide in `docs/driver-guide.md`

2. **Complete the app** in `app/phantomfpga_app.c`
   - Same deal - find the TODOs, implement them

3. **Test your work**
   - Load driver, run app, watch data flow, feel accomplished

### Building the Driver

Build on your host machine, test inside the VM. The `driver/` and `app/` directories on your host are automatically shared with the VM, so anything you build shows up inside the VM instantly.

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

### Building the App

```bash
# On your host:
cd app
make
```

```bash
# Inside the VM:
/mnt/app/phantomfpga_app --help
```

## Documentation

Start here and work your way through:

1. **[Architecture Overview](docs/architecture.md)** - Understand how all the pieces fit together
2. **[Register Reference](docs/register-reference.md)** - The hardware interface you're programming against
3. **[Driver Implementation Guide](docs/driver-guide.md)** - Step-by-step instructions for completing the driver

## Building for a Different Architecture

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

## VM Options

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

Did you build Buildroot? Make sure you wait for it to complete - it takes 20-40 minutes.
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
