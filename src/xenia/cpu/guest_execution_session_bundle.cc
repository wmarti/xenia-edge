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
#include <cstdio>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "xenia/base/platform.h"
#include "xenia/cpu/execution_jit_corpus.h"
#include "xenia/cpu/guest_execution_continuous_event.h"

#if XE_PLATFORM_MAC
#include <fcntl.h>
#include <sys/stdio.h>
#include <unistd.h>
#elif XE_PLATFORM_WIN32
#include <io.h>

#include "xenia/base/platform_win.h"
#elif XE_PLATFORM_LINUX
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace xe {
namespace cpu {

namespace {

constexpr std::string_view kStagingSuffix = ".part";
constexpr std::string_view kChunkPrefix = "chunk-";
constexpr std::string_view kChunkSuffix = ".xegc";
constexpr std::string_view kBlobPrefix = "blob-";
constexpr std::string_view kBlobSuffix = ".xegb";

bool Fail(std::string* error, std::string_view message) {
  if (error) {
    error->assign(message);
  }
  return false;
}

bool CheckedAdd(uint64_t left, uint64_t right, uint64_t* result) {
  if (right > std::numeric_limits<uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

bool IsNonzeroHash(const GuestExecutionSessionSha256& hash) {
  for (uint8_t value : hash) {
    if (value) {
      return true;
    }
  }
  return false;
}

std::string HexDigest(const GuestExecutionSessionSha256& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(digest.size() * 2);
  for (uint8_t value : digest) {
    result.push_back(kHex[value >> 4]);
    result.push_back(kHex[value & 0xF]);
  }
  return result;
}

std::string HexOrdinal(uint32_t ordinal) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result(8, '0');
  for (size_t i = 0; i < result.size(); ++i) {
    result[i] = kHex[(ordinal >> ((result.size() - i - 1) * 4)) & 0xF];
  }
  return result;
}

std::filesystem::path ChunkFileName(
    const GuestExecutionSessionChunkReference& chunk) {
  std::string_view kind;
  switch (chunk.kind) {
    case GuestExecutionSessionChunkKind::kEvents:
      kind = "events";
      break;
    case GuestExecutionSessionChunkKind::kCheckpoint:
      kind = "checkpoint";
      break;
    case GuestExecutionSessionChunkKind::kContinuousEvents:
      kind = "continuous";
      break;
    case GuestExecutionSessionChunkKind::kCodeCorpus:
      kind = "code-corpus";
      break;
    default:
      kind = "unknown";
      break;
  }
  return std::string(kChunkPrefix) + HexOrdinal(chunk.ordinal) + "-" +
         std::string(kind) + "-" + HexDigest(chunk.encoded_sha256) +
         std::string(kChunkSuffix);
}

std::filesystem::path BlobFileName(const GuestExecutionSessionSha256& digest) {
  return std::string(kBlobPrefix) + HexDigest(digest) +
         std::string(kBlobSuffix);
}

bool HasParentTraversal(const std::filesystem::path& path) {
  for (const std::filesystem::path& component : path) {
    if (component == "..") {
      return true;
    }
  }
  return false;
}

bool HasStagingSuffix(const std::filesystem::path& path) {
  const std::string filename = path.filename().string();
  return filename.size() >= kStagingSuffix.size() &&
         filename.compare(filename.size() - kStagingSuffix.size(),
                          kStagingSuffix.size(), kStagingSuffix) == 0;
}

bool PathEntryExists(const std::filesystem::path& path, bool* exists,
                     std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (filesystem_error &&
      filesystem_error != std::errc::no_such_file_or_directory) {
    return Fail(error, "failed to inspect a bundle filesystem entry");
  }
  *exists = !filesystem_error &&
            status.type() != std::filesystem::file_type::not_found;
  return true;
}

bool ResolveOutputDirectory(const std::filesystem::path& requested,
                            std::filesystem::path* output, std::string* error) {
  if (requested.empty() || requested == "." || requested == ".." ||
      requested == requested.root_path() || requested.filename().empty() ||
      HasParentTraversal(requested) || HasStagingSuffix(requested)) {
    return Fail(error, "session bundle output directory is unsafe");
  }
  const std::filesystem::path requested_parent =
      requested.has_parent_path() ? requested.parent_path() : ".";
  std::error_code filesystem_error;
  const std::filesystem::path canonical_parent =
      std::filesystem::canonical(requested_parent, filesystem_error);
  if (filesystem_error || canonical_parent == canonical_parent.root_path() ||
      !std::filesystem::is_directory(canonical_parent, filesystem_error) ||
      filesystem_error) {
    return Fail(error,
                "session bundle output parent is missing, unsafe or not a "
                "directory");
  }
  *output = canonical_parent / requested.filename();
  return true;
}

bool SyncOpenFile(FILE* file) {
#if XE_PLATFORM_WIN32
  const intptr_t handle_value = _get_osfhandle(_fileno(file));
  return handle_value != -1 &&
         ::FlushFileBuffers(reinterpret_cast<HANDLE>(handle_value));
#elif XE_PLATFORM_MAC
  const int descriptor = fileno(file);
  return descriptor >= 0 &&
         (::fcntl(descriptor, F_FULLFSYNC) == 0 || ::fsync(descriptor) == 0);
#elif XE_PLATFORM_LINUX
  const int descriptor = fileno(file);
  return descriptor >= 0 && ::fsync(descriptor) == 0;
#else
  return false;
#endif
}

FILE* OpenBundleFile(const std::filesystem::path& path, bool write_exclusive) {
#if XE_PLATFORM_WIN32
  return _wfopen(path.c_str(), write_exclusive ? L"wbx" : L"rb");
#else
  return std::fopen(path.c_str(), write_exclusive ? "wbx" : "rb");
#endif
}

bool WriteBytesExclusiveDurable(const std::filesystem::path& path,
                                const std::vector<uint8_t>& bytes,
                                std::string* error) {
  FILE* file = OpenBundleFile(path, true);
  if (!file) {
    return Fail(error, "failed to exclusively create a session bundle file");
  }
  const bool write_succeeded =
      bytes.empty() ||
      std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
  const bool flush_succeeded = write_succeeded && std::fflush(file) == 0;
  const bool sync_succeeded = flush_succeeded && SyncOpenFile(file);
  const bool close_succeeded = std::fclose(file) == 0;
  if (!write_succeeded || !flush_succeeded || !sync_succeeded ||
      !close_succeeded) {
    return Fail(error, "failed to durably write a complete bundle file");
  }
  return true;
}

bool SyncDirectory(const std::filesystem::path& path, std::string* error) {
#if XE_PLATFORM_WIN32
  HANDLE handle = ::CreateFileW(
      path.c_str(), GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Fail(error, "failed to open a bundle directory for durable sync");
  }
  const bool succeeded = ::FlushFileBuffers(handle) != 0;
  ::CloseHandle(handle);
  return succeeded ? true
                   : Fail(error, "failed to durably sync a bundle directory");
#elif XE_PLATFORM_MAC || XE_PLATFORM_LINUX
  const int descriptor = ::open(path.c_str(), O_RDONLY);
  if (descriptor < 0) {
    return Fail(error, "failed to open a bundle directory for durable sync");
  }
  const bool succeeded = ::fsync(descriptor) == 0;
  const bool close_succeeded = ::close(descriptor) == 0;
  return succeeded && close_succeeded
             ? true
             : Fail(error, "failed to durably sync a bundle directory");
#else
  return Fail(error, "durable bundle directory sync is unsupported");
#endif
}

bool PublishDirectoryNoReplace(const std::filesystem::path& staging_directory,
                               const std::filesystem::path& output_directory,
                               std::string* error) {
#if XE_PLATFORM_MAC
  if (::renamex_np(staging_directory.c_str(), output_directory.c_str(),
                   RENAME_EXCL) == 0) {
    return true;
  }
#elif XE_PLATFORM_WIN32
  if (::MoveFileExW(staging_directory.c_str(), output_directory.c_str(),
                    MOVEFILE_WRITE_THROUGH)) {
    return true;
  }
#elif XE_PLATFORM_LINUX && defined(SYS_renameat2)
  constexpr unsigned int kRenameNoReplace = 1;
  if (::syscall(SYS_renameat2, AT_FDCWD, staging_directory.c_str(), AT_FDCWD,
                output_directory.c_str(), kRenameNoReplace) == 0) {
    return true;
  }
#endif
  return Fail(error,
              "failed to atomically publish the session bundle without "
              "replacement");
}

class StagingDirectory {
 public:
  explicit StagingDirectory(std::filesystem::path path)
      : path_(std::move(path)) {}
  ~StagingDirectory() {
    if (active_) {
      std::error_code filesystem_error;
      std::filesystem::remove_all(path_, filesystem_error);
    }
  }

  void Release() { active_ = false; }

 private:
  std::filesystem::path path_;
  bool active_ = true;
};

struct RequiredBlob {
  bool has_expected_size = false;
  uint64_t expected_size = 0;
};

bool AddRequiredBlob(
    const GuestExecutionSessionSha256& digest, bool has_expected_size,
    uint64_t expected_size,
    std::map<GuestExecutionSessionSha256, RequiredBlob>* requirements,
    std::string* error) {
  if (!IsNonzeroHash(digest) || (has_expected_size && !expected_size)) {
    return Fail(error, "session bundle contains an invalid blob reference");
  }
  const auto [it, inserted] = requirements->emplace(
      digest, RequiredBlob{has_expected_size, expected_size});
  if (!inserted && has_expected_size) {
    if (it->second.has_expected_size &&
        it->second.expected_size != expected_size) {
      return Fail(error, "one blob digest has conflicting referenced sizes");
    }
    it->second.has_expected_size = true;
    it->second.expected_size = expected_size;
  }
  return true;
}

bool CollectRequiredBlobs(
    const GuestExecutionSessionManifest& manifest,
    const std::vector<std::vector<uint8_t>>& chunks,
    const GuestExecutionSessionBundleLimits& limits,
    std::map<GuestExecutionSessionSha256, RequiredBlob>* requirements,
    std::string* error) {
  requirements->clear();
  for (const GuestExecutionSessionParticipant& participant :
       manifest.participants) {
    if (!AddRequiredBlob(participant.initial_state_sha256, true,
                         participant.initial_state_size, requirements, error)) {
      return false;
    }
  }
  for (const GuestExecutionSessionSegmentReference& segment :
       manifest.segments) {
    if (!AddRequiredBlob(segment.code_corpus_sha256, false, 0, requirements,
                         error) ||
        !AddRequiredBlob(segment.segment_sha256, false, 0, requirements,
                         error)) {
      return false;
    }
  }

  for (size_t i = 0; i < chunks.size(); ++i) {
    if (manifest.chunks[i].kind == GuestExecutionSessionChunkKind::kEvents) {
      GuestExecutionSessionEventChunk chunk;
      if (!GuestExecutionSessionCodec::DecodeEventChunk(
              chunks[i], &chunk, error, limits.session)) {
        return false;
      }
      for (const GuestExecutionSessionEvent& event : chunk.events) {
        if (event.payload_size &&
            !AddRequiredBlob(event.payload_sha256, true, event.payload_size,
                             requirements, error)) {
          return false;
        }
      }
    } else if (manifest.chunks[i].kind ==
               GuestExecutionSessionChunkKind::kContinuousEvents) {
      continue;
    } else if (manifest.chunks[i].kind ==
               GuestExecutionSessionChunkKind::kCheckpoint) {
      GuestExecutionSessionCheckpointChunk chunk;
      if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
              chunks[i], &chunk, error, limits.session)) {
        return false;
      }
      for (const GuestExecutionSessionThreadStateReference& state :
           chunk.checkpoint.thread_states) {
        if (!AddRequiredBlob(state.sha256, true, state.byte_size, requirements,
                             error)) {
          return false;
        }
      }
      for (const GuestExecutionSessionContentReference& content :
           chunk.checkpoint.content) {
        if (!AddRequiredBlob(content.sha256, true, content.byte_size,
                             requirements, error)) {
          return false;
        }
      }
    } else {
      GuestExecutionSessionCodeCorpusChunk chunk;
      if (!GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
              chunks[i], &chunk, error, limits.session) ||
          !AddRequiredBlob(chunk.code_corpus_sha256, false, 0, requirements,
                           error)) {
        return false;
      }
    }
  }
  if (requirements->size() > limits.maximum_content_blobs) {
    return Fail(error, "session bundle exceeds the referenced-blob limit");
  }
  return true;
}

struct ValidatedBundle {
  std::vector<uint8_t> manifest_bytes;
  std::map<GuestExecutionSessionSha256, RequiredBlob> requirements;
  std::map<GuestExecutionSessionSha256, const GuestExecutionSessionContentBlob*>
      blobs;
};

bool ValidateContinuousCheckpointBlobs(
    const GuestExecutionSessionBundle& bundle,
    const GuestExecutionSessionBundleLimits& limits,
    const ValidatedBundle& validated, std::string* error) {
  GuestExecutionContinuousEventLimits continuous_limits;
  continuous_limits.maximum_encoded_bytes = limits.session.maximum_chunk_bytes;
  continuous_limits.maximum_records = limits.session.maximum_events_per_chunk;
  for (size_t chunk_index = 0; chunk_index < bundle.chunks.size();
       ++chunk_index) {
    if (bundle.manifest.chunks[chunk_index].kind !=
        GuestExecutionSessionChunkKind::kContinuousEvents) {
      continue;
    }
    std::vector<GuestExecutionContinuousEvent> events;
    if (!GuestExecutionContinuousEventCodec::Decode(
            bundle.chunks[chunk_index], &events, error, continuous_limits)) {
      return false;
    }
    for (const GuestExecutionContinuousEvent& event : events) {
      if (event.checkpoint.kind !=
          GuestExecutionContinuousCheckpointReferenceKind::kThreadState) {
        continue;
      }
      const auto blob = validated.blobs.find(event.checkpoint.state_sha256);
      if (blob == validated.blobs.end()) {
        return Fail(error, "continuous event checkpoint blob is not present");
      }
      ppc::GuestPPCThreadCheckpoint checkpoint;
      std::string checkpoint_error;
      if (!GuestExecutionContinuousEventCodec::DecodeAndValidateCheckpoint(
              event, blob->second->bytes, event.checkpoint.binding, &checkpoint,
              &checkpoint_error)) {
        return Fail(error,
                    "continuous event checkpoint blob or subject is invalid: " +
                        checkpoint_error);
      }
    }
  }
  return true;
}

bool ValidateContinuousCodeClosure(
    const GuestExecutionSessionBundle& bundle,
    const std::map<GuestExecutionSessionSha256,
                   const GuestExecutionSessionContentBlob*>& blobs,
    const GuestExecutionSessionLimits& limits, std::string* error) {
  if (!bundle.manifest.segments.empty()) {
    return true;
  }

  GuestExecutionSessionCheckpointChunk initial;
  GuestExecutionSessionCheckpointChunk final_checkpoint;
  GuestExecutionSessionCodeCorpusChunk corpus_reference;
  if (!GuestExecutionSessionCodec::DecodeCheckpointChunk(
          bundle.chunks.front(), &initial, error, limits) ||
      !GuestExecutionSessionCodec::DecodeCodeCorpusChunk(
          bundle.chunks[1], &corpus_reference, error, limits) ||
      !GuestExecutionSessionCodec::DecodeCheckpointChunk(
          bundle.chunks.back(), &final_checkpoint, error, limits)) {
    return false;
  }
  const auto corpus_blob = blobs.find(corpus_reference.code_corpus_sha256);
  if (corpus_blob == blobs.end()) {
    return Fail(error, "continuous session code corpus blob is missing");
  }
  ExecutionJitCorpus corpus;
  std::string corpus_error;
  if (!ExecutionJitCorpus::Decode(corpus_blob->second->bytes, &corpus,
                                  &corpus_error)) {
    return Fail(error,
                "continuous session code corpus is invalid: " + corpus_error);
  }

  // A zero-segment session has one mandatory session-level corpus. Close every
  // resumable overlay route to an exact function record in that corpus, not
  // merely to a captured page containing the declared extent. Segmented
  // version-2 sessions retain their per-segment corpus contract and are not
  // retroactively interpreted as one union corpus here.
  GuestExecutionContinuousEventLimits continuous_limits;
  continuous_limits.maximum_encoded_bytes = limits.maximum_chunk_bytes;
  continuous_limits.maximum_records = limits.maximum_events_per_chunk;
  for (size_t chunk_index = 0; chunk_index < bundle.chunks.size();
       ++chunk_index) {
    if (bundle.manifest.chunks[chunk_index].kind !=
        GuestExecutionSessionChunkKind::kContinuousEvents) {
      continue;
    }
    std::vector<GuestExecutionContinuousEvent> events;
    if (!GuestExecutionContinuousEventCodec::Decode(
            bundle.chunks[chunk_index], &events, error, continuous_limits)) {
      return false;
    }
    for (const GuestExecutionContinuousEvent& event : events) {
      if (event.checkpoint.kind !=
          GuestExecutionContinuousCheckpointReferenceKind::kThreadState) {
        continue;
      }
      const ppc::GuestPPCThreadCheckpointBinding& binding =
          event.checkpoint.binding;
      const ExecutionJitCorpus::FunctionRecord* function =
          corpus.FindFunction(binding.owning_function_address);
      if (!function) {
        return Fail(
            error,
            "zero-segment continuous checkpoint owning function is absent "
            "from the session code corpus");
      }
      if (function->end_address != binding.owning_function_end_address) {
        return Fail(error,
                    "zero-segment continuous checkpoint owning function extent "
                    "differs from the session code corpus");
      }
    }
  }

  std::map<uint64_t, GuestExecutionSessionContentReference> initial_code;
  for (const GuestExecutionSessionContentReference& content :
       initial.checkpoint.content) {
    if (content.kind == GuestExecutionSessionContentKind::kGuestCode) {
      initial_code.emplace(content.guest_address, content);
    }
  }
  if (initial_code.empty() ||
      initial_code.size() != corpus.page_addresses().size()) {
    return Fail(error,
                "continuous initial checkpoint does not close the exact code "
                "corpus pages");
  }
  size_t page_index = 0;
  for (const auto& [guest_address, content] : initial_code) {
    const uint32_t corpus_page = corpus.page_addresses()[page_index++];
    const auto code_blob = blobs.find(content.sha256);
    const uint8_t* corpus_bytes = corpus.FindPageData(corpus_page);
    if (guest_address != corpus_page ||
        content.byte_size != JitCorpus::kPageSize || code_blob == blobs.end() ||
        !corpus_bytes ||
        !std::equal(code_blob->second->bytes.cbegin(),
                    code_blob->second->bytes.cend(), corpus_bytes)) {
      return Fail(error,
                  "continuous initial guest code differs from its exact "
                  "corpus");
    }
  }

  // The final checkpoint is sparse. Any final code record therefore names a
  // dirtied range, and must still match its initial corpus-bound page until a
  // self-modifying-code event model is added.
  for (const GuestExecutionSessionContentReference& content :
       final_checkpoint.checkpoint.content) {
    const auto initial_it = initial_code.find(content.guest_address);
    if (content.kind != GuestExecutionSessionContentKind::kGuestCode &&
        initial_it == initial_code.end()) {
      continue;
    }
    if (initial_it == initial_code.end() || initial_it->second != content) {
      return Fail(error,
                  "continuous final guest code differs without an explicit "
                  "code-mutation model");
    }
  }
  return true;
}

bool ValidateBundle(const GuestExecutionSessionBundle& bundle,
                    const GuestExecutionSessionBundleLimits& limits,
                    ValidatedBundle* output, std::string* error) {
  *output = {};
  if (bundle.content_blobs.size() > limits.maximum_content_blobs) {
    return Fail(error, "session bundle exceeds the supplied-blob limit");
  }
  if (!GuestExecutionSessionCodec::ValidateSession(
          bundle.manifest, bundle.chunks, error, limits.session) ||
      !GuestExecutionSessionCodec::EncodeManifest(
          bundle.manifest, &output->manifest_bytes, error, limits.session)) {
    return false;
  }
  GuestExecutionSessionManifest decoded_manifest;
  if (!GuestExecutionSessionCodec::DecodeManifest(
          output->manifest_bytes, &decoded_manifest, error, limits.session) ||
      decoded_manifest != bundle.manifest) {
    return Fail(error,
                "session manifest failed its pre-publication round trip");
  }
  if (!CollectRequiredBlobs(bundle.manifest, bundle.chunks, limits,
                            &output->requirements, error)) {
    return false;
  }
  if (output->requirements.size() != bundle.content_blobs.size()) {
    return Fail(error,
                "session bundle has missing, extra or duplicate blob objects");
  }

  uint64_t total_content_bytes = 0;
  for (const GuestExecutionSessionContentBlob& blob : bundle.content_blobs) {
    if (!IsNonzeroHash(blob.sha256) || blob.bytes.empty() ||
        blob.bytes.size() > limits.session.maximum_content_blob_bytes ||
        GuestExecutionSessionCodec::HashBytes(blob.bytes) != blob.sha256 ||
        !output->blobs.emplace(blob.sha256, &blob).second ||
        !CheckedAdd(total_content_bytes, blob.bytes.size(),
                    &total_content_bytes) ||
        total_content_bytes > limits.maximum_total_content_bytes) {
      return Fail(error,
                  "session bundle supplied blob is invalid or duplicated");
    }
  }
  for (const auto& [digest, requirement] : output->requirements) {
    const auto blob = output->blobs.find(digest);
    if (blob == output->blobs.end() ||
        (requirement.has_expected_size &&
         blob->second->bytes.size() != requirement.expected_size)) {
      return Fail(error, "session bundle blob reference is not satisfied");
    }
  }
  if (!ValidateContinuousCheckpointBlobs(bundle, limits, *output, error) ||
      !ValidateContinuousCodeClosure(bundle, output->blobs, limits.session,
                                     error)) {
    return false;
  }

  uint64_t total_bundle_bytes = output->manifest_bytes.size();
  for (const std::vector<uint8_t>& chunk : bundle.chunks) {
    if (!CheckedAdd(total_bundle_bytes, chunk.size(), &total_bundle_bytes)) {
      return Fail(error, "session bundle byte count overflows");
    }
  }
  if (!CheckedAdd(total_bundle_bytes, total_content_bytes,
                  &total_bundle_bytes) ||
      total_bundle_bytes > limits.maximum_bundle_bytes) {
    return Fail(error, "session bundle exceeds the total byte limit");
  }
  return true;
}

bool ReadRegularFile(const std::filesystem::path& path, uint64_t maximum_size,
                     bool has_expected_size, uint64_t expected_size,
                     std::vector<uint8_t>* output, std::string* error) {
  output->clear();
  std::error_code filesystem_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (filesystem_error ||
      status.type() != std::filesystem::file_type::regular) {
    return Fail(error, "session bundle entry is missing or not a regular file");
  }
  const uint64_t file_size = std::filesystem::file_size(path, filesystem_error);
  if (filesystem_error || file_size > maximum_size ||
      file_size > std::numeric_limits<size_t>::max() ||
      (has_expected_size && file_size != expected_size)) {
    return Fail(error, "session bundle file byte count is invalid");
  }
  FILE* file = OpenBundleFile(path, false);
  if (!file) {
    return Fail(error, "failed to open a session bundle file");
  }
  output->resize(static_cast<size_t>(file_size));
  const bool read_succeeded =
      output->empty() ||
      std::fread(output->data(), 1, output->size(), file) == output->size();
  const int trailing_byte = read_succeeded ? std::fgetc(file) : 0;
  const bool exact_end =
      read_succeeded && trailing_byte == EOF && std::ferror(file) == 0;
  const bool close_succeeded = std::fclose(file) == 0;
  if (!exact_end || !close_succeeded) {
    output->clear();
    return Fail(error, "failed to read one exact session bundle file");
  }
  return true;
}

bool ListRegularEntries(
    const std::filesystem::path& directory, uint64_t maximum_entries,
    std::map<std::filesystem::path, std::filesystem::path>* entries,
    std::string* error) {
  entries->clear();
  std::error_code filesystem_error;
  std::filesystem::directory_iterator iterator(directory, filesystem_error);
  const std::filesystem::directory_iterator end;
  if (filesystem_error) {
    return Fail(error, "failed to enumerate the session bundle directory");
  }
  while (iterator != end) {
    const std::filesystem::directory_entry& entry = *iterator;
    const std::filesystem::file_status status =
        entry.symlink_status(filesystem_error);
    if (filesystem_error ||
        status.type() != std::filesystem::file_type::regular ||
        entry.path().filename().empty() || entries->size() >= maximum_entries ||
        !entries->emplace(entry.path().filename(), entry.path()).second) {
      return Fail(error,
                  "session bundle has a symlink, non-regular, duplicate or "
                  "excess entry");
    }
    iterator.increment(filesystem_error);
    if (filesystem_error) {
      return Fail(error, "failed while enumerating the bundle directory");
    }
  }
  return true;
}

bool ReadBundleInternal(const std::filesystem::path& requested_directory,
                        bool allow_staging_root,
                        GuestExecutionSessionBundle* output, std::string* error,
                        const GuestExecutionSessionBundleLimits& limits) {
  *output = {};
  if (requested_directory.empty() || requested_directory == "." ||
      requested_directory == ".." ||
      requested_directory == requested_directory.root_path() ||
      requested_directory.filename().empty() ||
      HasParentTraversal(requested_directory) ||
      (!allow_staging_root && HasStagingSuffix(requested_directory))) {
    return Fail(error, "session bundle input directory is unsafe");
  }
  std::error_code filesystem_error;
  const std::filesystem::file_status root_status =
      std::filesystem::symlink_status(requested_directory, filesystem_error);
  if (filesystem_error ||
      root_status.type() != std::filesystem::file_type::directory) {
    return Fail(error,
                "session bundle root is missing, a symlink or not a directory");
  }
  const std::filesystem::path directory =
      std::filesystem::canonical(requested_directory, filesystem_error);
  if (filesystem_error) {
    return Fail(error, "failed to resolve the session bundle directory");
  }

  uint64_t maximum_entries = 1;
  if (!CheckedAdd(maximum_entries, limits.session.maximum_chunks,
                  &maximum_entries) ||
      !CheckedAdd(maximum_entries, limits.maximum_content_blobs,
                  &maximum_entries)) {
    return Fail(error, "session bundle file-count limit overflows");
  }
  std::map<std::filesystem::path, std::filesystem::path> entries;
  if (!ListRegularEntries(directory, maximum_entries, &entries, error)) {
    return false;
  }

  const std::filesystem::path manifest_name =
      kGuestExecutionSessionBundleManifestFileName;
  const auto manifest_entry = entries.find(manifest_name);
  if (manifest_entry == entries.end()) {
    return Fail(error, "session bundle manifest is missing");
  }
  std::vector<uint8_t> manifest_bytes;
  if (!ReadRegularFile(manifest_entry->second,
                       std::min(limits.session.maximum_manifest_bytes,
                                limits.maximum_bundle_bytes),
                       false, 0, &manifest_bytes, error)) {
    return false;
  }
  GuestExecutionSessionBundle bundle;
  if (!GuestExecutionSessionCodec::DecodeManifest(
          manifest_bytes, &bundle.manifest, error, limits.session)) {
    return false;
  }

  uint64_t total_bundle_bytes = manifest_bytes.size();
  std::set<std::filesystem::path> expected_names = {manifest_name};
  bundle.chunks.resize(bundle.manifest.chunks.size());
  for (size_t i = 0; i < bundle.manifest.chunks.size(); ++i) {
    const GuestExecutionSessionChunkReference& reference =
        bundle.manifest.chunks[i];
    const std::filesystem::path name = ChunkFileName(reference);
    if (!expected_names.insert(name).second) {
      return Fail(error, "session bundle generated duplicate chunk names");
    }
    const auto entry = entries.find(name);
    const uint64_t remaining_bundle_bytes =
        total_bundle_bytes <= limits.maximum_bundle_bytes
            ? limits.maximum_bundle_bytes - total_bundle_bytes
            : 0;
    if (entry == entries.end() ||
        !ReadRegularFile(entry->second,
                         std::min(limits.session.maximum_chunk_bytes,
                                  remaining_bundle_bytes),
                         true, reference.encoded_size, &bundle.chunks[i],
                         error) ||
        GuestExecutionSessionCodec::HashBytes(bundle.chunks[i]) !=
            reference.encoded_sha256 ||
        !CheckedAdd(total_bundle_bytes, bundle.chunks[i].size(),
                    &total_bundle_bytes) ||
        total_bundle_bytes > limits.maximum_bundle_bytes) {
      if (error && error->empty()) {
        error->assign("session bundle chunk is missing or substituted");
      }
      return false;
    }
  }
  if (!GuestExecutionSessionCodec::ValidateSession(
          bundle.manifest, bundle.chunks, error, limits.session)) {
    return false;
  }

  std::map<GuestExecutionSessionSha256, RequiredBlob> requirements;
  if (!CollectRequiredBlobs(bundle.manifest, bundle.chunks, limits,
                            &requirements, error)) {
    return false;
  }
  uint64_t total_content_bytes = 0;
  bundle.content_blobs.reserve(requirements.size());
  for (const auto& [digest, requirement] : requirements) {
    const std::filesystem::path name = BlobFileName(digest);
    if (!expected_names.insert(name).second) {
      return Fail(error, "session bundle generated duplicate blob names");
    }
    const auto entry = entries.find(name);
    GuestExecutionSessionContentBlob blob;
    blob.sha256 = digest;
    const uint64_t remaining_content_bytes =
        total_content_bytes <= limits.maximum_total_content_bytes
            ? limits.maximum_total_content_bytes - total_content_bytes
            : 0;
    const uint64_t remaining_bundle_bytes =
        total_bundle_bytes <= limits.maximum_bundle_bytes
            ? limits.maximum_bundle_bytes - total_bundle_bytes
            : 0;
    const uint64_t maximum_blob_bytes =
        std::min({limits.session.maximum_content_blob_bytes,
                  remaining_content_bytes, remaining_bundle_bytes});
    if (entry == entries.end() ||
        !ReadRegularFile(entry->second, maximum_blob_bytes,
                         requirement.has_expected_size,
                         requirement.expected_size, &blob.bytes, error) ||
        blob.bytes.empty() ||
        GuestExecutionSessionCodec::HashBytes(blob.bytes) != digest ||
        !CheckedAdd(total_content_bytes, blob.bytes.size(),
                    &total_content_bytes) ||
        total_content_bytes > limits.maximum_total_content_bytes ||
        !CheckedAdd(total_bundle_bytes, blob.bytes.size(),
                    &total_bundle_bytes) ||
        total_bundle_bytes > limits.maximum_bundle_bytes) {
      if (error && error->empty()) {
        error->assign("session bundle blob is missing or substituted");
      }
      return false;
    }
    bundle.content_blobs.push_back(std::move(blob));
  }
  if (entries.size() != expected_names.size()) {
    return Fail(error, "session bundle contains an extra filesystem object");
  }
  for (const auto& [name, path] : entries) {
    (void)path;
    if (!expected_names.contains(name)) {
      return Fail(error, "session bundle contains an unexpected file name");
    }
  }
  ValidatedBundle validated;
  if (!ValidateBundle(bundle, limits, &validated, error)) {
    return false;
  }
  *output = std::move(bundle);
  return true;
}

}  // namespace

bool ValidateGuestExecutionSessionBundle(
    const GuestExecutionSessionBundle& bundle, std::string* error,
    GuestExecutionSessionBundleLimits limits) {
  if (error) {
    error->clear();
  }
  try {
    ValidatedBundle validated;
    return ValidateBundle(bundle, limits, &validated, error);
  } catch (const std::bad_alloc&) {
    return Fail(error, "session bundle validation allocation failed");
  } catch (const std::length_error&) {
    return Fail(error, "session bundle validation collection size is invalid");
  }
}

bool WriteGuestExecutionSessionBundle(
    const std::filesystem::path& output_directory,
    const GuestExecutionSessionBundle& bundle, std::string* error,
    GuestExecutionSessionBundleLimits limits) try {
  if (error) {
    error->clear();
  }
  ValidatedBundle validated;
  if (!ValidateBundle(bundle, limits, &validated, error)) {
    return false;
  }

  std::filesystem::path resolved_output;
  if (!ResolveOutputDirectory(output_directory, &resolved_output, error)) {
    return false;
  }
  bool output_exists = false;
  if (!PathEntryExists(resolved_output, &output_exists, error) ||
      output_exists) {
    if (error && error->empty()) {
      error->assign("session bundle output directory already exists");
    }
    return false;
  }
  std::filesystem::path staging_directory = resolved_output;
  staging_directory += kStagingSuffix;
  bool staging_exists = false;
  if (!PathEntryExists(staging_directory, &staging_exists, error) ||
      staging_exists) {
    if (error && error->empty()) {
      error->assign("session bundle staging directory already exists");
    }
    return false;
  }

  std::error_code filesystem_error;
  if (!std::filesystem::create_directory(staging_directory, filesystem_error) ||
      filesystem_error) {
    return Fail(error, "failed to create the session bundle staging directory");
  }
  StagingDirectory staging_guard(staging_directory);

  if (!WriteBytesExclusiveDurable(
          staging_directory / kGuestExecutionSessionBundleManifestFileName,
          validated.manifest_bytes, error)) {
    return false;
  }
  for (size_t i = 0; i < bundle.manifest.chunks.size(); ++i) {
    if (!WriteBytesExclusiveDurable(
            staging_directory / ChunkFileName(bundle.manifest.chunks[i]),
            bundle.chunks[i], error)) {
      return false;
    }
  }
  for (const auto& [digest, blob] : validated.blobs) {
    if (!WriteBytesExclusiveDurable(staging_directory / BlobFileName(digest),
                                    blob->bytes, error)) {
      return false;
    }
  }
  if (!SyncDirectory(staging_directory, error)) {
    return false;
  }

  GuestExecutionSessionBundle written_bundle;
  if (!ReadBundleInternal(staging_directory, true, &written_bundle, error,
                          limits) ||
      written_bundle.manifest != bundle.manifest ||
      written_bundle.chunks != bundle.chunks) {
    if (error && error->empty()) {
      error->assign("written session bundle failed its complete readback");
    }
    return false;
  }
  for (const GuestExecutionSessionContentBlob& written_blob :
       written_bundle.content_blobs) {
    const auto original = validated.blobs.find(written_blob.sha256);
    if (original == validated.blobs.end() ||
        original->second->bytes != written_blob.bytes) {
      return Fail(error, "written session bundle blob changed during staging");
    }
  }

  if (!PublishDirectoryNoReplace(staging_directory, resolved_output, error)) {
    return false;
  }
  staging_guard.Release();
  return SyncDirectory(resolved_output.parent_path(), error);
} catch (const std::bad_alloc&) {
  return Fail(error, "session bundle publication allocation failed");
} catch (const std::length_error&) {
  return Fail(error, "session bundle publication collection size is invalid");
}

bool ReadGuestExecutionSessionBundle(
    const std::filesystem::path& bundle_directory,
    GuestExecutionSessionBundle* output, std::string* error,
    GuestExecutionSessionBundleLimits limits) {
  if (!output) {
    return Fail(error, "session bundle decoded output is null");
  }
  *output = {};
  if (error) {
    error->clear();
  }
  try {
    return ReadBundleInternal(bundle_directory, false, output, error, limits);
  } catch (const std::bad_alloc&) {
    return Fail(error, "session bundle read allocation failed");
  } catch (const std::length_error&) {
    return Fail(error, "session bundle read collection size is invalid");
  }
}

}  // namespace cpu
}  // namespace xe
