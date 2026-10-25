# VLC 3 module libnvvsr_plugin.so (vout display "nvvsr" + video filter "nvvsr"). Uses only core/include/nvvsr.h
# and libnvvsr.so; the VLC plugin headers come from pkg-config vlc-plugin (/usr/include/vlc/plugins).
#
#   cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core -j       (once)
#   cmake -S vlc  -B vlc/build  -DCMAKE_BUILD_TYPE=Release && cmake --build vlc/build -j
#
# The result, vlc/build/libnvvsr_plugin.so, finds libnvvsr.so.0 through RUNPATH (the core build directory,
# plus $ORIGIN for an installed copy), so VLC needs no LD_LIBRARY_PATH. The NVIDIA wheel libraries are located
# at run time by libnvvsr (NVVSR_LIBDIR or ~/.config/nvvsr/libdir). Only the vlc_entry* symbols are exported.
cmake_minimum_required(VERSION 3.20)
project(nvvsr_vlc VERSION 0.1.0 DESCRIPTION "VLC 3 module for NVIDIA video super resolution" LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)   # VLC's headers use GNU extensions
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NVVSR_VLC_TESTHOOK "Test only: env NVVSR_VLC_FAIL_AFTER=N / NVVSR_VLC_POWER_DIR=dir" OFF)
set(NVVSR_VLC_DISPLAY_SCORE 300 CACHE STRING "Score of the vout display (0 = only with --vout=nvvsr)")
set(NVVSR_CORE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../core/include" CACHE PATH "directory with nvvsr.h")
set(NVVSR_CORE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/core" CACHE PATH "build directory of libnvvsr")

find_package(PkgConfig REQUIRED)
pkg_check_modules(VLC_PLUGIN REQUIRED vlc-plugin>=3.0)
find_library(NVVSR_LIBRARY nvvsr HINTS "${NVVSR_CORE_BUILD_DIR}" NO_DEFAULT_PATH)
find_library(NVVSR_LIBRARY nvvsr REQUIRED)
find_library(MATH_LIBRARY m)
find_package(Threads REQUIRED)

add_library(nvvsr_plugin MODULE src/plugin.c src/common.c src/display.c src/filter.c)
target_include_directories(nvvsr_plugin PRIVATE "${NVVSR_CORE_INCLUDE_DIR}" ${VLC_PLUGIN_INCLUDE_DIRS})
target_compile_definitions(nvvsr_plugin PRIVATE
  __PLUGIN__ _FILE_OFFSET_BITS=64 _REENTRANT _THREAD_SAFE
  "MODULE_STRING=\"nvvsr\""
  NVVSR_VLC_DISPLAY_SCORE=${NVVSR_VLC_DISPLAY_SCORE})
target_compile_options(nvvsr_plugin PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations -fvisibility=hidden)
if(NVVSR_VLC_TESTHOOK)
  target_compile_definitions(nvvsr_plugin PRIVATE NVVSR_VLC_TESTHOOK=1)
endif()

# libnvvsr is linked by SONAME through an INTERFACE target; the RUNPATH is written explicitly (see
# vapoursynth/CMakeLists.txt): the core build directory (development) and $ORIGIN (installed copy).
get_filename_component(NVVSR_LIBRARY_DIR "${NVVSR_LIBRARY}" DIRECTORY)
add_library(nvvsr_core INTERFACE)
target_link_libraries(nvvsr_core INTERFACE "-L${NVVSR_LIBRARY_DIR}" "-l:libnvvsr.so.0")
# libvlccore resolves from the VLC process; link it so -z,defs can check the plugin's undefined symbols.
target_link_libraries(nvvsr_plugin PRIVATE nvvsr_core ${VLC_PLUGIN_LIBRARIES} ${MATH_LIBRARY} Threads::Threads)
target_link_options(nvvsr_plugin PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map"
  "LINKER:-rpath,${NVVSR_LIBRARY_DIR}" "LINKER:-rpath,$ORIGIN")
set_target_properties(nvvsr_plugin PROPERTIES
  PREFIX "lib" OUTPUT_NAME nvvsr_plugin
  LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/plugin.map"
  SKIP_BUILD_RPATH ON SKIP_INSTALL_RPATH ON)

# Unit tests of src/common.c (no VLC instance, no GPU) and the output reference for tests/run.sh.
option(NVVSR_VLC_BUILD_TESTS "Build the unit test and the reference tool used by tests/run.sh" ON)
if(NVVSR_VLC_BUILD_TESTS)
  add_executable(nvvsr_vlc_unit tests/unit.c)
  target_include_directories(nvvsr_vlc_unit PRIVATE "${NVVSR_CORE_INCLUDE_DIR}" ${VLC_PLUGIN_INCLUDE_DIRS})
  target_compile_definitions(nvvsr_vlc_unit PRIVATE _FILE_OFFSET_BITS=64 _REENTRANT _THREAD_SAFE "MODULE_STRING=\"nvvsr\"")
  target_compile_options(nvvsr_vlc_unit PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations)
  target_link_libraries(nvvsr_vlc_unit PRIVATE nvvsr_core ${VLC_PLUGIN_LIBRARIES} ${MATH_LIBRARY} Threads::Threads)
  set_target_properties(nvvsr_vlc_unit PROPERTIES SKIP_BUILD_RPATH ON)
  target_link_options(nvvsr_vlc_unit PRIVATE "LINKER:-rpath,${NVVSR_LIBRARY_DIR}")
endif()

# Output-size/PSNR helper for tests/run.sh (libnvvsr only, no VLC).
if(NVVSR_VLC_BUILD_TESTS)
  add_executable(nvvsr_vlc_ref tests/ref_upscale.c)
  target_include_directories(nvvsr_vlc_ref PRIVATE "${NVVSR_CORE_INCLUDE_DIR}")
  target_link_libraries(nvvsr_vlc_ref PRIVATE nvvsr_core)
  set_target_properties(nvvsr_vlc_ref PROPERTIES SKIP_BUILD_RPATH ON)
  target_link_options(nvvsr_vlc_ref PRIVATE "LINKER:-rpath,${NVVSR_LIBRARY_DIR}")
  target_compile_options(nvvsr_vlc_ref PRIVATE -Wall -Wextra)
endif()

# VLC looks for plugins below <libdir>/vlc/plugins/<category>/; Phase 6 chooses the user or system location.
install(TARGETS nvvsr_plugin LIBRARY DESTINATION lib/vlc/plugins/video_output)
