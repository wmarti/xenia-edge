/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/gpu/pm4_marker_sink.h"
#include "xenia/base/platform.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <limits>
#include <utility>

namespace xe {
namespace gpu {
namespace {

thread_local Pm4MarkerDispatcher* active_marker_dispatcher = nullptr;

}  // namespace

Pm4MarkerDispatchLease::~Pm4MarkerDispatchLease() {
  if (dispatcher_ && ticket_live_) {
    dispatcher_->CompleteLease(this, 0, false);
  }
}

Pm4MarkerDispatchLease::Pm4MarkerDispatchLease(
    Pm4MarkerDispatchLease&& other) noexcept {
  *this = std::move(other);
}

Pm4MarkerDispatchLease& Pm4MarkerDispatchLease::operator=(
    Pm4MarkerDispatchLease&& other) noexcept {
  if (this != &other) {
    if (dispatcher_ && ticket_live_) {
      dispatcher_->CompleteLease(this, 0, false);
    }
    dispatcher_ = other.dispatcher_;
    sink_ = std::move(other.sink_);
    event_ = other.event_;
    ticket_ = other.ticket_;
    ticket_live_ = other.ticket_live_;
    other.Reset();
  }
  return *this;
}

void Pm4MarkerDispatchLease::Reset() noexcept {
  dispatcher_ = nullptr;
  sink_.reset();
  event_ = {};
  ticket_ = 0;
  ticket_live_ = false;
}

bool Pm4MarkerDispatcher::AttachSink(std::shared_ptr<Pm4MarkerSink> sink) {
  if (!sink) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      return false;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::unique_lock<std::mutex> lock(mutex_);
  dispatch_condition_.wait(
      lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
  bool attached = false;
  if (sink_ || held_sink_ || sink_failed_ || shut_down_ ||
      sink_generation_ == std::numeric_limits<uint64_t>::max()) {
    attached = false;
  } else {
    ++sink_generation_;
    sink_ = std::move(sink);
    admission_open_.store(true, std::memory_order_release);
    attached = true;
  }
  ++serving_admission_ticket_;
  lock.unlock();
  dispatch_condition_.notify_all();
  return attached;
}

bool Pm4MarkerDispatcher::HoldSink(const std::shared_ptr<Pm4MarkerSink>& sink,
                                   Pm4MarkerHoldToken* token) {
  if (!sink || !token) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      return false;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::unique_lock<std::mutex> lock(mutex_);
  dispatch_condition_.wait(
      lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
  bool held = false;
  if (held_sink_ == sink) {
    if (!sink_failed_ && !source_advanced_while_held_) {
      *token = hold_token_;
      held = true;
    }
  } else {
    const uint64_t hold_epoch = hold_epoch_.load(std::memory_order_relaxed);
    if (sink_ == sink && !held_sink_ && !shut_down_ &&
        hold_epoch != std::numeric_limits<uint64_t>::max()) {
      admission_open_.store(false, std::memory_order_release);
      hold_epoch_.store(hold_epoch + 1, std::memory_order_release);
      hold_token_.sink_generation = sink_generation_;
      hold_token_.hold_epoch = hold_epoch + 1;
      hold_token_.last_ordinal = marker_count_.load(std::memory_order_relaxed);
      held_sink_ = std::move(sink_);
      *token = hold_token_;
      held = true;
    }
  }
  ++serving_admission_ticket_;
  lock.unlock();
  dispatch_condition_.notify_all();
  return held;
}

bool Pm4MarkerDispatcher::ResumeSink(const std::shared_ptr<Pm4MarkerSink>& sink,
                                     const Pm4MarkerHoldToken& token) {
  if (!sink || !token) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      return false;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::unique_lock<std::mutex> lock(mutex_);
  dispatch_condition_.wait(
      lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
  bool resumed = false;
  if (held_sink_ != sink || sink_ || token != hold_token_ || shut_down_ ||
      sink_failed_ || source_advanced_while_held_ ||
      marker_count_.load(std::memory_order_relaxed) != token.last_ordinal ||
      sink_generation_ == std::numeric_limits<uint64_t>::max()) {
    resumed = false;
  } else {
    ++sink_generation_;
    sink_ = std::move(held_sink_);
    hold_token_ = {};
    admission_open_.store(true, std::memory_order_release);
    resumed = true;
  }
  ++serving_admission_ticket_;
  lock.unlock();
  dispatch_condition_.notify_all();
  return resumed;
}

bool Pm4MarkerDispatcher::SealAndDetachHeldSink(
    const std::shared_ptr<Pm4MarkerSink>& sink,
    const Pm4MarkerHoldToken& token) {
  if (!sink || !token) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      return false;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::unique_lock<std::mutex> lock(mutex_);
  dispatch_condition_.wait(
      lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
  bool detached = false;
  if (!sink_ && held_sink_ == sink && token == hold_token_ && !shut_down_ &&
      !sink_failed_ && !source_advanced_while_held_ &&
      marker_count_.load(std::memory_order_relaxed) == token.last_ordinal) {
    held_sink_.reset();
    hold_token_ = {};
    detached = true;
  }
  ++serving_admission_ticket_;
  lock.unlock();
  dispatch_condition_.notify_all();
  return detached;
}

bool Pm4MarkerDispatcher::DetachSink(
    const std::shared_ptr<Pm4MarkerSink>& sink) {
  if (!sink) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      return false;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::unique_lock<std::mutex> lock(mutex_);
  dispatch_condition_.wait(
      lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
  bool detached = false;
  if (sink_ == sink || held_sink_ == sink) {
    admission_open_.store(false, std::memory_order_release);
    sink_.reset();
    held_sink_.reset();
    hold_token_ = {};
    detached = true;
  }
  ++serving_admission_ticket_;
  lock.unlock();
  dispatch_condition_.notify_all();
  return detached;
}

bool Pm4MarkerDispatcher::sink_failed() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return sink_failed_;
}

Pm4MarkerDispatcherStatus Pm4MarkerDispatcher::status() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Pm4MarkerDispatcherStatus result;
  result.marker_count = marker_count_.load(std::memory_order_relaxed);
  result.sink_generation = sink_generation_;
  result.hold_epoch = hold_epoch_.load(std::memory_order_relaxed);
  result.sink_attached = static_cast<bool>(sink_);
  result.sink_held = static_cast<bool>(held_sink_);
  result.sink_failed = sink_failed_;
  result.source_advanced_while_held = source_advanced_while_held_;
  result.shut_down = shut_down_;
  return result;
}

Pm4MarkerDispatchLease Pm4MarkerDispatcher::BeginPm4Swap() noexcept {
  Pm4MarkerDispatchLease lease;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_marker_dispatcher == this ||
        (dispatching_ && dispatch_thread_ == std::this_thread::get_id())) {
      const uint64_t last = marker_count_.load(std::memory_order_relaxed);
      if (last != std::numeric_limits<uint64_t>::max()) {
        marker_count_.store(last + 1, std::memory_order_release);
      }
      sink_failed_ = true;
      return lease;
    }
  }
  // Taking the ticket is the semantic source occurrence. CompletePm4Swap keeps
  // this ticket live through IssueSwap and the callback, so a later Hold cannot
  // close admission in the middle of one guest swap.
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  Pm4MarkerEvent event;
  event.source = Pm4MarkerSource::kPm4Swap;
  event.opcode = kPm4SwapMarkerOpcode;

  std::shared_ptr<Pm4MarkerSink> sink;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    dispatch_condition_.wait(
        lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
    const uint64_t last = marker_count_.load(std::memory_order_relaxed);
    if (last == std::numeric_limits<uint64_t>::max()) {
      sink_failed_ = true;
      if (held_sink_) {
        source_advanced_while_held_ = true;
      }
      ++serving_admission_ticket_;
      lock.unlock();
      dispatch_condition_.notify_all();
      return lease;
    }
    event.ordinal = last + 1;
    marker_count_.store(event.ordinal, std::memory_order_release);
    if (post_ordinal_assignment_hook_) {
      post_ordinal_assignment_hook_(post_ordinal_assignment_context_);
    }
    // Admission is decided only when this ticket reaches service. Snapshotting
    // it before service lets an earlier Attach or Resume falsely poison a
    // later occurrence.
    if (held_sink_) {
      if (sink_ || held_sink_) {
        sink_failed_ = true;
        source_advanced_while_held_ = true;
      }
      ++serving_admission_ticket_;
      lock.unlock();
      dispatch_condition_.notify_all();
      return lease;
    }
    if (!sink_ || !admission_open_.load(std::memory_order_relaxed) ||
        sink_failed_ || shut_down_) {
      ++serving_admission_ticket_;
      lock.unlock();
      dispatch_condition_.notify_all();
      return lease;
    }
    event.sink_generation = sink_generation_;
    sink = sink_;
    dispatching_ = true;
    dispatch_thread_ = std::this_thread::get_id();
  }

  lease.dispatcher_ = this;
  lease.sink_ = std::move(sink);
  lease.event_ = event;
  lease.ticket_ = ticket;
  lease.ticket_live_ = true;
  return lease;
}

void Pm4MarkerDispatcher::CompleteLease(Pm4MarkerDispatchLease* lease,
                                        uint64_t host_tick,
                                        bool deliver) noexcept {
  if (!lease || lease->dispatcher_ != this || !lease->ticket_live_) {
    return;
  }
  lease->event_.host_tick = host_tick;

  Pm4MarkerDispatcher* previous_dispatcher = active_marker_dispatcher;
  active_marker_dispatcher = this;
  const bool accepted = deliver && lease->sink_->OnPm4Marker(lease->event_);
  active_marker_dispatcher = previous_dispatcher;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatching_ = false;
    dispatch_thread_ = {};
    if (!accepted) {
      sink_failed_ = true;
    }
    ++serving_admission_ticket_;
  }
  lease->Reset();
  dispatch_condition_.notify_all();
}

void Pm4MarkerDispatcher::CompletePm4Swap(Pm4MarkerDispatchLease lease,
                                          uint64_t host_tick) noexcept {
  CompleteLease(&lease, host_tick, true);
}

void Pm4MarkerDispatcher::NotifyPm4Swap(uint64_t host_tick) noexcept {
  CompletePm4Swap(BeginPm4Swap(), host_tick);
}

void Pm4MarkerDispatcher::Shutdown() noexcept {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dispatching_ && dispatch_thread_ == std::this_thread::get_id()) {
      sink_failed_ = true;
      return;
    }
  }
  const uint64_t ticket =
      next_admission_ticket_.fetch_add(1, std::memory_order_acq_rel);
  RunPostTicketAssignmentHook(ticket);
  std::shared_ptr<Pm4MarkerSink> sink;
  {
    std::unique_lock<std::mutex> lock(mutex_);
    dispatch_condition_.wait(
        lock, [this, ticket]() { return serving_admission_ticket_ == ticket; });
    if (!shut_down_) {
      shut_down_ = true;
      admission_open_.store(false, std::memory_order_release);
      sink = sink_ ? std::move(sink_) : std::move(held_sink_);
      hold_token_ = {};
    }
    if (sink) {
      dispatching_ = true;
      dispatch_thread_ = std::this_thread::get_id();
    } else {
      ++serving_admission_ticket_;
    }
  }
  dispatch_condition_.notify_all();
  if (sink) {
    Pm4MarkerDispatcher* previous_dispatcher = active_marker_dispatcher;
    active_marker_dispatcher = this;
    sink->OnPm4MarkerSourceShutdown();
    active_marker_dispatcher = previous_dispatcher;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dispatching_ = false;
      dispatch_thread_ = {};
      ++serving_admission_ticket_;
    }
    dispatch_condition_.notify_all();
  }
}

void Pm4MarkerDispatcher::SetPostOrdinalAssignmentHookForTesting(
    void (*hook)(void*), void* context) {
  std::lock_guard<std::mutex> lock(mutex_);
  post_ordinal_assignment_hook_ = hook;
  post_ordinal_assignment_context_ = context;
}

void Pm4MarkerDispatcher::SetPostTicketAssignmentHookForTesting(
    void (*hook)(void*, uint64_t), void* context) {
  std::lock_guard<std::mutex> lock(mutex_);
  post_ticket_assignment_hook_ = hook;
  post_ticket_assignment_context_ = context;
}

void Pm4MarkerDispatcher::RunPostTicketAssignmentHook(uint64_t ticket) const {
  void (*hook)(void*, uint64_t) = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    hook = post_ticket_assignment_hook_;
    context = post_ticket_assignment_context_;
  }
  if (hook) {
    hook(context, ticket);
  }
}

}  // namespace gpu
}  // namespace xe

#endif
