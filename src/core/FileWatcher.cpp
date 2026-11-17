#include "core/FileWatcher.hpp"

#include <vector>

namespace vela {

FileWatcher::~FileWatcher() {
    Stop();
}

void FileWatcher::StartFile(const std::filesystem::path& filePath, Callback onChanged) {
    Stop();

    std::error_code ec;
    const auto absPath = std::filesystem::absolute(filePath, ec);
    if (ec || absPath.parent_path().empty()) return;

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) return;

    m_running = true;
    m_thread = std::thread(
        &FileWatcher::WatchLoop, this,
        absPath.parent_path(), absPath.filename().wstring(), 45, std::move(onChanged)
    );
}

void FileWatcher::StartDirectory(const std::filesystem::path& dirPath, Callback onChanged) {
    Stop();

    if (dirPath.empty()) return;

    m_stopEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!m_stopEvent) return;

    m_running = true;
    m_thread = std::thread(
        &FileWatcher::WatchLoop, this,
        dirPath, std::wstring(), 140, std::move(onChanged)
    );
}

void FileWatcher::Stop() {
    if (!m_running.exchange(false)) {
        return;
    }
    if (m_stopEvent) {
        ::SetEvent(m_stopEvent);
    }
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_stopEvent) {
        ::CloseHandle(m_stopEvent);
        m_stopEvent = nullptr;
    }
}

void FileWatcher::WatchLoop(std::filesystem::path directory, std::wstring targetFilename, DWORD debounceMs, Callback onChanged) {
    HANDLE dirHandle = ::CreateFileW(
        directory.c_str(),
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        nullptr
    );

    if (dirHandle == INVALID_HANDLE_VALUE) {
        return;
    }

    HANDLE ioEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ioEvent) {
        ::CloseHandle(dirHandle);
        return;
    }

    const DWORD notifyFilter = targetFilename.empty()
        ? (FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
           FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES)
        : (FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE);

    std::vector<BYTE> buffer(16384);
    ULONGLONG lastTriggerTick = 0;

    while (m_running.load()) {
        OVERLAPPED overlapped{};
        overlapped.hEvent = ioEvent;
        ::ResetEvent(ioEvent);

        DWORD bytesReturned = 0;
        const BOOL ok = ::ReadDirectoryChangesW(
            dirHandle,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            FALSE,
            notifyFilter,
            &bytesReturned,
            &overlapped,
            nullptr
        );

        if (!ok && ::GetLastError() != ERROR_IO_PENDING) {
            break;
        }

        HANDLE handles[2] = {m_stopEvent, ioEvent};
        const DWORD wait = ::WaitForMultipleObjects(2, handles, FALSE, INFINITE);

        if (wait == WAIT_OBJECT_0 || !m_running.load()) {
            ::CancelIo(dirHandle);
            DWORD dummy = 0;
            ::GetOverlappedResult(dirHandle, &overlapped, &dummy, TRUE);
            break;
        }

        if (wait == WAIT_OBJECT_0 + 1) {
            DWORD transferred = 0;
            if (::GetOverlappedResult(dirHandle, &overlapped, &transferred, FALSE)) {
                bool matched = targetFilename.empty();
                if (!matched && transferred > 0) {
                    size_t offset = 0;
                    while (true) {
                        const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                        const std::wstring changedName(info->FileName, info->FileNameLength / sizeof(WCHAR));
                        if (_wcsicmp(changedName.c_str(), targetFilename.c_str()) == 0) {
                            matched = true;
                            break;
                        }
                        if (info->NextEntryOffset == 0) break;
                        offset += info->NextEntryOffset;
                    }
                }

                if (matched) {
                    // Wait briefly on stop event during debounce so Stop() remains instantaneous
                    if (::WaitForSingleObject(m_stopEvent, debounceMs) == WAIT_OBJECT_0 || !m_running.load()) {
                        break;
                    }
                    const ULONGLONG now = ::GetTickCount64();
                    if (now - lastTriggerTick >= debounceMs) {
                        lastTriggerTick = now;
                        if (onChanged) onChanged();
                    }
                }
            }
        }
    }

    ::CloseHandle(ioEvent);
    ::CloseHandle(dirHandle);
}

} // namespace vela