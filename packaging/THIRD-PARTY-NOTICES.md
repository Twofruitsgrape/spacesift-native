# 第三方组件与许可

SpaceSift 本体是 **MIT**（见 `LICENSE`）。发行包里另外带了几个第三方组件，
它们各自的许可如下。**这些组件都是可选的**：删掉对应的文件，程序照常运行，
只是少一类格式。

## 随发行包分发的组件

| 组件 | 文件 | 许可 | 用途 |
|---|---|---|---|
| **PDFium** | `pdfium.dll` | BSD-3-Clause（含 abseil/zlib/freetype 等，见 `licenses/pdfium/`） | 读 PDF 文本层、渲染扫描页 |
| **LibreDWG** | `dwg2dxf.exe`、`libredwg-0.dll`、`libiconv-2.dll` | **GPL-3.0-or-later**（全文见 `licenses/GPL-3.0.txt`） | 把 DWG 转成 DXF |
| **Tesseract 语言包** | `tessdata/*.traineddata` | Apache-2.0 | 扫描件/图片 OCR |
| SQLite | 静态编译进 exe | Public Domain | 索引与全文检索 |
| Dear ImGui | 静态编译进 exe | MIT | 界面 |
| miniz | 静态编译进 exe | MIT | zip 解压 |
| pugixml | 静态编译进 exe | MIT | OOXML / SMODEL 解析 |
| openswx | 静态编译进 exe | MIT | SolidWorks 文件解析 |

### 关于 LibreDWG（GPL-3.0）

`dwg2dxf.exe` / `libredwg-0.dll` 来自 [LibreDWG](https://www.gnu.org/software/libredwg/)，
按 **GPL-3.0-or-later** 分发。

**它们和主程序是两个独立程序**：主程序通过子进程调用 `dwg2dxf.exe`，
不链接任何 GPL 代码，因此主程序可以继续使用 MIT 许可
（这是 FSF 认可的"mere aggregation / 独立进程"用法）。

LibreDWG 完整源码获取方式：
- 上游：https://github.com/LibreDWG/libredwg （发行版 0.14）
- 或 GNU FTP：https://ftp.gnu.org/gnu/libredwg/

**不想接受 GPL-3.0 的用户**：直接删除 `dwg2dxf.exe`、`libredwg-0.dll`、`libiconv-2.dll`，
程序不会加载它们，DWG 格式不再参与检索，其余功能不受影响。

### 运行时不随包分发的外部工具

下列工具由用户自行安装，本发行包**不含**它们的代码：

| 格式 | 工具 | 许可 |
|---|---|---|
| rar / 7z | WinRAR 的 `UnRAR.exe` 或 7-Zip 的 `7z.exe` | 各自许可（unRAR restriction / LGPL） |
| doc / xls / ppt（英文内容） | catdoc 或 LibreOffice（可选，程序会自动探测） | GPL / MPL |
| 更精确的 DWG | ODA File Converter（可选，自动探测） | 专有，禁止再分发 |

## 字体

界面中文使用系统字体 `msyh.ttc` / `simhei.ttf` / `simsun.ttc`，不随包分发。
