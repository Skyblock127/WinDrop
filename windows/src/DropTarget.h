#pragma once
#include "common.h"

#include <oleidl.h>
#include <shobjidl.h>

namespace td {

class TopOverlay;

// OLE drop target for the top pill: files from Explorer (CF_HDROP) and text/URLs (CF_UNICODETEXT).
class DropTarget : public IDropTarget {
 public:
  DropTarget(TopOverlay* overlay, HWND hwnd);
  virtual ~DropTarget();

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override;
  ULONG STDMETHODCALLTYPE AddRef() override;
  ULONG STDMETHODCALLTYPE Release() override;

  HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* effect) override;
  HRESULT STDMETHODCALLTYPE DragLeave() override;
  HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD keys, POINTL pt, DWORD* effect) override;

  static bool Extract(IDataObject* data, std::vector<std::wstring>* files, std::wstring* text);

 private:
  LONG ref_ = 1;
  TopOverlay* overlay_;
  HWND hwnd_;
  bool accept_ = false;
  IDropTargetHelper* helper_ = nullptr;
};

}  // namespace td
