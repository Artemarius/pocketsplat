# GLSL -> SPIR-V at build time via glslangValidator (Vulkan SDK), embedded as C headers with
# --vn (uint32_t arrays). No runtime shader compilation (CLAUDE.md §3).
#
# pocketsplat_embed_shaders(<target> <shader> [<shader>...])
#   For desktop/shaders/triangle.vert this generates <build>/shaders/triangle_vert.spv.h defining
#   `const uint32_t k_triangle_vert_spv[]`, added to the target with the directory on its include
#   path.
function(pocketsplat_embed_shaders TARGET)
    file(GLOB _sdk_bins "C:/VulkanSDK/*/Bin" "D:/VulkanSDK/*/Bin" "E:/VulkanSDK/*/Bin")
    find_program(POCKETSPLAT_GLSLANG glslangValidator
        HINTS ENV VULKAN_SDK
        PATH_SUFFIXES Bin bin
        PATHS ${_sdk_bins}
        REQUIRED)
    set(_out_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
    file(MAKE_DIRECTORY ${_out_dir})
    foreach(_shader IN LISTS ARGN)
        get_filename_component(_name ${_shader} NAME)
        string(REPLACE "." "_" _ident ${_name})
        set(_header ${_out_dir}/${_ident}.spv.h)
        add_custom_command(
            OUTPUT ${_header}
            COMMAND ${POCKETSPLAT_GLSLANG} -V --target-env vulkan1.3 --quiet
                    --vn k_${_ident}_spv -o ${_header} ${CMAKE_CURRENT_SOURCE_DIR}/${_shader}
            DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${_shader}
            COMMENT "GLSL -> SPIR-V: ${_name}")
        target_sources(${TARGET} PRIVATE ${_header})
    endforeach()
    target_include_directories(${TARGET} PRIVATE ${_out_dir})
endfunction()
