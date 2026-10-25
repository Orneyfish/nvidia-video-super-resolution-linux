// NVVSR content script (allowlisted pages only). When a video goes fullscreen after a user gesture and the source
// is smaller than the screen (so upscaling helps), it pauses the video, leaves fullscreen and hands the page URL and
// the current time to mpv through the service worker. On success the page video seeks to mpv's last position and
// stays paused. On any failure the page goes back to fullscreen, with a small toast when that is not possible.
// The decisions live in lib/logic.js (loaded before this file); this file only touches the DOM.
(function () {
  "use strict";
  if (window.__nvvsrHandoffLoaded) return;
  window.__nvvsrHandoffLoaded = true;

  const L = globalThis.NvvsrLogic;
  if (!L || typeof chrome === "undefined" || !chrome.runtime || !chrome.runtime.id) return;

  let settings = null;          // {enabled, allowlist}; null until storage is read (fail closed)
  let busy = false;             // one handoff at a time in this page
  let lastGestureAt = null;     // Date.now() of the last trusted pointer/key/touch gesture
  let lastContextTarget = null; // element of the last trusted right click
  let toastEl = null;
  let toastTimer = null;
  const limiter = new L.RateLimiter(L.HANDOFF_INTERVAL_MS);

  function loadSettings() {
    chrome.storage.sync.get({ enabled: true, allowlist: L.DEFAULT_ALLOWLIST.slice() }).then((s) => {
      settings = { enabled: s.enabled === true, allowlist: Array.isArray(s.allowlist) ? s.allowlist : [] };
    }).catch(() => { settings = null; });
  }
  loadSettings();
  chrome.storage.onChanged.addListener((changes, area) => {
    if (area === "sync" && (changes.enabled || changes.allowlist)) loadSettings();
  });

  // Gestures: only trusted events count (a page cannot synthesise them).
  for (const type of ["pointerdown", "keydown", "touchstart"]) {
    document.addEventListener(type, (e) => {
      if (e.isTrusted) lastGestureAt = Date.now();
    }, { capture: true, passive: true });
  }
  document.addEventListener("contextmenu", (e) => {
    if (e.isTrusted) lastContextTarget = e.target;
  }, true);

  function videoIn(el) {
    if (!el) return null;
    if (el.tagName === "VIDEO") return el;
    if (typeof el.querySelectorAll === "function") return L.pickVideo(Array.from(el.querySelectorAll("video")));
    return null;
  }

  function findVideo() {
    return L.pickVideo(Array.from(document.querySelectorAll("video")));
  }

  function videoInfo(video) {
    return { videoHeight: video.videoHeight, mediaKeys: video.mediaKeys || null };
  }

  // Fullscreen trigger: the page entered fullscreen on a <video> (or a player that contains one).
  document.addEventListener("fullscreenchange", () => {
    const fs = document.fullscreenElement;
    if (!fs || busy) return;
    if (!L.isUserInitiated(lastGestureAt, Date.now(), L.GESTURE_WINDOW_MS)) return;
    const video = videoIn(fs);
    if (!video || !settings) return;
    if (!L.checkPage(location.hostname, settings).ok) return;
    if (!L.shouldHandoff(videoInfo(video), { height: screen.height }, window.devicePixelRatio, settings).ok) return;
    handoff(video, "fullscreen");
  });

  function sendHandoff(request) {
    return chrome.runtime.sendMessage({ type: "handoff", request: request }).catch(() => ({
      ok: false, error: "extension messaging failed",
    }));
  }

  async function handoff(video, source) {
    if (busy) return;
    const page = L.checkPage(location.hostname, settings || { enabled: false, allowlist: [] });
    if (!page.ok) return;
    if (video.mediaKeys) {
      toast("NVVSR: DRM-protected video cannot be handed off to mpv.", null);
      return;
    }
    const url = L.canonicalizeUrl(location.href);
    if (!url) return;
    const gate = limiter.check("page", Date.now());
    if (!gate.ok) {
      if (source === "context") toast("NVVSR: wait a few seconds between handoffs.", null);
      return;
    }
    const req = L.buildRequest(url, video.currentTime, document.title);
    if (!req) return;

    busy = true;
    const wasPlaying = !video.paused;
    // While mpv plays, the page must stay quiet: players (YouTube's included) resume playback and may re-enter
    // fullscreen by themselves after the first pause/exit, which would play the video twice. Undo both, a few times.
    let reentries = 0;
    let restoring = false;
    const keepPaused = () => { try { if (busy) video.pause(); } catch (e) { /* element gone */ } };
    const keepWindowed = () => {
      if (busy && !restoring && document.fullscreenElement && reentries < 5) {
        reentries += 1;
        document.exitFullscreen().catch(() => {});
      }
    };
    video.addEventListener("play", keepPaused, true);
    document.addEventListener("fullscreenchange", keepWindowed, true);
    try {
      video.pause();
      if (document.fullscreenElement) {
        try { await document.exitFullscreen(); } catch (e) { /* not fullscreen any more */ }
      }
      const result = L.interpretReply(await sendHandoff(req));
      if (result.ok) {
        const target = L.seekTarget(result, liveVideo(video).duration);
        seekTo(liveVideo(video), target);
        if (document.fullscreenElement) await document.exitFullscreen().catch(() => {});
        return;
      }
      // The handoff failed: give the page back to the user the way they left it (fullscreen, playing).
      restoring = true;
      video.removeEventListener("play", keepPaused, true);
      if (result.time !== null) seekTo(liveVideo(video), result.time);
      await restoreFullscreen(liveVideo(video), result.error);
      if (wasPlaying) { try { await liveVideo(video).play(); } catch (e) { /* needs a gesture: the toast covers it */ } }
    } finally {
      video.removeEventListener("play", keepPaused, true);
      document.removeEventListener("fullscreenchange", keepWindowed, true);
      busy = false;
    }
  }

  // The video element may have been replaced by the page while mpv played.
  function liveVideo(video) {
    return video && video.isConnected ? video : (findVideo() || video);
  }

  function seekTo(video, t) {
    if (!video || t === null || !Number.isFinite(t) || t < 0) return;
    try {
      video.currentTime = t;
      video.pause();
    } catch (e) { /* not seekable: leave the page video where it is */ }
  }

  async function restoreFullscreen(video, reason) {
    let restored = false;
    try {
      if (video && typeof video.requestFullscreen === "function") {
        await video.requestFullscreen();
        restored = true;
      }
    } catch (e) {
      restored = false;
    }
    if (restored) return;  // back in fullscreen: no toast needed, a toast would sit under the video
    toast("NVVSR: mpv handoff failed (" + reason + "). Click to return to fullscreen.", () => {
      if (video && typeof video.requestFullscreen === "function") video.requestFullscreen().catch(() => {});
    });
  }

  // Toast in a closed shadow root, so page CSS cannot reach it. Text via textContent only.
  function toast(text, onClick) {
    if (!document.documentElement) return;
    if (toastEl) toastEl.remove();
    clearTimeout(toastTimer);
    const host = document.createElement("nvvsr-toast");
    host.style.cssText = "all: initial; position: fixed; left: 16px; bottom: 16px; z-index: 2147483647;";
    const root = host.attachShadow({ mode: "closed" });
    const box = document.createElement("div");
    box.setAttribute("style", "font: 13px/1.4 system-ui, sans-serif; color: #fff; background: rgba(20,20,20,0.92);" +
      " padding: 8px 12px; border-radius: 6px; max-width: 420px; box-shadow: 0 2px 8px rgba(0,0,0,0.4);");
    const msg = document.createElement("span");
    msg.textContent = text;
    box.appendChild(msg);
    if (onClick) {
      box.style.cursor = "pointer";
      box.addEventListener("click", (e) => {
        e.stopPropagation();
        onClick();
        host.remove();
      });
    }
    root.appendChild(box);
    document.documentElement.appendChild(host);
    toastEl = host;
    toastTimer = setTimeout(() => host.remove(), onClick ? 15000 : 6000);
  }

  // Toolbar-independent entry point: "Open in mpv (super resolution)" from the background (trusted sender only).
  chrome.runtime.onMessage.addListener((msg, sender) => {
    if (!sender || sender.id !== chrome.runtime.id) return false;
    if (!msg || msg.type !== "contextHandoff") return false;
    const t = lastContextTarget;
    const video = (t && typeof t.closest === "function" && t.closest("video")) || videoIn(t) || findVideo();
    if (!video) {
      toast("NVVSR: no video found on this page.", null);
    } else if (!settings || !L.checkPage(location.hostname, settings).ok) {
      toast("NVVSR: this site is not on the allowlist or handoff is off.", null);
    } else {
      handoff(video, "context");
    }
    return false;
  });
})();
