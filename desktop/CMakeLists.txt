# pocketsplat-desktop — GLFW host window + ImGui debug HUD (development loop on the RTX 3060)

# Dear ImGui has no build system; compile the core + GLFW/Vulkan backends here. Third-party code,
# so it stays off our warning flags and gets the volk bindings.
add_library(imgui_backend STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_backend SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_compile_definitions(imgui_backend PUBLIC IMGUI_IMPL_VULKAN_USE_VOLK VK_NO_PROTOTYPES)
target_link_libraries(imgui_backend PUBLIC glfw volk)

add_executable(pocketsplat-desktop src/main.cpp)
target_link_libraries(pocketsplat-desktop PRIVATE splatrender imgui_backend glfw glm::glm
    pocketsplat_warnings)
pocketsplat_embed_shaders(pocketsplat-desktop shaders/triangle.vert shaders/triangle.frag)

# pocketsplat-viewer - the end-user viewer (no console window on Windows): same renderer, import and
# navigation as the Android app. Released as a zip (scripts/package_windows.py).
add_executable(pocketsplat-viewer WIN32 src/viewer_main.cpp src/platform.cpp)
target_link_libraries(pocketsplat-viewer PRIVATE splatrender imgui_backend glfw glm::glm pocketsplat_warnings)
if(WIN32)
    target_link_libraries(pocketsplat-viewer PRIVATE comdlg32 shell32 ole32)
    if(MSVC)
        target_link_options(pocketsplat-viewer PRIVATE /ENTRY:mainCRTStartup)  # main(), no console
    endif()
    # App icon (exe, window, taskbar); desktop/resources/pocketsplat.rc says how GLFW picks it up.
    target_sources(pocketsplat-viewer PRIVATE resources/pocketsplat.rc)
    target_sources(pocketsplat-desktop PRIVATE resources/pocketsplat.rc)
endif()

# Golden-image regression gate (CLAUDE.md §7): render with pinned camera/flags, compare against
# desktop/tests/golden/*.png at PSNR >= 40 dB. Skips (exit 77) on machines without the local
# test scenes. Re-record via scripts/golden_check.py mode 'record' after a reviewed change.
if(POCKETSPLAT_BUILD_TESTS)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        add_test(NAME golden_museum1
            COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/golden_check.py check
                --exe $<TARGET_FILE:pocketsplat-desktop>
                --scene data/derived/museum1.pspk
                --ref desktop/tests/golden/museum1.png
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        add_test(NAME golden_synthetic_sphere
            COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/golden_check.py check
                --exe $<TARGET_FILE:pocketsplat-desktop>
                --scene data/raw/synthetic_sphere.sh1.ply
                --ref desktop/tests/golden/synthetic_sphere_sh1.png
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        set_tests_properties(golden_museum1 golden_synthetic_sphere PROPERTIES SKIP_RETURN_CODE 77)
        # Chiral orientation gate: fully synthetic (never skips), asserts the projected
        # major-axis slope has the correct handedness. Image-vs-self goldens cannot catch a
        # y-mirrored covariance — this can (the S12 EWA Jacobian sign bug).
        if(TARGET splatconv)
            add_test(NAME golden_chiral_orientation
                COMMAND Python3::Interpreter ${CMAKE_SOURCE_DIR}/scripts/chiral_check.py
                    --exe $<TARGET_FILE:pocketsplat-desktop>
                    --splatconv $<TARGET_FILE:splatconv>
                    --workdir ${CMAKE_BINARY_DIR}/chiral
                WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
        endif()
    endif()
endif()
