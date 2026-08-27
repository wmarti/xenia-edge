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

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"
#include "xenia/cpu/backend/a64/a64_guest_invocation_capture.h"
#include "xenia/cpu/guest_invocation_artifact.h"
#include "xenia/cpu/guest_invocation_capture.h"
#include "xenia/cpu/testing/util.h"

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
    events.push_back(std::move(event));
    return true;
  }

  bool OnUnsupportedDependency(
      const ppc::GuestInvocationRecorderIdentity& identity,
      uint32_t dependency_flags) override {
    CaptureEvent event = {CaptureEventKind::kUnsupported};
    event.identity = identity;
    event.flags = dependency_flags;
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

}  // namespace testing
}  // namespace cpu
}  // namespace xe

#endif
