/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_recorder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace ppc {
namespace test {

namespace {

constexpr uint32_t kRootAddress = 0x82040000u;
constexpr uint32_t kRootEndAddress = kRootAddress + 0xFC;
constexpr uint32_t kNestedAddress = 0x82041000u;
constexpr uint32_t kNestedEndAddress = kNestedAddress + 0x7C;
constexpr uint32_t kDeclaredOnlyAddress = 0x82042000u;
constexpr uint32_t kDeclaredOnlyEndAddress = kDeclaredOnlyAddress + 0x3C;
constexpr uint32_t kUnrelatedAddress = 0x82043000u;
constexpr uint32_t kUnrelatedEndAddress = kUnrelatedAddress + 0x5C;
constexpr uint32_t kReturnAddress = 0x83000000u;
constexpr uint32_t kDataPageA = 0x10000000u;
constexpr uint32_t kDataPageB = kDataPageA + 0x1000;
constexpr uint32_t kDataPageC = kDataPageA + 0x2000;
constexpr uint32_t kGuestPageSize = 4096;
constexpr GuestInvocationRecorderIdentity kOwner = {0x1111, 0x2222};
constexpr GuestInvocationRecorderIdentity kOther = {0x3333, 0x4444};

class FakeClock final : public GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override { return now; }
  uint64_t now = 100;
};

class FakePageReader final : public GuestInvocationRecorderPageReader {
 public:
  bool ReadPage(uint32_t page_address,
                std::array<uint8_t, 4096>* output) override {
    last_read_retryable = false;
    ++read_count;
    read_addresses.push_back(page_address);
    if (reenter_on_read) {
      reenter_on_read = false;
      reentrant_result = reentrant_recorder->OnAsyncReentry(kOwner);
    }
    if (retryable_read_count) {
      --retryable_read_count;
      last_read_retryable = true;
      return false;
    }
    if (failed_pages.contains(page_address)) {
      return false;
    }
    const auto it = pages.find(page_address);
    if (it == pages.cend()) {
      if (page_address < 0x80000000u || page_address >= 0xA0000000u) {
        return false;
      }
      for (size_t i = 0; i < output->size(); ++i) {
        (*output)[i] = static_cast<uint8_t>((page_address >> 12) + i * 17);
      }
      return true;
    }
    *output = it->second;
    if (unstable_pages.contains(page_address)) {
      pages[page_address][0] ^= 1;
    }
    return true;
  }

  bool last_read_was_retryable() const override { return last_read_retryable; }

  void AddPage(uint32_t page_address, uint8_t seed) {
    std::array<uint8_t, 4096>& page = pages[page_address];
    for (size_t i = 0; i < page.size(); ++i) {
      page[i] = static_cast<uint8_t>(seed + i * 17);
    }
  }

  void AddPages(uint32_t first_page_address, uint32_t page_count,
                uint8_t seed) {
    for (uint32_t i = 0; i < page_count; ++i) {
      AddPage(first_page_address + i * kGuestPageSize,
              static_cast<uint8_t>(seed + i));
    }
  }

  std::map<uint32_t, std::array<uint8_t, 4096>> pages;
  std::set<uint32_t> failed_pages;
  std::set<uint32_t> unstable_pages;
  std::vector<uint32_t> read_addresses;
  uint32_t read_count = 0;
  uint32_t retryable_read_count = 0;
  GuestInvocationRecorder* reentrant_recorder = nullptr;
  bool reenter_on_read = false;
  bool reentrant_result = true;
  bool last_read_retryable = false;
};

GuestInvocationRecorderSelection MakeSelection(
    uint32_t occurrence = 1, GuestInvocationRecorderIdentity owner = {}) {
  GuestInvocationRecorderSelection selection;
  selection.root_address = kRootAddress;
  selection.root_end_address = kRootEndAddress;
  selection.occurrence = occurrence;
  selection.owner = owner;
  return selection;
}

GuestInvocationRecorderLimits MakeLimits() {
  GuestInvocationRecorderLimits limits;
  limits.max_attempts = 8;
  limits.max_duration_ticks = 1000;
  limits.max_page_count = 16;
  limits.max_access_count = 128;
  limits.max_call_depth = 8;
  limits.max_event_count = 1024;
  limits.max_function_count = 16;
  return limits;
}

GuestPPCRegisterState MakeState(uint64_t seed) {
  GuestPPCRegisterState state;
  state.link_register = kReturnAddress;
  state.count_register = seed + 1;
  state.gpr[3] = seed;
  state.fpr_bits[1] = 0x7FF8000000000000ull | seed;
  state.vector_registers[2][7] = static_cast<uint8_t>(seed);
  state.condition_register_fields[3][1] = static_cast<uint8_t>(seed + 3);
  state.fpscr = static_cast<uint32_t>(seed + 4);
  return state;
}

std::unique_ptr<GuestInvocationRecorder> MakeRecorder(
    FakePageReader& reader, FakeClock& clock,
    const GuestInvocationRecorderLimits& limits = MakeLimits(),
    const GuestInvocationRecorderSelection& selection = MakeSelection(),
    bool define_root = true) {
  std::string error;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      GuestInvocationRecorder::Create(selection, limits, reader, clock, &error);
  REQUIRE(recorder);
  REQUIRE(error.empty());
  if (define_root) {
    REQUIRE(recorder->OnFunctionDefined(selection.root_address,
                                        selection.root_end_address));
  }
  return recorder;
}

void Define(GuestInvocationRecorder& recorder, uint32_t address,
            uint32_t end_address) {
  REQUIRE(recorder.OnFunctionDefined(address, end_address));
}

void EnterRoot(GuestInvocationRecorder& recorder,
               const GuestPPCRegisterState& state = MakeState(1)) {
  REQUIRE(
      recorder.OnFunctionEntry(kOwner, kRootAddress, kRootEndAddress, state));
}

void ExitRoot(GuestInvocationRecorder& recorder,
              const GuestPPCRegisterState& state = MakeState(2)) {
  REQUIRE(recorder.OnFunctionExit(kOwner, kRootAddress, kReturnAddress, state));
}

void Access(GuestInvocationRecorder& recorder, uint32_t page_address,
            GuestInvocationRecorderMemoryAccess access =
                GuestInvocationRecorderMemoryAccess::kRead) {
  REQUIRE(recorder.OnMemoryAccess(kOwner, page_address + 16, 8, access));
}

void DiscoveryAttempt(GuestInvocationRecorder& recorder,
                      const std::vector<uint32_t>& pages) {
  EnterRoot(recorder);
  for (uint32_t page : pages) {
    Access(recorder, page);
  }
  ExitRoot(recorder);
}

void ConvergeOnPage(GuestInvocationRecorder& recorder,
                    uint32_t page_address = kDataPageA) {
  DiscoveryAttempt(recorder, {page_address});
  DiscoveryAttempt(recorder, {page_address});
  REQUIRE(recorder.state() ==
          GuestInvocationRecorderState::kWaitingForFinalAttempt);
}

size_t ReadCountForPage(const FakePageReader& reader, uint32_t page_address) {
  return static_cast<size_t>(std::count(reader.read_addresses.cbegin(),
                                        reader.read_addresses.cend(),
                                        page_address));
}

void RequireRejected(const GuestInvocationRecorder& recorder,
                     GuestInvocationRecorderRejection rejection,
                     uint32_t dependency_flags = 0) {
  REQUIRE(recorder.state() == GuestInvocationRecorderState::kRejected);
  REQUIRE(recorder.rejection() == rejection);
  REQUIRE(recorder.rejected_dependency_flags() == dependency_flags);
  REQUIRE_FALSE(recorder.rejection_message().empty());
  REQUIRE(recorder.result() == nullptr);
}

void RequireClosedGranuleCapture(uint32_t host_page_size) {
  const uint32_t pages_per_granule = host_page_size / kGuestPageSize;
  FakePageReader reader;
  reader.AddPages(kRootAddress, pages_per_granule, 0x20);
  reader.AddPages(kDataPageA, pages_per_granule, 0x40);
  FakeClock clock;
  GuestInvocationRecorderLimits limits = MakeLimits();
  limits.host_protection_page_size = host_page_size;
  limits.max_page_count = pages_per_granule;
  limits.max_code_page_count = pages_per_granule;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      MakeRecorder(reader, clock, limits);

  ConvergeOnPage(*recorder);
  EnterRoot(*recorder, MakeState(30));
  Access(*recorder, kDataPageA);
  ExitRoot(*recorder, MakeState(31));

  REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
  const GuestInvocationRecorderResult* result = recorder->result();
  REQUIRE(result);
  REQUIRE(result->code_pages.size() == pages_per_granule);
  REQUIRE(result->invocation.input_data_pages.size() == pages_per_granule);
  REQUIRE(
      (result->touched_page_addresses == std::vector<uint32_t>{kDataPageA}));
  for (uint32_t i = 0; i < pages_per_granule; ++i) {
    REQUIRE(result->code_pages[i].guest_address ==
            kRootAddress + i * kGuestPageSize);
    REQUIRE(result->invocation.input_data_pages[i].guest_address ==
            kDataPageA + i * kGuestPageSize);
  }
}

}  // namespace

TEST_CASE("guest invocation recorder validates explicit selection and limits",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  FakeClock clock;
  std::string error;

  SECTION("root must be aligned") {
    GuestInvocationRecorderSelection selection = MakeSelection();
    selection.root_address += 2;
    REQUIRE_FALSE(GuestInvocationRecorder::Create(selection, MakeLimits(),
                                                  reader, clock, &error));
    REQUIRE(error.find("aligned root") != std::string::npos);
  }
  SECTION("occurrence is one based") {
    GuestInvocationRecorderSelection selection = MakeSelection(0);
    REQUIRE_FALSE(GuestInvocationRecorder::Create(selection, MakeLimits(),
                                                  reader, clock, &error));
    REQUIRE(error.find("1-based") != std::string::npos);
  }
  SECTION("owner is either omitted or fully specified") {
    GuestInvocationRecorderSelection selection = MakeSelection(1, kOwner);
    selection.owner.thread_id = 0;
    REQUIRE_FALSE(GuestInvocationRecorder::Create(selection, MakeLimits(),
                                                  reader, clock, &error));
    REQUIRE(error.find("owner") != std::string::npos);
  }
  SECTION("omitted owner is accepted for occurrence claim") {
    REQUIRE(GuestInvocationRecorder::Create(MakeSelection(), MakeLimits(),
                                            reader, clock, &error));
    REQUIRE(error.empty());
  }
  SECTION("host protection granule is a bounded power of two") {
    for (uint32_t invalid_size : {0u, 2048u, 12288u, 128u * 1024}) {
      GuestInvocationRecorderLimits limits = MakeLimits();
      limits.host_protection_page_size = invalid_size;
      REQUIRE_FALSE(GuestInvocationRecorder::Create(MakeSelection(), limits,
                                                    reader, clock, &error));
      REQUIRE_FALSE(error.empty());
    }
    for (uint32_t valid_size : {4096u, 8192u, 16384u, 32768u, 65536u}) {
      GuestInvocationRecorderLimits limits = MakeLimits();
      limits.host_protection_page_size = valid_size;
      REQUIRE(GuestInvocationRecorder::Create(MakeSelection(), limits, reader,
                                              clock, &error));
      REQUIRE(error.empty());
    }
  }
  SECTION("three attempts are the minimum") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_attempts = 2;
    REQUIRE_FALSE(GuestInvocationRecorder::Create(MakeSelection(), limits,
                                                  reader, clock, &error));
    REQUIRE_FALSE(error.empty());
  }
  SECTION("deadline addition cannot wrap") {
    clock.now = std::numeric_limits<uint64_t>::max() - 1;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_duration_ticks = 2;
    REQUIRE_FALSE(GuestInvocationRecorder::Create(MakeSelection(), limits,
                                                  reader, clock, &error));
    REQUIRE(error.find("overflows") != std::string::npos);
  }
  SECTION("definition code-page catalog must be bounded") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_code_page_count = 0;
    REQUIRE_FALSE(GuestInvocationRecorder::Create(MakeSelection(), limits,
                                                  reader, clock, &error));
    REQUIRE_FALSE(error.empty());
  }
}

TEST_CASE("guest invocation recorder closes host protection granules",
          "[guest-invocation-recorder]") {
  SECTION("4 KiB") { RequireClosedGranuleCapture(4 * 1024); }
  SECTION("16 KiB") { RequireClosedGranuleCapture(16 * 1024); }
  SECTION("64 KiB") { RequireClosedGranuleCapture(64 * 1024); }
}

TEST_CASE("guest invocation recorder closes boundary-spanning ranges",
          "[guest-invocation-recorder]") {
  SECTION("function spans two host granules") {
    constexpr uint32_t kBoundaryRoot = 0x82043F00u;
    constexpr uint32_t kBoundaryRootEnd = 0x820440FCu;
    FakePageReader reader;
    FakeClock clock;
    GuestInvocationRecorderSelection selection = MakeSelection();
    selection.root_address = kBoundaryRoot;
    selection.root_end_address = kBoundaryRootEnd;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits, selection);

    auto attempt = [&] {
      REQUIRE(recorder->OnFunctionEntry(kOwner, kBoundaryRoot, kBoundaryRootEnd,
                                        MakeState(1)));
      REQUIRE(recorder->OnFunctionExit(kOwner, kBoundaryRoot, kReturnAddress,
                                       MakeState(2)));
    };
    attempt();
    attempt();
    attempt();

    REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
    REQUIRE(recorder->result());
    REQUIRE(recorder->result()->code_pages.size() == 8);
    for (uint32_t i = 0; i < 8; ++i) {
      REQUIRE(recorder->result()->code_pages[i].guest_address ==
              0x82040000u + i * kGuestPageSize);
    }
  }

  SECTION("access spans two host granules") {
    FakePageReader reader;
    reader.AddPages(kDataPageA, 8, 0x10);
    FakeClock clock;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    limits.max_page_count = 8;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);

    auto attempt = [&] {
      EnterRoot(*recorder);
      REQUIRE(
          recorder->OnMemoryAccess(kOwner, kDataPageA + 0x3FF8, 16,
                                   GuestInvocationRecorderMemoryAccess::kRead));
      ExitRoot(*recorder);
    };
    attempt();
    attempt();
    attempt();

    REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
    const GuestInvocationRecorderResult* result = recorder->result();
    REQUIRE(result);
    REQUIRE(result->invocation.input_data_pages.size() == 8);
    REQUIRE((result->touched_page_addresses ==
             std::vector<uint32_t>{kDataPageA + 0x3000, kDataPageA + 0x4000}));
    for (uint32_t i = 0; i < 8; ++i) {
      REQUIRE(result->invocation.input_data_pages[i].guest_address ==
              kDataPageA + i * kGuestPageSize);
    }
  }
}

TEST_CASE("guest invocation recorder fails closed on granule hazards",
          "[guest-invocation-recorder]") {
  FakeClock clock;

  SECTION("code closure exceeds its bound") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    limits.max_code_page_count = 3;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits, MakeSelection(), false);
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }

  SECTION("data closure exceeds its bound") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    limits.max_page_count = 3;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }

  SECTION("code closure sibling is unreadable") {
    FakePageReader reader;
    reader.failed_pages.insert(kRootAddress + kGuestPageSize);
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits, MakeSelection(), false);
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kPageReadFailure,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }

  SECTION("data closure sibling is unreadable") {
    FakePageReader reader;
    reader.AddPage(kDataPageA, 1);
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    ConvergeOnPage(*recorder);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(3)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kPageReadFailure,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }

  SECTION("data access enters a closure-only code sibling") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(
        recorder->OnMemoryAccess(kOwner, kRootAddress + kGuestPageSize, 4,
                                 GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }

  SECTION("write enters a closure-only code sibling") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(
        recorder->OnMemoryAccess(kOwner, kRootAddress + kGuestPageSize, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }

  SECTION("data aliases a code granule") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, 0x92040000u, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyPhysicalAlias);
  }

  SECTION("cross-thread write touches a data closure sibling") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOther, kDataPageB, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }

  SECTION("data closure discovers a prior cross-thread sibling write") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOther, kDataPageB, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }

  SECTION("code closure rejects aliased function pages") {
    constexpr uint32_t kAliasedRootAddress = 0x92040000u;
    constexpr uint32_t kAliasedRootEndAddress = kAliasedRootAddress + 0xFC;
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits, MakeSelection(), false);
    REQUIRE(recorder->OnFunctionDependency(kRootAddress, kAliasedRootAddress));
    Define(*recorder, kAliasedRootAddress, kAliasedRootEndAddress);
    Define(*recorder, kRootAddress, kRootEndAddress);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(1)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyPhysicalAlias);
  }

  SECTION("cross-thread watches share the data closure bound") {
    FakePageReader reader;
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    limits.max_page_count = 4;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE(
        recorder->OnMemoryAccess(kOther, kDataPageA + 0x10000, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }

  SECTION("untouched closure sibling changes during the final attempt") {
    FakePageReader reader;
    reader.AddPages(kDataPageA, 4, 1);
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.host_protection_page_size = 16 * 1024;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    ConvergeOnPage(*recorder);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    reader.pages[kDataPageB][0] ^= 1;
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                                           MakeState(4)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }
}

TEST_CASE("guest invocation recorder converges and builds a complete result",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 0x10);
  reader.AddPage(kDataPageB, 0x20);
  reader.AddPage(kRootAddress, 0x30);
  reader.AddPage(kNestedAddress, 0x40);
  reader.AddPage(kDeclaredOnlyAddress, 0x50);
  reader.AddPage(kUnrelatedAddress, 0x60);
  FakeClock clock;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      MakeRecorder(reader, clock, MakeLimits(), MakeSelection(2), false);

  REQUIRE(recorder->OnFunctionDependency(kRootAddress, kDeclaredOnlyAddress));
  Define(*recorder, kUnrelatedAddress, kUnrelatedEndAddress);
  Define(*recorder, kDeclaredOnlyAddress, kDeclaredOnlyEndAddress);
  Define(*recorder, kNestedAddress, kNestedEndAddress);
  Define(*recorder, kRootAddress, kRootEndAddress);

  // The first global root occurrence is ignored. The selected second
  // occurrence claims a different context/thread as the retry owner.
  REQUIRE(recorder->OnFunctionEntry(kOther, kRootAddress, kRootEndAddress,
                                    MakeState(10)));
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForOccurrence);
  REQUIRE(recorder->OnFunctionExit(kOther, kRootAddress, kReturnAddress,
                                   MakeState(11)));

  EnterRoot(*recorder, MakeState(20));
  GuestPPCRegisterState nested_entry = MakeState(21);
  nested_entry.link_register = kRootAddress + 8;
  REQUIRE(recorder->OnFunctionEntry(kOwner, kNestedAddress, kNestedEndAddress,
                                    nested_entry));
  Access(*recorder, kDataPageA);
  Access(*recorder, kDataPageB, GuestInvocationRecorderMemoryAccess::kWrite);
  REQUIRE(recorder->OnFunctionExit(kOwner, kNestedAddress, kRootAddress + 8,
                                   MakeState(22)));
  ExitRoot(*recorder, MakeState(23));
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForDiscoveryAttempt);

  DiscoveryAttempt(*recorder, {kDataPageA, kDataPageB});
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForFinalAttempt);
  REQUIRE(ReadCountForPage(reader, kDataPageA) == 0);
  REQUIRE(ReadCountForPage(reader, kDataPageB) == 0);

  const GuestPPCRegisterState final_input = MakeState(30);
  EnterRoot(*recorder, final_input);
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kRecordingFinalAttempt);
  REQUIRE(ReadCountForPage(reader, kDataPageA) == 1);
  REQUIRE(ReadCountForPage(reader, kDataPageB) == 1);
  Access(*recorder, kDataPageA);
  Access(*recorder, kDataPageB, GuestInvocationRecorderMemoryAccess::kWrite);
  reader.pages[kDataPageB][77] ^= 0xFF;
  const GuestPPCRegisterState final_output = MakeState(31);
  ExitRoot(*recorder, final_output);

  REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
  REQUIRE(recorder->rejection() == GuestInvocationRecorderRejection::kNone);
  const GuestInvocationRecorderResult* result = recorder->result();
  REQUIRE(result);
  REQUIRE(result->owner == kOwner);
  REQUIRE(result->attempt_count == 3);
  REQUIRE((result->touched_page_addresses ==
           std::vector<uint32_t>{kDataPageA, kDataPageB}));
  REQUIRE((result->translation_dependencies ==
           std::vector<GuestInvocationRecorderFunction>{
               {kDeclaredOnlyAddress, kDeclaredOnlyEndAddress},
               {kNestedAddress, kNestedEndAddress},
               {kRootAddress, kRootEndAddress}}));
  REQUIRE(result->code_pages.size() == 3);
  REQUIRE(result->code_pages[0].guest_address == kRootAddress);
  REQUIRE(result->code_pages[0].data == reader.pages[kRootAddress]);
  REQUIRE(result->code_pages[1].guest_address == kNestedAddress);
  REQUIRE(result->code_pages[1].data == reader.pages[kNestedAddress]);
  REQUIRE(result->code_pages[2].guest_address == kDeclaredOnlyAddress);
  REQUIRE(result->code_pages[2].data == reader.pages[kDeclaredOnlyAddress]);
  REQUIRE((result->entered_functions ==
           std::vector<GuestInvocationRecorderFunction>{
               {kRootAddress, kRootEndAddress},
               {kNestedAddress, kNestedEndAddress}}));

  const GuestFunctionInvocation& invocation = result->invocation;
  REQUIRE(invocation.function_address == kRootAddress);
  REQUIRE(invocation.function_end_address == kRootEndAddress);
  REQUIRE(invocation.entry_address == kRootAddress);
  REQUIRE(invocation.expected_return_address == kReturnAddress);
  REQUIRE(invocation.dependency_flags == 0);
  REQUIRE(invocation.input == final_input);
  REQUIRE(invocation.expected_output == final_output);
  REQUIRE(invocation.input_data_pages.size() == 2);
  REQUIRE(invocation.input_data_pages[0].guest_address == kDataPageA);
  REQUIRE(invocation.input_data_pages[1].guest_address == kDataPageB);
  REQUIRE(invocation.expected_dirty_pages.size() == 1);
  REQUIRE(invocation.expected_dirty_pages[0].guest_address == kDataPageB);
  REQUIRE(invocation.expected_dirty_pages[0].data == reader.pages[kDataPageB]);
  REQUIRE(ReadCountForPage(reader, kDataPageA) == 2);
  REQUIRE(ReadCountForPage(reader, kDataPageB) == 2);

  GuestInvocationArtifact artifact;
  artifact.capture_build_sha256.fill(1);
  artifact.code_corpus_sha256.fill(2);
  artifact.replay_config_sha256.fill(3);
  artifact.invocations.push_back(invocation);
  std::vector<uint8_t> encoded;
  std::string error;
  REQUIRE(GuestInvocationArtifactCodec::Encode(artifact, &encoded, &error));
  REQUIRE(error.empty());
  REQUIRE_FALSE(encoded.empty());
}

TEST_CASE("guest invocation recorder retries contended definition snapshots",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kRootAddress, 0x30);
  reader.retryable_read_count = 1;
  FakeClock clock;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      MakeRecorder(reader, clock);

  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForOccurrence);
  REQUIRE(recorder->rejection() == GuestInvocationRecorderRejection::kNone);
  REQUIRE(reader.read_count == 1);
  REQUIRE(recorder->Poll());
  REQUIRE(reader.read_count == 3);

  DiscoveryAttempt(*recorder, {});
  DiscoveryAttempt(*recorder, {});
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForFinalAttempt);
  EnterRoot(*recorder, MakeState(30));
  ExitRoot(*recorder, MakeState(31));

  REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
  REQUIRE(recorder->rejection() == GuestInvocationRecorderRejection::kNone);
  REQUIRE(recorder->result());
  REQUIRE(recorder->result()->code_pages.size() == 1);
  REQUIRE(recorder->result()->code_pages[0].guest_address == kRootAddress);
  REQUIRE(recorder->result()->code_pages[0].data == reader.pages[kRootAddress]);
}

TEST_CASE("guest invocation recorder rejects writes before deferred snapshot",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kRootAddress, 0x30);
  reader.retryable_read_count = 1;
  FakeClock clock;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      MakeRecorder(reader, clock);

  REQUIRE(recorder->OnMemoryAccess(
      kOther, kRootAddress, 4, GuestInvocationRecorderMemoryAccess::kWrite));
  REQUIRE_FALSE(recorder->Poll());
  RequireRejected(*recorder,
                  GuestInvocationRecorderRejection::kSelfModifyingCode,
                  kGuestInvocationDependencySelfModifyingCode);
}

TEST_CASE("guest invocation recorder claims owner and validates registry order",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  FakeClock clock;

  SECTION("configured owner must match the selected global occurrence") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, MakeLimits(), MakeSelection(1, kOwner));
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOther, kRootAddress,
                                            kRootEndAddress, MakeState(1)));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAsyncReentry,
                    kGuestInvocationDependencyAsyncInterrupt);
  }
  SECTION("root must have a successful definition") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(1)));
    RequireRejected(
        *recorder,
        GuestInvocationRecorderRejection::kIncompleteTranslationClosure);
  }
  SECTION("a declared-only dependency stays out of the selected closure") {
    reader.AddPage(kDataPageA, 1);
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE(recorder->OnFunctionDependency(kRootAddress, kDeclaredOnlyAddress));
    Define(*recorder, kRootAddress, kRootEndAddress);
    ConvergeOnPage(*recorder);
    EnterRoot(*recorder, MakeState(3));
    Access(*recorder, kDataPageA);
    ExitRoot(*recorder, MakeState(4));
    REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
    const GuestInvocationRecorderResult* result = recorder->result();
    REQUIRE(result);
    REQUIRE((result->translation_dependencies ==
             std::vector<GuestInvocationRecorderFunction>{
                 {kRootAddress, kRootEndAddress}}));
    REQUIRE(result->code_pages.size() == 1);
    REQUIRE(result->code_pages[0].guest_address == kRootAddress);
  }
  SECTION("dependency edges must precede their source definition") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    REQUIRE_FALSE(recorder->OnFunctionDependency(kRootAddress, kNestedAddress));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kInvalidEvent);
  }
  SECTION("a successful definition is reported exactly once") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kInvalidEvent);
  }
}

TEST_CASE("guest invocation discovery requires exact consecutive page sets",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 1);
  reader.AddPage(kDataPageB, 2);
  FakeClock clock;
  std::unique_ptr<GuestInvocationRecorder> recorder =
      MakeRecorder(reader, clock);

  DiscoveryAttempt(*recorder, {kDataPageA});
  DiscoveryAttempt(*recorder, {kDataPageA, kDataPageB});
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForDiscoveryAttempt);
  DiscoveryAttempt(*recorder, {kDataPageA, kDataPageB});
  REQUIRE(recorder->state() ==
          GuestInvocationRecorderState::kWaitingForFinalAttempt);

  EnterRoot(*recorder);
  REQUIRE_FALSE(recorder->OnMemoryAccess(
      kOwner, kDataPageC, 4, GuestInvocationRecorderMemoryAccess::kRead));
  RequireRejected(*recorder,
                  GuestInvocationRecorderRejection::kIncompletePageDiscovery,
                  kGuestInvocationDependencyIncompletePageDiscovery);
}

TEST_CASE("guest invocation recorder rejects every resource bound",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 1);
  reader.AddPage(kDataPageB, 2);
  FakeClock clock;

  SECTION("deadline") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    clock.now += MakeLimits().max_duration_ticks;
    REQUIRE_FALSE(recorder->Poll());
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kDeadlineExceeded);
  }
  SECTION("deadline on ignored function entry") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    clock.now += MakeLimits().max_duration_ticks;
    REQUIRE_FALSE(recorder->OnFunctionEntry(
        kOther, kUnrelatedAddress, kUnrelatedEndAddress, MakeState(1)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kDeadlineExceeded);
  }
  SECTION("deadline on pre-attempt write") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    clock.now += MakeLimits().max_duration_ticks;
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOther, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kDeadlineExceeded);
  }
  SECTION("attempts") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_attempts = 3;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    DiscoveryAttempt(*recorder, {kDataPageA});
    DiscoveryAttempt(*recorder, {kDataPageB});
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                                           MakeState(2)));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAttemptLimit);
  }
  SECTION("pages") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_page_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(
        recorder->OnMemoryAccess(kOwner, kDataPageA + 4092, 8,
                                 GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }
  SECTION("accesses") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_access_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAccessLimit);
  }
  SECTION("call depth") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_call_depth = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kNestedAddress,
                                            kNestedEndAddress, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCallDepthLimit);
  }
  SECTION("events") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_event_count = 2;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kEventLimit);
  }
  SECTION("functions") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_function_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    REQUIRE_FALSE(recorder->OnFunctionDependency(kRootAddress, kNestedAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kFunctionLimit);
  }
}

TEST_CASE("guest invocation recorder rejects abnormal control flow",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  FakeClock clock;

  SECTION("recursion") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kRecursiveInvocation,
                    kGuestInvocationDependencyRecursiveInvocation);
  }
  SECTION("unbalanced return") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    Define(*recorder, kNestedAddress, kNestedEndAddress);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnFunctionEntry(kOwner, kNestedAddress, kNestedEndAddress,
                                      MakeState(2)));
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                                           MakeState(3)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnbalancedReturn,
                    kGuestInvocationDependencyUnbalancedReturn);
  }
  SECTION("wrong root continuation") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kRootAddress,
                                           kReturnAddress + 4, MakeState(3)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnbalancedReturn,
                    kGuestInvocationDependencyUnbalancedReturn);
  }
  SECTION("wrong nested continuation") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    Define(*recorder, kNestedAddress, kNestedEndAddress);
    EnterRoot(*recorder);
    GuestPPCRegisterState nested_entry = MakeState(2);
    nested_entry.link_register = kRootAddress + 8;
    REQUIRE(recorder->OnFunctionEntry(kOwner, kNestedAddress, kNestedEndAddress,
                                      nested_entry));
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kNestedAddress,
                                           kRootAddress + 12, MakeState(3)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnbalancedReturn,
                    kGuestInvocationDependencyUnbalancedReturn);
  }
  SECTION("tail call replaces the calling frame") {
    reader.AddPage(kDataPageA, 1);
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    Define(*recorder, kNestedAddress, kNestedEndAddress);
    // A non-linking branch leaves LR holding whatever the caller last put
    // there, so the target's entry reports a link register that is not a
    // return boundary at all and may point into the target itself.
    GuestPPCRegisterState tail_entry = MakeState(2);
    tail_entry.link_register = kNestedAddress;
    for (int attempt = 0; attempt < 2; ++attempt) {
      EnterRoot(*recorder);
      REQUIRE(recorder->OnTailCall(kOwner, kRootAddress, kNestedAddress));
      REQUIRE(recorder->OnFunctionEntry(kOwner, kNestedAddress,
                                        kNestedEndAddress, tail_entry));
      Access(*recorder, kDataPageA);
      REQUIRE(recorder->OnFunctionExit(kOwner, kNestedAddress, kReturnAddress,
                                       MakeState(3)));
    }
    REQUIRE(recorder->state() ==
            GuestInvocationRecorderState::kWaitingForFinalAttempt);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnTailCall(kOwner, kRootAddress, kNestedAddress));
    REQUIRE(recorder->OnFunctionEntry(kOwner, kNestedAddress, kNestedEndAddress,
                                      tail_entry));
    Access(*recorder, kDataPageA);
    REQUIRE(recorder->OnFunctionExit(kOwner, kNestedAddress, kReturnAddress,
                                     MakeState(3)));
    REQUIRE(recorder->state() == GuestInvocationRecorderState::kComplete);
    const GuestInvocationRecorderResult* result = recorder->result();
    REQUIRE(result);
    REQUIRE(result->invocation.function_address == kRootAddress);
    REQUIRE(result->invocation.expected_return_address == kReturnAddress);
    REQUIRE((result->entered_functions ==
             std::vector<GuestInvocationRecorderFunction>{
                 {kRootAddress, kRootEndAddress},
                 {kNestedAddress, kNestedEndAddress}}));
  }
  SECTION("tail call must leave the recorded call stack") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    Define(*recorder, kNestedAddress, kNestedEndAddress);
    EnterRoot(*recorder);
    REQUIRE_FALSE(
        recorder->OnTailCall(kOwner, kNestedAddress, kUnrelatedAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnbalancedReturn,
                    kGuestInvocationDependencyUnbalancedReturn);
  }
  SECTION("longjmp") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnUnwindOrLongjmp(kOwner));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnbalancedReturn,
                    kGuestInvocationDependencyUnbalancedReturn);
  }
  SECTION("async reentry") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnAsyncReentry(kOwner));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAsyncReentry,
                    kGuestInvocationDependencyAsyncInterrupt);
  }
}

TEST_CASE("guest invocation recorder rejects dependencies and code mutation",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 1);
  FakeClock clock;

  SECTION("known dependency") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnUnsupportedDependency(
        kOwner, kGuestInvocationDependencyMmio));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyMmio);
  }
  SECTION("unknown dependency") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnUnsupportedDependency(kOwner, 1u << 31));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    1u << 31);
  }
  SECTION("write overlaps declared code") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE(recorder->OnFunctionDependency(kRootAddress, kNestedAddress));
    Define(*recorder, kNestedAddress, kNestedEndAddress);
    Define(*recorder, kRootAddress, kRootEndAddress);
    EnterRoot(*recorder);
    REQUIRE_FALSE(
        recorder->OnMemoryAccess(kOwner, kNestedAddress + 4, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("later declaration overlaps an earlier write") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA, GuestInvocationRecorderMemoryAccess::kWrite);
    Define(*recorder, kDataPageA, kDataPageA + 0xFC);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kDataPageA,
                                            kDataPageA + 0xFC, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("owner code writes between attempts remain hazardous") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    DiscoveryAttempt(*recorder, {kDataPageA});
    REQUIRE(
        recorder->OnMemoryAccess(kOwner, kRootAddress + 0x200, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("a pre-occurrence guest code write is retained across an ABA") {
    reader.AddPage(kRootAddress, 0x21);
    const auto original = reader.pages[kRootAddress];
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    REQUIRE(
        recorder->OnMemoryAccess(kOther, kRootAddress + 4, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    reader.pages[kRootAddress][4] ^= 1;
    reader.pages[kRootAddress] = original;
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("an unreported pre-occurrence code mutation fails live validation") {
    reader.AddPage(kRootAddress, 0x22);
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    reader.pages[kRootAddress][4] ^= 1;
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("later entered code intersects an earlier cross-thread write") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOther, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    Define(*recorder, kDataPageA, kDataPageA + 0xFC);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kDataPageA,
                                            kDataPageA + 0xFC, MakeState(2)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode |
                        kGuestInvocationDependencyCrossThreadMutation);
  }
}

TEST_CASE("guest invocation recorder rejects cross-thread closure writes",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 1);
  FakeClock clock;

  SECTION("write follows owner access") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOther, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }
  SECTION("write precedes owner access") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOther, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }
  SECTION("owner identity aliases are async reentry") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    const GuestInvocationRecorderIdentity wrong_thread = {kOwner.context_id,
                                                          kOther.thread_id};
    REQUIRE_FALSE(
        recorder->OnMemoryAccess(wrong_thread, kDataPageA, 4,
                                 GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAsyncReentry,
                    kGuestInvocationDependencyAsyncInterrupt);
  }
  SECTION("irrelevant reads do not consume the capture access budget") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_access_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    for (uint32_t i = 0; i < 32; ++i) {
      REQUIRE(
          recorder->OnMemoryAccess(kOther, kDataPageB + i * 4, 4,
                                   GuestInvocationRecorderMemoryAccess::kRead));
    }
    Access(*recorder, kDataPageA);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAccessLimit);
  }
  SECTION(
      "owner writes between attempts do not consume the active access budget") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_access_count = 2;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    DiscoveryAttempt(*recorder, {kDataPageB});
    for (uint32_t i = 0; i < 32; ++i) {
      REQUIRE(recorder->OnMemoryAccess(
          kOwner, kDataPageB + i * 4, 4,
          GuestInvocationRecorderMemoryAccess::kWrite));
    }
    DiscoveryAttempt(*recorder, {kDataPageB});
    REQUIRE(recorder->state() ==
            GuestInvocationRecorderState::kWaitingForFinalAttempt);
  }
  SECTION("cross-thread writes between attempts remain in the watch set") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    DiscoveryAttempt(*recorder, {kDataPageB});
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOther, kDataPageB, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }
  SECTION("cross-thread pages share the total page budget") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_page_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOther, kDataPageA, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, kDataPageB, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }
  SECTION("cross-thread physical aliases intersect the closure") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOther, 0x92000000u, 4, GuestInvocationRecorderMemoryAccess::kWrite));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, 0x82000000u, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kCrossThreadMutation,
                    kGuestInvocationDependencyCrossThreadMutation);
  }
}

TEST_CASE("guest invocation recorder fails closed on snapshot and page hazards",
          "[guest-invocation-recorder]") {
  FakePageReader reader;
  reader.AddPage(kDataPageA, 1);
  reader.AddPage(kRootAddress, 2);
  FakeClock clock;

  SECTION("page snapshot failure") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    ConvergeOnPage(*recorder);
    reader.failed_pages.insert(kDataPageA);
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(3)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kPageReadFailure,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }
  SECTION("unstable successful-definition bytes") {
    FakePageReader unstable_reader;
    unstable_reader.AddPage(kRootAddress, 3);
    unstable_reader.unstable_pages.insert(kRootAddress);
    std::unique_ptr<GuestInvocationRecorder> recorder = MakeRecorder(
        unstable_reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("successful definition page must be readable") {
    FakePageReader failed_reader;
    failed_reader.failed_pages.insert(kRootAddress);
    std::unique_ptr<GuestInvocationRecorder> recorder = MakeRecorder(
        failed_reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kPageReadFailure,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }
  SECTION("shared-page definitions keep one immutable version") {
    constexpr uint32_t kSharedAddress = kRootAddress + 0x200;
    constexpr uint32_t kSharedEndAddress = kSharedAddress + 0x3C;
    reader.AddPage(kRootAddress, 4);
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, MakeLimits(), MakeSelection(), false);
    REQUIRE(recorder->OnFunctionDependency(kRootAddress, kSharedAddress));
    Define(*recorder, kSharedAddress, kSharedEndAddress);
    reader.pages[kRootAddress][0] ^= 1;
    REQUIRE_FALSE(recorder->OnFunctionDefined(kRootAddress, kRootEndAddress));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
  SECTION("definition code pages have an independent hard bound") {
    GuestInvocationRecorderLimits limits = MakeLimits();
    limits.max_code_page_count = 1;
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock, limits, MakeSelection(), false);
    REQUIRE_FALSE(
        recorder->OnFunctionDefined(kRootAddress, kRootAddress + 0x1000));
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kPageLimit,
                    kGuestInvocationDependencyPageDiscoveryOverflow);
  }
  SECTION("serialized callback stream cannot reenter through page reader") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    ConvergeOnPage(*recorder);
    reader.reentrant_recorder = recorder.get();
    reader.reenter_on_read = true;
    REQUIRE_FALSE(recorder->OnFunctionEntry(kOwner, kRootAddress,
                                            kRootEndAddress, MakeState(3)));
    REQUIRE_FALSE(reader.reentrant_result);
    RequireRejected(*recorder, GuestInvocationRecorderRejection::kAsyncReentry,
                    kGuestInvocationDependencyAsyncInterrupt);
  }
  SECTION("unsupported page") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, 0x100, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }
  SECTION("physical alias") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    EnterRoot(*recorder);
    REQUIRE(recorder->OnMemoryAccess(
        kOwner, 0x82000000u, 4, GuestInvocationRecorderMemoryAccess::kRead));
    REQUIRE_FALSE(recorder->OnMemoryAccess(
        kOwner, 0x92000000u, 4, GuestInvocationRecorderMemoryAccess::kRead));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kUnsupportedDependency,
                    kGuestInvocationDependencyPhysicalAlias);
  }
  SECTION("unreported final code change") {
    std::unique_ptr<GuestInvocationRecorder> recorder =
        MakeRecorder(reader, clock);
    ConvergeOnPage(*recorder, kDataPageA);
    EnterRoot(*recorder);
    Access(*recorder, kDataPageA);
    reader.pages[kRootAddress][0] ^= 1;
    REQUIRE_FALSE(recorder->OnFunctionExit(kOwner, kRootAddress, kReturnAddress,
                                           MakeState(4)));
    RequireRejected(*recorder,
                    GuestInvocationRecorderRejection::kSelfModifyingCode,
                    kGuestInvocationDependencySelfModifyingCode);
  }
}

}  // namespace test
}  // namespace ppc
}  // namespace cpu
}  // namespace xe
