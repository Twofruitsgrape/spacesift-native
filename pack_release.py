"""打便携包 + exe-only 包 + SHA256SUMS。

版本号从 src/version.h 读，不在这里再写一遍（之前 README 2.0.0、发行页 2.1.1、
脚本注释里 2.0.6，全靠手改总会漏）。

缺依赖直接报错退出，而不是打一个"看起来完整、但 PDF/OCR 用不了"的包 ——
exe-only 包允许缺 dll，因为它本来就不带；便携包必须齐全。
"""
import hashlib
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(ROOT, "dist")
PKG = os.path.join(ROOT, "packaging")
OUT = os.path.join(ROOT, "build", "pkg")


def version():
    src = open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+SPACESIFT_VERSION\s+"([^"]+)"', src)
    if not m:
        sys.exit("src/version.h 里找不到 SPACESIFT_VERSION")
    return m.group(1)


VER = version()
NAME = f"SpaceSift-{VER}"

# 便携包必须齐全：主程序 + PDF + DWG + OCR 语言包
REQUIRED = ["SpaceSift.exe", "pdfium.dll", "dwg2dxf.exe", "libredwg-0.dll",
            "libiconv-2.dll"]
missing = [f for f in REQUIRED if not os.path.isfile(os.path.join(DIST, f))]
if not os.path.isdir(os.path.join(DIST, "tessdata")):
    missing.append("tessdata/")
if missing:
    sys.exit("dist 缺依赖，先跑 build.bat 收集：" + ", ".join(missing))

if os.path.isdir(OUT):
    shutil.rmtree(OUT)
stage = os.path.join(OUT, NAME)
os.makedirs(stage)

for f in REQUIRED + ["USAGE.md"]:
    shutil.copy2(os.path.join(DIST, f), os.path.join(stage, f))
shutil.copytree(os.path.join(DIST, "tessdata"), os.path.join(stage, "tessdata"))
for f in ["install.bat", "install.ps1", "THIRD-PARTY-NOTICES.md"]:
    shutil.copy2(os.path.join(PKG, f), os.path.join(stage, f))
shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(stage, "LICENSE"))
shutil.copytree(os.path.join(PKG, "licenses"), os.path.join(stage, "licenses"))


def zipdir(src_dir, zip_path, arc_root):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _dirs, files in os.walk(src_dir):
            for f in files:
                full = os.path.join(base, f)
                rel = os.path.relpath(full, src_dir)
                z.write(full, os.path.join(arc_root, rel))


portable = os.path.join(OUT, f"{NAME}-portable.zip")
zipdir(stage, portable, NAME)

exe_only_dir = os.path.join(OUT, "exeonly", NAME)
os.makedirs(exe_only_dir)
shutil.copy2(os.path.join(DIST, "SpaceSift.exe"), os.path.join(exe_only_dir, "SpaceSift.exe"))
shutil.copy2(os.path.join(DIST, "USAGE.md"), os.path.join(exe_only_dir, "USAGE.md"))
exe_only = os.path.join(OUT, f"{NAME}-exe-only.zip")
zipdir(exe_only_dir, exe_only, NAME)


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


lines = []
for p in (portable, exe_only):
    lines.append(f"{sha(p)}  {os.path.basename(p)}")
sums = os.path.join(OUT, "SHA256SUMS.txt")
open(sums, "w", encoding="utf-8").write("\n".join(lines) + "\n")
for p in (portable, exe_only, sums):
    print(f"{os.path.getsize(p):>10}  {p}")
print(open(sums, encoding="utf-8").read())
