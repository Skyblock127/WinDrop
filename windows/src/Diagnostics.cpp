#include "Diagnostics.h"

#include <wincrypt.h>

#include <ctime>

#include "Settings.h"
#include "WifiDirectHost.h"

namespace td {

namespace {

// Everything here is read-only. Each section prints its heading even when it finds nothing.
constexpr char kScript[] = R"PS(
$ErrorActionPreference = 'SilentlyContinue'
'== Firewall profiles (AllowInboundRules False = "block all incoming connections" is on)'
Get-NetFirewallProfile | Format-Table Name, Enabled, DefaultInboundAction, AllowInboundRules -AutoSize | Out-String -Width 200
'== Firewall rules for WinDrop'
Get-NetFirewallApplicationFilter | Where-Object { $_.Program -like '*WinDrop*' } | ForEach-Object {
  $r = $_ | Get-NetFirewallRule
  '{0} | enabled {1} | {2} | {3} | {4} | {5}' -f $r.DisplayName, $r.Enabled, $r.Direction, $r.Action, $r.Profile, $_.Program
}
'== Antivirus products'
Get-CimInstance -Namespace root/SecurityCenter2 -ClassName AntivirusProduct | ForEach-Object { '{0} (state {1})' -f $_.displayName, $_.productState }
'== Third-party firewalls'
Get-CimInstance -Namespace root/SecurityCenter2 -ClassName FirewallProduct | ForEach-Object { $_.displayName }
'== Network profiles'
Get-NetConnectionProfile | Format-Table InterfaceAlias, Name, NetworkCategory -AutoSize | Out-String -Width 200
'== IPv4 addresses'
Get-NetIPAddress -AddressFamily IPv4 | Format-Table InterfaceAlias, IPAddress, PrefixLength -AutoSize | Out-String -Width 200
'== Network adapters'
Get-NetAdapter -IncludeHidden | Where-Object { $_.Status -ne 'Not Present' } | Format-Table Name, InterfaceDescription, Status -AutoSize | Out-String -Width 200
'== VPN connections'
Get-VpnConnection | Format-Table Name, ConnectionStatus -AutoSize | Out-String -Width 200
'== Wi-Fi driver'
netsh wlan show drivers | Select-String 'Driver|Vendor|Version|Radio types|Wireless Display|Hosted network'
'== Bluetooth'
Get-PnpDevice -Class Bluetooth -PresentOnly | Format-Table FriendlyName, Status -AutoSize | Out-String -Width 200
)PS";

// Runs PowerShell hidden and returns what it printed (UTF-8), or why it couldn't.
std::string RunDiagnostics() {
  std::wstring script = U2W(kScript);
  DWORD n = 0;
  auto bytes = (const BYTE*)script.data();
  DWORD len = (DWORD)(script.size() * sizeof(wchar_t));
  CryptBinaryToStringW(bytes, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &n);
  std::wstring b64(n, L'\0');
  CryptBinaryToStringW(bytes, len, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64.data(), &n);
  b64.resize(n);
  std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -OutputFormat Text "
                     L"-Command \"[Console]::OutputEncoding=[Text.Encoding]::UTF8; "
                     L"& ([ScriptBlock]::Create([Text.Encoding]::Unicode.GetString([Convert]::FromBase64String('" +
                     b64 + L"'))))\"";

  SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
  HANDLE rd = nullptr, wr = nullptr;
  if (!CreatePipe(&rd, &wr, &sa, 0)) return "(couldn't run PowerShell)\n";
  SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
  STARTUPINFOW si{sizeof si};
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = wr;
  si.hStdError = wr;
  PROCESS_INFORMATION pi{};
  BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
  CloseHandle(wr);
  if (!ok) { CloseHandle(rd); return "(couldn't run PowerShell)\n"; }
  std::string out;
  char buf[4096];
  DWORD got = 0;
  ULONGLONG deadline = GetTickCount64() + 60000;
  while (ReadFile(rd, buf, sizeof buf, &got, nullptr) && got) {
    out.append(buf, got);
    if (GetTickCount64() > deadline) break;
  }
  if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(rd);
  return out;
}

// The log is written while we read it, so open it shared.
std::string ReadShared(const std::wstring& path) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return "";
  std::string s;
  char buf[65536];
  DWORD got = 0;
  while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got) s.append(buf, got);
  CloseHandle(f);
  return s;
}

std::string WindowsVersion() {
  HKEY k;
  std::string v = "?";
  if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion", 0, KEY_READ, &k) == ERROR_SUCCESS) {
    wchar_t name[128] = L"", build[32] = L"";
    DWORD a = sizeof name, b = sizeof build;
    RegQueryValueExW(k, L"ProductName", nullptr, nullptr, (BYTE*)name, &a);
    RegQueryValueExW(k, L"CurrentBuild", nullptr, nullptr, (BYTE*)build, &b);
    RegCloseKey(k);
    v = W2U(name) + " (build " + W2U(build) + ")";
  }
  return v;
}

}  // namespace

bool SaveLogs(std::wstring* savedPath) {
  std::wstring dir, data;
  std::string report;
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    dir = Cfg().downloadDir;
    data = Cfg().dataDir;
    report += "WinDrop " WINDROP_VERSION " on " + W2U(Cfg().deviceName) + "\r\n";
    report += "Windows: " + WindowsVersion() + "\r\n";
    report += "On: " + std::string(Cfg().enabled ? "yes" : "no") + ", phones paired: " + std::to_string(Cfg().phones.size()) + "\r\n";
    for (auto& p : Cfg().phones) report += "  phone: " + W2U(p.name) + " (" + p.id + ")\r\n";
  }
  report += "Wi-Fi Direct: " + W2U(TheWifiDirect().Status()) + "\r\n";
  report += "Queued items: " + std::to_string(TheOutbox().Count()) + "\r\n\r\n";
  Log("logs: collecting diagnostics");
  std::string diag = RunDiagnostics();
  for (size_t i = 0; (i = diag.find('\n', i)) != std::string::npos; i += 2)
    if (i == 0 || diag[i - 1] != '\r') diag.insert(i, 1, '\r');  // Notepad-friendly line ends
  report += diag + "\r\n== windrop.log\r\n" + ReadShared(data + L"\\windrop.log.1") + ReadShared(data + L"\\windrop.log");

  wchar_t stamp[32];
  time_t now = time(nullptr);
  tm t{};
  localtime_s(&t, &now);
  wcsftime(stamp, 32, L"%Y-%m-%d %H%M%S", &t);
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = dir + L"\\WinDrop-PC-log " + stamp + L".txt";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  DWORD wrote = 0;
  bool ok = WriteFile(f, report.data(), (DWORD)report.size(), &wrote, nullptr) && wrote == report.size();
  CloseHandle(f);
  if (ok) {
    *savedPath = path;
    Log("logs: saved %s", W2U(path).c_str());
  }
  return ok;
}

}  // namespace td
