/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_PROCESSOR_H_
#define XENIA_CPU_PROCESSOR_H_

#include <atomic>
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include <condition_variable>
#endif
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "xenia/base/cvar.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/backend/backend.h"
#include "xenia/cpu/debug_listener.h"
#include "xenia/cpu/entry_table.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/cpu/function.h"
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_execution_capture.h"
#endif
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/thread_debug_info.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/memory.h"

DECLARE_bool(debug);

namespace xe {
namespace cpu {

constexpr fourcc_t kProcessorSaveSignature = make_fourcc("PROC");

class Breakpoint;
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
class GuestExecutionCaptureRegistryTestAccess;
class GuestInvocationCaptureEventSink;
#endif
class JitCorpusWriter;
class StackWalker;
class XexModule;

enum class Irql : uint32_t {
  PASSIVE = 0,
  APC = 1,
  DISPATCH = 2,
  DPC = 3,
};

// Describes the current state of the emulator as known to the debugger.
// This determines which state the debugger is in and what operations are
// allowed.
enum class ExecutionState {
  // Target is running; the debugger is not waiting for any events.
  kRunning,
  // Target is running in stepping mode with the debugger waiting for feedback.
  kStepping,
  // Target is paused for debugging.
  kPaused,
  // Target has been stopped and cannot be restarted (crash, etc).
  kEnded,
};

class Processor {
 public:
  Processor(Memory* memory, ExportResolver* export_resolver);
  ~Processor();

  Memory* memory() const { return memory_; }
  StackWalker* stack_walker() const { return stack_walker_.get(); }
  ppc::PPCFrontend* frontend() const { return frontend_.get(); }
  backend::Backend* backend() const { return backend_.get(); }
  ExportResolver* export_resolver() const { return export_resolver_; }

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // The owner must install this before guest translation starts and clear it
  // only after capture callbacks have stopped. The sink serializes and routes
  // all recorder access; Processor does not own it.
  void set_guest_invocation_capture_sink(
      GuestInvocationCaptureEventSink* sink) {
    guest_invocation_capture_sink_ = sink;
  }
  GuestInvocationCaptureEventSink* guest_invocation_capture_sink() const {
    return guest_invocation_capture_sink_;
  }

  // Orthogonal to the one-invocation event sink above. Continuous capture must
  // install one observer before title dispatch, then arm and disarm internally.
  // Once any generic host-to-guest dispatch begins, the installed observer is
  // Processor-lifetime permanent and later attachment or detachment is
  // rejected so no outer call can be omitted. Registration is shared-owned;
  // Processor releases it after guest execution is torn down.
  bool AttachGuestExecutionCaptureHostCallObserver(
      std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer);
  bool DetachGuestExecutionCaptureHostCallObserver(
      const std::shared_ptr<GuestExecutionCaptureHostCallObserver>& observer);
  GuestExecutionCaptureHostCallToken BeginGuestExecutionCaptureHostCall(
      const ThreadState& thread_state, const GuestFunction& function,
      uint32_t return_address);
  bool EndGuestExecutionCaptureHostCall(
      GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
      const GuestFunction& function,
      GuestExecutionCaptureHostCallOutcome outcome);

  // Value-only snapshot of all currently live capture ThreadState identities,
  // publication states and the sticky registry rejection. No ThreadState or
  // host-context pointer escapes the lifetime registry lock.
  GuestExecutionCaptureThreadStateRegistrySnapshot
  QueryGuestExecutionCaptureParticipants() const;

  // Visits live ThreadState objects while destruction is excluded by the
  // capture-only lifetime registry lock. The visitor must not retain a
  // ThreadState reference or call back into Processor.
  GuestExecutionCaptureThreadStateVisitResult
  VisitGuestExecutionCaptureThreadStates(
      GuestExecutionCaptureThreadStateVisitor& visitor) const noexcept;
#endif

  bool Setup(std::unique_ptr<backend::Backend> backend);

  // Runs any pre-launch logic once the module and thread have been setup.
  void PreLaunch();

  // The current execution state of the emulator.
  ExecutionState execution_state() const { return execution_state_; }

  // True if a debug listener is attached and the debugger is active.
  bool is_debugger_attached() const { return !!debug_listener_; }
  // Gets the active debug listener, if any.
  DebugListener* debug_listener() const { return debug_listener_; }
  // Sets the active debug listener, if any.
  // This can be used to detach the listener.
  void set_debug_listener(DebugListener* debug_listener);
  // Sets a handler that will be called from a random thread when a debugger
  // listener is required (such as on a breakpoint hit/etc).
  // Will only be called if the debug listener has not already been specified
  // with set_debug_listener.
  void set_debug_listener_request_handler(
      std::function<DebugListener*(Processor*)> handler) {
    debug_listener_handler_ = std::move(handler);
  }

  void set_debug_info_flags(uint32_t debug_info_flags) {
    debug_info_flags_ = debug_info_flags;
  }

  bool AddModule(std::unique_ptr<Module> module);
  void RemoveModule(const std::string_view name);
  Module* GetModule(const std::string_view name);
  std::vector<Module*> GetModules();

  Module* builtin_module() const { return builtin_module_; }
  Function* DefineBuiltin(const std::string_view name,
                          BuiltinFunction::Handler handler, void* arg0,
                          void* arg1);

  Function* QueryFunction(uint32_t address);
  std::vector<Function*> FindFunctionsWithAddress(uint32_t address);
  void RemoveFunctionByAddress(uint32_t address);

  Function* LookupFunction(uint32_t address);
  Module* LookupModule(uint32_t address);
  Function* LookupFunction(Module* module, uint32_t address);
  Function* ResolveFunction(uint32_t address);

  bool Execute(ThreadState* thread_state, uint32_t address);
  bool ExecuteRaw(ThreadState* thread_state, uint32_t address);
  uint64_t Execute(ThreadState* thread_state, uint32_t address, uint64_t args[],
                   size_t arg_count);

  bool Save(ByteStream* stream);
  bool Restore(ByteStream* stream);

  // Returns a list of debugger info for all threads that have ever existed.
  // This is the preferred way to sample thread state vs. attempting to ask
  // the kernel.
  std::vector<ThreadDebugInfo*> QueryThreadDebugInfos();

  // Returns the debugger info for the given thread.
  ThreadDebugInfo* QueryThreadDebugInfo(uint32_t thread_id);

  // Adds a breakpoint to the debugger and activates it (if enabled).
  // The given breakpoint will not be owned by the debugger and must remain
  // allocated so long as it is added.
  void AddBreakpoint(Breakpoint* breakpoint);

  // Removes a breakpoint from the debugger and deactivates it.
  void RemoveBreakpoint(Breakpoint* breakpoint);

  // Finds a breakpoint that may be registered at the given address.
  Breakpoint* FindBreakpoint(uint32_t address);

  // Returns all currently registered breakpoints.
  std::vector<Breakpoint*> breakpoints() const;

  // Shows the debug listener, focusing it if it already exists.
  void ShowDebugger();

  // Pauses target execution by suspending all threads.
  // The debug listener will be requested if it has not been attached.
  void Pause();

  // Continues target execution from wherever it is.
  // This will cancel any active step operations and resume all threads.
  void Continue();

  // Steps the given thread a single x64 host instruction.
  // If the step is over a branch the branch will be followed.
  void StepHostInstruction(uint32_t thread_id);

  // Steps the given thread a single PPC guest instruction.
  // If the step is over a branch the branch will be followed.
  void StepGuestInstruction(uint32_t thread_id);

  // Steps the given thread until the guest address is hit.
  // Returns false if the step could not be completed (invalid target address).
  bool StepToGuestAddress(uint32_t thread_id, uint32_t pc);

  // Steps the given thread to the target of the branch at the specified guest
  // address. The address must specify a branch instruction.
  // Returns the new PC guest address.
  uint32_t StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc);

  // Steps the thread to a point where it's safe to terminate or read its
  // context. Returns the PC after we've finished stepping.
  // Pass true for ignore_host if you've stopped the thread yourself
  // in host code you want to ignore.
  // Returns the new PC guest address.
  uint32_t StepToGuestSafePoint(uint32_t thread_id, bool ignore_host = false);

  uint32_t GuestAtomicIncrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicDecrement32(ppc::PPCContext* context,
                                  uint32_t guest_address);
  uint32_t GuestAtomicOr32(ppc::PPCContext* context, uint32_t guest_address,
                           uint32_t mask);
  uint32_t GuestAtomicXor32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  uint32_t GuestAtomicAnd32(ppc::PPCContext* context, uint32_t guest_address,
                            uint32_t mask);
  bool GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                        uint32_t new_value, uint32_t guest_address);

 public:
  // TODO(benvanik): hide.
  void OnThreadCreated(uint32_t handle, ThreadState* thread_state,
                       Thread* thread);
  void OnThreadExit(uint32_t thread_id);
  void OnThreadDestroyed(uint32_t thread_id);
  void OnThreadEnteringWait(uint32_t thread_id);
  void OnThreadLeavingWait(uint32_t thread_id);

  bool OnUnhandledException(Exception* ex);
  bool OnThreadBreakpointHit(Exception* ex);

  // Instruction coverage counters live in a per-thread arena so the JIT can
  // increment them unsynchronized. Every arena shares one layout: a function
  // reserves an offset here, and each thread holds a private copy at that
  // offset. Returns GuestFunction::kInvalidCoverageOffset if the arena is
  // full.
  size_t AllocateTraceCountsOffset(uint32_t start_address,
                                   uint32_t instruction_count);
  uint8_t* AcquireTraceCounts(uint32_t thread_id);
  void ReleaseTraceCounts(uint8_t* arena);
  // Starts a fresh capture window. Racing threads may land a count on either
  // side of this, which does not matter at the scale being measured.
  void ResetTraceCounts();
  // Call once a title's config is applied and before its code is translated.
  void RefreshTraceCountsEnabled();
  // Handed over by the backend once a function is emitted, so the sequences it
  // selected can be weighted by how often each guest instruction runs.
  void RecordSequenceSamples(uint32_t start_address,
                             std::vector<backend::SequenceSample> samples);
  bool trace_counts_enabled() const { return trace_counts_enabled_; }
  // Write the coverage tables to --trace_function_coverage_out, if set.
  void DumpTraceCountsToFile();

 private:
  friend class ThreadState;

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  friend class GuestExecutionCaptureRegistryTestAccess;
  friend GuestExecutionCaptureJitSafepointResult
  HandleGuestExecutionCaptureJitSafepoint(void* raw_context,
                                          uint64_t guest_address) noexcept;

  void RegisterGuestExecutionCaptureThreadState(
      ThreadState& thread_state) noexcept;
  GuestExecutionCaptureThreadStateLifecycleDisposition
  PublishGuestExecutionCaptureThreadStateReady(
      ThreadState& thread_state) noexcept;
  void BeginGuestExecutionCaptureThreadStateDestruction(
      ThreadState& thread_state) noexcept;
  void CompleteGuestExecutionCaptureThreadStateDestruction(
      ThreadState& thread_state) noexcept;
  std::shared_ptr<GuestExecutionCaptureHostCallObserver>
  AcquireGuestExecutionCaptureObserverDispatch(bool host_call_begin);
  void ReleaseGuestExecutionCaptureObserverDispatch() noexcept;

  // Delivers a consumed per-context request to the permanent observer. The
  // callback may park; ThreadState destruction waits for only this lifetime's
  // outstanding callbacks while other participants remain able to arrive.
  GuestExecutionCaptureJitSafepointResult
  DeliverGuestExecutionCaptureJitSafepoint(ThreadState& thread_state,
                                           uint32_t guest_address) noexcept;
#endif

  // Write the guestcoverage, guestcoveragethreads and guestsequences tables
  // appended to the profiler's CSV dump. Both require trace_counts_mutex_.
  void DumpTraceCounts(FILE* f);
  void DumpSequences(FILE* f);

  // All require trace_counts_mutex_ held.
  uint8_t* ReserveTraceCountsArenaLocked(uint32_t thread_id);
  void SetTraceCountsArenaThreadLocked(uint8_t* arena, uint32_t thread_id);
  bool EnsureTraceCountsFallbackLocked();
  bool CommitTraceCountsLocked(size_t required);
  void FoldTraceCountsLocked(uint8_t* arena);

  // Synchronously demands a debug listener.
  void DemandDebugListener();

  // Suspends all known threads (except the caller).
  bool SuspendAllThreads();
  // Resumes the given thread.
  bool ResumeThread(uint32_t thread_id);
  // Resumes all known threads (except the caller).
  bool ResumeAllThreads();
  // Updates all cached thread execution info (state, call stacks, etc).
  // The given override thread handle and context will be used in place of
  // sampled values for that thread.
  void UpdateThreadExecutionStates(
      uint32_t override_handle = 0,
      HostThreadContext* override_context = nullptr);

  // Suspends all breakpoints, uninstalling them as required.
  // No breakpoints will be triggered until they are resumed.
  void SuspendAllBreakpoints();
  // Resumes all breakpoints, re-installing them if required.
  void ResumeAllBreakpoints();

  void OnFunctionDefined(Function* function);

  static bool ExceptionCallbackThunk(Exception* ex, void* data);
  bool ExceptionCallback(Exception* ex);
  void OnStepCompleted(ThreadDebugInfo* thread_info);
  void OnBreakpointHit(ThreadDebugInfo* thread_info, Breakpoint* breakpoint);

  // Calculates the next guest instruction based on the current thread state and
  // current PC. This will look for branches and other control flow
  // instructions.
  uint32_t CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                         uint32_t current_pc);

  bool DemandFunction(Function* function);

  Memory* memory_ = nullptr;
  std::unique_ptr<StackWalker> stack_walker_;

  std::function<DebugListener*(Processor*)> debug_listener_handler_;
  DebugListener* debug_listener_ = nullptr;

  // Which debug features are enabled in generated code.
  uint32_t debug_info_flags_ = 0;
  struct TraceCountsRegion {
    uint32_t start_address;
    size_t offset;
    size_t count;
    // Counts from threads that have exited, folded in as they go.
    std::unique_ptr<uint64_t[]> retired;
    // What the backend emitted for this function, empty until it is compiled.
    std::vector<backend::SequenceSample> samples;
  };
  struct TraceCountsArena {
    uint8_t* base;
    // Zero once the owning thread exits, or for the shared fallback.
    uint32_t thread_id;
  };
  uintptr_t trace_counts_dump_section_ = 0;
  std::mutex trace_counts_mutex_;
  std::vector<TraceCountsRegion> trace_counts_regions_;
  // Every arena ever reserved, and the subset free for a new thread to claim.
  std::vector<TraceCountsArena> trace_counts_arenas_;
  std::vector<uint8_t*> trace_counts_free_;
  // Shared by any thread that could not get a private arena.
  uint8_t* trace_counts_fallback_ = nullptr;
  bool trace_counts_failed_ = false;
  bool trace_counts_enabled_ = false;
  bool trace_counts_periodic_started_ = false;
  size_t trace_counts_next_offset_ = 0;
  size_t trace_counts_committed_ = 0;

  // If --jit_corpus_out was given, every compiled function is streamed here for
  // later offline codegen replay.
  std::unique_ptr<JitCorpusWriter> jit_corpus_writer_;

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  GuestInvocationCaptureEventSink* guest_invocation_capture_sink_ = nullptr;

  // Intrusive and allocation-free so every successfully constructed
  // ThreadState is registered. Always acquire this before the observer mutex
  // when both are needed. Lifecycle callbacks run while this is held.
  mutable std::mutex guest_execution_capture_thread_state_mutex_;
  std::condition_variable guest_execution_capture_thread_state_condition_;
  ThreadState* guest_execution_capture_thread_state_head_ = nullptr;
  GuestExecutionCaptureThreadStateRegistryRejection
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::kNone;
  // Non-owning deterministic test rendezvous at the exact point where a
  // ThreadState destructor is about to acquire the lifetime registry gate.
  // Tests configure and clear this only while no destruction is in flight.
  std::atomic<bool>*
      guest_execution_capture_thread_state_destruction_gate_test_signal_ =
          nullptr;

  // Protects observer registration and short callback acquisition leases. It
  // is never held while invoking the observer, which may later rendezvous and
  // block guest execution in a dedicated capture build.
  std::mutex guest_execution_capture_host_call_observer_mutex_;
  std::shared_ptr<GuestExecutionCaptureHostCallObserver>
      guest_execution_capture_host_call_observer_;
  uint64_t guest_execution_capture_host_call_dispatch_count_ = 0;
  uint64_t guest_execution_capture_host_call_dispatch_epoch_ = 0;
  bool guest_execution_capture_host_call_dispatch_seen_ = false;
  bool guest_execution_capture_host_call_observer_transition_pending_ = false;
#endif

  // Opt-in capture ordering lock. Recursive because defining a guest function
  // may synchronously demand another function on the same host thread.
  std::recursive_mutex guest_function_definition_mutex_;

  std::unique_ptr<ppc::PPCFrontend> frontend_;
  std::unique_ptr<backend::Backend> backend_;
  ExportResolver* export_resolver_ = nullptr;

  EntryTable entry_table_;
  xe::global_critical_region global_critical_region_;
  ExecutionState execution_state_ = ExecutionState::kPaused;
  std::vector<std::unique_ptr<Module>> modules_;
  Module* builtin_module_ = nullptr;
  uint32_t next_builtin_address_ = 0xFFFF0000u;

  // Maps thread ID to state. Updated on thread create, and threads are never
  // removed. Must be guarded with the global lock.
  std::map<uint32_t, std::unique_ptr<ThreadDebugInfo>> thread_debug_infos_;

  // TODO(benvanik): cleanup/change structures.
  std::vector<Breakpoint*> breakpoints_;

  Irql irql_;
};

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
// Direct target of the backend guest-to-host thunk. It validates context
// ownership, atomically consumes one request and forwards to Processor.
GuestExecutionCaptureJitSafepointResult HandleGuestExecutionCaptureJitSafepoint(
    void* raw_context, uint64_t guest_address) noexcept;
#endif

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_PROCESSOR_H_
