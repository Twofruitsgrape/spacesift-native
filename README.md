# SpaceSift Native

磁盘空间分析与清理工具。原生 C++ + ImGui，单文件 exe，双击即用，不装任何运行时。

**简体中文** | [English](README.en.md)

## 功能

- **方块图** — 按真实占用面积展示，点方块逐层下钻；扫描过程中方块实时生长
- **推荐清理** — 17 条规则检查系统与应用缓存，勾选后清理
- **深度清理** — 列出当前目录的全部子项，可勾选加入待清理
- **检索结果** — 两种模式：**文件名**（扫描时同步建索引，338 万条约 0.4 秒出结果）和 **内容**（搜文件正文，见下）
- **内容检索** — 搜文件内部文字，结果带命中片段，可勾选进批量清理
  - 覆盖：文本/代码（`txt md csv log json xml yml ini html` 等 60+ 后缀，自动识别 UTF-8/GBK/UTF-16）
  - 图纸：`dxf`（抓 TEXT/MTEXT/ATTRIB 属性块，不吞坐标）
  - 三维：`step stp`（只抓单引号里的零件名/文件名）、`iges igs`、`obj`
  - 索引独立成库 `content_<root>.db`，按文件大小+修改时间增量更新，不重复抽取
- **二次确认** — 任何删除都先弹窗列出清单，确认后才执行，不进回收站
- **安全边界** — 只处理扫描范围内的路径，系统受保护目录一律跳过
- 明暗主题切换、右键菜单（打开所在位置 / 复制路径 / 从此处扫描）

## 使用

1. 下载 `SpaceSift.exe`，双击运行（免安装）
2. 选磁盘或输入路径，点「扫描」
3. 扫描完成后：
   - 左侧点方块，逐层查看空间被谁占了
   - 右侧勾选要清理的项目
   - 点「清理选中项」，在弹窗里确认

> 推荐清理的规则针对系统/应用缓存目录。扫描非系统盘时这些项目不会出现，属正常。

### 内容检索怎么用

1. 扫描完一个目录（内容索引按扫描根目录分开存）
2. 顶部搜索框左边切到 **「内容」**
3. 右侧「检索结果」页签里点 **「建立内容索引」**（只需一次）
4. 输入关键词回车；之后文件改动会自动增量更新，也可以手动点「增量更新」

索引库放在 `%LOCALAPPDATA%\SpaceSiftNative\content_<根目录>.db`。
实测 7058 个源码文件：建索引 1.6 秒，库 86 MB，查询 70–140 ms。

脚本化调用（便于自动化测试）：

```bat
set SPACESIFT_ROOT=D:\图纸
set SPACESIFT_MODE=content
set SPACESIFT_CONTENT=1
set SPACESIFT_SEARCH=法兰盘
SpaceSift.exe
```

## 实测

C 盘 339 万文件 / 436 GB：扫描 38 秒，内存峰值 125 MB，exe 2.3 MB。

## 运行环境

Windows 10 1803+ / Windows 11，64 位。依赖系统自带的 DirectX 11 与 UCRT，无需额外安装运行时。

## 从源码构建

需要 MinGW-w64（`g++`、`gcc`、`windres` 在 PATH 中）。

```bat
build.bat
```

或手动执行：

```bash
# 1) 编译 SQLite（只需一次）
gcc -O2 -c third_party/sqlite/sqlite-amalgamation-3450000/sqlite3.c -o build/sqlite3.o \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_EXPLAIN

# 2) 编译资源与主程序
windres src/app.rc -o build/app_res.o

g++ -O2 -std=c++17 -static -s -mwindows \
  -I third_party/imgui -I third_party/imgui/backends \
  -I third_party/sqlite/sqlite-amalgamation-3450000 -I src \
  src/main.cpp src/db.cpp src/rules.cpp build/sqlite3.o build/app_res.o \
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp \
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp \
  third_party/imgui/backends/imgui_impl_win32.cpp \
  third_party/imgui/backends/imgui_impl_dx11.cpp \
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -limm32 \
  -o build/SpaceSift.exe
```

## 许可

MIT
