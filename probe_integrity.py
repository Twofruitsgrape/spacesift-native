"""检查 explorer.exe 与当前进程的完整性级别（判断 shell 是否被提权）。"""
import ctypes
from ctypes import wintypes

k32 = ctypes.windll.kernel32
adv = ctypes.windll.advapi32
psapi = ctypes.windll.psapi

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
TOKEN_QUERY = 0x0008
TokenIntegrityLevel = 25


class SID_AND_ATTRIBUTES(ctypes.Structure):
    _fields_ = [("Sid", ctypes.c_void_p), ("Attributes", wintypes.DWORD)]


class TOKEN_MANDATORY_LABEL(ctypes.Structure):
    _fields_ = [("Label", SID_AND_ATTRIBUTES)]


def integrity_of(pid):
    h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not h:
        return "open-fail(%d)" % ctypes.get_last_error()
    tok = ctypes.c_void_p()
    if not adv.OpenProcessToken(h, TOKEN_QUERY, ctypes.byref(tok)):
        k32.CloseHandle(h)
        return "token-fail(%d)" % ctypes.get_last_error()
    need = wintypes.DWORD(0)
    adv.GetTokenInformation(tok, TokenIntegrityLevel, None, 0, ctypes.byref(need))
    buf = ctypes.create_string_buffer(need.value)
    ok = adv.GetTokenInformation(tok, TokenIntegrityLevel, buf, need, ctypes.byref(need))
    level = "?"
    if ok:
        label = ctypes.cast(buf, ctypes.POINTER(TOKEN_MANDATORY_LABEL)).contents
        subauth_count = ctypes.cast(label.Label.Sid, ctypes.POINTER(ctypes.c_ubyte))[1]
        addr = ctypes.cast(label.Label.Sid, ctypes.c_void_p).value
        # 最后一个子权威就是等级
        cnt = ctypes.cast(label.Label.Sid, ctypes.POINTER(ctypes.c_ubyte))[1]
        off = 8 + 4 * (cnt - 1)
        rid = ctypes.cast(addr + off, ctypes.POINTER(ctypes.c_uint)).contents.value
        names = {0x0000: "Untrusted", 0x1000: "Low", 0x2000: "Medium", 0x3000: "High",
                 0x4000: "System"}
        level = "%s (0x%x)" % (names.get(rid, "?"), rid)
    k32.CloseHandle(tok)
    k32.CloseHandle(h)
    return level


def processes(name):
    out = []
    arr = (wintypes.DWORD * 4096)()
    need = wintypes.DWORD(0)
    psapi.EnumProcesses(arr, ctypes.sizeof(arr), ctypes.byref(need))
    n = need.value // ctypes.sizeof(wintypes.DWORD)
    for i in range(n):
        pid = arr[i]
        h = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            continue
        buf = ctypes.create_unicode_buffer(1024)
        size = wintypes.DWORD(1024)
        if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(size)):
            if buf.value.lower().endswith(name.lower()):
                out.append((pid, buf.value))
        k32.CloseHandle(h)
    return out


print("当前进程完整性:", integrity_of(k32.GetCurrentProcessId()))
for pid, path in processes("explorer.exe"):
    print(f"explorer.exe pid={pid} 完整性={integrity_of(pid)}  {path}")
for pid, path in processes("taskmgr.exe"):
    print(f"taskmgr.exe pid={pid} 完整性={integrity_of(pid)}")
