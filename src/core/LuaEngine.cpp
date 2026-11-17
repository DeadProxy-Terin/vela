#include "core/LuaEngine.hpp"
#include "core/StringUtil.hpp"

#include <sol/sol.hpp>
#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace vela {
namespace {

bool ParseHexColor(std::string_view hex, ColorRGBA& out) {
    if (!hex.empty() && hex.front() == '#') {
        hex.remove_prefix(1);
    }
    if (hex.size() != 6 && hex.size() != 8) {
        return false;
    }

    auto parseByte = [](std::string_view pair, uint8_t& val) -> bool {
        if (!std::isxdigit(static_cast<unsigned char>(pair[0])) ||
            !std::isxdigit(static_cast<unsigned char>(pair[1]))) {
            return false;
        }
        char buf[3] = {pair[0], pair[1], '\0'};
        char* end = nullptr;
        const long v = std::strtol(buf, &end, 16);
        if (end != buf + 2) return false;
        val = static_cast<uint8_t>(v);
        return true;
    };

    uint8_t r = 255, g = 255, b = 255, a = 255;
    if (!parseByte(hex.substr(0, 2), r)) return false;
    if (!parseByte(hex.substr(2, 2), g)) return false;
    if (!parseByte(hex.substr(4, 2), b)) return false;
    if (hex.size() == 8 && !parseByte(hex.substr(6, 2), a)) return false;

    out.r = static_cast<float>(r) / 255.0f;
    out.g = static_cast<float>(g) / 255.0f;
    out.b = static_cast<float>(b) / 255.0f;
    out.a = static_cast<float>(a) / 255.0f;
    return true;
}

void ReadColor(const sol::table& tbl, const char* key, ColorRGBA& target) {
    if (sol::optional<std::string> hex = tbl[key]; hex.has_value()) {
        ColorRGBA parsed;
        if (ParseHexColor(*hex, parsed)) {
            target = parsed;
        }
    }
}

void ReadString(const sol::table& tbl, const char* key, std::string& target) {
    if (sol::optional<std::string> val = tbl[key]; val.has_value()) {
        target = *val;
    }
}

BackdropMode ParseBackdrop(std::string_view raw) {
    const std::string s = str::ToLowerAscii(raw);
    if (s == "mica") return BackdropMode::Mica;
    if (s == "acrylic") return BackdropMode::Acrylic;
    if (s == "solid" || s == "none") return BackdropMode::Solid;
    return BackdropMode::MicaAlt;
}

void ApplyLightThemeDefaults(ThemeConfig& t) {
    t.chromeBg           = {0.000f, 0.000f, 0.000f, 0.000f};
    t.sidebarBg          = {0.000f, 0.000f, 0.000f, 0.000f};
    t.tabActiveBg        = {1.000f, 1.000f, 1.000f, 0.910f};
    t.tabHoverBg         = {0.000f, 0.000f, 0.000f, 0.045f};
    t.toolbarBg          = {1.000f, 1.000f, 1.000f, 0.350f};
    t.statusbarBg        = {0.000f, 0.000f, 0.000f, 0.000f};
    t.contentCardBg      = {1.000f, 1.000f, 1.000f, 0.880f};
    t.contentCardBorder  = {0.000f, 0.000f, 0.000f, 0.080f};

    t.popupBg            = {0.980f, 0.980f, 0.990f, 0.970f};
    t.popupBorder        = {0.000f, 0.000f, 0.000f, 0.110f};
    t.inputBg            = {1.000f, 1.000f, 1.000f, 0.720f};
    t.inputBorder        = {0.000f, 0.000f, 0.000f, 0.100f};
    t.buttonBg           = {0.000f, 0.000f, 0.000f, 0.045f};
    t.buttonHoverBg      = {0.000f, 0.000f, 0.000f, 0.085f};
    t.buttonActiveBg     = {0.000f, 0.000f, 0.000f, 0.125f};
    t.hoverBg            = {0.000f, 0.000f, 0.000f, 0.050f};
    t.selectionBg        = {0.000f, 0.404f, 0.753f, 0.130f};
    t.separator          = {0.000f, 0.000f, 0.000f, 0.090f};
    t.accent             = {0.000f, 0.404f, 0.753f, 1.000f};

    t.scrollbarBg        = {0.000f, 0.000f, 0.000f, 0.000f};
    t.scrollbarGrab      = {0.000f, 0.000f, 0.000f, 0.220f};
    t.scrollbarGrabHover = {0.000f, 0.000f, 0.000f, 0.360f};
    t.progressTrackBg    = {0.000f, 0.000f, 0.000f, 0.100f};
    t.toastBg            = {0.965f, 0.965f, 0.975f, 0.950f};
    t.toastErrorBg       = {0.990f, 0.880f, 0.880f, 0.960f};
    t.danger             = {0.769f, 0.169f, 0.110f, 1.000f};
    t.dangerText         = {0.769f, 0.169f, 0.110f, 1.000f};
    t.closeHoverBg       = {0.769f, 0.169f, 0.110f, 1.000f};

    t.textPrimary        = {0.094f, 0.094f, 0.094f, 0.920f};
    t.textSecondary      = {0.094f, 0.094f, 0.094f, 0.620f};
    t.textMuted          = {0.094f, 0.094f, 0.094f, 0.400f};
}

} // namespace

bool LuaEngine::LoadConfig(const std::filesystem::path& filePath, Config& outConfig, std::string& outError) {
    sol::state lua;
    lua.open_libraries(sol::lib::base, sol::lib::table, sol::lib::string, sol::lib::math, sol::lib::os);

    const auto pathU8 = filePath.u8string();
    const std::string pathStr(reinterpret_cast<const char*>(pathU8.data()), pathU8.size());

    sol::protected_function_result result = lua.safe_script_file(pathStr, sol::script_pass_on_error);
    if (!result.valid()) {
        sol::error err = result;
        outError = err.what();
        return false;
    }

    sol::table root;
    if (result.get_type() == sol::type::table) {
        root = result.get<sol::table>();
    } else if (lua["vela"].get_type() == sol::type::table) {
        root = lua["vela"];
    } else {
        outError = "vela.lua must return a configuration table.";
        return false;
    }

    Config next;

    // 1. Window
    if (sol::optional<sol::table> win = root["window"]; win.has_value()) {
        next.window.width = std::clamp(win->get_or("width", next.window.width), 480, 7680);
        next.window.height = std::clamp(win->get_or("height", next.window.height), 360, 4320);
        next.window.minWidth = std::clamp(win->get_or("min_width", next.window.minWidth), 400, 2000);
        next.window.minHeight = std::clamp(win->get_or("min_height", next.window.minHeight), 300, 2000);
        next.window.darkMode = win->get_or("dark_mode", next.window.darkMode);
        next.window.titlebarHeight = std::clamp(win->get_or("titlebar_height", next.window.titlebarHeight), 32.0f, 72.0f);
        if (sol::optional<std::string> bd = (*win)["backdrop"]; bd.has_value()) {
            next.window.backdrop = ParseBackdrop(*bd);
        }
    }

    if (!next.window.darkMode) {
        ApplyLightThemeDefaults(next.theme);
    }

    // 2. Theme
    if (sol::optional<sol::table> th = root["theme"]; th.has_value()) {
        ReadString(*th, "font_path", next.theme.fontPath);
        next.theme.fontSize = std::clamp(th->get_or("font_size", next.theme.fontSize), 10.0f, 32.0f);

        ReadColor(*th, "chrome_bg", next.theme.chromeBg);
        ReadColor(*th, "sidebar_bg", next.theme.sidebarBg);
        ReadColor(*th, "tab_active_bg", next.theme.tabActiveBg);
        ReadColor(*th, "tab_hover_bg", next.theme.tabHoverBg);
        ReadColor(*th, "toolbar_bg", next.theme.toolbarBg);
        ReadColor(*th, "statusbar_bg", next.theme.statusbarBg);
        ReadColor(*th, "content_card_bg", next.theme.contentCardBg);
        ReadColor(*th, "content_card_border", next.theme.contentCardBorder);

        ReadColor(*th, "popup_bg", next.theme.popupBg);
        ReadColor(*th, "popup_border", next.theme.popupBorder);
        ReadColor(*th, "input_bg", next.theme.inputBg);
        ReadColor(*th, "input_border", next.theme.inputBorder);
        ReadColor(*th, "button_bg", next.theme.buttonBg);
        ReadColor(*th, "button_hover_bg", next.theme.buttonHoverBg);
        ReadColor(*th, "button_active_bg", next.theme.buttonActiveBg);
        ReadColor(*th, "hover_bg", next.theme.hoverBg);
        ReadColor(*th, "selection_bg", next.theme.selectionBg);
        ReadColor(*th, "separator", next.theme.separator);
        ReadColor(*th, "accent", next.theme.accent);

        ReadColor(*th, "scrollbar_bg", next.theme.scrollbarBg);
        ReadColor(*th, "scrollbar_grab", next.theme.scrollbarGrab);
        ReadColor(*th, "scrollbar_grab_hover", next.theme.scrollbarGrabHover);
        ReadColor(*th, "progress_track_bg", next.theme.progressTrackBg);
        ReadColor(*th, "toast_bg", next.theme.toastBg);
        ReadColor(*th, "toast_error_bg", next.theme.toastErrorBg);
        ReadColor(*th, "danger", next.theme.danger);
        ReadColor(*th, "danger_text", next.theme.dangerText);
        ReadColor(*th, "close_hover_bg", next.theme.closeHoverBg);

        ReadColor(*th, "text_primary", next.theme.textPrimary);
        ReadColor(*th, "text_secondary", next.theme.textSecondary);
        ReadColor(*th, "text_muted", next.theme.textMuted);
        next.theme.cutItemAlpha = std::clamp(th->get_or("cut_item_alpha", next.theme.cutItemAlpha), 0.1f, 1.0f);

        next.theme.cardRounding = std::clamp(th->get_or("card_rounding", next.theme.cardRounding), 0.0f, 24.0f);
        next.theme.itemRounding = std::clamp(th->get_or("item_rounding", next.theme.itemRounding), 0.0f, 20.0f);
        next.theme.pillRounding = std::clamp(th->get_or("pill_rounding", next.theme.pillRounding), 0.0f, 20.0f);
    }

    // 3. Layout
    if (sol::optional<sol::table> lay = root["layout"]; lay.has_value()) {
        next.layout.showToolbar = lay->get_or("show_toolbar", next.layout.showToolbar);
        next.layout.showSidebar = lay->get_or("show_sidebar", next.layout.showSidebar);
        next.layout.showStatusbar = lay->get_or("show_statusbar", next.layout.showStatusbar);
        if (sol::optional<std::string> pos = (*lay)["sidebar_position"]; pos.has_value()) {
            const std::string lower = str::ToLowerAscii(*pos);
            next.layout.sidebarPosition = (lower == "right") ? "right" : "left";
        }
        next.layout.sidebarWidth = std::clamp(lay->get_or("sidebar_width", next.layout.sidebarWidth), 150.0f, 480.0f);
        next.layout.navbarHeight = std::clamp(lay->get_or("navbar_height", next.layout.navbarHeight), 34.0f, 64.0f);
        next.layout.toolbarHeight = std::clamp(lay->get_or("toolbar_height", next.layout.toolbarHeight), 34.0f, 64.0f);
        next.layout.statusbarHeight = std::clamp(lay->get_or("statusbar_height", next.layout.statusbarHeight), 22.0f, 48.0f);

        ReadString(*lay, "default_view", next.layout.defaultView);
        ReadString(*lay, "default_sort_by", next.layout.defaultSortBy);
        ReadString(*lay, "default_sort_order", next.layout.defaultSortOrder);
        next.layout.foldersFirst = lay->get_or("folders_first", next.layout.foldersFirst);
        next.layout.showHidden = lay->get_or("show_hidden", next.layout.showHidden);
        next.layout.showExtensions = lay->get_or("show_extensions", next.layout.showExtensions);

        next.layout.gridTileWidth = std::clamp(lay->get_or("grid_tile_width", next.layout.gridTileWidth), 80.0f, 280.0f);
        next.layout.gridIconSize = std::clamp(lay->get_or("grid_icon_size", next.layout.gridIconSize), 32.0f, 160.0f);
        next.layout.gridGap = std::clamp(lay->get_or("grid_gap", next.layout.gridGap), 2.0f, 40.0f);
        next.layout.listRowHeight = std::clamp(lay->get_or("list_row_height", next.layout.listRowHeight), 20.0f, 64.0f);
        next.layout.listIconSize = std::clamp(lay->get_or("list_icon_size", next.layout.listIconSize), 12.0f, 48.0f);
        next.layout.detailsRowHeight = std::clamp(lay->get_or("details_row_height", next.layout.detailsRowHeight), 20.0f, 64.0f);
        next.layout.detailsIconSize = std::clamp(lay->get_or("details_icon_size", next.layout.detailsIconSize), 12.0f, 48.0f);
        next.layout.detailsShowDate = lay->get_or("details_show_date", next.layout.detailsShowDate);
        next.layout.detailsShowType = lay->get_or("details_show_type", next.layout.detailsShowType);
        next.layout.detailsShowSize = lay->get_or("details_show_size", next.layout.detailsShowSize);
        next.layout.thumbnailCacheMax = std::clamp(lay->get_or("thumbnail_cache_max", next.layout.thumbnailCacheMax), 64, 4096);
    }

    // 4. Sidebar
    if (sol::optional<sol::table> sb = root["sidebar"]; sb.has_value()) {
        next.sidebar.showHome = sb->get_or("show_home", next.sidebar.showHome);
        next.sidebar.showPinned = sb->get_or("show_pinned", next.sidebar.showPinned);
        next.sidebar.showDrives = sb->get_or("show_drives", next.sidebar.showDrives);
        next.sidebar.showCloudStorage = sb->get_or("show_cloud_storage", next.sidebar.showCloudStorage);
        next.sidebar.showNetwork = sb->get_or("show_network", next.sidebar.showNetwork);
        next.sidebar.showWsl = sb->get_or("show_wsl", next.sidebar.showWsl);
        next.sidebar.showTags = sb->get_or("show_tags", next.sidebar.showTags);

        if (sol::optional<sol::table> pinned = (*sb)["pinned"]; pinned.has_value()) {
            for (size_t i = 1; i <= pinned->size(); ++i) {
                sol::object item = (*pinned)[i];
                if (item.get_type() == sol::type::table) {
                    sol::table t = item.as<sol::table>();
                    PinnedEntry entry;
                    ReadString(t, "name", entry.name);
                    ReadString(t, "path", entry.path);
                    ReadString(t, "icon", entry.icon);
                    if (!entry.name.empty()) {
                        next.sidebar.pinned.push_back(std::move(entry));
                    }
                }
            }
        }

        if (sol::optional<sol::table> tags = (*sb)["tags"]; tags.has_value()) {
            for (size_t i = 1; i <= tags->size(); ++i) {
                sol::object item = (*tags)[i];
                if (item.get_type() == sol::type::table) {
                    sol::table t = item.as<sol::table>();
                    TagEntry tag;
                    ReadString(t, "name", tag.name);
                    ReadColor(t, "color", tag.color);
                    if (!tag.name.empty()) {
                        next.sidebar.tags.push_back(std::move(tag));
                    }
                }
            }
        }
    }

    // 5. Terminal
    if (sol::optional<sol::table> term = root["terminal"]; term.has_value()) {
        ReadString(*term, "program", next.terminal.program);
        ReadString(*term, "args", next.terminal.args);
    }

    // 6. Icons
    if (sol::optional<sol::table> ic = root["icons"]; ic.has_value()) {
        ReadString(*ic, "provider", next.icons.provider);
        ReadString(*ic, "custom_pack", next.icons.customPack);
        if (sol::optional<sol::table> ov = (*ic)["overrides"]; ov.has_value()) {
            for (const auto& kv : *ov) {
                if (kv.first.get_type() == sol::type::string && kv.second.get_type() == sol::type::string) {
                    next.icons.overrides[str::ToLowerAscii(kv.first.as<std::string>())] = kv.second.as<std::string>();
                }
            }
        }
    }

    // 7. Drag & Drop
    if (sol::optional<sol::table> dd = root["drag_drop"]; dd.has_value()) {
        ReadString(*dd, "mode", next.dragDrop.mode);
        next.dragDrop.enableSymlink = dd->get_or("enable_symlink", next.dragDrop.enableSymlink);
        next.dragDrop.enableJunction = dd->get_or("enable_junction", next.dragDrop.enableJunction);
        next.dragDrop.enableHardlink = dd->get_or("enable_hardlink", next.dragDrop.enableHardlink);
    }

    // 8. Keybinds
    if (sol::optional<sol::table> kb = root["keybinds"]; kb.has_value()) {
        ReadString(*kb, "reload_config", next.keybinds.reloadConfig);
        ReadString(*kb, "refresh", next.keybinds.refresh);
        ReadString(*kb, "new_tab", next.keybinds.newTab);
        ReadString(*kb, "close_tab", next.keybinds.closeTab);
        ReadString(*kb, "focus_address", next.keybinds.focusAddress);
        ReadString(*kb, "focus_search", next.keybinds.focusSearch);
        ReadString(*kb, "copy_path", next.keybinds.copyPath);
        ReadString(*kb, "copy_path_posix", next.keybinds.copyPathPosix);
        ReadString(*kb, "copy_path_quoted", next.keybinds.copyPathQuoted);
        ReadString(*kb, "toggle_hidden", next.keybinds.toggleHidden);
        ReadString(*kb, "toggle_sidebar", next.keybinds.toggleSidebar);
        ReadString(*kb, "open_terminal", next.keybinds.openTerminal);
        ReadString(*kb, "go_up", next.keybinds.goUp);
        ReadString(*kb, "go_back", next.keybinds.goBack);
        ReadString(*kb, "go_forward", next.keybinds.goForward);

        ReadString(*kb, "copy", next.keybinds.copy);
        ReadString(*kb, "cut", next.keybinds.cut);
        ReadString(*kb, "paste", next.keybinds.paste);
        ReadString(*kb, "new_folder", next.keybinds.newFolder);
        ReadString(*kb, "rename", next.keybinds.rename);
        ReadString(*kb, "delete", next.keybinds.deleteItem);
        ReadString(*kb, "delete_permanent", next.keybinds.deletePermanent);
        ReadString(*kb, "properties", next.keybinds.properties);
        ReadString(*kb, "select_all", next.keybinds.selectAll);
    }

    // 9. Per-Folder Rules
    if (sol::optional<sol::table> rulesTbl = root["rules"]; rulesTbl.has_value()) {
        for (size_t i = 1; i <= rulesTbl->size(); ++i) {
            sol::object item = (*rulesTbl)[i];
            if (item.get_type() == sol::type::table) {
                sol::table t = item.as<sol::table>();
                FolderRule rule;
                ReadString(t, "match", rule.match);
                ReadString(t, "sort_by", rule.sortBy);
                ReadString(t, "order", rule.order);
                ReadString(t, "view", rule.view);
                if (!rule.match.empty()) {
                    next.rules.push_back(std::move(rule));
                }
            }
        }
    }

    outConfig = std::move(next);
    outError.clear();
    return true;
}

} // namespace vela