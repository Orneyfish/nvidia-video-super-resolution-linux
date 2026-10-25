/*
 * filter.c - "video filter" module nvvsr: size-changing super resolution filter for transcode/sout chains
 * (`--sout '#transcode{vfilter=nvvsr{width=2560,height=1440},vcodec=...}'`).
 *
 * It follows the croppadd pattern (b_allow_fmt_out_change, fmt_out != fmt_in) and is exactly what a "video
 * filter" can do in VLC 3. It cannot make the *player's own* video output show a bigger picture: the vout
 * appends a converter after its filter chains that restores the decoder's size. Refusing to load there
 * (with a warning) keeps the GPU from upscaling pictures that are scaled straight back; use
 * `--vout=nvvsr` for playback (display.c).
 *
 * Every failure to start means "not loaded", i.e. the chain runs without this filter.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_variables.h>

#include "nvvsr_vlc.h"

struct filter_sys_t {
  nv_engine_t eng;
  bool warned;
};

static const char *const filter_options[] = {"width", "height", "quality", "min-height", "min-gain", NULL};

static picture_t *Filter(filter_t *filter, picture_t *pic) {
  filter_sys_t *sys = filter->p_sys;
  picture_t *out = filter_NewPicture(filter);
  if (out == NULL) {
    picture_Release(pic);
    return NULL;
  }
  if (out->i_planes < 3 || out->p[0].i_pitch < (int)sys->eng.out_w || out->p[0].i_lines < (int)sys->eng.out_h) {
    if (!sys->warned) msg_Warn(filter, "output picture does not fit %ux%u", sys->eng.out_w, sys->eng.out_h);
    sys->warned = true;
    picture_Release(out);
    picture_Release(pic);
    return NULL;
  }
  uint8_t *const planes[3] = {out->p[0].p_pixels, out->p[1].p_pixels, out->p[2].p_pixels};
  const int strides[3] = {out->p[0].i_pitch, out->p[1].i_pitch, out->p[2].i_pitch};
  if (nv_engine_run(&sys->eng, pic, planes, strides) < 0) {
    if (!sys->warned) msg_Warn(filter, "picture does not match the negotiated format, dropping it");
    sys->warned = true;
    picture_Release(out);
    picture_Release(pic);
    return NULL;
  }
  picture_CopyProperties(out, pic);
  picture_Release(pic);
  return out;
}

int FilterOpen(vlc_object_t *obj) {
  filter_t *filter = (filter_t *)obj;

  if (!filter->b_allow_fmt_out_change) {
    msg_Dbg(filter, "picture format change isn't allowed here");
    return VLC_EGENERIC;
  }
  const vlc_object_t *parent = obj->obj.parent;
  if (parent != NULL && parent->obj.object_type != NULL && strcmp(parent->obj.object_type, "video output") == 0) {
    msg_Warn(filter,
             "not loaded: the VLC 3 video output restores the decoder's picture size after video filters, so a "
             "super resolution filter would only waste the GPU. Use --vout=nvvsr to upscale during playback");
    return VLC_EGENERIC;
  }

  config_ChainParse(filter, NV_CFG, filter_options, filter->p_cfg);
  nv_opts_t opts;
  nv_opts_read(obj, &opts);

  nv_src_t si;
  nv_classify(&filter->fmt_in.video, &si);
  const char *why = NULL;
  unsigned ow = 0, oh = 0;
  if (!si.ok)
    why = si.why;
  else if (si.convert)
    why = "hardware surface input"; /* the ",chain" wrapper of the filter chain inserts a converter */
  else if ((int)si.h < opts.min_height)
    why = "source is smaller than " NV_CFG "min-height";
  else if (opts.width <= 0 || opts.height <= 0)
    why = "set " NV_CFG "width and " NV_CFG "height (the panel size) for the video filter";
  else if (!nv_target_size(&si, (unsigned)opts.width, (unsigned)opts.height, opts.min_gain, &ow, &oh, NULL))
    why = "source is not smaller than the target size";
  if (why != NULL) {
    msg_Dbg(filter, "not applied: %s", why);
    nv_opts_clean(&opts);
    return VLC_EGENERIC;
  }

  filter_sys_t *sys = calloc(1, sizeof *sys);
  if (sys == NULL) {
    nv_opts_clean(&opts);
    return VLC_ENOMEM;
  }
  if (nv_engine_open(&sys->eng, obj, &si, ow, oh, opts.quality) != 0) {
    free(sys);
    nv_opts_clean(&opts);
    return VLC_EGENERIC;
  }
  nv_opts_clean(&opts);

  video_format_t *out = &filter->fmt_out.video;
  const vlc_fourcc_t chroma = si.full_range && si.chroma != VLC_CODEC_NV12 ? VLC_CODEC_J420 : VLC_CODEC_I420;
  filter->fmt_out.i_codec = out->i_chroma = chroma;
  out->i_width = out->i_visible_width = ow;
  out->i_height = out->i_visible_height = oh;
  out->i_x_offset = out->i_y_offset = 0;
  out->i_sar_num = filter->fmt_in.video.i_sar_num;
  out->i_sar_den = filter->fmt_in.video.i_sar_den;
  out->b_color_range_full = filter->fmt_in.video.b_color_range_full;
  out->space = filter->fmt_in.video.space;
  out->primaries = filter->fmt_in.video.primaries;
  out->transfer = filter->fmt_in.video.transfer;

  filter->p_sys = sys;
  filter->pf_video_filter = Filter;
  msg_Dbg(filter, "%ux%u -> %ux%u", filter->fmt_in.video.i_width, filter->fmt_in.video.i_height, ow, oh);
  return VLC_SUCCESS;
}

void FilterClose(vlc_object_t *obj) {
  filter_t *filter = (filter_t *)obj;
  filter_sys_t *sys = filter->p_sys;
  nv_engine_log_summary(&sys->eng);
  nv_engine_close(&sys->eng);
  free(sys);
}
