cmake_minimum_required(VERSION 3.20)
project(nvc VERSION 0.1.0 DESCRIPTION "NV Control panel backend" LANGUAGES CXX)
find_package(PkgConfig REQUIRED)
pkg_check_modules(JSONGLIB REQUIRED json-glib-1.0)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(NVC_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

add_compile_options(-Wall -Wextra)
if(NVC_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

option(NVC_BUILD_APP "Build the GTK4/libadwaita app (nvcontrol)" ON)

# Every src/*.cpp goes into the GTK-free library, except the app's own: main.cpp, app.cpp, page_*.cpp, ui_*.cpp.
file(GLOB NVC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
file(GLOB NVC_APP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/app.cpp
     ${CMAKE_CURRENT_SOURCE_DIR}/src/page_*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/src/ui_*.cpp)
list(REMOVE_ITEM NVC_SOURCES ${NVC_APP_SOURCES})
add_library(nvc STATIC ${NVC_SOURCES})
target_include_directories(nvc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
# JSON (install.json, scheme.json) comes from json-glib, installed with the GTK stack; headers keep to std types.
target_include_directories(nvc PRIVATE ${JSONGLIB_INCLUDE_DIRS})
target_link_libraries(nvc PUBLIC ${JSONGLIB_LIBRARIES})

enable_testing()
file(GLOB NVC_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_*.cpp)
foreach(src ${NVC_TEST_SOURCES})
  get_filename_component(name ${src} NAME_WE)
  add_executable(${name} ${src})
  target_link_libraries(${name} PRIVATE nvc)
  add_test(NAME ${name} COMMAND ${name})
endforeach()

if(NVC_BUILD_APP)
  pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)
  pkg_check_modules(ADW REQUIRED IMPORTED_TARGET libadwaita-1)
  add_executable(nvcontrol ${NVC_APP_SOURCES})
  target_include_directories(nvcontrol PRIVATE ${JSONGLIB_INCLUDE_DIRS})  # the smoke result is written with json-glib
  target_link_libraries(nvcontrol PRIVATE nvc PkgConfig::GTK4 PkgConfig::ADW)
  # the development checkout of the panel: icons/ and the repo lookup start here
  target_compile_definitions(nvcontrol PRIVATE NVC_APP_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
endif()
