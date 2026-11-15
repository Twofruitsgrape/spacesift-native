// 无界面建内容索引：给脚本/诊断用，不弹窗口（GUI 实例会打扰用户且可能被误关）。
// 用法: build_cli.exe <目录>   （或 set BUILD_ROOT=<目录>）
// 进度每 2 秒打一行；结束打印错误/通知与 docs 总数。
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include "content.h"
#include "extract.h"
#include "sqlite3.h"

int main(int argc, char** argv) {
    std::string root;
    if (argc >= 2) root = argv[1];
    // 中文路径别走 argv —— 控制台代码页会把 UTF-8 字节弄坏（会建出哈希对不上的垃圾库）。
    // BUILD_ROOT_FILE 指向一个 UTF-8 文本文件，第一行就是目录。
    if (root.empty() && std::getenv("BUILD_ROOT_FILE")) {
        FILE* f = fopen(std::getenv("BUILD_ROOT_FILE"), "rb");
        if (f) {
            char buf[2048] = {0};
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            fclose(f);
            std::string s(buf, n);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
                s.pop_back();
            root = s;
        }
    }
    if (root.empty() && _wgetenv(L"BUILD_ROOT")) {
        wchar_t* w = _wgetenv(L"BUILD_ROOT");
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
        root.resize(n - 1);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, root.data(), n, nullptr, nullptr);
    }
    if (root.empty()) {
        printf("用法: build_cli.exe <目录>\n");
        return 2;
    }
    printf("建内容索引: %s\n", root.c_str());
    printf("库路径: %s\n\n", content::DbPathFor(root).c_str());

    std::atomic<bool> cancel{false};
    const auto t0 = std::chrono::steady_clock::now();
    std::thread worker([&] { content::Build(root, cancel); });

    while (content::g_busy.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        printf("[%4llds] pass=%d 见=%d 处理=%d 抽取=%d 跳过=%d\n", (long long)sec,
               content::g_passes.load(), content::g_seen.load(), content::g_done.load(),
               content::g_parsed.load(), content::g_skipped.load());
        fflush(stdout);
    }
    worker.join();
    const auto sec = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::steady_clock::now() - t0)
                         .count();

    const std::string err = content::LastError();
    const std::string notice = content::LastNotice();
    if (!err.empty()) printf("\n[错误] %s\n", err.c_str());
    if (!notice.empty()) printf("\n[通知] %s\n", notice.c_str());

    // 结果盘点
    const std::string dbPath = content::DbPathFor(root);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM docs", -1, &st, nullptr) == SQLITE_OK &&
            sqlite3_step(st) == SQLITE_ROW) {
            printf("\n完成用时 %lld 秒，docs=%d 条\n", (long long)sec,
                   sqlite3_column_int(st, 0));
        }
        sqlite3_finalize(st);
    } else if (db) {
        sqlite3_close(db);
    }
    if (db) sqlite3_close(db);
    return err.empty() ? 0 : 1;
}
