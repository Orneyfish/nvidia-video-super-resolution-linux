// Extension decision logic (browser/extension/lib/logic.js). Run: node --test browser/tests/logic.test.js
"use strict";
const test = require("node:test");
const assert = require("node:assert/strict");
const L = require("../extension/lib/logic.js");

const ON = { enabled: true, allowlist: L.DEFAULT_ALLOWLIST.slice() };
const screen1440 = { height: 1440 };

test("thresholds: a 720p video on a 1440p screen is handed off", () => {
  assert.deepEqual(L.shouldHandoff({ videoHeight: 720, mediaKeys: null }, screen1440, 1, ON), { ok: true, reason: "upscale" });
});

test("thresholds: a 1440p video on a 1440p screen is not", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 1440, mediaKeys: null }, screen1440, 1, ON).reason, "native-resolution");
});

test("thresholds: 1080p yes, 2160p no, 1440p with dpr 1.25 on a 1152 CSS-px panel no", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 1080 }, screen1440, 1, ON).ok, true);
  assert.equal(L.shouldHandoff({ videoHeight: 2160 }, screen1440, 1, ON).ok, false);
  assert.equal(L.shouldHandoff({ videoHeight: 1440 }, { height: 1152 }, 1.25, ON).ok, false);
  assert.equal(L.shouldHandoff({ videoHeight: 1080 }, { height: 1152 }, 1.25, ON).ok, true);
});

test("thresholds: devicePixelRatio 2 on a 900 CSS-px screen (1800 physical) upscales 1080p, not 1800p", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 1080 }, { height: 900 }, 2, ON).ok, true);
  assert.equal(L.shouldHandoff({ videoHeight: 1800 }, { height: 900 }, 2, ON).ok, false);
});

test("thresholds: below 360 lines is not handed off, 360 is", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 240 }, screen1440, 1, ON).reason, "too-small");
  assert.equal(L.shouldHandoff({ videoHeight: 352 }, screen1440, 1, ON).reason, "too-small");
  assert.equal(L.shouldHandoff({ videoHeight: 360 }, screen1440, 1, ON).ok, true);
});

test("DRM: a video with mediaKeys is never handed off", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 720, mediaKeys: {} }, screen1440, 1, ON).reason, "drm");
});

test("video size unknown (not loaded yet) is not handed off", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 0 }, screen1440, 1, ON).reason, "no-video-size");
  assert.equal(L.shouldHandoff({ videoHeight: NaN }, screen1440, 1, ON).reason, "no-video-size");
  assert.equal(L.shouldHandoff(null, screen1440, 1, ON).reason, "no-video");
});

test("the global switch and missing settings turn everything off", () => {
  assert.equal(L.shouldHandoff({ videoHeight: 720 }, screen1440, 1, { enabled: false, allowlist: [] }).reason, "disabled");
  assert.equal(L.shouldHandoff({ videoHeight: 720 }, screen1440, 1, null).reason, "disabled");
  assert.equal(L.checkPage("youtube.com", { enabled: false, allowlist: ["youtube.com"] }).reason, "disabled");
  assert.equal(L.checkPage("youtube.com", null).ok, false);
});

test("allowlist: default hosts match exactly, other hosts do not", () => {
  for (const h of ["youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be", "vimeo.com", "twitch.tv"]) {
    assert.equal(L.checkPage(h, ON).ok, true, h);
  }
  for (const h of ["example.com", "notyoutube.com", "youtube.com.evil.example", "evil-youtube.com", "player.vimeo.com"]) {
    assert.equal(L.checkPage(h, ON).reason, "not-allowlisted", h);
  }
  assert.equal(L.checkPage("YouTube.COM.", ON).ok, true, "case and trailing dot");
});

test("allowlist: *.example.com matches subdomains only", () => {
  const s = { enabled: true, allowlist: ["*.twitch.tv"] };
  assert.equal(L.checkPage("www.twitch.tv", s).ok, true);
  assert.equal(L.checkPage("a.b.twitch.tv", s).ok, true);
  assert.equal(L.checkPage("twitch.tv", s).ok, false);
  assert.equal(L.checkPage("eviltwitch.tv", s).ok, false);
});

test("DRM-only sites are refused even when allowlisted", () => {
  const s = { enabled: true, allowlist: ["netflix.com", "www.netflix.com", "*.hulu.com"] };
  assert.equal(L.checkPage("www.netflix.com", s).reason, "drm-site");
  assert.equal(L.checkPage("netflix.com", s).reason, "drm-site");
  assert.equal(L.checkPage("help.hulu.com", s).reason, "drm-site");
  assert.equal(L.isDrmHost("notnetflix.com"), false);
});

test("allowlist entries are validated", () => {
  const { entries, invalid } = L.parseAllowlist("YouTube.com, *.Twitch.tv bad/entry m.youtube.com. https://x.y\n  example.org  youtube.com");
  assert.deepEqual(entries, ["youtube.com", "*.twitch.tv", "m.youtube.com", "example.org"]);
  assert.deepEqual(invalid, ["bad/entry", "https://x.y"]);
  assert.equal(L.isValidAllowEntry("*.*.foo.com"), false);
  assert.equal(L.isValidAllowEntry("a b"), false);
  assert.equal(L.isValidAllowEntry(""), false);
  const many = Array.from({ length: 60 }, (_, i) => "site" + i + ".example");
  const parsed = L.parseAllowlist(many.join(" "));
  assert.equal(parsed.entries.length, 50);
  assert.ok(parsed.invalid.length >= 1);
});

test("URL canonicalisation: YouTube keeps v, list, t, index and drops tracking", () => {
  assert.equal(
    L.canonicalizeUrl("https://www.youtube.com/watch?v=abc&utm_source=x&list=PL1&t=10s&si=zz&feature=share#h"),
    "https://www.youtube.com/watch?v=abc&list=PL1&t=10s",
  );
  assert.equal(L.canonicalizeUrl("https://youtu.be/abc?si=q&t=42"), "https://youtu.be/abc?t=42");
  assert.equal(L.canonicalizeUrl("https://m.youtube.com/watch?v=abc&pp=0gcJ"), "https://m.youtube.com/watch?v=abc");
});

test("URL canonicalisation: other sites drop tracking parameters, keep the rest", () => {
  assert.equal(L.canonicalizeUrl("https://vimeo.com/123?utm_medium=x&fbclid=y&autoplay=1"), "https://vimeo.com/123?autoplay=1");
  assert.equal(L.canonicalizeUrl("https://www.twitch.tv/videos/999?t=1h2m"), "https://www.twitch.tv/videos/999?t=1h2m");
});

test("URL canonicalisation: credentials and fragments are removed, non-http(s) refused", () => {
  assert.equal(L.canonicalizeUrl("https://user:pw@vimeo.com/1#frag"), "https://vimeo.com/1");
  assert.equal(L.canonicalizeUrl("file:///etc/passwd"), null);
  assert.equal(L.canonicalizeUrl("javascript:alert(1)"), null);
  assert.equal(L.canonicalizeUrl("data:text/html,hi"), null);
  assert.equal(L.canonicalizeUrl("not a url"), null);
  assert.equal(L.canonicalizeUrl(""), null);
});

test("titles: control and format characters go, emoji stay, length is capped", () => {
  assert.equal(L.cleanTitle("a\u0000b\u001b[31m c\u0007"), "a b [31m c");
  assert.equal(L.cleanTitle("x‮evil‬"), "x evil");
  assert.equal(L.cleanTitle("line1\nline2\r\n\tend"), "line1 line2 end");
  assert.equal(L.cleanTitle("Movie 🎬 trailer"), "Movie 🎬 trailer");
  assert.equal(L.cleanTitle("a\ud800b"), "a b");
  assert.equal(L.cleanTitle("x".repeat(1000)).length, L.MAX_TITLE);
  assert.equal(L.cleanTitle(undefined), "");
  assert.equal(L.cleanTitle(42), "");
});

test("request: time rounded and bounded, title only when present", () => {
  assert.deepEqual(L.buildRequest("https://a.example/", 12.34567, "T"),
    { op: "handoff", url: "https://a.example/", currentTime: 12.346, title: "T" });
  assert.deepEqual(L.buildRequest("https://a.example/", 0, ""), { op: "handoff", url: "https://a.example/", currentTime: 0 });
  assert.equal(L.buildRequest("https://a.example/", -1, ""), null);
  assert.equal(L.buildRequest("https://a.example/", NaN, ""), null);
  assert.equal(L.buildRequest("https://a.example/", "5", ""), null);
  assert.equal(L.buildRequest("https://a.example/", null, ""), null);
  assert.equal(L.buildRequest("", 5, ""), null);
  assert.equal(L.buildRequest("https://a.example/", 1e9, "").currentTime, L.MAX_TIME);
});

test("request from a content script is re-validated: extra fields are refused", () => {
  assert.equal(L.validateRequest({ op: "handoff", url: "https://a.example/", currentTime: 1, mpvArgs: "--x" }), null);
  assert.equal(L.validateRequest({ op: "launch", url: "https://a.example/", currentTime: 1 }), null);
  assert.equal(L.validateRequest(null), null);
  assert.equal(L.validateRequest({ op: "handoff", url: "https://a.example/", currentTime: 3 }).currentTime, 3);
});

test("rate limit: one handoff per tab per 3 s, refusals do not extend the wait", () => {
  const rl = new L.RateLimiter(3000);
  assert.deepEqual(rl.check("tab:1", 10000), { ok: true, retryInMs: 0 });
  assert.equal(rl.check("tab:1", 10500).ok, false);
  assert.equal(rl.check("tab:1", 10500).retryInMs, 2500);
  assert.equal(rl.check("tab:1", 12900).ok, false, "refusal at 12.9 s must not restart the window");
  assert.equal(rl.check("tab:1", 13000).ok, true, "allowed 3 s after the last accepted attempt");
  assert.equal(rl.check("tab:2", 13000).ok, true, "other tabs are independent");
  rl.forget("tab:2");
  assert.equal(rl.check("tab:2", 13001).ok, true);
});

test("rate limit: a clock that jumps backwards does not lock the tab", () => {
  const rl = new L.RateLimiter(3000);
  rl.check("k", 100000);
  assert.equal(rl.check("k", 5000).ok, true);
});

test("user-initiated: a fullscreen change within 5 s of a gesture counts, older or future ones do not", () => {
  assert.equal(L.isUserInitiated(1000, 3000, 5000), true);
  assert.equal(L.isUserInitiated(1000, 6001, 5000), false);
  assert.equal(L.isUserInitiated(null, 3000, 5000), false);
  assert.equal(L.isUserInitiated(5000, 3000, 5000), false);
});

test("replies: success, failure, and garbage are normalised", () => {
  assert.deepEqual(L.interpretReply({ ok: true, time: 8.25, ended: false, error: null }),
    { ok: true, time: 8.25, ended: false, error: null });
  assert.equal(L.interpretReply({ ok: true, time: 0, ended: true }).ended, true);
  assert.deepEqual(L.interpretReply({ ok: false, time: null, error: "mpv not found" }),
    { ok: false, time: null, ended: false, error: "mpv not found" });
  assert.equal(L.interpretReply({ ok: false, time: 12, error: "mpv crashed" }).time, 12);
  assert.equal(L.interpretReply({ ok: true, time: NaN }).ok, false, "a NaN time is not success");
  assert.equal(L.interpretReply(undefined).ok, false);
  assert.equal(L.interpretReply("text").error, "no reply from the host");
  assert.equal(L.interpretReply({ ok: false, error: "x".repeat(500) }).error.length, 200);
});

test("seek target: mpv's time, the end when ended, nothing on failure", () => {
  assert.equal(L.seekTarget({ ok: true, time: 8.2, ended: false }, 100), 8.2);
  assert.equal(L.seekTarget({ ok: true, time: 19.9, ended: true }, 20), 20);
  assert.equal(L.seekTarget({ ok: true, time: 19.9, ended: true }, NaN), 19.9);
  assert.equal(L.seekTarget({ ok: false, time: 4 }, 100), null);
  assert.equal(L.seekTarget(null, 100), null);
});

test("host errors map to the two fallback classes", () => {
  assert.equal(L.hostErrorFrom("Specified native messaging host not found."), L.HOST_ERRORS.missing);
  assert.equal(L.hostErrorFrom("Access to the specified native messaging host is forbidden."), L.HOST_ERRORS.failed);
  assert.equal(L.hostErrorFrom(undefined), L.HOST_ERRORS.failed);
});

test("pickVideo chooses the largest picture, null when there is none", () => {
  const a = { videoWidth: 640, videoHeight: 360 };
  const b = { videoWidth: 1280, videoHeight: 720 };
  assert.equal(L.pickVideo([a, b]), b);
  assert.equal(L.pickVideo([{ videoWidth: 0, videoHeight: 0 }]), null);
  assert.equal(L.pickVideo([]), null);
  assert.equal(L.pickVideo(undefined), null);
});

test("the handoff host name matches the manifest template name", () => {
  assert.equal(L.HOST_NAME, "com.nvvsr.mpv_handoff");
});
