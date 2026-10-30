#include "Crypto.h"

#include <bcrypt.h>
#include <dpapi.h>
#include <wincrypt.h>

#include <algorithm>
#include <ctime>

namespace td {

namespace {

bool Ok(NTSTATUS s) { return s >= 0; }

// Big-endian integers into a byte vector.
void PutU32(Bytes& b, uint32_t v) { for (int i = 3; i >= 0; --i) b.push_back(uint8_t(v >> (i * 8))); }
void PutU64(uint8_t* p, uint64_t v) { for (int i = 7; i >= 0; --i) *p++ = uint8_t(v >> (i * 8)); }

BCRYPT_KEY_HANDLE AesKey(const Bytes& k) {
  BCRYPT_KEY_HANDLE h = nullptr;
  if (!Ok(BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &h, nullptr, 0, (PUCHAR)k.data(), (ULONG)k.size(), 0)))
    return nullptr;
  return h;
}

}  // namespace

Bytes RandomBytes(size_t n) {
  Bytes b(n);
  BCryptGenRandom(nullptr, b.data(), (ULONG)n, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  return b;
}

Bytes Sha256(const Bytes& data) {
  Bytes out(32);
  BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), out.data(), 32);
  return out;
}

Bytes HmacSha256(const Bytes& key, const Bytes& data) {
  Bytes out(32);
  BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, (PUCHAR)key.data(), (ULONG)key.size(), (PUCHAR)data.data(),
             (ULONG)data.size(), out.data(), 32);
  return out;
}

// RFC 5869 with SHA-256; len <= 32 is all we need.
Bytes Hkdf(const Bytes& ikm, const Bytes& salt, const std::string& info, size_t len) {
  Bytes prk = HmacSha256(salt.empty() ? Bytes(32, 0) : salt, ikm);
  Bytes t = Str(info);
  t.push_back(1);
  Bytes okm = HmacSha256(prk, t);
  okm.resize(len);
  return okm;
}

bool SameBytes(const Bytes& a, const Bytes& b) {
  if (a.size() != b.size()) return false;
  uint8_t d = 0;
  for (size_t i = 0; i < a.size(); ++i) d |= a[i] ^ b[i];
  return d == 0;
}

Bytes Concat(std::initializer_list<const Bytes*> parts) {
  Bytes out;
  for (auto* p : parts) out.insert(out.end(), p->begin(), p->end());
  return out;
}

Bytes Str(const std::string& s) { return Bytes(s.begin(), s.end()); }

int64_t BeaconWindow() { return (int64_t)time(nullptr) / 600; }

Bytes BeaconTag(const Bytes& key, int64_t window) {
  Bytes msg = Str("WDR2 ble");
  uint8_t w[8];
  PutU64(w, (uint64_t)window);
  msg.insert(msg.end(), w, w + 8);
  Bytes tag = HmacSha256(key, msg);
  tag.resize(6);
  return tag;
}

std::string ProtectKey(const Bytes& key) {
  DATA_BLOB in{(DWORD)key.size(), (BYTE*)key.data()}, out{};
  if (!CryptProtectData(&in, L"WinDrop pairing key", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return "";
  static const char* hex = "0123456789abcdef";
  std::string s;
  for (DWORD i = 0; i < out.cbData; ++i) { s += hex[out.pbData[i] >> 4]; s += hex[out.pbData[i] & 15]; }
  LocalFree(out.pbData);
  return s;
}

Bytes UnprotectKey(const std::string& stored) {
  if (stored.empty() || stored.size() % 2) return {};
  Bytes blob(stored.size() / 2);
  for (size_t i = 0; i < blob.size(); ++i) blob[i] = (uint8_t)std::stoi(stored.substr(i * 2, 2), nullptr, 16);
  DATA_BLOB in{(DWORD)blob.size(), blob.data()}, out{};
  if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) return {};
  Bytes key(out.pbData, out.pbData + out.cbData);
  SecureZeroMemory(out.pbData, out.cbData);
  LocalFree(out.pbData);
  return key;
}

// ---------------- ECDH P-256 ----------------
EcdhKey::EcdhKey() {
  BCRYPT_KEY_HANDLE h = nullptr;
  if (Ok(BCryptGenerateKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, &h, 256, 0)) && Ok(BCryptFinalizeKeyPair(h, 0))) key_ = h;
}

EcdhKey::~EcdhKey() {
  if (key_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)key_);
}

Bytes EcdhKey::PublicKey() const {
  if (!key_) return {};
  uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
  ULONG got = 0;
  if (!Ok(BCryptExportKey((BCRYPT_KEY_HANDLE)key_, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob, sizeof blob, &got, 0))) return {};
  Bytes pub{0x04};
  pub.insert(pub.end(), blob + sizeof(BCRYPT_ECCKEY_BLOB), blob + sizeof(BCRYPT_ECCKEY_BLOB) + 64);
  return pub;
}

Bytes EcdhKey::Agree(const Bytes& peer) const {
  if (!key_ || peer.size() != 65 || peer[0] != 0x04) return {};
  uint8_t blob[sizeof(BCRYPT_ECCKEY_BLOB) + 64];
  auto* hdr = (BCRYPT_ECCKEY_BLOB*)blob;
  hdr->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
  hdr->cbKey = 32;
  memcpy(blob + sizeof(BCRYPT_ECCKEY_BLOB), peer.data() + 1, 64);
  BCRYPT_KEY_HANDLE pk = nullptr;
  if (!Ok(BCryptImportKeyPair(BCRYPT_ECDH_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &pk, blob, sizeof blob, 0)))
    return {};
  BCRYPT_SECRET_HANDLE sec = nullptr;
  Bytes z(32);
  ULONG got = 0;
  bool ok = Ok(BCryptSecretAgreement((BCRYPT_KEY_HANDLE)key_, pk, &sec, 0)) &&
            Ok(BCryptDeriveKey(sec, BCRYPT_KDF_RAW_SECRET, nullptr, z.data(), 32, &got, 0)) && got == 32;
  if (sec) BCryptDestroySecret(sec);
  BCryptDestroyKey(pk);
  if (!ok) return {};
  std::reverse(z.begin(), z.end());  // CNG hands the raw secret back little-endian
  return z;
}

// ---------------- raw socket I/O ----------------
bool SendRaw(SOCKET s, const void* p, size_t n) {
  auto c = (const char*)p;
  while (n) {
    int r = send(s, c, (int)std::min<size_t>(n, 1 << 20), 0);
    if (r <= 0) return false;
    c += r;
    n -= r;
  }
  return true;
}

bool RecvRaw(SOCKET s, void* p, size_t n) {
  auto c = (char*)p;
  while (n) {
    int r = recv(s, c, (int)std::min<size_t>(n, 1 << 20), 0);
    if (r <= 0) return false;
    c += r;
    n -= r;
  }
  return true;
}

// ---------------- secure channel ----------------
SecureChannel::~SecureChannel() {
  if (txKey_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)txKey_);
  if (rxKey_) BCryptDestroyKey((BCRYPT_KEY_HANDLE)rxKey_);
}

bool SecureChannel::SetKeys(const Bytes& key, const Bytes& nc, const Bytes& ns, bool client) {
  Bytes salt = Concat({&nc, &ns});
  Bytes c2s = Hkdf(key, salt, "WDR2 c2s", 32), s2c = Hkdf(key, salt, "WDR2 s2c", 32);
  txKey_ = AesKey(client ? c2s : s2c);
  rxKey_ = AesKey(client ? s2c : c2s);
  memcpy(txDir_, client ? "c2s" : "s2c", 4);
  memcpy(rxDir_, client ? "s2c" : "c2s", 4);
  return txKey_ && rxKey_;
}

// ---------------- version envelope ----------------
Bytes Envelope(uint8_t type) {
  std::string ver = WINDROP_VERSION;
  Bytes b = Str(kMagic);
  b.push_back(kProto);
  b.push_back(type);
  b.push_back((uint8_t)ver.size());
  b.insert(b.end(), ver.begin(), ver.end());
  return b;
}

bool ReadEnvelope(SOCKET s, uint8_t* type, PeerVersion* peer, bool* compatible) {
  uint8_t h[7];
  if (!RecvRaw(s, h, 7) || memcmp(h, kMagic, 4) != 0) return false;
  peer->proto = h[4];
  *type = h[5];
  peer->version.assign(h[6], '\0');
  if (h[6] && !RecvRaw(s, peer->version.data(), h[6])) return false;
  *compatible = peer->proto == kProto;
  return true;
}

void SendIncompatible(SOCKET s) {
  std::string ver = WINDROP_VERSION;
  Bytes b{kStatusIncompatible, kProto, (uint8_t)ver.size()};
  b.insert(b.end(), ver.begin(), ver.end());
  SendRaw(s, b.data(), b.size());
}

int ReadStatus(SOCKET s, PeerVersion* peer) {
  uint8_t st = 0;
  if (!RecvRaw(s, &st, 1)) return -1;
  if (st == kStatusIncompatible) {
    uint8_t pv[2];
    if (!RecvRaw(s, pv, 2)) return -1;
    peer->proto = pv[0];
    peer->version.assign(pv[1], '\0');
    if (pv[1] && !RecvRaw(s, peer->version.data(), pv[1])) return -1;
  }
  return st;
}

SecureChannel::Outcome SecureChannel::ClientHandshake(const Bytes& key, const std::string& myId, PeerVersion* peer) {
  Bytes nc = RandomBytes(16);
  Bytes hello = Envelope(kHelloSession);
  hello.push_back((uint8_t)myId.size());
  hello.insert(hello.end(), myId.begin(), myId.end());
  hello.insert(hello.end(), nc.begin(), nc.end());
  if (!SendRaw(s, hello.data(), hello.size())) return kFailed;
  int st = ReadStatus(s, peer);
  if (st == kStatusIncompatible) return kIncompatible;
  if (st == kStatusRefused) return kRefused;
  if (st != kStatusGo) return kFailed;
  Bytes ns(16), macS(32);
  if (!RecvRaw(s, ns.data(), 16) || !RecvRaw(s, macS.data(), 32)) return kFailed;
  Bytes srvLabel = Str("WDR2 srv"), cliLabel = Str("WDR2 cli");
  if (!SameBytes(macS, HmacSha256(key, Concat({&srvLabel, &nc, &ns})))) return kRefused;  // not our PC/phone
  Bytes macC = HmacSha256(key, Concat({&cliLabel, &nc, &ns}));
  if (!SendRaw(s, macC.data(), 32)) return kFailed;
  return SetKeys(key, nc, ns, true) ? kOk : kFailed;
}

bool SecureChannel::ServerHandshakeWith(const Bytes&, const Bytes& key, const Bytes& nc) {
  Bytes ns = RandomBytes(16);
  Bytes srvLabel = Str("WDR2 srv"), cliLabel = Str("WDR2 cli");
  Bytes macS = HmacSha256(key, Concat({&srvLabel, &nc, &ns}));
  Bytes go{kStatusGo};
  Bytes reply = Concat({&go, &ns, &macS});
  if (!SendRaw(s, reply.data(), reply.size())) return false;
  Bytes macC(32);
  if (!RecvRaw(s, macC.data(), 32)) return false;
  if (!SameBytes(macC, HmacSha256(key, Concat({&cliLabel, &nc, &ns})))) return false;
  return SetKeys(key, nc, ns, false);
}

// Record: u32 length | ciphertext | 16-byte tag. Nonce: direction label + u64 counter.
// The length header is authenticated as associated data.
bool SecureChannel::SendRecord(const uint8_t* p, size_t n) {
  Bytes rec;
  rec.reserve(4 + n + 16);
  PutU32(rec, (uint32_t)n);
  rec.resize(4 + n + 16);
  uint8_t nonce[12];
  memcpy(nonce, txDir_, 4);
  PutU64(nonce + 4, txSeq_++);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
  BCRYPT_INIT_AUTH_MODE_INFO(ai);
  ai.pbNonce = nonce;
  ai.cbNonce = 12;
  ai.pbAuthData = rec.data();
  ai.cbAuthData = 4;
  ai.pbTag = rec.data() + 4 + n;
  ai.cbTag = 16;
  ULONG got = 0;
  if (!Ok(BCryptEncrypt((BCRYPT_KEY_HANDLE)txKey_, (PUCHAR)p, (ULONG)n, &ai, nullptr, 0, rec.data() + 4, (ULONG)n, &got, 0)))
    return false;
  return SendRaw(s, rec.data(), rec.size());
}

bool SecureChannel::RecvRecord() {
  uint8_t hdr[4];
  if (!RecvRaw(s, hdr, 4)) return false;
  uint32_t n = uint32_t(hdr[0]) << 24 | uint32_t(hdr[1]) << 16 | uint32_t(hdr[2]) << 8 | hdr[3];
  if (n > kRecordMax) return false;
  Bytes ct(n + 16);
  if (!RecvRaw(s, ct.data(), ct.size())) return false;
  uint8_t nonce[12];
  memcpy(nonce, rxDir_, 4);
  PutU64(nonce + 4, rxSeq_++);
  BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO ai;
  BCRYPT_INIT_AUTH_MODE_INFO(ai);
  ai.pbNonce = nonce;
  ai.cbNonce = 12;
  ai.pbAuthData = hdr;
  ai.cbAuthData = 4;
  ai.pbTag = ct.data() + n;
  ai.cbTag = 16;
  in_.resize(n);
  inPos_ = 0;
  ULONG got = 0;
  return Ok(BCryptDecrypt((BCRYPT_KEY_HANDLE)rxKey_, ct.data(), n, &ai, nullptr, 0, in_.data(), n, &got, 0));
}

bool SecureChannel::Send(const void* p, size_t n) {
  auto c = (const uint8_t*)p;
  while (n) {
    size_t take = std::min(n, kRecordMax - out_.size());
    out_.insert(out_.end(), c, c + take);
    c += take;
    n -= take;
    if (out_.size() == kRecordMax && !Flush()) return false;
  }
  return true;
}

bool SecureChannel::Flush() {
  if (out_.empty()) return true;
  bool ok = SendRecord(out_.data(), out_.size());
  out_.clear();
  return ok;
}

bool SecureChannel::Recv(void* p, size_t n) {
  if (!Flush()) return false;  // never wait for a reply with our own bytes still buffered
  auto c = (uint8_t*)p;
  while (n) {
    if (inPos_ == in_.size() && !RecvRecord()) return false;
    size_t take = std::min(n, in_.size() - inPos_);
    memcpy(c, in_.data() + inPos_, take);
    inPos_ += take;
    c += take;
    n -= take;
  }
  return true;
}

}  // namespace td
