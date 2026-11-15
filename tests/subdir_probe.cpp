// 验证"子目录搜索"：索引建在父目录时，子目录能不能搜到、会不会串到兄弟目录。
// 用法: subdir_probe.exe <parentDir> <subA> <subB> <kwA> <kwB>
//   —— parent 需要有内容索引；subA/subB 是它的两个子目录，各自有专属关键词
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "content.h"
#include "extract.h"

static std::string FromWide(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// 命令行参数是 ANSI(代码页) 字节，直接当 UTF-8 用会让中文关键词变乱码，
// 乱码又让 MATCH 构造不出来 —— 测出来的 0 条是假的。
static std::string ArgToUtf8(const char* a) {
    const int n = MultiByteToWideChar(CP_ACP, 0, a, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_ACP, 0, a, -1, w.data(), n);
    return FromWide(w.c_str());
}

int main(int argc, char** argv) {
    if (argc < 6) {
        fprintf(stderr, "usage: subdir_probe <parent> <subA> <subB> <kwA> <kwB>\n");
        return 2;
    }
    const std::string parent = ArgToUtf8(argv[1]), subA = ArgToUtf8(argv[2]), subB = ArgToUtf8(argv[3]);
    const std::string kwA = ArgToUtf8(argv[4]), kwB = ArgToUtf8(argv[5]);

    int fail = 0;
    auto expect = [&](bool ok, const char* what) {
        printf("%s %s\n", ok ? "OK  " : "!!  ", what);
        if (!ok) ++fail;
    };

    printf("parent=%s\n  subA=%s\n  subB=%s\n", parent.c_str(), subA.c_str(), subB.c_str());

    printf("-- 覆盖判定\n");
    printf("   HasIndex(parent)        = %d\n", (int)content::HasIndex(parent));
    printf("   HasIndex(subA)          = %d  (期望 0：subA 自己没有库)\n", (int)content::HasIndex(subA));
    printf("   HasCoveringIndex(subA)  = %d  (期望 1：父目录的库覆盖它)\n", (int)content::HasCoveringIndex(subA));
    expect(content::HasCoveringIndex(subA), "subA 能找到覆盖它的索引");
    std::string cov = content::FindCoveringIndex(subA);
    expect(cov == content::NormRoot(parent), "覆盖索引的 root 就是父目录");

    printf("-- 子目录搜索：只应返回自己目录下的文件\n");
    auto hitsA = content::Search(subA, kwA, 50, nullptr);
    auto hitsB = content::Search(subB, kwB, 50, nullptr);
    printf("   Search(subA, \"%s\") -> %d 条\n", kwA.c_str(), (int)hitsA.size());
    for (auto& h : hitsA) printf("      %s\n", h.path.c_str());
    printf("   Search(subB, \"%s\") -> %d 条\n", kwB.c_str(), (int)hitsB.size());
    for (auto& h : hitsB) printf("      %s\n", h.path.c_str());

    expect(!hitsA.empty(), "subA 的专属关键词能在父目录索引里搜到（这就是换文件夹就搜不到的那类）");
    expect(!hitsB.empty(), "subB 的专属关键词能搜到");

    // 关键：不能串库
    const std::string normA = content::NormRoot(subA);
    bool leak = false;
    for (auto& h : hitsA)
        if (h.path.compare(0, normA.size(), normA) != 0) { leak = true; printf("      !! 越界: %s\n", h.path.c_str()); }
    expect(!leak, "subA 的结果没有跑到 subA 之外（不串兄弟目录）");

    bool cross = false;
    for (auto& h : hitsA)
        if (h.path.find(kwB) != std::string::npos) cross = true;
    expect(!cross, "搜 A 的专属词不会命中 B 的文件");

    printf("-- 父目录搜索：两个子目录的结果都该有\n");
    auto allA = content::Search(parent, kwA, 50, nullptr);
    auto allB = content::Search(parent, kwB, 50, nullptr);
    printf("   Search(parent, \"%s\") -> %d 条；Search(parent, \"%s\") -> %d 条\n",
           kwA.c_str(), (int)allA.size(), kwB.c_str(), (int)allB.size());
    expect(!allA.empty() && !allB.empty(), "父目录能同时搜到两个子目录的内容");

    printf("-- 完全没索引的目录\n");
    const std::string orphan = FromWide(L"C:\\Temp\\ssnope_never_indexed");
    const std::string covOrphan = content::FindCoveringIndex(orphan);
    printf("   覆盖索引 = %s\n", covOrphan.empty() ? "(无)" : covOrphan.c_str());
    // 没有自己的库时，要么完全没有覆盖，要么只能被自己的祖先覆盖
    // （比如给 C:\ 建过全盘索引，那 C:\ 下任何目录都被它覆盖，这是对的）
    const std::string normOrphan = content::NormRoot(orphan);
    expect(covOrphan.empty() || normOrphan.compare(0, covOrphan.size(), covOrphan) == 0,
           "未索引目录的覆盖索引只能是自己或祖先，不能是别人的目录");
    {
        const ULONGLONG t0 = GetTickCount64();
        auto none = content::Search(orphan, kwA, 50, nullptr);
        const ULONGLONG ms = GetTickCount64() - t0;
        printf("   在该目录搜 \"%s\" -> %d 条，用时 %llu ms\n",
               kwA.c_str(), (int)none.size(), (unsigned long long)ms);
        expect(true, "未索引目录不会崩溃/卡死");
    }

    printf("\n%s\n", fail == 0 ? "全部通过" : "有失败项");
    return fail == 0 ? 0 : 1;
}
