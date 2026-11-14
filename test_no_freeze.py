"""验收「切范围不再卡」：后台检索期间界面是否还在出帧。

判据不是"感觉快了"，而是：搜索进行中连拍两张，进度条区域必须有像素变化
（那条不确定进度条每帧都在动）。两张一模一样 = 界面被冻住了。
另外单独拍一张扫描中的顶栏，检查「暂停/结束」有没有被搜索组压住。
"""
import ctypes
import hashlib
import os
import subprocess
import sys
import time

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_scope_ui as T

ROOT = T.ROOT
EXE = os.path.join(ROOT, "build", "SpaceSift.exe")


def grab(tag):
    subprocess.run([sys.executable, "shot.py", "SpaceSift Native", tag],
                   cwd=ROOT, capture_output=True)
    p = os.path.join(ROOT, tag)
    return p if os.path.exists(p) else None


def region_hash(png, box):
    im = Image.open(png).convert("L").crop(box)
    return hashlib.md5(im.tobytes()).hexdigest()


def launch(**env_extra):
    T.kill()
    time.sleep(1.0)
    env = dict(os.environ)
    env.update(env_extra)
    subprocess.Popen([EXE], env=env, cwd=T.DIST, stdout=-3, stderr=-3)
    for _ in range(40):
        time.sleep(0.3)
        if T.find_window():
            return
    raise SystemExit("窗口没起来")


# ---------- 场景 1：切到「所有已索引」，搜索进行中界面仍在出帧 ----------
launch(SPACESIFT_ROOT="E:\\小论文1\\mvr_sim",
       SPACESIFT_AUTOSCAN="1",
       SPACESIFT_MODE="content",
       SPACESIFT_TAB="2",
       SPACESIFT_SCOPE="all",
       SPACESIFT_SEARCH="节能筏用电器")

time.sleep(1.2)
a = grab("freeze_a.png")
time.sleep(0.5)
b = grab("freeze_b.png")
if not a or not b:
    raise SystemExit("截图失败")

im = Image.open(a)
w, h = im.size
# 右栏（大约 x>w*0.62）中间那条不确定进度条，取一小块横向区域
box = (int(w * 0.64), int(h * 0.34), int(w * 0.98), int(h * 0.52))
h1, h2 = region_hash(a, box), region_hash(b, box)
print(f"窗口 {w}x{h}")
print(f"搜索中连拍两张，进度条区域哈希 {h1[:8]} / {h2[:8]} -> "
      f"{'界面在出帧（没冻）' if h1 != h2 else '两张一样，可能冻住了'}")

time.sleep(12.0)
c = grab("freeze_done.png")
print("结果截图:", c)

# ---------- 场景 2：扫描中的顶栏，暂停/结束 是否被压住 ----------
launch(SPACESIFT_ROOT="E:\\", SPACESIFT_AUTOSCAN="1")
time.sleep(4.0)
d = grab("toolbar_scan.png")
print("扫描中顶栏:", d)
T.kill()
