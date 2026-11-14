// SpaceSift Native v2 —— C++ 重写版
//
// 和 Python 版对齐的功能：扫描 / 方块图下钻 / 三页签右栏（推荐·深度·检索）/
// 清理规则检测 / 清理执行 / 文件名搜索。区别是它只有一个静态链接的 exe。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
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
#include <exdisp.h>

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
#include "ime_win32.h"
#include "selftest.h"
#include "version.h"

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
// 检索范围：false = 只搜当前扫描目录，true = 搜所有建过索引的目录。
// 以前只有前者，换个文件夹就等于"搜不到"，看着像假功能。
// 文件名和内容两种模式共用这一个开关。
static bool g_searchAllRoots = false;
// 内容模式下没索引就搜索时置位，右栏据此显示"先建索引"的引导
static bool g_contentNoIndex = false;
static std::set<std::string> g_extFilter;   // 后缀筛选（空 = 全部）
static std::vector<content::ExtCount> g_extList;   // 弹窗里的后缀列表（缓存）
static bool g_extListDirty = true;
static bool g_extPopOnce = false;   // 测试钩子：启动就打开后缀弹窗

static int  g_current = -1;
static char g_pathBuf[512] = "C:\\";
static bool g_focusSearch = false;
static char g_searchBuf[256] = "";
static int  g_mode = 0;
static std::set<std::string> g_selected;        // 推荐清理：按规则 id
static std::map<std::string, uint64_t> g_deepSelected;   // 深度清理：路径 -> 大小
static std::map<std::string, uint64_t> g_hitSelected;    // 检索结果：路径 -> 大小
static std::string g_lastCleanMsg;
static bool g_revealDenied = false;   // 最近一次「打开所在位置」是不是被权限拦下了

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
static bool PathUnder(const std::string& path, const std::string& dir);
static int  g_cleanOutOfScope = 0;   // 上一次清理被范围校验挡掉的项数
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
        // 暗色：三层灰（页面 / 面板 / 卡片）拉开层次，边框只做极轻的分隔
        t.windowBg      = RGBA(24, 26, 30);
        t.childBg       = RGBA(33, 35, 40);
        t.cardBg        = RGBA(39, 42, 47);
        t.cardHover     = RGBA(48, 51, 57);
        t.cardSelected  = RGBA(40, 56, 80);
        t.border        = RGBA(52, 56, 63);
        t.borderStrong  = RGBA(80, 85, 94);
        t.inputBg       = RGBA(34, 37, 43);
        t.tabBg         = RGBA(40, 43, 49);
        t.text          = RGBA(231, 233, 238);
        t.textDim       = RGBA(163, 169, 178);
        t.textFaint     = RGBA(144, 150, 160);
        t.accent        = RGBA(112, 165, 246);
        t.accentHover   = RGBA(146, 190, 255);
        t.accentText    = RGBA(21, 23, 27);
        t.brand         = RGBA(38, 106, 200);
        t.success       = RGBA(112, 190, 144);
        t.warning       = RGBA(232, 180, 92);
        t.danger        = RGBA(236, 122, 112);
        t.canvas        = RGBA(19, 21, 25);
        t.titleOverlay  = RGBA(0, 0, 0, 0.62f);
        t.sizeOverlay   = RGBA(0, 0, 0, 0.38f);
        t.blockText     = RGBA(233, 235, 240);
        t.blockSubText  = RGBA(196, 200, 208);
        t.blockBorder   = RGBA(0, 0, 0, 0.45f);
        t.fileBlock     = RGBA(58, 62, 70);
        t.dimBg         = RGBA(0, 0, 0, 0.58f);
    } else {
        // 亮色：页面用浅灰，卡片用纯白，边框压到几乎看不见
        t.windowBg      = RGBA(246, 247, 249);
        t.childBg       = RGBA(255, 255, 255);
        t.cardBg        = RGBA(255, 255, 255);
        t.cardHover     = RGBA(241, 244, 248);
        t.cardSelected  = RGBA(229, 239, 254);
        t.border        = RGBA(227, 230, 235);
        t.borderStrong  = RGBA(198, 204, 212);
        t.inputBg       = RGBA(239, 242, 246);
        t.tabBg         = RGBA(240, 243, 247);
        t.text          = RGBA(26, 28, 32);
        t.textDim       = RGBA(78, 84, 94);
        t.textFaint     = RGBA(100, 106, 116);
        t.accent        = RGBA(37, 99, 235);
        t.accentHover   = RGBA(29, 78, 190);
        t.accentText    = RGBA(255, 255, 255);
        t.brand         = RGBA(37, 99, 235);
        t.success       = RGBA(22, 122, 66);
        t.warning       = RGBA(196, 108, 12);
        t.danger        = RGBA(202, 46, 40);
        t.canvas        = RGBA(241, 243, 246);
        t.titleOverlay  = RGBA(255, 255, 255, 0.80f);
        t.sizeOverlay   = RGBA(255, 255, 255, 0.64f);
        t.blockText     = RGBA(26, 28, 32);
        t.blockSubText  = RGBA(88, 94, 103);
        t.blockBorder   = RGBA(0, 0, 0, 0.07f);
        t.fileBlock     = RGBA(214, 218, 224);
        t.dimBg         = RGBA(24, 26, 30, 0.38f);
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

    // 圆角只用两档：控件 8，容器 12。之前 8/9/10/12 混用，一眼能看出拼凑感。
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 12.0f;
    s.FrameRounding     = 8.0f;
    s.PopupRounding     = 10.0f;
    s.ScrollbarRounding = 8.0f;
    s.GrabRounding      = 8.0f;
    s.WindowBorderSize  = 0.0f;
    s.ChildBorderSize   = 1.0f;
    s.FrameBorderSize   = 0.0f;
    s.PopupBorderSize   = 1.0f;
    s.WindowPadding     = ImVec2(20, 16);
    s.FramePadding      = ImVec2(11, 7);
    s.ItemSpacing       = ImVec2(8, 8);
    s.ItemInnerSpacing  = ImVec2(7, 5);
    s.ScrollbarSize     = 10.0f;
    s.GrabMinSize       = 10.0f;
    s.SeparatorTextBorderSize = 1.0f;

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

static bool LoadSearchScopeSetting() {
    wchar_t buf[32] = {0};
    GetPrivateProfileStringW(L"ui", L"search_scope", L"current", buf, 31, SettingsPath().c_str());
    return wcscmp(buf, L"all") == 0;
}

static void SaveSearchScopeSetting() {
    WritePrivateProfileStringW(L"ui", L"search_scope", g_searchAllRoots ? L"all" : L"current",
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
// 调试日志：SPACESIFT_DEBUG_LOG 指定路径时写那个文件；
// 没指定也写一份到 %LOCALAPPDATA%\SpaceSift\reveal.log —— 「打开所在位置没反应」
// 这种问题只能靠日志定位，不能让用户先去设环境变量。
static void RvLog(const char* fmt, ...) {
    static FILE* f = nullptr;
    static bool inited = false;
    if (!inited) {
        inited = true;
        const wchar_t* p = _wgetenv(L"SPACESIFT_DEBUG_LOG");
        if (p && *p) {
            std::wstring wp(p);
            f = _wfopen(wp.c_str(), L"a");
        } else {
            wchar_t base[MAX_PATH] = {0};
            if (GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH) > 0) {
                std::wstring dir = std::wstring(base) + L"\\SpaceSift";
                CreateDirectoryW(dir.c_str(), nullptr);
                std::wstring lp = dir + L"\\reveal.log";
                // 超过 256 KB 就重开，免得越滚越大
                WIN32_FILE_ATTRIBUTE_DATA fad = {};
                if (GetFileAttributesExW(lp.c_str(), GetFileExInfoStandard, &fad) &&
                    fad.nFileSizeLow > 256u * 1024u)
                    _wremove(lp.c_str());
                f = _wfopen(lp.c_str(), L"a");
            }
        }
    }
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

// 进程完整性级别：资源管理器被提权过（High）时，普通权限的程序
// 调 SHOpenFolderAndSelectItems / ShellExecute 都会拿到 0x80070005 或 5。
// 用户看到的就是「点了没反应」，所以失败信息里要把这一点说清楚。
static const char* IntegrityName(HANDLE hProc) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(hProc, TOKEN_QUERY, &tok)) return "?";
    DWORD need = 0;
    GetTokenInformation(tok, TokenIntegrityLevel, nullptr, 0, &need);
    std::vector<unsigned char> buf(need ? need : 64);
    const char* name = "?";
    if (GetTokenInformation(tok, TokenIntegrityLevel, buf.data(), (DWORD)buf.size(), &need)) {
        auto* label = (TOKEN_MANDATORY_LABEL*)buf.data();
        BYTE* sid = (BYTE*)label->Label.Sid;
        DWORD rid = *(DWORD*)(sid + 8 + 4 * (sid[1] - 1));
        switch (rid) {
            case 0x1000: name = "Low"; break;
            case 0x2000: name = "Medium"; break;
            case 0x3000: name = "High"; break;
            case 0x4000: name = "System"; break;
            default: name = "Other"; break;
        }
    }
    CloseHandle(tok);
    return name;
}

static int IntegrityRank(const char* s) {
    if (!s) return 0;
    if (!strcmp(s, "Low")) return 1;
    if (!strcmp(s, "Medium")) return 2;
    if (!strcmp(s, "High")) return 3;
    if (!strcmp(s, "System")) return 4;
    return 0;
}

// 本程序 + 资源管理器各自的完整性级别，用于日志和提示
static void LogIntegrity(const char* where) {
    char self[16] = "?";
    char shell[16] = "?";
    HANDLE me = GetCurrentProcess();
    snprintf(self, sizeof self, "%s", IntegrityName(me));
    DWORD shellPid = 0;
    GetWindowThreadProcessId(GetShellWindow(), &shellPid);
    HANDLE hs = shellPid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, shellPid)
                         : nullptr;
    if (hs) {
        snprintf(shell, sizeof shell, "%s", IntegrityName(hs));
        CloseHandle(hs);
    }
    RvLog("[reveal] %s 本程序=%s 资源管理器=%s", where, self, shell);
}

// 用管理员权限重新启动自己，带上要定位的路径。
// 用户实测「右键以管理员身份运行就正常」，所以把这条路做成一键。
static bool RelaunchElevatedForReveal(const std::wstring& path) {
    wchar_t exe[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    std::wstring args = L"--reveal \"" + path + L"\"";
    HINSTANCE r = ShellExecuteW(nullptr, L"runas", exe, args.c_str(), nullptr, SW_SHOWNORMAL);
    bool ok = (INT_PTR)r > 32;
    RvLog("[reveal] RelaunchElevated ok=%d r=%ld", (int)ok, (long)(INT_PTR)r);
    return ok;
}

// 直接起 explorer.exe（用完整路径），参数原样传。不走 shell 关联、不走 DDE。
static bool SpawnExplorer(const std::wstring& args) {
    wchar_t windir[MAX_PATH] = {0};
    if (!GetWindowsDirectoryW(windir, MAX_PATH)) return false;
    std::wstring exe = std::wstring(windir) + L"\\explorer.exe";
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, nullptr, &si, &pi);
    DWORD err = ok ? 0 : GetLastError();
    if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
    RvLog("[reveal] SpawnExplorer ok=%d err=%lu args=%s", (int)ok, (unsigned long)err, WideToUtf8(args).c_str());
    return ok != 0;
}

// 以管理员身份运行时，本进程和资源管理器不在同一个完整性级别：
// SHOpenFolderAndSelectItems 返回 0x80070005、ShellExecute 返回 5（拒绝访问），
// 直接 CreateProcess 出来的 explorer.exe 又会因为「shell 已经在了」立刻退出。
// 唯一稳的绕法：复制 explorer.exe 的令牌，用它降权起一个 explorer.exe。
static bool SpawnExplorerWithShellToken(const std::wstring& args) {
    HWND shell = GetShellWindow();
    if (!shell) { RvLog("[reveal] shelltoken: 没有 shell 窗口"); return false; }
    DWORD pid = 0;
    GetWindowThreadProcessId(shell, &pid);
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { RvLog("[reveal] shelltoken: OpenProcess err=%lu", (unsigned long)GetLastError()); return false; }

    bool ok = false;
    HANDLE tok = nullptr;
    if (OpenProcessToken(proc, TOKEN_DUPLICATE | TOKEN_QUERY, &tok)) {
        HANDLE dup = nullptr;
        if (DuplicateTokenEx(tok, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                             TokenPrimary, &dup)) {
            wchar_t windir[MAX_PATH] = {0};
            GetWindowsDirectoryW(windir, MAX_PATH);
            std::wstring exe = std::wstring(windir) + L"\\explorer.exe";
            std::wstring cmd = L"\"" + exe + L"\" " + args;
            STARTUPINFOW si = {};
            si.cb = sizeof(si);
            si.wShowWindow = SW_SHOWNORMAL;
            PROCESS_INFORMATION pi = {};
            ok = CreateProcessWithTokenW(dup, LOGON_WITH_PROFILE, exe.c_str(), cmd.data(),
                                         0, nullptr, nullptr, &si, &pi) != 0;
            if (ok) { CloseHandle(pi.hProcess); CloseHandle(pi.hThread); }
            else RvLog("[reveal] shelltoken: CreateProcessWithTokenW err=%lu", (unsigned long)GetLastError());
            CloseHandle(dup);
        } else RvLog("[reveal] shelltoken: DuplicateTokenEx err=%lu", (unsigned long)GetLastError());
        CloseHandle(tok);
    } else RvLog("[reveal] shelltoken: OpenProcessToken err=%lu", (unsigned long)GetLastError());
    CloseHandle(proc);
    RvLog("[reveal] SpawnExplorerWithShellToken ok=%d args=%s", (int)ok, WideToUtf8(args).c_str());
    return ok;
}

// 路径规范化：折叠重复的反斜杠、正斜杠换反斜杠、去掉末尾分隔符。
// 旧索引里存过 "c:\\program files" 这种双反斜杠写法，explorer 会直接跳到「此电脑」。
static std::wstring NormalizePathW(const std::string& utf8) {
    std::wstring in = Utf8ToWide(utf8);
    while (!in.empty() && (in.back() == L' ' || in.back() == L'\t' || in.back() == L'\r' ||
                           in.back() == L'\n'))
        in.pop_back();
    std::wstring out;
    out.reserve(in.size());
    size_t i = 0;
    if (in.size() >= 2 && in[0] == L'\\' && in[1] == L'\\') {   // UNC：保留开头的 \\
        out = L"\\\\";
        i = 2;
    }
    for (; i < in.size(); ++i) {
        wchar_t c = in[i];
        if (c == L'/') c = L'\\';
        if (c == L'\\' && !out.empty() && out.back() == L'\\') continue;
        out.push_back(c);
    }
    if (out.size() > 3 && out.back() == L'\\') out.pop_back();
    return out;
}

static std::string HexDword(DWORD v) {
    char b[16];
    snprintf(b, sizeof b, "0x%08lx", (unsigned long)v);
    return b;
}

// ---- 资源管理器窗口：找到目录对应的窗口并拉到最前 ----
// SHOpenFolderAndSelectItems 返回 S_OK 不代表用户看得见：窗口可能开在程序后面，
// 用户看到的就是「点了没反应」。所以这里显式 SetForegroundWindow。

struct ShellWinSnap {
    HWND hwnd;
    std::wstring url;
};

static std::wstring UrlToPathW(const std::wstring& url) {
    std::wstring u = url;
    if (u.rfind(L"file:///", 0) == 0) u = u.substr(8);
    else if (u.rfind(L"file://", 0) == 0) u = u.substr(7);
    std::string bytes;
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] == L'%' && i + 2 < u.size()) {
            auto hex = [](wchar_t c) -> int {
                if (c >= L'0' && c <= L'9') return (int)(c - L'0');
                if (c >= L'a' && c <= L'f') return (int)(c - L'a') + 10;
                if (c >= L'A' && c <= L'F') return (int)(c - L'A') + 10;
                return -1;
            };
            int h1 = hex(u[i + 1]), h2 = hex(u[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                bytes.push_back((char)(h1 * 16 + h2));
                i += 2;
                continue;
            }
        }
        bytes += WideToUtf8(std::wstring(1, u[i]));
    }
    std::wstring p = Utf8ToWide(bytes);
    std::replace(p.begin(), p.end(), L'/', L'\\');
    return p;
}

static void SnapshotShellWindows(std::vector<ShellWinSnap>& out) {
    out.clear();
    IShellWindows* psw = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_IShellWindows,
                                (void**)&psw)) ||
        !psw)
        return;
    long n = 0;
    if (SUCCEEDED(psw->get_Count(&n))) {
        for (long i = 0; i < n && i < 512; ++i) {
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_I4;
            v.lVal = i;
            IDispatch* pd = nullptr;
            if (FAILED(psw->Item(v, &pd)) || !pd) continue;
            IWebBrowser2* wb = nullptr;
            if (SUCCEEDED(pd->QueryInterface(IID_IWebBrowser2, (void**)&wb)) && wb) {
                SHANDLE_PTR h = 0;
                if (SUCCEEDED(wb->get_HWND(&h)) && h) {
                    ShellWinSnap s;
                    s.hwnd = (HWND)h;
                    BSTR url = nullptr;
                    if (SUCCEEDED(wb->get_LocationURL(&url)) && url) {
                        s.url = url;
                        SysFreeString(url);
                    }
                    out.push_back(s);
                }
                wb->Release();
            }
            pd->Release();
        }
    }
    psw->Release();
}

static bool SamePathW(const std::wstring& a, const std::wstring& b) {
    std::wstring x = a, y = b;
    if (x.size() > 3 && x.back() == L'\\') x.pop_back();
    if (y.size() > 3 && y.back() == L'\\') y.pop_back();
    return _wcsicmp(x.c_str(), y.c_str()) == 0;
}

// before 是调用前的窗口快照：URL 对不上时退而把「新出现的窗口」拉到最前。
// 资源管理器需要几十毫秒才把新窗口注册到 IShellWindows，所以轮询几次。
// tries 次、每次 60ms；找到窗口就立刻返回。
static bool ActivateFolderWindow(const std::wstring& dir, const std::vector<HWND>& before,
                                 int tries = 14) {
    if (dir.empty()) return false;
    std::wstring leaf = dir;
    size_t slash = leaf.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash + 1 < leaf.size()) leaf = leaf.substr(slash + 1);

    HWND target = nullptr;
    for (int attempt = 0; attempt < tries && !target; ++attempt) {
        if (attempt) Sleep(60);
        std::vector<ShellWinSnap> list;
        SnapshotShellWindows(list);

        // 1) URL 对上目录
        for (auto& s : list) {
            if (!s.url.empty() && SamePathW(UrlToPathW(s.url), dir)) { target = s.hwnd; break; }
        }
        // 2) 标题等于目录名（特殊目录 / URL 编码异常的兜底）
        if (!target && !leaf.empty()) {
            for (auto& s : list) {
                wchar_t title[MAX_PATH] = {0};
                GetWindowTextW(s.hwnd, title, MAX_PATH);
                if (title[0] && _wcsicmp(title, leaf.c_str()) == 0) { target = s.hwnd; break; }
            }
        }
        // 3) 实在不行：把「调用前不存在的新窗口」拉出来
        if (!target) {
            for (auto& s : list) {
                bool known = std::find(before.begin(), before.end(), s.hwnd) != before.end();
                if (!known && IsWindowVisible(s.hwnd)) { target = s.hwnd; break; }
            }
        }
        if (attempt == 0) {
            RvLog("[reveal] activate: dir=%s shellwin=%d", WideToUtf8(dir).c_str(), (int)list.size());
            for (size_t i = 0; i < list.size() && i < 6; ++i)
                RvLog("[reveal]   win%02d hwnd=%p url=%s", (int)i, (void*)list[i].hwnd,
                      WideToUtf8(list[i].url).c_str());
        }
    }
    if (!target) {
        RvLog("[reveal] activate: 没有对应窗口 dir=%s", WideToUtf8(dir).c_str());
        return false;
    }

    if (IsIconic(target)) ShowWindow(target, SW_RESTORE);
    SetWindowPos(target, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    AllowSetForegroundWindow(GetWindowThreadProcessId(target, nullptr));
    bool ok = SetForegroundWindow(target) != 0;
    if (!ok) {
        // 前台锁通常只放行「刚被用户操作过的进程」；不行就临时 AttachThreadInput
        DWORD fgTid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
        DWORD myTid = GetCurrentThreadId();
        AttachThreadInput(myTid, fgTid, TRUE);
        ok = SetForegroundWindow(target) != 0;
        AttachThreadInput(myTid, fgTid, FALSE);
    }
    RvLog("[reveal] activate %p ok=%d dir=%s", (void*)target, (int)ok, WideToUtf8(dir).c_str());
    return ok;
}

static void ShellWindowsBefore(std::vector<HWND>& out) {
    std::vector<ShellWinSnap> list;
    SnapshotShellWindows(list);
    for (auto& s : list) out.push_back(s.hwnd);
}

static std::wstring ParentDirW(const std::wstring& p) {
    size_t slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash < 3) return std::wstring();
    return p.substr(0, slash);
}

// 打开一个目录，并且确认用户真的看见了（窗口在前台）。
// 关键：SHOpenFolderAndSelectItems 返回 S_OK 不代表窗口真的出来了 ——
// Windows 上确实会出现「S_OK 但什么也没打开」，所以每级都要验证，
// 验证失败就继续往下走。
//   1) SHOpenFolderAndSelectItems          走 shell 命名空间
//   2) ShellExecuteW(open)                 走 shell 关联
//   2b) ShellExecuteW(explore)             换动词，绕开被改坏的 open 关联
//   3) CreateProcessW(explorer.exe "dir" ) 不和运行中的 explorer 握手
//   4) 用 explorer 令牌降权起 explorer      管理员模式下唯一能过 UIPI 的路
static bool OpenFolderW(const std::wstring& dir, const std::vector<HWND>& before,
                        std::string* diag) {
    char buf[192];

    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(dir.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        snprintf(buf, sizeof buf, "shell=%s ", HexDword((DWORD)hr).c_str());
        if (diag) *diag += buf;
        RvLog("[reveal] openFolder #1 %s", buf);
        if (SUCCEEDED(hr) && ActivateFolderWindow(dir, before)) return true;
        if (SUCCEEDED(hr)) RvLog("[reveal] openFolder #1 S_OK 但没有窗口，继续兜底");
    } else {
        if (diag) *diag += "pidl=null ";
        RvLog("[reveal] openFolder #1 pidl=null err=%lu", (unsigned long)GetLastError());
    }

    INT_PTR r = (INT_PTR)ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    snprintf(buf, sizeof buf, "SE=%ld ", (long)r);
    if (diag) *diag += buf;
    RvLog("[reveal] openFolder #2 %s", buf);
    if (r > 32 && ActivateFolderWindow(dir, before)) return true;

    r = (INT_PTR)ShellExecuteW(nullptr, L"explore", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    snprintf(buf, sizeof buf, "EX=%ld ", (long)r);
    if (diag) *diag += buf;
    RvLog("[reveal] openFolder #2b %s", buf);
    if (r > 32 && ActivateFolderWindow(dir, before)) return true;

    if (SpawnExplorer(L"\"" + dir + L"\"") && ActivateFolderWindow(dir, before, 30)) return true;
    if (SpawnExplorerWithShellToken(L"\"" + dir + L"\"") &&
        ActivateFolderWindow(dir, before, 30))
        return true;
    if (diag) *diag += "CP/TOK=0 ";
    return false;
}

// 在资源管理器里选中一个文件，并确认用户看得见。
// 同样：SHOpenFolderAndSelectItems 可能返回 S_OK 但窗口没出来/没选中。
static bool SelectFileW(const std::wstring& file, const std::wstring& parent,
                        const std::vector<HWND>& before, std::string* diag) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(file.c_str());
    if (pidl) {
        HRESULT hr = SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
        RvLog("[reveal] SHOpenFolderAndSelectItems hr=0x%08lx", (unsigned long)hr);
        if (diag) *diag += "shell=" + HexDword((DWORD)hr) + " ";
        if (SUCCEEDED(hr) && ActivateFolderWindow(parent, before)) return true;
        if (SUCCEEDED(hr)) RvLog("[reveal] 选中文件 S_OK 但没有窗口，继续兜底");
    } else {
        RvLog("[reveal] file ILCreateFromPath failed err=%lu", (unsigned long)GetLastError());
        if (diag) *diag += "pidl=null ";
    }

    // 1b) 不握手：直接起 explorer.exe /select
    std::wstring sel = L"/select,\"" + file + L"\"";
    if (IsUserAnAdmin() && SpawnExplorerWithShellToken(sel) &&
        ActivateFolderWindow(parent, before, 30))
        return true;
    if (SpawnExplorer(sel) && ActivateFolderWindow(parent, before, 30)) return true;
    if (SpawnExplorerWithShellToken(sel) && ActivateFolderWindow(parent, before, 30)) return true;
    return false;
}

static void RevealInExplorer(const std::string& path) {
    if (path.empty()) { g_lastCleanMsg = "路径为空"; return; }
    const std::wstring w = NormalizePathW(path);
    if (w.empty()) { g_lastCleanMsg = "路径为空"; return; }

    DWORD attr = GetFileAttributesW(w.c_str());
    const bool exists = attr != INVALID_FILE_ATTRIBUTES;
    const bool isDir = exists && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool admin = IsUserAnAdmin() != 0;
    RvLog("[reveal] path=%s size=%d attr=0x%lx exists=%d dir=%d admin=%d", WideToUtf8(w).c_str(),
          (int)w.size(), (unsigned long)attr, (int)exists, (int)isDir, (int)admin);

    AllowSetForegroundWindow(ASFW_ANY);
    g_revealDenied = false;

    std::vector<HWND> before;
    ShellWindowsBefore(before);

    const std::wstring parent = isDir ? w : ParentDirW(w);

    // ---- 1) 文件存在：在资源管理器里选中它 ----
    if (exists && !isDir) {
        std::string diag;
        if (SelectFileW(w, parent, before, &diag)) {
            g_lastCleanMsg = "已在资源管理器中定位";
            return;
        }
        if (diag.find("0x80070005") != std::string::npos ||
            diag.find("SE=5") != std::string::npos)
            g_revealDenied = true;
        // 选不中就至少打开所在目录
        std::string d2;
        if (!parent.empty() && OpenFolderW(parent, before, &d2)) {
            g_lastCleanMsg = std::string("已打开所在目录（未能选中文件：") + diag + "）" +
                             (admin ? " 管理员模式" : "");
            return;
        }
        LogIntegrity("失败诊断:");
        g_lastCleanMsg = std::string("权限被拦(0x80070005)：") +
                         (admin ? "以管理员运行时打不开，试试普通权限启动" 
                                : "右键这条 → 以管理员身份打开所在位置");
        return;
    }

    // ---- 2) 本身就是目录 ----
    if (exists && isDir) {
        std::string diag;
        if (OpenFolderW(w, before, &diag)) {
            g_lastCleanMsg = "已打开目录";
        } else {
            if (diag.find("0x80070005") != std::string::npos ||
                diag.find("SE=5") != std::string::npos)
                g_revealDenied = true;
            g_lastCleanMsg = std::string("打开目录失败(") + diag + ")" + (admin ? " 管理员模式" : "");
        }
        return;
    }

    // ---- 3) 文件不在了：打开它所在的目录 ----
    if (!parent.empty() && GetFileAttributesW(parent.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::string diag;
        if (OpenFolderW(parent, before, &diag)) {
            g_lastCleanMsg = "文件已不存在，已打开所在目录";
        } else {
            g_lastCleanMsg = std::string("打开资源管理器失败(") + diag + ")" +
                             (admin ? " 管理员模式" : "");
        }
        return;
    }

    g_lastCleanMsg = "找不到该文件，所在目录也不存在";
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
    if (g_revealDenied && !IsUserAnAdmin()) {
        if (ImGui::MenuItem("以管理员身份打开所在位置")) {
            RelaunchElevatedForReveal(Utf8ToWide(path));
            g_lastCleanMsg = "已请求管理员权限，确认后会自动定位";
        }
    }
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
    int outOfScope = 0;
    auto add = [&](const std::string& p) {
        if (p.empty()) return;
        // 扫描范围外的一律不删：跨目录检索的结果、旧索引里别的目录的记录，
        // 都可能出现在列表里。README 写着"只处理扫描范围内的路径"，得让它成立。
        if (!PathUnder(p, g_rootPath)) { ++outOfScope; return; }
        if (rules::IsProtected(p)) return;
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
    g_cleanOutOfScope = outOfScope;
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
        g_lastCleanMsg = g_cleanOutOfScope > 0
            ? "选中项都不在当前扫描范围内，已全部跳过（先扫描它所在的目录）"
            : "没有可删除的项（可能都不在当前扫描范围内）";
        return;
    }
    if (g_cleanOutOfScope > 0) {
        // 但这句会被"已清理"覆盖，所以真正的说明放在收尾里
        g_lastCleanMsg = "已跳过 " + std::to_string(g_cleanOutOfScope) +
                         " 项范围外的路径，只删当前扫描范围内的";
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
    case WM_GETMINMAXINFO: {
        // 顶栏一行放得下的下限：logo + 磁盘 + 路径(160) + 浏览 + 暂停/结束 +
        // 模式开关 + 搜索框(140) + 搜索 + 主题。再窄就只能互相压住了，
        // 与其画出错位的顶栏，不如不让窗口缩到那一步。
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        const UINT dpi = GetDpiForWindow(hWnd);
        const float s = dpi ? (float)dpi / 96.0f : 1.0f;
        mmi->ptMinTrackSize.x = (LONG)(1120 * s);
        mmi->ptMinTrackSize.y = (LONG)(640 * s);
        return 0;
    }
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

// ---- 方块里的文字 ----------------------------------------------------------
//
// 三条规矩，都是为了"读得出来"：
//   1. 截断必须落在 UTF-8 字符边界上。以前按字节 substr，中文名字被切成半个
//      汉字，显示成 "??" —— 路径里那几个问号就是这么来的。
//   2. 放不下就用省略号，而不是让文字被裁成半个字。
//   3. 窄块给缩写尺寸（169.4 GB → 169G）。36px 宽的竖条塞不下全称，
//      以前干脆不画 —— 用户看到的是"这块没有数字"。

static size_t Utf8Step(const std::string& s, size_t i) {
    if (i >= s.size()) return i;
    const unsigned char c = (unsigned char)s[i];
    size_t len = 1;
    if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    return std::min(s.size(), i + len);
}

static float MeasureText(ImFont* font, float fs, const char* b, const char* e) {
    if (!font || b >= e) return 0.0f;
    return font->CalcTextSizeA(fs, FLT_MAX, 0.0f, b, e).x;
}

// 尾部省略：能放下就原样返回
static std::string FitEnd(ImFont* font, float fs, const std::string& s, float maxW) {
    if (!font || maxW <= 1.0f || s.empty()) return std::string();
    if (MeasureText(font, fs, s.data(), s.data() + s.size()) <= maxW) return s;
    static const char* kDots = "…";
    const float dotsW = MeasureText(font, fs, kDots, kDots + 3);
    if (maxW <= dotsW + 1.0f) return std::string();
    const float budget = maxW - dotsW;
    size_t last = 0, i = 0;
    while (i < s.size()) {
        const size_t next = Utf8Step(s, i);
        if (MeasureText(font, fs, s.data(), s.data() + next) > budget) break;
        last = next;
        i = next;
    }
    if (last == 0) return std::string();
    return s.substr(0, last) + kDots;
}

// 中段省略：保留开头（盘符/上级）和结尾（文件名），中间用 … 顶掉。
// 路径最有信息量的是尾部，但只看尾部会不知道是哪个盘、哪个项目。
static std::string FitMiddle(ImFont* font, float fs, const std::string& s, float maxW) {
    if (!font || maxW <= 1.0f || s.empty()) return std::string();
    if (MeasureText(font, fs, s.data(), s.data() + s.size()) <= maxW) return s;
    static const char* kDots = "…";
    const float dotsW = MeasureText(font, fs, kDots, kDots + 3);
    if (maxW <= dotsW + 8.0f) return FitEnd(font, fs, s, maxW);

    // 尾部从最后一个分隔符往前找，至少保住文件名
    size_t tailStart = s.find_last_of("\\/");
    tailStart = (tailStart == std::string::npos) ? 0 : tailStart;
    // 尾部最多占 70% 宽，剩下留给开头
    while (tailStart > 0 &&
           MeasureText(font, fs, s.data() + tailStart, s.data() + s.size()) > maxW * 0.7f) {
        size_t prev = s.find_last_of("\\/", tailStart - 1);
        if (prev == std::string::npos) { tailStart = 0; break; }
        tailStart = prev;
    }

    std::string tail = s.substr(tailStart);
    if (MeasureText(font, fs, tail.data(), tail.data() + tail.size()) > maxW - dotsW)
        tail = FitEnd(font, fs, tail, maxW - dotsW);   // 文件名本身就太长

    const float tailW = MeasureText(font, fs, tail.data(), tail.data() + tail.size());
    float budget = maxW - dotsW - tailW;
    size_t head = 0, i = 0;
    while (i < tailStart && budget > 0.0f) {
        const size_t next = Utf8Step(s, i);
        if (MeasureText(font, fs, s.data(), s.data() + next) > budget) break;
        head = next;
        i = next;
    }
    if (head == 0) return std::string(kDots) + tail;
    return s.substr(0, head) + kDots + tail;
}

// 窄块用的缩写尺寸：169.4 GB → 169G，1.2 TB → 1.2T
static std::string ShortBytes(uint64_t n) {
    if (n < 1024ull) return std::to_string(n) + "B";
    const char* unit[] = {"K", "M", "G", "T", "P"};
    double v = (double)n / 1024.0;
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    char buf[32];
    if (v < 10.0) snprintf(buf, sizeof buf, "%.1f%s", v, unit[u]);
    else snprintf(buf, sizeof buf, "%d%s", (int)(v + 0.5), unit[u]);
    return buf;
}

// 每个方块的当前矩形：目标由 squarify 算，显示值朝目标插值。
// 只按名字记，换目录后旧条目留着也无害（超过上限整体清掉）。
struct AnimBox { float x = 0, y = 0, w = 0, h = 0; bool init = false; };
static std::unordered_map<std::string, AnimBox> g_boxAnim;

// rootColor：top 层的基色，传给整棵子树。depth == 0 时自己算 TopColor。
static void DrawTreemapImpl(ImDrawList* dl, const std::vector<Blk>& blocks,
                            float x, float y, float w, float h, int depth, ImU32 rootColor,
                            const std::vector<LiveItem>* liveSnap = nullptr) {
    if (blocks.empty() || w < 6 || h < 6) return;
    // 扫描结束就不再做生长动画，直接按真实面积画满
    const bool animate = g_scanning.load();
    // 动画按时间推进，不按帧数：扫描时 8ms 一帧、空闲时 40ms 一帧，
    // 固定系数会让同一段动画一会儿快一会儿慢。
    const float dt = std::min(std::max(ImGui::GetIO().DeltaTime, 0.0f), 0.1f);
    if (depth == 0 && g_boxAnim.size() > 8000) g_boxAnim.clear();

    // 面积用「真实大小 × 动画进度」，这样块是长出来的而不是突然铺满
    std::vector<float> areas;
    std::vector<int> keys;
    for (size_t i = 0; i < blocks.size(); ++i) {
        float v = 1.0f;
        if (animate) {
            std::string key = blocks[i].name + "|" + std::to_string(depth);
            float& a = g_animMap[key];
            if (a < 0.02f) a = 0.02f;
            a += (1.0f - a) * (1.0f - std::exp(-dt * 10.0f));
            v = a;
        }
        areas.push_back((float)std::max<uint64_t>(blocks[i].size, 1) * v);
        keys.push_back((int)i);
    }

    std::vector<Tile> tiles;
    Squarify(areas, keys, 0, 0, w, h, tiles);

    // ---- 卡片排版：标题条 + 底部尺寸条，尺寸和 SpaceSniffer 一个量级
    const float kTitleH = 20.0f;
    const float kSizeH  = 16.0f;
    const float padX    = 5.0f;
    ImFont* fMain = ImGui::GetFont();
    const float fMainSize = ImGui::GetFontSize();
    ImFont* fSmall = g_fontSmall ? g_fontSmall : fMain;
    const float fSmallSize = g_fontSmall ? g_fontSmall->LegacySize : fMainSize - 2.0f;

    const float ease = 1.0f - std::exp(-dt * 16.0f);   // 帧率无关的插值系数

    for (size_t ti = 0; ti < tiles.size(); ++ti) {
        const Tile& t = tiles[ti];
        const Blk& b = blocks[t.index];

        // 目标矩形：留 1px 缝，块之间才有分隔感
        const float tx = x + t.r.x + 1.0f, ty = y + t.r.y + 1.0f;
        const float tw = t.r.w - 2.0f, th = t.r.h - 2.0f;
        if (tw < 2.0f || th < 2.0f) continue;

        // 位置也插值：扫描时新块插入会让整层重排，直接跳过去就是"一格一格蹦"。
        // 同一目录不会有重名子项，(层数, 名字) 足够当动画键。
        const std::string key = std::to_string(depth) + "|" + b.name;
        AnimBox& box = g_boxAnim[key];
        if (!box.init) {
            box.x = tx; box.y = ty; box.w = tw; box.h = th; box.init = true;
        } else {
            box.x += (tx - box.x) * ease;
            box.y += (ty - box.y) * ease;
            box.w += (tw - box.w) * ease;
            box.h += (th - box.h) * ease;
        }
        const ImVec2 a(box.x, box.y);
        const ImVec2 c(box.x + box.w, box.y + box.h);
        const float bw = box.w, bh = box.h;
        if (bw < 2.0f || bh < 2.0f) continue;

        ImU32 color = b.isFile ? ThemeColor(g_ui.fileBlock)
                               : ((depth == 0) ? TopColor(b.name)
                                               : ChildColor(rootColor, (int)ti, depth));
        ImU32 base = (depth == 0 || b.isFile) ? color : rootColor;
        dl->AddRectFilled(a, c, color, 0.0f);            // 直角，和 SpaceSniffer 一致
        dl->AddRect(a, c, ThemeColor(g_ui.blockBorder), 0.0f, 0, 1.0f);   // 1px 描边

        // 文字分三档：宽够 → 标题 + 尺寸；稍窄 → 只留尺寸（竖条也能看到数字）；
        // 再窄 → 只留色块。以前尺寸要求 bw > 54，36px 的竖条永远不显示数字。
        const bool showTitle = bw >= 44.0f && bh >= 24.0f;
        const bool showSize  = bw >= 30.0f &&
                               bh >= (showTitle ? kTitleH + kSizeH + 2.0f : kSizeH + 2.0f);

        if (showTitle) {
            dl->AddRectFilled(a, ImVec2(c.x, a.y + kTitleH), ThemeColor(g_ui.titleOverlay));
            const bool narrow = bw < 96.0f;              // 窄块换小字号，能多塞两个字
            ImFont* f = narrow ? fSmall : fMain;
            const float fs = narrow ? fSmallSize : fMainSize;
            const std::string nm = FitEnd(f, fs, b.name, bw - padX * 2.0f);
            if (!nm.empty())
                dl->AddText(f, fs, ImVec2(a.x + padX, a.y + (kTitleH - fs) * 0.5f - 1.0f),
                            ThemeColor(g_ui.blockText), nm.c_str());
        }

        if (showSize) {
            const float y0 = c.y - kSizeH;
            // 底条从透明渐变到实色，比一刀切的色块柔和
            const ImU32 solid = ThemeColor(g_ui.sizeOverlay);
            dl->AddRectFilledMultiColor(ImVec2(a.x, y0), ImVec2(c.x, c.y),
                                        solid & 0x00FFFFFFu, solid & 0x00FFFFFFu, solid, solid);
            const bool narrow = bw < 66.0f;
            ImFont* f = narrow ? fSmall : fMain;
            const float fs = narrow ? fSmallSize : fMainSize - 1.0f;
            std::string sz = FormatBytes(b.size);
            if (MeasureText(f, fs, sz.data(), sz.data() + sz.size()) > bw - padX * 2.0f)
                sz = ShortBytes(b.size);                 // 全称放不下就给 169G
            sz = FitEnd(f, fs, sz, bw - padX * 2.0f);
            if (!sz.empty())
                dl->AddText(f, fs, ImVec2(a.x + padX, y0 + (kSizeH - fs) * 0.5f),
                            ThemeColor(g_ui.blockSubText), sz.c_str());
        }

        // 递归画下一层：只要这块还够大就继续切，一直到看不出结构为止。
        // 原来写死 depth < 2，点进 Program Files 后 Siemens 36.6 GB 就不再细分了。
        if (depth < 8 && b.idx >= 0 && bw > 64 && bh > 54) {
            std::vector<Blk> children;
            if (liveSnap) GatherLiveChildren(b.idx, children, liveSnap, 6);
            else GatherChildren(b.idx, children, 6);
            float top = showTitle ? kTitleH + 2.0f : 3.0f;
            // 父块底部盖着容量条，子块必须停在它上方，否则容量文字会被子块盖掉
            float bottom = showSize ? kSizeH : 0.0f;
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
                dl->AddRectFilled(a, c, IM_COL32(255, 255, 255, 16));
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

// 跨库检索要挨个打开每一份索引库。实测 27 份库 5.5 秒（C 盘那份 600 万文档占 4.1 秒），
// 以前这段跑在 UI 线程里 —— 切一下「范围」界面就定住几秒，用户只能干等。
// 现在界面只登记请求，后台线程算，结果下一帧取走；新请求用代号作废旧结果。
struct SearchJob {
    uint64_t gen = 0;
    std::string query;
    int  mode = 0;                    // 0 = 文件名，1 = 内容
    bool allRoots = false;
    std::string root;                 // 当前扫描根（内容模式要用）
    std::set<std::string> extFilter;
};

struct SearchOut {
    uint64_t gen = 0;
    std::vector<db::Hit> hits;
    std::string msg;                  // 状态栏那句「已在全部索引里搜到 N 条」
    bool noIndex = false;             // 内容模式：当前目录没索引，右栏要出引导
};

static std::mutex              g_sMx;
static std::condition_variable g_sCv;
static SearchJob               g_sJob;
static bool                    g_sPending = false;
static uint64_t                g_sGen = 0;
static bool                    g_sQuit = false;
static SearchOut               g_sOut;          // 后台写、UI 取
static bool                    g_sOutValid = false;
static std::thread             g_sThread;
static std::atomic<bool>       g_searchBusy{false};
static std::atomic<bool>       g_sAbort{false};   // 关窗口时让后台检索提前收手
static double                  g_sReqsAt = 0;   // 本次请求的提交时刻（显示用时）

// 真正干活的部分：不碰任何界面状态，全部写进 out。
static void DoSearch(const SearchJob& job, SearchOut& out) {
    out.gen = job.gen;
    if (job.query.empty()) return;

    if (job.mode == 1) {
        std::string q = job.query;
        if (!job.extFilter.empty()) {
            q += " ext:";
            bool first = true;
            for (const auto& e : job.extFilter) {
                std::string one = e;
                if (!one.empty() && one[0] == '.') one.erase(one.begin());
                if (!first) q += ",";
                q += one;
                first = false;
            }
        }
        const bool here = content::HasIndex(job.root);
        // 没索引就直接搜 = 静默 0 条，用户只会以为"这功能是假的"。
        // 这里显式区分三种情况，并把用户推到"建立索引"那个按钮上。
        if (!here) {
            if (job.allRoots) {
                out.noIndex = content::IndexSnapshot().empty();
                if (out.noIndex) {
                    out.msg = "还没有任何内容索引：先在右栏点「建立内容索引」";
                    return;
                }
            } else {
                out.noIndex = true;
                out.msg = "「" + job.root + "」还没有内容索引：右栏点「建立内容索引」，"
                          "或把范围切到「所有已索引」";
                return;
            }
        }
        out.noIndex = false;
        if (job.allRoots) {
            out.hits = content::SearchAll(q, 400, &g_sAbort);
            out.msg = out.hits.empty()
                ? "所有已建索引的目录里都没有匹配「" + job.query + "」的内容"
                : "已在全部索引里搜到 " + std::to_string((int)out.hits.size()) + " 条";
        } else {
            out.hits = content::Search(job.root, q, 400, &g_sAbort);
        }
        return;
    }

    // ---- 文件名检索
    if (job.allRoots) {
        out.hits = db::SearchAll(job.query, 400, &g_sAbort);
        if (out.hits.empty())
            out.msg = "所有已建索引的目录里都没有文件名包含「" + job.query + "」的项";
    } else {
        const std::string indexPath = db::IndexPathFor(job.root);
        if (GetFileAttributesW(Utf8ToWide(indexPath).c_str()) == INVALID_FILE_ATTRIBUTES) {
            out.msg = "「" + job.root + "」还没扫描过，没有文件名索引";
            return;
        }
        out.hits = db::SearchIn(indexPath, job.query, 400, &g_sAbort);
    }
    if (!job.extFilter.empty()) {
        auto& hits = out.hits;
        hits.erase(std::remove_if(hits.begin(), hits.end(),
                                  [&](const db::Hit& h) {
                                      std::string e = h.path;
                                      const size_t dot = e.find_last_of('.');
                                      if (dot == std::string::npos) return true;
                                      e = e.substr(dot);
                                      for (auto& c : e) c = (char)tolower((unsigned char)c);
                                      return job.extFilter.count(e) == 0;
                                  }),
                   hits.end());
    }
}

static void SearchWorker() {
    for (;;) {
        SearchJob job;
        {
            std::unique_lock<std::mutex> lk(g_sMx);
            g_sCv.wait(lk, [] { return g_sPending || g_sQuit; });
            if (g_sQuit) return;
            job = g_sJob;
            g_sPending = false;
        }
        SearchOut out;
        DoSearch(job, out);
        if (g_sAbort.load()) return;
        std::lock_guard<std::mutex> lk(g_sMx);
        if (job.gen == g_sGen) {        // 期间没有更新的请求才认这份结果
            g_sOut = std::move(out);
            g_sOutValid = true;
            g_searchBusy = false;
        }
    }
}

// 界面侧：登记一次检索，立刻返回。真正的耗时在 SearchWorker 里。
static void RunSearchNow(const std::string& query) {
    // 扫描线程正在用小 sqlite 连接写索引；这时读到的可能是半成品，
    // 而且 index_*.db 正在被 .building 换名覆盖。
    if (g_scanning.load()) {
        g_lastCleanMsg = "扫描进行中，索引正在重建，等扫描结束后再搜索";
        return;
    }
    g_searchQuery = query;
    g_hits.clear();
    g_contentNoIndex = false;
    if (query.empty()) {
        g_searchBusy = false;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_sMx);
        g_sJob.gen = ++g_sGen;
        g_sJob.query = query;
        g_sJob.mode = g_searchMode;
        g_sJob.allRoots = g_searchAllRoots;
        g_sJob.root = content::NormRoot(g_rootPath);
        g_sJob.extFilter = g_extFilter;
        g_sPending = true;
    }
    g_searchBusy = true;
    g_sReqsAt = ImGui::GetTime();
    if (!g_sThread.joinable()) g_sThread = std::thread(SearchWorker);
    g_sCv.notify_one();
}

// 每帧调一次：后台算完了就把结果搬进 g_hits。
static void PollSearch() {
    SearchOut out;
    {
        std::lock_guard<std::mutex> lk(g_sMx);
        if (!g_sOutValid) return;
        out = std::move(g_sOut);
        g_sOutValid = false;
    }
    if (out.gen != g_sGen) return;      // 已被更新的请求取代，丢掉
    g_hits = std::move(out.hits);
    g_contentNoIndex = out.noIndex;
    if (!out.msg.empty()) g_lastCleanMsg = out.msg;
    g_searchBusy = false;
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
                     float height = 32.0f, float* outWidth = nullptr, bool neutral = false) {
    ImGui::PushID(id);
    float total = SegmentedWidth(labels, count);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p, ImVec2(p.x + total, p.y + height),
                      ThemeColor(g_ui.inputBg), height * 0.5f);
    dl->AddRect(p, ImVec2(p.x + total, p.y + height), ThemeColor(g_ui.border),
                height * 0.5f, 0, 1.0f);

    const float innerH = height - 6.0f;
    float x = p.x + 3.0f;
    for (int i = 0; i < count; ++i) {
        float w = ImGui::CalcTextSize(labels[i]).x + 26.0f;
        ImVec2 a(x, p.y + 3.0f);
        ImVec2 b(x + w, p.y + 3.0f + innerH);
        bool active = (i == current);
        bool hovered = ImGui::IsMouseHoveringRect(a, b) && ImGui::IsWindowHovered();
        if (active) {
            if (neutral) {
                dl->AddRectFilled(a, b, ThemeColor(g_ui.cardSelected), innerH * 0.5f);
            } else {
                ImVec4 pill = g_ui.accent;
                pill.w = g_darkTheme ? 0.22f : 0.16f;
                dl->AddRectFilled(a, b, ThemeColor(pill), innerH * 0.5f);
                ImVec4 edge = g_ui.accent;
                edge.w = 0.55f;
                dl->AddRect(a, b, ThemeColor(edge), innerH * 0.5f, 0, 1.0f);
            }
        } else if (hovered) {
            dl->AddRectFilled(a, b, ThemeColor(g_ui.cardHover), innerH * 0.5f);
        }

        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(labels[i], ImVec2(w, innerH));
        if (ImGui::IsItemClicked()) current = i;

        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const ImVec4 fg = active ? (neutral ? g_ui.text : g_ui.accent)
                                 : (hovered ? g_ui.text : g_ui.textDim);
        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                    ThemeColor(fg), labels[i]);
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
                      : ThemeColor(hovered ? g_ui.accent : g_ui.borderStrong);
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

// 次级按钮：平时无边框无底色，鼠标移上去才浮出一层底色。
// 工具栏里堆一排带框按钮会显得很吵，这样只有「扫描」这种主操作是实心的。
static bool GhostButton(const char* label, const ImVec2& size = ImVec2(0, 0)) {
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_ui.cardHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_ui.cardSelected);
    ImGui::PushStyleColor(ImGuiCol_Border, g_ui.border);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);
    return r;
}

// 文字链：面包屑行里的「上一层 / 回到根」，hover 变强调色并加下划线。
static bool TextLink(const char* label) {
    ImVec2 ts = ImGui::CalcTextSize(label);
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(label, ImVec2(ts.x + 8.0f, ImGui::GetTextLineHeight() + 8.0f));
    const bool hovered = ImGui::IsItemHovered();
    const bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 q(p.x + 4.0f, p.y + (ImGui::GetItemRectSize().y - ts.y) * 0.5f);
    dl->AddText(q, ThemeColor(hovered ? g_ui.accent : g_ui.textDim), label);
    if (hovered)
        dl->AddLine(ImVec2(q.x, q.y + ts.y + 1.0f), ImVec2(q.x + ts.x, q.y + ts.y + 1.0f),
                    ThemeColor(g_ui.accent), 1.0f);
    return clicked;
}

// 右栏页签：下划线式。比胶囊更安静，也不会和「扫描」抢视线。
static int TabBar(const char* id, const char* const* labels, int count, int current,
                  float* outWidth = nullptr) {
    ImGui::PushID(id);
    const float h = 34.0f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    float fullW = ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(p.x, p.y + h - 1), ImVec2(p.x + fullW, p.y + h - 1),
                ThemeColor(g_ui.border), 1.0f);

    float x = p.x;
    for (int i = 0; i < count; ++i) {
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        const float w = ts.x + 20.0f;
        ImVec2 a(x, p.y);
        ImVec2 b(x + w, p.y + h - 1);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton(labels[i], ImVec2(w, h - 1));
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) current = i;
        const bool active = (i == current);

        dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, a.y + (h - 1 - ts.y) * 0.5f),
                    ThemeColor(active ? g_ui.text : (hovered ? g_ui.text : g_ui.textDim)),
                    labels[i]);
        if (active)
            dl->AddRectFilled(ImVec2(a.x + 6, b.y - 2.0f), ImVec2(b.x - 6, b.y + 2.0f),
                              ThemeColor(g_ui.accent), 2.0f);
        x += w;
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h));
    ImGui::Dummy(ImVec2(fullW, 0.0f));
    ImGui::PopID();
    if (outWidth) *outWidth = x - p.x;
    return current;
}

// 1px 竖向分隔线，用来把工具栏分组。
static void ToolbarDivider(float height = 20.0f) {
    ImGui::SameLine(0, 10);
    ImVec2 p = ImGui::GetCursorScreenPos();
    float y0 = p.y + (ImGui::GetFrameHeight() - height) * 0.5f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, y0), ImVec2(p.x, y0 + height),
                                        ThemeColor(g_ui.border), 1.0f);
    ImGui::Dummy(ImVec2(1.0f, height));
    ImGui::SameLine(0, 10);
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

// 空状态：居中的一句标题 + 一行说明。比在左上角丢一句灰字更像是"设计过的"。
static void EmptyState(const char* title, const char* body = nullptr) {
    const float availY = ImGui::GetContentRegionAvail().y;
    ImGui::Dummy(ImVec2(0, std::max(20.0f, availY * 0.22f)));
    const float w = ImGui::GetContentRegionAvail().x;
    const float pad = 24.0f;

    ImVec2 ts = ImGui::CalcTextSize(title);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (w - ts.x) * 0.5f));
    ImGui::TextColored(g_ui.textDim, "%s", title);

    if (body && body[0]) {
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PushFont(g_fontSmall, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Text, g_ui.textFaint);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + pad);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::max(120.0f, w - pad * 2));
        ImGui::TextWrapped("%s", body);
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
}

// ============================================================ 右栏卡片行

// 一行圆角卡片：左侧复选框 + 标题（+ 可选徽标 / 副标题），右侧容量。
// 返回 true 表示整行被左键点到（用于下钻）。
//
// 关键点：先放一个透明 InvisibleButton 当整行点击区，再 SetItemAllowOverlap()
// 让复选框盖在上面 —— 这样点勾选框只切换勾选，点别处才触发整行。
static bool RowCard(const char* id, const std::string& title, const std::string& right,
                    bool* checked, const char* badge, const std::string& tooltip,
                    const std::string& subtitle = "", bool checkable = true) {
    ImGui::PushID(id);

    const float rowH = 38.0f;
    const float pad = 8.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    if (avail < 80.0f) avail = 80.0f;
    ImVec2 p0 = ImGui::GetCursorScreenPos();

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##row", ImVec2(avail, rowH));
    bool hovered = ImGui::IsItemHovered();
    bool rowClicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    bool rowRightClicked = ImGui::IsItemClicked(ImGuiMouseButton_Right);

    // 默认不画底和框：列表靠 hover 浮出底色来分辨，一屏十几行不会被框线切碎。
    bool selected = checked && *checked;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + rowH),
                          ThemeColor(g_ui.cardSelected), 8.0f);
        dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 10), ImVec2(p0.x + 4.5f, p0.y + rowH - 10),
                          ThemeColor(g_ui.accent), 1.5f);
    } else if (hovered) {
        dl->AddRectFilled(p0, ImVec2(p0.x + avail, p0.y + rowH),
                          ThemeColor(g_ui.cardHover), 8.0f);
    }

    // 自绘勾选框
    ImGui::SetCursorScreenPos(ImVec2(p0.x + pad, p0.y + (rowH - 16.0f) * 0.5f));
    if (checkable) {
        ThemeCheckbox("##chk", checked, 16.0f);
    } else {
        // 不在当前扫描范围内：不给勾选框，画一把小锁，点了也不加入待清理
        ImGui::Dummy(ImVec2(16.0f, 16.0f));
        ImDrawList* l = ImGui::GetWindowDrawList();
        const ImVec2 q(p0.x + pad, p0.y + (rowH - 16.0f) * 0.5f);
        l->AddRect(ImVec2(q.x + 3, q.y + 8), ImVec2(q.x + 13, q.y + 15),
                   ThemeColor(g_ui.textFaint), 2.0f, 0, 1.2f);
        l->AddBezierCubic(ImVec2(q.x + 5, q.y + 8), ImVec2(q.x + 5, q.y + 3),
                          ImVec2(q.x + 11, q.y + 3), ImVec2(q.x + 11, q.y + 8),
                          ThemeColor(g_ui.textFaint), 1.2f);
        rowClicked = false;
    }

    float textY = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    std::string rs = right;
    float rsW = 0.0f;
    {
        ImGui::PushFont(g_fontSmall, 0.0f);
        rsW = ImGui::CalcTextSize(rs.c_str()).x;
        ImGui::PopFont();
    }
    float rightX = p0.x + avail - rsW - pad - 4.0f;
    float textX = p0.x + pad + 26.0f;
    float clipMax = std::max(textX + 1.0f, rightX - 8.0f);

    // 标题 + 徽标 + 副标题；整组裁剪到右侧容量文字左边
    ImGui::PushClipRect(ImVec2(textX, p0.y), ImVec2(clipMax, p0.y + rowH), true);
    float cx = textX;
    ImGui::SetCursorScreenPos(ImVec2(cx, textY));
    ImGui::TextUnformatted(title.c_str());
    cx += ImGui::CalcTextSize(title.c_str()).x;
    if (badge && badge[0]) {
        // 放不下就干脆不画：画一半被裁掉比不画更难看（长文件名时常见）
        const float bw = ImGui::CalcTextSize(badge).x;
        if (cx + 8.0f + bw <= clipMax) {
            cx += 8.0f;
            ImGui::SetCursorScreenPos(ImVec2(cx, textY));
            ImGui::TextColored(g_ui.warning, "%s", badge);
            cx += bw;
        }
    }
    if (!subtitle.empty() && cx + 12 < clipMax) {
        ImGui::PushFont(g_fontSmall, 0.0f);
        float sy = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(cx + 10, sy));
        ImGui::TextColored(g_ui.textFaint, "%s", subtitle.c_str());
        ImGui::PopFont();
    }
    ImGui::PopClipRect();

    // 右侧容量：小字灰色。整列都绿会让"大小"抢走标题的注意力。
    {
        ImGui::PushFont(g_fontSmall, 0.0f);
        float sy = p0.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        ImGui::SetCursorScreenPos(ImVec2(rightX, sy));
        ImGui::TextColored(selected ? g_ui.text : g_ui.textDim, "%s", rs.c_str());
        ImGui::PopFont();
    }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + rowH));
    ImGui::Dummy(ImVec2(avail, 2.0f));

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

// 范围切换：当前扫描目录 / 所有建过索引的目录。
// 这是"换个文件夹就搜不到"的正解 —— 索引还在，只是要看哪一份。
static void DrawSearchScopeSwitch() {
    static const char* kScopes[] = {"当前目录", "所有已索引"};
    ImGui::PushFont(g_fontSmall);
    ImGui::TextDisabled("范围");
    ImGui::PopFont();
    ImGui::SameLine(0, 8);
    const int cur = g_searchAllRoots ? 1 : 0;
    const int next = Segmented("cscope", kScopes, 2, cur, 26.0f);
    if (next != cur) {
        g_searchAllRoots = (next == 1);
        SaveSearchScopeSetting();
        g_extListDirty = true;
        if (g_searchMode == 1 && g_searchBuf[0]) {
            RunSearchNow(g_searchBuf);
            if (!g_contentNoIndex) g_mode = 2;
        }
    }
}

// 索引管理：看得见每份库占了多少、什么时候建的，也能删。
// 不提供这个入口的话，%LOCALAPPDATA% 里能悄悄堆出好几个 GB ——
// 一个清理磁盘的工具自己占着系统盘，说不过去。
static void DrawIndexManager() {
    // 走缓存：ListIndexes 要逐份打开库读 meta（27 份 ≈ 0.7 秒），
    // 放绘制路径里就是每 2 秒卡 0.7 秒。建库/删库会把它标脏，这里自然重新读。
    const std::vector<content::IndexInfo> list = content::IndexSnapshot();

    uint64_t total = 0;
    for (const auto& i : list) total += i.bytes;
    char head[128];
    snprintf(head, sizeof head, "索引管理 · %d 份 · %s", (int)list.size(),
             FormatBytes(total).c_str());
    if (!ImGui::CollapsingHeader(head)) return;
    if (list.empty()) {
        ImGui::TextDisabled("还没有任何内容索引。");
        return;
    }

    ImGui::PushFont(g_fontSmall);
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& idx = list[i];
        ImGui::PushID((int)i);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(idx.root.c_str());
        ImGui::SameLine();
        char meta[160];
        snprintf(meta, sizeof meta, "  %lld 个文档 · %s%s%s", (long long)idx.docs,
                 FormatBytes(idx.bytes).c_str(),
                 idx.partial ? " · 未建完" : "", idx.stale ? " · 需重建" : "");
        ImGui::TextDisabled("%s", meta);
        ImGui::SameLine();
        // 建索引时删库会把 SQLite 连接从底下抽掉，直接禁用
        const bool busy = g_contentBusy.load();
        if (busy) ImGui::BeginDisabled();
        if (GhostButton("删除", ImVec2(48, 22))) {
            if (content::DeleteIndexFile(idx.dbPath)) {
                g_lastCleanMsg = "已删除索引：" + idx.root;
                g_extListDirty = true;
                content::InvalidateIndexes();   // 下一帧重读，别把删掉的还列着
            }
        }
        if (busy) ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::PopFont();
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

    // 建索引刚开始的这一刻记时；结束/重新开始时复位
    static bool   indexWasBusy = false;
    static double indexStartedAt = 0.0;
    if (!busyNow) indexWasBusy = false;

    if (busyNow) {
        const int total  = content::g_seen.load();
        const int done   = content::g_done.load();
        const int fresh  = content::g_parsed.load();
        const int reused = content::g_skipped.load();
        const int pass   = content::g_passes.load();   // 0=清点 1=抽取

        // 这一轮从什么时候开始的：算速度、算剩余时间都靠它
        if (!indexWasBusy) {
            indexStartedAt = ImGui::GetTime();
            indexWasBusy = true;
        }
        const double elapsed = ImGui::GetTime() - indexStartedAt;
        char eta[64] = "";

        // 阶段标题 + 剩余时间（同一行，右边对齐）
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(pass == 0 ? "① 清点文件" : "② 抽取正文");
        ImGui::SameLine();
        {
            const float avail = ImGui::GetContentRegionAvail().x;
            const float tw = ImGui::CalcTextSize("约剩 -- 分 -- 秒").x;
            if (avail > tw) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - tw);
            if (pass == 1 && total > 0 && done > 1 && elapsed > 1.0) {
                const int remain = (int)((double)(total - done) * elapsed / (double)done);
                if (remain > 3600)
                    snprintf(eta, sizeof eta, "约剩 %d 时 %02d 分", remain / 3600, (remain / 60) % 60);
                else
                    snprintf(eta, sizeof eta, "约剩 %d 分 %02d 秒", remain / 60, remain % 60);
                ImGui::TextDisabled("%s", eta);
            } else {
                ImGui::TextDisabled(pass == 0 ? "统计可抽正文的文件" : "");
            }
        }

        // 进度条：清点阶段还没有总量，就画不确定进度
        if (pass == 1 && total > 0) {
            const float frac = (float)done / (float)total;
            char ov[64];
            snprintf(ov, sizeof ov, "%.0f%%", frac * 100.0f);
            ImGui::ProgressBar(frac, ImVec2(-1, 10), ov);
        } else {
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 10), "");
        }

        // 计数：两行等宽，数字用等宽排列，扫一眼就知道在动
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::PushFont(g_fontSmall, 0.0f);
        if (pass == 1 && total > 0) {
            ImGui::TextDisabled("已处理 %d / %d     新抽取 %d · 复用 %d",
                                done, total, fresh, reused);
        } else {
            ImGui::TextDisabled("已清点 %d 个候选     已抽正文 %d 个", total, fresh);
        }
        ImGui::PopFont();

        // 当前文件：中间省略，保住盘符和文件名；悬停看全路径
        {
            const std::string cur = content::CurrentPath();
            if (!cur.empty()) {
                const float maxW = ImGui::GetContentRegionAvail().x;
                const std::string shown = FitMiddle(g_fontSmall, 12.0f, cur, maxW);
                ImGui::PushFont(g_fontSmall, 0.0f);
                ImGui::TextDisabled("%s", shown.c_str());
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", cur.c_str());
                ImGui::PopFont();
            }
        }

        // 最近抽取：让这块空间告诉我们"它具体在做什么"，
        // 而不是几行字顶在最上面、下面一大片空白。
        {
            const auto recent = content::RecentFiles();
            if (!recent.empty()) {
                ImGui::Dummy(ImVec2(0, 8));
                ImGui::PushFont(g_fontSmall, 0.0f);
                ImGui::TextDisabled("最近抽取");
                const float rowW = ImGui::GetContentRegionAvail().x;
                const float x0 = ImGui::GetCursorPosX();
                // 按剩余高度决定列几行，把这面板的空白用起来
                const float lineH = ImGui::GetTextLineHeightWithSpacing();
                const int maxRows = std::max(0, (int)((ImGui::GetContentRegionAvail().y - 46.0f) / lineH));
                int shown = 0;
                for (int i = (int)recent.size() - 1; i >= 0 && shown < maxRows; --i, ++shown) {
                    const auto& rf = recent[(size_t)i];
                    const std::string sz = FormatBytes(rf.size);
                    const float szW = ImGui::CalcTextSize(sz.c_str()).x;
                    const float nameW = std::max(40.0f, rowW - szW - 10.0f);
                    const std::string nm = FitEnd(ImGui::GetFont(), ImGui::GetFontSize(),
                                                  rf.name, nameW);
                    ImGui::PushStyleColor(ImGuiCol_Text,
                                          ThemeColor(rf.ok ? g_ui.textDim : g_ui.textFaint));
                    ImGui::TextUnformatted(nm.c_str());
                    ImGui::SameLine(x0 + std::max(0.0f, rowW - szW));
                    ImGui::TextUnformatted(sz.c_str());
                    ImGui::PopStyleColor();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rf.path.c_str());
                }
                ImGui::PopFont();
            }
        }

        ImGui::Dummy(ImVec2(0, 6));
        if (GhostButton("停止", ImVec2(64, 26))) g_contentCancel = true;
        ImGui::SameLine();
        ImGui::PushFont(g_fontSmall, 0.0f);
        ImGui::TextDisabled("停下的部分会保留，下次点「增量更新」接着做");
        ImGui::PopFont();
        return;
    }

    const std::string root = content::NormRoot(g_rootPath);
    const bool hasHere = content::HasIndex(root);

    DrawSearchScopeSwitch();
    ImGui::Dummy(ImVec2(0, 2));

    // 用户在没索引的情况下搜过东西：把原因摆在他面前，而不是只显示"0 条"
    if (g_contentNoIndex) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.warning));
        ImGui::TextWrapped("「%s」还没有内容索引，所以刚才那次搜索什么也没返回。",
                           root.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 2));
    }

    // 这份清单走缓存：以前每帧 ListIndexes()（0.7 秒），界面直接僵住
    const auto idxList = content::IndexSnapshot();
    if (!hasHere && !(g_searchAllRoots && !idxList.empty())) {
        ImGui::TextDisabled(g_searchAllRoots ? "还没有任何内容索引。"
                                              : "当前目录还没有内容索引。");
        ImGui::TextDisabled("只抽文本/代码、Office、PDF、图纸、三维模型；"
                            "建一次，之后按大小和时间增量更新。");
        ImGui::Dummy(ImVec2(0, 6));
        const bool blocked = g_scanning.load();
        if (blocked) ImGui::BeginDisabled();
        if (PrimaryButton("建立内容索引", ImVec2(0, 30))) StartContentIndex();
        if (blocked) ImGui::EndDisabled();
        if (blocked) ImGui::TextDisabled("扫描进行中，等扫描结束后再建。");
        std::string err = content::LastError();
        if (!err.empty()) ImGui::TextDisabled("%s", err.c_str());

        // 缺依赖时先说清楚，别等用户搜不到 PDF 才猜
        {
            int miss = 0;
            if (!extract::PdfReady()) ++miss;
            if (!extract::DwgReady()) ++miss;
            if (!extract::OcrAvailable()) ++miss;
            if (miss > 0) {
                ImGui::Dummy(ImVec2(0, 4));
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
                ImGui::TextWrapped("当前缺 %d 项可选依赖：PDF/DWG/OCR 里对应格式搜不到，"
                                   "其余格式不受影响（详见 USAGE.md）。", miss);
                ImGui::PopStyleColor();
            }
        }
        ImGui::Dummy(ImVec2(0, 8));
        DrawIndexManager();
        return;
    }

    static int lastFresh = -1, lastReused = -1;
    if (lastFresh != content::g_parsed.load() || lastReused != content::g_skipped.load()) {
        lastFresh = content::g_parsed.load();
        lastReused = content::g_skipped.load();
    }
    const std::string notice = content::LastNotice();
    if (!notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
        ImGui::TextWrapped("%s", notice.c_str());
        ImGui::PopStyleColor();
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
            ImGui::TextDisabled("提示：.doc/.ppt/.xls 走原生解析；装 catdoc/LibreOffice 对复杂文件更稳。");
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
        ImGui::Dummy(ImVec2(0, 8));
        DrawIndexManager();
        return;
    }
    if (g_searchBusy.load()) {
        ImGui::TextDisabled("正在搜索…");
        ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 6), "");
        ImGui::TextDisabled(g_searchAllRoots ? "逐个查所有已建索引的目录，先出结果先算完"
                                             : "正在查当前目录的内容索引");
        return;
    }
    if (g_hits.empty()) {
        ImGui::TextDisabled("没有文件内容匹配「%s」。", g_searchQuery.c_str());
        if (!g_searchAllRoots) {
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
            ImGui::TextWrapped("想连之前扫过的其他目录一起搜，点上面的「所有已索引」。");
            ImGui::PopStyleColor();
        }
        return;
    }
    {
        int outCount = 0;
        for (const auto& h : g_hits) if (!InCurrentDir(h.path)) ++outCount;
        ImGui::TextDisabled("内容命中 %d 项（按相关度排序，勾选可加入待清理）", (int)g_hits.size());
        if (outCount > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
            ImGui::TextWrapped("%d 项在扫描范围外（带锁，不可勾选），扫它所在目录后可清理",
                               outCount);
            ImGui::PopStyleColor();
        }
    }
    for (auto& hit : g_hits) {
        const bool inScope = InCurrentDir(hit.path);
        bool was = g_hitSelected.count(hit.path) > 0;
        bool now = was;
        std::string sz = FormatBytes(hit.size);
        RowCard(hit.path.c_str(), hit.name, sz, &now, inScope ? "" : "范围外",
                inScope ? hit.path : hit.path + "\n不在当前扫描范围内，勾选与清理会被跳过",
                hit.snippet.empty() ? ShortParent(hit.path) : hit.snippet, inScope);
        if (!inScope) continue;
        if (now != was) {
            if (now) g_hitSelected[hit.path] = hit.size;
            else g_hitSelected.erase(hit.path);
        }
    }
}

static void DrawRightPanel() {
    // ---- 标签行：三个页签在左，两个批量按钮贴右
    static const char* tabLabels[] = {"推荐清理", "深度清理", "检索结果"};
    int nextTab = TabBar("tabs", tabLabels, 3, g_mode);
    if (nextTab != g_mode) g_mode = nextTab;

    // 批量操作：右对齐的小按钮，单独一行，不和页签挤在一起
    float rightEdge = ImGui::GetWindowContentRegionMax().x;
    const float kBatchW = 72.0f + 6.0f + 66.0f;
    float batchX = rightEdge - kBatchW;
    if (batchX > 8.0f) ImGui::SameLine(batchX);
    else ImGui::SetCursorPosX(std::max(0.0f, batchX));
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
    if (GhostButton("全选安全项", ImVec2(72, 26))) {
        std::lock_guard<std::mutex> lk(g_findMutex);
        for (auto& f : g_findings) {
            if (!f.rule || f.rule->risk != rules::SAFE || f.size == 0) continue;
            for (auto& p : f.existing)
                if (InCurrentDir(p)) { g_selected.insert(f.rule->id); break; }
        }
    }
    ImGui::SameLine(0, 6);
    if (GhostButton("取消全选", ImVec2(66, 26))) g_selected.clear();
    if (!hasSafe) ImGui::EndDisabled();
    ImGui::Dummy(ImVec2(0, 2));

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
                EmptyState("还没有检查", "扫描一个目录后，这里会列出当前目录下可清理的缓存项。");
            } else if (!g_detecting.load()) {
                char body[256];
                if (g_currentFileTotal > 0)
                    snprintf(body, sizeof body,
                             "缓存规则只覆盖系统和应用的缓存目录。当前目录下还有 %d 个直接文件，"
                             "可以在「深度清理」里看。",
                             g_currentFileTotal);
                else
                    snprintf(body, sizeof body, "缓存规则只覆盖系统和应用的缓存目录。");
                EmptyState("这里没有可清理的缓存项", body);
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
            EmptyState("正在扫描", "扫完这里会列出当前目录下的每一项。");
        } else if (g_current < 0 || g_entries.empty()) {
            EmptyState("还没有内容", "先在上面选一个目录扫描，这里会按大小列出子项。");
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
                const std::string rowTitle = c.isFile ? c.name : (c.name + "/");
                bool goIn = RowCard(rowId.c_str(), rowTitle, sz, &now, "", path);
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
        DrawSearchScopeSwitch();
        if (g_searchBusy.load() && !g_searchQuery.empty()) {
            ImGui::TextDisabled("正在搜索…");
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(-1, 6), "");
            ImGui::TextDisabled(g_searchAllRoots ? "逐个查所有已建索引的目录"
                                                 : "正在查当前目录的文件名索引");
        } else if (g_searchQuery.empty()) {
            EmptyState("还没有搜索", "在上面搜索框输入文件名，回车或点搜索。Ctrl+F 可以直接跳过去。");
        } else if (g_hits.empty()) {
            EmptyState("没有命中", g_searchAllRoots
                       ? "所有已建索引的目录里都没有匹配的文件名。"
                       : "换个关键词试试，或者把上面的范围切成「所有已索引」。");
        } else {
            int outCount = 0;
            for (const auto& h : g_hits) if (!InCurrentDir(h.path)) ++outCount;
            ImGui::TextDisabled("命中 %d 项（按大小排序，勾选可加入待清理）",
                                (int)g_hits.size());
            if (outCount > 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, ThemeColor(g_ui.textDim));
                ImGui::TextWrapped("%d 项在扫描范围外（带锁，不可勾选），扫它所在目录后可清理",
                                   outCount);
                ImGui::PopStyleColor();
            }
            for (auto& hit : g_hits) {
                const bool inScope = InCurrentDir(hit.path);
                bool was = g_hitSelected.count(hit.path) > 0;
                bool now = was;
                std::string sz = FormatBytes(hit.size);
                // 标题用文件名；同名文件太多，用父路径末尾区分
                RowCard(hit.path.c_str(), hit.name, sz, &now, inScope ? "" : "范围外",
                        inScope ? hit.path : hit.path + "\n不在当前扫描范围内，勾选与清理会被跳过",
                        ShortParent(hit.path), inScope);
                if (!inScope) continue;
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

    // 底部操作条：上面一条发丝线，左边选中量，右边主按钮
    {
        ImVec2 fp = ImGui::GetCursorScreenPos();
        float fw = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(fp.x, fp.y + 2), ImVec2(fp.x + fw, fp.y + 2),
                                            ThemeColor(g_ui.border), 1.0f);
    }
    ImGui::Dummy(ImVec2(0, 4));

    std::string selSz = FormatBytes(selectedBytes);
    ImGui::TextColored(selectedCount > 0 ? g_ui.text : g_ui.textDim, "%s", selSz.c_str());
    ImGui::SameLine();
    ImGui::PushFont(g_fontSmall, 0.0f);
    ImGui::TextDisabled("%d 项", selectedCount);
    ImGui::PopFont();
    ImGui::SameLine(std::max(180.0f, ImGui::GetWindowContentRegionMax().x - 104.0f));

    const bool canClean = selectedCount > 0 && !g_cleaning.load();
    if (!canClean) {
        ImGui::BeginDisabled();
        GhostButton(g_cleaning.load() ? "清理中" : "清理选中项", ImVec2(100, 30));
        ImGui::EndDisabled();
    } else if (PrimaryButton(g_cleaning.load() ? "清理中" : "清理选中项", ImVec2(100, 30))) {
        // 任何删除都要先弹窗确认 —— 这一步不可撤销，不给用户留反悔机会
        ImGui::OpenPopup("##confirmClean");
    }
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));

    if (ImGui::BeginPopupModal("##confirmClean", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        std::vector<rules::Finding> fsnap;
        {
            std::lock_guard<std::mutex> lk(g_findMutex);
            fsnap = g_findings;
        }
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextUnformatted("确认删除这些内容");
        ImGui::Dummy(ImVec2(0, 2));
        ImGui::TextDisabled("推荐清理 %d 项 · 深度勾选 %d 项 · 检索勾选 %d 项 · 合计 %s",
                            (int)g_selected.size(), (int)g_deepSelected.size(),
                            (int)g_hitSelected.size(), selSz.c_str());
        ImGui::Dummy(ImVec2(0, 8));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, g_ui.inputBg);
        ImGui::PushStyleColor(ImGuiCol_Border, g_ui.border);
        ImGui::BeginChild("##cleanlist", ImVec2(500, 152), true);
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
            ImGui::TextDisabled("还有 %d 项未列出", selectedCount - kMaxShown);
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
        ImGui::Dummy(ImVec2(0, 12));
        ImGui::TextColored(g_ui.danger, "直接删除，不进回收站，无法撤销。");
        ImGui::Dummy(ImVec2(0, 8));

        // 按钮右对齐：取消在左，确认在右（危险操作放最后）
        const float btnRow = ImGui::GetContentRegionMax().x - (92.0f + 8.0f + 104.0f);
        if (btnRow > 0) ImGui::SetCursorPosX(btnRow);
        if (GhostButton("取消", ImVec2(92, 30))) ImGui::CloseCurrentPopup();
        ImGui::SameLine(0, 8);
        if (DangerButton("确认删除", ImVec2(104, 30))) {
            StartClean();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
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
    // 内容索引和扫描互斥：新扫描开始先把索引线程叫停。
    // 但别默默掐掉 —— 用户会看到进度条突然消失，以为程序坏了。
    if (g_contentBusy.load()) {
        g_contentCancel = true;
        g_lastCleanMsg = "开始扫描，内容索引已停止（已完成的部分保留，稍后可「增量更新」）";
    }
    g_contentNoIndex = false;
    // 新扫描一开始，旧检测的结果全部作废（旧线程即使还在跑也写不进界面）
    RequestDetectCancel();
    ++g_detectGen;
    g_detecting = false;
    g_detectStarted = false;
    ClearDetectMsg();
    // 统一斜杠：输入框里留着 "E:/" 的话，面包屑会拼出 "E:/\consol" 这种怪东西，
    // 索引库也会把同一个目录存成两种 path。扫描入口归一化一次，后面全都干净。
    const std::string norm = content::NormRoot(root);
    g_rootPath = norm;
    snprintf(g_pathBuf, sizeof g_pathBuf, "%s", norm.c_str());
    ProbeVolume(norm);
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
    const char* sub = "磁盘空间分析  v" SPACESIFT_VERSION;
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
            snprintf(left, sizeof left, "%.0f%%   %s / %s", pct * 100.0f,
                     FormatBytes(scanned).c_str(), FormatBytes(denom).c_str());
        else
            snprintf(left, sizeof left, "已扫 %s · %lld 文件",
                     FormatBytes(scanned).c_str(), (long long)g_fileCount.load());
    } else if (g_scanSeconds > 0) {
        snprintf(left, sizeof left, "%lld 文件 · %s · %.1f 秒",
                 (long long)g_fileCount.load(), FormatBytes(scanned).c_str(), g_scanSeconds);
    } else {
        snprintf(left, sizeof left, "就绪 · 选一个位置开始");
    }

    std::string right;
    ImVec4 rightColor = g_ui.textFaint;
    bool warn = false;
    if (g_cleaning.load()) {
        right = "正在清理 " + std::to_string(g_cleanDone.load()) + "/" +
                std::to_string(g_cleanTotal.load());
        rightColor = g_ui.warning;
        warn = true;
    } else if (!g_lastCleanMsg.empty()) {
        static std::string shownMsg;
        static double shownAt = 0.0;
        if (shownMsg != g_lastCleanMsg) {
            shownMsg = g_lastCleanMsg;
            shownAt = ImGui::GetTime();
        }
        if (ImGui::GetTime() - shownAt < 6.0) {
            right = g_lastCleanMsg;
            rightColor = g_ui.textDim;
        }
    } else if (scanning) {
        std::string cur;
        {
            std::lock_guard<std::mutex> lock(g_liveMutex);
            cur = WideToUtf8(g_livePathW);
        }
        if (cur.size() > 58) cur = "…" + cur.substr(cur.size() - 57);
        right = "扫描中 · " + std::to_string(g_visited.load()) + " 目录 · " + cur;
        rightColor = g_ui.textFaint;
    } else if (g_contentBusy.load()) {
        right = "建立内容索引 · 已写入 " + std::to_string(content::g_parsed.load()) + " 个";
        rightColor = g_ui.warning;
        warn = true;
    } else if (g_detecting.load()) {
        right = GetDetectMsg();
        rightColor = g_ui.textFaint;
    } else if (g_scanSeconds > 0) {
        right = "扫描完成";
        rightColor = g_ui.textFaint;
    }

    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const float h = 30.0f;
    ImGui::Dummy(ImVec2(w, h));

    // 只是一行信息，不再套一层卡片：上面工具栏、下面内容区，中间不需要第三层框。
    const float barX = p.x + 2.0f;
    const float barW = 116.0f;
    const float barH = 4.0f;
    const float barY = p.y + (h - barH) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW, barY + barH),
                      ThemeColor(g_ui.border), barH * 0.5f);
    if (pct > 0.01f)
        dl->AddRectFilled(ImVec2(barX, barY), ImVec2(barX + barW * pct, barY + barH),
                          ThemeColor(warn ? g_ui.warning : g_ui.accent), barH * 0.5f);

    ImGui::PushFont(g_fontSmall, 0.0f);
    const float ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(barX + barW + 12.0f, ty));
    ImGui::TextColored(g_ui.textDim, "%s", left);

    if (!right.empty()) {
        const float tw = ImGui::CalcTextSize(right.c_str()).x;
        const float rx = p.x + w - tw - 12.0f;
        const float minX = barX + barW + 200.0f;
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

// 工具栏统一高度：控件都是 30px，基线才对得齐。
static const float kCtrlH = 30.0f;

// 顶栏：位置组（磁盘 / 路径 / 浏览 / 扫描）· 搜索组（模式 / 输入 / 搜索）· 主题
//
// 宽度从右边界倒推：主题开关贴右，搜索组整体右对齐，路径框吃掉剩下的空间。
// 之前用「预留 N 像素」估算法，窗口一变窄主题开关就被挤出右边界。
static void DrawTopBar() {
    const float padY = std::max(2.0f, (kCtrlH - ImGui::GetTextLineHeight() * 1.0f) * 0.5f);
    static const char* kModes[] = {"文件名", "内容"};
    static const char* kThemes[] = {"亮色", "暗色"};
    const float modeW = SegmentedWidth(kModes, 2);
    const float themeW = SegmentedWidth(kThemes, 2);
    const float E = ImGui::GetWindowContentRegionMax().x;   // 内容区右边界

    // 搜索组：模式 · 输入 · 按钮；输入框宽度跟着窗口走，但夹在 180..300 之间
    float searchW = E * 0.22f;
    if (searchW < 180.0f) searchW = 180.0f;
    if (searchW > 300.0f) searchW = 300.0f;
    const float searchBtnW = 52.0f;
    float blockW = modeW + 8.0f + searchW + 8.0f + searchBtnW;
    const float themeX = E - themeW;
    const float blockRight = themeX - 24.0f;
    float modeX = blockRight - blockW;

    DrawLogo();
    ImGui::SameLine(0, 14);

    // ---- 位置组 ----
    const float browseW = 52.0f, scanW = 64.0f;
    const float pauseW = 52.0f, stopW = 52.0f;
    const bool scanningNow = g_scanning.load();
    // 扫描中这一格是「暂停 + 结束」两颗（52+6+52 = 110），比平时的「扫描」
    // 宽 46px。以前一律按 scanW 预留，多出来的 46px 就被搜索组盖住了
    // —— 截图里「结束」只露出左边一条红边。
    const float leftBtnW = scanningNow ? (pauseW + 6.0f + stopW) : scanW;
    const float dividerW = scanningNow ? 0.0f : 24.0f;   // ToolbarDivider 两边各 10px
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, g_ui.borderStrong);
    ImGui::SetNextItemWidth(118);
    const char* driveLabel = (g_driveIdx >= 0 && g_driveIdx < (int)g_drives.size())
                                 ? g_drives[g_driveIdx].c_str() : "选择磁盘";
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
    ImGui::SameLine(0, 8);

    {
        const float pathX = ImGui::GetCursorPosX();
        // 窗口太窄时先压搜索框，绝不让左边这组钻到搜索框底下
        const float needModeX = pathX + 160.0f + 8.0f + browseW + 8.0f +
                                leftBtnW + dividerW + 12.0f;
        if (modeX < needModeX) {
            searchW = std::max(140.0f, searchW - (needModeX - modeX));
            blockW = modeW + 8.0f + searchW + 8.0f + searchBtnW;
            modeX = blockRight - blockW;
        }
        float pathW = modeX - 16.0f - (browseW + 8.0f + leftBtnW + dividerW) - pathX;
        if (pathW < 160.0f) pathW = 160.0f;
        ImGui::SetNextItemWidth(pathW);
    }
    if (ImGui::InputText("##path", g_pathBuf, sizeof g_pathBuf)) g_rootPath = g_pathBuf;
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    ImGui::SameLine(0, 8);

    if (GhostButton("浏览", ImVec2(browseW, kCtrlH))) {
        std::string picked;
        if (BrowseFolder(picked)) {
            snprintf(g_pathBuf, sizeof g_pathBuf, "%s", picked.c_str());
            g_rootPath = g_pathBuf;
        }
    }
    ImGui::SameLine(0, 8);

    if (scanningNow) {
        if (GhostButton(g_paused.load() ? "继续" : "暂停", ImVec2(pauseW, kCtrlH)))
            g_paused = !g_paused.load();
        ImGui::SameLine(0, 6);
        if (DangerButton(g_stopReq.load() ? "结束中" : "结束", ImVec2(stopW, kCtrlH))) g_stopReq = true;
    } else {
        if (PrimaryButton("扫描", ImVec2(scanW, kCtrlH))) StartScan(g_pathBuf);
    }

    if (!scanningNow) ToolbarDivider(kCtrlH - 10);

    // ---- 搜索组（按算好的绝对位置摆放，不受前面宽度影响）----
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, modeX));
    {
        const ImVec2 rowPos = ImGui::GetCursorScreenPos();
        int m = Segmented("smode", kModes, 2, g_searchMode, kCtrlH);
        if (m != g_searchMode) {
            g_searchMode = m;
            g_hits.clear();          // 别让上一种模式的结果留在列表里
            if (g_searchBuf[0]) {
                RunSearchNow(g_searchBuf);
                g_mode = 2;
            }
        }
        // Segmented 会把光标推到下一行，这里按宽度把它拉回来
        ImGui::SetCursorScreenPos(ImVec2(rowPos.x + modeW + 8.0f, rowPos.y));
    }

    ImGui::SetNextItemWidth(searchW);
    if (g_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        g_focusSearch = false;
    }
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(11, padY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Border, g_ui.borderStrong);
    const bool go = ImGui::InputTextWithHint("##search",
                                             g_searchMode ? "搜索文件内容" : "搜索文件名",
                                             g_searchBuf, sizeof g_searchBuf,
                                             ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);

    bool doSearch = go;
    ImGui::SameLine(0, 8);
    if (GhostButton("搜索", ImVec2(searchBtnW, kCtrlH))) doSearch = true;
    if (doSearch) {
        RunSearchNow(g_searchBuf);
        g_mode = g_searchBuf[0] ? 2 : 0;
    }

    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 12.0f, themeX));
    const int themeIdx = Segmented("theme", kThemes, 2, g_darkTheme ? 1 : 0, kCtrlH, nullptr, true);
    if ((themeIdx == 1) != g_darkTheme) SetTheme(themeIdx == 1);
}

static void DrawUI() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoBringToFrontOnFocus);

    // 后台检索算完了就把结果搬进来（每帧一次，几乎零成本）
    PollSearch();

    // 清理线程结束后统一收尾（g_selected 等状态只能在 UI 线程改）
    if (g_cleanFinish.exchange(false)) {
        int errors = g_cleanErrors.load();
        std::string msg = "已清理 " + FormatBytes(g_cleanFreed.load());
        if (errors) msg += "（" + std::to_string(errors) + " 个文件被占用，已跳过）";
        if (g_cleanOutOfScope > 0)
            msg += "；另有 " + std::to_string(g_cleanOutOfScope) + " 项在扫描范围外，未处理";
        msg += " · 重新检测中，方块图下次扫描后更新";
        g_lastCleanMsg = msg;
        g_selected.clear();
        g_deepSelected.clear();
        g_hitSelected.clear();
        g_currentFilesFor = -2;   // 刷新当前目录的文件列表
        StartDetect();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_F5) && !g_scanning.load()) StartScan(g_pathBuf);
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_F)) g_focusSearch = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape) && g_scanning.load()) g_stopReq = true;

    EnsureCurrentFiles();

    DrawTopBar();

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
            const float rowX = 20.0f;
            const float rightEdge = ImGui::GetWindowContentRegionMax().x;
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

    ImGui::Dummy(ImVec2(0, 4));
    DrawStatusStrip();

    // 面包屑行：无论是否下钻都占同一行，否则点进方块时整块界面会上下跳一格。
    ImGui::Dummy(ImVec2(0, 4));
    {
        const bool canGo = g_current > 0 && !g_entries.empty() && !g_scanning.load();
        std::string crumb = canGo ? PathOf(g_current) : g_rootPath;
        if (crumb.size() > 74) crumb = "…" + crumb.substr(crumb.size() - 73);

        ImGui::PushFont(g_fontSmall, 0.0f);
        ImGui::TextColored(g_ui.textFaint, "%s", crumb.c_str());
        ImGui::PopFont();

        // 导航放右边：路径左对齐、操作右对齐，视线不用来回跳
        const float linksW = ImGui::CalcTextSize("上一层").x + 8.0f + 10.0f +
                             ImGui::CalcTextSize("回到根").x + 8.0f;
        const float lx = ImGui::GetWindowContentRegionMax().x - linksW;
        if (lx > ImGui::GetCursorPosX() + 12.0f) ImGui::SameLine(lx);
        else ImGui::SameLine(0, 12);

        if (!canGo) ImGui::BeginDisabled();
        if (TextLink("上一层")) {
            g_current = g_entries[g_current].parent;
            g_expandRestParent = -1;
        }
        ImGui::SameLine(0, 10);
        if (TextLink("回到根")) {
            g_current = 0;
            g_expandRestParent = -1;
        }
        if (!canGo) ImGui::EndDisabled();
    }
    ImGui::Dummy(ImVec2(0, 3));
    {
        // 发丝线代替 ImGui::Separator()，别让整屏出现一条粗分割
        ImVec2 sp = ImGui::GetCursorScreenPos();
        float sw = ImGui::GetContentRegionAvail().x;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(sp.x, sp.y), ImVec2(sp.x + sw, sp.y),
                                            ThemeColor(g_ui.border), 1.0f);
        ImGui::Dummy(ImVec2(0, 1));
    }

    float totalWidth = ImGui::GetContentRegionAvail().x;
    float rightWidth = std::max(360.0f, std::min(460.0f, totalWidth * 0.34f));

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
    // 出厂自检：SpaceSift.exe --selftest [目录]
    // 下载后第一件事就该是跑它 —— 不建窗口，造样本、抽正文、建索引、搜结果，
    // 全程只在 %TEMP% 里写，跑完自己清理。
    {
        const std::wstring cl = GetCommandLineW();
        if (cl.find(L"--selftest") != std::wstring::npos) {
            int argc = 0;
            LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
            const wchar_t* folder = nullptr;
            for (int i = 1; argv && i < argc; ++i)
                if (argv[i][0] != L'-') folder = argv[i];
            const int rc = selftest::Run(folder);
            if (argv) LocalFree(argv);
            return rc;
        }
    }

    // 图标来自 app.rc 里嵌入的 1 号资源（assets/icon.ico）
    HICON appIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    if (!appIcon) appIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
    WNDCLASSEXW wc = {sizeof wc, CS_CLASSDC, WndProc, 0, 0, hInstance,
                      appIcon,
                      LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)),
                      nullptr, nullptr, L"SpaceSiftNative", appIcon};
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowW(wc.lpszClassName, L"SpaceSift Native " SPACESIFT_VERSION_W L" · 磁盘空间分析", WS_OVERLAPPEDWINDOW,
                              80, 80, 1320, 880, nullptr, nullptr, wc.hInstance, nullptr);
    g_mainHwnd = hwnd;
    // COM 要在读 shell 目录（SHGetFolderPathW）之前初始化
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    // 提权实例：--reveal "路径" → 只做定位然后退出，不显示窗口
    {
        std::wstring cl = GetCommandLineW();
        const std::wstring key = L"--reveal ";
        size_t at = cl.find(key);
        if (at != std::wstring::npos) {
            std::wstring rest = cl.substr(at + key.size());
            if (!rest.empty() && rest[0] == L'"') {
                size_t end = rest.find(L'"', 1);
                if (end != std::wstring::npos) rest = rest.substr(1, end - 1);
            } else {
                size_t sp = rest.find(L' ');
                if (sp != std::wstring::npos) rest = rest.substr(0, sp);
            }
            RvLog("[reveal] 提权实例接手 %s", WideToUtf8(rest).c_str());
            RevealInExplorer(WideToUtf8(rest));
            Sleep(1500);
            return 0;
        }
    }
    g_darkTheme = LoadThemeSetting();
    g_searchAllRoots = LoadSearchScopeSetting();
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
    SpaceSift_InstallImeSupport();   // 补上后端缺失的 IME 定位（否则中文候选框跑到屏幕角落）

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
    if (const wchar_t* f = _wgetenv(L"SPACESIFT_FOCUS_SEARCH")) g_focusSearch = (f[0] && f[0] != L'0');
    // 脚本化钩子：SPACESIFT_MODE=content 直接进内容检索模式；
    // SPACESIFT_CONTENT=1 扫完后自动建内容索引（和上面的 SPACESIFT_SEARCH 配套用）
    if (const char* m = std::getenv("SPACESIFT_MODE")) {
        if (std::string(m) == "content") g_searchMode = 1;
    }
    if (std::getenv("SPACESIFT_CONTENT")) g_pendingContent = true;
    if (const char* sc = std::getenv("SPACESIFT_SCOPE")) g_searchAllRoots = std::string(sc) == "all";
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
        SpaceSift_ImeFrame();
        ImGui::Render();

        const float clear[4] = {0x0f / 255.0f, 0x11 / 255.0f, 0x15 / 255.0f, 1.0f};
        g_pd3dContext->OMSetRenderTargets(1, &g_mainRTV, nullptr);
        g_pd3dContext->ClearRenderTargetView(g_mainRTV, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(0, 0);
        Sleep(g_scanning.load() ? 8 : 40);
    }

    // 后台检索线程收尾：不 join 的话它可能在静态对象析构时还在跑。
    // 先置中止标志，正在跨库检索也会在一个库查完后停下，不至于关窗口卡几秒。
    g_sAbort = true;
    {
        std::lock_guard<std::mutex> lk(g_sMx);
        g_sQuit = true;
    }
    g_sCv.notify_all();
    if (g_sThread.joinable()) g_sThread.join();

    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
