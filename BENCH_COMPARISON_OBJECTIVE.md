# Objective: LeanStore vs. osiris on YCSB-A (Zipfian) — what is the gap made of?

Handoff brief for an agent executing a controlled benchmark comparison. Self-contained:
it assumes no prior conversation context.

## 1. The question we are trying to answer

On a YCSB-A (50% read / 50% update), Zipfian, **out-of-memory** workload, osiris was observed
to be ~3.15× faster than LeanStore:

- LeanStore: **198,859 ops/s**
- osiris:    **626,653 ops/s**

**We want to know how much of that gap is a genuine storage-engine / architecture
difference, and how much is an artifact of a non-matched benchmark configuration and
I/O setup.** The deliverable is a *defensible, apples-to-apples* comparison plus an
attribution of the remaining gap to specific causes.

The two systems:
- **LeanStore** — in-place, hybrid-latched (optimistic) B-tree with a coroutine/io_uring
  I/O stack and a buffer pool managed by dedicated page-provider threads.
- **osiris** — the user's system, benchmarked via a `db_bench`-style harness. Its flags
  (`--cache_impl=hyperclock`, `--kv=.../db`, `--buffer_pool_size`) strongly suggest an
  **LSM / RocksDB-family** engine. Confirm this; it matters (see §4).

## 2. The two configurations as originally run (verbatim)

LeanStore:
```
build/frontend/ycsb --ssd_path=/home/gsd/daniel/nvme/leanstore --trunc --ioengine=io_uring \
  --io_uring_poll_mode=false --partition_bits=12 --dram_gib=14 \
  --ycsb_tuple_count=136718750 --ycsb_payload_size=1024 --ycsb_read_ratio=50 \
  --zipf_factor=0.99 --worker_threads=32 --pp_threads=8 --worker_tasks=256 \
  --async_batch_size=256 --wal=false --vi=false --run_for_seconds=600 \
  --csv_path=run --csv_truncate
```

osiris:
```
build/db_bench --benchmarks=ycsb-a --num=1093750000 --key_space=1093750000 \
  --value_size=128 --duration=600 --threads=32 --num_workers=8 --outstanding=512 \
  --ring_depth=512 --sync=false --stats=true --cache_size=0GB --buffer_pool_size=14GB \
  --cache_impl=hyperclock --distribution=zipfian --use_kv=true \
  --kv=/home/gsd/daniel/nvme/osiriskv/db --warmup=true --warmup_seconds=30
```

## 3. Configuration parity analysis (what is and isn't matched)

Matched:
- **RAM / cache budget:** 14 GB each (LeanStore `dram_gib=14`; osiris `buffer_pool_size=14GB`,
  `cache_size=0GB`).
- **Total dataset bytes:** ~140 GB each. 136,718,750 × 1024 B == 1,093,750,000 × 128 B
  (exactly 8× the records at 1/8 the record size). So both are ~10:1 dataset:RAM → both
  genuinely out-of-memory.
- **Durability:** both effectively off (LeanStore `wal=false`; osiris `sync=false`).
- **Threads:** 32 each; 8 background workers each (`pp_threads` vs `num_workers`).
- **Skew:** Zipf 0.99 both. **Read ratio:** 50/50 both.

NOT matched (these are the suspects):
- **Record size: 1024 B (LeanStore) vs 128 B (osiris) — 8×.** Affects per-op bytes moved and
  cache packing. Effect direction is ambiguous and depends on each engine's cache
  granularity (page/block vs record/row); must be measured, not assumed.
- **I/O queue depth: 256 (LeanStore `worker_tasks`/`async_batch_size`) vs 512 (osiris
  `outstanding`/`ring_depth`).** NVMe random-read IOPS scales with in-flight depth up to a
  knee ~QD128–512; LeanStore may be below the knee.
- **I/O backend:** LeanStore on a **buffered regular file** with `io_uring_poll_mode=false`
  (not its fast path: syscall + VFS/page-cache per completion). Unknown whether osiris uses
  O_DIRECT / polling. Same physical NVMe in the original runs (`/home/gsd/daniel/nvme/...`).
- **Write architecture:** in-place B-tree page writeback (random 4 KiB writes via page-provider
  threads) vs (likely) LSM memtable+sequential-flush+background-compaction. YCSB-A is 50%
  writes, so this is potentially the dominant factor (see §4).

## 4. Hypotheses, ranked by expected impact

1. **Write path (likely dominant).** If osiris is LSM-structured, its update half is nearly
   free during the run (memtable absorb, sequential flush, deferred compaction, `sync=false`),
   while LeanStore does random 4 KiB dirty-page writebacks that compete with reads for the
   same device queue. YCSB-A being 50% writes, this alone could explain most of 3.15×.
2. **Queue depth (512 vs 256).** osiris may simply extract more IOPS from the same drive.
3. **I/O backend (regular file + poll off).** LeanStore off its fast path → fewer IOPS from
   identical hardware.
4. **Record size (8×).** Fairness caveat; affects bytes/op and cache efficiency.
5. **Restart storm (LeanStore-specific CPU tax).** Under heavy eviction the optimistic B-tree
   re-traverses ~40×/miss (measured — see §6). Secondary unless the run becomes CPU-bound
   after the I/O path is fixed.

## 5. Experiments to run (in order)

**E1 — Isolate the read path: YCSB-C (100% reads) on both, otherwise matched.**
LeanStore: `--ycsb_read_ratio=100`. osiris: its read-only / ycsb-c mode.
*Interpretation:* if LeanStore roughly matches osiris on pure reads but loses on YCSB-A, the
gap is the write path (hypothesis 1) — the most important and most architectural finding.

**E2 — Matched YCSB-A.** Equalize value size, key count, queue depth, device, durability, then
re-run 50/50. Example matched LeanStore command (value 128 B, 512 QD; scale per §7):
```
build/frontend/ycsb --ssd_path=<DEVICE_PATH> --trunc --ioengine=io_uring \
  --io_uring_poll_mode=false --partition_bits=12 --dram_gib=<POOL_GIB> \
  --ycsb_tuple_count=<KEYS> --ycsb_payload_size=128 --ycsb_read_ratio=50 \
  --zipf_factor=0.99 --worker_threads=32 --pp_threads=8 --worker_tasks=512 \
  --async_batch_size=512 --wal=false --vi=false --run_for_seconds=<SECS> \
  --csv_path=run --csv_truncate
```
The gap remaining after E1+E2 is the genuinely architectural part.

**E3 — Device-level capture during every run:** `iostat -x 1 <device>` (and `mpstat`/`top` for
CPU). Tells:
- LeanStore showing heavy **random write** IOPS competing with reads, osiris bursty
  **sequential** writes → confirms hypothesis 1 (write path).
- osiris higher total IOPS / %util → hypotheses 2/3 (depth / backend).
- Both pegged at the drive max but osiris more *ops* per I/O → hypothesis 4 (cache efficiency).

**E4 (optional) — Sweeps:** LeanStore queue depth 256→512→1024; buffer pool size; poll mode on
a raw device if available. To quantify hypotheses 2 and 3 in isolation.

## 6. Metrics to collect per run

- Throughput (ops/s), run duration, warmup handling.
- **Read-miss / hit rate** on each system, and **I/Os per op**.
- Device: **IOPS, bandwidth, %util, read vs write split** (iostat -x). CPU utilization.
- Latency p50 / p99 / p99.9.
- **LeanStore-specific (already instrumented in this tree):**
  - `===LEANSTORE_OP_LATENCY===` table — per-op (read/update × cached/miss) Avg/p50/p99/p99.9,
    I/O-wait%, Avg I/O, **Rst/op** (re-traversals per op), **LeafCont%** (leaf read/write
    contention). Also written to `log_latency_<csv_path>.txt`.
  - `===LEANSTORE_LOOKUP_IO===` histogram of I/Os per lookup. Also `log_lookup_io_<csv_path>.txt`.
  - CSV logs: `log_thr.csv` (throughput/tx per thread), `log_cpu.csv`, `log_bm.csv`,
    `log_dt.csv` (datastructure: `dt_restarts_read`, `contention_split_succ/fail`),
    `log_ssd.csv`, `log_cr.csv`, `log_configs.csv`.

### Empirical findings already in hand (from LeanStore instrumentation, original out-of-memory run)
- Point lookups take ~1 I/O per miss (max 3); the inner B-tree path stays cached.
- Misses averaged ~30 ms (p99 ~393 ms) at ~99% I/O-wait → **device saturated; the engine is
  I/O-wall-bound, not CPU/btree-bound.** (In-memory LeanStore on a dev box does ~1.9M tx/s,
  ~10× the out-of-memory number — the whole gap lives in the I/O path.)
- ~**40 re-traversals per miss op** (`Rst/op`), driven by eviction/swizzling churn invalidating
  optimistic paths during the long I/O wait — **not** leaf read/write contention
  (`LeafCont%` ≈ 0 on misses, ~0.3% on cached ops). Contention-splitting keeps leaf
  contention negligible even at high skew.

## 7. Environment constraints on THIS host (trooper) — must resolve before running

Discovered via pre-flight check:
- **Host:** trooper. **24 cores. 30 GB RAM total (~17 GB free).** (NOT the original `gsd` host.)
- **`/mnt/bench`** = `/dev/sda1`, ext4, **79 GB free**. Likely SATA (`sda`), *not* NVMe.
- **`/mnt/data`** = `/dev/nvme0n1p2`, ext4 — the NVMe mount (closer to the original hardware).
- LeanStore `build/frontend/ycsb` is present. **osiris / `db_bench` binary was NOT located on
  this host — its path must be supplied.**

Implications / decisions the executing agent must settle:
1. **Dataset won't fit at original scale.** ~140 GB each vs 79 GB free on /mnt/bench. Must shrink.
   **Recommended:** `dram_gib`/pool = 4 GB, dataset ~40 GB, keeping the ~10:1 out-of-memory
   ratio (e.g. LeanStore `--ycsb_tuple_count` for 128 B records ≈ 40 GB / (128 B + overhead);
   pick matching osiris `--num`). Keep both ≤ what the chosen filesystem holds (two datasets if
   run side by side, or truncate between runs).
2. **RAM is 30 GB.** A 14 GB pool is tight and leaves little for OS/page-cache; 4 GB is safer and
   preserves the out-of-memory character. Do not run both systems simultaneously.
3. **Device choice:** `/mnt/bench` (sda, as requested — fair A/B but SATA, absolute numbers won't
   match the NVMe originals) vs `/mnt/data` (NVMe, closer to original). Check free space on the
   chosen device first.
4. **osiris binary path on trooper** — required to run the osiris side.
5. **LeanStore page size** — determine the configured page size (affects bytes/IO and
   records-per-leaf, hence cache packing for the record-size analysis).

## 8. Deliverable

A short report that states, with iostat/throughput/latency evidence:
- the controlled (matched) YCSB-A throughput for each engine on the same device and scale;
- the YCSB-C (read-only) result isolating the read path;
- an attribution of the ~3.15× original gap across: write architecture, queue depth, I/O
  backend, record size, and any residual engine difference;
- whether, after matching, LeanStore is I/O-bound or CPU-bound (restart storm) on this host.
