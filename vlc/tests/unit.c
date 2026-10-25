/*
 * Unit tests of the pure logic of the module (src/common.c): option parsing, source classification, target
 * geometry, power state and the CPU fallback upscaler. No VLC instance and no GPU needed.
 * Built by CMake as nvvsr_vlc_unit; tests/run.sh runs it. Prints PASS/FAIL lines, exit status = failures.
 */
#define NVVSR_VLC_TESTHOOK 1
#include "../src/common.c"

/* normally emitted by vlc_module_begin() */
const char vlc_module_name[] = "nvvsr";

#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass, g_fail;

static void check(bool ok, const char *name) {
  if (ok) {
    g_pass++;
    printf("PASS unit: %s\n", name);
  } else {
    g_fail++;
    printf("FAIL unit: %s\n", name);
  }
}

static video_format_t fmt(vlc_fourcc_t chroma, unsigned w, unsigned h) {
  video_format_t f;
  video_format_Init(&f, chroma);
  f.i_width = f.i_visible_width = w;
  f.i_height = f.i_visible_height = h;
  f.i_sar_num = f.i_sar_den = 1;
  f.i_frame_rate = 30;
  f.i_frame_rate_base = 1;
  return f;
}

static void test_quality(void) {
  check(nv_quality_parse("auto") == NVVSR_QUALITY_AUTO, "quality auto");
  check(nv_quality_parse("HIGH") == NVVSR_QUALITY_HIGH, "quality is case-insensitive");
  check(nv_quality_parse("bicubic") == NVVSR_QUALITY_BICUBIC, "quality bicubic");
  check(nv_quality_parse("low") == NVVSR_QUALITY_LOW && nv_quality_parse("medium") == NVVSR_QUALITY_MEDIUM &&
            nv_quality_parse("ultra") == NVVSR_QUALITY_ULTRA,
        "quality low/medium/ultra");
  check(nv_quality_parse("streaming_medium") == NVVSR_QUALITY_STREAMING_MEDIUM &&
            nv_quality_parse("streaming_ultra") == NVVSR_QUALITY_STREAMING_ULTRA,
        "quality streaming_*");
  check(nv_quality_parse("superb") == INT32_MIN && nv_quality_parse("") == INT32_MIN && nv_quality_parse(NULL) == INT32_MIN &&
            nv_quality_parse("high2") == INT32_MIN,
        "unknown quality names are rejected");
}

static void test_classify(void) {
  nv_src_t s;
  video_format_t f = fmt(VLC_CODEC_I420, 1280, 720);
  nv_classify(&f, &s);
  check(s.ok && s.chroma == VLC_CODEC_I420 && !s.convert && s.in_format == NVVSR_IN_I420 && s.w == 1280 && s.h == 720 &&
            s.matrix == NVVSR_MATRIX_AUTO && !s.full_range && s.fps == 30.0,
        "I420 720p is accepted (matrix auto, limited range, 30 fps)");

  f = fmt(VLC_CODEC_NV12, 1920, 1080);
  nv_classify(&f, &s);
  check(s.ok && s.in_format == NVVSR_IN_NV12 && s.chroma == VLC_CODEC_NV12, "NV12 is accepted");

  f = fmt(VLC_CODEC_J420, 1280, 720);
  nv_classify(&f, &s);
  check(s.ok && s.full_range == 1 && s.chroma == VLC_CODEC_J420, "J420 means full range");

  f = fmt(VLC_CODEC_YV12, 1280, 720);
  nv_classify(&f, &s);
  check(s.ok && s.swap_uv && s.in_format == NVVSR_IN_I420, "YV12 is I420 with swapped chroma planes");

  f = fmt(VLC_CODEC_VAAPI_420, 1280, 720);
  nv_classify(&f, &s);
  check(s.ok && s.convert && s.chroma == VLC_CODEC_I420, "VAAPI 4:2:0 surfaces are accepted through VLC's converter");
  f = fmt(VLC_CODEC_VDPAU_VIDEO_420, 1280, 720);
  nv_classify(&f, &s);
  check(s.ok && s.convert && s.chroma == VLC_CODEC_I420, "VDPAU 4:2:0 surfaces are accepted through VLC's converter");

  f = fmt(VLC_CODEC_I420_10L, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "10-bit 4:2:0 is rejected");
  f = fmt(VLC_CODEC_P010, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "P010 is rejected");
  f = fmt(VLC_CODEC_I422, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "4:2:2 is rejected");
  f = fmt(VLC_CODEC_I444, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "4:4:4 is rejected");
  f = fmt(VLC_CODEC_RGB32, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "RGB is rejected");
  f = fmt(VLC_CODEC_VAAPI_420_10BPP, 1280, 720);
  nv_classify(&f, &s);
  check(!s.ok, "10-bit VAAPI surfaces are rejected");

  f = fmt(VLC_CODEC_I420, 1281, 720);
  nv_classify(&f, &s);
  check(!s.ok, "odd width is rejected");
  f = fmt(VLC_CODEC_I420, 1280, 721);
  nv_classify(&f, &s);
  check(!s.ok, "odd height is rejected");
  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.i_x_offset = 1;
  nv_classify(&f, &s);
  check(!s.ok, "odd crop offset is rejected");
  f = fmt(VLC_CODEC_I420, 8, 8);
  nv_classify(&f, &s);
  check(!s.ok, "tiny pictures are rejected");
  f = fmt(VLC_CODEC_I420, 8194, 4320);
  nv_classify(&f, &s);
  check(!s.ok, "pictures wider than 8192 are rejected");

  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.transfer = TRANSFER_FUNC_SMPTE_ST2084;
  nv_classify(&f, &s);
  check(!s.ok && strcmp(s.why, "HDR") == 0, "PQ is rejected as HDR");
  f.transfer = TRANSFER_FUNC_HLG;
  nv_classify(&f, &s);
  check(!s.ok, "HLG is rejected as HDR");
  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.space = COLOR_SPACE_BT2020;
  nv_classify(&f, &s);
  check(!s.ok, "BT.2020 matrix is rejected");
  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.primaries = COLOR_PRIMARIES_BT2020;
  nv_classify(&f, &s);
  check(!s.ok, "BT.2020 primaries are rejected");
  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.orientation = ORIENT_ROTATED_90;
  nv_classify(&f, &s);
  check(!s.ok, "rotated video is rejected");

  f = fmt(VLC_CODEC_I420, 1280, 720);
  f.space = COLOR_SPACE_BT601;
  f.b_color_range_full = true;
  nv_classify(&f, &s);
  check(s.ok && s.matrix == NVVSR_MATRIX_BT601 && s.full_range == 1, "BT.601 and full range flags are passed on");
  f.space = COLOR_SPACE_BT709;
  nv_classify(&f, &s);
  check(s.ok && s.matrix == NVVSR_MATRIX_BT709, "BT.709 flag is passed on");

  f = fmt(VLC_CODEC_I420, 1280, 738);
  f.i_visible_height = 720;
  nv_classify(&f, &s);
  check(s.ok && s.h == 720 && s.w == 1280, "the visible rectangle is used, not the coded size");
}

static bool size_is(const nv_src_t *s, unsigned pw, unsigned ph, unsigned ew, unsigned eh) {
  unsigned ow = 0, oh = 0;
  double sc;
  return nv_target_size(s, pw, ph, 0.02, &ow, &oh, &sc) && ow == ew && oh == eh;
}

static void test_geometry(void) {
  nv_src_t s;
  video_format_t f = fmt(VLC_CODEC_I420, 1280, 720);
  nv_classify(&f, &s);
  check(size_is(&s, 2560, 1440, 2560, 1440), "720p on 2560x1440 -> 2560x1440 (2x)");
  check(size_is(&s, 1920, 1080, 1920, 1080), "720p on 1080p -> 1920x1080");
  check(size_is(&s, 2560, 1600, 2560, 1440), "720p on a 16:10 panel keeps the aspect ratio (fit by width)");
  check(size_is(&s, 3000, 1440, 2560, 1440), "720p on a wide panel is fitted by height");
  check(size_is(&s, 5120, 1440, 2560, 1440), "ultrawide panel: 2x only, the display adds the bars");

  unsigned ow, oh;
  double sc;
  check(!nv_target_size(&s, 1280, 720, 0.02, &ow, &oh, &sc) && sc == 1.0, "source as big as the panel is left alone");
  check(!nv_target_size(&s, 1300, 731, 0.02, &ow, &oh, &sc), "gain below 2% is left alone");
  check(nv_target_size(&s, 1306, 735, 0.02, &ow, &oh, &sc) && ow >= 1280, "gain of 2% is upscaled");
  check(!nv_target_size(&s, 0, 0, 0.02, &ow, &oh, &sc) && !nv_target_size(&s, 2560, 0, 0.02, &ow, &oh, &sc),
        "unknown panel size is left alone");
  check(!nv_target_size(&s, 640, 360, 0.02, &ow, &oh, &sc), "never downscales");

  f = fmt(VLC_CODEC_I420, 1920, 1080);
  nv_classify(&f, &s);
  check(size_is(&s, 2560, 1440, 2560, 1440), "1080p on 2560x1440 -> 2560x1440 (1.33x)");
  f = fmt(VLC_CODEC_I420, 2560, 1440);
  nv_classify(&f, &s);
  check(!nv_target_size(&s, 2560, 1440, 0.02, &ow, &oh, &sc), "1440p on a 1440p panel is left alone");
  f = fmt(VLC_CODEC_I420, 480, 270);
  nv_classify(&f, &s);
  check(size_is(&s, 2560, 1440, 1920, 1080), "5.3x is capped at 4x (480x270 -> 1920x1080)");
  f = fmt(VLC_CODEC_I420, 854, 480);
  nv_classify(&f, &s);
  check(nv_target_size(&s, 2560, 1440, 0.02, &ow, &oh, &sc) && ow % 2 == 0 && oh % 2 == 0 && ow >= 854 && oh >= 480,
        "output sizes are even");

  /* anamorphic: 720x576 with SAR 16:15 shows as 768x576 */
  f = fmt(VLC_CODEC_I420, 720, 576);
  f.i_sar_num = 16;
  f.i_sar_den = 15;
  nv_classify(&f, &s);
  check(nv_target_size(&s, 1920, 1080, 0.02, &ow, &oh, &sc) && oh == 1080 && ow == 1350 && sc > 1.87 && sc < 1.88,
        "the pixel aspect ratio is part of the fit (720x576 SAR 16:15 -> scale 1.875)");

  /* invariants over a sweep */
  bool ok = true;
  for (unsigned w = 320; w <= 3840 && ok; w += 160)
    for (unsigned h = 240; h <= 2160 && ok; h += 120)
      for (unsigned p = 0; p < 4 && ok; p++) {
        static const unsigned panels[4][2] = {{2560, 1440}, {1920, 1080}, {3840, 2160}, {1366, 768}};
        f = fmt(VLC_CODEC_I420, w, h);
        nv_classify(&f, &s);
        if (!s.ok) continue;
        if (nv_target_size(&s, panels[p][0], panels[p][1], 0.02, &ow, &oh, &sc))
          ok = ow % 2 == 0 && oh % 2 == 0 && ow >= w && oh >= h && ow <= 4 * w && oh <= 4 * h && ow <= panels[p][0] + 2 &&
               oh <= panels[p][1] + 2;
      }
  check(ok, "sweep: sizes are even, within 1x..4x and never exceed the panel");
}

static void write_file(const char *dir, const char *sub, const char *name, const char *content) {
  char path[PATH_MAX];
  snprintf(path, sizeof path, "%s/%s", dir, sub);
  mkdir(path, 0700);
  snprintf(path, sizeof path, "%s/%s/%s", dir, sub, name);
  FILE *fp = fopen(path, "w");
  assert(fp);
  fputs(content, fp);
  fclose(fp);
}

static void test_power(void) {
  const char *base = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
  char root[PATH_MAX];
  snprintf(root, sizeof root, "%s/nvvsr-power-XXXXXX", base);
  if (mkdtemp(root) == NULL) {
    check(false, "power: cannot create a scratch directory");
    return;
  }
  setenv("NVVSR_VLC_POWER_DIR", root, 1);
  check(nv_on_ac(), "power: no supplies at all counts as mains");
  write_file(root, "BAT0", "type", "Battery\n");
  check(!nv_on_ac(), "power: battery only is not mains");
  write_file(root, "AC", "type", "Mains\n");
  write_file(root, "AC", "online", "0\n");
  check(!nv_on_ac(), "power: offline adapter + battery is not mains");
  write_file(root, "AC", "online", "1\n");
  check(nv_on_ac(), "power: online adapter is mains");
  write_file(root, "AC", "online", "0\n");
  write_file(root, "ucsi-source-psy-USBC000:001", "type", "USB\n");
  write_file(root, "ucsi-source-psy-USBC000:001", "online", "1\n");
  check(nv_on_ac(), "power: an online USB-C supply counts as mains");
  unsetenv("NVVSR_VLC_POWER_DIR");
  char cmd[PATH_MAX + 16];
  snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
  if (system(cmd) != 0) { /* scratch only */ }
}

static picture_t *make_pic(unsigned w, unsigned h, bool nv12, int *pitch0) {
  picture_t *p = calloc(1, sizeof *p);
  assert(p);
  const int pitch = (int)w + 17; /* deliberately unaligned */
  *pitch0 = pitch;
  p->i_planes = nv12 ? 2 : 3;
  p->p[0].i_pitch = pitch;
  p->p[0].i_lines = (int)h;
  p->p[0].p_pixels = calloc((size_t)pitch, h);
  for (int i = 1; i < p->i_planes; i++) {
    p->p[i].i_pitch = nv12 ? pitch : pitch / 2 + 3;
    p->p[i].i_lines = (int)h / 2;
    p->p[i].p_pixels = calloc((size_t)p->p[i].i_pitch, h / 2);
  }
  return p;
}

static void free_pic(picture_t *p) {
  for (int i = 0; i < p->i_planes; i++) free(p->p[i].p_pixels);
  free(p);
}

static void test_cpu_fallback(void) {
  for (int nv12 = 0; nv12 < 2; nv12++) {
    video_format_t f = fmt(nv12 ? VLC_CODEC_NV12 : VLC_CODEC_I420, 64, 48);
    nv_src_t s;
    nv_classify(&f, &s);
    nv_engine_t e;
    memset(&e, 0, sizeof e);
    e.src = s;
    e.out_w = 130;
    e.out_h = 98;
    e.fail_after = -1;

    int pitch;
    picture_t *p = make_pic(64, 48, nv12, &pitch);
    /* luma ramp left to right, constant chroma */
    for (int y = 0; y < 48; y++)
      for (int x = 0; x < 64; x++) p->p[0].p_pixels[y * pitch + x] = (uint8_t)(x * 4);
    for (int y = 0; y < 24; y++)
      for (int x = 0; x < 32; x++) {
        if (nv12) {
          p->p[1].p_pixels[y * p->p[1].i_pitch + 2 * x] = 100;
          p->p[1].p_pixels[y * p->p[1].i_pitch + 2 * x + 1] = 200;
        } else {
          p->p[1].p_pixels[y * p->p[1].i_pitch + x] = 100;
          p->p[2].p_pixels[y * p->p[2].i_pitch + x] = 200;
        }
      }
    const int os[3] = {131 + 5, 65 + 5, 65 + 5};
    uint8_t *out[3];
    for (int i = 0; i < 3; i++) {
      out[i] = malloc((size_t)os[i] * 98);
      memset(out[i], 0xAA, (size_t)os[i] * 98);
    }
    const int rc = nv_engine_run(&e, p, out, os);
    bool mono = true, chroma_ok = true, pad_ok = true;
    for (int y = 0; y < 98; y++) {
      for (int x = 1; x < 130; x++)
        if (out[0][y * os[0] + x] < out[0][y * os[0] + x - 1]) mono = false;
      for (int x = 130; x < os[0]; x++)
        if (out[0][y * os[0] + x] != 0xAA) pad_ok = false;
    }
    for (int y = 0; y < 49; y++)
      for (int x = 0; x < 65; x++)
        if (out[1][y * os[1] + x] != 100 || out[2][y * os[2] + x] != 200) chroma_ok = false;
    check(rc == 1 && e.fallback_frames == 1, nv12 ? "cpu fallback runs for NV12 input" : "cpu fallback runs for I420 input");
    check(mono && out[0][0] == 0 && out[0][129] > 240, "cpu fallback: luma ramp stays monotonic and spans the range");
    check(chroma_ok, "cpu fallback: constant chroma stays constant (NV12 de-interleaved)");
    check(pad_ok, "cpu fallback: row padding is not written");
    for (int i = 0; i < 3; i++) free(out[i]);
    free_pic(p);

    /* a picture that does not match the source description is refused */
    picture_t *small = make_pic(32, 24, nv12, &pitch);
    uint8_t *dummy[3] = {NULL, NULL, NULL};
    const int ds[3] = {1, 1, 1};
    check(nv_engine_run(&e, small, dummy, ds) == -1, "a picture smaller than the source is refused");
    free_pic(small);
  }
}

int main(void) {
  test_quality();
  test_classify();
  test_geometry();
  test_power();
  test_cpu_fallback();
  printf("unit: %d passed, %d failed\n", g_pass, g_fail);
  return g_fail;
}
