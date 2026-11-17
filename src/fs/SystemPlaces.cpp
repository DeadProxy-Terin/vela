#include "fs/SystemPlaces.hpp"
#include "core/StringUtil.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <algorithm>
#include <filesystem>

namespace vela {
namespace {

std::wstring GetKnownFolderSafe(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        std::wstring result(raw);
        ::CoTaskMemFree(raw);
        return str::NormalizeWindowsPath(result);
    }
    return {};
}

bool DirExists(const std::wstring& p) {
    if (p.empty()) return false;
    const DWORD attrs = ::GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void TryAddCloud(std::vector<CloudPlace>& out, const std::string& label, const std::wstring& candidate) {
    if (!DirExists(candidate)) return;
    const std::wstring norm = str::NormalizeWindowsPath(candidate);
    for (const auto& existing : out) {
        if (_wcsicmp(existing.pathW.c_str(), norm.c_str()) == 0) return;
    }
    out.push_back(CloudPlace{label, str::WideToUtf8(norm), norm});
}

} // namespace

std::wstring SystemPlaces::ResolveKnownFolder(std::string_view token) {
    const std::string lower = str::ToLowerAscii(token);
    if (lower == "known:desktop")   return GetKnownFolderSafe(FOLDERID_Desktop);
    if (lower == "known:downloads") return GetKnownFolderSafe(FOLDERID_Downloads);
    if (lower == "known:documents") return GetKnownFolderSafe(FOLDERID_Documents);
    if (lower == "known:music")     return GetKnownFolderSafe(FOLDERID_Music);
    if (lower == "known:pictures")  return GetKnownFolderSafe(FOLDERID_Pictures);
    if (lower == "known:videos")    return GetKnownFolderSafe(FOLDERID_Videos);
    if (lower == "known:profile")   return GetKnownFolderSafe(FOLDERID_Profile);
    return {};
}

SystemPlacesSnapshot SystemPlaces::Refresh(const SidebarConfig& sidebarCfg) {
    SystemPlacesSnapshot snap;

    snap.defaultHomePathW = GetKnownFolderSafe(FOLDERID_Profile);
    if (snap.defaultHomePathW.empty()) {
        snap.defaultHomePathW = L"C:\\";
    }
    snap.defaultHomePath = str::WideToUtf8(snap.defaultHomePathW);

    // 1. Pinned entries from vela.lua
    for (const auto& entry : sidebarCfg.pinned) {
        ResolvedPinnedPlace place;
        place.name = entry.name;
        place.icon = entry.icon;

        const std::string lowerPath = str::ToLowerAscii(entry.path);
        if (lowerPath.rfind("shell:", 0) == 0) {
            place.isSpecialShell = true;
            place.path = entry.path;
            place.pathW = str::Utf8ToWide(entry.path);
        } else if (lowerPath.rfind("known:", 0) == 0) {
            place.pathW = ResolveKnownFolder(entry.path);
            place.path = str::WideToUtf8(place.pathW);
        } else {
            place.pathW = str::NormalizeWindowsPath(str::ExpandEnvVars(str::Utf8ToWide(entry.path)));
            place.path = str::WideToUtf8(place.pathW);
        }

        if (!place.pathW.empty()) {
            snap.pinned.push_back(std::move(place));
        }
    }

    // 2. Logical Drives
    const DWORD bufLen = ::GetLogicalDriveStringsW(0, nullptr);
    if (bufLen > 0) {
        std::wstring buffer(bufLen + 1, L'\0');
        const DWORD written = ::GetLogicalDriveStringsW(bufLen, buffer.data());
        if (written > 0) {
            const wchar_t* ptr = buffer.c_str();
            while (*ptr != L'\0') {
                std::wstring root(ptr);
                ptr += root.size() + 1;

                const UINT dtype = ::GetDriveTypeW(root.c_str());
                if (dtype == DRIVE_UNKNOWN || dtype == DRIVE_NO_ROOT_DIR) continue;

                DrivePlace drv;
                drv.rootPathW = str::NormalizeWindowsPath(root);
                drv.rootPath = str::WideToUtf8(drv.rootPathW);
                drv.driveType = dtype;

                wchar_t volName[MAX_PATH]{};
                ::GetVolumeInformationW(drv.rootPathW.c_str(), volName, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);

                std::string label = str::WideToUtf8(volName);
                if (label.empty()) {
                    if (dtype == DRIVE_CDROM) label = "DVD Drive";
                    else if (dtype == DRIVE_REMOVABLE) label = "Removable Disk";
                    else if (dtype == DRIVE_REMOTE) label = "Network Drive";
                    else label = "Local Disk";
                }

                // e.g. "Windows (C:)"
                std::string letterTag = drv.rootPath;
                if (!letterTag.empty() && letterTag.back() == '\\') {
                    letterTag.pop_back();
                }
                drv.displayName = label + " (" + letterTag + ")";

                ULARGE_INTEGER freeAvail{}, totalBytes{};
                if (::GetDiskFreeSpaceExW(drv.rootPathW.c_str(), &freeAvail, &totalBytes, nullptr) && totalBytes.QuadPart > 0) {
                    drv.freeBytes = freeAvail.QuadPart;
                    drv.totalBytes = totalBytes.QuadPart;
                    drv.capacityText = str::FormatFileSize(drv.freeBytes) + " free of " + str::FormatFileSize(drv.totalBytes);
                }

                snap.drives.push_back(std::move(drv));
            }
        }
    }

    // 3. Cloud Storage
    TryAddCloud(snap.cloud, "OneDrive",          str::ExpandEnvVars(L"%OneDrive%"));
    TryAddCloud(snap.cloud, "OneDrive Work",     str::ExpandEnvVars(L"%OneDriveCommercial%"));
    TryAddCloud(snap.cloud, "iCloud Drive",      str::ExpandEnvVars(L"%USERPROFILE%\\iCloudDrive"));
    TryAddCloud(snap.cloud, "Google Drive",      str::ExpandEnvVars(L"%USERPROFILE%\\Google Drive"));
    TryAddCloud(snap.cloud, "Google Drive (G:)", L"G:\\My Drive");

    // 4. WSL Distributions (via Registry, zero wsl.exe process spawn)
    HKEY lxssKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss",
                        0, KEY_READ, &lxssKey) == ERROR_SUCCESS) {
        DWORD subkeyIndex = 0;
        wchar_t subkeyName[256]{};
        DWORD subkeyLen = 256;

        while (::RegEnumKeyExW(lxssKey, subkeyIndex++, subkeyName, &subkeyLen, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
            HKEY distroKey = nullptr;
            if (::RegOpenKeyExW(lxssKey, subkeyName, 0, KEY_READ, &distroKey) == ERROR_SUCCESS) {
                wchar_t distroName[256]{};
                DWORD dataSize = sizeof(distroName);
                if (::RegGetValueW(distroKey, nullptr, L"DistributionName", RRF_RT_REG_SZ, nullptr, distroName, &dataSize) == ERROR_SUCCESS) {
                    WslDistroPlace wsl;
                    wsl.name = str::WideToUtf8(distroName);
                    wsl.pathW = L"\\\\wsl.localhost\\" + std::wstring(distroName);
                    wsl.path = str::WideToUtf8(wsl.pathW);
                    snap.wsl.push_back(std::move(wsl));
                }
                ::RegCloseKey(distroKey);
            }
            subkeyLen = 256;
        }
        ::RegCloseKey(lxssKey);
    }

    return snap;
}

} // namespace vela