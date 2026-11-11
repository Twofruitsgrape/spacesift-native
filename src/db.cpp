#include "db.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <unordered_map>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "sqlite3.h"

namespace db {

static sqlite3*   g_db = nullptr;
static sqlite3_stmt* g_insert = nullptr;
static std::string g_tmpPath;
static std::string g_finalPath;
static bool        g_inBulk = false;
static uint64_t    g_rows = 0;

static std::string NormCase(std::string s) {
    for (auto& c : s) c = (char)tolower((unsigned char)c);
    std::replace(s.begin(), s.end(), '/', '\\');
    while (!s.empty() && s.back() == '\\') s.pop_back();
    return s;
}

// 程序内部全程 UTF-8；和 Windows 文件 API 打交道时转成宽字符。
static std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

static std::string NarrowW(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string DataDir() {
    // 必须用 W 版：SHGetFolderPathA 在中文用户名下返回 GBK，
    // 后面既交给 sqlite（要求 UTF-8）又交给 W 版 API，会找不到目录。
    wchar_t buf[MAX_PATH] = {0};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf))) {
        std::wstring wdir = std::wstring(buf) + L"\\SpaceSiftNative";
        CreateDirectoryW(wdir.c_str(), nullptr);
        return NarrowW(wdir);
    }
    return ".";
}

std::string IndexPathFor(const std::string& root) {
    return IndexPathWithPrefix(root, "index_");
}

std::string IndexPathWithPrefix(const std::string& root, const char* prefix) {
    // 先归一化（斜杠、尾部反斜杠、盘符冒号），否则同一个目录
    // 扫 "...\AppData" 和 "...\AppData\" 会建出两份 70 MB 的库。
    std::string norm = root;
    for (auto& c : norm) if (c == '/') c = '\\';
    while (norm.size() > 3 && norm.back() == '\\') norm.pop_back();
    if (norm.size() == 2 && norm[1] == ':') norm += '\\';

    std::string safe;
    for (char c : norm) {
        if (isalnum((unsigned char)c) || c == '_') safe.push_back(c);
        else safe.push_back('_');
    }
    if (safe.size() > 40) safe.resize(40);
    return DataDir() + "\\" + prefix + safe + ".db";
}

static bool Exec(const char* sql) {
    return g_db && sqlite3_exec(g_db, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

static void PrepareInsert() {
    if (g_insert) return;
    sqlite3_prepare_v2(g_db,
        "INSERT INTO files(parent,name,size) VALUES(?,?,?)", -1, &g_insert, nullptr);
}

bool Open(const std::string& dbPath) {
    Close();
    g_finalPath = dbPath;
    g_tmpPath = dbPath + ".building";

    // 上一次中断留下的半成品直接清掉
    DeleteFileW(WidenUtf8(g_tmpPath).c_str());
    DeleteFileW(WidenUtf8(g_tmpPath + "-journal").c_str());
    DeleteFileW(WidenUtf8(g_tmpPath + "-wal").c_str());

    if (sqlite3_open(g_tmpPath.c_str(), &g_db) != SQLITE_OK) {
        g_db = nullptr;
        return false;
    }
    Exec("PRAGMA journal_mode=OFF;");
    Exec("PRAGMA synchronous=OFF;");
    Exec("PRAGMA cache_size=-16384;");      // 16 MB 页缓存，原来 64 MB 全摊在内存里
    Exec("PRAGMA page_size=8192;");
    // 只存 parent id + 名字：336 万行 × 32 字节 ≈ 107 MB。
    // 存完整路径的话每行多 80 字节，库直接涨到 656 MB。
    // id 是 rowid 别名，主键索引由 SQLite 自带，拼路径时用它反查祖先。
    Exec("CREATE TABLE IF NOT EXISTS files(id INTEGER PRIMARY KEY,parent INTEGER,"
         "name TEXT,size INTEGER);");
    Exec("CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY,value TEXT);");
    PrepareInsert();
    g_rows = 0;
    return true;
}

void Close() {
    if (g_insert) { sqlite3_finalize(g_insert); g_insert = nullptr; }
    if (g_db) { sqlite3_close(g_db); g_db = nullptr; }
    g_inBulk = false;
}

void BeginBulk() {
    if (!g_db || g_inBulk) return;
    Exec("BEGIN;");
    g_inBulk = true;
}

void FlushBulk() {
    if (!g_db || !g_inBulk) return;
    Exec("COMMIT;");
    g_inBulk = false;
}

long long Add(long long parent, const std::string& name, uint64_t size, bool isDir) {
    if (!g_db || !g_insert) return 0;
    sqlite3_reset(g_insert);
    sqlite3_bind_int64(g_insert, 1, parent);
    // 名字统一存小写，搜索时不用再做 COLLATE NOCASE
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back((char)tolower((unsigned char)c));
    sqlite3_bind_text(g_insert, 2, lower.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(g_insert, 3, (sqlite3_int64)size);
    sqlite3_step(g_insert);
    ++g_rows;
    (void)isDir;
    return (long long)sqlite3_last_insert_rowid(g_db);
}

bool EndBulk(const std::string& root, bool partial) {
    FlushBulk();
    if (!g_db) return false;
    PrepareInsert();

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(g_db, "INSERT OR REPLACE INTO meta(key,value) VALUES(?,?)",
                           -1, &st, nullptr) == SQLITE_OK) {
        auto put = [&](const char* key, const std::string& value) {
            sqlite3_reset(st);
            sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
            sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(st);
        };
        put("root", NormCase(root));
        put("built_at", std::to_string((long long)time(nullptr)));
        put("files", std::to_string(g_rows));
        put("partial", partial ? "1" : "0");
        sqlite3_finalize(st);
    }
    Close();

    // 索引库可能正被另一个窗口读着，换名会失败
    DeleteFileW(WidenUtf8(g_finalPath).c_str());
    if (!MoveFileExW(WidenUtf8(g_tmpPath).c_str(), WidenUtf8(g_finalPath).c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }
    return true;
}

void AbortBulk() {
    if (g_db) {
        sqlite3_exec(g_db, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
    Close();
    DeleteFileW(WidenUtf8(g_tmpPath).c_str());
}

// 清理索引目录：删掉超过 keepDays 没更新的库，以及中断残留的 .building。
// 只动 %LOCALAPPDATA%\SpaceSiftNative 下本程序自己的文件。
void PruneOld(int keepDays) {
    const std::string dir = DataDir();
    if (dir == "." || keepDays < 1) return;

    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    const ULONGLONG kSecond = 10000000ULL;
    const ULONGLONG buildingLimit = 24ULL * 3600ULL * kSecond;
    const ULONGLONG dbLimit = (ULONGLONG)keepDays * 24ULL * 3600ULL * kSecond;

    std::wstring pattern = WidenUtf8(dir) + L"\\*";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> kill;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = fd.cFileName;
        bool building = name.size() > 9 &&
                        name.compare(name.size() - 9, 9, L".building") == 0;
        bool isDb = name.size() > 3 &&
                    name.compare(name.size() - 3, 3, L".db") == 0;
        if (!building && !isDb) continue;

        ULARGE_INTEGER t;
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        ULONGLONG age = now.QuadPart > t.QuadPart ? now.QuadPart - t.QuadPart : 0;
        if ((building && age > buildingLimit) || (isDb && age > dbLimit))
            kill.push_back(WidenUtf8(dir) + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    for (auto& p : kill) DeleteFileW(p.c_str());
}

uint64_t Count() {
    if (!g_db) {
        // 没打开时用只读连接数一下
        if (g_finalPath.empty()) return 0;
        sqlite3* tmp = nullptr;
        if (sqlite3_open_v2(g_finalPath.c_str(), &tmp, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
            return 0;
        uint64_t n = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(tmp, "SELECT COUNT(*) FROM files", -1, &st, nullptr) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) n = (uint64_t)sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        sqlite3_close(tmp);
        return n;
    }
    return g_rows;
}

static std::string MetaValue(const std::string& dbPath, const char* key) {
    sqlite3* tmp = nullptr;
    if (sqlite3_open_v2(dbPath.c_str(), &tmp, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return {};
    std::string out;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(tmp, "SELECT value FROM meta WHERE key=?", -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, key, -1, SQLITE_STATIC);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* txt = sqlite3_column_text(st, 0);
            if (txt) out = (const char*)txt;
        }
        sqlite3_finalize(st);
    }
    sqlite3_close(tmp);
    return out;
}

bool IsFresh(const std::string& root, int maxAgeHours) {
    std::string path = IndexPathFor(root);
    std::string gotRoot = MetaValue(path, "root");
    if (gotRoot.empty()) return false;
    if (gotRoot != NormCase(root)) return false;
    if (MetaValue(path, "partial") == "1") return false;
    std::string built = MetaValue(path, "built_at");
    if (built.empty()) return false;
    long long when = atoll(built.c_str());
    long long age = (long long)time(nullptr) - when;
    return age >= 0 && age < (long long)maxAgeHours * 3600;
}

std::vector<Hit> Search(const std::string& query, int limit) {
    std::vector<Hit> out;
    std::string lower;
    for (char c : query) lower.push_back((char)tolower((unsigned char)c));
    if (lower.empty() || g_finalPath.empty()) return out;

    sqlite3* tmp = nullptr;
    if (sqlite3_open_v2(g_finalPath.c_str(), &tmp, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return out;
    sqlite3_exec(tmp, "PRAGMA cache_size=-16384;", nullptr, nullptr, nullptr);

    // 祖先缓存：400 条结果的父链大量重叠，不缓存会重复查几千次
    std::unordered_map<long long, std::pair<long long, std::string>> cache;

    sqlite3_stmt* lookup = nullptr;
    sqlite3_prepare_v2(tmp, "SELECT parent,name FROM files WHERE id=?", -1, &lookup, nullptr);

    auto nameOf = [&](long long id) -> std::string {
        auto it = cache.find(id);
        if (it != cache.end()) return it->second.second;
        std::string name;
        long long parent = -1;
        if (lookup) {
            sqlite3_reset(lookup);
            sqlite3_bind_int64(lookup, 1, id);
            if (sqlite3_step(lookup) == SQLITE_ROW) {
                parent = sqlite3_column_int64(lookup, 0);
                const unsigned char* txt = sqlite3_column_text(lookup, 1);
                if (txt) name = (const char*)txt;
            }
        }
        cache[id] = {parent, name};
        return name;
    };

    sqlite3_stmt* st = nullptr;
    // 全表扫：336 万行 LIKE 约 0.4~0.8 秒，换来库只占一百多 MB
    if (sqlite3_prepare_v2(tmp,
            "SELECT id,name,size FROM files WHERE name LIKE ? ORDER BY size DESC LIMIT ?",
            -1, &st, nullptr) == SQLITE_OK) {
        std::string pattern = "%" + lower + "%";
        sqlite3_bind_text(st, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(st, 2, limit);
        while (sqlite3_step(st) == SQLITE_ROW) {
            Hit hit;
            long long id = sqlite3_column_int64(st, 0);
            const unsigned char* txt = sqlite3_column_text(st, 1);
            hit.name = txt ? (const char*)txt : "";
            hit.size = (uint64_t)sqlite3_column_int64(st, 2);

            // 顺着 parent 往上拼出完整路径
            std::vector<std::string> parts;
            parts.push_back(hit.name);
            long long cur = cache.count(id) ? cache[id].first : -1;
            if (!cache.count(id)) {
                if (lookup) {
                    sqlite3_reset(lookup);
                    sqlite3_bind_int64(lookup, 1, id);
                    if (sqlite3_step(lookup) == SQLITE_ROW)
                        cur = sqlite3_column_int64(lookup, 0);
                }
            }
            // cur >= 1 而不是 > 1：id 1 是根节点（C:\），漏掉它拼出来的路径
            // 就是 "program files\..." 这种没有盘符的样子
            int guard = 0;
            while (cur >= 1 && guard++ < 64) {
                parts.push_back(nameOf(cur));
                auto it = cache.find(cur);
                cur = (it != cache.end()) ? it->second.first : -1;
            }
            std::string path;
            for (auto p = parts.rbegin(); p != parts.rend(); ++p) {
                // 根节点名字本身以反斜杠结尾（"c:\"），无条件再补一个会拼出
                // "c:\\program files"，explorer 不认这种形式，跳转会掉到「此电脑」。
                if (!path.empty() && path.back() != '\\') path.push_back('\\');
                path += *p;
            }
            hit.path = path;
            out.push_back(std::move(hit));
        }
        sqlite3_finalize(st);
    }
    if (lookup) sqlite3_finalize(lookup);
    sqlite3_close(tmp);
    return out;
}

// 打开已有索引做只读查询（搜索前调用）
bool OpenExisting(const std::string& dbPath) {
    Close();
    g_finalPath = dbPath;
    return sqlite3_open_v2(dbPath.c_str(), &g_db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK;
}

}  // namespace db
