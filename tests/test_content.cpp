// 无界面测试：验证 extract 抽取 + content 索引/检索。
// 构建：见 tests/build_test.bat
#include <windows.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "content.h"
#include "external.h"
#include "extract.h"
#include "miniz.h"
#include "sqlite3.h"

static int g_fail = 0;

static void Check(bool ok, const char* what) {
    printf("%s  %s\n", ok ? "[PASS]" : "[FAIL]", what);
    if (!ok) ++g_fail;
}

static bool Contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// 手搓一个最小 PDF（单页、Type1 Helvetica、纯 ASCII 文本），xref 偏移算准
static std::string MakePdf(const std::string& text) {
    std::string objs[6];
    objs[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    objs[2] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    objs[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 120] "
              "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>";
    const std::string stream = "BT /F1 20 Tf 20 60 Td (" + text + ") Tj ET";
    objs[4] = "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" +
              stream + "\nendstream";
    objs[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";

    std::string out = "%PDF-1.4\n";
    std::string off[6];
    for (int i = 1; i <= 5; ++i) {
        char b[32];
        snprintf(b, sizeof b, "%010zu", out.size());
        off[i] = b;
        out += std::to_string(i) + " 0 obj\n" + objs[i] + "\nendobj\n";
    }
    const size_t xrefPos = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) out += off[i] + " 00000 n \n";
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xrefPos) + "\n%%EOF\n";
    return out;
}

static void WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
}

static std::string Gbk(const char* utf8) {    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
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
    Check(extract::Supports(".dwg") == extract::DwgReady(), ".dwg 随转换器可用性启用/禁用");
    // 旧版 Office 有原生解析（office.cpp）+ UTF-16 兜底，不该被外部工具门控
    Check(extract::Supports(".doc") && extract::Supports(".xls") && extract::Supports(".ppt"),
          "旧版 Office 无外部工具也参与索引（原生解析）");
    Check(extract::PdfReady(), "pdfium.dll 已加载（PDF 可检索）");
    Check(extract::Supports(".pdf"), "支持 .pdf");

    // 外部进程层：拿 cmd 的回显跑一遍 RunCapture
    {
        std::string out;
        Check(external::RunCapture(L"C:\\Windows\\System32\\cmd.exe",
                                   L"/c echo probe_from_external", out),
              "external::RunCapture 能跑进程并收回输出");
        Check(Contains(out, "probe_from_external"), "回收的输出内容正确");
    }

    if (extract::DwgReady()) {
        Check(true, "DWG 转换器已就位（dwg2dxf / ODA）");
    } else {
        printf("[SKIP] 未找到 DWG 转换器，跳过 DWG 用例\n");
    }
    Check(extract::Supports(".png") == extract::OcrReady(), ".png/.jpg 随 tesseract 可用性启用");
    Check(extract::Supports(".sldprt") && extract::Supports(".sldasm"), "支持 .sldprt/.sldasm（openswx）");
    if (extract::OcrReady()) printf("[INFO] tesseract 已就位，扫描件/截图 OCR 可用\n");
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
               // STEP 把非 ASCII 写成 \X2\<UTF-16 hex>\X0\，不还原就搜不到中文
               "#43 = PRODUCT('\\X2\\52A070ED9488\\X0\\','x','',(#44));\n"
               "ENDSEC;\nEND-ISO-10303-21;\n");
    // 7) P4：DWG（靠外部 dwg2dxf.exe）——没转换器就跳过这一段
    const bool dwg = extract::DwgReady();
    if (dwg) {
        std::string src = "C:\\Users\\ALICE\\AppData\\Local\\Temp\\libredwg-0.14\\test\\test-data\\example_2000.dwg";
        std::string bytes;
        if (external::ReadFileBytes(extract::ToWide(src), bytes, 8u * 1024 * 1024))
            WriteBytes(dir + L"\\e.dwg", bytes);
    }

    // 5) OBJ：只有命名行有意义
    WriteBytes(dir + L"\\o.obj", "# comment 关键词丁\ng 支架组\nv 1.0 2.0 3.0\nusemtl 材料A\n");

    // 6) P2：OOXML 与压缩包
    auto makeZip = [](const std::vector<std::pair<std::string, std::string>>& entries) {
        mz_zip_archive w;
        memset(&w, 0, sizeof w);
        std::string out;
        if (!mz_zip_writer_init_heap(&w, 0, 0)) return out;
        for (auto& e : entries)
            mz_zip_writer_add_mem(&w, e.first.c_str(), e.second.data(), e.second.size(),
                                  MZ_BEST_COMPRESSION);
        void* buf = nullptr;
        size_t sz = 0;
        if (mz_zip_writer_finalize_heap_archive(&w, &buf, &sz) && buf)
            out.assign((const char*)buf, sz);
        mz_zip_writer_end(&w);
        return out;
    };
    WriteBytes(dir + L"\\c.docx", makeZip({{"word/document.xml",
        "<?xml version=\"1.0\"?><w:document><w:body><w:p><w:r><w:t>合同正文关键词戊</w:t></w:r>"
        "<w:r><w:t>&#x4E2D;&amp;</w:t></w:r></w:p></w:body></w:document>"}}));
    WriteBytes(dir + L"\\s.xlsx", makeZip({{"xl/sharedStrings.xml",
        "<sst><si><t>物料清单关键词己</t></si></sst>"}}));
    WriteBytes(dir + L"\\s.pptx", makeZip({{"ppt/slides/slide1.xml",
        "<p:sld><a:t>汇报要点关键词庚</a:t></p:sld>"}}));
    WriteBytes(dir + L"\\a.zip", makeZip({{"内层说明.md", "压缩包内文本关键词辛"},
                                         {"sub/inner.docx", ""}}));
    // 7) P3：PDF 文本层
    WriteBytes(dir + L"\\r.pdf", MakePdf("Hello PDF Keyword Omega"));

    // 9) CAE 输入卡（ABAQUS / ANSYS 都是纯文本）
    WriteBytes(dir + L"\\job.inp",
               "** ABAQUS input deck\n*Heading\n** 三维流固耦合算例 关键词癸\n"
               "*Material, name=STEEL\n*Elastic\n210000., 0.3\n");

    // 10) 旧版 .doc：UTF-16LE 扫描（零依赖，不靠 antiword/catdoc）
    {
        std::string u16;
        const char* cn = "南京航空航天大学 研究生中期评定表";
        // 把 UTF-8 转成 UTF-16LE 字节串，冒充 .doc 的正文区
        int wn = MultiByteToWideChar(CP_UTF8, 0, cn, -1, nullptr, 0);
        std::wstring w((size_t)wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cn, -1, &w[0], wn);
        u16.assign((const char*)w.data(), w.size() * sizeof(wchar_t));
        u16 = std::string("\xd0\xcf\x11\xe0", 4) + std::string(300, '\0') + u16 + std::string(300, '\0');
        WriteBytes(dir + L"\\legacy.doc", u16);
    }

    WriteBytes(dir + L"\\rank_a.txt", "关键词壬 填充填充填充填充填充填充填充填充填充填充");
    WriteBytes(dir + L"\\rank_b.txt",
               "关键词壬 关键词壬 关键词壬 填充填充填充填充填充填充填充填充填充填充");

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
        {"\\p.step",   "加热针"},          // \X2\ 转义的中文
        {"\\o.obj",    "支架组"},
        {"\\c.docx",   "关键词戊"},
        {"\\c.docx",   "中&"},              // 实体解码 \u0026#x4E2D; 和 &amp;
        {"\\s.xlsx",   "关键词己"},
        {"\\s.pptx",   "关键词庚"},
        {"\\a.zip",    "内层说明.md"},      // 压缩包条目名
        {"\\a.zip",    "压缩包内文本关键词辛"},   // 压缩包内文本文件
        {"\\r.pdf",    "Keyword Omega"},      // PDF 文本层
        {"\\e.dwg",    "teksto"},            // DWG（经 dwg2dxf 转 DXF）
        {"\\legacy.doc", "南京航空航天大学"}, // 旧版 .doc 的 UTF-16LE 扫描
        {"\\job.inp",   "关键词癸"},        // ABAQUS/ANSYS 输入卡（纯文本）
    };
    // 没装 DWG 转换器时跳过那一条（按文件名找，不靠数组位置）
    if (!extract::DwgReady())
        for (auto& c : cases)
            if (std::string(c.f) == "\\e.dwg") c.must = "__skip__";
    for (auto& c : cases) {
        if (std::string(c.must) == "__skip__") continue;
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
    Check(content::g_parsed.load() >= 10, "至少写入 10 个文件");

    struct { const char* q; const char* wantName; } queries[] = {
        {"关键词甲", "utf8.txt"},
        {"关键词乙", "gbk.txt"},
        {"关键词丙", "d.dxf"},
        {"法兰盘3寸", "d.dxf"},     // 属性块里的值
        {"装配总成", "p.step"},     // FILE_NAME
        {"加热针", "p.step"},       // STEP \X2\ 转义（加热针）
        {"支架组", "o.obj"},
        {"关键词戊", "c.docx"},     // OOXML 正文
        {"关键词己", "s.xlsx"},     // sharedStrings
        {"关键词庚", "s.pptx"},     // slide
        {"压缩包内文本关键词辛", "a.zip"},   // zip 内文本
        {"Keyword Omega", "r.pdf"},        // PDF
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
    // 高级语法
    {
        auto hits = content::Search(dirU8, "ext:dxf", 50);
        Check(!hits.empty() && Contains(hits[0].path, ".dxf"), "ext:dxf 只返回 dxf");
    }
    {
        auto hits = content::Search(dirU8, "关键词丙 ext:pdf", 50);
        Check(hits.empty(), "关键词 + ext:pdf 过滤后为空");
    }
    // ↓ 肯定用例：之前只写了否定用例，导致「关键词 + 后缀」永远返回 0 的 bug 没被发现
    {
        auto hits = content::Search(dirU8, "关键词丙 ext:dxf", 50);
        bool ok = !hits.empty();
        for (auto& h : hits) if (!Contains(h.path, ".dxf")) ok = false;
        Check(ok, "关键词 + ext:dxf 能命中 d.dxf（肯定用例）");
    }
    {
        auto hits = content::Search(dirU8, "关键词丙 ext:docx", 50);
        Check(hits.empty(), "关键词 + ext:docx 不应命中 dxf");
    }
    {
        auto hits = content::Search(dirU8, "re:关键词[甲乙] ext:txt", 50);
        bool hasA = false, hasB = false;
        for (auto& h : hits) {
            if (Contains(h.path, "utf8.txt")) hasA = true;
            if (Contains(h.path, "gbk.txt")) hasB = true;
        }
        Check(hasA && hasB, "正则 + ext:txt 过滤生效");
    }
    {
        auto hits = content::Search(dirU8, "关键词丙 ext:dxf size:>10", 50);
        bool ok = !hits.empty();
        for (auto& h : hits) if (h.size <= 10) ok = false;
        Check(ok, "关键词 + ext + size 三个条件同时生效");
    }
    {
        auto hits = content::Search(dirU8, "关键词丙 单行文字", 50);
        Check(!hits.empty() && Contains(hits[0].path, "d.dxf"), "空格分隔 = AND");
    }
    {
        auto hits = content::Search(dirU8, "关键词丙 -单行文字", 50);
        Check(hits.empty(), "-词 能排除");
    }
    {
        auto hits = content::Search(dirU8, "关键词甲 OR 关键词乙", 50);
        Check(hits.size() >= 2, "OR 能取并集");
    }
    {
        auto hits = content::Search(dirU8, "size:>50 ext:txt", 50);
        bool ok = !hits.empty();
        for (auto& h : hits) if (h.size <= 50) ok = false;
        Check(ok, "size:>50 过滤生效");
    }

    // 正则
    {
        auto hits = content::Search(dirU8, "re:关键词[甲乙]", 50);
        bool hasA = false, hasB = false;
        for (auto& h : hits) {
            if (Contains(h.path, "utf8.txt")) hasA = true;
            if (Contains(h.path, "gbk.txt")) hasB = true;
        }
        Check(hasA && hasB, "re: 正则能同时命中两个文件");
    }
    {
        auto hits = content::Search(dirU8, "/加热针/", 50);
        Check(!hits.empty(), "/.../ 也能当正则用");
    }

    // 相关度排序：命中次数多的在前
    {
        auto hits = content::Search(dirU8, "关键词壬", 10);
        Check(!hits.empty() && Contains(hits[0].path, "rank_b"),
              "排序：出现 3 次的 rank_b 排在只出现 1 次的 rank_a 前面");
        if (hits.size() >= 2) printf("      前两名: %s / %s\n", hits[0].name.c_str(), hits[1].name.c_str());
    }
    // 增量：第二次应跳过全部
    content::ResetProgress();
    content::Build(dirU8, cancel);
    Check(content::g_parsed.load() == 0, "增量：文件没变则不重复抽取");

    printf("\n%s（失败 %d 项）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
