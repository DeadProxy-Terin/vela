#pragma once

#include <windows.h>
#include <shlwapi.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace vela::str {

inline std::wstring Utf8ToWide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0
    );
    if (needed <= 0) return {};
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed
    );
    return out;
}

inline std::string WideToUtf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr
    );
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed, nullptr, nullptr
    );
    return out;
}

inline std::wstring ExpandEnvVars(std::wstring_view input) {
    if (input.empty()) return {};
    std::wstring src(input);
    const DWORD needed = ::ExpandEnvironmentStringsW(src.c_str(), nullptr, 0);
    if (needed == 0) return src;
    std::wstring out(needed, L'\0');
    const DWORD written = ::ExpandEnvironmentStringsW(src.c_str(), out.data(), needed);
    if (written > 0 && written <= needed) {
        out.resize(written - 1);
        return out;
    }
    return src;
}

inline std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

inline std::wstring ToLowerWide(std::wstring_view s) {
    if (s.empty()) return {};
    std::wstring out(s);
    ::CharLowerBuffW(out.data(), static_cast<DWORD>(out.size()));
    return out;
}

inline bool ContainsCaseInsensitive(std::string_view haystackLower, std::string_view needle) {
    if (needle.empty()) return true;
    const std::string needleLower = ToLowerAscii(needle);
    return haystackLower.find(needleLower) != std::string_view::npos;
}

inline int NaturalCompareWide(const wchar_t* a, const wchar_t* b) {
    return ::StrCmpLogicalW(a ? a : L"", b ? b : L"");
}

inline std::string ToPosixPath(std::string_view path) {
    std::string out(path);
    std::replace(out.begin(), out.end(), '\\', '/');
    return out;
}

inline std::string QuotePath(std::string_view path) {
    std::string out;
    out.reserve(path.size() + 2);
    out.push_back('"');
    out.append(path);
    out.push_back('"');
    return out;
}

inline std::wstring NormalizeWindowsPath(std::wstring_view raw) {
    if (raw.empty()) return {};
    std::wstring out;
    out.reserve(raw.size());

    for (size_t i = 0; i < raw.size(); ++i) {
        wchar_t ch = (raw[i] == L'/') ? L'\\' : raw[i];
        // Collapse duplicate backslashes except leading UNC prefix ("\\")
        if (ch == L'\\' && !out.empty() && out.back() == L'\\' && out.size() > 1) {
            continue;
        }
        out.push_back(ch);
    }

    // Keep trailing slash only for drive roots like "C:\"
    while (out.size() > 3 && out.back() == L'\\') {
        out.pop_back();
    }
    if (out.size() == 2 && out[1] == L':') {
        out.push_back(L'\\');
    }
    return out;
}

inline std::wstring GetParentWindowsPath(std::wstring_view raw) {
    std::wstring path = NormalizeWindowsPath(raw);
    if (path.empty()) return {};
    // Drive root (e.g. "C:\") has no parent directory
    if (path.size() <= 3 && path.size() >= 2 && path[1] == L':') {
        return {};
    }
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    const size_t pos = path.find_last_of(L'\\');
    if (pos == std::wstring::npos || pos == 0) {
        return {};
    }
    // Preserve UNC prefix ("\\server\share") check
    if (pos == 1 && path[0] == L'\\') {
        return {};
    }
    std::wstring parent = path.substr(0, pos);
    if (parent.size() == 2 && parent[1] == L':') {
        parent.push_back(L'\\');
    }
    return parent;
}

inline std::string FormatFileSize(uint64_t bytes) {
    char buf[64];
    if (bytes < 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL * 1024ULL) {
        std::snprintf(buf, sizeof(buf), "%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f TB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
    return std::string(buf);
}

inline std::string FormatFileTime(uint64_t fileTime64) {
    if (fileTime64 == 0) return {};
    FILETIME ft{};
    ft.dwLowDateTime = static_cast<DWORD>(fileTime64 & 0xFFFFFFFFULL);
    ft.dwHighDateTime = static_cast<DWORD>((fileTime64 >> 32) & 0xFFFFFFFFULL);

    FILETIME localFt{};
    SYSTEMTIME st{};
    if (!::FileTimeToLocalFileTime(&ft, &localFt)) return {};
    if (!::FileTimeToSystemTime(&localFt, &st)) return {};

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04u-%02u-%02u %02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return std::string(buf);
}

} // namespace vela::str