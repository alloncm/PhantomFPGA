# PhantomFPGA CI/CD Pipeline

This directory contains CI helper scripts. The actual pipeline is defined in
`.github/workflows/ci.yml` (GitHub Actions).

## Pipeline Overview

The GitHub Actions workflow runs these jobs:

```
+----------------+     +------------------+
|  Build QEMU    |     | Build Buildroot  |  <- Parallel
|  (x86 + arm64) |     |  (x86 + arm64)   |
+-------+--------+     +--------+---------+
        |                       |
        v                       v
+-------+--------+     +--------+---------+
|  Smoke Test    |     |  Build Driver    |
|                |     |  Build App       |
+-------+--------+     +--------+---------+
        |                       |
        +----------+------------+
                   |
                   v
          +--------+---------+
          | Integration Test |
          | (on push only)   |
          +------------------+
```

## Triggers

- **Push to main/develop**: Full pipeline including integration tests
- **Pull requests to main**: Build and smoke tests (faster feedback)
- **Documentation changes**: Skipped (saves resources)

## Helper Scripts

The `scripts/` directory contains helper scripts you can also run locally:

| Script | Description |
|--------|-------------|
| `build-qemu.sh` | Build QEMU with PhantomFPGA device |
| `build-buildroot.sh` | Build guest kernel and rootfs |
| `run-integration-tests.sh` | Run integration tests in QEMU VM |
| `smoke-test.sh` | Quick verification without booting VM |

### Usage Examples

```bash
# Build QEMU for x86_64
./scripts/build-qemu.sh --arch x86_64 --jobs 8

# Build Buildroot with shared download cache
./scripts/build-buildroot.sh --arch both --download-dir /tmp/br-cache

# Run integration tests
./scripts/run-integration-tests.sh --arch x86_64 --timeout 300

# Quick smoke test
./scripts/smoke-test.sh --arch both
```

## Caching Strategy

The pipeline uses GitHub Actions caching for:

1. **QEMU source**: Cached by version
2. **QEMU builds**: Cached per architecture + source hash
3. **Buildroot downloads**: Shared across all builds
4. **Buildroot output**: Cached per architecture + config hash

## What Gets Tested

| Check | What it verifies |
|-------|------------------|
| Build QEMU | Device compiles into QEMU |
| Build Driver | Driver compiles against kernel headers |
| Build App | App compiles with CMake |
| Smoke Test | PhantomFPGA device shows up in QEMU |
| Integration | VM boots, device is visible via lspci |

## Artifacts

The pipeline produces these artifacts (retained for 7 days):

| Artifact | Contents |
|----------|----------|
| `qemu-x86_64` | QEMU binary for x86_64 |
| `qemu-aarch64` | QEMU binary for aarch64 |
| `guest-x86_64` | Kernel + rootfs for x86_64 |
| `guest-aarch64` | Kernel + rootfs for aarch64 |

## Local Testing

To test what CI tests locally:

```bash
# Build QEMU (same as CI)
cd platform/qemu
./setup.sh && make build

# Verify device exists
./build/qemu-system-x86_64 -device help 2>&1 | grep phantom

# Build driver (same as CI)
cd driver && make

# Build app (same as CI)
cd app && mkdir -p build && cd build && cmake .. && make
```

## Timeouts

| Job | Timeout |
|-----|---------|
| QEMU build | 60 minutes |
| Buildroot build | 120 minutes |
| Integration tests | 15 minutes |

## Troubleshooting

### "PhantomFPGA device not found"

Make sure QEMU was built with our patches:
```bash
./platform/qemu/build/qemu-system-x86_64 -device help 2>&1 | grep phantom
```

### Build taking forever

Buildroot builds the entire guest Linux from source. First build takes 30-60
minutes. Subsequent builds use cache.

### Integration test failures

Check if all artifacts downloaded correctly. The VM needs both QEMU binary
and guest images to boot.

## Adding New Tests

1. Add test script to `tests/integration/`
2. Make it exit 0 on success, non-zero on failure
3. Add to `tests/integration/run_all.sh`
4. CI will pick it up automatically
