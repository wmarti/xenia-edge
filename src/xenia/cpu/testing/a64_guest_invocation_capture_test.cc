/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/platform.h"

#if XE_ARCH_ARM64 && defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/base/cvar.h"
#include "xenia/cpu/backend/a64/a64_guest_invocation_capture.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/mmio_handler.h"
#include "xenia/cpu/testing/util.h"
#include "xenia/memory.h"

DECLARE_bool(emit_mmio_aware_stores_for_recorded_exception_addresses);

namespace xe {
namespace cpu {
namespace backend {
namespace a64 {

uint64_t ResolveFunction(void* raw_context, uint64_t target_address);

}  // namespace a64
}  // namespace backend
namespace testing {
namespace {

enum class CaptureEventKind {
  kPoll,
  kDependency,
  kDefined,
  kEntry,
  kExit,
  kMemory,
  kUnsupported,
  kTail,
  kUnwind,
  kAsync,
  kHostTransition,
  kResolveReturned,
};

struct CaptureEvent {
  CaptureEventKind kind = CaptureEventKind::kPoll;
  ppc::GuestInvocationRecorderIdentity identity = {};
  uint32_t address = 0;
  uint32_t second_address = 0;
  uint32_t flags = 0;
  ppc::GuestPPCRegisterState state = {};
};

class RecordingCaptureSink final : public GuestInvocationCaptureEventSink {
 public:
  bool Poll() override {
    events.push_back({CaptureEventKind::kPoll});
    return true;
  }

  bool OnFunctionDependency(uint32_t source_address,
                            uint32_t dependency_address) override {
    CaptureEvent event = {CaptureEventKind::kDependency};
    event.address = source_address;
    event.second_address = dependency_address;
    events.push_back(std::move(event));
    return true;
  }

  bool OnFunctionDefined(uint32_t address, uint32_t end_address) override {
    CaptureEvent event = {CaptureEventKind::kDefined};
    event.address = address;
    event.second_address = end_address;
    events.push_back(std::move(event));
    return true;
  }

  bool OnFunctionEntry(const ppc::GuestInvocationRecorderIdentity& identity,
                       uint32_t address, uint32_t end_address,
                       const ppc::GuestPPCRegisterState& state) override {
    CaptureEvent event = {CaptureEventKind::kEntry};
    event.identity = identity;
    event.address = address;
    event.second_address = end_address;
    event.state = state;
    events.push_back(std::move(event));
    return true;
  }

  bool OnFunctionExit(const ppc::GuestInvocationRecorderIdentity& identity,
                      uint32_t address, uint32_t return_address,
                      const ppc::GuestPPCRegisterState& state) override {
    CaptureEvent event = {CaptureEventKind::kExit};
    event.identity = identity;
    event.address = address;
    event.second_address = return_address;
    event.state = state;
    events.push_back(std::move(event));
    return true;
  }

  bool OnMemoryAccess(
      const ppc::GuestInvocationRecorderIdentity& identity, uint32_t address,
      uint32_t size, ppc::GuestInvocationRecorderMemoryAccess access) override {
    CaptureEvent event = {CaptureEventKind::kMemory};
    event.identity = identity;
    event.address = address;
    event.second_address = size;
    event.flags = static_cast<uint32_t>(access);
    if (memory_observer) {
      memory_observer(event);
    }
    events.push_back(std::move(event));
    return true;
  }

  bool OnUnsupportedDependency(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint32_t dependency_flags) override {
    CaptureEvent event = {CaptureEventKind::kUnsupported};
    event.identity = identity;
    event.flags = dependency_flags;
    if (unsupported_observer) {
      unsupported_observer(event);
    }
    events.push_back(std::move(event));
    return true;
  }

  bool OnTailCall(const ppc::GuestInvocationRecorderIdentity& identity,
                  uint32_t from_address, uint32_t target_address) override {
    CaptureEvent event = {CaptureEventKind::kTail};
    event.identity = identity;
    event.address = from_address;
    event.second_address = target_address;
    events.push_back(std::move(event));
    return true;
  }

  bool OnUnwindOrLongjmp(
      const ppc::GuestInvocationRecorderIdentity& identity) override {
    CaptureEvent event = {CaptureEventKind::kUnwind};
    event.identity = identity;
    events.push_back(std::move(event));
    return true;
  }

  bool OnAsyncReentry(
      const ppc::GuestInvocationRecorderIdentity& identity) override {
    CaptureEvent event = {CaptureEventKind::kAsync};
    event.identity = identity;
    events.push_back(std::move(event));
    return true;
  }

  void OnHostTransition() {
    events.push_back({CaptureEventKind::kHostTransition});
  }

  void OnResolveReturned() {
    events.push_back({CaptureEventKind::kResolveReturned});
  }

  std::vector<CaptureEvent> ControlEvents() const {
    std::vector<CaptureEvent> result;
    for (const CaptureEvent& event : events) {
      if (event.kind != CaptureEventKind::kPoll &&
          event.kind != CaptureEventKind::kDependency &&
          event.kind != CaptureEventKind::kDefined &&
          event.kind != CaptureEventKind::kMemory) {
        result.push_back(event);
      }
    }
    return result;
  }

  std::vector<CaptureEvent> events;
  std::function<void(const CaptureEvent&)> memory_observer;
  std::function<void(const CaptureEvent&)> unsupported_observer;
};

class SyntheticCaptureClock final : public ppc::GuestInvocationRecorderClock {
 public:
  uint64_t NowTicks() const override { return now; }

  uint64_t now = 100;
};

class MemoryCapturePageReader final
    : public ppc::GuestInvocationRecorderPageReader {
 public:
  explicit MemoryCapturePageReader(Memory& memory) : memory_(memory) {}

  bool ReadPage(uint32_t page_address,
                std::array<uint8_t, 4096>* output) override {
    std::memcpy(output->data(), memory_.TranslateVirtual(page_address),
                output->size());
    read_addresses.push_back(page_address);
    return true;
  }

  std::vector<uint32_t> read_addresses;

 private:
  Memory& memory_;
};

class ScopedCaptureSink final {
 public:
  ScopedCaptureSink(Processor& processor,
                    GuestInvocationCaptureEventSink& capture_sink)
      : processor_(processor) {
    processor_.set_guest_invocation_capture_sink(&capture_sink);
  }

  ~ScopedCaptureSink() {
    processor_.set_guest_invocation_capture_sink(nullptr);
  }

 private:
  Processor& processor_;
};

ppc::GuestPPCRegisterState MakeRegisterState(uint64_t seed) {
  ppc::GuestPPCRegisterState state;
  for (size_t i = 0; i < state.gpr.size(); ++i) {
    state.gpr[i] = seed + i * 0x0101010101010101ull;
    state.fpr_bits[i] = seed ^ (i * 0x102030405060708ull);
  }
  for (size_t i = 0; i < state.vector_registers.size(); ++i) {
    for (size_t j = 0; j < state.vector_registers[i].size(); ++j) {
      state.vector_registers[i][j] = static_cast<uint8_t>(seed + i * 3 + j);
    }
  }
  for (size_t i = 0; i < state.condition_register_fields.size(); ++i) {
    for (size_t j = 0; j < state.condition_register_fields[i].size(); ++j) {
      state.condition_register_fields[i][j] =
          static_cast<uint8_t>(seed + i * 4 + j);
    }
  }
  state.link_register = seed + 0x1000;
  state.count_register = seed + 0x2000;
  state.machine_state_register = seed + 0x3000;
  state.fpscr = static_cast<uint32_t>(seed + 0x4000);
  for (size_t i = 0; i < state.vscr_vector.size(); ++i) {
    state.vscr_vector[i] = static_cast<uint8_t>(seed + i * 5);
  }
  state.vrsave = static_cast<uint32_t>(seed + 0x5000);
  state.xer_ca = static_cast<uint8_t>(seed + 1);
  state.xer_ov = static_cast<uint8_t>(seed + 2);
  state.xer_so = static_cast<uint8_t>(seed + 3);
  state.vscr_sat = static_cast<uint8_t>(seed + 4);
  return state;
}

void HostTransitionBuiltin(ppc::PPCContext*, void* arg0, void*) {
  reinterpret_cast<RecordingCaptureSink*>(arg0)->OnHostTransition();
}

struct ReentryArguments {
  RecordingCaptureSink* sink = nullptr;
  Function* nested_function = nullptr;
  uint32_t return_address = 0;
};

void ReentryBuiltin(ppc::PPCContext* context, void* arg0, void*) {
  auto* arguments = reinterpret_cast<ReentryArguments*>(arg0);
  arguments->sink->OnHostTransition();
  REQUIRE(arguments->nested_function->Call(context->thread_state,
                                           arguments->return_address));
}

struct MmioCaptureProbe {
  RecordingCaptureSink* sink = nullptr;
  uint32_t read_address = 0;
  uint32_t write_address = 0;
  uint32_t write_value = 0;
};

uint32_t CapturedMmioRead(void*, void* callback_context, uint32_t address) {
  auto* probe = reinterpret_cast<MmioCaptureProbe*>(callback_context);
  probe->sink->OnHostTransition();
  probe->read_address = address;
  return 0x12345678;
}

void CapturedMmioWrite(void*, void* callback_context, uint32_t address,
                       uint32_t value) {
  auto* probe = reinterpret_cast<MmioCaptureProbe*>(callback_context);
  probe->sink->OnHostTransition();
  probe->write_address = address;
  probe->write_value = value;
}

void KernelExtern(ppc::PPCContext*, kernel::KernelState*) {}

class ReturnSiteTestModule final : public TestModule {
 public:
  ReturnSiteTestModule(Processor* processor, uint32_t start_address,
                       uint32_t return_site_address, uint32_t end_address,
                       std::function<bool(hir::HIRBuilder&)> generate)
      : TestModule(
            processor, "CaptureLongjmpTarget",
            [start_address, end_address](uint32_t address) {
              return address >= start_address && address < end_address;
            },
            std::move(generate), /*skip_cf_simplification=*/true),
        start_address_(start_address),
        return_site_address_(return_site_address),
        end_address_(end_address) {
    return_site_flags_.is_return_site = true;
  }

  InfoCacheFlags* GetInstructionAddressFlags(uint32_t address) override {
    return address == return_site_address_ ? &return_site_flags_ : nullptr;
  }

  Symbol::Status DeclareFunction(uint32_t address,
                                 Function** out_function) override {
    Symbol::Status status = TestModule::DeclareFunction(address, out_function);
    if (address == start_address_ && *out_function) {
      (*out_function)->set_end_address(end_address_);
    }
    return status;
  }

 private:
  const uint32_t start_address_;
  const uint32_t return_site_address_;
  const uint32_t end_address_;
  InfoCacheFlags return_site_flags_ = {};
};

class MmioSiteTestModule final : public TestModule {
 public:
  MmioSiteTestModule(Processor* processor, uint32_t function_address,
                     uint32_t function_end,
                     std::function<bool(hir::HIRBuilder&)> generate)
      : TestModule(
            processor, "CaptureRecordedMmioSite",
            [function_address](uint32_t address) {
              return address == function_address;
            },
            std::move(generate), /*skip_cf_simplification=*/true),
        function_address_(function_address),
        function_end_(function_end) {
    mmio_site_flags_.accessed_mmio = true;
  }

  InfoCacheFlags* GetInstructionAddressFlags(uint32_t address) override {
    return address >= function_address_ && address < function_end_
               ? &mmio_site_flags_
               : nullptr;
  }

 private:
  const uint32_t function_address_;
  const uint32_t function_end_;
  InfoCacheFlags mmio_site_flags_ = {};
};

class ScopedRecordedMmioAwareness {
 public:
  ScopedRecordedMmioAwareness()
      : old_value_(
            cvars::emit_mmio_aware_stores_for_recorded_exception_addresses) {
    cvars::emit_mmio_aware_stores_for_recorded_exception_addresses = true;
  }

  ~ScopedRecordedMmioAwareness() {
    cvars::emit_mmio_aware_stores_for_recorded_exception_addresses = old_value_;
  }

  ScopedRecordedMmioAwareness(const ScopedRecordedMmioAwareness&) = delete;
  ScopedRecordedMmioAwareness& operator=(const ScopedRecordedMmioAwareness&) =
      delete;

 private:
  bool old_value_;
};

struct LongjmpResolveArguments {
  RecordingCaptureSink* sink = nullptr;
  uint32_t target_address = 0;
  uint64_t resolved_host_address = 0;
  const backend::a64::A64StackpointNode* pending_before_resolve = nullptr;
  const backend::a64::A64StackpointNode* pending_after_resolve = nullptr;
};

void ResolveLongjmpBuiltin(ppc::PPCContext* context, void* arg0, void*) {
  auto* arguments = reinterpret_cast<LongjmpResolveArguments*>(arg0);
  auto* backend = static_cast<backend::a64::A64Backend*>(
      context->thread_state->processor()->backend());
  auto* backend_context = backend->BackendContextForGuestContext(context);
  arguments->sink->OnHostTransition();
  arguments->pending_before_resolve =
      backend_context->pending_stackpoint_sync_node;
  arguments->resolved_host_address =
      backend::a64::ResolveFunction(context, arguments->target_address);
  arguments->pending_after_resolve =
      backend_context->pending_stackpoint_sync_node;
  arguments->sink->OnResolveReturned();

  // This test exercises detection and ordering, but must not ask the emitted
  // caller to repair its host SP to the synthetic stackpoint chain below.
  backend_context->pending_stackpoint_sync_node = nullptr;
}

std::vector<CaptureEvent> RunTailCapture(bool indirect) {
  constexpr uint32_t kRootAddress = 0x80000000;
  constexpr uint32_t kTargetAddress = 0x80001000;
  constexpr uint64_t kSentinel = 0xABCDEF0123456789ull;

  Function* target_function = nullptr;
  TestFunction root([&](hir::HIRBuilder& builder) {
    if (indirect) {
      builder.CallIndirect(builder.LoadConstantUint64(kTargetAddress),
                           hir::CALL_TAIL);
    } else {
      REQUIRE(target_function != nullptr);
      builder.Call(target_function, hir::CALL_TAIL);
    }
    builder.Return();
  });
  REQUIRE(root.processors.size() == 1);
  Processor* processor = root.processors[0].get();

  auto target_module = std::make_unique<TestModule>(
      processor, "CaptureTailTarget",
      [](uint32_t address) { return address == kTargetAddress; },
      [](hir::HIRBuilder& builder) {
        StoreGPR(builder, 3, builder.LoadConstantUint64(kSentinel));
        builder.Return();
        return true;
      },
      /*skip_cf_simplification=*/true);
  processor->AddModule(std::move(target_module));
  target_function = processor->ResolveFunction(kTargetAddress);
  REQUIRE(target_function != nullptr);

  RecordingCaptureSink capture;
  processor->set_guest_invocation_capture_sink(&capture);
  root.Run(
      [](ppc::PPCContext* context) {
        context->lr = 0xB0B0B0B0;
        context->r[3] = 0;
      },
      [=](ppc::PPCContext* context) { REQUIRE(context->r[3] == kSentinel); });
  processor->set_guest_invocation_capture_sink(nullptr);
  return capture.ControlEvents();
}

}  // namespace

TEST_CASE("A64_CAPTURE_HELPERS_FORWARD_EXACT_CONTROL_EVENTS",
          "[backend][guest-invocation-capture]") {
  Processor processor(nullptr, nullptr);
  RecordingCaptureSink capture;
  processor.set_guest_invocation_capture_sink(&capture);

  ppc::PPCContext context = {};
  context.processor = &processor;
  context.thread_id = 0x12345678;
  const ppc::GuestPPCRegisterState state = MakeRegisterState(0x55);
  ppc::RestoreGuestPPCRegisterState(state, &context);

  constexpr uint32_t kFunction = 0x82001000;
  constexpr uint32_t kFunctionEnd = 0x820012FC;
  constexpr uint32_t kReturn = 0x82002004;
  constexpr uint32_t kTarget = 0x82003000;
  constexpr uint32_t kFlags = ppc::kGuestInvocationDependencyKernelExport |
                              ppc::kGuestInvocationDependencyExternOrBuiltin;

  backend::a64::CaptureGuestInvocationFunctionEntry(&context, kFunction,
                                                    kFunctionEnd);
  backend::a64::CaptureGuestInvocationFunctionExit(&context, kFunction,
                                                   kReturn);
  REQUIRE(backend::a64::CaptureGuestInvocationTailCall(&context, kFunction,
                                                       kTarget) == kTarget);
  backend::a64::CaptureGuestInvocationUnsupportedDependency(&context, kFlags);
  backend::a64::CaptureGuestInvocationUnwindOrLongjmp(&context);
  backend::a64::CaptureGuestInvocationAsyncReentry(&context);

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 6);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[0].address == kFunction);
  REQUIRE(events[0].second_address == kFunctionEnd);
  REQUIRE(events[0].state == state);
  REQUIRE(events[1].kind == CaptureEventKind::kExit);
  REQUIRE(events[1].address == kFunction);
  REQUIRE(events[1].second_address == kReturn);
  REQUIRE(events[1].state == state);
  REQUIRE(events[2].kind == CaptureEventKind::kTail);
  REQUIRE(events[2].address == kFunction);
  REQUIRE(events[2].second_address == kTarget);
  REQUIRE(events[3].kind == CaptureEventKind::kUnsupported);
  REQUIRE(events[3].flags == kFlags);
  REQUIRE(events[4].kind == CaptureEventKind::kUnwind);
  REQUIRE(events[5].kind == CaptureEventKind::kAsync);
  for (const CaptureEvent& event : events) {
    REQUIRE(event.identity.context_id == reinterpret_cast<uintptr_t>(&context));
    REQUIRE(event.identity.thread_id == context.thread_id);
  }
}

TEST_CASE("A64_CAPTURE_ENTRY_EXIT_PRESERVE_STATE_AND_SAVED_RETURN",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kRootAddress = 0x80000000;
  constexpr uint32_t kReturnAddress = 0xBCBCBCBC;
  constexpr uint64_t kNewLinkRegister = 0xDEADBEEF;

  TestFunction test([](hir::HIRBuilder& builder) {
    StoreGPR(builder, 3, LoadGPR(builder, 4));
    StoreVR(builder, 3, LoadVR(builder, 4));
    builder.StoreContext(offsetof(ppc::PPCContext, lr),
                         builder.LoadConstantUint64(kNewLinkRegister));
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);

  RecordingCaptureSink capture;
  test.processors[0]->set_guest_invocation_capture_sink(&capture);
  ppc::GuestPPCRegisterState expected_entry;
  ppc::GuestPPCRegisterState expected_exit;
  test.Run(
      [&](ppc::PPCContext* context) {
        context->lr = kReturnAddress;
        context->r[4] = 0x1122334455667788ull;
        context->v[4] = vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444);
        expected_entry = ppc::CaptureGuestPPCRegisterState(*context);
      },
      [&](ppc::PPCContext* context) {
        expected_exit = ppc::CaptureGuestPPCRegisterState(*context);
        REQUIRE(context->r[3] == 0x1122334455667788ull);
        REQUIRE(context->v[3] ==
                vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444));
        REQUIRE(context->lr == kNewLinkRegister);
      });
  test.processors[0]->set_guest_invocation_capture_sink(nullptr);

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[0].address == kRootAddress);
  REQUIRE(events[0].state == expected_entry);
  REQUIRE(events[1].kind == CaptureEventKind::kExit);
  REQUIRE(events[1].address == kRootAddress);
  REQUIRE(events[1].second_address == kReturnAddress);
  REQUIRE(events[1].state == expected_exit);
}

TEST_CASE("A64_CAPTURE_REJECTS_BUILTIN_BEFORE_HOST_TRANSITION",
          "[backend][guest-invocation-capture]") {
  Function* builtin = nullptr;
  TestFunction test([&](hir::HIRBuilder& builder) {
    REQUIRE(builtin != nullptr);
    builder.CallExtern(builtin);
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);

  RecordingCaptureSink capture;
  builtin = test.processors[0]->DefineBuiltin(
      "CaptureHostTransition", HostTransitionBuiltin, &capture, nullptr);
  test.processors[0]->set_guest_invocation_capture_sink(&capture);
  test.Run([](ppc::PPCContext*) {}, [](ppc::PPCContext*) {});
  test.processors[0]->set_guest_invocation_capture_sink(nullptr);

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[1].kind == CaptureEventKind::kUnsupported);
  REQUIRE(events[1].flags == ppc::kGuestInvocationDependencyExternOrBuiltin);
  REQUIRE(events[2].kind == CaptureEventKind::kHostTransition);
  REQUIRE(events[3].kind == CaptureEventKind::kExit);
}

TEST_CASE("A64_CAPTURE_REJECTS_KERNEL_EXTERN_BEFORE_HOST_TRANSITION",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kExternAddress = 0x80002000;
  Function* extern_function = nullptr;
  TestFunction test([&](hir::HIRBuilder& builder) {
    REQUIRE(extern_function != nullptr);
    builder.CallExtern(extern_function);
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);
  Processor* processor = test.processors[0].get();

  auto extern_module = std::make_unique<TestModule>(
      processor, "CaptureKernelExtern",
      [](uint32_t address) { return address == kExternAddress; },
      [](hir::HIRBuilder& builder) {
        builder.Return();
        return true;
      });
  processor->AddModule(std::move(extern_module));
  extern_function = processor->ResolveFunction(kExternAddress);
  REQUIRE(extern_function != nullptr);
  static_cast<GuestFunction*>(extern_function)->SetupExtern(KernelExtern);

  RecordingCaptureSink capture;
  processor->set_guest_invocation_capture_sink(&capture);
  test.Run([](ppc::PPCContext*) {}, [](ppc::PPCContext*) {});
  processor->set_guest_invocation_capture_sink(nullptr);

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 3);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[1].kind == CaptureEventKind::kUnsupported);
  REQUIRE(events[1].flags == (ppc::kGuestInvocationDependencyKernelExport |
                              ppc::kGuestInvocationDependencyExternOrBuiltin));
  REQUIRE(events[2].kind == CaptureEventKind::kExit);
}

TEST_CASE("A64_CAPTURE_REPORTS_HOST_ASYNC_REENTRY_BEFORE_NESTED_ENTRY",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kNestedAddress = 0x80001000;
  constexpr uint32_t kNestedReturn = 0x80000008;
  Function* builtin = nullptr;
  TestFunction test([&](hir::HIRBuilder& builder) {
    REQUIRE(builtin != nullptr);
    builder.CallExtern(builtin);
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);
  Processor* processor = test.processors[0].get();

  auto nested_module = std::make_unique<TestModule>(
      processor, "CaptureNestedEntry",
      [](uint32_t address) { return address == kNestedAddress; },
      [](hir::HIRBuilder& builder) {
        StoreGPR(builder, 7, builder.LoadConstantUint64(0x71717171));
        builder.Return();
        return true;
      });
  processor->AddModule(std::move(nested_module));
  Function* nested_function = processor->ResolveFunction(kNestedAddress);
  REQUIRE(nested_function != nullptr);

  RecordingCaptureSink capture;
  ReentryArguments arguments = {&capture, nested_function, kNestedReturn};
  builtin = processor->DefineBuiltin("CaptureReentry", ReentryBuiltin,
                                     &arguments, nullptr);
  processor->set_guest_invocation_capture_sink(&capture);
  test.Run(
      [](ppc::PPCContext*) {},
      [](ppc::PPCContext* context) { REQUIRE(context->r[7] == 0x71717171); });
  processor->set_guest_invocation_capture_sink(nullptr);

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 7);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[1].kind == CaptureEventKind::kUnsupported);
  REQUIRE(events[2].kind == CaptureEventKind::kHostTransition);
  REQUIRE(events[3].kind == CaptureEventKind::kAsync);
  REQUIRE(events[4].kind == CaptureEventKind::kEntry);
  REQUIRE(events[4].address == kNestedAddress);
  REQUIRE(events[5].kind == CaptureEventKind::kExit);
  REQUIRE(events[5].address == kNestedAddress);
  REQUIRE(events[5].second_address == kNestedReturn);
  REQUIRE(events[6].kind == CaptureEventKind::kExit);
}

TEST_CASE("A64_CAPTURE_REPORTS_LONGJMP_DURING_EMITTED_HOST_TRANSITION",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kTargetAddress = 0x80001000;
  constexpr uint32_t kReturnSiteAddress = kTargetAddress + 4;
  constexpr uint32_t kTargetEndAddress = kTargetAddress + 8;
  constexpr uint32_t kGuestStackUnwind = 0x1000;

  Function* builtin = nullptr;
  TestFunction test([&](hir::HIRBuilder& builder) {
    StoreGPR(builder, 1,
             builder.Add(LoadGPR(builder, 1),
                         builder.LoadConstantUint64(kGuestStackUnwind)));
    REQUIRE(builtin != nullptr);
    builder.CallExtern(builtin);
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);
  Processor* processor = test.processors[0].get();
  auto* a64_backend =
      static_cast<backend::a64::A64Backend*>(processor->backend());

  auto target_module = std::make_unique<ReturnSiteTestModule>(
      processor, kTargetAddress, kReturnSiteAddress, kTargetEndAddress,
      [](hir::HIRBuilder& builder) {
        builder.SourceOffset(kTargetAddress);
        StoreGPR(builder, 8, builder.LoadConstantUint64(0x11111111));
        builder.SourceOffset(kReturnSiteAddress);
        StoreGPR(builder, 8, builder.LoadConstantUint64(0x22222222));
        builder.Return();
        return true;
      });
  processor->AddModule(std::move(target_module));
  auto* target_function =
      static_cast<GuestFunction*>(processor->ResolveFunction(kTargetAddress));
  REQUIRE(target_function != nullptr);
  REQUIRE(target_function->LookupGuestAddress(kReturnSiteAddress) != nullptr);

  RecordingCaptureSink capture;
  LongjmpResolveArguments arguments;
  arguments.sink = &capture;
  arguments.target_address = kReturnSiteAddress;
  builtin = processor->DefineBuiltin(
      "CaptureLongjmpResolve", ResolveLongjmpBuiltin, &arguments, nullptr);
  processor->set_guest_invocation_capture_sink(&capture);

  backend::a64::A64StackpointNode restore_node = {};
  backend::a64::A64StackpointNode skipped_node = {};
  test.Run(
      [&](ppc::PPCContext* context) {
        const uint32_t initial_guest_stack =
            static_cast<uint32_t>(context->r[1]);
        restore_node = {nullptr, initial_guest_stack + kGuestStackUnwind,
                        0x80004004};
        skipped_node = {&restore_node, initial_guest_stack + 0x100, 0x80003004};
        auto* backend_context =
            a64_backend->BackendContextForGuestContext(context);
        backend_context->stackpoint_head = &skipped_node;
        backend_context->pending_stackpoint_sync_node = nullptr;
      },
      [&](ppc::PPCContext* context) {
        auto* backend_context =
            a64_backend->BackendContextForGuestContext(context);
        backend_context->stackpoint_head = nullptr;
        backend_context->pending_stackpoint_sync_node = nullptr;
      });
  processor->set_guest_invocation_capture_sink(nullptr);

  REQUIRE(arguments.pending_before_resolve == nullptr);
  REQUIRE(arguments.pending_after_resolve == &restore_node);
  REQUIRE(arguments.resolved_host_address != 0);
  REQUIRE(arguments.resolved_host_address !=
          reinterpret_cast<uintptr_t>(target_function->machine_code()));

  const std::vector<CaptureEvent> events = capture.ControlEvents();
  REQUIRE(events.size() == 6);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[1].kind == CaptureEventKind::kUnsupported);
  REQUIRE(events[2].kind == CaptureEventKind::kHostTransition);
  REQUIRE(events[3].kind == CaptureEventKind::kUnwind);
  REQUIRE(events[4].kind == CaptureEventKind::kResolveReturned);
  REQUIRE(events[5].kind == CaptureEventKind::kExit);
}

TEST_CASE("A64_CAPTURE_REPORTS_DIRECT_TAIL_BEFORE_TARGET_ENTRY",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kRootAddress = 0x80000000;
  constexpr uint32_t kTargetAddress = 0x80001000;
  const std::vector<CaptureEvent> events = RunTailCapture(false);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[0].address == kRootAddress);
  REQUIRE(events[1].kind == CaptureEventKind::kTail);
  REQUIRE(events[1].address == kRootAddress);
  REQUIRE(events[1].second_address == kTargetAddress);
  REQUIRE(events[2].kind == CaptureEventKind::kEntry);
  REQUIRE(events[2].address == kTargetAddress);
  REQUIRE(events[3].kind == CaptureEventKind::kExit);
  REQUIRE(events[3].address == kTargetAddress);
}

TEST_CASE("A64_CAPTURE_ROUND_TRIPS_INDIRECT_TAIL_TARGET",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kRootAddress = 0x80000000;
  constexpr uint32_t kTargetAddress = 0x80001000;
  const std::vector<CaptureEvent> events = RunTailCapture(true);
  REQUIRE(events.size() == 4);
  REQUIRE(events[0].kind == CaptureEventKind::kEntry);
  REQUIRE(events[0].address == kRootAddress);
  REQUIRE(events[1].kind == CaptureEventKind::kTail);
  REQUIRE(events[1].address == kRootAddress);
  REQUIRE(events[1].second_address == kTargetAddress);
  REQUIRE(events[2].kind == CaptureEventKind::kEntry);
  REQUIRE(events[2].address == kTargetAddress);
  REQUIRE(events[3].kind == CaptureEventKind::kExit);
  REQUIRE(events[3].address == kTargetAddress);
}

TEST_CASE("A64_CAPTURE_COORDINATOR_ACCEPTS_EMITTED_INVOCATION",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kRootAddress = 0x80000000;
  constexpr uint32_t kRootEndAddress = kRootAddress + 0x0C;
  constexpr uint32_t kPageSize = 4096;
  constexpr uint32_t kWordOffset = 0x180;
  constexpr uint32_t kInitialWord = 0x10203040;
  constexpr uint32_t kFinalWord = 0xA1B2C3D4;
  constexpr uint64_t kSegmentOrdinal = 23;
  constexpr uint64_t kCaptureStartTick = 100;
  constexpr uint64_t kCaptureEndTick = 200;

  TestFunction test(
      [](hir::HIRBuilder& builder) {
        hir::Value* address = LoadGPR(builder, 4);
        StoreGPR(builder, 3,
                 builder.ZeroExtend(builder.Load(address, hir::INT32_TYPE),
                                    hir::INT64_TYPE));
        builder.Store(address, builder.LoadConstantUint32(kFinalWord));
        StoreGPR(builder, 5, builder.LoadConstantUint64(kFinalWord));
        builder.Return();
      },
      kRootEndAddress);
  REQUIRE(test.processors.size() == 1);
  Processor* processor = test.processors[0].get();

  const uint32_t data_page = test.memory->SystemHeapAlloc(kPageSize, kPageSize);
  REQUIRE(data_page != 0);
  const uint32_t data_address = data_page + kWordOffset;
  std::array<uint8_t, kPageSize> initial_page = {};
  for (size_t i = 0; i < initial_page.size(); ++i) {
    initial_page[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
  }
  std::memcpy(initial_page.data() + kWordOffset, &kInitialWord,
              sizeof(kInitialWord));
  std::array<uint8_t, kPageSize> expected_dirty_page = initial_page;
  std::memcpy(expected_dirty_page.data() + kWordOffset, &kFinalWord,
              sizeof(kFinalWord));
  std::memcpy(test.memory->TranslateVirtual(data_page), initial_page.data(),
              initial_page.size());

  ppc::GuestInvocationRecorderSelection selection;
  selection.root_address = kRootAddress;
  selection.root_end_address = kRootEndAddress;
  selection.occurrence = 1;
  ppc::GuestInvocationRecorderLimits limits;
  limits.max_attempts = 4;
  limits.max_duration_ticks = 1000;
  limits.max_page_count = 4;
  limits.max_access_count = 16;
  limits.max_call_depth = 2;
  limits.max_event_count = 64;
  limits.max_function_count = 4;

  MemoryCapturePageReader page_reader(*test.memory);
  SyntheticCaptureClock clock;
  std::optional<ppc::GuestInvocationRecorderResult> published_result;
  uint32_t publication_count = 0;
  uint64_t published_ordinal = 0;
  uint64_t published_start_tick = 0;
  uint64_t published_end_tick = 0;
  std::string error;
  std::unique_ptr<GuestInvocationCaptureCoordinator> coordinator =
      GuestInvocationCaptureCoordinator::Create(
          kSegmentOrdinal, selection, limits, page_reader, clock,
          [&](uint64_t ordinal, uint64_t start_tick, uint64_t end_tick,
              const ppc::GuestInvocationRecorderResult& result, std::string*) {
            ++publication_count;
            published_ordinal = ordinal;
            published_start_tick = start_tick;
            published_end_tick = end_tick;
            published_result = result;
            return true;
          },
          &error);
  REQUIRE(coordinator);
  REQUIRE(error.empty());

  ppc::GuestInvocationRecorderIdentity expected_owner = {};
  ppc::GuestPPCRegisterState final_input = {};
  ppc::GuestPPCRegisterState final_output = {};
  const auto require_recorder_state =
      [&](ppc::GuestInvocationRecorderState expected_state) {
        const GuestInvocationCaptureStatus capture_status =
            coordinator->status();
        INFO("capture state = " << static_cast<uint32_t>(capture_status.state));
        INFO("recorder rejection = "
             << static_cast<uint32_t>(capture_status.rejection));
        INFO("recorder rejection flags = "
             << capture_status.rejected_dependency_flags);
        INFO("capture diagnostic = " << capture_status.message);
        REQUIRE(capture_status.recorder_state == expected_state);
      };
  {
    ScopedCaptureSink capture_sink(*processor, *coordinator);
    test.Run(
        [&](ppc::PPCContext* context) {
          context->r[3] = 0x0303030303030303ull;
          context->r[4] = data_address;
          context->r[5] = 0x0505050505050505ull;
          expected_owner = {reinterpret_cast<uintptr_t>(context),
                            context->thread_id};
        },
        [&](ppc::PPCContext* context) {
          require_recorder_state(
              ppc::GuestInvocationRecorderState::kWaitingForDiscoveryAttempt);
          Function* function = processor->ResolveFunction(kRootAddress);
          REQUIRE(function != nullptr);

          context->r[4] = data_address;
          REQUIRE(function->Call(context->thread_state,
                                 static_cast<uint32_t>(context->lr)));
          require_recorder_state(
              ppc::GuestInvocationRecorderState::kWaitingForFinalAttempt);

          std::memcpy(test.memory->TranslateVirtual(data_page),
                      initial_page.data(), initial_page.size());
          context->r[3] = 0x1313131313131313ull;
          context->r[4] = data_address;
          context->r[5] = 0x1515151515151515ull;
          context->ctr = 0x1717171717171717ull;
          context->v[7] =
              vec128i(0x21222324, 0x31323334, 0x41424344, 0x51525354);
          final_input = ppc::CaptureGuestPPCRegisterState(*context);
          clock.now = kCaptureEndTick;
          REQUIRE(function->Call(context->thread_state,
                                 static_cast<uint32_t>(context->lr)));
          final_output = ppc::CaptureGuestPPCRegisterState(*context);
          REQUIRE(final_output.gpr[3] == kInitialWord);
          REQUIRE(final_output.gpr[5] == kFinalWord);

          REQUIRE(function->Call(context->thread_state,
                                 static_cast<uint32_t>(context->lr)));
          REQUIRE(coordinator->Poll());
        });
  }

  REQUIRE(publication_count == 1);
  REQUIRE(published_result);
  REQUIRE(published_ordinal == kSegmentOrdinal);
  REQUIRE(published_start_tick == kCaptureStartTick);
  REQUIRE(published_end_tick == kCaptureEndTick);
  const GuestInvocationCaptureStatus status = coordinator->status();
  INFO("capture state = " << static_cast<uint32_t>(status.state));
  INFO("recorder rejection = " << static_cast<uint32_t>(status.rejection));
  INFO("recorder rejection flags = " << status.rejected_dependency_flags);
  INFO("capture diagnostic = " << status.message);
  REQUIRE(status.state == GuestInvocationCaptureState::kPublished);
  REQUIRE(status.recorder_state ==
          ppc::GuestInvocationRecorderState::kComplete);
  REQUIRE(status.accepted_segment_count == 1);
  REQUIRE(status.rejected_segment_count == 0);
  REQUIRE(status.capture_start_tick == kCaptureStartTick);
  REQUIRE(status.capture_end_tick == kCaptureEndTick);
  REQUIRE(status.message.empty());

  const ppc::GuestInvocationRecorderResult& result = *published_result;
  REQUIRE(result.owner == expected_owner);
  REQUIRE(result.attempt_count == 3);
  REQUIRE((result.touched_page_addresses == std::vector<uint32_t>{data_page}));
  REQUIRE((result.translation_dependencies ==
           std::vector<ppc::GuestInvocationRecorderFunction>{
               {kRootAddress, kRootEndAddress}}));
  REQUIRE((result.entered_functions ==
           std::vector<ppc::GuestInvocationRecorderFunction>{
               {kRootAddress, kRootEndAddress}}));
  const ppc::GuestFunctionInvocation& invocation = result.invocation;
  REQUIRE(invocation.function_address == kRootAddress);
  REQUIRE(invocation.function_end_address == kRootEndAddress);
  REQUIRE(invocation.entry_address == kRootAddress);
  REQUIRE(invocation.expected_return_address == 0xBCBCBCBC);
  REQUIRE(invocation.dependency_flags == 0);
  REQUIRE(invocation.input == final_input);
  REQUIRE(invocation.expected_output == final_output);
  REQUIRE(invocation.input_data_pages.size() == 1);
  REQUIRE(invocation.input_data_pages[0].guest_address == data_page);
  REQUIRE(invocation.input_data_pages[0].data == initial_page);
  REQUIRE(invocation.expected_dirty_pages.size() == 1);
  REQUIRE(invocation.expected_dirty_pages[0].guest_address == data_page);
  REQUIRE(invocation.expected_dirty_pages[0].data == expected_dirty_page);
  REQUIRE((page_reader.read_addresses ==
           std::vector<uint32_t>{data_page, data_page}));
  REQUIRE(std::memcmp(test.memory->TranslateVirtual(data_page),
                      expected_dirty_page.data(),
                      expected_dirty_page.size()) == 0);

  test.memory->SystemHeapFree(data_page);
}

TEST_CASE("A64_CAPTURE_MEMORY_HELPER_VALIDATES_LOGICAL_RANGES",
          "[backend][guest-invocation-capture]") {
  auto memory = std::make_unique<Memory>();
  REQUIRE(memory->Initialize());
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  RecordingCaptureSink capture;
  processor->set_guest_invocation_capture_sink(&capture);

  ppc::PPCContext context = {};
  context.processor = processor.get();
  context.thread_id = 0x12345678;

  using MemoryAccess = ppc::GuestInvocationRecorderMemoryAccess;
  constexpr uint32_t kCrossPageAddress = 0x00001FFEu;
  constexpr uint32_t kCrossIntoMmioAddress = 0x7FBFFFFEu;
  constexpr uint32_t kPhysicalLogicalAddress = 0xE0000000u;
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, kCrossPageAddress, 4,
      static_cast<uint64_t>(MemoryAccess::kRead));
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, kCrossIntoMmioAddress, 4,
      static_cast<uint64_t>(MemoryAccess::kWrite));
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, kPhysicalLogicalAddress, 8,
      static_cast<uint64_t>(MemoryAccess::kWrite));
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, 0x2000, uint64_t{1} << 40,
      static_cast<uint64_t>(MemoryAccess::kRead));
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, uint64_t{1} << 32, 4,
      static_cast<uint64_t>(MemoryAccess::kRead));
  backend::a64::CaptureGuestInvocationMemoryAccess(
      &context, 0x2000, 4, static_cast<uint64_t>(MemoryAccess::kReadWrite) + 1);
  processor->set_guest_invocation_capture_sink(nullptr);

  REQUIRE(capture.events.size() == 6);
  REQUIRE(capture.events[0].kind == CaptureEventKind::kMemory);
  REQUIRE(capture.events[0].address == kCrossPageAddress);
  REQUIRE(capture.events[0].second_address == 4);
  REQUIRE(capture.events[0].flags ==
          static_cast<uint32_t>(MemoryAccess::kRead));
  REQUIRE(capture.events[1].kind == CaptureEventKind::kUnsupported);
  REQUIRE(capture.events[1].flags == ppc::kGuestInvocationDependencyMmio);
  REQUIRE(capture.events[2].kind == CaptureEventKind::kMemory);
  REQUIRE(capture.events[2].address == kPhysicalLogicalAddress);
  REQUIRE(capture.events[2].second_address == 8);
  REQUIRE(capture.events[2].flags ==
          static_cast<uint32_t>(MemoryAccess::kWrite));
  for (size_t i = 3; i < capture.events.size(); ++i) {
    REQUIRE(capture.events[i].kind == CaptureEventKind::kUnsupported);
    REQUIRE(capture.events[i].flags ==
            ppc::kGuestInvocationDependencyUnsupportedMappingOrProtection);
  }
  for (const CaptureEvent& event : capture.events) {
    REQUIRE(event.identity.context_id == reinterpret_cast<uintptr_t>(&context));
    REQUIRE(event.identity.thread_id == context.thread_id);
  }
}

TEST_CASE("A64_CAPTURE_MEMORY_EVENTS_PRECEDE_GUEST_ACCESSES",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kPageSize = 4096;
  constexpr uint32_t kLoadSentinel = 0x11223344;
  constexpr uint32_t kStoreSentinel = 0xAABBCCDD;
  constexpr uint64_t kOffsetLoadSentinel = 0x0123456789ABCDEFull;
  constexpr uint64_t kOffsetStoreSentinel = 0xFEDCBA9876543210ull;
  constexpr vec128_t kStoredVector =
      vec128i(0x11111111, 0x22222222, 0x33333333, 0x44444444);
  constexpr vec128_t kLoadedVector =
      vec128i(0xA1A2A3A4, 0xB1B2B3B4, 0xC1C2C3C4, 0xD1D2D3D4);

  RecordingCaptureSink capture;
  TestFunction test([](hir::HIRBuilder& builder) {
    StoreGPR(
        builder, 3,
        builder.ZeroExtend(builder.Load(LoadGPR(builder, 4), hir::INT32_TYPE),
                           hir::INT64_TYPE));
    builder.Store(LoadGPR(builder, 4),
                  builder.LoadConstantUint32(kStoreSentinel));

    StoreGPR(builder, 7,
             builder.LoadOffset(LoadGPR(builder, 5), LoadGPR(builder, 6),
                                hir::INT64_TYPE));
    builder.StoreOffset(
        LoadGPR(builder, 5),
        builder.Add(LoadGPR(builder, 6), builder.LoadConstantInt64(8)),
        builder.LoadConstantUint64(kOffsetStoreSentinel));

    builder.Memset(LoadGPR(builder, 8), builder.LoadZeroInt8(),
                   builder.LoadConstantInt64(128));
    builder.Store(LoadGPR(builder, 9), LoadVR(builder, 4));
    StoreVR(builder, 3, builder.Load(LoadGPR(builder, 10), hir::VEC128_TYPE));
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);

  const uint32_t allocation =
      test.memory->SystemHeapAlloc(kPageSize * 4, kPageSize);
  REQUIRE(allocation != 0);
  const uint32_t cross_page_address = allocation + kPageSize - 2;
  const uint32_t wrapped_address = allocation + 512;
  const uint32_t memset_address = allocation + 2 * kPageSize - 64;
  const uint32_t vector_address = allocation + 3 * kPageSize + 256;

  std::memset(test.memory->TranslateVirtual(allocation), 0x5A, kPageSize * 4);
  std::memcpy(test.memory->TranslateVirtual(wrapped_address),
              &kOffsetLoadSentinel, sizeof(kOffsetLoadSentinel));
  std::memcpy(test.memory->TranslateVirtual(vector_address + 16),
              &kLoadedVector, sizeof(kLoadedVector));

  bool saw_load_before_access = false;
  bool saw_store_before_access = false;
  bool saw_memset_before_access = false;
  capture.memory_observer = [&](const CaptureEvent& event) {
    using MemoryAccess = ppc::GuestInvocationRecorderMemoryAccess;
    if (event.address == cross_page_address &&
        event.flags == static_cast<uint32_t>(MemoryAccess::kRead)) {
      std::memcpy(test.memory->TranslateVirtual(cross_page_address),
                  &kLoadSentinel, sizeof(kLoadSentinel));
      saw_load_before_access = true;
    } else if (event.address == cross_page_address &&
               event.flags == static_cast<uint32_t>(MemoryAccess::kWrite)) {
      uint32_t value = 0;
      std::memcpy(&value, test.memory->TranslateVirtual(cross_page_address),
                  sizeof(value));
      saw_store_before_access = value == kLoadSentinel;
    } else if (event.address == memset_address) {
      const uint8_t* bytes = test.memory->TranslateVirtual(memset_address);
      saw_memset_before_access = true;
      for (uint32_t i = 0; i < 128; ++i) {
        saw_memset_before_access &= bytes[i] == 0x5A;
      }
    }
  };

  test.processors[0]->set_guest_invocation_capture_sink(&capture);
  test.Run(
      [&](ppc::PPCContext* context) {
        context->r[4] = 0xDEAD000000000000ull | cross_page_address;
        context->r[5] = 0xFFFFF000u;
        context->r[6] = uint64_t(wrapped_address) + 0x1000;
        context->r[8] = 0xBEEF000000000000ull | memset_address;
        context->r[9] = 0xCAFE000000000000ull | vector_address;
        context->r[10] = 0xFACE000000000000ull | uint64_t(vector_address + 16);
        context->v[4] = kStoredVector;
      },
      [&](ppc::PPCContext* context) {
        REQUIRE(static_cast<uint32_t>(context->r[3]) == kLoadSentinel);
        REQUIRE(context->r[7] == kOffsetLoadSentinel);
        REQUIRE(context->v[3] == kLoadedVector);
      });
  test.processors[0]->set_guest_invocation_capture_sink(nullptr);

  REQUIRE(saw_load_before_access);
  REQUIRE(saw_store_before_access);
  REQUIRE(saw_memset_before_access);
  uint32_t stored_value = 0;
  std::memcpy(&stored_value, test.memory->TranslateVirtual(cross_page_address),
              sizeof(stored_value));
  REQUIRE(stored_value == kStoreSentinel);
  uint64_t offset_stored_value = 0;
  std::memcpy(&offset_stored_value,
              test.memory->TranslateVirtual(wrapped_address + 8),
              sizeof(offset_stored_value));
  REQUIRE(offset_stored_value == kOffsetStoreSentinel);
  const uint8_t* memset_bytes = test.memory->TranslateVirtual(memset_address);
  for (uint32_t i = 0; i < 128; ++i) {
    REQUIRE(memset_bytes[i] == 0);
  }
  vec128_t stored_vector = {};
  std::memcpy(&stored_vector, test.memory->TranslateVirtual(vector_address),
              sizeof(stored_vector));
  REQUIRE(stored_vector == kStoredVector);

  std::vector<CaptureEvent> memory_events;
  for (const CaptureEvent& event : capture.events) {
    if (event.kind == CaptureEventKind::kMemory) {
      memory_events.push_back(event);
    }
  }
  REQUIRE(memory_events.size() == 7);
  using MemoryAccess = ppc::GuestInvocationRecorderMemoryAccess;
  const uint32_t read = static_cast<uint32_t>(MemoryAccess::kRead);
  const uint32_t write = static_cast<uint32_t>(MemoryAccess::kWrite);
  REQUIRE(memory_events[0].address == cross_page_address);
  REQUIRE(memory_events[0].second_address == 4);
  REQUIRE(memory_events[0].flags == read);
  REQUIRE(memory_events[1].address == cross_page_address);
  REQUIRE(memory_events[1].second_address == 4);
  REQUIRE(memory_events[1].flags == write);
  REQUIRE(memory_events[2].address == wrapped_address);
  REQUIRE(memory_events[2].second_address == 8);
  REQUIRE(memory_events[2].flags == read);
  REQUIRE(memory_events[3].address == wrapped_address + 8);
  REQUIRE(memory_events[3].second_address == 8);
  REQUIRE(memory_events[3].flags == write);
  REQUIRE(memory_events[4].address == memset_address);
  REQUIRE(memory_events[4].second_address == 128);
  REQUIRE(memory_events[4].flags == write);
  REQUIRE(memory_events[5].address == vector_address);
  REQUIRE(memory_events[5].second_address == 16);
  REQUIRE(memory_events[5].flags == write);
  REQUIRE(memory_events[6].address == vector_address + 16);
  REQUIRE(memory_events[6].second_address == 16);
  REQUIRE(memory_events[6].flags == read);
  for (const CaptureEvent& event : memory_events) {
    REQUIRE(event.identity.thread_id == 0x100);
  }

  test.memory->SystemHeapFree(allocation);
}

TEST_CASE("A64_CAPTURE_REJECTS_RECORDED_MMIO_SITES_BEFORE_DISPATCH",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kFunctionAddress = 0x80001000;
  constexpr uint32_t kFunctionEnd = kFunctionAddress + 16;
  constexpr uint32_t kLogicalAddress = 0xE0001000;
  constexpr uint32_t kRemappedAddress = kLogicalAddress + 0x1000;
  constexpr uint32_t kStoreValue = 0xA1B2C3D4;
  constexpr uint32_t kOffsetStoreValue = 0x10203040;

  RecordingCaptureSink capture;
  MmioCaptureProbe probe = {&capture};
  TestFunction scaffold([](hir::HIRBuilder& builder) { builder.Return(); });
  REQUIRE(scaffold.processors.size() == 1);
  Processor* processor = scaffold.processors[0].get();
  REQUIRE(scaffold.memory->AddVirtualMappedRange(
      kRemappedAddress, 0xFFFFF000u, 0x1000, &probe, CapturedMmioRead,
      CapturedMmioWrite));

  auto module = std::make_unique<MmioSiteTestModule>(
      processor, kFunctionAddress, kFunctionEnd, [](hir::HIRBuilder& builder) {
        builder.SourceOffset(kFunctionAddress);
        StoreGPR(builder, 3,
                 builder.ZeroExtend(
                     builder.Load(LoadGPR(builder, 4), hir::INT32_TYPE),
                     hir::INT64_TYPE));
        builder.SourceOffset(kFunctionAddress + 4);
        builder.Store(LoadGPR(builder, 4),
                      builder.LoadConstantUint32(kStoreValue));
        builder.SourceOffset(kFunctionAddress + 8);
        StoreGPR(builder, 5,
                 builder.ZeroExtend(
                     builder.LoadOffset(LoadGPR(builder, 6),
                                        LoadGPR(builder, 7), hir::INT32_TYPE),
                     hir::INT64_TYPE));
        builder.SourceOffset(kFunctionAddress + 12);
        builder.StoreOffset(LoadGPR(builder, 6), LoadGPR(builder, 7),
                            builder.LoadConstantUint32(kOffsetStoreValue));
        builder.Return();
        return true;
      });
  processor->AddModule(std::move(module));

  ScopedRecordedMmioAwareness recorded_mmio_awareness;
  processor->set_guest_invocation_capture_sink(&capture);
  Function* function = processor->ResolveFunction(kFunctionAddress);
  REQUIRE(function != nullptr);
  const uint32_t stack_size = 64 * 1024;
  const uint32_t stack_address = scaffold.memory->SystemHeapAlloc(stack_size);
  REQUIRE(stack_address != 0);
  {
    ThreadState thread_state(processor, 0x100, stack_address + stack_size);
    ppc::PPCContext* context = thread_state.context();
    context->lr = 0xBCBCBCBC;
    context->r[4] = kLogicalAddress;
    context->r[6] = 0xE0000000;
    context->r[7] = 0x1000;
    REQUIRE(function->Call(&thread_state, static_cast<uint32_t>(context->lr)));
  }
  processor->set_guest_invocation_capture_sink(nullptr);
  scaffold.memory->SystemHeapFree(stack_address);

  std::vector<CaptureEvent> relevant_events;
  for (const CaptureEvent& event : capture.events) {
    REQUIRE(event.kind != CaptureEventKind::kMemory);
    if (event.kind == CaptureEventKind::kUnsupported ||
        event.kind == CaptureEventKind::kHostTransition) {
      relevant_events.push_back(event);
    }
  }
  REQUIRE(relevant_events.size() == 8);
  for (size_t i = 0; i < relevant_events.size(); i += 2) {
    REQUIRE(relevant_events[i].kind == CaptureEventKind::kUnsupported);
    REQUIRE(relevant_events[i].flags == ppc::kGuestInvocationDependencyMmio);
    REQUIRE(relevant_events[i + 1].kind == CaptureEventKind::kHostTransition);
  }
  REQUIRE(probe.read_address == kRemappedAddress);
  REQUIRE(probe.write_address == kRemappedAddress);
  REQUIRE(probe.write_value == xe::byte_swap(kOffsetStoreValue));
}

TEST_CASE("A64_CAPTURE_REJECTS_NONDETERMINISTIC_MEMORY_BEFORE_OPERATION",
          "[backend][guest-invocation-capture]") {
  constexpr uint32_t kInitialValue = 0x11111111;
  constexpr uint32_t kCasValue = 0x22222222;
  constexpr uint32_t kReservedStoreValue = 0x33333333;
  constexpr uint32_t kMmioAddress = 0x7FC01234;
  constexpr uint32_t kMmioStoreValue = 0xA1B2C3D4;

  RecordingCaptureSink capture;
  MmioCaptureProbe probe = {&capture};
  MMIORange mmio_range = {kMmioAddress, 0xFFFFFFFFu,      4,
                          &probe,       CapturedMmioRead, CapturedMmioWrite};
  TestFunction test([&](hir::HIRBuilder& builder) {
    StoreGPR(builder, 3, builder.LoadClock());
    hir::Value* address = LoadGPR(builder, 4);
    StoreGPR(builder, 5,
             builder.ZeroExtend(
                 builder.AtomicCompareExchange(
                     address, builder.LoadConstantUint32(kInitialValue),
                     builder.LoadConstantUint32(kCasValue)),
                 hir::INT64_TYPE));
    StoreGPR(
        builder, 6,
        builder.ZeroExtend(builder.LoadWithReserve(address, hir::INT32_TYPE),
                           hir::INT64_TYPE));
    StoreGPR(builder, 7,
             builder.ZeroExtend(
                 builder.StoreWithReserve(
                     address, builder.LoadConstantUint32(kReservedStoreValue),
                     hir::INT32_TYPE),
                 hir::INT64_TYPE));
    StoreGPR(builder, 8,
             builder.ZeroExtend(
                 builder.LoadMmio(&mmio_range, kMmioAddress, hir::INT32_TYPE),
                 hir::INT64_TYPE));
    builder.StoreMmio(&mmio_range, kMmioAddress,
                      builder.LoadConstantUint32(kMmioStoreValue));
    builder.Return();
  });
  REQUIRE(test.processors.size() == 1);

  const uint32_t guest_address = test.memory->SystemHeapAlloc(4, 4);
  REQUIRE(guest_address != 0);
  std::vector<uint32_t> values_before_atomic_operations;
  capture.unsupported_observer = [&](const CaptureEvent& event) {
    if (event.flags == ppc::kGuestInvocationDependencyAtomicReservation) {
      uint32_t value = 0;
      std::memcpy(&value, test.memory->TranslateVirtual(guest_address),
                  sizeof(value));
      values_before_atomic_operations.push_back(value);
    }
  };

  test.processors[0]->set_guest_invocation_capture_sink(&capture);
  test.Run(
      [&](ppc::PPCContext* context) {
        std::memcpy(test.memory->TranslateVirtual(guest_address),
                    &kInitialValue, sizeof(kInitialValue));
        context->r[4] = guest_address;
      },
      [](ppc::PPCContext*) {});
  test.processors[0]->set_guest_invocation_capture_sink(nullptr);

  REQUIRE(values_before_atomic_operations.size() == 3);
  REQUIRE(values_before_atomic_operations[0] == kInitialValue);
  REQUIRE(values_before_atomic_operations[1] == kCasValue);
  REQUIRE(values_before_atomic_operations[2] == kCasValue);
  uint32_t final_value = 0;
  std::memcpy(&final_value, test.memory->TranslateVirtual(guest_address),
              sizeof(final_value));
  REQUIRE(final_value == kReservedStoreValue);
  REQUIRE(probe.read_address == kMmioAddress);
  REQUIRE(probe.write_address == kMmioAddress);
  REQUIRE(probe.write_value == xe::byte_swap(kMmioStoreValue));

  std::vector<CaptureEvent> relevant_events;
  for (const CaptureEvent& event : capture.events) {
    if (event.kind == CaptureEventKind::kUnsupported ||
        event.kind == CaptureEventKind::kHostTransition) {
      relevant_events.push_back(event);
    }
  }
  REQUIRE(relevant_events.size() == 8);
  REQUIRE(relevant_events[0].kind == CaptureEventKind::kUnsupported);
  REQUIRE(relevant_events[0].flags ==
          ppc::kGuestInvocationDependencyClockOrTimebase);
  for (size_t i = 1; i <= 3; ++i) {
    REQUIRE(relevant_events[i].kind == CaptureEventKind::kUnsupported);
    REQUIRE(relevant_events[i].flags ==
            ppc::kGuestInvocationDependencyAtomicReservation);
  }
  REQUIRE(relevant_events[4].kind == CaptureEventKind::kUnsupported);
  REQUIRE(relevant_events[4].flags == ppc::kGuestInvocationDependencyMmio);
  REQUIRE(relevant_events[5].kind == CaptureEventKind::kHostTransition);
  REQUIRE(relevant_events[6].kind == CaptureEventKind::kUnsupported);
  REQUIRE(relevant_events[6].flags == ppc::kGuestInvocationDependencyMmio);
  REQUIRE(relevant_events[7].kind == CaptureEventKind::kHostTransition);

  test.memory->SystemHeapFree(guest_address);
}

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
