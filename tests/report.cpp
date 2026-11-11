// 体检报告：对任意目录跑一遍完整内容索引，输出统计。
// 用法: report.exe "D:\\某目录"
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "content.h"
#include "extract.h"
#include "sqlite3.h"

namespace {

struct Stat {
    long long files = 0;
    long long bytes = 0;
};

std::string LowerExtOf(const std::string& p) {
    const size_t dot = p.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string e = p.substr(dot);
    if (e.size() > 12) return {};
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

// 与 content.cpp 一致：只统计能进内容索引的候选文件
void Walk(const std::wstring& dir, std::map<std::string, Stat>& all,
          std::map<std::string, Stat>& cand, long long& skippedBig) {
    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subs;
    do {
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += n;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring low;
            for (const wchar_t* q = n; *q; ++q) low.push_back((wchar_t)towlower(*q));
            if (low == L"node_modules" || low == L".git" || low == L"__pycache__" ||
                low == L".venv" || low == L"venv" || low == L"site-packages")
                continue;
            subs.push_back(full);
            continue;
        }

        const long long size =
            (long long)(((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow);
        const std::string ext = LowerExtOf(extract::FromWide(full));
        all[ext].files++;
        all[ext].bytes += size;
        if (size == 0) continue;
        if (!extract::Supports(ext)) continue;
        if (size > (long long)extract::kSkipAbove && !extract::StreamsLargeFiles(ext)) {
            ++skippedBig;
            continue;
        }
        cand[ext].files++;
        cand[ext].bytes += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (auto& d : subs) Walk(d, all, cand, skippedBig);
}

std::string Human(long long b) {
    char buf[64];
    if (b >= 1024LL * 1024 * 1024) snprintf(buf, sizeof buf, "%.2f GB", (double)b / (1024.0 * 1024 * 1024));
    else if (b >= 1024LL * 1024)   snprintf(buf, sizeof buf, "%.1f MB", (double)b / (1024.0 * 1024));
    else if (b >= 1024)            snprintf(buf, sizeof buf, "%.1f KB", (double)b / 1024.0);
    else                           snprintf(buf, sizeof buf, "%lld B", b);
    return buf;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // 命令行里的中文路径经 MSYS/ANSI 中转会变乱码，改用宽字符命令行 + 环境变量
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::string root;
    if (const wchar_t* env = _wgetenv(L"REPORT_ROOT")) {
        if (env[0]) root = extract::FromWide(env);
    }
    if (root.empty() && wargv && wargc >= 2) root = extract::FromWide(wargv[1]);
    if (wargv) LocalFree(wargv);
    if (root.empty()) {
        printf("用法: set REPORT_ROOT=<目录> 然后 report.exe\n");
        return 2;
    }
    printf("目录: %s\n\n", root.c_str());

    // 1) 先摸清全貌
    std::map<std::string, Stat> all, cand;
    long long skippedBig = 0;
    auto t0 = std::chrono::steady_clock::now();
    Walk(extract::ToWide(root), all, cand, skippedBig);
    auto t1 = std::chrono::steady_clock::now();

    long long totalFiles = 0, totalBytes = 0, candFiles = 0, candBytes = 0;
    for (auto& kv : all) { totalFiles += kv.second.files; totalBytes += kv.second.bytes; }
    for (auto& kv : cand) { candFiles += kv.second.files; candBytes += kv.second.bytes; }

    printf("== 磁盘全貌 ==\n");
    printf("  文件总数 %lld（%s），扩展名 %zu 种\n", totalFiles, Human(totalBytes).c_str(), all.size());
    printf("  可抽正文的候选 %lld（%s，占文件数 %.1f%%）\n", candFiles, Human(candBytes).c_str(),
           totalFiles ? 100.0 * candFiles / totalFiles : 0.0);
    if (skippedBig) printf("  因体积超限跳过 %lld 个\n", skippedBig);
    printf("  遍历耗时 %.2f 秒\n\n", std::chrono::duration<double>(t1 - t0).count());

    // 2) 建内容索引（跑的是应用里同一套代码）
    printf("== 建内容索引 ==\n");
    std::atomic<bool> cancel{false};
    auto t2 = std::chrono::steady_clock::now();
    content::Build(root, cancel);
    auto t3 = std::chrono::steady_clock::now();
    const double buildSec = std::chrono::duration<double>(t3 - t2).count();
    printf("  写入 %d 个（扫描到候选 %d 个），耗时 %.2f 秒\n\n",
           content::g_parsed.load(), content::g_seen.load(), buildSec);

    // 3) 从库里读明细
    const std::string dbPath = content::DbPathFor(root);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        printf("打不开索引库: %s\n", dbPath.c_str());
        return 1;
    }

    struct Row { std::string ext; long long n, err, chars; };
    std::vector<Row> rows;
    {
        sqlite3_stmt* st = nullptr;
        sqlite3_prepare_v2(db,
            "SELECT ext, COUNT(*), SUM(err), SUM(LENGTH(text)) FROM docs "
            "GROUP BY ext ORDER BY COUNT(*) DESC", -1, &st, nullptr);
        while (st && sqlite3_step(st) == SQLITE_ROW) {
            Row r;
            const unsigned char* e = sqlite3_column_text(st, 0);
            r.ext = e ? (const char*)e : "";
            r.n = sqlite3_column_int64(st, 1);
            r.err = sqlite3_column_int64(st, 2);
            r.chars = sqlite3_column_int64(st, 3);
            rows.push_back(r);
        }
        if (st) sqlite3_finalize(st);
    }

    printf("== 各格式索引情况（前 20）==\n");
    printf("  %-9s %7s %7s %12s %10s\n", "扩展名", "已索引", "抽空", "正文字符", "平均");
    long long sumN = 0, sumErr = 0, sumChars = 0;
    for (size_t i = 0; i < rows.size(); ++i) {
        const Row& r = rows[i];
        sumN += r.n; sumErr += r.err; sumChars += r.chars;
        if (i < 20)
            printf("  %-9s %7lld %7lld %12lld %10lld\n", r.ext.c_str(), r.n, r.err, r.chars,
                   r.n ? r.chars / r.n : 0);
    }
    printf("  %-9s %7lld %7lld %12lld\n\n", "合计", sumN, sumErr, sumChars);

    // 4) 查询延迟
    const char* queries[] = {"加热卷烟", "孔隙率", "湍流", "法兰", "MVR", "参数", "论文"};
    printf("== 检索延迟 ==\n");
    for (const char* q : queries) {
        auto a = std::chrono::steady_clock::now();
        auto hits = content::Search(root, q, 400);
        auto b = std::chrono::steady_clock::now();
        printf("  %-10s %4d 条  %6.1f ms", q, (int)hits.size(),
               std::chrono::duration<double, std::milli>(b - a).count());
        if (!hits.empty()) printf("   ← %s", hits[0].name.c_str());
        printf("\n");
    }
    sqlite3_close(db);

    // 5) 库体积
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(extract::ToWide(dbPath).c_str(), GetFileExInfoStandard, &fad)) {
        const long long sz = ((long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        printf("\n索引库: %s\n", Human(sz).c_str());
    }
    return 0;
}
