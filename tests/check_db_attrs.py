# -*- coding: utf-8 -*-
"""按 content.cpp 的规则复算 DbPathFor，并检查目标文件属性/ACL。"""
import ctypes
import os
import stat
import subprocess

DATA_DIR = r"C:\Users\ALICE\AppData\Local\SpaceSiftNative"


def fnv1a(s: str) -> int:
    h = 2166136261
    for b in s.encode():
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def norm_root(s: str) -> str:
    s = s.replace("/", "\\")
    if len(s) >= 2 and s[1] == ":" and s[0].islower():
        s = s[0].upper() + s[1:]
    while len(s) > 3 and s.endswith("\\"):
        s = s[:-1]
    if len(s) == 2 and s[1] == ":":
        s += "\\"
    return s


def db_path_for(root: str) -> str:
    norm = norm_root(root)
    safe = "".join(c if (c.isalnum() or c == "_") else "_" for c in norm)
    safe = safe[:33]
    return os.path.join(DATA_DIR, "content_%s_%08x.db" % (safe, fnv1a(norm)))


def describe(p: str) -> None:
    print("路径:", p)
    if not os.path.exists(p):
        print("  [不存在]")
        return
    st = os.stat(p)
    print("  大小:", st.st_size, " 属性:", hex(st.st_file_attributes) if hasattr(st, "st_file_attributes") else "?")
    attrs = st.st_file_attributes if hasattr(st, "st_file_attributes") else 0
    if attrs & stat.FILE_ATTRIBUTE_READONLY:
        print("  >>> 只读属性已置位 <<<")
    if attrs & stat.FILE_ATTRIBUTE_DIRECTORY:
        print("  >>> 这是个目录，不是文件! <<<")
    # 尝试读写打开
    try:
        with open(p, "r+b"):
            print("  Python 读写打开: 成功")
    except Exception as e:
        print("  Python 读写打开: 失败 ->", e)
    # ACL
    r = subprocess.run(["icacls", p], capture_output=True, text=True, errors="replace")
    print("  icacls:", " | ".join(x.strip() for x in r.stdout.splitlines()[1:]))


for root in [r"D:\隔膜压缩机", "D:/隔膜压缩机", r"D:\隔膜压缩机\\", r"d:\隔膜压缩机"]:
    describe(db_path_for(root))

print("\n=== 数据目录下所有只读或可疑的 content 库 ===")
for name in sorted(os.listdir(DATA_DIR)):
    p = os.path.join(DATA_DIR, name)
    if not name.startswith("content_") or not name.endswith(".db"):
        continue
    st = os.stat(p)
    attrs = getattr(st, "st_file_attributes", 0)
    if attrs & stat.FILE_ATTRIBUTE_READONLY or st.st_size == 0:
        print("  %-46s size=%-10d readonly=%s" % (name, st.st_size, bool(attrs & stat.FILE_ATTRIBUTE_READONLY)))

print("\n=== E:\\comsol 那个库（对照组，健康的） ===")
for name in sorted(os.listdir(DATA_DIR)):
    if "comsol_" in name and name.endswith(".db"):
        p = os.path.join(DATA_DIR, name)
        st = os.stat(p)
        print("  %-40s size=%d attrs=%s" % (name, st.st_size, hex(getattr(st, "st_file_attributes", 0))))
