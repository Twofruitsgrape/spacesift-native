# SpaceSift Native

C++ 重写版。目标是「一个小 exe，双击就能用，不装任何运行时」。

## 功能（与 Python 版对齐）

- **扫描**：选择磁盘 / 粘贴路径 / 「浏览…」挑文件夹，单线程 `FindFirstFileEx` 遍历
- **方块图**：squarified treemap，面积按真实占用分配；目录块逐层下钻，文件块点击在资源管理器中定位，面包屑回上层
- **右栏三页签**
  - 推荐清理：当前左侧打开目录下的规则命中项（扫 E: 就只列 E: 下的，其他盘/目录不会出现）
  - 深度清理：当前目录下的子目录和直接文件，按大小排序；目录可点进去，文件可勾选清理
  - 检索结果：文件名搜索，按大小降序，最多 400 条
- **全选安全项 / 取消全选**：只作用于 `safe` 级规则
- **清理选中项**：后台线程删除，界面不卡；只删规则声明过、且真实存在的路径；受保护目录（Windows / Program Files / ProgramData）一律跳过
- **搜索**：扫描时同步写 SQLite 索引，扫完即可搜，不用再走一遍盘
- **暂停 / 继续 / 结束**：扫描中可随时中止
- **亮色 / 暗色主题**：右上角切换，选择记在 `settings.ini`，下次启动保持
- 双击打开即进入「先选一个位置」引导界面，不会自己去扫 C 盘

## 与 Python 版的数据对比（同一台机器，C 盘 338 万文件 / 435 GB）

| 指标 | Python + WebView2 | **C++ 原生** |
|---|---|---|
| exe 体积 | 15.2 MB | **2.12 MB** |
| 扫描耗时 | 44 ~ 100 秒 | **23 ~ 39 秒** |
| 内存峰值 | 143.9 MB | **125 MB** |
| 索引体积 | — | **173 MB** |
| 运行时依赖 | 需 WebView2 | 无（静态链接） |

## 构建

需要 g++（MinGW-w64）。`build/sqlite3.o` 与 `build/app_res.o` 已随仓库提供；
没有的话按下面第 1、2 步生成。

```bash
# 1) 编译 SQLite（只需一次）
gcc -O2 -c third_party/sqlite/sqlite-amalgamation-3450000/sqlite3.c -o build/sqlite3.o \
  -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 \
  -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_EXPLAIN

# 2) 编译图标资源（只需一次）
windres src/app.rc -O coff -o build/app_res.o

# 3) 编译主程序
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

# 4) 同步到发布目录（重要：dist 里的旧 exe 不会自动更新）
cp build/SpaceSift.exe dist/SpaceSift.exe
```

也可以直接运行 `build.bat`（同样是上面这套命令，最后自动复制到 `dist/`）。

## 运行环境

Windows 10 1803+ / Windows 11，64 位。依赖系统自带的 `d3d11.dll`、`D3DCOMPILER_47.dll`、
UCRT（`api-ms-win-crt-*`），这几样 Win10 起都是系统组件，不用额外装。

## 设计取舍

| 决定 | 原因 |
|---|---|
| 只把**目录**放内存 | 338 万文件的完整路径要 270 MB。当前视图的直接文件按需枚举（最多保留最大的 400 个），不常驻内存 |
| 索引只存 `parent id + name + size` | 存完整路径时每行多 80 字节，库从 173 MB 涨到 656 MB；存 TEXT 主键更要 1.7 GB |
| 扫描结束时**后序累加**体积 | 每个文件都爬父链是 2700 万次跳转，扫描时间翻倍（50.7s → 23s） |
| 搜索走**全表扫 + LIKE** | 不建索引省一大截空间；338 万行约 0.4 秒，够用 |
| 扫描期间**不渲染方块图** | `entries` 在 push_back 时重分配，渲染线程同时读会越界崩溃 |
| 规则表**硬编码** | 配置文件被改坏（比如路径写成 `C:\`）就是灾难，硬编码至少随程序一起审查过 |
| 空闲降到 ~25fps | 60fps 空转白烧 3.9% CPU |
| 不打包中文字体 | 读系统 `msyh.ttc`，省 10 MB；正文 15px + 小字 12.5px 两档，层级更清楚 |

## 界面设计参考

配色/分段控件/勾选框的组织方式参考了这些开源项目，但**没有引入任何依赖**，全部在本项目里自己实现：

- [Patitotective/ImThemes](https://github.com/Patitotective/ImThemes) —— 主题浏览器，用于对比亮/暗色 token 取法
- [GraphicsProgramming/dear-imgui-styles](https://github.com/GraphicsProgramming/dear-imgui-styles) —— 各类 ImGui 主题集合
- [pthom/hello_imgui](https://github.com/pthom/hello_imgui) —— 语义化主题/亮暗切换的组织方式
- [yuanliangxie/FluentImGui](https://github.com/yuanliangxie/FluentImGui) —— 圆角、留白、分段控件、亮暗两套调色板
- [cmdwtf/imgui_toggle](https://github.com/cmdwtf/imgui_toggle) —— 自绘勾选/开关的命中区处理

## 已知限制

- 索引按盘/目录各建一份，放在 `%LOCALAPPDATA%\SpaceSiftNative\`，C 盘约 173 MB；
  路径会先归一化（`...\AppData` 与 `...\AppData\` 共用一份），超过 30 天没用的库会在启动时清理
- 搜索结果显示小写文件名（库里为省空间统一存小写）
- 方块图/深度清理里的「文件」只枚举当前打开的目录；点进下一层会重新枚举，目录本身的体积仍来自扫描结果
- 清理走直接删除，不进回收站；清理后方块图/深度列表要下次扫描才刷新
- 扫描期间不能搜索（索引正在重建），等扫描结束即可
- 主题/索引都在 `%LOCALAPPDATA%\SpaceSiftNative\`；删掉该目录即恢复首次启动状态（默认暗色）
- 尚未做归档（搬到其它盘）和右键菜单
