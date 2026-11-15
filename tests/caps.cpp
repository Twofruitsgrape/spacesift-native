// 打印本机实际启用的内容索引格式（受外部依赖影响）。
// 用法: caps.exe
#include <cstdio>
#include <string>
#include <vector>

#include "extract.h"

int main() {
    printf("外部依赖：pdfium=%s  dwg转换器=%s  压缩包工具=%s  OCR(tesseract)=%s\n\n",
           extract::PdfReady() ? "有" : "无", extract::DwgReady() ? "有" : "无",
           extract::ArchiveReady() ? "有" : "无", extract::OcrAvailable() ? "有" : "无");

    const char* all[] = {
        // 文本/代码
        ".txt", ".md", ".csv", ".tsv", ".log", ".json", ".xml", ".yml", ".ini", ".toml",
        ".html", ".css", ".svg", ".c", ".cpp", ".h", ".cs", ".java", ".py", ".js", ".ts",
        ".rs", ".go", ".sql", ".sh", ".ps1", ".bat", ".lua", ".rb", ".php", ".r", ".m",
        // Office
        ".docx", ".docm", ".xlsx", ".xlsm", ".pptx", ".pptm", ".odt", ".ods", ".odp",
        ".doc", ".xls", ".ppt",
        // 文档/电子书/邮件
        ".pdf", ".epub", ".eml", ".msg",
        // 压缩包
        ".zip", ".rar", ".7z",
        // 图纸/三维
        ".dxf", ".dwg", ".step", ".stp", ".iges", ".igs", ".obj", ".stl", ".ply",
        // SolidWorks
        ".sldprt", ".sldasm", ".slddrw",
        // CAE
        ".mph", ".inp", ".cdb", ".anf", ".wbpj", ".dat", ".msg2", ".sta", ".jnl", ".rpy",
        // MATLAB / Simulink
        ".slx", ".slxc", ".sldd", ".slddc", ".mldatx", ".mlx", ".mat", ".asv", ".nb",
        // 其它工程文档
        ".vsdx", ".xmind", ".3mf", ".ipynb", ".drawio", ".mm", ".opml",
        // 图片
        ".png", ".jpg", ".bmp", ".tif",
        // 反例
        ".caj", ".djvu", ".scdoc", ".dll", ".cae", ".odb", ".rst",
    };
    int yes = 0, no = 0;
    std::string sup, unsup;
    for (const char* e : all) {
        if (extract::Supports(e)) { ++yes; sup += e; sup += ' '; }
        else { ++no; unsup += e; unsup += ' '; }
    }
    printf("支持 %d 种：\n%s\n\n不支持 %d 种：\n%s\n", yes, sup.c_str(), no, unsup.c_str());
    return 0;
}
