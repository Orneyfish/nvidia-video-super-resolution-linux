// Reference for the plugin tests: runs libnvvsr directly on raw planar YUV420P8 frames (the layout
// `vspipe script.vpy -` writes: Y, U, V per frame, no row padding) and writes the raw planar result
// (RGB24: R,G,B planes, or I420: Y,U,V planes) in the same layout, so the output can be compared
// byte for byte with what the VapourSynth plugin produces.
//
//   nvvsr_vs_ref IWxIH OWxOH QUALITY MATRIX RANGE FORMAT FRAMES in.raw out.raw
//     QUALITY auto|bicubic|low|medium|high|ultra   MATRIX auto|601|709   RANGE limited|full   FORMAT rgb24|yuv420p8
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nvvsr.h"

static int quality_of(const std::string &q) {
  if (q == "bicubic") return NVVSR_QUALITY_BICUBIC;
  if (q == "low") return NVVSR_QUALITY_LOW;
  if (q == "medium") return NVVSR_QUALITY_MEDIUM;
  if (q == "high") return NVVSR_QUALITY_HIGH;
  if (q == "ultra") return NVVSR_QUALITY_ULTRA;
  return NVVSR_QUALITY_AUTO;
}

int main(int argc, char **argv) {
  if (argc != 10) {
    std::fprintf(stderr, "usage: %s IWxIH OWxOH QUALITY MATRIX RANGE FORMAT FRAMES in.raw out.raw\n", argv[0]);
    return 2;
  }
  int iw, ih, ow, oh;
  if (std::sscanf(argv[1], "%dx%d", &iw, &ih) != 2 || std::sscanf(argv[2], "%dx%d", &ow, &oh) != 2) return 2;
  const std::string matrix = argv[4], range = argv[5], format = argv[6];
  const int frames = std::atoi(argv[7]);

  nvvsr_config cfg;
  nvvsr_config_init(&cfg);
  cfg.in_w = iw; cfg.in_h = ih; cfg.out_w = ow; cfg.out_h = oh;
  cfg.quality = quality_of(argv[3]);
  cfg.in_format = NVVSR_IN_I420;
  cfg.out_format = format == "yuv420p8" ? NVVSR_OUT_I420 : NVVSR_OUT_RGB24_PLANAR;
  cfg.matrix = matrix == "601" ? NVVSR_MATRIX_BT601 : matrix == "709" ? NVVSR_MATRIX_BT709 : NVVSR_MATRIX_AUTO;
  cfg.full_range = range == "full";
  if (nvvsr_probe() != NVVSR_OK) { std::fprintf(stderr, "probe: %s\n", nvvsr_last_error()); return 1; }
  nvvsr_ctx *ctx = nullptr;
  if (nvvsr_create(&cfg, &ctx) != NVVSR_OK) { std::fprintf(stderr, "create: %s\n", nvvsr_last_error()); return 1; }

  const size_t in_bytes = (size_t)iw * ih + 2 * (size_t)(iw / 2) * (ih / 2);
  const bool yuv = cfg.out_format == NVVSR_OUT_I420;
  const size_t out_bytes = yuv ? (size_t)ow * oh + 2 * (size_t)(ow / 2) * (oh / 2) : 3 * (size_t)ow * oh;
  std::vector<uint8_t> in(in_bytes), out(out_bytes);
  FILE *fi = std::fopen(argv[8], "rb"), *fo = std::fopen(argv[9], "wb");
  if (!fi || !fo) { std::perror("open"); return 1; }
  for (int n = 0; n < frames; ++n) {
    if (std::fread(in.data(), 1, in_bytes, fi) != in_bytes) { std::fprintf(stderr, "short input at frame %d\n", n); return 1; }
    nvvsr_image_in src{};
    nvvsr_image_out dst{};
    src.plane[0] = in.data();                          src.stride[0] = iw;
    src.plane[1] = in.data() + (size_t)iw * ih;        src.stride[1] = iw / 2;
    src.plane[2] = src.plane[1] + (size_t)(iw / 2) * (ih / 2); src.stride[2] = iw / 2;
    dst.plane[0] = out.data();                         dst.stride[0] = ow;
    dst.plane[1] = out.data() + (size_t)ow * oh;       dst.stride[1] = yuv ? ow / 2 : ow;
    dst.plane[2] = dst.plane[1] + (yuv ? (size_t)(ow / 2) * (oh / 2) : (size_t)ow * oh); dst.stride[2] = dst.stride[1];
    int rc = nvvsr_process(ctx, &src, &dst);
    if (rc != NVVSR_OK) { std::fprintf(stderr, "process: %s (%s)\n", nvvsr_strerror(rc), nvvsr_last_error()); return 1; }
    std::fwrite(out.data(), 1, out_bytes, fo);
  }
  std::fclose(fi);
  std::fclose(fo);
  nvvsr_destroy(ctx);
  return 0;
}
