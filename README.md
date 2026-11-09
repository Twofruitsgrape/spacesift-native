# SpaceSift Native

磁盘空间分析与清理工具。原生 C++ + ImGui，单文件 exe，双击即用，不装任何运行时。

**简体中文** | [English](README.en.md)

## 功能

- **方块图** — 按真实占用面积展示，点方块逐层下钻；扫描过程中方块实时生长
- **推荐清理** — 17 条规则检查系统与应用缓存，勾选后清理
- **深度清理** — 列出当前目录的全部子项，可勾选加入待清理
- **检索结果** — 按文件名搜索，扫描时同步建索引，338 万条约 0.4 秒出结果
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
