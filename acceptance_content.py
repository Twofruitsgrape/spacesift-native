"""内容检索页验收：内容模式 → 检索结果 → 搜关键词 → 右键命中 → 打开所在位置。"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
ROOT = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(ROOT, "build", "SpaceSift.exe")
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


def logtxt():
    return open(DEFAULT_LOG, encoding="utf-8", errors="replace").read() if os.path.exists(DEFAULT_LOG) else ""


def shot(name):
    subprocess.run([sys.executable, "shot.py", "SpaceSift", name], cwd=ROOT, capture_output=True)


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

hwnd = find_window()
r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
ox, oy = r.left, r.top
from winmsg import App as WM
app = WM()


def click(x, y, right=False, settle=0.8):
    u.ShowWindow(hwnd, 9); u.SetForegroundWindow(hwnd); time.sleep(0.3)
    u.SetCursorPos(ox + x, oy + y); time.sleep(0.3)
    a, b = (RD, RU) if right else (LD, LU)
    u.mouse_event(a, 0, 0, 0, 0); time.sleep(0.09)
    u.mouse_event(b, 0, 0, 0, 0); time.sleep(settle)


# 内容模式
click(795, 65)
time.sleep(1.0)
shot("c_0_mode.png")
# 检索结果页签（内容模式整体下移）
click(1036, 275)
time.sleep(1.0)
shot("c_1_tab.png")
# 搜关键词
click(950, 65)
app.text("DWG")
click(1148, 65)
time.sleep(2.5)
shot("c_2_hits.png")
n0 = len(logtxt())
click(1055, 388, right=True)      # 第一条命中
shot("c_3_menu.png")
click(1130, 411)
time.sleep(2.5)
fg = u.GetForegroundWindow()
new = [x for x in logtxt()[n0:].strip().split("\n") if x]
print("内容命中 reveal 之后前台:", cls_of(fg))
for x in new:
    print("   ", x)
ok = cls_of(fg) == "CabinetWClass" and any(("activate" in x and "ok=1" in x) or "#1 ok" in x for x in new)
print("结论:", "通过" if ok else "失败")
kill()
sys.exit(0 if ok else 1)
