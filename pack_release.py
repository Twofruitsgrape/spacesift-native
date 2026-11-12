"""打 v2.0.6 便携包 + exe-only 包 + SHA256SUMS。"""
import hashlib, os, shutil, zipfile

ROOT = r"C:\Users\ALICE\dev\SpaceSift-Native"
DIST = os.path.join(ROOT, "dist")
PKG = os.path.join(ROOT, "packaging")
OUT = os.path.join(ROOT, "build", "pkg")
VER = "2.1.0"
NAME = f"SpaceSift-{VER}"

if os.path.isdir(OUT):
    shutil.rmtree(OUT)
stage = os.path.join(OUT, NAME)
os.makedirs(stage)

# 便携包内容
for f in ["SpaceSift.exe", "dwg2dxf.exe", "libiconv-2.dll", "libredwg-0.dll",
          "pdfium.dll", "USAGE.md"]:
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
