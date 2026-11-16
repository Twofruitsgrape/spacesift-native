// 回归：MAT 压缩块对齐、删除清理、STL 头、UTF-16BE、EML 编码主题/短 base64、
// .msg 分流、STEP 头部挤占、CJK unigram / 拉丁前缀查询。
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include "content.h"
#include "extract.h"
#include "miniz.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static bool Contains(const std::string& h, const std::string& n) {
    return h.find(n) != std::string::npos;
}

static void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
}

static void U32(std::string& o, uint32_t v) {
    for (int i = 0; i < 4; ++i) o.push_back((char)(v >> (8 * i)));
}

static std::string Elem(uint32_t type, const std::string& data) {
    std::string o;
    U32(o, type);
    U32(o, (uint32_t)data.size());
    o += data;
    while (o.size() % 8) o.push_back('\0');
    return o;
}

static std::string Matrix(const std::string& name, const std::string& value) {
    std::string flags(8, '\0');
    flags[0] = 4;                       // mxCHAR
    std::string dims(8, '\0');
    dims[0] = 1;
    U32(dims, 1);
    std::string payload;
    payload += Elem(6, flags);
    payload += Elem(5, dims);
    payload += Elem(1, name);
    payload += Elem(16, value);         // miUTF8
    return Elem(14, payload);
}

static std::string Compress(const std::string& data) {
    mz_ulong cap = mz_compressBound(data.size());
    std::string out((size_t)cap, '\0');
    mz_ulong len = cap;
    if (mz_compress((unsigned char*)&out[0], &len, (const unsigned char*)data.data(),
                    data.size()) != MZ_OK)
        return {};
    out.resize((size_t)len);
    return out;
}

// 两个连续 miCOMPRESSED 元素，中间不补齐 8 字节（MAT v5 的实际写法）
static std::string MakeMat() {
    std::string o(128, '\0');
    const char* hdr = "MATLAB 5.0 MAT-file, Platform: PCWIN64, Created by regression";
    memcpy(&o[0], hdr, strlen(hdr));
    o[124] = 0; o[125] = 1; o[126] = 'I'; o[127] = 'M';
    const char* names[] = {"var_one", "var_two"};
    const char* vals[] = {"字符串甲", "关键词乙"};
    for (int i = 0; i < 2; ++i) {
        const std::string comp = Compress(Matrix(names[i], vals[i]));
        U32(o, 15);                     // miCOMPRESSED
        U32(o, (uint32_t)comp.size());
        o += comp;                      // 注意：不补 padding
    }
    return o;
}

static std::string Utf16Be(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    std::string o("\xFE\xFF", 2);
    for (wchar_t c : w) {
        o.push_back((char)(c >> 8));
        o.push_back((char)(c & 0xFF));
    }
    return o;
}

int main() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring dir = std::wstring(tmp) + L"spacesift_reg_" +
                             std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(dir.c_str(), nullptr);

    // 1) MAT 两个压缩块
    WriteBytes(dir + L"\\two.mat", MakeMat());
    {
        const std::string t = extract::Text(extract::FromWide(dir) + "\\two.mat", ".mat");
        Check(Contains(t, "var_one") && Contains(t, "var_two") && Contains(t, "关键词乙"),
              "MAT：两个 miCOMPRESSED 块的变量名都能抽出");
    }

    // 2) 二进制 STL 的 solid 头
    {
        std::string stl = "solid PART-123 底盖";
        stl.resize(80, ' ');
        stl += std::string(200, '\x01');
        WriteBytes(dir + L"\\part.stl", stl);
        const std::string t = extract::Text(extract::FromWide(dir) + "\\part.stl", ".stl");
        Check(Contains(t, "PART-123"), "STL：二进制体的 solid 头零件名能抽出");
    }

    // 3) UTF-16BE
    WriteBytes(dir + L"\\be.txt", Utf16Be("BE关键词丙"));
    Check(Contains(extract::Text(extract::FromWide(dir) + "\\be.txt", ".txt"), "BE关键词丙"),
          "UTF-16BE 文本能解码");

    // 4) .msg：ABAQUS ASCII 与 Outlook/UTF-16 都要能读
    WriteBytes(dir + L"\\abaqus.msg", "ABAQUS MSG 关键词丁\nSTEP COMPLETED\n");
    Check(Contains(extract::Text(extract::FromWide(dir) + "\\abaqus.msg", ".msg"),
                   "ABAQUS MSG 关键词丁"),
          ".msg：ABAQUS ASCII 不被邮件分支吃掉");
    {
        const char* cn = "邮件主题关键词戊";
        int n = MultiByteToWideChar(CP_UTF8, 0, cn, -1, nullptr, 0);
        std::wstring w((size_t)n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cn, -1, &w[0], n);
        std::string u16;
        for (wchar_t c : w) { u16.push_back((char)(c & 0xFF)); u16.push_back((char)(c >> 8)); }
        WriteBytes(dir + L"\\utf16.msg", u16);
        Check(Contains(extract::Text(extract::FromWide(dir) + "\\utf16.msg", ".msg"),
                       "邮件主题关键词戊"),
              ".msg：无 BOM UTF-16 兜底仍可用");
    }

    // 5) EML 编码主题 + 短 base64 正文
    WriteBytes(dir + L"\\subject.eml",
               "Subject: =?utf-8?B?RU1M5Li76aKY5oiK?=\r\n"
               "Content-Type: text/plain; charset=utf-8\r\n\r\nbody\r\n");
    Check(Contains(extract::Text(extract::FromWide(dir) + "\\subject.eml", ".eml"),
                   "EML主题戊"),
          "EML：RFC 2047 编码主题能解码");
    WriteBytes(dir + L"\\shortb64.eml",
               "Subject: b64\r\nContent-Type: text/plain; charset=utf-8\r\n"
               "Content-Transfer-Encoding: base64\r\n\r\n"
               "RU1MIGJhc2U2NCDmraPmloflhbPplK7or41C5LmZCg==\r\n");
    Check(Contains(extract::Text(extract::FromWide(dir) + "\\shortb64.eml", ".eml"),
                   "EML base64 正文关键词B乙"),
          "EML：短 base64 正文能解码");

    // 6) STEP 头部 60KB+ NONE 后的 PRODUCT
    {
        std::string step = "ISO-10303-21;\nHEADER;\n";
        for (int i = 0; i < 2500; ++i) step += "FILE_DESCRIPTION(('NONE'),'2;1');\n";
        step += "ENDSEC;\nDATA;\n#1=PRODUCT('STEP尾部关键词己','x','',(#2));\n"
                "ENDSEC;\nEND-ISO-10303-21;\n";
        WriteBytes(dir + L"\\tail.step", step);
        Check(Contains(extract::Text(extract::FromWide(dir) + "\\tail.step", ".step"),
                       "STEP尾部关键词己"),
              "STEP：尾部 PRODUCT 不被头部 NONE 挤掉");
    }

    // 7) 检索：CJK unigram + 拉丁前缀 + 删除清理
    const std::wstring sdir = dir + L"\\search";
    CreateDirectoryW(sdir.c_str(), nullptr);
    WriteBytes(sdir + L"\\s.txt", "法兰盘3寸 alpha engineering\n");
    const std::string sdirU8 = extract::FromWide(sdir);
    std::atomic<bool> cancel{false};
    content::Build(sdirU8, cancel);
    Check(!content::Search(sdirU8, "盘3", 20).empty(), "检索：混合查询「盘3」命中");
    Check(!content::Search(sdirU8, "alph", 20).empty(), "检索：拉丁前缀「alph」命中 alpha");
    Check(!content::Search(sdirU8, "engineer", 20).empty(),
          "检索：拉丁前缀「engineer」命中 engineering");

    WriteBytes(sdir + L"\\del.txt", "删除关键词庚\n");
    content::Build(sdirU8, cancel);
    Check(!content::Search(sdirU8, "删除关键词庚", 20).empty(), "删除前能搜到");
    DeleteFileW((sdir + L"\\del.txt").c_str());
    content::Build(sdirU8, cancel);
    Check(content::Search(sdirU8, "删除关键词庚", 20).empty(), "删除文件后索引行被清理");
    Check(!content::Search(sdirU8, "盘3", 20).empty(), "清理不影响其它文件");

    content::DeleteIndex(sdirU8);
    DeleteFileW((sdir + L"\\s.txt").c_str());
    RemoveDirectoryW(sdir.c_str());
    DeleteFileW((dir + L"\\two.mat").c_str());
    DeleteFileW((dir + L"\\part.stl").c_str());
    DeleteFileW((dir + L"\\be.txt").c_str());
    DeleteFileW((dir + L"\\abaqus.msg").c_str());
    DeleteFileW((dir + L"\\utf16.msg").c_str());
    DeleteFileW((dir + L"\\subject.eml").c_str());
    DeleteFileW((dir + L"\\shortb64.eml").c_str());
    DeleteFileW((dir + L"\\tail.step").c_str());
    RemoveDirectoryW(dir.c_str());

    printf("\n%s（失败 %d 项）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
