/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_runner.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/cpu/testing/util.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

namespace {

constexpr uint32_t kCodeAddress = 0x82040000u;
constexpr uint32_t kDataAddress = 0x10000000u;
constexpr uint32_t kReturnAddress = 0x83000000u;

void AppendU32(std::vector<uint8_t>* data, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    data->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void StoreGuestInstruction(std::array<uint8_t, JitCorpus::kPageSize>* page,
                           uint32_t offset, uint32_t instruction) {
  (*page)[offset + 0] = static_cast<uint8_t>(instruction >> 24);
  (*page)[offset + 1] = static_cast<uint8_t>(instruction >> 16);
  (*page)[offset + 2] = static_cast<uint8_t>(instruction >> 8);
  (*page)[offset + 3] = static_cast<uint8_t>(instruction);
}

void AppendCorpusPage(std::vector<uint8_t>* encoded, uint32_t address,
                      const std::array<uint8_t, JitCorpus::kPageSize>& data) {
  AppendU32(encoded, JitCorpus::kTagPage);
  AppendU32(encoded, address);
  encoded->insert(encoded->end(), data.cbegin(), data.cend());
}

ExecutionJitCorpus MakeCorpus(uint32_t closure_page_count) {
  std::vector<uint8_t> encoded;
  AppendU32(&encoded, JitCorpus::kMagic);
  AppendU32(&encoded, JitCorpus::kVersion);
  AppendU32(&encoded, JitCorpus::kPageSize);
  AppendU32(&encoded, 0);

  for (uint32_t i = 0; i < closure_page_count; ++i) {
    std::array<uint8_t, JitCorpus::kPageSize> page = {};
    if (!i) {
      StoreGuestInstruction(&page, 0, 0x80A30000u);   // lwz r5, 0(r3)
      StoreGuestInstruction(&page, 4, 0x38A50001u);   // addi r5, r5, 1
      StoreGuestInstruction(&page, 8, 0x90A30000u);   // stw r5, 0(r3)
      StoreGuestInstruction(&page, 12, 0x4E800020u);  // blr
    }
    AppendCorpusPage(&encoded, kCodeAddress + i * JitCorpus::kPageSize, page);
  }

  AppendU32(&encoded, JitCorpus::kTagFunction);
  AppendU32(&encoded, kCodeAddress);
  AppendU32(&encoded, kCodeAddress + 12);
  AppendU32(&encoded, 64);
  AppendU32(&encoded, 0);

  ExecutionJitCorpus corpus;
  std::string error;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus, &error));
  REQUIRE(error.empty());
  return corpus;
}

ppc::GuestInvocationPage MakeDataPage(uint32_t address, uint8_t seed) {
  ppc::GuestInvocationPage page;
  page.guest_address = address;
  for (size_t i = 0; i < page.data.size(); ++i) {
    page.data[i] = static_cast<uint8_t>(seed + i * 13);
  }
  return page;
}

ppc::GuestFunctionInvocation MakeInvocation(uint32_t closure_page_count) {
  ppc::GuestFunctionInvocation invocation;
  invocation.function_address = kCodeAddress;
  invocation.function_end_address = kCodeAddress + 12;
  invocation.entry_address = kCodeAddress;
  invocation.expected_return_address = kReturnAddress;
  invocation.input.link_register = kReturnAddress;
  invocation.input.gpr[3] = kDataAddress;
  invocation.expected_output = invocation.input;
  invocation.expected_output.gpr[5] = 42;

  for (uint32_t i = 0; i < closure_page_count; ++i) {
    invocation.input_data_pages.push_back(
        MakeDataPage(kDataAddress + i * JitCorpus::kPageSize,
                     static_cast<uint8_t>(0x20 + i)));
  }
  invocation.input_data_pages[0].data[0] = 0;
  invocation.input_data_pages[0].data[1] = 0;
  invocation.input_data_pages[0].data[2] = 0;
  invocation.input_data_pages[0].data[3] = 41;

  ppc::GuestInvocationPage dirty_page = invocation.input_data_pages[0];
  dirty_page.data[3] = 42;
  invocation.expected_dirty_pages.push_back(std::move(dirty_page));
  return invocation;
}

}  // namespace

TEST_CASE("guest invocation planner derives closed reset and access sets",
          "[guest-invocation-runner]") {
  ExecutionJitCorpus corpus = MakeCorpus(4);
  ppc::GuestFunctionInvocation invocation = MakeInvocation(4);

  GuestInvocationReplayPlan plan;
  std::string error;
  REQUIRE(BuildGuestInvocationReplayPlan(invocation, corpus, 16 * 1024, &plan,
                                         &error));
  REQUIRE(error.empty());
  REQUIRE(plan.host_page_size == 16 * 1024);
  REQUIRE(plan.supplied_page_addresses.size() == 8);
  REQUIRE(plan.reset_page_addresses ==
          std::vector<uint32_t>{kDataAddress, kDataAddress + 0x1000,
                                kDataAddress + 0x2000, kDataAddress + 0x3000});
  REQUIRE(
      plan.protection_granules ==
      std::vector<GuestInvocationReplayProtectionGranule>{
          {kDataAddress, 16 * 1024, true}, {kCodeAddress, 16 * 1024, false}});
}

TEST_CASE("guest invocation planner rejects unsafe page layouts",
          "[guest-invocation-runner]") {
  ExecutionJitCorpus corpus = MakeCorpus(4);
  ppc::GuestFunctionInvocation invocation = MakeInvocation(4);
  GuestInvocationReplayPlan plan;
  std::string error;

  SECTION("data and code overlap") {
    invocation.input_data_pages.push_back(MakeDataPage(kCodeAddress, 0x55));
    std::sort(invocation.input_data_pages.begin(),
              invocation.input_data_pages.end(),
              [](const ppc::GuestInvocationPage& left,
                 const ppc::GuestInvocationPage& right) {
                return left.guest_address < right.guest_address;
              });
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 16 * 1024,
                                                 &plan, &error));
    REQUIRE(error == "invocation data overlaps an exact corpus code page");
    REQUIRE(plan.supplied_page_addresses.empty());
  }

  SECTION("incomplete host granule") {
    invocation.input_data_pages.pop_back();
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 16 * 1024,
                                                 &plan, &error));
    REQUIRE(error == "supplied pages do not close a host protection granule");
    REQUIRE(plan.protection_granules.empty());
  }

  SECTION("0x8 and 0x9 views alias the same backing") {
    for (ppc::GuestInvocationPage& page : invocation.input_data_pages) {
      page.guest_address = 0x92040000u + (page.guest_address - kDataAddress);
    }
    invocation.expected_dirty_pages[0].guest_address = 0x92040000u;
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 16 * 1024,
                                                 &plan, &error));
    REQUIRE(error == "supplied 0x8 and 0x9 pages alias the same backing page");
    REQUIRE(plan.reset_page_addresses.empty());
  }
}

TEST_CASE("guest invocation runner executes and verifies a real backend",
          "[guest-invocation-runner][backend]") {
  constexpr uint32_t kMaximumClosurePageCount = (64 * 1024) / 4096;
  ExecutionJitCorpus corpus = MakeCorpus(kMaximumClosurePageCount);
  ppc::GuestFunctionInvocation invocation =
      MakeInvocation(kMaximumClosurePageCount);

  auto backend = testing::CreateBackend();
  if (!backend) {
    WARN("No executable host backend is available");
    return;
  }

  std::string error;
  std::unique_ptr<GuestInvocationRunner> runner = GuestInvocationRunner::Create(
      invocation, corpus, std::move(backend), &error);
#if !XE_PLATFORM_MAC || !XE_ARCH_ARM64
  REQUIRE_FALSE(runner);
  REQUIRE(error == "guest invocation replay runner requires Apple A64");
  return;
#else
  REQUIRE(runner);
  REQUIRE(error.empty());
  REQUIRE(runner->WarmAndVerify(&error));
  REQUIRE(error.empty());

  GuestInvocationReplayMetrics metrics;
  REQUIRE(runner->RunTimed(128, &metrics, &error));
  REQUIRE(error.empty());
  REQUIRE(metrics.timed_invocation_count == 128);
  REQUIRE(metrics.thread_cpu_nanoseconds > 0);
  REQUIRE(metrics.uptime_raw_nanoseconds > 0);
  REQUIRE(metrics.reset_only_uptime_raw_nanoseconds > 0);
  REQUIRE(metrics.placement_generation_before ==
          metrics.placement_generation_after);
  REQUIRE(metrics.reset_page_count_per_invocation ==
          xe::memory::page_size() / JitCorpus::kPageSize);
  REQUIRE(metrics.reset_bytes_per_invocation == xe::memory::page_size());
#endif  // !XE_PLATFORM_MAC || !XE_ARCH_ARM64
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
