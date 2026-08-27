/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/testing/util.h"

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/module.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace testing {
namespace {

constexpr uint32_t kCallerAddress = 0x80000000u;
constexpr uint32_t kCalleeAddress = 0x80001000u;
constexpr uint32_t kAfterCaptureAddress = 0x80002000u;
constexpr uint32_t kBranchAndLinkToCallee = 0x48001001u;
constexpr uint32_t kBranchToLinkRegister = 0x4E800020u;

struct FunctionEdge {
  uint32_t source;
  uint32_t dependency;

  bool operator==(const FunctionEdge&) const = default;
};

struct FunctionExtent {
  uint32_t address;
  uint32_t end_address;

  bool operator==(const FunctionExtent&) const = default;
};

struct CaptureEvent {
  bool is_definition;
  uint32_t address;
  uint32_t end_or_dependency_address;

  bool operator==(const CaptureEvent&) const = default;
};

class RecordingCaptureSink final : public GuestInvocationCaptureEventSink {
 public:
  bool Poll() override { return true; }

  bool OnFunctionDependency(uint32_t source_address,
                            uint32_t dependency_address) override {
    dependencies.push_back({source_address, dependency_address});
    events.push_back({false, source_address, dependency_address});
    return accept_callbacks;
  }

  bool OnFunctionDefined(uint32_t address, uint32_t end_address) override {
    definitions.push_back({address, end_address});
    events.push_back({true, address, end_address});
    return accept_callbacks;
  }

  bool OnFunctionEntry(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                       uint32_t, const ppc::GuestPPCRegisterState&) override {
    return true;
  }

  bool OnFunctionExit(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                      uint32_t, const ppc::GuestPPCRegisterState&) override {
    return true;
  }

  bool OnMemoryAccess(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                      uint32_t,
                      ppc::GuestInvocationRecorderMemoryAccess) override {
    return true;
  }

  bool OnUnsupportedDependency(const ppc::GuestInvocationRecorderIdentity&,
                               uint32_t) override {
    return true;
  }

  bool OnTailCall(const ppc::GuestInvocationRecorderIdentity&, uint32_t,
                  uint32_t) override {
    return true;
  }

  bool OnUnwindOrLongjmp(const ppc::GuestInvocationRecorderIdentity&) override {
    return true;
  }

  bool OnAsyncReentry(const ppc::GuestInvocationRecorderIdentity&) override {
    return true;
  }

  bool accept_callbacks = true;
  std::vector<FunctionEdge> dependencies;
  std::vector<FunctionExtent> definitions;
  std::vector<CaptureEvent> events;
};

class FrontendCaptureTestModule final : public Module {
 public:
  explicit FrontendCaptureTestModule(Processor* processor)
      : Module(processor) {}

  const std::string& name() const override {
    static const std::string kName = "FrontendCaptureTest";
    return kName;
  }

  bool is_executable() const override { return true; }

  bool ContainsAddress(uint32_t address) override {
    return address == kCallerAddress || address == kCalleeAddress ||
           address == kAfterCaptureAddress;
  }

  Symbol::Status DeclareFunction(uint32_t address,
                                 Function** out_function) override {
    const Symbol::Status status =
        Module::DeclareFunction(address, out_function);
    if (status == Symbol::Status::kNew) {
      (*out_function)
          ->set_end_address(address == kCallerAddress ? address + 4 : address);
    }
    return status;
  }

 protected:
  std::unique_ptr<Function> CreateFunction(uint32_t address) override {
    return std::unique_ptr<Function>(
        processor_->backend()->CreateGuestFunction(this, address));
  }
};

}  // namespace

TEST_CASE("GUEST_INVOCATION_CAPTURE_FRONTEND_DEFINITION_CLOSURE",
          "[capture][frontend]") {
  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  store_and_swap<uint32_t>(memory->TranslateVirtual(kCallerAddress),
                           kBranchAndLinkToCallee);
  store_and_swap<uint32_t>(memory->TranslateVirtual(kCallerAddress + 4),
                           kBranchToLinkRegister);
  store_and_swap<uint32_t>(memory->TranslateVirtual(kCalleeAddress),
                           kBranchToLinkRegister);
  store_and_swap<uint32_t>(memory->TranslateVirtual(kAfterCaptureAddress),
                           kBranchToLinkRegister);

  auto backend = CreateBackend();
  REQUIRE(backend);
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  REQUIRE(processor->Setup(std::move(backend)));
  REQUIRE(processor->AddModule(
      std::make_unique<FrontendCaptureTestModule>(processor.get())));
  processor->backend()->CommitExecutableRange(kCallerAddress,
                                              kAfterCaptureAddress + 0x1000);

  RecordingCaptureSink capture;
  processor->set_guest_invocation_capture_sink(&capture);
  REQUIRE(processor->guest_invocation_capture_sink() == &capture);

  // Translating the caller declares the direct callee and reports the edge
  // before the caller's successful definition. The declaration alone must not
  // be reported as a successful callee definition.
  REQUIRE(processor->ResolveFunction(kCallerAddress));
  REQUIRE((capture.dependencies ==
           std::vector<FunctionEdge>{{kCallerAddress, kCalleeAddress}}));
  REQUIRE((capture.definitions ==
           std::vector<FunctionExtent>{{kCallerAddress, kCallerAddress + 4}}));
  REQUIRE((capture.events == std::vector<CaptureEvent>{
                                 {false, kCallerAddress, kCalleeAddress},
                                 {true, kCallerAddress, kCallerAddress + 4}}));

  REQUIRE(processor->ResolveFunction(kCalleeAddress));
  REQUIRE((capture.definitions ==
           std::vector<FunctionExtent>{{kCallerAddress, kCallerAddress + 4},
                                       {kCalleeAddress, kCalleeAddress}}));

  // Already-defined functions must not mint duplicate definition events.
  REQUIRE(processor->ResolveFunction(kCallerAddress));
  REQUIRE(processor->ResolveFunction(kCalleeAddress));
  REQUIRE(capture.dependencies.size() == 1);
  REQUIRE(capture.definitions.size() == 2);

  // Capture rejection is sink-owned and must not change title translation.
  capture.accept_callbacks = false;
  REQUIRE(processor->ResolveFunction(kAfterCaptureAddress));
  REQUIRE((capture.definitions.back() ==
           FunctionExtent{kAfterCaptureAddress, kAfterCaptureAddress}));

  processor->set_guest_invocation_capture_sink(nullptr);
  REQUIRE(processor->guest_invocation_capture_sink() == nullptr);
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif  // XE_ENABLE_GUEST_INVOCATION_CAPTURE
