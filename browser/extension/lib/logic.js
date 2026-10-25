// Pure decision logic for the NVVSR mpv handoff. No DOM and no chrome.* access, so the content script, the
// service worker, the options page and the node tests (browser/tests/logic.test.js) share one copy.
// Loaded as a classic script (manifest content_scripts, importScripts, a script tag): it sets globalThis.NvvsrLogic,
// or module.exports under node.
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module && module.exports) {
    module.exports = api;
  } else {
    root.NvvsrLogic = api;
  }
})(typeof globalThis !== "undefined" ? globalThis : self, function () {
  "use strict";

  const HOST_NAME = "com.nvvsr.mpv_handoff";
  const DEFAULT_ALLOWLIST = Object.freeze([
    "youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be", "vimeo.com", "twitch.tv",
  ]);
  // Sites whose video is DRM-protected in practice: never handed off, even when allowlisted.
  const DRM_ONLY_HOSTS = Object.freeze([
    "netflix.com", "disneyplus.com", "primevideo.com", "hulu.com", "max.com", "hbomax.com",
    "peacocktv.com", "tv.apple.com",
  ]);
  const MIN_VIDEO_HEIGHT = 360;
  const HANDOFF_INTERVAL_MS = 3000;   // at most one handoff per tab in this window
  const GESTURE_WINDOW_MS = 5000;     // a fullscreen change this soon after a user gesture counts as user-initiated
  const MAX_ALLOWLIST = 50;
  const MAX_TITLE = 200;
  const MAX_TIME = 7 * 86400;
  const HOST_ERRORS = Object.freeze({
    missing: "host-missing",
    failed: "host-error",
  });

  const TRACKING_PARAMS = /^(utm_[a-z_]+|fbclid|gclid|mc_cid|mc_eid|ref_src|igshid|si|feature|pp)$/i;
  const YOUTUBE_KEEP = new Set(["v", "list", "t", "index"]);
  const ALLOW_ENTRY = /^(\*\.)?[a-z0-9-]+(\.[a-z0-9-]+)*$/;

  function normalizeHost(host) {
    return String(host || "").toLowerCase().replace(/\.+$/, "");
  }

  // Allowlist entries are hostnames ("youtube.com", exact) or "*.example.com" (any subdomain of example.com).
  function isValidAllowEntry(entry) {
    if (typeof entry !== "string" || entry.length === 0 || entry.length > 253) return false;
    return ALLOW_ENTRY.test(entry);
  }

  function hostMatches(host, entry) {
    const h = normalizeHost(host);
    const e = normalizeHost(entry);
    if (!h || !e) return false;
    if (e.startsWith("*.")) {
      const base = e.slice(2);
      return h.length > base.length + 1 && h.endsWith("." + base);
    }
    return h === e;
  }

  function allowlisted(host, allowlist) {
    return Array.isArray(allowlist) && allowlist.some((entry) => hostMatches(host, entry));
  }

  function isDrmHost(host) {
    const h = normalizeHost(host);
    return DRM_ONLY_HOSTS.some((d) => h === d || h.endsWith("." + d));
  }

  // Page-level gate, checked before anything else: handoff enabled, host allowlisted, not a DRM-only site.
  function checkPage(host, settings) {
    if (!settings || settings.enabled !== true) return { ok: false, reason: "disabled" };
    if (!allowlisted(host, settings.allowlist)) return { ok: false, reason: "not-allowlisted" };
    if (isDrmHost(host)) return { ok: false, reason: "drm-site" };
    return { ok: true, reason: "" };
  }

  // Video-level gate for the fullscreen trigger. `video` = {videoHeight, mediaKeys}, `screen` = {height} (CSS px),
  // dpr = devicePixelRatio. Upscaling only helps when the source is smaller than the physical screen height.
  function shouldHandoff(video, screen, dpr, settings) {
    if (!settings || settings.enabled !== true) return { ok: false, reason: "disabled" };
    if (!video) return { ok: false, reason: "no-video" };
    if (video.mediaKeys) return { ok: false, reason: "drm" };
    const h = Number(video.videoHeight);
    if (!Number.isFinite(h) || h <= 0) return { ok: false, reason: "no-video-size" };
    if (h < MIN_VIDEO_HEIGHT) return { ok: false, reason: "too-small" };
    const target = Number(screen && screen.height) * Number(dpr);
    if (!(target > 0)) return { ok: false, reason: "no-screen" };
    if (h >= target) return { ok: false, reason: "native-resolution" };
    return { ok: true, reason: "upscale" };
  }

  // The largest video (by pixels) that has a picture, from a list of candidates; null when none.
  function pickVideo(list) {
    let best = null;
    let bestArea = 0;
    for (const v of list || []) {
      const area = Number(v && v.videoWidth) * Number(v && v.videoHeight);
      if (area > bestArea) {
        best = v;
        bestArea = area;
      }
    }
    return best;
  }

  function isYouTubeHost(host) {
    const h = normalizeHost(host);
    return h === "youtu.be" || h === "youtube.com" || h.endsWith(".youtube.com");
  }

  // Canonical page URL for the handoff: http(s) only, no credentials, no fragment. YouTube keeps only v, list, t,
  // index; other sites drop well-known tracking parameters. Returns null when the URL is not http(s).
  function canonicalizeUrl(href) {
    let u;
    try {
      u = new URL(String(href));
    } catch (e) {
      return null;
    }
    if (u.protocol !== "https:" && u.protocol !== "http:") return null;
    if (!u.hostname) return null;
    u.username = "";
    u.password = "";
    u.hash = "";
    const keep = [];
    const youtube = isYouTubeHost(u.hostname);
    for (const [k, v] of u.searchParams) {
      if (youtube ? YOUTUBE_KEEP.has(k) : !TRACKING_PARAMS.test(k)) keep.push([k, v]);
    }
    u.search = "";
    for (const [k, v] of keep) u.searchParams.append(k, v);
    return u.href;
  }

  // Strips control and format characters (bidi overrides included), collapses whitespace, caps the length.
  function cleanTitle(value) {
    if (typeof value !== "string") return "";
    // \p{C} (control, format, surrogate, private use, unassigned) and line/paragraph separators become spaces;
    // the u flag keeps emoji (surrogate pairs) intact.
    const cleaned = value.slice(0, 4096).replace(/[\p{C}\p{Zl}\p{Zp}]/gu, " ");
    return cleaned.split(/\s+/).filter(Boolean).join(" ").slice(0, MAX_TITLE);
  }

  // The request sent to the host. Returns null when the inputs cannot form a valid request.
  function buildRequest(url, currentTime, title) {
    if (typeof url !== "string" || url.length === 0) return null;
    if (typeof currentTime !== "number" || !Number.isFinite(currentTime) || currentTime < 0) return null;
    const t = currentTime;
    const req = { op: "handoff", url: url, currentTime: Math.min(Math.round(t * 1000) / 1000, MAX_TIME) };
    const cleaned = cleanTitle(title);
    if (cleaned) req.title = cleaned;
    return req;
  }

  // The reply from the host, normalised. `time` is null when unknown.
  function interpretReply(reply) {
    if (!reply || typeof reply !== "object") {
      return { ok: false, time: null, ended: false, error: "no reply from the host" };
    }
    const time = Number.isFinite(reply.time) && reply.time >= 0 ? reply.time : null;
    if (reply.ok === true && time !== null) {
      return { ok: true, time: time, ended: reply.ended === true, error: null };
    }
    const error = typeof reply.error === "string" ? reply.error.slice(0, 200) : "the host failed";
    return { ok: false, time: time, ended: false, error: error };
  }

  // Where the page video goes after a successful handoff: the end when mpv reached it, else mpv's time.
  function seekTarget(result, duration) {
    if (!result || !result.ok) return null;
    if (result.ended && Number.isFinite(duration) && duration > 0) return duration;
    return result.time;
  }

  // Splits the options text (one entry per line, commas or spaces allowed) into valid, unique entries.
  function parseAllowlist(text) {
    const seen = new Set();
    const entries = [];
    const invalid = [];
    const tokens = String(text || "").split(/[\s,]+/).filter(Boolean);
    for (const raw of tokens) {
      const entry = raw.toLowerCase().replace(/\.+$/, "");
      if (!isValidAllowEntry(entry)) {
        invalid.push(raw);
      } else if (!seen.has(entry)) {
        seen.add(entry);
        entries.push(entry);
      }
    }
    if (entries.length > MAX_ALLOWLIST) invalid.push("more than " + MAX_ALLOWLIST + " entries");
    return { entries: entries.slice(0, MAX_ALLOWLIST), invalid: invalid };
  }

  // Sliding limiter, one slot per key: a key may act once per intervalMs. A refused attempt does not extend the wait.
  class RateLimiter {
    constructor(intervalMs) {
      this.intervalMs = intervalMs;
      this.last = new Map();
    }
    check(key, now) {
      const prev = this.last.get(key);
      if (prev !== undefined) {
        const elapsed = now - prev;
        if (elapsed >= 0 && elapsed < this.intervalMs) {
          return { ok: false, retryInMs: this.intervalMs - elapsed };
        }
      }
      this.last.set(key, now);
      return { ok: true, retryInMs: 0 };
    }
    forget(key) {
      this.last.delete(key);
    }
  }

  // A fullscreen change counts as user-initiated when a trusted user gesture happened shortly before it.
  function isUserInitiated(lastGestureAt, now, windowMs) {
    if (typeof lastGestureAt !== "number" || typeof now !== "number") return false;
    const elapsed = now - lastGestureAt;
    return elapsed >= 0 && elapsed <= windowMs;
  }

  // Checks a request coming from a content script before the service worker launches the host.
  function validateRequest(req) {
    if (!req || typeof req !== "object" || req.op !== "handoff") return null;
    const allowed = new Set(["op", "url", "currentTime", "title"]);
    if (Object.keys(req).some((k) => !allowed.has(k))) return null;
    return buildRequest(req.url, req.currentTime, req.title);
  }

  function hostErrorFrom(message) {
    if (typeof message === "string" && /host not found|not installed/i.test(message)) {
      return HOST_ERRORS.missing;
    }
    return HOST_ERRORS.failed;
  }

  return {
    HOST_NAME,
    DEFAULT_ALLOWLIST,
    DRM_ONLY_HOSTS,
    MIN_VIDEO_HEIGHT,
    HANDOFF_INTERVAL_MS,
    GESTURE_WINDOW_MS,
    MAX_TITLE,
    MAX_TIME,
    HOST_ERRORS,
    normalizeHost,
    isValidAllowEntry,
    hostMatches,
    allowlisted,
    isDrmHost,
    checkPage,
    shouldHandoff,
    pickVideo,
    canonicalizeUrl,
    cleanTitle,
    buildRequest,
    validateRequest,
    interpretReply,
    seekTarget,
    parseAllowlist,
    RateLimiter,
    isUserInitiated,
    hostErrorFrom,
  };
});
