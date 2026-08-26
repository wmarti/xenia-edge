/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/execution_jit_corpus.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <string_view>
#include <utility>

namespace xe {
namespace cpu {

namespace {

constexpr uint32_t kKnownConfigFlags = JitCorpus::kConfigGuestScheduler;
constexpr uint32_t kSupportedVersion = 3;
constexpr uint32_t kSupportedPageSize = 4096;
constexpr size_t kHeaderSize = 4 * sizeof(uint32_t);

static_assert(JitCorpus::kVersion == kSupportedVersion,
              "review the execution decoder before accepting a new format");
static_assert(JitCorpus::kPageSize == kSupportedPageSize,
              "execution replay requires canonical 4 KiB corpus pages");

struct PageRecord {
  uint32_t address = 0;
  std::array<uint8_t, JitCorpus::kPageSize> data = {};
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - offset_; }

  bool ReadU32(uint32_t* value) {
    uint8_t bytes[4];
    if (!ReadBytes(bytes, sizeof(bytes))) {
      return false;
    }
    *value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
             (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return true;
  }

  bool ReadBytes(uint8_t* output, size_t size) {
    if (size > remaining()) {
      return false;
    }
    std::memcpy(output, data_ + offset_, size);
    offset_ += size;
    return true;
  }

 private:
  const uint8_t* data_;
  size_t size_;
  size_t offset_ = 0;
};

bool Fail(ExecutionJitCorpus* output, std::string* error,
          std::string_view message) {
  if (output) {
    *output = {};
  }
  if (error) {
    error->assign(message);
  }
  return false;
}

bool IsSupportedCodePageAddress(uint32_t address) {
  // Replay code must be in the XEX range backed by CodeCacheBase's indirection
  // table. Both host backends reserve [0x80000000, 0x80040000) for guest and
  // hypervisor trampoline slots. This is a code-corpus/runtime-reservation
  // constraint, not an artifact data-page constraint. The table's
  // exclusive-end check also makes 0x9FFFF000 the first 4 KiB XEX page that
  // cannot be fully committed.
  return address >= 0x80040000u && address <= 0x9FFFE000u;
}

bool ValidateFunctionRecord(const JitCorpus::FunctionRecord& function,
                            std::string* error) {
  if (function.flags & ~JitCorpus::kKnownFunctionFlags) {
    if (error) {
      error->assign("function record contains unsupported flags");
    }
    return false;
  }
  JitCorpus::FunctionMetadata metadata;
  if (!JitCorpus::DecodeFunctionFlags(function.flags, &metadata)) {
    if (error) {
      error->assign("function record contains invalid metadata");
    }
    return false;
  }
  if (!function.address || (function.address & 3) ||
      (function.end_address & 3) || function.end_address < function.address) {
    if (error) {
      error->assign("function record contains an invalid extent");
    }
    return false;
  }
  // The scanner and backend use an exclusive end at several boundaries.
  // Reject an inclusive end whose +4 would wrap instead of relying on a
  // uint64_t value that those consumers cannot represent.
  if (function.end_address >
      std::numeric_limits<uint32_t>::max() - sizeof(uint32_t)) {
    if (error) {
      error->assign("function record extent overflows the guest address space");
    }
    return false;
  }
  const uint64_t function_size =
      uint64_t(function.end_address) - function.address + sizeof(uint32_t);
  if (function_size > ExecutionJitCorpus::kMaxFunctionSize) {
    if (error) {
      error->assign("function record extent exceeds the size limit");
    }
    return false;
  }
  const uint32_t first_page = function.address & ~(JitCorpus::kPageSize - 1);
  const uint32_t last_page = function.end_address & ~(JitCorpus::kPageSize - 1);
  if (!IsSupportedCodePageAddress(first_page) ||
      !IsSupportedCodePageAddress(last_page)) {
    if (error) {
      error->assign(
          "function record is outside the supported XEX executable range");
    }
    return false;
  }
  return true;
}

}  // namespace

bool ExecutionJitCorpus::Decode(const uint8_t* data, size_t data_size,
                                ExecutionJitCorpus* output,
                                std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, error, "output corpus is null");
  }
  *output = {};
  if (data_size > kMaxCorpusSize) {
    return Fail(output, error, "corpus exceeds the byte-size limit");
  }
  if (!data && data_size) {
    return Fail(output, error, "input data is null");
  }
  if (data_size < kHeaderSize) {
    return Fail(output, error, "corpus header is truncated");
  }

  Reader reader(data, data_size);
  uint32_t magic = 0;
  uint32_t version = 0;
  uint32_t page_size = 0;
  uint32_t config_flags = 0;
  if (!reader.ReadU32(&magic) || !reader.ReadU32(&version) ||
      !reader.ReadU32(&page_size) || !reader.ReadU32(&config_flags)) {
    return Fail(output, error, "corpus header is truncated");
  }
  if (magic != JitCorpus::kMagic) {
    return Fail(output, error, "corpus magic is invalid");
  }
  if (version != kSupportedVersion) {
    return Fail(output, error, "corpus version is unsupported for execution");
  }
  if (page_size != kSupportedPageSize) {
    return Fail(output, error, "corpus page size is unsupported");
  }
  if (config_flags & ~kKnownConfigFlags) {
    return Fail(output, error,
                "corpus configuration contains unsupported flags");
  }

  std::vector<PageRecord> pages;
  std::vector<FunctionRecord> functions;
  while (reader.remaining()) {
    if (reader.remaining() < sizeof(uint32_t)) {
      return Fail(output, error, "corpus has trailing bytes");
    }

    uint32_t tag = 0;
    reader.ReadU32(&tag);
    if (tag == JitCorpus::kTagPage) {
      PageRecord page;
      if (!reader.ReadU32(&page.address) ||
          !reader.ReadBytes(page.data.data(), page.data.size())) {
        return Fail(output, error, "corpus page record is truncated");
      }
      if (page.address & (JitCorpus::kPageSize - 1)) {
        return Fail(output, error, "corpus contains an unaligned code page");
      }
      if (!IsSupportedCodePageAddress(page.address)) {
        return Fail(output, error,
                    "corpus contains a code page outside the supported XEX "
                    "executable range");
      }
      if (pages.size() >= kMaxPageRecords) {
        return Fail(output, error, "corpus exceeds the code-page limit");
      }
      pages.push_back(std::move(page));
    } else if (tag == JitCorpus::kTagFunction) {
      FunctionRecord function = {};
      if (!reader.ReadU32(&function.address) ||
          !reader.ReadU32(&function.end_address) ||
          !reader.ReadU32(&function.host_code_size) ||
          !reader.ReadU32(&function.flags)) {
        return Fail(output, error, "corpus function record is truncated");
      }
      std::string validation_error;
      if (!ValidateFunctionRecord(function, &validation_error)) {
        return Fail(output, error, validation_error);
      }
      if (functions.size() >= kMaxFunctionRecords) {
        return Fail(output, error, "corpus exceeds the function-count limit");
      }
      functions.push_back(function);
    } else {
      return Fail(output, error, "corpus contains an unsupported record tag");
    }
  }

  if (pages.empty()) {
    return Fail(output, error, "corpus contains no code pages");
  }
  if (functions.empty()) {
    return Fail(output, error, "corpus contains no functions");
  }

  std::sort(pages.begin(), pages.end(),
            [](const PageRecord& left, const PageRecord& right) {
              return left.address < right.address;
            });
  for (size_t i = 1; i < pages.size(); ++i) {
    if (pages[i - 1].address == pages[i].address) {
      return Fail(output, error, "corpus contains duplicate code pages");
    }
  }

  std::vector<uint32_t> function_definition_order;
  function_definition_order.reserve(functions.size());
  for (const FunctionRecord& function : functions) {
    function_definition_order.push_back(function.address);
  }

  std::sort(functions.begin(), functions.end(),
            [](const FunctionRecord& left, const FunctionRecord& right) {
              return left.address < right.address;
            });
  for (size_t i = 1; i < functions.size(); ++i) {
    if (functions[i - 1].address == functions[i].address) {
      return Fail(output, error, "corpus contains duplicate function entries");
    }
  }

  // Require a complete page set for every inclusive function extent. Extra
  // pages are permitted because replay may need every 4 KiB page in a larger
  // host protection granule, including pages with no recorded function.
  for (const FunctionRecord& function : functions) {
    const uint32_t first_page = function.address & ~(JitCorpus::kPageSize - 1);
    const uint32_t last_page =
        function.end_address & ~(JitCorpus::kPageSize - 1);
    const auto find_page = [&pages](uint32_t address) {
      return std::lower_bound(
          pages.cbegin(), pages.cend(), address,
          [](const PageRecord& page, uint32_t candidate_address) {
            return page.address < candidate_address;
          });
    };
    const auto first_page_it = find_page(first_page);
    const auto last_page_it = find_page(last_page);
    const size_t expected_page_count =
        (uint64_t(last_page) - first_page) / JitCorpus::kPageSize + 1;
    if (first_page_it == pages.cend() || first_page_it->address != first_page ||
        last_page_it == pages.cend() || last_page_it->address != last_page ||
        static_cast<size_t>(last_page_it - first_page_it) + 1 !=
            expected_page_count) {
      return Fail(output, error,
                  "corpus is missing a page in a function extent");
    }
  }

  ExecutionJitCorpus decoded;
  decoded.version_ = version;
  decoded.config_flags_ = config_flags;
  decoded.functions_ = std::move(functions);
  decoded.function_definition_order_ = std::move(function_definition_order);
  decoded.page_addresses_.reserve(pages.size());
  decoded.page_data_.reserve(pages.size() * JitCorpus::kPageSize);
  for (const PageRecord& page : pages) {
    decoded.page_addresses_.push_back(page.address);
    decoded.page_data_.insert(decoded.page_data_.end(), page.data.cbegin(),
                              page.data.cend());
  }
  *output = std::move(decoded);
  return true;
}

bool ExecutionJitCorpus::Read(const std::filesystem::path& path,
                              ExecutionJitCorpus* output, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, error, "output corpus is null");
  }
  *output = {};

  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file) {
    return Fail(output, error, "corpus file is missing, empty, or unreadable");
  }
  const std::streamoff file_size = file.tellg();
  if (file_size < 0) {
    return Fail(output, error, "corpus file size could not be read");
  }
  if (!file_size) {
    return Fail(output, error, "corpus file is missing, empty, or unreadable");
  }
  if (static_cast<uint64_t>(file_size) > kMaxCorpusSize) {
    return Fail(output, error, "corpus exceeds the byte-size limit");
  }

  std::vector<uint8_t> data(static_cast<size_t>(file_size));
  file.seekg(0, std::ios::beg);
  if ((!data.empty() &&
       !file.read(reinterpret_cast<char*>(data.data()),
                  static_cast<std::streamsize>(data.size()))) ||
      file.peek() != std::char_traits<char>::eof()) {
    return Fail(output, error, "corpus file changed or could not be read");
  }
  return Decode(data, output, error);
}

const uint8_t* ExecutionJitCorpus::FindPageData(uint32_t page_address) const {
  const auto it = std::lower_bound(page_addresses_.cbegin(),
                                   page_addresses_.cend(), page_address);
  if (it == page_addresses_.cend() || *it != page_address) {
    return nullptr;
  }
  const size_t index = static_cast<size_t>(it - page_addresses_.cbegin());
  return page_data_.data() + index * JitCorpus::kPageSize;
}

const ExecutionJitCorpus::FunctionRecord* ExecutionJitCorpus::FindFunction(
    uint32_t entry_address) const {
  const auto it =
      std::lower_bound(functions_.cbegin(), functions_.cend(), entry_address,
                       [](const FunctionRecord& function, uint32_t address) {
                         return function.address < address;
                       });
  return it != functions_.cend() && it->address == entry_address ? &*it
                                                                 : nullptr;
}

}  // namespace cpu
}  // namespace xe
