#include "content.h"

#include <windows.h>

#include <algorithm>
#include <functional>
#include <mutex>

#include "extract.h"
#include "sqlite3.h"

namespace content {

std::atomic<int>  g_seen{0};
std::atomic<int>  g_parsed{0};
std::atomic<bool> g_busy{false};
std::atomic<int>  g_files{0};

namespace {

std::mutex  g_mutex;          // 保护下面两个字符串
std::string g_curPath;
std::string g_lastError;

void SetCurrent(const std::string& p) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_curPath = p;
}

void SetError(const std::string& e) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_lastError = e;
}

bool Exec(sqlite3* db, const char* sql) {
    return db && sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

// 抽取器逻辑一改，旧正文就不能信了；这个值一改就整库重建
const char* kExtractVersion = "2";

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

// LIKE 里的 % _ \ 要转义，否则用户搜 "100%" 会变成通配
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
        unsigned char c = (unsigned char)s[i];
        if (c == '\r' || c == '\n' || c == '\t' || c == ' ') {
            space = !o.empty();
            continue;
        }
        if (space) { o.push_back(' '); space = false; ++chars; }
        o.push_back((char)c);
        // UTF-8 续字节不算字符数
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

        uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | (uint64_t)fd.nFileSizeLow;
        if (size == 0 || size > extract::kSkipAbove) continue;

        std::string u8 = extract::FromWide(full);
        if (!extract::Supports(extract::LowerExt(u8))) continue;

        g_seen.fetch_add(1);
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

std::string DbPathFor(const std::string& root) {
    return db::IndexPathWithPrefix(root, "content_");
}

bool HasIndex(const std::string& root) {
    std::wstring w = extract::ToWide(DbPathFor(root));
    DWORD a = GetFileAttributesW(w.c_str());
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

void ResetProgress() {
    g_seen = 0;
    g_parsed = 0;
    { std::lock_guard<std::mutex> lk(g_mutex); g_curPath.clear(); g_lastError.clear(); }
}

void Build(const std::string& root, const std::atomic<bool>& cancel) {
    ResetProgress();
    g_busy = true;

    const std::string dbPath = DbPathFor(root);
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
    Exec(db,
         "CREATE TABLE IF NOT EXISTS docs(path TEXT PRIMARY KEY,name TEXT,size INTEGER,"
         "mtime INTEGER,ext TEXT,text TEXT,err INTEGER DEFAULT 0);");
    Exec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT);");

    // 抽取逻辑升级过（比如修了 STEP 的 \X2\ 转义）→ 旧正文全部作废重建
    if (MetaGet(db, "extract_version") != kExtractVersion) {
        Exec(db, "DELETE FROM docs;");
        MetaPut(db, "extract_version", kExtractVersion);
    }

    sqlite3_stmt* qry = nullptr;
    sqlite3_stmt* ins = nullptr;
    sqlite3_prepare_v2(db, "SELECT size,mtime FROM docs WHERE path=?", -1, &qry, nullptr);
    sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO docs(path,name,size,mtime,ext,text,err) "
        "VALUES(?,?,?,?,?,?,?)", -1, &ins, nullptr);

    Exec(db, "BEGIN;");
    int sinceCommit = 0;

    Walk(extract::ToWide(root), cancel,
         [&](const std::string& full, uint64_t size, int64_t mtime) {
             // 没变就跳过：失败的也一并跳过，等文件真的变了再重试
             if (qry) {
                 sqlite3_reset(qry);
                 sqlite3_bind_text(qry, 1, full.c_str(), -1, SQLITE_TRANSIENT);
                 bool found = sqlite3_step(qry) == SQLITE_ROW;
                 if (found) {
                     int64_t oldSize = sqlite3_column_int64(qry, 0);
                     int64_t oldM    = sqlite3_column_int64(qry, 1);
                     if (oldSize == (int64_t)size && oldM == mtime) return;
                 }
             }
             if (cancel.load()) return;

             const std::string ext = extract::LowerExt(full);
             std::string txt = extract::Text(full, ext);
             const int err = txt.empty() ? 1 : 0;

             size_t slash = full.find_last_of('\\');
             std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);

             if (ins) {
                 sqlite3_reset(ins);
                 sqlite3_bind_text(ins, 1, full.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(ins, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_int64(ins, 3, (sqlite3_int64)size);
                 sqlite3_bind_int64(ins, 4, (sqlite3_int64)mtime);
                 sqlite3_bind_text(ins, 5, ext.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(ins, 6, txt.c_str(), (int)txt.size(), SQLITE_TRANSIENT);
                 sqlite3_bind_int(ins, 7, err);
                 sqlite3_step(ins);
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

    // 写 meta：记录完成时间和文档数，方便以后判新旧
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                               -1, &st, nullptr) == SQLITE_OK) {
            auto put = [&](const char* k, const std::string& v) {
                sqlite3_reset(st);
                sqlite3_bind_text(st, 1, k, -1, SQLITE_STATIC);
                sqlite3_bind_text(st, 2, v.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
            };
            put("root", root);
            put("built_at", std::to_string((long long)time(nullptr)));
            put("partial", cancel.load() ? "1" : "0");
            put("version", "1");
            put("extract_version", kExtractVersion);
            sqlite3_finalize(st);
        }
    }
    if (qry) sqlite3_finalize(qry);
    if (ins) sqlite3_finalize(ins);

    // 顺手数一下总量
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM docs WHERE err=0", -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) g_files = (int)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    Exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    sqlite3_close(db);
    g_busy = false;
}

std::vector<db::Hit> Search(const std::string& root, const std::string& query, int limit) {
    std::vector<db::Hit> out;
    if (query.empty() || !HasIndex(root)) return out;

    const std::string dbPath = DbPathFor(root);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_exec(db, "PRAGMA cache_size=-16384;", nullptr, nullptr, nullptr);

    // instr 找不到返回 0，max(1, 0-40) = 1，于是退回从正文开头截一段，不会空着
    const char* sql =
        "SELECT path,name,size,"
        "       substr(text, max(1, instr(lower(text), lower(?1)) - 40), 220) "
        "FROM docs WHERE text LIKE ?2 ESCAPE '\\' ORDER BY mtime DESC LIMIT ?3";

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) == SQLITE_OK) {
        std::string pat = "%" + LikeEscape(query) + "%";
        sqlite3_bind_text(st, 1, query.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, pat.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 3, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            db::Hit h;
            const unsigned char* p = sqlite3_column_text(st, 0);
            const unsigned char* n = sqlite3_column_text(st, 1);
            const unsigned char* s = sqlite3_column_text(st, 3);
            h.path = p ? (const char*)p : "";
            h.name = n ? (const char*)n : "";
            h.size = (uint64_t)sqlite3_column_int64(st, 2);
            h.snippet = Flatten(s ? (const char*)s : "", 90);
            h.content = true;
            out.push_back(std::move(h));
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(db);
    return out;
}

}  // namespace content
