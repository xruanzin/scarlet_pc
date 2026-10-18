// Windows host layer (runtime/platform.h).
#include <windows.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <timeapi.h>
#include <io.h>
#include <wchar.h>

#include "../platform.h"
#include "../paths.h"

// ---- guest address space
// The window is reserved up front and committed per region (plat_commit from the loader / kernel). Anything
// the guest touches outside a committed region is committed on its first access by the exception handler
// below, so the window behaves like macOS's all-RW reservation. The OS itself (ReadFile into a guest buffer)
// does not fault: such buffers must be inside committed regions.
static uint64_t g_lo, g_hi;
#define COMMIT_CHUNK 0x10000ull

static bool lazy_commit(uint64_t a) {
    if (a < g_lo || a >= g_hi) return false;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_RESERVE) return false;
    uint64_t base = a & ~(COMMIT_CHUNK - 1);
    return VirtualAlloc((void *)(uintptr_t)base, COMMIT_CHUNK, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

static bool g_report_crashes;

static LONG CALLBACK vectored_handler(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    uint64_t fa = er->ExceptionInformation[1];
    if (lazy_commit(fa)) return EXCEPTION_CONTINUE_EXECUTION;
    if (g_report_crashes && rt_fault_watch(fa, ep->ContextRecord->Rip)) return EXCEPTION_CONTINUE_EXECUTION;
    return EXCEPTION_CONTINUE_SEARCH;
}

bool plat_reserve_guest(uint64_t lo, uint64_t hi) {
    void *p = VirtualAlloc((void *)(uintptr_t)lo, hi - lo, MEM_RESERVE, PAGE_READWRITE);
    if (p != (void *)(uintptr_t)lo) return false;
    g_lo = lo;
    g_hi = hi;
    AddVectoredExceptionHandler(1, vectored_handler);
    return true;
}

// Decommit everything: the range stays reserved, and pages come back zeroed when committed again (lazily).
bool plat_reset_guest(uint64_t lo, uint64_t hi) {
    return VirtualFree((void *)(uintptr_t)lo, hi - lo, MEM_DECOMMIT) != 0;
}

void plat_commit(uint64_t addr, uint64_t size) {
    if (!size) return;
    uint64_t a = addr & ~0xFFFull, e = (addr + size + 0xFFF) & ~0xFFFull;
    if (!VirtualAlloc((void *)(uintptr_t)a, e - a, MEM_COMMIT, PAGE_READWRITE))
        fprintf(stderr, "[plat] commit %llx+%llx failed (%lu)\n", (unsigned long long)addr,
                (unsigned long long)size, GetLastError());
}

int plat_protect(uint64_t addr, uint64_t size, int prot) {
    DWORD p = prot == PLAT_PROT_NONE ? PAGE_NOACCESS : prot == PLAT_PROT_READ ? PAGE_READONLY : PAGE_READWRITE, old;
    if (addr >= g_lo && addr < g_hi) plat_commit(addr, size);
    return VirtualProtect((void *)(uintptr_t)addr, size, p, &old) ? 0 : -1;
}

// ---- time
static LARGE_INTEGER qpf;

uint64_t plat_now_ns(void) {
    if (!qpf.QuadPart) QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (uint64_t)((unsigned __int128)t.QuadPart * 1000000000u / (uint64_t)qpf.QuadPart);
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x2
#endif

void plat_sleep_until_ns(uint64_t t) {
    static __thread HANDLE timer;
    if (!timer) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS);
    }
    for (;;) {
        uint64_t now = plat_now_ns();
        if (now >= t) return;
        uint64_t left = t - now;
        if (left < 200000) { YieldProcessor(); continue; }  // last 0.2 ms: spin (timer slack)
        LARGE_INTEGER due = {.QuadPart = -(LONGLONG)((left - 100000) / 100)};  // relative, 100 ns units
        if (!timer || !SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { Sleep(1); continue; }
        WaitForSingleObject(timer, INFINITE);
    }
}

void plat_thread_realtime(uint64_t period_ns) {
    (void)period_ns;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
}

const void *plat_map_file_ro(int fd, uint64_t *size) {
    if (size) *size = 0;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    LARGE_INTEGER n;
    if (!GetFileSizeEx(h, &n) || n.QuadPart <= 0) return NULL;
    HANDLE mapping = CreateFileMappingW(h, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) return NULL;
    void *p = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);  // the mapped view keeps the section alive
    if (!p) return NULL;
    if (size) *size = (uint64_t)n.QuadPart;
    return p;
}

void plat_unmap_file(const void *ptr) {
    if (ptr) UnmapViewOfFile(ptr);
}

void plat_set_cpu_boost(bool enabled) {
    static DWORD normal_class;
    if (!normal_class) normal_class = GetPriorityClass(GetCurrentProcess());
    DWORD target = enabled ? HIGH_PRIORITY_CLASS : (normal_class ? normal_class : NORMAL_PRIORITY_CLASS);
    if (GetPriorityClass(GetCurrentProcess()) != target) {
        SetPriorityClass(GetCurrentProcess(), target);
        fprintf(stderr, "[plat] CPU fast-load boost %s (priority class %#lx)\n", enabled ? "on" : "off", (unsigned long)target);
    }
}

void plat_guest_thread_config(int priority, int core) {
    // Horizon uses smaller numbers for higher priority. Scarlet's latency-sensitive
    // renderer/audio threads sit around 42-43, main/jobs around 44-45.
    int winprio = priority <= 43 ? THREAD_PRIORITY_ABOVE_NORMAL :
                  priority >= 50 ? THREAD_PRIORITY_BELOW_NORMAL :
                                   THREAD_PRIORITY_NORMAL;
    SetThreadPriority(GetCurrentThread(), winprio);

    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpus && core >= 0) {
        // Spread virtual Switch cores over the host's logical CPUs without
        // hard affinity: 0,3,6,9 on a 12-thread Ryzen, while Windows may still migrate.
        DWORD ideal = ((DWORD)(core & 3) * cpus) / 4;
        if (ideal >= cpus) ideal = cpus - 1;
        SetThreadIdealProcessor(GetCurrentThread(), ideal);
    }
}

// ---- native text input (nn::swkbd replacement)
enum { TEXTDLG_EDIT = 4101, TEXTDLG_OK = 4102, TEXTDLG_CANCEL = 4103 };

typedef struct {
    HWND hwnd, edit, owner;
    int done, ok, min_chars;
    char *out;
    size_t out_cap;
} TextInputDialog;

typedef struct { HWND hwnd; LONG64 area; } TextOwnerFind;

static BOOL CALLBACK text_owner_enum(HWND w, LPARAM lp) {
    if (!IsWindowVisible(w) || GetWindow(w, GW_OWNER)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[64] = L"";
    GetClassNameW(w, cls, 64);
    if (!wcscmp(cls, L"ConsoleWindowClass")) return TRUE;
    RECT r;
    if (!GetWindowRect(w, &r)) return TRUE;
    LONG64 area = (LONG64)(r.right - r.left) * (r.bottom - r.top);
    TextOwnerFind *f = (TextOwnerFind *)lp;
    if (area > f->area) { f->area = area; f->hwnd = w; }
    return TRUE;
}

static int utf8_wide(const char *src, wchar_t *dst, int cap) {
    if (!src) src = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, src, -1, dst, cap);
    if (!n && cap) dst[0] = 0;
    return n;
}

static LRESULT CALLBACK textdlg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    TextInputDialog *d = (TextInputDialog *)(uintptr_t)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        d = (TextInputDialog *)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)d);
        d->hwnd = h;
    }
    if (!d) return DefWindowProcW(h, msg, wp, lp);

    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == TEXTDLG_OK) {
            int n = GetWindowTextLengthW(d->edit);
            if (n < d->min_chars) {
                MessageBeep(MB_ICONWARNING);
                SetFocus(d->edit);
                return 0;
            }
            wchar_t *w = calloc((size_t)n + 1, sizeof *w);
            if (!w) return 0;
            GetWindowTextW(d->edit, w, n + 1);
            if (d->out && d->out_cap) {
                int got = WideCharToMultiByte(CP_UTF8, 0, w, -1, d->out, (int)d->out_cap, NULL, NULL);
                if (!got) d->out[0] = 0;
                d->out[d->out_cap - 1] = 0;
            }
            free(w);
            d->ok = 1;
            d->done = 1;
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(wp) == TEXTDLG_CANCEL) {
            d->ok = 0;
            d->done = 1;
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_CLOSE:
        d->ok = 0;
        d->done = 1;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        d->done = 1;
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int plat_text_input_dialog(const char *title, const char *subtitle, const char *guide,
                           const char *initial, int min_chars, int max_chars,
                           char *out, size_t out_cap) {
    static ATOM atom;
    if (!atom) {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof wc;
        wc.lpfnWndProc = textdlg_proc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.hCursor = LoadCursorW(NULL, IDC_IBEAM);
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.lpszClassName = L"ScarletNativeTextInput";
        atom = RegisterClassExW(&wc);
        if (!atom && GetLastError() == ERROR_CLASS_ALREADY_EXISTS) atom = 1;
    }
    if (!atom) return 0;

    TextOwnerFind find = {0};
    EnumWindows(text_owner_enum, (LPARAM)&find);

    wchar_t wtitle[256], wsub[512], wguide[768], winitial[1024], body[1280];
    utf8_wide(title && *title ? title : "Inserir texto", wtitle, 256);
    utf8_wide(subtitle, wsub, 512);
    utf8_wide(guide, wguide, 768);
    utf8_wide(initial, winitial, 1024);
    if (wsub[0] && wguide[0]) _snwprintf(body, 1280, L"%ls\n%ls", wsub, wguide);
    else _snwprintf(body, 1280, L"%ls", wsub[0] ? wsub : wguide);
    body[1279] = 0;

    const int ww = 580, wh = 245;
    RECT base = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (find.hwnd) GetWindowRect(find.hwnd, &base);
    int x = base.left + ((base.right - base.left) - ww) / 2;
    int y = base.top + ((base.bottom - base.top) - wh) / 2;

    TextInputDialog d = {0};
    d.owner = find.hwnd;
    d.min_chars = min_chars > 0 ? min_chars : 0;
    d.out = out;
    d.out_cap = out_cap;
    if (out && out_cap) out[0] = 0;

    HWND h = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST | WS_EX_CONTROLPARENT,
                             L"ScarletNativeTextInput", wtitle,
                             WS_POPUP | WS_CAPTION | WS_SYSMENU,
                             x, y, ww, wh, find.hwnd, NULL, GetModuleHandleW(NULL), &d);
    if (!h) return 0;

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HWND label = CreateWindowExW(0, L"STATIC", body, WS_CHILD | WS_VISIBLE,
                                 22, 18, 520, 58, h, NULL, GetModuleHandleW(NULL), NULL);
    d.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", winitial,
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             22, 83, 520, 32, h, (HMENU)(INT_PTR)TEXTDLG_EDIT, GetModuleHandleW(NULL), NULL);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                              330, 145, 100, 34, h, (HMENU)(INT_PTR)TEXTDLG_OK, GetModuleHandleW(NULL), NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancelar",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                  442, 145, 100, 34, h, (HMENU)(INT_PTR)TEXTDLG_CANCEL, GetModuleHandleW(NULL), NULL);
    SendMessageW(label, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(d.edit, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(ok, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(cancel, WM_SETFONT, (WPARAM)font, TRUE);
    SendMessageW(d.edit, EM_SETLIMITTEXT, (WPARAM)(max_chars > 0 ? max_chars : 500), 0);

    if (find.hwnd) EnableWindow(find.hwnd, FALSE);
    ShowWindow(h, SW_SHOW);
    UpdateWindow(h);
    SetForegroundWindow(h);
    SetFocus(d.edit);
    SendMessageW(d.edit, EM_SETSEL, 0, -1);

    MSG msg;
    while (!d.done && GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == d.edit) {
            if (msg.wParam == VK_RETURN) { SendMessageW(h, WM_COMMAND, TEXTDLG_OK, 0); continue; }
            if (msg.wParam == VK_ESCAPE) { SendMessageW(h, WM_COMMAND, TEXTDLG_CANCEL, 0); continue; }
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (find.hwnd) {
        EnableWindow(find.hwnd, TRUE);
        SetForegroundWindow(find.hwnd);
    }
    return d.ok;
}

// ---- process
// Release builds are GUI-subsystem executables (no console window): stdout / stderr go to lasrecomp.log next to the
// exe, and a fatal error shows a message box (plat_fatal_notice) instead of vanishing. Processes started by this one
// (the start screen re-executing itself for the game) append to the same log.
static char g_log_path[MAX_PATH];

static void log_to_file_without_console(void) {
    if (GetStdHandle(STD_ERROR_HANDLE) && GetStdHandle(STD_ERROR_HANDLE) != INVALID_HANDLE_VALUE) return;  // console / pipe
    char exe[MAX_PATH];
    DWORD n = GetModuleFileNameA(NULL, exe, sizeof exe);
    if (!n || n >= sizeof exe) return;
    char *slash = strrchr(exe, '\\');
    if (slash) slash[1] = 0;
    snprintf(g_log_path, sizeof g_log_path, "%slasrecomp.log", exe);
    bool append = getenv("LASRECOMP_LOG_APPEND") != NULL;
    if (!append) {
        // Keep the previous run around for crash reports: a fresh launch moves it aside first
        // (a crash log would otherwise be lost the moment the game is started again).
        char prev[MAX_PATH];
        snprintf(prev, sizeof prev, "%slasrecomp-prev.log", exe);
        DeleteFileA(prev);
        MoveFileA(g_log_path, prev);
    }
    if (!freopen(g_log_path, append ? "ab" : "wb", stderr)) { g_log_path[0] = 0; return; }
    setvbuf(stderr, NULL, _IONBF, 0);
    _dup2(_fileno(stderr), _fileno(stdout));
    _putenv("LASRECOMP_LOG_APPEND=1");
}

// The recompiled game code is built for x86-64-v3 (Makefile GENARCH: AVX2, FMA, BMI2): say so instead of crashing
// with an illegal instruction on an older CPU.
static void require_cpu(void) {
    __builtin_cpu_init();
    if (__builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma") && __builtin_cpu_supports("bmi2")) return;
    MessageBoxA(NULL, RT_APP_NAME " needs a CPU with AVX2 (Intel Haswell / 2013 or newer, AMD Excavator / Zen or newer).",
                RT_APP_NAME, MB_ICONERROR | MB_OK);
    ExitProcess(1);
}

// Fatal error (rt_fatal_trace): the last FATAL / exception line of the log in a message box, when there is no console.
void plat_fatal_notice(void) {
    if (!g_log_path[0]) return;
    fflush(stderr);
    char text[1024] = RT_APP_NAME " stopped because of an error.";
    FILE *f = fopen(g_log_path, "rb");
    if (f) {
        static char tail[16384];
        fseek(f, 0, SEEK_END);
        long sz = ftell(f), from = sz > (long)sizeof tail - 1 ? sz - (long)sizeof tail + 1 : 0;
        fseek(f, from, SEEK_SET);
        size_t got = fread(tail, 1, sizeof tail - 1, f);
        tail[got] = 0;
        fclose(f);
        char *line = NULL;
        for (char *p = tail; (p = strstr(p, "FATAL: ")) != NULL; p++) line = p;
        if (!line) for (char *p = tail; (p = strstr(p, "*** ")) != NULL; p++) line = p;
        if (line) {
            char *end = strchr(line, '\n');
            if (end) *end = 0;
            snprintf(text, sizeof text, RT_APP_NAME " stopped because of an error:\n\n%.600s", line);
            if (strstr(line, "Vulkan") || strstr(line, "VK_") || strstr(line, "vk")) {
                size_t l = strlen(text);
                snprintf(text + l, sizeof text - l, "\n\n" RT_APP_NAME " needs a graphics driver with Vulkan 1.3: install the "
                         "latest driver for your GPU (NVIDIA, AMD or Intel).");
            }
        }
    }
    size_t l = strlen(text);
    snprintf(text + l, sizeof text - l, "\n\nDetails: %s", g_log_path);
    MessageBoxA(NULL, text, RT_APP_NAME, MB_ICONERROR | MB_OK);
}

void plat_init(void) {
    log_to_file_without_console();
    require_cpu();
    _fmode = _O_BINARY;  // fopen/open default to binary (the runtime's "r"/"w" files are binary or LF text)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    SetConsoleOutputCP(CP_UTF8);
    timeBeginPeriod(1);  // 1 ms Sleep / condition-variable timeouts (default 15.6 ms)
    // Opt out of EcoQoS and of Windows 11 ignoring the timer resolution while the window is occluded.
    PROCESS_POWER_THROTTLING_STATE pt = {
        .Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION,
        .ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION,
        .StateMask = 0,
    };
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &pt, sizeof pt);
}

bool plat_exe_path(char *buf, size_t size) {
    DWORD n = GetModuleFileNameA(NULL, buf, (DWORD)size);
    return n > 0 && n < size;
}

// Quote one argument the way CommandLineToArgvW / the CRT parse it back.
static void append_arg(char *out, size_t cap, const char *a) {
    size_t n = strlen(out);
    bool q = !*a || strpbrk(a, " \t\"") != NULL;
    if (n && n + 1 < cap) out[n++] = ' ';
    if (q && n + 1 < cap) out[n++] = '"';
    for (const char *p = a; *p; p++) {
        int bs = 0;
        while (*p == '\\') { bs++; p++; }
        int reps = !*p ? (q ? 2 * bs : bs) : *p == '"' ? 2 * bs + 1 : bs;
        for (int i = 0; i < reps && n + 1 < cap; i++) out[n++] = '\\';
        if (!*p) break;
        if (n + 1 < cap) out[n++] = *p;
    }
    if (q && n + 1 < cap) out[n++] = '"';
    out[n] = 0;
}

// No exec() on Windows: start the new process with the same console / standard handles, then exit.
void plat_exec(char *const argv[]) {
    STARTUPINFOW si = {.cb = sizeof si};
    PROCESS_INFORMATION pi;
    BOOL ok;
    fflush(stdout);
    fflush(stderr);
    if (!argv) {
        ok = CreateProcessW(NULL, GetCommandLineW(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    } else {
        static char cmd[32768];
        cmd[0] = 0;
        for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
        static wchar_t wcmd[32768];
        MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
        ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    }
    if (!ok) return;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    ExitProcess(0);
}

// ---- diagnostics
// DLL frames are named module+offset; frames in our own executable are left to the caller, which maps
// them to guest functions (runtime/dispatch.c) - mingw builds carry DWARF, not PDB symbols.
const char *plat_symbol(uint64_t pc, uint64_t *offset) {
    static __thread char name[MAX_PATH];
    HMODULE m;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)(uintptr_t)pc, &m))
        return NULL;
    if (m == GetModuleHandleA(NULL)) return NULL;
    char path[MAX_PATH];
    if (!GetModuleFileNameA(m, path, sizeof path)) return NULL;
    const char *b = strrchr(path, '\\');
    snprintf(name, sizeof name, "%s", b ? b + 1 : path);
    if (offset) *offset = pc - (uint64_t)(uintptr_t)m;
    return name;
}

const char *rt_host_fn_name(uint64_t pc, uint64_t *offset);  // runtime/dispatch.c

void plat_print_backtrace(void) {
    void *bt[64];
    USHORT n = RtlCaptureStackBackTrace(0, 64, bt, NULL);
    uint64_t exe = (uint64_t)(uintptr_t)GetModuleHandleA(NULL);
    for (int i = 0; i < n; i++) {
        uint64_t pc = (uint64_t)(uintptr_t)bt[i], off = 0;
        const char *s = rt_host_fn_name(pc, &off);  // (recompiled game code first: f_<guest addr>)
        if (!s) s = plat_symbol(pc, &off);
        if (s) fprintf(stderr, "  #%-2d %016llx %s+%llx\n", i, (unsigned long long)pc, s, (unsigned long long)off);
        else fprintf(stderr, "  #%-2d %016llx lasrecomp+%llx\n", i, (unsigned long long)pc,
                     (unsigned long long)(pc - exe));
    }
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS *ep) {
    EXCEPTION_RECORD *er = ep->ExceptionRecord;
    uint64_t fa = er->NumberParameters >= 2 ? er->ExceptionInformation[1] : 0;
    char what[48];
    snprintf(what, sizeof what, "exception %08lx", er->ExceptionCode);
    // x64 has no link register: report the return address on top of the stack (valid at function entry).
    uint64_t lr = 0;
    MEMORY_BASIC_INFORMATION mi;
    if (VirtualQuery((void *)(uintptr_t)ep->ContextRecord->Rsp, &mi, sizeof mi) && mi.State == MEM_COMMIT)
        lr = *(uint64_t *)(uintptr_t)ep->ContextRecord->Rsp;
    rt_fault_report(what, fa, ep->ContextRecord->Rip, lr);
}

void plat_crash_handler_install(void) {
    ULONG guarantee = 64 << 10;  // room for the report after a stack overflow
    SetThreadStackGuarantee(&guarantee);
    if (g_report_crashes) return;
    g_report_crashes = true;
    SetUnhandledExceptionFilter(unhandled_filter);
}

// ---- POSIX shims (win_compat.h)
static ssize_t prw(int fd, void *buf, size_t n, off_t off, bool write) {
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    size_t done = 0;
    while (done < n) {
        OVERLAPPED ov = {0};
        uint64_t o = (uint64_t)off + done;
        ov.Offset = (DWORD)o;
        ov.OffsetHigh = (DWORD)(o >> 32);
        DWORD chunk = n - done > 0x40000000 ? 0x40000000 : (DWORD)(n - done), got = 0;
        BOOL ok = write ? WriteFile(h, (const char *)buf + done, chunk, &got, &ov)
                        : ReadFile(h, (char *)buf + done, chunk, &got, &ov);
        if (!ok) {
            if (GetLastError() == ERROR_HANDLE_EOF) break;
            return done ? (ssize_t)done : -1;
        }
        if (!got) break;
        done += got;
    }
    return (ssize_t)done;
}
ssize_t pread(int fd, void *buf, size_t n, off_t off) { return prw(fd, buf, n, off, false); }
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) { return prw(fd, (void *)buf, n, off, true); }

// ---- child processes
PlatProc plat_spawn(const char *const argv[], const char *log_path, bool append_log) {
    static __thread char cmd[32768];
    cmd[0] = 0;
    for (int i = 0; argv[i]; i++) append_arg(cmd, sizeof cmd, argv[i]);
    static __thread wchar_t wcmd[32768], wlog[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, 32768);
    MultiByteToWideChar(CP_UTF8, 0, log_path ? log_path : "NUL", -1, wlog, MAX_PATH);
    SECURITY_ATTRIBUTES sa = {.nLength = sizeof sa, .bInheritHandle = TRUE};
    HANDLE log = CreateFileW(wlog, append_log ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &sa, append_log ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE) return 0;
    STARTUPINFOW si = {.cb = sizeof si, .dwFlags = STARTF_USESTDHANDLES, .hStdInput = NULL, .hStdOutput = log,
                       .hStdError = log};
    PROCESS_INFORMATION pi;
    // CreateProcess doesn't resolve relative paths written with '/' (e.g. a helper under tools/): pass the program
    // as a full path, like posix_spawn takes it as given
    static __thread wchar_t wprog[MAX_PATH], wfull[MAX_PATH];
    MultiByteToWideChar(CP_UTF8, 0, argv[0], -1, wprog, MAX_PATH);
    for (wchar_t *p = wprog; *p; p++) if (*p == L'/') *p = L'\\';
    const wchar_t *app = GetFullPathNameW(wprog, MAX_PATH, wfull, NULL) ? wfull : NULL;
    BOOL ok = CreateProcessW(app, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
    if (!ok) fprintf(stderr, "[plat] could not start %s (error %lu)\n", argv[0], GetLastError());
    CloseHandle(log);
    if (!ok) return 0;
    // every child in one job that ends with this process (a compiler's own children too: no leftovers on quit)
    static HANDLE job;
    if (!job && (job = CreateJobObjectW(NULL, NULL))) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    return (PlatProc)pi.hProcess;
}

void plat_proc_kill(PlatProc p) {  // (what it started ends with this process: the job above)
    TerminateProcess((HANDLE)p, 255);
    WaitForSingleObject((HANDLE)p, INFINITE);
    CloseHandle((HANDLE)p);
}

int plat_proc_wait(PlatProc p, bool block) {
    HANDLE h = (HANDLE)p;
    DWORD w = WaitForSingleObject(h, block ? INFINITE : 0);
    if (w == WAIT_TIMEOUT) return -1;
    if (w != WAIT_OBJECT_0) return 255;  // (not a process we can wait for: ended as far as the caller is concerned)
    DWORD code = 255;
    GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return code > 255 ? 255 : (int)code;  // (a crash's NTSTATUS, e.g. 0xC0000409, is not "still running" (-1))
}

bool plat_remove_tree(const char *dir) {
    // SHFileOperation wants a double-NUL-terminated path with backslashes
    static __thread wchar_t w[MAX_PATH + 2];
    int n = MultiByteToWideChar(CP_UTF8, 0, dir, -1, w, MAX_PATH);
    if (n <= 1) return false;
    w[n] = 0;
    for (wchar_t *p = w; *p; p++) if (*p == L'/') *p = L'\\';
    SHFILEOPSTRUCTW op = {.wFunc = FO_DELETE, .pFrom = w, .fFlags = FOF_NO_UI};
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// End the process at once, skipping DLL teardown (from an atexit handler, after flushing). Exit status 0.
void plat_terminate(void) { TerminateProcess(GetCurrentProcess(), 0); }
