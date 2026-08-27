# Guest scheduler capture events

This document defines the capture-build-only observer of `GuestScheduler`
transitions that the bounded execution replay in `guest_execution_replay.md`
records as its deterministic thread-dispatch tape. It covers only the
observation mechanism: what the scheduler reports, in which order, under which
locking rules, and how the observer is installed and torn down. Event storage,
the session codec, PPC checkpoints and combined capture safepoint delivery are
separate mechanisms with their own documents.

The scheduler remains Xenia's normal `guest_scheduler=true` behavior. Capture
observes it; it never changes a queue, a priority, a quantum or a wake.

## Compile-out

Everything below exists only when `XE_ENABLE_GUEST_INVOCATION_CAPTURE` is 1,
the same definition that guards every other capture hook. The observer header
`src/xenia/kernel/guest_scheduler_capture_observer.h`, its implementation, the
`GuestScheduler` members and every emission site are wrapped in that guard, so
a normal build has no observer pointer, sequence counter, callback, branch or
extra lock acquisition in the scheduler. `PPCContext` is untouched.

## Participants and identity

Each event names one participant by the capture instance ID of its
`ThreadState` (`ThreadState::guest_execution_capture_instance_id()`), which is
process-unique per ThreadState lifetime and never reused. The guest thread ID
is carried as a diagnostic only. Host addresses, fibers, `XThread` pointers and
scheduler-lock-owned references never reach the observer: an event is a
fixed-size value (`GuestSchedulerCaptureEvent`) with no pointer field.

## Ordering contract

`GuestScheduler::lock_` is the single lock guarding every per-CPU queue. Every
event is assigned its sequence number and delivered to the observer while that
lock is held. For an event that describes a queue mutation the emission and
the mutation happen inside the same lock hold, so no other CPU can interleave
between them. Three events describe a decision rather than a mutation and are
sequenced in a lock hold of their own, acquired solely to emit, immediately
before the mutation they lead to on the same fiber: `kYield` precedes the
fiber's own `kEnqueueReady`, `kExit` precedes the dispatcher's `kSwitchOut`,
and `kSafepoint` precedes the fiber's `kYield`. Another CPU's events may be
sequenced between such a pair; the pair itself is never reordered because the
same fiber issues both. The sequence is therefore one total order that agrees
with the scheduler's own mutation order across all six dispatch threads, the
watchdog, the I/O worker and external callers such as `MarkReady` or
`TerminateThread`.

The sequence is scheduler-local: it starts at 1 at attachment, is reset by a
later attachment, and is contiguous for the observer's lifetime. It is not the
session's global event sequence. The session sink must assign the session
sequence at delivery, in callback order, and treat a gap or regression in the
scheduler sequence as loss of this contract and reject the session.

Because callbacks run under the scheduler lock, an observer must be bounded and
nonblocking, must not call any `GuestScheduler`, `XThread`, `KernelState` or
`Processor` API, and must not retain anything beyond the event value. The lock
order is always `lock_` then any observer-internal lock. Returning `false`
latches a sticky rejection in the scheduler; nothing further, including
`kShutdown`, is delivered. The scheduler logs the latch once at error level and
exposes it as `GuestScheduler::capture_rejected()`, which the session owner
must consult before accepting a tape that ends without `kShutdown`.

Every `kSafepoint` carries the exact aligned PPC address passed by the JIT
safepoint. All other scheduler events require a zero `guest_pc`; `kYield` and
`kBlock` still locate host-service transitions only relative to the
participant's other events. This exact PC is independent of the boundary
checkpoint route and lets the replay controller authenticate each scheduler
safepoint rather than infer it from coverage.

`kBlock` and `kReready` carry a pointer-free wait snapshot: up to eight guest
handles, the per-object signal epochs sampled before the failed poll and at the
scheduler decision, their aggregate epochs, deadline and observation uptime,
and gated, alertable, interruptible and pending-user-APC bits. The bridge
rejects a wait naming more than eight handles instead of replaying a truncated
identity. For tracked multi-waits the per-object epochs identify which member
moved. The diagnostic signal ring's signaler LR and host thread are not read
under the scheduler lock because its inverse lock order would deadlock; they
remain diagnostics rather than authenticated replay input.

`RereadyBlocked` takes that decision snapshot once while it owns the scheduler
lock. The epoch comparison, deadline and APC choice are derived from that same
immutable value, and the event serializes the value unchanged. A signal racing
after the snapshot therefore wakes a later scheduler pass; it cannot rewrite
the epoch or cause attached to the decision already in progress.

The durable wait-kind values are:

| Value | Kind | Handles | Deadline |
| ---: | --- | ---: | --- |
| 1 | single object, including `SignalAndWait` | 1 | optional |
| 2 | wait-any | 1-8 | optional |
| 3 | wait-all | 1-8 | optional |
| 4 | delay | 0 | required |
| 5 | host fence | 0 | forbidden |
| 6 | offloaded host call | 0 | forbidden |
| 7 | spin backoff | 0 | forbidden |
| 8 | I/O completion | 1 | optional |
| 9 | socket I/O | 1 | optional |

The I/O completion and socket identities are their real guest object handles;
they do not invent signal epochs for polling-only sources. A socket wait event
authenticates only the scheduler park and its real timeout. Socket result bytes,
errors and host readiness are separate external inputs and remain fail-closed
unless the session's external-event machinery captures them; this wait kind by
itself does not make a socket call replayable.

The canonical scheduler payload is version 2 and 192 bytes. Version 1's
48-byte payload omitted exact PCs and wait causes, so the decoder recognizes it
only to return the explicit `not deterministic-replayable` rejection. Capture
emits version 2 only. The replay configuration schema separately authenticates
`guest_scheduler_quantum_us`; changing the quantum therefore changes the
configuration hash even though raw host slice-deadline ticks remain local.
Version 2 also rejects lifecycle kinds `kExit`, `kTerminate`, `kForget` and
`kShutdown`, and rejects `kBlock`/`kReready` with wait kind `kNone`. They lack
enough durable lifecycle or wait-cause provenance for deterministic replay;
observing one poisons the session instead of silently treating it as a control
echo.

`GuestSchedulerCaptureEventRecorder` is the reference nonblocking handoff. Its
buffer is reserved at construction so a callback never allocates, it validates
sequence continuity for every delivered event whether or not it is armed, and
an overflow while armed fails closed (`kOverflow`) rather than dropping events.
Arming and disarming happen inside the recorder, not by attaching or detaching.

## Installation and teardown

- `GuestScheduler::AttachCaptureObserver` installs one shared-owned observer.
  It is rejected if an observer is already attached, if any thread is queued
  on any CPU (`ready_summary != 0`, so an earlier `kEnqueueReady` can never be
  missing), if any fiber has been dispatched (`SwitchTo` marks this under
  `lock_`), after `Shutdown`, or if the observer reports
  `CanDetach() == false`. Attach before the first `XThread::Create`.
- `DetachCaptureObserver` is permitted only before the first dispatch and only
  when the observer agrees. After the first dispatch the registration is
  scheduler-lifetime permanent.
- `Shutdown` joins the dispatch, watchdog and I/O threads, drains every queue
  (emitting `kForget` with reason `kShutdown` for each leftover thread), emits
  the terminal `kShutdown` event, closes attachment, and drops the shared
  registration outside `lock_`. All of this precedes `ReclaimExited` on the
  leftovers, so every scheduler event of a participant precedes that
  participant's `ThreadState` destruction as seen by the Processor lifetime
  registry. A never-started scheduler performs the same release on `Shutdown`.

## Event kinds

| Kind | Emitted at | Fields |
| --- | --- | --- |
| `kEnqueueReady` | `EnqueueReady` before linking | `cpu` caller, `target_cpu` queue CPU; `AtHead`, `YieldToOther` |
| `kDequeueReady` | `DequeueReady` on selection | `cpu`; `HonoredYield` when the yielder was passed over |
| `kDispatch` | `SwitchTo` before the fiber switch | `cpu`; `FirstRun`, `FreshQuantum` |
| `kSwitchOut` | `SwitchTo` after control returns to the idle fiber | `cpu` |
| `kYield` | `YieldCurrentThread` before re-queueing | `cpu`; `QuantumEnd`, `ToLower`, `Preempted` |
| `kPreemptRequest` | every raise of `preempt_requested` by the scheduler; `kShutdown` once per fiber | `cpu`; reason `kPriority`, `kWake`, `kTimeslice`, `kShutdown` |
| `kSafepoint` | the JIT safepoint handler: once per deferral episode, and every terminal outcome | `cpu`; exact `guest_pc`; reason `kDeferredLock`, `kDeferredIrql`, `kForcedIrql`, `kYielded`; `value` IRQL; `count` declined safepoints (terminal only); `SchedulerRequested`, `CaptureRequested` |
| `kBlock` | `BlockCurrentThread` after parking | `cpu`; `Gated`, `Alertable`, `Interruptible`, `HasDeadline`; `value` wait kind; handles, pre-poll and observed signal epochs, deadline and APC provenance |
| `kReready` | `RereadyBlocked` per released waiter | `cpu` poller, `target_cpu` home; reason `kPolled`, `kSignalEpoch`, `kDeadline`, `kUserApc`, `kBackstop`; `AtHead`; `value` wait kind; the same wait identity plus decision-time epochs, uptime and APC state |
| `kParkSuspended` | `ParkSuspended` | `cpu` |
| `kResume` | `ResumeThread` unparking a suspended thread | `cpu` |
| `kPriorityChange` | `PublishPriority` | `cpu` owner or -1 before first enqueue; `priority` new level, `value` old level; a ready participant is re-linked at the new level in the same locked transaction |
| `kMigrate` | `RunLoop` honoring an affinity change | `cpu` from, `target_cpu` to; `AtHead` |
| `kExit` | `NotifyThreadExited` | `cpu` |
| `kTerminate` | `TerminateThread` | `cpu`; reason `kDetached`, `kPreemptRequested`, `kReadied`, `kNeverRan`, `kDeferredToDispatcher` |
| `kForget` | `ForgetThread`, and the `Shutdown` drain with reason `kShutdown` | `cpu` |
| `kShutdown` | end of `Shutdown` | no participant; terminal |

`priority` is the participant's clamped ready level at the event. `cpu` is -1
when the emitting caller is not a dispatch thread.

A `TerminateThread` that raises the running participant's flag reports
`kTerminate` with reason `kPreemptRequested` and does not also report
`kPreemptRequest`.

Safepoint deferrals are bounded per episode, not per safepoint. The handler
emits `kDeferredLock` on the first decline while the global critical region is
held and `kDeferredIrql` on the first decline at IRQL >= 2; the further
declines of that episode (up to 65536 and 4096 respectively) are counted in
`XThread::SchedulerLinks::capture_declined_safepoints` without an event or a
lock acquisition. The next terminal outcome, `kYielded` or `kForcedIrql`,
carries that total in `count` and resets it. A lock episode nested inside an
IRQL episode therefore yields exactly two opening events and one terminal whose
`count` includes both. If the fiber leaves guest code by a wait before a
terminal safepoint, the count persists and lands on its next terminal, so a
`kBlock` may separate an opening event from its terminal.

## Capture safepoint dependency

Capture and scheduler safepoint requests are independent flags. This observer
records only what the scheduler's own handler saw and did; it does not
implement combined delivery. `GuestScheduler::NoteCaptureSafepoint` accepts the
request flags so the combined safepoint handler must report
`kGuestSchedulerCaptureFlagCaptureRequested` alongside
`kGuestSchedulerCaptureFlagSchedulerRequested` when it consumes both at one
safepoint, producing exactly one `kSafepoint` event with both bits set. Until
that handler lands, every `kSafepoint` event carries only the scheduler bit.

## Source-line inventory (base 467031339)

Every scheduler transition below is observed unless marked otherwise. Lines
refer to `src/xenia/kernel/guest_scheduler.cc` at the base commit.

- Enqueue: `EnqueueReady` 325-357 (`kEnqueueReady`). Silently ignored requests
  at 333-343 (already blocked, suspended, queued, or running on another CPU)
  change no state and are not reported.
- Dequeue and CPU selection: `DequeueReady` 424-467, including the voluntary
  yield opt-out at 438-453 (`kDequeueReady`).
- Dispatch: `SwitchTo` 645-692 (`kDispatch`, `kSwitchOut`).
- Cooperative yield: `YieldCurrentThread` 779-811 (`kYield`), reached from
  `NtYieldExecution`, `Delay(0)`, self-`Suspend`, `SpinYield` 813-833 and the
  safepoint handler. The caller is not distinguished.
- Timeslice preemption request: `WatchdogLoop` 1558-1564 (`kPreemptRequest`
  `kTimeslice`). Consumption: `PreemptCurrentFiber` 69-119 (`kSafepoint`).
- Priority preemption: `LinkReadyLocked` 499-506 (`kPreemptRequest`
  `kPriority`).
- Wake preemption: `WakeAll` 957-963 and `WakeForSignal` 1022-1027
  (`kPreemptRequest` `kWake`). The `repoll_now` poke itself is host timing and
  is not an event; its effect appears as `kReready`.
- Block: `BlockCurrentThread` 1049-1151 (`kBlock`). Wake: `RereadyBlocked`
  1153-1237 (`kReready`).
- Suspend and resume: `ParkSuspended` 386-405 (`kParkSuspended`),
  `ResumeThread` 370-384 (`kResume`).
- Effective priority publication: `PublishPriority` (`kPriorityChange`) records
  every real change under the scheduler lock. Ready participants are re-linked
  there; blocked-priority caches are repaired there; running and suspended
  changes retain their prior scheduling semantics.
- Migration: `RunLoop` 1265-1284 (`kMigrate`); the blocked-then-moved case is
  covered by `kReready` with `target_cpu != cpu`.
- Termination: `TerminateThread` 581-643 (`kTerminate`), `NotifyThreadExited`
  1038-1047 (`kExit`), `ForgetThread` 541-579 (`kForget`).
- IRQL and critical-region deferral: `PreemptCurrentFiber` 80-101
  (`kSafepoint` `kDeferredLock`, `kDeferredIrql`, first decline of an episode
  only), forced at 102-112 (`kForcedIrql` with the episode count).
- Shutdown: 216-323 (`kPreemptRequest` `kShutdown` once per fiber still
  running at 246-253, `kForget` `kShutdown`, `kShutdown`).

## Paths still unobserved

- Affinity changes (`XThread::SetActiveCpu`) are observed only when the
  scheduler acts on them (`kMigrate`, `kReready` to another CPU).
- The idle loop's decision to sleep, the `repoll_now` and `ready_event` pokes,
  slice deadlines in host ticks and the stats counters are host timing and are
  intentionally not reported. The configured quantum is authenticated in the
  replay configuration instead.
- The I/O worker's offloaded call itself is not an event; its park and release
  appear as `kBlock` and `kReready` with wait kind `kIoOffload`.
- A guest thread on the host-thread model (`guest_scheduler=false` or an
  `XHostThread`) never enters this scheduler and produces no events.
- `EnqueueReady` requests that are ignored because the thread already owns its
  wake-up are not reported.
- The individual declined safepoints inside a deferral episode are counted,
  not reported; only the episode's opening and terminal outcome are events.

## Test coverage

`src/xenia/cpu/testing/guest_scheduler_capture_observer_test.cc` drives the
recorder through a fake locked emitter for two participants, block/wake, yield,
priority and timeslice preemption, simultaneous scheduler and capture safepoint
requests, migration, missing and out-of-order callbacks, overflow, disarmed
continuity, invalid events, deferral episode counts, shutdown and a concurrent
teardown race, and exercises `GuestScheduler` attachment, detachment, shutdown
release and the rejection latch (`capture_rejected()`) on a never-started
scheduler. It lives in the CPU suite because `xenia-kernel-tests` links only
`xenia-base`.

`guest_execution_session_assembler_test.cc` additionally round-trips version 2
block and signal-reready provenance through the canonical bundle, exercises all
13 replay-modeled event kinds and all nine wait kinds, and proves malformed
wait-kind combinations, exact-PC, wait-epoch, participant, sequence, high-bit
kind aliases and legacy-version payloads reject closed.

`guest_scheduler_checkpoint_runtime_test.cc` runs scheduler-backed fibers
through single, multi, delay, fence, offloaded-host-call, `SignalAndWait`, spin
backoff, I/O completion and socket paths and checks their live `kBlock` records.
It also forces a signal between the reready snapshot and cause selection and
proves the published event retains the earlier snapshot and backstop cause.

Still not exercised without a title: attachment rejection after a real
`SwitchTo` or with a queued thread, per-episode safepoint accounting in
`PreemptCurrentFiber`, and the shutdown drain of leftover title fibers.
