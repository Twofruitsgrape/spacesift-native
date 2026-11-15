// 测「所有已索引」跨库检索的真实耗时（在用户真实数据目录上跑）
// 用法: time_searchall.exe <关键词> [次数]
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <string>

#include "content.h"

int main(int argc, char** argv) {
    const std::string kw = argc > 1 ? argv[1] : "test";
    const int times = argc > 2 ? atoi(argv[2]) : 1;
    printf("数据目录: %s\n", db::DataDir().c_str());
    printf("关键词: %s\n\n", kw.c_str());
    for (int i = 0; i < times; ++i) {
        const auto t0 = std::chrono::steady_clock::now();
        const auto hits = content::SearchAll(kw, 50, nullptr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
        printf("第 %d 次：%lld ms，命中 %zu 条\n", i + 1, (long long)ms, hits.size());
        fflush(stdout);
    }
    return 0;
}
