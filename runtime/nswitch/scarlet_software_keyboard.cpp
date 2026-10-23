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
constexpr int kIdReturn = 603;
constexpr int kIdHeader = 610;
constexpr int kIdSub = 611;
constexpr int kIdGuide = 612;
constexpr int kIdCounter = 613;
constexpr int kIdError = 614;
constexpr int kIdLegend = 615;
constexpr int kKeyBase = 1000;

constexpr COLORREF kTransparentKey = RGB(1, 2, 3);
constexpr int kBaseWidth = 1280;
constexpr int kBaseHeight = 720;

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
        if (owner_pid == pid && IsWindowVisible(foreground) && GetParent(foreground) == nullptr) {
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
            if (owner_pid != search.pid || !IsWindowVisible(hwnd) || GetParent(hwnd) != nullptr) {
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
    if (ch == L' ' && (flags & (1u << 1))) return true;
    if (ch == L'@' && (flags & (1u << 2))) return true;
    if (ch == L'%' && (flags & (1u << 3))) return true;
    if (ch == L'/' && (flags & (1u << 4))) return true;
    if (ch == L'\\' && (flags & (1u << 5))) return true;
    if (std::iswdigit(ch) && (flags & (1u << 6))) return true;
    return false;
}

struct TextUpdate {
    std::u16string text;
    s32 cursor{};
};

struct CharacterKey {
    HWND hwnd{};
    wchar_t lower{};
    wchar_t upper{};
};

struct DialogState {
    KeyboardInitializeParameters params;
    std::u16string error_text;
    HWND owner{};
    HWND scrim{};
    HWND hwnd{};
    HWND edit{};
    HWND error_label{};
    HWND counter_label{};
    HWND shift_button{};
    HWND return_button{};
    HWND ok_button{};
    HBRUSH transparent_brush{};
    HBRUSH panel_brush{};
    HBRUSH edit_brush{};
    HFONT font_header{};
    HFONT font_sub{};
    HFONT font_editor{};
    HFONT font_key{};
    HFONT font_label{};
    bool shift{};
    bool accepted{};
    bool silent{};
    bool done{};
    bool attached_input{};
    DWORD owner_thread{};
    std::u16string final_text;
    std::vector<CharacterKey> character_keys;
    std::vector<std::vector<HWND>> nav_rows;
    WORD previous_gamepad_buttons{};
};

int ScaleX(HWND hwnd, int value) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return MulDiv(value, rect.right - rect.left, kBaseWidth);
}

int ScaleY(HWND hwnd, int value) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return MulDiv(value, rect.bottom - rect.top, kBaseHeight);
}

HFONT CreateUiFont(HWND hwnd, int base_points, int weight = FW_NORMAL) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    const int scaled_points =
        std::max(8, MulDiv(base_points, std::max(1L, rect.bottom - rect.top), kBaseHeight));
    const UINT dpi = GetDpiForWindow(hwnd);
    const int height = -MulDiv(scaled_points, static_cast<int>(dpi), 72);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void ApplyFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

HWND CreateControl(HWND parent, const wchar_t* cls, const std::wstring& text, DWORD style, int x,
                   int y, int w, int h, int id, HFONT font) {
    HWND control = CreateWindowExW(
        0, cls, text.c_str(), WS_CHILD | WS_VISIBLE | style, x, y, w, h, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    ApplyFont(control, font);
    return control;
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

HWND AddCharacter(DialogState& state, wchar_t lower, wchar_t upper, int x, int y, int w, int h) {
    const int id = kKeyBase + static_cast<int>(state.character_keys.size());
    wchar_t label[2]{lower, L'\0'};
    HWND button = CreateKeyButton(state, label, x, y, w, h, id);
    state.character_keys.push_back({button, lower, upper});
    if (CharacterDisabled(state.params, lower) && CharacterDisabled(state.params, upper)) {
        EnableWindow(button, FALSE);
    }
    return button;
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
    if (state.counter_label == nullptr || state.edit == nullptr) return;
    const auto text = GetEditText(state.edit);
    const auto count = CharacterCount(text);
    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;
    wchar_t label[64]{};
    swprintf_s(label, L"%zu/%u", count, max_length);
    SetWindowTextW(state.counter_label, label);
}

void SetError(DialogState& state, std::u16string_view message) {
    state.error_text.assign(message.begin(), message.end());
    if (state.error_label != nullptr) {
        const auto wide = ToWide(message);
        SetWindowTextW(state.error_label, wide.c_str());
        InvalidateRect(state.error_label, nullptr, TRUE);
    }
}

void FocusControl(HWND control) {
    if (control != nullptr && IsWindowEnabled(control) && IsWindowVisible(control)) {
        SetFocus(control);
        InvalidateRect(control, nullptr, TRUE);
    }
}

RECT ScreenRect(HWND hwnd) {
    RECT r{};
    GetWindowRect(hwnd, &r);
    return r;
}

void Navigate(DialogState& state, int dx, int dy) {
    if (state.nav_rows.empty()) return;

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
        if (row_index >= 0) break;
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

    RECT current = ScreenRect(focused);
    const int current_x = (current.left + current.right) / 2;
    int r = row_index;
    for (int attempts = 0; attempts < static_cast<int>(state.nav_rows.size()); ++attempts) {
        r = (r + dy + static_cast<int>(state.nav_rows.size())) %
            static_cast<int>(state.nav_rows.size());
        HWND best = nullptr;
        int best_distance = INT_MAX;
        for (HWND candidate : state.nav_rows[r]) {
            if (!IsWindowEnabled(candidate)) continue;
            RECT rect = ScreenRect(candidate);
            const int distance = std::abs(((rect.left + rect.right) / 2) - current_x);
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

void ActivateFocused(DialogState& state) {
    HWND focused = GetFocus();
    if (focused == nullptr || focused == state.edit) {
        FocusControl(state.nav_rows.front().front());
        return;
    }
    const int id = GetDlgCtrlID(focused);
    if (id != 0) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                     reinterpret_cast<LPARAM>(focused));
    }
}

void UpdateCharacterLabels(DialogState& state) {
    for (const auto& key : state.character_keys) {
        const wchar_t value = state.shift ? key.upper : key.lower;
        wchar_t label[2]{value, L'\0'};
        SetWindowTextW(key.hwnd, label);
        EnableWindow(key.hwnd, !CharacterDisabled(state.params, value));
        InvalidateRect(key.hwnd, nullptr, TRUE);
    }
    if (state.shift_button != nullptr) {
        InvalidateRect(state.shift_button, nullptr, TRUE);
    }
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
        return false;
    }

    state.final_text = text;
    state.accepted = true;
    state.done = true;
    DestroyWindow(state.hwnd);
    return true;
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
    if (get_state == nullptr) return;

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
    if (pressed & XINPUT_GAMEPAD_Y) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(kIdShift, BN_CLICKED),
                     reinterpret_cast<LPARAM>(state.shift_button));
    }
    if (pressed & XINPUT_GAMEPAD_START) {
        SendMessageW(state.hwnd, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
    }
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

        if (notification != BN_CLICKED) break;

        if (id >= kKeyBase &&
            id < kKeyBase + static_cast<int>(state->character_keys.size())) {
            const auto& key = state->character_keys[static_cast<std::size_t>(id - kKeyBase)];
            const wchar_t value = state->shift ? key.upper : key.lower;
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
        case kIdReturn:
            if (state->params.enable_return_button) {
                ReplaceSelection(state->edit, L"\n");
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
        if (draw == nullptr || draw->CtlType != ODT_BUTTON) break;

        RECT rect = draw->rcItem;
        HDC dc = draw->hDC;
        const bool focused = (draw->itemState & ODS_FOCUS) != 0;
        const bool pressed = (draw->itemState & ODS_SELECTED) != 0;
        const bool disabled = (draw->itemState & ODS_DISABLED) != 0;
        const int id = static_cast<int>(draw->CtlID);

        COLORREF fill = RGB(247, 247, 247);
        COLORREF text_color = disabled ? RGB(175, 175, 175) : RGB(25, 25, 25);
        COLORREF border = focused ? RGB(82, 238, 184) : RGB(225, 225, 225);

        if (id == IDOK) {
            fill = pressed ? RGB(39, 65, 205) : RGB(51, 78, 238);
            text_color = RGB(255, 255, 255);
            border = focused ? RGB(82, 238, 184) : fill;
        } else if (id == kIdBackspace) {
            fill = pressed ? RGB(28, 28, 28) : RGB(45, 45, 45);
            text_color = RGB(255, 255, 255);
            border = focused ? RGB(82, 238, 184) : fill;
        } else if (id == kIdReturn && !state->params.enable_return_button) {
            fill = RGB(224, 224, 224);
            text_color = RGB(175, 175, 175);
        } else if (id == kIdShift && state->shift) {
            fill = RGB(55, 55, 55);
            text_color = RGB(255, 255, 255);
        } else if (pressed) {
            fill = RGB(224, 224, 224);
        }

        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, focused ? 3 : 1, border);
        HGDIOBJ old_brush = SelectObject(dc, brush);
        HGDIOBJ old_pen = SelectObject(dc, pen);
        Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
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

        if ((id == IDOK || id == kIdBackspace) && !disabled) {
            const wchar_t* badge_text = id == IDOK ? L"A" : L"B";
            const int radius = std::max(9, static_cast<int>((rect.bottom - rect.top) / 8));
            const int cx = rect.right - radius - 8;
            const int cy = rect.top + radius + 8;
            HBRUSH badge = CreateSolidBrush(RGB(255, 255, 255));
            HPEN badge_pen = CreatePen(PS_SOLID, 1, RGB(210, 210, 210));
            old_brush = SelectObject(dc, badge);
            old_pen = SelectObject(dc, badge_pen);
            Ellipse(dc, cx - radius, cy - radius, cx + radius, cy + radius);
            SelectObject(dc, old_pen);
            SelectObject(dc, old_brush);
            DeleteObject(badge_pen);
            DeleteObject(badge);
            SetTextColor(dc, RGB(55, 55, 55));
            RECT badge_rect{cx - radius, cy - radius, cx + radius, cy + radius};
            DrawTextW(dc, badge_text, -1, &badge_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        if (old_font != nullptr) SelectObject(dc, old_font);
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        HWND control = reinterpret_cast<HWND>(lparam);
        const int id = GetDlgCtrlID(control);
        SetBkMode(dc, TRANSPARENT);

        if (id == kIdLegend) {
            SetTextColor(dc, RGB(45, 45, 45));
            return reinterpret_cast<LRESULT>(state->panel_brush);
        }

        if (id == kIdError && !state->error_text.empty()) {
            SetTextColor(dc, RGB(255, 120, 120));
        } else {
            SetTextColor(dc, RGB(255, 255, 255));
        }
        return reinterpret_cast<LRESULT>(state->transparent_brush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetBkColor(dc, RGB(255, 255, 255));
        SetTextColor(dc, RGB(25, 25, 25));
        return reinterpret_cast<LRESULT>(state->edit_brush);
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        FillRect(dc, &rect, state->transparent_brush);
        RECT keyboard = rect;
        keyboard.top = ScaleY(hwnd, 320);
        FillRect(dc, &keyboard, state->panel_brush);
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
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kKeyboardClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    (void)registered;
}

void BuildNormalKeyboard(DialogState& state) {
    const int row_y[] = {335, 398, 461, 524, 587};
    const int key_h = 63;
    const int margin_x = 54;
    const int key_w = 96;
    const int special_w = 116;

    auto X = [&](int v) { return ScaleX(state.hwnd, v); };
    auto Y = [&](int v) { return ScaleY(state.hwnd, v); };
    auto W = [&](int v) { return ScaleX(state.hwnd, v); };
    auto H = [&](int v) { return ScaleY(state.hwnd, v); };
    auto col_x = [&](int col) { return margin_x + (col - 1) * key_w; };

    struct Pair {
        wchar_t lower;
        wchar_t upper;
    };

    static constexpr std::array<Pair, 11> row1{{
        {L'1', L'#'}, {L'2', L'['}, {L'3', L']'}, {L'4', L'$'}, {L'5', L'%'},
        {L'6', L'^'}, {L'7', L'&'}, {L'8', L'*'}, {L'9', L'('}, {L'0', L')'},
        {L'-', L'_'},
    }};
    static constexpr std::array<Pair, 11> row2{{
        {L'q', L'Q'}, {L'w', L'W'}, {L'e', L'E'}, {L'r', L'R'}, {L't', L'T'},
        {L'y', L'Y'}, {L'u', L'U'}, {L'i', L'I'}, {L'o', L'O'}, {L'p', L'P'},
        {L'/', L'@'},
    }};
    static constexpr std::array<Pair, 11> row3{{
        {L'a', L'A'}, {L's', L'S'}, {L'd', L'D'}, {L'f', L'F'}, {L'g', L'G'},
        {L'h', L'H'}, {L'j', L'J'}, {L'k', L'K'}, {L'l', L'L'}, {L':', L';'},
        {L'\'', L'"'},
    }};
    static constexpr std::array<Pair, 11> row4{{
        {L'z', L'Z'}, {L'x', L'X'}, {L'c', L'C'}, {L'v', L'V'}, {L'b', L'B'},
        {L'n', L'N'}, {L'm', L'M'}, {L',', L'<'}, {L'.', L'>'}, {L'?', L'+'},
        {L'!', L'='},
    }};

    const std::array<const Pair*, 4> data{row1.data(), row2.data(), row3.data(), row4.data()};
    for (int r = 0; r < 4; ++r) {
        std::vector<HWND> nav;
        for (int c = 0; c < 11; ++c) {
            nav.push_back(AddCharacter(state, data[r][c].lower, data[r][c].upper,
                                       X(col_x(c + 1)), Y(row_y[r]), W(key_w), H(key_h)));
        }

        if (r == 0) {
            HWND backspace =
                CreateKeyButton(state, L"⌫", X(col_x(12)), Y(row_y[r]), W(special_w), H(key_h),
                                kIdBackspace);
            nav.push_back(backspace);
        } else if (r == 1) {
            state.return_button =
                CreateKeyButton(state, L"Return", X(col_x(12)), Y(row_y[r]), W(special_w),
                                H(key_h * 2), kIdReturn);
            EnableWindow(state.return_button, state.params.enable_return_button);
            nav.push_back(state.return_button);
        } else if (r == 2) {
            nav.push_back(state.return_button);
        } else {
            const std::wstring ok_text =
                state.params.ok_text.empty() ? std::wstring{L"OK"} : ToWide(state.params.ok_text);
            state.ok_button =
                CreateKeyButton(state, ok_text, X(col_x(12)), Y(row_y[r]), W(special_w),
                                H(key_h * 2), IDOK);
            nav.push_back(state.ok_button);
        }
        AddNavRow(state, std::move(nav));
    }

    std::vector<HWND> bottom;
    state.shift_button =
        CreateKeyButton(state, L"⇧", X(col_x(1)), Y(row_y[4]), W(key_w * 2), H(key_h), kIdShift);
    bottom.push_back(state.shift_button);

    HWND space = CreateKeyButton(state, L"Space", X(col_x(3)), Y(row_y[4]), W(key_w * 9), H(key_h),
                                 kIdSpace);
    if (CharacterDisabled(state.params, L' ')) EnableWindow(space, FALSE);
    bottom.push_back(space);
    bottom.push_back(state.ok_button);
    AddNavRow(state, std::move(bottom));

    CreateControl(state.hwnd, L"STATIC",
                  L"L  ←      R  →          ●  Shift          X  Cancel          A  Enter",
                  SS_CENTER, X(54), Y(654), W(1172), H(52), kIdLegend, state.font_label);
}

void BuildNumberPad(DialogState& state) {
    auto X = [&](int v) { return ScaleX(state.hwnd, v); };
    auto Y = [&](int v) { return ScaleY(state.hwnd, v); };
    auto W = [&](int v) { return ScaleX(state.hwnd, v); };
    auto H = [&](int v) { return ScaleY(state.hwnd, v); };

    const int key_w = 180;
    const int key_h = 76;
    const int gap = 4;
    const int start_x = (kBaseWidth - (key_w * 4 + gap * 3)) / 2;
    const int start_y = 348;
    static constexpr wchar_t digits[4][3] = {
        {L'1', L'2', L'3'},
        {L'4', L'5', L'6'},
        {L'7', L'8', L'9'},
        {L'\0', L'0', L'\0'},
    };

    for (int r = 0; r < 4; ++r) {
        std::vector<HWND> nav;
        for (int c = 0; c < 3; ++c) {
            wchar_t ch = digits[r][c];
            if (r == 3 && c == 0 && state.params.left_optional_symbol_key != 0) {
                ch = static_cast<wchar_t>(state.params.left_optional_symbol_key);
            }
            if (r == 3 && c == 2 && state.params.right_optional_symbol_key != 0) {
                ch = static_cast<wchar_t>(state.params.right_optional_symbol_key);
            }
            if (ch != L'\0') {
                nav.push_back(AddCharacter(state, ch, ch, X(start_x + c * (key_w + gap)),
                                           Y(start_y + r * (key_h + gap)), W(key_w), H(key_h)));
            }
        }

        if (r == 0) {
            nav.push_back(CreateKeyButton(state, L"⌫",
                                          X(start_x + 3 * (key_w + gap)), Y(start_y),
                                          W(key_w), H(key_h), kIdBackspace));
        } else if (r == 1) {
            const std::wstring ok_text =
                state.params.ok_text.empty() ? std::wstring{L"OK"} : ToWide(state.params.ok_text);
            state.ok_button = CreateKeyButton(
                state, ok_text, X(start_x + 3 * (key_w + gap)), Y(start_y + key_h + gap),
                W(key_w), H(key_h * 3 + gap * 2), IDOK);
            nav.push_back(state.ok_button);
        } else {
            nav.push_back(state.ok_button);
        }
        AddNavRow(state, std::move(nav));
    }

    CreateControl(state.hwnd, L"STATIC", L"X  Cancel          A  Enter", SS_CENTER, X(54), Y(654),
                  W(1172), H(52), kIdLegend, state.font_label);
}

void BuildKeyboardUi(DialogState& state) {
    RECT client{};
    GetClientRect(state.hwnd, &client);

    state.font_header = CreateUiFont(state.hwnd, 23, FW_NORMAL);
    state.font_sub = CreateUiFont(state.hwnd, 17, FW_NORMAL);
    state.font_editor = CreateUiFont(state.hwnd, 26, FW_NORMAL);
    state.font_key = CreateUiFont(state.hwnd, 28, FW_NORMAL);
    state.font_label = CreateUiFont(state.hwnd, 18, FW_NORMAL);

    auto X = [&](int v) { return ScaleX(state.hwnd, v); };
    auto Y = [&](int v) { return ScaleY(state.hwnd, v); };
    auto W = [&](int v) { return ScaleX(state.hwnd, v); };
    auto H = [&](int v) { return ScaleY(state.hwnd, v); };

    const std::u16string title =
        state.params.header_text.empty() ? std::u16string{u"Enter Text"} : state.params.header_text;
    CreateControl(state.hwnd, L"STATIC", ToWide(title), SS_CENTER, X(130), Y(42), W(1020), H(48),
                  kIdHeader, state.font_header);

    if (!state.params.sub_text.empty()) {
        CreateControl(state.hwnd, L"STATIC", ToWide(state.params.sub_text), SS_CENTER, X(130), Y(92),
                      W(1020), H(28), kIdSub, state.font_sub);
    }

    if (!state.params.guide_text.empty()) {
        CreateControl(state.hwnd, L"STATIC", ToWide(state.params.guide_text), SS_CENTER, X(130),
                      Y(122), W(1020), H(28), kIdGuide, state.font_sub);
    }

    DWORD edit_style = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
    if (state.params.password_mode == Service::AM::Frontend::SwkbdPasswordMode::Enabled) {
        edit_style |= ES_PASSWORD;
    }

    state.edit = CreateControl(state.hwnd, L"EDIT", ToWide(state.params.initial_text), edit_style,
                               X(140), Y(170), W(1000), H(48), 14, state.font_editor);

    state.counter_label = CreateControl(state.hwnd, L"STATIC", L"", SS_RIGHT, X(130), Y(230),
                                        W(1020), H(28), kIdCounter, state.font_sub);

    state.error_label = CreateControl(state.hwnd, L"STATIC", ToWide(state.error_text), SS_CENTER,
                                      X(130), Y(270), W(1020), H(34), kIdError, state.font_sub);

    const u32 max_length = state.params.max_text_length > 0 ? state.params.max_text_length : 500;
    SendMessageW(state.edit, EM_SETLIMITTEXT, static_cast<WPARAM>(max_length), 0);
    if (state.params.initial_cursor_position <= 0) {
        SendMessageW(state.edit, EM_SETSEL, 0, 0);
    } else {
        SendMessageW(state.edit, EM_SETSEL, static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    }
    UpdateCounter(state);

    if (state.params.type == SwkbdType::NumberPad) {
        BuildNumberPad(state);
    } else {
        BuildNormalKeyboard(state);
    }

    UpdateCharacterLabels(state);
    if (!state.nav_rows.empty()) {
        FocusControl(state.nav_rows.front().front());
    }
}

KeyboardResponse RunKeyboard(KeyboardInitializeParameters params, std::u16string initial,
                             std::u16string error) {
    RegisterKeyboardClass();
    params.initial_text = std::move(initial);

    HWND owner = FindGameWindow();
    if (owner == nullptr) {
        LOG_ERROR(Service_AM, "[Scarlet.Keyboard] Game window unavailable; keyboard overlay not opened");
        return {};
    }

    RECT client{};
    GetClientRect(owner, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    if (width <= 0 || height <= 0) {
        LOG_ERROR(Service_AM, "[Scarlet.Keyboard] Invalid game client size");
        return {};
    }

    DialogState state{
        .params = std::move(params),
        .error_text = std::move(error),
        .owner = owner,
        .transparent_brush = CreateSolidBrush(kTransparentKey),
        .panel_brush = CreateSolidBrush(RGB(239, 239, 239)),
        .edit_brush = CreateSolidBrush(RGB(255, 255, 255)),
    };

    // Eden/Yuzu places its software keyboard over the framebuffer, not in a separate
    // desktop window. The scrim and keyboard below are child windows of the game HWND,
    // so Alt-Tab/taskbar never sees another Scarlet PC window.
    state.scrim = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
        0, 0, width, height, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (state.scrim != nullptr) {
        SetLayeredWindowAttributes(state.scrim, 0, 82, LWA_ALPHA);
    }

    state.hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_CONTROLPARENT, kKeyboardClass, L"",
        WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, width, height, owner,
        nullptr, GetModuleHandleW(nullptr), &state);

    if (state.hwnd == nullptr) {
        if (state.scrim != nullptr) DestroyWindow(state.scrim);
        DeleteObject(state.transparent_brush);
        DeleteObject(state.panel_brush);
        DeleteObject(state.edit_brush);
        LOG_ERROR(Service_AM, "[Scarlet.Keyboard] Failed to create in-game keyboard overlay");
        return {};
    }

    SetLayeredWindowAttributes(state.hwnd, kTransparentKey, 255, LWA_COLORKEY);
    BuildKeyboardUi(state);
    SetTimer(state.hwnd, 1, 16, nullptr);
    g_keyboard_window.store(state.hwnd, std::memory_order_release);

    state.owner_thread = GetWindowThreadProcessId(owner, nullptr);
    const DWORD this_thread = GetCurrentThreadId();
    if (state.owner_thread != 0 && state.owner_thread != this_thread) {
        state.attached_input = AttachThreadInput(this_thread, state.owner_thread, TRUE) != FALSE;
    }

    SetForegroundWindow(owner);
    BringWindowToTop(state.scrim);
    BringWindowToTop(state.hwnd);
    ShowWindow(state.scrim, SW_SHOW);
    ShowWindow(state.hwnd, SW_SHOW);
    UpdateWindow(state.hwnd);
    if (!state.nav_rows.empty()) FocusControl(state.nav_rows.front().front());

    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool handled = false;
        if (message.message == WM_KEYDOWN) {
            switch (message.wParam) {
            case VK_LEFT:
                if (GetFocus() != state.edit) {
                    Navigate(state, -1, 0);
                    handled = true;
                }
                break;
            case VK_RIGHT:
                if (GetFocus() != state.edit) {
                    Navigate(state, 1, 0);
                    handled = true;
                }
                break;
            case VK_UP:
                if (GetFocus() != state.edit) {
                    Navigate(state, 0, -1);
                    handled = true;
                }
                break;
            case VK_DOWN:
                if (GetFocus() != state.edit) {
                    Navigate(state, 0, 1);
                    handled = true;
                }
                break;
            case VK_RETURN:
                if (GetFocus() != state.edit) {
                    ActivateFocused(state);
                    handled = true;
                } else if (state.params.enable_return_button) {
                    ReplaceSelection(state.edit, L"\n");
                    handled = true;
                } else {
                    ValidateAndAccept(state);
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

    if (state.attached_input) {
        AttachThreadInput(GetCurrentThreadId(), state.owner_thread, FALSE);
    }
    if (state.scrim != nullptr && IsWindow(state.scrim)) DestroyWindow(state.scrim);

    DeleteObject(state.transparent_brush);
    DeleteObject(state.panel_brush);
    DeleteObject(state.edit_brush);
    if (state.font_header) DeleteObject(state.font_header);
    if (state.font_sub) DeleteObject(state.font_sub);
    if (state.font_editor) DeleteObject(state.font_editor);
    if (state.font_key) DeleteObject(state.font_key);
    if (state.font_label) DeleteObject(state.font_label);

    SetForegroundWindow(owner);
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
