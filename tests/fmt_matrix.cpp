// 逐格式验证内容索引：建两个目录的索引，对清单里每一行断言四件事
//   1) docs 表里有这条（入库）
//   2) tlen > 0（抽到了正文）
//   3) 在"当前目录"范围搜得到（content::Search）
//   4) 切到"所有已索引"也搜得到（content::SearchAll）—— 这就是"换个文件夹搜不到"的检测点
//
// 用法: fmt_matrix.exe <rootA> <rootB> <manifestA> <manifestB>
// 清单每行: 扩展名 \t 关键词 \t 完整路径
// 结束会删掉这两个测试索引，不留垃圾。
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "content.h"
#include "extract.h"
#include "sqlite3.h"

struct Case {
    std::string ext;
    std::string kw;
    std::string path;
};

static std::vector<Case> LoadManifest(const char* file) {
    std::vector<Case> out;
    std::ifstream f(file, std::ios::binary);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const size_t t1 = line.find('\t');
        if (t1 == std::string::npos) continue;
        const size_t t2 = line.find('\t', t1 + 1);
        if (t2 == std::string::npos) continue;
        out.push_back({line.substr(0, t1), line.substr(t1 + 1, t2 - t1 - 1),
                       line.substr(t2 + 1)});
    }
    return out;
}

// 从库里读这条文档的状态：0 = 不存在，1 = 存在但没正文，2 = 有正文
static int DbState(const std::string& root, const std::string& path, int64_t* tlen) {
    sqlite3* db = nullptr;
    int state = 0;
    if (sqlite3_open_v2(content::DbPathFor(root).c_str(), &db, SQLITE_OPEN_READONLY,
                        nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT tlen,err FROM docs WHERE path=?", -1, &st, nullptr) ==
        SQLITE_OK) {
        sqlite3_bind_text(st, 1, path.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const int64_t n = sqlite3_column_int64(st, 0);
            if (tlen) *tlen = n;
            state = n > 0 ? 2 : 1;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return state;
}

static bool Hits(const std::vector<db::Hit>& v, const std::string& path) {
    for (const auto& h : v)
        if (h.path == path) return true;
    return false;
}

static void Run(const char* tag, const std::string& root, const std::vector<Case>& cases) {
    for (const auto& c : cases) {
        int64_t tlen = 0;
        const int dbState = DbState(root, c.path, &tlen);
        const bool cur = Hits(content::Search(root, c.kw, 30, nullptr), c.path);
        const bool all = Hits(content::SearchAll(c.kw, 200, nullptr), c.path);
        // ext | dbState | tlen | 当前目录 | 跨库 | 关键词 | 路径
        printf("%s\t%s\t%d\t%lld\t%d\t%d\t%s\t%s\n", tag, c.ext.c_str(), dbState,
               (long long)tlen, (int)cur, (int)all, c.kw.c_str(), c.path.c_str());
        fflush(stdout);
    }
}

int main(int argc, char** argv) {
    if (argc < 5) {
        printf("用法: fmt_matrix.exe <rootA> <rootB> <manifestA> <manifestB>\n");
        return 2;
    }
    // 图片只有开了 OCR 才进得了索引（Supports 门控），由驱动脚本用环境变量传进来
    if (std::getenv("SPACESIFT_OCR")) extract::SetOcrEnabled(true);

    const std::string rootA = argv[1];
    const std::string rootB = argv[2];
    auto casesA = LoadManifest(argv[3]);
    auto casesB = LoadManifest(argv[4]);

    std::atomic<bool> cancel{false};
    printf("== 建索引 A: %s (%zu 个样本)\n", rootA.c_str(), casesA.size());
    fflush(stdout);
    content::Build(rootA, cancel);
    if (!content::LastError().empty())
        printf("!! A 建库失败: %s\n", content::LastError().c_str());
    printf("== 建索引 B: %s (%zu 个样本)\n", rootB.c_str(), casesB.size());
    fflush(stdout);
    content::Build(rootB, cancel);
    if (!content::LastError().empty())
        printf("!! B 建库失败: %s\n", content::LastError().c_str());

    printf("===ROWS===\n");
    Run("A", rootA, casesA);
    Run("B", rootB, casesB);

    // 反向断言：在 A 的"当前目录"范围搜 B 的专属词，应当搜不到（这是设计）
    printf("===CROSS===\n");
    for (const auto& c : casesB) {
        const bool leaked = Hits(content::Search(rootA, c.kw, 10, nullptr), c.path);
        if (leaked) printf("LEAK\t%s\t%s\t%s\n", c.ext.c_str(), c.kw.c_str(), c.path.c_str());
    }
    printf("===DONE===\n");

    content::DeleteIndex(rootA);
    content::DeleteIndex(rootB);
    return 0;
}
