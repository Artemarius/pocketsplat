# pocketsplat-android — JNI bridge + Android Vulkan host over splatcore/splatrender.
# Driven by Gradle (externalNativeBuild); reuses the repository root build with the desktop-only
# pieces switched off. Needs the SDK's CMake 4.1.2 (the root requires >= 3.24).
cmake_minimum_required(VERSION 3.24)
project(pocketsplat-android LANGUAGES CXX)

# Every static library here (splatcore, splatrender, spz, vk-bootstrap, zlib, ...) links into
# the one shared JNI library, so all of it must be position-independent.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(POCKETSPLAT_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(POCKETSPLAT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(POCKETSPLAT_BUILD_RENDER ON CACHE BOOL "" FORCE)
set(POCKETSPLAT_BUILD_DESKTOP OFF CACHE BOOL "" FORCE)
get_filename_component(POCKETSPLAT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. ABSOLUTE)
add_subdirectory(${POCKETSPLAT_ROOT} pocketsplat)

add_library(pocketsplat SHARED
    android_host.cpp
    jni_bridge.cpp)
target_link_libraries(pocketsplat PRIVATE
    splatrender splatcore glm::glm
    android nativewindow log  # nativewindow: ANativeWindow_setFrameRate
    pocketsplat_warnings)
