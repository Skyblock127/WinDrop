#include "Radios.h"

#include "common.h"

#include <mutex>
#include <thread>
#include <vector>

#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

namespace td {

using namespace winrt::Windows::Devices::Radios;

namespace {
std::mutex gMx;
std::vector<Radio> gRadios;  // kept alive for their StateChanged events; under gMx
}  // namespace

Radios& TheRadios() {
  static Radios r;
  return r;
}

std::wstring Radios::OffText() const {
  bool wifi = !WifiOn(), bt = !BluetoothOn();
  return wifi && bt ? L"Wi-Fi and Bluetooth are off" : wifi ? L"Wi-Fi is off" : bt ? L"Bluetooth is off" : L"";
}

// Several radios of a kind (two Wi-Fi adapters) count as on if any is. None of a kind (no
// Bluetooth at all) counts as off: WinDrop can't use what isn't there either.
void Radios::Read() {
  int wifi = kOff, bt = kOff;
  {
    std::lock_guard<std::mutex> lk(gMx);
    for (auto const& r : gRadios) {
      bool on = r.State() == RadioState::On;
      if (r.Kind() == RadioKind::WiFi && on) wifi = kOn;
      if (r.Kind() == RadioKind::Bluetooth && on) bt = kOn;
    }
  }
  int oldWifi = wifi_.exchange(wifi), oldBt = bt_.exchange(bt);
  if (oldWifi == wifi && oldBt == bt) return;
  Log("radios: Wi-Fi %s, Bluetooth %s", wifi == kOn ? "on" : "off", bt == kOn ? "on" : "off");
  if (onChange_) onChange_();  // the first answer too: until then both counted as on
}

void Radios::Start(std::function<void()> onChange) {
  onChange_ = std::move(onChange);
  std::thread([this] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
      auto radios = Radio::GetRadiosAsync().get();
      {
        std::lock_guard<std::mutex> lk(gMx);
        for (auto const& r : radios) gRadios.push_back(r);
      }
      for (auto const& r : radios) r.StateChanged([this](auto const&, auto const&) { Read(); });
      if (radios.Size()) Read();
      else Log("radios: Windows lists none; assuming Wi-Fi and Bluetooth are on");
    } catch (winrt::hresult_error const& ex) {
      Log("radios: can't read them (0x%08x); assuming Wi-Fi and Bluetooth are on", (unsigned)ex.code());
    }
  }).detach();
}

}  // namespace td
