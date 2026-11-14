// 诊断用探针：验证索引命名、跨目录检索、索引清单。
// 用法:
//   probe_scope.exe                 -> 打印命名映射 + 已建索引清单
//   probe_scope.exe <root> <query>  -> 建/更新该目录索引，再分别做「本目录」和「全部」检索
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "content.h"
#include "db.h"
#include "extract.h"

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

int main() {
    // 用宽字符命令行取参数：走 main(int,char**) 时中文会被控制台代码页弄坏
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(extract::FromWide(wargv[i]));
    if (wargv) LocalFree(wargv);

    printf("=== 索引库命名映射 ===\n");
    const char* roots[] = {
        "E:\\Projects\\comsol with ai 2\\build_cache",
        "E:\\Projects\\comsol with ai dsh\\build_cache",
        "E:/小论文1/mvr_sim",
        "E:\\小论文1\\mvr_sim",
    };
    std::vector<std::string> paths;
    for (const char* r : roots) {
        const std::string p = content::DbPathFor(r);
        const size_t slash = p.find_last_of('\\');
        printf("%-46s -> %s\n", r, slash == std::string::npos ? p.c_str() : p.c_str() + slash + 1);
        paths.push_back(Lower(p));
    }
    for (size_t i = 0; i < paths.size(); ++i)
        for (size_t j = i + 1; j < paths.size(); ++j)
            if (paths[i] == paths[j]) printf("!! 撞车: %zu 与 %zu 指向同一个库\n", i, j);
    printf("（第 3、4 条是同一个目录的两种写法，必须落到同一个库）\n");

    printf("\n=== 已建内容索引 ===\n");
    for (const auto& i : content::ListIndexes()) {
        printf("%-40s docs=%-8lld %6.1f MB  建于 %lld%s%s\n", i.root.c_str(),
               (long long)i.docs, i.bytes / 1048576.0, (long long)i.builtAt,
               i.partial ? " [未建完]" : "", i.stale ? " [需重建]" : "");
    }

    if (args.size() < 3) return 0;

    const std::string root = args[1];
    const std::string q = args[2];
    printf("\n=== 索引 %s ===\n", root.c_str());
    std::atomic<bool> cancel{false};
    content::Build(root, cancel);
    printf("候选 %d · 写入 %d · 复用 %d · 库内 %d\n", content::g_seen.load(),
           content::g_parsed.load(), content::g_skipped.load(), content::g_files.load());
    const std::string notice = content::LastNotice();
    if (!notice.empty()) printf("提示: %s\n", notice.c_str());

    printf("\n=== 本目录搜「%s」===\n", q.c_str());
    auto hits = content::Search(root, q, 20);
    printf("命中 %d 条\n", (int)hits.size());
    for (size_t i = 0; i < hits.size() && i < 3; ++i)
        printf("  [%d] %s\n      %s\n", hits[i].score, hits[i].path.c_str(), hits[i].snippet.c_str());

    printf("\n=== 全部已索引目录搜「%s」===\n", q.c_str());
    auto all = content::SearchAll(q, 20);
    printf("命中 %d 条\n", (int)all.size());
    for (size_t i = 0; i < all.size() && i < 5; ++i)
        printf("  [%d] %s\n", all[i].score, all[i].path.c_str());
    return 0;
}
