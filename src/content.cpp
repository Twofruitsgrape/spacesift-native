#include "content.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <functional>
#include <mutex>
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

// 抽取器逻辑一改，旧正文就不能信了；这个值一改就整库重建。
// OCR 开关也算在内：关着时被记为"抽空"的扫描件，开了之后必须重来一遍。
std::string ExtractVersion() {
    return std::string("6") + (extract::OcrReady() ? "-ocr" : "");
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

void ResetProgress() {
    extract::ResetOcrBudget();
    g_seen = 0;
    g_done = 0;
    g_parsed = 0;
    g_skipped = 0;
    { std::lock_guard<std::mutex> lk(g_mutex); g_curPath.clear(); g_lastError.clear(); }
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
    Exec(db, "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT);");

    // 抽取器或索引结构升级 → 旧正文全部作废重建
    const std::string version = ExtractVersion();
    if (MetaGet(db, "extract_version") != version) {
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

    // 第一阶段：先把候选收齐，这样界面上才有真实的总量和百分比
    struct Cand { std::string path; uint64_t size; int64_t mtime; };
    std::vector<Cand> cands;
    cands.reserve(4096);
    Walk(extract::ToWide(root), cancel, [&](const std::string& full, uint64_t size, int64_t mtime) {
        if (cands.size() < 600000) cands.push_back({full, size, mtime});
    });
    g_seen = (int)cands.size();

    // 第二阶段：逐个比对 + 抽取
    Exec(db, "BEGIN;");
    int sinceCommit = 0;

    for (const Cand& c : cands) {
        if (cancel.load()) break;
        const std::string& full = c.path;
        const uint64_t size = c.size;
        const int64_t mtime = c.mtime;
        g_done.fetch_add(1);
        {
             // 没变就跳过：失败的也一并跳过，等文件真的变了再重试
             if (qry) {
                 sqlite3_reset(qry);
                 sqlite3_bind_text(qry, 1, full.c_str(), -1, SQLITE_TRANSIENT);
                 if (sqlite3_step(qry) == SQLITE_ROW) {
                     const int64_t oldSize = sqlite3_column_int64(qry, 0);
                     const int64_t oldM    = sqlite3_column_int64(qry, 1);
                     if (oldSize == (int64_t)size && oldM == mtime) {
                         g_skipped.fetch_add(1);   // 复用上次结果
                         continue;
                     }
                 }
             }
             if (cancel.load()) continue;

             const std::string ext = extract::LowerExt(full);
             std::string txt = extract::Text(full, ext);
             // OCR 预算用完导致没抽到正文 → 不写库，下次建索引还会重试
             if (txt.empty() && extract::OcrBudgetLeft() <= 0) {
                 g_done.fetch_add(1);
                 continue;
             }
             const int err = txt.empty() ? 1 : 0;
             // token 数当“文档长度”：排序时短文档命中更可信
             const std::string tokens = err ? std::string() : FtsTokens(txt);
             int64_t tlen = 0;
             for (char c : tokens) if (c == ' ') ++tlen;

             const size_t slash = full.find_last_of('\\');
             const std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);

             if (ups) {
                 sqlite3_reset(ups);
                 sqlite3_bind_text(ups, 1, full.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(ups, 2, name.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_int64(ups, 3, (sqlite3_int64)size);
                 sqlite3_bind_int64(ups, 4, (sqlite3_int64)mtime);
                 sqlite3_bind_text(ups, 5, ext.c_str(), -1, SQLITE_TRANSIENT);
                 sqlite3_bind_text(ups, 6, txt.c_str(), (int)txt.size(), SQLITE_TRANSIENT);
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
        }
    }

    Exec(db, "COMMIT;");

    MetaPut(db, "root", root);
    MetaPut(db, "built_at", std::to_string((long long)time(nullptr)));
    MetaPut(db, "partial", cancel.load() ? "1" : "0");
    MetaPut(db, "version", "2");
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
    }
    Exec(db, "PRAGMA wal_checkpoint(TRUNCATE);");
    sqlite3_close(db);
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

std::vector<db::Hit> Search(const std::string& root, const std::string& query, int limit) {
    std::vector<db::Hit> out;
    if (query.empty() || !HasIndex(root)) return out;

    const ParsedQuery pq = ParseQuery(query);
    if (pq.Empty()) return out;

    const std::string dbPath = DbPathFor(root);
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return out;
    }
    sqlite3_exec(db, "PRAGMA cache_size=-16384;", nullptr, nullptr, nullptr);

    std::string filterSql;
    if (!pq.exts.empty()) {
        filterSql += " AND d.ext IN (";
        for (size_t i = 0; i < pq.exts.size(); ++i) filterSql += (i ? ",?" : "?");
        filterSql += ")";
    }
    if (!pq.nameLike.empty()) filterSql += " AND d.name LIKE ? ESCAPE '\\'";
    if (pq.hasSize) filterSql += std::string(" AND d.size ") + pq.sizeOp + " ?";
    if (pq.hasAfter) filterSql += " AND d.mtime >= ?";

    const std::string match = BuildMatch(pq);
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
            sqlite3_bind_text(st, idx++, rawTerm.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, idx++, match.c_str(), -1, SQLITE_TRANSIENT);
            for (const auto& e : pq.exts)
                sqlite3_bind_text(st, idx++, e.c_str(), -1, SQLITE_TRANSIENT);
            if (!pq.nameLike.empty()) {
                const std::string pat = "%" + LikeEscape(pq.nameLike) + "%";
                sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (pq.hasSize) sqlite3_bind_int64(st, idx++, (sqlite3_int64)pq.sizeVal);
            if (pq.hasAfter) sqlite3_bind_int64(st, idx++, (sqlite3_int64)pq.after);
            sqlite3_bind_int(st, idx++, limit);

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
        if (!out.empty()) { sqlite3_close(db); return out; }
    }

    // 只有过滤器（如 ext:pdf size:>10m）：直接按大小列
    if (match.empty() && pq.HasFilters()) {
        std::string sql =
            "SELECT d.path, d.name, d.size, '' FROM docs d WHERE 1=1";
        sql += filterSql;
        sql += " ORDER BY d.size DESC LIMIT ?";

        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK) {
            int idx = 1;
            for (const auto& e : pq.exts)
                sqlite3_bind_text(st, idx++, e.c_str(), -1, SQLITE_TRANSIENT);
            if (!pq.nameLike.empty()) {
                const std::string pat = "%" + LikeEscape(pq.nameLike) + "%";
                sqlite3_bind_text(st, idx++, pat.c_str(), -1, SQLITE_TRANSIENT);
            }
            if (pq.hasSize) sqlite3_bind_int64(st, idx++, (sqlite3_int64)pq.sizeVal);
            if (pq.hasAfter) sqlite3_bind_int64(st, idx++, (sqlite3_int64)pq.after);
            sqlite3_bind_int(st, idx++, limit);

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

}  // namespace content
