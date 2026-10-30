// Upscales the fullscreen <video> with NVIDIA VSR (tensor cores) through the local daemon nvvsr_webd (webgpu/daemon): each decoded
// frame goes out as I420 over ws://127.0.0.1, the upscaled I420 reply is drawn on a canvas laid over the video; audio and controls
// are untouched. Fullscreen only, on AC power, when the picture is smaller than the video box; any failure gives back the plain video.
// Alt+S switches it off/on (A/B). Debug: open a page with #nvvsr-debug (or #nvvsr-debug=PORT to also post to a local test server)
// to log the stats every 2 s.
(() => {
  const DAEMON = 'ws://127.0.0.1:41777';  // start it with --origin <this page's origin>: a content script's WebSocket carries the page origin
  const port = /[#&]nvvsr-debug=(\d+)/.exec(location.hash)?.[1], debug = /[#&]nvvsr-debug/.test(location.hash);
  const log = (...a) => { console.log('[nvvsr]', ...a); report({ log: a.join(' ') }); };
  const report = o => { if (port) navigator.sendBeacon(`http://127.0.0.1:${port}/post?name=ext-${location.hostname}-${Date.now()}.json`, JSON.stringify({ url: location.href, ...o })); };
  let active = null, off = false, gen = 0;  // gen: a start() that sees a newer gen was cancelled by stop()

  const badge = text => {
    const b = document.createElement('div');
    b.textContent = text;
    b.style.cssText = 'position:fixed;top:10px;right:12px;z-index:2147483647;font:11px sans-serif;color:#fff;background:#0008;padding:1px 6px;border-radius:3px;opacity:.55;pointer-events:none';
    (document.fullscreenElement || document.body).append(b);
    setTimeout(() => b.remove(), 1500);
  };

  const biggestVideo = root => root instanceof HTMLVideoElement ? root
    : [...root.querySelectorAll('video')].map(v => [v, v.getBoundingClientRect()]).sort((a, b) => b[1].width * b[1].height - a[1].width * a[1].height)[0]?.[0];

  // Output size: the source fitted into the video box (object-fit: contain) x devicePixelRatio, 1x to 4x, even, inside the daemon's
  // limits. null = nothing to gain (< 2%) or a size the daemon refuses: leave the plain video.
  const FORCE = /[#&]nvvsr-box=(\d+)x(\d+)/.exec(location.hash);  // debug: pretend the video box is WxH device pixels (headless outputs are small)
  const fit = (w, h, box) => {
    const bw = FORCE ? +FORCE[1] : box.width * devicePixelRatio, bh = FORCE ? +FORCE[2] : box.height * devicePixelRatio;
    const k = Math.min(bw / w, bh / h, 4, 4096 / w, 2304 / h);
    return (w | h) & 1 || w < 64 || h < 64 || w > 4096 || h > 2304 || k < 1.02 ? null : [Math.round(w * k / 2) * 2, Math.round(h * k / 2) * 2];
  };

  const stop = () => {
    gen++;
    if (!active) return;
    const a = active;
    active = null;
    clearInterval(a.timer); a.ro.disconnect(); a.ws.close(); a.canvas.remove(); a.video.cancelVideoFrameCallback(a.vfc);
  };

  async function start(video, mine) {
    if (video.mediaKeys) return log('DRM video, off');
    if (!video.videoHeight || !fit(video.videoWidth, video.videoHeight, video.getBoundingClientRect())) return log('nothing to gain', video.videoWidth + 'x' + video.videoHeight);
    const bat = await navigator.getBattery?.().catch(() => null);
    if (bat && !bat.charging) return log('on battery, off');
    if (mine !== gen) return;

    const canvas = document.createElement('canvas'), ctx = canvas.getContext('2d');
    const st = { sent: 0, recv: 0, dropped: 0, daemonDrop: 0, daemonErr: 0, fmt: '' };  // counters since the start
    const win = { t: performance.now(), fin: 0, recv: 0, rtt: [], copy: [], draw: [], lag: [] };  // the last 2 s
    let box = video.getBoundingClientRect(), seq = 0, copying = false, opened = false, buf;
    const inflight = new Map();  // seq -> request, for the replies not yet drawn
    const MAX_INFLIGHT = 2;  // two keep the daemon busy across the round trip (60 fps needs it); more only adds lag
    const place = () => {
      box = video.getBoundingClientRect();
      Object.assign(canvas.style, { position: 'absolute', left: video.offsetLeft + 'px', top: video.offsetTop + 'px',
        width: video.offsetWidth + 'px', height: video.offsetHeight + 'px', objectFit: getComputedStyle(video).objectFit, pointerEvents: 'none' });
    };
    place(); video.after(canvas);
    const ws = new WebSocket(DAEMON);
    ws.binaryType = 'arraybuffer';
    const me = active = { video, canvas, ws, ro: new ResizeObserver(place), timer: 0, vfc: 0 };
    me.ro.observe(video);
    const fail = why => { if (active !== me) return; log('failed:', why); stop(); badge('SR failed'); };
    me.timer = setTimeout(() => fail('daemon not reachable within 1.5 s'), 1500);

    // p50/p95 of a window array, mean for the cheap ones
    const q = (a, p) => a.length ? +[...a].sort((x, y) => x - y)[Math.min(a.length - 1, Math.floor(a.length * p))].toFixed(1) : null;
    const mean = a => a.length ? +(a.reduce((x, y) => x + y, 0) / a.length).toFixed(1) : null;
    const tick = () => {
      place();  // follows the video if the page moves it
      const s = (performance.now() - win.t) / 1000;
      const stats = { ...st, in: [video.videoWidth, video.videoHeight], out: [canvas.width, canvas.height], fpsIn: +(win.fin / s).toFixed(1), fpsOut: +(win.recv / s).toFixed(1),
        rttP50: q(win.rtt, .5), rttP95: q(win.rtt, .95), copyMs: mean(win.copy), drawMs: mean(win.draw), lagMsP50: q(win.lag, .5) };
      Object.assign(win, { t: performance.now(), fin: 0, recv: 0, rtt: [], copy: [], draw: [], lag: [] });
      for (const [k, p] of inflight) if (performance.now() - p.t0 > 3000) { inflight.delete(k); st.dropped++; }  // lost reply: never wedge the loop
      if (debug) console.log('[nvvsr] stats', Object.entries(stats).map(([k, v]) => k + '=' + v).join(' '));
      report({ stats });
    };

    const onFrame = async (_, meta) => {
      if (active !== me) return;
      me.vfc = video.requestVideoFrameCallback(onFrame);
      win.fin++;
      if (copying || inflight.size >= MAX_INFLIGHT) { st.dropped++; return; }  // skip this frame
      copying = true;
      let f;
      try {
        f = new VideoFrame(video);
        const w = f.visibleRect.width, h = f.visibleRect.height, dim = fit(w, h, box);
        canvas.style.visibility = dim ? '' : 'hidden';
        if (!dim) return;
        if (canvas.width !== dim[0] || canvas.height !== dim[1]) [canvas.width, canvas.height] = dim;
        if (f.format !== 'NV12' && f.format !== 'I420') throw new Error('unsupported frame format ' + f.format);
        if (!buf || buf.byteLength !== 16 + w * h * 3 / 2) buf = new ArrayBuffer(16 + w * h * 3 / 2);
        const t = performance.now();
        st.fmt = f.format;
        await f.copyTo(new Uint8Array(buf, 16));  // NV12 (VA-API / most decoders) goes out as it is, flag bit 1: the daemon converts
        win.copy.push(performance.now() - t);
        if (active !== me) return;
        seq = (seq + 1) & 0xffff;
        // the daemon converts in and out with the same matrix, so the reply keeps the source's colour tag
        inflight.set(seq, { mt: meta.mediaTime, us: Math.round(meta.mediaTime * 1e6), t0: performance.now(), cs: f.colorSpace.toJSON() });
        const d = new DataView(buf);
        d.setUint32(0, 0x5253564E, true);  // "NVSR"
        d.setUint16(4, w, true); d.setUint16(6, h, true); d.setUint16(8, dim[0], true); d.setUint16(10, dim[1], true);
        d.setUint8(12, 0); d.setUint8(13, (f.colorSpace.fullRange ? 1 : 0) | (f.format === 'NV12' ? 2 : 0)); d.setUint16(14, seq, true);
        ws.send(buf);  // the browser copies the bytes: the buffer can be reused for the next frame at once
        st.sent++;
      } catch (e) { fail(e.name + ': ' + e.message); } finally { f?.close(); copying = false; }
    };

    ws.onopen = () => {
      opened = true;
      clearInterval(me.timer);
      me.timer = setInterval(tick, 2000);
      me.vfc = video.requestVideoFrameCallback(onFrame);
      badge('SR'); log('on', video.videoWidth + 'x' + video.videoHeight);
    };
    ws.onclose = () => fail(opened ? 'connection closed' : 'daemon not reachable');
    ws.onmessage = e => {
      try {
        if (typeof e.data === 'string') {  // "DROP <seq>" frees that request; "ERR <reason>" names none, so every request is freed
          if (e.data.startsWith('DROP')) { st.daemonDrop++; inflight.delete(+e.data.split(' ')[1]); }
          else { if (!st.daemonErr++) log('daemon said:', e.data); inflight.clear(); }
          return;
        }
        const d = new DataView(e.data), sq = d.getUint16(8, true), p = d.getUint32(0, true) === 0x4F53564E && inflight.get(sq);  // "NVSO": the reply to one of our requests
        if (!p) return;
        inflight.delete(sq);
        const t = performance.now();
        const vf = new VideoFrame(new Uint8Array(e.data, 16), { format: 'I420', codedWidth: d.getUint16(4, true), codedHeight: d.getUint16(6, true), timestamp: p.us, colorSpace: p.cs });
        try { ctx.drawImage(vf, 0, 0, canvas.width, canvas.height); } finally { vf.close(); }
        const t1 = performance.now();
        st.recv++; win.recv++; win.draw.push(t1 - t); win.rtt.push(t1 - p.t0); win.lag.push((video.currentTime - p.mt) * 1000);
      } catch (err) { fail(err.name + ': ' + err.message); }
    };
  }

  const onFullscreen = async () => {
    stop();
    const mine = gen, root = document.fullscreenElement, video = root && biggestVideo(root);
    if (!video || off) return;
    await new Promise(r => setTimeout(r, 300));  // players resize the video after the event: judge the fullscreen box, not the old one
    if (mine === gen) start(video, mine);
  };
  document.addEventListener('fullscreenchange', onFullscreen);
  addEventListener('keydown', e => { if (e.altKey && e.code === 'KeyS') { off = !off; stop(); if (!off) onFullscreen(); badge(off ? 'SR off' : 'SR'); } });
  report({ loaded: true });
})();
