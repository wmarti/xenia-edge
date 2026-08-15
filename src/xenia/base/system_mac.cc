/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include <spawn.h>
#include <sys/wait.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "xenia/base/system.h"

extern char** environ;

namespace xe {
namespace {

// Runs a program with an explicit argument vector. Nothing goes through a
// shell, so no argument can be read as syntax.
void SpawnDetached(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);
  pid_t pid = 0;
  if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ)) {
    return;
  }
  int status = 0;
  waitpid(pid, &status, 0);
}

}  // namespace

void LaunchWebBrowser(const std::string_view url) {
  // Never build a shell command line out of this: a URL or path is attacker
  // -influenced data and any metacharacter in it would be executed.
  SpawnDetached({"open", std::string(url)});
}

void LaunchFileExplorer(const std::filesystem::path& path) {
  SpawnDetached({"open", path.string()});
}

void ShowSimpleMessageBox(SimpleMessageBoxType type, std::string_view message) {
  const char* icon;
  switch (type) {
    case SimpleMessageBoxType::Help:
      icon = "note";
      break;
    case SimpleMessageBoxType::Warning:
      icon = "caution";
      break;
    default:
    case SimpleMessageBoxType::Error:
      icon = "stop";
      break;
  }
  // Build the AppleScript as one argument and hand it to osascript directly.
  // The old form wrapped it in single quotes for a shell, which a single quote
  // in the message closed, making the rest of the message shell syntax.
  std::string script = "display dialog \"";
  // Escape what AppleScript treats as syntax inside a quoted string.
  for (char c : message) {
    if (c == '"' || c == '\\') {
      script += '\\';
    }
    script += c;
  }
  script += "\" with icon ";
  script += icon;
  script += " buttons {\"OK\"} default button \"OK\" with title \"Xenia\"";
  SpawnDetached({"osascript", "-e", script});
}

bool SetProcessPriorityClass(const uint32_t priority_class) { return true; }

bool IsUseNexusForGameBarEnabled() { return false; }

}  // namespace xe
