/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/guest_invocation_artifact.h"

#include "third_party/fmt/include/fmt/format.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <limits>
#include <string_view>
#include <utility>

#include "xenia/cpu/ppc/ppc_context.h"

namespace xe {
namespace cpu {
namespace ppc {

namespace {

constexpr std::array<uint8_t, 8> kMagic = {'X', 'E', 'P', 'P',
                                           'C', 'I', 'R', 0};
constexpr std::array<uint8_t, 8> kRegisterStateMagic = {'X', 'E', 'P', 'P',
                                                        'C', 'S', 'T', 0};
constexpr std::array<uint8_t, 8> kThreadCheckpointMagic = {'X', 'E', 'P', 'P',
                                                           'C', 'T', 'C', 0};
constexpr uint32_t kArtifactKnownFlags = 0;
constexpr uint32_t kRegisterStateKnownFlags = 0;
constexpr uint32_t kThreadCheckpointKnownFlags = 0;
constexpr uint32_t kRegisterStatePayloadSize =
    32 * sizeof(uint64_t) + 32 * sizeof(uint64_t) + 128 * 16 + 8 * 4 +
    3 * sizeof(uint64_t) + sizeof(uint32_t) + 16 + sizeof(uint32_t) + 4;
constexpr uint64_t kMinimumInvocationSize =
    GuestInvocationArtifactCodec::kInvocationHeaderSize +
    2ull * GuestInvocationArtifactCodec::kArchitecturalStateSize;

static_assert(sizeof(PPCContext_s::cr0) == 4);
static_assert(offsetof(PPCContext_s, cr7) + sizeof(PPCContext_s::cr7) -
                  offsetof(PPCContext_s, cr0) ==
              8 * 4);
static_assert(sizeof(PPCContext_s::v[0]) == 16);
static_assert(sizeof(PPCContext_s::vscr_vec) == 16);
static_assert(kRegisterStatePayloadSize ==
              GuestPPCRegisterStateCodec::kPayloadSize);
static_assert(8 + 4 + 4 + 8 + 4 + 4 == GuestPPCRegisterStateCodec::kHeaderSize);
static_assert(8 + 4 + 4 + 8 + 4 + 4 ==
              GuestPPCThreadCheckpointCodec::kHeaderSize);
static_assert(8 * sizeof(uint32_t) + sizeof(uint64_t) +
                  GuestPPCThreadCheckpointCodec::kReservationReservedSize +
                  GuestPPCThreadCheckpointCodec::kLifecycleReservedSize ==
              GuestPPCThreadCheckpointCodec::kMetadataSize);
static_assert(GuestPPCThreadCheckpointCodec::kReservationReservedSize %
                      sizeof(uint64_t) ==
                  0 &&
              GuestPPCThreadCheckpointCodec::kLifecycleReservedSize %
                      sizeof(uint64_t) ==
                  0);
static_assert(GuestPPCThreadCheckpointCodec::kRegisterStateOffset == 104);
static_assert(GuestPPCThreadCheckpointCodec::kEncodedSize == 2780);

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

bool CheckedMultiply(uint64_t left, uint64_t right, uint64_t* result) {
  if (left && right > std::numeric_limits<uint64_t>::max() / left) {
    return false;
  }
  *result = left * right;
  return true;
}

bool IsNonzeroHash(const std::array<uint8_t, 32>& hash) {
  return std::any_of(hash.cbegin(), hash.cend(),
                     [](uint8_t value) { return value != 0; });
}

bool InvocationWireSize(uint64_t input_page_count, uint64_t output_page_count,
                        uint64_t* size) {
  uint64_t page_count = 0;
  uint64_t page_bytes = 0;
  return CheckedAdd(input_page_count, output_page_count, &page_count) &&
         CheckedMultiply(page_count,
                         GuestInvocationArtifactCodec::kPageRecordSize,
                         &page_bytes) &&
         CheckedAdd(kMinimumInvocationSize, page_bytes, size);
}

bool ValidatePages(const std::vector<GuestInvocationPage>& pages,
                   std::string_view name, std::string* error) {
  if (pages.size() > GuestInvocationArtifactCodec::kMaxDataPagesPerInvocation) {
    return Fail(error, std::string(name) + " exceeds the page-count limit");
  }

  uint32_t previous_address = 0;
  bool has_previous_address = false;
  for (const GuestInvocationPage& page : pages) {
    if (page.guest_address & (GuestInvocationArtifactCodec::kPageSize - 1)) {
      return Fail(error,
                  std::string(name) + " contains an unaligned guest page");
    }
    if (!IsSupportedGuestDataPageAddress(page.guest_address)) {
      return Fail(error, std::string(name) +
                             " contains a page outside supported ordinary "
                             "virtual/XEX memory");
    }
    if (has_previous_address && page.guest_address <= previous_address) {
      return Fail(error, std::string(name) +
                             " must be strictly ordered without duplicate or "
                             "overlapping pages");
    }
    previous_address = page.guest_address;
    has_previous_address = true;
  }
  return true;
}

const GuestInvocationPage* FindPage(
    const std::vector<GuestInvocationPage>& pages, uint32_t guest_address) {
  const auto it =
      std::lower_bound(pages.cbegin(), pages.cend(), guest_address,
                       [](const GuestInvocationPage& page, uint32_t address) {
                         return page.guest_address < address;
                       });
  return it != pages.cend() && it->guest_address == guest_address ? &*it
                                                                  : nullptr;
}

bool ValidateInvocation(const GuestFunctionInvocation& invocation,
                        std::string* error) {
  if (invocation.dependency_flags & ~kGuestInvocationKnownDependencyMask) {
    return Fail(error, "invocation contains unknown dependency flags");
  }
  if (invocation.dependency_flags) {
    return Fail(error,
                "invocation has dependencies unsupported by standalone replay");
  }

  if (!invocation.function_address || (invocation.function_address & 3) ||
      (invocation.function_end_address & 3) ||
      invocation.function_end_address < invocation.function_address) {
    return Fail(error, "invocation has an invalid function range");
  }
  const uint64_t function_size = uint64_t(invocation.function_end_address) -
                                 invocation.function_address + 4;
  if (function_size > GuestInvocationArtifactCodec::kMaxFunctionSize) {
    return Fail(error, "invocation function range exceeds the size limit");
  }
  if (invocation.entry_address != invocation.function_address) {
    return Fail(error,
                "version 1 invocation entry must equal the function start");
  }
  if (!invocation.expected_return_address ||
      (invocation.expected_return_address & 3) ||
      (invocation.expected_return_address >= invocation.function_address &&
       invocation.expected_return_address <= invocation.function_end_address)) {
    return Fail(error, "invocation has an invalid return boundary");
  }
  if (invocation.input.link_register != invocation.expected_return_address) {
    return Fail(error,
                "invocation input LR does not match its return boundary");
  }

  if (!ValidatePages(invocation.input_data_pages, "input_data_pages", error) ||
      !ValidatePages(invocation.expected_dirty_pages, "expected_dirty_pages",
                     error)) {
    return false;
  }

  for (const GuestInvocationPage& page : invocation.expected_dirty_pages) {
    const GuestInvocationPage* input_page =
        FindPage(invocation.input_data_pages, page.guest_address);
    if (!input_page) {
      return Fail(error,
                  "expected_dirty_pages contains a page without initial data");
    }
    if (input_page->data == page.data) {
      return Fail(error,
                  "expected_dirty_pages contains an unchanged data page");
    }
  }
  return true;
}

bool ValidateThreadCheckpointRoute(
    uint32_t guest_thread_id, GuestPPCThreadResumeKind resume_kind,
    uint32_t resume_pc, uint32_t owning_function_address,
    uint32_t owning_function_end_address, uint32_t outer_guest_return_address,
    uint64_t pending_external_event_sequence,
    uint32_t pending_export_guest_address, std::string* error) {
  if (!guest_thread_id) {
    return Fail(error, "thread checkpoint guest thread ID is zero");
  }
  if (resume_kind == GuestPPCThreadResumeKind::kOutsideGuest) {
    if (resume_pc || owning_function_address || owning_function_end_address ||
        outer_guest_return_address || pending_external_event_sequence ||
        pending_export_guest_address) {
      return Fail(error,
                  "outside-guest checkpoint contains an executable route");
    }
    return true;
  }
  if (!resume_pc || (resume_pc & 3)) {
    return Fail(error, "thread checkpoint resume PC is invalid");
  }
  if (!owning_function_address || (owning_function_address & 3) ||
      (owning_function_end_address & 3) ||
      owning_function_end_address < owning_function_address) {
    return Fail(error, "thread checkpoint owning function extent is invalid");
  }
  if (resume_pc < owning_function_address ||
      resume_pc > owning_function_end_address) {
    return Fail(error,
                "thread checkpoint resume PC is outside its owning function");
  }
  if (!outer_guest_return_address || (outer_guest_return_address & 3)) {
    return Fail(error, "thread checkpoint outer return boundary is invalid");
  }

  switch (resume_kind) {
    case GuestPPCThreadResumeKind::kGuestBlockHead:
      if (pending_external_event_sequence || pending_export_guest_address) {
        return Fail(error,
                    "block-head checkpoint contains pending extern identity");
      }
      return true;
    case GuestPPCThreadResumeKind::kPendingModeledBlockingExtern:
      if (!pending_external_event_sequence || !pending_export_guest_address ||
          (pending_export_guest_address & 3)) {
        return Fail(
            error,
            "pending-extern checkpoint has invalid event/export identity");
      }
      return true;
    default:
      return Fail(error, "thread checkpoint resume kind is unsupported");
  }
}

bool ValidateThreadCheckpoint(const GuestPPCThreadCheckpoint& checkpoint,
                              std::string* error) {
  return ValidateThreadCheckpointRoute(
      checkpoint.guest_thread_id, checkpoint.resume_kind, checkpoint.resume_pc,
      checkpoint.owning_function_address,
      checkpoint.owning_function_end_address,
      checkpoint.outer_guest_return_address,
      checkpoint.pending_external_event_sequence,
      checkpoint.pending_export_guest_address, error);
}

bool ValidateArtifact(const GuestInvocationArtifact& artifact,
                      uint64_t* encoded_size, std::string* error) {
  if (!IsNonzeroHash(artifact.capture_build_sha256)) {
    return Fail(error, "capture build SHA-256 is missing");
  }
  if (!IsNonzeroHash(artifact.code_corpus_sha256)) {
    return Fail(error, "code corpus SHA-256 is missing");
  }
  if (!IsNonzeroHash(artifact.replay_config_sha256)) {
    return Fail(error, "replay configuration SHA-256 is missing");
  }
  if (artifact.invocations.empty()) {
    return Fail(error, "artifact contains no invocations");
  }
  if (artifact.invocations.size() >
      GuestInvocationArtifactCodec::kMaxInvocations) {
    return Fail(error, "artifact exceeds the invocation-count limit");
  }

  uint64_t total_size = GuestInvocationArtifactCodec::kHeaderSize;
  for (const GuestFunctionInvocation& invocation : artifact.invocations) {
    if (!ValidateInvocation(invocation, error)) {
      return false;
    }
    uint64_t invocation_size = 0;
    if (!InvocationWireSize(invocation.input_data_pages.size(),
                            invocation.expected_dirty_pages.size(),
                            &invocation_size) ||
        !CheckedAdd(total_size, invocation_size, &total_size)) {
      return Fail(error, "artifact encoded size overflows");
    }
  }
  if (total_size > GuestInvocationArtifactCodec::kMaxArtifactSize) {
    return Fail(error, "artifact exceeds the encoded-size limit");
  }
  *encoded_size = total_size;
  return true;
}

class Writer {
 public:
  explicit Writer(size_t capacity) { data_.reserve(capacity); }

  void WriteU8(uint8_t value) { data_.push_back(value); }

  void WriteU32(uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) {
      WriteU8(static_cast<uint8_t>(value >> (i * 8)));
    }
  }

  void WriteU64(uint64_t value) {
    for (uint32_t i = 0; i < 8; ++i) {
      WriteU8(static_cast<uint8_t>(value >> (i * 8)));
    }
  }

  void WriteBytes(const uint8_t* data, size_t size) {
    data_.insert(data_.end(), data, data + size);
  }

  std::vector<uint8_t> TakeData() { return std::move(data_); }

 private:
  std::vector<uint8_t> data_;
};

class Reader {
 public:
  Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

  size_t remaining() const { return size_ - offset_; }

  bool ReadU8(uint8_t* value) { return ReadBytes(value, sizeof(*value)); }

  bool ReadU32(uint32_t* value) {
    uint8_t bytes[4];
    if (!ReadBytes(bytes, sizeof(bytes))) {
      return false;
    }
    *value = uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) |
             (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
    return true;
  }

  bool ReadU64(uint64_t* value) {
    uint8_t bytes[8];
    if (!ReadBytes(bytes, sizeof(bytes))) {
      return false;
    }
    *value = uint64_t(bytes[0]) | (uint64_t(bytes[1]) << 8) |
             (uint64_t(bytes[2]) << 16) | (uint64_t(bytes[3]) << 24) |
             (uint64_t(bytes[4]) << 32) | (uint64_t(bytes[5]) << 40) |
             (uint64_t(bytes[6]) << 48) | (uint64_t(bytes[7]) << 56);
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

void WriteRegisterStatePayload(Writer* writer,
                               const GuestPPCRegisterState& state) {
  for (uint64_t value : state.gpr) {
    writer->WriteU64(value);
  }
  for (uint64_t value : state.fpr_bits) {
    writer->WriteU64(value);
  }
  for (const auto& value : state.vector_registers) {
    writer->WriteBytes(value.data(), value.size());
  }
  for (const auto& value : state.condition_register_fields) {
    writer->WriteBytes(value.data(), value.size());
  }
  writer->WriteU64(state.link_register);
  writer->WriteU64(state.count_register);
  writer->WriteU64(state.machine_state_register);
  writer->WriteU32(state.fpscr);
  writer->WriteBytes(state.vscr_vector.data(), state.vscr_vector.size());
  writer->WriteU32(state.vrsave);
  writer->WriteU8(state.xer_ca);
  writer->WriteU8(state.xer_ov);
  writer->WriteU8(state.xer_so);
  writer->WriteU8(state.vscr_sat);
}

bool ReadRegisterStatePayload(Reader* reader, GuestPPCRegisterState* state) {
  for (uint64_t& value : state->gpr) {
    if (!reader->ReadU64(&value)) {
      return false;
    }
  }
  for (uint64_t& value : state->fpr_bits) {
    if (!reader->ReadU64(&value)) {
      return false;
    }
  }
  for (auto& value : state->vector_registers) {
    if (!reader->ReadBytes(value.data(), value.size())) {
      return false;
    }
  }
  for (auto& value : state->condition_register_fields) {
    if (!reader->ReadBytes(value.data(), value.size())) {
      return false;
    }
  }
  if (!reader->ReadU64(&state->link_register) ||
      !reader->ReadU64(&state->count_register) ||
      !reader->ReadU64(&state->machine_state_register) ||
      !reader->ReadU32(&state->fpscr) ||
      !reader->ReadBytes(state->vscr_vector.data(),
                         state->vscr_vector.size()) ||
      !reader->ReadU32(&state->vrsave) || !reader->ReadU8(&state->xer_ca) ||
      !reader->ReadU8(&state->xer_ov) || !reader->ReadU8(&state->xer_so) ||
      !reader->ReadU8(&state->vscr_sat)) {
    return false;
  }
  return true;
}

void WritePage(Writer* writer, const GuestInvocationPage& page) {
  writer->WriteU32(page.guest_address);
  writer->WriteBytes(page.data.data(), page.data.size());
}

bool ReadPage(Reader* reader, GuestInvocationPage* page) {
  return reader->ReadU32(&page->guest_address) &&
         reader->ReadBytes(page->data.data(), page->data.size());
}

}  // namespace

std::string DescribeGuestPPCRegisterStateDifference(
    const GuestPPCRegisterState& expected,
    const GuestPPCRegisterState& actual) {
  std::vector<std::string> differences;
  const auto note = [&differences](std::string text) {
    if (differences.size() < 6) {
      differences.push_back(std::move(text));
    }
  };
  for (size_t i = 0; i < expected.gpr.size(); ++i) {
    if (expected.gpr[i] != actual.gpr[i]) {
      note(fmt::format("r{}={:016X} want {:016X}", i, actual.gpr[i],
                       expected.gpr[i]));
    }
  }
  for (size_t i = 0; i < expected.fpr_bits.size(); ++i) {
    if (expected.fpr_bits[i] != actual.fpr_bits[i]) {
      note(fmt::format("f{}={:016X} want {:016X}", i, actual.fpr_bits[i],
                       expected.fpr_bits[i]));
    }
  }
  for (size_t i = 0; i < expected.vector_registers.size(); ++i) {
    if (expected.vector_registers[i] != actual.vector_registers[i]) {
      note(fmt::format("v{}", i));
    }
  }
  const auto field_bytes = [](const std::array<uint8_t, 4>& field) {
    return fmt::format("{:02X}{:02X}{:02X}{:02X}", field[0], field[1], field[2],
                       field[3]);
  };
  for (size_t i = 0; i < expected.condition_register_fields.size(); ++i) {
    if (expected.condition_register_fields[i] !=
        actual.condition_register_fields[i]) {
      note(fmt::format("cr{}={} want {}", i,
                       field_bytes(actual.condition_register_fields[i]),
                       field_bytes(expected.condition_register_fields[i])));
    }
  }
  const auto scalar = [&note](const char* name, uint64_t got, uint64_t want) {
    if (got != want) {
      note(fmt::format("{}={:X} want {:X}", name, got, want));
    }
  };
  scalar("lr", actual.link_register, expected.link_register);
  scalar("ctr", actual.count_register, expected.count_register);
  scalar("msr", actual.machine_state_register, expected.machine_state_register);
  scalar("fpscr", actual.fpscr, expected.fpscr);
  scalar("vrsave", actual.vrsave, expected.vrsave);
  scalar("xer_ca", actual.xer_ca, expected.xer_ca);
  scalar("xer_ov", actual.xer_ov, expected.xer_ov);
  scalar("xer_so", actual.xer_so, expected.xer_so);
  scalar("vscr_sat", actual.vscr_sat, expected.vscr_sat);
  if (expected.vscr_vector != actual.vscr_vector) {
    note("vscr");
  }
  if (differences.empty()) {
    return "no architectural field differs";
  }
  std::string text = differences[0];
  for (size_t i = 1; i < differences.size(); ++i) {
    text += ", " + differences[i];
  }
  return text;
}

GuestPPCRegisterState CaptureGuestPPCRegisterState(
    const PPCContext_s& context) {
  GuestPPCRegisterState state;
  std::copy(std::begin(context.r), std::end(context.r), state.gpr.begin());
  for (size_t i = 0; i < state.fpr_bits.size(); ++i) {
    state.fpr_bits[i] = std::bit_cast<uint64_t>(context.f[i]);
  }
  for (size_t i = 0; i < state.vector_registers.size(); ++i) {
    std::memcpy(state.vector_registers[i].data(), &context.v[i],
                state.vector_registers[i].size());
  }
  const uint8_t* condition_register_bytes =
      reinterpret_cast<const uint8_t*>(&context.cr0);
  for (size_t i = 0; i < state.condition_register_fields.size(); ++i) {
    std::memcpy(state.condition_register_fields[i].data(),
                condition_register_bytes + i * 4,
                state.condition_register_fields[i].size());
  }
  state.link_register = context.lr;
  state.count_register = context.ctr;
  state.machine_state_register = context.msr;
  state.fpscr = context.fpscr.value;
  std::memcpy(state.vscr_vector.data(), &context.vscr_vec,
              state.vscr_vector.size());
  state.vrsave = context.vrsave;
  state.xer_ca = context.xer_ca;
  state.xer_ov = context.xer_ov;
  state.xer_so = context.xer_so;
  state.vscr_sat = context.vscr_sat;
  return state;
}

void RestoreGuestPPCRegisterState(const GuestPPCRegisterState& state,
                                  PPCContext_s* context) {
  if (!context) {
    return;
  }
  std::copy(state.gpr.cbegin(), state.gpr.cend(), std::begin(context->r));
  for (size_t i = 0; i < state.fpr_bits.size(); ++i) {
    context->f[i] = std::bit_cast<double>(state.fpr_bits[i]);
  }
  for (size_t i = 0; i < state.vector_registers.size(); ++i) {
    std::memcpy(&context->v[i], state.vector_registers[i].data(),
                state.vector_registers[i].size());
  }
  uint8_t* condition_register_bytes = reinterpret_cast<uint8_t*>(&context->cr0);
  for (size_t i = 0; i < state.condition_register_fields.size(); ++i) {
    std::memcpy(condition_register_bytes + i * 4,
                state.condition_register_fields[i].data(),
                state.condition_register_fields[i].size());
  }
  context->lr = state.link_register;
  context->ctr = state.count_register;
  context->msr = state.machine_state_register;
  context->fpscr.value = state.fpscr;
  std::memcpy(&context->vscr_vec, state.vscr_vector.data(),
              state.vscr_vector.size());
  context->vrsave = state.vrsave;
  context->xer_ca = state.xer_ca;
  context->xer_ov = state.xer_ov;
  context->xer_so = state.xer_so;
  context->vscr_sat = state.vscr_sat;
}

bool GuestPPCRegisterStateCodec::Encode(const GuestPPCRegisterState& state,
                                        std::vector<uint8_t>* output,
                                        std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "output vector is null");
  }
  output->clear();

  Writer writer(kEncodedSize);
  writer.WriteBytes(kRegisterStateMagic.data(), kRegisterStateMagic.size());
  writer.WriteU32(kVersion);
  writer.WriteU32(kHeaderSize);
  writer.WriteU64(kEncodedSize);
  writer.WriteU32(kRegisterStateKnownFlags);
  writer.WriteU32(0);
  WriteRegisterStatePayload(&writer, state);

  *output = writer.TakeData();
  if (output->size() != kEncodedSize) {
    output->clear();
    return Fail(error, "internal encoded-size mismatch");
  }
  return true;
}

bool GuestPPCRegisterStateCodec::Decode(const uint8_t* data, size_t data_size,
                                        GuestPPCRegisterState* output,
                                        std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "output register state is null");
  }
  *output = {};
  if (!data && data_size) {
    return Fail(error, "input data is null");
  }

  Reader reader(data, data_size);
  std::array<uint8_t, kRegisterStateMagic.size()> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint64_t encoded_size = 0;
  uint32_t flags = 0;
  uint32_t reserved = 0;
  if (!reader.ReadBytes(magic.data(), magic.size()) ||
      !reader.ReadU32(&version) || !reader.ReadU32(&header_size) ||
      !reader.ReadU64(&encoded_size) || !reader.ReadU32(&flags) ||
      !reader.ReadU32(&reserved)) {
    return Fail(error, "register-state header is truncated");
  }
  if (magic != kRegisterStateMagic) {
    return Fail(error, "register-state magic is invalid");
  }
  if (version != kVersion) {
    return Fail(error, "register-state version is unsupported");
  }
  if (header_size != kHeaderSize) {
    return Fail(error, "register-state header size is unsupported");
  }
  if (encoded_size != kEncodedSize) {
    return Fail(error, "register-state encoded size is noncanonical");
  }
  if (flags & ~kRegisterStateKnownFlags) {
    return Fail(error, "register-state contains unknown flags");
  }
  if (reserved) {
    return Fail(error, "register-state reserved field is nonzero");
  }
  if (data_size != kEncodedSize) {
    return Fail(error, data_size < kEncodedSize
                           ? "register-state blob is truncated"
                           : "register-state blob has trailing data");
  }

  GuestPPCRegisterState state;
  if (!ReadRegisterStatePayload(&reader, &state)) {
    return Fail(error, "register-state payload is truncated");
  }
  if (reader.remaining()) {
    return Fail(error, "register-state blob has trailing data");
  }
  *output = std::move(state);
  return true;
}

bool GuestPPCThreadCheckpointCodec::Encode(
    const GuestPPCThreadCheckpoint& checkpoint, std::vector<uint8_t>* output,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "thread checkpoint encoded output is null");
  }
  output->clear();
  if (!ValidateThreadCheckpoint(checkpoint, error)) {
    return false;
  }

  std::vector<uint8_t> register_state;
  if (!GuestPPCRegisterStateCodec::Encode(checkpoint.registers, &register_state,
                                          error)) {
    return false;
  }
  if (register_state.size() != GuestPPCRegisterStateCodec::kEncodedSize) {
    return Fail(error,
                "thread checkpoint nested register size is noncanonical");
  }

  Writer writer(kEncodedSize);
  writer.WriteBytes(kThreadCheckpointMagic.data(),
                    kThreadCheckpointMagic.size());
  writer.WriteU32(kVersion);
  writer.WriteU32(kHeaderSize);
  writer.WriteU64(kEncodedSize);
  writer.WriteU32(kThreadCheckpointKnownFlags);
  writer.WriteU32(0);
  writer.WriteU32(checkpoint.participant_ordinal);
  writer.WriteU32(checkpoint.guest_thread_id);
  writer.WriteU32(static_cast<uint32_t>(checkpoint.resume_kind));
  writer.WriteU32(checkpoint.resume_pc);
  writer.WriteU32(checkpoint.owning_function_address);
  writer.WriteU32(checkpoint.owning_function_end_address);
  writer.WriteU32(checkpoint.outer_guest_return_address);
  writer.WriteU32(checkpoint.pending_export_guest_address);
  writer.WriteU64(checkpoint.pending_external_event_sequence);
  for (uint32_t i = 0; i < kReservationReservedSize / sizeof(uint64_t); ++i) {
    writer.WriteU64(0);
  }
  for (uint32_t i = 0; i < kLifecycleReservedSize / sizeof(uint64_t); ++i) {
    writer.WriteU64(0);
  }
  writer.WriteBytes(register_state.data(), register_state.size());

  *output = writer.TakeData();
  if (output->size() != kEncodedSize) {
    output->clear();
    return Fail(error, "thread checkpoint internal encoded-size mismatch");
  }
  return true;
}

bool GuestPPCThreadCheckpointCodec::Decode(const uint8_t* data,
                                           size_t data_size,
                                           GuestPPCThreadCheckpoint* output,
                                           std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "thread checkpoint decoded output is null");
  }
  if (!data && data_size) {
    return Fail(error, "thread checkpoint input data is null");
  }

  Reader reader(data, data_size);
  std::array<uint8_t, kThreadCheckpointMagic.size()> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint64_t encoded_size = 0;
  uint32_t flags = 0;
  uint32_t header_reserved = 0;
  if (!reader.ReadBytes(magic.data(), magic.size()) ||
      !reader.ReadU32(&version) || !reader.ReadU32(&header_size) ||
      !reader.ReadU64(&encoded_size) || !reader.ReadU32(&flags) ||
      !reader.ReadU32(&header_reserved)) {
    return Fail(error, "thread checkpoint header is truncated");
  }
  if (magic != kThreadCheckpointMagic) {
    return Fail(error, "thread checkpoint magic is invalid");
  }
  if (version != kVersion) {
    return Fail(error, "thread checkpoint version is unsupported");
  }
  if (header_size != kHeaderSize) {
    return Fail(error, "thread checkpoint header size is unsupported");
  }
  if (encoded_size != kEncodedSize) {
    return Fail(error, "thread checkpoint encoded size is noncanonical");
  }
  if (flags & ~kThreadCheckpointKnownFlags) {
    return Fail(error, "thread checkpoint contains unknown flags");
  }
  if (header_reserved) {
    return Fail(error, "thread checkpoint header reserved field is nonzero");
  }
  if (data_size != kEncodedSize) {
    return Fail(error, data_size < kEncodedSize
                           ? "thread checkpoint blob is truncated"
                           : "thread checkpoint blob has trailing data");
  }

  GuestPPCThreadCheckpoint checkpoint;
  uint32_t resume_kind = 0;
  std::array<uint64_t, kReservationReservedSize / sizeof(uint64_t)>
      reservation_reserved = {};
  std::array<uint64_t, kLifecycleReservedSize / sizeof(uint64_t)>
      lifecycle_reserved = {};
  if (!reader.ReadU32(&checkpoint.participant_ordinal) ||
      !reader.ReadU32(&checkpoint.guest_thread_id) ||
      !reader.ReadU32(&resume_kind) || !reader.ReadU32(&checkpoint.resume_pc) ||
      !reader.ReadU32(&checkpoint.owning_function_address) ||
      !reader.ReadU32(&checkpoint.owning_function_end_address) ||
      !reader.ReadU32(&checkpoint.outer_guest_return_address) ||
      !reader.ReadU32(&checkpoint.pending_export_guest_address) ||
      !reader.ReadU64(&checkpoint.pending_external_event_sequence)) {
    return Fail(error, "thread checkpoint metadata is truncated");
  }
  for (uint64_t& value : reservation_reserved) {
    if (!reader.ReadU64(&value)) {
      return Fail(error, "thread checkpoint reservation space is truncated");
    }
  }
  for (uint64_t& value : lifecycle_reserved) {
    if (!reader.ReadU64(&value)) {
      return Fail(error, "thread checkpoint lifecycle space is truncated");
    }
  }
  if (std::any_of(reservation_reserved.cbegin(), reservation_reserved.cend(),
                  [](uint64_t value) { return value != 0; })) {
    return Fail(error, "thread checkpoint reservation space is nonzero");
  }
  if (std::any_of(lifecycle_reserved.cbegin(), lifecycle_reserved.cend(),
                  [](uint64_t value) { return value != 0; })) {
    return Fail(error, "thread checkpoint lifecycle space is nonzero");
  }
  if (reader.remaining() != GuestPPCRegisterStateCodec::kEncodedSize) {
    return Fail(error,
                "thread checkpoint register-state extent is noncanonical");
  }

  checkpoint.resume_kind = static_cast<GuestPPCThreadResumeKind>(resume_kind);
  if (!ValidateThreadCheckpoint(checkpoint, error)) {
    return false;
  }

  std::string register_error;
  if (!GuestPPCRegisterStateCodec::Decode(
          data + kRegisterStateOffset, GuestPPCRegisterStateCodec::kEncodedSize,
          &checkpoint.registers, &register_error)) {
    return Fail(error, "thread checkpoint nested register blob is invalid: " +
                           register_error);
  }

  *output = std::move(checkpoint);
  return true;
}

bool GuestPPCThreadCheckpointCodec::ValidateBinding(
    const GuestPPCThreadCheckpoint& checkpoint,
    const GuestPPCThreadCheckpointBinding& binding, std::string* error) {
  if (error) {
    error->clear();
  }
  if (!ValidateThreadCheckpoint(checkpoint, error) ||
      !ValidateThreadCheckpointRoute(
          binding.guest_thread_id, binding.resume_kind, binding.resume_pc,
          binding.owning_function_address, binding.owning_function_end_address,
          binding.outer_guest_return_address,
          binding.pending_external_event_sequence,
          binding.pending_export_guest_address, error)) {
    return false;
  }
  if (checkpoint.participant_ordinal != binding.participant_ordinal ||
      checkpoint.guest_thread_id != binding.guest_thread_id) {
    return Fail(error, "thread checkpoint participant identity mismatch");
  }
  if (checkpoint.resume_kind != binding.resume_kind ||
      checkpoint.resume_pc != binding.resume_pc ||
      checkpoint.owning_function_address != binding.owning_function_address ||
      checkpoint.owning_function_end_address !=
          binding.owning_function_end_address ||
      checkpoint.outer_guest_return_address !=
          binding.outer_guest_return_address) {
    return Fail(error, "thread checkpoint resume catalog binding mismatch");
  }
  if (checkpoint.pending_external_event_sequence !=
          binding.pending_external_event_sequence ||
      checkpoint.pending_export_guest_address !=
          binding.pending_export_guest_address) {
    return Fail(error, "thread checkpoint pending extern binding mismatch");
  }
  return true;
}

bool GuestInvocationArtifactCodec::Encode(
    const GuestInvocationArtifact& artifact, std::vector<uint8_t>* output,
    std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "output vector is null");
  }
  output->clear();

  uint64_t encoded_size = 0;
  if (!ValidateArtifact(artifact, &encoded_size, error)) {
    return false;
  }

  Writer writer(static_cast<size_t>(encoded_size));
  writer.WriteBytes(kMagic.data(), kMagic.size());
  writer.WriteU32(kVersion);
  writer.WriteU32(kHeaderSize);
  writer.WriteU32(kPageSize);
  writer.WriteU32(kArtifactKnownFlags);
  writer.WriteU64(encoded_size);
  writer.WriteU32(static_cast<uint32_t>(artifact.invocations.size()));
  writer.WriteU32(0);
  writer.WriteBytes(artifact.capture_build_sha256.data(),
                    artifact.capture_build_sha256.size());
  writer.WriteBytes(artifact.code_corpus_sha256.data(),
                    artifact.code_corpus_sha256.size());
  writer.WriteBytes(artifact.replay_config_sha256.data(),
                    artifact.replay_config_sha256.size());

  for (const GuestFunctionInvocation& invocation : artifact.invocations) {
    uint64_t invocation_size = 0;
    InvocationWireSize(invocation.input_data_pages.size(),
                       invocation.expected_dirty_pages.size(),
                       &invocation_size);
    writer.WriteU64(invocation_size);
    writer.WriteU32(invocation.function_address);
    writer.WriteU32(invocation.function_end_address);
    writer.WriteU32(invocation.entry_address);
    writer.WriteU32(invocation.expected_return_address);
    writer.WriteU32(invocation.dependency_flags);
    writer.WriteU32(static_cast<uint32_t>(invocation.input_data_pages.size()));
    writer.WriteU32(
        static_cast<uint32_t>(invocation.expected_dirty_pages.size()));
    writer.WriteU32(0);
    WriteRegisterStatePayload(&writer, invocation.input);
    WriteRegisterStatePayload(&writer, invocation.expected_output);
    for (const GuestInvocationPage& page : invocation.input_data_pages) {
      WritePage(&writer, page);
    }
    for (const GuestInvocationPage& page : invocation.expected_dirty_pages) {
      WritePage(&writer, page);
    }
  }

  *output = writer.TakeData();
  if (output->size() != encoded_size) {
    output->clear();
    return Fail(error, "internal encoded-size mismatch");
  }
  return true;
}

bool GuestInvocationArtifactCodec::Decode(const uint8_t* data, size_t data_size,
                                          GuestInvocationArtifact* output,
                                          std::string* error) {
  if (error) {
    error->clear();
  }
  if (!output) {
    return Fail(error, "output artifact is null");
  }
  *output = {};
  if (!data && data_size) {
    return Fail(error, "input data is null");
  }
  if (data_size > kMaxArtifactSize) {
    return Fail(error, "artifact exceeds the encoded-size limit");
  }

  Reader reader(data, data_size);
  std::array<uint8_t, kMagic.size()> magic = {};
  uint32_t version = 0;
  uint32_t header_size = 0;
  uint32_t page_size = 0;
  uint32_t artifact_flags = 0;
  uint64_t encoded_size = 0;
  uint32_t invocation_count = 0;
  uint32_t reserved = 0;
  GuestInvocationArtifact artifact;
  if (!reader.ReadBytes(magic.data(), magic.size()) ||
      !reader.ReadU32(&version) || !reader.ReadU32(&header_size) ||
      !reader.ReadU32(&page_size) || !reader.ReadU32(&artifact_flags) ||
      !reader.ReadU64(&encoded_size) || !reader.ReadU32(&invocation_count) ||
      !reader.ReadU32(&reserved) ||
      !reader.ReadBytes(artifact.capture_build_sha256.data(),
                        artifact.capture_build_sha256.size()) ||
      !reader.ReadBytes(artifact.code_corpus_sha256.data(),
                        artifact.code_corpus_sha256.size()) ||
      !reader.ReadBytes(artifact.replay_config_sha256.data(),
                        artifact.replay_config_sha256.size())) {
    return Fail(error, "artifact header is truncated");
  }
  if (magic != kMagic) {
    return Fail(error, "artifact magic is invalid");
  }
  if (version != kVersion) {
    return Fail(error, "artifact version is unsupported");
  }
  if (header_size != kHeaderSize) {
    return Fail(error, "artifact header size is unsupported");
  }
  if (page_size != kPageSize) {
    return Fail(error, "artifact page size is unsupported");
  }
  if (artifact_flags & ~kArtifactKnownFlags) {
    return Fail(error, "artifact contains unknown flags");
  }
  if (reserved) {
    return Fail(error, "artifact reserved header field is nonzero");
  }
  if (encoded_size != data_size) {
    return Fail(error, encoded_size > data_size ? "artifact is truncated"
                                                : "artifact has trailing data");
  }
  if (!invocation_count || invocation_count > kMaxInvocations) {
    return Fail(error, "artifact invocation count is invalid");
  }
  if (uint64_t(invocation_count) >
      (encoded_size - kHeaderSize) / kMinimumInvocationSize) {
    return Fail(error, "artifact invocation count exceeds its encoded size");
  }
  if (!IsNonzeroHash(artifact.capture_build_sha256) ||
      !IsNonzeroHash(artifact.code_corpus_sha256) ||
      !IsNonzeroHash(artifact.replay_config_sha256)) {
    return Fail(error, "artifact is missing a required hash");
  }

  artifact.invocations.reserve(invocation_count);
  for (uint32_t invocation_index = 0; invocation_index < invocation_count;
       ++invocation_index) {
    uint64_t invocation_size = 0;
    uint32_t input_page_count = 0;
    uint32_t output_page_count = 0;
    GuestFunctionInvocation invocation;
    if (!reader.ReadU64(&invocation_size) ||
        !reader.ReadU32(&invocation.function_address) ||
        !reader.ReadU32(&invocation.function_end_address) ||
        !reader.ReadU32(&invocation.entry_address) ||
        !reader.ReadU32(&invocation.expected_return_address) ||
        !reader.ReadU32(&invocation.dependency_flags) ||
        !reader.ReadU32(&input_page_count) ||
        !reader.ReadU32(&output_page_count) || !reader.ReadU32(&reserved)) {
      return Fail(error, "invocation header is truncated");
    }
    if (reserved) {
      return Fail(error, "invocation reserved field is nonzero");
    }
    if (invocation.dependency_flags & ~kGuestInvocationKnownDependencyMask) {
      return Fail(error, "invocation contains unknown dependency flags");
    }
    if (invocation.dependency_flags) {
      return Fail(
          error,
          "invocation has dependencies unsupported by standalone replay");
    }
    if (input_page_count > kMaxDataPagesPerInvocation ||
        output_page_count > kMaxDataPagesPerInvocation) {
      return Fail(error, "invocation page count exceeds the limit");
    }
    uint64_t expected_invocation_size = 0;
    if (!InvocationWireSize(input_page_count, output_page_count,
                            &expected_invocation_size) ||
        invocation_size != expected_invocation_size) {
      return Fail(error, "invocation encoded size is invalid");
    }
    if (invocation_size - kInvocationHeaderSize > reader.remaining()) {
      return Fail(error, "invocation is truncated");
    }
    if (!ReadRegisterStatePayload(&reader, &invocation.input) ||
        !ReadRegisterStatePayload(&reader, &invocation.expected_output)) {
      return Fail(error, "invocation register state is truncated");
    }
    invocation.input_data_pages.resize(input_page_count);
    invocation.expected_dirty_pages.resize(output_page_count);
    for (GuestInvocationPage& page : invocation.input_data_pages) {
      if (!ReadPage(&reader, &page)) {
        return Fail(error, "invocation input page is truncated");
      }
    }
    for (GuestInvocationPage& page : invocation.expected_dirty_pages) {
      if (!ReadPage(&reader, &page)) {
        return Fail(error, "invocation expected page is truncated");
      }
    }
    if (!ValidateInvocation(invocation, error)) {
      return false;
    }
    artifact.invocations.push_back(std::move(invocation));
  }
  if (reader.remaining()) {
    return Fail(error, "artifact has trailing data");
  }

  uint64_t validated_size = 0;
  if (!ValidateArtifact(artifact, &validated_size, error)) {
    return false;
  }
  if (validated_size != encoded_size) {
    return Fail(error, "artifact size does not match decoded contents");
  }
  *output = std::move(artifact);
  return true;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
