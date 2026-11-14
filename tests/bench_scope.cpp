// 量化「切换检索范围」的耗时：ListIndexes 每帧调一次是什么代价、
// SearchAll 在真实机器上要多久。改完再跑一遍对比。
#include <cstdio>
#include <chrono>
#include <string>
#include <vector>

#include "content.h"
#include "extract.h"

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a) {
    return std::chrono::duration<double, std::milli>(Clock::now() - a).count();
}

int main(int argc, char** argv) {
    const std::string q = argc > 2 ? argv[2] : "报告";
    (void)argc;

    auto t0 = Clock::now();
    std::vector<content::IndexInfo> list = content::ListIndexes();
    const double list1 = Ms(t0);
    printf("ListIndexes 第 1 次: %8.1f ms  (%d 份索引)\n", list1, (int)list.size());

    t0 = Clock::now();
    for (int i = 0; i < 10; ++i) list = content::ListIndexes();
    const double listN = Ms(t0) / 10.0;
    printf("ListIndexes 之后 10 次均值: %6.1f ms   ← 每帧都调就是这个代价\n", listN);

    t0 = Clock::now();
    std::vector<db::Hit> hits = content::SearchAll(q, 400);
    const double sAll = Ms(t0);
    printf("SearchAll(\"%s\"): %8.1f ms  (%d 条)\n", q.c_str(), sAll, (int)hits.size());

    // 逐库拆开，看时间花在哪一份上
    for (const auto& idx : list) {
        auto t = Clock::now();
        auto part = content::Search(idx.root, q, 400);
        const double ms = Ms(t);
        if (ms > 30.0)
            printf("   %7.1f ms  %-42s %5d 条\n", ms, idx.root.c_str(), (int)part.size());
    }
    return 0;
}
