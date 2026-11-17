#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace vela {

enum class ViewMode {
    Grid,
    List,
    Details
};

enum class SortField {
    Name,
    DateModified,
    Type,
    Size
};

enum class SortOrder {
    Ascending,
    Descending
};

struct FileEntry {
    std::wstring nameW;
    std::string name;
    std::string nameLower;
    std::string displayName; // Name without extension if show_extensions == false

    std::wstring fullPathW;
    std::string fullPath;

    std::string extensionLower;
    std::string typeLabel;
    std::string sizeFormatted;
    std::string dateFormatted;
    std::string thumbKey; // Smart deduplication key for ThumbnailCache

    uint64_t sizeBytes = 0;
    uint64_t lastWriteTime = 0;
    DWORD attributes = 0;

    bool isDir = false;
    bool isHidden = false;
    bool isSymlink = false;
};

struct DirectoryListing {
    uint64_t generation = 0;
    std::wstring directoryW;
    std::string directory;
    std::vector<FileEntry> entries;
    std::string errorMessage;
    bool success = true;
};

} // namespace vela