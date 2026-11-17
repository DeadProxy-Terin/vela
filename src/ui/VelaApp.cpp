#include "ui/VelaApp.hpp"
#include "ui/FluentIcons.hpp"
#include "core/StringUtil.hpp"
#include "fs/FileScanner.hpp"
#include "platform/OleDragDrop.hpp"
#include "platform/ThumbnailCache.hpp"

#include <imgui.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>

namespace vela {
namespace {

ImVec4 ToImVec4(const ColorRGBA& c) {
    return ImVec4(c.r, c.g, c.b, c.a);
}

uint32_t MakeCutTint(float alpha) {
    const auto a = static_cast<uint32_t>(std::clamp(alpha, 0.1f, 1.0f) * 255.0f + 0.5f) & 0xFF;
    return IM_COL32(255, 255, 255, a);
}

size_t NextUtf8(std::string_view s, size_t i) {
    if (i >= s.size()) return s.size();
    ++i;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i;
}

size_t PrevUtf8(std::string_view s, size_t i) {
    if (i == 0) return 0;
    --i;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

void SplitTwoLinesEllipsis(std::string_view text, float maxWidth,
                           std::string& outLine1, std::string& outLine2, bool& outTruncated) {
    outLine1.clear();
    outLine2.clear();
    outTruncated = false;
    if (text.empty()) return;

    auto widthOf = [](std::string_view sv) {
        if (sv.empty()) return 0.0f;
        return ImGui::CalcTextSize(sv.data(), sv.data() + sv.size()).x;
    };

    if (widthOf(text) <= maxWidth) {
        outLine1 = std::string(text);
        return;
    }

    // Find max UTF-8 prefix that fits on line 1
    size_t fit1 = 0;
    size_t lastSpace = std::string_view::npos;
    for (size_t pos = 0; pos < text.size(); ) {
        const size_t next = NextUtf8(text, pos);
        if (widthOf(text.substr(0, next)) > maxWidth) break;
        if (text[pos] == ' ') lastSpace = pos;
        fit1 = next;
        pos = next;
    }
    if (fit1 == 0) fit1 = NextUtf8(text, 0);

    size_t splitAt = fit1;
    size_t restStart = fit1;
    if (lastSpace != std::string_view::npos && lastSpace > fit1 / 2) {
        splitAt = lastSpace;
        restStart = lastSpace + 1;
    }

    outLine1 = std::string(text.substr(0, splitAt));
    std::string_view rest = (restStart < text.size()) ? text.substr(restStart) : std::string_view{};
    if (rest.empty()) return;

    if (widthOf(rest) <= maxWidth) {
        outLine2 = std::string(rest);
        return;
    }

    outTruncated = true;
    const float ellipsisW = widthOf("...");
    const float targetW = (std::max)(8.0f, maxWidth - ellipsisW);

    size_t fit2 = 0;
    for (size_t pos = 0; pos < rest.size(); ) {
        const size_t next = NextUtf8(rest, pos);
        if (widthOf(rest.substr(0, next)) > targetW) break;
        fit2 = next;
        pos = next;
    }
    while (fit2 > 0 && rest[fit2 - 1] == ' ') {
        fit2 = PrevUtf8(rest, fit2);
    }
    outLine2 = std::string(rest.substr(0, fit2)) + "...";
}

RECT ToRect(const ImVec2& minPt, const ImVec2& maxPt) {
    return RECT{
        static_cast<LONG>(std::floor(minPt.x)),
        static_cast<LONG>(std::floor(minPt.y)),
        static_cast<LONG>(std::ceil(maxPt.x)),
        static_cast<LONG>(std::ceil(maxPt.y))
    };
}

ViewMode ParseViewMode(std::string_view v) {
    const std::string s = str::ToLowerAscii(v);
    if (s == "list") return ViewMode::List;
    if (s == "details") return ViewMode::Details;
    return ViewMode::Grid;
}

SortField ParseSortField(std::string_view v) {
    const std::string s = str::ToLowerAscii(v);
    if (s == "date_modified" || s == "date") return SortField::DateModified;
    if (s == "type") return SortField::Type;
    if (s == "size") return SortField::Size;
    return SortField::Name;
}

SortOrder ParseSortOrder(std::string_view v) {
    const std::string s = str::ToLowerAscii(v);
    if (s == "desc" || s == "descending") return SortOrder::Descending;
    return SortOrder::Ascending;
}

std::string MakeFolderTabTitle(const std::wstring& pathW, const std::vector<DrivePlace>& drives) {
    if (pathW.empty()) return "Home";
    const std::string utf8 = str::WideToUtf8(pathW);

    for (const auto& d : drives) {
        if (_wcsicmp(d.rootPathW.c_str(), pathW.c_str()) == 0) {
            return d.displayName;
        }
    }

    std::string trimmed = utf8;
    while (trimmed.size() > 3 && trimmed.back() == '\\') {
        trimmed.pop_back();
    }
    const size_t slash = trimmed.find_last_of('\\');
    if (slash != std::string::npos && slash + 1 < trimmed.size()) {
        return trimmed.substr(slash + 1);
    }
    return trimmed;
}

struct BreadcrumbSegment {
    std::string label;
    std::wstring pathW;
};

std::vector<BreadcrumbSegment> SplitBreadcrumbs(const std::wstring& pathW) {
    std::vector<BreadcrumbSegment> segs;
    if (pathW.empty()) return segs;

    const std::wstring norm = str::NormalizeWindowsPath(pathW);

    // Handle UNC / WSL prefix: \\server\share\sub...
    if (norm.size() >= 2 && norm[0] == L'\\' && norm[1] == L'\\') {
        const size_t srvEnd = norm.find(L'\\', 2);
        if (srvEnd == std::wstring::npos) {
            segs.push_back({str::WideToUtf8(norm), norm});
            return segs;
        }
        const size_t shareEnd = norm.find(L'\\', srvEnd + 1);
        if (shareEnd == std::wstring::npos) {
            segs.push_back({str::WideToUtf8(norm), norm});
            return segs;
        }
        const std::wstring rootW = norm.substr(0, shareEnd);
        segs.push_back({str::WideToUtf8(rootW), rootW});

        size_t pos = shareEnd + 1;
        while (pos < norm.size()) {
            const size_t next = norm.find(L'\\', pos);
            const size_t end = (next == std::wstring::npos) ? norm.size() : next;
            if (end > pos) {
                const std::wstring subPath = norm.substr(0, end);
                segs.push_back({str::WideToUtf8(norm.substr(pos, end - pos)), subPath});
            }
            if (next == std::wstring::npos) break;
            pos = next + 1;
        }
        return segs;
    }

    // Standard drive path: C:\Users\...
    if (norm.size() >= 2 && norm[1] == L':') {
        const std::wstring driveRoot = norm.substr(0, 2) + L"\\";
        segs.push_back({str::WideToUtf8(norm.substr(0, 2)), driveRoot});
        size_t pos = (norm.size() > 3 && norm[2] == L'\\') ? 3 : norm.size();
        while (pos < norm.size()) {
            const size_t next = norm.find(L'\\', pos);
            const size_t end = (next == std::wstring::npos) ? norm.size() : next;
            if (end > pos) {
                const std::wstring subPath = norm.substr(0, end);
                segs.push_back({str::WideToUtf8(norm.substr(pos, end - pos)), subPath});
            }
            if (next == std::wstring::npos) break;
            pos = next + 1;
        }
        return segs;
    }

    segs.push_back({str::WideToUtf8(norm), norm});
    return segs;
}

ImGuiKey ParseKeyToken(std::string_view token) {
    const std::string t = str::ToLowerAscii(token);
    if (t.size() == 1) {
        const char c = t[0];
        if (c >= 'a' && c <= 'z') {
            return static_cast<ImGuiKey>(ImGuiKey_A + (c - 'a'));
        }
        if (c >= '0' && c <= '9') {
            return static_cast<ImGuiKey>(ImGuiKey_0 + (c - '0'));
        }
    }
    if (t == "f1") return ImGuiKey_F1;
    if (t == "f2") return ImGuiKey_F2;
    if (t == "f3") return ImGuiKey_F3;
    if (t == "f4") return ImGuiKey_F4;
    if (t == "f5") return ImGuiKey_F5;
    if (t == "f6") return ImGuiKey_F6;
    if (t == "f7") return ImGuiKey_F7;
    if (t == "f8") return ImGuiKey_F8;
    if (t == "f9") return ImGuiKey_F9;
    if (t == "f10") return ImGuiKey_F10;
    if (t == "f11") return ImGuiKey_F11;
    if (t == "f12") return ImGuiKey_F12;
    if (t == "up") return ImGuiKey_UpArrow;
    if (t == "down") return ImGuiKey_DownArrow;
    if (t == "left") return ImGuiKey_LeftArrow;
    if (t == "right") return ImGuiKey_RightArrow;
    if (t == "enter" || t == "return") return ImGuiKey_Enter;
    if (t == "backspace") return ImGuiKey_Backspace;
    if (t == "delete" || t == "del") return ImGuiKey_Delete;
    if (t == "escape" || t == "esc") return ImGuiKey_Escape;
    if (t == "tab") return ImGuiKey_Tab;
    if (t == "space") return ImGuiKey_Space;
    return ImGuiKey_None;
}

bool MatchKeybind(const std::string& spec) {
    if (spec.empty()) return false;

    bool wantCtrl = false;
    bool wantShift = false;
    bool wantAlt = false;
    ImGuiKey mainKey = ImGuiKey_None;

    size_t start = 0;
    while (start <= spec.size()) {
        const size_t plus = spec.find('+', start);
        std::string_view part = (plus == std::string::npos)
            ? std::string_view(spec).substr(start)
            : std::string_view(spec).substr(start, plus - start);

        while (!part.empty() && part.front() == ' ') part.remove_prefix(1);
        while (!part.empty() && part.back() == ' ') part.remove_suffix(1);

        const std::string lower = str::ToLowerAscii(part);
        if (lower == "ctrl" || lower == "control") {
            wantCtrl = true;
        } else if (lower == "shift") {
            wantShift = true;
        } else if (lower == "alt") {
            wantAlt = true;
        } else if (!lower.empty()) {
            mainKey = ParseKeyToken(lower);
        }

        if (plus == std::string::npos) break;
        start = plus + 1;
    }

    if (mainKey == ImGuiKey_None) return false;
    const ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput && !wantCtrl && !wantAlt && mainKey < ImGuiKey_F1) {
        return false;
    }

    return io.KeyCtrl == wantCtrl &&
           io.KeyShift == wantShift &&
           io.KeyAlt == wantAlt &&
           ImGui::IsKeyPressed(mainKey, false);
}

void DrawAspectImage(ImDrawList* dl, ID3D11ShaderResourceView* srv, int texW, int texH,
                     const ImVec2& boxMin, const ImVec2& boxMax, uint32_t tintCol = IM_COL32_WHITE) {
    if (!srv || texW <= 0 || texH <= 0) return;

    const float boxW = boxMax.x - boxMin.x;
    const float boxH = boxMax.y - boxMin.y;
    const float scale = (std::min)(boxW / static_cast<float>(texW), boxH / static_cast<float>(texH));
    const float drawW = static_cast<float>(texW) * scale;
    const float drawH = static_cast<float>(texH) * scale;

    const ImVec2 imgMin(boxMin.x + (boxW - drawW) * 0.5f, boxMin.y + (boxH - drawH) * 0.5f);
    const ImVec2 imgMax(imgMin.x + drawW, imgMin.y + drawH);
    dl->AddImage(reinterpret_cast<ImTextureID>(srv), imgMin, imgMax, ImVec2(0, 0), ImVec2(1, 1), tintCol);
}

bool IsSamePathW(const std::wstring& aW, const std::wstring& bW) {
    const std::wstring na = str::NormalizeWindowsPath(aW);
    const std::wstring nb = str::NormalizeWindowsPath(bW);
    return _wcsicmp(na.c_str(), nb.c_str()) == 0;
}

bool IsSubpathOrEqualW(const std::wstring& parentW, const std::wstring& candidateW) {
    std::wstring p = str::NormalizeWindowsPath(parentW);
    std::wstring c = str::NormalizeWindowsPath(candidateW);
    if (p.empty() || c.empty()) return false;
    if (_wcsicmp(p.c_str(), c.c_str()) == 0) return true;
    if (p.back() != L'\\') p.push_back(L'\\');
    if (c.size() <= p.size()) return false;
    return _wcsnicmp(c.c_str(), p.c_str(), p.size()) == 0;
}

bool HasSameDriveW(const std::wstring& aW, const std::wstring& bW) {
    if (aW.size() < 2 || bW.size() < 2) return false;
    if (aW[1] != L':' || bW[1] != L':') return false;
    return std::towupper(aW[0]) == std::towupper(bW[0]);
}

bool AreAllDirectChildrenW(const std::vector<std::wstring>& sourcesW, const std::wstring& dirW) {
    if (sourcesW.empty() || dirW.empty()) return false;
    for (const auto& srcW : sourcesW) {
        const std::wstring parentW = str::GetParentWindowsPath(srcW);
        if (parentW.empty() || !IsSamePathW(parentW, dirW)) {
            return false;
        }
    }
    return true;
}

} // namespace

void VelaApp::Initialize(
    Win32Window* window,
    FileScanner* scanner,
    ThumbnailCache* thumbCache,
    FileOps* fileOps,
    std::filesystem::path configPath,
    std::function<void(const std::wstring&)> onWatchDirectory,
    std::function<void()> onReloadConfig
) {
    m_window = window;
    m_scanner = scanner;
    m_thumbCache = thumbCache;
    m_fileOps = fileOps;
    m_configPath = std::move(configPath);
    m_onWatchDirectory = std::move(onWatchDirectory);
    m_onReloadConfig = std::move(onReloadConfig);

    m_dialogs.Initialize(m_window, m_fileOps, [this](bool isErr, const std::string& msg) {
        ShowToast(isErr, msg);
    });
}

void VelaApp::Shutdown() {
    m_dialogs.Shutdown();
}

void VelaApp::SetConfig(const Config& cfg) {
    m_cfg = cfg;
    RefreshSystemPlaces();

    if (m_thumbCache) {
        m_thumbCache->SetMaxEntries(static_cast<size_t>(m_cfg.layout.thumbnailCacheMax));
    }

    for (auto& tab : m_tabs) {
        tab.viewMode = ParseViewMode(m_cfg.layout.defaultView);
        tab.sortField = ParseSortField(m_cfg.layout.defaultSortBy);
        tab.sortOrder = ParseSortOrder(m_cfg.layout.defaultSortOrder);
        tab.showHidden = m_cfg.layout.showHidden;
        if (!tab.isHome) {
            ApplyFolderRules(tab);
            SortAndFilterTab(tab);
        }
    }
}

void VelaApp::RefreshSystemPlaces() {
    m_places = SystemPlaces::Refresh(m_cfg.sidebar);
}

void VelaApp::OpenInitialDirectory(const std::wstring& pathW) {
    m_tabs.clear();

    std::wstring startPathW = pathW;
    if (startPathW.empty()) {
        startPathW = SystemPlaces::ResolveKnownFolder("known:Documents");
    }
    if (startPathW.empty()) {
        startPathW = m_places.defaultHomePathW;
    }

    if (!startPathW.empty()) {
        OpenNewTab(NavTarget{false, str::NormalizeWindowsPath(startPathW)}, true);
        OpenNewTab(NavTarget{true, L""}, false);
    } else {
        OpenNewTab(NavTarget{true, L""}, true);
    }
}

void VelaApp::OnScanCompleted() {
    if (!m_scanner) return;
    DirectoryListing listing;
    if (!m_scanner->TryPopResult(listing)) return;

    for (size_t i = 0; i < m_tabs.size(); ++i) {
        TabState& tab = m_tabs[i];
        if (!tab.isHome && tab.expectedGeneration == listing.generation) {
            tab.isLoading = false;
            tab.errorMessage = listing.success ? "" : listing.errorMessage;
            tab.allEntries = std::move(listing.entries);
            tab.selectedAllIndices.clear();
            tab.anchorVisibleIdx = -1;
            tab.focusedVisibleIdx = -1;
            SortAndFilterTab(tab);
            if (!m_pendingSelectPathW.empty()) {
                for (size_t v = 0; v < tab.visibleIndices.size(); ++v) {
                    const size_t idx = tab.visibleIndices[v];
                    if (IsSamePathW(tab.allEntries[idx].fullPathW, m_pendingSelectPathW)) {
                        tab.selectedAllIndices.insert(idx);
                        tab.anchorVisibleIdx = static_cast<int>(v);
                        tab.focusedVisibleIdx = static_cast<int>(v);
                        break;
                    }
                }
                m_pendingSelectPathW.clear();
            }
            break;
        }
    }
}

void VelaApp::OnDirectoryChangedExternally() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome || tab.currentPathW.empty() || !m_scanner) return;

    tab.expectedGeneration = m_scanner->RequestScan(tab.currentPathW);
    if (m_thumbCache) {
        m_thumbCache->SetGeneration(tab.expectedGeneration);
    }
}

void VelaApp::OnFileOpNotify() {
    if (!m_fileOps) return;

    bool shouldRefreshActive = false;
    FileOpResult res;
    while (m_fileOps->TryPopResult(res)) {
        if (!res.summaryMessage.empty()) {
            const bool isErr = !res.success && !res.cancelled;
            ShowToast(isErr, res.summaryMessage, isErr ? 4200 : 2400);
        }
        if (!res.primaryCreatedPathW.empty() &&
            (res.kind == FileOpKind::Rename ||
             res.kind == FileOpKind::CreateFolder ||
             res.kind == FileOpKind::CreateFile ||
             res.kind == FileOpKind::CreateSymlink ||
             res.kind == FileOpKind::CreateJunction ||
             res.kind == FileOpKind::CreateHardlink)) {
            m_pendingSelectPathW = res.primaryCreatedPathW;
        }
        if (res.kind == FileOpKind::Delete && res.success) {
            m_cutPathsLower.clear();
        }

        if (m_activeTab >= 0 && m_activeTab < static_cast<int>(m_tabs.size())) {
            const TabState& tab = m_tabs[m_activeTab];
            if (!tab.isHome && !tab.currentPathW.empty()) {
                const bool matchTarget = !res.targetDirectoryW.empty() &&
                    IsSamePathW(tab.currentPathW, res.targetDirectoryW);
                const bool mayAffectCurrent = (res.kind == FileOpKind::Move || res.kind == FileOpKind::Delete);
                if (matchTarget || mayAffectCurrent) {
                    shouldRefreshActive = true;
                }
            }
        }
    }

    if (shouldRefreshActive) {
        RefreshCurrentTab();
    }
    if (m_window) {
        m_window->Wake();
    }
}

void VelaApp::ProcessPendingDragDrop() {
    if (m_pendingDragPathsW.empty()) return;
    auto pathsW = std::move(m_pendingDragPathsW);
    m_pendingDragPathsW.clear();

    OleDragDropHost::StartFileDrag(pathsW);

    m_oleDragActive = false;
    m_oleDragPathsW.clear();
    m_hoveredDropZoneIdx = -1;
    if (m_window) {
        m_window->Wake();
    }
}

void VelaApp::NotifyConfigReloaded(bool success, const std::string& message) {
    ShowToast(!success, message, success ? 2200 : 4800);
}

void VelaApp::ShowToast(bool isError, const std::string& text, UINT durationMs) {
    m_toastText = text;
    m_toastIsError = isError;
    m_toastUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    if (m_window) {
        m_window->ScheduleWake(durationMs + 80);
        m_window->Wake();
    }
}

void VelaApp::OpenNewTab(const NavTarget& target, bool switchToTab) {
    TabState tab;
    tab.viewMode = ParseViewMode(m_cfg.layout.defaultView);
    tab.sortField = ParseSortField(m_cfg.layout.defaultSortBy);
    tab.sortOrder = ParseSortOrder(m_cfg.layout.defaultSortOrder);
    tab.showHidden = m_cfg.layout.showHidden;

    m_tabs.push_back(std::move(tab));
    const int newIdx = static_cast<int>(m_tabs.size()) - 1;

    if (switchToTab) {
        m_activeTab = newIdx;
        NavigateTo(target, false);
    } else {
        TabState& created = m_tabs[newIdx];
        created.isHome = target.isHome;
        created.currentPathW = target.isHome ? L"" : str::NormalizeWindowsPath(target.pathW);
        created.currentPath = str::WideToUtf8(created.currentPathW);
        created.title = target.isHome ? "Home" : MakeFolderTabTitle(created.currentPathW, m_places.drives);
    }
}

void VelaApp::CloseTab(int index) {
    if (m_tabs.size() <= 1 || index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    m_tabs.erase(m_tabs.begin() + index);
    m_activeTab = std::clamp(m_activeTab, 0, static_cast<int>(m_tabs.size()) - 1);
    SwitchTab(m_activeTab);
}

void VelaApp::SwitchTab(int index) {
    if (index < 0 || index >= static_cast<int>(m_tabs.size())) return;
    m_activeTab = index;
    m_editingAddress = false;
    m_searchBuffer[0] = '\0';

    TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome) {
        if (m_onWatchDirectory) m_onWatchDirectory(L"");
    } else {
        RefreshCurrentTab();
    }
}

void VelaApp::NavigateTo(const NavTarget& target, bool recordHistory) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];

    const std::wstring cleanTargetW = target.isHome ? L"" : str::NormalizeWindowsPath(target.pathW);
    if (recordHistory && (!tab.isHome || !tab.currentPathW.empty() || !target.isHome)) {
        if (tab.isHome != target.isHome || _wcsicmp(tab.currentPathW.c_str(), cleanTargetW.c_str()) != 0) {
            tab.backHistory.push_back(NavTarget{tab.isHome, tab.currentPathW});
            tab.forwardHistory.clear();
        }
    }

    m_editingAddress = false;
    m_searchBuffer[0] = '\0';
    tab.selectedAllIndices.clear();
    tab.anchorVisibleIdx = -1;
    tab.focusedVisibleIdx = -1;
    tab.errorMessage.clear();
    tab.scrollToTopPending = true;

    if (target.isHome || cleanTargetW.empty()) {
        tab.isHome = true;
        tab.currentPathW.clear();
        tab.currentPath.clear();
        tab.title = "Home";
        tab.isLoading = false;
        tab.allEntries.clear();
        tab.visibleIndices.clear();
        if (m_onWatchDirectory) m_onWatchDirectory(L"");
        return;
    }

    tab.isHome = false;
    tab.currentPathW = cleanTargetW;
    tab.currentPath = str::WideToUtf8(cleanTargetW);
    tab.title = MakeFolderTabTitle(cleanTargetW, m_places.drives);

    // Reset to global defaults first, then apply declarative per-folder overrides
    tab.viewMode = ParseViewMode(m_cfg.layout.defaultView);
    tab.sortField = ParseSortField(m_cfg.layout.defaultSortBy);
    tab.sortOrder = ParseSortOrder(m_cfg.layout.defaultSortOrder);
    tab.showHidden = m_cfg.layout.showHidden;
    ApplyFolderRules(tab);

    RefreshCurrentTab();
}

void VelaApp::NavigateUp() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome || tab.currentPathW.empty()) return;

    const std::wstring parentW = str::GetParentWindowsPath(tab.currentPathW);
    if (parentW.empty()) {
        NavigateTo(NavTarget{true, L""});
    } else {
        NavigateTo(NavTarget{false, parentW});
    }
}

void VelaApp::NavigateBack() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];
    if (tab.backHistory.empty()) return;

    tab.forwardHistory.push_back(NavTarget{tab.isHome, tab.currentPathW});
    const NavTarget prev = tab.backHistory.back();
    tab.backHistory.pop_back();
    NavigateTo(prev, false);
}

void VelaApp::NavigateForward() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];
    if (tab.forwardHistory.empty()) return;

    tab.backHistory.push_back(NavTarget{tab.isHome, tab.currentPathW});
    const NavTarget next = tab.forwardHistory.back();
    tab.forwardHistory.pop_back();
    NavigateTo(next, false);
}

void VelaApp::RefreshCurrentTab() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];

    if (tab.isHome) {
        RefreshSystemPlaces();
        return;
    }

    if (m_scanner && !tab.currentPathW.empty()) {
        tab.isLoading = true;
        tab.expectedGeneration = m_scanner->RequestScan(tab.currentPathW);
        if (m_thumbCache) {
            m_thumbCache->SetGeneration(tab.expectedGeneration);
        }
    }
    if (m_onWatchDirectory) {
        m_onWatchDirectory(tab.currentPathW);
    }
}

void VelaApp::ApplyFolderRules(TabState& tab) {
    if (tab.isHome || tab.currentPath.empty()) return;
    const std::string pathLower = str::ToLowerAscii(tab.currentPath);

    for (const auto& rule : m_cfg.rules) {
        const std::string matchLower = str::ToLowerAscii(rule.match);
        if (!matchLower.empty() && pathLower.find(matchLower) != std::string::npos) {
            if (!rule.view.empty()) tab.viewMode = ParseViewMode(rule.view);
            if (!rule.sortBy.empty()) tab.sortField = ParseSortField(rule.sortBy);
            if (!rule.order.empty()) tab.sortOrder = ParseSortOrder(rule.order);
        }
    }
}

void VelaApp::SortAndFilterTab(TabState& tab) {
    FileScanner::SortEntries(tab.allEntries, tab.sortField, tab.sortOrder, m_cfg.layout.foldersFirst);
    RebuildVisibleIndices(tab);
}

void VelaApp::RebuildVisibleIndices(TabState& tab) {
    tab.visibleIndices.clear();
    tab.visibleIndices.reserve(tab.allEntries.size());

    const std::string filter(m_searchBuffer);
    for (size_t i = 0; i < tab.allEntries.size(); ++i) {
        const FileEntry& entry = tab.allEntries[i];
        if (!tab.showHidden && entry.isHidden) continue;
        if (!filter.empty() && !str::ContainsCaseInsensitive(entry.nameLower, filter)) continue;
        tab.visibleIndices.push_back(i);
    }
}

void VelaApp::ActivateEntry(TabState& tab, size_t allIndex) {
    if (allIndex >= tab.allEntries.size()) return;
    const FileEntry& entry = tab.allEntries[allIndex];
    if (entry.isDir) {
        NavigateTo(NavTarget{false, entry.fullPathW});
    } else if (m_window) {
        if (!m_window->OpenShellItem(entry.fullPathW)) {
            ShowToast(true, "Failed to open: " + entry.name);
        }
    }
}

void VelaApp::HandleItemClick(TabState& tab, int visibleIdx, bool ctrlDown, bool shiftDown) {
    if (visibleIdx < 0 || visibleIdx >= static_cast<int>(tab.visibleIndices.size())) return;
    const size_t allIdx = tab.visibleIndices[visibleIdx];

    if (shiftDown && tab.anchorVisibleIdx >= 0 &&
        tab.anchorVisibleIdx < static_cast<int>(tab.visibleIndices.size())) {
        if (!ctrlDown) {
            tab.selectedAllIndices.clear();
        }
        const int lo = (std::min)(tab.anchorVisibleIdx, visibleIdx);
        const int hi = (std::max)(tab.anchorVisibleIdx, visibleIdx);
        for (int i = lo; i <= hi; ++i) {
            tab.selectedAllIndices.insert(tab.visibleIndices[i]);
        }
    } else if (ctrlDown) {
        if (tab.selectedAllIndices.contains(allIdx)) {
            tab.selectedAllIndices.erase(allIdx);
        } else {
            tab.selectedAllIndices.insert(allIdx);
        }
        tab.anchorVisibleIdx = visibleIdx;
    } else {
        tab.selectedAllIndices.clear();
        tab.selectedAllIndices.insert(allIdx);
        tab.anchorVisibleIdx = visibleIdx;
    }

    tab.focusedVisibleIdx = visibleIdx;
}

void VelaApp::SelectAll(TabState& tab) {
    tab.selectedAllIndices.clear();
    for (size_t idx : tab.visibleIndices) {
        tab.selectedAllIndices.insert(idx);
    }
}

void VelaApp::ClearSelection(TabState& tab) {
    tab.selectedAllIndices.clear();
    tab.anchorVisibleIdx = -1;
}

void VelaApp::CopySelectedPaths(CopyPathFormat format) {
    if (!m_window || m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];

    auto formatOne = [&](const std::string& fullPath, const std::string& fileName) -> std::string {
        switch (format) {
            case CopyPathFormat::Windows:  return fullPath;
            case CopyPathFormat::Posix:    return str::ToPosixPath(fullPath);
            case CopyPathFormat::Quoted:   return str::QuotePath(fullPath);
            case CopyPathFormat::Filename: return fileName;
        }
        return fullPath;
    };

    std::string payload;
    size_t count = 0;

    if (!tab.isHome && !tab.selectedAllIndices.empty()) {
        for (size_t allIdx : tab.visibleIndices) {
            if (!tab.selectedAllIndices.contains(allIdx)) continue;
            const auto& entry = tab.allEntries[allIdx];
            if (!payload.empty()) payload += "\r\n";
            payload += formatOne(entry.fullPath, entry.name);
            ++count;
        }
    } else if (!tab.isHome && !tab.currentPath.empty()) {
        payload = formatOne(tab.currentPath, tab.title);
        count = 1;
    }

    if (payload.empty()) return;

    if (m_window->SetClipboardText(payload)) {
        const char* kind = "path";
        if (format == CopyPathFormat::Posix) kind = "POSIX path";
        else if (format == CopyPathFormat::Quoted) kind = "quoted path";
        else if (format == CopyPathFormat::Filename) kind = "filename";

        char msg[128];
        if (count > 1) {
            std::snprintf(msg, sizeof(msg), "Copied %zu %ss to clipboard", count, kind);
        } else {
            std::snprintf(msg, sizeof(msg), "Copied %s: %s", kind, payload.c_str());
        }
        ShowToast(false, msg);
    }
}

void VelaApp::OpenSelectedInTerminal() {
    if (!m_window || m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];

    std::wstring targetDirW = tab.currentPathW;
    if (!tab.selectedAllIndices.empty()) {
        for (size_t idx : tab.visibleIndices) {
            if (tab.selectedAllIndices.contains(idx) && tab.allEntries[idx].isDir) {
                targetDirW = tab.allEntries[idx].fullPathW;
                break;
            }
        }
    }
    if (targetDirW.empty()) {
        targetDirW = m_places.defaultHomePathW;
    }
    m_window->OpenInTerminal(targetDirW, m_cfg.terminal.program, m_cfg.terminal.args);
}

std::vector<std::wstring> VelaApp::GetSelectedPathsW(const TabState& tab) const {
    std::vector<std::wstring> out;
    if (tab.isHome || tab.selectedAllIndices.empty()) return out;
    out.reserve(tab.selectedAllIndices.size());
    for (size_t allIdx : tab.visibleIndices) {
        if (tab.selectedAllIndices.contains(allIdx) && allIdx < tab.allEntries.size()) {
            out.push_back(tab.allEntries[allIdx].fullPathW);
        }
    }
    return out;
}

void VelaApp::HandleItemMouseInteraction(
    TabState& tab, int visibleIdx, size_t allIdx, const FileEntry& entry, float dpiScale
) {
    const ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_dragCandidateActive = false;
        m_deferCollapseSelection = false;
        ActivateEntry(tab, allIdx);
        return;
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
        if (!io.KeyCtrl && !io.KeyShift && tab.selectedAllIndices.contains(allIdx) &&
            tab.selectedAllIndices.size() > 1) {
            m_deferCollapseSelection = true;
            m_deferCollapseVisibleIdx = visibleIdx;
        } else {
            m_deferCollapseSelection = false;
            HandleItemClick(tab, visibleIdx, io.KeyCtrl, io.KeyShift);
        }
        m_dragCandidateActive = true;
        m_dragStartX = io.MousePos.x;
        m_dragStartY = io.MousePos.y;
    }

    if (m_dragCandidateActive && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float dx = io.MousePos.x - m_dragStartX;
        const float dy = io.MousePos.y - m_dragStartY;
        const float thresh = 6.0f * dpiScale;
        if ((dx * dx + dy * dy) >= (thresh * thresh)) {
            m_dragCandidateActive = false;
            m_deferCollapseSelection = false;

            auto pathsW = GetSelectedPathsW(tab);
            if (pathsW.empty()) {
                pathsW.push_back(entry.fullPathW);
            }
            m_pendingDragPathsW = std::move(pathsW);
        }
    }

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (m_deferCollapseSelection && m_deferCollapseVisibleIdx == visibleIdx) {
            HandleItemClick(tab, visibleIdx, false, false);
        }
        m_dragCandidateActive = false;
        m_deferCollapseSelection = false;
    }

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        m_dragCandidateActive = false;
        m_deferCollapseSelection = false;
        if (!tab.selectedAllIndices.contains(allIdx)) {
            HandleItemClick(tab, visibleIdx, false, false);
        }
        ImGui::OpenPopup("##ItemContextMenu");
    }
}

void VelaApp::CopySelectedFilesToClipboard(bool isCut) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    auto pathsW = GetSelectedPathsW(tab);
    if (pathsW.empty()) return;

    if (!OleDragDropHost::SetClipboardFiles(pathsW, isCut)) {
        ShowToast(true, "Failed to update Windows clipboard");
        return;
    }

    m_cutPathsLower.clear();
    if (isCut) {
        for (const auto& pW : pathsW) {
            m_cutPathsLower.insert(str::ToLowerAscii(str::WideToUtf8(pW)));
        }
    }

    char msg[96];
    std::snprintf(msg, sizeof(msg), "%s %zu item%s",
                  isCut ? "Cut" : "Copied",
                  pathsW.size(),
                  pathsW.size() == 1 ? "" : "s");
    ShowToast(false, msg);
}

void VelaApp::PasteClipboardFiles(const std::wstring& overrideDestDirW) {
    if (!m_fileOps || m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];

    const std::wstring destDirW = !overrideDestDirW.empty() ? overrideDestDirW : tab.currentPathW;
    if (destDirW.empty() || (overrideDestDirW.empty() && tab.isHome)) return;

    std::vector<std::wstring> pathsW;
    bool isCut = false;
    if (!OleDragDropHost::ReadClipboardFiles(pathsW, isCut) || pathsW.empty()) {
        ShowToast(false, "Clipboard does not contain any files");
        return;
    }

    FileOpRequest req;
    req.kind = isCut ? FileOpKind::Move : FileOpKind::Copy;
    req.sources = std::move(pathsW);
    req.destDirW = destDirW;
    req.ownerHwnd = m_window ? m_window->GetHandle() : nullptr;
    m_fileOps->Enqueue(std::move(req));

    if (isCut) {
        m_cutPathsLower.clear();
    }
}

void VelaApp::PromptNewItem(bool isFolder) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome || tab.currentPathW.empty()) return;
    m_dialogs.OpenNewItem(isFolder, tab.currentPathW);
}

void VelaApp::PromptRenameSelected() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome || tab.selectedAllIndices.empty()) return;

    for (size_t idx : tab.visibleIndices) {
        if (tab.selectedAllIndices.contains(idx) && idx < tab.allEntries.size()) {
            const FileEntry& e = tab.allEntries[idx];
            m_dialogs.OpenRename(e.fullPathW, e.isDir);
            break;
        }
    }
}

void VelaApp::PromptDeleteSelected(bool permanent) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    auto pathsW = GetSelectedPathsW(tab);
    if (pathsW.empty()) return;
    m_dialogs.OpenDeleteConfirm(std::move(pathsW), permanent);
}

void VelaApp::OpenSelectedProperties() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];
    if (tab.isHome) return;

    auto targetsW = GetSelectedPathsW(tab);
    if (targetsW.empty() && !tab.currentPathW.empty()) {
        targetsW.push_back(tab.currentPathW);
    }
    if (!targetsW.empty()) {
        m_dialogs.OpenProperties(std::move(targetsW));
    }
}

void VelaApp::RegisterDropZone(
    DropZone::Kind kind, float minX, float minY, float maxX, float maxY,
    const std::wstring& targetDirW, int tabIndex
) {
    if (targetDirW.empty()) return;
    m_dropZones.push_back(DropZone{kind, minX, minY, maxX, maxY, targetDirW, tabIndex});
}

const VelaApp::DropZone* VelaApp::ResolveDropZone(POINT clientPt) const {
    const float px = static_cast<float>(clientPt.x);
    const float py = static_cast<float>(clientPt.y);

    // Priority 1: Specific targets (tabs, breadcrumbs, sidebar rows, folder items)
    for (auto it = m_dropZones.rbegin(); it != m_dropZones.rend(); ++it) {
        if (it->kind == DropZone::Kind::Viewport) continue;
        if (px >= it->minX && px < it->maxX && py >= it->minY && py < it->maxY) {
            return &(*it);
        }
    }
    // Priority 2: Active directory viewport fallback
    for (auto it = m_dropZones.rbegin(); it != m_dropZones.rend(); ++it) {
        if (it->kind != DropZone::Kind::Viewport) continue;
        if (px >= it->minX && px < it->maxX && py >= it->minY && py < it->maxY) {
            return &(*it);
        }
    }
    return nullptr;
}

bool VelaApp::CanDropSourcesInto(
    const std::vector<std::wstring>& sourcesW, const std::wstring& targetDirW
) const {
    if (sourcesW.empty() || targetDirW.empty()) return false;
    for (const auto& srcW : sourcesW) {
        if (IsSubpathOrEqualW(srcW, targetDirW)) {
            return false;
        }
    }
    if (AreAllDirectChildrenW(sourcesW, targetDirW)) {
        return false;
    }
    return true;
}

DWORD VelaApp::EvaluateDropEffect(
    POINT clientPt, DWORD keyState, DWORD allowedEffects, const DropZone*& outZone
) {
    outZone = ResolveDropZone(clientPt);
    if (!outZone || !CanDropSourcesInto(m_oleDragPathsW, outZone->targetDirW)) {
        outZone = nullptr;
        return DROPEFFECT_NONE;
    }

    const std::string mode = str::ToLowerAscii(m_cfg.dragDrop.mode);
    const bool sameDrive = HasSameDriveW(m_oleDragPathsW.front(), outZone->targetDirW);
    const bool ctrl = (keyState & MK_CONTROL) != 0;
    const bool shift = (keyState & MK_SHIFT) != 0;

    DWORD desired = DROPEFFECT_COPY;
    if (mode == "ask_always" || (mode == "ask_cross_drive" && !sameDrive)) {
        desired = DROPEFFECT_COPY;
    } else if (ctrl && shift) {
        desired = DROPEFFECT_LINK;
    } else if (ctrl) {
        desired = DROPEFFECT_COPY;
    } else if (shift) {
        desired = DROPEFFECT_MOVE;
    } else {
        desired = sameDrive ? DROPEFFECT_MOVE : DROPEFFECT_COPY;
    }

    if ((desired & allowedEffects) != 0) return desired;
    if ((allowedEffects & DROPEFFECT_COPY) != 0) return DROPEFFECT_COPY;
    if ((allowedEffects & DROPEFFECT_MOVE) != 0) return DROPEFFECT_MOVE;
    if ((allowedEffects & DROPEFFECT_LINK) != 0) return DROPEFFECT_LINK;
    return DROPEFFECT_NONE;
}

void VelaApp::ExecuteDropAction(
    const std::vector<std::wstring>& sourcesW, const std::wstring& destDirW, DWORD keyState
) {
    if (sourcesW.empty() || destDirW.empty() || !m_fileOps) return;

    const std::string mode = str::ToLowerAscii(m_cfg.dragDrop.mode);
    const bool sameDrive = HasSameDriveW(sourcesW.front(), destDirW);

    if (mode == "ask_always" || (mode == "ask_cross_drive" && !sameDrive)) {
        m_dialogs.OpenDragDropPopup(sourcesW, destDirW);
        return;
    }

    const bool ctrl = (keyState & MK_CONTROL) != 0;
    const bool shift = (keyState & MK_SHIFT) != 0;

    FileOpKind kind = FileOpKind::Copy;
    if (ctrl && shift) {
        kind = FileOpKind::CreateSymlink;
    } else if (ctrl) {
        kind = FileOpKind::Copy;
    } else if (shift) {
        kind = FileOpKind::Move;
    } else {
        kind = sameDrive ? FileOpKind::Move : FileOpKind::Copy;
    }

    FileOpRequest req;
    req.kind = kind;
    req.sources = sourcesW;
    req.destDirW = destDirW;
    req.ownerHwnd = m_window ? m_window->GetHandle() : nullptr;
    m_fileOps->Enqueue(std::move(req));
}

DWORD VelaApp::OnOleDragEnter(
    POINT clientPt, DWORD keyState, DWORD allowedEffects, const std::vector<std::wstring>& pathsW
) {
    m_oleDragActive = true;
    m_oleDragPathsW = pathsW;
    m_oleDragClientPt = clientPt;

    const DropZone* zone = nullptr;
    const DWORD effect = EvaluateDropEffect(clientPt, keyState, allowedEffects, zone);
    m_hoveredDropZoneIdx = zone ? static_cast<int>(zone - m_dropZones.data()) : -1;
    if (m_window) m_window->Wake();
    return effect;
}

DWORD VelaApp::OnOleDragOver(POINT clientPt, DWORD keyState, DWORD allowedEffects) {
    m_oleDragClientPt = clientPt;
    const int prevZone = m_hoveredDropZoneIdx;

    const DropZone* zone = nullptr;
    const DWORD effect = EvaluateDropEffect(clientPt, keyState, allowedEffects, zone);
    m_hoveredDropZoneIdx = zone ? static_cast<int>(zone - m_dropZones.data()) : -1;
    if (m_hoveredDropZoneIdx != prevZone && m_window) {
        m_window->Wake();
    }
    return effect;
}

void VelaApp::OnOleDragLeave() {
    m_oleDragActive = false;
    m_oleDragPathsW.clear();
    m_hoveredDropZoneIdx = -1;
    if (m_window) m_window->Wake();
}

DWORD VelaApp::OnOleDrop(
    POINT clientPt, DWORD keyState, DWORD allowedEffects, std::vector<std::wstring> pathsW
) {
    m_oleDragPathsW = pathsW;
    const DropZone* zone = nullptr;
    const DWORD effect = EvaluateDropEffect(clientPt, keyState, allowedEffects, zone);
    const std::wstring targetDirW = zone ? zone->targetDirW : L"";

    m_oleDragActive = false;
    m_oleDragPathsW.clear();
    m_hoveredDropZoneIdx = -1;

    if (effect != DROPEFFECT_NONE && !targetDirW.empty()) {
        ExecuteDropAction(pathsW, targetDirW, keyState);
        if (m_window) m_window->Wake();
        // Always report COPY when Thunar popup asks, so external sources never auto-delete
        const std::string mode = str::ToLowerAscii(m_cfg.dragDrop.mode);
        if (mode == "ask_always") return DROPEFFECT_COPY;
        return effect;
    }

    if (m_window) m_window->Wake();
    return DROPEFFECT_NONE;
}

void VelaApp::HandleGlobalShortcuts() {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    if (m_dialogs.IsAnyModalOpen()) return;

    TabState& tab = m_tabs[m_activeTab];
    const ImGuiIO& io = ImGui::GetIO();

    if (MatchKeybind(m_cfg.keybinds.reloadConfig)) {
        if (m_onReloadConfig) m_onReloadConfig();
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.refresh)) {
        RefreshCurrentTab();
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.newTab)) {
        OpenNewTab(NavTarget{true, L""}, true);
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.closeTab)) {
        CloseTab(m_activeTab);
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.focusAddress)) {
        if (!tab.isHome) {
            std::snprintf(m_addressBuffer, sizeof(m_addressBuffer), "%s", tab.currentPath.c_str());
        } else {
            m_addressBuffer[0] = '\0';
        }
        m_editingAddress = true;
        m_focusAddressNextFrame = true;
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.focusSearch)) {
        m_focusSearchNextFrame = true;
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.copyPath)) {
        CopySelectedPaths(CopyPathFormat::Windows);
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.copyPathPosix)) {
        CopySelectedPaths(CopyPathFormat::Posix);
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.copyPathQuoted)) {
        CopySelectedPaths(CopyPathFormat::Quoted);
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.toggleHidden)) {
        tab.showHidden = !tab.showHidden;
        RebuildVisibleIndices(tab);
        ShowToast(false, tab.showHidden ? "Showing hidden items" : "Hiding hidden items");
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.toggleSidebar)) {
        m_cfg.layout.showSidebar = !m_cfg.layout.showSidebar;
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.openTerminal)) {
        OpenSelectedInTerminal();
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.goUp)) {
        NavigateUp();
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.goBack)) {
        NavigateBack();
        return;
    }
    if (MatchKeybind(m_cfg.keybinds.goForward)) {
        NavigateForward();
        return;
    }

    if (!io.WantTextInput) {
        if (MatchKeybind(m_cfg.keybinds.copy)) {
            CopySelectedFilesToClipboard(false);
        } else if (MatchKeybind(m_cfg.keybinds.cut)) {
            CopySelectedFilesToClipboard(true);
        } else if (MatchKeybind(m_cfg.keybinds.paste)) {
            PasteClipboardFiles();
        } else if (MatchKeybind(m_cfg.keybinds.newFolder)) {
            PromptNewItem(true);
        } else if (MatchKeybind(m_cfg.keybinds.rename)) {
            PromptRenameSelected();
        } else if (MatchKeybind(m_cfg.keybinds.deletePermanent)) {
            PromptDeleteSelected(true);
        } else if (MatchKeybind(m_cfg.keybinds.deleteItem)) {
            PromptDeleteSelected(false);
        } else if (MatchKeybind(m_cfg.keybinds.properties)) {
            OpenSelectedProperties();
        } else if (MatchKeybind(m_cfg.keybinds.selectAll)) {
            SelectAll(tab);
        } else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
            NavigateUp();
        } else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            if (m_searchBuffer[0] != '\0') {
                m_searchBuffer[0] = '\0';
                RebuildVisibleIndices(tab);
            } else {
                ClearSelection(tab);
            }
        } else if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift &&
                   ImGui::IsKeyPressed(ImGuiKey_Enter, false) &&
                   tab.focusedVisibleIdx >= 0 &&
                   tab.focusedVisibleIdx < static_cast<int>(tab.visibleIndices.size())) {
            ActivateEntry(tab, tab.visibleIndices[tab.focusedVisibleIdx]);
        }
    }

    // Mouse thumb buttons (Back / Forward)
    if (ImGui::IsMouseClicked(3)) NavigateBack();
    if (ImGui::IsMouseClicked(4)) NavigateForward();
}

void VelaApp::Render(float dpiScale, TitlebarHitRegions& outRegions) {
    m_dropZones.clear();
    outRegions.interactiveRects.clear();
    outRegions.titlebarHeightPx = static_cast<int>(std::round(m_cfg.window.titlebarHeight * dpiScale));

    ImGuiStyle& style = ImGui::GetStyle();
    style.PopupRounding = m_cfg.theme.cardRounding * dpiScale;
    style.FrameRounding = m_cfg.theme.itemRounding * dpiScale;
    style.ScrollbarRounding = m_cfg.theme.pillRounding * dpiScale;
    style.PopupBorderSize = 1.0f;
    style.WindowPadding = ImVec2(6.0f * dpiScale, 6.0f * dpiScale);
    style.ItemSpacing = ImVec2(18.0f * dpiScale, 6.0f * dpiScale);

    style.Colors[ImGuiCol_Text]                 = ToImVec4(m_cfg.theme.textPrimary);
    style.Colors[ImGuiCol_TextDisabled]         = ToImVec4(m_cfg.theme.textMuted);
    style.Colors[ImGuiCol_WindowBg]             = ToImVec4(m_cfg.theme.popupBg);
    style.Colors[ImGuiCol_PopupBg]              = ToImVec4(m_cfg.theme.popupBg);
    style.Colors[ImGuiCol_Border]               = ToImVec4(m_cfg.theme.popupBorder);
    style.Colors[ImGuiCol_FrameBg]              = ToImVec4(m_cfg.theme.inputBg);
    style.Colors[ImGuiCol_FrameBgHovered]       = ToImVec4(m_cfg.theme.hoverBg);
    style.Colors[ImGuiCol_FrameBgActive]        = ToImVec4(m_cfg.theme.selectionBg);
    style.Colors[ImGuiCol_Button]               = ToImVec4(m_cfg.theme.buttonBg);
    style.Colors[ImGuiCol_ButtonHovered]        = ToImVec4(m_cfg.theme.buttonHoverBg);
    style.Colors[ImGuiCol_ButtonActive]         = ToImVec4(m_cfg.theme.buttonActiveBg);
    style.Colors[ImGuiCol_Header]               = ToImVec4(m_cfg.theme.selectionBg);
    style.Colors[ImGuiCol_HeaderHovered]        = ToImVec4(m_cfg.theme.hoverBg);
    style.Colors[ImGuiCol_HeaderActive]         = ToImVec4(m_cfg.theme.selectionBg);
    style.Colors[ImGuiCol_Separator]            = ToImVec4(m_cfg.theme.separator);
    style.Colors[ImGuiCol_SeparatorHovered]     = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_SeparatorActive]      = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_ResizeGrip]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_ResizeGripHovered]    = ToImVec4(m_cfg.theme.hoverBg);
    style.Colors[ImGuiCol_ResizeGripActive]     = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_CheckMark]            = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_PlotHistogram]        = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_ScrollbarBg]          = ToImVec4(m_cfg.theme.scrollbarBg);
    style.Colors[ImGuiCol_ScrollbarGrab]        = ToImVec4(m_cfg.theme.scrollbarGrab);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = ToImVec4(m_cfg.theme.scrollbarGrabHover);
    style.Colors[ImGuiCol_ScrollbarGrabActive]  = ToImVec4(m_cfg.theme.scrollbarGrabHover);
    style.Colors[ImGuiCol_TableHeaderBg]        = ToImVec4(m_cfg.theme.buttonBg);
    style.Colors[ImGuiCol_TableBorderStrong]    = ToImVec4(m_cfg.theme.contentCardBorder);
    style.Colors[ImGuiCol_TableBorderLight]     = ToImVec4(m_cfg.theme.separator);
    style.Colors[ImGuiCol_TableRowBg]           = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    style.Colors[ImGuiCol_TableRowBgAlt]        = ToImVec4(m_cfg.theme.hoverBg.WithAlpha(m_cfg.theme.hoverBg.a * 0.45f));
    style.Colors[ImGuiCol_TextSelectedBg]       = ToImVec4(m_cfg.theme.accent.WithAlpha(0.32f));
    style.Colors[ImGuiCol_NavCursor]            = ToImVec4(m_cfg.theme.accent);
    style.Colors[ImGuiCol_ModalWindowDimBg]     = m_cfg.window.darkMode
        ? ImVec4(0.0f, 0.0f, 0.0f, 0.45f)
        : ImVec4(0.0f, 0.0f, 0.0f, 0.22f);

    HandleGlobalShortcuts();

    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(io.DisplaySize);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags rootFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground;

    if (ImGui::Begin("##VelaRoot", nullptr, rootFlags)) {
        if (m_cfg.window.backdrop == BackdropMode::Solid || m_cfg.theme.chromeBg.a > 0.0f) {
            ColorRGBA bg = m_cfg.theme.chromeBg;
            if (m_cfg.window.backdrop == BackdropMode::Solid && bg.a == 0.0f) {
                bg = m_cfg.window.darkMode
                    ? ColorRGBA{0.10f, 0.10f, 0.10f, 1.0f}
                    : ColorRGBA{0.95f, 0.95f, 0.96f, 1.0f};
            }
            ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, bg.ToImU32());
        }

        RenderTitlebar(dpiScale, outRegions);
        RenderNavBar(dpiScale);
        if (m_cfg.layout.showToolbar) {
            RenderToolbar(dpiScale);
        }

        const float statusH = m_cfg.layout.showStatusbar ? (m_cfg.layout.statusbarHeight * dpiScale) : 0.0f;
        const float bodyH = (std::max)(100.0f, io.DisplaySize.y - ImGui::GetCursorPosY() - statusH);
        const bool sidebarRight = (m_cfg.layout.sidebarPosition == "right");

        if (ImGui::BeginChild("##VelaBody", ImVec2(io.DisplaySize.x, bodyH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground)) {
            if (m_cfg.layout.showSidebar) {
                if (sidebarRight) {
                    RenderContentCard(dpiScale);
                    ImGui::SameLine(0.0f, 0.0f);
                    RenderSidebar(dpiScale);
                } else {
                    RenderSidebar(dpiScale);
                    ImGui::SameLine(0.0f, 0.0f);
                    RenderContentCard(dpiScale);
                }
            } else {
                RenderContentCard(dpiScale);
            }
        }
        ImGui::EndChild();

        if (m_cfg.layout.showStatusbar) {
            ImGui::SetCursorPos(ImVec2(0.0f, io.DisplaySize.y - statusH));
            RenderStatusBar(dpiScale);
        }

        RenderToast(dpiScale);

        if (m_oleDragActive) {
            const DropZone* zone = ResolveDropZone(m_oleDragClientPt);
            if (zone && CanDropSourcesInto(m_oleDragPathsW, zone->targetDirW)) {
                ImDrawList* fg = ImGui::GetForegroundDrawList();
                const ImVec2 zMin(zone->minX, zone->minY);
                const ImVec2 zMax(zone->maxX, zone->maxY);
                const float round = (zone->kind == DropZone::Kind::Viewport)
                    ? (m_cfg.theme.cardRounding * dpiScale)
                    : (m_cfg.theme.itemRounding * dpiScale);

                ColorRGBA fill = m_cfg.theme.accent;
                fill.a = (zone->kind == DropZone::Kind::Viewport) ? 0.08f : 0.18f;
                fg->AddRectFilled(zMin, zMax, fill.ToImU32(), round);
                fg->AddRect(zMin, zMax, m_cfg.theme.accent.ToImU32(), round, 0, 2.0f * dpiScale);
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);

    // Render modals/popups outside ##VelaRoot so they retain Fluent WindowPadding & ItemSpacing
    m_dialogs.Render(m_cfg, dpiScale);
}

void VelaApp::RenderTitlebar(float dpiScale, TitlebarHitRegions& outRegions) {
    const float h = m_cfg.window.titlebarHeight * dpiScale;
    const float totalW = ImGui::GetIO().DisplaySize.x;
    const float chromeBtnW = 46.0f * dpiScale;
    const float chromeGroupW = chromeBtnW * 3.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 barMin = ImGui::GetCursorScreenPos();

    const ImVec2 minPos(barMin.x + totalW - chromeGroupW, barMin.y);
    const ImVec2 maxPos(minPos.x + chromeBtnW, barMin.y);
    const ImVec2 closePos(maxPos.x + chromeBtnW, barMin.y);

    // 1. Minimize
    ImGui::SetCursorScreenPos(minPos);
    if (ImGui::InvisibleButton("##WinMin", ImVec2(chromeBtnW, h)) && m_window) {
        m_window->Minimize();
    }
    outRegions.interactiveRects.push_back(ToRect(minPos, ImVec2(minPos.x + chromeBtnW, minPos.y + h)));
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(minPos, ImVec2(minPos.x + chromeBtnW, minPos.y + h), m_cfg.theme.hoverBg.ToImU32());
    }
    {
        const ImVec2 ts = ImGui::CalcTextSize(icons::ChromeMinimize);
        dl->AddText(
            ImVec2(minPos.x + (chromeBtnW - ts.x) * 0.5f, minPos.y + (h - ts.y) * 0.5f),
            m_cfg.theme.textPrimary.ToImU32(), icons::ChromeMinimize
        );
    }

    // 2. Maximize / Restore (HTMAXBUTTON enables Windows 11 Snap Layouts)
    outRegions.maximizeButton = ToRect(maxPos, ImVec2(maxPos.x + chromeBtnW, maxPos.y + h));
    if (m_window && m_window->IsMaximizePressed()) {
        dl->AddRectFilled(maxPos, ImVec2(maxPos.x + chromeBtnW, maxPos.y + h), m_cfg.theme.selectionBg.ToImU32());
    } else if (m_window && m_window->IsMaximizeHovered()) {
        dl->AddRectFilled(maxPos, ImVec2(maxPos.x + chromeBtnW, maxPos.y + h), m_cfg.theme.hoverBg.ToImU32());
    }
    {
        const char* maxGlyph = (m_window && m_window->IsMaximized()) ? icons::ChromeRestore : icons::ChromeMaximize;
        const ImVec2 ts = ImGui::CalcTextSize(maxGlyph);
        dl->AddText(
            ImVec2(maxPos.x + (chromeBtnW - ts.x) * 0.5f, maxPos.y + (h - ts.y) * 0.5f),
            m_cfg.theme.textPrimary.ToImU32(), maxGlyph
        );
    }

    // 3. Close
    ImGui::SetCursorScreenPos(closePos);
    if (ImGui::InvisibleButton("##WinClose", ImVec2(chromeBtnW, h)) && m_window) {
        m_window->Close();
    }
    outRegions.interactiveRects.push_back(ToRect(closePos, ImVec2(closePos.x + chromeBtnW, closePos.y + h)));
    const bool closeHovered = ImGui::IsItemHovered();
    if (closeHovered) {
        dl->AddRectFilled(closePos, ImVec2(closePos.x + chromeBtnW, closePos.y + h), m_cfg.theme.closeHoverBg.ToImU32());
    }
    {
        const ImVec2 ts = ImGui::CalcTextSize(icons::ChromeClose);
        dl->AddText(
            ImVec2(closePos.x + (chromeBtnW - ts.x) * 0.5f, closePos.y + (h - ts.y) * 0.5f),
            closeHovered ? IM_COL32(255, 255, 255, 255) : m_cfg.theme.textPrimary.ToImU32(),
            icons::ChromeClose
        );
    }

    // Tabs Strip
    float cursorX = barMin.x + 10.0f * dpiScale;
    const float tabTop = barMin.y + 6.0f * dpiScale;
    const float tabH = h - 6.0f * dpiScale;
    const float maxTabsWidth = (std::max)(120.0f, minPos.x - 44.0f * dpiScale - cursorX);
    const float tabW = (std::min)(216.0f * dpiScale, maxTabsWidth / (std::max)(1.0f, static_cast<float>(m_tabs.size())));
    const float tabSpacing = 4.0f * dpiScale;
    const float rounding = m_cfg.theme.itemRounding * dpiScale;

    for (int i = 0; i < static_cast<int>(m_tabs.size()); ++i) {
        if (cursorX + tabW > minPos.x - 36.0f * dpiScale) break;

        const ImVec2 tMin(cursorX, tabTop);
        const ImVec2 tMax(cursorX + tabW, tabTop + tabH);
        const bool isActive = (i == m_activeTab);

        ImGui::SetCursorScreenPos(tMin);
        char btnId[32];
        std::snprintf(btnId, sizeof(btnId), "##Tab%d", i);
        if (ImGui::InvisibleButton(btnId, ImVec2((std::max)(24.0f, tabW - 28.0f * dpiScale), tabH))) {
            SwitchTab(i);
        }
        const bool tabHovered = ImGui::IsItemHovered();
        outRegions.interactiveRects.push_back(ToRect(tMin, tMax));
        if (!m_tabs[i].isHome && !m_tabs[i].currentPathW.empty()) {
            RegisterDropZone(DropZone::Kind::Tab, tMin.x, tMin.y, tMax.x, tMax.y, m_tabs[i].currentPathW, i);
        }

        if (isActive) {
            dl->AddRectFilled(tMin, tMax, m_cfg.theme.tabActiveBg.ToImU32(), rounding, ImDrawFlags_RoundCornersTop);
        } else if (tabHovered) {
            dl->AddRectFilled(tMin, tMax, m_cfg.theme.tabHoverBg.ToImU32(), rounding, ImDrawFlags_RoundCornersTop);
        }

        const char* tabIcon = m_tabs[i].isHome ? icons::Home : icons::Folder;
        const float textY = tMin.y + (tabH - ImGui::GetFontSize()) * 0.5f;
        dl->AddText(ImVec2(tMin.x + 12.0f * dpiScale, textY), m_cfg.theme.accent.ToImU32(), tabIcon);

        dl->PushClipRect(ImVec2(tMin.x + 34.0f * dpiScale, tMin.y), ImVec2(tMax.x - 28.0f * dpiScale, tMax.y), true);
        dl->AddText(
            ImVec2(tMin.x + 36.0f * dpiScale, textY),
            isActive ? m_cfg.theme.textPrimary.ToImU32() : m_cfg.theme.textSecondary.ToImU32(),
            m_tabs[i].title.c_str()
        );
        dl->PopClipRect();

        if (m_tabs.size() > 1) {
            const ImVec2 xMin(tMax.x - 26.0f * dpiScale, tMin.y + 4.0f * dpiScale);
            const ImVec2 xMax(tMax.x - 4.0f * dpiScale, tMax.y - 4.0f * dpiScale);
            ImGui::SetCursorScreenPos(xMin);
            std::snprintf(btnId, sizeof(btnId), "##TabClose%d", i);
            if (ImGui::InvisibleButton(btnId, ImVec2(xMax.x - xMin.x, xMax.y - xMin.y))) {
                CloseTab(i);
                break;
            }
            if (ImGui::IsItemHovered()) {
                dl->AddRectFilled(xMin, xMax, m_cfg.theme.hoverBg.ToImU32(), 4.0f * dpiScale);
            }
            const ImVec2 xTs = ImGui::CalcTextSize(icons::ChromeClose);
            dl->AddText(
                ImVec2(xMin.x + (xMax.x - xMin.x - xTs.x) * 0.5f, xMin.y + (xMax.y - xMin.y - xTs.y) * 0.5f),
                m_cfg.theme.textSecondary.ToImU32(), icons::ChromeClose
            );
        }

        cursorX += tabW + tabSpacing;
    }

    // "+" New Tab Button
    const float addSize = 28.0f * dpiScale;
    const ImVec2 addMin(cursorX + 4.0f * dpiScale, tabTop + (tabH - addSize) * 0.5f);
    const ImVec2 addMax(addMin.x + addSize, addMin.y + addSize);
    if (addMax.x < minPos.x - 8.0f * dpiScale) {
        ImGui::SetCursorScreenPos(addMin);
        if (ImGui::InvisibleButton("##NewTab", ImVec2(addSize, addSize))) {
            OpenNewTab(NavTarget{true, L""}, true);
        }
        outRegions.interactiveRects.push_back(ToRect(addMin, addMax));
        if (ImGui::IsItemHovered()) {
            dl->AddRectFilled(addMin, addMax, m_cfg.theme.hoverBg.ToImU32(), 4.0f * dpiScale);
        }
        const ImVec2 addTs = ImGui::CalcTextSize(icons::Add);
        dl->AddText(
            ImVec2(addMin.x + (addSize - addTs.x) * 0.5f, addMin.y + (addSize - addTs.y) * 0.5f),
            m_cfg.theme.textSecondary.ToImU32(), icons::Add
        );
    }

    ImGui::SetCursorScreenPos(ImVec2(barMin.x, barMin.y + h));
    ImGui::Dummy(ImVec2(totalW, 0.0f));
}

void VelaApp::RenderNavBar(float dpiScale) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];

    const float h = m_cfg.layout.navbarHeight * dpiScale;
    const float w = ImGui::GetIO().DisplaySize.x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    if (m_cfg.theme.toolbarBg.a > 0.0f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.toolbarBg.ToImU32());
    }

    const float padX = 12.0f * dpiScale;
    const float btnSize = 30.0f * dpiScale;
    const float btnY = pos.y + (h - btnSize) * 0.5f;
    float x = pos.x + padX;

    auto tip = [](const char* name, const std::string& kb) -> std::string {
        if (kb.empty()) return name;
        return std::string(name) + " (" + kb + ")";
    };

    auto drawNavBtn = [&](const char* id, const char* glyph, bool enabled, const char* tooltip) -> bool {
        ImGui::SetCursorScreenPos(ImVec2(x, btnY));
        const bool clicked = ImGui::InvisibleButton(id, ImVec2(btnSize, btnSize));
        if (ImGui::IsItemHovered()) {
            if (enabled) {
                dl->AddRectFilled(ImVec2(x, btnY), ImVec2(x + btnSize, btnY + btnSize),
                                  m_cfg.theme.hoverBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
            }
            if (tooltip) ImGui::SetTooltip("%s", tooltip);
        }
        const ImVec2 ts = ImGui::CalcTextSize(glyph);
        dl->AddText(
            ImVec2(x + (btnSize - ts.x) * 0.5f, btnY + (btnSize - ts.y) * 0.5f),
            enabled ? m_cfg.theme.textPrimary.ToImU32() : m_cfg.theme.textMuted.ToImU32(),
            glyph
        );
        x += btnSize + 4.0f * dpiScale;
        return enabled && clicked;
    };

    if (drawNavBtn("##NavBack", icons::Back, !tab.backHistory.empty(), tip("Back", m_cfg.keybinds.goBack).c_str())) {
        NavigateBack();
    }
    if (drawNavBtn("##NavFwd", icons::Forward, !tab.forwardHistory.empty(), tip("Forward", m_cfg.keybinds.goForward).c_str())) {
        NavigateForward();
    }
    if (drawNavBtn("##NavUp", icons::Up, !tab.isHome, tip("Up", m_cfg.keybinds.goUp).c_str())) {
        NavigateUp();
    }
    if (drawNavBtn("##NavRefresh", icons::Refresh, true, tip("Refresh", m_cfg.keybinds.refresh).c_str())) {
        RefreshCurrentTab();
    }

    x += 8.0f * dpiScale;

    const float settingsSize = 30.0f * dpiScale;
    const float searchW = (std::min)(250.0f * dpiScale, w * 0.25f);
    const float rightEdge = pos.x + w - padX;
    const float settingsX = rightEdge - settingsSize;
    const float searchX = settingsX - 8.0f * dpiScale - searchW;
    const float barH = 30.0f * dpiScale;
    const float barY = pos.y + (h - barH) * 0.5f;

    // Address / Breadcrumb Bar
    const float addrW = (std::max)(120.0f * dpiScale, searchX - 10.0f * dpiScale - x);
    const ImVec2 addrMin(x, barY);
    const ImVec2 addrMax(x + addrW, barY + barH);
    dl->AddRectFilled(addrMin, addrMax, m_cfg.theme.inputBg.ToImU32(), m_cfg.theme.pillRounding * dpiScale);
    dl->AddRect(addrMin, addrMax,
                m_editingAddress ? m_cfg.theme.accent.ToImU32() : m_cfg.theme.inputBorder.ToImU32(),
                m_cfg.theme.pillRounding * dpiScale);

    if (m_editingAddress) {
        if (m_focusAddressNextFrame) {
            ImGui::SetKeyboardFocusHere();
            m_focusAddressNextFrame = false;
        }
        ImGui::SetCursorScreenPos(ImVec2(addrMin.x + 10.0f * dpiScale, barY + 3.0f * dpiScale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textPrimary.ToImU32());
        ImGui::SetNextItemWidth(addrW - 20.0f * dpiScale);
        const bool submitted = ImGui::InputText(
            "##VelaAddressInput", m_addressBuffer, sizeof(m_addressBuffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll
        );
        const bool active = ImGui::IsItemActive();
        ImGui::PopStyleColor(2);

        if (submitted) {
            const std::string rawStr(m_addressBuffer);
            const std::string lower = str::ToLowerAscii(rawStr);
            if (lower == "home") {
                NavigateTo(NavTarget{true, L""});
            } else {
                const std::wstring expanded = str::ExpandEnvVars(str::Utf8ToWide(rawStr));
                const std::wstring clean = str::NormalizeWindowsPath(expanded);
                if (!clean.empty()) {
                    NavigateTo(NavTarget{false, clean});
                }
            }
            m_editingAddress = false;
        } else if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || (!active && ImGui::IsMouseClicked(0) && !ImGui::IsItemHovered())) {
            m_editingAddress = false;
        }
    } else {
        // Render clickable Breadcrumb segments inside address pill
        float bcX = addrMin.x + 8.0f * dpiScale;
        const float bcMaxX = addrMax.x - 26.0f * dpiScale;
        const float textY = addrMin.y + (barH - ImGui::GetFontSize()) * 0.5f;

        if (tab.isHome) {
            dl->AddText(ImVec2(bcX + 4.0f * dpiScale, textY), m_cfg.theme.textPrimary.ToImU32(), "Home");
            bcX += ImGui::CalcTextSize("Home").x + 12.0f * dpiScale;
        } else {
            const auto segs = SplitBreadcrumbs(tab.currentPathW);
            for (size_t s = 0; s < segs.size(); ++s) {
                const ImVec2 segTs = ImGui::CalcTextSize(segs[s].label.c_str());
                const float segW = segTs.x + 12.0f * dpiScale;
                if (bcX + segW > bcMaxX && s + 1 < segs.size()) {
                    continue; // Compact earlier segments when window is narrow
                }
                if (bcX + segW > bcMaxX) break;

                ImGui::SetCursorScreenPos(ImVec2(bcX, addrMin.y + 2.0f * dpiScale));
                char segId[32];
                std::snprintf(segId, sizeof(segId), "##Bc%zu", s);
                if (ImGui::InvisibleButton(segId, ImVec2(segW, barH - 4.0f * dpiScale))) {
                    NavigateTo(NavTarget{false, segs[s].pathW});
                }

                RegisterDropZone(
                    DropZone::Kind::Breadcrumb,
                    bcX, addrMin.y + 2.0f * dpiScale,
                    bcX + segW, addrMax.y - 2.0f * dpiScale,
                    segs[s].pathW
                );

                if (ImGui::IsItemHovered()) {
                    dl->AddRectFilled(
                        ImVec2(bcX, addrMin.y + 2.0f * dpiScale),
                        ImVec2(bcX + segW, addrMax.y - 2.0f * dpiScale),
                        m_cfg.theme.hoverBg.ToImU32(), 4.0f * dpiScale
                    );
                }
                dl->AddText(ImVec2(bcX + 6.0f * dpiScale, textY), m_cfg.theme.textPrimary.ToImU32(), segs[s].label.c_str());
                bcX += segW;

                if (s + 1 < segs.size()) {
                    const ImVec2 chevTs = ImGui::CalcTextSize(icons::ChevronRight);
                    if (bcX + chevTs.x + 4.0f * dpiScale < bcMaxX) {
                        dl->AddText(
                            ImVec2(bcX, addrMin.y + (barH - chevTs.y) * 0.5f),
                            m_cfg.theme.textMuted.ToImU32(), icons::ChevronRight
                        );
                        bcX += chevTs.x + 2.0f * dpiScale;
                    }
                }
            }
        }

        // Click remaining empty space in pill to enter text-editing mode
        const float restW = addrMax.x - 24.0f * dpiScale - bcX;
        if (restW > 8.0f) {
            ImGui::SetCursorScreenPos(ImVec2(bcX, addrMin.y));
            if (ImGui::InvisibleButton("##EnterEditAddr", ImVec2(restW, barH))) {
                std::snprintf(m_addressBuffer, sizeof(m_addressBuffer), "%s", tab.isHome ? "" : tab.currentPath.c_str());
                m_editingAddress = true;
                m_focusAddressNextFrame = true;
            }
        }

        const ImVec2 chevTs = ImGui::CalcTextSize(icons::ChevronDown);
        dl->AddText(
            ImVec2(addrMax.x - 22.0f * dpiScale, addrMin.y + (barH - chevTs.y) * 0.5f),
            m_cfg.theme.textSecondary.ToImU32(), icons::ChevronDown
        );
    }

    // Search Pill (Instant In-Memory Filter)
    const ImVec2 searchMin(searchX, barY);
    const ImVec2 searchMax(searchX + searchW, barY + barH);
    dl->AddRectFilled(searchMin, searchMax, m_cfg.theme.inputBg.ToImU32(), m_cfg.theme.pillRounding * dpiScale);
    dl->AddRect(searchMin, searchMax, m_cfg.theme.inputBorder.ToImU32(), m_cfg.theme.pillRounding * dpiScale);

    if (m_focusSearchNextFrame) {
        ImGui::SetKeyboardFocusHere();
        m_focusSearchNextFrame = false;
    }
    ImGui::SetCursorScreenPos(ImVec2(searchMin.x + 10.0f * dpiScale, barY + 3.0f * dpiScale));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textPrimary.ToImU32());
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, m_cfg.theme.textSecondary.ToImU32());
    ImGui::SetNextItemWidth(searchW - 38.0f * dpiScale);
    if (ImGui::InputTextWithHint("##VelaSearch", "Search", m_searchBuffer, sizeof(m_searchBuffer))) {
        RebuildVisibleIndices(tab);
    }
    ImGui::PopStyleColor(3);

    const ImVec2 searchIconTs = ImGui::CalcTextSize(icons::Search);
    dl->AddText(
        ImVec2(searchMax.x - 24.0f * dpiScale, barY + (barH - searchIconTs.y) * 0.5f),
        m_cfg.theme.textSecondary.ToImU32(), icons::Search
    );

    // Settings Button (Opens config/vela.lua directly in user's default Lua/code editor)
    ImGui::SetCursorScreenPos(ImVec2(settingsX, btnY));
    if (ImGui::InvisibleButton("##NavSettings", ImVec2(settingsSize, settingsSize)) && m_window) {
        m_window->EditFileDefault(m_configPath);
        ShowToast(false, "Opened config/vela.lua");
    }
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(ImVec2(settingsX, btnY), ImVec2(settingsX + settingsSize, btnY + settingsSize),
                          m_cfg.theme.hoverBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
        ImGui::SetTooltip("Open config/vela.lua (Hot-reloads on save)");
    }
    const ImVec2 setTs = ImGui::CalcTextSize(icons::Settings);
    dl->AddText(
        ImVec2(settingsX + (settingsSize - setTs.x) * 0.5f, btnY + (settingsSize - setTs.y) * 0.5f),
        m_cfg.theme.textSecondary.ToImU32(), icons::Settings
    );

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

bool VelaApp::ToolbarIconButton(const char* id, const char* icon, const char* tooltip, float dpiScale,
                                const char* label, bool hasDropdown, bool enabled) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float btnH = 28.0f * dpiScale;
    const float padX = 8.0f * dpiScale;
    const ImVec2 iconTs = ImGui::CalcTextSize(icon);
    float w = iconTs.x + padX * 2.0f;

    ImVec2 labelTs{};
    if (label) {
        labelTs = ImGui::CalcTextSize(label);
        w += labelTs.x + 6.0f * dpiScale;
    }
    if (hasDropdown) {
        w += 14.0f * dpiScale;
    }

    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, btnH));
    if (ImGui::IsItemHovered()) {
        if (enabled) {
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + btnH),
                              m_cfg.theme.hoverBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
        }
        if (tooltip) {
            ImGui::SetTooltip("%s", tooltip);
        }
    }

    const uint32_t fg = enabled ? m_cfg.theme.textPrimary.ToImU32() : m_cfg.theme.textMuted.ToImU32();
    float drawX = pos.x + padX;
    dl->AddText(ImVec2(drawX, pos.y + (btnH - iconTs.y) * 0.5f), fg, icon);
    drawX += iconTs.x;

    if (label) {
        drawX += 6.0f * dpiScale;
        dl->AddText(ImVec2(drawX, pos.y + (btnH - labelTs.y) * 0.5f), fg, label);
        drawX += labelTs.x;
    }
    if (hasDropdown) {
        const ImVec2 chevTs = ImGui::CalcTextSize(icons::ChevronDown);
        dl->AddText(ImVec2(drawX + 4.0f * dpiScale, pos.y + (btnH - chevTs.y) * 0.5f),
                    enabled ? m_cfg.theme.textSecondary.ToImU32() : m_cfg.theme.textMuted.ToImU32(),
                    icons::ChevronDown);
    }

    return enabled && clicked;
}

void VelaApp::RenderToolbar(float dpiScale) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];

    const float h = m_cfg.layout.toolbarHeight * dpiScale;
    const float w = ImGui::GetIO().DisplaySize.x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    if (m_cfg.theme.toolbarBg.a > 0.0f) {
        ImGui::GetWindowDrawList()->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.toolbarBg.ToImU32());
    }

    const bool sidebarLeft = m_cfg.layout.showSidebar && (m_cfg.layout.sidebarPosition != "right");
    const float sidebarOffset = sidebarLeft ? (m_cfg.layout.sidebarWidth * dpiScale) : (12.0f * dpiScale);
    const float y = pos.y + (h - 28.0f * dpiScale) * 0.5f;
    const bool hasDir = !tab.isHome && !tab.currentPathW.empty();
    const bool hasSelection = !tab.selectedAllIndices.empty();
    const bool hasClipboard = hasDir && OleDragDropHost::HasClipboardFiles();

    auto tip = [](const char* name, const std::string& kb) -> std::string {
        if (kb.empty()) return name;
        return std::string(name) + " (" + kb + ")";
    };

    ImGui::SetCursorScreenPos(ImVec2(pos.x + sidebarOffset, y));
    if (ToolbarIconButton("##TbNew", icons::Add, tip("New Item", m_cfg.keybinds.newFolder).c_str(), dpiScale, "New", true, hasDir)) {
        ImGui::OpenPopup("##NewItemPopup");
    }
    if (ImGui::BeginPopup("##NewItemPopup")) {
        if (ImGui::MenuItem("New Folder", m_cfg.keybinds.newFolder.c_str())) PromptNewItem(true);
        if (ImGui::MenuItem("New Text File")) PromptNewItem(false);
        ImGui::EndPopup();
    }

    ImGui::SameLine(0.0f, 10.0f * dpiScale);
    if (ToolbarIconButton("##TbCut", icons::Cut, tip("Cut", m_cfg.keybinds.cut).c_str(), dpiScale, nullptr, false, hasSelection)) {
        CopySelectedFilesToClipboard(true);
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    if (ToolbarIconButton("##TbCopy", icons::Copy, tip("Copy", m_cfg.keybinds.copy).c_str(), dpiScale, nullptr, false, hasSelection)) {
        CopySelectedFilesToClipboard(false);
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    if (ToolbarIconButton("##TbPaste", icons::Paste, tip("Paste", m_cfg.keybinds.paste).c_str(), dpiScale, nullptr, false, hasClipboard)) {
        PasteClipboardFiles();
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    if (ToolbarIconButton("##TbRename", icons::Rename, tip("Rename", m_cfg.keybinds.rename).c_str(), dpiScale, nullptr, false, hasSelection)) {
        PromptRenameSelected();
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    if (ToolbarIconButton("##TbTerminal", icons::Terminal, tip("Open in Terminal", m_cfg.keybinds.openTerminal).c_str(), dpiScale, nullptr, false, true)) {
        OpenSelectedInTerminal();
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    {
        std::string delTip = "Delete";
        if (!m_cfg.keybinds.deleteItem.empty() || !m_cfg.keybinds.deletePermanent.empty()) {
            delTip += " (" + m_cfg.keybinds.deleteItem + " / " + m_cfg.keybinds.deletePermanent + ")";
        }
        if (ToolbarIconButton("##TbDelete", icons::Delete, delTip.c_str(), dpiScale, nullptr, false, hasSelection)) {
            PromptDeleteSelected(ImGui::GetIO().KeyShift);
        }
    }
    ImGui::SameLine(0.0f, 6.0f * dpiScale);
    if (ToolbarIconButton("##TbInfo", icons::Info, tip("Properties", m_cfg.keybinds.properties).c_str(), dpiScale, nullptr, false, hasDir)) {
        OpenSelectedProperties();
    }

    // Right-aligned Selection, Sort, View, and More menus
    const float rightStart = pos.x + w - 215.0f * dpiScale;
    if (rightStart > ImGui::GetCursorScreenPos().x + 20.0f * dpiScale) {
        ImGui::SetCursorScreenPos(ImVec2(rightStart, y));

        if (ToolbarIconButton("##TbSelect", icons::MultiSelect, "Selection & Copy Path Options", dpiScale, nullptr, true, hasDir)) {
            ImGui::OpenPopup("##SelectPopup");
        }
        if (ImGui::BeginPopup("##SelectPopup")) {
            if (ImGui::MenuItem("Select all", m_cfg.keybinds.selectAll.c_str())) SelectAll(tab);
            if (ImGui::MenuItem("Clear selection", "Esc")) ClearSelection(tab);
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Path (Windows)", m_cfg.keybinds.copyPath.c_str())) CopySelectedPaths(CopyPathFormat::Windows);
            if (ImGui::MenuItem("Copy Path (POSIX /)", m_cfg.keybinds.copyPathPosix.c_str())) CopySelectedPaths(CopyPathFormat::Posix);
            if (ImGui::MenuItem("Copy Path (Quoted)", m_cfg.keybinds.copyPathQuoted.c_str())) CopySelectedPaths(CopyPathFormat::Quoted);
            if (ImGui::MenuItem("Copy File Name")) CopySelectedPaths(CopyPathFormat::Filename);
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, 6.0f * dpiScale);
        if (ToolbarIconButton("##TbSort", icons::Sort, "Sort By", dpiScale, nullptr, true, hasDir)) {
            ImGui::OpenPopup("##SortPopup");
        }
        if (ImGui::BeginPopup("##SortPopup")) {
            auto sortItem = [&](const char* lbl, SortField f) {
                if (ImGui::MenuItem(lbl, nullptr, tab.sortField == f)) {
                    tab.sortField = f;
                    SortAndFilterTab(tab);
                }
            };
            sortItem("Name", SortField::Name);
            sortItem("Date modified", SortField::DateModified);
            sortItem("Type", SortField::Type);
            sortItem("Size", SortField::Size);
            ImGui::Separator();
            if (ImGui::MenuItem("Ascending", nullptr, tab.sortOrder == SortOrder::Ascending)) {
                tab.sortOrder = SortOrder::Ascending;
                SortAndFilterTab(tab);
            }
            if (ImGui::MenuItem("Descending", nullptr, tab.sortOrder == SortOrder::Descending)) {
                tab.sortOrder = SortOrder::Descending;
                SortAndFilterTab(tab);
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, 6.0f * dpiScale);
        const char* activeViewIcon = icons::ViewGrid;
        if (tab.viewMode == ViewMode::List) activeViewIcon = icons::ViewList;
        else if (tab.viewMode == ViewMode::Details) activeViewIcon = icons::ViewDetails;

        if (ToolbarIconButton("##TbLayout", activeViewIcon, "Layout & View Mode", dpiScale, nullptr, true, true)) {
            ImGui::OpenPopup("##ViewPopup");
        }
        if (ImGui::BeginPopup("##ViewPopup")) {
            if (ImGui::MenuItem("Grid", nullptr, tab.viewMode == ViewMode::Grid)) tab.viewMode = ViewMode::Grid;
            if (ImGui::MenuItem("List", nullptr, tab.viewMode == ViewMode::List)) tab.viewMode = ViewMode::List;
            if (ImGui::MenuItem("Details", nullptr, tab.viewMode == ViewMode::Details)) tab.viewMode = ViewMode::Details;
            ImGui::Separator();
            if (ImGui::MenuItem("Show hidden items", m_cfg.keybinds.toggleHidden.c_str(), tab.showHidden)) {
                tab.showHidden = !tab.showHidden;
                RebuildVisibleIndices(tab);
            }
            ImGui::EndPopup();
        }

        ImGui::SameLine(0.0f, 6.0f * dpiScale);
        ToolbarIconButton("##TbPreview", icons::PreviewPane, "Preview Pane (Phase 4)", dpiScale, nullptr, false, false);
        ImGui::SameLine(0.0f, 6.0f * dpiScale);
        if (ToolbarIconButton("##TbMore", icons::More, "More Options", dpiScale, nullptr, false, true)) {
            ImGui::OpenPopup("##MorePopup");
        }
        if (ImGui::BeginPopup("##MorePopup")) {
            if (ImGui::MenuItem("Open in Terminal", m_cfg.keybinds.openTerminal.c_str())) OpenSelectedInTerminal();
            if (ImGui::MenuItem("Edit config/vela.lua") && m_window) m_window->EditFileDefault(m_configPath);
            if (ImGui::MenuItem("Reload config/vela.lua", m_cfg.keybinds.reloadConfig.c_str()) && m_onReloadConfig) m_onReloadConfig();
            ImGui::EndPopup();
        }
    }

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0.0f));
}

bool VelaApp::SectionHeader(const char* id, const std::string& label, bool& expanded, float dpiScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 28.0f * dpiScale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    if (ImGui::InvisibleButton(id, ImVec2(w, h))) {
        expanded = !expanded;
    }
    if (ImGui::IsItemHovered()) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.hoverBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
    }

    dl->AddText(
        ImVec2(pos.x + 12.0f * dpiScale, pos.y + (h - ImGui::GetFontSize()) * 0.5f),
        m_cfg.theme.textSecondary.ToImU32(), label.c_str()
    );
    const char* chev = expanded ? icons::ChevronDown : icons::ChevronRight;
    const ImVec2 chevTs = ImGui::CalcTextSize(chev);
    dl->AddText(
        ImVec2(pos.x + w - 22.0f * dpiScale, pos.y + (h - chevTs.y) * 0.5f),
        m_cfg.theme.textSecondary.ToImU32(), chev
    );

    return expanded;
}

bool VelaApp::SidebarRow(const char* id, const char* icon, const std::string& label, bool active,
                         bool showPin, float dpiScale, const ColorRGBA* iconTint) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 32.0f * dpiScale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered();

    if (active) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.selectionBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
        const float pillH = 16.0f * dpiScale;
        const float pillY = pos.y + (h - pillH) * 0.5f;
        dl->AddRectFilled(
            ImVec2(pos.x + 2.0f * dpiScale, pillY),
            ImVec2(pos.x + 5.0f * dpiScale, pillY + pillH),
            m_cfg.theme.accent.ToImU32(),
            2.0f * dpiScale
        );
    } else if (hovered) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.hoverBg.ToImU32(), m_cfg.theme.itemRounding * dpiScale);
    }

    const ColorRGBA tint = iconTint ? *iconTint : m_cfg.theme.accent;
    const ImVec2 iconTs = ImGui::CalcTextSize(icon);
    dl->AddText(
        ImVec2(pos.x + 14.0f * dpiScale, pos.y + (h - iconTs.y) * 0.5f),
        tint.ToImU32(), icon
    );

    const float maxTextX = pos.x + w - (showPin ? 28.0f : 10.0f) * dpiScale;
    dl->PushClipRect(ImVec2(pos.x + 36.0f * dpiScale, pos.y), ImVec2(maxTextX, pos.y + h), true);
    dl->AddText(
        ImVec2(pos.x + 38.0f * dpiScale, pos.y + (h - ImGui::GetFontSize()) * 0.5f),
        m_cfg.theme.textPrimary.ToImU32(), label.c_str()
    );
    dl->PopClipRect();

    if (showPin) {
        const ImVec2 pinTs = ImGui::CalcTextSize(icons::Pin);
        dl->AddText(
            ImVec2(pos.x + w - 22.0f * dpiScale, pos.y + (h - pinTs.y) * 0.5f),
            m_cfg.theme.textMuted.ToImU32(), icons::Pin
        );
    }

    return clicked;
}

void VelaApp::RenderSidebar(float dpiScale) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];

    const float w = m_cfg.layout.sidebarWidth * dpiScale;
    const float h = ImGui::GetContentRegionAvail().y;

    auto isActivePath = [&](const std::wstring& targetW) -> bool {
        return !tab.isHome && !targetW.empty() && _wcsicmp(tab.currentPathW.c_str(), targetW.c_str()) == 0;
    };

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * dpiScale, 4.0f * dpiScale));
    if (ImGui::BeginChild("##VelaSidebar", ImVec2(w, h), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground)) {
        const ImVec2 sbMin = ImGui::GetWindowPos();
        const ImVec2 sbMax(sbMin.x + ImGui::GetWindowSize().x, sbMin.y + ImGui::GetWindowSize().y);
        if (m_cfg.theme.sidebarBg.a > 0.0f) {
            ImGui::GetWindowDrawList()->AddRectFilled(sbMin, sbMax, m_cfg.theme.sidebarBg.ToImU32());
        }

        auto regSidebarDrop = [&](const std::wstring& targetDirW) {
            if (targetDirW.empty()) return;
            const ImVec2 rMin = ImGui::GetItemRectMin();
            const ImVec2 rMax = ImGui::GetItemRectMax();
            const float cMinY = (std::max)(rMin.y, sbMin.y);
            const float cMaxY = (std::min)(rMax.y, sbMax.y);
            if (cMaxY > cMinY) {
                RegisterDropZone(DropZone::Kind::SidebarItem, rMin.x, cMinY, rMax.x, cMaxY, targetDirW);
            }
        };

        if (m_cfg.sidebar.showHome) {
            if (SidebarRow("##SbHome", icons::Home, "Home", tab.isHome, false, dpiScale)) {
                NavigateTo(NavTarget{true, L""});
            }
            ImGui::Dummy(ImVec2(0, 6.0f * dpiScale));
        }

        if (m_cfg.sidebar.showPinned) {
            if (SectionHeader("##SecPinned", "Pinned", m_pinnedExpanded, dpiScale)) {
                for (size_t i = 0; i < m_places.pinned.size(); ++i) {
                    const auto& item = m_places.pinned[i];
                    const char* glyph = icons::Folder;
                    if (item.icon == "desktop") glyph = icons::Desktop;
                    else if (item.icon == "downloads") glyph = icons::Download;
                    else if (item.icon == "documents") glyph = icons::Document;
                    else if (item.icon == "music") glyph = icons::Music;
                    else if (item.icon == "trash") glyph = icons::Delete;

                    char id[32];
                    std::snprintf(id, sizeof(id), "##Pin%zu", i);
                    const bool active = !item.isSpecialShell && isActivePath(item.pathW);
                    if (SidebarRow(id, glyph, item.name, active, true, dpiScale)) {
                        if (item.isSpecialShell && m_window) {
                            m_window->OpenShellItem(item.pathW);
                        } else {
                            NavigateTo(NavTarget{false, item.pathW});
                        }
                    }
                    if (!item.isSpecialShell) regSidebarDrop(item.pathW);
                }
            }
            ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
        }

        if (m_cfg.sidebar.showDrives) {
            if (SectionHeader("##SecDrives", "Drives", m_drivesExpanded, dpiScale)) {
                for (size_t i = 0; i < m_places.drives.size(); ++i) {
                    const auto& drv = m_places.drives[i];
                    char id[32];
                    std::snprintf(id, sizeof(id), "##Drv%zu", i);
                    if (SidebarRow(id, icons::Drive, drv.displayName, isActivePath(drv.rootPathW), false, dpiScale)) {
                        NavigateTo(NavTarget{false, drv.rootPathW});
                    }
                    regSidebarDrop(drv.rootPathW);
                    if (!drv.capacityText.empty() && ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", drv.capacityText.c_str());
                    }
                }
            }
            ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
        }

        if (m_cfg.sidebar.showCloudStorage && !m_places.cloud.empty()) {
            if (SectionHeader("##SecCloud", "Cloud Storage", m_cloudExpanded, dpiScale)) {
                for (size_t i = 0; i < m_places.cloud.size(); ++i) {
                    const auto& cld = m_places.cloud[i];
                    char id[32];
                    std::snprintf(id, sizeof(id), "##Cld%zu", i);
                    if (SidebarRow(id, icons::Cloud, cld.name, isActivePath(cld.pathW), false, dpiScale)) {
                        NavigateTo(NavTarget{false, cld.pathW});
                    }
                    regSidebarDrop(cld.pathW);
                }
            }
            ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
        }

        if (m_cfg.sidebar.showNetwork) {
            if (SectionHeader("##SecNet", "Network", m_networkExpanded, dpiScale)) {
                if (SidebarRow("##NetOpen", icons::Network, "Network Places", false, false, dpiScale) && m_window) {
                    m_window->OpenShellItem(L"shell:NetworkPlacesFolder");
                }
            }
        }

        if (m_cfg.sidebar.showWsl && !m_places.wsl.empty()) {
            if (SectionHeader("##SecWsl", "WSL", m_wslExpanded, dpiScale)) {
                for (size_t i = 0; i < m_places.wsl.size(); ++i) {
                    const auto& wsl = m_places.wsl[i];
                    char id[32];
                    std::snprintf(id, sizeof(id), "##Wsl%zu", i);
                    if (SidebarRow(id, icons::Terminal, wsl.name, isActivePath(wsl.pathW), false, dpiScale)) {
                        NavigateTo(NavTarget{false, wsl.pathW});
                    }
                    regSidebarDrop(wsl.pathW);
                }
            }
        }

        if (m_cfg.sidebar.showTags && !m_cfg.sidebar.tags.empty()) {
            ImGui::Dummy(ImVec2(0, 4.0f * dpiScale));
            if (SectionHeader("##SecTags", "Tags", m_tagsExpanded, dpiScale)) {
                for (size_t i = 0; i < m_cfg.sidebar.tags.size(); ++i) {
                    const auto& tag = m_cfg.sidebar.tags[i];
                    char id[32];
                    std::snprintf(id, sizeof(id), "##Tag%zu", i);
                    if (SidebarRow(id, icons::Tag, tag.name, false, false, dpiScale, &tag.color)) {
                        NavigateTo(NavTarget{true, L""});
                    }
                }
            }
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void VelaApp::RenderContentCard(float dpiScale) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    TabState& tab = m_tabs[m_activeTab];

    const bool sidebarRight = m_cfg.layout.showSidebar && (m_cfg.layout.sidebarPosition == "right");
    const float sidebarReserve = sidebarRight ? (m_cfg.layout.sidebarWidth * dpiScale) : 0.0f;
    const float availW = (std::max)(100.0f, ImGui::GetContentRegionAvail().x - sidebarReserve);
    const float availH = ImGui::GetContentRegionAvail().y;
    const float outerMargin = 8.0f * dpiScale;
    const float cardW = (std::max)(100.0f, availW - outerMargin);

    if (sidebarRight) {
        const ImVec2 cur = ImGui::GetCursorScreenPos();
        ImGui::SetCursorScreenPos(ImVec2(cur.x + outerMargin, cur.y));
    }

    const ImVec2 cardMin = ImGui::GetCursorScreenPos();
    const ImVec2 cardMax(cardMin.x + cardW, cardMin.y + availH);

    ImDrawList* parentDl = ImGui::GetWindowDrawList();
    const float rounding = m_cfg.theme.cardRounding * dpiScale;
    parentDl->AddRectFilled(cardMin, cardMax, m_cfg.theme.contentCardBg.ToImU32(), rounding);
    parentDl->AddRect(cardMin, cardMax, m_cfg.theme.contentCardBorder.ToImU32(), rounding);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * dpiScale, 12.0f * dpiScale));
    if (ImGui::BeginChild("##VelaContent", ImVec2(cardW, availH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground)) {
        if (!tab.isHome && !tab.currentPathW.empty()) {
            RegisterDropZone(DropZone::Kind::Viewport, cardMin.x, cardMin.y, cardMax.x, cardMax.y, tab.currentPathW);
        }

        if (tab.scrollToTopPending) {
            ImGui::SetScrollY(0.0f);
            tab.scrollToTopPending = false;
        }

        if (tab.isHome) {
            RenderHomeDashboard(dpiScale);
        } else if (!tab.errorMessage.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.dangerText.ToImU32());
            ImGui::TextWrapped("%s", tab.errorMessage.c_str());
            ImGui::PopStyleColor();
        } else if (tab.isLoading && tab.allEntries.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textSecondary.ToImU32());
            ImGui::TextUnformatted("Loading...");
            ImGui::PopStyleColor();
        } else if (tab.visibleIndices.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textMuted.ToImU32());
            ImGui::TextUnformatted(m_searchBuffer[0] != '\0' ? "No items match your search." : "This folder is empty.");
            ImGui::PopStyleColor();
        } else {
            switch (tab.viewMode) {
                case ViewMode::Grid:    RenderVirtualGrid(tab, dpiScale); break;
                case ViewMode::List:    RenderVirtualList(tab, dpiScale); break;
                case ViewMode::Details: RenderVirtualDetails(tab, dpiScale); break;
            }
        }

        // Background right-click on empty viewport space
        if (!tab.isHome && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
            ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !ImGui::IsAnyItemHovered()) {
            ImGui::OpenPopup("##EmptyContextMenu");
        }

        RenderContextMenu(tab);
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void VelaApp::RenderHomeDashboard(float dpiScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float innerW = ImGui::GetContentRegionAvail().x;
    const float gap = 12.0f * dpiScale;
    const float round = m_cfg.theme.itemRounding * dpiScale;

    auto drawSectionTitle = [&](const char* title) {
        ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textPrimary.ToImU32());
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 8.0f * dpiScale));
    };

    // 1. Pinned Cards
    if (!m_places.pinned.empty()) {
        drawSectionTitle("Quick Access");
        const float cardW = 190.0f * dpiScale;
        const float cardH = 56.0f * dpiScale;
        const int cols = (std::max)(1, static_cast<int>((innerW + gap) / (cardW + gap)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        int idx = 0;

        for (size_t i = 0; i < m_places.pinned.size(); ++i) {
            const auto& item = m_places.pinned[i];
            const int r = idx / cols;
            const int c = idx % cols;
            const ImVec2 cMin(origin.x + c * (cardW + gap), origin.y + r * (cardH + gap));
            const ImVec2 cMax(cMin.x + cardW, cMin.y + cardH);

            ImGui::SetCursorScreenPos(cMin);
            char id[32];
            std::snprintf(id, sizeof(id), "##HomePin%zu", i);
            if (ImGui::InvisibleButton(id, ImVec2(cardW, cardH))) {
                if (item.isSpecialShell && m_window) m_window->OpenShellItem(item.pathW);
                else NavigateTo(NavTarget{false, item.pathW});
            }
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(cMin, cMax, hovered ? m_cfg.theme.selectionBg.ToImU32() : m_cfg.theme.inputBg.ToImU32(), round);
            dl->AddRect(cMin, cMax, m_cfg.theme.inputBorder.ToImU32(), round);

            const char* glyph = icons::Folder;
            if (item.icon == "desktop") glyph = icons::Desktop;
            else if (item.icon == "downloads") glyph = icons::Download;
            else if (item.icon == "documents") glyph = icons::Document;
            else if (item.icon == "music") glyph = icons::Music;
            else if (item.icon == "trash") glyph = icons::Delete;

            dl->AddText(ImVec2(cMin.x + 14.0f * dpiScale, cMin.y + (cardH - ImGui::GetFontSize()) * 0.5f),
                        m_cfg.theme.accent.ToImU32(), glyph);
            dl->AddText(ImVec2(cMin.x + 40.0f * dpiScale, cMin.y + (cardH - ImGui::GetFontSize()) * 0.5f),
                        m_cfg.theme.textPrimary.ToImU32(), item.name.c_str());
            ++idx;
        }
        const int rows = (idx + cols - 1) / cols;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (cardH + gap) + 14.0f * dpiScale));
    }

    // 2. Drives with Storage Bars
    if (!m_places.drives.empty()) {
        drawSectionTitle("Drives");
        const float cardW = 240.0f * dpiScale;
        const float cardH = 74.0f * dpiScale;
        const int cols = (std::max)(1, static_cast<int>((innerW + gap) / (cardW + gap)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        for (int i = 0; i < static_cast<int>(m_places.drives.size()); ++i) {
            const auto& drv = m_places.drives[i];
            const int r = i / cols;
            const int c = i % cols;
            const ImVec2 cMin(origin.x + c * (cardW + gap), origin.y + r * (cardH + gap));
            const ImVec2 cMax(cMin.x + cardW, cMin.y + cardH);

            ImGui::SetCursorScreenPos(cMin);
            char id[32];
            std::snprintf(id, sizeof(id), "##HomeDrv%d", i);
            if (ImGui::InvisibleButton(id, ImVec2(cardW, cardH))) {
                NavigateTo(NavTarget{false, drv.rootPathW});
            }
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(cMin, cMax, hovered ? m_cfg.theme.selectionBg.ToImU32() : m_cfg.theme.inputBg.ToImU32(), round);
            dl->AddRect(cMin, cMax, m_cfg.theme.inputBorder.ToImU32(), round);

            dl->AddText(ImVec2(cMin.x + 14.0f * dpiScale, cMin.y + 14.0f * dpiScale),
                        m_cfg.theme.accent.ToImU32(), icons::Drive);
            dl->AddText(ImVec2(cMin.x + 40.0f * dpiScale, cMin.y + 12.0f * dpiScale),
                        m_cfg.theme.textPrimary.ToImU32(), drv.displayName.c_str());

            if (drv.totalBytes > 0) {
                const float usedRatio = std::clamp(
                    static_cast<float>(drv.totalBytes - drv.freeBytes) / static_cast<float>(drv.totalBytes),
                    0.0f, 1.0f
                );
                const ImVec2 barMin(cMin.x + 40.0f * dpiScale, cMin.y + 36.0f * dpiScale);
                const ImVec2 barMax(cMax.x - 14.0f * dpiScale, barMin.y + 5.0f * dpiScale);
                dl->AddRectFilled(barMin, barMax, m_cfg.theme.progressTrackBg.ToImU32(), 3.0f * dpiScale);
                const uint32_t barCol = (usedRatio > 0.9f) ? m_cfg.theme.danger.ToImU32() : m_cfg.theme.accent.ToImU32();
                dl->AddRectFilled(barMin, ImVec2(barMin.x + (barMax.x - barMin.x) * usedRatio, barMax.y), barCol, 3.0f * dpiScale);

                dl->AddText(ImVec2(cMin.x + 40.0f * dpiScale, cMin.y + 46.0f * dpiScale),
                            m_cfg.theme.textSecondary.ToImU32(), drv.capacityText.c_str());
            }
        }
        const int rows = (static_cast<int>(m_places.drives.size()) + cols - 1) / cols;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + rows * (cardH + gap) + 14.0f * dpiScale));
    }

    // 3. WSL Distributions
    if (!m_places.wsl.empty()) {
        drawSectionTitle("WSL Distributions");
        const float cardW = 190.0f * dpiScale;
        const float cardH = 52.0f * dpiScale;
        const int cols = (std::max)(1, static_cast<int>((innerW + gap) / (cardW + gap)));
        const ImVec2 origin = ImGui::GetCursorScreenPos();

        for (int i = 0; i < static_cast<int>(m_places.wsl.size()); ++i) {
            const auto& wsl = m_places.wsl[i];
            const int r = i / cols;
            const int c = i % cols;
            const ImVec2 cMin(origin.x + c * (cardW + gap), origin.y + r * (cardH + gap));
            const ImVec2 cMax(cMin.x + cardW, cMin.y + cardH);

            ImGui::SetCursorScreenPos(cMin);
            char id[32];
            std::snprintf(id, sizeof(id), "##HomeWsl%d", i);
            if (ImGui::InvisibleButton(id, ImVec2(cardW, cardH))) {
                NavigateTo(NavTarget{false, wsl.pathW});
            }
            const bool hovered = ImGui::IsItemHovered();
            dl->AddRectFilled(cMin, cMax, hovered ? m_cfg.theme.selectionBg.ToImU32() : m_cfg.theme.inputBg.ToImU32(), round);
            dl->AddRect(cMin, cMax, m_cfg.theme.inputBorder.ToImU32(), round);

            dl->AddText(ImVec2(cMin.x + 14.0f * dpiScale, cMin.y + (cardH - ImGui::GetFontSize()) * 0.5f),
                        m_cfg.theme.accent.ToImU32(), icons::Terminal);
            dl->AddText(ImVec2(cMin.x + 40.0f * dpiScale, cMin.y + (cardH - ImGui::GetFontSize()) * 0.5f),
                        m_cfg.theme.textPrimary.ToImU32(), wsl.name.c_str());
        }
    }
}

void VelaApp::RenderVirtualGrid(TabState& tab, float dpiScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winMin = ImGui::GetWindowPos();
    const ImVec2 winMax(winMin.x + ImGui::GetWindowSize().x, winMin.y + ImGui::GetWindowSize().y);

    const float innerW = ImGui::GetContentRegionAvail().x;
    const float tileW = m_cfg.layout.gridTileWidth * dpiScale;
    const float iconBox = m_cfg.layout.gridIconSize * dpiScale;
    const float lineH = ImGui::GetTextLineHeight();
    const float tileH = iconBox + 26.0f * dpiScale + (lineH * 2.0f);
    const float gapX = m_cfg.layout.gridGap * dpiScale;
    const float gapY = m_cfg.layout.gridGap * dpiScale;

    const int cols = (std::max)(1, static_cast<int>((innerW + gapX) / (tileW + gapX)));
    const int totalItems = static_cast<int>(tab.visibleIndices.size());
    const int totalRows = (totalItems + cols - 1) / cols;
    const float rowStride = tileH + gapY;
    const int iconRequestPx = static_cast<int>(std::round(iconBox * 1.25f));
    const uint32_t cutTint = MakeCutTint(m_cfg.theme.cutItemAlpha);

    ImGuiListClipper clipper;
    clipper.Begin(totalRows, rowStride);

    while (clipper.Step()) {
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const ImVec2 rowOrigin = ImGui::GetCursorScreenPos();
            for (int col = 0; col < cols; ++col) {
                const int visibleIdx = row * cols + col;
                if (visibleIdx >= totalItems) break;

                const size_t allIdx = tab.visibleIndices[visibleIdx];
                const FileEntry& entry = tab.allEntries[allIdx];

                const bool isCut = m_cutPathsLower.contains(str::ToLowerAscii(entry.fullPath));
                const uint32_t thumbTint = isCut ? cutTint : IM_COL32_WHITE;

                const ImVec2 tMin(rowOrigin.x + col * (tileW + gapX), rowOrigin.y);
                const ImVec2 tMax(tMin.x + tileW, tMin.y + tileH);

                ImGui::SetCursorScreenPos(tMin);
                char id[32];
                std::snprintf(id, sizeof(id), "##Grid%d", visibleIdx);
                ImGui::InvisibleButton(id, ImVec2(tileW, tileH));

                HandleItemMouseInteraction(tab, visibleIdx, allIdx, entry, dpiScale);
                if (entry.isDir) {
                    const float cMinY = (std::max)(tMin.y, winMin.y);
                    const float cMaxY = (std::min)(tMax.y, winMax.y);
                    if (cMaxY > cMinY) {
                        RegisterDropZone(DropZone::Kind::FolderEntry, tMin.x, cMinY, tMax.x, cMaxY, entry.fullPathW);
                    }
                }

                const bool hovered = ImGui::IsItemHovered();
                const bool selected = tab.selectedAllIndices.contains(allIdx);
                const float itemRound = m_cfg.theme.itemRounding * dpiScale;

                if (selected) {
                    dl->AddRectFilled(tMin, tMax, m_cfg.theme.selectionBg.ToImU32(), itemRound);
                    dl->AddRect(tMin, tMax, m_cfg.theme.accent.ToImU32(), itemRound, 0, 1.5f * dpiScale);
                } else if (hovered) {
                    dl->AddRectFilled(tMin, tMax, m_cfg.theme.hoverBg.ToImU32(), itemRound);
                }

                // Native Windows Shell Icon / Thumbnail
                const ImVec2 boxMin(tMin.x + (tileW - iconBox) * 0.5f, tMin.y + 10.0f * dpiScale);
                const ImVec2 boxMax(boxMin.x + iconBox, boxMin.y + iconBox);

                TextureView tv;
                if (m_thumbCache) {
                    tv = m_thumbCache->GetOrRequest(entry.thumbKey, entry.fullPathW, iconRequestPx, tab.expectedGeneration);
                }
                if (tv.srv) {
                    DrawAspectImage(dl, tv.srv, tv.width, tv.height, boxMin, boxMax, thumbTint);
                } else {
                    const char* fallback = entry.isDir ? icons::Folder : icons::FilePage;
                    const ImVec2 fbTs = ImGui::CalcTextSize(fallback);
                    dl->AddText(
                        ImVec2(boxMin.x + (iconBox - fbTs.x) * 0.5f, boxMin.y + (iconBox - fbTs.y) * 0.5f),
                        m_cfg.theme.textMuted.ToImU32(), fallback
                    );
                }

                // Two-line UTF-8 wrapped label with clean ellipsis
                const std::string& label = m_cfg.layout.showExtensions ? entry.name : entry.displayName;
                const float maxTextW = tileW - 14.0f * dpiScale;
                std::string line1, line2;
                bool truncated = false;
                SplitTwoLinesEllipsis(label, maxTextW, line1, line2, truncated);

                uint32_t textCol = entry.isHidden
                    ? m_cfg.theme.textSecondary.ToImU32()
                    : m_cfg.theme.textPrimary.ToImU32();
                if (isCut) textCol = m_cfg.theme.textMuted.ToImU32();

                const float textY1 = boxMax.y + 4.0f * dpiScale;

                if (!line1.empty()) {
                    const float w1 = ImGui::CalcTextSize(line1.c_str()).x;
                    dl->AddText(ImVec2(tMin.x + (tileW - w1) * 0.5f, textY1), textCol, line1.c_str());
                }
                if (!line2.empty()) {
                    const float w2 = ImGui::CalcTextSize(line2.c_str()).x;
                    dl->AddText(ImVec2(tMin.x + (tileW - w2) * 0.5f, textY1 + lineH), textCol, line2.c_str());
                }
                if (hovered && truncated) {
                    ImGui::SetTooltip("%s", entry.name.c_str());
                }
            }
            // Advance row cleanly
            ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x, rowOrigin.y + rowStride));
            ImGui::Dummy(ImVec2(innerW, 0.0f));
        }
    }
}

void VelaApp::RenderVirtualList(TabState& tab, float dpiScale) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 winMin = ImGui::GetWindowPos();
    const ImVec2 winMax(winMin.x + ImGui::GetWindowSize().x, winMin.y + ImGui::GetWindowSize().y);

    const float rowH = m_cfg.layout.listRowHeight * dpiScale;
    const float iconSize = m_cfg.layout.listIconSize * dpiScale;
    const float rowW = ImGui::GetContentRegionAvail().x;
    const int iconPx = static_cast<int>(std::round(iconSize * 1.15f));
    const uint32_t cutTint = MakeCutTint(m_cfg.theme.cutItemAlpha);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(tab.visibleIndices.size()), rowH);

    while (clipper.Step()) {
        for (int visibleIdx = clipper.DisplayStart; visibleIdx < clipper.DisplayEnd; ++visibleIdx) {
            const size_t allIdx = tab.visibleIndices[visibleIdx];
            const FileEntry& entry = tab.allEntries[allIdx];
            const bool isCut = m_cutPathsLower.contains(str::ToLowerAscii(entry.fullPath));
            const uint32_t thumbTint = isCut ? cutTint : IM_COL32_WHITE;

            const ImVec2 pos = ImGui::GetCursorScreenPos();
            const ImVec2 rMax(pos.x + rowW, pos.y + rowH);

            char id[32];
            std::snprintf(id, sizeof(id), "##List%d", visibleIdx);
            ImGui::InvisibleButton(id, ImVec2(rowW, rowH));

            HandleItemMouseInteraction(tab, visibleIdx, allIdx, entry, dpiScale);
            if (entry.isDir) {
                const float cMinY = (std::max)(pos.y, winMin.y);
                const float cMaxY = (std::min)(rMax.y, winMax.y);
                if (cMaxY > cMinY) {
                    RegisterDropZone(DropZone::Kind::FolderEntry, pos.x, cMinY, rMax.x, cMaxY, entry.fullPathW);
                }
            }

            const bool selected = tab.selectedAllIndices.contains(allIdx);
            const bool hovered = ImGui::IsItemHovered();
            const float round = m_cfg.theme.itemRounding * dpiScale;

            if (selected) {
                dl->AddRectFilled(pos, rMax, m_cfg.theme.selectionBg.ToImU32(), round);
                dl->AddRect(pos, rMax, m_cfg.theme.accent.ToImU32(), round);
            } else if (hovered) {
                dl->AddRectFilled(pos, rMax, m_cfg.theme.hoverBg.ToImU32(), round);
            }

            const ImVec2 iconMin(pos.x + 10.0f * dpiScale, pos.y + (rowH - iconSize) * 0.5f);
            const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            if (m_thumbCache) {
                const TextureView tv = m_thumbCache->GetOrRequest(entry.thumbKey, entry.fullPathW, iconPx, tab.expectedGeneration);
                if (tv.srv) {
                    DrawAspectImage(dl, tv.srv, tv.width, tv.height, iconMin, iconMax, thumbTint);
                }
            }

            uint32_t textCol = entry.isHidden
                ? m_cfg.theme.textSecondary.ToImU32()
                : m_cfg.theme.textPrimary.ToImU32();
            if (isCut) textCol = m_cfg.theme.textMuted.ToImU32();

            const std::string& label = m_cfg.layout.showExtensions ? entry.name : entry.displayName;
            dl->AddText(
                ImVec2(iconMax.x + 10.0f * dpiScale, pos.y + (rowH - ImGui::GetFontSize()) * 0.5f),
                textCol,
                label.c_str()
            );
        }
    }
}

void VelaApp::RenderVirtualDetails(TabState& tab, float dpiScale) {
    const bool showDate = m_cfg.layout.detailsShowDate;
    const bool showType = m_cfg.layout.detailsShowType;
    const bool showSize = m_cfg.layout.detailsShowSize;
    const int colCount = 1 + (showDate ? 1 : 0) + (showType ? 1 : 0) + (showSize ? 1 : 0);

    char tableId[32];
    std::snprintf(tableId, sizeof(tableId), "##VelaDetails_%d%d%d", showDate ? 1 : 0, showType ? 1 : 0, showSize ? 1 : 0);

    constexpr ImGuiTableFlags tableFlags =
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_Sortable |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;

    if (!ImGui::BeginTable(tableId, colCount, tableFlags)) return;

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",          ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.45f, 0);
    if (showDate) ImGui::TableSetupColumn("Date modified", ImGuiTableColumnFlags_WidthStretch, 0.22f, 1);
    if (showType) ImGui::TableSetupColumn("Type",          ImGuiTableColumnFlags_WidthStretch, 0.20f, 2);
    if (showSize) ImGui::TableSetupColumn("Size",          ImGuiTableColumnFlags_WidthStretch, 0.13f, 3);
    ImGui::TableHeadersRow();

    if (ImGuiTableSortSpecs* specs = ImGui::TableGetSortSpecs()) {
        if (specs->SpecsDirty && specs->SpecsCount > 0) {
            const ImGuiTableColumnSortSpecs& s = specs->Specs[0];
            switch (s.ColumnUserID) {
                case 0: tab.sortField = SortField::Name; break;
                case 1: tab.sortField = SortField::DateModified; break;
                case 2: tab.sortField = SortField::Type; break;
                case 3: tab.sortField = SortField::Size; break;
                default: break;
            }
            tab.sortOrder = (s.SortDirection == ImGuiSortDirection_Descending)
                ? SortOrder::Descending : SortOrder::Ascending;
            SortAndFilterTab(tab);
            specs->SpecsDirty = false;
        }
    }

    const float rowH = m_cfg.layout.detailsRowHeight * dpiScale;
    const float iconSize = m_cfg.layout.detailsIconSize * dpiScale;
    const int iconPx = static_cast<int>(std::round(iconSize * 1.15f));
    const uint32_t cutTint = MakeCutTint(m_cfg.theme.cutItemAlpha);

    const ImVec2 winMin = ImGui::GetWindowPos();
    const ImVec2 winMax(winMin.x + ImGui::GetWindowSize().x, winMin.y + ImGui::GetWindowSize().y);

    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(tab.visibleIndices.size()), rowH);

    while (clipper.Step()) {
        for (int visibleIdx = clipper.DisplayStart; visibleIdx < clipper.DisplayEnd; ++visibleIdx) {
            const size_t allIdx = tab.visibleIndices[visibleIdx];
            const FileEntry& entry = tab.allEntries[allIdx];
            const bool selected = tab.selectedAllIndices.contains(allIdx);
            const bool isCut = m_cutPathsLower.contains(str::ToLowerAscii(entry.fullPath));
            const uint32_t thumbTint = isCut ? cutTint : IM_COL32_WHITE;

            ImGui::TableNextRow(ImGuiTableRowFlags_None, rowH);
            ImGui::TableNextColumn();

            char id[32];
            std::snprintf(id, sizeof(id), "##Det%d", visibleIdx);

            const ImVec2 cellPos = ImGui::GetCursorScreenPos();
            ImGui::Selectable(id, selected,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowDoubleClick,
                ImVec2(0.0f, rowH));
            HandleItemMouseInteraction(tab, visibleIdx, allIdx, entry, dpiScale);
            if (entry.isDir) {
                const ImVec2 rMin = ImGui::GetItemRectMin();
                const ImVec2 rMax = ImGui::GetItemRectMax();
                const float cMinY = (std::max)(rMin.y, winMin.y);
                const float cMaxY = (std::min)(rMax.y, winMax.y);
                if (cMaxY > cMinY) {
                    RegisterDropZone(DropZone::Kind::FolderEntry, rMin.x, cMinY, rMax.x, cMaxY, entry.fullPathW);
                }
            }

            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 iconMin(cellPos.x + 4.0f * dpiScale, cellPos.y + (rowH - iconSize) * 0.5f);
            const ImVec2 iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            if (m_thumbCache) {
                const TextureView tv = m_thumbCache->GetOrRequest(entry.thumbKey, entry.fullPathW, iconPx, tab.expectedGeneration);
                if (tv.srv) {
                    DrawAspectImage(dl, tv.srv, tv.width, tv.height, iconMin, iconMax, thumbTint);
                }
            }

            uint32_t textCol = entry.isHidden
                ? m_cfg.theme.textSecondary.ToImU32()
                : m_cfg.theme.textPrimary.ToImU32();
            if (isCut) textCol = m_cfg.theme.textMuted.ToImU32();

            const std::string& label = m_cfg.layout.showExtensions ? entry.name : entry.displayName;
            dl->AddText(
                ImVec2(iconMax.x + 8.0f * dpiScale, cellPos.y + (rowH - ImGui::GetFontSize()) * 0.5f),
                textCol,
                label.c_str()
            );

            ImGui::PushStyleColor(ImGuiCol_Text, m_cfg.theme.textSecondary.ToImU32());
            if (showDate) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.dateFormatted.c_str());
            }
            if (showType) {
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry.typeLabel.c_str());
            }
            if (showSize) {
                ImGui::TableNextColumn();
                if (!entry.isDir) {
                    ImGui::TextUnformatted(entry.sizeFormatted.c_str());
                }
            }
            ImGui::PopStyleColor();
        }
    }

    ImGui::EndTable();
}

void VelaApp::RenderContextMenu(TabState& tab) {
    if (ImGui::BeginPopup("##ItemContextMenu")) {
        const size_t selCount = tab.selectedAllIndices.size();
        const FileEntry* primary = nullptr;
        if (selCount == 1) {
            const size_t idx = *tab.selectedAllIndices.begin();
            if (idx < tab.allEntries.size()) primary = &tab.allEntries[idx];
        }

        if (primary) {
            if (ImGui::MenuItem("Open")) {
                ActivateEntry(tab, *tab.selectedAllIndices.begin());
            }
            if (primary->isDir && ImGui::MenuItem("Open in new tab")) {
                OpenNewTab(NavTarget{false, primary->fullPathW}, true);
            }
            ImGui::Separator();
        }

        if (ImGui::MenuItem("Cut", m_cfg.keybinds.cut.c_str())) {
            CopySelectedFilesToClipboard(true);
        }
        if (ImGui::MenuItem("Copy", m_cfg.keybinds.copy.c_str())) {
            CopySelectedFilesToClipboard(false);
        }
        if (primary && primary->isDir &&
            ImGui::MenuItem("Paste into folder", nullptr, false, OleDragDropHost::HasClipboardFiles())) {
            PasteClipboardFiles(primary->fullPathW);
        }
        ImGui::Separator();

        if (selCount == 1 && ImGui::MenuItem("Rename", m_cfg.keybinds.rename.c_str())) {
            PromptRenameSelected();
        }
        if (ImGui::MenuItem("Delete (Recycle Bin)", m_cfg.keybinds.deleteItem.c_str())) {
            PromptDeleteSelected(false);
        }
        if (ImGui::MenuItem("Delete Permanently", m_cfg.keybinds.deletePermanent.c_str())) {
            PromptDeleteSelected(true);
        }
        ImGui::Separator();

        if (ImGui::MenuItem("Copy Path", m_cfg.keybinds.copyPath.c_str())) {
            CopySelectedPaths(CopyPathFormat::Windows);
        }
        if (ImGui::BeginMenu("Copy Path as...")) {
            if (ImGui::MenuItem("Windows Path (\\)", m_cfg.keybinds.copyPath.c_str())) CopySelectedPaths(CopyPathFormat::Windows);
            if (ImGui::MenuItem("POSIX Path (/)", m_cfg.keybinds.copyPathPosix.c_str())) CopySelectedPaths(CopyPathFormat::Posix);
            if (ImGui::MenuItem("Quoted Path (\"...\")", m_cfg.keybinds.copyPathQuoted.c_str())) CopySelectedPaths(CopyPathFormat::Quoted);
            if (ImGui::MenuItem("File Name Only")) CopySelectedPaths(CopyPathFormat::Filename);
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Open in Terminal", m_cfg.keybinds.openTerminal.c_str())) {
            OpenSelectedInTerminal();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Properties", m_cfg.keybinds.properties.c_str())) {
            OpenSelectedProperties();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##EmptyContextMenu")) {
        if (ImGui::MenuItem("New Folder", m_cfg.keybinds.newFolder.c_str())) {
            PromptNewItem(true);
        }
        if (ImGui::MenuItem("New Text File")) {
            PromptNewItem(false);
        }
        if (ImGui::MenuItem("Paste", m_cfg.keybinds.paste.c_str(), false, OleDragDropHost::HasClipboardFiles())) {
            PasteClipboardFiles();
        }
        ImGui::Separator();

        if (ImGui::MenuItem("Refresh", m_cfg.keybinds.refresh.c_str())) {
            RefreshCurrentTab();
        }
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Grid", nullptr, tab.viewMode == ViewMode::Grid)) tab.viewMode = ViewMode::Grid;
            if (ImGui::MenuItem("List", nullptr, tab.viewMode == ViewMode::List)) tab.viewMode = ViewMode::List;
            if (ImGui::MenuItem("Details", nullptr, tab.viewMode == ViewMode::Details)) tab.viewMode = ViewMode::Details;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Show hidden items", m_cfg.keybinds.toggleHidden.c_str(), tab.showHidden)) {
            tab.showHidden = !tab.showHidden;
            RebuildVisibleIndices(tab);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Copy Folder Path", m_cfg.keybinds.copyPath.c_str())) {
            ClearSelection(tab);
            CopySelectedPaths(CopyPathFormat::Windows);
        }
        if (ImGui::MenuItem("Copy Folder POSIX Path", m_cfg.keybinds.copyPathPosix.c_str())) {
            ClearSelection(tab);
            CopySelectedPaths(CopyPathFormat::Posix);
        }
        if (ImGui::MenuItem("Open in Terminal", m_cfg.keybinds.openTerminal.c_str())) {
            OpenSelectedInTerminal();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Properties", m_cfg.keybinds.properties.c_str())) {
            ClearSelection(tab);
            OpenSelectedProperties();
        }
        ImGui::EndPopup();
    }
}

void VelaApp::RenderStatusBar(float dpiScale) {
    if (m_activeTab < 0 || m_activeTab >= static_cast<int>(m_tabs.size())) return;
    const TabState& tab = m_tabs[m_activeTab];

    const float h = m_cfg.layout.statusbarHeight * dpiScale;
    const float w = ImGui::GetIO().DisplaySize.x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool sidebarLeft = m_cfg.layout.showSidebar && (m_cfg.layout.sidebarPosition != "right");
    const float leftX = sidebarLeft ? (m_cfg.layout.sidebarWidth * dpiScale) : (16.0f * dpiScale);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (m_cfg.theme.statusbarBg.a > 0.0f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), m_cfg.theme.statusbarBg.ToImU32());
    }

    char buf[160];
    if (tab.isHome) {
        std::snprintf(buf, sizeof(buf), "%zu drives    %zu pinned",
                      m_places.drives.size(), m_places.pinned.size());
    } else if (tab.selectedAllIndices.empty()) {
        std::snprintf(buf, sizeof(buf), "%zu items", tab.visibleIndices.size());
    } else if (tab.selectedAllIndices.size() == 1) {
        const size_t idx = *tab.selectedAllIndices.begin();
        if (idx < tab.allEntries.size() && !tab.allEntries[idx].isDir) {
            std::snprintf(buf, sizeof(buf), "%zu items    1 item selected (%s)",
                          tab.visibleIndices.size(), tab.allEntries[idx].sizeFormatted.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%zu items    1 item selected", tab.visibleIndices.size());
        }
    } else {
        uint64_t totalBytes = 0;
        for (size_t idx : tab.selectedAllIndices) {
            if (idx < tab.allEntries.size() && !tab.allEntries[idx].isDir) {
                totalBytes += tab.allEntries[idx].sizeBytes;
            }
        }
        if (totalBytes > 0) {
            const std::string szText = str::FormatFileSize(totalBytes);
            std::snprintf(buf, sizeof(buf), "%zu items    %zu items selected (%s)",
                          tab.visibleIndices.size(), tab.selectedAllIndices.size(), szText.c_str());
        } else {
            std::snprintf(buf, sizeof(buf), "%zu items    %zu items selected",
                          tab.visibleIndices.size(), tab.selectedAllIndices.size());
        }
    }

    dl->AddText(
        ImVec2(pos.x + leftX, pos.y + (h - ImGui::GetFontSize()) * 0.5f),
        m_cfg.theme.textSecondary.ToImU32(), buf
    );

    const char* hint = "vela.lua live";
    const ImVec2 hintTs = ImGui::CalcTextSize(hint);
    dl->AddText(
        ImVec2(pos.x + w - hintTs.x - 14.0f * dpiScale, pos.y + (h - ImGui::GetFontSize()) * 0.5f),
        m_cfg.theme.textMuted.ToImU32(), hint
    );

    ImGui::Dummy(ImVec2(w, h));
}

void VelaApp::RenderToast(float dpiScale) {
    if (m_toastText.empty()) return;
    if (std::chrono::steady_clock::now() > m_toastUntil) {
        m_toastText.clear();
        return;
    }

    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 disp = ImGui::GetIO().DisplaySize;
    const ImVec2 ts = ImGui::CalcTextSize(m_toastText.c_str());
    const float padX = 14.0f * dpiScale;
    const float padY = 8.0f * dpiScale;
    const float boxW = (std::min)(disp.x - 32.0f * dpiScale, ts.x + padX * 2.0f);
    const float boxH = ts.y + padY * 2.0f;

    const ImVec2 bMin(disp.x - boxW - 18.0f * dpiScale, disp.y - boxH - 38.0f * dpiScale);
    const ImVec2 bMax(bMin.x + boxW, bMin.y + boxH);

    const uint32_t bg = m_toastIsError ? m_cfg.theme.toastErrorBg.ToImU32() : m_cfg.theme.toastBg.ToImU32();
    const uint32_t border = m_toastIsError ? m_cfg.theme.danger.ToImU32() : m_cfg.theme.accent.ToImU32();

    dl->AddRectFilled(bMin, bMax, bg, m_cfg.theme.itemRounding * dpiScale);
    dl->AddRect(bMin, bMax, border, m_cfg.theme.itemRounding * dpiScale);
    dl->PushClipRect(ImVec2(bMin.x + padX * 0.5f, bMin.y), ImVec2(bMax.x - padX * 0.5f, bMax.y), true);
    dl->AddText(ImVec2(bMin.x + padX, bMin.y + padY), m_cfg.theme.textPrimary.ToImU32(), m_toastText.c_str());
    dl->PopClipRect();
}

} // namespace vela