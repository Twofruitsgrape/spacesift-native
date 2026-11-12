"""直接向 ImGui 窗口投递 Windows 消息来做 UI 测试。

为什么不用 SendInput / pyautogui：
  - 需要窗口在前台、抢焦点，Windows 前台锁经常失败；
  - 坐标要换算屏幕/客户区/DPI，容易点偏；
  - 键盘会被中文输入法吞掉（打 mvr 变成「大」）。
直接 PostMessage 到 HWND 则完全绕开这些：ImGui 的 win32 后端在 WndProc 里
处理 WM_MOUSEMOVE / WM_LBUTTONDOWN / WM_CHAR，与是否前台无关。

用法：
    from winmsg import App
    app = App()                    # 按标题找已运行的 SpaceSift
    app.click(975, 30)             # 客户区坐标
    app.text("MVR")                # ASCII 文本（走 WM_CHAR，不过输入法）
    app.key(0x08)                  # VK_BACK
"""
import ctypes
import time
from ctypes import wintypes

user32 = ctypes.windll.user32

WM_MOUSEMOVE = 0x0200
WM_LBUTTONDOWN = 0x0201
WM_LBUTTONUP = 0x0202
WM_CHAR = 0x0102
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101

MK_LBUTTON = 0x0001


def _lparam(x, y):
    return ((y & 0xFFFF) << 16) | (x & 0xFFFF)


class App:
    def __init__(self, title="SpaceSift", pid=None):
        self.hwnd = self._find(title, pid)
        if not self.hwnd:
            raise RuntimeError("找不到窗口")

    @staticmethod
    def _find(title, pid):
        found = []

        @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
        def cb(hwnd, _):
            if not user32.IsWindowVisible(hwnd):
                return True
            n = user32.GetWindowTextLengthW(hwnd)
            if n == 0:
                return True
            buf = ctypes.create_unicode_buffer(n + 1)
            user32.GetWindowTextW(hwnd, buf, n + 1)
            if title not in buf.value:
                return True
            if pid is not None:
                p = wintypes.DWORD()
                user32.GetWindowThreadProcessId(hwnd, ctypes.byref(p))
                if p.value != pid:
                    return True
            r = wintypes.RECT()
            user32.GetWindowRect(hwnd, ctypes.byref(r))
            if r.right - r.left > 200:
                found.append(hwnd)
            return True

        user32.EnumWindows(cb, 0)
        return found[0] if found else None

    # ---- 输入 ----
    def move(self, x, y):
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, _lparam(x, y))
        time.sleep(0.06)

    def move_to_corner(self, w=1300, h=860):
        """应用会恢复上次窗口位置（可能跑到屏幕外），挪回左上角，否则合成点击会被裁掉。"""
        for _ in range(8):
            user32.SetWindowPos(self.hwnd, 0, 0, 0, w, h, 0x0004)  # SWP_NOZORDER
            time.sleep(0.6)
            r = wintypes.RECT()
            user32.GetWindowRect(self.hwnd, ctypes.byref(r))
            if abs(r.left) < 20 and abs(r.top) < 20:
                return r.left, r.top
        return None

    def client_origin(self):
        pt = wintypes.POINT(0, 0)
        user32.ClientToScreen(self.hwnd, ctypes.byref(pt))
        return pt.x, pt.y

    def click(self, x, y, settle=0.5):
        # 真实光标也挪过去：后端在窗口未被 hover 时每帧用真实光标回填 io.MousePos，
        # 只 PostMessage 会被它覆盖掉。
        cx, cy = self.client_origin()
        user32.SetCursorPos(cx + x, cy + y)
        time.sleep(0.15)

        lp = _lparam(x, y)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, lp)
        time.sleep(0.08)
        user32.PostMessageW(self.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, lp)
        time.sleep(0.08)
        user32.PostMessageW(self.hwnd, WM_LBUTTONUP, 0, lp)
        time.sleep(settle)

    def right_click(self, x, y, settle=0.6):
        WM_RBUTTONDOWN, WM_RBUTTONUP = 0x0204, 0x0205
        MK_RBUTTON = 0x0002
        cx, cy = self.client_origin()
        user32.SetCursorPos(cx + x, cy + y)
        time.sleep(0.15)
        lp = _lparam(x, y)
        user32.PostMessageW(self.hwnd, WM_MOUSEMOVE, 0, lp)
        time.sleep(0.08)
        user32.PostMessageW(self.hwnd, WM_RBUTTONDOWN, MK_RBUTTON, lp)
        time.sleep(0.08)
        user32.PostMessageW(self.hwnd, WM_RBUTTONUP, 0, lp)
        time.sleep(settle)

    def text(self, s):
        for ch in s:
            user32.PostMessageW(self.hwnd, WM_CHAR, ord(ch), 0)
            time.sleep(0.05)

    def key(self, vk, settle=0.08):
        user32.PostMessageW(self.hwnd, WM_KEYDOWN, vk, 0)
        time.sleep(0.05)
        user32.PostMessageW(self.hwnd, WM_KEYUP, vk, 0)
        time.sleep(settle)

    def key_ctrl(self, vk, settle=0.15):
        VK_CONTROL = 0x11
        user32.PostMessageW(self.hwnd, WM_KEYDOWN, VK_CONTROL, 0)
        time.sleep(0.03)
        user32.PostMessageW(self.hwnd, WM_KEYDOWN, vk, 0)
        time.sleep(0.03)
        user32.PostMessageW(self.hwnd, WM_KEYUP, vk, 0)
        time.sleep(0.03)
        user32.PostMessageW(self.hwnd, WM_KEYUP, VK_CONTROL, 0)
        time.sleep(settle)

    def client_size(self):
        r = wintypes.RECT()
        user32.GetClientRect(self.hwnd, ctypes.byref(r))
        return r.right, r.bottom
