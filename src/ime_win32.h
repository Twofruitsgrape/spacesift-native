#pragma once

// 补上 imgui win32 后端缺失的 IME 定位回调。
// 必须在 ImGui::CreateContext() + ImGui_ImplWin32_Init() 之后调用。
void SpaceSift_InstallImeSupport();

// 每帧调用（仅 SPACESIFT_IME_DEBUG 时打日志，用来确认 ImGui 有没有把焦点给输入框）
void SpaceSift_ImeFrame();
