// 探针：验证 openswx 能读出 SolidWorks 文件的哪些东西。
// 构建见 build.bat 的 openswx 段；这里先手动编。
#include <cstdio>
#include <string>

#include "openswx/document.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("用法: %s <file.sldprt|sldasm|slddrw>\n", argv[0]);
        return 2;
    }
    auto res = openswx::SwxDocument::Open(std::filesystem::path(argv[1]));
    if (!res.ok()) {
        printf("打开失败: %s\n", res.error().c_str());
        return 1;
    }
    const openswx::SwxDocument& sw = res.value();
    const openswx::Document& doc = sw.doc();

    printf("类型: %d (0=part 1=assembly 2=drawing)\n", (int)doc.type);
    printf("内部版本号: %d\n", doc.version);
    printf("全局自定义属性: %d 条\n", (int)doc.global_properties.size());
    for (const auto& kv : doc.global_properties)
        printf("   %s = %s\n", kv.first.c_str(), kv.second.c_str());

    printf("配置: %d 个\n", (int)doc.configurations.size());
    for (const auto& c : doc.configurations) {
        printf("   [%d] %s\n", c.index, c.name.c_str());
        for (const auto& kv : c.properties)
            printf("        属性 %s = %s\n", kv.first.c_str(), kv.second.c_str());
        for (const auto& comp : c.components)
            printf("        引用: %s | 路径: %s | 配置: %s%s\n", comp.name.c_str(),
                   comp.path.c_str(), comp.configuration_name.c_str(),
                   comp.is_suppressed ? " [压缩]" : "");
    }
    printf("图纸页: %d 个\n", (int)doc.sheets.size());
    for (const auto& s : doc.sheets) printf("   %s\n", s.name.c_str());
    return 0;
}
