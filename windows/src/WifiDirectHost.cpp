#include "WifiDirectHost.h"

#include "common.h"
#include "Crypto.h"
#include "Radios.h"
#include "Settings.h"

#include <algorithm>
#include <functional>
#include <map>

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.WiFiDirect.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.h>
#include <winrt/Windows.Storage.Streams.h>

namespace td {

using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::WiFiDirect;
using winrt::Windows::Storage::Streams::DataWriter;

namespace {

using namespace winrt::Windows::Devices::Bluetooth::Advertisement;

constexpr uint16_t kBleCompanyId = 0xFFFF;  // "no company" id, reserved for testing / internal use

std::atomic<int> gAccepting{0};
std::mutex gPeerMx;
BluetoothLEAdvertisementPublisher gBeacon{nullptr};
BluetoothLEAdvertisementPublisher gNearbyBeacon{nullptr};  // "WE" visible / "WC" calling a nearby phone
// A phone on our group. Its link lives as long as its device object does.
struct Peer {
  WiFiDirectDevice dev{nullptr};
  std::wstring devId;    // Windows' id for that phone: one entry per phone
  std::wstring name;     // its Wi-Fi Direct name
  std::string ip;
  uint64_t seq = 0;      // LinkSeq() when it came up
  std::string phoneId;   // the paired phone it proved to be; empty until then (and for strangers)
  bool sawConn = false;  // it has connected to us over the link
  ULONGLONG at = 0;      // when it came up
};
std::vector<Peer> gPeers;                          // under gPeerMx
std::map<std::wstring, ULONGLONG> gRejectedUntil;  // by Wi-Fi Direct name; under gPeerMx

// Drops a half-made pairing so the next attempt starts clean. A good pairing is kept: re-pairing
// on every link cost ~4.5 s, and the phone asks for temporary groups, so no invitations follow.
// Runs on the Accept() thread, so it's done before the publisher is refreshed for the retry.
void Unpair(winrt::hstring id) {
  if (id.empty()) return;
  try {
    auto info = DeviceInformation::CreateFromIdAsync(id).get();
    if (info.Pairing().IsPaired()) Log("wfd: unpair %d", (int)info.Pairing().UnpairAsync().get().Status());
  } catch (winrt::hresult_error const& ex) {
    // Temporary groups leave no device record behind: nothing to unpair.
    if (ex.code() != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) Log("wfd: unpair failed 0x%08x", (unsigned)ex.code());
  }
}

// Closes the links `pick` chooses (all of them when it's empty), outside the lock: closing can block.
void DropPeers(const std::function<bool(const Peer&)>& pick) {
  std::vector<WiFiDirectDevice> gone;
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    for (auto it = gPeers.begin(); it != gPeers.end();) {
      if (!pick || pick(*it)) {
        gone.push_back(it->dev);
        it = gPeers.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& d : gone)
    if (d) try { d.Close(); } catch (...) {}
}

// That phone's link went down by itself: forget it (the others stay).
void LinkDown(WiFiDirectDevice const& d) {
  std::string ip;
  std::wstring name;
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    for (auto it = gPeers.begin(); it != gPeers.end(); ++it)
      if (winrt::get_abi(it->dev) == winrt::get_abi(d)) {
        ip = it->ip;
        name = it->name;
        gPeers.erase(it);
        break;
      }
  }
  if (!ip.empty()) Log("wfd: link down (%s, %s)", W2U(name).c_str(), ip.c_str());
}

winrt::Windows::Storage::Streams::IBuffer Buf(const std::string& s) {
  DataWriter w;
  for (unsigned char c : s) w.WriteByte(c);
  return w.DetachBuffer();
}

bool HavePairedPhone() {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  return !Cfg().phones.empty();
}

// The phone asked to join: pair without any system prompt (push-button), then open the device to
// bring the link up. Windows can't tell who is asking, so the link alone grants nothing: a link
// that doesn't pass WinDrop's key check within a few seconds is dropped (see LinkUp).
void Accept(WiFiDirectConnectionRequest const& req) {
  ULONGLONG began = GetTickCount64();
  gAccepting++;
  struct Done { ~Done() { gAccepting--; } } done;
  auto info = req.DeviceInformation();
  Log("wfd: connection request from %s (paired=%d)", W2U(std::wstring(info.Name())).c_str(), (int)info.Pairing().IsPaired());
  TheWifiDirect().PhoneFoundUs();  // whatever happens next, the phone could find us this way
  // Another phone already on the group stays on it: each has its own link (see Peer).
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    auto r = gRejectedUntil.find(std::wstring(info.Name()));
    if (r != gRejectedUntil.end() && GetTickCount64() < r->second && !TheWifiDirect().OpenToStrangers()) {
      Log("wfd: ignored: %s isn't paired here", W2U(std::wstring(info.Name())).c_str());
      try { req.Close(); } catch (...) {}
      return;
    }
  }
  if (!HavePairedPhone() && !TheWifiDirect().OpenToStrangers()) {
    Log("wfd: refused: no phone paired, and not pairing or visible to nearby phones");
    // Close it rather than leave it hanging: an unanswered request makes Windows sit on this phone's
    // next requests for about 2 minutes, which would stall the pairing the user starts next.
    try { req.Close(); } catch (...) {}
    return;
  }
  try {
    WiFiDirectConnectionParameters params;
    params.GroupOwnerIntent(15);  // PC stays group owner so the phone keeps its own Wi-Fi
    params.PreferenceOrderedConfigurationMethods().Append(WiFiDirectConfigurationMethod::PushButton);
    if (!info.Pairing().IsPaired()) {
      auto custom = info.Pairing().Custom();
      auto token = custom.PairingRequested([](auto const&, DevicePairingRequestedEventArgs const& a) { a.Accept(); });
      auto op = custom.PairAsync(DevicePairingKinds::ConfirmOnly, DevicePairingProtectionLevel::Default, params);
      // A radio hiccup on the phone can leave this hanging; don't let it block the next attempt.
      if (op.wait_for(std::chrono::seconds(25)) != winrt::Windows::Foundation::AsyncStatus::Completed) {
        op.Cancel();
        custom.PairingRequested(token);
        Log("wfd: pairing timed out");
        TheWifiDirect().JoinFailed();
        TheWifiDirect().Refresh();  // the phone retries: be findable again straight away
        Unpair(info.Id());
        return;
      }
      auto r = op.GetResults();
      custom.PairingRequested(token);
      Log("wfd: pairing result %d", (int)r.Status());
      // A phone that joined our group before keeps its credentials and joins again without the
      // push-button step, so Windows' pairing fails though the phone is on the group: open it anyway.
      if (r.Status() != DevicePairingResultStatus::Paired && r.Status() != DevicePairingResultStatus::AlreadyPaired &&
          !TheWifiDirect().AsGroupOwner()) {
        Unpair(info.Id());  // half-made pairing would block the next attempt
        TheWifiDirect().JoinFailed();
        TheWifiDirect().Refresh();
        return;
      }
    }
    auto open = WiFiDirectDevice::FromIdAsync(info.Id(), params);
    // A failed open usually means Windows' saved pairing for this phone went stale: drop it so the
    // next attempt pairs afresh instead of failing the same way.
    if (open.wait_for(std::chrono::seconds(20)) != winrt::Windows::Foundation::AsyncStatus::Completed) {
      open.Cancel();
      Log("wfd: opening the link timed out");
      TheWifiDirect().JoinFailed();
      Unpair(info.Id());
      TheWifiDirect().Refresh();
      return;
    }
    auto dev = open.GetResults();
    if (!dev) {
      Log("wfd: FromIdAsync returned null");
      Unpair(info.Id());
      TheWifiDirect().Refresh();
      return;
    }
    std::string ip;
    for (auto const& pair : dev.GetConnectionEndpointPairs()) {
      ip = W2U(std::wstring(pair.RemoteHostName().CanonicalName()));
      Log("wfd: link up, phone %s, pc %s", ip.c_str(), W2U(std::wstring(pair.LocalHostName().CanonicalName())).c_str());
      break;
    }
    dev.ConnectionStatusChanged([](WiFiDirectDevice const& d, auto const&) {
      if (d.ConnectionStatus() == WiFiDirectConnectionStatus::Disconnected) LinkDown(d);
    });
    std::wstring devId(info.Id());
    std::vector<WiFiDirectDevice> old;
    {
      // The same phone again (it rejoined): its old link object goes. Other phones' links stay:
      // closing one of theirs here is what made two phones knock each other off, every few seconds.
      std::lock_guard<std::mutex> lk(gPeerMx);
      for (auto it = gPeers.begin(); it != gPeers.end();) {
        if (it->devId == devId || it->ip == ip) {
          old.push_back(it->dev);
          it = gPeers.erase(it);
        } else {
          ++it;
        }
      }
      Peer p;
      p.dev = dev;
      p.devId = devId;
      p.name = std::wstring(info.Name());
      p.ip = ip;
      p.at = GetTickCount64();
      gPeers.push_back(std::move(p));
    }
    for (auto& o : old)
      if (o && winrt::get_abi(o) != winrt::get_abi(dev)) try { o.Close(); } catch (...) {}
    TheWifiDirect().JoinWorked();
    TheWifiDirect().LinkUp(began, ip);
    // Push queued items now instead of waiting for the phone to say something over the new link.
    if (auto cb = TheWifiDirect().onLinkUp) cb();
  } catch (winrt::hresult_error const& ex) {
    Log("wfd: accept failed 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
    Unpair(info.Id());
    TheWifiDirect().Refresh();
  }
}

}  // namespace

WifiDirectHost& TheWifiDirect() {
  static WifiDirectHost h;
  return h;
}


void WifiDirectHost::LinkUp(ULONGLONG since, const std::string& ip) {
  uint64_t seq;
  std::string authed;
  bool saw;
  {
    // The phone can get its address and connect before Accept() gets here.
    std::lock_guard<std::mutex> lk(mx_);
    seq = ++linkSeq_;
    authed = lastAuthIp_ == ip && lastAuthAt_ >= since ? lastAuthPhone_ : "";
    saw = lastSawIp_ == ip && lastSawAt_ >= since;
  }
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    for (auto& p : gPeers)
      if (p.ip == ip) {
        p.seq = seq;
        if (!authed.empty()) p.phoneId = authed;
        p.sawConn = p.sawConn || saw;
      }
  }
  std::thread([this, seq, ip] {
    Sleep(kAuthGraceMs);
    // A pairing phone proves itself only once the user clicks Yes, which can take up to a minute;
    // a nearby (unpaired) phone never does, but its exchange keeps it (pairExchanges).
    for (;;) {
      bool on = false, proven = false, saw = false;
      {
        std::lock_guard<std::mutex> lk(gPeerMx);
        for (auto& p : gPeers)
          if (p.seq == seq) {
            on = true;
            proven = !p.phoneId.empty();
            saw = p.sawConn;
          }
      }
      if (!on || proven) return;
      if (pairExchanges > 0 || busy > 0) {
        Sleep(500);
        continue;
      }
      if (!saw) {
        Log("wfd: the phone never connected over the link: a firewall or antivirus on this PC is probably blocking WinDrop");
        if (onUnreachable) onUnreachable();
      }
      Log("wfd: link to %s never proved it's a paired phone, dropping it", ip.c_str());
      DropPeers([seq](const Peer& p) { return p.seq == seq; });
      return;
    }
  }).detach();
}

void WifiDirectHost::PhoneFoundUs() {
  bool go = asGroupOwner_;
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  if (Cfg().wfdGroupOwner == go) return;
  Cfg().wfdGroupOwner = go;
  Cfg().Save();
  Log("wfd: the phone found this PC %s; advertising that way from now on", go ? "as group owner" : "in listen mode");
}

void WifiDirectHost::JoinFailed() {
  if (asGroupOwner_ || Pairing()) return;  // pairing takes turns by itself
  ULONGLONG now = GetTickCount64();
  bool again;
  {
    std::lock_guard<std::mutex> lk(mx_);
    again = failedJoinAt_ && now - failedJoinAt_ < 180000;
    failedJoinAt_ = now;
  }
  if (!again || now < goTrialUntil_) return;
  goTrialUntil_ = now + 60000;
  Log("wfd: joins keep failing in listen mode; advertising as group owner for a minute");
  Refresh();
}

void WifiDirectHost::JoinWorked() {
  std::lock_guard<std::mutex> lk(mx_);
  failedJoinAt_ = 0;
}

// Both note the address under mx_ first (LinkUp reads it there), then mark the link if it's
// registered already.
void WifiDirectHost::SawConnection(const std::string& ip) {
  {
    std::lock_guard<std::mutex> lk(mx_);
    lastSawIp_ = ip;
    lastSawAt_ = GetTickCount64();
  }
  std::lock_guard<std::mutex> lk(gPeerMx);
  for (auto& p : gPeers)
    if (p.ip == ip) p.sawConn = true;
}

void WifiDirectHost::MarkAuthenticated(const std::string& phoneId, const std::string& ip) {
  {
    std::lock_guard<std::mutex> lk(mx_);
    lastAuthIp_ = ip;
    lastAuthPhone_ = phoneId;
    lastAuthAt_ = GetTickCount64();
  }
  std::lock_guard<std::mutex> lk(gPeerMx);
  for (auto& p : gPeers) {
    if (p.ip == ip) p.phoneId = phoneId;
  }
}

std::string WifiDirectHost::PeerIp() {
  std::lock_guard<std::mutex> lk(gPeerMx);
  const Peer* best = nullptr;
  for (auto& p : gPeers)
    if (!best || p.seq > best->seq) best = &p;
  return best ? best->ip : "";
}

bool WifiDirectHost::HasPeer(const std::string& ip) {
  std::lock_guard<std::mutex> lk(gPeerMx);
  return std::any_of(gPeers.begin(), gPeers.end(), [&](const Peer& p) { return p.ip == ip; });
}

std::string WifiDirectHost::IpOf(const std::string& phoneId) {
  if (phoneId.empty()) return "";
  std::lock_guard<std::mutex> lk(gPeerMx);
  const Peer* best = nullptr;
  for (auto& p : gPeers)
    if (p.phoneId == phoneId && (!best || p.seq > best->seq)) best = &p;
  return best ? best->ip : "";
}

std::vector<std::pair<uint64_t, std::string>> WifiDirectHost::UnprovenLinks() {
  std::vector<std::pair<uint64_t, std::string>> v;
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    for (auto& p : gPeers)
      if (p.phoneId.empty()) v.push_back({p.seq, p.ip});
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  return v;
}

std::vector<std::string> WifiDirectHost::UnprovenIps() {
  std::vector<std::string> ips;
  for (auto& [seq, ip] : UnprovenLinks()) ips.push_back(ip);
  return ips;
}

bool WifiDirectHost::FreshLink(const std::string& ip) {
  std::lock_guard<std::mutex> lk(gPeerMx);
  ULONGLONG now = GetTickCount64();
  return std::any_of(gPeers.begin(), gPeers.end(), [&](const Peer& p) { return p.ip == ip && now - p.at < 8000; });
}

bool WifiDirectHost::IsUnproven(const std::string& ip) {
  std::lock_guard<std::mutex> lk(gPeerMx);
  return std::any_of(gPeers.begin(), gPeers.end(), [&](const Peer& p) { return p.ip == ip && p.phoneId.empty(); });
}

std::vector<std::string> WifiDirectHost::LinkedPhones() {
  std::vector<std::string> ids;
  std::lock_guard<std::mutex> lk(gPeerMx);
  for (auto& p : gPeers)
    if (!p.phoneId.empty() && std::find(ids.begin(), ids.end(), p.phoneId) == ids.end()) ids.push_back(p.phoneId);
  return ids;
}

void WifiDirectHost::Refresh() {
  std::lock_guard<std::mutex> lk(mx_);
  refresh_ = true;
  cv_.notify_all();
}

void WifiDirectHost::OpenPairWindow() {
  ULONGLONG now = GetTickCount64();
  pairFrom_ = now;
  pairUntil_ = now + kPairWindowMs;
  Refresh();  // a phone is about to search for us: start the first turn now
  std::lock_guard<std::mutex> lk(gPeerMx);
  gRejectedUntil.clear();
}

void WifiDirectHost::ClosePairWindow() {
  if (!Pairing()) return;
  pairUntil_ = 0;
  Log("wfd: pairing window closed");
  Refresh();  // back to advertising the saved way (only once no phone is linked)
}

void WifiDirectHost::RejectPeer(const std::string& ip) {
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    auto it = std::find_if(gPeers.begin(), gPeers.end(), [&](const Peer& p) { return p.ip == ip; });
    if (it == gPeers.end() || it->name.empty()) return;
    gRejectedUntil[it->name] = GetTickCount64() + 10 * 60 * 1000;
    Log("wfd: %s isn't paired here; ignoring it for 10 minutes", W2U(it->name).c_str());
  }
  Disconnect(ip);
}

void WifiDirectHost::Disconnect(const std::string& ip) {
  // Closing a device object can block: do it off the network thread.
  std::thread([ip] { DropPeers([&](const Peer& p) { return ip.empty() || p.ip == ip; }); }).detach();
}

// The name Windows uses in Wi-Fi Direct discovery (NetBIOS computer name), cut to what fits in a
// legacy 31-byte advert after flags, the company id and our 7-byte header.
std::string WfdName() {
  wchar_t n[MAX_COMPUTERNAME_LENGTH + 1];
  DWORD len = ARRAYSIZE(n);
  std::string s = GetComputerNameW(n, &len) ? W2U(n) : "";
  if (s.size() <= 17) return s;
  size_t cut = 17;
  while (cut > 0 && (uint8_t(s[cut]) & 0xC0) == 0x80) --cut;  // don't split a UTF-8 character
  return s.substr(0, cut);
}

// Beacon payloads (manufacturer data, company 0xFFFF):
//   pending: "WD" + 6-byte tag + 0x01. The tag is HMAC(pairing key, 10-minute window), so only the
//            paired phone recognises it and it can't be used to follow this PC around.
//   pairing: "WP" + flags + this PC's Wi-Fi Direct name, only while "Pair phone…" is open, so a
//            phone that is pairing can find us. Flags bit 0: Wi-Fi is off here, so the phone can
//            say so instead of searching in vain (phones before 0.2.6 ignore the byte).
void WifiDirectHost::SetBeacon(Beacon mode, const std::string& phoneId) {
  std::lock_guard<std::mutex> lk(mx_);
  int64_t window = BeaconWindow();
  uint8_t flags = TheRadios().WifiOn() ? 0 : 1;
  if (mode == beacon_ && (mode == kBeaconPending ? window == beaconWindow_ && phoneId == beaconPhone_
                                                 : mode == kBeaconOff || flags == beaconFlags_))
    return;
  if (mode == kBeaconPresence && beacon_ == kBeaconPresence && flags != beaconFlags_)
    Log("ble: pairing beacon now says Wi-Fi is %s", flags ? "off" : "on");
  bool entering = mode != beacon_ || phoneId != beaconPhone_;
  beacon_ = mode;
  beaconWindow_ = window;
  beaconPhone_ = phoneId;
  beaconFlags_ = flags;
  try {
    if (gBeacon) { gBeacon.Stop(); gBeacon = nullptr; }
    if (mode == kBeaconOff) { Log("ble: beacon off"); return; }
    std::string data;
    if (mode == kBeaconPending) {
      td::Bytes key = KeyFor(phoneId);
      if (key.empty()) { beacon_ = kBeaconOff; return; }
      td::Bytes tag = BeaconTag(key, window);
      data = "WD" + std::string(tag.begin(), tag.end()) + std::string(1, char(1));
      if (entering && !asGroupOwner_) {
        // The phone is about to search for us: make sure Windows is actively answering probes. (A
        // group owner answers anyway; restarting it would only take its group down.)
        refresh_ = true;
        cv_.notify_all();
      }
    } else {
      data = "WP" + std::string(1, char(flags)) + WfdName();
    }
    BluetoothLEAdvertisement adv;
    BluetoothLEManufacturerData md;
    md.CompanyId(kBleCompanyId);
    md.Data(Buf(data));
    adv.ManufacturerData().Append(md);
    gBeacon = BluetoothLEAdvertisementPublisher(adv);
    gBeacon.Start();
    beaconBroken_ = false;
    if (entering)
      Log("ble: beacon %s", mode == kBeaconPending ? ("on (wake " + W2U(PhoneLabel(phoneId)) + ")").c_str() : "on (pairing)");
  } catch (winrt::hresult_error const& ex) {
    // Retried on the next tick; say it once, not every second.
    if (!beaconBroken_.exchange(true))
      Log("ble: this PC can't broadcast the beacon: 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
    beacon_ = kBeaconOff;
  }
}

// Second beacon: "WC" + tag[4] + flags + name calls a nearby phone that has something coming;
// otherwise "WE" + flags + name while visible. Flags bit 0: this PC's Wi-Fi is off.
void WifiDirectHost::SetNearbyBeacon(const std::vector<uint8_t>& callTag) {
  calling_ = callTag.size() == 4;
  std::string data;
  std::string flags(1, char(TheRadios().WifiOn() ? 0 : 1));
  if (calling_) data = "WC" + std::string(callTag.begin(), callTag.end()) + flags + WfdName();
  else if (Visible()) data = "WE" + flags + WfdName();
  std::lock_guard<std::mutex> lk(mx_);
  if (data == nearbyBeacon_) return;
  nearbyBeacon_ = data;
  try {
    if (gNearbyBeacon) { gNearbyBeacon.Stop(); gNearbyBeacon = nullptr; }
    if (data.empty()) { Log("ble: nearby beacon off"); return; }
    BluetoothLEAdvertisement adv;
    BluetoothLEManufacturerData md;
    md.CompanyId(kBleCompanyId);
    md.Data(Buf(data));
    adv.ManufacturerData().Append(md);
    gNearbyBeacon = BluetoothLEAdvertisementPublisher(adv);
    gNearbyBeacon.Start();
    Log("ble: nearby beacon %s", calling_ ? "calling a nearby phone" : "on (visible)");
  } catch (winrt::hresult_error const& ex) {
    Log("ble: nearby beacon failed: 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
    nearbyBeacon_.clear();
  }
}

void WifiDirectHost::Start() {
  Stop();
  stop_ = false;
  thread_ = std::thread(&WifiDirectHost::Loop, this);
}

void WifiDirectHost::Stop() {
  {
    std::lock_guard<std::mutex> lk(mx_);
    stop_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

std::wstring WifiDirectHost::Status() {
  std::lock_guard<std::mutex> lk(mx_);
  std::string ip = PeerIp();
  return ip.empty() ? status_ : L"phone connected (" + U2W(ip) + L")";
}

void WifiDirectHost::Loop() {
  winrt::init_apartment(winrt::apartment_type::multi_threaded);
  int backoff = 2;
  for (;;) {
    // Wi-Fi off: nothing can answer a phone (a group-owner advertisement is aborted at once, one in
    // listen mode only seems to start). Wait for it; Radios calls Refresh() when it comes back.
    if (!TheRadios().WifiOn()) {
      std::unique_lock<std::mutex> lk(mx_);
      if (status_ != L"Wi-Fi is off") Log("wfd: Wi-Fi is off; not advertising until it's back");
      status_ = L"Wi-Fi is off";
      running_ = false;
      cv_.wait(lk, [&] { return stop_ || refresh_; });
      refresh_ = false;
      if (stop_) break;
      continue;
    }
    std::atomic<bool> aborted{false};
    ULONGLONG turnEnds = 0;  // pairing: when this turn's way of advertising ends
    bool go = false;         // advertising as group owner (else in listen mode)
    WiFiDirectAdvertisementPublisher pub{nullptr};
    WiFiDirectConnectionListener listener{nullptr};
    try {
      pub = WiFiDirectAdvertisementPublisher();
      auto adv = pub.Advertisement();
      // Normally not an autonomous GO: the phone's connect then runs GO negotiation, which raises
      // ConnectionRequested below. Some Wi-Fi cards (MediaTek MT7921) never answer searches in
      // listen mode, only while running their own group; the phone then joins that group (by
      // push-button or saved credentials, never an invitation, which Windows ignores), which raises
      // ConnectionRequested too. While pairing the two take turns on a fixed clock from when
      // "Pair phone…" was opened (restarts within a turn keep its way), and the way a phone came
      // in is kept (PhoneFoundUs).
      {
        std::lock_guard<std::recursive_mutex> lk(CfgMutex());
        go = Cfg().wfdGroupOwner;
      }
      if (Pairing()) {
        ULONGLONG turn = (GetTickCount64() - pairFrom_) / kPairTurnMs;
        if (turn % 2) go = !go;
        turnEnds = pairFrom_ + (turn + 1) * kPairTurnMs;
      } else if (!go && GetTickCount64() < goTrialUntil_) {
        go = true;  // a trial turn (JoinFailed); the saved way again after it unless a phone came
        turnEnds = goTrialUntil_;
      }
      asGroupOwner_ = go;
      adv.IsAutonomousGroupOwnerEnabled(go);
      adv.ListenStateDiscoverability(WiFiDirectAdvertisementListenStateDiscoverability::Intensive);

      listener = WiFiDirectConnectionListener();
      listener.ConnectionRequested([](auto const&, WiFiDirectConnectionRequestedEventArgs const& e) {
        WiFiDirectConnectionRequest req = e.GetConnectionRequest();
        std::thread([req] {
          winrt::init_apartment(winrt::apartment_type::multi_threaded);
          Accept(req);
        }).detach();
      });

      pub.StatusChanged([this, &aborted](auto const&, WiFiDirectAdvertisementPublisherStatusChangedEventArgs const& e) {
        auto st = e.Status();
        Log("wfd: status %d error %d", (int)st, (int)e.Error());
        if (st == WiFiDirectAdvertisementPublisherStatus::Started) {
          running_ = true;
          std::lock_guard<std::mutex> lk(mx_);
          status_ = L"waiting for phone";
        } else if (st == WiFiDirectAdvertisementPublisherStatus::Aborted ||
                   st == WiFiDirectAdvertisementPublisherStatus::Stopped) {
          running_ = false;
          aborted = true;
          {
            std::lock_guard<std::mutex> lk(mx_);
            status_ = e.Error() == WiFiDirectError::RadioNotAvailable ? L"Wi-Fi is off" : L"restarting";
          }
          cv_.notify_all();
        }
      });
      pub.Start();
      Log("wfd: publishing%s", go ? " as group owner" : "");
    } catch (winrt::hresult_error const& ex) {
      Log("wfd: start failed 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
      std::lock_guard<std::mutex> lk(mx_);
      status_ = kUnsupported;
      aborted = true;
    }

    ULONGLONG began = GetTickCount64();
    bool quit = false, refresh = false;
    {
      std::unique_lock<std::mutex> lk(mx_);
      // When to start over (asked to via Refresh(), or on a timer):
      // - Wi-Fi went off: always, linked or not (the link went with it); the loop's top waits.
      // - Pairing: at the end of each turn, to advertise the other way.
      // - Listen mode: every 90 s, since Windows quietly stops answering P2P probes a few minutes
      //   after Start(). A group owner keeps beaconing by itself, so it never starts over on a
      //   timer: a phone joins its group before ConnectionRequested arrives (so gAccepting can't
      //   cover it) and a restart then would pull the group out from under the phone.
      // Never while a phone is linked, nor in the middle of a phone's connect (gAccepting).
      bool renews = turnEnds || !go;
      for (;;) {
        ULONGLONG now = GetTickCount64();
        ULONGLONG ms = turnEnds ? (turnEnds > now ? turnEnds - now : 500) : 90000;
        bool asked = cv_.wait_for(lk, std::chrono::milliseconds(ms), [&] { return stop_ || aborted.load() || refresh_; });
        if (stop_ || aborted) break;
        if (!TheRadios().WifiOn()) { refresh_ = false; refresh = true; break; }
        if (!asked && !renews) continue;                        // a group owner's timer: nothing to renew
        if (!PeerIp().empty()) { refresh_ = false; continue; }  // linked: nothing to renew
        if (gAccepting == 0) { refresh_ = false; refresh = true; break; }
        // Still handling a connection request: start over once it's done.
        lk.unlock();
        Sleep(500);
        lk.lock();
      }
      quit = stop_;
    }
    if (pub) try { pub.Stop(); } catch (...) {}
    pub = nullptr;
    listener = nullptr;
    if (quit) break;
    if (refresh) {
      backoff = 2;
      Sleep(400);  // restarting at once sometimes aborts the new publisher
      continue;
    }
    if (GetTickCount64() - began > 60000) backoff = 2;
    // Radio off, sleep, adapter reset: come back with a capped back-off.
    std::unique_lock<std::mutex> lk(mx_);
    if (cv_.wait_for(lk, std::chrono::seconds(backoff), [&] { return stop_; })) break;
    backoff = std::min(backoff * 2, 60);
  }
  DropPeers(nullptr);
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(mx_);
    status_ = L"off";
  }
  winrt::uninit_apartment();
}

}  // namespace td
