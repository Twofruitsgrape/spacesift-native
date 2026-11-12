#include "office.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "extract.h"

namespace office {
namespace {

const size_t kCap = 64 * 1024;   // 和 extract 的 kStoreCap 一致

// ---------------- 基础工具 ----------------

uint16_t Rd16(const std::vector<uint8_t>& b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return (uint16_t)(b[o] | (b[o + 1] << 8));
}
uint32_t Rd32(const std::vector<uint8_t>& b, size_t o) {
    if (o + 4 > b.size()) return 0;
    return (uint32_t)(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24));
}

void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

bool Readable(unsigned cp) {
    return (cp >= 0x20 && cp <= 0x7E) || (cp >= 0xA0 && cp <= 0xFF) ||
           (cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || cp == 0x09;
}

// 把一段字节按"UTF-16LE 还是单字节"解码成文本，只保留可读片段
std::string DecodeRun(const std::vector<uint8_t>& b, size_t from, size_t to, bool utf16) {
    std::string out;
    std::string cur;
    int cnt = 0;
    if (to > b.size()) to = b.size();
    if (utf16) {
        for (size_t i = from; i + 1 < to; i += 2) {
            const unsigned cp = (unsigned)b[i] | ((unsigned)b[i + 1] << 8);
            if (Readable(cp) && cp != 0) {
                AppendUtf8(cur, cp);
                ++cnt;
                continue;
            }
            if (cnt >= 4) { out += cur; out += '\n'; }
            cur.clear();
            cnt = 0;
        }
    } else {
        for (size_t i = from; i < to; ++i) {
            const unsigned c = b[i];
            if (c >= 0x20 && c != 0x7F) {
                cur.push_back((char)c);
                ++cnt;
                continue;
            }
            if (cnt >= 4) { out += cur; out += '\n'; }
            cur.clear();
            cnt = 0;
        }
    }
    if (cnt >= 4) { out += cur; out += '\n'; }
    return out;
}

// 整文件扫 UTF-16LE，过滤"ASCII 被两两读成假汉字"（低字节全 ASCII 就丢掉）
std::string ScanUtf16Filtered(const std::vector<uint8_t>& b, size_t limit) {
    std::string out, cur;
    int cnt = 0, cjk = 0;
    bool allLowAscii = true;
    if (limit > b.size()) limit = b.size();
    for (size_t i = 0; i + 1 < limit; i += 2) {
        const unsigned cp = (unsigned)b[i] | ((unsigned)b[i + 1] << 8);
        if (Readable(cp) && cp != 0) {
            AppendUtf8(cur, cp);
            ++cnt;
            if (cp >= 0x4E00 && cp <= 0x9FFF) ++cjk;
            if ((cp & 0xFF) > 0x7F) allLowAscii = false;
            continue;
        }
        if (cnt >= 6 && cjk * 2 >= cnt && !allLowAscii) { out += cur; out += '\n'; }
        cur.clear();
        cnt = 0;
        cjk = 0;
        allLowAscii = true;
        ++i;   // 错位重试
    }
    if (cnt >= 6 && cjk * 2 >= cnt && !allLowAscii) { out += cur; out += '\n'; }
    return out;
}

// ---------------- CFB（OLE2 复合文档）----------------

class Cfb {
  public:
    bool Open(const std::string& path) {
        std::wstring w = extract::ToWide(path);
        HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER sz;
        if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (200ll << 20)) {
            CloseHandle(h);
            return false;
        }
        data_.resize((size_t)sz.QuadPart);
        size_t off = 0;
        while (off < data_.size()) {
            DWORD got = 0;
            const DWORD want = (DWORD)std::min<size_t>(data_.size() - off, 1u << 20);
            if (!ReadFile(h, data_.data() + off, want, &got, nullptr) || got == 0) break;
            off += got;
        }
        CloseHandle(h);
        data_.resize(off);
        return Parse();
    }

    // 取名为 name 的流的字节
    std::vector<uint8_t> Stream(const char* name) const {
        for (const auto& e : dir_) {
            if (e.type != 2) continue;                     // 2 = stream
            if (e.name != name) continue;
            return ReadChain(e.start, e.size, e.size < miniCutoff_);
        }
        return {};
    }

    const std::vector<uint8_t>& Raw() const { return data_; }

    // 所有流名（调试用）
    std::vector<std::string> StreamNames() const {
        std::vector<std::string> v;
        for (const auto& e : dir_) if (e.type == 2) v.push_back(e.name);
        return v;
    }

  private:
    struct Entry {
        std::string name;
        int         type = 0;
        uint32_t    start = 0xFFFFFFFE;
        uint64_t    size = 0;
    };

    bool Parse() {
        if (data_.size() < 512) return false;
        static const uint8_t sig[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        if (memcmp(data_.data(), sig, 8) != 0) return false;

        const uint32_t sectShift = Rd16(data_, 0x1E);
        const uint32_t miniShift = Rd16(data_, 0x20);
        if (sectShift < 7 || sectShift > 16) return false;
        sectSize_ = 1u << sectShift;
        miniSize_ = 1u << miniShift;
        miniCutoff_ = Rd32(data_, 0x38);

        const uint32_t numFat = Rd32(data_, 0x2C);
        const uint32_t dirStart = Rd32(data_, 0x30);
        uint32_t difatStart = Rd32(data_, 0x44);
        uint32_t numDifat = Rd32(data_, 0x48);

        // DIFAT：头里 109 个 + 扩展
        std::vector<uint32_t> fatSectors;
        for (int i = 0; i < 109; ++i) {
            const uint32_t s = Rd32(data_, 0x4C + i * 4);
            if (s < 0xFFFFFFFA) fatSectors.push_back(s);
        }
        uint32_t guard = 0;
        while (difatStart < 0xFFFFFFFA && guard++ < numDifat + 8) {
            const size_t base = SectOff(difatStart);
            if (base + sectSize_ > data_.size()) break;
            for (uint32_t i = 0; i + 1 < sectSize_ / 4; ++i) {
                const uint32_t s = Rd32(data_, base + i * 4);
                if (s < 0xFFFFFFFA) fatSectors.push_back(s);
            }
            difatStart = Rd32(data_, base + (sectSize_ / 4 - 1) * 4);
        }
        if (fatSectors.size() < numFat) fatSectors.resize(numFat);

        // FAT
        fat_.clear();
        for (uint32_t s : fatSectors) {
            const size_t base = SectOff(s);
            if (base + sectSize_ > data_.size()) continue;
            for (uint32_t i = 0; i < sectSize_ / 4; ++i) fat_.push_back(Rd32(data_, base + i * 4));
        }
        if (fat_.empty()) return false;

        // 目录
        const std::vector<uint8_t> dirData = ReadChain(dirStart, (uint64_t)-1, false);
        for (size_t o = 0; o + 128 <= dirData.size(); o += 128) {
            Entry e;
            const uint16_t nameLen = (uint16_t)(dirData[o + 64] | (dirData[o + 65] << 8));
            if (nameLen >= 2 && nameLen <= 64) {
                for (uint16_t k = 0; k + 1 < nameLen; k += 2) {
                    const unsigned ch = (unsigned)(dirData[o + k] | (dirData[o + k + 1] << 8));
                    if (ch == 0) break;
                    if (ch < 0x80) e.name.push_back((char)ch);
                    else AppendUtf8(e.name, ch);
                }
            }
            e.type = dirData[o + 66];
            e.start = Rd32(dirData, o + 116);
            e.size = Rd32(dirData, o + 120);
            if (e.type == 5) { rootStart_ = e.start; rootSize_ = e.size; }
            if (e.type != 0) dir_.push_back(e);
        }

        // mini 流（小流都放在根项的流里，按 64 字节一块切）
        if (rootStart_ < 0xFFFFFFFA) {
            miniStream_ = ReadChain(rootStart_, rootSize_, false);
            const std::vector<uint8_t> mf = ReadChain(Rd32(data_, 0x3C), (uint64_t)-1, false);
            miniFat_.clear();
            for (size_t i = 0; i + 4 <= mf.size(); i += 4) miniFat_.push_back(Rd32(mf, i));
        }
        return !dir_.empty();
    }

    size_t SectOff(uint32_t sect) const { return 512 + (size_t)sect * sectSize_; }

    std::vector<uint8_t> ReadChain(uint32_t start, uint64_t want, bool mini) const {
        std::vector<uint8_t> out;
        const auto& table = mini ? miniFat_ : fat_;
        const auto& src = mini ? miniStream_ : data_;
        const size_t unit = mini ? miniSize_ : sectSize_;
        uint32_t cur = start;
        size_t guard = 0;
        while (cur < 0xFFFFFFFA && guard++ < 200000) {
            const size_t base = mini ? (size_t)cur * unit : SectOff(cur);
            if (base + unit > src.size()) break;
            const size_t take = (want == (uint64_t)-1) ? unit
                              : (size_t)std::min<uint64_t>(unit, want - out.size());
            out.insert(out.end(), src.begin() + base, src.begin() + base + take);
            if (want != (uint64_t)-1 && out.size() >= want) break;
            if (cur >= table.size()) break;
            cur = table[cur];
        }
        if (want != (uint64_t)-1 && out.size() > want) out.resize((size_t)want);
        return out;
    }

    std::vector<uint8_t>  data_;
    std::vector<uint32_t> fat_, miniFat_;
    std::vector<uint8_t>  miniStream_;
    std::vector<Entry>    dir_;
    uint32_t sectSize_ = 512, miniSize_ = 64, miniCutoff_ = 4096;
    uint32_t rootStart_ = 0xFFFFFFFE;
    uint64_t rootSize_ = 0;
};

void AppendCapped(std::string& out, const std::string& s) {
    if (s.empty() || out.size() >= kCap) return;
    const size_t room = kCap - out.size();
    out.append(s, 0, std::min(room, s.size()));
}

// ---------------- .doc ----------------

std::string DocText(Cfb& cfb) {
    const std::vector<uint8_t> wd = cfb.Stream("WordDocument");
    if (wd.size() < 0x200) return {};
    // FIB：fcMin @0x18, fcMac @0x1C（Word 97+）
    const uint32_t fcMin = Rd32(wd, 0x18);
    const uint32_t fcMac = Rd32(wd, 0x1C);
    std::string out;
    if (fcMin >= 0x200 && fcMac > fcMin && fcMac <= wd.size()) {
        // 判断是 Unicode 还是单字节：看有没有大量 0x00 落在奇数位
        size_t zeros = 0, total = 0;
        for (size_t i = fcMin + 1; i < fcMac; i += 2) {
            ++total;
            if (wd[i] == 0) ++zeros;
        }
        const bool utf16 = total > 0 && zeros * 2 > total;
        AppendCapped(out, DecodeRun(wd, fcMin, fcMac, utf16));
    }
    if (out.size() < 32) {
        // 兜底：整条流都扫一遍（快存/分片文件的正文字段不一定连续）
        AppendCapped(out, DecodeRun(wd, 0, std::min<size_t>(wd.size(), 4u << 20), true));
        AppendCapped(out, DecodeRun(wd, 0, std::min<size_t>(wd.size(), 4u << 20), false));
    }
    return out;
}

// ---------------- .xls（BIFF8）----------------

std::string XlsText(Cfb& cfb) {
    std::vector<uint8_t> wb = cfb.Stream("Workbook");
    if (wb.empty()) wb = cfb.Stream("Book");     // BIFF5
    if (wb.empty()) return {};

    std::string out;
    // 逐条记录扫描：type(2) len(2) payload
    // SST(0x00FC) 里是共享字符串；LABEL(0x0204)/RSTRING(0x00D6) 是内联字符串
    std::vector<uint8_t> sst;
    for (size_t o = 0; o + 4 <= wb.size() && out.size() < kCap;) {
        const uint16_t type = (uint16_t)(wb[o] | (wb[o + 1] << 8));
        const uint16_t len = (uint16_t)(wb[o + 2] | (wb[o + 3] << 8));
        const size_t payload = o + 4;
        if (payload + len > wb.size()) break;
        if (type == 0x00FC) {
            sst.insert(sst.end(), wb.begin() + payload, wb.begin() + payload + len);
        } else if (type == 0x0204 || type == 0x00D6) {
            // LABEL: rw(2) col(2) xf(2) len(2) grbit(1) chars
            if (len > 9) {
                const uint16_t clen = (uint16_t)(wb[payload + 6] | (wb[payload + 7] << 8));
                const bool wide = (wb[payload + 8] & 1) != 0;
                const size_t start = payload + 9;
                const size_t bytes = wide ? (size_t)clen * 2 : clen;
                if (start + bytes <= payload + len) {
                    std::vector<uint8_t> tmp(wb.begin() + start, wb.begin() + start + bytes);
                    AppendCapped(out, DecodeRun(tmp, 0, tmp.size(), wide));
                }
            }
        } else if (type == 0x003C) {   // CONTINUE，紧跟在 SST 后面
            if (!sst.empty()) sst.insert(sst.end(), wb.begin() + payload, wb.begin() + payload + len);
        }
        o = payload + len;
    }

    // 解析 SST：cstTotal(4) cstUnique(4)，然后是一串 XLUnicodeRichExtendedString
    if (sst.size() > 8) {
        size_t o = 8;
        while (o + 3 <= sst.size() && out.size() < kCap) {
            const uint16_t cch = (uint16_t)(sst[o] | (sst[o + 1] << 8));
            const uint8_t flags = sst[o + 2];
            const bool wide = (flags & 1) != 0;
            const bool ext = (flags & 4) != 0;
            const bool rich = (flags & 8) != 0;
            o += 3;
            uint16_t cRun = 0, cbExt = 0;
            if (rich && o + 2 <= sst.size()) { cRun = (uint16_t)(sst[o] | (sst[o + 1] << 8)); o += 2; }
            if (ext && o + 4 <= sst.size()) { cbExt = (uint16_t)(sst[o] | (sst[o + 1] << 8)); o += 4; }
            const size_t bytes = wide ? (size_t)cch * 2 : cch;
            if (o + bytes > sst.size()) break;
            std::vector<uint8_t> tmp(sst.begin() + o, sst.begin() + o + bytes);
            AppendCapped(out, DecodeRun(tmp, 0, tmp.size(), wide));
            o += bytes + (size_t)cRun * 4 + cbExt;
        }
    }
    return out;
}

// ---------------- .ppt ----------------

// 只收"像单词"的 ASCII 串：长度够、字母占比高。避免前面 64 KB 被噪声占满。
std::string ScanAsciiWords(const std::vector<uint8_t>& b, size_t limit) {
    std::string out, cur;
    if (limit > b.size()) limit = b.size();
    for (size_t i = 0; i < limit; ++i) {
        const unsigned c = b[i];
        if (c >= 0x20 && c <= 0x7E) {
            cur.push_back((char)c);
            continue;
        }
        if (cur.size() >= 8) {
            size_t letters = 0;
            for (char ch : cur)
                if (isalpha((unsigned char)ch) || ch == ' ') ++letters;
            if (letters * 4 >= cur.size() * 3) {
                out += cur;
                out += '\n';
                if (out.size() >= kCap) return out;
            }
        }
        cur.clear();
    }
    return out;
}

std::string PptTextRaw(const std::vector<uint8_t>& raw) {
    // 直接扫整个文件：.ppt 的文本原子散落在扇区里，按流读容易漏（实测丢整页正文）。
    // 两个方向都扫：单字节（英文） + UTF-16（中文，带假汉字过滤）。
    std::string out;
    const size_t lim = std::min<size_t>(raw.size(), 32u << 20);

    // 一：直接在原始字节里找文本原子（实测按 CFB 流读会漏掉整页正文）
    for (size_t o = 0; o + 8 < lim && out.size() < kCap; ++o) {
        const uint16_t type = (uint16_t)(raw[o + 2] | (raw[o + 3] << 8));
        if (type != 0x0FA0 && type != 0x0FA8) continue;
        const uint32_t len = (uint32_t)(raw[o + 4] | (raw[o + 5] << 8) | (raw[o + 6] << 16) |
                                        ((uint32_t)raw[o + 7] << 24));
        if (len == 0 || len > (16u << 10) || o + 8 + len > lim) continue;
        std::vector<uint8_t> tmp(raw.begin() + o + 8, raw.begin() + o + 8 + len);
        const std::string t = DecodeRun(tmp, 0, tmp.size(), type == 0x0FA0);
        if (!t.empty()) AppendCapped(out, t);   // 不跳：错误的 len 会吞掉后面的真原子
    }

    // 二：兜底扫"像单词"的 ASCII 串（有些版本文本不在原子里）
    if (out.size() < 256) AppendCapped(out, ScanAsciiWords(raw, lim));
    return out;
}

}  // namespace

bool Handles(const std::string& ext) {
    return ext == ".doc" || ext == ".xls" || ext == ".ppt";
}

// 直接读整个文件（.ppt 不依赖 CFB 解析，大文件的扇区链容易读错）
bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out) {
    const std::wstring w = extract::ToWide(path);
    HANDLE h = CreateFileW(w.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > (400ll << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    size_t off = 0;
    while (off < out.size()) {
        DWORD got = 0;
        const DWORD want = (DWORD)std::min<size_t>(out.size() - off, 1u << 20);
        if (!ReadFile(h, out.data() + off, want, &got, nullptr) || got == 0) break;
        off += got;
    }
    CloseHandle(h);
    out.resize(off);
    return !out.empty();
}

std::string Text(const std::string& path, const std::string& ext) {
    // .ppt 走整文件扫描，不依赖 CFB（实测 5 MB 的 .ppt 扇区链读不对）
    if (ext == ".ppt") {
        std::vector<uint8_t> raw;
        if (!ReadWholeFile(path, raw)) return {};
        return PptTextRaw(raw);
    }

    Cfb cfb;
    if (!cfb.Open(path)) return {};
    if (std::getenv("SPACESIFT_DEBUG_CFB")) {
        for (const auto& n : cfb.StreamNames()) {
            const std::vector<uint8_t> b = cfb.Stream(n.c_str());
            fprintf(stderr, "[cfb] %-40s %zu\n", n.c_str(), b.size());
        }
    }
    if (ext == ".doc") return DocText(cfb);
    if (ext == ".xls") return XlsText(cfb);
    return {};
}

}  // namespace office
