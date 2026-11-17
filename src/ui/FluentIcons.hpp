#pragma once

#include <array>
#include <cstdint>

namespace vela::icons {

// Converts BMP PUA Unicode codepoints (U+E700..U+F8FF) to compile-time UTF-8 char*
// safely under C++20 without char8_t type mismatch.
template <char32_t Cp>
struct GlyphLiteral {
    static constexpr auto Make() {
        std::array<char, 4> buf{};
        buf[0] = static_cast<char>(0xE0 | ((Cp >> 12) & 0x0F));
        buf[1] = static_cast<char>(0x80 | ((Cp >> 6) & 0x3F));
        buf[2] = static_cast<char>(0x80 | (Cp & 0x3F));
        buf[3] = '\0';
        return buf;
    }
    static constexpr auto data = Make();
    static constexpr const char* c_str() { return data.data(); }
};

// Navigation & Chrome
inline constexpr const char* Back            = GlyphLiteral<0xE72B>::c_str();
inline constexpr const char* Forward         = GlyphLiteral<0xE72A>::c_str();
inline constexpr const char* Up              = GlyphLiteral<0xE74A>::c_str();
inline constexpr const char* Refresh         = GlyphLiteral<0xE72C>::c_str();
inline constexpr const char* Search          = GlyphLiteral<0xE721>::c_str();
inline constexpr const char* Settings        = GlyphLiteral<0xE713>::c_str();

// Command Toolbar
inline constexpr const char* Add             = GlyphLiteral<0xE710>::c_str();
inline constexpr const char* Cut             = GlyphLiteral<0xE8C6>::c_str();
inline constexpr const char* Copy            = GlyphLiteral<0xE8C8>::c_str();
inline constexpr const char* Paste           = GlyphLiteral<0xE77F>::c_str();
inline constexpr const char* Rename          = GlyphLiteral<0xE8AC>::c_str();
inline constexpr const char* Share           = GlyphLiteral<0xE72D>::c_str();
inline constexpr const char* Delete          = GlyphLiteral<0xE74D>::c_str();
inline constexpr const char* Info            = GlyphLiteral<0xE946>::c_str();
inline constexpr const char* MultiSelect     = GlyphLiteral<0xE762>::c_str();
inline constexpr const char* Sort            = GlyphLiteral<0xE8CB>::c_str();
inline constexpr const char* ViewGrid        = GlyphLiteral<0xF0E2>::c_str();
inline constexpr const char* ViewList        = GlyphLiteral<0xE8FD>::c_str();
inline constexpr const char* ViewDetails     = GlyphLiteral<0xE8A9>::c_str();
inline constexpr const char* PreviewPane     = GlyphLiteral<0xE8A0>::c_str();
inline constexpr const char* More            = GlyphLiteral<0xE712>::c_str();

// Sidebar & Filesystem
inline constexpr const char* Home            = GlyphLiteral<0xE80F>::c_str();
inline constexpr const char* Pin             = GlyphLiteral<0xE718>::c_str();
inline constexpr const char* Folder          = GlyphLiteral<0xE8B7>::c_str();
inline constexpr const char* FolderOpen      = GlyphLiteral<0xE838>::c_str();
inline constexpr const char* FilePage        = GlyphLiteral<0xE7C3>::c_str();
inline constexpr const char* Document        = GlyphLiteral<0xE8A5>::c_str();
inline constexpr const char* Music           = GlyphLiteral<0xEC4F>::c_str();
inline constexpr const char* Desktop         = GlyphLiteral<0xE7F4>::c_str();
inline constexpr const char* Download        = GlyphLiteral<0xE896>::c_str();
inline constexpr const char* Drive           = GlyphLiteral<0xEDA2>::c_str();
inline constexpr const char* Cloud           = GlyphLiteral<0xE753>::c_str();
inline constexpr const char* Network         = GlyphLiteral<0xE968>::c_str();
inline constexpr const char* Terminal        = GlyphLiteral<0xE756>::c_str();
inline constexpr const char* Tag             = GlyphLiteral<0xE8EC>::c_str();

// Actions & Indicators
inline constexpr const char* Link            = GlyphLiteral<0xE71B>::c_str();
inline constexpr const char* OpenInNewWindow = GlyphLiteral<0xE8A7>::c_str();
inline constexpr const char* CheckMark       = GlyphLiteral<0xE73E>::c_str();
inline constexpr const char* Eye             = GlyphLiteral<0xE7B3>::c_str();
inline constexpr const char* ChevronRight    = GlyphLiteral<0xE76C>::c_str();
inline constexpr const char* ChevronDown     = GlyphLiteral<0xE70D>::c_str();
inline constexpr const char* ChevronUp       = GlyphLiteral<0xE70E>::c_str();

// Window Caption Controls
inline constexpr const char* ChromeMinimize  = GlyphLiteral<0xE921>::c_str();
inline constexpr const char* ChromeMaximize  = GlyphLiteral<0xE922>::c_str();
inline constexpr const char* ChromeRestore   = GlyphLiteral<0xE923>::c_str();
inline constexpr const char* ChromeClose     = GlyphLiteral<0xE8BB>::c_str();

} // namespace vela::icons