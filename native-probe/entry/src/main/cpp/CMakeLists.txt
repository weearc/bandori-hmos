cmake_minimum_required(VERSION 3.5.0)
project(nativeprobe)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(PAGE_GEOMETRY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../native-elf-loader-next)
include_directories(${PAGE_GEOMETRY_DIR})
add_library(probe SHARED probe.cpp ${PAGE_GEOMETRY_DIR}/page_geometry.c)
target_link_libraries(probe PUBLIC libace_napi.z.so libace_ndk.z.so libEGL.so libGLESv3.so libohaudio.so libnative_window.so libhilog_ndk.z.so libdl.so libz.so libffrt.z.so libohinput.so)
add_library(probe_payload SHARED payload.cpp)

# Keep our host DSOs loadable on both 4K and 16K kernels.
foreach(page_target probe probe_payload)
  set_property(TARGET ${page_target} APPEND_STRING PROPERTY LINK_FLAGS " -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384")
endforeach()
