# ctest suite for libnvvsr. GPU tests exit 77 (SKIP) when nvvsr_probe() says SR is unavailable;
# run them with NVVSR_LIBDIR=<site-packages>/nvvfx/libs:<site-packages>/nvidia_vfx.libs
function(nvvsr_exe name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE nvvsr Threads::Threads)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../tools)
  target_compile_options(${name} PRIVATE -Wall -Wextra)
endfunction()

# Sanitizer settings (ignored by non-sanitizer builds). protect_shadow_gap=0 and replace_intrin=0
# are REQUIRED for CUDA under AddressSanitizer: without them cuInit fails with CUDA error 2 and every
# GPU test silently skips. Leaks inside NVIDIA's libraries are suppressed (see lsan.supp).
set(NVVSR_SAN_ENV
  "ASAN_OPTIONS=protect_shadow_gap=0:replace_intrin=0"
  "LSAN_OPTIONS=suppressions=${CMAKE_CURRENT_SOURCE_DIR}/lsan.supp:print_suppressions=0")

function(nvvsr_add_test tname exe)
  add_test(NAME ${tname} COMMAND ${exe} ${ARGN})
  set_tests_properties(${tname} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 900 ENVIRONMENT "${NVVSR_SAN_ENV}")
endfunction()

nvvsr_exe(test_errors)
nvvsr_exe(test_api)
nvvsr_exe(test_formats)
nvvsr_exe(test_quality)
nvvsr_exe(test_color)
nvvsr_exe(test_stress)
nvvsr_exe(test_timing)
nvvsr_exe(test_smallstack)
nvvsr_exe(test_initfail)

# 1. probe + error paths (the first three need no GPU)
nvvsr_add_test(errors_args test_errors args)
nvvsr_add_test(errors_status test_errors status)
nvvsr_add_test(errors_bad_libdir test_errors bad_libdir)
nvvsr_add_test(errors_empty_libdir test_errors empty_libdir)
nvvsr_add_test(errors_no_libdir test_errors no_libdir)
nvvsr_add_test(errors_libdir_file test_errors libdir_file)
nvvsr_add_test(api test_api)
nvvsr_add_test(api_nocache test_api nocache)
# 2. formats / dimensions
nvvsr_add_test(formats test_formats)
# 3. quality vs ground truth
nvvsr_add_test(quality test_quality)
# 4. colour correctness
nvvsr_add_test(color test_color)
# 5. stress / leaks
nvvsr_add_test(stress_sequential test_stress seq)
nvvsr_add_test(stress_leak_info test_stress leak_nocache)
nvvsr_add_test(stress_concurrent test_stress par)
nvvsr_add_test(vram test_stress vram)
nvvsr_add_test(oom test_stress oom)   # skips unless NVVSR_TEST_OOM=1
# 6. timing report
nvvsr_add_test(timing test_timing)

# 7. create/process/destroy on small thread stacks (hosts like VLC use 1 MiB)
nvvsr_add_test(smallstack test_smallstack)
# 8. no init thread -> clean error, never an init on the caller's stack
nvvsr_add_test(initfail test_initfail)

# Phase 0 smoke test (links the wheel libs directly, needs NVVSR_WHEEL_LIBDIRS)
if(NVVSR_BUILD_SMOKE)
  if(NOT NVVSR_WHEEL_LIBDIRS)
    message(FATAL_ERROR "NVVSR_BUILD_SMOKE needs -DNVVSR_WHEEL_LIBDIRS=<nvvfx/libs>;<nvidia_vfx.libs>")
  endif()
  add_executable(vsr_smoke vsr_smoke.cpp)
  target_include_directories(vsr_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/maxine)
  list(GET NVVSR_WHEEL_LIBDIRS 0 _vfx_dir)
  target_link_directories(vsr_smoke PRIVATE ${_vfx_dir})
  target_link_libraries(vsr_smoke PRIVATE VideoFX NVCVImage)
  string(REPLACE ";" ":" _vfx_path "${NVVSR_WHEEL_LIBDIRS}")
  target_link_options(vsr_smoke PRIVATE "LINKER:-rpath-link,${_vfx_path}")
  add_test(NAME vsr_smoke COMMAND vsr_smoke 1280 720 2560 1440 3 20 --state)
  set_tests_properties(vsr_smoke PROPERTIES ENVIRONMENT "LD_LIBRARY_PATH=${_vfx_path}" TIMEOUT 120)
endif()
