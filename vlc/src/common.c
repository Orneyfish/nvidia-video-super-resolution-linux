/*
 * common.c - options, source classification, target geometry, power state and the SR engine wrapper
 * (nvvsr_ctx + CPU bilinear fallback) used by both the vout display and the video filter.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <vlc_common.h>
#include <vlc_fourcc.h>
#include <vlc_mtime.h>
#include <vlc_variables.h>

#include "nvvsr_vlc.h"

/* ------------------------------------------------------------------------------------------- */
/* Options                                                                                      */
/* ------------------------------------------------------------------------------------------- */

int nv_quality_parse(const char *s) {
  static const struct {
    const char *name;
    int value;
  } tab[] = {{"auto", NVVSR_QUALITY_AUTO},
             {"bicubic", NVVSR_QUALITY_BICUBIC},
             {"low", NVVSR_QUALITY_LOW},
             {"medium", NVVSR_QUALITY_MEDIUM},
             {"high", NVVSR_QUALITY_HIGH},
             {"ultra", NVVSR_QUALITY_ULTRA},
             {"streaming_medium", NVVSR_QUALITY_STREAMING_MEDIUM},
             {"streaming_ultra", NVVSR_QUALITY_STREAMING_ULTRA}};
  if (s == NULL) return INT32_MIN;
  while (*s == ' ') s++;
  for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++)
    if (strncasecmp(s, tab[i].name, strlen(tab[i].name)) == 0) {
      const char *rest = s + strlen(tab[i].name);
      while (*rest == ' ') rest++;
      if (*rest == '\0') return tab[i].value;
    }
  return INT32_MIN;
}

static int clamp_int(int64_t v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : (int)v); }

void nv_opts_read(vlc_object_t *obj, nv_opts_t *o) {
  memset(o, 0, sizeof *o);
  o->enable = var_InheritBool(obj, NV_CFG "enable");
  o->ac_only = var_InheritBool(obj, NV_CFG "ac-only");
  o->fullscreen_only = var_InheritBool(obj, NV_CFG "fullscreen-only");
  o->width = clamp_int(var_InheritInteger(obj, NV_CFG "width"), 0, 16384);
  o->height = clamp_int(var_InheritInteger(obj, NV_CFG "height"), 0, 16384);
  o->min_height = clamp_int(var_InheritInteger(obj, NV_CFG "min-height"), 0, 4320);
  o->min_gain = clamp_int(var_InheritInteger(obj, NV_CFG "min-gain"), 0, 100) / 100.0;

  char *q = var_InheritString(obj, NV_CFG "quality");
  o->quality = NVVSR_QUALITY_AUTO;
  if (q != NULL && *q != '\0') {
    int v = nv_quality_parse(q);
    if (v == INT32_MIN)
      msg_Warn(obj, "unknown " NV_CFG "quality '%s', using auto", q);
    else
      o->quality = v;
  }
  free(q);

  o->vout = var_InheritString(obj, NV_CFG "vout");
  if (o->vout != NULL && *o->vout == '\0') {
    free(o->vout);
    o->vout = NULL;
  }
}

void nv_opts_clean(nv_opts_t *o) {
  free(o->vout);
  o->vout = NULL;
}

/* ------------------------------------------------------------------------------------------- */
/* Source classification and geometry                                                           */
/* ------------------------------------------------------------------------------------------- */

void nv_classify(const video_format_t *f, nv_src_t *o) {
  memset(o, 0, sizeof *o);
  o->why = "";
  o->x = f->i_x_offset;
  o->y = f->i_y_offset;
  o->w = f->i_visible_width ? f->i_visible_width : f->i_width;
  o->h = f->i_visible_height ? f->i_visible_height : f->i_height;
  o->sar_num = f->i_sar_num && f->i_sar_den ? f->i_sar_num : 1;
  o->sar_den = f->i_sar_num && f->i_sar_den ? f->i_sar_den : 1;
  o->fps = f->i_frame_rate && f->i_frame_rate_base ? (double)f->i_frame_rate / f->i_frame_rate_base : 0.0;
  o->matrix = NVVSR_MATRIX_AUTO;

  switch (f->i_chroma) {
    case VLC_CODEC_I420:
      o->chroma = VLC_CODEC_I420;
      o->in_format = NVVSR_IN_I420;
      break;
    case VLC_CODEC_J420:
      o->chroma = VLC_CODEC_J420;
      o->in_format = NVVSR_IN_I420;
      o->full_range = 1;
      break;
    case VLC_CODEC_YV12:
      o->chroma = VLC_CODEC_YV12;
      o->in_format = NVVSR_IN_I420;
      o->swap_uv = true;
      break;
    case VLC_CODEC_NV12:
      o->chroma = VLC_CODEC_NV12;
      o->in_format = NVVSR_IN_NV12;
      break;
    case VLC_CODEC_VAAPI_420:        /* 8-bit hardware surfaces: VLC downloads them to I420 for us */
    case VLC_CODEC_VDPAU_VIDEO_420:
      o->chroma = VLC_CODEC_I420;
      o->in_format = NVVSR_IN_I420;
      o->convert = true;
      break;
    default:
      o->why = "pixel format is not 8-bit 4:2:0";
      return;
  }

  if (f->orientation != ORIENT_NORMAL) {
    o->why = "rotated or flipped video";
    return;
  }
  if (f->transfer == TRANSFER_FUNC_SMPTE_ST2084 || f->transfer == TRANSFER_FUNC_HLG) {
    o->why = "HDR";
    return;
  }
  if (f->space == COLOR_SPACE_BT2020 || f->primaries == COLOR_PRIMARIES_BT2020) {
    o->why = "BT.2020 (wide gamut)";
    return;
  }
  if (f->projection_mode != PROJECTION_MODE_RECTANGULAR) {
    o->why = "360 degree video";
    return;
  }
  if (o->w < 16 || o->h < 16 || o->w > 8192 || o->h > 8192) {
    o->why = "picture size out of range";
    return;
  }
  if ((o->w | o->h | o->x | o->y) & 1u) {
    o->why = "odd picture size or crop offset";
    return;
  }
  if (f->space == COLOR_SPACE_BT601)
    o->matrix = NVVSR_MATRIX_BT601;
  else if (f->space == COLOR_SPACE_BT709)
    o->matrix = NVVSR_MATRIX_BT709;
  if (f->b_color_range_full) o->full_range = 1;
  o->ok = true;
}

static unsigned even_round(double v) {
  long n = lround(v / 2.0) * 2;
  return n < 2 ? 2u : (unsigned)n;
}

bool nv_target_size(const nv_src_t *s, unsigned pw, unsigned ph, double min_gain, unsigned *ow, unsigned *oh,
                    double *scale_out) {
  if (scale_out != NULL) *scale_out = 0.0;
  if (pw == 0 || ph == 0 || s->w == 0 || s->h == 0 || s->sar_den == 0) return false;
  const double dw = (double)s->w * (double)s->sar_num / (double)s->sar_den;
  const double dh = (double)s->h;
  double scale = fmin((double)pw / dw, (double)ph / dh);
  if (scale_out != NULL) *scale_out = scale;
  if (!(scale >= 1.0 + min_gain)) return false;
  const double c = fmin(scale, 4.0);
  unsigned w = even_round(s->w * c), h = even_round(s->h * c);
  if (w < s->w) w = s->w;
  if (h < s->h) h = s->h;
  if (w > 4 * s->w) w = 4 * s->w;
  if (h > 4 * s->h) h = 4 * s->h;
  *ow = w;
  *oh = h;
  return true;
}

/* ------------------------------------------------------------------------------------------- */
/* Power                                                                                        */
/* ------------------------------------------------------------------------------------------- */

static bool read_word(const char *path, char *buf, size_t n) {
  FILE *f = fopen(path, "r");
  if (f == NULL) return false;
  bool ok = fgets(buf, (int)n, f) != NULL;
  fclose(f);
  if (ok) buf[strcspn(buf, "\r\n")] = '\0';
  return ok;
}

bool nv_on_ac(void) {
  const char *dir = "/sys/class/power_supply";
#ifdef NVVSR_VLC_TESTHOOK
  const char *over = getenv("NVVSR_VLC_POWER_DIR");
  if (over != NULL && *over != '\0') dir = over;
#endif
  DIR *d = opendir(dir);
  if (d == NULL) return true; /* no information (desktop, container): treat as AC */
  bool battery = false, ac = false;
  struct dirent *de;
  while ((de = readdir(d)) != NULL) {
    if (de->d_name[0] == '.') continue;
    char path[PATH_MAX], type[64] = "", online[16] = "";
    snprintf(path, sizeof path, "%s/%s/type", dir, de->d_name);
    if (!read_word(path, type, sizeof type)) continue;
    if (strcasecmp(type, "battery") == 0) {
      battery = true;
    } else if (strcasecmp(type, "mains") == 0 || strncasecmp(type, "usb", 3) == 0) {
      snprintf(path, sizeof path, "%s/%s/online", dir, de->d_name);
      if (read_word(path, online, sizeof online) && atoi(online) == 1) ac = true;
    }
  }
  closedir(d);
  if (ac) return true;
  if (battery) return false;
  return true;
}

/* ------------------------------------------------------------------------------------------- */
/* Engine                                                                                       */
/* ------------------------------------------------------------------------------------------- */

static int engine_open_impl(nv_engine_t *e, vlc_object_t *obj, const nv_src_t *s, unsigned ow, unsigned oh, int quality) {
  memset(e, 0, sizeof *e);
  e->obj = obj;
  e->src = *s;
  e->out_w = ow;
  e->out_h = oh;
  e->fail_after = -1;
#ifdef NVVSR_VLC_TESTHOOK
  const char *fa = getenv("NVVSR_VLC_FAIL_AFTER");
  if (fa != NULL && *fa != '\0') e->fail_after = atol(fa);
#endif

  nvvsr_config c;
  nvvsr_config_init(&c);
  c.in_w = (int32_t)s->w;
  c.in_h = (int32_t)s->h;
  c.out_w = (int32_t)ow;
  c.out_h = (int32_t)oh;
  c.quality = quality;
  c.in_format = s->in_format;
  c.out_format = NVVSR_OUT_I420;
  c.matrix = s->matrix;
  c.full_range = s->full_range;
  c.fps = s->fps;
  const int rc = nvvsr_create(&c, &e->ctx);
  if (rc != NVVSR_OK) {
    e->ctx = NULL;
    msg_Warn(obj, "cannot create the super resolution context (%s: %s)", nvvsr_strerror(rc), nvvsr_last_error());
    return rc;
  }
  msg_Info(obj, "super resolution %ux%u -> %ux%u, quality %d (requested %d)", s->w, s->h, ow, oh,
           nvvsr_get_quality(e->ctx), quality);
  return 0;
}

struct open_args {
  nv_engine_t *e;
  vlc_object_t *obj;
  const nv_src_t *s;
  unsigned ow, oh;
  int quality;
  int rc;
};

static void *engine_open_thread(void *data) {
  struct open_args *a = data;
  a->rc = engine_open_impl(a->e, a->obj, a->s, a->ow, a->oh, a->quality);
  return NULL;
}

/* nvvsr_create() loads the NVIDIA model through libnvidia-ngx, which overflows VLC's 1 MiB thread stacks (SIGSEGV
 * in NVSDK_NGX_CUDA_Init_Ext1; >= 2 MiB is needed). So it always runs on a short-lived thread with a large stack. */
int nv_engine_open(nv_engine_t *e, vlc_object_t *obj, const nv_src_t *s, unsigned ow, unsigned oh, int quality) {
  struct open_args a = {e, obj, s, ow, oh, quality, NVVSR_ERR_INTERNAL};
  pthread_attr_t attr;
  pthread_t th;
  bool started = false;
  if (pthread_attr_init(&attr) == 0) {
    started = pthread_attr_setstacksize(&attr, (size_t)8 << 20) == 0 && pthread_create(&th, &attr, engine_open_thread, &a) == 0;
    pthread_attr_destroy(&attr);
  }
  if (!started) {
    memset(e, 0, sizeof *e);
    msg_Warn(obj, "cannot start a thread for the super resolution context");
    return NVVSR_ERR_INTERNAL;
  }
  pthread_join(th, NULL);
  return a.rc;
}

void nv_engine_close(nv_engine_t *e) {
  if (e->ctx != NULL) {
    nvvsr_destroy(e->ctx);
    e->ctx = NULL;
  }
}

void nv_engine_log_summary(const nv_engine_t *e) {
  if (e->frames == 0 && e->fallback_frames == 0) return;
  msg_Info(e->obj, "super resolution %ux%u -> %ux%u: %lu frames, SR %.2f ms avg / %.2f ms max, %lu frames by CPU fallback%s",
           e->src.w, e->src.h, e->out_w, e->out_h, e->frames, e->frames ? e->ms_sum / (double)e->frames : 0.0,
           e->ms_max, e->fallback_frames, e->failed ? " (SR failed)" : "");
}

/* Pointers to the source rectangle inside the decoder picture; false if the picture does not fit. */
static bool map_input(const nv_src_t *s, const picture_t *p, nvvsr_image_in *img) {
  memset(img, 0, sizeof *img);
  const unsigned x = s->x, y = s->y, w = s->w, h = s->h;
  if (p->p[0].p_pixels == NULL || p->p[0].i_pitch < (int)(x + w) || p->p[0].i_lines < (int)(y + h)) return false;
  img->plane[0] = p->p[0].p_pixels + (size_t)y * (size_t)p->p[0].i_pitch + x;
  img->stride[0] = p->p[0].i_pitch;
  if (s->in_format == NVVSR_IN_NV12) {
    if (p->i_planes < 2 || p->p[1].p_pixels == NULL || p->p[1].i_pitch < (int)(x + w) ||
        p->p[1].i_lines < (int)((y + h) / 2))
      return false;
    img->plane[1] = p->p[1].p_pixels + (size_t)(y / 2) * (size_t)p->p[1].i_pitch + x;
    img->stride[1] = p->p[1].i_pitch;
  } else {
    if (p->i_planes < 3) return false;
    for (int i = 0; i < 2; i++) {
      const plane_t *c = &p->p[s->swap_uv ? 2 - i : 1 + i];
      if (c->p_pixels == NULL || c->i_pitch < (int)((x + w) / 2) || c->i_lines < (int)((y + h) / 2)) return false;
      img->plane[1 + i] = c->p_pixels + (size_t)(y / 2) * (size_t)c->i_pitch + x / 2;
      img->stride[1 + i] = c->i_pitch;
    }
  }
  return true;
}

/* Bilinear resize of one plane (8-bit samples `sstep` bytes apart), half-pixel centres, 8-bit fixed point. */
static void plane_bilinear(const uint8_t *src, int sstride, int sstep, unsigned sw, unsigned sh, uint8_t *dst,
                           int dstride, unsigned dw, unsigned dh) {
  int *xi = malloc(sizeof(int) * 2 * dw);
  if (xi == NULL) { /* out of memory: nearest neighbour without tables */
    for (unsigned y = 0; y < dh; y++) {
      const uint8_t *r = src + (size_t)((uint64_t)y * sh / dh) * (size_t)sstride;
      for (unsigned x = 0; x < dw; x++) dst[(size_t)y * (size_t)dstride + x] = r[(size_t)((uint64_t)x * sw / dw) * (size_t)sstep];
    }
    return;
  }
  int *xw = xi + dw;
  const double xs = (double)sw / (double)dw, ys = (double)sh / (double)dh;
  for (unsigned x = 0; x < dw; x++) {
    double fx = ((double)x + 0.5) * xs - 0.5;
    if (fx < 0) fx = 0;
    int x0 = (int)fx;
    if (x0 > (int)sw - 1) x0 = (int)sw - 1;
    xi[x] = x0;
    xw[x] = (int)((fx - x0) * 256.0);
  }
  for (unsigned y = 0; y < dh; y++) {
    double fy = ((double)y + 0.5) * ys - 0.5;
    if (fy < 0) fy = 0;
    int y0 = (int)fy;
    if (y0 > (int)sh - 1) y0 = (int)sh - 1;
    const int y1 = y0 + 1 < (int)sh ? y0 + 1 : y0;
    const int wy = (int)((fy - y0) * 256.0);
    const uint8_t *r0 = src + (size_t)y0 * (size_t)sstride, *r1 = src + (size_t)y1 * (size_t)sstride;
    uint8_t *d = dst + (size_t)y * (size_t)dstride;
    for (unsigned x = 0; x < dw; x++) {
      const int x0 = xi[x], x1 = x0 + 1 < (int)sw ? x0 + 1 : x0, wx = xw[x];
      const int top = r0[x0 * sstep] * (256 - wx) + r0[x1 * sstep] * wx;
      const int bot = r1[x0 * sstep] * (256 - wx) + r1[x1 * sstep] * wx;
      d[x] = (uint8_t)((top * (256 - wy) + bot * wy + 32768) >> 16);
    }
  }
  free(xi);
}

static void cpu_upscale(const nv_engine_t *e, const nvvsr_image_in *in, uint8_t *const out[3], const int ostride[3]) {
  const nv_src_t *s = &e->src;
  plane_bilinear(in->plane[0], in->stride[0], 1, s->w, s->h, out[0], ostride[0], e->out_w, e->out_h);
  if (s->in_format == NVVSR_IN_NV12) {
    plane_bilinear(in->plane[1], in->stride[1], 2, s->w / 2, s->h / 2, out[1], ostride[1], e->out_w / 2, e->out_h / 2);
    plane_bilinear(in->plane[1] + 1, in->stride[1], 2, s->w / 2, s->h / 2, out[2], ostride[2], e->out_w / 2,
                   e->out_h / 2);
  } else {
    plane_bilinear(in->plane[1], in->stride[1], 1, s->w / 2, s->h / 2, out[1], ostride[1], e->out_w / 2, e->out_h / 2);
    plane_bilinear(in->plane[2], in->stride[2], 1, s->w / 2, s->h / 2, out[2], ostride[2], e->out_w / 2, e->out_h / 2);
  }
}

int nv_engine_run(nv_engine_t *e, const picture_t *in, uint8_t *const out[3], const int ostride[3]) {
  nvvsr_image_in img;
  if (!map_input(&e->src, in, &img)) return -1;

  bool try_sr = e->ctx != NULL && !e->failed;
  if (try_sr && e->fail_after >= 0 && (long)e->frames >= e->fail_after) {
    e->failed = true;
    msg_Warn(e->obj, "super resolution failed (test hook): using a plain upscale");
    try_sr = false;
  }
  if (try_sr) {
    const nvvsr_image_out o = {{out[0], out[1], out[2]}, {ostride[0], ostride[1], ostride[2]}};
    const vlc_tick_t t0 = mdate();
    const int rc = nvvsr_process(e->ctx, &img, &o);
    if (rc == NVVSR_OK) {
      const double ms = (double)(mdate() - t0) / 1000.0;
      e->frames++;
      e->ms_sum += ms;
      if (ms > e->ms_max) e->ms_max = ms;
      return 0;
    }
    e->failed = true;
    msg_Warn(e->obj, "super resolution failed (%s: %s): using a plain upscale", nvvsr_strerror(rc), nvvsr_last_error());
  }
  cpu_upscale(e, &img, out, ostride);
  e->fallback_frames++;
  return 1;
}
