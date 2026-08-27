/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_execution_session_bundle.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe {
namespace cpu {
namespace test {

namespace {

class ScopedTestDirectory {
 public:
  ScopedTestDirectory() {
    std::error_code filesystem_error;
    const std::filesystem::path temporary_root =
        std::filesystem::temp_directory_path(filesystem_error);
    if (filesystem_error) {
      throw std::runtime_error("temporary directory is unavailable");
    }
    const uint64_t nonce = static_cast<uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (uint32_t attempt = 0; attempt < 100; ++attempt) {
      path_ = temporary_root /
              ("xenia-session-bundle-test-" + std::to_string(nonce) + "-" +
               std::to_string(attempt));
      filesystem_error.clear();
      if (std::filesystem::create_directory(path_, filesystem_error)) {
        return;
      }
      if (filesystem_error) {
        throw std::runtime_error("temporary directory could not be created");
      }
    }
    throw std::runtime_error("unique temporary directory could not be created");
  }

  ~ScopedTestDirectory() {
    std::error_code filesystem_error;
    std::filesystem::remove_all(path_, filesystem_error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

std::vector<uint8_t> Bytes(size_t size, uint8_t seed) {
  std::vector<uint8_t> bytes(size);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<uint8_t>(seed + i * 17);
  }
  return bytes;
}

GuestExecutionSessionSha256 IdentityDigest(uint8_t seed) {
  GuestExecutionSessionSha256 digest = {};
  for (size_t i = 0; i < digest.size(); ++i) {
    digest[i] = static_cast<uint8_t>(seed + i);
  }
  return digest;
}

GuestExecutionSessionSha256 AddBlob(GuestExecutionSessionBundle* bundle,
                                    std::vector<uint8_t> bytes) {
  GuestExecutionSessionContentBlob blob;
  blob.sha256 = GuestExecutionSessionCodec::HashBytes(bytes);
  blob.bytes = std::move(bytes);
  const GuestExecutionSessionSha256 digest = blob.sha256;
  bundle->content_blobs.push_back(std::move(blob));
  return digest;
}

GuestExecutionSessionChunkReference ReferenceFor(
    GuestExecutionSessionChunkKind kind, uint32_t ordinal,
    uint64_t first_sequence, uint64_t last_sequence, uint32_t record_count,
    const std::vector<uint8_t>& encoded) {
  GuestExecutionSessionChunkReference reference;
  reference.kind = kind;
  reference.ordinal = ordinal;
  reference.first_event_sequence = first_sequence;
  reference.last_event_sequence = last_sequence;
  reference.record_count = record_count;
  reference.encoded_size = encoded.size();
  reference.encoded_sha256 = GuestExecutionSessionCodec::HashBytes(encoded);
  return reference;
}

GuestExecutionSessionBundle MakeBundle() {
  GuestExecutionSessionBundle bundle;
  const GuestExecutionSessionSha256 initial_state =
      AddBlob(&bundle, Bytes(64, 1));
  const GuestExecutionSessionSha256 final_state =
      AddBlob(&bundle, Bytes(64, 2));
  const GuestExecutionSessionSha256 page = AddBlob(&bundle, Bytes(4096, 3));
  const GuestExecutionSessionSha256 code = AddBlob(&bundle, Bytes(256, 4));
  const GuestExecutionSessionSha256 scalar = AddBlob(&bundle, Bytes(8, 5));
  const GuestExecutionSessionSha256 mutation = AddBlob(&bundle, Bytes(4, 6));
  const GuestExecutionSessionSha256 corpus = AddBlob(&bundle, Bytes(128, 7));
  const GuestExecutionSessionSha256 segment_blob =
      AddBlob(&bundle, Bytes(96, 8));

  constexpr uint64_t kEpoch = 0x123456789ABCDEF0ull;
  GuestExecutionSessionCheckpointChunk initial;
  initial.session_epoch = kEpoch;
  initial.ordinal = 0;
  initial.checkpoint.global_sequence = 0;
  initial.checkpoint.thread_states.push_back({0, 64, initial_state});
  initial.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestPage, 0x1000, 4096, page});
  initial.checkpoint.content.push_back(
      {GuestExecutionSessionContentKind::kGuestCode, 0x4000, 256, code});

  GuestExecutionSessionEventChunk events;
  events.session_epoch = kEpoch;
  events.ordinal = 1;
  GuestExecutionSessionEvent begin;
  begin.global_sequence = 1;
  begin.thread_ordinal = 0;
  begin.kind = GuestExecutionSessionEventKind::kSegmentBegin;
  begin.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  events.events.push_back(begin);
  GuestExecutionSessionEvent external;
  external.global_sequence = 2;
  external.thread_ordinal = 0;
  external.kind = GuestExecutionSessionEventKind::kKernelExport;
  external.payload_kind =
      GuestExecutionSessionPayloadKind::kLittleEndianUnsignedInteger;
  external.payload_size = 8;
  external.payload_sha256 = scalar;
  events.events.push_back(external);
  GuestExecutionSessionEvent write;
  write.global_sequence = 3;
  write.kind = GuestExecutionSessionEventKind::kMemoryMutation;
  write.mutation_source = GuestExecutionSessionMutationSource::kGpu;
  write.payload_kind = GuestExecutionSessionPayloadKind::kGuestBytes;
  write.guest_address = 0x2000;
  write.byte_count = 4;
  write.payload_size = 4;
  write.payload_sha256 = mutation;
  events.events.push_back(write);
  GuestExecutionSessionEvent end;
  end.global_sequence = 4;
  end.thread_ordinal = 0;
  end.kind = GuestExecutionSessionEventKind::kSegmentEnd;
  end.disposition =
      GuestExecutionSessionEventDisposition::kValidateDeterministic;
  events.events.push_back(end);
  GuestExecutionSessionEvent request;
  request.global_sequence = 5;
  request.kind = GuestExecutionSessionEventKind::kBoundaryRequest;
  events.events.push_back(request);
  GuestExecutionSessionEvent held;
  held.global_sequence = 6;
  held.kind = GuestExecutionSessionEventKind::kBoundaryHeld;
  events.events.push_back(held);

  GuestExecutionSessionCheckpointChunk final;
  final.session_epoch = kEpoch;
  final.ordinal = 2;
  final.checkpoint.global_sequence = 6;
  final.checkpoint.thread_states.push_back({0, 64, final_state});
  final.checkpoint.content = initial.checkpoint.content;

  std::string error;
  bundle.chunks.resize(3);
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      initial, &bundle.chunks[0], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeEventChunk(
      events, &bundle.chunks[1], &error));
  REQUIRE(GuestExecutionSessionCodec::EncodeCheckpointChunk(
      final, &bundle.chunks[2], &error));

  GuestExecutionSessionManifest& manifest = bundle.manifest;
  manifest.session_epoch = kEpoch;
  manifest.boundary.kind = GuestExecutionSessionBoundaryKind::kSegmentCount;
  manifest.boundary.value = 1;
  manifest.first_event_sequence = 1;
  manifest.last_event_sequence = 6;
  manifest.capture_start_tick = 100;
  manifest.capture_end_tick = 900;
  manifest.capture_tick_frequency = 1000000000;
  manifest.capture_build_sha256 = IdentityDigest(0x10);
  manifest.replay_config_sha256 = IdentityDigest(0x20);
  manifest.title_identity_sha256 = IdentityDigest(0x30);
  manifest.module_identity_sha256 = IdentityDigest(0x40);
  manifest.accepted_segment_count = 1;
  manifest.accepted_event_count = 6;
  manifest.stop_reason = GuestExecutionSessionStopReason::kRequestedBoundary;
  manifest.stop_request_event_sequence = 5;
  manifest.stop_request_tick = 800;
  manifest.stop_request_accepted_segment_count = 1;
  manifest.maximum_stop_tail_event_count = 16;
  manifest.maximum_stop_tail_guest_instruction_count = 64;
  manifest.maximum_stop_tail_ticks = 1000;
  GuestExecutionSessionParticipant participant;
  participant.ordinal = 0;
  participant.guest_thread_id = 7;
  participant.capture_instance_id = 0x100;
  participant.boundary_arrival_kind =
      GuestExecutionSessionBoundaryArrivalKind::kAlreadyOutside;
  participant.first_event_sequence = 1;
  participant.last_event_sequence = 4;
  participant.held_after_event_sequence = 5;
  participant.initial_state_size = 64;
  participant.initial_state_sha256 = initial_state;
  manifest.participants.push_back(participant);
  manifest.segments.push_back(
      {0, 0, 1, 4, 0x82000000, 0x820000FC, corpus, segment_blob});
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 0, 0, 0, 1,
                   bundle.chunks[0]));
  manifest.chunks.push_back(ReferenceFor(
      GuestExecutionSessionChunkKind::kEvents, 1, 1, 6, 6, bundle.chunks[1]));
  manifest.chunks.push_back(
      ReferenceFor(GuestExecutionSessionChunkKind::kCheckpoint, 2, 6, 6, 1,
                   bundle.chunks[2]));
  REQUIRE(GuestExecutionSessionCodec::ValidateSession(manifest, bundle.chunks,
                                                      &error));

  std::reverse(bundle.content_blobs.begin(), bundle.content_blobs.end());
  return bundle;
}

std::map<GuestExecutionSessionSha256, std::vector<uint8_t>> BlobMap(
    const GuestExecutionSessionBundle& bundle) {
  std::map<GuestExecutionSessionSha256, std::vector<uint8_t>> result;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    REQUIRE(result.emplace(blob.sha256, blob.bytes).second);
  }
  return result;
}

std::string HexDigest(const GuestExecutionSessionSha256& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  for (uint8_t value : digest) {
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 0xF]);
  }
  return result;
}

std::filesystem::path BlobName(const GuestExecutionSessionSha256& digest) {
  return "blob-" + HexDigest(digest) + ".xegb";
}

std::filesystem::path ChunkName(
    const GuestExecutionSessionChunkReference& reference) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string ordinal(8, '0');
  for (size_t i = 0; i < ordinal.size(); ++i) {
    ordinal[i] =
        kHex[(reference.ordinal >> ((ordinal.size() - i - 1) * 4)) & 0xF];
  }
  const std::string kind =
      reference.kind == GuestExecutionSessionChunkKind::kEvents ? "events"
                                                                : "checkpoint";
  return "chunk-" + ordinal + "-" + kind + "-" +
         HexDigest(reference.encoded_sha256) + ".xegc";
}

void WriteFile(const std::filesystem::path& path,
               const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  REQUIRE(stream.is_open());
  stream.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  REQUIRE(stream.good());
}

void WriteText(const std::filesystem::path& path, const std::string& text) {
  WriteFile(path, std::vector<uint8_t>(text.cbegin(), text.cend()));
}

std::string ReadText(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  REQUIRE(stream.is_open());
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

std::filesystem::path StagingPath(const std::filesystem::path& output) {
  std::filesystem::path staging = output;
  staging += ".part";
  return staging;
}

}  // namespace

TEST_CASE("session bundle publishes and reads one exact closed object graph",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  const std::filesystem::path output = temporary_directory.path() / "capture";
  std::string error = "stale";

  REQUIRE(WriteGuestExecutionSessionBundle(output, bundle, &error));
  CHECK(error.empty());
  CHECK(std::filesystem::is_directory(output));
  CHECK_FALSE(std::filesystem::exists(StagingPath(output)));

  GuestExecutionSessionBundle decoded;
  REQUIRE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  CHECK(error.empty());
  CHECK(decoded.manifest == bundle.manifest);
  CHECK(decoded.chunks == bundle.chunks);
  CHECK(BlobMap(decoded) == BlobMap(bundle));
  CHECK(std::is_sorted(decoded.content_blobs.cbegin(),
                       decoded.content_blobs.cend(),
                       [](const GuestExecutionSessionContentBlob& left,
                          const GuestExecutionSessionContentBlob& right) {
                         return left.sha256 < right.sha256;
                       }));

  size_t file_count = 0;
  for (const std::filesystem::directory_entry& entry :
       std::filesystem::directory_iterator(output)) {
    CHECK(entry.is_regular_file());
    CHECK_FALSE(entry.is_symlink());
    ++file_count;
  }
  CHECK(file_count == 1 + bundle.chunks.size() + bundle.content_blobs.size());
}

TEST_CASE("session bundle rejects an invalid graph before creating staging",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle valid = MakeBundle();
  std::string error;

  auto RejectWithoutStaging =
      [&](std::string_view name, const GuestExecutionSessionBundle& bundle,
          GuestExecutionSessionBundleLimits limits = {}) {
        const std::filesystem::path output =
            temporary_directory.path() / std::string(name);
        CHECK_FALSE(
            WriteGuestExecutionSessionBundle(output, bundle, &error, limits));
        CHECK_FALSE(std::filesystem::exists(output));
        CHECK_FALSE(std::filesystem::exists(StagingPath(output)));
      };

  GuestExecutionSessionBundle invalid = valid;
  invalid.content_blobs.pop_back();
  RejectWithoutStaging("missing-blob", invalid);

  invalid = valid;
  AddBlob(&invalid, Bytes(31, 0xA0));
  RejectWithoutStaging("extra-blob", invalid);

  invalid = valid;
  invalid.content_blobs.push_back(invalid.content_blobs.front());
  RejectWithoutStaging("duplicate-blob", invalid);

  invalid = valid;
  invalid.content_blobs.front().bytes.front() ^= 1;
  RejectWithoutStaging("substituted-blob", invalid);

  invalid = valid;
  invalid.chunks[1][GuestExecutionSessionCodec::kEnvelopeHeaderSize] ^= 1;
  RejectWithoutStaging("substituted-chunk", invalid);

  GuestExecutionSessionBundleLimits limits;
  limits.maximum_content_blobs =
      static_cast<uint32_t>(valid.content_blobs.size() - 1);
  RejectWithoutStaging("blob-count-limit", valid, limits);

  limits = {};
  limits.maximum_total_content_bytes = 1;
  RejectWithoutStaging("blob-byte-limit", valid, limits);

  limits = {};
  limits.maximum_bundle_bytes = 1;
  RejectWithoutStaging("bundle-byte-limit", valid, limits);
}

TEST_CASE("session bundle preserves existing roots and rejects unsafe paths",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  std::string error;

  const std::filesystem::path existing = temporary_directory.path() / "exists";
  REQUIRE(std::filesystem::create_directory(existing));
  WriteText(existing / "sentinel", "keep-output");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(existing, bundle, &error));
  CHECK(ReadText(existing / "sentinel") == "keep-output");
  CHECK_FALSE(std::filesystem::exists(StagingPath(existing)));

  const std::filesystem::path output =
      temporary_directory.path() / "staging-exists";
  const std::filesystem::path staging = StagingPath(output);
  REQUIRE(std::filesystem::create_directory(staging));
  WriteText(staging / "sentinel", "keep-staging");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(output, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(output));
  CHECK(ReadText(staging / "sentinel") == "keep-staging");

  const std::filesystem::path traversal =
      temporary_directory.path() / ".." /
      (temporary_directory.path().filename().string() + "-escaped");
  CHECK_FALSE(WriteGuestExecutionSessionBundle(traversal, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(traversal));

  const std::filesystem::path staging_name =
      temporary_directory.path() / "not-a-final.part";
  CHECK_FALSE(WriteGuestExecutionSessionBundle(staging_name, bundle, &error));
  CHECK_FALSE(std::filesystem::exists(staging_name));

  const std::filesystem::path missing_target =
      temporary_directory.path() / "missing-target";
  const std::filesystem::path linked_output =
      temporary_directory.path() / "linked-output";
  std::error_code filesystem_error;
  std::filesystem::create_symlink(missing_target, linked_output,
                                  filesystem_error);
  REQUIRE_FALSE(filesystem_error);
  CHECK_FALSE(WriteGuestExecutionSessionBundle(linked_output, bundle, &error));
  CHECK(std::filesystem::is_symlink(linked_output));

  const std::filesystem::path output_with_linked_staging =
      temporary_directory.path() / "linked-staging";
  const std::filesystem::path linked_staging =
      StagingPath(output_with_linked_staging);
  std::filesystem::create_symlink(missing_target, linked_staging,
                                  filesystem_error);
  REQUIRE_FALSE(filesystem_error);
  CHECK_FALSE(WriteGuestExecutionSessionBundle(output_with_linked_staging,
                                               bundle, &error));
  CHECK_FALSE(std::filesystem::exists(output_with_linked_staging));
  CHECK(std::filesystem::is_symlink(linked_staging));
}

TEST_CASE("session bundle reader fails closed on filesystem substitution",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  const GuestExecutionSessionBundle bundle = MakeBundle();
  const std::filesystem::path output = temporary_directory.path() / "capture";
  std::string error;
  REQUIRE(WriteGuestExecutionSessionBundle(output, bundle, &error));

  GuestExecutionSessionBundle decoded = bundle;
  SECTION("content bytes changed") {
    const GuestExecutionSessionContentBlob& blob = bundle.content_blobs.front();
    std::vector<uint8_t> changed = blob.bytes;
    changed.front() ^= 1;
    WriteFile(output / BlobName(blob.sha256), changed);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("chunk is missing") {
    REQUIRE(
        std::filesystem::remove(output / ChunkName(bundle.manifest.chunks[1])));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("chunk bytes changed") {
    std::vector<uint8_t> changed = bundle.chunks[1];
    changed[GuestExecutionSessionCodec::kEnvelopeHeaderSize] ^= 1;
    WriteFile(output / ChunkName(bundle.manifest.chunks[1]), changed);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("manifest closure changed") {
    const std::filesystem::path path =
        output / kGuestExecutionSessionBundleManifestFileName;
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(stream),
                               std::istreambuf_iterator<char>()};
    REQUIRE_FALSE(bytes.empty());
    bytes.back() ^= 1;
    WriteFile(path, bytes);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("extra regular file") {
    WriteText(output / "extra", "unexpected");
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("extra hard-link alias") {
    std::error_code filesystem_error;
    std::filesystem::create_hard_link(
        output / kGuestExecutionSessionBundleManifestFileName,
        output / "manifest-alias", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("symlink entry") {
    std::error_code filesystem_error;
    std::filesystem::create_symlink(
        output / kGuestExecutionSessionBundleManifestFileName,
        output / "manifest-link", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("non-regular entry") {
    REQUIRE(std::filesystem::create_directory(output / "nested"));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("symlink root") {
    const std::filesystem::path alias = temporary_directory.path() / "alias";
    std::error_code filesystem_error;
    std::filesystem::create_directory_symlink(output, alias, filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(alias, &decoded, &error));
  }
  SECTION("renamed object") {
    const std::filesystem::path blob =
        output / BlobName(bundle.content_blobs.front().sha256);
    std::error_code filesystem_error;
    std::filesystem::rename(blob, output / "renamed-blob", filesystem_error);
    REQUIRE_FALSE(filesystem_error);
    CHECK_FALSE(ReadGuestExecutionSessionBundle(output, &decoded, &error));
  }
  SECTION("published root presented as staging") {
    const std::filesystem::path partial = StagingPath(output);
    REQUIRE_NOTHROW(std::filesystem::rename(output, partial));
    CHECK_FALSE(ReadGuestExecutionSessionBundle(partial, &decoded, &error));
  }
  SECTION("path traversal alias") {
    const std::filesystem::path traversed = output / ".." / output.filename();
    CHECK_FALSE(ReadGuestExecutionSessionBundle(traversed, &decoded, &error));
  }
  SECTION("content size limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.session.maximum_content_blob_bytes = 4095;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  SECTION("bundle byte limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.maximum_bundle_bytes = 1;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  SECTION("directory entry limit") {
    GuestExecutionSessionBundleLimits limits;
    limits.maximum_content_blobs = 1;
    CHECK_FALSE(
        ReadGuestExecutionSessionBundle(output, &decoded, &error, limits));
  }
  CHECK(decoded == GuestExecutionSessionBundle{});
}

TEST_CASE("session bundle no-replace publication cleans racing staging",
          "[guest-execution-session-bundle]") {
  ScopedTestDirectory temporary_directory;
  GuestExecutionSessionBundle bundle = MakeBundle();
  // Make two size-unconstrained segment objects large enough that the racing
  // creator deterministically observes staging before final publication.
  for (GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    if (blob.sha256 == bundle.manifest.segments[0].code_corpus_sha256) {
      blob.bytes = Bytes(2 * 1024 * 1024, 0x70);
      blob.sha256 = GuestExecutionSessionCodec::HashBytes(blob.bytes);
      bundle.manifest.segments[0].code_corpus_sha256 = blob.sha256;
    } else if (blob.sha256 == bundle.manifest.segments[0].segment_sha256) {
      blob.bytes = Bytes(2 * 1024 * 1024, 0x80);
      blob.sha256 = GuestExecutionSessionCodec::HashBytes(blob.bytes);
      bundle.manifest.segments[0].segment_sha256 = blob.sha256;
    }
  }

  const std::filesystem::path output = temporary_directory.path() / "race";
  const std::filesystem::path staging = StagingPath(output);
  const std::filesystem::path sentinel = output / "sentinel";
  std::atomic<bool> writer_finished = false;
  std::atomic<bool> competitor_created = false;
  std::thread competitor([&]() {
    while (!writer_finished.load(std::memory_order_acquire)) {
      std::error_code filesystem_error;
      if (std::filesystem::exists(staging, filesystem_error) &&
          !filesystem_error &&
          std::filesystem::create_directory(output, filesystem_error)) {
        std::ofstream stream(sentinel, std::ios::binary);
        stream << "keep-racing-output";
        competitor_created.store(stream.good(), std::memory_order_release);
        return;
      }
      std::this_thread::yield();
    }
  });
  std::string error;
  const bool succeeded =
      WriteGuestExecutionSessionBundle(output, bundle, &error);
  writer_finished.store(true, std::memory_order_release);
  competitor.join();

  REQUIRE(competitor_created.load(std::memory_order_acquire));
  CHECK_FALSE(succeeded);
  CHECK(ReadText(sentinel) == "keep-racing-output");
  CHECK_FALSE(std::filesystem::exists(staging));
}

}  // namespace test
}  // namespace cpu
}  // namespace xe
