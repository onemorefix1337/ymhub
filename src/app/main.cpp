// YMHub — Yandex Music Hub
// Hub window (sidebar + 3 tabs) + floating overlay + DLL injection

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define UNICODE
#include <Windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <wrl.h>
#include <WebView2.h>
#include <string>
#include <atomic>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wincodec.h>
#include <shcore.h>
#include <winhttp.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <vector>
#include <ctime>
#include "../shared/ipc.h"
#include "../shared/version.h"

#pragma comment(lib,"shell32.lib")
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")
#pragma comment(lib,"dwmapi.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"shlwapi.lib")
#pragma comment(lib,"runtimeobject.lib")
#pragma comment(lib,"windowscodecs.lib")
#pragma comment(lib,"shcore.lib")
#pragma comment(lib,"winhttp.lib")
#pragma comment(lib,"ws2_32.lib")

#define YM_CDP_PORT 9876

using Microsoft::WRL::ComPtr;
namespace wmc = winrt::Windows::Media::Control;

// ── Window sizes ──────────────────────────────────────────
static const int CW = 420, CH = 188;   // overlay
static const int HW = 720, HH = 500;   // hub

// ── Position ─────────────────────────────────────────────
enum class Pos { BL=0,BC=1,BR=2,TL=3,TC=4,TR=5 };
static Pos g_pos = Pos::BC;

// ── Globals ───────────────────────────────────────────────
static HWND      g_hwnd   = nullptr;   // overlay
static HWND      g_hub    = nullptr;   // hub
static HINSTANCE g_hInst  = nullptr;
static HHOOK     g_hook   = nullptr;
static bool      g_visible = false;
static HANDLE    g_instanceMutex = nullptr; // single-instance lock, released early during self-update
static int       g_scrW = 0, g_scrH = 0;

// Overlay WebView2
static ComPtr<ICoreWebView2Environment>  g_env;
static ComPtr<ICoreWebView2Controller>   g_ctrl;
static ComPtr<ICoreWebView2>             g_wv;
static bool g_wvInited = false;

// Hub WebView2
static ComPtr<ICoreWebView2Controller>   g_hubCtrl;
static ComPtr<ICoreWebView2>             g_hubWv;

// ── Track state ───────────────────────────────────────────
static std::wstring g_track, g_artist;
static bool g_playing = false;
static bool g_liked   = false;
static std::string  g_artB64;
static std::wstring g_artTrackKey;
static std::atomic<bool> g_artReady{false};
static std::string       g_artPending;
static CRITICAL_SECTION  g_artCS;
static volatile bool     g_artFetching=false;

// ── IPC / DLL ─────────────────────────────────────────────
static HANDLE    g_hMem  = nullptr;
static YMHubIPC* g_ipc   = nullptr;

// ── Tray / timers ─────────────────────────────────────────
#define WM_TRAY      (WM_APP+1)
#define IDM_EXIT      1
#define IDM_AUTOSTART 2
#define IDM_OPENHUB   3
#define TIMER_TRACK   1
#define TIMER_INJECT  2

// ── Hotkeys ───────────────────────────────────────────────
struct HKey { DWORD mods, vk; };
static HKey g_keys[6] = {
    {3,0x31},{3,VK_LEFT},{3,VK_RIGHT},{3,VK_SPACE},{0,0},{0,0}
};
static bool g_rebinding = false;

// YM's own native in-page hotkeys (see YM_ACTION_* in shared/ipc.h) —
// {0,0} means "not remapped, use YM's own default key".
static HKey g_ymKeys[13] = {};

// ── Registry ─────────────────────────────────────────────
static const wchar_t* REG_APP = L"Software\\YMHub";
static const wchar_t* REG_RUN = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static DWORD RegGetDW(HKEY root,const wchar_t* k,const wchar_t* v,DWORD def){
    HKEY hk;DWORD d=def,sz=sizeof(d);
    if(RegOpenKeyExW(root,k,0,KEY_READ,&hk)==ERROR_SUCCESS){
        RegQueryValueExW(hk,v,nullptr,nullptr,(BYTE*)&d,&sz);
        RegCloseKey(hk);}
    return d;}
static void RegSetDW(HKEY root,const wchar_t* k,const wchar_t* v,DWORD d){
    HKEY hk;
    RegCreateKeyExW(root,k,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr);
    RegSetValueExW(hk,v,0,REG_DWORD,(BYTE*)&d,sizeof(d));
    RegCloseKey(hk);}
static std::wstring RegGetStr(HKEY root,const wchar_t* k,const wchar_t* v){
    // Big enough for both the short CustomName value and the much longer
    // CustomCss one — RegQueryValueExW fails outright (not just truncates)
    // if the buffer is smaller than the stored value, so this has to cover
    // the largest user-editable string field (see customCss in ipc.h).
    HKEY hk;wchar_t buf[4096]={0};DWORD sz=sizeof(buf);
    if(RegOpenKeyExW(root,k,0,KEY_READ,&hk)==ERROR_SUCCESS){
        if(RegQueryValueExW(hk,v,nullptr,nullptr,(BYTE*)buf,&sz)!=ERROR_SUCCESS)buf[0]=0;
        RegCloseKey(hk);}
    return buf;}
static void RegSetStr(HKEY root,const wchar_t* k,const wchar_t* v,const std::wstring& s){
    HKEY hk;
    RegCreateKeyExW(root,k,0,nullptr,0,KEY_WRITE,nullptr,&hk,nullptr);
    RegSetValueExW(hk,v,0,REG_SZ,(BYTE*)s.c_str(),(DWORD)((s.size()+1)*sizeof(wchar_t)));
    RegCloseKey(hk);}
static bool IsAutostart(){
    HKEY hk;bool has=false;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,REG_RUN,0,KEY_READ,&hk)==ERROR_SUCCESS){
        has=(RegQueryValueExW(hk,L"YMHub",nullptr,nullptr,nullptr,nullptr)==ERROR_SUCCESS);
        RegCloseKey(hk);}
    return has;}
static void SetAutostart(bool on){
    HKEY hk;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,REG_RUN,0,KEY_WRITE,&hk)!=ERROR_SUCCESS)return;
    if(on){
        wchar_t p[MAX_PATH];GetModuleFileNameW(nullptr,p,MAX_PATH);
        RegSetValueExW(hk,L"YMHub",0,REG_SZ,(BYTE*)p,(DWORD)((wcslen(p)+1)*sizeof(wchar_t)));
    }else RegDeleteValueW(hk,L"YMHub");
    RegCloseKey(hk);}

static const wchar_t* KEY_REG[6]={L"Key0",L"Key1",L"Key2",L"Key3",L"Key4",L"Key5"};
static const DWORD    KEY_DEF[6]={0x30031,0x30025,0x30027,0x30020,0,0};
static void LoadKeys(){
    for(int i=0;i<6;i++){
        DWORD v=RegGetDW(HKEY_CURRENT_USER,REG_APP,KEY_REG[i],KEY_DEF[i]);
        g_keys[i]={v>>16,v&0xFFFF};}
}
static void PushKeysToIPC(){
    if(!g_ipc)return;
    g_ipc->hostHwnd=g_hwnd;
    for(int i=0;i<6;i++){
        g_ipc->keys[i].mods=g_keys[i].mods;
        g_ipc->keys[i].vk  =g_keys[i].vk;}
    InterlockedIncrement(&g_ipc->keySeq);}

static void SaveKeys(){
    for(int i=0;i<6;i++)
        RegSetDW(HKEY_CURRENT_USER,REG_APP,KEY_REG[i],(g_keys[i].mods<<16)|g_keys[i].vk);
    PushKeysToIPC();}

static const wchar_t* YMKEY_REG[13]={
    L"YmKey0",L"YmKey1",L"YmKey2",L"YmKey3",L"YmKey4",L"YmKey5",L"YmKey6",
    L"YmKey7",L"YmKey8",L"YmKey9",L"YmKey10",L"YmKey11",L"YmKey12"};
static void LoadYmKeys(){
    for(int i=0;i<13;i++){
        DWORD v=RegGetDW(HKEY_CURRENT_USER,REG_APP,YMKEY_REG[i],0);
        g_ymKeys[i]={v>>16,v&0xFFFF};}
}
static void SaveYmKeys(){
    for(int i=0;i<13;i++)
        RegSetDW(HKEY_CURRENT_USER,REG_APP,YMKEY_REG[i],(g_ymKeys[i].mods<<16)|g_ymKeys[i].vk);}

// "Твики" hub tab — UI declutter toggles (see TWEAK_* in shared/ipc.h).
static DWORD g_tweaksMask=0;
// Replacement text for TWEAK_HIDE_NAME — empty means the DLL falls back to
// "Скрыто" itself (see CdpApplyNameHide in dllmain.cpp).
static std::wstring g_customName;
// Arbitrary user CSS from the "Свой CSS" box — appended to the built-in
// tweak rules so the user isn't limited to the canned toggle list.
static std::wstring g_customCss;
// Opt-in switch for the in-page cheat menu ("Меню в Яндекс Музыке" on the
// Плагины tab) — a separate feature from the Твики/Свой CSS settings
// above, but pushed to the DLL alongside them since CheatMenuThreadFn
// already polls g_ipc on the same tick as the tweaks it builds the menu
// around.
static bool g_cheatMenuEnabled=false;
static void LoadTweaks(){
    g_tweaksMask=RegGetDW(HKEY_CURRENT_USER,REG_APP,L"Tweaks",0);
    g_customName=RegGetStr(HKEY_CURRENT_USER,REG_APP,L"CustomName");
    g_customCss=RegGetStr(HKEY_CURRENT_USER,REG_APP,L"CustomCss");
    g_cheatMenuEnabled=RegGetDW(HKEY_CURRENT_USER,REG_APP,L"CheatMenu",0)!=0;}
static void PushTweaksToIPC(){
    if(!g_ipc)return;
    g_ipc->tweaksMask=g_tweaksMask;
    wcsncpy_s(g_ipc->customName,g_customName.c_str(),_TRUNCATE);
    wcsncpy_s(g_ipc->customCss,g_customCss.c_str(),_TRUNCATE);
    g_ipc->cheatMenuEnabled=g_cheatMenuEnabled?1:0;
    InterlockedIncrement(&g_ipc->tweaksSeq);}
static void SaveTweaks(){
    RegSetDW(HKEY_CURRENT_USER,REG_APP,L"Tweaks",g_tweaksMask);
    PushTweaksToIPC();}
static void SaveCheatMenuSetting(bool on){
    g_cheatMenuEnabled=on;
    RegSetDW(HKEY_CURRENT_USER,REG_APP,L"CheatMenu",on?1:0);
    PushTweaksToIPC();}
static void SaveCustomName(const std::wstring& s){
    g_customName=s;
    RegSetStr(HKEY_CURRENT_USER,REG_APP,L"CustomName",g_customName);
    PushTweaksToIPC();}
static void SaveCustomCss(const std::wstring& s){
    g_customCss=s;
    RegSetStr(HKEY_CURRENT_USER,REG_APP,L"CustomCss",g_customCss);
    PushTweaksToIPC();}

// Default VK codes for YM's own "Горячие клавиши", in the same order as
// g_ymKeys/YM_ROW_INFO in the hub UI (play/pause, mute, seek fwd/back,
// vol up/down, like, dislike, repeat, shuffle, next, prev, fullscreen).
static const DWORD YM_DEFAULT_VK[13]={
    'K','M','L','J',VK_UP,VK_DOWN,'F','D','R','S','N','P','W'};

// SendInput-synthesized key events are silently ignored by YM/Electron
// (confirmed empirically — neither toggled playback, while a real keypress
// did), so the translated default key has to come from the DLL's CDP
// connection via Input.dispatchKeyEvent instead. This just signals the
// DLL which action's default key to send; WorkerThread in dllmain.cpp
// polls ymSendSeq at ~15ms alongside the existing cmdSeq/keySeq fields.
static void SendYmDefaultKey(int idx){
    if(!g_ipc)return;
    g_ipc->ymSendIdx=(DWORD)idx;
    InterlockedIncrement(&g_ipc->ymSendSeq);}

// ── Position helper ───────────────────────────────────────
static void GetCardPos(int& x,int& y){
    const int M=60;int pi=(int)g_pos;
    x=(pi==0||pi==3)?M:(pi==1||pi==4)?(g_scrW-CW)/2:g_scrW-CW-M;
    y=(pi>=3)?M:g_scrH-CH-M;}

// ── Base64 ────────────────────────────────────────────────
static const char B64T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string Base64Enc(const BYTE* d,size_t n){
    std::string r;r.reserve(((n+2)/3)*4);
    for(size_t i=0;i<n;i+=3){
        unsigned v=(unsigned)d[i]<<16;
        if(i+1<n)v|=(unsigned)d[i+1]<<8;
        if(i+2<n)v|=d[i+2];
        r+=B64T[(v>>18)&63];r+=B64T[(v>>12)&63];
        r+=(i+1<n)?B64T[(v>>6)&63]:'=';
        r+=(i+2<n)?B64T[v&63]:'=';}
    return r;}

// ── SMTC ─────────────────────────────────────────────────
static HWND FindYM(){
    struct S{HWND hw;}s={};
    EnumWindows([](HWND hw,LPARAM lp)->BOOL{
        wchar_t t[512]={};GetWindowTextW(hw,t,512);
        if((wcsstr(t,L"Яндекс Музыка")||wcsstr(t,L"Yandex Music"))&&IsWindowVisible(hw))
            {((S*)lp)->hw=hw;return FALSE;}
        return TRUE;},(LPARAM)&s);
    return s.hw;}

// GlobalSystemMediaTransportControlsSessionManager activates an
// out-of-process COM server (an ALPC call under the hood). If that
// broker ever gets stuck system-side, .get() blocks forever with no
// timeout and freezes our message loop permanently. Poll Status()
// instead so a stuck call is abandoned rather than hanging the UI.
template<typename Op>
static auto AwaitOrTimeout(Op const& op,int timeoutMs)->decltype(op.GetResults()){
    using winrt::Windows::Foundation::AsyncStatus;
    ULONGLONG start=GetTickCount64();
    while(op.Status()==AsyncStatus::Started){
        if(GetTickCount64()-start>(ULONGLONG)timeoutMs){
            op.Cancel();
            throw winrt::hresult_error(E_ABORT);}
        Sleep(20);}
    return op.GetResults();}

static void LoadArtAsync(winrt::Windows::Storage::Streams::IRandomAccessStreamReference ref,std::wstring trackKey){
    std::thread([ref,trackKey]()mutable{
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        std::string b64;
        // Thumbnail reads can hang or fail transiently (same flaky SMTC
        // broker as elsewhere, plus art that isn't cached locally yet) —
        // bound the wait and retry a couple of times before giving up.
        for(int attempt=0;attempt<3 && b64.empty();attempt++){
        if(attempt>0)Sleep(400);
        try{
            auto openOp=ref.OpenReadAsync();
            auto ras=AwaitOrTimeout(openOp,3000);
            if(!ras)continue;
            winrt::com_ptr<IStream> cs;
            if(FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(ras),IID_PPV_ARGS(cs.put()))))throw 0;
            IWICImagingFactory* wic=nullptr;
            CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic));
            if(!wic)throw 0;
            IWICBitmapDecoder* dec=nullptr;
            wic->CreateDecoderFromStream(cs.get(),nullptr,WICDecodeMetadataCacheOnLoad,&dec);
            if(!dec){wic->Release();throw 0;}
            IWICBitmapFrameDecode* fr=nullptr;dec->GetFrame(0,&fr);dec->Release();
            if(!fr){wic->Release();throw 0;}
            IWICBitmapScaler* sc2=nullptr;wic->CreateBitmapScaler(&sc2);
            sc2->Initialize(fr,128,128,WICBitmapInterpolationModeFant);
            IWICFormatConverter* cv=nullptr;wic->CreateFormatConverter(&cv);
            cv->Initialize(sc2,GUID_WICPixelFormat32bppRGBA,WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom);
            fr->Release();sc2->Release();
            UINT w=0,h=0;cv->GetSize(&w,&h);
            std::vector<BYTE> px(w*h*4);
            cv->CopyPixels(nullptr,w*4,(UINT)px.size(),px.data());
            cv->Release();wic->Release();
            // WIC outputs BGRA bytes; swap R<->B so PNG is correct RGBA
            for(size_t i=0;i<px.size();i+=4) std::swap(px[i],px[i+2]);
            IWICImagingFactory* wic2=nullptr;
            CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&wic2));
            IWICStream* ws=nullptr;wic2->CreateStream(&ws);
            IStream* ms=nullptr;CreateStreamOnHGlobal(nullptr,TRUE,&ms);
            ws->InitializeFromIStream(ms);
            IWICBitmapEncoder* enc=nullptr;wic2->CreateEncoder(GUID_ContainerFormatPng,nullptr,&enc);
            enc->Initialize(ws,WICBitmapEncoderNoCache);
            IWICBitmapFrameEncode* fe=nullptr;IPropertyBag2* opts=nullptr;
            enc->CreateNewFrame(&fe,&opts);fe->Initialize(opts);
            fe->SetSize(w,h);WICPixelFormatGUID pf=GUID_WICPixelFormat32bppRGBA;
            fe->SetPixelFormat(&pf);
            fe->WritePixels(h,w*4,(UINT)px.size(),px.data());
            fe->Commit();enc->Commit();
            HGLOBAL hg=nullptr;GetHGlobalFromStream(ms,&hg);
            SIZE_T sz=GlobalSize(hg);void* ptr=GlobalLock(hg);
            b64=Base64Enc((BYTE*)ptr,sz);GlobalUnlock(hg);
            fe->Release();enc->Release();if(opts)opts->Release();
            ws->Release();ms->Release();wic2->Release();
        }catch(...){b64="";}
        }
        g_artFetching=false;
        // Drop the result if the user already moved to a different track
        // while this (possibly slow/retried) fetch was in flight, so a
        // late failure can't blank out art that's already correct, and a
        // late success can't paint the wrong track's cover.
        if(g_artTrackKey!=trackKey)return;
        if(b64.empty())return; // let the next track-poll tick retry instead of latching empty
        EnterCriticalSection(&g_artCS);
        g_artPending=b64;g_artReady=true;
        LeaveCriticalSection(&g_artCS);
        if(g_hwnd)PostMessageW(g_hwnd,WM_APP+3,0,0);
    }).detach();}

static volatile bool g_parsing=false;

// The actual SMTC fetch, including the timeout-bounded waits. Runs off
// the UI thread (see ParseYM below) so the up-to-3s worst case from
// AwaitOrTimeout never blocks window dragging/message processing.
static void ParseYMWork(){
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    HWND hw=g_hwnd;
    bool justChangedTrack=false;
    try{
        auto mgrOp=wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync();
        auto mgr=AwaitOrTimeout(mgrOp,1500);
        if(mgr){
            wmc::GlobalSystemMediaTransportControlsSession ym=nullptr;
            for(auto s:mgr.GetSessions())
                if(s.SourceAppUserModelId()==L"ru.yandex.desktop.music"){ym=s;break;}
            if(!ym)ym=mgr.GetCurrentSession();
            if(ym){
                auto propOp=ym.TryGetMediaPropertiesAsync();
                auto p=AwaitOrTimeout(propOp,1500);
                if(p&&!p.Title().empty()){
                    std::wstring track=p.Title().c_str();
                    g_track=track;g_artist=p.Artist().c_str();
                    g_playing=(ym.GetPlaybackInfo().PlaybackStatus()==
                        wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing);
                    bool changed=(g_artTrackKey!=track);
                    if(changed){g_artTrackKey=track;g_artB64="";g_liked=false;justChangedTrack=true;}
                    // Retry every poll while art is still missing for the
                    // current track — SMTC's Thumbnail() is sometimes not
                    // populated yet on the very first poll after a track
                    // change, and without this it never got fetched again.
                    if(g_artB64.empty()&&!g_artFetching&&p.Thumbnail()){
                        g_artFetching=true;
                        LoadArtAsync(p.Thumbnail(),track);}
                }
            }
        }
    }catch(...){}
    // Resync with the DLL's real (DOM-polled) liked state — corrects the
    // optimistic toggle in DoLike()/DoDislike() if it ever drifts, and
    // picks up likes made outside the hub entirely (YM's own UI, remapped
    // native hotkeys, or a track that was already liked on load). Skipped
    // right on a track change: the DLL polls every ~2s independently, so
    // ymLiked can still hold the *previous* track's state for up to that
    // long — applying it here would flash the old status before the DLL
    // catches up, instead of the correct assume-unliked default above.
    if(g_ipc&&!justChangedTrack)g_liked=(g_ipc->ymLiked!=0);
    g_parsing=false;
    if(hw)PostMessageW(hw,WM_APP+27,0,0);}

static void ParseYM(){
    if(g_parsing)return;
    g_parsing=true;
    std::thread(ParseYMWork).detach();}

// ── JSON helpers ──────────────────────────────────────────
static std::wstring JsonEsc(const std::wstring& s){
    std::wstring r;for(wchar_t c:s){
        if(c==L'"')r+=L"\\\"";
        else if(c==L'\\')r+=L"\\\\";
        else if(c==L'\n')r+=L"\\n";
        else r+=c;}
    return r;}
static std::string ToUtf8(const std::wstring& w){
    if(w.empty())return std::string();
    int n=WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),nullptr,0,nullptr,nullptr);
    std::string r(n,0);
    WideCharToMultiByte(CP_UTF8,0,w.c_str(),(int)w.size(),r.data(),n,nullptr,nullptr);
    return r;}

// ── Discord Rich Presence ─────────────────────────────────
// No SDK, just Discord's local RPC pipe protocol: connect to
// \\.\pipe\discord-ipc-N (N=0..9 — desktop Discord is almost always 0,
// higher indices only matter if multiple Discord-family clients are
// running), send a one-time handshake frame, then a SET_ACTIVITY frame
// whenever the track changes. Each frame is an 8-byte header (int32
// opcode, int32 length) followed by that many bytes of JSON. All I/O
// goes through overlapped reads/writes with a short timeout — a hung
// or unresponsive Discord must not be able to stall this thread
// indefinitely (the DLL's CDP client has the same concern, see its
// WinHTTP timeout comments).
static const char* DISCORD_CLIENT_ID = "1518944722310266911";
static HANDLE g_discordPipe = nullptr;
static HANDLE g_discordEvt  = nullptr;
static bool   g_discordEnabled = false;
static time_t g_discordStart = 0;
static std::wstring g_discordLastTrack, g_discordLastArtist, g_discordLastCover;
static bool g_discordWasSent = false;

static void LoadDiscordSetting(){
    g_discordEnabled = RegGetDW(HKEY_CURRENT_USER,REG_APP,L"DiscordRpc",0)!=0;}
static void SaveDiscordSetting(bool on){
    g_discordEnabled=on;
    RegSetDW(HKEY_CURRENT_USER,REG_APP,L"DiscordRpc",on?1:0);}

static void DiscordClose(){
    if(g_discordPipe){CloseHandle(g_discordPipe);g_discordPipe=nullptr;}}

static bool DiscordIo(bool isWrite,void* buf,DWORD len,DWORD timeoutMs){
    if(!g_discordPipe)return false;
    OVERLAPPED ov={};ov.hEvent=g_discordEvt;ResetEvent(g_discordEvt);
    BOOL ok=isWrite?WriteFile(g_discordPipe,buf,len,nullptr,&ov):ReadFile(g_discordPipe,buf,len,nullptr,&ov);
    if(!ok){
        if(GetLastError()!=ERROR_IO_PENDING)return false;
        if(WaitForSingleObject(g_discordEvt,timeoutMs)!=WAIT_OBJECT_0){CancelIoEx(g_discordPipe,&ov);return false;}
    }
    DWORD xferred=0;
    return GetOverlappedResult(g_discordPipe,&ov,&xferred,FALSE)&&xferred==len;}

static bool DiscordSendFrame(int opcode,const std::string& json){
    BYTE hdr[8];DWORD len=(DWORD)json.size();
    memcpy(hdr,&opcode,4);memcpy(hdr+4,&len,4);
    if(!DiscordIo(true,hdr,8,300))return false;
    if(len&&!DiscordIo(true,(void*)json.data(),len,300))return false;
    return true;}

// Measured empirically: Discord's very first handshake reply (the READY
// dispatch, carrying its whole user/config blob) can take ~850ms — way
// past what a steady-state ack needs — so the handshake recv gets its own
// much longer allowance than regular frames.
static bool DiscordRecvFrame(DWORD timeoutMs=500){
    BYTE hdr[8];
    if(!DiscordIo(false,hdr,8,timeoutMs))return false;
    DWORD len;memcpy(&len,hdr+4,4);
    if(len>65536)return false;
    std::string body(len,'\0');
    if(len&&!DiscordIo(false,body.data(),len,timeoutMs))return false;
    return true;}

static bool DiscordConnect(){
    if(g_discordPipe)return true;
    if(!g_discordEvt)g_discordEvt=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    for(int i=0;i<10;i++){
        wchar_t path[64];swprintf_s(path,L"\\\\.\\pipe\\discord-ipc-%d",i);
        HANDLE h=CreateFileW(path,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
        if(h!=INVALID_HANDLE_VALUE){g_discordPipe=h;break;}
    }
    if(!g_discordPipe)return false; // Discord isn't running — not an error, just nothing to do yet
    std::string hs=std::string("{\"v\":1,\"client_id\":\"")+DISCORD_CLIENT_ID+"\"}";
    if(!DiscordSendFrame(0,hs)||!DiscordRecvFrame(3000)){DiscordClose();return false;}
    return true;}

static void DiscordClearActivity(){
    if(!DiscordConnect())return;
    std::string activity="{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+
        ",\"activity\":null},\"nonce\":\"ymhub-clear\"}";
    if(!DiscordSendFrame(1,activity)||!DiscordRecvFrame())DiscordClose();}

// Called every ~2s from a dedicated thread (DiscordThreadFn) — track/
// artist/playing are read without synchronization, same as
// BroadcastState() already does with these same globals elsewhere in
// this file; a torn read here just means presence updates a tick late.
static void DiscordTick(){
    if(!g_discordEnabled){
        if(g_discordWasSent){DiscordClearActivity();g_discordWasSent=false;g_discordLastTrack.clear();g_discordLastCover.clear();}
        return;}
    if(!g_playing||g_track.empty()){
        if(g_discordWasSent){DiscordClearActivity();g_discordWasSent=false;g_discordLastTrack.clear();g_discordLastCover.clear();}
        return;}
    std::wstring cover=g_ipc?g_ipc->coverUrl:L"";
    if(g_track==g_discordLastTrack&&g_artist==g_discordLastArtist&&cover==g_discordLastCover&&g_discordWasSent)
        return; // nothing actually changed — don't spam SET_ACTIVITY
    if(g_track!=g_discordLastTrack||g_artist!=g_discordLastArtist)g_discordStart=time(nullptr);
    if(!DiscordConnect())return;
    std::string details=ToUtf8(JsonEsc(g_track));
    std::string state  =ToUtf8(JsonEsc(g_artist));
    std::string assets;
    if(!cover.empty())
        assets=",\"assets\":{\"large_image\":\""+ToUtf8(JsonEsc(cover))+"\",\"large_text\":\""+details+"\"}";
    std::string activity=
        "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":"+std::to_string(GetCurrentProcessId())+
        ",\"activity\":{\"type\":2,\"details\":\""+details+"\",\"state\":\""+state+"\","
        "\"timestamps\":{\"start\":"+std::to_string((long long)g_discordStart)+"}"+assets+","
        "\"instance\":false}},\"nonce\":\"ymhub-set\"}";
    if(DiscordSendFrame(1,activity)&&DiscordRecvFrame()){
        g_discordLastTrack=g_track;g_discordLastArtist=g_artist;g_discordLastCover=cover;g_discordWasSent=true;
    } else DiscordClose(); // reconnect from scratch next tick
}

static void DiscordThreadFn(){
    for(;;){DiscordTick();Sleep(2000);}}

static bool IsDllLoaded(){
    HANDLE h=OpenMutexW(SYNCHRONIZE,FALSE,YMH_MUTEX_NAME);
    if(h){CloseHandle(h);return true;}
    return false;}

static void BroadcastState(){
    std::wstring art(g_artB64.begin(),g_artB64.end());
    bool dll=IsDllLoaded();
    std::wstring msg=
        L"{\"type\":\"state\""
        L",\"track\":\""+JsonEsc(g_track)+L"\""
        L",\"artist\":\""+JsonEsc(g_artist)+L"\""
        L",\"playing\":"+(g_playing?L"true":L"false")+
        L",\"liked\":"+(g_liked?L"true":L"false")+
        L",\"art\":\""+art+L"\""+
        L",\"dllLoaded\":"+(dll?L"true":L"false")+
        L",\"overlayVisible\":"+(g_visible?L"true":L"false")+
        L",\"pos\":"+std::to_wstring((int)g_pos)+
        L",\"drpcEnabled\":"+(g_discordEnabled?L"true":L"false")+
        L",\"drpcConnected\":"+(g_discordPipe?L"true":L"false")+
        L",\"cheatMenuEnabled\":"+(g_cheatMenuEnabled?L"true":L"false")+
        L",\"ver\":\"" YMHUB_VERSION_W L"\""
        L"}";
    if(g_wv)    g_wv->PostWebMessageAsString(msg.c_str());
    if(g_hubWv) g_hubWv->PostWebMessageAsString(msg.c_str());}

static void SendHubKeys(){
    if(!g_hubWv)return;
    wchar_t buf[512];
    swprintf_s(buf,
        L"{\"type\":\"init-keys\",\"keys\":["
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u},"
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u},"
        L"{\"m\":%u,\"v\":%u},{\"m\":%u,\"v\":%u}]}",
        g_keys[0].mods,g_keys[0].vk,g_keys[1].mods,g_keys[1].vk,
        g_keys[2].mods,g_keys[2].vk,g_keys[3].mods,g_keys[3].vk,
        g_keys[4].mods,g_keys[4].vk,g_keys[5].mods,g_keys[5].vk);
    g_hubWv->PostWebMessageAsString(buf);}

static void SendHubYmKeys(){
    if(!g_hubWv)return;
    wchar_t buf[768]=L"{\"type\":\"init-ymkeys\",\"keys\":[";
    for(int i=0;i<13;i++){
        wchar_t part[48];
        swprintf_s(part,L"%s{\"m\":%u,\"v\":%u}",i?L",":L"",g_ymKeys[i].mods,g_ymKeys[i].vk);
        wcscat_s(buf,part);}
    wcscat_s(buf,L"]}");
    g_hubWv->PostWebMessageAsString(buf);}

static void SendHubTweaks(){
    if(!g_hubWv)return;
    std::wstring msg=L"{\"type\":\"init-tweaks\",\"mask\":"+std::to_wstring(g_tweaksMask)+
        L",\"customName\":\""+JsonEsc(g_customName)+L"\""
        L",\"customCss\":\""+JsonEsc(g_customCss)+L"\"}";
    g_hubWv->PostWebMessageAsString(msg.c_str());}

// ── IPC / DLL injection ───────────────────────────────────
static void InitIPC(){
    if(g_ipc)return;
    g_hMem=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,
        0,sizeof(YMHubIPC),YMH_SHMEM_NAME);
    if(!g_hMem)return;
    bool fresh=(GetLastError()!=ERROR_ALREADY_EXISTS);
    g_ipc=(YMHubIPC*)MapViewOfFile(g_hMem,FILE_MAP_ALL_ACCESS,0,0,sizeof(YMHubIPC));
    if(fresh&&g_ipc)ZeroMemory(g_ipc,sizeof(YMHubIPC));}

static std::wstring ExtractDll(){
    HRSRC hr=FindResourceW(g_hInst,MAKEINTRESOURCEW(101),RT_RCDATA);
    if(!hr)return L"";
    HGLOBAL hg=LoadResource(g_hInst,hr);
    if(!hg)return L"";
    DWORD sz=SizeofResource(g_hInst,hr);
    void* data=LockResource(hg);
    if(!data||sz==0)return L"";
    wchar_t tmp[MAX_PATH];GetTempPathW(MAX_PATH,tmp);
    std::wstring path=std::wstring(tmp)+L"YMHubDll.dll";
    HANDLE hf=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,
        CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(hf==INVALID_HANDLE_VALUE)
        return(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES)?path:L"";
    DWORD written=0;WriteFile(hf,data,sz,&written,nullptr);CloseHandle(hf);
    return written==sz?path:L"";}

static bool InjectDll(DWORD pid,const std::wstring& dll){
    HANDLE proc=OpenProcess(
        PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|
        PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ,FALSE,pid);
    if(!proc)return false;
    SIZE_T nb=(dll.size()+1)*sizeof(wchar_t);
    void* mem=VirtualAllocEx(proc,nullptr,nb,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!mem){CloseHandle(proc);return false;}
    WriteProcessMemory(proc,mem,dll.c_str(),nb,nullptr);
    FARPROC loadLib=GetProcAddress(GetModuleHandleW(L"kernel32.dll"),"LoadLibraryW");
    HANDLE t=CreateRemoteThread(proc,nullptr,0,(LPTHREAD_START_ROUTINE)loadLib,mem,0,nullptr);
    bool ok=(t!=nullptr);
    if(t){WaitForSingleObject(t,5000);CloseHandle(t);}
    VirtualFreeEx(proc,mem,0,MEM_RELEASE);
    CloseHandle(proc);
    return ok;}

static void TryInject(){
    if(IsDllLoaded())return;
    HWND ym=FindYM();if(!ym)return;
    DWORD pid=0;GetWindowThreadProcessId(ym,&pid);if(!pid)return;
    std::wstring dll=ExtractDll();if(dll.empty())return;
    InjectDll(pid,dll);}

// ── Chrome DevTools Protocol port (for background like/dislike) ───
// Quick TCP connect probe — true if *something* is already listening on
// 127.0.0.1:port (regardless of whether it's a valid CDP endpoint).
static bool IsPortInUse(int port){
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2,2),&wsa)!=0)return false;
    SOCKET s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    bool inUse=false;
    if(s!=INVALID_SOCKET){
        u_long mode=1;ioctlsocket(s,FIONBIO,&mode); // non-blocking
        sockaddr_in addr{};addr.sin_family=AF_INET;
        addr.sin_port=htons((u_short)port);
        InetPtonA(AF_INET,"127.0.0.1",&addr.sin_addr);
        connect(s,(sockaddr*)&addr,sizeof(addr));
        fd_set wr;FD_ZERO(&wr);FD_SET(s,&wr);
        timeval tv{0,80000}; // 80ms — loopback, no real round-trip to wait for
        inUse=select(0,nullptr,&wr,nullptr,&tv)>0;
        closesocket(s);}
    WSACleanup();
    return inUse;}

// WinHttpSetTimeouts is not reliably honored for the *connect* phase against
// a closed port (observed ~2s per attempt instead of the configured 300ms —
// the same WinHTTP-timeout class of bug seen elsewhere in this codebase).
// Gate the slow WinHttp request behind the fast Winsock check above so a
// 20-port scan over closed ports stays fast instead of taking ~40s.
static bool ProbeDebugPort(int port){
    if(!IsPortInUse(port))return false;
    HINTERNET hSession=WinHttpOpen(L"YMHub",WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession)return false;
    WinHttpSetTimeouts(hSession,300,300,300,300);
    bool ok=false;
    HINTERNET hConnect=WinHttpConnect(hSession,L"127.0.0.1",(INTERNET_PORT)port,0);
    if(hConnect){
        HINTERNET hReq=WinHttpOpenRequest(hConnect,L"GET",L"/json/version",nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0);
        if(hReq){
            if(WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(hReq,nullptr))
                ok=true;
            WinHttpCloseHandle(hReq);}
        WinHttpCloseHandle(hConnect);}
    WinHttpCloseHandle(hSession);
    return ok;}

static void KillAllByExeName(const wchar_t* exeName){
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS,0);
    if(snap==INVALID_HANDLE_VALUE)return;
    PROCESSENTRY32W pe{};pe.dwSize=sizeof(pe);
    if(Process32FirstW(snap,&pe)){
        do{
            if(_wcsicmp(pe.szExeFile,exeName)==0){
                HANDLE h=OpenProcess(PROCESS_TERMINATE,FALSE,pe.th32ProcessID);
                if(h){TerminateProcess(h,0);CloseHandle(h);}}
        }while(Process32NextW(snap,&pe));}
    CloseHandle(snap);}

// Kills and relaunches YM (plain, no --remote-debugging-port) so the new
// YMHub instance's own EnsureDebugPort/TryInject cycle injects the fresh
// DLL right away, instead of leaving the old DLL loaded in YM's process
// until the user happens to restart YM on their own. No-op if YM wasn't
// running at all — nothing to refresh, and we shouldn't start it.
static void RestartYM(){
    HWND ym=FindYM();if(!ym)return;
    DWORD pid=0;GetWindowThreadProcessId(ym,&pid);if(!pid)return;
    std::wstring exePath;
    HANDLE hProc=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(hProc){
        wchar_t buf[MAX_PATH];DWORD sz=MAX_PATH;
        if(QueryFullProcessImageNameW(hProc,0,buf,&sz))exePath.assign(buf,sz);
        CloseHandle(hProc);}
    if(exePath.empty())return;
    const wchar_t* exeName=PathFindFileNameW(exePath.c_str());
    KillAllByExeName(exeName);
    Sleep(1500);
    STARTUPINFOW si{};si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd=L"\""+exePath+L"\"";
    std::wstring cmdMut=cmd;
    if(CreateProcessW(nullptr,cmdMut.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
        CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}}

// Pick a port for --remote-debugging-port: reuse one that's already
// serving valid CDP (port==0 means "search from YM_CDP_PORT"), otherwise
// scan forward from the default for the first genuinely free port.
static int PickCdpPort(){
    for(int p=YM_CDP_PORT;p<YM_CDP_PORT+20;p++){
        if(ProbeDebugPort(p))return p;     // already our valid CDP
        if(!IsPortInUse(p))return p;}      // free, can launch here
    return YM_CDP_PORT;}

static bool g_debugPortFixDone=false;
static void EnsureDebugPort(){
    if(g_debugPortFixDone)return;
    int existing=0;
    for(int p=YM_CDP_PORT;p<YM_CDP_PORT+20;p++)
        if(ProbeDebugPort(p)){existing=p;break;}
    if(existing){
        g_debugPortFixDone=true;
        if(g_ipc)g_ipc->cdpPort=(DWORD)existing;
        return;}
    HWND ym=FindYM();if(!ym)return; // only act if YM is actually running
    DWORD pid=0;GetWindowThreadProcessId(ym,&pid);if(!pid)return;
    std::wstring exePath;
    HANDLE hProc=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
    if(hProc){
        wchar_t buf[MAX_PATH];DWORD sz=MAX_PATH;
        if(QueryFullProcessImageNameW(hProc,0,buf,&sz))exePath.assign(buf,sz);
        CloseHandle(hProc);}
    if(exePath.empty()){g_debugPortFixDone=true;return;}
    g_debugPortFixDone=true; // only ever attempt this once per YMHub session
    int port=PickCdpPort();
    if(g_ipc)g_ipc->cdpPort=(DWORD)port; // DLL will connect here once it's up
    const wchar_t* exeName=PathFindFileNameW(exePath.c_str());
    KillAllByExeName(exeName);
    Sleep(1500);
    std::wstring cmd=L"\""+exePath+L"\" --remote-debugging-port="+std::to_wstring(port);
    STARTUPINFOW si{};si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmdMut=cmd; // CreateProcessW needs a mutable buffer
    if(CreateProcessW(nullptr,cmdMut.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi)){
        CloseHandle(pi.hProcess);CloseHandle(pi.hThread);}}

// ── Updater (GitHub Releases) ──────────────────────────────
static bool HttpsGet(const wchar_t* host,const wchar_t* path,std::string& outBody){
    HINTERNET hSession=WinHttpOpen(L"YMHub-Updater",WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession)return false;
    WinHttpSetTimeouts(hSession,5000,5000,5000,10000);
    bool ok=false;
    HINTERNET hConnect=WinHttpConnect(hSession,host,INTERNET_DEFAULT_HTTPS_PORT,0);
    if(hConnect){
        HINTERNET hReq=WinHttpOpenRequest(hConnect,L"GET",path,nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(hReq){
            WinHttpAddRequestHeaders(hReq,L"User-Agent: YMHub-Updater",(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
            if(WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(hReq,nullptr)){
                DWORD avail=0;
                while(WinHttpQueryDataAvailable(hReq,&avail)&&avail>0){
                    std::string chunk(avail,0);DWORD read=0;
                    if(!WinHttpReadData(hReq,chunk.data(),avail,&read))break;
                    outBody.append(chunk.data(),read);}
                ok=true;}
            WinHttpCloseHandle(hReq);}
        WinHttpCloseHandle(hConnect);}
    WinHttpCloseHandle(hSession);
    return ok;}

static bool HttpsDownloadToFileOnce(const std::wstring& url,const std::wstring& destPath){
    if(url.substr(0,8)!=L"https://")return false;
    auto rest=url.substr(8);
    auto slash=rest.find(L'/');
    std::wstring host=(slash==std::wstring::npos)?rest:rest.substr(0,slash);
    std::wstring path=(slash==std::wstring::npos)?L"/":rest.substr(slash);

    HINTERNET hSession=WinHttpOpen(L"YMHub-Updater",WINHTTP_ACCESS_TYPE_NO_PROXY,
        WINHTTP_NO_PROXY_NAME,WINHTTP_NO_PROXY_BYPASS,0);
    if(!hSession)return false;
    WinHttpSetTimeouts(hSession,5000,5000,5000,30000);
    bool ok=false;
    HINTERNET hConnect=WinHttpConnect(hSession,host.c_str(),INTERNET_DEFAULT_HTTPS_PORT,0);
    if(hConnect){
        HINTERNET hReq=WinHttpOpenRequest(hConnect,L"GET",path.c_str(),nullptr,
            WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,WINHTTP_FLAG_SECURE);
        if(hReq){
            WinHttpAddRequestHeaders(hReq,L"User-Agent: YMHub-Updater",(DWORD)-1,WINHTTP_ADDREQ_FLAG_ADD);
            if(WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
                WINHTTP_NO_REQUEST_DATA,0,0,0)&&WinHttpReceiveResponse(hReq,nullptr)){
                HANDLE hf=CreateFileW(destPath.c_str(),GENERIC_WRITE,0,nullptr,
                    CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
                if(hf!=INVALID_HANDLE_VALUE){
                    DWORD avail=0;bool writeOk=true;
                    while(writeOk&&WinHttpQueryDataAvailable(hReq,&avail)&&avail>0){
                        std::vector<char> chunk(avail);DWORD read=0;
                        if(!WinHttpReadData(hReq,chunk.data(),avail,&read)){writeOk=false;break;}
                        DWORD written=0;
                        if(!WriteFile(hf,chunk.data(),read,&written,nullptr)){writeOk=false;break;}}
                    CloseHandle(hf);
                    ok=writeOk;}}
            WinHttpCloseHandle(hReq);}
        WinHttpCloseHandle(hConnect);}
    WinHttpCloseHandle(hSession);
    return ok;}

// The release-asset download (github.com redirecting to the CDN) has been
// observed to fail transiently once in a while — retry a couple of times
// before giving up, same lesson as the album-art fetch retries.
static bool HttpsDownloadToFile(const std::wstring& url,const std::wstring& destPath){
    for(int attempt=0;attempt<3;attempt++){
        if(attempt>0)Sleep(800);
        if(HttpsDownloadToFileOnce(url,destPath))return true;
    }
    return false;}

// Pulls tag_name + the YMHub.exe asset URL out of GitHub's "latest
// release" JSON via plain substring search (consistent with the rest of
// this codebase — no JSON library in use anywhere else either).
static bool ParseLatestRelease(const std::string& json,std::wstring& tag,std::wstring& assetUrl){
    auto tp=json.find("\"tag_name\"");
    if(tp==std::string::npos)return false;
    auto q1=json.find('"',tp+11);
    auto q2=(q1!=std::string::npos)?json.find('"',q1+1):std::string::npos;
    if(q1==std::string::npos||q2==std::string::npos)return false;
    std::string t=json.substr(q1+1,q2-q1-1);
    tag.assign(t.begin(),t.end());

    auto ap=json.find("\"YMHub.exe\"");
    if(ap==std::string::npos)return false;
    auto up=json.find("\"browser_download_url\"",ap);
    if(up==std::string::npos)return false;
    auto u1=json.find('"',up+23);
    auto u2=(u1!=std::string::npos)?json.find('"',u1+1):std::string::npos;
    if(u1==std::string::npos||u2==std::string::npos)return false;
    std::string u=json.substr(u1+1,u2-u1-1);
    assetUrl.assign(u.begin(),u.end());
    return true;}

// Returns <0 if a<b, 0 if equal, >0 if a>b, comparing dotted version
// strings ("1.2.3") component-by-component as integers.
static int VerCompare(const std::wstring& a,const std::wstring& b){
    auto split=[](const std::wstring& s){
        std::vector<int> parts;std::wstring cur;
        for(wchar_t c:s+L"."){
            if(c==L'.'){parts.push_back(cur.empty()?0:_wtoi(cur.c_str()));cur.clear();}
            else cur+=c;}
        return parts;};
    auto pa=split(a),pb=split(b);
    size_t n=(pa.size()>pb.size())?pa.size():pb.size();
    for(size_t i=0;i<n;i++){
        int va=i<pa.size()?pa[i]:0, vb=i<pb.size()?pb[i]:0;
        if(va!=vb)return va<vb?-1:1;}
    return 0;}

// Renaming a running EXE's own file is allowed on NTFS (the mapped image
// keeps running under the old inode) — so the update can swap files in
// place without a separate watcher process: rename current -> .old,
// rename the downloaded build -> current's name, relaunch, exit.
// Both renames use MOVEFILE_COPY_ALLOWED — plain MoveFileW fails outright
// (ERROR_NOT_SAME_DEVICE) when the install dir and %TEMP% are on different
// volumes, e.g. YMHub.exe living on a D: drive while %TEMP% is on C:, which
// is exactly the kind of setup a friend's machine can have and a dev box
// usually doesn't. That failure used to be swallowed silently (just
// `return`, no UI feedback) leaving the hub stuck showing "Скачивание..."
// forever — every failure path here now reports back through ok=false so
// the caller can surface a real error instead of hanging.
static bool ApplyUpdateAndRestart(const std::wstring& newExePath){
    wchar_t curPath[MAX_PATH];GetModuleFileNameW(nullptr,curPath,MAX_PATH);
    std::wstring oldPath=std::wstring(curPath)+L".old";
    DeleteFileW(oldPath.c_str()); // leftover from a previous update, if any
    if(!MoveFileExW(curPath,oldPath.c_str(),MOVEFILE_COPY_ALLOWED|MOVEFILE_REPLACE_EXISTING))
        return false;
    if(!MoveFileExW(newExePath.c_str(),curPath,MOVEFILE_COPY_ALLOWED|MOVEFILE_REPLACE_EXISTING)){
        MoveFileExW(oldPath.c_str(),curPath,MOVEFILE_COPY_ALLOWED|MOVEFILE_REPLACE_EXISTING); // best-effort rollback
        return false;}
    // The new process does a single-instance check on the same named
    // mutex — release it now (the old process is still shutting down
    // and would otherwise hold it long enough for the new one to see
    // ERROR_ALREADY_EXISTS and exit immediately, leaving nothing running).
    if(g_instanceMutex){CloseHandle(g_instanceMutex);g_instanceMutex=nullptr;}
    STARTUPINFOW si{};si.cb=sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd=L"\""+std::wstring(curPath)+L"\"";
    std::wstring cmdMut=cmd;
    if(!CreateProcessW(nullptr,cmdMut.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))
        return false;
    CloseHandle(pi.hProcess);CloseHandle(pi.hThread);
    RestartYM(); // so the new YMHub injects its fresh DLL right away
    if(g_hwnd)PostMessageW(g_hwnd,WM_CLOSE,0,0);
    return true;}

// WebView2's ICoreWebView2 is thread-affine to the UI thread that created
// it — calling PostWebMessageAsString from a background thread silently
// does nothing. So the background update-check thread only stashes the
// result here and posts WM_APP+33; the actual PostWebMessageAsString call
// happens in WndProc, on the UI thread.
static std::wstring g_updState, g_updLatest;
static void BroadcastUpdateStatus(const wchar_t* state,const std::wstring& latest=L""){
    std::wstring msg=
        L"{\"type\":\"update-status\",\"state\":\""+std::wstring(state)+L"\""
        L",\"current\":\"" YMHUB_VERSION_W L"\""
        L",\"latest\":\""+JsonEsc(latest)+L"\"}";
    if(g_hubWv)g_hubWv->PostWebMessageAsString(msg.c_str());}

static void PostUpdateStatus(const wchar_t* state,const std::wstring& latest=L""){
    g_updState=state;g_updLatest=latest;
    if(g_hwnd)PostMessageW(g_hwnd,WM_APP+33,0,0);}

// An update that's been found but not yet confirmed by the user — stashed
// here so the "confirm-update" message (sent after the hub shows its
// blurred warning dialog and the user clicks "Обновить") doesn't need to
// re-fetch the release info.
static std::wstring g_pendingUpdateUrl, g_pendingUpdateVer;

static void InstallPendingUpdate(){
    if(g_pendingUpdateUrl.empty())return;
    PostUpdateStatus(L"installing",g_pendingUpdateVer);
    wchar_t tmpDir[MAX_PATH];GetTempPathW(MAX_PATH,tmpDir);
    std::wstring newPath=std::wstring(tmpDir)+L"YMHub_update.exe";
    if(!HttpsDownloadToFile(g_pendingUpdateUrl,newPath)){
        PostUpdateStatus(L"error");return;}
    if(!ApplyUpdateAndRestart(newPath))PostUpdateStatus(L"error");}

// manual=true (the "Проверить обновления" button) pushes status updates
// to the hub UI at every step. Either way, finding an update never installs
// it silently — it always surfaces a "found" status so the hub can show its
// blurred confirmation dialog and wait for the user to approve the install.
static void CheckForUpdate(bool manual=false){
    if(manual)PostUpdateStatus(L"checking");
    std::string body;
    std::wstring path=L"/repos/" YMHUB_REPO_W L"/releases/latest";
    if(!HttpsGet(L"api.github.com",path.c_str(),body)){
        if(manual)PostUpdateStatus(L"error");return;}
    std::wstring tag,assetUrl;
    if(!ParseLatestRelease(body,tag,assetUrl)){
        if(manual)PostUpdateStatus(L"error");return;}
    std::wstring latest=tag;
    if(!latest.empty()&&(latest[0]==L'v'||latest[0]==L'V'))latest=latest.substr(1);
    if(VerCompare(YMHUB_VERSION_W,latest)>=0){
        if(manual)PostUpdateStatus(L"latest",latest);
        return;} // already current or newer

    g_pendingUpdateUrl=assetUrl;g_pendingUpdateVer=latest;
    PostUpdateStatus(L"found",latest);}

// ── Media / commands ──────────────────────────────────────
static void SendCmd(DWORD cmd){
    if(g_ipc){
        g_ipc->command=cmd;
        InterlockedIncrement(&g_ipc->cmdSeq);
    }else{
        HWND y=FindYM();if(!y)return;
        DWORD ac=0;
        switch(cmd){
        case YMHC_PREV:   ac=APPCOMMAND_MEDIA_PREVIOUSTRACK;break;
        case YMHC_NEXT:   ac=APPCOMMAND_MEDIA_NEXTTRACK;    break;
        case YMHC_TOGGLE: ac=APPCOMMAND_MEDIA_PLAY_PAUSE;   break;
        default:break;}
        if(ac)PostMessageW(y,WM_APPCOMMAND,(WPARAM)y,MAKELPARAM(0,ac));}}

static void DoPrev()  {SendCmd(YMHC_PREV);  Sleep(220);ParseYM();BroadcastState();}
static void DoNext()  {SendCmd(YMHC_NEXT);  Sleep(220);ParseYM();BroadcastState();}
static void DoToggle(){SendCmd(YMHC_TOGGLE);g_playing=!g_playing;BroadcastState();}
static void DoLike()  {SendCmd(YMHC_LIKE);  g_liked=!g_liked;   BroadcastState();}
static void DoDislike(){SendCmd(YMHC_DISLIKE);g_liked=false;     Sleep(220);ParseYM();BroadcastState();}

// ── Show/hide overlay ─────────────────────────────────────
static void InitWebView();  // forward

static void ShowOverlay(){
    if(!g_visible){
        g_visible=true;
        int x,y;GetCardPos(x,y);
        SetWindowPos(g_hwnd,HWND_TOPMOST,x,y,CW,CH,SWP_SHOWWINDOW|SWP_NOACTIVATE);
        if(!g_wvInited){g_wvInited=true;InitWebView();}
        else{
            if(g_ctrl){RECT r={0,0,CW,CH};g_ctrl->put_Bounds(r);g_ctrl->put_IsVisible(TRUE);}
            if(g_wv)g_wv->PostWebMessageAsString(L"{\"type\":\"show\"}");
            ParseYM();BroadcastState();}
    }else{
        g_visible=false;
        if(g_wv)g_wv->PostWebMessageAsString(L"{\"type\":\"hide\"}");
        else ShowWindow(g_hwnd,SW_HIDE);}
    // Notify hub about overlay state change
    BroadcastState();}

// ── Open/show hub ─────────────────────────────────────────
static void OpenHub(){
    if(!g_hub)return;
    ShowWindow(g_hub,SW_SHOW);
    SetForegroundWindow(g_hub);
    // WebView2 is a windowless/airspace control — without this, the very
    // first click right after the hub window opens only activates the
    // window and never reaches the page's DOM, requiring an extra click.
    if(g_hubCtrl)g_hubCtrl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);}

// ── Overlay HTML ──────────────────────────────────────────
static const wchar_t* HTML = LR"HTML(<!DOCTYPE html><html><head><meta charset="utf-8"><style>
*{margin:0;padding:0;box-sizing:border-box}
html,body{width:100%;height:100%;overflow:hidden;
  font-family:'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;
  background:#0e0e12;}
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

// ── Hub HTML ──────────────────────────────────────────────
static const wchar_t* HTML_HUB = LR"HUB(<!DOCTYPE html>
<html><head><meta charset='utf-8'><style>
*,*::before,*::after{margin:0;padding:0;box-sizing:border-box}
:root{--ac:#5b8fff;--ac2:#7c6fff;--bg:#0d0d14;--sb:#09090e;
  --card:rgba(255,255,255,.05);--bord:rgba(255,255,255,.08);
  --txt:rgba(255,255,255,.88);--txt2:rgba(255,255,255,.4)}
html,body{width:100%;height:100%;overflow:hidden;
  font-family:'Segoe UI Variable Text','Segoe UI',system-ui,sans-serif;
  background:var(--bg);color:var(--txt);user-select:none;}
::-webkit-scrollbar{width:4px}
::-webkit-scrollbar-track{background:transparent}
::-webkit-scrollbar-thumb{background:rgba(255,255,255,.14);border-radius:2px}

/* ── App shell ── */
#app{display:flex;height:100%}

/* ── Sidebar ── */
#sidebar{
  width:56px;background:var(--sb);
  display:flex;flex-direction:column;align-items:center;
  padding:12px 0 16px;gap:4px;
  border-right:1px solid var(--bord);flex-shrink:0;
}
#logo{
  width:34px;height:34px;border-radius:10px;margin-bottom:12px;
  background:linear-gradient(135deg,var(--ac),var(--ac2));
  display:flex;align-items:center;justify-content:center;
  font-size:17px;box-shadow:0 4px 16px rgba(91,143,255,.3);
}
.nav-btn{
  width:40px;height:40px;border:none;border-radius:10px;
  background:transparent;cursor:pointer;
  display:flex;align-items:center;justify-content:center;
  color:var(--txt2);transition:all .15s;position:relative;
}
.nav-btn:hover{background:rgba(255,255,255,.07);color:var(--txt);}
.nav-btn.active{background:rgba(91,143,255,.14);color:var(--ac);}
.nav-btn.active::before{
  content:'';position:absolute;left:0;top:50%;
  transform:translateY(-50%);
  width:3px;height:20px;border-radius:0 3px 3px 0;background:var(--ac);
}
.nav-btn svg{pointer-events:none;}
#dll-dot{
  margin-top:auto;width:8px;height:8px;border-radius:50%;
  background:#3a3a4a;transition:all .4s;
}
#dll-dot.ok{background:#3ddc84;box-shadow:0 0 8px rgba(61,220,132,.5);}
#dll-dot.err{background:#ff5555;}

/* ── Content ── */
#content{flex:1;overflow:hidden;position:relative;}
.tab{
  position:absolute;inset:0;padding:24px;
  overflow-y:auto;opacity:0;pointer-events:none;
  transform:translateY(6px);
  transition:opacity .22s ease,transform .22s ease;
}
.tab.active{opacity:1;pointer-events:auto;transform:translateY(0);}
.tab-title{font-size:20px;font-weight:700;color:#fff;margin-bottom:4px;}
.tab-sub{font-size:12px;color:var(--txt2);margin-bottom:20px;}

/* ── Player tab ── */
#track-card{
  display:flex;gap:16px;align-items:center;
  background:var(--card);border:1px solid var(--bord);
  border-radius:14px;padding:16px;margin-bottom:14px;
  position:relative;overflow:hidden;
}
#tc-bg{
  position:absolute;inset:-20px;
  background-size:cover;background-position:center;
  filter:blur(32px) brightness(.22) saturate(1.8);
  transform:scale(1.1);transition:opacity .6s;opacity:0;pointer-events:none;
}
#hub-art-wrap{
  width:104px;height:104px;border-radius:12px;overflow:hidden;
  flex-shrink:0;background:rgba(255,255,255,.07);
  display:flex;align-items:center;justify-content:center;
  font-size:32px;color:rgba(255,255,255,.2);position:relative;z-index:1;
  box-shadow:0 8px 28px rgba(0,0,0,.6);transition:box-shadow .4s;
}
#hub-art-wrap.playing{animation:artP 2.4s ease-in-out infinite;}
@keyframes artP{0%,100%{box-shadow:0 8px 28px rgba(0,0,0,.6),0 0 0 2.5px rgba(255,255,255,.12)}50%{box-shadow:0 8px 28px rgba(0,0,0,.6),0 0 0 4px rgba(255,255,255,.05)}}
#hub-art-img{width:104px;height:104px;object-fit:cover;display:none;}
#hub-track-info{flex:1;min-width:0;position:relative;z-index:1;}
#hub-track-name{
  font-size:17px;font-weight:700;
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:5px;
}
#hub-artist-name{
  font-size:13px;color:var(--txt2);
  white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-bottom:8px;
}
#hub-badge{
  display:none;align-items:center;gap:6px;
  font-size:11px;color:rgba(91,143,255,.85);
}
#hub-badge .dot{width:6px;height:6px;border-radius:50%;background:var(--ac);
  animation:bdot .9s ease-in-out infinite;}
@keyframes bdot{0%,100%{opacity:1}50%{opacity:.25}}

#hub-ctrls{
  display:flex;align-items:center;justify-content:center;
  gap:8px;padding:10px 0 14px;
}
.hcb{border:none;border-radius:50%;cursor:pointer;
  display:flex;align-items:center;justify-content:center;transition:all .15s;}
.hcb.skip{
  width:40px;height:40px;background:rgba(255,255,255,.07);
  color:rgba(255,255,255,.55);
}
.hcb.skip:hover{background:rgba(255,255,255,.14);color:#fff;transform:scale(1.06);}
.hcb.skip:active{transform:scale(.88);}
#hub-pbtn{
  width:52px;height:52px;
  background:linear-gradient(135deg,var(--ac),var(--ac2));
  color:#fff;box-shadow:0 4px 20px rgba(91,143,255,.35);
}
#hub-pbtn:hover{filter:brightness(1.1);transform:scale(1.05);}
#hub-pbtn:active{transform:scale(.92);}
.hcb-act{
  width:36px;height:36px;background:rgba(255,255,255,.06);
  color:rgba(255,255,255,.4);
}
.hcb-act:hover{background:rgba(255,255,255,.12);color:rgba(255,255,255,.8);transform:scale(1.07);}
#hub-like.liked{background:rgba(255,60,90,.2);color:#ff4d6d;animation:hpop .26s cubic-bezier(.34,1.6,.64,1);}
@keyframes hpop{0%{transform:scale(1)}40%{transform:scale(1.3)}100%{transform:scale(1)}}
.csep{width:1px;height:26px;background:var(--bord);margin:0 4px;}

.section{
  background:var(--card);border:1px solid var(--bord);
  border-radius:12px;padding:14px 16px;margin-bottom:12px;
}
.sec-head{display:flex;align-items:center;justify-content:space-between;margin-bottom:0;}
.sec-title{font-size:13px;font-weight:600;color:var(--txt);}
.sec-desc{font-size:11.5px;color:var(--txt2);margin-top:3px;}
.pill-btn{
  height:28px;padding:0 14px;border-radius:20px;
  border:1px solid var(--bord);background:rgba(255,255,255,.06);
  font-family:inherit;font-size:12px;color:var(--txt2);cursor:pointer;transition:all .15s;
}
.pill-btn:hover{border-color:rgba(91,143,255,.45);color:var(--ac);background:rgba(91,143,255,.1);}
.pill-btn.on{background:rgba(91,143,255,.15);border-color:rgba(91,143,255,.4);color:var(--ac);}

#pos-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:6px;margin-top:12px;}
.pos-cell{
  height:36px;border-radius:8px;
  border:1px solid var(--bord);background:rgba(255,255,255,.03);
  cursor:pointer;display:flex;align-items:center;justify-content:center;
  font-size:11px;color:var(--txt2);transition:all .15s;gap:5px;
}
.pos-cell:hover{background:rgba(91,143,255,.12);border-color:rgba(91,143,255,.3);color:var(--ac);}
.pos-cell.active{background:rgba(91,143,255,.18);border-color:var(--ac);color:var(--ac);font-weight:600;}

/* ── Keys tab ── */
.key-row{
  display:flex;align-items:center;padding:0 2px;height:54px;gap:10px;
  border-radius:10px;transition:background .15s;
}
.key-row:hover{background:rgba(255,255,255,.03);}
.kr-icon{
  width:32px;height:32px;border-radius:8px;flex-shrink:0;
  background:rgba(255,255,255,.06);
  display:flex;align-items:center;justify-content:center;font-size:15px;
}
.kr-lbl{flex:1;min-width:0;}
.kr-act{font-size:12.5px;font-weight:500;}
.kr-sub{font-size:10.5px;color:var(--txt2);margin-top:1px;}
.key-combo{display:flex;align-items:center;gap:4px;flex-shrink:0;min-width:148px;justify-content:flex-end;}
.kchip{
  height:22px;padding:0 7px;border-radius:5px;
  background:rgba(255,255,255,.09);border:1px solid rgba(255,255,255,.13);
  font-size:11px;font-weight:600;color:rgba(255,255,255,.62);
  display:flex;align-items:center;white-space:nowrap;
}
.kchip.mod{background:rgba(91,143,255,.15);border-color:rgba(91,143,255,.3);color:rgba(130,160,255,.9);}
.kplus{color:rgba(255,255,255,.22);font-size:10px;margin:0 1px;}
.knone{font-size:11px;color:rgba(255,255,255,.2);font-style:italic;}
.rec-btn{
  height:28px;padding:0 12px;border-radius:7px;
  border:1px solid var(--bord);background:rgba(255,255,255,.06);
  font-family:inherit;font-size:11px;font-weight:500;color:var(--txt2);
  cursor:pointer;transition:all .15s;white-space:nowrap;flex-shrink:0;
  display:flex;align-items:center;gap:5px;
}
.rec-btn:hover{border-color:rgba(91,143,255,.4);color:var(--ac);background:rgba(91,143,255,.1);}
.rec-btn.recording{
  border-color:var(--ac);background:rgba(91,143,255,.12);color:var(--ac);
  animation:recP 1.1s ease-in-out infinite;
}
@keyframes recP{0%,100%{box-shadow:0 0 0 0 rgba(91,143,255,.3)}50%{box-shadow:0 0 0 6px rgba(91,143,255,0)}}
.rec-dot{width:6px;height:6px;border-radius:50%;background:currentColor;
  animation:rdot .8s step-end infinite;}
@keyframes rdot{0%,100%{opacity:1}50%{opacity:0}}
.kr-div{height:1px;background:var(--bord);margin:2px;}
#keys-footer,#ymkeys-footer{display:flex;justify-content:flex-end;gap:8px;margin-top:16px;padding-top:16px;border-top:1px solid var(--bord);}
.kf-btn{
  height:32px;padding:0 18px;border-radius:8px;border:none;
  font-family:inherit;font-size:12.5px;font-weight:500;cursor:pointer;transition:all .15s;
}
#kbtn-reset,#ymkbtn-reset{background:rgba(255,255,255,.07);color:var(--txt2);}
#kbtn-reset:hover,#ymkbtn-reset:hover{background:rgba(255,255,255,.12);color:#fff;}
#kbtn-save,#ymkbtn-save{
  background:linear-gradient(135deg,var(--ac),var(--ac2));color:#fff;
  box-shadow:0 3px 12px rgba(91,143,255,.3);
}
#kbtn-save:hover,#ymkbtn-save:hover{filter:brightness(1.1);}
#kbtn-save:active,#ymkbtn-save:active{filter:brightness(.9);transform:scale(.97);}

/* ── Plugins tab ── */
.plugin-card{
  background:var(--card);border:1px solid var(--bord);
  border-radius:14px;padding:18px;margin-bottom:12px;
}
.pc-head{display:flex;align-items:center;gap:14px;margin-bottom:14px;}
.pc-icon{
  width:42px;height:42px;border-radius:11px;flex-shrink:0;
  display:flex;align-items:center;justify-content:center;font-size:20px;
}
.pc-icon.blue{background:rgba(91,143,255,.18);}
.pc-icon.violet{background:rgba(124,111,255,.18);}
.pc-icon.discord{background:rgba(88,101,242,.18);}
.pc-name{font-size:14px;font-weight:600;margin-bottom:3px;}
.pc-desc{font-size:12px;color:var(--txt2);}
.pc-row{display:flex;align-items:center;gap:10px;}
.sdot{width:7px;height:7px;border-radius:50%;background:#3a3a4a;flex-shrink:0;transition:all .4s;}
.sdot.ok{background:#3ddc84;box-shadow:0 0 7px rgba(61,220,132,.5);}
.sdot.err{background:#ff5555;}
.stxt{font-size:12px;color:var(--txt2);flex:1;}
.pc-btn{
  height:30px;padding:0 16px;border-radius:8px;
  border:1px solid var(--bord);background:rgba(255,255,255,.07);
  font-family:inherit;font-size:12px;color:var(--txt2);cursor:pointer;transition:all .15s;
}
.pc-btn:hover{border-color:rgba(91,143,255,.4);color:var(--ac);background:rgba(91,143,255,.1);}
.pc-btn.primary{background:linear-gradient(135deg,var(--ac),var(--ac2));border:none;color:#fff;font-weight:600;}
.pc-btn.primary:hover{filter:brightness(1.1);}
.pc-btn:disabled{opacity:.35;cursor:default;pointer-events:none;}

/* ── Tweaks tab ── */
.tw-row{
  display:flex;align-items:center;gap:14px;padding:14px 4px;
  border-bottom:1px solid var(--bord);
}
.tw-row:last-child{border-bottom:none;}
.tw-lbl{flex:1;min-width:0;}
.tw-name{font-size:13.5px;font-weight:500;}
.tw-desc{font-size:11.5px;color:var(--txt2);margin-top:2px;}
.tw-switch{
  position:relative;width:38px;height:22px;flex-shrink:0;border-radius:99px;
  background:rgba(255,255,255,.12);border:1px solid var(--bord);
  cursor:pointer;transition:background .35s ease,border-color .35s ease,box-shadow .35s ease;
}
.tw-switch.on{
  background:linear-gradient(135deg,var(--ac),var(--ac2));border-color:transparent;
  box-shadow:0 0 10px rgba(91,143,255,.45);
}
.tw-switch:active{transform:scale(.93);}
.tw-switch .knob{
  position:absolute;top:2px;left:2px;width:16px;height:16px;border-radius:50%;
  background:#fff;box-shadow:0 1px 3px rgba(0,0,0,.4);
  transition:left .35s cubic-bezier(.34,1.56,.64,1);
}
.tw-switch.on .knob{left:18px;}
.tw-row{flex-wrap:wrap;}
.tw-input{
  flex-basis:100%;margin-top:10px;display:none;
  background:rgba(255,255,255,.05);border:1px solid var(--bord);border-radius:8px;
  color:var(--txt);font-size:13px;padding:8px 10px;outline:none;
}
.tw-input:focus{border-color:rgba(91,143,255,.4);}
.tw-row.on .tw-input{display:block;}

.cc-wrap{margin-top:18px;padding:16px;border-radius:14px;background:var(--card);border:1px solid var(--bord);}
.cc-title{font-size:14px;font-weight:700;margin-bottom:4px;}
.cc-desc{font-size:12px;color:var(--txt2);line-height:1.55;margin-bottom:12px;}
.cc-desc code{font-family:Consolas,'Cascadia Code',monospace;background:rgba(255,255,255,.07);padding:1px 5px;border-radius:5px;}
.cc-area{
  width:100%;min-height:130px;resize:vertical;
  background:rgba(255,255,255,.05);border:1px solid var(--bord);border-radius:8px;
  color:var(--txt);font-size:12.5px;font-family:Consolas,'Cascadia Code',monospace;
  padding:10px 12px;outline:none;line-height:1.5;
}
.cc-area:focus{border-color:rgba(91,143,255,.4);}

/* ── Update confirmation modal ── */
#upd-scrim{
  position:fixed;inset:0;z-index:100;display:none;
  align-items:center;justify-content:center;
  background:rgba(8,8,14,.45);backdrop-filter:blur(10px);-webkit-backdrop-filter:blur(10px);
  opacity:0;transition:opacity .25s ease;
}
#upd-scrim.show{display:flex;opacity:1;}
#upd-modal{
  width:280px;border-radius:20px;padding:22px 20px 18px;
  background:#1d1b2a;border:1px solid rgba(255,255,255,.08);
  box-shadow:0 16px 48px rgba(0,0,0,.5);
  transform:scale(.9) translateY(8px);transition:transform .25s ease;
}
#upd-scrim.show #upd-modal{transform:scale(1) translateY(0);}
#upd-modal-icon{
  width:44px;height:44px;border-radius:14px;margin-bottom:14px;
  background:linear-gradient(135deg,var(--ac),var(--ac2));
  display:flex;align-items:center;justify-content:center;font-size:20px;
  box-shadow:0 4px 16px rgba(91,143,255,.35);
}
#upd-modal-title{font-size:15px;font-weight:600;margin-bottom:6px;color:rgba(255,255,255,.92);}
#upd-modal-sub{font-size:12.5px;line-height:1.5;color:rgba(255,255,255,.5);margin-bottom:18px;}
#upd-modal-actions{display:flex;justify-content:flex-end;gap:8px;}
.upd-btn2{
  height:32px;padding:0 16px;border-radius:9px;border:none;
  font-family:inherit;font-size:12.5px;font-weight:600;cursor:pointer;transition:all .15s;
}
.upd-btn2.ghost{background:transparent;color:rgba(255,255,255,.5);}
.upd-btn2.ghost:hover{background:rgba(255,255,255,.06);color:rgba(255,255,255,.8);}
.upd-btn2.go{background:linear-gradient(135deg,var(--ac),var(--ac2));color:#fff;}
.upd-btn2.go:hover{filter:brightness(1.1);}
.upd-btn2:disabled{opacity:.4;cursor:default;pointer-events:none;}
</style></head>
<body>
<div id='app'>
  <!-- Sidebar -->
  <nav id='sidebar'>
    <div id='logo'>♪</div>
    <button class='nav-btn active' id='nav-player' onclick='nav("player")' title='Плеер'>
      <svg width='19' height='19' viewBox='0 0 24 24' fill='currentColor'>
        <path d='M12 3v10.55A4 4 0 1 0 14 17V7h4V3h-6z'/>
      </svg>
    </button>
    <button class='nav-btn' id='nav-keys' onclick='nav("keys")' title='Горячие клавиши'>
      <svg width='19' height='19' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>
        <rect x='2' y='6' width='20' height='12' rx='2'/>
        <path d='M6 10h.01M10 10h.01M14 10h.01M18 10h.01M8 14h8'/>
      </svg>
    </button>
    <button class='nav-btn' id='nav-plugins' onclick='nav("plugins")' title='Плагины'>
      <svg width='19' height='19' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>
        <path d='M12 2L2 7l10 5 10-5-10-5zM2 17l10 5 10-5M2 12l10 5 10-5'/>
      </svg>
    </button>
    <button class='nav-btn' id='nav-tweaks' onclick='nav("tweaks")' title='Твики'>
      <svg width='19' height='19' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>
        <path d='M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3'/>
        <circle cx='4' cy='12' r='2'/><circle cx='12' cy='10' r='2'/><circle cx='20' cy='14' r='2'/>
      </svg>
    </button>
    <button class='nav-btn' id='nav-about' onclick='nav("about")' title='О приложении'>
      <svg width='19' height='19' viewBox='0 0 24 24' fill='none' stroke='currentColor' stroke-width='1.8' stroke-linecap='round' stroke-linejoin='round'>
        <circle cx='12' cy='12' r='10'/><line x1='12' y1='16' x2='12' y2='11'/><circle cx='12' cy='8' r='.5' fill='currentColor'/>
      </svg>
    </button>
    <div id='dll-dot'></div>
  </nav>

  <!-- Content -->
  <main id='content'>

    <!-- ── Player tab ── -->
    <div class='tab active' id='tab-player'>
      <div class='tab-title'>Плеер</div>
      <div class='tab-sub'>Управление воспроизведением Яндекс Музыки</div>

      <div id='track-card'>
        <div id='tc-bg'></div>
        <div id='hub-art-wrap'>
          <span id='hub-art-ph'>♪</span>
          <img id='hub-art-img'>
        </div>
        <div id='hub-track-info'>
          <div id='hub-track-name'>Нет воспроизведения</div>
          <div id='hub-artist-name'>&nbsp;</div>
          <div id='hub-badge'><div class='dot'></div>Воспроизводится</div>
        </div>
      </div>

      <div id='hub-ctrls'>
        <button class='hcb skip' onclick='send("prev")' title='Предыдущий'>
          <svg width='16' height='16' viewBox='0 0 15 15' fill='currentColor'>
            <rect x='1.5' y='1.5' width='2.5' height='12' rx='1.1'/>
            <path d='M13 2.5 5.5 7.5 13 12.5V2.5z'/>
          </svg>
        </button>
        <button class='hcb' id='hub-pbtn' onclick='hubPlay()'>
          <svg id='hi-play' width='19' height='19' viewBox='0 0 17 17' fill='currentColor'>
            <path d='M5.5 3.5 14 8.5 5.5 13.5V3.5z'/>
          </svg>
          <svg id='hi-pause' width='19' height='19' viewBox='0 0 17 17' fill='currentColor' style='display:none'>
            <rect x='3.5' y='3' width='3.2' height='11' rx='1.3'/>
            <rect x='10.3' y='3' width='3.2' height='11' rx='1.3'/>
          </svg>
        </button>
        <button class='hcb skip' onclick='send("next")' title='Следующий'>
          <svg width='16' height='16' viewBox='0 0 15 15' fill='currentColor'>
            <rect x='11' y='1.5' width='2.5' height='12' rx='1.1'/>
            <path d='M2 2.5l7.5 5L2 12.5V2.5z'/>
          </svg>
        </button>
        <div class='csep'></div>
        <button class='hcb hcb-act' id='hub-like' onclick='hubLike()' title='Лайк'>
          <svg width='16' height='16' viewBox='0 0 24 24' fill='currentColor'>
            <path d='M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z'/>
          </svg>
        </button>
        <button class='hcb hcb-act' onclick='send("dislike")' title='Дизлайк'>
          <svg width='14' height='14' viewBox='0 0 24 24' fill='currentColor'>
            <path d='M15 3H6c-.83 0-1.54.5-1.84 1.22l-3.02 7.05c-.09.23-.14.47-.14.73v2c0 1.1.9 2 2 2h6.31l-.95 4.57-.03.32c0 .41.17.79.44 1.06L9.83 23l6.59-6.59c.36-.36.58-.86.58-1.41V5c0-1.1-.9-2-2-2zm4 0v12h4V3h-4z'/>
          </svg>
        </button>
      </div>

      <div class='section'>
        <div class='sec-head'>
          <div>
            <div class='sec-title'>Мини-плеер</div>
            <div class='sec-desc'>Компактный виджет поверх всех окон</div>
          </div>
          <button class='pill-btn' id='ovl-btn' onclick='send("overlay-toggle")'>Показать</button>
        </div>
        <div id='pos-grid'>
          <div class='pos-cell' id='pc3' onclick='send("pos:3")'>↖ Сверху лево</div>
          <div class='pos-cell' id='pc4' onclick='send("pos:4")'>↑ Сверху</div>
          <div class='pos-cell' id='pc5' onclick='send("pos:5")'>↗ Сверху право</div>
          <div class='pos-cell' id='pc0' onclick='send("pos:0")'>↙ Снизу лево</div>
          <div class='pos-cell' id='pc1' onclick='send("pos:1")'>↓ Снизу</div>
          <div class='pos-cell' id='pc2' onclick='send("pos:2")'>↘ Снизу право</div>
        </div>
      </div>
    </div>

    <!-- ── Keys tab ── -->
    <div class='tab' id='tab-keys'>
      <div class='tab-title'>Горячие клавиши</div>
      <div class='tab-sub'>Нажмите «Изменить» чтобы назначить новую комбинацию</div>
      <div id='key-rows'></div>
      <div id='keys-footer'>
        <button class='kf-btn' id='kbtn-reset' onclick='resetKeys()'>По умолчанию</button>
        <button class='kf-btn' id='kbtn-save' onclick='saveKeys()'>Сохранить</button>
      </div>

      <div class='tab-title' style='margin-top:30px;font-size:16px;'>Встроенные клавиши Яндекс Музыки</div>
      <div class='tab-sub'>Работают только когда окно Яндекс Музыки активно</div>
      <div id='ymkey-rows'></div>
      <div id='ymkeys-footer'>
        <button class='kf-btn' id='ymkbtn-reset' onclick='resetYmKeys()'>По умолчанию</button>
        <button class='kf-btn' id='ymkbtn-save' onclick='saveYmKeys()'>Сохранить</button>
      </div>
    </div>

    <!-- ── Plugins tab ── -->
    <div class='tab' id='tab-plugins'>
      <div class='tab-title'>Плагины</div>
      <div class='tab-sub'>Расширения для Яндекс Музыки</div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon blue'>🔗</div>
          <div>
            <div class='pc-name'>YM Sync</div>
            <div class='pc-desc'>Прямое управление через DLL внутри процесса ЯМ — лайки, дизлайки, шаффл и повтор без фокуса</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='sdot' id='sync-dot'></div>
          <div class='stxt' id='sync-txt'>Не загружена</div>
          <button class='pc-btn primary' id='sync-btn' onclick='send("inject")'>Загрузить</button>
        </div>
      </div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon violet'>🎴</div>
          <div>
            <div class='pc-name'>YM Overlay</div>
            <div class='pc-desc'>Компактный мини-плеер поверх всех окон — обложка, название, управление</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='sdot' id='ovl-dot'></div>
          <div class='stxt' id='ovl-txt'>Скрыт</div>
          <button class='pc-btn' id='ovl-plug-btn' onclick='send("overlay-toggle")'>Показать</button>
        </div>
      </div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon discord'>🎧</div>
          <div>
            <div class='pc-name'>Discord Rich Presence</div>
            <div class='pc-desc'>Показывает играющий трек в статусе Discord — название и исполнителя</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='sdot' id='drpc-dot'></div>
          <div class='stxt' id='drpc-txt'>Выключено</div>
          <button class='pc-btn' id='drpc-btn' onclick='send("drpc-toggle")'>Включить</button>
        </div>
      </div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon blue'>⚡</div>
          <div>
            <div class='pc-name'>Меню в Яндекс Музыке</div>
            <div class='pc-desc'>Весь YMHub прямо внутри окна ЯМ — открывается по Shift, без отдельного окна хаба (бета)</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='sdot' id='cheat-dot'></div>
          <div class='stxt' id='cheat-txt'>Выключено</div>
          <button class='pc-btn' id='cheat-btn' onclick='send("cheatmenu-toggle")'>Включить</button>
        </div>
      </div>
    </div>

    <!-- ── Tweaks tab ── -->
    <div class='tab' id='tab-tweaks'>
      <div class='tab-title'>Твики</div>
      <div class='tab-sub'>Скрыть отдельные элементы интерфейса Яндекс Музыки — применяется сразу</div>
      <div id='tweak-rows'></div>

      <div class='cc-wrap'>
        <div class='cc-title'>Свой CSS</div>
        <div class='cc-desc'>Полный контроль интерфейса Яндекс Музыки — любые правила CSS, например <code>[class*=&quot;VibePage_wheel__&quot;]{display:none!important}</code>. Применяется сразу, сохраняется между запусками. Никогда не писали CSS? <a href='https://onemorefix1337.github.io/ymhub/css-guide.html' target='_blank' style='color:var(--ac)'>Гайд для новичков →</a></div>
        <textarea id='cc-input' class='cc-area' spellcheck='false' placeholder='.selector{ ... }' onchange='send("set-custom-css:"+this.value)'></textarea>
      </div>
    </div>

    <!-- ── About tab ── -->
    <div class='tab' id='tab-about'>
      <div class='tab-title'>О приложении</div>
      <div class='tab-sub'>Версия, обновления и информация</div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon blue'>♪</div>
          <div>
            <div class='pc-name'>YMHub</div>
            <div class='pc-desc'>Оверлей-плеер, горячие клавиши и фоновая синхронизация для Яндекс Музыки</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='stxt'>Версия: <b id='about-ver'>—</b></div>
        </div>
        <div class='pc-row'>
          <div class='sdot' id='upd-dot'></div>
          <div class='stxt' id='upd-txt'>Нажмите, чтобы проверить обновления</div>
          <button class='pc-btn primary' id='upd-btn' onclick='checkUpdate()'>Проверить обновления</button>
        </div>
      </div>

      <div class='plugin-card'>
        <div class='pc-head'>
          <div class='pc-icon violet'>ℹ</div>
          <div>
            <div class='pc-name'>Репозиторий</div>
            <div class='pc-desc'>Исходный код и релизы на GitHub</div>
          </div>
        </div>
        <div class='pc-row'>
          <div class='stxt'>github.com/onemorefix1337/ymhub</div>
        </div>
      </div>
    </div>

  </main>
</div>

<div id='upd-scrim'>
  <div id='upd-modal'>
    <div id='upd-modal-icon'>↑</div>
    <div id='upd-modal-title'>Доступно обновление</div>
    <div id='upd-modal-sub'></div>
    <div id='upd-modal-actions'>
      <button class='upd-btn2 ghost' id='upd-modal-later' onclick='dismissUpdate()'>Позже</button>
      <button class='upd-btn2 go' id='upd-modal-go' onclick='confirmUpdate()'>Обновить</button>
    </div>
  </div>
</div>

<canvas id='cv' style='display:none' width='200' height='200'></canvas>
<script>
const send = a => window.chrome.webview.postMessage(a);
const $ = id => document.getElementById(id);

// Navigation
function nav(tab) {
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
  $('nav-'+tab).classList.add('active');
  $('tab-'+tab).classList.add('active');
}

// Player
let hPlaying=false,hLiked=false;
function hubPlay(){send('toggle');}
function hubLike(){$('hub-like').classList.toggle('liked');send('like');}

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
  return `rgb(${R},${G},${B})`;}

function updateState(d) {
  // Track info
  const tn=$('hub-track-name'),an=$('hub-artist-name');
  tn.textContent = d.track||'Нет воспроизведения';
  an.textContent = d.artist||'';
  hPlaying=!!d.playing; hLiked=!!d.liked;
  $('hi-play').style.display=hPlaying?'none':'';
  $('hi-pause').style.display=hPlaying?'':'none';
  $('hub-badge').style.display=hPlaying?'flex':'none';
  $('hub-art-wrap').classList.toggle('playing',hPlaying);
  $('hub-like').classList.toggle('liked',hLiked);

  // Art
  if(d.art){
    const src='data:image/png;base64,'+d.art;
    $('hub-art-img').src=src;$('hub-art-img').style.display='block';
    $('hub-art-ph').style.display='none';
    const tmp=new Image();tmp.onload=()=>{
      const ac=extractAccent(tmp);
      document.documentElement.style.setProperty('--ac',ac);
      document.documentElement.style.setProperty('--ac2',ac);
      $('tc-bg').style.backgroundImage=`url(${src})`;
      $('tc-bg').style.opacity='1';
    };tmp.src=src;
  } else {
    $('hub-art-img').style.display='none';$('hub-art-ph').style.display='';
    $('tc-bg').style.opacity='0';
  }

  // Overlay state
  const ovl=!!d.overlayVisible;
  $('ovl-btn').textContent=ovl?'Скрыть':'Показать';
  $('ovl-btn').classList.toggle('on',ovl);
  $('ovl-dot').classList.toggle('ok',ovl);
  $('ovl-txt').textContent=ovl?'Показан':'Скрыт';
  $('ovl-plug-btn').textContent=ovl?'Скрыть':'Показать';

  // DLL state
  const dll=!!d.dllLoaded;
  $('dll-dot').classList.toggle('ok',dll);
  $('dll-dot').classList.toggle('err',!dll);
  $('sync-dot').classList.toggle('ok',dll);
  $('sync-dot').classList.toggle('err',!dll);
  $('sync-txt').textContent=dll?'Активна — лайки и дизлайки работают':'Не загружена';
  $('sync-btn').disabled=dll;
  $('sync-btn').textContent=dll?'✓ Загружена':'Загрузить';
  $('sync-btn').className=dll?'pc-btn':'pc-btn primary';

  // Discord Rich Presence state
  const drpcOn=!!d.drpcEnabled,drpcConn=!!d.drpcConnected;
  $('drpc-dot').classList.toggle('ok',drpcOn&&drpcConn);
  $('drpc-dot').classList.toggle('err',drpcOn&&!drpcConn);
  $('drpc-txt').textContent=!drpcOn?'Выключено':(drpcConn?'Подключено':'Discord не найден');
  $('drpc-btn').textContent=drpcOn?'Выключить':'Включить';

  // In-page cheat menu state
  const cheatOn=!!d.cheatMenuEnabled;
  $('cheat-dot').classList.toggle('ok',cheatOn);
  $('cheat-txt').textContent=cheatOn?'Включено — Shift внутри ЯМ':'Выключено';
  $('cheat-btn').textContent=cheatOn?'Выключить':'Включить';

  // Position
  if(typeof d.pos==='number'){
    document.querySelectorAll('.pos-cell').forEach(c=>c.classList.remove('active'));
    const pc=$('pc'+d.pos);if(pc)pc.classList.add('active');
  }
}

// Keys
const VK={8:'Bksp',9:'Tab',13:'Enter',27:'Esc',32:'Space',
  33:'PgUp',34:'PgDn',35:'End',36:'Home',
  37:'←',38:'↑',39:'→',40:'↓',45:'Ins',46:'Del',
  48:'0',49:'1',50:'2',51:'3',52:'4',53:'5',54:'6',55:'7',56:'8',57:'9',
  65:'A',66:'B',67:'C',68:'D',69:'E',70:'F',71:'G',72:'H',73:'I',74:'J',
  75:'K',76:'L',77:'M',78:'N',79:'O',80:'P',81:'Q',82:'R',83:'S',84:'T',
  85:'U',86:'V',87:'W',88:'X',89:'Y',90:'Z',
  96:'Num0',97:'Num1',98:'Num2',99:'Num3',100:'Num4',
  101:'Num5',102:'Num6',103:'Num7',104:'Num8',105:'Num9',
  112:'F1',113:'F2',114:'F3',115:'F4',116:'F5',117:'F6',
  118:'F7',119:'F8',120:'F9',121:'F10',122:'F11',123:'F12'};
const MODS_VK=new Set([16,17,18,91,92]);
const DEF_KEYS=[{m:3,v:49},{m:3,v:37},{m:3,v:39},{m:3,v:32},{m:0,v:0},{m:0,v:0}];
const ROW_INFO=[
  {i:'👁',a:'Показать / скрыть',s:'Открыть или закрыть плеер'},
  {i:'⏮',a:'Предыдущий трек',s:'Вернуться к предыдущей песне'},
  {i:'⏭',a:'Следующий трек',s:'Пропустить текущую песню'},
  {i:'⏯',a:'Пауза / Играть',s:'Переключить воспроизведение'},
  {i:'♥',a:'Лайк',s:'Добавить в понравившиеся'},
  {i:'👎',a:'Дизлайк',s:'Не нравится, следующая'}
];
let keys=DEF_KEYS.map(k=>({...k}));
let recording=-1;

// YM's own in-page hotkeys (only work while YM's window has focus — see
// YM_ACTION_* in shared/ipc.h). {m:0,v:0} means "not remapped, use YM's
// own default". DEF_YM_DISPLAY is only for the "default" chip shown when
// a row hasn't been overridden — the host doesn't need to know it.
const DEF_YM_DISPLAY=['K','M','L','J','↑','↓','F','D','R','S','N','P','W'];
const YM_ROW_INFO=[
  {a:'Пауза / Играть',s:'Переключить воспроизведение'},
  {a:'Без звука',s:'Включить или выключить звук'},
  {a:'Промотать вперёд',s:'Перемотка трека вперёд'},
  {a:'Промотать назад',s:'Перемотка трека назад'},
  {a:'Громче',s:'Увеличить громкость'},
  {a:'Тише',s:'Уменьшить громкость'},
  {a:'Лайк',s:'Добавить в понравившиеся'},
  {a:'Дизлайк',s:'Не нравится'},
  {a:'Повтор',s:'Переключить режим повтора'},
  {a:'Шаффл',s:'Случайный порядок'},
  {a:'Следующий трек',s:'Пропустить текущую песню'},
  {a:'Предыдущий трек',s:'Вернуться к предыдущей песне'},
  {a:'Полный экран',s:'Открыть / закрыть фулскрин плеер'}
];
let ymKeys=DEF_YM_DISPLAY.map(()=>({m:0,v:0}));
let ymRecording=-1;

// "Твики" tab — bit index matches TWEAK_* in shared/ipc.h.
const TWEAK_INFO=[
  {n:'AI-комментарии о треке',d:'Подсказка со искрой под плеером'},
  {n:'Анимация фона плеера',d:'Цветной волновой фон Моей волны'},
  {n:'Плашка «Версия приложения»',d:'Кнопка с заметками о новой версии'},
  {n:'Барабан рекомендаций',d:'Карточки плейлистов слева — плеер займёт всю ширину'},
  {n:'Плашка «Моя волна обновилась»',d:'Уведомление в правом верхнем углу'},
  {n:'Лишние разделы меню',d:'«Для вас и Тренды», «Концерты», «Книги и подкасты»'},
  {n:'Плюс-бейдж в профиле',d:'Ссылка и плашка подписки рядом с именем'},
  {n:'Крупная обложка трека',d:'Увеличивает обложку на странице «Моя волна»'},
  {n:'Скрыть имя пользователя',d:'Своё имя вместо настоящего, или просто «Скрыто»',input:true}
];
let tweaksMask=0,customName='',customCss='';
function renderTweaks(){
  // Built once; later mask updates only flip the .on class on the existing
  // switch nodes (see applyTweaksMask) so the CSS transition actually has
  // a "before" frame to animate from — rebuilding the nodes from scratch
  // on every toggle made them appear already in their final state.
  const cont=$('tweak-rows');if(!cont)return;
  if(cont.children.length===TWEAK_INFO.length){applyTweaksMask();return;}
  cont.innerHTML='';
  TWEAK_INFO.forEach((t,i)=>{
    const on=(tweaksMask&(1<<i))!==0;
    const row=document.createElement('div');row.className='tw-row'+(on?' on':'');
    row.innerHTML=`<div class='tw-lbl'><div class='tw-name'>${t.n}</div><div class='tw-desc'>${t.d}</div></div>
      <div class='tw-switch${on?' on':''}' id='tws${i}' onclick='toggleTweak(${i})'><div class='knob'></div></div>`+
      (t.input?`<input class='tw-input' id='twi${i}' placeholder='Скрыто' maxlength='31' value='${customName}' onchange='send("set-custom-name:"+this.value)'>`:'');
    cont.appendChild(row);});}
function applyTweaksMask(){
  TWEAK_INFO.forEach((t,i)=>{
    const sw=$('tws'+i);if(!sw)return;
    const on=(tweaksMask&(1<<i))!==0;
    sw.classList.toggle('on',on);
    sw.parentElement.classList.toggle('on',on);});}
function toggleTweak(i){
  tweaksMask^=(1<<i);applyTweaksMask(); // optimistic, instant — see renderTweaks comment
  send('toggle-tweak:'+i);}

function buildRows(){
  const cont=$('key-rows');cont.innerHTML='';
  for(let i=0;i<6;i++){
    if(i>0){const d=document.createElement('div');d.className='kr-div';cont.appendChild(d);}
    const row=document.createElement('div');row.className='key-row';row.id='kr'+i;
    row.innerHTML=`<div class='kr-icon'>${ROW_INFO[i].i}</div>
      <div class='kr-lbl'><div class='kr-act'>${ROW_INFO[i].a}</div><div class='kr-sub'>${ROW_INFO[i].s}</div></div>
      <div class='key-combo' id='kc${i}'></div>
      <button class='rec-btn' id='rb${i}' onclick='startRec(${i})'>Изменить</button>`;
    cont.appendChild(row);}
  for(let i=0;i<6;i++)renderCombo(i);}

function renderCombo(i){
  const kc=$('kc'+i),key=keys[i];kc.innerHTML='';
  if(!key.v){kc.innerHTML="<span class='knone'>не задано</span>";return;}
  const chips=[];
  if(key.m&1)chips.push({t:'Ctrl',mod:true});
  if(key.m&2)chips.push({t:'Shift',mod:true});
  if(key.m&4)chips.push({t:'Alt',mod:true});
  chips.push({t:VK[key.v]||('#'+key.v),mod:false});
  chips.forEach((c,idx)=>{
    if(idx>0){const s=document.createElement('span');s.className='kplus';s.textContent='+';kc.appendChild(s);}
    const el=document.createElement('span');
    el.className='kchip'+(c.mod?' mod':'');el.textContent=c.t;kc.appendChild(el);});}

function startRec(i){
  if(recording===i){stopRec();return;}
  stopRec();recording=i;
  const rb=$('rb'+i);rb.classList.add('recording');
  rb.innerHTML="<span class='rec-dot'></span>Нажмите...";
  $('kc'+i).innerHTML='';send('rebind-start');}

function stopRec(){
  if(recording>=0){
    const rb=$('rb'+recording);
    rb.classList.remove('recording');rb.innerHTML='Изменить';
    renderCombo(recording);recording=-1;send('rebind-end');}}

function resetKeys(){keys=DEF_KEYS.map(k=>({...k}));for(let i=0;i<6;i++)renderCombo(i);stopRec();}
function saveKeys(){stopRec();send('rebind:'+keys.map(k=>k.m+','+k.v).join(','));}

function buildYmRows(){
  const cont=$('ymkey-rows');cont.innerHTML='';
  for(let i=0;i<13;i++){
    if(i>0){const d=document.createElement('div');d.className='kr-div';cont.appendChild(d);}
    const row=document.createElement('div');row.className='key-row';row.id='ymkr'+i;
    row.innerHTML=`<div class='kr-lbl'><div class='kr-act'>${YM_ROW_INFO[i].a}</div><div class='kr-sub'>${YM_ROW_INFO[i].s}</div></div>
      <div class='key-combo' id='ymkc${i}'></div>
      <button class='rec-btn' id='ymrb${i}' onclick='startYmRec(${i})'>Изменить</button>`;
    cont.appendChild(row);}
  for(let i=0;i<13;i++)renderYmCombo(i);}

function renderYmCombo(i){
  const kc=$('ymkc'+i),key=ymKeys[i];kc.innerHTML='';
  if(!key.v){kc.innerHTML=`<span class='knone'>по умолчанию (${DEF_YM_DISPLAY[i]})</span>`;return;}
  const chips=[];
  if(key.m&1)chips.push({t:'Ctrl',mod:true});
  if(key.m&2)chips.push({t:'Shift',mod:true});
  if(key.m&4)chips.push({t:'Alt',mod:true});
  chips.push({t:VK[key.v]||('#'+key.v),mod:false});
  chips.forEach((c,idx)=>{
    if(idx>0){const s=document.createElement('span');s.className='kplus';s.textContent='+';kc.appendChild(s);}
    const el=document.createElement('span');
    el.className='kchip'+(c.mod?' mod':'');el.textContent=c.t;kc.appendChild(el);});}

function startYmRec(i){
  if(ymRecording===i){stopYmRec();return;}
  stopYmRec();stopRec();ymRecording=i;
  const rb=$('ymrb'+i);rb.classList.add('recording');
  rb.innerHTML="<span class='rec-dot'></span>Нажмите...";
  $('ymkc'+i).innerHTML='';send('rebind-start');}

function stopYmRec(){
  if(ymRecording>=0){
    const rb=$('ymrb'+ymRecording);
    rb.classList.remove('recording');rb.innerHTML='Изменить';
    renderYmCombo(ymRecording);ymRecording=-1;send('rebind-end');}}

function resetYmKeys(){ymKeys=DEF_YM_DISPLAY.map(()=>({m:0,v:0}));for(let i=0;i<13;i++)renderYmCombo(i);stopYmRec();}
function saveYmKeys(){stopYmRec();send('ymrebind:'+ymKeys.map(k=>k.m+','+k.v).join(','));}

document.addEventListener('keydown',e=>{
  if(ymRecording<0)return;e.preventDefault();
  if(MODS_VK.has(e.keyCode))return;
  const mods=(e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0);
  ymKeys[ymRecording]={m:mods,v:e.keyCode};renderYmCombo(ymRecording);
  const rb=$('ymrb'+ymRecording);rb.classList.remove('recording');
  rb.innerHTML='✓ Назначено';rb.style.color='#5bff8a';rb.style.borderColor='rgba(91,255,138,.4)';
  setTimeout(()=>{rb.style.color='';rb.style.borderColor='';stopYmRec();},900);});
document.addEventListener('keyup',e=>{if(ymRecording>=0&&e.keyCode===27)stopYmRec();});

document.addEventListener('keydown',e=>{
  if(recording<0)return;e.preventDefault();
  if(MODS_VK.has(e.keyCode))return;
  const mods=(e.ctrlKey?1:0)|(e.shiftKey?2:0)|(e.altKey?4:0);
  keys[recording]={m:mods,v:e.keyCode};renderCombo(recording);
  const rb=$('rb'+recording);rb.classList.remove('recording');
  rb.innerHTML='✓ Назначено';rb.style.color='#5bff8a';rb.style.borderColor='rgba(91,255,138,.4)';
  setTimeout(()=>{rb.style.color='';rb.style.borderColor='';stopRec();},900);});
document.addEventListener('keyup',e=>{if(recording>=0&&e.keyCode===27)stopRec();});

// About / updater
function checkUpdate(){
  $('upd-btn').disabled=true;
  $('upd-dot').className='sdot';
  $('upd-txt').textContent='Проверка...';
  send('check-update');
}

function showUpdateModal(latest){
  $('upd-modal-sub').textContent='Версия '+latest+' доступна для установки. Приложение скачает обновление, перезапустится и сделает это автоматически.';
  $('upd-modal-go').disabled=false;$('upd-modal-go').textContent='Обновить';
  $('upd-modal-later').style.display='';
  $('upd-scrim').classList.add('show');
}
function dismissUpdate(){
  $('upd-scrim').classList.remove('show');
  $('upd-txt').textContent='Найдено обновление';$('upd-btn').disabled=false;
}
function confirmUpdate(){
  $('upd-modal-go').disabled=true;$('upd-modal-go').textContent='Установка...';
  $('upd-modal-later').style.display='none';
  $('upd-modal-sub').textContent='Скачивание и установка обновления, приложение перезапустится автоматически…';
  send('confirm-update');
}

// Messages
window.chrome.webview.addEventListener('message',e=>{
  const d=JSON.parse(e.data);
  if(d.type==='state'){updateState(d);if(d.ver)$('about-ver').textContent=d.ver;return;}
  if(d.type==='init-keys'){keys=d.keys.map(k=>({m:k.m,v:k.v}));for(let i=0;i<6;i++)renderCombo(i);return;}
  if(d.type==='init-ymkeys'){ymKeys=d.keys.map(k=>({m:k.m,v:k.v}));for(let i=0;i<13;i++)renderYmCombo(i);return;}
  if(d.type==='init-tweaks'){
    tweaksMask=d.mask;customName=d.customName||'';renderTweaks();
    customCss=d.customCss||'';
    const cc=$('cc-input');if(cc&&document.activeElement!==cc)cc.value=customCss;
    return;}
  if(d.type==='update-status'){
    if(d.current)$('about-ver').textContent=d.current;
    if(d.state==='checking'){$('upd-txt').textContent='Проверка...';}
    else if(d.state==='latest'){$('upd-dot').className='sdot ok';$('upd-txt').textContent='У вас последняя версия';$('upd-btn').disabled=false;}
    else if(d.state==='found'){$('upd-dot').className='sdot ok';$('upd-txt').textContent='Найдено обновление: '+d.latest;$('upd-btn').disabled=false;showUpdateModal(d.latest);}
    else if(d.state==='installing'){$('upd-txt').textContent='Скачивание версии '+d.latest+'…';}
    else if(d.state==='error'){
      $('upd-dot').className='sdot err';$('upd-txt').textContent='Ошибка проверки обновлений';$('upd-btn').disabled=false;
      if($('upd-scrim').classList.contains('show')){
        $('upd-modal-sub').textContent='Не удалось установить обновление. Попробуйте ещё раз позже.';
        $('upd-modal-go').disabled=false;$('upd-modal-go').textContent='Повторить';
        $('upd-modal-later').style.display='';
      }
    }
    return;}
});

buildRows();
buildYmRows();
renderTweaks();
</script>
</body></html>)HUB";

// Shared by the hub WebView2's WebMessageReceived and by the IPC reqText
// relay (see OnInjectTick) so the in-page "cheat menu" overlay — which
// has no WebView2 of its own — can reach the exact same, already-tested
// handling as the standalone hub window, instead of a second copy.
static void HandleUiMessage(const std::wstring& msg){
    if(msg==L"prev")          PostMessageW(g_hwnd,WM_APP+10,0,0);
    else if(msg==L"next")     PostMessageW(g_hwnd,WM_APP+11,0,0);
    else if(msg==L"toggle")   PostMessageW(g_hwnd,WM_APP+12,0,0);
    else if(msg==L"like")     PostMessageW(g_hwnd,WM_APP+13,0,0);
    else if(msg==L"dislike")  PostMessageW(g_hwnd,WM_APP+14,0,0);
    else if(msg==L"overlay-toggle") PostMessageW(g_hwnd,WM_APP+20,0,0);
    else if(msg==L"inject")   PostMessageW(g_hwnd,WM_APP+32,0,0);
    else if(msg==L"close")    PostMessageW(g_hub,WM_CLOSE,0,0);
    else if(msg==L"check-update") std::thread([](){CheckForUpdate(true);}).detach();
    else if(msg==L"confirm-update") std::thread([](){InstallPendingUpdate();}).detach();
    else if(msg==L"drpc-toggle"){SaveDiscordSetting(!g_discordEnabled);BroadcastState();}
    else if(msg==L"cheatmenu-toggle"){SaveCheatMenuSetting(!g_cheatMenuEnabled);BroadcastState();}
    else if(msg==L"rebind-start"){g_rebinding=true; if(g_ipc)g_ipc->rebinding=TRUE;}
    else if(msg==L"rebind-end")  {g_rebinding=false;if(g_ipc)g_ipc->rebinding=FALSE;}
    else if(msg.rfind(L"pos:",0)==0){
        int n=_wtoi(msg.c_str()+4);
        if(n>=0&&n<=5)PostMessageW(g_hwnd,WM_APP+31,(WPARAM)n,0);}
    else if(msg.rfind(L"rebind:",0)==0){
        unsigned m0,v0,m1,v1,m2,v2,m3,v3,m4,v4,m5,v5;
        if(swscanf_s(msg.c_str()+7,L"%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u",
            &m0,&v0,&m1,&v1,&m2,&v2,&m3,&v3,&m4,&v4,&m5,&v5)==12){
            g_keys[0]={m0,v0};g_keys[1]={m1,v1};
            g_keys[2]={m2,v2};g_keys[3]={m3,v3};
            g_keys[4]={m4,v4};g_keys[5]={m5,v5};
            SaveKeys();SendHubKeys();}}
    else if(msg.rfind(L"ymrebind:",0)==0){
        std::vector<DWORD> nums;
        wchar_t* ctx=nullptr;
        std::wstring rest=msg.substr(9);
        wchar_t* tok=wcstok_s(rest.data(),L",",&ctx);
        while(tok){nums.push_back((DWORD)_wtoi(tok));tok=wcstok_s(nullptr,L",",&ctx);}
        if(nums.size()==26){
            for(int i=0;i<13;i++)g_ymKeys[i]={nums[i*2],nums[i*2+1]};
            SaveYmKeys();SendHubYmKeys();}}
    else if(msg.rfind(L"toggle-tweak:",0)==0){
        int idx=_wtoi(msg.c_str()+13);
        if(idx>=0&&idx<9){
            g_tweaksMask^=(1u<<idx);
            SaveTweaks();SendHubTweaks();}}
    else if(msg.rfind(L"set-custom-name:",0)==0){
        SaveCustomName(msg.substr(16));}
    else if(msg.rfind(L"set-custom-css:",0)==0){
        SaveCustomCss(msg.substr(15));}}

// ── WebView2 setup ────────────────────────────────────────
static void SetupController(ICoreWebView2Controller* ctrl, ICoreWebView2* wv, bool isHub){
    ComPtr<ICoreWebView2Controller2> c2;
    if(SUCCEEDED(ctrl->QueryInterface(IID_PPV_ARGS(&c2))))
        c2->put_DefaultBackgroundColor({0,0,0,0});
    ComPtr<ICoreWebView2Settings> stt;wv->get_Settings(&stt);
    if(stt){stt->put_AreDefaultContextMenusEnabled(FALSE);
            stt->put_IsStatusBarEnabled(FALSE);
            stt->put_AreDevToolsEnabled(FALSE);}
    (void)isHub;}

static void InitWebView(){
    // Default WebView2 behavior creates a "<exe>.WebView2" folder next to
    // the executable — keep that out of wherever the user put YMHub.exe
    // (e.g. Desktop) by pointing it at %TEMP% instead.
    wchar_t tmp[MAX_PATH];GetTempPathW(MAX_PATH,tmp);
    std::wstring userDataFolder=std::wstring(tmp)+L"YMHub.WebView2";
    CreateCoreWebView2EnvironmentWithOptions(nullptr,userDataFolder.c_str(),nullptr,
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
        [](HRESULT,ICoreWebView2Environment* env)->HRESULT{
            if(!env)return S_OK;
            g_env=env;

            // ── Overlay controller ──────────────────────────
            env->CreateCoreWebView2Controller(g_hwnd,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT,ICoreWebView2Controller* ctrl)->HRESULT{
                    if(!ctrl)return S_OK;
                    g_ctrl=ctrl;ctrl->get_CoreWebView2(&g_wv);
                    SetupController(ctrl,g_wv.Get(),false);
                    g_wv->add_WebMessageReceived(
                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* a)->HRESULT{
                            LPWSTR s=nullptr;a->TryGetWebMessageAsString(&s);
                            if(s){
                                if(wcscmp(s,L"prev")==0)         PostMessageW(g_hwnd,WM_APP+10,0,0);
                                else if(wcscmp(s,L"next")==0)    PostMessageW(g_hwnd,WM_APP+11,0,0);
                                else if(wcscmp(s,L"toggle")==0)  PostMessageW(g_hwnd,WM_APP+12,0,0);
                                else if(wcscmp(s,L"like")==0)    PostMessageW(g_hwnd,WM_APP+13,0,0);
                                else if(wcscmp(s,L"dislike")==0) PostMessageW(g_hwnd,WM_APP+14,0,0);
                                else if(wcscmp(s,L"hidden")==0)  PostMessageW(g_hwnd,WM_APP+21,0,0);
                                CoTaskMemFree(s);}
                            return S_OK;}).Get(),nullptr);
                    RECT r={0,0,CW,CH};ctrl->put_Bounds(r);
                    g_wv->add_NavigationCompleted(
                        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*)->HRESULT{
                            PostMessageW(g_hwnd,WM_APP+25,0,0);return S_OK;}).Get(),nullptr);
                    g_wv->NavigateToString(HTML);
                    return S_OK;}).Get());

            // ── Hub controller ──────────────────────────────
            env->CreateCoreWebView2Controller(g_hub,
                Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                [](HRESULT,ICoreWebView2Controller* ctrl)->HRESULT{
                    if(!ctrl)return S_OK;
                    g_hubCtrl=ctrl;ctrl->get_CoreWebView2(&g_hubWv);
                    SetupController(ctrl,g_hubWv.Get(),true);
                    g_hubWv->add_WebMessageReceived(
                        Microsoft::WRL::Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                        [](ICoreWebView2*,ICoreWebView2WebMessageReceivedEventArgs* a)->HRESULT{
                            LPWSTR s=nullptr;a->TryGetWebMessageAsString(&s);
                            if(s){
                                std::wstring msg(s);CoTaskMemFree(s);
                                HandleUiMessage(msg);}
                            return S_OK;}).Get(),nullptr);
                    RECT r;GetClientRect(g_hub,&r);ctrl->put_Bounds(r);
                    g_hubWv->add_NavigationCompleted(
                        Microsoft::WRL::Callback<ICoreWebView2NavigationCompletedEventHandler>(
                        [](ICoreWebView2*,ICoreWebView2NavigationCompletedEventArgs*)->HRESULT{
                            // Send initial state once hub is ready
                            PostMessageW(g_hwnd,WM_APP+26,0,0);
                            return S_OK;}).Get(),nullptr);
                    g_hubWv->NavigateToString(HTML_HUB);
                    return S_OK;}).Get());

            return S_OK;}).Get());}

// ── Tray ─────────────────────────────────────────────────
static void ShowTrayMenu(){
    HMENU m=CreatePopupMenu();
    AppendMenuW(m,MF_STRING,IDM_OPENHUB,L"Открыть YMHub");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING|(IsAutostart()?MF_CHECKED:0),IDM_AUTOSTART,L"Автозапуск при входе в Windows");
    AppendMenuW(m,MF_SEPARATOR,0,nullptr);
    AppendMenuW(m,MF_STRING,IDM_EXIT,L"Выход");
    POINT pt;GetCursorPos(&pt);SetForegroundWindow(g_hwnd);
    int cmd=TrackPopupMenu(m,TPM_RETURNCMD|TPM_RIGHTBUTTON,pt.x,pt.y,0,g_hwnd,nullptr);
    DestroyMenu(m);
    if(cmd==IDM_EXIT){DestroyWindow(g_hwnd);return;}
    if(cmd==IDM_OPENHUB){OpenHub();return;}
    if(cmd==IDM_AUTOSTART)SetAutostart(!IsAutostart());}

// ── Keyboard hook ─────────────────────────────────────────
static LRESULT CALLBACK LLKeyProc(int code,WPARAM wp,LPARAM lp){
    if(code==HC_ACTION&&(wp==WM_KEYDOWN||wp==WM_SYSKEYDOWN)&&!g_rebinding){
        auto* k=(KBDLLHOOKSTRUCT*)lp;
        bool ctrl=(GetAsyncKeyState(VK_CONTROL)&0x8000)!=0;
        bool sh  =(GetAsyncKeyState(VK_SHIFT)  &0x8000)!=0;
        bool alt =(GetAsyncKeyState(VK_MENU)   &0x8000)!=0;
        DWORD mods=(ctrl?1u:0u)|(sh?2u:0u)|(alt?4u:0u);
        if(!IsDllLoaded()){
            static const UINT wm[]={WM_APP+20,WM_APP+10,WM_APP+11,WM_APP+12,WM_APP+13,WM_APP+14};
            for(int i=0;i<6;i++)
                if(g_keys[i].vk&&mods==g_keys[i].mods&&k->vkCode==g_keys[i].vk)
                    {PostMessageW(g_hwnd,wm[i],0,0);return 1;}}
        // YM's own native hotkeys (Горячие клавиши) — only act while YM's
        // own window is foreground. Detection/suppression happens here
        // (real, non-injected input), but the actual translated keypress
        // has to be emitted by the DLL via CDP — see SendYmDefaultKey.
        {
            static HWND s_ymWin=nullptr;static ULONGLONG s_ymTick=0;
            ULONGLONG now=GetTickCount64();
            if(now-s_ymTick>500){s_ymWin=FindYM();s_ymTick=now;}
            if(s_ymWin&&GetForegroundWindow()==s_ymWin){
                for(int i=0;i<13;i++)
                    if(g_ymKeys[i].vk&&mods==g_ymKeys[i].mods&&k->vkCode==g_ymKeys[i].vk)
                        {SendYmDefaultKey(i);return 1;}
                if(mods==0)
                    for(int i=0;i<13;i++)
                        if(k->vkCode==YM_DEFAULT_VK[i]&&g_ymKeys[i].vk&&g_ymKeys[i].vk!=YM_DEFAULT_VK[i])
                            return 1;}}}
    return CallNextHookEx(g_hook,code,wp,lp);}

// ── Timers ────────────────────────────────────────────────
static void CALLBACK OnTrackTick(HWND,UINT,UINT_PTR,DWORD){
    ParseYM();BroadcastState();}
// WinHTTP's synchronous calls inside EnsureDebugPort have been observed
// to block far longer than their configured timeouts (same class of bug
// as the SMTC freeze) — run off the UI thread. EnsureDebugPort itself
// bails out without setting g_debugPortFixDone if YM isn't running yet,
// so this must retry every tick (not just once) — otherwise the port
// fix only happens if YM happened to already be up on the very first
// tick, and silently never again for the rest of the session.
static volatile bool g_debugPortCheckRunning=false;
static LONG g_lastReqSeq=0;
static void CALLBACK OnInjectTick(HWND,UINT,UINT_PTR,DWORD){
    if(!g_debugPortFixDone&&!g_debugPortCheckRunning){
        g_debugPortCheckRunning=true;
        std::thread([](){EnsureDebugPort();g_debugPortCheckRunning=false;}).detach();}
    bool wasMissing=!IsDllLoaded();
    TryInject();
    if(wasMissing&&IsDllLoaded()){PushKeysToIPC();PushTweaksToIPC();}
    // In-page cheat-menu relay (see reqSeq/reqText in ipc.h) — only the
    // handful of host-owned things (tweak/CSS persistence) come through
    // here; the overlay's own playback buttons never touch this path.
    if(g_ipc&&g_ipc->reqSeq!=g_lastReqSeq){
        g_lastReqSeq=g_ipc->reqSeq;
        HandleUiMessage(g_ipc->reqText);}
    BroadcastState();}

// ── Hub WndProc ───────────────────────────────────────────
static LRESULT CALLBACK HubWndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hw,&ps);EndPaint(hw,&ps);return 0;}
    case WM_SIZE:
        if(g_hubCtrl){RECT r;GetClientRect(hw,&r);g_hubCtrl->put_Bounds(r);}
        return 0;
    case WM_CLOSE:ShowWindow(hw,SW_HIDE);return 0;
    }
    return DefWindowProcW(hw,msg,wp,lp);}

// ── Overlay WndProc ───────────────────────────────────────
static LRESULT CALLBACK WndProc(HWND hw,UINT msg,WPARAM wp,LPARAM lp){
    switch(msg){
    case WM_ERASEBKGND:return 1;
    case WM_PAINT:{PAINTSTRUCT ps;BeginPaint(hw,&ps);EndPaint(hw,&ps);return 0;}
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;
    case WM_TRAY:
        if(LOWORD(lp)==WM_RBUTTONUP)  ShowTrayMenu();
        if(LOWORD(lp)==WM_LBUTTONDBLCLK) OpenHub();
        return 0;
    case WM_APP+10: DoPrev();    return 0;
    case WM_APP+11: DoNext();    return 0;
    case WM_APP+12: DoToggle();  return 0;
    case WM_APP+13: DoLike();    return 0;
    case WM_APP+14: DoDislike(); return 0;
    case WM_APP+20: ShowOverlay();return 0;
    case WM_APP+27: BroadcastState();return 0; // ParseYM worker finished
    case WM_APP+21:
        if(g_ctrl)g_ctrl->put_IsVisible(FALSE);
        ShowWindow(hw,SW_HIDE);
        g_visible=false;
        BroadcastState();
        return 0;
    case WM_APP+25: ParseYM();BroadcastState();return 0;
    case WM_APP+26: // hub nav complete → send initial state + keys
        ParseYM();BroadcastState();SendHubKeys();SendHubYmKeys();SendHubTweaks();return 0;
    case WM_APP+31: // pos change from hub
        {int n=(int)wp;if(n>=0&&n<=5){
            g_pos=(Pos)n;
            RegSetDW(HKEY_CURRENT_USER,REG_APP,L"Pos",(DWORD)g_pos);
            if(g_visible){int x,y;GetCardPos(x,y);
                SetWindowPos(g_hwnd,HWND_TOPMOST,x,y,CW,CH,SWP_NOACTIVATE);}
            BroadcastState();}}
        return 0;
    case WM_APP+32: // inject from hub
        TryInject();Sleep(500);BroadcastState();return 0;
    case WM_APP+33: // updater status from background thread → UI thread
        BroadcastUpdateStatus(g_updState.c_str(),g_updLatest);return 0;
    case WM_APP+3:
        EnterCriticalSection(&g_artCS);
        if(g_artReady.load()){g_artB64=g_artPending;g_artPending.clear();g_artReady=false;}
        LeaveCriticalSection(&g_artCS);
        BroadcastState();return 0;
    case WM_DESTROY:{
        NOTIFYICONDATAW n={sizeof(n)};n.hWnd=hw;n.uID=1;
        Shell_NotifyIconW(NIM_DELETE,&n);
        KillTimer(hw,TIMER_TRACK);KillTimer(hw,TIMER_INJECT);
        PostQuitMessage(0);return 0;}}
    return DefWindowProcW(hw,msg,wp,lp);}

// ── Entry ─────────────────────────────────────────────────
int WINAPI wWinMain(HINSTANCE hInst,HINSTANCE,LPWSTR,int){
    g_hInst=hInst;
    g_instanceMutex=CreateMutexW(nullptr,TRUE,L"YMHub_Host_v1");
    if(GetLastError()==ERROR_ALREADY_EXISTS){CloseHandle(g_instanceMutex);return 0;}

    InitializeCriticalSection(&g_artCS);
    winrt::init_apartment(winrt::apartment_type::single_threaded);

    // Clean up a previous update's renamed-out old exe, now that we're a
    // fresh process and the old file is no longer in use.
    {wchar_t p[MAX_PATH];GetModuleFileNameW(nullptr,p,MAX_PATH);
    DeleteFileW((std::wstring(p)+L".old").c_str());}

    std::thread([](){
        Sleep(4000); // let the app finish starting up first
        for(;;){
            CheckForUpdate();
            Sleep(6*60*60*1000); // re-check every 6 hours
        }
    }).detach();

    LoadKeys();
    LoadYmKeys();
    LoadTweaks();
    LoadDiscordSetting();
    std::thread(DiscordThreadFn).detach();
    InitIPC();
    g_pos=(Pos)RegGetDW(HKEY_CURRENT_USER,REG_APP,L"Pos",(DWORD)Pos::BC);
    if((int)g_pos<0||(int)g_pos>5)g_pos=Pos::BC;
    g_scrW=GetSystemMetrics(SM_CXSCREEN);
    g_scrH=GetSystemMetrics(SM_CYSCREEN);

    // ── Overlay window ──────────────────────────────────────
    {WNDCLASSEXW wc={sizeof(wc),0,WndProc,0,0,hInst,
        nullptr,nullptr,(HBRUSH)GetStockObject(NULL_BRUSH),nullptr,L"YMHubOvl",nullptr};
    RegisterClassExW(&wc);}
    g_hwnd=CreateWindowExW(
        WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,
        L"YMHubOvl",L"YMHub Overlay",WS_POPUP,
        0,0,CW,CH,nullptr,nullptr,hInst,nullptr);
    {BOOL dark=TRUE;DwmSetWindowAttribute(g_hwnd,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE cp=DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hwnd,DWMWA_WINDOW_CORNER_PREFERENCE,&cp,sizeof(cp));}

    // ── Hub window ──────────────────────────────────────────
    // WS_CAPTION gives the native titlebar (drag + close + icon) for
    // free — the page used to draw its own redundant copy on top of it
    // (a second "YMHub" title and a second close button); that's been
    // removed from the HTML instead of removing the native caption,
    // since the page's drag region was never actually wired to
    // WM_NCHITTEST/NonClientRegion and so never worked anyway. hIcon/
    // hIconSm are set here so the titlebar's icon slot isn't blank.
    HICON hAppIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    {WNDCLASSEXW wc={sizeof(wc),0,HubWndProc,0,0,hInst,
        hAppIcon,nullptr,(HBRUSH)GetStockObject(NULL_BRUSH),nullptr,L"YMHubMain",hAppIcon};
    RegisterClassExW(&wc);}
    // Compute exact window size so client area = HW x HH
    {RECT wr={0,0,HW,HH};
    AdjustWindowRectEx(&wr,WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,FALSE,0);
    int ww=wr.right-wr.left, wh=wr.bottom-wr.top;
    int wx=(g_scrW-ww)/2, wy=(g_scrH-wh)/2;
    g_hub=CreateWindowExW(
        WS_EX_APPWINDOW,
        L"YMHubMain",L"YMHub",
        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
        wx,wy,ww,wh,nullptr,nullptr,hInst,nullptr);}
    {BOOL dark=TRUE;DwmSetWindowAttribute(g_hub,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    DWM_WINDOW_CORNER_PREFERENCE cp=DWMWCP_ROUND;
    DwmSetWindowAttribute(g_hub,DWMWA_WINDOW_CORNER_PREFERENCE,&cp,sizeof(cp));
    MARGINS mg={1,1,1,1};DwmExtendFrameIntoClientArea(g_hub,&mg);}

    // ── Tray icon ───────────────────────────────────────────
    NOTIFYICONDATAW nid={sizeof(nid)};
    nid.hWnd=g_hwnd;nid.uID=1;
    nid.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;
    nid.uCallbackMessage=WM_TRAY;
    nid.hIcon=LoadIconW(hInst,MAKEINTRESOURCEW(1));
    wcscpy_s(nid.szTip,L"YMHub");
    Shell_NotifyIconW(NIM_ADD,&nid);

    // ── Hook + timers ───────────────────────────────────────
    g_hook=SetWindowsHookExW(WH_KEYBOARD_LL,LLKeyProc,hInst,0);
    SetTimer(g_hwnd,TIMER_TRACK,2000,OnTrackTick);
    SetTimer(g_hwnd,TIMER_INJECT,1500,OnInjectTick);

    // ── Init WebView2 (eager, for both windows) ─────────────
    g_wvInited=true;
    InitWebView();

    // ── Show hub on startup ─────────────────────────────────
    ShowWindow(g_hub,SW_SHOW);

    // ── Initial inject attempt ──────────────────────────────
    TryInject();
    PushKeysToIPC(); // write hotkey config + hostHwnd to IPC for DLL
    PushTweaksToIPC();

    MSG msg;
    while(GetMessageW(&msg,nullptr,0,0)){TranslateMessage(&msg);DispatchMessageW(&msg);}

    if(g_wv)g_wv.Reset();
    if(g_ctrl)g_ctrl.Reset();
    if(g_hubWv)g_hubWv.Reset();
    if(g_hubCtrl)g_hubCtrl.Reset();
    if(g_env)g_env.Reset();
    if(g_ipc){UnmapViewOfFile(g_ipc);g_ipc=nullptr;}
    if(g_hMem){CloseHandle(g_hMem);g_hMem=nullptr;}
    UnhookWindowsHookEx(g_hook);
    DeleteCriticalSection(&g_artCS);
    if(g_instanceMutex)CloseHandle(g_instanceMutex);
    return 0;
}
