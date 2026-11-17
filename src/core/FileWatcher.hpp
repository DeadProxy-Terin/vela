#pragma once

#include <windows.h>
#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <thread>

namespace vela {

class FileWatcher {
public:
    using Callback = std::function<void()>;

    FileWatcher() = default;
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    // Watch a specific file (e.g., config/vela.lua)
    void StartFile(const std::filesystem::path& filePath, Callback onChanged);

    // Watch any file/folder additions, deletions, renames, or size writes inside a directory
    void StartDirectory(const std::filesystem::path& dirPath, Callback onChanged);

    void Stop();

private:
    void WatchLoop(std::filesystem::path directory, std::wstring targetFilename, DWORD debounceMs, Callback onChanged);

    HANDLE m_stopEvent = nullptr;
    std::thread m_thread;
    std::atomic<bool> m_running{false};
};

} // namespace vela