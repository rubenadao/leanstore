# LeanStore — Building and Running with io_uring

This guide covers building LeanStore on the `io` branch using the Nix development environment and running YCSB benchmarks with io_uring on a regular file or block device.

---

## Prerequisites

- [Nix](https://nixos.org/download) with flakes enabled
- `direnv` (optional, for automatic environment activation)
- A pre-allocated storage file or block device

The Nix flake (`flake.nix`) provides all build dependencies:
`cmake`, `gcc`, `libaio`, `liburing`, `libnvme`, `boost`, `tbb`, `rocksdb`, `lmdb`, `wiredtiger`, and others.

---

## 1. Setup

### Branch

```bash
git checkout io
```

The `io` branch contains the VLDB 2023 high-performance I/O implementation with `libaio`, `io_uring`, SPDK, and xnvme backends.

### Make nix files visible to Git

The `flake.nix`, `flake.lock`, and `.envrc` must be staged (not necessarily committed) for `nix develop` to see them:

```bash
git add flake.nix flake.lock .envrc
```

### (Optional) direnv

If you have `direnv` installed, `direnv allow` will automatically activate the environment when you `cd` into the directory. Otherwise use `nix develop` explicitly for each command.

---

## 2. Build

```bash
# Configure
nix develop --command cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo .

# Build all targets
nix develop --command make -C build -j$(nproc)

# Or build only what you need
nix develop --command make -C build -j$(nproc) ycsb
nix develop --command make -C build -j$(nproc) iob
```

Binaries are placed in `build/frontend/`:

| Binary | Description |
|--------|-------------|
| `build/frontend/ycsb` | YCSB key-value benchmark |
| `build/frontend/iob` | Raw I/O benchmark tool |
| `build/frontend/frontend` | General frontend |
| `build/frontend/tpcc` | TPC-C benchmark |

> **Note:** `tpcc` may fail to compile under GCC 15 due to a discarded `std::unique()` return value being treated as an error. The other binaries are unaffected.

---

## 3. Storage file

LeanStore uses a pre-allocated file as its storage backend. Create and size it before running:

```bash
# Create a 14 GB storage file
fallocate -l 14G /mnt/nvme/leanstore

# Or to resize an existing one
fallocate -l 14G /mnt/nvme/leanstore
```

Size guidance: LeanStore's B-tree has overhead beyond raw data. For N records of V bytes each, budget roughly **2×** the raw data size on disk. For 10M × 1024B records, use at least 14 GB.

---

## 4. Running YCSB

### Key flags

| Flag | Description | Default |
|------|-------------|---------|
| `--ssd_path` | Path to the storage file | `./leanstore` |
| `--ioengine` | I/O backend: `io_uring`, `libaio`, `spdk` | `auto` |
| `--io_uring_poll_mode` | Enable `IORING_SETUP_IOPOLL` — **requires a raw NVMe block device**, do not use with regular files | `true` |
| `--dram_gib` | Buffer pool size in GiB | `1` |
| `--worker_threads` | Client/worker thread count | `10` |
| `--pp_threads` | Background page-provider thread count | — |
| `--worker_tasks` | Coroutine tasks per worker (affects I/O depth) | `1` |
| `--async_batch_size` | Async batch size (affects I/O depth) | `256` |
| `--ycsb_tuple_count` | Exact number of records to insert | `0` (uses `target_gib`) |
| `--target_gib` | Dataset size in GiB (used when `ycsb_tuple_count=0`) | `1700` |
| `--ycsb_read_ratio` | % of transactions that are reads (0–100) | `100` |
| `--zipf_factor` | Zipfian skew (0 = uniform, 0.99 = highly skewed) | `0` |
| `--run_for_seconds` | Benchmark duration | — |
| `--partition_bits` | Buffer pool partition bits | — |

> **Important:** The value size is hardcoded at compile time in `frontend/ycsb/ycsb.cpp` as `BytesPayload<N>`. Change N and rebuild to use a different size. Currently set to **1024 bytes**.

### Effective I/O depth

```
iodepth = (async_batch_size + worker_tasks) × 2
```

For iodepth ≈ 1024: `--async_batch_size=256 --worker_tasks=256`

### io_uring on a regular file

`IORING_SETUP_IOPOLL` (poll mode) only works on raw NVMe block devices. When using a regular file, always disable it:

```bash
--io_uring_poll_mode=false
```

---

## 5. Example commands

### YCSB-A equivalent (50% reads, 50% updates, uniform, 10M records)

```bash
nix develop --command ./build/frontend/ycsb \
  --ssd_path="/mnt/nvme/leanstore" \
  --ioengine=io_uring \
  --io_uring_poll_mode=false \
  --partition_bits=12 \
  --ycsb_tuple_count=10000000 \
  --ycsb_read_ratio=50 \
  --zipf_factor=0 \
  --run_for_seconds=30 \
  --dram_gib=5 \
  --worker_tasks=256 \
  --async_batch_size=256 \
  --worker_threads=16 \
  --pp_threads=4
```

### Read-only (YCSB-C), Zipfian 0.99

```bash
nix develop --command ./build/frontend/ycsb \
  --ssd_path="/mnt/nvme/leanstore" \
  --ioengine=io_uring \
  --io_uring_poll_mode=false \
  --partition_bits=12 \
  --ycsb_tuple_count=10000000 \
  --ycsb_read_ratio=100 \
  --zipf_factor=0.99 \
  --run_for_seconds=30 \
  --dram_gib=5 \
  --worker_tasks=256 \
  --async_batch_size=256 \
  --worker_threads=16 \
  --pp_threads=4
```

---

## 6. Reading the output

LeanStore prints a profiling table to stdout every second during the benchmark phase:

```
| t  | wt       | TX P [M] | TX A | TX C | W MiB  | R MiB  | ... |
| 1  | 1.000000 | 0.432061 | 0    | 0    | 2.6    | 1266.0 | ... |
```

| Column | Meaning |
|--------|---------|
| `t` | Elapsed seconds |
| `wt` | Wall time of this interval (should be ≈ 1.0) |
| `TX P [M]` | **Throughput in millions of TPS** (key result) |
| `TX A` | Aborted transactions |
| `TX C` | Committed transactions (WAL-based, 0 when WAL is off) |
| `W MiB` | Write bandwidth to storage (MiB/s) |
| `R MiB` | Read bandwidth from storage (MiB/s) |
| `Instrs/-` | CPU instructions per transaction |
| `Cycles/-` | CPU cycles per transaction |

**To convert TX P to plain TPS:** multiply by 1,000,000.  
Example: `0.432 M TPS = 432,000 TPS`

### CSV log files

LeanStore writes per-second metrics to CSV files in the working directory:

| File | Contents |
|------|----------|
| `log_bm.csv` | Buffer manager stats (pages, evictions) |
| `log_configs.csv` | Run configuration snapshot |
| `log_cpu.csv` | CPU counters |
| `log_cr.csv` | Concurrency/recovery counters |
| `log_dt.csv` | Data structure (B-tree) counters |
| `log_ssd.csv` | SSD / I/O counters |
| `log_thr.csv` | Per-thread counters |

These are written to whichever directory you launch the binary from.

---

## 7. Known issues and fixes applied

| Issue | Fix |
|-------|-----|
| `flake.nix` not found by `nix develop` | `git add flake.nix flake.lock .envrc` — Nix requires files to be git-tracked |
| Missing `boost::context` | Added `boost` to `flake.nix` devShell packages |
| Boost static lib not found (nixpkgs only ships shared) | Set `Boost_USE_STATIC_LIBS OFF` in `libs/boost.cmake` |
| Missing `libnvme.h` | Added `libnvme` to `flake.nix` devShell packages |
| `EOPNOTSUPP` from io_uring on regular file | Pass `--io_uring_poll_mode=false` (default is `true` in `Config.cpp`) |
| Crash: "Affinity could not be set" | `LeanStore.cpp`: changed hardcoded `CPU_SET(63, ...)` to `CPU_SET(std::thread::hardware_concurrency() - 1, ...)` |
