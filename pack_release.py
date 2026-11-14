"""打便携包 + exe-only 包 + SHA256SUMS，并在出包前做一次"出厂审计"。

三条硬规则（任一不满足就 exit 1，不产出 zip）：
  1. 白名单：包里只允许出现 EXPECTED_FILES / EXPECTED_DIRS 里列的东西，多一个都不行
     —— 防止某个索引库、截图、构建中间产物被顺手 copytree 带出去
  2. 内容扫描：任何文件里出现本机用户名 / C:\\Users\\xxx / 索引库文件名 / 真实盘符路径
     都算泄露，直接拒绝
  3. 依赖齐全 + 版本一致：便携包必须带 pdfium.dll / dwg2dxf.exe / tessdata，
     且 dist/SpaceSift.exe 里必须能读到当前版本号（否则包名会骗人）

版本号从 src/version.h 读，不在这里再写一遍。
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

# 便携包里允许出现的文件（多一个都算异常）
EXPECTED_FILES = {
    "SpaceSift.exe", "pdfium.dll", "dwg2dxf.exe", "libredwg-0.dll", "libiconv-2.dll",
    "USAGE.md", "LICENSE", "THIRD-PARTY-NOTICES.md", "install.bat", "install.ps1",
    "selftest.bat",
}
EXPECTED_DIRS = {"tessdata/", "licenses/"}

# 文档里允许出现的"示例路径"（占位符，不含任何真实目录名）
PATH_ALLOW = {
    r"D:\某目录",
    r"D:\某个目录",
    r"C:\path\to\w64devkit",
}


def version():
    src = open(os.path.join(ROOT, "src", "version.h"), encoding="utf-8").read()
    m = re.search(r'#define\s+SPACESIFT_VERSION\s+"([^"]+)"', src)
    if not m:
        sys.exit("src/version.h 里找不到 SPACESIFT_VERSION")
    return m.group(1)


VER = version()
NAME = f"SpaceSift-{VER}"


def die(msg, items=()):
    print("[审计失败] " + msg, file=sys.stderr)
    for it in items:
        print("    " + it, file=sys.stderr)
    sys.exit(1)


def check_dist():
    exe = os.path.join(DIST, "SpaceSift.exe")
    if not os.path.isfile(exe):
        die("dist/SpaceSift.exe 不存在，先跑 build.bat")
    data = open(exe, "rb").read()
    if VER.encode("utf-8") not in data and VER.encode("utf-16-le") not in data:
        die(f"dist/SpaceSift.exe 里读不到版本号 {VER}",
            ["先重新编译、复制到 dist/，再打包"])

    missing = [f for f in ("pdfium.dll", "dwg2dxf.exe", "libredwg-0.dll",
                           "libiconv-2.dll", "USAGE.md")
               if not os.path.isfile(os.path.join(DIST, f))]
    if not os.path.isdir(os.path.join(DIST, "tessdata")):
        missing.append("tessdata/")
    if missing:
        die("dist 缺依赖，先跑 build.bat 收集", missing)


def scan_leaks(stage):
    """扫正文和二进制，找本机痕迹。"""
    user = os.environ.get("USERNAME") or os.environ.get("USER") or ""
    pats = [
        ("Windows 用户目录", re.compile(rb"[A-Za-z]:[\\/]{1,2}Users[\\/]{1,2}[A-Za-z0-9_.$-]+")),
        ("mintty 风格路径", re.compile(rb"/[a-z]/Users/[A-Za-z0-9_.$-]+")),
        ("索引库文件名", re.compile(rb"(content|index)_[A-Za-z0-9_]{6,}\.db")),
        ("索引 WAL/SHM", re.compile(rb"\.db-(wal|shm)")),
        ("工作区路径", re.compile(rb"dev[\\/]SpaceSift-Native")),
    ]
    if user:
        pats.append(("本机用户名", re.compile(re.escape(user.encode("utf-8")))))
        pats.append(("本机用户名(UTF-16)", re.compile(re.escape(user.encode("utf-16-le")))))

    problems = []
    for base, _dirs, files in os.walk(stage):
        for f in files:
            full = os.path.join(base, f)
            rel = os.path.relpath(full, stage).replace("\\", "/")
            data = open(full, "rb").read()
            for name, pat in pats:
                m = pat.search(data)
                if m:
                    ctx = data[max(0, m.start() - 24): m.end() + 24]
                    problems.append(f"{rel}: 命中「{name}」→ {ctx.decode('utf-8', 'replace')!r}")
            if f.lower().endswith((".md", ".txt", ".bat", ".ps1")):
                text = data.decode("utf-8", "replace")
                for m in re.finditer(r"\b[A-Z]:\\[^\s\"'`<>|]*", text):
                    p = m.group(0).rstrip("）)，,。.;；")
                    if p in PATH_ALLOW or len(p) <= 3:
                        continue
                    problems.append(f"{rel}: 出现绝对路径 {p}")
    if problems:
        die("包内容疑似带了本机信息", problems)


def audit_tree(stage):
    """白名单校验：目录结构必须正好等于预期。"""
    actual_files, actual_dirs = set(), set()
    for base, _dirs, files in os.walk(stage):
        rel_base = os.path.relpath(base, stage).replace("\\", "/")
        if rel_base != ".":
            actual_dirs.add(rel_base + "/")
        for f in files:
            actual_files.add(os.path.relpath(os.path.join(base, f), stage).replace("\\", "/"))

    extra = [f for f in actual_files
             if f not in EXPECTED_FILES and not any(f.startswith(d) for d in EXPECTED_DIRS)]
    # licenses/ 下是纯许可文本，允许任意层级
    extra = [f for f in extra if not f.startswith("licenses/")]
    missing = [f for f in EXPECTED_FILES if f not in actual_files]
    if extra:
        die("包里出现了白名单之外的文件", extra)
    if missing:
        die("包里缺少必需文件", missing)

    for d in actual_dirs:
        if d in EXPECTED_DIRS or any(d.startswith(x) for x in EXPECTED_DIRS):
            continue
        die("包里出现了白名单之外的目录", [d])


def zipdir(src_dir, zip_path, arc_root):
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for base, _dirs, files in os.walk(src_dir):
            for f in files:
                full = os.path.join(base, f)
                rel = os.path.relpath(full, src_dir)
                z.write(full, os.path.join(arc_root, rel))


def sha(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    check_dist()

    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    stage = os.path.join(OUT, NAME)
    os.makedirs(stage)

    for f in ("SpaceSift.exe", "pdfium.dll", "dwg2dxf.exe", "libredwg-0.dll",
              "libiconv-2.dll", "USAGE.md"):
        shutil.copy2(os.path.join(DIST, f), os.path.join(stage, f))
    shutil.copytree(os.path.join(DIST, "tessdata"), os.path.join(stage, "tessdata"))
    for f in ("install.bat", "install.ps1", "selftest.bat", "THIRD-PARTY-NOTICES.md"):
        shutil.copy2(os.path.join(PKG, f), os.path.join(stage, f))
    shutil.copy2(os.path.join(ROOT, "LICENSE"), os.path.join(stage, "LICENSE"))
    shutil.copytree(os.path.join(PKG, "licenses"), os.path.join(stage, "licenses"))

    # 先审计，再压包：审计不过就不该有 zip 产物
    audit_tree(stage)
    scan_leaks(stage)
    print(f"[审计通过] 文件集合与白名单一致，共 {len(EXPECTED_FILES)} 个文件 + "
          f"{len(EXPECTED_DIRS)} 个目录；未发现本机路径/用户名/索引库")

    portable = os.path.join(OUT, f"{NAME}-portable.zip")
    zipdir(stage, portable, NAME)

    exe_only_dir = os.path.join(OUT, "exeonly", NAME)
    os.makedirs(exe_only_dir)
    shutil.copy2(os.path.join(DIST, "SpaceSift.exe"), os.path.join(exe_only_dir, "SpaceSift.exe"))
    shutil.copy2(os.path.join(DIST, "USAGE.md"), os.path.join(exe_only_dir, "USAGE.md"))
    shutil.copy2(os.path.join(PKG, "selftest.bat"), os.path.join(exe_only_dir, "selftest.bat"))
    exe_only = os.path.join(OUT, f"{NAME}-exe-only.zip")
    zipdir(exe_only_dir, exe_only, NAME)

    for zp in (portable, exe_only):
        with zipfile.ZipFile(zp) as z:
            names = [n for n in z.namelist() if not n.endswith("/")]
            bad = []
            for n in names:
                parts = n.split("/")
                # 允许 <包名>/<文件>、<包名>/tessdata/<语言包>、<包名>/licenses/**
                if len(parts) <= 2:
                    continue
                if parts[1] + "/" in EXPECTED_DIRS or parts[1] == "licenses":
                    continue
                bad.append(n)
            if bad:
                die(f"{os.path.basename(zp)} 里出现了预期之外的子目录", bad)
        print(f"[OK] {os.path.basename(zp)}: {len(names)} 个文件")

    lines = [f"{sha(p)}  {os.path.basename(p)}" for p in (portable, exe_only)]
    sums = os.path.join(OUT, "SHA256SUMS.txt")
    open(sums, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    for p in (portable, exe_only, sums):
        print(f"{os.path.getsize(p):>10}  {p}")
    print(open(sums, encoding="utf-8").read())


if __name__ == "__main__":
    main()
