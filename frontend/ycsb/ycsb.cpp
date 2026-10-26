#include "Time.hpp"
#include "Units.hpp"
#include "leanstore/BTreeAdapter.hpp"
#include "leanstore/Config.hpp"
#include "leanstore/LeanStore.hpp"
#include "leanstore/profiling/counters/WorkerCounters.hpp"
#include "leanstore/profiling/counters/ThreadCounters.hpp"
#include "leanstore/profiling/counters/PPCounters.hpp"
#include "leanstore/utils/FVector.hpp"
#include "leanstore/utils/Files.hpp"
#include "leanstore/utils/RandomGenerator.hpp"
#include "leanstore/utils/ScrambledZipfGenerator.hpp"
// -------------------------------------------------------------------------------------
#include "leanstore/concurrency/Mean.hpp"
#include "leanstore/concurrency/Task.hpp"
#include "leanstore/io/IoInterface.hpp"
// -------------------------------------------------------------------------------------
#include <gflags/gflags.h>
// -------------------------------------------------------------------------------------
#include <iomanip>
#include <iostream>
#include <locale>
#include <set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <atomic>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <sys/stat.h>
// -------------------------------------------------------------------------------------
// Per-lookup I/O profiling: count the page reads a single lookup incurs and build a
// histogram plus the global worst case. The counter lives on the coroutine Task
// (BufferManager::readPageSync increments mean::task::this_task().io_count), so it is
// correct across worker-thread migration at yield points and under any --worker_tasks.
static std::atomic<u64> g_max_lookup_ios{0};
static std::array<std::atomic<u64>, 16> g_lookup_io_hist{};  // bucket 15 = ">=15" overflow
static void recordLookupIos(u64 ios)
{
   // Track the TRUE (unclamped) worst case first.
   u64 prev = g_max_lookup_ios.load(std::memory_order_relaxed);
   while (ios > prev && !g_max_lookup_ios.compare_exchange_weak(prev, ios, std::memory_order_relaxed)) {}
   // Histogram bucket (last bucket is a >=15 overflow catch-all).
   const u64 b = ios > 15 ? 15 : ios;
   g_lookup_io_hist[b].fetch_add(1, std::memory_order_relaxed);
}
// Compact human-readable count: 1234567 -> "1.23M", 15679 -> "15.7K", 32 -> "32".
static std::string humanCount(u64 n)
{
   char buf[32];
   if (n >= 1000000ull)   std::snprintf(buf, sizeof(buf), "%.2fM", n / 1e6);
   else if (n >= 1000ull) std::snprintf(buf, sizeof(buf), "%.1fK", n / 1e3);
   else                   std::snprintf(buf, sizeof(buf), "%llu", (unsigned long long)n);
   return buf;
}
// Share as a percentage with precision that adapts to magnitude.
static std::string humanPct(double p)
{
   if (p == 0.0) return "0%";
   char buf[32];
   if (p >= 1.0)        std::snprintf(buf, sizeof(buf), "~%.1f%%", p);
   else if (p >= 0.01)  std::snprintf(buf, sizeof(buf), "~%.2f%%", p);
   else                 std::snprintf(buf, sizeof(buf), "~%.4g%%", p);
   return buf;
}
// Display width = number of UTF-8 code points (so multi-byte chars like "—" count as 1).
static size_t dispWidth(const std::string& s)
{
   size_t w = 0;
   for (unsigned char c : s)
      if ((c & 0xC0) != 0x80) w++;  // count every byte that is not a UTF-8 continuation byte
   return w;
}
// Render the per-lookup I/O histogram as a boxed table to the given stream.
static void printLookupIoTable(std::ostream& os)
{
   const std::array<std::string, 3> hdr = {"I/Os", "Count", "Share"};
   std::vector<std::array<std::string, 3>> rows;
   u64 total = 0;
   for (size_t b = 0; b < g_lookup_io_hist.size(); b++) total += g_lookup_io_hist[b].load();
   for (size_t b = 0; b < g_lookup_io_hist.size(); b++) {
      const u64 c = g_lookup_io_hist[b].load();
      if (!c) continue;
      const std::string label = (b == 15 ? "15+" : std::to_string(b));
      const double share = total ? 100.0 * c / total : 0.0;
      rows.push_back({label, humanCount(c), humanPct(share)});
   }
   rows.push_back({"max", std::to_string(g_max_lookup_ios.load()), "—"});
   // Column widths from header + all cells (by display width).
   size_t w[3];
   for (int i = 0; i < 3; i++) {
      w[i] = dispWidth(hdr[i]);
      for (auto& r : rows) w[i] = std::max(w[i], dispWidth(r[i]));
   }
   auto rule = [&](const char* l, const char* m, const char* r) {
      std::string s = l;
      for (int i = 0; i < 3; i++) {
         for (size_t k = 0; k < w[i] + 2; k++) s += "─";
         s += (i < 2 ? m : r);
      }
      os << s << "\n";
   };
   auto row = [&](const std::array<std::string, 3>& cells) {
      std::string s = "│";
      for (int i = 0; i < 3; i++) {
         s += " " + cells[i];
         for (size_t k = dispWidth(cells[i]); k < w[i]; k++) s += " ";  // left-align pad
         s += " │";
      }
      os << s << "\n";
   };
   rule("┌", "┬", "┐");
   row(hdr);
   rule("├", "┼", "┤");
   for (size_t i = 0; i < rows.size(); i++) {
      row(rows[i]);
      if (i + 1 < rows.size()) rule("├", "┼", "┤");
   }
   rule("└", "┴", "┘");
}
// -------------------------------------------------------------------------------------
// Format a double compactly for the latency table (more precision for small values).
static std::string fmtNum(double v)
{
   char buf[32];
   if (v >= 100)      std::snprintf(buf, sizeof(buf), "%.0f", v);
   else if (v >= 10)  std::snprintf(buf, sizeof(buf), "%.1f", v);
   else               std::snprintf(buf, sizeof(buf), "%.2f", v);
   return buf;
}
// Generic boxed-table renderer (any column count), left-aligned, UTF-8-width aware.
static void renderBoxTable(std::ostream& os,
                           const std::vector<std::string>& hdr,
                           const std::vector<std::vector<std::string>>& rows)
{
   const size_t ncol = hdr.size();
   std::vector<size_t> w(ncol);
   for (size_t i = 0; i < ncol; i++) {
      w[i] = dispWidth(hdr[i]);
      for (auto& r : rows) if (i < r.size()) w[i] = std::max(w[i], dispWidth(r[i]));
   }
   auto rule = [&](const char* l, const char* m, const char* r) {
      std::string s = l;
      for (size_t i = 0; i < ncol; i++) {
         for (size_t k = 0; k < w[i] + 2; k++) s += "─";
         s += (i + 1 < ncol ? m : r);
      }
      os << s << "\n";
   };
   auto rowf = [&](const std::vector<std::string>& cells) {
      std::string s = "│";
      for (size_t i = 0; i < ncol; i++) {
         const std::string c = i < cells.size() ? cells[i] : std::string();
         s += " " + c;
         for (size_t k = dispWidth(c); k < w[i]; k++) s += " ";  // left-align pad
         s += " │";
      }
      os << s << "\n";
   };
   rule("┌", "┬", "┐");
   rowf(hdr);
   rule("├", "┼", "┤");
   for (size_t i = 0; i < rows.size(); i++) {
      rowf(rows[i]);
      if (i + 1 < rows.size()) rule("├", "┼", "┤");
   }
   rule("└", "┴", "┘");
}
// -------------------------------------------------------------------------------------
// Per-operation latency breakdown. For each op we record total latency, how much of it was
// spent blocked on page reads (io_wait, measured on the coroutine Task in readPageSync),
// and the page-read count -- segmented by op type (read/update) and whether it was served
// from cache (0 I/O) or took a miss (>=1 I/O). This isolates the latency sources on a mixed
// Zipfian workload: cached updates expose latch+WAL contention (I/O-wait ~0), misses expose
// device I/O wait, and (cached-update minus cached-read) avg is the contention+WAL cost.
struct OpStats {
   static constexpr int SUB = 16;   // sub-buckets per octave (~6% relative resolution)
   static constexpr int OCT = 28;   // octaves: covers up to 2^27 us ~ 134 s
   static constexpr int N = OCT * SUB;
   std::array<std::atomic<u64>, N> hist{};  // latency histogram over microseconds
   std::atomic<u64> count{0};
   std::atomic<u64> sum_total_us{0};
   std::atomic<u64> sum_io_wait_us{0};
   std::atomic<u64> sum_io_count{0};
   std::atomic<u64> sum_restarts{0};    // total optimistic re-traversals (jumpmu restarts)
   std::atomic<u64> ops_leaf_cont{0};   // ops whose leaf guard faced read/write contention
   static int bucketOf(u64 us)
   {
      if (us == 0) us = 1;
      int e = 63 - __builtin_clzll(us);  // floor(log2(us))
      if (e >= OCT) e = OCT - 1;
      const u64 base = 1ull << e;
      int sub = static_cast<int>(((us - base) * SUB) / base);
      if (sub >= SUB) sub = SUB - 1;
      const int idx = e * SUB + sub;
      return idx < N ? idx : N - 1;
   }
   static u64 valueOf(int idx)  // lower bound of a bucket, in microseconds
   {
      const int e = idx / SUB, sub = idx % SUB;
      const u64 base = 1ull << e;
      return base + (base / SUB) * sub;
   }
   void record(u64 total_us, u64 io_wait_us, u64 ios, u64 restarts, bool leaf_cont)
   {
      count.fetch_add(1, std::memory_order_relaxed);
      sum_total_us.fetch_add(total_us, std::memory_order_relaxed);
      sum_io_wait_us.fetch_add(io_wait_us, std::memory_order_relaxed);
      sum_io_count.fetch_add(ios, std::memory_order_relaxed);
      sum_restarts.fetch_add(restarts, std::memory_order_relaxed);
      if (leaf_cont) ops_leaf_cont.fetch_add(1, std::memory_order_relaxed);
      hist[bucketOf(total_us)].fetch_add(1, std::memory_order_relaxed);
   }
   void reset()
   {
      for (auto& h : hist) h.store(0, std::memory_order_relaxed);
      count.store(0); sum_total_us.store(0); sum_io_wait_us.store(0); sum_io_count.store(0);
      sum_restarts.store(0); ops_leaf_cont.store(0);
   }
   u64 percentile(double p) const
   {
      const u64 tot = count.load();
      if (!tot) return 0;
      const u64 target = static_cast<u64>(tot * p / 100.0);
      u64 cum = 0;
      for (int i = 0; i < N; i++) {
         cum += hist[i].load();
         if (cum >= target) return valueOf(i);
      }
      return valueOf(N - 1);
   }
};
enum { OP_READ = 0, OP_UPDATE = 1, POP_CACHED = 0, POP_MISS = 1 };
static std::array<OpStats, 4> g_op_stats{};  // indexed [op*2 + pop]
static OpStats& opStats(int op, int pop) { return g_op_stats[op * 2 + pop]; }
static void recordOpLatency(bool is_read, u64 ios, u64 total_us, u64 io_wait_us, u64 restarts, u64 leaf_cont)
{
   opStats(is_read ? OP_READ : OP_UPDATE, ios ? POP_MISS : POP_CACHED)
       .record(total_us, io_wait_us, ios, restarts, leaf_cont > 0);
}
// Render the per-op latency breakdown (read/update x cached/miss) as a boxed table.
static void printLatencyTable(std::ostream& os)
{
   const std::vector<std::string> hdr =
       {"Op", "Pop", "Count", "Avg us", "p50 us", "p99 us", "p99.9 us", "I/O-wait%", "Avg I/O", "Rst/op", "LeafCont%"};
   const char* opname[2] = {"read", "update"};
   const char* popname[2] = {"cached", "miss"};
   std::vector<std::vector<std::string>> rows;
   for (int op = 0; op < 2; op++) {
      for (int pop = 0; pop < 2; pop++) {
         OpStats& s = opStats(op, pop);
         const u64 c = s.count.load();
         if (!c) continue;
         const u64 tot_us = s.sum_total_us.load();
         const double io_wait_pct = tot_us ? 100.0 * s.sum_io_wait_us.load() / tot_us : 0.0;
         const double leaf_cont_pct = 100.0 * s.ops_leaf_cont.load() / c;
         rows.push_back({opname[op], popname[pop], humanCount(c),
                         fmtNum(static_cast<double>(tot_us) / c),
                         std::to_string(s.percentile(50)),
                         std::to_string(s.percentile(99)),
                         std::to_string(s.percentile(99.9)),
                         humanPct(io_wait_pct),
                         fmtNum(static_cast<double>(s.sum_io_count.load()) / c),
                         fmtNum(static_cast<double>(s.sum_restarts.load()) / c),
                         humanPct(leaf_cont_pct)});
      }
   }
   renderBoxTable(os, hdr, rows);
}
// -------------------------------------------------------------------------------------
DEFINE_uint32(ycsb_read_ratio, 100, "");
DEFINE_string(ycsb_workloads, "",
              "Comma-separated workloads run back-to-back over ONE fill. Each entry is "
              "name[:zipf_factor[:read_ratio[:insert]]], e.g. 'uniform:0:50,zipfian:0.99:50'. "
              "A trailing ':insert' makes it YCSB-D: writes insert fresh keys instead of "
              "updating, and reads draw from the keys inserted so far (zipf_factor > 0: "
              "latest, skewed to the newest; 0: uniform). "
              "Empty = single run using --zipf_factor/--ycsb_read_ratio.");
DEFINE_bool(ycsb_print_tree_stats, false,
            "Also print total_nodes/inner_nodes after fill. Off by default: these walk the "
            "whole B-tree, faulting every evicted node through the pool (slow out-of-cache).");
DEFINE_uint64(ycsb_tuple_count, 0, "");
DEFINE_uint32(ycsb_payload_size, 100, "tuple size in bytes");
DEFINE_uint32(ycsb_warmup_rounds, 0, "");
DEFINE_uint64(ycsb_warmup_seconds, 0,
              "Before each measured workload, run it unmeasured for this long (logged as "
              "'warmup_<name>'), so the measured window starts on its own steady state "
              "instead of the previous workload's cache and dirty pages. 0 = off.");
DEFINE_bool(ycsb_drain_after_fill, false,
            "After the fill, run unmeasured uniform reads (logged as 'drain_after_fill') until "
            "the fill's write-back has died down, so it does not bleed into the first measured "
            "workload. Reads write nothing themselves, so every page written meanwhile is the "
            "fill's.");
DEFINE_uint64(ycsb_drain_max_seconds, 3600, "Upper bound on the drain phase.");
DEFINE_double(ycsb_drain_write_pct, 1.0,
              "Drain ends once pages written over the last 10 s are below this % of pages read.");
DEFINE_uint32(ycsb_tx_rounds, 1, "");
DEFINE_uint32(ycsb_tx_count, 0, "default = tuples");
DEFINE_bool(verify, false, "");
DEFINE_bool(ycsb_scan, false, "");
DEFINE_bool(ycsb_tx, true, "");
DEFINE_bool(ycsb_count_unique_lookup_keys, true, "");
// -------------------------------------------------------------------------------------
using namespace leanstore;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using YCSBKey = u64;
// Value size is compile-time (BytesPayload<N>); override with -DYCSB_PAYLOAD_SIZE=N.
// --ycsb_payload_size is NOT read — it must match this for the reported config to be true.
#ifndef YCSB_PAYLOAD_SIZE
#define YCSB_PAYLOAD_SIZE 1024
#endif
using YCSBPayload = BytesPayload<YCSB_PAYLOAD_SIZE>;
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
double calculateMTPS(chrono::high_resolution_clock::time_point begin, chrono::high_resolution_clock::time_point end, u64 factor)
{
   double tps = ((factor * 1.0 / (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0)));
   return (tps / 1000000.0);
}
// -------------------------------------------------------------------------------------
struct YcsbWorkload {
   std::string name;
   double zipf_factor;
   u32 read_ratio;
   bool insert = false;  // YCSB-D: writes are inserts of fresh keys
   u64 seconds = 0;      // 0 = --run_for_seconds
   bool drain = false;   // ends early once the fill's write-back has settled
};
// -------------------------------------------------------------------------------------
// YCSB-D support, mirroring YCSB's AcknowledgedCounterGenerator + SkewedLatestGenerator
// (and OsirisKV's db_bench). Keys [0, start) are the loaded tuples; inserts take start,
// start+1, ... from one counter shared by all tasks. Last() is the highest key K such that
// every key <= K has finished inserting, so reads never target an in-flight insert.
// The counter lives for the whole process: a later workload keeps inserting fresh keys.
class AcknowledgedCounter
{
  public:
   explicit AcknowledgedCounter(u64 start) : next_(start), limit_(start - 1), window_(new std::atomic<bool>[kWindow]())
   {
      ensure(start > 0);
   }
   u64 next() { return next_.fetch_add(1, std::memory_order_relaxed); }
   u64 last() const { return limit_.load(std::memory_order_acquire); }
   void acknowledge(u64 key)
   {
      // In-flight inserts are bounded by worker_threads * worker_tasks, far below kWindow.
      ensure(key - limit_.load(std::memory_order_acquire) <= kWindow);
      window_[key & kMask].store(true, std::memory_order_release);
      // One task at a time slides the limit over the acknowledged prefix; a task that
      // loses try_lock leaves its slot for the holder or the next acknowledge (never blocks).
      if (!mu_.try_lock()) return;
      u64 l = limit_.load(std::memory_order_relaxed);
      while (window_[(l + 1) & kMask].load(std::memory_order_acquire)) {
         window_[(l + 1) & kMask].store(false, std::memory_order_relaxed);
         ++l;
      }
      limit_.store(l, std::memory_order_release);
      mu_.unlock();
   }

  private:
   static constexpr u64 kWindow = u64{1} << 20;
   static constexpr u64 kMask = kWindow - 1;
   std::atomic<u64> next_;
   std::atomic<u64> limit_;
   std::unique_ptr<std::atomic<bool>[]> window_;
   std::mutex mu_;
};
// Unscrambled Zipf rank over [0, n) (Gray et al., as YCSB's ZipfianGenerator): rank 0 is
// the most popular. For "latest" reads key = Last() - rank, so the newest key is hottest.
// zeta is computed once over the item count at workload start; YCSB grows it as inserts
// land, but a run adds only a few percent of keys, which leaves the distribution unchanged.
class ZipfRank
{
  public:
   ZipfRank(u64 n, double theta) : n_(n), theta_(theta)
   {
      for (u64 i = 1; i <= n_; i++) zetan_ += 1.0 / std::pow(double(i), theta_);
      alpha_ = 1.0 / (1.0 - theta_);
      const double zeta2 = 1.0 + 1.0 / std::pow(2.0, theta_);
      eta_ = (1.0 - std::pow(2.0 / double(n_), 1.0 - theta_)) / (1.0 - zeta2 / zetan_);
   }
   u64 rank(double u) const
   {
      const double uz = u * zetan_;
      u64 ret;
      if (uz < 1.0)
         ret = 1;
      else if (uz < 1.0 + std::pow(0.5, theta_))
         ret = 2;
      else
         ret = std::min<u64>(n_, 1 + u64(double(n_) * std::pow(eta_ * u - eta_ + 1.0, alpha_)));
      return ret - 1;
   }

  private:
   u64 n_;
   double theta_, zetan_ = 0, alpha_, eta_;
};
static double randUnit()  // uniform in [0, 1)
{
   return double(utils::RandomGenerator::getRandU64() >> 11) * 0x1.0p-53;
}
// Parse --ycsb_workloads ("name[:zipf[:read_ratio]],...") into a list. An empty spec
// yields a single workload using the legacy --zipf_factor / --ycsb_read_ratio flags.
static std::vector<YcsbWorkload> parseWorkloadList(const std::string& spec, double default_zipf, u32 default_rr)
{
   std::vector<YcsbWorkload> out;
   if (spec.empty()) {
      out.push_back({"run", default_zipf, default_rr});
      return out;
   }
   std::stringstream items(spec);
   std::string item;
   while (std::getline(items, item, ',')) {
      if (item.empty()) continue;
      YcsbWorkload w{item, default_zipf, default_rr};
      std::stringstream fields(item);
      std::string f;
      std::vector<std::string> parts;
      while (std::getline(fields, f, ':')) parts.push_back(f);
      if (parts.size() >= 1 && !parts[0].empty()) w.name = parts[0];
      if (parts.size() >= 2) w.zipf_factor = std::stod(parts[1]);
      if (parts.size() >= 3) w.read_ratio = static_cast<u32>(std::stoul(parts[2]));
      if (parts.size() >= 4) {
         ensure(parts[3] == "insert");
         w.insert = true;
      }
      out.push_back(w);
   }
   return out;
}
// Echo a profiling CSV to stdout so the run log captures it (the harness parser reads it).
static void dumpCsvFile(const std::string& path)
{
   std::ifstream f(path);
   std::stringstream buf;
   buf << f.rdbuf();
   std::cout << buf.str();
}
// -------------------------------------------------------------------------------------
void run_ycsb() {
   // -------------------------------------------------------------------------------------
   chrono::high_resolution_clock::time_point begin, end;
   // -------------------------------------------------------------------------------------
   // LeanStore DB
   LeanStore db;
   unique_ptr<BTreeInterface<YCSBKey, YCSBPayload>> adapter;
   leanstore::storage::btree::BTreeLL* btree_ptr = nullptr;
   mean::task::scheduleTaskSync([&]() {
         auto& vs_btree = db.registerBTreeLL("ycsb", "y");
         btree_ptr = &vs_btree;
         adapter.reset(new BTreeVSAdapter<YCSBKey, YCSBPayload>(vs_btree));
         db.registerConfigEntry("ycsb_read_ratio", FLAGS_ycsb_read_ratio);
         db.registerConfigEntry("ycsb_target_gib", FLAGS_target_gib);
   });
   // -------------------------------------------------------------------------------------
   auto& table = *adapter;
   const u64 ycsb_tuple_count = (FLAGS_ycsb_tuple_count)
                                    ? FLAGS_ycsb_tuple_count
                                    : FLAGS_target_gib * 1024 * 1024 * 1024 * 1.0 / 2.0 / (sizeof(YCSBKey) + sizeof(YCSBPayload));
   // Insert values
   {
      const u64 n = ycsb_tuple_count;
      cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Inserting values" << endl;
      begin = chrono::high_resolution_clock::now();

      mean::BlockedRange bb(0, (u64)n);
      ensure((bool)((bb.end - bb.begin) > 1));
      auto ycsb_insert_fun = [&](mean::BlockedRange bb, std::atomic<bool>&) {
         // vector<u64> keys(range.size());
         // std::iota(keys.begin(), keys.end(), range.begin());
         // std::random_shuffle(keys.begin(), keys.end());
         for (u64 t_i = bb.begin; t_i < bb.end; t_i++) {
            YCSBPayload payload;
            utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
            auto& key = t_i;
            table.insert(key, payload);
            YCSBPayload result; /// FIXME remove this check
            table.lookup(t_i, result);
            ensure(result == payload);

            mean::task::yield();
         }
      };
      mean::task::parallelFor(bb, ycsb_insert_fun, FLAGS_worker_tasks, 100000);
      end = chrono::high_resolution_clock::now();
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      // -------------------------------------------------------------------------------------
      const u64 written_pages = db.getBufferManager().consumedPages();
      const u64 mib = written_pages * PAGE_SIZE / 1024 / 1024;
      cout << "Inserted volume: (pages, MiB) = (" << written_pages << ", " << mib << ")" << endl;
      if (btree_ptr) {
         mean::task::scheduleTaskSync([&]() {
            // getHeight() is O(1) (a stored field) and is all we need for the worst-case
            // I/O bound (= height - 1, root always resident). countPages()/countInner()
            // walk the ENTIRE tree, faulting every evicted node back through the pool, so
            // out-of-cache they are pathologically slow and pollute the pool right before
            // the measured workload -- only do them when explicitly asked.
            cout << "btree_height=" << btree_ptr->getHeight();
            if (FLAGS_ycsb_print_tree_stats) {
               cout << " total_nodes=" << btree_ptr->countPages()
                    << " inner_nodes=" << btree_ptr->countInner();
            }
            cout << endl;
         });
      }
      cout << "-------------------------------------------------------------------------------------" << endl;
   }
   cout << setprecision(4);
   // Workloads to run back-to-back over the single filled dataset (see --ycsb_workloads).
   // Unmeasured phases are separate entries with their own names, so the harness only
   // ingests the measured ones (it maps workloads by name).
   std::vector<YcsbWorkload> workloads;
   if (FLAGS_ycsb_drain_after_fill) {
      YcsbWorkload drain{"drain_after_fill", 0.0, 100};
      drain.seconds = FLAGS_ycsb_drain_max_seconds;
      drain.drain = true;
      workloads.push_back(drain);
   }
   for (const auto& wl : parseWorkloadList(FLAGS_ycsb_workloads, FLAGS_zipf_factor, FLAGS_ycsb_read_ratio)) {
      if (FLAGS_ycsb_warmup_seconds) {
         YcsbWorkload warm = wl;
         warm.name = "warmup_" + wl.name;
         warm.seconds = FLAGS_ycsb_warmup_seconds;
         workloads.push_back(warm);
      }
      workloads.push_back(wl);
   }
   // -------------------------------------------------------------------------------------
   // Scan
   if (FLAGS_ycsb_scan) {
      const u64 n = ycsb_tuple_count;
      cout << "-------------------------------------------------------------------------------------" << endl;
      cout << "Scan" << endl;
      {
         begin = chrono::high_resolution_clock::now();
         mean::BlockedRange bb(0, (u64)n);
         ensure((bool)((bb.end - bb.begin) > 1));
         auto ycsb_fun = [&](mean::BlockedRange bb, std::atomic<bool>&) {
            for (u64 i = bb.begin; i < bb.end; i++) {
               YCSBPayload result;
               table.lookup(i, result);
               mean::task::yield();
            }
         };
         mean::task::parallelFor(bb, ycsb_fun, FLAGS_worker_tasks, 100000);
         end = chrono::high_resolution_clock::now();
      }
      // -------------------------------------------------------------------------------------
      cout << "time elapsed = " << (chrono::duration_cast<chrono::microseconds>(end - begin).count() / 1000000.0) << endl;
      // -------------------------------------------------------------------------------------
      cout << calculateMTPS(begin, end, n) << " M tps" << endl;
      cout << "-------------------------------------------------------------------------------------" << endl;
   }
   // -------------------------------------------------------------------------------------
   cout << "-------------------------------------------------------------------------------------" << endl;
   cout << "~Transactions" << endl;
   std::unique_ptr<AcknowledgedCounter> insert_keys;  // YCSB-D key counter, shared by all D workloads
   for (const auto& wl : workloads) {
      cout << "=== workload " << wl.name << " (zipf=" << wl.zipf_factor
           << ", read_ratio=" << wl.read_ratio << (wl.insert ? ", insert" : "") << ") ===" << endl;
      // Reset per-lookup I/O profiling for this workload
      g_max_lookup_ios.store(0);
      for (auto& h : g_lookup_io_hist) h.store(0);
      for (auto& s : g_op_stats) s.reset();
      // Fresh per-second profiling CSVs for this workload (csv_truncate rewrites them).
      db.startProfilingThread();
      // Wall clock at the profiling CSVs' t=0, for lining them up with sysmetrics (sar).
      const double wl_start_epoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
      std::unique_ptr<utils::ScrambledZipfGenerator> zipf_random;
      std::unique_ptr<ZipfRank> latest_rank;  // YCSB-D with zipf_factor > 0
      if (wl.insert) {
         if (!insert_keys) insert_keys = std::make_unique<AcknowledgedCounter>(ycsb_tuple_count);
         if (wl.zipf_factor > 0) latest_rank = std::make_unique<ZipfRank>(insert_keys->last() + 1, wl.zipf_factor);
      } else {
         zipf_random = std::make_unique<utils::ScrambledZipfGenerator>(0, ycsb_tuple_count, wl.zipf_factor);
      }
      std::atomic<u64> not_found{0};
      const u32 read_ratio = wl.read_ratio;
      atomic<bool> keep_running = true;
      atomic<u64> running_threads_counter = 0;
      const u64 wl_seconds = wl.seconds ? wl.seconds : FLAGS_run_for_seconds;
      // Drain: once a second, compare pages written back with pages read over the last
      // 10 s; stop when the writes (all of them the fill's, as this phase only reads)
      // are a negligible share of the I/O.
      std::atomic<bool> drained{false};
      std::thread drain_monitor;
      if (wl.drain) {
         drain_monitor = std::thread([&]() {
            std::vector<std::pair<u64, u64>> hist;  // (written, read) totals, one per second
            for (u64 t = 0; !drained.load(); t++) {
               hist.emplace_back(PPCounters::total_written_pages.load(), PPCounters::total_read_pages.load());
               if (t >= 10) {
                  const u64 w = hist[t].first - hist[t - 10].first;
                  const u64 r = hist[t].second - hist[t - 10].second;
                  std::ostringstream line;
                  line.imbue(std::locale::classic());
                  line << std::fixed << std::setprecision(2) << "drain t=" << t << " last10s written_pages=" << w << " read_pages=" << r
                       << " write_pct=" << (r ? 100.0 * w / r : 0.0) << "\n";
                  std::cerr << line.str() << std::flush;
                  if (t >= 30 && r > 0 && 100.0 * w < FLAGS_ycsb_drain_write_pct * r) {
                     std::cerr << "drain settled after " << t << " s\n" << std::flush;
                     drained = true;
                     break;
                  }
               }
               if (t >= wl_seconds) {
                  std::cerr << "drain hit --ycsb_drain_max_seconds without settling\n" << std::flush;
                  break;
               }
               std::this_thread::sleep_for(std::chrono::seconds(1));
            }
         });
      }
      {
      auto start = mean::getSeconds();
      auto ycsb_tx = [&](mean::BlockedRange bb, std::atomic<bool>& cancelled){

       thread_local auto nextStartTime = mean::readTSC();
       thread_local u64 longLat = 0;
       auto tx_start_time = nextStartTime;
       const float rate = FLAGS_tx_rate / mean::env::workerCount();
       std::random_device rd;
       std::mt19937 gen(rd());
       std::exponential_distribution<> expDist(rate);
       volatile u64 i = bb.begin;

         running_threads_counter++;
         int timeCheck = 0;
         while (i < bb.end && keep_running) {
            auto before = mean::readTSC();
            timeCheck++;
            if (timeCheck % 32 == 0 && (mean::getSeconds() - start > wl_seconds || drained.load(std::memory_order_relaxed))) {
               cancelled = true;
               break;
            }
            YCSBPayload result;
            const bool is_read = (read_ratio == 100 || utils::RandomGenerator::getRandU64(0, 100) < read_ratio);
            YCSBKey key;
            if (!wl.insert) {
               key = zipf_random->rand();
               assert(key < ycsb_tuple_count);
            } else if (is_read) {
               const u64 last = insert_keys->last();
               key = latest_rank ? last - std::min(last, latest_rank->rank(randUnit()))
                                 : utils::RandomGenerator::getRandU64(0, last + 1);
            } else {
               key = insert_keys->next();
            }
            // Reset the coroutine-local per-op counters, run the op, read them back. These
            // live on the Task so they are correct across worker-thread migration at yields.
            mean::task::this_task().io_count = 0;
            mean::task::this_task().io_wait_ns = 0;
            mean::task::this_task().restart_count = 0;
            mean::task::this_task().leaf_contention = 0;
            const auto op_before = mean::readTSC();
            if (is_read) {
               if (!table.lookup(key, result)) not_found++;
            } else {
               YCSBPayload payload;
               utils::RandomGenerator::getRandString(reinterpret_cast<u8*>(&payload), sizeof(YCSBPayload));
               if (wl.insert) {
                  table.insert(key, payload);
                  insert_keys->acknowledge(key);
               } else {
                  table.update(key, payload);
               }
            }
            const u64 op_ios = mean::task::this_task().io_count;
            const u64 op_us = mean::tscDifferenceUs(mean::readTSC(), op_before);
            const u64 op_io_wait_us = mean::task::this_task().io_wait_ns / 1000;
            const u64 op_restarts = mean::task::this_task().restart_count;
            const u64 op_leaf_cont = mean::task::this_task().leaf_contention;
            if (is_read) recordLookupIos(op_ios);  // reads-only I/O histogram (LOOKUP_IO table)
            recordOpLatency(is_read, op_ios, op_us, op_io_wait_us, op_restarts, op_leaf_cont);
           i++;
           auto now = mean::readTSC();
           auto timeDiff = mean::tscDifferenceUs(now, before);
           auto timeDiffIncWait = mean::tscDifferenceUs(now, tx_start_time);
           WorkerCounters::myCounters().total_tx_time += timeDiff;
           WorkerCounters::myCounters().tx_latency_hist.increaseSlot(timeDiff);
           if (timeDiffIncWait < 10000000) {
              WorkerCounters::myCounters().total_tx_time_inc_wait += timeDiffIncWait;
           }
           WorkerCounters::myCounters().tx_latency_hist_incwait.increaseSlot(timeDiffIncWait);
           WorkerCounters::myCounters().tx++;
           ThreadCounters::myCounters().tx++;
           while (i < bb.end && keep_running) {
              mean::task::yield();
              now = mean::readTSC();
              if (rate == 0) break;
              if (now >= nextStartTime) {
                 if (mean::tscDifferenceS(now, nextStartTime) > 5) {
                     longLat++;
                     nextStartTime = now;
                     std::cout << "reset start time" << std::endl;
                     if (longLat % 100000 == 0) {
                        //std::cout << "thr: " << mean::exec::getId() << " long latency: " << longLat << std::endl;
                    }
                 }
                 auto d = expDist(gen);
                 tx_start_time = nextStartTime;
                  nextStartTime += mean::nsToTSC(d*1e9);
                  //std::cout << "next: " << nextStartTime << std::flush << std::endl;
                  break;
              }
           }
         }
         running_threads_counter--;
      };
      mean::BlockedRange bb(0, (u64)1000000000000ul);
      auto startTsc = mean::readTSC();
      auto startTP = mean::getTimePoint();
      mean::task::parallelFor(bb, ycsb_tx, FLAGS_worker_tasks, 100000);
      auto diffTSC = mean::tscDifferenceNs(mean::readTSC(), startTsc) / 1e9;
      auto diffTP = mean::timePointDifference(mean::getTimePoint(), startTP) / 1e9;
      std::cout << "done: time: " << diffTP << " tsc: " << diffTSC << std::endl;
      if (drain_monitor.joinable()) {
         drained = true;
         drain_monitor.join();
      }
      std::cout << "not_found=" << not_found.load();
      if (insert_keys) std::cout << " inserted_up_to=" << insert_keys->last();
      std::cout << std::endl;
      {
         // Machine-readable workload window (epoch seconds) + the data file's size at its end.
         const double wl_end_epoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
         struct stat st {};
         const bool have_st = ::stat(FLAGS_ssd_path.c_str(), &st) == 0;
         std::ostringstream line;
         line.imbue(std::locale::classic());
         line << std::fixed << std::setprecision(3) << "bk_window " << wl.name << " start=" << wl_start_epoch << " end=" << wl_end_epoch
              << " apparent_bytes=" << (have_st ? (u64)st.st_size : 0) << " allocated_bytes=" << (have_st ? (u64)st.st_blocks * 512 : 0);
         std::cout << line.str() << std::endl;
      }
      }
      // Stop this workload's profiling, then emit its CSVs tagged with the workload name
      // so the harness parser can attribute rows to the right distribution/read ratio.
      db.stopProfilingThread();
      std::cout << "===LEANSTORE_WL " << wl.name << "===" << std::endl;
      std::cout << "===LEANSTORE_CR===" << std::endl;
      dumpCsvFile(FLAGS_csv_path + "_cr.csv");
      std::cout << "===LEANSTORE_BM===" << std::endl;
      dumpCsvFile(FLAGS_csv_path + "_bm.csv");
      // Per-lookup I/O distribution (worst case = max row).
      u64 total_lookups = 0, total_ios = 0;
      for (size_t b = 0; b < g_lookup_io_hist.size(); b++) {
         const u64 c = g_lookup_io_hist[b].load();
         total_lookups += c;
         total_ios += c * b;
      }
      const double avg_ios = total_lookups ? (1.0 * total_ios / total_lookups) : 0.0;
      std::cout << "===LEANSTORE_LOOKUP_IO===" << std::endl;
      printLookupIoTable(std::cout);
      // Machine-readable summary line for scripting across machines.
      std::cout << "measured_lookups=" << total_lookups
                << " total_ios=" << total_ios
                << " max_ios_per_lookup=" << g_max_lookup_ios.load()
                << " avg_ios_per_lookup=" << avg_ios << std::endl;
      // Also drop the table + summary into its own file so it is trivial to find even when
      // stdout is redirected (block-buffered) and the tail is lost at shutdown.
      {
         const std::string io_path = FLAGS_csv_path + "_lookup_io_" + wl.name + ".txt";
         std::ofstream iof(io_path);
         printLookupIoTable(iof);
         iof << "measured_lookups=" << total_lookups
             << " total_ios=" << total_ios
             << " max_ios_per_lookup=" << g_max_lookup_ios.load()
             << " avg_ios_per_lookup=" << avg_ios << "\n";
         std::cout << "lookup I/O table written to " << io_path << std::endl;
      }
      // Per-op latency breakdown (read/update x cached/miss): where the latency comes from.
      std::cout << "===LEANSTORE_OP_LATENCY===" << std::endl;
      printLatencyTable(std::cout);
      {
         const std::string lat_path = FLAGS_csv_path + "_latency_" + wl.name + ".txt";
         std::ofstream latf(lat_path);
         printLatencyTable(latf);
         std::cout << "op latency breakdown written to " << lat_path << std::endl;
      }
      std::cout.flush();
   }
   mean::env::shutdown();
   cout << "-------------------------------------------------------------------------------------" << endl;
   // -------------------------------------------------------------------------------------
}
// -------------------------------------------------------------------------------------
int main(int argc, char** argv)
{
   gflags::SetUsageMessage("Leanstore Frontend");
   gflags::ParseCommandLineFlags(&argc, &argv, true);
   // -------------------------------------------------------------------------------------
   // -------------------------------------------------------------------------------------
   using namespace mean;
   IoOptions ioOptions("auto", FLAGS_ssd_path);
   ioOptions.write_back_buffer_size = PAGE_SIZE;
   ioOptions.engine = FLAGS_ioengine;
   ioOptions.ioUringPollMode = FLAGS_io_uring_poll_mode;
   ioOptions.ioUringShareWq = FLAGS_io_uring_share_wq;
   ioOptions.raid5 = FLAGS_raid5;
   ioOptions.iodepth = (FLAGS_async_batch_size + FLAGS_worker_tasks)*2; // hacky, how to take into account for remotes 
   // -------------------------------------------------------------------------------------
   if (FLAGS_nopp) {
      ioOptions.channelCount = FLAGS_worker_threads;
      mean::env::init(
         FLAGS_worker_threads, //std::min(std::thread::hardware_concurrency(), FLAGS_tpcc_warehouse_count),
         0/*FLAGS_pp_threads*/, ioOptions);
   } else {
      ioOptions.channelCount = FLAGS_worker_threads + FLAGS_pp_threads;
      mean::env::init(FLAGS_worker_threads, FLAGS_pp_threads, ioOptions);
   }
   mean::env::start(run_ycsb);
   // -------------------------------------------------------------------------------------
   mean::env::join();
   // -------------------------------------------------------------------------------------
   return 0;
}
