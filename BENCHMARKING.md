# LeanStore — Build & YCSB Benchmark Guide

Reference for building LeanStore with Nix and running the YCSB frontend with
reproducible configurations. Written against the `frontend/ycsb` binary in this
tree.

---

## 1. Build

The build is driven by `flake.nix` + `.envrc` (direnv). A dev shell provides
CMake, TBB, libaio, liburing, and the vendored deps (gflags/rapidjson/tabulate
are pre-fetched into the Nix store so no network is needed at configure time).

```bash
# One-time: allow direnv (or run `nix develop` manually each time)
direnv allow

# Configure + build inside the dev shell
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
make ycsb -j$(nproc)
```

Rebuild just the benchmark after editing `frontend/ycsb/ycsb.cpp`:

```bash
nix develop --command bash -c "cd build && make ycsb -j\$(nproc)"
```

> The dev shell sets `CMAKE_POLICY_VERSION_MINIMUM=3.5` so CMake 4.x accepts the
> vendored gflags/tabulate sub-builds. Don't drop it or the ExternalProject
> configure step fails.

The binary lands at `build/frontend/ycsb`.

---

## 2. Storage requirement

The SSD file is opened with `O_DIRECT`, which **does not work on tmpfs**
(`/tmp`, `/dev/shm`). Point `--ssd_path` at a real block device / filesystem.
The file is created only when `--trunc` is passed (which also truncates it).

```bash
--ssd_path=/mnt/nvme/leanstore.dat --trunc   # first run: create + zero the file
--ssd_path=/mnt/nvme/leanstore.dat           # reuse existing file
```

---

## 3. Flag reference (the ones that matter)

### Engine / buffer manager
| Flag | Default | Meaning |
|---|---|---|
| `--ssd_path` | — | Path to the backing file (must be O_DIRECT-capable) |
| `--trunc` | false | Create/truncate the backing file (use on first run) |
| `--dram_gib` | 1 | Buffer pool size in GiB |
| `--worker_threads` | 4 | Worker (client) threads |
| `--pp_threads` | 1 | Page-provider (eviction) threads |
| `--free_pct` | 1 | Target % of buffer frames kept free by eviction |
| `--worker_page_eviction` | false | If false, the per-op `evictLastPage()` call is a no-op |
| `--persist` | false | On shutdown, flush all buffer frames + serialize state |
| `--recover` | false | Reopen an existing dataset instead of inserting |

### Transactions / durability
| Flag | Default | Meaning |
|---|---|---|
| `--isolation_level` | — | `ru`, `rc`, `si`, `ser`. RocksDB default ≈ `rc` |
| `--wal` | true | Write-ahead log (durability). Set `false` for eviction-only |
| `--wal_variant` | 0 | 0 = fast path, **no latency instrumentation**; 1/2 = instrumented |
| `--vi` | true | In-place snapshot-isolation versioning (needs `--wal`) |
| `--mv` | true | Multi-version tuples |

> **Latency caveat:** per-commit latency is only recorded under
> `--wal_variant=1` or `2` (plus `--profile_latency`), which requires `--wal=true`.
> You cannot get LeanStore latencies in a non-durable (`--wal=false`) run.

### YCSB workload
| Flag | Default | Meaning |
|---|---|---|
| `--ycsb_tuple_count` | 0 | Number of key/value pairs (0 ⇒ derive from `--target_gib`) |
| `--ycsb_payload_size` | 100 | Value size in bytes. **Supported: 8, 100, 120, 512, 1024, 4096** |
| `--ycsb_read_ratio` | 100 | % reads; the rest are updates (e.g. 50 ⇒ 50/50) |
| `--ycsb_threads` | 0 | Benchmark threads (0 ⇒ use `--worker_threads`) |
| `--ycsb_insert_threads` | 0 | Threads for the load phase (0 ⇒ use `--worker_threads`) |
| `--zipf_factor` | 0 | 0 = uniform; >0 = Zipfian skew (e.g. 0.9) |

> `--ycsb_payload_size` selects a compile-time `BytesPayload<N>` instantiation at
> runtime. Only the values listed above are compiled in; to add another, add a
> `case N: return runYCSB<N>();` in `frontend/ycsb/ycsb.cpp` `main()`.
> The key is a fixed `u64` (8 bytes), so on-disk record ≈ `8 + N` bytes.

### Run control / output
| Flag | Default | Meaning |
|---|---|---|
| `--warmup_for_seconds` | 10 | Warmup before measurement |
| `--run_for_seconds` | — | Measured run length |
| `--profile_latency` | false | Enable latency profiling (with `--wal_variant=1/2`) |
| `--csv_path` | ./log | Prefix for CSV output (`<prefix>_cr.csv`, `_bm.csv`, `_dt.csv`, …) |
| `--csv_truncate` | false | Overwrite CSVs instead of appending |

### Output CSVs (`<csv_path>_*.csv`)
- `_cr.csv` — throughput (`tx` = committed txns/interval)
- `_bm.csv` — buffer manager IO: `r_mib`, `w_mib`, `free_pct`, `evicted_mib`
- `_dt.csv` — datastructure stats
- `_latency.csv` — commit latency in microseconds (only with instrumented WAL)

---

## 4. Ready-to-run configurations

All examples: 10M keys × 1 KiB values, 18 worker threads, 50/50 read/update,
RocksDB-equivalent isolation (`rc`). Adjust `--ssd_path` to your device.

### A. In-memory (fits the buffer pool) — LeanStore's best case
```bash
./build/frontend/ycsb \
  --ssd_path=/mnt/nvme/leanstore.dat --trunc \
  --dram_gib=20 \
  --ycsb_tuple_count=10000000 --ycsb_payload_size=1024 \
  --ycsb_read_ratio=50 \
  --worker_threads=18 \
  --pp_threads=4 \
  --isolation_level=rc \
  --wal=false --vi=false --mv=false \
  --warmup_for_seconds=15 --run_for_seconds=60 \
  --csv_path=./log_inmem --csv_truncate
```

### B. Out-of-memory, uniform (IO-bound; eviction only, no durability)
```bash
./build/frontend/ycsb \
  --ssd_path=/mnt/nvme/leanstore.dat --trunc \
  --dram_gib=5 \
  --ycsb_tuple_count=10000000 --ycsb_payload_size=1024 \
  --ycsb_read_ratio=50 --zipf_factor=0 \
  --worker_threads=18 \
  --pp_threads=4 \
  --isolation_level=rc \
  --wal=false --vi=false --mv=false \
  --warmup_for_seconds=15 --run_for_seconds=60 \
  --csv_path=./log_oom_uniform --csv_truncate
```

### C. Out-of-memory, Zipfian (representative skewed workload)
```bash
./build/frontend/ycsb \
  --ssd_path=/mnt/nvme/leanstore.dat --trunc \
  --dram_gib=5 \
  --ycsb_tuple_count=10000000 --ycsb_payload_size=1024 \
  --ycsb_read_ratio=50 --zipf_factor=0.9 \
  --worker_threads=18 \
  --pp_threads=4 \
  --isolation_level=rc \
  --wal=false --vi=false --mv=false \
  --warmup_for_seconds=15 --run_for_seconds=60 \
  --csv_path=./log_oom_zipf --csv_truncate
```

### D. Durable (WAL on) — enables latency measurement
```bash
./build/frontend/ycsb \
  --ssd_path=/mnt/nvme/leanstore.dat --trunc \
  --dram_gib=5 \
  --ycsb_tuple_count=10000000 --ycsb_payload_size=1024 \
  --ycsb_read_ratio=50 --zipf_factor=0.9 \
  --worker_threads=18 \
  --pp_threads=4 \
  --isolation_level=si \
  --wal=true --vi=true --wal_variant=2 --profile_latency \
  --warmup_for_seconds=15 --run_for_seconds=60 \
  --csv_path=./log_durable --csv_truncate
```

---

## 5. Notes & gotchas

- **Eviction ≠ durability.** With `--wal=false`, dirty pages are still written to
  the SSD via async eviction (`io_submit`/libaio), but there is no fsync at
  commit. For durability use the WAL configs (D).
- **Write-bandwidth bound at high update ratios.** In OOM 1 KiB 50/50 runs the
  eviction write rate saturates (~300 MiB/s on the test NVMe). Throughput ≈
  `device_write_ceiling / writes_per_op`, so the lever is write amplification:
  Zipfian skew lowers it (hot pages absorb many updates before eviction);
  more aggressive eviction (`--pp_threads` high, `--free_pct` high) *raises* it.
- **Tuning parity ≠ better.** For write-bound OOM workloads the default
  (`--pp_threads=4`, low `--free_pct`) can beat aggressive eviction.
- `--ycsb_payload_size` was previously defined but unused; it is now wired to the
  runtime payload dispatch.
- The `insert` (load) phase always uses SNAPSHOT_ISOLATION and can take a while
  for large datasets — budget for it before the measured window.
```
