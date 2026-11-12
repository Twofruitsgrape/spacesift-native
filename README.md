# SpaceSift Native

磁盘空间分析 + **文件内容检索**。原生 C++，单文件 exe，不装任何运行时。

**简体中文** | [English](README.en.md)

## 下载

到 [Releases](https://github.com/Twofruitsgrape/spacesift-native/releases) 页面：

| 包 | 大小 | 说明 |
|---|---|---|
| **`SpaceSift-2.0.0-portable.zip`** | 21 MB | **推荐**。解压后双击 `SpaceSift.exe`，或运行 `install.bat` 一键安装（装到当前用户目录 + 建桌面快捷方式，不需要管理员） |
| `SpaceSift-2.0.0-exe-only.zip` | 1.9 MB | 只要主程序。文本/代码/Office/压缩包/三维模型可用；**PDF、DWG、OCR 需要额外的 dll**（见 `USAGE.md`） |

两种都不写注册表，删除目录即卸载。

## 功能

- **方块图** — 按真实占用面积展示，点方块逐层下钻；扫描过程中方块实时生长
- **推荐清理** — 17 条规则检查系统与应用缓存，勾选后清理
- **深度清理** — 列出当前目录的全部子项，可勾选加入待清理
- **检索结果** — 两种模式：
  - **文件名**：扫描时同步建索引，338 万条约 0.4 秒出结果
  - **内容**：搜文件正文，带命中片段，可勾选进批量清理
- **二次确认** — 任何删除都先弹窗列出清单，确认后才执行，不进回收站
- **安全边界** — 只处理扫描范围内的路径，系统受保护目录一律跳过
- 明暗主题切换、右键菜单（打开所在位置 / 复制路径 / 从此处扫描）

## 内容检索支持哪些格式

| 类别 | 格式 |
|---|---|
| 文本 / 代码 | 60+ 后缀（txt md csv log json xml yml ini html css + c/cpp/py/js/ts/rs/go/java…），自动识别 UTF-8 / GBK / UTF-16 |
| Office 新版 | docx docm xlsx xlsm pptx pptm odt ods odp |
| **Office 旧版** | **doc / xls / ppt**（原生解析 OLE 复合文档，中文英文都能读） |
| PDF | 文本层；**扫描件自动 OCR**（含坏文本层抢救） |
| 图片 | png / jpg / bmp / tif（可选，需装 Tesseract） |
| 电子书 | **epub** |
| 邮件 | **eml**（MIME 解码）/ **msg** |
| 图纸 | **dxf** / **dwg** / 扫描图 |
| 三维 | **step / stp** / iges / igs / obj |
| SolidWorks | **sldprt / sldasm / slddrw**（自定义属性、配置名、装配体零件引用、焊接清单、图纸页） |
| COMSOL | **mph**（流式读，几百 MB 不占内存） |
| CAE 文本 | inp / cdb / anf / wbpj / dat / msg / sta / jnl / rpy（ABAQUS、ANSYS） |
| 压缩包 | **zip / rar / 7z**（条目名 + 内层文本文件） |

**读不了的**：`.cae` `.odb`（Abaqus）、`.db` `.rst`（ANSYS）——私有二进制，必须装对应软件。

## 检索语法

```
词A 词B              同时包含（默认 AND）
"完整短语"           短语匹配
词A OR 词B           任一包含
-词C                 排除
ext:pdf              只看某类扩展名（可写 ext:pdf,docx）
name:报告            文件名里包含
size:>10m            大于 10 MB（支持 k/m/g）
after:2024-06        修改时间在 2024-06 之后
re:正则  或 /正则/   正则匹配（全表扫描，慢）
```

界面里也可以直接点搜索框下面那排**后缀标签**筛选，点一下立刻重搜。

## 使用

1. 双击 `SpaceSift.exe`（或安装后的桌面快捷方式）
2. 选磁盘或输入路径，点「扫描」
3. 扫描完成后：
   - 左侧点方块，逐层查看空间被谁占了
   - 顶部搜索框切到「内容」，右栏点「建立内容索引」，然后搜关键词
   - 右侧勾选要清理的项目，点「清理选中项」，在弹窗里确认

> 推荐清理的规则针对系统/应用缓存目录。扫描非系统盘时这些项目不会出现，属正常。

### 外部依赖（全部可选，缺了只是少一类格式）

| 格式 | 需要 | 体积 |
|---|---|---|
| PDF | `pdfium.dll`（便携包里已带） | 7.8 MB |
| DWG | `dwg2dxf.exe` + `libredwg-0.dll`（便携包里已带，GPLv3） | 32 MB |
| 扫描件/图片 OCR | Tesseract-OCR + `tessdata/`（便携包里已带语言包） | 17 MB |
| rar / 7z | WinRAR 或 7-Zip（从注册表/Program Files 自动找） | — |
| 旧版 Office 英文 | 不需要（原生解析）；复杂文件可选装 catdoc / LibreOffice | — |

## 实测

| 语料 | 文件 | 体积 | 首次建索引 | 正文字符 | 索引库 |
|---|---:|---:|---:|---:|---:|
| 个人资料 | 3,274 | 46 GB | 42 秒 | 3,372 万 | 64 MB |
| 代码仓库 | 16,624 | 135 GB | 251 秒 | 7,819 万 | 117 MB |

之后都是**增量**（文件没变就跳过，实测 0.06 秒）。查询 2–120 ms。

C 盘 339 万文件 / 436 GB：扫描 38 秒，内存峰值 125 MB，exe 4 MB。

## 运行环境

Windows 10 1803+ / Windows 11，64 位。依赖系统自带的 DirectX 11 与 UCRT，无需额外安装运行时。

## 从源码构建

需要 MinGW-w64（`g++`、`gcc`、`windres`）。没有的话三种任选一种：

```bat
setx W64DEVKIT_DIR "C:\path\to\w64devkit"      :: 方式一：环境变量
:: 方式二：在 build.bat 同目录建 toolchain.txt，写工具链根目录（一行）
:: 方式三：把 MinGW 的 bin 直接加进 PATH
```

```bat
build.bat
```

## 许可

主程序 **MIT**。第三方组件的许可见 [`packaging/THIRD-PARTY-NOTICES.md`](packaging/THIRD-PARTY-NOTICES.md)。

> LibreDWG（`dwg2dxf.exe` / `libredwg-0.dll`）是 **GPL-3.0**，但它是**独立进程**被调用的，
> 主程序不链接任何 GPL 代码，所以主程序可以保持 MIT。删掉这几个文件即可完全避开 GPL，
> DWG 格式不再参与检索，其余功能不受影响。
