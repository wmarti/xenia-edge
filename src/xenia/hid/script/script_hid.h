/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SCRIPT_SCRIPT_HID_H_
#define XENIA_HID_SCRIPT_SCRIPT_HID_H_

#include <memory>

#include "xenia/hid/input_driver.h"

namespace xe {
namespace hid {
namespace script {

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order);

}  // namespace script
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SCRIPT_SCRIPT_HID_H_
