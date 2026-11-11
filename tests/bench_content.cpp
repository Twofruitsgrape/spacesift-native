// 压力/基准：对任意目录建一次内容索引，量一下耗时和库体积。
// 用法：bench_content.exe "D:\\资料目录"
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "content.h"
#include "extract.h"

static double MB(uint64_t b) { return (double)b / (1024.0 * 1024.0); }

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s \"目录\"\n", argv[0]);
        return 2;
    }
    std::string root = argv[1];
    printf("目录: %s\n", root.c_str());

    std::atomic<bool> cancel{false};
    auto t0 = std::chrono::steady_clock::now();
    content::Build(root, cancel);
    auto t1 = std::chrono::steady_clock::now();
    double sec = std::chrono::duration<double>(t1 - t0).count();

    int seen = content::g_seen.load();
    int parsed = content::g_parsed.load();
    printf("\n扫描候选 %d 个 · 写入 %d 个 · 耗时 %.2f 秒（%.0f 文件/秒）\n",
           seen, parsed, sec, sec > 0 ? parsed / sec : 0.0);

    // 库体积
    std::wstring db = extract::ToWide(content::DbPathFor(root));
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(db.c_str(), GetFileExInfoStandard, &fad)) {
        uint64_t sz = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
        printf("索引库: %.1f MB  (%s)\n", MB(sz), content::DbPathFor(root).c_str());
    } else {
        printf("索引库: 读不到\n");
    }

    // 测几条查询
    const char* qs[] = {"图纸", "法兰", "PRODUCT", "关键词", "function"};
    for (const char* q : qs) {
        auto a = std::chrono::steady_clock::now();
        auto hits = content::Search(root, q, 400);
        auto b = std::chrono::steady_clock::now();
        printf("  搜「%s」: %d 条, %.0f ms\n", q, (int)hits.size(),
               std::chrono::duration<double, std::milli>(b - a).count());
        if (!hits.empty()) printf("      → %s\n         %s\n",
                                  hits[0].name.c_str(), hits[0].snippet.c_str());
    }
    return 0;
}
