#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "common.h"

// Everything security-related, on Windows CNG (bcrypt) only. Must match
// android/.../Crypto.kt byte for byte.
//
// Pairing (once): P-256 ECDH over the Wi-Fi Direct link while "Pair phone…" is open; both screens
// show a 6-digit check number and the user confirms on the PC. Result: a 32-byte key K both keep.
//
// Every connection: both sides prove they hold K (HMAC challenge-response), then all traffic is
// AES-256-GCM records with per-connection keys derived from K and both sides' nonces.
namespace td {

using Bytes = std::vector<uint8_t>;

Bytes RandomBytes(size_t n);
Bytes Sha256(const Bytes& data);
Bytes HmacSha256(const Bytes& key, const Bytes& data);
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const std::string& info, size_t len);
bool SameBytes(const Bytes& a, const Bytes& b);  // constant time
Bytes Concat(std::initializer_list<const Bytes*> parts);
Bytes Str(const std::string& s);

// Time-varying BLE tag: only someone holding K can tell it's us.
Bytes BeaconTag(const Bytes& key, int64_t window);
int64_t BeaconWindow();  // unix time / 600

// The pairing key at rest: protected with DPAPI (only this Windows user can unwrap it).
std::string ProtectKey(const Bytes& key);
Bytes UnprotectKey(const std::string& stored);

class EcdhKey {
 public:
  EcdhKey();
  ~EcdhKey();
  Bytes PublicKey() const;                // 65 bytes: 04 | X | Y
  Bytes Agree(const Bytes& peerPub) const;  // 32-byte shared X coordinate, empty on failure
 private:
  void* key_ = nullptr;
};

// The other side's protocol number and app version, as it reported them.
struct PeerVersion {
  int proto = 0;
  std::string version;
};

// A connection after the handshake: length-prefixed AES-256-GCM records.
class SecureChannel {
 public:
  SOCKET s = INVALID_SOCKET;

  bool Send(const void* p, size_t n);
  bool Recv(void* p, size_t n);
  bool Flush();  // pushes out buffered bytes as a record

  // Client: proves itself to the server holding the same key. `peer` gets the other side's
  // protocol and app version when it answers kIncompatible.
  enum Outcome { kOk, kRefused, kIncompatible, kFailed };
  Outcome ClientHandshake(const Bytes& key, const std::string& myId, PeerVersion* peer);
  // Server, after ReadEnvelope said kHelloSession with our protocol: `keyFor` maps the client's id
  // to its key (empty = unknown, refuse).
  template <class F>
  bool ServerHandshake(F keyFor, std::string* peerId);

  ~SecureChannel();

 private:
  bool SetKeys(const Bytes& key, const Bytes& nc, const Bytes& ns, bool client);
  bool SendRecord(const uint8_t* p, size_t n);
  bool RecvRecord();
  bool ServerHandshakeWith(const Bytes& hello, const Bytes& key, const Bytes& nc);

  void* txKey_ = nullptr;
  void* rxKey_ = nullptr;
  uint8_t txDir_[4] = {}, rxDir_[4] = {};
  uint64_t txSeq_ = 0, rxSeq_ = 0;
  Bytes out_, in_;
  size_t inPos_ = 0;
};

// Raw socket helpers, used before encryption starts.
bool SendRaw(SOCKET s, const void* p, size_t n);
bool RecvRaw(SOCKET s, void* p, size_t n);

constexpr size_t kRecordMax = 1 << 20;

// Version envelope. Its shape must never change, so any two WinDrop releases can at least tell
// each other that they don't match:
//   C->S "WDRP" | proto u8 | type u8 | verLen u8 | version (app version, UTF-8) | type's body
//   S->C status u8: 01 go on (type's reply follows), 00 refused,
//                   FE incompatible, then proto u8 | verLen u8 | version, and the server closes.
// kProto changes only when the bytes on the wire change; app versions with the same kProto work
// together.
inline const char kMagic[5] = "WDRP";
constexpr uint8_t kProto = 3;
constexpr uint8_t kHelloSession = 1, kHelloPair = 2;
constexpr uint8_t kStatusRefused = 0, kStatusGo = 1, kStatusIncompatible = 0xFE;

Bytes Envelope(uint8_t type);  // the client's opening bytes for `type`
// Server: reads the envelope. False on a bad/old header. *compatible = same kProto.
bool ReadEnvelope(SOCKET s, uint8_t* type, PeerVersion* peer, bool* compatible);
void SendIncompatible(SOCKET s);  // our protocol + version, then the caller closes
// Client: reads the server's status byte (and its version when it's kStatusIncompatible). -1 on error.
int ReadStatus(SOCKET s, PeerVersion* peer);

template <class F>
bool SecureChannel::ServerHandshake(F keyFor, std::string* peerId) {
  // envelope | idLen | id | nC[16]   (the envelope is read by the caller)
  uint8_t idLen = 0;
  if (!RecvRaw(s, &idLen, 1) || idLen == 0) return false;
  std::string id(idLen, '\0');
  Bytes nc(16);
  if (!RecvRaw(s, id.data(), idLen) || !RecvRaw(s, nc.data(), 16)) return false;
  Bytes key = keyFor(id);
  if (key.size() != 32) {
    uint8_t no = kStatusRefused;
    SendRaw(s, &no, 1);
    return false;
  }
  *peerId = id;
  return ServerHandshakeWith({}, key, nc);
}

}  // namespace td
