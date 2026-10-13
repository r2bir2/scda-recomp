// VulkanMemoryAllocator implementation unit, as xemu's thirdparty/vma.cc.
// Built with VMA_STATIC_VULKAN_FUNCTIONS=0 / VMA_DYNAMIC_VULKAN_FUNCTIONS=0:
// the renderer hands VMA the volk-loaded function pointers (vk/instance.c).
#define VMA_IMPLEMENTATION
#include <volk.h>
#include "vk_mem_alloc.h"
