// 把某个文件的抽取结果全文写成文件（验证"关键词到底进没进索引"）。
// 用法: build\extract_one.exe <输入路径文件(UTF-8)> <输出文件>
//   输入路径从文件读，避免命令行代码页把中文路径弄坏
#include <windows.h>

#include <cstdio>
#include <string>

#include "extract.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("用法: extract_one.exe <路径文件> <输出文件>\n");
        return 2;
    }
    auto readAll = [](const char* p) {
        std::string out;
        FILE* f = fopen(p, "rb");
        if (!f) return out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
        fclose(f);
        return out;
    };
    std::string path = readAll(argv[1]);
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
        path.pop_back();

    const std::string ext = extract::LowerExt(path);
    const std::string text = extract::Text(path, ext);
    printf("路径: %s\n扩展名: %s\n支持: %d\n抽出: %zu 字节\n", path.c_str(), ext.c_str(),
           (int)extract::Supports(ext), text.size());

    FILE* f = fopen(argv[2], "wb");
    if (f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
    }
    return 0;
}
