/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_capture_page_reader.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <thread>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/mutex.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {
namespace test {

namespace {

constexpr uint32_t kReadablePage = 0x00010000u;
constexpr uint32_t kReservedPage = 0x00020000u;
constexpr uint32_t kNoAccessPage = 0x00030000u;
constexpr uint32_t kDecommittedPage = 0x00040000u;
constexpr uint32_t kFreePage = 0x00050000u;
constexpr uint32_t kHostReadableUntrackedPage = 0x80100000u;
constexpr uint32_t kTrackedAliasPage = 0x90100000u;

void AllocatePage(Memory& memory, uint32_t address, uint32_t allocation_type,
                  uint32_t protect) {
  BaseHeap* const heap = memory.LookupHeap(address);
  REQUIRE(heap);
  REQUIRE(heap->AllocFixed(address, 4096, 4096, allocation_type, protect));
}

}  // namespace

TEST_CASE("guest invocation capture page reader fails closed",
          "[guest-invocation-capture]") {
  std::array<uint8_t, 4096> output = {};
  {
    Memory uninitialized_memory;
    GuestInvocationCapturePageReader uninitialized_reader(uninitialized_memory);
    REQUIRE_FALSE(uninitialized_reader.ReadPage(kReadablePage, &output));
  }

  Memory memory;
  REQUIRE(memory.Initialize());
  GuestInvocationCapturePageReader reader(memory);

  AllocatePage(memory, kReadablePage,
               kMemoryAllocationReserve | kMemoryAllocationCommit,
               kMemoryProtectRead | kMemoryProtectWrite);
  std::array<uint8_t, 4096> expected = {};
  for (size_t i = 0; i < expected.size(); ++i) {
    expected[i] = static_cast<uint8_t>(i * 29u + 7u);
  }
  std::memcpy(memory.TranslateVirtual(kReadablePage), expected.data(),
              expected.size());
  REQUIRE(memory.LookupHeap(kReadablePage)
              ->Protect(kReadablePage, 4096, kMemoryProtectRead));
  REQUIRE(reader.ReadPage(kReadablePage, &output));
  REQUIRE(output == expected);

  HeapAllocationInfo untracked_info = {};
  REQUIRE(memory.LookupHeap(kHostReadableUntrackedPage)
              ->QueryRegionInfo(kHostReadableUntrackedPage, &untracked_info));
  REQUIRE(untracked_info.state == 0);
  REQUIRE(untracked_info.protect == 0);
  AllocatePage(memory, kTrackedAliasPage,
               kMemoryAllocationReserve | kMemoryAllocationCommit,
               kMemoryProtectRead | kMemoryProtectWrite);
  std::memcpy(memory.TranslateVirtual(kTrackedAliasPage), expected.data(),
              expected.size());
  REQUIRE(memory.LookupHeap(kTrackedAliasPage)
              ->Protect(kTrackedAliasPage, 4096, kMemoryProtectRead));
  REQUIRE(reader.ReadPage(kHostReadableUntrackedPage, &output));
  REQUIRE(output == expected);
  // Releasing the alias leaves the host mapping in place, and the host mapping
  // is what the guest reads: a title that loads several modules into one heap
  // leaves earlier images executing behind entries the heap has forgotten.
  REQUIRE(memory.LookupHeap(kTrackedAliasPage)->Release(kTrackedAliasPage));
  REQUIRE(reader.ReadPage(kHostReadableUntrackedPage, &output));

  // A reservation may retain a readable page-table protection while lacking
  // commitment. The reader must validate both attributes.
  AllocatePage(memory, kReservedPage, kMemoryAllocationReserve,
               kMemoryProtectRead);
  REQUIRE_FALSE(reader.ReadPage(kReservedPage, &output));

  // This range is genuinely inaccessible in the host mapping. Reaching memcpy
  // here would terminate the test process rather than merely fail an assertion.
  AllocatePage(memory, kNoAccessPage,
               kMemoryAllocationReserve | kMemoryAllocationCommit,
               kMemoryProtectNoAccess);
  REQUIRE_FALSE(reader.ReadPage(kNoAccessPage, &output));

  AllocatePage(memory, kDecommittedPage,
               kMemoryAllocationReserve | kMemoryAllocationCommit,
               kMemoryProtectRead | kMemoryProtectWrite);
  REQUIRE(
      memory.LookupHeap(kDecommittedPage)->Decommit(kDecommittedPage, 4096));
  REQUIRE_FALSE(reader.ReadPage(kDecommittedPage, &output));

  REQUIRE_FALSE(reader.ReadPage(kFreePage, &output));
  REQUIRE_FALSE(reader.ReadPage(0x7F000000u, &output));
  REQUIRE_FALSE(reader.ReadPage(kReadablePage + 1, &output));
  REQUIRE_FALSE(reader.ReadPage(std::numeric_limits<uint32_t>::max(), &output));
  REQUIRE_FALSE(reader.ReadPage(kReadablePage, nullptr));
}

TEST_CASE("guest invocation capture page reader never waits for global region",
          "[guest-invocation-capture]") {
  Memory memory;
  REQUIRE(memory.Initialize());
  AllocatePage(memory, kReadablePage,
               kMemoryAllocationReserve | kMemoryAllocationCommit,
               kMemoryProtectRead);
  GuestInvocationCapturePageReader reader(memory);
  std::array<uint8_t, 4096> output = {};

  std::atomic<bool> global_region_held = false;
  std::atomic<bool> release_global_region = false;
  std::thread holder([&] {
    auto global_lock = xe::global_critical_region::AcquireDirect();
    global_region_held.store(true, std::memory_order_release);
    while (!release_global_region.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  const auto holder_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!global_region_held.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < holder_deadline) {
    std::this_thread::yield();
  }
  const bool global_region_held_observed =
      global_region_held.load(std::memory_order_acquire);

  std::atomic<bool> read_started = false;
  std::atomic<bool> read_finished = false;
  bool read_result = true;
  std::thread reader_thread([&] {
    read_started.store(true, std::memory_order_release);
    read_result = reader.ReadPage(kReadablePage, &output);
    read_finished.store(true, std::memory_order_release);
  });
  const auto reader_start_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!read_started.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < reader_start_deadline) {
    std::this_thread::yield();
  }
  const auto reader_finish_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (!read_finished.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < reader_finish_deadline) {
    std::this_thread::yield();
  }
  const bool read_finished_while_contended =
      read_finished.load(std::memory_order_acquire);

  release_global_region.store(true, std::memory_order_release);
  holder.join();
  reader_thread.join();

  REQUIRE(global_region_held_observed);
  REQUIRE(read_started.load(std::memory_order_acquire));
  REQUIRE(read_finished_while_contended);
  REQUIRE_FALSE(read_result);
  REQUIRE(reader.last_read_was_retryable());
  REQUIRE(reader.ReadPage(kReadablePage, &output));
  REQUIRE_FALSE(reader.last_read_was_retryable());
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

#endif
