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
};
