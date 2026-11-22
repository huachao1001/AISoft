// 托盘图标：只负责「挂上去、把点击转成回调」，菜单每次现取
//
// 为什么菜单每次现取而不缓存：
//   托盘菜单的可用项依赖运行状态（是否正在压缩、是否有活动窗口）。
//   每次右键时重新问一遍宿主，就不必在状态变化时逐项同步菜单内容——
//   同步一定会漏，而现取不会。
#pragma once

#include "pyui/menu.h"
#include "pyui/painter.h"
#include "pyui/types.h"

#include <functional>
#include <string>

namespace pyui {

class TrayHost {
 public:
  virtual ~TrayHost() = default;

  // 挂上图标；tooltip 是悬停提示。返回是否成功。
  // 图标由后端从当前可执行文件取，应用不必再引一套跨平台的图片约定。
  virtual bool show(const std::string& tooltip) = 0;
  virtual void hide() = 0;
  virtual void setTooltip(const std::string& tooltip) = 0;
  virtual bool isVisible() const = 0;

  // 左键单击（或双击，看平台习惯）时回调；通常用来还原主窗口。
  using ActivateHandler = std::function<void()>;
  virtual void setOnActivate(ActivateHandler fn) = 0;

  // 右键菜单：菜单内容由宿主现场提供，选中后回调 id。
  using MenuProvider = std::function<std::vector<MenuItem>()>;
  using MenuHandler = std::function<void(const std::string& id)>;
  virtual void setMenu(MenuProvider provider, MenuHandler handler) = 0;
};

}  // namespace pyui
