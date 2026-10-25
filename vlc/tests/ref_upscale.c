/*
 * Reference for the VLC tests: runs libnvvsr directly on raw planar I420 frames (no row padding, Y then U then V
 * per frame, the layout `ffmpeg -pix_fmt yuv420p -f rawvideo` writes) and writes the raw I420 result, so the
 * pictures VLC shows through the nvvsr module can be compared byte for byte.
 *
 *   nvvsr_vlc_ref IWxIH OWxOH QUALITY MATRIX RANGE FRAMES in.i420 out.i420
 *     QUALITY auto|bicubic|low|medium|high|ultra   MATRIX auto|601|709   RANGE limited|full
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nvvsr.h"

static int quality_of(const char *q) {
  if (!strcmp(q, "bicubic")) return NVVSR_QUALITY_BICUBIC;
  if (!strcmp(q, "low")) return NVVSR_QUALITY_LOW;
  if (!strcmp(q, "medium")) return NVVSR_QUALITY_MEDIUM;
  if (!strcmp(q, "high")) return NVVSR_QUALITY_HIGH;
  if (!strcmp(q, "ultra")) return NVVSR_QUALITY_ULTRA;
  return NVVSR_QUALITY_AUTO;
}

int main(int argc, char **argv) {
  if (argc != 9) {
    fprintf(stderr, "usage: %s IWxIH OWxOH QUALITY MATRIX RANGE FRAMES in.i420 out.i420\n", argv[0]);
    return 2;
  }
  int iw, ih, ow, oh;
  if (sscanf(argv[1], "%dx%d", &iw, &ih) != 2 || sscanf(argv[2], "%dx%d", &ow, &oh) != 2) return 2;
  const int frames = atoi(argv[6]);

  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = iw;
  cfg.in_h = ih;
  cfg.out_w = ow;
  cfg.out_h = oh;
  cfg.quality = quality_of(argv[3]);
  cfg.in_format = NVVSR_IN_I420;
  cfg.out_format = NVVSR_OUT_I420;
  cfg.matrix = !strcmp(argv[4], "601") ? NVVSR_MATRIX_BT601 : !strcmp(argv[4], "709") ? NVVSR_MATRIX_BT709 : NVVSR_MATRIX_AUTO;
  cfg.full_range = !strcmp(argv[5], "full");
  if (nvvsr_probe() != NVVSR_OK) {
    fprintf(stderr, "probe: %s\n", nvvsr_last_error());
    return 1;
  }
  nvvsr_ctx *ctx = NULL;
  if (nvvsr_create(&cfg, &ctx) != NVVSR_OK) {
    fprintf(stderr, "create: %s\n", nvvsr_last_error());
    return 1;
  }

  const size_t in_bytes = (size_t)iw * ih + 2 * (size_t)(iw / 2) * (ih / 2);
  const size_t out_bytes = (size_t)ow * oh + 2 * (size_t)(ow / 2) * (oh / 2);
  unsigned char *in = malloc(in_bytes), *out = malloc(out_bytes);
  FILE *fi = fopen(argv[7], "rb"), *fo = fopen(argv[8], "wb");
  if (!in || !out || !fi || !fo) {
    perror("open");
    return 1;
  }
  for (int n = 0; n < frames; n++) {
    if (fread(in, 1, in_bytes, fi) != in_bytes) {
      fprintf(stderr, "short input at frame %d\n", n);
      return 1;
    }
    nvvsr_image_in src;
    nvvsr_image_out dst;
    memset(&src, 0, sizeof src);
    memset(&dst, 0, sizeof dst);
    src.plane[0] = in;
    src.stride[0] = iw;
    src.plane[1] = in + (size_t)iw * ih;
    src.stride[1] = iw / 2;
    src.plane[2] = src.plane[1] + (size_t)(iw / 2) * (ih / 2);
    src.stride[2] = iw / 2;
    dst.plane[0] = out;
    dst.stride[0] = ow;
    dst.plane[1] = out + (size_t)ow * oh;
    dst.stride[1] = ow / 2;
    dst.plane[2] = dst.plane[1] + (size_t)(ow / 2) * (oh / 2);
    dst.stride[2] = ow / 2;
    const int rc = nvvsr_process(ctx, &src, &dst);
    if (rc != NVVSR_OK) {
      fprintf(stderr, "process: %s (%s)\n", nvvsr_strerror(rc), nvvsr_last_error());
      return 1;
    }
    fwrite(out, 1, out_bytes, fo);
  }
  fclose(fi);
  fclose(fo);
  nvvsr_destroy(ctx);
  free(in);
  free(out);
  return 0;
}
