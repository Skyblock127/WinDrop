#include "common.h"

#include <cstdarg>
#include <cstdio>
#include <mutex>

#include "Settings.h"

namespace td {

std::string W2U(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring U2W(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::wstring HumanSize(uint64_t b) {
  const wchar_t* units[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
  double v = (double)b;
  int u = 0;
  while (v >= 1024 && u < 4) { v /= 1024; ++u; }
  wchar_t buf[32];
  swprintf(buf, 32, u == 0 ? L"%.0f %s" : L"%.1f %s", v, units[u]);
  return buf;
}

std::wstring FileNameOf(const std::wstring& path) {
  size_t p = path.find_last_of(L"\\/");
  return p == std::wstring::npos ? path : path.substr(p + 1);
}

constexpr wchar_t kLogName[] = L"windrop.log";
constexpr DWORD kLogRollBytes = 5u << 20;

void Log(const char* fmt, ...) {
  static std::mutex mx;
  char line[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof line, fmt, ap);
  va_end(ap);
  OutputDebugStringA(line);
  OutputDebugStringA("\n");

  std::lock_guard<std::mutex> lk(mx);
  std::wstring file = Cfg().dataDir + L"\\" + kLogName;
  // Capped at ~10 MB: roll the current file over to .1 once it passes 5 MB.
  WIN32_FILE_ATTRIBUTE_DATA fa;
  if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > kLogRollBytes) {
    std::wstring old = file + L".1";
    MoveFileExW(file.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
  }
  FILE* f = _wfopen(file.c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME st;
  GetLocalTime(&st);
  fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d.%03d %s\r\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, line);
  fclose(f);
}

}  // namespace td
