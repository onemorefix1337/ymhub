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

local ACCENT      = draw.Color(120, 190, 255)
local ACCENT_DIM  = draw.Color(70, 90, 110)
local TEXT_MAIN   = draw.Color(255, 255, 255)
local TEXT_DIM    = draw.Color(165, 165, 175)
local TEXT_FAINT  = draw.Color(120, 120, 130)
local CARD_BG     = draw.Color(14, 14, 18, 215)
local BAR_BG      = draw.Color(50, 50, 58)

-- One filled square per active flag instead of bracketed debug text
-- ("[liked]") — a row of small colored dots reads at a glance instead of
-- having to read words. Liked = accent pink, shuffle/repeat = accent blue
-- when on, dim gray when off (dot is always drawn so the row doesn't jump
-- around as state changes).
local function draw_status_dots(d, x, y)
    local dot = 6
    local specs = {
        { status.liked,    draw.Color(230, 90, 130) },
        { status.shuffle,  ACCENT },
        { status.repeatOn, ACCENT },
    }
    for _, s in ipairs(specs) do
        local on, col = s[1], s[2]
        d:AddRectFilled(draw.Rect(x, y, x + dot, y + dot), on and col or ACCENT_DIM)
        x = x + dot + 6
    end
end

local function draw_hud()
    if not status.ok then return end
    local d = draw.surface

    local show_time = status.seekable
    local card_h = show_time and 78 or 60
    local x, y = HUD_X, HUD_Y

    d:AddRectFilledRounded(draw.Rect(x, y, x + CARD_W, y + card_h), CARD_BG, 8)
    -- Thin left accent bar doubles as the playing/paused indicator so the
    -- state doesn't need its own text line.
    d:AddRectFilled(draw.Rect(x, y, x + 3, y + card_h), status.playing and ACCENT or ACCENT_DIM)

    local pad = 16
    d.font = draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 10), status.artist or '', TEXT_DIM)

    d.font = draw.fonts['gui_bold'] or draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 26), status.title or '', TEXT_MAIN)

    d.font = draw.fonts['gui_main']
    draw_status_dots(d, x + pad, y + 48)

    if show_time then
        local bar_y = y + card_h - 18
        local bar_x1, bar_x2 = x + pad, x + CARD_W - pad
        d:AddRectFilled(draw.Rect(bar_x1, bar_y, bar_x2, bar_y + 3), BAR_BG)
        -- Plain clamp, no math.min/max — same lesson as the pcall mistake
        -- above: never confirmed the math library exists here either, and
        -- this is trivial to write without it.
        local frac = 0
        if status.seekMax and status.seekMax > 0 then
            frac = (status.seekPos or 0) / status.seekMax
            if frac < 0 then frac = 0 end
            if frac > 1 then frac = 1 end
        end
        d:AddRectFilled(draw.Rect(bar_x1, bar_y, bar_x1 + (bar_x2 - bar_x1) * frac, bar_y + 3), ACCENT)
        d:AddText(draw.Vec2(bar_x1, bar_y + 6), status.posText or '0:00', TEXT_FAINT)
        local dur = status.durText or '0:00'
        d:AddText(draw.Vec2(bar_x2 - (#dur * 6), bar_y + 6), dur, TEXT_FAINT)
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
