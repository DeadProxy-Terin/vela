#include "fs/FileScanner.hpp"
#include "core/StringUtil.hpp"

#include <algorithm>
#include <cctype>
#include <cwchar>

namespace vela {
namespace {

bool IsThumbnailUniqueExtension(std::string_view ext) {
    static constexpr std::string_view kUniqueExts[] = {
        ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".gif", ".tif", ".tiff", ".ico", ".svg",
        ".heic", ".avif", ".mp4", ".mkv", ".mov", ".avi", ".webm", ".wmv",
        ".exe", ".lnk", ".url", ".msi", ".pdf", ".psd", ".ai"
    };
    for (auto candidate : kUniqueExts) {
        if (ext == candidate) return true;
    }
    return false;
}

std::string MakeTypeLabel(bool isDir, std::string_view extLower) {
    if (isDir) return "File folder";
    if (extLower.empty() || extLower.size() == 1) return "File";

    std::string upper(extLower.substr(1));
    std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return upper + " File";
}

std::string MakeThumbKey(bool isDir, DWORD attrs, std::string_view fullPath, std::string_view extLower) {
    if (isDir) {
        // Windows marks folders with custom icons (Desktop, Downloads, etc.) as ReadOnly or System
        if (attrs & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_SYSTEM)) {
            return "path:" + str::ToLowerAscii(fullPath);
        }
        return "__generic_dir__";
    }
    if (IsThumbnailUniqueExtension(extLower)) {
        return "path:" + str::ToLowerAscii(fullPath);
    }
    if (extLower.empty()) {
        return "ext:__none__";
    }
    return std::string("ext:") + std::string(extLower);
}

std::string FormatWin32Error(DWORD err) {
    wchar_t* msgBuf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msgBuf), 0, nullptr
    );
    std::string out;
    if (len > 0 && msgBuf) {
        out = str::WideToUtf8(std::wstring_view(msgBuf, len));
        ::LocalFree(msgBuf);
        while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) {
            out.pop_back();
        }
    }
    if (out.empty()) {
        out = "Failed to open directory (Win32 error " + std::to_string(err) + ")";
    }
    return out;
}

} // namespace

FileScanner::~FileScanner() {
    Stop();
}

void FileScanner::Start(CompletionCallback onScanReady) {
    Stop();
    {
        std::lock_guard lock(m_queueMutex);
        m_onScanReady = std::move(onScanReady);
        m_running = true;
    }
    m_worker = std::thread(&FileScanner::WorkerLoop, this);
}

void FileScanner::Stop() {
    {
        std::lock_guard lock(m_queueMutex);
        if (!m_running) return;
        m_running = false;
        m_pending.reset();
    }
    m_activeGeneration.fetch_add(1);
    m_cv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

uint64_t FileScanner::RequestScan(const std::wstring& directoryW) {
    const uint64_t gen = m_activeGeneration.fetch_add(1) + 1;
    {
        std::lock_guard lock(m_queueMutex);
        m_pending = PendingTask{str::NormalizeWindowsPath(directoryW), gen};
    }
    m_cv.notify_one();
    return gen;
}

bool FileScanner::TryPopResult(DirectoryListing& outListing) {
    std::lock_guard lock(m_resultMutex);
    if (!m_readyResult.has_value()) return false;
    outListing = std::move(*m_readyResult);
    m_readyResult.reset();
    return true;
}

void FileScanner::WorkerLoop() {
    while (true) {
        PendingTask task;
        {
            std::unique_lock lock(m_queueMutex);
            m_cv.wait(lock, [this]() { return !m_running || m_pending.has_value(); });
            if (!m_running) break;
            task = std::move(*m_pending);
            m_pending.reset();
        }

        if (task.generation != m_activeGeneration.load()) {
            continue;
        }

        DirectoryListing result = ScanDirectorySync(task.directoryW, task.generation);

        // Drop stale result if user navigated away mid-scan
        if (result.generation != m_activeGeneration.load()) {
            continue;
        }

        {
            std::lock_guard lock(m_resultMutex);
            m_readyResult = std::move(result);
        }
        if (m_onScanReady) {
            m_onScanReady();
        }
    }
}

DirectoryListing FileScanner::ScanDirectorySync(const std::wstring& directoryW, uint64_t generation) {
    DirectoryListing listing;
    listing.generation = generation;
    listing.directoryW = directoryW;
    listing.directory = str::WideToUtf8(directoryW);

    if (directoryW.empty()) {
        listing.success = false;
        listing.errorMessage = "Empty path.";
        return listing;
    }

    std::wstring searchPattern = directoryW;
    if (searchPattern.back() != L'\\') {
        searchPattern.push_back(L'\\');
    }
    searchPattern.push_back(L'*');

    WIN32_FIND_DATAW ffd{};
    HANDLE hFind = ::FindFirstFileExW(
        searchPattern.c_str(),
        FindExInfoBasic,
        &ffd,
        FindExSearchNameMatch,
        nullptr,
        FIND_FIRST_EX_LARGE_FETCH
    );

    // Fallback without FIND_FIRST_EX_LARGE_FETCH for legacy network providers
    if (hFind == INVALID_HANDLE_VALUE && ::GetLastError() == ERROR_INVALID_PARAMETER) {
        hFind = ::FindFirstFileExW(
            searchPattern.c_str(),
            FindExInfoBasic,
            &ffd,
            FindExSearchNameMatch,
            nullptr,
            0
        );
    }

    if (hFind == INVALID_HANDLE_VALUE) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) {
            listing.success = true;
            return listing;
        }
        listing.success = false;
        listing.errorMessage = FormatWin32Error(err);
        return listing;
    }

    listing.entries.reserve(256);
    const std::wstring basePrefix = (directoryW.back() == L'\\') ? directoryW : (directoryW + L'\\');
    size_t iterations = 0;

    do {
        if ((++iterations & 255u) == 0 && generation != m_activeGeneration.load()) {
            ::FindClose(hFind);
            return listing;
        }

        if (std::wcscmp(ffd.cFileName, L".") == 0 || std::wcscmp(ffd.cFileName, L"..") == 0) {
            continue;
        }

        FileEntry entry;
        entry.nameW = ffd.cFileName;
        entry.name = str::WideToUtf8(entry.nameW);
        entry.nameLower = str::ToLowerAscii(entry.name);
        entry.fullPathW = basePrefix + entry.nameW;
        entry.fullPath = str::WideToUtf8(entry.fullPathW);

        entry.attributes = ffd.dwFileAttributes;
        entry.isDir = (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.isHidden = (ffd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        entry.isSymlink = (ffd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
                          (ffd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || ffd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);

        if (!entry.isDir) {
            const size_t dot = entry.name.find_last_of('.');
            if (dot != std::string::npos && dot > 0 && dot + 1 < entry.name.size()) {
                entry.extensionLower = str::ToLowerAscii(std::string_view(entry.name).substr(dot));
                entry.displayName = entry.name.substr(0, dot);
            } else {
                entry.displayName = entry.name;
            }

            ULARGE_INTEGER sz{};
            sz.LowPart = ffd.nFileSizeLow;
            sz.HighPart = ffd.nFileSizeHigh;
            entry.sizeBytes = sz.QuadPart;
            entry.sizeFormatted = str::FormatFileSize(entry.sizeBytes);
        } else {
            entry.displayName = entry.name;
        }

        ULARGE_INTEGER wt{};
        wt.LowPart = ffd.ftLastWriteTime.dwLowDateTime;
        wt.HighPart = ffd.ftLastWriteTime.dwHighDateTime;
        entry.lastWriteTime = wt.QuadPart;
        entry.dateFormatted = str::FormatFileTime(entry.lastWriteTime);

        entry.typeLabel = MakeTypeLabel(entry.isDir, entry.extensionLower);
        entry.thumbKey = MakeThumbKey(entry.isDir, entry.attributes, entry.fullPath, entry.extensionLower);

        listing.entries.push_back(std::move(entry));
    } while (::FindNextFileW(hFind, &ffd));

    ::FindClose(hFind);
    return listing;
}

void FileScanner::SortEntries(std::vector<FileEntry>& entries, SortField field, SortOrder order, bool foldersFirst) {
    const bool asc = (order == SortOrder::Ascending);

    std::sort(entries.begin(), entries.end(), [&](const FileEntry& a, const FileEntry& b) {
        if (foldersFirst && a.isDir != b.isDir) {
            return a.isDir;
        }

        int cmp = 0;
        switch (field) {
            case SortField::Name:
                cmp = str::NaturalCompareWide(a.nameW.c_str(), b.nameW.c_str());
                break;

            case SortField::DateModified:
                if (a.lastWriteTime < b.lastWriteTime) cmp = -1;
                else if (a.lastWriteTime > b.lastWriteTime) cmp = 1;
                else cmp = str::NaturalCompareWide(a.nameW.c_str(), b.nameW.c_str());
                break;

            case SortField::Type:
                cmp = _stricmp(a.typeLabel.c_str(), b.typeLabel.c_str());
                if (cmp == 0) cmp = str::NaturalCompareWide(a.nameW.c_str(), b.nameW.c_str());
                break;

            case SortField::Size:
                if (a.sizeBytes < b.sizeBytes) cmp = -1;
                else if (a.sizeBytes > b.sizeBytes) cmp = 1;
                else cmp = str::NaturalCompareWide(a.nameW.c_str(), b.nameW.c_str());
                break;
        }

        return asc ? (cmp < 0) : (cmp > 0);
    });
}

} // namespace vela