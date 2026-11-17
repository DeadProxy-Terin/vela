#pragma once

#include "core/Config.hpp"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace vela {

constexpr UINT WM_APP_CONFIG_CHANGED = WM_APP + 1;
constexpr UINT WM_APP_WAKE           = WM_APP + 2;
constexpr UINT WM_APP_SCAN_COMPLETE  = WM_APP + 3;
constexpr UINT WM_APP_DIR_CHANGED    = WM_APP + 4;
constexpr UINT WM_APP_THUMB_READY    = WM_APP + 5;
constexpr UINT WM_APP_FILEOP_NOTIFY  = WM_APP + 6;

struct TitlebarHitRegions {
    int titlebarHeightPx = 42;
    RECT maximizeButton{};
    std::vector<RECT> interactiveRects; // Tabs, "+", Minimize, Close
};

struct WindowCallbacks {
    std::function<void()> onBeforeFrame;
    std::function<void(float dpiScale, TitlebarHitRegions& outRegions)> onRender;
    std::function<void()> onAfterFrame;
    std::function<void()> onConfigChanged;
    std::function<void()> onScanComplete;
    std::function<void()> onDirChanged;
    std::function<void()> onDevicesChanged;
    std::function<void()> onFileOpNotify;
};

class Win32Window {
public:
    Win32Window() = default;
    ~Win32Window();

    Win32Window(const Win32Window&) = delete;
    Win32Window& operator=(const Win32Window&) = delete;

    bool Create(const Config& cfg, const wchar_t* title);
    void ApplyWindowConfig(const Config& cfg);
    void RequestFontRebuild();
    void Wake();
    void ScheduleWake(UINT delayMs);

    int Run(const Config& cfg, WindowCallbacks callbacks);

    HWND GetHandle() const { return m_hwnd; }
    ID3D11Device* GetDevice() const { return m_device.Get(); }
    ID3D11DeviceContext* GetContext() const { return m_context.Get(); }
    float GetDpiScale() const { return m_dpiScale; }

    bool IsMaximized() const { return m_hwnd && ::IsZoomed(m_hwnd) != FALSE; }
    bool IsMaximizeHovered() const { return m_maxBtnHovered; }
    bool IsMaximizePressed() const { return m_maxBtnPressed; }

    void Minimize();
    void ToggleMaximize();
    void Close();

    // Native Windows OS helpers
    bool SetClipboardText(const std::string& utf8Text) const;
    bool OpenShellItem(const std::wstring& pathOrShellUri) const;
    bool OpenPropertiesDialog(const std::wstring& pathW) const;
    bool OpenInTerminal(const std::wstring& dirPathW, const std::string& program, const std::string& args) const;
    bool EditFileDefault(const std::filesystem::path& filePath) const;

private:
    static LRESULT CALLBACK StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

    bool CreateDeviceAndSwapChain();
    void CreateRenderTarget();
    void ResizeSwapChain(UINT width, UINT height);
    void CleanupDevice();
    void RebuildFontsIfNeeded(const Config& cfg);
    void RenderFrame();

    static constexpr UINT_PTR kWakeTimerId = 1;

    HWND m_hwnd = nullptr;
    HINSTANCE m_hInstance = nullptr;
    float m_dpiScale = 1.0f;

    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGISwapChain1>        m_swapChain;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_rtv;
    Microsoft::WRL::ComPtr<IDCompositionDevice>    m_dcompDevice;
    Microsoft::WRL::ComPtr<IDCompositionTarget>    m_dcompTarget;
    Microsoft::WRL::ComPtr<IDCompositionVisual>    m_dcompVisual;

    TitlebarHitRegions m_hitRegions;
    int m_minWidth = 760;
    int m_minHeight = 500;
    bool m_maxBtnHovered = false;
    bool m_maxBtnPressed = false;
    bool m_trackingNcLeave = false;
    bool m_needsFontRebuild = true;
    int m_activeFramesRemaining = 10;
    bool m_inFrame = false;
    bool m_inAfterFrame = false;

    WindowCallbacks m_callbacks;
    const Config* m_currentConfig = nullptr;
};

} // namespace vela