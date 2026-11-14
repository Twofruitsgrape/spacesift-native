// 界面双语（中/英）的轻量机制。
//
// 设计取舍：不上 gettext/资源表 —— 本项目的文案就 200 来条，直接把两种语言
// 并排写在调用点可读性最好，改一处文案不用跨文件对键。
//
// 用法：TR("中文文案", "English text")。TR 返回 const char*，可直接做
// printf/snprintf 的格式串（两种语言的格式占位符必须一致）。
//
// 语言状态是 inline 变量：所有翻译单元共享同一份，main.cpp 启动时从
// settings.ini 读入，顶栏「EN/中」按钮切换并写回。
#pragma once

namespace i18n {

enum Lang { Lang_ZH = 0, Lang_EN = 1 };

inline Lang g_lang = Lang_ZH;

inline const char* Tr(const char* zh, const char* en) {
    return g_lang == Lang_ZH ? zh : en;
}

}  // namespace i18n

#define TR(zh, en) ::i18n::Tr(zh, en)
