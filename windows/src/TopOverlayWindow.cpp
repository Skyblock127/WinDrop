#include "TopOverlayWindow.h"
#include "Nearby.h"

#include "WifiDirectHost.h"

#include <algorithm>
using std::max;
using std::min;
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shellscalingapi.h>

#include <cmath>
#include <memory>

#include "DropTarget.h"
#include "Radios.h"
#include "Settings.h"

namespace td {
namespace {

constexpr wchar_t kClass[] = L"WinDropOverlay";
constexpr UINT_PTR kPollTimer = 1, kAnimTimer = 2, kMsgTimer = 3, kXferTimer = 4, kChooseTimer = 5;
constexpr UINT kPillMs = 4000;  // a pill shows this long, then gets out of the way
// While a drag is near the top, the window grows into an invisible band this big (DIPs) so a
// drop anywhere along the top edge counts, not only on the pill itself.
constexpr float kBandW = 720, kBandH = 130;
constexpr int kBtnAccept = 1, kBtnDecline = 2, kBtnCancelOutbox = 3, kBtnAcceptAll = 4, kBtnDeclineAll = 5,
              kBtnPick = 100;  // + row
constexpr ULONGLONG kFadeMs = 160;

// Solid, non-purple palette.
constexpr uint32_t kBg = 0xF51E1E1E;
constexpr uint32_t kBlue = 0xFF2F7CF6;
constexpr uint32_t kGreen = 0xFF2BB673;
constexpr uint32_t kAmber = 0xFFF5A524;
constexpr uint32_t kRed = 0xFFE5484D;
constexpr uint32_t kGrayBtn = 0xFF3A3A3A;

// Segoe Fluent Icons / MDL2 code points
constexpr wchar_t kGlyphUp = 0xE898, kGlyphDown = 0xE896, kGlyphCheck = 0xE73E, kGlyphWarn = 0xE7BA,
                  kGlyphLink = 0xE71B, kGlyphCopy = 0xE8C8, kGlyphPhone = 0xE8EA, kGlyphCancel = 0xE711;

Gdiplus::Color Fade(uint32_t argb, float a) {
  BYTE alpha = (BYTE)std::lround(((argb >> 24) & 0xFF) * std::clamp(a, 0.f, 1.f));
  return Gdiplus::Color(alpha, (argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
}

void AddRoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
  r = min(r, min(w, h) / 2);
  float d = r * 2;
  if (d < 0.5f) { p.AddRectangle(Gdiplus::RectF(x, y, w, h)); return; }
  p.AddArc(x, y, d, d, 180, 90);
  p.AddArc(x + w - d, y, d, d, 270, 90);
  p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
  p.AddArc(x, y + h - d, d, d, 90, 90);
  p.CloseFigure();
}

// Tick and cross drawn as strokes: icon-font glyphs sit off-centre in their em box.
void DrawMark(Gdiplus::Graphics& g, wchar_t icon, const Gdiplus::RectF& r, float s, float a) {
  using namespace Gdiplus;
  Pen pen(Fade(0xFFFFFFFF, a), 2.2f * s);
  pen.SetStartCap(LineCapRound);
  pen.SetEndCap(LineCapRound);
  pen.SetLineJoin(LineJoinRound);
  float cx = r.X + r.Width / 2, cy = r.Y + r.Height / 2, k = 7 * s;
  if (icon == kGlyphCheck) {
    PointF pts[3] = {PointF(cx - k * 0.8f, cy + k * 0.05f), PointF(cx - k * 0.2f, cy + k * 0.62f), PointF(cx + k * 0.85f, cy - k * 0.6f)};
    g.DrawLines(&pen, pts, 3);
  } else {
    g.DrawLine(&pen, cx - k * 0.62f, cy - k * 0.62f, cx + k * 0.62f, cy + k * 0.62f);
    g.DrawLine(&pen, cx + k * 0.62f, cy - k * 0.62f, cx - k * 0.62f, cy + k * 0.62f);
  }
}

struct Fonts {
  float scale = 0;
  std::unique_ptr<Gdiplus::Font> title, sub, tiny, icon, btn;
};

Fonts& FontsFor(float s) {
  static Fonts f;
  if (f.scale == s) return f;
  f.scale = s;
  auto make = [&](const wchar_t* fam, const wchar_t* fallback, float px, int style) {
    auto font = std::make_unique<Gdiplus::Font>(fam, px * s, style, Gdiplus::UnitPixel);
    if (font->GetLastStatus() != Gdiplus::Ok)
      font = std::make_unique<Gdiplus::Font>(fallback, px * s, style, Gdiplus::UnitPixel);
    return font;
  };
  f.title = make(L"Segoe UI Semibold", L"Segoe UI", 14, Gdiplus::FontStyleRegular);
  f.sub = make(L"Segoe UI", L"Arial", 12, Gdiplus::FontStyleRegular);
  f.tiny = make(L"Segoe UI", L"Arial", 11, Gdiplus::FontStyleRegular);
  f.btn = make(L"Segoe UI Semibold", L"Segoe UI", 12.5f, Gdiplus::FontStyleRegular);
  f.icon = make(L"Segoe Fluent Icons", L"Segoe MDL2 Assets", 16, Gdiplus::FontStyleRegular);
  return f;
}

std::string DefaultPhone() {
  std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  return Cfg().DefaultPhone();
}

bool LooksLikeUrl(const std::wstring& t) {
  return (t.rfind(L"http://", 0) == 0 || t.rfind(L"https://", 0) == 0) && t.find_first_of(L" \r\n\t") == std::wstring::npos;
}

}  // namespace

bool TopOverlay::Create(HINSTANCE inst, NetworkEngine* net) {
  net_ = net;
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = WndProc;
  wc.hInstance = inst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kClass;
  RegisterClassExW(&wc);

  hwnd_ = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClass,
                          L"WinDrop", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, inst, this);
  if (!hwnd_) return false;

  // No DWM corner rounding / border: Windows 11 drew it as a box around the whole window.
  const DWORD kCornerPref = 33, kDoNotRound = 1;  // DWMWA_WINDOW_CORNER_PREFERENCE, DWMWCP_DONOTROUND
  DwmSetWindowAttribute(hwnd_, kCornerPref, &kDoNotRound, sizeof kDoNotRound);
  const DWORD kBorderColor = 34, kColorNone = 0xFFFFFFFE;  // DWMWA_BORDER_COLOR, DWMWA_COLOR_NONE
  DwmSetWindowAttribute(hwnd_, kBorderColor, &kColorNone, sizeof kColorNone);

  drop_ = new DropTarget(this, hwnd_);
  RegisterDragDrop(hwnd_, drop_);
  memDC_ = CreateCompatibleDC(nullptr);
  monitor_ = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
  SetTimer(hwnd_, kPollTimer, 100, nullptr);
  return true;
}

void TopOverlay::Destroy() {
  if (!hwnd_) return;
  RevokeDragDrop(hwnd_);
  if (drop_) drop_->Release();
  DestroyWindow(hwnd_);
  if (memDC_) {
    if (oldBmp_) SelectObject(memDC_, oldBmp_);
    DeleteDC(memDC_);
  }
  if (dib_) DeleteObject(dib_);
  hwnd_ = nullptr;
}

void TopOverlay::SetEnabled(bool on) {
  enabled_ = on;
  if (!on && drag_) { drag_ = 0; Refresh(); }
}

void TopOverlay::SetPillShown(bool on) {
  pillShown_ = on;
  if (!on) { drag_ = 0; msg_.active = false; Refresh(); }
}

void TopOverlay::Dismiss() {
  Log("ui: pill pushed away");
  drag_ = 0;
  msg_.active = false;
  outboxBatch_.clear();
  if (choose_.active) {
    choose_ = {};
    KillTimer(hwnd_, kChooseTimer);
    Notify(L"Not sent", L"Nothing was sent");
    return;
  }
  if (xfer_.active) xferHidden_ = true;
  KillTimer(hwnd_, kMsgTimer);
  Refresh();
}

LRESULT CALLBACK TopOverlay::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto self = (TopOverlay*)((CREATESTRUCTW*)l)->lpCreateParams;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
    self->hwnd_ = h;
  }
  auto self = (TopOverlay*)GetWindowLongPtrW(h, GWLP_USERDATA);
  return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT TopOverlay::Handle(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_TIMER:
      if (w == kPollTimer) PollDrag();
      else if (w == kAnimTimer) Tick();
      else if (w == kChooseTimer) UpdateChooser();
      else if (w == kXferTimer) {
        KillTimer(hwnd_, kXferTimer);
        if (xfer_.active) { xferHidden_ = true; Refresh(); }
      }
      else if (w == kMsgTimer) {
        KillTimer(hwnd_, kMsgTimer);
        POINT pt;
        RECT rc;
        GetCursorPos(&pt);
        GetWindowRect(hwnd_, &rc);
        if (visible_ && PtInRect(&rc, pt) && !msg_.buttons.empty()) {
          SetTimer(hwnd_, kMsgTimer, 1500, nullptr);  // about to click a button: wait for it
        } else {
          msg_.active = false;
          outboxBatch_.clear();
          Refresh();
        }
      }
      return 0;
    case WM_LBUTTONDOWN:
      pressed_ = true;
      pressAt_ = POINT{(short)LOWORD(l), (short)HIWORD(l)};
      SetCapture(hwnd_);
      return 0;
    case WM_MOUSEMOVE: {
      POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
      // Flick it up and away.
      if (pressed_ && !drag_ && pressAt_.y - pt.y > (LONG)(14 * Scale())) {
        pressed_ = false;
        ReleaseCapture();
        Dismiss();
        return 0;
      }
      int hb = HitButton(pt);
      if (hb != hoverBtn_) { hoverBtn_ = hb; Render(); }
      TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
      TrackMouseEvent(&t);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hoverBtn_) { hoverBtn_ = 0; Render(); }
      return 0;
    case WM_SETCURSOR: {
      POINT pt;
      GetCursorPos(&pt);
      ScreenToClient(hwnd_, &pt);
      if (HitButton(pt) || look_.clickable) {
        SetCursor(LoadCursor(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }
    case WM_LBUTTONUP: {
      if (!pressed_) return 0;
      pressed_ = false;
      ReleaseCapture();
      POINT pt{(short)LOWORD(l), (short)HIWORD(l)};
      if (int b = HitButton(pt)) OnButton(b);
      else OnBodyClick();
      return 0;
    }
    case WM_RBUTTONUP:
      Dismiss();
      return 0;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

float TopOverlay::Scale() const {
  UINT dx = 96, dy = 96;
  if (FAILED(GetDpiForMonitor(monitor_, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
  return dx / 96.f;
}

void TopOverlay::SetClickThrough(bool on) {
  if (on == clickThrough_) return;
  clickThrough_ = on;
  LONG_PTR ex = GetWindowLongPtrW(hwnd_, GWL_EXSTYLE);
  ex = on ? (ex | WS_EX_TRANSPARENT) : (ex & ~WS_EX_TRANSPARENT);
  SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, ex);
}

void TopOverlay::PollDrag() {
  int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
  bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
  POINT pt;
  GetCursorPos(&pt);
  if (down && !wasDown_) pressPt_ = pt;
  wasDown_ = down;
  if (!enabled_ || !pillShown_ || oleInside_) return;

  if (!down) {
    if (drag_) { drag_ = 0; Refresh(); }
    return;
  }
  HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(mon, &mi);
  UINT dpi = 96, dy;
  GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
  float s = dpi / 96.f;
  LONG fromTop = pt.y - mi.rcMonitor.top;
  LONG pressFromTop = pressPt_.y - mi.rcMonitor.top;

  if (drag_ == 0) {
    bool moved = std::abs(pt.x - pressPt_.x) + std::abs(pt.y - pressPt_.y) > (LONG)(16 * s);
    // Drags that *start* up top are tab drags / text selection, not "throw it to the top".
    bool startedBelow = pressFromTop > (LONG)(110 * s);
    GUITHREADINFO gi{sizeof gi};
    bool moveSize = GetGUIThreadInfo(0, &gi) && (gi.flags & GUI_INMOVESIZE);
    // Only the very top edge (5 px) of the middle stretch wakes the pill, so dragging things
    // near the top of a window (tabs, toolbars) never triggers it.
    LONG fromCenter = std::abs(pt.x - (mi.rcMonitor.left + mi.rcMonitor.right) / 2);
    bool atEdge = fromTop < 5 && fromCenter < (LONG)(kBandW / 2 * s);
    if (moved && startedBelow && atEdge && !moveSize) {
      monitor_ = mon;
      drag_ = 1;
      TheWifiDirect().Prewarm(DefaultPhone());  // start waking the phone while the user is still dragging
      Log("ui: drag heading for the pill, prewarming link");
      Refresh();
    }
  } else if (drag_ == 1) {
    // Out of the drop band = no pill. Getting it back means touching the top edge again.
    LONG fromCenter = std::abs(pt.x - (mi.rcMonitor.left + mi.rcMonitor.right) / 2);
    if (fromTop > (LONG)(kBandH * s) || fromCenter > (LONG)(BandW() / 2 * s)) {
      drag_ = 0;
      Refresh();
    }
  }
}

void TopOverlay::DragEntered() {
  oleInside_ = true;
  drag_ = 2;
  Refresh();
}

float TopOverlay::BandW() const { return max(kBandW, look_.w + 60); }

void TopOverlay::DragLeft() {
  oleInside_ = false;
  if (drag_) { drag_ = 1; Refresh(); }
}

void TopOverlay::Dropped(const std::vector<std::wstring>& files, const std::wstring& text) {
  oleInside_ = false;
  drag_ = 0;
  TheWifiDirect().EndPrewarm();
  Send(files, text);
}

std::vector<std::string> TopOverlay::Targets() const {
  std::vector<std::string> ids;
  std::string def = DefaultPhone();
  if (!def.empty()) ids.push_back(def);
  for (auto& p : Phones())
    if (p.id != def) ids.push_back(p.id);
  for (auto& n : NearbyPhones())
    if (ids.size() < 6) ids.push_back(n.id);
  return ids;
}

void TopOverlay::Send(const std::vector<std::wstring>& files, const std::wstring& text) {
  auto ids = Targets();
  bool nearby = std::any_of(ids.begin(), ids.end(), [](const std::string& id) { return IsNearbyId(id); });
  if (ids.size() == 1 && !nearby) { Deliver(ids[0], files, text); return; }
  if (ids.empty() && !Phones().empty()) { Deliver(DefaultPhone(), files, text); return; }
  // Ask where to. The list keeps itself current: nearby phones come and go.
  choose_ = {};
  choose_.active = true;
  choose_.files = files;
  choose_.text = text;
  choose_.what = files.size() > 1 ? std::to_wstring(files.size()) + L" files"
                 : !files.empty()  ? FileNameOf(files[0])
                                   : L"text";
  choose_.ids = ids;
  choose_.until = GetTickCount64() + 20000;
  SetTimer(hwnd_, kChooseTimer, 1000, nullptr);
  Log("ui: asking where to send (%zu choices)", ids.size());
  Refresh();
}

void TopOverlay::UpdateChooser() {
  if (!choose_.active) { KillTimer(hwnd_, kChooseTimer); return; }
  if (GetTickCount64() > choose_.until) {
    bool none = choose_.ids.empty();
    choose_ = {};
    KillTimer(hwnd_, kChooseTimer);
    if (none) Notify(L"No phone found nearby", L"Pair one in the WinDrop window, or on the phone: Let nearby PCs send", true);
    else Notify(L"Not sent", L"No phone was picked");
    return;
  }
  auto ids = Targets();
  if (ids != choose_.ids) { choose_.ids = ids; Refresh(); }
}

void TopOverlay::Deliver(const std::string& phone, const std::vector<std::wstring>& files, const std::wstring& text) {
  std::vector<std::wstring> send;
  int dirs = 0;
  for (auto& f : files) {
    DWORD a = GetFileAttributesW(f.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ++dirs;
    else send.push_back(f);
  }
  if (!send.empty()) net_->SendFiles(send, phone);
  else if (!text.empty()) net_->SendText(W2U(text), phone);

  if (dirs && send.empty()) {
    Notify(L"Folders can't be sent yet", L"Zip the folder, then drop the .zip", true);
  } else {
    Message m;
    m.title = send.size() > 1 ? L"Sending " + std::to_wstring(send.size()) + L" files to " + PhoneLabel(phone) : L"Sending to " + PhoneLabel(phone) + L"…";
    m.sub = IsNearbyId(phone) ? L"Connecting to it; it accepts on its screen" : L"Connecting";
    m.glyph = kGlyphUp;
    m.accent = kBlue;
    ShowMessage(m, 2000);
  }
}

void TopOverlay::Notify(const std::wstring& title, const std::wstring& sub, bool warn) {
  Message m;
  m.title = title;
  m.sub = sub;
  m.glyph = warn ? kGlyphWarn : kGlyphPhone;
  m.accent = warn ? kAmber : kBlue;
  ShowMessage(m, 2500);
}

void TopOverlay::ShowMessage(Message m, UINT ms) {
  bool keepBatch = false;
  for (auto& b : m.buttons)
    if (b.id == kBtnCancelOutbox) keepBatch = true;
  if (!keepBatch) outboxBatch_.clear();
  m.active = true;
  msg_ = std::move(m);
  SetTimer(hwnd_, kMsgTimer, min(ms, kPillMs), nullptr);
  Refresh();
}

void TopOverlay::OnNetEvent(const UiEvent& e) {
  switch (e.type) {
    case NetEv::Offer:
      offer_.active = true;
      offer_.id = e.id;
      offer_.name = e.name;
      offer_.sender = e.path;
      offer_.size = e.size;
      Refresh();
      break;
    case NetEv::OfferClosed:
      if (offer_.id == e.id && offer_.active) { offer_.active = false; Refresh(); }
      break;
    case NetEv::SendStart:
    case NetEv::RecvStart:
      xfer_.active = true;
      xfer_.sending = e.type == NetEv::SendStart;
      xfer_.fromOutbox = e.count != 0;
      xfer_.name = e.name;
      msg_.active = false;
      xferHidden_ = false;
      SetTimer(hwnd_, kXferTimer, kPillMs, nullptr);  // show progress briefly, then get out of the way
      Refresh();
      break;
    case NetEv::SendDone: {
      xfer_.active = false;
      Message m;
      m.title = L"Sent " + e.name;
      m.sub = L"Delivered to " + PhoneLabel(e.phone);
      m.glyph = kGlyphCheck;
      m.accent = kGreen;
      ShowMessage(m, 2000);
      break;
    }
    case NetEv::RecvDone: {
      xfer_.active = false;
      Message m;
      m.title = L"Received " + e.name;
      m.sub = L"Click to open folder";
      m.glyph = kGlyphCheck;
      m.accent = kGreen;
      m.openPath = e.path;
      ShowMessage(m, 3000);
      break;
    }
    case NetEv::TextReceived: {
      xfer_.active = false;
      std::wstring t = U2W(e.text);
      bool url = LooksLikeUrl(t);
      std::wstring preview = t.substr(0, t.find_first_of(L"\r\n"));
      if (preview.size() > 90) preview = preview.substr(0, 90) + L"…";
      Message m;
      m.title = url ? L"Link from phone — copied" : L"Text from phone — copied";
      m.sub = url ? preview + L"  (click to open)" : preview;
      m.glyph = url ? kGlyphLink : kGlyphCopy;
      m.accent = kBlue;
      if (url) { m.openPath = t; m.isUrl = true; }
      ShowMessage(m, 3500);
      break;
    }
    case NetEv::Offline: {
      outboxBatch_.push_back(e.id);
      Message m;
      std::wstring waiting = e.count > 1 ? std::to_wstring(e.count) + L" items waiting" : L"Waiting";
      if (!TheRadios().WifiOn()) {
        // Nothing can connect: say why, so it doesn't look like it's trying.
        m.title = TheRadios().OffText();
        m.sub = waiting + L" \u00b7 sends once Wi-Fi is back on";
        m.glyph = kGlyphWarn;
        m.accent = kAmber;
      } else if (!TheRadios().BluetoothOn()) {
        // The phone can't be called; it still connects when it next sends something.
        m.title = L"Bluetooth is off";
        m.sub = waiting + L" \u00b7 turn it on so the phone can be called";
        m.glyph = kGlyphWarn;
        m.accent = kAmber;
      } else {
        m.title = L"Connecting to " + PhoneLabel(e.phone) + L"\u2026";
        m.sub = e.count > 1 ? waiting + L" \u00b7 they send as soon as it connects" : L"Sends as soon as it connects";
        m.glyph = kGlyphUp;
        m.accent = kBlue;
      }
      m.buttons.push_back({L"Cancel", kBtnCancelOutbox, kGrayBtn});
      ShowMessage(m, 4000);
      break;
    }
    case NetEv::Declined: {
      xfer_.active = false;
      Message m;
      m.title = PhoneLabel(e.phone) + L" declined " + e.name;
      m.glyph = kGlyphCancel;
      m.accent = kRed;
      ShowMessage(m, 2500);
      break;
    }
    case NetEv::Waiting: {
      Message m;
      m.title = L"Waiting for " + PhoneLabel(e.phone) + L" to accept";
      m.sub = e.name;
      m.glyph = kGlyphUp;
      m.accent = kBlue;
      ShowMessage(m, kPillMs);
      break;
    }
    case NetEv::Failed: {
      xfer_.active = false;
      Message m;
      m.title = e.text.empty() ? L"Transfer failed" : U2W(e.text);
      m.sub = e.text.empty() ? e.name : e.name + L" wasn't sent";
      m.glyph = kGlyphWarn;
      m.accent = kRed;
      ShowMessage(m, 3000);
      break;
    }
    case NetEv::PeerSeen:
    case NetEv::PairRequest:
    case NetEv::PairDone:
    case NetEv::Incompatible:
      break;
  }
}

void TopOverlay::OnButton(int id) {
  if (id >= kBtnPick && choose_.active) {
    size_t i = (size_t)(id - kBtnPick);
    if (i >= choose_.ids.size()) return;
    auto c = choose_;
    choose_ = {};
    KillTimer(hwnd_, kChooseTimer);
    Deliver(c.ids[i], c.files, c.text);
  } else if ((id == kBtnAccept || id == kBtnDecline || id == kBtnAcceptAll || id == kBtnDeclineAll) && offer_.active) {
    net_->Decide(offer_.id, id == kBtnAccept || id == kBtnAcceptAll, id == kBtnAcceptAll || id == kBtnDeclineAll);
    offer_.active = false;
    offerExpanded_ = false;
    Refresh();
  } else if (id == kBtnCancelOutbox) {
    net_->CancelOutbox(outboxBatch_);
    size_t n = outboxBatch_.size();
    outboxBatch_.clear();
    Message m;
    m.title = n > 1 ? L"Cancelled " + std::to_wstring(n) + L" items" : L"Cancelled";
    m.sub = L"Won't be sent";
    m.glyph = kGlyphCancel;
    m.accent = kGrayBtn;
    ShowMessage(m, 1500);
  }
}

void TopOverlay::OnBodyClick() {
  if (offer_.active && !drag_) {  // show (or fold away) the whole name
    offerExpanded_ = !offerExpanded_;
    Refresh();
    return;
  }
  if (!msg_.active || msg_.openPath.empty()) return;
  if (msg_.isUrl) {
    ShellExecuteW(nullptr, L"open", msg_.openPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  } else {
    std::wstring args = L"/select,\"" + msg_.openPath + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  }
  msg_.active = false;
  Refresh();
}

int TopOverlay::HitButton(POINT pt) const {
  for (auto& b : look_.buttons)
    if (PtInRect(&b.rc, pt)) return b.id;
  return 0;
}

void TopOverlay::Refresh() {
  Look L;
  if (drag_) {
    L.idle = false;
    L.w = drag_ == 2 ? 340.f : 320.f;
    L.h = drag_ == 2 ? 68.f : 64.f;
    L.y = 8;
    L.title = drag_ == 2 ? L"Release to send" : L"Drop to send";
    // One paired phone and nobody nearby: it goes there. Otherwise the pill asks after the drop.
    auto ids = Targets();
    bool nearby = std::any_of(ids.begin(), ids.end(), [](const std::string& id) { return IsNearbyId(id); });
    L.sub = ids.empty()                ? L"Looks for nearby phones next"
            : ids.size() == 1 && !nearby ? L"To " + PhoneLabel(ids[0])
            : Phones().empty()          ? L"To a nearby phone: pick it next"
                                        : L"Pick the phone next";
    L.glyph = kGlyphUp;
    L.accent = kBlue;
    L.highlight = drag_ == 2;
  } else if (offer_.active) {
    L.idle = false;
    L.w = 430;
    L.y = 10;
    L.title = offer_.name;
    L.sub = L"From " + offer_.sender + L" · " + HumanSize(offer_.size);
    L.glyph = kGlyphDown;
    L.accent = kBlue;
    L.clickable = true;
    if (offerExpanded_) {
      // The whole name, wrapped, and labelled buttons underneath.
      L.expanded = true;
      Gdiplus::Graphics g(memDC_);
      Gdiplus::Font f(L"Segoe UI Semibold", 14, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
      Gdiplus::RectF box;
      g.MeasureString(L.title.c_str(), -1, &f, Gdiplus::RectF(0, 0, L.w - 14 - 36 - 12 - 14, 2000), nullptr, &box);
      L.titleH = std::clamp(box.Height, 20.f, 120.f);
      // "... all" (the rest from this sender too) on its own row above.
      L.h = 16 + L.titleH + 4 + 18 + 14 + 32 + 8 + 32 + 14;
      L.buttons.push_back({L"Decline all", kBtnDeclineAll, kGrayBtn});
      L.buttons.push_back({L"Accept all", kBtnAcceptAll, kGrayBtn});
      L.buttons.push_back({L"Decline", kBtnDecline, kGrayBtn});
      L.buttons.push_back({L"Accept", kBtnAccept, kBlue});
    } else {
      L.h = 72;
      L.buttons.push_back({L"", kBtnDecline, kGrayBtn, kGlyphCancel});
      L.buttons.push_back({L"", kBtnAccept, kBlue, kGlyphCheck});
    }
  } else if (choose_.active) {
    L.idle = false;
    L.w = 400;
    L.y = 10;
    L.stack = true;
    L.title = L"Send " + choose_.what + L" to…";
    L.sub = L"Looking for nearby phones…";
    L.glyph = kGlyphUp;
    L.accent = kBlue;
    for (size_t i = 0; i < choose_.ids.size(); ++i) {
      auto& id = choose_.ids[i];
      L.buttons.push_back({PhoneLabel(id) + (IsNearbyId(id) ? L" · nearby" : L" · paired"), kBtnPick + (int)i, kGrayBtn});
    }
    L.h = 64 + 46.f * L.buttons.size() + (L.buttons.empty() ? 0 : 6);
  } else if (xfer_.active && !xferHidden_ && pillShown_) {
    L.idle = false;
    L.w = 380;
    L.h = 64;
    L.y = 10;
    if (xfer_.sending)
      L.title = (xfer_.fromOutbox ? L"Phone is back — sending " : L"Sending ") + xfer_.name;
    else
      L.title = L"Receiving " + xfer_.name;
    L.glyph = xfer_.sending ? kGlyphUp : kGlyphDown;
    L.accent = kBlue;
    L.progress = true;
  } else if (msg_.active && pillShown_) {
    L.idle = false;
    L.w = msg_.buttons.empty() ? 380.f : 440.f;
    L.h = 64;
    L.y = 10;
    L.title = msg_.title;
    L.sub = msg_.sub;
    L.glyph = msg_.glyph;
    L.accent = msg_.accent;
    L.buttons = msg_.buttons;
    L.clickable = !msg_.openPath.empty();
  }
  // New content fades in; the size animates by itself.
  std::wstring key = L.title + L"\x1f" + L.sub + L"\x1f" + std::to_wstring(L.buttons.size()) + (L.expanded ? L"e" : L"");
  if (key != fadeKey_) {
    fadeKey_ = key;
    fadeAt_ = GetTickCount64();
  }
  look_ = L;

  if (!L.idle) {
    if (!visible_) {
      if (!drag_) {
        POINT pt;
        GetCursorPos(&pt);
        monitor_ = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
      }
      cw_ = L.w * 0.55f;
      ch_ = 4;
      cy_ = 0;
      visible_ = true;
      Render();
      ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    }
    SetClickThrough(false);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  } else {
    SetClickThrough(true);
  }
  if (!animating_) {
    animating_ = true;
    SetTimer(hwnd_, kAnimTimer, 15, nullptr);
  }
}

void TopOverlay::Tick() {
  float tw = look_.idle ? 140.f : look_.w;
  float th = look_.idle ? 2.f : look_.h;
  float ty = look_.idle ? 0.f : look_.y;
  bool settled = true;
  auto step = [&](float& c, float t) {
    float d = t - c;
    if (std::fabs(d) < 0.5f) c = t;
    else { c += d * 0.28f; settled = false; }
  };
  step(cw_, tw);
  step(ch_, th);
  step(cy_, ty);
  if (GetTickCount64() - fadeAt_ < kFadeMs + 20) settled = false;
  if (look_.idle && (settled || ch_ < 3.f)) {
    ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
    KillTimer(hwnd_, kAnimTimer);
    animating_ = false;
    return;
  }
  if (visible_) Render();
  if (settled && !look_.progress) {
    KillTimer(hwnd_, kAnimTimer);
    animating_ = false;
  }
}

// The tall pills: the "send to which phone" list (rows under the title) and the expanded offer
// (whole name wrapped, labelled buttons underneath).
void TopOverlay::DrawTall(Gdiplus::Graphics& g, int W, int H, float s, float ca, int px, int py) {
  using namespace Gdiplus;
  auto& F = FontsFor(s);
  float pad = 14 * s, d = 36 * s;
  float top = 14 * s;
  SolidBrush ib(Fade(look_.accent, ca));
  g.FillEllipse(&ib, pad, top, d, d);
  StringFormat cf;
  cf.SetAlignment(StringAlignmentCenter);
  cf.SetLineAlignment(StringAlignmentCenter);
  SolidBrush white(Fade(0xFFFFFFFF, ca));
  SolidBrush gray(Fade(0xFFB4B4B4, ca));
  if (look_.glyph) {
    wchar_t gs[2] = {look_.glyph, 0};
    g.DrawString(gs, 1, F.icon.get(), RectF(pad, top, d, d), &cf, &white);
  }
  float tx = pad + d + 12 * s, tw = max(10.f, W - tx - pad);
  StringFormat one;
  one.SetTrimming(StringTrimmingEllipsisCharacter);
  one.SetFormatFlags(StringFormatFlagsNoWrap);
  one.SetLineAlignment(StringAlignmentCenter);
  float y;
  if (look_.expanded) {
    StringFormat wrap;
    wrap.SetTrimming(StringTrimmingEllipsisCharacter);
    g.DrawString(look_.title.c_str(), -1, F.title.get(), RectF(tx, top, tw, look_.titleH * s), &wrap, &white);
    y = top + look_.titleH * s + 4 * s;
    g.DrawString(look_.sub.c_str(), -1, F.sub.get(), RectF(tx, y, tw, 18 * s), &one, &gray);
  } else {
    g.DrawString(look_.title.c_str(), -1, F.title.get(), RectF(tx, top + d / 2 - 20 * s, tw, 20 * s), &one, &white);
    g.DrawString(look_.sub.c_str(), -1, F.sub.get(), RectF(tx, top + d / 2 + 1 * s, tw, 18 * s), &one, &gray);
  }
  if (look_.stack) {
    // One full-width row per choice.
    y = 64 * s;
    float bh = 38 * s;
    for (auto& b : look_.buttons) {
      RectF r(pad, y, W - 2 * pad, bh);
      GraphicsPath bp;
      AddRoundRect(bp, r.X, r.Y, r.Width, r.Height, bh / 2);
      uint32_t fill = hoverBtn_ == b.id ? kBlue : b.fill;
      SolidBrush fb(Fade(fill, ca));
      g.FillPath(&fb, &bp);
      StringFormat bf;
      bf.SetAlignment(StringAlignmentCenter);
      bf.SetLineAlignment(StringAlignmentCenter);
      bf.SetTrimming(StringTrimmingEllipsisCharacter);
      bf.SetFormatFlags(StringFormatFlagsNoWrap);
      g.DrawString(b.label.c_str(), -1, F.btn.get(), RectF(r.X + 12 * s, r.Y, r.Width - 24 * s, r.Height), &bf, &white);
      b.rc = RECT{(LONG)r.X + px, (LONG)r.Y + py, (LONG)(r.X + r.Width) + px, (LONG)(r.Y + r.Height) + py};
      y += bh + 8 * s;
    }
    return;
  }
  // Expanded: buttons right-aligned in rows of two along the bottom, the last pair lowest.
  float right = W - pad, bh = 32 * s, by = H - 14 * s - bh;
  StringFormat bf;
  bf.SetAlignment(StringAlignmentCenter);
  bf.SetLineAlignment(StringAlignmentCenter);
  for (int i = (int)look_.buttons.size() - 1; i >= 0; --i) {
    if (i % 2 == 1 && i + 1 < (int)look_.buttons.size()) {  // next row up
      right = W - pad;
      by -= bh + 8 * s;
    }
    auto& b = look_.buttons[i];
    RectF m;
    g.MeasureString(b.label.c_str(), -1, F.btn.get(), PointF(0, 0), &m);
    float bw = m.Width + 36 * s;
    RectF r(right - bw, by, bw, bh);
    GraphicsPath bp;
    AddRoundRect(bp, r.X, r.Y, r.Width, r.Height, bh / 2);
    uint32_t fill = b.fill;
    if (hoverBtn_ == b.id) fill = (fill & 0xFF000000) | (((fill & 0xFEFEFE) >> 1) + 0x404040);
    SolidBrush fb(Fade(fill, ca));
    g.FillPath(&fb, &bp);
    g.DrawString(b.label.c_str(), -1, F.btn.get(), r, &bf, &white);
    b.rc = RECT{(LONG)r.X + px, (LONG)r.Y + py, (LONG)(r.X + r.Width) + px, (LONG)(r.Y + r.Height) + py};
    right -= bw + 8 * s;
  }
}

void TopOverlay::Render() {
  using namespace Gdiplus;
  float s = Scale();
  int W = max(1, (int)std::lround(cw_ * s));
  int H = max(1, (int)std::lround(ch_ * s));
  // While a drag is near the top the window is a wide invisible band from the screen edge down,
  // with the pill drawn inside it; otherwise it's exactly the pill.
  bool band = drag_ != 0;
  int py = band ? (int)std::lround(cy_ * s) : 0;
  int WW = band ? max(W, (int)std::lround(BandW() * s)) : W;
  int WH = band ? max(H + py, (int)std::lround(kBandH * s)) : H;
  int px = (WW - W) / 2;
  int needW = (int)std::ceil(max(460.f, BandW()) * s) + 2, needH = (int)std::ceil(max(look_.h + 20, kBandH) * s) + 2;
  if (needW > dibW_ || needH > dibH_ || !dib_) {
    if (dib_) { SelectObject(memDC_, oldBmp_); DeleteObject(dib_); }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = needW;
    bi.bmiHeader.biHeight = -needH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    dib_ = CreateDIBSection(memDC_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
    oldBmp_ = SelectObject(memDC_, dib_);
    dibW_ = needW;
    dibH_ = needH;
  }
  WW = min(WW, dibW_);
  WH = min(WH, dibH_);
  W = min(W, WW);
  H = min(H, WH - py);
  memset(bits_, 0, (size_t)dibW_ * dibH_ * 4);
  if (band) {
    // Alpha 1 (of 255): invisible, but layered windows only hit-test (and accept drops) where
    // alpha is non-zero.
    auto* px32 = (uint32_t*)bits_;
    for (int y = 0; y < WH; ++y)
      for (int x = 0; x < WW; ++x) px32[(size_t)y * dibW_ + x] = 0x01000000;
  }

  {
    Bitmap bmp(WW, WH, dibW_ * 4, PixelFormat32bppPARGB, (BYTE*)bits_);
    Graphics g(&bmp);
    g.TranslateTransform((REAL)px, (REAL)py);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintAntiAliasGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeHalf);

    GraphicsPath pill;
    AddRoundRect(pill, 0.5f, 0.5f, W - 1.f, H - 1.f, min(H / 2.f, 30 * s));
    SolidBrush bg(Fade(kBg, 1));
    g.FillPath(&bg, &pill);
    Pen border(look_.highlight ? Fade(look_.accent, 1) : Color(40, 255, 255, 255), look_.highlight ? 2 * s : 1 * s);
    g.DrawPath(&border, &pill);

    float ca = look_.h > 30 ? (ch_ - 30) / (look_.h - 30) : 0;
    ca = std::clamp(ca, 0.f, 1.f);
    ca *= std::clamp((cw_ - look_.w * 0.6f) / (look_.w * 0.4f), 0.f, 1.f);
    ca *= std::clamp((GetTickCount64() - fadeAt_) / (float)kFadeMs, 0.f, 1.f);
    for (auto& b : look_.buttons) b.rc = RECT{};
    if (ca > 0.05f && !look_.idle && (look_.stack || look_.expanded)) {
      DrawTall(g, W, H, s, ca, px, py);
    } else if (ca > 0.05f && !look_.idle) {
      auto& F = FontsFor(s);
      float pad = 14 * s;
      float cy = H / 2.f;

      // icon bubble
      float d = 36 * s;
      SolidBrush ib(Fade(look_.accent, ca));
      g.FillEllipse(&ib, pad, cy - d / 2, d, d);
      if (look_.glyph) {
        wchar_t gs[2] = {look_.glyph, 0};
        StringFormat cf;
        cf.SetAlignment(StringAlignmentCenter);
        cf.SetLineAlignment(StringAlignmentCenter);
        SolidBrush wb(Fade(0xFFFFFFFF, ca));
        g.DrawString(gs, 1, F.icon.get(), RectF(pad, cy - d / 2, d, d), &cf, &wb);
      }

      // buttons, right-aligned
      float right = W - pad;
      StringFormat bf;
      bf.SetAlignment(StringAlignmentCenter);
      bf.SetLineAlignment(StringAlignmentCenter);
      for (int i = (int)look_.buttons.size() - 1; i >= 0; --i) {
        auto& b = look_.buttons[i];
        RectF m;
        g.MeasureString(b.label.c_str(), -1, F.btn.get(), PointF(0, 0), &m);
        float bw = b.icon ? 36 * s : m.Width + 28 * s, bh = b.icon ? 36 * s : 32 * s;
        RectF r(right - bw, cy - bh / 2, bw, bh);
        GraphicsPath bp;
        AddRoundRect(bp, r.X, r.Y, r.Width, r.Height, bh / 2);
        uint32_t fill = b.fill;
        if (hoverBtn_ == b.id) fill = (fill & 0xFF000000) | (((fill & 0xFEFEFE) >> 1) + 0x404040);  // lighten
        SolidBrush fb(Fade(fill, ca));
        g.FillPath(&fb, &bp);
        SolidBrush tb(Fade(0xFFFFFFFF, ca));
        if (b.icon) {
          DrawMark(g, b.icon, r, s, ca);
        } else {
          g.DrawString(b.label.c_str(), -1, F.btn.get(), r, &bf, &tb);
        }
        b.rc = RECT{(LONG)r.X + px, (LONG)r.Y + py, (LONG)(r.X + r.Width) + px, (LONG)(r.Y + r.Height) + py};
        right -= bw + 8 * s;
      }

      float tx = pad + d + 12 * s;
      float tw = max(10.f, right - tx - (look_.buttons.empty() ? 0 : 4 * s));
      StringFormat tf;
      tf.SetTrimming(StringTrimmingEllipsisCharacter);
      tf.SetFormatFlags(StringFormatFlagsNoWrap);
      tf.SetLineAlignment(StringAlignmentCenter);
      SolidBrush white(Fade(0xFFFFFFFF, ca));
      SolidBrush gray(Fade(0xFFB4B4B4, ca));

      if (look_.progress) {
        uint64_t done = net_->xferDone, total = net_->xferTotal;
        double frac = total ? (double)done / total : 1.0;
        frac = std::clamp(frac, 0.0, 1.0);
        wchar_t pct[16];
        swprintf(pct, 16, L"%d%%", (int)(frac * 100));
        StringFormat rf;
        rf.SetAlignment(StringAlignmentFar);
        rf.SetLineAlignment(StringAlignmentCenter);
        float pctW = 42 * s;
        g.DrawString(look_.title.c_str(), -1, F.title.get(), RectF(tx, cy - 22 * s, tw - pctW, 20 * s), &tf, &white);
        g.DrawString(pct, -1, F.sub.get(), RectF(tx + tw - pctW, cy - 22 * s, pctW, 20 * s), &rf, &gray);
        float by = cy + 2 * s, bh = 5 * s;
        GraphicsPath track, fillp;
        AddRoundRect(track, tx, by, tw, bh, bh / 2);
        SolidBrush tb(Fade(0x40FFFFFF, ca));
        g.FillPath(&tb, &track);
        if (frac > 0) {
          AddRoundRect(fillp, tx, by, max(bh, (float)(tw * frac)), bh, bh / 2);
          SolidBrush pb(Fade(look_.accent, ca));
          g.FillPath(&pb, &fillp);
        }
        std::wstring sz = HumanSize(done) + L" of " + HumanSize(total);
        g.DrawString(sz.c_str(), -1, F.tiny.get(), RectF(tx, cy + 9 * s, tw, 16 * s), &tf, &gray);
      } else if (look_.sub.empty()) {
        g.DrawString(look_.title.c_str(), -1, F.title.get(), RectF(tx, cy - 11 * s, tw, 22 * s), &tf, &white);
      } else {
        g.DrawString(look_.title.c_str(), -1, F.title.get(), RectF(tx, cy - 20 * s, tw, 20 * s), &tf, &white);
        g.DrawString(look_.sub.c_str(), -1, F.sub.get(), RectF(tx, cy + 1 * s, tw, 18 * s), &tf, &gray);
      }
    }
  }

  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(monitor_, &mi);
  POINT dst{mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - WW) / 2,
            mi.rcMonitor.top + (band ? 0 : (LONG)std::lround(cy_ * s))};
  SIZE sz{WW, WH};
  POINT src{0, 0};
  BLENDFUNCTION bf{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
  HDC screen = GetDC(nullptr);
  UpdateLayeredWindow(hwnd_, screen, &dst, &sz, memDC_, &src, 0, &bf, ULW_ALPHA);
  ReleaseDC(nullptr, screen);
}

}  // namespace td
