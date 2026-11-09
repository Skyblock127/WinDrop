#pragma once
#include "NetworkEngine.h"
#include "common.h"

namespace Gdiplus { class Graphics; }

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
  // Sends a drop or the clipboard: straight to the one paired phone, else the pill asks where to
  // (paired phones and nearby ones, kept up to date while it asks).
  void Send(const std::vector<std::wstring>& files, const std::wstring& text);
  // Called by DropTarget.
  void DragEntered();
  void DragLeft();
  void Dropped(const std::vector<std::wstring>& files, const std::wstring& text);

 private:
  struct Btn {
    std::wstring label;
    int id;
    uint32_t fill;
    wchar_t icon = 0;  // round icon-only button (accept / decline) instead of a label
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
    bool stack = false;     // buttons as full-width rows under the title (the "send to which phone" list)
    bool expanded = false;  // tall: the whole title wrapped, buttons in a row underneath
    float titleH = 0;       // expanded: height of the wrapped title, DIPs
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
  void DrawTall(Gdiplus::Graphics& g, int W, int H, float s, float ca, int px, int py);
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
  float BandW() const;
  void Deliver(const std::string& phone, const std::vector<std::wstring>& files, const std::wstring& text);
  std::vector<std::string> Targets() const;  // paired phones (default first), then nearby ones
  void UpdateChooser();
  struct {
    bool active = false;
    std::vector<std::wstring> files;
    std::wstring text, what;
    std::vector<std::string> ids;  // the rows, top to bottom
    ULONGLONG until = 0;
  } choose_;
  bool offerExpanded_ = false;  // the offer pill was clicked: show the whole name
  // Content fades in when the pill's content changes (the size animates on its own).
  std::wstring fadeKey_;
  ULONGLONG fadeAt_ = 0;
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
