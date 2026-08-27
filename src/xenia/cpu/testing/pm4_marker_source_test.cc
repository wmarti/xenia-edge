/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"
#include "xenia/gpu/pm4_marker_sink.h"

#include "third_party/catch/include/catch.hpp"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <cstdint>
#include <memory>
#include <vector>

namespace xe {
namespace gpu {
namespace testing {
namespace {

class RecordingSink final : public Pm4MarkerSink {
 public:
  bool OnPm4Marker(const Pm4MarkerEvent& event) noexcept override {
    events.push_back(event);
    return accept;
  }

  void OnPm4MarkerSourceShutdown() noexcept override { ++shutdown_count; }

  std::vector<Pm4MarkerEvent> events;
  bool accept = true;
  uint32_t shutdown_count = 0;
};

class ReentrantDetachSink final
    : public Pm4MarkerSink,
      public std::enable_shared_from_this<ReentrantDetachSink> {
 public:
  explicit ReentrantDetachSink(Pm4MarkerDispatcher& dispatcher)
      : dispatcher_(dispatcher) {}

  bool OnPm4Marker(const Pm4MarkerEvent&) noexcept override {
    ++event_count;
    detach_result = dispatcher_.DetachSink(shared_from_this());
    return true;
  }

  void OnPm4MarkerSourceShutdown() noexcept override { ++shutdown_count; }

  Pm4MarkerDispatcher& dispatcher_;
  uint32_t event_count = 0;
  uint32_t shutdown_count = 0;
  bool detach_result = true;
};

}  // namespace

TEST_CASE("PM4 marker source keeps source-local ordinals",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  REQUIRE(dispatcher.marker_count() == 0);

  dispatcher.NotifyPm4Swap(10);
  REQUIRE(dispatcher.marker_count() == 1);

  auto sink = std::make_shared<RecordingSink>();
  REQUIRE(dispatcher.AttachSink(sink));
  dispatcher.NotifyPm4Swap(20);
  dispatcher.NotifyPm4Swap(30);

  REQUIRE(dispatcher.marker_count() == 3);
  REQUIRE(sink->events.size() == 2);
  const Pm4MarkerEvent expected_first{Pm4MarkerSource::kPm4Swap,
                                      kPm4SwapMarkerOpcode, 2, 20};
  const Pm4MarkerEvent expected_second{Pm4MarkerSource::kPm4Swap,
                                       kPm4SwapMarkerOpcode, 3, 30};
  REQUIRE(sink->events[0] == expected_first);
  REQUIRE(sink->events[1] == expected_second);
  REQUIRE_FALSE(dispatcher.sink_failed());

  REQUIRE(dispatcher.DetachSink(sink));
  dispatcher.NotifyPm4Swap(40);
  REQUIRE(dispatcher.marker_count() == 4);
  REQUIRE(sink->events.size() == 2);
}

TEST_CASE("PM4 marker source owns one sink and shuts it down once",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto first = std::make_shared<RecordingSink>();
  auto second = std::make_shared<RecordingSink>();

  REQUIRE_FALSE(dispatcher.AttachSink({}));
  REQUIRE(dispatcher.AttachSink(first));
  REQUIRE_FALSE(dispatcher.AttachSink(second));
  REQUIRE_FALSE(dispatcher.DetachSink(second));
  REQUIRE(dispatcher.DetachSink(first));
  REQUIRE(dispatcher.AttachSink(second));

  dispatcher.Shutdown();
  dispatcher.Shutdown();
  REQUIRE(second->shutdown_count == 1);
  REQUIRE_FALSE(dispatcher.AttachSink(first));
  REQUIRE_FALSE(dispatcher.DetachSink(second));

  dispatcher.NotifyPm4Swap(50);
  REQUIRE(dispatcher.marker_count() == 1);
  REQUIRE(second->events.empty());
}

TEST_CASE("PM4 marker source fails delivery closed", "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<RecordingSink>();
  sink->accept = false;
  REQUIRE(dispatcher.AttachSink(sink));

  dispatcher.NotifyPm4Swap(60);
  REQUIRE(dispatcher.sink_failed());
  REQUIRE(sink->events.size() == 1);

  dispatcher.NotifyPm4Swap(70);
  REQUIRE(dispatcher.marker_count() == 2);
  REQUIRE(sink->events.size() == 1);

  REQUIRE(dispatcher.DetachSink(sink));
  REQUIRE_FALSE(dispatcher.AttachSink(std::make_shared<RecordingSink>()));
}

TEST_CASE("PM4 marker source rejects detach from its callback",
          "[pm4-marker-source]") {
  Pm4MarkerDispatcher dispatcher;
  auto sink = std::make_shared<ReentrantDetachSink>(dispatcher);
  REQUIRE(dispatcher.AttachSink(sink));

  dispatcher.NotifyPm4Swap(80);
  REQUIRE(sink->event_count == 1);
  REQUIRE_FALSE(sink->detach_result);
  REQUIRE(dispatcher.DetachSink(sink));
  dispatcher.Shutdown();
  REQUIRE(sink->shutdown_count == 0);
}

}  // namespace testing
}  // namespace gpu
}  // namespace xe

#else

TEST_CASE("PM4 marker source compiles out of normal builds",
          "[pm4-marker-source]") {
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE)
  REQUIRE(XE_ENABLE_GUEST_INVOCATION_CAPTURE == 0);
#else
  SUCCEED();
#endif
}

#endif
