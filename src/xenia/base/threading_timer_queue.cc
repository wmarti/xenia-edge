/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <forward_list>

#include "third_party/disruptorplus/include/disruptorplus/blocking_wait_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/multi_threaded_claim_strategy.hpp"
#include "third_party/disruptorplus/include/disruptorplus/ring_buffer.hpp"
#include "third_party/disruptorplus/include/disruptorplus/sequence_barrier.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait.hpp"
#include "third_party/disruptorplus/include/disruptorplus/spin_wait_strategy.hpp"
#include "xenia/base/assert.h"
#include "xenia/base/platform.h"
#include "xenia/base/threading.h"
#include "xenia/base/threading_timer_queue.h"

namespace dp = disruptorplus;

namespace xe {
namespace threading {

using WaitItem = TimerQueueWaitItem;
/*
        chrispy: changed this to a blocking wait from a spin-wait, the spin was
   monopolizing a ton of cpu time (depending on the game 2-4% of total cpu time)
   on my 3990x no complaints since that change
*/

/*
        edit: actually had to change it back, when i was testing it only worked
   because i fixed disruptorplus' code to compile (it gives wrong args to
   condition_variable::wait_until) but now builds

*/

/*
    edit2: (30.12.2024) After uplifting version of MSVC compiler Xenia cannot be
   correctly initialized if you're using proton.
*/
/*
    edit3: (2026-08-25) Measured on macOS/arm64. The spin strategy is the second
   largest CPU consumer in this emulator at a title's main menu:
   xe::threading::TimerQueue is 3.9% of on-core CPU at the Halo Reach menu and
   8.7% at the Halo 3 menu, of which 80% and 72% respectively is the thread
   sitting in a sched_yield trap rather than doing anything. A further 0.50%
   (Reach) and 1.25% (Halo 3) of whole-process on-core CPU is sysctl, because
   spin_wait's constructor calls std::thread::hardware_concurrency() -- an
   uncached syscall on this platform -- and a spin_wait is constructed on every
   wait.

   So chrispy's original reading was right, and the two reasons it was reverted
   do not apply here. The wrong-args-to-wait_until bug is NOT fixed in this
   vendored copy -- blocking_wait_strategy.hpp:119 still passes the predicate
   and the timeout to wait_for in the wrong order -- but that member template
   is never instantiated on this path: the queue waits on a time_point, which
   selects the time_point overload at :172, and that one has its arguments
   right. Anyone who later calls a duration-based wait with this strategy gets
   a compile error, which is the correct failure mode. And edit2's proton/MSVC
   initialisation failure is a Windows problem. Windows therefore keeps
   upstream's behaviour exactly; only POSIX changes.

   KNOWN LIMITATION, macOS only. This class declares clock = steady_clock and
   static_asserts it, but a std::condition_variable timed wait cannot honour
   that here. _LIBCPP_HAS_COND_CLOCKWAIT is 0 on macOS (__config: it needs
   Android API >= 30 or glibc >= 2.30), so wait_until falls back to
   __do_timed_wait -> wait_for, which re-anchors on system_clock::now() and
   hands a CLOCK_REALTIME deadline to pthread_cond_timedwait. A wall-clock step
   -- NTP step, a manual date change, restore from sleep -- can therefore delay
   one wait by the size of the step, which delays the timers due in it.
   spin_wait_strategy could not do this because it compared against the steady
   clock in its poll loop.
   The blast radius is one wait, not a permanent skew: TimerThreadMain re-reads
   steady_clock every iteration and fires everything already due, so the queue
   catches up on the next wake rather than drifting. Linux takes the
   clockwait path and is unaffected; Windows still spins. Fixing it properly
   needs a Darwin wait built on pthread_cond_timedwait_relative_np, which is a
   bigger change than this one and is not attempted here.

   Correctness of the switch: blocking_wait_strategy only wakes on
   signal_all_when_blocking(), and both publish paths call it --
   multi_threaded_claim_strategy::publish and sequence_barrier::publish -- so a
   queued timer wakes the dispatch thread immediately rather than waiting out
   its timeout.
*/
#if XE_PLATFORM_WIN32
using WaitStrat = dp::spin_wait_strategy;
#else
using WaitStrat = dp::blocking_wait_strategy;
#endif

class TimerQueue {
 public:
  using clock = WaitItem::clock;
  static_assert(clock::is_steady);

 public:
  TimerQueue()
      : buffer_(kWaitCount),
        wait_strategy_(),
        claim_strategy_(kWaitCount, wait_strategy_),
        consumed_(wait_strategy_),
        shutdown_(false) {
    claim_strategy_.add_claim_barrier(consumed_);
    dispatch_thread_ = std::thread(&TimerQueue::TimerThreadMain, this);
  }

  ~TimerQueue() {
    shutdown_.store(true, std::memory_order_release);

    // Kick dispatch thread to check shutdown flag
    auto wait_item = std::make_shared<WaitItem>(nullptr, nullptr, this,
                                                clock::time_point::min(),
                                                clock::duration::zero());
    wait_item->Disarm();
    QueueTimer(std::move(wait_item));

    dispatch_thread_.join();
  }

  void TimerThreadMain() {
    dp::sequence_t next_sequence = 0;
    const auto comp = [](const std::shared_ptr<WaitItem>& left,
                         const std::shared_ptr<WaitItem>& right) {
      return left->due_ < right->due_;
    };

    xe::threading::set_name("xe::threading::TimerQueue");

    while (!shutdown_.load(std::memory_order_relaxed)) {
      {
        // Consume new wait items and add them to sorted wait queue
        dp::sequence_t available = claim_strategy_.wait_until_published(
            next_sequence, next_sequence - 1,
            wait_queue_.empty() ? clock::now() + kIdleWait
                                : wait_queue_.front()->due_);

        // Check for timeout
        if (available != next_sequence - 1) {
          std::forward_list<std::shared_ptr<WaitItem>> wait_items;
          do {
            wait_items.push_front(std::move(buffer_[next_sequence]));
          } while (next_sequence++ != available);

          consumed_.publish(available);

          wait_items.sort(comp);
          wait_queue_.merge(wait_items, comp);
        }
      }

      {
        // Check wait queue, invoke callbacks and reschedule
        std::forward_list<std::shared_ptr<WaitItem>> wait_items;
        while (!wait_queue_.empty() &&
               wait_queue_.front()->due_ <= clock::now()) {
          auto wait_item = std::move(wait_queue_.front());
          wait_queue_.pop_front();

          // Ensure that it isn't disarmed
          auto state = WaitItem::State::kIdle;
          if (wait_item->state_.compare_exchange_strong(
                  state, WaitItem::State::kInCallback,
                  std::memory_order_acq_rel)) {
            // Possibility to dispatch to a thread pool here
            assert_not_null(wait_item->callback_);
            wait_item->callback_(wait_item->userdata_);

            if (wait_item->interval_ != clock::duration::zero() &&
                wait_item->state_.load(std::memory_order_acquire) !=
                    WaitItem::State::kInCallbackSelfDisarmed) {
              // Item is recurring and didn't self-disarm during callback:
              wait_item->due_ += wait_item->interval_;
              wait_item->state_.store(WaitItem::State::kIdle,
                                      std::memory_order_release);
              wait_items.push_front(std::move(wait_item));
            } else {
              wait_item->state_.store(WaitItem::State::kDisarmed,
                                      std::memory_order_release);
            }
          } else {
            // Specifically, kInCallback is illegal here
            assert_true(WaitItem::State::kDisarmed == state);
          }
        }
        wait_items.sort(comp);
        wait_queue_.merge(wait_items, comp);
      }
    }
  }

  std::weak_ptr<WaitItem> QueueTimer(std::shared_ptr<WaitItem> wait_item) {
    auto wait_item_weak = std::weak_ptr<WaitItem>(wait_item);

    // Mitigate callback flooding
    wait_item->due_ =
        std::max(clock::now() - wait_item->interval_, wait_item->due_);

    auto sequence = claim_strategy_.claim_one();
    buffer_[sequence] = std::move(wait_item);
    claim_strategy_.publish(sequence);

    return wait_item_weak;
  }

  const std::thread& dispatch_thread() const { return dispatch_thread_; }

 private:
  // How long to wait when no timer is queued at all, rather than
  // time_point::max(). Not for the reason first given here: libc++ does handle
  // an unbounded deadline, clamping it explicitly in wait_for before the cast.
  // It is bounded so the wait does not depend on a library's overflow handling
  // at all, and so an idle queue is provably not parked forever. A queued timer
  // wakes the thread through signal_all_when_blocking() long before this
  // expires, so the cost is one wakeup a minute in a process with no timers.
  static constexpr clock::duration kIdleWait = std::chrono::seconds(60);

  // This ring buffer will be used to introduce timers queued by the public API
  static constexpr size_t kWaitCount = 512;
  dp::ring_buffer<std::shared_ptr<WaitItem>> buffer_;

  WaitStrat wait_strategy_;
  dp::multi_threaded_claim_strategy<WaitStrat> claim_strategy_;
  dp::sequence_barrier<WaitStrat> consumed_;

  // This is a _sorted_ (ascending due_) list of active timers managed by a
  // dedicated thread
  std::forward_list<std::shared_ptr<WaitItem>> wait_queue_;
  std::atomic_bool shutdown_;
  std::thread dispatch_thread_;
};

xe::threading::TimerQueue timer_queue_;

void TimerQueueWaitItem::Disarm() {
  State state;

  // Special case for calling from a callback itself
  if (std::this_thread::get_id() == parent_queue_->dispatch_thread().get_id()) {
    state = State::kInCallback;
    if (state_.compare_exchange_strong(state, State::kInCallbackSelfDisarmed,
                                       std::memory_order_acq_rel)) {
      // If we are self disarming from the callback set this special state and
      // exit
      return;
    }
    // Normal case can handle the rest
  }

  dp::spin_wait spinner;
  state = State::kIdle;
  // Classes which hold WaitItems will often call Disarm() to cancel them during
  // destruction. This may lead to race conditions when the dispatch thread
  // executes a callback which accesses memory that is freed simultaneously due
  // to this. Therefore, we need to guarantee that no callbacks will be running
  // once Disarm() has returned.
  while (!state_.compare_exchange_weak(state, State::kDisarmed,
                                       std::memory_order_acq_rel)) {
    if (state == State::kDisarmed) {
      // Do not break for kInCallbackSelfDisarmed and keep spinning in order to
      // meet guarantees
      break;
    }
    state = State::kIdle;
    spinner.spin_once();
  }
}
// unused
std::weak_ptr<WaitItem> QueueTimerOnce(std::function<void(void*)> callback,
                                       void* userdata,
                                       WaitItem::clock::time_point due) {
  return timer_queue_.QueueTimer(
      std::make_shared<WaitItem>(std::move(callback), userdata, &timer_queue_,
                                 due, WaitItem::clock::duration::zero()));
}
// only used by HighResolutionTimer
std::weak_ptr<WaitItem> QueueTimerRecurring(
    std::function<void(void*)> callback, void* userdata,
    WaitItem::clock::time_point due, WaitItem::clock::duration interval) {
  return timer_queue_.QueueTimer(std::make_shared<WaitItem>(
      std::move(callback), userdata, &timer_queue_, due, interval));
}

}  // namespace threading
}  // namespace xe
