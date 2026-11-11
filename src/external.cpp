#include "external.h"

#include <vector>

namespace external {
namespace {

std::wstring BuildCmd(const std::wstring& exe, const std::wstring& args) {
    std::wstring cmd = L"\"";
    cmd += exe;
    cmd += L"\"";
    if (!args.empty()) {
        cmd += L' ';
        cmd += args;
    }
    return cmd;
}

// captureFile 非空时把 stdout/stderr 都重定向进去
bool Spawn(const std::wstring& exe, const std::wstring& args,
           const std::wstring& captureFile, DWORD timeoutMs) {
    if (!FileExists(exe)) return false;

    const std::wstring cmd = BuildCmd(exe, args);
    std::vector<wchar_t> cmdbuf(cmd.begin(), cmd.end());
    cmdbuf.push_back(L'\0');

    STARTUPINFOW si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    bool capturing = !captureFile.empty();
    HANDLE cap = INVALID_HANDLE_VALUE;
    HANDLE nul = INVALID_HANDLE_VALUE;

    if (capturing) {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = nullptr;
        sa.bInheritHandle = TRUE;      // 默认创建出来的是不可继承的，必须显式给

        cap = CreateFileW(captureFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (cap != INVALID_HANDLE_VALUE) {
            nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            si.dwFlags |= STARTF_USESTDHANDLES;
            si.hStdOutput = cap;
            si.hStdError = cap;
            si.hStdInput = nul;
        } else {
            capturing = false;
        }
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    const BOOL ok = CreateProcessW(nullptr, cmdbuf.data(), nullptr, nullptr,
                                   capturing ? TRUE : FALSE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &si, &pi);
    if (cap != INVALID_HANDLE_VALUE) CloseHandle(cap);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!ok) return false;

    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // 有些工具转换成功但退出码非 0（只报 warning），只要等到进程结束就算跑过
    return wait == WAIT_OBJECT_0;
}

}  // namespace

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return L".";
    std::wstring p(buf, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring(L".") : p.substr(0, slash);
}

std::wstring FindOnPath(const wchar_t* fileName) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = SearchPathW(nullptr, fileName, nullptr,
                                (DWORD)(sizeof buf / sizeof buf[0]), buf, nullptr);
    return n ? std::wstring(buf, n) : std::wstring();
}

std::wstring TempName(const wchar_t* suffix) {
    wchar_t dir[MAX_PATH];
    const DWORD n = GetTempPathW(MAX_PATH, dir);
    if (n == 0) return std::wstring();
    wchar_t name[96];
    // 不要用 %llu：MinGW 的 MSVCRT 不认 ll 长度修饰符；
    // 宽字符串必须用 %ls，MSVCRT 的 %s 指的是 char*
    swprintf(name, 96, L"spacesift_%lu_%lu%ls",
             (unsigned long)GetCurrentProcessId(),
             (unsigned long)(GetTickCount64() & 0xFFFFFFFFull), suffix);
    return std::wstring(dir, n) + name;
}

bool FileExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool DirExists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileBytes(const std::wstring& path, std::string& out, size_t cap) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[65536];
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(h, buf, sizeof buf, &got, nullptr) || got == 0) break;
        const size_t take = (got < cap - out.size()) ? got : (cap - out.size());
        out.append(buf, take);
        if (out.size() >= cap) break;
    }
    CloseHandle(h);
    return !out.empty();
}

bool DeleteQuiet(const std::wstring& path) { return DeleteFileW(path.c_str()) != 0; }

bool RemoveDirQuiet(const std::wstring& path) { return RemoveDirectoryW(path.c_str()) != 0; }

bool RunCapture(const std::wstring& exe, const std::wstring& args, std::string& out,
                size_t cap, DWORD timeoutMs) {
    out.clear();
    const std::wstring tmp = TempName(L".out");
    if (tmp.empty()) return false;
    const bool ran = Spawn(exe, args, tmp, timeoutMs);
    ReadFileBytes(tmp, out, cap);
    DeleteQuiet(tmp);
    return ran && !out.empty();
}

bool RunSilent(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs) {
    return Spawn(exe, args, std::wstring(), timeoutMs);
}

}  // namespace external
