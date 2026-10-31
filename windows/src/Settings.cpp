#include "Settings.h"

#include <shlobj.h>

#include <algorithm>
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
  wfdGroupOwner = ReadIni(L"WfdGroupOwner", L"0") == L"1";
  phones.clear();
  int count = _wtoi(ReadIni(L"PhoneCount", L"0").c_str());
  for (int i = 0; i < count && phones.size() < kMaxPhones; ++i) {
    std::wstring k = L"Phone" + std::to_wstring(i);
    PairedPhone p{W2U(ReadIni((k + L"Id").c_str())), ReadIni((k + L"Name").c_str()),
                  UnprotectKey(W2U(ReadIni((k + L"Key").c_str())))};
    if (!p.id.empty() && p.key.size() == 32) phones.push_back(p);
  }
  // From 0.1.x, which had a single paired phone.
  if (count == 0) {
    PairedPhone p{W2U(ReadIni(L"PairedPhoneId")), ReadIni(L"PairedPhoneName"), UnprotectKey(W2U(ReadIni(L"PairKey")))};
    if (!p.id.empty() && p.key.size() == 32) phones.push_back(p);
  }
  for (auto* k : {L"PairedPhoneId", L"PairedPhoneName", L"PairKey", L"PairCode", L"LastPhoneIp"})
    WritePrivateProfileStringW(L"WinDrop", k, nullptr, IniPath().c_str());
  lastPhoneId = W2U(ReadIni(L"LastPhoneId"));
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
  WriteIni(L"WfdGroupOwner", wfdGroupOwner ? L"1" : L"0");
  int old = _wtoi(ReadIni(L"PhoneCount", L"0").c_str());
  WriteIni(L"PhoneCount", std::to_wstring(phones.size()));
  for (size_t i = 0; i < std::max<size_t>(phones.size(), (size_t)old); ++i) {
    std::wstring k = L"Phone" + std::to_wstring(i);
    bool has = i < phones.size();
    auto put = [&](const std::wstring& key, const std::wstring& v) {
      WritePrivateProfileStringW(L"WinDrop", key.c_str(), has ? v.c_str() : nullptr, IniPath().c_str());
    };
    put(k + L"Id", has ? U2W(phones[i].id) : L"");
    put(k + L"Name", has ? phones[i].name : L"");
    put(k + L"Key", has ? U2W(ProtectKey(phones[i].key)) : L"");
  }
  WriteIni(L"LastPhoneId", U2W(lastPhoneId));
  WriteIni(L"DeviceId", U2W(deviceId));
}

const PairedPhone* Settings::Find(const std::string& id) const {
  for (auto& p : phones)
    if (p.id == id) return &p;
  return nullptr;
}

void Settings::AddPhone(const PairedPhone& p) {
  RemovePhone(p.id);
  phones.push_back(p);
}

void Settings::RemovePhone(const std::string& id) {
  phones.erase(std::remove_if(phones.begin(), phones.end(), [&](const PairedPhone& p) { return p.id == id; }), phones.end());
  if (lastPhoneId == id) lastPhoneId.clear();
}

std::string Settings::DefaultPhone() const {
  if (Find(lastPhoneId)) return lastPhoneId;
  return phones.empty() ? "" : phones.front().id;
}

std::vector<PairedPhone> Phones() {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  return Cfg().phones;
}

Bytes KeyFor(const std::string& phoneId) {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  auto* p = Cfg().Find(phoneId);
  return p ? p->key : Bytes{};
}

std::wstring PhoneLabel(const std::string& phoneId) {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  auto* p = Cfg().Find(phoneId);
  return p && !p->name.empty() ? p->name : L"your phone";
}

// Outbox file format: one item per line, "f\t<phone id>\t<utf8 path>" or "t\t<phone id>\t<hex utf8 text>".
// 0.1.x wrote "F\t<path>" / "T\t<hex>" with no phone: those go to the default phone.
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
    std::string rest = line.substr(2);
    char kind = line[0];
    if (kind == 'f' || kind == 't') {
      size_t tab = rest.find('\t');
      if (tab == std::string::npos) continue;
      it.phoneId = rest.substr(0, tab);
      rest = rest.substr(tab + 1);
    }
    if (kind == 'F' || kind == 'f') it.path = U2W(rest);
    else if (kind == 'T' || kind == 't') { it.isText = true; it.text = Unhex(rest); }
    else continue;
    items_.push_back(std::move(it));
  }
}

void Outbox::SaveLocked() {
  FILE* f = _wfopen(file_.c_str(), L"wb");
  if (!f) return;
  for (auto& it : items_) {
    std::string line = (it.isText ? "t\t" : "f\t") + it.phoneId + "\t" + (it.isText ? Hex(it.text) : W2U(it.path));
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

void Outbox::AssignPhone(const std::string& phoneId) {
  std::lock_guard<std::mutex> lk(mx_);
  bool changed = false;
  for (auto& it : items_)
    if (it.phoneId.empty()) { it.phoneId = phoneId; changed = true; }
  if (changed) SaveLocked();
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
