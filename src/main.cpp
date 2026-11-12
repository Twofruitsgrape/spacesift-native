// SpaceSift Native v2 —— C++ 重写版
//
// 和 Python 版对齐的功能：扫描 / 方块图下钻 / 三页签右栏（推荐·深度·检索）/
// 清理规则检测 / 清理执行 / 文件名搜索。区别是它只有一个静态链接的 exe。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <cstring>
#include <mutex>
#include <memory>
#include <map>
#include <set>
#include <unordered_map>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <d3d11.h>
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#include "db.h"
#include "content.h"
#include "extract.h"
#include "rules.h"

// ============================================================ 扫描结果

struct Entry {
    uint32_t nameOff;
    uint32_t nameLen;
    int32_t  parent;
    int32_t  firstChild;
    int32_t  nextSibling;
    uint64_t size;
    uint8_t  isDir;
    uint8_t  depth;
};

static std::vector<char>  g_pool;
static std::vector<Entry> g_entries;

static inline const char* NameOf(int i) { return g_pool.data() + g_entries[i].nameOff; }

static std::string PathOf(int i) {
    if (i < 0) return {};
    std::vector<int> chain;
    for (int a = i; a >= 0; a = g_entries[a].parent) chain.push_back(a);
    std::string out;
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Entry& e = g_entries[*it];
        if (!out.empty() && out.back() != '\\') out.push_back('\\');
        out.append(NameOf(*it), e.nameLen);
    }
    return out;
}

// ============================================================ 实时快照
//
// 扫描线程在不停 push_back g_entries，渲染线程直接读会踩到重分配。
// 所以扫描线程每 100ms 把已扫到的目录树（含 parent 关系）抄进 g_live，
// 渲染线程只读这份拷贝 —— 这样扫描过程中方块图也能逐层下钻。

struct LiveItem {
    std::string name;
    std::string path;
    uint64_t size = 0;        // 在快照内部的树大小（已含子目录）
    uint64_t rawSize = 0;     // g_entries 中的直接文件大小
    int index = -1;           // 对应 g_entries 下标
    int parent = -1;          // 对应 g_entries 下标
    std::vector<int> children; // 在 g_live 内的子项下标
};

static std::mutex            g_liveMutex;
static std::vector<LiveItem> g_live;          // 互斥保护
static std::wstring          g_livePathW;     // 当前正在扫的目录（宽字符）
static std::atomic<long long> g_liveVisited{0};
static double                g_liveAnim[64] = {0};   // 每块的动画进度
static std::string           g_liveNames[64];        // 上一帧的名字，用于对齐动画

static std::atomic<bool>  g_scanning{false};
static std::atomic<bool>  g_stopReq{false};
static std::atomic<bool>  g_paused{false};
static std::atomic<long long> g_visited{0};
static std::atomic<long long> g_fileCount{0};
static std::atomic<unsigned long long> g_totalBytes{0};
static std::atomic<unsigned long long> g_volumeUsed{0};   // 整盘已用字节，做进度分母
static std::atomic<unsigned long long> g_volumeTotal{0};
static std::wstring       g_currentPathW;
static std::mutex         g_currentMutex;
static double             g_scanSeconds = 0;
static std::string        g_indexMsg = "索引未建立";

static std::vector<rules::Rule>    g_rules;    // 规则表本体，只在 WinMain 里构造一次
static std::vector<rules::Finding> g_findings;  // Finding.rule 指向 g_rules 里的元素
static std::mutex                  g_findMutex; // 检测线程写、UI 线程读
static std::atomic<bool> g_detecting{false};
static std::string       g_detectMsg;
static std::mutex        g_msgMutex;            // 保护 g_detectMsg（检测线程写、UI 读）
static std::atomic<int>  g_detectGen{0};        // 检测代号：新扫描开始后旧结果作废
static std::mutex        g_detectJobMutex;
static std::shared_ptr<std::atomic<bool>> g_detectJob;  // 当前检测的取消标志
static bool              g_detectStarted = false;

// 清理在后台线程里做，避免删几十万文件时界面假死
static std::atomic<bool> g_cleaning{false};
static std::atomic<int>  g_cleanDone{0};
static std::atomic<int>  g_cleanTotal{0};
static std::atomic<int>  g_cleanErrors{0};
static std::atomic<unsigned long long> g_cleanFreed{0};
static std::atomic<bool> g_cleanFinish{false};

static std::vector<db::Hit> g_hits;
static std::string          g_searchQuery;
// 检索模式：0 = 文件名（走 index_*.db），1 = 文件内容（走 content_*.db）
static int                 g_searchMode = 0;
static std::atomic<bool>   g_contentBusy{false};   // UI 侧镜像，防止重复起线程
static std::atomic<bool>   g_contentCancel{false};
static std::set<std::string> g_extFilter;   // 后缀筛选（空 = 全部）
static std::vector<content::ExtCount> g_extList;   // 弹窗里的后缀列表（缓存）
static bool g_extListDirty = true;
static bool g_extPopOnce = false;   // 测试钩子：启动就打开后缀弹窗

static int  g_current = -1;
static char g_pathBuf[512] = "C:\\";
static char g_searchBuf[256] = "";
static int  g_mode = 0;
static std::set<std::string> g_selected;        // 推荐清理：按规则 id
static std::map<std::string, uint64_t> g_deepSelected;   // 深度清理：路径 -> 大小
static std::map<std::string, uint64_t> g_hitSelected;    // 检索结果：路径 -> 大小
static std::string g_lastCleanMsg;
static int  g_expandRestParent = -1;   // 点击「（其余 N 项）」后展开这个父目录的全部子项

// ============================================================ UI 主题
//
// 参考了 GitHub 上的 ImThemes / dear-imgui-styles / FluentImGui：
// 不引入第三方依赖，只做「语义化颜色 token + 两套完整调色板」。
struct UiTheme {
    bool dark = true;
    ImVec4 windowBg, childBg, cardBg, cardHover, cardSelected;
    ImVec4 border, borderStrong, inputBg, tabBg;
    ImVec4 text, textDim, textFaint;
    ImVec4 accent, accentHover, accentText, brand;
    ImVec4 success, warning, danger;
    ImVec4 canvas, titleOverlay, sizeOverlay, blockText, blockSubText, blockBorder, fileBlock;
    ImVec4 dimBg;
};
static UiTheme g_ui;
static bool     g_darkTheme = true;
static ImFont*  g_fontSmall = nullptr;
static HWND     g_mainHwnd = nullptr;

static ImVec4 RGBA(int r, int g, int b, float a = 1.0f) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
static ImU32 ThemeColor(const ImVec4& c) { return ImGui::GetColorU32(c); }
static bool InCurrentDir(const std::string& path);   // 定义在右栏卡片区，前面清理要用
static std::string g_pendingScan;     // 右键「从这里开始扫描」选中的路径
static std::string g_rootPath = "C:\\";
static std::string g_ctxPath;         // 当前右键菜单目标路径
static bool        g_ctxRequested = false; // 本帧是否有右键请求
// 前置声明：这两个在文件后半段才定义
static std::vector<std::string> ListDrives();
static void StartScan(const std::string& root);

static int  g_driveIdx = -1;
static std::vector<std::string> g_drives = ListDrives();

static std::string FormatBytes(uint64_t v) {
    char buf[48];
    if (v >= 1024ull * 1024 * 1024) snprintf(buf, sizeof buf, "%.1f GB", v / 1073741824.0);
    else if (v >= 1024ull * 1024)   snprintf(buf, sizeof buf, "%.1f MB", v / 1048576.0);
    else if (v >= 1024ull)          snprintf(buf, sizeof buf, "%.0f KB", v / 1024.0);
    else                            snprintf(buf, sizeof buf, "%llu B", (unsigned long long)v);
    return buf;
}

// ============================================================ 系统小工具

// 全程 UTF-8。之前用 CP_ACP(GBK) 转换，GBK 表示不了的文件名（日文、emoji、
// 部分符号）会被写成 '?'，方块图和状态栏里就出现一堆 "????"。
static std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string WideToUtf8(const std::wstring& w) { return WideToUtf8(w.c_str()); }

static std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::vector<std::string> ListDrives() {
    std::vector<std::string> out;
    for (char c = 'A'; c <= 'Z'; ++c) {
        std::string root = std::string(1, c) + ":\\";
        if (GetDriveTypeA(root.c_str()) != DRIVE_NO_ROOT_DIR) out.push_back(root);
    }
    return out;
}

static bool BrowseFolder(std::string& out) {
    BROWSEINFOW bi{};
    bi.lpszTitle = L"选择要扫描的文件夹";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path[MAX_PATH] = {0};
    bool ok = SHGetPathFromIDListW(pidl, path) != FALSE;
    CoTaskMemFree(pidl);
    if (!ok) return false;
    out = WideToUtf8(path);
    return true;
}

// ============================================================ 主题实现

static UiTheme MakeTheme(bool dark) {
    UiTheme t;
    t.dark = dark;
    if (dark) {
        t.windowBg      = RGBA(32, 33, 36);
        t.childBg       = RGBA(41, 42, 45);
        t.cardBg        = RGBA(41, 42, 45);
        t.cardHover     = RGBA(53, 54, 58);
        t.cardSelected  = RGBA(46, 58, 78);
        t.border        = RGBA(60, 64, 67);
        t.borderStrong  = RGBA(95, 99, 104);
        t.inputBg       = RGBA(41, 42, 45);
        t.tabBg         = RGBA(53, 54, 58);
        t.text          = RGBA(232, 234, 237);
        t.textDim       = RGBA(154, 160, 166);
        t.textFaint     = RGBA(128, 134, 139);
        t.accent        = RGBA(138, 180, 248);
        t.accentHover   = RGBA(174, 203, 250);
        t.accentText    = RGBA(32, 33, 36);
        t.brand         = RGBA(26, 115, 232);
        t.success       = RGBA(129, 201, 149);
        t.warning       = RGBA(253, 214, 99);
        t.danger        = RGBA(242, 139, 130);
        t.canvas        = RGBA(23, 24, 26);
        t.titleOverlay  = RGBA(0, 0, 0, 0.55f);
        t.sizeOverlay   = RGBA(0, 0, 0, 0.45f);
        t.blockText     = RGBA(232, 234, 237);
        t.blockSubText  = RGBA(189, 193, 198);
        t.blockBorder   = RGBA(0, 0, 0, 0.50f);
        t.fileBlock     = RGBA(60, 64, 67);
        t.dimBg         = RGBA(0, 0, 0, 0.40f);
    } else {
        t.windowBg      = RGBA(255, 255, 255);
        t.childBg       = RGBA(255, 255, 255);
        t.cardBg        = RGBA(255, 255, 255);
        t.cardHover     = RGBA(241, 243, 244);
        t.cardSelected  = RGBA(232, 240, 254);
        t.border        = RGBA(218, 220, 224);
        t.borderStrong  = RGBA(189, 193, 198);
        t.inputBg       = RGBA(241, 243, 244);
        t.tabBg         = RGBA(241, 243, 244);
        t.text          = RGBA(32, 33, 36);
        t.textDim       = RGBA(95, 99, 104);
        t.textFaint     = RGBA(128, 134, 139);
        t.accent        = RGBA(26, 115, 232);
        t.accentHover   = RGBA(23, 101, 204);
        t.accentText    = RGBA(255, 255, 255);
        t.brand         = RGBA(26, 115, 232);
        t.success       = RGBA(24, 128, 56);
        t.warning       = RGBA(227, 116, 0);
        t.danger        = RGBA(217, 48, 37);
        t.canvas        = RGBA(241, 243, 244);
        t.titleOverlay  = RGBA(255, 255, 255, 0.78f);
        t.sizeOverlay   = RGBA(255, 255, 255, 0.62f);
        t.blockText     = RGBA(32, 33, 36);
        t.blockSubText  = RGBA(95, 99, 104);
        t.blockBorder   = RGBA(0, 0, 0, 0.08f);
        t.fileBlock     = RGBA(218, 220, 224);
        t.dimBg         = RGBA(32, 33, 36, 0.32f);
    }
    return t;
}

static void ApplyDarkTitleBar() {
    if (!g_mainHwnd) return;
    BOOL dark = g_darkTheme ? TRUE : FALSE;
    // Win11 / Win10 20H1 用 20，老版本用 19；两个都写一遍，避免某些版本静默忽略
    DwmSetWindowAttribute(g_mainHwnd, 20, &dark, sizeof dark);
    DwmSetWindowAttribute(g_mainHwnd, 19, &dark, sizeof dark);
    SetWindowPos(g_mainHwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    // Windows 10/11 有些版本只改属性不会立刻重绘非客户区：模拟一次激活切换
    SendMessageW(g_mainHwnd, WM_NCACTIVATE, FALSE, 0);
    SendMessageW(g_mainHwnd, WM_NCACTIVATE, TRUE, 0);
    RedrawWindow(g_mainHwnd, nullptr, nullptr,
                 RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    DwmFlush();   // 让 DWM 立刻提交这一帧，标题栏不会慢半拍
}

static void ApplyTheme() {
    g_ui = MakeTheme(g_darkTheme);
    ImGuiStyle& s = ImGui::GetStyle();

    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 12.0f;
    s.FrameRounding     = 9.0f;
    s.PopupRounding     = 12.0f;
    s.ScrollbarRounding = 9.0f;
    s.GrabRounding      = 9.0f;
    s.WindowBorderSize  = 0.0f;
    s.FrameBorderSize   = 1.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(18, 14);
    s.FramePadding      = ImVec2(12, 7);
    s.ItemSpacing       = ImVec2(10, 8);
    s.ItemInnerSpacing  = ImVec2(8, 6);
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 10.0f;

    s.Colors[ImGuiCol_WindowBg]             = g_ui.windowBg;
    s.Colors[ImGuiCol_ChildBg]              = g_ui.childBg;
    s.Colors[ImGuiCol_PopupBg]              = g_ui.childBg;
    s.Colors[ImGuiCol_Border]               = g_ui.border;
    s.Colors[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_FrameBg]              = g_ui.inputBg;
    s.Colors[ImGuiCol_FrameBgHovered]       = g_ui.cardHover;
    s.Colors[ImGuiCol_FrameBgActive]        = g_ui.cardSelected;
    s.Colors[ImGuiCol_TitleBg]              = g_ui.windowBg;
    s.Colors[ImGuiCol_TitleBgActive]        = g_ui.windowBg;
    s.Colors[ImGuiCol_MenuBarBg]            = g_ui.childBg;
    s.Colors[ImGuiCol_ScrollbarBg]          = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ScrollbarGrab]        = g_ui.borderStrong;
    s.Colors[ImGuiCol_ScrollbarGrabHovered] = g_ui.textFaint;
    s.Colors[ImGuiCol_ScrollbarGrabActive]  = g_ui.accent;
    s.Colors[ImGuiCol_CheckMark]            = g_ui.accentText;
    s.Colors[ImGuiCol_SliderGrab]           = g_ui.accent;
    s.Colors[ImGuiCol_SliderGrabActive]     = g_ui.accentHover;
    s.Colors[ImGuiCol_Button]               = g_ui.cardBg;
    s.Colors[ImGuiCol_ButtonHovered]        = g_ui.cardHover;
    s.Colors[ImGuiCol_ButtonActive]         = g_ui.cardSelected;
    s.Colors[ImGuiCol_Header]               = g_ui.cardSelected;
    s.Colors[ImGuiCol_HeaderHovered]        = g_ui.cardHover;
    s.Colors[ImGuiCol_HeaderActive]         = g_ui.cardSelected;
    s.Colors[ImGuiCol_Separator]            = g_ui.border;
    s.Colors[ImGuiCol_SeparatorHovered]     = g_ui.accent;
    s.Colors[ImGuiCol_SeparatorActive]      = g_ui.accent;
    s.Colors[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_ResizeGripHovered]    = g_ui.accent;
    s.Colors[ImGuiCol_ResizeGripActive]     = g_ui.accentHover;
    s.Colors[ImGuiCol_Text]                 = g_ui.text;
    s.Colors[ImGuiCol_TextDisabled]         = g_ui.textDim;
    s.Colors[ImGuiCol_TextSelectedBg]       = ImVec4(g_ui.accent.x, g_ui.accent.y, g_ui.accent.z, 0.30f);
    s.Colors[ImGuiCol_DragDropTarget]       = g_ui.accent;
    s.Colors[ImGuiCol_NavCursor]            = g_ui.accent;
    s.Colors[ImGuiCol_ModalWindowDimBg]     = g_ui.dimBg;
    s.Colors[ImGuiCol_TableHeaderBg]        = g_ui.cardBg;
    s.Colors[ImGuiCol_TableBorderStrong]    = g_ui.border;
    s.Colors[ImGuiCol_TableBorderLight]     = g_ui.border;
    s.Colors[ImGuiCol_TableRowBg]           = ImVec4(0, 0, 0, 0);
    s.Colors[ImGuiCol_TableRowBgAlt]        = ImVec4(1, 1, 1, g_ui.dark ? 0.02f : 0.35f);

    ApplyDarkTitleBar();
}

static std::wstring SettingsPath() {
    return Utf8ToWide(db::DataDir() + "\\settings.ini");
}

static bool LoadThemeSetting() {
    wchar_t buf[32] = {0};
    GetPrivateProfileStringW(L"ui", L"theme", L"dark", buf, 31, SettingsPath().c_str());
    return wcscmp(buf, L"light") != 0;
}

static void SaveThemeSetting() {
    WritePrivateProfileStringW(L"ui", L"theme", g_darkTheme ? L"dark" : L"light",
                               SettingsPath().c_str());
}

static void SetTheme(bool dark) {
    if (g_darkTheme == dark) return;
    g_darkTheme = dark;
    ApplyTheme();
    SaveThemeSetting();
}

// ============================================================ 右键菜单相关

// 打开所在位置。
//
// 原来用 explorer.exe /select 拼命令行，两个坑：
//   1. 路径里出现 "c:\\program files"（双反斜杠）时 explorer 直接放弃，跳到「此电脑」
//   2. 命令行参数解析依赖引号，路径带空格/中文时容易断
// 改用 Shell 官方接口 SHOpenFolderAndSelectItems —— 它接受 PIDL，
// 不经过命令行，路径怎么写都不会错。
static void RevealInExplorer(const std::string& path) {
    if (path.empty()) return;
    std::wstring w = Utf8ToWide(path);

    DWORD attr = GetFileAttributesW(w.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        // 索引可能是旧的，文件已经被删/挪走了：退一步打开它的父目录
        size_t slash = w.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2) {
            std::wstring parent = w.substr(0, slash);
            if (GetFileAttributesW(parent.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ShellExecuteW(nullptr, L"open", parent.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                g_lastCleanMsg = "文件已不存在，已打开所在目录";
                return;
            }
        }
        g_lastCleanMsg = "文件已不存在，目录也找不到了";
        return;
    }

    // 目录：直接打开
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }

    // 文件：在资源管理器里选中它
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w.c_str());
    if (!pidl) {
        g_lastCleanMsg = "无法定位该文件";
        return;
    }
    HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
    ILFree(pidl);
    if (FAILED(hr)) {
        // 极少数情况下 PIDL 失效，退回打开父目录
        size_t slash = w.find_last_of(L'\\');
        if (slash != std::wstring::npos && slash > 2)
            ShellExecuteW(nullptr, L"open", w.substr(0, slash).c_str(),
                          nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// 写进 Windows 剪贴板（CF_UNICODETEXT）。
// GlobalAlloc 的句柄在 64 位下必须先声明返回类型，否则被截断成 32 位。
static bool CopyToClipboard(const std::string& text) {
    if (text.empty()) return false;
    std::wstring w;
    int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (n <= 1) return false;
    w.resize((size_t)n - 1);
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, &w[0], n);

    size_t bytes = (w.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return false;
    void* dst = GlobalLock(mem);
    if (!dst) { GlobalFree(mem); return false; }
    memcpy(dst, w.c_str(), bytes);
    GlobalUnlock(mem);

    if (!OpenClipboard(nullptr)) { GlobalFree(mem); return false; }
    EmptyClipboard();
    bool ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
    CloseClipboard();
    if (!ok) GlobalFree(mem);    // 成功后所有权归系统，不能再释放
    return ok;
}

// 统一的路径右键菜单。调用前先 OpenPopup("ctx")，ID 由外层 PushID 保证唯一。
static bool ShowPathMenu(const std::string& path) {
    if (!ImGui::BeginPopup("ctx")) return false;
    bool picked = false;
    if (ImGui::MenuItem("打开所在位置")) RevealInExplorer(path);
    if (ImGui::MenuItem("复制路径")) {
        g_lastCleanMsg = CopyToClipboard(path) ? "路径已复制" : "复制失败";
    }
    if (ImGui::MenuItem("从这里开始扫描")) {
        snprintf(g_pathBuf, sizeof g_pathBuf, "%s", path.c_str());
        g_rootPath = path;
        picked = true;
    }
    ImGui::EndPopup();
    return picked;
}

// 立即模式：调用方已在自己的 PushID 作用域内，这里直接开/画菜单。
// 回 true 表示用户选了「从这里开始扫描」。
static bool PathContextMenuInline(const std::string& path) {
    if (!ImGui::BeginPopup("ctx")) return false;
    bool scan = false;
    if (ImGui::MenuItem("打开所在位置")) RevealInExplorer(path);
    if (ImGui::MenuItem("复制路径"))
        g_lastCleanMsg = CopyToClipboard(path) ? "路径已复制" : "复制失败";
    if (ImGui::MenuItem("从这里开始扫描")) scan = true;
    ImGui::EndPopup();
    return scan;
}

// 统一处理右键：置标志的在列表项里，这里负责真正开菜单并画内容。
// pi 之前只写了 OpenPopup、漏了 ShowPathMenu，所以菜单永远弹不出来。
static std::string ScanRootForPath(const std::string& path) {
    DWORD attr = GetFileAttributesW(Utf8ToWide(path).c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        size_t slash = path.find_last_of("\\/");
        if (slash != std::string::npos && slash > 2) return path.substr(0, slash);
    }
    return path;
}

static void HandlePendingContextMenu() {
    if (g_ctxRequested) {
        if (!g_ctxPath.empty()) ImGui::OpenPopup("ctx");
        g_ctxRequested = false;
    }
    if (!g_ctxPath.empty() && ShowPathMenu(g_ctxPath))
        g_pendingScan = ScanRootForPath(g_ctxPath);
}

// ============================================================ treemap

struct Rect { float x, y, w, h; };
struct Tile { int index; Rect r; };
static std::vector<Tile> g_tiles;

static float WorstRatio(float side, float sum, float other) {
    if (side <= 0.0f || sum <= 0.0f || other <= 0.0f) return 1e30f;
    float thick = sum / other;
    float t = std::max(thick, 1e-6f);
    return std::max(side / t, t / std::max(side, 1e-6f));
}

static void Squarify(const std::vector<float>& areas, const std::vector<int>& keys,
                     float x, float y, float w, float h, std::vector<Tile>& out) {
    if (areas.empty()) return;
    float totalArea = 0.0f;
    for (float a : areas) totalArea += a;
    if (totalArea <= 0.0f) return;

    float boxArea = w * h;
    std::vector<float> scaled(areas.size());
    for (size_t i = 0; i < areas.size(); ++i) scaled[i] = areas[i] / totalArea * boxArea;

    size_t i = 0;
    float cx = x, cy = y, cw = w, ch = h;
    while (i < scaled.size()) {
        float side = std::min(cw, ch);
        size_t end = i;
        float sum = 0.0f, best = 1e30f;
        while (end < scaled.size()) {
            float trial = sum + scaled[end];
            float worst = WorstRatio(side, trial, std::max(cw, ch));
            if (end > i && worst > best) break;
            best = worst;
            sum = trial;
            ++end;
        }
        bool horizontal = cw >= ch;
        float thick = horizontal ? sum / std::max(cw, 1e-6f) : sum / std::max(ch, 1e-6f);
        if (thick <= 0.0f) break;
        float offset = 0.0f;
        for (size_t k = i; k < end; ++k) {
            float len = scaled[k] / thick;
            Tile t;
            t.index = keys[k];
            t.r = horizontal ? Rect{cx + offset, cy, len, thick} : Rect{cx, cy + offset, thick, len};
            out.push_back(t);
            offset += len;
        }
        if (horizontal) { cy += thick; ch -= thick; }
        else            { cx += thick; cw -= thick; }
        if (cw <= 0.5f || ch <= 0.5f) break;
        i = end;
    }
}

// ============================================================ 扫描

static std::wstring JoinPathW(const std::wstring& dir, const wchar_t* name) {
    std::wstring out = dir;
    if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
    out += name;
    return out;
}

static void ScanWorker(std::string root) {
    auto started = std::chrono::steady_clock::now();

    g_pool.clear();
    g_entries.clear();
    g_visited = 0;
    g_fileCount = 0;
    g_totalBytes = 0;
    g_current = 0;

    Entry rootEntry{};
    size_t rootLen = root.size();
    g_pool.insert(g_pool.end(), root.begin(), root.end());
    g_pool.push_back('\0');
    rootEntry.nameLen = (uint32_t)rootLen;
    rootEntry.parent = -1;
    rootEntry.firstChild = -1;
    rootEntry.nextSibling = -1;
    rootEntry.isDir = 1;
    g_entries.push_back(rootEntry);

    // 边扫边写索引：扫完的那一刻就能搜，不用再走一遍盘
    std::string indexPath = db::IndexPathFor(root);
    bool indexOk = db::Open(indexPath);
    if (indexOk) {
        db::BeginBulk();
        g_indexMsg = "索引随扫描建立中…";
    } else {
        g_indexMsg = "索引建立失败（不影响扫描）";
    }
    long long pendingCommits = 0;
    long long rootId = 0;
    if (indexOk) rootId = db::Add(0, root, 0, true);

    // 每层目录都记着自己的索引 id，子项拿它当 parent
    struct Pending { int parent; long long dbId; std::wstring dir; };
    // 队列 = 广度优先。用栈的话会一头扎进 C:\Windows 几千层，
    // 方块图上半天只有一个块，看不到「铺开」的过程。
    std::deque<Pending> queue;
    queue.push_back({0, rootId, Utf8ToWide(root)});

    while (!queue.empty()) {
        if (g_stopReq.load()) break;
        while (g_paused.load() && !g_stopReq.load()) Sleep(50);

        Pending cur = queue.front();
        queue.pop_front();
        g_visited.fetch_add(1);
        { std::lock_guard<std::mutex> lock(g_currentMutex); g_currentPathW = cur.dir; }

        // 每 100ms 把已扫到的目录树抄给渲染线程
        {
            static auto lastPush = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - lastPush).count() > 0.1) {
                lastPush = now;
                const size_t kLiveKeep = 128;

                // 1) 收集所有目录的原始大小
                std::vector<std::pair<int, uint64_t>> all;
                all.reserve(g_entries.size());
                for (size_t i = 0; i < g_entries.size(); ++i) {
                    if (g_entries[i].isDir) all.push_back({(int)i, g_entries[i].size});
                }
                std::sort(all.begin(), all.end(),
                          [](const auto& a, const auto& b) { return a.second > b.second; });
                if (all.size() > kLiveKeep) all.resize(kLiveKeep);

                // 2) 把选中项及其祖先一起标记进快照
                std::vector<char> mark(g_entries.size(), 0);
                for (auto& p : all) {
                    for (int a = p.first; a >= 0; a = g_entries[a].parent) mark[a] = 1;
                }

                std::vector<LiveItem> snap;
                std::unordered_map<int, int> idxToSnap;
                for (size_t i = 0; i < g_entries.size(); ++i) {
                    if (!mark[i]) continue;
                    LiveItem it;
                    it.name = std::string(NameOf((int)i));
                    it.path = PathOf((int)i);
                    it.rawSize = g_entries[i].size;
                    it.size = g_entries[i].size;
                    it.index = (int)i;
                    it.parent = g_entries[i].parent;
                    idxToSnap[(int)i] = (int)snap.size();
                    snap.push_back(std::move(it));
                }

                // 3) 在快照内建立父子关系并汇总树大小
                for (size_t i = 0; i < snap.size(); ++i) {
                    auto it = idxToSnap.find(snap[i].parent);
                    if (it != idxToSnap.end()) snap[it->second].children.push_back((int)i);
                }
                std::vector<int> order(snap.size());
                for (size_t i = 0; i < snap.size(); ++i) order[i] = (int)i;
                std::sort(order.begin(), order.end(), [&](int a, int b) {
                    return g_entries[snap[a].index].depth > g_entries[snap[b].index].depth;
                });
                for (int si : order) {
                    for (int ci : snap[si].children) snap[si].size += snap[ci].size;
                }
                for (auto& it : snap) {
                    std::sort(it.children.begin(), it.children.end(),
                              [&](int a, int b) { return snap[a].size > snap[b].size; });
                }

                std::lock_guard<std::mutex> lock(g_liveMutex);
                g_live = std::move(snap);
                g_livePathW = cur.dir;
                g_liveVisited = g_visited.load();
            }
        }

        std::wstring pattern = cur.dir;
        if (!pattern.empty() && pattern.back() != L'\\') pattern.push_back(L'\\');
        pattern += L'*';

        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                    FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;

        do {
            if (g_stopReq.load()) break;
            const wchar_t* nmW = fd.cFileName;
            if (nmW[0] == L'.' && (nmW[1] == L'\0' || (nmW[1] == L'.' && nmW[2] == L'\0'))) continue;
            std::string nm = WideToUtf8(nmW);   // UTF-8 存进名字池
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            uint64_t size = isDir ? 0 : ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            std::wstring fullW = JoinPathW(cur.dir, nmW);
            std::string full = WideToUtf8(fullW);

            long long childId = 0;
            if (indexOk) {
                childId = db::Add(cur.dbId, nm, size, isDir);
                if (++pendingCommits >= 200000) {
                    db::FlushBulk();
                    db::BeginBulk();
                    pendingCommits = 0;
                }
            }

            if (!isDir) {
                // 只加给直接父目录；祖先留到最后一次性后序累加
                g_fileCount.fetch_add(1);
                g_entries[cur.parent].size += size;
                g_totalBytes.fetch_add(size);
                continue;
            }

            Entry e{};
            size_t len = nm.size();
            e.nameOff = (uint32_t)g_pool.size();
            e.nameLen = (uint32_t)len;
            g_pool.insert(g_pool.end(), nm.begin(), nm.end());
            g_pool.push_back('\0');
            e.parent = cur.parent;
            e.firstChild = -1;
            e.nextSibling = g_entries[cur.parent].firstChild;
            g_entries[cur.parent].firstChild = (int32_t)g_entries.size();
            e.size = 0;
            e.isDir = 1;
            e.depth = (uint8_t)std::min<int>(255, g_entries[cur.parent].depth + 1);
            g_entries.push_back(e);
            queue.push_back({(int)g_entries.size() - 1, childId, fullW});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    {
        std::vector<int> order(g_entries.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
        std::sort(order.begin(), order.end(), [](int a, int b) {
            return g_entries[a].depth > g_entries[b].depth;
        });
        for (int idx : order) {
            int p = g_entries[idx].parent;
            if (p >= 0) g_entries[p].size += g_entries[idx].size;
        }
    }

    if (indexOk) {
        bool interrupted = g_stopReq.load();
        if (db::EndBulk(root, interrupted)) {
            g_indexMsg = interrupted
                ? "索引只建了一部分，下次会重来"
                : ("索引已就绪 · " + std::to_string((long long)g_entries.size()) + " 目录 + "
                   + std::to_string(g_fileCount.load()) + " 文件");
        } else {
            g_indexMsg = "索引换名失败（可能有另一个窗口在用）";
        }
    }

    auto done = std::chrono::steady_clock::now();
    g_scanSeconds = std::chrono::duration<double>(done - started).count();
    g_scanning = false;
}

static void ScanWorkerSafe(std::string root) {
    try {
        ScanWorker(std::move(root));
    } catch (...) {
        g_scanning = false;
    }
}

// ============================================================ 规则检测

static void SetDetectMsg(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_msgMutex);
    g_detectMsg = s;
}

static std::string GetDetectMsg() {
    std::lock_guard<std::mutex> lk(g_msgMutex);
    return g_detectMsg;
}

static void ClearDetectMsg() {
    std::lock_guard<std::mutex> lk(g_msgMutex);
    g_detectMsg.clear();
}

static void RequestDetectCancel() {
    std::lock_guard<std::mutex> lk(g_detectJobMutex);
    if (g_detectJob) g_detectJob->store(true);
}

// gen 是该次检测的代号：扫描一重启就 ++g_detectGen，旧线程即使还在跑
// 也不会往界面里写数据。cancel 是这次检测自己的取消标志。
static void DetectWorker(int gen, std::shared_ptr<std::atomic<bool>> cancel) {
    if (g_detectGen.load() != gen) return;
    g_detecting = true;
    SetDetectMsg("正在检查可清理项…");
    try {
        // g_rules 只在 WinMain 里构造一次。这里绝不能重建 vector：
        // 界面里的 Finding.rule 全部指向它的元素，重建会让指针悬空。
        {
            std::lock_guard<std::mutex> lk(g_findMutex);
            if (g_detectGen.load() == gen) g_findings.clear();
        }

        auto found = rules::Detect(
            g_rules,
            [gen](const std::string& name, int idx, int total, int elapsedMs,
                  const rules::Finding* partial) {
                if (g_detectGen.load() != gen) return;
                // 心跳：实时显示当前规则名和已耗时
                SetDetectMsg(name + "  (" + std::to_string(idx + 1) + "/" +
                             std::to_string(total) + ")  " + std::to_string(elapsedMs) + " ms");
                // 流式结果：每完成一条就 append，界面不用等到全部算完
                if (partial) {
                    std::lock_guard<std::mutex> lk(g_findMutex);
                    if (g_detectGen.load() == gen) g_findings.push_back(*partial);
                }
            },
            cancel.get());

        // 最终用完整、排序后的结果替换流式追加的临时列表
        {
            std::lock_guard<std::mutex> lk(g_findMutex);
            if (g_detectGen.load() == gen) g_findings = std::move(found);
        }
        if (g_detectGen.load() == gen) ClearDetectMsg();
    } catch (...) {
        if (g_detectGen.load() == gen) SetDetectMsg("检查失败");
    }
    if (g_detectGen.load() == gen) g_detecting = false;
}

static void StartDetect() {
    int gen = ++g_detectGen;
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lk(g_detectJobMutex);
        g_detectJob = cancel;
    }
    g_detectStarted = true;
    std::thread(DetectWorker, gen, cancel).detach();
}

// ============================================================ 清理

static uint64_t DeleteTreeW(const std::wstring& path, int* errors) {
    uint64_t freed = 0;
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return 0;

    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        std::wstring pattern = path + L"\\*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                freed += DeleteTreeW(path + L"\\" + fd.cFileName, errors);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        if (!RemoveDirectoryW(path.c_str()) && errors) ++*errors;
    } else {
        uint64_t size = 0;
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
            size = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
        if (DeleteFileW(path.c_str())) freed += size;
        else if (errors) ++*errors;
    }
    return freed;
}

static uint64_t DeleteTree(const std::string& path, int* errors) {
    return DeleteTreeW(Utf8ToWide(path), errors);
}

// 在 UI 线程上收集要删的路径（这里还读 g_selected / g_deepSelected / g_hitSelected）。
// 真正的删除交给后台线程，否则删 12 GB / 几万个文件时界面会假死。
static std::vector<std::string> CollectCleanPaths() {
    std::vector<std::string> paths;
    std::set<std::string> seen;
    auto add = [&](const std::string& p) {
        if (p.empty() || rules::IsProtected(p)) return;
        if (seen.insert(p).second) paths.push_back(p);
    };

    std::vector<rules::Finding> snap;
    {
        std::lock_guard<std::mutex> lk(g_findMutex);
        snap = g_findings;
    }
    for (auto& f : snap) {
        if (!f.rule || !g_selected.count(f.rule->id)) continue;
        for (const std::string& path : f.existing) {
            if (InCurrentDir(path)) add(path);
        }
    }
    for (auto& kv : g_deepSelected) add(kv.first);
    for (auto& kv : g_hitSelected) add(kv.first);
    return paths;
}

static void CleanWorker(std::vector<std::string> paths) {
    uint64_t freed = 0;
    int errors = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        freed += DeleteTree(paths[i], &errors);
        g_cleanFreed = freed;
        g_cleanDone = (int)i + 1;
    }
    g_cleanErrors = errors;
    g_cleanFinish = true;
    g_cleaning = false;
}

static void StartClean() {
    if (g_cleaning.load()) return;
    auto paths = CollectCleanPaths();
    if (paths.empty()) {
        g_lastCleanMsg = "没有可删除的项（可能都不在当前扫描范围内）";
        return;
    }
    g_cleanFreed = 0;
    g_cleanErrors = 0;
    g_cleanDone = 0;
    g_cleanTotal = (int)paths.size();
    g_cleanFinish = false;
    g_cleaning = true;
    std::thread(CleanWorker, std::move(paths)).detach();
}

// ============================================================ D3D

static ID3D11Device*           g_pd3dDevice = nullptr;
static ID3D11DeviceContext*    g_pd3dContext = nullptr;
static IDXGISwapChain*         g_swapChain = nullptr;
static ID3D11RenderTargetView* g_mainRTV = nullptr;

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL got{};
    if (D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                      D3D11_SDK_VERSION, &sd, &g_swapChain, &g_pd3dDevice, &got,
                                      &g_pd3dContext) != S_OK)
        return false;
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV);
        back->Release();
    }
    return true;
}

static void CleanupDeviceD3D() {
    if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_pd3dContext) { g_pd3dContext->Release(); g_pd3dContext = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
}

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_pd3dDevice->CreateRenderTargetView(back, nullptr, &g_mainRTV);
        back->Release();
    }
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (g_pd3dDevice && wParam != SIZE_MINIMIZED) {
            if (g_mainRTV) { g_mainRTV->Release(); g_mainRTV = nullptr; }
            g_swapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam),
                                       DXGI_FORMAT_UNKNOWN, 0);
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ============================================================ 方块图

static void CollectChildren(int parent, std::vector<int>& out) {
    for (int c = g_entries[parent].firstChild; c >= 0; c = g_entries[c].nextSibling)
        out.push_back(c);
}

// 一个方块的数据源：扫描中是快照，扫描后是 g_entries 的子项
struct Blk {
    std::string name;
    std::string path;
    uint64_t size = 0;
    int idx = -1;        // >=0 表示能继续下钻
    bool isRest = false; // 「（其余 N 项）」合成块
    int restParent = -1; // 合成块对应的父目录（g_entries 下标）
    bool isFile = false; // 当前目录下的直接文件
};

// 每个名字记住自己的动画进度，让新块从 0 长出来
static std::unordered_map<std::string, float> g_animMap;
static int g_tileIdCounter = 0;   // 每帧重置，保证方块按钮 ID 唯一

static void GatherChildren(int parent, std::vector<Blk>& out, size_t keep = 0) {
    for (int c = g_entries[parent].firstChild; c >= 0; c = g_entries[c].nextSibling) {
        Blk b;
        b.name = NameOf(c);
        b.size = g_entries[c].size;
        b.idx = c;
        b.path.clear();
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Blk& a, const Blk& b) { return a.size > b.size; });

    // 只留前 keep 个，其余并成一块。C:\ 下有几十个子目录，
    // 全画出来就是一排看不见的碎条 —— Python 版顶层也只有 4~5 块。
    if (keep > 0 && out.size() > keep) {
        size_t restCount = out.size() - keep;
        uint64_t rest = 0;
        for (size_t i = keep; i < out.size(); ++i) rest += out[i].size;
        out.resize(keep);
        Blk more;
        more.name = "（其余 " + std::to_string(restCount) + " 项）";
        more.size = rest;
        more.idx = -1;
        more.path = PathOf(parent);   // 右键用父目录的菜单
        more.isRest = true;
        more.restParent = parent;
        out.push_back(std::move(more));
    }
}

static void GatherLiveChildren(int parent, std::vector<Blk>& out,
                               const std::vector<LiveItem>* snap, size_t keep = 0) {
    if (!snap) return;
    const LiveItem* parentItem = nullptr;
    for (const auto& it : *snap) {
        if (it.index == parent) { parentItem = &it; break; }
    }
    if (!parentItem) return;
    for (int ci : parentItem->children) {
        const auto& it = (*snap)[ci];
        Blk b;
        b.name = it.name;
        b.path = it.path;
        b.size = it.size;
        b.idx = it.index;
        out.push_back(std::move(b));
    }
    if (keep > 0 && out.size() > keep) {
        size_t restCount = out.size() - keep;
        uint64_t rest = 0;
        for (size_t i = keep; i < out.size(); ++i) rest += out[i].size;
        out.resize(keep);
        Blk more;
        more.name = "（其余 " + std::to_string(restCount) + " 项）";
        more.size = rest;
        more.idx = -1;
        more.path = parentItem->path;   // 右键用父目录的菜单
        more.isRest = true;
        more.restParent = parent;
        out.push_back(std::move(more));
    }
}

// ---- 当前目录的文件
//
// 扫描只把目录放进内存（338 万文件的完整路径要 270 MB）。但用户点进一个只有
// 文件的目录时，右栏和方块图不能是空的 —— 这里按需枚举「当前目录」的直接文件，
// 只保留最大的 kFileKeep 个，内存有上限。
struct FileItem {
    std::string name;
    std::string path;
    uint64_t size = 0;
};
static std::vector<FileItem> g_currentFiles;
static int      g_currentFilesFor = -2;
static int      g_currentFileTotal = 0;
static uint64_t g_currentFilesBytes = 0;
static bool     g_currentFilesTruncated = false;
static const int kFileKeep = 400;
static const int kFileScanCap = 200000;   // 极端目录别把 UI 卡死

static void EnsureCurrentFiles() {
    if (g_scanning.load() || g_current < 0 || g_entries.empty()) {
        g_currentFiles.clear();
        g_currentFilesFor = -2;
        g_currentFileTotal = 0;
        g_currentFilesBytes = 0;
        g_currentFilesTruncated = false;
        return;
    }
    if (g_currentFilesFor == g_current) return;
    g_currentFilesFor = g_current;
    g_currentFiles.clear();
    g_currentFileTotal = 0;
    g_currentFilesBytes = 0;
    g_currentFilesTruncated = false;

    const std::string dir = PathOf(g_current);
    if (dir.empty()) return;
    std::wstring pattern = Utf8ToWide(dir);
    if (pattern.empty()) return;
    if (pattern.back() != L'\\') pattern.push_back(L'\\');
    pattern.push_back(L'*');

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<FileItem> items;
    items.reserve((size_t)kFileKeep * 2);
    bool capped = false;
    int seen = 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (++seen > kFileScanCap) { capped = true; break; }

        FileItem f;
        f.name = WideToUtf8(fd.cFileName);
        f.path = dir;
        if (!f.path.empty() && f.path.back() != '\\') f.path.push_back('\\');
        f.path += f.name;
        f.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        g_currentFilesBytes += f.size;
        g_currentFileTotal++;
        items.push_back(std::move(f));

        if (items.size() >= (size_t)kFileKeep * 8) {
            std::nth_element(items.begin(), items.begin() + kFileKeep, items.end(),
                             [](const FileItem& a, const FileItem& b) { return a.size > b.size; });
            items.resize(kFileKeep);
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(items.begin(), items.end(),
              [](const FileItem& a, const FileItem& b) { return a.size > b.size; });
    if ((int)items.size() > kFileKeep) items.resize(kFileKeep);
    g_currentFilesTruncated = capped || g_currentFileTotal > (int)items.size();
    g_currentFiles = std::move(items);
}

static void ApplyKeep(std::vector<Blk>& out, size_t keep, int parent) {
    if (keep == 0 || out.size() <= keep) return;
    size_t restCount = out.size() - keep;
    uint64_t rest = 0;
    for (size_t i = keep; i < out.size(); ++i) rest += out[i].size;
    out.resize(keep);
    Blk more;
    more.name = "（其余 " + std::to_string(restCount) + " 项）";
    more.size = rest;
    more.idx = -1;
    more.path = PathOf(parent);
    more.isRest = true;
    more.restParent = parent;
    out.push_back(std::move(more));
}

// 当前视图 = 子目录 + 当前目录的直接文件。递归到下一层时仍只画目录，
// 因为每一层点进去都会重新变成「当前视图」。
static void GatherViewChildren(std::vector<Blk>& out, size_t keep) {
    if (g_current < 0 || g_entries.empty()) return;
    GatherChildren(g_current, out, 0);
    for (const FileItem& f : g_currentFiles) {
        Blk b;
        b.name = f.name;
        b.path = f.path;
        b.size = f.size;
        b.idx = -1;
        b.isFile = true;
        out.push_back(std::move(b));
    }
    std::sort(out.begin(), out.end(), [](const Blk& a, const Blk& b) { return a.size > b.size; });
    ApplyKeep(out, keep, g_current);
}

// 配色规则：
//   顶层 —— 从 8 个挑选过的色相里取，哈希决定，保证相邻大概率不同
//   子层 —— 以顶色为基准，按绝对层数调整明暗/饱和度
//   亮色主题用低饱和高亮度 + 深色文字；暗色主题用高饱和低亮度 + 浅色文字
static const float kTopHues[8] = {0.60f, 0.015f, 0.13f, 0.36f, 0.77f, 0.50f, 0.075f, 0.44f};

static ImU32 MakeColor(float hue, float sat, float val) {
    hue -= floorf(hue);
    if (sat < 0.0f) sat = 0.0f;
    if (sat > 1.0f) sat = 1.0f;
    if (val < 0.0f) val = 0.0f;
    if (val > 1.0f) val = 1.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, sat, val, r, g, b);
    return IM_COL32((int)(r * 255), (int)(g * 255), (int)(b * 255), 255);
}

static uint32_t HashName(const std::string& name) {
    uint32_t h = 2166136261u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    return h;
}

static ImU32 TopColor(const std::string& name) {
    uint32_t h = HashName(name);
    float hue = kTopHues[h % 8] + (float)((h >> 7) % 7) * 0.006f;
    if (g_darkTheme) {
        float sat = 0.36f + (float)((h >> 13) % 10) / 100.0f;
        float val = 0.40f + (float)((h >> 17) % 8) / 100.0f;
        return MakeColor(hue, sat, val);
    }
    float sat = 0.22f + (float)((h >> 13) % 7) / 100.0f;
    float val = 0.97f - (float)((h >> 17) % 3) / 100.0f;
    return MakeColor(hue, sat, val);
}

static ImU32 ChildColor(ImU32 base, int siblingIndex, int generation) {
    float r = ((base >> IM_COL32_R_SHIFT) & 0xFF) / 255.0f;
    float g = ((base >> IM_COL32_G_SHIFT) & 0xFF) / 255.0f;
    float b = ((base >> IM_COL32_B_SHIFT) & 0xFF) / 255.0f;
    float h, s, v;
    ImGui::ColorConvertRGBtoHSV(r, g, b, h, s, v);

    // 以「顶层基色」为基准按绝对层数计算，而不是从父色逐代累加。
    // 旧写法 h/s/v 每层再叠加，第 3~4 层就变成近白色，深层块完全分不清。
    const int gen = std::min(generation, 3);
    h += 0.020f * (float)((siblingIndex % 4) - 1) + 0.010f * (float)gen;
    if (g_darkTheme) {
        s = std::max(0.28f, s - 0.035f * (float)gen);
        v = std::min(0.72f, v + 0.055f * (float)gen);
    } else {
        s = std::min(0.48f, s + 0.022f * (float)gen);
        v = std::max(0.80f, v - 0.030f * (float)gen);
    }
    return MakeColor(h, s, v);
}

// rootColor：top 层的基色，传给整棵子树。depth == 0 时自己算 TopColor。
static void DrawTreemapImpl(ImDrawList* dl, const std::vector<Blk>& blocks,
                            float x, float y, float w, float h, int depth, ImU32 rootColor,
                            const std::vector<LiveItem>* liveSnap = nullptr) {
    if (blocks.empty() || w < 6 || h < 6) return;
    // 扫描结束就不再做生长动画，直接按真实面积画满
    const bool animate = g_scanning.load();

    // 面积用「真实大小 × 动画进度」，这样块是长出来的而不是突然铺满
    std::vector<float> areas;
    std::vector<int> keys;
    for (size_t i = 0; i < blocks.size(); ++i) {
        float v = 1.0f;
        if (animate) {
            std::string key = blocks[i].name + "|" + std::to_string(depth);
            float& a = g_animMap[key];
            if (a < 0.02f) a = 0.02f;
            a += (1.0f - a) * 0.22f;
            v = a;
        }
        areas.push_back((float)std::max<uint64_t>(blocks[i].size, 1) * v);
        keys.push_back((int)i);
    }

    std::vector<Tile> tiles;
    Squarify(areas, keys, 0, 0, w, h, tiles);

    for (size_t ti = 0; ti < tiles.size(); ++ti) {
        const Tile& t = tiles[ti];
        const Blk& b = blocks[t.index];
        // 留 1.5px 缝，块之间才有分隔感
        ImVec2 a(x + t.r.x + 1.0f, y + t.r.y + 1.0f);
        ImVec2 c(x + t.r.x + t.r.w - 1.0f, y + t.r.y + t.r.h - 1.0f);
        float bw = c.x - a.x, bh = c.y - a.y;
        if (bw < 2 || bh < 2) continue;

        ImU32 color = b.isFile ? ThemeColor(g_ui.fileBlock)
                               : ((depth == 0) ? TopColor(b.name)
                                               : ChildColor(rootColor, (int)ti, depth));
        ImU32 base = (depth == 0 || b.isFile) ? color : rootColor;
        dl->AddRectFilled(a, c, color, 0.0f);            // 直角，和 SpaceSniffer 一致
        dl->AddRect(a, c, ThemeColor(g_ui.blockBorder), 0.0f, 0, 1.0f);   // 1px 描边

        // 标题条 / 容量：按块高分档，避免重叠；文字单行并裁剪到各自区域
        const float titleH = 19.0f;
        const float sizeH = 17.0f;
        const bool drawTitle = (bw > 40.0f && bh > 22.0f);
        const bool drawSize = drawTitle && (bw > 54.0f && bh >= titleH + sizeH + 6.0f);
        if (drawTitle) {
            dl->AddRectFilled(a, ImVec2(c.x, a.y + titleH), ThemeColor(g_ui.titleOverlay));
            dl->PushClipRect(a, ImVec2(c.x, a.y + titleH), true);
            dl->AddText(ImVec2(a.x + 5, a.y + 1), ThemeColor(g_ui.blockText), b.name.c_str());
            dl->PopClipRect();

            if (drawSize) {
                std::string sz = FormatBytes(b.size);
                dl->AddRectFilled(ImVec2(a.x, c.y - sizeH), c, ThemeColor(g_ui.sizeOverlay));
                dl->PushClipRect(ImVec2(a.x, c.y - sizeH), c, true);
                dl->AddText(ImVec2(a.x + 5, c.y - sizeH), ThemeColor(g_ui.blockSubText), sz.c_str());
                dl->PopClipRect();
            }
        }

        // 递归画下一层：只要这块还够大就继续切，一直到看不出结构为止。
        // 原来写死 depth < 2，点进 Program Files 后 Siemens 36.6 GB 就不再细分了。
        if (depth < 8 && b.idx >= 0 && bw > 64 && bh > 54) {
            std::vector<Blk> children;
            if (liveSnap) GatherLiveChildren(b.idx, children, liveSnap, 6);
            else GatherChildren(b.idx, children, 6);
            float top = drawTitle ? 22.0f : 4.0f;
            // 父块底部盖着容量条，子块必须停在它上方，否则容量文字会被子块盖掉
            float bottom = drawSize ? sizeH : 0.0f;
            float childH = bh - top - bottom - 1.0f;
            if (childH > 6.0f) {
                if (children.size() == 1) {
                    // 只有一个子目录时直接穿透（Siemens 下面只有 NX2412），
                    // 但不要继续叠边框，否则一层套一层全是粗边。
                    DrawTreemapImpl(dl, children, a.x, a.y + top, bw, childH, depth, base, liveSnap);
                } else if (children.size() > 1) {
                    DrawTreemapImpl(dl, children, a.x + 1, a.y + top, bw - 2, childH,
                                    depth + 1, base, liveSnap);
                }
            }
        }

        // 点击区（只有第一层可点，避免和子块抢）
        if (depth == 0) {
            ImGui::SetCursorScreenPos(a);
            // 用全局递增 ID：递归里如果拿 t.index 当名字，
            // 每层的 "tk0" 都是同一个 ID，点一个块会点到另一个。
            ImGui::PushID(g_tileIdCounter++);
            ImGui::InvisibleButton("##tk", ImVec2(std::max(1.0f, bw), std::max(1.0f, bh)));
            bool clickedRight = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            bool clickedLeft = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (clickedRight) {
                // 合成块也能右键：菜单对象是它的父目录
                if (!b.path.empty()) {
                    g_ctxPath = b.path;
                    g_ctxRequested = true;
                } else if (b.idx >= 0) {
                    g_ctxPath = PathOf(b.idx);
                    g_ctxRequested = true;
                }
            }
            if (clickedLeft) {
                if (b.isRest) {
                    // 「其余 N 项」可点：展开当前目录全部子项，并切到深度清理列表
                    if (b.restParent >= 0) g_expandRestParent = b.restParent;
                    if (b.restParent == g_current) g_mode = 1;
                } else if (b.idx >= 0) {
                    g_current = b.idx;
                    g_expandRestParent = -1;   // 换了目录，取消展开
                } else if (b.isFile && !b.path.empty()) {
                    RevealInExplorer(b.path);  // 文件块：在资源管理器中定位
                }
            }
            if (hovered) {
                dl->AddRect(a, c, ThemeColor(g_ui.accent), 4.0f, 0, 2.0f);
                std::string sz = FormatBytes(b.size);
                if (b.isRest)
                    ImGui::SetTooltip("%s\n共 %s\n点击展开并查看列表", b.name.c_str(), sz.c_str());
                else if (b.isFile)
                    ImGui::SetTooltip("%s\n%s\n点击在资源管理器中显示", b.name.c_str(), sz.c_str());
                else
                    ImGui::SetTooltip("%s\n%s", b.name.c_str(), sz.c_str());
            }
        }
    }
}

static void RenderTreemap() {
    ImVec2 canvasPos = ImGui::GetCursorScreenPos();
    ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    if (canvasSize.x < 10 || canvasSize.y < 10) return;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    g_tileIdCounter = 0;
    dl->AddRectFilled(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y),
                      ThemeColor(g_ui.canvas), 12.0f);

    // ---- 扫描中：用快照画，方块会跟着扫描一起长
    if (g_scanning.load()) {
        std::vector<LiveItem> snap;
        std::string path;
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            snap = g_live;
            path = WideToUtf8(g_livePathW);
        }
        if (snap.empty()) {
            const char* hint = "正在扫描…";
            ImVec2 sz = ImGui::CalcTextSize(hint);
            dl->AddText(ImVec2(canvasPos.x + (canvasSize.x - sz.x) * 0.5f,
                               canvasPos.y + canvasSize.y * 0.5f),
                        ThemeColor(g_ui.textDim), hint);
            return;
        }
        std::vector<Blk> blocks;
        // 顶层同样聚合：AppData\Local 有 135 个子目录时，不该在图上画 135 条碎条
        GatherLiveChildren(0, blocks, &snap, g_expandRestParent == 0 ? 0 : 8);
        if (blocks.empty()) {
            const char* hint = "正在扫描…";
            ImVec2 sz = ImGui::CalcTextSize(hint);
            dl->AddText(ImVec2(canvasPos.x + (canvasSize.x - sz.x) * 0.5f,
                               canvasPos.y + canvasSize.y * 0.5f),
                        ThemeColor(g_ui.textDim), hint);
            return;
        }
        dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x,
                                           canvasPos.y + canvasSize.y), true);
        DrawTreemapImpl(dl, blocks, canvasPos.x + 6, canvasPos.y + 6,
                        canvasSize.x - 12, canvasSize.y - 12, 0, 0, &snap);
        dl->PopClipRect();
        HandlePendingContextMenu();
        return;
    }

    if (g_current < 0 || g_entries.empty()) {
        float cx = canvasPos.x + canvasSize.x * 0.5f;
        float cy = canvasPos.y + canvasSize.y * 0.42f;

        const char* title = "先选一个位置";
        ImVec2 tsz = ImGui::CalcTextSize(title);
        dl->AddText(ImVec2(cx - tsz.x * 0.5f, cy), ThemeColor(g_ui.text), title);

        const char* sub = "上面选磁盘、粘贴路径，或点「浏览...」挑文件夹，然后点「扫描」";
        ImVec2 ssz = ImGui::CalcTextSize(sub);
        dl->AddText(ImVec2(cx - ssz.x * 0.5f, cy + 28), ThemeColor(g_ui.textDim), sub);

        ImVec2 btnSize(118, 32);
        ImGui::SetCursorScreenPos(ImVec2(cx - btnSize.x * 0.5f, cy + 62));
        ImGui::PushStyleColor(ImGuiCol_Button, g_ui.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_ui.accentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, g_ui.accentText);
        if (ImGui::Button("开始扫描", btnSize)) StartScan(g_pathBuf);
        ImGui::PopStyleColor(3);
        return;
    }

    std::vector<Blk> blocks;
    GatherViewChildren(blocks, g_expandRestParent == g_current ? 0 : 8);
    if (blocks.empty()) {
        const char* hint = "这个目录下没有可显示的子项";
        ImVec2 sz = ImGui::CalcTextSize(hint);
        dl->AddText(ImVec2(canvasPos.x + (canvasSize.x - sz.x) * 0.5f,
                           canvasPos.y + canvasSize.y * 0.5f),
                    ThemeColor(g_ui.textDim), hint);
        return;
    }

    dl->PushClipRect(canvasPos, ImVec2(canvasPos.x + canvasSize.x, canvasPos.y + canvasSize.y), true);
    DrawTreemapImpl(dl, blocks, canvasPos.x + 6, canvasPos.y + 6,
                    canvasSize.x - 12, canvasSize.y - 12, 0, 0);
    dl->PopClipRect();
    HandlePendingContextMenu();
}

// ============================================================ 搜索

static void RunSearchNow(const std::string& query) {
    // 扫描线程正在用小 sqlite 连接写索引；UI 线程若走 OpenExisting/Close
    // 会把这个连接和语句在另一个线程里关掉，轻则索引丢失，重则崩溃。
    if (g_scanning.load()) {
        g_lastCleanMsg = "扫描进行中，索引正在重建，等扫描结束后再搜索";
        return;
    }
    g_searchQuery = query;
    g_hits.clear();
    if (query.empty()) return;
    if (g_searchMode == 1) {
        // 内容索引是独立库；没建过就直接返回空，由右栏引导去建
        std::string q = query;
        if (!g_extFilter.empty()) {
            q += " ext:";
            bool first = true;
            for (const auto& e : g_extFilter) {
                std::string one = e;
                if (!one.empty() && one[0] == '.') one.erase(one.begin());
                if (!first) q += ",";
                q += one;
                first = false;
            }
        }
        g_hits = content::Search(g_rootPath, q, 400);
        return;
    }
    std::string indexPath = db::IndexPathFor(g_rootPath);
    if (GetFileAttributesW(Utf8ToWide(indexPath).c_str()) == INVALID_FILE_ATTRIBUTES) return;
    db::OpenExisting(indexPath);
    g_hits = db::Search(query, 400);
    db::Close();
    if (!g_extFilter.empty()) {
        g_hits.erase(std::remove_if(g_hits.begin(), g_hits.end(),
                                    [](const db::Hit& h) {
                                        std::string e = h.path;
                                        const size_t dot = e.find_last_of('.');
                                        if (dot == std::string::npos) return true;
                                        e = e.substr(dot);
                                        for (auto& c : e) c = (char)tolower((unsigned char)c);
                                        return g_extFilter.count(e) == 0;
                                    }),
                     g_hits.end());
    }
}

// 当前左侧打开目录的完整路径。扫描中 g_entries 正在重建，只能退回扫描根。
static std::string CurrentDirPath() {
    if (!g_scanning.load() && g_current > 0 && g_current < (int)g_entries.size())
        return PathOf(g_current);
    return g_rootPath;
}

static bool PathUnder(const std::string& path, const std::string& dir) {
    if (dir.empty()) return true;
    auto norm = [](std::string s) {
        for (auto& c : s) c = (char)tolower((unsigned char)c);
        for (auto& c : s) if (c == '/') c = '\\';
        while (s.size() > 3 && s.back() == '\\') s.pop_back();
        return s;
    };
    std::string p = norm(path);
    std::string d = norm(dir);
    if (p.size() < d.size()) return false;
    if (p.compare(0, d.size(), d) != 0) return false;
    return p.size() == d.size() || d.back() == '\\' || p[d.size()] == '\\';
}

// 只显示「当前左侧打开的目录」下的项。
// 扫 E: 时右栏绝不出现 C: 的缓存；钻进子目录后列表也跟着收窄。
static bool InCurrentDir(const std::string& path) {
    return PathUnder(path, CurrentDirPath());
}

// ============================================================ 自定义控件

static float SegmentedWidth(const char* const* labels, int count) {
    float total = 6.0f;
    for (int i = 0; i < count; ++i)
        total += ImGui::CalcTextSize(labels[i]).x + 26.0f + (i ? 2.0f : 0.0f);
    return total;
}

// iOS 风格分段控件：一个胶囊底 + 选中项一个强调色胶囊。
static int Segmented(const char* id, const char* const* labels, int count, int current,
                     float height = 32.0f, float* outWidth = nullptr) {
    ImGui::PushID(id);
    float total = SegmentedWidth(labels, count);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + height),
                      ThemeColor(g_ui.tabBg), height * 0.5f);

    const float innerH = height - 6.0f;
    float x = p.x + 3.0f;
    for (int i = 0; i < count; ++i) {
        float w = ImGui::CalcTextSize(labels[i]).x + 26.0f;
        ImVec2 a(x, p.y + 3.0f);
        ImVec2 b(x + w, p.y + 3.0f + innerH);
        bool active = (i == current);
        bool hovered = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered();
        if (active)
            dl->AddRectFilled(a, b, ThemeColor(g_ui.accent), innerH * 0.5f);
        else if (hovered)
            dl->AddRectFilled(a, b, ThemeColor(g_ui.cardHover), innerH * 0.5f);

        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(labels[i], ImVec2(w, innerH));
        if (ImGui::IsItemClicked()) current = i;

        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                    ThemeColor(active ? g_ui.accentText
                                      : (hovered ? g_ui.text : g_ui.textDim)),
                    labels[i]);
        x += w + 2.0f;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + height));
    ImGui::Dummy(ImVec2(total, 0.0f));
    ImGui::PopID();
    if (outWidth) *outWidth = total;
    return current;
}

// 自绘勾选框：圆角方块 + 加强色填充 + 白色对勾。
static bool ThemeCheckbox(const char* id, bool* value, float size = 18.0f) {
    ImGui::PushID(id);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cb", ImVec2(size, size));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) *value = !*value;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = *value ? ThemeColor(g_ui.accent) : ThemeColor(g_ui.inputBg);
    ImU32 bd = *value ? ThemeColor(g_ui.accent)
                      : ThemeColor(hovered ? g_ui.borderStrong : g_ui.border);
    dl->AddRectFilled(p, ImVec2(p.x + size, p.y + size), bg, 5.0f);
    dl->AddRect(p, ImVec2(p.x + size, p.y + size), bd, 5.0f, 0, 1.0f);
    if (*value) {
        dl->AddLine(ImVec2(p.x + size * 0.24f, p.y + size * 0.52f),
                    ImVec2(p.x + size * 0.43f, p.y + size * 0.72f),
                    ThemeColor(g_ui.accentText), 2.0f);
        dl->AddLine(ImVec2(p.x + size * 0.43f, p.y + size * 0.72f),
                    ImVec2(p.x + size * 0.78f, p.y + size * 0.28f),
                    ThemeColor(g_ui.accentText), 2.0f);
    }
    ImGui::PopID();
    return *value;
}

// 幽灵按钮：只有细边框和极浅的底色，适合次级操作。
static bool GhostButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, g_ui.cardBg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_ui.cardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_ui.cardSelected);
    ImGui::PushStyleColor(ImGuiCol_Border, g_ui.border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// 主操作按钮：实心强调色。
static bool PrimaryButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, g_ui.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_ui.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_ui.accentHover);
    ImGui::PushStyleColor(ImGuiCol_Text, g_ui.accentText);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

static bool DangerButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, g_ui.danger);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_ui.danger);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_ui.danger);
    ImGui::PushStyleColor(ImGuiCol_Text, g_ui.accentText);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// ============================================================ 右栏卡片行

// 一行圆角卡片：左侧复选框 + 标题（+ 可选徽标 / 副标题），右侧容量。
// 返回 true 表示整行被左键点到（用于下钻）。
//
// 关键点：先放一个透明 InvisibleButton 当整行点击区，再 SetItemAllowOverlap()
// 让复选框盖在上面 —— 这样点勾选框只切换勾选，点别处才触发整行。
static bool RowCard(const char* id, const std::string& title, const std::string& right,
                    bool* checked, const char* badge, const std::string& tooltip,
                    const std::string& subtitle = "") {
    ImGui::PushID(id);

    const float rowH = 36.0f;
    const float pad = 10.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail < 80.0f) avail = 80.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(avail, rowH));
    bool hovered = ImGui::IsItemHovered();
    bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool rowRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    bool selected = checked && *checked;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 bg = selected ? ThemeColor(g_ui.cardSelected)
                        : (hovered ? ThemeColor(g_ui.cardHover) : ThemeColor(g_ui.cardBg));
    ImU32 bd = selected ? ThemeColor(g_ui.accent) : ThemeColor(g_ui.border);
    dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + rowH), bg, 9.0f);
    dl->AddRect(p0, ImVec2(p0.x + avail, p0.y + rowH), bd, 9.0f, 0, 1.0f);
    if (selected) {
        dl->AddRectFilled(ImVec2(p0.x + 1, p0.y + 9), ImVec2(p0.x + 4, p0.y + rowH - 9),
                          ThemeColor(g_ui.accent), 1.5f);
    }

    // 自绘勾选框
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + (rowH - 18.0f) * 0.5f));
    ThemeCheckbox("##chk", checked, 18.0f);

    float textY = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    std::string rs = right;
    float rsW = 0.0f;
    {
        ImGui::PushFont(g_fontSmall, 0.0f);
        rsW = ImGui::CalcTextSize(rs.c_str()).x;
        ImGui::PopFont();
    }
    float rightX = p0.x + avail - rsW - pad;
    float textX = p0.x + pad + 30.0f;
    float clipMax = std::max(textX + 1.0f, rightX - 8.0f);

    // 标题 + 徽标 + 副标题；整组裁剪到右侧容量文字左边
    ImGui::PushClipRect(ImVec2(textX, p0.y), ImVec2(clipMax, p0.y + rowH), true);
    float cx = textX;
    ImGui::SetCursorScreenPos(ImVec2(cx, textY));
    ImGui::TextUnformatted(title.c_str());
    cx += ImGui::CalcTextSize(title.c_str()).x;
    if (badge && badge[0]) {
        cx += 8.0f;
        ImGui::SetCursorScreenPos(ImVec2(cx, textY));
        ImGui::TextColored(g_ui.warning, "%s", badge);
        cx += ImGui::CalcTextSize(badge).x;
    }
    if (!subtitle.empty() && cx + 10 < clipMax) {
        ImGui::PushFont(g_fontSmall, 0.0f);
        float sy = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx + 10, sy));
        ImGui::TextColored(g_ui.textFaint, "%s", subtitle.c_str());
        ImGui::PopFont();
    }
    ImGui::PopClipRect();

    // 右侧容量（小字，绿色）
    {
        ImGui::PushFont(g_fontSmall, 0.0f);
        float sy = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, sy));
        ImGui::TextColored(g_ui.success, "%s", rs.c_str());
        ImGui::PopFont();
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH));
    ImGui::Dummy(ImVec2(avail, 6.0f));

    if (hovered && !tooltip.empty()) ImGui::SetTooltip("%s", tooltip.c_str());
    if (rowRightClicked) {
        g_ctxPath = tooltip.empty() ? title : tooltip;
        g_ctxRequested = true;
    }

    ImGui::PopID();
    return rowClicked;
}

// ============================================================ 右栏

// 检索结果里 400 条同名文件只看文件名分不清，副标题显示路径末尾。
static std::string ShortParent(const std::string& path) {
    size_t slash = path.find_last_of("\\/");
    std::string parent = (slash == std::string::npos) ? std::string() : path.substr(0, slash);
    if (parent.size() > 52) parent = "…" + parent.substr(parent.size() - 51);
    return parent;
}

// 规则可能有多条路径；只累加落在当前目录里的那些，避免钻进子目录后
// 仍显示整条规则的总大小（Chrome 和 Chrome Beta 各一份的情况）。
static uint64_t ScopedRuleSize(const rules::Finding& f) {
    if (f.pathSize.size() != f.existing.size()) return f.size;
    uint64_t s = 0;
    for (size_t i = 0; i < f.existing.size(); ++i)
        if (InCurrentDir(f.existing[i])) s += f.pathSize[i];
    return s;
}

// ============================================================ 内容检索

// 索引建在扫描根目录对应的库里；扫描线程和索引线程互斥
static void StartContentIndex() {
    if (g_scanning.load() || g_contentBusy.load()) return;
    g_contentCancel = false;
    g_contentBusy = true;
    content::ResetProgress();
    std::string root = g_rootPath;
    std::thread([root] {
        content::Build(root, g_contentCancel);
        g_contentBusy = false;
    }).detach();
}

static void DrawContentSearchTab() {
    // 建完之后如果搜索框里还有词，自动补一次查询
    static bool prevBusy = false;
    const bool busyNow = g_contentBusy.load();
    if (prevBusy && !busyNow && g_searchMode == 1 && g_searchBuf[0]) {
        RunSearchNow(g_searchBuf);
        g_mode = 2;
    }
    prevBusy = busyNow;

    if (busyNow) {
        const int total = content::g_seen.load();
        const int done  = content::g_done.load();
        const int fresh = content::g_parsed.load();
        const int reused = content::g_skipped.load();

        ImGui::TextDisabled("正在建立内容索引…");
        if (total > 0) {
            const float frac = (float)done / (float)total;
            // 进度条本身不写字：10px 高的条里塞文字会压到上下两行
            ImGui::ProgressBar(frac, ImVec2(-1, 8), "");
            ImGui::TextDisabled("%d / %d  (%.0f%%)", done, total, frac * 100.0f);
        } else {
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 8), "");
            ImGui::TextDisabled("正在清点文件…");
        }
        ImGui::TextDisabled("新抽取 %d 个 · 复用上次 %d 个", fresh, reused);
        std::string cur = content::CurrentPath();
        if (cur.size() > 56) cur = "…" + cur.substr(cur.size() - 55);
        if (!cur.empty()) ImGui::TextDisabled("%s", cur.c_str());
        ImGui::Dummy(ImVec2(0, 4));
        if (GhostButton("停止", ImVec2(64, 26))) g_contentCancel = true;
        return;
    }

    if (!content::HasIndex(g_rootPath)) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextDisabled("还没有内容索引。");
        ImGui::TextDisabled("只抽文本/代码、DXF、STEP/IGES/OBJ；建一次，之后按大小和时间增量更新。");
        ImGui::Dummy(ImVec2(0, 6));
        const bool blocked = g_scanning.load();
        if (blocked) ImGui::BeginDisabled();
        if (PrimaryButton("建立内容索引", ImVec2(0, 30))) StartContentIndex();
        if (blocked) ImGui::EndDisabled();
        if (blocked) ImGui::TextDisabled("扫描进行中，等扫描结束后再建。");
        std::string err = content::LastError();
        if (!err.empty()) ImGui::TextDisabled("%s", err.c_str());
        return;
    }

    static int lastFresh = -1, lastReused = -1;
    if (lastFresh != content::g_parsed.load() || lastReused != content::g_skipped.load()) {
        lastFresh = content::g_parsed.load();
        lastReused = content::g_skipped.load();
    }
    if (g_searchQuery.empty()) {
        ImGui::TextDisabled("内容索引就绪。在上面搜索框输入关键词即可搜正文。");
        // 解释"为什么只有这么点文件"：全盘扫描数 vs 可抽正文数
        {
            const int scanned = (int)g_fileCount.load();
            const int cand = content::g_seen.load();
            if (scanned > 0 && cand > 0)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
                ImGui::TextWrapped("扫描 %d 个文件 → 可抽正文 %d 个（其余是依赖目录、不支持的格式、未开 OCR 的图片）",
                                   scanned, cand);
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, 2));
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
            ImGui::TextWrapped("语法：词A 词B（同时含）· A OR B · -排除 · ext:pdf · name:报告 · size:>10m · after:2024-06");
            ImGui::PopStyleColor();
        }
        if (lastFresh > 0 || lastReused > 0)
            ImGui::TextDisabled("本次：新抽取 %d 个，复用上次结果 %d 个。", lastFresh, lastReused);
        if (GhostButton("增量更新", ImVec2(0, 26))) StartContentIndex();
        if (!extract::PdfReady())
            ImGui::TextDisabled("提示：同目录放一个 pdfium.dll 才能搜 PDF 正文。");
        if (!extract::DwgReady())
            ImGui::TextDisabled("提示：要搜 .dwg 需要 dwg2dxf.exe + libredwg-0.dll 或 ODA File Converter。");
        if (!extract::LegacyReady())
            ImGui::TextDisabled("提示：.doc/.ppt/.xls 需要 catdoc 系列或 LibreOffice。");
        if (!extract::ArchiveReady())
            ImGui::TextDisabled("提示：.7z/.rar 需要安装 7-Zip 或 WinRAR。");
        {
            static bool ocrDirty = false;
            const bool ocrOn = extract::OcrReady();
            char ocrLbl[64];
            snprintf(ocrLbl, sizeof ocrLbl, "OCR 图片：%s", ocrOn ? "开" : "关");
            if (GhostButton(ocrLbl, ImVec2(0, 26))) {
                extract::SetOcrEnabled(!ocrOn);
                ocrDirty = true;      // 开关变了要整库重建，否则旧的"抽空"记录不会重试
            }
            ImGui::SameLine();
            if (ocrDirty && content::HasIndex(g_rootPath))
                ImGui::TextDisabled("设置已改变 —— 点下面的「增量更新」重新索引。");
            if (!extract::OcrAvailable())
                ImGui::TextDisabled("（扫描件 PDF 需装 Tesseract-OCR 才会自动识别）");
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
                ImGui::TextWrapped("扫描件 PDF 自动识别（每次最多 120 页）；这个开关只管图片。");
                ImGui::PopStyleColor();
            }
        }
        return;
    }
    if (g_hits.empty()) {
        ImGui::TextDisabled("没有文件内容匹配「%s」。", g_searchQuery.c_str());
        return;
    }
    ImGui::TextDisabled("内容命中 %d 项（按相关度排序，勾选可加入待清理）", (int)g_hits.size());
    for (auto& hit : g_hits) {
        bool was = g_hitSelected.count(hit.path) > 0;
        bool now = was;
        std::string sz = FormatBytes(hit.size);
        RowCard(hit.path.c_str(), hit.name, sz, &now, "", hit.path,
                hit.snippet.empty() ? ShortParent(hit.path) : hit.snippet);
        if (now != was) {
            if (now) g_hitSelected[hit.path] = hit.size;
            else g_hitSelected.erase(hit.path);
        }
    }
}

static void DrawRightPanel() {
    // ---- 标签行：三个页签在左，两个批量按钮贴右
    static const char* tabLabels[] = {"推荐清理", "深度清理", "检索结果"};
    int nextTab = Segmented("tabs", tabLabels, 3, g_mode, 32.0f);
    if (nextTab != g_mode) g_mode = nextTab;

    // 批量按钮靠右：基准图里它们和标签同一行；窄窗口放不下就换到下一行，避免压住页签
    float rightEdge = ImGui::GetWindowContentRegionMax().x;
    const float kBatchW = 76.0f + 8.0f + 70.0f;
    float batchX = rightEdge - kBatchW;
    if (batchX > 286.0f) ImGui::SameLine(batchX);
    else ImGui::SetCursorPosX(std::max(0.0f, batchX));   // 换行后靠右，别留一大片空白
    bool hasSafe = false;
    {
        std::lock_guard<std::mutex> lk(g_findMutex);
        for (auto& f : g_findings) {
            if (!f.rule || f.rule->risk != rules::SAFE || f.size == 0) continue;
            for (auto& p : f.existing)
                if (InCurrentDir(p)) { hasSafe = true; break; }
            if (hasSafe) break;
        }
    }
    if (!hasSafe) ImGui::BeginDisabled();
    if (GhostButton("全选安全项", ImVec2(76, 28))) {
        std::lock_guard<std::mutex> lk(g_findMutex);
        for (auto& f : g_findings) {
            if (!f.rule || f.rule->risk != rules::SAFE || f.size == 0) continue;
            for (auto& p : f.existing)
                if (InCurrentDir(p)) { g_selected.insert(f.rule->id); break; }
        }
    }
    ImGui::SameLine();
    if (GhostButton("取消全选", ImVec2(70, 28))) g_selected.clear();
    if (!hasSafe) ImGui::EndDisabled();

    ImGui::Separator();

    // ---- 列表区（底部留 34px 给状态栏）
    ImGui::BeginChild("##rightlist", ImVec2(0, -34), false);

    if (g_mode == 0) {
        std::string dmsg = GetDetectMsg();
        if (g_detecting.load()) ImGui::TextDisabled("%s", dmsg.c_str());

        std::vector<rules::Finding> snap;
        {
            std::lock_guard<std::mutex> lk(g_findMutex);
            snap = g_findings;
        }

        // 只看当前左侧打开的目录：扫 E: 就只列 E: 下的项，钻进子目录后列表跟着收窄。
        // 其他盘的缓存一律不显示，也不提供「显示其他盘」开关。
        std::vector<rules::Finding> rows;
        for (auto& f : snap) {
            if (!f.rule || f.size == 0) continue;
            std::vector<std::string> keep;
            uint64_t keepSize = 0;
            bool haveSizes = f.pathSize.size() == f.existing.size();
            for (size_t i = 0; i < f.existing.size(); ++i) {
                if (!InCurrentDir(f.existing[i])) continue;
                keep.push_back(f.existing[i]);
                if (haveSizes) keepSize += f.pathSize[i];
            }
            if (keep.empty()) continue;
            rules::Finding g = f;
            g.existing = std::move(keep);
            g.size = haveSizes ? keepSize : f.size;
            rows.push_back(std::move(g));
        }

        if (rows.empty()) {
            if (!g_detecting.load() && g_scanSeconds == 0 && !g_scanning.load()) {
                ImGui::TextDisabled("先扫描一个目录，这里会列出当前目录下可清理的缓存项。");
            } else if (!g_detecting.load()) {
                ImGui::TextDisabled("「%s」下没有可清理的缓存项。", CurrentDirPath().c_str());
                ImGui::TextDisabled("（缓存规则只覆盖系统/应用缓存目录；其他盘的内容不会出现在这里）");
                if (g_currentFileTotal > 0)
                    ImGui::TextDisabled("这个目录下还有 %d 个直接文件，可在「深度清理」查看。",
                                        g_currentFileTotal);
            }
        } else {
            for (auto& f : rows) {
                bool was = g_selected.count(f.rule->id) > 0;
                bool now = was;
                std::string sz = FormatBytes(f.size);
                const char* badge = f.timedOut                  ? "超时"
                                    : f.rule->risk != rules::SAFE ? "需确认"
                                                                  : "";
                RowCard(f.rule->id.c_str(), f.rule->name, sz, &now, badge,
                        f.existing.empty() ? f.rule->why : f.existing[0]);
                if (now != was) {
                    if (now) g_selected.insert(f.rule->id);
                    else g_selected.erase(f.rule->id);
                }
            }
        }
    } else if (g_mode == 1) {
        if (g_scanning.load()) {
            // 扫描线程正在重建 g_entries，这里直接读会越界
            ImGui::TextDisabled("扫描进行中，完成后显示这个目录的子项。");
        } else if (g_current < 0 || g_entries.empty()) {
            ImGui::TextDisabled("先扫描一个目录，这里会列出它下面的每一项。");
        } else {
            std::vector<Blk> children;
            GatherViewChildren(children, 0);
            int dirCount = 0;
            for (auto& c : children) if (!c.isFile) ++dirCount;
            std::string head = FormatBytes(g_entries[g_current].size);
            ImGui::TextDisabled("%d 个子目录 · %d 个文件 · 合计 %s（勾选可加入待清理）",
                                dirCount, g_currentFileTotal, head.c_str());
            for (auto& c : children) {
                std::string path = c.isFile ? c.path : PathOf(c.idx);
                bool was = g_deepSelected.count(path) > 0;
                bool now = was;
                std::string sz = FormatBytes(c.size);
                std::string rowId = c.isFile ? ("deepf:" + path)
                                             : ("deep:" + std::to_string(c.idx));
                bool goIn = RowCard(rowId.c_str(), c.name, sz, &now, "", path);
                if (now != was) {
                    if (now) g_deepSelected[path] = c.size;
                    else g_deepSelected.erase(path);
                }
                if (goIn) {
                    if (c.isFile) RevealInExplorer(path);
                    else { g_current = c.idx; g_expandRestParent = -1; }
                }
            }
            if (g_currentFilesTruncated) {
                ImGui::TextDisabled("…文件较多，仅列出最大的 %d 个（共 %d 个）",
                                    (int)g_currentFiles.size(), g_currentFileTotal);
            }
        }
    } else if (g_searchMode == 1) {
        DrawContentSearchTab();
    } else {
        if (g_searchQuery.empty()) {
            ImGui::TextDisabled("在上面搜索框输入文件名。");
        } else if (g_hits.empty()) {
            ImGui::TextDisabled("没有匹配「%s」的文件。", g_searchQuery.c_str());
        } else {
            ImGui::TextDisabled("命中 %d 项（按大小排序，勾选可加入待清理）",
                                (int)g_hits.size());
            for (auto& hit : g_hits) {
                bool was = g_hitSelected.count(hit.path) > 0;
                bool now = was;
                std::string sz = FormatBytes(hit.size);
                // 标题用文件名；同名文件太多，用父路径末尾区分
                RowCard(hit.path.c_str(), hit.name, sz, &now, "", hit.path, ShortParent(hit.path));
                if (now != was) {
                    if (now) g_hitSelected[hit.path] = hit.size;
                    else g_hitSelected.erase(hit.path);
                }
            }
        }
    }
    ImGui::EndChild();

    // popup 不能画在 BeginChild 里面，必须等 child 关掉之后统一开
    HandlePendingContextMenu();

    // ---- 底部状态栏：已选 X · N 项 + 清理按钮
    uint64_t selectedBytes = 0;
    int selectedCount = 0;
    {
        std::lock_guard<std::mutex> lk(g_findMutex);
        for (auto& f : g_findings) {
            if (!f.rule || !g_selected.count(f.rule->id)) continue;
            // 只统计会被实际删除的路径（和列表/清理逻辑保持一致）
            bool anyIn = false;
            for (auto& p : f.existing)
                if (InCurrentDir(p)) { anyIn = true; break; }
            if (anyIn) {
                selectedBytes += ScopedRuleSize(f);
                ++selectedCount;
            }
        }
    }
    for (auto& kv : g_deepSelected) { selectedBytes += kv.second; ++selectedCount; }
    for (auto& kv : g_hitSelected) { selectedBytes += kv.second; ++selectedCount; }

    std::string selSz = FormatBytes(selectedBytes);
    ImGui::TextColored(g_ui.success, "%s", selSz.c_str());
    ImGui::SameLine();
    ImGui::PushFont(g_fontSmall, 0.0f);
    ImGui::TextDisabled("· %d 项", selectedCount);
    ImGui::PopFont();
    ImGui::SameLine(std::max(180.0f, ImGui::GetWindowContentRegionMax().x - 100.0f));

    bool canClean = selectedCount > 0 && !g_cleaning.load();
    if (!canClean) ImGui::BeginDisabled();
    if (PrimaryButton(g_cleaning.load() ? "清理中…" : "清理选中项", ImVec2(96, 28))) {
        // 任何删除都要先弹窗确认 —— 这一步不可撤销，不给用户留反悔机会
        ImGui::OpenPopup("##confirmClean");
    }
    if (!canClean) ImGui::EndDisabled();

    if (ImGui::BeginPopupModal("##confirmClean", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<rules::Finding> fsnap;
        {
            std::lock_guard<std::mutex> lk(g_findMutex);
            fsnap = g_findings;
        }
        ImGui::Text("确认删除以下内容？");
        ImGui::Separator();
        ImGui::TextDisabled("推荐清理 %d 项 · 深度勾选 %d 项 · 检索勾选 %d 项",
                            (int)g_selected.size(), (int)g_deepSelected.size(),
                            (int)g_hitSelected.size());
        ImGui::TextDisabled("合计 %s", selSz.c_str());
        ImGui::Separator();
        int shown = 0;
        const int kMaxShown = 8;
        for (auto& f : fsnap) {
            if (!f.rule || !g_selected.count(f.rule->id)) continue;
            bool anyIn = false;
            for (auto& p : f.existing)
                if (InCurrentDir(p)) { anyIn = true; break; }
            if (!anyIn) continue;
            if (shown++ >= kMaxShown) break;
            ImGui::BulletText("%s  (%s)", f.rule->name.c_str(),
                              FormatBytes(ScopedRuleSize(f)).c_str());
        }
        for (auto& kv : g_deepSelected) {
            if (shown++ >= kMaxShown) break;
            ImGui::BulletText("%s", kv.first.c_str());
        }
        for (auto& kv : g_hitSelected) {
            if (shown++ >= kMaxShown) break;
            ImGui::BulletText("%s", kv.first.c_str());
        }
        if (selectedCount > kMaxShown)
            ImGui::TextDisabled("…还有 %d 项未列出", selectedCount - kMaxShown);
        ImGui::Separator();
        ImGui::TextColored(g_ui.danger, "直接删除，不进回收站，无法撤销。");
        if (DangerButton("确认删除", ImVec2(100, 0))) {
            StartClean();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (GhostButton("取消", ImVec2(80, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ============================================================ 主界面

// 扫描盘根时能拿到「已用空间」，用它做进度分母；
// 扫的是普通文件夹就没有分母，只显示已扫量。
static void ProbeVolume(const std::string& root) {
    g_volumeUsed = 0;
    g_volumeTotal = 0;
    std::string r = root;
    if (r.size() < 2 || r[1] != ':') return;
    if (r.size() > 3) return;              // 不是盘根，没有可靠分母
    std::wstring wr = Utf8ToWide(r);
    ULARGE_INTEGER total{}, freeBytes{}, freeToCaller{};
    if (!GetDiskFreeSpaceExW(wr.c_str(), &freeToCaller, &total, &freeBytes)) return;
    g_volumeTotal = total.QuadPart;
    g_volumeUsed = total.QuadPart - freeBytes.QuadPart;
}

static void StartScan(const std::string& root) {
    if (g_scanning.load()) return;
    // 内容索引和扫描互斥：新扫描开始先把索引线程叫停
    g_contentCancel = true;
    // 新扫描一开始，旧检测的结果全部作废（旧线程即使还在跑也写不进界面）
    RequestDetectCancel();
    ++g_detectGen;
    g_detecting = false;
    g_detectStarted = false;
    ClearDetectMsg();
    g_rootPath = root;
    ProbeVolume(root);
    g_scanning = true;
    g_stopReq = false;
    g_paused = false;
    {
        std::lock_guard<std::mutex> lk(g_findMutex);
        g_findings.clear();
    }
    {
        // 不清掉的话，新扫描头 100ms 会画出上一次的旧快照
        std::lock_guard<std::mutex> lk(g_liveMutex);
        g_live.clear();
        g_livePathW.clear();
    }
    g_selected.clear();
    g_deepSelected.clear();
    g_hitSelected.clear();
    g_hits.clear();
    g_searchQuery.clear();
    g_scanSeconds = 0;
    g_animMap.clear();
    g_current = 0;          // 旧下标不能留到新 g_entries 里被读到
    g_expandRestParent = -1;
    g_currentFiles.clear();
    g_currentFilesFor = -2;
    g_currentFileTotal = 0;
    g_currentFilesBytes = 0;
    g_currentFilesTruncated = false;
    std::thread(ScanWorkerSafe, root).detach();
}

static std::string g_pendingSearch;   // 来自 SPACESIFT_SEARCH，扫完后自动执行
static bool        g_pendingContent = false;   // 来自 SPACESIFT_CONTENT，扫完后自动建内容索引

// ============================================================ 顶栏 / 状态条

static void DrawLogo() {
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float box = 34.0f;
    const float h = 38.0f;
    const char* word = "SpaceSift";
    const char* sub = "磁盘空间分析";
    float logoW = box + 11.0f + ImGui::CalcTextSize(word).x;

    ImGui::SetCursorScreenPos(p);
    ImGui::Dummy(ImVec2(logoW, h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + box, p.y + box), ThemeColor(g_ui.brand), 10.0f);
    ImVec2 c(p.x + box * 0.5f, p.y + box * 0.5f);
    dl->PathArcTo(c, 9.0f, -1.35f, 0.55f, 20);
    dl->PathFillConvex(IM_COL32(255, 255, 255, 235));
    dl->AddCircle(c, 9.0f, IM_COL32(255, 255, 255, 242), 0, 1.6f);

    dl->AddText(ImGui::GetFont(), 15.0f, ImVec2(p.x + box + 11.0f, p.y - 2.0f),
                ThemeColor(g_ui.text), word);
    if (g_fontSmall)
        dl->AddText(g_fontSmall, 11.5f, ImVec2(p.x + box + 11.0f, p.y + 18.0f),
                    ThemeColor(g_ui.textDim), sub);

    ImGui::SetCursorScreenPos(ImVec2(p.x + logoW, p.y));
    ImGui::Dummy(ImVec2(0.0f, h));
}

static void DrawStatusStrip() {
    uint64_t scanned = g_totalBytes.load();
    uint64_t denom = g_volumeUsed.load();
    bool scanning = g_scanning.load();

    float pct = 0.0f;
    bool known = false;
    if (!scanning && g_scanSeconds > 0) {
        pct = 1.0f;
        known = denom > 0;
    } else if (scanning && denom > 0) {
        pct = (float)((double)scanned / (double)denom);
        if (pct > 1.0f) pct = 1.0f;
        known = true;
    }
    if (scanning && !known) pct = (float)fmod(ImGui::GetTime() * 0.6, 1.0);

    char left[192];
    if (scanning) {
        if (known)
            snprintf(left, sizeof left, "%.1f%%   %s / %s", pct * 100.0f,
                     FormatBytes(scanned).c_str(), FormatBytes(denom).c_str());
        else
            snprintf(left, sizeof left, "已扫 %s · %lld 文件",
                     FormatBytes(scanned).c_str(), (long long)g_fileCount.load());
    } else if (g_scanSeconds > 0) {
        snprintf(left, sizeof left, "%lld 文件 · %s · %.1f 秒",
                 (long long)g_fileCount.load(), FormatBytes(scanned).c_str(), g_scanSeconds);
    } else {
        snprintf(left, sizeof left, "就绪 · 选择一个位置开始");
    }

    std::string right;
    ImVec4 rightColor = g_ui.textFaint;
    if (g_cleaning.load()) {
        right = "正在清理 " + std::to_string(g_cleanDone.load()) + "/" +
                std::to_string(g_cleanTotal.load()) + " …";
        rightColor = g_ui.warning;
    } else if (!g_lastCleanMsg.empty()) {
        right = g_lastCleanMsg;
        rightColor = g_ui.success;
    } else if (scanning) {
        std::string cur;
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            cur = WideToUtf8(g_livePathW);
        }
        if (cur.size() > 58) cur = "…" + cur.substr(cur.size() - 57);
        right = "扫描中 · " + std::to_string(g_visited.load()) + " 目录 · " + cur;
        rightColor = g_ui.textDim;
    } else if (g_contentBusy.load()) {
        right = "建立内容索引 · 已写入 " + std::to_string(content::g_parsed.load()) + " 个 …";
        rightColor = g_ui.warning;
    } else if (g_detecting.load()) {
        right = GetDetectMsg();
        rightColor = g_ui.textDim;
    } else if (g_scanSeconds > 0) {
        right = "扫描完成";
        rightColor = g_ui.textFaint;
    }

    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const float h = 32.0f;
    ImGui::Dummy(ImVec2(w, h));

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ThemeColor(g_ui.cardBg), 10.0f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ThemeColor(g_ui.border), 10.0f, 0, 1.0f);

    const float barX = p.x + 12.0f;
    const float barW = 150.0f;
    const float barH = 6.0f;
    float barY = p.y + (h - barH) * 0.5f;
    dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                      ThemeColor(g_ui.border), barH * 0.5f);
    float fill = barW * pct;
    if (fill > 0.5f)
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + fill, barY + barH),
                          ThemeColor(g_ui.accent), barH * 0.5f);

    ImGui::PushFont(g_fontSmall, 0.0f);
    float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(barX + barW + 12.0f, ty));
    ImGui::TextColored(g_ui.textDim, "%s", left);

    if (!right.empty()) {
        float tw = ImGui::CalcTextSize(right.c_str()).x;
        float rx = p.x + w - tw - 12.0f;
        float minX = barX + barW + 220.0f;
        if (rx > minX) {
            ImGui::SetCursorScreenPos(ImVec2(rx, ty));
            ImGui::TextColored(rightColor, "%s", right.c_str());
        } else {
            ImGui::PushClipRect(ImVec2(minX, p.y), ImVec2(p.x + w - 10.0f, p.y + h), true);
            ImGui::SetCursorScreenPos(ImVec2(minX, ty));
            ImGui::TextColored(rightColor, "%s", right.c_str());
            ImGui::PopClipRect();
        }
    }
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
}

static void DrawUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 清理线程结束后统一收尾（g_selected 等状态只能在 UI 线程改）
    if (g_cleanFinish.exchange(false)) {
        int errors = g_cleanErrors.load();
        std::string msg = "已清理 " + FormatBytes(g_cleanFreed.load());
        if (errors) msg += "（" + std::to_string(errors) + " 个文件被占用，已跳过）";
        msg += " · 重新检测中，方块图下次扫描后更新";
        g_lastCleanMsg = msg;
        g_selected.clear();
        g_deepSelected.clear();
        g_hitSelected.clear();
        g_currentFilesFor = -2;   // 刷新当前目录的文件列表
        StartDetect();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F5) && !g_scanning.load()) StartScan(g_pathBuf);
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_scanning.load()) g_stopReq = true;

    EnsureCurrentFiles();

    // ---- 顶栏：logo / 位置 / 操作 / 主题切换
    DrawLogo();
    ImGui::SameLine(0, 14);

    ImGui::SetNextItemWidth(112);
    const char* driveLabel = (g_driveIdx >= 0 && g_driveIdx < (int)g_drives.size())
                                 ? g_drives[g_driveIdx].c_str() : "选择磁盘...";
    if (ImGui::BeginCombo("##drive", driveLabel)) {
        for (int i = 0; i < (int)g_drives.size(); ++i) {
            if (ImGui::Selectable(g_drives[i].c_str(), g_driveIdx == i)) {
                g_driveIdx = i;
                snprintf(g_pathBuf, sizeof g_pathBuf, "%s", g_drives[i].c_str());
                g_rootPath = g_pathBuf;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(280);
    if (ImGui::InputText("##path", g_pathBuf, sizeof g_pathBuf)) g_rootPath = g_pathBuf;
    ImGui::SameLine();

    if (GhostButton("浏览…", ImVec2(62, 0))) {
        std::string picked;
        if (BrowseFolder(picked)) {
            snprintf(g_pathBuf, sizeof g_pathBuf, "%s", picked.c_str());
            g_rootPath = g_pathBuf;
        }
    }
    ImGui::SameLine();

    if (g_scanning.load()) {
        if (GhostButton(g_paused.load() ? "继续" : "暂停", ImVec2(62, 0)))
            g_paused = !g_paused.load();
        ImGui::SameLine();
        if (DangerButton(g_stopReq.load() ? "结束中" : "结束", ImVec2(56, 0))) g_stopReq = true;
    } else {
        if (PrimaryButton("扫描", ImVec2(64, 0))) StartScan(g_pathBuf);
    }
    ImGui::SameLine();

    {
        static const char* searchModeLabels[] = {"文件名", "内容"};
        int m = Segmented("smode", searchModeLabels, 2, g_searchMode, 30.0f);
        if (m != g_searchMode) {
            g_searchMode = m;
            g_hits.clear();          // 别让上一种模式的结果留在列表里
            if (g_searchBuf[0]) {
                RunSearchNow(g_searchBuf);
                g_mode = 2;
            }
        }
    }
    ImGui::SameLine();

    // 搜索框跟在 文件名|内容 后面（左对齐），主题切换推到最右
    ImGui::SameLine();
    ImGui::SetNextItemWidth(320);
    const bool go = ImGui::InputTextWithHint("##search",
                                             g_searchMode ? "搜索文件内容…" : "搜索文件名…",
                                             g_searchBuf, sizeof g_searchBuf,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    if (go) {
        RunSearchNow(g_searchBuf);
        g_mode = g_searchBuf[0] ? 2 : 0;
    }

    static const char* themeLabels[] = {"亮色", "暗色"};
    const float toggleW = SegmentedWidth(themeLabels, 2);
    const float rightEdge = ImGui::GetWindowContentRegionMax().x;
    const float toggleX = rightEdge - toggleW;
    if (toggleX > ImGui::GetCursorPosX() + 8.0f) ImGui::SameLine(toggleX);
    else ImGui::SameLine();
    const int themeIdx = Segmented("theme", themeLabels, 2, g_darkTheme ? 1 : 0, 30.0f);
    if ((themeIdx == 1) != g_darkTheme) SetTheme(themeIdx == 1);

    // ---- 筛选条：只在「内容模式 + 已有搜索结果」时出现在下面那块空位 ----
    if (g_searchMode == 1 && !g_searchQuery.empty()) {
        static std::string lastExtRoot;
        static int lastExtFiles = -1;
        if (g_extListDirty || lastExtRoot != g_rootPath || lastExtFiles != content::g_files.load()) {
            g_extList = content::ListExtensions(g_rootPath);
            lastExtRoot = g_rootPath;
            lastExtFiles = content::g_files.load();
            g_extListDirty = false;
        }
        if (!g_extList.empty()) {
            const float rowX = 8.0f;
            ImGui::Dummy(ImVec2(0, 12));      // 上方留白
            ImGui::SetCursorPosX(rowX);

            ImGui::PushFont(g_fontSmall);
            ImGui::TextDisabled("筛选");
            ImGui::PopFont();
            ImGui::SameLine(0, 10);

            float x = ImGui::GetCursorPosX();
            const float startX = x;
            bool firstChip = true;
            auto chip = [&](const std::string& text, bool active) -> bool {
                ImGui::PushFont(g_fontSmall);
                const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
                const float w = ts.x + 20.0f;
                const float h = 26.0f;

                if (!firstChip) {
                    if (x + w > rightEdge) { ImGui::NewLine(); ImGui::SetCursorPosX(startX); x = startX; }
                    else ImGui::SameLine(x);
                }
                firstChip = false;

                const ImVec2 p = ImGui::GetCursorScreenPos();
                ImGui::InvisibleButton(text.c_str(), ImVec2(w, h));
                const bool hovered = ImGui::IsItemHovered();
                const bool clicked = ImGui::IsItemClicked();

                ImDrawList* dl = ImGui::GetWindowDrawList();
                const ImU32 bg = active ? ThemeColor(g_ui.brand)
                                : hovered ? ThemeColor(g_ui.cardHover)
                                          : ThemeColor(g_ui.cardBg);
                const ImU32 bd = active ? ThemeColor(g_ui.brand) : ThemeColor(g_ui.border);
                dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), bg, 8.0f);
                dl->AddRect(p, ImVec2(p.x + w, p.y + h), bd, 8.0f, 0, 1.0f);
                dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                            ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f),
                            active ? IM_COL32(255, 255, 255, 255) : ThemeColor(g_ui.text),
                            text.c_str());
                ImGui::PopFont();
                x += w + 4.0f;
                return clicked;
            };

            if (chip("全部", g_extFilter.empty())) {
                g_extFilter.clear();
                RunSearchNow(g_searchBuf);
            }
            for (auto& e : g_extList) {
                const bool on = g_extFilter.count(e.ext) > 0;
                if (chip(e.ext, on)) {
                    if (on) g_extFilter.erase(e.ext);
                    else g_extFilter.insert(e.ext);
                    RunSearchNow(g_searchBuf);   // 点一下立刻重搜，不用再按回车
                }
            }
            ImGui::Dummy(ImVec2(0, 10));      // 下方留白
        }
    }

    ImGui::Dummy(ImVec2(0, 2));
    DrawStatusStrip();

    // 面包屑行：无论是否下钻都占同一行，否则点进方块时整块界面会上下跳一格。
    ImGui::Dummy(ImVec2(0, 2));
    {
        const bool canGo = g_current > 0 && !g_entries.empty() && !g_scanning.load();
        std::string crumb = canGo ? PathOf(g_current) : g_rootPath;
        if (crumb.size() > 74) crumb = "…" + crumb.substr(crumb.size() - 73);
        ImGui::PushFont(g_fontSmall, 0.0f);
        ImGui::TextDisabled("%s", crumb.c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 10);
        if (!canGo) ImGui::BeginDisabled();
        if (GhostButton("↑ 上一层", ImVec2(0, 24))) {
            g_current = g_entries[g_current].parent;
            g_expandRestParent = -1;
        }
        ImGui::SameLine(0, 6);
        if (GhostButton("回到根", ImVec2(0, 24))) {
            g_current = 0;
            g_expandRestParent = -1;
        }
        if (!canGo) ImGui::EndDisabled();
    }
    ImGui::Separator();

    float totalWidth = ImGui::GetContentRegionAvail().x;
    float rightWidth = std::max(380.0f, totalWidth * 0.36f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_ui.windowBg);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));

    ImGui::BeginChild("##left", ImVec2(totalWidth - rightWidth - 10, 0), false,
                      ImGuiWindowFlags_NoScrollbar);
    RenderTreemap();
    ImGui::EndChild();

    ImGui::SameLine(0, 10);
    ImGui::PopStyleVar();          // WindowPadding 只给左边那块
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, g_ui.childBg);
    ImGui::PushStyleColor(ImGuiCol_Border, g_ui.border);
    ImGui::BeginChild("##right", ImVec2(rightWidth, 0), true);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();          // ChildRounding
    DrawRightPanel();
    ImGui::EndChild();

    ImGui::End();

    // 扫描结束就顺手算清理项
    if (!g_scanning.load() && g_scanSeconds > 0 && !g_detectStarted) {
        StartDetect();
    }
    // 右键菜单里选了「从这里开始扫描」
    if (!g_pendingScan.empty() && !g_scanning.load()) {
        std::string target = g_pendingScan;
        g_pendingScan.clear();
        StartScan(target);
    }

    // 自动化测试用：扫完立刻跑一次搜索
    // 内容索引在建时不要搜，否则拿到的是半成品
    if (g_pendingContent && !g_scanning.load() && g_scanSeconds > 0 && !g_contentBusy.load()) {
        g_pendingContent = false;
        StartContentIndex();
    }
    if (!g_scanning.load() && !g_contentBusy.load() && g_scanSeconds > 0 &&
        !g_pendingSearch.empty()) {
        std::string q = g_pendingSearch;
        g_pendingSearch.clear();
        snprintf(g_searchBuf, sizeof g_searchBuf, "%s", q.c_str());
        RunSearchNow(q);
        g_mode = 2;
    }
}

// ============================================================ 入口

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int) {
    // 图标来自 app.rc 里嵌入的 1 号资源（assets/icon.ico）
    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!appIcon) appIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, hInstance,
                      appIcon,
                      LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                      nullptr, nullptr, L"SpaceSiftNative", appIcon};
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SpaceSift Native", WS_OVERLAPPEDWINDOW,
                              80, 80, 1320, 880, nullptr, nullptr, wc.hInstance, nullptr);
    g_mainHwnd = hwnd;
    // COM 要在读 shell 目录（SHGetFolderPathW）之前初始化
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_darkTheme = LoadThemeSetting();
    ApplyDarkTitleBar();
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        MessageBoxW(nullptr, L"Direct3D 初始化失败", L"SpaceSift", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;
    // 这是给开发者排查用的浮层，正式版弹出来只会让人以为程序坏了。
    // 方块按钮的 ID 已经改成全局递增，这里只是不再把调试信息糊到界面上。
    io.ConfigDebugHighlightIdConflicts = false;

    ImFontConfig cfg;
    cfg.OversampleH = cfg.OversampleV = 1;
    cfg.PixelSnapH = true;
    // 字号 15，正文；另加一份 12.5 的小字做副标题/状态栏，层级更清楚。
    // 只用常用汉字表：全量字形会把字体纹理撑到上百 MB 显存。
    const ImWchar* ranges = io.Fonts->GetGlyphRangesChineseSimplifiedCommon();
    const char* fonts[] = {"C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
                           "C:\\Windows\\Fonts\\simsun.ttc"};
    const char* chosen = nullptr;
    for (const char* f : fonts) {
        if (GetFileAttributesA(f) == INVALID_FILE_ATTRIBUTES) continue;
        if (io.Fonts->AddFontFromFileTTF(f, 15.0f, &cfg, ranges)) { chosen = f; break; }
    }
    if (!chosen) io.Fonts->AddFontDefault();
    if (chosen) {
        ImFontConfig scfg;
        scfg.OversampleH = scfg.OversampleV = 1;
        scfg.PixelSnapH = true;
        g_fontSmall = io.Fonts->AddFontFromFileTTF(chosen, 12.5f, &scfg, ranges);
    }
    if (!g_fontSmall && !io.Fonts->Fonts.empty()) g_fontSmall = io.Fonts->Fonts[0];

    ApplyTheme();

    ImGuiStyle& style = ImGui::GetStyle();
    (void)style;   // 所有颜色/圆角已由 ApplyTheme 设置

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dContext);

    // 必须用 _wgetenv：getenv 返回的是系统代码页(GBK)字节，
    // 而程序内部全程 UTF-8，中文路径会被打成 "????"。
    // SHOpenFolderAndSelectItems 依赖 COM（已在 CreateWindow 后初始化）

    // 规则表只构造一次：Finding.rule 指向它的元素，之后绝不能再重建 vector。
    g_rules = rules::All();
    // 清掉长期没用到的索引库和上次中断残留的 .building（只动本程序自己的缓存目录）
    db::PruneOld(30);

    if (const wchar_t* r = _wgetenv(L"SPACESIFT_ROOT")) {
        std::string u = WideToUtf8(r);
        snprintf(g_pathBuf, sizeof g_pathBuf, "%s", u.c_str());
    }
    // 必须用 _wgetenv：getenv 拿到的是系统代码页(GBK)字节，中文查询词会变成 "?????"
    if (const wchar_t* q = _wgetenv(L"SPACESIFT_SEARCH")) g_pendingSearch = WideToUtf8(q);
    // 脚本化钩子：SPACESIFT_MODE=content 直接进内容检索模式；
    // SPACESIFT_CONTENT=1 扫完后自动建内容索引（和上面的 SPACESIFT_SEARCH 配套用）
    if (const char* m = std::getenv("SPACESIFT_MODE")) {
        if (std::string(m) == "content") g_searchMode = 1;
    }
    if (std::getenv("SPACESIFT_CONTENT")) g_pendingContent = true;
    // 测试钩子：SPACESIFT_TAB=2 直接打开「检索结果」页签（看建索引进度用）
    if (const char* tab = std::getenv("SPACESIFT_TAB")) g_mode = atoi(tab);
    if (std::getenv("SPACESIFT_EXTPOP")) g_extPopOnce = true;
    if (std::getenv("SPACESIFT_AUTOSCAN")) StartScan(g_pathBuf);

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        DrawUI();
        ImGui::Render();

        const float clear[4] = {0x0f / 255.0f, 0x11 / 255.0f, 0x15 / 255.0f, 1.0f};
        g_pd3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(0, 0);
        Sleep(g_scanning.load() ? 8 : 40);
    }

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
