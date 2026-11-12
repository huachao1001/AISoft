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

// 一次事件在视图树里的传播状态。由分发器创建，事件对象持指针。
// 控件处理完后调用 stopPropagation()，外层容器就不会再收到该事件——
// 与 Android 的「消费即不再上报」一致。
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
  int wheelDelta = 0;  // 一格为 120
  Modifiers modifiers;
  // 分发器注入；控件据此中断冒泡。不属于用户构造的事件，默认空。
  EventState* propagation = nullptr;

  // 消费事件并阻止继续向外层冒泡
  void stopPropagation() const {
    if (propagation) propagation->stop();
  }
  // 只标记已处理，仍允许继续冒泡
  void markHandled() const {
    if (propagation) propagation->markHandled();
  }
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
