"""复现用户场景：Downloads 里的 exe（普通权限）+ D:\\cigarette + 内容搜索 → 打开所在位置。

只按用户的操作路径点，最后读 reveal.log 看每一级错误码。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
LD, LU, RD, RU = 0x0002, 0x0004, 0x0008, 0x0010
EXE = r"C:\Users\ALICE\Downloads\SpaceSift-2.1.0-portable\SpaceSift-2.1.0\SpaceSift.exe"
LOG = os.path.join(os.environ["LOCALAPPDATA"], "SpaceSift", "reveal.log")


def find_window():
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        n = u.GetWindowTextLengthW(h)
        if n and u.IsWindowVisible(h):
            b = ctypes.create_unicode_buffer(n + 1)
            u.GetWindowTextW(h, b, n + 1)
            if b.value == "SpaceSift Native":
                found.append(h)
        return True

    u.EnumWindows(cb, 0)
    return found[0] if found else 0


if os.path.exists(LOG):
    os.remove(LOG)
subprocess.Popen([EXE], env=dict(os.environ, SPACESIFT_ROOT=r"D:\cigarette", SPACESIFT_AUTOSCAN="1"),
                 cwd=os.path.dirname(EXE))
for _ in range(30):
    time.sleep(0.4)
    if find_window():
        break
time.sleep(9)
hwnd = find_window()
r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
ox, oy = r.left, r.top
from winmsg import App as WM
app = WM()
u.ShowWindow(hwnd, 9); u.SetForegroundWindow(hwnd); time.sleep(0.5)


def click(x, y, right=False, settle=0.8):
    u.SetCursorPos(ox + x, oy + y); time.sleep(0.25)
    a, b = (RD, RU) if right else (LD, LU)
    u.mouse_event(a, 0, 0, 0, 0); time.sleep(0.09)
    u.mouse_event(b, 0, 0, 0, 0); time.sleep(settle)


click(820, 53)          # 内容模式
time.sleep(1)
click(1042, 292)        # 检索结果页签
click(950, 53)
app.text("唐伟")
click(1160, 53)         # 搜索
time.sleep(2.5)
subprocess.run([sys.executable, "shot.py", "SpaceSift Native", "repro_show_hits.png"], capture_output=True)
click(1055, 388, right=True)
subprocess.run([sys.executable, "shot.py", "SpaceSift Native", "repro_show_menu.png"], capture_output=True)
n0 = len(open(LOG, encoding="utf-8", errors="replace").read()) if os.path.exists(LOG) else 0
click(1130, 411)        # 打开所在位置
time.sleep(2.5)
txt = open(LOG, encoding="utf-8", errors="replace").read() if os.path.exists(LOG) else ""
print("新增日志:")
print(txt[n0:] or "(无)")
subprocess.run([sys.executable, "shot.py", "SpaceSift Native", "repro_show_after.png"], capture_output=True)
