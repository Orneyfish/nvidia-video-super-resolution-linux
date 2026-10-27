// Suite runner. run.sh opens harness.html?suites=probe,cost,...&gpu=...&config=...&seconds=...&b=PORT_B[&only=STR]
// Every suite posts <suite>-<config>.json and .md to server.cpp; log.txt gets progress lines; the file "done" ends the run.
const H = window.H, Q = new URLSearchParams(location.search);
const CONFIG = Q.get('config') || 'intel', GPU = Q.get('gpu') || 'high-performance', SECONDS = +(Q.get('seconds') || 15), ONLY = Q.get('only') || '';
const B = `http://127.0.0.1:${Q.get('b')}`;       // second origin (no CORS) for the cross-origin tests
const { sleep, round, fmtErr } = H;

const log = m => fetch('/post?name=log.txt&append=1', { method: 'POST', body: m + '\n' });
const save = (suite, json, md) => Promise.all([H.post(`${suite}-${CONFIG}.json`, JSON.stringify(json, null, 1)), H.post(`${suite}-${CONFIG}.md`, md)]);
const get = async u => (await fetch(u)).text();
const mon = async phase => (await fetch('/mon?phase=' + phase)).json();
const md = (head, rows) => ['| ' + head.join(' | ') + ' |', '|' + head.map(() => '---').join('|') + '|', ...rows.map(r => '| ' + r.join(' | ') + ' |')].join('\n') + '\n';
const clip = n => '/media/' + n;
const cpu = m => ['gpu-process', 'renderer', 'browser'].map(k => m.chromeCpuPercentOfOneCore[k] ?? 0).join('/');
const rtx = m => [m.gpu.powerW.avg, m.gpu.clockMHz.median, m.gpu.utilPct.avg, m.gpu.vramMiB.max];
const RTX_HEAD = ['RTX W', 'RTX MHz', 'RTX util%', 'RTX VRAM MiB'];
const shot = async n => { const r = await fetch('/shot?name=' + n); if (!r.ok) throw new Error('screenshot failed: ' + await r.text()); };

/** Run fn inside a real keydown (user activation) typed into the headless cage by server.cpp's /key. */
function withGesture(fn) {
  return new Promise(res => {
    const h = async () => { try { res(await fn()); } catch (e) { res({ error: fmtErr(e) }); } };
    addEventListener('keydown', h, { once: true });
    fetch('/key');
    setTimeout(() => { removeEventListener('keydown', h); res({ error: 'no keydown within 5 s: wtype could not reach cage' }); }, 5000);
  });
}

const S = {
  async probe() {
    const probe = await H.probe();
    const a = await navigator.gpu.requestAdapter({ powerPreference: GPU }); window.__held = await a?.requestDevice();     // hold a device so nvidia-smi lists Chrome
    const smi = await get('/smi');
    const rows = ['high-performance', 'low-power', 'default'].map(k => { const x = probe[k]; return x ? [k, x.vendor, x.architecture, x.description || '', x.features.includes('timestamp-query'), x.limits.maxTextureDimension2D] : [k, 'none']; });
    return [{ probe, nvidiaSmi: smi }, md(['requestAdapter', 'vendor', 'architecture', 'description', 'timestamp-query', 'maxTexture2D'], rows)];
  },

  async selfcheck() {
    const r = await H.selfcheck({ clip: clip('mountain_lr.mp4') });
    if (!r.pass) throw new Error('selfcheck failed: ' + JSON.stringify(r));
    return [r, `CNN-S, GPU output vs CPU float32 reference: ${r.psnr} dB (>= ${r.minPsnr} required), ${r.size.join('x')} source\n`];
  },

  async cost() {
    const clips = ['pixie_480p30.mp4', 'pixie_720p30.mp4', 'pixie_1080p30.mp4'].map(clip);
    const modes = ['bilinear', 'fsr_easu', 'fsr', 'cnn_S', 'cnn_M', 'cnn_S_rS', 'cnn_M_rM'];
    await mon('start'); const res = await H.cost({ clips, modes, iterations: 60, batch: 4, warmup: 30 }); const monitor = await mon('stop');
    return [{ res, monitor }, md(['clip', 'source', 'target', 'mode', 'median ms', 'p95 ms', 'min', 'max', 'passes'],
      res.map(r => r.error ? [r.clip, '', '', r.mode, 'ERR ' + r.error.message.slice(0, 60), '', '', '', ''] : [r.clip, r.src.join('x'), r.dst.join('x'), r.mode, r.median, r.p95, r.min, r.max, r.steps.split(' > ').length])) ];
  },

  async pipeline() {
    const plan = [['pixie_720p30.mp4', ['bilinear', 'fsr', 'cnn_S', 'cnn_M']], ['pixie_1080p30.mp4', ['bilinear', 'fsr', 'cnn_S']], ['pixie_480p30.mp4', ['fsr', 'cnn_S']],
      ['pixie_720p60.mp4', ['bilinear', 'fsr', 'cnn_S', 'cnn_M']], ['pixie_1080p60.mp4', ['bilinear', 'fsr']]];
    const runs = [];
    for (const [c, modes] of plan) for (const m of modes) {
      if (ONLY && !c.includes(ONLY)) continue;
      await sleep(1500); await mon('start');
      const r = await H.pipeline({ clip: clip(c), mode: m, seconds: SECONDS }); r.monitor = await mon('stop'); runs.push(r);
      await log(`pipeline ${c} ${m}: ${r.stats.fps} fps, busy drops ${r.stats.droppedBusy}, missed ${r.stats.missed}`);
    }
    for (const c of ['pixie_720p30.mp4', 'pixie_1080p60.mp4']) {       // baseline: plain playback, no upscaler
      if (ONLY && !c.includes(ONLY)) continue;
      await sleep(1500); await mon('start');
      const r = await H.plainPlay({ clip: clip(c), seconds: SECONDS }); runs.push({ clip: r.clip, mode: 'plain-video', plain: r, monitor: await mon('stop') });
    }
    return [runs, md(['clip', 'mode', 'frames/s rendered', 'video frames/s presented', 'busy drops', 'missed', 'rVFC p95 ms', 'cpu ms med', 'gpu ms med', 'gpu ms p95', 'Chrome CPU% gpu/renderer/browser', ...RTX_HEAD],
      runs.map(r => r.stats ? [r.clip, r.mode, r.stats.fps, r.cadence.presentedFps, r.stats.droppedBusy, r.stats.missed, r.cadence.p95Ms, r.stats.cpuMs.median, r.stats.gpuMs?.median, r.stats.gpuMs?.p95, cpu(r.monitor), ...rtx(r.monitor)]
        : [r.clip, r.mode, r.plain.rvfcFps + ' (rVFC)', '-', '-', '-', '-', '-', '-', '-', cpu(r.monitor), ...rtx(r.monitor)]))];
  },

  /** Captures only; run.sh turns them into PSNR/SSIM tables with metrics.sh (ffmpeg psnr / ssim filters). */
  async quality() {
    const contents = [['maldives', 2560, 1440], ['pixie', 2560, 1440], ['lady', 2560, 1440], ['space', 2560, 1440], ['north', 2560, 1440], ['mountain', 1280, 720]];
    const modes = ['bilinear', 'fsr_easu', 'fsr', 'cnn_S', 'cnn_M', 'cnn_S_rS', 'cnn_M_rM'];
    const frames = [10, 70, 130, 190, 250, 290], out = [], labels = [];
    for (const variant of ['lr', 'lrq', 'lr133', 'lr300']) for (const [c, w, h] of contents) {   // lr: clean box 2x; lrq: bicubic + CRF 27 (2x); lr133 / lr300: 1080p / 480p sources
      if (c === 'mountain' && (variant === 'lr133' || variant === 'lr300')) continue;
      labels.push(`${c}.${variant}`);
      out.push(await H.quality({ content: `${c}.${variant}`, lrUrl: clip(`${c}_${variant}.mp4`), frames, fps: 30, modes, width: w, height: h }));
      await log(`quality captured ${c}.${variant}`);
    }
    const all = out.flatMap(o => o.frames), bad = all.filter(f => Math.abs(f.mediaTime - f.expected) > 0.002);
    await H.post('quality-spec.txt', `CONTENTS="${labels.join(' ')}"\nMODES="${modes.join(' ')}"\nFRAMES="${frames.join(' ')}"\n`);   // sourced by metrics.sh
    return [{ capture: out, frameMismatches: bad.length }, `${all.length} frames captured, ${bad.length} with an rVFC mediaTime more than 2 ms away from the requested frame\n`];
  },

  async present() {
    const rows = [];
    for (const [w, h] of [[640, 360], [1280, 720], [2560, 1440], [2560, 1440]]) {
      await sleep(1500); await mon('start'); const r = await H.present({ width: w, height: h, seconds: 8 }); r.monitor = await mon('stop'); rows.push(r);
    }
    return [rows, md(['canvas', 'fps', 'frame ms mean', 'p95', 'max', 'Chrome CPU% gpu/renderer/browser', ...RTX_HEAD], rows.map(r => [`${r.width}x${r.height}`, r.fps, r.meanMs, r.p95Ms, r.maxMs, cpu(r.monitor), ...rtx(r.monitor)]))];
  },

  async compat() {
    const R = {}, out = [];
    const note = (title, head, rows) => out.push(`### ${title}\n\n` + md(head, rows));
    const code = c => c.error ? c.error.code || c.error.name : c.loadError ? 'video-load-error' : c.ok ? 'ok' : '?';
    const attempt = async (name, fn) => { try { R[name] = await fn(); } catch (e) { R[name] = { error: fmtErr(e) }; } await log('compat ' + name); };

    await attempt('crossOrigin', async () => {
      const res = [];
      for (const [label, url, cross] of [
        ['same origin', clip('pixie_720p30.mp4'), null],
        ['cross origin, no CORS headers, no crossorigin attr', `${B}/media/pixie_720p30.mp4`, null],
        ['cross origin, ACAO:*, no crossorigin attr', `${B}/cors/media/pixie_720p30.mp4`, null],
        ['cross origin, ACAO:*, crossorigin=anonymous', `${B}/cors/media/pixie_720p30.mp4`, 'anonymous'],
        ['cross origin, no CORS headers, crossorigin=anonymous', `${B}/media/pixie_720p30.mp4`, 'anonymous']]) {
        const r = await H.tryCreate({ url, cross }); res.push({ label, result: code(r), nonBlack: r.nonBlack, detail: (r.error?.message || r.loadError?.message || '').slice(0, 110) });
      }
      note('Cross-origin video', ['case', 'result', 'canvas not black', 'detail'], res.map(r => [r.label, r.result, r.nonBlack ?? '', r.detail]));
      return res;
    });

    await attempt('colour', async () => {
      const patches = await get('/media/colour_patches.txt'), res = [];
      for (const tag of ['bt709', 'bt601', 'bt709pc', 'bt2020']) for (const mode of ['bilinear', 'fsr']) {
        const r = await H.colour({ url: clip(`colour_${tag}.mp4`), patchesTxt: patches, mode });
        res.push({ clip: tag, mode, maxErr: r.maxErr, meanMaxErr: r.meanErr, worst: r.rows.reduce((w, x) => x.maxErr > w.maxErr ? x : w, r.rows[0]) });
      }
      note('Colour (16 flat patches read back from the canvas, error in 8-bit levels vs the RGB the clip was made from)', ['clip tag', 'mode', 'max error', 'mean of per-patch max', 'worst patch (expected -> got)'],
        res.map(r => [r.clip, r.mode, r.maxErr, r.meanMaxErr, `${r.worst.expected} -> ${r.worst.got}`]));
      return res;
    });

    await attempt('rotated', async () => {
      const r = await H.rotated({ url: clip('rot90.mp4') });
      note('Rotated video (display matrix 90)', ['corner', 'WebGPU canvas', 'drawImage(video)'], Object.entries(r.corners).map(([k, v]) => [k, v.webgpu.join(','), v.drawImage.join(',')]));
      return r;
    });

    await attempt('geometry', async () => {
      const res = [], scenes = [
        ['contain, same aspect box', { w: 640, h: 360, fit: 'contain' }], ['contain, 4:3 box (letterbox)', { w: 480, h: 360, fit: 'contain' }],
        ['cover, square box', { w: 360, h: 360, fit: 'cover' }], ['fill, 4:3 box (stretch)', { w: 480, h: 360, fit: 'fill' }],
        ['CSS rotate(10deg) scale(.8) on the container', { w: 640, h: 360, fit: 'contain', boxCss: 'transform:rotate(10deg) scale(0.8);transform-origin:center;' }],
        ['CSS scaleX(-1) on the container', { w: 640, h: 360, fit: 'contain', boxCss: 'transform:scaleX(-1);' }]];
      for (const [i, [label, sc]] of scenes.entries()) {
        H.resetLayout();
        const info = await H.scene({ url: clip('pixie_480p30.mp4'), mode: 'bilinear', ...sc });
        H.showOnly('video'); await sleep(400); await shot(`geo${i}_video`);
        H.showOnly('canvas'); await sleep(400); await shot(`geo${i}_canvas`);
        const psnr = +(await get(`/psnr?a=geo${i}_video&b=geo${i}_canvas`));
        res.push({ label, source: info.src.join('x'), canvasBacking: info.target.join('x'), psnrVideoVsCanvas: round(psnr, 1) });
      }
      H.resetLayout();
      note('Geometry: screenshot with only the <video> visible vs only the canvas visible (same CSS box)', ['scene', 'video', 'canvas backing store', 'PSNR video vs canvas (dB)'], res.map(r => [r.label, r.source, r.canvasBacking, r.psnrVideoVsCanvas]));
      return res;
    });

    await attempt('iframe', async () => {
      const res = [];
      for (const [label, base] of [['same-origin iframe', location.origin], ['cross-origin iframe (other port), no allow=', B], ['iframe on an insecure origin (http://nvvsr.test)', `http://nvvsr.test:${location.port}`]]) {
        const r = await H.iframe({ src: `${base}/test/frame.html?clip=${encodeURIComponent(`${base}/media/pixie_720p30.mp4`)}&gpu=${GPU}` }); res.push({ label, ...r });
      }
      note('Iframes / secure context', ['case', 'isSecureContext', 'navigator.gpu', 'upscaler', 'detail'], res.map(r => [r.label, r.isSecureContext, r.hasGpu, r.ok ? 'ok (' + r.adapter + ')' : r.error?.code || 'timeout', (r.error?.message || '').slice(0, 90)]));
      return res;
    });

    await attempt('eme', async () => {
      const [kid, key] = (await get('/media/clearkey.key')).trim().split(' '), codec = (await get('/media/clearkey.codec')).trim();
      const r = await H.eme({ url: clip('clearkey.mp4'), mime: `video/mp4; codecs="${codec}"`, kid, key });
      note('EME (ClearKey, CENC-encrypted fMP4)', ['loaded', 'video.mediaKeys set', 'upscaler', 'canvas not black', 'detail'], [[r.loaded ?? false, r.mediaKeys ?? '', r.ok ? 'ok' : r.error?.code || r.error?.name, r.nonBlack ?? '', `${(r.error?.message || '').slice(0, 80)} events: ${r.events.join(',')}; ${JSON.stringify(r.state)}`]]);
      return r;
    });

    await attempt('pip', async () => {
      await H.tryCreate({ url: clip('pixie_720p30.mp4'), cross: null, play: false });
      const r = await withGesture(() => H.pip());
      note('Picture-in-picture', ['requestPictureInPicture', 'detail'], [[r.ok ? 'ok' : 'failed', r.error?.message || r.error || (r.size ? r.size.join('x') : '')]]);
      return r;
    });

    await attempt('fullscreen', async () => {
      H.resetLayout();
      await H.scene({ url: clip('pixie_480p30.mp4'), mode: 'bilinear', w: 640, h: 360, fit: 'contain' });
      document.getElementById('c').style.filter = 'hue-rotate(180deg)';      // makes the canvas distinguishable from the video
      const box = '640:360:0:0', mean = async n => (await get(`/mean?name=${n}&box=${box}`)).split(',').map(Number), dist = (a, b) => Math.abs(a[0] - b[0]) + Math.abs(a[2] - b[2]);
      H.showOnly('video'); await sleep(400); await shot('fs_video_only'); const mv = await mean('fs_video_only');
      H.showOnly('canvas'); await sleep(400); await shot('fs_canvas_only'); const mc = await mean('fs_canvas_only');
      H.showOnly('both');
      const res = [];
      for (const target of ['video', 'box']) {
        const r = await withGesture(() => H.fullscreen({ target })); await sleep(500);
        let shows = 'screenshot failed'; try { await shot('fs_' + target); const mf = await mean('fs_' + target); shows = dist(mf, mv) < dist(mf, mc) ? 'original video (canvas hidden)' : 'canvas (upscaled)'; } catch (e) { shows = String(e.message); }
        res.push({ fullscreenElement: target === 'video' ? '<video> element' : 'container <div> with video + canvas', result: r.error ? JSON.stringify(r.error) : r.fullscreenElement, shows });
        await H.exitFullscreen();
      }
      note('Fullscreen (canvas is hue-rotated so it can be told from the video)', ['fullscreen element', 'document.fullscreenElement', 'what is on screen'], res.map(r => [r.fullscreenElement, r.result, r.shows]));
      return res;
    });
    return [R, out.join('\n')];
  },
};

(async () => {
  const names = (Q.get('suites') || '').split(',').filter(Boolean);
  for (const n of names) {
    await log(`suite ${n}`);
    try { const [json, text] = await S[n](); await save(n, json, text); await log(`suite ${n} done`); }
    catch (e) { await log(`suite ${n} FAILED: ${e && e.stack || e}`); }
  }
  await H.post('done', 'done');
})();
