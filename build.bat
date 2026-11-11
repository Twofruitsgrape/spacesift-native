@echo off
REM SpaceSift Native 构建脚本（需要 MinGW-w64 的 g++/gcc/windres）
setlocal

REM ---- 找编译器：PATH 优先，其次 W64DEVKIT_DIR 或 toolchain.txt ----
call :find_toolchain
if errorlevel 1 exit /b 1

if not exist build mkdir build
if not exist build\openswx mkdir build\openswx

REM 文件名留 fts5 后缀：带不带 FTS5 是两套二进制，避免旧目标文件被误用
if not exist build\sqlite3_fts5.o (
  echo [1/6] 编译 SQLite（含 FTS5）...
  gcc -O2 -c third_party/sqlite/sqlite-amalgamation-3450000/sqlite3.c -o build\sqlite3_fts5.o ^
    -DSQLITE_ENABLE_FTS5 ^
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 ^
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_EXPLAIN
  if errorlevel 1 exit /b 1
) else (
  echo [1/6] 复用已有的 build\sqlite3_fts5.o
)

if not exist build\miniz_zip.o (
  echo [2/6] 编译 miniz...
  for %%f in (miniz miniz_tdef miniz_tinfl miniz_zip) do (
    gcc -O2 -c third_party\miniz\%%f.c -o build\%%f.o
    if errorlevel 1 exit /b 1
  )
) else (
  echo [2/6] 复用已有的 miniz 目标文件
)

REM openswx 是 C++20 库，必须单独用 -std=c++20 编；主程序也用 C++20
if not exist build\openswx\document.o (
  echo [3/6] 编译 openswx（SolidWorks 解析，C++20）...
  g++ -O2 -std=c++20 -c third_party\pugixml\pugixml.cpp -o build\openswx\pugixml.o ^
    -I third_party\pugixml -I third_party\zlibshim -I third_party\miniz
  if errorlevel 1 exit /b 1
  g++ -O2 -std=c++20 -c third_party\openswx\src\document.cc -o build\openswx\document.o ^
    -I third_party\openswx\include -I third_party\openswx\src ^
    -I third_party\pugixml -I third_party\zlibshim -I third_party\miniz
  if errorlevel 1 exit /b 1
  for %%f in (decompressor modern_parser ole2_parser reference_reader sheet_name_reader xml_readers) do (
    g++ -O2 -std=c++20 -c third_party\openswx\src\internal\%%f.cc -o build\openswx\%%f.o ^
      -I third_party\openswx\include -I third_party\openswx\src ^
      -I third_party\pugixml -I third_party\zlibshim -I third_party\miniz
    if errorlevel 1 exit /b 1
  )
) else (
  echo [3/6] 复用已有的 openswx 目标文件
)

echo [4/6] 编译资源...
windres src\app.rc -o build\app_res.o
if errorlevel 1 exit /b 1

echo [5/6] 编译主程序...
g++ -O2 -std=c++20 -static -s -mwindows ^
  -I third_party/imgui -I third_party/imgui/backends ^
  -I third_party/sqlite/sqlite-amalgamation-3450000 -I third_party/miniz ^
  -I third_party/pdfium/include -I third_party/openswx/include -I src ^
  src/main.cpp src/db.cpp src/rules.cpp src/extract.cpp src/content.cpp src/external.cpp ^
  build/sqlite3_fts5.o ^
  build/miniz.o build/miniz_tdef.o build/miniz_tinfl.o build/miniz_zip.o ^
  build/openswx/pugixml.o build/openswx/document.o build/openswx/decompressor.o ^
  build/openswx/modern_parser.o build/openswx/ole2_parser.o ^
  build/openswx/reference_reader.o build/openswx/sheet_name_reader.o ^
  build/openswx/xml_readers.o ^
  build/app_res.o ^
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp ^
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp ^
  third_party/imgui/backends/imgui_impl_win32.cpp third_party/imgui/backends/imgui_impl_dx11.cpp ^
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -limm32 ^
  -ladvapi32 ^
  -o build\SpaceSift.exe
if errorlevel 1 exit /b 1

echo [6/6] 收集运行时依赖...
REM 可选依赖：DWG 靠 dwg2dxf.exe（LibreDWG，GPLv3）+ libredwg-0.dll（~32 MB）
if exist third_party\libredwg\dwg2dxf.exe (
  copy /y third_party\libredwg\dwg2dxf.exe build\ >nul
  copy /y third_party\libredwg\libredwg-0.dll build\ >nul
  copy /y third_party\libredwg\libiconv-2.dll build\ >nul
  echo    + DWG 转换器（LibreDWG）
) else (
  echo    - 没有 third_party\libredwg，.dwg 不参与索引
)

REM PDF 靠运行时的 pdfium.dll（7.8 MB）
if exist third_party\pdfium\bin\pdfium.dll (
  copy /y third_party\pdfium\bin\pdfium.dll build\ >nul
  echo    + pdfium.dll（PDF）
) else (
  echo    - 未找到 pdfium.dll，PDF 不参与索引
)

REM OCR 语言包放 build\tessdata（没有就用系统装的 Tesseract）
if exist third_party\tessdata (
  if not exist build\tessdata mkdir build\tessdata
  copy /y third_party\tessdata\*.traineddata build\tessdata\ >nul
  echo    + tessdata 语言包（OCR）
) else (
  echo    - 没有 third_party\tessdata，OCR 用系统安装的 Tesseract
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
