// Single translation unit hosting the VMA implementation. Third-party code compiled at low
// warning level on purpose; our own flags stay strict everywhere else.
#include <volk.h>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
