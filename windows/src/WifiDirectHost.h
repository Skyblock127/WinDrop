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
  // A link to `ip` just came up (its request arrived at `since`): it has kAuthGraceMs to prove it's
  // a paired phone. A proof that raced ahead of this call (from `ip`, after `since`) counts.
  void LinkUp(ULONGLONG since, const std::string& ip);
  void MarkAuthenticated(const std::string& phoneId, const std::string& ip);
  std::string LinkedPhone();  // the paired phone on the link, empty if none (or not proven yet)
  // Transfers in progress over the link; a busy link isn't handed to another phone.
  std::atomic<int> busy{0};
  // Pairing exchanges in progress; the link isn't dropped while the user checks the number.
  std::atomic<int> pairExchanges{0};
  static constexpr DWORD kAuthGraceMs = 15000;
  // Bluetooth LE beacon: manufacturer data "TD" + code[0..4] + flag + this PC's Wi-Fi Direct name.
  // Pending (flag 1) asks the phone to open the link now; presence (flag 0) only lets a phone that
  // is pairing learn our name, which it needs to pick us out of its Wi-Fi Direct peer list.
  // Cheap to call every tick; only acts on changes.
  enum Beacon { kBeaconOff, kBeaconPresence, kBeaconPending };
  void SetBeacon(Beacon mode, const std::string& phoneId = "");  // pending: which phone to wake
  void OpenPairWindow();  // also forgets rejected phones: they may be the one being paired
  void Refresh();         // re-publish now, so a phone searching for us finds us at once
  // The phone on the link isn't paired here: drop it and ignore its requests for 10 minutes.
  void RejectPeer();
  bool Pairing() const { return GetTickCount64() < pairUntil_; }
  // A drag is heading for the pill: wake the phone before the drop so the link is ready by then.
  void Prewarm(const std::string& phoneId) {
    std::lock_guard<std::mutex> lk(mx_);
    prewarmPhone_ = phoneId;
    prewarmUntil_ = GetTickCount64() + 25000;
  }
  void EndPrewarm() { prewarmUntil_ = 0; }  // dropped: the send itself keeps the phone wanted
  std::string PrewarmPhone() {  // empty when not prewarming
    std::lock_guard<std::mutex> lk(mx_);
    return GetTickCount64() < prewarmUntil_ ? prewarmPhone_ : "";
  }
  std::function<void()> onLinkUp;  // called on a worker thread once the phone's link is usable
  // Called (worker thread) when a phone got the link but never connected to us over it, which
  // almost always means a firewall or antivirus on this PC blocks WinDrop.
  std::function<void()> onUnreachable;
  void SawConnection(const std::string& ip);  // a TCP connection arrived from this address


 private:
  void Loop();

  std::thread thread_;
  std::mutex mx_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> running_{false};
  std::wstring status_ = L"off";
  Beacon beacon_ = kBeaconOff;
  std::string beaconPhone_, authedPhone_, prewarmPhone_;
  bool refresh_ = false;  // re-publish now (fresh listen state) because we're about to need the phone
  std::atomic<ULONGLONG> pairUntil_{0};
  int64_t beaconWindow_ = 0;
  std::atomic<uint64_t> linkSeq_{0}, authedSeq_{0}, sawConnSeq_{0};
  // Last proof / connection by address, under mx_ (see LinkUp).
  std::string lastAuthIp_, lastAuthPhone_, lastSawIp_;
  ULONGLONG lastAuthAt_ = 0, lastSawAt_ = 0;
  std::atomic<ULONGLONG> prewarmUntil_{0};
};

WifiDirectHost& TheWifiDirect();

}  // namespace td
