#pragma once
// The WinDrop window: everything the tray menu used to hold, on one compact page (plus Settings
// and the full "waiting to send" list), drawn in the same night palette as the phone app.
#include "common.h"

#include <functional>
#include <map>
#include <memory>

namespace Gdiplus {
class Graphics;
class Font;
}

namespace td {

// What the window shows; main.cpp fills it in on every refresh.
struct AppView {
  bool on = false, pillShown = false, startup = false;
  std::wstring status;              // "Connected to Pixel" / "On · ready" / "Off"
  uint32_t statusColor = 0;         // the dot next to it
  std::wstring warning;             // something WinDrop needs (Wi-Fi off...); empty = nothing
  ULONGLONG visibleLeftMs = 0, visibleTotalMs = 1;  // nearby phones can send
  ULONGLONG pairingLeftMs = 0, pairingTotalMs = 1;  // the pairing window is open
  struct Phone {
    std::string id;
    std::wstring name;
    bool linked = false;
  };
  std::vector<Phone> phones;
  size_t maxPhones = 4;
  struct Item {
    uint64_t id = 0;
    std::wstring label, sub;
  };
  std::vector<Item> queue;          // waiting to send
  struct Transfer {
    std::wstring label, sub;  // the file; "To Pixel · 12.0 MB of 450 MB"
    float frac = 0;
    bool sending = true;
  };
  std::vector<Transfer> transfers;  // under way right now
  std::wstring version;             // "Version 0.3.0 · up to date"
  std::wstring updateLabel;         // "Check for updates" / "Update to 0.3.1" / "Checking…"
  bool updateReady = false, updateBusy = false;
  uint64_t pairAskId = 0;           // a phone asking to pair: compare the number
  std::wstring pairAskName, pairAskSas;
};

enum class AppAct {
  On, Pill, Startup, Visible, SendClip, OpenDir, Pair, PairStop, PairYes, PairNo, RemovePhone, CancelItem,
  Update, SaveLogs, ClearLogs, Quit
};

class AppWindow {
 public:
  using StateFn = std::function<AppView()>;
  // num: an offer / outbox id; id: a phone id.
  using ActFn = std::function<void(AppAct act, uint64_t num, const std::string& id)>;

  bool Create(HINSTANCE inst, HICON icon, StateFn state, ActFn act);
  void Show();      // opens (or brings forward) the window
  void Toggle();    // tray click: open, or hide when it's in front
  void Hide();
  bool Shown() const;
  void Refresh();   // the state changed: redraw, and fit the window to it
  void SetAlwaysOnTop(bool on);  // while a pairing number waits for an answer
  HWND hwnd() const { return hwnd_; }

 private:
  enum class Page { Home, Settings, Waiting };
  struct Hit {
    float x, y, w, h;
    int kind;
    uint64_t num;
    std::string id;
  };
  struct Paint;  // draw helpers (AppWindow.cpp)

  static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT, WPARAM, LPARAM);
  float Render(Gdiplus::Graphics& g, bool draw);
  float Header(Paint& p, float y);
  float Home(Paint& p, float y);
  float SettingsPage(Paint& p, float y);
  float WaitingPage(Paint& p, float y);
  void Fit();
  void Click(const Hit& h);
  const Hit* HitAt(int px, int py);
  void PaintNow();
  float Scale() const { return s_; }

  HWND hwnd_ = nullptr;
  HINSTANCE inst_ = nullptr;
  HICON icon_ = nullptr, bigIcon_ = nullptr;
  int bigIconPx_ = 0;
  StateFn state_;
  ActFn act_;
  AppView v_;
  Page page_ = Page::Home;
  std::vector<Hit> hits_;
  float s_ = 1;
  float contentH_ = 0, scroll_ = 0;
  int clientH_ = 0;
  bool centred_ = true;  // opens in the middle of the screen and stays centred as it grows, until moved
  bool placed_ = false;
  // Hover / press, by what the hit is (indices change on every render).
  int hoverKind_ = 0, pressKind_ = 0;
  uint64_t hoverNum_ = 0, pressNum_ = 0;
  std::string hoverId_, pressId_;
  bool tracking_ = false;
  bool animating_ = false;       // a switch is still sliding: paint again shortly
  ULONGLONG deactivatedAt_ = 0;  // lost the focus (a tray click does that first)
  std::string confirmRemove_;   // phone whose "Remove?" row is open
  std::map<int, std::pair<bool, ULONGLONG>> switchAnim_;  // switch -> (state, when it flipped)
};

AppWindow& TheApp();

}  // namespace td
