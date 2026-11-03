# PocketSplat — top-level build
cmake_minimum_required(VERSION 3.24)
project(pocketsplat VERSION 0.0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(POCKETSPLAT_BUILD_TOOLS   "Build splatconv CLI"                 ON)
option(POCKETSPLAT_BUILD_TESTS   "Build unit tests"                    ON)
option(POCKETSPLAT_BUILD_RENDER  "Build the Vulkan renderer library"   OFF)
option(POCKETSPLAT_BUILD_DESKTOP "Build the desktop GLFW host"         OFF)
option(POCKETSPLAT_STATIC_CRT    "MSVC: link the C++ runtime statically (release packaging)" OFF)

# A released viewer must run without the Visual C++ redistributable, so the packaging build links
# the runtime statically. Set before the dependencies are fetched: every library must agree.
if(MSVC AND POCKETSPLAT_STATIC_CRT)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Strict warnings apply to our own targets only (they link pocketsplat_warnings); third-party
# FetchContent code builds with its default flags.
add_library(pocketsplat_warnings INTERFACE)
if(MSVC)
    target_compile_options(pocketsplat_warnings INTERFACE /W4 /WX /permissive- /utf-8)
else()
    target_compile_options(pocketsplat_warnings INTERFACE -Wall -Wextra -Wpedantic -Werror)
endif()

include(cmake/deps.cmake)
# Included here (not via CMAKE_SOURCE_DIR in subdirectories) so the function exists when this
# repository is itself an add_subdirectory child, as in the Android Gradle build.
include(cmake/embed_shaders.cmake)

if(POCKETSPLAT_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(core)
if(POCKETSPLAT_BUILD_TOOLS)
    add_subdirectory(tools/splatconv)
endif()
if(POCKETSPLAT_BUILD_RENDER OR POCKETSPLAT_BUILD_DESKTOP)
    add_subdirectory(render)
endif()
if(POCKETSPLAT_BUILD_DESKTOP)
    add_subdirectory(desktop)
endif()
