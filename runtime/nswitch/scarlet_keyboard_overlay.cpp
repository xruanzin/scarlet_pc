#include "video_core/renderer_vulkan/scarlet_keyboard_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging/log.h"

#ifdef _WIN32
#include <windows.h>
#include <xinput.h>
#endif

namespace ScarletPC::KeyboardOverlay {
namespace {

enum class KeyAction {
    Character,
    Backspace,
    Shift,
    Space,
    Return,
    Cancel,
    Accept,
};

struct KeySpec {
    const wchar_t* label;
    wchar_t lower;
    wchar_t upper;
    KeyAction action;
    int x;
    int y;
    int w;
    int h;
};

constexpr int BaseWidth = 1280;
constexpr int BaseHeight = 720;

const std::vector<KeySpec>& NormalKeys() {
    static const std::vector<KeySpec> keys = [] {
        std::vector<KeySpec> out;
        constexpr int row_y[] = {335, 398, 461, 524, 587};
        constexpr int key_h = 63;
        constexpr int margin_x = 54;
        constexpr int key_w = 96;
        constexpr int special_w = 116;
        const auto col_x = [](int col) { return margin_x + (col - 1) * key_w; };
        struct Pair { wchar_t lower; wchar_t upper; };
        static constexpr std::array<Pair, 11> row1{{{L'1',L'#'},{L'2',L'['},{L'3',L']'},{L'4',L'
    std::mutex mutex;
    Config config{};
    std::u16string text;
    std::u16string error;
    bool visible{};
    bool done{};
    bool accepted{};
    bool silent{};
    bool shift{};
    std::size_t selected{};
    std::uint64_t version{1};
#ifdef _WIN32
    std::array<bool, 256> key_down{};
    WORD gamepad_buttons{};
#endif
};

State g_state;

std::wstring ToWide(std::u16string_view value) {
    std::wstring out;
    out.reserve(value.size());
    for (const char16_t ch : value) {
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

bool CharacterDisabled(const Config& config, wchar_t ch) {
    const auto flags = config.disable_flags;
    if (ch == L' ' && (flags & (1u << 1))) return true;
    if (ch == L'@' && (flags & (1u << 2))) return true;
    if (ch == L'%' && (flags & (1u << 3))) return true;
    if (ch == L'/' && (flags & (1u << 4))) return true;
    if (ch == L'\\' && (flags & (1u << 5))) return true;
    if (std::iswdigit(ch) && (flags & (1u << 6))) return true;
    return false;
}

bool KeyEnabled(const Config& config, const KeySpec& key) {
    switch (key.action) {
    case KeyAction::Character: return !CharacterDisabled(config,key.lower) || !CharacterDisabled(config,key.upper);
    case KeyAction::Space: return !CharacterDisabled(config,L' ');
    case KeyAction::Return: return config.enable_return;
    case KeyAction::Cancel: return !config.disable_cancel;
    default: return true;
    }
}

void Touch(State& state) {
    ++state.version;
    if (state.version == 0) {
        state.version = 1;
    }
}

void AppendCharacter(State& state, wchar_t ch) {
    if (CharacterDisabled(state.config, ch)) {
        return;
    }
    if (state.config.number_pad && !std::iswdigit(ch) && ch != L'-' && ch != L'.') {
        return;
    }
    const auto max_length = state.config.max_length ? state.config.max_length : 500u;
    if (state.text.size() >= max_length) {
        return;
    }
    state.text.push_back(static_cast<char16_t>(ch));
    state.error.clear();
    Touch(state);
}

void Backspace(State& state) {
    if (!state.text.empty()) {
        state.text.pop_back();
        state.error.clear();
        Touch(state);
    }
}

void Accept(State& state) {
    const auto count = static_cast<std::uint32_t>(state.text.size());
    const auto max_length = state.config.max_length ? state.config.max_length : 500u;
    if (count < state.config.min_length || count > max_length) {
        if (state.config.min_length == max_length) {
            state.error = u"Digite exatamente a quantidade solicitada de caracteres.";
        } else if (state.config.min_length > 0) {
            state.error = u"Digite mais caracteres antes de confirmar.";
        } else {
            state.error = u"Texto longo demais.";
        }
        Touch(state);
        return;
    }
    state.accepted = true;
    state.done = true;
    state.visible = false;
    Touch(state);
}

void Cancel(State& state) {
    if (state.config.disable_cancel) {
        return;
    }
    state.accepted = false;
    state.done = true;
    state.visible = false;
    Touch(state);
}

void Activate(State& state, const KeySpec& key) {
    if (!KeyEnabled(state.config, key)) return;
    switch (key.action) {
    case KeyAction::Character:
        AppendCharacter(state, state.shift ? key.upper : key.lower);
        break;
    case KeyAction::Backspace:
        Backspace(state);
        break;
    case KeyAction::Shift:
        state.shift = !state.shift;
        Touch(state);
        break;
    case KeyAction::Space:
        AppendCharacter(state, L' ');
        break;
    case KeyAction::Return:
        if (state.text.size() < (state.config.max_length ? state.config.max_length : 500u)) {
            state.text.push_back(u'\n');
            state.error.clear();
            Touch(state);
        }
        break;
    case KeyAction::Cancel:
        Cancel(state);
        break;
    case KeyAction::Accept:
        Accept(state);
        break;
    }
}

void Navigate(State& state, int dx, int dy) {
    const auto& keys = Keys(state.config);
    if (keys.empty() || state.selected >= keys.size()) {
        state.selected = 0;
        Touch(state);
        return;
    }
    const auto& current = keys[state.selected];
    const float cx = current.x + current.w * 0.5f;
    const float cy = current.y + current.h * 0.5f;

    std::size_t best = state.selected;
    float best_score = 1.0e30f;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i == state.selected) continue;
        const auto& candidate = keys[i];
        if (!KeyEnabled(state.config, candidate)) continue;
        const float tx = candidate.x + candidate.w * 0.5f;
        const float ty = candidate.y + candidate.h * 0.5f;
        const float vx = tx - cx;
        const float vy = ty - cy;
        if (dx < 0 && vx >= -1.0f) continue;
        if (dx > 0 && vx <= 1.0f) continue;
        if (dy < 0 && vy >= -1.0f) continue;
        if (dy > 0 && vy <= 1.0f) continue;

        const float primary = dx != 0 ? std::abs(vx) : std::abs(vy);
        const float secondary = dx != 0 ? std::abs(vy) : std::abs(vx);
        const float score = primary + secondary * 2.5f;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best != state.selected) {
        state.selected = best;
        Touch(state);
    }
}

#ifdef _WIN32

bool Pressed(State& state, int vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !state.key_down[static_cast<std::size_t>(vk)];
    state.key_down[static_cast<std::size_t>(vk)] = down;
    return pressed;
}

wchar_t PhysicalCharacter(State& state) {
    const bool upper = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (int vk = 'A'; vk <= 'Z'; ++vk) {
        if (Pressed(state, vk)) {
            wchar_t ch = static_cast<wchar_t>(upper ? vk : (vk + (L'a' - L'A')));
            return ch;
        }
    }
    for (int vk = '0'; vk <= '9'; ++vk) {
        if (Pressed(state, vk)) {
            return static_cast<wchar_t>(vk);
        }
    }
    struct Mapping { int vk; wchar_t normal; wchar_t shifted; };
    static constexpr Mapping map[] = {
        {VK_OEM_MINUS, L'-', L'_'}, {VK_OEM_COMMA, L',', L'<'},
        {VK_OEM_PERIOD, L'.', L'>'}, {VK_OEM_2, L'/', L'?'},
        {VK_OEM_1, L';', L':'}, {VK_OEM_7, L'\'', L'"'},
    };
    for (const auto& entry : map) {
        if (Pressed(state, entry.vk)) {
            return upper ? entry.shifted : entry.normal;
        }
    }
    return 0;
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn GetXInputGetState() {
    static XInputGetStateFn fn = [] {
        HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");
        if (!module) module = LoadLibraryW(L"xinput1_3.dll");
        if (!module) return static_cast<XInputGetStateFn>(nullptr);
        return reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
    }();
    return fn;
}

void PollGamepad(State& state) {
    const auto fn = GetXInputGetState();
    if (!fn) return;
    XINPUT_STATE input{};
    if (fn(0, &input) != ERROR_SUCCESS) {
        state.gamepad_buttons = 0;
        return;
    }
    const WORD buttons = input.Gamepad.wButtons;
    const WORD pressed = buttons & ~state.gamepad_buttons;
    state.gamepad_buttons = buttons;

    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) Navigate(state, -1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) Navigate(state, 1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) Navigate(state, 0, -1);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) Navigate(state, 0, 1);
    if (pressed & XINPUT_GAMEPAD_A) Activate(state, Keys(state.config)[state.selected]);
    if (pressed & XINPUT_GAMEPAD_B) Cancel(state);
    if (pressed & XINPUT_GAMEPAD_X) Backspace(state);
    if (pressed & XINPUT_GAMEPAD_Y) {
        state.shift = !state.shift;
        Touch(state);
    }
}

void DrawCentered(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color) {
    const auto old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

#endif

} // namespace

void Show(Config config, std::u16string initial_text, std::u16string error_text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.config = std::move(config);
    g_state.text = std::move(initial_text);
    g_state.error = std::move(error_text);
    g_state.visible = true;
    g_state.done = false;
    g_state.accepted = false;
    g_state.silent = false;
    g_state.shift = false;
    g_state.selected = 0;
#ifdef _WIN32
    g_state.key_down.fill(false);
    g_state.gamepad_buttons = 0;
#endif
    Touch(g_state);
    LOG_INFO(Service_AM, "[Scarlet.Keyboard] Native Vulkan overlay shown");
}

void Hide(bool silent) {
    std::scoped_lock lock{g_state.mutex};
    g_state.silent = silent;
    g_state.accepted = false;
    g_state.done = true;
    g_state.visible = false;
    Touch(g_state);
}

void UpdateText(std::u16string text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.text = std::move(text);
    Touch(g_state);
}

void SetError(std::u16string error_text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.error = std::move(error_text);
    Touch(g_state);
}

void TickInput() {
#ifdef _WIN32
    std::scoped_lock lock{g_state.mutex};
    if (!g_state.visible || g_state.done) return;

    const wchar_t typed = PhysicalCharacter(g_state);
    if (typed) AppendCharacter(g_state, typed);

    if (Pressed(g_state, VK_LEFT)) Navigate(g_state, -1, 0);
    if (Pressed(g_state, VK_RIGHT)) Navigate(g_state, 1, 0);
    if (Pressed(g_state, VK_UP)) Navigate(g_state, 0, -1);
    if (Pressed(g_state, VK_DOWN)) Navigate(g_state, 0, 1);
    if (Pressed(g_state, VK_BACK)) Backspace(g_state);
    if (Pressed(g_state, VK_SPACE)) AppendCharacter(g_state, L' ');
    if (Pressed(g_state, VK_ESCAPE)) Cancel(g_state);
    if (Pressed(g_state, VK_RETURN)) Activate(g_state, Keys(g_state.config)[g_state.selected]);

    PollGamepad(g_state);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
}

bool PollResult(Result& result) {
    std::scoped_lock lock{g_state.mutex};
    if (!g_state.done) return false;
    result.accepted = g_state.accepted;
    result.silent = g_state.silent;
    result.text = g_state.text;
    g_state.done = false;
    return true;
}

bool Visible() {
    std::scoped_lock lock{g_state.mutex};
    return g_state.visible;
}

std::uint64_t Version() {
    std::scoped_lock lock{g_state.mutex};
    return g_state.version;
}

bool BuildBgraFrame(std::uint32_t width, std::uint32_t height, std::uint64_t known_version,
                    std::uint64_t& out_version, std::vector<std::uint8_t>& out_pixels) {
#ifdef _WIN32
    Config config;
    std::u16string text;
    std::u16string error;
    bool shift{};
    std::size_t selected{};
    std::uint64_t version{};
    {
        std::scoped_lock lock{g_state.mutex};
        if (!g_state.visible) return false;
        version = g_state.version;
        if (version == known_version) return false;
        config = g_state.config;
        text = g_state.text;
        error = g_state.error;
        shift = g_state.shift;
        selected = g_state.selected;
    }

    if (width == 0 || height == 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old_bitmap = SelectObject(dc, bitmap);

    const float sx = static_cast<float>(width) / BaseWidth;
    const float sy = static_cast<float>(height) / BaseHeight;
    const auto X = [&](int v) { return static_cast<LONG>(std::lround(v * sx)); };
    const auto Y = [&](int v) { return static_cast<LONG>(std::lround(v * sy)); };

    RECT full{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    HBRUSH white = CreateSolidBrush(RGB(248, 248, 248));
    FillRect(dc, &full, white);

    HFONT title_font = CreateFontW(-Y(30), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT body_font = CreateFontW(-Y(20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT key_font = CreateFontW(-Y(25), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    std::wstring title = config.header.empty() ? L"Enter Text" : ToWide(config.header);
    RECT title_rect{X(80), Y(35), X(1200), Y(90)};
    DrawCentered(dc, title, title_rect, title_font, RGB(20, 20, 20));

    if (!config.sub.empty()) {
        RECT r{X(100), Y(82), X(1180), Y(112)};
        DrawCentered(dc, ToWide(config.sub), r, body_font, RGB(70, 70, 70));
    }
    if (!config.guide.empty()) {
        RECT r{X(100), Y(108), X(1180), Y(138)};
        DrawCentered(dc, ToWide(config.guide), r, body_font, RGB(90, 90, 90));
    }

    RECT edit{X(155), Y(150), X(1125), Y(205)};
    HBRUSH edit_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &edit, edit_brush);
    FrameRect(dc, &edit, GetSysColorBrush(COLOR_WINDOWFRAME));

    std::wstring shown = ToWide(text);
    if (config.password) shown.assign(shown.size(), L'•');
    RECT text_rect{edit.left + X(16), edit.top, edit.right - X(16), edit.bottom};
    const auto old_body = SelectObject(dc, body_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    DrawTextW(dc, shown.c_str(), static_cast<int>(shown.size()), &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_body);

    wchar_t counter[64]{};
    swprintf_s(counter, L"%zu/%u", text.size(), config.max_length ? config.max_length : 500);
    RECT counter_rect{X(940), Y(207), X(1125), Y(240)};
    DrawCentered(dc, counter, counter_rect, body_font, RGB(60, 60, 60));

    if (!error.empty()) {
        RECT error_rect{X(130), Y(224), X(1150), Y(258)};
        DrawCentered(dc, ToWide(error), error_rect, body_font, RGB(180, 35, 35));
    }

    const auto& keys = Keys(state.config);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        if (key.action == KeyAction::Cancel && config.disable_cancel) continue;
        RECT rect{X(key.x), Y(key.y), X(key.x + key.w), Y(key.y + key.h)};
        const bool is_selected = i == selected;
        HBRUSH brush = CreateSolidBrush(is_selected ? RGB(55, 86, 232) : RGB(255, 255, 255));
        FillRect(dc, &rect, brush);
        FrameRect(dc, &rect, GetSysColorBrush(COLOR_3DSHADOW));
        DeleteObject(brush);

        std::wstring label;
        if (key.action == KeyAction::Character) {
            wchar_t temp[2]{shift ? key.upper : key.lower, 0};
            label = temp;
        } else if (key.action == KeyAction::Accept) {
            label = config.ok_text.empty() ? L"OK" : ToWide(config.ok_text);
        } else {
            label = key.label ? key.label : L"";
        }
        DrawCentered(dc, label, rect, key_font,
                     is_selected ? RGB(255, 255, 255) : RGB(25, 25, 25));
    }

    RECT legend{X(100), Y(625), X(1180), Y(680)};
    DrawCentered(dc, L"D-Pad/Arrows: Move    A/Enter: Select    B/Esc: Cancel    X/Backspace: Delete",
                 legend, body_font, RGB(55, 55, 55));

    out_pixels.resize(static_cast<std::size_t>(width) * height * 4);
    std::memcpy(out_pixels.data(), bits, out_pixels.size());

    SelectObject(dc, old_bitmap);
    DeleteObject(title_font);
    DeleteObject(body_font);
    DeleteObject(key_font);
    DeleteObject(edit_brush);
    DeleteObject(white);
    DeleteObject(bitmap);
    DeleteDC(dc);

    out_version = version;
    return true;
#else
    (void)width;
    (void)height;
    (void)known_version;
    (void)out_version;
    (void)out_pixels;
    return false;
#endif
}

} // namespace ScarletPC::KeyboardOverlay
},{L'5',L'%'},{L'6',L'^'},{L'7',L'&'},{L'8',L'*'},{L'9',L'('},{L'0',L')'},{L'-',L'_'}}};
        static constexpr std::array<Pair, 11> row2{{{L'q',L'Q'},{L'w',L'W'},{L'e',L'E'},{L'r',L'R'},{L't',L'T'},{L'y',L'Y'},{L'u',L'U'},{L'i',L'I'},{L'o',L'O'},{L'p',L'P'},{L'/',L'@'}}};
        static constexpr std::array<Pair, 11> row3{{{L'a',L'A'},{L's',L'S'},{L'd',L'D'},{L'f',L'F'},{L'g',L'G'},{L'h',L'H'},{L'j',L'J'},{L'k',L'K'},{L'l',L'L'},{L':',L';'},{L'\'',L'"'}}};
        static constexpr std::array<Pair, 11> row4{{{L'z',L'Z'},{L'x',L'X'},{L'c',L'C'},{L'v',L'V'},{L'b',L'B'},{L'n',L'N'},{L'm',L'M'},{L',',L'<'},{L'.',L'>'},{L'?',L'+'},{L'!',L'='}}};
        const std::array<const Pair*,4> rows{row1.data(),row2.data(),row3.data(),row4.data()};
        for(int r=0;r<4;++r) for(int c=0;c<11;++c) out.push_back(KeySpec{nullptr,rows[r][c].lower,rows[r][c].upper,KeyAction::Character,col_x(c+1),row_y[r],key_w,key_h});
        out.push_back(KeySpec{L"⌫",0,0,KeyAction::Backspace,col_x(12),row_y[0],special_w,key_h});
        out.push_back(KeySpec{L"Return",0,0,KeyAction::Return,col_x(12),row_y[1],special_w,key_h*2});
        out.push_back(KeySpec{L"OK",0,0,KeyAction::Accept,col_x(12),row_y[3],special_w,key_h*2});
        out.push_back(KeySpec{L"⇧",0,0,KeyAction::Shift,col_x(1),row_y[4],key_w*2,key_h});
        out.push_back(KeySpec{L"Space",0,0,KeyAction::Space,col_x(3),row_y[4],key_w*9,key_h});
        return out;
    }();
    return keys;
}

const std::vector<KeySpec>& NumberPadKeys() {
    static const std::vector<KeySpec> keys = [] {
        std::vector<KeySpec> out;
        constexpr int key_w=180,key_h=76,gap=4,start_x=(BaseWidth-(key_w*4+gap*3))/2,start_y=348;
        static constexpr wchar_t digits[4][3]={{L'1',L'2',L'3'},{L'4',L'5',L'6'},{L'7',L'8',L'9'},{L'\0',L'0',L'\0'}};
        for(int r=0;r<4;++r) for(int c=0;c<3;++c){const wchar_t ch=digits[r][c]; if(ch!=L'\0') out.push_back(KeySpec{nullptr,ch,ch,KeyAction::Character,start_x+c*(key_w+gap),start_y+r*(key_h+gap),key_w,key_h});}
        out.push_back(KeySpec{L"⌫",0,0,KeyAction::Backspace,start_x+3*(key_w+gap),start_y,key_w,key_h});
        out.push_back(KeySpec{L"OK",0,0,KeyAction::Accept,start_x+3*(key_w+gap),start_y+key_h+gap,key_w,key_h*3+gap*2});
        return out;
    }();
    return keys;
}
const std::vector<KeySpec>& Keys(const Config& config) { return config.number_pad ? NumberPadKeys() : NormalKeys(); }

struct State {
    std::mutex mutex;
    Config config{};
    std::u16string text;
    std::u16string error;
    bool visible{};
    bool done{};
    bool accepted{};
    bool silent{};
    bool shift{};
    std::size_t selected{};
    std::uint64_t version{1};
#ifdef _WIN32
    std::array<bool, 256> key_down{};
    WORD gamepad_buttons{};
#endif
};

State g_state;

std::wstring ToWide(std::u16string_view value) {
    std::wstring out;
    out.reserve(value.size());
    for (const char16_t ch : value) {
        out.push_back(static_cast<wchar_t>(ch));
    }
    return out;
}

bool CharacterDisabled(const Config& config, wchar_t ch) {
    const auto flags = config.disable_flags;
    if (ch == L' ' && (flags & (1u << 1))) return true;
    if (ch == L'@' && (flags & (1u << 2))) return true;
    if (ch == L'%' && (flags & (1u << 3))) return true;
    if (ch == L'/' && (flags & (1u << 4))) return true;
    if (ch == L'\\' && (flags & (1u << 5))) return true;
    if (std::iswdigit(ch) && (flags & (1u << 6))) return true;
    return false;
}

void Touch(State& state) {
    ++state.version;
    if (state.version == 0) {
        state.version = 1;
    }
}

void AppendCharacter(State& state, wchar_t ch) {
    if (CharacterDisabled(state.config, ch)) {
        return;
    }
    if (state.config.number_pad && !std::iswdigit(ch) && ch != L'-' && ch != L'.') {
        return;
    }
    const auto max_length = state.config.max_length ? state.config.max_length : 500u;
    if (state.text.size() >= max_length) {
        return;
    }
    state.text.push_back(static_cast<char16_t>(ch));
    state.error.clear();
    Touch(state);
}

void Backspace(State& state) {
    if (!state.text.empty()) {
        state.text.pop_back();
        state.error.clear();
        Touch(state);
    }
}

void Accept(State& state) {
    const auto count = static_cast<std::uint32_t>(state.text.size());
    const auto max_length = state.config.max_length ? state.config.max_length : 500u;
    if (count < state.config.min_length || count > max_length) {
        if (state.config.min_length == max_length) {
            state.error = u"Digite exatamente a quantidade solicitada de caracteres.";
        } else if (state.config.min_length > 0) {
            state.error = u"Digite mais caracteres antes de confirmar.";
        } else {
            state.error = u"Texto longo demais.";
        }
        Touch(state);
        return;
    }
    state.accepted = true;
    state.done = true;
    state.visible = false;
    Touch(state);
}

void Cancel(State& state) {
    if (state.config.disable_cancel) {
        return;
    }
    state.accepted = false;
    state.done = true;
    state.visible = false;
    Touch(state);
}

void Activate(State& state, const KeySpec& key) {
    switch (key.action) {
    case KeyAction::Character:
        AppendCharacter(state, state.shift ? key.upper : key.lower);
        break;
    case KeyAction::Backspace:
        Backspace(state);
        break;
    case KeyAction::Shift:
        state.shift = !state.shift;
        Touch(state);
        break;
    case KeyAction::Space:
        AppendCharacter(state, L' ');
        break;
    case KeyAction::Cancel:
        Cancel(state);
        break;
    case KeyAction::Accept:
        Accept(state);
        break;
    }
}

void Navigate(State& state, int dx, int dy) {
    const auto& keys = Keys(state.config);
    if (keys.empty() || state.selected >= keys.size()) {
        state.selected = 0;
        Touch(state);
        return;
    }
    const auto& current = keys[state.selected];
    const float cx = current.x + current.w * 0.5f;
    const float cy = current.y + current.h * 0.5f;

    std::size_t best = state.selected;
    float best_score = 1.0e30f;
    for (std::size_t i = 0; i < keys.size(); ++i) {
        if (i == state.selected) continue;
        const auto& candidate = keys[i];
        if (candidate.action == KeyAction::Cancel && state.config.disable_cancel) continue;
        const float tx = candidate.x + candidate.w * 0.5f;
        const float ty = candidate.y + candidate.h * 0.5f;
        const float vx = tx - cx;
        const float vy = ty - cy;
        if (dx < 0 && vx >= -1.0f) continue;
        if (dx > 0 && vx <= 1.0f) continue;
        if (dy < 0 && vy >= -1.0f) continue;
        if (dy > 0 && vy <= 1.0f) continue;

        const float primary = dx != 0 ? std::abs(vx) : std::abs(vy);
        const float secondary = dx != 0 ? std::abs(vy) : std::abs(vx);
        const float score = primary + secondary * 2.5f;
        if (score < best_score) {
            best_score = score;
            best = i;
        }
    }
    if (best != state.selected) {
        state.selected = best;
        Touch(state);
    }
}

#ifdef _WIN32

bool Pressed(State& state, int vk) {
    const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    const bool pressed = down && !state.key_down[static_cast<std::size_t>(vk)];
    state.key_down[static_cast<std::size_t>(vk)] = down;
    return pressed;
}

wchar_t PhysicalCharacter(State& state) {
    const bool upper = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    for (int vk = 'A'; vk <= 'Z'; ++vk) {
        if (Pressed(state, vk)) {
            wchar_t ch = static_cast<wchar_t>(upper ? vk : (vk + (L'a' - L'A')));
            return ch;
        }
    }
    for (int vk = '0'; vk <= '9'; ++vk) {
        if (Pressed(state, vk)) {
            return static_cast<wchar_t>(vk);
        }
    }
    struct Mapping { int vk; wchar_t normal; wchar_t shifted; };
    static constexpr Mapping map[] = {
        {VK_OEM_MINUS, L'-', L'_'}, {VK_OEM_COMMA, L',', L'<'},
        {VK_OEM_PERIOD, L'.', L'>'}, {VK_OEM_2, L'/', L'?'},
        {VK_OEM_1, L';', L':'}, {VK_OEM_7, L'\'', L'"'},
    };
    for (const auto& entry : map) {
        if (Pressed(state, entry.vk)) {
            return upper ? entry.shifted : entry.normal;
        }
    }
    return 0;
}

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);

XInputGetStateFn GetXInputGetState() {
    static XInputGetStateFn fn = [] {
        HMODULE module = LoadLibraryW(L"xinput1_4.dll");
        if (!module) module = LoadLibraryW(L"xinput9_1_0.dll");
        if (!module) module = LoadLibraryW(L"xinput1_3.dll");
        if (!module) return static_cast<XInputGetStateFn>(nullptr);
        return reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState"));
    }();
    return fn;
}

void PollGamepad(State& state) {
    const auto fn = GetXInputGetState();
    if (!fn) return;
    XINPUT_STATE input{};
    if (fn(0, &input) != ERROR_SUCCESS) {
        state.gamepad_buttons = 0;
        return;
    }
    const WORD buttons = input.Gamepad.wButtons;
    const WORD pressed = buttons & ~state.gamepad_buttons;
    state.gamepad_buttons = buttons;

    if (pressed & XINPUT_GAMEPAD_DPAD_LEFT) Navigate(state, -1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_RIGHT) Navigate(state, 1, 0);
    if (pressed & XINPUT_GAMEPAD_DPAD_UP) Navigate(state, 0, -1);
    if (pressed & XINPUT_GAMEPAD_DPAD_DOWN) Navigate(state, 0, 1);
    if (pressed & XINPUT_GAMEPAD_A) Activate(state, Keys(state.config)[state.selected]);
    if (pressed & XINPUT_GAMEPAD_B) Cancel(state);
    if (pressed & XINPUT_GAMEPAD_X) Backspace(state);
    if (pressed & XINPUT_GAMEPAD_Y) {
        state.shift = !state.shift;
        Touch(state);
    }
}

void DrawCentered(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color) {
    const auto old_font = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &rect,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_font);
}

#endif

} // namespace

void Show(Config config, std::u16string initial_text, std::u16string error_text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.config = std::move(config);
    g_state.text = std::move(initial_text);
    g_state.error = std::move(error_text);
    g_state.visible = true;
    g_state.done = false;
    g_state.accepted = false;
    g_state.silent = false;
    g_state.shift = false;
    g_state.selected = 0;
#ifdef _WIN32
    g_state.key_down.fill(false);
    g_state.gamepad_buttons = 0;
#endif
    Touch(g_state);
    LOG_INFO(Service_AM, "[Scarlet.Keyboard] Native Vulkan overlay shown");
}

void Hide(bool silent) {
    std::scoped_lock lock{g_state.mutex};
    g_state.silent = silent;
    g_state.accepted = false;
    g_state.done = true;
    g_state.visible = false;
    Touch(g_state);
}

void UpdateText(std::u16string text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.text = std::move(text);
    Touch(g_state);
}

void SetError(std::u16string error_text) {
    std::scoped_lock lock{g_state.mutex};
    g_state.error = std::move(error_text);
    Touch(g_state);
}

void TickInput() {
#ifdef _WIN32
    std::scoped_lock lock{g_state.mutex};
    if (!g_state.visible || g_state.done) return;

    const wchar_t typed = PhysicalCharacter(g_state);
    if (typed) AppendCharacter(g_state, typed);

    if (Pressed(g_state, VK_LEFT)) Navigate(g_state, -1, 0);
    if (Pressed(g_state, VK_RIGHT)) Navigate(g_state, 1, 0);
    if (Pressed(g_state, VK_UP)) Navigate(g_state, 0, -1);
    if (Pressed(g_state, VK_DOWN)) Navigate(g_state, 0, 1);
    if (Pressed(g_state, VK_BACK)) Backspace(g_state);
    if (Pressed(g_state, VK_SPACE)) AppendCharacter(g_state, L' ');
    if (Pressed(g_state, VK_ESCAPE)) Cancel(g_state);
    if (Pressed(g_state, VK_RETURN)) Activate(g_state, Keys(g_state.config)[g_state.selected]);

    PollGamepad(g_state);
#else
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
}

bool PollResult(Result& result) {
    std::scoped_lock lock{g_state.mutex};
    if (!g_state.done) return false;
    result.accepted = g_state.accepted;
    result.silent = g_state.silent;
    result.text = g_state.text;
    g_state.done = false;
    return true;
}

bool Visible() {
    std::scoped_lock lock{g_state.mutex};
    return g_state.visible;
}

std::uint64_t Version() {
    std::scoped_lock lock{g_state.mutex};
    return g_state.version;
}

bool BuildBgraFrame(std::uint32_t width, std::uint32_t height, std::uint64_t known_version,
                    std::uint64_t& out_version, std::vector<std::uint8_t>& out_pixels) {
#ifdef _WIN32
    Config config;
    std::u16string text;
    std::u16string error;
    bool shift{};
    std::size_t selected{};
    std::uint64_t version{};
    {
        std::scoped_lock lock{g_state.mutex};
        if (!g_state.visible) return false;
        version = g_state.version;
        if (version == known_version) return false;
        config = g_state.config;
        text = g_state.text;
        error = g_state.error;
        shift = g_state.shift;
        selected = g_state.selected;
    }

    if (width == 0 || height == 0) return false;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits{};
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
        return false;
    }
    const auto old_bitmap = SelectObject(dc, bitmap);

    const float sx = static_cast<float>(width) / BaseWidth;
    const float sy = static_cast<float>(height) / BaseHeight;
    const auto X = [&](int v) { return static_cast<LONG>(std::lround(v * sx)); };
    const auto Y = [&](int v) { return static_cast<LONG>(std::lround(v * sy)); };

    RECT full{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    HBRUSH white = CreateSolidBrush(RGB(248, 248, 248));
    FillRect(dc, &full, white);

    HFONT title_font = CreateFontW(-Y(30), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT body_font = CreateFontW(-Y(20), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                  DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT key_font = CreateFontW(-Y(25), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    std::wstring title = config.header.empty() ? L"Enter Text" : ToWide(config.header);
    RECT title_rect{X(80), Y(35), X(1200), Y(90)};
    DrawCentered(dc, title, title_rect, title_font, RGB(20, 20, 20));

    if (!config.sub.empty()) {
        RECT r{X(100), Y(82), X(1180), Y(112)};
        DrawCentered(dc, ToWide(config.sub), r, body_font, RGB(70, 70, 70));
    }
    if (!config.guide.empty()) {
        RECT r{X(100), Y(108), X(1180), Y(138)};
        DrawCentered(dc, ToWide(config.guide), r, body_font, RGB(90, 90, 90));
    }

    RECT edit{X(155), Y(150), X(1125), Y(205)};
    HBRUSH edit_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(dc, &edit, edit_brush);
    FrameRect(dc, &edit, GetSysColorBrush(COLOR_WINDOWFRAME));

    std::wstring shown = ToWide(text);
    if (config.password) shown.assign(shown.size(), L'•');
    RECT text_rect{edit.left + X(16), edit.top, edit.right - X(16), edit.bottom};
    const auto old_body = SelectObject(dc, body_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(20, 20, 20));
    DrawTextW(dc, shown.c_str(), static_cast<int>(shown.size()), &text_rect,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old_body);

    wchar_t counter[64]{};
    swprintf_s(counter, L"%zu/%u", text.size(), config.max_length ? config.max_length : 500);
    RECT counter_rect{X(940), Y(207), X(1125), Y(240)};
    DrawCentered(dc, counter, counter_rect, body_font, RGB(60, 60, 60));

    if (!error.empty()) {
        RECT error_rect{X(130), Y(224), X(1150), Y(258)};
        DrawCentered(dc, ToWide(error), error_rect, body_font, RGB(180, 35, 35));
    }

    const auto& keys = Keys(state.config);
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto& key = keys[i];
        if (key.action == KeyAction::Cancel && config.disable_cancel) continue;
        RECT rect{X(key.x), Y(key.y), X(key.x + key.w), Y(key.y + key.h)};
        const bool is_selected = i == selected;
        HBRUSH brush = CreateSolidBrush(is_selected ? RGB(55, 86, 232) : RGB(255, 255, 255));
        FillRect(dc, &rect, brush);
        FrameRect(dc, &rect, GetSysColorBrush(COLOR_3DSHADOW));
        DeleteObject(brush);

        std::wstring label;
        if (key.action == KeyAction::Character) {
            wchar_t temp[2]{shift ? key.upper : key.lower, 0};
            label = temp;
        } else if (key.action == KeyAction::Accept) {
            label = config.ok_text.empty() ? L"OK" : ToWide(config.ok_text);
        } else {
            label = key.label ? key.label : L"";
        }
        DrawCentered(dc, label, rect, key_font,
                     is_selected ? RGB(255, 255, 255) : RGB(25, 25, 25));
    }

    RECT legend{X(100), Y(625), X(1180), Y(680)};
    DrawCentered(dc, L"D-Pad/Arrows: Move    A/Enter: Select    B/Esc: Cancel    X/Backspace: Delete",
                 legend, body_font, RGB(55, 55, 55));

    out_pixels.resize(static_cast<std::size_t>(width) * height * 4);
    std::memcpy(out_pixels.data(), bits, out_pixels.size());

    SelectObject(dc, old_bitmap);
    DeleteObject(title_font);
    DeleteObject(body_font);
    DeleteObject(key_font);
    DeleteObject(edit_brush);
    DeleteObject(white);
    DeleteObject(bitmap);
    DeleteDC(dc);

    out_version = version;
    return true;
#else
    (void)width;
    (void)height;
    (void)known_version;
    (void)out_version;
    (void)out_pixels;
    return false;
#endif
}

} // namespace ScarletPC::KeyboardOverlay
