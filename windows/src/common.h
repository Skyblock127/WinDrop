#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#ifndef WINDROP_VERSION
#define WINDROP_VERSION "0.0.0"  // set from ../VERSION by CMake
#endif

namespace td {

constexpr uint16_t kTcpPort = 53318;
// USB fallback: `adb forward tcp:53320 tcp:53318` makes the phone reachable on 127.0.0.1:53320.

constexpr UINT WM_APP_NET = WM_APP + 1;
constexpr UINT WM_APP_TRAY = WM_APP + 2;

std::string W2U(const std::wstring& w);
std::wstring U2W(const std::string& s);
std::wstring HumanSize(uint64_t bytes);
std::wstring FileNameOf(const std::wstring& path);
void Log(const char* fmt, ...);

}  // namespace td
