-- YMHub sync for Fatality — in-game HUD + playback control for Yandex
-- Music, talking to YMHub's loopback bridge (see HttpBridgeThreadFn in
-- src/dll/dllmain.cpp) at 127.0.0.1:47990.
--
-- Setup:
--   1. YMHub must be running (it injects into Yandex Music and starts the
--      bridge automatically — nothing to configure on that side).
--   2. This script needs "Allow insecure" enabled when you load it, since
--      Fatality only exposes http.* under insecure mode.
--
-- Protocol: GET /status returns the JSON below; POST /cmd takes a raw
-- action string body (no JSON envelope) — the same vocabulary the in-page
-- cheat menu itself uses: like, dislike, prev, next, toggle, shuffle,
-- repeat, seek:<n>, vol:<0..1>. Anything else is rejected by the bridge.

local BRIDGE = 'http://127.0.0.1:47990'
local POLL_INTERVAL = 1 -- seconds; the bridge itself already throttles at ~700ms internally

local status = { ok = false }
local bridge_warned = false

local function poll()
    http.Get(BRIDGE .. '/status', {}, function(code, data)
        if code == 200 then
            local decoded = utils.JsonDecode(data)
            if decoded then status = decoded end
            bridge_warned = false
        else
            status = { ok = false }
            if not bridge_warned then
                print('[YMHub] bridge unreachable at ' .. BRIDGE .. ' — is YMHub running?')
                bridge_warned = true
            end
        end
    end)
    Delay(POLL_INTERVAL, poll)
end

local function send_action(action)
    http.Post(BRIDGE .. '/cmd', { data = action }, function(code, data)
        if code ~= 200 then
            print('[YMHub] action "' .. action .. '" failed (' .. tostring(code) .. '): ' .. tostring(data))
        end
    end)
end

-- gui.MakeControlEasy only has stateful control types (checkbox/slider/...),
-- no plain momentary "button" — each action here is a checkbox that fires
-- on the ON edge and immediately resets itself. That also means it inherits
-- Fatality's normal per-control hotkey binding, same as any other checkbox.
--
-- No pcall here — confirmed live that this host's Lua doesn't expose it as
-- a global at all (only print/error/unpack/Delay/__shutdown are documented
-- globals, and calling pcall threw "attempt to call global 'pcall' (a nil
-- value)"). Fatality's own console already prints a full traceback for any
-- uncaught error anyway (seen live for that exact pcall mistake), so a
-- bare call gets the same diagnostic for free without needing pcall at all.
local function make_action_row(id, label, action)
    local ctrl, row = gui.MakeControlEasy(id, label, 'checkbox')
    ctrl:AddCallback(function()
        if ctrl:GetValue():Get() then
            send_action(action)
            ctrl:GetValue():Set(false)
            ctrl:Reset() -- required after a direct ValueParam:Set() for the widget to actually reflect it
        end
    end)
    return row
end

local ACTIONS = {
    { 'ymhub_playpause', 'Play / Pause', 'toggle' },
    { 'ymhub_prev',      'Prev',         'prev' },
    { 'ymhub_next',      'Next',         'next' },
    { 'ymhub_like',      'Like',         'like' },
    { 'ymhub_dislike',   'Dislike',      'dislike' },
    { 'ymhub_shuffle',   'Shuffle',      'shuffle' },
    { 'ymhub_repeat',    'Repeat',       'repeat' },
}

local function build_menu()
    local wnd = gui.GetMainWindow()
    local tab = wnd:AddTab('ymhub_tab', draw.textures['gui_icon_down'], 'YMHub', gui.TabLayoutMode.DEFAULT)
    local group = gui.Group('ymhub_group', 'Yandex Music', 160, gui.GroupWidthMode.FULL)
    tab:Add(group)

    for _, a in ipairs(ACTIONS) do
        local row = make_action_row(a[1], a[2], a[3])
        if row then group:Add(row) end
    end
end

-- Top-left at y=20 sat right on top of CS2's own top HUD row (visible
-- live — it overlapped the team/round chrome). Pushed down to clear that;
-- still just a fixed guess since there's no way to test against a real
-- match HUD from here — nudge HUD_X/HUD_Y below if it still collides with
-- something on your actual layout.
local HUD_X, HUD_Y = 20, 120
local CARD_W = 250

-- Deliberately staying off AddRectFilledRounded/AddGlow/AddWithBlur here —
-- rounded corners would read nicer, but that was the one genuinely new,
-- unconfirmed native call in the version that preceded a real game crash,
-- and there's no way to test a replacement before it ships. Everything
-- below is built from AddRectFilled/AddLine/AddText, all already proven
-- crash-free across many reloads — animation comes from re-computing
-- plain numbers every frame, not from any new native primitive.
--
-- rgba() returns a fresh Color each call so alpha can be animated; base
-- colors are kept as plain {r,g,b} tables rather than pre-built Color
-- objects for exactly that reason.
local C_ACCENT     = { 120, 190, 255 }
local C_ACCENT_DIM = { 70, 90, 110 }
local C_LIKED      = { 230, 90, 130 }
local C_TEXT_MAIN  = { 255, 255, 255 }
local C_TEXT_DIM   = { 165, 165, 175 }
local C_TEXT_FAINT = { 120, 120, 130 }
local C_CARD_BG    = { 14, 14, 18 }
local C_BAR_BG     = { 50, 50, 58 }

local function rgba(c, a, mul)
    mul = mul or 1
    return draw.Color(c[1], c[2], c[3], (a or 255) * mul)
end

-- Track-change fade/slide, plus a slow triangle-wave pulse on the accent
-- bar while playing. draw.GetTime() is the one timing primitive confirmed
-- to exist (a float render clock) — no math.sin available (math.* was
-- never confirmed either, same category as the pcall mistake), so the
-- pulse is a hand-rolled triangle wave via % instead of a sine curve:
-- rougher shape, same "breathing" effect, needs nothing but the modulo
-- operator.
local FADE_TIME = 0.25
local last_title = nil
local anim_start = 0
local display_frac = 0

local function ease_out(t)
    return 1 - (1 - t) * (1 - t)
end

local function triangle_wave(t, period)
    local phase = (t % period) / period -- 0..1 sawtooth
    if phase > 0.5 then phase = 1 - phase end
    return phase * 2 -- 0..1..0 triangle
end

-- One filled square per active flag instead of bracketed debug text
-- ("[liked]") — a row of small colored dots reads at a glance instead of
-- having to read words. Liked = pink, shuffle/repeat = accent blue when
-- on, dim gray when off (dot is always drawn so the row doesn't jump
-- around as state changes).
local function draw_status_dots(d, x, y, alpha_mul)
    local dot = 6
    local specs = {
        { status.liked,    C_LIKED },
        { status.shuffle,  C_ACCENT },
        { status.repeatOn, C_ACCENT },
    }
    for _, s in ipairs(specs) do
        local on, c = s[1], s[2]
        d:AddRectFilled(draw.Rect(x, y, x + dot, y + dot), rgba(on and c or C_ACCENT_DIM, 255, alpha_mul))
        x = x + dot + 6
    end
end

local function draw_hud()
    if not status.ok then return end
    local d = draw.surface
    local now = draw.GetTime()

    if status.title ~= last_title then
        last_title = status.title
        anim_start = now
    end
    local t = (now - anim_start) / FADE_TIME
    if t < 0 then t = 0 end
    if t > 1 then t = 1 end
    local e = ease_out(t)          -- 0..1 fade-in progress
    local slide = (1 - e) * 14     -- px, card eases up into its resting spot

    local show_time = status.seekable
    local card_h = show_time and 78 or 60
    local x, y = HUD_X, HUD_Y + slide

    d:AddRectFilled(draw.Rect(x, y, x + CARD_W, y + card_h), rgba(C_CARD_BG, 215, e))

    -- Accent bar: steady when paused, gently pulsing brightness while
    -- playing — cheap "alive" feel without needing a real glow primitive.
    local bar_col = C_ACCENT_DIM
    if status.playing then
        local pulse = 0.6 + 0.4 * triangle_wave(now, 1.6)
        bar_col = { C_ACCENT[1] * pulse, C_ACCENT[2] * pulse, C_ACCENT[3] * pulse }
    end
    d:AddRectFilled(draw.Rect(x, y, x + 3, y + card_h), rgba(bar_col, 255, e))

    local pad = 16
    d.font = draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 10), status.artist or '', rgba(C_TEXT_DIM, 255, e))

    d.font = draw.fonts['gui_bold'] or draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 26), status.title or '', rgba(C_TEXT_MAIN, 255, e))

    d.font = draw.fonts['gui_main']
    draw_status_dots(d, x + pad, y + 48, e)

    if show_time then
        local bar_y = y + card_h - 18
        local bar_x1, bar_x2 = x + pad, x + CARD_W - pad
        d:AddRectFilled(draw.Rect(bar_x1, bar_y, bar_x2, bar_y + 3), rgba(C_BAR_BG, 255, e))

        local target = 0
        if status.seekMax and status.seekMax > 0 then
            target = (status.seekPos or 0) / status.seekMax
            if target < 0 then target = 0 end
            if target > 1 then target = 1 end
        end
        -- Eased toward the real position instead of snapping to it every
        -- tick — the bridge only updates ~1x/sec, so a hard snap would
        -- visibly jump; this glides between updates instead.
        display_frac = display_frac + (target - display_frac) * 0.12

        d:AddRectFilled(draw.Rect(bar_x1, bar_y, bar_x1 + (bar_x2 - bar_x1) * display_frac, bar_y + 3), rgba(C_ACCENT, 255, e))
        d:AddText(draw.Vec2(bar_x1, bar_y + 6), status.posText or '0:00', rgba(C_TEXT_FAINT, 255, e))
        local dur = status.durText or '0:00'
        d:AddText(draw.Vec2(bar_x2 - (#dur * 6), bar_y + 6), dur, rgba(C_TEXT_FAINT, 255, e))
    end
end

if ws.TestCapability('http') then
    build_menu()
    events.presentQueue:Add(draw_hud)
    poll()
else
    print('[YMHub] "http" capability not available — enable "Allow insecure" for this script to use it.')
end

function __shutdown()
    print('[YMHub] sync script unloaded')
end
