#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include "common.h"

namespace td {

class SecureChannel;
struct PeerVersion;

enum class NetEv {
  PeerSeen,      // name = phone name
  Offer,         // (unused since pairing: only the paired phone is accepted)
  OfferClosed,   // id
  SendStart,     // name, size, count = 1 if it came from the outbox
  SendDone,      // name
  RecvStart,     // name, size
  RecvDone,      // name, path
  TextReceived,  // text
  Offline,       // id = outbox item id, count = outbox size
  Declined,      // name
  Failed,        // name
  PairRequest,   // id, name = phone name, text = 6-digit check number; answer with Decide(id, ...)
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
  void Decide(uint64_t id, bool accept);
  void CancelOutbox(const std::vector<uint64_t>& ids);
  void FlushOutbox(const std::string& phoneId);  // what's waiting for the phone now on the link
  void OnLinkUp();

  std::atomic<bool> enabled{true};

  // Live progress of the current transfer, polled by the overlay.
  std::atomic<int> xferDir{0};  // 0 idle, 1 sending, 2 receiving
  std::atomic<uint64_t> xferDone{0}, xferTotal{0};

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
  void SenderLoop();
  void ServerLoop();
  void TickerLoop();
  bool OpenListener();
  Result SendOne(const Job& job);
  void HandleIncoming(SOCKET c, const std::string& ip);
  void HandleSession(SecureChannel& ch, const std::string& ip, const std::string& phoneId);
  void UsedPhone(const std::string& phoneId);  // remember it as the default target
  void HandOff(const std::string& phoneId);    // ask the phone on the link to let it go
  void HandlePair(SOCKET c);
  void PostIncompatible(const PeerVersion& peer);
  void ReceivePhoneLog(SecureChannel& ch, uint64_t size);
  bool WaitDecision(uint64_t* id, int seconds, std::function<void(uint64_t)> post);
  SOCKET ConnectPeer(std::string* usedIp);

  HWND notify_ = nullptr;
  std::atomic<bool> stop_{false};
  std::mutex lmx_;
  SOCKET listen_ = INVALID_SOCKET;
  std::vector<std::thread> threads_;

  std::mutex qmx_;
  std::condition_variable qcv_;
  std::deque<Job> queue_;
  bool flushing_ = false;
  size_t flushLeft_ = 0;

  std::mutex omx_;
  std::condition_variable ocv_;
  uint64_t decisionSeq_ = 0;
  uint64_t pendingDecision_ = 0;
  int decision_ = -1;

  std::mutex tmx_;
  std::condition_variable tcv_;
  int backoffSec_ = 15;
  ULONGLONG nextProbe_ = 0;
  std::string waking_;         // the phone the beacon is calling (one link at a time)
  ULONGLONG wakingSince_ = 0;
  std::atomic<ULONGLONG> lastActivity_{0};  // end of the last transfer
  std::atomic<bool> handingOff_{false};
};

}  // namespace td
