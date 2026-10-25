/*
 * display.c - "vout display" module nvvsr: a transparent wrapper around the real video output that
 * feeds it a super-resolved picture while VLC is fullscreen.
 *
 * Why a display and not a video filter: the vout of VLC 3 (src/video_output/video_output.c,
 * ThreadChangeFilters) appends a converter after the video-filter chains that brings every picture back
 * to the decoder's size and chroma ("Adding a filter to compensate for format changes"), so a filter
 * cannot hand a bigger picture to the display. A vout display, on the other hand, is told the size of
 * the pictures it shows by its `source`/`fmt`, which is exactly what has to change. See vlc/README.md.
 *
 * Structure
 *   core (vout thread) --pictures at the source size--> [this module] --pictures at the SR size--> real display
 *
 *   - The real display (gl, xcb, wayland, ...) is a nested vout_display_t created here with
 *     module_need("vout display"); its events, windows and controls are forwarded to/from the core.
 *   - pass-through mode (default, windowed, ineligible source, any error): the nested display has the
 *     decoder's format; pool/prepare/display/control are plain forwards, `vd->fmt` is the nested fmt, so
 *     the core converts and renders exactly as it would without us (zero-copy VAAPI interop included).
 *   - SR mode (fullscreen + eligible): the nested display is re-opened with a source of the super
 *     resolved size; this module asks the core for 8-bit 4:2:0 pictures at the decoder's size, runs
 *     libnvvsr in prepare() and writes straight into the nested display's picture.
 *   - Mode changes use the display's "pictures invalid" protocol (VOUT_DISPLAY_EVENT_PICTURES_INVALID ->
 *     VOUT_DISPLAY_RESET_PICTURES), in which the core accepts a changed vd->fmt.
 *   - The nvvsr probe and the (slow) context creation run in a worker thread, so entering fullscreen
 *     does not stall playback for the model load.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_modules.h>
#include <vlc_mtime.h>
#include <vlc_picture_pool.h>
#include <vlc_subpicture.h>
#include <vlc_threads.h>
#include <vlc_variables.h>
#include <vlc_vout_display.h>

#include "nvvsr_vlc.h"

#define WORKER_STACK ((size_t)8 << 20)
#define NESTED_POOL 4            /* pictures requested from the real display's pool in SR mode */
#define POLL_INTERVAL VLC_TICK_FROM_MS(50)
#define SETTLE_ON VLC_TICK_FROM_MS(200)   /* a decision must hold this long before the display is reopened */
#define SETTLE_OFF VLC_TICK_FROM_MS(300)
#define AC_INTERVAL VLC_TICK_FROM_SEC(30)
#define MIN_RESIZE_GAP VLC_TICK_FROM_SEC(2)

#ifndef VLC_TICK_FROM_MS
#define VLC_TICK_FROM_MS(ms) ((vlc_tick_t)(ms) * 1000)
#define VLC_TICK_FROM_SEC(s) ((vlc_tick_t)(s) * 1000000)
#endif

/* Process wide result of nvvsr_probe(): 0 unknown, 1 usable, -1 unusable. */
static atomic_int g_probe = 0;
static atomic_flag g_probe_logged = ATOMIC_FLAG_INIT;

typedef struct {
  bool sr;
  unsigned w, h;
} nv_mode_t;

enum { JOB_PROBE = 1, JOB_ENGINE = 2 };

struct vout_display_sys_t {
  vout_display_t *vd;
  nv_opts_t opts;
  nv_src_t sinfo;        /* classification of the source given to Open */
  vlc_object_t *vout;    /* parent object, carries the "fullscreen" variable */
  bool cb_installed;
  atomic_bool fullscreen;

  /* the real display */
  vout_display_t *nested;
  bool sr_mode;
  unsigned out_w, out_h;     /* size of the nested source in SR mode */
  unsigned map_w, map_h;     /* same, read by the event thread of the nested display (0 = no mapping) */
  picture_pool_t *pool;      /* our pool in SR mode (pictures at the decoder's size) */
  picture_pool_t *bigpool;   /* SR output pictures when a chroma conversion towards the nested display is needed */
  filter_chain_t *conv;
  picture_t *shown;          /* picture prepared for nested->display */

  /* SR engine, valid for eng_w x eng_h while eng_valid */
  nv_engine_t eng;
  bool eng_valid;

  /* worker thread */
  struct {
    pthread_t th;
    bool running;
    atomic_int done;
    int kind;
    int rc;
    char err[256];
    unsigned w, h;
    nv_engine_t eng;
  } job;

  /* decision state (vout thread only) */
  nv_mode_t want;            /* what the policy wants right now ... */
  vlc_tick_t want_since;     /* ... since when */
  nv_mode_t desired;         /* mode requested from the core, valid while reconf_requested */
  bool reconf_requested;
  vlc_tick_t reconf_at;
  vlc_tick_t last_switch;    /* when the last mode switch finished */
  atomic_bool nested_invalid; /* the real display asked for new pictures (set by its event thread) */
  picture_pool_t *npool;     /* the real display's pool in SR mode */
  vlc_tick_t next_poll;
  vlc_tick_t ac_next;
  bool ac_ok;
  bool sr_failed;            /* SR cannot be used on this vout any more */
  const char *why_off;       /* last reason SR is not wanted, for the log */
  unsigned interlaced_count;
  bool interlaced;
  bool warned_no_pic;
  bool warned_mismatch;
};

/* ------------------------------------------------------------------------------------------- */
/* Forwarding helpers for the nested display                                                    */
/* ------------------------------------------------------------------------------------------- */

static vout_window_t *NestedNewWindow(vout_display_t *n, unsigned type) {
  return vout_display_NewWindow(n->owner.sys, type);
}

static void NestedDelWindow(vout_display_t *n, vout_window_t *window) {
  vout_display_DeleteWindow(n->owner.sys, window);
}

/* Mouse positions are expressed in the units of the display's source: map the SR size back to the
 * decoder's picture. */
static void MapMouse(const vout_display_sys_t *sys, int *x, int *y) {
  if (sys->map_w == 0 || sys->map_h == 0) return;
  *x = (int)((int64_t)sys->sinfo.x + (int64_t)*x * sys->sinfo.w / sys->map_w);
  *y = (int)((int64_t)sys->sinfo.y + (int64_t)*y * sys->sinfo.h / sys->map_h);
}

static void NestedEvent(vout_display_t *n, int event, va_list args) {
  vout_display_t *vd = n->owner.sys;
  vout_display_sys_t *sys = vd->sys;
  switch (event) {
    case VOUT_DISPLAY_EVENT_MOUSE_STATE: {
      int x = va_arg(args, int), y = va_arg(args, int);
      const int mask = va_arg(args, int);
      MapMouse(sys, &x, &y);
      vout_display_SendEventMouseState(vd, x, y, mask);
      break;
    }
    case VOUT_DISPLAY_EVENT_MOUSE_MOVED: {
      int x = va_arg(args, int), y = va_arg(args, int);
      MapMouse(sys, &x, &y);
      vout_display_SendEventMouseMoved(vd, x, y);
      break;
    }
    case VOUT_DISPLAY_EVENT_PICTURES_INVALID:
      atomic_store(&sys->nested_invalid, true);
      vd->owner.event(vd, event, args);
      break;
    default:
      vd->owner.event(vd, event, args);
      break;
  }
}

/* Format of the nested display's source: the decoder's current source (crop and aspect ratio the user may
 * have applied included), or in SR mode the same picture at the SR size. */
static void FillNestedSource(const vout_display_sys_t *sys, bool sr, unsigned ow, unsigned oh, video_format_t *out) {
  const video_format_t *cur = &sys->vd->source;
  video_format_Copy(out, cur);
  if (!sr) return;
  const nv_src_t *s = &sys->sinfo;
  out->i_chroma = s->full_range && s->chroma != VLC_CODEC_NV12 ? VLC_CODEC_J420 : VLC_CODEC_I420;
  out->i_width = out->i_visible_width = ow;
  out->i_height = out->i_visible_height = oh;
  out->i_x_offset = out->i_y_offset = 0;
  out->i_bits_per_pixel = 12;
  free(out->p_palette);
  out->p_palette = NULL;
  /* A user crop is expressed against the decoder's picture: scale it. */
  const int64_t cx = (int64_t)cur->i_x_offset - (int64_t)s->x, cy = (int64_t)cur->i_y_offset - (int64_t)s->y;
  if (cur->i_visible_width != s->w || cur->i_visible_height != s->h || cx != 0 || cy != 0) {
    int64_t nx = cx * ow / s->w, ny = cy * oh / s->h;
    int64_t nw = (int64_t)cur->i_visible_width * ow / s->w, nh = (int64_t)cur->i_visible_height * oh / s->h;
    if (nx < 0) nx = 0;
    if (ny < 0) ny = 0;
    if (nx > (int64_t)ow - 2) nx = (int64_t)ow - 2;
    if (ny > (int64_t)oh - 2) ny = (int64_t)oh - 2;
    if (nw < 2) nw = 2;
    if (nh < 2) nh = 2;
    if (nx + nw > (int64_t)ow) nw = (int64_t)ow - nx;
    if (ny + nh > (int64_t)oh) nh = (int64_t)oh - ny;
    out->i_x_offset = (unsigned)nx;
    out->i_y_offset = (unsigned)ny;
    out->i_visible_width = (unsigned)nw;
    out->i_visible_height = (unsigned)nh;
  }
}

static void NestedClose(vout_display_sys_t *sys) {
  if (sys->shown != NULL) {
    picture_Release(sys->shown);
    sys->shown = NULL;
  }
  if (sys->conv != NULL) {
    filter_chain_Delete(sys->conv);
    sys->conv = NULL;
  }
  if (sys->bigpool != NULL) {
    picture_pool_Release(sys->bigpool);
    sys->bigpool = NULL;
  }
  if (sys->pool != NULL) {
    picture_pool_Release(sys->pool);
    sys->pool = NULL;
  }
  sys->map_w = sys->map_h = 0;
  sys->npool = NULL;
  vout_display_t *n = sys->nested;
  if (n != NULL) {
    sys->nested = NULL;
    module_unneed(n, n->module);
    video_format_Clean(&n->source);
    video_format_Clean(&n->fmt);
    vlc_object_release(n);
  }
  sys->sr_mode = false;
}

/* Opens the real display with a source of the decoder's size (sr=false) or of ow x oh (sr=true). */
static int NestedOpen(vout_display_t *vd, vout_display_sys_t *sys, bool sr, unsigned ow, unsigned oh) {
  vout_display_t *n = vlc_object_create(vd, sizeof *n);
  if (n == NULL) return -1;
  memset((char *)n + sizeof n->obj, 0, sizeof *n - sizeof n->obj);
  var_Create(n, "nvvsr-nested", VLC_VAR_BOOL); /* our own Open refuses to wrap itself */
  FillNestedSource(sys, sr, ow, oh, &n->source);
  video_format_Copy(&n->fmt, &n->source);
  n->fmt.i_sar_num = n->fmt.i_sar_den = 0;
  n->cfg = vd->cfg;
  n->owner.sys = vd;
  n->owner.event = NestedEvent;
  n->owner.window_new = NestedNewWindow;
  n->owner.window_del = NestedDelWindow;

  sys->map_w = sr ? ow : 0;
  sys->map_h = sr ? oh : 0;
  n->module = module_need(n, "vout display", sys->opts.vout, false); /* a named module is only a preference */
  if (n->module == NULL) {
    sys->map_w = sys->map_h = 0;
    video_format_Clean(&n->source);
    video_format_Clean(&n->fmt);
    vlc_object_release(n);
    return -1;
  }
  sys->nested = n;
  sys->sr_mode = sr;
  sys->out_w = sr ? ow : 0;
  sys->out_h = sr ? oh : 0;
  return 0;
}

static picture_t *ConvBufferNew(filter_t *filter) {
  vout_display_sys_t *sys = filter->owner.sys;
  return sys->npool != NULL ? picture_pool_Get(sys->npool) : NULL;
}

static bool SameLayout(const video_format_t *a, const video_format_t *b) {
  const bool i420 = (a->i_chroma == VLC_CODEC_I420 || a->i_chroma == VLC_CODEC_J420) &&
                    (b->i_chroma == VLC_CODEC_I420 || b->i_chroma == VLC_CODEC_J420);
  return (a->i_chroma == b->i_chroma || i420) && a->i_width == b->i_width && a->i_height == b->i_height;
}

/* SR mode only: allocate what is needed between the engine and the nested display. */
static int SrSetup(vout_display_t *vd, vout_display_sys_t *sys) {
  vout_display_t *n = sys->nested;
  sys->npool = n->pool != NULL ? n->pool(n, NESTED_POOL) : NULL;
  if (sys->npool == NULL) return -1;
  if (SameLayout(&n->fmt, &n->source)) return 0; /* the engine writes into the display's pictures */

  msg_Dbg(vd, "the real display wants %4.4s, converting from %4.4s", (const char *)&n->fmt.i_chroma,
          (const char *)&n->source.i_chroma);
  filter_owner_t owner = {.sys = sys, .video = {.buffer_new = ConvBufferNew}};
  sys->conv = filter_chain_NewVideo(vd, false, &owner);
  if (sys->conv == NULL) return -1;
  es_format_t src, dst;
  video_format_t vs = n->source, vf = n->fmt;
  vs.i_sar_num = vs.i_sar_den = vf.i_sar_num = vf.i_sar_den = 0;
  es_format_InitFromVideo(&src, &vs);
  es_format_InitFromVideo(&dst, &vf);
  filter_chain_Reset(sys->conv, &src, &dst);
  const int rc = filter_chain_AppendConverter(sys->conv, &src, &dst);
  es_format_Clean(&src);
  es_format_Clean(&dst);
  if (rc != 0) {
    msg_Warn(vd, "no converter from %4.4s to %4.4s", (const char *)&n->source.i_chroma, (const char *)&n->fmt.i_chroma);
    return -1;
  }
  sys->bigpool = picture_pool_NewFromFormat(&n->source, NESTED_POOL);
  return sys->bigpool != NULL ? 0 : -1;
}

/* vd->fmt and vd->info follow the current mode. */
static void SetOuterFormat(vout_display_t *vd, vout_display_sys_t *sys) {
  vout_display_t *n = sys->nested;
  video_format_t nf;
  if (sys->sr_mode) {
    video_format_Copy(&nf, &vd->source);
    nf.i_chroma = sys->sinfo.chroma;
    free(nf.p_palette);
    nf.p_palette = NULL;
  } else {
    video_format_Copy(&nf, &n->fmt);
  }
  nf.i_sar_num = nf.i_sar_den = 0;
  video_format_Clean(&vd->fmt);
  vd->fmt = nf;

  vd->info = n->info;
  vd->info.has_pictures_invalid = true; /* required for mode changes; also disables direct rendering */
  if (sys->sr_mode) {
    vd->info.is_slow = false;
    vd->info.subpicture_chromas = NULL; /* subtitles are blended into the decoder's picture, before SR */
  }
}

/* ------------------------------------------------------------------------------------------- */
/* Worker thread: nvvsr_probe() and nvvsr_create()                                              */
/* ------------------------------------------------------------------------------------------- */

static void *JobMain(void *data) {
  vout_display_sys_t *sys = data;
  if (sys->job.kind == JOB_PROBE) {
    const int rc = nvvsr_probe();
    sys->job.rc = rc;
    strncpy(sys->job.err, rc == NVVSR_OK ? "" : nvvsr_last_error(), sizeof sys->job.err - 1);
    atomic_store(&g_probe, rc == NVVSR_OK ? 1 : -1);
  } else {
    sys->job.rc = nv_engine_open(&sys->job.eng, VLC_OBJECT(sys->vd), &sys->sinfo, sys->job.w, sys->job.h,
                                 sys->opts.quality);
  }
  atomic_store(&sys->job.done, 1);
  return NULL;
}

static bool JobStart(vout_display_sys_t *sys, int kind, unsigned w, unsigned h) {
  if (sys->job.running) return false;
  sys->job.kind = kind;
  sys->job.w = w;
  sys->job.h = h;
  sys->job.rc = 0;
  sys->job.err[0] = '\0';
  atomic_store(&sys->job.done, 0);
  /* nvvsr_create() crashes inside libnvidia-ngx on VLC's 1 MiB thread stacks (it needs >= 2 MiB), so the worker
   * gets its own large stack instead of using vlc_clone(). */
  pthread_attr_t attr;
  bool started = false;
  if (pthread_attr_init(&attr) == 0) {
    started = pthread_attr_setstacksize(&attr, WORKER_STACK) == 0 && pthread_create(&sys->job.th, &attr, JobMain, sys) == 0;
    pthread_attr_destroy(&attr);
  }
  if (!started) {
    /* no thread: treat as a failure of the feature, not of playback */
    sys->sr_failed = true;
    return false;
  }
  sys->job.running = true;
  return true;
}

static void EngineRelease(vout_display_sys_t *sys) {
  if (sys->eng_valid) {
    nv_engine_log_summary(&sys->eng);
    nv_engine_close(&sys->eng);
    sys->eng_valid = false;
  }
}

/* Post-processing of a finished worker job (the thread has been joined). */
static void JobFinish(vout_display_t *vd, vout_display_sys_t *sys) {
  sys->job.running = false;
  if (sys->job.kind == JOB_PROBE) {
    if (sys->job.rc != NVVSR_OK) {
      if (!atomic_flag_test_and_set(&g_probe_logged))
        msg_Warn(vd, "NVIDIA super resolution is not available (%s: %s): playing without it", nvvsr_strerror(sys->job.rc),
                 sys->job.err);
      sys->sr_failed = true;
    } else {
      msg_Dbg(vd, "libnvvsr %s is usable", nvvsr_version());
    }
  } else if (sys->job.rc == NVVSR_OK) {
    EngineRelease(sys);
    sys->eng = sys->job.eng;
    sys->eng_valid = true;
  } else {
    sys->sr_failed = true; /* nv_engine_open has logged the reason */
    msg_Warn(vd, "super resolution is disabled for this video, playing it unchanged");
  }
}

/* Called from the vout thread once the worker has set job.done. */
static void JobReap(vout_display_t *vd, vout_display_sys_t *sys) {
  pthread_join(sys->job.th, NULL);
  JobFinish(vd, sys);
}

/* ------------------------------------------------------------------------------------------- */
/* Decision                                                                                     */
/* ------------------------------------------------------------------------------------------- */

static bool WantSr(vout_display_t *vd, vout_display_sys_t *sys, vlc_tick_t now, unsigned *w, unsigned *h) {
  sys->why_off = NULL;
  if (sys->sr_failed || atomic_load(&g_probe) < 0) {
    sys->why_off = "unavailable";
    return false;
  }
  if (sys->interlaced) {
    sys->why_off = "interlaced video";
    return false;
  }
  if (sys->opts.fullscreen_only && !atomic_load(&sys->fullscreen)) {
    sys->why_off = "not fullscreen";
    return false;
  }
  if (sys->opts.ac_only) {
    if (now >= sys->ac_next) {
      const bool ok = nv_on_ac();
      if (ok != sys->ac_ok) msg_Dbg(vd, "power source: %s", ok ? "mains" : "battery");
      sys->ac_ok = ok;
      sys->ac_next = now + AC_INTERVAL;
    }
    if (!sys->ac_ok) {
      sys->why_off = "on battery";
      return false;
    }
  }
  unsigned pw = (unsigned)sys->opts.width, ph = (unsigned)sys->opts.height;
  if (pw == 0 || ph == 0) {
    pw = vd->cfg->display.width;
    ph = vd->cfg->display.height;
  }
  if (!nv_target_size(&sys->sinfo, pw, ph, sys->opts.min_gain, w, h, NULL)) {
    sys->why_off = "source is not smaller than the display";
    return false;
  }
  /* Small changes of the display size do not justify reopening the display. */
  if (sys->sr_mode && (*w > sys->out_w ? *w - sys->out_w : sys->out_w - *w) * 50 < sys->out_w &&
      (*h > sys->out_h ? *h - sys->out_h : sys->out_h - *h) * 50 < sys->out_h) {
    *w = sys->out_w;
    *h = sys->out_h;
  }
  return true;
}

static bool SameMode(const nv_mode_t *a, const nv_mode_t *b) {
  return a->sr == b->sr && (!a->sr || (a->w == b->w && a->h == b->h));
}

/* Runs in the vout thread from the display's manage(): every POLL_INTERVAL it evaluates the policy, drives
 * the worker (probe, engine creation) and, once a decision has been stable for a moment and everything it
 * needs is ready, asks the core to reset the pictures, which ends in Reconfigure(). */
static void Poll(vout_display_t *vd) {
  vout_display_sys_t *sys = vd->sys;
  const vlc_tick_t now = mdate();
  if (now < sys->next_poll) return;
  sys->next_poll = now + POLL_INTERVAL;

  if (sys->job.running && atomic_load(&sys->job.done)) JobReap(vd, sys);

  if (atomic_load(&g_probe) == 0 && !sys->sr_failed) {
    if (!sys->job.running) JobStart(sys, JOB_PROBE, 0, 0);
    return; /* the policy needs the probe result first */
  }

  unsigned w = 0, h = 0;
  nv_mode_t want = {false, 0, 0};
  want.sr = WantSr(vd, sys, now, &w, &h);
  if (want.sr) {
    want.w = w;
    want.h = h;
  }
  if (!SameMode(&want, &sys->want)) {
    sys->want = want;
    sys->want_since = now;
  }
  const nv_mode_t cur = {sys->sr_mode, sys->out_w, sys->out_h};

  /* Engine management: created ahead of the switch, released when it is not needed. */
  if (!sys->job.running) {
    if (want.sr) {
      const bool matches = sys->eng_valid && sys->eng.out_w == w && sys->eng.out_h == h;
      if (!matches && !(sys->sr_mode && sys->eng_valid)) {
        EngineRelease(sys);
        JobStart(sys, JOB_ENGINE, w, h);
      }
    } else if (!sys->sr_mode && sys->eng_valid) {
      EngineRelease(sys);
    }
  }

  nv_mode_t target = cur;
  /* A failure is not a decision to be debounced: leave SR at once. */
  const vlc_tick_t settle = want.sr ? SETTLE_ON : (sys->sr_failed || sys->interlaced ? 0 : SETTLE_OFF);
  if (now - sys->want_since >= settle) {
    if (!want.sr)
      target = want;
    else if (sys->eng_valid && sys->eng.out_w == w && sys->eng.out_h == h)
      target = want;
    else if (sys->sr_mode)
      target.sr = false; /* a different size is wanted: leave SR, the engine is rebuilt in the pass-through mode */
  }

  if (sys->reconf_requested && now - sys->reconf_at > VLC_TICK_FROM_SEC(2)) sys->reconf_requested = false;
  /* Resizing a window must not reopen the video output over and over: a change from one SR size to another
   * waits for the previous switch to be MIN_RESIZE_GAP old (switching SR on or off is never delayed). */
  if (target.sr && cur.sr && now - sys->last_switch < MIN_RESIZE_GAP) target = cur;
  if (!SameMode(&target, &cur) && !sys->reconf_requested) {
    sys->desired = target;
    sys->reconf_requested = true;
    sys->reconf_at = now;
    if (target.sr)
      msg_Info(vd, "super resolution on: %ux%u -> %ux%u", sys->sinfo.w, sys->sinfo.h, target.w, target.h);
    else
      msg_Info(vd, "super resolution off (%s)", sys->why_off != NULL ? sys->why_off : "display size changed");
    vout_display_SendEventPicturesInvalid(vd);
  }
}

static int FullscreenCb(vlc_object_t *obj, const char *name, vlc_value_t oldv, vlc_value_t newv, void *data) {
  (void)obj;
  (void)name;
  (void)oldv;
  vout_display_sys_t *sys = data;
  atomic_store(&sys->fullscreen, newv.b_bool);
  return VLC_SUCCESS;
}

/* ------------------------------------------------------------------------------------------- */
/* vout_display_t callbacks                                                                     */
/* ------------------------------------------------------------------------------------------- */

/* After a failed reopen (Reconfigure) there is no real display until the core has processed our close request:
 * every callback must tolerate that. */
static picture_pool_t *Pool(vout_display_t *vd, unsigned count) {
  vout_display_sys_t *sys = vd->sys;
  vout_display_t *n = sys->nested;
  if (n == NULL) return NULL;
  if (!sys->sr_mode) return n->pool != NULL ? n->pool(n, count) : NULL;
  if (sys->pool == NULL) sys->pool = picture_pool_NewFromFormat(&vd->fmt, count > 3 ? count : 3);
  return sys->pool;
}

/* SR mode: upscale `pic` into a picture of the nested display. */
static void SrPrepare(vout_display_t *vd, vout_display_sys_t *sys, picture_t *pic, subpicture_t *sub) {
  vout_display_t *n = sys->nested;
  if (sys->shown != NULL) {
    picture_Release(sys->shown);
    sys->shown = NULL;
  }
  picture_t *dst = sys->conv != NULL ? picture_pool_Get(sys->bigpool) : picture_pool_Get(sys->npool);
  if (dst == NULL) {
    if (!sys->warned_no_pic) msg_Warn(vd, "no picture available for the super resolved frame, dropping frames");
    sys->warned_no_pic = true;
    return;
  }
  if (dst->i_planes < 3 || dst->p[0].i_pitch < (int)sys->out_w || dst->p[0].i_lines < (int)sys->out_h ||
      dst->p[1].i_pitch < (int)(sys->out_w / 2) || dst->p[2].i_pitch < (int)(sys->out_w / 2)) {
    msg_Warn(vd, "the real display's picture does not fit the super resolved frame: back to the source size");
    picture_Release(dst);
    sys->sr_failed = true;
    return;
  }
  uint8_t *const out[3] = {dst->p[0].p_pixels, dst->p[1].p_pixels, dst->p[2].p_pixels};
  const int stride[3] = {dst->p[0].i_pitch, dst->p[1].i_pitch, dst->p[2].i_pitch};
  const int r = nv_engine_run(&sys->eng, pic, out, stride);
  if (r < 0) {
    if (!sys->warned_mismatch) msg_Warn(vd, "picture does not match the source format: back to the source size");
    sys->warned_mismatch = true;
    picture_Release(dst);
    sys->sr_failed = true;
    return;
  }
  if (sys->eng.failed) sys->sr_failed = true; /* this frame came from the CPU fallback; Poll() goes back to pass-through */
  picture_CopyProperties(dst, pic);
  if (sys->conv != NULL) {
    dst = filter_chain_VideoFilter(sys->conv, dst); /* consumes dst */
    if (dst == NULL) {
      sys->sr_failed = true;
      return;
    }
  }
  sys->shown = dst;
  if (n->prepare != NULL) n->prepare(n, dst, sub);
}

static void Prepare(vout_display_t *vd, picture_t *pic, subpicture_t *sub) {
  vout_display_sys_t *sys = vd->sys;
  vout_display_t *n = sys->nested;
  if (n == NULL) return;
  /* Interlaced pictures (no deinterlacing filter in front) are not upscaled: the neural network would amplify
   * the combing. Watched in both modes so that SR is not even started for such a stream. */
  if (!pic->b_progressive && !sys->interlaced && ++sys->interlaced_count >= 3) {
    sys->interlaced = true;
    msg_Info(vd, "interlaced video: super resolution is not applied");
  }
  if (!sys->sr_mode) {
    if (n->prepare != NULL) n->prepare(n, pic, sub);
    return;
  }
  SrPrepare(vd, sys, pic, sub);
}

static void Display(vout_display_t *vd, picture_t *pic, subpicture_t *sub) {
  vout_display_sys_t *sys = vd->sys;
  vout_display_t *n = sys->nested;
  if (n == NULL) {
    picture_Release(pic);
    if (sub != NULL) subpicture_Delete(sub);
    return;
  }
  if (!sys->sr_mode) {
    n->display(n, pic, sub);
    return;
  }
  picture_t *shown = sys->shown;
  sys->shown = NULL;
  picture_Release(pic);
  if (shown != NULL)
    n->display(n, shown, sub);
  else if (sub != NULL)
    subpicture_Delete(sub);
}

static int NestedControl(vout_display_t *n, int query, ...) {
  if (n->control == NULL) return VLC_EGENERIC;
  va_list ap;
  va_start(ap, query);
  const int rc = n->control(n, query, ap);
  va_end(ap);
  return rc;
}

/* VOUT_DISPLAY_RESET_PICTURES: apply the mode chosen by Poll(), reopening the real display if needed. */
static int Reconfigure(vout_display_t *vd) {
  vout_display_sys_t *sys = vd->sys;
  if (sys->nested == NULL) return VLC_EGENERIC;
  const nv_mode_t cur = {sys->sr_mode, sys->out_w, sys->out_h};
  nv_mode_t want = sys->reconf_requested ? sys->desired : cur;
  const bool nested_asked = atomic_exchange(&sys->nested_invalid, false);
  sys->reconf_requested = false;
  if (sys->sr_failed) want.sr = false;
  if (want.sr && !(sys->eng_valid && sys->eng.out_w == want.w && sys->eng.out_h == want.h)) want.sr = false;

  if (SameMode(&want, &cur)) {
    /* No mode change: only the real display needs fresh pictures (device lost, ...). */
    vout_display_t *n = sys->nested;
    int rc = VLC_SUCCESS;
    if (nested_asked && n->info.has_pictures_invalid) {
      rc = NestedControl(n, VOUT_DISPLAY_RESET_PICTURES);
      if (sys->pool != NULL) {
        picture_pool_Release(sys->pool);
        sys->pool = NULL;
      }
      if (sys->sr_mode) {
        if (sys->conv != NULL) {
          filter_chain_Delete(sys->conv);
          sys->conv = NULL;
        }
        if (sys->bigpool != NULL) {
          picture_pool_Release(sys->bigpool);
          sys->bigpool = NULL;
        }
        if (SrSetup(vd, sys) != 0) {
          msg_Warn(vd, "cannot adapt the super resolved picture to the real display: back to the source size");
          sys->sr_failed = true;
        }
      }
      SetOuterFormat(vd, sys);
    }
    return rc;
  }

  const vlc_tick_t t0 = mdate();
  NestedClose(sys);
  bool ok = false;
  if (want.sr) {
    if (NestedOpen(vd, sys, true, want.w, want.h) == 0) {
      if (SrSetup(vd, sys) == 0)
        ok = true;
      else
        NestedClose(sys);
    }
    if (!ok) {
      msg_Warn(vd, "cannot show %ux%u pictures on the real display: staying at the source size", want.w, want.h);
      sys->sr_failed = true;
    }
  }
  if (!ok) {
    if (NestedOpen(vd, sys, false, 0, 0) != 0) {
      msg_Err(vd, "cannot reopen the video output");
      vout_display_SendEventClose(vd);
      return VLC_EGENERIC;
    }
    if (sys->eng_valid) EngineRelease(sys);
  }
  sys->want = (nv_mode_t){sys->sr_mode, sys->out_w, sys->out_h};
  sys->last_switch = sys->want_since = mdate();
  SetOuterFormat(vd, sys);
  msg_Dbg(vd, "video output reopened for %s in %.0f ms", sys->sr_mode ? "super resolution" : "the source size",
          (double)(sys->last_switch - t0) / 1000.0);
  return VLC_SUCCESS;
}

static void SyncSource(vout_display_t *vd, vout_display_sys_t *sys, bool crop) {
  vout_display_t *n = sys->nested;
  if (!sys->sr_mode) {
    if (crop)
      video_format_CopyCrop(&n->source, &vd->source);
    else {
      n->source.i_sar_num = vd->source.i_sar_num;
      n->source.i_sar_den = vd->source.i_sar_den;
    }
    return;
  }
  if (crop) {
    video_format_t tmp;
    FillNestedSource(sys, true, sys->out_w, sys->out_h, &tmp);
    video_format_CopyCrop(&n->source, &tmp);
    video_format_Clean(&tmp);
  } else {
    n->source.i_sar_num = vd->source.i_sar_num;
    n->source.i_sar_den = vd->source.i_sar_den;
  }
}

static int Control(vout_display_t *vd, int query, va_list args) {
  vout_display_sys_t *sys = vd->sys;
  vout_display_t *n = sys->nested;
  if (n == NULL) return VLC_EGENERIC;
  switch (query) {
    case VOUT_DISPLAY_RESET_PICTURES:
      return Reconfigure(vd);
    case VOUT_DISPLAY_CHANGE_SOURCE_ASPECT:
      SyncSource(vd, sys, false);
      break;
    case VOUT_DISPLAY_CHANGE_SOURCE_CROP:
      SyncSource(vd, sys, true);
      break;
    default:
      break;
  }
  return n->control != NULL ? n->control(n, query, args) : VLC_EGENERIC;
}

static void Manage(vout_display_t *vd) {
  vout_display_sys_t *sys = vd->sys;
  vout_display_t *n = sys->nested;
  if (n == NULL) return;
  if (n->manage != NULL) n->manage(n);
  Poll(vd);
}

/* ------------------------------------------------------------------------------------------- */
/* Open / Close                                                                                 */
/* ------------------------------------------------------------------------------------------- */

int DisplayOpen(vlc_object_t *obj) {
  vout_display_t *vd = (vout_display_t *)obj;
  if (var_Type(vd, "nvvsr-nested") != 0) return VLC_EGENERIC; /* we are being offered as the real display */

  /* When VLC picks the output automatically, step aside whenever super resolution cannot help (the normal output
   * is then used with no wrapper at all). When the user asked for --vout=nvvsr explicitly there is no "next"
   * output to fall back to, so wrap anyway and just never upscale. */
  const bool forced = obj->obj.force;
  nv_opts_t opts;
  nv_opts_read(obj, &opts);

  nv_src_t si;
  nv_classify(&vd->source, &si);
  const char *why = NULL;
  if (!opts.enable)
    why = "disabled by --no-" NV_CFG "enable";
  else if (atomic_load(&g_probe) < 0)
    why = "super resolution is not usable on this machine";
  else if (!si.ok)
    why = si.why;
  else if ((int)si.h < opts.min_height)
    why = "source is smaller than " NV_CFG "min-height";
  else if (opts.width > 0 && opts.height > 0) {
    unsigned ow, oh;
    if (!nv_target_size(&si, (unsigned)opts.width, (unsigned)opts.height, opts.min_gain, &ow, &oh, NULL))
      why = "source is not smaller than the panel";
  }
  if (why != NULL) {
    msg_Dbg(vd, "super resolution not applicable: %s (%4.4s %ux%u)%s", why, (const char *)&vd->source.i_chroma, si.w,
            si.h, forced ? ", passing the video through" : ", not wrapping the video output");
    if (!forced) {
      nv_opts_clean(&opts);
      return VLC_EGENERIC;
    }
  }

  vout_display_sys_t *sys = calloc(1, sizeof *sys);
  if (sys == NULL) {
    nv_opts_clean(&opts);
    return VLC_ENOMEM;
  }
  sys->vd = vd;
  sys->opts = opts;
  sys->sinfo = si;
  sys->sr_failed = why != NULL; /* forced and not applicable: a plain pass-through wrapper */
  sys->ac_ok = true;
  sys->vout = obj->obj.parent;
  atomic_init(&sys->fullscreen, false);
  atomic_init(&sys->nested_invalid, false);
  atomic_init(&sys->job.done, 0);
  vd->sys = sys;

  if (sys->vout != NULL && (var_Type(sys->vout, "fullscreen") & VLC_VAR_TYPE) == VLC_VAR_BOOL) {
    atomic_store(&sys->fullscreen, var_GetBool(sys->vout, "fullscreen"));
    var_AddCallback(sys->vout, "fullscreen", FullscreenCb, sys);
    sys->cb_installed = true;
  } else {
    atomic_store(&sys->fullscreen, var_InheritBool(obj, "fullscreen"));
  }

  if (NestedOpen(vd, sys, false, 0, 0) != 0) {
    msg_Dbg(vd, "the real video output could not be opened");
    if (sys->cb_installed) var_DelCallback(sys->vout, "fullscreen", FullscreenCb, sys);
    nv_opts_clean(&sys->opts);
    free(sys);
    vd->sys = NULL;
    return VLC_EGENERIC;
  }
  vout_display_t *n = sys->nested;
  if (n->display == NULL || n->control == NULL || n->pool == NULL) {
    msg_Warn(vd, "the real video output is incomplete");
    NestedClose(sys);
    if (sys->cb_installed) var_DelCallback(sys->vout, "fullscreen", FullscreenCb, sys);
    nv_opts_clean(&sys->opts);
    free(sys);
    vd->sys = NULL;
    return VLC_EGENERIC;
  }

  video_format_Clean(&vd->fmt);
  video_format_Init(&vd->fmt, 0);
  SetOuterFormat(vd, sys);
  vd->pool = Pool;
  vd->prepare = Prepare;
  vd->display = Display;
  vd->control = Control;
  vd->manage = Manage;

  if (why == NULL)
    msg_Info(vd, "wrapping the video output (%s), %ux%u, super resolution when %s", module_get_name(n->module, true),
             si.w, si.h, sys->opts.fullscreen_only ? "fullscreen" : "the display is larger");
  else
    msg_Info(vd, "passing the video through to the video output (%s): %s", module_get_name(n->module, true), why);
  return VLC_SUCCESS;
}

void DisplayClose(vlc_object_t *obj) {
  vout_display_t *vd = (vout_display_t *)obj;
  vout_display_sys_t *sys = vd->sys;
  if (sys->cb_installed) var_DelCallback(sys->vout, "fullscreen", FullscreenCb, sys);
  if (sys->job.running) {
    pthread_join(sys->job.th, NULL);
    sys->job.running = false;
    if (sys->job.kind == JOB_PROBE)
      JobFinish(vd, sys); /* a failed probe is reported even if the video output was closed meanwhile */
    else if (sys->job.rc == NVVSR_OK)
      nv_engine_close(&sys->job.eng);
  }
  NestedClose(sys);
  EngineRelease(sys);
  nv_opts_clean(&sys->opts);
  free(sys);
}
