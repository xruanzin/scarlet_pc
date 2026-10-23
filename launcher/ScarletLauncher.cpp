#define UNICODE
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "uxtheme.lib")

using namespace Gdiplus;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kWindowClass[] = L"ScarletPCLauncherWindow";
constexpr wchar_t kTitle[] = L"Scarlet PC";
constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 760;

enum ControlId : int {
    ID_TAB_GAME = 100,
    ID_TAB_GRAPHICS,
    ID_TAB_CONTROLS,
    ID_TAB_GENERAL,

    ID_GAME_EDIT = 200,
    ID_GAME_BROWSE,
    ID_UPDATE_EDIT,
    ID_UPDATE_BROWSE,
    ID_KEYS_EDIT,
    ID_KEYS_BROWSE,
    ID_DLC_LIST,
    ID_DLC_ADD,
    ID_DLC_REMOVE,
    ID_GAME_STATUS,

    ID_RESOLUTION = 300,
    ID_DISPLAY_MODE,
    ID_FPS,
    ID_UPSCALER,
    ID_UPSCALER_QUALITY,
    ID_FRAME_GENERATION,
    ID_ASYNC_SHADERS,

    ID_CONTROLLER_STYLE = 400,
    ID_AUTO_CONTROLLER,
    ID_CONTROLLER_HINT,

    ID_LANGUAGE = 500,
    ID_OPEN_USER,
    ID_OPEN_LOGS,
    ID_CLOSE_ON_LAUNCH,

    ID_PLAY = 900,
};

enum Page : int { Game = 0, Graphics = 1, Controls = 2, General = 3 };

struct ResolutionEntry {
    const wchar_t* label;
    int width;
    int height;
};

constexpr std::array<ResolutionEntry, 7> kResolutions{{
    {L"1366 x 768", 1366, 768},
    {L"1600 x 900", 1600, 900},
    {L"1920 x 1080", 1920, 1080},
    {L"1920 x 1200", 1920, 1200},
    {L"2560 x 1440", 2560, 1440},
    {L"2560 x 1600", 2560, 1600},
    {L"3840 x 2160", 3840, 2160},
}};

constexpr std::array<const wchar_t*, 3> kDisplayModes{{
    L"Janela", L"Borderless", L"Tela cheia"
}};

constexpr std::array<const wchar_t*, 5> kFpsModes{{
    L"30 FPS", L"60 FPS", L"120 FPS", L"144 FPS", L"Ilimitado"
}};

constexpr std::array<const wchar_t*, 6> kUpscalers{{
    L"Nativo", L"DLSS", L"FSR 1", L"FSR 3", L"FSR 3.1", L"FSR 4"
}};

constexpr std::array<const wchar_t*, 4> kUpscalerQualities{{
    L"Qualidade", L"Balanceado", L"Performance", L"Ultra Performance"
}};

constexpr std::array<const wchar_t*, 4> kControllerStyles{{
    L"Automático", L"Xbox", L"PlayStation", L"Nintendo"
}};

constexpr std::array<const wchar_t*, 2> kLanguages{{
    L"Português (Brasil)", L"English"
}};

HINSTANCE g_instance{};
ULONG_PTR g_gdiplus_token{};
std::unique_ptr<Image> g_background;
HBRUSH g_edit_brush{};
HBRUSH g_panel_brush{};
HFONT g_font{};
HFONT g_font_bold{};
HFONT g_font_large{};

HWND g_main{};
Page g_page = Game;
std::array<std::vector<HWND>, 4> g_page_controls;

HWND g_game_edit{};
HWND g_update_edit{};
HWND g_keys_edit{};
HWND g_dlc_list{};
HWND g_status{};

HWND g_resolution{};
HWND g_display_mode{};
HWND g_fps{};
HWND g_upscaler{};
HWND g_quality{};
HWND g_frame_generation{};
HWND g_async_shaders{};

HWND g_controller_style{};
HWND g_auto_controller{};

HWND g_language{};
HWND g_close_on_launch{};

std::vector<std::wstring> g_dlcs;
fs::path g_exe_dir;
fs::path g_ini_path;

std::wstring GetText(HWND hwnd) {
    const int len = GetWindowTextLengthW(hwnd);
    std::wstring text(static_cast<size_t>(len), L'\0');
    if (len > 0) {
        GetWindowTextW(hwnd, text.data(), len + 1);
    }
    return text;
}

void SetText(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(hwnd, text.c_str());
}

std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") {
    std::array<wchar_t, 32768> buffer{};
    GetPrivateProfileStringW(section, key, fallback, buffer.data(),
                             static_cast<DWORD>(buffer.size()), g_ini_path.c_str());
    return buffer.data();
}

int IniReadInt(const wchar_t* section, const wchar_t* key, int fallback) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, fallback, g_ini_path.c_str()));
}

void IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(section, key, value.c_str(), g_ini_path.c_str());
}

void IniWriteInt(const wchar_t* section, const wchar_t* key, int value) {
    IniWrite(section, key, std::to_wstring(value));
}

fs::path ExeDir() {
    std::array<wchar_t, 32768> path{};
    GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    return fs::path(path.data()).parent_path();
}

std::wstring QuoteArg(const std::wstring& arg) {
    if (arg.empty()) return L"\"\"";
    if (arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    unsigned slashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
            slashes = 0;
            continue;
        }
        out.append(slashes, L'\\');
        slashes = 0;
        out.push_back(ch);
    }
    out.append(slashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

bool FileExists(const std::wstring& path) {
    std::error_code ec;
    return !path.empty() && fs::is_regular_file(fs::path(path), ec);
}

bool HasExtension(const std::wstring& path, std::initializer_list<const wchar_t*> exts) {
    std::wstring ext = fs::path(path).extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
    for (const auto* allowed : exts) {
        if (ext == allowed) return true;
    }
    return false;
}

std::wstring OpenFile(HWND owner, const wchar_t* filter, const std::wstring& initial = {}) {
    std::array<wchar_t, 32768> file{};
    if (!initial.empty() && initial.size() < file.size()) {
        wcscpy_s(file.data(), file.size(), initial.c_str());
    }

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetOpenFileNameW(&ofn)) return {};
    return file.data();
}

void AddToPage(Page page, HWND hwnd) {
    g_page_controls[static_cast<size_t>(page)].push_back(hwnd);
}

HWND MakeStatic(Page page, int x, int y, int w, int h, const wchar_t* text, bool bold = false) {
    HWND hnd = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                               x, y, w, h, g_main, nullptr, g_instance, nullptr);
    SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_font_bold : g_font), TRUE);
    AddToPage(page, hnd);
    return hnd;
}

HWND MakeEdit(Page page, int id, int x, int y, int w, int h) {
    HWND hnd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                               WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                               x, y, w, h, g_main, reinterpret_cast<HMENU>(id), g_instance, nullptr);
    SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SetWindowTheme(hnd, L"DarkMode_Explorer", nullptr);
    AddToPage(page, hnd);
    return hnd;
}

HWND MakeButton(Page page, int id, int x, int y, int w, int h, const wchar_t* text) {
    HWND hnd = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                               x, y, w, h, g_main, reinterpret_cast<HMENU>(id), g_instance, nullptr);
    SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
    AddToPage(page, hnd);
    return hnd;
}

HWND MakeCombo(Page page, int id, int x, int y, int w, int h,
               const wchar_t* const* items, size_t count, int selection) {
    HWND hnd = CreateWindowExW(0, WC_COMBOBOXW, L"",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                               x, y, w, h, g_main, reinterpret_cast<HMENU>(id), g_instance, nullptr);
    SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SetWindowTheme(hnd, L"DarkMode_Explorer", nullptr);
    for (size_t i = 0; i < count; ++i) {
        SendMessageW(hnd, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i]));
    }
    SendMessageW(hnd, CB_SETCURSEL, std::clamp(selection, 0, static_cast<int>(count) - 1), 0);
    AddToPage(page, hnd);
    return hnd;
}

template <size_t N>
HWND MakeCombo(Page page, int id, int x, int y, int w, int h,
               const std::array<const wchar_t*, N>& items, int selection) {
    return MakeCombo(page, id, x, y, w, h, items.data(), items.size(), selection);
}

HWND MakeCheckbox(Page page, int id, int x, int y, int w, int h,
                  const wchar_t* text, bool checked) {
    HWND hnd = CreateWindowExW(0, L"BUTTON", text,
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                               x, y, w, h, g_main, reinterpret_cast<HMENU>(id), g_instance, nullptr);
    SendMessageW(hnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SendMessageW(hnd, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
    SetWindowTheme(hnd, L"DarkMode_Explorer", nullptr);
    AddToPage(page, hnd);
    return hnd;
}

void RefreshDlcList() {
    SendMessageW(g_dlc_list, LB_RESETCONTENT, 0, 0);
    for (const auto& dlc : g_dlcs) {
        SendMessageW(g_dlc_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(dlc.c_str()));
    }
}

void RefreshStatus() {
    const auto game = GetText(g_game_edit);
    const auto update = GetText(g_update_edit);
    const auto keys = GetText(g_keys_edit);

    const bool game_ok = FileExists(game) && HasExtension(game, {L".nsp", L".xci"});
    const bool update_ok = update.empty() || (FileExists(update) && HasExtension(update, {L".nsp", L".xci"}));
    const bool keys_ok = FileExists(keys);

    std::wstring status;
    status += game_ok ? L"✓ Jogo encontrado" : L"● Selecione o jogo (.NSP ou .XCI)";
    status += L"     ";
    if (!update.empty()) {
        const bool is_400 = update.find(L"4.0.0") != std::wstring::npos;
        status += update_ok ? (is_400 ? L"✓ Update 4.0.0" : L"✓ Update selecionada")
                            : L"● Update inválida";
    } else {
        status += L"○ Sem update";
    }
    status += L"     ";
    status += keys_ok ? L"✓ Keys encontradas" : L"● Selecione prod.keys";
    status += L"     ";
    status += L"✓ " + std::to_wstring(g_dlcs.size()) + (g_dlcs.size() == 1 ? L" DLC" : L" DLCs");
    SetText(g_status, status);
}

void ShowPage(Page page) {
    g_page = page;
    for (size_t i = 0; i < g_page_controls.size(); ++i) {
        for (HWND control : g_page_controls[i]) {
            ShowWindow(control, i == static_cast<size_t>(page) ? SW_SHOW : SW_HIDE);
        }
    }
    InvalidateRect(g_main, nullptr, TRUE);
}

void SetComboSelection(HWND combo, int index) {
    const int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    if (count > 0) SendMessageW(combo, CB_SETCURSEL, std::clamp(index, 0, count - 1), 0);
}

int ComboSelection(HWND combo) {
    const LRESULT value = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    return value == CB_ERR ? 0 : static_cast<int>(value);
}

bool Checked(HWND checkbox) {
    return SendMessageW(checkbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SaveSettings() {
    std::error_code ec;
    fs::create_directories(g_ini_path.parent_path(), ec);

    IniWrite(L"Game", L"game", GetText(g_game_edit));
    IniWrite(L"Game", L"update", GetText(g_update_edit));
    IniWrite(L"Game", L"keys", GetText(g_keys_edit));
    IniWriteInt(L"Game", L"dlc_count", static_cast<int>(g_dlcs.size()));
    for (size_t i = 0; i < g_dlcs.size(); ++i) {
        IniWrite(L"Game", (L"dlc_" + std::to_wstring(i)).c_str(), g_dlcs[i]);
    }
    // Clear stale DLC entries from older configurations.
    for (size_t i = g_dlcs.size(); i < 64; ++i) {
        const std::wstring key = L"dlc_" + std::to_wstring(i);
        if (IniRead(L"Game", key.c_str()).empty()) break;
        WritePrivateProfileStringW(L"Game", key.c_str(), nullptr, g_ini_path.c_str());
    }

    IniWriteInt(L"Graphics", L"resolution", ComboSelection(g_resolution));
    IniWriteInt(L"Graphics", L"display_mode", ComboSelection(g_display_mode));
    IniWriteInt(L"Graphics", L"fps", ComboSelection(g_fps));
    IniWriteInt(L"Graphics", L"upscaler", ComboSelection(g_upscaler));
    IniWriteInt(L"Graphics", L"quality", ComboSelection(g_quality));
    IniWriteInt(L"Graphics", L"frame_generation", Checked(g_frame_generation) ? 1 : 0);
    IniWriteInt(L"Graphics", L"async_shaders", Checked(g_async_shaders) ? 1 : 0);

    IniWriteInt(L"Controls", L"style", ComboSelection(g_controller_style));
    IniWriteInt(L"Controls", L"auto_assign", Checked(g_auto_controller) ? 1 : 0);

    IniWriteInt(L"General", L"language", ComboSelection(g_language));
    IniWriteInt(L"General", L"close_on_launch", Checked(g_close_on_launch) ? 1 : 0);
}

void LoadSettings() {
    SetText(g_game_edit, IniRead(L"Game", L"game"));
    SetText(g_update_edit, IniRead(L"Game", L"update"));
    SetText(g_keys_edit, IniRead(L"Game", L"keys"));

    const int dlc_count = std::clamp(IniReadInt(L"Game", L"dlc_count", 0), 0, 64);
    g_dlcs.clear();
    for (int i = 0; i < dlc_count; ++i) {
        const auto value = IniRead(L"Game", (L"dlc_" + std::to_wstring(i)).c_str());
        if (!value.empty()) g_dlcs.push_back(value);
    }
    RefreshDlcList();

    SetComboSelection(g_resolution, IniReadInt(L"Graphics", L"resolution", 2));
    SetComboSelection(g_display_mode, IniReadInt(L"Graphics", L"display_mode", 1));
    SetComboSelection(g_fps, IniReadInt(L"Graphics", L"fps", 1));
    SetComboSelection(g_upscaler, IniReadInt(L"Graphics", L"upscaler", 0));
    SetComboSelection(g_quality, IniReadInt(L"Graphics", L"quality", 0));
    SendMessageW(g_frame_generation, BM_SETCHECK,
                 IniReadInt(L"Graphics", L"frame_generation", 0) ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(g_async_shaders, BM_SETCHECK,
                 IniReadInt(L"Graphics", L"async_shaders", 1) ? BST_CHECKED : BST_UNCHECKED, 0);

    SetComboSelection(g_controller_style, IniReadInt(L"Controls", L"style", 0));
    SendMessageW(g_auto_controller, BM_SETCHECK,
                 IniReadInt(L"Controls", L"auto_assign", 1) ? BST_CHECKED : BST_UNCHECKED, 0);

    SetComboSelection(g_language, IniReadInt(L"General", L"language", 0));
    SendMessageW(g_close_on_launch, BM_SETCHECK,
                 IniReadInt(L"General", L"close_on_launch", 0) ? BST_CHECKED : BST_UNCHECKED, 0);

    RefreshStatus();
}

void OpenFolder(const fs::path& path) {
    std::error_code ec;
    fs::create_directories(path, ec);
    ShellExecuteW(g_main, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::wstring UpscalerEnv(int index) {
    switch (index) {
    case 1: return L"dlss";
    case 2: return L"fsr1";
    case 3: return L"fsr3";
    case 4: return L"fsr31";
    case 5: return L"fsr4";
    default: return L"native";
    }
}

std::wstring QualityEnv(int index) {
    switch (index) {
    case 1: return L"balanced";
    case 2: return L"performance";
    case 3: return L"ultra_performance";
    default: return L"quality";
    }
}

std::wstring DisplayModeEnv(int index) {
    switch (index) {
    case 1: return L"borderless";
    case 2: return L"fullscreen";
    default: return L"windowed";
    }
}

int TargetFpsValue(int index) {
    switch (index) {
    case 0: return 30;
    case 2: return 120;
    case 3: return 144;
    case 4: return 240; // practical "unlimited" ceiling used by the runtime
    default: return 60;
    }
}

void SetChildEnvironment(int width, int height) {
    SetEnvironmentVariableW(L"NSWITCH_AOT_FALLBACK", L"1");
    SetEnvironmentVariableW(L"SCARLET_TARGET_FPS",
                            std::to_wstring(TargetFpsValue(ComboSelection(g_fps))).c_str());
    SetEnvironmentVariableW(L"SCARLET_ASYNC_SHADERS",
                            Checked(g_async_shaders) ? L"1" : L"0");
    SetEnvironmentVariableW(L"SCARLET_WINDOW_MODE",
                            DisplayModeEnv(ComboSelection(g_display_mode)).c_str());
    SetEnvironmentVariableW(L"SCARLET_WINDOW_WIDTH", std::to_wstring(width).c_str());
    SetEnvironmentVariableW(L"SCARLET_WINDOW_HEIGHT", std::to_wstring(height).c_str());
    SetEnvironmentVariableW(L"SCARLET_UPSCALER",
                            UpscalerEnv(ComboSelection(g_upscaler)).c_str());
    SetEnvironmentVariableW(L"SCARLET_UPSCALER_QUALITY",
                            QualityEnv(ComboSelection(g_quality)).c_str());
    SetEnvironmentVariableW(L"SCARLET_FRAME_GENERATION",
                            Checked(g_frame_generation) ? L"1" : L"0");
    SetEnvironmentVariableW(L"SCARLET_CONTROLLER_STYLE",
                            std::to_wstring(ComboSelection(g_controller_style)).c_str());
    SetEnvironmentVariableW(L"SCARLET_AUTO_CONTROLLER",
                            Checked(g_auto_controller) ? L"1" : L"0");
}

bool PrepareKeys(const std::wstring& selected) {
    if (!FileExists(selected)) return false;
    const fs::path keys_dir = g_exe_dir / L"user" / L"keys";
    std::error_code ec;
    fs::create_directories(keys_dir, ec);
    const fs::path target = keys_dir / L"prod.keys";
    if (!CopyFileW(selected.c_str(), target.c_str(), FALSE)) {
        if (GetLastError() != ERROR_SHARING_VIOLATION && GetLastError() != ERROR_ACCESS_DENIED) {
            return false;
        }
        // If it is the same file already in use, it is still valid for launching.
        if (!fs::equivalent(fs::path(selected), target, ec)) return false;
    }
    return true;
}

void LaunchGame() {
    SaveSettings();

    const std::wstring game = GetText(g_game_edit);
    const std::wstring update = GetText(g_update_edit);
    const std::wstring keys = GetText(g_keys_edit);

    if (!FileExists(game) || !HasExtension(game, {L".nsp", L".xci"})) {
        MessageBoxW(g_main, L"Selecione um jogo válido em formato NSP ou XCI.",
                    kTitle, MB_OK | MB_ICONWARNING);
        ShowPage(Game);
        return;
    }
    if (!update.empty() && (!FileExists(update) || !HasExtension(update, {L".nsp", L".xci"}))) {
        MessageBoxW(g_main, L"O arquivo de update selecionado não é válido.",
                    kTitle, MB_OK | MB_ICONWARNING);
        ShowPage(Game);
        return;
    }
    if (!PrepareKeys(keys)) {
        MessageBoxW(g_main, L"Selecione um arquivo prod.keys válido.",
                    kTitle, MB_OK | MB_ICONWARNING);
        ShowPage(Game);
        return;
    }
    for (const auto& dlc : g_dlcs) {
        if (!FileExists(dlc) || !HasExtension(dlc, {L".nsp", L".xci"})) {
            MessageBoxW(g_main, (L"DLC não encontrada:\n" + dlc).c_str(),
                        kTitle, MB_OK | MB_ICONWARNING);
            ShowPage(Game);
            return;
        }
    }
    if (!g_dlcs.empty() && update.empty()) {
        MessageBoxW(g_main,
                    L"Para carregar DLCs externas neste build, selecione também a update do jogo.",
                    kTitle, MB_OK | MB_ICONWARNING);
        ShowPage(Game);
        return;
    }

    const fs::path runtime = g_exe_dir / L"ScarletPC.exe";
    if (!fs::is_regular_file(runtime)) {
        MessageBoxW(g_main, L"ScarletPC.exe não foi encontrado ao lado do launcher.",
                    kTitle, MB_OK | MB_ICONERROR);
        return;
    }

    const int res_index = std::clamp(ComboSelection(g_resolution), 0,
                                     static_cast<int>(kResolutions.size()) - 1);
    const auto& res = kResolutions[static_cast<size_t>(res_index)];
    SetChildEnvironment(res.width, res.height);

    std::wstring command = QuoteArg(runtime.wstring());
    if (!update.empty()) {
        command += L" --content-base " + QuoteArg(game);
        command += L" --content-update " + QuoteArg(update);
        for (const auto& dlc : g_dlcs) {
            command += L" --content-dlc " + QuoteArg(dlc);
        }
    } else {
        command += L" -g " + QuoteArg(game);
    }

    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(runtime.c_str(), mutable_command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, g_exe_dir.c_str(), &si, &pi)) {
        const DWORD error = GetLastError();
        MessageBoxW(g_main,
                    (L"Não foi possível iniciar Pokémon Scarlet.\nErro do Windows: " +
                     std::to_wstring(error)).c_str(),
                    kTitle, MB_OK | MB_ICONERROR);
        return;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (Checked(g_close_on_launch)) {
        PostMessageW(g_main, WM_CLOSE, 0, 0);
    } else {
        SetWindowTextW(g_main, L"Scarlet PC — jogo iniciado");
    }
}

void CreateGamePage() {
    MakeStatic(Game, 300, 102, 700, 34, L"JOGO E CONTEÚDO", true);

    MakeStatic(Game, 300, 152, 160, 24, L"Jogo (.NSP / .XCI)");
    g_game_edit = MakeEdit(Game, ID_GAME_EDIT, 300, 178, 650, 32);
    MakeButton(Game, ID_GAME_BROWSE, 964, 178, 150, 32, L"PROCURAR");

    MakeStatic(Game, 300, 226, 160, 24, L"Update");
    g_update_edit = MakeEdit(Game, ID_UPDATE_EDIT, 300, 252, 650, 32);
    MakeButton(Game, ID_UPDATE_BROWSE, 964, 252, 150, 32, L"PROCURAR");

    MakeStatic(Game, 300, 300, 160, 24, L"Prod.keys");
    g_keys_edit = MakeEdit(Game, ID_KEYS_EDIT, 300, 326, 650, 32);
    MakeButton(Game, ID_KEYS_BROWSE, 964, 326, 150, 32, L"PROCURAR");

    MakeStatic(Game, 300, 376, 280, 24, L"DLC / Conteúdo adicional");
    g_dlc_list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                                     LBS_NOINTEGRALHEIGHT | LBS_NOTIFY,
                                 300, 402, 650, 132, g_main,
                                 reinterpret_cast<HMENU>(ID_DLC_LIST), g_instance, nullptr);
    SendMessageW(g_dlc_list, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), TRUE);
    SetWindowTheme(g_dlc_list, L"DarkMode_Explorer", nullptr);
    AddToPage(Game, g_dlc_list);
    MakeButton(Game, ID_DLC_ADD, 964, 402, 150, 34, L"+ ADICIONAR");
    MakeButton(Game, ID_DLC_REMOVE, 964, 446, 150, 34, L"REMOVER");

    g_status = MakeStatic(Game, 300, 556, 820, 42, L"", false);
}

void CreateGraphicsPage() {
    MakeStatic(Graphics, 300, 102, 700, 34, L"GRÁFICOS", true);

    MakeStatic(Graphics, 300, 160, 260, 24, L"Resolução");
    std::array<const wchar_t*, kResolutions.size()> res_labels{};
    for (size_t i = 0; i < kResolutions.size(); ++i) res_labels[i] = kResolutions[i].label;
    g_resolution = MakeCombo(Graphics, ID_RESOLUTION, 650, 154, 360, 240,
                             res_labels.data(), res_labels.size(), 2);

    MakeStatic(Graphics, 300, 215, 260, 24, L"Modo de exibição");
    g_display_mode = MakeCombo(Graphics, ID_DISPLAY_MODE, 650, 209, 360, 160, kDisplayModes, 1);

    MakeStatic(Graphics, 300, 270, 260, 24, L"Limite de FPS");
    g_fps = MakeCombo(Graphics, ID_FPS, 650, 264, 360, 180, kFpsModes, 1);

    MakeStatic(Graphics, 300, 325, 260, 24, L"Upscaling");
    g_upscaler = MakeCombo(Graphics, ID_UPSCALER, 650, 319, 360, 220, kUpscalers, 0);

    MakeStatic(Graphics, 300, 380, 260, 24, L"Qualidade do upscaling");
    g_quality = MakeCombo(Graphics, ID_UPSCALER_QUALITY, 650, 374, 360, 180,
                          kUpscalerQualities, 0);

    g_frame_generation = MakeCheckbox(Graphics, ID_FRAME_GENERATION, 300, 444, 450, 30,
                                      L"Frame Generation", false);
    g_async_shaders = MakeCheckbox(Graphics, ID_ASYNC_SHADERS, 300, 488, 450, 30,
                                   L"Shader assíncrono", true);

    MakeStatic(Graphics, 300, 544, 810, 55,
               L"DLSS, FSR 3/3.1/4 e Frame Generation permanecem selecionáveis mesmo em GPUs não compatíveis. "
               L"O runtime usa o melhor caminho disponível e registra o fallback quando necessário.");
}

void CreateControlsPage() {
    MakeStatic(Controls, 300, 102, 700, 34, L"CONTROLES", true);

    MakeStatic(Controls, 300, 164, 300, 24, L"Estilo dos ícones do controle");
    g_controller_style = MakeCombo(Controls, ID_CONTROLLER_STYLE, 650, 158, 360, 180,
                                   kControllerStyles, 0);

    g_auto_controller = MakeCheckbox(Controls, ID_AUTO_CONTROLLER, 300, 224, 540, 30,
                                     L"Configurar automaticamente o primeiro controle conectado", true);

    MakeStatic(Controls, 300, 296, 780, 80,
               L"O Scarlet PC detecta controles automaticamente. Para remapeamento detalhado, "
               L"pressione F12 durante o jogo e use o painel de controles integrado.");

    MakeStatic(Controls, 300, 400, 780, 70,
               L"Perfis visuais: Automático, Xbox, PlayStation e Nintendo. "
               L"A preferência fica salva no launcher para o overlay de controles.");
}

void CreateGeneralPage() {
    MakeStatic(General, 300, 102, 700, 34, L"GERAL", true);

    MakeStatic(General, 300, 164, 260, 24, L"Idioma do launcher");
    g_language = MakeCombo(General, ID_LANGUAGE, 650, 158, 360, 150, kLanguages, 0);

    g_close_on_launch = MakeCheckbox(General, ID_CLOSE_ON_LAUNCH, 300, 224, 560, 30,
                                     L"Fechar o launcher depois de iniciar o jogo", false);

    MakeButton(General, ID_OPEN_USER, 300, 310, 330, 42, L"ABRIR PASTA DO USUÁRIO");
    MakeButton(General, ID_OPEN_LOGS, 650, 310, 330, 42, L"ABRIR LOGS");

    MakeStatic(General, 300, 392, 780, 90,
               L"O launcher salva apenas caminhos e preferências locais em user\\config\\scarlet_launcher.ini. "
               L"Nenhum jogo, DLC, update ou key é incluído no pacote Scarlet PC.");
}

void CreateNavigation() {
    auto make_nav = [](int id, int y, const wchar_t* text) {
        HWND h = CreateWindowExW(0, L"BUTTON", text,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                 28, y, 205, 52, g_main, reinterpret_cast<HMENU>(id),
                                 g_instance, nullptr);
        SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_bold), TRUE);
        return h;
    };
    make_nav(ID_TAB_GAME, 190, L"JOGO");
    make_nav(ID_TAB_GRAPHICS, 250, L"GRÁFICOS");
    make_nav(ID_TAB_CONTROLS, 310, L"CONTROLES");
    make_nav(ID_TAB_GENERAL, 370, L"GERAL");

    HWND play = CreateWindowExW(0, L"BUTTON", L"JOGAR",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                1008, 650, 220, 58, g_main,
                                reinterpret_cast<HMENU>(ID_PLAY), g_instance, nullptr);
    SendMessageW(play, WM_SETFONT, reinterpret_cast<WPARAM>(g_font_large), TRUE);
}

void CreateUi() {
    CreateNavigation();
    CreateGamePage();
    CreateGraphicsPage();
    CreateControlsPage();
    CreateGeneralPage();
    ShowPage(Game);
}

void DrawButton(const DRAWITEMSTRUCT& dis) {
    wchar_t text[256]{};
    GetWindowTextW(dis.hwndItem, text, 256);

    const int id = static_cast<int>(dis.CtlID);
    const bool pressed = (dis.itemState & ODS_SELECTED) != 0;
    const bool focused = (dis.itemState & ODS_FOCUS) != 0;
    const bool tab = id >= ID_TAB_GAME && id <= ID_TAB_GENERAL;
    const bool active_tab = tab && static_cast<int>(g_page) == (id - ID_TAB_GAME);
    const bool play = id == ID_PLAY;

    COLORREF bg = RGB(38, 29, 33);
    COLORREF border = RGB(180, 143, 42);
    COLORREF fg = RGB(245, 240, 224);

    if (play) {
        bg = pressed ? RGB(125, 18, 18) : RGB(171, 35, 31);
        border = RGB(233, 196, 50);
    } else if (active_tab) {
        bg = RGB(143, 30, 27);
        border = RGB(231, 193, 50);
    } else if (pressed) {
        bg = RGB(68, 43, 45);
    }

    HBRUSH brush = CreateSolidBrush(bg);
    FillRect(dis.hDC, &dis.rcItem, brush);
    DeleteObject(brush);

    HPEN pen = CreatePen(PS_SOLID, (active_tab || play || focused) ? 2 : 1, border);
    HGDIOBJ old_pen = SelectObject(dis.hDC, pen);
    HGDIOBJ old_brush = SelectObject(dis.hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis.hDC, dis.rcItem.left, dis.rcItem.top, dis.rcItem.right, dis.rcItem.bottom);
    SelectObject(dis.hDC, old_brush);
    SelectObject(dis.hDC, old_pen);
    DeleteObject(pen);

    SetBkMode(dis.hDC, TRANSPARENT);
    SetTextColor(dis.hDC, fg);
    HFONT font = play ? g_font_large : g_font_bold;
    HGDIOBJ old_font = SelectObject(dis.hDC, font);
    RECT text_rect = dis.rcItem;
    DrawTextW(dis.hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis.hDC, old_font);
}

void DrawBackground(HWND hwnd, HDC hdc) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    Graphics graphics(hdc);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (g_background && g_background->GetLastStatus() == Ok) {
        graphics.DrawImage(g_background.get(), 0, 0, width, height);
    } else {
        SolidBrush fallback(Color(255, 94, 13, 16));
        graphics.FillRectangle(&fallback, 0, 0, width, height);
    }

    SolidBrush left_panel(Color(220, 16, 10, 16));
    graphics.FillRectangle(&left_panel, 0, 0, 260, height);

    SolidBrush content_panel(Color(220, 17, 14, 20));
    graphics.FillRectangle(&content_panel, 270, 72, width - 300, height - 132);

    Pen gold(Color(230, 222, 181, 46), 2.0f);
    graphics.DrawLine(&gold, 270, 72, width - 30, 72);
    graphics.DrawLine(&gold, 270, height - 60, width - 30, height - 60);

    FontFamily family(L"Segoe UI");
    Gdiplus::Font title_font(&family, 28.0f, FontStyleBold, UnitPixel);
    SolidBrush title_brush(Color(255, 245, 213, 77));
    graphics.DrawString(L"SCARLET PC", -1, &title_font, PointF(28.0f, 94.0f), &title_brush);

    Gdiplus::Font small_font(&family, 13.0f, FontStyleRegular, UnitPixel);
    SolidBrush small_brush(Color(220, 240, 231, 213));
    graphics.DrawString(L"Pokémon Scarlet • Windows", -1, &small_font,
                        PointF(30.0f, 133.0f), &small_brush);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_CREATE:
        g_main = hwnd;
        CreateUi();
        LoadSettings();
        return 0;

    case WM_COMMAND: {
        const int id = LOWORD(wparam);
        switch (id) {
        case ID_TAB_GAME: ShowPage(Game); return 0;
        case ID_TAB_GRAPHICS: ShowPage(Graphics); return 0;
        case ID_TAB_CONTROLS: ShowPage(Controls); return 0;
        case ID_TAB_GENERAL: ShowPage(General); return 0;
        case ID_PLAY: LaunchGame(); return 0;

        case ID_GAME_BROWSE: {
            const auto file = OpenFile(hwnd,
                L"Nintendo Switch (*.nsp;*.xci)\0*.nsp;*.xci\0NSP (*.nsp)\0*.nsp\0XCI (*.xci)\0*.xci\0Todos os arquivos\0*.*\0",
                GetText(g_game_edit));
            if (!file.empty()) SetText(g_game_edit, file);
            RefreshStatus();
            return 0;
        }
        case ID_UPDATE_BROWSE: {
            const auto file = OpenFile(hwnd,
                L"Update Nintendo Switch (*.nsp;*.xci)\0*.nsp;*.xci\0Todos os arquivos\0*.*\0",
                GetText(g_update_edit));
            if (!file.empty()) SetText(g_update_edit, file);
            RefreshStatus();
            return 0;
        }
        case ID_KEYS_BROWSE: {
            const auto file = OpenFile(hwnd,
                L"prod.keys\0prod.keys\0Arquivos de keys (*.keys)\0*.keys\0Todos os arquivos\0*.*\0",
                GetText(g_keys_edit));
            if (!file.empty()) SetText(g_keys_edit, file);
            RefreshStatus();
            return 0;
        }
        case ID_DLC_ADD: {
            const auto file = OpenFile(hwnd,
                L"DLC Nintendo Switch (*.nsp;*.xci)\0*.nsp;*.xci\0Todos os arquivos\0*.*\0");
            if (!file.empty() &&
                std::find(g_dlcs.begin(), g_dlcs.end(), file) == g_dlcs.end()) {
                g_dlcs.push_back(file);
                RefreshDlcList();
                RefreshStatus();
            }
            return 0;
        }
        case ID_DLC_REMOVE: {
            const int selected = static_cast<int>(SendMessageW(g_dlc_list, LB_GETCURSEL, 0, 0));
            if (selected >= 0 && selected < static_cast<int>(g_dlcs.size())) {
                g_dlcs.erase(g_dlcs.begin() + selected);
                RefreshDlcList();
                RefreshStatus();
            }
            return 0;
        }
        case ID_OPEN_USER:
            OpenFolder(g_exe_dir / L"user");
            return 0;
        case ID_OPEN_LOGS:
            OpenFolder(g_exe_dir / L"user" / L"log");
            return 0;
        default:
            break;
        }
        if (HIWORD(wparam) == EN_CHANGE &&
            (id == ID_GAME_EDIT || id == ID_UPDATE_EDIT || id == ID_KEYS_EDIT)) {
            RefreshStatus();
        }
        break;
    }

    case WM_DRAWITEM:
        DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam));
        return TRUE;

    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(244, 238, 226));
        SetBkMode(dc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC dc = reinterpret_cast<HDC>(wparam);
        SetTextColor(dc, RGB(244, 238, 226));
        SetBkColor(dc, RGB(30, 25, 31));
        return reinterpret_cast<LRESULT>(g_edit_brush);
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        DrawBackground(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        SaveSettings();
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    g_instance = instance;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&controls);

    GdiplusStartupInput gdiplus_input;
    if (GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr) != Ok) return 1;

    g_exe_dir = ExeDir();
    g_ini_path = g_exe_dir / L"user" / L"config" / L"scarlet_launcher.ini";
    const fs::path background = g_exe_dir / L"assets" / L"scarlet_launcher_background.jpg";
    if (fs::is_regular_file(background)) {
        g_background = std::make_unique<Image>(background.c_str());
    }

    g_edit_brush = CreateSolidBrush(RGB(30, 25, 31));
    g_panel_brush = CreateSolidBrush(RGB(20, 16, 21));

    g_font = CreateFontW(-17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_bold = CreateFontW(-17, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    g_font_large = CreateFontW(-24, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kWindowClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&wc);

    RECT window_rect{0, 0, kWindowWidth, kWindowHeight};
    AdjustWindowRectEx(&window_rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                       FALSE, 0);

    HWND hwnd = CreateWindowExW(
        0, kWindowClass, kTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT,
        window_rect.right - window_rect.left, window_rect.bottom - window_rect.top,
        nullptr, nullptr, instance, nullptr);

    if (!hwnd) return 2;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_font) DeleteObject(g_font);
    if (g_font_bold) DeleteObject(g_font_bold);
    if (g_font_large) DeleteObject(g_font_large);
    if (g_edit_brush) DeleteObject(g_edit_brush);
    if (g_panel_brush) DeleteObject(g_panel_brush);
    g_background.reset();
    GdiplusShutdown(g_gdiplus_token);
    return static_cast<int>(msg.wParam);
}
