# splatrender — Vulkan 1.3 renderer library (vkutil layer + splat pipeline)
add_library(splatrender STATIC
    src/radix_sort.cpp
    src/splat_renderer.cpp
    src/vk_buffer.cpp
    src/vk_context.cpp
    src/vk_descriptors.cpp
    src/vk_frame.cpp
    src/vk_pipeline.cpp
    src/vk_swapchain.cpp
    src/vk_timer.cpp
    src/vma_impl.cpp)
pocketsplat_embed_shaders(splatrender
    shaders/splat_chunk_cull.comp
    shaders/splat_prepare_indirect.comp
    shaders/splat_preprocess.comp
    shaders/splat_pick.comp
    shaders/splat_sorted_copy.comp
    shaders/splat.vert
    shaders/splat.frag
    shaders/radix_upsweep.comp
    shaders/radix_spine.comp
    shaders/radix_downsweep.comp
    shaders/radix_downsweep_mask.comp
    shaders/radix_downsweep_staged.comp)

# Standalone sort correctness test + benchmark (ADR-0004). Needs a GPU; registered with ctest so
# sort changes cannot merge silently on the dev machine.
add_executable(sortbench sortbench/main.cpp)
target_link_libraries(sortbench PRIVATE splatrender pocketsplat_warnings)
pocketsplat_embed_shaders(sortbench sortbench/subgroup_probe.comp)
if(POCKETSPLAT_BUILD_TESTS)
    add_test(NAME sortbench_correctness COMMAND sortbench --validation --iters 3)
    add_test(NAME sortbench_correctness_staged COMMAND sortbench --validation --iters 3 --scatter staged)
endif()
target_include_directories(splatrender PUBLIC include)
target_compile_definitions(splatrender PUBLIC VK_NO_PROTOTYPES)
target_link_libraries(splatrender
    PUBLIC splatcore volk vk-bootstrap::vk-bootstrap GPUOpen::VulkanMemoryAllocator
    PRIVATE pocketsplat_warnings)
