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

local function build_menu()
    local wnd = gui.GetMainWindow()
    local tab = wnd:AddTab('ymhub_tab', draw.textures['gui_icon_down'], 'YMHub', gui.TabLayoutMode.DEFAULT)
    local group = gui.Group('ymhub_group', 'Yandex Music', 160, gui.GroupWidthMode.FULL)
    tab:Add(group)

    group:Add(make_action_row('ymhub_playpause', 'Play / Pause', 'toggle'))
    group:Add(make_action_row('ymhub_prev', 'Prev', 'prev'))
    group:Add(make_action_row('ymhub_next', 'Next', 'next'))
    group:Add(make_action_row('ymhub_like', 'Like', 'like'))
    group:Add(make_action_row('ymhub_dislike', 'Dislike', 'dislike'))
    group:Add(make_action_row('ymhub_shuffle', 'Shuffle', 'shuffle'))
    group:Add(make_action_row('ymhub_repeat', 'Repeat', 'repeat'))
end

local function draw_hud()
    if not status.ok then return end
    local d = draw.surface
    d.font = draw.fonts['gui_main']

    local line1 = (status.artist or '') .. '  —  ' .. (status.title or '')
    d:AddText(draw.Vec2(20, 20), line1, draw.Color(255, 255, 255))

    local state = status.playing and 'playing' or 'paused'
    if status.liked then state = state .. '  [liked]' end
    if status.shuffle then state = state .. '  [shuffle]' end
    if status.repeatOn then state = state .. '  [repeat]' end
    d:AddText(draw.Vec2(20, 40), state, draw.Color(180, 180, 180))

    if status.seekable then
        local time_line = (status.posText or '0:00') .. ' / ' .. (status.durText or '0:00')
        d:AddText(draw.Vec2(20, 60), time_line, draw.Color(140, 140, 140))
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
