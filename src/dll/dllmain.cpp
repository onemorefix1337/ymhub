#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#include <string>
#include <vector>
#include <mutex>
#include <cstdlib>
#include <cstdio>
#include "../shared/ipc.h"

#pragma comment(lib, "winhttp.lib")

#define YM_CDP_PORT 9876

static HANDLE        g_hMem       = nullptr;
static YMHubIPC*     g_ipc        = nullptr;
static HANDLE        g_mutex      = nullptr;
static volatile bool g_run        = false;
static LONG          g_lastCmdSeq = 0;

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

static void CdpClose() {
    if (g_cdpWs) { WinHttpWebSocketClose(g_cdpWs, 1000, nullptr, 0); WinHttpCloseHandle(g_cdpWs); g_cdpWs = nullptr; }
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
    if (!CdpEnsureConnected()) return;
    std::string req = "{\"id\":9,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":\"" +
        CdpJsonEscape(jsUtf8) + "\"}}";
    if (!CdpSend(req)) { CdpClose(); return; }
    std::string resp; CdpRecv(resp);
}

// ── Material 3 (You) staged connect animation, injected into YM's page ──
// Stage 1 (checking): a tonal card with a spinning M3 progress ring around
// a player glyph, "Проверка синхронизации…". Stage 2 (success): ring
// stops, "YMHub" wordmark springs in below it, auto-dismisses. Stage 3
// (error): ring stops, glyph/text turn to M3 error tone, the message is
// written directly on screen and stays up longer. All three stages share
// one persistent #ymhub-status container so transitions are smooth. Built
// via plain DOM calls (no innerHTML/backticks) so the only quoting to
// worry about is the outer JSON-escape.
static void CdpShowStage1Checking() {
    std::wstring js =
        L"(function(){if(document.getElementById('ymhub-status'))return;"
        L"var o=document.createElement('div');o.id='ymhub-status';"
        L"o.style.cssText='position:fixed;inset:0;z-index:999999;display:flex;"
        L"align-items:center;justify-content:center;background:rgba(0,0,0,.5);"
        L"opacity:0;transition:opacity .35s ease;pointer-events:none;';"
        L"var st=document.createElement('style');"
        L"st.textContent='@keyframes ymhubSpin{to{transform:rotate(360deg)}}';"
        L"document.head.appendChild(st);"
        L"var card=document.createElement('div');"
        L"card.style.cssText='display:flex;flex-direction:column;align-items:center;"
        L"background:#1D1B2A;border-radius:28px;padding:32px 44px;"
        L"box-shadow:0 12px 40px rgba(0,0,0,.5);min-width:200px;';"
        L"var iw=document.createElement('div');"
        L"iw.style.cssText='position:relative;width:64px;height:64px;"
        L"display:flex;align-items:center;justify-content:center;margin-bottom:16px;';"
        L"var spin=document.createElement('div');spin.id='ymhub-spin';"
        L"spin.style.cssText='position:absolute;inset:0;border-radius:50%;"
        L"border:4px solid rgba(182,166,255,.22);border-top-color:#B7A6FF;"
        L"animation:ymhubSpin .9s linear infinite;';"
        L"var icon=document.createElement('div');icon.id='ymhub-icon';"
        L"icon.textContent='\\u266A';"
        L"icon.style.cssText='font-size:26px;color:#B7A6FF;transition:color .3s ease;';"
        L"iw.appendChild(spin);iw.appendChild(icon);"
        L"var title=document.createElement('div');title.id='ymhub-title';"
        L"title.textContent='YMHub';"
        L"title.style.cssText='font:700 20px system-ui,sans-serif;color:#E6E1E9;"
        L"letter-spacing:.3px;opacity:0;transform:scale(.7);margin-top:2px;"
        L"transition:opacity .4s ease,transform .45s cubic-bezier(.34,1.56,.64,1);';"
        L"var sub=document.createElement('div');sub.id='ymhub-sub';"
        L"sub.textContent='Проверка синхронизации\\u2026';"
        L"sub.style.cssText='font:500 13px system-ui,sans-serif;color:#C9C4D0;"
        L"margin-top:6px;text-align:center;transition:color .3s ease;';"
        L"card.appendChild(iw);card.appendChild(title);card.appendChild(sub);"
        L"o.appendChild(card);document.body.appendChild(o);"
        L"requestAnimationFrame(function(){o.style.opacity='1';});})()";
    CdpRunJs(CdpUtf8(js.c_str()));
}

static void CdpShowStage2Success() {
    std::wstring js =
        L"(function(){var s=document.getElementById('ymhub-spin');if(s)s.style.display='none';"
        L"var t=document.getElementById('ymhub-title');"
        L"if(t){t.style.opacity='1';t.style.transform='scale(1)';}"
        L"var sub=document.getElementById('ymhub-sub');if(sub)sub.textContent='Подключено';"
        L"setTimeout(function(){var o=document.getElementById('ymhub-status');"
        L"if(o){o.style.opacity='0';setTimeout(function(){o.remove();},400);}},1600);})()";
    CdpRunJs(CdpUtf8(js.c_str()));
}

static void CdpShowStage3Error(const wchar_t* message) {
    std::wstring js =
        L"(function(){var s=document.getElementById('ymhub-spin');if(s)s.style.display='none';"
        L"var icon=document.getElementById('ymhub-icon');"
        L"if(icon){icon.textContent='!';icon.style.color='#FFB4AB';}"
        L"var sub=document.getElementById('ymhub-sub');"
        L"if(sub){sub.style.color='#FFB4AB';sub.textContent='";
    js += message;
    js +=
        L"';}"
        L"setTimeout(function(){var o=document.getElementById('ymhub-status');"
        L"if(o){o.style.opacity='0';setTimeout(function(){o.remove();},400);}},4500);})()";
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
        "card.style.cssText='width:560px;max-height:70vh;display:flex;flex-direction:column;"
        "background:#1D1B2A;border-radius:20px;padding:20px;box-shadow:0 12px 40px rgba(0,0,0,.5);';"
        "var head=document.createElement('div');"
        "head.style.cssText='display:flex;align-items:center;justify-content:space-between;margin-bottom:10px;';"
        "var title=document.createElement('div');title.textContent='Логи YMHub';"
        "title.style.cssText='font:700 15px system-ui,sans-serif;color:#E6E1E9;';"
        "var actions=document.createElement('div');actions.style.cssText='display:flex;gap:8px;';"
        "var copyBtn=document.createElement('button');copyBtn.textContent='Копировать';"
        "var closeBtn=document.createElement('button');closeBtn.textContent='Закрыть';"
        "[copyBtn,closeBtn].forEach(function(b){b.style.cssText="
        "'height:28px;padding:0 12px;border-radius:8px;border:1px solid rgba(255,255,255,.12);"
        "background:rgba(255,255,255,.06);color:#C9C4D0;font:600 12px system-ui,sans-serif;cursor:pointer;';});"
        "var pre=document.createElement('pre');pre.id='ymhub-log-text';"
        "pre.style.cssText='flex:1;overflow:auto;margin:0;white-space:pre-wrap;word-break:break-word;"
        "font:11px Consolas,monospace;color:#C9C4D0;background:#15131f;border-radius:12px;padding:12px;';"
        "copyBtn.onclick=function(){navigator.clipboard.writeText(pre.textContent).catch(function(){});};"
        "closeBtn.onclick=function(){modal.style.display='none';};"
        "actions.appendChild(copyBtn);actions.appendChild(closeBtn);"
        "head.appendChild(title);head.appendChild(actions);"
        "card.appendChild(head);card.appendChild(pre);modal.appendChild(card);"
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

// Confirms the actual player UI is present (not just that CDP is reachable)
// before declaring success — checks for the like button in EITHER of its
// two states ("Нравится" / "Не нравится", depending on whether the current
// track is already liked), since checking only one label falsely reported
// "player not found" whenever the track happened to already be liked.
// Retries for a few seconds since the page can still be hydrating right
// after a fresh launch.
static bool CdpVerifyPlayerReady() {
    for (int i = 0; i < 20; i++) {
        if (i > 0) Sleep(500);
        std::string req =
            "{\"id\":5,\"method\":\"Runtime.evaluate\",\"params\":{\"expression\":"
            "\"!!(document.querySelector(\\\"button[aria-label='\\u041d\\u0440\\u0430\\u0432\\u0438\\u0442\\u0441\\u044f']\\\")"
            "||document.querySelector(\\\"button[aria-label='\\u041d\\u0435 \\u043d\\u0440\\u0430\\u0432\\u0438\\u0442\\u0441\\u044f']\\\"))\","
            "\"returnByValue\":true}}";
        if (!CdpSend(req)) { CdpClose(); return false; }
        std::string resp;
        if (!CdpRecv(resp)) { CdpClose(); return false; }
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

static void CdpClickButton(const wchar_t* ariaLabel) {
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

// ── Command execution ───────────────────────────────────────────
static void ExecCmd(DWORD cmd) {
    // Overlay toggle -> notify host
    if (cmd == YMHC_OVL_TOGGLE) {
        if (g_ipc && g_ipc->hostHwnd)
            PostMessageW(g_ipc->hostHwnd, WM_APP + 20, 0, 0);
        return;
    }

    // Like/dislike via CDP — works regardless of OS focus
    if (cmd == YMHC_LIKE)    { LogMsg("Like clicked"); CdpClickButton(L"Нравится"); return; }
    if (cmd == YMHC_DISLIKE) { LogMsg("Dislike clicked"); CdpClickButton(L"Не нравится"); return; }

    HWND main = FindMainWnd();
    if (!main) return;

    // Media commands via WM_APPCOMMAND — no focus needed
    switch (cmd) {
    case YMHC_PREV:
        PostMessageW(main, WM_APPCOMMAND, (WPARAM)main,
            MAKELPARAM(0, APPCOMMAND_MEDIA_PREVIOUSTRACK)); return;
    case YMHC_NEXT:
        PostMessageW(main, WM_APPCOMMAND, (WPARAM)main,
            MAKELPARAM(0, APPCOMMAND_MEDIA_NEXTTRACK)); return;
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
        }
    }
}

static LRESULT CALLBACK HotkeyWndProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_HOTKEY) {
        int id = (int)(short)LOWORD(wp);
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
static DWORD WINAPI CdpAnnounceThread(LPVOID) {
    LogMsg("Connecting to CDP on port " + std::to_string(CdpPort()) + "...");
    for (int i = 0; i < 30 && g_run; i++) {
        if (CdpEnsureConnected()) break;
        Sleep(500);
    }
    if (!g_cdpWs) { LogMsg("CDP connect failed after 30 attempts"); return 0; }
    LogMsg("CDP connected");
    CdpShowStage1Checking();
    if (CdpVerifyPlayerReady()) { LogMsg("Player ready"); CdpShowStage2Success(); }
    else { LogMsg("Player not found"); CdpShowStage3Error(L"Не удалось найти плеер Яндекс Музыки"); }
    return 0;
}

// Keeps the Settings-page log row present (while Settings is open) and its
// content current. Runs independently of CdpAnnounceThread so logs are
// visible even if the initial connect/verify sequence above failed.
static DWORD WINAPI LogBadgeThread(LPVOID) {
    while (g_run) {
        if (CdpEnsureConnected()) CdpInjectSettingsLogRow(LogBlob());
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

    g_lastCmdSeq = g_ipc->cmdSeq;
    g_lastKeySeq = g_ipc->keySeq;

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
