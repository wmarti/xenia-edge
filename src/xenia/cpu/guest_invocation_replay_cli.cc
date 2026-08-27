/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_replay_cli.h"

#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <string_view>

#include "third_party/crypto/sha256.h"

namespace xe {
namespace cpu {
namespace {

bool Fail(GuestInvocationReplayFile* file, GuestInvocationReplaySha256* sha256,
          std::string* error, std::string_view message) {
  if (file) {
    *file = {};
  }
  if (sha256) {
    sha256->fill(0);
  }
  if (error) {
    error->assign(message);
  }
  return false;
}

GuestInvocationReplaySha256 FinalizeSha256(sha256::SHA256* hasher) {
  GuestInvocationReplaySha256 digest = {};
  static_assert(sizeof(unsigned char) == sizeof(uint8_t));
  hasher->getHash(reinterpret_cast<unsigned char*>(digest.data()));
  return digest;
}

void AppendField(std::string* marker, std::string_view name,
                 std::string_view value) {
  marker->push_back('\t');
  marker->append(name);
  marker->push_back('=');
  marker->append(value);
}

void AppendUint64Field(std::string* marker, std::string_view name,
                       uint64_t value) {
  std::array<char, std::numeric_limits<uint64_t>::digits10 + 1> digits = {};
  const auto result =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  AppendField(marker, name,
              std::string_view(digits.data(), static_cast<size_t>(
                                                  result.ptr - digits.data())));
}

}  // namespace

bool ReadGuestInvocationReplayFile(const std::filesystem::path& path,
                                   uint64_t maximum_size,
                                   GuestInvocationReplayFile* output,
                                   std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, nullptr, error, "replay file output is null");
  }
  *output = {};
  if (!maximum_size) {
    return Fail(output, nullptr, error, "replay file size limit is zero");
  }

  std::error_code file_size_error;
  const uintmax_t file_size = std::filesystem::file_size(path, file_size_error);
  if (file_size_error) {
    return Fail(output, nullptr, error, "replay file size is unavailable");
  }
  if (!file_size) {
    return Fail(output, nullptr, error, "replay file is empty");
  }
  if (file_size > maximum_size ||
      file_size > std::numeric_limits<size_t>::max() ||
      file_size >
          static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    return Fail(output, nullptr, error, "replay file exceeds its size limit");
  }

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return Fail(output, nullptr, error, "replay file could not be opened");
  }

  GuestInvocationReplayFile file;
  file.bytes.resize(static_cast<size_t>(file_size));
  stream.read(reinterpret_cast<char*>(file.bytes.data()),
              static_cast<std::streamsize>(file.bytes.size()));
  if (stream.gcount() != static_cast<std::streamsize>(file.bytes.size()) ||
      stream.bad()) {
    return Fail(output, nullptr, error,
                "replay file changed or could not be read completely");
  }
  char trailing_byte = 0;
  stream.read(&trailing_byte, 1);
  if (stream.gcount() || !stream.eof()) {
    return Fail(output, nullptr, error,
                "replay file changed while it was being read");
  }

  sha256::SHA256 hasher;
  hasher.add(file.bytes.data(), file.bytes.size());
  file.sha256 = FinalizeSha256(&hasher);
  *output = std::move(file);
  return true;
}

bool HashGuestInvocationReplayFile(const std::filesystem::path& path,
                                   GuestInvocationReplaySha256* output,
                                   std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(nullptr, nullptr, error, "replay file hash output is null");
  }
  output->fill(0);

  std::ifstream stream(path, std::ios::binary);
  if (!stream.is_open()) {
    return Fail(nullptr, output, error, "replay file could not be opened");
  }

  constexpr size_t kChunkSize = 64 * 1024;
  std::array<uint8_t, kChunkSize> chunk = {};
  sha256::SHA256 hasher;
  uint64_t byte_count = 0;
  while (stream) {
    stream.read(reinterpret_cast<char*>(chunk.data()), chunk.size());
    const std::streamsize count = stream.gcount();
    if (count > 0) {
      hasher.add(chunk.data(), static_cast<size_t>(count));
      byte_count += static_cast<uint64_t>(count);
    }
  }
  if (stream.bad() || !stream.eof()) {
    return Fail(nullptr, output, error,
                "replay file could not be read completely");
  }
  if (!byte_count) {
    return Fail(nullptr, output, error, "replay file is empty");
  }

  *output = FinalizeSha256(&hasher);
  return true;
}

std::string GuestInvocationReplaySha256Hex(
    const GuestInvocationReplaySha256& sha256) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string output;
  output.resize(sha256.size() * 2);
  for (size_t i = 0; i < sha256.size(); ++i) {
    output[i * 2] = kHex[sha256[i] >> 4];
    output[i * 2 + 1] = kHex[sha256[i] & 0xFu];
  }
  return output;
}

std::string FormatGuestInvocationReplayBenchmarkMarker(
    const GuestInvocationReplayBenchmarkProvenance& provenance,
    const GuestInvocationReplayMetrics& metrics) {
  std::string marker = "XENIA_GUEST_INVOCATION_BENCHMARK_V3";
  marker.reserve(800);
  AppendField(&marker, "artifact_sha256",
              GuestInvocationReplaySha256Hex(provenance.artifact_sha256));
  AppendField(&marker, "corpus_sha256",
              GuestInvocationReplaySha256Hex(provenance.corpus_sha256));
  AppendField(&marker, "capture_build_sha256",
              GuestInvocationReplaySha256Hex(provenance.capture_build_sha256));
  AppendField(
      &marker, "candidate_build_sha256",
      GuestInvocationReplaySha256Hex(provenance.candidate_build_sha256));
  AppendField(&marker, "config_sha256",
              GuestInvocationReplaySha256Hex(provenance.config_sha256));
  AppendField(&marker, "code_shape_sha256",
              GuestInvocationReplaySha256Hex(metrics.code_shape.sha256));
  AppendUint64Field(&marker, "code_shape_functions",
                    metrics.code_shape.function_count);
  AppendUint64Field(&marker, "host_instructions",
                    metrics.code_shape.host_instruction_count);
  AppendUint64Field(&marker, "wide_materialization_sites",
                    metrics.code_shape.wide_materialization_site_count);
  AppendUint64Field(&marker, "pc_relative_sites",
                    metrics.code_shape.pc_relative_site_count);
  AppendUint64Field(&marker, "iterations", metrics.timed_invocation_count);
  AppendUint64Field(&marker, "reset_pages",
                    metrics.reset_page_count_per_invocation);
  AppendUint64Field(&marker, "reset_bytes_per_iteration",
                    metrics.reset_bytes_per_invocation);
  AppendUint64Field(&marker, "thread_cpu_ns", metrics.thread_cpu_nanoseconds);
  AppendUint64Field(&marker, "uptime_raw_ns", metrics.uptime_raw_nanoseconds);
  AppendUint64Field(&marker, "reset_only_thread_cpu_ns",
                    metrics.reset_only_thread_cpu_nanoseconds);
  AppendUint64Field(&marker, "reset_only_uptime_raw_ns",
                    metrics.reset_only_uptime_raw_nanoseconds);
  AppendUint64Field(&marker, "placement_generation_before",
                    metrics.placement_generation_before);
  AppendUint64Field(&marker, "placement_generation_after",
                    metrics.placement_generation_after);
  AppendField(&marker, "warm_verified", "1");
  AppendField(&marker, "timed_exit_verified", "1");
  AppendField(&marker, "final_verified", "1");
  return marker;
}

}  // namespace cpu
}  // namespace xe
