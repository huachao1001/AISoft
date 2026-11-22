// 中性输入事件：平台后端把原生消息翻译成这些结构后再交给控件
#pragma once

#include "pyui/types.h"

#include <cstdint>

namespace pyui {

enum class MouseButton { kNone, kLeft, kRight, kMiddle };

struct Modifiers {
  bool ctrl = false;
  bool shift = false;
  bool alt = false;
  bool meta = false;  // Windows 键 / Command 键

  bool operator==(const Modifiers& o) const {
    return ctrl == o.ctrl && shift == o.shift && alt == o.alt && meta == o.meta;
  }
};

// 一次事件在视图树里的传播状态。分发器创建，事件对象持指针。
// 控件处理完调用 stopPropagation()，外层容器不再收到该事件。
struct EventState {
  bool handled = false;
  bool stopped = false;

  void markHandled() { handled = true; }
  void stop() {
    handled = true;
    stopped = true;
  }
};

struct MouseEvent {
  Point position;  // 当前接收视图的局部坐标
  MouseButton button = MouseButton::kNone;
  int wheelDelta = 0;  // 一格 = 120
  Modifiers modifiers;
  // 分发器注入；控件据此中断冒泡。不参与相等比较。
  EventState* propagation = nullptr;

  void stopPropagation() const {
    if (propagation) propagation->stop();
  }
  void markHandled() const {
    if (propagation) propagation->markHandled();
  }
};

// 鼠标指针形状。kGrab / kGrabbing 是「手掌」：拖动画布、拖文件这类抓住并
// 移动的语义，用握起来的手掌才不会和「点击链接」的食指混淆。
enum class CursorShape { kArrow, kHand, kSizeHorizontal, kSizeVertical, kIBeam, kGrab, kGrabbing };

// 键位：不绑定具体平台的虚拟键码
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

}  // namespace pyui
