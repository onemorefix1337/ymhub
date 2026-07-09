#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <dwmapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <ctime>
#include <cstdlib>
#include <cstdio>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <shcore.h>
#include <shlobj.h>
#include <DbgHelp.h>
#include <CommCtrl.h>
#include "../shared/ipc.h"
#include "../shared/version.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace Microsoft::WRL;
namespace wmc = winrt::Windows::Media::Control;

#define YM_CDP_PORT 9876

// Loopback-only bridge port for external tools (e.g. a game overlay script)
// to read now-playing state / issue playback actions — see HttpBridgeThreadFn.
#define YMHUB_BRIDGE_PORT 47990

static HANDLE        g_mutex      = nullptr;
static volatile bool g_run        = false;
// DOM-polled state (CdpQueryLiked/CdpQueryCoverUrl, see CdpAnnounceThread/
// LogBadgeThread below) — plain process-local now that there's no host on
// the other end of what used to be an IPC round-trip (g_ipc->ymLiked/
// coverUrl). Фаза 5's "реестр вместо IPC" cleanup.
static bool          g_domLiked   = false;
static std::wstring  g_domCoverUrl;
// Host set this while the hub's rebind UI was open, so hotkey handlers
// would skip live keys mid-rebind — no such UI exists yet post-migration,
// stays as a plain local for when in-page rebinding lands.
static bool          g_rebinding  = false;
// Overlay window handle — created by UiThread (Фаза 4a). Declared this
// early because ExecCmd (below) needs to PostMessageW to it.
static HWND          g_hwnd       = nullptr;

// Captured from DllMain's first parameter — needed for SetWindowsHookExW,
// which wants the HINSTANCE of the module that actually contains the hook
// procedure (this DLL now, not whatever GetModuleHandleW(nullptr) would
// resolve to inside an injected context — that's YM's own EXE module,
// which would be semantically wrong even if it happened to work).
static HINSTANCE g_hInst = nullptr;

// ── Registry (Фаза 1: hotkey config now read directly here instead of
// via the host->DLL IPC copy — see LoadKeys/LoadYmKeys below) ──
static const wchar_t* REG_APP = L"Software\\YMHub";
static DWORD RegGetDW(HKEY root, const wchar_t* k, const wchar_t* v, DWORD def) {
    HKEY hk; DWORD d = def, sz = sizeof(d);
    if (RegOpenKeyExW(root, k, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        RegQueryValueExW(hk, v, nullptr, nullptr, (BYTE*)&d, &sz);
        RegCloseKey(hk);
    }
    return d;
}
static void RegSetDW(HKEY root, const wchar_t* k, const wchar_t* v, DWORD d) {
    HKEY hk;
    RegCreateKeyExW(root, k, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    RegSetValueExW(hk, v, 0, REG_DWORD, (BYTE*)&d, sizeof(d));
    RegCloseKey(hk);
}
static std::wstring RegGetStr(HKEY root, const wchar_t* k, const wchar_t* v) {
    HKEY hk; wchar_t buf[4096] = { 0 }; DWORD sz = sizeof(buf);
    if (RegOpenKeyExW(root, k, 0, KEY_READ, &hk) == ERROR_SUCCESS) {
        if (RegQueryValueExW(hk, v, nullptr, nullptr, (BYTE*)buf, &sz) != ERROR_SUCCESS) buf[0] = 0;
        RegCloseKey(hk);
    }
    return buf;
}
static void RegSetStr(HKEY root, const wchar_t* k, const wchar_t* v, const std::wstring& s) {
    HKEY hk;
    RegCreateKeyExW(root, k, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr);
    RegSetValueExW(hk, v, 0, REG_SZ, (BYTE*)s.c_str(), (DWORD)((s.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(hk);
}

// Forward-declarations — реально определены гораздо ниже, среди остальной
// CDP-машинерии, но нужны здесь: Save*/LoadTweaks живут в начале файла
// (рядом с остальными Load*/Save* конфига), а с уходом IPC-раунд-трипа
// (см. ApplyTweaksNow ниже) они зовут их напрямую.
static void CdpApplyTweaks(DWORD mask, const wchar_t* customCssW);
static void CdpApplyNameHide(bool on, const wchar_t* customNameW);
static void CdpApplyPassportNameHide(bool on, const wchar_t* customNameW);

// Настройки твиков/своего CSS — раньше эти же значения реестра принадлежали
// хосту, а применялись через IPC-раунд-трип (записать в shared memory,
// затем WorkerThread замечает смену tweaksSeq и применяет). Без хоста
// обе стороны этого раунд-трипа всё равно оказались бы в одном процессе,
// так что Save* теперь просто применяют изменения сразу.
static DWORD        g_tweaksMask = 0;
static std::wstring g_customName;
static std::wstring g_customCss;
static bool         g_cheatMenuEnabled = false;
static void ApplyTweaksNow() {
    CdpApplyTweaks(g_tweaksMask, g_customCss.c_str());
    bool hideName = (g_tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0;
    CdpApplyNameHide(hideName, g_customName.c_str());
    CdpApplyPassportNameHide(hideName, g_customName.c_str());
}
static void LoadTweaks() {
    g_tweaksMask = RegGetDW(HKEY_CURRENT_USER, REG_APP, L"Tweaks", 0);
    g_customName = RegGetStr(HKEY_CURRENT_USER, REG_APP, L"CustomName");
    g_customCss = RegGetStr(HKEY_CURRENT_USER, REG_APP, L"CustomCss");
    g_cheatMenuEnabled = RegGetDW(HKEY_CURRENT_USER, REG_APP, L"CheatMenu", 0) != 0;
    ApplyTweaksNow();
}
static void SaveTweaks() {
    RegSetDW(HKEY_CURRENT_USER, REG_APP, L"Tweaks", g_tweaksMask);
    ApplyTweaksNow();
}
static void SaveCustomName(const std::wstring& s) {
    g_customName = s;
    RegSetStr(HKEY_CURRENT_USER, REG_APP, L"CustomName", g_customName);
    ApplyTweaksNow();
}
static void SaveCustomCss(const std::wstring& s) {
    g_customCss = s;
    RegSetStr(HKEY_CURRENT_USER, REG_APP, L"CustomCss", g_customCss);
    ApplyTweaksNow();
}

// Hotkey thread state
static HWND          g_hkWnd      = nullptr;
static DWORD         g_hkTid      = 0;
static bool          g_regIds[6]  = {};
static HHOOK         g_hook       = nullptr;

// Overlay-toggle/prev/next/toggle/like/dislike hotkeys — mirrors main.cpp's
// own g_keys/KEY_REG/KEY_DEF exactly (same registry values, so either side
// reads whatever the other last wrote there).
static IPCKey g_keys[6] = {};
static const wchar_t* KEY_REG[6] = { L"Key0", L"Key1", L"Key2", L"Key3", L"Key4", L"Key5" };
static const DWORD    KEY_DEF[6] = { 0x30031, 0x30025, 0x30027, 0x30020, 0, 0 };
static void LoadKeys() {
    for (int i = 0; i < 6; i++) {
        DWORD v = RegGetDW(HKEY_CURRENT_USER, REG_APP, KEY_REG[i], KEY_DEF[i]);
        g_keys[i] = { v >> 16, v & 0xFFFF };
    }
}

// YM's own native "Горячие клавиши" remap table + defaults — mirrors
// main.cpp's g_ymKeys/YMKEY_REG/YM_DEFAULT_VK, same order (play/pause,
// mute, seek fwd/back, vol up/down, like, dislike, repeat, shuffle,
// next, prev, fullscreen).
static IPCKey g_ymKeys[13] = {};
static const wchar_t* YMKEY_REG[13] = {
    L"YmKey0", L"YmKey1", L"YmKey2", L"YmKey3", L"YmKey4", L"YmKey5", L"YmKey6",
    L"YmKey7", L"YmKey8", L"YmKey9", L"YmKey10", L"YmKey11", L"YmKey12" };
static const DWORD YM_DEFAULT_VK[13] = {
    'K', 'M', 'L', 'J', VK_UP, VK_DOWN, 'F', 'D', 'R', 'S', 'N', 'P', 'W' };
static void LoadYmKeys() {
    for (int i = 0; i < 13; i++) {
        DWORD v = RegGetDW(HKEY_CURRENT_USER, REG_APP, YMKEY_REG[i], 0);
        g_ymKeys[i] = { v >> 16, v & 0xFFFF };
    }
}

// Custom message: worker thread -> hotkey thread to re-register keys
#define WM_REFRESHHK (WM_APP + 1)

// ── YM window finders ──────────────────────────────────────────
static HWND FindMainWnd() {
    struct S { DWORD pid; HWND hw; int area; };
    S s = { GetCurrentProcessId(), nullptr, 0 };
    EnumWindows([](HWND hw, LPARAM lp) -> BOOL {
        auto* s = reinterpret_cast<S*>(lp);
        DWORD pid = 0; GetWindowThreadProcessId(hw, &pid);
        if (pid != s->pid || !IsWindowVisible(hw)) return TRUE;
        RECT r; GetWindowRect(hw, &r);
        int a = (r.right - r.left) * (r.bottom - r.top);
        if (a > s->area) { s->area = a; s->hw = hw; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&s));
    return s.hw;
}

static HWND FindRenderWidget() {
    HWND main = FindMainWnd();
    if (!main) return nullptr;
    HWND render = nullptr;
    EnumChildWindows(main, [](HWND hw, LPARAM lp) -> BOOL {
        wchar_t cls[64] = {};
        GetClassNameW(hw, cls, 64);
        if (wcscmp(cls, L"Chrome_RenderWidgetHostHWND") == 0)
            { *reinterpret_cast<HWND*>(lp) = hw; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&render));
    return render ? render : main;
}

static void LogMsg(const std::string& s);

// ── Minimize/restore transition watch ───────────────────────────
// Polling IsIconic() alone (see CheatMenuThreadFn/LogBadgeThread) only
// catches the *steady state* of already being minimized — it still races
// the actual instant of the transition, in either direction, since a poll
// can land right as the window is mid-flip. Confirmed live: minimize-only
// and restore-after-minimize both still occasionally crashed with just
// the IsIconic guard. A WH_CALLWNDPROC hook on the main window's own
// thread sees WM_WINDOWPOSCHANGING/WM_SYSCOMMAND synchronously, before
// Windows finishes processing them, and opens a short cooldown from that
// instant — closing the gap a poll can't.
static HHOOK g_transitionHook = nullptr;
static volatile LONGLONG g_transitionCooldownUntil = 0; // GetTickCount64() deadline

// Hook procedures run inside the kernel-to-user callback boundary
// (win32u!NtUserMessageCall / KiUserCallbackDispatcherContinue) — an
// exception that escapes here can't unwind across that boundary and
// takes down the whole process with a generic "unhandled exception in
// a user callback" termination. Earlier version called LogMsg() (string
// building + file I/O) from here, which is exactly the kind of non-
// trivial work hook procs must never do. Keep this to the one atomic
// write, nothing else, ever.
static LRESULT CALLBACK TransitionWatchProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const CWPSTRUCT* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (cwp->message == WM_WINDOWPOSCHANGING || cwp->message == WM_SYSCOMMAND) {
            InterlockedExchange64(&g_transitionCooldownUntil, (LONGLONG)GetTickCount64() + 2500);
        }
    }
    return CallNextHookEx(g_transitionHook, nCode, wParam, lParam);
}

// Idempotent — cheap to call from every poll loop until the main window
// (and therefore its owning thread) actually exists to hook.
static void EnsureTransitionWatch() {
    if (g_transitionHook) return;
    HWND main = FindMainWnd();
    if (!main) return;
    DWORD tid = GetWindowThreadProcessId(main, nullptr);
    if (!tid) return;
    g_transitionHook = SetWindowsHookExW(WH_CALLWNDPROC, TransitionWatchProc, g_hInst, tid);
    LogMsg(g_transitionHook
        ? ("TransitionWatch installed on tid=" + std::to_string(tid))
        : ("TransitionWatch FAILED, err=" + std::to_string(GetLastError())));
}
static bool InTransitionCooldown() {
    return (LONGLONG)GetTickCount64() < g_transitionCooldownUntil;
}
// Minimize turned out not to be the only trigger — losing focus because
// another app went exclusive-fullscreen crashes it too (reported live),
// with no minimize/restore message involved at all. Being unfocused in
// the *steady state* is completely normal for a background music player
// (must not pause CDP for that — Discord/liked-status sync would just
// stop working the moment the user alt-tabs away) — what's dangerous is
// the *instant of change* itself, in either iconic-state or foreground-
// window identity. Detect that edge directly by comparing against the
// last poll, arm the same cooldown used for the message-hook case, and
// only ever treat the steady "minimized" state (not "merely unfocused")
// as unsafe on its own.
static HWND g_lastForeground = nullptr;
static bool g_lastIconic = false;
static bool CdpUnsafeNow(HWND main) {
    if (!main) return true;
    bool iconic = IsIconic(main) != FALSE;
    HWND fg = GetForegroundWindow();
    if (iconic != g_lastIconic || fg != g_lastForeground) {
        InterlockedExchange64(&g_transitionCooldownUntil, (LONGLONG)GetTickCount64() + 5000);
    }
    g_lastIconic = iconic;
    g_lastForeground = fg;
    return iconic || InTransitionCooldown();
}
// WaitUntilSafeForCdp() lives further down, right after CdpClose() is
// declared — it needs to actively drop the CDP session while waiting,
// not just poll IsIconic/cooldown.

// ── Chrome DevTools Protocol client (background-safe like/dislike) ──
// Chromium only treats WM_KEYDOWN as a real shortcut when the window
// has actual OS focus, and UI Automation's accessibility tree is empty
// for this app (Chromium a11y not active). CDP's Input.dispatchMouseEvent
// dispatches a trusted synthetic click straight into the page's input
// pipeline, independent of OS window focus. The host (main.cpp) relaunches
// YM once with --remote-debugging-port so this port is available.
static HINTERNET g_cdpSession = nullptr, g_cdpConnect = nullptr, g_cdpWs = nullptr;

// Guards every CdpSend+CdpRecv pair on the shared g_cdpWs connection.
// Harmless while only LogBadgeThread/WorkerThread/CdpAnnounceThread (each
// occasional, rarely truly concurrent) touched it, but the cheat-menu
// queue-drain thread (see CdpQueueDrainThread) polls far more often, and
// responses here aren't id-correlated — without this, one thread's recv
// could read back a *different* thread's in-flight response.
static std::mutex g_cdpMx;

// The injector (Forge) writes the actual port into the registry right
// before LoadLibraryW — it may have had to pick a fallback if YM_CDP_PORT
// was already taken by something else.
static int CdpPort() {
    DWORD v = RegGetDW(HKEY_CURRENT_USER, REG_APP, L"CdpPort", 0);
    return v ? (int)v : YM_CDP_PORT;
}

static std::string CdpUtf8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// %TEMP% gets wiped by disk-cleanup tools and cycles per-session on some
// setups — moved logs/dumps to a stable, dedicated folder so they're
// still there whenever this gets looked at later, possibly long after
// the crash that produced them.
static std::wstring PersistentLogDir() {
    wchar_t local[MAX_PATH];
    if (SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local) != S_OK) {
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        return std::wstring(tmp) + L"YMHub";
    }
    return std::wstring(local) + L"\\YMHub";
}
static void EnsurePersistentLogDir() {
    CreateDirectoryW(PersistentLogDir().c_str(), nullptr);
}

// ── In-memory log, surfaced as a row in YM's own Settings page (see
// LogBadgeThread / CdpInjectSettingsLogRow) so the user can see what the
// DLL is doing without attaching a debugger. Also mirrored to a file so
// logs survive across injections and are there to look at after a crash.
static std::mutex g_logMx;
static std::vector<std::string> g_log;
static void LogMsg(const std::string& s) {
    SYSTEMTIME t; GetLocalTime(&t);
    char ts[16]; sprintf_s(ts, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    std::string line = ts + s;
    {
        std::lock_guard<std::mutex> lk(g_logMx);
        g_log.push_back(line);
        if (g_log.size() > 300) g_log.erase(g_log.begin());
    }
    EnsurePersistentLogDir();
    std::wstring path = PersistentLogDir() + L"\\YMHubDll.log";
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SetFilePointer(f, 0, nullptr, FILE_END);
    std::string l = line + "\r\n"; DWORD w = 0;
    WriteFile(f, l.data(), (DWORD)l.size(), &w, nullptr);
    CloseHandle(f);
}
static std::string LogBlob() {
    std::lock_guard<std::mutex> lk(g_logMx);
    std::string r;
    for (auto& l : g_log) { r += l; r += "\n"; }
    return r;
}

// ── Crash handler ────────────────────────────────────────────────
// The minimize/focus-loss crash (see CdpUnsafeNow's own comment) hits
// Windows' "unhandled exception during a user callback" fast-fail path
// — confirmed live via System Informer's own "Во время обратного вызова
// пользователя обнаружено необработанное исключение" message, which is
// the exact wording for this specific failure mode. By default
// (PROCESS_CALLBACK_FILTER_ENABLED) that path skips normal SEH
// propagation entirely, so nothing in this process — including the
// handler below — ever gets a chance to see the exception. Clearing the
// flag is a documented Microsoft workaround (originally KB976038) that
// restores ordinary exception propagation for this scenario, which is
// what lets a real handler run instead of an instant, silent kill.
static void DisableCallbackExceptionFiltering() {
    typedef BOOL(WINAPI* GetPolicyFn)(LPDWORD);
    typedef BOOL(WINAPI* SetPolicyFn)(DWORD);
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    if (!k32) return;
    auto getPolicy = (GetPolicyFn)GetProcAddress(k32, "GetProcessUserModeExceptionPolicy");
    auto setPolicy = (SetPolicyFn)GetProcAddress(k32, "SetProcessUserModeExceptionPolicy");
    if (!getPolicy || !setPolicy) return;
    DWORD flags = 0;
    if (getPolicy(&flags)) {
        const DWORD PROCESS_CALLBACK_FILTER_ENABLED = 0x1;
        setPolicy(flags & ~PROCESS_CALLBACK_FILTER_ENABLED);
    }
}

// This crash (see CdpUnsafeNow's own comment) is a *deliberate* int3/
// CHECK-failure breakpoint inside Chromium's own code, not memory
// corruption — confirmed via WinDbg on a dump this same handler wrote.
// Chromium is intentionally aborting because it detected a broken
// internal invariant; nothing here can make the process continue
// afterward, only show what happened before it goes down. Modeled after
// the reference screenshot the user provided (a Neverlose-style crash
// dialog) — same shape, own wording, and no claim of auto-submitting a
// report anywhere, since there's no server behind this to send it to.
static void ShowCrashDialog(DWORD code, void* addr, const std::wstring& dumpPath, const std::wstring& logPath) {
    wchar_t content[2048];
    swprintf(content, 2048,
        L"Обнаружена критическая внутренняя ошибка движка Яндекс Музыки, из-за которой приложение пришлось закрыть.\n\n"
        L"Возможные причины:\n"
        L"1. Внутренний сбой встроенного Chromium-движка Яндекс Музыки.\n"
        L"2. Конфликт с другим ПО, работающим поверх Яндекс Музыки.\n"
        L"3. Повреждённые файлы приложения.\n\n"
        L"Рекомендуемые действия:\n"
        L"1. Запустите Яндекс Музыку через Forge ещё раз.\n"
        L"2. Если ошибка повторяется — приложите файлы лога и дампа ниже при обращении в поддержку.\n\n"
        L"Техническая информация:\n"
        L"Версия YMHub: %s\n"
        L"Код ошибки: 0x%08X\n"
        L"Адрес: 0x%p\n"
        L"Лог: %s\n"
        L"Дамп: %s",
        YMHUB_VERSION_W, code, addr, logPath.c_str(), dumpPath.c_str());

    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_SIZE_TO_CONTENT;
    cfg.pszWindowTitle = L"YMHub";
    cfg.pszMainIcon = TD_ERROR_ICON;
    cfg.pszMainInstruction = L"Яндекс Музыка неожиданно закрылась";
    cfg.pszContent = content;
    cfg.pszFooter = L"Лог и дамп сохранены только локально — никуда не отправляются автоматически.";
    cfg.dwCommonButtons = TDCBF_OK_BUTTON;
    cfg.nDefaultButton = IDOK;
    TaskDialogIndirect(&cfg, nullptr, nullptr, nullptr);
}

// Last-resort handler: writes a readable summary to the persistent log,
// a full .dmp next to it (same shape WER already produces for these,
// just guaranteed to be sitting right next to the readable log instead
// of buried in %LOCALAPPDATA%\CrashDumps), shows the dialog above, then
// exits deliberately — letting EXCEPTION_CONTINUE_SEARCH run afterward
// would hand this same exception to WER too, stacking a second, generic
// "Яндекс Музыка has stopped working" dialog on top of ours.
static std::mutex g_crashMx;
static LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    std::lock_guard<std::mutex> lk(g_crashMx);
    EnsurePersistentLogDir();
    std::wstring dir = PersistentLogDir();

    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t stamp[32];
    swprintf(stamp, 32, L"%04d%02d%02d_%02d%02d%02d",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    DWORD code = ep->ExceptionRecord->ExceptionCode;
    void* addr = ep->ExceptionRecord->ExceptionAddress;
    char buf[256];
    sprintf_s(buf, "CRASH code=0x%08X addr=%p pid=%lu tid=%lu",
        code, addr, GetCurrentProcessId(), GetCurrentThreadId());
    LogMsg(buf);

    std::wstring logPath = dir + L"\\YMHubDll.log";
    std::wstring dumpPath = dir + L"\\crash_" + stamp + L".dmp";
    HANDLE hFile = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool dumpOk = false;
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        dumpOk = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
            (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory |
                MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules),
            &mei, nullptr, nullptr) != FALSE;
        CloseHandle(hFile);
        LogMsg(dumpOk ? ("CRASH dump written: " + CdpUtf8(dumpPath.c_str()))
                      : "CRASH MiniDumpWriteDump FAILED");
    } else {
        LogMsg("CRASH dump CreateFile FAILED");
    }

    ShowCrashDialog(code, addr, dumpOk ? dumpPath : L"(не удалось записать)", logPath);
    ExitProcess((UINT)code);
}
static void InstallCrashHandler() {
    DisableCallbackExceptionFiltering();
    SetUnhandledExceptionFilter(CrashHandler);
}

// Escapes text for embedding inside a single-quoted JS string literal
// (the log content itself, as opposed to CdpJsonEscape which escapes the
// JS *source* for the outer JSON request).
static std::string JsStrEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
        case '\'': r += "\\'"; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n"; break;
        case '\r': break;
        default: r += (char)c;
        }
    }
    return r;
}

static std::string CdpJsonEscape(const std::string& s) {
    std::string r;
    for (unsigned char c : s) {
        switch (c) {
        case '"':  r += "\\\""; break;
        case '\\': r += "\\\\"; break;
        case '\n': r += "\\n";  break;
        case '\r': r += "\\r";  break;
        default:
            if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); r += b; }
            else r += (char)c;
        }
    }
    return r;
}

static bool CdpHttpGet(const wchar_t* path, std::string& outBody) {
    HINTERNET hSession = WinHttpOpen(L"YMHub", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 1000, 1000, 1000, 1000);
    bool ok = false;
    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", (INTERNET_PORT)CdpPort(), 0);
    if (hConnect) {
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", path, nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hReq) {
            if (WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(hReq, nullptr)) {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
                    std::string chunk(avail, 0);
                    DWORD read = 0;
                    if (!WinHttpReadData(hReq, chunk.data(), avail, &read)) break;
                    outBody.append(chunk.data(), read);
                }
                ok = true;
            }
            WinHttpCloseHandle(hReq);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return ok;
}

static std::wstring CdpFindWsPath() {
    std::string body;
    if (!CdpHttpGet(L"/json/list", body)) return L"";
    auto p = body.find("music-application");
    if (p == std::string::npos) return L"";
    auto wsKey = body.find("webSocketDebuggerUrl", p);
    if (wsKey == std::string::npos) return L"";
    auto q1 = body.find('"', wsKey + 22);
    auto q2 = (q1 != std::string::npos) ? body.find('"', q1 + 1) : std::string::npos;
    if (q1 == std::string::npos || q2 == std::string::npos) return L"";
    std::string url = body.substr(q1 + 1, q2 - q1 - 1);
    std::string marker = "/devtools";
    auto dp = url.find(marker);
    if (dp == std::string::npos) return L"";
    std::string path = url.substr(dp);
    return std::wstring(path.begin(), path.end());
}

// No WinHttpWebSocketClose handshake here on purpose — it performs a
// synchronous close exchange that has been observed to block far longer
// than expected (same class of WinHTTP-timeout bug seen elsewhere in this
// codebase), hanging this entire thread forever on teardown. We're either
// tearing down or about to reconnect, so just drop the handle.
static void CdpClose() {
    if (g_cdpWs) { WinHttpCloseHandle(g_cdpWs); g_cdpWs = nullptr; }
    if (g_cdpConnect) { WinHttpCloseHandle(g_cdpConnect); g_cdpConnect = nullptr; }
    if (g_cdpSession) { WinHttpCloseHandle(g_cdpSession); g_cdpSession = nullptr; }
}

// For one-shot CDP work (CdpAnnounceThread) where skipping isn't an
// option the way it is for the polling loops — block briefly instead
// until the window's not mid-transition, actively dropping any live
// session while waiting rather than just declining to poll it.
static void WaitUntilSafeForCdp() {
    for (;;) {
        HWND main = FindMainWnd();
        if (!main || !CdpUnsafeNow(main)) return;
        EnsureTransitionWatch();
        {
            std::lock_guard<std::mutex> lk(g_cdpMx);
            CdpClose();
        }
        Sleep(100);
    }
}

static bool CdpEnsureConnected() {
    if (g_cdpWs) return true;
    std::wstring path = CdpFindWsPath();
    if (path.empty()) return false;

    g_cdpSession = WinHttpOpen(L"YMHub", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!g_cdpSession) return false;
    WinHttpSetTimeouts(g_cdpSession, 1000, 1000, 1000, 2000);
    g_cdpConnect = WinHttpConnect(g_cdpSession, L"127.0.0.1", (INTERNET_PORT)CdpPort(), 0);
    if (!g_cdpConnect) { CdpClose(); return false; }
    HINTERNET hReq = WinHttpOpenRequest(g_cdpConnect, L"GET", path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!hReq) { CdpClose(); return false; }
    bool ok = WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
        WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(hReq, nullptr);
    if (ok) g_cdpWs = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    WinHttpCloseHandle(hReq);
    if (!g_cdpWs) { CdpClose(); return false; }
    return true;
}

static bool CdpSend(const std::string& msg) {
    if (!g_cdpWs) return false;
    return WinHttpWebSocketSend(g_cdpWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        (PVOID)msg.data(), (DWORD)msg.size()) == NO_ERROR;
}

static bool CdpRecv(std::string& out) {
    if (!g_cdpWs) return false;
    char buf[16384]; DWORD read = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
    if (WinHttpWebSocketReceive(g_cdpWs, buf, sizeof(buf), &read, &bt) != NO_ERROR) return false;
    out.assign(buf, read);
    return true;
}

// Fire-and-forget JS eval — used for the connect-confirmation banner.
static void CdpRunJs(const std::string& jsUtf8) {
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return;
    std::string req = "{\"id\":9,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(jsUtf8) + "\"}}";
    if (!CdpSend(req)) { CdpClose(); return; }
    std::string resp; CdpRecv(resp);
}

// ── Compact step-counter connect toast, injected into YM's page ──
// A small non-blocking corner card (icon badge + title/status + "N of M"
// counter + a thin accent glow) instead of the old full-screen dimmed
// modal — walks the real connect sequence (CDP connect -> player-ready
// poll) as honest, distinct steps rather than one generic "checking"
// message, then settles into a success/error state and auto-dismisses.
// Built via plain DOM calls (no innerHTML/backticks) so the only quoting
// to worry about is the outer JSON-escape.
static void CdpInitToastEnsure() {
    std::wstring js =
        L"(function(){if(document.getElementById('ymhub-status'))return;"
        L"var o=document.createElement('div');o.id='ymhub-status';"
        L"o.style.cssText='position:fixed;top:20px;right:20px;z-index:999999;"
        L"opacity:0;transform:translateY(-8px);"
        L"transition:opacity .3s ease,transform .3s ease;pointer-events:none;';"
        L"var card=document.createElement('div');card.id='ymhub-status-card';"
        L"card.style.cssText='display:flex;align-items:center;gap:12px;"
        L"background:rgba(13,13,20,.88);backdrop-filter:blur(16px);"
        L"border:1px solid rgba(91,143,255,.35);border-radius:14px;padding:12px 16px;"
        L"box-shadow:0 0 0 1px rgba(91,143,255,.08),0 8px 28px rgba(0,0,0,.45),"
        L"0 0 24px rgba(91,143,255,.18);min-width:230px;"
        L"transition:border-color .3s ease,box-shadow .3s ease;';"
        L"var badge=document.createElement('div');badge.id='ymhub-status-badge';"
        L"badge.style.cssText='width:32px;height:32px;flex-shrink:0;border-radius:9px;"
        L"background:#5B8FFF;display:flex;align-items:center;justify-content:center;"
        L"font-size:15px;color:#fff;transition:background .3s ease;';"
        L"badge.textContent='\\u266A';"
        L"var body=document.createElement('div');body.style.cssText='flex:1;min-width:0;';"
        L"var row=document.createElement('div');"
        L"row.style.cssText='display:flex;align-items:baseline;justify-content:space-between;gap:8px;';"
        L"var title=document.createElement('div');title.id='ymhub-title';"
        L"title.textContent='YMHub';"
        L"title.style.cssText='font:600 13.5px \"Segoe UI Variable Text\",\"Segoe UI\",sans-serif;"
        L"color:rgba(255,255,255,.92);';"
        L"var step=document.createElement('div');step.id='ymhub-step';"
        L"step.style.cssText='font:500 11px \"Segoe UI Variable Text\",\"Segoe UI\",sans-serif;"
        L"color:rgba(255,255,255,.4);white-space:nowrap;';"
        L"row.appendChild(title);row.appendChild(step);"
        L"var sub=document.createElement('div');sub.id='ymhub-sub';"
        L"sub.style.cssText='font:400 12px \"Segoe UI Variable Text\",\"Segoe UI\",sans-serif;"
        L"color:rgba(255,255,255,.55);margin-top:2px;transition:color .3s ease;"
        L"overflow:hidden;text-overflow:ellipsis;white-space:nowrap;';"
        L"body.appendChild(row);body.appendChild(sub);"
        L"card.appendChild(badge);card.appendChild(body);"
        L"o.appendChild(card);document.body.appendChild(o);"
        L"requestAnimationFrame(function(){o.style.opacity='1';o.style.transform='translateY(0)';});})()";
    CdpRunJs(CdpUtf8(js.c_str()));
}

static void CdpInitToastStep(int step, int total, const wchar_t* label) {
    CdpInitToastEnsure();
    std::wstring js =
        L"(function(){var sub=document.getElementById('ymhub-sub');if(sub)sub.textContent='";
    js += label;
    js += L"';var st=document.getElementById('ymhub-step');if(st)st.textContent='";
    js += std::to_wstring(step) + L" \\u0438\\u0437 " + std::to_wstring(total);
    js += L"';})()";
    CdpRunJs(CdpUtf8(js.c_str()));
}

static void CdpInitToastDone(bool ok, const wchar_t* message) {
    std::wstring js =
        L"(function(){var badge=document.getElementById('ymhub-status-badge');"
        L"var card=document.getElementById('ymhub-status-card');"
        L"var sub=document.getElementById('ymhub-sub');var st=document.getElementById('ymhub-step');"
        L"if(st)st.remove();";
    if (ok) {
        js +=
            L"if(badge)badge.style.background='#3DD68C';"
            L"if(card)card.style.borderColor='rgba(61,214,140,.35)';"
            L"if(sub){sub.style.color='rgba(255,255,255,.7)';sub.textContent='";
        js += message;
        js += L"';}";
    } else {
        js +=
            L"if(badge){badge.style.background='#FF5B6E';badge.textContent='!';}"
            L"if(card)card.style.borderColor='rgba(255,91,110,.4)';"
            L"if(sub){sub.style.color='#FF9AA6';sub.textContent='";
        js += message;
        js += L"';}";
    }
    js += L"setTimeout(function(){var o=document.getElementById('ymhub-status');"
        L"if(o){o.style.opacity='0';o.style.transform='translateY(-8px);';"
        L"setTimeout(function(){o.remove();},350);}},";
    js += ok ? L"1800);})()" : L"4500);})()";
    CdpRunJs(CdpUtf8(js.c_str()));
}

// Inserts a real row into YM's own Settings list (cloned from the "О
// приложении" row so it matches YM's current visual style exactly, instead
// of guessing at colors/spacing), right below it. The row only exists while
// the Settings page is actually mounted — React unmounts the whole list
// when leaving Settings, so this re-inserts itself (idempotent, checked by
// id) every poll tick rather than once. Clicking it opens an M3-styled
// modal with the DLL's recent log lines, refreshed on every tick so it
// stays current even while open.
static void CdpInjectSettingsLogRow(const std::string& logBlobUtf8) {
    std::string logJs = JsStrEscape(logBlobUtf8);
    std::string js =
        "(function(){"
        "var LOG='" + logJs + "';"
        "var modal=document.getElementById('ymhub-log-modal');"
        "if(!modal){"
        "modal=document.createElement('div');modal.id='ymhub-log-modal';"
        "modal.style.cssText='position:fixed;inset:0;z-index:999999;display:none;"
        "align-items:center;justify-content:center;background:rgba(0,0,0,.55);';"
        "var card=document.createElement('div');"
        "card.style.cssText='width:480px;max-height:75vh;display:flex;flex-direction:column;"
        "background:#1b1b1f;border-radius:24px;padding:24px;box-shadow:0 12px 40px rgba(0,0,0,.5);';"
        "var head=document.createElement('div');"
        "head.style.cssText='display:flex;align-items:center;justify-content:space-between;margin-bottom:18px;';"
        "var title=document.createElement('div');title.textContent='Логи YMHub';"
        "title.style.cssText='font:700 22px system-ui,sans-serif;color:#fff;';"
        "var closeBtn=document.createElement('button');closeBtn.innerHTML="
        "'<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\">"
        "<path d=\"M5 5L19 19M19 5L5 19\" stroke=\"currentColor\" stroke-width=\"2.2\" stroke-linecap=\"round\"/></svg>';"
        "closeBtn.style.cssText='width:34px;height:34px;border-radius:50%;border:none;"
        "background:rgba(255,255,255,.08);color:#fff;display:flex;align-items:center;justify-content:center;"
        "cursor:pointer;flex-shrink:0;';"
        "closeBtn.onclick=function(){modal.style.display='none';};"
        "head.appendChild(title);head.appendChild(closeBtn);"
        "var copyBtn=document.createElement('button');copyBtn.textContent='\\u0421\\u043a\\u043e\\u043f\\u0438\\u0440\\u043e\\u0432\\u0430\\u0442\\u044c';"
        "copyBtn.style.cssText='align-self:flex-start;margin-bottom:12px;height:30px;padding:0 14px;"
        "border-radius:9px;border:none;background:rgba(255,255,255,.08);color:#C9C4D0;"
        "font:600 12.5px system-ui,sans-serif;cursor:pointer;';"
        "var pre=document.createElement('pre');pre.id='ymhub-log-text';"
        "pre.style.cssText='flex:1;overflow:auto;margin:0;white-space:pre-wrap;word-break:break-word;"
        "font:11px Consolas,monospace;color:#a8a8b3;background:#0f0f12;border-radius:14px;padding:14px;';"
        "copyBtn.onclick=function(){navigator.clipboard.writeText(pre.textContent).catch(function(){});};"
        "card.appendChild(head);card.appendChild(copyBtn);card.appendChild(pre);modal.appendChild(card);"
        "modal.onclick=function(e){if(e.target===modal)modal.style.display='none';};"
        "document.body.appendChild(modal);"
        "}"
        "var pre=document.getElementById('ymhub-log-text');"
        "pre.textContent=LOG||'(пока нет записей)';"
        "if(modal.style.display!=='none')pre.scrollTop=pre.scrollHeight;"
        "if(document.getElementById('ymhub-settings-row'))return;"
        "var titleEl=[...document.querySelectorAll('[class*=SettingsListButtonItem_title]')]"
        ".find(function(e){return e.textContent.trim()==='\\u041e \\u043f\\u0440\\u0438\\u043b\\u043e\\u0436\\u0435\\u043d\\u0438\\u0438';});"
        "if(!titleEl)return;"
        "var origRow=titleEl.closest('button');if(!origRow)return;"
        "var row=origRow.cloneNode(true);row.id='ymhub-settings-row';"
        "row.style.marginTop='28px';"
        "var t=row.querySelector('[class*=SettingsListButtonItem_title]');"
        "if(t)t.textContent='Логи YMHub';"
        "var d=row.querySelector('[class*=SettingsListButtonItem_description]');"
        "if(d)d.textContent='\\u0414\\u0438\\u0430\\u0433\\u043d\\u043e\\u0441\\u0442\\u0438\\u043a\\u0430 \\u0438 \\u0436\\u0443\\u0440\\u043d\\u0430\\u043b \\u0441\\u043e\\u0431\\u044b\\u0442\\u0438\\u0439';"
        "row.onclick=function(e){e.preventDefault();e.stopPropagation();"
        "modal.style.display='flex';pre.scrollTop=pre.scrollHeight;};"
        "origRow.insertAdjacentElement('afterend',row);"
        "})()";
    CdpRunJs(js);
}

// Confirms the actual app UI is present (not just that CDP is reachable)
// before declaring success. Originally checked only the like/dislike
// button (in either of its two states), but that button doesn't exist at
// all until a track is actually loaded into the player bar — if YM opened
// without anything auto-resuming, there's nothing to find and this falsely
// reported "player not found" despite everything working fine. Now also
// accepts the search nav button, which is part of the app shell and is
// present regardless of playback state. Retries for a few seconds since
// the page can still be hydrating right after a fresh launch.
//
// A single transient CdpSend/CdpRecv failure (e.g. the websocket isn't
// fully settled yet right after the upgrade) used to abort the whole check
// immediately instead of retrying like the loop implies — that was the
// other half of the false "player not found" reports. Now a failed
// send/receive just closes and reconnects on the next iteration instead of
// giving up outright.
static bool CdpVerifyPlayerReady() {
    for (int i = 0; i < 20; i++) {
        if (i > 0) Sleep(500);
        if (!CdpEnsureConnected()) continue;
        std::string req =
            "{\"id\":5,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":"
            "\"!!(document.querySelector(\\\"button[aria-label='\\u041d\\u0440\\u0430\\u0432\\u0438\\u0442\\u0441\\u044f']\\\")"
            "||document.querySelector(\\\"button[aria-label='\\u041d\\u0435 \\u043d\\u0440\\u0430\\u0432\\u0438\\u0442\\u0441\\u044f']\\\")"
            "||document.querySelector(\\\"button[aria-label='\\u041f\\u043e\\u0438\\u0441\\u043a']\\\"))\","
            "\"returnByValue\":true}}";
        if (!CdpSend(req)) { CdpClose(); continue; }
        std::string resp;
        if (!CdpRecv(resp)) { CdpClose(); continue; }
        if (resp.find("\"value\":true") != std::string::npos) return true;
    }
    return false;
}

static bool CdpExtractXY(const std::string& resp, double& x, double& y) {
    auto xp = resp.find("\"x\":");
    auto yp = resp.find("\"y\":");
    if (xp == std::string::npos || yp == std::string::npos) return false;
    x = atof(resp.c_str() + xp + 4);
    y = atof(resp.c_str() + yp + 4);
    return true;
}

// Polled from LogBadgeThread to keep the hub/mini-player heart icon in
// sync with the real liked state — covers likes done from YM's own UI,
// remapped native hotkeys, or a track that was already liked before the
// hub started, none of which go through DoLike()'s optimistic toggle.
// aria-pressed on the main player's "Нравится" button is the one place
// this is reliably exposed (confirmed empirically: false -> true the
// instant a like lands, same button identity as CdpClickButton above).
static bool CdpQueryLiked() {
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return false;
    std::string label = CdpUtf8(L"Нравится");
    std::string expr =
        "(function(){var b=document.querySelector(\"button[aria-label='" + label +
        "']\");return b?b.getAttribute('aria-pressed')==='true':false;})()";
    std::string req = "{\"id\":6,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(expr) + "\",\"returnByValue\":true}}";
    if (!CdpSend(req)) { CdpClose(); return false; }
    std::string resp;
    if (!CdpRecv(resp)) { CdpClose(); return false; }
    return resp.find("\"value\":true") != std::string::npos;
}

// Polled alongside CdpQueryLiked to feed Discord Rich Presence (see
// DiscordTick in main.cpp) a per-track cover: the player bar's <img> src
// is already a public Yandex CDN URL (avatars.yandex.net/...), so it can
// go straight into Discord's large_image as an external asset — no need
// to host or proxy the image ourselves. Same selector as TWEAK_BIG_COVER.
// CDN URLs are plain ASCII, so a byte-widening narrow->wide conversion
// (same shortcut CdpFindWsPath already uses) is lossless here.
static std::wstring CdpQueryCoverUrl() {
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return L"";
    std::string expr =
        "(function(){var el=document.querySelector(\"[class*='AlbumCover_cover__']\");"
        "return el&&el.src?el.src:'';})()";
    std::string req = "{\"id\":8,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(expr) + "\",\"returnByValue\":true}}";
    if (!CdpSend(req)) { CdpClose(); return L""; }
    std::string resp;
    if (!CdpRecv(resp)) { CdpClose(); return L""; }
    auto p = resp.find("\"value\":\"");
    if (p == std::string::npos) return L"";
    p += 9;
    auto e = resp.find('"', p);
    if (e == std::string::npos) return L"";
    std::string url = resp.substr(p, e - p);
    return std::wstring(url.begin(), url.end());
}

static void CdpClickButton(const wchar_t* ariaLabel) {
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return;
    std::string label = CdpUtf8(ariaLabel); // plain text, no special chars to escape
    // Real JS source with real double/single quotes — escaped exactly once
    // below when embedding it as the outer JSON request's string value.
    std::string expr =
        "(function(){var b=document.querySelector(\"button[aria-label='" + label +
        "']\");if(!b)return null;var r=b.getBoundingClientRect();"
        "return {x:r.x+r.width/2,y:r.y+r.height/2};})()";
    std::string req1 = "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(expr) + "\",\"returnByValue\":true}}";
    if (!CdpSend(req1)) { CdpClose(); return; }
    std::string resp;
    if (!CdpRecv(resp)) { CdpClose(); return; }
    double x = 0, y = 0;
    if (!CdpExtractXY(resp, x, y)) return;

    char buf[256];
    sprintf_s(buf, "{\"id\":2,\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"mousePressed\",\"x\":%.2f,\"y\":%.2f,\"button\":\"left\",\"clickCount\":1}}", x, y);
    CdpSend(buf); CdpRecv(resp);
    sprintf_s(buf, "{\"id\":3,\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"mouseReleased\",\"x\":%.2f,\"y\":%.2f,\"button\":\"left\",\"clickCount\":1}}", x, y);
    CdpSend(buf); CdpRecv(resp);
}

// Like CdpClickButton, but scoped to the current player bar (PLAYERBAR_
// DESKTOP or VIBE_PLAYERBAR) and addressed by data-test-id rather than
// aria-label — used for shuffle/repeat from the cheat menu's Advanced
// section, where the unscoped aria-label lookup risks the same list-page
// ambiguity already documented for CdpClickButton/CdpQueryLiked. Resolves
// to null (no-op, no click sent) if the button doesn't exist or is itself
// disabled — YM disables SHUFFLE_BUTTON in some queue contexts, confirmed
// live, and a disabled native button simply ignores clicks too, so this
// just mirrors that instead of silently doing nothing for a worse reason.
static void CdpClickScoped(const char* dataTestIdPrefix) {
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return;
    std::string expr =
        "(function(){var pb=document.querySelector(\"[data-test-id='PLAYERBAR_DESKTOP']\")||"
        "document.querySelector(\"[data-test-id='VIBE_PLAYERBAR']\");if(!pb)return null;"
        "var b=pb.querySelector(\"[data-test-id^='" + std::string(dataTestIdPrefix) + "']\");"
        "if(!b||b.disabled)return null;var r=b.getBoundingClientRect();"
        "return {x:r.x+r.width/2,y:r.y+r.height/2};})()";
    std::string req1 = "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(expr) + "\",\"returnByValue\":true}}";
    if (!CdpSend(req1)) { CdpClose(); return; }
    std::string resp;
    if (!CdpRecv(resp)) { CdpClose(); return; }
    double x = 0, y = 0;
    if (!CdpExtractXY(resp, x, y)) return;

    char buf[256];
    sprintf_s(buf, "{\"id\":2,\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"mousePressed\",\"x\":%.2f,\"y\":%.2f,\"button\":\"left\",\"clickCount\":1}}", x, y);
    CdpSend(buf); CdpRecv(resp);
    sprintf_s(buf, "{\"id\":3,\"method\":\"Input.dispatchMouseEvent\",\"params\":{\"type\":\"mouseReleased\",\"x\":%.2f,\"y\":%.2f,\"button\":\"left\",\"clickCount\":1}}", x, y);
    CdpSend(buf); CdpRecv(resp);
}

// Default key for each YM_ACTION_* (see shared/ipc.h) — used to emit the
// *original* key via CDP when the host's hook detects the user's remapped
// key. Input.dispatchKeyEvent is trusted input as far as Chromium/Electron
// is concerned (same as the Input.dispatchMouseEvent calls above, already
// relied on for like/dislike), unlike page-JS-dispatched events or
// SendInput, both of which were tried first and silently ignored.
struct YmDefKey { const char* key; const char* code; int vk; };
static const YmDefKey kYmDefKeys[13] = {
    {"k","KeyK",75}, {"m","KeyM",77}, {"l","KeyL",76}, {"j","KeyJ",74},
    {"ArrowUp","ArrowUp",38}, {"ArrowDown","ArrowDown",40},
    {"f","KeyF",70}, {"d","KeyD",68}, {"r","KeyR",82}, {"s","KeyS",83},
    {"n","KeyN",78}, {"p","KeyP",80}, {"w","KeyW",87},
};
static void CdpSendYmKey(DWORD idx) {
    if (idx >= 13) return;
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return;
    const YmDefKey& d = kYmDefKeys[idx];
    char buf[384]; std::string resp;
    sprintf_s(buf,
        "{\"id\":4,\"method\":\"Input.dispatchKeyEvent\",\"params\":{\"type\":\"keyDown\","
        "\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d}}",
        d.key, d.code, d.vk, d.vk);
    if (!CdpSend(buf)) { CdpClose(); return; }
    CdpRecv(resp);
    sprintf_s(buf,
        "{\"id\":4,\"method\":\"Input.dispatchKeyEvent\",\"params\":{\"type\":\"keyUp\","
        "\"key\":\"%s\",\"code\":\"%s\",\"windowsVirtualKeyCode\":%d,\"nativeVirtualKeyCode\":%d}}",
        d.key, d.code, d.vk, d.vk);
    CdpSend(buf); CdpRecv(resp);
}

// "Твики" hub tab — bit i in tweaksMask injects kTweakRules[i] verbatim
// (see TWEAK_* in shared/ipc.h). Each entry is a complete CSS rule (or
// several), not just a bare selector, so most are a plain hide
// ("{display:none!important}") but a tweak can just as well resize or
// restyle something instead. Injected via a single persistent <style>
// tag rebuilt from the current mask — cheap and idempotent, so it's safe
// to re-run on every LogBadgeThread tick as well as immediately on toggle
// (WorkerThread's tweaksSeq watch), which keeps it self-healing across
// YM's own SPA re-renders.
static const char* kTweakRules[8] = {
    "[class*='VibePage_words__']{display:none!important;}",      // AI-комментарии о треке
    "[data-test-id='VIBE_ANIMATION']{display:none!important;}",  // анимация фона плеера
    "[class*='MainPage_betaSlot__']{display:none!important;}",   // плашка "версия приложения"
    "[class*='VibePage_wheel__']{display:none!important;}",      // барабан рекомендаций слева — VibePage_root
                                         // is a flex row with the player block as
                                         // the other child (already centered within
                                         // itself), so hiding this also re-centers
                                         // the player for free via flex redistribution
    "[class*='MainPage_feedbackForm__']{display:none!important;}", // плашка "Моя волна обновилась"
    "[data-test-id='NAVBAR_NAVIGATION_ITEM_FOR_YOU_AND_TRENDS'],"
    "[data-test-id='NAVBAR_NAVIGATION_ITEM_CONCERTS'],"
    "[data-test-id='NAVBAR_NAVIGATION_ITEM_NON_MUSIC']{display:none!important;}", // лишние разделы меню
    "[data-test-id='USER_PROFILE_PLUS_LINK'],"
    "[data-test-id='USER_PROFILE_PLUS_BADGE']{display:none!important;}", // плюс-бейдж в профиле
    // крупная обложка трека — coverContainer/link/img are all exactly
    // 80x80 and sized in lockstep (the link is position:absolute;inset:0
    // inside coverContainer, the img fills the link), so growing all
    // three together to the same size needs no overflow/clipping fixes.
    // The parent column (VibePlayerBar_root__) is flex+align-items:center
    // with the cover as its first child, so a taller cover just pushes
    // the progress bar/controls below it down — no overlap there. But
    // the cover sits in a layout slot (playerBlock) whose own box height
    // doesn't grow with it, so a bigger cover bleeds upward out of that
    // slot and starts overlapping the artist-name block above it —
    // nudge that block up a bit so it clears the taller cover.
    "[class*='AlbumCover_coverContainer__'],"
    "[class*='AlbumCover_link__'],"
    "[class*='AlbumCover_cover__']{width:152px!important;height:152px!important;}"
    "[class*='AlbumCover_cover__']{border-radius:16px!important;}"
    "[class*='VibePage_entityMetaBody__']{transform:translateY(-24px)!important;}",
};
// customCssW is arbitrary user-typed text (the "Свой CSS" box), unlike
// kTweakRules' own developer-written entries, so the combined string can
// no longer go into a backtick template literal as-is (a stray backtick
// or ${...} in the user's CSS would break the generated JS). Escaped into
// a normal quoted JS string instead — safe for any input.
static void CdpApplyTweaks(DWORD mask, const wchar_t* customCssW) {
    if (!CdpEnsureConnected()) return;
    std::string css;
    for (int i = 0; i < 8; i++) {
        if (mask & (1u << i)) css += kTweakRules[i];
    }
    if (customCssW && customCssW[0]) css += CdpUtf8(customCssW);
    std::string esc; esc.reserve(css.size());
    for (unsigned char c : css) {
        if (c == '\\') esc += "\\\\";
        else if (c == '"') esc += "\\\"";
        else if (c == '\n') esc += "\\n";
        else if (c == '\r') continue;
        else esc += (char)c;
    }
    std::string js =
        "(function(){var s=document.getElementById('ymhub-tweaks-style');"
        "if(!s){s=document.createElement('style');s.id='ymhub-tweaks-style';document.head.appendChild(s);}"
        "s.textContent=\"" + esc + "\";})()";
    CdpRunJs(js);
}

// TWEAK_HIDE_NAME — unlike kTweakRules above, this needs the actual name
// text overwritten (CSS can't substitute arbitrary user-provided text),
// so it's a direct DOM mutation instead of a stylesheet rule. The real
// name is stashed in a data attribute the first time it's hidden so
// turning the tweak back off can restore it exactly, instead of leaving
// it stuck on whatever replacement text was last shown.
//
// Re-running this from scratch only happens every ~2s (LogBadgeThread) or
// on toggle (WorkerThread's tweaksSeq watch), but switching between YM's
// own sections (Моя волна / Поиск / Коллекция...) re-renders the sidebar
// and recreates this element with the real name — which was visible for
// up to that ~2s until the next poll caught it. A MutationObserver
// installed on the page itself reacts to that re-render immediately
// instead of waiting on our poll, so it's installed once (guarded by
// window.__ymhubNameObs) and re-points itself at whatever apply() closure
// (capturing the current replacement text/on-off state) this function
// last installed on window.__ymhubNameApply.
static void CdpApplyNameHide(bool on, const wchar_t* customNameW) {
    if (!CdpEnsureConnected()) return;
    std::string name = (customNameW && customNameW[0]) ? CdpUtf8(customNameW) : CdpUtf8(L"Скрыто");
    std::string esc; esc.reserve(name.size());
    for (unsigned char c : name) { // escape for the JS string literal below
        if (c == '"' || c == '\\') esc += '\\';
        esc += (char)c;
    }
    std::string js =
        "(function(){var rep=\"" + esc + "\";var on=" + std::string(on ? "true" : "false") + ";"
        "function apply(){"
        "var el=document.querySelector(\"[data-test-id='USER_PROFILE_USERNAME']\");"
        "if(!el)return;"
        "if(on){"
        "if(!el.dataset.ymhubOrig)el.dataset.ymhubOrig=el.textContent;"
        "if(el.textContent!==rep)el.textContent=rep;"
        "}else if(el.dataset.ymhubOrig){"
        "el.textContent=el.dataset.ymhubOrig;delete el.dataset.ymhubOrig;"
        "}}"
        "window.__ymhubNameApply=apply;"
        "apply();"
        "if(!window.__ymhubNameObs){"
        "window.__ymhubNameObs=new MutationObserver(function(){if(window.__ymhubNameApply)window.__ymhubNameApply();});"
        "window.__ymhubNameObs.observe(document.body,{childList:true,subtree:true,characterData:true});"
        "}})()";
    CdpRunJs(js);
}

// The profile dropdown (clicking the avatar at the bottom of the sidebar —
// "Управление аккаунтом", phone/login, account switcher) is not part of the
// YM page at all: it's Yandex Passport's own account-switcher UI rendered
// as a genuinely cross-origin <iframe src="https://yandex.ru/user-id/...">.
// Same-origin policy means JS running in the main page (CdpApplyNameHide
// above, via the already-open g_cdpWs connection) cannot reach inside it —
// that's a browser security boundary, not a CDP limitation. CDP itself
// exposes the iframe as its own top-level debug target (visible in /json
// as type:"iframe", with its own webSocketDebuggerUrl) only while the
// dropdown is actually open, so this opens a short-lived second WebSocket
// straight to that target instead of trying to extend the main connection.
static std::wstring CdpFindPassportWsPath() {
    std::string body;
    if (!CdpHttpGet(L"/json", body)) return L"";
    auto p = body.find("https://yandex.ru/user-id");
    if (p == std::string::npos) return L""; // dropdown isn't open right now
    auto wsKey = body.find("webSocketDebuggerUrl", p);
    if (wsKey == std::string::npos) return L"";
    auto q1 = body.find('"', wsKey + 22);
    auto q2 = (q1 != std::string::npos) ? body.find('"', q1 + 1) : std::string::npos;
    if (q1 == std::string::npos || q2 == std::string::npos) return L"";
    std::string url = body.substr(q1 + 1, q2 - q1 - 1);
    auto dp = url.find("/devtools");
    if (dp == std::string::npos) return L"";
    std::string path = url.substr(dp);
    return std::wstring(path.begin(), path.end());
}

// One-shot connect/eval/close against an arbitrary CDP target path, kept
// fully separate from g_cdpWs (which stays dedicated to the main page) so
// this can't disturb that connection's state.
static void CdpRunJsOnPath(const std::wstring& wsPath, const std::string& jsUtf8) {
    HINTERNET hSession = WinHttpOpen(L"YMHub", WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return;
    WinHttpSetTimeouts(hSession, 1000, 1000, 1000, 2000);
    HINTERNET hConnect = WinHttpConnect(hSession, L"127.0.0.1", (INTERNET_PORT)CdpPort(), 0);
    if (hConnect) {
        HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", wsPath.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
        if (hReq) {
            bool ok = WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(hReq, nullptr);
            HINTERNET hWs = ok ? WinHttpWebSocketCompleteUpgrade(hReq, 0) : nullptr;
            WinHttpCloseHandle(hReq);
            if (hWs) {
                std::string req = "{\"id\":1,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
                    CdpJsonEscape(jsUtf8) + "\"}}";
                if (WinHttpWebSocketSend(hWs, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                        (PVOID)req.data(), (DWORD)req.size()) == NO_ERROR) {
                    char buf[16384]; DWORD read = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE bt;
                    WinHttpWebSocketReceive(hWs, buf, sizeof(buf), &read, &bt);
                }
                WinHttpCloseHandle(hWs);
            }
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
}

// Same replace/restore idea as CdpApplyNameHide, but reaching into the
// Passport iframe: the top profile card's name + masked phone/login line
// (stable-ish CSS-module classes, substring-matched the same way the rest
// of kTweakRules matches YM's own hashed classes), and every account-
// switcher row ([data-testid='user-item'], a stable attribute Passport
// itself uses) — those rows have no per-field class to target individual
// text, so all of a row's leaf text nodes are captured up front (so the
// node count/order needed to restore them is known before anything is
// mutated) and only the first non-blank one is overwritten with the
// replacement text, the rest blanked.
static void CdpApplyPassportNameHide(bool on, const wchar_t* customNameW) {
    std::wstring wsPath = CdpFindPassportWsPath();
    if (wsPath.empty()) return;
    std::string name = (customNameW && customNameW[0]) ? CdpUtf8(customNameW) : CdpUtf8(L"Скрыто");
    std::string esc; esc.reserve(name.size());
    for (unsigned char c : name) {
        if (c == '"' || c == '\\') esc += '\\';
        esc += (char)c;
    }
    std::string js =
        "(function(){var rep=\"" + esc + "\";var on=" + std::string(on ? "true" : "false") + ";"
        "function swap(el,blank){if(!el)return;"
        "if(on){if(!el.dataset.ymhubOrig)el.dataset.ymhubOrig=el.textContent;el.textContent=blank?'':rep;}"
        "else if(el.dataset.ymhubOrig){el.textContent=el.dataset.ymhubOrig;delete el.dataset.ymhubOrig;}}"
        "swap(document.querySelector(\"[class*='_title_1aljm_']\"),false);"
        "swap(document.querySelector(\"[class*='_caption_1aljm_']\"),true);"
        "document.querySelectorAll(\"[data-testid='user-item']\").forEach(function(item){"
        "if(on){if(!item.dataset.ymhubOrig){"
        "var orig=[];var w=document.createTreeWalker(item,NodeFilter.SHOW_TEXT);var n;"
        "while(n=w.nextNode())orig.push(n.nodeValue);"
        "item.dataset.ymhubOrig=JSON.stringify(orig);"
        "var w2=document.createTreeWalker(item,NodeFilter.SHOW_TEXT);var n2,first=true;"
        "while(n2=w2.nextNode()){if(n2.nodeValue.trim()){n2.nodeValue=first?rep:'';first=false;}}"
        "}}else if(item.dataset.ymhubOrig){"
        "var orig=JSON.parse(item.dataset.ymhubOrig);"
        "var w=document.createTreeWalker(item,NodeFilter.SHOW_TEXT);var n,i=0;"
        "while(n=w.nextNode()){if(i<orig.length){n.nodeValue=orig[i];i++;}}"
        "delete item.dataset.ymhubOrig;"
        "}});"
        "})()";
    CdpRunJsOnPath(wsPath, js);
}

// ── Command execution ───────────────────────────────────────────
static void ExecCmd(DWORD cmd) {
    if (cmd == YMHC_OVL_TOGGLE) {
        // ShowOverlay() touches WebView2 COM objects that are bound to
        // UiThread — ExecCmd itself runs on whatever thread triggered it
        // (HotkeyThread for a hotkey, WorkerThread for a hub/IPC command),
        // so this has to marshal over via the window's own message queue
        // rather than calling ShowOverlay() inline. OverlayWndProc's own
        // WM_APP+20 case (dispatched on UiThread) is what actually calls it.
        if (g_hwnd) PostMessageW(g_hwnd, WM_APP + 20, 0, 0);
        return;
    }

    // Like/dislike/prev/next via CDP — works regardless of OS focus. YM's
    // Electron main process doesn't actually act on WM_APPCOMMAND PREVIOUS/
    // NEXTTRACK (confirmed empirically — track never changed), unlike
    // PLAY_PAUSE which Windows also drives through SMTC independently of
    // our message, which is why only that one appeared to "work" before.
    if (cmd == YMHC_LIKE)    { LogMsg("Like clicked"); CdpClickButton(L"Нравится"); return; }
    if (cmd == YMHC_DISLIKE) { LogMsg("Dislike clicked"); CdpClickButton(L"Не нравится"); return; }
    if (cmd == YMHC_PREV)    { LogMsg("Prev clicked"); CdpClickButton(L"Предыдущая песня"); return; }
    if (cmd == YMHC_NEXT)    { LogMsg("Next clicked"); CdpClickButton(L"Следующая песня"); return; }

    HWND main = FindMainWnd();
    if (!main) return;

    // Media commands via WM_APPCOMMAND — no focus needed
    switch (cmd) {
    case YMHC_TOGGLE:
        PostMessageW(main, WM_APPCOMMAND, (WPARAM)main,
            MAKELPARAM(0, APPCOMMAND_MEDIA_PLAY_PAUSE)); return;
    }

    // Remaining YM-specific key shortcuts (shuffle/repeat) to render widget.
    // Only honored by Chromium when YM has actual OS focus — not flagged
    // as broken by the user, so left as-is for now.
    WORD vk = 0;
    switch (cmd) {
    case YMHC_SHUFFLE: vk = 'S'; break;
    case YMHC_REPEAT:  vk = 'R'; break;
    default: return;
    }

    HWND w = FindRenderWidget();
    if (!w) return;
    DWORD sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    PostMessageW(w, WM_KEYDOWN, vk, 1 | (sc << 16));
    PostMessageW(w, WM_KEYUP,   vk, 1 | (sc << 16) | 0xC0000000);
}

// ── In-page "cheat menu" overlay — now the ONLY settings surface ──
// (no tray, no separate hub window — see the plan's Фаза 3/4 notes) —
// rendered directly inside YM's own page, always injected (CheatMenuThreadFn
// below no longer gates this behind a toggle), toggled visible by tapping
// Shift alone while YM has focus (a deliberate *tap*, not held: a keydown
// immediately followed by another key's keydown before Shift's keyup is
// treated as a real Shift+letter combo and ignored, so normal capitalized
// typing elsewhere on the page never false-triggers it). Player controls
// reuse ExecCmd directly (no IPC round-trip needed — this *is* the process
// that already executes those commands); tweaks/CSS go through
// SaveTweaks/SaveCustomCss directly (registry + tweaksSeq bump).
//
// Scoped to [data-test-id='PLAYERBAR_DESKTOP'] for reading now-playing
// state (track/artist/cover/like/play — all stable data-test-id's found
// by inspecting the live page), since the unscoped aria-label selectors
// CdpClickButton/CdpQueryLiked use can match a *different* track's like
// button on list pages where several are rendered at once. Read-only
// here, so that ambiguity (a pre-existing characteristic of those two
// functions, not something this introduces) doesn't carry over.
static const wchar_t* kTweakLabels[9] = {
    L"AI-комментарии о треке", L"Анимация фона плеера", L"Плашка «Версия приложения»",
    L"Барабан рекомендаций", L"Плашка «Моя волна обновилась»", L"Лишние разделы меню",
    L"Плюс-бейдж в профиле", L"Крупная обложка трека", L"Скрыть имя пользователя",
};

static void CdpInjectMenu(DWORD mask, const wchar_t* customCssW) {
    std::wstring js;
    js += L"(function(){"
        L"var ROOT=document.getElementById('ymhub-cheat');"
        // Every CdpInjectMenu tick is a brand-new, independent
        // Runtime.evaluate call -- nothing from a previous tick's execution
        // persists except actual window.* properties. These three need to
        // be callable both from the one-time DOM-building code below
        // (inside if(!ROOT), runs only on the very first tick) *and* from
        // the unconditional resync tail at the end (runs every tick) --
        // declaring them inside if(!ROOT) instead left them undefined on
        // every tick after the first, since a function declared inside a
        // block doesn't survive past that block in strict-mode code
        // (confirmed live: threw "ymhubComboText is not a function" from
        // the resync tail once tested via the real generated script, not
        // just reasoned about).
        L"function ymhubVkLabel(vk){"
        L"var m={8:'Backspace',9:'Tab',13:'Enter',27:'Esc',32:'Space',37:'\\u2190',38:'\\u2191',39:'\\u2192',40:'\\u2193',"
        L"186:';',187:'+',188:',',189:'-',190:'.',191:'/',192:'`',219:'[',220:'\\\\',221:']',222:'\\''};"
        L"if(m[vk])return m[vk];"
        L"if(vk>=112&&vk<=123)return 'F'+(vk-111);"
        L"if(vk>=48&&vk<=90)return String.fromCharCode(vk);"
        L"return '#'+vk;}"
        L"function ymhubComboText(mods,vk){if(!vk)return 'Не задано';"
        L"var p=[];if(mods&1)p.push('Ctrl');if(mods&2)p.push('Shift');if(mods&4)p.push('Alt');"
        L"p.push(ymhubVkLabel(vk));return p.join('+');}"
        // Shared by both bind lists below (YMHub's own 6 + YM's native-
        // hotkey remap table's 13) -- same capture/display logic either
        // way, only the DOM ids, labels, message prefix and "what's the
        // currently-effective combo" getter differ.
        L"function ymhubBuildBinds(wrapId,labels,msgPrefix,getEffective){"
        L"var wrap=document.getElementById(wrapId);"
        L"labels.forEach(function(label,i){"
        L"var row=document.createElement('div');row.className='yc-tw-row';"
        L"var nm=document.createElement('div');nm.className='yc-tw-name';nm.textContent=label;"
        L"var btn=document.createElement('button');btn.className='yc-bind-btn';btn.id=wrapId+'-'+i;"
        L"row.appendChild(nm);row.appendChild(btn);wrap.appendChild(row);"
        L"btn.onclick=function(){"
        L"if(btn.classList.contains('rec'))return;"
        L"btn.classList.add('rec');btn.textContent='Нажмите клавишу\\u2026';"
        // Bare Ctrl/Shift/Alt keydowns arrive on their own before the real
        // key -- only 16/17/18 (Shift/Ctrl/Alt) get ignored so the capture
        // keeps waiting for an actual key instead of binding to "just Shift".
        L"var onKey=function(e){"
        L"e.preventDefault();e.stopPropagation();"
        L"if(e.keyCode===16||e.keyCode===17||e.keyCode===18)return;"
        L"document.removeEventListener('keydown',onKey,true);btn.classList.remove('rec');"
        L"if(e.keyCode===27){var cur=getEffective(i);btn.textContent=ymhubComboText(cur.mods,cur.vk);return;}"
        L"var mods=(e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0);"
        L"btn.textContent=ymhubComboText(mods,e.keyCode);"
        L"window.__ymhubQ.push(msgPrefix+i+':'+mods+':'+e.keyCode);};"
        L"document.addEventListener('keydown',onKey,true);};});}"
        L"if(!ROOT){"
        L"var st=document.createElement('style');st.id='ymhub-cheat-style';"
        // Same design tokens as the standalone hub window (--ac/--ac2/
        // --card/--bord/--txt/--txt2 in main.cpp's HTML_HUB) so the
        // overlay reads as the same product, not a generic dark panel.
        L"st.textContent="
        L"'#ymhub-cheat{position:fixed;top:50%;left:50%;z-index:999998;width:400px;"
        L"font-family:\"Segoe UI Variable Text\",\"Segoe UI\",system-ui,sans-serif;"
        L"opacity:0;pointer-events:none;transform:translate(-50%,-50%) scale(.96);"
        L"transition:opacity .15s ease,transform .15s ease;}"
        L"#ymhub-cheat.open{opacity:1;pointer-events:auto;transform:translate(-50%,-50%) scale(1);}"
        // Was a fully opaque #0d0d14 — backdrop-filter had nothing to
        // actually blur behind it, so the "glass" effect never showed at
        // all despite being declared. rgba background + a real accent-
        // tinted glow (same language as the connect toast) is what
        // actually reads as glass instead of just a flat dark panel.
        L"#ymhub-cheat .yc-panel{max-height:calc(100vh - 36px);overflow:auto;"
        L"background:rgba(13,13,20,.74);backdrop-filter:blur(24px);"
        L"border:1px solid rgba(91,143,255,.28);border-radius:16px;padding:16px;"
        L"box-shadow:0 0 0 1px rgba(91,143,255,.06),0 16px 48px rgba(0,0,0,.5),"
        L"0 0 32px rgba(91,143,255,.14);"
        L"color:rgba(255,255,255,.88);}"
        L"#ymhub-cheat .yc-head{display:flex;align-items:center;justify-content:space-between;"
        L"font-weight:700;font-size:13.5px;margin-bottom:12px;}"
        L"#ymhub-cheat .yc-hint{font-weight:500;font-size:11px;color:rgba(255,255,255,.4);cursor:pointer;}"
        // Left icon rail + right content pane (Neverlose-style shell) —
        // present in both Simple and Advanced; Advanced only reveals the
        // extra "pro" rail item, the rail itself is always there.
        L"#ymhub-cheat .yc-body{display:flex;align-items:flex-start;}"
        L"#ymhub-cheat .yc-rail{display:flex;flex-direction:column;align-items:center;"
        L"width:40px;flex-shrink:0;padding-right:10px;margin-right:12px;gap:4px;"
        L"border-right:1px solid rgba(255,255,255,.08);}"
        L"#ymhub-cheat .yc-rail-item{width:34px;height:34px;border-radius:10px;cursor:pointer;"
        L"display:flex;align-items:center;justify-content:center;color:rgba(255,255,255,.45);"
        L"transition:background .15s,color .15s;}"
        L"#ymhub-cheat .yc-rail-item:hover{background:rgba(255,255,255,.08);color:#fff;}"
        L"#ymhub-cheat .yc-rail-item.active{background:rgba(91,143,255,.18);color:#5b8fff;}"
        L"#ymhub-cheat .yc-rail-spacer{flex:1;min-height:8px;}"
        L"#ymhub-cheat .yc-rail-lbl{font-size:8px;color:rgba(255,255,255,.3);"
        L"text-transform:uppercase;letter-spacing:.3px;margin-bottom:3px;}"
        // A plain "transition:background" between a flat rgba() and a
        // linear-gradient() doesn't actually animate -- background-image
        // isn't interpolable, so browsers just snap it instantly no matter
        // what transition-duration says. Splitting the gradient into its
        // own ::before layer that fades via opacity (which *does*
        // interpolate) is what makes the on/off flip actually animate
        // instead of only the knob sliding while the track color jumps.
        L"#ymhub-cheat .yc-rail-adv{width:26px;height:16px;border-radius:99px;flex-shrink:0;margin-bottom:6px;"
        L"background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.08);position:relative;cursor:pointer;"
        L"overflow:hidden;transition:border-color .3s ease;}"
        // This whole CSS blob is itself one big JS single-quoted string
        // (st.textContent='...') -- an unescaped '' for the CSS content
        // property terminates that JS string right there instead of
        // producing an empty CSS string, corrupting everything after it
        // into raw (invalid) JS. Confirmed live: broke CdpInjectMenu's
        // entire script, not just this rule -- #ymhub-cheat never
        // appeared at all as a result, menu included.
        L"#ymhub-cheat .yc-rail-adv::before{content:\\'\\';position:absolute;inset:0;border-radius:inherit;"
        L"background:linear-gradient(135deg,#5b8fff,#7c6fff);opacity:0;transition:opacity .3s ease;}"
        L"#ymhub-cheat .yc-rail-adv.on{border-color:transparent;}"
        L"#ymhub-cheat .yc-rail-adv.on::before{opacity:1;}"
        L"#ymhub-cheat .yc-rail-adv .yc-knob{width:10px;height:10px;top:2px;left:2px;}"
        L"#ymhub-cheat .yc-rail-adv.on .yc-knob{left:14px;}"
        L"#ymhub-cheat .yc-pane{flex:1;min-width:0;}"
        // Everything else in this panel is transitioned (rail items,
        // switches, buttons) but switching between Player/Твики/Pro itself
        // used to be an instant display:none/block snap — the one
        // interaction in here with zero animation. display can't be
        // transitioned directly, but pairing it with an opacity fade-in
        // on the same class toggle still reads as a smooth appearance
        // (the outgoing section just disappears, which is the normal,
        // unremarkable way tab-like panels handle the "leaving" side).
        L"#ymhub-cheat .yc-sec{display:none;opacity:0;}"
        L"#ymhub-cheat .yc-sec.active{display:block;opacity:1;animation:ycSecIn .18s ease;}"
        L"@keyframes ycSecIn{from{opacity:0;transform:translateY(3px)}to{opacity:1;transform:translateY(0)}}"
        L"#ymhub-cheat .yc-player{display:flex;gap:10px;align-items:center;margin-bottom:12px;}"
        L"#ymhub-cheat .yc-cover{width:44px;height:44px;border-radius:10px;background:rgba(255,255,255,.05);"
        L"object-fit:cover;flex-shrink:0;}"
        L"#ymhub-cheat .yc-meta{overflow:hidden;}"
        L"#ymhub-cheat .yc-title{font-weight:600;font-size:12.5px;white-space:nowrap;text-overflow:ellipsis;overflow:hidden;}"
        L"#ymhub-cheat .yc-artist{font-size:11.5px;color:rgba(255,255,255,.4);white-space:nowrap;"
        L"text-overflow:ellipsis;overflow:hidden;margin-top:2px;}"
        // .yc-cb/.yc-skip/.yc-play/.yc-like/.yc-dislike mirror the
        // mini-player overlay's own .cb/.skip/#pbtn/#btn-like/#btn-dislike
        // (same file, the HTML constant) — same shapes, same hover/active
        // feel, just sized for this panel.
        L"#ymhub-cheat .yc-controls{display:flex;align-items:center;gap:6px;}"
        L"#ymhub-cheat .yc-cb{border:none;cursor:pointer;border-radius:50%;flex-shrink:0;"
        L"display:flex;align-items:center;justify-content:center;color:rgba(255,255,255,.65);"
        L"background:rgba(255,255,255,.06);transition:transform .15s,background .15s,color .15s;}"
        L"#ymhub-cheat .yc-cb:hover{background:rgba(255,255,255,.13);color:#fff;transform:scale(1.07);}"
        L"#ymhub-cheat .yc-cb:active{transform:scale(.88);}"
        L"#ymhub-cheat .yc-cb:disabled{opacity:.3;pointer-events:none;}"
        L"#ymhub-cheat .yc-skip{width:30px;height:30px;}"
        L"#ymhub-cheat .yc-play{width:38px;height:38px;"
        L"background:linear-gradient(135deg,#5b8fff,#7c6fff);color:#fff;}"
        L"#ymhub-cheat .yc-play:hover{filter:brightness(1.1);transform:scale(1.06);}"
        L"#ymhub-cheat .yc-like{width:30px;height:30px;color:rgba(255,255,255,.4);}"
        L"#ymhub-cheat .yc-like:hover{background:rgba(255,80,100,.18);color:rgba(255,100,120,.9);}"
        L"#ymhub-cheat .yc-like.on{background:rgba(255,60,90,.22);color:#ff4d6d;}"
        L"#ymhub-cheat .yc-dislike{width:30px;height:30px;color:rgba(255,255,255,.3);}"
        L"#ymhub-cheat .yc-dislike:hover{background:rgba(255,255,255,.12);color:rgba(255,255,255,.7);}"
        L"#ymhub-cheat .yc-modebtn{width:34px;height:34px;}"
        L"#ymhub-cheat .yc-modebtn.on{background:rgba(91,143,255,.18);color:#5b8fff;}"
        L"#ymhub-cheat .yc-sectitle{font-weight:700;font-size:11px;color:rgba(255,255,255,.4);"
        L"text-transform:uppercase;letter-spacing:.3px;margin-bottom:6px;}"
        // .yc-tw-row/.yc-tw-switch/.yc-knob mirror the hub's own
        // .tw-row/.tw-switch/.knob (same file) pixel-for-pixel in spirit —
        // a real sliding toggle, not a native checkbox.
        L"#ymhub-cheat .yc-tw-row{display:flex;align-items:center;gap:10px;padding:8px 0;"
        L"border-bottom:1px solid rgba(255,255,255,.08);}"
        L"#ymhub-cheat .yc-tw-row:last-child{border-bottom:none;}"
        L"#ymhub-cheat .yc-tw-name{flex:1;font-size:12px;}"
        // Same gradient-can't-transition issue as .yc-rail-adv above --
        // ::before fades in via opacity instead of snapping.
        L"#ymhub-cheat .yc-tw-switch{position:relative;width:32px;height:19px;flex-shrink:0;border-radius:99px;"
        L"background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.08);cursor:pointer;"
        L"overflow:hidden;transition:border-color .35s ease;}"
        L"#ymhub-cheat .yc-tw-switch::before{content:\\'\\';position:absolute;inset:0;border-radius:inherit;"
        L"background:linear-gradient(135deg,#5b8fff,#7c6fff);opacity:0;transition:opacity .35s ease;}"
        L"#ymhub-cheat .yc-tw-switch.on{border-color:transparent;}"
        L"#ymhub-cheat .yc-tw-switch.on::before{opacity:1;}"
        L"#ymhub-cheat .yc-knob{position:absolute;top:2px;left:2px;width:13px;height:13px;border-radius:50%;z-index:1;"
        L"background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.4);transition:left .35s cubic-bezier(.34,1.56,.64,1);}"
        L"#ymhub-cheat .yc-tw-switch.on .yc-knob{left:17px;}"
        L"#ymhub-cheat .yc-bind-btn{background:rgba(255,255,255,.06);border:1px solid rgba(255,255,255,.1);"
        L"color:rgba(255,255,255,.75);border-radius:7px;padding:4px 10px;font-size:11px;cursor:pointer;"
        L"font-family:inherit;white-space:nowrap;transition:background .15s,border-color .15s,color .15s;}"
        L"#ymhub-cheat .yc-bind-btn:hover{background:rgba(255,255,255,.12);color:#fff;}"
        L"#ymhub-cheat .yc-bind-btn.rec{background:rgba(91,143,255,.18);border-color:#5b8fff;color:#5b8fff;}"
        // .yc-cc/.yc-css mirror the hub's .cc-wrap/.cc-area.
        L"#ymhub-cheat .yc-cc{margin-top:14px;padding:12px;border-radius:12px;"
        L"background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);}"
        L"#ymhub-cheat .yc-cc-title{font-size:12px;font-weight:700;margin-bottom:8px;}"
        L"#ymhub-cheat .yc-css{width:100%;min-height:64px;resize:vertical;"
        L"background:rgba(255,255,255,.05);border:1px solid rgba(255,255,255,.08);border-radius:8px;"
        L"color:rgba(255,255,255,.88);font:11px Consolas,\"Cascadia Code\",monospace;"
        L"padding:8px;box-sizing:border-box;outline:none;transition:border-color .2s ease;}"
        L"#ymhub-cheat .yc-css:focus{border-color:rgba(91,143,255,.4);}"
        // Advanced-only "YM Pro" section: seek + volume + shuffle/repeat —
        // native range inputs restyled to match the rest of the panel.
        L"#ymhub-cheat .yc-seekrow,#ymhub-cheat .yc-volrow{display:flex;align-items:center;gap:8px;}"
        L"#ymhub-cheat .yc-time{font-size:10.5px;color:rgba(255,255,255,.4);flex-shrink:0;width:34px;}"
        L"#ymhub-cheat .yc-time.end{text-align:right;}"
        L"#ymhub-cheat input[type=range]{flex:1;-webkit-appearance:none;height:4px;border-radius:99px;"
        L"background:rgba(255,255,255,.12);outline:none;cursor:pointer;margin:0;}"
        L"#ymhub-cheat input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;"
        L"width:12px;height:12px;border-radius:50%;background:#5b8fff;box-shadow:0 1px 3px rgba(0,0,0,.4);}"
        L"#ymhub-cheat .yc-volicon{color:rgba(255,255,255,.4);flex-shrink:0;display:flex;}"
        L"#ymhub-cheat input[type=range]:disabled{opacity:.3;cursor:default;}"
        L"';"
        L"document.head.appendChild(st);"
        L"ROOT=document.createElement('div');ROOT.id='ymhub-cheat';"
        // Player control icon paths copied verbatim from the mini-player
        // overlay's own #btn-prev/#pbtn/#btn-next/#btn-like/#btn-dislike
        // SVGs (same file) — real shapes instead of Unicode glyphs, which
        // render inconsistently across fonts at small sizes. Rail/mode
        // icons (note, sliders, shuffle, repeat, speaker) are plain
        // generic outline glyphs in the same spirit, not traced from any
        // specific icon set.
        L"ROOT.innerHTML="
        L"'<div class=\"yc-panel\">"
        L"<div class=\"yc-head\">YMHub <span class=\"yc-hint\" id=\"yc-close\">Shift — закрыть</span></div>"
        L"<div class=\"yc-body\">"
        L"<div class=\"yc-rail\">"
        L"<div class=\"yc-rail-item active\" id=\"yc-rail-player\" data-sec=\"player\" title=\"Плеер\">"
        L"<svg width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
        L"<path d=\"M9 18V5l12-2v13\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>"
        L"<circle cx=\"6\" cy=\"18\" r=\"3\"/><circle cx=\"18\" cy=\"16\" r=\"3\"/></svg></div>"
        L"<div class=\"yc-rail-item\" id=\"yc-rail-tweaks\" data-sec=\"tweaks\" title=\"Твики\">"
        L"<svg width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\">"
        L"<line x1=\"4\" y1=\"21\" x2=\"4\" y2=\"14\"/><line x1=\"4\" y1=\"10\" x2=\"4\" y2=\"3\"/>"
        L"<line x1=\"12\" y1=\"21\" x2=\"12\" y2=\"12\"/><line x1=\"12\" y1=\"8\" x2=\"12\" y2=\"3\"/>"
        L"<line x1=\"20\" y1=\"21\" x2=\"20\" y2=\"16\"/><line x1=\"20\" y1=\"12\" x2=\"20\" y2=\"3\"/>"
        L"<circle cx=\"4\" cy=\"12\" r=\"2\" fill=\"currentColor\"/><circle cx=\"12\" cy=\"10\" r=\"2\" fill=\"currentColor\"/>"
        L"<circle cx=\"20\" cy=\"14\" r=\"2\" fill=\"currentColor\"/></svg></div>"
        L"<div class=\"yc-rail-item\" id=\"yc-rail-binds\" data-sec=\"binds\" title=\"Бинды\">"
        L"<svg width=\"16\" height=\"16\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        L"<rect x=\"2\" y=\"5\" width=\"20\" height=\"14\" rx=\"2\"/>"
        L"<circle cx=\"6.5\" cy=\"9.5\" r=\"1\" fill=\"currentColor\"/><circle cx=\"11\" cy=\"9.5\" r=\"1\" fill=\"currentColor\"/>"
        L"<circle cx=\"15.5\" cy=\"9.5\" r=\"1\" fill=\"currentColor\"/>"
        L"<line x1=\"7\" y1=\"14.5\" x2=\"15\" y2=\"14.5\"/></svg></div>"
        L"<div class=\"yc-rail-item\" id=\"yc-rail-pro\" data-sec=\"pro\" title=\"YM Pro\" style=\"display:none\">⚡</div>"
        L"<div class=\"yc-rail-spacer\"></div>"
        L"<div class=\"yc-rail-lbl\">ADV</div>"
        L"<div class=\"yc-rail-adv\" id=\"yc-adv-toggle\" title=\"Расширенный режим\"><div class=\"yc-knob\"></div></div>"
        L"</div>"
        L"<div class=\"yc-pane\">"
        L"<div class=\"yc-sec active\" data-sec=\"player\">"
        L"<div class=\"yc-player\"><img class=\"yc-cover\" id=\"yc-cover\"><div class=\"yc-meta\">"
        L"<div class=\"yc-title\" id=\"yc-title\">—</div><div class=\"yc-artist\" id=\"yc-artist\"></div></div></div>"
        L"<div class=\"yc-controls\">"
        L"<button class=\"yc-cb yc-skip\" id=\"yc-prev\"><svg width=\"13\" height=\"13\" viewBox=\"0 0 15 15\" fill=\"currentColor\">"
        L"<rect x=\"1.5\" y=\"1.5\" width=\"2.5\" height=\"12\" rx=\"1.1\"/><path d=\"M13 2.5 5.5 7.5 13 12.5V2.5z\"/></svg></button>"
        L"<button class=\"yc-cb yc-play\" id=\"yc-play\">"
        L"<svg id=\"yc-i-play\" width=\"15\" height=\"15\" viewBox=\"0 0 17 17\" fill=\"currentColor\"><path d=\"M5.5 3.5 14 8.5 5.5 13.5V3.5z\"/></svg>"
        L"<svg id=\"yc-i-pause\" width=\"15\" height=\"15\" viewBox=\"0 0 17 17\" fill=\"currentColor\" style=\"display:none\">"
        L"<rect x=\"3.5\" y=\"3\" width=\"3.2\" height=\"11\" rx=\"1.3\"/><rect x=\"10.3\" y=\"3\" width=\"3.2\" height=\"11\" rx=\"1.3\"/></svg></button>"
        L"<button class=\"yc-cb yc-skip\" id=\"yc-next\"><svg width=\"13\" height=\"13\" viewBox=\"0 0 15 15\" fill=\"currentColor\">"
        L"<rect x=\"11\" y=\"1.5\" width=\"2.5\" height=\"12\" rx=\"1.1\"/><path d=\"M2 2.5l7.5 5L2 12.5V2.5z\"/></svg></button>"
        L"<button class=\"yc-cb yc-like\" id=\"yc-like\"><svg width=\"13\" height=\"13\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
        L"<path d=\"M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 "
        L"19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z\"/></svg></button>"
        L"<button class=\"yc-cb yc-dislike\" id=\"yc-dislike\"><svg width=\"12\" height=\"12\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
        L"<path d=\"M15 3H6c-.83 0-1.54.5-1.84 1.22l-3.02 7.05c-.09.23-.14.47-.14.73v2c0 1.1.9 2 2 2h6.31l-.95 4.57-.03.32c0 "
        L".41.17.79.44 1.06L9.83 23l6.59-6.59c.36-.36.58-.86.58-1.41V5c0-1.1-.9-2-2-2zm4 0v12h4V3h-4z\"/></svg></button>"
        L"</div></div>"
        L"<div class=\"yc-sec\" data-sec=\"tweaks\"><div class=\"yc-sectitle\">Твики</div>"
        L"<div id=\"yc-tweaks\"></div>"
        L"<div class=\"yc-cc\"><div class=\"yc-cc-title\">Свой CSS</div>"
        L"<textarea class=\"yc-css\" id=\"yc-css\" spellcheck=\"false\" placeholder=\".selector{ ... }\"></textarea></div>"
        L"</div>"
        L"<div class=\"yc-sec\" data-sec=\"binds\"><div class=\"yc-sectitle\">Бинды</div>"
        L"<div id=\"yc-binds\"></div>"
        L"<div class=\"yc-sectitle\" style=\"margin-top:14px\">Хоткеи Яндекс Музыки</div>"
        L"<div id=\"yc-ymbinds\"></div>"
        L"</div>"
        L"<div class=\"yc-sec\" data-sec=\"pro\">"
        L"<div class=\"yc-sectitle\">Перемотка</div>"
        L"<div class=\"yc-seekrow\"><span class=\"yc-time\" id=\"yc-t-start\">0:00</span>"
        L"<input type=\"range\" id=\"yc-seek\" min=\"0\" max=\"100\" value=\"0\">"
        L"<span class=\"yc-time end\" id=\"yc-t-end\">0:00</span></div>"
        L"<div class=\"yc-sectitle\" style=\"margin-top:14px\">Громкость</div>"
        L"<div class=\"yc-volrow\"><span class=\"yc-volicon\"><svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
        L"<path d=\"M3 9v6h4l5 5V4L7 9H3z\"/></svg></span>"
        L"<input type=\"range\" id=\"yc-vol\" min=\"0\" max=\"1\" step=\"0.01\" value=\"0.5\"></div>"
        L"<div class=\"yc-sectitle\" style=\"margin-top:14px\">Режимы</div>"
        L"<div class=\"yc-controls\">"
        L"<button class=\"yc-cb yc-modebtn\" id=\"yc-shuffle\" title=\"Случайный порядок\">"
        L"<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        L"<polyline points=\"16 3 21 3 21 8\"/><line x1=\"4\" y1=\"20\" x2=\"21\" y2=\"3\"/>"
        L"<polyline points=\"21 16 21 21 16 21\"/><line x1=\"15\" y1=\"15\" x2=\"21\" y2=\"21\"/>"
        L"<line x1=\"4\" y1=\"4\" x2=\"9\" y2=\"9\"/></svg></button>"
        L"<button class=\"yc-cb yc-modebtn\" id=\"yc-repeat\" title=\"Повтор трека\">"
        L"<svg width=\"14\" height=\"14\" viewBox=\"0 0 24 24\" fill=\"none\" stroke=\"currentColor\" stroke-width=\"2.2\" stroke-linecap=\"round\" stroke-linejoin=\"round\">"
        L"<polyline points=\"17 1 21 5 17 9\"/><path d=\"M3 11V9a4 4 0 0 1 4-4h14\"/>"
        L"<polyline points=\"7 23 3 19 7 15\"/><path d=\"M21 13v2a4 4 0 0 1-4 4H3\"/></svg></button>"
        L"</div></div>"
        L"</div></div></div>';"
        L"document.body.appendChild(ROOT);"
        L"document.getElementById('yc-close').onclick=function(){ROOT.classList.remove('open');};"
        L"document.getElementById('yc-prev').onclick=function(){window.__ymhubQ.push('prev');};"
        L"document.getElementById('yc-play').onclick=function(){window.__ymhubQ.push('toggle');};"
        L"document.getElementById('yc-next').onclick=function(){window.__ymhubQ.push('next');};"
        L"document.getElementById('yc-like').onclick=function(){window.__ymhubQ.push('like');};"
        L"document.getElementById('yc-dislike').onclick=function(){window.__ymhubQ.push('dislike');};"
        // Rail navigation — switches the active section; the rail itself
        // (and the Плеер/Твики items) is always present in both Simple and
        // Advanced, only the "pro" item's visibility depends on the toggle.
        L"Array.prototype.forEach.call(document.querySelectorAll('#ymhub-cheat .yc-rail-item'),function(item){"
        L"item.onclick=function(){"
        L"Array.prototype.forEach.call(document.querySelectorAll('#ymhub-cheat .yc-rail-item'),function(i){i.classList.remove('active');});"
        L"Array.prototype.forEach.call(document.querySelectorAll('#ymhub-cheat .yc-sec'),function(s){s.classList.remove('active');});"
        L"item.classList.add('active');"
        L"document.querySelector('#ymhub-cheat .yc-sec[data-sec=\"'+item.dataset.sec+'\"]').classList.add('active');};});"
        // Advanced mode is a pure page-local display preference (which
        // rail items are visible) — persisted in this page's own
        // localStorage, never round-tripped through the DLL/host at all.
        L"var advBtn=document.getElementById('yc-adv-toggle');"
        L"var railPro=document.getElementById('yc-rail-pro');"
        L"function applyAdvanced(on){advBtn.classList.toggle('on',on);railPro.style.display=on?'':'none';"
        L"if(!on&&railPro.classList.contains('active'))document.getElementById('yc-rail-player').click();}"
        L"applyAdvanced(localStorage.getItem('ymhubAdvanced')==='1');"
        L"advBtn.onclick=function(){var on=!advBtn.classList.contains('on');"
        L"localStorage.setItem('ymhubAdvanced',on?'1':'0');applyAdvanced(on);};"
        L"document.getElementById('yc-seek').addEventListener('input',function(){window.__ymhubQ.push('seek:'+this.value);});"
        L"document.getElementById('yc-vol').addEventListener('input',function(){window.__ymhubQ.push('vol:'+this.value);});"
        L"document.getElementById('yc-shuffle').onclick=function(){window.__ymhubQ.push('shuffle');};"
        L"document.getElementById('yc-repeat').onclick=function(){window.__ymhubQ.push('repeat');};"
        L"var twWrap=document.getElementById('yc-tweaks');"
        L"window.__ymhubTwLabels.forEach(function(label,i){"
        L"var row=document.createElement('div');row.className='yc-tw-row';"
        L"var nm=document.createElement('div');nm.className='yc-tw-name';nm.textContent=label;"
        L"var sw=document.createElement('div');sw.className='yc-tw-switch';sw.id='yc-tw-'+i;"
        L"var knob=document.createElement('div');knob.className='yc-knob';sw.appendChild(knob);"
        L"sw.onclick=function(){sw.classList.toggle('on');window.__ymhubQ.push('tweak:'+i);};"
        L"row.appendChild(nm);row.appendChild(sw);twWrap.appendChild(row);});"
        L"document.getElementById('yc-css').addEventListener('change',function(){"
        L"window.__ymhubQ.push('css:'+this.value);});"
        L"window.__ymhubQ=window.__ymhubQ||[];"
        L"ymhubBuildBinds('yc-binds',['Показать/скрыть плеер','Предыдущий трек','Следующий трек',"
        L"'Пауза/воспроизведение','Нравится','Не нравится'],'rebind:',"
        L"function(i){return window.__ymhubKeys[i];});"
        // YM's own native hotkeys (see LLKeyProc/g_ymKeys) -- vk:0 means "no
        // remap, YM's own default key still works untouched", so the
        // effective combo shown here falls back to YM_DEFAULT_VK/mods:0
        // rather than "Не задано" (that fallback text is only right for
        // YMHub's own binds above, where vk:0 genuinely means nothing
        // happens at all).
        L"ymhubBuildBinds('yc-ymbinds',['Пауза/воспроизведение','Заглушить','Перемотка вперёд',"
        L"'Перемотка назад','Громкость +','Громкость \\u2212','Нравится','Не нравится','Повтор',"
        L"'Перемешать','Следующий трек','Предыдущий трек','Полный экран'],'ymrebind:',"
        L"function(i){var k=window.__ymhubYmKeys[i];"
        L"return k.vk?k:{mods:0,vk:window.__ymhubYmDefaults[i]};});"
        // Named, window-stored handlers (not anonymous inline closures)
        // so CdpRemoveMenu can actually remove them when the feature is
        // switched off instead of leaving a stray document-wide hook.
        // location===2 is the right Shift key specifically (1 is left,
        // KeyboardEvent never reports 0/"standard" for Shift) — only the
        // right one opens the menu, confirmed live that left Shift was
        // triggering it too and that's not wanted (collides with normal
        // left-Shift use elsewhere).
        L"window.__ymhubShiftArmed=false;"
        L"window.__ymhubShiftDown=function(e){"
        L"if(e.key==='Escape'){ROOT.classList.remove('open');return;}"
        L"if(e.key==='Shift'){if(e.location===2&&!window.__ymhubShiftArmed)window.__ymhubShiftArmed=true;}"
        L"else{window.__ymhubShiftArmed=false;}};"
        L"window.__ymhubShiftUp=function(e){"
        L"if(e.key!=='Shift'||e.location!==2)return;"
        L"if(window.__ymhubShiftArmed){var ae=document.activeElement,tag=ae&&ae.tagName;"
        L"if(tag!=='INPUT'&&tag!=='TEXTAREA')ROOT.classList.toggle('open');}"
        L"window.__ymhubShiftArmed=false;};"
        L"document.addEventListener('keydown',window.__ymhubShiftDown,true);"
        L"document.addEventListener('keyup',window.__ymhubShiftUp,true);"
        // Shared by syncPlaying and syncPro (each calls getPb() fresh, not
        // once at build time — the element identity changes when the page
        // navigates between "Моя волна" and everywhere else).
        L"function getPb(){return document.querySelector(\"[data-test-id='PLAYERBAR_DESKTOP']\")||"
        L"document.querySelector(\"[data-test-id='VIBE_PLAYERBAR']\");}"
        L"function qIn(pb,id){return pb.querySelector(\"[data-test-id='\"+id+\"']\");}"
        L"function syncPlaying(){"
        // "Моя волна" (Vibe) renders its own separate player bar
        // (VIBE_PLAYERBAR) instead of the regular PLAYERBAR_DESKTOP one —
        // confirmed empirically (PLAYERBAR_DESKTOP is simply absent while
        // that page is open) — so both are tried, in the order they're
        // actually likely to exist.
        L"var pb=getPb();if(!pb)return;"
        L"function q(id){return qIn(pb,id);}"
        // Both TRACK_TITLE and VIBE_PLAYERBAR_TRACK_NAME wrap a marquee
        // pair of two duplicate text nodes for scroll-on-overflow — taking
        // textContent on the wrapper concatenates both copies, so the
        // first child specifically is used instead.
        L"var t=q('TRACK_TITLE')||q('VIBE_PLAYERBAR_TRACK_NAME');"
        // SEPARATED_ARTIST_TITLE only exists inside PLAYERBAR_DESKTOP;
        // Vibe has no equivalent in its own player bar, only the big
        // page heading (a hash-suffixed class, less stable, but it's the
        // only thing showing it in that view).
        L"var a=q('SEPARATED_ARTIST_TITLE')||document.querySelector(\"[class*='VibePage_text__']\");"
        L"var c=q('ENTITY_COVER_IMAGE')||q('VIBE_ALBUM_COVER');"
        L"var lk=q('LIKE_BUTTON');"
        L"var tt=t?(t.children[0]?t.children[0].textContent:t.textContent):'';"
        L"document.getElementById('yc-title').textContent=tt||'\\u2014';"
        L"document.getElementById('yc-artist').textContent=a?a.textContent:'';"
        L"var csrc=c?(c.src||(c.querySelector&&c.querySelector('img')?c.querySelector('img').src:'')):'';"
        L"if(csrc)document.getElementById('yc-cover').src=csrc;"
        L"document.getElementById('yc-like').classList.toggle('on',!!lk&&lk.getAttribute('aria-pressed')==='true');"
        // Confirmed empirically: playing swaps the button's data-test-id
        // to PAUSE_BUTTON entirely (not just its aria-label on the same
        // id) — same family as SHUFFLE_BUTTON_ON/OFF and
        // REPEAT_BUTTON_NO_REPEAT, so presence/absence of PAUSE_BUTTON is
        // the state signal, not any attribute on PLAY_BUTTON.
        L"var playing=!!q('PAUSE_BUTTON');"
        L"document.getElementById('yc-i-play').style.display=playing?'none':'';"
        L"document.getElementById('yc-i-pause').style.display=playing?'':'none';"
        L"}"
        // Advanced "YM Pro" sync — seek/volume mirror the real sliders
        // (skipped while the user has OUR slider focused, so polling
        // doesn't fight an in-progress drag); shuffle reads .disabled too
        // since YM itself disables that button in some queue contexts
        // (confirmed empirically — clicks on a disabled SHUFFLE_BUTTON are
        // simply no-ops, nothing wrong with the click mechanism itself).
        // Repeat here is a plain two-state toggle (NO_REPEAT/REPEAT_ONE) —
        // unlike some other players YM has no separate "repeat all".
        L"function syncPro(){"
        L"var pb=getPb();if(!pb)return;"
        L"function q(id){return qIn(pb,id);}"
        // On "Моя волна" the timecode element is a plain DIV (a passive
        // progress bar, no fixed queue to seek within) rather than the
        // real range input the rest of the app uses — confirmed live.
        // Mirror it as a disabled slider instead of trying to drive a
        // value-set against an element that has no .value at all.
        L"var seekEl=q('TIMECODE_SLIDER')||q('VIBE_PLAYERBAR_TIMECODE_SLIDER');"
        L"var mySeek=document.getElementById('yc-seek');"
        L"var seekable=!!seekEl&&seekEl.tagName==='INPUT';"
        L"mySeek.disabled=!seekable;"
        L"if(seekable&&document.activeElement!==mySeek){"
        L"mySeek.max=seekEl.getAttribute('max')||100;mySeek.value=seekEl.value;"
        L"var ts=q('TIMECODE_TIME_START'),te=q('TIMECODE_TIME_END');"
        L"document.getElementById('yc-t-start').textContent=ts?ts.textContent:'0:00';"
        L"document.getElementById('yc-t-end').textContent=te?te.textContent:'0:00';"
        L"}else if(!seekable){"
        L"var vtc=q('VIBE_PLAYERBAR_TIMECODE'),parts=vtc?vtc.textContent.split('/'):null;"
        L"document.getElementById('yc-t-start').textContent=parts?parts[0].trim():'0:00';"
        L"document.getElementById('yc-t-end').textContent=parts?parts[1].trim():'0:00';"
        L"}"
        L"var volEl=q('CHANGE_VOLUME_SLIDER');var myVol=document.getElementById('yc-vol');"
        L"if(volEl&&document.activeElement!==myVol)myVol.value=volEl.value;"
        // Confirmed live: like PLAY_BUTTON/PAUSE_BUTTON, the id itself
        // swaps to SHUFFLE_BUTTON_ON when active (plain SHUFFLE_BUTTON
        // otherwise) — an exact-match lookup only ever sees the off state.
        L"var shuf=pb.querySelector(\"[data-test-id^='SHUFFLE_BUTTON']\");var shufBtn=document.getElementById('yc-shuffle');"
        L"if(shuf){shufBtn.disabled=shuf.disabled;"
        L"shufBtn.classList.toggle('on',shuf.getAttribute('data-test-id')!=='SHUFFLE_BUTTON');}"
        // Confirmed live: repeat actually has *three* states (cycles
        // NO_REPEAT -> REPEAT_BUTTON_REPEAT_CONTEXT -> _REPEAT_ONE ->
        // back), not just the two this was first tested against — rather
        // than allowlist every "on" suffix, anything that isn't the bare
        // NO_REPEAT id counts as on.
        L"var rep=pb.querySelector(\"[data-test-id^='REPEAT_BUTTON']\");"
        L"if(rep)document.getElementById('yc-repeat').classList.toggle('on',"
        L"rep.getAttribute('data-test-id')!=='REPEAT_BUTTON_NO_REPEAT');"
        L"}"
        L"window.__ymhubSyncPlaying=syncPlaying;"
        // This interval runs entirely inside the page's own JS engine, on
        // its own event loop — completely outside anything the DLL's own
        // native-side minimize/focus guards (CdpUnsafeNow) can see or
        // pause. It kept running every 700ms regardless of what the
        // native side did, which is exactly why those guards alone never
        // fully closed the minimize/focus-loss crash: this was the other
        // half. document.hidden is true exactly when a window minimizes
        // or gets fully occluded (e.g. by another app's exclusive
        // fullscreen) — skipping the actual DOM work on those ticks
        // keeps the crash-prone code from ever running during exactly
        // the moments that were racing it.
        // A rapid, repeated back-and-forth (alt-tabbing fast, or minimize
        // immediately followed by refocus) can still land a sync call
        // right in the settling period after visibilitychange fires but
        // before document.hidden has genuinely stabilized — confirmed
        // live under randomized rapid-fire testing. The listener plus a
        // short cooldown after *any* change closes that: not just "is it
        // hidden right now" but "did it change recently at all."
        L"var __ymhubHideAt=0;"
        L"document.addEventListener('visibilitychange',function(){__ymhubHideAt=Date.now();});"
        L"function __ymhubSyncSafe(){"
        L"if(!document.hidden&&(Date.now()-__ymhubHideAt)>1500){syncPlaying();syncPro();}}"
        L"window.__ymhubSyncTimer=setInterval(__ymhubSyncSafe,700);"
        L"if(!document.hidden){syncPlaying();syncPro();}"
        L"}"
        L"var cssBox=document.getElementById('yc-css');"
        L"if(document.activeElement!==cssBox)cssBox.value=window.__ymhubCss||'';"
        L"for(var i=0;i<9;i++){var sw=document.getElementById('yc-tw-'+i);"
        L"if(sw)sw.classList.toggle('on',!!(window.__ymhubMask&(1<<i)));}"
        // Skips any button mid-capture ('rec') so a native-side refresh
        // landing while the user is actively pressing a new key can't
        // stomp the "Нажмите клавишу…" placeholder out from under them.
        L"for(var bi=0;bi<6;bi++){var bb=document.getElementById('yc-binds-'+bi);"
        L"if(bb&&!bb.classList.contains('rec')){var k=window.__ymhubKeys[bi];"
        L"bb.textContent=ymhubComboText(k.mods,k.vk);}}"
        L"for(var yi=0;yi<13;yi++){var yb=document.getElementById('yc-ymbinds-'+yi);"
        L"if(yb&&!yb.classList.contains('rec')){var yk=window.__ymhubYmKeys[yi];"
        L"if(!yk.vk)yk={mods:0,vk:window.__ymhubYmDefaults[yi]};"
        L"yb.textContent=ymhubComboText(yk.mods,yk.vk);}}"
        L"})()";
    std::string preamble =
        "window.__ymhubTwLabels=[";
    for (int i = 0; i < 9; i++) {
        if (i) preamble += ",";
        preamble += "\"" + CdpJsonEscape(CdpUtf8(kTweakLabels[i])) + "\"";
    }
    preamble += "];window.__ymhubMask=" + std::to_string(mask) + ";"
        "window.__ymhubCss=\"" + CdpJsonEscape(CdpUtf8(customCssW ? customCssW : L"")) + "\";";
    preamble += "window.__ymhubKeys=[";
    for (int i = 0; i < 6; i++) {
        if (i) preamble += ",";
        preamble += "{\"mods\":" + std::to_string(g_keys[i].mods) + ",\"vk\":" + std::to_string(g_keys[i].vk) + "}";
    }
    preamble += "];";
    preamble += "window.__ymhubYmKeys=[";
    for (int i = 0; i < 13; i++) {
        if (i) preamble += ",";
        preamble += "{\"mods\":" + std::to_string(g_ymKeys[i].mods) + ",\"vk\":" + std::to_string(g_ymKeys[i].vk) + "}";
    }
    preamble += "];window.__ymhubYmDefaults=[";
    for (int i = 0; i < 13; i++) {
        if (i) preamble += ",";
        preamble += std::to_string(YM_DEFAULT_VK[i]);
    }
    preamble += "];";
    CdpRunJs(preamble + CdpUtf8(js.c_str()));
}

// Decodes one JSON string literal starting at s[i] (must be '"'),
// advancing i past the closing quote — used to pull action strings back
// out of window.__ymhubQ (see CdpQueueDrain), where a "css:..." entry can
// contain absolutely any text the user typed, including quotes and
// backslashes, so a naive find('"') for the closing quote isn't safe.
static std::string JsonDecodeStringAt(const std::string& s, size_t& i) {
    std::string out;
    if (i >= s.size() || s[i] != '"') return out;
    i++;
    while (i < s.size() && s[i] != '"') {
        char c = s[i];
        if (c == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            switch (n) {
            case '"':  out += '"';  i += 2; break;
            case '\\': out += '\\'; i += 2; break;
            case '/':  out += '/';  i += 2; break;
            case 'n':  out += '\n'; i += 2; break;
            case 'r':  out += '\r'; i += 2; break;
            case 't':  out += '\t'; i += 2; break;
            case 'u':
                if (i + 5 < s.size()) {
                    int cp = (int)strtol(s.substr(i + 2, 4).c_str(), nullptr, 16);
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    i += 6;
                } else i++;
                break;
            default: out += n; i += 2; break;
            }
        } else { out += c; i++; }
    }
    if (i < s.size()) i++;
    return out;
}

// seek:/vol: values are spliced directly into a JS source string run via
// CdpRunJs (see below) — they should always be a plain number straight
// from our own <input type=range>.value, but since that text ends up
// inside eval'd script, reject anything that isn't actually numeric
// rather than trusting the queue contents.
static bool IsPlainNumber(const std::string& s) {
    if (s.empty()) return false;
    bool seenDigit = false, seenDot = false;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (c == '-' && i == 0) continue;
        if (c == '.' && !seenDot) { seenDot = true; continue; }
        if (c < '0' || c > '9') return false;
        seenDigit = true;
    }
    return seenDigit;
}

// Dispatches one queued action from the overlay. Playback commands reuse
// ExecCmd directly — see the block comment above CdpInjectMenu for why
// that needs no IPC hop. Tweaks/CSS go through SaveTweaks/SaveCustomCss
// (registry + tweaksSeq bump) — WorkerThread's own tweaksSeq watch is
// what actually re-runs CdpApplyTweaks, same as any other tweak change.
static void DispatchCheatAction(const std::string& item) {
    if (item == "like")    { ExecCmd(YMHC_LIKE);    return; }
    if (item == "dislike") { ExecCmd(YMHC_DISLIKE); return; }
    if (item == "prev")    { ExecCmd(YMHC_PREV);    return; }
    if (item == "next")    { ExecCmd(YMHC_NEXT);    return; }
    if (item == "toggle")  { ExecCmd(YMHC_TOGGLE);  return; }
    // Scoped coordinate-click (CdpClickScoped), not ExecCmd's YMHC_SHUFFLE/
    // YMHC_REPEAT key-send — that path only works while the page's
    // *focused element* isn't a text input (e.g. the search box), which
    // the OS-focus check alone (already guaranteed here) doesn't cover.
    if (item == "shuffle") { CdpClickScoped("SHUFFLE_BUTTON"); return; }
    if (item == "repeat")  { CdpClickScoped("REPEAT_BUTTON");  return; }
    // Seek/volume: real native range inputs, confirmed live — set via the
    // standard React-controlled-input trick (native value setter + a real
    // 'input'/'change' event) rather than a coordinate drag simulation,
    // since the value already comes from OUR OWN slider (always a plain
    // number, never arbitrary user text like the css: case below).
    if (item.rfind("seek:", 0) == 0) {
        std::string val = item.substr(5);
        if (!IsPlainNumber(val)) return;
        std::string js =
            "(function(){var pb=document.querySelector(\"[data-test-id='PLAYERBAR_DESKTOP']\")||"
            "document.querySelector(\"[data-test-id='VIBE_PLAYERBAR']\");if(!pb)return;"
            "var el=pb.querySelector(\"[data-test-id='TIMECODE_SLIDER']\")||"
            "pb.querySelector(\"[data-test-id='VIBE_PLAYERBAR_TIMECODE_SLIDER']\");"
            // On "Моя волна" this resolves to a plain DIV (no fixed queue
            // to seek within) rather than the real range input the rest
            // of the app uses for this — confirmed live. The panel itself
            // already disables its seek slider in that case (see
            // syncPro), this is the matching guard on the write side.
            "if(!el||el.tagName!=='INPUT')return;"
            "var d=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value');"
            "d.set.call(el,'" + val + "');"
            "el.dispatchEvent(new Event('input',{bubbles:true}));"
            "el.dispatchEvent(new Event('change',{bubbles:true}));})()";
        CdpRunJs(js);
        return;
    }
    if (item.rfind("vol:", 0) == 0) {
        std::string val = item.substr(4);
        if (!IsPlainNumber(val)) return;
        std::string js =
            "(function(){var pb=document.querySelector(\"[data-test-id='PLAYERBAR_DESKTOP']\")||"
            "document.querySelector(\"[data-test-id='VIBE_PLAYERBAR']\");if(!pb)return;"
            "var el=pb.querySelector(\"[data-test-id='CHANGE_VOLUME_SLIDER']\");if(!el)return;"
            "var d=Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype,'value');"
            "d.set.call(el,'" + val + "');"
            "el.dispatchEvent(new Event('input',{bubbles:true}));"
            "el.dispatchEvent(new Event('change',{bubbles:true}));})()";
        CdpRunJs(js);
        return;
    }
    if (item.rfind("tweak:", 0) == 0) {
        int idx = atoi(item.c_str() + 6);
        if (idx < 0 || idx >= 9) return;
        g_tweaksMask ^= (1u << idx);
        SaveTweaks();
        return;
    }
    if (item.rfind("css:", 0) == 0) {
        std::string cssUtf8 = item.substr(4);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cssUtf8.c_str(), (int)cssUtf8.size(), nullptr, 0);
        std::wstring cssW(wlen, 0);
        if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, cssUtf8.c_str(), (int)cssUtf8.size(), cssW.data(), wlen);
        SaveCustomCss(cssW);
        return;
    }
    // "rebind:<idx>:<mods>:<vk>" from the in-page binds capture UI --
    // written straight to the same Key0..Key5 registry values LoadKeys()
    // already reads, then RefreshHotkeys() (on HotkeyThread, via
    // WM_REFRESHHK -- it's the thread that owns g_hkWnd/the actual
    // RegisterHotKey calls) picks it up. g_keys itself gets updated too so
    // the very next CdpInjectMenu tick's preamble reflects it immediately
    // instead of only after RefreshHotkeys() re-runs LoadKeys().
    if (item.rfind("rebind:", 0) == 0) {
        int idx = 0, mods = 0, vk = 0;
        if (sscanf_s(item.c_str() + 7, "%d:%d:%d", &idx, &mods, &vk) == 3 && idx >= 0 && idx < 6) {
            g_keys[idx] = { (DWORD)mods, (DWORD)vk };
            RegSetDW(HKEY_CURRENT_USER, REG_APP, KEY_REG[idx], ((DWORD)mods << 16) | (DWORD)vk);
            if (g_hkWnd) PostMessageW(g_hkWnd, WM_REFRESHHK, 0, 0);
        }
        return;
    }
    // YM's own native-hotkey remap table (g_ymKeys, read live by LLKeyProc
    // on every keypress) -- unlike g_keys above, this needs no
    // RegisterHotKey/WM_REFRESHHK dance, just the in-memory array updated
    // so the very next keypress already sees it.
    if (item.rfind("ymrebind:", 0) == 0) {
        int idx = 0, mods = 0, vk = 0;
        if (sscanf_s(item.c_str() + 9, "%d:%d:%d", &idx, &mods, &vk) == 3 && idx >= 0 && idx < 13) {
            g_ymKeys[idx] = { (DWORD)mods, (DWORD)vk };
            RegSetDW(HKEY_CURRENT_USER, REG_APP, YMKEY_REG[idx], ((DWORD)mods << 16) | (DWORD)vk);
        }
        return;
    }
}

// Drains window.__ymhubQ (pushed by the overlay's own button/checkbox/
// textarea handlers) every tick. id:10, distinct from every other fixed
// request id already in use on this connection.
static void CdpQueueDrain() {
    std::string resp;
    {
        std::lock_guard<std::mutex> lk(g_cdpMx);
        if (!CdpEnsureConnected()) return;
        std::string req = "{\"id\":10,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":"
            "\"(function(){var q=window.__ymhubQ||[];window.__ymhubQ=[];return JSON.stringify(q);})()\","
            "\"returnByValue\":true}}";
        if (!CdpSend(req)) { CdpClose(); return; }
        if (!CdpRecv(resp)) { CdpClose(); return; }
    }
    auto p = resp.find("\"value\":");
    if (p == std::string::npos) return;
    size_t i = p + 8;
    std::string arrJson = JsonDecodeStringAt(resp, i); // un-escape one level -> "[...]"
    size_t j = arrJson.find('[');
    if (j == std::string::npos) return;
    j++;
    std::vector<std::string> items;
    while (j < arrJson.size()) {
        while (j < arrJson.size() && (arrJson[j] == ',' || arrJson[j] == ' ')) j++;
        if (j >= arrJson.size() || arrJson[j] == ']') break;
        if (arrJson[j] != '"') break;
        items.push_back(JsonDecodeStringAt(arrJson, j));
    }
    // A drag on the seek/volume sliders pushes one entry per native
    // 'input' event — easily a dozen+ within one drain window — and each
    // one used to cost its own full synchronous CDP round-trip serialized
    // on g_cdpMx, so real playback visibly lagged behind the thumb during
    // a drag. Only the final value of each ever matters, so keep just the
    // last 'seek:'/'vol:' entry and drop the rest; every other action
    // (clicks, tweaks, css) still dispatches in order, unthrottled.
    int lastSeek = -1, lastVol = -1;
    for (int k = 0; k < (int)items.size(); k++) {
        if (items[k].rfind("seek:", 0) == 0) lastSeek = k;
        else if (items[k].rfind("vol:", 0) == 0) lastVol = k;
    }
    for (int k = 0; k < (int)items.size(); k++) {
        if ((items[k].rfind("seek:", 0) == 0 && k != lastSeek) ||
            (items[k].rfind("vol:", 0) == 0 && k != lastVol)) continue;
        DispatchCheatAction(items[k]);
    }
}

// Only playback actions cross the network bridge — tweak:/css: stay
// reachable solely through the hub's own UI (registry-backed, host-
// authoritative), never from an external process.
static bool IsBridgeAllowedAction(const std::string& item) {
    static const char* kExact[] = { "like", "dislike", "prev", "next", "toggle", "shuffle", "repeat" };
    for (const char* e : kExact) if (item == e) return true;
    if (item.rfind("seek:", 0) == 0) return true;
    if (item.rfind("vol:", 0) == 0) return true;
    return false;
}

static void HttpSendResponse(SOCKET s, int code, const char* status, const std::string& body) {
    char head[256];
    sprintf_s(head, "HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
        code, status, body.size());
    send(s, head, (int)strlen(head), 0);
    if (!body.empty()) send(s, body.data(), (int)body.size(), 0);
}

// Independent of the in-page cheat menu (works whether or not that's even
// enabled) — same selectors syncPlaying/syncPro already established, just
// queried fresh and packed into one JSON object via the page's own
// JSON.stringify rather than reading several separate DOM properties back.
static std::string CdpQueryStatusJson() {
    std::string js =
        "(function(){"
        "var pb=document.querySelector(\"[data-test-id='PLAYERBAR_DESKTOP']\")||"
        "document.querySelector(\"[data-test-id='VIBE_PLAYERBAR']\");"
        "if(!pb)return JSON.stringify({ok:false});"
        "function q(id){return pb.querySelector(\"[data-test-id='\"+id+\"']\");}"
        "var t=q('TRACK_TITLE')||q('VIBE_PLAYERBAR_TRACK_NAME');"
        "var a=q('SEPARATED_ARTIST_TITLE')||document.querySelector(\"[class*='VibePage_text__']\");"
        "var lk=q('LIKE_BUTTON');"
        "var shuf=pb.querySelector(\"[data-test-id^='SHUFFLE_BUTTON']\");"
        "var rep=pb.querySelector(\"[data-test-id^='REPEAT_BUTTON']\");"
        "var seekEl=q('TIMECODE_SLIDER')||q('VIBE_PLAYERBAR_TIMECODE_SLIDER');"
        "var volEl=q('CHANGE_VOLUME_SLIDER');"
        "var seekable=!!seekEl&&seekEl.tagName==='INPUT';"
        // Time is sent as already-formatted text (same TIMECODE_TIME_START/
        // END nodes the panel itself mirrors), not raw slider value/max —
        // that pair's actual unit was never confirmed to be seconds, so a
        // consumer re-deriving mm:ss from it would be guessing.
        "var posText='0:00',durText='0:00';"
        "if(seekable){var ts=q('TIMECODE_TIME_START'),te=q('TIMECODE_TIME_END');"
        "posText=ts?ts.textContent:posText;durText=te?te.textContent:durText;"
        "}else{var vtc=q('VIBE_PLAYERBAR_TIMECODE'),parts=vtc?vtc.textContent.split('/'):null;"
        "if(parts){posText=parts[0].trim();durText=parts[1].trim();}}"
        "return JSON.stringify({ok:true,"
        "title:t?(t.children[0]?t.children[0].textContent:t.textContent):'',"
        "artist:a?a.textContent:'',"
        "liked:!!lk&&lk.getAttribute('aria-pressed')==='true',"
        "playing:!!q('PAUSE_BUTTON'),"
        "shuffle:!!shuf&&shuf.getAttribute('data-test-id')!=='SHUFFLE_BUTTON',"
        "repeatOn:!!rep&&rep.getAttribute('data-test-id')!=='REPEAT_BUTTON_NO_REPEAT',"
        "seekable:seekable,posText:posText,durText:durText,"
        "seekPos:seekable?Number(seekEl.value):0,"
        "seekMax:seekable?Number(seekEl.getAttribute('max')||100):0,"
        "volume:volEl?Number(volEl.value):0});"
        "})()";
    std::lock_guard<std::mutex> lk(g_cdpMx);
    if (!CdpEnsureConnected()) return "{\"ok\":false}";
    std::string req = "{\"id\":11,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(js) + "\",\"returnByValue\":true}}";
    if (!CdpSend(req)) { CdpClose(); return "{\"ok\":false}"; }
    std::string resp;
    if (!CdpRecv(resp)) { CdpClose(); return "{\"ok\":false}"; }
    auto p = resp.find("\"value\":");
    if (p == std::string::npos) return "{\"ok\":false}";
    size_t i = p + 8;
    return JsonDecodeStringAt(resp, i);
}

// Loopback-only JSON bridge so an external process (a Lua script running
// inside a separate game, in this project's case) can read now-playing
// state and issue the same playback actions the in-page cheat menu already
// can — GET /status, POST /cmd with a raw action string body (the exact
// vocabulary DispatchCheatAction/window.__ymhubQ already use, e.g. "next"
// or "seek:42.5" — no JSON envelope, since both ends of this bridge are
// ours). Bound to 127.0.0.1 specifically, never 0.0.0.0 — nothing off-box
// can reach it, same trust boundary the CDP debug port itself already
// relies on elsewhere in this file. One request handled at a time, same
// "simple over clever" tradeoff as CdpQueueDrain's single eval per tick.
static DWORD WINAPI HttpBridgeThreadFn(LPVOID) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { WSACleanup(); return 1; }

    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(YMHUB_BRIDGE_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(listener, 8) == SOCKET_ERROR) {
        LogMsg("HTTP bridge: bind/listen failed, err=" + std::to_string(WSAGetLastError()));
        closesocket(listener); WSACleanup(); return 1;
    }
    LogMsg("HTTP bridge listening on 127.0.0.1:" + std::to_string(YMHUB_BRIDGE_PORT));

    while (g_run) {
        SOCKET c = accept(listener, nullptr, nullptr);
        if (c == INVALID_SOCKET) { if (!g_run) break; continue; }

        DWORD tv = 3000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        std::string req;
        char buf[4096];
        int n;
        while (req.find("\r\n\r\n") == std::string::npos &&
            (n = recv(c, buf, sizeof(buf), 0)) > 0) {
            req.append(buf, n);
        }
        size_t headEnd = req.find("\r\n\r\n");
        if (headEnd == std::string::npos) { closesocket(c); continue; }

        bool isPost = req.rfind("POST", 0) == 0;
        std::string path;
        size_t sp1 = req.find(' ');
        size_t sp2 = sp1 == std::string::npos ? std::string::npos : req.find(' ', sp1 + 1);
        if (sp1 != std::string::npos && sp2 != std::string::npos)
            path = req.substr(sp1 + 1, sp2 - sp1 - 1);

        std::string body = req.substr(headEnd + 4);
        if (isPost) {
            size_t clPos = req.find("Content-Length:");
            size_t want = clPos != std::string::npos ? (size_t)atoi(req.c_str() + clPos + 16) : 0;
            while (body.size() < want && (n = recv(c, buf, sizeof(buf), 0)) > 0) body.append(buf, n);
        }

        if (!isPost && path == "/status") {
            HttpSendResponse(c, 200, "OK", CdpQueryStatusJson());
        } else if (isPost && path == "/cmd") {
            if (IsBridgeAllowedAction(body)) {
                DispatchCheatAction(body);
                HttpSendResponse(c, 200, "OK", "{\"ok\":true}");
            } else {
                HttpSendResponse(c, 403, "Forbidden", "{\"ok\":false,\"error\":\"not allowed\"}");
            }
        } else {
            HttpSendResponse(c, 404, "Not Found", "{\"ok\":false,\"error\":\"not found\"}");
        }

        shutdown(c, SD_SEND);
        closesocket(c);
    }

    closesocket(listener);
    WSACleanup();
    return 0;
}

// Always on — this in-page menu (Shift inside YM) is now the only
// settings surface at all (no tray, no separate hub window), so it can't
// be an opt-in feature gated behind a toggle that used to live in the
// hub itself. See CdpInjectMenu above for what it renders.
static DWORD WINAPI CheatMenuThreadFn(LPVOID) {
    while (g_run) {
        // Minimizing/restoring, or just losing focus to another app going
        // fullscreen, races CDP traffic against the transition and
        // reliably crashes the whole host process (confirmed via crash-
        // dump analysis + isolated repro, both triggers reported live).
        // CdpUnsafeNow catches the change itself, not just steady-state
        // iconic — see its own comment.
        HWND main = FindMainWnd();
        EnsureTransitionWatch();
        bool danger = CdpUnsafeNow(main);
        if (danger) {
            // Pausing new polls wasn't enough — a *connected* CDP session
            // itself, not just an in-flight call, appears to be what's
            // fragile across a visibility transition (confirmed: even a
            // single realistic minimize still crashed with polls paused).
            // Fully drop the session so nothing about it is live across
            // the transition; CdpEnsureConnected() below re-attaches once
            // it's safe again.
            std::lock_guard<std::mutex> lk(g_cdpMx);
            CdpClose();
        } else if (CdpEnsureConnected()) {
            CdpInjectMenu(g_tweaksMask, g_customCss.c_str());
            CdpQueueDrain();
        }
        // Was 300ms — that's roughly 3x/sec of CDP traffic hitting the
        // renderer just to keep an already-injected menu current, which
        // is what made the minimize race so easy to hit in practice. The
        // menu doesn't need sub-second freshness; cutting the poll rate
        // directly cuts how often a call can be in flight when a
        // transition happens, which the guards above can't fully do
        // alone (the actual danger window is the renderer's own JS
        // execution time, not anything this thread's local timeout
        // controls).
        Sleep(1000);
    }
    return 0;
}

// ── Hotkey window ───────────────────────────────────────────────

// Convert host mods (1=Ctrl,2=Shift,4=Alt) to RegisterHotKey mods
static UINT HostToWinMods(DWORD m) {
    UINT r = MOD_NOREPEAT;
    if (m & 1) r |= MOD_CONTROL;
    if (m & 2) r |= MOD_SHIFT;
    if (m & 4) r |= MOD_ALT;
    return r;
}

static void RefreshHotkeys() {
    if (!g_hkWnd) return;
    // Unregister all current
    for (int i = 0; i < 6; i++) {
        if (g_regIds[i]) {
            UnregisterHotKey(g_hkWnd, i);
            g_regIds[i] = false;
        }
    }
    // Registry is the only source of truth for these now — no more IPC
    // struct anywhere (Фаза 5). WM_REFRESHHK is what wakes this up, posted
    // either from WorkerThread's own startup or from wherever ends up
    // actually rebinding a key next (see plan — no such UI exists yet).
    LoadKeys();
    // Register configured keys
    for (int i = 0; i < 6; i++) {
        DWORD vk = g_keys[i].vk;
        if (vk) {
            UINT mods = HostToWinMods(g_keys[i].mods);
            g_regIds[i] = !!RegisterHotKey(g_hkWnd, i, mods, vk);
            char b[96];
            sprintf_s(b, "RegisterHotKey id=%d mods=%u vk=%u -> %s (err=%lu)",
                i, mods, vk, g_regIds[i] ? "ok" : "FAIL", g_regIds[i] ? 0 : GetLastError());
            LogMsg(b);
        }
    }
}

static LRESULT CALLBACK HotkeyWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        int id = (int)(short)LOWORD(wp);
        { char b[32]; sprintf_s(b, "WM_HOTKEY id=%d", id); LogMsg(b); }
        if (id >= 0 && id < 6 && !g_rebinding) {
            static const DWORD cmds[6] = {
                YMHC_OVL_TOGGLE, YMHC_PREV, YMHC_NEXT,
                YMHC_TOGGLE, YMHC_LIKE, YMHC_DISLIKE
            };
            ExecCmd(cmds[id]);
        }
        return 0;
    }
    if (msg == WM_REFRESHHK) {
        RefreshHotkeys();
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

// Фаза 1: moved from main.cpp's LLKeyProc — detects the user's own
// hotkeys plus YM's native "Горячие клавиши" being shadowed by a remap,
// so the original default key can still be emitted via CDP
// (CdpSendYmKey) even though the real key was suppressed. Originally
// had an "if DLL not loaded yet, handle via host PostMessage instead"
// fallback branch — moot now that this hook only ever runs *as* the
// DLL, so the direct ExecCmd() path (same one HotkeyWndProc's own
// WM_HOTKEY handler already uses) is unconditional. FindMainWnd() (this
// file, finds a window belonging to the current process) replaces the
// host's FindYM() (which searched *other* processes' windows) — from
// inside the DLL, "the YM window" is just "our own process's window".
static LRESULT CALLBACK LLKeyProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) && !g_rebinding) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        bool sh   = (GetAsyncKeyState(VK_SHIFT)   & 0x8000) != 0;
        bool alt  = (GetAsyncKeyState(VK_MENU)    & 0x8000) != 0;
        DWORD mods = (ctrl ? 1u : 0u) | (sh ? 2u : 0u) | (alt ? 4u : 0u);
        for (int i = 0; i < 6; i++)
            if (g_keys[i].vk && mods == g_keys[i].mods && k->vkCode == g_keys[i].vk) {
                static const DWORD cmds[6] = {
                    YMHC_OVL_TOGGLE, YMHC_PREV, YMHC_NEXT,
                    YMHC_TOGGLE, YMHC_LIKE, YMHC_DISLIKE };
                ExecCmd(cmds[i]);
                return 1;
            }
        // YM's own native hotkeys — only act while YM's own window is
        // foreground (real, non-injected input); the translated keypress
        // itself still has to go out via CDP, see CdpSendYmKey.
        static HWND s_ymWin = nullptr; static ULONGLONG s_ymTick = 0;
        ULONGLONG now = GetTickCount64();
        if (now - s_ymTick > 500) { s_ymWin = FindMainWnd(); s_ymTick = now; }
        if (s_ymWin && GetForegroundWindow() == s_ymWin) {
            for (int i = 0; i < 13; i++)
                if (g_ymKeys[i].vk && mods == g_ymKeys[i].mods && k->vkCode == g_ymKeys[i].vk)
                    { CdpSendYmKey(i); return 1; }
            if (mods == 0)
                for (int i = 0; i < 13; i++)
                    if (k->vkCode == YM_DEFAULT_VK[i] && g_ymKeys[i].vk && g_ymKeys[i].vk != YM_DEFAULT_VK[i])
                        return 1;
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

static DWORD WINAPI HotkeyThread(LPVOID) {
    g_hkTid = GetCurrentThreadId();

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = HotkeyWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"YMHubHKWnd";
    RegisterClassExW(&wc); // may fail if class exists, that's OK

    g_hkWnd = CreateWindowExW(0, L"YMHubHKWnd", nullptr, 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hkWnd) return 1;

    LoadYmKeys();
    // WH_KEYBOARD_LL needs a message pump on the installing thread to
    // actually receive callbacks — this thread already has one (below),
    // unlike main.cpp's old host thread this is replacing which needed
    // no special justification since a GUI app's main thread always
    // pumps messages anyway.
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LLKeyProc, g_hInst, 0);
    LogMsg(g_hook ? "LLKeyProc hook installed" : "LLKeyProc hook FAILED");

    // Message loop — WM_HOTKEY is dispatched to HotkeyWndProc
    MSG m;
    while (g_run && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    for (int i = 0; i < 6; i++)
        if (g_regIds[i]) UnregisterHotKey(g_hkWnd, i);
    DestroyWindow(g_hkWnd);
    g_hkWnd = nullptr;
    return 0;
}

// Proactively connect to CDP and walk the three-stage status card inside
// YM's own window once the debug port is reachable (host may still be
// finishing the relaunch-with-flag dance at this point): checking ->
// success, or checking -> error if the page isn't the player we expect.
// 5 steps, each held on screen for a minimum stretch (STEP_MIN) rather
// than flashing by as fast as the underlying work actually completes —
// deliberately slower and more visible, on request. Every step still
// corresponds to something this thread genuinely does; the last two
// (settings, initial state) used to just happen silently a moment later
// via LogBadgeThread's own first 2s tick — pulled forward into this
// sequence so they're both real work AND visible progress instead of
// two separate things.
static const DWORD STEP_MIN_MS = 550;

static DWORD WINAPI CdpAnnounceThread(LPVOID) {
    DWORD stepStart;
    LogMsg("Connecting to CDP on port " + std::to_string(CdpPort()) + "...");

    stepStart = GetTickCount();
    CdpInitToastStep(1, 5, L"Поиск процесса Яндекс Музыки");
    for (int i = 0; i < 30 && g_run; i++) {
        if (CdpEnsureConnected()) break;
        Sleep(500);
    }
    if (!g_cdpWs) {
        LogMsg("CDP connect failed after 30 attempts");
        CdpInitToastDone(false, L"Не удалось подключиться к Яндекс Музыке");
        return 0;
    }
    LogMsg("CDP connected");
    DWORD elapsed = GetTickCount() - stepStart;
    if (elapsed < STEP_MIN_MS) Sleep(STEP_MIN_MS - elapsed);

    CdpInitToastStep(2, 5, L"Подключение к DevTools Protocol");
    Sleep(STEP_MIN_MS);

    CdpInitToastStep(3, 5, L"Проверка готовности плеера");
    stepStart = GetTickCount();
    WaitUntilSafeForCdp();
    bool ready = CdpVerifyPlayerReady();
    elapsed = GetTickCount() - stepStart;
    if (elapsed < STEP_MIN_MS) Sleep(STEP_MIN_MS - elapsed);
    if (!ready) {
        LogMsg("Player not found");
        CdpInitToastDone(false, L"Не удалось найти плеер Яндекс Музыки");
        return 0;
    }
    LogMsg("Player ready");

    CdpInitToastStep(4, 5, L"Применение сохранённых настроек");
    WaitUntilSafeForCdp();
    ApplyTweaksNow();
    Sleep(STEP_MIN_MS);

    CdpInitToastStep(5, 5, L"Оптимизация интерфейса");
    WaitUntilSafeForCdp();
    g_domLiked = CdpQueryLiked();
    g_domCoverUrl = CdpQueryCoverUrl();
    Sleep(STEP_MIN_MS);

    CdpInitToastDone(true, L"Подключено");
    return 0;
}

// Keeps the Settings-page log row present (while Settings is open) and its
// content current. Runs independently of CdpAnnounceThread so logs are
// visible even if the initial connect/verify sequence above failed.
static DWORD WINAPI LogBadgeThread(LPVOID) {
    while (g_run) {
        // Same transition guard as CheatMenuThreadFn above — this loop
        // hits the same CDP path just on a slower cadence, so it's just
        // as capable of racing a minimize or focus-loss transition.
        HWND main = FindMainWnd();
        EnsureTransitionWatch();
        bool danger = CdpUnsafeNow(main);
        if (danger) {
            std::lock_guard<std::mutex> lk(g_cdpMx);
            CdpClose();
        } else if (CdpEnsureConnected()) {
            CdpInjectSettingsLogRow(LogBlob());
            ApplyTweaksNow();
            g_domLiked = CdpQueryLiked();
            g_domCoverUrl = CdpQueryCoverUrl();
        }
        Sleep(2000);
    }
    return 0;
}

// ── Base64 (Фаза 2: album-art WIC pipeline, ported from main.cpp) ──
static const char B64T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64Enc(const BYTE* d, size_t n) {
    std::string r; r.reserve(((n + 2) / 3) * 4);
    for (size_t i = 0; i < n; i += 3) {
        DWORD v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        r += B64T[(v >> 18) & 0x3F]; r += B64T[(v >> 12) & 0x3F];
        r += (i + 1 < n) ? B64T[(v >> 6) & 0x3F] : '=';
        r += (i + 2 < n) ? B64T[v & 0x3F] : '=';
    }
    return r;
}

// Bounded wait on a WinRT async op — SMTC's broker can hang indefinitely
// on a flaky session, so every call here is polled with a hard timeout
// instead of a blocking .get(), same reasoning as the DLL's own WinHTTP
// timeout handling elsewhere.
template<typename Op>
static auto AwaitOrTimeout(Op const& op, int timeoutMs) -> decltype(op.GetResults()) {
    using winrt::Windows::Foundation::AsyncStatus;
    ULONGLONG start = GetTickCount64();
    while (op.Status() == AsyncStatus::Started) {
        if (GetTickCount64() - start > (ULONGLONG)timeoutMs) {
            op.Cancel();
            throw winrt::hresult_error(E_ABORT);
        }
        Sleep(20);
    }
    return op.GetResults();
}

// ── Track state (Фаза 2: SMTC polling, ported from main.cpp) ──
static std::wstring  g_track, g_artist;
static bool          g_playing = false;
static bool          g_liked = false;
static std::string   g_artB64;
static std::wstring  g_artTrackKey;
static std::atomic<bool> g_artReady{ false };
static std::string       g_artPending;
static CRITICAL_SECTION  g_artCS;
static volatile bool     g_artFetching = false;
static volatile bool     g_parsing = false;

static void LoadArtAsync(winrt::Windows::Storage::Streams::IRandomAccessStreamReference ref, std::wstring trackKey) {
    std::thread([ref, trackKey]()mutable {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::string b64;
        // Thumbnail reads can hang or fail transiently (same flaky SMTC
        // broker as elsewhere, plus art that isn't cached locally yet) —
        // bound the wait and retry a couple of times before giving up.
        for (int attempt = 0; attempt < 3 && b64.empty(); attempt++) {
            if (attempt > 0)Sleep(400);
            try {
                auto openOp = ref.OpenReadAsync();
                auto ras = AwaitOrTimeout(openOp, 3000);
                if (!ras)continue;
                winrt::com_ptr<IStream> cs;
                if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(ras), IID_PPV_ARGS(cs.put()))))throw 0;
                IWICImagingFactory* wic = nullptr;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
                if (!wic)throw 0;
                IWICBitmapDecoder* dec = nullptr;
                wic->CreateDecoderFromStream(cs.get(), nullptr, WICDecodeMetadataCacheOnLoad, &dec);
                if (!dec) { wic->Release(); throw 0; }
                IWICBitmapFrameDecode* fr = nullptr; dec->GetFrame(0, &fr); dec->Release();
                if (!fr) { wic->Release(); throw 0; }
                IWICBitmapScaler* sc2 = nullptr; wic->CreateBitmapScaler(&sc2);
                sc2->Initialize(fr, 128, 128, WICBitmapInterpolationModeFant);
                IWICFormatConverter* cv = nullptr; wic->CreateFormatConverter(&cv);
                cv->Initialize(sc2, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
                fr->Release(); sc2->Release();
                UINT w = 0, h = 0; cv->GetSize(&w, &h);
                std::vector<BYTE> px(w * h * 4);
                cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data());
                cv->Release(); wic->Release();
                // WIC outputs BGRA bytes; swap R<->B so PNG is correct RGBA
                for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);
                IWICImagingFactory* wic2 = nullptr;
                CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic2));
                IWICStream* ws = nullptr; wic2->CreateStream(&ws);
                IStream* ms = nullptr; CreateStreamOnHGlobal(nullptr, TRUE, &ms);
                ws->InitializeFromIStream(ms);
                IWICBitmapEncoder* enc = nullptr; wic2->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
                enc->Initialize(ws, WICBitmapEncoderNoCache);
                IWICBitmapFrameEncode* fe = nullptr; IPropertyBag2* opts = nullptr;
                enc->CreateNewFrame(&fe, &opts); fe->Initialize(opts);
                fe->SetSize(w, h); WICPixelFormatGUID pf = GUID_WICPixelFormat32bppRGBA;
                fe->SetPixelFormat(&pf);
                fe->WritePixels(h, w * 4, (UINT)px.size(), px.data());
                fe->Commit(); enc->Commit();
                HGLOBAL hg = nullptr; GetHGlobalFromStream(ms, &hg);
                SIZE_T sz = GlobalSize(hg); void* ptr = GlobalLock(hg);
                b64 = Base64Enc((BYTE*)ptr, sz); GlobalUnlock(hg);
                fe->Release(); enc->Release(); if (opts)opts->Release();
                ws->Release(); ms->Release(); wic2->Release();
            } catch (...) { b64 = ""; }
        }
        g_artFetching = false;
        // Drop the result if the user already moved to a different track
        // while this (possibly slow/retried) fetch was in flight, so a
        // late failure can't blank out art that's already correct, and a
        // late success can't paint the wrong track's cover.
        if (g_artTrackKey != trackKey)return;
        if (b64.empty())return; // let the next track-poll tick retry instead of latching empty
        EnterCriticalSection(&g_artCS);
        g_artPending = b64; g_artReady = true;
        LeaveCriticalSection(&g_artCS);
        if (g_hwnd)PostMessageW(g_hwnd, WM_APP + 3, 0, 0);
        }).detach();
}

// The actual SMTC fetch, including the timeout-bounded waits. Runs off
// a dedicated thread (see ParseYM below) so the up-to-3s worst case from
// AwaitOrTimeout never blocks anything else in the DLL.
static void ParseYMWork() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HWND hw = g_hwnd;
    bool justChangedTrack = false;
    try {
        auto mgrOp = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        auto mgr = AwaitOrTimeout(mgrOp, 1500);
        if (mgr) {
            wmc::GlobalSystemMediaTransportControlsSession ym = nullptr;
            for (auto s : mgr.GetSessions())
                if (s.SourceAppUserModelId() == L"ru.yandex.desktop.music") { ym = s; break; }
            if (!ym)ym = mgr.GetCurrentSession();
            if (ym) {
                auto propOp = ym.TryGetMediaPropertiesAsync();
                auto p = AwaitOrTimeout(propOp, 1500);
                if (p && !p.Title().empty()) {
                    std::wstring track = p.Title().c_str();
                    g_track = track; g_artist = p.Artist().c_str();
                    g_playing = (ym.GetPlaybackInfo().PlaybackStatus() ==
                        wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    bool changed = (g_artTrackKey != track);
                    if (changed) { g_artTrackKey = track; g_artB64 = ""; g_liked = false; justChangedTrack = true; }
                    // Retry every poll while art is still missing for the
                    // current track — SMTC's Thumbnail() is sometimes not
                    // populated yet on the very first poll after a track
                    // change, and without this it never got fetched again.
                    if (g_artB64.empty() && !g_artFetching && p.Thumbnail()) {
                        g_artFetching = true;
                        LoadArtAsync(p.Thumbnail(), track);
                    }
                }
            }
        }
    } catch (...) {}
    // Resync with the DLL's own CDP-polled liked state (LogBadgeThread) —
    // corrects the optimistic toggle in DoLike()/DoDislike() (Фаза 4) if
    // it ever drifts, and picks up likes made outside the hub entirely
    // (YM's own UI, remapped native hotkeys, or a track already liked on
    // load). Skipped right on a track change: CDP polls independently on
    // its own ~2s cadence, so g_domLiked can still hold the *previous*
    // track's state for up to that long — applying it here would flash
    // the old status before that poll catches up, instead of the correct
    // assume-unliked default set above.
    if (!justChangedTrack) g_liked = g_domLiked;
    g_parsing = false;
    if (hw)PostMessageW(hw, WM_APP + 27, 0, 0);
}

static void ParseYM() {
    if (g_parsing)return;
    g_parsing = true;
    std::thread(ParseYMWork).detach();
}

// ── JSON helpers (Фаза 2) ──────────────────────────────────
static std::wstring JsonEsc(const std::wstring& s) {
    std::wstring r; for (wchar_t c : s) {
        if (c == L'"')r += L"\\\"";
        else if (c == L'\\')r += L"\\\\";
        else if (c == L'\n')r += L"\\n";
        else r += c;
    }
    return r;
}
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty())return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), r.data(), n, nullptr, nullptr);
    return r;
}

// ── Discord Rich Presence (Фаза 2, ported from main.cpp) ────
// No SDK, just Discord's local RPC pipe protocol: connect to
// \\.\pipe\discord-ipc-N (N=0..9), send a one-time handshake frame, then
// a SET_ACTIVITY frame whenever the track changes. Each frame is an
// 8-byte header (int32 opcode, int32 length) followed by that many bytes
// of JSON, all through overlapped reads/writes with a short timeout — a
// hung or unresponsive Discord must not be able to stall this thread.
static const char* DISCORD_CLIENT_ID = "1518944722310266911";
static HANDLE g_discordPipe = nullptr;
static HANDLE g_discordEvt = nullptr;
static bool   g_discordEnabled = false;
static time_t g_discordStart = 0;
static std::wstring g_discordLastTrack, g_discordLastArtist, g_discordLastCover;
static bool g_discordWasSent = false;

static void LoadDiscordSetting() {
    g_discordEnabled = RegGetDW(HKEY_CURRENT_USER, REG_APP, L"DiscordRpc", 0) != 0;
}
static void SaveDiscordSetting(bool on) {
    g_discordEnabled = on;
    RegSetDW(HKEY_CURRENT_USER, REG_APP, L"DiscordRpc", on ? 1 : 0);
}

static void DiscordClose() {
    if (g_discordPipe) { CloseHandle(g_discordPipe); g_discordPipe = nullptr; }
}

static bool DiscordIo(bool isWrite, void* buf, DWORD len, DWORD timeoutMs) {
    if (!g_discordPipe)return false;
    OVERLAPPED ov = {}; ov.hEvent = g_discordEvt; ResetEvent(g_discordEvt);
    BOOL ok = isWrite ? WriteFile(g_discordPipe, buf, len, nullptr, &ov) : ReadFile(g_discordPipe, buf, len, nullptr, &ov);
    if (!ok) {
        if (GetLastError() != ERROR_IO_PENDING)return false;
        if (WaitForSingleObject(g_discordEvt, timeoutMs) != WAIT_OBJECT_0) { CancelIoEx(g_discordPipe, &ov); return false; }
    }
    DWORD xferred = 0;
    return GetOverlappedResult(g_discordPipe, &ov, &xferred, FALSE) && xferred == len;
}

static bool DiscordSendFrame(int opcode, const std::string& json) {
    BYTE hdr[8]; DWORD len = (DWORD)json.size();
    memcpy(hdr, &opcode, 4); memcpy(hdr + 4, &len, 4);
    if (!DiscordIo(true, hdr, 8, 300))return false;
    if (len && !DiscordIo(true, (void*)json.data(), len, 300))return false;
    return true;
}

// Measured empirically: Discord's very first handshake reply (the READY
// dispatch, carrying its whole user/config blob) can take ~850ms — way
// past what a steady-state ack needs — so the handshake recv gets its own
// much longer allowance than regular frames.
static bool DiscordRecvFrame(DWORD timeoutMs = 500) {
    BYTE hdr[8];
    if (!DiscordIo(false, hdr, 8, timeoutMs))return false;
    DWORD len; memcpy(&len, hdr + 4, 4);
    if (len > 65536)return false;
    std::string body(len, '\0');
    if (len && !DiscordIo(false, body.data(), len, timeoutMs))return false;
    return true;
}

static bool DiscordConnect() {
    if (g_discordPipe)return true;
    if (!g_discordEvt)g_discordEvt = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (int i = 0; i < 10; i++) {
        wchar_t path[64]; swprintf_s(path, L"\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) { g_discordPipe = h; break; }
    }
    if (!g_discordPipe)return false; // Discord isn't running — not an error, just nothing to do yet
    std::string hs = std::string("{\"v\":1,\"client_id\":\"") + DISCORD_CLIENT_ID + "\"}";
    if (!DiscordSendFrame(0, hs) || !DiscordRecvFrame(3000)) { DiscordClose(); return false; }
    return true;
}

static void DiscordClearActivity() {
    if (!DiscordConnect())return;
    std::string activity = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"activity\":null},\"nonce\":\"ymhub-clear\"}";
    if (!DiscordSendFrame(1, activity) || !DiscordRecvFrame())DiscordClose();
}

// Called every ~2s from a dedicated thread (DiscordThreadFn) — track/
// artist/playing are read without synchronization, same as other cross-
// thread state in this DLL; a torn read here just means presence updates
// a tick late.
static void DiscordTick() {
    if (!g_discordEnabled) {
        if (g_discordWasSent) { DiscordClearActivity(); g_discordWasSent = false; g_discordLastTrack.clear(); g_discordLastCover.clear(); }
        return;
    }
    if (!g_playing || g_track.empty()) {
        if (g_discordWasSent) { DiscordClearActivity(); g_discordWasSent = false; g_discordLastTrack.clear(); g_discordLastCover.clear(); }
        return;
    }
    std::wstring cover = g_domCoverUrl;
    if (g_track == g_discordLastTrack && g_artist == g_discordLastArtist && cover == g_discordLastCover && g_discordWasSent)
        return; // nothing actually changed — don't spam SET_ACTIVITY
    if (g_track != g_discordLastTrack || g_artist != g_discordLastArtist)g_discordStart = time(nullptr);
    if (!DiscordConnect())return;
    std::string details = ToUtf8(JsonEsc(g_track));
    std::string state = ToUtf8(JsonEsc(g_artist));
    std::string assets;
    if (!cover.empty())
        assets = ",\"assets\":{\"large_image\":\"" + ToUtf8(JsonEsc(cover)) + "\",\"large_text\":\"" + details + "\"}";
    std::string activity =
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId()) +
        ",\"activity\":{\"type\":2,\"details\":\"" + details + "\",\"state\":\"" + state + "\","
        "\"timestamps\":{\"start\":" + std::to_string((long long)g_discordStart) + "}" + assets + ","
        "\"instance\":false}},\"nonce\":\"ymhub-set\"}";
    if (DiscordSendFrame(1, activity) && DiscordRecvFrame()) {
        g_discordLastTrack = g_track; g_discordLastArtist = g_artist; g_discordLastCover = cover; g_discordWasSent = true;
    } else DiscordClose(); // reconnect from scratch next tick
}

static DWORD WINAPI DiscordThreadFn(LPVOID) {
    while (g_run) { DiscordTick(); Sleep(2000); }
    return 0;
}

// Drives ParseYM on the same ~2s cadence the host used to via its overlay
// window's TIMER_TRACK (OnTrackTick) — now a standalone thread since no
// window exists yet to own a Win32 timer (Фаза 4 adds one; BroadcastState,
// OnTrackTick's other half, is UI-only and stays out of scope until then).
static DWORD WINAPI TrackThread(LPVOID) {
    while (g_run) { ParseYM(); Sleep(2000); }
    return 0;
}

// ── Overlay window (Фаза 4a: mini-player, ported from main.cpp) ──
// Mini-player only for now — hub/settings window is Фаза 4b. g_hub/
// g_hubCtrl/g_hubWv already exist as the eventual hook-up point;
// BroadcastState/SendHub* below null-guard them so this compiles and
// runs correctly before that window exists.
static const int CW = 420, CH = 188;
enum class Pos { BL = 0, BC = 1, BR = 2, TL = 3, TC = 4, TR = 5 };
static Pos   g_pos = Pos::BC;
static bool  g_customPos = false;
static int   g_posX = 0, g_posY = 0;
static bool  g_visible = false;
static int   g_scrW = 0, g_scrH = 0;
static DWORD g_uiTid = 0;
static HWND  g_hub = nullptr; // Фаза 4b

static ComPtr<ICoreWebView2Environment> g_env;
static ComPtr<ICoreWebView2Controller>  g_ctrl;
static ComPtr<ICoreWebView2>            g_wv;
static bool g_wvInited = false;
static ComPtr<ICoreWebView2Controller>  g_hubCtrl; // Фаза 4b
static ComPtr<ICoreWebView2>            g_hubWv;   // Фаза 4b

// ── Position helper ───────────────────────────────────────
static void GetCardPos(int& x, int& y) {
    if (g_customPos) { x = g_posX; y = g_posY; return; }
    const int M = 60; int pi = (int)g_pos;
    x = (pi == 0 || pi == 3) ? M : (pi == 1 || pi == 4) ? (g_scrW - CW) / 2 : g_scrW - CW - M;
    y = (pi >= 3) ? M : g_scrH - CH - M;
}

static void BroadcastState() {
    std::wstring art(g_artB64.begin(), g_artB64.end());
    std::wstring msg =
        L"{\"type\":\"state\""
        L",\"track\":\"" + JsonEsc(g_track) + L"\""
        L",\"artist\":\"" + JsonEsc(g_artist) + L"\""
        L",\"playing\":" + (g_playing ? L"true" : L"false") +
        L",\"liked\":" + (g_liked ? L"true" : L"false") +
        L",\"art\":\"" + art + L"\"" +
        L",\"dllLoaded\":true" // tautological from inside the DLL itself
        L",\"overlayVisible\":" + (g_visible ? L"true" : L"false") +
        L",\"pos\":" + std::to_wstring((int)g_pos) +
        L",\"drpcEnabled\":" + (g_discordEnabled ? L"true" : L"false") +
        L",\"drpcConnected\":" + (g_discordPipe ? L"true" : L"false") +
        L",\"cheatMenuEnabled\":" + (g_cheatMenuEnabled ? L"true" : L"false") +
        L",\"ver\":\"" YMHUB_VERSION_W L"\""
        L"}";
    if (g_wv)    g_wv->PostWebMessageAsString(msg.c_str());
    if (g_hubWv) g_hubWv->PostWebMessageAsString(msg.c_str());
}
static void SendHubKeys() {
    if (!g_hubWv) return;
    wchar_t buf[512];
    swprintf_s(buf,
        L"{\"type\":\"init-keys\",\"keys\":["
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u},"
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u},"
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u}]}",
        g_keys[0].mods, g_keys[0].vk, g_keys[1].mods, g_keys[1].vk,
        g_keys[2].mods, g_keys[2].vk, g_keys[3].mods, g_keys[3].vk,
        g_keys[4].mods, g_keys[4].vk, g_keys[5].mods, g_keys[5].vk);
    g_hubWv->PostWebMessageAsString(buf);
}
static void SendHubYmKeys() {
    if (!g_hubWv) return;
    wchar_t buf[768] = L"{\"type\":\"init-ymkeys\",\"keys\":[";
    for (int i = 0; i < 13; i++) {
        wchar_t part[48];
        swprintf_s(part, L"%s{\"m\":%u,\"v\":%u}", i ? L"," : L"", g_ymKeys[i].mods, g_ymKeys[i].vk);
        wcscat_s(buf, part);
    }
    wcscat_s(buf, L"]}");
    g_hubWv->PostWebMessageAsString(buf);
}
static void SendHubTweaks() {
    if (!g_hubWv) return;
    std::wstring msg = L"{\"type\":\"init-tweaks\",\"mask\":" + std::to_wstring(g_tweaksMask) +
        L",\"customName\":\"" + JsonEsc(g_customName) + L"\""
        L",\"customCss\":\"" + JsonEsc(g_customCss) + L"\"}";
    g_hubWv->PostWebMessageAsString(msg.c_str());
}

// ── Media / commands (Фаза 4a: direct ExecCmd call, no more IPC
// round-trip needed — the hub UI and the overlay UI are now the same
// process as CDP/CdpClickButton) ──
static void DoPrev() { ExecCmd(YMHC_PREV); Sleep(220); ParseYM(); BroadcastState(); }
static void DoNext() { ExecCmd(YMHC_NEXT); Sleep(220); ParseYM(); BroadcastState(); }
static void DoToggle() { ExecCmd(YMHC_TOGGLE); g_playing = !g_playing; BroadcastState(); }
static void DoLike() { ExecCmd(YMHC_LIKE); g_liked = !g_liked; BroadcastState(); }
static void DoDislike() { ExecCmd(YMHC_DISLIKE); g_liked = false; Sleep(220); ParseYM(); BroadcastState(); }

// ── Overlay HTML (Фаза 4a, ported from main.cpp verbatim) ──
static const wchar_t* HTML = LR"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;overflow:hidden;
  font-family:'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;
  background:#0e0e12;-webkit-app-region:drag;}
#bg1,#bg2{
  position:absolute;inset:-20px;
  background-size:cover;background-position:center;
  filter:blur(30px) brightness(.36) saturate(1.7);
  transform:scale(1.09);will-change:opacity;transition:opacity .6s ease;}
#bg2{opacity:0;}
#shell{
  position:absolute;inset:0;display:flex;flex-direction:column;
  animation:popIn .3s cubic-bezier(.34,1.5,.64,1) both;}
#shell.out{animation:popOut .2s cubic-bezier(.55,0,1,.45) both;}
@keyframes popIn{0%{opacity:0;transform:scale(.88) translateY(12px)}65%{transform:scale(1.02) translateY(-2px)}100%{opacity:1;transform:scale(1) translateY(0)}}
@keyframes popOut{0%{opacity:1;transform:scale(1) translateY(0)}100%{opacity:0;transform:scale(.9) translateY(10px)}}
#top{display:flex;align-items:center;padding:14px 16px 8px;gap:14px;flex:1;}
#art-wrap{position:relative;flex-shrink:0;width:84px;height:84px;border-radius:10px;overflow:hidden;
  box-shadow:0 8px 28px rgba(0,0,0,.6);transition:box-shadow .4s;}
#art-wrap.playing{box-shadow:0 8px 28px rgba(0,0,0,.6),0 0 0 2px var(--ac,#fff3);
  animation:artPulse 2.4s ease-in-out infinite;}
@keyframes artPulse{0%,100%{box-shadow:0 8px 28px rgba(0,0,0,.6),0 0 0 2px var(--ac,#fff3)}50%{box-shadow:0 8px 28px rgba(0,0,0,.6),0 0 0 4px var(--ac,#fff1)}}
#art-img{width:100%;height:100%;object-fit:cover;display:block;opacity:0;transition:opacity .35s ease;}
#art-ph{position:absolute;inset:0;display:flex;align-items:center;justify-content:center;
  background:rgba(255,255,255,.06);font-size:30px;color:rgba(255,255,255,.25);transition:opacity .3s;}
#eq-wrap{position:absolute;inset:0;background:rgba(0,0,0,.45);
  display:flex;align-items:flex-end;justify-content:center;
  padding-bottom:12px;gap:3.5px;opacity:0;transition:opacity .3s;}
#eq-wrap.on{opacity:1;}
.bar{width:3.5px;border-radius:3px;background:rgba(255,255,255,.92);height:3px;
  animation:barA .7s ease-in-out infinite alternate;}
.bar:nth-child(1){animation-name:barA;animation-duration:.52s;animation-delay:0s}
.bar:nth-child(2){animation-name:barB;animation-duration:.81s;animation-delay:.11s}
.bar:nth-child(3){animation-name:barA;animation-duration:.63s;animation-delay:.06s}
.bar:nth-child(4){animation-name:barB;animation-duration:.74s;animation-delay:.19s}
@keyframes barA{0%{height:3px}100%{height:16px}}
@keyframes barB{0%{height:5px}100%{height:20px}}
.bar.p{animation-play-state:paused;}
#info{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:center;gap:4px;overflow:hidden;}
#ym-label{font-size:9px;font-weight:700;letter-spacing:1.4px;text-transform:uppercase;
  color:var(--ac,rgba(255,255,255,.4));transition:color .5s;}
#track-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
  font-size:17px;font-weight:700;color:#fff;line-height:1.25;text-shadow:0 1px 8px rgba(0,0,0,.4);}
#artist-name{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;
  font-size:12.5px;color:rgba(255,255,255,.48);}
.slide-in{animation:slideIn .22s cubic-bezier(.25,.8,.25,1) both;}
.slide-out{animation:slideOut .18s ease-in both;}
@keyframes slideIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:translateY(0)}}
@keyframes slideOut{from{opacity:1;transform:translateY(0)}to{opacity:0;transform:translateY(-8px)}}
#sep{height:1px;margin:0 16px;
  background:linear-gradient(90deg,transparent,rgba(255,255,255,.1) 40%,rgba(255,255,255,.1) 60%,transparent);}
#ctrls{display:flex;align-items:center;justify-content:center;gap:6px;padding:6px 0 12px;}
.cb{border:none;cursor:pointer;border-radius:50%;
  display:flex;align-items:center;justify-content:center;
  -webkit-app-region:no-drag;flex-shrink:0;position:relative;overflow:hidden;
  transition:transform .15s,background .15s,color .15s;}
.cb::after{content:'';position:absolute;inset:0;border-radius:50%;
  background:rgba(255,255,255,.25);transform:scale(0);opacity:1;
  transition:transform .35s ease,opacity .35s ease;}
.cb:active::after{transform:scale(2.2);opacity:0;transition:none;}
.skip{width:36px;height:36px;background:rgba(255,255,255,.08);color:rgba(255,255,255,.65);}
.skip:hover{background:rgba(255,255,255,.16);color:#fff;transform:scale(1.07);}
.skip:active{transform:scale(.88);}
#pbtn{width:46px;height:46px;background:var(--ac,#ffffffef);color:#000;
  box-shadow:0 4px 20px rgba(0,0,0,.4);
  transition:transform .15s,filter .15s,background .5s,box-shadow .5s;}
#pbtn:hover{filter:brightness(1.15);transform:scale(1.06);}
#pbtn:active{filter:brightness(.86);transform:scale(.91);}
#pbtn.pop{animation:btnPop .22s cubic-bezier(.34,1.6,.64,1);}
@keyframes btnPop{0%{transform:scale(1)}45%{transform:scale(.8)}100%{transform:scale(1)}}
.cb svg{pointer-events:none;}
#btn-like{width:32px;height:32px;background:rgba(255,255,255,.06);color:rgba(255,255,255,.4);transition:all .2s;}
#btn-like:hover{background:rgba(255,80,100,.18);color:rgba(255,100,120,.9);transform:scale(1.1);}
#btn-like.liked{background:rgba(255,60,90,.22);color:#ff4d6d;animation:heartPop .28s cubic-bezier(.34,1.6,.64,1);}
@keyframes heartPop{0%{transform:scale(1)}40%{transform:scale(1.35)}100%{transform:scale(1)}}
#btn-dislike{width:32px;height:32px;background:rgba(255,255,255,.06);color:rgba(255,255,255,.3);}
#btn-dislike:hover{background:rgba(255,255,255,.12);color:rgba(255,255,255,.7);transform:scale(1.07);}
.ctrl-sep{width:1px;height:22px;background:rgba(255,255,255,.1);margin:0 2px;flex-shrink:0;}
</style></head><body>
<div id="bg1"></div><div id="bg2"></div>
<div id="shell">
  <div id="top">
    <div id="art-wrap">
      <div id="art-ph">♪</div>
      <img id="art-img">
      <div id="eq-wrap">
        <div class="bar p"></div><div class="bar p"></div>
        <div class="bar p"></div><div class="bar p"></div>
      </div>
    </div>
    <div id="info">
      <div id="ym-label">Яндекс Музыка</div>
      <div id="track-name">Нет воспроизведения</div>
      <div id="artist-name"></div>
    </div>
  </div>
  <div id="sep"></div>
  <div id="ctrls">
    <button class="cb skip" id="btn-prev" onclick="send('prev')">
      <svg width="15" height="15" viewBox="0 0 15 15" fill="currentColor">
        <rect x="1.5" y="1.5" width="2.5" height="12" rx="1.1"/>
        <path d="M13 2.5 5.5 7.5 13 12.5V2.5z"/>
      </svg>
    </button>
    <button class="cb" id="pbtn" onclick="onPlay()">
      <svg id="i-play" width="17" height="17" viewBox="0 0 17 17" fill="currentColor">
        <path d="M5.5 3.5 14 8.5 5.5 13.5V3.5z"/>
      </svg>
      <svg id="i-pause" width="17" height="17" viewBox="0 0 17 17" fill="currentColor" style="display:none">
        <rect x="3.5" y="3" width="3.2" height="11" rx="1.3"/>
        <rect x="10.3" y="3" width="3.2" height="11" rx="1.3"/>
      </svg>
    </button>
    <button class="cb skip" id="btn-next" onclick="send('next')">
      <svg width="15" height="15" viewBox="0 0 15 15" fill="currentColor">
        <rect x="11" y="1.5" width="2.5" height="12" rx="1.1"/>
        <path d="M2 2.5l7.5 5L2 12.5V2.5z"/>
      </svg>
    </button>
    <div class="ctrl-sep"></div>
    <button class="cb" id="btn-like" onclick="onLike()" title="Лайк">
      <svg width="15" height="15" viewBox="0 0 24 24" fill="currentColor">
        <path d="M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"/>
      </svg>
    </button>
    <button class="cb" id="btn-dislike" onclick="send('dislike')" title="Дизлайк">
      <svg width="14" height="14" viewBox="0 0 24 24" fill="currentColor">
        <path d="M15 3H6c-.83 0-1.54.5-1.84 1.22l-3.02 7.05c-.09.23-.14.47-.14.73v2c0 1.1.9 2 2 2h6.31l-.95 4.57-.03.32c0 .41.17.79.44 1.06L9.83 23l6.59-6.59c.36-.36.58-.86.58-1.41V5c0-1.1-.9-2-2-2zm4 0v12h4V3h-4z"/>
      </svg>
    </button>
  </div>
</div>
<canvas id="cv" style="display:none" width="200" height="200"></canvas>
<script>
const $=id=>document.getElementById(id);
function send(a){window.chrome.webview.postMessage(a)}
document.body.addEventListener('mousedown',e=>{
  if(e.button===0&&!e.target.closest('.cb'))send('drag');});
function onPlay(){const b=$('pbtn');b.classList.remove('pop');void b.offsetWidth;b.classList.add('pop');send('toggle');}
function onLike(){$('btn-like').classList.toggle('liked');send('like');}
function setText(el,txt){
  if(el.textContent===txt)return;
  el.classList.remove('slide-in','slide-out');void el.offsetWidth;
  el.classList.add('slide-out');
  setTimeout(()=>{el.textContent=txt;el.classList.remove('slide-out');el.classList.add('slide-in');},160);}
function extractAccent(img){
  const cv=$('cv'),ctx=cv.getContext('2d');
  ctx.drawImage(img,0,0,128,128);
  let sR=0,sG=0,sB=0,wt=0;
  for(let x=4;x<124;x+=8){for(let y=4;y<124;y+=8){
    const d=ctx.getImageData(x,y,1,1).data;
    const r=d[0],g=d[1],b=d[2];
    const mx=Math.max(r,g,b),mn=Math.min(r,g,b);
    const sat=mx>0?(mx-mn)/mx:0;
    const lig=(mx+mn)/510;
    if(lig>0.08&&lig<0.93&&sat>0.15){
      const w=sat*sat;sR+=r*w;sG+=g*w;sB+=b*w;wt+=w;}
  }}
  let R,G,B;
  if(wt<0.5){R=91;G=143;B=255;}
  else{const r=sR/wt|0,g=sG/wt|0,b=sB/wt|0;
    const mx=Math.max(r,g,b)||1,k=Math.min(255/mx,1.9);
    R=Math.min(255,r*k|0);G=Math.min(255,g*k|0);B=Math.min(255,b*k|0);}
  document.documentElement.style.setProperty('--ac',`rgb(${R},${G},${B})`);}
let bgActive=1;
function setBg(src){
  const next=bgActive===1?$('bg2'):$('bg1'),curr=bgActive===1?$('bg1'):$('bg2');
  next.style.backgroundImage=`url(${src})`;next.style.opacity='1';curr.style.opacity='0';
  bgActive=bgActive===1?2:1;}
function loadArt(src){
  const img=$('art-img');img.style.opacity='0';$('art-ph').style.opacity='0';
  const tmp=new Image();
  tmp.onload=()=>{
    img.src=src;img.style.transition='none';img.style.transform='scale(.85)';void img.offsetWidth;
    img.style.transition='opacity .3s ease,transform .35s cubic-bezier(.34,1.4,.64,1)';
    img.style.opacity='1';img.style.transform='scale(1)';extractAccent(tmp);setBg(src);};
  tmp.src=src;}
let lastArt='',lastPlaying=null,lastLiked=null;
window.chrome.webview.addEventListener('message',e=>{
  const d=JSON.parse(e.data);
  if(d.type==='hide'){$('shell').classList.add('out');setTimeout(()=>send('hidden'),210);return;}
  if(d.type==='show'){
    const s=$('shell');s.classList.remove('out');s.style.animation='none';
    void s.offsetWidth;s.style.animation='';return;}
  if(d.type!=='state')return;
  setText($('track-name'),d.track||'Нет воспроизведения');
  setText($('artist-name'),d.artist||'');
  const playing=!!d.playing;
  if(playing!==lastPlaying){lastPlaying=playing;
    $('i-play').style.display=playing?'none':'';$('i-pause').style.display=playing?'':'none';
    $('art-wrap').classList.toggle('playing',playing);}
  const liked=!!d.liked;
  if(liked!==lastLiked){lastLiked=liked;$('btn-like').classList.toggle('liked',liked);}
  document.querySelectorAll('.bar').forEach(b=>b.classList.toggle('p',!playing));
  $('eq-wrap').classList.toggle('on',playing&&!!d.art);
  if(d.art&&d.art!==lastArt){lastArt=d.art;loadArt('data:image/png;base64,'+d.art);}
  else if(!d.art&&lastArt){lastArt='';$('art-img').style.opacity='0';
    setTimeout(()=>{$('art-ph').style.opacity='1';},320);
    $('bg1').style.backgroundImage='none';$('bg2').style.backgroundImage='none';
    document.documentElement.style.setProperty('--ac','rgba(255,255,255,.7)');}
});
</script></body></html>)HTML";

static void SetupController(ICoreWebView2Controller* ctrl, ICoreWebView2* wv, bool isHub) {
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(ctrl->QueryInterface(IID_PPV_ARGS(&c2))))
        c2->put_DefaultBackgroundColor({ 0,0,0,0 });
    ComPtr<ICoreWebView2Settings> stt; wv->get_Settings(&stt);
    if (stt) {
        stt->put_AreDefaultContextMenusEnabled(FALSE);
        stt->put_IsStatusBarEnabled(FALSE);
        stt->put_AreDevToolsEnabled(FALSE);
    }
    (void)isHub;
}

// Фаза 4b will add the hub controller here too — deliberately not yet,
// so a null g_hub is never handed to CreateCoreWebView2Controller.
static void InitWebView() {
    // Separate profile dir from main.cpp's own "YMHub.WebView2" — this
    // DLL and a still-running old-architecture host can otherwise both be
    // alive at once during the migration, and two WebView2 environments
    // can't safely share one profile directory.
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring userDataFolder = std::wstring(tmp) + L"YMHubDll.WebView2";
    // The completion handler below only ever runs if this call actually
    // manages to *start* the async environment creation. If the WebView2
    // Runtime isn't installed/reachable at all, this can fail synchronously
    // instead (returns here without ever invoking the handler) -- silently,
    // with neither "ready" nor "FAILED" ever reaching the log, and the
    // already-shown native overlay window just sitting there empty forever.
    // Confirmed missing in a real report: a log with WM_HOTKEY firing
    // correctly for the overlay toggle but zero WebView2 log lines at all.
    HRESULT hrStart = CreateCoreWebView2EnvironmentWithOptions(nullptr, userDataFolder.c_str(), nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT hr, ICoreWebView2Environment* env) -> HRESULT {
                if (!env) {
                    char b[80]; sprintf_s(b, "WebView2 environment creation FAILED: 0x%08lX", (unsigned long)hr);
                    LogMsg(b);
                    return S_OK;
                }
                g_env = env;
                env->CreateCoreWebView2Controller(g_hwnd,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT hr, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (!ctrl) {
                                char b[80]; sprintf_s(b, "WebView2 controller creation FAILED: 0x%08lX", (unsigned long)hr);
                                LogMsg(b);
                                return S_OK;
                            }
                            g_ctrl = ctrl; ctrl->get_CoreWebView2(&g_wv);
                            SetupController(ctrl, g_wv.Get(), false);
                            g_wv->add_WebMessageReceived(
                                Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                        LPWSTR s = nullptr; a->TryGetWebMessageAsString(&s);
                                        if (s) {
                                            if (wcscmp(s, L"prev") == 0)         PostMessageW(g_hwnd, WM_APP + 10, 0, 0);
                                            else if (wcscmp(s, L"next") == 0)    PostMessageW(g_hwnd, WM_APP + 11, 0, 0);
                                            else if (wcscmp(s, L"toggle") == 0)  PostMessageW(g_hwnd, WM_APP + 12, 0, 0);
                                            else if (wcscmp(s, L"like") == 0)    PostMessageW(g_hwnd, WM_APP + 13, 0, 0);
                                            else if (wcscmp(s, L"dislike") == 0) PostMessageW(g_hwnd, WM_APP + 14, 0, 0);
                                            else if (wcscmp(s, L"hidden") == 0)  PostMessageW(g_hwnd, WM_APP + 21, 0, 0);
                                            // Standard borderless-window drag trick — see the
                                            // mousedown listener in HTML above.
                                            else if (wcscmp(s, L"drag") == 0) {
                                                ReleaseCapture();
                                                SendMessage(g_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                                            }
                                            CoTaskMemFree(s);
                                        }
                                        return S_OK;
                                    }).Get(), nullptr);
                            RECT r = { 0,0,CW,CH }; ctrl->put_Bounds(r);
                            g_wv->add_NavigationCompleted(
                                Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                    [](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs*) -> HRESULT {
                                        PostMessageW(g_hwnd, WM_APP + 25, 0, 0); return S_OK;
                                    }).Get(), nullptr);
                            g_wv->NavigateToString(HTML);
                            LogMsg("Overlay WebView2 ready");
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hrStart)) {
        char b[64]; sprintf_s(b, "CreateCoreWebView2EnvironmentWithOptions failed synchronously: 0x%08lX", (unsigned long)hrStart);
        LogMsg(b);
    }
}

static void ShowOverlay() {
    if (!g_visible) {
        g_visible = true;
        int x, y; GetCardPos(x, y);
        SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, CW, CH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
        if (!g_wvInited) { g_wvInited = true; InitWebView(); }
        else {
            if (g_ctrl) { RECT r = { 0,0,CW,CH }; g_ctrl->put_Bounds(r); g_ctrl->put_IsVisible(TRUE); }
            if (g_wv) g_wv->PostWebMessageAsString(L"{\"type\":\"show\"}");
            ParseYM(); BroadcastState();
        }
    } else {
        g_visible = false;
        if (g_wv) g_wv->PostWebMessageAsString(L"{\"type\":\"hide\"}");
        else ShowWindow(g_hwnd, SW_HIDE);
    }
    BroadcastState();
}

static LRESULT CALLBACK OverlayWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(hw, &ps); EndPaint(hw, &ps); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_APP + 10: DoPrev();    return 0;
    case WM_APP + 11: DoNext();    return 0;
    case WM_APP + 12: DoToggle();  return 0;
    case WM_APP + 13: DoLike();    return 0;
    case WM_APP + 14: DoDislike(); return 0;
    case WM_APP + 20: ShowOverlay(); return 0;
    case WM_APP + 27: BroadcastState(); return 0; // ParseYM worker finished
    case WM_APP + 21:
        // Fully tear down the WebView2 controller here instead of just
        // hiding it — this is a *second*, separate Chromium/Edge engine
        // coexisting in the same process as YM's own, and confirmed live
        // (via a proper control test disabling it entirely) to be the
        // actual cause of the minimize/focus-loss crash, not the CDP-
        // based in-page menu this was originally suspected to be. Tying
        // its lifetime to "currently shown" instead of "ever shown once
        // this session" keeps the exposure window to just that, rather
        // than the whole rest of the process's life after first use.
        if (g_ctrl) { g_ctrl->Close(); g_ctrl = nullptr; }
        g_wv = nullptr;
        g_wvInited = false;
        ShowWindow(hw, SW_HIDE);
        g_visible = false;
        BroadcastState();
        return 0;
    case WM_APP + 25: ParseYM(); BroadcastState(); return 0;
    case WM_APP + 26: // Фаза 4b: hub nav complete → send initial state + keys
        ParseYM(); BroadcastState(); SendHubKeys(); SendHubYmKeys(); SendHubTweaks(); return 0;
    // Fires once when an interactive move ends — see main.cpp's own
    // identical comment; drag itself is driven by WebView2/Chromium
    // honoring -webkit-app-region:drag (see HTML above).
    case WM_EXITSIZEMOVE: {
        RECT r; GetWindowRect(hw, &r);
        g_customPos = true; g_posX = r.left; g_posY = r.top;
        RegSetDW(HKEY_CURRENT_USER, REG_APP, L"PosCustom", 1);
        RegSetDW(HKEY_CURRENT_USER, REG_APP, L"PosX", (DWORD)g_posX);
        RegSetDW(HKEY_CURRENT_USER, REG_APP, L"PosY", (DWORD)g_posY);
        return 0;
    }
    case WM_APP + 31: // Фаза 4b: pos change from hub — picking a preset
        // always wins over a previous drag.
    {
        int n = (int)wp; if (n >= 0 && n <= 5) {
            g_pos = (Pos)n;
            g_customPos = false;
            RegSetDW(HKEY_CURRENT_USER, REG_APP, L"Pos", (DWORD)g_pos);
            RegSetDW(HKEY_CURRENT_USER, REG_APP, L"PosCustom", 0);
            if (g_visible) {
                int x, y; GetCardPos(x, y);
                SetWindowPos(g_hwnd, HWND_TOPMOST, x, y, CW, CH, SWP_NOACTIVATE);
            }
            BroadcastState();
        }
        return 0;
    }
    case WM_APP + 3:
        EnterCriticalSection(&g_artCS);
        if (g_artReady.load()) { g_artB64 = g_artPending; g_artPending.clear(); g_artReady = false; }
        LeaveCriticalSection(&g_artCS);
        BroadcastState();
        return 0;
    }
    return DefWindowProcW(hw, msg, wp, lp);
}

static DWORD WINAPI UiThread(LPVOID) {
    g_uiTid = GetCurrentThreadId();
    // WebView2's environment creation is COM-backed and fails outright
    // with CO_E_NOTINITIALIZED without this — main.cpp's wWinMain got it
    // for free from its own winrt::init_apartment() near process startup;
    // this thread needs its own since it's the one actually calling into
    // WebView2, not the thread DllMain ran on.
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    LoadTweaks();
    g_pos = (Pos)RegGetDW(HKEY_CURRENT_USER, REG_APP, L"Pos", (DWORD)Pos::BC);
    if ((int)g_pos < 0 || (int)g_pos > 5) g_pos = Pos::BC;
    g_customPos = RegGetDW(HKEY_CURRENT_USER, REG_APP, L"PosCustom", 0) != 0;
    g_posX = (int)RegGetDW(HKEY_CURRENT_USER, REG_APP, L"PosX", 0);
    g_posY = (int)RegGetDW(HKEY_CURRENT_USER, REG_APP, L"PosY", 0);
    g_scrW = GetSystemMetrics(SM_CXSCREEN);
    g_scrH = GetSystemMetrics(SM_CYSCREEN);

    WNDCLASSEXW wc = { sizeof(wc),0,OverlayWndProc,0,0,g_hInst,
        nullptr,nullptr,(HBRUSH)GetStockObject(NULL_BRUSH),nullptr,L"YMHubOvl",nullptr };
    RegisterClassExW(&wc);
    g_hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"YMHubOvl", L"YMHub Overlay", WS_POPUP,
        0, 0, CW, CH, nullptr, nullptr, g_hInst, nullptr);
    LogMsg(g_hwnd ? "Overlay window created" : "Overlay window creation FAILED");
    if (g_hwnd) {
        BOOL dark = TRUE; DwmSetWindowAttribute(g_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
        DWM_WINDOW_CORNER_PREFERENCE cp = DWMWCP_ROUND;
        DwmSetWindowAttribute(g_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));
    }

    // WebView2 is created lazily now, on first ShowOverlay() call (see
    // its own comment, and WM_APP+21's) — not eagerly here. A second
    // Chromium/Edge engine sitting in this process for the DLL's entire
    // lifetime, whether or not the mini-player was ever opened, is
    // exactly what a live control test pinned as the actual cause of
    // the minimize/focus-loss crash.

    MSG m;
    while (g_run && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return 0;
}

// ── Worker thread ───────────────────────────────────────────────
// Used to also own an IPC shared-memory struct and poll it for commands
// from the host — that host doesn't exist anymore (Фаза 5), and every
// one of those commands now arrives through a direct call instead
// (ExecCmd from hotkeys/HTTP-bridge/in-page menu, SaveKeys/SaveTweaks
// calling straight into RefreshHotkeys/CdpApplyTweaks). What's left is
// just: advertise the DLL's presence via the named mutex Forge checks
// before deciding whether to inject, spin up every other subsystem's
// thread, and do the hotkeys' one-time initial registration.
static DWORD WINAPI WorkerThread(LPVOID) {
    InstallCrashHandler();
    g_mutex = CreateMutexW(nullptr, TRUE, YMH_MUTEX_NAME);
    LogMsg("DLL attached, pid=" + std::to_string(GetCurrentProcessId()));
    InitializeCriticalSection(&g_artCS);
    LoadDiscordSetting();
    // TEMP CONTROL TEST round 2: CDP threads back on, mini-player's own
    // WebView2 (UiThread, disabled separately below) still off — isolating
    // whether the mini-player alone is sufficient, without also needing
    // CDP disabled, now that round 1 pointed at it instead of CDP.
    CreateThread(nullptr, 0, CdpAnnounceThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, LogBadgeThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, CheatMenuThreadFn, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, HttpBridgeThreadFn, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, TrackThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, DiscordThreadFn, nullptr, 0, nullptr);

    // Wait for hotkey window to be ready (up to 2 s), then do initial registration
    for (int i = 0; i < 200 && !g_hkWnd && g_run; i++) Sleep(10);
    if (g_hkWnd) PostMessageW(g_hkWnd, WM_REFRESHHK, 0, 0);

    while (g_run) Sleep(200); // keep the mutex held for the DLL's lifetime

    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
    return 0;
}

// Phase 0 of the host->DLL migration (see plan) proved live that a
// WebView2 controller can be created and rendered from a thread inside
// this DLL after CreateRemoteThread+LoadLibrary injection into Yandex
// Music's already-running process — confirmed via a throwaway popup
// window with a ticking JS counter (77+ ticks across two separate fresh
// injections), a correctly-parented msedgewebview2.exe subprocess tree,
// and clean teardown with no orphaned processes when Yandex Music was
// killed. That scratch code has been removed now that the question is
// answered; src/dll/CMakeLists.txt keeps the WebView2 SDK linkage this
// proved out, ready for the real overlay/hub window port (Фаза 4).

BOOL WINAPI DllMain(HINSTANCE hInst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hInst;
        g_run = true;
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, WorkerThread,  nullptr, 0, nullptr);
        CreateThread(nullptr, 0, UiThread,      nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        // Wake up hotkey/UI message loops so they can exit
        if (g_hkTid) PostThreadMessageW(g_hkTid, WM_QUIT, 0, 0);
        if (g_uiTid) PostThreadMessageW(g_uiTid, WM_QUIT, 0, 0);
    }
    return TRUE;
}
