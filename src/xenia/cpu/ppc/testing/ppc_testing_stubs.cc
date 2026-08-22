/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Kernel-side symbols that xex_module.cc references, supplied here so the PPC
// test runner links without xenia-kernel (and therefore without xenia-ui, which
// needs GTK3 and fontconfig on Linux).
//
// Only compiled when XENIA_BUILD_EMULATOR is OFF. None of these run: the test
// driver loads guest code through RawModule and never opens a XEX. They exist
// because Processor::LookupModule does dynamic_cast<XexModule*>, which pulls
// xex_module.o out of the archive whether or not the path is taken.

#include "xenia/base/cvar.h"

DEFINE_bool(allow_plugins, false,
            "Unused by the PPC test runner; see xenia-kernel.", "General");
DEFINE_bool(guest_scheduler, true,
            "Unused by the PPC test runner; see xenia-kernel.", "Kernel");
