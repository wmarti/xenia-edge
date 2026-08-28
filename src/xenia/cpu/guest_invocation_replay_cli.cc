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
#include <utility>

#include "third_party/crypto/sha256.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_session.h"
#include "xenia/cpu/guest_execution_session_runner.h"
#include "xenia/cpu/jit_corpus.h"

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

std::string Uint64Text(uint64_t value) {
  std::array<char, std::numeric_limits<uint64_t>::digits10 + 2> digits = {};
  const auto result =
      std::to_chars(digits.data(), digits.data() + digits.size(), value);
  return std::string(digits.data(),
                     static_cast<size_t>(result.ptr - digits.data()));
}

// The record is one line of space-separated fields, so a quote, backslash or
// control byte from a decoder message must not be able to end it early.
std::string QuoteVerdictReason(std::string_view reason) {
  std::string quoted;
  quoted.reserve(reason.size() + 8);
  for (const char raw : reason) {
    const auto byte = static_cast<unsigned char>(raw);
    if (raw == '"' || raw == '\\') {
      quoted.push_back('\\');
      quoted.push_back(raw);
    } else if (byte < 0x20 || byte == 0x7F) {
      quoted.push_back(' ');
    } else {
      quoted.push_back(raw);
    }
  }
  return quoted;
}

bool RejectContinuousSession(GuestSessionContinuousReplayVerdict* output,
                             std::string_view reason) {
  output->planned = false;
  output->plan_line = FormatGuestSessionContinuousPlanRejection(reason);
  output->exec_line.clear();
  return false;
}

const std::vector<uint8_t>* FindContentBlob(
    const GuestExecutionSessionBundle& bundle,
    const GuestExecutionSessionSha256& sha256) {
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    if (blob.sha256 == sha256) {
      return &blob.bytes;
    }
  }
  return nullptr;
}

void AppendUint64Field(std::string* marker, std::string_view name,
                       uint64_t value) {
  AppendField(marker, name, Uint64Text(value));
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

std::string FormatGuestSessionContinuousPlanRejection(std::string_view reason) {
  // An empty reason would leave an unfalsifiable record, so name the gap.
  constexpr std::string_view kUnnamedReason =
      "continuous replay rejected without a reason";
  std::string line = kGuestSessionContinuousPlanMarker;
  line += " status=rejected reason=\"";
  line += QuoteVerdictReason(reason.empty() ? kUnnamedReason : reason);
  line += '"';
  return line;
}

bool AttemptGuestSessionContinuousReplayPlan(
    const GuestExecutionSessionBundle& bundle, uint32_t host_page_size,
    bool runtime_guest_scheduler,
    const GuestInvocationReplaySha256& runtime_replay_config_sha256,
    GuestSessionContinuousReplayVerdict* output) {
  if (!output) {
    return false;
  }
  *output = {};

  const GuestExecutionSessionManifest& manifest = bundle.manifest;
  if (bundle.chunks.size() != manifest.chunks.size()) {
    return RejectContinuousSession(
        output, "continuous session chunk count does not match its manifest");
  }

  size_t corpus_chunk_index = manifest.chunks.size();
  for (size_t i = 0; i < manifest.chunks.size(); ++i) {
    if (manifest.chunks[i].kind !=
        GuestExecutionSessionChunkKind::kCodeCorpus) {
      continue;
    }
    if (corpus_chunk_index != manifest.chunks.size()) {
      return RejectContinuousSession(
          output, "continuous session code corpus chunk is duplicated");
    }
    corpus_chunk_index = i;
  }
  std::string error;
  // guest_scheduler is already inside the configuration hash, but it is named
  // separately because a corpus built without safepoint polls would otherwise
  // run to the tape timeout instead of rejecting. A session with no corpus
  // chunk cannot plan at all, so the planner's own closure rejection speaks.
  if (corpus_chunk_index != manifest.chunks.size()) {
    GuestExecutionSessionCodeCorpusChunk corpus_chunk;
    if (!GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
            bundle.chunks[corpus_chunk_index], &corpus_chunk, &error)) {
      return RejectContinuousSession(output, error);
    }
    const std::vector<uint8_t>* corpus_bytes =
        FindContentBlob(bundle, corpus_chunk.code_corpus_sha256);
    ExecutionJitCorpus corpus;
    if (!corpus_bytes ||
        !ExecutionJitCorpus::Decode(*corpus_bytes, &corpus, &error)) {
      return RejectContinuousSession(
          output, "continuous session code corpus failed to decode");
    }
    const bool corpus_guest_scheduler =
        (corpus.config_flags() & JitCorpus::kConfigGuestScheduler) != 0;
    if (corpus_guest_scheduler != runtime_guest_scheduler) {
      return RejectContinuousSession(
          output,
          "continuous replay corpus guest_scheduler does not match the "
          "runtime");
    }
  }

  if (manifest.replay_config_sha256 != runtime_replay_config_sha256) {
    return RejectContinuousSession(
        output,
        "continuous replay configuration SHA-256 does not match the session");
  }

  GuestExecutionContinuousReplayPlan plan;
  if (!BuildGuestExecutionContinuousReplayPlan(bundle, host_page_size, &plan,
                                               &error)) {
    return RejectContinuousSession(output, error);
  }

  std::string plan_line = kGuestSessionContinuousPlanMarker;
  plan_line += " status=planned participants=";
  plan_line += Uint64Text(plan.participants.size());
  plan_line += " events=";
  plan_line += Uint64Text(plan.events.size());
  plan_line += " pages=";
  plan_line += Uint64Text(plan.pages.size());
  plan_line += " resume_entries=";
  plan_line += Uint64Text(plan.resume_entries.size());

  std::string exec_line = kGuestSessionContinuousExecMarker;
  exec_line +=
      " status=rejected reason=\"continuous executor is not "
      "implemented\"";

  output->planned = true;
  output->plan_line = std::move(plan_line);
  output->exec_line = std::move(exec_line);
  return true;
}

bool AttemptGuestSessionContinuousReplay(
    const std::filesystem::path& bundle_directory, uint32_t host_page_size,
    bool runtime_guest_scheduler,
    const GuestInvocationReplaySha256& runtime_replay_config_sha256,
    GuestSessionContinuousReplayVerdict* output) {
  if (!output) {
    return false;
  }
  *output = {};
  if (bundle_directory.empty()) {
    return RejectContinuousSession(output,
                                   "continuous session bundle path is empty");
  }
  GuestExecutionSessionBundle bundle;
  std::string error;
  if (!ReadGuestExecutionSessionBundle(bundle_directory, &bundle, &error)) {
    return RejectContinuousSession(output, error);
  }
  return AttemptGuestSessionContinuousReplayPlan(
      bundle, host_page_size, runtime_guest_scheduler,
      runtime_replay_config_sha256, output);
}

}  // namespace cpu
}  // namespace xe
