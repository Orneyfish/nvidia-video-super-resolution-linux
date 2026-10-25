# Phase 0: what the `nvidia-vfx` wheel ships

Installed `nvidia-vfx 0.2.0.0` (internal VFX SDK 1.3.0.0) into `.venv` (created with `--system-site-packages`) on Python 3.14.8. The wheel is `cp312-abi3`, `manylinux_2_27/2_28`, and pip accepted it on 3.14 without the fallback download. No Ollama model was resident at start. No GPU work was done: no effect was created or loaded, and no benchmark was run.

## Verdict

- **(a) C ABI:** Yes. `libVideoFX.so` exports the `NvVFX_*` C API (38 symbols). `libNVCVImage.so` exports `NvCVImage_*` (21 symbols) plus `NvCV_GetErrorStringFromCode`. The feature library `libnvVFXVideoSuperRes.so` exports only C++-mangled internals; it is loaded through the `VideoSuperRes` selector. Loading works: with the wheel's own preload order (`RTLD_GLOBAL`), all 21 bundled libraries mapped, checked via `/proc/self/maps`.
- **(b) Headers:** None are shipped (`find` finds no `.h`/`.hpp`). The VFX SDK docs (docs.nvidia.com/maxine/vfx/latest/) say the C API is declared in `nvVideoEffects.h`, and that the SDK Core package "includes the API headers, library files, and runtime dependencies". Feature installs go under `/usr/local/VideoFX/features`. The NVIDIA GitHub repo suggested (`github.com/NVIDIA/MAXINE-VFX-SDK`) returns 404. A GitHub search found no NVIDIA-org repo for the VFX SDK. Headers must come from the SDK Core download (developer/NGC login, not verified here).
- **(c) Missing here:** CUDA toolkit (no `nvcc`, no `/usr/local/cuda`, no `cuda_runtime.h`). The CUDA runtime is not needed at run time because `libcudart-256e6409.so.12.9.79` is bundled. cuDNN sub-libraries (`libcudnn_{adv,cnn,graph,ops}.so`) are not shipped; only a 125 KB stub is. `/usr/local/VideoFX` (SDK Core) is absent. Driver is fine: `libcuda.so.1` -> `libcuda.so.615.78.08`, RTX 3070 Ti Laptop, 8192 MiB.
- **(d) Surprises:** see the last section.

## Package contents

- Installed size: about 581 MB (`nvvfx/` 577 MB including `__pycache__`, `nvidia_vfx.libs/` 2.1 MB, dist-info 1.7 MB). Download was 443 MB.
- Python deps: none declared (no `Requires-Dist`). `pip check` is clean. The install added only `nvidia-vfx` (numpy 2.5.4 was already in system site-packages). No torch or cupy on this machine.
- Shared libraries (21 files, 578 MB total), largest first:
  - `nvvfx/libs/libnvVFXVideoFrameGeneration.so` 218.1 MB (not needed for VSR)
  - `nvvfx/libs/libnppif.so.12` 128.5 MB, `libnppig.so.12` 55.9 MB, `libnppist.so.12` 49.7 MB, `libnppial.so.12` 22.0 MB, `libnppidei.so.12` 13.6 MB, `libnppim.so.12` 13.2 MB, `libnppicc.so.12` 9.4 MB, `libnppitc.so.12` 6.7 MB, `libnppc.so.12` 1.7 MB (NPP 12.x)
  - `nvvfx/libs/libnvidia-ngx-vsr.so.1.8.3` 59.1 MB (VSR model/runtime)
  - `nvvfx/libs/libNVCVImage.so` 4.9 MB, `libVideoFX.so` 4.2 MB, `libVideoFXLocal.so` 4.2 MB, `libnvVFXVideoSuperRes.so` 4.2 MB, `libnvVFXTrueHDR.so` 4.2 MB, `libnvidia-ngx-truehdr.so.1.2.3` 3.8 MB
  - `nvvfx/libs/libcudnn.so.9` 0.125 MB (stub), `libnvngxruntime.so` 0.08 MB
  - `nvidia_vfx.libs/libcudart-256e6409.so.12.9.79` 2.1 MB
  - `nvvfx/_ext.abi3.so` 0.14 MB (pybind module)
- Model/weight files: none as separate files (no `.onnx/.engine/.plan/.pb/.bin`). Model data appears to be inside `libnvidia-ngx-vsr.so` (inferred, not verified). `ModelDir` is a parameter name in `libVideoFXLocal.so`, not a path.
- Headers: none. Licences: 3 NVIDIA PDFs, `ThirdPartyLicenses_VFX_SDK.txt`, `THIRD_PARTY.md`.
- The `.so` files are mode `rw-r--r--`. `ldd` warns about this. `dlopen` does not need the execute bit.

## Exported C symbols

`libVideoFX.so` (`NvVFX_*`, 38): AllocateState, ConfigureLogger, ConnectTritonServer, CreateEffect, CreateEffectTriton, CudaStreamCreate/Destroy/Synchronize, DeallocateState, DestroyEffect, DisconnectTritonServer, GetCudaStream, GetF32/F64/Image/Object/S32/String/U32/U64, GetVersion, GetVersionTriton, IsUsingTriton, Load, ResetState, Run, SetCudaStream, SetF32/F64/Image/Object/S32/String/U32/U64, SetStateObjectHandleArray, SetTritonServer, SynchronizeTriton.

`libVideoFXLocal.so`: same local set, without the Triton functions.

`libNVCVImage.so` (`NvCVImage_*`, 21): Alloc, ComponentOffsets, Composite, CompositeOverConstant, CompositeRect, Create, Dealloc, DeallocAsync, Destroy, FlipY, GetYUVPointers, Init, InitView, MapResource, Realloc, Reshape, Sharpen, Transfer, TransferFromYUV, TransferRect, TransferToYUV, UnmapResource.

The `_ext.abi3.so` imports exactly these C calls: `NvVFX_CreateEffect`, `SetU32/SetF32/SetS32/SetF64/SetString`, `SetImage`, `Load`, `Run`, `SetCudaStream`, `CudaStreamCreate/Destroy`, `GetU32/GetF32/...`, `DestroyEffect`, `NvCVImage_Alloc/Dealloc/Transfer`, `NvCV_GetErrorStringFromCode`. This is a working reference for the call sequence.

## Dependencies (`ldd` and loader)

- `RPATH` on the feature libs is `$ORIGIN/../../nvidia_vfx.libs` only. The sibling libs in `nvvfx/libs/` are therefore reported `not found` by `ldd` (NVCVImage, npp*, cudnn, VideoFXLocal, ngxruntime). They resolve only because `nvvfx/_lib_loader.py` preloads them in a fixed order with `ctypes.CDLL(..., RTLD_GLOBAL)`.
- Preload order (`_LINUX_LIBS`): npp (`libnppc`, `libnppial`, `libnppicc`, `libnppidei`, `libnppif`, `libnppig`, `libnppim`, `libnppist`, `libnppitc`), `libcudnn.so.9`, `libNVCVImage.so`, `libnvngxruntime.so`, `libVideoFXLocal.so`, `libVideoFX.so`, `libnvVFXVideoSuperRes.so`, `libnvidia-ngx-vsr.so.1.8.3`, then the VFG and TrueHDR libs.
- `_lib_loader` also sets `LD_LIBRARY_PATH` in `os.environ`. That has no effect on the running process, since the dynamic loader reads it only at exec. A C++ core must preload by absolute path, or set `LD_LIBRARY_PATH` before exec.
- `_lib_loader` swallows `OSError`, so a successful `import nvvfx` does not prove every library loaded. I checked `/proc/self/maps`: all 21 mapped.
- The cuDNN stub's sub-libraries are not shipped. The stub mapped, but `libVideoFXLocal.so` has `NEEDED libcudnn.so.9`. Whether the VSR path ever calls cuDNN is not determined statically.
- `libcuda.so.1` (driver) is the only external library not bundled.

## Python API facts (from `nvvfx/effects/video_super_res.py`)

- Selector: `"VideoSuperRes"`.
- Parameters, set by string name: `"QualityLevel"` (u32), `"Strength"` (f32, range [0.0, 1.0], `ValueError` outside it), `"ImageEncodingMode"` (u32). The VSR docs call the strength selector `NVVFX_STRENGTH`.
- `QualityLevel` values: BICUBIC=0, LOW=1, MEDIUM=2, HIGH=3 (default), ULTRA=4; DENOISE_LOW..ULTRA=8..11; DEBLUR_LOW..ULTRA=12..15; HIGHBITRATE_LOW..ULTRA=16..19; STREAMING_MEDIUM=21, STREAMING_ULTRA=23. Values 5-7, 20, 22 are reserved.
- `ImageEncoding`: RGB8=0, RGB10A2=1.
- Input (RGB8): `(3, H, W)` float32 CUDA, contiguous, values in [0,1], planar. Input (RGB10A2): `(H, W)` uint32, packed R[9:0] G[19:10] B[29:20] A[31:30]. Errors: "must be contiguous", "must be float32", "must have 3 channels", "must be on CUDA device".
- Output: DLPack capsule `(3, out_h, out_w)` float32 (RGB8) or `(out_h, out_w)` uint32 (RGB10A2). The buffer is owned by the effect and is overwritten on the next `run()`, so the caller must copy first.
- Order: set `output_width` and `output_height` (each setter calls `set_output_image` once both are set), then `load()`, then `run()`. Changing `image_encoding` after `load()` sets the reload flag, and `run()` raises until `load()` is called again. `quality`, `strength` and the output size can change between calls without a reload (per the class docstring).
- `run()` sequence: `set_input_image(w,h)`, `set_cuda_stream(stream_ptr)`, `transfer_input(array, stream_ptr)`, `run(non_blocking)`, `get_output(device)`. `stream_ptr` is a raw CUDA stream pointer (0 = default stream). `non_blocking` is passed to `run()` on the C side; it presumably maps to the `async` flag of `NvVFX_Run` (inferred, not read from `_ext`).
- Temporal state: the Python layer exposes none. The C library does export `NvVFX_AllocateState`, `ResetState`, `DeallocateState` and `SetStateObjectHandleArray` through a generic `VFXFeatureImpl` base. The bindings do not call them. Whether VSR uses state is undetermined.
- Scale limits: no limit is enforced in Python. README says "up to 4x". Denoise and deblur modes require output == input resolution (enforced in C: "Denoise and Deblur modes do not support a change in resolution"). The docs recommend a minimum input of 360p and say the aspect ratio is unrestricted but best kept the same.

## C-level image contract (from the VSR filter strings and the VFX docs)

- The C filter takes `SrcImage0` and `DstImage0` as `NvCVImage*` in `NVCV_CUDA` or `NVCV_CPU_PINNED` memory.
- Mode 0 (8-bit): chunky `RGBA` or `BGRA` `U8`. Mode 1 (10-bit): chunky `RGB10A2`.
- Planar layouts are rejected: "image must be chunky/interleaved layout (planar=%d)".
- Output is the same format and depth as input, upscaled.
- `Strength` must be in [0.0, 1.0].
- The Python layer converts planar float32 RGB into this layout internally. This is inferred from the strings, not read from the code. A C++ core should feed interleaved RGBA U8 directly, which matches the planned `YUV→RGBA` stage in CLAUDE.md.
- `NvVFX_Run(handle, int async)`: 1 = asynchronous, 0 = synchronous.

## Surprises

1. The wheel bundles the feature libraries (VSR, VFG, TrueHDR and their model/runtime libs). The SDK docs say features are normally delivered through NGC, and the SDK Core (headers) is separate. So the wheel is a source of the feature libraries, but the headers are not in it. The licence terms for using wheel-only libraries are for the user to judge.
2. The wheel is 578 MB of `.so`, of which 218 MB is the VFG library a VSR-only core does not need. A C++ core can preload only the VSR chain.
3. The headers are the blocker, not the libraries. Hand-writing `NvCVImage` and the `NvVFX_*` prototypes is possible but fragile without the SDK headers.
4. The cuDNN stub is bundled but its sub-libraries are not, and the loader hides load failures. Whether VSR needs cuDNN is unknown until a smoke test runs.
5. The CUDA runtime is bundled (12.9.79), so no CUDA toolkit is needed at run time. A C++ core that includes CUDA headers at compile time would still need them, since `cuda_runtime.h` is not present.
6. CLAUDE.md says "VFX SDK 1.3". The wheel reports `get_sdk_version() == 1.3.0`, which matches.
7. The `.venv` (gitignored: `.venv/`, `*.so`) sees 144 system site-packages because of `--system-site-packages`.

## Not done

- No effect was created or loaded on the GPU, and no VRAM was used. A minimal create/load/run on a 360p test frame would settle the cuDNN question and confirm GPU init. It needs the user's go-ahead, since it is GPU work and the phase brief said to hold off on GPU work.
- The SDK Core headers were not downloaded. They need the NVIDIA developer or NGC login.
