#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace vela {

struct ColorRGBA {
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
    float a = 1.0f;

    constexpr ColorRGBA WithAlpha(float alpha) const {
        return ColorRGBA{r, g, b, alpha};
    }

    constexpr uint32_t ToImU32() const {
        const auto cr = static_cast<uint32_t>(r * 255.0f + 0.5f) & 0xFF;
        const auto cg = static_cast<uint32_t>(g * 255.0f + 0.5f) & 0xFF;
        const auto cb = static_cast<uint32_t>(b * 255.0f + 0.5f) & 0xFF;
        const auto ca = static_cast<uint32_t>(a * 255.0f + 0.5f) & 0xFF;
        return (ca << 24) | (cb << 16) | (cg << 8) | cr;
    }
};

enum class BackdropMode {
    MicaAlt,
    Mica,
    Acrylic,
    Solid
};

struct WindowConfig {
    int width = 1280;
    int height = 820;
    int minWidth = 760;
    int minHeight = 500;
    BackdropMode backdrop = BackdropMode::MicaAlt;
    bool darkMode = true;
    float titlebarHeight = 42.0f;
};

struct ThemeConfig {
    std::string fontPath;
    float fontSize = 14.5f;

    // Surface & Chrome Colors
    ColorRGBA chromeBg           {0.000f, 0.000f, 0.000f, 0.000f};
    ColorRGBA sidebarBg          {0.000f, 0.000f, 0.000f, 0.000f};
    ColorRGBA tabActiveBg        {0.173f, 0.173f, 0.173f, 0.910f};
    ColorRGBA tabHoverBg         {1.000f, 1.000f, 1.000f, 0.063f};
    ColorRGBA toolbarBg          {1.000f, 1.000f, 1.000f, 0.024f};
    ColorRGBA statusbarBg        {0.000f, 0.000f, 0.000f, 0.000f};
    ColorRGBA contentCardBg      {0.125f, 0.125f, 0.125f, 0.847f};
    ColorRGBA contentCardBorder  {1.000f, 1.000f, 1.000f, 0.071f};

    // Popups, Modals, Controls & Selection
    ColorRGBA popupBg            {0.160f, 0.160f, 0.170f, 0.970f};
    ColorRGBA popupBorder        {1.000f, 1.000f, 1.000f, 0.120f};
    ColorRGBA inputBg            {1.000f, 1.000f, 1.000f, 0.051f};
    ColorRGBA inputBorder        {1.000f, 1.000f, 1.000f, 0.078f};
    ColorRGBA buttonBg           {1.000f, 1.000f, 1.000f, 0.080f};
    ColorRGBA buttonHoverBg      {1.000f, 1.000f, 1.000f, 0.130f};
    ColorRGBA buttonActiveBg     {1.000f, 1.000f, 1.000f, 0.180f};
    ColorRGBA hoverBg            {1.000f, 1.000f, 1.000f, 0.063f};
    ColorRGBA selectionBg        {1.000f, 1.000f, 1.000f, 0.086f};
    ColorRGBA separator          {1.000f, 1.000f, 1.000f, 0.100f};
    ColorRGBA accent             {0.376f, 0.804f, 1.000f, 1.000f};

    // Scrollbars, Progress & Semantic Colors
    ColorRGBA scrollbarBg        {0.000f, 0.000f, 0.000f, 0.000f};
    ColorRGBA scrollbarGrab      {1.000f, 1.000f, 1.000f, 0.220f};
    ColorRGBA scrollbarGrabHover {1.000f, 1.000f, 1.000f, 0.350f};
    ColorRGBA progressTrackBg    {1.000f, 1.000f, 1.000f, 0.110f};
    ColorRGBA toastBg            {0.141f, 0.141f, 0.157f, 0.922f};
    ColorRGBA toastErrorBg       {0.549f, 0.125f, 0.125f, 0.922f};
    ColorRGBA danger             {0.686f, 0.165f, 0.165f, 1.000f};
    ColorRGBA dangerText         {1.000f, 0.471f, 0.471f, 1.000f};
    ColorRGBA closeHoverBg       {0.769f, 0.169f, 0.110f, 1.000f};

    // Typography
    ColorRGBA textPrimary        {1.000f, 1.000f, 1.000f, 0.949f};
    ColorRGBA textSecondary      {1.000f, 1.000f, 1.000f, 0.651f};
    ColorRGBA textMuted          {1.000f, 1.000f, 1.000f, 0.431f};
    float cutItemAlpha = 0.45f;

    float cardRounding = 8.0f;
    float itemRounding = 6.0f;
    float pillRounding = 6.0f;
};

struct LayoutConfig {
    bool showToolbar = true;
    bool showSidebar = true;
    bool showStatusbar = true;
    std::string sidebarPosition = "left";   // "left" | "right"
    float sidebarWidth = 235.0f;
    float navbarHeight = 44.0f;
    float toolbarHeight = 42.0f;
    float statusbarHeight = 30.0f;

    std::string defaultView = "grid";       // "grid" | "list" | "details"
    std::string defaultSortBy = "name";     // "name" | "date_modified" | "type" | "size"
    std::string defaultSortOrder = "asc";   // "asc" | "desc"
    bool foldersFirst = true;
    bool showHidden = false;
    bool showExtensions = true;

    float gridTileWidth = 132.0f;
    float gridIconSize = 64.0f;
    float gridGap = 10.0f;
    float listRowHeight = 30.0f;
    float listIconSize = 18.0f;
    float detailsRowHeight = 26.0f;
    float detailsIconSize = 16.0f;
    bool detailsShowDate = true;
    bool detailsShowType = true;
    bool detailsShowSize = true;
    int thumbnailCacheMax = 512;
};

struct PinnedEntry {
    std::string name;
    std::string path;
    std::string icon;
};

struct TagEntry {
    std::string name;
    ColorRGBA color{0.376f, 0.804f, 1.000f, 1.000f};
};

struct SidebarConfig {
    bool showHome = true;
    bool showPinned = true;
    bool showDrives = true;
    bool showCloudStorage = true;
    bool showNetwork = true;
    bool showWsl = true;
    bool showTags = true;
    std::vector<PinnedEntry> pinned;
    std::vector<TagEntry> tags;
};

struct TerminalConfig {
    std::string program = "wt.exe";
    std::string args = "-d";
};

struct IconsConfig {
    std::string provider = "windows_shell";
    std::string customPack;
    std::unordered_map<std::string, std::string> overrides;
};

struct DragDropConfig {
    std::string mode = "ask_always";
    bool enableSymlink = true;
    bool enableJunction = true;
    bool enableHardlink = true;
};

struct KeybindsConfig {
    std::string reloadConfig    = "Ctrl+Shift+R";
    std::string refresh         = "F5";
    std::string newTab          = "Ctrl+T";
    std::string closeTab        = "Ctrl+W";
    std::string focusAddress    = "Ctrl+L";
    std::string focusSearch     = "Ctrl+F";
    std::string copyPath        = "Ctrl+Shift+C";
    std::string copyPathPosix   = "Ctrl+Alt+C";
    std::string copyPathQuoted  = "Ctrl+Shift+Q";
    std::string toggleHidden    = "Ctrl+H";
    std::string toggleSidebar   = "Ctrl+B";
    std::string openTerminal    = "Ctrl+Shift+T";
    std::string goUp            = "Alt+Up";
    std::string goBack          = "Alt+Left";
    std::string goForward       = "Alt+Right";

    // File operations & selection
    std::string copy            = "Ctrl+C";
    std::string cut             = "Ctrl+X";
    std::string paste           = "Ctrl+V";
    std::string newFolder       = "Ctrl+Shift+N";
    std::string rename          = "F2";
    std::string deleteItem      = "Delete";
    std::string deletePermanent = "Shift+Delete";
    std::string properties      = "Alt+Enter";
    std::string selectAll       = "Ctrl+A";
};

struct FolderRule {
    std::string match;
    std::string sortBy;
    std::string order;
    std::string view;
};

struct Config {
    WindowConfig window;
    ThemeConfig theme;
    LayoutConfig layout;
    SidebarConfig sidebar;
    TerminalConfig terminal;
    IconsConfig icons;
    DragDropConfig dragDrop;
    KeybindsConfig keybinds;
    std::vector<FolderRule> rules;
};

} // namespace vela