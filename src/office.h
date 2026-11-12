// 97-2003 版 Office（.doc/.xls/.ppt）的原生文本抽取。
//
// 这三个格式都是 OLE2 复合文档（CFB）：文件内部是"扇区 → FAT → 目录项 → 流"。
// 这里只实现读取需要的最小集合：解析头、FAT、目录、取指定流的字节。
// 然后按各自的格式解析：
//   .doc  —— FIB 里的 fcMin/fcMac 就是正文区间（快存文件才需要分片表）
//   .xls  —— BIFF8 的 SST 记录（共享字符串表）存着所有文本单元格
//   .ppt  —— PowerPoint Document 流里的 TextCharsAtom / TextBytesAtom 记录
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace office {

// 是否由本模块处理（只认 .doc/.xls/.ppt）
bool Handles(const std::string& ext);

// 抽正文（UTF-8）。失败返回空串。
std::string Text(const std::string& path, const std::string& ext);

}  // namespace office
