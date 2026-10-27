// WGSL for the non-CNN passes. All passes are full-screen-triangle render passes (no compute) so they can write straight
// into the canvas texture, whose format (bgra8unorm) cannot be a storage texture without an optional feature.
//
// FSR 1.0 EASU / RCAS: ported to WGSL from AMD's ffx_fsr1.h (FidelityFX Super Resolution 1.0, v1.20210629),
//   Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved. MIT licence (full text: ../LICENSE-FSR.txt).
//   Differences from the reference: exact 1/x and inverseSqrt() instead of the bit-trick approximations (APrx*),
//   denominators clamped away from zero (WGSL division by zero is not defined to give the +inf AMD's code relies on),
//   RCAS without the optional FSR_RCAS_DENOISE path, taps via textureGather exactly like the FSR_EASU_F reference.

export const VS = /* wgsl */`
struct VOut { @builtin(position) pos: vec4f, @location(0) uv: vec2f };
@vertex fn vs(@builtin(vertex_index) i: u32) -> VOut {
  var p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var o: VOut;
  o.pos = vec4f(p[i], 0.0, 1.0);
  o.uv = vec2f((p[i].x + 1.0) * 0.5, (1.0 - p[i].y) * 0.5);
  return o;
}
`;

// Video frame -> RGB (decoded + colour-converted by the browser) sampled bilinearly. Used as the `bilinear` mode (target =
// canvas) and as the first pass of every other mode (target = rgba8unorm texture of the source size: at texel centres the
// bilinear tap returns the texel itself, so the copy is exact).
export const FS_EXTERNAL = /* wgsl */`
@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var ext: texture_external;
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  return vec4f(textureSampleBaseClampToEdge(ext, smp, in.uv).rgb, 1.0);
}
`;

// Plain texture -> texture/canvas, bilinear (used to resample a CNN's 2x output to the target when it is smaller).
// Four taps inside the output pixel footprint = cheap supersampling for the 1x..2x downscale case.
export const FS_RESAMPLE = /* wgsl */`
struct P { inSize: vec2f, outSize: vec2f, extra: vec4f };
@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> u: P;
@fragment fn fs(in: VOut) -> @location(0) vec4f {
  let d = 0.25 / u.outSize;
  var c = textureSampleLevel(src, smp, in.uv + vec2f(-d.x, -d.y), 0.0).rgb;
  c += textureSampleLevel(src, smp, in.uv + vec2f( d.x, -d.y), 0.0).rgb;
  c += textureSampleLevel(src, smp, in.uv + vec2f(-d.x,  d.y), 0.0).rgb;
  c += textureSampleLevel(src, smp, in.uv + vec2f( d.x,  d.y), 0.0).rgb;
  return vec4f(c * 0.25, 1.0);
}
`;

export const FS_EASU = /* wgsl */`
struct P { inSize: vec2f, outSize: vec2f, extra: vec4f };
@group(0) @binding(0) var smp: sampler;
@group(0) @binding(1) var src: texture_2d<f32>;
@group(0) @binding(2) var<uniform> u: P;

fn rcp(x: f32) -> f32 { return 1.0 / max(x, 1.0e-6); }

// Filtering for one tap (EASU's anisotropic Lanczos-2 approximation).
fn easuTap(aC: ptr<function, vec3f>, aW: ptr<function, f32>, off: vec2f, dir: vec2f, len: vec2f, lob: f32, clp: f32, c: vec3f) {
  var v = vec2f(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
  v = v * len;
  var d2 = min(v.x * v.x + v.y * v.y, clp);
  var wB = (2.0 / 5.0) * d2 - 1.0;
  var wA = lob * d2 - 1.0;
  wB = wB * wB;
  wA = wA * wA;
  wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
  let w = wB * wA;
  *aC = *aC + c * w;
  *aW = *aW + w;
}

// Accumulate gradient direction and length for one of the four bilinear quadrants.
fn easuSet(dir: ptr<function, vec2f>, len: ptr<function, f32>, pp: vec2f, w: f32, lA: f32, lB: f32, lC: f32, lD: f32, lE: f32) {
  let dc = lD - lC;
  let cb = lC - lB;
  var lenX = rcp(max(abs(dc), abs(cb)));
  let dirX = lD - lB;
  (*dir).x = (*dir).x + dirX * w;
  lenX = clamp(abs(dirX) * lenX, 0.0, 1.0);
  lenX = lenX * lenX;
  *len = *len + lenX * w;
  let ec = lE - lC;
  let ca = lC - lA;
  var lenY = rcp(max(abs(ec), abs(ca)));
  let dirY = lE - lA;
  (*dir).y = (*dir).y + dirY * w;
  lenY = clamp(abs(dirY) * lenY, 0.0, 1.0);
  lenY = lenY * lenY;
  *len = *len + lenY * w;
}

@fragment fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let ip = floor(frag.xy);
  let sc = u.inSize / u.outSize;
  var pp = ip * sc + (0.5 * sc - 0.5);
  let fp = floor(pp);
  pp = pp - fp;
  let r = 1.0 / u.inSize;
  let p0 = fp * r + r * vec2f(1.0, -1.0);
  let p1 = p0 + r * vec2f(-1.0, 2.0);
  let p2 = p0 + r * vec2f(1.0, 2.0);
  let p3 = p0 + r * vec2f(0.0, 4.0);
  // 12-tap kernel:    b c
  //                 e f g h
  //                 i j k l
  //                   n o        (gather order: x = bottom-left, y = bottom-right, z = top-right, w = top-left)
  let bczzR = textureGather(0, src, smp, p0); let bczzG = textureGather(1, src, smp, p0); let bczzB = textureGather(2, src, smp, p0);
  let ijfeR = textureGather(0, src, smp, p1); let ijfeG = textureGather(1, src, smp, p1); let ijfeB = textureGather(2, src, smp, p1);
  let klhgR = textureGather(0, src, smp, p2); let klhgG = textureGather(1, src, smp, p2); let klhgB = textureGather(2, src, smp, p2);
  let zzonR = textureGather(0, src, smp, p3); let zzonG = textureGather(1, src, smp, p3); let zzonB = textureGather(2, src, smp, p3);
  // Luma times 2 (cheapest possible approximation).
  let bczzL = bczzB * 0.5 + (bczzR * 0.5 + bczzG);
  let ijfeL = ijfeB * 0.5 + (ijfeR * 0.5 + ijfeG);
  let klhgL = klhgB * 0.5 + (klhgR * 0.5 + klhgG);
  let zzonL = zzonB * 0.5 + (zzonR * 0.5 + zzonG);
  let bL = bczzL.x; let cL = bczzL.y;
  let iL = ijfeL.x; let jL = ijfeL.y; let fL = ijfeL.z; let eL = ijfeL.w;
  let kL = klhgL.x; let lL = klhgL.y; let hL = klhgL.z; let gL = klhgL.w;
  let oL = zzonL.z; let nL = zzonL.w;
  var dir = vec2f(0.0);
  var len = 0.0;
  easuSet(&dir, &len, pp, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
  easuSet(&dir, &len, pp, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
  easuSet(&dir, &len, pp, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
  easuSet(&dir, &len, pp, pp.x * pp.y, gL, jL, kL, lL, oL);
  // Normalize the direction, cleanup close to zero.
  let dir2 = dir * dir;
  var dirR = dir2.x + dir2.y;
  let zro = dirR < (1.0 / 32768.0);
  dirR = inverseSqrt(max(dirR, 1.0e-12));
  dirR = select(dirR, 1.0, zro);
  dir.x = select(dir.x, 1.0, zro);
  dir = dir * dirR;
  // Shape the length.
  len = len * 0.5;
  len = len * len;
  let stretch = (dir.x * dir.x + dir.y * dir.y) * rcp(max(abs(dir.x), abs(dir.y)));
  let len2 = vec2f(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
  let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
  let clp = rcp(lob);
  // Min/max of the 4 nearest taps (f g j k) for the final de-ringing clamp.
  let f3 = vec3f(ijfeR.z, ijfeG.z, ijfeB.z);
  let g3 = vec3f(klhgR.w, klhgG.w, klhgB.w);
  let j3 = vec3f(ijfeR.y, ijfeG.y, ijfeB.y);
  let k3 = vec3f(klhgR.x, klhgG.x, klhgB.x);
  let min4 = min(min(min(f3, g3), j3), k3);
  let max4 = max(max(max(f3, g3), j3), k3);
  var aC = vec3f(0.0);
  var aW = 0.0;
  easuTap(&aC, &aW, vec2f( 0.0, -1.0) - pp, dir, len2, lob, clp, vec3f(bczzR.x, bczzG.x, bczzB.x)); // b
  easuTap(&aC, &aW, vec2f( 1.0, -1.0) - pp, dir, len2, lob, clp, vec3f(bczzR.y, bczzG.y, bczzB.y)); // c
  easuTap(&aC, &aW, vec2f(-1.0,  1.0) - pp, dir, len2, lob, clp, vec3f(ijfeR.x, ijfeG.x, ijfeB.x)); // i
  easuTap(&aC, &aW, vec2f( 0.0,  1.0) - pp, dir, len2, lob, clp, vec3f(ijfeR.y, ijfeG.y, ijfeB.y)); // j
  easuTap(&aC, &aW, vec2f( 0.0,  0.0) - pp, dir, len2, lob, clp, vec3f(ijfeR.z, ijfeG.z, ijfeB.z)); // f
  easuTap(&aC, &aW, vec2f(-1.0,  0.0) - pp, dir, len2, lob, clp, vec3f(ijfeR.w, ijfeG.w, ijfeB.w)); // e
  easuTap(&aC, &aW, vec2f( 1.0,  1.0) - pp, dir, len2, lob, clp, vec3f(klhgR.x, klhgG.x, klhgB.x)); // k
  easuTap(&aC, &aW, vec2f( 2.0,  1.0) - pp, dir, len2, lob, clp, vec3f(klhgR.y, klhgG.y, klhgB.y)); // l
  easuTap(&aC, &aW, vec2f( 2.0,  0.0) - pp, dir, len2, lob, clp, vec3f(klhgR.z, klhgG.z, klhgB.z)); // h
  easuTap(&aC, &aW, vec2f( 1.0,  0.0) - pp, dir, len2, lob, clp, vec3f(klhgR.w, klhgG.w, klhgB.w)); // g
  easuTap(&aC, &aW, vec2f( 1.0,  2.0) - pp, dir, len2, lob, clp, vec3f(zzonR.z, zzonG.z, zzonB.z)); // o
  easuTap(&aC, &aW, vec2f( 0.0,  2.0) - pp, dir, len2, lob, clp, vec3f(zzonR.w, zzonG.w, zzonB.w)); // n
  let pix = min(max4, max(min4, aC * (1.0 / aW)));
  return vec4f(pix, 1.0);
}
`;

// RCAS: robust contrast-adaptive sharpening on the EASU output. u.extra.x = 2^-sharpnessStops (0 stops = maximum).
export const FS_RCAS = /* wgsl */`
struct P { inSize: vec2f, outSize: vec2f, extra: vec4f };
@group(0) @binding(0) var src: texture_2d<f32>;
@group(0) @binding(1) var<uniform> u: P;
const RCAS_LIMIT: f32 = 0.1875; // 0.25 - 1/16

fn ld(p: vec2i) -> vec3f {
  let mx = vec2i(textureDimensions(src)) - vec2i(1);
  return textureLoad(src, clamp(p, vec2i(0), mx), 0).rgb;
}

@fragment fn fs(@builtin(position) frag: vec4f) -> @location(0) vec4f {
  let sp = vec2i(frag.xy);
  //    b
  //  d e f
  //    h
  let b = ld(sp + vec2i(0, -1));
  let d = ld(sp + vec2i(-1, 0));
  let e = ld(sp);
  let f = ld(sp + vec2i(1, 0));
  let h = ld(sp + vec2i(0, 1));
  let mn4 = min(min(min(b, d), f), h);
  let mx4 = max(max(max(b, d), f), h);
  let hitMin = min(mn4, e) / (4.0 * max(mx4, vec3f(1.0e-4)));
  let hitMax = (vec3f(1.0) - max(mx4, e)) / min(4.0 * mn4 - vec3f(4.0), vec3f(-1.0e-4));
  let lobeRGB = max(-hitMin, hitMax);
  let lobe = max(-RCAS_LIMIT, min(max(max(lobeRGB.r, lobeRGB.g), lobeRGB.b), 0.0)) * u.extra.x;
  let rcpL = 1.0 / (4.0 * lobe + 1.0);
  return vec4f((lobe * b + lobe * d + lobe * h + lobe * f + e) * rcpL, 1.0);
}
`;
