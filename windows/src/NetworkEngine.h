#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>

#include "common.h"

namespace td {

class SecureChannel;
struct PeerVersion;

enum class NetEv {
  PeerSeen,      // name = phone name
  Offer,         // an unpaired phone offers something: id, name, size, path = its name; answer with Decide(id, ...)
  OfferClosed,   // id
  SendStart,     // name, size, count = 1 if it came from the outbox
  SendDone,      // name
  RecvStart,     // name, size
  RecvDone,      // name, path
  TextReceived,  // text
  Offline,       // id = outbox item id, count = outbox size
  Declined,      // name
  Failed,        // name; text = why, if known (title then says it)
  Waiting,       // a nearby phone has the offer: phone, name
  PairRequest,   // id, name = phone name, text = 6-digit check number; answer with Decide(id, ...)
  PairClosed,    // id: that pairing request is over (answered, timed out or replaced); drop its prompt
  PairDone,      // name = phone name, count = 1 on success
  Incompatible,  // the phone's WinDrop speaks another protocol: name = its app version, count = its protocol
};

struct UiEvent {
  NetEv type;
  uint64_t id = 0;
  std::string phone;  // phone id, for sends
  std::wstring name;
  std::wstring path;
  std::string text;
  uint64_t size = 0;
  size_t count = 0;
};

class NetworkEngine {
 public:
  bool Start(HWND notify);
  void Stop();
  // Off: the listening socket is closed (nothing on the network answers) and nothing is sent.
  void SetEnabled(bool on);

  // phoneId empty: the default phone (last used).
  void SendFiles(const std::vector<std::wstring>& paths, std::string phoneId = "");
  void SendText(const std::string& utf8, std::string phoneId = "");
  // `all`: the same answer for whatever else that sender offers in the next couple of minutes.
  void Decide(uint64_t id, bool accept, bool all = false);
  void CancelOutbox(const std::vector<uint64_t>& ids);
  void FlushOutbox(const std::string& phoneId);  // what's waiting for the phone now on the link
  void OnLinkUp();

  std::atomic<bool> enabled{true};
  // An unpaired phone's offer waits this long for the user, then counts as declined.
  static constexpr int kOfferWaitSec = 60;

  // Live progress, polled by the overlay: every transfer running now added together.
  std::atomic<uint64_t> xferDone{0}, xferTotal{0};
  std::atomic<int> sendingNow{0}, receivingNow{0};
  // One transfer under way. `done` grows as it goes (see Progress in the .cpp).
  struct Live {
    bool sending = false;
    std::wstring name, phone;  // the file, and the phone it goes to or comes from
    std::string phoneId;
    uint64_t total = 0;
    std::atomic<uint64_t> done{0};
  };
  struct Running {
    bool sending;
    std::wstring name, phone;
    uint64_t done, total;
  };
  std::vector<Running> Transfers();  // what's being sent and received right now, oldest first
  void Began(const std::shared_ptr<Live>& t);
  void Ended(const std::shared_ptr<Live>& t);

 private:
  struct Job {
    std::string phoneId;
    bool isText = false;
    std::wstring path;
    std::string text;
    uint64_t outboxId = 0;
  };
  enum class Result { Ok, Offline, Declined, Failed, Missing };

  void Post(UiEvent* e);
  void SenderLoop();  // paired phones: one of these per phone that can be sent to at once
  void NearbyLoop();  // unpaired phones: on their own, so neither holds up the other
  void PostResult(const Job& job, Result r, const std::wstring& name, const std::string& why);
  void ServerLoop();
  void TickerLoop();
  bool OpenListener();
  Result SendOne(const Job& job, std::string* why);
  void HandleIncoming(SOCKET c, const std::string& ip);
  // `stranger`: the unpaired sender's name; its offers need the user's OK.
  void HandleSession(SecureChannel& ch, const std::string& ip, const std::string& phoneId,
                     const std::wstring* stranger = nullptr);
  void HandleOpen(SOCKET c, const std::string& ip);  // an unpaired phone wants to send
  // `s`: the sender's connection; if it hangs up meanwhile, the question goes away.
  bool AskOffer(const std::wstring& name, uint64_t size, const std::wstring& from, const std::string& fromId, SOCKET s);
  // Brings that phone onto our group and connects to it. `tried`: links already tried (by LinkSeq).
  SOCKET CallNearby(const std::string& phoneId, std::string* ip, ULONGLONG& until, std::vector<uint64_t>& tried);
  void UsedPhone(const std::string& phoneId);  // remember it as the default target
  void HandlePair(SOCKET c, const std::string& ip);
  void PostIncompatible(const PeerVersion& peer);
  void ReceivePhoneLog(SecureChannel& ch, uint64_t size);
  bool WaitDecision(uint64_t* id, int seconds, std::function<void(uint64_t)> post);
  SOCKET ConnectTo(const std::string& ip);  // a phone on our Wi-Fi Direct group

  HWND notify_ = nullptr;
  std::atomic<bool> stop_{false};
  std::mutex lmx_;
  SOCKET listen_ = INVALID_SOCKET;
  std::vector<std::thread> threads_;

  std::mutex qmx_;
  std::condition_variable qcv_;
  std::deque<Job> queue_;
  std::vector<std::string> sendingTo_;  // phones a SenderLoop is busy with (under qmx_)
  std::deque<Job> nearbyQueue_;  // under qmx_
  std::condition_variable ncv_;
  std::map<std::string, size_t> flushLeft_;       // per phone: its outbox items still queued
  std::map<std::string, ULONGLONG> flushAfter_;  // per phone: don't try its outbox again before this

  std::mutex pmx_;
  std::vector<std::shared_ptr<Live>> running_;    // under pmx_
  std::map<std::string, ULONGLONG> lastActive_;  // per phone: when a transfer with it last began or ended; under pmx_
  bool PhoneIdle(const std::string& phoneId, ULONGLONG now);  // nothing with it for 10 s
  // Asks that phone to let go of its link (some Wi-Fi cards take only so many phones at once).
  void HandOff(const std::string& phoneId);
  std::atomic<bool> handingOff_{false};
  ULONGLONG nextHandOff_ = 0;  // under tmx_

  std::mutex omx_;
  std::condition_variable ocv_;
  uint64_t decisionSeq_ = 0;
  uint64_t pendingDecision_ = 0;
  uint64_t decidedId_ = 0;  // the request decision_ answers
  int decision_ = -1;
  std::map<uint64_t, int> offers_;  // unpaired offers waiting for the user: -1, then 0 / 1
  std::map<uint64_t, bool> offerAll_;  // answered with "accept all" / "decline all"
  struct AutoAnswer {
    bool yes;
    ULONGLONG until;
  };
  std::map<std::string, AutoAnswer> autoAnswer_;  // by sender id, after "... all"
  std::atomic<int> handlers_{0};  // incoming connections being handled

  std::mutex tmx_;
  std::condition_variable tcv_;
  int backoffSec_ = 15;
  ULONGLONG nextProbe_ = 0;
  std::string waking_;         // the phone the beacon is calling (one link at a time)
  std::string calling_;        // nearby (unpaired) phone being called for a send; under tmx_
  std::string nearbyOnLink_, nearbyIp_;  // the nearby phone we're sending to, and where it is
  ULONGLONG wakingSince_ = 0;
  std::atomic<ULONGLONG> lastActivity_{0};  // end of the last transfer
};

}  // namespace td
