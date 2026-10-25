#include "loader.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace nvvsr {

// ---------------------------------------------------------------------------
// thread-local error text
// ---------------------------------------------------------------------------
static thread_local std::string g_err;

void set_error_str(const std::string &s) { g_err = s; }

void set_error(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  g_err = buf;
}

const char *last_error() { return g_err.c_str(); }

int map_nvcv(NvCV_Status s, const char *what) {
  if (s == NVCV_SUCCESS) return NVVSR_OK;
  const char *txt = "?";
  try {
    if (api().ErrorString) txt = api().ErrorString(s);
  } catch (...) {
  }
  set_error("%s: %s (NvCV status %d)", what, txt ? txt : "?", (int)s);
  switch (s) {
    case NVCV_ERR_MEMORY:
    case NVCV_ERR_CUDA_MEMORY:
      return NVVSR_ERR_OUT_OF_MEMORY;
    case NVCV_ERR_UNSUPPORTEDDRIVER:
    case NVCV_ERR_CUDA_DRIVER:
      return NVVSR_ERR_DRIVER;
    case NVCV_ERR_UNSUPPORTEDGPU:
    case NVCV_ERR_WRONGGPU:
    case NVCV_ERR_CUDA_INIT:
      return NVVSR_ERR_NO_GPU;
    case NVCV_ERR_LIBRARY:
      return NVVSR_ERR_LIBRARY;
    case NVCV_ERR_RESOLUTION:
    case NVCV_ERR_TOOBIG:
    case NVCV_ERR_PIXELFORMAT:
    case NVCV_ERR_CONFIG:
    case NVCV_ERR_UNIMPLEMENTED:
      return NVVSR_ERR_UNSUPPORTED;
    default:
      return NVVSR_ERR_EFFECT;
  }
}

// ---------------------------------------------------------------------------
// library directory resolution
// ---------------------------------------------------------------------------
static std::vector<std::string> split_colon(const std::string &s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i <= s.size()) {
    size_t j = s.find(':', i);
    if (j == std::string::npos) j = s.size();
    std::string part = s.substr(i, j - i);
    // trim spaces
    size_t a = part.find_first_not_of(" \t\r\n"), b = part.find_last_not_of(" \t\r\n");
    if (a != std::string::npos) out.push_back(part.substr(a, b - a + 1));
    i = j + 1;
  }
  return out;
}

static std::string expand_home(const std::string &p) {
  if (p.size() >= 2 && p[0] == '~' && p[1] == '/') {
    const char *h = std::getenv("HOME");
    if (h && *h) return std::string(h) + p.substr(1);
  }
  return p;
}

// Returns NVVSR_OK and fills dirs, or NVVSR_ERR_NO_LIBDIR.
static int resolve_libdirs(std::vector<std::string> &dirs, std::string &source) {
  const char *env = std::getenv("NVVSR_LIBDIR");
  if (env && *env) {
    dirs = split_colon(env);
    source = "environment NVVSR_LIBDIR";
  } else {
    std::string cfg;
    const char *xdg = std::getenv("XDG_CONFIG_HOME");
    const char *home = std::getenv("HOME");
    if (xdg && *xdg) cfg = std::string(xdg) + "/nvvsr/libdir";
    else if (home && *home) cfg = std::string(home) + "/.config/nvvsr/libdir";
    if (!cfg.empty()) {
      std::ifstream f(cfg);
      std::string line;
      while (f && std::getline(f, line)) {
        size_t a = line.find_first_not_of(" \t\r\n");
        if (a == std::string::npos || line[a] == '#') continue;
        dirs = split_colon(line);
        source = "file " + cfg;
        break;
      }
    }
  }
  for (auto &d : dirs) d = expand_home(d);
  if (dirs.empty()) {
    set_error("no library directory configured: set NVVSR_LIBDIR=<site-packages>/nvvfx/libs:<site-packages>/nvidia_vfx.libs "
              "or write that line to ~/.config/nvvsr/libdir");
    return NVVSR_ERR_NO_LIBDIR;
  }
  return NVVSR_OK;
}

static bool file_exists(const std::string &p) {
  struct stat st;
  return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static std::string find_in_dirs(const std::vector<std::string> &dirs, const char *name) {
  for (const auto &d : dirs) {
    std::string p = d + "/" + name;
    if (file_exists(p)) return p;
  }
  return std::string();
}

// libcudart-<hash>.so.12.x in the wheel's nvidia_vfx.libs: found by prefix, since the hash varies.
static std::string find_cudart(const std::vector<std::string> &dirs) {
  for (const auto &d : dirs) {
    DIR *dir = ::opendir(d.c_str());
    if (!dir) continue;
    std::string found;
    while (struct dirent *e = ::readdir(dir)) {
      if (std::strncmp(e->d_name, "libcudart", 9) == 0 && std::strstr(e->d_name, ".so")) {
        found = d + "/" + e->d_name;
        break;
      }
    }
    ::closedir(dir);
    if (!found.empty()) return found;
  }
  return std::string();
}

// ---------------------------------------------------------------------------
// one-time initialisation
// ---------------------------------------------------------------------------
namespace {

struct InitState {
  int status = NVVSR_ERR_INTERNAL;
  std::string detail;
  Api api;
};

InitState &state() {
  static InitState s;
  return s;
}

std::once_flag g_once;

template <class T>
bool sym(void *h, const char *name, T &out, std::string &err) {
  void *p = ::dlsym(h, name);
  if (!p) {
    err = std::string("symbol not found: ") + name;
    return false;
  }
  out = reinterpret_cast<T>(p);
  return true;
}

// The VSR chain, in the order of nvvfx/_lib_loader.py (_LINUX_LIBS), without the
// 218 MB VideoFrameGeneration and the TrueHDR libraries.
const char *const kChain[] = {
    "libnppc.so.12",  "libnppial.so.12", "libnppicc.so.12", "libnppidei.so.12", "libnppif.so.12",
    "libnppig.so.12", "libnppim.so.12",  "libnppist.so.12", "libnppitc.so.12",  "libcudnn.so.9",
    "libNVCVImage.so", "libnvngxruntime.so", "libVideoFXLocal.so", "libVideoFX.so",
    "libnvVFXVideoSuperRes.so", "libnvidia-ngx-vsr.so.1.8.3",
};

// Minimal driver-API probe: dlopen libcuda, cuInit, version, device count. Creates no context.
int check_driver(std::string &err) {
  void *cu = ::dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!cu) {
    const char *e = ::dlerror();
    err = std::string("NVIDIA driver library libcuda.so.1 not found: ") + (e ? e : "?");
    return NVVSR_ERR_NO_GPU;
  }
  using cuInit_t = int (*)(unsigned);
  using cuVer_t = int (*)(int *);
  using cuCount_t = int (*)(int *);
  cuInit_t cuInit = reinterpret_cast<cuInit_t>(::dlsym(cu, "cuInit"));
  cuVer_t cuVer = reinterpret_cast<cuVer_t>(::dlsym(cu, "cuDriverGetVersion"));
  cuCount_t cuCount = reinterpret_cast<cuCount_t>(::dlsym(cu, "cuDeviceGetCount"));
  if (!cuInit || !cuVer || !cuCount) {
    err = "libcuda.so.1 lacks cuInit/cuDriverGetVersion/cuDeviceGetCount";
    return NVVSR_ERR_NO_GPU;
  }
  int r = cuInit(0);
  if (r != 0) {
    char b[96];
    std::snprintf(b, sizeof b, "cuInit failed (CUDA error %d): no usable NVIDIA GPU/driver", r);
    err = b;
    return NVVSR_ERR_NO_GPU;
  }
  int ver = 0, n = 0;
  cuVer(&ver);
  cuCount(&n);
  if (n <= 0) {
    err = "no CUDA device found";
    return NVVSR_ERR_NO_GPU;
  }
  if (ver < 12080) {  // driver >= 570 corresponds to CUDA 12.8
    char b[160];
    std::snprintf(b, sizeof b, "NVIDIA driver supports only CUDA %d.%d, the VFX SDK needs a driver >= 570.190 (CUDA 12.8)",
                  ver / 1000, (ver % 1000) / 10);
    err = b;
    return NVVSR_ERR_DRIVER;
  }
  return NVVSR_OK;
}

void do_init() {
  InitState &st = state();
  try {
    std::vector<std::string> dirs;
    std::string source;
    int rc = resolve_libdirs(dirs, source);
    if (rc != NVVSR_OK) {
      st.status = rc;
      st.detail = last_error();
      return;
    }
    // 1. all files must exist (reported before any driver problem: the answer to "is the configured
    //    directory right?" must not depend on the machine having a GPU)
    std::vector<std::string> paths;
    for (const char *name : kChain) {
      std::string p = find_in_dirs(dirs, name);
      if (p.empty()) {
        st.status = NVVSR_ERR_LIBRARY;
        st.detail = std::string(name) + " not found in " + source + " (";
        for (size_t i = 0; i < dirs.size(); ++i) st.detail += (i ? ":" : "") + dirs[i];
        st.detail += ")";
        return;
      }
      paths.push_back(p);
    }

    // 2. the NVIDIA driver
    std::string derr;
    rc = check_driver(derr);
    if (rc != NVVSR_OK) {
      st.status = rc;
      st.detail = derr;
      return;
    }

    // Same mode as ctypes.CDLL(path, RTLD_GLOBAL) in the wheel's own loader. RTLD_LOCAL also works
    // for the VSR chain (verified: dependencies resolve by SONAME) and keeps the process's global
    // symbol namespace clean; NVVSR_DLOPEN_LOCAL=1 selects it.
    const char *loc = std::getenv("NVVSR_DLOPEN_LOCAL");
    const int mode = RTLD_NOW | ((loc && loc[0] == '1') ? RTLD_LOCAL : RTLD_GLOBAL);

    // CUDA runtime of the wheel first (optional: the feature libs also find it via RPATH).
    std::string cudart = find_cudart(dirs);
    void *hRt = cudart.empty() ? nullptr : ::dlopen(cudart.c_str(), mode);

    void *hVfx = nullptr, *hCv = nullptr;
    for (size_t i = 0; i < paths.size(); ++i) {
      void *h = ::dlopen(paths[i].c_str(), mode);
      if (!h) {
        const char *e = ::dlerror();
        st.status = NVVSR_ERR_LIBRARY;
        st.detail = "dlopen " + paths[i] + " failed: " + (e ? e : "?");
        return;
      }
      if (!std::strcmp(kChain[i], "libVideoFX.so")) hVfx = h;
      if (!std::strcmp(kChain[i], "libNVCVImage.so")) hCv = h;
    }

    Api &a = st.api;
    std::string serr;
    bool ok = sym(hVfx, "NvVFX_CreateEffect", a.CreateEffect, serr) &&
              sym(hVfx, "NvVFX_DestroyEffect", a.DestroyEffect, serr) &&
              sym(hVfx, "NvVFX_SetImage", a.SetImage, serr) && sym(hVfx, "NvVFX_SetU32", a.SetU32, serr) &&
              sym(hVfx, "NvVFX_SetF32", a.SetF32, serr) && sym(hVfx, "NvVFX_SetCudaStream", a.SetCudaStream, serr) &&
              sym(hVfx, "NvVFX_Load", a.Load, serr) && sym(hVfx, "NvVFX_Run", a.Run, serr) &&
              sym(hVfx, "NvVFX_CudaStreamCreate", a.CudaStreamCreate, serr) &&
              sym(hVfx, "NvVFX_CudaStreamDestroy", a.CudaStreamDestroy, serr) &&
              sym(hVfx, "NvVFX_CudaStreamSynchronize", a.CudaStreamSynchronize, serr) &&
              sym(hCv, "NvCVImage_Init", a.ImgInit, serr) && sym(hCv, "NvCVImage_Alloc", a.ImgAlloc, serr) &&
              sym(hCv, "NvCVImage_Dealloc", a.ImgDealloc, serr) && sym(hCv, "NvCVImage_Transfer", a.ImgTransfer, serr) &&
              sym(hCv, "NvCVImage_GetYUVPointers", a.ImgGetYUVPointers, serr) &&
              sym(hCv, "NvCV_GetErrorStringFromCode", a.ErrorString, serr);
    if (!ok) {
      st.status = NVVSR_ERR_LIBRARY;
      st.detail = serr + " (wheel version mismatch?)";
      return;
    }
    if (hRt) {  // optional: used for the free-VRAM guard in nvvsr_create
      void *p = ::dlsym(hRt, "cudaMemGetInfo");
      a.MemGetInfo = reinterpret_cast<int (*)(size_t *, size_t *)>(p);
    }
    st.status = NVVSR_OK;
    st.detail.clear();
  } catch (const std::exception &e) {
    st.status = NVVSR_ERR_INTERNAL;
    st.detail = std::string("exception during init: ") + e.what();
  } catch (...) {
    st.status = NVVSR_ERR_INTERNAL;
    st.detail = "unknown exception during init";
  }
}

}  // namespace

int ensure_init() {
  std::call_once(g_once, do_init);
  InitState &st = state();
  if (st.status != NVVSR_OK) set_error_str(st.detail);
  return st.status;
}

const Api &api() { return state().api; }

}  // namespace nvvsr
