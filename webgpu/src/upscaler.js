// VideoUpscaler: upscale an HTMLVideoElement into a WebGPU <canvas> in real time.
//
//   const up = await VideoUpscaler.create(video, canvas, { mode: 'fsr', onError: e => ... });
//   up.start();            // follows video.requestVideoFrameCallback
//   up.setMode('cnn', { cnnSize: 'M', cnnRestore: 'none' });
//   up.stats();            // fps, drops, cpu/gpu ms ...
//   up.stop();             // or up.destroy()
//
// Modes: 'bilinear' (what the browser does anyway), 'fsr' (AMD FSR 1.0 EASU + RCAS), 'cnn' (Anime4K CNN, size S or M, optional restore pass).
// Everything is plain WebGPU render passes: no compute, no storage textures, no network, no build step.
// FSR: MIT, (c) 2021 AMD (see shaders.js).  Anime4K: MIT, (c) 2019-2021 bloc97 (see cnn.js).
import { VS, FS_EXTERNAL, FS_RESAMPLE, FS_EASU, FS_RCAS } from './shaders.js';
import { planModel, loadModel } from './cnn.js';

/** Typed error so a wrapper can decide to fall back to the plain video.
 *  code: 'unsupported' (no navigator.gpu / insecure context / no rVFC), 'no-adapter', 'device-failed', 'cross-origin'
 *        (importExternalTexture SecurityError: video without CORS), 'import-failed' (DRM, or the GPU cannot share memory with the compositor), 'shader',
 *        'device-lost', 'internal'. */
export class UpscalerError extends Error {
  constructor(code, message, cause) { super(message); this.name = 'UpscalerError'; this.code = code; if (cause) this.cause = cause; }
}

export const MODES = ['bilinear', 'fsr', 'cnn'];
const DEFAULTS = {
  mode: 'fsr',
  sharpness: 0.2,            // FSR RCAS, in stops of reduction (0 = maximum, <0 or false = RCAS off)
  cnnSize: 'S',              // 'S' | 'M'
  cnnRestore: 'none',        // 'none' | 'S' | 'M'   (Anime4K "Restore" before the upscaler)
  maxInFlight: 2,            // frames submitted but not finished; more are dropped, never queued
  maxScale: 4,
  powerPreference: 'high-performance',  // 'low-power' = the iGPU: on a hybrid laptop that is the GPU Chrome composites on (see README)
  readback: false,           // make the canvas texture COPY_SRC so renderOnce({capture:true}) works
};

const median = a => { if (!a.length) return 0; const s = [...a].sort((x, y) => x - y); return s[s.length >> 1]; };
const pct = (a, p) => { if (!a.length) return 0; const s = [...a].sort((x, y) => x - y); return s[Math.min(s.length - 1, Math.floor(s.length * p))]; };
const ring = (arr, v, n = 240) => { arr.push(v); if (arr.length > n) arr.shift(); };

export class VideoUpscaler {
  /** @param {HTMLVideoElement} video @param {HTMLCanvasElement|OffscreenCanvas} canvas @param {object} opts */
  static async create(video, canvas, opts = {}) {
    if (typeof navigator === 'undefined' || !navigator.gpu)
      throw new UpscalerError('unsupported', `navigator.gpu is not available (isSecureContext=${globalThis.isSecureContext})`);
    if (!video || typeof video.requestVideoFrameCallback !== 'function')
      throw new UpscalerError('unsupported', 'HTMLVideoElement.requestVideoFrameCallback is not available');
    const o = { ...DEFAULTS, ...opts };
    let adapter;
    try { adapter = await navigator.gpu.requestAdapter({ powerPreference: o.powerPreference }); }
    catch (e) { throw new UpscalerError('no-adapter', String(e), e); }
    if (!adapter) throw new UpscalerError('no-adapter', 'requestAdapter() returned null');
    const requiredFeatures = [];
    if (adapter.features.has('timestamp-query')) requiredFeatures.push('timestamp-query');
    let device;
    try { device = await adapter.requestDevice({ requiredFeatures, label: 'nvvsr-upscaler' }); }
    catch (e) { throw new UpscalerError('device-failed', String(e), e); }
    const up = new VideoUpscaler(video, canvas, o, adapter, device);
    try {
      up._configure();
      await up._buildPlan();
      await up._probe();
    } catch (e) { up.destroy(); throw e; }
    return up;
  }

  constructor(video, canvas, opts, adapter, device) {
    this.video = video; this.canvas = canvas; this.opts = opts; this.adapter = adapter; this.device = device;
    this.mode = opts.mode; this.modeOpts = { cnnSize: opts.cnnSize, cnnRestore: opts.cnnRestore, sharpness: opts.sharpness };
    this.adapterInfo = adapter.info ? { vendor: adapter.info.vendor, architecture: adapter.info.architecture, device: adapter.info.device,
      description: adapter.info.description, backend: adapter.info.backend ?? undefined } : {};
    this.canvasFormat = navigator.gpu.getPreferredCanvasFormat();
    this.hasTimestamps = device.features.has('timestamp-query');
    this.error = null; this.state = 'idle';
    this._running = false; this._rvfc = 0; this._building = false; this._plan = null; this._pipes = new Map();
    this._inFlight = 0; this._geomDirty = true; this._pendingCapture = null;
    this._ts = [];                                         // timestamp query ring
    this._c = { rendered: 0, dropBusy: 0, dropBuild: 0, presented: 0, firstPresented: -1, completed: 0, rebuilds: 0, errors: 0 };
    this._cpu = []; this._gpu = []; this._t0 = 0;
    this._sampler = device.createSampler({ magFilter: 'linear', minFilter: 'linear', addressModeU: 'clamp-to-edge', addressModeV: 'clamp-to-edge' });
    device.lost.then(info => { if (info.reason !== 'destroyed') this._fail(new UpscalerError('device-lost', info.message || 'GPU device lost')); });
    device.addEventListener('uncapturederror', ev => {
      const m = ev.error?.message || ''; this._c.errors++; this._lastGpuError = m;
      if (/ImportMemory|ExternalTexture/.test(m)) this._fail(new UpscalerError('import-failed', m.split('\n')[0]));
    });
    this._onFrame = this._onFrame.bind(this);
    this._onGeom = () => { this._geomDirty = true; };
    video.addEventListener('resize', this._onGeom);
    video.addEventListener('loadedmetadata', this._onGeom);
    if (typeof ResizeObserver !== 'undefined') { this._ro = new ResizeObserver(this._onGeom); this._ro.observe(video); }
    if (typeof window !== 'undefined') window.addEventListener('resize', this._onGeom);
  }

  // ---------------------------------------------------------------------------------------------------- public API
  start() {
    if (this.error) throw this.error;
    if (this._running) return;
    this._running = true; this.state = 'running'; this._t0 = performance.now();
    this._rvfc = this.video.requestVideoFrameCallback(this._onFrame);
    if (this.video.readyState >= 2) this._renderCurrent();   // show the current frame right away (paused video)
  }

  stop() {
    this._running = false;
    if (this._rvfc) { try { this.video.cancelVideoFrameCallback(this._rvfc); } catch {} this._rvfc = 0; }
    if (this.state === 'running') this.state = 'stopped';
  }

  /** Release everything (also destroys the GPU device we created). */
  destroy() {
    this.stop();
    this.video.removeEventListener('resize', this._onGeom); this.video.removeEventListener('loadedmetadata', this._onGeom);
    this._ro?.disconnect(); if (typeof window !== 'undefined') window.removeEventListener('resize', this._onGeom);
    this._releasePlan(this._plan); this._plan = null;
    try { this.context?.unconfigure(); } catch {}
    try { this.device.destroy(); } catch {}
    if (this.state !== 'failed') this.state = 'destroyed';
  }

  /** Switch mode; resolves once the new pipelines are built (frames keep using the old plan until then). */
  async setMode(mode, params = {}) {
    if (!MODES.includes(mode)) throw new UpscalerError('internal', `unknown mode ${mode}`);
    const prevMode = this.mode, prevOpts = this.modeOpts;
    this.mode = mode; this.modeOpts = { ...this.modeOpts, ...params };
    try { await this._buildPlan(); } catch (e) { this.mode = prevMode; this.modeOpts = prevOpts; throw e; }
    if (this._running) this._renderCurrent();
  }

  /** Zero the counters/timings (e.g. before a measured run). */
  resetStats() {
    this._c = { rendered: 0, dropBusy: 0, dropBuild: 0, presented: 0, firstPresented: -1, completed: 0, rebuilds: this._c.rebuilds, errors: 0 };
    this._cpu = []; this._gpu = []; this._t0 = performance.now();
  }

  /** Fix the output size (setSize() with no arguments goes back to following the video element x devicePixelRatio). */
  setSize(width, height) { this.opts.width = width; this.opts.height = height; this._geomDirty = true; }

  stats() {
    const dt = Math.max(1e-3, (performance.now() - this._t0) / 1000), c = this._c;
    return {
      state: this.state, mode: this._modeLabel(), source: [this.sw, this.sh], target: [this.dw, this.dh],
      scale: this.sw ? +(this.dw / this.sw).toFixed(3) : 0,
      rendered: c.rendered, completed: c.completed, droppedBusy: c.dropBusy, droppedBuild: c.dropBuild,
      videoPresented: c.presented, missed: c.firstPresented < 0 ? 0 : Math.max(0, c.presented - c.firstPresented + 1 - c.rendered - c.dropBusy - c.dropBuild),
      fps: +(c.rendered / dt).toFixed(2), inFlight: this._inFlight, rebuilds: c.rebuilds, gpuErrors: c.errors, lastGpuError: this._lastGpuError || null,
      cpuMs: { median: +median(this._cpu).toFixed(3), p95: +pct(this._cpu, 0.95).toFixed(3) },
      gpuMs: this.hasTimestamps ? { median: +median(this._gpu).toFixed(3), p95: +pct(this._gpu, 0.95).toFixed(3), n: this._gpu.length } : null,
      adapter: this.adapterInfo, error: this.error ? { code: this.error.code, message: this.error.message } : null,
    };
  }

  /** Render the current video frame once, regardless of the in-flight limit. With {capture:true} resolves with
   *  {width,height,data:Uint8Array(RGBA)} read back from the canvas texture (needs opts.readback). Test/benchmark helper. */
  async renderOnce({ capture = false } = {}) {
    if (this.error) throw this.error;
    while (this._building) await new Promise(r => setTimeout(r, 5));
    if (this._geomDirty || this._srcChanged()) await this._buildPlan();
    let p = null;
    if (capture) { if (!this.opts.readback) throw new UpscalerError('internal', 'create with {readback:true} to capture'); p = new Promise((res, rej) => { this._pendingCapture = { res, rej }; }); }
    this._submitFrame();
    if (this.error) throw this.error;
    await this.device.queue.onSubmittedWorkDone();
    return p;
  }

  /** GPU benchmark of the current plan on the current video frame: `iterations` submits, each repeating the whole pass chain
   *  `batch` times between one pair of timestamps. Returns ms per frame for every iteration (needs timestamp-query). */
  async benchmark({ iterations = 60, batch = 4, warmup = 30 } = {}) {
    if (!this.hasTimestamps) throw new UpscalerError('internal', 'timestamp-query not available');
    while (this._building) await new Promise(r => setTimeout(r, 5));
    if (this._geomDirty || this._srcChanged()) await this._buildPlan();
    const d = this.device, out = [];
    const dst = d.createTexture({ size: [this.dw, this.dh], format: this.canvasFormat, usage: GPUTextureUsage.RENDER_ATTACHMENT });   // not the canvas: no present, no vsync pacing, GPU stays busy
    const qs = d.createQuerySet({ type: 'timestamp', count: 2 });
    const rb = d.createBuffer({ size: 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC });
    const mb = d.createBuffer({ size: 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST });
    for (let i = 0; i < warmup + iterations; i++) {
      const enc = d.createCommandEncoder();
      const ext = d.importExternalTexture({ source: this.video });
      this._encodeChain(enc, ext, dst.createView(), batch, qs);
      enc.resolveQuerySet(qs, 0, 2, rb, 0); enc.copyBufferToBuffer(rb, 0, mb, 0, 16);
      d.queue.submit([enc.finish()]);
      await mb.mapAsync(GPUMapMode.READ);
      const t = new BigUint64Array(mb.getMappedRange().slice(0)); mb.unmap();
      if (i >= warmup) out.push(Number(t[1] - t[0]) / 1e6 / batch);
    }
    dst.destroy(); qs.destroy(); rb.destroy(); mb.destroy();
    return out;
  }

  // ---------------------------------------------------------------------------------------------------- geometry
  _configure() {
    const ctx = this.canvas.getContext('webgpu');
    if (!ctx) throw new UpscalerError('unsupported', 'canvas.getContext("webgpu") returned null (canvas already has another context?)');
    this.context = ctx;
    this._updateGeometry();
    ctx.configure({ device: this.device, format: this.canvasFormat, alphaMode: 'opaque', colorSpace: 'srgb',
      usage: GPUTextureUsage.RENDER_ATTACHMENT | (this.opts.readback ? GPUTextureUsage.COPY_SRC : 0) });
  }

  /** Source size = video size; target = opts / video element x dpr / canvas size, never below the source, at most maxScale x. */
  _updateGeometry() {
    const v = this.video, o = this.opts;
    this.sw = v.videoWidth | 0; this.sh = v.videoHeight | 0;
    let w, h;
    if (o.width && o.height) { w = o.width; h = o.height; }
    else if (this.sw) {
      const dpr = window.devicePixelRatio || 1, bw = (v.clientWidth || this.canvas.clientWidth || this.sw) * dpr, bh = (v.clientHeight || this.canvas.clientHeight || this.sh) * dpr;
      const fit = (getComputedStyle(v).objectFit || 'contain');
      const s = fit === 'fill' ? null : fit === 'cover' ? Math.max(bw / this.sw, bh / this.sh) : fit === 'none' ? dpr : Math.min(bw / this.sw, bh / this.sh);
      w = s === null ? bw : this.sw * s; h = s === null ? bh : this.sh * s;
    } else { w = this.canvas.width; h = this.canvas.height; }  // no frame size yet
    const lim = this.device.limits.maxTextureDimension2D;
    if (this.sw) { w = Math.min(Math.max(w, this.sw), this.sw * o.maxScale); h = Math.min(Math.max(h, this.sh), this.sh * o.maxScale); }
    w = Math.max(1, Math.min(lim, Math.round(w))); h = Math.max(1, Math.min(lim, Math.round(h)));
    if (this.canvas.width !== w) this.canvas.width = w;
    if (this.canvas.height !== h) this.canvas.height = h;
    this.dw = w; this.dh = h; this._geomDirty = false;
  }

  _srcChanged() { return this.video.videoWidth !== this.sw || this.video.videoHeight !== this.sh || this.canvas.width !== this.dw || this.canvas.height !== this.dh; }

  // ---------------------------------------------------------------------------------------------------- plan
  _pipeline(code, format) {
    const key = format + '|' + code;
    let p = this._pipes.get(key);
    if (!p) {
      const module = this.device.createShaderModule({ code: VS + code });
      p = this.device.createRenderPipelineAsync({ layout: 'auto', vertex: { module, entryPoint: 'vs' },
        fragment: { module, entryPoint: 'fs', targets: [{ format }] }, primitive: { topology: 'triangle-list' } });
      this._pipes.set(key, p);
    }
    return p;
  }

  _modeLabel() {
    const m = this.modeOpts;
    return this.mode === 'cnn' ? `cnn-${m.cnnSize}${m.cnnRestore !== 'none' ? '+restore' + m.cnnRestore : ''}` : this.mode;
  }

  _tex(w, h, format, label) {
    return this.device.createTexture({ label, size: [w, h], format, usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
  }

  _params(inW, inH, outW, outH, extra0 = 0) {
    const b = this.device.createBuffer({ size: 32, usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    this.device.queue.writeBuffer(b, 0, new Float32Array([inW, inH, outW, outH, extra0, 0, 0, 0]));
    return b;
  }

  async _buildPlan() {
    const gen = (this._gen = (this._gen || 0) + 1);
    this._building = true;
    const d = this.device, old = this._plan;
    try {
      this._updateGeometry();
      if (!this.sw) { this._plan = null; return; }      // no frame size yet: wait for loadedmetadata
      const sw = this.sw, sh = this.sh, dw = this.dw, dh = this.dh, cf = this.canvasFormat;
      d.pushErrorScope('validation');
      const plan = { steps: [], textures: [], buffers: [], mode: this.mode, sw, sh, dw, dh };
      const addTex = (...a) => { const t = this._tex(...a); plan.textures.push(t); return t; };
      const m = this.modeOpts;
      const rcasOn = s => s !== false && s >= 0;
      // the optional "finish" stage: [EASU|resample] [-> RCAS] into the canvas, from a texture `from` of size fw x fh
      const finish = async (from, fw, fh, sharp, label) => {
        const fromView = from.createView();
        const needRcas = rcasOn(sharp);
        let target = null;
        if (needRcas) { const mid = addTex(dw, dh, 'rgba16float', label + '/easu'); target = mid.createView(); }
        const up = dw >= fw && dh >= fh;
        const pipeline = await this._pipeline(up ? FS_EASU : FS_RESAMPLE, needRcas ? 'rgba16float' : cf);
        const pb = this._params(fw, fh, dw, dh); plan.buffers.push(pb);
        plan.steps.push({ pipeline, bindGroup: d.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: this._sampler }, { binding: 1, resource: fromView }, { binding: 2, resource: { buffer: pb } }] }), target, w: dw, h: dh, label: label + (up ? '/easu' : '/resample') });
        if (needRcas) {
          const p2 = await this._pipeline(FS_RCAS, cf);
          const pb2 = this._params(dw, dh, dw, dh, Math.pow(2, -sharp)); plan.buffers.push(pb2);
          plan.steps.push({ pipeline: p2, bindGroup: d.createBindGroup({ layout: p2.getBindGroupLayout(0), entries: [{ binding: 0, resource: target }, { binding: 1, resource: { buffer: pb2 } }] }), target: null, w: dw, h: dh, label: label + '/rcas' });
        }
      };

      if (this.mode === 'bilinear') {
        plan.steps.push({ external: true, pipeline: await this._pipeline(FS_EXTERNAL, cf), target: null, w: dw, h: dh, label: 'bilinear' });
      } else {
        // pass 0: decoded frame -> rgba8unorm texture of the source size (exact copy)
        const src = addTex(sw, sh, 'rgba8unorm', 'src');
        plan.steps.push({ external: true, pipeline: await this._pipeline(FS_EXTERNAL, 'rgba8unorm'), target: src.createView(), w: sw, h: sh, label: 'blit' });
        if (this.mode === 'fsr') {
          await finish(src, sw, sh, m.sharpness, 'fsr');
        } else { // cnn
          let main = src;
          if (m.cnnRestore !== 'none') {
            const restored = addTex(sw, sh, 'rgba16float', 'restored');
            const r = await planModel(d, (c, f) => this._pipeline(c, f), this._sampler, await loadModel('restore', m.cnnRestore),
              { main: { view: main.createView(), w: sw, h: sh }, out: { view: restored.createView(), format: 'rgba16float' } }, 'restore' + m.cnnRestore);
            plan.steps.push(...r.steps); plan.textures.push(...r.textures); plan.buffers.push(...r.buffers);
            main = restored;
          }
          const direct = dw === sw * 2 && dh === sh * 2;
          const up2 = direct ? null : addTex(sw * 2, sh * 2, 'rgba16float', 'up2x');
          const r = await planModel(d, (c, f) => this._pipeline(c, f), this._sampler, await loadModel('upscale', m.cnnSize),
            { main: { view: main.createView(), w: sw, h: sh }, out: direct ? { view: null, format: cf } : { view: up2.createView(), format: 'rgba16float' } }, 'cnn' + m.cnnSize);
          plan.steps.push(...r.steps); plan.textures.push(...r.textures); plan.buffers.push(...r.buffers);
          if (!direct) await finish(up2, sw * 2, sh * 2, false, 'cnn');
        }
      }
      const err = await d.popErrorScope();
      if (err) throw new UpscalerError('shader', `plan for ${this._modeLabel()} failed: ${err.message}`);
      if (gen !== this._gen) { this._releasePlan(plan); return; }     // a newer build superseded this one
      this._plan = plan; this._c.rebuilds++;
      if (old) { const o = old; d.queue.onSubmittedWorkDone().then(() => this._releasePlan(o)); }
    } catch (e) {
      try { await d.popErrorScope(); } catch {}
      throw e instanceof UpscalerError ? e : new UpscalerError('shader', String(e && e.message || e), e);
    } finally { if (gen === this._gen) this._building = false; }
  }

  _releasePlan(p) { if (!p) return; for (const t of p.textures) try { t.destroy(); } catch {} for (const b of p.buffers) try { b.destroy(); } catch {} }

  // ---------------------------------------------------------------------------------------------------- frame loop
  /** Push one real frame through the whole plan (import + present) inside an error scope. On a hybrid laptop the NVIDIA adapter
   *  can exist yet be unable to share memory with Chrome's iGPU compositor: that surfaces here as an async validation error. */
  async _probe() {
    const v = this.video;
    if (v.readyState < 2 || !v.videoWidth) return;      // no frame yet: uncapturederror reports it later
    this.device.pushErrorScope('validation');
    this._submitFrame();
    await this.device.queue.onSubmittedWorkDone();
    await new Promise(r => { requestAnimationFrame(r); setTimeout(r, 200); });   // one present; rAF is paused in hidden tabs
    const err = await this.device.popErrorScope();
    if (this.error) throw this.error;
    if (err) throw new UpscalerError('import-failed', `GPU rejected the first frame: ${err.message.split('\n')[0]}`);
  }

  _importError(e) {
    if (e && e.name === 'SecurityError') return new UpscalerError('cross-origin', 'video is cross-origin without CORS (tainted); serve it with Access-Control-Allow-Origin and crossorigin="anonymous"', e);
    return new UpscalerError('import-failed', `importExternalTexture failed: ${e && e.message || e}`, e);
  }

  _onFrame(now, meta) {
    if (!this._running) return;
    this._rvfc = this.video.requestVideoFrameCallback(this._onFrame);
    if (meta) { if (this._c.firstPresented < 0) this._c.firstPresented = meta.presentedFrames; this._c.presented = meta.presentedFrames; }
    this._renderCurrent();
  }

  _renderCurrent() {
    if (this.error) return;
    if (this._building) { this._c.dropBuild++; return; }
    if (this._geomDirty || this._srcChanged() || !this._plan) {
      this._c.dropBuild++;
      this._buildPlan().catch(e => this._fail(e));
      return;
    }
    if (this._inFlight >= this.opts.maxInFlight) { this._c.dropBusy++; return; }
    this._submitFrame();
  }

  /** Encode the passes of the current plan `reps` times (reps > 1 is only used by benchmark()). */
  _encodeChain(enc, ext, canvasView, reps = 1, qs = null) {
    const steps = this._plan.steps, n = steps.length;
    for (let r = 0; r < reps; r++) {
      for (let i = 0; i < n; i++) {
        const s = steps[i];
        const desc = { colorAttachments: [{ view: s.target || canvasView, loadOp: 'clear', storeOp: 'store', clearValue: { r: 0, g: 0, b: 0, a: 1 } }] };
        if (qs && (r === 0 && i === 0 || r === reps - 1 && i === n - 1)) {
          desc.timestampWrites = { querySet: qs };
          if (r === 0 && i === 0) desc.timestampWrites.beginningOfPassWriteIndex = 0;
          if (r === reps - 1 && i === n - 1) desc.timestampWrites.endOfPassWriteIndex = 1;
        }
        const pass = enc.beginRenderPass(desc);
        pass.setPipeline(s.pipeline);
        pass.setBindGroup(0, s.external
          ? this.device.createBindGroup({ layout: s.pipeline.getBindGroupLayout(0), entries: [{ binding: 0, resource: this._sampler }, { binding: 1, resource: ext }] })
          : s.bindGroup);
        pass.draw(3);
        pass.end();
      }
    }
  }

  _tsSlot() {
    if (!this.hasTimestamps) return null;
    let s = this._ts.find(x => !x.busy);
    if (!s && this._ts.length < 4) {
      const d = this.device;
      s = { busy: false, qs: d.createQuerySet({ type: 'timestamp', count: 2 }),
        rb: d.createBuffer({ size: 16, usage: GPUBufferUsage.QUERY_RESOLVE | GPUBufferUsage.COPY_SRC }),
        mb: d.createBuffer({ size: 16, usage: GPUBufferUsage.MAP_READ | GPUBufferUsage.COPY_DST }) };
      this._ts.push(s);
    }
    return s || null;
  }

  _submitFrame() {
    const d = this.device, t0 = performance.now();
    let ext;
    try { ext = d.importExternalTexture({ source: this.video }); }
    catch (e) { this._fail(this._importError(e)); return; }
    const tex = this.context.getCurrentTexture();
    const enc = d.createCommandEncoder();
    const slot = this._tsSlot();
    this._encodeChain(enc, ext, tex.createView(), 1, slot ? slot.qs : null);
    if (slot) { enc.resolveQuerySet(slot.qs, 0, 2, slot.rb, 0); enc.copyBufferToBuffer(slot.rb, 0, slot.mb, 0, 16); slot.busy = true; }
    let cap = null;
    if (this._pendingCapture) {
      cap = this._pendingCapture; this._pendingCapture = null;
      const bpr = Math.ceil(this.dw * 4 / 256) * 256;
      cap.buf = d.createBuffer({ size: bpr * this.dh, usage: GPUBufferUsage.COPY_DST | GPUBufferUsage.MAP_READ });
      cap.bpr = bpr; cap.w = this.dw; cap.h = this.dh;
      enc.copyTextureToBuffer({ texture: tex }, { buffer: cap.buf, bytesPerRow: bpr, rowsPerImage: this.dh }, [this.dw, this.dh]);
    }
    d.queue.submit([enc.finish()]);
    this._inFlight++; this._c.rendered++;
    ring(this._cpu, performance.now() - t0);
    d.queue.onSubmittedWorkDone().then(() => { this._inFlight--; this._c.completed++; }, () => { this._inFlight = Math.max(0, this._inFlight - 1); });
    if (slot) slot.mb.mapAsync(GPUMapMode.READ).then(() => {
      const t = new BigUint64Array(slot.mb.getMappedRange().slice(0)); slot.mb.unmap(); slot.busy = false;
      ring(this._gpu, Number(t[1] - t[0]) / 1e6);
    }, () => { slot.busy = false; });
    if (cap) cap.buf.mapAsync(GPUMapMode.READ).then(() => {
      const src = new Uint8Array(cap.buf.getMappedRange());
      const out = new Uint8Array(cap.w * cap.h * 4), bgra = this.canvasFormat.startsWith('bgra');
      for (let y = 0; y < cap.h; y++) {
        const row = src.subarray(y * cap.bpr, y * cap.bpr + cap.w * 4), o = y * cap.w * 4;
        if (bgra) for (let x = 0; x < cap.w * 4; x += 4) { out[o + x] = row[x + 2]; out[o + x + 1] = row[x + 1]; out[o + x + 2] = row[x]; out[o + x + 3] = 255; }
        else out.set(row, o);
      }
      cap.buf.unmap(); cap.buf.destroy();
      cap.res({ width: cap.w, height: cap.h, data: out, mediaTime: this.video.currentTime });
    }, e => cap.rej(e));
  }

  _fail(err) {
    if (this.error) return;
    this.error = err instanceof UpscalerError ? err : new UpscalerError('internal', String(err && err.message || err), err);
    this.state = 'failed';
    this.stop();
    try { this.opts.onError?.(this.error); } catch {}
  }
}
