// Windows 输入法（IME）定位
//
// 背景：随包的 imgui 1.93.1 的 win32 后端没有实现 platform_io.Platform_SetImeDataFn，
// 于是 Windows 从来不知道 ImGui 的输入光标在哪，中文输入法的组合串和候选框
// 就跑到屏幕默认位置（左下角），跟输入框完全脱节。
//
// 这里补上这个回调：把 ImGui 给的光标屏幕坐标翻成客户区坐标，喂给
// ImmSetCompositionWindow / ImmSetCandidateWindow。

#include <windows.h>
#include <imm.h>

#include "imgui.h"
#include "ime_win32.h"

#include <cstdio>
#include <cstdlib>

namespace {

bool ImeDebug() {
    static int on = -1;
    if (on < 0) {
        const char* v = std::getenv("SPACESIFT_IME_DEBUG");
        on = (v && *v && *v != '0') ? 1 : 0;
    }
    return on == 1;
}

void ImeLog(const char* fmt, ...) {
    if (!ImeDebug()) return;
    static FILE* f = nullptr;
    if (!f) {
        const char* p = std::getenv("SPACESIFT_IME_LOG");
        f = fopen(p ? p : "ime_debug.log", "a");
        if (!f) return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fflush(f);
}

void SetImeData(ImGuiContext*, ImGuiViewport* vp, ImGuiPlatformImeData* data) {
    HWND hwnd = (HWND)vp->PlatformHandleRaw;
    if (!hwnd) return;

    HIMC himc = ::ImmGetContext(hwnd);
    if (!himc) {
        ImeLog("no himc (hwnd=%p)\n", (void*)hwnd);
        return;
    }

    if (!data->WantVisible) {
        // 焦点离开输入框：把组合窗口挪到看不见的地方，避免残影
        COMPOSITIONFORM cf = {};
        cf.dwStyle = CFS_POINT;
        cf.ptCurrentPos.x = -32000;
        cf.ptCurrentPos.y = -32000;
        ::ImmSetCompositionWindow(himc, &cf);
        ::ImmReleaseContext(hwnd, himc);
        ImeLog("hide\n");
        return;
    }

    // 坐标系换算：
    //   ImGui 的 InputPos 是「主视口坐标系」，主视口原点 = vp->Pos。
    //   但这个 win32 后端从不更新 main_viewport->Pos（一直是 0,0），
    //   所以 InputPos 实际上已经是客户区坐标 —— 直接 ScreenToClient 会
    //   把客户区原点（标题栏高度）重复减一次，y 变成负数，候选框就飞了。
    //   这里先还原成屏幕坐标，再转客户区，两种后端都能正确工作。
    POINT c0 = {0, 0};
    ::ClientToScreen(hwnd, &c0);            // 客户区原点（屏幕坐标）

    POINT pt;
    pt.x = c0.x + (LONG)(data->InputPos.x - vp->Pos.x);
    pt.y = c0.y + (LONG)(data->InputPos.y - vp->Pos.y);
    ::ScreenToClient(hwnd, &pt);            // → 客户区坐标

    // 组合串画在光标处
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = pt;
    ::ImmSetCompositionWindow(himc, &cf);

    // 候选列表：整行（输入框那一行）都别盖住，让系统排到下面
    RECT rc = {};
    ::GetClientRect(hwnd, &rc);
    const LONG lineH = (LONG)(data->InputLineHeight > 0.0f ? data->InputLineHeight : 20.0f);

    CANDIDATEFORM cd = {};
    cd.dwIndex = 0;
    cd.dwStyle = CFS_EXCLUDE;
    cd.ptCurrentPos = pt;
    cd.rcArea.left = 0;
    cd.rcArea.top = pt.y;
    cd.rcArea.right = rc.right;
    cd.rcArea.bottom = pt.y + lineH;
    ::ImmSetCandidateWindow(himc, &cd);

    ::ImmReleaseContext(hwnd, himc);

    ImeLog("show in=(%.1f,%.1f) vp.Pos=(%.1f,%.1f) client=(%ld,%ld) lineH=%.1f clientOrigin=(%ld,%ld)\n",
           data->InputPos.x, data->InputPos.y, vp->Pos.x, vp->Pos.y,
           pt.x, pt.y, data->InputLineHeight, c0.x, c0.y);

    // 回读：确认系统真的把组合窗口/候选窗口存在了我们给的位置
    if (ImeDebug()) {
        COMPOSITIONFORM rb_cf = {};
        CANDIDATEFORM rb_cd = {};
        const BOOL ok_cf = ::ImmGetCompositionWindow(himc, &rb_cf);
        const BOOL ok_cd = ::ImmGetCandidateWindow(himc, 0, &rb_cd);
        ImeLog("  readback comp=%d (%ld,%ld) style=0x%lx | cand=%d (%ld,%ld) style=0x%lx\n",
               (int)ok_cf, rb_cf.ptCurrentPos.x, rb_cf.ptCurrentPos.y, (unsigned long)rb_cf.dwStyle,
               (int)ok_cd, rb_cd.ptCurrentPos.x, rb_cd.ptCurrentPos.y, (unsigned long)rb_cd.dwStyle);
    }
}

} // namespace

void SpaceSift_ImeFrame() {
    if (!ImeDebug()) return;
    static bool prev = false;
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput != prev) {
        ImeLog("WantTextInput=%d (WantCaptureKeyboard=%d)\n", (int)io.WantTextInput, (int)io.WantCaptureKeyboard);
        prev = io.WantTextInput;
    }
}

void SpaceSift_InstallImeSupport() {
    ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
    pio.Platform_SetImeDataFn = SetImeData;
    ImeLog("installed\n");
}
