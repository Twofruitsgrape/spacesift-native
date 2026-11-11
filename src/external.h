// 外部转换器调用：进程启动、输出回收、工具查找。
//
// 用途：那些没有 MIT 小库可用的格式，靠用户机器上已有的命令行工具来兜底
// （DWG → dwg2dxf / ODA File Converter，.doc/.ppt → catdoc / soffice）。
// 全部走 subprocess，主程序不链接任何 GPL 代码。
#pragma once

#include <windows.h>

#include <string>

namespace external {

std::wstring ExeDir();
std::wstring FindOnPath(const wchar_t* fileName);

// 临时文件/目录名（不创建）；suffix 形如 L".dxf"
std::wstring TempName(const wchar_t* suffix);

bool FileExists(const std::wstring& path);
bool DirExists(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::string& out, size_t cap = 256 * 1024);
bool DeleteQuiet(const std::wstring& path);
bool RemoveDirQuiet(const std::wstring& path);

// 跑进程并把 stdout+stderr 收回到 out（≤ cap 字节）
bool RunCapture(const std::wstring& exe, const std::wstring& args, std::string& out,
                size_t cap = 256 * 1024, DWORD timeoutMs = 60000);

// 跑进程，丢掉输出
bool RunSilent(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs = 60000);

}  // namespace external
