/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_CPU_GUEST_INVOCATION_CAPTURE_PAGE_READER_H_
#define XENIA_CPU_GUEST_INVOCATION_CAPTURE_PAGE_READER_H_

#if defined(XE_ENABLE_GUEST_INVOCATION_CAPTURE) && \
    XE_ENABLE_GUEST_INVOCATION_CAPTURE

#include <array>
#include <cstdint>

#include "xenia/cpu/guest_invocation_recorder.h"

namespace xe {

class Memory;

namespace cpu {

class GuestInvocationCapturePageReader final
    : public ppc::GuestInvocationRecorderPageReader {
 public:
  explicit GuestInvocationCapturePageReader(Memory& memory) : memory_(memory) {}

  bool ReadPage(uint32_t page_address,
                std::array<uint8_t, 4096>* output) override;
  bool last_read_was_retryable() const override {
    return last_read_was_retryable_;
  }
  // Which guard refused the page. A failed snapshot is otherwise
  // indistinguishable from an unmapped address at the call site.
  uint32_t last_read_failure_site() const { return last_read_failure_site_; }

 private:
  Memory& memory_;
  bool last_read_was_retryable_ = false;
  uint32_t last_read_failure_site_ = 0;
};

}  // namespace cpu
}  // namespace xe

#endif

#endif  // XENIA_CPU_GUEST_INVOCATION_CAPTURE_PAGE_READER_H_
