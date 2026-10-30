#include "Updater.h"

#include <shellapi.h>
#include <winhttp.h>

#include <memory>

#include "Crypto.h"

namespace td {

namespace {

constexpr wchar_t kApiHost[] = L"api.github.com";
constexpr wchar_t kLatestPath[] = L"/repos/Skyblock127/WinDrop/releases/latest";
constexpr char kAsset[] = "WinDrop-Setup.exe";

struct Inet {
  HINTERNET h = nullptr;
  ~Inet() { if (h) WinHttpCloseHandle(h); }
};

// GET over HTTPS (redirects followed). `sink` gets the body in pieces plus the total length (0 if
// unknown); returning false from it aborts.
bool HttpGet(const std::wstring& url, std::function<bool(const char*, size_t, uint64_t)> sink, std::wstring* error) {
  URL_COMPONENTS uc{sizeof uc};
  wchar_t host[256], path[2048];
  uc.lpszHostName = host;
  uc.dwHostNameLength = 256;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 2048;
  if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
    *error = L"bad update address";
    return false;
  }
  std::wstring agent = L"WinDrop/" + U2W(WINDROP_VERSION);
  Inet session{WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0)};
  if (!session.h) { *error = L"no network"; return false; }
  WinHttpSetTimeouts(session.h, 10000, 10000, 15000, 30000);
  Inet conn{WinHttpConnect(session.h, host, uc.nPort, 0)};
  Inet req{conn.h ? WinHttpOpenRequest(conn.h, L"GET", path, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr};
  if (!req.h) { *error = L"no network"; return false; }
  const wchar_t* hdrs = L"Accept: application/vnd.github+json\r\n";
  if (!WinHttpSendRequest(req.h, hdrs, (DWORD)-1, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(req.h, nullptr)) {
    *error = L"couldn't reach GitHub (offline?)";
    return false;
  }
  DWORD status = 0, len = sizeof status;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len, nullptr);
  if (status != 200) {
    *error = L"GitHub answered " + std::to_wstring(status);
    return false;
  }
  wchar_t clen[32] = L"0";
  len = sizeof clen;
  WinHttpQueryHeaders(req.h, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, clen, &len, nullptr);
  uint64_t total = _wcstoui64(clen, nullptr, 10);
  std::vector<char> buf(256 * 1024);
  for (;;) {
    DWORD got = 0;
    if (!WinHttpReadData(req.h, buf.data(), (DWORD)buf.size(), &got)) { *error = L"download interrupted"; return false; }
    if (got == 0) return true;
    if (!sink(buf.data(), got, total)) { *error = L"download stopped"; return false; }
  }
}

// The string value of "key" at or after `from` in GitHub's JSON (values we read have no escapes).
std::string JsonString(const std::string& json, const std::string& key, size_t from, size_t* at = nullptr) {
  size_t k = json.find("\"" + key + "\"", from);
  if (k == std::string::npos) return "";
  size_t q = json.find('"', json.find(':', k) + 1);
  size_t e = q == std::string::npos ? q : json.find('"', q + 1);
  if (e == std::string::npos) return "";
  if (at) *at = k;
  return json.substr(q + 1, e - q - 1);
}

std::string HexOf(const Bytes& b) {
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (auto c : b) { s += hex[c >> 4]; s += hex[c & 15]; }
  return s;
}

}  // namespace

bool VersionNewer(const std::string& a, const std::string& b) {
  auto parts = [](const std::string& v) {
    std::vector<int> n;
    size_t i = 0;
    while (i < v.size() && n.size() < 4) {
      n.push_back(atoi(v.c_str() + i));
      i = v.find('.', i);
      if (i == std::string::npos) break;
      ++i;
    }
    n.resize(4, 0);
    return n;
  };
  return parts(a) > parts(b);
}

bool CheckForUpdate(UpdateInfo* info) {
  std::string json;
  std::wstring err;
  bool ok = HttpGet(std::wstring(L"https://") + kApiHost + kLatestPath,
                    [&](const char* p, size_t n, uint64_t) { json.append(p, n); return json.size() < (4u << 20); }, &err);
  if (!ok) { Log("update: check failed: %s", W2U(err).c_str()); return false; }
  std::string tag = JsonString(json, "tag_name", 0);
  if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
  *info = {};
  if (tag.empty() || !VersionNewer(tag, WINDROP_VERSION)) {
    Log("update: up to date (%s, latest %s)", WINDROP_VERSION, tag.c_str());
    return true;
  }
  size_t asset = json.find("\"name\":\"" + std::string(kAsset) + "\"");
  if (asset == std::string::npos) asset = json.find("\"name\": \"" + std::string(kAsset) + "\"");
  if (asset == std::string::npos) { Log("update: %s has no %s", tag.c_str(), kAsset); return true; }
  // Within this asset's object: digest comes before browser_download_url, which ends it.
  size_t urlAt = 0;
  std::string url = JsonString(json, "browser_download_url", asset, &urlAt);
  size_t digAt = 0;
  std::string digest = JsonString(json, "digest", asset, &digAt);
  if (digAt > urlAt || digest.rfind("sha256:", 0) != 0) digest.clear();
  if (url.rfind("https://github.com/", 0) != 0) { Log("update: unexpected download address"); return false; }
  info->version = tag;
  info->url = U2W(url);
  info->sha256 = digest.empty() ? "" : digest.substr(7);
  Log("update: %s available", tag.c_str());
  return true;
}

bool InstallUpdate(const UpdateInfo& info, std::function<void(int)> progress, std::wstring* error) {
  wchar_t tmp[MAX_PATH];
  GetTempPathW(MAX_PATH, tmp);
  std::wstring path = std::wstring(tmp) + L"WinDrop-Setup-" + U2W(info.version) + L".exe";
  Bytes data;
  bool ok = HttpGet(info.url, [&](const char* p, size_t n, uint64_t total) {
    data.insert(data.end(), p, p + n);
    if (total) progress((int)(data.size() * 100 / total));
    return data.size() < (200u << 20);
  }, error);
  if (!ok) return false;
  if (!info.sha256.empty() && HexOf(Sha256(data)) != info.sha256) {
    *error = L"the download didn't match its checksum";
    Log("update: checksum mismatch, not installing");
    return false;
  }
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  DWORD wrote = 0;
  ok = f != INVALID_HANDLE_VALUE && WriteFile(f, data.data(), (DWORD)data.size(), &wrote, nullptr) && wrote == data.size();
  if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
  if (!ok) { *error = L"couldn't save the installer"; return false; }
  Log("update: starting installer for %s", info.version.c_str());
  // The installer elevates itself (one UAC prompt), stops this app and starts the new one.
  SHELLEXECUTEINFOW sei{sizeof sei};
  sei.fMask = SEE_MASK_NOASYNC;
  sei.lpFile = path.c_str();
  sei.lpParameters = L"/SILENT /SUPPRESSMSGBOXES /NORESTART /SP-";
  sei.nShow = SW_SHOWNORMAL;
  if (!ShellExecuteExW(&sei)) {
    *error = GetLastError() == ERROR_CANCELLED ? L"cancelled" : L"couldn't start the installer";
    return false;
  }
  return true;
}

}  // namespace td
