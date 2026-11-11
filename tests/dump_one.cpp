// 调试用：打印某个文件抽出来的正文（判断"搜不到"是格式问题还是抽取问题）
// 用法: set DUMP_FILE=<路径> 然后 dump.exe
#include <windows.h>
#include <shellapi.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "extract.h"

int main() {
    std::string path;
    if (const wchar_t* env = _wgetenv(L"DUMP_FILE")) path = extract::FromWide(env);
    if (path.empty()) {
        int argc = 0;
        LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc >= 2) path = extract::FromWide(argv[1]);
        if (argv) LocalFree(argv);
    }
    if (path.empty()) {
        printf("用法: set DUMP_FILE=<文件> 然后 dump.exe\n");
        return 2;
    }

    if (std::getenv("DUMP_OCR")) extract::SetOcrEnabled(true);
    const std::string ext = extract::LowerExt(path);
    printf("文件: %s\n扩展名: %s\n支持: %s\nOCR: 可用=%d 已启用=%d\n\n", path.c_str(), ext.c_str(),
           extract::Supports(ext) ? "是" : "否", (int)extract::OcrAvailable(),
           (int)extract::OcrReady());

    const std::string text = extract::Text(path, ext);
    printf("抽出 %zu 字节 / %zu 字符\n", text.size(), text.size());
    if (text.empty()) {
        printf("（空 —— 无文本层，或抽取失败）\n");
        return 0;
    }
    printf("---------- 前 800 字节 ----------\n");
    printf("%.800s\n", text.c_str());

    // 垃圾比例：? 与控制字符
    size_t bad = 0;
    for (unsigned char c : text)
        if (c == '?' || c < 0x09 || (c > 0x0D && c < 0x20)) ++bad;
    printf("\n---------- 统计 ----------\n");
    printf("可疑字符（? 与控制符）: %zu / %zu = %.1f%%\n", bad, text.size(),
           text.empty() ? 0.0 : 100.0 * bad / text.size());
    return 0;
}
