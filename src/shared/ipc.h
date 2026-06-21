#pragma once
#include <Windows.h>

#define YMH_SHMEM_NAME  L"YMHub_IPC_v2"
#define YMH_MUTEX_NAME  L"YMHub_Loaded_v1"

enum : DWORD {
    YMHC_NONE        = 0,
    YMHC_LIKE        = 1,
    YMHC_DISLIKE     = 2,
    YMHC_PREV        = 3,
    YMHC_NEXT        = 4,
    YMHC_TOGGLE      = 5,
    YMHC_SHUFFLE     = 6,
    YMHC_REPEAT      = 7,
    YMHC_OVL_TOGGLE  = 8,
};

// Hotkey mods encoding: 1=Ctrl, 2=Shift, 4=Alt  (same as g_keys in main.cpp)
struct IPCKey {
    DWORD mods;
    DWORD vk;
};

struct YMHubIPC {
    // Host -> DLL: explicit commands (hub/overlay button clicks)
    volatile LONG  cmdSeq;      // host increments each new command
    volatile DWORD command;     // YMHC_*
    volatile LONG  ack;         // DLL increments after executing

    // Host -> DLL: hotkey config
    volatile LONG  keySeq;      // incremented whenever keys[] changes
    IPCKey         keys[6];     // [0]=ovl-toggle [1]=prev [2]=next [3]=toggle [4]=like [5]=dislike
    HWND           hostHwnd;    // host overlay HWND for DLL->host PostMessage

    // Rebind state: host sets TRUE while rebind UI is open, DLL skips hotkeys
    volatile BOOL  rebinding;

    // Host -> DLL: which port YM's --remote-debugging-port was launched
    // with (0 until the host has confirmed/opened it). DLL uses this for
    // its CDP client instead of a hardcoded port, since the host may have
    // had to fall back to a different port if the default was busy.
    volatile DWORD cdpPort;

    // Host -> DLL: "please send YM's default key for action ymSendIdx"
    // (see YM_ACTION_* below). The host's own foreground-gated low-level
    // keyboard hook (LLKeyProc/g_ymKeys in main.cpp) detects the user's
    // remapped key and the suppression of a remapped-away default key —
    // both of those work fine on real (non-injected) input. But actually
    // *emitting* the translated key has to happen via the DLL's CDP
    // connection using Input.dispatchKeyEvent, not host-side SendInput:
    // Chromium/Electron only treats page-dispatched JS KeyboardEvents and
    // SendInput-synthesized OS input as untrusted (confirmed empirically —
    // neither toggled playback, while a real keypress and CDP's own
    // Input.dispatchMouseEvent, already used for like/dislike, both work),
    // whereas CDP's Input domain is explicitly trusted input as far as
    // Chromium is concerned.
    volatile LONG  ymSendSeq;
    volatile DWORD ymSendIdx;

    // Host -> DLL: UI declutter toggles ("Твики" hub tab) — bit i set means
    // hide the element(s) matching kTweakSelectors[i] in dllmain.cpp. Pure
    // CSS injection (display:none via a <style> tag), so it's reversible
    // and doesn't touch YM's own state/behavior, just what's rendered.
    volatile LONG  tweaksSeq;
    volatile DWORD tweaksMask;
};

// Bit indices into YMHubIPC::tweaksMask — matches kTweakSelectors order in
// dllmain.cpp and the row order in the hub's "Твики" tab.
enum {
    TWEAK_AI_WORDS    = 0, // AI-комментарии о треке (искра под плеером)
    TWEAK_VIBE_ANIM   = 1, // Анимация фона плеера
    TWEAK_RELEASE_PIN = 2, // Плашка "Версия приложения" / что нового
    TWEAK_WHEEL       = 3, // Барабан рекомендаций слева (плейлисты-карточки)
    TWEAK_WAVE_PILL   = 4, // Плашка "Моя волна обновилась" в углу
    TWEAK_EXTRA_NAV   = 5, // "Для вас и Тренды" / "Концерты" / "Книги и подкасты"
    TWEAK_COUNT       = 6,
};

// Index order for YMHubIPC::ymSendIdx and the matching default-key table
// in dllmain.cpp — mirrors YM's own "Горячие клавиши" list top to bottom.
enum {
    YM_ACTION_TOGGLE   = 0,  // play/pause       — default K
    YM_ACTION_MUTE     = 1,  // mute/unmute      — default M
    YM_ACTION_SEEK_FWD = 2,  // seek forward     — default L
    YM_ACTION_SEEK_BCK = 3,  // seek backward    — default J
    YM_ACTION_VOL_UP   = 4,  // volume up        — default ↑
    YM_ACTION_VOL_DOWN = 5,  // volume down      — default ↓
    YM_ACTION_LIKE     = 6,  // like             — default F
    YM_ACTION_DISLIKE  = 7,  // dislike          — default D
    YM_ACTION_REPEAT   = 8,  // toggle repeat    — default R
    YM_ACTION_SHUFFLE  = 9,  // toggle shuffle   — default S
    YM_ACTION_NEXT     = 10, // next track       — default N
    YM_ACTION_PREV     = 11, // previous track   — default P
    YM_ACTION_FULLSCR  = 12, // fullscreen player — default W
    YM_ACTION_COUNT    = 13,
};
