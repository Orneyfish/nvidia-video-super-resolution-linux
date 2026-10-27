// Upscales the fullscreen <video> with WebGPU (src/upscaler.js) onto a canvas laid over it; audio and controls are untouched.
// Runs only in fullscreen, on AC power, when the picture is smaller than the screen; any failure leaves the plain video.
// Alt+S switches it off/on (A/B). Debug: open a page with #nvvsr-debug=PORT to post the stats to a local test server.
(() => {
  const OPTS = { mode: 'cnn', cnnSize: 'S' };  // FSR ({mode:'fsr'}) is the cheaper fallback, see webgpu/README.md
  const port = /[#&]nvvsr-debug=(\d+)/.exec(location.hash)?.[1];
  const log = (...a) => { console.log('[nvvsr]', ...a); report({ log: a.join(' ') }); };
  const report = o => { if (port) navigator.sendBeacon(`http://127.0.0.1:${port}/post?name=ext-${location.hostname}-${Date.now()}.json`, JSON.stringify({ url: location.href, ...o })); };
  let active = null, off = false;

  const badge = text => {
    const b = document.createElement('div');
    b.textContent = text;
    b.style.cssText = 'position:fixed;top:10px;right:12px;z-index:2147483647;font:11px sans-serif;color:#fff;background:#0008;padding:1px 6px;border-radius:3px;opacity:.55;pointer-events:none';
    (document.fullscreenElement || document.body).append(b);
    setTimeout(() => b.remove(), 1500);
  };

  const biggestVideo = root => root instanceof HTMLVideoElement ? root
    : [...root.querySelectorAll('video')].map(v => [v, v.getBoundingClientRect()]).sort((a, b) => b[1].width * b[1].height - a[1].width * a[1].height)[0]?.[0];

  const stop = () => {
    if (!active) return;
    clearInterval(active.timer); active.ro.disconnect(); active.up.destroy(); active.canvas.remove(); active = null;
  };

  async function start(video) {  // NVIDIA only (user rule: never the iGPU); Chrome itself must run on the RTX or the frame import fails
    const dpr = devicePixelRatio, box = video.getBoundingClientRect();
    if (!video.videoHeight || Math.min(box.width * dpr / video.videoWidth, box.height * dpr / video.videoHeight) < 1.02) return log('nothing to gain', video.videoWidth + 'x' + video.videoHeight);
    const bat = await navigator.getBattery?.().catch(() => null);
    if (bat && !bat.charging) return log('on battery, off');
    const canvas = document.createElement('canvas');
    const place = () => Object.assign(canvas.style, { position: 'absolute', left: video.offsetLeft + 'px', top: video.offsetTop + 'px',
      width: video.offsetWidth + 'px', height: video.offsetHeight + 'px', objectFit: getComputedStyle(video).objectFit, pointerEvents: 'none' });
    place(); video.after(canvas);
    try {
      const { VideoUpscaler } = await import(chrome.runtime.getURL('src/upscaler.js'));
      const up = await VideoUpscaler.create(video, canvas, { ...OPTS, powerPreference: 'high-performance', onError: e => {
        log('failed', e.code, e.message); stop(); badge('SR failed');
      } });
      if (up.adapterInfo.vendor !== 'nvidia' || !document.fullscreenElement) { up.destroy(); canvas.remove(); return log('off:', up.adapterInfo.vendor, 'adapter or fullscreen left'); }
      const ro = new ResizeObserver(place); ro.observe(video);
      const timer = setInterval(() => { place(); report({ stats: up.stats() }); }, 2000);
      active = { up, canvas, ro, timer }; up.start(); badge('SR'); log('on', up.adapterInfo.vendor, video.videoWidth + 'x' + video.videoHeight);
    } catch (e) { canvas.remove(); log('failed', e.code || e.name, e.message); badge('SR failed'); }
  }

  const onFullscreen = () => {
    stop();
    const root = document.fullscreenElement, video = root && biggestVideo(root);
    if (video && !off) start(video);
  };
  document.addEventListener('fullscreenchange', onFullscreen);
  addEventListener('keydown', e => { if (e.altKey && e.code === 'KeyS') { off = !off; stop(); if (!off) onFullscreen(); badge(off ? 'SR off' : 'SR'); } });
  report({ loaded: true });
})();
