/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/execution_jit_corpus.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "third_party/catch/include/catch.hpp"

namespace xe::cpu::test {

namespace {

void AppendU32(std::vector<uint8_t>* data, uint32_t value) {
  for (uint32_t i = 0; i < 4; ++i) {
    data->push_back(static_cast<uint8_t>(value >> (i * 8)));
  }
}

void SetU32(std::vector<uint8_t>* data, size_t offset, uint32_t value) {
  REQUIRE(offset + 4 <= data->size());
  for (uint32_t i = 0; i < 4; ++i) {
    (*data)[offset + i] = static_cast<uint8_t>(value >> (i * 8));
  }
}

std::vector<uint8_t> MakeHeader(
    uint32_t version = JitCorpus::kVersion,
    uint32_t config_flags = JitCorpus::kConfigGuestScheduler) {
  std::vector<uint8_t> data;
  AppendU32(&data, JitCorpus::kMagic);
  AppendU32(&data, version);
  AppendU32(&data, JitCorpus::kPageSize);
  AppendU32(&data, config_flags);
  return data;
}

void AppendPage(std::vector<uint8_t>* data, uint32_t address, uint8_t seed) {
  AppendU32(data, JitCorpus::kTagPage);
  AppendU32(data, address);
  for (uint32_t i = 0; i < JitCorpus::kPageSize; ++i) {
    data->push_back(static_cast<uint8_t>(seed + i * 17));
  }
}

void AppendFunction(std::vector<uint8_t>* data, uint32_t address,
                    uint32_t end_address, uint32_t host_code_size = 64,
                    uint32_t flags = 0) {
  AppendU32(data, JitCorpus::kTagFunction);
  AppendU32(data, address);
  AppendU32(data, end_address);
  AppendU32(data, host_code_size);
  AppendU32(data, flags);
}

uint32_t FunctionFlags(Function::Behavior behavior,
                       SaveRestoreType saverest_type, bool is_restore,
                       uint8_t saverest_index) {
  return JitCorpus::EncodeFunctionFlags(behavior, saverest_type, is_restore,
                                        saverest_index);
}

ExecutionJitCorpusBuilder::CodePage MakeCodePage(uint8_t seed) {
  ExecutionJitCorpusBuilder::CodePage page;
  for (size_t i = 0; i < page.size(); ++i) {
    page[i] = static_cast<uint8_t>(seed + i * 17);
  }
  return page;
}

std::vector<uint8_t> MakeSpanningCorpus() {
  std::vector<uint8_t> data = MakeHeader();
  // Streaming capture order need not be address order.
  AppendPage(&data, 0x82001000, 0x22);
  AppendPage(&data, 0x82000000, 0x11);
  AppendFunction(&data, 0x82000FFC, 0x82001004);
  return data;
}

std::vector<uint8_t> MakeSingleFunctionCorpus(uint32_t page_address,
                                              uint32_t function_address,
                                              uint32_t end_address,
                                              uint32_t function_flags = 0) {
  std::vector<uint8_t> data = MakeHeader();
  AppendPage(&data, page_address, 0x31);
  AppendFunction(&data, function_address, end_address, 64, function_flags);
  return data;
}

void RequireDecodeFailure(const std::vector<uint8_t>& data) {
  ExecutionJitCorpus output;
  std::string error;
  REQUIRE_FALSE(ExecutionJitCorpus::Decode(data, &output, &error));
  REQUIRE_FALSE(error.empty());
  REQUIRE(output.page_addresses().empty());
  REQUIRE(output.functions().empty());
}

}  // namespace

TEST_CASE("execution JIT corpus decodes exact pages, extents and entries",
          "[execution-jit-corpus]") {
  ExecutionJitCorpus corpus;
  std::string error;
  REQUIRE(ExecutionJitCorpus::Decode(MakeSpanningCorpus(), &corpus, &error));
  REQUIRE(error.empty());
  REQUIRE(corpus.version() == JitCorpus::kVersion);
  REQUIRE(corpus.config_flags() == JitCorpus::kConfigGuestScheduler);
  REQUIRE(corpus.page_addresses() ==
          std::vector<uint32_t>{0x82000000, 0x82001000});
  REQUIRE(corpus.page_data().size() == 2 * JitCorpus::kPageSize);
  REQUIRE(corpus.FindPageData(0x82000000));
  REQUIRE(corpus.FindPageData(0x82000000)[0] == 0x11);
  REQUIRE(corpus.FindPageData(0x82001000)[0] == 0x22);
  REQUIRE_FALSE(corpus.FindPageData(0x82002000));

  const auto* function = corpus.FindFunction(0x82000FFC);
  REQUIRE(function);
  REQUIRE(function->end_address == 0x82001004);
  REQUIRE_FALSE(corpus.FindFunction(0x82001000));
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{0x82000FFC});

  ExecutionJitCorpus boundary_corpus;
  REQUIRE(ExecutionJitCorpus::Decode(
      MakeSingleFunctionCorpus(0x80040000, 0x80040000, 0x80040004),
      &boundary_corpus));
  REQUIRE(ExecutionJitCorpus::Decode(
      MakeSingleFunctionCorpus(0x9FFFE000, 0x9FFFEFF8, 0x9FFFEFFC),
      &boundary_corpus));
}

TEST_CASE("execution JIT corpus preserves definition order and metadata",
          "[execution-jit-corpus]") {
  std::vector<uint8_t> encoded = MakeHeader();
  AppendPage(&encoded, 0x82000000, 0x11);
  AppendPage(&encoded, 0x83000000, 0x22);
  AppendFunction(&encoded, 0x83000010, 0x8300001C, 64,
                 FunctionFlags(Function::Behavior::kProlog,
                               SaveRestoreType::FPR, false, 31));
  AppendFunction(&encoded, 0x82000000, 0x82000004, 64,
                 FunctionFlags(Function::Behavior::kEpilogReturn,
                               SaveRestoreType::GPR, true, 14));

  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus));
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{0x83000010, 0x82000000});
  REQUIRE(corpus.functions().size() == 2);
  REQUIRE(corpus.functions()[0].address == 0x82000000);
  REQUIRE(corpus.functions()[1].address == 0x83000010);

  JitCorpus::FunctionMetadata metadata;
  REQUIRE(JitCorpus::DecodeFunctionFlags(corpus.FindFunction(0x83000010)->flags,
                                         &metadata));
  REQUIRE(metadata.behavior == Function::Behavior::kProlog);
  REQUIRE(metadata.saverest_type == SaveRestoreType::FPR);
  REQUIRE_FALSE(metadata.is_restore);
  REQUIRE(metadata.saverest_index == 31);

  REQUIRE(JitCorpus::DecodeFunctionFlags(corpus.FindFunction(0x82000000)->flags,
                                         &metadata));
  REQUIRE(metadata.behavior == Function::Behavior::kEpilogReturn);
  REQUIRE(metadata.saverest_type == SaveRestoreType::GPR);
  REQUIRE(metadata.is_restore);
  REQUIRE(metadata.saverest_index == 14);
}

TEST_CASE(
    "execution JIT corpus builder round trips the full definition closure",
    "[execution-jit-corpus][execution-jit-corpus-builder]") {
  const ExecutionJitCorpusBuilder::CodePage low_page = MakeCodePage(0x11);
  const ExecutionJitCorpusBuilder::CodePage high_page = MakeCodePage(0x22);
  const JitCorpus::FunctionRecord helper = {
      0x83000020, 0x8300002C, 113,
      FunctionFlags(Function::Behavior::kProlog, SaveRestoreType::FPR, false,
                    31)};
  const JitCorpus::FunctionRecord static_callee = {0x83000100, 0x8300010C, 79,
                                                   0};
  const JitCorpus::FunctionRecord root = {0x82000000, 0x82000008, 61, 0};

  ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
  std::string error;
  // Page insertion order is deliberately noncanonical. Definitions retain
  // successful-definition order, including the helper and static callee that
  // an invocation recorder may need even when neither was entered.
  REQUIRE(builder.AddCodePage(0x83000000, high_page.data(), high_page.size(),
                              &error));
  REQUIRE(builder.AddCodePage(0x82000000, low_page.data(), low_page.size(),
                              &error));
  REQUIRE(builder.AddFunction(helper, &error));
  REQUIRE(builder.AddFunction(static_callee, &error));
  REQUIRE(builder.AddFunction(root, &error));
  REQUIRE(builder.code_page_count() == 2);
  REQUIRE(builder.function_count() == 3);

  std::vector<uint8_t> encoded;
  REQUIRE(builder.Encode(&encoded, &error));
  REQUIRE(error.empty());
  REQUIRE(encoded.size() ==
          4 * sizeof(uint32_t) +
              2 * (2 * sizeof(uint32_t) + JitCorpus::kPageSize) +
              3 * 5 * sizeof(uint32_t));
  // Header and records are explicitly little-endian, not native-endian dumps.
  REQUIRE(encoded[0] == 0x4A);
  REQUIRE(encoded[1] == 0x58);
  REQUIRE(encoded[2] == 0x43);
  REQUIRE(encoded[3] == 0x31);
  REQUIRE(encoded[4] == JitCorpus::kVersion);
  REQUIRE(encoded[16] == JitCorpus::kTagPage);
  REQUIRE(encoded[20] == 0x00);
  REQUIRE(encoded[21] == 0x00);
  REQUIRE(encoded[22] == 0x00);
  REQUIRE(encoded[23] == 0x82);
  const size_t first_function_offset =
      4 * sizeof(uint32_t) + 2 * (2 * sizeof(uint32_t) + JitCorpus::kPageSize);
  REQUIRE(encoded[first_function_offset] == JitCorpus::kTagFunction);
  REQUIRE(encoded[first_function_offset + 4] == 0x20);
  REQUIRE(encoded[first_function_offset + 5] == 0x00);
  REQUIRE(encoded[first_function_offset + 6] == 0x00);
  REQUIRE(encoded[first_function_offset + 7] == 0x83);

  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus, &error));
  REQUIRE(corpus.config_flags() == JitCorpus::kConfigGuestScheduler);
  REQUIRE(corpus.page_addresses() ==
          std::vector<uint32_t>{0x82000000, 0x83000000});
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{helper.address, static_callee.address,
                                root.address});
  REQUIRE(corpus.FindFunction(helper.address)->host_code_size == 113);
  REQUIRE(corpus.FindFunction(helper.address)->flags == helper.flags);
  REQUIRE(corpus.FindFunction(static_callee.address)->host_code_size == 79);
  REQUIRE(corpus.FindFunction(root.address)->host_code_size == 61);

  ExecutionJitCorpusBuilder canonical_builder(JitCorpus::kConfigGuestScheduler);
  REQUIRE(canonical_builder.AddCodePage(0x82000000, low_page.data(),
                                        low_page.size()));
  REQUIRE(canonical_builder.AddCodePage(0x83000000, high_page.data(),
                                        high_page.size()));
  REQUIRE(canonical_builder.AddFunction(helper));
  REQUIRE(canonical_builder.AddFunction(static_callee));
  REQUIRE(canonical_builder.AddFunction(root));
  std::vector<uint8_t> canonical_encoded;
  REQUIRE(canonical_builder.Encode(&canonical_encoded));
  REQUIRE(canonical_encoded == encoded);
}

TEST_CASE("execution JIT corpus builder fails closed on conflicting records",
          "[execution-jit-corpus][execution-jit-corpus-builder]") {
  const ExecutionJitCorpusBuilder::CodePage first_page = MakeCodePage(0x11);
  const ExecutionJitCorpusBuilder::CodePage second_page = MakeCodePage(0x22);
  const JitCorpus::FunctionRecord function = {0x82000000, 0x82000008, 64, 0};

  SECTION("duplicate page") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE(builder.AddCodePage(0x82000000, first_page.data(),
                                first_page.size(), &error));
    REQUIRE_FALSE(builder.AddCodePage(0x82000000, first_page.data(),
                                      first_page.size(), &error));
    REQUIRE(error == "exact corpus contains a duplicate code page");
    std::vector<uint8_t> encoded = {0xAA};
    REQUIRE_FALSE(builder.Encode(&encoded, &error));
    REQUIRE(encoded.empty());
    REQUIRE(error == "exact corpus contains a duplicate code page");
  }

  SECTION("conflicting page") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE(builder.AddCodePage(0x82000000, first_page.data(),
                                first_page.size(), &error));
    REQUIRE_FALSE(builder.AddCodePage(0x82000000, second_page.data(),
                                      second_page.size(), &error));
    REQUIRE(error == "exact corpus contains conflicting code-page contents");
  }

  SECTION("duplicate function") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE(builder.AddFunction(function, &error));
    REQUIRE_FALSE(builder.AddFunction(function, &error));
    REQUIRE(error == "exact corpus contains a duplicate function definition");
  }
}

TEST_CASE("execution JIT corpus builder rejects invalid closure and bounds",
          "[execution-jit-corpus][execution-jit-corpus-builder]") {
  const ExecutionJitCorpusBuilder::CodePage page = MakeCodePage(0x31);

  SECTION("unsupported config") {
    ExecutionJitCorpusBuilder builder(1u << 31);
    REQUIRE(builder.AddCodePage(0x82000000, page.data(), page.size()));
    REQUIRE(builder.AddFunction({0x82000000, 0x82000008, 64, 0}));
    std::vector<uint8_t> encoded = {0xAA};
    std::string error;
    REQUIRE_FALSE(builder.Encode(&encoded, &error));
    REQUIRE(encoded.empty());
    REQUIRE(error == "corpus configuration contains unsupported flags");
  }

  SECTION("missing extent closure") {
    ExecutionJitCorpusBuilder builder(0);
    REQUIRE(builder.AddCodePage(0x82000000, page.data(), page.size()));
    REQUIRE(builder.AddFunction({0x82000FFC, 0x82001004, 64, 0}));
    std::vector<uint8_t> encoded;
    std::string error;
    REQUIRE_FALSE(builder.Encode(&encoded, &error));
    REQUIRE(encoded.empty());
    REQUIRE(error == "corpus is missing a page in a function extent");
  }

  SECTION("invalid extent") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE_FALSE(builder.AddFunction({0x82000008, 0x82000004, 64, 0}, &error));
    REQUIRE(error == "function record contains an invalid extent");
  }

  SECTION("invalid metadata") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE_FALSE(
        builder.AddFunction({0x82000000, 0x82000004, 64, 1u << 31}, &error));
    REQUIRE(error == "function record contains unsupported flags");
  }

  SECTION("oversized extent") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE_FALSE(builder.AddFunction(
        {0x82000000, 0x82000000 + ExecutionJitCorpus::kMaxFunctionSize, 64, 0},
        &error));
    REQUIRE(error == "function record extent exceeds the size limit");
  }

  SECTION("page byte bound") {
    ExecutionJitCorpusBuilder builder(0);
    std::string error;
    REQUIRE_FALSE(
        builder.AddCodePage(0x82000000, page.data(), page.size() - 1, &error));
    REQUIRE(error == "exact corpus code page has an invalid byte size");
  }
}

TEST_CASE(
    "execution JIT corpus rejects every truncated prefix and trailing data",
    "[execution-jit-corpus]") {
  const std::vector<uint8_t> valid = MakeSpanningCorpus();
  for (size_t size = 0; size < valid.size(); ++size) {
    INFO("truncated size " << size);
    ExecutionJitCorpus output;
    std::string error;
    REQUIRE_FALSE(
        ExecutionJitCorpus::Decode(valid.data(), size, &output, &error));
    REQUIRE_FALSE(error.empty());
    REQUIRE(output.functions().empty());
  }

  std::vector<uint8_t> trailing = valid;
  trailing.push_back(0xAA);
  RequireDecodeFailure(trailing);

  std::vector<uint8_t> unknown_tag = valid;
  AppendU32(&unknown_tag, 0xFFFFFFFF);
  RequireDecodeFailure(unknown_tag);

  // Adjacent pages with no recorded function are valid host-protection-
  // granule closure and must not be mistaken for trailing data.
  std::vector<uint8_t> closure_page = valid;
  AppendPage(&closure_page, 0x82002000, 0x55);
  ExecutionJitCorpus closure_corpus;
  REQUIRE(ExecutionJitCorpus::Decode(closure_page, &closure_corpus));
  REQUIRE(closure_corpus.FindPageData(0x82002000));
}

TEST_CASE("execution JIT corpus rejects unsupported versions and flags",
          "[execution-jit-corpus]") {
  std::vector<uint8_t> malformed = MakeSpanningCorpus();
  SetU32(&malformed, 4, 1);
  RequireDecodeFailure(malformed);

  malformed = MakeSpanningCorpus();
  SetU32(&malformed, 4, 2);
  RequireDecodeFailure(malformed);

  malformed = MakeSpanningCorpus();
  SetU32(&malformed, 4, JitCorpus::kVersion + 1);
  RequireDecodeFailure(malformed);

  malformed = MakeSpanningCorpus();
  SetU32(&malformed, 12, 1u << 31);
  RequireDecodeFailure(malformed);

  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004, 1u << 6);
  RequireDecodeFailure(malformed);

  // Builtins and externs are not guest JIT definitions and cannot be replayed.
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kBuiltin,
                                             SaveRestoreType::NONE, false, 0));
  RequireDecodeFailure(malformed);

  // No-save/restore metadata must be entirely canonical.
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kDefault,
                                             SaveRestoreType::NONE, true, 0));
  RequireDecodeFailure(malformed);
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kDefault,
                                             SaveRestoreType::NONE, false, 14));
  RequireDecodeFailure(malformed);

  // Save helpers are prologs; GPR restores return, while FPR and VMX restores
  // are epilogs. Register indices must be in the XEX helper ranges.
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kProlog,
                                             SaveRestoreType::GPR, true, 14));
  RequireDecodeFailure(malformed);
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kEpilog,
                                             SaveRestoreType::GPR, true, 14));
  RequireDecodeFailure(malformed);
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kEpilogReturn,
                                             SaveRestoreType::FPR, true, 14));
  RequireDecodeFailure(malformed);
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kProlog,
                                             SaveRestoreType::GPR, false, 13));
  RequireDecodeFailure(malformed);
  malformed =
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000004,
                               FunctionFlags(Function::Behavior::kProlog,
                                             SaveRestoreType::VMX, false, 32));
  RequireDecodeFailure(malformed);

  malformed = MakeSpanningCorpus();
  SetU32(&malformed, 0, 0);
  RequireDecodeFailure(malformed);

  malformed = MakeSpanningCorpus();
  SetU32(&malformed, 8, JitCorpus::kPageSize * 2);
  RequireDecodeFailure(malformed);
}

TEST_CASE("execution JIT corpus rejects malformed pages and functions",
          "[execution-jit-corpus]") {
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x82000001, 0x82000000, 0x82000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x00000000, 0x00000004, 0x00000008));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x00001000, 0x00001000, 0x00001004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x7EFFF000, 0x7EFFF000, 0x7EFFF004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x7F000000, 0x7F000000, 0x7F000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x7FFFF000, 0x7FFFF000, 0x7FFFF004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x80000000, 0x80000000, 0x80000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x8003F000, 0x8003F000, 0x8003F004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0xA0000000, 0xA0000000, 0xA0000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x9FFFF000, 0x9FFFF000, 0x9FFFF004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0xFFFFF000, 0xFFFFF000, 0xFFFFF004));
  RequireDecodeFailure(MakeSingleFunctionCorpus(0x82000000, 0, 0x00000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x82000000, 0x82000001, 0x82000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x82000000, 0x82000000, 0x82000005));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x82000000, 0x82000008, 0x82000004));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x7EFFF000, 0x7EFFFFFC, 0x7F000000));
  RequireDecodeFailure(
      MakeSingleFunctionCorpus(0x9FFFE000, 0x9FFFEFFC, 0xA0000000));

  std::vector<uint8_t> oversized_extent = MakeHeader();
  AppendPage(&oversized_extent, 0x82000000, 0x61);
  AppendFunction(&oversized_extent, 0x82000000,
                 0x82000000 + ExecutionJitCorpus::kMaxFunctionSize);
  RequireDecodeFailure(oversized_extent);

  std::vector<uint8_t> overflow = MakeHeader();
  AppendPage(&overflow, 0xFFFFE000, 0x61);
  AppendFunction(&overflow, 0xFFFFFFF8, 0xFFFFFFFC);
  RequireDecodeFailure(overflow);

  std::vector<uint8_t> duplicate_pages = MakeHeader();
  AppendPage(&duplicate_pages, 0x82000000, 0x11);
  AppendPage(&duplicate_pages, 0x82000000, 0x22);
  AppendFunction(&duplicate_pages, 0x82000000, 0x82000004);
  RequireDecodeFailure(duplicate_pages);

  std::vector<uint8_t> duplicate_functions = MakeHeader();
  AppendPage(&duplicate_functions, 0x82000000, 0x11);
  AppendFunction(&duplicate_functions, 0x82000000, 0x82000004);
  AppendFunction(&duplicate_functions, 0x82000000, 0x82000008);
  RequireDecodeFailure(duplicate_functions);
}

TEST_CASE("execution JIT corpus enforces its byte-size envelope",
          "[execution-jit-corpus]") {
  const uint8_t byte = 0;
  ExecutionJitCorpus output;
  std::string error;
  REQUIRE_FALSE(ExecutionJitCorpus::Decode(
      &byte, ExecutionJitCorpus::kMaxCorpusSize + 1, &output, &error));
  REQUIRE(error == "corpus exceeds the byte-size limit");
  REQUIRE(output.page_addresses().empty());
  REQUIRE(output.functions().empty());
}

TEST_CASE("execution JIT corpus requires every page spanning every extent",
          "[execution-jit-corpus]") {
  std::vector<uint8_t> missing_page = MakeHeader();
  AppendPage(&missing_page, 0x82000000, 0x11);
  AppendFunction(&missing_page, 0x82000FFC, 0x82001004);
  RequireDecodeFailure(missing_page);

  std::vector<uint8_t> missing_middle_page = MakeHeader();
  AppendPage(&missing_middle_page, 0x82000000, 0x11);
  AppendPage(&missing_middle_page, 0x82002000, 0x33);
  AppendFunction(&missing_middle_page, 0x82000000, 0x82002004);
  RequireDecodeFailure(missing_middle_page);
}

TEST_CASE("execution JIT corpus carries save/restore declarations",
          "[execution-jit-corpus]") {
  const ExecutionJitCorpusBuilder::CodePage page = MakeCodePage(0x11);
  const ExecutionJitCorpus::FunctionRecord root = {
      0x82000000, 0x820000FC, 64,
      FunctionFlags(Function::Behavior::kDefault, SaveRestoreType::NONE, false,
                    0)};
  // Deliberately on a page the corpus does not supply. A declaration carries
  // no body, so it must not pull a page set in behind it.
  const ExecutionJitCorpus::DeclarationRecord helper = {
      0x83000000, 0x83000048,
      FunctionFlags(Function::Behavior::kProlog, SaveRestoreType::GPR, false,
                    14)};

  ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
  std::string error;
  REQUIRE(builder.AddCodePage(0x82000000, page.data(), page.size(), &error));
  REQUIRE(builder.AddFunction(root, &error));
  REQUIRE(builder.AddDeclaration(helper, &error));
  REQUIRE(builder.declaration_count() == 1);

  std::vector<uint8_t> encoded;
  REQUIRE(builder.Encode(&encoded, &error));
  REQUIRE(error.empty());

  ExecutionJitCorpus corpus;
  REQUIRE(ExecutionJitCorpus::Decode(encoded, &corpus, &error));
  REQUIRE(corpus.declaration_records().size() == 1);
  REQUIRE(corpus.FindDeclaration(0x83000000));
  REQUIRE(corpus.FindDeclaration(0x83000000)->end_address == 0x83000048);
  REQUIRE_FALSE(corpus.FindDeclaration(0x82000000));
  // A declaration is not a function: it takes no definition-order slot and
  // adds no page requirement.
  REQUIRE(corpus.function_definition_order() ==
          std::vector<uint32_t>{0x82000000});
  REQUIRE(corpus.page_addresses() == std::vector<uint32_t>{0x82000000});
}

TEST_CASE("execution JIT corpus admits only body-less declarations",
          "[execution-jit-corpus]") {
  ExecutionJitCorpusBuilder builder(JitCorpus::kConfigGuestScheduler);
  std::string error;

  SECTION("a host-backed extern is admitted") {
    // Replay reaches an extern through its handler, so it needs the
    // declaration and never the bytes.
    const ExecutionJitCorpus::DeclarationRecord record = {
        0x83000000, 0x83000048,
        FunctionFlags(Function::Behavior::kExtern, SaveRestoreType::NONE, false,
                      0)};
    REQUIRE(builder.AddDeclaration(record, &error));
    REQUIRE(error.empty());
  }
  SECTION("a host-backed builtin is admitted") {
    const ExecutionJitCorpus::DeclarationRecord record = {
        0x83000000, 0x83000048,
        FunctionFlags(Function::Behavior::kBuiltin, SaveRestoreType::NONE,
                      false, 0)};
    REQUIRE(builder.AddDeclaration(record, &error));
    REQUIRE(error.empty());
  }
  SECTION("an ordinary declared function is refused") {
    // Legal metadata, but not a helper. This is the case the predicate has to
    // catch on its own: keying admission on being undefined instead would let
    // it through.
    const ExecutionJitCorpus::DeclarationRecord record = {
        0x83000000, 0x83000048,
        FunctionFlags(Function::Behavior::kDefault, SaveRestoreType::NONE,
                      false, 0)};
    REQUIRE_FALSE(builder.AddDeclaration(record, &error));
    REQUIRE_FALSE(error.empty());
  }
  SECTION("one address cannot be both a function and a declaration") {
    const uint32_t helper_flags = FunctionFlags(
        Function::Behavior::kProlog, SaveRestoreType::GPR, false, 14);
    const ExecutionJitCorpus::FunctionRecord function = {0x83000000, 0x83000048,
                                                         64, helper_flags};
    const ExecutionJitCorpus::DeclarationRecord declaration = {
        0x83000000, 0x83000048, helper_flags};
    REQUIRE(builder.AddDeclaration(declaration, &error));
    REQUIRE_FALSE(builder.AddFunction(function, &error));
    REQUIRE_FALSE(error.empty());

    ExecutionJitCorpusBuilder reverse(JitCorpus::kConfigGuestScheduler);
    REQUIRE(reverse.AddFunction(function, &error));
    REQUIRE_FALSE(reverse.AddDeclaration(declaration, &error));
    REQUIRE_FALSE(error.empty());
  }
}

}  // namespace xe::cpu::test
