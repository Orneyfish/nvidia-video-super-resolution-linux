// Page side of the test harness: window.H.<fn>(args) functions that exercise the upscaler and return plain JSON.
// suites.js strings them together (run.sh opens harness.html?suites=...); results and PNG captures go to server.cpp via POST.
import { VideoUpscaler } from '../src/upscaler.js';

const video = document.getElementById('v'), canvas = document.getElementById('c');
const sleep = ms => new Promise(r => setTimeout(r, ms));
const median = a => { const s = [...a].sort((x, y) => x - y); return s.length ? s[s.length >> 1] : 0; };
const pct = (a, p) => { const s = [...a].sort((x, y) => x - y); return s.length ? s[Math.min(s.length - 1, Math.floor(s.length * p))] : 0; };
const round = (x, n = 3) => +x.toFixed(n);

// name -> [mode, params]
export const MODESETS = {
  bilinear: ['bilinear', {}],
  fsr: ['fsr', { sharpness: 0.2 }],
  fsr_easu: ['fsr', { sharpness: false }],
  cnn_S: ['cnn', { cnnSize: 'S', cnnRestore: 'none' }],
  cnn_M: ['cnn', { cnnSize: 'M', cnnRestore: 'none' }],
  cnn_S_rS: ['cnn', { cnnSize: 'S', cnnRestore: 'S' }],
  cnn_M_rM: ['cnn', { cnnSize: 'M', cnnRestore: 'M' }],
};

const GPU = new URLSearchParams(location.search).get('gpu') || 'high-performance';   // adapter powerPreference for every upscaler
let up = null, upReadback = null;

function waitEvent(el, ev, ms = 20000) {
  return new Promise((res, rej) => {
    const t = setTimeout(() => { el.removeEventListener(ev, ok); el.removeEventListener('error', bad); rej(new Error('timeout waiting for ' + ev)); }, ms);
    const ok = () => { clearTimeout(t); el.removeEventListener('error', bad); res(); };
    const bad = () => { clearTimeout(t); el.removeEventListener(ev, ok); rej(new Error('video error ' + (el.error && el.error.message))); };
    el.addEventListener(ev, ok, { once: true }); el.addEventListener('error', bad, { once: true });
  });
}

async function loadClip(url, cross = null) {
  video.pause();
  if (cross) video.crossOrigin = cross; else video.removeAttribute('crossorigin');
  video.loop = false; video.muted = true;
  video.src = url; video.load();
  await waitEvent(video, 'loadeddata');
  return { w: video.videoWidth, h: video.videoHeight, duration: video.duration };
}

async function seek(t) {
  const frame = new Promise(r => { const id = video.requestVideoFrameCallback((n, m) => r(m)); setTimeout(() => r(null), 3000); });
  const done = waitEvent(video, 'seeked');
  video.currentTime = t;
  await done;
  const m = await frame;
  await sleep(30);
  return m;
}

async function getUp(readback = false, opts = {}) {
  if (up && upReadback === readback && !opts.fresh) return up;
  if (up) { up.destroy(); up = null; }
  up = await VideoUpscaler.create(video, canvas, { readback, powerPreference: GPU, ...opts });
  upReadback = readback;
  return up;
}

async function setModeByName(name) {
  const [mode, params] = MODESETS[name];
  await up.setMode(mode, params);
}

function fmtErr(e) { return { name: e && e.name, code: e && e.code, message: String(e && e.message || e) }; }

async function post(name, body, type) {
  const r = await fetch('/post?name=' + encodeURIComponent(name), { method: 'POST', body, headers: type ? { 'content-type': type } : {} });
  if (!r.ok) throw new Error('post failed ' + r.status);
}

async function postPNG(name, cap) {
  const oc = new OffscreenCanvas(cap.width, cap.height);
  oc.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(cap.data.buffer), cap.width, cap.height), 0, 0);
  await post(name, await oc.convertToBlob({ type: 'image/png' }), 'image/png');
}

const H = {
  MODESETS,

  async probe() {
    const out = { gpuPreference: GPU, isSecureContext, origin: location.origin, devicePixelRatio, innerWidth, innerHeight, userAgent: navigator.userAgent, hasGpu: !!navigator.gpu };
    out.apis = { VideoFrame: typeof VideoFrame, MediaStreamTrackProcessor: typeof MediaStreamTrackProcessor, OffscreenCanvas: typeof OffscreenCanvas,
      requestPictureInPicture: typeof video.requestPictureInPicture, documentPictureInPicture: typeof documentPictureInPicture, requestVideoFrameCallback: typeof video.requestVideoFrameCallback };
    if (!navigator.gpu) return out;
    out.preferredCanvasFormat = navigator.gpu.getPreferredCanvasFormat();
    out.wgslLanguageFeatures = [...navigator.gpu.wgslLanguageFeatures];
    for (const pp of ['high-performance', 'low-power', undefined]) {
      let a = null; for (let i = 0; i < 6 && !a; i++) { a = await navigator.gpu.requestAdapter(pp ? { powerPreference: pp } : {}); if (!a) await sleep(500); }
      const info = a ? a.info : null;
      out[pp || 'default'] = a ? { vendor: info.vendor, architecture: info.architecture, device: info.device, description: info.description,
        isFallback: a.isFallbackAdapter, features: [...a.features], limits: { maxTextureDimension2D: a.limits.maxTextureDimension2D,
          maxBufferSize: a.limits.maxBufferSize, maxUniformBufferBindingSize: a.limits.maxUniformBufferBindingSize,
          maxColorAttachmentBytesPerSample: a.limits.maxColorAttachmentBytesPerSample } } : null;
    }
    return out;
  },

  /** GPU time per frame for each (clip, mode): timestamp queries around the whole pass chain on a paused video frame. */
  async cost({ clips, modes, iterations = 60, batch = 4, warmup = 30 }) {
    const res = [];
    for (const [i, url] of clips.entries()) {
      const info = await loadClip(url);
      await seek(Math.min(1.0, info.duration / 2));
      if (!up) await getUp(false);
      up.setSize(2560, 1440);      // fixed 1440p target, independent of the page layout
      for (const m of modes) {
        try {
          await setModeByName(m);
          const ms = await up.benchmark({ iterations, batch, warmup });
          const st = up.stats();
          res.push({ clip: url.split('/').pop(), src: st.source, dst: st.target, mode: m, steps: up._plan.steps.map(s => s.label).join(' > '),
            median: round(median(ms)), p95: round(pct(ms, 0.95)), min: round(Math.min(...ms)), max: round(Math.max(...ms)), n: ms.length });
        } catch (e) { res.push({ clip: url.split('/').pop(), mode: m, error: fmtErr(e) }); }
      }
    }
    return res;
  },

  /** Whole pipeline in the page: play the clip for `seconds` with the upscaler following rVFC. */
  async pipeline({ clip, mode, seconds = 12, width = 2560, height = 1440, maxInFlight = 2 }) {
    await loadClip(clip);
    await getUp(false, { fresh: true, maxInFlight, width, height });
    await setModeByName(mode);
    await seek(0);
    const cad = []; let last = 0, pf0 = -1, pf1 = 0, stop = false;
    const cb = (now, m) => { if (stop) return; if (last) cad.push(now - last); last = now; if (pf0 < 0) pf0 = m.presentedFrames; pf1 = m.presentedFrames; video.requestVideoFrameCallback(cb); };
    video.requestVideoFrameCallback(cb);
    up.resetStats(); up.start();
    const t0 = performance.now();
    await video.play();
    await sleep(seconds * 1000);
    const wall = (performance.now() - t0) / 1000;
    const st = up.stats();
    video.pause(); up.stop(); stop = true;
    // video.getVideoPlaybackQuality().droppedVideoFrames is not reported: with an opaque canvas over the video Chrome counts the occluded video's frames as dropped
    return { clip: clip.split('/').pop(), mode, wall: round(wall, 2), videoTime: round(video.currentTime, 2), stats: st,
      cadence: { n: cad.length, meanMs: round(cad.length ? cad.reduce((a, b) => a + b, 0) / cad.length : 0), p95Ms: round(pct(cad, 0.95)), maxMs: round(Math.max(0, ...cad)),
        presentedFrames: pf1 - pf0 + 1, presentedFps: round((pf1 - pf0 + 1) / Math.max(0.001, wall), 1) } };
  },

  /** Quality: seek to each reference frame, run the mode, read the canvas back, POST it as PNG. */
  async quality({ content, lrUrl, frames, fps = 30, modes, width, height }) {
    const info = await loadClip(lrUrl);
    await getUp(true, { fresh: false, width, height });
    up.setSize(width, height);
    const out = [];
    for (const m of modes) {
      await setModeByName(m);
      for (const k of frames) {
        const meta = await seek((k + 0.5) / fps);
        const cap = await up.renderOnce({ capture: true });
        await postPNG(`q_${content}_${m}_${k}.png`, cap);
        out.push({ content, mode: m, frame: k, mediaTime: meta ? round(meta.mediaTime, 4) : null, expected: round(k / fps, 4), w: cap.width, h: cap.height });
      }
    }
    return { src: [info.w, info.h], frames: out };
  },

  /** Colour correctness: read the canvas back and sample the centre of each patch. */
  async colour({ url, patchesTxt, mode = 'bilinear', width = 1280, height = 720 }) {
    await loadClip(url);
    await getUp(true, { fresh: false, width, height });
    up.setSize(width, height);
    await setModeByName(mode);
    await seek(0.5);
    const cap = await up.renderOnce({ capture: true });
    const exp = patchesTxt.trim().split('\n').map(l => l.split(',').map(Number));
    const pw = cap.width / 4, ph = cap.height / 4, rows = [];
    for (let i = 0; i < exp.length; i++) {
      const cx = Math.floor((i % 4) * pw + pw / 2), cy = Math.floor(Math.floor(i / 4) * ph + ph / 2);
      const o = (cy * cap.width + cx) * 4, got = [cap.data[o], cap.data[o + 1], cap.data[o + 2]];
      rows.push({ expected: exp[i], got, maxErr: Math.max(...got.map((g, j) => Math.abs(g - exp[i][j]))) });
    }
    return { url: url.split('/').pop(), mode, videoSize: [video.videoWidth, video.videoHeight], maxErr: Math.max(...rows.map(r => r.maxErr)),
      meanErr: round(rows.reduce((s, r) => s + r.maxErr, 0) / rows.length, 2), rows };
  },

  /** Does the upscaler accept this video? (cross-origin / DRM tests). Resolves with the typed error code. */
  async tryCreate({ url, cross, mode = 'fsr', play = true }) {
    const res = { url, cross };
    try {
      const info = await loadClip(url, cross);
      res.loaded = true; res.size = [info.w, info.h];
    } catch (e) { res.loaded = false; res.loadError = fmtErr(e); return res; }
    try {
      if (up) { up.destroy(); up = null; }
      let asyncErr = null;
      up = await VideoUpscaler.create(video, canvas, { mode, readback: true, powerPreference: GPU, width: 1280, height: 720, onError: e => { asyncErr = e; } });
      up.start();
      if (play) { await video.play(); await sleep(800); video.pause(); }
      await sleep(200);
      if (asyncErr) throw asyncErr;
      const cap = await up.renderOnce({ capture: true });
      let sum = 0; for (let i = 0; i < cap.data.length; i += 4 * 997) sum += cap.data[i] + cap.data[i + 1] + cap.data[i + 2];
      res.ok = true; res.nonBlack = sum > 1000; res.stats = up.stats();
    } catch (e) { res.ok = false; res.error = fmtErr(e); }
    return res;
  },

  /** Rotated clip (display matrix): corner colours of the WebGPU canvas vs. the same corners of a 2D-canvas drawImage(video), which is what the browser shows. */
  async rotated({ url }) {
    const info = await loadClip(url);
    await getUp(true, { fresh: false });
    up.setSize(info.w * 2, info.h * 2);
    await setModeByName('bilinear');
    await seek(0.5);
    const cap = await up.renderOnce({ capture: true });
    const oc = new OffscreenCanvas(cap.width, cap.height), cx = oc.getContext('2d'); cx.drawImage(video, 0, 0, cap.width, cap.height);
    const ref = cx.getImageData(0, 0, cap.width, cap.height).data;
    const px = (d, x, y) => { const o = (y * cap.width + x) * 4; return [d[o], d[o + 1], d[o + 2]]; };
    const q = 0.125, pts = { tl: [q, q], tr: [1 - q, q], bl: [q, 1 - q], br: [1 - q, 1 - q] }, corners = {};
    for (const [k, [fx, fy]] of Object.entries(pts)) { const x = Math.floor(cap.width * fx), y = Math.floor(cap.height * fy); corners[k] = { webgpu: px(cap.data, x, y), drawImage: px(ref, x, y) }; }
    return { videoSize: [info.w, info.h], canvas: [cap.width, cap.height], corners };
  },

  /** A video + upscaler inside an <iframe> (the page at `src` reports through postMessage). */
  async iframe({ src, allow = '' }) {
    const f = document.createElement('iframe'); f.style.cssText = 'position:absolute;left:0;top:0;width:640px;height:360px;border:0'; if (allow) f.allow = allow;
    const done = new Promise(res => { const t = setTimeout(() => res({ timeout: true }), 20000); addEventListener('message', e => { if (e.source === f.contentWindow) { clearTimeout(t); res(e.data); } }, { once: true }); });
    f.src = src; document.body.appendChild(f);
    const r = await done; f.remove(); return r;
  },

  /** Lay the video + canvas out in some CSS scene for screenshot comparison. */
  async scene({ url, mode = 'bilinear', css = {}, boxCss = '', fit = 'contain', w = 640, h = 360 }) {
    const info = await loadClip(url);
    const box = document.getElementById('box');
    box.style.cssText = `position:absolute;left:0;top:0;width:${w}px;height:${h}px;${boxCss}`;
    for (const el of [video, canvas]) { el.style.cssText = `position:absolute;left:0;top:0;width:${w}px;height:${h}px;object-fit:${fit};` + (css.el || ''); }
    await getUp(false, { fresh: true });
    up.setSize();
    await setModeByName(mode);
    await seek(1.0);
    await up.renderOnce();
    await sleep(100);
    return { src: [info.w, info.h], target: [canvas.width, canvas.height], stats: up.stats() };
  },
  showOnly(which) { video.style.visibility = which !== 'canvas' ? 'visible' : 'hidden'; canvas.style.visibility = which !== 'video' ? 'visible' : 'hidden'; },   // 'video' | 'canvas' | 'both'
  resetLayout() {
    const box = document.getElementById('box'); box.style.cssText = ''; for (const el of [video, canvas]) { el.style.cssText = ''; }
    video.style.visibility = canvas.style.visibility = 'visible';
  },

  /** Native video fullscreen vs. container fullscreen (call from inside a user-gesture handler; suites.js takes the screenshot). */
  async fullscreen({ target }) {
    const el = target === 'video' ? video : document.getElementById('box');
    await el.requestFullscreen();
    await sleep(500);
    return { fullscreenElement: document.fullscreenElement && document.fullscreenElement.tagName + '#' + document.fullscreenElement.id };
  },
  async exitFullscreen() { if (document.fullscreenElement) await document.exitFullscreen(); await sleep(300); },

  async pip() {
    const r = {};
    try {
      const pw = await video.requestPictureInPicture();
      r.ok = true; r.size = [pw.width, pw.height];
      await sleep(300);
      r.upscalerKeepsRunning = up ? !up.error : null;
      await document.exitPictureInPicture();
    } catch (e) { r.ok = false; r.error = fmtErr(e); }
    return r;
  },

  /** Clear-key (EME) clip: does importExternalTexture work for a MediaKeys-protected video? */
  async eme({ url, mime, kid, key }) {
    const r = { events: [] }, v = document.createElement('video'), c = document.createElement('canvas');   // own elements: a failed MSE/EME load must not poison the shared <video>
    v.muted = true; v.style.cssText = c.style.cssText = 'position:absolute;left:0;top:0;width:320px;height:180px;';
    c.style.visibility = 'hidden'; document.body.append(v, c);
    for (const ev of ['encrypted', 'waitingforkey', 'loadedmetadata', 'loadeddata', 'canplay', 'stalled', 'error']) v.addEventListener(ev, () => r.events.push(ev));
    const b64u = hex => btoa(String.fromCharCode(...hex.match(/../g).map(h => parseInt(h, 16)))).replace(/\+/g, '-').replace(/\//g, '_').replace(/=+$/, '');
    let ms = null, upE = null;
    try {
      const access = await navigator.requestMediaKeySystemAccess('org.w3.clearkey', [{ initDataTypes: ['keyids', 'cenc'], videoCapabilities: [{ contentType: mime, encryptionScheme: 'cenc' }, { contentType: mime }] }]);
      const keys = await access.createMediaKeys();
      await v.setMediaKeys(keys);
      ms = new MediaSource(); v.src = URL.createObjectURL(ms);
      await new Promise(res => ms.addEventListener('sourceopen', res, { once: true }));
      const sb = ms.addSourceBuffer(mime);
      v.addEventListener('encrypted', async ev => {
        const s = keys.createSession('temporary');
        s.addEventListener('message', async () => {
          r.events.push('license-request');
          await s.update(new TextEncoder().encode(JSON.stringify({ keys: [{ kty: 'oct', kid: b64u(kid), k: b64u(key) }], type: 'temporary' })));
        });
        await s.generateRequest(ev.initDataType, ev.initData);
      });
      sb.appendBuffer(await (await fetch(url)).arrayBuffer());
      await new Promise((res, rej) => { sb.addEventListener('updateend', res, { once: true }); sb.addEventListener('error', () => rej(new Error('SourceBuffer error')), { once: true }); });
      ms.endOfStream();
      await waitEvent(v, 'loadeddata', 8000);
      r.loaded = true; r.mediaKeys = !!v.mediaKeys; r.size = [v.videoWidth, v.videoHeight];
      await v.play(); await sleep(500);
      let asyncErr = null;
      upE = await VideoUpscaler.create(v, c, { mode: 'fsr', readback: true, powerPreference: GPU, width: 1280, height: 720, onError: e => { asyncErr = e; } });
      upE.start(); await sleep(600);
      if (asyncErr) throw asyncErr;
      const cap = await upE.renderOnce({ capture: true });
      let sum = 0; for (let i = 0; i < cap.data.length; i += 4 * 997) sum += cap.data[i] + cap.data[i + 1] + cap.data[i + 2];
      r.ok = true; r.nonBlack = sum > 1000; r.meanLevel = round(sum / (cap.data.length / 4 / 997) / 3, 1);
    } catch (e) { r.ok = false; r.error = fmtErr(e); }
    r.state = { videoReady: v.readyState, videoError: v.error && v.error.message, mediaSource: ms && ms.readyState };
    upE?.destroy(); v.remove(); c.remove();
    return r;
  },

  /** Sustained present cost: a WebGPU canvas of the given size redrawn every rAF with a trivial pass (no video). */
  async present({ width, height, seconds = 8 }) {
    const c2 = document.createElement('canvas'); c2.width = width; c2.height = height;
    c2.style.cssText = 'position:absolute;left:0;top:0;width:1280px;height:720px;z-index:5';
    document.body.appendChild(c2);
    const a = await navigator.gpu.requestAdapter({ powerPreference: GPU });
    const dev = await a.requestDevice();
    const ctx = c2.getContext('webgpu'); ctx.configure({ device: dev, format: navigator.gpu.getPreferredCanvasFormat(), alphaMode: 'opaque' });
    const dts = []; let last = 0, n = 0; const end = performance.now() + seconds * 1000;
    await new Promise(res => {
      const f = now => {
        if (last) dts.push(now - last); last = now; n++;
        const enc = dev.createCommandEncoder();
        const pass = enc.beginRenderPass({ colorAttachments: [{ view: ctx.getCurrentTexture().createView(), loadOp: 'clear', storeOp: 'store', clearValue: { r: (n % 60) / 60, g: 0.3, b: 0.5, a: 1 } }] });
        pass.end(); dev.queue.submit([enc.finish()]);
        if (performance.now() < end) requestAnimationFrame(f); else res();
      };
      requestAnimationFrame(f);
    });
    dev.destroy(); c2.remove();
    return { width, height, frames: n, fps: round(n / seconds, 1), meanMs: round(dts.reduce((s, v) => s + v, 0) / dts.length), p95Ms: round(pct(dts, 0.95)), maxMs: round(Math.max(...dts)) };
  },

  /** Plain video playback with no upscaler (baseline: rVFC callbacks per second; CPU and GPU numbers come from server.cpp's monitor). */
  async plainPlay({ clip, seconds = 12 }) {
    await loadClip(clip);
    if (up) { up.destroy(); up = null; }
    await seek(0);
    let n = 0, stop = false;
    const cb = () => { if (stop) return; n++; video.requestVideoFrameCallback(cb); };
    video.requestVideoFrameCallback(cb);
    await video.play(); await sleep(seconds * 1000); video.pause(); stop = true;
    return { clip: clip.split('/').pop(), rvfcFps: round(n / seconds, 1) };
  },

  /** Self-check of the CNN port: a CPU float32 evaluation of the Anime4K-S passes (straight from the weight module) must match the GPU output. */
  async selfcheck({ clip, minPsnr = 45 }) {
    const model = (await import('../src/models/anime4k_upscale_s.js')).default;
    await loadClip(clip); await getUp(true, { fresh: false }); await seek(1.0);
    const W = video.videoWidth, Hh = video.videoHeight;
    up.setSize(W, Hh); await setModeByName('bilinear'); const src = await up.renderOnce({ capture: true });   // the source pixels exactly as the GPU sees them
    up.setSize(W * 2, Hh * 2); await setModeByName('cnn_S'); const gpu = await up.renderOnce({ capture: true });
    const main = new Float32Array(W * Hh * 4); for (let i = 0; i < main.length; i++) main[i] = i % 4 === 3 ? 1 : src.data[i] / 255;
    const feat = new Map([['MAIN', main]]);
    const at = (t, x, y, c) => t[(Math.min(Hh - 1, Math.max(0, y)) * W + Math.min(W - 1, Math.max(0, x))) * 4 + c];
    for (const p of model.passes) {
      if (p.post === 'd2s') break;
      const out = new Float32Array(W * Hh * 4);
      for (let y = 0; y < Hh; y++) for (let x = 0; x < W; x++) {
        const acc = [...p.bias];
        p.taps.forEach(([g, dx, dy], i) => {
          const [tn, sign] = p.defs[g], t = feat.get(tn);
          for (let c = 0; c < 4; c++) {
            let v = at(t, x + dx, y + dy, c); v = sign === 0 ? v : sign > 0 ? Math.max(v, 0) : Math.max(-v, 0);
            for (let r = 0; r < 4; r++) acc[r] += p.w[i * 16 + c * 4 + r] * v;       // column-major mat4: column c holds the weights of input channel c
          }
        });
        out.set(acc, (y * W + x) * 4);
      }
      feat.set(p.save, out);
    }
    const last = feat.get('conv2d_last_tf'), W2 = W * 2; let se = 0, n = 0;
    for (let iy = 0; iy < Hh * 2; iy++) for (let ix = 0; ix < W2; ix++) {
      const sx = (ix + 0.5) / 2 - 0.5, sy = (iy + 0.5) / 2 - 0.5, x0 = Math.floor(sx), y0 = Math.floor(sy), fx = sx - x0, fy = sy - y0;
      const res = last[((iy >> 1) * W + (ix >> 1)) * 4 + (iy & 1) * 2 + (ix & 1)];
      for (let c = 0; c < 3; c++) {
        const base = (1 - fy) * ((1 - fx) * at(main, x0, y0, c) + fx * at(main, x0 + 1, y0, c)) + fy * ((1 - fx) * at(main, x0, y0 + 1, c) + fx * at(main, x0 + 1, y0 + 1, c));
        const cpu = Math.min(255, Math.max(0, Math.round((base + res) * 255))), d = cpu - gpu.data[(iy * W2 + ix) * 4 + c]; se += d * d; n++;
      }
    }
    const psnr = se === 0 ? 99 : 10 * Math.log10(255 * 255 / (se / n));
    return { size: [W, Hh], psnr: round(psnr, 2), minPsnr, pass: psnr >= minPsnr };
  },

  stats() { return up ? up.stats() : null; },
};

H.sleep = sleep; H.post = post; H.fmtErr = fmtErr; H.round = round;
window.H = H;
