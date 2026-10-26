#include "PPCounters.hpp"
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
namespace leanstore
{
tbb::enumerable_thread_specific<PPCounters> PPCounters::pp_counters;
std::atomic<u64> PPCounters::total_written_pages{0};
std::atomic<u64> PPCounters::total_read_pages{0};
}
