// 应用状态
//
// 只放「一个窗口要记住的东西」。缩放与平移属于控件的临时视图状态，
// 不进这里；旋转要进，因为它会改变另存出去的内容。
#pragma once

#include "util.h"
#include "AiUI/icon.h"
#include "AiUI/platform.h"

#include <string>
#include <vector>

struct App {
  // 主窗口原生句柄；只用于给系统弹窗做 owner，不参与业务判断
  AiUI::NativeHandle mainWnd = nullptr;
  util::Settings settings;

  int dpi = 96;
  int px(int v) const { return (v * dpi + 48) / 96; }
};

extern App g_app;

// ── 当前图 ──
// 打开图片：解码到像素并存下去。失败返回 false，调用方据此提示。
bool AppOpenImage(const std::wstring& path);
void AppCloseImage();
const std::wstring& AppImagePath();
bool AppHasImage();

// 旋转（90° 的整数倍，顺时针为正）。像素本身不动，只在另存时按它重排，
// 这样来回转几次不会因为反复重采样而掉画质。
int AppRotation();
void AppSetRotation(int quarterTurns);

// 按当前旋转导出像素：另存为与「复制到剪贴板」都用它
AiUI::RgbaImage AppExportPixels();
// 原始（未旋转）像素尺寸
AiUI::Size AppImageSize();

// ── 同目录翻页 ──
// 打开一张图时顺带记下同目录的可显示图片，左右键据此切换。只记路径，
// 不预加载内容：一个目录几千张图也不该拖慢打开动作。
const std::vector<std::wstring>& AppSiblingImages();
// 在当前目录里跳到前/后一张；返回 false 表示没有可跳的目标
bool AppStepImage(int delta);
