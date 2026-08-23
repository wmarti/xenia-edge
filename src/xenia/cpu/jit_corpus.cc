/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/jit_corpus.h"

#include <algorithm>
#include <cstdio>

#include "xenia/base/filesystem.h"
#include "xenia/base/logging.h"
#include "xenia/memory.h"

namespace xe {
namespace cpu {

namespace {

// Safety of reading a page is established by the caller, not by asking the
// heap.
//
// BaseHeap::QueryProtect is NOT a usable predicate here: on a real title most
// guest code pages report protect=0 while the frontend is reading them every
// time it scans a function (measured on Halo 3: 6063 rejections across 912
// distinct pages, all protect=00000000, interleaved with pages that reported
// read access). Xenia's page table simply does not record a protect for pages
// it did not commit itself, even though the host mapping behind them is
// readable.
//
// What IS sound: RecordFunction is only ever called for a function the frontend
// already scanned and translated, and the scanner reads every word in
// [address, end_address] via TranslateVirtual to decide where the function
// ends. So that exact range has provably been read without faulting, and
// copying it is no more dangerous than the scan that just happened. Nothing
// outside that range is touched -- no read-ahead margin -- because a page past
// the function end may never have been read by anyone.
bool HasGuestHeap(Memory* memory, uint32_t page_address) {
  return memory->LookupHeap(page_address) != nullptr;
}

}  // namespace

std::unique_ptr<JitCorpusWriter> JitCorpusWriter::Create(
    const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "wb");
  if (!file) {
    XELOGE("JitCorpusWriter: could not open {} for writing",
           xe::path_to_utf8(path));
    return nullptr;
  }
  const uint32_t header[4] = {JitCorpus::kMagic, JitCorpus::kVersion,
                              JitCorpus::kPageSize, 0};
  if (fwrite(header, sizeof(header), 1, file) != 1) {
    fclose(file);
    return nullptr;
  }
  auto writer = std::unique_ptr<JitCorpusWriter>(new JitCorpusWriter());
  writer->file_ = file;
  return writer;
}

JitCorpusWriter::~JitCorpusWriter() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (file_) {
    fflush(file_);
    fclose(file_);
    file_ = nullptr;
  }
}

void JitCorpusWriter::RecordFunction(Memory* memory, uint32_t address,
                                     uint32_t end_address,
                                     uint32_t host_code_size) {
  if (!address) {
    return;
  }
  // end_address is inclusive (ppc_scanner sets it to the address of the last
  // instruction it read). A function whose end is missing, or below its start,
  // has no trustworthy extent: record it so the replay can report it, but do
  // not guess which pages to copy. end < start really happens -- the scanner
  // emits start-4 when the first word it fetches is zero.
  if (!end_address || end_address < address) {
    std::lock_guard<std::mutex> lock(mutex_);
    WriteFunctionRecord(address, end_address, host_code_size);
    return;
  }
  const uint32_t last = end_address + 4;

  std::lock_guard<std::mutex> lock(mutex_);
  if (!file_) {
    return;
  }
  for (uint32_t page = address & ~(JitCorpus::kPageSize - 1); page < last;
       page += JitCorpus::kPageSize) {
    if (written_pages_.count(page)) {
      continue;
    }
    if (!HasGuestHeap(memory, page)) {
      XELOGW("JitCorpus: no guest heap for page {:08X}", page);
      continue;
    }
    // Mark written even on a short write: retrying a failed page on the next
    // function would just fail again and stall every compiling thread.
    written_pages_.insert(page);
    const uint32_t tag = JitCorpus::kTagPage;
    const uint8_t* data = memory->TranslateVirtual(page);
    if (fwrite(&tag, sizeof(tag), 1, file_) != 1 ||
        fwrite(&page, sizeof(page), 1, file_) != 1 ||
        fwrite(data, JitCorpus::kPageSize, 1, file_) != 1) {
      return;
    }
    ++page_count_;
  }

  WriteFunctionRecord(address, end_address, host_code_size);
}

void JitCorpusWriter::WriteFunctionRecord(uint32_t address,
                                          uint32_t end_address,
                                          uint32_t host_code_size) {
  if (!file_) {
    return;
  }
  const uint32_t tag = JitCorpus::kTagFunction;
  const uint32_t record[4] = {address, end_address, host_code_size, 0};
  if (fwrite(&tag, sizeof(tag), 1, file_) != 1 ||
      fwrite(record, sizeof(record), 1, file_) != 1) {
    return;
  }
  ++function_count_;
  // Capture runs are normally ended with SIGKILL, so durability per record is
  // the whole point of streaming this file.
  fflush(file_);
}

std::unique_ptr<JitCorpus> JitCorpus::Read(const std::filesystem::path& path) {
  FILE* file = xe::filesystem::OpenFile(path, "rb");
  if (!file) {
    return nullptr;
  }
  uint32_t header[4] = {};
  if (fread(header, sizeof(header), 1, file) != 1) {
    fclose(file);
    return nullptr;
  }
  if (header[0] != kMagic || header[1] != kVersion || header[2] != kPageSize) {
    XELOGE("JitCorpus: {} is not a v{} corpus with {}-byte pages",
           xe::path_to_utf8(path), kVersion, kPageSize);
    fclose(file);
    return nullptr;
  }

  auto corpus = std::make_unique<JitCorpus>();
  // Pages arrive in first-touch order; collect them with their data and sort at
  // the end so the replay can map contiguous runs in one allocation.
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> pages;
  for (;;) {
    uint32_t tag = 0;
    if (fread(&tag, sizeof(tag), 1, file) != 1) {
      break;  // Clean EOF at a record boundary.
    }
    if (tag == kTagPage) {
      uint32_t page = 0;
      std::vector<uint8_t> data(kPageSize);
      if (fread(&page, sizeof(page), 1, file) != 1 ||
          fread(data.data(), kPageSize, 1, file) != 1) {
        corpus->truncated_ = true;
        break;
      }
      pages.emplace_back(page, std::move(data));
    } else if (tag == kTagFunction) {
      uint32_t record[4] = {};
      if (fread(record, sizeof(record), 1, file) != 1) {
        corpus->truncated_ = true;
        break;
      }
      corpus->functions_.push_back(
          {record[0], record[1], record[2], record[3]});
    } else {
      // Unknown tag: the stream is desynchronized and nothing after it can be
      // trusted, so stop here rather than guess a length.
      corpus->truncated_ = true;
      break;
    }
  }
  fclose(file);

  std::sort(pages.begin(), pages.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  corpus->page_addresses_.reserve(pages.size());
  corpus->page_data_.reserve(pages.size() * kPageSize);
  for (auto& entry : pages) {
    corpus->page_addresses_.push_back(entry.first);
    corpus->page_data_.insert(corpus->page_data_.end(), entry.second.begin(),
                              entry.second.end());
  }
  return corpus;
}

}  // namespace cpu
}  // namespace xe
