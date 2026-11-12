"""交互回归：搜索回车 / 行勾选 + 清理确认弹窗 / 下钻 + 上一层 / 窄窗口布局。

只在界面上点，不真的删文件（弹窗里按 Esc 取消）。
"""
import ctypes, os, subprocess, sys, time
from ctypes import wintypes

u = ctypes.windll.user32
ROOT = os.path.dirname(os.path.abspath(__file__))
LD, LU = 0x0002, 0x0004
TAB_RECO, TAB_DEEP, TAB_SEARCH = 913, 976, 1039
TAB_Y = 222
ROW1, ROW2, ROW3 = 374, 422, 470
CHK_X = 866


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


def shot(name):
    subprocess.run([sys.executable, "shot.py", "SpaceSift Native", name], cwd=ROOT,
                   capture_output=True)
    print("   shot:", name)


hwnd = find_window()
if not hwnd:
    print("no window")
    sys.exit(1)
u.ShowWindow(hwnd, 9)
u.SetForegroundWindow(hwnd)
time.sleep(0.6)
r = wintypes.RECT(); u.GetWindowRect(hwnd, ctypes.byref(r))
ox, oy = r.left, r.top
from winmsg import App as WM
app = WM()


def click(x, y, settle=0.7):
    u.SetCursorPos(ox + x, oy + y)
    time.sleep(0.25)
    u.mouse_event(LD, 0, 0, 0, 0)
    time.sleep(0.08)
    u.mouse_event(LU, 0, 0, 0, 0)
    time.sleep(settle)


def key(vk):
    u.keybd_event(vk, 0, 0, 0)
    time.sleep(0.05)
    u.keybd_event(vk, 0, 2, 0)
    time.sleep(0.4)


VK_RETURN, VK_ESCAPE = 0x0D, 0x1B

# 1) 文件名搜索：只用回车
click(TAB_SEARCH, TAB_Y)
click(950, 53)
app.text("readme")
key(VK_RETURN)
time.sleep(1.2)
shot("s1_search_enter.png")

# 2) 勾选 + 清理确认弹窗（Esc 取消，不删）
click(TAB_DEEP, TAB_Y)
click(CHK_X, ROW1)
click(CHK_X, ROW2)
shot("s2_selected.png")
click(1240, 828)          # 清理选中项
time.sleep(0.8)
shot("s3_confirm.png")
key(VK_ESCAPE)
time.sleep(0.6)
shot("s4_cancelled.png")

# 3) 下钻：点第二行（目录）→ 面包屑变化 → 上一层
click(1055, ROW2)
time.sleep(1.0)
shot("s5_drill.png")
click(1180, 262)          # 上一层（面包屑右侧文字链）
time.sleep(1.0)
shot("s6_back.png")

# 4) 窄窗口
u.SetWindowPos(hwnd, 0, ox - 60, oy - 40, 980, 760, 0x0004)
time.sleep(1.5)
shot("s7_narrow.png")
u.SetWindowPos(hwnd, 0, ox, oy, 1320, 880, 0x0004)
time.sleep(1.0)
shot("s8_restored.png")
print("done")
