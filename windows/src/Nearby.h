#pragma once
#include <string>
#include <vector>

#include "Crypto.h"

// Phones that aren't paired here but said "nearby PCs may send to me" (their Bluetooth beacon
// "WV" + flags + tag[4] + name). They show up as extra drop spots on the pill; sending to one calls
// it with our "WC" beacon, then the transfer runs over an open (unpaired) session.
namespace td {

struct NearbyPhone {
  std::string id;      // "nearby:" + tag in hex; usable wherever a phone id is
  std::wstring name;
};

void StartNearbyWatch();
void StopNearbyWatch();
std::vector<NearbyPhone> NearbyPhones();  // heard recently, not one of our paired phones
bool IsNearbyId(const std::string& id);
std::wstring NearbyName(const std::string& id);
Bytes NearbyTag(const std::string& id);  // the 4 bytes the phone listens for

// The phone's tag: the first 4 bytes of SHA-256("WV" + its device id). Lets a paired phone that
// is also visible be shown once.
Bytes NearbyTagFor(const std::string& deviceId);

}  // namespace td
