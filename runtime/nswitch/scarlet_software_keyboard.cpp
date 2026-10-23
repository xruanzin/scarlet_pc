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

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
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
constexpr int kIdSymbols = 603;
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
            GetWindowTextW(hwnd, title, 256);
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
    HWND counter_label{};
    HWND shift_button{};
    HBRUSH background_brush{};
    HBRUSH panel_brush{};
    HBRUSH edit_brush{};
    HFONT font_title{};
    HFONT font_text{};
    HFONT font_key{};
    HFONT font_input{};
    bool shift{};
    bool accepted{};
    bool silent{};
    bool done{};
    std::u16string final_text;
    std::vector<HWND> character_buttons;
    std::vector<wchar_t> character_values;
    std::vector<std::vector<HWND>> nav_rows;
    WORD previous_gamepad_buttons{};
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

HFONT CreateUiFont(HWND hwnd, int points, int weight = FW_NORMAL) {
    const UINT dpi = GetDpiForWindow(hwnd);
    const int height = -MulDiv(points, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

HWND CreateKeyButton(DialogState& state, const std::wstring& text, int x, int y, int w, int h,
                     int id) {
    return CreateControl(state.hwnd, L"BUTTON", text, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id,
                         state.font_key);
}

void AddNavRow(DialogState& state, std::vector<HWND> row) {
    row.erase(std::remove(row.begin(), row.end(), nullptr), row.end());
    if (!row.empty()) {
        state.nav_rows.push_back(std::move(row));
    }
}

void ReplaceSelection(HWND edit, std::wstring_view value) {
    std::wstring copy{value};
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(copy.c_str()));
}

void Backspace(HWND edit) {
    DWORD start{}, end{};
    SendMessageW(edit, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
    if (start == end && start > 0) {
        --start;
        SendMessageW(edit, EM_SETSEL, start, end);
    }
    SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L""));
}

void UpdateCounter(DialogState& state) {
    if (state.counter_label == nullptr || state.edit == nullptr) {
        return;
    }
    const auto text = GetEditText(state.edit);
    const auto count = CharacterCount(text);
    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;
    wchar_t label[64]{};
    swprintf_s(label, L"%zu / %u", count, max_length);
    SetWindowTextW(state.counter_label, label);
}

RECT ScreenRect(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}

void FocusControl(HWND control) {
    if (control != nullptr && IsWindowEnabled(control) && IsWindowVisible(control)) {
        SetFocus(control);
        InvalidateRect(control, nullptr, TRUE);
    }
}

void Navigate(DialogState& state, int dx, int dy) {
    if (state.nav_rows.empty()) {
        return;
    }

    HWND focused = GetFocus();
    int row_index = -1;
    int col_index = -1;
    for (int r = 0; r < static_cast<int>(state.nav_rows.size()); ++r) {
        for (int c = 0; c < static_cast<int>(state.nav_rows[r].size()); ++c) {
            if (state.nav_rows[r][c] == focused) {
                row_index = r;
                col_index = c;
                break;
            }
        }
        if (row_index >= 0) {
            break;
        }
    }

    if (row_index < 0) {
        FocusControl(state.nav_rows.front().front());
        return;
    }

    if (dx != 0) {
        auto& row = state.nav_rows[row_index];
        int c = col_index;
        for (int attempts = 0; attempts < static_cast<int>(row.size()); ++attempts) {
            c = (c + dx + static_cast<int>(row.size())) % static_cast<int>(row.size());
            if (IsWindowEnabled(row[c])) {
                FocusControl(row[c]);
                return;
            }
        }
        return;
    }

    if (dy != 0) {
        RECT current = ScreenRect(focused);
        const int current_x = (current.left + current.right) / 2;
        int r = row_index;
        for (int attempts = 0; attempts < static_cast<int>(state.nav_rows.size()); ++attempts) {
            r = (r + dy + static_cast<int>(state.nav_rows.size())) %
                static_cast<int>(state.nav_rows.size());
            HWND best = nullptr;
            int best_distance = INT_MAX;
            for (HWND candidate : state.nav_rows[r]) {
                if (!IsWindowEnabled(candidate)) {
                    continue;
                }
                RECT rect = ScreenRect(candidate);
                const int center_x = (rect.left + rect.right) / 2;
                const int distance = std::abs(center_x - current_x);
                if (distance < best_distance) {
                    best_distance = distance;
                    best = candidate;
                }
            }
            if (best != nullptr) {
                FocusControl(best);
                return;
            }
        }
    }
}

void ActivateFocused(DialogState& state) {
    HWND focused = GetFocus();
    if (focused == nullptr || focused == state.edit) {
        if (!state.nav_rows.empty()) {
            FocusControl(state.nav_rows.front().front());
        }
        return;
    }
    const int id = GetDlgCtrlID(focused);
    if (id != 0) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                     reinterpret_cast<LPARAM>(focused));
    }
}

void PollGamepad(DialogState& state) {
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    static HMODULE xinput = [] {
        HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        if (module == nullptr) module = LoadLibraryW(L"xinput9_1_0.dll");
        if (module == nullptr) module = LoadLibraryW(L"xinput1_3.dll");
        return module;
    }();
    static XInputGetStateFn get_state =
        xinput != nullptr ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(xinput, "XInputGetState"))
                          : nullptr;
    if (get_state == nullptr) {
        return;
    }

    XINPUT_STATE pad{};
    bool connected = false;
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        if (get_state(index, &pad) == ERROR_SUCCESS) {
            connected = true;
            break;
        }
    }
    if (!connected) {
        state.previous_gamepad_buttons = 0;
        return;
    }

    const WORD current = pad.Gamepad.wButtons;
    const WORD pressed = static_cast<WORD>(current & ~state.previous_gamepad_buttons);
    state.previous_gamepad_buttons = current;

    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) Navigate(state, -1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) Navigate(state, 1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) Navigate(state, 0, -1);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) Navigate(state, 0, 1);
    if (pressed & XINPUT_GAMEPAD_A) ActivateFocused(state);
    if (pressed & XINPUT_GAMEPAD_B) {
        if (!state.params.disable_cancel_button) {
            SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
        }
    }
    if (pressed & XINPUT_GAMEPAD_X) Backspace(state.edit);
    if (pressed & XINPUT_GAMEPAD_Y && state.shift_button != nullptr) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(kIdShift, BN_CLICKED),
                     reinterpret_cast<LPARAM>(state.shift_button));
    }
    if (pressed & XINPUT_GAMEPAD_START) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    }
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
        InvalidateRect(state.character_buttons[index], nullptr, TRUE);
    }
    if (state.shift_button != nullptr) {
        InvalidateRect(state.shift_button, nullptr, TRUE);
    }
}

void SetError(DialogState& state, std::u16string_view message) {
    state.error_text.assign(message.begin(), message.end());
    if (state.error_label == nullptr) {
        return;
    }
    const auto wide = ToWide(message);
    SetWindowTextW(state.error_label, wide.c_str());
    InvalidateRect(state.error_label, nullptr, TRUE);
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
        if (reinterpret_cast<HWND>(lparam) == state->edit && notification == EN_CHANGE) {
            UpdateCounter(*state);
            SetError(*state, std::u16string_view{});
            return 0;
        }
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
            return 0;
        case kIdSpace:
            if (!CharacterDisabled(state->params, L' ')) {
                ReplaceSelection(state->edit, L" ");
            }
            return 0;
        case kIdSymbols:
            for (const auto& row : state->nav_rows) {
                for (HWND button : row) {
                    if (GetDlgCtrlID(button) >= kKeyBase) {
                        wchar_t label[8]{};
                        GetWindowTextW(button, label, 8);
                        if (label[0] == L'-') {
                            FocusControl(button);
                            return 0;
                        }
                    }
                }
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
            UpdateCounter(*state);
        }
        return 0;
    }
    case WM_TIMER:
        PollGamepad(*state);
        return 0;
    case WM_DRAWITEM: {
        const auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (draw == nullptr || draw->CtlType != ODT_BUTTON) {
            break;
        }

        RECT rect = draw->rcItem;
        HDC dc = draw->hDC;
        const bool focused = (draw->itemState & ODS_FOCUS) != 0;
        const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
        const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        const int id = static_cast<int>(draw->CtlID);

        COLORREF fill = RGB(244, 245, 247);
        COLORREF text_color = disabled ? RGB(170, 173, 178) : RGB(38, 40, 45);
        COLORREF border = focused ? RGB(0, 187, 196) : RGB(205, 208, 213);

        if (id == IDOK) {
            fill = pressed ? RGB(32, 87, 198) : RGB(49, 105, 221);
            text_color = RGB(255, 255, 255);
            border = focused ? RGB(0, 207, 218) : fill;
        } else if (id == kIdShift && state->shift) {
            fill = RGB(75, 79, 87);
            text_color = RGB(255, 255, 255);
        } else if (pressed) {
            fill = RGB(220, 223, 228);
        }

        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, focused ? 3 : 1, border);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        RoundRect(dc, rect.left + 1, rect.top + 1, rect.right - 1, rect.bottom - 1, 10, 10);
        SelectObject(dc, old_pen);
        SelectObject(dc, old_brush);
        DeleteObject(pen);
        DeleteObject(brush);

        wchar_t label[96]{};
        GetWindowTextW(draw->hwndItem, label, 96);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, text_color);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(draw->hwndItem, WM_GETFONT, 0, 0));
        HGDIOBJ old_font = font != nullptr ? SelectObject(dc, font) : nullptr;
        DrawTextW(dc, label, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (old_font != nullptr) {
            SelectObject(dc, old_font);
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkMode(dc, TRANSPARENT);
        if (reinterpret_cast<HWND>(lparam) == state->error_label && !state->error_text.empty()) {
            SetTextColor(dc, RGB(200, 52, 60));
        } else {
            SetTextColor(dc, RGB(43, 45, 50));
        }
        return reinterpret_cast<LRESULT>(state->background_brush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(25, 27, 31));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, state->background_brush);
        RECT panel = rect;
        panel.bottom = std::min(rect.bottom, 170);
        FillRect(dc, &panel, state->panel_brush);
        HPEN line = CreatePen(PS_SOLID, 1, RGB(215, 218, 223));
        HGDIOBJ old = SelectObject(dc, line);
        MoveToEx(dc, 0, panel.bottom - 1, nullptr);
        LineTo(dc, rect.right, panel.bottom - 1);
        SelectObject(dc, old);
        DeleteObject(line);
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

void AddCharacterRow(DialogState& state, HFONT, std::wstring_view characters, int y,
                     int button_width, int button_height, int gap, int client_width) {
    const int total_width =
        static_cast<int>(characters.size()) * button_width +
        std::max(0, static_cast<int>(characters.size()) - 1) * gap;
    int x = std::max(12, (client_width - total_width) / 2);
    std::vector<HWND> nav_row;

    for (const wchar_t ch : characters) {
        const int id = kKeyBase + static_cast<int>(state.character_values.size());
        wchar_t label[2]{ch, L'\0'};
        HWND button = CreateKeyButton(state, label, x, y, button_width, button_height, id);
        state.character_buttons.push_back(button);
        state.character_values.push_back(ch);
        nav_row.push_back(button);
        if (CharacterDisabled(state.params, ch)) {
            EnableWindow(button, FALSE);
        }
        x += button_width + gap;
    }
    AddNavRow(state, std::move(nav_row));
}

void BuildKeyboardUi(DialogState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;

    state.font_title = CreateUiFont(state.hwnd, 18, FW_SEMIBOLD);
    state.font_text = CreateUiFont(state.hwnd, 12, FW_NORMAL);
    state.font_key = CreateUiFont(state.hwnd, 16, FW_SEMIBOLD);
    state.font_input = CreateUiFont(state.hwnd, 21, FW_NORMAL);

    std::u16string title = state.params.header_text;
    if (title.empty()) {
        title = u"Digite o texto";
    }
    const auto title_w = ToWide(title);
    CreateControl(state.hwnd, L"STATIC", title_w, SS_CENTER, 32, 18, width - 64, 30, 10,
                  state.font_title);

    if (!state.params.sub_text.empty()) {
        const auto sub = ToWide(state.params.sub_text);
        CreateControl(state.hwnd, L"STATIC", sub, SS_CENTER, 32, 48, width - 64, 22, 11,
                      state.font_text);
    }

    DWORD edit_style = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (state.params.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled) {
        edit_style |= ES_PASSWORD;
    }

    const int edit_w = std::min(width - 180, 900);
    const int edit_x = (width - edit_w) / 2;
    state.edit = CreateControl(state.hwnd, L"EDIT", ToWide(state.params.initial_text), edit_style,
                               edit_x, 78, edit_w, 48, 14, state.font_input);

    state.counter_label = CreateControl(state.hwnd, L"STATIC", L"", SS_RIGHT, edit_x, 130, edit_w,
                                        22, 15, state.font_text);

    state.error_label = CreateControl(state.hwnd, L"STATIC", ToWide(state.error_text), SS_CENTER,
                                      32, 146, width - 64, 22, 13, state.font_text);

    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;
    SendMessageW(state.edit, EM_SETLIMITTEXT, max_length, 0);
    if (state.params.initial_cursor_position <= 0) {
        SendMessageW(state.edit, EM_SETSEL, 0, 0);
    } else {
        SendMessageW(state.edit, EM_SETSEL, -1, -1);
    }
    UpdateCounter(state);

    const bool number_pad = state.params.type == SwkbdType::NumberPad;
    const int gap = std::clamp(width / 150, 6, 10);
    const int rows_needed = number_pad ? 6 : 7;
    const int key_h = std::clamp(
        (height - 188 - 20 - gap * (rows_needed - 1)) / rows_needed, 44, 64);
    int y = 188;

    if (number_pad) {
        const int key_w = std::clamp(width / 9, 72, 110);
        AddCharacterRow(state, state.font_key, L"123", y, key_w, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, state.font_key, L"456", y, key_w, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, state.font_key, L"789", y, key_w, key_h, gap, width);
        y += key_h + gap;

        std::wstring last;
        if (state.params.left_optional_symbol_key != 0) {
            last.push_back(static_cast<wchar_t>(state.params.left_optional_symbol_key));
        }
        last.push_back(L'0');
        if (state.params.right_optional_symbol_key != 0) {
            last.push_back(static_cast<wchar_t>(state.params.right_optional_symbol_key));
        }
        AddCharacterRow(state, state.font_key, last, y, key_w, key_h, gap, width);
        y += key_h + gap;

        std::vector<HWND> row;
        if (state.params.enable_backspace_button) {
            row.push_back(CreateKeyButton(state, L"⌫", width / 2 - 180, y, 170, key_h,
                                          kIdBackspace));
        }
        row.push_back(CreateKeyButton(state,
                                     state.params.ok_text.empty() ? L"OK" : ToWide(state.params.ok_text),
                                     width / 2 + 10, y, 170, key_h, IDOK));
        AddNavRow(state, std::move(row));
        y += key_h + gap;
    } else {
        const int key_w = std::clamp((width - 170) / 12, 48, 82);
        AddCharacterRow(state, state.font_key, L"1234567890", y, key_w, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, state.font_key, L"qwertyuiop", y, key_w, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, state.font_key, L"asdfghjkl", y, key_w, key_h, gap, width);
        y += key_h + gap;
        AddCharacterRow(state, state.font_key, L"zxcvbnm", y, key_w, key_h, gap, width);
        y += key_h + gap;

        const int special_w = std::clamp(width / 9, 90, 150);
        const int space_w = std::clamp(width / 3, 240, 440);
        const int total = special_w * 3 + space_w + gap * 3;
        int x = (width - total) / 2;
        std::vector<HWND> bottom;

        state.shift_button =
            CreateKeyButton(state, L"⇧ Shift", x, y, special_w, key_h, kIdShift);
        bottom.push_back(state.shift_button);
        x += special_w + gap;

        HWND symbols =
            CreateKeyButton(state, L"Símbolos", x, y, special_w, key_h, kIdSymbols);
        bottom.push_back(symbols);
        x += special_w + gap;

        HWND space = CreateKeyButton(state, L"Espaço", x, y, space_w, key_h, kIdSpace);
        if (CharacterDisabled(state.params, L' ')) {
            EnableWindow(space, FALSE);
        }
        bottom.push_back(space);
        x += space_w + gap;

        if (state.params.enable_backspace_button) {
            bottom.push_back(CreateKeyButton(state, L"⌫", x, y, special_w, key_h, kIdBackspace));
        }
        AddNavRow(state, std::move(bottom));
        y += key_h + gap;

        const int sym_w = std::clamp((width - 260) / 8, 52, 84);
        AddCharacterRow(state, state.font_key, L"-/,:?!@%", y, sym_w, key_h, gap, width);
        y += key_h + gap;
    }

    const bool show_cancel = !state.params.disable_cancel_button;
    const int action_w = std::clamp(width / 6, 150, 220);
    const int action_y = std::min(height - key_h - 18, y + 4);
    std::vector<HWND> action_row;
    if (show_cancel) {
        action_row.push_back(CreateKeyButton(state, L"B   Cancelar", width / 2 - action_w - gap,
                                             action_y, action_w, key_h, IDCANCEL));
    }
    const std::wstring ok_text =
        state.params.ok_text.empty() ? std::wstring{L"A   OK"} : ToWide(state.params.ok_text);
    action_row.push_back(CreateKeyButton(state, ok_text, width / 2 + (show_cancel ? gap : -action_w / 2),
                                         action_y, action_w, key_h, IDOK));
    AddNavRow(state, std::move(action_row));

    if (!state.nav_rows.empty()) {
        FocusControl(state.nav_rows.front().front());
    } else {
        SetFocus(state.edit);
    }
}

KeyboardResponse RunKeyboard(KeyboardInitializeParameters params, std::u16string initial,
                             std::u16string error) {
    RegisterKeyboardClass();

    params.initial_text = std::move(initial);

    DialogState state{
        .params = std::move(params),
        .error_text = std::move(error),
        .background_brush = CreateSolidBrush(RGB(252, 252, 253)),
        .panel_brush = CreateSolidBrush(RGB(239, 241, 244)),
        .edit_brush = CreateSolidBrush(RGB(255, 255, 255)),
    };

    HWND owner = FindGameWindow();

    RECT owner_rect{};
    if (owner != nullptr) {
        GetClientRect(owner, &owner_rect);
        POINT origin{0, 0};
        ClientToScreen(owner, &origin);
        OffsetRect(&owner_rect, origin.x, origin.y);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &owner_rect, 0);
    }

    const int owner_w = owner_rect.right - owner_rect.left;
    const int owner_h = owner_rect.bottom - owner_rect.top;
    const int width = std::max(owner_w, 760);
    const int height = std::max(owner_h, 560);
    const int x = owner_rect.left;
    const int y = owner_rect.top;

    state.hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW, kKeyboardClass, L"Pokémon Scarlet - Teclado",
        WS_POPUP, x, y, width, height, owner, nullptr, GetModuleHandleW(nullptr), &state);

    if (state.hwnd == nullptr) {
        DeleteObject(state.background_brush);
        DeleteObject(state.panel_brush);
        DeleteObject(state.edit_brush);
        LOG_ERROR(Service_AM, "[Scarlet.Keyboard] Failed to create Windows keyboard window");
        return {};
    }

    BuildKeyboardUi(state);
    SetTimer(state.hwnd, 1, 16, nullptr);
    g_keyboard_window.store(state.hwnd, std::memory_order_release);

    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    SetForegroundWindow(state.hwnd);

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool handled = false;
        if (message.message == WM_KEYDOWN) {
            switch (message.wParam) {
            case VK_LEFT: Navigate(state, -1, 0); handled = true; break;
            case VK_RIGHT: Navigate(state, 1, 0); handled = true; break;
            case VK_UP: Navigate(state, 0, -1); handled = true; break;
            case VK_DOWN: Navigate(state, 0, 1); handled = true; break;
            case VK_RETURN:
                if (GetFocus() != state.edit) {
                    ActivateFocused(state);
                    handled = true;
                }
                break;
            case VK_ESCAPE:
                if (!state.params.disable_cancel_button) {
                    SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(IDCANCEL, BN_CLICKED), 0);
                    handled = true;
                }
                break;
            case VK_BACK:
                if (GetFocus() != state.edit) {
                    Backspace(state.edit);
                    handled = true;
                }
                break;
            default:
                break;
            }
        } else if (message.message == WM_CHAR && GetFocus() != state.edit &&
                   message.wParam >= 0x20 && message.wParam != 0x7F) {
            wchar_t typed[2]{static_cast<wchar_t>(message.wParam), L'\0'};
            if (!CharacterDisabled(state.params, typed[0])) {
                ReplaceSelection(state.edit, typed);
            }
            handled = true;
        }

        if (!handled && !IsDialogMessageW(state.hwnd, &message)) {
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

    KillTimer(state.hwnd, 1);
    DeleteObject(state.background_brush);
    DeleteObject(state.panel_brush);
    DeleteObject(state.edit_brush);
    if (state.font_title) DeleteObject(state.font_title);
    if (state.font_text) DeleteObject(state.font_text);
    if (state.font_key) DeleteObject(state.font_key);
    if (state.font_input) DeleteObject(state.font_input);

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
