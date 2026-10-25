# nvvsr VapourSynth plugin (libvsnvvsr.so). Uses only core/include/nvvsr.h and libnvvsr.so.
#
#   cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core -j     (once)
#   cmake -S vapoursynth -B vapoursynth/build -DCMAKE_BUILD_TYPE=Release && cmake --build vapoursynth/build -j
#
# The result, vapoursynth/build/libvsnvvsr.so, finds libnvvsr.so through RUNPATH (the directory of the
# core build, plus $ORIGIN for an installed copy), so no LD_LIBRARY_PATH is needed. The NVIDIA wheel
# libraries are located at run time by libnvvsr (NVVSR_LIBDIR or ~/.config/nvvsr/libdir).
cmake_minimum_required(VERSION 3.20)
project(vsnvvsr VERSION 0.1.0 DESCRIPTION "VapourSynth plugin for NVIDIA video super resolution" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NVVSR_VS_TESTHOOK "Test only: honour env NVVSR_VS_FAIL_AFTER=N (fail every frame after N)" OFF)
set(NVVSR_CORE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" CACHE PATH "directory with nvvsr.h")
set(NVVSR_CORE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/core" CACHE PATH "build directory of libnvvsr")

find_path(VAPOURSYNTH_INCLUDE_DIR VapourSynth4.h PATH_SUFFIXES vapoursynth REQUIRED)
find_library(NVVSR_LIBRARY nvvsr HINTS "${NVVSR_CORE_BUILD_DIR}" NO_DEFAULT_PATH)
find_library(NVVSR_LIBRARY nvvsr REQUIRED)
find_package(Threads REQUIRED)

add_library(vsnvvsr MODULE src/nvvsr_vs.cpp)
target_include_directories(vsnvvsr PRIVATE "${VAPOURSYNTH_INCLUDE_DIR}" "${NVVSR_CORE_INCLUDE_DIR}")
# libnvvsr is linked by SONAME through an INTERFACE target: CMake's own RUNPATH computation for an
# library target leaves an empty (= current directory) entry in the RUNPATH of a shared object, so the
# RUNPATH is written explicitly instead: the core build dir (development) and $ORIGIN (an installed copy
# with libnvvsr.so.0 next to the plugin).
get_filename_component(NVVSR_LIBRARY_DIR "${NVVSR_LIBRARY}" DIRECTORY)
add_library(nvvsr_core INTERFACE)
target_link_libraries(nvvsr_core INTERFACE "-L${NVVSR_LIBRARY_DIR}" "-l:libnvvsr.so.0")
target_link_libraries(vsnvvsr PRIVATE nvvsr_core Threads::Threads)
target_compile_options(vsnvvsr PRIVATE -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
if(NVVSR_VS_TESTHOOK)
  target_compile_definitions(vsnvvsr PRIVATE NVVSR_VS_TESTHOOK=1)
endif()
# Export only VapourSynthPluginInit2. libnvvsr.so.0 is found in the core build dir (build tree) or
# next to the plugin (installed copy).
target_link_options(vsnvvsr PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map"
  "LINKER:-rpath,${NVVSR_LIBRARY_DIR}" "LINKER:-rpath,$ORIGIN")
set_target_properties(vsnvvsr PROPERTIES
  PREFIX "lib" OUTPUT_NAME vsnvvsr
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map"
  SKIP_BUILD_RPATH ON SKIP_INSTALL_RPATH ON)

# Reference tool for tests/run.sh (libnvvsr only, no VapourSynth).
option(NVVSR_VS_BUILD_TESTS "Build the reference tool used by tests/run.sh" ON)
if(NVVSR_VS_BUILD_TESTS)
  add_executable(nvvsr_vs_ref tests/ref_upscale.cpp)
  target_include_directories(nvvsr_vs_ref PRIVATE "${NVVSR_CORE_INCLUDE_DIR}")
  target_link_libraries(nvvsr_vs_ref PRIVATE nvvsr_core)
  set_target_properties(nvvsr_vs_ref PROPERTIES SKIP_BUILD_RPATH ON)
  target_link_options(nvvsr_vs_ref PRIVATE "LINKER:-rpath,${NVVSR_LIBRARY_DIR}")
  target_compile_options(nvvsr_vs_ref PRIVATE -Wall -Wextra)
endif()

install(TARGETS vsnvvsr LIBRARY DESTINATION lib/nvvsr)
