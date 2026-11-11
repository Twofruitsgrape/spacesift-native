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

// 取小写扩展名（含点）；没有扩展名返回空串
std::string LowerExt(const std::string& path);

// 抽文本，返回 UTF-8；抽不出来返回空串
std::string Text(const std::string& path, const std::string& ext);

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
