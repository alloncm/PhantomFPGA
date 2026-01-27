# PhantomFPGA CI/CD Pipeline

This directory contains the CI/CD configuration for the PhantomFPGA training platform.

## Pipeline Overview

The Azure Pipelines configuration (`azure-pipelines.yml`) runs the following stages:

```
+----------------+     +------------------+
|  Build QEMU    |     | Build Buildroot  |  <- Parallel
|  (x86 + arm64) |     |  (x86 + arm64)   |
+-------+--------+     +--------+---------+
        |                       |
        v                       v
+-------+--------+     +--------+---------+
|  QTest Unit    |     | Integration      |
|    Tests       |     |    Tests         |
+-------+--------+     +--------+---------+
        |                       |
        +----------+------------+
                   |
                   v
          +--------+---------+
          | Collect Artifacts|
          +------------------+
```

## Triggers

- **Push to main**: Full pipeline
- **Pull requests to main**: Full pipeline
- **Feature branches**: Full pipeline
- **Documentation changes**: Skipped (*.md, docs/**)

## Helper Scripts

The `scripts/` directory contains helper scripts for CI tasks:

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
./scripts/run-integration-tests.sh --arch x86_64 --timeout 300 --junit results.xml

# Quick smoke test
./scripts/smoke-test.sh --arch both
```

## Caching Strategy

The pipeline uses Azure Pipelines caching for:

1. **QEMU builds**: Cached per commit + architecture
2. **Buildroot downloads**: Shared across all builds
3. **Buildroot output**: Cached per commit + architecture

Cache keys include a version string (`CACHE_VERSION`) that can be incremented to invalidate caches.

## KVM Support

The pipeline is designed to work with:

1. **Microsoft-hosted agents**: Standard_D4s_v3 VMs support nested virtualization
2. **Self-hosted agents**: Must have KVM access (`/dev/kvm`)

When KVM is not available, tests run in software emulation mode (slower but functional).

## Test Results

Test results are published in JUnit XML format:
- `qtest-results.xml`: Unit test results
- `integration-results-*.xml`: Integration test results

## Artifacts

The pipeline produces these artifacts:

| Artifact | Contents |
|----------|----------|
| `qemu-x86_64` | QEMU binary for x86_64 |
| `qemu-aarch64` | QEMU binary for aarch64 |
| `images-x86_64` | Kernel + rootfs for x86_64 |
| `images-aarch64` | Kernel + rootfs for aarch64 |
| `logs-integration-*` | Test logs for debugging |
| `build-summary` | Build information |

## Timeouts

| Stage | Timeout |
|-------|---------|
| QEMU build | 60 minutes |
| Buildroot build | 120 minutes |
| Unit tests | 30 minutes |
| Integration tests | 30 minutes |

## Local Testing

To test the CI pipeline locally:

```bash
# Run the smoke test
./ci/scripts/smoke-test.sh

# Build everything
./ci/scripts/build-qemu.sh --arch both
./ci/scripts/build-buildroot.sh --arch both

# Run integration tests
./ci/scripts/run-integration-tests.sh --arch x86_64
```

## Self-Hosted Agent Setup

For self-hosted agents with KVM:

1. Install agent with `kvm` capability
2. Add user to `kvm` group: `sudo usermod -aG kvm azureuser`
3. Uncomment self-hosted pool section in `azure-pipelines.yml`

## Troubleshooting

### Build Failures

1. Check if dependencies are installed
2. Verify QEMU version compatibility
3. Check disk space (Buildroot needs ~10GB)

### Test Failures

1. Check QEMU log in artifacts
2. Verify KVM availability
3. Check timeout settings

### Cache Issues

Increment `CACHE_VERSION` variable to invalidate all caches:

```yaml
variables:
  CACHE_VERSION: 'v2'  # was 'v1'
```
