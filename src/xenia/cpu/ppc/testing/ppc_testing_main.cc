/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <cstdlib>

#include "xenia/base/console_app_main.h"
#include "xenia/base/cvar.h"
#include "xenia/base/filesystem.h"
#include "xenia/base/literals.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/base/platform.h"
#include "xenia/base/string_buffer.h"
#include "xenia/cpu/backend/code_cache.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/jit_corpus.h"

DECLARE_bool(guest_scheduler);
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_frontend.h"
#include "xenia/cpu/processor.h"
#include "xenia/cpu/raw_module.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <unordered_set>

#if XE_PLATFORM_MAC
#include <mach/mach.h>
#include <mach/thread_info.h>
#include <time.h>
#endif

#if XE_ARCH_AMD64
#include "xenia/cpu/backend/x64/x64_backend.h"
#elif XE_ARCH_ARM64
#include "xenia/cpu/backend/a64/a64_backend.h"
#endif  // XE_ARCH

#if XE_COMPILER_MSVC
#include "xenia/base/platform_win.h"
#endif  // XE_COMPILER_MSVC

DEFINE_path(test_path, "src/xenia/cpu/ppc/testing/",
            "Directory scanned for test files.", "Other");
DEFINE_path(test_bin_path, "src/xenia/cpu/ppc/testing/bin/",
            "Directory with binary outputs of the test files.", "Other");
DEFINE_path(test_skip_file, "src/xenia/cpu/ppc/testing/skip.txt",
            "File containing test case names to skip (one per line).", "Other");
DEFINE_bool(test_only_skipped, false,
            "Invert the skip list: run only the test cases it names. Entries "
            "that pass are stale and can be deleted from it.",
            "Other");
DEFINE_path(test_passed_file, "",
            "Write the name of every passing test case here, one per line.",
            "Other");
DEFINE_bool(test_benchmark_warmed, false,
            "Run exactly one selected test once to warm and verify its JIT "
            "code, reset its guest state, then time one verified invocation "
            "in-process. macOS only.",
            "CPU");
DEFINE_transient_string(test_name, "", "Test suite name.", "General");

DEFINE_path(jit_corpus_in, "",
            "Recompile a guest code corpus captured by --jit_corpus_out and "
            "report emitted host code size. Measures a codegen change against "
            "real guest code without booting a title.",
            "CPU");
DEFINE_int32(jit_corpus_top, 20,
             "List this many heaviest functions in --jit_corpus_in output.",
             "CPU");
DEFINE_bool(
    jit_corpus_allow_incomplete, false,
    "Report --jit_corpus_in totals even when some functions failed to "
    "compile, were skipped, or disagreed with the capture's extent. "
    "Those functions are excluded from the totals, so an incomplete run "
    "understates the instruction count and is not comparable with "
    "another run -- which is why it fails by default.",
    "CPU");
DEFINE_string(jit_corpus_disasm, "",
              "Print the guest, HIR and emitted host disassembly for this "
              "guest address (hex) during --jit_corpus_in, and nothing else. "
              "Answers what a function actually emitted, which a size alone "
              "cannot -- use a corpus holding only the function of interest, "
              "as this forces disassembly for every function compiled.",
              "CPU");
DEFINE_path(jit_corpus_csv, "",
            "Write per-function corpus replay results here as CSV, for joining "
            "against profile data offline.",
            "CPU");

namespace xe {
namespace cpu {
namespace test {

using xe::cpu::ppc::PPCContext;
using namespace xe::literals;

typedef std::vector<std::pair<std::string, std::string>> AnnotationList;

constexpr uint32_t START_ADDRESS = 0x80000000;

#if XE_PLATFORM_MAC
bool ReadCurrentThreadCpuNanoseconds(thread_t thread,
                                     uint64_t& cpu_nanoseconds_out) {
  thread_basic_info_data_t info = {};
  mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
  const kern_return_t result =
      thread_info(thread, THREAD_BASIC_INFO,
                  reinterpret_cast<thread_info_t>(&info), &count);
  if (result != KERN_SUCCESS || count < THREAD_BASIC_INFO_COUNT ||
      info.user_time.seconds < 0 || info.user_time.microseconds < 0 ||
      info.system_time.seconds < 0 || info.system_time.microseconds < 0) {
    return false;
  }
  constexpr uint64_t kNanosecondsPerSecond = 1000000000ull;
  constexpr uint64_t kNanosecondsPerMicrosecond = 1000ull;
  cpu_nanoseconds_out =
      static_cast<uint64_t>(info.user_time.seconds) * kNanosecondsPerSecond +
      static_cast<uint64_t>(info.user_time.microseconds) *
          kNanosecondsPerMicrosecond +
      static_cast<uint64_t>(info.system_time.seconds) * kNanosecondsPerSecond +
      static_cast<uint64_t>(info.system_time.microseconds) *
          kNanosecondsPerMicrosecond;
  return true;
}
#endif  // XE_PLATFORM_MAC

// Load skip list from file
std::unordered_set<std::string> LoadSkipList(
    const std::filesystem::path& skip_file_path) {
  std::unordered_set<std::string> skip_list;

  FILE* f = filesystem::OpenFile(skip_file_path, "r");
  if (!f) {
    // Skip file doesn't exist or can't be opened - that's okay
    return skip_list;
  }

  char line_buffer[BUFSIZ];
  while (fgets(line_buffer, sizeof(line_buffer), f)) {
    // Remove trailing whitespace/newline
    char* end = line_buffer + strlen(line_buffer) - 1;
    while (end >= line_buffer &&
           (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t')) {
      *end = '\0';
      --end;
    }

    // Skip empty lines and comments
    if (strlen(line_buffer) == 0 || line_buffer[0] == '#') {
      continue;
    }

    skip_list.insert(std::string(line_buffer));
  }

  fclose(f);
  return skip_list;
}

struct TestCase {
  TestCase(uint32_t address, std::string& name)
      : address(address), name(name) {}
  uint32_t address;
  std::string name;
  AnnotationList annotations;
};

class TestSuite {
 public:
  TestSuite(const std::filesystem::path& src_file_path)
      : src_file_path_(src_file_path) {
    auto name = src_file_path.filename();
    name = name.replace_extension();

    name_ = xe::path_to_utf8(name);
    map_file_path_ = std::filesystem::path(XE_SOURCE_ROOT) /
                     cvars::test_bin_path / name.replace_extension(".map");
    bin_file_path_ = std::filesystem::path(XE_SOURCE_ROOT) /
                     cvars::test_bin_path / name.replace_extension(".bin");
  }

  bool Load() {
    if (!ReadMap()) {
      XELOGE("Unable to read map for test {}", src_file_path_);
      return false;
    }
    if (!ReadAnnotations()) {
      XELOGE("Unable to read annotations for test {}", src_file_path_);
      return false;
    }
    return true;
  }

  const std::string& name() const { return name_; }
  const std::filesystem::path& src_file_path() const { return src_file_path_; }
  const std::filesystem::path& map_file_path() const { return map_file_path_; }
  const std::filesystem::path& bin_file_path() const { return bin_file_path_; }
  std::vector<TestCase>& test_cases() { return test_cases_; }

 private:
  std::string name_;
  std::filesystem::path src_file_path_;
  std::filesystem::path map_file_path_;
  std::filesystem::path bin_file_path_;
  std::vector<TestCase> test_cases_;

  TestCase* FindTestCase(const std::string_view name) {
    for (auto& test_case : test_cases_) {
      if (test_case.name == name) {
        return &test_case;
      }
    }
    return nullptr;
  }

  bool ReadMap() {
    FILE* f = filesystem::OpenFile(map_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // 0000000000000000 t test_add1\n
      char* newline = strrchr(line_buffer, '\n');
      if (newline) {
        *newline = 0;
      }
      char* t_test_ = strstr(line_buffer, " t test_");
      if (!t_test_) {
        continue;
      }
      std::string address(line_buffer, t_test_ - line_buffer);
      std::string name(t_test_ + strlen(" t test_"));
      test_cases_.emplace_back(START_ADDRESS + std::stoul(address, 0, 16),
                               name);
    }
    fclose(f);
    return true;
  }

  bool ReadAnnotations() {
    TestCase* current_test_case = nullptr;
    FILE* f = filesystem::OpenFile(src_file_path_, "r");
    if (!f) {
      return false;
    }
    char line_buffer[BUFSIZ];
    while (fgets(line_buffer, sizeof(line_buffer), f)) {
      if (!strlen(line_buffer)) {
        continue;
      }
      // Eat leading whitespace.
      char* start = line_buffer;
      while (*start == ' ') {
        ++start;
      }
      if (strncmp(start, "test_", strlen("test_")) == 0) {
        // Global test label.
        std::string label(start + strlen("test_"), strchr(start, ':'));
        current_test_case = FindTestCase(label);
        if (!current_test_case) {
          XELOGE("Test case {} not found in corresponding map for {}", label,
                 src_file_path_);
          return false;
        }
      } else if (strlen(start) > 3 && start[0] == '#' && start[1] == '_') {
        // Annotation.
        // We don't actually verify anything here.
        char* next_space = strchr(start + 3, ' ');
        if (next_space) {
          // Looks legit.
          std::string key(start + 3, next_space);
          std::string value(next_space + 1);
          while (value.find_last_of(" \t\n") == value.size() - 1) {
            value.erase(value.end() - 1);
          }
          if (!current_test_case) {
            XELOGE("Annotation outside of test case in {}", src_file_path_);
            return false;
          }
          current_test_case->annotations.emplace_back(key, value);
        }
      }
    }
    fclose(f);
    return true;
  }
};

class TestRunner {
 public:
  TestRunner() : memory_size_(64_MiB) {
    memory_.reset(new Memory());
    memory_->Initialize();
  }

  ~TestRunner() {
    thread_state_.reset();
    processor_.reset();
    memory_.reset();
  }

  bool Setup(TestSuite& suite) {
    // Reset thread state first so it can properly deinitialize with the
    // existing processor before any teardown.
    thread_state_.reset();

    if (current_suite_ != &suite) {
      // New suite: rebuild Processor/Backend/Module. Tests in the same
      // suite share the same .bin and JIT cache, so we keep them alive
      // across tests to skip the backend setup mmap/signal-handler work.
      processor_.reset();
      memory_->Reset();

      std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
      if (cvars::cpu == "x64" || cvars::cpu == "any") {
        backend.reset(new xe::cpu::backend::x64::X64Backend());
      }
#elif XE_ARCH_ARM64
      if (cvars::cpu == "a64" || cvars::cpu == "any") {
        backend.reset(new xe::cpu::backend::a64::A64Backend());
      }
#endif  // XE_ARCH

      processor_.reset(new Processor(memory_.get(), nullptr));
      processor_->Setup(std::move(backend));
      processor_->set_debug_info_flags(DebugInfoFlags::kDebugInfoAll);

      auto module = std::make_unique<xe::cpu::RawModule>(processor_.get());
      if (!module->LoadFile(START_ADDRESS, suite.bin_file_path())) {
        XELOGE("Unable to load test binary {}", suite.bin_file_path());
        return false;
      }
      processor_->AddModule(std::move(module));

      // Snapshot the .bin so the same-suite path can repopulate v80000000
      // without re-reading from disk.
      bin_size_ = static_cast<uint32_t>(
          std::filesystem::file_size(suite.bin_file_path()));
      bin_cache_.assign(memory_->TranslateVirtual(START_ADDRESS),
                        memory_->TranslateVirtual(START_ADDRESS) + bin_size_);

      processor_->backend()->CommitExecutableRange(START_ADDRESS,
                                                   START_ADDRESS + 1024 * 1024);

      current_suite_ = &suite;
    } else {
      // Same suite as last test: reuse Processor/Backend. Memory::Reset
      // wipes v80000000 so restore the .bin from the cached snapshot.
      memory_->Reset();
      auto* heap = memory_->LookupHeap(START_ADDRESS);
      if (!heap->AllocFixed(START_ADDRESS, bin_size_, 0,
                            kMemoryAllocationReserve | kMemoryAllocationCommit,
                            kMemoryProtectRead | kMemoryProtectWrite)) {
        return false;
      }
      std::memcpy(memory_->TranslateVirtual(START_ADDRESS), bin_cache_.data(),
                  bin_size_);
    }

    // Add dummy space for memory.
    processor_->memory()->LookupHeap(0)->AllocFixed(
        0x10001000, 0xEFFF, 0,
        kMemoryAllocationReserve | kMemoryAllocationCommit,
        kMemoryProtectRead | kMemoryProtectWrite);

    // Simulate a thread.
    uint32_t stack_size = 64 * 1024;
    uint32_t stack_address = START_ADDRESS - stack_size;
    uint32_t pcr_address = stack_address - 0x1000;
    thread_state_.reset(
        new ThreadState(processor_.get(), 0x100, stack_address, pcr_address));

    return true;
  }

  bool PrepareTestState(TestCase& test_case) {
    // Setup test state from annotations.
    if (!SetupTestState(test_case)) {
      fprintf(stderr, "    [%s] Test setup failed\n", test_case.name.c_str());
      fflush(stderr);
      return false;
    }

#if XE_ARCH_AMD64
    // Reset MXCSR and backend flags to default FPU state before each test.
    // Without this, a previous test using VMX mode may leave FTZ/DAZ set,
    // causing subsequent scalar FPU tests to incorrectly flush denormals.
    _mm_setcsr(xe::cpu::backend::x64::DEFAULT_FPU_MXCSR);
    {
      auto* x64_backend = static_cast<xe::cpu::backend::x64::X64Backend*>(
          processor_->backend());
      auto* bctx =
          x64_backend->BackendContextForGuestContext(thread_state_->context());
      // Also drop any reservation a previous test left behind, so stwcx.
      // tests don't depend on file/test ordering.
      bctx->flags &= ~((1U << xe::cpu::backend::x64::kX64BackendMXCSRModeBit) |
                       (1U << xe::cpu::backend::x64::kX64BackendHasReserveBit));
    }
#elif XE_ARCH_ARM64
    // Reset FPCR and backend flags to default FPU state before each test.
    {
      auto* a64_backend = static_cast<xe::cpu::backend::a64::A64Backend*>(
          processor_->backend());
      auto* bctx =
          a64_backend->BackendContextForGuestContext(thread_state_->context());
      // Also drop any reservation a previous test left behind, so stwcx.
      // tests don't depend on file/test ordering.
      bctx->flags &= ~((1U << xe::cpu::backend::a64::kA64BackendFPCRModeBit) |
                       (1U << xe::cpu::backend::a64::kA64BackendHasReserveBit));
      // Explicitly reset the hardware FPCR to default FPU mode (0 = round
      // nearest, no flush-to-zero, no default-NaN). Without this, a previous
      // test that set VMX mode (FZ|DN) leaves the hardware FPCR dirty, and
      // subsequent scalar FP tests produce wrong NaN results because DN=1
      // causes ARM64 to return the default NaN instead of propagating inputs.
      a64_backend->SetGuestRoundingMode(thread_state_->context(), 0);
    }
#endif
    return true;
  }

  bool Run(TestCase& test_case) {
    if (!PrepareTestState(test_case)) {
      return false;
    }

    // Execute test.
    auto fn = processor_->ResolveFunction(test_case.address);
    if (!fn) {
      fprintf(stderr, "    [%s] Entry function not found\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }

    auto ctx = thread_state_->context();
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state_.get(), uint32_t(ctx->lr));

    return VerifyTestResults(test_case, fn);
  }

  bool VerifyTestResults(TestCase& test_case, Function* fn) {
    const bool result = CheckTestResults(test_case);
    if (!result) {
      // Also dump all disasm/etc.
      if (fn->is_guest()) {
        static_cast<xe::cpu::GuestFunction*>(fn)->debug_info()->Dump();
      }
    }

    return result;
  }

  bool RunWarmedBenchmark(TestSuite& suite, TestCase& test_case) {
#if !XE_PLATFORM_MAC
    fprintf(stderr, "    [%s] Warmed in-process benchmarking is macOS-only\n",
            test_case.name.c_str());
    fflush(stderr);
    return false;
#else
    // First execute the complete workload, not a reduced proxy. This resolves
    // all lazily reached functions and lets call-site backpatching finish.
    if (!Setup(suite)) {
      fprintf(stderr, "    [%s] Benchmark warmup setup failed\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    if (!Run(test_case)) {
      fprintf(stderr, "    [%s] Benchmark warmup verification failed\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }

    // Reset memory and ThreadState while preserving this suite's Processor,
    // Backend and JIT cache. Resolve the root before taking either clock.
    if (!Setup(suite)) {
      fprintf(stderr, "    [%s] Timed benchmark reset failed\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    if (!PrepareTestState(test_case)) {
      return false;
    }
    auto* fn = processor_->ResolveFunction(test_case.address);
    if (!fn) {
      fprintf(stderr, "    [%s] Entry function not found after warmup\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    auto* code_cache = processor_->backend()->code_cache();
    if (!code_cache) {
      fprintf(stderr, "    [%s] Backend has no code cache\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    const uint64_t placement_generation_before =
        code_cache->placement_generation();

    const thread_t current_thread = mach_thread_self();
    uint64_t thread_cpu_start = 0;
    if (current_thread == MACH_PORT_NULL ||
        !ReadCurrentThreadCpuNanoseconds(current_thread, thread_cpu_start)) {
      if (current_thread != MACH_PORT_NULL) {
        mach_port_deallocate(mach_task_self(), current_thread);
      }
      fprintf(stderr, "    [%s] Unable to read current-thread CPU time\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }

    const uint64_t wall_start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    auto* ctx = thread_state_->context();
    ctx->lr = 0xBCBCBCBC;
    fn->Call(thread_state_.get(), uint32_t(ctx->lr));
    const uint64_t wall_end = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);

    uint64_t thread_cpu_end = 0;
    const bool cpu_read_succeeded =
        ReadCurrentThreadCpuNanoseconds(current_thread, thread_cpu_end);
    mach_port_deallocate(mach_task_self(), current_thread);
    const uint64_t placement_generation_after =
        code_cache->placement_generation();

    // Always verify the timed invocation before accepting a metric.
    if (!VerifyTestResults(test_case, fn)) {
      return false;
    }
    if (!cpu_read_succeeded || thread_cpu_end <= thread_cpu_start) {
      fprintf(stderr,
              "    [%s] Current-thread CPU interval is missing or zero\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    if (wall_end <= wall_start) {
      fprintf(stderr, "    [%s] Uptime wall interval is missing or zero\n",
              test_case.name.c_str());
      fflush(stderr);
      return false;
    }
    if (placement_generation_after != placement_generation_before) {
      fprintf(stderr,
              "    [%s] Code placement changed in timed region "
              "(%llu -> %llu)\n",
              test_case.name.c_str(),
              static_cast<unsigned long long>(placement_generation_before),
              static_cast<unsigned long long>(placement_generation_after));
      fflush(stderr);
      return false;
    }

    const uint64_t thread_cpu_ns = thread_cpu_end - thread_cpu_start;
    const uint64_t uptime_raw_ns = wall_end - wall_start;
    fprintf(stdout,
            "XENIA_PPC_BENCHMARK_V1\tthread_cpu_ns=%llu\t"
            "uptime_raw_ns=%llu\tplacement_generation_before=%llu\t"
            "placement_generation_after=%llu\n",
            static_cast<unsigned long long>(thread_cpu_ns),
            static_cast<unsigned long long>(uptime_raw_ns),
            static_cast<unsigned long long>(placement_generation_before),
            static_cast<unsigned long long>(placement_generation_after));
    fflush(stdout);
    return true;
#endif  // XE_PLATFORM_MAC
  }

  bool SetupTestState(TestCase& test_case) {
    auto ppc_context = thread_state_->context();
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_IN") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        ppc_context->SetRegFromString(reg_name.c_str(), reg_value.c_str());
      } else if (it.first == "MEMORY_IN") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto p = memory_->TranslateVirtual(address);
        const char* c = bytes_str.c_str();
        while (*c) {
          while (*c == ' ') {
            ++c;
          }
          // Need at least two chars for a hex pair; otherwise c+=2 below
          // would jump past the null terminator into adjacent heap.
          if (!*c || !c[1]) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          uint32_t b = std::strtoul(ccs, nullptr, 16);
          *p = static_cast<uint8_t>(b);
          ++p;
        }
      }
    }
    return true;
  }

  bool CheckTestResults(TestCase& test_case) {
    auto ppc_context = thread_state_->context();

    bool any_failed = false;
    for (auto& it : test_case.annotations) {
      if (it.first == "REGISTER_OUT") {
        size_t space_pos = it.second.find(" ");
        auto reg_name = it.second.substr(0, space_pos);
        auto reg_value = it.second.substr(space_pos + 1);
        std::string actual_value;
        if (!ppc_context->CompareRegWithString(
                reg_name.c_str(), reg_value.c_str(), actual_value)) {
          any_failed = true;
          fprintf(stderr, "    [%s] Register %s assert failed:\n",
                  test_case.name.c_str(), reg_name.c_str());
          fprintf(stderr, "      Expected: %s == %s\n", reg_name.c_str(),
                  reg_value.c_str());
          fprintf(stderr, "        Actual: %s == %s\n", reg_name.c_str(),
                  actual_value.c_str());
          fflush(stderr);
        }
      } else if (it.first == "MEMORY_OUT") {
        size_t space_pos = it.second.find(" ");
        auto address_str = it.second.substr(0, space_pos);
        auto bytes_str = it.second.substr(space_pos + 1);
        uint32_t address = std::strtoul(address_str.c_str(), nullptr, 16);
        auto p = memory_->TranslateVirtual(address);
        const char* c = bytes_str.c_str();
        bool failed = false;
        StringBuffer expecteds;
        StringBuffer actuals;
        while (*c) {
          while (*c == ' ') {
            ++c;
          }
          if (!*c || !c[1]) {
            break;
          }
          char ccs[3] = {c[0], c[1], 0};
          c += 2;
          uint32_t expected = std::strtoul(ccs, nullptr, 16);
          uint8_t actual = *p;

          expecteds.AppendFormat(" {:02X}", expected);
          actuals.AppendFormat(" {:02X}", actual);

          if (expected != actual) {
            any_failed = true;
            failed = true;
          }
          ++p;
        }
        if (failed) {
          fprintf(stderr, "    [%s] Memory %s assert failed:\n",
                  test_case.name.c_str(), address_str.c_str());
          fprintf(stderr, "      Expected:%s\n", expecteds.to_string().c_str());
          fprintf(stderr, "        Actual:%s\n", actuals.to_string().c_str());
          fflush(stderr);
        }
      }
    }
    return !any_failed;
  }

  size_t memory_size_;
  std::unique_ptr<Memory> memory_;
  std::unique_ptr<Processor> processor_;
  std::unique_ptr<ThreadState> thread_state_;

  // Reuse Processor/Backend/Module across tests in the same suite.
  TestSuite* current_suite_ = nullptr;
  std::vector<uint8_t> bin_cache_;
  uint32_t bin_size_ = 0;
};

bool DiscoverTests(const std::filesystem::path& test_path,
                   std::vector<std::filesystem::path>& test_files) {
  auto file_infos = xe::filesystem::ListFiles(
      std::filesystem::path(XE_SOURCE_ROOT) / test_path);
  for (auto& file_info : file_infos) {
    if (file_info.name.extension() == ".s") {
      // Only include test files (instr_*.s), not helper files
      auto filename = file_info.name.filename().string();
      if (filename.find("instr_") == 0) {
        test_files.push_back(test_path / file_info.name);
      }
    }
  }
  return true;
}

#if XE_COMPILER_MSVC
int filter(unsigned int code) {
  if (code == EXCEPTION_ILLEGAL_INSTRUCTION) {
    return EXCEPTION_EXECUTE_HANDLER;
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
#endif  // XE_COMPILER_MSVC

void ProtectedRunTest(TestSuite& test_suite, TestRunner& runner,
                      TestCase& test_case, int& failed_count, int& passed_count,
                      std::vector<std::string>& passed_names) {
#if XE_COMPILER_MSVC
  try {
    if (!runner.Setup(test_suite)) {
      fprintf(stderr, "  [%s] FAILED SETUP\n", test_case.name.c_str());
      fflush(stderr);
      ++failed_count;
      return;
    }
    if (runner.Run(test_case)) {
      ++passed_count;
      passed_names.push_back(test_case.name);
    } else {
      fprintf(stderr, "  [%s] FAILED\n", test_case.name.c_str());
      fflush(stderr);
      ++failed_count;
    }
  } catch (const std::exception& e) {
    fprintf(stderr, "  [%s] CRASHED (C++ exception: %s)\n",
            test_case.name.c_str(), e.what());
    fflush(stderr);
    ++failed_count;
  }
#else
  // Run directly in-process; the amortized runner makes fork-per-test
  // both expensive and impossible (cache would not survive).
  if (!runner.Setup(test_suite)) {
    fprintf(stderr, "  [%s] FAILED SETUP\n", test_case.name.c_str());
    fflush(stderr);
    ++failed_count;
    return;
  }
  if (runner.Run(test_case)) {
    ++passed_count;
    passed_names.push_back(test_case.name);
  } else {
    fprintf(stderr, "  [%s] FAILED\n", test_case.name.c_str());
    fflush(stderr);
    ++failed_count;
  }
#endif  // XE_COMPILER_MSVC
}

bool RunTests(const std::vector<std::string>& test_names) {
  int result_code = 1;
  int failed_count = 0;
  int passed_count = 0;

#if XE_ARCH_AMD64
  XELOGI("Instruction feature mask {}.", cvars::x64_extension_mask);
#endif  // XE_ARCH_AMD64

  // Load skip list
  auto skip_list = LoadSkipList(cvars::test_skip_file);
  if (!skip_list.empty()) {
    fprintf(stderr, "Loaded skip list with %zu test cases to %s.\n",
            skip_list.size(), cvars::test_only_skipped ? "run" : "skip");
  } else {
    fprintf(stderr, "Warning: skip list is empty (path: %s)\n",
            cvars::test_skip_file.string().c_str());
  }
  // Inverted, the skip list becomes the run list: whatever passes is a stale
  // entry that can be deleted from it.
  auto should_run = [&skip_list](const std::string& name) {
    return (skip_list.find(name) != skip_list.end()) ==
           cvars::test_only_skipped;
  };
  std::vector<std::string> passed_names;

  // Build a set of requested test names for fast lookup
  std::unordered_set<std::string> test_name_filter(test_names.begin(),
                                                   test_names.end());

  auto test_path_root = cvars::test_path;
  std::vector<std::filesystem::path> test_files;
  if (!DiscoverTests(test_path_root, test_files)) {
    return false;
  }
  if (!test_files.size()) {
    XELOGE("No tests discovered - invalid path?");
    return false;
  }
  XELOGI("{} tests discovered.", test_files.size());
  XELOGI("");

  std::vector<TestSuite> test_suites;
  bool load_failed = false;
  for (auto& test_path : test_files) {
    TestSuite test_suite(std::filesystem::path(XE_SOURCE_ROOT) / test_path);
    if (!test_name_filter.empty() &&
        test_name_filter.find(test_suite.name()) == test_name_filter.end()) {
      continue;
    }
    if (!test_suite.Load()) {
      XELOGE("TEST SUITE {} FAILED TO LOAD", test_path);
      load_failed = true;
      continue;
    }
    test_suites.push_back(std::move(test_suite));
  }
  if (load_failed) {
    XELOGE("One or more test suites failed to load.");
  }

  XELOGI("{} tests loaded.", test_suites.size());

  // Collect all test cases across all suites, filtering out skipped tests
  std::vector<std::pair<TestSuite*, TestCase*>> all_tests;
  int skipped_count = 0;
  for (auto& test_suite : test_suites) {
    for (auto& test_case : test_suite.test_cases()) {
      if (!should_run(test_case.name)) {
        ++skipped_count;
        continue;
      }
      all_tests.push_back({&test_suite, &test_case});
    }
  }

  if (skipped_count > 0) {
    fprintf(stderr, "Filtered out %d test cases based on skip list.\n",
            skipped_count);
  }
  if (cvars::test_benchmark_warmed &&
      (test_suites.size() != 1 || all_tests.size() != 1)) {
    fprintf(stderr,
            "--test_benchmark_warmed requires exactly one loaded suite and "
            "one runnable test case (got %zu suites, %zu cases).\n",
            test_suites.size(), all_tests.size());
    fflush(stderr);
    return false;
  }
  fprintf(stderr, "Running %zu test suites, %zu test cases...\n",
          test_suites.size(), all_tests.size());

  auto start_time = std::chrono::steady_clock::now();

  // Run tests serially grouped by suite. The TestRunner amortizes
  // Processor/Backend/Module/JIT across tests in the same suite.
  TestRunner runner;
  int suite_index = 0;
  int suite_total = 0;
  size_t tests_done = 0;
  size_t total_tests = all_tests.size();
  for (auto& test_suite : test_suites) {
    for (auto& test_case : test_suite.test_cases()) {
      if (should_run(test_case.name)) {
        ++suite_total;
        break;
      }
    }
  }
  for (auto& test_suite : test_suites) {
    // Collect non-skipped test cases for this suite
    std::vector<TestCase*> suite_tests;
    for (auto& test_case : test_suite.test_cases()) {
      if (should_run(test_case.name)) {
        suite_tests.push_back(&test_case);
      }
    }
    if (suite_tests.empty()) {
      continue;
    }
    ++suite_index;

    int pct =
        total_tests ? static_cast<int>(tests_done * 100 / total_tests) : 0;
    fprintf(stdout, "[%d/%d] %s (%zu tests) %d%%\n", suite_index, suite_total,
            test_suite.name().c_str(), suite_tests.size(), pct);
    fflush(stdout);
    for (size_t i = 0; i < suite_tests.size(); i++) {
      if (cvars::test_benchmark_warmed) {
        if (runner.RunWarmedBenchmark(test_suite, *suite_tests[i])) {
          ++passed_count;
          passed_names.push_back(suite_tests[i]->name);
        } else {
          fprintf(stderr, "  [%s] FAILED\n", suite_tests[i]->name.c_str());
          fflush(stderr);
          ++failed_count;
        }
      } else {
        ProtectedRunTest(test_suite, runner, *suite_tests[i], failed_count,
                         passed_count, passed_names);
      }
      ++tests_done;
      if ((i + 1) % 500 == 0 && i + 1 < suite_tests.size()) {
        pct = static_cast<int>(tests_done * 100 / total_tests);
        fprintf(stdout, "  ... %zu/%zu %d%%\n", i + 1, suite_tests.size(), pct);
        fflush(stdout);
      }
    }
  }

  auto end_time = std::chrono::steady_clock::now();
  auto elapsed_sec =
      std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time)
          .count();
  int minutes = static_cast<int>(elapsed_sec / 60);
  int seconds = static_cast<int>(elapsed_sec % 60);

  fprintf(stderr, "\nTotal tests: %d\n", failed_count + passed_count);
  fprintf(stderr, "Passed: %d\n", passed_count);
  fprintf(stderr, "Failed: %d\n", failed_count);
  fprintf(stderr, "Time: %dm %ds\n", minutes, seconds);
  fflush(stderr);

  if (!cvars::test_passed_file.empty()) {
    std::sort(passed_names.begin(), passed_names.end());
    FILE* f = filesystem::OpenFile(cvars::test_passed_file, "w");
    if (f) {
      for (const auto& name : passed_names) {
        fprintf(f, "%s\n", name.c_str());
      }
      fclose(f);
      fprintf(stderr, "Wrote %zu passing test names to %s\n",
              passed_names.size(), cvars::test_passed_file.string().c_str());
    } else {
      fprintf(stderr, "Failed to open %s for writing\n",
              cvars::test_passed_file.string().c_str());
    }
  }

  return failed_count ? false : true;
}

// Offline codegen replay: recompile the guest code a live run actually JIT'd
// and report what the backend emitted for it.
//
// This exists because a microbenchmark measures the instruction sequences
// someone chose to look at, while a title runs whatever mix it runs. A corpus
// replay measures the real mix, costs seconds, and needs no title, no GPU and
// no kernel -- Processor takes a bare Memory and a null export resolver.
//
// What it measures is emitted host code, not time. That is a proxy, but it is
// a proxy worth having: weighting guest ops by host instructions per op is
// what tells you which opcode is worth optimising, as opposed to which one is
// easy to write a kernel for.
//
// Faithfulness is checked, not assumed: the corpus records the host size each
// function produced during capture, so replaying a corpus on the binary that
// captured it must reproduce every size exactly. The mismatch count is
// reported, and a nonzero count on an unchanged binary means the offline
// environment diverges and its totals cannot be trusted.
namespace corpus {

struct FunctionResult {
  uint32_t address;
  uint32_t guest_instructions;
  uint32_t host_bytes;
  uint32_t captured_host_bytes;
  uint32_t wide_move_instructions;
  // Split out so a per-function join against execution counts can price the
  // materialization sites separately from the immediates. A chain is one site;
  // its instruction count varies with where the process happened to load.
  uint32_t address_chains;
  uint32_t address_chain_instructions;
};

// Outside the ARM64 guard on purpose: the totals below are summed and
// printed unconditionally, so the type has to exist on every host. On a
// backend with no wide-move idiom every counter stays zero, which is the
// correct answer rather than a missing one.
struct WideMoveStats {
  uint32_t total = 0;
  // Instructions belonging to a chain whose value needs more than 32 bits, i.e.
  // a host address in this process.
  uint32_t in_address_chains = 0;
  // Chains whose value fits in 32 bits: guest immediates and small constants.
  uint32_t in_small_chains = 0;
  // Chains that begin with a MOVK, so the starting value is unknown and no
  // claim can be made. Expected to be zero; counted rather than assumed.
  uint32_t in_unknown_chains = 0;
  // How many chains, not how many instructions. The instruction count of an
  // address chain varies with the load address; the number of chains does not,
  // because it is one per materialization site in the emitted code. This is
  // what lets `stable` charge a fixed price per materialization instead of
  // excluding the whole thing -- see the stable computation for why excluding
  // it made the metric score the obvious optimization backwards.
  uint32_t address_chains = 0;
  uint32_t small_chains = 0;
  uint32_t unknown_chains = 0;
};

#if XE_ARCH_ARM64
// Count MOVZ/MOVK in a block of emitted a64 code.
//
// These are how a 64-bit constant is built, and the emitter builds host
// addresses that way -- the address of a helper, of the guest-to-host thunk.
// Xbyak_aarch64 emits one instruction per nonzero 16-bit lane, so the count
// depends on where the process happened to be loaded. That makes raw emitted
// size vary between runs of the SAME binary: measured 0.65% between two replays
// of one corpus and 3.5% between a capture and a replay, scaling with the
// number of call sites in a function (up to 7% on the largest).
//
// Excluding them gives a total that is stable across processes and is therefore
// the number to compare when A/B-ing a codegen change. It is not a substitute
// for the raw total: a change that alters how constants are built is invisible
// here, so both are reported.
//
// Move wide immediate: sf | opc(2) | 100101 | hw(2) | imm16 | Rd, with opc=00
// for MOVN, 10 for MOVZ and 11 for MOVK. Masking off sf leaves bits 30:23.
// A constant is a MOVZ/MOVN followed by MOVKs into the same register, one per
// remaining lane that the first instruction did not already produce.
//
// The point of splitting these out is to say how much of the MOVZ/MOVK bulk is
// *host address* materialization -- which a single load from the backend
// context could replace -- versus guest immediates, which cannot be.
//
// Classify by the reconstructed value, not by chain length. Chain length is not
// a usable proxy: the assembler emits one MOVK per lane that differs from what
// MOVZ/MOVN already put there, so a host address whose bits 47:32 happen to be
// zero (or 0xFFFF) needs one instruction fewer. Which lanes are zero depends on
// where the process was loaded, so the same binary reports different chain
// lengths run to run -- measured 5.00% vs 8.82% of emitted instructions "in
// chains > 2 lanes" across two replays of one corpus whose totals agreed to
// within 0.001%. The reconstructed value stays above 4 GiB in every run, so the
// classification is stable even though the immediate is not.

bool IsMovz(uint32_t insn) { return (insn & 0x7F800000u) == 0x52800000u; }
bool IsMovk(uint32_t insn) { return (insn & 0x7F800000u) == 0x72800000u; }
bool IsMovn(uint32_t insn) { return (insn & 0x7F800000u) == 0x12800000u; }
uint32_t WideMoveRd(uint32_t insn) { return insn & 0x1Fu; }
uint32_t WideMoveImm16(uint32_t insn) { return (insn >> 5) & 0xFFFFu; }
uint32_t WideMoveShift(uint32_t insn) { return ((insn >> 21) & 0x3u) * 16u; }

void AccumulateWideMoves(const uint8_t* code, size_t length,
                         WideMoveStats* stats) {
  const size_t instruction_count = length / 4;
  const uint32_t* words = reinterpret_cast<const uint32_t*>(code);
  for (size_t i = 0; i < instruction_count;) {
    const uint32_t first = words[i];
    if (!IsMovz(first) && !IsMovk(first) && !IsMovn(first)) {
      ++i;
      continue;
    }
    // Walk the chain: same destination register, MOVK continuation.
    const uint32_t rd = WideMoveRd(first);
    uint64_t value = 0;
    bool value_known = true;
    if (IsMovz(first)) {
      value = uint64_t(WideMoveImm16(first)) << WideMoveShift(first);
    } else if (IsMovn(first)) {
      value = ~(uint64_t(WideMoveImm16(first)) << WideMoveShift(first));
    } else {
      value_known = false;  // starts mid-chain; nothing to reconstruct from
    }
    size_t j = i + 1;
    while (j < instruction_count && IsMovk(words[j]) &&
           WideMoveRd(words[j]) == rd) {
      const uint32_t shift = WideMoveShift(words[j]);
      value &= ~(uint64_t(0xFFFFu) << shift);
      value |= uint64_t(WideMoveImm16(words[j])) << shift;
      ++j;
    }
    // A 32-bit form leaves the upper half zero regardless of the lanes written.
    if (!(first & 0x80000000u)) {
      value &= 0xFFFFFFFFu;
    }
    const uint32_t chain = static_cast<uint32_t>(j - i);
    stats->total += chain;
    if (!value_known) {
      stats->in_unknown_chains += chain;
      ++stats->unknown_chains;
    } else if (value > 0xFFFFFFFFull) {
      stats->in_address_chains += chain;
      ++stats->address_chains;
    } else {
      stats->in_small_chains += chain;
      ++stats->small_chains;
    }
    i = j;
  }
}
#endif  // XE_ARCH_ARM64

bool RunCorpusReplay() {
  auto corpus = JitCorpus::Read(cvars::jit_corpus_in);
  if (!corpus) {
    fprintf(stderr, "Could not read a JIT corpus from %s\n",
            xe::path_to_utf8(cvars::jit_corpus_in).c_str());
    return false;
  }
  if (corpus->page_addresses().empty() || corpus->functions().empty()) {
    fprintf(stderr, "Corpus is empty (%zu pages, %zu functions).\n",
            corpus->page_addresses().size(), corpus->functions().size());
    return false;
  }
  if (corpus->truncated()) {
    fprintf(stdout,
            "note: corpus was truncated mid-record (capture was killed); "
            "replaying the %zu complete function records.\n",
            corpus->functions().size());
  }

  // A replay compiles with whatever cvars this process has, so any setting that
  // changes codegen has to be taken from the capture rather than from the
  // defaults of a different binary. guest_scheduler is the one that bites:
  // it gates PreemptCheckInjectionPass, and replaying a --guest_scheduler=false
  // capture under the default made 13,323 of 13,564 functions differ from their
  // own capture. Applied before the first compile, and reported, because a
  // silently mismatched replay still prints a confident total.
  if (!corpus->config_known()) {
    fprintf(stdout,
            "  config     v1 corpus: capture configuration unknown; compiling "
            "with this process's flags (--guest_scheduler=%s)\n",
            cvars::guest_scheduler ? "true" : "false");
  } else if (corpus->captured_with_guest_scheduler() !=
             cvars::guest_scheduler) {
    fprintf(stdout,
            "  config     capture had --guest_scheduler=%s, this process has "
            "%s; using the capture's\n",
            corpus->captured_with_guest_scheduler() ? "true" : "false",
            cvars::guest_scheduler ? "true" : "false");
    cvars::guest_scheduler = corpus->captured_with_guest_scheduler();
  }

  // Disassembly is built during translation, so this has to be latched before
  // the first compile rather than at the point it is printed.
  if (!cvars::jit_corpus_disasm.empty()) {
    cvars::disassemble_functions = true;
  }

  auto memory = std::make_unique<Memory>();
  memory->Initialize();

  std::unique_ptr<xe::cpu::backend::Backend> backend;
#if XE_ARCH_AMD64
  if (cvars::cpu == "x64" || cvars::cpu == "any") {
    backend.reset(new xe::cpu::backend::x64::X64Backend());
  }
#elif XE_ARCH_ARM64
  if (cvars::cpu == "a64" || cvars::cpu == "any") {
    backend.reset(new xe::cpu::backend::a64::A64Backend());
  }
#endif  // XE_ARCH
  if (!backend) {
    fprintf(stderr, "No backend for this host/--cpu combination.\n");
    return false;
  }
  auto processor = std::make_unique<Processor>(memory.get(), nullptr);
  processor->Setup(std::move(backend));
  // Deliberately NOT setting debug info flags, to match a capture run that was
  // not launched with --debug. A capture taken with --debug would emit extra
  // per-function instrumentation and its sizes would not reproduce here.

  // Map the captured code pages, one allocation per contiguous run.
  const auto& page_addresses = corpus->page_addresses();
  const uint8_t* page_data = corpus->page_data().data();
  size_t run_count = 0;
  for (size_t i = 0; i < page_addresses.size();) {
    size_t j = i + 1;
    while (j < page_addresses.size() &&
           page_addresses[j] == page_addresses[j - 1] + JitCorpus::kPageSize) {
      ++j;
    }
    const uint32_t base = page_addresses[i];
    const uint32_t size = static_cast<uint32_t>((j - i) * JitCorpus::kPageSize);
    auto* heap = memory->LookupHeap(base);
    if (!heap ||
        !heap->AllocFixed(base, size, 0,
                          kMemoryAllocationReserve | kMemoryAllocationCommit,
                          kMemoryProtectRead | kMemoryProtectWrite)) {
      fprintf(stderr, "Failed to map corpus pages at %.8X (+%u bytes).\n", base,
              size);
      return false;
    }
    std::memcpy(memory->TranslateVirtual(base),
                page_data + i * JitCorpus::kPageSize, size);
    ++run_count;
    i = j;
  }
  const uint32_t low = page_addresses.front();
  const uint32_t high = page_addresses.back() + JitCorpus::kPageSize;

  // Commit the gaps between runs as zeros. A corpus only carries the pages its
  // functions occupied, but the scanner walks forward from an entry point and
  // decides where the function ends, and offline it can walk further than the
  // capture did (the capture may have had an end address supplied by the XEX
  // that a bare RawModule has to rediscover). Reading an unmapped gap would
  // fault; reading zeros is handled -- ppc_scanner treats a 0x00000000 fetch as
  // the end of the function, which is exactly the right behaviour here. Invalid
  // instructions, by contrast, do NOT stop the scan, so leaving gaps filled
  // with garbage would not be safe.
  uint32_t gap_pages = 0;
  uint32_t gap_failures = 0;
  for (size_t i = 1; i < page_addresses.size(); ++i) {
    uint32_t gap = page_addresses[i - 1] + JitCorpus::kPageSize;
    while (gap < page_addresses[i]) {
      // Clamp each allocation to one heap, since a gap can span heaps.
      auto* heap = memory->LookupHeap(gap);
      if (!heap) {
        ++gap_failures;
        gap += JitCorpus::kPageSize;
        continue;
      }
      const uint32_t heap_end = heap->heap_base() + heap->heap_size();
      const uint32_t chunk_end = std::min(page_addresses[i], heap_end);
      const uint32_t chunk = chunk_end - gap;
      if (!heap->AllocFixed(gap, chunk, 0,
                            kMemoryAllocationReserve | kMemoryAllocationCommit,
                            kMemoryProtectRead | kMemoryProtectWrite)) {
        ++gap_failures;
      } else {
        gap_pages += chunk / JitCorpus::kPageSize;
      }
      gap = chunk_end;
    }
  }

  auto module = std::make_unique<xe::cpu::RawModule>(processor.get());
  module->set_name("corpus");
  auto* module_ptr = module.get();
  processor->AddModule(std::move(module));
  module_ptr->SetAddressRange(low, high - low);

  // Compile in address order so the run is deterministic regardless of the
  // order the capture happened to define functions in.
  std::vector<JitCorpus::FunctionRecord> records = corpus->functions();
  std::sort(records.begin(), records.end(),
            [](const auto& a, const auto& b) { return a.address < b.address; });
  records.erase(std::unique(records.begin(), records.end(),
                            [](const auto& a, const auto& b) {
                              return a.address == b.address;
                            }),
                records.end());

  // Report the setup BEFORE compiling: recompiling untrusted guest code can
  // fault, and a report printed only at the end would take the diagnosis with
  // it. The progress line names the function being compiled for the same
  // reason.
  fprintf(stdout, "\nJIT corpus replay: %s\n",
          xe::path_to_utf8(cvars::jit_corpus_in).c_str());
  fprintf(stdout,
          "  code       %zu pages in %zu runs, %.8X..%.8X (%.2f MiB), %u gap "
          "pages zero-filled, %u gap allocations failed\n",
          page_addresses.size(), run_count, low, high,
          double(page_addresses.size() * JitCorpus::kPageSize) / (1024 * 1024),
          gap_pages, gap_failures);
  fprintf(stdout, "  records    %zu unique addresses\n", records.size());
  fflush(stdout);

  std::vector<FunctionResult> results;
  results.reserve(records.size());
  uint64_t total_host_bytes = 0;
  uint64_t total_wide_moves = 0;
  WideMoveStats wide_move_stats;
  uint64_t total_guest_instructions = 0;
  uint64_t total_captured_bytes = 0;
  uint32_t failed = 0;
  uint32_t skipped = 0;
  uint32_t validated = 0;
  uint32_t mismatched = 0;
  // Functions the offline scan gave a different extent than the capture did. A
  // codegen comparison is only meaningful once this is zero: a function that
  // covers different guest instructions is not the same function.
  uint32_t extent_mismatched = 0;

  const auto start = std::chrono::steady_clock::now();
  size_t index = 0;
  for (const auto& record : records) {
    // Unbuffered and on stderr so it survives a fault in the compile below.
    if ((index++ % 256) == 0) {
      fprintf(stderr, "\r  compiling %zu/%zu (%.8X)   ", index, records.size(),
              record.address);
      fflush(stderr);
    }
    // A record with no usable extent had no code pages captured for it, so
    // rescanning it here would read whatever the gap fill left behind.
    //
    // end < start is not hypothetical: ppc_scanner sets end_address to
    // address-4 when the first word it fetches is zero, and PPCHIRBuilder::Emit
    // then sizes an allocation from (end - start), which underflows to ~4 GiB
    // and memsets 8.5 GiB. That is how the first version of this replay died.
    if (!record.end_address || record.end_address < record.address) {
      ++skipped;
      continue;
    }
    auto* function = processor->ResolveFunction(record.address);
    if (!function || !function->is_guest()) {
      ++failed;
      continue;
    }
    auto* guest_function = static_cast<GuestFunction*>(function);
    if (guest_function->end_address() != record.end_address) {
      ++extent_mismatched;
    }
    if (!cvars::jit_corpus_disasm.empty()) {
      const uint32_t want = static_cast<uint32_t>(
          std::strtoul(cvars::jit_corpus_disasm.c_str(), nullptr, 16));
      if (record.address == want) {
        auto* info = guest_function->debug_info();
        fprintf(stdout, "\n=== %.8X..%.8X, %zu host bytes ===\n",
                record.address, record.end_address,
                guest_function->machine_code_length());
        if (info && info->source_disasm()) {
          fprintf(stdout, "\n--- guest ---\n%s", info->source_disasm());
        }
        if (info && info->hir_disasm()) {
          fprintf(stdout, "\n--- HIR (optimized) ---\n%s", info->hir_disasm());
        }
        if (info && info->machine_code_disasm()) {
          fprintf(stdout, "\n--- host ---\n%s", info->machine_code_disasm());
        }
        fflush(stdout);
      }
    }
    const uint32_t host_bytes =
        static_cast<uint32_t>(guest_function->machine_code_length());
    const uint32_t guest_instructions =
        (record.end_address - record.address) / 4 + 1;
    uint32_t wide_moves = 0;
    uint32_t fn_address_chains = 0;
    uint32_t fn_address_chain_instructions = 0;
#if XE_ARCH_ARM64
    WideMoveStats fn_stats;
    AccumulateWideMoves(guest_function->machine_code(), host_bytes, &fn_stats);
    wide_moves = fn_stats.total;
    fn_address_chains = fn_stats.address_chains;
    fn_address_chain_instructions = fn_stats.in_address_chains;
    wide_move_stats.total += fn_stats.total;
    wide_move_stats.in_address_chains += fn_stats.in_address_chains;
    wide_move_stats.in_small_chains += fn_stats.in_small_chains;
    wide_move_stats.in_unknown_chains += fn_stats.in_unknown_chains;
    wide_move_stats.address_chains += fn_stats.address_chains;
    wide_move_stats.small_chains += fn_stats.small_chains;
    wide_move_stats.unknown_chains += fn_stats.unknown_chains;
#endif  // XE_ARCH_ARM64
    total_host_bytes += host_bytes;
    total_wide_moves += wide_moves;
    total_guest_instructions += guest_instructions;
    if (record.captured_host_bytes_valid()) {
      total_captured_bytes += record.host_code_size;
      if (record.host_code_size == host_bytes) {
        ++validated;
      } else {
        ++mismatched;
      }
    }
    results.push_back({record.address, guest_instructions, host_bytes,
                       record.host_code_size, wide_moves, fn_address_chains,
                       fn_address_chain_instructions});
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  const double compile_seconds = std::chrono::duration<double>(elapsed).count();

  fprintf(stderr, "\r%40s\r", "");
  fprintf(stdout, "  functions  %zu compiled, %u failed, %u skipped\n",
          results.size(), failed, skipped);
  fprintf(stdout, "  guest      %llu instructions\n",
          static_cast<unsigned long long>(total_guest_instructions));
  fprintf(stdout, "  host       %llu bytes (%llu instructions)\n",
          static_cast<unsigned long long>(total_host_bytes),
          static_cast<unsigned long long>(total_host_bytes / 4));
  // `stable` charges one instruction per address materialization instead of
  // excluding the materialization entirely.
  //
  // Excluding it (which is what this did originally, by subtracting EVERY
  // MOVZ/MOVK) makes the metric score the obvious optimization BACKWARDS:
  // replacing a 3-instruction MOVZ/MOVK address chain with a single ldr from
  // the backend context moves that fragment from 0 counted instructions to 1,
  // so `stable` RISES by one while the emitted code shrinks by two. That is
  // precisely the change the "replaceable by one ldr" note below is inviting,
  // so the metric would have punished the fix it was pointing at.
  //
  // Charging one per chain is ASLR-invariant for the reason the classification
  // is: chain LENGTH depends on which 16-bit lanes of the load address happen
  // to be zero, but the NUMBER of chains is one per materialization site in the
  // emitted code. So a chain and the ldr that replaces it both cost 1, and
  // `stable` neither rewards nor punishes the substitution -- it stays a pure
  // regression detector, and the address-chain count below is what shows the
  // substitution happened.
  //
  // Chains building 32-bit-or-smaller values are guest immediates. Those are a
  // EVERY chain is charged 1, not just the ones classified as addresses. An
  // earlier version of this charged only the >32-bit chains and counted the
  // 32-bit-or-smaller ones in full, on the theory that those are guest
  // immediates and so identical in every process. Measured over 10 runs, that
  // was bimodal: 8 runs at one value and 2 at another 40,819 lower, and the
  // whole difference was in the "32-bit-or-smaller" bucket. The reason is that
  // a 64-bit-form chain whose reconstructed value happens to fall below 4 GiB
  // is indistinguishable from a guest immediate, and Xenia does place host
  // allocations low (the trampoline region and the trace counter both ask for
  // sub-4-GiB mappings). So that bucket carries host addresses too, and its
  // instruction count moves with the load address.
  //
  // Charging every chain 1 removes chain length from the metric entirely. The
  // cost is that a change to how guest constants are BUILT is invisible here --
  // which was already true of the old formula, and is why the raw total is
  // reported alongside.
  const uint64_t total_chains = wide_move_stats.address_chains +
                                wide_move_stats.small_chains +
                                wide_move_stats.unknown_chains;
  const uint64_t stable_instructions =
      total_host_bytes / 4 - total_wide_moves + total_chains;
  fprintf(stdout,
          "  stable     %llu instructions (%llu MOVZ/MOVN/MOVK charged as %llu "
          "materializations, 1 each) <-- compare THIS across processes\n",
          static_cast<unsigned long long>(stable_instructions),
          static_cast<unsigned long long>(total_wide_moves),
          static_cast<unsigned long long>(total_chains));
#if XE_ARCH_ARM64
  {
    const double per_total =
        total_host_bytes ? 100.0 / double(total_host_bytes / 4) : 0.0;
    fprintf(stdout,
            "  wide moves %u instructions in %u chains build >32-bit values "
            "(%.2f%% of all emitted), %u in %u chains build "
            "32-bit-or-smaller values, %u in %u unclassified chains\n",
            wide_move_stats.in_address_chains, wide_move_stats.address_chains,
            wide_move_stats.in_address_chains * per_total,
            wide_move_stats.in_small_chains, wide_move_stats.small_chains,
            wide_move_stats.in_unknown_chains, wide_move_stats.unknown_chains);
    fprintf(
        stdout,
        "             a >32-bit value is a host address in this process -- "
        "replaceable by one ldr from the backend context. Classified by the "
        "reconstructed immediate, so this is stable across load addresses. "
        "Watch the CHAIN COUNT to score such a change; the instruction total "
        "is not comparable across processes.\n");
    if (wide_move_stats.address_chains) {
      fprintf(
          stdout,
          "             at %.2f instructions per chain here, replacing all "
          "of them with one load each would remove %u executed instructions "
          "-- an in-process figure, not a cross-process one.\n",
          double(wide_move_stats.in_address_chains) /
              double(wide_move_stats.address_chains),
          wide_move_stats.in_address_chains - wide_move_stats.address_chains);
    }
  }
#endif  // XE_ARCH_ARM64
  if (total_guest_instructions) {
    fprintf(stdout, "  host/gi    %.4f (stable %.4f)\n",
            double(total_host_bytes / 4) / double(total_guest_instructions),
            double(stable_instructions) / double(total_guest_instructions));
  }
  fprintf(stdout, "  compile    %.3f s (%.0f functions/s)\n", compile_seconds,
          compile_seconds > 0 ? double(results.size()) / compile_seconds : 0.0);

  // The faithfulness check. Without this the totals above are just numbers.
  fprintf(stdout, "  extents    %u of %zu differ from capture\n",
          extent_mismatched, results.size());
  if (validated || mismatched) {
    fprintf(stdout,
            "  vs capture %u functions identical, %u differ (captured %llu "
            "bytes, replayed %llu)\n",
            validated, mismatched,
            static_cast<unsigned long long>(total_captured_bytes),
            static_cast<unsigned long long>(total_host_bytes));
    if (mismatched) {
      fprintf(stdout,
              "             a nonzero differ count on the SAME binary that "
              "captured the corpus means this replay is not faithful; on a "
              "CHANGED binary it is the size of the change.\n");
    }
  }

  const int top = static_cast<int>(cvars::jit_corpus_top);
  if (top > 0 && !results.empty()) {
    auto by_host_bytes = results;
    std::sort(by_host_bytes.begin(), by_host_bytes.end(),
              [](const auto& a, const auto& b) {
                return a.host_bytes > b.host_bytes;
              });
    const size_t show =
        std::min(static_cast<size_t>(top), by_host_bytes.size());
    fprintf(stdout, "\n  heaviest %zu functions by emitted host bytes:\n",
            show);
    fprintf(stdout, "    %-10s %8s %10s %8s  %s\n", "guest", "gi", "host_b",
            "host/gi", "delta_vs_capture");
    for (size_t i = 0; i < show; ++i) {
      const auto& r = by_host_bytes[i];
      fprintf(stdout, "    %.8X   %8u %10u %8.2f  %+d\n", r.address,
              r.guest_instructions, r.host_bytes,
              r.guest_instructions
                  ? double(r.host_bytes / 4) / double(r.guest_instructions)
                  : 0.0,
              r.captured_host_bytes
                  ? int(r.host_bytes) - int(r.captured_host_bytes)
                  : 0);
    }
  }

  if (!cvars::jit_corpus_csv.empty()) {
    FILE* csv = xe::filesystem::OpenFile(cvars::jit_corpus_csv, "wb");
    if (!csv) {
      fprintf(stderr, "Could not open %s for writing.\n",
              xe::path_to_utf8(cvars::jit_corpus_csv).c_str());
      return false;
    }
    fprintf(csv,
            "guest_address,guest_instructions,host_bytes,captured_bytes,"
            "wide_moves,address_chains,address_chain_instructions\n");
    for (const auto& r : results) {
      fprintf(csv, "%08X,%u,%u,%u,%u,%u,%u\n", r.address, r.guest_instructions,
              r.host_bytes, r.captured_host_bytes, r.wide_move_instructions,
              r.address_chains, r.address_chain_instructions);
    }
    fclose(csv);
    fprintf(stdout, "\n  wrote %s\n",
            xe::path_to_utf8(cvars::jit_corpus_csv).c_str());
  }

  // Fail closed. Failed, skipped and extent-mismatched functions are all
  // EXCLUDED from the totals above, so a candidate that stops compiling part of
  // the corpus reports a smaller instruction count and, until this check
  // existed, a successful run. `return !results.empty()` passed as long as one
  // function survived.
  //
  // Extents count as failures too: a function covering different guest
  // instructions than the capture is not the same function, so comparing its
  // emitted size to anything is meaningless.
  const bool clean = failed == 0 && skipped == 0 && extent_mismatched == 0;
  if (!clean) {
    fprintf(
        stdout,
        "\n  GATE FAILED: %u failed to compile, %u skipped, %u extent "
        "mismatches. All three are excluded from the totals above, so those "
        "totals describe less than the whole corpus and must not be compared "
        "against another run. Pass --jit_corpus_allow_incomplete to measure "
        "anyway.\n",
        failed, skipped, extent_mismatched);
  }
  fflush(stdout);
  if (results.empty()) {
    return false;
  }
  return clean || cvars::jit_corpus_allow_incomplete;
}

}  // namespace corpus

int main(const std::vector<std::string>& args) {
  std::vector<std::string> test_names;
  // Collect test names from all positional arguments.
  // argv[0] is the program name, skip it. Also skip --flag arguments
  // since those are handled by cvar parsing.
  for (size_t i = 1; i < args.size(); ++i) {
    if (!args[i].empty() && args[i][0] != '-') {
      test_names.push_back(args[i]);
    }
  }
  // Fall back to --test_name flag if no positional args given
  if (test_names.empty() && !cvars::test_name.empty()) {
    test_names.push_back(cvars::test_name);
  }
  if (!cvars::jit_corpus_in.empty()) {
    return corpus::RunCorpusReplay() ? 0 : 1;
  }

  return RunTests(test_names) ? 0 : 1;
}

}  // namespace test
}  // namespace cpu
}  // namespace xe

XE_DEFINE_CONSOLE_APP("xenia-cpu-ppc-test", xe::cpu::test::main,
                      "[test names...]", "test_name");
