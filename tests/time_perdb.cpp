// 逐库计时：找出「所有已索引」里哪一份最贵
// 用法: time_perdb.exe <关键词>
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "content.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: time_perdb.exe <关键词>\n");
        return 2;
    }
    std::string kw = argv[1];
    if (const char* f = std::getenv("KW_FILE")) {
        FILE* fp = fopen(f, "rb");
        if (fp) {
            char buf[512] = {0};
            size_t n = fread(buf, 1, sizeof buf - 1, fp);
            fclose(fp);
            kw.assign(buf, n);
            while (!kw.empty() && (kw.back() == '\n' || kw.back() == '\r')) kw.pop_back();
        }
    }
    const auto t0 = std::chrono::steady_clock::now();
    auto idxs = content::IndexSnapshot();
    const auto snapMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    printf("索引 %zu 份，IndexSnapshot 耗时 %lld ms\n\n", idxs.size(), (long long)snapMs);

    long long total = 0;
    for (const auto& i : idxs) {
        if (i.root.empty()) continue;
        const auto s = std::chrono::steady_clock::now();
        auto hits = content::Search(i.root, kw, 50, nullptr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - s)
                            .count();
        total += ms;
        if (ms > 50 || !hits.empty())
            printf("%6lld ms  %5.1f MB  %-50s 命中 %zu\n", (long long)ms,
                   i.bytes / 1048576.0, i.root.c_str(), hits.size());
    }
    printf("\n逐库合计 %lld ms\n", total);
    return 0;
}
