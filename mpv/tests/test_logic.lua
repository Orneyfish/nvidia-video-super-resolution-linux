-- Unit tests of the pure logic in mpv/vsr.lua. Run with plain LuaJIT (no mpv):
--   luajit mpv/tests/test_logic.lua
-- Prints PASS/FAIL per check; the exit status is the number of failures.
local here = arg and arg[0] and arg[0]:match("^(.*)/[^/]*$") or "."
local M = dofile(here .. "/../vsr.lua")

local pass, fail = 0, 0
local function check(name, cond, detail)
  if cond then
    pass = pass + 1
    print("PASS " .. name)
  else
    fail = fail + 1
    print("FAIL " .. name .. (detail and (": " .. tostring(detail)) or ""))
  end
end
local function eq(name, got, want) check(name, got == want, "got " .. tostring(got) .. ", expected " .. tostring(want)) end

local O = M.normalize({}) -- defaults
local function opts(over)
  local raw = {}
  for k, v in pairs(over or {}) do raw[k] = v end
  return (M.normalize(raw))
end

local NIL = {} -- marks a field to remove from the state

-- A typical eligible state: 720p SDR 8-bit, fullscreen on a 2560x1440 display, on AC.
local function state(over)
  local s = { fullscreen = true, video = true, image = false, w = 1280, h = 720, dw = 1280, dh = 720, rotate = 0,
              pixfmt = "yuv420p", gamma = "bt.1886", primaries = "bt.709", colormatrix = "bt.709",
              interlaced = false, deinterlace_active = false, display_w = 2560, display_h = 1440, on_ac = true }
  for k, v in pairs(over or {}) do
    if v == NIL then s[k] = nil else s[k] = v end
  end
  return s
end
local function why(s, o)
  local ok, reason, info = M.eligibility(s, o or O)
  return ok, reason, info
end

print("== eligibility")
do
  local ok, reason, info = why(state())
  check("720p SDR 8-bit fullscreen on 1440p on AC -> eligible", ok == true and reason == nil and info.convert == false)
  local function excluded(name, over, needle, o)
    local ok2, r = why(state(over), o)
    check(name, ok2 == false and r and r:find(needle, 1, true), r)
  end
  excluded("not fullscreen", { fullscreen = false }, "not fullscreen")
  excluded("no video track", { video = false }, "no video")
  excluded("image", { video = true, image = true }, "image")
  excluded("PQ HDR", { gamma = "pq", pixfmt = "yuv420p10" }, "HDR")
  excluded("HLG HDR", { gamma = "hlg" }, "HDR")
  excluded("Dolby Vision", { colormatrix = "dolbyvision" }, "HDR")
  excluded("BT.2020 primaries on SDR gamma", { primaries = "bt.2020" }, "BT.2020")
  excluded("BT.2020 matrix (ncl)", { colormatrix = "bt.2020-ncl", primaries = "bt.709" }, "BT.2020")
  excluded("BT.2020 matrix (cl)", { colormatrix = "bt.2020-cl", primaries = "bt.709" }, "BT.2020")
  excluded("interlaced", { interlaced = true }, "interlaced")
  excluded("interlaced but mpv deinterlaces -> still excluded only when not deinterlaced", { interlaced = true, deinterlace_active = false }, "interlaced")
  excluded("rotated video", { rotate = 90 }, "rotated")
  excluded("RGB source", { pixfmt = "rgb24" }, "pixel format")
  excluded("gray source", { pixfmt = "gray" }, "pixel format")
  excluded("alpha source (yuva420p)", { pixfmt = "yuva420p" }, "pixel format")
  excluded("source too small (240p)", { w = 320, h = 240, dw = 320, dh = 240 }, "too small")
  excluded("359p is below min_height 360", { w = 640, h = 359, dw = 640, dh = 359 }, "too small")
  excluded("display unknown", { display_w = NIL, display_h = NIL }, "display size unknown")
  excluded("on battery with ac_only", { on_ac = false }, "on battery")
  excluded("failed earlier", { failed = true }, "failed")
  excluded("turned off by user", { user_off = true }, "turned off")
  excluded("enabled=no", {}, "disabled in vsr.conf", opts({ enabled = false }))

  check("360p is accepted (min_height boundary)", why(state({ w = 640, h = 360, dw = 640, dh = 360 })))
  check("interlaced but mpv's deinterlacer is active -> eligible", why(state({ interlaced = true, deinterlace_active = true })))
  check("unknown interlace state (nil) -> eligible", why(state({ interlaced = nil })))
  check("on battery but ac_only=no -> eligible", why(state({ on_ac = false }), opts({ ac_only = false })))
  check("no power information (nil) -> eligible", why(state({ on_ac = nil })))
  check("enabled=no but turned on by the user -> eligible", why(state({ user_on = true }), opts({ enabled = false })))
  check("a BT.2020 matrix alone is enough to exclude", not why(state({ colormatrix = "bt.2020-ncl" })))
end

print("== display size rule (fit, never downscale, slop)")
do
  local function elig(w, h, dw_, dh_, disp_w, disp_h, o)
    return (why(state({ w = w, h = h, dw = dw_, dh = dh_, display_w = disp_w, display_h = disp_h }), o))
  end
  check("1080p on 1440p -> eligible (1.33x)", elig(1920, 1080, 1920, 1080, 2560, 1440))
  check("720p on 1080p -> eligible", elig(1280, 720, 1280, 720, 1920, 1080))
  check("1440p on 1440p -> not eligible", not elig(2560, 1440, 2560, 1440, 2560, 1440))
  check("1080p on 1080p -> not eligible", not elig(1920, 1080, 1920, 1080, 1920, 1080))
  check("4K on 1440p -> not eligible (never downscale)", not elig(3840, 2160, 3840, 2160, 2560, 1440))
  check("ultrawide 2560x1080 on 2560x1440: smaller in height only, fills the width -> not eligible",
    not elig(2560, 1080, 2560, 1080, 2560, 1440))
  check("1920x800 (2.4:1) on 1920x1080 -> not eligible (width already fills the display)", not elig(1920, 800, 1920, 800, 1920, 1080))
  check("1920x800 on 2560x1440 -> eligible", elig(1920, 800, 1920, 800, 2560, 1440))
  check("2540x1426 on 2560x1440 (within the 2% slop) -> not eligible", not elig(2540, 1426, 2540, 1426, 2560, 1440))
  check("2400x1350 on 2560x1440 (1.067x) -> eligible", elig(2400, 1350, 2400, 1350, 2560, 1440))
  check("slop=0 makes 2540x1426 eligible", elig(2540, 1426, 2540, 1426, 2560, 1440, opts({ slop = 0 })))
  check("anamorphic 720x576 stored, displayed 1024x576 on 1920x1080 -> eligible (uses the display size dw/dh)",
    elig(720, 576, 1024, 576, 1920, 1080))
  local scale, ow, oh = M.fit(1280, 720, 1280, 720, 2560, 1440)
  check("fit 720p -> 2560x1440", scale == 2 and ow == 2560 and oh == 1440)
  scale, ow, oh = M.fit(854, 480, 854, 480, 2560, 1440)
  check("fit 854x480 -> 2560x1438 (even, aspect kept; same as vsr.vpy)", ow == 2560 and oh == 1438, ow .. "x" .. oh)
  scale, ow, oh = M.fit(320, 180, 320, 180, 2560, 1440)
  check("fit is capped at 4x (320x180 -> 1280x720)", ow == 1280 and oh == 720)
  scale, ow, oh = M.fit(1280, 544, 1280, 544, 2560, 1440)
  check("fit 2.35:1 keeps the aspect (1280x544 -> 2560x1088)", ow == 2560 and oh == 1088)
end

print("== pixel formats and the 10-bit decision")
do
  for _, f in ipairs({ "yuv420p", "yuvj420p", "nv12", "nv21" }) do eq("direct: " .. f, M.classify_format(f), "direct") end
  for _, f in ipairs({ "yuv420p10", "yuv420p12", "p010", "p016", "yuv422p", "yuv444p", "yuv422p10", "yuv444p12", "nv16", "nv24", "yuvj444p" }) do
    eq("convert: " .. f, M.classify_format(f), "convert")
  end
  for _, f in ipairs({ "rgb24", "bgr0", "gbrp", "gray", "gray10", "pal8", "yuva420p", "ayuv" }) do
    eq("rejected: " .. f, M.classify_format(f), nil)
  end
  eq("hardware format with a known 8-bit sw format -> direct", M.classify_format("vaapi", "nv12"), "direct")
  eq("hardware format with a 10-bit sw format -> convert", M.classify_format("vaapi", "p010"), "convert")
  eq("hardware format, sw format unknown -> convert (harmless on 8-bit)", M.classify_format("cuda", nil), "convert")
  local ok, _, info = why(state({ pixfmt = "yuv420p10" }))
  check("10-bit 4:2:0 SDR -> eligible with an 8-bit conversion filter", ok and info.convert)
  ok, _, info = why(state({ pixfmt = "nv12" }))
  check("nv12 -> eligible without conversion filter", ok and not info.convert)
  local ok2, r = why(state({ pixfmt = "yuv420p10" }), opts({ convert_10bit = false }))
  check("10-bit with convert_10bit=no -> skipped", ok2 == false and r:find("convert_10bit=no", 1, true), r)
  ok, _, info = why(state({ pixfmt = "vaapi", hw_pixfmt = "p010" }))
  check("hardware surface carrying P010 -> convert", ok and info.convert)
end

print("== battery / power supplies")
do
  eq("Mains online -> AC", M.on_ac({ { type = "Mains", online = 1 }, { type = "Battery" } }), true)
  eq("Mains offline + battery -> battery", M.on_ac({ { type = "Mains", online = 0 }, { type = "Battery" } }), false)
  eq("USB-C PD charger online -> AC", M.on_ac({ { type = "USB", online = 1 }, { type = "Mains", online = 0 }, { type = "Battery" } }), true)
  eq("only a battery -> battery", M.on_ac({ { type = "Battery" } }), false)
  eq("no supplies at all (unknown) -> nil", M.on_ac({}), nil)
  eq("desktop: mains online, no battery -> AC", M.on_ac({ { type = "Mains", online = 1 } }), true)
  eq("desktop: mains offline, no battery -> unknown (not blocked)", M.on_ac({ { type = "Mains", online = 0 } }), nil)
  eq("a USB supply that is offline does not count", M.on_ac({ { type = "USB", online = 0 }, { type = "Battery" } }), false)
  eq("type match is case-insensitive", M.on_ac({ { type = "mains", online = 1 } }), true)
end

print("== quality ladder")
do
  local seq, q = {}, "ultra"
  while q do seq[#seq + 1] = q; q = M.next_quality(q) end
  eq("step-down ladder", table.concat(seq, ">"), "ultra>high>medium>low>bicubic")
  eq("bicubic is the floor", M.next_quality("bicubic"), nil)
  eq("streaming_ultra steps like ultra", M.next_quality("streaming_ultra"), "high")
  eq("streaming_medium steps like medium", M.next_quality("streaming_medium"), "low")
  eq("unknown name has no next", M.next_quality("superb"), nil)
  eq("auto_quality 360p", M.auto_quality(360, 30), "ultra")
  eq("auto_quality 540p boundary", M.auto_quality(540, 30), "ultra")
  eq("auto_quality 541p", M.auto_quality(541, 30), "high")
  eq("auto_quality 720p", M.auto_quality(720, 30), "high")
  eq("auto_quality 800p boundary", M.auto_quality(800, 60), "high")
  eq("auto_quality 801p", M.auto_quality(801, 30), "medium")
  eq("auto_quality 1080p60", M.auto_quality(1080, 60), "medium")
  eq("auto_quality 1440p at 60 fps -> low", M.auto_quality(1440, 60), "low")
  eq("auto_quality 720p at 120 fps -> one level down", M.auto_quality(720, 120), "medium")
  eq("cycle: auto -> ultra", M.cycle_quality("auto"), "ultra")
  eq("cycle: bicubic -> auto", M.cycle_quality("bicubic"), "auto")
  eq("cycle: unknown -> auto", M.cycle_quality("streaming_ultra"), "auto")
  local c, seen = "auto", {}
  for _ = 1, 6 do seen[#seen + 1] = c; c = M.cycle_quality(c) end
  eq("cycle visits every level once and returns", table.concat(seen, ",") .. "," .. c, "auto,ultra,high,medium,low,bicubic,auto")
end

print("== drop window")
do
  local samples = {}
  for i = 0, 10 do samples[#samples + 1] = { t = i * 0.5, n = 0 } end -- 5 s, no drops
  local trouble = M.in_trouble(samples, 5, 5, 10)
  check("no drops -> no trouble", not trouble)
  samples = {}
  for i = 0, 10 do samples[#samples + 1] = { t = i * 0.5, n = i } end -- 10 drops in 5 s
  local tr, n = M.in_trouble(samples, 5, 5, 10)
  check("10 drops in a full 5 s window -> trouble", tr and n == 10, n)
  samples = {}
  for i = 0, 10 do samples[#samples + 1] = { t = i * 0.5, n = i } end
  tr = M.in_trouble(samples, 5, 5, 11)
  check("threshold 11 -> no trouble", not tr)
  samples = { { t = 0, n = 0 }, { t = 0.5, n = 40 } }
  tr = M.in_trouble(samples, 0.5, 5, 10)
  check("burst but window not yet full -> no verdict", not tr)
  samples = {}
  for i = 0, 40 do samples[#samples + 1] = { t = i * 0.5, n = i < 10 and i * 5 or 45 } end -- burst early, then calm
  tr = M.in_trouble(samples, 20, 5, 10)
  check("old drops fall out of the window", not tr)
  eq("old samples are trimmed", #samples <= 12, true)
end

print("== user-data and vf specification")
do
  local ud, body = M.user_data("high", 1)
  eq("user-data is length-prefixed (N = byte length)", ud, "%" .. #body .. "%quality=high;pool=1")
  eq("user-data body", body, "quality=high;pool=1")
  eq("user-data literal", (M.user_data("auto", 1)), "%19%quality=auto;pool=1")
  eq("user-data with strength", (M.user_data("ultra", 2, 0.5)), "%" .. #"quality=ultra;pool=2;strength=0.5" .. "%quality=ultra;pool=2;strength=0.5")
  eq("N counts bytes, not characters", (M.user_data("medium", 1)):match("^%%(%d+)%%"), tostring(#"quality=medium;pool=1"))
  local spec = M.vf_spec("/opt/my dir:x/vsr.vpy", "low", O)
  eq("vf spec (path with a space and a colon is length-prefixed)", spec,
    "@nvvsr:vapoursynth=file=%21%/opt/my dir:x/vsr.vpy:buffered-frames=4:concurrent-frames=2:user-data=%18%quality=low;pool=1")
  eq("format filter spec", M.FMT_SPEC, "@nvvsr-fmt:format=fmt=yuv420p")
end

print("== hwdec")
do
  local function j(l) return l and table.concat(l, ",") or "nil" end
  eq("no hwdec -> nothing to do", j(M.copy_hwdec({ "no" })), "nil")
  eq("auto -> auto-copy", j(M.copy_hwdec({ "auto" })), "auto-copy")
  eq("auto-safe -> auto-copy", j(M.copy_hwdec({ "auto-safe" })), "auto-copy")
  eq("vaapi -> vaapi-copy", j(M.copy_hwdec({ "vaapi" })), "vaapi-copy")
  eq("nvdec,vaapi -> both copy", j(M.copy_hwdec({ "nvdec", "vaapi" })), "nvdec-copy,vaapi-copy")
  eq("already copy -> nothing to do", j(M.copy_hwdec({ "nvdec-copy" })), "nil")
  eq("mixed", j(M.copy_hwdec({ "nvdec-copy", "vaapi" })), "nvdec-copy,vaapi-copy")
  eq("empty list", j(M.copy_hwdec({})), "nil")
  eq("nil", j(M.copy_hwdec(nil)), "nil")
  check("same_list", M.same_list({ "a", "b" }, { "a", "b" }) and not M.same_list({ "a" }, { "b" }) and not M.same_list(nil, { "a" }))
end

print("== option parsing")
do
  local o, w = M.normalize({})
  check("defaults: no warnings", #w == 0 and o.enabled == true and o.ac_only == true and o.min_height == 360 and o.quality == "auto"
    and o.adaptive == true and o.hwdec == "copy" and o.pool == 1 and o.strength == nil)
  o, w = M.normalize({ quality = "  HIGH " })
  eq("quality is trimmed and lower-cased", o.quality, "high")
  o, w = M.normalize({ quality = "superb" })
  check("unknown quality -> auto + warning", o.quality == "auto" and #w == 1, w[1])
  o, w = M.normalize({ pool = 9 })
  check("pool is clamped to 4 with a warning", o.pool == 4 and #w == 1)
  o, w = M.normalize({ pool = 0 })
  eq("pool 0 -> 1", o.pool, 1)
  o, w = M.normalize({ strength = "0.4" })
  eq("strength parsed", o.strength, 0.4)
  o, w = M.normalize({ strength = "7" })
  check("strength out of range ignored", o.strength == nil and #w == 1)
  o, w = M.normalize({ strength = "x" })
  check("strength garbage ignored", o.strength == nil and #w == 1)
  o, w = M.normalize({ min_height = "abc" })
  check("non-numeric number -> default + warning", o.min_height == 360 and #w == 1)
  o = M.normalize({ hwdec = " Keep " })
  eq("hwdec normalised", o.hwdec, "keep")
  o = M.normalize({ hwdec = "" })
  eq("empty hwdec -> copy", o.hwdec, "copy")
  o = M.normalize({ adaptive_drops = 3.4 })
  eq("integer options are rounded", o.adaptive_drops, 3)
end

-- The on-screen status badge: small, transparent, one or two words, switchable.
do
  local o = M.normalize({})
  eq("osd badge on by default", o.osd, true)
  eq("osd_size default is small", o.osd_size, 11)
  check("osd_opacity default is translucent", o.osd_opacity > 0 and o.osd_opacity < 1, o.osd_opacity)
  eq("osd_position default is top right", o.osd_position, 9)
  local ass = M.badge_ass("SR", o)
  check("badge ass: top right, size 11", ass:find("\\an9", 1, true) and ass:find("\\fs11", 1, true), ass)
  check("badge ass: text is just SR", ass:match("}(.*)$") == "SR", ass)
  check("badge ass: no resolutions anywhere", not ass:find("%d+x%d+"), ass)
  local a = ass:match("\\1a&H(%x%x)&")
  check("badge ass: translucent text (alpha strictly between 00 and FF)", a and tonumber(a, 16) > 0 and tonumber(a, 16) < 255, a)
  local solid = M.badge_ass("SR", M.normalize({ osd_opacity = 1 }))
  check("opacity 1 -> alpha 00", solid:find("\\1a&H00&", 1, true) ~= nil, solid)
  local faint = M.badge_ass("SR", M.normalize({ osd_opacity = 0.1 }))
  check("opacity 0.1 -> mostly transparent", tonumber(faint:match("\\1a&H(%x%x)&"), 16) > 200, faint)
  local o2, w = M.normalize({ osd_size = 500, osd_opacity = 0, osd_position = 12, osd_duration = -3 })
  eq("osd_size clamped", o2.osd_size, 40)
  eq("osd_opacity clamped", o2.osd_opacity, 0.1)
  eq("osd_position clamped", o2.osd_position, 9)
  eq("osd_duration clamped", o2.osd_duration, 0)
  eq("four clamp warnings", #w, 4)
  eq("osd can be switched off", M.normalize({ osd = false }).osd, false)
  eq("osd_position 7 -> top left", M.badge_ass("SR", M.normalize({ osd_position = 7 })):match("\\an(%d)"), "7")
end

print(string.format("\nSUMMARY: %d passed, %d failed", pass, fail))
os.exit(fail)
