#pragma once

#include <windows.h>
#include <oleidl.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vela {

struct DragTargetCallbacks {
    std::function<DWORD(POINT clientPt, DWORD keyState, DWORD allowedEffects, const std::vector<std::wstring>& pathsW)> onDragEnter;
    std::function<DWORD(POINT clientPt, DWORD keyState, DWORD allowedEffects)> onDragOver;
    std::function<void()> onDragLeave;
    std::function<DWORD(POINT clientPt, DWORD keyState, DWORD allowedEffects, std::vector<std::wstring> pathsW)> onDrop;
};

class OleDragDropHost {
public:
    OleDragDropHost();
    ~OleDragDropHost();

    OleDragDropHost(const OleDragDropHost&) = delete;
    OleDragDropHost& operator=(const OleDragDropHost&) = delete;

    bool Attach(HWND hwnd, DragTargetCallbacks callbacks);
    void Detach();

    // Native Win32 OLE Drag Source & Shell Clipboard
    static DWORD StartFileDrag(
        const std::vector<std::wstring>& pathsW,
        DWORD allowedEffects = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK
    );
    static bool SetClipboardFiles(const std::vector<std::wstring>& pathsW, bool isCut);
    static bool HasClipboardFiles();
    static bool ReadClipboardFiles(std::vector<std::wstring>& outPathsW, bool& outIsCut);
    static void FlushClipboardOnExit();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace vela