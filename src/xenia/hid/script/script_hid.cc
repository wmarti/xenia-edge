/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/hid/script/script_hid.h"

#include "xenia/hid/script/script_input_driver.h"

namespace xe {
namespace hid {
namespace script {

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order) {
  return std::make_unique<ScriptInputDriver>(window, window_z_order);
}

}  // namespace script
}  // namespace hid
}  // namespace xe
