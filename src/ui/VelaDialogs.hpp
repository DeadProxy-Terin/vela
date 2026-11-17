#pragma once

#include "core/Config.hpp"
#include "fs/FileOps.hpp"
#include "platform/Win32Window.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vela {

class VelaDialogs {
public:
    VelaDialogs() = default;
    ~VelaDialogs();

    VelaDialogs(const VelaDialogs&) = delete;
    VelaDialogs& operator=(const VelaDialogs&) = delete;

    void Initialize(
        Win32Window* window,
        FileOps* fileOps,
        std::function<void(bool isError, const std::string& message)> onToast
    );
    void Shutdown();

    bool IsAnyModalOpen() const;

    void OpenNewItem(bool isFolder, const std::wstring& destDirW);
    void OpenRename(const std::wstring& itemPathW, bool isDir);
    void OpenDeleteConfirm(std::vector<std::wstring> pathsW, bool permanent);
    void OpenProperties(std::vector<std::wstring> targetPathsW);
    void OpenDragDropPopup(std::vector<std::wstring> sourcesW, std::wstring destDirW);

    void Render(const Config& cfg, float dpiScale);

private:
    struct PropertiesState {
        std::vector<std::wstring> targetsW;
        std::string title;
        std::string location;
        std::string typeLabel;
        std::string linkKind;
        std::string linkTarget;
        std::string createdText;
        std::string modifiedText;
        std::string accessedText;
        DWORD attributes = 0;

        bool isSingleFile = false;
        bool isMulti = false;

        bool calculatingStats = false;
        size_t fileCount = 0;
        size_t folderCount = 0;
        uint64_t totalBytes = 0;

        bool computingHash = false;
        float hashProgress = 0.0f;
        std::string sha256Hex;
        std::string hashError;
    };

    void StopBgWorker();
    void StartFolderStatsWorker(std::vector<std::wstring> pathsW);
    void StartSha256Worker(std::wstring filePathW);

    void RenderProgressOverlay(const Config& cfg, const FileOpProgress& prog, float dpiScale);
    void RenderConflictModal(const Config& cfg, const FileOpProgress& prog, float dpiScale);
    void RenderNewItemModal(const Config& cfg, float dpiScale);
    void RenderRenameModal(const Config& cfg, float dpiScale);
    void RenderDeleteModal(const Config& cfg, float dpiScale);
    void RenderPropertiesModal(const Config& cfg, float dpiScale);
    void RenderDropPopup(const Config& cfg);

    Win32Window* m_window = nullptr;
    FileOps* m_fileOps = nullptr;
    std::function<void(bool, const std::string&)> m_onToast;

    // New Item dialog
    bool m_openNewItemPending = false;
    bool m_newItemOpen = false;
    bool m_newItemIsFolder = true;
    bool m_newItemFocus = false;
    std::wstring m_newItemDirW;
    char m_newItemBuf[512]{};

    // Rename dialog
    bool m_openRenamePending = false;
    bool m_renameOpen = false;
    bool m_renameIsDir = false;
    bool m_renameFocus = false;
    std::wstring m_renameTargetW;
    char m_renameBuf[512]{};

    // Delete confirm dialog
    bool m_openDeletePending = false;
    bool m_deleteOpen = false;
    bool m_deletePermanent = false;
    std::vector<std::wstring> m_deletePathsW;

    // Conflict modal tracking
    bool m_conflictModalOpen = false;
    bool m_conflictApplyAll = false;

    // Properties modal
    bool m_openPropsPending = false;
    bool m_propsOpen = false;
    mutable std::mutex m_propMutex;
    PropertiesState m_propState;
    std::thread m_bgThread;
    std::atomic<bool> m_bgCancel{false};

    // Thunar Drag & Drop Action Popup
    bool m_openDropPopupPending = false;
    bool m_dropPopupOpen = false;
    std::vector<std::wstring> m_dropSourcesW;
    std::wstring m_dropDestDirW;
    std::string m_dropHeader;
    bool m_dropAllLocalDirs = false;
    bool m_dropAllFilesSameDrive = false;
};

} // namespace vela