#pragma once
#include <Windows.h>

// Named mutex only — "is the DLL already loaded into some process" is the
// one thing that still needs to be checked from outside that process
// (Forge, before deciding whether to inject). Everything else that used
// to live in a YMHubIPC shared-memory struct here collapsed into either
// plain process-local globals or registry values once host and DLL
// became the same process (see ymhub's migration plan,
// sprightly-twirling-salamander, "Реестр вместо IPC-структуры") — the
// one registry value with no other home, CdpPort, is read directly via
// RegGetDW in dllmain.cpp, not declared here.
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

// Hotkey mods encoding: 1=Ctrl, 2=Shift, 4=Alt
struct IPCKey {
    DWORD mods;
    DWORD vk;
};

// Bit indices for the "Твики" tweak mask — matches kTweakRules order in
// dllmain.cpp and kTweakLabels' order in the in-page menu.
// TWEAK_WAVE_PILL ("Моя волна обновилась" pill) removed -- Yandex Music
// dropped that element from their own UI, so the tweak had nothing left to
// hide. Everything after it shifted down a slot; existing saved
// g_tweaksMask bits for users who had TWEAK_EXTRA_NAV..TWEAK_HIDE_NAME
// toggled will point at a different tweak after this update (a one-time,
// low-stakes remap -- easy to notice and re-toggle from the menu).
enum {
    TWEAK_AI_WORDS    = 0, // AI-комментарии о треке (искра под плеером)
    TWEAK_VIBE_ANIM   = 1, // Анимация фона плеера
    TWEAK_RELEASE_PIN = 2, // Плашка "Версия приложения" / что нового
    TWEAK_WHEEL       = 3, // Барабан рекомендаций слева (плейлисты-карточки)
    TWEAK_EXTRA_NAV   = 4, // "Для вас и Тренды" / "Концерты" / "Книги и подкасты"
    TWEAK_PLUS_BADGE  = 5, // ссылка/плашка подписки Плюс рядом с именем профиля
    TWEAK_BIG_COVER   = 6, // крупная обложка трека на странице "Моя волна"
    TWEAK_HIDE_NAME   = 7, // подменяет имя пользователя на свой текст / "Скрыто"
    TWEAK_COUNT       = 8,
};

// Index order for the DLL's own YM-native-hotkey remap table
// (g_ymKeys/YM_DEFAULT_VK in dllmain.cpp) — mirrors YM's own "Горячие
// клавиши" list top to bottom.
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
