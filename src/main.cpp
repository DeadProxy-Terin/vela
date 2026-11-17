#include "core/Config.hpp"
#include "core/FileWatcher.hpp"
#include "core/LuaEngine.hpp"
#include "core/StringUtil.hpp"
#include "fs/FileOps.hpp"
#include "fs/FileScanner.hpp"
#include "platform/OleDragDrop.hpp"
#include "platform/ThumbnailCache.hpp"
#include "platform/Win32Window.hpp"
#include "ui/VelaApp.hpp"

#include <windows.h>
#include <ole2.h>
#include <shellapi.h>

#include <filesystem>
#include <string>

namespace {

std::filesystem::path ResolveConfigPath() {
    wchar_t exePath[MAX_PATH]{};
    ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const auto exeDir = std::filesystem::path(exePath).parent_path();

    const auto localCfg = exeDir / "config" / "vela.lua";
    if (std::filesystem::exists(localCfg)) {
        return localCfg;
    }

    const auto cwdCfg = std::filesystem::current_path() / "config" / "vela.lua";
    if (std::filesystem::exists(cwdCfg)) {
        return cwdCfg;
    }

    return localCfg;
}

std::wstring ResolveStartupDirectoryFromCli() {
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (!argv) return {};

    std::wstring result;
    if (argc >= 2 && argv[1] && argv[1][0] != L'\0') {
        std::error_code ec;
        std::filesystem::path candidate(argv[1]);
        if (candidate.is_relative()) {
            candidate = std::filesystem::weakly_canonical(candidate, ec);
        }
        if (!ec && std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec)) {
            result = vela::str::NormalizeWindowsPath(candidate.wstring());
        }
    }

    ::LocalFree(argv);
    return result;
}

} // namespace

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Initialize OLE + COM as STA (required for Shell APIs, RegisterDragDrop, DoDragDrop, OleSetClipboard)
    if (FAILED(::OleInitialize(nullptr))) {
        return 1;
    }

    const std::filesystem::path configPath = ResolveConfigPath();
    const std::wstring cliDirW = ResolveStartupDirectoryFromCli();

    vela::Config activeConfig;
    vela::LuaEngine luaEngine;
    std::string initialLuaError;
    bool initialLuaOk = true;

    if (std::filesystem::exists(configPath)) {
        initialLuaOk = luaEngine.LoadConfig(configPath, activeConfig, initialLuaError);
    }

    vela::Win32Window window;
    if (!window.Create(activeConfig, L"Vela")) {
        ::OleUninitialize();
        return 1;
    }

    const HWND hwnd = window.GetHandle();

    vela::FileScanner scanner;
    scanner.Start([hwnd]() {
        ::PostMessageW(hwnd, vela::WM_APP_SCAN_COMPLETE, 0, 0);
    });

    vela::ThumbnailCache thumbCache;
    thumbCache.Start(
        window.GetDevice(),
        static_cast<size_t>(activeConfig.layout.thumbnailCacheMax),
        [hwnd]() {
            ::PostMessageW(hwnd, vela::WM_APP_THUMB_READY, 0, 0);
        }
    );

    vela::FileOps fileOps;
    fileOps.Start([hwnd]() {
        ::PostMessageW(hwnd, vela::WM_APP_FILEOP_NOTIFY, 0, 0);
    });

    vela::FileWatcher configWatcher;
    if (std::filesystem::exists(configPath)) {
        configWatcher.StartFile(configPath, [hwnd]() {
            ::PostMessageW(hwnd, vela::WM_APP_CONFIG_CHANGED, 0, 0);
        });
    }

    vela::FileWatcher activeDirWatcher;
    std::wstring watchedDirW;

    vela::VelaApp app;

    auto reloadConfig = [&]() {
        vela::Config nextConfig;
        std::string err;
        if (luaEngine.LoadConfig(configPath, nextConfig, err)) {
            const bool fontChanged =
                (nextConfig.theme.fontSize != activeConfig.theme.fontSize) ||
                (nextConfig.theme.fontPath != activeConfig.theme.fontPath);

            activeConfig = std::move(nextConfig);
            app.SetConfig(activeConfig);
            window.ApplyWindowConfig(activeConfig);
            if (fontChanged) {
                window.RequestFontRebuild();
            }
            app.NotifyConfigReloaded(true, "Reloaded vela.lua");
        } else {
            app.NotifyConfigReloaded(false, "vela.lua error: " + err);
        }
    };

    auto watchDirectory = [&](const std::wstring& dirW) {
        if (_wcsicmp(watchedDirW.c_str(), dirW.c_str()) == 0) {
            return;
        }
        watchedDirW = dirW;
        if (dirW.empty()) {
            activeDirWatcher.Stop();
        } else {
            activeDirWatcher.StartDirectory(std::filesystem::path(dirW), [hwnd]() {
                ::PostMessageW(hwnd, vela::WM_APP_DIR_CHANGED, 0, 0);
            });
        }
    };

    app.Initialize(&window, &scanner, &thumbCache, &fileOps, configPath, watchDirectory, reloadConfig);
    app.SetConfig(activeConfig);

    if (!initialLuaOk) {
        app.NotifyConfigReloaded(false, "vela.lua error: " + initialLuaError);
    }

    app.OpenInitialDirectory(cliDirW);

    vela::OleDragDropHost dropHost;
    {
        vela::DragTargetCallbacks dtCb;
        dtCb.onDragEnter = [&](POINT clientPt, DWORD keyState, DWORD allowed, const std::vector<std::wstring>& pathsW) {
            return app.OnOleDragEnter(clientPt, keyState, allowed, pathsW);
        };
        dtCb.onDragOver = [&](POINT clientPt, DWORD keyState, DWORD allowed) {
            return app.OnOleDragOver(clientPt, keyState, allowed);
        };
        dtCb.onDragLeave = [&]() {
            app.OnOleDragLeave();
        };
        dtCb.onDrop = [&](POINT clientPt, DWORD keyState, DWORD allowed, std::vector<std::wstring> pathsW) {
            return app.OnOleDrop(clientPt, keyState, allowed, std::move(pathsW));
        };
        dropHost.Attach(hwnd, std::move(dtCb));
    }

    vela::WindowCallbacks callbacks;
    callbacks.onBeforeFrame = [&]() {
        thumbCache.PumpUploads(16);
    };
    callbacks.onRender = [&](float dpiScale, vela::TitlebarHitRegions& regions) {
        app.Render(dpiScale, regions);
    };
    callbacks.onAfterFrame = [&]() {
        app.ProcessPendingDragDrop();
    };
    callbacks.onConfigChanged = reloadConfig;
    callbacks.onScanComplete = [&]() {
        app.OnScanCompleted();
    };
    callbacks.onDirChanged = [&]() {
        app.OnDirectoryChangedExternally();
    };
    callbacks.onDevicesChanged = [&]() {
        app.RefreshSystemPlaces();
    };
    callbacks.onFileOpNotify = [&]() {
        app.OnFileOpNotify();
    };

    const int exitCode = window.Run(activeConfig, std::move(callbacks));

    // Clean deterministic teardown
    dropHost.Detach();
    app.Shutdown();
    fileOps.Stop();
    activeDirWatcher.Stop();
    configWatcher.Stop();
    thumbCache.Stop();
    scanner.Stop();

    vela::OleDragDropHost::FlushClipboardOnExit();
    ::OleUninitialize();
    return exitCode;
}