// PasswordBox 实现
#include "AiUI/widgets/password_box.h"

namespace AiUI {
namespace {

// 把掩码与图标写进 spec。图标画的是当前状态：
// 睁眼 = 明文可见，闭眼 = 已掩码，与系统密码框的惯例一致。
void applyState(InputSpec& spec, bool revealed) {
  spec.password = !revealed;
  spec.hasTailIcon = true;
  spec.tailIcon = revealed ? IconId::kEye : IconId::kEyeOff;
}

}  // namespace

PasswordBox::PasswordBox() {
  // 显隐开关就是 InputBox 的尾部图标，这里只负责把状态接上去
  setOnTailIcon([this] { setRevealed(!revealed_); });

  InputSpec spec = this->spec();
  applyState(spec, revealed_);
  InputBox::setSpec(spec);
}

void PasswordBox::setRevealed(bool revealed) {
  if (revealed_ == revealed) return;
  revealed_ = revealed;
  InputSpec spec = this->spec();
  applyState(spec, revealed_);
  InputBox::setSpec(spec);
}

void PasswordBox::setSpec(const InputSpec& spec) {
  // 构造顺序可能反过来（先 setSpec 后 setRevealed），这里统一覆写，
  // 否则 XML 注入的 spec.password 会把状态冲掉
  InputSpec local = spec;
  applyState(local, revealed_);
  InputBox::setSpec(local);
}

}  // namespace AiUI
