// PasswordBox：带显隐开关的密码输入框
//
// 只是 InputBox 的一层薄包装：文本、光标、选区、剪贴板全部复用，
// 这里只管「掩码 / 明文」这一个状态，以及随之更换的尾部眼睛图标。
#pragma once

#include "AiUI/widgets/input_box.h"

namespace AiUI {

class PasswordBox : public InputBox {
 public:
  PasswordBox();

  // 当前是否显示明文
  bool revealed() const { return revealed_; }
  void setRevealed(bool revealed);
  // XML 注入的 spec 不关心掩码与图标，这里统一按当前状态覆写
  void setSpec(const InputSpec& spec) override;

 private:
  bool revealed_ = false;
};

}  // namespace AiUI
