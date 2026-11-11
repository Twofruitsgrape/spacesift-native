// 无界面测试：验证 extract 抽取 + content 索引/检索。
// 构建：见 tests/build_test.bat
#include <windows.h>

#include <cstdio>
#include <string>

#include "content.h"
#include "extract.h"
#include "sqlite3.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

static void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
}

static std::string Gbk(const char* utf8) {
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    int m = WideCharToMultiByte(936, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(936, 0, w.c_str(), -1, &s[0], m, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

int main() {
    printf("=== extract 单元测试 ===\n");

    // 白名单
    Check(extract::Supports(".txt"), "支持 .txt");
    Check(extract::Supports(".dxf"), "支持 .dxf");
    Check(extract::Supports(".step") && extract::Supports(".stp"), "支持 .step/.stp");
    Check(!extract::Supports(".dwg"), ".dwg 暂不支持（P4）");
    Check(!extract::Supports(".pdf"), ".pdf 暂不支持（P3）");
    Check(extract::LowerExt("C:\\A\\B.STEP") == ".step", "扩展名转小写");

    // 造一个测试目录
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"spacesift_ct";
    CreateDirectoryW(dir.c_str(), nullptr);

    // 1) UTF-8 文本
    WriteBytes(dir + L"\\utf8.txt", "第一行 关键词甲\n第二行\n");
    // 2) GBK 文本（中文 Windows 上的老文件）
    WriteBytes(dir + L"\\gbk.txt", Gbk("这是GBK编码 关键词乙 内容\n"));
    // 3) DXF：文字在 MTEXT 和属性块 ATTRIB 里，坐标不该被当成文字
    WriteBytes(dir + L"\\d.dxf",
               "0\nSECTION\n2\nHEADER\n0\nENDSEC\n0\nSECTION\n2\nENTITIES\n"
               "0\nMTEXT\n8\n图层A\n10\n123.456\n20\n-99.1\n1\n图纸标题栏 关键词丙\n"
               "0\nTEXT\n8\n0\n10\n0.0\n20\n0.0\n1\n单行文字\n"
               "0\nINSERT\n2\n螺栓M8\n10\n5.0\n"
               "0\nATTRIB\n1\n法兰盘3寸\n2\n零件名\n"
               "0\nENDSEC\n0\nEOF\n");
    // 4) STEP：名字在单引号里，坐标是数字
    WriteBytes(dir + L"\\p.step",
               "ISO-10303-21;\nHEADER;\nFILE_NAME('装配总成.step','',(''),(''),'','','');\n"
               "ENDSEC;\nDATA;\n"
               "#1 = CARTESIAN_POINT('',(0.,0.,0.));\n"
               "#42 = PRODUCT('法兰盘','法兰盘','',(#43));\n"
               "ENDSEC;\nEND-ISO-10303-21;\n");
    // 5) OBJ：只有命名行有意义
    WriteBytes(dir + L"\\o.obj", "# comment 关键词丁\ng 支架组\nv 1.0 2.0 3.0\nusemtl 材料A\n");

    std::string dirU8 = extract::FromWide(dir);
    printf("\n--- 抽取结果 ---\n");
    struct { const char* f; const char* must; } cases[] = {
        {"\\utf8.txt", "关键词甲"},
        {"\\gbk.txt",  "关键词乙"},
        {"\\d.dxf",    "关键词丙"},
        {"\\d.dxf",    "法兰盘3寸"},
        {"\\d.dxf",    "螺栓M8"},
        {"\\p.step",   "法兰盘"},
        {"\\p.step",   "装配总成"},
        {"\\o.obj",    "支架组"},
    };
    for (auto& c : cases) {
        std::string p = dirU8 + c.f;
        std::string t = extract::Text(p, extract::LowerExt(p));
        char label[160];
        snprintf(label, sizeof label, "%s 抽到「%s」", c.f, c.must);
        Check(Contains(t, c.must), label);
        if (!Contains(t, c.must)) printf("      实际文本: %.200s\n", t.c_str());
    }
    // 坐标不该进正文（DXF 只收 code 1/2/3）
    {
        std::string t = extract::Text(dirU8 + "\\d.dxf", ".dxf");
        Check(!Contains(t, "123.456"), "DXF 坐标未被当成正文");
    }

    // ---------------- 内容索引 ----------------
    printf("\n=== content 索引测试 ===\n");
    std::atomic<bool> cancel{false};
    content::Build(dirU8, cancel);
    printf("已扫描 %d · 已写入 %d\n", content::g_seen.load(), content::g_parsed.load());
    Check(content::HasIndex(dirU8), "索引库已生成");
    Check(content::g_parsed.load() >= 5, "至少写入 5 个文件");

    struct { const char* q; const char* wantName; } queries[] = {
        {"关键词甲", "utf8.txt"},
        {"关键词乙", "gbk.txt"},
        {"关键词丙", "d.dxf"},
        {"法兰盘3寸", "d.dxf"},     // 属性块里的值
        {"装配总成", "p.step"},     // FILE_NAME
        {"支架组", "o.obj"},
    };
    for (auto& q : queries) {
        auto hits = content::Search(dirU8, q.q, 100);
        bool got = false;
        for (auto& h : hits) if (Contains(h.path, q.wantName)) got = true;
        char label[160];
        snprintf(label, sizeof label, "搜「%s」命中 %s（%d 条）", q.q, q.wantName, (int)hits.size());
        Check(got, label);
        if (!hits.empty()) printf("      片段: %s\n", hits[0].snippet.c_str());
    }
    // 不该命中的
    {
        auto hits = content::Search(dirU8, "完全不存在的词", 100);
        Check(hits.empty(), "无关词零命中");
    }
    // 增量：第二次应跳过全部
    content::ResetProgress();
    content::Build(dirU8, cancel);
    Check(content::g_parsed.load() == 0, "增量：文件没变则不重复抽取");

    printf("\n%s（失败 %d 项）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
