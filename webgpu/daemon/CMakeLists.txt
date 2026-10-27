# nvvsr_webd: localhost WebSocket daemon that upscales frames from a browser extension with libnvvsr. Uses only
# core/include/nvvsr.h and libnvvsr.so.
#
#   cmake -S core -B build/core -DCMAKE_BUILD_TYPE=Release && cmake --build build/core -j     (once)
#   cmake -S webgpu/daemon -B webgpu/daemon/build -DCMAKE_BUILD_TYPE=Release && cmake --build webgpu/daemon/build -j
#   NVVSR_LIBDIR=$(tools/libdir.sh) ctest --test-dir webgpu/daemon/build
cmake_minimum_required(VERSION 3.20)
project(nvvsr_webd VERSION 0.1.0 DESCRIPTION "WebSocket front end of libnvvsr for the browser extension" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(NVVSR_CORE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../core/include" CACHE PATH "directory with nvvsr.h")
set(NVVSR_CORE_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../build/core" CACHE PATH "build directory of libnvvsr")
find_library(NVVSR_LIBRARY nvvsr HINTS "${NVVSR_CORE_BUILD_DIR}" NO_DEFAULT_PATH)
find_library(NVVSR_LIBRARY nvvsr REQUIRED)
find_package(Threads REQUIRED)

# libnvvsr by SONAME through an INTERFACE target with an explicit RUNPATH (core build dir, $ORIGIN for an installed
# copy): the same pattern as vapoursynth/ and vlc/.
get_filename_component(NVVSR_LIBRARY_DIR "${NVVSR_LIBRARY}" DIRECTORY)
add_library(nvvsr_core INTERFACE)
target_link_libraries(nvvsr_core INTERFACE "-L${NVVSR_LIBRARY_DIR}" "-l:libnvvsr.so.0")

add_executable(nvvsr_webd nvvsr_webd.cpp)
target_include_directories(nvvsr_webd PRIVATE "${NVVSR_CORE_INCLUDE_DIR}")
target_link_libraries(nvvsr_webd PRIVATE nvvsr_core Threads::Threads)
target_compile_options(nvvsr_webd PRIVATE -Wall -Wextra)
set_target_properties(nvvsr_webd PROPERTIES SKIP_BUILD_RPATH ON SKIP_INSTALL_RPATH ON)
target_link_options(nvvsr_webd PRIVATE "LINKER:-rpath,${NVVSR_LIBRARY_DIR}" "LINKER:-rpath,$ORIGIN")

option(NVVSR_WEBD_BUILD_TESTS "Build the ctest program (needs the GPU; exits 77 = skip without it)" ON)
if(NVVSR_WEBD_BUILD_TESTS)
  enable_testing()
  execute_process(COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/../../tools/libdir.sh" OUTPUT_VARIABLE NVVSR_TEST_LIBDIR
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  add_executable(test_webd tests/test_webd.cpp)
  add_dependencies(test_webd nvvsr_webd)
  target_include_directories(test_webd PRIVATE "${NVVSR_CORE_INCLUDE_DIR}")
  target_compile_definitions(test_webd PRIVATE "NVVSR_WEBD_BIN=\"$<TARGET_FILE:nvvsr_webd>\"")
  target_link_libraries(test_webd PRIVATE nvvsr_core Threads::Threads)
  target_compile_options(test_webd PRIVATE -Wall -Wextra)
  set_target_properties(test_webd PROPERTIES SKIP_BUILD_RPATH ON)
  target_link_options(test_webd PRIVATE "LINKER:-rpath,${NVVSR_LIBRARY_DIR}")
  add_test(NAME test_webd COMMAND test_webd)
  # A NVVSR_LIBDIR in the caller's environment wins over the default (the test applies it with setenv overwrite = 0).
  set_tests_properties(test_webd PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 300 ENVIRONMENT "NVVSR_LIBDIR_DEFAULT=${NVVSR_TEST_LIBDIR}")
endif()
