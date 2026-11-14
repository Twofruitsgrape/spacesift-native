"""截图脚本：深度清理 / 检索结果 / 亮色，用于改版前后对比。

用法: python ui_shot.py after
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get("SPACESIFT_TEST_EXE") or os.path.join(ROOT, "build", "SpaceSift.exe")
DIST = os.environ.get("SPACESIFT_TEST_CWD") or os.path.join(ROOT, "dist")
LD, LU = 0x0002, 0x0004
TAG = sys.argv[1] if len(sys.argv) > 1 else "shot"

TAB_RECO, TAB_DEEP, TAB_SEARCH = 913, 976, 1039
TAB_Y = 222
SEARCH_X, SEARCH_Y = 950, 53
GO_X = 1160
THEME_X, THEME_Y = 1200, 53
THEME_DARK_X = 1275


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


def shot(name):
    subprocess.run([sys.executable, "shot.py", "SpaceSift Native", name], cwd=ROOT,
                   capture_output=True)
    print("  ", name)


kill()
time.sleep(1.2)
env = dict(os.environ)
env["SPACESIFT_ROOT"] = r"E:\Projects\电脑文件全局检索"
env["SPACESIFT_AUTOSCAN"] = "1"
env.pop("SPACESIFT_DEBUG_LOG", None)
subprocess.Popen([EXE], env=env, cwd=DIST, stdout=-3, stderr=-3)
for _ in range(30):
    time.sleep(0.4)
    if find_window():
        break
time.sleep(7)
hwnd = find_window()
r = wintypes.RECT()
u.GetWindowRect(hwnd, ctypes.byref(r))
ox, oy = r.left, r.top
from winmsg import App as WM
app = WM()


def click(x, y, settle=0.8):
    u.ShowWindow(hwnd, 9)
    u.SetForegroundWindow(hwnd)
    time.sleep(0.25)
    u.SetCursorPos(ox + x, oy + y)
    time.sleep(0.25)
    u.mouse_event(LD, 0, 0, 0, 0)
    time.sleep(0.09)
    u.mouse_event(LU, 0, 0, 0, 0)
    time.sleep(settle)


click(TAB_DEEP, TAB_Y)
shot(f"{TAG}_deep.png")

click(TAB_SEARCH, TAB_Y)
click(SEARCH_X, SEARCH_Y)
app.text("md")
click(GO_X, SEARCH_Y)
time.sleep(1.5)
shot(f"{TAG}_search.png")

click(THEME_X, THEME_Y)
time.sleep(1.2)
shot(f"{TAG}_light.png")

click(THEME_DARK_X, THEME_Y)
time.sleep(0.8)
click(TAB_RECO, TAB_Y)
shot(f"{TAG}_reco.png")
kill()
