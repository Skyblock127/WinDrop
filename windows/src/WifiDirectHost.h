#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include "common.h"  // winsock2 before windows.h
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
  // Phones on our Wi-Fi Direct group. This PC owns the group, so several phones can be on it at
  // once, each at its own address; one phone's link coming or going never touches another's.
  std::string PeerIp();  // the newest link's phone address, empty when no phone is on
  bool HasPeer(const std::string& ip);
  std::string IpOf(const std::string& phoneId);  // where that paired phone is, once it has proven itself
  std::vector<std::string> UnprovenIps();        // links that haven't said who they are yet, newest first
  bool IsUnproven(const std::string& ip);
  bool FreshLink(const std::string& ip);         // came up moments ago: the phone may still be setting up
  // The same, with each link's LinkSeq(), so a caller can tell a link it tried from a new one.
  std::vector<std::pair<uint64_t, std::string>> UnprovenLinks();
  std::vector<std::string> LinkedPhones();       // the paired phones on the group
  void Disconnect(const std::string& ip);        // close that phone's link (empty: every link)
  // A link to `ip` just came up (its request arrived at `since`): it has kAuthGraceMs to prove it's
  // a paired phone. A proof that raced ahead of this call (from `ip`, after `since`) counts.
  void LinkUp(ULONGLONG since, const std::string& ip);
  void MarkAuthenticated(const std::string& phoneId, const std::string& ip);
  // Transfers in progress over the link; a busy link isn't handed to another phone.
  std::atomic<int> busy{0};
  // Pairing and unpaired ("nearby") exchanges in progress: the link isn't dropped for lacking a
  // paired phone's proof while one runs (the user may be checking a number or deciding on an offer).
  std::atomic<int> pairExchanges{0};
  static constexpr DWORD kAuthGraceMs = 15000;
  // Bluetooth LE beacon: manufacturer data "TD" + code[0..4] + flag + this PC's Wi-Fi Direct name.
  // Pending (flag 1) asks the phone to open the link now; presence (flag 0) only lets a phone that
  // is pairing learn our name, which it needs to pick us out of its Wi-Fi Direct peer list.
  // Cheap to call every tick; only acts on changes.
  enum Beacon { kBeaconOff, kBeaconPresence, kBeaconPending };
  void SetBeacon(Beacon mode, const std::string& phoneId = "");  // pending: which phone to wake
  // The last beacon couldn't be started (Bluetooth that can't advertise): phones can't find us.
  bool BeaconBroken() const { return beaconBroken_; }
  // Windows refused to advertise over Wi-Fi Direct at all (driver without Wi-Fi Direct).
  bool Unsupported() { std::lock_guard<std::mutex> lk(mx_); return status_ == kUnsupported; }
  static constexpr const wchar_t* kUnsupported = L"not supported here";
  void OpenPairWindow();  // also forgets rejected phones: they may be the one being paired
  void ClosePairWindow();  // a phone just paired, or "Stop pairing"
  void Refresh();         // re-publish now, so a phone searching for us finds us at once
  // The phone at `ip` isn't paired here: drop it and ignore its requests for 10 minutes.
  void RejectPeer(const std::string& ip);
  bool Pairing() const { return GetTickCount64() < pairUntil_; }
  ULONGLONG PairingLeftMs() const { ULONGLONG now = GetTickCount64(); return now < pairUntil_ ? pairUntil_ - now : 0; }
  static constexpr ULONGLONG kPairWindowMs = 180000;
  // While pairing, advertising takes turns of this long: the saved way first, then the other one.
  static constexpr ULONGLONG kPairTurnMs = 30000;
  // Unpaired ("nearby") phones. Visible: for 10 minutes any phone may send to us (each offer is
  // accepted or declined on the pill). Calling: we have something for the nearby phone whose
  // Bluetooth tag this is. Both go out on a second beacon, so pairing and waking keep theirs.
  void SetVisible(bool on) { visibleUntil_ = on ? GetTickCount64() + kVisibleMs : 0; }
  bool Visible() const { return GetTickCount64() < visibleUntil_; }
  ULONGLONG VisibleLeftMs() const { ULONGLONG now = GetTickCount64(); return now < visibleUntil_ ? visibleUntil_ - now : 0; }
  static constexpr ULONGLONG kVisibleMs = 600000;
  // Once the 10 minutes run out by themselves (not switched off), a phone that picked this PC just
  // before still gets through for this long. New phones can't pick it: its beacon is off by then.
  static constexpr ULONGLONG kVisibleGraceMs = 60000;
  bool VisibleOrJustEnded() const {
    ULONGLONG until = visibleUntil_;
    return until && GetTickCount64() < until + kVisibleGraceMs;
  }
  void SetNearbyBeacon(const std::vector<uint8_t>& callTag);  // ticker: call that phone, else say we're visible, else off
  bool Calling() const { return calling_; }
  // A phone we don't have a key for may join now (pairing, visible, or we called it).
  bool OpenToStrangers() const { return Pairing() || VisibleOrJustEnded() || calling_; }
  uint64_t LinkSeq() const { return linkSeq_; }  // changes with every new link
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
  void PhoneFoundUs();  // a phone asked to connect: keep advertising the way it found us
  // A phone's join failed / worked. Joins that keep failing in listen mode get a minute of
  // advertising as group owner (some phones only rejoin a group they know), kept if a phone comes.
  void JoinFailed();
  void JoinWorked();
  bool AsGroupOwner() const { return asGroupOwner_; }


 private:
  void Loop();

  std::atomic<bool> asGroupOwner_{false};  // how the current advertisement runs
  std::atomic<ULONGLONG> goTrialUntil_{0};  // trying group owner after failed joins, until then
  ULONGLONG failedJoinAt_ = 0;              // the last failed join in listen mode; under mx_
  std::atomic<ULONGLONG> pairFrom_{0};     // when "Pair phone…" was opened; turns count from here
  uint8_t beaconFlags_ = 0;                // pairing beacon flags sent (bit 0: Wi-Fi off); under mx_
  std::atomic<bool> beaconBroken_{false};

  std::thread thread_;
  std::mutex mx_;
  std::condition_variable cv_;
  bool stop_ = false;
  std::atomic<bool> running_{false};
  std::wstring status_ = L"off";
  Beacon beacon_ = kBeaconOff;
  std::string beaconPhone_, prewarmPhone_;
  bool refresh_ = false;  // re-publish now (fresh listen state) because we're about to need the phone
  std::atomic<ULONGLONG> pairUntil_{0};
  int64_t beaconWindow_ = 0;
  std::atomic<uint64_t> linkSeq_{0};
  // Last proof / connection by address, under mx_ (see LinkUp).
  std::string lastAuthIp_, lastAuthPhone_, lastSawIp_;
  ULONGLONG lastAuthAt_ = 0, lastSawAt_ = 0;
  std::atomic<ULONGLONG> prewarmUntil_{0};
  std::atomic<ULONGLONG> visibleUntil_{0};
  std::atomic<bool> calling_{false};
  std::string nearbyBeacon_;  // payload of the second beacon now on air; under mx_
};

WifiDirectHost& TheWifiDirect();

}  // namespace td
