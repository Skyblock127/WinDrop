#include "AppWindow.h"

#include <algorithm>
using std::max;
using std::min;
#include <dwmapi.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <cmath>

namespace td {
namespace {

using namespace Gdiplus;

constexpr wchar_t kClass[] = L"WinDropAppWnd";
constexpr UINT_PTR kTickTimer = 1, kAnimTimer = 2;
constexpr int kAppIconRes = 1;  // res/app.rc

// The phone app's night palette, and the pill's blue. Solid fills only.
constexpr uint32_t kBg = 0xFF0E0F11, kCard = 0xFF1A1B1E, kField = 0xFF232428, kText = 0xFFECEDEF,
                   kMuted = 0xFF8E9096, kAccent = 0xFF2F7CF6, kAmber = 0xFFF5A524, kRed = 0xFFE5484D,
                   kTrackOff = 0xFF3A3B3F, kKnobOff = 0xFFB8B9BD, kWhite = 0xFFFFFFFF;

constexpr float kW = 372;      // card width, logical px
constexpr float kPad = 16;     // window edge to card, card edge to content
constexpr float kIn = kPad * 2;
constexpr float kInW = kW - kPad * 2;

// Segoe Fluent Icons (Windows 11) / Segoe MDL2 Assets (Windows 10): same code points.
constexpr wchar_t kGlyphGear = 0xE713, kGlyphBack = 0xE72B, kGlyphX = 0xE711;

// Hit kinds: pages and confirmations handled here; kAct + AppAct goes to main.cpp.
enum : int { kGoSettings = 1, kGoHome, kGoWaiting, kAskRemove, kCancelRemove, kAct = 100 };
constexpr int Act(AppAct a) { return kAct + int(a); }

struct Fonts {
  float s = 0;
  std::unique_ptr<Font> h1, title, body, sub, label, btn, sas, icon;
};

Fonts& F(float s) {
  static Fonts f;
  if (f.s == s) return f;
  f.s = s;
  auto make = [&](const wchar_t* fam, const wchar_t* fallback, float px) {
    auto font = std::make_unique<Font>(fam, px * s, FontStyleRegular, UnitPixel);
    if (font->GetLastStatus() != Ok) font = std::make_unique<Font>(fallback, px * s, FontStyleRegular, UnitPixel);
    return font;
  };
  f.h1 = make(L"Segoe UI Semibold", L"Segoe UI", 20);
  f.title = make(L"Segoe UI Semibold", L"Segoe UI", 14);
  f.body = make(L"Segoe UI", L"Arial", 13.5f);
  f.sub = make(L"Segoe UI", L"Arial", 12);
  f.label = make(L"Segoe UI Semibold", L"Segoe UI", 11);
  f.btn = make(L"Segoe UI Semibold", L"Segoe UI", 13);
  f.sas = make(L"Segoe UI Semibold", L"Segoe UI", 32);
  f.icon = make(L"Segoe Fluent Icons", L"Segoe MDL2 Assets", 14);
  return f;
}

uint32_t Mix(uint32_t a, uint32_t b, float t) {
  auto ch = [&](int sh) { return uint32_t(((a >> sh) & 0xFF) + (((int)((b >> sh) & 0xFF) - (int)((a >> sh) & 0xFF)) * t)) << sh; };
  return ch(24) | ch(16) | ch(8) | ch(0);
}

uint32_t Lighten(uint32_t c) { return Mix(c, kWhite, 0.12f) | 0xFF000000; }

std::wstring Clock(ULONGLONG ms) {
  ULONGLONG sec = (ms + 999) / 1000;
  wchar_t b[16];
  swprintf_s(b, L"%llu:%02llu", sec / 60, sec % 60);
  return b;
}

std::wstring Upper(std::wstring s) {
  for (auto& c : s) c = towupper(c);
  return s;
}

}  // namespace

// Draw helpers in logical px: content coordinates, scrolled by oy. With draw = false they only
// measure and record hit areas, so the window can be fitted before it's painted.
struct AppWindow::Paint {
  AppWindow* w;
  Graphics& g;
  bool draw;
  float s, oy;
  float alpha = 1;      // dimmed: needs WinDrop switched on
  bool enabled = true;  // dimmed parts take no clicks

  Fonts& f() { return F(s); }
  Color C(uint32_t c) const { return Color(BYTE(((c >> 24) & 0xFF) * alpha), BYTE(c >> 16), BYTE(c >> 8), BYTE(c)); }
  RectF R(float x, float y, float wd, float h) const { return RectF(x * s, (y - oy) * s, wd * s, h * s); }

  void Round(float x, float y, float wd, float h, float r, uint32_t c) {
    if (!draw || wd <= 0 || h <= 0) return;
    RectF b = R(x, y, wd, h);
    float d = min(2 * r * s, min(b.Width, b.Height));
    GraphicsPath path;
    path.AddArc(b.X, b.Y, d, d, 180, 90);
    path.AddArc(b.X + b.Width - d, b.Y, d, d, 270, 90);
    path.AddArc(b.X + b.Width - d, b.Y + b.Height - d, d, d, 0, 90);
    path.AddArc(b.X, b.Y + b.Height - d, d, d, 90, 90);
    path.CloseFigure();
    SolidBrush br(C(c));
    g.FillPath(&br, &path);
  }
  void Circle(float cx, float cy, float r, uint32_t c) {
    if (!draw) return;
    SolidBrush br(C(c));
    g.FillEllipse(&br, R(cx - r, cy - r, 2 * r, 2 * r));
  }
  // align: 0 left, 1 centre, 2 right. One line (ellipsis) unless wrap.
  void Text(const std::wstring& t, const std::unique_ptr<Font>& fnt, uint32_t c, float x, float y, float wd, float h, int align = 0, bool wrap = false) {
    if (!draw || t.empty()) return;
    StringFormat sf;
    sf.SetAlignment(align == 1 ? StringAlignmentCenter : align == 2 ? StringAlignmentFar : StringAlignmentNear);
    if (wrap) {
      sf.SetLineAlignment(StringAlignmentNear);
    } else {
      sf.SetLineAlignment(StringAlignmentCenter);
      sf.SetFormatFlags(StringFormatFlagsNoWrap);
      sf.SetTrimming(StringTrimmingEllipsisCharacter);
    }
    SolidBrush br(C(c));
    g.DrawString(t.c_str(), (INT)t.size(), fnt.get(), R(x, y, wd, h), &sf, &br);
  }
  float TextH(const std::wstring& t, const std::unique_ptr<Font>& fnt, float wd) {
    RectF out;
    StringFormat sf;
    g.MeasureString(t.c_str(), (INT)t.size(), fnt.get(), RectF(0, 0, wd * s, 10000), &sf, &out);
    return std::ceil(out.Height / s);
  }
  void Hit(float x, float y, float wd, float h, int kind, uint64_t num = 0, const std::string& id = {}) {
    if (enabled) w->hits_.push_back({x, y, wd, h, kind, num, id});
  }
  bool Hover(int kind, uint64_t num = 0, const std::string& id = {}) const {
    return enabled && w->hoverKind_ == kind && w->hoverNum_ == num && w->hoverId_ == id;
  }

  void Card(float y, float h) { Round(kPad, y, kW, h, 14, kCard); }
  void Label(const std::wstring& t, uint32_t c, float y, const std::wstring& right = {}) {
    Text(Upper(t), f().label, c, kIn, y, kInW, 16);
    if (!right.empty()) Text(right, f().sub, kMuted, kIn, y, kInW, 16, 2);
  }
  void Bar(float x, float y, float wd, float frac) {
    Round(x, y, wd, 4, 2, kField);
    Round(x, y, wd * max(0.f, min(1.f, frac)), 4, 2, kAccent);
  }
  // style: 0 plain, 1 blue, 2 red.
  void Button(float x, float y, float wd, float h, const std::wstring& label, int style, int kind, uint64_t num = 0,
              const std::string& id = {}, uint32_t textColor = kText) {
    uint32_t fill = style == 1 ? kAccent : style == 2 ? kRed : kField;
    if (Hover(kind, num, id)) fill = Lighten(fill);
    Round(x, y, wd, h, 10, fill);
    Text(label, f().btn, style ? kWhite : textColor, x + 10, y, wd - 20, h, 1);
    Hit(x, y, wd, h, kind, num, id);
  }
  void IconButton(float x, float y, float size, wchar_t glyph, int kind, uint64_t num = 0, const std::string& id = {},
                  uint32_t bg = kField) {
    if (Hover(kind, num, id)) Round(x, y, size, size, size / 2, Lighten(bg));
    wchar_t gs[2] = {glyph, 0};
    Text(gs, f().icon, Hover(kind, num, id) ? kText : kMuted, x, y, size, size, 1);
    Hit(x, y, size, size, kind, num, id);
  }
  // 40 x 22; the knob slides when the state flips.
  void Switch(float x, float y, bool on, int kind) {
    float pos = on ? 1.f : 0.f;
    ULONGLONG now = GetTickCount64();
    auto it = w->switchAnim_.find(kind);
    if (it == w->switchAnim_.end()) {
      w->switchAnim_[kind] = {on, 0};
    } else {
      if (it->second.first != on) it->second = {on, now};
      if (it->second.second) {
        float t = min(1.f, float(now - it->second.second) / 160.f);
        if (t < 1) w->animating_ = true;
        t = 1 - (1 - t) * (1 - t);
        pos = on ? t : 1 - t;
      }
    }
    Round(x, y, 40, 22, 11, Mix(kTrackOff, kAccent, pos));
    Circle(x + 11 + 18 * pos, y + 11, 8, Mix(kKnobOff, kWhite, pos));
  }
  // A whole-row switch: title, a line under it, the switch on the right.
  float ToggleRow(float y, const std::wstring& title, const std::wstring& sub, bool on, int kind) {
    float h = sub.empty() ? 40.f : 52.f;
    if (Hover(kind)) Round(kIn - 8, y, kInW + 16, h, 10, kField);
    if (sub.empty()) {
      Text(title, f().title, kText, kIn, y, kInW - 52, h);
    } else {
      Text(title, f().title, kText, kIn, y + 7, kInW - 52, 20);
      Text(sub, f().sub, kMuted, kIn, y + 27, kInW - 52, 18);
    }
    Switch(kIn + kInW - 40, y + (h - 22) / 2, on, kind);
    Hit(kIn - 8, y, kInW + 16, h, kind);
    return y + h;
  }
};

AppWindow& TheApp() {
  static AppWindow w;
  return w;
}

bool AppWindow::Create(HINSTANCE inst, HICON icon, StateFn state, ActFn act) {
  inst_ = inst;
  icon_ = icon;
  state_ = std::move(state);
  act_ = std::move(act);
  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = Proc;
  wc.hInstance = inst;
  wc.lpszClassName = kClass;
  wc.hIcon = icon;
  wc.hIconSm = icon;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);
  hwnd_ = CreateWindowExW(WS_EX_APPWINDOW, kClass, L"WinDrop", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                          CW_USEDEFAULT, CW_USEDEFAULT, 200, 200, nullptr, nullptr, inst, this);
  if (!hwnd_) return false;
  // Dark title bar in the window's own background colour (Windows 11; older ones ignore these).
  BOOL dark = TRUE;
  DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof dark);
  COLORREF cap = RGB((kBg >> 16) & 0xFF, (kBg >> 8) & 0xFF, kBg & 0xFF), txt = RGB(0xEC, 0xED, 0xEF);
  DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &cap, sizeof cap);
  DwmSetWindowAttribute(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &txt, sizeof txt);
  return true;
}

bool AppWindow::Shown() const { return hwnd_ && IsWindowVisible(hwnd_) && !IsIconic(hwnd_); }

void AppWindow::Show() {
  if (!hwnd_) return;
  v_ = state_();
  Fit();
  ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
  SetForegroundWindow(hwnd_);
  SetTimer(hwnd_, kTickTimer, 1000, nullptr);
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::Toggle() {
  // Clicking the tray icon takes the focus first: "was in front" means it lost it just now.
  if (Shown() && (GetForegroundWindow() == hwnd_ || GetTickCount64() - deactivatedAt_ < 500)) Hide();
  else Show();
}

void AppWindow::Hide() {
  if (!hwnd_) return;
  ShowWindow(hwnd_, SW_HIDE);
  KillTimer(hwnd_, kTickTimer);
  page_ = Page::Home;
  scroll_ = 0;
  confirmRemove_.clear();
}

void AppWindow::Refresh() {
  if (!hwnd_ || !IsWindowVisible(hwnd_)) return;
  v_ = state_();
  if (page_ == Page::Waiting && v_.queue.empty()) page_ = Page::Home;
  if (!confirmRemove_.empty() &&
      std::none_of(v_.phones.begin(), v_.phones.end(), [&](const AppView::Phone& p) { return p.id == confirmRemove_; }))
    confirmRemove_.clear();
  Fit();
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void AppWindow::SetAlwaysOnTop(bool on) {
  if (hwnd_) SetWindowPos(hwnd_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

// Sizes the window to its content (scrolling only past the screen's height). It opens above the
// tray and grows upward from there, until it's moved.
void AppWindow::Fit() {
  UINT dpi = GetDpiForWindow(hwnd_);
  s_ = dpi / 96.f;
  HDC mem = CreateCompatibleDC(nullptr);
  {
    Graphics g(mem);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    contentH_ = Render(g, false);
  }
  DeleteDC(mem);

  DWORD style = (DWORD)GetWindowLongW(hwnd_, GWL_STYLE), ex = (DWORD)GetWindowLongW(hwnd_, GWL_EXSTYLE);
  RECT frame{0, 0, 0, 0};
  AdjustWindowRectExForDpi(&frame, style, FALSE, ex, dpi);
  int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
  POINT cur;
  GetCursorPos(&cur);
  HMONITOR mon = placed_ ? MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST) : MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
  MONITORINFO mi{sizeof mi};
  GetMonitorInfoW(mon, &mi);
  RECT work = mi.rcWork;
  int margin = int(12 * s_);
  int cw = int((kW + 2 * kPad) * s_ + 0.5f);
  int ch = min(int(contentH_ * s_ + 0.5f), int(work.bottom - work.top) - fh - 2 * margin);
  clientH_ = ch;
  scroll_ = max(0.f, min(scroll_, contentH_ - ch / s_));
  int ww = cw + fw, wh = ch + fh;
  RECT wr;
  GetWindowRect(hwnd_, &wr);
  int x, y;
  if (!placed_) {
    x = work.right - ww - margin;
    y = work.bottom - wh - margin;
    placed_ = true;
    anchoredBottom_ = true;
  } else {
    x = wr.left;
    y = anchoredBottom_ ? wr.bottom - wh : wr.top;
  }
  x = max<int>(work.left, min<int>(x, work.right - ww));
  y = max<int>(work.top, min<int>(y, work.bottom - wh));
  if (x != wr.left || y != wr.top || ww != wr.right - wr.left || wh != wr.bottom - wr.top)
    SetWindowPos(hwnd_, nullptr, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
}

float AppWindow::Render(Graphics& g, bool draw) {
  hits_.clear();
  animating_ = false;
  Paint p{this, g, draw, s_, scroll_};
  float y = Header(p, 14);
  if (page_ == Page::Settings) y = SettingsPage(p, y);
  else if (page_ == Page::Waiting) y = WaitingPage(p, y);
  else y = Home(p, y);
  return y + kPad - 12;  // the last card's gap becomes the bottom margin
}

float AppWindow::Header(Paint& p, float y) {
  auto& f = p.f();
  if (page_ != Page::Home) {
    p.IconButton(kPad - 4, y + 4, 32, kGlyphBack, kGoHome, 0, {}, kBg);
    p.Text(page_ == Page::Settings ? L"Settings" : L"Waiting to send", f.h1, kText, kPad + 34, y, kW - 40, 40);
    return y + 40 + 14;
  }
  if (p.draw) {
    int px = int(40 * s_ + 0.5f);
    if (px != bigIconPx_) {
      if (bigIcon_) DestroyIcon(bigIcon_);
      bigIcon_ = (HICON)LoadImageW(inst_, MAKEINTRESOURCEW(kAppIconRes), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
      bigIconPx_ = px;
    }
    if (bigIcon_) {
      HDC dc = p.g.GetHDC();
      DrawIconEx(dc, int(kPad * s_), int((y - scroll_) * s_), bigIcon_, px, px, 0, nullptr, DI_NORMAL);
      p.g.ReleaseHDC(dc);
    }
  }
  float tx = kPad + 52;
  p.Text(L"WinDrop", f.h1, kText, tx, y - 2, 200, 26);
  p.Circle(tx + 4, y + 31, 4, v_.statusColor);
  p.Text(v_.status, f.sub, kMuted, tx + 14, y + 22, kW - 52 - 14 - 92, 18);
  p.IconButton(kPad + kW - 40 - 44, y + 4, 32, kGlyphGear, kGoSettings, 0, {}, kBg);
  p.Switch(kPad + kW - 40, y + 9, v_.on, Act(AppAct::On));
  p.Hit(kPad + kW - 44, y + 4, 48, 32, Act(AppAct::On));
  return y + 40 + 14;
}

float AppWindow::Home(Paint& p, float y) {
  auto& f = p.f();
  const float gap = 12;

  // A phone asking to pair: the number first, above everything.
  if (v_.pairAskId) {
    float h = kPad + 16 + 6 + 44 + 4 + 18 + 12 + 40 + kPad;
    p.Round(kPad, y, kW, h, 14, kAccent);
    p.Round(kPad + 2, y + 2, kW - 4, h - 4, 12, kCard);
    float cy = y + kPad;
    p.Label(L"Pair with " + v_.pairAskName + L"?", kAccent, cy);
    cy += 22;
    std::wstring sas = v_.pairAskSas.size() == 6 ? v_.pairAskSas.substr(0, 3) + L" " + v_.pairAskSas.substr(3) : v_.pairAskSas;
    p.Text(sas, f.sas, kText, kIn, cy, kInW, 44, 1);
    cy += 48;
    p.Text(L"Pair only if your phone shows the same number.", f.sub, kMuted, kIn, cy, kInW, 18, 1);
    cy += 30;
    float bw = (kInW - 8) / 2;
    p.Button(kIn, cy, bw, 40, L"Don't pair", 0, Act(AppAct::PairNo), v_.pairAskId);
    p.Button(kIn + bw + 8, cy, bw, 40, L"Pair", 1, Act(AppAct::PairYes), v_.pairAskId);
    y += h + gap;
  }

  if (!v_.warning.empty()) {
    float th = p.TextH(v_.warning, f.body, kInW);
    float h = kPad + 22 + th + kPad - 2;
    p.Card(y, h);
    p.Label(L"Needs attention", kAmber, y + kPad);
    p.Text(v_.warning, f.body, kText, kIn, y + kPad + 22, kInW, th + 4, 0, true);
    y += h + gap;
  }

  // Send, and the nearby-phones switch.
  {
    bool vis = v_.visibleLeftMs > 0;
    float h = kPad + 40 + 10 + 52 + (vis ? 10 : 0) + kPad - 6;
    p.Card(y, h);
    float cy = y + kPad;
    p.enabled = v_.on;
    p.alpha = v_.on ? 1.f : 0.4f;
    float bw = (kInW - 8) / 2;
    p.Button(kIn, cy, bw, 40, L"Send clipboard", 0, Act(AppAct::SendClip));
    p.alpha = 1;
    p.enabled = true;
    p.Button(kIn + bw + 8, cy, bw, 40, L"Received files", 0, Act(AppAct::OpenDir));
    cy += 40 + 10;
    p.enabled = v_.on;
    p.alpha = v_.on ? 1.f : 0.4f;
    std::wstring sub = vis ? Clock(v_.visibleLeftMs) + L" left · each one asks here first"
                           : L"For 10 minutes · each one asks here first";
    cy = p.ToggleRow(cy, L"Let nearby phones send", sub, vis, Act(AppAct::Visible));
    if (vis) p.Bar(kIn, cy + 2, kInW, float(v_.visibleLeftMs) / float(v_.visibleTotalMs));
    p.alpha = 1;
    p.enabled = true;
    y += h + gap;
  }

  // Paired phones, and pairing.
  {
    bool pairing = v_.pairingLeftMs > 0;
    std::wstring removeNote = L"It can't send here or get files until it's paired again. Anything waiting for it is dropped.";
    float noteH = p.TextH(removeNote, f.sub, kInW - 24);
    std::wstring emptyNote = L"No phone paired yet. Pair one to send to it whenever it's in reach.";
    float h = kPad + 22;
    if (v_.phones.empty()) h += p.TextH(emptyNote, f.body, kInW) + 10;
    for (auto& ph : v_.phones) h += (ph.id == confirmRemove_ ? 12 + 20 + 4 + noteH + 10 + 36 + 12 : 52) + 6;
    h += 4 + (pairing ? 74 : 40) + kPad;
    p.Card(y, h);
    float cy = y + kPad;
    p.Label(L"Paired phones", kMuted, cy, v_.phones.empty() ? L"" : std::to_wstring(v_.phones.size()) + L" of " + std::to_wstring(v_.maxPhones));
    cy += 22;
    if (v_.phones.empty()) {
      float th = p.TextH(emptyNote, f.body, kInW);
      p.Text(emptyNote, f.body, kMuted, kIn, cy, kInW, th + 4, 0, true);
      cy += th + 10;
    }
    for (auto& ph : v_.phones) {
      if (ph.id == confirmRemove_) {
        float rh = 12 + 20 + 4 + noteH + 10 + 36 + 12;
        p.Round(kIn, cy, kInW, rh, 12, kField);
        p.Text(L"Remove " + ph.name + L"?", f.title, kText, kIn + 12, cy + 12, kInW - 24, 20);
        p.Text(removeNote, f.sub, kMuted, kIn + 12, cy + 36, kInW - 24, noteH + 4, 0, true);
        float by = cy + 36 + noteH + 10, bw = 96;
        p.Button(kIn + kInW - 12 - bw, by, bw, 36, L"Remove", 2, Act(AppAct::RemovePhone), 0, ph.id);
        p.Button(kIn + kInW - 12 - 2 * bw - 8, by, bw, 36, L"Cancel", 0, kCancelRemove, 0, {}, kText);
        cy += rh + 6;
        continue;
      }
      p.Round(kIn, cy, kInW, 52, 12, kField);
      p.Circle(kIn + 18, cy + 26, 4, ph.linked ? 0xFF2BB673 : kTrackOff);
      p.Text(ph.name, f.title, kText, kIn + 32, cy + 7, kInW - 32 - 48, 20);
      p.Text(ph.sub, f.sub, kMuted, kIn + 32, cy + 27, kInW - 32 - 48, 18);
      p.IconButton(kIn + kInW - 42, cy + 10, 32, kGlyphX, kAskRemove, 0, ph.id);
      cy += 52 + 6;
    }
    cy += 4;
    p.enabled = v_.on || pairing;
    p.alpha = p.enabled ? 1.f : 0.4f;
    if (pairing) {
      p.Round(kIn, cy, kInW, 74, 12, kField);
      p.Text(L"Pairing is open · " + Clock(v_.pairingLeftMs), f.title, kText, kIn + 14, cy + 10, kInW - 28 - 80, 20);
      p.Text(L"On the phone: WinDrop → Pair with PC", f.sub, kMuted, kIn + 14, cy + 30, kInW - 28 - 80, 18);
      p.Button(kIn + kInW - 14 - 72, cy + 12, 72, 32, L"Stop", 0, Act(AppAct::PairStop));
      p.Bar(kIn + 14, cy + 58, kInW - 28, float(v_.pairingLeftMs) / float(v_.pairingTotalMs));
    } else if (v_.phones.size() >= v_.maxPhones) {
      p.Text(L"That's the most WinDrop pairs: remove one to pair another.", f.sub, kMuted, kIn, cy, kInW, 40, 1);
    } else {
      p.Button(kIn, cy, kInW, 40, v_.phones.empty() ? L"Pair phone" : L"Pair another phone", v_.phones.empty() ? 1 : 0,
               Act(AppAct::Pair));
    }
    p.alpha = 1;
    p.enabled = true;
    y += h + gap;
  }

  // Waiting to send: the first few, the rest on their own page.
  if (!v_.queue.empty()) {
    size_t shown = min<size_t>(v_.queue.size(), 3);
    float h = kPad + 22 + 20 + shown * (46 + 6) + 4 + 40 + kPad;
    p.Card(y, h);
    float cy = y + kPad;
    p.Label(L"Waiting to send", kAmber, cy, std::to_wstring(v_.queue.size()));
    cy += 22;
    p.Text(L"Goes out by itself when the phone is in reach.", f.sub, kMuted, kIn, cy, kInW, 16);
    cy += 20;
    for (size_t i = 0; i < shown; ++i) {
      auto& it = v_.queue[i];
      p.Round(kIn, cy, kInW, 46, 10, kField);
      p.Text(it.label, f.body, kText, kIn + 12, cy + 5, kInW - 12 - 44, 19);
      p.Text(it.sub, f.sub, kMuted, kIn + 12, cy + 24, kInW - 12 - 44, 16);
      p.IconButton(kIn + kInW - 40, cy + 7, 32, kGlyphX, Act(AppAct::CancelItem), it.id);
      cy += 46 + 6;
    }
    cy += 4;
    if (v_.queue.size() > shown) {
      float bw = (kInW - 8) / 2;
      p.Button(kIn, cy, bw, 40, L"Show all " + std::to_wstring(v_.queue.size()), 0, kGoWaiting);
      p.Button(kIn + bw + 8, cy, bw, 40, L"Cancel all", 0, Act(AppAct::CancelAll));
    } else {
      p.Button(kIn, cy, kInW, 40, v_.queue.size() == 1 ? L"Cancel it" : L"Cancel all", 0, Act(AppAct::CancelAll));
    }
    y += h + gap;
  }
  return y;
}

float AppWindow::SettingsPage(Paint& p, float y) {
  auto& f = p.f();
  const float gap = 12;
  {
    float h = kPad + 22 + 52 + 52 + 8 + 18 + kPad;
    p.Card(y, h);
    float cy = y + kPad;
    p.Label(L"General", kMuted, cy);
    cy += 22;
    p.enabled = v_.on;
    p.alpha = v_.on ? 1.f : 0.4f;
    cy = p.ToggleRow(cy, L"Show pill", L"The bar at the top of the screen to drop files on", v_.pillShown, Act(AppAct::Pill));
    p.enabled = true;
    p.alpha = 1;
    cy = p.ToggleRow(cy, L"Start with Windows", L"WinDrop waits in the tray from sign-in", v_.startup, Act(AppAct::Startup));
    cy += 8;
    p.Text(L"Ctrl+Alt+P sends the clipboard from any app.", f.sub, kMuted, kIn, cy, kInW, 18);
    y += h + gap;
  }
  {
    float h = kPad + 22 + 20 + 10 + 40 + 8 + 40 + 8 + 40 + kPad;
    p.Card(y, h);
    float cy = y + kPad;
    p.Label(L"App", kMuted, cy);
    cy += 22;
    p.Text(v_.version, f.body, kText, kIn, cy, kInW, 20);
    cy += 20 + 10;
    p.enabled = !v_.updateBusy;
    p.alpha = v_.updateBusy ? 0.5f : 1.f;
    p.Button(kIn, cy, kInW, 40, v_.updateLabel, v_.updateReady ? 1 : 0, Act(AppAct::Update));
    p.enabled = true;
    p.alpha = 1;
    cy += 48;
    float bw = (kInW - 8) / 2;
    p.Button(kIn, cy, bw, 40, L"Save logs", 0, Act(AppAct::SaveLogs));
    p.Button(kIn + bw + 8, cy, bw, 40, L"Clear logs", 0, Act(AppAct::ClearLogs));
    cy += 48;
    p.Button(kIn, cy, kInW, 40, L"Quit WinDrop", 0, Act(AppAct::Quit), 0, {}, kRed);
    y += h + gap;
  }
  return y;
}

float AppWindow::WaitingPage(Paint& p, float y) {
  auto& f = p.f();
  float h = kPad + 20 + v_.queue.size() * (46 + 6) + 4 + 40 + kPad;
  p.Card(y, h);
  float cy = y + kPad;
  p.Text(L"Goes out by itself when the phone is in reach.", f.sub, kMuted, kIn, cy, kInW, 16);
  cy += 20;
  for (auto& it : v_.queue) {
    p.Round(kIn, cy, kInW, 46, 10, kField);
    p.Text(it.label, f.body, kText, kIn + 12, cy + 5, kInW - 12 - 44, 19);
    p.Text(it.sub, f.sub, kMuted, kIn + 12, cy + 24, kInW - 12 - 44, 16);
    p.IconButton(kIn + kInW - 40, cy + 7, 32, kGlyphX, Act(AppAct::CancelItem), it.id);
    cy += 46 + 6;
  }
  cy += 4;
  p.Button(kIn, cy, kInW, 40, L"Cancel all", 0, Act(AppAct::CancelAll));
  return y + h + 12;
}

const AppWindow::Hit* AppWindow::HitAt(int px, int py) {
  float x = px / s_, y = py / s_ + scroll_;
  for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)  // later (smaller, on top) first
    if (x >= it->x && x < it->x + it->w && y >= it->y && y < it->y + it->h) return &*it;
  return nullptr;
}

void AppWindow::Click(const Hit& h) {
  switch (h.kind) {
    case kGoSettings: page_ = Page::Settings; scroll_ = 0; confirmRemove_.clear(); Refresh(); return;
    case kGoHome: page_ = Page::Home; scroll_ = 0; Refresh(); return;
    case kGoWaiting: page_ = Page::Waiting; scroll_ = 0; Refresh(); return;
    case kAskRemove: confirmRemove_ = h.id; Refresh(); return;
    case kCancelRemove: confirmRemove_.clear(); Refresh(); return;
  }
  if (h.kind < kAct) return;
  auto a = AppAct(h.kind - kAct);
  if (a == AppAct::RemovePhone) confirmRemove_.clear();
  act_(a, h.num, h.id);
  if (a != AppAct::Quit) Refresh();
}

void AppWindow::PaintNow() {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd_, &ps);
  RECT rc;
  GetClientRect(hwnd_, &rc);
  int W = rc.right, H = rc.bottom;
  if (W > 0 && H > 0) {
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, W, H);
    HGDIOBJ old = SelectObject(mem, bmp);
    {
      Graphics g(mem);
      g.SetSmoothingMode(SmoothingModeAntiAlias);
      g.SetPixelOffsetMode(PixelOffsetModeHalf);
      g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
      g.Clear(Color(kBg));
      Render(g, true);
    }
    BitBlt(dc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
  }
  EndPaint(hwnd_, &ps);
  if (animating_) SetTimer(hwnd_, kAnimTimer, 15, nullptr);
}

LRESULT CALLBACK AppWindow::Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == WM_NCCREATE) {
    auto self = (AppWindow*)((CREATESTRUCTW*)l)->lpCreateParams;
    self->hwnd_ = h;
    SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)self);
  }
  auto self = (AppWindow*)GetWindowLongPtrW(h, GWLP_USERDATA);
  return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
}

LRESULT AppWindow::Handle(UINT m, WPARAM w, LPARAM l) {
  switch (m) {
    case WM_PAINT:
      PaintNow();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_CLOSE:  // the tray icon keeps WinDrop running
      Hide();
      return 0;
    case WM_ACTIVATE:
      if (LOWORD(w) == WA_INACTIVE) deactivatedAt_ = GetTickCount64();
      break;
    case WM_EXITSIZEMOVE:
      anchoredBottom_ = false;  // moved by the user: stays where they put it
      return 0;
    case WM_DPICHANGED: {
      auto r = (RECT*)l;
      SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      Refresh();
      return 0;
    }
    case WM_TIMER:
      if (w == kTickTimer) {
        Refresh();
      } else if (w == kAnimTimer) {
        if (!animating_) KillTimer(hwnd_, kAnimTimer);
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_MOUSEMOVE: {
      if (!tracking_) {
        TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
        tracking_ = TrackMouseEvent(&t);
      }
      const Hit* hit = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      int k = hit ? hit->kind : 0;
      uint64_t n = hit ? hit->num : 0;
      std::string id = hit ? hit->id : std::string();
      if (k != hoverKind_ || n != hoverNum_ || id != hoverId_) {
        hoverKind_ = k;
        hoverNum_ = n;
        hoverId_ = id;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      tracking_ = false;
      if (hoverKind_) {
        hoverKind_ = 0;
        hoverNum_ = 0;
        hoverId_.clear();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_SETCURSOR:
      if (LOWORD(l) == HTCLIENT && hoverKind_) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    case WM_LBUTTONDOWN: {
      const Hit* hit = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      pressKind_ = hit ? hit->kind : 0;
      pressNum_ = hit ? hit->num : 0;
      pressId_ = hit ? hit->id : std::string();
      SetCapture(hwnd_);
      return 0;
    }
    case WM_LBUTTONUP: {
      ReleaseCapture();
      const Hit* hit = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      if (hit && pressKind_ && hit->kind == pressKind_ && hit->num == pressNum_ && hit->id == pressId_) {
        Hit copy = *hit;  // Click re-renders, which rebuilds hits_
        pressKind_ = 0;
        Click(copy);
      }
      pressKind_ = 0;
      return 0;
    }
    case WM_MOUSEWHEEL: {
      float maxScroll = max(0.f, contentH_ - clientH_ / s_);
      scroll_ = max(0.f, min(maxScroll, scroll_ - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 48));
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    }
    case WM_KEYDOWN:
      if (w == VK_ESCAPE) {
        if (page_ != Page::Home) {
          page_ = Page::Home;
          scroll_ = 0;
          Refresh();
        } else {
          Hide();
        }
        return 0;
      }
      break;
  }
  return DefWindowProcW(hwnd_, m, w, l);
}

}  // namespace td
