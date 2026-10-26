#include "suyu_cmd/scarlet_software_keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdlib>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "common/string_util.h"
#include "video_core/renderer_vulkan/scarlet_keyboard_overlay.h"

#ifdef _WIN32

KeyboardResponse RunKeyboard(KeyboardInitializeParameters params, std::u16string initial,
                             std::u16string error) {
    KeyboardOverlay::Config config{
        .header = params.header_text,
        .sub = params.sub_text,
        .guide = params.guide_text,
        .ok_text = params.ok_text,
        .max_length = params.max_text_length,
        .min_length = params.min_text_length,
        .disable_flags = params.key_disable_flags.raw,
        .password =
            params.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled,
        .number_pad = params.type == SwkbdType::NumberPad,
        .enable_return = params.enable_return_button,
        .disable_cancel = params.disable_cancel_button,
    };

    LOG_INFO(Service_AM,
             "[Scarlet.Keyboard] Showing native in-frame Vulkan keyboard overlay");
    KeyboardOverlay::Show(std::move(config), std::move(initial), std::move(error));

    KeyboardOverlay::Result native_result{};
    while (!KeyboardOverlay::PollResult(native_result)) {
        KeyboardOverlay::TickInput();
        Sleep(16);
    }

    KeyboardResponse response{};
    response.accepted = native_result.accepted;
    response.silent = native_result.silent;
    response.text = std::move(native_result.text);
    LOG_INFO(Service_AM,
             "[Scarlet.Keyboard] Native in-frame keyboard finished: accepted={}",
             response.accepted);
    return response;
}

void CloseKeyboard(bool silent) {
    KeyboardOverlay::Hide(silent);
}

void UpdateKeyboardText(const InlineTextParameters& params) {
    KeyboardOverlay::UpdateText(params.input_text);
}

#else

KeyboardResponse RunKeyboard(KeyboardInitializeParameters params, std::u16string initial,
                             std::u16string error) {
    (void)params;
    (void)error;
    return KeyboardResponse{true, false, std::move(initial)};
}

void CloseKeyboard(bool silent) {
    (void)silent;
}

void UpdateKeyboardText(const InlineTextParameters& params) {
    (void)params;
}

#endif

} // namespace

bool ConsumeKeyboardDesktopModeRequest() {
    return false;
}

bool ConsumeKeyboardFullscreenRestoreRequest() {
    return false;
}

void AcknowledgeKeyboardDesktopMode() {}

void AcknowledgeKeyboardFullscreenRestore() {}

bool KeyboardOverlayActive() {
    return KeyboardOverlay::Visible();
}

SoftwareKeyboardApplet::~SoftwareKeyboardApplet() {
    CloseKeyboard(true);
}

void SoftwareKeyboardApplet::Close() const {
    CloseKeyboard(true);
}

void SoftwareKeyboardApplet::InitializeKeyboard(
    bool is_inline, KeyboardInitializeParameters initialize_parameters,
    SubmitNormalCallback submit_normal_callback_, SubmitInlineCallback submit_inline_callback_) {
    parameters = std::move(initialize_parameters);
    current_text = parameters.initial_text;

    if (is_inline) {
        submit_inline_callback = std::move(submit_inline_callback_);
        submit_normal_callback = {};
    } else {
        submit_normal_callback = std::move(submit_normal_callback_);
        submit_inline_callback = {};
    }

    LOG_INFO(Service_AM,
             "[Scarlet.Keyboard] Initialized: inline={}, max={}, min={}, initial='{}'",
             is_inline, parameters.max_text_length, parameters.min_text_length,
             Common::UTF16ToUTF8(parameters.initial_text));
}

void SoftwareKeyboardApplet::ShowNormalKeyboard() const {
    const auto params = parameters;
    const auto initial = current_text;

    std::thread([this, params, initial] {
        LOG_INFO(Service_AM, "[Scarlet.Keyboard] Showing normal keyboard");
        auto response = RunKeyboard(params, initial, {});
        if (response.silent) {
            return;
        }
        if (!response.accepted) {
            submit_normal_callback(SwkbdResult::Cancel, {}, false);
            return;
        }

        current_text = response.text;
        // false is intentional: when Scarlet requested TextCheck, the applet must
        // give the candidate back to the game for validation before final acceptance.
        submit_normal_callback(SwkbdResult::Ok, response.text, false);
    }).detach();
}

void SoftwareKeyboardApplet::ShowTextCheckDialog(SwkbdTextCheckResult result,
                                                 std::u16string message) const {
    if (result == SwkbdTextCheckResult::Failure) {
        const auto params = parameters;
        const auto initial = current_text;

        std::thread([this, params, initial, message = std::move(message)]() mutable {
            LOG_INFO(Service_AM, "[Scarlet.Keyboard] Reopening after game text check failure");
            auto response = RunKeyboard(params, initial, std::move(message));
            if (response.silent) {
                return;
            }
            if (!response.accepted) {
                submit_normal_callback(SwkbdResult::Cancel, {}, false);
                return;
            }

            current_text = response.text;
            submit_normal_callback(SwkbdResult::Ok, response.text, false);
        }).detach();
        return;
    }

    if (result == SwkbdTextCheckResult::Confirm) {
        const auto params = parameters;
        const auto initial = current_text;

        std::thread([this, params, initial, message = std::move(message)]() mutable {
            // Keep confirmation inside the same in-game keyboard overlay. If the user
            // leaves the candidate unchanged, report it as confirmed; if they edit it,
            // send it back through the game's text-check path again.
            auto response =
                RunKeyboard(params, initial, message.empty() ? u"Usar este texto?" : message);
            if (response.silent) {
                return;
            }
            if (!response.accepted) {
                submit_normal_callback(SwkbdResult::Cancel, {}, false);
                return;
            }

            const bool unchanged = response.text == initial;
            current_text = response.text;
            submit_normal_callback(SwkbdResult::Ok, response.text, unchanged);
        }).detach();
    }
}

void SoftwareKeyboardApplet::ShowInlineKeyboard(InlineAppearParameters appear) const {
    auto params = parameters;
    params.max_text_length = appear.max_text_length;
    params.min_text_length = appear.min_text_length;
    params.type = appear.type;
    params.key_disable_flags = appear.key_disable_flags;
    params.enable_backspace_button = appear.enable_backspace_button;
    params.enable_return_button = appear.enable_return_button;
    params.disable_cancel_button = appear.disable_cancel_button;
    params.initial_text = current_text;

    inline_visible.store(true, std::memory_order_release);

    std::thread([this, params] {
        LOG_INFO(Service_AM, "[Scarlet.Keyboard] Showing inline keyboard");
        auto response = RunKeyboard(params, current_text, {});
        inline_visible.store(false, std::memory_order_release);

        if (response.silent) {
            return;
        }
        if (!response.accepted) {
            submit_inline_callback(SwkbdReplyType::DecidedCancel, current_text,
                                   static_cast<s32>(current_text.size()));
            return;
        }

        current_text = response.text;
        const s32 cursor = static_cast<s32>(current_text.size());
        submit_inline_callback(SwkbdReplyType::ChangedString, current_text, cursor);
        submit_inline_callback(SwkbdReplyType::DecidedEnter, current_text, cursor);
    }).detach();
}

void SoftwareKeyboardApplet::HideInlineKeyboard() const {
    inline_visible.store(false, std::memory_order_release);
    CloseKeyboard(true);
}

void SoftwareKeyboardApplet::InlineTextChanged(InlineTextParameters text_parameters) const {
    current_text = text_parameters.input_text;
    UpdateKeyboardText(text_parameters);
}

void SoftwareKeyboardApplet::ExitKeyboard() const {
    inline_visible.store(false, std::memory_order_release);
    CloseKeyboard(true);
}

} // namespace ScarletPC
