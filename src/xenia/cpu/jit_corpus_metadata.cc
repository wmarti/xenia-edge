/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/jit_corpus.h"

namespace xe {
namespace cpu {

namespace {

bool IsLegalSaverestIndex(SaveRestoreType type, uint8_t index) {
  switch (type) {
    case SaveRestoreType::GPR:
    case SaveRestoreType::FPR:
      return index >= 14 && index <= 31;
    case SaveRestoreType::VMX:
      return (index >= 14 && index <= 31) || (index >= 64 && index <= 127);
    case SaveRestoreType::NONE:
      return false;
  }
  return false;
}

bool IsLegalFunctionMetadata(const JitCorpus::FunctionMetadata& metadata) {
  if (metadata.saverest_type == SaveRestoreType::NONE) {
    return metadata.behavior == Function::Behavior::kDefault &&
           !metadata.is_restore && !metadata.saverest_index;
  }
  if (!IsLegalSaverestIndex(metadata.saverest_type, metadata.saverest_index)) {
    return false;
  }
  if (!metadata.is_restore) {
    return metadata.behavior == Function::Behavior::kProlog;
  }
  if (metadata.saverest_type == SaveRestoreType::GPR) {
    return metadata.behavior == Function::Behavior::kEpilogReturn;
  }
  return metadata.behavior == Function::Behavior::kEpilog;
}

}  // namespace

uint32_t JitCorpus::EncodeFunctionFlags(const Function& function) {
  return EncodeFunctionFlags(function.behavior(), function.SaverestType(),
                             function.IsRestore(),
                             static_cast<uint8_t>(function.SaverestIndex()));
}

uint32_t JitCorpus::EncodeFunctionFlags(Function::Behavior behavior,
                                        SaveRestoreType saverest_type,
                                        bool is_restore,
                                        uint8_t saverest_index) {
  return (static_cast<uint32_t>(behavior) << kFunctionBehaviorShift) |
         (static_cast<uint32_t>(saverest_type) << kFunctionSaverestTypeShift) |
         (is_restore ? kFunctionSaverestRestore : 0) |
         (uint32_t(saverest_index) << kFunctionSaverestIndexShift);
}

bool JitCorpus::DecodeFunctionFlags(uint32_t flags,
                                    FunctionMetadata* metadata) {
  if (!metadata) {
    return false;
  }
  *metadata = {};
  if (flags & ~kKnownFunctionFlags) {
    return false;
  }

  const uint32_t behavior_value =
      (flags & kFunctionBehaviorMask) >> kFunctionBehaviorShift;
  const uint32_t saverest_type_value =
      (flags & kFunctionSaverestTypeMask) >> kFunctionSaverestTypeShift;
  if (behavior_value >
          static_cast<uint32_t>(Function::Behavior::kEpilogReturn) ||
      saverest_type_value > static_cast<uint32_t>(SaveRestoreType::FPR)) {
    return false;
  }

  metadata->behavior = static_cast<Function::Behavior>(behavior_value);
  metadata->saverest_type = static_cast<SaveRestoreType>(saverest_type_value);
  metadata->is_restore = (flags & kFunctionSaverestRestore) != 0;
  metadata->saverest_index = static_cast<uint8_t>(
      (flags & kFunctionSaverestIndexMask) >> kFunctionSaverestIndexShift);
  if (!IsLegalFunctionMetadata(*metadata)) {
    *metadata = {};
    return false;
  }
  return true;
}

}  // namespace cpu
}  // namespace xe
