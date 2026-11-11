// 内容索引：独立于文件名索引的一个库。
//
// 为什么不塞进 index_<root>.db：
//   那个库每次扫描都是全量重建（写 .building 再改名覆盖），
//   正文跟着一起重建的话，每次重扫都要把几百 MB 文本重新抽一遍。
// 所以单独一个 content_<root>.db，按 (size, mtime) 增量，跨多次扫描复用。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "db.h"

namespace content {

std::string DbPathFor(const std::string& root);
bool HasIndex(const std::string& root);

// P1 用 LIKE 子串匹配：对中文天然正确，不需要分词，也就不用开 FTS5。
std::vector<db::Hit> Search(const std::string& root, const std::string& query, int limit);

// 后台线程跑。cancel 置位即尽快退出（已写入的部分保留）。
void Build(const std::string& root, const std::atomic<bool>& cancel);

// 进度：Build 写，UI 读
extern std::atomic<int>  g_seen;     // 遍历到的候选文件数
extern std::atomic<int>  g_parsed;   // 实际抽取过的文件数
extern std::atomic<bool> g_busy;
extern std::atomic<int>  g_files;    // 库里已有的文档数

std::string CurrentPath();
std::string LastError();
void ResetProgress();

}  // namespace content
