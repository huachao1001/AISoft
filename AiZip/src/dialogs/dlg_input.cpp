// 单行输入对话框
#include "dlg_input.h"

#include "AiUI/widgets/button.h"
#include "AiUI/widgets/input_box.h"
#include "AiUI/widgets/text_view.h"
#include "dialog_activity.h"

namespace ui {
namespace {

class DlgInput : public DialogActivity {
 public:
  DlgInput(std::wstring label, std::wstring initial, bool password)
      : label_(std::move(label)), initial_(std::move(initial)), password_(password) {}

  std::wstring value() const;

 protected:
  const wchar_t* layoutName() const override { return L"dlg_input"; }
  void onCreate() override;
  bool onAccept() override;

 private:
  std::wstring label_;
  std::wstring initial_;
  bool password_ = false;
  AiUI::InputBox* edit_ = nullptr;
};

std::wstring DlgInput::value() const {
  return edit_ ? edit_->text() : initial_;
}

void DlgInput::onCreate() {
  setEscapeCloses(true);

  if (auto* label = findView<AiUI::TextView>("label")) {
    label->setText(label_);
  }

  edit_ = findView<AiUI::InputBox>("edit");
  if (edit_) {
    AiUI::InputSpec spec = edit_->spec();
    spec.password = password_;
    spec.outerBackground = AiUI::kColorDialogBg;
    edit_->setSpec(spec);

    edit_->setText(initial_);
    edit_->selectAll();
    edit_->setFocus();
    // 回车等同确定：输入框是唯一焦点时这是最快路径
    edit_->setOnSubmit([this](bool committed) {
      if (!committed) {
        close();
        return;
      }
      if (onAccept()) {
        setAccepted(true);
        close();
      }
    });
  }

  if (auto* ok = findView<AiUI::Button>("btnOk")) {
    ok->setOnClick([this] {
      if (!onAccept()) return;
      setAccepted(true);
      close();
    });
  }
  if (auto* cancel = findView<AiUI::Button>("btnCancel")) {
    cancel->setOnClick([this] { close(); });
  }
}

bool DlgInput::onAccept() { return true; }

}  // namespace

bool PromptText(AiUI::NativeHandle owner, const std::wstring& title, const std::wstring& label,
                std::wstring& value, bool password) {
  DlgInput dlg(label, value, password);
  dlg.setTitle(title);
  if (!dlg.runModal(owner, 380, 168)) return false;
  value = dlg.value();
  return true;
}

}  // namespace ui
