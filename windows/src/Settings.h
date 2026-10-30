#pragma once
#include <mutex>

#include "Crypto.h"
#include "common.h"

namespace td {

struct Settings {
  bool enabled = true;    // master switch (tray "On")
  bool pillShown = true;  // tray "Show pill"
  std::string pairedPhoneId;     // set by pairing (with the check number confirmed here)
  std::wstring pairedPhoneName;
  Bytes pairKey;                 // 32-byte key from pairing; stored DPAPI-protected
  std::string deviceId;
  std::wstring deviceName;
  std::wstring dataDir;      // %LOCALAPPDATA%\WinDrop
  std::wstring downloadDir;  // %USERPROFILE%\Downloads\WinDrop

  void Load();
  void Save();
};

// Settings fields are touched from network threads; hold this while reading/writing strings.
std::recursive_mutex& CfgMutex();
Settings& Cfg();

struct OutboxItem {
  uint64_t id = 0;
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
