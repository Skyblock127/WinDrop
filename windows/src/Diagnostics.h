#pragma once
#include "common.h"

namespace td {

// Writes Downloads\WinDrop\WinDrop-PC-log-<date>.txt: a short report on what usually stops WinDrop
// (firewall profiles and rules, antivirus, network adapters and profiles, Wi-Fi and Bluetooth
// radios) followed by windrop.log. Slow (runs PowerShell): call off the UI thread.
bool SaveLogs(std::wstring* savedPath);

}  // namespace td
