# SpaceSift 内容检索 — 使用与交付说明

> 版本：2026-11-11 ｜ 主程序 MIT ｜ 单文件 exe + 可选依赖

## 一、它是什么

在 SpaceSift（磁盘空间分析）里加了一层**文件内容检索**：扫描一个目录，之后可以按
**文件名**或**文件内部文字**搜索，结果能直接勾选进批量清理。

和纯文件名搜索工具的区别：它能读进 PDF、Word、图纸、三维模型、压缩包、扫描件里的文字。

## 二、怎么用

1. 选一个目录，点「扫描」（这一步只建文件名索引，很快）
2. 顶部搜索框左边切到 **「内容」**
3. 右栏「检索结果」里点 **「建立内容索引」**（只需一次，之后按文件大小+修改时间增量更新）
4. 输入关键词回车

> 内容索引和文件名索引是**分开的两个库**，重扫不会重建正文。
> 换一个扫描目录 = 另一份内容索引。

## 三、格式覆盖

| 类别 | 格式 | 实现方式 |
|---|---|---|
| 文本/代码 | 60+ 后缀（txt/md/csv/log/json/xml/yml/ini/html/css + c/cpp/py/js/ts/rs/go/java…） | 直读 + 编码探测（UTF-8/GBK/UTF-16） |
| Office | docx / docm / xlsx / xlsm / pptx / pptm / odt / ods / odp | 直读 OOXML |
| 旧版 Office | **doc / ppt / xls** | UTF-16LE 扫描（零依赖）；有 catdoc/LibreOffice 时优先用 |
| PDF | 文本层 | pdfium |
| PDF 扫描件 | 无文本层的页 | pdfium 渲染 → Tesseract（需勾选 OCR） |
| 图片 | png / jpg / bmp / tif | Tesseract（需勾选 OCR） |
| 图纸 | **dxf**（TEXT/MTEXT/ATTRIB）、**dwg**（外部转换器）、扫描图 | 自研 DXF 解析 / LibreDWG / OCR |
| 三维 | **step / stp**（只取单引号里的名字，含 `\X2\` 转义）、iges / igs / obj | 自研 |
| SolidWorks | **sldprt / sldasm / slddrw** | openswx（自定义属性、配置名、装配体零件引用、焊接清单、图纸页） |
| COMSOL | **mph** | 流式 zip → smodel.json（模型树 label）+ dmodel.xml（参数/变量/物理场） |
| CAE 文本 | **inp / cdb / anf / wbpj / dat / msg / sta** | 纯文本 |
| 压缩包 | **zip**（条目名 + 内层文本）、**rar** / **7z**（条目名） | miniz / UnRAR / 7-Zip |

**读不了的**（私有二进制，必须装对应软件）：
`.cae` `.odb`（Abaqus）、`.db` `.rst`（ANSYS）。

## 四、可选依赖

缺哪个就少一类格式，右栏会提示，不影响其他格式。

| 格式 | 需要 | 体积 |
|---|---|---|
| PDF | `pdfium.dll` 放 exe 同目录 | 7.8 MB |
| DWG | `dwg2dxf.exe` + `libredwg-0.dll` + `libiconv-2.dll`（LibreDWG，GPLv3） | 32 MB |
| DWG（更准） | ODA File Converter（自动探测注册表/Program Files） | — |
| 扫描件 PDF 自动 OCR | Tesseract-OCR + 语言包放 `tessdata/` | 16.7 MB |
| 图片 OCR（截图/照片） | 同上，另需勾选「OCR 图片」 | — |
| 旧版 Office | catdoc 系列 或 LibreOffice（可选，没有也能读中文） | — |
| rar / 7z | WinRAR 或 7-Zip（从注册表/Program Files 自动找） | — |

主程序不链接任何 GPL 代码，全部走子进程调用。

## 五、实测性能

### D:\毕少华（个人资料，3274 文件 / 46 GB）

| 指标 | 数值 |
|---|---|
| 可抽正文的候选 | 2303（70.3%） |
| 建索引 | **42 秒** |
| 写入正文 | 3372 万字符 |
| 索引库 | 64.4 MB（原始体积的 0.14%） |
| 查询 | 精确词 2–10 ms，泛词 90–120 ms |

### E:\Projects（代码库，16624 文件 / 135 GB）

| 指标 | 数值 |
|---|---|
| 候选 | 9306（56.0%） |
| 建索引 | **251 秒** |
| 写入正文 | 7819 万字符 |
| 索引库 | 116.6 MB |

首次建索引的成本主要在 FTS5 倒排写入（不是文件读取）。
**之后都是增量**：文件没变就跳过，实测 0.06 秒。

### 逐格式抽取耗时（每格式采样 20 个）

| 格式 | 平均 | 最大 |
|---|---|---|
| .pdf | 59 ms | 355 ms |
| .rar | 47 ms | 58 ms（进程启动占大头） |
| .mph | 12 ms | 24 ms（400 MB 模型也是这个量级，因为是流式读） |
| .xlsx | 8.6 ms | 13 ms |
| 文本/代码 | < 0.2 ms | — |

## 六、已知限制

- **单文件正文最多 64 KB**，超出的部分搜不到（索引体积换来的）
- **超过 256 MB 的文件跳过**（zip/mph 除外，它们是流式读的）
- **扫描件 PDF 自动 OCR**：无文本层或文本层是坏的（扫描软件生成的假 OCR 层）都会自动渲染识别，
  每次建索引最多 120 页（约 2–3 分钟）；超出的文件不写库，下次建索引接着做
- **图片 OCR 默认关闭**：截图/照片大多是噪音，且一张 1–2 秒，勾选后才做
- **扫描件 PDF 需要 OCR 才有文字**，默认关闭时这类文件会被记为"抽空"
- `.7z/.rar` 只索引**条目名**，不抽里面的正文（逐条解压太慢）
- `.mph` 只读模型树/参数/物理场，**不读网格与求解结果**（那是二进制，且无文字）
- 无相关度学习：排序是「命中次数 / 文档长度」，不是 BM25

## 七、命令行/脚本化（便于自动化测试）

```bat
set SPACESIFT_ROOT=D:\图纸
set SPACESIFT_MODE=content
set SPACESIFT_CONTENT=1
set SPACESIFT_SEARCH=法兰盘
SpaceSift.exe
```

## 八、体检报告工具（开发用）

```
build\report.exe            （目录用环境变量 REPORT_ROOT 传，避免中文被中转坏）
REPORT_ROOT="D:\某目录" build\report.exe
REPORT_ROOT=... REPORT_SKIP_BUILD=1 REPORT_SLOWEST=1 build\report.exe
```

输出：文件总数、候选数、各格式索引情况、抽空数、逐格式耗时、最慢文件、查询延迟、库体积。
