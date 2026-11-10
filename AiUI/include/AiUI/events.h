// 中性输入事件：由平台后端翻译成这些结构后再交给控件
#pragma once

#include "AiUI/types.h"

namespace AiUI {

enum class MouseButton { kNone, kLeft, kRight, kMiddle };

struct Modifiers {
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
};

struct MouseEvent {
  Point position;  // 控件客户区坐标
  MouseButton button = MouseButton::kNone;
  int wheelDelta = 0;  // 一格为 120
  Modifiers modifiers;
};

// 鼠标指针形状
enum class CursorShape { kArrow, kHand, kSizeHorizontal, kIBeam };

// 键位：只列界面会用到的键，避免绑定具体平台的虚拟键码
enum class Key {
  kNone = 0,
  kBackspace,
  kTab,
  kEnter,
  kEscape,
  kSpace,
  kInsert,
  kDelete,
  kHome,
  kEnd,
  kPageUp,
  kPageDown,
  kLeft,
  kUp,
  kRight,
  kDown,
  kA,
  kB,
  kC,
  kD,
  kE,
  kF,
  kG,
  kH,
  kI,
  kJ,
  kK,
  kL,
  kM,
  kN,
  kO,
  kP,
  kQ,
  kR,
  kS,
  kT,
  kU,
  kV,
  kW,
  kX,
  kY,
  kZ,
  k0,
  k1,
  k2,
  k3,
  k4,
  k5,
  k6,
  k7,
  k8,
  k9,
  kF1,
  kF2,
  kF3,
  kF4,
  kF5,
  kF6,
  kF7,
  kF8,
  kF9,
  kF10,
  kF11,
  kF12,
};

}  // namespace AiUI
