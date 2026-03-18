# PhantomFPGA CI

This directory contains CI helper scripts. The actual pipeline is defined in
`.github/workflows/ci.yml` (GitHub Actions).

## Pipeline overview

The CI pipeline verifies that everything compiles. No artifacts are uploaded --
this is a training project, not a release pipeline.

```
+----------------+   +----------------+   +----------------+
|  Build QEMU    |   | Build Driver   |   |  Build App     |   <- All parallel
|  (x86 + arm64) |   |                |   |                |
+-------+--------+   +-------+--------+   +-------+--------+
        |                     |                    |
        +----------+----------+--------------------+
                   |
                   v
          +--------+---------+
          |   CI Success     |   <- Gate job (branch protection)
          +------------------+
```

All three build jobs run in parallel. The `CI Success` gate job waits for all
of them and fails if any failed. Use it as your branch protection rule.

## Triggers

- **Push to main/develop**: Full pipeline
- **Pull requests to main**: Full pipeline
- **Documentation changes** (`.md`, `docs/`, `LICENSE`): Skipped

## What gets tested

| Job | What it verifies |
|-----|------------------|
| Build QEMU | Device compiles into QEMU, appears in `-device help` |
| Build Driver | Driver module compiles against kernel headers, warns on warnings |
| Build App | App compiles with `make`, `--help` runs without crashing |
| CI Success | All of the above passed |

## Caching

The pipeline uses GitHub Actions caching for:

1. **QEMU source**: Cached by version (`v10.2.0`)
2. **QEMU builds**: Cached per architecture + source file hash

## Helper scripts

The `scripts/` directory has helper scripts for local use and future CI
expansion. They're not all wired into the GitHub Actions workflow yet.

| Script | Used by CI? | Description |
|--------|:-----------:|-------------|
| `build-qemu.sh` | No | Build QEMU with dependency detection |
| `build-buildroot.sh` | No | Build guest kernel and rootfs |
| `smoke-test.sh` | No | Quick file-existence and sanity checks |
| `run-integration-tests.sh` | No | Boot QEMU VM and run tests via SSH |

The CI workflow does its own inline build steps rather than calling these
scripts. The scripts are still useful for local development and testing.

### Usage examples

```bash
# Build QEMU for x86_64
./scripts/build-qemu.sh --arch x86_64 --jobs 8

# Build Buildroot with shared download cache
./scripts/build-buildroot.sh --arch both --download-dir /tmp/br-cache

# Run smoke tests
./scripts/smoke-test.sh --arch both

# Run integration tests (needs QEMU + guest images built first)
./scripts/run-integration-tests.sh --arch x86_64 --timeout 300
```

## Local testing

To reproduce what CI does locally:

```bash
# Build QEMU (same as CI)
cd platform/qemu
./setup.sh && make build

# Verify device exists
./build/qemu-system-x86_64 -device help 2>&1 | grep phantom

# Build driver (same as CI)
cd driver && make

# Build app (same as CI)
cd app && make
```

## Troubleshooting

### "PhantomFPGA device not found"

Make sure QEMU was built with our patches:
```bash
./platform/qemu/build/qemu-system-x86_64 -device help 2>&1 | grep phantom
```

### Driver build fails on CI but not locally

CI uses `ubuntu-22.04` with the runner's kernel headers. If your driver uses
APIs that differ between kernel versions, this can happen. Check the build log
for the exact error.

### Adding new CI checks

The workflow is in `.github/workflows/ci.yml`. Add a new job and include it
in the `ci-success` job's `needs` list so branch protection catches it.
