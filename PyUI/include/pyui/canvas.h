// 画布：一次绘制的目标
//
// 平台后端的 createCanvas 返回它，析构时自动呈现（blit 到窗口）。
// 关键约定：**painter() 收的始终是客户区坐标**。画布的离屏位图只覆盖 area，
// 所以后端在内部已经平移过，把客户区原点对到 DIB 原点；上层（AppWindow /
// ViewRoot）因此不需要任何换算，脏区算出来是哪儿就画哪儿。
//
// 离线渲染用 SurfaceCanvas，两者对上层是同一种东西，所以「渲染到屏幕」与
// 「渲染到内存」走同一条路径。
#pragma once

#include "pyui/painter.h"
#include "pyui/types.h"

#include <memory>

namespace pyui {

class Canvas {
 public:
  virtual ~Canvas() = default;

  // 绘制入口。坐标是客户区坐标，与 area 同源。
  virtual Painter& painter() = 0;
  // 本画布覆盖的窗口区域（客户区坐标）
  virtual Rect area() const = 0;
  // 把内容呈现到目标；析构时后端会自动调用一次。
  virtual void present() = 0;
};

}  // namespace pyui
