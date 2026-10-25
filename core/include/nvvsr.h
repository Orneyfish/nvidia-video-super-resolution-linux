/*
 * nvvsr.h - public C API of libnvvsr (NVIDIA Video Super Resolution core).
 *
 * This is the ONLY header front-ends (VapourSynth plugin, VLC module, ...) may use.
 * It is plain C99/C++ and includes no NVIDIA header. libnvvsr itself does not link
 * against any NVIDIA library: it dlopen()s the `nvidia-vfx` wheel libraries at run time
 * (see "Library discovery" below), so a front-end built against this header loads and
 * runs everywhere; nvvsr_probe() tells whether super resolution is actually usable.
 *
 * Data flow of one frame (all work on the GPU, host memory in/out):
 *
 *    host YUV420 (NV12/I420) --H2D--> GPU YUV --NPP--> GPU RGBA8 --VSR--> GPU RGBA8
 *        --NPP--> GPU {planar RGB | RGBA | I420} --D2H--> host planes
 *
 * Error handling: every function reports failure through its return value and never
 * throws, aborts or lets an exception cross the ABI. A front-end is expected to fall
 * back to pass-through on any non-zero status. nvvsr_last_error() returns a human
 * readable detail string for the most recent failure on the calling thread.
 *
 * Threading: libnvvsr initialisation is lazy and thread safe. A nvvsr_ctx is NOT
 * thread safe (use it from one thread at a time), but different contexts are fully
 * independent and may be used concurrently from different threads (the effect is
 * stateless, so contexts may also process frames out of order). Creating a context
 * takes ~130 ms the first time (model load) and ~10 ms when an idle effect is recycled
 * (next paragraph), so keeping a small pool of contexts is cheap.
 *
 * Effect cache: NvVFX (SDK 1.3) leaks memory when an effect is destroyed: ~12.5 MiB of host
 * memory per model load and ~215 MiB of GPU memory for every effect that coexisted with
 * another one. nvvsr_destroy() therefore does not destroy the SDK effect: it parks it
 * (up to 8, env NVVSR_CACHE_MAX; NVVSR_CACHE=0 disables) and nvvsr_create() reuses a
 * parked one (no reload; quality and sizes are plain parameters). The GPU buffers of a
 * context are freed at nvvsr_destroy(); a parked effect keeps ~200-800 MiB of VRAM
 * (model weights + workspace per image size it has served) until the process exits.
 *
 * Library discovery (first match wins):
 *   1. environment variable NVVSR_LIBDIR  = "<dirA>:<dirB>"
 *   2. first non-empty, non-'#' line of $XDG_CONFIG_HOME/nvvsr/libdir
 *      (default ~/.config/nvvsr/libdir), same colon-separated format
 * The directories are those of the nvidia-vfx wheel, i.e.
 *   <site-packages>/nvvfx/libs  and  <site-packages>/nvidia_vfx.libs
 * The list is read once, at the first call that needs the libraries. The libraries are loaded
 * like the wheel's own loader does (dlopen RTLD_GLOBAL, in dependency order); set
 * NVVSR_DLOPEN_LOCAL=1 for RTLD_LOCAL if the host process needs a clean global symbol namespace.
 */
#ifndef NVVSR_H
#define NVVSR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(__GNUC__) || defined(__clang__)
#define NVVSR_API __attribute__((visibility("default")))
#else
#define NVVSR_API
#endif

#define NVVSR_VERSION_MAJOR 0
#define NVVSR_VERSION_MINOR 1
#define NVVSR_VERSION_PATCH 0
/* Bumped on incompatible changes; compare with nvvsr_api_version() at run time. */
#define NVVSR_API_VERSION 1

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

typedef enum nvvsr_status {
  NVVSR_OK = 0,
  NVVSR_ERR_INVALID_ARG = -1,    /* NULL pointer, bad size/stride/enum, bad struct_size */
  NVVSR_ERR_UNSUPPORTED = -2,    /* valid but unsupported: 10-bit formats, odd dims, scale outside 1x..4x */
  NVVSR_ERR_NO_LIBDIR = -3,      /* neither NVVSR_LIBDIR nor ~/.config/nvvsr/libdir is set */
  NVVSR_ERR_LIBRARY = -4,        /* wheel libraries missing / failed to load / symbol missing */
  NVVSR_ERR_NO_GPU = -5,         /* no NVIDIA driver (libcuda) or no CUDA device */
  NVVSR_ERR_DRIVER = -6,         /* NVIDIA driver too old for the SDK (needs >= 570.190) */
  NVVSR_ERR_OUT_OF_MEMORY = -7,  /* GPU (or host) memory exhausted */
  NVVSR_ERR_EFFECT = -8,         /* an SDK call failed; see nvvsr_last_error() */
  NVVSR_ERR_INTERNAL = -9        /* unexpected internal failure (caught exception) */
} nvvsr_status;

/* Constant, never-NULL description of a status code ("ok", "invalid argument", ...). */
NVVSR_API const char *nvvsr_strerror(int status);

/* Detail of the most recent failure on the calling thread (SDK error text, missing
 * file name, ...). Never NULL, may be empty. Valid until the next nvvsr_* call on
 * this thread. */
NVVSR_API const char *nvvsr_last_error(void);

/* ------------------------------------------------------------------------- */
/* Enumerations                                                              */
/* ------------------------------------------------------------------------- */

/* Super-resolution quality. The numeric values (except AUTO) are the SDK's QualityLevel.
 * Cost @1080p->1440p on an RTX 3070 Ti Laptop (SR only): MEDIUM 3.6 ms, HIGH 8.2 ms,
 * ULTRA 11.5 ms; @720p: HIGH 3.7 ms, ULTRA 5.1 ms. */
typedef enum nvvsr_quality {
  NVVSR_QUALITY_AUTO = -1,            /* nvvsr_auto_quality(in_h, fps) */
  NVVSR_QUALITY_BICUBIC = 0,          /* plain GPU bicubic, no neural network (fast, also a safe floor) */
  NVVSR_QUALITY_LOW = 1,
  NVVSR_QUALITY_MEDIUM = 2,
  NVVSR_QUALITY_HIGH = 3,
  NVVSR_QUALITY_ULTRA = 4,
  NVVSR_QUALITY_STREAMING_MEDIUM = 21,  /* tuned for compressed/low-bitrate streams */
  NVVSR_QUALITY_STREAMING_ULTRA = 23
} nvvsr_quality;

typedef enum nvvsr_in_format {
  NVVSR_IN_NV12 = 0,   /* 8-bit 4:2:0: plane[0]=Y, plane[1]=interleaved UV, plane[2] unused */
  NVVSR_IN_I420 = 1,   /* 8-bit 4:2:0: plane[0]=Y, plane[1]=U, plane[2]=V (YUV420P) */
  NVVSR_IN_P010 = 2,   /* reserved (10-bit): returns NVVSR_ERR_UNSUPPORTED */
  NVVSR_IN_I42010 = 3  /* reserved (10-bit): returns NVVSR_ERR_UNSUPPORTED */
} nvvsr_in_format;

typedef enum nvvsr_out_format {
  NVVSR_OUT_RGB24_PLANAR = 0, /* 3 separate 8-bit planes R,G,B (plane[0..2]), e.g. VapourSynth RGB24 */
  NVVSR_OUT_RGBA_CHUNKY = 1,  /* plane[0]: R,G,B,A bytes per pixel, A is always 255 */
  NVVSR_OUT_I420 = 2,         /* 8-bit planar YUV420 (Y,U,V); out_w and out_h must be even */
  NVVSR_OUT_RGB48_PLANAR = 3, /* reserved (16-bit): returns NVVSR_ERR_UNSUPPORTED */
  NVVSR_OUT_I42010 = 4        /* reserved (10-bit): returns NVVSR_ERR_UNSUPPORTED */
} nvvsr_out_format;

/* YUV<->RGB matrix. Applies to the input conversion and, for NVVSR_OUT_I420, to the
 * output conversion. */
typedef enum nvvsr_matrix {
  NVVSR_MATRIX_AUTO = 0,   /* BT.709 if in_w >= 1280 or in_h > 576, else BT.601 (mpv/ffmpeg rule) */
  NVVSR_MATRIX_BT601 = 1,
  NVVSR_MATRIX_BT709 = 2,
  NVVSR_MATRIX_BT2020 = 3  /* reserved: returns NVVSR_ERR_UNSUPPORTED */
} nvvsr_matrix;

enum {
  /* Collect per-stage timings (nvvsr_last_timing). Adds a stream synchronisation after
   * every stage, so only enable it for measurements. */
  NVVSR_FLAG_TIMING = 1u << 0
};

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

typedef struct nvvsr_config {
  uint32_t struct_size;  /* set by nvvsr_config_init(); fields beyond it are treated as default (ABI growth) */
  int32_t in_w, in_h;    /* source size; both must be even; 16..8192 */
  int32_t out_w, out_h;  /* destination size; each in [in, 4*in] (equal size is allowed) */
  int32_t quality;       /* nvvsr_quality, or NVVSR_QUALITY_AUTO */
  float strength;        /* 0..1; negative selects the default 0.4 (only affects some modes) */
  int32_t in_format;     /* nvvsr_in_format */
  int32_t out_format;    /* nvvsr_out_format */
  int32_t matrix;        /* nvvsr_matrix */
  int32_t full_range;    /* 0 = limited/video range (16..235), 1 = full range; same for input and I420 output */
  uint32_t flags;        /* NVVSR_FLAG_* */
  double fps;            /* frame rate hint for QUALITY_AUTO; 0 = unknown (treated as < 50) */
} nvvsr_config;

/* Fill cfg with defaults (quality AUTO, strength default, NV12 -> RGB24_PLANAR, matrix AUTO,
 * limited range). Sizes are left 0 and must be set by the caller. */
NVVSR_API void nvvsr_config_init(nvvsr_config *cfg);

/* Policy from the Phase 0 measurements: source height <= 540 -> ULTRA, 541..800 -> HIGH,
 * > 800 -> MEDIUM. A high frame rate can only lower the result: fps >= 50 and in_h > 1200
 * -> LOW; fps >= 100 additionally steps one level down (ULTRA->HIGH->MEDIUM->LOW). */
NVVSR_API int nvvsr_auto_quality(int in_h, double fps);

/* ------------------------------------------------------------------------- */
/* Library state                                                             */
/* ------------------------------------------------------------------------- */

typedef struct nvvsr_ctx nvvsr_ctx;

/* Version string of libnvvsr ("0.1.0") and the API version it implements. */
NVVSR_API const char *nvvsr_version(void);
NVVSR_API int nvvsr_api_version(void);

/* Is super resolution usable on this machine? Locates and loads the wheel libraries
 * (once, thread safe, result cached for the life of the process), checks the CUDA
 * driver (libcuda present, >= CUDA 12.8, at least one device). Does NOT create a CUDA
 * context or allocate GPU memory. Returns NVVSR_OK, or the reason (NVVSR_ERR_NO_LIBDIR,
 * _LIBRARY, _NO_GPU, _DRIVER); details via nvvsr_last_error(). A later change of
 * NVVSR_LIBDIR is not picked up (first result is cached). */
NVVSR_API int nvvsr_probe(void);

/* ------------------------------------------------------------------------- */
/* Contexts and frames                                                       */
/* ------------------------------------------------------------------------- */

/* Source frame in host memory. Each plane pointer addresses pixel (0,0) of that plane;
 * stride is the byte distance between rows (> 0, >= row bytes; need not be aligned).
 * Plane sizes (in_w x in_h luma): NV12 UV plane: in_w bytes x in_h/2 rows;
 * I420 U and V: in_w/2 x in_h/2. Pinned memory (nvvsr_alloc_host) makes the transfer
 * faster but is not required. The pointers are only read during nvvsr_process(). */
typedef struct nvvsr_image_in {
  const uint8_t *plane[3];
  int32_t stride[3];
} nvvsr_image_in;

/* Destination frame in host memory, same conventions. Row bytes: RGB24_PLANAR and
 * I420-Y: out_w each (I420 U/V: out_w/2); RGBA_CHUNKY: 4*out_w in plane[0]. Only the
 * pixels inside the frame are written, padding between rows is left untouched. */
typedef struct nvvsr_image_out {
  uint8_t *plane[3];
  int32_t stride[3];
} nvvsr_image_out;

/* Create a context: validates cfg (before touching the GPU), initialises the library
 * if needed, allocates all GPU buffers once and loads the model. On failure *out is
 * set to NULL and nothing is leaked. */
NVVSR_API int nvvsr_create(const nvvsr_config *cfg, nvvsr_ctx **out);

/* Process one frame, synchronously: on success `out` holds the upscaled frame.
 * On failure the contents of `out` are unspecified (the front-end should pass the
 * source frame through). The context stays usable after a failure. */
NVVSR_API int nvvsr_process(nvvsr_ctx *ctx, const nvvsr_image_in *in, const nvvsr_image_out *out);

/* Change the quality (NVVSR_QUALITY_AUTO allowed). Takes effect on the next frame; it is
 * instant (no model reload) and a no-op if the resolved quality is unchanged. */
NVVSR_API int nvvsr_set_quality(nvvsr_ctx *ctx, int quality);

/* The quality currently in effect (never AUTO), or a negative status for ctx == NULL. */
NVVSR_API int nvvsr_get_quality(const nvvsr_ctx *ctx);

/* Per-stage wall-clock times of the last nvvsr_process() in milliseconds. Stage fields
 * are only filled when the context was created with NVVSR_FLAG_TIMING; total_ms always. */
typedef struct nvvsr_timing {
  double upload_ms;    /* host -> GPU copies of the source planes */
  double convert_ms;   /* YUV -> RGBA before SR plus RGBA -> output format after SR */
  double sr_ms;        /* the super-resolution effect */
  double download_ms;  /* GPU -> host copies of the result */
  double total_ms;     /* the whole nvvsr_process() call */
} nvvsr_timing;
NVVSR_API int nvvsr_last_timing(const nvvsr_ctx *ctx, nvvsr_timing *out);

/* Destroy a context (NULL is ignored): releases its frame buffers and parks the SDK
 * effect for reuse (see "Effect cache" above). */
NVVSR_API void nvvsr_destroy(nvvsr_ctx *ctx);

/* Really destroy all parked effects, returning the VRAM they hold. Because of the SDK leak
 * this gives back little when more than one effect existed (~215 MiB stay lost per extra
 * effect), so call it only when the memory is needed, never in a normal teardown path. */
NVVSR_API void nvvsr_trim(void);

/* ------------------------------------------------------------------------- */
/* Optional helpers                                                          */
/* ------------------------------------------------------------------------- */

/* Page-locked (pinned) host memory, usable as frame planes: copies to/from it are
 * 2-3x faster than from ordinary pageable memory. Returns NULL if unavailable (library
 * not usable, out of memory); callers fall back to malloc(). Free with nvvsr_free_host
 * (NULL is ignored). Thread safe. */
NVVSR_API void *nvvsr_alloc_host(size_t bytes);
NVVSR_API void nvvsr_free_host(void *ptr);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* NVVSR_H */
