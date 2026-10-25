/*
 * nvvsr_vlc.h - declarations shared by the parts of the VLC 3 module (libnvvsr_plugin.so).
 *
 * The module has two entry points that share this code:
 *   - "vout display" nvvsr: wraps the real video output and feeds it the super-resolved picture
 *     (display.c). This is the one that makes VLC show an upscaled picture, see vlc/README.md for
 *     why a plain "video filter" cannot do that in VLC 3.
 *   - "video filter" nvvsr: size-changing video filter for transcode/sout chains (filter.c).
 * The only header of the core used here is nvvsr.h.
 */
#ifndef NVVSR_VLC_H
#define NVVSR_VLC_H

#include <stdbool.h>
#include <stdint.h>

#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_picture.h>

#include "nvvsr.h"

#define NV_CFG "nvvsr-"

/* gettext marker used by the module descriptor (VLC's own N_ is not in the installed plugin headers) */
#ifndef N_
#define N_(str) (str)
#endif

/* ------------------------------------------------------------------------------------------- */
/* Options                                                                                      */
/* ------------------------------------------------------------------------------------------- */

typedef struct nv_opts {
  bool enable;          /* --nvvsr-enable */
  bool ac_only;         /* --nvvsr-ac-only: only on mains power */
  bool fullscreen_only; /* --nvvsr-fullscreen-only */
  int quality;          /* --nvvsr-quality: nvvsr_quality value, NVVSR_QUALITY_AUTO = -1 */
  int width, height;    /* --nvvsr-width/-height: panel size, 0 = use the display size */
  int min_height;       /* --nvvsr-min-height: smallest source height that is upscaled */
  double min_gain;      /* --nvvsr-min-gain (percent) as a fraction: source must fit the panel by >= this */
  char *vout;           /* --nvvsr-vout: the real video output module ("" = automatic) */
} nv_opts_t;

/* Reads the options of `obj` (module options and, for a filter, the config chain values). */
void nv_opts_read(vlc_object_t *obj, nv_opts_t *o);
void nv_opts_clean(nv_opts_t *o);

/* "auto", "bicubic", "low", "medium", "high", "ultra", "streaming_medium", "streaming_ultra" ->
 * nvvsr_quality; returns INT32_MIN for anything else. */
int nv_quality_parse(const char *name);

/* ------------------------------------------------------------------------------------------- */
/* Source classification and target geometry                                                    */
/* ------------------------------------------------------------------------------------------- */

typedef struct nv_src {
  bool ok;              /* usable as a source of super resolution */
  const char *why;      /* reason when !ok (constant string) */
  vlc_fourcc_t chroma;  /* chroma the engine reads: I420, J420, YV12 or NV12 */
  bool convert;         /* the decoder delivers another chroma (e.g. VAAPI surfaces) that VLC converts to `chroma` */
  int in_format;        /* NVVSR_IN_NV12 / NVVSR_IN_I420 */
  bool swap_uv;         /* YV12: plane 1 is V */
  int matrix;           /* nvvsr_matrix */
  int full_range;       /* 0 limited, 1 full */
  unsigned x, y, w, h;  /* visible rectangle inside the coded picture */
  unsigned sar_num, sar_den;
  double fps;           /* 0 = unknown */
} nv_src_t;

/* Classifies a decoder output format. Rules mirror mpv/vsr.lua: 8-bit 4:2:0 only (planar, NV12 or a
 * hardware surface VLC converts), SDR (no PQ/HLG), no BT.2020, no rotation, even geometry. */
void nv_classify(const video_format_t *fmt, nv_src_t *out);

/* Size of the super-resolved picture: the source (with its pixel aspect ratio) is fitted into the
 * panel (pw x ph), like mpv/vsr.vpy: scale = min(pw/dw, ph/dh) capped at 4, both sizes even and >= the
 * source. Returns false (no upscaling wanted) when scale < 1 + min_gain or the panel is unknown. */
bool nv_target_size(const nv_src_t *s, unsigned pw, unsigned ph, double min_gain, unsigned *ow, unsigned *oh,
                    double *scale);

/* Power supply: true on mains (or an online USB-C charger) or when nothing is known, false on battery. */
bool nv_on_ac(void);

/* ------------------------------------------------------------------------------------------- */
/* Engine: one nvvsr_ctx plus the CPU fallback                                                  */
/* ------------------------------------------------------------------------------------------- */

typedef struct nv_engine {
  vlc_object_t *obj;     /* log target */
  nvvsr_ctx *ctx;
  nv_src_t src;
  unsigned out_w, out_h;
  bool failed;           /* SR failed at run time: every later frame uses the CPU fallback until closed */
  unsigned long frames;  /* frames handled by SR */
  unsigned long fallback_frames;
  double ms_sum, ms_max;
  long fail_after;       /* test hook (NVVSR_VLC_TESTHOOK builds): fail every frame after N, -1 = never */
} nv_engine_t;

/* Creates the nvvsr context (blocking: ~10 ms when an effect is cached, ~130-220 ms the first time).
 * Safe to call from a worker thread. Returns 0 or a negative nvvsr status. */
int nv_engine_open(nv_engine_t *e, vlc_object_t *obj, const nv_src_t *s, unsigned ow, unsigned oh, int quality);
void nv_engine_close(nv_engine_t *e);

/* Upscales the visible rectangle of `in` into the I420 planes `out` (out_w x out_h luma). Never leaves the
 * output undefined: if SR fails, the frame is produced by a CPU bilinear upscale, `failed` is set and a
 * warning is logged once. Returns 0 when SR produced the frame, 1 when the fallback did, -1 when `in` does
 * not match the source description (nothing written). */
int nv_engine_run(nv_engine_t *e, const picture_t *in, uint8_t *const out[3], const int ostride[3]);

/* Summary line for the log (messages at info level). */
void nv_engine_log_summary(const nv_engine_t *e);

#endif
