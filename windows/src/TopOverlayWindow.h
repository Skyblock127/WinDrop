#pragma once
#include "NetworkEngine.h"
#include "common.h"

namespace td {

class DropTarget;

// The "island": a layered, non-activating pill at the top-center of the screen.
// Hidden when idle; wakes when a drag approaches the top edge or a transfer needs attention.
class TopOverlay {
 public:
  bool Create(HINSTANCE inst, NetworkEngine* net);
  void Destroy();
  void SetEnabled(bool on);
  // Tray "Show pill": off keeps everything working in the background with no pill on screen
  // (except a transfer request that needs an answer).
  void SetPillShown(bool on);
  void OnNetEvent(const UiEvent& e);
  void Notify(const std::wstring& title, const std::wstring& sub, bool warn = false);

  // Called by DropTarget.
  void DragEntered();
  void DragMoved(POINT screenPt);
  void DragLeft();
  void Dropped(const std::vector<std::wstring>& files, const std::wstring& text);

 private:
  struct Btn {
    std::wstring label;
    int id;
    uint32_t fill;
    RECT rc{};
  };
  struct Look {
    float w = 260, h = 2, y = 0;
    bool idle = true;
    std::wstring title, sub;
    wchar_t glyph = 0;
    uint32_t accent = 0;
    bool progress = false;
    bool highlight = false;
    bool clickable = false;
    std::vector<Btn> buttons;
    std::vector<std::wstring> spots;  // 2+ phones paired: one drop spot per phone instead of the pill
    int hot = -1;                     // the spot under the cursor
  };
  struct Message {
    bool active = false;
    std::wstring title, sub;
    wchar_t glyph = 0;
    uint32_t accent = 0;
    std::vector<Btn> buttons;
    std::wstring openPath;  // file to select in Explorer, or URL to open
    bool isUrl = false;
  };

  static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
  LRESULT Handle(UINT m, WPARAM w, LPARAM l);
  void Refresh();
  void Tick();
  void Render();
  void PollDrag();
  void ShowMessage(Message m, UINT ms);
  void OnButton(int id);
  void OnBodyClick();
  int HitButton(POINT pt) const;
  float Scale() const;
  void SetClickThrough(bool on);
  void Dismiss();  // the user pushed the pill away

  HWND hwnd_ = nullptr;
  NetworkEngine* net_ = nullptr;
  DropTarget* drop_ = nullptr;
  bool enabled_ = true;
  bool pillShown_ = true;
  bool xferHidden_ = false;  // progress pill already shown its few seconds (or pushed away)
  bool pressed_ = false;
  POINT pressAt_{};
  bool visible_ = false;
  bool animating_ = false;
  bool clickThrough_ = true;
  HMONITOR monitor_ = nullptr;

  // geometry in DIPs
  float cw_ = 260, ch_ = 2, cy_ = 0;
  Look look_;

  int drag_ = 0;  // 0 none, 1 hint, 2 hovering the pill
  std::vector<std::string> spotPhones_;  // phone ids of the drop spots, left to right
  std::vector<RECT> spotRects_;          // their last drawn bounds, screen coordinates
  int hotSpot_ = -1;
  int SpotAt(POINT screenPt) const;
  float BandW() const;
  bool oleInside_ = false;
  bool wasDown_ = false;
  POINT pressPt_{};

  struct {
    bool active = false;
    uint64_t id = 0;
    std::wstring name, sender;
    uint64_t size = 0;
  } offer_;
  struct {
    bool active = false;
    bool sending = false;
    bool fromOutbox = false;
    std::wstring name;
  } xfer_;
  Message msg_;
  std::vector<uint64_t> outboxBatch_;
  int hoverBtn_ = 0;

  // backing store for UpdateLayeredWindow
  HDC memDC_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ oldBmp_ = nullptr;
  void* bits_ = nullptr;
  int dibW_ = 0, dibH_ = 0;
};

}  // namespace td
