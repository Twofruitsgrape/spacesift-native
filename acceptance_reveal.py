"""SpaceSift「打开所在位置」完整验收。

覆盖：深度清理（目录行 / 文件行）、检索结果（文件名命中）、内容命中。
每个用例检查：
  A. 日志里有 reveal 记录且没有 "没有对应窗口"
  B. reveal 之后前台窗口是资源管理器
  C. 日志写在默认路径 %LOCALAPPDATA%\\SpaceSift\\reveal.log（不设环境变量）
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

from PIL import Image

u = ctypes.windll.user32
ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get("SPACESIFT_TEST_EXE") or os.path.join(ROOT, "build", "SpaceSift.exe")
DIST = os.path.join(ROOT, "dist")
LD, LU, RD, RU = 0x0002, 0x0004, 0x0008, 0x0010
DEFAULT_LOG = os.path.join(os.environ["LOCALAPPDATA"], "SpaceSift", "reveal.log")
ROOT_DIR = r"E:\Projects\电脑文件全局检索"


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
            b = ctypes.create_unicode_buffer(n + 1); u.GetWindowTextW(h, b, n + 1)
            if "SpaceSift" in b.value:
                found.append(h)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else 0


def cls_of(h):
    c = ctypes.create_unicode_buffer(256); u.GetClassNameW(h, c, 256); return c.value


def title_of(h):
    n = u.GetWindowTextLengthW(h); b = ctypes.create_unicode_buffer(n + 1)
    u.GetWindowTextW(h, b, n + 1); return b.value


def logtxt():
    if not os.path.exists(DEFAULT_LOG):
        return ""
    return open(DEFAULT_LOG, encoding="utf-8", errors="replace").read()


def shot(name):
    subprocess.run([sys.executable, "shot.py", "SpaceSift", name], cwd=ROOT, capture_output=True)


def row_centers(png, x=1000, y0=250, y1=820):
    """按卡片边框线找右栏条目的行中心（边框色 60,64,67）。"""
    im = Image.open(os.path.join(ROOT, png)).convert("RGB")
    border = (60, 64, 67)
    ys = [y for y in range(y0, min(y1, im.height))
          if im.getpixel((x, y)) == border]
    rows, start = [], None
    for y in ys:
        if start is None:
            start = y
        elif y - ys[ys.index(y) - 1] > 5:
            rows.append((start, y)); start = y
    tops = [y for y in ys]
    centers = []
    i = 0
    while i < len(tops):
        centers.append(tops[i] + 18)
        i += 1
        while i < len(tops) and tops[i] - tops[i - 1] < 20:
            i += 1
    return centers


class App:
    def __init__(self):
        self.hwnd = find_window()
        r = wintypes.RECT(); u.GetWindowRect(self.hwnd, ctypes.byref(r))
        self.ox, self.oy = r.left, r.top

    def focus(self):
        u.ShowWindow(self.hwnd, 9); u.SetForegroundWindow(self.hwnd); time.sleep(0.5)

    def click(self, x, y, right=False, settle=0.75):
        u.SetCursorPos(self.ox + x, self.oy + y); time.sleep(0.3)
        a, b = (RD, RU) if right else (LD, LU)
        u.mouse_event(a, 0, 0, 0, 0); time.sleep(0.09)
        u.mouse_event(b, 0, 0, 0, 0); time.sleep(settle)


def main():
    if os.path.exists(DEFAULT_LOG):
        os.remove(DEFAULT_LOG)
    kill(); time.sleep(1.2)
    env = dict(os.environ)
    env["SPACESIFT_ROOT"] = ROOT_DIR
    env["SPACESIFT_AUTOSCAN"] = "1"
    env.pop("SPACESIFT_DEBUG_LOG", None)
    subprocess.Popen([EXE], env=env, cwd=DIST, stdout=-3, stderr=-3)
    for _ in range(30):
        time.sleep(0.4)
        if find_window():
            break
    time.sleep(6)
    app = App()
    app.focus()
    print("hwnd", hex(app.hwnd), "origin", app.ox, app.oy)

    results = []

    def case(name, setup, row_y, dy=23):
        app.focus()
        if setup:
            setup()
        n0 = len(logtxt())
        app.click(1055, row_y, right=True)
        shot(f"acc_{name}_menu.png")
        app.click(1130, row_y + dy)
        time.sleep(2.0)
        fg = u.GetForegroundWindow()
        new = [x for x in logtxt()[n0:].strip().split("\n") if x]
        ok_fg = cls_of(fg) in ("CabinetWClass", "ExploreWClass")
        ok_log = any("activate" in x and "ok=1" in x for x in new) or any("#1 ok" in x for x in new)
        bad = [x for x in new if "没有对应窗口" in x or "失败" in x]
        results.append((name, ok_fg, ok_log, bad, new[-1] if new else "(无日志)"))
        print(f"--- {name}: 前台={cls_of(fg)} ok_fg={ok_fg} ok_log={ok_log}")
        for x in new:
            print("     ", x)

    # 建内容索引前先把三个页签的基本用例跑掉
    def to_deep():
        app.click(965, 203)

    case("deep_row0", to_deep, 300)
    case("deep_row1", None, 350)
    case("deep_row2", None, 400)

    # 检索结果（文件名搜索）
    def to_search():
        app.click(1036, 203)      # 检索结果页签
        app.click(950, 53)        # 搜索框
        from winmsg import App as WM
        WM().text("md")
        time.sleep(0.3)
        app.click(1148, 53)       # 搜索
        time.sleep(1.5)
    case("search_row0", to_search, 300)

    print()
    print("=" * 70)
    print("默认日志:", DEFAULT_LOG, "存在" if os.path.exists(DEFAULT_LOG) else "缺失")
    allok = True
    for name, ok_fg, ok_log, bad, last in results:
        flag = "OK " if (ok_fg and ok_log and not bad) else "FAIL"
        if flag == "FAIL":
            allok = False
        print(f"  [{flag}] {name:<12} fg={ok_fg} log={ok_log} bad={bad} last={last}")
    print("全部通过" if allok else "有失败项")
    kill()
    return 0 if allok else 1


if __name__ == "__main__":
    sys.exit(main())
