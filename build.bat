@echo off
REM SpaceSift Native 构建脚本（需要 MinGW-w64 的 g++ 在 PATH 里）
setlocal

if not exist build mkdir build

if not exist build\sqlite3.o (
  echo [1/2] 编译 SQLite...
  gcc -O2 -c third_party/sqlite/sqlite-amalgamation-3450000/sqlite3.c -o build\sqlite3.o ^
    -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_THREADSAFE=1 -DSQLITE_DEFAULT_MEMSTATUS=0 ^
    -DSQLITE_OMIT_DEPRECATED -DSQLITE_DQS=0 -DSQLITE_OMIT_EXPLAIN
  if errorlevel 1 exit /b 1
) else (
  echo [1/2] 复用已有的 build\sqlite3.o
)

echo [2/2] 编译资源与主程序...
windres src\app.rc -o build\app_res.o
if errorlevel 1 exit /b 1

g++ -O2 -std=c++17 -static -s -mwindows ^
  -I third_party/imgui -I third_party/imgui/backends ^
  -I third_party/sqlite/sqlite-amalgamation-3450000 -I src ^
  src/main.cpp src/db.cpp src/rules.cpp src/extract.cpp src/content.cpp ^
  build/sqlite3.o build/app_res.o ^
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp ^
  third_party/imgui/imgui_tables.cpp third_party/imgui/imgui_widgets.cpp ^
  third_party/imgui/backends/imgui_impl_win32.cpp ^
  third_party/imgui/backends/imgui_impl_dx11.cpp ^
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -limm32 ^
  -o build\SpaceSift.exe
if errorlevel 1 exit /b 1

echo.
echo 构建完成: build\SpaceSift.exe
