// 复现"无法创建内容索引库: unable to open database file"。
// 模拟 content.cpp Build 的打开序列，但把每一层的 Win32 错误都打出来：
//   1. 目标文件现状（属性/只读/是否目录）
//   2. CreateFileW 直接建文件的 Win32 错误码（杀软持锁 = 32 sharing violation）
//   3. sqlite3_open 的 rc + errmsg，多次重试观察是否恢复
// 用法: set REPRO_ROOT=E:\xxx 然后 repro_open.exe
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

#include "sqlite3.h"

static std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string NormRoot(std::string s) {
    for (auto& c : s) if (c == '/') c = '\\';
    if (s.size() >= 2 && s[1] == ':' && s[0] >= 'a' && s[0] <= 'z')
        s[0] = (char)toupper((unsigned char)s[0]);
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    if (s.size() == 2 && s[1] == ':') s += '\\';
    return s;
}

static uint32_t Fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

static std::string DbPathFor(const std::string& root) {
    const std::string norm = NormRoot(root);
    std::string safe;
    safe.reserve(norm.size());
    for (char c : norm) {
        if (isalnum((unsigned char)c) || c == '_') safe.push_back(c);
        else safe.push_back('_');
    }
    if (safe.size() > 33) safe.resize(33);
    char hex[16];
    snprintf(hex, sizeof hex, "_%08x", Fnv1a(norm));
    wchar_t wbuf[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, wbuf);
    return WideToUtf8(std::wstring(wbuf) + L"\\SpaceSiftNative") + "\\content_" + safe + hex + ".db";
}

static const char* WinErr(DWORD e) {
    switch (e) {
        case 0: return "成功";
        case ERROR_SHARING_VIOLATION: return "ERROR_SHARING_VIOLATION(32) 共享冲突——文件被别的进程(常见:杀软)占着";
        case ERROR_ACCESS_DENIED: return "ERROR_ACCESS_DENIED(5) 拒绝访问——权限/只读/杀软拦截";
        case ERROR_FILE_NOT_FOUND: return "ERROR_FILE_NOT_FOUND(2)";
        case ERROR_PATH_NOT_FOUND: return "ERROR_PATH_NOT_FOUND(3) 目录不存在";
        case ERROR_SHARING_BUFFER_EXCEEDED: return "共享缓冲超限";
        case ERROR_HANDLE_DISK_FULL: case ERROR_DISK_FULL: return "磁盘满";
        case ERROR_INVALID_PARAMETER: return "ERROR_INVALID_PARAMETER(87)";
        case ERROR_DIRECTORY: return "目标是目录";
        default: return "";
    }
}

int main() {
    char rootU[1024] = {0};
    if (const wchar_t* env = _wgetenv(L"REPRO_ROOT")) {
        WideCharToMultiByte(CP_UTF8, 0, env, -1, rootU, sizeof rootU, nullptr, nullptr);
    } else {
        printf("用法: set REPRO_ROOT=<目录> 然后 repro_open.exe\n");
        return 2;
    }

    const std::string dbPath = DbPathFor(rootU);
    const std::wstring w = std::wstring(dbPath.begin(), dbPath.end());
    printf("目标库: %s\n\n", dbPath.c_str());

    // 1. 现状
    const DWORD a = GetFileAttributesW(w.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        printf("[现状] 文件不存在 (GetLastError=%lu %s)\n", GetLastError(), WinErr(GetLastError()));
    } else {
        printf("[现状] 存在 属性=0x%08lX %s%s%s\n", a,
               (a & FILE_ATTRIBUTE_READONLY) ? "[只读]" : "",
               (a & FILE_ATTRIBUTE_DIRECTORY) ? "[目录]" : "",
               (a & FILE_ATTRIBUTE_HIDDEN) ? "[隐藏]" : "");
    }

    // 2. Win32 层直接建（不经过 sqlite）
    {
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            printf("[Win32] CreateFileW 失败: %lu %s\n", GetLastError(), WinErr(GetLastError()));
        } else {
            CloseHandle(h);
            printf("[Win32] CreateFileW 成功（说明目录可写、杀软没拦）\n");
        }
    }

    // 3. sqlite3_open 序列（和 Build 一样：失败重试，退避拉长观察恢复时间）
    printf("\n[sqlite] 开始重试序列:\n");
    sqlite3* db = nullptr;
    int rc = sqlite3_open(dbPath.c_str(), &db);
    int attempt = 0;
    int delays[] = {250, 250, 500, 500, 1000, 1000, 2000, 2000, 4000, 4000};
    while (rc != SQLITE_OK && attempt < 10) {
        printf("  尝试 %d: rc=%d (%s)  errmsg=%s\n", attempt + 1, rc,
               sqlite3_errstr(rc), db ? sqlite3_errmsg(db) : "-");
        if (db) { sqlite3_close(db); db = nullptr; }
        Sleep(delays[attempt]);
        ++attempt;
        rc = sqlite3_open(dbPath.c_str(), &db);
    }
    if (rc == SQLITE_OK) {
        printf("  尝试 %d: 成功打开\n", attempt + 1);
        sqlite3_close(db);
        DeleteFileW(w.c_str());
        printf("\n结论: 最终能打开——之前的失败是持续性锁(杀软/占位)超过重试窗口\n");
    } else {
        printf("  10 次全部失败: %s\n", db ? sqlite3_errmsg(db) : "-");
        // 最后再探一次 Win32
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE)
            printf("  Win32 复核: %lu %s\n", GetLastError(), WinErr(GetLastError()));
        else
            CloseHandle(h);
        if (db) sqlite3_close(db);
    }
    return 0;
}
