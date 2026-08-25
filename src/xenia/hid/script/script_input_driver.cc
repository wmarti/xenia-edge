/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/script/script_input_driver.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"

DEFINE_path(
    input_script, "",
    "Replay a timed controller sequence from this file instead of waiting for "
    "a person. Each line is `<duration_ms> <tokens>`, where a token is a "
    "button name (A B X Y START BACK GUIDE LB RB LS RS UP DOWN LEFT RIGHT), an "
    "axis (LX= LY= RX= RY=, -32768..32767), a trigger (LT= RT=, 0..255), or `-` "
    "for nothing held. Blank lines and # comments are ignored. Empty disables "
    "the driver entirely.",
    "HID");

namespace xe {
namespace hid {
namespace script {

namespace {

struct NamedButton {
  const char* name;
  uint16_t mask;
};

constexpr NamedButton kButtons[] = {
    {"UP", X_INPUT_GAMEPAD_DPAD_UP},
    {"DOWN", X_INPUT_GAMEPAD_DPAD_DOWN},
    {"LEFT", X_INPUT_GAMEPAD_DPAD_LEFT},
    {"RIGHT", X_INPUT_GAMEPAD_DPAD_RIGHT},
    {"START", X_INPUT_GAMEPAD_START},
    {"BACK", X_INPUT_GAMEPAD_BACK},
    {"LS", X_INPUT_GAMEPAD_LEFT_THUMB},
    {"RS", X_INPUT_GAMEPAD_RIGHT_THUMB},
    {"LB", X_INPUT_GAMEPAD_LEFT_SHOULDER},
    {"RB", X_INPUT_GAMEPAD_RIGHT_SHOULDER},
    {"GUIDE", X_INPUT_GAMEPAD_GUIDE},
    {"A", X_INPUT_GAMEPAD_A},
    {"B", X_INPUT_GAMEPAD_B},
    {"X", X_INPUT_GAMEPAD_X},
    {"Y", X_INPUT_GAMEPAD_Y},
};

// Returns false when the token is not a `NAME=value` assignment this
// understands, so the caller can fall through to the button table.
bool ApplyAssignment(const std::string& token, X_INPUT_GAMEPAD* pad) {
  const size_t eq = token.find('=');
  if (eq == std::string::npos) {
    return false;
  }
  const std::string key = token.substr(0, eq);
  const std::string value = token.substr(eq + 1);
  const long v = std::strtol(value.c_str(), nullptr, 10);
  if (key == "LX") {
    pad->thumb_lx = static_cast<int16_t>(v);
  } else if (key == "LY") {
    pad->thumb_ly = static_cast<int16_t>(v);
  } else if (key == "RX") {
    pad->thumb_rx = static_cast<int16_t>(v);
  } else if (key == "RY") {
    pad->thumb_ry = static_cast<int16_t>(v);
  } else if (key == "LT") {
    pad->left_trigger = static_cast<uint8_t>(v);
  } else if (key == "RT") {
    pad->right_trigger = static_cast<uint8_t>(v);
  } else {
    return false;
  }
  return true;
}

}  // namespace

ScriptInputDriver::ScriptInputDriver(xe::ui::Window* window,
                                     size_t window_z_order)
    : InputDriver(window, window_z_order) {}

ScriptInputDriver::~ScriptInputDriver() = default;

bool ScriptInputDriver::LoadScript() {
  std::ifstream f(cvars::input_script);
  if (!f) {
    XELOGE("input script: cannot open {}", cvars::input_script.string());
    return false;
  }
  std::string line;
  size_t line_no = 0;
  while (std::getline(f, line)) {
    ++line_no;
    const size_t hash = line.find('#');
    if (hash != std::string::npos) {
      line = line.substr(0, hash);
    }
    std::istringstream ls(line);
    std::string first;
    if (!(ls >> first)) {
      continue;
    }
    char* end = nullptr;
    const long long duration = std::strtoll(first.c_str(), &end, 10);
    if (end == first.c_str() || duration < 0) {
      XELOGW("input script: line {} does not start with a duration, skipped",
             line_no);
      continue;
    }
    Step step;
    uint16_t buttons = 0;
    std::string token;
    while (ls >> token) {
      if (token == "-") {
        continue;
      }
      if (ApplyAssignment(token, &step.gamepad)) {
        continue;
      }
      bool matched = false;
      for (const auto& b : kButtons) {
        if (token == b.name) {
          buttons |= b.mask;
          matched = true;
          break;
        }
      }
      if (!matched) {
        XELOGW("input script: line {} has unknown token \"{}\"", line_no,
               token);
      }
    }
    step.gamepad.buttons = buttons;
    total_ms_ += static_cast<uint64_t>(duration);
    step.end_ms = total_ms_;
    steps_.push_back(step);
  }
  if (steps_.empty()) {
    XELOGE("input script: {} contained no steps", cvars::input_script.string());
    return false;
  }
  XELOGI("input script: {} steps over {} ms from {}", steps_.size(), total_ms_,
         cvars::input_script.string());
  return true;
}

X_STATUS ScriptInputDriver::Setup() {
  if (cvars::input_script.empty()) {
    return X_STATUS_UNSUCCESSFUL;
  }
  std::error_code ec;
  script_mtime_ = std::filesystem::last_write_time(cvars::input_script, ec);
  return LoadScript() ? X_STATUS_SUCCESS : X_STATUS_UNSUCCESSFUL;
}

// Rewriting the file replays it from the top. That is what makes exploring a
// title's menus practical: press one button, look, write the next step.
void ScriptInputDriver::MaybeReload() {
  const auto now = std::chrono::steady_clock::now();
  if (now - last_stat_ < std::chrono::milliseconds(250)) {
    return;
  }
  last_stat_ = now;
  std::error_code ec;
  const auto mtime = std::filesystem::last_write_time(cvars::input_script, ec);
  if (ec || mtime == script_mtime_) {
    return;
  }
  script_mtime_ = mtime;
  steps_.clear();
  total_ms_ = 0;
  if (LoadScript()) {
    start_ = now;
    last_step_ = SIZE_MAX;
  }
}

X_RESULT ScriptInputDriver::GetCapabilities(uint32_t user_index, uint32_t flags,
                                            X_INPUT_CAPABILITIES* out_caps) {
  if (user_index != 0 || !out_caps) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  // A plain wired gamepad. Titles that refuse input from a pad they cannot
  // enumerate need this to answer before they will read any state at all.
  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  out_caps->flags = 0;
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ly = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_rx = static_cast<int16_t>(0xFFFFu);
  out_caps->gamepad.thumb_ry = static_cast<int16_t>(0xFFFFu);
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::GetState(uint32_t user_index,
                                     X_INPUT_STATE* out_state) {
  if (user_index != 0 || !out_state) {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  if (!started_) {
    start_ = std::chrono::steady_clock::now();
    started_ = true;
  }
  MaybeReload();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start_)
          .count();

  std::memset(out_state, 0, sizeof(*out_state));
  size_t step = SIZE_MAX;
  if (static_cast<uint64_t>(elapsed) < total_ms_) {
    for (size_t n = 0; n < steps_.size(); ++n) {
      if (static_cast<uint64_t>(elapsed) < steps_[n].end_ms) {
        step = n;
        break;
      }
    }
  }
  // Past the end the pad goes neutral rather than holding the last step, so a
  // script that ends on a button press does not leave it stuck down for the
  // rest of the run.
  if (step != SIZE_MAX) {
    out_state->gamepad = steps_[step].gamepad;
  }
  if (step != last_step_) {
    last_step_ = step;
    packet_number_.fetch_add(1);
  }
  out_state->packet_number = packet_number_.load();
  return X_ERROR_SUCCESS;
}

X_RESULT ScriptInputDriver::SetState(uint32_t user_index,
                                     X_INPUT_VIBRATION* vibration) {
  return user_index == 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
}

X_RESULT ScriptInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                         X_INPUT_KEYSTROKE* out_keystroke) {
  return X_ERROR_EMPTY;
}

InputType ScriptInputDriver::GetInputType() const {
  return InputType::Controller;
}

// InputSystem only routes GetState to drivers it has bound to a guest slot,
// and it binds from this list. Without it the driver is loaded, polled by
// nothing, and silently does nothing at all.
std::vector<InputDeviceInfo> ScriptInputDriver::EnumerateDevices() {
  if (steps_.empty()) {
    return {};
  }
  InputDeviceInfo info;
  info.driver_slot = 0;
  info.stable_id = "script:0";
  info.display_name = "Scripted input";
  info.subtype = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  info.preferred_slot = 0;
  info.auto_bind = true;
  return {info};
}

}  // namespace script
}  // namespace hid
}  // namespace xe
