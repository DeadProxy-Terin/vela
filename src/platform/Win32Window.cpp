#include "platform/Win32Window.hpp"
#include "core/StringUtil.hpp"

#include <imgui.h>
#include <imgui_freetype.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include <dbt.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMSBT_TRANSIENTWINDOW
#define DWMSBT_TRANSIENTWINDOW 3
#endif
#ifndef DWMSBT_TABBEDWINDOW
#define DWMSBT_TABBEDWINDOW 4
#endif

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace vela {
namespace {

bool PointInRect(const RECT& rc, POINT pt) {
    return pt.x >= rc.left && pt.x < rc.right && pt.y >= rc.top && pt.y < rc.bottom;
}

} // namespace

Win32Window::~Win32Window() {
    CleanupDevice();
    if (m_hwnd) {
        ::DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

bool Win32Window::Create(const Config& cfg, const wchar_t* title) {
    ImGui_ImplWin32_EnableDpiAwareness();
    m_hInstance = ::GetModuleHandleW(nullptr);
    m_minWidth = cfg.window.minWidth;
    m_minHeight = cfg.window.minHeight;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = &Win32Window::StaticWndProc;
    wc.hInstance = m_hInstance;
    wc.hInstance = m_hInstance;
    wc.hIcon = static_cast<HICON>(::LoadImageW(
        m_hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXICON), ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR
    ));
    wc.hIconSm = static_cast<HICON>(::LoadImageW(
        m_hInstance, MAKEINTRESOURCEW(101), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR
    ));
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"VelaWindowClass";
    ::RegisterClassExW(&wc);

    POINT origin{0, 0};
    HMONITOR mon = ::MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    m_dpiScale = ImGui_ImplWin32_GetDpiScaleForMonitor(mon);

    const int physW = static_cast<int>(cfg.window.width * m_dpiScale);
    const int physH = static_cast<int>(cfg.window.height * m_dpiScale);

    m_hwnd = ::CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW,
        wc.lpszClassName,
        title,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT,
        physW, physH,
        nullptr, nullptr,
        m_hInstance,
        this
    );

    if (!m_hwnd) {
        return false;
    }

    MARGINS margins = {-1, -1, -1, -1};
    ::DwmExtendFrameIntoClientArea(m_hwnd, &margins);
    ApplyWindowConfig(cfg);

    if (!CreateDeviceAndSwapChain()) {
        return false;
    }

    ::SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0,
        SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    ::ShowWindow(m_hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(m_hwnd);
    return true;
}

void Win32Window::ApplyWindowConfig(const Config& cfg) {
    m_minWidth = cfg.window.minWidth;
    m_minHeight = cfg.window.minHeight;
    if (!m_hwnd) return;

    BOOL dark = cfg.window.darkMode ? TRUE : FALSE;
    ::DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    int backdrop = DWMSBT_TABBEDWINDOW;
    switch (cfg.window.backdrop) {
        case BackdropMode::MicaAlt: backdrop = DWMSBT_TABBEDWINDOW; break;
        case BackdropMode::Mica:    backdrop = DWMSBT_MAINWINDOW; break;
        case BackdropMode::Acrylic: backdrop = DWMSBT_TRANSIENTWINDOW; break;
        case BackdropMode::Solid:   backdrop = DWMSBT_NONE; break;
    }
    ::DwmSetWindowAttribute(m_hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    Wake();
}

void Win32Window::RequestFontRebuild() {
    m_needsFontRebuild = true;
    Wake();
}

void Win32Window::Wake() {
    m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
    if (m_hwnd) {
        ::PostMessageW(m_hwnd, WM_APP_WAKE, 0, 0);
    }
}

void Win32Window::ScheduleWake(UINT delayMs) {
    if (m_hwnd) {
        ::SetTimer(m_hwnd, kWakeTimerId, delayMs, nullptr);
    }
}

void Win32Window::Minimize() {
    if (m_hwnd) ::ShowWindow(m_hwnd, SW_MINIMIZE);
}

void Win32Window::ToggleMaximize() {
    if (!m_hwnd) return;
    ::ShowWindow(m_hwnd, ::IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE);
}

void Win32Window::Close() {
    if (m_hwnd) ::PostMessageW(m_hwnd, WM_CLOSE, 0, 0);
}

bool Win32Window::SetClipboardText(const std::string& utf8Text) const {
    const std::wstring wide = str::Utf8ToWide(utf8Text);
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);

    HGLOBAL hMem = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!hMem) return false;

    void* dst = ::GlobalLock(hMem);
    if (!dst) {
        ::GlobalFree(hMem);
        return false;
    }
    std::memcpy(dst, wide.c_str(), bytes);
    ::GlobalUnlock(hMem);

    if (!::OpenClipboard(m_hwnd)) {
        ::GlobalFree(hMem);
        return false;
    }
    ::EmptyClipboard();
    if (!::SetClipboardData(CF_UNICODETEXT, hMem)) {
        ::GlobalFree(hMem);
        ::CloseClipboard();
        return false;
    }
    ::CloseClipboard();
    return true;
}

bool Win32Window::OpenShellItem(const std::wstring& pathOrShellUri) const {
    if (pathOrShellUri.empty()) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = pathOrShellUri.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&sei) != FALSE;
}

bool Win32Window::OpenPropertiesDialog(const std::wstring& pathW) const {
    if (pathW.empty()) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_INVOKEIDLIST;
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"properties";
    sei.lpFile = pathW.c_str();
    sei.nShow = SW_SHOW;
    return ::ShellExecuteExW(&sei) != FALSE;
}

bool Win32Window::OpenInTerminal(const std::wstring& dirPathW, const std::string& program, const std::string& args) const {
    if (dirPathW.empty()) return false;

    auto existsOnPath = [](const wchar_t* exeName) -> bool {
        if (!exeName || exeName[0] == L'\0') return false;
        if (std::filesystem::exists(exeName)) return true;
        return ::SearchPathW(nullptr, exeName, L".exe", 0, nullptr, nullptr) != 0;
    };

    std::wstring progW = str::Utf8ToWide(program.empty() ? "wt.exe" : program);
    std::wstring argsW = str::Utf8ToWide(args);

    if (!existsOnPath(progW.c_str())) {
        if (existsOnPath(L"pwsh.exe")) {
            progW = L"pwsh.exe";
            argsW.clear();
        } else if (existsOnPath(L"powershell.exe")) {
            progW = L"powershell.exe";
            argsW.clear();
        } else {
            progW = L"cmd.exe";
            argsW.clear();
        }
    }

    std::wstring cleanDir = dirPathW;
    if (!cleanDir.empty() && cleanDir.back() == L'\\') {
        cleanDir.push_back(L'\\');
    }

    std::wstring params;
    if (!argsW.empty()) {
        params = argsW + L" \"" + cleanDir + L"\"";
    }

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = progW.c_str();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.lpDirectory = dirPathW.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&sei) != FALSE;
}

bool Win32Window::EditFileDefault(const std::filesystem::path& filePath) const {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.hwnd = m_hwnd;
    sei.lpVerb = L"open";
    sei.lpFile = filePath.c_str();
    sei.nShow = SW_SHOWNORMAL;
    return ::ShellExecuteExW(&sei) != FALSE;
}

bool Win32Window::CreateDeviceAndSwapChain() {
    RECT rc{};
    ::GetClientRect(m_hwnd, &rc);
    const UINT width = (std::max)(1L, rc.right - rc.left);
    const UINT height = (std::max)(1L, rc.bottom - rc.top);

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL createdLevel{};

    HRESULT hr = ::D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        levels, 2, D3D11_SDK_VERSION,
        &m_device, &createdLevel, &m_context
    );
    if (hr == DXGI_ERROR_UNSUPPORTED || FAILED(hr)) {
        hr = ::D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            levels, 2, D3D11_SDK_VERSION,
            &m_device, &createdLevel, &m_context
        );
    }
    if (FAILED(hr)) return false;

    Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_device.As(&dxgiDevice))) return false;

    Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return false;

    Microsoft::WRL::ComPtr<IDXGIFactory2> factory;
    if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;

    if (FAILED(factory->CreateSwapChainForComposition(m_device.Get(), &desc, nullptr, &m_swapChain))) {
        return false;
    }

    if (FAILED(::DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&m_dcompDevice)))) {
        return false;
    }
    if (FAILED(m_dcompDevice->CreateTargetForHwnd(m_hwnd, TRUE, &m_dcompTarget))) {
        return false;
    }
    if (FAILED(m_dcompDevice->CreateVisual(&m_dcompVisual))) {
        return false;
    }
    if (FAILED(m_dcompVisual->SetContent(m_swapChain.Get()))) {
        return false;
    }
    if (FAILED(m_dcompTarget->SetRoot(m_dcompVisual.Get()))) {
        return false;
    }
    if (FAILED(m_dcompDevice->Commit())) {
        return false;
    }

    CreateRenderTarget();
    return m_rtv != nullptr;
}

void Win32Window::CreateRenderTarget() {
    Microsoft::WRL::ComPtr<ID3D11Texture2D> backBuffer;
    if (SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer)))) {
        m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv);
    }
}

void Win32Window::ResizeSwapChain(UINT width, UINT height) {
    if (!m_swapChain || width == 0 || height == 0) return;
    m_rtv.Reset();
    m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    CreateRenderTarget();
    m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
}

void Win32Window::CleanupDevice() {
    m_rtv.Reset();
    m_dcompVisual.Reset();
    m_dcompTarget.Reset();
    m_dcompDevice.Reset();
    m_swapChain.Reset();
    m_context.Reset();
    m_device.Reset();
}

void Win32Window::RebuildFontsIfNeeded(const Config& cfg) {
    if (!m_needsFontRebuild) return;
    m_needsFontRebuild = false;

    ImGuiIO& io = ImGui::GetIO();
    ImGui_ImplDX11_InvalidateDeviceObjects();
    io.Fonts->Clear();

    const float scaledSize = std::round(cfg.theme.fontSize * m_dpiScale);
    std::string chosenFont;

    if (!cfg.theme.fontPath.empty() && std::filesystem::exists(cfg.theme.fontPath)) {
        chosenFont = cfg.theme.fontPath;
    } else if (std::filesystem::exists("C:\\Windows\\Fonts\\SegUIVar.ttf")) {
        chosenFont = "C:\\Windows\\Fonts\\SegUIVar.ttf";
    } else if (std::filesystem::exists("C:\\Windows\\Fonts\\segoeui.ttf")) {
        chosenFont = "C:\\Windows\\Fonts\\segoeui.ttf";
    }

    ImFontConfig fontCfg;
    fontCfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;

    if (!chosenFont.empty()) {
        io.Fonts->AddFontFromFileTTF(chosenFont.c_str(), scaledSize, &fontCfg);
    } else {
        io.Fonts->AddFontDefault();
    }

    std::string iconFontPath;
    if (std::filesystem::exists("C:\\Windows\\Fonts\\SegoeIcons.ttf")) {
        iconFontPath = "C:\\Windows\\Fonts\\SegoeIcons.ttf";
    } else if (std::filesystem::exists("C:\\Windows\\Fonts\\segmdl2.ttf")) {
        iconFontPath = "C:\\Windows\\Fonts\\segmdl2.ttf";
    }

    if (!iconFontPath.empty()) {
        static constexpr ImWchar iconRanges[] = {0xE700, 0xF8FF, 0};
        ImFontConfig iconCfg;
        iconCfg.MergeMode = true;
        iconCfg.PixelSnapH = true;
        iconCfg.GlyphOffset = ImVec2(0.0f, 1.0f * m_dpiScale);
        iconCfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
        io.Fonts->AddFontFromFileTTF(iconFontPath.c_str(), scaledSize, &iconCfg, iconRanges);
    }

    // Merge Japanese / CJK glyphs from built-in Windows fonts
    const char* cjkCandidates[] = {
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\YuGothR.ttc",
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc"
    };
    for (const char* cjkPath : cjkCandidates) {
        if (std::filesystem::exists(cjkPath)) {
            ImFontConfig cjkCfg;
            cjkCfg.MergeMode = true;
            cjkCfg.FontBuilderFlags = ImGuiFreeTypeBuilderFlags_LightHinting;
            io.Fonts->AddFontFromFileTTF(cjkPath, scaledSize, &cjkCfg, io.Fonts->GetGlyphRangesJapanese());
            break;
        }
    }

    io.Fonts->Build();
    ImGui_ImplDX11_CreateDeviceObjects();
}

void Win32Window::RenderFrame() {
    if (!m_currentConfig || !m_rtv || m_inFrame || ::IsIconic(m_hwnd)) return;
    m_inFrame = true;

    RebuildFontsIfNeeded(*m_currentConfig);

    if (m_callbacks.onBeforeFrame) {
        m_callbacks.onBeforeFrame();
    }

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    if (m_callbacks.onRender) {
        m_callbacks.onRender(m_dpiScale, m_hitRegions);
    }

    ImGui::Render();

    const float clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
    m_context->ClearRenderTargetView(m_rtv.Get(), clearColor);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    m_swapChain->Present(1, 0);

    if (m_activeFramesRemaining > 0) {
        --m_activeFramesRemaining;
    }

    m_inFrame = false;
}

int Win32Window::Run(const Config& cfg, WindowCallbacks callbacks) {
    m_currentConfig = &cfg;
    m_callbacks = std::move(callbacks);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui_ImplWin32_Init(m_hwnd);
    ImGui_ImplDX11_Init(m_device.Get(), m_context.Get());

    bool running = true;
    while (running) {
        if (m_activeFramesRemaining <= 0) {
            ::WaitMessage();
            m_activeFramesRemaining = 4;
        }

        MSG msg{};
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) {
                running = false;
            }
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 5);
        }
        if (!running) break;

        if (::IsIconic(m_hwnd)) {
            ::Sleep(16);
            continue;
        }

        RenderFrame();

        if (m_callbacks.onAfterFrame && !m_inAfterFrame) {
            m_inAfterFrame = true;
            m_callbacks.onAfterFrame();
            m_inAfterFrame = false;
        }
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    return 0;
}

LRESULT CALLBACK Win32Window::StaticWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    Win32Window* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<Win32Window*>(cs->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->m_hwnd = hwnd;
    } else {
        self = reinterpret_cast<Win32Window*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (self) {
        return self->WndProc(hwnd, msg, wParam, lParam);
    }
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT Win32Window::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui::GetCurrentContext() != nullptr) {
        if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) {
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
            return true;
        }
    }

       switch (msg) {
        case WM_APP_CONFIG_CHANGED:
            if (m_callbacks.onConfigChanged) m_callbacks.onConfigChanged();
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            return 0;

        case WM_APP_SCAN_COMPLETE:
            if (m_callbacks.onScanComplete) m_callbacks.onScanComplete();
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            return 0;

        case WM_APP_DIR_CHANGED:
            if (m_callbacks.onDirChanged) m_callbacks.onDirChanged();
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            return 0;

        case WM_APP_FILEOP_NOTIFY:
            if (m_callbacks.onFileOpNotify) m_callbacks.onFileOpNotify();
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            return 0;

        case WM_APP_THUMB_READY:
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
            return 0;

        case WM_APP_WAKE: {
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
            if (m_inAfterFrame && !m_inFrame) {
                MSG dummy{};
                while (::PeekMessageW(&dummy, hwnd, WM_APP_WAKE, WM_APP_WAKE, PM_REMOVE)) {}
                RenderFrame();
            }
            return 0;
        }

        case WM_TIMER:
            if (wParam == kWakeTimerId) {
                ::KillTimer(hwnd, kWakeTimerId);
                m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
                return 0;
            }
            break;

        case WM_DEVICECHANGE:
            if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
                if (m_callbacks.onDevicesChanged) m_callbacks.onDevicesChanged();
                m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            }
            break;

        case WM_NCCALCSIZE: {
            if (!wParam) break;
            auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
            const UINT dpi = ::GetDpiForWindow(hwnd);
            const int frameX = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                               ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int frameY = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                               ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            params->rgrc[0].left += frameX;
            params->rgrc[0].right -= frameX;
            params->rgrc[0].bottom -= frameY;
            if (::IsZoomed(hwnd)) {
                params->rgrc[0].top += frameY;
            }
            return 0;
        }

        case WM_NCHITTEST: {
            LRESULT dwmResult = 0;
            if (::DwmDefWindowProc(hwnd, msg, wParam, lParam, &dwmResult)) {
                return dwmResult;
            }

            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ::ScreenToClient(hwnd, &pt);

            RECT rc{};
            ::GetClientRect(hwnd, &rc);

            const UINT dpi = ::GetDpiForWindow(hwnd);
            const int borderX = ::GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
                                ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
            const int borderY = ::GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
                                ::GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);

            if (!::IsZoomed(hwnd)) {
                const bool left = pt.x < borderX;
                const bool right = pt.x >= rc.right - borderX;
                const bool top = pt.y < borderY;
                const bool bottom = pt.y >= rc.bottom - borderY;

                if (top && left) return HTTOPLEFT;
                if (top && right) return HTTOPRIGHT;
                if (bottom && left) return HTBOTTOMLEFT;
                if (bottom && right) return HTBOTTOMRIGHT;
                if (top) return HTTOP;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
                if (bottom) return HTBOTTOM;
            }

            if (PointInRect(m_hitRegions.maximizeButton, pt)) {
                return HTMAXBUTTON;
            }

            for (const RECT& interactive : m_hitRegions.interactiveRects) {
                if (PointInRect(interactive, pt)) {
                    return HTCLIENT;
                }
            }

            if (pt.y < m_hitRegions.titlebarHeightPx) {
                return HTCAPTION;
            }
            return HTCLIENT;
        }

        case WM_NCMOUSEMOVE: {
            const bool nowMaxHover = (wParam == HTMAXBUTTON);
            if (nowMaxHover != m_maxBtnHovered) {
                m_maxBtnHovered = nowMaxHover;
                m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
            }
            if (!m_trackingNcLeave) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_NONCLIENT | TME_LEAVE, hwnd, 0};
                if (::TrackMouseEvent(&tme)) {
                    m_trackingNcLeave = true;
                }
            }
            break;
        }

        case WM_NCMOUSELEAVE:
        case WM_MOUSELEAVE:
            m_trackingNcLeave = false;
            if (m_maxBtnHovered || m_maxBtnPressed) {
                m_maxBtnHovered = false;
                m_maxBtnPressed = false;
                m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
            }
            break;

        case WM_NCLBUTTONDOWN:
            if (wParam == HTMAXBUTTON) {
                m_maxBtnPressed = true;
                m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 4);
                return 0;
            }
            break;

        case WM_NCLBUTTONUP:
            if (wParam == HTMAXBUTTON) {
                if (m_maxBtnPressed) {
                    m_maxBtnPressed = false;
                    ToggleMaximize();
                }
                return 0;
            }
            m_maxBtnPressed = false;
            break;

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = static_cast<LONG>(m_minWidth * m_dpiScale);
            mmi->ptMinTrackSize.y = static_cast<LONG>(m_minHeight * m_dpiScale);
            return 0;
        }

        case WM_DPICHANGED: {
            const UINT newDpi = HIWORD(wParam);
            m_dpiScale = static_cast<float>(newDpi) / 96.0f;
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(
                hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE
            );
            m_needsFontRebuild = true;
            m_activeFramesRemaining = (std::max)(m_activeFramesRemaining, 6);
            return 0;
        }

        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) {
                ResizeSwapChain(LOWORD(lParam), HIWORD(lParam));
            }
            return 0;

        case WM_DESTROY:
            ::PostQuitMessage(0);
            return 0;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace vela