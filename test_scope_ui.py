"""界面验收：内容检索的三种作用域场景，逐张截图。

A 当前目录有索引      -> 正常命中
B 当前目录无索引      -> 明确提示"还没建索引"，而不是 0 条
C 无索引 + 范围=所有  -> 跨目录命中（原来这里是死路）

用法: python test_scope_ui.py
"""
import ctypes
import os
import subprocess
import sys
import time
from ctypes import wintypes

u = ctypes.windll.user32
ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(ROOT, "build", "SpaceSift.exe")
DIST = os.path.join(ROOT, "dist")
QUERY = "横管降膜"


def kill():
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-Process -Name SpaceSift* -ErrorAction SilentlyContinue | Stop-Process -Force"],
                   capture_output=True)


def find_window():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        n = u.GetWindowTextLengthW(h)
        if n and u.IsWindowVisible(h):
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if b.value.startswith("SpaceSift Native"):
                found.append(h)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else 0


def run(tag, root, scope_all=False, wait=9.0):
    kill()
    time.sleep(1.0)
    env = dict(os.environ)
    env["SPACESIFT_ROOT"] = root
    env["SPACESIFT_AUTOSCAN"] = "1"
    env["SPACESIFT_MODE"] = "content"
    env["SPACESIFT_TAB"] = "2"
    env["SPACESIFT_SEARCH"] = QUERY
    if scope_all:
        env["SPACESIFT_SCOPE"] = "all"
    else:
        env.pop("SPACESIFT_SCOPE", None)
    subprocess.Popen([EXE], env=env, cwd=DIST, stdout=-3, stderr=-3)
    for _ in range(40):
        time.sleep(0.3)
        if find_window():
            break
    time.sleep(wait)
    subprocess.run([sys.executable, "shot.py", "SpaceSift Native", f"{tag}.png"],
                   cwd=ROOT, capture_output=True)
    print("  shot:", tag)
    kill()
    time.sleep(0.6)


if __name__ == "__main__":
    run("scope_a_indexed", r"E:\小论文1\mvr_sim")
    run("scope_b_noindex", r"E:\others", wait=7.0)
    run("scope_c_all", r"E:\others", scope_all=True, wait=8.0)
