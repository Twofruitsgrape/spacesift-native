@echo off
REM SpaceSift Native 构建脚本（需要 MinGW-w64 的 g++/gcc/windres）
setlocal

REM ---- 找编译器：PATH 优先，其次 W64DEVKIT_DIR 或 toolchain.txt ----
call :find_toolchain
if errorlevel 1 exit /b 1

if not exist build mkdir build

REM 文件名留 fts5 后缀：带不带 FTS5 是两套二进制，避免旧目标文件被误用
if not exist build\sqlite3_fts5.o (
  echo [1/3] 编译 SQLite（含 FTS5）...
  gcc -O2 -c third_party/sqlite/sqlite-amalgamation-3450000/sqlite3.c -o build\sqlite3_fts5.o ^
    -DSQLITE_ENABLE_FTS5 ^
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 ^
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_EXPLAIN
  if errorlevel 1 exit /b 1
) else (
  echo [1/3] 复用已有的 build\sqlite3_fts5.o
)

if not exist build\miniz_zip.o (
  echo [2/3] 编译 miniz...
  for %%f in (miniz miniz_tdef miniz_tinfl miniz_zip) do (
    gcc -O2 -c third_party\miniz\%%f.c -o build\%%f.o
    if errorlevel 1 exit /b 1
  )
) else (
  echo [2/3] 复用已有的 miniz 目标文件
)

echo [3/3] 编译资源与主程序...
windres src\app.rc -o build\app_res.o
if errorlevel 1 exit /b 1

g++ -O2 -std=c++17 -static -s -mwindows ^
  -I third_party/imgui -I third_party/imgui/backends ^
  -I third_party/sqlite/sqlite-amalgamation-3450000 -I third_party/miniz ^
  -I third_party/pdfium/include -I src ^
  src/main.cpp src/db.cpp src/rules.cpp src/extract.cpp src/content.cpp src/external.cpp ^
  build\sqlite3_fts5.o build\miniz.o build\miniz_tdef.o build\miniz_tinfl.o build\miniz_zip.o ^
  build\app_res.o ^
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp ^
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp ^
  third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_dx11.cpp ^
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -limm32 ^
  -ladvapi32 ^
  -o build\SpaceSift.exe
if errorlevel 1 exit /b 1

REM 可选依赖：DWG 靠 dwg2dxf.exe（LibreDWG，GPLv3）+ libredwg-0.dll（~32 MB）
REM 把它们放在 third_party\libredwg\ 下就会被自动带上；没有就跳过 .dwg
if exist third_party\libredwg\dwg2dxf.exe (
  copy /y third_party\libredwg\dwg2dxf.exe build\ >nul
  copy /y third_party\libredwg\libredwg-0.dll build\ >nul
  copy /y third_party\libredwg\libiconv-2.dll build\ >nul
  echo [4/5] 已带上 DWG 转换器（LibreDWG）
) else (
  echo [4/5] 没有 third_party\libredwg，.dwg 不参与索引
)

REM PDF 支持靠运行时的 pdfium.dll（7.8 MB），没有它也能跑，只是搜不到 PDF 正文
if exist third_party\pdfium\bin\pdfium.dll (
  copy /y third_party\pdfium\bin\pdfium.dll build\ >nul
  echo [5/5] 已复制 pdfium.dll（PDF 内容检索可用）
) else (
  echo [5/5] 未找到 pdfium.dll，PDF 不参与索引
)

echo.
echo 构建完成: build\SpaceSift.exe
exit /b 0

:find_toolchain
where g++ >nul 2>nul
if not errorlevel 1 (
  echo 使用 PATH 中的 g++
  exit /b 0
)
if defined W64DEVKIT_DIR (
  set "PATH=%W64DEVKIT_DIR%\bin;%PATH%"
  where g++ >nul 2>nul
  if not errorlevel 1 (
    echo 使用 W64DEVKIT_DIR=%W64DEVKIT_DIR%
    exit /b 0
  )
)
if exist "%~dp0toolchain.txt" (
  set /p TOOLCHAIN_DIR=<"%~dp0toolchain.txt"
  if defined TOOLCHAIN_DIR (
    set "PATH=%TOOLCHAIN_DIR%\bin;%PATH%"
    where g++ >nul 2>nul
    if not errorlevel 1 (
      echo 使用 toolchain.txt: %TOOLCHAIN_DIR%
      exit /b 0
    )
  )
)
echo [错误] 找不到 g++。
echo        任选一种做法：
echo          1) 安装 MinGW-w64 并把 bin 加进 PATH
echo          2) setx W64DEVKIT_DIR "C:\path\to\w64devkit"
echo          3) 在 build.bat 同目录建 toolchain.txt，里面写工具链根目录（一行）
exit /b 1
