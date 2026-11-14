// 内容索引：独立于文件名索引的一个库。
//
// 为什么不塞进 index_<root>.db：
//   那个库每次扫描都是全量重建（写 .building 再改名覆盖），
//   正文跟着一起重建的话，每次重扫都要把几百 MB 文本重新抽一遍。
// 所以单独一个 content_<root>_<hash>.db，按 (size, mtime) 增量，跨多次扫描复用。
//
// 作用域：一个扫描根一份库。搜索默认只看当前根（"当前目录"），
// 但界面可以切到"所有已索引"，那走 SearchAll —— 把每份库都查一遍再合并去重。
// 这样"换个文件夹就搜不到"不再是死路：建过索引的目录都还在。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.h"

namespace content {

// 根路径规范化：'/' → '\'、去掉尾部反斜杠（盘根保留 "X:\"）、盘符大写。
// Build / Search / DbPathFor 全部先过这里 —— 否则 "E:/a" 和 "E:\a"
// 会存成两种 path，增量比对失配、结果里还会出现 E:/a\b 这种混合分隔符。
std::string NormRoot(const std::string& root);

std::string DbPathFor(const std::string& root);
bool HasIndex(const std::string& root);

// 已建立好的一份内容索引（给"索引管理"用）
struct IndexInfo {
    std::string root;          // 这份索引覆盖的目录
    std::string dbPath;
    int64_t  docs = 0;         // 抽到正文的文档数
    uint64_t bytes = 0;        // 库文件体积（不含 -wal/-shm）
    int64_t  builtAt = 0;      // Unix 秒
    bool     partial = false;  // 上次没建完（中途停止）
    bool     stale = false;    // 抽取器版本变了，下次更新会整库重建
};
std::vector<IndexInfo> ListIndexes();

// ListIndexes 的缓存版：要打开每一份库读 meta，27 份约 0.7 秒
// （老库缺 docs 元数据时更慢）。界面上每帧调一次 = 每帧冻 0.7 秒，
// 所以绘制路径一律走这里，只在缓存变脏时读盘。
std::vector<IndexInfo> IndexSnapshot();
void InvalidateIndexes();   // 建库 / 删库 / 增量更新后调用

// FTS5 倒排索引 + CJK bigram 分词；排序用「命中次数/文档长度」（bm25 在本环境不可靠）。
// 单字查询或 MATCH 不可用时退回 LIKE 全表扫描。
std::vector<db::Hit> Search(const std::string& root, const std::string& query, int limit,
                           const std::atomic<bool>* cancel = nullptr);

// 跨所有已建索引搜索：每份库各查一次，按路径去重，按相关度合并。
// 同一个文件同时被父目录和子目录索引过时只会出现一条。
// cancel 非空时逐库检查，用于"搜索中关窗口"能立刻退出
std::vector<db::Hit> SearchAll(const std::string& query, int limit,
                              const std::atomic<bool>* cancel = nullptr);

// 删掉这份索引（连同 -wal / -shm 残留）。按库文件删，root 元信息坏掉的也能删。
bool DeleteIndex(const std::string& root);
bool DeleteIndexFile(const std::string& dbPath);

// 索引里有哪些扩展名、各多少条（给界面做后缀筛选）
struct ExtCount { std::string ext; int count; };
std::vector<ExtCount> ListExtensions(const std::string& root);

// 后台线程跑。cancel 置位即尽快退出（已写入的部分保留）。
void Build(const std::string& root, const std::atomic<bool>& cancel);

// 进度：Build 写，UI 读
extern std::atomic<int>  g_seen;     // 遍历到的候选文件数（= 本次任务总量）
extern std::atomic<int>  g_done;     // 已处理（含跳过）
extern std::atomic<int>  g_parsed;   // 实际重新抽取的
extern std::atomic<int>  g_skipped;  // 命中增量、直接复用上次结果的
extern std::atomic<bool> g_busy;
extern std::atomic<int>  g_files;    // 库里已有的文档数
extern std::atomic<int>  g_passes;   // 0=清点中 1=抽取中

std::string CurrentPath();
std::string LastError();
// 需要让用户知道、但不是错误的事（旧索引作废、被中途停止……）
std::string LastNotice();
void ResetProgress();

}  // namespace content
