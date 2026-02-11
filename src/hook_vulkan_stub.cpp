// Minimal stub for platforms/arches without Vulkan support (x86 builds)

#include "global.h"

// Provide an empty InitVulkanHook so dllmain can call it unconditionally.
void InitVulkanHook() {
    // No-op on x86 builds (Vulkan backend not available)
}

// If other translation units expect DestroyVulkanHook, provide a no-op as well.
void DestroyVulkanHook() {
}
