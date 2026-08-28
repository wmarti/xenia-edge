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
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
#include "xenia/cpu/guest_execution_capture.h"
#include "xenia/cpu/processor.h"
#endif
#include "xenia/cpu/testing/util.h"

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
class GuestInvocationRunnerTestAccess final {
 public:
  static GuestExecutionCaptureThreadStateRegistrySnapshot QueryParticipants(
      const GuestInvocationRunner& runner) {
    return runner.processor_->QueryGuestExecutionCaptureParticipants();
  }
};
#endif

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

std::vector<uint8_t> A64Code(std::initializer_list<uint32_t> instructions) {
  std::vector<uint8_t> code;
  code.reserve(instructions.size() * sizeof(uint32_t));
  for (uint32_t instruction : instructions) {
    AppendU32(&code, instruction);
  }
  return code;
}

uint32_t A64Movz64(uint32_t rd, uint32_t immediate, uint32_t shift) {
  return 0xD2800000u | ((shift / 16) << 21) | (immediate << 5) | rd;
}

uint32_t A64Movk64(uint32_t rd, uint32_t immediate, uint32_t shift) {
  return 0xF2800000u | ((shift / 16) << 21) | (immediate << 5) | rd;
}

uint32_t A64Movz32(uint32_t rd, uint32_t immediate, uint32_t shift) {
  return 0x52800000u | ((shift / 16) << 21) | (immediate << 5) | rd;
}

GuestInvocationReplayCodeShape HashShape(const std::vector<uint8_t>& code) {
  const std::vector<GuestInvocationReplayCodeShapeFunction> functions = {
      {kCodeAddress, kCodeAddress + 12, code.data(), code.size()}};
  GuestInvocationReplayCodeShape shape;
  std::string error;
  REQUIRE(HashGuestInvocationReplayA64CodeShape(functions, &shape, &error));
  REQUIRE(error.empty());
  return shape;
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

ExecutionJitCorpus MakeCorpus(uint32_t closure_page_count,
                              uint32_t config_flags = 0) {
  std::vector<uint8_t> encoded;
  AppendU32(&encoded, JitCorpus::kMagic);
  AppendU32(&encoded, JitCorpus::kVersion);
  AppendU32(&encoded, JitCorpus::kPageSize);
  AppendU32(&encoded, config_flags);

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

ExecutionJitCorpus MakeUniformWorkloadCorpus(
    uint32_t function_count, uint32_t guest_code_bytes_per_function,
    uint32_t captured_host_code_bytes_per_function,
    uint32_t final_guest_code_bytes = 0,
    uint32_t final_captured_host_code_bytes = 0) {
  REQUIRE(function_count > 0);
  REQUIRE(guest_code_bytes_per_function > 0);
  REQUIRE(!(guest_code_bytes_per_function & 3));
  REQUIRE((!final_guest_code_bytes || !(final_guest_code_bytes & 3)));

  const uint32_t last_guest_code_bytes = final_guest_code_bytes
                                             ? final_guest_code_bytes
                                             : guest_code_bytes_per_function;
  const uint64_t last_function_address =
      uint64_t(kCodeAddress) + uint64_t(function_count - 1) * 4;
  const uint64_t last_function_end =
      last_function_address + last_guest_code_bytes - 4;
  REQUIRE(last_function_end <= UINT32_MAX);
  const uint32_t code_page_count = static_cast<uint32_t>(
      (last_function_end - kCodeAddress) / JitCorpus::kPageSize + 1);

  std::vector<uint8_t> encoded;
  AppendU32(&encoded, JitCorpus::kMagic);
  AppendU32(&encoded, JitCorpus::kVersion);
  AppendU32(&encoded, JitCorpus::kPageSize);
  AppendU32(&encoded, 0);
  for (uint32_t i = 0; i < code_page_count; ++i) {
    AppendCorpusPage(&encoded, kCodeAddress + i * JitCorpus::kPageSize, {});
  }
  for (uint32_t i = 0; i < function_count; ++i) {
    const uint32_t guest_code_bytes =
        i + 1 == function_count && final_guest_code_bytes
            ? final_guest_code_bytes
            : guest_code_bytes_per_function;
    const uint32_t captured_host_code_bytes =
        i + 1 == function_count && final_captured_host_code_bytes
            ? final_captured_host_code_bytes
            : captured_host_code_bytes_per_function;
    const uint32_t address = kCodeAddress + i * 4;
    AppendU32(&encoded, JitCorpus::kTagFunction);
    AppendU32(&encoded, address);
    AppendU32(&encoded, address + guest_code_bytes - 4);
    AppendU32(&encoded, captured_host_code_bytes);
    AppendU32(&encoded, 0);
  }

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

ppc::GuestFunctionInvocation MakeInvocation(uint32_t closure_page_count,
                                            uint32_t function_size = 16) {
  ppc::GuestFunctionInvocation invocation;
  invocation.function_address = kCodeAddress;
  invocation.function_end_address = kCodeAddress + function_size - 4;
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

TEST_CASE("guest invocation code shape normalizes A64 ASLR materialization",
          "[guest-invocation-runner][code-shape]") {
  const std::vector<uint8_t> first = A64Code({
      A64Movz64(9, 1, 0),
      A64Movk64(9, 1, 32),
      A64Movk64(9, 1, 48),
      0x54000000u | (4u << 5),  // b.eq with one process-relative displacement
      0x8B010000u,              // add x0, x0, x1
  });
  const std::vector<uint8_t> second = A64Code({
      A64Movz64(9, 2, 0),
      A64Movk64(9, 2, 32),
      A64Movk64(9, 3, 48),
      0x54000000u | (9u << 5),  // Same branch shape, shifted native layout.
      0x8B010000u,
  });

  const GuestInvocationReplayCodeShape first_shape = HashShape(first);
  const GuestInvocationReplayCodeShape second_shape = HashShape(second);
  REQUIRE(first_shape == second_shape);
  REQUIRE(first_shape.function_count == 1);
  REQUIRE(first_shape.host_instruction_count == 5);
  REQUIRE(first_shape.wide_materialization_site_count == 1);
  REQUIRE(first_shape.pc_relative_site_count == 1);
}

TEST_CASE("guest invocation code shape retains non-ASLR A64 structure",
          "[guest-invocation-runner][code-shape]") {
  const std::vector<uint8_t> baseline =
      A64Code({A64Movz32(9, 7, 0), 0x8B010000u});
  const std::vector<uint8_t> changed_constant =
      A64Code({A64Movz32(9, 8, 0), 0x8B010000u});
  const std::vector<uint8_t> changed_opcode =
      A64Code({A64Movz32(9, 7, 0), 0xCB010000u});
  const std::vector<uint8_t> changed_address_register =
      A64Code({A64Movz64(10, 1, 0), A64Movk64(10, 1, 32), 0x8B010000u});
  const std::vector<uint8_t> baseline_address_register =
      A64Code({A64Movz64(9, 1, 0), A64Movk64(9, 1, 32), 0x8B010000u});
  const std::vector<uint8_t> changed_materialization_chain =
      A64Code({A64Movz64(9, 1, 0), A64Movk64(9, 1, 32), A64Movk64(9, 1, 48),
               0x8B010000u});

  REQUIRE(HashShape(baseline).sha256 != HashShape(changed_constant).sha256);
  REQUIRE(HashShape(baseline).sha256 != HashShape(changed_opcode).sha256);
  REQUIRE(HashShape(baseline_address_register).sha256 !=
          HashShape(changed_address_register).sha256);
  REQUIRE(HashShape(baseline_address_register).sha256 !=
          HashShape(changed_materialization_chain).sha256);
}

TEST_CASE("guest invocation code shape retains captured function order",
          "[guest-invocation-runner][code-shape]") {
  const std::vector<uint8_t> first_code = A64Code({0xD503201Fu});   // nop
  const std::vector<uint8_t> second_code = A64Code({0xD503205Fu});  // wfe
  const std::vector<GuestInvocationReplayCodeShapeFunction> forward = {
      {kCodeAddress, kCodeAddress, first_code.data(), first_code.size()},
      {kCodeAddress + 4, kCodeAddress + 4, second_code.data(),
       second_code.size()},
  };
  const std::vector<GuestInvocationReplayCodeShapeFunction> reverse = {
      forward[1], forward[0]};
  GuestInvocationReplayCodeShape forward_shape;
  GuestInvocationReplayCodeShape reverse_shape;
  std::string error;
  REQUIRE(
      HashGuestInvocationReplayA64CodeShape(forward, &forward_shape, &error));
  REQUIRE(
      HashGuestInvocationReplayA64CodeShape(reverse, &reverse_shape, &error));
  REQUIRE(forward_shape.sha256 != reverse_shape.sha256);
}

TEST_CASE("guest invocation code shape rejects malformed function streams",
          "[guest-invocation-runner][code-shape]") {
  const std::vector<uint8_t> code = A64Code({0xD503201Fu});  // nop
  GuestInvocationReplayCodeShape shape;
  std::string error;

  SECTION("empty list") {
    REQUIRE_FALSE(HashGuestInvocationReplayA64CodeShape({}, &shape, &error));
    REQUIRE(error == "code-shape function list is empty");
  }

  SECTION("unaligned native bytes") {
    const std::vector<GuestInvocationReplayCodeShapeFunction> functions = {
        {kCodeAddress, kCodeAddress, code.data(), code.size() - 1}};
    REQUIRE_FALSE(
        HashGuestInvocationReplayA64CodeShape(functions, &shape, &error));
    REQUIRE(error == "code-shape native function body is invalid");
  }

  SECTION("duplicate guest functions") {
    const std::vector<GuestInvocationReplayCodeShapeFunction> functions = {
        {kCodeAddress, kCodeAddress, code.data(), code.size()},
        {kCodeAddress, kCodeAddress + 4, code.data(), code.size()},
    };
    REQUIRE_FALSE(
        HashGuestInvocationReplayA64CodeShape(functions, &shape, &error));
    REQUIRE(error == "code-shape guest extents are invalid or duplicate");
  }
}

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
  REQUIRE(plan.eager_function_count == 1);
  REQUIRE(plan.eager_guest_code_bytes == 16);
  REQUIRE(plan.captured_host_code_bytes == 64);
  REQUIRE(plan.supplied_page_addresses.size() == 8);
  // The granule is protected whole because that is the only granularity the
  // host offers; the reset covers only the page the capture recorded.
  REQUIRE(plan.reset_page_addresses == std::vector<uint32_t>{kDataAddress});
  REQUIRE(
      plan.protection_granules ==
      std::vector<GuestInvocationReplayProtectionGranule>{
          {kDataAddress, 16 * 1024, true}, {kCodeAddress, 16 * 1024, false}});
}

TEST_CASE("guest invocation planner accepts known corpus configuration",
          "[guest-invocation-runner]") {
  ppc::GuestFunctionInvocation invocation = MakeInvocation(4);
  GuestInvocationReplayPlan plan;
  std::string error;

  ExecutionJitCorpus corpus = MakeCorpus(4, JitCorpus::kConfigGuestScheduler);
  REQUIRE(corpus.config_flags() == JitCorpus::kConfigGuestScheduler);
  REQUIRE(BuildGuestInvocationReplayPlan(invocation, corpus, 16 * 1024, &plan,
                                         &error));
  REQUIRE(error.empty());
  REQUIRE(plan.supplied_page_addresses.size() == 8);
  REQUIRE(plan.reset_page_addresses.size() == 1);
  REQUIRE(plan.protection_granules.size() == 2);

#if XE_PLATFORM_MAC && XE_ARCH_ARM64
  std::unique_ptr<GuestInvocationRunner> runner = GuestInvocationRunner::Create(
      invocation, corpus, testing::CreateBackend(), &error);
  REQUIRE(runner);
  REQUIRE(error.empty());
#endif  // XE_PLATFORM_MAC && XE_ARCH_ARM64
}

TEST_CASE("guest invocation planner bounds eager function count",
          "[guest-invocation-runner]") {
  constexpr uint32_t kFunctionLimit =
      static_cast<uint32_t>(kGuestInvocationReplayMaxEagerFunctionCount);
  ppc::GuestFunctionInvocation invocation = MakeInvocation(1, 4);
  GuestInvocationReplayPlan plan;
  std::string error;

  SECTION("accepts the exact boundary") {
    ExecutionJitCorpus corpus =
        MakeUniformWorkloadCorpus(kFunctionLimit, 4, 16);
    REQUIRE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096, &plan,
                                           &error));
    REQUIRE(error.empty());
    REQUIRE(plan.eager_function_count == kFunctionLimit);
    REQUIRE(plan.eager_guest_code_bytes == uint64_t(kFunctionLimit) * 4);
    REQUIRE(plan.captured_host_code_bytes == uint64_t(kFunctionLimit) * 16);
  }

  SECTION("rejects one function over the boundary") {
    ExecutionJitCorpus corpus =
        MakeUniformWorkloadCorpus(kFunctionLimit + 1, 4, 16);
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096,
                                                 &plan, &error));
    REQUIRE(error == "replay corpus exceeds the eager function-count budget");
    REQUIRE(plan.eager_function_count == 0);
  }
}

TEST_CASE("guest invocation planner bounds aggregate eager guest code",
          "[guest-invocation-runner]") {
  constexpr uint32_t kFunctionCount = 16;
  constexpr uint32_t kGuestBytesPerFunction = 1 * 1024 * 1024;
  static_assert(uint64_t(kFunctionCount) * kGuestBytesPerFunction ==
                kGuestInvocationReplayMaxEagerGuestCodeBytes);
  ppc::GuestFunctionInvocation invocation =
      MakeInvocation(1, kGuestBytesPerFunction);
  GuestInvocationReplayPlan plan;
  std::string error;

  SECTION("accepts the exact boundary") {
    ExecutionJitCorpus corpus =
        MakeUniformWorkloadCorpus(kFunctionCount, kGuestBytesPerFunction, 16);
    REQUIRE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096, &plan,
                                           &error));
    REQUIRE(error.empty());
    REQUIRE(plan.eager_guest_code_bytes ==
            kGuestInvocationReplayMaxEagerGuestCodeBytes);
  }

  SECTION("rejects an addition beyond the remaining budget") {
    ExecutionJitCorpus corpus = MakeUniformWorkloadCorpus(
        kFunctionCount, kGuestBytesPerFunction, 16, kGuestBytesPerFunction + 4);
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096,
                                                 &plan, &error));
    REQUIRE(error == "replay corpus exceeds the eager guest-code byte budget");
    REQUIRE(plan.eager_guest_code_bytes == 0);
  }
}

TEST_CASE("guest invocation planner bounds captured host code",
          "[guest-invocation-runner]") {
  constexpr uint32_t kHostBytesPerFunction = 64 * 1024 * 1024;
  static_assert(uint64_t(kHostBytesPerFunction) * 2 ==
                kGuestInvocationReplayMaxCapturedHostCodeBytes);
  ppc::GuestFunctionInvocation invocation = MakeInvocation(1, 4);
  GuestInvocationReplayPlan plan;
  std::string error;

  SECTION("accepts the exact boundary") {
    ExecutionJitCorpus corpus =
        MakeUniformWorkloadCorpus(2, 4, kHostBytesPerFunction);
    REQUIRE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096, &plan,
                                           &error));
    REQUIRE(error.empty());
    REQUIRE(plan.captured_host_code_bytes ==
            kGuestInvocationReplayMaxCapturedHostCodeBytes);
  }

  SECTION("rejects one aligned unit over the boundary") {
    ExecutionJitCorpus corpus = MakeUniformWorkloadCorpus(
        2, 4, kHostBytesPerFunction, 0, kHostBytesPerFunction + 4);
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096,
                                                 &plan, &error));
    REQUIRE(error ==
            "replay corpus exceeds the captured host-code byte budget");
    REQUIRE(plan.captured_host_code_bytes == 0);
  }

  SECTION("a large encoded size cannot wrap the accumulator") {
    ExecutionJitCorpus corpus =
        MakeUniformWorkloadCorpus(2, 4, kHostBytesPerFunction, 0, UINT32_MAX);
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096,
                                                 &plan, &error));
    REQUIRE(error ==
            "replay corpus exceeds the captured host-code byte budget");
    REQUIRE(plan.captured_host_code_bytes == 0);
  }

  SECTION("missing captured size cannot bypass the budget") {
    ExecutionJitCorpus corpus = MakeUniformWorkloadCorpus(1, 4, 0);
    REQUIRE_FALSE(BuildGuestInvocationReplayPlan(invocation, corpus, 4096,
                                                 &plan, &error));
    REQUIRE(error == "replay corpus function has no captured host-code size");
    REQUIRE(plan.captured_host_code_bytes == 0);
  }
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
#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE
  const GuestExecutionCaptureThreadStateRegistrySnapshot prewarm_snapshot =
      GuestInvocationRunnerTestAccess::QueryParticipants(*runner);
  REQUIRE(prewarm_snapshot.rejection ==
          GuestExecutionCaptureThreadStateRegistryRejection::kNone);
  REQUIRE(prewarm_snapshot.participants.size() == 1);
  REQUIRE(prewarm_snapshot.all_ready());
  REQUIRE(prewarm_snapshot.participants[0].participant.guest_thread_id ==
          0x100u);
#endif
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
  REQUIRE(metrics.reset_page_count_per_invocation == 1);
  REQUIRE(metrics.reset_bytes_per_invocation == JitCorpus::kPageSize);
  REQUIRE(metrics.code_shape.function_count == 1);
  REQUIRE(metrics.code_shape.host_instruction_count > 0);
#endif  // !XE_PLATFORM_MAC || !XE_ARCH_ARM64
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
