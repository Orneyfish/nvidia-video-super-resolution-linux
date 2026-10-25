-- vsr.lua - turns NVIDIA RTX video super resolution on and off by itself in mpv.
--
-- While a video plays fullscreen, is smaller than the display, SDR, progressive and the machine is on AC power,
-- this script adds the VapourSynth filter @nvvsr (mpv/vsr.vpy + libvsnvvsr.so) to the video filter chain. When
-- one of the conditions stops holding it removes the filter again; on frame drops it steps the quality down.
-- Every failure falls back to the original video. Options: script-opts/vsr.conf or --script-opts=vsr-<key>=<value>
-- (documented in mpv/script-opts/vsr.conf). Keys (rebindable in input.conf, "<key> script-binding vsr/<name>"):
--   Ctrl+s toggle   Ctrl+Shift+s quality   Alt+s stats
-- State for other scripts and tests: the properties user-data/vsr/{state,reason,quality,steps,hwdec,stats}.
--
-- Layout: the first half (module M) is pure logic without any mpv dependency, unit-tested by
-- mpv/tests/test_logic.lua under plain LuaJIT; the second half is the mpv glue and only runs inside mpv.
local M = {}

-- ---------------------------------------------------------------------------------------------------------
-- Pure logic
-- ---------------------------------------------------------------------------------------------------------

M.LABEL, M.FMT_LABEL = "nvvsr", "nvvsr-fmt"
M.LADDER = { "ultra", "high", "medium", "low", "bicubic" } -- step-down order, best first
M.QUALITIES = { auto = true, bicubic = true, low = true, medium = true, high = true, ultra = true,
                streaming_medium = true, streaming_ultra = true }

M.defaults = {
  enabled = true, ac_only = true, min_height = 360, slop = 0.02, quality = "auto", pool = 1, strength = "",
  adaptive = true, adaptive_window = 5, adaptive_drops = 10, settle = 2,
  hwdec = "copy", convert_10bit = true, retry_on_next_file = true, osd = true,
  osd_size = 11, osd_opacity = 0.55, osd_duration = 1.5, osd_position = 9,
  check_interval = 30, confirm_timeout = 8, stats_interval = 1,
  buffered_frames = 4, concurrent_frames = 2, vpy = "", power_dir = "/sys/class/power_supply",
}

local function clamp(v, lo, hi) return math.max(lo, math.min(hi, v)) end

-- Validates the options read by mp.options: returns a sanitised copy and a list of warnings.
function M.normalize(raw)
  local o, warns = {}, {}
  for k, v in pairs(M.defaults) do
    if raw[k] ~= nil then o[k] = raw[k] else o[k] = v end -- (not "raw[k] and ... or v": that loses false)
  end
  local function num(key, lo, hi, int)
    local v = tonumber(o[key])
    if not v or v ~= v then warns[#warns + 1] = key .. ": not a number, using " .. tostring(M.defaults[key]); v = M.defaults[key] end
    if v < lo or v > hi then warns[#warns + 1] = key .. ": " .. tostring(v) .. " out of range"; v = clamp(v, lo, hi) end
    o[key] = int and math.floor(v + 0.5) or v
  end
  num("min_height", 0, 4320, true); num("slop", 0, 1); num("pool", 1, 4, true)
  num("adaptive_window", 1, 120); num("adaptive_drops", 1, 10000, true); num("settle", 0, 60)
  num("check_interval", 1, 3600); num("confirm_timeout", 1, 120); num("stats_interval", 0.2, 10)
  num("buffered_frames", 1, 16, true); num("concurrent_frames", 1, 16, true)
  num("osd_size", 6, 40, true); num("osd_opacity", 0.1, 1); num("osd_duration", 0, 30); num("osd_position", 1, 9, true)
  o.quality = tostring(o.quality):lower():gsub("%s+", "")
  if not M.QUALITIES[o.quality] then warns[#warns + 1] = "quality: unknown '" .. o.quality .. "', using auto"; o.quality = "auto" end
  local s = tostring(o.strength):gsub("%s+", "")
  if s == "" then
    o.strength = nil
  else
    local sv = tonumber(s)
    if not sv or sv < 0 or sv > 1 then warns[#warns + 1] = "strength: must be 0..1, ignored"; o.strength = nil else o.strength = sv end
  end
  o.hwdec = tostring(o.hwdec):lower():gsub("%s+", "")
  if o.hwdec == "" then o.hwdec = "copy" end
  return o, warns
end

-- The status badge: small, semi-transparent text in a screen corner (numpad layout: 9 = top right, 7 = top left,
-- 3 = bottom right, ...). Drawn at a 1280x720 coordinate scale, so `osd_size` 11 is about 2% of the screen height.
function M.badge_ass(text, o)
  local alpha = string.format("%02X", clamp(math.floor((1 - o.osd_opacity) * 255 + 0.5), 0, 255))
  return string.format("{\\an%d\\fs%d\\bord1\\shad0\\1c&HFFFFFF&\\3c&H000000&\\1a&H%s&\\3a&H%s&}%s",
    o.osd_position, o.osd_size, alpha, alpha, text)
end

-- Mirror of nvvsr_auto_quality() in core/include/nvvsr.h (the plugin is the source of truth; this is only used
-- to start the step-down ladder and for the stats). <=540p ULTRA, 541..800 HIGH, >800 MEDIUM; fps >= 50 and
-- height > 1200 -> LOW; fps >= 100 steps one level down.
function M.auto_quality(h, fps)
  local i = h <= 540 and 1 or (h <= 800 and 2 or 3)
  if fps >= 50 and h > 1200 then i = 4 end
  if fps >= 100 and i < 4 then i = i + 1 end
  return M.LADDER[i]
end

local STREAM_POS = { streaming_ultra = "ultra", streaming_medium = "medium" }

-- The level after `q` on the ladder, nil at the bottom (or for an unknown name).
function M.next_quality(q)
  q = STREAM_POS[q] or q
  for i, name in ipairs(M.LADDER) do
    if name == q then return M.LADDER[i + 1] end
  end
end

-- Ctrl+Shift+s: auto -> ultra -> high -> medium -> low -> bicubic -> auto
function M.cycle_quality(q)
  local order = { "auto", "ultra", "high", "medium", "low", "bicubic" }
  for i, name in ipairs(order) do
    if name == q then return order[i % #order + 1] end
  end
  return "auto"
end

-- Frame drops since the oldest sample inside the window; samples are {t=seconds, n=cumulative drops}.
-- Returns drops, span (seconds covered). Samples older than the window are discarded.
function M.window_drops(samples, now, window)
  while #samples > 1 and samples[2].t <= now - window do table.remove(samples, 1) end
  local first, last = samples[1], samples[#samples]
  if not first then return 0, 0 end
  return last.n - first.n, last.t - first.t
end

-- Sliding-window trouble test: at least `drops` drops while the window is (almost) full.
function M.in_trouble(samples, now, window, drops)
  local n, span = M.window_drops(samples, now, window)
  return span >= window * 0.8 and n >= drops, n
end

local function byte_len_prefix(s) return "%" .. #s .. "%" .. s end

-- user-data in the only form mpv's vapoursynth filter parses reliably: "%N%quality=..;pool=..", N = byte length.
function M.user_data(quality, pool, strength)
  local body = "quality=" .. quality .. ";pool=" .. pool
  if strength then body = body .. string.format(";strength=%g", strength) end
  return byte_len_prefix(body), body
end

-- The vf specification of the SR filter. `file` is length-prefixed too, so spaces/colons in the path are safe.
function M.vf_spec(vpy, quality, o)
  return string.format("@%s:vapoursynth=file=%s:buffered-frames=%d:concurrent-frames=%d:user-data=%s", M.LABEL,
    byte_len_prefix(vpy), o.buffered_frames, o.concurrent_frames, (M.user_data(quality, o.pool, o.strength)))
end

M.FMT_SPEC = "@" .. M.FMT_LABEL .. ":format=fmt=yuv420p"

-- Pixel format (mpv name, e.g. "yuv420p10", "nv12", "p010", "vaapi") -> "direct" (8-bit 4:2:0, the filter takes
-- it as is), "convert" (mpv's format filter converts to 8-bit 4:2:0 first) or nil (not video we handle: RGB,
-- gray, alpha, ...). Hardware formats count as convert: the decoder output is downloaded and converted.
local HW_FORMATS = { vaapi = 1, cuda = 1, nvdec = 1, vdpau = 1, drm_prime = 1, vulkan = 1, d3d11 = 1, dxva2_vld = 1,
                     videotoolbox = 1, mediacodec = 1, opencl = 1, d3d12 = 1, vaapi_vld = 1 }
function M.classify_format(pf, hw_pf)
  local f = pf
  if f and HW_FORMATS[f] then
    if not hw_pf then return "convert" end
    f = hw_pf
  end
  if not f then return nil end
  if f == "yuv420p" or f == "yuvj420p" or f == "nv12" or f == "nv21" then return "direct" end
  if f:match("^yuvj?4%d%dp%d*$") or f:match("^nv%d%d$") or f:match("^p%d%d%d$") then return "convert" end
  return nil
end

-- Fit of the displayed picture (dw x dh, aspect corrected) into the display: the SR scale factor
-- (same rule as mpv/vsr.vpy: min of the two ratios, at most 4x) and the output size (even, >= source).
function M.fit(w, h, dw, dh, disp_w, disp_h)
  local scale = math.min(disp_w / dw, disp_h / dh)
  local capped = math.min(scale, 4)
  local function even(x) return math.max(2, 2 * math.floor(x / 2 + 0.5)) end
  return scale, math.max(even(w * capped), w), math.max(even(h * capped), h)
end

-- Is the machine on AC? `supplies` = {{type=, online=}, ...} (from /sys/class/power_supply). true / false /
-- nil (no power information, e.g. a desktop: treated as AC by the caller). Mains and USB(-C PD) chargers count.
function M.on_ac(supplies)
  local battery = false
  for _, p in ipairs(supplies) do
    local t = (p.type or ""):lower()
    if t == "battery" then
      battery = true
    elseif (t == "mains" or t:find("^usb")) and p.online == 1 then
      return true
    end
  end
  if battery then return false end
  return nil
end

local NOT_SDR_GAMMA = { pq = true, hlg = true, ["v-log"] = true, ["s-log1"] = true, ["s-log2"] = true }

-- Decides whether SR should run. `s` is a snapshot of the player state, `o` the normalised options.
-- Returns true, nil, info (info.convert = a format filter is needed) or false, reason.
function M.eligibility(s, o)
  if s.failed then return false, "failed earlier on this file" end
  if s.user_off then return false, "turned off by user" end
  if not (o.enabled or s.user_on) then return false, "disabled in vsr.conf" end
  if not s.video then return false, "no video" end
  if s.image then return false, "image or cover art" end
  if not s.fullscreen then return false, "not fullscreen" end
  if not s.w then return false, "video parameters unknown" end
  if NOT_SDR_GAMMA[s.gamma or ""] or s.colormatrix == "dolbyvision" then return false, "HDR" end
  if s.primaries == "bt.2020" or (s.colormatrix or ""):find("^bt%.2020") then return false, "BT.2020 (wide gamut)" end
  if s.interlaced and not s.deinterlace_active then return false, "interlaced" end
  if (s.rotate or 0) % 360 ~= 0 then return false, "rotated video" end
  local class = M.classify_format(s.pixfmt, s.hw_pixfmt)
  if not class then return false, "pixel format " .. tostring(s.pixfmt) end
  if class == "convert" and not o.convert_10bit then return false, "pixel format " .. tostring(s.hw_pixfmt or s.pixfmt) .. " (convert_10bit=no)" end
  if s.h < o.min_height then return false, string.format("source too small (%dp < %dp)", s.h, o.min_height) end
  if not (s.display_w and s.display_h and s.display_w > 0 and s.display_h > 0) then return false, "display size unknown" end
  local scale = M.fit(s.w, s.h, s.dw or s.w, s.dh or s.h, s.display_w, s.display_h)
  if scale < 1 + o.slop then
    return false, string.format("source (%dx%d) is not smaller than the display (%dx%d)", s.dw or s.w, s.dh or s.h, s.display_w, s.display_h)
  end
  if o.ac_only and s.on_ac == false then return false, "on battery" end
  return true, nil, { convert = class == "convert" }
end

-- `hwdec` option value (list of strings) -> the same with every hardware decoder in its "-copy" variant, or
-- nil when nothing needs to change (software decoding, already copy variants).
function M.copy_hwdec(list)
  if type(list) ~= "table" or #list == 0 then return nil end
  local out, changed, seen = {}, false, {}
  for _, name in ipairs(list) do
    local n = name
    if name == "no" or name:find("%-copy$") then
      n = name
    elseif name:find("^auto") then
      n = "auto-copy"; changed = true
    else
      n = name .. "-copy"; changed = true
    end
    if not seen[n] then seen[n] = true; out[#out + 1] = n end
  end
  return changed and out or nil
end

function M.same_list(a, b)
  return type(a) == "table" and type(b) == "table" and table.concat(a, ",") == table.concat(b, ",")
end

if not mp then return M end -- loaded by the unit tests

-- ---------------------------------------------------------------------------------------------------------
-- mpv glue
-- ---------------------------------------------------------------------------------------------------------
local utils = require "mp.utils"
local options = require "mp.options"
local msg = require "mp.msg"

local raw = {}
for k, v in pairs(M.defaults) do raw[k] = v end
options.read_options(raw, "vsr")
local opt, warns = M.normalize(raw)
for _, w in ipairs(warns) do msg.warn("option " .. w) end

-- Per-file state. state: off | starting (filter added, no scaled frame seen yet) | on | failed
local S = { state = "off", reason = "not started", loaded = false, probed = false, steps = 0, enables = {} }
local session_failed = false -- set when a failure must not be retried on the next file (retry_on_next_file=no)
local eval_timer, tick_timer, probe_timer, overlay, overlay_timer
local restart -- forward declaration (used by evaluate)

-- On-screen feedback is a small transparent badge with one or two words (details go to the log).
-- `osd_duration` 0 keeps it up until the next message; Ctrl+Alt+s switches the badge on and off.
local badge = mp.create_osd_overlay("ass-events")
badge.res_x, badge.res_y = 1280, 720
local badge_timer = mp.add_timeout(1, function() badge:remove() end, true)

local function osd(text)
  if not opt.osd then return end
  badge.data = M.badge_ass(text, opt)
  badge:update()
  badge_timer:kill()
  if opt.osd_duration > 0 then
    badge_timer.timeout = opt.osd_duration
    badge_timer:resume()
  end
end

local function native(name) return mp.get_property_native(name) end

-- command_native returns nil on success too; only the second value tells: true, or false + message.
local function command(...)
  local _, err = mp.command_native({ ... })
  return err == nil, err
end

local function ac_name(v) return v == true and "AC" or (v == false and "battery" or "unknown") end

-- The quality the plugin gets: the user's/auto setting until a step-down made it explicit.
local function requested_quality() return S.step_q or S.q_override or opt.quality end

local function quality_label()
  local q = requested_quality()
  return q == "auto" and ("auto/" .. (S.ladder_q or "?")) or q
end

local function publish()
  mp.set_property_native("user-data/vsr/state", S.state)
  mp.set_property_native("user-data/vsr/reason", S.reason or "")
  mp.set_property_native("user-data/vsr/quality", S.state == "off" and "" or quality_label())
  mp.set_property_native("user-data/vsr/steps", S.steps)
  mp.set_property_native("user-data/vsr/hwdec", S.saved_hwdec and table.concat(S.saved_hwdec, ",") or "")
end

local function set_state(state, reason)
  S.state, S.reason = state, reason or S.reason
  publish()
  if state == "starting" or state == "on" then tick_timer:resume() else tick_timer:kill() end
end

-- Where vsr.vpy is: option, next to this script, or in the mpv config directory.
local function find_vpy()
  local cands = {}
  if opt.vpy ~= "" then cands[#cands + 1] = opt.vpy end
  local dir = mp.get_script_directory() or (debug.getinfo(1, "S").source:match("^@(.*)/[^/]*$"))
  if dir then cands[#cands + 1] = dir .. "/vsr.vpy" end
  for _, rel in ipairs({ "~~/scripts/vsr.vpy", "~~/vsr.vpy" }) do cands[#cands + 1] = mp.command_native({ "expand-path", rel }) end
  for _, c in ipairs(cands) do
    local info = utils.file_info(c)
    if info and info.is_file then return c end
  end
end

-- AC power (true/false/nil=unknown), read straight from sysfs.
local function read_power()
  local function slurp(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local v = f:read("*l")
    f:close()
    return v
  end
  local supplies = {}
  for _, name in ipairs(utils.readdir(opt.power_dir, "all") or {}) do
    local base = opt.power_dir .. "/" .. name
    local t = slurp(base .. "/type")
    if t then supplies[#supplies + 1] = { type = t, online = tonumber(slurp(base .. "/online") or "") } end
  end
  return M.on_ac(supplies)
end

-- Display size: the compositor's output size (the same number mpv hands to vsr.vpy as display_res), then the
-- property user-data/vsr/display ("WxH", a seam for tests and other scripts), osd-dimensions in fullscreen, and
-- NVVSR_DISPLAY=WxH (also read by vsr.vpy) when mpv cannot tell (--vo=null).
local function display_size()
  local w, h = mp.get_property_number("display-width", 0), mp.get_property_number("display-height", 0)
  if w > 0 and h > 0 then return w, h end
  local uw, uh = tostring(mp.get_property_native("user-data/vsr/display") or ""):match("^(%d+)x(%d+)$")
  if uw then return tonumber(uw), tonumber(uh) end
  local od = native("osd-dimensions")
  if od and od.w > 0 and od.h > 0 and mp.get_property_bool("fullscreen", false) then return od.w, od.h end
  local ew, eh = (os.getenv("NVVSR_DISPLAY") or ""):match("^(%d+)x(%d+)$")
  if ew then return tonumber(ew), tonumber(eh) end
end

local function snapshot()
  local vp, trk, fi = native("video-params"), native("current-tracks/video"), native("video-frame-info")
  local s = { fullscreen = mp.get_property_bool("fullscreen", false), video = trk ~= nil,
              image = trk ~= nil and (trk.image or trk.albumart) or false,
              user_off = S.user_off, user_on = S.user_on, failed = S.failed or session_failed,
              interlaced = fi and fi.interlaced or false,
              deinterlace_active = mp.get_property_bool("deinterlace-active", false) }
  if vp then
    s.w, s.h, s.dw, s.dh = vp.w, vp.h, vp.dw, vp.dh
    s.pixfmt, s.hw_pixfmt, s.gamma, s.primaries = vp.pixelformat, vp["hw-pixelformat"], vp.gamma, vp.primaries
    s.colormatrix, s.rotate = vp.colormatrix, vp.rotate
  end
  s.display_w, s.display_h = display_size()
  if opt.ac_only then s.on_ac = read_power() end
  return s
end

local function filter_labels()
  local found = {}
  for _, f in ipairs(native("vf") or {}) do
    if f.label == M.LABEL or f.label == M.FMT_LABEL then found[f.label] = true end
  end
  return found
end

local function remove_filters(only_sr)
  local found = filter_labels()
  for _, label in ipairs(only_sr and { M.LABEL } or { M.LABEL, M.FMT_LABEL }) do
    if found[label] then command("vf", "remove", "@" .. label) end
  end
end

local function restore_hwdec()
  if S.saved_hwdec then
    local saved = S.saved_hwdec
    S.saved_hwdec, S.hwdec_target = nil, nil
    mp.set_property_native("hwdec", saved)
    msg.verbose("hwdec restored to " .. table.concat(saved, ","))
  end
end

-- Removes everything this script added (filters, hwdec change). state is "off" or "failed".
local function disable(reason, state)
  local was = S.state
  remove_filters(false)
  restore_hwdec()
  set_state(state or "off", reason)
  if was == "starting" or was == "on" then msg.info("SR off: " .. reason) end
end

local function fail(why)
  if S.state == "failed" then return end
  msg.warn("SR failed: " .. why .. " - playing the original video")
  if not opt.retry_on_next_file then session_failed = true end
  S.failed = true
  disable("failed: " .. why, "failed")
  osd("SR failed")
end

local function add_sr_filter()
  local vpy = find_vpy()
  if not vpy then return false, "vsr.vpy not found" end
  local ok, err = command("vf", "add", M.vf_spec(vpy, requested_quality(), opt))
  if not ok then return false, "vf add failed: " .. tostring(err) end
  return true
end

-- The ladder starts at the configured quality, or at what 'auto' resolves to for this source.
local function resolved_quality(h, fps)
  local q = S.q_override or opt.quality
  if q == "auto" then return M.auto_quality(h, fps) end
  return STREAM_POS[q] or q
end

local function begin_starting()
  local now = mp.get_time()
  S.t_enable, S.settle_until, S.unscaled_since, S.samples = now, now, nil, {}
  set_state("starting", "starting")
end

local function enable(info, s)
  -- at most 6 enables in 30 s: a safety net against a flapping condition (fullscreen/hwdec reinit loops)
  local now = mp.get_time()
  local recent = {}
  for _, t in ipairs(S.enables) do if now - t < 30 then recent[#recent + 1] = t end end
  S.enables = recent
  if #S.enables >= 6 then return fail("too many restarts") end
  S.enables[#S.enables + 1] = now

  S.ladder_q = resolved_quality(s.h, mp.get_property_number("container-fps", 0)) -- where the step-down ladder starts
  S.step_q = nil

  -- hardware decoded frames cannot enter the VapourSynth filter directly: use the "-copy" variant meanwhile
  if opt.hwdec ~= "keep" and not S.saved_hwdec then
    local cur, target = native("hwdec"), nil
    if opt.hwdec == "copy" then
      target = M.copy_hwdec(cur)
    else
      target = {}
      for item in opt.hwdec:gmatch("[^,]+") do target[#target + 1] = item end
      if M.same_list(cur, target) then target = nil end
    end
    if target then
      if mp.set_property_native("hwdec", target) then
        S.saved_hwdec, S.hwdec_target = cur, table.concat(target, ",")
      else
        msg.warn("cannot switch hwdec to " .. table.concat(target, ",") .. ", keeping " .. table.concat(cur or {}, ","))
      end
    end
  end

  if info.convert and not command("vf", "add", M.FMT_SPEC) then return fail("cannot add the format filter") end
  local ok, err = add_sr_filter()
  if not ok then return fail(err) end
  begin_starting()
  S.display = s.display_w .. "x" .. s.display_h
  local _, ow, oh = M.fit(s.w, s.h, s.dw, s.dh, s.display_w, s.display_h)
  S.sr_dims = string.format("%dx%d -> %dx%d", s.w, s.h, ow, oh)
  msg.info(string.format("SR on: %s, quality %s%s, hwdec %s%s", S.sr_dims, quality_label(),
    info.convert and ", 8-bit conversion" or "",
    S.saved_hwdec and (S.hwdec_target .. " (was " .. table.concat(S.saved_hwdec, ",") .. ")") or table.concat(native("hwdec") or { "?" }, ","),
    s.on_ac == false and ", on battery" or ""))
end

local function evaluate()
  if not S.loaded or not S.probed then return end
  local s = snapshot()
  -- video-params vanish for a moment while the decoder or the filter chain restarts: not a reason to act
  if not s.w and (S.state == "starting" or S.state == "on") and s.video then return end
  local ok, reason, info = M.eligibility(s, opt)
  if ok then
    if S.state == "off" then
      enable(info, s)
    elseif S.display and S.display ~= s.display_w .. "x" .. s.display_h then
      restart("display changed to " .. s.display_w .. "x" .. s.display_h, true) -- vsr.vpy sizes the output for the display
    end
    return
  end
  if S.state == "starting" or S.state == "on" then
    disable(reason) -- automatic: the reason is in the log, no badge
  elseif S.state == "off" then
    set_state("off", reason)
    if reason == "on battery" and not S.battery_noted then
      S.battery_noted = true
      osd("SR off, battery")
      msg.info("SR off: on battery (ac_only=yes)")
    end
  end
end

local function schedule_eval()
  eval_timer:kill()
  eval_timer:resume()
end

-- Re-adds the filter (quality and display size are read when the filter starts, so changing them means a
-- restart). The format filter and hwdec stay as they are.
function restart(why, quiet)
  remove_filters(true)
  local ok, err = add_sr_filter()
  if not ok then return fail(err) end
  begin_starting()
  local dw, dh = display_size()
  S.display = dw and (dw .. "x" .. dh)
  msg.info("SR restarted, quality " .. quality_label() .. " (" .. why .. ")")
  if not quiet then osd("SR " .. quality_label()) end
end

local function step_down()
  local cur = S.step_q or S.ladder_q
  local nxt = M.next_quality(cur)
  if not nxt then
    S.failed = true
    disable("cannot keep up even at " .. cur .. " quality", "failed")
    osd("SR off, too slow")
    return
  end
  S.steps, S.step_q = S.steps + 1, nxt
  restart("frame drops")
end

local function drop_counter()
  return mp.get_property_number("frame-drop-count", 0) + mp.get_property_number("decoder-frame-drop-count", 0)
       + mp.get_property_number("vo-delayed-frame-count", 0)
end

-- Every 0.5 s while SR runs: confirms that the filter scales, notices a bypassed or removed filter, watches drops.
local function tick()
  if S.state ~= "starting" and S.state ~= "on" then return end
  local now = mp.get_time()
  if not filter_labels()[M.LABEL] then -- vf clr / vf remove by the user: respect that for this file
    S.user_off = true
    return disable("filter removed by user")
  end
  if mp.get_property_bool("pause", false) or mp.get_property_bool("seeking", false)
     or mp.get_property_bool("paused-for-cache", false) then
    S.unscaled_since, S.samples, S.settle_until = nil, {}, now + opt.settle -- nothing to judge while not playing
    return
  end
  local out, vp = native("video-out-params"), native("video-params")
  local scaled = out and vp and (out.w > vp.w or out.h > vp.h)
  if not scaled then
    -- mpv bypasses a failed filter silently (the vf entry stays), and vsr.vpy passes the video through when it
    -- cannot run: both look like "the picture did not get bigger"
    S.unscaled_since = S.unscaled_since or now
    if now - S.unscaled_since > (S.state == "starting" and opt.confirm_timeout or 2) then
      fail(S.state == "starting" and "no scaled frames from the filter" or "filter output is not scaled")
    end
    return
  end
  S.unscaled_since = nil
  if S.state == "starting" then
    set_state("on", "running")
    S.settle_until = now + opt.settle
    osd("SR")
  end
  if mp.get_property_bool("core-idle", false) then
    S.samples, S.settle_until = {}, now + opt.settle
  elseif opt.adaptive and now >= S.settle_until then
    S.samples[#S.samples + 1] = { t = now, n = drop_counter() }
    local trouble, n = M.in_trouble(S.samples, now, opt.adaptive_window, opt.adaptive_drops)
    if trouble then
      msg.info(string.format("%d dropped frames in %gs", n, opt.adaptive_window))
      step_down()
    end
  end
end

-- ---------------------------------------------------------------------------------------------------------
-- Stats overlay
-- ---------------------------------------------------------------------------------------------------------
local function stats_text()
  local out, vp = native("video-out-params"), native("video-params")
  local function dims(p) return p and string.format("%dx%d", p.w, p.h) or "?" end
  local dw, dh = display_size()
  return table.concat({
    "{\\b1}Super resolution{\\b0}",
    string.format("state:   %s (%s)", S.state, S.reason or ""),
    string.format("quality: %s%s", S.state == "off" and "-" or quality_label(),
      S.steps > 0 and string.format("  (stepped down %dx)", S.steps) or ""),
    string.format("size:    %s -> %s", dims(vp), dims(out)),
    string.format("fps:     %.1f filter out / %.1f container", mp.get_property_number("estimated-vf-fps", 0),
      mp.get_property_number("container-fps", 0)),
    string.format("drops:   %d vo, %d decoder, %d delayed", mp.get_property_number("frame-drop-count", 0),
      mp.get_property_number("decoder-frame-drop-count", 0), mp.get_property_number("vo-delayed-frame-count", 0)),
    string.format("pool:    %d   hwdec: %s", opt.pool, mp.get_property("hwdec-current", "?")),
    string.format("display: %s   power: %s", dw and (dw .. "x" .. dh) or "unknown", ac_name(read_power())),
  }, "\\N")
end

local function render_stats()
  local text = stats_text()
  overlay.data = "{\\an7\\fs16\\bord1.2\\1c&HFFFFFF&\\3c&H000000&\\fnmonospace}" .. text
  overlay:update()
  mp.set_property_native("user-data/vsr/stats", (text:gsub("{[^}]*}", ""):gsub("\\N", "\n")))
end

local function toggle_stats()
  if overlay_timer:is_enabled() then
    overlay_timer:kill()
    overlay:remove()
  else
    render_stats()
    overlay_timer:resume()
  end
end

-- ---------------------------------------------------------------------------------------------------------
-- Events and key bindings
-- ---------------------------------------------------------------------------------------------------------
eval_timer = mp.add_timeout(0.2, evaluate, true)
tick_timer = mp.add_periodic_timer(0.5, tick, true)
probe_timer = mp.add_timeout(1.5, function() S.probed = true; schedule_eval() end, true)
overlay = mp.create_osd_overlay("ass-events")
overlay_timer = mp.add_periodic_timer(opt.stats_interval, render_stats, true)
overlay_timer:kill()

local last_ac = opt.ac_only and read_power() or nil
mp.add_periodic_timer(opt.check_interval, function()
  if not opt.ac_only then return end
  local ac = read_power()
  if ac ~= last_ac then
    last_ac = ac
    msg.info("power source: " .. ac_name(ac))
    if ac ~= false then S.battery_noted = false end
    schedule_eval()
  end
end)

local function has_leftovers() return S.state ~= "off" or S.saved_hwdec or next(filter_labels()) end

mp.add_hook("on_before_start_file", 50, function()
  -- the previous file is gone: leave no filter or hwdec change behind for the next one
  if has_leftovers() then disable("new file") end
  S.loaded = false
end)

mp.register_event("file-loaded", function()
  S.loaded, S.probed = true, false
  S.user_off, S.user_on, S.failed, S.q_override, S.battery_noted = false, false, false, nil, false
  S.steps, S.ladder_q, S.step_q, S.enables = 0, nil, nil, {}
  set_state("off", "file loaded")
  probe_timer:kill()
  probe_timer:resume() -- waits for the first frame (playback-restart), but never longer than 1.5 s
end)

mp.register_event("playback-restart", function()
  if S.loaded and not S.probed then
    S.probed = true
    probe_timer:kill()
    schedule_eval()
  end
end)

mp.register_event("end-file", function()
  S.loaded, S.probed = false, false
  probe_timer:kill()
  if has_leftovers() then disable("end of file") end
end)

-- mpv logs this when the filter failed and bypasses it from then on (the entry stays in the vf list)
mp.enable_messages("error")
mp.register_event("log-message", function(e)
  if e.text and e.text:find("Disabling filter " .. M.LABEL, 1, true) and (S.state == "starting" or S.state == "on") then
    fail("mpv disabled the filter after an error")
  end
end)

for _, name in ipairs({ "fullscreen", "video-params", "display-width", "display-height", "deinterlace-active",
                        "user-data/vsr/display" }) do
  mp.observe_property(name, "native", schedule_eval)
end
-- seeking, pausing, buffering: the drops they cause say nothing about SR, so the drop window starts over
for _, name in ipairs({ "seeking", "pause", "paused-for-cache", "core-idle", "speed" }) do
  mp.observe_property(name, "native", function()
    S.samples, S.settle_until = {}, mp.get_time() + opt.settle
  end)
end
mp.observe_property("osd-dimensions", "native", function(_, v)
  if v and (v.w ~= S.osd_w or v.h ~= S.osd_h) then S.osd_w, S.osd_h = v.w, v.h; schedule_eval() end
end)
mp.observe_property("hwdec", "native", function(_, v)
  -- the user changed hwdec while SR runs (Ctrl+h): do not restore the old value over it later
  if S.saved_hwdec and v and table.concat(v, ",") ~= S.hwdec_target then
    S.saved_hwdec, S.hwdec_target = nil, nil
    publish()
  end
end)

mp.add_key_binding("Ctrl+s", "toggle", function()
  if S.state == "starting" or S.state == "on" then
    S.user_off, S.user_on = true, false -- sticks until toggled on again or the next file
    disable("turned off by user")
    osd("SR off")
  else
    S.user_off, S.user_on, S.failed, S.enables, S.probed, S.steps, S.step_q = false, true, false, {}, true, 0, nil
    session_failed = false -- an explicit request overrides retry_on_next_file=no
    if S.state == "failed" then set_state("off", "retry requested") end
    evaluate()
    if S.state ~= "starting" and S.state ~= "on" then osd("SR unavailable") end
  end
end)

mp.add_key_binding("Ctrl+Shift+s", "quality", function()
  S.q_override, S.step_q = M.cycle_quality(S.q_override or opt.quality), nil
  local vp = native("video-params")
  S.ladder_q = resolved_quality(vp and vp.h or 720, mp.get_property_number("container-fps", 0))
  if S.state == "starting" or S.state == "on" then
    restart("user")
  else
    osd("SR " .. S.q_override)
  end
end)

mp.add_key_binding("Alt+s", "stats", toggle_stats)

mp.add_key_binding("Ctrl+Alt+s", "osd", function()
  opt.osd = not opt.osd
  msg.info("on-screen badge " .. (opt.osd and "on" or "off"))
  if opt.osd then
    osd((S.state == "on" or S.state == "starting") and "SR" or "SR off")
  else
    badge_timer:kill()
    badge:remove()
  end
end)

publish()
