#include "DropTarget.h"

#include <shellapi.h>
#include <shlobj.h>

#include <gdiplus.h>

#include <ctime>

#include "Settings.h"
#include "TopOverlayWindow.h"

namespace td {

DropTarget::DropTarget(TopOverlay* overlay, HWND hwnd) : overlay_(overlay), hwnd_(hwnd) {
  CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_IDropTargetHelper, (void**)&helper_);
}

DropTarget::~DropTarget() {
  if (helper_) helper_->Release();
}

HRESULT DropTarget::QueryInterface(REFIID riid, void** ppv) {
  if (riid == IID_IUnknown || riid == IID_IDropTarget) {
    *ppv = static_cast<IDropTarget*>(this);
    AddRef();
    return S_OK;
  }
  *ppv = nullptr;
  return E_NOINTERFACE;
}

ULONG DropTarget::AddRef() { return InterlockedIncrement(&ref_); }

ULONG DropTarget::Release() {
  ULONG r = InterlockedDecrement(&ref_);
  if (r == 0) delete this;
  return r;
}

static bool Has(IDataObject* d, CLIPFORMAT cf) {
  FORMATETC fe{cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  return d->QueryGetData(&fe) == S_OK;
}

static CLIPFORMAT PngFormat() {
  static CLIPFORMAT cf = (CLIPFORMAT)RegisterClipboardFormatW(L"PNG");
  return cf;
}

static bool HasImage(IDataObject* d) { return Has(d, PngFormat()) || Has(d, CF_DIBV5) || Has(d, CF_DIB); }

// "Clipboard <date> <time>.png" in our data folder. The name tells the other side to also put the
// image on its clipboard.
static std::wstring ClipImagePath() {
  std::wstring dir = Cfg().dataDir + L"\\clip";
  CreateDirectoryW(dir.c_str(), nullptr);
  wchar_t name[64];
  time_t t = time(nullptr);
  tm lt;
  localtime_s(&lt, &t);
  wcsftime(name, 64, L"Clipboard %Y-%m-%d %H.%M.%S.png", &lt);
  return dir + L"\\" + name;
}

static bool PngEncoder(CLSID* clsid) {
  UINT n = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&n, &size);
  if (!size) return false;
  std::vector<BYTE> buf(size);
  auto* codecs = (Gdiplus::ImageCodecInfo*)buf.data();
  Gdiplus::GetImageEncoders(n, size, codecs);
  for (UINT i = 0; i < n; ++i)
    if (wcscmp(codecs[i].MimeType, L"image/png") == 0) { *clsid = codecs[i].Clsid; return true; }
  return false;
}

// Screenshots (Snipping Tool, PrtScn) and "Copy image" put pixels on the clipboard, not a file:
// write them out as a PNG so they can be sent like any file.
static std::wstring SaveImage(IDataObject* data) {
  std::wstring path = ClipImagePath();
  FORMATETC fe{PngFormat(), nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM sm{};
  if (data->GetData(&fe, &sm) == S_OK) {
    bool ok = false;
    if (auto p = GlobalLock(sm.hGlobal)) {
      SIZE_T n = GlobalSize(sm.hGlobal);
      HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      DWORD wrote = 0;
      if (f != INVALID_HANDLE_VALUE) { ok = WriteFile(f, p, (DWORD)n, &wrote, nullptr) && wrote == n; CloseHandle(f); }
      GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    if (ok) return path;
  }
  for (CLIPFORMAT cf : {(CLIPFORMAT)CF_DIBV5, (CLIPFORMAT)CF_DIB}) {
    fe.cfFormat = cf;
    if (data->GetData(&fe, &sm) != S_OK) continue;
    bool ok = false;
    if (auto bi = (BITMAPINFO*)GlobalLock(sm.hGlobal)) {
      auto& h = bi->bmiHeader;
      DWORD colors = h.biClrUsed ? h.biClrUsed : (h.biBitCount <= 8 ? 1u << h.biBitCount : 0);
      DWORD masks = (h.biCompression == BI_BITFIELDS && h.biSize == sizeof(BITMAPINFOHEADER)) ? 3 * sizeof(DWORD) : 0;
      BYTE* bits = (BYTE*)bi + h.biSize + masks + colors * sizeof(RGBQUAD);
      Gdiplus::Bitmap bmp(bi, bits);
      CLSID png;
      ok = bmp.GetLastStatus() == Gdiplus::Ok && PngEncoder(&png) && bmp.Save(path.c_str(), &png) == Gdiplus::Ok;
      GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    if (ok) return path;
  }
  return L"";
}

HRESULT DropTarget::DragEnter(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
  accept_ = Has(data, CF_HDROP) || Has(data, CF_UNICODETEXT) || HasImage(data);
  *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (helper_) {
    POINT p{pt.x, pt.y};
    helper_->DragEnter(hwnd_, data, &p, *effect);
  }
  if (accept_) overlay_->DragEntered();
  return S_OK;
}

HRESULT DropTarget::DragOver(DWORD, POINTL pt, DWORD* effect) {
  *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (helper_) {
    POINT p{pt.x, pt.y};
    helper_->DragOver(&p, *effect);
  }
  return S_OK;
}

HRESULT DropTarget::DragLeave() {
  if (helper_) helper_->DragLeave();
  if (accept_) overlay_->DragLeft();
  return S_OK;
}

HRESULT DropTarget::Drop(IDataObject* data, DWORD, POINTL pt, DWORD* effect) {
  *effect = accept_ ? DROPEFFECT_COPY : DROPEFFECT_NONE;
  if (helper_) {
    POINT p{pt.x, pt.y};
    helper_->Drop(data, &p, *effect);
  }
  std::vector<std::wstring> files;
  std::wstring text;
  if (accept_ && Extract(data, &files, &text)) overlay_->Dropped(files, text);
  else overlay_->DragLeft();
  return S_OK;
}

bool DropTarget::Extract(IDataObject* data, std::vector<std::wstring>* files, std::wstring* text) {
  FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM sm{};
  if (data->GetData(&fe, &sm) == S_OK) {
    auto drop = (HDROP)GlobalLock(sm.hGlobal);
    if (drop) {
      UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
      for (UINT i = 0; i < n; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        std::wstring p(len, L'\0');
        DragQueryFileW(drop, i, p.data(), len + 1);
        files->push_back(p);
      }
      GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    if (!files->empty()) return true;
  }
  fe.cfFormat = CF_UNICODETEXT;
  if (data->GetData(&fe, &sm) == S_OK) {
    auto p = (const wchar_t*)GlobalLock(sm.hGlobal);
    if (p) {
      *text = p;
      GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
  }
  if (!text->empty()) return true;
  if (HasImage(data)) {
    std::wstring img = SaveImage(data);
    if (!img.empty()) {
      Log("clipboard: image saved as %s", W2U(FileNameOf(img)).c_str());
      files->push_back(img);
      return true;
    }
  }
  return false;
}

}  // namespace td
