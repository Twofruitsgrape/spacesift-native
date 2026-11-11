#include "extract.h"

#include <windows.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace extract {
namespace {

bool IsTextExt(const std::string& e) {
    static const char* kExts[] = {
        // 纯文本 / 数据
        ".txt", ".md", ".markdown", ".csv", ".tsv", ".log", ".json", ".xml",
        ".yml", ".yaml", ".ini", ".cfg", ".conf", ".toml", ".properties",
        // 网页 / 样式
        ".html", ".htm", ".css", ".scss", ".less", ".vue", ".svg",
        // 代码
        ".c", ".h", ".cpp", ".cc", ".cxx", ".hpp", ".hh", ".cs", ".java",
        ".py", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".rs", ".go",
        ".sql", ".sh", ".bash", ".ps1", ".bat", ".cmd", ".lua", ".rb", ".php",
        ".pl", ".r", ".m", ".gradle", ".cmake", ".make", ".mk", ".dockerfile",
        // 三维里本来就是 ASCII 的
        ".iges", ".igs", ".stl", ".ply",
        // CAD 附属文本
        ".ctb", ".stb", ".scr", ".lsp",
    };
    for (const char* x : kExts) if (e == x) return true;
    return false;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

bool ValidUtf8(const std::string& s) {
    const unsigned char* p = (const unsigned char*)s.data();
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = p[i];
        if (c < 0x80) { ++i; continue; }
        int len;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        else return false;
        if (i + (size_t)len > n) return false;
        for (int k = 1; k < len; ++k)
            if ((p[i + k] & 0xC0) != 0x80) return false;
        i += (size_t)len;
    }
    return true;
}

// 偶数位大量 0x00 → 多半是 UTF-16LE
bool LooksUtf16Le(const std::string& s) {
    if (s.size() < 8) return false;
    size_t zero = 0;
    for (size_t i = 1; i < s.size(); i += 2) if (s[i] == '\0') ++zero;
    return zero * 4 > s.size();   // > 一半的奇数位是 0
}

std::string GbkToUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(936, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(936, 0, s.data(), (int)s.size(), &w[0], n);
    int m = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (m <= 0) return {};
    std::string out((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &out[0], m, nullptr, nullptr);
    return out;
}

std::string Utf16LeToUtf8(const std::string& s) {
    if (s.size() < 2) return {};
    const wchar_t* p = (const wchar_t*)s.data();
    size_t n = s.size() / sizeof(wchar_t);
    int m = WideCharToMultiByte(CP_UTF8, 0, p, (int)n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return {};
    std::string out((size_t)m, '\0');
    WideCharToMultiByte(CP_UTF8, 0, p, (int)n, &out[0], m, nullptr, nullptr);
    return out;
}

// 原始字节 → UTF-8。中文 Windows 上的老文件大量是 GBK，不探测就会整片乱码。
std::string Decode(const std::string& raw) {
    if (raw.empty()) return {};
    if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF &&
        (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
        return raw.substr(3);
    if (raw.size() >= 2 && (unsigned char)raw[0] == 0xFF && (unsigned char)raw[1] == 0xFE)
        return Utf16LeToUtf8(raw.substr(2));
    if (LooksUtf16Le(raw)) return Utf16LeToUtf8(raw);
    if (ValidUtf8(raw)) return raw;
    std::string g = GbkToUtf8(raw);
    return g.empty() ? raw : g;   // 兜底：至少别丢光
}

// 逐行读取，内存只留一个块，几百 MB 的 STEP 也不会把进程拖爆
class Reader {
  public:
    ~Reader() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    bool Open(const std::string& path, uint64_t cap) {
        std::wstring w = ToWide(path);
        h_ = CreateFileW(w.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        cap_ = cap;
        return true;
    }

    // 读一行（不含 \r\n）；读完返回 false
    bool Line(std::string& out) {
        for (;;) {
            size_t nl = buf_.find('\n', pos_);
            if (nl != std::string::npos) {
                out.assign(buf_, pos_, nl - pos_);
                pos_ = nl + 1;
                if (!out.empty() && out.back() == '\r') out.pop_back();
                return true;
            }
            if (eof_) {
                if (pos_ < buf_.size()) {
                    out.assign(buf_, pos_, buf_.size() - pos_);
                    pos_ = buf_.size();
                    if (!out.empty() && out.back() == '\r') out.pop_back();
                    return true;
                }
                return false;
            }
            if (pos_ > 0) { buf_.erase(0, pos_); pos_ = 0; }
            Fill();
        }
    }

    bool ReadPrefix(size_t maxBytes, std::string& out) {
        out.clear();
        while (out.size() < maxBytes) {
            if (pos_ >= buf_.size()) {
                if (eof_) break;
                buf_.clear();
                pos_ = 0;
                Fill();
                if (buf_.empty()) break;
            }
            size_t take = std::min(maxBytes - out.size(), buf_.size() - pos_);
            out.append(buf_, pos_, take);
            pos_ += take;
        }
        return !out.empty();
    }

  private:
    void Fill() {
        if (eof_ || read_ >= cap_) { eof_ = true; return; }
        char tmp[65536];
        uint64_t left = cap_ - read_;
        DWORD want = (DWORD)std::min<uint64_t>(sizeof tmp, left);
        DWORD got = 0;
        if (!ReadFile(h_, tmp, want, &got, nullptr) || got == 0) { eof_ = true; return; }
        read_ += got;
        buf_.append(tmp, got);
    }

    HANDLE      h_ = INVALID_HANDLE_VALUE;
    std::string buf_;
    size_t      pos_ = 0;
    uint64_t    cap_ = 0;
    uint64_t    read_ = 0;
    bool        eof_ = false;
};

void Append(std::string& out, const std::string& s) {
    if (out.size() >= kStoreCap) return;
    std::string t = Trim(s);
    if (t.empty()) return;
    size_t room = kStoreCap - out.size();
    out.append(t, 0, std::min(room, t.size()));
    out.push_back('\n');
}

// ---- 各格式抽取 ----

// .step / .stp：坐标数字占 95%，真正可搜的是单引号里的名字
//   #42 = PRODUCT('法兰盘 3寸','flange','',(#43));
std::string StepStrings(const std::string& path) {
    Reader r;
    if (!r.Open(path, kSkipAbove)) return {};
    std::string out, line, cur;
    bool inQuote = false;
    while (out.size() < kStoreCap && r.Line(line)) {
        for (size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
            if (!inQuote) {
                if (c == '\'') { inQuote = true; cur.clear(); }
            } else if (c == '\'') {
                // STEP 里 '' 表示一个单引号
                if (i + 1 < line.size() && line[i + 1] == '\'') { cur.push_back('\''); ++i; }
                else { inQuote = false; Append(out, cur); }
            } else {
                cur.push_back(c);
            }
        }
        if (inQuote) cur.push_back(' ');   // 跨行的字符串保留成一段
    }
    return out;
}

// .dxf：按 group code 走状态机。坐标是 code 10/20/30，不用管；
// 文字在 code 1/3，块名/图层名/样式名在 code 2。
std::string DxfStrings(const std::string& path) {
    Reader r;
    if (!r.Open(path, kSkipAbove)) return {};
    std::string line;
    if (!r.Line(line)) return {};
    if (line.compare(0, 18, "AutoCAD Binary DXF") == 0) return {};   // 二进制版交给上层兜底

    // 注意：第一行本身就是第一个 group code，不能当“探测行”吃掉，
    // 否则 code/value 全部错位，什么都匹配不上。
    int  code = -1;
    bool haveCode = false;
    bool textCtx = false, nameCtx = false;
    bool more = true;
    std::string out;
    while (more && out.size() < kStoreCap) {
        if (!haveCode) {
            code = atoi(line.c_str());
            haveCode = true;
        } else {
            if (code == 0) {
                std::string ent = Trim(line);
                textCtx = (ent == "TEXT" || ent == "MTEXT" || ent == "ATTRIB" ||
                           ent == "ATTDEF" || ent == "DIMENSION" || ent == "LEADER");
                nameCtx = (ent == "LAYER" || ent == "BLOCK" || ent == "INSERT" ||
                           ent == "STYLE" || ent == "DIMSTYLE");
            } else if (textCtx && (code == 1 || code == 3)) {
                Append(out, line);
            } else if (nameCtx && code == 2) {
                Append(out, line);
                nameCtx = false;
            }
            haveCode = false;
        }
        more = r.Line(line);
    }
    return out;
}

// .obj：只留命名行，顶点坐标没意义
std::string ObjStrings(const std::string& path) {
    Reader r;
    if (!r.Open(path, kSkipAbove)) return {};
    std::string out, line;
    while (out.size() < kStoreCap && r.Line(line)) {
        if (line.size() > 2) {
            if (line.compare(0, 2, "g ") == 0 || line.compare(0, 2, "o ") == 0 ||
                line.compare(0, 8, "usemtl ") == 0 || line.compare(0, 7, "mtllib ") == 0 ||
                line[0] == '#')
                Append(out, line);
        }
    }
    return out;
}

// 兜底：从二进制里捞连续可打印串（≥6 字节）
std::string Strings(const std::string& path) {
    Reader r;
    if (!r.Open(path, kSkipAbove)) return {};
    std::string out, line, run;
    auto flush = [&] {
        if (run.size() >= 6) Append(out, run);
        run.clear();
    };
    while (out.size() < kStoreCap && r.Line(line)) {
        for (unsigned char c : line) {
            if ((c >= 0x20 && c < 0x7F) || c >= 0x80) run.push_back((char)c);
            else flush();
        }
        flush();
    }
    return out;
}

std::string PlainText(const std::string& path) {
    Reader r;
    if (!r.Open(path, kReadCap)) return {};
    std::string raw;
    if (!r.ReadPrefix((size_t)kReadCap, raw)) return {};
    std::string u8 = Decode(raw);
    if (u8.size() > kStoreCap) u8.resize(kStoreCap);
    return u8;
}

}  // namespace

std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w((size_t)n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, &w[0], n);
    return w;
}

std::string FromWide(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, nullptr, nullptr);
    return s;
}

std::string LowerExt(const std::string& path) {
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return {};
    size_t sep = path.find_last_of("\\/");
    if (sep != std::string::npos && dot < sep) return {};
    std::string e = path.substr(dot);
    if (e.size() > 12) return {};
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    return e;
}

bool Supports(const std::string& ext) {
    if (IsTextExt(ext)) return true;
    return ext == ".dxf" || ext == ".step" || ext == ".stp" || ext == ".obj";
}

std::string Text(const std::string& path, const std::string& ext) {
    if (ext == ".dxf") {
        std::string t = DxfStrings(path);
        if (!t.empty()) return t;
        return Strings(path);          // 二进制 DXF / 加密图纸兜底
    }
    if (ext == ".step" || ext == ".stp") {
        std::string t = StepStrings(path);
        if (!t.empty()) return t;
        return Strings(path);
    }
    if (ext == ".obj") return ObjStrings(path);
    return PlainText(path);
}

}  // namespace extract
