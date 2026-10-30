#pragma once
#include <functional>

#include "common.h"

// In-app updates from the project's GitHub releases. The latest release carries WinDrop-Setup.exe;
// it's downloaded over HTTPS, checked against the SHA-256 GitHub publishes for it, and run. The
// installer stops this app, replaces it and starts it again.
namespace td {

struct UpdateInfo {
  std::string version;   // "1.2.0" (tag without the "v")
  std::wstring url;      // WinDrop-Setup.exe download
  std::string sha256;    // hex, from the release asset's digest; empty if GitHub didn't give one
};

// Blocking. True when the check worked; info->version is empty when nothing newer exists.
bool CheckForUpdate(UpdateInfo* info);

// Blocking. Downloads the installer to %TEMP%, verifies it and starts it silently (one UAC prompt).
// `progress` gets 0..100. On failure returns false and fills *error.
bool InstallUpdate(const UpdateInfo& info, std::function<void(int)> progress, std::wstring* error);

// a > b for dotted numeric versions ("1.10.0" > "1.9.3").
bool VersionNewer(const std::string& a, const std::string& b);

}  // namespace td
