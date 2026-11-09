#include "Nearby.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "Settings.h"
#include "common.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

namespace td {

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

namespace {

constexpr uint16_t kCompany = 0xFFFF;
constexpr ULONGLONG kForgetMs = 12000;  // a phone not heard this long has gone (or stopped)

struct Seen {
  std::wstring name;
  ULONGLONG at = 0;
};

std::mutex gMx;
std::map<std::string, Seen> gSeen;  // by hex tag
BluetoothLEAdvertisementWatcher gWatcher{nullptr};

std::string Hex(const uint8_t* p, size_t n) {
  static const char* d = "0123456789abcdef";
  std::string s;
  for (size_t i = 0; i < n; ++i) { s += d[p[i] >> 4]; s += d[p[i] & 15]; }
  return s;
}

void OnAdvert(BluetoothLEAdvertisementReceivedEventArgs const& e) {
  for (auto const& md : e.Advertisement().ManufacturerData()) {
    if (md.CompanyId() != kCompany) continue;
    auto buf = md.Data();
    if (buf.Length() < 8 || buf.data()[0] != 'W' || buf.data()[1] != 'V') continue;
    const uint8_t* p = buf.data();
    std::string tag = Hex(p + 3, 4);
    std::string name((const char*)p + 7, buf.Length() - 7);
    std::lock_guard<std::mutex> lk(gMx);
    auto& s = gSeen[tag];
    if (s.at == 0) Log("nearby: %s is visible", name.c_str());
    s.name = U2W(name);
    s.at = GetTickCount64();
  }
}

}  // namespace

Bytes NearbyTagFor(const std::string& deviceId) {
  Bytes h = Sha256(Str("WV" + deviceId));
  return Bytes(h.begin(), h.begin() + 4);
}

void StartNearbyWatch() {
  if (gWatcher) return;
  try {
    gWatcher = BluetoothLEAdvertisementWatcher();
    gWatcher.ScanningMode(BluetoothLEScanningMode::Passive);
    gWatcher.Received([](auto const&, BluetoothLEAdvertisementReceivedEventArgs const& e) { OnAdvert(e); });
    gWatcher.Start();
  } catch (winrt::hresult_error const& ex) {
    Log("nearby: can't listen for phones: 0x%08x", (unsigned)ex.code());
    gWatcher = nullptr;
  }
}

void StopNearbyWatch() {
  if (gWatcher) try { gWatcher.Stop(); } catch (...) {}
  gWatcher = nullptr;
  std::lock_guard<std::mutex> lk(gMx);
  gSeen.clear();
}

std::vector<NearbyPhone> NearbyPhones() {
  std::vector<std::string> paired;
  for (auto& p : Phones()) {
    Bytes t = NearbyTagFor(p.id);
    paired.push_back(Hex(t.data(), t.size()));
  }
  std::vector<NearbyPhone> out;
  ULONGLONG now = GetTickCount64();
  std::lock_guard<std::mutex> lk(gMx);
  for (auto& [tag, s] : gSeen)
    if (now - s.at < kForgetMs && std::find(paired.begin(), paired.end(), tag) == paired.end())
      out.push_back({"nearby:" + tag, s.name});
  return out;
}

bool IsNearbyId(const std::string& id) { return id.rfind("nearby:", 0) == 0; }

std::wstring NearbyName(const std::string& id) {
  if (!IsNearbyId(id)) return L"";
  std::lock_guard<std::mutex> lk(gMx);
  auto it = gSeen.find(id.substr(7));
  return it == gSeen.end() ? L"nearby phone" : it->second.name;
}

Bytes NearbyTag(const std::string& id) {
  Bytes t;
  if (!IsNearbyId(id) || id.size() != 15) return t;
  for (size_t i = 7; i + 1 < id.size(); i += 2) t.push_back((uint8_t)std::stoi(id.substr(i, 2), nullptr, 16));
  return t;
}

}  // namespace td
