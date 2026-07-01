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

local function draw_hud()
    if not status.ok then return end
    local d = draw.surface
    d.font = draw.fonts['gui_main']

    local line1 = (status.artist or '') .. '  —  ' .. (status.title or '')
    local state = status.playing and 'playing' or 'paused'
    if status.liked then state = state .. '  [liked]' end
    if status.shuffle then state = state .. '  [shuffle]' end
    if status.repeatOn then state = state .. '  [repeat]' end
    local show_time = status.seekable
    local time_line = show_time and ((status.posText or '0:00') .. ' / ' .. (status.durText or '0:00')) or nil

    -- Plain solid background (not the rounded/alpha variant — its exact
    -- parameter units weren't confirmed in the docs and this isn't worth
    -- guessing on) so the text is readable over whatever's behind it,
    -- rather than floating bare over the game world like before.
    local box_h = show_time and 66 or 46
    d:AddRectFilled(draw.Rect(HUD_X - 10, HUD_Y - 10, HUD_X + 260, HUD_Y - 10 + box_h), draw.Color(10, 10, 14))

    d:AddText(draw.Vec2(HUD_X, HUD_Y), line1, draw.Color(255, 255, 255))
    d:AddText(draw.Vec2(HUD_X, HUD_Y + 20), state, draw.Color(180, 180, 180))
    if show_time then
        d:AddText(draw.Vec2(HUD_X, HUD_Y + 40), time_line, draw.Color(140, 140, 140))
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
