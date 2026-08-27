/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/processor.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>
#include <vector>

#include "xenia/base/assert.h"
#include "xenia/base/atomic.h"
#include "xenia/base/byte_order.h"
#include "xenia/base/byte_stream.h"
#include "xenia/base/cvar.h"
#include "xenia/base/debugging.h"
#include "xenia/base/exception_handler.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/base/profiling.h"
#include "xenia/base/threading.h"
#include "xenia/cpu/breakpoint.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/export_resolver.h"
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_invocation_capture.h"
#endif
#include "xenia/cpu/jit_corpus.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/ppc/ppc_decode_data.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/stack_walker.h"
#include "xenia/cpu/thread.h"
#include "xenia/cpu/thread_state.h"
#include "xenia/cpu/xex_module.h"

// TODO(benvanik): based on compiler support
#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif

#if 0 && DEBUG
#define DEFAULT_DEBUG_FLAG true
#else
#define DEFAULT_DEBUG_FLAG false
#endif

DEFINE_bool(debug, DEFAULT_DEBUG_FLAG,
            "Allow debugging and retain debug information.", "General");
DEFINE_path(
    jit_corpus_out, "",
    "Capture the guest code the JIT compiles to this file, for offline codegen "
    "replay by xenia-cpu-ppc-tests --jit_corpus_in. Contains copyrighted guest "
    "code; keep it out of version control.",
    "CPU");
DEFINE_path(
    trace_function_coverage_out, "",
    "Write the --trace_function_coverage tables to this file when the emulator "
    "shuts down. Without it the counters are only reachable through the "
    "profiler's CSV dump, and a profiler build cannot survive a real title: "
    "every guest function mints a MicroProfile token and "
    "MICROPROFILE_MAX_TIMERS "
    "is 1024, after which the token is invalid and its 0xFFFF timer index is "
    "written past the end of the timer arrays.",
    "CPU");
DEFINE_int32(
    trace_function_coverage_period, 0,
    "Rewrite --trace_function_coverage_out every this many seconds while the "
    "title runs, so a scripted run does not depend on someone quitting the "
    "emulator by hand. Written to a sibling file and renamed, so a reader "
    "never sees a partial table. 0 dumps only at shutdown.",
    "CPU");
DEFINE_bool(break_on_start, false, "Break into the debugger on startup.",
            "CPU");
DEFINE_bool(
    serialize_guest_function_definitions, false,
    "Serialize guest function definition and publication for deterministic "
    "JIT capture. Set before guest translation starts and do not change while "
    "definitions are in flight.",
    "CPU");

namespace xe {
namespace kernel {
class XThread;
}  // namespace kernel

namespace cpu {

using xe::cpu::ppc::PPCOpcode;
using xe::kernel::XThread;

using namespace xe::literals;

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
namespace {

thread_local bool guest_execution_capture_callback_active = false;
thread_local uint32_t guest_invocation_capture_sink_callback_depth = 0;

uint32_t NextGuestInvocationCaptureGeneration(uint32_t generation) {
  if (generation == kGuestInvocationCaptureControlGenerationMask) {
    std::abort();
  }
  return generation + 1;
}

class GuestExecutionCaptureCallbackScope final {
 public:
  GuestExecutionCaptureCallbackScope() {
    if (guest_execution_capture_callback_active) {
      std::abort();
    }
    guest_execution_capture_callback_active = true;
  }
  ~GuestExecutionCaptureCallbackScope() {
    guest_execution_capture_callback_active = false;
  }

  GuestExecutionCaptureCallbackScope(
      const GuestExecutionCaptureCallbackScope&) = delete;
  GuestExecutionCaptureCallbackScope& operator=(
      const GuestExecutionCaptureCallbackScope&) = delete;
};

GuestExecutionCaptureThreadStateLifecycleEvent MakeThreadStateLifecycleEvent(
    const ThreadState& thread_state,
    GuestExecutionCaptureThreadStateLifecycleState state) {
  return {
      {
          thread_state.guest_execution_capture_instance_id(),
          thread_state.thread_id(),
      },
      state,
  };
}

}  // namespace
#endif

class BuiltinModule : public Module {
 public:
  explicit BuiltinModule(Processor* processor)
      : Module(processor), name_("builtin") {}

  const std::string& name() const override { return name_; }
  bool is_executable() const override { return false; }

  bool ContainsAddress(uint32_t address) override {
    return (address & 0xFFFFFFF0) == 0xFFFFFFF0;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(new BuiltinFunction(this, address));
  }

 private:
  std::string name_;
};

Processor::Processor(xe::Memory* memory, ExportResolver* export_resolver)
    : memory_(memory), export_resolver_(export_resolver) {}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
void Processor::set_guest_invocation_capture_sink(
    GuestInvocationCaptureEventSink* sink) {
  if (!ReconfigureGuestInvocationCaptureSink(nullptr, sink, false)) {
    std::abort();
  }
}

bool Processor::TrySetGuestInvocationCaptureSink(
    GuestInvocationCaptureEventSink* expected_sink,
    GuestInvocationCaptureEventSink* sink) {
  return ReconfigureGuestInvocationCaptureSink(expected_sink, sink, true);
}

bool Processor::DisableGuestInvocationCaptureEventsForSink(
    GuestInvocationCaptureEventSink* expected_sink) {
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  if (guest_invocation_capture_sink_transition_pending_ ||
      guest_invocation_capture_sink_ != expected_sink) {
    return false;
  }
  const uint64_t control =
      guest_invocation_capture_control_.load(std::memory_order_relaxed);
  const uint64_t disabled_control = MakeGuestInvocationCaptureControl(
      GuestInvocationCaptureControlRoot(control), 0,
      GuestInvocationCaptureControlGeneration(control));
  guest_invocation_capture_control_.store(disabled_control,
                                          std::memory_order_release);
  for (ThreadState* thread_state = guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    std::atomic_ref<uint64_t>(
        thread_state->context()->guest_invocation_capture_control)
        .store(disabled_control, std::memory_order_release);
  }
  return true;
}

bool Processor::ReconfigureGuestInvocationCaptureSink(
    GuestInvocationCaptureEventSink* expected_sink,
    GuestInvocationCaptureEventSink* sink, bool require_expected_sink) {
  if (guest_invocation_capture_sink_callback_depth) {
    std::abort();
  }
  if (guest_execution_capture_callback_active) {
    return false;
  }
  const uint32_t root_address = sink ? sink->root_address() : 0;
  const uint8_t initial_event_mask = sink ? sink->initial_event_mask() : 0;
  std::unique_lock<std::mutex> lock(
      guest_execution_capture_thread_state_mutex_);
  guest_invocation_capture_sink_condition_.wait(lock, [this] {
    return !guest_invocation_capture_sink_transition_pending_;
  });
  if (require_expected_sink &&
      guest_invocation_capture_sink_ != expected_sink) {
    return false;
  }
  guest_invocation_capture_sink_transition_pending_ = true;
  const uint64_t old_control =
      guest_invocation_capture_control_.load(std::memory_order_relaxed);
  const uint64_t disabled_control = MakeGuestInvocationCaptureControl(
      GuestInvocationCaptureControlRoot(old_control), 0,
      GuestInvocationCaptureControlGeneration(old_control));
  guest_invocation_capture_control_.store(disabled_control,
                                          std::memory_order_release);
  for (ThreadState* thread_state = guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    std::atomic_ref<uint64_t>(
        thread_state->context()->guest_invocation_capture_control)
        .store(disabled_control, std::memory_order_release);
  }
  guest_invocation_capture_sink_condition_.wait(lock, [this] {
    return guest_invocation_capture_sink_callback_count_ == 0;
  });

  const uint32_t generation = NextGuestInvocationCaptureGeneration(
      GuestInvocationCaptureControlGeneration(old_control));
  const uint64_t new_control = MakeGuestInvocationCaptureControl(
      root_address, initial_event_mask, generation);
  guest_invocation_capture_sink_ = sink;
  guest_invocation_capture_control_.store(new_control,
                                          std::memory_order_release);
  for (ThreadState* thread_state = guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    std::atomic_ref<uint64_t>(
        thread_state->context()->guest_invocation_capture_control)
        .store(new_control, std::memory_order_release);
  }
  guest_invocation_capture_sink_transition_pending_ = false;
  lock.unlock();
  guest_invocation_capture_sink_condition_.notify_all();
  return true;
}

Processor::GuestInvocationCaptureSinkLease::GuestInvocationCaptureSinkLease(
    GuestInvocationCaptureSinkLease&& other) noexcept
    : processor_(other.processor_),
      sink_(other.sink_),
      generation_(other.generation_),
      context_(other.context_) {
  other.processor_ = nullptr;
  other.sink_ = nullptr;
  other.generation_ = 0;
  other.context_ = nullptr;
}

Processor::GuestInvocationCaptureSinkLease::~GuestInvocationCaptureSinkLease() {
  if (processor_) {
    processor_->ReleaseGuestInvocationCaptureSink(sink_, generation_, context_);
  }
}

GuestInvocationCaptureEventSink* Processor::guest_invocation_capture_sink()
    const {
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  return guest_invocation_capture_sink_;
}

uint32_t Processor::guest_invocation_capture_root_address() const {
  return GuestInvocationCaptureControlRoot(
      guest_invocation_capture_control_.load(std::memory_order_acquire));
}

uint8_t Processor::guest_invocation_capture_initial_event_mask() const {
  return GuestInvocationCaptureControlEventMask(
      guest_invocation_capture_control_.load(std::memory_order_acquire));
}

uint64_t Processor::guest_invocation_capture_control() const {
  return guest_invocation_capture_control_.load(std::memory_order_acquire);
}

Processor::GuestInvocationCaptureSinkLease
Processor::AcquireGuestInvocationCaptureSink(uint64_t observed_control,
                                             uint8_t required_event_mask,
                                             ppc::PPCContext* context) {
  if (!GuestInvocationCaptureControlGeneration(observed_control) ||
      (required_event_mask &&
       !(GuestInvocationCaptureControlEventMask(observed_control) &
         required_event_mask))) {
    return {};
  }
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  const uint64_t current_control =
      guest_invocation_capture_control_.load(std::memory_order_relaxed);
  const uint32_t generation =
      GuestInvocationCaptureControlGeneration(observed_control);
  if (guest_invocation_capture_sink_transition_pending_ ||
      !guest_invocation_capture_sink_ ||
      generation != GuestInvocationCaptureControlGeneration(current_control) ||
      GuestInvocationCaptureControlRoot(observed_control) !=
          GuestInvocationCaptureControlRoot(current_control) ||
      (required_event_mask &&
       !GuestInvocationCaptureControlEventMask(current_control))) {
    return {};
  }
  ++guest_invocation_capture_sink_callback_count_;
  ++guest_invocation_capture_sink_callback_depth;
  return {this, guest_invocation_capture_sink_, generation, context};
}

void Processor::ReleaseGuestInvocationCaptureSink(
    GuestInvocationCaptureEventSink* sink, uint32_t generation,
    ppc::PPCContext* context) {
  const uint8_t initial_event_mask = sink->initial_event_mask();
  uint8_t context_event_mask = 0;
  if (context) {
    context_event_mask = sink->event_mask(
        {reinterpret_cast<uintptr_t>(context), context->thread_id});
  }

  {
    std::lock_guard<std::mutex> lock(
        guest_execution_capture_thread_state_mutex_);
    const uint64_t current_control =
        guest_invocation_capture_control_.load(std::memory_order_relaxed);
    const uint32_t current_generation =
        GuestInvocationCaptureControlGeneration(current_control);
    if (!guest_invocation_capture_sink_transition_pending_ &&
        sink == guest_invocation_capture_sink_ &&
        generation == current_generation) {
      if (!initial_event_mask ||
          !GuestInvocationCaptureControlEventMask(current_control)) {
        const uint64_t disabled_control = MakeGuestInvocationCaptureControl(
            GuestInvocationCaptureControlRoot(current_control), 0,
            current_generation);
        guest_invocation_capture_control_.store(disabled_control,
                                                std::memory_order_release);
        for (ThreadState* thread_state =
                 guest_execution_capture_thread_state_head_;
             thread_state;
             thread_state = thread_state->guest_execution_capture_next_) {
          std::atomic_ref<uint64_t>(
              thread_state->context()->guest_invocation_capture_control)
              .store(disabled_control, std::memory_order_release);
        }
      } else if (context) {
        const uint64_t context_control =
            std::atomic_ref<uint64_t>(context->guest_invocation_capture_control)
                .load(std::memory_order_acquire);
        if (GuestInvocationCaptureControlGeneration(context_control) ==
            current_generation) {
          std::atomic_ref<uint64_t>(context->guest_invocation_capture_control)
              .store(MakeGuestInvocationCaptureControl(
                         GuestInvocationCaptureControlRoot(current_control),
                         context_event_mask, current_generation),
                     std::memory_order_release);
        }
      }
    }
    if (!guest_invocation_capture_sink_callback_count_) {
      std::abort();
    }
    --guest_invocation_capture_sink_callback_count_;
    if (!guest_invocation_capture_sink_callback_count_) {
      guest_invocation_capture_sink_condition_.notify_all();
    }
  }
  if (!guest_invocation_capture_sink_callback_depth) {
    std::abort();
  }
  --guest_invocation_capture_sink_callback_depth;
}

void Processor::NotifyGuestInvocationCaptureFunctionDependency(
    uint32_t source_address, uint32_t target_address) {
  const uint64_t control = guest_invocation_capture_control();
  GuestInvocationCaptureSinkLease lease =
      AcquireGuestInvocationCaptureSink(control, 0);
  if (!lease) {
    return;
  }
  lease.sink()->OnFunctionDependency(source_address, target_address);
}

void Processor::NotifyGuestInvocationCaptureFunctionDefined(
    uint32_t address, uint32_t end_address) {
  const uint64_t control = guest_invocation_capture_control();
  GuestInvocationCaptureSinkLease lease =
      AcquireGuestInvocationCaptureSink(control, 0);
  if (!lease) {
    return;
  }
  lease.sink()->OnFunctionDefined(address, end_address);
}
#endif

Processor::~Processor() {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  if (guest_execution_capture_callback_active) {
    std::abort();
  }
  // ThreadState and generated-call owners must quiesce before Processor.
  // Failing closed here prevents their later destructors or callback releases
  // from touching a destroyed registry.
  {
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    std::lock_guard<std::mutex> observer_lock(
        guest_execution_capture_host_call_observer_mutex_);
    if (guest_execution_capture_thread_state_head_ ||
        guest_invocation_capture_sink_callback_count_ ||
        guest_invocation_capture_sink_transition_pending_ ||
        guest_execution_capture_host_call_dispatch_count_ ||
        guest_execution_capture_host_call_observer_transition_pending_) {
      std::abort();
    }
  }
#endif

  {
    auto global_lock = global_critical_region_.Acquire();
    modules_.clear();
  }

  frontend_.reset();
  backend_.reset();

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  // Registration is shared-owned, so even a failed owner-side detach cannot
  // dangle. Release only after generated guest execution has been destroyed,
  // and never run an observer destructor under the registration lock.
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer_to_release;
  {
    std::lock_guard<std::mutex> lock(
        guest_execution_capture_host_call_observer_mutex_);
    observer_to_release =
        std::move(guest_execution_capture_host_call_observer_);
  }
  observer_to_release.reset();
#endif

  if (trace_counts_dump_section_) {
    Profiler::UnregisterDumpSection(trace_counts_dump_section_);
    trace_counts_dump_section_ = 0;
  }

  // Backend teardown above took the generated code with it, so nothing can
  // still be counting.
  {
    std::lock_guard<std::mutex> lock(trace_counts_mutex_);
    for (auto& arena : trace_counts_arenas_) {
      xe::memory::DeallocFixed(arena.base, 0,
                               xe::memory::DeallocationType::kRelease);
    }
    trace_counts_arenas_.clear();
    trace_counts_free_.clear();
    trace_counts_regions_.clear();
    trace_counts_fallback_ = nullptr;
  }
}

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
bool Processor::AttachGuestExecutionCaptureHostCallObserver(
    std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer) {
  if (guest_execution_capture_callback_active || !observer) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(
        guest_execution_capture_host_call_observer_mutex_);
    if (guest_execution_capture_host_call_observer_ ||
        guest_execution_capture_host_call_dispatch_count_ ||
        guest_execution_capture_host_call_dispatch_seen_ ||
        guest_execution_capture_host_call_observer_transition_pending_) {
      return false;
    }
    guest_execution_capture_host_call_observer_transition_pending_ = true;
  }
  bool observer_can_attach = false;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    observer_can_attach = observer->CanDetach();
  }
  if (!observer_can_attach) {
    std::lock_guard<std::mutex> lock(
        guest_execution_capture_host_call_observer_mutex_);
    if (guest_execution_capture_host_call_observer_transition_pending_) {
      guest_execution_capture_host_call_observer_transition_pending_ = false;
    }
    return false;
  }

  // Freeze the registry through validation, seeding and installation.
  // Pre-validation guarantees that a pending lifetime causes zero partial seed
  // callbacks.
  std::lock_guard<std::mutex> thread_state_lock(
      guest_execution_capture_thread_state_mutex_);
  {
    std::lock_guard<std::mutex> observer_lock(
        guest_execution_capture_host_call_observer_mutex_);
    if (guest_execution_capture_host_call_observer_ ||
        guest_execution_capture_host_call_dispatch_count_ ||
        guest_execution_capture_host_call_dispatch_seen_ ||
        !guest_execution_capture_host_call_observer_transition_pending_) {
      guest_execution_capture_host_call_observer_transition_pending_ = false;
      return false;
    }
  }
  if (guest_execution_capture_thread_state_rejection_ !=
      GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
    std::lock_guard<std::mutex> observer_lock(
        guest_execution_capture_host_call_observer_mutex_);
    guest_execution_capture_host_call_observer_transition_pending_ = false;
    return false;
  }
  for (const ThreadState* thread_state =
           guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    if (thread_state->guest_execution_capture_lifecycle_state_ !=
        GuestExecutionCaptureThreadStateLifecycleState::kReady) {
      std::lock_guard<std::mutex> observer_lock(
          guest_execution_capture_host_call_observer_mutex_);
      guest_execution_capture_host_call_observer_transition_pending_ = false;
      return false;
    }
  }

  std::vector<GuestExecutionCaptureThreadStateLifecycleEvent> seed;
  try {
    for (const ThreadState* thread_state =
             guest_execution_capture_thread_state_head_;
         thread_state;
         thread_state = thread_state->guest_execution_capture_next_) {
      seed.push_back(MakeThreadStateLifecycleEvent(
          *thread_state,
          GuestExecutionCaptureThreadStateLifecycleState::kReady));
    }
  } catch (...) {
    std::lock_guard<std::mutex> observer_lock(
        guest_execution_capture_host_call_observer_mutex_);
    guest_execution_capture_host_call_observer_transition_pending_ = false;
    return false;
  }
  GuestExecutionCaptureThreadStateLifecycleDisposition seed_disposition;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    seed_disposition = observer->OnThreadStateSeed(seed);
  }
  const bool seed_accepted =
      seed_disposition ==
      GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;

  std::lock_guard<std::mutex> observer_lock(
      guest_execution_capture_host_call_observer_mutex_);
  if (guest_execution_capture_host_call_observer_ ||
      !guest_execution_capture_host_call_observer_transition_pending_ ||
      guest_execution_capture_host_call_dispatch_count_ ||
      guest_execution_capture_host_call_dispatch_seen_) {
    if (guest_execution_capture_host_call_observer_transition_pending_) {
      guest_execution_capture_host_call_observer_transition_pending_ = false;
    }
    return false;
  }
  if (seed_accepted) {
    guest_execution_capture_host_call_observer_ = std::move(observer);
    guest_execution_capture_host_call_dispatch_epoch_ = 0;
  }
  guest_execution_capture_host_call_observer_transition_pending_ = false;
  return seed_accepted;
}

bool Processor::DetachGuestExecutionCaptureHostCallObserver(
    const std::shared_ptr<GuestExecutionCaptureHostCallObserver>& observer) {
  if (guest_execution_capture_callback_active || !observer) {
    return false;
  }
  uint64_t dispatch_epoch = 0;
  {
    std::lock_guard<std::mutex> lock(
        guest_execution_capture_host_call_observer_mutex_);
    if (guest_execution_capture_host_call_observer_ != observer ||
        guest_execution_capture_host_call_dispatch_count_ ||
        guest_execution_capture_host_call_dispatch_seen_ ||
        guest_execution_capture_host_call_observer_transition_pending_) {
      return false;
    }
    guest_execution_capture_host_call_observer_transition_pending_ = true;
    dispatch_epoch = guest_execution_capture_host_call_dispatch_epoch_;
  }
  bool observer_can_detach = false;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    observer_can_detach = observer->CanDetach();
  }
  std::lock_guard<std::mutex> thread_state_lock(
      guest_execution_capture_thread_state_mutex_);
  std::lock_guard<std::mutex> observer_lock(
      guest_execution_capture_host_call_observer_mutex_);
  if (guest_execution_capture_host_call_observer_ != observer ||
      !guest_execution_capture_host_call_observer_transition_pending_) {
    if (guest_execution_capture_host_call_observer_transition_pending_) {
      guest_execution_capture_host_call_observer_transition_pending_ = false;
    }
    return false;
  }
  const bool can_detach =
      observer_can_detach &&
      !guest_execution_capture_host_call_dispatch_count_ &&
      !guest_execution_capture_host_call_dispatch_seen_ &&
      guest_execution_capture_host_call_dispatch_epoch_ == dispatch_epoch;
  if (can_detach) {
    guest_execution_capture_host_call_observer_ = nullptr;
    if (guest_execution_capture_thread_state_rejection_ ==
            GuestExecutionCaptureThreadStateRegistryRejection::
                kObserverRejectedRuntimeEvent ||
        guest_execution_capture_thread_state_rejection_ ==
            GuestExecutionCaptureThreadStateRegistryRejection::
                kObserverRejectedJitSafepoint) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::kNone;
    }
  }
  guest_execution_capture_host_call_observer_transition_pending_ = false;
  return can_detach;
}

GuestExecutionCaptureHostCallToken
Processor::BeginGuestExecutionCaptureHostCall(const ThreadState& thread_state,
                                              const GuestFunction& function,
                                              uint32_t return_address) {
  if (guest_execution_capture_callback_active) {
    return {};
  }
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer;
  {
    // Attachment holds this lock through its transactional seed and install.
    // Taking it before latching dispatch makes the two operations atomic: a
    // racing call either makes attachment fail before seeding, or enters only
    // after the observer is installed.
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    observer = AcquireGuestExecutionCaptureObserverDispatch(true);
  }
  if (!observer) {
    return {};
  }
  GuestExecutionCaptureHostCallToken token;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    token =
        observer->OnHostGuestCallBegin(thread_state, function, return_address);
  }
  ReleaseGuestExecutionCaptureObserverDispatch();
  return token;
}

bool Processor::EndGuestExecutionCaptureHostCall(
    GuestExecutionCaptureHostCallToken token, const ThreadState& thread_state,
    const GuestFunction& function,
    GuestExecutionCaptureHostCallOutcome outcome) {
  if (guest_execution_capture_callback_active || !token) {
    return false;
  }
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer =
      AcquireGuestExecutionCaptureObserverDispatch(false);
  if (!observer) {
    return false;
  }
  bool result = false;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    result =
        observer->OnHostGuestCallEnd(token, thread_state, function, outcome);
  }
  ReleaseGuestExecutionCaptureObserverDispatch();
  return result;
}

std::shared_ptr<GuestExecutionCaptureHostCallObserver>
Processor::AcquireGuestExecutionCaptureObserverDispatch(bool host_call_begin) {
  std::lock_guard<std::mutex> lock(
      guest_execution_capture_host_call_observer_mutex_);
  if (host_call_begin) {
    guest_execution_capture_host_call_dispatch_seen_ = true;
  }
  if (!guest_execution_capture_host_call_observer_) {
    return nullptr;
  }
  if (guest_execution_capture_host_call_dispatch_count_ ==
          std::numeric_limits<uint64_t>::max() ||
      guest_execution_capture_host_call_dispatch_epoch_ ==
          std::numeric_limits<uint64_t>::max()) {
    std::abort();
  }
  ++guest_execution_capture_host_call_dispatch_count_;
  ++guest_execution_capture_host_call_dispatch_epoch_;
  return guest_execution_capture_host_call_observer_;
}

void Processor::ReleaseGuestExecutionCaptureObserverDispatch() noexcept {
  std::lock_guard<std::mutex> lock(
      guest_execution_capture_host_call_observer_mutex_);
  if (!guest_execution_capture_host_call_dispatch_count_) {
    std::abort();
  }
  --guest_execution_capture_host_call_dispatch_count_;
}

void Processor::RegisterGuestExecutionCaptureThreadState(
    ThreadState& thread_state) noexcept {
  if (guest_execution_capture_thread_state_registration_gate_test_signal_) {
    guest_execution_capture_thread_state_registration_gate_test_signal_->store(
        true, std::memory_order_release);
    while (!guest_execution_capture_thread_state_registration_gate_test_release_
                ->load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
  if (guest_execution_capture_callback_active) {
    std::abort();
  }
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer;
  {
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    ThreadState** insertion = &guest_execution_capture_thread_state_head_;
    while (*insertion) {
      if (*insertion == &thread_state) {
        guest_execution_capture_thread_state_rejection_ =
            GuestExecutionCaptureThreadStateRegistryRejection::
                kDuplicateRegistration;
        std::abort();
      }
      insertion = &(*insertion)->guest_execution_capture_next_;
    }
    std::atomic_ref<uint64_t>(
        thread_state.context()->guest_invocation_capture_control)
        .store(
            guest_invocation_capture_control_.load(std::memory_order_acquire),
            std::memory_order_release);
    thread_state.guest_execution_capture_next_ = nullptr;
    *insertion = &thread_state;

    observer = AcquireGuestExecutionCaptureObserverDispatch(false);
    if (observer) {
      GuestExecutionCaptureThreadStateLifecycleDisposition disposition;
      {
        GuestExecutionCaptureCallbackScope callback_scope;
        disposition =
            observer->OnThreadStateLifecycle(MakeThreadStateLifecycleEvent(
                thread_state,
                GuestExecutionCaptureThreadStateLifecycleState::kPending));
      }
      if (disposition !=
              GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept &&
          guest_execution_capture_thread_state_rejection_ ==
              GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
        guest_execution_capture_thread_state_rejection_ =
            GuestExecutionCaptureThreadStateRegistryRejection::
                kObserverRejectedRuntimeEvent;
      }
      ReleaseGuestExecutionCaptureObserverDispatch();
    }
  }
  observer.reset();
}

GuestExecutionCaptureThreadStateLifecycleDisposition
Processor::PublishGuestExecutionCaptureThreadStateReady(
    ThreadState& thread_state) noexcept {
  if (guest_execution_capture_callback_active) {
    return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
  }
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer;
  {
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    ThreadState* registered = guest_execution_capture_thread_state_head_;
    while (registered && registered != &thread_state) {
      registered = registered->guest_execution_capture_next_;
    }
    if (!registered) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::
              kMissingRegistration;
      return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
    }
    if (thread_state.guest_execution_capture_lifecycle_state_ !=
        GuestExecutionCaptureThreadStateLifecycleState::kPending) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::
              kInvalidReadyTransition;
      return GuestExecutionCaptureThreadStateLifecycleDisposition::kReject;
    }
    thread_state.guest_execution_capture_lifecycle_state_ =
        GuestExecutionCaptureThreadStateLifecycleState::kReady;

    observer = AcquireGuestExecutionCaptureObserverDispatch(false);
    if (observer) {
      GuestExecutionCaptureThreadStateLifecycleDisposition disposition;
      {
        GuestExecutionCaptureCallbackScope callback_scope;
        disposition =
            observer->OnThreadStateLifecycle(MakeThreadStateLifecycleEvent(
                thread_state,
                GuestExecutionCaptureThreadStateLifecycleState::kReady));
      }
      if (disposition !=
              GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept &&
          guest_execution_capture_thread_state_rejection_ ==
              GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
        guest_execution_capture_thread_state_rejection_ =
            GuestExecutionCaptureThreadStateRegistryRejection::
                kObserverRejectedRuntimeEvent;
      }
      ReleaseGuestExecutionCaptureObserverDispatch();
    }
  }
  observer.reset();
  return GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept;
}

void Processor::BeginGuestExecutionCaptureThreadStateDestruction(
    ThreadState& thread_state) noexcept {
  if (guest_execution_capture_callback_active) {
    std::abort();
  }
  if (guest_execution_capture_thread_state_destruction_gate_test_signal_) {
    guest_execution_capture_thread_state_destruction_gate_test_signal_->store(
        true, std::memory_order_release);
  }
  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer;
  {
    std::unique_lock<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    ThreadState** registered = &guest_execution_capture_thread_state_head_;
    while (*registered && *registered != &thread_state) {
      registered = &(*registered)->guest_execution_capture_next_;
    }
    if (!*registered) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::
              kMissingRegistration;
      std::abort();
    }
    if (thread_state.guest_execution_capture_lifecycle_state_ !=
            GuestExecutionCaptureThreadStateLifecycleState::kPending &&
        thread_state.guest_execution_capture_lifecycle_state_ !=
            GuestExecutionCaptureThreadStateLifecycleState::kReady) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::
              kInvalidDestroyTransition;
      std::abort();
    }
    thread_state.guest_execution_capture_lifecycle_state_ =
        GuestExecutionCaptureThreadStateLifecycleState::kDestroying;

    // Reject new leases first, then let callbacks already parked in the
    // observer finish without holding the registry gate. Other ThreadStates
    // can still reach their safepoints while this lifetime is draining.
    guest_execution_capture_thread_state_condition_.wait(
        thread_state_lock, [&thread_state] {
          return !thread_state
                      .guest_execution_capture_jit_safepoint_callback_count_;
        });

    observer = AcquireGuestExecutionCaptureObserverDispatch(false);
    if (observer) {
      GuestExecutionCaptureThreadStateLifecycleDisposition disposition;
      {
        GuestExecutionCaptureCallbackScope callback_scope;
        disposition =
            observer->OnThreadStateLifecycle(MakeThreadStateLifecycleEvent(
                thread_state,
                GuestExecutionCaptureThreadStateLifecycleState::kDestroying));
      }
      if (disposition !=
              GuestExecutionCaptureThreadStateLifecycleDisposition::kAccept &&
          guest_execution_capture_thread_state_rejection_ ==
              GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
        guest_execution_capture_thread_state_rejection_ =
            GuestExecutionCaptureThreadStateRegistryRejection::
                kObserverRejectedRuntimeEvent;
      }
    }

    if (observer) {
      ReleaseGuestExecutionCaptureObserverDispatch();
    }
  }
  observer.reset();
}

void Processor::CompleteGuestExecutionCaptureThreadStateDestruction(
    ThreadState& thread_state) noexcept {
  if (guest_execution_capture_callback_active) {
    std::abort();
  }
  std::lock_guard<std::mutex> thread_state_lock(
      guest_execution_capture_thread_state_mutex_);
  ThreadState** registered = &guest_execution_capture_thread_state_head_;
  while (*registered && *registered != &thread_state) {
    registered = &(*registered)->guest_execution_capture_next_;
  }
  if (!*registered) {
    guest_execution_capture_thread_state_rejection_ =
        GuestExecutionCaptureThreadStateRegistryRejection::kMissingRegistration;
    std::abort();
  }
  if (thread_state.guest_execution_capture_lifecycle_state_ !=
      GuestExecutionCaptureThreadStateLifecycleState::kDestroying) {
    guest_execution_capture_thread_state_rejection_ =
        GuestExecutionCaptureThreadStateRegistryRejection::
            kInvalidDestroyTransition;
    std::abort();
  }
  *registered = thread_state.guest_execution_capture_next_;
  thread_state.guest_execution_capture_next_ = nullptr;
}

GuestExecutionCaptureThreadStateRegistrySnapshot
Processor::QueryGuestExecutionCaptureParticipants() const {
  GuestExecutionCaptureThreadStateRegistrySnapshot result;
  if (guest_execution_capture_callback_active) {
    result.rejection = GuestExecutionCaptureThreadStateRegistryRejection::
        kObserverCallbackReentry;
    return result;
  }
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  result.rejection = guest_execution_capture_thread_state_rejection_;
  for (const ThreadState* thread_state =
           guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    result.participants.push_back(MakeThreadStateLifecycleEvent(
        *thread_state, thread_state->guest_execution_capture_lifecycle_state_));
  }
  return result;
}

GuestExecutionCaptureThreadStateRegistrySnapshot
Processor::QueryGuestExecutionCaptureParticipantsAtCutoff(
    std::atomic<bool>& capture_active) const {
  GuestExecutionCaptureThreadStateRegistrySnapshot result;
  if (guest_execution_capture_callback_active) {
    result.rejection = GuestExecutionCaptureThreadStateRegistryRejection::
        kObserverCallbackReentry;
    return result;
  }
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  capture_active.store(false, std::memory_order_release);
  result.rejection = guest_execution_capture_thread_state_rejection_;
  for (const ThreadState* thread_state =
           guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    result.participants.push_back(MakeThreadStateLifecycleEvent(
        *thread_state, thread_state->guest_execution_capture_lifecycle_state_));
  }
  return result;
}

GuestExecutionCaptureThreadStateVisitResult
Processor::VisitGuestExecutionCaptureThreadStates(
    GuestExecutionCaptureThreadStateVisitor& visitor) const noexcept {
  if (guest_execution_capture_callback_active) {
    return GuestExecutionCaptureThreadStateVisitResult::
        kObserverCallbackReentry;
  }
  std::lock_guard<std::mutex> lock(guest_execution_capture_thread_state_mutex_);
  if (guest_execution_capture_thread_state_rejection_ !=
      GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
    return GuestExecutionCaptureThreadStateVisitResult::kRegistryRejected;
  }
  for (const ThreadState* thread_state =
           guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    if (thread_state->guest_execution_capture_lifecycle_state_ !=
        GuestExecutionCaptureThreadStateLifecycleState::kReady) {
      return GuestExecutionCaptureThreadStateVisitResult::kParticipantNotReady;
    }
  }
  for (const ThreadState* thread_state =
           guest_execution_capture_thread_state_head_;
       thread_state;
       thread_state = thread_state->guest_execution_capture_next_) {
    bool continue_visiting = false;
    {
      GuestExecutionCaptureCallbackScope callback_scope;
      continue_visiting = visitor.VisitThreadState(*thread_state);
    }
    if (!continue_visiting) {
      return GuestExecutionCaptureThreadStateVisitResult::kStoppedByVisitor;
    }
  }
  return GuestExecutionCaptureThreadStateVisitResult::kCompleted;
}

GuestExecutionCaptureJitSafepointResult
Processor::DeliverGuestExecutionCaptureJitSafepoint(
    ThreadState& thread_state, uint32_t guest_address) noexcept {
  if (guest_execution_capture_callback_active) {
    return GuestExecutionCaptureJitSafepointResult::kObserverCallbackReentry;
  }

  std::shared_ptr<GuestExecutionCaptureHostCallObserver> observer;
  {
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    ThreadState* registered = guest_execution_capture_thread_state_head_;
    while (registered && registered != &thread_state) {
      registered = registered->guest_execution_capture_next_;
    }
    if (!registered || thread_state.processor() != this ||
        thread_state.guest_execution_capture_lifecycle_state_ !=
            GuestExecutionCaptureThreadStateLifecycleState::kReady) {
      return GuestExecutionCaptureJitSafepointResult::kParticipantNotReady;
    }
    if (guest_execution_capture_thread_state_rejection_ !=
        GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
      return GuestExecutionCaptureJitSafepointResult::kRegistryRejected;
    }
    if (thread_state.guest_execution_capture_jit_safepoint_callback_count_ ==
        std::numeric_limits<uint64_t>::max()) {
      std::abort();
    }
    observer = AcquireGuestExecutionCaptureObserverDispatch(false);
    if (!observer) {
      return GuestExecutionCaptureJitSafepointResult::kNoObserver;
    }
    ++thread_state.guest_execution_capture_jit_safepoint_callback_count_;
  }

  GuestExecutionCaptureJitSafepointDisposition disposition;
  {
    GuestExecutionCaptureCallbackScope callback_scope;
    disposition = observer->OnJitSafepoint(thread_state, guest_address);
  }

  {
    std::lock_guard<std::mutex> thread_state_lock(
        guest_execution_capture_thread_state_mutex_);
    if (!thread_state.guest_execution_capture_jit_safepoint_callback_count_) {
      std::abort();
    }
    --thread_state.guest_execution_capture_jit_safepoint_callback_count_;
    if (disposition == GuestExecutionCaptureJitSafepointDisposition::kReject &&
        guest_execution_capture_thread_state_rejection_ ==
            GuestExecutionCaptureThreadStateRegistryRejection::kNone) {
      guest_execution_capture_thread_state_rejection_ =
          GuestExecutionCaptureThreadStateRegistryRejection::
              kObserverRejectedJitSafepoint;
    }
  }
  guest_execution_capture_thread_state_condition_.notify_all();
  ReleaseGuestExecutionCaptureObserverDispatch();
  observer.reset();
  return disposition == GuestExecutionCaptureJitSafepointDisposition::kAccept
             ? GuestExecutionCaptureJitSafepointResult::kDelivered
             : GuestExecutionCaptureJitSafepointResult::kObserverRejected;
}

GuestExecutionCaptureJitSafepointResult HandleGuestExecutionCaptureJitSafepoint(
    void* raw_context, uint64_t guest_address) noexcept {
  if (guest_execution_capture_callback_active) {
    return GuestExecutionCaptureJitSafepointResult::kObserverCallbackReentry;
  }
  auto* context = reinterpret_cast<ppc::PPCContext*>(raw_context);
  if (!context || !context->processor || !context->thread_state ||
      context->thread_state->context() != context ||
      context->thread_state->processor() != context->processor) {
    return GuestExecutionCaptureJitSafepointResult::kInvalidContext;
  }
  if (!std::atomic_ref<uint8_t>(context->capture_rendezvous_requested)
           .exchange(0, std::memory_order_acq_rel)) {
    return GuestExecutionCaptureJitSafepointResult::kNotRequested;
  }
  return context->processor->DeliverGuestExecutionCaptureJitSafepoint(
      *context->thread_state, static_cast<uint32_t>(guest_address));
}
#endif

bool Processor::Setup(std::unique_ptr<backend::Backend> backend) {
  // TODO(benvanik): query mode from debugger?
  debug_info_flags_ = 0;

  auto frontend = std::make_unique<ppc::PPCFrontend>(this);
  // TODO(benvanik): set options/etc.

  // Must be initialized by subclass before calling into this.
  assert_not_null(memory_);

  std::unique_ptr<Module> builtin_module(new BuiltinModule(this));
  builtin_module_ = builtin_module.get();
  modules_.push_back(std::move(builtin_module));

  if (frontend_ || backend_) {
    return false;
  }

  if (!backend) {
    return false;
  }
  if (!backend->Initialize(this)) {
    return false;
  }
  if (!frontend->Initialize()) {
    return false;
  }

  backend_ = std::move(backend);
  frontend_ = std::move(frontend);

  // Stack walker is used when profiling, debugging, and dumping.
  // Note that creation may fail, in which case we'll have to disable those
  // features.
  // The code cache may be unavailable in case of a "null" backend.
  cpu::backend::CodeCache* code_cache = backend_->code_cache();
  if (code_cache) {
    stack_walker_ = StackWalker::Create(code_cache);
  }
  if (!stack_walker_) {
    // TODO(benvanik): disable features.
    if (cvars::debug) {
      XELOGW("Disabling --debug due to lack of stack walker");
      cvars::debug = false;
    }
  }

  RefreshTraceCountsEnabled();

  // Open the JIT corpus, if requested.
  if (!cvars::jit_corpus_out.empty()) {
    jit_corpus_writer_ = JitCorpusWriter::Create(cvars::jit_corpus_out);
    if (jit_corpus_writer_) {
      XELOGI("Capturing JIT corpus to {}",
             xe::path_to_utf8(cvars::jit_corpus_out));
    }
  }

  return true;
}

// Re-read at every title launch, because per-title config is applied after
// this processor was built and a first launch never re-runs Setup. Turning
// coverage off again is ignored, counting code already generated keeps
// writing to its arena and dropping it would only lose counts.
void Processor::RefreshTraceCountsEnabled() {
  if (!cvars::trace_function_coverage || trace_counts_enabled_) {
    return;
  }
  trace_counts_enabled_ = true;
  // A run that has to be quit by hand is not a run that can be repeated, and
  // the counters only reach a file through a clean shutdown otherwise.
  if (cvars::trace_function_coverage_period > 0 &&
      !trace_counts_periodic_started_) {
    trace_counts_periodic_started_ = true;
    std::thread([this]() {
      xe::threading::set_name("Coverage Dump");
      for (;;) {
        xe::threading::Sleep(
            std::chrono::seconds(cvars::trace_function_coverage_period));
        DumpTraceCountsToFile();
      }
    }).detach();
  }
  if (!trace_counts_dump_section_) {
    trace_counts_dump_section_ =
        Profiler::RegisterDumpSection([this](FILE* f) { DumpTraceCounts(f); });
  }
  // Counters accumulate until reset, so the timers have to as well or the two
  // halves of the dump would describe different windows.
  Profiler::ResetAggregation();
}

// Same tables the profiler dump appends, written on their own to a file the
// caller names. This is the only route to them on an ordinary build.
void Processor::DumpTraceCountsToFile() {
  if (cvars::trace_function_coverage_out.empty() || !trace_counts_enabled_) {
    return;
  }
  // Written to a sibling and renamed, because the periodic writer below can be
  // running when the process is killed; a reader must never see half a table.
  std::filesystem::path final_path = cvars::trace_function_coverage_out;
  std::filesystem::path tmp_path = final_path;
  tmp_path += ".part";
  FILE* f = xe::filesystem::OpenFile(tmp_path, "wb");
  if (!f) {
    XELOGE("Could not open {} for the coverage dump",
           xe::path_to_utf8(cvars::trace_function_coverage_out));
    return;
  }
  DumpTraceCounts(f);
  fclose(f);
  std::error_code ec;
  std::filesystem::rename(tmp_path, final_path, ec);
  if (ec) {
    XELOGE("Could not move the coverage dump into place: {}", ec.message());
    return;
  }
  XELOGI("Guest coverage written to {}", xe::path_to_utf8(final_path));
}

// Reads the shared retired totals plus every live arena without disturbing
// them. Racing increments are benign, an in-flight count lands next dump.
void Processor::DumpTraceCounts(FILE* f) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);

  struct Row {
    uint32_t address;
    uint32_t instruction_count;
    uint64_t executed;
    uint32_t hottest_address;
    uint64_t hottest;
    size_t region;
  };
  std::vector<Row> rows;
  rows.reserve(trace_counts_regions_.size());
  uint64_t grand_total = 0;
  for (size_t r = 0; r < trace_counts_regions_.size(); ++r) {
    auto& region = trace_counts_regions_[r];
    Row row = {};
    row.address = region.start_address;
    row.instruction_count = static_cast<uint32_t>(region.count);
    row.region = r;
    for (size_t i = 0; i < region.count; ++i) {
      uint64_t total = region.retired[i];
      for (auto& arena : trace_counts_arenas_) {
        total +=
            reinterpret_cast<const uint64_t*>(arena.base + region.offset)[i];
      }
      row.executed += total;
      if (total > row.hottest) {
        row.hottest = total;
        row.hottest_address =
            region.start_address + static_cast<uint32_t>(i * 4);
      }
    }
    if (row.executed) {
      grand_total += row.executed;
      rows.push_back(row);
    }
  }
  if (rows.empty()) {
    return;
  }
  std::sort(rows.begin(), rows.end(),
            [](const Row& a, const Row& b) { return a.executed > b.executed; });

  std::fprintf(f, "\n\nguestcoverage,%llu\n",
               static_cast<unsigned long long>(grand_total));
  std::fprintf(f, "address,instructions,executed,share,hottest,hottestcount\n");
  for (auto& row : rows) {
    std::fprintf(
        f, "\"%08X\",%u,%llu,%.6f,\"%08X\",%llu\n", row.address,
        row.instruction_count, static_cast<unsigned long long>(row.executed),
        double(row.executed) / double(grand_total), row.hottest_address,
        static_cast<unsigned long long>(row.hottest));
  }

  // Every counter, not just the per-function total and the one hottest PC.
  // A function total cannot tell an inner loop from the call setup around it,
  // and that distinction is what decides whether a codegen change is worth
  // anything: the loop is where the executions are, and it is often a handful
  // of the function's instructions.
  std::fprintf(f, "\n\nguestpc,%llu\n",
               static_cast<unsigned long long>(grand_total));
  std::fprintf(f, "address,executed\n");
  for (auto& row : rows) {
    auto& region = trace_counts_regions_[row.region];
    for (size_t i = 0; i < region.count; ++i) {
      uint64_t total = region.retired[i];
      for (auto& arena : trace_counts_arenas_) {
        total +=
            reinterpret_cast<const uint64_t*>(arena.base + region.offset)[i];
      }
      if (total) {
        std::fprintf(f, "\"%08X\",%llu\n",
                     region.start_address + static_cast<uint32_t>(i * 4),
                     static_cast<unsigned long long>(total));
      }
    }
  }

  // Long format so the column count does not depend on how many guest threads
  // happen to be live. Threads that already exited share the retired row.
  std::fprintf(f, "\n\nguestcoveragethreads\n");
  std::fprintf(f, "address,thread,executed\n");
  for (auto& row : rows) {
    auto& region = trace_counts_regions_[row.region];
    uint64_t retired = 0;
    for (size_t i = 0; i < region.count; ++i) {
      retired += region.retired[i];
    }
    if (retired) {
      std::fprintf(f, "\"%08X\",\"retired\",%llu\n", row.address,
                   static_cast<unsigned long long>(retired));
    }
    for (auto& arena : trace_counts_arenas_) {
      auto counts =
          reinterpret_cast<const uint64_t*>(arena.base + region.offset);
      uint64_t executed = 0;
      for (size_t i = 0; i < region.count; ++i) {
        executed += counts[i];
      }
      if (executed) {
        std::fprintf(f, "\"%08X\",\"%08X\",%llu\n", row.address,
                     arena.thread_id,
                     static_cast<unsigned long long>(executed));
      }
    }
  }

  DumpSequences(f);
}

// Which backend sequence the emitted code actually spends its executions in.
// Every key maps to exactly one selection in the backend's sequence table, so
// the top rows name the emitters worth optimizing for this title.
void Processor::DumpSequences(FILE* f) {
  struct SequenceRow {
    uint64_t executed = 0;
    uint64_t occurrences = 0;
    uint64_t host_bytes = 0;
    // Executions x that site's OWN emitted bytes, summed per site. The
    // alternative - executions x the sequence's average bytes - is only right
    // when a site's size is uncorrelated with how hot it is, which is exactly
    // what cannot be assumed of a sequence whose variants differ in size.
    uint64_t executed_host_bytes = 0;
    // The same for the bytes the sequence pushed into the function's tail,
    // kept separate because a tail is the cold side of a branch and its bytes
    // are not paid on the executed path.
    uint64_t executed_tail_bytes = 0;
    uint64_t executed_chain_instructions = 0;
    uint64_t executed_chains = 0;
    uint64_t tail_bytes = 0;
    uint64_t chains = 0;
  };
  std::map<uint64_t, SequenceRow> sequences;
  std::vector<uint64_t> totals;
  uint64_t grand_total = 0;
  for (auto& region : trace_counts_regions_) {
    if (region.samples.empty()) {
      continue;
    }
    totals.assign(region.count, 0);
    for (size_t i = 0; i < region.count; ++i) {
      uint64_t total = region.retired[i];
      for (auto& arena : trace_counts_arenas_) {
        total +=
            reinterpret_cast<const uint64_t*>(arena.base + region.offset)[i];
      }
      totals[i] = total;
    }
    for (auto& sample : region.samples) {
      if (sample.guest_index >= region.count) {
        continue;
      }
      auto& row = sequences[sample.key];
      const uint64_t executed = totals[sample.guest_index];
      row.executed += executed;
      row.occurrences += 1;
      row.host_bytes += sample.host_bytes;
      row.tail_bytes += sample.tail_bytes;
      row.chains += sample.chains;
      row.executed_host_bytes += executed * sample.host_bytes;
      row.executed_tail_bytes += executed * sample.tail_bytes;
      row.executed_chain_instructions += executed * sample.chain_instructions;
      row.executed_chains += executed * sample.chains;
      grand_total += executed;
    }
  }
  if (!grand_total) {
    return;
  }

  std::vector<std::pair<uint64_t, SequenceRow>> rows(sequences.begin(),
                                                     sequences.end());
  // Ranked by executed host bytes, which is the cost. Ranking by executions
  // alone puts a one-instruction sequence above one that is eight times the
  // size and nearly as hot.
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    return a.second.executed_host_bytes > b.second.executed_host_bytes;
  });
  uint64_t grand_host_bytes = 0;
  for (auto& row : rows) {
    grand_host_bytes += row.second.executed_host_bytes;
  }

  std::fprintf(f, "\n\nguestsequences,%llu,%llu\n",
               static_cast<unsigned long long>(grand_total),
               static_cast<unsigned long long>(grand_host_bytes));
  std::fprintf(f,
               "key,sequence,executed,share,occurrences,avgbytes,"
               "exechostbytes,hostshare,exectailbytes,execchaininsts,"
               "execchains,tailbytes,chains\n");
  for (auto& row : rows) {
    std::string label =
        backend_ ? backend_->FormatSequenceKey(row.first) : std::string();
    std::fprintf(
        f,
        "\"%016llX\",\"%s\",%llu,%.6f,%llu,%.2f,%llu,%.6f,%llu,%llu,%llu,"
        "%llu,%llu\n",
        static_cast<unsigned long long>(row.first), label.c_str(),
        static_cast<unsigned long long>(row.second.executed),
        double(row.second.executed) / double(grand_total),
        static_cast<unsigned long long>(row.second.occurrences),
        double(row.second.host_bytes) / double(row.second.occurrences),
        static_cast<unsigned long long>(row.second.executed_host_bytes),
        grand_host_bytes
            ? double(row.second.executed_host_bytes) / double(grand_host_bytes)
            : 0.0,
        static_cast<unsigned long long>(row.second.executed_tail_bytes),
        static_cast<unsigned long long>(row.second.executed_chain_instructions),
        static_cast<unsigned long long>(row.second.executed_chains),
        static_cast<unsigned long long>(row.second.tail_bytes),
        static_cast<unsigned long long>(row.second.chains));
  }
}

void Processor::PreLaunch() {
  if (cvars::break_on_start) {
    // Start paused.
    XELOGI("Breaking into debugger because of --break_on_start...");
    execution_state_ = ExecutionState::kRunning;
    Pause();
  } else {
    // Start running.
    execution_state_ = ExecutionState::kRunning;
  }
}

bool Processor::AddModule(std::unique_ptr<Module> module) {
  auto global_lock = global_critical_region_.Acquire();
  modules_.push_back(std::move(module));
  return true;
}

void Processor::RemoveModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();

  auto itr = std::ranges::find_if(
      std::as_const(modules_),
      [name](std::unique_ptr<xe::cpu::Module> const& module) {
        return module->name() == name;
      });

  if (itr != modules_.cend()) {
    const std::vector<uint32_t> addressed_functions =
        (*itr)->GetAddressedFunctions();

    modules_.erase(itr);

    for (const uint32_t entry : addressed_functions) {
      RemoveFunctionByAddress(entry);
    }
  }
}

Module* Processor::GetModule(const std::string_view name) {
  auto global_lock = global_critical_region_.Acquire();
  for (const auto& module : modules_) {
    if (module->name() == name) {
      return module.get();
    }
  }
  return nullptr;
}

std::vector<Module*> Processor::GetModules() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<Module*> clone(modules_.size());
  for (const auto& module : modules_) {
    clone.push_back(module.get());
  }
  return clone;
}

Function* Processor::DefineBuiltin(const std::string_view name,
                                   BuiltinFunction::Handler handler, void* arg0,
                                   void* arg1) {
  uint32_t address = next_builtin_address_;
  next_builtin_address_ += 4;

  Function* function;
  builtin_module_->DeclareFunction(address, &function);
  function->set_end_address(address + 4);
  function->set_name(name);

  auto builtin_function = static_cast<BuiltinFunction*>(function);
  builtin_function->SetupBuiltin(handler, arg0, arg1);

  function->set_status(Symbol::Status::kDeclared);
  return function;
}

Function* Processor::QueryFunction(uint32_t address) {
  auto entry = entry_table_.Get(address);
  if (!entry) {
    return nullptr;
  }
  return entry->function;
}

std::vector<Function*> Processor::FindFunctionsWithAddress(uint32_t address) {
  return entry_table_.FindWithAddress(address);
}

void Processor::RemoveFunctionByAddress(uint32_t address) {
  entry_table_.Delete(address);
}

Function* Processor::ResolveFunction(uint32_t address) {
  Entry* entry;
  Entry::Status status = entry_table_.GetOrCreate(address, &entry);
  if (status == Entry::STATUS_NEW) {
    // Needs to be generated. We have the 'lock' on it and must do so now.

    // Grab symbol declaration.
    auto function = LookupFunction(address);

    if (!function) {
      entry_table_.MarkFailed(entry);
      return nullptr;
    }

    if (!DemandFunction(function)) {
      entry_table_.MarkFailed(entry);
      return nullptr;
    }
    // only add it to the list of resolved functions if resolving succeeded
    auto module_for = function->module();

    auto xexmod = dynamic_cast<XexModule*>(module_for);
    if (xexmod) {
      auto addr_flags = xexmod->GetInstructionAddressFlags(address);
      if (addr_flags) {
        InfoCacheFlags bits{};
        bits.was_resolved = 1;
        AtomicSetInfoCacheFlags(addr_flags, bits);
      }
    }

    entry_table_.MarkReady(entry, function, function->end_address());
    status = Entry::STATUS_READY;
  }
  if (status == Entry::STATUS_READY) {
    // Ready to use.
    return entry->function;
  } else {
    // Failed or bad state.
    return nullptr;
  }
}
Module* Processor::LookupModule(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  // TODO(benvanik): sort by code address (if contiguous) so can bsearch.
  // TODO(benvanik): cache last module low/high, as likely to be in there.
  for (const auto& module : modules_) {
    if (module->ContainsAddress(address)) {
      return module.get();
    }
  }
  return nullptr;
}
Function* Processor::LookupFunction(uint32_t address) {
  // TODO(benvanik): fast reject invalid addresses/log errors.

  // Find the module that contains the address.
  Module* code_module = LookupModule(address);

  if (!code_module) {
    // No module found that could contain the address.
    return nullptr;
  }

  return LookupFunction(code_module, address);
}

Function* Processor::LookupFunction(Module* module, uint32_t address) {
  // Atomic create/lookup symbol in module.
  // If we get back the NEW flag we must declare it now.
  Function* function = nullptr;
  auto symbol_status = module->DeclareFunction(address, &function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undeclared, so declare now.
    assert_true(function->is_guest());
    if (!frontend_->DeclareFunction(static_cast<GuestFunction*>(function))) {
      function->set_status(Symbol::Status::kFailed);
      return nullptr;
    }
    function->set_status(Symbol::Status::kDeclared);
  }
  return function;
}

bool Processor::DemandFunction(Function* function) {
  // Acquire before claiming kDefining so another translation can't make a
  // callee's machine_code visible partway through this translation. A
  // recursive mutex permits a definition triggered synchronously by the
  // current definition; the control must not be toggled while definitions are
  // in flight.
  std::unique_lock<std::recursive_mutex> definition_lock;
  if (cvars::serialize_guest_function_definitions) {
    definition_lock = std::unique_lock<std::recursive_mutex>(
        guest_function_definition_mutex_);
  }

  // Lock function for generation. If it's already being generated
  // by another thread this will block and return DECLARED.
  auto module = function->module();
  auto symbol_status = module->DefineFunction(function);
  if (symbol_status == Symbol::Status::kNew) {
    // Symbol is undefined, so define now.
    assert_true(function->is_guest());
    if (!frontend_->DefineFunction(static_cast<GuestFunction*>(function),
                                   debug_info_flags_)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
    NotifyGuestInvocationCaptureFunctionDefined(function->address(),
                                                function->end_address());
#endif

    auto* guest_function = static_cast<GuestFunction*>(function);
    if (!backend_->PublishGuestFunction(guest_function)) {
      function->set_status(Symbol::Status::kFailed);
      return false;
    }

    // Before we give the symbol back to the rest, let the debugger know.
    OnFunctionDefined(function);

    if (jit_corpus_writer_) {
      // Externs carry no emitted code, so they have nothing to replay.
      if (guest_function->machine_code()) {
        jit_corpus_writer_->RecordFunction(
            memory_, *guest_function,
            static_cast<uint32_t>(guest_function->machine_code_length()));
      }
    }

    function->set_status(Symbol::Status::kDefined);
    symbol_status = function->status();
  }

  if (symbol_status == Symbol::Status::kFailed) {
    // Symbol likely failed.
    return false;
  }

  return true;
}

bool Processor::Execute(ThreadState* thread_state, uint32_t address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  auto context = thread_state->context();

  // Pad out stack a bit, as some games seem to overwrite the caller by about
  // 16 to 32b.
  context->r[1] -= 64 + 112;

  // This could be set to anything to give us a unique identifier to track
  // re-entrancy/etc.
  uint64_t previous_lr = context->lr;
  context->lr = 0xBCBCBCBC;

  // Execute the function.
  auto result = function->Call(thread_state, uint32_t(context->lr));

  context->lr = previous_lr;
  context->r[1] += 64 + 112;

  return result;
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address) {
  return ExecuteRaw(thread_state, address, 0xBCBCBCBC);
}

bool Processor::ExecuteRaw(ThreadState* thread_state, uint32_t address,
                           uint32_t return_address) {
  SCOPE_profile_cpu_f("cpu");

  // Attempt to get the function.
  auto function = ResolveFunction(address);
  if (!function) {
    // Symbol not found in any module.
    XELOGCPU("Execute({:08X}): failed to find function", address);
    return false;
  }

  return function->Call(thread_state, return_address);
}

uint64_t Processor::Execute(ThreadState* thread_state, uint32_t address,
                            uint64_t args[], size_t arg_count) {
  SCOPE_profile_cpu_f("cpu");

  auto context = thread_state->context();
  for (size_t i = 0; i < std::min(arg_count, static_cast<size_t>(8)); ++i) {
    context->r[3 + i] = args[i];
  }

  if (arg_count > 7) {
    // Rest of the arguments go on the stack.
    // FIXME: This assumes arguments are 32 bits!
    auto stack_arg_base =
        memory()->TranslateVirtual((uint32_t)context->r[1] + 0x54 - (64 + 112));
    for (size_t i = 0; i < arg_count - 8; i++) {
      xe::store_and_swap<uint32_t>(stack_arg_base + (i * 8),
                                   (uint32_t)args[i + 8]);
    }
  }

  if (!Execute(thread_state, address)) {
    return 0xDEADBABE;
  }
  return context->r[3];
}

bool Processor::Save(ByteStream* stream) {
  stream->Write(kProcessorSaveSignature);
  return true;
}

bool Processor::Restore(ByteStream* stream) {
  if (stream->Read<uint32_t>() != kProcessorSaveSignature) {
    XELOGE("Processor::Restore - Invalid magic value!");
    return false;
  }

  // Clear cached thread data for zombie threads.
  std::vector<uint32_t> to_delete;
  for (auto& it : thread_debug_infos_) {
    if (it.second->state == ThreadDebugInfo::State::kZombie) {
      it.second->thread_handle = 0;
      to_delete.push_back(it.first);
    }
  }
  for (uint32_t thread_id : to_delete) {
    thread_debug_infos_.erase(thread_id);
  }

  return true;
}

// Address space only, committed as functions reserve offsets. 8 bytes per
// guest instruction, so this covers 32M translated instructions.
static constexpr size_t kTraceCountsArenaSize = 256_MiB;

bool Processor::CommitTraceCountsLocked(size_t required) {
  size_t target = xe::round_up(required, xe::memory::allocation_granularity());
  if (target <= trace_counts_committed_) {
    return true;
  }
  size_t length = target - trace_counts_committed_;
  for (auto& arena : trace_counts_arenas_) {
    if (!xe::memory::AllocFixed(arena.base + trace_counts_committed_, length,
                                xe::memory::AllocationType::kCommit,
                                xe::memory::PageAccess::kReadWrite)) {
      return false;
    }
  }
  trace_counts_committed_ = target;
  return true;
}

void Processor::FoldTraceCountsLocked(uint8_t* arena) {
  for (auto& region : trace_counts_regions_) {
    auto counts = reinterpret_cast<uint64_t*>(arena + region.offset);
    for (size_t i = 0; i < region.count; ++i) {
      if (counts[i]) {
        region.retired[i] += counts[i];
        counts[i] = 0;
      }
    }
  }
}

uint8_t* Processor::ReserveTraceCountsArenaLocked(uint32_t thread_id) {
  auto base = reinterpret_cast<uint8_t*>(xe::memory::AllocFixed(
      nullptr, kTraceCountsArenaSize, xe::memory::AllocationType::kReserve,
      xe::memory::PageAccess::kNoAccess));
  if (!base) {
    return nullptr;
  }
  // Catch up to what the existing arenas already have committed.
  if (trace_counts_committed_ &&
      !xe::memory::AllocFixed(base, trace_counts_committed_,
                              xe::memory::AllocationType::kCommit,
                              xe::memory::PageAccess::kReadWrite)) {
    xe::memory::DeallocFixed(base, 0, xe::memory::DeallocationType::kRelease);
    return nullptr;
  }
  trace_counts_arenas_.push_back({base, thread_id});
  return base;
}

void Processor::SetTraceCountsArenaThreadLocked(uint8_t* arena,
                                                uint32_t thread_id) {
  for (auto& entry : trace_counts_arenas_) {
    if (entry.base == arena) {
      entry.thread_id = thread_id;
      return;
    }
  }
}

// Threads that cannot get a private arena share the fallback. The emitted
// counters do not null check, so no counting code may be generated until the
// fallback exists, and if it cannot be had coverage stays off for good.
bool Processor::EnsureTraceCountsFallbackLocked() {
  if (trace_counts_fallback_) {
    return true;
  }
  if (trace_counts_failed_) {
    return false;
  }
  trace_counts_fallback_ = ReserveTraceCountsArenaLocked(0);
  if (!trace_counts_fallback_) {
    trace_counts_failed_ = true;
    XELOGW("Unable to reserve an instruction coverage arena, coverage is off");
    return false;
  }
  return true;
}

size_t Processor::AllocateTraceCountsOffset(uint32_t start_address,
                                            uint32_t instruction_count) {
  size_t byte_count = size_t(instruction_count) * 8;
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  if (!EnsureTraceCountsFallbackLocked()) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  size_t offset = trace_counts_next_offset_;
  if (byte_count > kTraceCountsArenaSize - offset) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  if (!CommitTraceCountsLocked(offset + byte_count)) {
    return GuestFunction::kInvalidCoverageOffset;
  }
  trace_counts_next_offset_ = offset + byte_count;
  TraceCountsRegion region;
  region.start_address = start_address;
  region.offset = offset;
  region.count = instruction_count;
  region.retired = std::make_unique<uint64_t[]>(instruction_count);
  trace_counts_regions_.push_back(std::move(region));
  return offset;
}

uint8_t* Processor::AcquireTraceCounts(uint32_t thread_id) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  // Every thread gets an arena even with coverage off, because it can come on
  // at a later title launch and the emitted counters do not null check. Until
  // then the shared one will do, nothing is counting into it.
  if (!EnsureTraceCountsFallbackLocked()) {
    return nullptr;
  }
  if (!trace_counts_enabled_) {
    return trace_counts_fallback_;
  }
  if (!trace_counts_free_.empty()) {
    uint8_t* arena = trace_counts_free_.back();
    trace_counts_free_.pop_back();
    SetTraceCountsArenaThreadLocked(arena, thread_id);
    return arena;
  }
  uint8_t* arena = ReserveTraceCountsArenaLocked(thread_id);
  if (!arena) {
    XELOGW("Sharing the fallback instruction coverage arena, counts will race");
    return trace_counts_fallback_;
  }
  return arena;
}

void Processor::RecordSequenceSamples(
    uint32_t start_address, std::vector<backend::SequenceSample> samples) {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  // Searched from the back: the translator reserves a function's region
  // immediately before handing it to the backend, so it is normally the last.
  for (auto it = trace_counts_regions_.rbegin();
       it != trace_counts_regions_.rend(); ++it) {
    if (it->start_address == start_address) {
      it->samples = std::move(samples);
      return;
    }
  }
}

void Processor::ResetTraceCounts() {
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  for (auto& arena : trace_counts_arenas_) {
    std::memset(arena.base, 0, trace_counts_committed_);
  }
  for (auto& region : trace_counts_regions_) {
    std::memset(region.retired.get(), 0, region.count * sizeof(uint64_t));
  }
}

void Processor::ReleaseTraceCounts(uint8_t* arena) {
  if (!arena) {
    return;
  }
  std::lock_guard<std::mutex> lock(trace_counts_mutex_);
  if (arena == trace_counts_fallback_) {
    // Still shared with other threads, folded at teardown instead.
    return;
  }
  FoldTraceCountsLocked(arena);
  SetTraceCountsArenaThreadLocked(arena, 0);
  trace_counts_free_.push_back(arena);
}

void Processor::OnFunctionDefined(Function* function) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address_type() == Breakpoint::AddressType::kGuest) {
      if (function->ContainsAddress(breakpoint->guest_address())) {
        if (breakpoint->is_installed()) {
          backend_->InstallBreakpoint(breakpoint, function);
        }
      }
    }
  }
}

void Processor::OnThreadCreated(uint32_t thread_handle,
                                ThreadState* thread_state, Thread* thread) {
  auto global_lock = global_critical_region_.Acquire();
  auto thread_info = std::make_unique<ThreadDebugInfo>();
  thread_info->thread_id = thread_state->thread_id();
  thread_info->thread = thread;
  thread_info->state = ThreadDebugInfo::State::kAlive;
  thread_info->suspended = false;
  thread_info->thread_handle = thread_handle;
  thread_debug_infos_.emplace(thread_info->thread_id, std::move(thread_info));
}

void Processor::OnThreadExit(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    XELOGW("Processor::OnThreadExit ignored unknown thread {:08X}", thread_id);
    return;
  }
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kExited;
}

void Processor::OnThreadDestroyed(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    XELOGW("Processor::OnThreadDestroyed ignored unknown thread {:08X}",
           thread_id);
    return;
  }
  it->second->thread_handle = 0;
  thread_debug_infos_.erase(it);
}

void Processor::OnThreadEnteringWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  thread_info->state = ThreadDebugInfo::State::kWaiting;
}

void Processor::OnThreadLeavingWait(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  assert_true(it != thread_debug_infos_.end());
  auto thread_info = it->second.get();
  if (thread_info->state == ThreadDebugInfo::State::kWaiting) {
    thread_info->state = ThreadDebugInfo::State::kAlive;
  }
}

std::vector<ThreadDebugInfo*> Processor::QueryThreadDebugInfos() {
  auto global_lock = global_critical_region_.Acquire();
  std::vector<ThreadDebugInfo*> result;
  for (auto& it : thread_debug_infos_) {
    result.push_back(it.second.get());
  }
  return result;
}

ThreadDebugInfo* Processor::QueryThreadDebugInfo(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  const auto& it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return nullptr;
  }
  return it->second.get();
}

void Processor::AddBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Add to breakpoints map.
  breakpoints_.push_back(breakpoint);

  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Resume();
  }
}

void Processor::RemoveBreakpoint(Breakpoint* breakpoint) {
  auto global_lock = global_critical_region_.Acquire();

  // Uninstall (if needed).
  if (execution_state_ == ExecutionState::kRunning) {
    breakpoint->Suspend();
  }

  // Remove from breakpoint map.
  auto it = std::ranges::find(breakpoints_, breakpoint);
  breakpoints_.erase(it);
}

Breakpoint* Processor::FindBreakpoint(uint32_t address) {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    if (breakpoint->address() == address) {
      return breakpoint;
    }
  }
  return nullptr;
}

void Processor::set_debug_listener(DebugListener* debug_listener) {
  if (debug_listener == debug_listener_) {
    return;
  }
  if (debug_listener_) {
    // Detach old debug listener.
    debug_listener_->OnDetached();
    debug_listener_ = nullptr;
  }
  if (debug_listener) {
    debug_listener_ = debug_listener;
  } else {
    if (execution_state_ == ExecutionState::kPaused) {
      XELOGI("Debugger detaching while execution is paused; continuing...");
      Continue();
    }
  }
}

void Processor::DemandDebugListener() {
  if (debug_listener_) {
    // Already present.
    debug_listener_->OnFocus();
    return;
  }
  if (!debug_listener_handler_) {
    XELOGE("Debugger demanded a listener but no handler was registered.");
    xe::debugging::Break();
    return;
  }
  set_debug_listener(debug_listener_handler_(this));
}

bool Processor::OnThreadBreakpointHit(Exception* ex) {
  auto global_lock = global_critical_region_.Acquire();

  // Suspend all threads (but ourselves).
  SuspendAllThreads();

  // Lookup thread info block.
  auto it = thread_debug_infos_.find(ThreadState::GetThreadID());
  if (it == thread_debug_infos_.end()) {
    // Not found - exception on a thread we don't know about?
    assert_always("UD2 on a thread we don't track");
    return false;
  }
  auto thread_info = it->second.get();

  // Run through and uninstall all breakpoint UD2s to get us back to a clean
  // state.
  if (execution_state_ != ExecutionState::kStepping) {
    SuspendAllBreakpoints();
  }

  // Update all thread states with their latest values, using the context we
  // got from the exception instead of a sampled value (as it would just show
  // the exception handler).
  UpdateThreadExecutionStates(thread_info->thread_id, ex->thread_context());

  // Walk the captured thread stack and look for breakpoints at any address in
  // the stack. We just look for the first one.
  Breakpoint* breakpoint = nullptr;
  for (size_t i = 0; i < thread_info->frames.size(); ++i) {
    auto& frame = thread_info->frames[i];
    for (auto scan_breakpoint : breakpoints_) {
      if ((scan_breakpoint->address_type() == Breakpoint::AddressType::kGuest &&
           scan_breakpoint->guest_address() == frame.guest_pc) ||
          (scan_breakpoint->address_type() == Breakpoint::AddressType::kHost &&
           scan_breakpoint->host_address() == frame.host_pc) ||
          scan_breakpoint->ContainsHostAddress(frame.host_pc)) {
        breakpoint = scan_breakpoint;
        break;
      }
    }
    if (breakpoint) {
      breakpoint->OnHit(thread_info, frame.host_pc);
      break;
    }
  }

  // We are waiting on the debugger now. Either wait for it to continue, add a
  // new step, or direct us somewhere else.
  // The debugger will ResumeAllThreads or just resume us (depending on what
  // it wants to do).
  execution_state_ = ExecutionState::kPaused;
  thread_info->suspended = true;

  // Must unlock, or we will deadlock.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  thread_info->thread->thread()->Suspend();

  // Apply thread context changes.
  // TODO(benvanik): apply to all threads?
#if XE_ARCH_AMD64
  ex->set_resume_pc(thread_info->host_context.rip);
#elif XE_ARCH_ARM64
  ex->set_resume_pc(thread_info->host_context.pc);
#else
#error Instruction pointer not specified for the target CPU architecture.
#endif  // XE_ARCH

  // Resume execution.
  return true;
}

void Processor::OnStepCompleted(ThreadDebugInfo* thread_info) {
  auto global_lock = global_critical_region_.Acquire();
  execution_state_ = ExecutionState::kPaused;

  // Unlock before notifying to avoid deadlock with debugger stub.
  global_lock.unlock();

  if (debug_listener_) {
    debug_listener_->OnExecutionPaused();
  }

  // Note that we stay suspended.
}

bool Processor::OnUnhandledException(Exception* ex) {
  // If we have no listener return right away.
  // TODO(benvanik): DemandDebugListener()?
  if (!debug_listener_) {
    return false;
  }

  // Only pause on exceptions when debugging is explicitly enabled.
  // Without --debug flag, let the exception propagate normally.
  if (!cvars::debug) {
    return false;
  }

  // If this isn't a managed thread, fail - let VS handle it for now.
  if (!Thread::IsInThread()) {
    return false;
  }

  auto global_lock = global_critical_region_.Acquire();

  // Suspend all guest threads (but this one).
  SuspendAllThreads();

  UpdateThreadExecutionStates(Thread::GetCurrentThreadId(),
                              ex->thread_context());

  // Stop and notify the listener.
  if (execution_state_ != ExecutionState::kRunning) {
    global_lock.unlock();
    Thread::GetCurrentThread()->thread()->Suspend();
    return true;
  }
  execution_state_ = ExecutionState::kPaused;

  // Notify debugger that execution stopped.
  debug_listener_->OnUnhandledException(ex);
  debug_listener_->OnExecutionPaused();

  // Unlock before suspending to avoid deadlock with debugger stub.
  global_lock.unlock();
  Thread::GetCurrentThread()->thread()->Suspend();

  return true;
}

void Processor::ShowDebugger() {
  if (debug_listener_) {
    debug_listener_->OnFocus();
  } else {
    DemandDebugListener();
  }
}

bool Processor::SuspendAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (thread_info->suspended) {
      // Already suspended - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be suspended - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't suspend ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    bool did_suspend = thread->thread()->Suspend(nullptr);
    assert_true(did_suspend);
    thread_info->suspended = true;
  }
  return true;
}

bool Processor::ResumeThread(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  auto it = thread_debug_infos_.find(thread_id);
  if (it == thread_debug_infos_.end()) {
    return false;
  }
  auto thread_info = it->second.get();
  assert_true(thread_info->suspended);
  assert_false(thread_info->state == ThreadDebugInfo::State::kExited ||
               thread_info->state == ThreadDebugInfo::State::kZombie);
  thread_info->suspended = false;
  auto thread = thread_info->thread;
  return thread->thread()->Resume();
}

bool Processor::ResumeAllThreads() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    if (!thread_info->suspended) {
      // Not suspended by us - ignore.
      continue;
    } else if (thread_info->state == ThreadDebugInfo::State::kZombie ||
               thread_info->state == ThreadDebugInfo::State::kExited) {
      // Thread is dead and cannot be resumed - ignore.
      continue;
    } else if (Thread::IsInThread() &&
               thread_info->thread_id == Thread::GetCurrentThreadId()) {
      // Can't resume ourselves.
      continue;
    }
    auto thread = thread_info->thread;
    if (!thread->can_debugger_suspend()) {
      // Thread is a host thread, and we aren't suspending those (for now).
      continue;
    }
    thread_info->suspended = false;
    bool did_resume = thread->thread()->Resume();
    assert_true(did_resume);
  }
  return true;
}

void Processor::UpdateThreadExecutionStates(
    uint32_t override_thread_id, HostThreadContext* override_context) {
  auto global_lock = global_critical_region_.Acquire();
  uint64_t frame_host_pcs[64];
  xe::cpu::StackFrame cpu_frames[64];
  for (auto& it : thread_debug_infos_) {
    auto thread_info = it.second.get();
    auto thread = thread_info->thread;
    if (!thread) {
      continue;
    }

    // Grab PPC context.
    // Note that this is only up to date if --store_all_context_values is
    // enabled (or --debug).
    if (thread->can_debugger_suspend()) {
      std::memcpy(&thread_info->guest_context,
                  thread->thread_state()->context(),
                  sizeof(thread_info->guest_context));
    }

    // Grab stack trace and X64 context then resolve all symbols.
    uint64_t hash;
    HostThreadContext* in_host_context = nullptr;
    if (override_thread_id == thread_info->thread_id) {
      // If we were passed an override context we use that. Otherwise, ask the
      // stack walker for a new context.
      in_host_context = override_context;
    }
    size_t count = stack_walker_->CaptureStackTrace(
        thread->thread()->native_handle(), frame_host_pcs, 0,
        xe::countof(frame_host_pcs), in_host_context,
        &thread_info->host_context, &hash);
    stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
    thread_info->frames.resize(count);
    for (size_t i = 0; i < count; ++i) {
      auto& cpu_frame = cpu_frames[i];
      auto& frame = thread_info->frames[i];
      frame.host_pc = cpu_frame.host_pc;
      frame.host_function_address = cpu_frame.host_symbol.address;
      frame.guest_pc = cpu_frame.guest_pc;
      frame.guest_function_address = 0;
      frame.guest_function = nullptr;
      auto function = cpu_frame.guest_symbol.function;
      if (cpu_frame.type == cpu::StackFrame::Type::kGuest && function) {
        frame.guest_function_address = function->address();
        frame.guest_function = function;
      } else {
        std::strncpy(frame.name, cpu_frame.host_symbol.name,
                     xe::countof(frame.name));
        frame.name[xe::countof(frame.name) - 1] = 0;
      }
    }
  }
}

void Processor::SuspendAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Suspend();
  }
}

void Processor::ResumeAllBreakpoints() {
  auto global_lock = global_critical_region_.Acquire();
  for (auto breakpoint : breakpoints_) {
    breakpoint->Resume();
  }
}

void Processor::Pause() {
  {
    auto global_lock = global_critical_region_.Acquire();
    assert_true(execution_state_ == ExecutionState::kRunning);
    SuspendAllThreads();
    SuspendAllBreakpoints();
    UpdateThreadExecutionStates();
    execution_state_ = ExecutionState::kPaused;
    if (debug_listener_) {
      debug_listener_->OnExecutionPaused();
    }
  }
  DemandDebugListener();
}

void Processor::Continue() {
  auto global_lock = global_critical_region_.Acquire();
  if (execution_state_ == ExecutionState::kRunning) {
    return;
  } else if (execution_state_ == ExecutionState::kStepping) {
    assert_always("cancel stepping not done yet");
  }
  execution_state_ = ExecutionState::kRunning;
  ResumeAllBreakpoints();
  ResumeAllThreads();

  if (debug_listener_) {
    debug_listener_->OnExecutionContinued();
  }
}

void Processor::StepHostInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint64_t new_host_pc = backend_->CalculateNextHostInstruction(
      thread_info, thread_info->frames[0].host_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kHost, new_host_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));

  // Add to front of breakpoints map, so this should get evaluated first
  breakpoints_.insert(breakpoints_.begin(), thread_info->step_breakpoint.get());

  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

void Processor::StepGuestInstruction(uint32_t thread_id) {
  auto global_lock = global_critical_region_.Acquire();
  assert_true(execution_state_ == ExecutionState::kPaused);
  execution_state_ = ExecutionState::kStepping;

  auto thread_info = QueryThreadDebugInfo(thread_id);

  uint32_t next_pc = CalculateNextGuestInstruction(
      thread_info, thread_info->frames[0].guest_pc);

  assert_null(thread_info->step_breakpoint.get());
  thread_info->step_breakpoint.reset(
      new Breakpoint(this, Breakpoint::AddressType::kGuest, next_pc,
                     [this, thread_info](Breakpoint* breakpoint,
                                         ThreadDebugInfo* breaking_thread_info,
                                         uint64_t host_address) {
                       if (thread_info != breaking_thread_info) {
                         assert_always("Step in another thread?");
                       }
                       // Our step request has completed. Remove the breakpoint
                       // and fire event.
                       breakpoint->Suspend();
                       RemoveBreakpoint(breakpoint);
                       thread_info->step_breakpoint.reset();
                       OnStepCompleted(thread_info);
                     }));

  // Add to front of breakpoints map, so this should get evaluated first
  breakpoints_.insert(breakpoints_.begin(), thread_info->step_breakpoint.get());

  thread_info->step_breakpoint->Resume();

  // ResumeAllBreakpoints();
  ResumeThread(thread_id);
}

bool Processor::StepToGuestAddress(uint32_t thread_id, uint32_t pc) {
  auto functions = FindFunctionsWithAddress(pc);
  if (functions.empty()) {
    // Function hasn't been generated yet. Generate it.
    if (!ResolveFunction(pc)) {
      XELOGE(
          "Processor::StepToAddress({:08X}) - Function could not be resolved",
          pc);
      return false;
    }
  }

  // Instruct the thread to step forwards.
  threading::Fence fence;
  cpu::Breakpoint bp(
      this, Breakpoint::AddressType::kGuest, pc,
      [&fence](Breakpoint* breakpoint, ThreadDebugInfo* thread_info,
               uint64_t host_address) { fence.Signal(); });
  bp.Resume();

  // HACK
  auto thread_info = QueryThreadDebugInfo(thread_id);
  uint32_t suspend_count = 1;
  while (suspend_count) {
    thread_info->thread->thread()->Resume(&suspend_count);
  }

  fence.Wait();
  bp.Suspend();

  return true;
}

uint32_t Processor::StepIntoGuestBranchTarget(uint32_t thread_id, uint32_t pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = pc;
  d.code = xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);

  // Must be on a branch.
  assert_true(xe::cpu::ppc::GetOpcodeInfo(opcode).group ==
              xe::cpu::ppc::PPCOpcodeGroup::kB);

  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;
  auto context = thread->thread_state()->context();

  if (d.code == 0x4E800020) {
    // blr
    uint32_t nia = uint32_t(context->lr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (d.code == 0x4E800420) {
    // bctr
    uint32_t nia = uint32_t(context->ctr);
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bx) {
    // bx
    uint32_t nia = d.I.ADDR();
    StepToGuestAddress(thread_id, nia);
    pc = nia;
  } else if (opcode == PPCOpcode::bcx || opcode == PPCOpcode::bcctrx ||
             opcode == PPCOpcode::bclrx) {
    threading::Fence fence;
    auto callback = [&fence, &pc](Breakpoint* breakpoint,
                                  ThreadDebugInfo* thread_info,
                                  uint64_t host_address) {
      pc = breakpoint->guest_address();
      fence.Signal();
    };

    cpu::Breakpoint bpf(this, Breakpoint::AddressType::kGuest, pc + 4,
                        callback);
    bpf.Resume();

    uint32_t nia = 0;
    if (opcode == PPCOpcode::bcx) {
      // bcx
      nia = d.B.ADDR();
    } else if (opcode == PPCOpcode::bcctrx) {
      // bcctrx
      nia = uint32_t(context->ctr);
    } else if (opcode == PPCOpcode::bclrx) {
      // bclrx
      nia = uint32_t(context->lr);
    }

    cpu::Breakpoint bpt(this, Breakpoint::AddressType::kGuest, nia, callback);
    bpt.Resume();

    // HACK
    uint32_t suspend_count = 1;
    while (suspend_count) {
      thread->thread()->Resume(&suspend_count);
    }

    fence.Wait();
    bpt.Suspend();
    bpf.Suspend();
  }

  return pc;
}

uint32_t Processor::StepToGuestSafePoint(uint32_t thread_id, bool ignore_host) {
  // This cannot be done if we're the calling thread!
  if (thread_id == ThreadState::GetThreadID()) {
    assert_always(
        "Processor::StepToSafePoint(): target thread is the calling thread!");
    return 0;
  }
  auto thread_info = QueryThreadDebugInfo(thread_id);
  auto thread = thread_info->thread;

  // Now the fun part begins: Registers are only guaranteed to be synchronized
  // with the PPC context at a basic block boundary. Unfortunately, we most
  // likely stopped the thread at some point other than a boundary. We need to
  // step forward until we reach a boundary, and then perform the save.
  uint64_t frame_host_pcs[64];
  cpu::StackFrame cpu_frames[64];
  size_t count = stack_walker_->CaptureStackTrace(
      thread->thread()->native_handle(), frame_host_pcs, 0,
      xe::countof(frame_host_pcs), nullptr, nullptr);
  stack_walker_->ResolveStack(frame_host_pcs, cpu_frames, count);
  if (count == 0) {
    return 0;
  }

  auto& first_frame = cpu_frames[0];
  if (ignore_host) {
    for (size_t i = 0; i < count; i++) {
      if (cpu_frames[i].type == cpu::StackFrame::Type::kGuest &&
          cpu_frames[i].guest_pc) {
        first_frame = cpu_frames[i];
        break;
      }
    }
  }

  // Check if we're in guest code or host code.
  uint32_t pc = 0;
  if (first_frame.type == cpu::StackFrame::Type::kGuest) {
    auto& frame = first_frame;
    if (!frame.guest_pc) {
      // Lame. The guest->host thunk is a "guest" function.
      frame = cpu_frames[1];
    }

    pc = frame.guest_pc;

    // We're in guest code.
    // First: Find a synchronizing instruction and go to it.
    xe::cpu::ppc::PPCDecodeData d;
    const xe::cpu::ppc::PPCOpcodeInfo* sync_info = nullptr;
    d.address = cpu_frames[0].guest_pc - 4;
    do {
      d.address += 4;
      d.code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(d.address));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(d.code);
      if (opcode_info.type == cpu::ppc::PPCOpcodeType::kSync) {
        sync_info = &opcode_info;
        break;
      }
    } while (true);

    if (d.address != pc) {
      StepToGuestAddress(thread_id, d.address);
      pc = d.address;
    }

    // Okay. Now we're on a synchronizing instruction but we need to step
    // past it in order to get a synchronized context.
    // If we're on a branching instruction, it's guaranteed only going to have
    // two possible targets. For non-branching instructions, we can just step
    // over them.
    if (sync_info->group == xe::cpu::ppc::PPCOpcodeGroup::kB) {
      pc = StepIntoGuestBranchTarget(thread_id, d.address);
    }
  } else {
    // We're in host code. Search backwards til we can get an idea of where
    // we are.
    cpu::GuestFunction* thunk_func = nullptr;
    cpu::Export* export_data = nullptr;
    uint32_t first_pc = 0;
    for (int i = 0; i < count; i++) {
      auto& frame = cpu_frames[i];
      if (frame.type == cpu::StackFrame::Type::kGuest && frame.guest_pc) {
        auto func = frame.guest_symbol.function;
        assert_true(func->is_guest());

        if (!first_pc) {
          first_pc = frame.guest_pc;
        }

        thunk_func = reinterpret_cast<cpu::GuestFunction*>(func);
        export_data = thunk_func->export_data();
        if (export_data) {
          break;
        }
      }
    }

    // If the export is blocking, we wrap up and save inside the export thunk.
    // When we're restored, we'll call the blocking export again.
    // Otherwise, we return from the thunk and save.
    if (export_data && export_data->tags & cpu::ExportTag::kBlocking) {
      pc = thunk_func->address();
    } else if (export_data) {
      // Non-blocking. Run until we return from the thunk.
      pc = static_cast<uint32_t>(thread->thread_state()->context()->lr);
      StepToGuestAddress(thread_id, pc);
    } else if (first_pc) {
      // We're in the MMIO handler/mfmsr/something calling out of the guest
      // that doesn't use an export. If the current instruction is
      // synchronizing, we can just save here. Otherwise, step forward
      // (and call ourselves again so we run the correct logic).
      uint32_t code =
          xe::load_and_swap<uint32_t>(memory()->TranslateVirtual(first_pc));
      auto& opcode_info = xe::cpu::ppc::LookupOpcodeInfo(code);
      if (opcode_info.type == xe::cpu::ppc::PPCOpcodeType::kSync) {
        // Good to go.
        pc = first_pc;
      } else {
        // Step forward and run this logic again.
        StepToGuestAddress(thread_id, first_pc + 4);
        return StepToGuestSafePoint(thread_id, true);
      }
    } else {
      // We've managed to catch a thread before it called into the guest.
      // Set a breakpoint on its startup procedure and capture it there.
      // TODO(DrChat): Reimplement
      assert_always("Unimplemented");
      /*
      auto creation_params = thread->creation_params();
      pc = creation_params->xapi_thread_startup
               ? creation_params->xapi_thread_startup
               : creation_params->start_address;
      StepToGuestAddress(thread_id, pc);
      */
    }
  }

  return pc;
}

bool TestPpcCondition(const xe::cpu::ppc::PPCContext* context, uint32_t bo,
                      uint32_t bi, bool check_ctr, bool check_cond) {
  bool ctr_ok = true;
  if (check_ctr) {
    if (select_bits(bo, 2, 2)) {
      ctr_ok = true;
    } else {
      uint32_t new_ctr_value = static_cast<uint32_t>(context->ctr - 1);
      if (select_bits(bo, 1, 1)) {
        ctr_ok = new_ctr_value == 0;
      } else {
        ctr_ok = new_ctr_value != 0;
      }
    }
  }
  bool cond_ok = true;
  if (check_cond) {
    if (select_bits(bo, 4, 4)) {
      cond_ok = true;
    } else {
      uint8_t cr = *(reinterpret_cast<const uint8_t*>(&context->cr0) +
                     (4 * (bi >> 2)) + (bi & 3));
      if (select_bits(bo, 3, 3)) {
        cond_ok = cr != 0;
      } else {
        cond_ok = cr == 0;
      }
    }
  }
  return ctr_ok && cond_ok;
}

uint32_t Processor::CalculateNextGuestInstruction(ThreadDebugInfo* thread_info,
                                                  uint32_t current_pc) {
  xe::cpu::ppc::PPCDecodeData d;
  d.address = current_pc;
  d.code = xe::load_and_swap<uint32_t>(memory_->TranslateVirtual(d.address));
  auto opcode = xe::cpu::ppc::LookupOpcode(d.code);
  if (d.code == 0x4E800020) {
    // blr -- unconditional branch to LR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    return target_pc;
  } else if (d.code == 0x4E800420) {
    // bctr -- unconditional branch to CTR.
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    return target_pc;
  } else if (opcode == PPCOpcode::bx) {
    // b/ba/bl/bla
    uint32_t target_pc = d.I.ADDR();
    return target_pc;
  } else if (opcode == PPCOpcode::bcx) {
    // bc/bca/bcl/bcla
    uint32_t target_pc = d.B.ADDR();
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.B.BO(),
                                        d.B.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bclrx) {
    // bclr/bclrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.lr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), true, true);
    return test_passed ? target_pc : current_pc + 4;
  } else if (opcode == PPCOpcode::bcctrx) {
    // bcctr/bcctrl
    uint32_t target_pc = static_cast<uint32_t>(thread_info->guest_context.ctr);
    bool test_passed = TestPpcCondition(&thread_info->guest_context, d.XL.BO(),
                                        d.XL.BI(), false, true);
    return test_passed ? target_pc : current_pc + 4;
  } else {
    return current_pc + 4;
  }
}
// These run a real lwarx/stwcx. loop, so they cancel any guest thread's
// reservation on the same granule. Each returns the pre-update value.
// context must be the calling thread's, it holds the reservation.
uint32_t Processor::GuestAtomicIncrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result + 1));
  return result;
}
uint32_t Processor::GuestAtomicDecrement32(ppc::PPCContext* context,
                                           uint32_t guest_address) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result - 1));
  return result;
}

uint32_t Processor::GuestAtomicOr32(ppc::PPCContext* context,
                                    uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result | mask));
  return result;
}
uint32_t Processor::GuestAtomicXor32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result ^ mask));
  return result;
}
uint32_t Processor::GuestAtomicAnd32(ppc::PPCContext* context,
                                     uint32_t guest_address, uint32_t mask) {
  uint32_t result;
  do {
    result = backend()->ReservedLoad32(context, guest_address);
  } while (!backend()->ReservedStore32(context, guest_address, result & mask));
  return result;
}

// Does not retry. A lost reservation reports a failed exchange, which is what
// a guest compare-exchange loop sees.
bool Processor::GuestAtomicCAS32(ppc::PPCContext* context, uint32_t old_value,
                                 uint32_t new_value, uint32_t guest_address) {
  if (backend()->ReservedLoad32(context, guest_address) != old_value) {
    return false;
  }
  return backend()->ReservedStore32(context, guest_address, new_value);
}
}  // namespace cpu
}  // namespace xe
