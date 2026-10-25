// Internal: run-time loader for the nvidia-vfx wheel libraries (no link-time dependency).
#pragma once

#include <string>

#include "../include/nvvsr.h"
#include "../third_party/maxine/nvCVImage.h"
#include "../third_party/maxine/nvVideoEffects.h"

namespace nvvsr {

// Function-pointer table for the few SDK entry points the core needs. Types are taken
// from the vendored headers via decltype (a declaration only, no link dependency).
struct Api {
  decltype(&::NvVFX_CreateEffect) CreateEffect = nullptr;
  decltype(&::NvVFX_DestroyEffect) DestroyEffect = nullptr;
  decltype(&::NvVFX_SetImage) SetImage = nullptr;
  decltype(&::NvVFX_SetU32) SetU32 = nullptr;
  decltype(&::NvVFX_SetF32) SetF32 = nullptr;
  decltype(&::NvVFX_SetCudaStream) SetCudaStream = nullptr;
  decltype(&::NvVFX_Load) Load = nullptr;
  decltype(&::NvVFX_Run) Run = nullptr;
  decltype(&::NvVFX_CudaStreamCreate) CudaStreamCreate = nullptr;
  decltype(&::NvVFX_CudaStreamDestroy) CudaStreamDestroy = nullptr;
  // Exported by the SDK 1.3 libVideoFX.so but missing from the v0.7.6 public headers.
  NvCV_Status (*CudaStreamSynchronize)(CUstream) = nullptr;

  decltype(&::NvCVImage_Init) ImgInit = nullptr;
  decltype(&::NvCVImage_Alloc) ImgAlloc = nullptr;
  decltype(&::NvCVImage_Dealloc) ImgDealloc = nullptr;
  decltype(&::NvCVImage_Transfer) ImgTransfer = nullptr;
  decltype(&::NvCVImage_GetYUVPointers) ImgGetYUVPointers = nullptr;
  decltype(&::NvCV_GetErrorStringFromCode) ErrorString = nullptr;

  // From the wheel's bundled CUDA runtime (may be null if the runtime was not found): free/total VRAM
  // of the current device. cudaError_t is 0 on success.
  int (*MemGetInfo)(size_t *free_bytes, size_t *total_bytes) = nullptr;
};

// Thread-safe lazy initialisation (std::call_once): finds the library directories,
// checks the CUDA driver and dlopens + dlsyms the VSR chain. The result (including the
// failure reason) is cached for the life of the process. Returns an nvvsr_status.
int ensure_init();

// Valid only after ensure_init() returned NVVSR_OK.
const Api &api();

// Thread-local error detail (nvvsr_last_error()).
void set_error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void set_error_str(const std::string &s);
const char *last_error();

// Maps an SDK status to an nvvsr_status and records "<what>: <sdk text> (<code>)".
int map_nvcv(NvCV_Status s, const char *what);

}  // namespace nvvsr
