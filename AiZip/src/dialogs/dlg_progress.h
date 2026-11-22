// 进度对话框：后台线程执行压缩/解压，UI 线程只负责刷新
#pragma once

#include "archive.h"
#include "AiUI/platform.h"

#include <functional>
#include <string>

namespace ui {

// work 收到一个进度回调，返回 false 表示用户取消
using WorkFn = std::function<bool(const arc::ProgressFn&)>;

// 模态执行 work，期间显示可取消的进度窗口；返回 work 是否成功。
// targetName 是这次操作的目标包名：压缩时是输出包名，解压时是输入包名。
// 主行显示「动作：包名」，正在处理的成员走副行。
//
// canceledOut 非空时写回「这次是不是用户自己取消的」。取消必须和失败分开：
// 用户点了取消是主动放弃，调用方不该再弹一个「解压失败」——那个框除了
// 打扰不提供任何信息。
bool ShowProgressDialog(AiUI::NativeHandle owner, const std::wstring& title,
                        const std::wstring& targetName, const WorkFn& work,
                        bool* canceledOut = nullptr);

// 在工作线程请求 UI 线程执行 fn 并等待完成。
// 没有进度窗口时直接就地执行。
void RunOnUiThread(const std::function<void()>& fn);

}  // namespace ui
