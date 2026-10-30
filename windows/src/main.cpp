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
#include "Updater.h"
#include "WifiDirectHost.h"

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
  IDM_QUEUE_ITEM = 1000,   // + index into gMenuQueue
  IDM_PHONE_REMOVE = 1100, // + index into gMenuPhones  // + index into gMenuQueue
};

constexpr UINT WM_APP_UPDATE = WM_APP + 40;         // wParam: 1 = asked by the user; lParam: UpdateInfo* (null = check failed)
constexpr UINT WM_APP_UPDATE_FAILED = WM_APP + 41;  // lParam: std::wstring* error
constexpr UINT WM_APP_LOGS = WM_APP + 42;         // lParam: std::wstring* saved path (null = failed)
constexpr UINT WM_APP_UNREACHABLE = WM_APP + 43;  // a phone got the link but couldn't connect to us
constexpr UINT_PTR kTimerUpdate = 7;
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
    std::wstring v = L"\"" + ExePath() + L"\"";
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

// ---- queued (not yet sent) items, shown in the tray menu ----
std::vector<OutboxItem> gMenuQueue;  // what the open menu listed, by index
std::vector<PairedPhone> gMenuPhones;

std::wstring QueueLabel(const OutboxItem& it) {
  std::wstring s;
  if (it.isText) {
    s = L"Text: " + U2W(it.text);
    for (auto& c : s)
      if (c == L'\r' || c == L'\n' || c == L'\t') c = L' ';
  } else {
    s = FileNameOf(it.path);
  }
  if (s.size() > 48) s = s.substr(0, 47) + L"…";
  std::wstring out;  // a lone & would underline the next letter
  for (auto c : s) {
    out += c;
    if (c == L'&') out += L'&';
  }
  return out;
}

HMENU QueueMenu(size_t* count) {
  gMenuQueue = TheOutbox().Snapshot();
  *count = gMenuQueue.size();
  HMENU q = CreatePopupMenu();
  if (gMenuQueue.empty()) {
    AppendMenuW(q, MF_STRING | MF_GRAYED, 0, L"Nothing waiting");
    return q;
  }
  AppendMenuW(q, MF_STRING | MF_GRAYED, 0, L"Sent when the phone is in reach · click one to remove it");
  // Grouped by phone (in pairing order), each under its own heading with its own count.
  auto phones = Phones();
  auto rank = [&](const std::string& id) {
    for (size_t i = 0; i < phones.size(); ++i)
      if (phones[i].id == id) return i;
    return phones.size();
  };
  std::stable_sort(gMenuQueue.begin(), gMenuQueue.end(),
                   [&](const OutboxItem& a, const OutboxItem& b) { return rank(a.phoneId) < rank(b.phoneId); });
  size_t shown = std::min<size_t>(gMenuQueue.size(), 30);
  for (size_t i = 0; i < shown; ++i) {
    const auto& it = gMenuQueue[i];
    if (i == 0 || it.phoneId != gMenuQueue[i - 1].phoneId) {
      AppendMenuW(q, MF_SEPARATOR, 0, nullptr);
      if (phones.size() > 1) {
        size_t n = std::count_if(gMenuQueue.begin(), gMenuQueue.end(), [&](const OutboxItem& o) { return o.phoneId == it.phoneId; });
        AppendMenuW(q, MF_STRING | MF_GRAYED, 0, (L"For " + PhoneLabel(it.phoneId) + L" (" + std::to_wstring(n) + L")").c_str());
      }
    }
    AppendMenuW(q, MF_STRING, IDM_QUEUE_ITEM + UINT(i), QueueLabel(it).c_str());
  }
  if (gMenuQueue.size() > shown)
    AppendMenuW(q, MF_STRING | MF_GRAYED, 0, (L"…and " + std::to_wstring(gMenuQueue.size() - shown) + L" more").c_str());
  AppendMenuW(q, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(q, MF_STRING, IDM_QUEUE_CLEAR, L"Clear queue");
  return q;
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
    if (!files.empty()) gNet.SendFiles(files);
    else gNet.SendText(W2U(text));
    gOverlay.Notify(L"Sending clipboard to " + PhoneLabel(std::string()) + L"…", files.empty() ? L"Text" : std::to_wstring(files.size()) + L" file(s)");
  } else {
    gOverlay.Notify(L"Clipboard is empty", L"Copy some text or files first", true);
  }
  data->Release();
}

// ---- tray ----
void UpdateTray() {
  bool on = Cfg().enabled;
  std::wstring tip = on ? L"WinDrop — " + PhonesLine() : L"WinDrop — off";
  wcsncpy_s(gNid.szTip, tip.c_str(), _TRUNCATE);
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
  } else {
    TheWifiDirect().SetBeacon(WifiDirectHost::kBeaconOff);
    TheWifiDirect().Stop();
  }
  UpdateTray();
}

// Opens pairing for 3 minutes; the phone finds us by itself, and the user confirms a 6-digit
// number that both screens show.
void ShowPairCode() {
  TheWifiDirect().OpenPairWindow();
  Log("ui: pairing window opened");
  gOverlay.Notify(L"Pairing is open for 3 minutes", L"On your phone: WinDrop \u2192 Pair with PC");
}

// ---- updates ----
UpdateInfo gUpdate;            // newest release found; version empty if none
std::atomic<bool> gUpdateBusy{false};
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
  gOverlay.Notify(L"Downloading WinDrop " + U2W(gUpdate.version) + L"…", L"It restarts by itself when it's done");
  UpdateInfo info = gUpdate;
  std::thread([info] {
    std::wstring err;
    bool ok = InstallUpdate(info, [](int) {}, &err);
    gUpdateBusy = false;
    if (ok) PostMessageW(gMain, WM_CLOSE, 0, 0);  // the installer replaces and restarts us
    else PostMessageW(gMain, WM_APP_UPDATE_FAILED, 0, (LPARAM) new std::wstring(err));
  }).detach();
}

void AskToUpdate() {
  std::wstring msg = L"WinDrop " + U2W(gUpdate.version) + L" is available (you have " + U2W(WINDROP_VERSION) +
                     L").\n\nUpdate now? WinDrop restarts by itself; Windows asks for permission once.";
  if (MessageBoxW(gMain, msg.c_str(), L"WinDrop update", MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES)
    InstallNow();
}

void OnUpdateChecked(bool manual, UpdateInfo* found) {
  std::unique_ptr<UpdateInfo> info(found);
  if (!info) {
    if (manual) gOverlay.Notify(L"Couldn't check for updates", L"Check the internet connection and try again", true);
    return;
  }
  if (info->version.empty()) {
    if (manual) gOverlay.Notify(L"WinDrop is up to date", L"Version " + U2W(WINDROP_VERSION));
    return;
  }
  gUpdate = *info;
  if (manual) {
    AskToUpdate();
  } else if (gUpdateAnnounced != gUpdate.version) {
    gUpdateAnnounced = gUpdate.version;
    gOverlay.Notify(L"WinDrop " + U2W(gUpdate.version) + L" is available", L"Tray icon → Update");
  }
}

void ShowMenu() {
  bool on = Cfg().enabled;
  HMENU m = CreatePopupMenu();
  std::wstring status = on ? PhonesLine() + L"  ·  link: " + TheWifiDirect().Status() : L"WinDrop is off";
  AppendMenuW(m, MF_STRING | MF_GRAYED, IDM_STATUS, status.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING | (on ? MF_CHECKED : 0), IDM_ON, L"On");
  AppendMenuW(m, MF_STRING | (Cfg().pillShown ? MF_CHECKED : 0) | (on ? 0 : MF_GRAYED), IDM_PILL, L"Show pill");
  AppendMenuW(m, MF_STRING | (on ? 0 : MF_GRAYED), IDM_SENDCLIP, L"Send clipboard to phone\tCtrl+Alt+P");
  size_t queued = 0;
  HMENU q = QueueMenu(&queued);
  std::wstring queueLabel = queued ? L"Waiting to send (" + std::to_wstring(queued) + L")" : L"Waiting to send";
  AppendMenuW(m, MF_POPUP, (UINT_PTR)q, queueLabel.c_str());  // the menu owns q now
  gMenuPhones = Phones();
  if (gMenuPhones.empty()) {
    AppendMenuW(m, MF_STRING, IDM_PAIR, L"Pair phone…");
  } else {
    HMENU ph = CreatePopupMenu();
    for (size_t i = 0; i < gMenuPhones.size(); ++i)
      AppendMenuW(ph, MF_STRING, IDM_PHONE_REMOVE + UINT(i), (L"Remove " + gMenuPhones[i].name + L"…").c_str());
    AppendMenuW(ph, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(ph, MF_STRING | (gMenuPhones.size() >= kMaxPhones ? MF_GRAYED : 0), IDM_PAIR, L"Pair another phone…");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)ph, L"Phones");
  }
  AppendMenuW(m, MF_STRING, IDM_OPENDIR, L"Open received folder");
  HMENU logs = CreatePopupMenu();
  AppendMenuW(logs, MF_STRING, IDM_SAVELOGS, L"Save logs to Downloads");
  AppendMenuW(logs, MF_STRING, IDM_CLEARLOGS, L"Clear logs");
  AppendMenuW(m, MF_POPUP, (UINT_PTR)logs, L"Logs");
  AppendMenuW(m, MF_STRING | (StartupEnabled() ? MF_CHECKED : 0), IDM_STARTUP, L"Start with Windows");
  std::wstring upd = gUpdate.version.empty() ? L"Check for updates (v" + U2W(WINDROP_VERSION) + L")"
                                             : L"Update to v" + U2W(gUpdate.version) + L"…";
  AppendMenuW(m, MF_STRING | (gUpdateBusy ? MF_GRAYED : 0), IDM_UPDATE, upd.c_str());
  AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(m, MF_STRING, IDM_EXIT, L"Quit");

  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(gMain);
  TrackPopupMenu(m, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, gMain, nullptr);
  PostMessageW(gMain, WM_NULL, 0, 0);
  DestroyMenu(m);
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
      else AskToUpdate();
      break;
    case IDM_QUEUE_CLEAR: {
      std::vector<uint64_t> ids;
      for (auto& it : TheOutbox().Snapshot()) ids.push_back(it.id);
      gNet.CancelOutbox(ids);
      Log("ui: queue cleared (%zu)", ids.size());
      gOverlay.Notify(L"Queue cleared", std::to_wstring(ids.size()) + L" item(s) won't be sent");
      break;
    }
    default:
      if (id >= IDM_PHONE_REMOVE && id - IDM_PHONE_REMOVE < gMenuPhones.size()) {
        auto p = gMenuPhones[id - IDM_PHONE_REMOVE];
        std::wstring q = L"Remove “" + p.name + L"”?\n\nIt won't be able to send to or receive from this PC until you pair it again. Anything waiting for it is dropped.";
        if (MessageBoxW(gMain, q.c_str(), L"WinDrop", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) != IDYES) break;
        std::vector<uint64_t> drop;
        for (auto& it : TheOutbox().Snapshot())
          if (it.phoneId == p.id) drop.push_back(it.id);
        gNet.CancelOutbox(drop);
        {
          std::lock_guard<std::recursive_mutex> lk(CfgMutex());
          Cfg().RemovePhone(p.id);
          Cfg().Save();
        }
        if (TheWifiDirect().LinkedPhone() == p.id) TheWifiDirect().Disconnect();
        Log("ui: removed phone %s", W2U(p.name).c_str());
        UpdateTray();
        gOverlay.Notify(L"Removed " + p.name, L"Pair it again from the tray to use it");
        break;
      }
      if (id >= IDM_QUEUE_ITEM && id - IDM_QUEUE_ITEM < gMenuQueue.size()) {
        auto& it = gMenuQueue[id - IDM_QUEUE_ITEM];
        gNet.CancelOutbox({it.id});
        Log("ui: removed from queue: %s", it.isText ? "text" : W2U(FileNameOf(it.path)).c_str());
        gOverlay.Notify(L"Removed from queue", QueueLabel(it));
      }
      break;
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
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
          ShowMenu();
          break;
      }
      return 0;
    case WM_APP_NET: {
      std::unique_ptr<UiEvent> e((UiEvent*)l);
      if (e->type == NetEv::TextReceived) SetClipboardText(U2W(e->text));
      if (e->type == NetEv::RecvDone && IsClipboardImage(e->name)) SetClipboardImage(e->path);
      if (e->type == NetEv::PairRequest) {
        // The phone shows the same number; a mismatch means someone is in the middle.
        std::wstring sas = U2W(e->text);
        std::wstring msg = L"Pair with \u201C" + e->name + L"\u201D?\n\nCheck that your phone shows the same number:\n\n        " +
                           sas.substr(0, 3) + L" " + sas.substr(3) + L"\n\nOnly click Yes if the numbers match.";
        int r = MessageBoxW(gMain, msg.c_str(), L"WinDrop \u2014 Pair phone", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST);
        gNet.Decide(e->id, r == IDYES);
        return 0;
      }
      if (e->type == NetEv::PairDone) {
        UpdateTray();
        if (e->count) gOverlay.Notify(L"Paired with " + e->name, L"Transfers are now encrypted");
        else gOverlay.Notify(L"Can't pair " + e->name, L"4 phones are paired already: tray → Phones to remove one", true);
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
      }
      return 0;
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
    if (HWND other = FindWindowW(kMainClass, nullptr); other && wcsstr(GetCommandLineW(), L"--pair")) {
      PostMessageW(other, WM_COMMAND, IDM_PAIR, 0);
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
  TheWifiDirect().onLinkUp = [] { gNet.OnLinkUp(); };
  TheWifiDirect().onUnreachable = [] { PostMessageW(gMain, WM_APP_UNREACHABLE, 0, 0); };
  if (!gNet.Start(gMain)) gOverlay.Notify(L"WinDrop can't listen on port 53318", L"Is another copy running?", true);
  AddTray();
  EnsureShortcuts();
  RefreshStartupPath();
  gOverlay.SetPillShown(Cfg().pillShown);
  SetOn(Cfg().enabled);
  RegisterHotKey(gMain, kHotkeySendClipboard, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
  if (!sendNow.empty() && Cfg().enabled) gOverlay.Dropped(sendNow, L"");
  SetTimer(gMain, kTimerUpdate, 30 * 1000, nullptr);  // first check shortly after start, then every 6 h

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
