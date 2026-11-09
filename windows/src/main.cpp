// WinDrop for Windows: tray app + top-edge drop pill. Talks to the WinDrop Android app over a
// Wi-Fi Direct link (TCP 53318 / UDP 53319), woken over Bluetooth LE.
#include "common.h"

#include <algorithm>
using std::max;
using std::min;
#include <gdiplus.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#include <atomic>
#include <memory>
#include <thread>

#include "Crypto.h"
#include "DropTarget.h"
#include "NetworkEngine.h"
#include "Settings.h"
#include "TopOverlayWindow.h"
#include "Diagnostics.h"
#include "Radios.h"
#include "Updater.h"
#include "WifiDirectHost.h"
#include "Nearby.h"
#include "AppWindow.h"

using namespace td;

namespace {

constexpr wchar_t kMainClass[] = L"WinDropMainWnd";
constexpr ULONG_PTR kCopyDataSend = 0x54445250;  // 'TDRP'
constexpr int kHotkeySendClipboard = 1;
constexpr int kIconOn = 1, kIconOff = 2;  // res/app.rc

enum : UINT {
  IDM_STATUS = 100,
  IDM_ON,
  IDM_PILL,
  IDM_PAIR,
  IDM_SENDCLIP,
  IDM_OPENDIR,
  IDM_STARTUP,
  IDM_EXIT,
  IDM_QUEUE_CLEAR,
  IDM_UPDATE,
  IDM_SAVELOGS,
  IDM_CLEARLOGS,
  IDM_PAIR_STOP,
  IDM_VISIBLE,
  IDM_OPEN,  // the WinDrop window
};

constexpr UINT WM_APP_UPDATE = WM_APP + 40;         // wParam: 1 = asked by the user; lParam: UpdateInfo* (null = check failed)
constexpr UINT WM_APP_UPDATE_FAILED = WM_APP + 41;  // lParam: std::wstring* error
constexpr UINT WM_APP_LOGS = WM_APP + 42;         // lParam: std::wstring* saved path (null = failed)
constexpr UINT WM_APP_UNREACHABLE = WM_APP + 43;  // a phone got the link but couldn't connect to us
constexpr UINT WM_APP_RADIO = WM_APP + 44;        // Wi-Fi or Bluetooth switched on or off
constexpr UINT_PTR kTimerUpdate = 7;
constexpr UINT_PTR kTimerPairEnd = 8;    // the pairing window just closed
constexpr UINT_PTR kTimerPairCheck = 9;  // pairing just opened: did its beacon start?
constexpr UINT kUpdateEveryMs = 6 * 60 * 60 * 1000;

HINSTANCE gInst;
HWND gMain;
NOTIFYICONDATAW gNid{};
UINT gTaskbarCreated;
HICON gIconOn, gIconOff;
NetworkEngine gNet;
TopOverlay gOverlay;

// "Phone: A" / "Phones: A, B" for the tray tooltip and menu.
std::wstring PhonesLine() {
  auto phones = Phones();
  if (phones.empty()) return L"No phone paired yet";
  std::wstring s = phones.size() == 1 ? L"Phone: " : L"Phones: ";
  for (size_t i = 0; i < phones.size(); ++i) s += (i ? L", " : L"") + (phones[i].name.empty() ? L"phone" : phones[i].name);
  return s;
}

HICON LoadTrayIcon(int id) {
  return (HICON)LoadImageW(gInst, MAKEINTRESOURCEW(id), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
}

std::wstring ExePath() {
  wchar_t p[MAX_PATH];
  GetModuleFileNameW(nullptr, p, MAX_PATH);
  return p;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR p = nullptr;
  std::wstring r;
  if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
  CoTaskMemFree(p);
  return r;
}

void SaveShortcut(const std::wstring& link, const wchar_t* args, const wchar_t* desc) {
  IShellLinkW* sl = nullptr;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl))) return;
  std::wstring exe = ExePath();
  sl->SetPath(exe.c_str());
  sl->SetArguments(args);
  sl->SetWorkingDirectory(exe.substr(0, exe.find_last_of(L'\\')).c_str());
  sl->SetDescription(desc);
  sl->SetIconLocation(exe.c_str(), 0);
  IPersistFile* pf = nullptr;
  if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf))) {
    pf->Save(link.c_str(), TRUE);
    pf->Release();
  }
  sl->Release();
}

// Explorer right-click > Send to > Phone (WinDrop). Rewritten on every start, so it follows the exe
// if it moves. The Start menu entry belongs to the installer.
void EnsureShortcuts() {
  std::wstring sendTo = KnownFolder(FOLDERID_SendTo);
  DeleteFileW((sendTo + L"\\Phone (TopDrop).lnk").c_str());  // from before the rename
  SaveShortcut(sendTo + L"\\Phone (WinDrop).lnk", L"--send", L"Send to phone with WinDrop");
  DeleteFileW((KnownFolder(FOLDERID_Programs) + L"\\WinDrop.lnk").c_str());  // made by pre-installer builds
}

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"WinDrop";

// Where Settings > Apps > Startup and Task Manager keep their on/off switch for each Run entry.
// First byte: 2 = on, 3 = off (switched off there).
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";

bool StartupEnabled() {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
  bool on = RegQueryValueExW(k, kRunValue, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
  RegCloseKey(k);
  BYTE flags[12];
  DWORD n = sizeof flags;
  if (on && RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kRunValue, RRF_RT_REG_BINARY, nullptr, flags, &n) ==
                ERROR_SUCCESS)
    on = n > 0 && !(flags[0] & 1);
  return on;
}

// Same entry Windows lists under Startup apps; turning it on here also clears an "off" set there.
void SetStartup(bool on) {
  HKEY k;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
  RegDeleteValueW(k, L"TopDrop");  // from before the rename
  if (on) {
    std::wstring v = L"\"" + ExePath() + L"\" --tray";
    RegSetValueExW(k, kRunValue, 0, REG_SZ, (const BYTE*)v.c_str(), DWORD((v.size() + 1) * sizeof(wchar_t)));
  } else {
    RegDeleteValueW(k, kRunValue);
  }
  RegCloseKey(k);
  HKEY a;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kApprovedKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &a, nullptr) ==
      ERROR_SUCCESS) {
    if (on) {
      BYTE flags[12] = {2};
      RegSetValueExW(a, kRunValue, 0, REG_BINARY, flags, sizeof flags);
    } else {
      RegDeleteValueW(a, kRunValue);
    }
    RegCloseKey(a);
  }
  Log("ui: start with Windows %s", on ? "on" : "off");
}

// If startup is on but points at an old copy of the exe (folder moved), follow this one.
void RefreshStartupPath() {
  if (StartupEnabled()) SetStartup(true);
}

// ---- queued (not yet sent) items, listed in the window ----
std::wstring QueueLabel(const OutboxItem& it) {
  std::wstring s;
  if (it.isText) {
    s = L"Text: " + U2W(it.text);
    for (auto& c : s)
      if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
  } else {
    s = FileNameOf(it.path);
  }
  if (s.size() > 80) s = s.substr(0, 79) + L"\u2026";
  return s;
}

void SetClipboardText(const std::wstring& t) {
  if (!OpenClipboard(gMain)) return;
  EmptyClipboard();
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t));
  if (h) {
    memcpy(GlobalLock(h), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
    GlobalUnlock(h);
    SetClipboardData(CF_UNICODETEXT, h);
  }
  CloseClipboard();
}

// An image the phone sent from its clipboard: make it pasteable here too (bitmap + PNG + file).
void SetClipboardImage(const std::wstring& path) {
  Gdiplus::Bitmap bmp(path.c_str());
  HBITMAP hb = nullptr;
  if (bmp.GetLastStatus() != Gdiplus::Ok || bmp.GetHBITMAP(Gdiplus::Color(255, 255, 255), &hb) != Gdiplus::Ok) return;
  if (!OpenClipboard(gMain)) { DeleteObject(hb); return; }
  EmptyClipboard();
  SetClipboardData(CF_BITMAP, hb);  // the clipboard owns it now
  HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    DWORD n = GetFileSize(f, nullptr), got = 0;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n)) {
      if (ReadFile(f, GlobalLock(g), n, &got, nullptr) && got == n) {
        GlobalUnlock(g);
        SetClipboardData(RegisterClipboardFormatW(L"PNG"), g);
      } else {
        GlobalUnlock(g);
        GlobalFree(g);
      }
    }
    CloseHandle(f);
  }
  CloseClipboard();
  Log("clipboard: image from phone copied");
}

bool IsClipboardImage(const std::wstring& name) {
  if (name.rfind(L"Clipboard ", 0) != 0) return false;
  auto dot = name.find_last_of(L'.');
  std::wstring ext = dot == std::wstring::npos ? L"" : name.substr(dot);
  for (auto& c : ext) c = towlower(c);
  return ext == L".png" || ext == L".jpg" || ext == L".jpeg" || ext == L".webp" || ext == L".bmp" || ext == L".gif";
}

void SendClipboard() {
  if (!Cfg().enabled) return;
  IDataObject* data = nullptr;
  if (OleGetClipboard(&data) != S_OK || !data) return;
  std::vector<std::wstring> files;
  std::wstring text;
  if (DropTarget::Extract(data, &files, &text)) {
    Log("ui: send clipboard (%zu files)", files.size());
    gOverlay.Send(files, text);  // the pill says where it goes, or asks
  } else {
    gOverlay.Notify(L"Clipboard is empty", L"Copy some text or files first", true);
  }
  data->Release();
}

// ---- tray ----
void UpdateTray() {
  bool on = Cfg().enabled;
  std::wstring off = TheRadios().OffText();
  std::wstring tip = !on ? L"WinDrop — off" : L"WinDrop — " + PhonesLine() + (off.empty() ? L"" : L" · " + off);
  wcsncpy_s(gNid.szTip, tip.c_str(), _TRUNCATE);
  TheApp().Refresh();
  gNid.hIcon = on ? gIconOn : gIconOff;
  gNid.uFlags = NIF_TIP | NIF_ICON | NIF_SHOWTIP;  // v4 icons show no tooltip without NIF_SHOWTIP
  Shell_NotifyIconW(NIM_MODIFY, &gNid);
}

void AddTray() {
  gNid.cbSize = sizeof gNid;
  gNid.hWnd = gMain;
  gNid.uID = 1;
  gNid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
  gNid.uCallbackMessage = WM_APP_TRAY;
  gNid.hIcon = Cfg().enabled ? gIconOn : gIconOff;
  wcsncpy_s(gNid.szTip, L"WinDrop", _TRUNCATE);
  Shell_NotifyIconW(NIM_ADD, &gNid);
  gNid.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &gNid);
  UpdateTray();
}

// The master switch: pill, Wi-Fi Direct advertising, BLE wake-up and incoming transfers.
void SetOn(bool on) {
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    Cfg().enabled = on;
    Cfg().Save();
  }
  Log("ui: WinDrop %s", on ? "on" : "off");
  gNet.SetEnabled(on);
  gOverlay.SetEnabled(on);
  if (on) {
    TheWifiDirect().Start();
    StartNearbyWatch();  // phones that let nearby PCs send to them
  } else {
    TheWifiDirect().SetBeacon(WifiDirectHost::kBeaconOff);
    TheWifiDirect().SetVisible(false);
    TheWifiDirect().SetNearbyBeacon({});
    TheWifiDirect().Stop();
    StopNearbyWatch();
  }
  UpdateTray();
}

bool gPairedInWindow = false;  // a phone was paired since "Pair phone\u2026" was last opened

// Opens pairing for 3 minutes (WifiDirectHost::kPairWindowMs); the phone finds us by itself, and
// the user confirms a 6-digit number that both screens show.
void ShowPairCode() {
  if (!Cfg().enabled) SetOn(true);  // off, nothing would answer the phone
  TheWifiDirect().OpenPairWindow();
  gPairedInWindow = false;
  SetTimer(gMain, kTimerPairEnd, UINT(WifiDirectHost::kPairWindowMs + 500), nullptr);
  SetTimer(gMain, kTimerPairCheck, 2000, nullptr);  // the beacon starts on the next tick
  Log("ui: pairing window opened");
  // The phone finds this PC over Bluetooth and links over Wi-Fi: say so now, not after it fails.
  std::wstring off = TheRadios().OffText();
  if (!off.empty())
    gOverlay.Notify(off + L": turn it on to pair", L"Pairing is open for 3 minutes and works once it's on", true);
  else if (TheWifiDirect().Unsupported())
    gOverlay.Notify(L"This PC's Wi-Fi can't do Wi-Fi Direct", L"WinDrop needs it: update the Wi-Fi driver, then try again", true);
  else
    gOverlay.Notify(L"Pairing is open for 3 minutes", L"On your phone: WinDrop \u2192 Pair with PC");
}

// "2 min left" / "40 s left" on the pairing window, for the tray menu; empty when it's closed.
std::wstring PairingLeft() {
  ULONGLONG left = TheWifiDirect().PairingLeftMs();
  if (!left) return L"";
  return left > 60000 ? std::to_wstring((left + 59999) / 60000) + L" min left" : std::to_wstring((left + 999) / 1000) + L" s left";
}

// A phone asking to pair: the window shows its number until it's answered or the request ends.
struct PairAsk {
  uint64_t id = 0;
  std::wstring name, sas;
} gPairAsk;

void AnswerPair(uint64_t id, bool yes) {
  gNet.Decide(id, yes);  // ignored if that request is already over
  if (gPairAsk.id == id) gPairAsk = {};
  TheApp().SetAlwaysOnTop(false);
}

// ---- updates ----
UpdateInfo gUpdate;            // newest release found; version empty if none
std::atomic<bool> gUpdateBusy{false};
std::atomic<bool> gInstalling{false};
std::wstring gUpdateNote;      // the last check's result, for the window: "up to date"
std::string gUpdateAnnounced;  // version the pill already announced

void StartUpdateCheck(bool manual) {
  if (gUpdateBusy.exchange(true)) return;
  std::thread([manual] {
    auto* info = new UpdateInfo;
    if (!CheckForUpdate(info)) { delete info; info = nullptr; }
    gUpdateBusy = false;
    PostMessageW(gMain, WM_APP_UPDATE, manual, (LPARAM)info);
  }).detach();
}

void InstallNow() {
  if (gUpdate.version.empty() || gUpdateBusy.exchange(true)) return;
  gInstalling = true;
  gOverlay.Notify(L"Downloading WinDrop " + U2W(gUpdate.version) + L"…", L"It restarts by itself when it's done");
  UpdateInfo info = gUpdate;
  std::thread([info] {
    std::wstring err;
    bool ok = InstallUpdate(info, [](int) {}, &err);
    gInstalling = false;
    gUpdateBusy = false;
    if (ok) PostMessageW(gMain, WM_CLOSE, 0, 0);  // the installer replaces and restarts us
    else PostMessageW(gMain, WM_APP_UPDATE_FAILED, 0, (LPARAM) new std::wstring(err));
  }).detach();
}

void OnUpdateChecked(bool manual, UpdateInfo* found) {
  std::unique_ptr<UpdateInfo> info(found);
  if (!info) {
    if (manual) gUpdateNote = L"couldn't check (offline?)";
    TheApp().Refresh();
    return;
  }
  if (info->version.empty()) {
    gUpdateNote = L"up to date";
    TheApp().Refresh();
    return;
  }
  gUpdate = *info;
  if (manual) {
    TheApp().Show();  // its "Update to ..." button is the question
  } else if (gUpdateAnnounced != gUpdate.version) {
    gUpdateAnnounced = gUpdate.version;
    gOverlay.Notify(L"WinDrop " + U2W(gUpdate.version) + L" is available", L"Tray icon \u2192 Settings \u2192 Update");
  }
  TheApp().Refresh();
}

// Everything the window shows, read fresh on each refresh.
AppView BuildView() {
  AppView v;
  v.on = Cfg().enabled;
  v.pillShown = Cfg().pillShown;
  v.startup = StartupEnabled();
  auto phones = Phones();
  std::string linked = TheWifiDirect().LinkedPhone();
  std::wstring off = TheRadios().OffText();
  if (!v.on) {
    v.status = L"Off";
    v.statusColor = 0xFF6B6D72;
  } else if (!off.empty()) {
    v.status = L"On \u00b7 " + off;
    v.statusColor = 0xFFF5A524;
  } else if (!linked.empty()) {
    v.status = L"Connected to " + PhoneLabel(linked);
    v.statusColor = 0xFF2BB673;
  } else if (phones.empty()) {
    v.status = L"On \u00b7 no phone paired";
    v.statusColor = 0xFFF5A524;
  } else {
    v.status = L"On \u00b7 ready";
    v.statusColor = 0xFF2BB673;
  }
  if (v.on && !TheRadios().WifiOn())
    v.warning = off + L": WinDrop can't send or receive until Wi-Fi is back on.";
  else if (v.on && !off.empty())
    v.warning = L"Bluetooth is off: pairing and sending to your phone need it. The phone can still send here.";
  else if (v.on && TheWifiDirect().Unsupported())
    v.warning = L"This PC's Wi-Fi can't do Wi-Fi Direct, which WinDrop needs. A Wi-Fi driver update may fix it.";
  v.visibleLeftMs = TheWifiDirect().VisibleLeftMs();
  v.visibleTotalMs = WifiDirectHost::kVisibleMs;
  v.pairingLeftMs = TheWifiDirect().PairingLeftMs();
  v.pairingTotalMs = WifiDirectHost::kPairWindowMs;

  auto queue = TheOutbox().Snapshot();
  for (auto& p : phones) {
    AppView::Phone ph;
    ph.id = p.id;
    ph.name = p.name.empty() ? L"Phone" : p.name;
    ph.linked = p.id == linked;
    size_t n = std::count_if(queue.begin(), queue.end(), [&](const OutboxItem& o) { return o.phoneId == p.id; });
    ph.sub = ph.linked ? L"Connected" : L"Paired";
    if (n) ph.sub += L" \u00b7 " + std::to_wstring(n) + L" waiting";
    v.phones.push_back(ph);
  }
  v.maxPhones = kMaxPhones;
  // Grouped by phone, in pairing order.
  auto rank = [&](const std::string& id) {
    for (size_t i = 0; i < phones.size(); ++i)
      if (phones[i].id == id) return i;
    return phones.size();
  };
  std::stable_sort(queue.begin(), queue.end(), [&](const OutboxItem& a, const OutboxItem& b) { return rank(a.phoneId) < rank(b.phoneId); });
  for (auto& it : queue) {
    AppView::Item item;
    item.id = it.id;
    item.label = QueueLabel(it);
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (it.isText) item.sub = L"Text";
    else if (GetFileAttributesExW(it.path.c_str(), GetFileExInfoStandard, &fa))
      item.sub = HumanSize((uint64_t(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow);
    else item.sub = L"File";
    if (phones.size() > 1) item.sub += L" \u00b7 for " + PhoneLabel(it.phoneId);
    v.queue.push_back(item);
  }

  std::wstring ver = U2W(WINDROP_VERSION);
  v.updateReady = !gUpdate.version.empty();
  v.updateBusy = gUpdateBusy;
  v.version = L"Version " + ver + (v.updateReady ? L" \u00b7 " + U2W(gUpdate.version) + L" is out"
                                                 : gUpdateNote.empty() ? L"" : L" \u00b7 " + gUpdateNote);
  v.updateLabel = gInstalling   ? L"Downloading\u2026"
                  : gUpdateBusy ? L"Checking\u2026"
                  : v.updateReady ? L"Update to " + U2W(gUpdate.version)
                                  : L"Check for updates";
  v.pairAskId = gPairAsk.id;
  v.pairAskName = gPairAsk.name;
  v.pairAskSas = gPairAsk.sas;
  return v;
}

// Right-click on the tray icon: just the everyday switches. The rest is in the window.
void ShowMenu() {
  bool on = Cfg().enabled;
  HMENU m = CreatePopupMenu();
  AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, BuildView().status.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | MF_DEFAULT, IDM_OPEN, L"Open WinDrop");
  AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_ON, L"On");
  AppendMenuW(m, MF_STRING | (on ? 0 : MF_GRAYED), IDM_SENDCLIP, L"Send clipboard\tCtrl+Alt+P");
  ULONGLONG visLeft = TheWifiDirect().VisibleLeftMs();
  std::wstring visLabel = visLeft ? L"Nearby phones can send (" + std::to_wstring((visLeft + 59999) / 60000) + L" min left)"
                                  : L"Let nearby phones send (10 min)";
  AppendMenuW(m, MF_STRING | (visLeft ? MF_CHECKED : 0) | (on ? 0 : MF_GRAYED), IDM_VISIBLE, visLabel.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, IDM_EXIT, L"Quit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(gMain);
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, gMain, nullptr);
  PostMessageW(gMain, WM_NULL, 0, 0);
  DestroyMenu(m);
}

// The window asked already ("Remove?" / "Cancel"), so these just do it.
void RemovePhone(const std::string& id) {
  std::wstring name = PhoneLabel(id);
  std::vector<uint64_t> drop;
  for (auto& it : TheOutbox().Snapshot())
    if (it.phoneId == id) drop.push_back(it.id);
  gNet.CancelOutbox(drop);
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
    Cfg().RemovePhone(id);
    Cfg().Save();
  }
  if (TheWifiDirect().LinkedPhone() == id) TheWifiDirect().Disconnect();
  Log("ui: removed phone %s", W2U(name).c_str());
  UpdateTray();
  gOverlay.Notify(L"Removed " + name, L"Pair it again from the WinDrop window to use it");
}

void CancelItem(uint64_t id) {
  for (auto& it : TheOutbox().Snapshot()) {
    if (it.id != id) continue;
    gNet.CancelOutbox({id});
    Log("ui: removed from queue: %s", it.isText ? "text" : W2U(FileNameOf(it.path)).c_str());
    gOverlay.Notify(L"Cancelled", QueueLabel(it) + L" won't be sent");
    return;
  }
}

void OnCommand(UINT id) {
  switch (id) {
    case IDM_ON:
      SetOn(!Cfg().enabled);
      break;
    case IDM_PILL: {
      bool show = !Cfg().pillShown;
      {
        std::lock_guard<std::recursive_mutex> lk(CfgMutex());
        Cfg().pillShown = show;
        Cfg().Save();
      }
      Log("ui: pill %s", show ? "shown" : "hidden");
      gOverlay.SetPillShown(show);
      break;
    }
    case IDM_SENDCLIP:
      SendClipboard();
      break;
    case IDM_PAIR:
      ShowPairCode();
      break;
    case IDM_VISIBLE: {
      bool on = !TheWifiDirect().Visible();
      TheWifiDirect().SetVisible(on);
      Log("ui: visible to nearby phones %s", on ? "on" : "off");
      if (on) gOverlay.Notify(L"Nearby phones can send for 10 minutes", L"On the phone: share or drop onto the pill, pick this PC");
      break;
    }
    case IDM_PAIR_STOP:
      KillTimer(gMain, kTimerPairEnd);
      Log("ui: pairing stopped");
      TheWifiDirect().ClosePairWindow();
      break;
    case IDM_SAVELOGS:
      gOverlay.Notify(L"Saving logs…", L"Checking the firewall and network too; takes a few seconds");
      std::thread([] {
        std::wstring path;
        bool ok = SaveLogs(&path);
        PostMessageW(gMain, WM_APP_LOGS, 0, ok ? (LPARAM) new std::wstring(path) : 0);
      }).detach();
      break;
    case IDM_CLEARLOGS: {
      std::wstring dir;
      {
        std::lock_guard<std::recursive_mutex> lk(CfgMutex());
        dir = Cfg().dataDir;
      }
      DeleteFileW((dir + L"\\windrop.log").c_str());
      DeleteFileW((dir + L"\\windrop.log.1").c_str());
      Log("Logs cleared. WinDrop %s, phones paired: %zu, Wi-Fi Direct: %s", WINDROP_VERSION, Phones().size(),
          W2U(TheWifiDirect().Status()).c_str());
      gOverlay.Notify(L"Logs cleared", L"Do the thing that fails, then Logs → Save logs to Downloads");
      break;
    }
    case IDM_OPENDIR:
      CreateDirectoryW(Cfg().downloadDir.c_str(), nullptr);
      ShellExecuteW(nullptr, L"open", Cfg().downloadDir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
      break;
    case IDM_STARTUP:
      SetStartup(!StartupEnabled());
      break;
    case IDM_EXIT:
      DestroyWindow(gMain);
      break;
    case IDM_UPDATE:
      if (gUpdate.version.empty()) StartUpdateCheck(true);
      else TheApp().Show();
      break;
    case IDM_OPEN:
      TheApp().Show();
      break;
    case IDM_QUEUE_CLEAR: {
      std::vector<uint64_t> ids;
      for (auto& it : TheOutbox().Snapshot()) ids.push_back(it.id);
      gNet.CancelOutbox(ids);
      Log("ui: queue cleared (%zu)", ids.size());
      gOverlay.Notify(L"Cancelled " + std::to_wstring(ids.size()) + (ids.size() == 1 ? L" item" : L" items"), L"Won't be sent");
      break;
    }
    default:
      break;
  }
  TheApp().Refresh();
}

void AppAction(AppAct a, uint64_t num, const std::string& id) {
  switch (a) {
    case AppAct::On: OnCommand(IDM_ON); break;
    case AppAct::Pill: OnCommand(IDM_PILL); break;
    case AppAct::Startup: OnCommand(IDM_STARTUP); break;
    case AppAct::Visible: OnCommand(IDM_VISIBLE); break;
    case AppAct::SendClip: OnCommand(IDM_SENDCLIP); break;
    case AppAct::OpenDir: OnCommand(IDM_OPENDIR); break;
    case AppAct::Pair: OnCommand(IDM_PAIR); break;
    case AppAct::PairStop: OnCommand(IDM_PAIR_STOP); break;
    case AppAct::PairYes:
    case AppAct::PairNo: AnswerPair(num, a == AppAct::PairYes); break;
    case AppAct::RemovePhone: RemovePhone(id); break;
    case AppAct::CancelItem: CancelItem(num); break;
    case AppAct::CancelAll: OnCommand(IDM_QUEUE_CLEAR); break;
    case AppAct::Update:
      if (gUpdate.version.empty()) {
        gUpdateNote.clear();
        StartUpdateCheck(true);
      } else {
        InstallNow();
      }
      break;
    case AppAct::SaveLogs: OnCommand(IDM_SAVELOGS); break;
    case AppAct::ClearLogs: OnCommand(IDM_CLEARLOGS); break;
    case AppAct::Quit: OnCommand(IDM_EXIT); break;
  }
}

std::vector<std::wstring> ParseSendArgs(const wchar_t* cmdLine) {
  std::vector<std::wstring> files;
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
  bool send = false;
  for (int i = 1; i < argc; ++i) {
    if (!wcscmp(argv[i], L"--send")) send = true;
    else if (send) files.push_back(argv[i]);
  }
  LocalFree(argv);
  return files;
}

LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  if (m == gTaskbarCreated && m) {
    AddTray();
    return 0;
  }
  switch (m) {
    case WM_APP_TRAY:
      switch (LOWORD(l)) {
        case NIN_SELECT:
        case NIN_KEYSELECT:
          TheApp().Toggle();
          break;
        case WM_CONTEXTMENU:
          ShowMenu();
          break;
      }
      return 0;
    case WM_APP_NET: {
      std::unique_ptr<UiEvent> e((UiEvent*)l);
      if (e->type == NetEv::TextReceived) SetClipboardText(U2W(e->text));
      if (e->type == NetEv::RecvDone && IsClipboardImage(e->name)) SetClipboardImage(e->path);
      if (e->type == NetEv::PairRequest) {
        // A newer request (the phone asked again) replaces the one on screen.
        if (gPairAsk.id && gPairAsk.id != e->id) gNet.Decide(gPairAsk.id, false);
        gPairAsk = {e->id, e->name, U2W(e->text)};
        TheApp().Show();
        TheApp().SetAlwaysOnTop(true);
        TheApp().Refresh();
        return 0;
      }
      if (e->type == NetEv::PairClosed) {
        if (e->id == gPairAsk.id) {
          gPairAsk = {};
          TheApp().SetAlwaysOnTop(false);
          TheApp().Refresh();
        }
        return 0;
      }
      if (e->type == NetEv::PairDone) {
        if (e->count) {
          gPairedInWindow = true;
          KillTimer(gMain, kTimerPairEnd);  // pairing ended with this phone (ClosePairWindow)
        }
        UpdateTray();
        if (e->count) gOverlay.Notify(L"Paired with " + e->name, L"Transfers are now encrypted");
        else gOverlay.Notify(L"Can't pair " + e->name, L"4 phones are paired already: remove one in the WinDrop window", true);
        TheApp().Refresh();
        return 0;
      }
      if (e->type == NetEv::Incompatible) {
        if ((int)e->count < kProto) {
          gOverlay.Notify(L"The phone's WinDrop is out of date",
                          L"It has v" + e->name + L": update it in the WinDrop app on the phone", true);
        } else {
          gOverlay.Notify(L"This PC's WinDrop is out of date", L"The phone has v" + e->name + L": checking for an update", true);
          StartUpdateCheck(true);
        }
        return 0;
      }
      if (e->type == NetEv::PeerSeen) UpdateTray();
      gOverlay.OnNetEvent(*e);
      TheApp().Refresh();
      return 0;
    }
    case WM_COMMAND:
      OnCommand(LOWORD(w));
      return 0;
    case WM_APP_LOGS: {
      std::unique_ptr<std::wstring> path((std::wstring*)l);
      if (!path) {
        gOverlay.Notify(L"Couldn't save the logs", L"", true);
      } else {
        gOverlay.Notify(L"Logs saved to Downloads\\WinDrop", L"Send that file to whoever is helping you");
        std::wstring args = L"/select,\"" + *path + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
      }
      return 0;
    }
    case WM_APP_UNREACHABLE:
      gOverlay.Notify(L"Your phone couldn't reach this PC",
                      L"A firewall or antivirus is blocking WinDrop: allow it for Public networks", true);
      return 0;
    case WM_APP_UPDATE:
      OnUpdateChecked(w != 0, (UpdateInfo*)l);
      return 0;
    case WM_APP_UPDATE_FAILED: {
      std::unique_ptr<std::wstring> err((std::wstring*)l);
      if (*err != L"cancelled") gOverlay.Notify(L"Update failed", *err, true);
      return 0;
    }
    case WM_TIMER:
      if (w == kTimerUpdate) {
        SetTimer(gMain, kTimerUpdate, kUpdateEveryMs, nullptr);
        StartUpdateCheck(false);
      } else if (w == kTimerPairCheck) {
        KillTimer(gMain, kTimerPairCheck);
        if (TheWifiDirect().Pairing() && TheRadios().BluetoothOn() && TheWifiDirect().BeaconBroken())
          gOverlay.Notify(L"This PC's Bluetooth can't broadcast", L"Your phone can't find it to pair: update the Bluetooth driver", true);
      } else if (w == kTimerPairEnd) {
        KillTimer(gMain, kTimerPairEnd);
        if (!gPairedInWindow && !TheWifiDirect().Pairing()) {
          Log("ui: pairing window closed, no phone paired");
          gOverlay.Notify(L"Pairing closed", L"No phone was paired. Pair phone in the WinDrop window to try again", true);
        }
      }
      return 0;
    case WM_APP_RADIO: {
      // Advertising and beacons follow the switches; say so once when one WinDrop needs goes off.
      static bool wifiWas = true, btWas = true;  // both counted as on until the first reading
      bool wifi = TheRadios().WifiOn(), bt = TheRadios().BluetoothOn();
      if (wifi != wifiWas) TheWifiDirect().Refresh();  // stop advertising, or start again
      UpdateTray();
      bool matters = Cfg().enabled && (!Phones().empty() || TheWifiDirect().Pairing());
      if (matters && wifiWas && !wifi)
        gOverlay.Notify(TheRadios().OffText(), L"WinDrop can't send or receive until Wi-Fi is back on", true);
      else if (matters && btWas && !bt)
        gOverlay.Notify(L"Bluetooth is off", L"Pairing and sending to your phone need it; the phone can still send here", true);
      wifiWas = wifi;
      btWas = bt;
      TheApp().Refresh();
      return 0;
    }
    case WM_HOTKEY:
      if (w == kHotkeySendClipboard) SendClipboard();
      return 0;
    case WM_COPYDATA: {
      auto cds = (COPYDATASTRUCT*)l;
      if (cds->dwData != kCopyDataSend) return FALSE;
      std::vector<std::wstring> files;
      auto p = (const wchar_t*)cds->lpData;
      size_t n = cds->cbData / sizeof(wchar_t);
      std::wstring cur;
      for (size_t i = 0; i < n; ++i) {
        if (p[i]) cur += p[i];
        else if (!cur.empty()) { files.push_back(cur); cur.clear(); }
      }
      if (!files.empty() && Cfg().enabled) gOverlay.Dropped(files, L"");
      return TRUE;
    }
    case WM_POWERBROADCAST:
      if (w == PBT_APMRESUMEAUTOMATIC) {
        Log("power: resumed");
      }
      return TRUE;
    case WM_DESTROY:
      Shell_NotifyIconW(NIM_DELETE, &gNid);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
  gInst = inst;
  std::vector<std::wstring> sendNow = ParseSendArgs(GetCommandLineW());

  HANDLE mutex = CreateMutexW(nullptr, TRUE, L"WinDrop.SingleInstance");
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    // Hand files to the running instance (Explorer "Send to" launches us once per selection).
    // `WinDrop.exe --pair` opens the running instance's "Pair phone…" window.
    HWND other = FindWindowW(kMainClass, nullptr);
    DWORD pid = 0;
    if (other && GetWindowThreadProcessId(other, &pid)) AllowSetForegroundWindow(pid);  // its window may come forward
    if (other && wcsstr(GetCommandLineW(), L"--pair")) {
      PostMessageW(other, WM_COMMAND, IDM_PAIR, 0);
    } else if (other && sendNow.empty()) {
      PostMessageW(other, WM_COMMAND, IDM_OPEN, 0);  // started again (Start menu): show the window
    } else if (other && !sendNow.empty()) {
      std::wstring blob;
      for (auto& f : sendNow) { blob += f; blob += L'\0'; }
      COPYDATASTRUCT cds{kCopyDataSend, DWORD(blob.size() * sizeof(wchar_t)), (void*)blob.data()};
      SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
    }
    return 0;
  }

  OleInitialize(nullptr);
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
  Gdiplus::GdiplusStartupInput gsi;
  ULONG_PTR gdiToken;
  Gdiplus::GdiplusStartup(&gdiToken, &gsi, nullptr);

  Cfg().Load();
  {
    std::lock_guard<std::recursive_mutex> lk(CfgMutex());
  }
  gIconOn = LoadTrayIcon(kIconOn);
  gIconOff = LoadTrayIcon(kIconOff);

  WNDCLASSEXW wc{sizeof wc};
  wc.lpfnWndProc = MainProc;
  wc.hInstance = inst;
  wc.lpszClassName = kMainClass;
  wc.hIcon = gIconOn;
  RegisterClassExW(&wc);
  gMain = CreateWindowExW(0, kMainClass, L"WinDrop", 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
  gTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

  gOverlay.Create(inst, &gNet);
  TheApp().Create(inst, gIconOn, BuildView, AppAction);
  TheWifiDirect().onLinkUp = [] { gNet.OnLinkUp(); };
  TheWifiDirect().onUnreachable = [] { PostMessageW(gMain, WM_APP_UNREACHABLE, 0, 0); };
  if (!gNet.Start(gMain)) gOverlay.Notify(L"WinDrop can't listen on port 53318", L"Is another copy running?", true);
  AddTray();
  EnsureShortcuts();
  RefreshStartupPath();
  gOverlay.SetPillShown(Cfg().pillShown);
  TheRadios().Start([] { PostMessageW(gMain, WM_APP_RADIO, 0, 0); });
  SetOn(Cfg().enabled);
  RegisterHotKey(gMain, kHotkeySendClipboard, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
  if (!sendNow.empty() && Cfg().enabled) gOverlay.Dropped(sendNow, L"");
  SetTimer(gMain, kTimerUpdate, 30 * 1000, nullptr);  // first check shortly after start, then every 6 h
  // Started by hand (Start menu, installer): open the window. Sign-in (--tray) and Send to don't.
  if (sendNow.empty() && !wcsstr(GetCommandLineW(), L"--tray")) TheApp().Show();

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  TheWifiDirect().Stop();
  gNet.Stop();
  gOverlay.Destroy();
  Gdiplus::GdiplusShutdown(gdiToken);
  WSACleanup();
  OleUninitialize();
  ReleaseMutex(mutex);
  return 0;
}
