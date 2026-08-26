/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdint>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/testing/util.h"

namespace xe::cpu::testing {
namespace {

class ScopedBoolCvar {
 public:
  ScopedBoolCvar(bool& cvar, bool value) : cvar_(cvar), old_value_(cvar) {
    cvar_ = value;
  }
  ~ScopedBoolCvar() { cvar_ = old_value_; }

 private:
  bool& cvar_;
  bool old_value_;
};

uint32_t RunReadonlyGuestLoad(bool fold_readonly_load) {
  ScopedBoolCvar fold_control(cvars::fold_readonly_guest_memory_loads,
                              fold_readonly_load);

  uint32_t guest_address = 0;
  TestFunction test([&guest_address](hir::HIRBuilder& b) {
    auto address =
        b.ZeroExtend(b.LoadConstantInt32(guest_address), hir::INT64_TYPE);
    auto value = b.Load(address, hir::INT32_TYPE);
    StoreGPR(b, 3, b.ZeroExtend(value, hir::INT64_TYPE));
    b.Return();
  });

  guest_address =
      test.memory->SystemHeapAlloc(sizeof(uint32_t), alignof(uint32_t));
  REQUIRE(guest_address != 0);
  auto* heap = test.memory->LookupHeap(guest_address);
  REQUIRE(heap != nullptr);
  auto* host_address =
      reinterpret_cast<uint32_t*>(test.memory->TranslateVirtual(guest_address));

  constexpr uint32_t kCompileTimeValue = 0x11223344;
  constexpr uint32_t kCallTimeValue = 0x55667788;
  *host_address = kCompileTimeValue;
  REQUIRE(heap->Protect(guest_address, sizeof(uint32_t), kMemoryProtectRead));

  uint32_t result = 0;
  test.Run(
      [&](ppc::PPCContext*) {
        REQUIRE(heap->Protect(guest_address, sizeof(uint32_t),
                              kMemoryProtectRead | kMemoryProtectWrite));
        *host_address = kCallTimeValue;
        REQUIRE(
            heap->Protect(guest_address, sizeof(uint32_t), kMemoryProtectRead));
      },
      [&](ppc::PPCContext* context) {
        result = static_cast<uint32_t>(context->r[3]);
      });

  REQUIRE(heap->Protect(guest_address, sizeof(uint32_t),
                        kMemoryProtectRead | kMemoryProtectWrite));
  test.memory->SystemHeapFree(guest_address);
  return result;
}

}  // namespace

TEST_CASE("Readonly guest load folding is opt-out",
          "[cpu][replay-compile-controls]") {
  REQUIRE(RunReadonlyGuestLoad(true) == 0x11223344);
  REQUIRE(RunReadonlyGuestLoad(false) == 0x55667788);
}

TEST_CASE("Function definition serialization is opt-in",
          "[cpu][replay-compile-controls]") {
  // This test protects the live-title default. The mutex behavior itself is
  // exercised by every DemandFunction call when the control is enabled, but a
  // deterministic concurrency assertion requires a definition-stage test hook
  // and is intentionally not added to production code.
  REQUIRE_FALSE(cvars::serialize_guest_function_definitions);
}

}  // namespace xe::cpu::testing
