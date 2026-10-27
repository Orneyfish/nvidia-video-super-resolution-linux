cmake_minimum_required(VERSION 3.20)
project(nvvsr VERSION 0.1.0 DESCRIPTION "NVIDIA video super resolution core" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(NVVSR_BUILD_TESTS "Build the ctest suite and nvvsr_bench" ON)
option(NVVSR_BUILD_SMOKE "Build the Phase 0 vsr_smoke test (links the wheel libs directly)" OFF)
set(NVVSR_WHEEL_LIBDIRS "" CACHE STRING
    "For NVVSR_BUILD_SMOKE: semicolon list of the wheel's nvvfx/libs and nvidia_vfx.libs directories")

find_package(Threads REQUIRED)

# ---- libnvvsr: no NVIDIA link dependency, only dlopen -------------------------------
add_library(nvvsr SHARED src/nvvsr.cpp src/loader.cpp)
target_include_directories(nvvsr PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(nvvsr PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
target_compile_options(nvvsr PRIVATE -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
set_target_properties(nvvsr PROPERTIES
  VERSION ${PROJECT_VERSION} SOVERSION 0
  CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_options(nvvsr PRIVATE -Wl,--exclude-libs,ALL -Wl,-z,defs
  "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/nvvsr.map")
set_target_properties(nvvsr PROPERTIES LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/nvvsr.map)

# The public header must compile as C (no NVIDIA includes either); built with the library.
add_library(nvvsr_header_c OBJECT tests/header_check.c)
target_include_directories(nvvsr_header_c PRIVATE include)
target_compile_options(nvvsr_header_c PRIVATE -Wall -Wextra -pedantic -Werror)

install(TARGETS nvvsr LIBRARY DESTINATION lib)
install(FILES include/nvvsr.h DESTINATION include)

add_executable(nvvsr_probe tools/nvvsr_probe.c)
target_link_libraries(nvvsr_probe PRIVATE nvvsr)
install(TARGETS nvvsr_probe RUNTIME DESTINATION bin)

if(NVVSR_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  add_executable(nvvsr_bench tools/nvvsr_bench.cpp)
  target_link_libraries(nvvsr_bench PRIVATE nvvsr)
  target_compile_options(nvvsr_bench PRIVATE -Wall -Wextra)
endif()
