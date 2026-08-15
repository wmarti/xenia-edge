/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2017 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/base/debugging.h"

#include <csignal>
#include <cstdarg>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>

#include "xenia/base/string_buffer.h"

namespace xe {
namespace debugging {

bool IsDebuggerAttached() {
  std::ifstream proc_status_stream("/proc/self/status");
  if (!proc_status_stream.is_open()) {
    return false;
  }
  std::string line;
  while (std::getline(proc_status_stream, line)) {
    std::istringstream line_stream(line);
    std::string key;
    line_stream >> key;
    if (key == "TracerPid:") {
      uint32_t tracer_pid;
      line_stream >> tracer_pid;
      return tracer_pid != 0;
    }
  }
  return false;
}

void Break() {
  // Do not touch the SIGTRAP disposition here. ExceptionHandler::Install owns
  // it, and the previous std::signal call replaced the exception handler and
  // then left SIGTRAP at SIG_DFL for the rest of the run, so the next guest
  // breakpoint or trap killed the process.
  //
  // Only trap when a debugger is actually listening. It intercepts SIGTRAP
  // ahead of any handler, which is the point of the call; with nobody attached
  // the trap would reach the exception handler, go unclaimed, and be fatal.
  if (!IsDebuggerAttached()) {
    return;
  }
#if defined(__clang__)
  __builtin_debugtrap();
#else
  std::raise(SIGTRAP);
#endif
}

namespace internal {
void DebugPrint(const char* s) { std::clog << s << std::endl; }
}  // namespace internal

}  // namespace debugging
}  // namespace xe
