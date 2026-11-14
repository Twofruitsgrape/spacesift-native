// 出厂自检：解压后不用界面就能验"这包是不是完整的"。
//
// 为什么要有它：用户下载的是别人机器上编译的二进制，PDF/DWG/OCR 全指望
// 旁边的 dll 和 tessdata 放对位置。只看界面"能打开"说明不了任何问题 ——
// 要能自己造样本、自己抽正文、自己建索引、自己搜出结果，才算真的能用。
//
// 全程只写 %TEMP%\SpaceSift-selftest-<pid>，跑完连索引库一起删干净。
#pragma once

namespace selftest {

// 返回 0 = 全部通过；1 = 有失败项；2 = 参数/环境问题
int Run(const wchar_t* folder);   // folder 为空时只跑内置语料

}  // namespace selftest
