#include "rules.h"
#include "i18n.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace rules {

typedef std::wstring WString;

static WString Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

static std::string Expand(const std::string& raw) {
    // 必须用 W 版：A 版在中文用户名下返回 GBK 字节，再被当成 UTF-8 解释，
    // 结果 %LOCALAPPDATA% 展开成乱码，所有用户缓存规则都静默失效。
    wchar_t buf[1024];
    DWORD n = ExpandEnvironmentStringsW(Widen(raw).c_str(), buf,
                                       (DWORD)(sizeof buf / sizeof buf[0]));
    std::string out = (n > 0 && n <= sizeof buf / sizeof buf[0]) ? Narrow(buf) : raw;
    // 去掉尾部的反斜杠，比较时更省事
    while (out.size() > 3 && (out.back() == '\\' || out.back() == '/')) out.pop_back();
    return out;
}

static std::string Lower(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    std::replace(s.begin(), s.end(), '/', '\\');
    return s;
}

bool IsProtected(const std::string& path) {
    static const char* kProtected[] = {
        "c:\\windows", "c:\\program files", "c:\\program files (x86)", "c:\\programdata",
    };
    std::string p = Lower(path);
    for (const char* base : kProtected) {
        size_t len = strlen(base);
        if (p.compare(0, len, base) == 0 && (p.size() == len || p[len] == '\\')) return true;
    }
    return false;
}

std::vector<Rule> All() {
    std::vector<Rule> out;
    auto add = [&](const char* id, const char* name, const char* why, int risk,
                   std::initializer_list<const char*> paths, bool glob = false,
                   bool admin = false) {
        Rule r;
        r.id = id;
        r.name = name;
        r.why = why;
        r.risk = risk;
        r.glob = glob;
        r.needsAdmin = admin;
        for (const char* p : paths) r.paths.push_back(Expand(p));
        out.push_back(std::move(r));
    };

    // ---- 可直接清理（用户目录下的缓存，普通权限就能读能删）
    add("user-temp", TR("用户临时文件", "User temp files"),
        TR("程序和安装包留下的临时文件，重启后大多已经没用。", "Temp files left by apps and installers; mostly useless after a reboot."), SAFE,
        {"%TEMP%"});
    add("thumbnail-cache", TR("缩略图缓存", "Thumbnail cache"),
        TR("资源管理器的缩略图数据库，会自动重建。", "Explorer's thumbnail database; rebuilt automatically."), SAFE,
        {"%LOCALAPPDATA%\\Microsoft\\Windows\\Explorer\\thumbcache_*.db"}, true);
    add("crash-dumps", TR("崩溃转储", "Crash dumps"),
        TR("程序崩溃时写下的转储文件，排查完就没用了。", "Dump files written on crashes; useless once investigated."), SAFE,
        {"%LOCALAPPDATA%\\CrashDumps"});
    add("pip-cache", TR("pip 缓存", "pip cache"),
        TR("下载过的 wheel 包缓存，重装时会重新下载。", "Downloaded wheel cache; re-downloaded when reinstalling."), SAFE,
        {"%LOCALAPPDATA%\\pip\\Cache"});
    add("uv-cache", TR("uv 缓存", "uv cache"),
        TR("uv 的包缓存，可用 uv cache clean 重建。", "uv package cache; rebuild with \"uv cache clean\"."), SAFE,
        {"%LOCALAPPDATA%\\uv\\cache"});
    add("npm-cache", TR("npm 缓存", "npm cache"),
        TR("npm 的本地包缓存。", "npm local package cache."), SAFE,
        {"%LOCALAPPDATA%\\npm-cache"});
    add("browser-cache-chrome", TR("Chrome 缓存", "Chrome cache"),
        TR("网页缓存，删除后首次访问会稍慢，不影响登录状态。", "Web cache; first visits get slower, logins unaffected."), SAFE,
        {"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Cache",
         "%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\Code Cache",
         "%LOCALAPPDATA%\\Google\\Chrome\\User Data\\Default\\GPUCache"});
    add("browser-cache-edge", TR("Edge 缓存", "Edge cache"),
        TR("网页缓存，删除后首次访问会稍慢。", "Web cache; first visits get slower."), SAFE,
        {"%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Cache",
         "%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\Code Cache",
         "%LOCALAPPDATA%\\Microsoft\\Edge\\User Data\\Default\\GPUCache"});
    add("electron-cache", TR("Electron 应用缓存", "Electron app caches"),
        TR("VS Code、Cursor、Discord 等应用的缓存与日志。", "Caches and logs of VS Code, Cursor, Discord, etc."), SAFE,
        {"%APPDATA%\\Code\\Cache", "%APPDATA%\\Code\\CachedData",
         "%APPDATA%\\Code\\logs", "%APPDATA%\\discord\\Cache"});
    add("recycle-bin", TR("回收站", "Recycle Bin"),
        TR("清空回收站。删掉的文件就找不回来了。", "Empty the Recycle Bin. Deleted files cannot be recovered."), SAFE,
        {"%SYSTEMDRIVE%\\$Recycle.Bin"});

    // ---- 需要管理员权限的系统目录（检测和删除都可能失败，单独标注）
    add("windows-temp", TR("Windows 临时目录", "Windows temp folder"),
        TR("系统与安装程序共用的临时目录，需要管理员权限。", "Shared system temp folder; requires admin rights."), SAFE,
        {"%WINDIR%\\Temp"}, false, true);
    add("update-download", TR("Windows 更新下载缓存", "Windows Update download cache"),
        TR("已安装更新的安装包缓存，需要管理员权限。", "Installer cache of installed updates; requires admin rights."), SAFE,
        {"%WINDIR%\\SoftwareDistribution\\Download"}, false, true);
    add("wer-reports", TR("Windows 错误报告", "Windows Error Reporting"),
        TR("错误报告队列，需要管理员权限。", "Error report queue; requires admin rights."), SAFE,
        {"%PROGRAMDATA%\\Microsoft\\Windows\\WER"}, false, true);

    // ---- 确认后清理
    add("chrome-ondevice-model", TR("Chrome 端侧 AI 模型", "Chrome on-device AI models"),
        TR("Chrome 内置的端侧 AI 模型，每份约 4 GB，多个浏览器副本各存一份。删了下次用到会重新下载。", "Chrome built-in on-device AI models, ~4 GB each, one copy per browser profile. Re-downloaded when needed."),
        CAUTION, {"%LOCALAPPDATA%\\Google\\Chrome\\User Data\\OptGuideOnDeviceModel",
                  "%LOCALAPPDATA%\\Google\\Chrome Beta\\User Data\\OptGuideOnDeviceModel"});
    add("pnpm-store", TR("pnpm 全局存储", "pnpm global store"),
        TR("pnpm 用硬链接共享这个存储，删了已装项目的依赖会一起坏。", "pnpm hard-links this store; deleting it breaks installed projects."), CAUTION,
        {"%LOCALAPPDATA%\\pnpm\\store"});
    add("winsxs-backup", TR("组件存储备份 (WinSxS)", "Component store backup (WinSxS)"),
        TR("微软建议用 DISM 而不是手删；手删会破坏组件存储。", "Microsoft recommends DISM; manual deletion corrupts the component store."), CAUTION,
        {"%WINDIR%\\WinSxS\\Backup"}, false, true);
    add("windows-logs", TR("Windows 日志", "Windows logs"),
        TR("系统日志文件，排查问题时会用到。", "System log files; useful when troubleshooting."), CAUTION,
        {"%WINDIR%\\Logs"}, false, true);

    return out;
}

// ============================================================ 诊断日志
//
// 把检测过程写到固定日志文件，方便定位"到底卡在哪"。
// 日志路径用 ASCII，避免 ExpandEnvironmentStringsA 还没展开时的编码问题。
static std::mutex g_logMutex;
static FILE* g_logFp = nullptr;

static std::string LogPath() {
    char buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("TEMP", buf, sizeof buf);
    if (n == 0 || n >= sizeof buf) {
        n = GetEnvironmentVariableA("TMP", buf, sizeof buf);
    }
    std::string dir = (n > 0 && n < sizeof buf) ? std::string(buf) : std::string("C:\\temp");
    while (!dir.empty() && (dir.back() == '\\' || dir.back() == '/')) dir.pop_back();
    return dir + "\\spacesift_detect.log";
}

static void DetectLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMutex);
    if (!g_logFp) {
        g_logFp = fopen(LogPath().c_str(), "w");
        if (!g_logFp) return;
    }
    auto now = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    fprintf(g_logFp, "[%lld] ", (long long)ms);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_logFp, fmt, ap);
    va_end(ap);
    fprintf(g_logFp, "\n");
    fflush(g_logFp);
}

// ============================================================ PathSize

uint64_t PathSize(const std::string& rawPath, bool glob, uint64_t* fileCount, int timeoutMs) {
    DetectLog("PathSize START path=%s glob=%d timeoutMs=%d", rawPath.c_str(), (int)glob, timeoutMs);
    auto started = std::chrono::steady_clock::now();
    long long tick = 0;
    uint64_t total = 0;
    uint64_t count = 0;
    std::wstring wPath = Widen(rawPath);

    if (glob) {
        // 只处理同一目录下的通配（规则表里就是这么用的）
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(wPath.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow, ++count;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    } else {
        DWORD attr = GetFileAttributesW(wPath.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES) {
            if (fileCount) *fileCount = 0;
            DetectLog("PathSize END path=%s attr=missing size=0 files=0 elapsed=0", rawPath.c_str());
            return 0;
        }
        if (attr & FILE_ATTRIBUTE_DIRECTORY) {
            // 显式栈而不是递归 lambda：lambda 不能在自身定义里调用自己
            std::vector<std::wstring> pending;
            pending.push_back(wPath);
            while (!pending.empty()) {
                // 每 200 个目录查一次时间，超时就带着已统计的部分返回
                if (timeoutMs > 0 && ++tick % 200 == 0) {
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - started).count();
                    if (ms > timeoutMs) {
                        DetectLog("PathSize TIMEOUT path=%s size=%llu files=%llu elapsed=%lld",
                                  rawPath.c_str(), (unsigned long long)total,
                                  (unsigned long long)count, (long long)ms);
                        break;
                    }
                }
                std::wstring dir = pending.back();
                pending.pop_back();
                std::wstring pattern = dir + L"\\*";
                WIN32_FIND_DATAW fd;
                HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd,
                                            FindExSearchNameMatch, nullptr,
                                            FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) continue;
                bool overtime = false;
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)
                        continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        pending.push_back(dir + L"\\" + fd.cFileName);
                    } else {
                        total += ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                        ++count;
                    }
                    // 按条目计数查超时：大目录里文件动辄上百万，只看目录数会漏
                    if (timeoutMs > 0 && ++tick % 20000 == 0) {
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - started).count();
                        if (ms > timeoutMs) { overtime = true; break; }
                    }
                } while (FindNextFileW(h, &fd));
                FindClose(h);
                if (overtime) {
                    DetectLog("PathSize TIMEOUT path=%s size=%llu files=%llu elapsed=%lld",
                              rawPath.c_str(), (unsigned long long)total,
                              (unsigned long long)count, (long long)timeoutMs);
                    break;
                }
            }
        } else {
            WIN32_FILE_ATTRIBUTE_DATA data;
            if (GetFileAttributesExW(wPath.c_str(), GetFileExInfoStandard, &data))
                total = ((uint64_t)data.nFileSizeHigh << 32) | data.nFileSizeLow;
            count = 1;
        }
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - started).count();
    DetectLog("PathSize END path=%s size=%llu files=%llu elapsed=%lld",
              rawPath.c_str(), (unsigned long long)total, (unsigned long long)count,
              (long long)elapsed);
    if (fileCount) *fileCount = count;
    return total;
}

// ============================================================ 带超时的单规则执行

namespace {

// 在独立 Windows 线程里跑 fn，主线程最多等 timeoutMs。
// 返回 true 表示正常完成；false 表示超时，此时线程被放弃（可能还在 stuck I/O 里）。
// 用 CreateThread 而不是 std::thread，确保 native handle 是 CancelSynchronousIo 需要的 HANDLE。
//
// 关键：fn 的结果写在共享的 TimeoutTask 里，不用调用方的栈变量。
// 旧写法 lambda 按引用捕获栈上的 Finding，超时后调用方已经返回，
// 被放弃的线程还在写那块栈内存 —— use-after-free。
struct TimeoutTask {
    std::function<void(TimeoutTask&)> fn;
    std::mutex m;
    Finding out;
    std::atomic<bool> done{false};
};

bool RunWithTimeout(std::function<void(TimeoutTask&)> fn, int timeoutMs, Finding& result) {
    auto ctx = std::make_shared<TimeoutTask>();
    ctx->fn = std::move(fn);
    auto* box = new std::shared_ptr<TimeoutTask>(ctx);   // 线程自己持一份引用

    DWORD tid = 0;
    HANDLE hThread = CreateThread(nullptr, 0,
                                  [](LPVOID p) -> DWORD {
                                      auto* b = static_cast<std::shared_ptr<TimeoutTask>*>(p);
                                      TimeoutTask& c = **b;
                                      try {
                                          c.fn(c);
                                      } catch (...) {
                                          // 任何异常都按完成处理，避免检测线程崩溃
                                      }
                                      c.done.store(true);
                                      delete b;  // 这一份引用结束；若已超时，父线程那份仍在
                                      return 0;
                                  },
                                  box, 0, &tid);
    if (!hThread) {
        delete box;
        return false;
    }

    auto started = std::chrono::steady_clock::now();
    bool ok = true;
    while (!ctx->done.load()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started).count();
        if (elapsed > timeoutMs) {
            // 先尝试取消阻塞 I/O；如果线程正在 FindFirstFile/FindNextFile 里卡住，
            // 这一招在本地 NTFS 上通常能让它醒过来。
            CancelSynchronousIo(hThread);
            Sleep(80);
            if (ctx->done.load()) break;  // 取消成功，线程及时退出了
            // 仍然没退出：线程卡在不可取消的调用里。标记超时并放弃这个线程。
            ok = false;
            break;
        }
        Sleep(25);
    }
    CloseHandle(hThread);

    // 无论正常结束还是超时，都在锁内拷出已经算出的部分结果
    {
        std::lock_guard<std::mutex> lk(ctx->m);
        result = ctx->out;
    }
    return ok;
}

}  // namespace

std::vector<Finding> Detect(const std::vector<Rule>& all, ProgressFn progress,
                            std::atomic<bool>* cancel) {
    std::vector<Finding> out;
    const int kRuleTimeoutMs = 10000;  // 单条规则总时长上限（含所有 path）
    const int kPathTimeoutMs = 8000;   // 单个 PathSize 内部上限

    for (size_t i = 0; i < all.size(); ++i) {
        if (cancel && cancel->load()) {
            DetectLog("Detect CANCELLED at index=%zu", i);
            break;
        }
        const Rule& rule = all[i];
        auto ruleStarted = std::chrono::steady_clock::now();
        DetectLog("Detect RULE_START idx=%zu/%zu id=%s name=%s", i + 1, all.size(),
                  rule.id.c_str(), rule.name.c_str());

        Finding f;
        f.rule = &rule;
        Finding partial;
        bool ok = RunWithTimeout([&](TimeoutTask& t) {
            uint64_t files = 0;
            for (const std::string& path : rule.paths) {
                if (cancel && cancel->load()) return;
                DWORD attr = GetFileAttributesW(Widen(path).c_str());
                if (attr == INVALID_FILE_ATTRIBUTES && !rule.glob) continue;
                uint64_t n = 0;
                uint64_t sz = PathSize(path, rule.glob, &n, kPathTimeoutMs);
                // 0 字节的目录不展示：右栏一排「0 B」既没信息也没法清
                if (sz > 0) {
                    std::lock_guard<std::mutex> lk(t.m);
                    t.out.existing.push_back(path);
                    t.out.pathSize.push_back(sz);
                    t.out.size += sz;
                    files += n;
                    t.out.files = files;
                }
            }
        }, kRuleTimeoutMs, partial);
        partial.rule = &rule;
        f = std::move(partial);

        if (!ok) {
            f.timedOut = true;
            DetectLog("Detect RULE_TIMEOUT idx=%zu/%zu id=%s name=%s partial_size=%llu partial_files=%llu",
                      i + 1, all.size(), rule.id.c_str(), rule.name.c_str(),
                      (unsigned long long)f.size, (unsigned long long)f.files);
        } else {
            DetectLog("Detect RULE_END idx=%zu/%zu id=%s name=%s size=%llu files=%llu",
                      i + 1, all.size(), rule.id.c_str(), rule.name.c_str(),
                      (unsigned long long)f.size, (unsigned long long)f.files);
        }

        // 超时时保留已经算出来的部分结果，而不是直接丢弃；
        // 这样用户至少能看到"部分可清理"。
        if (f.size > 0 || !f.existing.empty() || f.timedOut) {
            out.push_back(std::move(f));
        }

        int elapsedMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - ruleStarted).count();
        if (progress) progress(rule.name, (int)i, (int)all.size(), elapsedMs,
                               out.empty() ? nullptr : &out.back());
    }

    std::sort(out.begin(), out.end(), [](const Finding& a, const Finding& b) {
        return a.size > b.size;
    });
    DetectLog("Detect DONE findings=%zu", out.size());
    return out;
}

}  // namespace rules
