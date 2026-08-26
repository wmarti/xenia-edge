/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/cvar.h"

#include "third_party/catch/include/catch.hpp"

TEST_CASE("ConfigVar reports its effective value", "[cvar]") {
  bool current_value = false;
  cvar::ConfigVar<bool> config_var("test", &current_value, "", "", "Test",
                                   false, false);

  REQUIRE(config_var.default_value() == "false");
  REQUIRE(config_var.effective_value() == "false");

  config_var.SetConfigValue(true);
  REQUIRE(config_var.effective_value() == "true");

  config_var.SetGameConfigValue(false);
  REQUIRE(config_var.effective_value() == "false");

  config_var.SetCommandLineValue(true);
  REQUIRE(config_var.effective_value() == "true");
}
