-- =============================================================================
-- Vela Configuration (Declarative Single Source of Truth)
-- Inspired by NixOS configuration.nix.
-- Save this file at any time to live-reload Vela's UI, theme, and behavior.
-- =============================================================================

return {
    window = {
        width = 1280,
        height = 820,
        min_width = 760,
        min_height = 500,
        -- Options: "mica_alt" (Windows 11 Tabbed Mica), "mica", "acrylic", "solid"
        backdrop = "mica_alt",
        -- Tip: Set dark_mode = false and comment out color keys in `theme`
        -- below to use the built-in Windows 11 Light Mode preset automatically.
        dark_mode = true,
        titlebar_height = 42,
    },

    theme = {
        -- Leave font_path empty to auto-detect Segoe UI Variable / Segoe UI
        font_path = "",
        font_size = 14.5,

        -- Surface & Chrome Colors (Hex #RRGGBB or #RRGGBBAA)
        -- Transparent/translucent values let Windows 11 Mica Alt shine through
        chrome_bg = "#00000000",
        sidebar_bg = "#00000000",
        tab_active_bg = "#2C2C2CE8",
        tab_hover_bg = "#FFFFFF10",
        toolbar_bg = "#FFFFFF06",
        statusbar_bg = "#00000000",
        content_card_bg = "#202020D8",
        content_card_border = "#FFFFFF12",

        -- Popups, Modals, Controls & Selection
        popup_bg = "#29292BF7",
        popup_border = "#FFFFFF1F",
        input_bg = "#FFFFFF0D",
        input_border = "#FFFFFF14",
        button_bg = "#FFFFFF14",
        button_hover_bg = "#FFFFFF21",
        button_active_bg = "#FFFFFF2E",
        hover_bg = "#FFFFFF10",
        selection_bg = "#FFFFFF16",
        separator = "#FFFFFF1A",
        accent = "#60CDFF",

        -- Scrollbars, Progress & Semantic Colors
        scrollbar_bg = "#00000000",
        scrollbar_grab = "#FFFFFF38",
        scrollbar_grab_hover = "#FFFFFF59",
        progress_track_bg = "#FFFFFF1C",
        toast_bg = "#242428EB",
        toast_error_bg = "#8C2020EB",
        danger = "#AF2A2A",
        danger_text = "#FF7878",
        close_hover_bg = "#C42B1C",

        -- Typography & Item States
        text_primary = "#FFFFFFF2",
        text_secondary = "#FFFFFFA6",
        text_muted = "#FFFFFF6E",
        cut_item_alpha = 0.45,

        -- Geometry
        card_rounding = 8.0,
        item_rounding = 6.0,
        pill_rounding = 6.0,
    },

    layout = {
        show_toolbar = true,
        show_sidebar = true,
        show_statusbar = true,
        sidebar_position = "left",      -- "left" | "right"
        sidebar_width = 235,
        navbar_height = 44,
        toolbar_height = 42,
        statusbar_height = 30,

        -- View & Enumeration Defaults
        default_view = "grid",          -- "grid" | "list" | "details"
        default_sort_by = "name",       -- "name" | "date_modified" | "type" | "size"
        default_sort_order = "asc",     -- "asc" | "desc"
        folders_first = true,
        show_hidden = false,
        show_extensions = true,

        grid_tile_width = 132,
        grid_icon_size = 64,
        grid_gap = 10,
        list_row_height = 30,
        list_icon_size = 18,
        details_row_height = 26,
        details_icon_size = 16,
        details_show_date = true,
        details_show_type = true,
        details_show_size = true,
        thumbnail_cache_max = 512,      -- Max GPU textures kept in LRU VRAM cache
    },

    -- Declarative Sidebar Sections (matches Windows 11 Files reference layout)
    sidebar = {
        show_home = true,
        show_pinned = true,
        show_drives = true,
        show_cloud_storage = true,
        show_network = true,
        show_wsl = true,
        show_tags = true,

        -- Supports "known:*" (Desktop/Downloads/Documents/Music/Pictures/Videos),
        -- "shell:RecycleBinFolder", "%ENV_VAR%\\Subdir", or absolute paths like "D:\\Projects"
        pinned = {
            { name = "Desktop",     path = "known:Desktop",           icon = "desktop" },
            { name = "Downloads",   path = "known:Downloads",         icon = "downloads" },
            { name = "Documents",   path = "known:Documents",         icon = "documents" },
            { name = "Music",       path = "known:Music",             icon = "music" },
            { name = "Recycle Bin", path = "shell:RecycleBinFolder",  icon = "trash" },
        },

        tags = {
            { name = "Home", color = "#60CDFF" },
        },
    },

    -- Integrated "Open in Terminal" target
    terminal = {
        program = "wt.exe",
        args = "-d", -- Current directory path is passed automatically
    },

    -- Icon Strategy (Default: Native Windows Shell; extensible via overrides later)
    icons = {
        provider = "windows_shell", -- "windows_shell" | "custom"
        custom_pack = nil,
        overrides = {
            -- Future custom icon pack hook:
            -- [".lua"] = "icons/lua.svg",
        },
    },

    -- Thunar-style Drag & Drop Preferences
    drag_drop = {
        -- "ask_always" | "ask_cross_drive" | "os_default"
        mode = "ask_always",
        enable_symlink = true,
        enable_junction = true,
        enable_hardlink = true,
    },

    -- Declarative Keybindings
    keybinds = {
        reload_config = "Ctrl+Shift+R",
        refresh = "F5",
        new_tab = "Ctrl+T",
        close_tab = "Ctrl+W",
        focus_address = "Ctrl+L",
        focus_search = "Ctrl+F",
        copy_path = "Ctrl+Shift+C",
        copy_path_posix = "Ctrl+Alt+C",
        copy_path_quoted = "Ctrl+Shift+Q",
        toggle_hidden = "Ctrl+H",
        toggle_sidebar = "Ctrl+B",
        open_terminal = "Ctrl+Shift+T",
        go_up = "Alt+Up",
        go_back = "Alt+Left",
        go_forward = "Alt+Right",

        -- File operations & selection
        copy = "Ctrl+C",
        cut = "Ctrl+X",
        paste = "Ctrl+V",
        new_folder = "Ctrl+Shift+N",
        rename = "F2",
        delete = "Delete",
        delete_permanent = "Shift+Delete",
        properties = "Alt+Enter",
        select_all = "Ctrl+A",
    },

    -- Per-folder declarative rules (case-insensitive substring match against full path)
    rules = {
        { match = "\\Downloads", sort_by = "date_modified", order = "desc", view = "details" },
        { match = "\\Pictures",  sort_by = "name",          order = "asc",  view = "grid" },
    },
}