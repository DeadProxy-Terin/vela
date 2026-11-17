#pragma once

#include "core/Config.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vela {

struct ResolvedPinnedPlace {
    std::string name;
    std::string icon;
    std::string path;
    std::wstring pathW;
    bool isSpecialShell = false;
};

struct DrivePlace {
    std::string displayName; // e.g. "Windows (C:)"
    std::string rootPath;    // e.g. "C:\"
    std::wstring rootPathW;
    uint32_t driveType = 0;  // DRIVE_FIXED, DRIVE_REMOVABLE, DRIVE_CDROM, DRIVE_REMOTE
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    std::string capacityText;
};

struct CloudPlace {
    std::string name;
    std::string path;
    std::wstring pathW;
};

struct WslDistroPlace {
    std::string name;
    std::string path;   // e.g. "\\wsl.localhost\Ubuntu"
    std::wstring pathW;
};

struct SystemPlacesSnapshot {
    std::vector<ResolvedPinnedPlace> pinned;
    std::vector<DrivePlace> drives;
    std::vector<CloudPlace> cloud;
    std::vector<WslDistroPlace> wsl;
    std::string defaultHomePath;
    std::wstring defaultHomePathW;
};

class SystemPlaces {
public:
    static SystemPlacesSnapshot Refresh(const SidebarConfig& sidebarCfg);
    static std::wstring ResolveKnownFolder(std::string_view token);
};

} // namespace vela