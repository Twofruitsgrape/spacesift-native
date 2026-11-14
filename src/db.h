// 文件名索引：SQLite 封装。
//
// 和 Python 版一样的设计 —— 扫描的同时把每个文件写进库，
// 这样「扫完」和「能搜」是同一个时刻，不用再走第二遍盘。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace db {

struct Hit {
    std::string path;
    std::string name;
    uint64_t size = 0;
    bool isDir = false;
    // 内容检索命中时的正文片段；文件名检索留空
    std::string snippet;
    bool content = false;
    // 相关度（内容检索=关键词出现次数）。跨多份索引合并时按它排序
    int score = 0;
};

// 每个盘/目录一份库，放在 %LOCALAPPDATA%/SpaceSiftNative 下
std::string IndexPathFor(const std::string& root);
// 同一套命名规则，换成别的前缀（内容索引靠它拿到 content_<root>.db）
std::string IndexPathWithPrefix(const std::string& root, const char* prefix);
std::string DataDir();

// 删除超过 keepDays 没更新的索引库，以及中断残留的 .building 临时文件。
void PruneOld(int keepDays);

// 打开库并建表；已有库直接复用。失败返回 false。
bool Open(const std::string& dbPath);
bool OpenExisting(const std::string& dbPath);   // 只读打开已有库（搜索用）
void Close();

// 批量写入（内部用事务，几千条一次提交）
void BeginBulk();
// 返回这一行自己的 id；子项拿它当 parent
long long Add(long long parent, const std::string& name, uint64_t size, bool isDir);
void FlushBulk();
bool EndBulk(const std::string& root, bool partial);   // 写 meta 并换名
void AbortBulk();

// 查询：按文件名子串匹配，按大小降序
std::vector<Hit> Search(const std::string& query, int limit = 400);

// 同一件事，但显式指定库文件：自开连接、不碰全局状态，可以在后台线程里跑。
std::vector<Hit> SearchIn(const std::string& dbPath, const std::string& query, int limit = 400,
                          const std::atomic<bool>* cancel = nullptr);

// 已建立的文件名索引清单（给"检索范围 = 所有已索引"与索引管理用）
struct IndexInfo {
    std::string root;
    std::string dbPath;
    long long   files = 0;
    long long   builtAt = 0;
    bool        partial = false;
};
std::vector<IndexInfo> ListIndexes();

// 跨所有已建索引按文件名搜：每份库各查一次，按路径去重，按大小合并。
std::vector<Hit> SearchAll(const std::string& query, int limit = 400,
                           const std::atomic<bool>* cancel = nullptr);

uint64_t Count();
bool IsFresh(const std::string& root, int maxAgeHours = 12);

}  // namespace db
