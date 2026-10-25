// Test 1: probe + error paths. No GPU needed except where stated.
//   test_errors args           config / argument validation (runs before any library init)
//   test_errors bad_libdir     NVVSR_LIBDIR points to nowhere -> clean error, no crash
//   test_errors empty_libdir   NVVSR_LIBDIR is an existing but empty directory
//   test_errors no_libdir      nothing configured at all
//   test_errors libdir_file    ~/.config/nvvsr/libdir file is honoured (needs a usable NVVSR_LIBDIR to copy)
//   test_errors status         strerror / version / auto_quality tables
#include <string>

#include "test_util.h"

static nvvsr_config good_cfg() {
  nvvsr_config c;
  nvvsr_config_init(&c);
  c.in_w = 1280; c.in_h = 720; c.out_w = 2560; c.out_h = 1440;
  c.quality = NVVSR_QUALITY_HIGH;
  return c;
}

static std::string make_tmpdir() {
  char tmpl[] = "/tmp/nvvsr_test_XXXXXX";
  char *d = mkdtemp(tmpl);
  return d ? d : "/nonexistent";
}

static void expect_create(nvvsr_config c, int expect, const char *what) {
  nvvsr_ctx *ctx = reinterpret_cast<nvvsr_ctx *>(0x1);  // must be reset to NULL
  int rc = nvvsr_create(&c, &ctx);
  T_CHECK(rc == expect, "%s: got %d (%s), expected %d (%s) [%s]", what, rc, nvvsr_strerror(rc), expect, nvvsr_strerror(expect),
          nvvsr_last_error());
  T_CHECK(ctx == nullptr, "%s: *out must be NULL after a failure", what);
  if (ctx && ctx != reinterpret_cast<nvvsr_ctx *>(0x1)) nvvsr_destroy(ctx);
  if (rc != NVVSR_OK) T_CHECK(nvvsr_last_error()[0] != 0, "%s: last_error is empty", what);
}

static int mode_args() {
  // NULL handling
  nvvsr_ctx *ctx = nullptr;
  T_CHECK(nvvsr_create(nullptr, &ctx) == NVVSR_ERR_INVALID_ARG, "NULL cfg");
  nvvsr_config c = good_cfg();
  T_CHECK(nvvsr_create(&c, nullptr) == NVVSR_ERR_INVALID_ARG, "NULL out");
  T_CHECK(nvvsr_process(nullptr, nullptr, nullptr) == NVVSR_ERR_INVALID_ARG, "NULL ctx process");
  T_CHECK(nvvsr_set_quality(nullptr, 3) == NVVSR_ERR_INVALID_ARG, "NULL ctx set_quality");
  T_CHECK(nvvsr_get_quality(nullptr) < 0, "NULL ctx get_quality");
  T_CHECK(nvvsr_last_timing(nullptr, nullptr) == NVVSR_ERR_INVALID_ARG, "NULL last_timing");
  nvvsr_destroy(nullptr);
  nvvsr_free_host(nullptr);
  nvvsr_config_init(nullptr);

  // struct_size
  c = good_cfg(); c.struct_size = 0;  expect_create(c, NVVSR_ERR_INVALID_ARG, "struct_size 0");
  c = good_cfg(); c.struct_size = 8;  expect_create(c, NVVSR_ERR_INVALID_ARG, "struct_size 8");

  // sizes
  c = good_cfg(); c.in_w = 0;         expect_create(c, NVVSR_ERR_INVALID_ARG, "in_w 0");
  c = good_cfg(); c.in_h = -720;      expect_create(c, NVVSR_ERR_INVALID_ARG, "in_h negative");
  c = good_cfg(); c.out_w = 0;        expect_create(c, NVVSR_ERR_INVALID_ARG, "out_w 0");
  c = good_cfg(); c.in_w = 1281;      expect_create(c, NVVSR_ERR_UNSUPPORTED, "odd in_w");
  c = good_cfg(); c.in_h = 719;       expect_create(c, NVVSR_ERR_UNSUPPORTED, "odd in_h");
  c = good_cfg(); c.in_w = 8; c.in_h = 8; expect_create(c, NVVSR_ERR_UNSUPPORTED, "input too small");
  c = good_cfg(); c.in_w = 9000;      expect_create(c, NVVSR_ERR_UNSUPPORTED, "input too large");
  c = good_cfg(); c.out_w = 1200;     expect_create(c, NVVSR_ERR_UNSUPPORTED, "downscale w");
  c = good_cfg(); c.out_h = 700;      expect_create(c, NVVSR_ERR_UNSUPPORTED, "downscale h");
  c = good_cfg(); c.out_w = 5200;     expect_create(c, NVVSR_ERR_UNSUPPORTED, "more than 4x");

  // enums
  c = good_cfg(); c.quality = 7;      expect_create(c, NVVSR_ERR_INVALID_ARG, "quality 7");
  c = good_cfg(); c.quality = -2;     expect_create(c, NVVSR_ERR_INVALID_ARG, "quality -2");
  c = good_cfg(); c.quality = 99;     expect_create(c, NVVSR_ERR_INVALID_ARG, "quality 99");
  c = good_cfg(); c.strength = 2.0f;  expect_create(c, NVVSR_ERR_INVALID_ARG, "strength 2");
  c = good_cfg(); c.strength = std::nanf(""); expect_create(c, NVVSR_ERR_INVALID_ARG, "strength NaN");
  c = good_cfg(); c.in_format = NVVSR_IN_P010;   expect_create(c, NVVSR_ERR_UNSUPPORTED, "P010 reserved");
  c = good_cfg(); c.in_format = NVVSR_IN_I42010; expect_create(c, NVVSR_ERR_UNSUPPORTED, "I42010 reserved");
  c = good_cfg(); c.in_format = 42;              expect_create(c, NVVSR_ERR_INVALID_ARG, "in_format 42");
  c = good_cfg(); c.in_format = -1;              expect_create(c, NVVSR_ERR_INVALID_ARG, "in_format -1");
  c = good_cfg(); c.out_format = NVVSR_OUT_RGB48_PLANAR; expect_create(c, NVVSR_ERR_UNSUPPORTED, "RGB48 reserved");
  c = good_cfg(); c.out_format = NVVSR_OUT_I42010;       expect_create(c, NVVSR_ERR_UNSUPPORTED, "I42010 out reserved");
  c = good_cfg(); c.out_format = 17;             expect_create(c, NVVSR_ERR_INVALID_ARG, "out_format 17");
  c = good_cfg(); c.out_format = NVVSR_OUT_I420; c.out_w = 2561; expect_create(c, NVVSR_ERR_UNSUPPORTED, "I420 odd out_w");
  c = good_cfg(); c.matrix = NVVSR_MATRIX_BT2020; expect_create(c, NVVSR_ERR_UNSUPPORTED, "BT2020 reserved");
  c = good_cfg(); c.matrix = 9;                   expect_create(c, NVVSR_ERR_INVALID_ARG, "matrix 9");

  // after all failures the library is still sane (no state corrupted): validation again
  c = good_cfg(); c.in_w = 0; expect_create(c, NVVSR_ERR_INVALID_ARG, "repeat after failures");
  return tu::finish("test_errors args");
}

static int mode_status() {
  for (int s = 0; s >= -9; --s) {
    const char *t = nvvsr_strerror(s);
    T_CHECK(t && t[0], "strerror(%d) empty", s);
  }
  T_CHECK(std::string(nvvsr_strerror(-12345)) == "unknown status", "unknown status text");
  T_CHECK(std::string(nvvsr_strerror(0)) == "ok", "ok text");
  T_CHECK(nvvsr_version() && nvvsr_version()[0] == '0', "version string");
  T_CHECK(nvvsr_api_version() == NVVSR_API_VERSION, "api version");
  // auto quality policy: <=540 ultra, 541..800 high, >800 medium
  T_CHECK(nvvsr_auto_quality(360, 30) == NVVSR_QUALITY_ULTRA, "360p");
  T_CHECK(nvvsr_auto_quality(480, 24) == NVVSR_QUALITY_ULTRA, "480p");
  T_CHECK(nvvsr_auto_quality(540, 60) == NVVSR_QUALITY_ULTRA, "540p");
  T_CHECK(nvvsr_auto_quality(541, 30) == NVVSR_QUALITY_HIGH, "541p");
  T_CHECK(nvvsr_auto_quality(720, 30) == NVVSR_QUALITY_HIGH, "720p");
  T_CHECK(nvvsr_auto_quality(720, 60) == NVVSR_QUALITY_HIGH, "720p60 never raised");
  T_CHECK(nvvsr_auto_quality(800, 0) == NVVSR_QUALITY_HIGH, "800p");
  T_CHECK(nvvsr_auto_quality(801, 30) == NVVSR_QUALITY_MEDIUM, "801p");
  T_CHECK(nvvsr_auto_quality(1080, 24) == NVVSR_QUALITY_MEDIUM, "1080p24");
  T_CHECK(nvvsr_auto_quality(1080, 60) == NVVSR_QUALITY_MEDIUM, "1080p60");
  T_CHECK(nvvsr_auto_quality(1080, 120) == NVVSR_QUALITY_LOW, "1080p120 lowered");
  T_CHECK(nvvsr_auto_quality(2160, 60) == NVVSR_QUALITY_LOW, "2160p60 lowered");
  T_CHECK(nvvsr_auto_quality(480, 120) == NVVSR_QUALITY_HIGH, "480p120 lowered one step");
  for (int h = 100; h < 3000; h += 7)
    for (double fps : {0.0, 24.0, 30.0, 60.0, 120.0}) {
      int q = nvvsr_auto_quality(h, fps);
      int base = nvvsr_auto_quality(h, 0.0);
      T_CHECK(q <= base && q >= NVVSR_QUALITY_LOW, "fps may only lower: h=%d fps=%g q=%d base=%d", h, fps, q, base);
    }
  nvvsr_config c;
  nvvsr_config_init(&c);
  T_CHECK(c.struct_size == sizeof c && c.quality == NVVSR_QUALITY_AUTO && c.strength < 0 && c.in_format == NVVSR_IN_NV12 &&
              c.out_format == NVVSR_OUT_RGB24_PLANAR && c.matrix == NVVSR_MATRIX_AUTO,
          "config_init defaults");
  return tu::finish("test_errors status");
}

// Common checks once the library cannot be loaded: everything fails cleanly.
static void expect_unusable(int expected_probe) {
  int rc = nvvsr_probe();
  T_CHECK(rc == expected_probe, "probe returned %d (%s), expected %d (%s)", rc, nvvsr_strerror(rc), expected_probe,
          nvvsr_strerror(expected_probe));
  std::printf("  probe: %s -- \"%s\"\n", nvvsr_strerror(rc), nvvsr_last_error());
  T_CHECK(nvvsr_last_error()[0] != 0, "probe failure has no detail text");
  T_CHECK(nvvsr_probe() == expected_probe, "probe result is cached / repeatable");
  nvvsr_config c = good_cfg();
  expect_create(c, expected_probe, "create without usable libs");
  T_CHECK(nvvsr_alloc_host(1 << 20) == nullptr, "alloc_host must return NULL");
  // argument errors still take precedence and need no libs
  c.in_w = 0;
  expect_create(c, NVVSR_ERR_INVALID_ARG, "invalid config with unusable libs");
}

int main(int argc, char **argv) {
  std::string mode = argc > 1 ? argv[1] : "args";
  if (mode == "args") return mode_args();
  if (mode == "status") return mode_status();
  if (mode == "bad_libdir") {
    setenv("NVVSR_LIBDIR", "/nonexistent/nvvfx/libs:/nonexistent/nvidia_vfx.libs", 1);
    expect_unusable(NVVSR_ERR_LIBRARY);
    return tu::finish("test_errors bad_libdir");
  }
  if (mode == "empty_libdir") {
    std::string d = make_tmpdir();
    setenv("NVVSR_LIBDIR", d.c_str(), 1);
    expect_unusable(NVVSR_ERR_LIBRARY);
    rmdir(d.c_str());
    return tu::finish("test_errors empty_libdir");
  }
  if (mode == "no_libdir") {
    std::string d = make_tmpdir();
    unsetenv("NVVSR_LIBDIR");
    setenv("XDG_CONFIG_HOME", d.c_str(), 1);
    setenv("HOME", d.c_str(), 1);
    expect_unusable(NVVSR_ERR_NO_LIBDIR);
    rmdir(d.c_str());
    return tu::finish("test_errors no_libdir");
  }
  if (mode == "libdir_file") {
    const char *real = getenv("NVVSR_LIBDIR");
    if (!real || !*real) {
      std::printf("test_errors libdir_file: SKIP: NVVSR_LIBDIR not set\n");
      return 77;
    }
    std::string value = real;
    std::string d = make_tmpdir();
    std::string sub = d + "/nvvsr";
    mkdir(sub.c_str(), 0755);
    std::string file = sub + "/libdir";
    FILE *f = fopen(file.c_str(), "w");
    fprintf(f, "# written by test_errors\n\n  %s  \n", value.c_str());
    fclose(f);
    unsetenv("NVVSR_LIBDIR");
    setenv("XDG_CONFIG_HOME", d.c_str(), 1);
    int rc = nvvsr_probe();
    std::printf("  probe via config file: %s (%s)\n", nvvsr_strerror(rc), nvvsr_last_error());
    unlink(file.c_str()); rmdir(sub.c_str()); rmdir(d.c_str());
    if (rc == NVVSR_ERR_NO_GPU || rc == NVVSR_ERR_DRIVER) { std::printf("test_errors libdir_file: SKIP: no GPU\n"); return 77; }
    T_CHECK(rc == NVVSR_OK, "probe via libdir file failed: %s [%s]", nvvsr_strerror(rc), nvvsr_last_error());
    return tu::finish("test_errors libdir_file");
  }
  std::fprintf(stderr, "unknown mode %s\n", mode.c_str());
  return 2;
}
