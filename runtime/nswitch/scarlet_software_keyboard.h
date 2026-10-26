#pragma once

#include <atomic>
#include <string>

#include "core/frontend/applets/software_keyboard.h"

namespace ScarletPC {

// Cross-thread handshake with the SDL window. The Win32 keyboard runs on its own
// UI thread; exclusive fullscreen transitions must stay on the SDL/main thread.
bool ConsumeKeyboardDesktopModeRequest();
bool ConsumeKeyboardFullscreenRestoreRequest();
void AcknowledgeKeyboardDesktopMode();
void AcknowledgeKeyboardFullscreenRestore();
bool KeyboardOverlayActive();

class SoftwareKeyboardApplet final : public Core::Frontend::SoftwareKeyboardApplet {
public:
    ~SoftwareKeyboardApplet() override;

    void Close() const override;

    void InitializeKeyboard(bool is_inline,
                            Core::Frontend::KeyboardInitializeParameters initialize_parameters,
                            SubmitNormalCallback submit_normal_callback_,
                            SubmitInlineCallback submit_inline_callback_) override;

    void ShowNormalKeyboard() const override;

    void ShowTextCheckDialog(
        Service::AM::Frontend::SwkbdTextCheckResult text_check_result,
        std::u16string text_check_message) const override;

    void ShowInlineKeyboard(Core::Frontend::InlineAppearParameters appear_parameters) const override;

    void HideInlineKeyboard() const override;

    void InlineTextChanged(Core::Frontend::InlineTextParameters text_parameters) const override;

    void ExitKeyboard() const override;

private:
    Core::Frontend::KeyboardInitializeParameters parameters{};
    mutable SubmitNormalCallback submit_normal_callback;
    mutable SubmitInlineCallback submit_inline_callback;
    mutable std::u16string current_text;
    mutable std::atomic_bool inline_visible{false};
};

} // namespace ScarletPC
