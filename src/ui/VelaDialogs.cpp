#include "ui/VelaDialogs.hpp"
#include "ui/FluentIcons.hpp"
#include "core/StringUtil.hpp"

#include <wincrypt.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace vela {
namespace {

ImVec4 ToImVec4(const ColorRGBA& c) {
    return ImVec4(c.r, c.g, c.b, c.a);
}

std::wstring GetBaseNameW(const std::wstring& pathW) {
    std::wstring clean = str::NormalizeWindowsPath(pathW);
    while (clean.size() > 3 && clean.back() == L'\\') {
        clean.pop_back();
    }
    const size_t pos = clean.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos + 1 >= clean.size()) return clean;
    return clean.substr(pos + 1);
}

std::string FormatCommas(uint64_t value) {
    std::string raw = std::to_string(value);
    std::string out;
    out.reserve(raw.size() + raw.size() / 3);
    int count = 0;
    for (size_t i = raw.size(); i > 0; --i) {
        if (count > 0 && (count % 3) == 0) out.push_back(',');
        out.push_back(raw[i - 1]);
        ++count;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::string FormatFullFileTime(const FILETIME& ft) {
    if (ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0) return "-";
    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&ft, &localFt)) return "-";
    if (!::FileTimeToSystemTime(&localFt, &st)) return "-";

    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return std::string(buf);
}

std::string FormatFileTime64(uint64_t ft64) {
    if (ft64 == 0) return "-";
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(ft64 & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>((ft64 >> 32) & 0xFFFFFFFFULL);
    return FormatFullFileTime(ft);
}

std::wstring JoinPathW(const std::wstring& dirW, const std::wstring& nameW) {
    if (dirW.empty()) return nameW;
    std::wstring out = dirW;
    if (out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out.append(nameW);
    return out;
}

bool HasSameDriveLetter(const std::wstring& aW, const std::wstring& bW) {
    if (aW.size() < 2 || bW.size() < 2) return false;
    if (aW[1] != L':' || bW[1] != L':') return false;
    return std::towupper(aW[0]) == std::towupper(bW[0]);
}

bool IsUncPath(const std::wstring& pathW) {
    return pathW.size() >= 2 && pathW[0] == L'\\' && pathW[1] == L'\\';
}

std::string DescribeAttributes(DWORD attrs) {
    std::string out;
    auto add = [&](const char* label) {
        if (!out.empty()) out += ", ";
        out += label;
    };
    if (attrs & FILE_ATTRIBUTE_READONLY)      add("Read-only");
    if (attrs & FILE_ATTRIBUTE_HIDDEN)        add("Hidden");
    if (attrs & FILE_ATTRIBUTE_SYSTEM)        add("System");
    if (attrs & FILE_ATTRIBUTE_ARCHIVE)       add("Archive");
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) add("Reparse Point");
    if (attrs & FILE_ATTRIBUTE_COMPRESSED)    add("Compressed");
    if (attrs & FILE_ATTRIBUTE_ENCRYPTED)     add("Encrypted");
    return out.empty() ? "Normal" : out;
}

} // namespace

VelaDialogs::~VelaDialogs() {
    Shutdown();
}

void VelaDialogs::Initialize(
    Win32Window* window,
    FileOps* fileOps,
    std::function<void(bool isError, const std::string& message)> onToast
) {
    m_window = window;
    m_fileOps = fileOps;
    m_onToast = std::move(onToast);
}

void VelaDialogs::Shutdown() {
    StopBgWorker();
}

bool VelaDialogs::IsAnyModalOpen() const {
    return m_newItemOpen || m_renameOpen || m_deleteOpen ||
           m_propsOpen || m_conflictModalOpen || m_dropPopupOpen;
}

void VelaDialogs::StopBgWorker() {
    m_bgCancel = true;
    if (m_bgThread.joinable()) {
        m_bgThread.join();
    }
    m_bgCancel = false;
}

void VelaDialogs::OpenNewItem(bool isFolder, const std::wstring& destDirW) {
    if (destDirW.empty()) return;
    m_newItemIsFolder = isFolder;
    m_newItemDirW = str::NormalizeWindowsPath(destDirW);

    const std::wstring baseName = isFolder ? L"New folder" : L"New Text Document.txt";
    const std::wstring uniquePath = FileOps::MakeUniquePath(m_newItemDirW, baseName, false);
    const std::string utf8 = str::WideToUtf8(GetBaseNameW(uniquePath));
    std::snprintf(m_newItemBuf, sizeof(m_newItemBuf), "%s", utf8.c_str());

    m_openNewItemPending = true;
    m_newItemFocus = true;
    if (m_window) m_window->Wake();
}

void VelaDialogs::OpenRename(const std::wstring& itemPathW, bool isDir) {
    if (itemPathW.empty()) return;
    m_renameTargetW = str::NormalizeWindowsPath(itemPathW);
    m_renameIsDir = isDir;

    const std::string utf8 = str::WideToUtf8(GetBaseNameW(m_renameTargetW));
    std::snprintf(m_renameBuf, sizeof(m_renameBuf), "%s", utf8.c_str());

    m_openRenamePending = true;
    m_renameFocus = true;
    if (m_window) m_window->Wake();
}

void VelaDialogs::OpenDeleteConfirm(std::vector<std::wstring> pathsW, bool permanent) {
    if (pathsW.empty()) return;
    m_deletePathsW = std::move(pathsW);
    m_deletePermanent = permanent;
    m_openDeletePending = true;
    if (m_window) m_window->Wake();
}

void VelaDialogs::OpenProperties(std::vector<std::wstring> targetPathsW) {
    if (targetPathsW.empty()) return;
    StopBgWorker();

    PropertiesState init;
    init.targetsW = std::move(targetPathsW);
    init.isMulti = (init.targetsW.size() > 1);

    if (init.isMulti) {
        init.title = std::to_string(init.targetsW.size()) + " items";
        init.location = str::WideToUtf8(str::GetParentWindowsPath(init.targetsW.front()));
        init.typeLabel = "Multiple Types";
        init.calculatingStats = true;
    } else {
        const std::wstring& pW = init.targetsW.front();
        init.title = str::WideToUtf8(GetBaseNameW(pW));
        const std::wstring parentW = str::GetParentWindowsPath(pW);
        init.location = parentW.empty() ? str::WideToUtf8(pW) : str::WideToUtf8(parentW);

        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (::GetFileAttributesExW(pW.c_str(), GetFileExInfoStandard, &fad)) {
            init.attributes = fad.dwFileAttributes;
            init.createdText = FormatFullFileTime(fad.ftCreationTime);
            init.modifiedText = FormatFullFileTime(fad.ftLastWriteTime);
            init.accessedText = FormatFullFileTime(fad.ftLastAccessTime);

            const bool isDir = (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
                std::wstring targetW;
                std::string kind;
                if (FileOps::ReadLinkTarget(pW, targetW, kind)) {
                    init.linkKind = kind;
                    init.linkTarget = str::WideToUtf8(targetW);
                    init.typeLabel = kind;
                } else {
                    init.typeLabel = isDir ? "Folder (Reparse Point)" : "Reparse Point";
                }
            } else if (isDir) {
                init.typeLabel = "File folder";
            } else {
                init.isSingleFile = true;
                init.fileCount = 1;
                init.totalBytes = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                init.typeLabel = "File";
            }

            if (isDir && (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                init.calculatingStats = true;
            }
        }
    }

    const bool needStats = init.calculatingStats;
    auto pathsCopy = init.targetsW;
    {
        std::lock_guard<std::mutex> lock(m_propMutex);
        m_propState = std::move(init);
    }

    if (needStats) {
        StartFolderStatsWorker(std::move(pathsCopy));
    }

    m_openPropsPending = true;
    if (m_window) m_window->Wake();
}

void VelaDialogs::OpenDragDropPopup(std::vector<std::wstring> sourcesW, std::wstring destDirW) {
    if (sourcesW.empty() || destDirW.empty()) return;

    m_dropSourcesW = std::move(sourcesW);
    m_dropDestDirW = str::NormalizeWindowsPath(std::move(destDirW));

    const std::string destName = str::WideToUtf8(GetBaseNameW(m_dropDestDirW));
    if (m_dropSourcesW.size() == 1) {
        m_dropHeader = "Drop \"" + str::WideToUtf8(GetBaseNameW(m_dropSourcesW.front())) +
                       "\" into \"" + destName + "\"";
    } else {
        m_dropHeader = "Drop " + std::to_string(m_dropSourcesW.size()) +
                       " items into \"" + destName + "\"";
    }

    bool allLocalDirs = true;
    bool allFilesSameDrive = true;
    for (const auto& sW : m_dropSourcesW) {
        const DWORD attrs = ::GetFileAttributesW(sW.c_str());
        const bool isDir = (attrs != INVALID_FILE_ATTRIBUTES) && (attrs & FILE_ATTRIBUTE_DIRECTORY);
        if (!isDir || IsUncPath(sW)) {
            allLocalDirs = false;
        }
        if (attrs == INVALID_FILE_ATTRIBUTES || isDir || !HasSameDriveLetter(sW, m_dropDestDirW)) {
            allFilesSameDrive = false;
        }
    }
    if (IsUncPath(m_dropDestDirW)) {
        allFilesSameDrive = false;
    }

    m_dropAllLocalDirs = allLocalDirs;
    m_dropAllFilesSameDrive = allFilesSameDrive;
    m_openDropPopupPending = true;
    if (m_window) m_window->Wake();
}

void VelaDialogs::StartFolderStatsWorker(std::vector<std::wstring> pathsW) {
    StopBgWorker();
    m_bgCancel = false;

    m_bgThread = std::thread([this, paths = std::move(pathsW)]() {
        size_t files = 0;
        size_t folders = 0;
        uint64_t bytes = 0;
        auto lastWake = std::chrono::steady_clock::now();

        auto publish = [&](bool done) {
            {
                std::lock_guard<std::mutex> lock(m_propMutex);
                m_propState.fileCount = files;
                m_propState.folderCount = folders;
                m_propState.totalBytes = bytes;
                if (done) m_propState.calculatingStats = false;
            }
            if (m_window) m_window->Wake();
        };

        std::vector<std::wstring> stack;
        for (const auto& pW : paths) {
            if (m_bgCancel.load()) break;
            const DWORD attrs = ::GetFileAttributesW(pW.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) continue;
            if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
                if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                    stack.push_back(pW);
                }
            } else {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (::GetFileAttributesExW(pW.c_str(), GetFileExInfoStandard, &fad)) {
                    ++files;
                    bytes += (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                }
            }
        }

        while (!stack.empty() && !m_bgCancel.load()) {
            const std::wstring dirW = std::move(stack.back());
            stack.pop_back();

            const std::wstring pattern = JoinPathW(dirW, L"*");
            WIN32_FIND_DATAW fd{};
            HANDLE hFind = ::FindFirstFileExW(
                pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH
            );
            if (hFind == INVALID_HANDLE_VALUE) continue;

            do {
                if (m_bgCancel.load()) break;
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    ++folders;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
                        stack.push_back(JoinPathW(dirW, fd.cFileName));
                    }
                } else {
                    ++files;
                    bytes += (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
                }

                const auto now = std::chrono::steady_clock::now();
                if (now - lastWake >= std::chrono::milliseconds(80)) {
                    lastWake = now;
                    publish(false);
                }
            } while (::FindNextFileW(hFind, &fd));

            ::FindClose(hFind);
        }

        publish(true);
    });
}

void VelaDialogs::StartSha256Worker(std::wstring filePathW) {
    StopBgWorker();
    m_bgCancel = false;
    {
        std::lock_guard<std::mutex> lock(m_propMutex);
        m_propState.computingHash = true;
        m_propState.hashProgress = 0.0f;
        m_propState.sha256Hex.clear();
        m_propState.hashError.clear();
    }

    m_bgThread = std::thread([this, pathW = std::move(filePathW)]() {
        std::string hex;
        std::string err;

        HANDLE hFile = ::CreateFileW(
            pathW.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr
        );
        if (hFile == INVALID_HANDLE_VALUE) {
            err = "Cannot open file for hashing.";
        } else {
            LARGE_INTEGER fileSize{};
            ::GetFileSizeEx(hFile, &fileSize);
            const uint64_t totalBytes = static_cast<uint64_t>(fileSize.QuadPart);

            HCRYPTPROV hProv = 0;
            HCRYPTHASH hHash = 0;
            if (!::CryptAcquireContextW(&hProv, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) {
                err = "CryptAcquireContext failed.";
            } else if (!::CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash)) {
                err = "CryptCreateHash(SHA-256) failed.";
            } else {
                std::vector<BYTE> buffer(256 * 1024);
                DWORD bytesRead = 0;
                uint64_t doneBytes = 0;
                auto lastWake = std::chrono::steady_clock::now();

                while (!m_bgCancel.load() &&
                       ::ReadFile(hFile, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr) &&
                       bytesRead > 0) {
                    if (!::CryptHashData(hHash, buffer.data(), bytesRead, 0)) {
                        err = "CryptHashData failed.";
                        break;
                    }
                    doneBytes += bytesRead;
                    const auto now = std::chrono::steady_clock::now();
                    if (totalBytes > 0 && now - lastWake >= std::chrono::milliseconds(80)) {
                        lastWake = now;
                        {
                            std::lock_guard<std::mutex> lock(m_propMutex);
                            m_propState.hashProgress = static_cast<float>(doneBytes) / static_cast<float>(totalBytes);
                        }
                        if (m_window) m_window->Wake();
                    }
                }

                if (err.empty() && !m_bgCancel.load()) {
                    BYTE hashBytes[32]{};
                    DWORD hashLen = sizeof(hashBytes);
                    if (::CryptGetHashParam(hHash, HP_HASHVAL, hashBytes, &hashLen, 0)) {
                        static constexpr char kHex[] = "0123456789abcdef";
                        hex.reserve(hashLen * 2);
                        for (DWORD i = 0; i < hashLen; ++i) {
                            hex.push_back(kHex[(hashBytes[i] >> 4) & 0xF]);
                            hex.push_back(kHex[hashBytes[i] & 0xF]);
                        }
                    } else {
                        err = "CryptGetHashParam failed.";
                    }
                }
            }

            if (hHash) ::CryptDestroyHash(hHash);
            if (hProv) ::CryptReleaseContext(hProv, 0);
            ::CloseHandle(hFile);
        }

        {
            std::lock_guard<std::mutex> lock(m_propMutex);
            m_propState.computingHash = false;
            m_propState.hashProgress = err.empty() ? 1.0f : 0.0f;
            m_propState.sha256Hex = std::move(hex);
            m_propState.hashError = std::move(err);
        }
        if (m_window) m_window->Wake();
    });
}

void VelaDialogs::Render(const Config& cfg, float dpiScale) {
    FileOpProgress prog;
    if (m_fileOps) {
        prog = m_fileOps->GetProgress();
    }

    if (prog.running && !prog.conflict.active) {
        RenderProgressOverlay(cfg, prog, dpiScale);
    }
    RenderConflictModal(cfg, prog, dpiScale);
    RenderNewItemModal(cfg, dpiScale);
    RenderRenameModal(cfg, dpiScale);
    RenderDeleteModal(cfg, dpiScale);
    RenderPropertiesModal(cfg, dpiScale);
    RenderDropPopup(cfg);
}

void VelaDialogs::RenderProgressOverlay(const Config& cfg, const FileOpProgress& prog, float dpiScale) {
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const float w = (std::min)(340.0f * dpiScale, disp.x - 32.0f * dpiScale);
    const float h = 98.0f * dpiScale;
    const float statusOffset = cfg.layout.showStatusbar ? (cfg.layout.statusbarHeight * dpiScale) : 0.0f;

    ImGui::SetNextWindowPos(ImVec2(disp.x - w - 16.0f * dpiScale, disp.y - h - statusOffset - 12.0f * dpiScale));
    ImGui::SetNextWindowSize(ImVec2(w, h));

    ImGui::PushStyleColor(ImGuiCol_WindowBg, ToImVec4(cfg.theme.popupBg));
    ImGui::PushStyleColor(ImGuiCol_Border, ToImVec4(cfg.theme.popupBorder));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, cfg.theme.cardRounding * dpiScale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * dpiScale, 10.0f * dpiScale));

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoFocusOnAppearing;

    if (ImGui::Begin("##FileOpProgressOverlay", nullptr, flags)) {
        std::string title = prog.statusLabel;
        if (prog.itemsTotal > 0) {
            title += " (" + std::to_string(prog.itemsDone) + " / " + std::to_string(prog.itemsTotal) + ")";
        }
        ImGui::TextUnformatted(title.c_str());

        ImGui::SameLine(w - 76.0f * dpiScale);
        if (ImGui::SmallButton("Cancel") && m_fileOps) {
            m_fileOps->RequestCancel();
        }

        if (!prog.currentItem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.textSecondary.ToImU32());
            ImGui::Text("%s", prog.currentItem.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::Dummy(ImVec2(0, ImGui::GetTextLineHeight()));
        }

        float frac = 0.0f;
        std::string overlay;
        if (prog.bytesTotal > 0) {
            frac = std::clamp(static_cast<float>(prog.bytesDone) / static_cast<float>(prog.bytesTotal), 0.0f, 1.0f);
            overlay = str::FormatFileSize(prog.bytesDone) + " / " + str::FormatFileSize(prog.bytesTotal);
        } else if (prog.itemsTotal > 0) {
            frac = std::clamp(static_cast<float>(prog.itemsDone) / static_cast<float>(prog.itemsTotal), 0.0f, 1.0f);
        }

        ImGui::PushStyleColor(ImGuiCol_FrameBg, ToImVec4(cfg.theme.progressTrackBg));
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cfg.theme.accent.ToImU32());
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 16.0f * dpiScale), overlay.empty() ? nullptr : overlay.c_str());
        ImGui::PopStyleColor(2);
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void VelaDialogs::RenderConflictModal(const Config& cfg, const FileOpProgress& prog, float dpiScale) {
    if (prog.conflict.active && !m_conflictModalOpen) {
        m_conflictApplyAll = false;
        ImGui::OpenPopup("##FileOpConflictModal");
    }

    ImGui::SetNextWindowSize(ImVec2(460.0f * dpiScale, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    m_conflictModalOpen = false;
    if (!ImGui::BeginPopupModal("##FileOpConflictModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    m_conflictModalOpen = true;

    if (!prog.conflict.active || !m_fileOps) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("File Conflict");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("An item named \"%s\" already exists in the destination.", prog.conflict.itemName.c_str());
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.textSecondary.ToImU32());
    if (!prog.conflict.isDir) {
        ImGui::Text("Source: %s  (%s)", str::FormatFileSize(prog.conflict.sourceSize).c_str(),
                    FormatFileTime64(prog.conflict.sourceTime).c_str());
        ImGui::Text("Existing: %s  (%s)", str::FormatFileSize(prog.conflict.destSize).c_str(),
                    FormatFileTime64(prog.conflict.destTime).c_str());
    } else {
        ImGui::TextWrapped("Destination folder already exists. Replace/merge contents?");
    }
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::Checkbox("Apply to all remaining conflicts", &m_conflictApplyAll);
    ImGui::Spacing();

    auto resolveBtn = [&](const char* label, ConflictDecision decision) {
        if (ImGui::Button(label)) {
            m_fileOps->ResolveConflict(decision, m_conflictApplyAll);
            ImGui::CloseCurrentPopup();
        }
    };

    resolveBtn(prog.conflict.isDir ? "Merge / Replace" : "Replace", ConflictDecision::Overwrite);
    ImGui::SameLine();
    resolveBtn("Skip", ConflictDecision::Skip);
    ImGui::SameLine();
    resolveBtn("Keep Both", ConflictDecision::AutoRename);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        m_fileOps->ResolveConflict(ConflictDecision::Cancel, false);
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void VelaDialogs::RenderNewItemModal(const Config& cfg, float dpiScale) {
    if (m_openNewItemPending) {
        ImGui::OpenPopup("##VelaNewItemModal");
        m_openNewItemPending = false;
    }

    ImGui::SetNextWindowSize(ImVec2(400.0f * dpiScale, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    m_newItemOpen = false;
    if (!ImGui::BeginPopupModal("##VelaNewItemModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    m_newItemOpen = true;

    ImGui::TextUnformatted(m_newItemIsFolder ? "New Folder" : "New File");
    ImGui::Separator();
    ImGui::Spacing();

    if (m_newItemFocus) {
        ImGui::SetKeyboardFocusHere();
        m_newItemFocus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputText(
        "##NewItemName", m_newItemBuf, sizeof(m_newItemBuf),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
    );

    ImGui::Spacing();
    const bool valid = (m_newItemBuf[0] != '\0');
    if ((enter || ImGui::Button("Create")) && valid && m_fileOps) {
        FileOpRequest req;
        req.kind = m_newItemIsFolder ? FileOpKind::CreateFolder : FileOpKind::CreateFile;
        req.destDirW = m_newItemDirW;
        req.newNameW = str::Utf8ToWide(m_newItemBuf);
        m_fileOps->Enqueue(std::move(req));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    (void)cfg;
    ImGui::EndPopup();
}

void VelaDialogs::RenderRenameModal(const Config& cfg, float dpiScale) {
    if (m_openRenamePending) {
        ImGui::OpenPopup("##VelaRenameModal");
        m_openRenamePending = false;
    }

    ImGui::SetNextWindowSize(ImVec2(420.0f * dpiScale, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    m_renameOpen = false;
    if (!ImGui::BeginPopupModal("##VelaRenameModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    m_renameOpen = true;

    ImGui::TextUnformatted(m_renameIsDir ? "Rename Folder" : "Rename File");
    ImGui::Separator();
    ImGui::Spacing();

    if (m_renameFocus) {
        ImGui::SetKeyboardFocusHere();
        m_renameFocus = false;
    }
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputText(
        "##RenameInput", m_renameBuf, sizeof(m_renameBuf),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
    );

    ImGui::Spacing();
    const bool valid = (m_renameBuf[0] != '\0');
    if ((enter || ImGui::Button("Rename")) && valid && m_fileOps) {
        FileOpRequest req;
        req.kind = FileOpKind::Rename;
        req.sources = {m_renameTargetW};
        req.newNameW = str::Utf8ToWide(m_renameBuf);
        m_fileOps->Enqueue(std::move(req));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    (void)cfg;
    ImGui::EndPopup();
}

void VelaDialogs::RenderDeleteModal(const Config& cfg, float dpiScale) {
    if (m_openDeletePending) {
        ImGui::OpenPopup("##VelaDeleteModal");
        m_openDeletePending = false;
    }

    ImGui::SetNextWindowSize(ImVec2(430.0f * dpiScale, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    m_deleteOpen = false;
    if (!ImGui::BeginPopupModal("##VelaDeleteModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    m_deleteOpen = true;

    ImGui::TextUnformatted(m_deletePermanent ? "Delete Permanently" : "Move to Recycle Bin");
    ImGui::Separator();
    ImGui::Spacing();

    if (m_deletePathsW.size() == 1) {
        const std::string name = str::WideToUtf8(GetBaseNameW(m_deletePathsW.front()));
        ImGui::TextWrapped(
            m_deletePermanent ? "Permanently delete \"%s\"?" : "Move \"%s\" to the Recycle Bin?",
            name.c_str()
        );
    } else {
        ImGui::TextWrapped(
            m_deletePermanent ? "Permanently delete these %zu items?" : "Move these %zu items to the Recycle Bin?",
            m_deletePathsW.size()
        );
        ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.textSecondary.ToImU32());
        const size_t preview = (std::min)(m_deletePathsW.size(), size_t{4});
        for (size_t i = 0; i < preview; ++i) {
            ImGui::BulletText("%s", str::WideToUtf8(GetBaseNameW(m_deletePathsW[i])).c_str());
        }
        if (m_deletePathsW.size() > preview) {
            ImGui::Text("  ...and %zu more", m_deletePathsW.size() - preview);
        }
        ImGui::PopStyleColor();
    }

    if (m_deletePermanent) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.dangerText.ToImU32());
        ImGui::TextUnformatted("Warning: This action cannot be undone.");
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (m_deletePermanent) {
        const ColorRGBA& d = cfg.theme.danger;
        const ImVec4 dHover(
            (std::min)(1.0f, d.r * 1.15f),
            (std::min)(1.0f, d.g * 1.15f),
            (std::min)(1.0f, d.b * 1.15f),
            d.a
        );
        ImGui::PushStyleColor(ImGuiCol_Button, ToImVec4(d));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, dHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ToImVec4(d));
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 255, 255));
    }
    const bool confirm = ImGui::Button(m_deletePermanent ? "Delete Permanently" : "Move to Recycle Bin") ||
                         ImGui::IsKeyPressed(ImGuiKey_Enter, false);
    if (m_deletePermanent) {
        ImGui::PopStyleColor(4);
    }

    if (confirm && m_fileOps) {
        FileOpRequest req;
        req.kind = FileOpKind::Delete;
        req.sources = m_deletePathsW;
        req.permanent = m_deletePermanent;
        req.ownerHwnd = m_window ? m_window->GetHandle() : nullptr;
        m_fileOps->Enqueue(std::move(req));
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void VelaDialogs::RenderPropertiesModal(const Config& cfg, float dpiScale) {
    if (m_openPropsPending) {
        ImGui::OpenPopup("##VelaPropertiesModal");
        m_openPropsPending = false;
    }

    ImGui::SetNextWindowSize(ImVec2(480.0f * dpiScale, 0.0f), ImGuiCond_Appearing);
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    m_propsOpen = false;
    if (!ImGui::BeginPopupModal("##VelaPropertiesModal", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        return;
    }
    m_propsOpen = true;

    PropertiesState snap;
    {
        std::lock_guard<std::mutex> lock(m_propMutex);
        snap = m_propState;
    }

    ImGui::Text("%s  %s", icons::Info, snap.title.c_str());
    ImGui::Separator();
    ImGui::Spacing();

    auto row = [&](const char* label, const std::string& val) {
        ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.textSecondary.ToImU32());
        ImGui::Text("%-12s", label);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextWrapped("%s", val.c_str());
    };

    row("Type:", snap.typeLabel);
    row("Location:", snap.location);

    const std::string sizeSummary =
        str::FormatFileSize(snap.totalBytes) + " (" + FormatCommas(snap.totalBytes) + " bytes)";
    row("Size:", snap.calculatingStats ? (sizeSummary + " [calculating...]") : sizeSummary);

    if (!snap.isSingleFile) {
        std::string contains = std::to_string(snap.fileCount) + " files, " +
                               std::to_string(snap.folderCount) + " folders";
        if (snap.calculatingStats) contains += "...";
        row("Contains:", contains);
    }

    if (!snap.linkTarget.empty()) {
        row("Target:", snap.linkTarget);
    }

    if (!snap.isMulti) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        row("Created:", snap.createdText);
        row("Modified:", snap.modifiedText);
        row("Accessed:", snap.accessedText);
        row("Attributes:", DescribeAttributes(snap.attributes));
    }

    if (snap.isSingleFile && snap.targetsW.size() == 1) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        if (snap.computingHash) {
            ImGui::TextUnformatted("Computing SHA-256...");
            ImGui::PushStyleColor(ImGuiCol_FrameBg, ToImVec4(cfg.theme.progressTrackBg));
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, cfg.theme.accent.ToImU32());
            ImGui::ProgressBar(snap.hashProgress, ImVec2(-1.0f, 14.0f * dpiScale));
            ImGui::PopStyleColor(2);
        } else if (!snap.sha256Hex.empty()) {
            row("SHA-256:", snap.sha256Hex);
            if (ImGui::SmallButton("Copy SHA-256") && m_window) {
                m_window->SetClipboardText(snap.sha256Hex);
                if (m_onToast) m_onToast(false, "Copied SHA-256 to clipboard");
            }
        } else {
            if (!snap.hashError.empty()) {
                ImGui::TextColored(ToImVec4(cfg.theme.dangerText), "%s", snap.hashError.c_str());
            }
            if (ImGui::Button("Compute SHA-256")) {
                StartSha256Worker(snap.targetsW.front());
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    if (!snap.targetsW.empty() && ImGui::Button("Copy Full Path") && m_window) {
        std::string joined;
        for (const auto& pW : snap.targetsW) {
            if (!joined.empty()) joined += "\r\n";
            joined += str::WideToUtf8(pW);
        }
        m_window->SetClipboardText(joined);
        if (m_onToast) m_onToast(false, "Copied path to clipboard");
    }

    if (!snap.isMulti && !snap.targetsW.empty()) {
        ImGui::SameLine();
        if (ImGui::Button("Windows Properties...") && m_window) {
            m_window->OpenPropertiesDialog(snap.targetsW.front());
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        StopBgWorker();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void VelaDialogs::RenderDropPopup(const Config& cfg) {
    if (m_openDropPopupPending) {
        ImGui::OpenPopup("##ThunarDropPopup");
        m_openDropPopupPending = false;
    }

    m_dropPopupOpen = false;
    if (!ImGui::BeginPopup("##ThunarDropPopup")) {
        return;
    }
    m_dropPopupOpen = true;

    ImGui::PushStyleColor(ImGuiCol_Text, cfg.theme.textSecondary.ToImU32());
    ImGui::TextUnformatted(m_dropHeader.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    auto triggerOp = [&](FileOpKind kind) {
        if (m_fileOps && !m_dropSourcesW.empty() && !m_dropDestDirW.empty()) {
            FileOpRequest req;
            req.kind = kind;
            req.sources = m_dropSourcesW;
            req.destDirW = m_dropDestDirW;
            req.ownerHwnd = m_window ? m_window->GetHandle() : nullptr;
            m_fileOps->Enqueue(std::move(req));
        }
        ImGui::CloseCurrentPopup();
    };

    if (ImGui::MenuItem("Copy Here", nullptr)) {
        triggerOp(FileOpKind::Copy);
    }
    if (ImGui::MenuItem("Move Here", nullptr)) {
        triggerOp(FileOpKind::Move);
    }

    if (cfg.dragDrop.enableSymlink || cfg.dragDrop.enableJunction || cfg.dragDrop.enableHardlink) {
        ImGui::Separator();
        if (cfg.dragDrop.enableSymlink) {
            if (ImGui::MenuItem("Create Symbolic Link Here")) {
                triggerOp(FileOpKind::CreateSymlink);
            }
        }
        if (cfg.dragDrop.enableJunction) {
            if (ImGui::MenuItem("Create Directory Junction Here", nullptr, false, m_dropAllLocalDirs)) {
                triggerOp(FileOpKind::CreateJunction);
            }
            if (!m_dropAllLocalDirs && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Directory Junctions require local folders");
            }
        }
        if (cfg.dragDrop.enableHardlink) {
            if (ImGui::MenuItem("Create Hard Link Here", nullptr, false, m_dropAllFilesSameDrive)) {
                triggerOp(FileOpKind::CreateHardlink);
            }
            if (!m_dropAllFilesSameDrive && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Hard Links require files on the same local NTFS drive");
            }
        }
    }

    ImGui::Separator();
    if (ImGui::MenuItem("Cancel", "Esc") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

} // namespace vela