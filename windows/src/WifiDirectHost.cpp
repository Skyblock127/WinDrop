#include "WifiDirectHost.h"

#include "common.h"
#include "Crypto.h"
#include "Settings.h"

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
WiFiDirectDevice gPeer{nullptr};  // the link lives as long as this object does
std::string gPeerIp;
std::wstring gPeerName;                          // Wi-Fi Direct name of the phone on the link
std::map<std::wstring, ULONGLONG> gRejectedUntil;  // by Wi-Fi Direct name; under gPeerMx

// Drops a half-made pairing so the next attempt starts clean. A good pairing is kept: re-pairing
// on every link cost ~4.5 s, and the phone asks for temporary groups, so no invitations follow.
void Unpair(winrt::hstring id) {
  if (id.empty()) return;
  std::thread([id] {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
      auto info = DeviceInformation::CreateFromIdAsync(id).get();
      if (info.Pairing().IsPaired()) Log("wfd: unpair %d", (int)info.Pairing().UnpairAsync().get().Status());
    } catch (winrt::hresult_error const& ex) {
      // Temporary groups leave no device record behind: nothing to unpair.
      if (ex.code() != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) Log("wfd: unpair failed 0x%08x", (unsigned)ex.code());
    }
  }).detach();
}

void DropPeer() {
  std::lock_guard<std::mutex> lk(gPeerMx);
  if (gPeer) try { gPeer.Close(); } catch (...) {}
  gPeer = nullptr;
  gPeerIp.clear();
  gPeerName.clear();
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
  gAccepting++;
  struct Done { ~Done() { gAccepting--; } } done;
  auto info = req.DeviceInformation();
  Log("wfd: connection request from %s (paired=%d)", W2U(std::wstring(info.Name())).c_str(), (int)info.Pairing().IsPaired());
  TheWifiDirect().PhoneFoundUs();  // whatever happens next, the phone could find us this way
  // One phone at a time: while another is mid-transfer, say no; it retries in a few seconds. An
  // idle link gives way (its phone reconnects when it next needs us).
  if (!TheWifiDirect().PeerIp().empty() && TheWifiDirect().busy > 0) {
    Log("wfd: refused %s: another phone is transferring", W2U(std::wstring(info.Name())).c_str());
    try { req.Close(); } catch (...) {}
    return;
  }
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    auto r = gRejectedUntil.find(std::wstring(info.Name()));
    if (r != gRejectedUntil.end() && GetTickCount64() < r->second && !TheWifiDirect().Pairing()) {
      Log("wfd: ignored: %s isn't paired here", W2U(std::wstring(info.Name())).c_str());
      try { req.Close(); } catch (...) {}
      return;
    }
  }
  if (!HavePairedPhone() && !TheWifiDirect().Pairing()) {
    Log("wfd: refused: no phone paired and the pairing window is closed");
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
      if (d.ConnectionStatus() == WiFiDirectConnectionStatus::Disconnected) {
        Log("wfd: link down");
        DropPeer();
      }
    });
    {
      std::lock_guard<std::mutex> lk(gPeerMx);
      if (gPeer) try { gPeer.Close(); } catch (...) {}
      gPeer = dev;
      gPeerIp = ip;
      gPeerName = std::wstring(info.Name());
    }
    TheWifiDirect().LinkUp();
    // Push queued items now instead of waiting for the phone to say something over the new link.
    if (auto cb = TheWifiDirect().onLinkUp) cb();
  } catch (winrt::hresult_error const& ex) {
    Log("wfd: accept failed 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
  }
}

}  // namespace

WifiDirectHost& TheWifiDirect() {
  static WifiDirectHost h;
  return h;
}


void WifiDirectHost::LinkUp() {
  uint64_t seq = ++linkSeq_;
  std::thread([this, seq] {
    Sleep(kAuthGraceMs);
    // A pairing phone proves itself only once the user clicks Yes, which can take up to a minute.
    while (linkSeq_ == seq && authedSeq_ != seq && pairExchanges > 0) Sleep(500);
    if (linkSeq_ == seq && authedSeq_ != seq && !PeerIp().empty()) {
      if (sawConnSeq_ != seq) {
        Log("wfd: the phone never connected over the link: a firewall or antivirus on this PC is probably blocking WinDrop");
        if (onUnreachable) onUnreachable();
      }
      Log("wfd: link never proved it's a paired phone, dropping it");
      DropPeer();
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

void WifiDirectHost::SawConnection(const std::string& ip) {
  if (!ip.empty() && ip == PeerIp()) sawConnSeq_ = linkSeq_.load();
}

void WifiDirectHost::MarkAuthenticated(const std::string& phoneId) {
  std::lock_guard<std::mutex> lk(mx_);
  authedSeq_ = linkSeq_.load();
  authedPhone_ = phoneId;
}

std::string WifiDirectHost::LinkedPhone() {
  if (PeerIp().empty()) return "";
  std::lock_guard<std::mutex> lk(mx_);
  return authedSeq_ == linkSeq_ ? authedPhone_ : "";
}

void WifiDirectHost::Refresh() {
  std::lock_guard<std::mutex> lk(mx_);
  refresh_ = true;
  cv_.notify_all();
}

void WifiDirectHost::OpenPairWindow() {
  pairUntil_ = GetTickCount64() + 180000;
  Refresh();  // a phone is about to search for us
  std::lock_guard<std::mutex> lk(gPeerMx);
  gRejectedUntil.clear();
}

void WifiDirectHost::RejectPeer() {
  {
    std::lock_guard<std::mutex> lk(gPeerMx);
    if (gPeerName.empty()) return;
    gRejectedUntil[gPeerName] = GetTickCount64() + 10 * 60 * 1000;
    Log("wfd: %s isn't paired here; ignoring it for 10 minutes", W2U(gPeerName).c_str());
  }
  Disconnect();
}

void WifiDirectHost::Disconnect() {
  // Closing the device object ends the group; do it off the network thread.
  std::thread([] { DropPeer(); }).detach();
}

std::string WifiDirectHost::PeerIp() {
  std::lock_guard<std::mutex> lk(gPeerMx);
  return gPeerIp;
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
//   pairing: "WP" + 0x00 + this PC's Wi-Fi Direct name, only while "Pair phone…" is open,
//            so a phone that is pairing can find us.
void WifiDirectHost::SetBeacon(Beacon mode, const std::string& phoneId) {
  std::lock_guard<std::mutex> lk(mx_);
  int64_t window = BeaconWindow();
  if (mode == beacon_ && (mode != kBeaconPending || (window == beaconWindow_ && phoneId == beaconPhone_))) return;
  bool entering = mode != beacon_ || phoneId != beaconPhone_;
  beacon_ = mode;
  beaconWindow_ = window;
  beaconPhone_ = phoneId;
  try {
    if (gBeacon) { gBeacon.Stop(); gBeacon = nullptr; }
    if (mode == kBeaconOff) { Log("ble: beacon off"); return; }
    std::string data;
    if (mode == kBeaconPending) {
      td::Bytes key = KeyFor(phoneId);
      if (key.empty()) { beacon_ = kBeaconOff; return; }
      td::Bytes tag = BeaconTag(key, window);
      data = "WD" + std::string(tag.begin(), tag.end()) + std::string(1, char(1));
      if (entering) {
        // The phone is about to search for us: make sure Windows is actively answering probes.
        refresh_ = true;
        cv_.notify_all();
      }
    } else {
      data = "WP" + std::string(1, char(0)) + WfdName();
    }
    BluetoothLEAdvertisement adv;
    BluetoothLEManufacturerData md;
    md.CompanyId(kBleCompanyId);
    md.Data(Buf(data));
    adv.ManufacturerData().Append(md);
    gBeacon = BluetoothLEAdvertisementPublisher(adv);
    gBeacon.Start();
    if (entering)
      Log("ble: beacon %s", mode == kBeaconPending ? ("on (wake " + W2U(PhoneLabel(phoneId)) + ")").c_str() : "on (pairing)");
  } catch (winrt::hresult_error const& ex) {
    Log("ble: beacon failed 0x%08x %s", (unsigned)ex.code(), W2U(std::wstring(ex.message())).c_str());
    beacon_ = kBeaconOff;
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
    std::atomic<bool> aborted{false};
    WiFiDirectAdvertisementPublisher pub{nullptr};
    WiFiDirectConnectionListener listener{nullptr};
    try {
      pub = WiFiDirectAdvertisementPublisher();
      auto adv = pub.Advertisement();
      // Normally not an autonomous GO: the phone's connect then runs GO negotiation, which raises
      // ConnectionRequested below. Some Wi-Fi cards (MediaTek MT7921) never answer searches in
      // listen mode, only while running their own group; the phone then joins that group (by
      // push-button or saved credentials, never an invitation, which Windows ignores), which raises
      // ConnectionRequested too. Pairing alternates between the two and keeps what worked.
      bool go;
      {
        std::lock_guard<std::recursive_mutex> lk(CfgMutex());
        go = Cfg().wfdGroupOwner;
      }
      if (Pairing()) {
        if (pairFlip_) go = !go;
        pairFlip_ = !pairFlip_;
      } else {
        pairFlip_ = false;
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
      status_ = L"not supported here";
      aborted = true;
    }

    ULONGLONG began = GetTickCount64();
    bool quit = false, refresh = false;
    {
      std::unique_lock<std::mutex> lk(mx_);
      // Windows quietly stops answering P2P probes a few minutes after Start(); while no phone is
      // linked, re-publish periodically so the phone can always find us (more often while pairing,
      // to try both ways, but not so often that a restart keeps landing in the phone's connect).
      for (;;) {
        auto every = std::chrono::seconds(Pairing() ? 30 : 90);
        if (cv_.wait_for(lk, every, [&] { return stop_ || aborted.load() || refresh_; })) {
          if (stop_ || aborted) break;
          if (PeerIp().empty() && gAccepting == 0) { refresh_ = false; refresh = true; break; }
          if (!PeerIp().empty()) { refresh_ = false; continue; }  // linked: nothing to refresh
          // Still handling a connection request: refresh once it's done.
          lk.unlock();
          Sleep(500);
          lk.lock();
          continue;
        }
        if (PeerIp().empty() && gAccepting == 0) { refresh = true; break; }
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
  DropPeer();
  running_ = false;
  {
    std::lock_guard<std::mutex> lk(mx_);
    status_ = L"off";
  }
  winrt::uninit_apartment();
}

}  // namespace td
