#pragma once
#include <atomic>
#include <functional>
#include <string>

namespace td {

// The PC's Wi-Fi and Bluetooth switches (Settings, airplane mode, keyboard keys). Wi-Fi carries
// every transfer (Wi-Fi Direct); Bluetooth is how a pairing phone finds this PC and how this PC
// wakes a paired phone. Until Windows has answered, or where it won't say, both count as on.
class Radios {
 public:
  void Start(std::function<void()> onChange);  // onChange runs on a worker thread, after a change
  bool WifiOn() const { return wifi_ != kOff; }
  bool BluetoothOn() const { return bt_ != kOff; }
  std::wstring OffText() const;  // "Wi-Fi is off", "Bluetooth is off", both, or empty

 private:
  void Read();
  enum : int { kUnknown, kOn, kOff };
  std::atomic<int> wifi_{kUnknown}, bt_{kUnknown};
  std::function<void()> onChange_;
};

Radios& TheRadios();

}  // namespace td
