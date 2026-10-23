#include "suyu_cmd/scarlet_software_keyboard.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cwctype>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "common/string_util.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace ScarletPC {

using Core::Frontend::InlineAppearParameters;
using Core::Frontend::InlineTextParameters;
using Core::Frontend::KeyboardInitializeParameters;
using Service::AM::Frontend::SwkbdReplyType;
using Service::AM::Frontend::SwkbdResult;
using Service::AM::Frontend::SwkbdTextCheckResult;
using Service::AM::Frontend::SwkbdType;

namespace {

struct KeyboardResponse {
    bool accepted{};
    bool silent{};
    std::u16string text;
};

#ifdef _WIN32

constexpr wchar_t kKeyboardClass[] = L"ScarletPCSoftwareKeyboard";
constexpr UINT kForceCloseMessage = WM_APP + 0x251;
constexpr UINT kUpdateTextMessage = WM_APP + 0x252;

constexpr int kIdBackspace = 600;
constexpr int kIdShift = 601;
constexpr int kIdSpace = 602;
constexpr int kKeyBase = 1000;

std::atomic<HWND> g_keyboard_window{nullptr};

std::wstring ToWide(std::u16string_view value) {
    std::wstring out;
    out.reserve(value.size());
    for (const char16_t ch : value) {
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

std::u16string ToU16(std::wstring_view value) {
    std::u16string out;
    out.reserve(value.size());
    for (const wchar_t ch : value) {
        out.push_back(static_cast<char16_t>(ch));
    }
    return out;
}

std::u16string GetEditText(HWND edit) {
    const int length = GetWindowTextLengthW(edit);
    if (length <= 0) {
        return {};
    }
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(edit, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return ToU16(buffer);
}

std::size_t CharacterCount(std::u16string_view text) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char16_t ch = text[i];
        if (ch >= 0xD800 && ch <= 0xDBFF && i + 1 < text.size()) {
            const char16_t lo = text[i + 1];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                ++i;
            }
        }
        ++count;
    }
    return count;
}

HWND FindGameWindow() {
    const DWORD pid = GetCurrentProcessId();

    HWND foreground = GetForegroundWindow();
    if (foreground != nullptr) {
        DWORD owner_pid{};
        GetWindowThreadProcessId(foreground, &owner_pid);
        if (owner_pid == pid && IsWindowVisible(foreground)) {
            return foreground;
        }
    }

    struct Search {
        DWORD pid;
        HWND result;
    } search{pid, nullptr};

    EnumWindows(
        [](HWND hwnd, LPARAM data) -> BOOL {
            auto& search = *reinterpret_cast<Search*>(data);
            DWORD owner_pid{};
            GetWindowThreadProcessId(hwnd, &owner_pid);
            if (owner_pid != search.pid || !IsWindowVisible(hwnd)) {
                return TRUE;
            }
            wchar_t title[256]{};
            GetWindowTextW(hwnd, title, static_cast<int>(std::size(title)));
            if (title[0] == L'\0') {
                return TRUE;
            }
            search.result = hwnd;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.result;
}

bool CharacterDisabled(const KeyboardInitializeParameters& params, wchar_t ch) {
    const u32 flags = params.key_disable_flags.raw;
    if (ch == L' ' && (flags & (1u << 1))) {
        return true;
    }
    if (ch == L'@' && (flags & (1u << 2))) {
        return true;
    }
    if (ch == L'%' && (flags & (1u << 3))) {
        return true;
    }
    if (ch == L'/' && (flags & (1u << 4))) {
        return true;
    }
    if (ch == L'\\' && (flags & (1u << 5))) {
        return true;
    }
    if (std::iswdigit(ch) && (flags & (1u << 6))) {
        return true;
    }
    return false;
}

struct TextUpdate {
    std::u16string text;
    s32 cursor{};
};

struct DialogState {
    KeyboardInitializeParameters params;
    std::u16string error_text;
    HWND hwnd{};
    HWND edit{};
    HWND error_label{};
    HBRUSH background_brush{};
    HBRUSH edit_brush{};
    bool shift{};
    bool accepted{};
    bool silent{};
    bool done{};
    std::u16string final_text;
    std::vector<HWND> character_buttons;
    std::vector<wchar_t> character_values;
};

void ApplyFont(HWND control, HFONT font) {
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND CreateControl(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style, int x,
                   int y, int w, int h, int id, HFONT font) {
    HWND control = CreateWindowExW(0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h,
                                   parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
    if (control != nullptr) {
        ApplyFont(control, font);
    }
    return control;
}

void ReplaceSelection(HWND edit, std::wstring_view value) {
    std::wstring copy{value};
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(copy.c_str()));
    SetFocus(edit);
}

void Backspace(HWND edit) {
    DWORD start{}, end{};
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end && start > 0) {
        --start;
        SendMessageW(edit, EM_SETSEL, start, end);
    }
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
    SetFocus(edit);
}

void UpdateCharacterLabels(DialogState& state) {
    for (std::size_t index = 0; index < state.character_buttons.size(); ++index) {
        wchar_t value = state.character_values[index];
        if (std::iswalpha(value)) {
            value = state.shift ? static_cast<wchar_t>(std::towupper(value))
                                : static_cast<wchar_t>(std::towlower(value));
        }
        wchar_t label[2]{value, L'\0'};
        SetWindowTextW(state.character_buttons[index], label);
    }
}

void SetError(DialogState& state, std::u16string_view message) {
    if (state.error_label == nullptr) {
        return;
    }
    const auto wide = ToWide(message);
    SetWindowTextW(state.error_label, wide.c_str());
}

bool ValidateAndAccept(DialogState& state) {
    const auto text = GetEditText(state.edit);
    const std::size_t count = CharacterCount(text);
    const u32 min_length = state.params.min_text_length;
    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;

    if (count < min_length || count > max_length) {
        wchar_t message[160]{};
        if (min_length == max_length) {
            swprintf_s(message, L"Digite exatamente %u caracteres.", min_length);
        } else if (min_length > 0) {
            swprintf_s(message, L"Digite entre %u e %u caracteres.", min_length, max_length);
        } else {
            swprintf_s(message, L"Digite no máximo %u caracteres.", max_length);
        }
        SetError(state, ToU16(message));
        MessageBeep(MB_ICONWARNING);
        SetFocus(state.edit);
        return false;
    }

    state.final_text = text;
    state.accepted = true;
    state.done = true;
    state.error_text.clear();
    DestroyWindow(state.hwnd);
    return true;
}

LRESULT CALLBACK KeyboardWindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    DialogState* state =
        reinterpret_cast<DialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = reinterpret_cast<DialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        state->hwnd = hwnd;
        return TRUE;
    }

    if (state == nullptr) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (notification != BN_CLICKED) {
            break;
        }

        if (id >= kKeyBase &&
            id < kKeyBase + static_cast<int>(state->character_values.size())) {
            wchar_t value = state->character_values[static_cast<std::size_t>(id - kKeyBase)];
            if (std::iswalpha(value)) {
                value = state->shift ? static_cast<wchar_t>(std::towupper(value))
                                     : static_cast<wchar_t>(std::towlower(value));
            }
            if (!CharacterDisabled(state->params, value)) {
                wchar_t text[2]{value, L'\0'};
                ReplaceSelection(state->edit, text);
            }
            return 0;
        }

        switch (id) {
        case kIdBackspace:
            Backspace(state->edit);
            return 0;
        case kIdShift:
            state->shift = !state->shift;
            UpdateCharacterLabels(*state);
            SetFocus(state->edit);
            return 0;
        case kIdSpace:
            if (!CharacterDisabled(state->params, L' ')) {
                ReplaceSelection(state->edit, L" ");
            }
            return 0;
        case IDOK:
            ValidateAndAccept(*state);
            return 0;
        case IDCANCEL:
            if (!state->params.disable_cancel_button) {
                state->done = true;
                state->accepted = false;
                DestroyWindow(hwnd);
            }
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_CLOSE:
        if (!state->params.disable_cancel_button) {
            state->done = true;
            state->accepted = false;
            DestroyWindow(hwnd);
        }
        return 0;
    case kForceCloseMessage:
        state->silent = wparam != 0;
        state->done = true;
        state->accepted = false;
        DestroyWindow(hwnd);
        return 0;
    case kUpdateTextMessage: {
        std::unique_ptr<TextUpdate> update{reinterpret_cast<TextUpdate*>(lparam)};
        if (update) {
            const auto text = ToWide(update->text);
            SetWindowTextW(state->edit, text.c_str());
            const auto cursor = static_cast<WPARAM>(
                std::clamp<s32>(update->cursor, 0, static_cast<s32>(update->text.size())));
            SendMessageW(state->edit, EM_SETSEL, cursor, cursor);
        }
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(242, 245, 250));
        return reinterpret_cast<LRESULT>(state->background_brush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(245, 247, 251));
        SetTextColor(dc, RGB(20, 27, 37));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    case WM_ERASEBKGND: {
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(reinterpret_cast<HDC>(wparam), &rect, state->background_brush);
        return 1;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}

void RegisterKeyboardClass() {
    static const bool registered = [] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = KeyboardWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kKeyboardClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
}

void AddCharacterRow(DialogState& state, HFONT font, std::wstring_view characters, int y,
                     int button_width, int button_height, int gap, int client_width) {
    const int total_width =
        static_cast<int>(characters.size()) * button_width +
        std::max(0, static_cast<int>(characters.size()) - 1) * gap;
    int x = std::max(12, (client_width - total_width) / 2);

    for (const wchar_t ch : characters) {
        const int id = kKeyBase + static_cast<int>(state.character_values.size());
        wchar_t label[2]{ch, L'\0'};
        HWND button = CreateControl(state.hwnd, L"BUTTON", label, BS_PUSHBUTTON | WS_TABSTOP, x, y,
                                    button_width, button_height, id, font);
        state.character_buttons.push_back(button);
        state.character_values.push_back(ch);
        if (CharacterDisabled(state.params, ch)) {
            EnableWindow(button, FALSE);
        }
        x += button_width + gap;
    }
}

void BuildKeyboardUi(DialogState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int width = client.right - client.left;

    HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));

    std::u16string title = state.params.header_text;
    if (title.empty()) {
        title = u"Digite o texto";
    }
    const auto title_w = ToWide(title);
    CreateControl(state.hwnd, L"STATIC", title_w, SS_CENTER, 24, 18, width - 48, 28, 10, font);

    if (!state.params.sub_text.empty()) {
        const auto sub = ToWide(state.params.sub_text);
        CreateControl(state.hwnd, L"STATIC", sub, SS_CENTER, 24, 48, width - 48, 24, 11, font);
    }

    if (!state.params.guide_text.empty()) {
        const auto guide = ToWide(state.params.guide_text);
        CreateControl(state.hwnd, L"STATIC", guide, SS_CENTER, 24, 72, width - 48, 24, 12, font);
    }

    state.error_label =
        CreateControl(state.hwnd, L"STATIC", ToWide(state.error_text), SS_CENTER, 24, 98,
                      width - 48, 28, 13, font);

    DWORD edit_style = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (state.params.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled) {
        edit_style |= ES_PASSWORD;
    }

    state.edit = CreateControl(state.hwnd, L"EDIT", ToWide(state.params.initial_text), edit_style,
                               90, 132, width - 180, 36, 14, font);

    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;
    SendMessageW(state.edit, EM_SETLIMITTEXT, max_length, 0);

    if (state.params.initial_cursor_position <= 0) {
        SendMessageW(state.edit, EM_SETSEL, 0, 0);
    } else {
        SendMessageW(state.edit, EM_SETSEL, -1, -1);
    }

    const bool number_pad = state.params.type == SwkbdType::NumberPad;
    const int key_h = 44;
    const int gap = 6;
    int y = 190;

    if (number_pad) {
        AddCharacterRow(state, font, L"123", y, 82, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"456", y, 82, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"789", y, 82, key_h, gap, width);
        y += key_h + gap;

        std::wstring last;
        if (state.params.left_optional_symbol_key != 0) {
            last.push_back(static_cast<wchar_t>(state.params.left_optional_symbol_key));
        }
        last.push_back(L'0');
        if (state.params.right_optional_symbol_key != 0) {
            last.push_back(static_cast<wchar_t>(state.params.right_optional_symbol_key));
        }
        AddCharacterRow(state, font, last, y, 82, key_h, gap, width);
        y += key_h + 12;
    } else {
        AddCharacterRow(state, font, L"1234567890", y, 66, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"qwertyuiop", y, 66, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"asdfghjkl", y, 66, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"zxcvbnm", y, 66, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, font, L"-/.,?!@%", y, 66, key_h, gap, width);
        y += key_h + 12;

        CreateControl(state.hwnd, L"BUTTON", L"Shift", BS_PUSHBUTTON | WS_TABSTOP,
                      110, y, 110, 42, kIdShift, font);
        HWND space = CreateControl(state.hwnd, L"BUTTON", L"Espaço", BS_PUSHBUTTON | WS_TABSTOP,
                                   230, y, width - 460, 42, kIdSpace, font);
        if (CharacterDisabled(state.params, L' ')) {
            EnableWindow(space, FALSE);
        }
        CreateControl(state.hwnd, L"BUTTON", L"Apagar", BS_PUSHBUTTON | WS_TABSTOP,
                      width - 220, y, 110, 42, kIdBackspace, font);
        y += 54;
    }

    const std::wstring ok_text =
        state.params.ok_text.empty() ? std::wstring{L"Confirmar"} : ToWide(state.params.ok_text);

    const int button_width = 170;
    const int button_gap = 12;
    const bool show_cancel = !state.params.disable_cancel_button;
    const int buttons = show_cancel ? 2 : 1;
    const int total = buttons * button_width + (buttons - 1) * button_gap;
    int bx = (width - total) / 2;

    if (show_cancel) {
        CreateControl(state.hwnd, L"BUTTON", L"Cancelar", BS_PUSHBUTTON | WS_TABSTOP, bx, y,
                      button_width, 42, IDCANCEL, font);
        bx += button_width + button_gap;
    }

    CreateControl(state.hwnd, L"BUTTON", ok_text, BS_DEFPUSHBUTTON | WS_TABSTOP, bx, y,
                  button_width, 42, IDOK, font);

    SetFocus(state.edit);
}

KeyboardResponse RunKeyboard(KeyboardInitializeParameters params, std::u16string initial,
                             std::u16string error) {
    RegisterKeyboardClass();

    params.initial_text = std::move(initial);

    DialogState state{
        .params = std::move(params),
        .error_text = std::move(error),
        .background_brush = CreateSolidBrush(RGB(18, 34, 54)),
        .edit_brush = CreateSolidBrush(RGB(245, 247, 251)),
    };

    HWND owner = FindGameWindow();

    RECT owner_rect{};
    if (owner != nullptr) {
        GetWindowRect(owner, &owner_rect);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
    }

    const int owner_w = owner_rect.right - owner_rect.left;
    const int owner_h = owner_rect.bottom - owner_rect.top;
    const int width = std::clamp(owner_w - 80, 760, 1020);
    const int height = std::clamp(owner_h - 60, 560, 680);
    const int x = owner_rect.left + (owner_w - width) / 2;
    const int y = owner_rect.top + (owner_h - height) / 2;

    state.hwnd = CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_TOOLWINDOW, kKeyboardClass, L"Pokémon Scarlet - Teclado",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
        GetModuleHandleW(nullptr), &state);

    if (state.hwnd == nullptr) {
        DeleteObject(state.background_brush);
        DeleteObject(state.edit_brush);
        LOG_ERROR(Service_AM, "[Scarlet.Keyboard] Failed to create Windows keyboard window");
        return {};
    }

    BuildKeyboardUi(state);
    g_keyboard_window.store(state.hwnd, std::memory_order_release);

    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    SetForegroundWindow(state.hwnd);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(state.hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    g_keyboard_window.store(nullptr, std::memory_order_release);

    KeyboardResponse response;
    response.accepted = state.accepted;
    response.silent = state.silent;
    if (state.accepted) {
        response.text = std::move(state.final_text);
    }

    DeleteObject(state.background_brush);
    DeleteObject(state.edit_brush);

    if (owner != nullptr) {
        SetForegroundWindow(owner);
    }

    return response;
}

void CloseKeyboard(bool silent) {
    if (HWND hwnd = g_keyboard_window.load(std::memory_order_acquire); hwnd != nullptr) {
        PostMessageW(hwnd, kForceCloseMessage, silent ? 1 : 0, 0);
    }
}

void UpdateKeyboardText(const InlineTextParameters& params) {
    if (HWND hwnd = g_keyboard_window.load(std::memory_order_acquire); hwnd != nullptr) {
        auto* update = new TextUpdate{params.input_text, params.cursor_position};
        if (!PostMessageW(hwnd, kUpdateTextMessage, 0, reinterpret_cast<LPARAM>(update))) {
            delete update;
        }
    }
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
#ifdef _WIN32
            const auto wide_message = ToWide(message.empty() ? u"Usar este texto?" : message);
            const int choice =
                MessageBoxW(FindGameWindow(), wide_message.c_str(), L"Pokémon Scarlet",
                            MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND);
            if (choice == IDOK) {
                submit_normal_callback(SwkbdResult::Ok, current_text, true);
                return;
            }
#endif
            auto response = RunKeyboard(params, initial, {});
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
