#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace vela {

enum class FileOpKind {
    Copy,
    Move,
    Delete,
    Rename,
    CreateFolder,
    CreateFile,
    CreateSymlink,
    CreateJunction,
    CreateHardlink
};

enum class ConflictPolicy {
    Ask,
    Overwrite,
    Skip,
    AutoRename
};

enum class ConflictDecision {
    None,
    Overwrite,
    Skip,
    AutoRename,
    Cancel
};

struct FileOpRequest {
    FileOpKind kind = FileOpKind::Copy;
    std::vector<std::wstring> sources;
    std::wstring destDirW;
    std::wstring newNameW;
    bool permanent = false;
    ConflictPolicy conflictPolicy = ConflictPolicy::Ask;
    HWND ownerHwnd = nullptr;
};

struct FileOpConflict {
    bool active = false;
    std::wstring sourcePathW;
    std::wstring destPathW;
    std::string sourcePath;
    std::string destPath;
    std::string itemName;
    uint64_t sourceSize = 0;
    uint64_t destSize = 0;
    uint64_t sourceTime = 0;
    uint64_t destTime = 0;
    bool isDir = false;
};

struct FileOpProgress {
    bool running = false;
    uint64_t taskId = 0;
    FileOpKind kind = FileOpKind::Copy;
    std::string statusLabel;
    std::string currentItem;
    size_t itemsDone = 0;
    size_t itemsTotal = 0;
    uint64_t bytesDone = 0;
    uint64_t bytesTotal = 0;
    size_t queuedCount = 0;
    FileOpConflict conflict;
};

struct FileOpResult {
    uint64_t taskId = 0;
    FileOpKind kind = FileOpKind::Copy;
    bool success = true;
    bool cancelled = false;
    size_t succeededCount = 0;
    size_t skippedCount = 0;
    size_t failedCount = 0;
    std::wstring targetDirectoryW;
    std::wstring primaryCreatedPathW;
    std::string summaryMessage;
    std::string errorMessage;
};

class FileOps {
public:
    FileOps() = default;
    ~FileOps();

    FileOps(const FileOps&) = delete;
    FileOps& operator=(const FileOps&) = delete;

    void Start(std::function<void()> onNotify);
    void Stop();

    uint64_t Enqueue(FileOpRequest request);
    void RequestCancel();
    void ResolveConflict(ConflictDecision decision, bool applyToAll);

    FileOpProgress GetProgress() const;
    bool TryPopResult(FileOpResult& outResult);

    // Pure filesystem helpers (safe on background threads)
    static std::wstring MakeUniquePath(
        const std::wstring& dirW,
        const std::wstring& desiredNameW,
        bool copySuffixStyle = false
    );
    static bool ReadLinkTarget(
        const std::wstring& pathW,
        std::wstring& outTargetW,
        std::string& outLinkKind
    );

private:
    struct QueuedTask {
        uint64_t id = 0;
        FileOpRequest request;
    };

    void WorkerLoop();
    void ExecuteTask(const QueuedTask& task, FileOpResult& outResult);

    void ExecuteCopyOrMove(const FileOpRequest& req, bool isMove, FileOpResult& outResult);
    void ExecuteDelete(const FileOpRequest& req, FileOpResult& outResult);
    void ExecuteRename(const FileOpRequest& req, FileOpResult& outResult);
    void ExecuteCreateItem(const FileOpRequest& req, bool isFolder, FileOpResult& outResult);
    void ExecuteCreateSymlink(const FileOpRequest& req, FileOpResult& outResult);
    void ExecuteCreateJunction(const FileOpRequest& req, FileOpResult& outResult);
    void ExecuteCreateHardlink(const FileOpRequest& req, FileOpResult& outResult);

    bool CopyFileWithProgress(
        const std::wstring& srcW,
        const std::wstring& dstW,
        bool overwrite,
        uint64_t fileSize,
        std::string& outErr
    );
    bool CopyTree(
        const std::wstring& srcW,
        const std::wstring& dstW,
        bool isMove,
        ConflictPolicy& policy,
        FileOpResult& outResult,
        std::string& outErr
    );
    bool MoveTree(
        const std::wstring& srcW,
        const std::wstring& dstW,
        ConflictPolicy& policy,
        FileOpResult& outResult,
        std::string& outErr
    );
    bool DeleteTreePermanent(const std::wstring& pathW, std::string& outErr);

    ConflictDecision ResolveItemConflict(
        const std::wstring& srcW,
        const std::wstring& dstW,
        bool isDir,
        ConflictPolicy& policy
    );

    void MeasureSources(
        const std::vector<std::wstring>& sources,
        size_t& outItems,
        uint64_t& outBytes
    ) const;
    void MeasureTree(const std::wstring& pathW, size_t& outItems, uint64_t& outBytes) const;

    void UpdateProgressItem(const std::string& itemUtf8);
    void NotifyImmediate();
    void NotifyThrottled();
    bool IsCancelled() const { return m_stopping.load() || m_cancelRequested.load(); }

    static DWORD CALLBACK CopyProgressRoutine(
        LARGE_INTEGER totalFileSize,
        LARGE_INTEGER totalBytesTransferred,
        LARGE_INTEGER streamSize,
        LARGE_INTEGER streamBytesTransferred,
        DWORD streamNumber,
        DWORD callbackReason,
        HANDLE sourceFile,
        HANDLE destinationFile,
        LPVOID data
    );

    std::function<void()> m_onNotify;
    std::thread m_worker;
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<uint64_t> m_nextTaskId{1};

    mutable std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<QueuedTask> m_queue;
    std::deque<FileOpResult> m_results;

    mutable std::mutex m_progressMutex;
    FileOpProgress m_progress;
    uint64_t m_bytesCommitted = 0;

    mutable std::mutex m_conflictMutex;
    std::condition_variable m_conflictCv;
    ConflictDecision m_conflictDecision = ConflictDecision::None;
    bool m_conflictApplyAll = false;

    std::chrono::steady_clock::time_point m_lastNotifyTime{};
};

} // namespace vela