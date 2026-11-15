# -*- coding: utf-8 -*-
"""按 content.cpp 的字节级规则算 DbPathFor（C 的 isalnum 对非 ASCII 返回假）。"""
import os
import sys

DATA_DIR = r"C:\Users\ALICE\AppData\Local\SpaceSiftNative"


def fnv1a_bytes(b: bytes) -> int:
    h = 2166136261
    for c in b:
        h ^= c
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def norm_root(s: str) -> str:
    s = s.replace("/", "\\")
    if len(s) >= 2 and s[1] == ":" and "a" <= s[0] <= "z":
        s = s[0].upper() + s[1:]
    while len(s) > 3 and s.endswith("\\"):
        s = s[:-1]
    if len(s) == 2 and s[1] == ":":
        s += "\\"
    return s


def db_path_for(root: str) -> str:
    norm = norm_root(root)
    nb = norm.encode("utf-8")
    safe = bytes((c if (chr(c).isalnum() and c < 128) or c == 0x5F else 0x5F) for c in nb)
    return os.path.join(DATA_DIR, "content_" + safe.decode()[:33] + "_%08x.db" % fnv1a_bytes(nb))


if __name__ == "__main__":
    root = sys.argv[1] if len(sys.argv) > 1 else "D:\\隔膜压缩机"
    print(db_path_for(root))
