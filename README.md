<div align="center">
  <img src="assets/vela.png" alt="Vela Logo" width="128" />
  <h1>Vela</h1>
  <p>
    A fast, GPU-accelerated Windows 11 file explorer written in C++20,
    declaratively configured via a single <code>config/vela.lua</code> file.
  </p>
  <p>
    Inspired by NixOS (<code>configuration.nix</code>) and Thunar.
  </p>
  <p>
    <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-blue.svg" alt="MIT License" /></a>
    <img src="https://img.shields.io/badge/Platform-Windows%2010%20%2F%2011-0078D4.svg" alt="Windows 10 / 11" />
    <img src="https://img.shields.io/badge/Standard-C%2B%2B20-00599C.svg" alt="C++20" />
  </p>
</div>

<!-- Uncomment after adding a UI screenshot to assets/preview.png:
<p align="center">
  <img src="assets/preview.png" alt="Vela Preview" width="900" />
</p>
-->

---

## Features

### Rendering & Performance
- **GPU-Accelerated UI:** Built with Direct3D 11, DXGI Flip-Sequential SwapChain, DirectComposition, Dear ImGui, and FreeType font rendering (with automatic Windows symbol icon and CJK glyph fallback).
- **Zero-Idle CPU Loop:** Sleeps via Win32 `WaitMessage()` when idle (~0% CPU usage) and wakes instantly on input, directory changes, background tasks, or config edits.
- **Native Windows 11 Chrome:** Borderless DWM integration (`WM_NCCALCSIZE`, `HTMAXBUTTON` Windows 11 Snap Layouts) supporting `mica_alt`, `mica`, `acrylic`, and `solid` backdrops.
- **Async Directory Scanning & VRAM Thumbnail Cache:** Multi-threaded directory scanner with virtualized Grid, List, and sortable Details views plus an LRU GPU texture cache for shell icons and thumbnails.

### Declarative NixOS-Style Configuration (`config/vela.lua`)
- **Live Hot-Reload:** Edit and save `config/vela.lua` to apply changes immediately without restarting (with safe fallback toast notifications if a Lua syntax error occurs).
- **Full Visual Control:** Configure window backdrop, Dark/Light mode presets, colors, transparency, rounding, fonts, sidebar position (`left` / `right`), view row heights, Details columns, keybindings, and per-folder rules.

### File Management & Power-User Tools
- **Multi-Tab Navigation:** Tabs, Back/Forward/Up history, mouse thumb button support, clickable breadcrumbs, editable address bar (`Ctrl+L`, supports environment variables and `home`), and instant in-memory search (`Ctrl+F`).
- **Home Dashboard & System Places:** Quick Access (Pinned), Drives with live storage usage bars, Cloud Storage, Network Places, and WSL distributions.
- **Thunar-Style Drag & Drop (Win32 OLE):** Drag files between Vela, Windows Explorer, tabs, breadcrumbs, sidebar items, or folders. Configurable action prompt (`ask_always`, `ask_cross_drive`, `os_default`) supporting Copy, Move, Symbolic Link, Directory Junction, and Hard Link.
- **File Operations & Shell Clipboard:** Cut/Copy/Paste (`Ctrl+X`/`Ctrl+C`/`Ctrl+V`) via Windows Shell clipboard with dimmed Cut items, New Folder/Text File, Rename (`F2`), Delete (Recycle Bin / Permanent), conflict resolution modal, and live progress overlay.
- **Properties & SHA-256:** Built-in Properties dialog (`Alt+Enter`) with async recursive folder size calculation and on-demand SHA-256 hashing.
- **Terminal & Multi-Format Path Copy:** Open current or selected folder in Windows Terminal / PowerShell / cmd, and copy paths as Windows (`\`), POSIX (`/`), Quoted (`"..."`), or filename only.

---

## Installation

### Option 1: Portable Release (Recommended)
1. Download `Vela-portable-v0.2.0-win64.zip` from the [**Releases**](https://github.com/DeadProxy-Terin/vela/releases) page.
2. Extract the archive to any user-writable folder (for example `D:\Apps\Vela` or `%LOCALAPPDATA%\Vela`):
   ```text
   Vela/
   ├── vela.exe
   └── config/
       └── vela.lua
   ```
3. Run `vela.exe`.
   *(If Windows SmartScreen shows a warning for an unsigned binary, click **More info → Run anyway**.)*

---

### Option 2: Build & Install from Source (Non-Portable)

> **Note:** Vela currently does not include a dedicated `.exe`/`.msi` installer yet. When installing manually from source, place `vela.exe` and `config/vela.lua` in a **user-writable directory** (such as `%LOCALAPPDATA%\Vela` or `D:\Apps\Vela`) rather than `C:\Program Files`, so you can edit `config/vela.lua` for live hot-reload without Administrator privileges.

#### Prerequisites
- **Windows 10 / 11** (64-bit)
- **Git** & **CMake 3.22+** (dependencies `freetype`, `imgui`, `lua`, and `sol2` are fetched automatically during configure)
- **Compiler:** MinGW-w64 (GCC with C++20) + Ninja **or** Visual Studio 2022 (MSVC)

#### Build with MinGW-w64 + Ninja (Standalone Static Runtime)
```bat
git clone https://github.com/DeadProxy-Terin/vela.git
cd vela
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```
The compiled binary and config will be generated at `build\vela.exe` and `build\config\vela.lua`.

#### Build with Visual Studio 2022 (MSVC)
```powershell
git clone https://github.com/DeadProxy-Terin/vela.git
cd vela
cmake -B build
cmake --build build --config Release
```
Output will be in `build\Release\vela.exe` and `build\Release\config\vela.lua`.

#### Optional: Install to User Folder & Add to `PATH`
Copy `vela.exe` and the `config\` folder to `%LOCALAPPDATA%\Vela` (or any preferred folder), then add that folder to your Windows **User `PATH`** environment variable. You can then launch Vela from any terminal:
```bat
vela .
vela D:\Projects
```
You can also right-click `vela.exe` → **Pin to taskbar** / **Pin to Start** or create a Desktop shortcut.

---

## Usage

### Launching & Navigation
- **Startup Path:** Opening `vela.exe` directly opens Documents + a Home tab; passing a directory argument (`vela.exe C:\Projects`) opens that directory directly.
- **Address Bar:** Click any breadcrumb segment to jump up, click the empty area of the pill (or press `Ctrl+L`) to type a path manually (supports `%USERPROFILE%`, `%LOCALAPPDATA%`, or `home`).
- **Search Filter:** Press `Ctrl+F` and type to filter the current folder instantaneously in memory (`Esc` clears the filter).
- **Selection & Context Menus:** Supports single click, `Ctrl+Click`, `Shift+Click`, double-click to open, right-click on items, and right-click on empty viewport space.

### Default Keyboard Shortcuts
*(All file-operation and navigation shortcuts below can be customized in `config/vela.lua`.)*

| Shortcut | Action |
| :--- | :--- |
| `Ctrl+Shift+R` | Reload `config/vela.lua` |
| `F5` | Refresh current directory |
| `Ctrl+T` / `Ctrl+W` | New tab / Close tab |
| `Ctrl+L` / `Ctrl+F` | Focus address bar / Focus search |
| `Alt+Left` / `Alt+Right` / `Alt+Up` | Back / Forward / Up (also Mouse 4/5 & `Backspace`) |
| `Ctrl+H` / `Ctrl+B` | Toggle hidden items / Toggle sidebar |
| `Ctrl+Shift+T` | Open current or selected directory in Terminal |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / Cut / Paste files |
| `Ctrl+Shift+N` | Create new folder |
| `F2` | Rename selected item |
| `Delete` / `Shift+Delete` | Move to Recycle Bin / Delete permanently |
| `Ctrl+A` / `Esc` | Select all / Clear filter or selection |
| `Alt+Enter` | Open Properties dialog (with SHA-256 tool) |
| `Ctrl+Shift+C` | Copy selected path (Windows `\`) |
| `Ctrl+Alt+C` / `Ctrl+Shift+Q` | Copy POSIX path (`/`) / Quoted path (`"..."`) |

---

## Customization (`config/vela.lua`)

Click the **Settings (⚙️)** button in the top-right corner of Vela (or open `config/vela.lua` next to `vela.exe` in any editor) and save (`Ctrl+S`) to preview changes live.

### 1. Window & Backdrop (`window`)
```lua
window = {
    width = 1280,
    height = 820,
    min_width = 760,
    min_height = 500,
    backdrop = "mica_alt", -- "mica_alt" | "mica" | "acrylic" | "solid"
    dark_mode = true,
    titlebar_height = 42,
}
```
- **`backdrop`:** Switch live between Windows 11 Tabbed Mica (`mica_alt`), Mica (`mica`), Acrylic (`acrylic`), or Solid (`solid`).
- **`dark_mode`:** Sets native DWM titlebar dark/light mode. When set to `false` and color keys inside `theme` are omitted/commented out, Vela automatically applies a built-in **Windows 11 Light Mode** base palette.

### 2. Theme, Fonts & Colors (`theme`)
```lua
theme = {
    font_path = "",       -- Empty = auto-detect Segoe UI Variable / Segoe UI
    font_size = 14.5,     -- Live hot-reloaded (10.0 - 32.0)

    chrome_bg = "#00000000",          -- Transparent lets Mica/Acrylic shine through
    sidebar_bg = "#00000000",
    toolbar_bg = "#FFFFFF06",
    statusbar_bg = "#00000000",
    content_card_bg = "#202020D8",
    content_card_border = "#FFFFFF12",
    popup_bg = "#29292BF7",
    accent = "#60CDFF",

    card_rounding = 8.0,
    item_rounding = 6.0,
    pill_rounding = 6.0,
    cut_item_alpha = 0.45,
}
```
- **Custom Fonts:** Point `font_path` to any `.ttf`/`.otf`/`.ttc` file (e.g., `"C:\\Windows\\Fonts\\CascadiaMono.ttf"`). Symbol icons and CJK glyphs are merged automatically.
- **Colors:** Accepts `#RRGGBB` or `#RRGGBBAA` hex strings. All major surfaces, buttons, popups, scrollbars, progress bars, toast notifications, and danger states are customizable.

### 3. Layout, Views & Details Columns (`layout`)
```lua
layout = {
    show_toolbar = true,
    show_sidebar = true,
    show_statusbar = true,
    sidebar_position = "left",      -- "left" | "right"
    sidebar_width = 235,
    navbar_height = 44,
    toolbar_height = 42,
    statusbar_height = 30,

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
    thumbnail_cache_max = 512,
}
```

### 4. Sidebar, Terminal, Drag & Drop, Keybinds & Rules
- **`sidebar`:** Toggle `show_home`, `show_pinned`, `show_drives`, `show_cloud_storage`, `show_network`, `show_wsl`, `show_tags`, and customize `pinned` items (`"known:Desktop"`, `"shell:RecycleBinFolder"`, `"%USERPROFILE%\\Projects"`, or absolute paths).
- **`terminal`:** Set `program` and `args` (defaults to `wt.exe -d`, automatically falls back to `pwsh.exe`, `powershell.exe`, or `cmd.exe` if unavailable).
- **`drag_drop`:** Choose `"ask_always"`, `"ask_cross_drive"`, or `"os_default"`, and toggle `enable_symlink`, `enable_junction`, and `enable_hardlink`.
- **`keybinds`:** Rebind any navigation or file operation shortcut (set to `""` to disable a binding).
- **`rules`:** Automatically switch view/sort order for matching folder paths:
  ```lua
  rules = {
      { match = "\\Downloads", sort_by = "date_modified", order = "desc", view = "details" },
      { match = "\\Pictures",  sort_by = "name",          order = "asc",  view = "grid" },
  }
  ```

---

## Roadmap / In Progress

The following features are currently **in progress** or **planned** for upcoming phases:
- **Preview Pane (Phase 4):** Built-in side preview panel for images, text/code files, and quick metadata (toolbar toggle currently disabled as a placeholder).
- **Custom Icon Packs & Extension Overrides:** Full implementation of `icons.provider = "custom"`, `custom_pack`, and per-extension overrides (`.lua`, etc.) defined in `config/vela.lua` (currently uses native Windows Shell icons).
- **File Tagging System:** Persistent file/folder tagging database and filtering when selecting items under `sidebar.tags` (currently renders visual tag entries only).
- **Dedicated Windows Installer & AppData Config Fallback:** `%APPDATA%\Vela\vela.lua` config path resolution so installed copies in `Program Files` remain user-configurable, plus optional Explorer context menu integration.
- **Cross-Platform / Linux Support:** Abstracting the platform, windowing, and filesystem backends (currently Windows 10/11-only using Win32, Direct3D 11, DirectComposition, OLE, and Shell APIs) to support Linux in the future.

---

## License

Licensed under the [MIT License](LICENSE). Copyright © 2026 Proxy (`DeadProxy-Terin`).