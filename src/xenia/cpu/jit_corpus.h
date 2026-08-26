/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_JIT_CORPUS_H_
#define XENIA_CPU_JIT_CORPUS_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>

namespace xe {
class Memory;
}  // namespace xe

namespace xe {
namespace cpu {

// A snapshot of the guest code a live run actually handed to the JIT: the code
// pages, the entry addresses, and the host code size each function produced.
//
// The point is to make a codegen change measurable against real guest code
// without booting a title. Compiling needs no kernel, no GPU and no thread
// state -- Processor takes a bare Memory and a null export resolver, and
// RawModule will hold arbitrary guest bytes -- so a corpus plus the offline
// replay in xenia-cpu-ppc-tests reproduces the compile side of a run exactly.
//
// host_code_size is recorded so a replay can be *validated*: replaying a corpus
// on the same binary that captured it must reproduce every function's size
// byte-for-byte. Any mismatch means the offline environment is not faithful and
// its totals cannot be trusted.
//
// The file is written incrementally as functions are defined, one record at a
// time, because capture runs end with SIGKILL far more often than they end
// cleanly. A truncated file is still a usable corpus: the reader stops at the
// last complete record.
class JitCorpus {
 public:
  // 'XJC1' -- bump kVersion on any layout change.
  static constexpr uint32_t kMagic = 0x3143584Au;
  static constexpr uint32_t kVersion = 2;
  static constexpr uint32_t kPageSize = 0x1000u;

  // Codegen-affecting settings latched at capture, carried in the header's
  // fourth word. A replay that does not reproduce these is not comparing the
  // same compiler: --guest_scheduler gates PreemptCheckInjectionPass, and a
  // preempt check plus its out-of-line handler is 11 a64 instructions at every
  // injection point. Replaying a --guest_scheduler=false capture under the
  // default (true) made 13,323 of 13,564 functions differ from their own
  // capture and inflated the total by 3.50%.
  static constexpr uint32_t kConfigGuestScheduler = 1u << 0;

  // A guest function that reached the backend, and what it emitted.
  struct FunctionRecord {
    uint32_t address;
    uint32_t end_address;
    // Host bytes emitted during capture. 0 when the capturing backend did not
    // report a size, in which case the replay cannot validate this function.
    uint32_t host_code_size;
    uint32_t flags;

    bool captured_host_bytes_valid() const { return host_code_size != 0; }
  };

  enum RecordTag : uint32_t {
    kTagPage = 1,
    kTagFunction = 2,
  };

  JitCorpus() = default;

  // Reads a corpus, tolerating truncation at any record boundary. Returns
  // nullptr only when the file is missing or its header is not a corpus.
  static std::unique_ptr<JitCorpus> Read(const std::filesystem::path& path);

  const std::vector<FunctionRecord>& functions() const { return functions_; }

  // Sorted page addresses, and their contents as one blob of kPageSize chunks
  // indexed identically to page_addresses().
  const std::vector<uint32_t>& page_addresses() const {
    return page_addresses_;
  }
  const std::vector<uint8_t>& page_data() const { return page_data_; }

  // True when the file ended mid-record, i.e. capture was killed.
  bool truncated() const { return truncated_; }

  // The codegen settings this corpus was captured under. Only v2 corpora
  // recorded them; a v1 capture is readable but its configuration is
  // unknown, and a replay must not pretend otherwise.
  bool config_known() const { return version_ >= 2; }
  uint32_t config_flags() const { return config_flags_; }
  bool captured_with_guest_scheduler() const {
    return (config_flags_ & kConfigGuestScheduler) != 0;
  }

 private:
  std::vector<FunctionRecord> functions_;
  std::vector<uint32_t> page_addresses_;
  std::vector<uint8_t> page_data_;
  uint32_t config_flags_ = 0;
  uint32_t version_ = 0;
  bool truncated_ = false;
};

// Streams a corpus to disk as functions are defined. Thread-safe: guest threads
// compile concurrently.
class JitCorpusWriter {
 public:
  ~JitCorpusWriter();

  // Returns nullptr if the file cannot be created.
  static std::unique_ptr<JitCorpusWriter> Create(
      const std::filesystem::path& path);

  // Records one compiled function plus any of its code pages not yet written.
  // Pages that are not readable guest memory are skipped rather than faulted
  // on, so a partially mapped range degrades to a function the replay reports
  // as failed instead of crashing the capture.
  void RecordFunction(Memory* memory, uint32_t address, uint32_t end_address,
                      uint32_t host_code_size);

  uint32_t function_count() const { return function_count_; }
  uint32_t page_count() const { return page_count_; }

 private:
  JitCorpusWriter() = default;

  // Caller must hold mutex_.
  void WriteFunctionRecord(uint32_t address, uint32_t end_address,
                           uint32_t host_code_size);

  std::mutex mutex_;
  FILE* file_ = nullptr;
  std::unordered_set<uint32_t> written_pages_;
  uint32_t function_count_ = 0;
  uint32_t page_count_ = 0;
};

}  // namespace cpu
}  // namespace xe

#endif  // XENIA_CPU_JIT_CORPUS_H_
