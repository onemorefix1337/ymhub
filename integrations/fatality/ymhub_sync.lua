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
local CARD_W = 320
local PAD = 20

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
-- Accent matches YMHub's own overlay palette (--ac in the in-page cheat
-- menu's CSS, dllmain.cpp) rather than an arbitrary blue, so this reads as
-- the same product instead of a generic HUD.
local C_ACCENT     = { 91, 143, 255 }
local C_ACCENT_DIM = { 60, 75, 100 }
local C_LIKED      = { 230, 90, 130 }
local C_TEXT_MAIN  = { 255, 255, 255 }
local C_TEXT_DIM   = { 165, 165, 175 }
local C_TEXT_FAINT = { 120, 120, 130 }
local C_CARD_BG    = { 13, 13, 20 }
local C_BAR_BG     = { 50, 50, 58 }
local CARD_ALPHA   = 175 -- was 215 — more glass, less solid block

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

-- Small outlined chips instead of bare dots — a thin AddRect border
-- around each one gives the same crisp, defined edges as the card's own
-- border rather than a soft blob, and the wider gap between them reads
-- less cramped.
local function draw_status_chips(d, x, y, alpha_mul)
    local chip = 8
    local gap = 12
    local specs = {
        { status.liked,    C_LIKED },
        { status.shuffle,  C_ACCENT },
        { status.repeatOn, C_ACCENT },
    }
    for _, s in ipairs(specs) do
        local on, c = s[1], s[2]
        local col = on and c or C_ACCENT_DIM
        d:AddRectFilled(draw.Rect(x, y, x + chip, y + chip), rgba(col, on and 255 or 60, alpha_mul))
        d:AddRect(draw.Rect(x, y, x + chip, y + chip), rgba(col, 255, alpha_mul), 1)
        x = x + chip + gap
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
    local card_h = show_time and 132 or 106
    local x, y = HUD_X, HUD_Y + slide
    local pad = PAD

    d:AddRectFilled(draw.Rect(x, y, x + CARD_W, y + card_h), rgba(C_CARD_BG, CARD_ALPHA, e))
    -- Thin 1px border around the whole card — crisp defined edge instead
    -- of a soft/flat block, same visual language as the outlined status
    -- chips below. No rounded corners (still avoiding
    -- AddRectFilledRounded after the crash), but a clean hairline outline
    -- reads considerably less "flat debug box" on its own.
    d:AddRect(draw.Rect(x, y, x + CARD_W, y + card_h), rgba(C_ACCENT_DIM, 255, e), 1)

    -- Accent bar: steady when paused, gently pulsing brightness while
    -- playing — cheap "alive" feel without needing a real glow primitive.
    local bar_col = C_ACCENT_DIM
    if status.playing then
        local pulse = 0.6 + 0.4 * triangle_wave(now, 1.6)
        bar_col = { C_ACCENT[1] * pulse, C_ACCENT[2] * pulse, C_ACCENT[3] * pulse }
    end
    d:AddRectFilled(draw.Rect(x, y, x + 3, y + card_h), rgba(bar_col, 255, e))

    -- Long titles/artists (collab tracks especially) were rendering well
    -- past the card's own edge with no truncation at all — confirmed live.
    -- Can't slice the string itself to truncate it (that needs the
    -- standard string library, never confirmed to exist here either — same
    -- category of risk as pcall/math), so instead the drawing itself is
    -- clipped to the card's inner width: the full string is still handed
    -- to AddText, it just can't paint outside this rect. Reset back to an
    -- unbounded clip right after, so this doesn't affect the chips/progress
    -- bar below or anything another script draws later this same frame.
    local text_clip = draw.Rect(x + pad, y, x + CARD_W - pad, y + card_h)
    local screen = draw.GetDisplay()
    local full_clip = draw.Rect(0, 0, screen.x, screen.y)

    d:OverrideClipRect(text_clip)

    -- Uppercase section-label header with a divider underneath it — the
    -- "card with a labeled header" language the redesign asked for,
    -- rather than the label just floating above the title with nothing
    -- separating them.
    d.font = draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 14), 'ЯНДЕКС МУЗЫКА', rgba(C_ACCENT, 255, e))
    d:AddLine(draw.Vec2(x + pad, y + 32), draw.Vec2(x + CARD_W - pad, y + 32), rgba(C_ACCENT_DIM, 255, e))

    d.font = draw.fonts['gui_bold'] or draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 42), status.title or '', rgba(C_TEXT_MAIN, 255, e))

    d.font = draw.fonts['gui_main']
    d:AddText(draw.Vec2(x + pad, y + 62), status.artist or '', rgba(C_TEXT_DIM, 255, e))
    d:OverrideClipRect(full_clip)

    draw_status_chips(d, x + pad, y + 86, e)

    if show_time then
        local bar_y = y + card_h - 22
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
