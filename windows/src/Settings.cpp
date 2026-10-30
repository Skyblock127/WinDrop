#include "Settings.h"

#include <shlobj.h>

#include <cstdio>
#include <random>

namespace td {

std::recursive_mutex& CfgMutex() {
  static std::recursive_mutex m;
  return m;
}
Settings& Cfg() {
  static Settings s;
  return s;
}
Outbox& TheOutbox() {
  static Outbox o;
  return o;
}

namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

std::wstring IniPath() { return Cfg().dataDir + L"\\settings.ini"; }

std::wstring ReadIni(const wchar_t* key, const wchar_t* def = L"") {
  wchar_t buf[1024];
  GetPrivateProfileStringW(L"WinDrop", key, def, buf, 1024, IniPath().c_str());
  return buf;
}

void WriteIni(const wchar_t* key, const std::wstring& v) {
  WritePrivateProfileStringW(L"WinDrop", key, v.c_str(), IniPath().c_str());
}

std::string Hex(const std::string& s) {
  static const char* d = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) { out += d[c >> 4]; out += d[c & 15]; }
  return out;
}

std::string Unhex(const std::string& h) {
  auto v = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
  std::string out;
  for (size_t i = 0; i + 1 < h.size(); i += 2) out += char(v(h[i]) << 4 | v(h[i + 1]));
  return out;
}

}  // namespace

void Settings::Load() {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  dataDir = KnownFolder(FOLDERID_LocalAppData) + L"\\WinDrop";
  CreateDirectoryW(dataDir.c_str(), nullptr);
  downloadDir = KnownFolder(FOLDERID_Downloads) + L"\\WinDrop";

  // A UTF-16 BOM makes the profile APIs keep non-ASCII names intact.
  if (GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
    FILE* f = _wfopen(IniPath().c_str(), L"wb");
    if (f) { fwrite("\xFF\xFE", 1, 2, f); fclose(f); }
  }

  enabled = ReadIni(L"Enabled", L"1") == L"1";
  pillShown = ReadIni(L"ShowPill", L"1") == L"1";
  pairedPhoneId = W2U(ReadIni(L"PairedPhoneId"));
  pairedPhoneName = ReadIni(L"PairedPhoneName");
  pairKey = UnprotectKey(W2U(ReadIni(L"PairKey")));
  if (pairKey.size() != 32) { pairKey.clear(); pairedPhoneId.clear(); }  // older, unencrypted pairing: pair again
  deviceId = W2U(ReadIni(L"DeviceId"));
  if (deviceId.empty()) {
    std::random_device rd;
    char b[24];
    snprintf(b, sizeof b, "%08x%08x", rd(), rd());
    deviceId = b;
  }
  wchar_t name[256];
  DWORD n = 256;
  deviceName = GetComputerNameExW(ComputerNamePhysicalDnsHostname, name, &n) ? name : L"Windows PC";
  Save();
}

void Settings::Save() {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  WriteIni(L"Enabled", enabled ? L"1" : L"0");
  WriteIni(L"ShowPill", pillShown ? L"1" : L"0");
  WriteIni(L"PairedPhoneId", U2W(pairedPhoneId));
  WriteIni(L"PairedPhoneName", pairedPhoneName);
  WriteIni(L"PairKey", pairKey.empty() ? L"" : U2W(ProtectKey(pairKey)));
  WriteIni(L"DeviceId", U2W(deviceId));
}

// Outbox file format: one item per line, "F\t<utf8 path>" or "T\t<hex utf8 text>".
void Outbox::Load(const std::wstring& file) {
  std::lock_guard<std::mutex> lk(mx_);
  file_ = file;
  items_.clear();
  FILE* f = _wfopen(file.c_str(), L"rb");
  if (!f) return;
  std::string all;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) all.append(buf, n);
  fclose(f);
  size_t pos = 0;
  while (pos < all.size()) {
    size_t e = all.find('\n', pos);
    if (e == std::string::npos) e = all.size();
    std::string line = all.substr(pos, e - pos);
    pos = e + 1;
    if (line.size() < 3 || line[1] != '\t') continue;
    OutboxItem it;
    it.id = nextId_++;
    if (line[0] == 'F') it.path = U2W(line.substr(2));
    else if (line[0] == 'T') { it.isText = true; it.text = Unhex(line.substr(2)); }
    else continue;
    items_.push_back(std::move(it));
  }
}

void Outbox::SaveLocked() {
  FILE* f = _wfopen(file_.c_str(), L"wb");
  if (!f) return;
  for (auto& it : items_) {
    std::string line = it.isText ? "T\t" + Hex(it.text) : "F\t" + W2U(it.path);
    line += '\n';
    fwrite(line.data(), 1, line.size(), f);
  }
  fclose(f);
}

uint64_t Outbox::Add(OutboxItem item) {
  std::lock_guard<std::mutex> lk(mx_);
  item.id = nextId_++;
  items_.push_back(item);
  SaveLocked();
  return item.id;
}

bool Outbox::Remove(uint64_t id) {
  std::lock_guard<std::mutex> lk(mx_);
  for (auto i = items_.begin(); i != items_.end(); ++i) {
    if (i->id == id) { items_.erase(i); SaveLocked(); return true; }
  }
  return false;
}

void Outbox::Clear() {
  std::lock_guard<std::mutex> lk(mx_);
  items_.clear();
  SaveLocked();
}

std::vector<OutboxItem> Outbox::Snapshot() {
  std::lock_guard<std::mutex> lk(mx_);
  return items_;
}

size_t Outbox::Count() {
  std::lock_guard<std::mutex> lk(mx_);
  return items_.size();
}

}  // namespace td
