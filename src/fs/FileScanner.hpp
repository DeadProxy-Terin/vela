#pragma once

#include "fs/FileEntry.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace vela {

class FileScanner {
public:
    using CompletionCallback = std::function<void()>;

    FileScanner() = default;
    ~FileScanner();

    FileScanner(const FileScanner&) = delete;
    FileScanner& operator=(const FileScanner&) = delete;

    void Start(CompletionCallback onScanReady);
    void Stop();

    // Enqueues an async scan and returns its generation ID (cancels older in-flight scans)
    uint64_t RequestScan(const std::wstring& directoryW);
    uint64_t GetActiveGeneration() const { return m_activeGeneration.load(); }

    // Called from UI thread to retrieve the newest completed listing
    bool TryPopResult(DirectoryListing& outListing);

    static void SortEntries(std::vector<FileEntry>& entries, SortField field, SortOrder order, bool foldersFirst);

private:
    void WorkerLoop();
    DirectoryListing ScanDirectorySync(const std::wstring& directoryW, uint64_t generation);

    std::thread m_worker;
    std::mutex m_queueMutex;
    std::condition_variable m_cv;
    bool m_running = false;

    struct PendingTask {
        std::wstring directoryW;
        uint64_t generation = 0;
    };
    std::optional<PendingTask> m_pending;

    std::mutex m_resultMutex;
    std::optional<DirectoryListing> m_readyResult;

    std::atomic<uint64_t> m_activeGeneration{0};
    CompletionCallback m_onScanReady;
};

} // namespace vela