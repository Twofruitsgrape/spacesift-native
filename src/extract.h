// 纯文本抽取：把各种文件里的可搜索文字挖出来，统一成 UTF-8。
//
// P1 只覆盖"字节里本来就有文字"的格式（ASCII 系 + 带编码的老文本）：
//   文本/代码  .txt .md .csv .log .json .xml .ini .yml .yaml .html 等
//   图纸       .dxf（按 group code 抓 TEXT/MTEXT/ATTRIB，而不是把坐标一起吞进来）
//   三维       .step .stp（只抓单引号里的名字） .iges .igs .obj
// 二进制格式（.doc/.xls/.ppt/.pdf/.dwg）留给后续阶段，见 docs/02-FORMAT-MATRIX.md。
#pragma once

#include <cstdint>
#include <string>

namespace extract {

// 参与内容索引的扩展名白名单（含点，小写）
bool Supports(const std::string& ext);

// 这些格式是流式读的（只取需要的条目），不受 kSkipAbove 体积上限限制
bool StreamsLargeFiles(const std::string& ext);

// 取小写扩展名（含点）；没有扩展名返回空串
std::string LowerExt(const std::string& path);

// 抽文本，返回 UTF-8；抽不出来返回空串
std::string Text(const std::string& path, const std::string& ext);

// PDF 抽取依赖运行时的 pdfium.dll（放 exe 同目录）。
// 没装也不影响其他格式，只是 .pdf 不参与索引。
bool PdfReady();

// 以下都靠外部命令行工具，没装对应工具就不把该扩展名列入索引：
//   DwgReady     ← dwg2dxf.exe（LibreDWG）或 ODA File Converter
//   LegacyReady  ← catdoc / catppt / xls2csv / antiword / LibreOffice
//   ArchiveReady ← 7z / 7za / 7zz / UnRAR
bool DwgReady();
bool LegacyReady();
bool ArchiveReady();

// 扫描件/截图 OCR 靠外部 tesseract.exe（装了才有）
// OCR 比其它抽取慢两个数量级（每张图 1–2 秒），所以默认关闭，由界面开关控制。
bool OcrAvailable();          // tesseract 装没装
void SetOcrEnabled(bool on);  // 只管图片（截图/照片）；扫描件 PDF 自动 OCR
bool OcrReady();              // 可用且已启用

// OCR 时间预算：扫描件 PDF 自动 OCR，但每次建索引最多做这么多页，
// 免得一个全是扫描件的目录把首次索引拖成几十分钟。
// 预算用完的文件不写库，下次建索引会接着做。
int  OcrBudgetLeft();
void ResetOcrBudget();

// UTF-8 <-> 宽字符（Win32 路径用）
std::wstring ToWide(const std::string& utf8);
std::string  FromWide(const std::wstring& w);

// 每个文件最多存这么多正文，避免一个 500 MB 的日志把库撑爆
constexpr size_t   kStoreCap  = 64 * 1024;
// 超过这个体积直接跳过（装配体 STEP 动辄几百 MB）
constexpr uint64_t kSkipAbove = 256ull * 1024 * 1024;
// 普通文本文件最多读这么多
constexpr uint64_t kReadCap   = 4ull * 1024 * 1024;

}  // namespace extract
