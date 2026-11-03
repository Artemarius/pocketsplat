# deps.cmake — FetchContent declarations, pinned to tags. Licenses in THIRD_PARTY.md.
# Render/desktop deps (volk, VMA, vk-bootstrap, imgui, glfw) are added in Phase 2.

include(FetchContent)

# zlib 1.3.1 (fetched transitively by spz below) declares cmake_minimum_required < 3.5, which
# CMake 4.x refuses to configure without this override.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# --- glm 1.0.3 (MIT) -------------------------------------------------------------------------
FetchContent_Declare(glm
    GIT_REPOSITORY https://github.com/g-truc/glm.git
    GIT_TAG 1.0.3)

# --- spz v3.0.0 (MIT) ------------------------------------------------------------------------
# spz fetches its own pinned zlib 1.3.1 (zlib license) and zstd 1.5.6 (BSD-3) via URL + SHA256
# when find_package does not locate them. We deliberately do not pre-build either one ourselves.
# But: spz points zlib at zlib.net/zlib-1.3.1.tar.gz, and zlib.net removes superseded tarballs
# (404 as of 2026-09-01). Re-declare it under the same name and hash from the permanent fossils/
# mirror — FetchContent's first declaration wins, so spz's inner declare becomes a no-op.
FetchContent_Declare(zlib
    URL https://zlib.net/fossils/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23)
set(SPZ_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(SPZ_BUILD_PYTHON_BINDINGS OFF CACHE BOOL "" FORCE)
set(SPZ_BUILD_EXTENSIONS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG v3.0.0)

FetchContent_MakeAvailable(glm spz)
# Treat spz headers as system includes so its warnings don't trip our /WX (e.g. C4244 in
# splat-types.h).
set_target_properties(spz PROPERTIES SYSTEM TRUE)
if(ANDROID)
    # spz's load-spz.h calls __android_log_print under #ifdef ANDROID but never includes
    # <android/log.h> (upstream expects the consumer to have it in scope). Inject the include
    # PUBLIC-ly - every TU that includes spz headers needs it - and link the log library.
    target_compile_options(spz PUBLIC -include android/log.h)
    target_link_libraries(spz PUBLIC log)
endif()

# When spz falls back to its FetchContent zlib it links the SHARED 'zlib' target, which would make
# every consumer executable depend on a zlib.dll that never lands next to it. Rewrite the interface
# to the 'zlibstatic' target instead (same sources/symbols; static is also what Android needs).
if(TARGET zlib AND TARGET zlibstatic)
    get_target_property(_spz_deps spz INTERFACE_LINK_LIBRARIES)
    if(_spz_deps)
        set(_spz_fixed "")
        foreach(_dep IN LISTS _spz_deps)
            if(_dep STREQUAL "zlib")
                list(APPEND _spz_fixed zlibstatic)
            elseif(_dep STREQUAL "$<LINK_ONLY:zlib>")
                list(APPEND _spz_fixed "$<LINK_ONLY:zlibstatic>")
            else()
                list(APPEND _spz_fixed "${_dep}")
            endif()
        endforeach()
        set_target_properties(spz PROPERTIES INTERFACE_LINK_LIBRARIES "${_spz_fixed}")
    endif()
endif()

# --- render/desktop dependencies (Phase 2) ---------------------------------------------------
if(POCKETSPLAT_BUILD_RENDER OR POCKETSPLAT_BUILD_DESKTOP)
    if(ANDROID)
        # volk must compile its Android surface entry point (vkCreateAndroidSurfaceKHR) into the
        # loaded function table; the define is PUBLIC so consumers see the matching declarations.
        set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_ANDROID_KHR CACHE STRING "" FORCE)
        # vk-bootstrap pins POSITION_INDEPENDENT_CODE to its own option (default follows
        # BUILD_SHARED_LIBS), ignoring CMAKE_POSITION_INDEPENDENT_CODE - everything must be PIC
        # to link into the shared JNI library.
        set(VK_BOOTSTRAP_POSITION_INDEPENDENT_CODE ON CACHE BOOL "" FORCE)
    endif()
    FetchContent_Declare(vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG vulkan-sdk-1.4.357.0)
    FetchContent_Declare(volk
        GIT_REPOSITORY https://github.com/zeux/volk.git
        GIT_TAG vulkan-sdk-1.4.357.0)
    FetchContent_Declare(vma
        GIT_REPOSITORY https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git
        GIT_TAG v3.4.0)
    FetchContent_Declare(vk_bootstrap
        GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap.git
        GIT_TAG v1.4.360)
    FetchContent_MakeAvailable(vulkan_headers volk vma vk_bootstrap)
    foreach(_t volk vk-bootstrap VulkanMemoryAllocator)
        if(TARGET ${_t})
            set_target_properties(${_t} PROPERTIES SYSTEM TRUE)
        endif()
    endforeach()
endif()

if(POCKETSPLAT_BUILD_DESKTOP)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG 3.5.1)
    # imgui has no CMakeLists; MakeAvailable only populates it and desktop/ builds the sources.
    FetchContent_Declare(imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9b)
    FetchContent_MakeAvailable(glfw imgui)
    set_target_properties(glfw PROPERTIES SYSTEM TRUE)
endif()

# --- googletest v1.18.0 (BSD-3), tests only --------------------------------------------------
if(POCKETSPLAT_BUILD_TESTS)
    if(NOT POCKETSPLAT_STATIC_CRT)
        set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # match the parent project's MSVC runtime
    endif()
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.18.0)
    FetchContent_MakeAvailable(googletest)
endif()
