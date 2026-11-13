"""直接调用 shell API，测「普通权限能不让资源管理器选中文件」。不涉及界面。"""
import ctypes, time
from ctypes import wintypes

shell32 = ctypes.windll.shell32
ole32 = ctypes.windll.ole32
k32 = ctypes.windll.kernel32
adv = ctypes.windll.advapi32

ole32.CoInitializeEx(None, 2)  # STA

shell32.ILCreateFromPathW.restype = ctypes.c_void_p
shell32.ILCreateFromPathW.argtypes = [ctypes.c_wchar_p]
shell32.SHOpenFolderAndSelectItems.argtypes = [ctypes.c_void_p, ctypes.c_uint,
                                               ctypes.c_void_p, ctypes.c_ulong]
shell32.SHOpenFolderAndSelectItems.restype = ctypes.c_long
shell32.ShellExecuteW.restype = ctypes.c_void_p
shell32.ShellExecuteW.argtypes = [wintypes.HWND, ctypes.c_wchar_p, ctypes.c_wchar_p,
                                  ctypes.c_wchar_p, ctypes.c_wchar_p, ctypes.c_int]

FILE = r"D:\cigarette\05 温度场和气流场对加热卷烟气溶胶关键成分释放影响研究20241204.pptx"
DIR = r"D:\cigarette"


def integrity(pid):
    PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return "?"
    tok = ctypes.c_void_p()
    if not adv.OpenProcessToken(h, 0x0008, ctypes.byref(tok)):
        k32.CloseHandle(h)
        return "?"
    need = wintypes.DWORD(0)
    adv.GetTokenInformation(tok, 25, None, 0, ctypes.byref(need))
    buf = ctypes.create_string_buffer(need.value)
    adv.GetTokenInformation(tok, 25, buf, need, ctypes.byref(need))
    sid = ctypes.cast(buf, ctypes.POINTER(ctypes.c_void_p))[0]
    cnt = ctypes.cast(sid, ctypes.POINTER(ctypes.c_ubyte))[1]
    off = 8 + 4 * (cnt - 1)
    rid = ctypes.cast(sid + off, ctypes.POINTER(ctypes.c_uint)).contents.value
    k32.CloseHandle(tok)
    k32.CloseHandle(h)
    return {0x1000: "Low", 0x2000: "Medium", 0x3000: "High", 0x4000: "System"}.get(rid, hex(rid))


shell_pid = wintypes.DWORD(0)
ctypes.windll.user32.GetWindowThreadProcessId(ctypes.windll.user32.GetShellWindow(),
                                              ctypes.byref(shell_pid))
print("本进程完整性:", integrity(k32.GetCurrentProcessId()))
print("shell 进程完整性:", integrity(shell_pid.value), "pid", shell_pid.value)

pidl = shell32.ILCreateFromPathW(FILE)
hr = shell32.SHOpenFolderAndSelectItems(pidl, 0, None, 0)
print("SHOpenFolderAndSelectItems(文件) hr=0x%08x" % (hr & 0xFFFFFFFF))
time.sleep(2)

r = shell32.ShellExecuteW(None, "open", DIR, None, None, 1)
print("ShellExecuteW(open, 目录) 返回", r)
time.sleep(2)

r2 = shell32.ShellExecuteW(None, "explore", DIR, None, None, 1)
print("ShellExecuteW(explore, 目录) 返回", r2)
time.sleep(1)
