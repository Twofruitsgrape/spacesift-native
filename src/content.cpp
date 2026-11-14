#include "content.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <regex>
#include <unordered_map>
#include <vector>

#include "extract.h"
#include "sqlite3.h"

namespace content {

std::atomic<int>  g_seen{0};
std::atomic<int>  g_done{0};
std::atomic<int>  g_parsed{0};
std::atomic<int>  g_skipped{0};
std::atomic<bool> g_busy{false};
std::atomic<int>  g_files{0};
std::atomic<int>  g_passes{0};

namespace {

std::mutex  g_mutex;          // 保护下面几个字符串
std::string g_curPath;
std::string g_lastError;
std::string g_lastNotice;

void SetCurrent(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_curPath = p;
}

void SetError(const std::string& e) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastError = e;
}

void SetNotice(const std::string& n) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastNotice = n;
}

bool Exec(sqlite3* db, const char* sql) {
    return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// 抽取器逻辑一改，旧正文就不能信了；这个值一改就整库重建。
// OCR 开关也算在内：关着时被记为"抽空"的扫描件，开了之后必须重来一遍。
// 8：原文只存前 16KB（倒排仍吃满 64KB，召回不变）；路径统一反斜杠。
std::string ExtractVersion() {
    return std::string("8") + (extract::OcrReady() ? "-ocr" : "");
}

std::string MetaGet(sqlite3* db, const char* key) {
    std::string out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT value FROM meta WHERE key=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            if (t) out = (const char*)t;
        }
        sqlite3_finalize(st);
    }
    return out;
}

void MetaPut(sqlite3* db, const char* key, const std::string& val) {
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                           -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 2, val.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
    }
}

int64_t FtToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const ULONGLONG epoch = 116444736000000000ULL;   // 1601 → 1970
    if (u.QuadPart < epoch) return 0;
    return (int64_t)((u.QuadPart - epoch) / 10000000ULL);
}

// ============================ 分词 ============================
//
// 为什么不用 jieba：FTS5 默认的 unicode61 会把一整串中文当成一个 token，
// "合同"永远搜不到；trigram 分词器又要求查询 ≥3 字，中文最常见的 2 字词直接失效。
// 这里自己做 **CJK bigram**：把"法兰盘"索引成 `法兰 兰盘`，查询时用短语
// `"法兰 兰盘"` 匹配 —— 等价于子串匹配，而且 BM25 有真实词频可用。

bool IsCjk(unsigned cp) {
    return (cp >= 0x3400 && cp <= 0x4DBF) ||   // 扩展 A
           (cp >= 0x4E00 && cp <= 0x9FFF) ||   // 基本汉字
           (cp >= 0xF900 && cp <= 0xFAFF) ||   // 兼容汉字
           (cp >= 0x3040 && cp <= 0x30FF) ||   // 日文假名
           (cp >= 0xAC00 && cp <= 0xD7AF);     // 韩文
}

// 解一个 UTF-8 码点，返回下一个位置
size_t NextCp(const std::string& s, size_t i, unsigned& cp) {
    const unsigned char c = (unsigned char)s[i];
    if (c < 0x80) { cp = c; return i + 1; }
    size_t n = ((c & 0xE0) == 0xC0) ? 2 : ((c & 0xF0) == 0xE0) ? 3 : ((c & 0xF8) == 0xF0) ? 4 : 0;
    if (n == 0 || i + n > s.size()) { cp = 0xFFFD; return i + 1; }
    cp = c & ((1u << (8 - (unsigned)n - 1)) - 1);
    for (size_t k = 1; k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) { cp = 0xFFFD; return i + 1; }
        cp = (cp << 6) | (cc & 0x3F);
    }
    return i + n;
}

void PutCp(std::string& o, unsigned cp) {
    if (cp < 0x80) {
        o.push_back((char)cp);
    } else if (cp < 0x800) {
        o.push_back((char)(0xC0 | (cp >> 6)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        o.push_back((char)(0xE0 | (cp >> 12)));
        o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        o.push_back((char)(0xF0 | (cp >> 18)));
        o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        o.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// 正文 → 索引 token 流（CJK bigram + 拉丁整词）
std::string FtsTokens(const std::string& text) {
    std::string o;
    o.reserve(text.size() + text.size() / 2);
    std::vector<unsigned> run;

    auto flush = [&] {
        for (size_t k = 0; k + 1 < run.size(); ++k) {
            PutCp(o, run[k]);
            PutCp(o, run[k + 1]);
            o.push_back(' ');
        }
        if (run.size() == 1) {          // 孤字单独存一份，单字查询才能命中
            PutCp(o, run[0]);
            o.push_back(' ');
        }
        run.clear();
    };

    size_t i = 0;
    while (i < text.size()) {
        const unsigned char c = (unsigned char)text[i];
        if (c < 0x80) {
            flush();
            if (isalnum(c)) {
                while (i < text.size() && (unsigned char)text[i] < 0x80 &&
                       (isalnum((unsigned char)text[i]) || text[i] == '_')) {
                    o.push_back((char)tolower((unsigned char)text[i]));
                    ++i;
                }
                o.push_back(' ');
                continue;
            }
            ++i;
            continue;
        }
        unsigned cp = 0;
        const size_t ni = NextCp(text, i, cp);
        if (IsCjk(cp)) run.push_back(cp);
        else flush();
        i = ni;
    }
    flush();
    return o;
}

// 查询 → FTS5 MATCH 表达式；返回空表示"交给 LIKE 兜底"
std::string FtsQuery(const std::string& q) {
    std::string out;
    std::vector<unsigned> run;

    auto flush = [&] {
        if (run.size() >= 2) {
            std::string ph;
            for (size_t k = 0; k + 1 < run.size(); ++k) {
                if (!ph.empty()) ph.push_back(' ');
                PutCp(ph, run[k]);
                PutCp(ph, run[k + 1]);
            }
            if (!out.empty()) out += " AND ";
            out += "\"";
            out += ph;
            out += "\"";
        } else if (run.size() == 1) {
            if (!out.empty()) out += " AND ";
            PutCp(out, run[0]);          // 索引里只有单字成 run 时才存在
        }
        run.clear();
    };

    size_t i = 0;
    while (i < q.size()) {
        const unsigned char c = (unsigned char)q[i];
        if (c < 0x80) {
            flush();
            if (isalnum(c)) {
                std::string w;
                while (i < q.size() && (unsigned char)q[i] < 0x80 &&
                       (isalnum((unsigned char)q[i]) || q[i] == '_')) {
                    w.push_back((char)tolower((unsigned char)q[i]));
                    ++i;
                }
                if (!out.empty()) out += " AND ";
                out += w;
                continue;
            }
            ++i;
            continue;
        }
        unsigned cp = 0;
        const size_t ni = NextCp(q, i, cp);
        if (IsCjk(cp)) run.push_back(cp);
        else flush();
        i = ni;
    }
    flush();
    return out;
}

// LIKE 里的 % _ \ 要转义
std::string LikeEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 4);
    for (char c : s) {
        if (c == '%' || c == '_' || c == '\\') o.push_back('\\');
        o.push_back(c);
    }
    return o;
}

// 片段里的换行压成空格，方便一行显示
std::string Flatten(const std::string& s, size_t maxChars) {
    std::string o;
    o.reserve(std::min(s.size(), maxChars + 8));
    bool space = false;
    int chars = 0;
    for (size_t i = 0; i < s.size() && chars < (int)maxChars; ++i) {
        const unsigned char c = (unsigned char)s[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = !o.empty();
            continue;
        }
        if (space) { o.push_back(' '); space = false; ++chars; }
        o.push_back((char)c);
        if ((c & 0xC0) != 0x80) ++chars;
    }
    if ((size_t)chars >= maxChars && !s.empty()) o += "…";
    return o;
}

// 这些目录里绝大多数是依赖/版本库，抽出来只会把索引撞大、把搜索结果洗掉
const char* kSkipDirs[] = {
    "$recycle.bin", "system volume information", "recovery",
    "node_modules", "__pycache__", ".venv", "venv", ".svn", ".hg",
    "site-packages", ".cache",
};

bool SkipDir(const std::wstring& name) {
    std::wstring low;
    low.reserve(name.size());
    for (wchar_t c : name) low.push_back((wchar_t)towlower(c));
    for (const char* s : kSkipDirs) {
        size_t i = 0;
        for (; s[i]; ++i) if (i >= low.size() || low[i] != (wchar_t)s[i]) break;
        if (!s[i] && i == low.size()) return true;
    }
    return false;
}

using FileFn = std::function<void(const std::string&, uint64_t, int64_t)>;

void Walk(const std::wstring& dir, const std::atomic<bool>& cancel, const FileFn& onFile) {
    if (cancel.load()) return;

    std::wstring pattern = dir;
    if (!pattern.empty() && pattern.back() != L'\\') pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subs;
    do {
        if (cancel.load()) break;
        const wchar_t* n = fd.cFileName;
        if (n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0))) continue;
        // 少扫一点系统垃圾，也避免重解析点（junction/软链）把遍历绕成环
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;

        std::wstring full = dir;
        if (!full.empty() && full.back() != L'\\') full += L'\\';
        full += n;

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!SkipDir(n)) subs.push_back(full);
            continue;
        }

        const uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
        if (size == 0) continue;

        std::string u8 = extract::FromWide(full);
        const std::string ext = extract::LowerExt(u8);
        if (!extract::Supports(ext)) continue;
        // zip/.mph 是流式读的，体积大也不影响（COMSOL 模型动辄几百 MB 到几 GB）
        if (size > extract::kSkipAbove && !extract::StreamsLargeFiles(ext)) continue;

        // 候选计数交给调用方：这里加会让两阶段遍历把总量算成两倍
        SetCurrent(u8);
        onFile(u8, size, FtToUnix(fd.ftLastWriteTime));
    } while (FindNextFileW(h, &fd));

    FindClose(h);

    for (auto& d : subs) {
        if (cancel.load()) break;
        Walk(d, cancel, onFile);
    }
}

}  // namespace

std::string NormRoot(const std::string& root) {
    std::string s = root;
    if (s.empty()) return s;
    for (auto& c : s) if (c == '/') c = '\\';
    // 盘符大写：E:\ 和 e:\ 必须是同一份库，否则 Windows 不区分大小写、
    // 而字符串区分，同一个目录会各建一份 600 MB 的库
    if (s.size() >= 2 && s[1] == ':' && s[0] >= 'a' && s[0] <= 'z')
        s[0] = (char)toupper((unsigned char)s[0]);
    while (s.size() > 3 && s.back() == '\\') s.pop_back();
    if (s.size() == 2 && s[1] == ':') s += '\\';
    return s;
}

namespace {

// 旧命名：路径压平后硬截断 40 字符。两个长目录会撞到同一个库
// （实测 E:\Projects\comsol with ai dsh\build_cache 已被砍成 ...build_cac），
// 结果就是搜 A 目录搜出 B 目录的文件，甚至把 B 目录的文件删掉。
std::string LegacyDbPath(const std::string& norm) {
    return db::IndexPathWithPrefix(norm, "content_");
}

uint32_t Fnv1a(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) { h ^= c; h *= 16777619u; }
    return h;
}

// 旧库改名到新名字（保留用户已经建好的索引，不强制重建）
void EnsureMigrated(const std::string& norm, const std::string& newPath) {
    const std::wstring wNew = extract::ToWide(newPath);
    if (GetFileAttributesW(wNew.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    const std::string old = LegacyDbPath(norm);
    if (old == newPath) return;
    const std::wstring wOld = extract::ToWide(old);
    if (GetFileAttributesW(wOld.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (MoveFileExW(wOld.c_str(), wNew.c_str(), MOVEFILE_COPY_ALLOWED)) {
        MoveFileExW((wOld + L"-wal").c_str(), (wNew + L"-wal").c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW((wOld + L"-shm").c_str(), (wNew + L"-shm").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
}

}  // namespace

std::string DbPathFor(const std::string& root) {
    const std::string norm = NormRoot(root);
    std::string safe;
    safe.reserve(norm.size());
    for (char c : norm) {
        if (isalnum((unsigned char)c) || c == '_') safe.push_back(c);
        else safe.push_back('_');
    }
    if (safe.size() > 33) safe.resize(33);
    char hex[16];
    snprintf(hex, sizeof hex, "_%08x", Fnv1a(norm));
    return db::DataDir() + "\\content_" + safe + hex + ".db";
}

bool HasIndex(const std::string& root) {
    const std::string norm = NormRoot(root);
    const std::string path = DbPathFor(norm);
    EnsureMigrated(norm, path);
    std::wstring w = extract::ToWide(path);
    const DWORD a = GetFileAttributesW(w.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string CurrentPath() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_curPath;
}

std::string LastError() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_lastError;
}

std::string LastNotice() {
    std::lock_guard<std::mutex> lk(g_mutex);
    return g_lastNotice;
}

void ResetProgress() {
    extract::ResetOcrBudget();
    g_seen = 0;
    g_done = 0;
    g_parsed = 0;
    g_skipped = 0;
    g_passes = 0;
    { std::lock_guard<std::mutex> lk(g_mutex); g_curPath.clear(); g_lastError.clear(); g_lastNotice.clear(); }
}

// 扫描数据目录，把每份内容库的 root / 条目数 / 体积 / 时间读出来。
// 只读 meta，不 COUNT(*) —— docs 表动辄几十万行，逐库数一遍会把搜索拖慢。
std::vector<IndexInfo> ListIndexes() {
    std::vector<IndexInfo> out;
    const std::string dir = db::DataDir();
    if (dir.empty()) return out;

    std::wstring pattern = extract::ToWide(dir) + L"\\content_*.db";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;

    const std::string curVer = ExtractVersion();
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const std::string name = extract::FromWide(fd.cFileName);
        const std::string full = dir + "\\" + name;
        if (name.size() > 12 && name.compare(name.size() - 12, 12, ".db.building") == 0) continue;

        IndexInfo info;
        info.dbPath = full;
        info.bytes = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;

        sqlite3* db = nullptr;
        if (sqlite3_open_v2(full.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
            info.root = NormRoot(MetaGet(db, "root"));
            info.builtAt = atoll(MetaGet(db, "built_at").c_str());
            info.partial = MetaGet(db, "partial") == "1";
            info.stale = MetaGet(db, "extract_version") != curVer;
            const std::string docs = MetaGet(db, "docs");
            if (!docs.empty()) {
                info.docs = atoll(docs.c_str());
            } else {
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM docs WHERE err=0",
                                       -1, &st, nullptr) == SQLITE_OK) {
                    if (sqlite3_step(st) == SQLITE_ROW) info.docs = sqlite3_column_int64(st, 0);
                    sqlite3_finalize(st);
                }
            }
            sqlite3_close(db);
        } else if (db) {
            sqlite3_close(db);
        }
        if (info.root.empty()) info.root = "(未知)";
        out.push_back(std::move(info));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    std::sort(out.begin(), out.end(), [](const IndexInfo& a, const IndexInfo& b) {
        return a.bytes > b.bytes;
    });
    return out;
}

bool DeleteIndexFile(const std::string& dbPath) {
    const std::wstring w = extract::ToWide(dbPath);
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    const BOOL ok = DeleteFileW(w.c_str());
    DeleteFileW((w + L"-wal").c_str());
    DeleteFileW((w + L"-shm").c_str());
    return ok != 0;
}

bool DeleteIndex(const std::string& root) {
    return DeleteIndexFile(DbPathFor(NormRoot(root)));
}

std::vector<ExtCount> ListExtensions(const std::string& root) {
    std::vector<ExtCount> out;
    if (!HasIndex(root)) return out;
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(DbPathFor(root).c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db,
            "SELECT ext, COUNT(*) FROM docs GROUP BY ext ORDER BY COUNT(*) DESC",
            -1, &st, nullptr) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* e = sqlite3_column_text(st, 0);
            ExtCount c;
            c.ext = e ? (const char*)e : "";
            c.count = (int)sqlite3_column_int64(st, 1);
            if (!c.ext.empty()) out.push_back(std::move(c));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

std::string TruncateUtf8(const std::string& s, size_t cap);   // 定义在查询语法段

void Build(const std::string& root, const std::atomic<bool>& cancel) {
    ResetProgress();
    g_busy = true;

    const std::string norm = NormRoot(root);
    const std::string dbPath = DbPathFor(norm);
    EnsureMigrated(norm, dbPath);
    const std::string version = ExtractVersion();

    // 抽取器升级 → 旧正文不能信。直接删库重建，而不是 DROP TABLE：
    // 删表不回收页面，6 GB 的库删完还占 6 GB，用户只会觉得"索引越用越大"。
    {
        const std::wstring w = extract::ToWide(dbPath);
        if (GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES) {
            bool hasDocs = false;
            std::string oldVer;
            sqlite3* probe = nullptr;
            if (sqlite3_open_v2(dbPath.c_str(), &probe, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK) {
                oldVer = MetaGet(probe, "extract_version");
                sqlite3_stmt* st = nullptr;
                if (sqlite3_prepare_v2(probe, "SELECT 1 FROM docs LIMIT 1", -1, &st, nullptr) == SQLITE_OK) {
                    hasDocs = sqlite3_step(st) == SQLITE_ROW;
                    sqlite3_finalize(st);
                }
                sqlite3_close(probe);
            } else if (probe) {
                sqlite3_close(probe);
            }
            if (hasDocs && oldVer != version) {
                SetNotice("抽取参数已升级，旧正文索引作废，本次重新抽取");
                DeleteFileW(w.c_str());
                DeleteFileW((w + L"-wal").c_str());
                DeleteFileW((w + L"-shm").c_str());
            }
        }
    }

    // 删不掉（另一个实例正开着这个库）就退回"删表"，至少不会拿旧正文冒充新正文
    const bool stillThere =
        GetFileAttributesW(extract::ToWide(dbPath).c_str()) != INVALID_FILE_ATTRIBUTES;

    sqlite3* db = nullptr;
    if (sqlite3_open(dbPath.c_str(), &db) != SQLITE_OK) {
        SetError("无法创建内容索引库");
        if (db) sqlite3_close(db);
        g_busy = false;
        return;
    }
    // WAL：建索引的同时还能搜（读连接不会被写锁挡住）
    Exec(db, "PRAGMA journal_mode=WAL;");
    Exec(db, "PRAGMA synchronous=NORMAL;");
    Exec(db, "PRAGMA cache_size=-32768;");
    Exec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT);");
    if (stillThere && MetaGet(db, "extract_version") != version) {
        Exec(db, "DROP TABLE IF EXISTS docs_fts;");
        Exec(db, "DROP TABLE IF EXISTS docs;");
        MetaPut(db, "extract_version", version);
    }

    Exec(db,
         "CREATE TABLE IF NOT EXISTS docs(id INTEGER PRIMARY KEY,path TEXT NOT NULL UNIQUE,"
         "name TEXT,size INTEGER,mtime INTEGER,ext TEXT,text TEXT,err INTEGER DEFAULT 0,"
         "tlen INTEGER DEFAULT 0);");
    // 外部内容表：索引里放分词后的 token，snippet() 取 docs.text 的原文
    Exec(db, "CREATE VIRTUAL TABLE IF NOT EXISTS docs_fts USING fts5("
             "text, content='docs', content_rowid='id');");

    sqlite3_stmt* qry = nullptr;
    sqlite3_stmt* ups = nullptr;
    sqlite3_stmt* ftsDel = nullptr;
    sqlite3_stmt* ftsIns = nullptr;
    sqlite3_prepare_v2(db, "SELECT size,mtime FROM docs WHERE path=?", -1, &qry, nullptr);
    sqlite3_prepare_v2(db,
        "INSERT INTO docs(path,name,size,mtime,ext,text,err,tlen) VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(path) DO UPDATE SET name=excluded.name,size=excluded.size,"
        "mtime=excluded.mtime,ext=excluded.ext,text=excluded.text,err=excluded.err,"
        "tlen=excluded.tlen",
        -1, &ups, nullptr);
    sqlite3_prepare_v2(db, "DELETE FROM docs_fts WHERE rowid=?", -1, &ftsDel, nullptr);
    sqlite3_prepare_v2(db, "INSERT INTO docs_fts(rowid,text) VALUES(?,?)", -1, &ftsIns, nullptr);
    sqlite3_stmt* getId = nullptr;
    sqlite3_prepare_v2(db, "SELECT id FROM docs WHERE path=?", -1, &getId, nullptr);

    // 第一阶段：只清点，不存列表。
    // 原来把候选全塞进 vector 再卡 60 万条 —— C 盘 300 多万文件会被静默砍掉一大半，
    // 用户搜不到只会以为"程序不行"。现在按流式处理，多的也照抽。
    g_passes = 0;
    {
        int64_t total = 0;
        Walk(extract::ToWide(norm), cancel, [&](const std::string&, uint64_t, int64_t) {
            ++total;
        });
        g_seen = (int)(total > INT32_MAX ? INT32_MAX : total);
    }
    if (cancel.load()) {
        SetNotice("索引已停止，已完成的部分保留");
        Exec(db, "COMMIT;");
    }

    // 第二阶段：逐个比对 + 抽取
    g_passes = 1;
    Exec(db, "BEGIN;");
    int sinceCommit = 0;

    Walk(extract::ToWide(norm), cancel, [&](const std::string& full, uint64_t size, int64_t mtime) {
        g_done.fetch_add(1);
        if (cancel.load()) return;

        // 没变就跳过：失败的也一并跳过，等文件真的变了再重试
        if (qry) {
            sqlite3_reset(qry);
            sqlite3_bind_text(qry, 1, full.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(qry) == SQLITE_ROW) {
                const int64_t oldSize = sqlite3_column_int64(qry, 0);
                const int64_t oldM    = sqlite3_column_int64(qry, 1);
                if (oldSize == (int64_t)size && oldM == mtime) {
                    g_skipped.fetch_add(1);   // 复用上次结果
                    return;
                }
            }
        }

        const std::string ext = extract::LowerExt(full);
        std::string txt = extract::Text(full, ext);
        // OCR 预算用完导致没抽到正文 → 不写库，下次建索引还会重试
        if (txt.empty() && extract::OcrBudgetLeft() <= 0) return;
        const int err = txt.empty() ? 1 : 0;
        // token 数当“文档长度”：排序时短文档命中更可信
        const std::string tokens = err ? std::string() : FtsTokens(txt);
        int64_t tlen = 0;
        for (char c : tokens) if (c == ' ') ++tlen;
        // 倒排吃全量（召回），原文只留前 16 KB（片段来源）——库体积的大头在原文
        const std::string stored = err ? std::string() : TruncateUtf8(txt, extract::kSnippetCap);

        const size_t slash = full.find_last_of('\\');
        const std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);

        if (ups) {
            sqlite3_reset(ups);
            sqlite3_bind_text(ups, 1, full.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ups, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ups, 3, (sqlite3_int64)size);
            sqlite3_bind_int64(ups, 4, (sqlite3_int64)mtime);
            sqlite3_bind_text(ups, 5, ext.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ups, 6, stored.c_str(), (int)stored.size(), SQLITE_TRANSIENT);
            sqlite3_bind_int(ups, 7, err);
            sqlite3_bind_int64(ups, 8, (sqlite3_int64)tlen);
            sqlite3_step(ups);
        }

        // 同步倒排索引
        if (getId && ftsDel && ftsIns) {
            sqlite3_reset(getId);
            sqlite3_bind_text(getId, 1, full.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(getId) == SQLITE_ROW) {
                const sqlite3_int64 id = sqlite3_column_int64(getId, 0);
                sqlite3_reset(ftsDel);
                sqlite3_bind_int64(ftsDel, 1, id);
                sqlite3_step(ftsDel);
                if (err == 0) {
                    sqlite3_reset(ftsIns);
                    sqlite3_bind_int64(ftsIns, 1, id);
                    sqlite3_bind_text(ftsIns, 2, tokens.c_str(), (int)tokens.size(),
                                      SQLITE_TRANSIENT);
                    sqlite3_step(ftsIns);
                }
            }
        }
        g_parsed.fetch_add(1);

        // 分批提交，每 500 个放一次锁，UI 才不至于一直搜不到东西
        if (++sinceCommit >= 500) {
            Exec(db, "COMMIT;");
            Exec(db, "BEGIN;");
            sinceCommit = 0;
        }
    });

    Exec(db, "COMMIT;");

    MetaPut(db, "root", norm);
    MetaPut(db, "built_at", std::to_string((long long)time(nullptr)));
    MetaPut(db, "partial", cancel.load() ? "1" : "0");
    MetaPut(db, "version", "3");
    MetaPut(db, "extract_version", version);

    if (qry) sqlite3_finalize(qry);
    if (ups) sqlite3_finalize(ups);
    if (ftsDel) sqlite3_finalize(ftsDel);
    if (ftsIns) sqlite3_finalize(ftsIns);
    if (getId) sqlite3_finalize(getId);

    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM docs WHERE err=0", -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) g_files = (int)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        MetaPut(db, "docs", std::to_string((long long)g_files.load()));
    }
    Exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    sqlite3_close(db);

    if (cancel.load()) SetNotice("索引已停止，已完成的部分保留，下次点「增量更新」接着做");
    g_busy = false;
}

// ============================ 查询语法 ============================
//
//  词A 词B              同时包含（默认 AND）
//  "完整短语"           短语匹配
//  词A OR 词B           任一包含
//  -词C                 排除
//  ext:pdf              只看某类扩展名（可写 ext:pdf,docx）
//  name:报告            文件名里包含
//  size:>10m            大于 10 MB（支持 k/m/g，也支持 < 和 =）
//  after:2024-06        修改时间在 2024-06 之后
//
// 只有过滤器没有关键词时（如 ext:pdf size:>10m），按大小列出文件。

struct ParsedQuery {
    std::vector<std::string> terms;      // AND
    std::vector<std::string> orTerms;    // OR
    std::vector<std::string> notTerms;   // NOT
    std::vector<std::string> exts;
    std::string nameLike;
    bool    hasSize = false;
    char    sizeOp = '>';
    int64_t sizeVal = 0;
    bool    hasAfter = false;
    int64_t after = 0;

    bool HasFilters() const {
        return !exts.empty() || !nameLike.empty() || hasSize || hasAfter;
    }
    bool Empty() const {
        return terms.empty() && orTerms.empty() && notTerms.empty() && !HasFilters();
    }
};

std::string LowerStr(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool StartsWithStr(const std::string& s, const char* p) {
    const size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

int64_t ParseSizeStr(const std::string& v) {
    if (v.empty()) return 0;
    char* end = nullptr;
    double d = strtod(v.c_str(), &end);
    if (end == v.c_str()) return 0;
    if (*end) {
        const char u = (char)tolower((unsigned char)*end);
        if (u == 'k') d *= 1024.0;
        else if (u == 'm') d *= 1024.0 * 1024.0;
        else if (u == 'g') d *= 1024.0 * 1024.0 * 1024.0;
    }
    return (int64_t)d;
}

// YYYY / YYYY-MM / YYYY-MM-DD → Unix 秒
int64_t ParseDateStr(const std::string& v) {
    int y = 0, mo = 1, d = 1;
    if (sscanf(v.c_str(), "%d-%d-%d", &y, &mo, &d) < 1) return 0;
    struct tm tmv;
    memset(&tmv, 0, sizeof tmv);
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    return (int64_t)mktime(&tmv);
}

// 把查询切成词，保留引号内的空格
std::vector<std::string> SplitQuery(const std::string& q) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    for (char c : q) {
        if (c == '"') { inQuote = !inQuote; cur.push_back(c); continue; }
        if (!inQuote && (c == ' ' || c == '\t')) {
            if (!cur.empty()) { out.push_back(cur); cur.clear(); }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

ParsedQuery ParseQuery(const std::string& q) {
    ParsedQuery p;
    bool nextOr = false;
    for (std::string tok : SplitQuery(q)) {
        if (tok.empty()) continue;
        if (tok == "OR" || tok == "or" || tok == "|") {
            // "A OR B"：A 也要从 AND 组挪到 OR 组，否则会变成 A AND B
            if (!p.terms.empty()) {
                p.orTerms.push_back(p.terms.back());
                p.terms.pop_back();
            }
            nextOr = true;
            continue;
        }

        if (StartsWithStr(tok, "ext:") || StartsWithStr(tok, "EXT:")) {
            const std::string v = tok.substr(4);
            size_t start = 0;
            while (start <= v.size()) {
                size_t comma = v.find(',', start);
                if (comma == std::string::npos) comma = v.size();
                std::string one = LowerStr(v.substr(start, comma - start));
                while (!one.empty() && one[0] == '.') one.erase(one.begin());
                if (!one.empty()) p.exts.push_back("." + one);
                start = comma + 1;
            }
            continue;
        }
        if (StartsWithStr(tok, "name:") || StartsWithStr(tok, "NAME:")) {
            p.nameLike = tok.substr(5);
            continue;
        }
        if (StartsWithStr(tok, "size:") || StartsWithStr(tok, "SIZE:")) {
            std::string v = tok.substr(5);
            if (!v.empty() && (v[0] == '>' || v[0] == '<' || v[0] == '=')) {
                p.sizeOp = v[0];
                v = v.substr(1);
            }
            p.sizeVal = ParseSizeStr(v);
            p.hasSize = p.sizeVal > 0;
            continue;
        }
        if (StartsWithStr(tok, "after:") || StartsWithStr(tok, "AFTER:")) {
            p.after = ParseDateStr(tok.substr(6));
            p.hasAfter = p.after > 0;
            continue;
        }

        bool neg = false;
        if (tok.size() > 1 && tok[0] == '-') { neg = true; tok = tok.substr(1); }
        if (tok.size() >= 2 && tok.front() == '"' && tok.back() == '"')
            tok = tok.substr(1, tok.size() - 2);
        if (tok.empty()) continue;

        if (neg) p.notTerms.push_back(tok);
        else if (nextOr) { p.orTerms.push_back(tok); nextOr = false; }
        else p.terms.push_back(tok);
    }
    return p;
}

// 单个词 → FTS5 片段：CJK 出 bigram 短语，拉丁出整词
std::string TermToFts(const std::string& q) {
    std::string out;
    std::vector<unsigned> run;

    auto flush = [&] {
        if (run.size() >= 2) {
            std::string ph;
            for (size_t k = 0; k + 1 < run.size(); ++k) {
                if (!ph.empty()) ph.push_back(' ');
                PutCp(ph, run[k]);
                PutCp(ph, run[k + 1]);
            }
            if (!out.empty()) out += " AND ";
            out += "\"";
            out += ph;
            out += "\"";
        } else if (run.size() == 1) {
            if (!out.empty()) out += " AND ";
            PutCp(out, run[0]);
        }
        run.clear();
    };

    for (size_t i = 0; i < q.size();) {
        const unsigned char c = (unsigned char)q[i];
        if (c < 0x80) {
            flush();
            if (isalnum(c)) {
                std::string w;
                while (i < q.size() && (unsigned char)q[i] < 0x80 &&
                       (isalnum((unsigned char)q[i]) || q[i] == '_')) {
                    w.push_back((char)tolower((unsigned char)q[i]));
                    ++i;
                }
                if (!out.empty()) out += " AND ";
                out += w;
                continue;
            }
            ++i;
            continue;
        }
        unsigned cp = 0;
        const size_t ni = NextCp(q, i, cp);
        if (IsCjk(cp)) run.push_back(cp);
        else flush();
        i = ni;
    }
    flush();
    return out;
}

// 正文截到 UTF-8 字符边界：切半个汉字会让 text 列变成非法 UTF-8，
// 后面 LIKE/instr 都按字节走，片段里就会出现乱码。
std::string TruncateUtf8(const std::string& s, size_t cap) {
    if (s.size() <= cap) return s;
    size_t n = cap;
    while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string BuildMatch(const ParsedQuery& p) {
    std::string andPart, orPart, notPart;
    for (const auto& t : p.terms) {
        const std::string f = TermToFts(t);
        if (f.empty()) continue;
        if (!andPart.empty()) andPart += " AND ";
        andPart += "(" + f + ")";
    }
    for (const auto& t : p.orTerms) {
        const std::string f = TermToFts(t);
        if (f.empty()) continue;
        if (!orPart.empty()) orPart += " OR ";
        orPart += "(" + f + ")";
    }
    for (const auto& t : p.notTerms) {
        const std::string f = TermToFts(t);
        if (f.empty()) continue;
        if (!notPart.empty()) notPart += " OR ";
        notPart += "(" + f + ")";
    }

    std::string m;
    if (!andPart.empty()) m = "(" + andPart + ")";
    if (!orPart.empty()) m += (m.empty() ? "" : " AND ") + std::string("(") + orPart + ")";
    if (!m.empty() && !notPart.empty()) m += " NOT (" + notPart + ")";
    return m;
}

// 给 SQLite 注册 regexp(pattern, text)，这样能用 WHERE text REGEXP ?
void RegexpFunc(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
    if (argc != 2) { sqlite3_result_int(ctx, 0); return; }
    const char* pat = (const char*)sqlite3_value_text(argv[0]);
    const char* txt = (const char*)sqlite3_value_text(argv[1]);
    if (!pat || !txt) { sqlite3_result_int(ctx, 0); return; }
    try {
        const std::regex re(pat, std::regex::ECMAScript | std::regex::optimize);
        sqlite3_result_int(ctx, std::regex_search(txt, re) ? 1 : 0);
    } catch (...) {
        sqlite3_result_int(ctx, 0);   // 正则写错就当没命中，不崩
    }
}

// 正则模式：re:xxx 或 /xxx/
// 注意：re: 后面若还跟了 ext:/size:/name:/after:，要把它们从正则里切出去，
// 否则 " ext:pdf" 会被当成正则的一部分，永远匹配不到。
bool IsRegexQuery(const std::string& q, std::string& pattern) {
    if (q.size() > 3 && (q.compare(0, 3, "re:") == 0 || q.compare(0, 3, "RE:") == 0)) {
        pattern = q.substr(3);
        static const char* kKeys[] = {" ext:", " size:", " name:", " after:"};
        for (;;) {
            size_t cut = std::string::npos;
            for (const char* k : kKeys) {
                const size_t pos = pattern.find(k);
                if (pos != std::string::npos && (cut == std::string::npos || pos < cut)) cut = pos;
            }
            if (cut == std::string::npos) break;
            pattern.resize(cut);
        }
        return !pattern.empty();
    }
    if (q.size() > 2 && q.front() == '/' && q.back() == '/') {
        pattern = q.substr(1, q.size() - 2);
        return !pattern.empty();
    }
    return false;
}

std::vector<db::Hit> Search(const std::string& root, const std::string& query, int limit) {
    std::vector<db::Hit> out;
    if (query.empty()) return out;
    const std::string norm = NormRoot(root);
    if (!HasIndex(norm)) return out;

    const ParsedQuery pq = ParseQuery(query);

    // ---- 正则模式：全表扫描（显式写 re: 才走这条路，慢是用户预期内的）
    std::string rePat;
    if (IsRegexQuery(query, rePat)) {
        const std::string dbPath = DbPathFor(norm);
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            if (db) sqlite3_close(db);
            return out;
        }
        sqlite3_create_function(db, "regexp", 2, SQLITE_UTF8, nullptr, RegexpFunc, nullptr, nullptr);
        sqlite3_exec(db, "PRAGMA cache_size=-32768;", nullptr, nullptr, nullptr);

        std::regex re;
        try { re = std::regex(rePat, std::regex::ECMAScript | std::regex::optimize); }
        catch (...) { sqlite3_close(db); return out; }

        // 正则也要支持 ext:/size:/after: 过滤
        std::string reFilter;
        std::vector<int> reIdx;
        int rp = 10;
        if (!pq.exts.empty()) {
            reFilter += " AND ext IN (";
            for (size_t i = 0; i < pq.exts.size(); ++i) {
                if (i) reFilter += ",";
                reFilter += "?" + std::to_string(rp);
                reIdx.push_back(rp++);
            }
            reFilter += ")";
        }
        if (!pq.nameLike.empty()) {
            reFilter += " AND name LIKE ?" + std::to_string(rp) + " ESCAPE '\'";
            reIdx.push_back(rp++);
        }
        if (pq.hasSize) {
            reFilter += std::string(" AND size ") + pq.sizeOp + " ?" + std::to_string(rp);
            reIdx.push_back(rp++);
        }
        if (pq.hasAfter) {
            reFilter += " AND mtime >= ?" + std::to_string(rp);
            reIdx.push_back(rp++);
        }

        sqlite3_stmt* st = nullptr;
        const std::string reSql =
            "SELECT path,name,size,substr(text,1,600) FROM docs WHERE text REGEXP ?1" +
            reFilter + " ORDER BY mtime DESC LIMIT ?2";
        if (sqlite3_prepare_v2(db, reSql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, rePat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 2, limit);
            size_t fi2 = 0;
            for (const auto& e : pq.exts)
                sqlite3_bind_text(st, reIdx[fi2++], e.c_str(), -1, SQLITE_TRANSIENT);
            if (!pq.nameLike.empty()) {
                const std::string pat = "%" + LikeEscape(pq.nameLike) + "%";
                sqlite3_bind_text(st, reIdx[fi2++], pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (pq.hasSize) sqlite3_bind_int64(st, reIdx[fi2++], (sqlite3_int64)pq.sizeVal);
            if (pq.hasAfter) sqlite3_bind_int64(st, reIdx[fi2++], (sqlite3_int64)pq.after);
            while (sqlite3_step(st) == SQLITE_ROW) {
                db::Hit h;
                const unsigned char* p0 = sqlite3_column_text(st, 0);
                const unsigned char* p1 = sqlite3_column_text(st, 1);
                const unsigned char* p3 = sqlite3_column_text(st, 3);
                h.path = p0 ? (const char*)p0 : "";
                h.name = p1 ? (const char*)p1 : "";
                h.size = (uint64_t)sqlite3_column_int64(st, 2);
                const std::string head = p3 ? (const char*)p3 : "";
                // 片段尽量落在命中位置附近
                std::smatch m;
                std::string snip = head;
                if (std::regex_search(head, m, re) && m.position(0) > 40)
                    snip = head.substr((size_t)m.position(0) - 40);
                h.snippet = Flatten(snip, 90);
                h.content = true;
                out.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
        return out;
    }

    if (pq.Empty()) return out;

    const std::string dbPath = DbPathFor(norm);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_exec(db, "PRAGMA cache_size=-16384;", nullptr, nullptr, nullptr);

    // 过滤条件用显式编号（从 10 起）：匿名 ? 会跟后面的 ?3（LIMIT）撞号，
    // 结果 LIMIT 被绑成 ".pdf"，关键词 + 后缀永远 0 行。
    std::vector<int> filterIdx;
    std::string filterSql;
    int nextParam = 10;
    if (!pq.exts.empty()) {
        filterSql += " AND d.ext IN (";
        for (size_t i = 0; i < pq.exts.size(); ++i) {
            if (i) filterSql += ",";
            filterSql += "?" + std::to_string(nextParam);
            filterIdx.push_back(nextParam++);
        }
        filterSql += ")";
    }
    if (!pq.nameLike.empty()) {
        filterSql += " AND d.name LIKE ?" + std::to_string(nextParam) + " ESCAPE '\\'";
        filterIdx.push_back(nextParam++);
    }
    if (pq.hasSize) {
        filterSql += std::string(" AND d.size ") + pq.sizeOp + " ?" + std::to_string(nextParam);
        filterIdx.push_back(nextParam++);
    }
    if (pq.hasAfter) {
        filterSql += " AND d.mtime >= ?" + std::to_string(nextParam);
        filterIdx.push_back(nextParam++);
    }

    const std::string match = BuildMatch(pq);
    if (std::getenv("SPACESIFT_DEBUG_Q")) fprintf(stderr, "[q] match=[%s]\n", match.c_str());
    sqlite3_stmt* st = nullptr;

    if (!match.empty()) {
        std::string sql =
            "WITH cand AS ("
            "  SELECT d.id AS id, d.tlen AS tlen FROM docs_fts CROSS JOIN docs d "
            "    ON d.id = docs_fts.rowid WHERE docs_fts MATCH ?2";
        sql += filterSql;
        sql +=
            "  ORDER BY d.tlen ASC LIMIT 2000"
            ")"
            "SELECT d.path, d.name, d.size, "
            "       substr(d.text, max(1, instr(lower(d.text), lower(?1)) - 40), 220), "
            "       (length(d.text) - length(replace(lower(d.text), lower(?1), ''))) "
            "         / max(1, length(?1)) AS hits "
            "FROM cand JOIN docs d ON d.id = cand.id "
            "ORDER BY hits DESC, d.tlen ASC LIMIT ?3";

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            int idx = 1;
            const std::string rawTerm = !pq.terms.empty() ? pq.terms[0]
                                      : (!pq.orTerms.empty() ? pq.orTerms[0] : query);
            sqlite3_bind_text(st, 1, rawTerm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, match.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, limit);
            size_t fi = 0;
            for (const auto& e : pq.exts)
                sqlite3_bind_text(st, filterIdx[fi++], e.c_str(), -1, SQLITE_TRANSIENT);
            if (!pq.nameLike.empty()) {
                const std::string pat = "%" + LikeEscape(pq.nameLike) + "%";
                sqlite3_bind_text(st, filterIdx[fi++], pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (pq.hasSize) sqlite3_bind_int64(st, filterIdx[fi++], (sqlite3_int64)pq.sizeVal);
            if (pq.hasAfter) sqlite3_bind_int64(st, filterIdx[fi++], (sqlite3_int64)pq.after);
            (void)idx;

            while (sqlite3_step(st) == SQLITE_ROW) {
                db::Hit h;
                const unsigned char* p0 = sqlite3_column_text(st, 0);
                const unsigned char* p1 = sqlite3_column_text(st, 1);
                const unsigned char* p3 = sqlite3_column_text(st, 3);
                h.path = p0 ? (const char*)p0 : "";
                h.name = p1 ? (const char*)p1 : "";
                h.size = (uint64_t)sqlite3_column_int64(st, 2);
                h.snippet = Flatten(p3 ? (const char*)p3 : "", 90);
                h.content = true;
                h.score = (int)sqlite3_column_int64(st, 4);   // 命中次数，跨库合并时比大小
                out.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
        if (!out.empty()) { sqlite3_close(db); return out; }
    }

    // 只有过滤器（如 ext:pdf size:>10m）：直接按大小列
    if (match.empty() && pq.HasFilters()) {
        std::string sql =
            "SELECT d.path, d.name, d.size, '' FROM docs d WHERE 1=1";
        sql += filterSql;
        sql += " ORDER BY d.size DESC LIMIT ?9";

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_int(st, 9, limit);
            size_t fi = 0;
            for (const auto& e : pq.exts)
                sqlite3_bind_text(st, filterIdx[fi++], e.c_str(), -1, SQLITE_TRANSIENT);
            if (!pq.nameLike.empty()) {
                const std::string pat = "%" + LikeEscape(pq.nameLike) + "%";
                sqlite3_bind_text(st, filterIdx[fi++], pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (pq.hasSize) sqlite3_bind_int64(st, filterIdx[fi++], (sqlite3_int64)pq.sizeVal);
            if (pq.hasAfter) sqlite3_bind_int64(st, filterIdx[fi++], (sqlite3_int64)pq.after);

            while (sqlite3_step(st) == SQLITE_ROW) {
                db::Hit h;
                const unsigned char* p0 = sqlite3_column_text(st, 0);
                const unsigned char* p1 = sqlite3_column_text(st, 1);
                const unsigned char* p3 = sqlite3_column_text(st, 3);
                h.path = p0 ? (const char*)p0 : "";
                h.name = p1 ? (const char*)p1 : "";
                h.size = (uint64_t)sqlite3_column_int64(st, 2);
                h.snippet = Flatten(p3 ? (const char*)p3 : "", 90);
                h.content = true;
                out.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
        sqlite3_close(db);
        return out;
    }

    // 兜底：单字查询 / MATCH 不可用 → 全表 LIKE
    {
        const char* sql2 =
            "SELECT path,name,size,"
            "       substr(text, max(1, instr(lower(text), lower(?1)) - 40), 220) "
            "FROM docs WHERE text LIKE ?2 ESCAPE '\\' ORDER BY mtime DESC LIMIT ?3";
        if (sqlite3_prepare_v2(db, sql2, -1, &st, nullptr) == SQLITE_OK) {
            const std::string pat = "%" + LikeEscape(query) + "%";
            sqlite3_bind_text(st, 1, query.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(st, 3, limit);
            while (sqlite3_step(st) == SQLITE_ROW) {
                db::Hit h;
                const unsigned char* p0 = sqlite3_column_text(st, 0);
                const unsigned char* p1 = sqlite3_column_text(st, 1);
                const unsigned char* p3 = sqlite3_column_text(st, 3);
                h.path = p0 ? (const char*)p0 : "";
                h.name = p1 ? (const char*)p1 : "";
                h.size = (uint64_t)sqlite3_column_int64(st, 2);
                h.snippet = Flatten(p3 ? (const char*)p3 : "", 90);
                h.content = true;
                out.push_back(std::move(h));
            }
            sqlite3_finalize(st);
        }
    }
    sqlite3_close(db);
    return out;
}

// 跨所有已建索引搜索。
// 这里不把多份库 ATTACH 起来查（FTS5 虚拟表跨库 join 很容易踩坑），
// 而是每份库各查一次再合并：库的数量是"用户扫过几个目录"级别，几十份顶天了。
std::vector<db::Hit> SearchAll(const std::string& query, int limit) {
    std::vector<db::Hit> merged;
    if (query.empty()) return merged;

    std::unordered_map<std::string, size_t> byPath;   // 小写路径 → merged 下标
    for (const IndexInfo& idx : ListIndexes()) {
        if (idx.root.empty() || idx.root == "(未知)") continue;
        std::vector<db::Hit> part = Search(idx.root, query, limit);
        for (auto& h : part) {
            std::string key = h.path;
            for (auto& c : key) c = (char)tolower((unsigned char)c);
            auto it = byPath.find(key);
            if (it == byPath.end()) {
                byPath[key] = merged.size();
                merged.push_back(std::move(h));
            } else if (h.score > merged[it->second].score) {
                merged[it->second] = std::move(h);   // 同一文件被父子目录各索引过，留更相关的
            }
        }
    }

    std::stable_sort(merged.begin(), merged.end(), [](const db::Hit& a, const db::Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.size > b.size;
    });
    if (limit > 0 && (int)merged.size() > limit) merged.resize((size_t)limit);
    return merged;
}

}  // namespace content
