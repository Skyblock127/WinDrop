#pragma once
#include <mutex>

#include "Crypto.h"
#include "common.h"

namespace td {

// A phone paired with this PC (the check number was confirmed here).
struct PairedPhone {
  std::string id;
  std::wstring name;
  Bytes key;  // 32-byte key from pairing; stored DPAPI-protected
};

constexpr size_t kMaxPhones = 4;

struct Settings {
  bool enabled = true;    // master switch (tray "On")
  bool pillShown = true;  // tray "Show pill"
  std::vector<PairedPhone> phones;
  std::string lastPhoneId;  // where Ctrl+Alt+P / Send to go: the phone last sent to or heard from
  std::string deviceId;
  std::wstring deviceName;
  std::wstring dataDir;      // %LOCALAPPDATA%\WinDrop
  std::wstring downloadDir;  // %USERPROFILE%\Downloads\WinDrop

  void Load();
  void Save();

  const PairedPhone* Find(const std::string& id) const;
  void AddPhone(const PairedPhone& p);  // replaces a phone with the same id
  void RemovePhone(const std::string& id);
  // The phone things go to by default: the last one used, else the first paired. Empty if none.
  std::string DefaultPhone() const;
};

// Lock-free copies for other threads.
std::vector<PairedPhone> Phones();
Bytes KeyFor(const std::string& phoneId);
std::wstring PhoneLabel(const std::string& phoneId);  // its name, or "your phone"

// Settings fields are touched from network threads; hold this while reading/writing strings.
std::recursive_mutex& CfgMutex();
Settings& Cfg();

struct OutboxItem {
  uint64_t id = 0;
  std::string phoneId;  // who it's for
  bool isText = false;
  std::wstring path;
  std::string text;  // UTF-8
};

class Outbox {
 public:
  void Load(const std::wstring& file);
  uint64_t Add(OutboxItem item);
  bool Remove(uint64_t id);
  void Clear();
  void AssignPhone(const std::string& phoneId);  // items with no phone (from 0.1.x) go to this one
  std::vector<OutboxItem> Snapshot();
  size_t Count();

 private:
  void SaveLocked();
  std::mutex mx_;
  std::vector<OutboxItem> items_;
  std::wstring file_;
  uint64_t nextId_ = 1;
};

Outbox& TheOutbox();

}  // namespace td
