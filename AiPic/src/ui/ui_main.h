// 主窗口的宿主接入：拖放与单实例转发
//
// 界面与生命周期在 MainActivity 里。拖放由平台层的 setFileDropHandler 提供，
// 不需要应用侧碰系统消息。
#pragma once

namespace ui {

// 主窗口最小尺寸（96 DPI 设计值）。工具栏要能一排放下全部按钮，再窄就得换行，
// 而 ButtonBar 不换行——所以下限由按钮总宽决定。
constexpr int kMinWidth = 860;
constexpr int kMinHeight = 520;

// 把拖放与转发挂到主窗口上；MainActivity::onCreate 之后调用
void AttachHostHooks();

}  // namespace ui
