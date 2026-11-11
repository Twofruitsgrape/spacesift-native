#include "extract.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "miniz.h"
#include "external.h"
// SolidWorks 零件/装配体/工程图（MIT，纯 C++ 解析，不装 SW 也能读）
#include "openswx/document.h"
// 只用声明和类型；函数体全部靠 LoadLibrary + GetProcAddress 拿到，
// 所以没装 pdfium.dll 也能编译、能运行，只是 PDF 不参与索引。
#include "fpdf_text.h"
#include "fpdfview.h"

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
        // CAE 输入/文本输出：ABAQUS .inp/.dat/.msg/.sta，ANSYS .cdb/.anf/.wbpj
        ".inp", ".dat", ".msg", ".sta", ".cdb", ".anf", ".wbpj",
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
    if (t.size() > room) {
        // 截断不能在 UTF-8 多字节字符中间停下，否则整段会被当成 GBK 解成乱码
        t.resize(room);
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty() && (unsigned char)t.back() >= 0xC0) t.pop_back();
    }
    out.append(t);
    out.push_back('\n');
}

// ---- 各格式抽取 ----

void AppendCodepoint(std::string& buf, unsigned long cp);   // 定义在 P2 那段

// STEP（ISO 10303-21）把非 ASCII 写成 \X2\<UTF-16 十六进制>\X0\，
// 不还原的话「加热针.step」里的中文永远搜不到。
void AppendUtf16Units(std::string& out, const std::string& hex) {
    unsigned long hi = 0;
    for (size_t k = 0; k + 4 <= hex.size(); k += 4) {
        const unsigned long u = strtoul(hex.substr(k, 4).c_str(), nullptr, 16);
        if (u >= 0xD800 && u <= 0xDBFF) {
            hi = u;
            continue;
        }
        if (u >= 0xDC00 && u <= 0xDFFF && hi) {
            const unsigned long cp = 0x10000 + ((hi - 0xD800) << 10) + (u - 0xDC00);
            AppendCodepoint(out, cp);
            hi = 0;
            continue;
        }
        hi = 0;
        AppendCodepoint(out, u);
    }
}

std::string DecodeStepEscapes(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] != '\\') {
            o.push_back(s[i++]);
            continue;
        }
        // \\ -> 反斜杠本身
        if (i + 1 < s.size() && s[i + 1] == '\\') {
            o.push_back('\\');
            i += 2;
            continue;
        }
        // \Xn\ ... \X0\    n = 1 字节 / 2 UTF-16 / 4 UTF-32
        if (i + 3 < s.size() && (s[i + 1] == 'X' || s[i + 1] == 'x') && s[i + 3] == '\\') {
            const char d = s[i + 2];
            const size_t unit = (d == '1') ? 2 : (d == '2') ? 4 : (d == '4') ? 8 : 0;
            if (unit != 0) {
                const size_t end = s.find("\\X0\\", i + 4);
                const size_t stop = (end == std::string::npos) ? s.size() : end;
                std::string hex;
                for (size_t k = i + 4; k < stop; ++k)
                    if (isxdigit((unsigned char)s[k])) hex.push_back(s[k]);

                if (d == '2') {
                    AppendUtf16Units(o, hex);
                } else {
                    for (size_t k = 0; k + unit <= hex.size(); k += unit)
                        AppendCodepoint(o, strtoul(hex.substr(k, unit).c_str(), nullptr, 16));
                }
                i = (end == std::string::npos) ? s.size() : end + 4;
                continue;
            }
        }
        o.push_back(s[i++]);   // 其他转义（\S\ \P?\ \N\）原样保留
    }
    return o;
}

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
    return DecodeStepEscapes(out);
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
    // 质量闸门：.dat/.db 这类扩展名太泛，浏览器缓存/二进制文件不能当正文入库
    size_t bad = 0;
    for (unsigned char c : u8)
        if (c < 0x09 || (c > 0x0D && c < 0x20)) ++bad;
    if (!u8.empty() && bad * 20 > u8.size()) return {};
    return u8;
}

// ======================= P2：OOXML / 压缩包 =======================

bool ReadAll(const std::string& path, uint64_t cap, std::string& out) {
    std::wstring w = ToWide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > cap) {
        CloseHandle(h);
        return false;
    }
    out.assign((size_t)sz.QuadPart, '\0');
    size_t off = 0;
    while (off < out.size()) {
        DWORD want = (DWORD)std::min<size_t>(out.size() - off, 1u << 20);
        DWORD got = 0;
        if (!ReadFile(h, &out[off], want, &got, nullptr) || got == 0) break;
        off += got;
    }
    CloseHandle(h);
    out.resize(off);
    return !out.empty();
}

void AppendCodepoint(std::string& buf, unsigned long cp) {
    if (cp < 0x80) {
        buf.push_back((char)cp);
    } else if (cp < 0x800) {
        buf.push_back((char)(0xC0 | (cp >> 6)));
        buf.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        buf.push_back((char)(0xE0 | (cp >> 12)));
        buf.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        buf.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        buf.push_back((char)(0xF0 | (cp >> 18)));
        buf.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        buf.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        buf.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// 抽 XML 的文本节点（跳过标签），顺手解 &amp; / &#x4E2D; 这类实体
void CollectXmlText(const std::string& xml, std::string& out, size_t cap) {
    std::string buf;
    bool inTag = false;
    for (size_t i = 0; i < xml.size() && out.size() < cap; ++i) {
        const char c = xml[i];
        if (inTag) {
            if (c == '>') {
                inTag = false;
                if (!buf.empty()) { Append(out, buf); buf.clear(); }
            }
            continue;
        }
        if (c == '<') {
            inTag = true;
            if (!buf.empty()) { Append(out, buf); buf.clear(); }
            continue;
        }
        if (c == '&') {
            const size_t semi = xml.find(';', i);
            if (semi != std::string::npos && semi - i <= 12) {
                const std::string ent = xml.substr(i + 1, semi - i - 1);
                if (ent == "amp") buf.push_back('&');
                else if (ent == "lt") buf.push_back('<');
                else if (ent == "gt") buf.push_back('>');
                else if (ent == "quot") buf.push_back('"');
                else if (ent == "apos") buf.push_back('\'');
                else if (ent.size() > 1 && ent[0] == '#') {
                    const unsigned long cp = (ent[1] == 'x' || ent[1] == 'X')
                        ? strtoul(ent.c_str() + 2, nullptr, 16)
                        : strtoul(ent.c_str() + 1, nullptr, 10);
                    if (cp) AppendCodepoint(buf, cp);
                } else {
                    buf.push_back('&');
                    buf += ent;
                    buf.push_back(';');
                }
                i = semi;
                continue;
            }
        }
        buf.push_back(c);
    }
    if (!buf.empty()) Append(out, buf);
}


// ======================= 流式 zip（.mph 能到几百 MB 甚至 GB）=======================

size_t MzReadCb(void* opaque, mz_uint64 ofs, void* buf, size_t n) {
    HANDLE h = (HANDLE)opaque;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)ofs;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, nullptr)) return 0;
    return got;
}

// 不把整个压缩包读进内存：只按需 seek + read
class ZipReader {
  public:
    ~ZipReader() { Close(); }

    bool Open(const std::string& path) {
        Close();
        const std::wstring w = ToWide(path);
        h_ = CreateFileW(w.c_str(), GENERIC_READ,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h_, &sz) || sz.QuadPart <= 0) { Close(); return false; }
        memset(&zip_, 0, sizeof zip_);
        zip_.m_pRead = MzReadCb;
        zip_.m_pIO_opaque = h_;
        if (!mz_zip_reader_init(&zip_, (mz_uint64)sz.QuadPart, 0)) { Close(); return false; }
        ok_ = true;
        return true;
    }

    void Close() {
        if (ok_) { mz_zip_reader_end(&zip_); ok_ = false; }
        if (h_ != INVALID_HANDLE_VALUE) { CloseHandle(h_); h_ = INVALID_HANDLE_VALUE; }
    }

    int Count() { return ok_ ? (int)mz_zip_reader_get_num_files(&zip_) : 0; }
    bool IsDir(int i) {
        return ok_ && mz_zip_reader_is_file_a_directory(&zip_, (mz_uint)i) != 0;
    }
    bool Name(int i, std::string& out) {
        if (!ok_) return false;
        char buf[1024];
        const mz_uint n = mz_zip_reader_get_filename(&zip_, (mz_uint)i, buf, sizeof buf);
        if (n == 0) return false;
        out.assign(buf);
        return true;
    }
    mz_uint64 Size(int i) {
        mz_zip_archive_file_stat st;
        if (!ok_ || !mz_zip_reader_file_stat(&zip_, (mz_uint)i, &st)) return 0;
        return st.m_uncomp_size;
    }
    bool Extract(int i, std::string& out, size_t cap) {
        out.clear();
        if (!ok_) return false;
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip_, (mz_uint)i, &sz, 0);
        if (!p) return false;
        out.assign((const char*)p, sz < cap ? sz : cap);
        mz_free(p);
        return true;
    }

  private:
    HANDLE         h_ = INVALID_HANDLE_VALUE;
    mz_zip_archive zip_{};
    bool           ok_ = false;
};

// JSON 里的字符串值（含 \uXXXX 转义）—— smodel.json 的 label 全在这
void CollectJsonStrings(const std::string& s, std::string& out, size_t cap) {
    std::string buf;
    for (size_t i = 0; i < s.size() && out.size() < cap;) {
        if (s[i] != '"') { ++i; continue; }
        ++i;
        buf.clear();
        while (i < s.size() && s[i] != '"') {
            if (s[i] == '\\' && i + 1 < s.size()) {
                const char e = s[i + 1];
                if (e == 'u' && i + 5 < s.size()) {
                    const unsigned cp = (unsigned)strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    if (cp) AppendCodepoint(buf, cp);
                    i += 6;
                    continue;
                }
                buf.push_back(e == 'n' || e == 'r' || e == 't' ? ' ' : e);
                i += 2;
                continue;
            }
            buf.push_back(s[i++]);
        }
        ++i;
        if (buf.size() >= 2) Append(out, buf);
    }
}

// ======================= COMSOL .mph =======================
//
// .mph 是 zip。COMSOL 6.x 的 model.xml 是空的（207 字节），真正的模型定义在：
//   smodel.json   —— 模型树，label 是用户看到的特征名（含中文）
//   dmodel.xml    —— 参数、变量、物理场、选择、注释
//   modelinfo.xml —— 标题/描述/版本/计算耗时
std::string MphText(const std::string& path) {
    ZipReader z;
    if (!z.Open(path)) return {};

    // 先按名字定位，再按优先级分配配额：
    // smodel.json 里的 label 是用户看得懂的特征名（含中文），最值钱；
    // dmodel.xml 有 5 MB 的参数字段，不限额会把配额吃光。
    int idxJson = -1, idxInfo = -1, idxDmodel = -1, idxVer = -1, idxLic = -1;
    const int n = z.Count();
    for (int i = 0; i < n; ++i) {
        if (z.IsDir(i)) continue;
        std::string name;
        if (!z.Name(i, name)) continue;
        std::string lower = name;
        for (auto& c : lower) c = (char)tolower((unsigned char)c);
        if (lower == "smodel.json") idxJson = i;
        else if (lower == "modelinfo.xml") idxInfo = i;
        else if (lower == "dmodel.xml") idxDmodel = i;
        else if (lower == "fileversion") idxVer = i;
        else if (lower == "usedlicenses.txt") idxLic = i;
    }

    std::string out;
    auto add = [&](int i, int mode, size_t subCap) {
        if (i < 0) return;
        if (z.Size(i) > 64ull * 1024 * 1024) return;
        std::string body;
        if (!z.Extract(i, body, 64u * 1024 * 1024)) return;
        std::string tmp;
        if (mode == 0)      CollectJsonStrings(body, tmp, subCap);
        else if (mode == 1) CollectXmlText(body, tmp, subCap);
        else {
            tmp = Decode(body);
            if (tmp.size() > subCap) tmp.resize(subCap);
        }
        if (out.size() < kStoreCap) {
            const size_t room = kStoreCap - out.size();
            out.append(tmp, 0, std::min(room, tmp.size()));
        }
    };
    add(idxJson, 0, 40 * 1024);     // 模型树 label：中文特征名、参数名
    add(idxInfo, 1, 4 * 1024);      // 标题/描述/版本/计算耗时
    add(idxDmodel, 1, 18 * 1024);   // 参数、变量、物理场、选择
    add(idxVer, 2, 256);
    add(idxLic, 2, 512);
    return out;
}

bool StartsWith(const std::string& s, const char* p) {
    const size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

bool IsOoxmlExt(const std::string& e) {
    return e == ".docx" || e == ".docm" || e == ".dotx" ||
           e == ".xlsx" || e == ".xlsm" ||
           e == ".pptx" || e == ".pptm" ||
           e == ".odt" || e == ".ods" || e == ".odp";
}

// 只读"装着正文"的那几个 XML，不碰 picture/fonts 这类二进制
bool OoxmlEntryWanted(const std::string& ext, const std::string& name) {
    if (name == "docProps/core.xml") return true;
    if (ext == ".docx" || ext == ".docm" || ext == ".dotx")
        return name == "word/document.xml" || StartsWith(name, "word/header") ||
               StartsWith(name, "word/footer") || name == "word/footnotes.xml" ||
               name == "word/endnotes.xml" || StartsWith(name, "word/comments");
    if (ext == ".xlsx" || ext == ".xlsm")
        return name == "xl/sharedStrings.xml" || StartsWith(name, "xl/worksheets/sheet");
    if (ext == ".pptx" || ext == ".pptm")
        return StartsWith(name, "ppt/slides/slide") || StartsWith(name, "ppt/notesSlides/");
    if (ext == ".odt" || ext == ".ods" || ext == ".odp")
        return name == "content.xml" || name == "meta.xml";
    return false;
}

std::string OoxmlFromMem(const std::string& data, const std::string& ext) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof zip);
    if (!mz_zip_reader_init_mem(&zip, data.data(), data.size(), 0)) return {};

    std::string out;
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < n && out.size() < kStoreCap; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
        const std::string name = st.m_filename;
        if (!OoxmlEntryWanted(ext, name)) continue;
        // 单条目上上限：防 zip 炸弹，也避免正文机器生成的大 sheet 卡住
        if (st.m_uncomp_size == 0 || st.m_uncomp_size > 32u * 1024 * 1024) continue;
        size_t sz = 0;
        void* p = mz_zip_reader_extract_to_heap(&zip, i, &sz, 0);
        if (!p) continue;
        std::string xml((const char*)p, sz);
        mz_free(p);
        CollectXmlText(xml, out, kStoreCap);
    }
    mz_zip_reader_end(&zip);
    return out;
}

std::string OoxmlText(const std::string& path, const std::string& ext) {
    ZipReader z;
    if (!z.Open(path)) return {};
    std::string out;
    const int n = z.Count();
    for (int i = 0; i < n && out.size() < kStoreCap; ++i) {
        if (z.IsDir(i)) continue;
        std::string name;
        if (!z.Name(i, name)) continue;
        if (!OoxmlEntryWanted(ext, name)) continue;
        const mz_uint64 sz = z.Size(i);
        if (sz == 0 || sz > 32ull * 1024 * 1024) continue;
        std::string body;
        if (!z.Extract(i, body, 32u * 1024 * 1024)) continue;
        CollectXmlText(body, out, kStoreCap);
    }
    return out;
}

std::string ZipStrings(const std::string& path) {
    ZipReader z;
    if (!z.Open(path)) return {};

    std::string out;
    const int n = std::min(z.Count(), 4000);
    uint64_t budget = 4ull * 1024 * 1024;   // 解压总量上限
    for (int i = 0; i < n && out.size() < kStoreCap; ++i) {
        if (z.IsDir(i)) continue;
        std::string name;
        if (!z.Name(i, name)) continue;
        Append(out, name);                  // 条目名本身也可搜

        const std::string ext = LowerExt(name);
        const bool plain = IsTextExt(ext) || ext == ".dxf" ||
                           ext == ".step" || ext == ".stp" || ext == ".obj";
        const bool ooxml = IsOoxmlExt(ext);
        const bool mph   = ext == ".mph";
        if (!plain && !ooxml && !mph) continue;
        const mz_uint64 sz = z.Size(i);
        if (sz == 0 || sz > budget) continue;

        std::string inner;
        if (!z.Extract(i, inner, (size_t)budget)) continue;
        budget -= std::min<uint64_t>(budget, inner.size());

        if (ooxml) {
            std::string t = OoxmlFromMem(inner, ext);
            if (out.size() < kStoreCap) {
                const size_t room = kStoreCap - out.size();
                out.append(t, 0, std::min(room, t.size()));
            }
        } else if (mph) {
            // 嵌套的 .mph 只当二进制，不递归展开（里面已经是压缩流）
            continue;
        } else {
            std::string u8 = Decode(inner);
            if (u8.size() > kStoreCap) u8.resize(kStoreCap);
            Append(out, u8);
        }
    }
    return out;
}

// ======================= P3：PDF（pdfium 动态加载）=======================

struct PdfApi {
    bool    tried = false;
    bool    ok = false;
    HMODULE mod = nullptr;
    decltype(&FPDF_InitLibrary) InitLibrary = nullptr;
    decltype(&FPDF_LoadCustomDocument) LoadCustom = nullptr;
    decltype(&FPDF_CloseDocument) CloseDocument = nullptr;
    decltype(&FPDF_GetPageCount) GetPageCount = nullptr;
    decltype(&FPDF_LoadPage) LoadPage = nullptr;
    decltype(&FPDF_ClosePage) ClosePage = nullptr;
    decltype(&FPDFText_LoadPage) TextLoad = nullptr;
    decltype(&FPDFText_ClosePage) TextClose = nullptr;
    decltype(&FPDFText_CountChars) TextCount = nullptr;
    decltype(&FPDFText_GetText) TextGet = nullptr;
    // 扫描件 OCR 用的渲染接口
    decltype(&FPDF_GetPageWidthF) PageWidth = nullptr;
    decltype(&FPDF_GetPageHeightF) PageHeight = nullptr;
    decltype(&FPDFBitmap_Create) BitmapCreate = nullptr;
    decltype(&FPDFBitmap_FillRect) BitmapFill = nullptr;
    decltype(&FPDFBitmap_GetBuffer) BitmapBuffer = nullptr;
    decltype(&FPDFBitmap_GetStride) BitmapStride = nullptr;
    decltype(&FPDFBitmap_Destroy) BitmapDestroy = nullptr;
    decltype(&FPDF_RenderPageBitmap) RenderPage = nullptr;
};

// 只在首次用到 PDF 时加载一次；InitLibrary 会装字体，不能每个文件重来
PdfApi& Pdf() {
    static PdfApi api;
    if (api.tried) return api;
    api.tried = true;

    api.mod = LoadLibraryW(L"pdfium.dll");
    if (!api.mod) return api;

    auto sym = [&](const char* n) { return (void*)GetProcAddress(api.mod, n); };
    api.InitLibrary   = (decltype(api.InitLibrary))sym("FPDF_InitLibrary");
    api.LoadCustom    = (decltype(api.LoadCustom))sym("FPDF_LoadCustomDocument");
    api.CloseDocument = (decltype(api.CloseDocument))sym("FPDF_CloseDocument");
    api.GetPageCount  = (decltype(api.GetPageCount))sym("FPDF_GetPageCount");
    api.LoadPage      = (decltype(api.LoadPage))sym("FPDF_LoadPage");
    api.ClosePage     = (decltype(api.ClosePage))sym("FPDF_ClosePage");
    api.TextLoad      = (decltype(api.TextLoad))sym("FPDFText_LoadPage");
    api.TextClose     = (decltype(api.TextClose))sym("FPDFText_ClosePage");
    api.TextCount     = (decltype(api.TextCount))sym("FPDFText_CountChars");
    api.TextGet       = (decltype(api.TextGet))sym("FPDFText_GetText");
    api.PageWidth     = (decltype(api.PageWidth))sym("FPDF_GetPageWidthF");
    api.PageHeight    = (decltype(api.PageHeight))sym("FPDF_GetPageHeightF");
    api.BitmapCreate  = (decltype(api.BitmapCreate))sym("FPDFBitmap_Create");
    api.BitmapFill    = (decltype(api.BitmapFill))sym("FPDFBitmap_FillRect");
    api.BitmapBuffer  = (decltype(api.BitmapBuffer))sym("FPDFBitmap_GetBuffer");
    api.BitmapStride  = (decltype(api.BitmapStride))sym("FPDFBitmap_GetStride");
    api.BitmapDestroy = (decltype(api.BitmapDestroy))sym("FPDFBitmap_Destroy");
    api.RenderPage    = (decltype(api.RenderPage))sym("FPDF_RenderPageBitmap");

    api.ok = api.InitLibrary && api.LoadCustom && api.CloseDocument && api.GetPageCount &&
             api.LoadPage && api.ClosePage && api.TextLoad && api.TextClose &&
             api.TextCount && api.TextGet;
    if (api.ok) api.InitLibrary();
    return api;
}

int PdfGetBlock(void* param, unsigned long position, unsigned char* buf, unsigned long size) {    HANDLE h = (HANDLE)param;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)position;
    if (!SetFilePointerEx(h, li, nullptr, FILE_BEGIN)) return 0;
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)size, &got, nullptr)) return 0;
    return (int)got;
}

std::string OcrText(const std::string& path);   // 定义在后面的 OCR 那段

// BGRA/BGRx 位图 → 24 位 BMP（自下而上，行 4 字节对齐）
bool WriteBmp(const std::wstring& path, const unsigned char* buf, int w, int h, int stride) {
    const int rowBytes = ((w * 3) + 3) & ~3;
    const uint32_t imgSize = (uint32_t)rowBytes * (uint32_t)h;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;

    unsigned char hdr[54] = {0};
    auto put16 = [&](int o, unsigned v) {
        hdr[o] = (unsigned char)v;
        hdr[o + 1] = (unsigned char)(v >> 8);
    };
    auto put32 = [&](int o, unsigned v) {
        for (int i = 0; i < 4; ++i) hdr[o + i] = (unsigned char)(v >> (8 * i));
    };
    hdr[0] = 'B'; hdr[1] = 'M';
    put32(2, 54 + imgSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, (unsigned)w);
    put32(22, (unsigned)h);
    put16(26, 1);
    put16(28, 24);
    put32(34, imgSize);
    put32(38, 2835);
    put32(42, 2835);

    DWORD wr = 0;
    WriteFile(f, hdr, 54, &wr, nullptr);
    std::vector<unsigned char> row((size_t)rowBytes, 0);
    for (int y = h - 1; y >= 0; --y) {
        const unsigned char* src = buf + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        WriteFile(f, row.data(), (DWORD)rowBytes, &wr, nullptr);
    }
    CloseHandle(f);
    return true;
}

// 渲染一页成图，交给 tesseract。扫描件 PDF 只有这条路能拿到文字。
std::string OcrPdfPage(PdfApi& api, FPDF_PAGE page) {
    const bool dbg = std::getenv("SPACESIFT_KEEP_OCR_BMP") != nullptr;
    if (!api.PageWidth || !api.PageHeight || !api.BitmapCreate || !api.RenderPage ||
        !api.BitmapBuffer || !api.BitmapStride || !api.BitmapDestroy) {
        if (dbg) fprintf(stderr, "[ocr] symbols missing\n");
        return {};
    }

    const float wPt = api.PageWidth(page);
    const float hPt = api.PageHeight(page);
    if (wPt <= 1.0f || hPt <= 1.0f) return {};

    float scale = 1600.0f / wPt;      // 大约 150–200 DPI，OCR 够用
    if (scale < 1.0f) scale = 1.0f;
    if (scale > 3.0f) scale = 3.0f;
    const int w = (int)(wPt * scale);
    const int h = (int)(hPt * scale);
    if (dbg) fprintf(stderr, "[ocr] page %.1fx%.1fpt -> %dx%d px\n", wPt, hPt, w, h);
    if (w <= 0 || h <= 0 || w > 6000 || h > 6000) return {};

    FPDF_BITMAP bmp = api.BitmapCreate(w, h, 0);   // 0 = BGRA
    if (!bmp) { if (dbg) fprintf(stderr, "[ocr] bitmap create failed\n"); return {}; }
    api.BitmapFill(bmp, 0, 0, w, h, 0xFFFFFFFF);
    api.RenderPage(bmp, page, 0, 0, w, h, 0, 0);

    const unsigned char* buf = (const unsigned char*)api.BitmapBuffer(bmp);
    const int stride = api.BitmapStride(bmp);
    std::string text;
    if (buf && stride > 0) {
        const std::wstring bmpPath = external::TempName(L".bmp");
        if (!bmpPath.empty() && WriteBmp(bmpPath, buf, w, h, stride)) {
            if (dbg) fprintf(stderr, "[ocr] wrote %ls\n", bmpPath.c_str());
            text = OcrText(FromWide(bmpPath));
            // 调试：SPACESIFT_KEEP_OCR_BMP=1 保留位图，方便看渲染对不对
            if (!std::getenv("SPACESIFT_KEEP_OCR_BMP")) external::DeleteQuiet(bmpPath);
        }
    }
    api.BitmapDestroy(bmp);
    return text;
}

// 只抽文本层。扫描件（纯图）没文本层时，渲染后走 OCR。
std::string PdfText(const std::string& path) {
    PdfApi& api = Pdf();
    if (!api.ok) return {};

    std::wstring w = ToWide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || (uint64_t)sz.QuadPart > kSkipAbove) {
        CloseHandle(h);
        return {};
    }

    FPDF_FILEACCESS acc;
    acc.m_FileLen = (unsigned long)sz.QuadPart;
    acc.m_GetBlock = PdfGetBlock;
    acc.m_Param = h;

    // 用自定义读取器而不是 FPDF_LoadDocument，避开中文路径的编码问题
    FPDF_DOCUMENT doc = api.LoadCustom(&acc, nullptr);
    if (!doc) {
        CloseHandle(h);
        return {};
    }

    std::string out;
    const int pages = api.GetPageCount(doc);
    const int kMaxPages = 300;          // 手册类 PDF 动辄上千页，截到前 300 页
    const bool canOcr = OcrReady();
    int ocrPages = 0;
    for (int i = 0; i < pages && i < kMaxPages && out.size() < kStoreCap; ++i) {
        FPDF_PAGE page = api.LoadPage(doc, i);
        if (!page) continue;

        int n = 0;
        FPDF_TEXTPAGE tp = api.TextLoad(page);
        if (tp) {
            n = api.TextCount(tp);
            if (n > 0) {
                std::vector<unsigned short> buf((size_t)n + 1, 0);
                const int got = api.TextGet(tp, 0, n + 1, buf.data());   // 返回值含结尾 0
                if (got > 1) {
                    std::string u16((const char*)buf.data(), (size_t)(got - 1) * 2);
                    std::string u8 = Utf16LeToUtf8(u16);
                    if (u8.size() > kStoreCap) u8.resize(kStoreCap);
                    Append(out, u8);
                }
            }
            api.TextClose(tp);
        }

        // 文本层为空 → 当扫描件处理。一篇文章最多 OCR 前 5 页，别让索引变慢动作
        const bool debugRender = std::getenv("SPACESIFT_KEEP_OCR_BMP") != nullptr;
        if (debugRender) fprintf(stderr, "[ocr] page %d textchars=%d canOcr=%d\n", i, n, (int)canOcr);
        if (n <= 2 && (canOcr || debugRender) && ocrPages < 5 && out.size() < kStoreCap) {
            std::string ocr = OcrPdfPage(api, page);
            if (!ocr.empty()) {
                ++ocrPages;
                if (ocr.size() > kStoreCap) ocr.resize(kStoreCap);
                Append(out, ocr);
            }
        }
        api.ClosePage(page);
    }

    api.CloseDocument(doc);
    CloseHandle(h);
    return out;
}

// ======================= P4：DWG（外部转换器）=======================

// 优先级：LibreDWG 的 dwg2dxf（自带、开箱即用）> ODA File Converter（机器上装了就用，更准）
struct DwgConv {
    bool         tried = false;
    int          kind = -1;     // 0 = dwg2dxf, 1 = ODA
    std::wstring exe;
};

std::wstring FindOda() {
    wchar_t win[MAX_PATH];
    const DWORD n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n == 0 || n < 2) return {};
    const std::wstring drive(win, 2);          // "C:"

    const wchar_t* roots[] = {L"\\Program Files\\ODA\\",
                              L"\\Program Files (x86)\\ODA\\"};
    for (const wchar_t* r : roots) {
        const std::wstring pattern = drive + r + L"*";
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            const std::wstring exe = drive + r + fd.cFileName + L"\\ODAFileConverter.exe";
            if (external::FileExists(exe)) {
                FindClose(h);
                return exe;
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return {};
}

DwgConv& DwgConverter() {
    static DwgConv c;
    if (c.tried) return c;
    c.tried = true;

    const std::wstring dir = external::ExeDir();
    const wchar_t* rel[] = {L"\\dwg2dxf.exe", L"\\libredwg\\dwg2dxf.exe",
                            L"\\dwg2dxf\\dwg2dxf.exe", L"\\bin\\dwg2dxf.exe"};
    for (const wchar_t* r : rel) {
        const std::wstring p = dir + r;
        if (external::FileExists(p)) { c.kind = 0; c.exe = p; return c; }
    }
    const std::wstring onPath = external::FindOnPath(L"dwg2dxf.exe");
    if (!onPath.empty()) { c.kind = 0; c.exe = onPath; return c; }

    const std::wstring oda = FindOda();
    if (!oda.empty()) { c.kind = 1; c.exe = oda; return c; }
    return c;
}

std::string DwgText(const std::string& path) {
    DwgConv& c = DwgConverter();
    if (c.kind < 0) return {};
    const std::wstring src = ToWide(path);
    std::string raw;

    if (c.kind == 0) {
        const std::wstring out = external::TempName(L".dxf");
        if (out.empty()) return {};
        const std::wstring args = L"-y -y -o \"" + out + L"\" \"" + src + L"\"";
        external::RunSilent(c.exe, args, 120000);
        if (external::FileExists(out)) raw = DxfStrings(FromWide(out));
        external::DeleteQuiet(out);
        return raw;
    }

    // ODA 只支持文件夹批处理：建临时入/出目录，把文件拷进去
    const std::wstring inDir = external::TempName(L"_in");
    const std::wstring outDir = external::TempName(L"_out");
    if (inDir.empty() || outDir.empty()) return {};
    if (!CreateDirectoryW(inDir.c_str(), nullptr)) return {};
    if (!CreateDirectoryW(outDir.c_str(), nullptr)) { RemoveDirectoryW(inDir.c_str()); return {}; }

    const std::wstring inFile = inDir + L"\\in.dwg";
    if (!CopyFileW(src.c_str(), inFile.c_str(), FALSE)) {
        RemoveDirectoryW(inDir.c_str());
        RemoveDirectoryW(outDir.c_str());
        return {};
    }

    const std::wstring args = L"\"" + inDir + L"\" \"" + outDir +
                              L"\" ACAD2018 DXF 0 1 \"*.dwg\"";
    external::RunSilent(c.exe, args, 300000);

    // 产物名不一定可靠，直接扫出目录里的第一个 .dxf
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((outDir + L"\\*.dxf").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            raw = DxfStrings(FromWide(outDir + L"\\" + fd.cFileName));
        FindClose(h);
        WIN32_FIND_DATAW f2;
        HANDLE h2 = FindFirstFileW((outDir + L"\\*").c_str(), &f2);
        if (h2 != INVALID_HANDLE_VALUE) {
            do {
                if ((f2.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
                    external::DeleteQuiet(outDir + L"\\" + f2.cFileName);
            } while (FindNextFileW(h2, &f2));
            FindClose(h2);
        }
    }
    external::DeleteQuiet(inFile);
    RemoveDirectoryW(inDir.c_str());
    RemoveDirectoryW(outDir.c_str());
    return raw;
}

// ======================= .doc / .ppt / .xls（外部转换器）=======================

// 97-2003 的 Office 二进制里，中文是 UTF-16LE 存的。
// 没装 catdoc/LibreOffice 时，直接扫 UTF-16LE 可读串——零依赖也能搜到中文。
// 实测：一份 46 KB 的中期评定表能抽出 2500 多字，标题、学院、专业、正文都在。
bool ReadableCp(unsigned cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3000 && cp <= 0x303F) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20 && cp <= 0x7E) ||
           cp == 0x0D || cp == 0x0A || cp == 0x09;
}

std::string ScanUtf16At(const std::string& raw, size_t parity, int* charsOut) {
    std::string out, buf;
    int cnt = 0, total = 0;
    const size_t n = raw.size() ? raw.size() - 1 : 0;
    size_t i = parity;
    while (i < n && out.size() < kStoreCap) {
        const unsigned cp = (unsigned char)raw[i] | ((unsigned char)raw[i + 1] << 8);
        if (ReadableCp(cp)) {
            AppendCodepoint(buf, cp);
            ++cnt;
            i += 2;
            continue;
        }
        if (cnt >= 6) { Append(out, buf); total += cnt; }   // 太短的段基本都是二进制噪音
        buf.clear();
        cnt = 0;
        // 断档后必须回到指定奇偶性再重开一段，
        // 否则奇数字节上的假阳性（如 0x5700 落在汉字区）会把首字吃掉
        i += 1;
        if (((i - parity) & 1) != 0) i += 1;
    }
    if (cnt >= 6) { Append(out, buf); total += cnt; }
    if (charsOut) *charsOut = total;
    return out;
}

std::string LegacyUtf16Strings(const std::string& path) {
    Reader r;
    if (!r.Open(path, 64ull * 1024 * 1024)) return {};
    std::string raw;
    if (!r.ReadPrefix(64u * 1024 * 1024, raw)) return {};

    // 奇/偶字节各扫一遍取更长的那个：
    // UTF-16LE 正常对齐到偶数字节，只扫一边的话错位处的假阳性会把首字吃掉。
    int evenChars = 0, oddChars = 0;
    std::string even = ScanUtf16At(raw, 0, &evenChars);
    std::string odd  = ScanUtf16At(raw, 1, &oddChars);
    return (oddChars > evenChars) ? odd : even;
}

// 从注册表 App Paths 找安装路径：WinRAR/7-Zip 装在非标准目录时靠它
// （本机 WinRAR 就在 C:\D\Program Files\WinRAR，写死的路径根本盖不到）
std::wstring RegAppPath(const wchar_t* exe) {
    const std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\App Paths\\") + exe;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &k) != ERROR_SUCCESS)
        return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD sz = sizeof buf, type = 0;
    std::wstring out;
    if (RegQueryValueExW(k, nullptr, nullptr, &type, (BYTE*)buf, &sz) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ))
        out.assign(buf, sz / sizeof(wchar_t));
    RegCloseKey(k);
    while (!out.empty() && out.back() == L'\0') out.pop_back();
    return out;
}

std::wstring FindTool(const wchar_t* name) {
    std::wstring p = external::FindOnPath(name);
    if (!p.empty()) return p;
    const std::wstring dir = external::ExeDir();
    for (const wchar_t* rel : {L"\\", L"\\bin\\", L"\\tools\\"}) {
        const std::wstring q = dir + rel + name;
        if (external::FileExists(q)) return q;
    }
    // 常见安装位置：装完但 PATH 没刷新也能找到
    wchar_t win[MAX_PATH];
    const DWORD n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n >= 2) {
        const std::wstring drive(win, 2);
        const std::wstring cands[] = {
            drive + L"\\Program Files\\Tesseract-OCR\\" + name,
            drive + L"\\Program Files (x86)\\Tesseract-OCR\\" + name,
            drive + L"\\Program Files\\7-Zip\\" + name,
            drive + L"\\Program Files (x86)\\7-Zip\\" + name,
            drive + L"\\Program Files\\WinRAR\\" + name,
        };
        for (const auto& c : cands)
            if (external::FileExists(c)) return c;
    }
    // 注册表兜底
    {
        const std::wstring direct = RegAppPath(name);
        if (!direct.empty() && external::FileExists(direct)) return direct;

        for (const wchar_t* host : {L"WinRAR.exe", L"7zFM.exe", L"7z.exe"}) {
            const std::wstring p = RegAppPath(host);
            if (p.empty()) continue;
            const size_t slash = p.find_last_of(L"\\/");
            if (slash == std::wstring::npos) continue;
            const std::wstring cand = p.substr(0, slash + 1) + name;
            if (external::FileExists(cand)) return cand;
        }
    }
    return {};
}

// 如果 exe 旁边有 tessdata 目录，就用它（用户可以免管理员装语言包）
std::wstring TessdataArgs() {
    const std::wstring dir = external::ExeDir() + L"\\tessdata";
    if (external::DirExists(dir)) return L"--tessdata-dir \"" + dir + L"\" ";
    return {};
}

std::wstring FindSoffice() {
    std::wstring p = FindTool(L"soffice.exe");
    if (!p.empty()) return p;
    wchar_t win[MAX_PATH];
    const DWORD n = GetWindowsDirectoryW(win, MAX_PATH);
    if (n == 0 || n < 2) return {};
    const std::wstring drive(win, 2);
    for (const wchar_t* r : {L"\\Program Files\\LibreOffice\\program\\soffice.exe",
                             L"\\Program Files (x86)\\LibreOffice\\program\\soffice.exe"}) {
        const std::wstring q = drive + r;
        if (external::FileExists(q)) return q;
    }
    return {};
}

// LibreOffice 把结果写到文件而不是 stdout
std::string SofficeText(const std::wstring& soffice, const std::string& path) {
    const std::wstring dir = external::TempName(L"_lo");
    if (dir.empty() || !CreateDirectoryW(dir.c_str(), nullptr)) return {};

    const std::wstring args = L"--headless --norestore --nolockcheck "
                              L"--convert-to \"txt:Text (encoded):UTF8\" "
                              L"--outdir \"" + dir + L"\" \"" + ToWide(path) + L"\"";
    external::RunSilent(soffice, args, 180000);

    size_t slash = path.find_last_of('\\');
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos) base = base.substr(0, dot);

    const std::wstring outFile = dir + L"\\" + ToWide(base) + L".txt";
    std::string raw;
    external::ReadFileBytes(outFile, raw, kStoreCap * 2);
    external::DeleteQuiet(outFile);
    RemoveDirectoryW(dir.c_str());
    return raw;
}

std::string LegacyText(const std::string& path, const std::string& ext) {
    const std::wstring src = ToWide(path);
    std::string out;
    DWORD code = 1;
    // 工具失败时错误信息会进 stdout/stderr，必须看退出码，否则会把错误当成正文
    auto tryRun = [&](const wchar_t* tool, const std::wstring& args, std::string& dst) -> bool {
        const std::wstring exe = FindTool(tool);
        if (exe.empty()) return false;
        dst.clear();
        return external::RunCapture(exe, args, dst, kStoreCap, 60000, &code) && code == 0;
    };
    std::string tmp;

    // catdoc 系优先：启动快，直接吐 stdout
    if (ext == ".doc") {
        if (tryRun(L"catdoc.exe", L"-d utf-8 \"" + src + L"\"", tmp)) return tmp;
    } else if (ext == ".ppt") {
        if (tryRun(L"catppt.exe", L"-d utf-8 \"" + src + L"\"", tmp)) return tmp;
    } else if (ext == ".xls") {
        if (tryRun(L"xls2csv.exe", L"-d utf-8 \"" + src + L"\"", tmp)) return tmp;
    }

    // 零依赖兜底：直接扫 UTF-16LE。
    // 这一档必须排在 antiword 前面 —— antiword 没有中文映射时会把汉字全输出成 '?'。
    {
        std::string u16 = LegacyUtf16Strings(path);
        if (u16.size() >= 200) return u16;
        if (!u16.empty()) out = u16;      // 太少就留着和后面的拼
    }

    if (ext == ".doc" && tryRun(L"antiword.exe", L"-m UTF-8.txt \"" + src + L"\"", tmp)) {
        if (!out.empty()) out += "\n";
        out += tmp;                       // 英文文档 antiword 排版更好，拼在后面
        return out;
    }

    // 兜底 LibreOffice（慢：每个文件都要起一次 soffice）
    const std::wstring so = FindSoffice();
    if (!so.empty()) {
        std::string soText = SofficeText(so, path);
        if (!soText.empty()) {
            if (!out.empty()) out += "\n";
            out += soText;
            return out;
        }
    }
    return out;
}

// ======================= .7z / .rar（外部 7z / UnRAR）=======================

std::wstring FindArchiver(std::wstring* kind) {
    for (const wchar_t* n : {L"7z.exe", L"7za.exe", L"7zz.exe"}) {
        const std::wstring p = FindTool(n);
        if (!p.empty()) { if (kind) *kind = L"7z"; return p; }
    }
    for (const wchar_t* n : {L"UnRAR.exe", L"unrar.exe"}) {
        const std::wstring p = FindTool(n);
        if (!p.empty()) { if (kind) *kind = L"unrar"; return p; }
    }
    return {};
}

// 只能列条目名：逐个解出来抽文本意味着每个条目起一次进程，代价太高。
// 但"哪个压缩包里有 XXX.dwg"已经能回答了。
std::string ArchiveText(const std::string& path) {
    std::wstring kind;
    const std::wstring exe = FindArchiver(&kind);
    if (exe.empty()) return {};

    std::string out;
    DWORD code = 1;
    if (kind == L"7z") {
        if (!external::RunCapture(exe, L"l -ba -slt -- \"" + ToWide(path) + L"\"", out,
                                  kStoreCap, 60000, &code) ||
            code != 0)
            return {};
        std::string names;
        size_t pos = 0;
        while (pos < out.size()) {
            size_t nl = out.find('\n', pos);
            if (nl == std::string::npos) nl = out.size();
            const std::string line = out.substr(pos, nl - pos);
            if (line.compare(0, 7, "Path = ") == 0) Append(names, line.substr(7));
            pos = nl + 1;
        }
        return names;
    }
    // UnRAR lb：一行一个文件名
    if (!external::RunCapture(exe, L"lb \"" + ToWide(path) + L"\"", out, kStoreCap, 60000, &code) ||
        code != 0)
        return {};
    std::string names;
    size_t pos = 0;
    while (pos < out.size()) {
        size_t nl = out.find('\n', pos);
        if (nl == std::string::npos) nl = out.size();
        Append(names, out.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return names;
}

// ======================= SolidWorks（.sldprt/.sldasm/.slddrw）=======================

// 几何体内部没有可搜文字，能搜的是：
//   全局/配置自定义属性、配置名、装配体引用的零件路径、焊接清单、图纸页与视图名
void AppendLine(std::string& out, const std::string& s) {
    if (s.empty() || out.size() >= kStoreCap) return;
    out += s;
    out.push_back('\n');
}

std::string SwText(const std::string& path) {
    std::string out;
    try {
        // 必须用宽字符构造 path：std::filesystem::path 从窄字符串构造时
        // 按 ANSI 解释，中文路径会直接挂掉。
        auto res = openswx::SwxDocument::Open(std::filesystem::path(ToWide(path)));
        if (!res.ok()) return {};
        const openswx::Document& d = res.value().doc();

        for (const auto& kv : d.global_properties) {
            AppendLine(out, kv.first);
            AppendLine(out, kv.second);
        }
        for (const auto& c : d.configurations) {
            AppendLine(out, c.name);
            for (const auto& kv : c.properties) {
                AppendLine(out, kv.first);
                AppendLine(out, kv.second);
            }
            // 装配体：引用的零件名/路径/配置名，以及焊接清单
            for (const auto& comp : c.components) {
                AppendLine(out, comp.name);
                AppendLine(out, comp.path);
                AppendLine(out, comp.configuration_name);
            }
            for (const auto& cut : c.cut_list) {
                AppendLine(out, cut.name);
                for (const auto& kv : cut.properties) {
                    AppendLine(out, kv.first);
                    AppendLine(out, kv.second);
                }
            }
        }
        for (const auto& sh : d.sheets) {
            AppendLine(out, sh.name);
            for (const auto& v : sh.views) {
                AppendLine(out, v.name);
                AppendLine(out, v.referenced_document);
                AppendLine(out, v.referenced_configuration);
            }
        }
    } catch (...) {
        return {};    // 第三方库遇到奇怪输入时可能抛异常，别把整个索引拖下去
    }
    if (out.size() > kStoreCap) out.resize(kStoreCap);
    return out;
}

// ======================= 扫描件/截图 OCR（外部 tesseract）=======================

std::string OcrText(const std::string& path) {
    const std::wstring exe = FindTool(L"tesseract.exe");
    if (exe.empty()) return {};
    const std::wstring src = ToWide(path);
    const std::wstring pre = TessdataArgs();
    std::string out;
    DWORD code = 1;
    // 中文文档用 chi_sim 单独识别比 chi_sim+eng 好得多：
    // 带 eng 时中文会被当英文认（实测“李工”变“SL”、“张工”变“KL”）。
    // 先试 chi_sim，拿不到东西再退回默认（英文）。
    if (external::RunCapture(exe, pre + L"\"" + src + L"\" stdout -l chi_sim", out,
                             kStoreCap, 120000, &code) && code == 0 && out.size() >= 8)
        return out;
    out.clear();
    if (external::RunCapture(exe, pre + L"\"" + src + L"\" stdout", out, kStoreCap, 120000, &code) &&
        code == 0)
        return out;
    return {};
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

bool StreamsLargeFiles(const std::string& ext) {
    return ext == ".zip" || ext == ".mph";   // 压缩包只按需读条目，GB 级也不怕
}

bool Supports(const std::string& ext) {
    if (IsTextExt(ext)) return true;
    if (ext == ".dxf" || ext == ".step" || ext == ".stp" || ext == ".obj") return true;
    if (IsOoxmlExt(ext)) return true;      // docx/xlsx/pptx/odt...
    if (ext == ".zip") return true;
    // 下面这些靠外部依赖，没装就不列进来，免得全部标成“抽取失败”
    if (ext == ".pdf") return PdfReady();
    if (ext == ".dwg") return DwgReady();
    if (ext == ".doc" || ext == ".ppt" || ext == ".xls") return LegacyReady();
    if (ext == ".7z" || ext == ".rar") return ArchiveReady();
    if (ext == ".sldprt" || ext == ".sldasm" || ext == ".slddrw") return true;
    if (ext == ".mph") return true;   // COMSOL 模型（zip）
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
        ext == ".tif" || ext == ".tiff")
        return OcrReady();
    return false;
}

bool PdfReady() { return Pdf().ok; }
bool DwgReady() { return DwgConverter().kind >= 0; }
bool LegacyReady() {
    return !FindTool(L"catdoc.exe").empty() || !FindTool(L"catppt.exe").empty() ||
           !FindTool(L"xls2csv.exe").empty() || !FindTool(L"antiword.exe").empty() ||
           !FindSoffice().empty();
}
bool ArchiveReady() { return !FindArchiver(nullptr).empty(); }
namespace {
bool g_ocrEnabled = false;   // 默认关闭：一张图 OCR 要 1–2 秒，全盘扫会拖死
}

bool OcrAvailable() { return !FindTool(L"tesseract.exe").empty(); }
void SetOcrEnabled(bool on) { g_ocrEnabled = on; }
bool OcrReady() { return g_ocrEnabled && OcrAvailable(); }

// 截到完整 UTF-8 字符边界：Append 按字节封顶，末尾可能剩半个字
static std::string DecodeAndTrim(const std::string& raw) {
    std::string u = Decode(raw);
    size_t i = u.size();
    while (i > 0 && ((unsigned char)u[i - 1] & 0xC0) == 0x80) --i;
    if (i > 0) {
        const unsigned char lead = (unsigned char)u[i - 1];
        const size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3 : (lead >= 0xC0) ? 2 : 1;
        if (u.size() - (i - 1) < need) u.resize(i - 1);
    }
    return u;
}

std::string Text(const std::string& path, const std::string& ext) {
    // 这些分支内部已经是 UTF-8，直接返回
    if (ext == ".pdf") return PdfText(path);
    if (IsOoxmlExt(ext)) return OoxmlText(path, ext);
    if (ext == ".zip") return ZipStrings(path);

    // 下面这些吐的是“原始字节”：DXF/STEP 可能是 GBK（$DWGCODEPAGE ANSI_936）
    if (ext == ".dwg") return DecodeAndTrim(DwgText(path));
    if (ext == ".doc" || ext == ".ppt" || ext == ".xls") return DecodeAndTrim(LegacyText(path, ext));
    if (ext == ".7z" || ext == ".rar") return DecodeAndTrim(ArchiveText(path));
    if (ext == ".sldprt" || ext == ".sldasm" || ext == ".slddrw") return SwText(path);
    if (ext == ".mph") return MphText(path);
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" ||
        ext == ".tif" || ext == ".tiff")
        return DecodeAndTrim(OcrText(path));

    std::string raw;
    if (ext == ".dxf") {
        raw = DxfStrings(path);
        if (raw.empty()) raw = Strings(path);      // 二进制 DXF / 加密图纸兜底
    } else if (ext == ".step" || ext == ".stp") {
        raw = StepStrings(path);
        if (raw.empty()) raw = Strings(path);
    } else if (ext == ".obj") {
        raw = ObjStrings(path);
    } else {
        return PlainText(path);                    // 内部已做过编码探测
    }
    return DecodeAndTrim(raw);
}

}  // namespace extract
