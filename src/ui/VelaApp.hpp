#pragma once

#include "core/Config.hpp"
#include "fs/FileEntry.hpp"
#include "fs/FileOps.hpp"
#include "fs/SystemPlaces.hpp"
#include "platform/Win32Window.hpp"
#include "ui/VelaDialogs.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_set>
#include <vector>

namespace vela {

class FileScanner;
class ThumbnailCache;

enum class CopyPathFormat {
    Windows,
    Posix,
    Quoted,
    Filename
};

struct NavTarget {
    bool isHome = false;
    std::wstring pathW;
};

struct TabState {
    std::string title = "Home";
    std::wstring currentPathW;
    std::string currentPath;
    bool isHome = true;

    std::vector<NavTarget> backHistory;
    std::vector<NavTarget> forwardHistory;

    ViewMode viewMode = ViewMode::Grid;
    SortField sortField = SortField::Name;
    SortOrder sortOrder = SortOrder::Ascending;
    bool showHidden = false;

    bool isLoading = false;
    uint64_t expectedGeneration = 0;
    std::string errorMessage;

    std::vector<FileEntry> allEntries;
    std::vector<size_t> visibleIndices;
    std::unordered_set<size_t> selectedAllIndices;
    int anchorVisibleIdx = -1;
    int focusedVisibleIdx = -1;
    bool scrollToTopPending = false;
};

class VelaApp {
public:
    VelaApp() = default;

    void Initialize(
        Win32Window* window,
        FileScanner* scanner,
        ThumbnailCache* thumbCache,
        FileOps* fileOps,
        std::filesystem::path configPath,
        std::function<void(const std::wstring&)> onWatchDirectory,
        std::function<void()> onReloadConfig
    );
    void Shutdown();

    void SetConfig(const Config& cfg);
    void RefreshSystemPlaces();
    void OpenInitialDirectory(const std::wstring& pathW);

    void OnScanCompleted();
    void OnDirectoryChangedExternally();
    void OnFileOpNotify();
    void NotifyConfigReloaded(bool success, const std::string& message);

    void Render(float dpiScale, TitlebarHitRegions& outRegions);
    void ProcessPendingDragDrop();

    // Win32 OLE IDropTarget integration
    DWORD OnOleDragEnter(POINT clientPt, DWORD keyState, DWORD allowedEffects, const std::vector<std::wstring>& pathsW);
    DWORD OnOleDragOver(POINT clientPt, DWORD keyState, DWORD allowedEffects);
    void OnOleDragLeave();
    DWORD OnOleDrop(POINT clientPt, DWORD keyState, DWORD allowedEffects, std::vector<std::wstring> pathsW);

private:
    struct DropZone {
        enum class Kind {
            Tab,
            Breadcrumb,
            SidebarItem,
            FolderEntry,
            Viewport
        };
        Kind kind = Kind::Viewport;
        float minX = 0.0f;
        float minY = 0.0f;
        float maxX = 0.0f;
        float maxY = 0.0f;
        std::wstring targetDirW;
        int tabIndex = -1;
    };

    // Navigation & Tab Management
    void OpenNewTab(const NavTarget& target, bool switchToTab = true);
    void CloseTab(int index);
    void SwitchTab(int index);
    void NavigateTo(const NavTarget& target, bool recordHistory = true);
    void NavigateUp();
    void NavigateBack();
    void NavigateForward();
    void RefreshCurrentTab();

    // Directory Listing & Sorting
    void ApplyFolderRules(TabState& tab);
    void SortAndFilterTab(TabState& tab);
    void RebuildVisibleIndices(TabState& tab);
    void ActivateEntry(TabState& tab, size_t allIndex);

    // Selection & Actions
    void HandleItemClick(TabState& tab, int visibleIdx, bool ctrlDown, bool shiftDown);
    void HandleItemMouseInteraction(TabState& tab, int visibleIdx, size_t allIdx, const FileEntry& entry, float dpiScale);
    void SelectAll(TabState& tab);
    void ClearSelection(TabState& tab);
    std::vector<std::wstring> GetSelectedPathsW(const TabState& tab) const;
    void CopySelectedPaths(CopyPathFormat format);
    void CopySelectedFilesToClipboard(bool isCut);
    void PasteClipboardFiles(const std::wstring& overrideDestDirW = L"");
    void PromptNewItem(bool isFolder);
    void PromptRenameSelected();
    void PromptDeleteSelected(bool permanent);
    void OpenSelectedInTerminal();
    void OpenSelectedProperties();

    // Drag & Drop Resolution
    void RegisterDropZone(DropZone::Kind kind, float minX, float minY, float maxX, float maxY,
                          const std::wstring& targetDirW, int tabIndex = -1);
    const DropZone* ResolveDropZone(POINT clientPt) const;
    bool CanDropSourcesInto(const std::vector<std::wstring>& sourcesW, const std::wstring& targetDirW) const;
    DWORD EvaluateDropEffect(POINT clientPt, DWORD keyState, DWORD allowedEffects, const DropZone*& outZone);
    void ExecuteDropAction(const std::vector<std::wstring>& sourcesW, const std::wstring& destDirW, DWORD keyState);

    // Shortcuts & UI Rendering
    void HandleGlobalShortcuts();
    void ShowToast(bool isError, const std::string& text, UINT durationMs = 2200);

    void RenderTitlebar(float dpiScale, TitlebarHitRegions& outRegions);
    void RenderNavBar(float dpiScale);
    void RenderToolbar(float dpiScale);
    void RenderSidebar(float dpiScale);
    void RenderContentCard(float dpiScale);
    void RenderHomeDashboard(float dpiScale);
    void RenderVirtualGrid(TabState& tab, float dpiScale);
    void RenderVirtualList(TabState& tab, float dpiScale);
    void RenderVirtualDetails(TabState& tab, float dpiScale);
    void RenderStatusBar(float dpiScale);
    void RenderContextMenu(TabState& tab);
    void RenderToast(float dpiScale);

    bool SidebarRow(const char* id, const char* icon, const std::string& label, bool active,
                    bool showPin, float dpiScale, const ColorRGBA* iconTint = nullptr);
    bool SectionHeader(const char* id, const std::string& label, bool& expanded, float dpiScale);
    bool ToolbarIconButton(const char* id, const char* icon, const char* tooltip, float dpiScale,
                           const char* label = nullptr, bool hasDropdown = false, bool enabled = true);

    Win32Window* m_window = nullptr;
    FileScanner* m_scanner = nullptr;
    ThumbnailCache* m_thumbCache = nullptr;
    FileOps* m_fileOps = nullptr;
    std::filesystem::path m_configPath;
    std::function<void(const std::wstring&)> m_onWatchDirectory;
    std::function<void()> m_onReloadConfig;

    Config m_cfg;
    SystemPlacesSnapshot m_places;
    VelaDialogs m_dialogs;

    std::vector<TabState> m_tabs;
    int m_activeTab = 0;

    bool m_editingAddress = false;
    bool m_focusAddressNextFrame = false;
    char m_addressBuffer[1024]{};

    bool m_focusSearchNextFrame = false;
    char m_searchBuffer[256]{};

    bool m_pinnedExpanded = true;
    bool m_drivesExpanded = true;
    bool m_cloudExpanded = true;
    bool m_networkExpanded = false;
    bool m_wslExpanded = true;
    bool m_tagsExpanded = true;

    // Drag & Drop + Clipboard UI state
    std::vector<DropZone> m_dropZones;
    bool m_oleDragActive = false;
    POINT m_oleDragClientPt{};
    std::vector<std::wstring> m_oleDragPathsW;
    int m_hoveredDropZoneIdx = -1;

    bool m_dragCandidateActive = false;
    bool m_deferCollapseSelection = false;
    int m_deferCollapseVisibleIdx = -1;
    float m_dragStartX = 0.0f;
    float m_dragStartY = 0.0f;
    std::vector<std::wstring> m_pendingDragPathsW;

    std::unordered_set<std::string> m_cutPathsLower;
    std::wstring m_pendingSelectPathW;

    std::string m_toastText;
    bool m_toastIsError = false;
    std::chrono::steady_clock::time_point m_toastUntil{};
};

} // namespace vela