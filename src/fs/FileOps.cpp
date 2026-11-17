#include "fs/FileOps.hpp"
#include "core/StringUtil.hpp"

#include <winioctl.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#ifndef COPY_FILE_COPY_SYMLINK
#define COPY_FILE_COPY_SYMLINK 0x00000800
#endif

namespace vela {
namespace {

constexpr DWORD kReparseTagMountPoint = 0xA0000003u;
constexpr DWORD kReparseTagSymlink    = 0xA000000Cu;
constexpr DWORD kMaxReparseBufferSize = 16384u;

// Explicit GUIDs avoid MinGW64 libuuid missing-symbol quirks for newer Shell CLSIDs
constexpr CLSID kClsidFileOperation = {
    0x3AD05575, 0x8857, 0x4850, {0x92, 0x77, 0x11, 0xB8, 0x5B, 0xDB, 0x8E, 0x09}
};
constexpr IID kIidIFileOperation = {
    0x947AAB5F, 0x0A5C, 0x4C13, {0xB4, 0xD6, 0x4B, 0xF7, 0x83, 0x6F, 0xC9, 0xF8}
};
constexpr IID kIidIShellItem = {
    0x43826D1E, 0xE718, 0x42EE, {0xBC, 0x55, 0xA1, 0xE2, 0x61, 0xC3, 0x7B, 0xFE}
};

struct ReparseHeader {
    ULONG ReparseTag;
    USHORT ReparseDataLength;
    USHORT Reserved;
};

struct MountPointFields {
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
};

struct SymlinkFields {
    USHORT SubstituteNameOffset;
    USHORT SubstituteNameLength;
    USHORT PrintNameOffset;
    USHORT PrintNameLength;
    ULONG Flags;
};

struct CopyCallbackContext {
    FileOps* owner = nullptr;
    uint64_t baseBytes = 0;
    uint64_t currentFileBytes = 0;
    uint64_t* outLastReported = nullptr;
};

std::string FormatWin32Error(DWORD err) {
    if (err == 0) return {};
    wchar_t* msgBuf = nullptr;
    const DWORD len = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&msgBuf),
        0,
        nullptr
    );
    std::string out;
    if (len > 0 && msgBuf) {
        std::wstring w(msgBuf, len);
        while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) {
            w.pop_back();
        }
        out = str::WideToUtf8(w);
        ::LocalFree(msgBuf);
    } else {
        out = "Win32 error " + std::to_string(err);
    }
    return out;
}

std::wstring JoinWinPath(const std::wstring& dirW, const std::wstring& childW) {
    if (dirW.empty()) return childW;
    if (childW.empty()) return dirW;
    std::wstring out = dirW;
    if (out.back() != L'\\' && out.back() != L'/') {
        out.push_back(L'\\');
    }
    out.append(childW);
    return out;
}

std::wstring GetFileNamePart(const std::wstring& pathW) {
    std::wstring clean = str::NormalizeWindowsPath(pathW);
    while (clean.size() > 3 && clean.back() == L'\\') {
        clean.pop_back();
    }
    const size_t pos = clean.find_last_of(L'\\');
    if (pos == std::wstring::npos) return clean;
    if (pos + 1 >= clean.size()) return clean;
    return clean.substr(pos + 1);
}

bool PathExists(const std::wstring& pathW) {
    return ::GetFileAttributesW(pathW.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool QueryMeta(const std::wstring& pathW, uint64_t& outSize, uint64_t& outTime, bool& outIsDir, DWORD& outAttrs) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!::GetFileAttributesExW(pathW.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    outAttrs = fad.dwFileAttributes;
    outIsDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    outSize = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
    outTime = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) | fad.ftLastWriteTime.dwLowDateTime;
    return true;
}

bool IsSamePath(const std::wstring& aW, const std::wstring& bW) {
    const std::wstring na = str::NormalizeWindowsPath(aW);
    const std::wstring nb = str::NormalizeWindowsPath(bW);
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

bool IsSubpathOrEqual(const std::wstring& parentW, const std::wstring& candidateW) {
    std::wstring p = str::NormalizeWindowsPath(parentW);
    std::wstring c = str::NormalizeWindowsPath(candidateW);
    if (p.empty() || c.empty()) return false;
    if (_wcsicmp(p.c_str(), c.c_str()) == 0) return true;
    if (p.back() != L'\\') p.push_back(L'\\');
    if (c.size() <= p.size()) return false;
    return _wcsnicmp(c.c_str(), p.c_str(), p.size()) == 0;
}

void ClearReadOnlyIfSet(const std::wstring& pathW) {
    const DWORD attrs = ::GetFileAttributesW(pathW.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY)) {
        ::SetFileAttributesW(pathW.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    }
}

std::wstring StripNtPrefix(std::wstring pathW) {
    constexpr std::wstring_view kNtPrefix = L"\\??\\";
    constexpr std::wstring_view kUncNtPrefix = L"\\??\\UNC\\";
    if (pathW.rfind(kUncNtPrefix, 0) == 0) {
        return L"\\\\" + pathW.substr(kUncNtPrefix.size());
    }
    if (pathW.rfind(kNtPrefix, 0) == 0) {
        return pathW.substr(kNtPrefix.size());
    }
    return pathW;
}

bool DeleteItemsViaShell(const std::vector<std::wstring>& sources, bool toRecycleBin, std::string& outErr) {
    if (sources.empty()) return true;

    // 1. Prefer modern IFileOperation COM interface
    Microsoft::WRL::ComPtr<IFileOperation> pfo;
    HRESULT hr = ::CoCreateInstance(
        kClsidFileOperation,
        nullptr,
        CLSCTX_ALL,
        kIidIFileOperation,
        reinterpret_cast<void**>(pfo.GetAddressOf())
    );
    if (SUCCEEDED(hr) && pfo) {
        DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
        if (toRecycleBin) {
            flags |= FOF_ALLOWUNDO;
        }
        pfo->SetOperationFlags(flags);

        bool addedAny = false;
        for (const auto& srcW : sources) {
            Microsoft::WRL::ComPtr<IShellItem> item;
            hr = ::SHCreateItemFromParsingName(
                srcW.c_str(), nullptr, kIidIShellItem, reinterpret_cast<void**>(item.GetAddressOf())
            );
            if (FAILED(hr) || !item) {
                outErr = "Failed to bind shell item: " + str::WideToUtf8(srcW);
                return false;
            }
            hr = pfo->DeleteItem(item.Get(), nullptr);
            if (FAILED(hr)) {
                outErr = "Failed to queue delete operation.";
                return false;
            }
            addedAny = true;
        }

        if (addedAny) {
            hr = pfo->PerformOperations();
            if (SUCCEEDED(hr)) {
                BOOL aborted = FALSE;
                pfo->GetAnyOperationsAborted(&aborted);
                if (!aborted) {
                    return true;
                }
                outErr = "Recycle Bin operation was aborted.";
                return false;
            }
        }
    }

    // 2. Fallback to SHFileOperationW (double-null-terminated list)
    std::wstring multiSz;
    for (const auto& srcW : sources) {
        multiSz.append(srcW);
        multiSz.push_back(L'\0');
    }
    multiSz.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = multiSz.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | (toRecycleBin ? FOF_ALLOWUNDO : 0);

    const int res = ::SHFileOperationW(&op);
    if (res == 0 && !op.fAnyOperationsAborted) {
        return true;
    }
    outErr = toRecycleBin
        ? "Cannot move item(s) to Recycle Bin (use Shift+Delete for permanent delete)."
        : "Shell delete failed (code " + std::to_string(res) + ").";
    return false;
}

const char* KindToVerb(FileOpKind kind) {
    switch (kind) {
        case FileOpKind::Copy:           return "Copying";
        case FileOpKind::Move:           return "Moving";
        case FileOpKind::Delete:         return "Deleting";
        case FileOpKind::Rename:         return "Renaming";
        case FileOpKind::CreateFolder:   return "Creating folder";
        case FileOpKind::CreateFile:     return "Creating file";
        case FileOpKind::CreateSymlink:  return "Creating symbolic link";
        case FileOpKind::CreateJunction: return "Creating junction";
        case FileOpKind::CreateHardlink: return "Creating hard link";
    }
    return "Working";
}

} // namespace

FileOps::~FileOps() {
    Stop();
}

void FileOps::Start(std::function<void()> onNotify) {
    if (m_worker.joinable()) return;
    m_onNotify = std::move(onNotify);
    m_stopping = false;
    m_cancelRequested = false;
    m_worker = std::thread(&FileOps::WorkerLoop, this);
}

void FileOps::Stop() {
    m_stopping = true;
    m_cancelRequested = true;
    {
        std::lock_guard<std::mutex> lock(m_conflictMutex);
        m_conflictDecision = ConflictDecision::Cancel;
    }
    m_conflictCv.notify_all();
    m_queueCv.notify_all();
    if (m_worker.joinable()) {
        m_worker.join();
    }
}

uint64_t FileOps::Enqueue(FileOpRequest request) {
    const uint64_t id = m_nextTaskId.fetch_add(1);
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.push_back(QueuedTask{id, std::move(request)});
    }
    m_queueCv.notify_one();
    NotifyImmediate();
    return id;
}

void FileOps::RequestCancel() {
    m_cancelRequested = true;
    {
        std::lock_guard<std::mutex> lock(m_conflictMutex);
        if (m_conflictDecision == ConflictDecision::None) {
            m_conflictDecision = ConflictDecision::Cancel;
        }
    }
    m_conflictCv.notify_all();
}

void FileOps::ResolveConflict(ConflictDecision decision, bool applyToAll) {
    {
        std::lock_guard<std::mutex> lock(m_conflictMutex);
        m_conflictDecision = decision;
        m_conflictApplyAll = applyToAll;
    }
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.conflict.active = false;
    }
    m_conflictCv.notify_one();
    NotifyImmediate();
}

FileOpProgress FileOps::GetProgress() const {
    std::lock_guard<std::mutex> pLock(m_progressMutex);
    FileOpProgress snap = m_progress;
    {
        std::lock_guard<std::mutex> qLock(m_queueMutex);
        snap.queuedCount = m_queue.size();
    }
    return snap;
}

bool FileOps::TryPopResult(FileOpResult& outResult) {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    if (m_results.empty()) return false;
    outResult = std::move(m_results.front());
    m_results.pop_front();
    return true;
}

std::wstring FileOps::MakeUniquePath(
    const std::wstring& dirW,
    const std::wstring& desiredNameW,
    bool copySuffixStyle
) {
    const std::wstring first = JoinWinPath(dirW, desiredNameW);
    if (!PathExists(first)) return first;

    std::wstring stem = desiredNameW;
    std::wstring ext;
    const size_t dot = desiredNameW.find_last_of(L'.');
    const bool isExistingDir = (::GetFileAttributesW(first.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!isExistingDir && dot != std::wstring::npos && dot > 0) {
        stem = desiredNameW.substr(0, dot);
        ext = desiredNameW.substr(dot);
    }

    if (copySuffixStyle) {
        const std::wstring copy1 = JoinWinPath(dirW, stem + L" - Copy" + ext);
        if (!PathExists(copy1)) return copy1;

        for (int n = 2; n < 10000; ++n) {
            const std::wstring candidate = JoinWinPath(
                dirW, stem + L" - Copy (" + std::to_wstring(n) + L")" + ext
            );
            if (!PathExists(candidate)) return candidate;
        }
    } else {
        for (int n = 2; n < 10000; ++n) {
            const std::wstring candidate = JoinWinPath(
                dirW, stem + L" (" + std::to_wstring(n) + L")" + ext
            );
            if (!PathExists(candidate)) return candidate;
        }
    }
    return first;
}

bool FileOps::ReadLinkTarget(
    const std::wstring& pathW,
    std::wstring& outTargetW,
    std::string& outLinkKind
) {
    outTargetW.clear();
    outLinkKind.clear();

    const DWORD attrs = ::GetFileAttributesW(pathW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        return false;
    }

    HANDLE h = ::CreateFileW(
        pathW.c_str(),
        FILE_READ_EA,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }

    std::vector<uint8_t> buf(kMaxReparseBufferSize, 0);
    DWORD bytesReturned = 0;
    const BOOL ok = ::DeviceIoControl(
        h,
        FSCTL_GET_REPARSE_POINT,
        nullptr,
        0,
        buf.data(),
        static_cast<DWORD>(buf.size()),
        &bytesReturned,
        nullptr
    );
    ::CloseHandle(h);
    if (!ok || bytesReturned < sizeof(ReparseHeader)) {
        return false;
    }

    ReparseHeader hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(ReparseHeader));

    auto extractWide = [&](size_t pathBufOffset, USHORT offsetBytes, USHORT lenBytes) -> std::wstring {
        if (lenBytes < sizeof(wchar_t)) return {};
        const size_t start = pathBufOffset + offsetBytes;
        if (start + lenBytes > bytesReturned) return {};
        std::wstring s(lenBytes / sizeof(wchar_t), L'\0');
        std::memcpy(s.data(), buf.data() + start, s.size() * sizeof(wchar_t));
        return s;
    };

    if (hdr.ReparseTag == kReparseTagMountPoint) {
        if (bytesReturned < sizeof(ReparseHeader) + sizeof(MountPointFields)) return false;
        MountPointFields fields{};
        std::memcpy(&fields, buf.data() + sizeof(ReparseHeader), sizeof(MountPointFields));
        const size_t pathBufOffset = sizeof(ReparseHeader) + sizeof(MountPointFields);

        std::wstring target = extractWide(pathBufOffset, fields.PrintNameOffset, fields.PrintNameLength);
        if (target.empty()) {
            target = StripNtPrefix(extractWide(pathBufOffset, fields.SubstituteNameOffset, fields.SubstituteNameLength));
        }
        outTargetW = target;
        outLinkKind = "Directory Junction";
        return !outTargetW.empty();
    }

    if (hdr.ReparseTag == kReparseTagSymlink) {
        if (bytesReturned < sizeof(ReparseHeader) + sizeof(SymlinkFields)) return false;
        SymlinkFields fields{};
        std::memcpy(&fields, buf.data() + sizeof(ReparseHeader), sizeof(SymlinkFields));
        const size_t pathBufOffset = sizeof(ReparseHeader) + sizeof(SymlinkFields);

        std::wstring target = extractWide(pathBufOffset, fields.PrintNameOffset, fields.PrintNameLength);
        if (target.empty()) {
            target = StripNtPrefix(extractWide(pathBufOffset, fields.SubstituteNameOffset, fields.SubstituteNameLength));
        }
        outTargetW = target;
        outLinkKind = (attrs & FILE_ATTRIBUTE_DIRECTORY)
            ? "Symbolic Link (Directory)"
            : "Symbolic Link (File)";
        return !outTargetW.empty();
    }

    return false;
}

void FileOps::WorkerLoop() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    while (!m_stopping.load()) {
        QueuedTask task;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [&]() {
                return m_stopping.load() || !m_queue.empty();
            });
            if (m_stopping.load()) break;
            task = std::move(m_queue.front());
            m_queue.pop_front();
        }

        m_cancelRequested = false;
        m_bytesCommitted = 0;
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress = FileOpProgress{};
            m_progress.running = true;
            m_progress.taskId = task.id;
            m_progress.kind = task.request.kind;
            m_progress.statusLabel = KindToVerb(task.request.kind);
        }
        NotifyImmediate();

        FileOpResult result;
        result.taskId = task.id;
        result.kind = task.request.kind;
        ExecuteTask(task, result);

        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.running = false;
            m_progress.conflict = FileOpConflict{};
        }
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_results.push_back(std::move(result));
        }
        NotifyImmediate();
    }

    ::CoUninitialize();
}

void FileOps::ExecuteTask(const QueuedTask& task, FileOpResult& outResult) {
    const FileOpRequest& req = task.request;
    switch (req.kind) {
        case FileOpKind::Copy:
            ExecuteCopyOrMove(req, false, outResult);
            break;
        case FileOpKind::Move:
            ExecuteCopyOrMove(req, true, outResult);
            break;
        case FileOpKind::Delete:
            ExecuteDelete(req, outResult);
            break;
        case FileOpKind::Rename:
            ExecuteRename(req, outResult);
            break;
        case FileOpKind::CreateFolder:
            ExecuteCreateItem(req, true, outResult);
            break;
        case FileOpKind::CreateFile:
            ExecuteCreateItem(req, false, outResult);
            break;
        case FileOpKind::CreateSymlink:
            ExecuteCreateSymlink(req, outResult);
            break;
        case FileOpKind::CreateJunction:
            ExecuteCreateJunction(req, outResult);
            break;
        case FileOpKind::CreateHardlink:
            ExecuteCreateHardlink(req, outResult);
            break;
    }
}

void FileOps::MeasureTree(const std::wstring& pathW, size_t& outItems, uint64_t& outBytes) const {
    if (IsCancelled()) return;

    uint64_t size = 0;
    uint64_t time = 0;
    bool isDir = false;
    DWORD attrs = 0;
    if (!QueryMeta(pathW, size, time, isDir, attrs)) return;

    ++outItems;
    if (!isDir) {
        outBytes += size;
        return;
    }

    // Never recurse into directory junctions or symlinks
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        return;
    }

    const std::wstring pattern = JoinWinPath(pathW, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileExW(
        pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
    );
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (IsCancelled()) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring childW = JoinWinPath(pathW, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            MeasureTree(childW, outItems, outBytes);
        } else {
            ++outItems;
            outBytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        }
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);
}

void FileOps::MeasureSources(
    const std::vector<std::wstring>& sources,
    size_t& outItems,
    uint64_t& outBytes
) const {
    outItems = 0;
    outBytes = 0;
    for (const auto& srcW : sources) {
        if (IsCancelled()) break;
        MeasureTree(srcW, outItems, outBytes);
    }
}

void FileOps::UpdateProgressItem(const std::string& itemUtf8) {
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.currentItem = itemUtf8;
    }
    NotifyThrottled();
}

void FileOps::NotifyImmediate() {
    m_lastNotifyTime = std::chrono::steady_clock::now();
    if (m_onNotify) m_onNotify();
}

void FileOps::NotifyThrottled() {
    const auto now = std::chrono::steady_clock::now();
    if (now - m_lastNotifyTime >= std::chrono::milliseconds(50)) {
        m_lastNotifyTime = now;
        if (m_onNotify) m_onNotify();
    }
}

DWORD CALLBACK FileOps::CopyProgressRoutine(
    LARGE_INTEGER /*totalFileSize*/,
    LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER /*streamSize*/,
    LARGE_INTEGER /*streamBytesTransferred*/,
    DWORD /*streamNumber*/,
    DWORD /*callbackReason*/,
    HANDLE /*sourceFile*/,
    HANDLE /*destinationFile*/,
    LPVOID data
) {
    auto* ctx = static_cast<CopyCallbackContext*>(data);
    if (!ctx || !ctx->owner) return PROGRESS_CONTINUE;

    if (ctx->owner->IsCancelled()) {
        return PROGRESS_CANCEL;
    }

    const uint64_t transferred = static_cast<uint64_t>(totalBytesTransferred.QuadPart);
    if (ctx->outLastReported) {
        *ctx->outLastReported = transferred;
    }
    {
        std::lock_guard<std::mutex> lock(ctx->owner->m_progressMutex);
        ctx->owner->m_progress.bytesDone = ctx->baseBytes + transferred;
    }
    ctx->owner->NotifyThrottled();
    return PROGRESS_CONTINUE;
}

ConflictDecision FileOps::ResolveItemConflict(
    const std::wstring& srcW,
    const std::wstring& dstW,
    bool isDir,
    ConflictPolicy& policy
) {
    if (IsCancelled()) return ConflictDecision::Cancel;
    switch (policy) {
        case ConflictPolicy::Overwrite:  return ConflictDecision::Overwrite;
        case ConflictPolicy::Skip:       return ConflictDecision::Skip;
        case ConflictPolicy::AutoRename: return ConflictDecision::AutoRename;
        case ConflictPolicy::Ask:        break;
    }

    uint64_t sSize = 0, sTime = 0, dSize = 0, dTime = 0;
    bool sDir = isDir, dDir = isDir;
    DWORD sAttrs = 0, dAttrs = 0;
    QueryMeta(srcW, sSize, sTime, sDir, sAttrs);
    QueryMeta(dstW, dSize, dTime, dDir, dAttrs);

    {
        std::lock_guard<std::mutex> lock(m_conflictMutex);
        m_conflictDecision = ConflictDecision::None;
        m_conflictApplyAll = false;
    }
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        FileOpConflict& c = m_progress.conflict;
        c.active = true;
        c.sourcePathW = srcW;
        c.destPathW = dstW;
        c.sourcePath = str::WideToUtf8(srcW);
        c.destPath = str::WideToUtf8(dstW);
        c.itemName = str::WideToUtf8(GetFileNamePart(dstW));
        c.sourceSize = sSize;
        c.destSize = dSize;
        c.sourceTime = sTime;
        c.destTime = dTime;
        c.isDir = dDir;
    }
    NotifyImmediate();

    ConflictDecision chosen = ConflictDecision::Cancel;
    bool applyAll = false;
    {
        std::unique_lock<std::mutex> lock(m_conflictMutex);
        m_conflictCv.wait(lock, [&]() {
            return IsCancelled() || m_conflictDecision != ConflictDecision::None;
        });
        chosen = IsCancelled() ? ConflictDecision::Cancel : m_conflictDecision;
        applyAll = m_conflictApplyAll;
        m_conflictDecision = ConflictDecision::None;
    }

    if (applyAll) {
        if (chosen == ConflictDecision::Overwrite) policy = ConflictPolicy::Overwrite;
        else if (chosen == ConflictDecision::Skip) policy = ConflictPolicy::Skip;
        else if (chosen == ConflictDecision::AutoRename) policy = ConflictPolicy::AutoRename;
    }
    return chosen;
}

bool FileOps::CopyFileWithProgress(
    const std::wstring& srcW,
    const std::wstring& dstW,
    bool overwrite,
    uint64_t fileSize,
    std::string& outErr
) {
    if (overwrite && PathExists(dstW)) {
        ClearReadOnlyIfSet(dstW);
    }

    uint64_t lastReported = 0;
    CopyCallbackContext ctx{this, m_bytesCommitted, fileSize, &lastReported};
    BOOL cancelFlag = FALSE;

    DWORD flags = overwrite ? 0 : COPY_FILE_FAIL_IF_EXISTS;
    const DWORD attrs = ::GetFileAttributesW(srcW.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        flags |= COPY_FILE_COPY_SYMLINK;
    }

    BOOL ok = ::CopyFileExW(
        srcW.c_str(),
        dstW.c_str(),
        &FileOps::CopyProgressRoutine,
        &ctx,
        &cancelFlag,
        flags
    );
    if (!ok) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_REQUEST_ABORTED || IsCancelled()) {
            outErr = "Operation cancelled.";
            return false;
        }
        outErr = FormatWin32Error(err);
        return false;
    }

    m_bytesCommitted += fileSize;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsDone++;
        m_progress.bytesDone = m_bytesCommitted;
    }
    NotifyThrottled();
    return true;
}

bool FileOps::CopyTree(
    const std::wstring& srcW,
    const std::wstring& dstW,
    bool isMove,
    ConflictPolicy& policy,
    FileOpResult& outResult,
    std::string& outErr
) {
    if (IsCancelled()) return false;

    uint64_t size = 0, time = 0;
    bool isDir = false;
    DWORD attrs = 0;
    if (!QueryMeta(srcW, size, time, isDir, attrs)) {
        outErr = "Source item not found: " + str::WideToUtf8(srcW);
        return false;
    }

    const std::string itemUtf8 = str::WideToUtf8(GetFileNamePart(srcW));
    UpdateProgressItem(itemUtf8);

    std::wstring targetPathW = dstW;
    if (PathExists(targetPathW)) {
        const DWORD dstAttrs = ::GetFileAttributesW(targetPathW.c_str());
        const bool dstIsDir = (dstAttrs != INVALID_FILE_ATTRIBUTES) && (dstAttrs & FILE_ATTRIBUTE_DIRECTORY);

        // Merging folders or resolving file conflicts
        const ConflictDecision decision = ResolveItemConflict(srcW, targetPathW, dstIsDir, policy);
        if (decision == ConflictDecision::Cancel) {
            outResult.cancelled = true;
            return false;
        }
        if (decision == ConflictDecision::Skip) {
            outResult.skippedCount++;
            m_bytesCommitted += size;
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.itemsDone++;
            m_progress.bytesDone = m_bytesCommitted;
            return true;
        }
        if (decision == ConflictDecision::AutoRename) {
            targetPathW = MakeUniquePath(
                str::GetParentWindowsPath(targetPathW),
                GetFileNamePart(targetPathW),
                false
            );
        }
    }

    if (!isDir) {
        if (!CopyFileWithProgress(srcW, targetPathW, true, size, outErr)) {
            return false;
        }
        if (outResult.primaryCreatedPathW.empty()) {
            outResult.primaryCreatedPathW = targetPathW;
        }
        if (isMove) {
            ClearReadOnlyIfSet(srcW);
            ::DeleteFileW(srcW.c_str());
        }
        return true;
    }

    // Create destination directory if missing
    if (!PathExists(targetPathW)) {
        if (!::CreateDirectoryW(targetPathW.c_str(), nullptr)) {
            const DWORD err = ::GetLastError();
            if (err != ERROR_ALREADY_EXISTS) {
                outErr = FormatWin32Error(err);
                return false;
            }
        }
    }
    if (outResult.primaryCreatedPathW.empty()) {
        outResult.primaryCreatedPathW = targetPathW;
    }
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsDone++;
    }

    // Do not copy-recurse into a reparse point (junction/symlink) target
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (isMove) {
            ::RemoveDirectoryW(srcW.c_str());
        }
        return true;
    }

    const std::wstring pattern = JoinWinPath(srcW, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileExW(
        pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
    );
    if (hFind == INVALID_HANDLE_VALUE) {
        outErr = FormatWin32Error(::GetLastError());
        return false;
    }

    bool allChildrenOk = true;
    do {
        if (IsCancelled()) {
            outResult.cancelled = true;
            allChildrenOk = false;
            break;
        }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        const std::wstring childSrcW = JoinWinPath(srcW, fd.cFileName);
        const std::wstring childDstW = JoinWinPath(targetPathW, fd.cFileName);
        if (!CopyTree(childSrcW, childDstW, isMove, policy, outResult, outErr)) {
            allChildrenOk = false;
            if (outResult.cancelled) break;
        }
    } while (::FindNextFileW(hFind, &fd));

    ::FindClose(hFind);

    if (isMove && allChildrenOk && !outResult.cancelled) {
        ClearReadOnlyIfSet(srcW);
        ::RemoveDirectoryW(srcW.c_str());
    }
    return allChildrenOk;
}

bool FileOps::MoveTree(
    const std::wstring& srcW,
    const std::wstring& dstW,
    ConflictPolicy& policy,
    FileOpResult& outResult,
    std::string& outErr
) {
    if (IsCancelled()) {
        outResult.cancelled = true;
        return false;
    }

    uint64_t size = 0, time = 0;
    bool isDir = false;
    DWORD attrs = 0;
    if (!QueryMeta(srcW, size, time, isDir, attrs)) {
        outErr = "Source item not found: " + str::WideToUtf8(srcW);
        return false;
    }

    UpdateProgressItem(str::WideToUtf8(GetFileNamePart(srcW)));
    std::wstring targetPathW = dstW;

    if (PathExists(targetPathW)) {
        const DWORD dstAttrs = ::GetFileAttributesW(targetPathW.c_str());
        const bool dstIsDir = (dstAttrs != INVALID_FILE_ATTRIBUTES) && (dstAttrs & FILE_ATTRIBUTE_DIRECTORY);

        const ConflictDecision decision = ResolveItemConflict(srcW, targetPathW, dstIsDir, policy);
        if (decision == ConflictDecision::Cancel) {
            outResult.cancelled = true;
            return false;
        }
        if (decision == ConflictDecision::Skip) {
            outResult.skippedCount++;
            return true;
        }
        if (decision == ConflictDecision::AutoRename) {
            targetPathW = MakeUniquePath(
                str::GetParentWindowsPath(targetPathW),
                GetFileNamePart(targetPathW),
                false
            );
        } else if (decision == ConflictDecision::Overwrite) {
            if (isDir && dstIsDir && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
                // Merge into existing folder recursively
                return CopyTree(srcW, targetPathW, true, policy, outResult, outErr);
            }
            if (!isDir && !dstIsDir) {
                ClearReadOnlyIfSet(targetPathW);
                if (::MoveFileExW(srcW.c_str(), targetPathW.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                    if (outResult.primaryCreatedPathW.empty()) outResult.primaryCreatedPathW = targetPathW;
                    std::lock_guard<std::mutex> lock(m_progressMutex);
                    m_progress.itemsDone++;
                    return true;
                }
                const DWORD err = ::GetLastError();
                if (err == ERROR_NOT_SAME_DEVICE) {
                    return CopyTree(srcW, targetPathW, true, policy, outResult, outErr);
                }
                outErr = FormatWin32Error(err);
                return false;
            }
        }
    }

    // Fast same-volume atomic move
    if (::MoveFileExW(srcW.c_str(), targetPathW.c_str(), 0)) {
        if (outResult.primaryCreatedPathW.empty()) outResult.primaryCreatedPathW = targetPathW;
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsDone++;
        return true;
    }

    const DWORD err = ::GetLastError();
    if (err == ERROR_NOT_SAME_DEVICE) {
        return CopyTree(srcW, targetPathW, true, policy, outResult, outErr);
    }
    outErr = FormatWin32Error(err);
    return false;
}

void FileOps::ExecuteCopyOrMove(const FileOpRequest& req, bool isMove, FileOpResult& outResult) {
    outResult.targetDirectoryW = str::NormalizeWindowsPath(req.destDirW);
    if (req.sources.empty() || outResult.targetDirectoryW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid source or destination.";
        return;
    }

    if (!PathExists(outResult.targetDirectoryW)) {
        outResult.success = false;
        outResult.errorMessage = "Destination folder does not exist.";
        return;
    }

    // Validate against self/descendant recursion before starting
    for (const auto& srcW : req.sources) {
        if (IsSubpathOrEqual(srcW, outResult.targetDirectoryW)) {
            outResult.success = false;
            outResult.errorMessage = "Cannot copy or move a folder into itself.";
            return;
        }
    }

    size_t totalItems = req.sources.size();
    uint64_t totalBytes = 0;
    if (!isMove) {
        MeasureSources(req.sources, totalItems, totalBytes);
    } else {
        // Quick count for same-volume move; cross-volume updates dynamically
        totalItems = req.sources.size();
    }

    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsTotal = totalItems;
        m_progress.bytesTotal = totalBytes;
    }
    NotifyImmediate();

    ConflictPolicy policy = req.conflictPolicy;
    std::string lastErr;

    for (const auto& srcW : req.sources) {
        if (IsCancelled()) {
            outResult.cancelled = true;
            break;
        }

        const std::wstring cleanSrcW = str::NormalizeWindowsPath(srcW);
        const std::wstring fileNameW = GetFileNamePart(cleanSrcW);
        const std::wstring srcParentW = str::GetParentWindowsPath(cleanSrcW);
        const bool sameFolder = !srcParentW.empty() && IsSamePath(srcParentW, outResult.targetDirectoryW);

        if (isMove && sameFolder) {
            outResult.skippedCount++;
            continue;
        }

        std::wstring dstW = sameFolder
            ? MakeUniquePath(outResult.targetDirectoryW, fileNameW, true)
            : JoinWinPath(outResult.targetDirectoryW, fileNameW);

        std::string err;
        const bool ok = isMove
            ? MoveTree(cleanSrcW, dstW, policy, outResult, err)
            : CopyTree(cleanSrcW, dstW, false, policy, outResult, err);

        if (ok) {
            outResult.succeededCount++;
        } else if (outResult.cancelled) {
            break;
        } else {
            outResult.failedCount++;
            lastErr = err;
        }
    }

    outResult.success = (outResult.failedCount == 0 && !outResult.cancelled);
    outResult.errorMessage = lastErr;
    if (outResult.cancelled) {
        outResult.summaryMessage = isMove ? "Move cancelled." : "Copy cancelled.";
    } else if (!outResult.success) {
        outResult.summaryMessage = lastErr.empty() ? "Operation failed." : lastErr;
    } else {
        const char* verb = isMove ? "Moved" : "Copied";
        outResult.summaryMessage = std::string(verb) + " " + std::to_string(outResult.succeededCount) +
            (outResult.succeededCount == 1 ? " item" : " items");
    }
}

bool FileOps::DeleteTreePermanent(const std::wstring& pathW, std::string& outErr) {
    if (IsCancelled()) return false;

    const DWORD attrs = ::GetFileAttributesW(pathW.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        outErr = FormatWin32Error(::GetLastError());
        return false;
    }

    UpdateProgressItem(str::WideToUtf8(GetFileNamePart(pathW)));

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        ClearReadOnlyIfSet(pathW);
        if (!::DeleteFileW(pathW.c_str())) {
            outErr = FormatWin32Error(::GetLastError());
            return false;
        }
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsDone++;
        return true;
    }

    // Critical safety: delete symlink/junction itself without touching target contents
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {
        ClearReadOnlyIfSet(pathW);
        if (!::RemoveDirectoryW(pathW.c_str())) {
            outErr = FormatWin32Error(::GetLastError());
            return false;
        }
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsDone++;
        return true;
    }

    const std::wstring pattern = JoinWinPath(pathW, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = ::FindFirstFileExW(
        pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
    );
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (IsCancelled()) break;
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            const std::wstring childW = JoinWinPath(pathW, fd.cFileName);
            if (!DeleteTreePermanent(childW, outErr)) {
                ::FindClose(hFind);
                return false;
            }
        } while (::FindNextFileW(hFind, &fd));
        ::FindClose(hFind);
    }

    if (IsCancelled()) return false;

    ClearReadOnlyIfSet(pathW);
    if (!::RemoveDirectoryW(pathW.c_str())) {
        outErr = FormatWin32Error(::GetLastError());
        return false;
    }
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.itemsDone++;
    return true;
}

void FileOps::ExecuteDelete(const FileOpRequest& req, FileOpResult& outResult) {
    if (req.sources.empty()) {
        outResult.success = false;
        outResult.errorMessage = "No items selected to delete.";
        return;
    }

    outResult.targetDirectoryW = str::GetParentWindowsPath(req.sources.front());
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.itemsTotal = req.sources.size();
    }

    if (!req.permanent) {
        UpdateProgressItem("Moving to Recycle Bin...");
        std::string err;
        if (DeleteItemsViaShell(req.sources, true, err)) {
            outResult.succeededCount = req.sources.size();
            outResult.success = true;
            outResult.summaryMessage = "Moved " + std::to_string(outResult.succeededCount) +
                (outResult.succeededCount == 1 ? " item" : " items") + " to Recycle Bin";
        } else {
            outResult.failedCount = req.sources.size();
            outResult.success = false;
            outResult.errorMessage = err;
            outResult.summaryMessage = err;
        }
        return;
    }

    std::string lastErr;
    for (const auto& srcW : req.sources) {
        if (IsCancelled()) {
            outResult.cancelled = true;
            break;
        }
        std::string err;
        if (DeleteTreePermanent(srcW, err)) {
            outResult.succeededCount++;
        } else {
            outResult.failedCount++;
            lastErr = err;
        }
    }

    outResult.success = (outResult.failedCount == 0 && !outResult.cancelled);
    outResult.errorMessage = lastErr;
    if (outResult.cancelled) {
        outResult.summaryMessage = "Delete cancelled.";
    } else if (!outResult.success) {
        outResult.summaryMessage = lastErr.empty() ? "Delete failed." : lastErr;
    } else {
        outResult.summaryMessage = "Permanently deleted " + std::to_string(outResult.succeededCount) +
            (outResult.succeededCount == 1 ? " item" : " items");
    }
}

void FileOps::ExecuteRename(const FileOpRequest& req, FileOpResult& outResult) {
    if (req.sources.empty() || req.newNameW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid rename name.";
        return;
    }

    const std::wstring srcW = str::NormalizeWindowsPath(req.sources.front());
    const std::wstring parentW = str::GetParentWindowsPath(srcW);
    outResult.targetDirectoryW = parentW;

    if (parentW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Cannot rename root volume.";
        return;
    }

    const std::wstring dstW = JoinWinPath(parentW, req.newNameW);
    if (IsSamePath(srcW, dstW)) {
        // Case-only rename on NTFS still works directly via MoveFileExW
        if (srcW == dstW) {
            outResult.success = true;
            outResult.primaryCreatedPathW = dstW;
            outResult.summaryMessage = "Unchanged.";
            return;
        }
    } else if (PathExists(dstW)) {
        outResult.success = false;
        outResult.errorMessage = "An item with that name already exists.";
        outResult.summaryMessage = outResult.errorMessage;
        return;
    }

    if (!::MoveFileExW(srcW.c_str(), dstW.c_str(), 0)) {
        outResult.success = false;
        outResult.errorMessage = FormatWin32Error(::GetLastError());
        outResult.summaryMessage = outResult.errorMessage;
        return;
    }

    outResult.succeededCount = 1;
    outResult.success = true;
    outResult.primaryCreatedPathW = dstW;
    outResult.summaryMessage = "Renamed to " + str::WideToUtf8(req.newNameW);
}

void FileOps::ExecuteCreateItem(const FileOpRequest& req, bool isFolder, FileOpResult& outResult) {
    const std::wstring dirW = str::NormalizeWindowsPath(req.destDirW);
    outResult.targetDirectoryW = dirW;

    if (dirW.empty() || req.newNameW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid destination or name.";
        return;
    }

    const std::wstring dstW = JoinWinPath(dirW, req.newNameW);
    if (PathExists(dstW)) {
        outResult.success = false;
        outResult.errorMessage = "An item named \"" + str::WideToUtf8(req.newNameW) + "\" already exists.";
        outResult.summaryMessage = outResult.errorMessage;
        return;
    }

    if (isFolder) {
        if (!::CreateDirectoryW(dstW.c_str(), nullptr)) {
            outResult.success = false;
            outResult.errorMessage = FormatWin32Error(::GetLastError());
            outResult.summaryMessage = outResult.errorMessage;
            return;
        }
    } else {
        HANDLE h = ::CreateFileW(
            dstW.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr
        );
        if (h == INVALID_HANDLE_VALUE) {
            outResult.success = false;
            outResult.errorMessage = FormatWin32Error(::GetLastError());
            outResult.summaryMessage = outResult.errorMessage;
            return;
        }
        ::CloseHandle(h);
    }

    outResult.succeededCount = 1;
    outResult.success = true;
    outResult.primaryCreatedPathW = dstW;
    outResult.summaryMessage = std::string("Created ") + str::WideToUtf8(req.newNameW);
}

void FileOps::ExecuteCreateSymlink(const FileOpRequest& req, FileOpResult& outResult) {
    const std::wstring dirW = str::NormalizeWindowsPath(req.destDirW);
    outResult.targetDirectoryW = dirW;
    if (req.sources.empty() || dirW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid target or destination for symbolic link.";
        return;
    }

    std::string lastErr;
    for (size_t i = 0; i < req.sources.size(); ++i) {
        const std::wstring targetW = str::NormalizeWindowsPath(req.sources[i]);
        const DWORD attrs = ::GetFileAttributesW(targetW.c_str());
        const bool isDir = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        const std::wstring desiredNameW = (req.sources.size() == 1 && !req.newNameW.empty())
            ? req.newNameW
            : GetFileNamePart(targetW);
        const std::wstring linkPathW = MakeUniquePath(dirW, desiredNameW, false);

        const DWORD baseFlags = isDir ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0;
        BOOL ok = ::CreateSymbolicLinkW(
            linkPathW.c_str(),
            targetW.c_str(),
            baseFlags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
        );
        if (!ok && ::GetLastError() == ERROR_INVALID_PARAMETER) {
            ok = ::CreateSymbolicLinkW(linkPathW.c_str(), targetW.c_str(), baseFlags);
        }

        if (ok) {
            outResult.succeededCount++;
            if (outResult.primaryCreatedPathW.empty()) outResult.primaryCreatedPathW = linkPathW;
        } else {
            outResult.failedCount++;
            const DWORD err = ::GetLastError();
            if (err == ERROR_PRIVILEGE_NOT_HELD) {
                lastErr = "Symbolic Link requires Windows Developer Mode or Administrator privileges.";
            } else {
                lastErr = FormatWin32Error(err);
            }
        }
    }

    outResult.success = (outResult.failedCount == 0);
    outResult.errorMessage = lastErr;
    outResult.summaryMessage = outResult.success
        ? ("Created " + std::to_string(outResult.succeededCount) + " symbolic link(s)")
        : lastErr;
}

void FileOps::ExecuteCreateJunction(const FileOpRequest& req, FileOpResult& outResult) {
    const std::wstring dirW = str::NormalizeWindowsPath(req.destDirW);
    outResult.targetDirectoryW = dirW;
    if (req.sources.empty() || dirW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid target or destination for directory junction.";
        return;
    }

    std::string lastErr;
    for (size_t i = 0; i < req.sources.size(); ++i) {
        std::wstring targetW = str::NormalizeWindowsPath(req.sources[i]);
        if (targetW.size() >= 2 && targetW[0] == L'\\' && targetW[1] == L'\\') {
            outResult.failedCount++;
            lastErr = "Directory Junctions only support local folders (use Symbolic Link for network paths).";
            continue;
        }

        const DWORD attrs = ::GetFileAttributesW(targetW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            outResult.failedCount++;
            lastErr = "Directory Junctions require an existing target folder.";
            continue;
        }

        const std::wstring desiredNameW = (req.sources.size() == 1 && !req.newNameW.empty())
            ? req.newNameW
            : GetFileNamePart(targetW);
        const std::wstring junctionPathW = MakeUniquePath(dirW, desiredNameW, false);

        if (!::CreateDirectoryW(junctionPathW.c_str(), nullptr)) {
            outResult.failedCount++;
            lastErr = FormatWin32Error(::GetLastError());
            continue;
        }

        HANDLE hDir = ::CreateFileW(
            junctionPathW.c_str(),
            GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
            nullptr
        );
        if (hDir == INVALID_HANDLE_VALUE) {
            outResult.failedCount++;
            lastErr = FormatWin32Error(::GetLastError());
            ::RemoveDirectoryW(junctionPathW.c_str());
            continue;
        }

        const std::wstring subStr = L"\\??\\" + targetW;
        const std::wstring printStr = targetW;

        const USHORT subBytes = static_cast<USHORT>(subStr.size() * sizeof(wchar_t));
        const USHORT printBytes = static_cast<USHORT>(printStr.size() * sizeof(wchar_t));
        const USHORT reparseDataLen = static_cast<USHORT>(
            sizeof(MountPointFields) + subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t)
        );
        const size_t totalBytes = sizeof(ReparseHeader) + reparseDataLen;

        std::vector<uint8_t> buf(totalBytes, 0);
        ReparseHeader hdr{};
        hdr.ReparseTag = kReparseTagMountPoint;
        hdr.ReparseDataLength = reparseDataLen;
        hdr.Reserved = 0;

        MountPointFields fields{};
        fields.SubstituteNameOffset = 0;
        fields.SubstituteNameLength = subBytes;
        fields.PrintNameOffset = static_cast<USHORT>(subBytes + sizeof(wchar_t));
        fields.PrintNameLength = printBytes;

        const size_t pathBufOffset = sizeof(ReparseHeader) + sizeof(MountPointFields);
        std::memcpy(buf.data(), &hdr, sizeof(hdr));
        std::memcpy(buf.data() + sizeof(ReparseHeader), &fields, sizeof(fields));
        std::memcpy(buf.data() + pathBufOffset + fields.SubstituteNameOffset, subStr.data(), subBytes);
        std::memcpy(buf.data() + pathBufOffset + fields.PrintNameOffset, printStr.data(), printBytes);

        DWORD bytesReturned = 0;
        const BOOL ok = ::DeviceIoControl(
            hDir,
            FSCTL_SET_REPARSE_POINT,
            buf.data(),
            static_cast<DWORD>(buf.size()),
            nullptr,
            0,
            &bytesReturned,
            nullptr
        );
        const DWORD ioErr = ::GetLastError();
        ::CloseHandle(hDir);

        if (ok) {
            outResult.succeededCount++;
            if (outResult.primaryCreatedPathW.empty()) outResult.primaryCreatedPathW = junctionPathW;
        } else {
            ::RemoveDirectoryW(junctionPathW.c_str());
            outResult.failedCount++;
            lastErr = FormatWin32Error(ioErr);
        }
    }

    outResult.success = (outResult.failedCount == 0);
    outResult.errorMessage = lastErr;
    outResult.summaryMessage = outResult.success
        ? ("Created " + std::to_string(outResult.succeededCount) + " directory junction(s)")
        : lastErr;
}

void FileOps::ExecuteCreateHardlink(const FileOpRequest& req, FileOpResult& outResult) {
    const std::wstring dirW = str::NormalizeWindowsPath(req.destDirW);
    outResult.targetDirectoryW = dirW;
    if (req.sources.empty() || dirW.empty()) {
        outResult.success = false;
        outResult.errorMessage = "Invalid target or destination for hard link.";
        return;
    }

    std::string lastErr;
    for (size_t i = 0; i < req.sources.size(); ++i) {
        const std::wstring targetW = str::NormalizeWindowsPath(req.sources[i]);
        const DWORD attrs = ::GetFileAttributesW(targetW.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            outResult.failedCount++;
            lastErr = "Hard Links only support files, not directories.";
            continue;
        }

        const std::wstring desiredNameW = (req.sources.size() == 1 && !req.newNameW.empty())
            ? req.newNameW
            : GetFileNamePart(targetW);
        const std::wstring linkPathW = MakeUniquePath(dirW, desiredNameW, false);

        if (::CreateHardLinkW(linkPathW.c_str(), targetW.c_str(), nullptr)) {
            outResult.succeededCount++;
            if (outResult.primaryCreatedPathW.empty()) outResult.primaryCreatedPathW = linkPathW;
        } else {
            outResult.failedCount++;
            const DWORD err = ::GetLastError();
            if (err == ERROR_NOT_SAME_DEVICE) {
                lastErr = "Hard Links require source and destination to be on the same NTFS volume.";
            } else {
                lastErr = FormatWin32Error(err);
            }
        }
    }

    outResult.success = (outResult.failedCount == 0);
    outResult.errorMessage = lastErr;
    outResult.summaryMessage = outResult.success
        ? ("Created " + std::to_string(outResult.succeededCount) + " hard link(s)")
        : lastErr;
}

} // namespace vela