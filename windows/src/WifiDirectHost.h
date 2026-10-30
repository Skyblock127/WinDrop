#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include "common.h"  // winsock2 before windows.h
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace td {

// Advertises this PC over Wi-Fi Direct and accepts the paired phone's connection requests, so the
// two get a direct link on any network without the PC leaving its own Wi-Fi. The phone finds us by
// the name in our BLE beacon; see android/.../WifiDirectLink.kt for the other half.
class WifiDirectHost {
 public:
  void Start();
  void Stop();
  bool Running() const { return running_; }
  std::wstring Status();
  std::string PeerIp();  // phone's address on the Wi-Fi Direct link, empty when not connected
  void Disconnect();     // tear our side of the link down now (the phone said bye)
  void LinkUp();         // a link just came up: it has kAuthGraceMs to prove it's the paired phone
  void MarkAuthenticated();
  static constexpr DWORD kAuthGraceMs = 15000;
  // Bluetooth LE beacon: manufacturer data "TD" + code[0..4] + flag + this PC's Wi-Fi Direct name.
  // Pending (flag 1) asks the phone to open the link now; presence (flag 0) only lets a phone that
  // is pairing learn our name, which it needs to pick us out of its Wi-Fi Direct peer list.
  // Cheap to call every tick; only acts on changes.
  enum Beacon { kBeaconOff, kBeaconPresence, kBeaconPending };
  void SetBeacon(Beacon mode);
  void OpenPairWindow() { pairUntil_ = GetTickCount64() + 180000; }
  bool Pairing() const { return GetTickCount64() < pairUntil_; }
  // A drag is heading for the pill: wake the phone before the drop so the link is ready by then.
  void Prewarm() { prewarmUntil_ = GetTickCount64() + 25000; }
  bool Prewarming() const { return GetTickCount64() < prewarmUntil_; }
  std::function<void()> onLinkUp;  // called on a worker thread once the phone's link is usable


 private:
  void Loop();

  std::thread thread_;
  std::mutex mx_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> running_{false};
  std::wstring status_ = L"off";
  Beacon beacon_ = kBeaconOff;
  bool refresh_ = false;  // re-publish now (fresh listen state) because we're about to need the phone
  std::atomic<ULONGLONG> pairUntil_{0};
  int64_t beaconWindow_ = 0;
  std::atomic<uint64_t> linkSeq_{0}, authedSeq_{0};
  std::atomic<ULONGLONG> prewarmUntil_{0};
};

WifiDirectHost& TheWifiDirect();

}  // namespace td
