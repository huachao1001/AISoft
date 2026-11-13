// 文件管理器右键菜单的「动态菜单」模型
//
// 静态注册表动词的文字在注册那一刻就定死了，做不出「压缩到 rnd.zip」这种
// 带文件名的文案。动态文案只能由文件管理器进程内的处理器在弹出菜单的瞬间
// 计算，所以这里把「选中了什么 → 显示哪些项」抽成平台无关的模型：
// 应用只写纯逻辑，怎么装进系统菜单由平台后端负责。
#pragma once

#include <string>
#include <vector>

namespace AiUI {

// 一次选择里的一个对象。
// isDirectory 由后端判断后填入，应用的命名规则不必自己去碰文件系统。
struct ShellSelectionItem {
  std::wstring path;
  bool isDirectory = false;
};

// 一个菜单项
struct ShellMenuItem {
  std::wstring label;
  // 追加在宿主可执行文件后的参数，如 "-extract-here"。
  // 被选中的路径由后端按平台约定拼在后面。
  std::wstring arguments;
  // 在这一项之后插入分隔线
  bool separatorAfter = false;
};

// 处理器对这次选择的结论；items 为空表示菜单里不出现本程序
struct ShellMenuResult {
  std::vector<ShellMenuItem> items;
};

}  // namespace AiUI

// 应用在自己的菜单模型源文件里实现这个函数，由 aiui_add_shell_menu_dll 编进组件。
// 它跑在文件管理器的进程里，只能做纯计算：不许弹窗、不许读配置、不许阻塞，
// 参数不合法或算不出来时返回空结果即可。
AiUI::ShellMenuResult aiuiBuildShellMenu(const std::vector<AiUI::ShellSelectionItem>& selection);
