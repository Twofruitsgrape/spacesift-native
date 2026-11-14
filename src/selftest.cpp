#include "selftest.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "content.h"
#include "db.h"
#include "extract.h"
#include "miniz.h"
#include "version.h"

namespace selftest {
namespace {

int g_pass = 0;
int g_fail = 0;
int g_skip = 0;
FILE* g_log = nullptr;

void Out(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    if (g_log) fputs(buf, g_log);
    fflush(stdout);
}

void Check(bool ok, const char* what) {
    if (ok) { ++g_pass; Out("  [PASS] %s\n", what); }
    else    { ++g_fail; Out("  [FAIL] %s\n", what); }
}

void Skip(const char* what, const char* why) {
    ++g_skip;
    Out("  [SKIP] %s —— %s\n", what, why);
}

std::string U8(const std::wstring& w) { return extract::FromWide(w); }

bool WriteBytes(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const BOOL ok = WriteFile(h, data.data(), (DWORD)data.size(), &wrote, nullptr);
    CloseHandle(h);
    return ok && wrote == (DWORD)data.size();
}

std::string Gbk(const char* utf8) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &w[0], n);
    const int m = WideCharToMultiByte(936, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)m, '\0');
    WideCharToMultiByte(936, 0, w.c_str(), -1, &s[0], m, nullptr, nullptr);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

// 最小可用 PDF（单页 Helvetica，纯 ASCII 文本），xref 偏移算准
std::string MakePdf(const std::string& text) {
    std::string o[6];
    o[1] = "<< /Type /Catalog /Pages 2 0 R >>";
    o[2] = "<< /Type /Pages /Kids [3 0 R] /Count 1 >>";
    o[3] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 400 120] "
           "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>";
    const std::string st = "BT /F1 20 Tf 20 60 Td (" + text + ") Tj ET";
    o[4] = "<< /Length " + std::to_string(st.size()) + " >>\nstream\n" + st + "\nendstream";
    o[5] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";

    std::string out = "%PDF-1.4\n";
    std::string off[6];
    for (int i = 1; i <= 5; ++i) {
        char b[32];
        snprintf(b, sizeof b, "%010zu", out.size());
        off[i] = b;
        out += std::to_string(i) + " 0 obj\n" + o[i] + "\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 6\n0000000000 65535 f \n";
    for (int i = 1; i <= 5; ++i) out += off[i] + " 00000 n \n";
    out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" +
           std::to_string(xref) + "\n%%EOF\n";
    return out;
}

std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& entries) {
    mz_zip_archive w;
    memset(&w, 0, sizeof w);
    std::string out;
    if (!mz_zip_writer_init_heap(&w, 0, 0)) return out;
    for (const auto& e : entries)
        mz_zip_writer_add_mem(&w, e.first.c_str(), e.second.data(), e.second.size(),
                              MZ_BEST_COMPRESSION);
    void* buf = nullptr;
    size_t sz = 0;
    if (mz_zip_writer_finalize_heap_archive(&w, &buf, &sz) && buf) out.assign((char*)buf, sz);
    mz_zip_writer_end(&w);
    return out;
}

bool Has(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

void Section(const char* name) { Out("\n%s\n", name); }

// 文件名索引（扫描时建的那份）单独测：直接走 db:: 的写入/读取路径，
// 不依赖界面线程，这样"下载后能不能按文件名搜"是可验的。
bool NameIndexRoundTrip(const std::wstring& dir, std::string* err) {
    const std::string root = U8(dir);
    const std::string dbPath = db::IndexPathFor(root);

    if (!db::Open(dbPath)) { *err = "db::Open 失败"; return false; }
    db::Add(0, root, 0, true);          // 根节点：名字就是完整路径
    db::BeginBulk();
    db::Add(1, "报告自检.docx", 1234, false);
    db::Add(1, "预算表自检.xlsx", 4321, false);
    db::FlushBulk();
    if (!db::EndBulk(root, false)) { *err = "EndBulk 失败"; return false; }

    bool ok = false;
    if (db::OpenExisting(dbPath)) {
        auto hits = db::Search("报告自检", 10);
        ok = !hits.empty() && Has(hits[0].path, "报告自检.docx");
        if (!ok) *err = "Search 没命中刚写入的文件名";
        auto all = db::SearchAll("预算表自检", 10);
        if (ok && all.empty()) { ok = false; *err = "SearchAll 没命中"; }
        db::Close();
    } else {
        *err = "OpenExisting 失败";
    }

    for (const std::wstring& p : {extract::ToWide(dbPath), extract::ToWide(dbPath + "-wal"),
                                  extract::ToWide(dbPath + "-shm")})
        DeleteFileW(p.c_str());
    return ok;
}

}  // namespace

int Run(const wchar_t* folder) {
    // -mwindows 编译出来没有控制台：要么挂到调用方的，要么自己开一个
    bool ownConsole = false;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        if (AllocConsole()) ownConsole = true;
    }
    SetConsoleOutputCP(CP_UTF8);
    if (ownConsole) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }

    wchar_t tempRoot[MAX_PATH] = {0};
    GetTempPathW(MAX_PATH, tempRoot);
    const std::wstring work = std::wstring(tempRoot) + L"SpaceSift-selftest-" +
                              std::to_wstring(GetCurrentProcessId());
    const std::wstring logPath = work + L".log";
    g_log = _wfopen(logPath.c_str(), L"w, ccs=UTF-8");

    wchar_t exePath[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring exeDir = exePath;
    {
        const size_t s = exeDir.find_last_of(L'\\');
        if (s != std::wstring::npos) exeDir.resize(s);
    }

    Out("========================================\n");
    Out(" SpaceSift %s 出厂自检\n", SPACESIFT_VERSION);
    Out(" exe 目录: %s\n", U8(exeDir).c_str());
    Out(" 索引目录: %s\n", db::DataDir().c_str());
    Out(" 临时目录: %s\n", U8(work).c_str());
    Out("========================================\n");

    Section("一、可选依赖");
    Out("  %-34s %s\n", "PDF (pdfium.dll)",
        extract::PdfReady() ? "就位" : "缺失 → PDF 正文搜不到");
    Out("  %-34s %s\n", "DWG (dwg2dxf.exe / ODA)",
        extract::DwgReady() ? "就位" : "缺失 → .dwg 不参与索引");
    Out("  %-34s %s\n", "OCR (tesseract + tessdata)",
        extract::OcrAvailable() ? "就位" : "缺失 → 扫描件/图片不识别");
    Out("  %-34s %s\n", "压缩包 (7-Zip / WinRAR)",
        extract::ArchiveReady() ? "就位" : "缺失 → .7z/.rar 只索引条目名");
    if (!extract::PdfReady())
        Out("  提示：pdfium.dll 必须和 exe 放同一层目录，便携包里已带。\n");

    // 内置语料 —— 不依赖用户任何文件
    CreateDirectoryW(work.c_str(), nullptr);
    const std::wstring sub = work + L"\\corpus";
    CreateDirectoryW(sub.c_str(), nullptr);

    Section("二、造样本");
    bool made = true;
    made &= WriteBytes(sub + L"\\utf8.txt", "第一行 自检关键词甲\n第二行\n");
    made &= WriteBytes(sub + L"\\gbk.txt", Gbk("这是GBK编码 自检关键词乙\n"));
    made &= WriteBytes(sub + L"\\d.dxf",
        "0\nSECTION\n2\nENTITIES\n0\nMTEXT\n8\n图层A\n10\n123.456\n20\n-99.1\n"
        "1\n图纸标题 自检关键词丙\n0\nTEXT\n8\n0\n10\n0.0\n20\n0.0\n1\n单行文字\n"
        // 属性块：值在 ATTRIB 的 code 1 里，零件名在 code 2 —— 图纸里最常见的可搜文字
        "0\nINSERT\n2\n螺栓自检M8\n10\n5.0\n20\n5.0\n"
        "0\nATTRIB\n1\n法兰盘自检3寸\n2\n零件名\n0\nENDSEC\n0\nEOF\n");
    made &= WriteBytes(sub + L"\\p.step",
        "ISO-10303-21;\nHEADER;\nFILE_NAME('装配自检.step','',(''),(''),'','','');\nENDSEC;\n"
        "DATA;\n#1 = CARTESIAN_POINT('',(0.,0.,0.));\n"
        "#42 = PRODUCT('法兰盘自检','x','',(#43));\nENDSEC;\nEND-ISO-10303-21;\n");
    made &= WriteBytes(sub + L"\\o.obj", "# comment\ng 支架自检组\nv 1.0 2.0 3.0\n");
    made &= WriteBytes(sub + L"\\c.docx", MakeZip({{"word/document.xml",
        "<?xml version=\"1.0\"?><w:document><w:body><w:p><w:r><w:t>合同自检关键词丁</w:t>"
        "</w:r></w:p></w:body></w:document>"}}));
    made &= WriteBytes(sub + L"\\s.xlsx", MakeZip({{"xl/sharedStrings.xml",
        "<sst><si><t>物料自检关键词戊</t></si></sst>"}}));
    made &= WriteBytes(sub + L"\\s.pptx", MakeZip({{"ppt/slides/slide1.xml",
        "<p:sld><a:t>汇报自检关键词己</a:t></p:sld>"}}));
    made &= WriteBytes(sub + L"\\a.zip", MakeZip({{"内层自检.md", "压缩包内文本自检关键词庚"},
                                                  {"sub/inner.docx", ""}}));
    made &= WriteBytes(sub + L"\\r.pdf", MakePdf("SpaceSift selftest PDF keyword"));
    made &= WriteBytes(sub + L"\\job.inp", "** ABAQUS\n*Heading\n** 三维算例 自检关键词辛\n");
    {
        // 旧版 .doc：正文区是 UTF-16LE，靠 OLE 头 + 宽字符扫描识别
        std::string u16;
        const char* cn = "自检旧版文档标题";
        int wn = MultiByteToWideChar(CP_UTF8, 0, cn, -1, nullptr, 0);
        std::wstring w((size_t)wn, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, cn, -1, &w[0], wn);
        u16.assign((const char*)w.data(), w.size() * sizeof(wchar_t));
        u16 = std::string("\xd0\xcf\x11\xe0", 4) + std::string(200, '\0') + u16 +
              std::string(200, '\0');
        made &= WriteBytes(sub + L"\\legacy.doc", u16);
    }
    Check(made, "12 个样本文件写入临时目录");

    Section("三、抽取（逐格式）");
    const std::string subU8 = U8(sub);
    struct Case { const char* file; const char* must; bool optional; };
    const Case cases[] = {
        {"\\utf8.txt",   "自检关键词甲", false},
        {"\\gbk.txt",    "自检关键词乙", false},
        {"\\d.dxf",      "自检关键词丙", false},
        {"\\d.dxf",      "螺栓自检M8",   false},
        {"\\d.dxf",      "法兰盘自检3寸", false},
        {"\\p.step",     "法兰盘自检",   false},
        {"\\o.obj",      "支架自检组",   false},
        {"\\c.docx",     "自检关键词丁", false},
        {"\\s.xlsx",     "自检关键词戊", false},
        {"\\s.pptx",     "自检关键词己", false},
        {"\\a.zip",      "内层自检.md",  false},
        {"\\a.zip",      "压缩包内文本自检关键词庚", false},
        {"\\r.pdf",      "selftest PDF keyword", true},
        {"\\job.inp",    "自检关键词辛", false},
        {"\\legacy.doc", "自检旧版文档标题", false},
    };
    for (const Case& c : cases) {
        const std::string p = subU8 + c.file;
        const std::string ext = extract::LowerExt(p);
        const std::string txt = extract::Text(p, ext);
        char label[192];
        snprintf(label, sizeof label, "%s → 「%s」", c.file, c.must);
        if (c.optional && txt.empty()) {
            Skip(label, "缺 pdfium.dll，PDF 不参与索引");
            continue;
        }
        Check(Has(txt, c.must), label);
        if (!Has(txt, c.must) && !txt.empty()) Out("         实际: %.120s\n", txt.c_str());
    }
    {
        const std::string t = extract::Text(subU8 + "\\d.dxf", ".dxf");
        Check(!Has(t, "123.456"), "DXF 坐标没被当成正文");
    }

    Section("四、内容索引 + 检索");
    std::atomic<bool> cancel{false};
    const DWORD t0 = GetTickCount();
    content::Build(subU8, cancel);
    const DWORD ms = GetTickCount() - t0;
    Out("  候选 %d · 写入 %d · 用时 %lu ms\n", content::g_seen.load(),
        content::g_parsed.load(), (unsigned long)ms);
    Check(content::HasIndex(subU8), "索引库已生成");
    Check(content::g_parsed.load() >= 10, "至少写入 10 个文档");

    struct Q { const char* q; const char* want; };
    const Q qs[] = {
        {"自检关键词甲", "utf8.txt"},
        {"自检关键词乙", "gbk.txt"},
        {"自检关键词丙", "d.dxf"},
        {"自检关键词丁", "c.docx"},
        {"自检关键词戊", "s.xlsx"},
        {"自检关键词己", "s.pptx"},
        {"压缩包内文本自检关键词庚", "a.zip"},
        {"自检关键词辛", "job.inp"},
    };
    for (const Q& q : qs) {
        auto hits = content::Search(subU8, q.q, 50);
        bool got = false;
        for (const auto& h : hits) if (Has(h.path, q.want)) got = true;
        char label[192];
        snprintf(label, sizeof label, "搜「%s」命中 %s", q.q, q.want);
        Check(got, label);
    }
    {
        auto hits = content::Search(subU8, "自检关键词丙 ext:dxf", 20);
        bool ok = !hits.empty();
        for (const auto& h : hits) if (!Has(h.path, ".dxf")) ok = false;
        Check(ok, "关键词 + ext:dxf 过滤生效");
    }
    {
        auto hits = content::Search(subU8, "完全不存在的词 zzz", 20);
        Check(hits.empty(), "无关词零命中");
    }
    {
        content::ResetProgress();
        content::Build(subU8, cancel);
        Check(content::g_parsed.load() == 0, "增量：文件没变则不重复抽取");
    }

    Section("五、跨目录检索（2.2.0 修的主问题）");
    const std::wstring sub2 = work + L"\\corpus2";
    CreateDirectoryW(sub2.c_str(), nullptr);
    WriteBytes(sub2 + L"\\other.txt", "另一个目录 自检关键词甲\n");
    const std::string sub2U8 = U8(sub2);
    {
        content::Build(sub2U8, cancel);
        auto all = content::SearchAll("自检关键词甲", 50);
        bool hasA = false, hasB = false;
        for (const auto& h : all) {
            if (Has(h.path, "corpus\\utf8.txt")) hasA = true;
            if (Has(h.path, "corpus2\\other.txt")) hasB = true;
        }
        Check(hasA && hasB, "一次查询同时命中两个目录的索引");
        auto scoped = content::Search(sub2U8, "自检关键词甲", 50);
        Check(scoped.size() == 1, "「当前目录」范围内只返回本目录那一条");
    }

    Section("六、文件名检索（扫描索引）");
    {
        std::string err;
        Check(NameIndexRoundTrip(sub2, &err), "写入→查询→跨库合并 全链路通");
        if (!err.empty()) Out("         %s\n", err.c_str());
    }

    Section("七、清理自检产物");
    {
        content::DeleteIndex(subU8);
        content::DeleteIndex(sub2U8);
        const wchar_t* files[] = {L"utf8.txt", L"gbk.txt", L"d.dxf", L"p.step", L"o.obj",
                                  L"c.docx", L"s.xlsx", L"s.pptx", L"a.zip", L"r.pdf",
                                  L"job.inp", L"legacy.doc"};
        for (const wchar_t* f : files) DeleteFileW((sub + L"\\" + f).c_str());
        RemoveDirectoryW(sub.c_str());
        DeleteFileW((sub2 + L"\\other.txt").c_str());
        RemoveDirectoryW(sub2.c_str());
        RemoveDirectoryW(work.c_str());
        Check(!content::HasIndex(subU8), "自检索引已删除，没在系统盘留垃圾");
    }

    if (folder && folder[0]) {
        Section("八、你指定的目录（只读体检）");
        const std::string r = content::NormRoot(U8(folder));
        const DWORD b0 = GetTickCount();
        content::Build(r, cancel);
        const DWORD bms = GetTickCount() - b0;
        Out("  目录: %s\n", r.c_str());
        Out("  候选 %d · 抽取 %d · 复用 %d · %.1f 秒\n", content::g_seen.load(),
            content::g_parsed.load(), content::g_skipped.load(), bms / 1000.0);
        int shown = 0;
        for (const auto& e : content::ListExtensions(r)) {
            if (shown++ >= 10) break;
            Out("    %-8s %d\n", e.ext.c_str(), e.count);
        }
        Check(content::HasIndex(r), "该目录可建立内容索引");
        Out("  注：这次是真实建索引，会保留下来供界面使用（不需要可在「索引管理」里删）。\n");
    }

    Out("\n========================================\n");
    Out(" 通过 %d · 失败 %d · 跳过 %d\n", g_pass, g_fail, g_skip);
    Out("%s", g_fail == 0 ? " 全部通过 —— 这个包可以正常使用\n"
                          : " 有失败项 —— 见上面 [FAIL] 的格式\n");
    // 别让"全部通过"盖住"PDF 用不了"这件事：exe-only 包就是这个状态
    if (g_fail == 0 && !extract::PdfReady())
        Out(" 注意：PDF 正文不可检索（缺 pdfium.dll）—— 用便携包，或把 dll 放到 exe 同目录\n");
    if (g_fail == 0 && !extract::DwgReady())
        Out(" 注意：.dwg 不参与索引（缺 dwg2dxf.exe）—— 需要的话用便携包\n");
    Out("========================================\n");
    if (g_log) fclose(g_log);
    g_log = nullptr;
    // 通过就把日志也删掉（说好了不留垃圾）；失败时留着，方便把线索发出来
    if (g_fail == 0) DeleteFileW(logPath.c_str());
    else Out(" 日志已保留: %s\n", U8(logPath).c_str());

    // 双击时控制台会随进程关掉，给一眼看结果的机会；
    // 自动化调用（bat / CI）设 SPACESIFT_SELFTEST_NOPAUSE=1 跳过弹窗，只看退出码
    const bool noPause = GetEnvironmentVariableW(L"SPACESIFT_SELFTEST_NOPAUSE", nullptr, 0) > 0;
    if (!ownConsole && !noPause) {
        MessageBoxW(nullptr,
                    g_fail == 0
                        ? L"自检全部通过：这个包可以正常使用。"
                        : L"自检有失败项，请看控制台输出，或 %TEMP% 下保留的日志。",
                    L"SpaceSift 自检",
                    MB_OK | (g_fail == 0 ? MB_ICONINFORMATION : MB_ICONWARNING));
    }
    return g_fail == 0 ? 0 : 1;
}

}  // namespace selftest
