/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_ARCH_ARM64

#include <cstdio>
#include <functional>
#include <string>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/function.h"
#include "xenia/cpu/testing/util.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

void PrintCodegenFixture(
    const char* name,
    const std::function<void(hir::HIRBuilder&)>& generate_function) {
  TestFunction test(generate_function);
  REQUIRE(test.processors.size() == 1);
  auto* function = static_cast<GuestFunction*>(
      test.processors[0]->ResolveFunction(0x80000000));
  REQUIRE(function != nullptr);
  REQUIRE(function->machine_code() != nullptr);
  REQUIRE(function->machine_code_length() != 0);
  REQUIRE((function->machine_code_length() & 3) == 0);

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string hex(function->machine_code_length() * 2, '\0');
  for (size_t i = 0; i < function->machine_code_length(); ++i) {
    const uint8_t byte = function->machine_code()[i];
    hex[i * 2] = kHexDigits[byte >> 4];
    hex[i * 2 + 1] = kHexDigits[byte & 0xF];
  }
  std::printf("A64_CAPTURE_CODEGEN %s %s\n", name, hex.c_str());
}

}  // namespace

TEST_CASE("A64_CAPTURE_CODEGEN_FIXTURES", "[a64-capture-codegen]") {
  PrintCodegenFixture("empty",
                      [](hir::HIRBuilder& builder) { builder.Return(); });
  PrintCodegenFixture("scalar", [](hir::HIRBuilder& builder) {
    StoreGPR(builder, 3, builder.Add(LoadGPR(builder, 4), LoadGPR(builder, 5)));
    builder.Return();
  });
  PrintCodegenFixture("vector", [](hir::HIRBuilder& builder) {
    StoreVR(builder, 3,
            builder.VectorAdd(LoadVR(builder, 4), LoadVR(builder, 5),
                              hir::INT32_TYPE));
    builder.Return();
  });
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
