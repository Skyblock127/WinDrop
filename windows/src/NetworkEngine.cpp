#include "NetworkEngine.h"
#include "Nearby.h"

#include <algorithm>
#include <cwchar>
#include <functional>

#include "Crypto.h"
#include "Radios.h"
#include "Settings.h"
#include "WifiDirectHost.h"

namespace td {
namespace {

// Inside an authenticated, encrypted channel (see Crypto.h), one item per connection:
//   OFFER "TDRP" ver=1 | type | size u64 | nameLen u16 | senderLen u16 | idLen u16 | name | sender | id
//   DECISION u8 · STREAM size bytes · ACK u8
// Types: 1 file, 2 text, 3 the phone's log lines (appended to ours), 4 the phone closes the link,
// 5 (PC to phone) another phone is waiting: let the link go. Phones before 0.2 answer 5 with "busy".
constexpr uint8_t kTypeFile = 1, kTypeText = 2, kTypeLog = 3, kTypeBye = 4, kTypeRelease = 5;
constexpr uint8_t kAccepted = 1, kDeclined = 2, kBusy = 3;
constexpr uint8_t kAckOk = 0x00, kAckBad = 0xFF;
constexpr size_t kChunk = 1 << 20;     // big reads/writes: fewer syscalls, full records
constexpr int kSockBuf = 4 << 20;      // deep socket buffers keep the radio busy
constexpr int kMaxHandlers = 6;         // incoming connections handled at once
constexpr uint64_t kMaxText = 16ull << 20;

void PutU16(std::string& b, uint16_t v) { b += char(v >> 8); b += char(v); }
void PutU64(std::string& b, uint64_t v) { for (int i = 7; i >= 0; --i) b += char(v >> (i * 8)); }
uint16_t GetU16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint64_t GetU64(const uint8_t* p) { uint64_t v = 0; for (int i = 0; i < 8; ++i) v = v << 8 | p[i]; return v; }

void SetTimeouts(SOCKET s, DWORD ms) {
  setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
  setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&ms, sizeof ms);
}

void Tune(SOCKET s) {
  setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char*)&kSockBuf, sizeof kSockBuf);
  setsockopt(s, SOL_SOCKET, SO_RCVBUF, (const char*)&kSockBuf, sizeof kSockBuf);
  BOOL on = TRUE;
  setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&on, sizeof on);
}

std::string BuildOffer(uint8_t type, uint64_t size, const std::string& name, const std::string& sender,
                       const std::string& id) {
  std::string b = "TDRP";
  b += char(1);
  b += char(type);
  PutU64(b, size);
  PutU16(b, (uint16_t)name.size());
  PutU16(b, (uint16_t)sender.size());
  PutU16(b, (uint16_t)id.size());
  return b + name + sender + id;
}

std::wstring SanitizeName(std::wstring n) {
  for (auto& c : n)
    if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
  while (!n.empty() && (n.back() == L'.' || n.back() == L' ')) n.pop_back();
  return n.empty() ? L"file" : n;
}

std::wstring UniquePath(const std::wstring& dir, const std::wstring& name) {
  std::wstring base = name, ext;
  size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos && dot > 0) { base = name.substr(0, dot); ext = name.substr(dot); }
  std::wstring p = dir + L"\\" + name;
  for (int i = 1; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; ++i)
    p = dir + L"\\" + base + L" (" + std::to_wstring(i) + L")" + ext;
  return p;
}

std::string IpStr(const in_addr& a) {
  char b[INET_ADDRSTRLEN] = {};
  inet_ntop(AF_INET, &a, b, sizeof b);
  return b;
}

// Counts a transfer in progress, so the link isn't handed to another phone under it.
struct BusyLink {
  BusyLink() { TheWifiDirect().busy++; }
  ~BusyLink() { TheWifiDirect().busy--; }
};

}  // namespace

void NetworkEngine::Post(UiEvent* e) {
  if (!PostMessageW(notify_, WM_APP_NET, 0, (LPARAM)e)) delete e;
}

bool NetworkEngine::OpenListener() {
  std::lock_guard<std::mutex> lk(lmx_);
  if (listen_ != INVALID_SOCKET) return true;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kTcpPort);
  a.sin_addr.s_addr = INADDR_ANY;
  Tune(s);
  if (bind(s, (sockaddr*)&a, sizeof a) != 0 || listen(s, 8) != 0) {
    Log("TCP bind/listen on %d failed: %d", kTcpPort, WSAGetLastError());
    closesocket(s);
    return false;
  }
  listen_ = s;
  return true;
}

bool NetworkEngine::Start(HWND notify) {
  notify_ = notify;
  TheOutbox().Load(Cfg().dataDir + L"\\outbox.txt");
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    TheOutbox().AssignPhone(Cfg().DefaultPhone());  // items queued by 0.1.x
  }
  bool ok = !enabled || OpenListener();
  // Paired phones are each on their own link, so each gets its own sender: a file for one never
  // waits behind a file for another.
  for (size_t i = 0; i < kMaxPhones; ++i) threads_.emplace_back(&NetworkEngine::SenderLoop, this);
  threads_.emplace_back(&NetworkEngine::NearbyLoop, this);
  threads_.emplace_back(&NetworkEngine::ServerLoop, this);
  threads_.emplace_back(&NetworkEngine::TickerLoop, this);
  Log("Started %s. id=%s, phones paired: %zu", WINDROP_VERSION, Cfg().deviceId.c_str(), Phones().size());
  return ok;
}

void NetworkEngine::Stop() {
  stop_ = true;
  {
    std::lock_guard<std::mutex> lk(lmx_);
    if (listen_ != INVALID_SOCKET) closesocket(listen_);
    listen_ = INVALID_SOCKET;
  }
  qcv_.notify_all();
  ncv_.notify_all();
  ocv_.notify_all();
  tcv_.notify_all();
  // Transfers in flight may sit in blocking calls; don't hold up exit for them.
  for (auto& t : threads_) t.detach();
}

void NetworkEngine::SetEnabled(bool on) {
  enabled = on;
  if (on) {
    OpenListener();
  } else {
    std::lock_guard<std::mutex> lk(lmx_);
    if (listen_ != INVALID_SOCKET) closesocket(listen_);
    listen_ = INVALID_SOCKET;
  }
}

void NetworkEngine::UsedPhone(const std::string& phoneId) {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  if (phoneId.empty() || Cfg().lastPhoneId == phoneId || !Cfg().Find(phoneId)) return;
  Cfg().lastPhoneId = phoneId;
  Cfg().Save();
}

void NetworkEngine::SendFiles(const std::vector<std::wstring>& paths, std::string phoneId) {
  if (phoneId.empty()) {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    phoneId = Cfg().DefaultPhone();
  }
  UsedPhone(phoneId);
  bool nearby = IsNearbyId(phoneId);
  std::lock_guard<std::mutex> lk(qmx_);
  for (auto& p : paths) {
    Job j;
    j.phoneId = phoneId;
    j.path = p;
    (nearby ? nearbyQueue_ : queue_).push_back(j);
  }
  (nearby ? ncv_ : qcv_).notify_all();
}

void NetworkEngine::SendText(const std::string& utf8, std::string phoneId) {
  if (utf8.empty()) return;
  if (phoneId.empty()) {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    phoneId = Cfg().DefaultPhone();
  }
  UsedPhone(phoneId);
  bool nearby = IsNearbyId(phoneId);
  std::lock_guard<std::mutex> lk(qmx_);
  Job j;
  j.phoneId = phoneId;
  j.isText = true;
  j.text = utf8;
  (nearby ? nearbyQueue_ : queue_).push_back(j);
  (nearby ? ncv_ : qcv_).notify_all();
}

void NetworkEngine::Decide(uint64_t id, bool accept, bool all) {
  std::lock_guard<std::mutex> lk(omx_);
  if (auto o = offers_.find(id); o != offers_.end()) {
    o->second = accept ? 1 : 0;
    if (all) offerAll_[id] = true;
    ocv_.notify_all();
    return;
  }
  if (id == pendingDecision_) {
    decidedId_ = id;
    decision_ = accept ? 1 : 0;
    ocv_.notify_all();
  }
}

// Asks the UI (via `post`) and waits for Decide(). False on "no", timeout, or when a newer request
// replaces this one (the phone gave up and asked again).
bool NetworkEngine::WaitDecision(uint64_t* id, int seconds, std::function<void(uint64_t)> post) {
  {
    std::lock_guard<std::mutex> lk(omx_);
    *id = pendingDecision_ = ++decisionSeq_;
    ocv_.notify_all();  // wakes a request this one replaces
  }
  post(*id);
  std::unique_lock<std::mutex> lk(omx_);
  uint64_t me = *id;
  ocv_.wait_for(lk, std::chrono::seconds(seconds), [&] { return decidedId_ == me || pendingDecision_ != me || stop_; });
  bool yes = decidedId_ == me && decision_ == 1;
  if (pendingDecision_ == me) pendingDecision_ = 0;
  return yes;
}

// Also drops them from a flush already in progress, so a cancelled item is never sent.
void NetworkEngine::CancelOutbox(const std::vector<uint64_t>& ids) {
  for (auto id : ids) TheOutbox().Remove(id);
  std::lock_guard<std::mutex> lk(qmx_);
  for (auto it = queue_.begin(); it != queue_.end();) {
    if (it->outboxId && std::find(ids.begin(), ids.end(), it->outboxId) != ids.end()) {
      auto f = flushLeft_.find(it->phoneId);
      if (f != flushLeft_.end() && (f->second <= 1 || !--f->second)) flushLeft_.erase(f);
      it = queue_.erase(it);
    } else {
      ++it;
    }
  }
}

void NetworkEngine::OnLinkUp() {
  // The link that just came up is almost always the phone the beacon called.
  std::string phone;
  {
    std::lock_guard<std::mutex> lk(tmx_);
    phone = waking_;
  }
  if (!phone.empty()) FlushOutbox(phone);
}

void NetworkEngine::FlushOutbox(const std::string& phoneId) {
  auto all = TheOutbox().Snapshot();
  std::vector<OutboxItem> items;
  for (auto& it : all)
    if (it.phoneId == phoneId) items.push_back(it);
  if (items.empty()) return;
  std::lock_guard<std::mutex> lk(qmx_);
  if (flushLeft_.count(phoneId)) return;  // already on its way
  flushLeft_[phoneId] = items.size();
  for (auto& it : items) {
    Job j;
    j.phoneId = it.phoneId;
    j.isText = it.isText;
    j.path = it.path;
    j.text = it.text;
    j.outboxId = it.id;
    queue_.push_back(j);
  }
  qcv_.notify_all();
}

// Paired phones. Several of these run at once, each taking the oldest item for a phone no other
// one is sending to, so phones are served side by side and each phone's items stay in order. Each
// phone's items wait in its own outbox: one phone out of reach never holds up another.
void NetworkEngine::SenderLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(qmx_);
      auto next = [&] {
        return std::find_if(queue_.begin(), queue_.end(), [&](const Job& j) {
          return std::find(sendingTo_.begin(), sendingTo_.end(), j.phoneId) == sendingTo_.end();
        });
      };
      qcv_.wait(lk, [&] { return stop_ || next() != queue_.end(); });
      if (stop_) return;
      auto it = next();
      job = std::move(*it);
      queue_.erase(it);
      sendingTo_.push_back(job.phoneId);
    }
    struct Free {  // that phone is free for the next item, however this one ends
      NetworkEngine* n;
      std::string phone;
      ~Free() {
        {
          std::lock_guard<std::mutex> lk(n->qmx_);
          auto& v = n->sendingTo_;
          v.erase(std::find(v.begin(), v.end(), phone));
        }
        n->qcv_.notify_all();
      }
    } freeAfter{this, job.phoneId};
    std::wstring name = job.isText ? L"Text" : FileNameOf(job.path);
    std::string why;
    Result r = SendOne(job, &why);
    Log("send %s -> %d", W2U(name).c_str(), (int)r);

    bool fromOutbox = job.outboxId != 0;
    if (r == Result::Offline && !fromOutbox) {
      OutboxItem it;
      it.phoneId = job.phoneId;
      it.isText = job.isText;
      it.path = job.path;
      it.text = job.text;
      auto* e = new UiEvent{NetEv::Offline};
      e->phone = job.phoneId;
      e->id = TheOutbox().Add(it);
      if (enabled && TheWifiDirect().IpOf(job.phoneId).empty()) {
        std::lock_guard<std::mutex> lk(tmx_);
        // Call it now, not on the next tick, unless the ticker would hold the call back anyway: Wi-Fi
        // off, pairing holding the beacon, or a nearby phone being called (see TickerLoop).
        if (TheRadios().WifiOn() && !TheWifiDirect().Pairing() && calling_.empty())
          TheWifiDirect().SetBeacon(WifiDirectHost::kBeaconPending, job.phoneId);
        if (waking_.empty()) { waking_ = job.phoneId; wakingSince_ = GetTickCount64(); }
      }
      e->count = 0;  // waiting for this phone only; other phones have their own queues
      for (auto& w : TheOutbox().Snapshot()) e->count += w.phoneId == job.phoneId;
      e->name = name;
      Post(e);
      std::lock_guard<std::mutex> lk(tmx_);
      nextProbe_ = GetTickCount64() + backoffSec_ * 1000ull;
    } else if (r == Result::Offline) {
      // Still unreachable: leave the rest of that phone's outbox for a few seconds (other phones' go on).
      std::lock_guard<std::mutex> lk(qmx_);
      queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                  [&](const Job& j) { return j.outboxId != 0 && j.phoneId == job.phoneId; }),
                   queue_.end());
      flushLeft_.erase(job.phoneId);
      flushAfter_[job.phoneId] = GetTickCount64() + 5000;
      continue;
    } else {
      if (fromOutbox && r != Result::Failed) TheOutbox().Remove(job.outboxId);
      if (fromOutbox && r == Result::Failed) {  // kept for later: not again at once
        std::lock_guard<std::mutex> lk(qmx_);
        flushAfter_[job.phoneId] = GetTickCount64() + 15000;
      }
      PostResult(job, r, name, why);
    }
    if (fromOutbox) {
      std::lock_guard<std::mutex> lk(qmx_);
      auto f = flushLeft_.find(job.phoneId);
      if (f != flushLeft_.end() && (f->second <= 1 || !--f->second)) flushLeft_.erase(f);
    }
  }
}

// Unpaired ("nearby") phones: called over Bluetooth for each drop, nothing kept for later. Runs
// apart from the paired phones' queue, so calling one (up to 45 s) or waiting for its user to
// accept (up to a minute) never holds up a paired phone's items, nor the other way round.
void NetworkEngine::NearbyLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(qmx_);
      ncv_.wait(lk, [&] { return stop_ || !nearbyQueue_.empty(); });
      if (stop_) return;
      job = std::move(nearbyQueue_.front());
      nearbyQueue_.pop_front();
    }
    std::wstring name = job.isText ? L"Text" : FileNameOf(job.path);
    std::string why;
    Result r = SendOne(job, &why);
    Log("send %s -> %d", W2U(name).c_str(), (int)r);
    if (r == Result::Offline) r = Result::Failed;  // not ours to keep trying: no outbox
    bool more = false;
    {
      std::lock_guard<std::mutex> lk(qmx_);
      for (auto& j : nearbyQueue_) more = more || j.phoneId == job.phoneId;
    }
    if (!more) {
      std::string ip;
      {
        std::lock_guard<std::mutex> lk(tmx_);
        if (nearbyOnLink_ == job.phoneId) ip = nearbyIp_;
        nearbyOnLink_.clear();
        nearbyIp_.clear();
      }
      if (!ip.empty() && TheWifiDirect().HasPeer(ip)) TheWifiDirect().Disconnect(ip);  // done with that phone
    }
    PostResult(job, r, name, why);
  }
}

void NetworkEngine::PostResult(const Job& job, Result r, const std::wstring& name, const std::string& why) {
  if (r == Result::Ok) {
    auto* e = new UiEvent{NetEv::SendDone};
    e->phone = job.phoneId;
    e->name = name;
    Post(e);
  } else if (r == Result::Declined) {
    auto* e = new UiEvent{NetEv::Declined};
    e->phone = job.phoneId;
    e->name = name;
    Post(e);
  } else {
    auto* e = new UiEvent{NetEv::Failed};
    e->name = r == Result::Missing ? name + L" (file not found)" : name;
    e->text = why;
    Post(e);
  }
}

// One transfer's part of the overlay's progress: transfers running at once add up.
struct Progress {
  NetworkEngine& n;
  std::shared_ptr<NetworkEngine::Live> t = std::make_shared<NetworkEngine::Live>();
  Progress(NetworkEngine& net, uint64_t size, bool sending, const std::wstring& name, const std::string& phoneId,
           const std::wstring& phone)
      : n(net) {
    t->sending = sending;
    t->name = name;
    t->phone = phone;
    t->phoneId = phoneId;
    t->total = size;
    n.xferTotal += size;
    n.Began(t);
  }
  void Add(uint64_t k) {
    t->done += k;
    n.xferDone += k;
  }
  ~Progress() {
    n.xferDone -= t->done;
    n.xferTotal -= t->total;
    n.Ended(t);
  }
};

void NetworkEngine::Began(const std::shared_ptr<Live>& t) {
  std::lock_guard<std::mutex> lk(pmx_);
  running_.push_back(t);
  (t->sending ? sendingNow : receivingNow)++;
  lastActive_[t->phoneId] = GetTickCount64();
}

void NetworkEngine::Ended(const std::shared_ptr<Live>& t) {
  std::lock_guard<std::mutex> lk(pmx_);
  running_.erase(std::remove(running_.begin(), running_.end(), t), running_.end());
  (t->sending ? sendingNow : receivingNow)--;
  lastActive_[t->phoneId] = GetTickCount64();
}

std::vector<NetworkEngine::Running> NetworkEngine::Transfers() {
  std::lock_guard<std::mutex> lk(pmx_);
  std::vector<Running> v;
  for (auto& t : running_) v.push_back({t->sending, t->name, t->phone, t->done.load(), t->total});
  return v;
}

bool NetworkEngine::PhoneIdle(const std::string& phoneId, ULONGLONG now) {
  {
    std::lock_guard<std::mutex> lk(pmx_);
    for (auto& t : running_)
      if (t->phoneId == phoneId) return false;
    auto a = lastActive_.find(phoneId);
    if (a != lastActive_.end() && now < a->second + 10000) return false;
  }
  std::lock_guard<std::mutex> lk(qmx_);
  if (std::find(sendingTo_.begin(), sendingTo_.end(), phoneId) != sendingTo_.end()) return false;
  return !std::any_of(queue_.begin(), queue_.end(), [&](const Job& j) { return j.phoneId == phoneId; });
}

// Phones are only ever reached over our Wi-Fi Direct group.
SOCKET NetworkEngine::ConnectTo(const std::string& ip) {
  if (ip.empty()) return INVALID_SOCKET;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kTcpPort);
  if (inet_pton(AF_INET, ip.c_str(), &a.sin_addr) != 1) return INVALID_SOCKET;
  SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  Tune(s);
  u_long nb = 1;
  ioctlsocket(s, FIONBIO, &nb);
  bool ok = connect(s, (sockaddr*)&a, sizeof a) == 0;
  if (!ok && WSAGetLastError() == WSAEWOULDBLOCK) {
    fd_set w, e;
    FD_ZERO(&w);
    FD_ZERO(&e);
    FD_SET(s, &w);
    FD_SET(s, &e);
    timeval tv{1, 0};
    if (select(0, nullptr, &w, &e, &tv) > 0 && FD_ISSET(s, &w)) {
      int err = 0, len = sizeof err;
      getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&err, &len);
      ok = err == 0;
    }
  }
  if (!ok) { closesocket(s); return INVALID_SOCKET; }
  nb = 0;
  ioctlsocket(s, FIONBIO, &nb);
  return s;
}

NetworkEngine::Result NetworkEngine::SendOne(const Job& job, std::string* why) {
  if (!enabled) return Result::Offline;
  bool nearby = IsNearbyId(job.phoneId);
  Bytes key = nearby ? Bytes{} : KeyFor(job.phoneId);
  if (!nearby && key.empty()) { Log("send: that phone isn't paired (any more)"); return Result::Failed; }
  uint64_t size = 0;
  HANDLE f = INVALID_HANDLE_VALUE;
  std::wstring name = L"text.txt";
  if (job.isText) {
    size = job.text.size();
  } else {
    f = CreateFileW(job.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return Result::Missing;
    LARGE_INTEGER li;
    GetFileSizeEx(f, &li);
    size = (uint64_t)li.QuadPart;
    name = FileNameOf(job.path);
  }
  struct HandleCloser {
    HANDLE h;
    ~HandleCloser() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
  } closer{f};

  // A nearby phone's link is unproven the whole time: keep LinkUp from dropping it.
  struct Stranger {
    bool on;
    Stranger(bool n) : on(n) { if (on) TheWifiDirect().pairExchanges++; }
    ~Stranger() { if (on) TheWifiDirect().pairExchanges--; }
  } stranger{nearby};
  std::string ip;
  SOCKET s = INVALID_SOCKET;
  SecureChannel ch;
  PeerVersion peer;
  std::string label = W2U(PhoneLabel(job.phoneId));
  if (nearby) {
    std::string myName;
    {
      std::lock_guard<std::recursive_mutex> lk(CfgMutex());
      myName = W2U(Cfg().deviceName);
    }
    Bytes want = NearbyTag(job.phoneId);
    ULONGLONG until = GetTickCount64() + 45000;
    std::vector<uint64_t> tried;
    for (int wrong = 0;;) {
      s = CallNearby(job.phoneId, &ip, until, tried);
      if (s == INVALID_SOCKET) {
        Log("send: %s didn't answer", label.c_str());
        *why = label + " didn't answer";
        return Result::Failed;
      }
      SetTimeouts(s, 10000);
      OpenPeer op;
      int st = OpenClient(s, Cfg().deviceId, myName, &op);
      if (st == 1 && NearbyTagFor(op.id) == want) {
        key = op.key;
        break;
      }
      closesocket(s);
      s = INVALID_SOCKET;
      // That link was another phone's (a paired one, say): keep calling ours. A refusal while ours
      // still says it takes files was most likely that other phone too.
      auto around = NearbyPhones();
      bool heard = std::any_of(around.begin(), around.end(), [&](const NearbyPhone& n) { return n.id == job.phoneId; });
      if ((st == 1 || (st == 0 && heard)) && ++wrong <= 3 && GetTickCount64() < until) {
        Log("send: the phone at %s isn't %s; still calling it", ip.c_str(), label.c_str());
        continue;
      }
      Log("send: %s %s", label.c_str(), st == 0 ? "isn't taking files from nearby PCs any more" : "broke off");
      *why = label + (st == 0 ? " stopped taking files from nearby PCs" : " broke off");
      return Result::Failed;
    }
    ch.s = s;
    auto hs = ch.ClientHandshake(key, Cfg().deviceId, &peer);
    if (hs != SecureChannel::kOk) {
      closesocket(s);
      if (hs == SecureChannel::kIncompatible) {
        Log("send: phone app %s (protocol %d) doesn't match ours (%d)", peer.version.c_str(), peer.proto, kProto);
        PostIncompatible(peer);
      } else {
        Log("send: %s broke off", label.c_str());
        *why = label + " broke off";
      }
      return Result::Failed;
    }
    std::lock_guard<std::mutex> lk(tmx_);
    nearbyOnLink_ = job.phoneId;
    nearbyIp_ = ip;
  } else {
    // Its own link once it has proven itself on one; until then the links that haven't said who
    // they are yet (it may have only just come up), newest first. Another phone there turns our
    // key away, and the next one is tried.
    std::string known = TheWifiDirect().IpOf(job.phoneId);
    std::vector<std::string> ips = known.empty() ? TheWifiDirect().UnprovenIps() : std::vector<std::string>{known};
    bool others = false;
    for (auto& cand : ips) {
      SOCKET c = ConnectTo(cand);
      // A fresh Wi-Fi Direct link takes the phone a second or so to finish its side (DHCP, server).
      for (int i = 0; c == INVALID_SOCKET && i < 8 && TheWifiDirect().FreshLink(cand); ++i) {
        Sleep(400);
        c = ConnectTo(cand);
      }
      if (c == INVALID_SOCKET) continue;
      SetTimeouts(c, 10000);
      ch.s = c;
      auto hs = ch.ClientHandshake(key, Cfg().deviceId, &peer);
      if (hs == SecureChannel::kOk) {
        s = c;
        ip = cand;
        break;
      }
      closesocket(c);
      ch.s = INVALID_SOCKET;
      if (hs == SecureChannel::kIncompatible) {
        Log("send: phone app %s (protocol %d) doesn't match ours (%d)", peer.version.c_str(), peer.proto, kProto);
        PostIncompatible(peer);
        return Result::Failed;
      }
      others = others || hs == SecureChannel::kRefused;
    }
    if (s == INVALID_SOCKET) {
      Log(others ? "send: %s isn't on the link yet (another phone is)" : "send: %s not reachable yet", label.c_str());
      return Result::Offline;
    }
    TheWifiDirect().MarkAuthenticated(job.phoneId, ip);
  }
  struct SockCloser {
    SOCKET s;
    ~SockCloser() { closesocket(s); }
  } sc{s};
  BusyLink busy;
  struct Done {
    std::atomic<ULONGLONG>& t;
    ~Done() { t = GetTickCount64(); }
  } done{lastActivity_};
  std::wstring sender;
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    sender = Cfg().deviceName;
  }
  std::string hdr = BuildOffer(job.isText ? kTypeText : kTypeFile, size, W2U(name), W2U(sender), Cfg().deviceId);
  if (!ch.Send(hdr.data(), hdr.size())) return Result::Offline;

  uint8_t d = 0;
  if (nearby) {
    SetTimeouts(s, 75000);  // its user has a minute to accept
    auto* w = new UiEvent{NetEv::Waiting};
    w->phone = job.phoneId;
    w->name = name;
    Post(w);
  }
  if (!ch.Recv(&d, 1)) { Log("no decision: %d", WSAGetLastError()); return Result::Failed; }
  if (d == kDeclined) return Result::Declined;
  if (d != kAccepted) return Result::Failed;

  auto* e = new UiEvent{NetEv::SendStart};
  e->name = job.isText ? L"Text" : name;
  e->size = size;
  e->count = job.outboxId ? 1 : 0;
  Post(e);
  Progress prog(*this, size, true, job.isText ? L"Text" : name, job.phoneId, PhoneLabel(job.phoneId));

  SetTimeouts(s, 30000);
  bool ok = true;
  if (job.isText) {
    ok = ch.Send(job.text.data(), job.text.size());
    prog.Add(size);
  } else {
    std::vector<char> buf(kChunk);
    uint64_t left = size;
    while (left && ok) {
      DWORD got = 0;
      if (!ReadFile(f, buf.data(), (DWORD)std::min<uint64_t>(left, kChunk), &got, nullptr) || got == 0) { ok = false; break; }
      ok = ch.Send(buf.data(), got);
      left -= got;
      prog.Add(got);
    }
  }
  uint8_t ack = kAckBad;
  if (ok) ok = ch.Recv(&ack, 1);
  return ok && ack == kAckOk ? Result::Ok : Result::Failed;
}

void NetworkEngine::ServerLoop() {
  while (!stop_) {
    SOCKET l;
    {
      std::lock_guard<std::mutex> lk(lmx_);
      l = listen_;
    }
    if (l == INVALID_SOCKET) { Sleep(200); continue; }  // switched off
    sockaddr_in a{};
    int len = sizeof a;
    SOCKET c = accept(l, (sockaddr*)&a, &len);
    if (c == INVALID_SOCKET) {
      if (stop_) return;
      Sleep(50);
      continue;
    }
    std::string ip = IpStr(a.sin_addr);
    // Each connection on its own thread: a pairing prompt waits up to a minute for the user and a
    // big transfer takes a while; neither may hold up the next connection. Capped, since the port
    // is open on the local network too.
    if (handlers_ >= kMaxHandlers) {
      Log("refused a connection from %s: %d already open", ip.c_str(), kMaxHandlers);
      closesocket(c);
      continue;
    }
    handlers_++;
    Tune(c);
    Log("incoming connection from %s", ip.c_str());
    TheWifiDirect().SawConnection(ip);
    std::thread([this, c, ip] {
      HandleIncoming(c, ip);
      // Close politely: closing with the client's bytes still unread sends a reset, which can
      // swallow our last reply (say, "incompatible version") before the other side reads it.
      shutdown(c, SD_SEND);
      SetTimeouts(c, 300);
      char junk[4096];
      for (int i = 0; i < 64 && recv(c, junk, sizeof junk, 0) > 0; ++i) {}
      closesocket(c);
      handlers_--;
    }).detach();
  }
}

void NetworkEngine::HandleIncoming(SOCKET c, const std::string& ip) {
  SetTimeouts(c, 15000);
  uint8_t type = 0;
  PeerVersion peer;
  bool compatible = false;
  if (!ReadEnvelope(c, &type, &peer, &compatible) || !enabled) {
    Log("refused a connection from %s: not a WinDrop it can talk to (a build from before 0.1.0?)", ip.c_str());
    return;
  }
  if (!compatible) {
    Log("phone app %s (protocol %d) doesn't match ours (%d)", peer.version.c_str(), peer.proto, kProto);
    SendIncompatible(c);
    PostIncompatible(peer);
    return;
  }
  if (type == kHelloPair) { HandlePair(c, ip); return; }
  if (type == kHelloOpen) { HandleOpen(c, ip); return; }
  if (type != kHelloSession) return;
  SecureChannel ch;
  ch.s = c;
  std::string peerId;
  bool ok = ch.ServerHandshake([](const std::string& id) { return KeyFor(id); }, &peerId);
  if (!ok) {
    Log("refused a connection from %s: not a paired phone", ip.c_str());
    // A phone this PC no longer has (removed, or paired with another PC) holding the link would
    // keep the paired phones off it: drop it now and ignore its requests for a while.
    if (TheWifiDirect().IsUnproven(ip)) TheWifiDirect().RejectPeer(ip);
    return;
  }
  TheWifiDirect().MarkAuthenticated(peerId, ip);
  BusyLink busy;
  HandleSession(ch, ip, peerId);
  lastActivity_ = GetTickCount64();
}

// Tells the UI which side needs updating; shown at most once a minute.
void NetworkEngine::PostIncompatible(const PeerVersion& peer) {
  static ULONGLONG last = 0;
  ULONGLONG now = GetTickCount64();
  if (last && now - last < 60000) return;
  last = now;
  auto* e = new UiEvent{NetEv::Incompatible};
  e->count = (size_t)peer.proto;
  e->name = U2W(peer.version);
  Post(e);
}

void NetworkEngine::HandleSession(SecureChannel& ch, const std::string& ip, const std::string& phoneId,
                                  const std::wstring* stranger) {
  uint8_t fx[20];
  if (!ch.Recv(fx, sizeof fx) || memcmp(fx, "TDRP", 4) != 0 || fx[4] != 1) return;
  uint8_t type = fx[5];
  uint64_t size = GetU64(fx + 6);
  uint16_t nl = GetU16(fx + 14), sl = GetU16(fx + 16), il = GetU16(fx + 18);
  if (nl > 4096 || sl > 1024 || il > 256) return;
  std::string name(nl, '\0'), sender(sl, '\0'), id(il, '\0');
  if (!ch.Recv(name.data(), nl) || !ch.Recv(sender.data(), sl) || !ch.Recv(id.data(), il)) return;
  if (type == kTypeLog) { if (!stranger) ReceivePhoneLog(ch, size); return; }
  if (type == kTypeBye) {
    uint8_t ok = kAckOk;
    ch.Send(&ok, 1);
    ch.Flush();
    Log("phone is closing the link (%s)", ip.c_str());
    TheWifiDirect().Disconnect(ip);
    return;
  }
  if (type != kTypeFile && type != kTypeText) { uint8_t b = kBusy; ch.Send(&b, 1); ch.Flush(); return; }
  if (type == kTypeText && size > kMaxText) { uint8_t b = kBusy; ch.Send(&b, 1); ch.Flush(); return; }
  Log("offer from %s (%s): %s, %llu bytes", ip.c_str(), sender.c_str(), name.c_str(), (unsigned long long)size);

  std::wstring wname = SanitizeName(U2W(name));
  if (stranger) {
    // Unpaired: nothing proves who this is, so the user decides.
    bool yes = AskOffer(type == kTypeText ? L"text" : wname, size, *stranger, phoneId, ch.s);
    uint8_t d = yes ? kAccepted : kDeclined;
    if (!ch.Send(&d, 1) || !ch.Flush() || !yes) {
      Log("offer from %s %s", W2U(*stranger).c_str(), yes ? "broke off" : "declined");
      return;
    }
  }
  if (!stranger) {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    for (auto& p : Cfg().phones)
      if (p.id == phoneId && !sender.empty() && p.name != U2W(sender)) {
        p.name = U2W(sender);  // renamed on the phone
        Cfg().Save();
      }
  }
  if (!stranger) {
    UsedPhone(phoneId);
    uint8_t d = kAccepted;  // authenticated as the paired phone: no prompt
    if (!ch.Send(&d, 1)) return;
  }

  SetTimeouts(ch.s, 30000);
  if (type == kTypeText) {
    std::string text(size, '\0');
    bool ok = ch.Recv(text.data(), size);
    uint8_t ack = ok ? kAckOk : kAckBad;
    ch.Send(&ack, 1);
    ch.Flush();
    if (ok) {
      auto* e = new UiEvent{NetEv::TextReceived};
      e->text = text;
      e->name = stranger ? *stranger : PhoneLabel(phoneId);  // which phone it came from
      Post(e);
    }
    return;
  }

  std::wstring dir = Cfg().downloadDir;
  CreateDirectoryW(dir.c_str(), nullptr);
  std::wstring path = UniquePath(dir, wname);
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) {
    uint8_t ack = kAckBad;
    ch.Send(&ack, 1);
    ch.Flush();
    return;
  }
  auto* e = new UiEvent{NetEv::RecvStart};
  e->name = wname;
  e->size = size;
  Post(e);

  std::vector<char> buf(kChunk);
  uint64_t left = size;
  bool ok = true;
  {
    Progress prog(*this, size, false, wname, phoneId, stranger ? *stranger : PhoneLabel(phoneId));
    while (left) {
      size_t n = (size_t)std::min<uint64_t>(left, kChunk);
      if (!ch.Recv(buf.data(), n)) { ok = false; break; }
      DWORD wrote = 0;
      if (!WriteFile(f, buf.data(), (DWORD)n, &wrote, nullptr) || wrote != (DWORD)n) { ok = false; break; }
      left -= n;
      prog.Add(n);
    }
  }
  CloseHandle(f);
  uint8_t ack = ok ? kAckOk : kAckBad;
  ch.Send(&ack, 1);
  ch.Flush();
  if (!ok) {
    DeleteFileW(path.c_str());
    auto* fe = new UiEvent{NetEv::Failed};
    fe->name = wname;
    Post(fe);
    return;
  }
  auto* de = new UiEvent{NetEv::RecvDone};
  de->name = FileNameOf(path);
  de->path = path;
  Post(de);
}

// Pairing: only while "Pair phone…" is open. Both sides show a 6-digit check number derived from
// the key exchange; the user confirms here that they match (that is what stops a stranger, or
// someone in the middle, from pairing).
void NetworkEngine::HandlePair(SOCKET c, const std::string& ip) {
  // phone: pub[65] nP[16] idLen id nameLen name
  Bytes pubP(65), nP(16);
  uint8_t idLen = 0, nameLen = 0;
  if (!RecvRaw(c, pubP.data(), 65) || !RecvRaw(c, nP.data(), 16) || !RecvRaw(c, &idLen, 1)) return;
  std::string id(idLen, '\0');
  if (!RecvRaw(c, id.data(), idLen) || !RecvRaw(c, &nameLen, 1)) return;
  std::string name(nameLen, '\0');
  if (!RecvRaw(c, name.data(), nameLen)) return;

  Bytes pairLabel = Str("WDR2 pair");
  bool full = false;
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    full = Cfg().phones.size() >= kMaxPhones && !Cfg().Find(id);
  }
  if (!TheWifiDirect().Pairing() || id.empty() || full) {
    Log("pair: refused (%s)", full ? "already 4 phones paired" : "pairing window closed");
    uint8_t no = kStatusRefused;
    SendRaw(c, &no, 1);
    if (full) {
      auto* e = new UiEvent{NetEv::PairDone};
      e->name = U2W(name);
      e->count = 0;
      Post(e);
    }
    return;
  }

  struct Exchange {
    Exchange() { TheWifiDirect().pairExchanges++; }
    ~Exchange() { TheWifiDirect().pairExchanges--; }
  } exchange;
  EcdhKey ec;
  Bytes pubS = ec.PublicKey(), nS = RandomBytes(16);
  std::string myId, myName;
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    myId = Cfg().deviceId;
    myName = W2U(Cfg().deviceName).substr(0, 255);
  }
  Bytes m2 = Concat({&pubS, &nS});
  m2.push_back((uint8_t)myId.size());
  m2.insert(m2.end(), myId.begin(), myId.end());
  m2.push_back((uint8_t)myName.size());
  m2.insert(m2.end(), myName.begin(), myName.end());
  Bytes reply{kStatusGo};
  reply.insert(reply.end(), m2.begin(), m2.end());
  if (!SendRaw(c, reply.data(), reply.size())) return;

  Bytes m1 = Concat({&pubP, &nP});
  m1.push_back(idLen);
  m1.insert(m1.end(), id.begin(), id.end());
  m1.push_back(nameLen);
  m1.insert(m1.end(), name.begin(), name.end());
  Bytes t = Sha256(Concat({&pairLabel, &m1, &m2}));
  Bytes z = ec.Agree(pubP);
  if (z.empty()) return;
  Bytes key = Hkdf(z, t, "WDR2 key", 32);
  Bytes sasB = Hkdf(z, t, "WDR2 sas", 4);
  uint32_t sasN = (uint32_t(sasB[0]) << 24 | uint32_t(sasB[1]) << 16 | uint32_t(sasB[2]) << 8 | sasB[3]) % 1000000;
  char sas[8];
  snprintf(sas, sizeof sas, "%06u", sasN);
  Log("pair: request from %s, check number %.3s %.3s", name.c_str(), sas, sas + 3);

  SetTimeouts(c, 75000);
  uint64_t did = 0;
  std::wstring wname = U2W(name);
  bool yes = WaitDecision(&did, 60, [&](uint64_t i) {
    auto* e = new UiEvent{NetEv::PairRequest};
    e->id = i;
    e->name = wname;
    e->text = sas;
    Post(e);
  });
  // Timed out or replaced by a newer request: take the prompt off the screen.
  auto* closed = new UiEvent{NetEv::PairClosed};
  closed->id = did;
  Post(closed);
  Bytes okLabel = Str("WDR2 pc-ok"), phoneLabel = Str("WDR2 phone-ok");
  Bytes msg{uint8_t(yes ? 1 : 2)};
  if (yes) {
    Bytes mac = HmacSha256(key, Concat({&okLabel, &t}));
    msg.insert(msg.end(), mac.begin(), mac.end());
  }
  if (!SendRaw(c, msg.data(), msg.size()) || !yes) {
    Log("pair: %s", yes ? "phone went away" : "declined");
    return;
  }
  Bytes phoneMac(32);
  if (!RecvRaw(c, phoneMac.data(), 32) || !SameBytes(phoneMac, HmacSha256(key, Concat({&phoneLabel, &t})))) {
    Log("pair: phone failed the final check");
    return;
  }
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    Cfg().AddPhone({id, wname, key});
    Cfg().lastPhoneId = id;
    Cfg().Save();
  }
  uint8_t done = 0;
  SendRaw(c, &done, 1);
  Log("pair: paired with %s", name.c_str());
  TheWifiDirect().MarkAuthenticated(id, ip);
  // Done: phones with something waiting can be called again (pairing has the beacon meanwhile).
  TheWifiDirect().ClosePairWindow();
  auto* e = new UiEvent{NetEv::PairDone};
  e->name = wname;
  e->count = 1;
  Post(e);
}

// An unpaired phone wants to send: only while "visible to nearby phones" is on.
void NetworkEngine::HandleOpen(SOCKET c, const std::string& ip) {
  if (!TheWifiDirect().VisibleOrJustEnded()) {
    Log("nearby: refused %s: not visible to nearby phones", ip.c_str());
    uint8_t no = kStatusRefused;
    SendRaw(c, &no, 1);
    return;
  }
  struct Exchange {
    Exchange() { TheWifiDirect().pairExchanges++; }
    ~Exchange() { TheWifiDirect().pairExchanges--; }
  } exchange;
  std::string myName;
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    myName = W2U(Cfg().deviceName);
  }
  OpenPeer op;
  if (!OpenServer(c, Cfg().deviceId, myName, &op)) return;
  uint8_t type = 0;
  PeerVersion peer;
  bool compatible = false;
  if (!ReadEnvelope(c, &type, &peer, &compatible) || !compatible || type != kHelloSession) return;
  SecureChannel ch;
  ch.s = c;
  std::string peerId;
  if (!ch.ServerHandshake([&](const std::string& id) { return id == op.id ? op.key : Bytes{}; }, &peerId)) return;
  std::wstring from = U2W(op.name.empty() ? std::string("A phone") : op.name);
  Log("nearby: %s connected", op.name.c_str());
  BusyLink busy;
  SetTimeouts(c, 75000);  // the user has a minute to accept
  HandleSession(ch, ip, peerId, &from);
  lastActivity_ = GetTickCount64();
}

// The other side closed the connection (or it broke): readable, with nothing to read.
static bool HungUp(SOCKET s) {
  fd_set r;
  FD_ZERO(&r);
  FD_SET(s, &r);
  timeval tv{0, 0};
  if (select(0, &r, nullptr, nullptr, &tv) <= 0) return false;
  char b;
  return recv(s, &b, 1, MSG_PEEK) <= 0;
}

// The pill asks; false on decline or after a minute. After "accept all" / "decline all", that
// sender's next offers get the same answer without asking, until it goes quiet for 2 minutes.
bool NetworkEngine::AskOffer(const std::wstring& name, uint64_t size, const std::wstring& from, const std::string& fromId,
                             SOCKET s) {
  constexpr ULONGLONG kAutoMs = 120000;
  uint64_t id;
  {
    std::lock_guard<std::mutex> lk(omx_);
    ULONGLONG now = GetTickCount64();
    if (auto a = autoAnswer_.find(fromId); a != autoAnswer_.end()) {
      if (now < a->second.until) {
        a->second.until = now + kAutoMs;
        Log("nearby: %s %s (all from %s)", W2U(name).c_str(), a->second.yes ? "accepted" : "declined", W2U(from).c_str());
        return a->second.yes;
      }
      autoAnswer_.erase(a);
    }
    id = ++decisionSeq_;
    offers_[id] = -1;
  }
  auto* e = new UiEvent{NetEv::Offer};
  e->id = id;
  e->name = name;
  e->size = size;
  e->path = from;
  Post(e);
  bool yes, gone = false;
  {
    std::unique_lock<std::mutex> lk(omx_);
    ULONGLONG until = GetTickCount64() + kOfferWaitSec * 1000ull;
    while (!stop_ && GetTickCount64() < until) {
      if (ocv_.wait_for(lk, std::chrono::milliseconds(500), [&] { return offers_[id] >= 0 || stop_; })) break;
      lk.unlock();
      gone = HungUp(s);  // the phone gave up (or went out of range): don't keep asking
      lk.lock();
      if (gone) break;
    }
    yes = !gone && offers_[id] == 1;
    offers_.erase(id);
    if (offerAll_.erase(id) && !gone) autoAnswer_[fromId] = {yes, GetTickCount64() + kAutoMs};
  }
  if (gone) Log("nearby: %s took back %s", W2U(from).c_str(), W2U(name).c_str());
  auto* closed = new UiEvent{NetEv::OfferClosed};
  closed->id = id;
  Post(closed);
  return yes;
}

// Calls the nearby phone over Bluetooth until a link that may be it comes up (or the time is up),
// then connects to it. Links that haven't proven to be a paired phone are tried newest first, each
// once (`tried`); the caller checks who answered. One already on our group counts too.
SOCKET NetworkEngine::CallNearby(const std::string& phoneId, std::string* ip, ULONGLONG& until,
                                 std::vector<uint64_t>& tried) {
  auto& wfd = TheWifiDirect();
  std::string known;
  {
    std::lock_guard<std::mutex> lk(tmx_);
    if (nearbyOnLink_ == phoneId) known = nearbyIp_;  // an earlier item of the same drop
    calling_ = phoneId;
  }
  tcv_.notify_all();
  SOCKET s = INVALID_SOCKET;
  if (!known.empty() && wfd.HasPeer(known)) {
    s = ConnectTo(known);
    if (s != INVALID_SOCKET) *ip = known;
  }
  if (s == INVALID_SOCKET) Log("nearby: calling %s", W2U(PhoneLabel(phoneId)).c_str());
  while (s == INVALID_SOCKET && GetTickCount64() < until && enabled && !stop_) {
    std::string cand;
    for (auto& [seq, lip] : wfd.UnprovenLinks())
      if (std::find(tried.begin(), tried.end(), seq) == tried.end()) {
        tried.push_back(seq);
        cand = lip;
        break;
      }
    if (cand.empty()) {
      Sleep(300);
      continue;
    }
    s = ConnectTo(cand);
    for (int i = 0; s == INVALID_SOCKET && i < 10 && wfd.FreshLink(cand); ++i) {
      Sleep(400);
      s = ConnectTo(cand);
    }
    if (s != INVALID_SOCKET) *ip = cand;
  }
  std::lock_guard<std::mutex> lk(tmx_);
  calling_.clear();
  return s;
}

void NetworkEngine::TickerLoop() {
  std::unique_lock<std::mutex> lk(tmx_);
  while (!stop_) {
    tcv_.wait_for(lk, std::chrono::seconds(1));
    if (stop_) return;
    ULONGLONG now = GetTickCount64();
    bool probe = false;
    auto& wfd = TheWifiDirect();
    // Several phones can be on our group at once. Phones with something waiting that aren't on it
    // are called over Bluetooth one at a time, oldest item first (a drag heading for a phone's spot
    // puts that phone first); if one doesn't come within 30 s, the next is tried.
    std::vector<std::string> linked = wfd.LinkedPhones();
    auto isLinked = [&](const std::string& p) { return std::find(linked.begin(), linked.end(), p) != linked.end(); };
    std::vector<std::string> waiting, linkedWaiting;
    std::string prewarm = wfd.PrewarmPhone();
    if (!prewarm.empty() && KeyFor(prewarm).size() && !isLinked(prewarm)) waiting.push_back(prewarm);
    for (auto& it : TheOutbox().Snapshot()) {
      if (KeyFor(it.phoneId).empty()) continue;
      auto& list = isLinked(it.phoneId) ? linkedWaiting : waiting;
      if (std::find(list.begin(), list.end(), it.phoneId) == list.end()) list.push_back(it.phoneId);
    }
    if (waiting.empty()) {
      waking_.clear();
    } else if (waking_.empty() || std::find(waiting.begin(), waiting.end(), waking_) == waiting.end() ||
               (!prewarm.empty() && waking_ != prewarm && !isLinked(prewarm))) {
      waking_ = waiting.front();
      wakingSince_ = now;
    } else if (now - wakingSince_ > 30000 && waiting.size() > 1) {
      auto i = std::find(waiting.begin(), waiting.end(), waking_);
      waking_ = (i + 1 == waiting.end()) ? waiting.front() : *(i + 1);
      wakingSince_ = now;
    }
    // While "Pair phone…" is open the pairing beacon has the air to itself, and a phone with
    // something waiting is called once pairing ends (pairing a phone ends it at once; otherwise
    // after 3 minutes, or "Stop pairing"). Taking turns every few seconds looked fairer, but in
    // testing the called phone never caught its turn. With Wi-Fi off here, calling a phone would
    // only have it search for a link that can't come up.
    // A nearby phone being called (a send the user just picked) has the air to itself too: two
    // phones joining at once made both fail in testing. Its call ends within 45 s.
    bool call = !waking_.empty() && TheRadios().WifiOn() && calling_.empty();
    if (!enabled) wfd.SetBeacon(WifiDirectHost::kBeaconOff);
    else if (wfd.Pairing()) wfd.SetBeacon(WifiDirectHost::kBeaconPresence);
    else if (call) wfd.SetBeacon(WifiDirectHost::kBeaconPending, waking_);
    else wfd.SetBeacon(WifiDirectHost::kBeaconOff);

    // Second beacon: call the nearby phone a send (or a drag hovering its spot) is for.
    {
      std::string call = !calling_.empty() ? calling_ : IsNearbyId(prewarm) ? prewarm : "";
      wfd.SetNearbyBeacon(enabled && !call.empty() ? NearbyTag(call) : Bytes{});
      if (!enabled && wfd.Visible()) wfd.SetVisible(false);
    }

    // A phone we've called for 20 s hasn't come, while others sit idle on the group: some Wi-Fi
    // cards take only so many phones at once. Ask the one that's been idle to let go (it comes back
    // by itself when it next needs us). At most every 30 s.
    if (enabled && !waking_.empty() && now - wakingSince_ > 20000 && now > nextHandOff_ && !linked.empty() &&
        !handingOff_) {
      for (auto& p : linked) {
        if (std::find(linkedWaiting.begin(), linkedWaiting.end(), p) != linkedWaiting.end() || p == prewarm) continue;
        lk.unlock();
        bool idle = PhoneIdle(p, now);
        lk.lock();
        if (!idle) continue;
        nextHandOff_ = now + 30000;
        handingOff_ = true;
        Log("link: %s hasn't come for 20 s; asking idle %s to let go", W2U(PhoneLabel(waking_)).c_str(), W2U(PhoneLabel(p)).c_str());
        std::thread([this, p] {
          HandOff(p);
          handingOff_ = false;
        }).detach();
        break;
      }
    }

    if (TheOutbox().Count() == 0) {
      backoffSec_ = 15;
      nextProbe_ = now + 15000;
    } else if (now >= nextProbe_) {
      probe = true;
      nextProbe_ = now + backoffSec_ * 1000ull;
      backoffSec_ = std::min(backoffSec_ * 2, 600);
    }
    lk.unlock();
    // Phones on our group with things waiting: send them now (one that just turned us away waits
    // a few seconds first).
    if (enabled) {
      for (auto& p : linkedWaiting) {
        bool later;
        {
          std::lock_guard<std::mutex> qlk(qmx_);
          auto a = flushAfter_.find(p);
          later = a != flushAfter_.end() && now < a->second && !probe;
        }
        if (!later) FlushOutbox(p);
      }
    }
    lk.lock();
  }
}

// Asks the phone to let go of its link. Just closing our side isn't enough: a phone that still
// wanted the link would rejoin by itself. One that's about to send something says no.
void NetworkEngine::HandOff(const std::string& phoneId) {
  std::string ip = TheWifiDirect().IpOf(phoneId);
  SOCKET s = ConnectTo(ip);
  if (s == INVALID_SOCKET) {
    Log("link: couldn't reach %s to ask", W2U(PhoneLabel(phoneId)).c_str());
    return;
  }
  int answer = -1;
  SetTimeouts(s, 5000);
  {
    SecureChannel ch;
    ch.s = s;
    PeerVersion peer;
    std::wstring me;
    {
      std::lock_guard<std::recursive_mutex> lk(CfgMutex());
      me = Cfg().deviceName;
    }
    if (ch.ClientHandshake(KeyFor(phoneId), Cfg().deviceId, &peer) == SecureChannel::kOk) {
      std::string hdr = BuildOffer(kTypeRelease, 0, "", W2U(me), Cfg().deviceId);
      uint8_t d = 0;
      if (ch.Send(hdr.data(), hdr.size()) && ch.Recv(&d, 1)) answer = d;
    }
  }
  closesocket(s);
  if (answer == kDeclined) {
    Log("link: %s is about to send something, keeping it", W2U(PhoneLabel(phoneId)).c_str());
    return;
  }
  Log(answer == kAccepted ? "link: %s let its link go" : "link: closing %s's link", W2U(PhoneLabel(phoneId)).c_str());
  TheWifiDirect().Disconnect(ip);
}

void NetworkEngine::ReceivePhoneLog(SecureChannel& ch, uint64_t size) {
  uint8_t d = size <= kMaxText ? kAccepted : kDeclined;
  if (!ch.Send(&d, 1) || d != kAccepted) { ch.Flush(); return; }
  std::string text(size, '\0');
  bool ok = ch.Recv(text.data(), size);
  uint8_t ack = ok ? kAckOk : kAckBad;
  ch.Send(&ack, 1);
  ch.Flush();
  if (!ok) return;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string::npos) end = text.size();
    if (end > start) Log("[phone] %s", text.substr(start, end - start).c_str());
    start = end + 1;
  }
}

}  // namespace td
