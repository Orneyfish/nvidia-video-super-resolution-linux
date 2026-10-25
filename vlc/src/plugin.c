/*
 * plugin.c - module descriptor of libnvvsr_plugin.so (NVIDIA RTX video super resolution for VLC 3).
 *
 *   vout display "nvvsr"   (display.c)  --vout=nvvsr or picked automatically: super resolution in fullscreen
 *   video filter "nvvsr"   (filter.c)   transcode/sout chains: --sout '#transcode{vfilter=nvvsr{...}}'
 *
 * Both only use core/include/nvvsr.h (libnvvsr) and degrade to "not loaded" / pass-through on any error.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_vout_display.h>

#include "nvvsr_vlc.h"

/* Score of the vout display. With a positive score VLC offers every video to the wrapper first; it steps aside
 * (the normal output is used) whenever it cannot help. 0 = only used when selected with --vout=nvvsr. */
#ifndef NVVSR_VLC_DISPLAY_SCORE
#define NVVSR_VLC_DISPLAY_SCORE 300
#endif

int DisplayOpen(vlc_object_t *);
void DisplayClose(vlc_object_t *);
int FilterOpen(vlc_object_t *);
void FilterClose(vlc_object_t *);

#define ENABLE_TEXT N_("Enable NVIDIA super resolution")
#define ENABLE_LONGTEXT \
  N_("Master switch of the nvvsr video output. When disabled VLC uses its normal video output.")
#define QUALITY_TEXT N_("Quality")
#define QUALITY_LONGTEXT \
  N_("Super resolution quality. auto picks it from the source height and frame rate (<=540p ultra, <=800p high, " \
     "otherwise medium; lower for 4K / high frame rates). bicubic uses no neural network.")
#define WIDTH_TEXT N_("Panel width")
#define WIDTH_LONGTEXT \
  N_("Width of the display in pixels. 0 uses the size of the video window (the screen while fullscreen). " \
     "Needed (with the height) for the video filter, and for headless runs.")
#define HEIGHT_TEXT N_("Panel height")
#define HEIGHT_LONGTEXT N_("Height of the display in pixels, see the width.")
#define AC_TEXT N_("Only on mains power")
#define AC_LONGTEXT \
  N_("Do not run super resolution on battery (the GPU draws about 150 W). Checked every 30 seconds.")
#define FS_TEXT N_("Only while fullscreen")
#define FS_LONGTEXT \
  N_("Upscale only while the video is fullscreen. When disabled it also runs in a window that is larger than " \
     "the video.")
#define MINH_TEXT N_("Smallest source height")
#define MINH_LONGTEXT N_("Sources lower than this many lines are left alone (very low quality sources look worse).")
#define GAIN_TEXT N_("Minimum gain (percent)")
#define GAIN_LONGTEXT \
  N_("The video must be at least this many percent smaller than the display (fitted, aspect ratio kept) to be " \
     "upscaled; otherwise the video is left alone.")
#define VOUT_TEXT N_("Real video output")
#define VOUT_LONGTEXT \
  N_("Video output module that shows the pictures (for example gl, xcb_x11, wl_shm). Empty selects it " \
     "automatically, like VLC would without nvvsr.")

static const char *const quality_values[] = {"auto", "bicubic", "low", "medium", "high", "ultra", "streaming_medium",
                                             "streaming_ultra"};
static const char *const quality_texts[] = {N_("Automatic"),   N_("Bicubic (no AI)"), N_("Low"),
                                            N_("Medium"),      N_("High"),            N_("Ultra"),
                                            N_("Streaming medium"), N_("Streaming ultra")};

vlc_module_begin()
    set_shortname(N_("NVIDIA VSR"))
    set_description(N_("NVIDIA RTX video super resolution output"))
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VOUT)
    set_capability("vout display", NVVSR_VLC_DISPLAY_SCORE)
    set_callbacks(DisplayOpen, DisplayClose)
    add_shortcut("nvvsr")

    add_bool(NV_CFG "enable", true, ENABLE_TEXT, ENABLE_LONGTEXT, false)
    add_string(NV_CFG "quality", "auto", QUALITY_TEXT, QUALITY_LONGTEXT, false)
        change_string_list(quality_values, quality_texts)
    add_bool(NV_CFG "fullscreen-only", true, FS_TEXT, FS_LONGTEXT, false)
    add_bool(NV_CFG "ac-only", true, AC_TEXT, AC_LONGTEXT, false)
    add_integer_with_range(NV_CFG "width", 0, 0, 16384, WIDTH_TEXT, WIDTH_LONGTEXT, true)
    add_integer_with_range(NV_CFG "height", 0, 0, 16384, HEIGHT_TEXT, HEIGHT_LONGTEXT, true)
    add_integer_with_range(NV_CFG "min-height", 360, 0, 4320, MINH_TEXT, MINH_LONGTEXT, true)
    add_integer_with_range(NV_CFG "min-gain", 2, 0, 100, GAIN_TEXT, GAIN_LONGTEXT, true)
    add_string(NV_CFG "vout", "", VOUT_TEXT, VOUT_LONGTEXT, true)

    add_submodule()
        set_shortname(N_("NVIDIA VSR"))
        set_description(N_("NVIDIA RTX video super resolution filter"))
        set_capability("video filter", 0)
        set_callbacks(FilterOpen, FilterClose)
        add_shortcut("nvvsr")
vlc_module_end()
