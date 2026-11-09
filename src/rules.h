// 清理规则 —— 从 Python 版 rules.py 移植过来的那张表。
//
// 刻意写成硬编码常量而不是读配置文件：配置文件一旦被改坏（比如某人把路径写成
// "C:\\"），这个工具就会去删整个盘。硬编码至少随程序一起审查过。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace rules {

enum Risk { SAFE = 0, CAUTION = 1, MANUAL = 2 };

struct Rule {
    std::string id;
    std::string name;
    std::string why;
    std::vector<std::string> paths;   // 已展开成绝对路径
    int risk = SAFE;
    bool glob = false;
    bool needsAdmin = false;
    bool isDir = true;
};

struct Finding {
    const Rule* rule = nullptr;
    uint64_t size = 0;
    uint64_t files = 0;
    std::vector<std::string> existing;   // 真正存在的路径（清理时只动这些）
    std::vector<uint64_t>    pathSize;   // 和 existing 一一对应，用于按当前目录求小计
    bool timedOut = false;               // 这条规则是否因为超时被截断
};

// 全部规则（路径已展开）
std::vector<Rule> All();

// 进度回调：ruleName=当前规则名，idx/total=进度，elapsedMs=已耗时，
// partial=刚完成的 Finding（nullptr 表示心跳，还未完成）。
using ProgressFn = std::function<void(const std::string& ruleName, int idx, int total,
                                      int elapsedMs, const Finding* partial)>;

// 逐条检查并计算大小。阻塞调用，界面放在后台线程里跑。
// progress 用于流式更新界面；cancel 用于提前终止（如用户点结束）。
// 每条规则最多 10 秒：PathSize 内部 8 秒 + 外层 2 秒缓冲，防止某个路径把检测拖死。
std::vector<Finding> Detect(const std::vector<Rule>& all, ProgressFn progress = nullptr,
                            std::atomic<bool>* cancel = nullptr);

// 递归算一个目录/文件/通配符的总字节数与文件数。
// timeoutMs > 0 时超过这个时间就停下并返回已统计的部分 ——
// WinSxS\Backup 这类几十万硬链接的目录能把整个检测堵死好几分钟。
uint64_t PathSize(const std::string& path, bool glob, uint64_t* fileCount = nullptr,
                  int timeoutMs = 0);

// 受保护路径：这些地方的东西一律不动
bool IsProtected(const std::string& path);

}  // namespace rules
