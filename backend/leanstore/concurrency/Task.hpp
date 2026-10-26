#pragma once
// -------------------------------------------------------------------------------------
#include "MessageHandler.hpp"
#include "leanstore/io/IoAbstraction.hpp"
#include "leanstore/sync-primitives/JumpMU.hpp"
// -------------------------------------------------------------------------------------
#include "boost/context/continuation.hpp"
#include "boost/context/continuation_fcontext.hpp"
// -------------------------------------------------------------------------------------
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <queue>
// -------------------------------------------------------------------------------------
namespace mean
{
// -------------------------------------------------------------------------------------
class Task;
class TaskExecutor;
enum class TaskState {
   New,
   Ready, // general yield, basically push back in queue
   ReadyMem, // ready, but waiting for mem
   ReadyLock, // ready, but waiting for lock
   ReadyJumpLock,
   Waiting, // general waiting, requires manual push ready
   WaitIo,
   Done,
};
// -------------------------------------------------------------------------------------
// -------------------------------------------------------------------------------------
using TaskFunction = std::function<void()>;  // std::add_pointer_t<void()>;
using StacklessFunction = std::function<void()>;
// -------------------------------------------------------------------------------------
struct TaskContext {
   bool init = false;
   boost::context::continuation this_task_context;
   boost::context::continuation* sink_process_context;
   s64 worker_id = -1;
   bool wait = false;
};
// -------------------------------------------------------------------------------------
class Task
{
   TaskContext context;
   jumpmu::JumpMUContext jumpctx;
   friend TaskExecutor;
   TaskFunction fun;
   TaskState state = TaskState::Ready;
  public:
   YieldLock* lock;
   Task(TaskFunction fun) : fun(fun) {}
   ~Task();
   void* userContext;
   // -------------------------------------------------------------------------------------
   // Per-task (coroutine-local) profiling counters for a single DB operation. They live on
   // the Task so they stay correct across worker-thread migration at yield points (unlike a
   // thread-local). io_count = page reads this op incurred; io_wait_ns = wall-clock ns this
   // op spent blocked waiting for those reads. The ycsb frontend resets them before each op.
   unsigned long io_count = 0;
   unsigned long io_wait_ns = 0;
   // restart_count = optimistic-latch re-traversals (jumpmu restarts) this op incurred;
   // leaf_contention = times the leaf guard had to fall back because the leaf was
   // exclusively latched (direct read/write contention on the leaf). ycsb resets per op.
   unsigned long restart_count = 0;
   unsigned long leaf_contention = 0;
   // -------------------------------------------------------------------------------------
   long dbg = 0;
   long dbgAddr = 0;
   // -------------------------------------------------------------------------------------
   TaskState getState();
};
// -------------------------------------------------------------------------------------
}  // namespace mean
// -------------------------------------------------------------------------------------
