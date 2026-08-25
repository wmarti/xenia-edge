/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SCRIPT_SCRIPT_INPUT_DRIVER_H_
#define XENIA_HID_SCRIPT_SCRIPT_INPUT_DRIVER_H_

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace script {

// Replays a timed controller sequence read from a file, so a benchmark can
// reach the same place in a title on every run without a person driving the
// menus. Inert unless --input_script names a readable file.
class ScriptInputDriver final : public InputDriver {
 public:
  explicit ScriptInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~ScriptInputDriver() override;

  X_STATUS Setup() override;

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  InputType GetInputType() const override;
  std::vector<InputDeviceInfo> EnumerateDevices() override;

 private:
  struct Step {
    uint64_t end_ms = 0;  // cumulative, so a lookup is one comparison
    X_INPUT_GAMEPAD gamepad = {};
  };

  bool LoadScript();
  void MaybeReload();

  std::vector<Step> steps_;
  uint64_t total_ms_ = 0;
  std::atomic<uint32_t> packet_number_{1};
  size_t last_step_ = SIZE_MAX;
  // Set on the first poll rather than at load: the guest does not ask for
  // input until it is up, and a script written as "wait, then press start"
  // means wait from when the title starts asking.
  std::chrono::steady_clock::time_point start_{};
  bool started_ = false;
  // Reloading on change turns a ~90 second boot per navigation experiment into
  // a file write, which is the difference between finding a title's menu path
  // and giving up on it.
  std::filesystem::file_time_type script_mtime_{};
  std::chrono::steady_clock::time_point last_stat_{};
};

}  // namespace script
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SCRIPT_SCRIPT_INPUT_DRIVER_H_
