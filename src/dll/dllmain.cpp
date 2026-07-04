#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include "../shared/ipc.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "ws2_32.lib")

#define YM_CDP_PORT 9876

// Loopback-only bridge port for external tools (e.g. a game overlay script)
// to read now-playing state / issue playback actions — see HttpBridgeThreadFn.
#define YMHUB_BRIDGE_PORT 47990

static HANDLE        g_hMem       = nullptr;
static YMHubIPC*     g_ipc        = nullptr;
static HANDLE        g_mutex      = nullptr;
static volatile bool g_run        = false;
static LONG          g_lastCmdSeq = 0;
static LONG          g_lastYmSendSeq = 0;
static LONG          g_lastTweaksSeq = 0;

// Hotkey thread state
static HWND          g_hkWnd      = nullptr;
static DWORD         g_hkTid      = 0;
static LONG          g_lastKeySeq = -1;
static bool          g_regIds[6]  = {};

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

// Host writes the actual port into IPC (it may have had to pick a
// fallback if YM_CDP_PORT was already taken by something else).
static int CdpPort() {
    if (g_ipc && g_ipc->cdpPort) return (int)g_ipc->cdpPort;
    return YM_CDP_PORT;
}

static std::string CdpUtf8(const wchar_t* w) {
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return "";
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
    return s;
}

// ── In-memory log, surfaced as a row in YM's own Settings page (see
// LogBadgeThread / CdpInjectSettingsLogRow) so the user can see what the
// DLL is doing without attaching a debugger. Also mirrored to a file in %TEMP%
// so logs survive across injections.
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
    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"YMHubDll.log";
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
    // Overlay toggle -> notify host
    if (cmd == YMHC_OVL_TOGGLE) {
        if (g_ipc && g_ipc->hostHwnd)
            PostMessageW(g_ipc->hostHwnd, WM_APP + 20, 0, 0);
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

// ── In-page "cheat menu" overlay (beta) ──────────────────────────
// Full YMHub UI rendered directly inside YM's own page instead of a
// separate WebView2 window — toggled by tapping Shift alone while YM
// has focus (a deliberate *tap*, not held: a keydown immediately
// followed by another key's keydown before Shift's keyup is treated as
// a real Shift+letter combo and ignored, so normal capitalized typing
// elsewhere on the page never false-triggers it). Player controls reuse
// ExecCmd directly (no IPC round-trip needed — this *is* the process
// that already executes those commands); tweaks/CSS additionally relay
// to the host via reqSeq/reqText purely so registry persistence and the
// standalone hub window stay in sync (see HandleUiMessage in main.cpp).
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

// Tears down the overlay's DOM/style/listeners entirely instead of just
// leaving them inert — a disabled "feature" that still has a live
// document-wide keydown/keyup hook installed wouldn't really be off.
static void CdpRemoveMenu() {
    std::string js =
        "(function(){"
        "var r=document.getElementById('ymhub-cheat');if(r)r.remove();"
        "var s=document.getElementById('ymhub-cheat-style');if(s)s.remove();"
        "if(window.__ymhubShiftDown){"
        "document.removeEventListener('keydown',window.__ymhubShiftDown,true);"
        "document.removeEventListener('keyup',window.__ymhubShiftUp,true);"
        "window.__ymhubShiftDown=null;window.__ymhubShiftUp=null;}"
        "if(window.__ymhubSyncTimer){clearInterval(window.__ymhubSyncTimer);window.__ymhubSyncTimer=null;}"
        "})()";
    CdpRunJs(js);
}

static void CdpInjectMenu(DWORD mask, const wchar_t* customCssW) {
    std::wstring js;
    js += L"(function(){"
        L"var ROOT=document.getElementById('ymhub-cheat');"
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
        L"#ymhub-cheat .yc-rail-adv{width:26px;height:16px;border-radius:99px;flex-shrink:0;margin-bottom:6px;"
        L"background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.08);position:relative;cursor:pointer;"
        L"transition:background .3s ease,border-color .3s ease;}"
        L"#ymhub-cheat .yc-rail-adv.on{background:linear-gradient(135deg,#5b8fff,#7c6fff);border-color:transparent;}"
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
        L"#ymhub-cheat .yc-tw-switch{position:relative;width:32px;height:19px;flex-shrink:0;border-radius:99px;"
        L"background:rgba(255,255,255,.12);border:1px solid rgba(255,255,255,.08);cursor:pointer;"
        L"transition:background .35s ease,border-color .35s ease;}"
        L"#ymhub-cheat .yc-tw-switch.on,#ymhub-cheat .yc-rail-adv.on"
        L"{background:linear-gradient(135deg,#5b8fff,#7c6fff);border-color:transparent;}"
        L"#ymhub-cheat .yc-knob{position:absolute;top:2px;left:2px;width:13px;height:13px;border-radius:50%;"
        L"background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.4);transition:left .35s cubic-bezier(.34,1.56,.64,1);}"
        L"#ymhub-cheat .yc-tw-switch.on .yc-knob{left:17px;}"
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
        L"window.__ymhubSyncTimer=setInterval(function(){syncPlaying();syncPro();},700);syncPlaying();syncPro();"
        L"}"
        L"var cssBox=document.getElementById('yc-css');"
        L"if(document.activeElement!==cssBox)cssBox.value=window.__ymhubCss||'';"
        L"for(var i=0;i<9;i++){var sw=document.getElementById('yc-tw-'+i);"
        L"if(sw)sw.classList.toggle('on',!!(window.__ymhubMask&(1<<i)));}"
        L"})()";
    std::string preamble =
        "window.__ymhubTwLabels=[";
    for (int i = 0; i < 9; i++) {
        if (i) preamble += ",";
        preamble += "\"" + CdpJsonEscape(CdpUtf8(kTweakLabels[i])) + "\"";
    }
    preamble += "];window.__ymhubMask=" + std::to_string(mask) + ";"
        "window.__ymhubCss=\"" + CdpJsonEscape(CdpUtf8(customCssW ? customCssW : L"")) + "\";";
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
// that needs no IPC hop. Tweaks/CSS apply immediately (CdpApplyTweaks)
// for instant visual feedback *and* relay to the host (reqSeq/reqText)
// purely so registry persistence and the standalone hub window agree —
// same message text HandleUiMessage already parses, just one source.
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
    if (!g_ipc) return;
    if (item.rfind("tweak:", 0) == 0) {
        int idx = atoi(item.c_str() + 6);
        if (idx < 0 || idx >= 9) return;
        DWORD newMask = g_ipc->tweaksMask ^ (1u << idx);
        CdpApplyTweaks(newMask, g_ipc->customCss);
        std::wstring relay = L"toggle-tweak:" + std::to_wstring(idx);
        wcsncpy_s(g_ipc->reqText, relay.c_str(), _TRUNCATE);
        InterlockedIncrement(&g_ipc->reqSeq);
        return;
    }
    if (item.rfind("css:", 0) == 0) {
        std::string cssUtf8 = item.substr(4);
        int wlen = MultiByteToWideChar(CP_UTF8, 0, cssUtf8.c_str(), (int)cssUtf8.size(), nullptr, 0);
        std::wstring cssW(wlen, 0);
        if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, cssUtf8.c_str(), (int)cssUtf8.size(), cssW.data(), wlen);
        CdpApplyTweaks(g_ipc->tweaksMask, cssW.c_str());
        std::wstring relay = L"set-custom-css:" + cssW;
        wcsncpy_s(g_ipc->reqText, relay.c_str(), _TRUNCATE);
        InterlockedIncrement(&g_ipc->reqSeq);
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

static DWORD WINAPI CheatMenuThreadFn(LPVOID) {
    bool wasEnabled = false;
    while (g_run) {
        if (g_ipc && CdpEnsureConnected()) {
            bool enabled = g_ipc->cheatMenuEnabled != 0;
            if (enabled) {
                CdpInjectMenu(g_ipc->tweaksMask, g_ipc->customCss);
                CdpQueueDrain();
            } else if (wasEnabled) {
                CdpRemoveMenu(); // just turned off — tear down what's there
            }
            wasEnabled = enabled;
        }
        Sleep(300);
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
    if (!g_ipc) return;
    // Register configured keys
    for (int i = 0; i < 6; i++) {
        DWORD vk = g_ipc->keys[i].vk;
        if (vk) {
            UINT mods = HostToWinMods(g_ipc->keys[i].mods);
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
        if (id >= 0 && id < 6 && (!g_ipc || !g_ipc->rebinding)) {
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

    // Message loop — WM_HOTKEY is dispatched to HotkeyWndProc
    MSG m;
    while (g_run && GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

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
    if (g_ipc) {
        CdpApplyTweaks(g_ipc->tweaksMask, g_ipc->customCss);
        bool hideName = (g_ipc->tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0;
        CdpApplyNameHide(hideName, g_ipc->customName);
        CdpApplyPassportNameHide(hideName, g_ipc->customName);
    }
    Sleep(STEP_MIN_MS);

    CdpInitToastStep(5, 5, L"Оптимизация интерфейса");
    if (g_ipc) {
        g_ipc->ymLiked = CdpQueryLiked() ? 1 : 0;
        wcsncpy_s(g_ipc->coverUrl, CdpQueryCoverUrl().c_str(), _TRUNCATE);
    }
    Sleep(STEP_MIN_MS);

    CdpInitToastDone(true, L"Подключено");
    return 0;
}

// Keeps the Settings-page log row present (while Settings is open) and its
// content current. Runs independently of CdpAnnounceThread so logs are
// visible even if the initial connect/verify sequence above failed.
static DWORD WINAPI LogBadgeThread(LPVOID) {
    while (g_run) {
        if (CdpEnsureConnected()) {
            CdpInjectSettingsLogRow(LogBlob());
            if (g_ipc) {
                CdpApplyTweaks(g_ipc->tweaksMask, g_ipc->customCss);
                CdpApplyNameHide((g_ipc->tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0, g_ipc->customName);
                CdpApplyPassportNameHide((g_ipc->tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0, g_ipc->customName);
                g_ipc->ymLiked = CdpQueryLiked() ? 1 : 0;
                wcsncpy_s(g_ipc->coverUrl, CdpQueryCoverUrl().c_str(), _TRUNCATE);
            }
        }
        Sleep(2000);
    }
    return 0;
}

// ── Worker thread ───────────────────────────────────────────────
static DWORD WINAPI WorkerThread(LPVOID) {
    // Open or create shared memory
    g_hMem = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, YMH_SHMEM_NAME);
    if (!g_hMem)
        g_hMem = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
            0, sizeof(YMHubIPC), YMH_SHMEM_NAME);
    if (!g_hMem) return 1;
    g_ipc = (YMHubIPC*)MapViewOfFile(g_hMem, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(YMHubIPC));
    if (!g_ipc) { CloseHandle(g_hMem); g_hMem = nullptr; return 1; }

    // Advertise DLL presence via named mutex
    g_mutex = CreateMutexW(nullptr, TRUE, YMH_MUTEX_NAME);
    LogMsg("DLL attached, pid=" + std::to_string(GetCurrentProcessId()));
    CreateThread(nullptr, 0, CdpAnnounceThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, LogBadgeThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, CheatMenuThreadFn, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, HttpBridgeThreadFn, nullptr, 0, nullptr);

    g_lastCmdSeq = g_ipc->cmdSeq;
    g_lastKeySeq = g_ipc->keySeq;
    g_lastYmSendSeq = g_ipc->ymSendSeq;
    g_lastTweaksSeq = g_ipc->tweaksSeq;

    // Wait for hotkey window to be ready (up to 2 s), then do initial registration
    for (int i = 0; i < 200 && !g_hkWnd && g_run; i++) Sleep(10);
    if (g_hkWnd) PostMessageW(g_hkWnd, WM_REFRESHHK, 0, 0);

    while (g_run) {
        // Handle explicit commands from hub/overlay
        LONG seq = InterlockedCompareExchange(&g_ipc->cmdSeq, 0, 0);
        if (seq != g_lastCmdSeq) {
            g_lastCmdSeq = seq;
            ExecCmd(g_ipc->command);
            InterlockedIncrement(&g_ipc->ack);
        }
        // Handle hotkey config changes
        LONG ks = InterlockedCompareExchange(&g_ipc->keySeq, 0, 0);
        if (ks != g_lastKeySeq) {
            g_lastKeySeq = ks;
            if (g_hkWnd) PostMessageW(g_hkWnd, WM_REFRESHHK, 0, 0);
        }
        // Host's keyboard hook detected a remapped YM hotkey — emit the
        // original default key via CDP (see CdpSendYmKey above).
        LONG ys = InterlockedCompareExchange(&g_ipc->ymSendSeq, 0, 0);
        if (ys != g_lastYmSendSeq) {
            g_lastYmSendSeq = ys;
            CdpSendYmKey(g_ipc->ymSendIdx);
        }
        // Tweak toggled in the hub — apply immediately rather than waiting
        // for LogBadgeThread's next 2s tick.
        LONG ts = InterlockedCompareExchange(&g_ipc->tweaksSeq, 0, 0);
        if (ts != g_lastTweaksSeq) {
            g_lastTweaksSeq = ts;
            CdpApplyTweaks(g_ipc->tweaksMask, g_ipc->customCss);
            CdpApplyNameHide((g_ipc->tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0, g_ipc->customName);
            CdpApplyPassportNameHide((g_ipc->tweaksMask & (1u << TWEAK_HIDE_NAME)) != 0, g_ipc->customName);
        }
        Sleep(15);
    }

    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
    if (g_ipc)   { UnmapViewOfFile(g_ipc); g_ipc = nullptr; }
    if (g_hMem)  { CloseHandle(g_hMem);   g_hMem = nullptr; }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_run = true;
        CreateThread(nullptr, 0, HotkeyThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, WorkerThread,  nullptr, 0, nullptr);
    } else if (reason == DLL_PROCESS_DETACH) {
        g_run = false;
        // Wake up hotkey message loop so it can exit
        if (g_hkTid) PostThreadMessageW(g_hkTid, WM_QUIT, 0, 0);
    }
    return TRUE;
}
