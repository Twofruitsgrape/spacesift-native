#!/bin/bash
# SpaceSift 快速重编（只重编主程序，复用 obj）
set -e
export PATH="/c/Users/ALICE/.local/toolchains/w64devkit/bin:$PATH"
cd "C:/Users/ALICE/dev/SpaceSift-Native"
g++ -O2 -std=c++20 -static -s -mwindows \
  -I third_party/imgui -I third_party/imgui/backends \
  -I third_party/sqlite/sqlite-amalgamation-3450000 -I third_party/miniz \
  -I third_party/pdfium/include -I third_party/openswx/include -I src \
  src/main.cpp src/db.cpp src/rules.cpp src/extract.cpp src/office.cpp src/content.cpp \
  src/external.cpp src/ime_win32.cpp src/selftest.cpp \
  build/sqlite3_fts5.o build/miniz.o build/miniz_tdef.o build/miniz_tinfl.o build/miniz_zip.o \
  build/openswx/pugixml.o build/openswx/document.o build/openswx/decompressor.o \
  build/openswx/modern_parser.o build/openswx/ole2_parser.o build/openswx/reference_reader.o \
  build/openswx/sheet_name_reader.o build/openswx/xml_readers.o build/app_res.o \
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp \
  third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_win32.cpp \
  third_party/imgui/backends/imgui_impl_dx11.cpp \
  -ld3d11 -ldxgi -ld3dcompiler -ldwmapi -lgdi32 -luser32 -lshell32 -lole32 -loleaut32 -luuid -limm32 -ladvapi32 \
  -o build/SpaceSift.exe "$@" 2>&1 | grep -v "^$" | head -30
ls -la build/SpaceSift.exe
