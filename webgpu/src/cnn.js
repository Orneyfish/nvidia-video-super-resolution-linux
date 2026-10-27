// Anime4K CNN passes (src/models/*.js, extracted from the MIT-licensed mpv GLSL hooks) as WGSL render passes.
// Anime4K: https://github.com/bloc97/Anime4K, Copyright (c) 2019-2021 bloc97, MIT (full text: ../LICENSE-Anime4K.txt).
//
// A model is a list of passes. Each conv pass reads 1..n rgba16float feature maps (the 4-channel outputs of earlier passes) at
// 3x3 (or 1x1) taps, applies CReLU (relu(x) and relu(-x) feed different matrices) and writes 4 new feature channels:
//    out = bias + sum_taps  M_tap * g(tex(x+dx, y+dy))
// The upscale models end in a "depth to space" pass: the last 4 channels are the 2x2 sub-pixel residual of a luma-like
// correction that is added to the bilinear-upscaled source. The restore models end in `addMain` (residual added at source size).

/** WGSL for one convolution pass. `texList` = names of the textures bound at @binding(1+i). */
function convWGSL(p, texList) {
  const nT = p.taps.length;
  const loads = new Map(); // "tex|dx|dy" -> variable name
  const lines = [];
  for (const [g, dx, dy] of p.taps) {
    const [tn] = p.defs[g];
    const key = `${tn}|${dx}|${dy}`;
    if (!loads.has(key)) {
      const v = 's' + loads.size;
      loads.set(key, v);
      lines.push(`  let ${v} = textureLoad(t${texList.indexOf(tn)}, clamp(c + vec2i(${dx}, ${dy}), vec2i(0), mx), 0);`);
    }
  }
  const terms = p.taps.map(([g, dx, dy], i) => {
    const [tn, sign] = p.defs[g];
    const v = loads.get(`${tn}|${dx}|${dy}`);
    const arg = sign === 0 ? v : sign > 0 ? `max(${v}, vec4f(0.0))` : `max(-${v}, vec4f(0.0))`;
    return `  r += P.m[${i}] * ${arg};`;
  });
  const addMain = p.post === 'addMain';
  const mainIdx = texList.indexOf('MAIN');
  return /* wgsl */`
struct Params { m: array<mat4x4f, ${nT}>, bias: vec4f };
@group(0) @binding(0) var<uniform> P: Params;
${texList.map((n, i) => `@group(0) @binding(${i + 1}) var t${i}: texture_2d<f32>; // ${n}`).join('\n')}
@fragment fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let c = vec2i(frag.xy);
  let mx = vec2i(textureDimensions(t0)) - vec2i(1);
${lines.join('\n')}
  var r = P.bias;
${terms.join('\n')}
${addMain ? `  return vec4f(r.rgb + textureLoad(t${mainIdx}, c, 0).rgb, 1.0);` : '  return r;'}
}
`;
}

/** Depth-to-space + bilinear base: output pixel (2x2 per source pixel) = bilinear(MAIN) + sub-pixel residual channel. */
const D2S_WGSL = /* wgsl */`
@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var tlast: texture_2d<f32>;
@group(0) @binding(2) var tmain: texture_2d<f32>;
@fragment fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let ip = vec2i(frag.xy);
  let lr = ip / vec2i(2);
  let sub = ip - lr * vec2i(2);
  let v = textureLoad(tlast, lr, 0);
  let top = select(v.x, v.y, sub.x == 1);
  let bot = select(v.z, v.w, sub.x == 1);
  let res = select(top, bot, sub.y == 1);
  let uv = frag.xy / (2.0 * vec2f(textureDimensions(tlast)));
  let base = textureSampleLevel(tmain, smp, uv, 0.0).rgb;
  return vec4f(base + vec3f(res), 1.0);
}
`;

const FEATURE_FORMAT = 'rgba16float';

/**
 * Build the render steps of one model.
 * @param io.main   {tex, view, w, h}    the model's MAIN input (source-sized RGB texture)
 * @param io.out    {view|null, format, tex?}  destination of the last pass (null view = the canvas)
 * @returns {{steps, textures, buffers}}   steps: {pipeline, bindGroup, target (view|null), w, h, label}
 */
export async function planModel(device, getPipeline, sampler, model, io, label) {
  const W = io.main.w, H = io.main.h;
  const textures = [], buffers = [], steps = [];
  const byName = new Map([['MAIN', io.main.view]]);
  const passes = model.passes;
  for (let pi = 0; pi < passes.length; pi++) {
    const p = passes[pi];
    const last = pi === passes.length - 1;
    const isD2S = p.post === 'd2s';
    const outW = isD2S ? W * 2 : W, outH = isD2S ? H * 2 : H;
    let target, format;
    if (last) { target = io.out.view; format = io.out.format; }
    else {
      const t = device.createTexture({ label: `${label}/${p.save}`, size: [outW, outH], format: FEATURE_FORMAT,
        usage: GPUTextureUsage.RENDER_ATTACHMENT | GPUTextureUsage.TEXTURE_BINDING });
      textures.push(t); target = t.createView(); format = FEATURE_FORMAT; byName.set(p.save, target);
    }
    if (isD2S) {
      const pipeline = await getPipeline(D2S_WGSL, format);
      const bindGroup = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
        { binding: 0, resource: sampler },
        { binding: 1, resource: byName.get('conv2d_last_tf') },
        { binding: 2, resource: byName.get('MAIN') }] });
      steps.push({ pipeline, bindGroup, target, w: outW, h: outH, label: `${label}/d2s` });
      continue;
    }
    // textures read by this pass, in a stable order (the ones named in defs, then MAIN for addMain)
    const texList = [];
    for (const g of Object.keys(p.defs)) if (!texList.includes(p.defs[g][0])) texList.push(p.defs[g][0]);
    if (p.post === 'addMain' && !texList.includes('MAIN')) texList.push('MAIN');
    const code = convWGSL(p, texList);
    const pipeline = await getPipeline(code, format);
    const data = new Float32Array(p.taps.length * 16 + 4);
    data.set(p.w, 0); data.set(p.bias, p.taps.length * 16);
    const buf = device.createBuffer({ label: `${label}/${p.save}/weights`, size: data.byteLength,
      usage: GPUBufferUsage.UNIFORM | GPUBufferUsage.COPY_DST });
    device.queue.writeBuffer(buf, 0, data);
    buffers.push(buf);
    const bindGroup = device.createBindGroup({ layout: pipeline.getBindGroupLayout(0), entries: [
      { binding: 0, resource: { buffer: buf } },
      ...texList.map((n, i) => ({ binding: i + 1, resource: byName.get(n) }))] });
    steps.push({ pipeline, bindGroup, target, w: outW, h: outH, label: `${label}/${p.save}` });
  }
  return { steps, textures, buffers };
}

export async function loadModel(kind, size) {
  const file = `./models/anime4k_${kind}_${size.toLowerCase()}.js`;
  try { return (await import(file)).default; }
  catch (e) { throw new Error(`unknown CNN model ${kind}/${size}: ${e.message}`); }
}
