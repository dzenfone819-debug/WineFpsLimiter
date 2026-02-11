#include "global.h"
#include <windows.h>
#include <vulkan/vulkan.h>
#include "imgui_impl_vulkan.h"
#include "imgui_impl_win32.h"
#include "MinHook.h"
#include <vector>

// Note: volk.h must be included BEFORE vulkan.h in real projects, 
// but since we rely on standard headers, we will manually define function pointers 
// if volk isn't available. For now, assume vulkan.h works or we load dynamically.

// --- Globals ---
static VkInstance g_Instance = VK_NULL_HANDLE;
static VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
static VkDevice g_Device = VK_NULL_HANDLE;
static VkQueue g_Queue = VK_NULL_HANDLE;
static uint32_t g_QueueFamily = 0;
static VkCommandPool g_CommandPool = VK_NULL_HANDLE;
static VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;
static VkRenderPass g_OverlayRenderPass = VK_NULL_HANDLE; // ImGui renders here (Clear)

// Swapchain Resources
struct SwapChainData {
    VkSwapchainKHR SwapChain;
    uint32_t Width, Height;
    std::vector<VkImage> Images;
    std::vector<VkImageView> Views;
    std::vector<VkFramebuffer> Framebuffers;
};
static SwapChainData g_SwapChainData = {};

// Overlay Resources (Render Target)
struct OverlayData {
    VkImage Image;
    VkDeviceMemory Memory;
    VkImageView View;
    VkFramebuffer Framebuffer;
    VkSampler Sampler;
    // VkDescriptorSet DescriptorSet; // For the composition shader
};
static OverlayData g_OverlayData = {};

// Synchronization
struct FrameData {
    VkCommandPool CommandPool;
    VkCommandBuffer CommandBuffer;
    VkFence Fence;
    VkSemaphore ImageAcquiredSemaphore; // Wait for game to acquire
    VkSemaphore RenderCompleteSemaphore; // Signal when we are done
};
static std::vector<FrameData> g_Frames;
static uint32_t g_FrameIndex = 0; // Current frame index for OUR resources (not swapchain index)

// Function Pointers
typedef VkResult (VKAPI_PTR *PFN_vkCreateInstance)(const VkInstanceCreateInfo*, const VkAllocationCallbacks*, VkInstance*);
typedef VkResult (VKAPI_PTR *PFN_vkCreateDevice)(VkPhysicalDevice, const VkDeviceCreateInfo*, const VkAllocationCallbacks*, VkDevice*);
typedef VkResult (VKAPI_PTR *PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef VkResult (VKAPI_PTR *PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);

static PFN_vkCreateInstance g_fpCreateInstance = nullptr;
static PFN_vkCreateDevice g_fpCreateDevice = nullptr;
static PFN_vkCreateSwapchainKHR g_fpCreateSwapchainKHR = nullptr;
static PFN_vkQueuePresentKHR g_fpQueuePresentKHR = nullptr;

// --- Helper Functions ---

uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties;
    vkGetPhysicalDeviceMemoryProperties(g_PhysicalDevice, &memProperties);
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return 0; // Failure
}

void CleanupVulkanOverlay() {
    if (g_Device == VK_NULL_HANDLE) return;
    
    vkDeviceWaitIdle(g_Device);
    
    // Shutdown ImGui
    if (g_DescriptorPool != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_Shutdown();
        vkDestroyDescriptorPool(g_Device, g_DescriptorPool, nullptr);
        g_DescriptorPool = VK_NULL_HANDLE;
    }

    // Destroy Frames
    for (auto& frame : g_Frames) {
        vkDestroyFence(g_Device, frame.Fence, nullptr);
        vkDestroySemaphore(g_Device, frame.ImageAcquiredSemaphore, nullptr);
        vkDestroySemaphore(g_Device, frame.RenderCompleteSemaphore, nullptr);
        vkDestroyCommandPool(g_Device, frame.CommandPool, nullptr);
    }
    g_Frames.clear();

    // Destroy Overlay Resources
    if (g_OverlayData.Framebuffer) vkDestroyFramebuffer(g_Device, g_OverlayData.Framebuffer, nullptr);
    if (g_OverlayData.View) vkDestroyImageView(g_Device, g_OverlayData.View, nullptr);
    if (g_OverlayData.Image) vkDestroyImage(g_Device, g_OverlayData.Image, nullptr);
    if (g_OverlayData.Memory) vkFreeMemory(g_Device, g_OverlayData.Memory, nullptr);
    if (g_OverlayData.Sampler) vkDestroySampler(g_Device, g_OverlayData.Sampler, nullptr);
    g_OverlayData = {};

    if (g_OverlayRenderPass) {
        vkDestroyRenderPass(g_Device, g_OverlayRenderPass, nullptr);
        g_OverlayRenderPass = VK_NULL_HANDLE;
    }
    
    // Cleanup Swapchain Views
    for (auto& view : g_SwapChainData.Views) {
        vkDestroyImageView(g_Device, view, nullptr);
    }
    g_SwapChainData.Views.clear();
    g_SwapChainData.Images.clear();
}

void CreateOverlayImage(uint32_t width, uint32_t height) {
    // 1. Create Image (Transfer Src for Blit)
    VkImageCreateInfo imageInfo = {};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    vkCreateImage(g_Device, &imageInfo, nullptr, &g_OverlayData.Image);

    // 2. Allocate Memory
    VkMemoryRequirements memRequirements;
    vkGetImageMemoryRequirements(g_Device, g_OverlayData.Image, &memRequirements);
    VkMemoryAllocateInfo allocInfo = {};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    allocInfo.memoryTypeIndex = FindMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(g_Device, &allocInfo, nullptr, &g_OverlayData.Memory);
    vkBindImageMemory(g_Device, g_OverlayData.Image, g_OverlayData.Memory, 0);

    // 3. Create View
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = g_OverlayData.Image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;
    vkCreateImageView(g_Device, &viewInfo, nullptr, &g_OverlayData.View);

    // 4. Create Sampler (Optional if using Blit, but good for ImGui)
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(g_Device, &samplerInfo, nullptr, &g_OverlayData.Sampler);

    // 5. Create Framebuffer (ImGui target)
    VkFramebufferCreateInfo fbInfo = {};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = g_OverlayRenderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments = &g_OverlayData.View;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;
    vkCreateFramebuffer(g_Device, &fbInfo, nullptr, &g_OverlayData.Framebuffer);
}

// --- Hooks ---

// Forward declare present hook so we can create the hook inside CreateDevice
VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);


VkResult VKAPI_PTR hook_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    // FORCE TRANSFER_DST usage so we can Blit to the swapchain
    VkSwapchainCreateInfoKHR modifiedInfo = *pCreateInfo;
    modifiedInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = g_fpCreateSwapchainKHR(device, &modifiedInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS) {
        g_Device = device;
        g_SwapChainData.SwapChain = *pSwapchain;
        g_SwapChainData.Width = pCreateInfo->imageExtent.width;
        g_SwapChainData.Height = pCreateInfo->imageExtent.height;
        
        // Cleanup old resources (Resize handling)
        CleanupVulkanOverlay();

        // --- Init Vulkan Overlay ---
        // 1. Get Images
        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device, *pSwapchain, &count, nullptr);
        g_SwapChainData.Images.resize(count);
        vkGetSwapchainImagesKHR(device, *pSwapchain, &count, g_SwapChainData.Images.data());
        
        // 2. Create Descriptor Pool (for ImGui)
        VkDescriptorPoolSize pool_sizes[] =
        {
            { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
            { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
        };
        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
        pool_info.poolSizeCount = (uint32_t)IM_ARRAYSIZE(pool_sizes);
        pool_info.pPoolSizes = pool_sizes;
        vkCreateDescriptorPool(device, &pool_info, nullptr, &g_DescriptorPool);

        // 3. Create ImGui Overlay Resources
        if (g_OverlayRenderPass == VK_NULL_HANDLE) {
            VkAttachmentDescription attachment = {};
            attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
            attachment.samples = VK_SAMPLE_COUNT_1_BIT;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; // We will transition to TRANSFER_SRC later
            
            VkAttachmentReference color_attachment = {};
            color_attachment.attachment = 0;
            color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            
            VkSubpassDescription subpass = {};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &color_attachment;
            
            VkRenderPassCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            info.attachmentCount = 1;
            info.pAttachments = &attachment;
            info.subpassCount = 1;
            info.pSubpasses = &subpass;
            vkCreateRenderPass(device, &info, nullptr, &g_OverlayRenderPass);
        }
        
        CreateOverlayImage(g_SwapChainData.Width, g_SwapChainData.Height);
        
        // 4. Create Sync Objects (Frames in Flight)
        g_Frames.resize(count); // Usually same as swapchain count
        for(size_t i=0; i<count; i++) {
            VkFenceCreateInfo fenceInfo = {};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            vkCreateFence(device, &fenceInfo, nullptr, &g_Frames[i].Fence);
            
            VkSemaphoreCreateInfo semInfo = {};
            semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            vkCreateSemaphore(device, &semInfo, nullptr, &g_Frames[i].ImageAcquiredSemaphore);
            vkCreateSemaphore(device, &semInfo, nullptr, &g_Frames[i].RenderCompleteSemaphore);
            
            VkCommandPoolCreateInfo poolInfo = {};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.queueFamilyIndex = g_QueueFamily; 
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            vkCreateCommandPool(device, &poolInfo, nullptr, &g_Frames[i].CommandPool);
            
            VkCommandBufferAllocateInfo allocInfo = {};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = g_Frames[i].CommandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
            vkAllocateCommandBuffers(device, &allocInfo, &g_Frames[i].CommandBuffer);
        }

        // Init ImGui Vulkan
        ImGui_ImplVulkan_InitInfo init_info = {};
        init_info.ApiVersion = VK_API_VERSION_1_0;
        init_info.Instance = g_Instance;
        init_info.PhysicalDevice = g_PhysicalDevice;
        init_info.Device = device;
        init_info.QueueFamily = g_QueueFamily;
        init_info.Queue = g_Queue;
        init_info.PipelineCache = VK_NULL_HANDLE;
        init_info.DescriptorPool = g_DescriptorPool;
        init_info.DescriptorPoolSize = 0;
        init_info.MinImageCount = count;
        init_info.ImageCount = count;
        init_info.Allocator = nullptr;
        init_info.CheckVkResultFn = nullptr;
        // Configure main pipeline info (render pass, subpass, msaa)
        init_info.PipelineInfoMain.RenderPass = g_OverlayRenderPass;
        init_info.PipelineInfoMain.Subpass = 0;
        init_info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        ImGui_ImplVulkan_Init(&init_info);
        
        // ImGui now handles font texture uploads internally during the first NewFrame().
        // No manual font upload commands are required here.
        
        // Setup Window
        g_hWnd = FindMainWindow(); // We need to find the window
        ImGui::CreateContext();
        ImGui_ImplWin32_Init(g_hWnd);
    }
    return res;
}

VkResult VKAPI_PTR hook_vkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDevice* pDevice) {
    VkResult res = g_fpCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res == VK_SUCCESS) {
        g_PhysicalDevice = physicalDevice;
        g_Device = *pDevice;
        
        // Find Graphics Queue (assume index 0 of first queue family requested for now, or scan)
        // Usually pCreateInfo->pQueueCreateInfos[0] is Graphics
        if (pCreateInfo->queueCreateInfoCount > 0) {
            g_QueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
            vkGetDeviceQueue(*pDevice, g_QueueFamily, 0, &g_Queue);
        }
        
        // Hook Swapchain
        void* addr = (void*)vkGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hook_vkCreateSwapchainKHR, (LPVOID*)&g_fpCreateSwapchainKHR);
            MH_EnableHook(addr);
        }
        
        // Hook Present
        addr = (void*)vkGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hook_vkQueuePresentKHR, (LPVOID*)&g_fpQueuePresentKHR);
            MH_EnableHook(addr);
        }
    }
    return res;
}

VkResult VKAPI_PTR hook_vkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkInstance* pInstance) {
    VkResult res = g_fpCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res == VK_SUCCESS) {
        g_Instance = *pInstance;
        // Hook CreateDevice
        void* addr = (void*)vkGetInstanceProcAddr(*pInstance, "vkCreateDevice");
        if (addr) {
            MH_CreateHook(addr, (LPVOID)hook_vkCreateDevice, (LPVOID*)&g_fpCreateDevice);
            MH_EnableHook(addr);
        }
    }
    return res;
}

VkResult VKAPI_PTR hook_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (g_Device == VK_NULL_HANDLE || g_Frames.empty()) return g_fpQueuePresentKHR(queue, pPresentInfo);
    
    // --- Render Overlay ---
    // 1. Wait for OUR Fence (ensure we are not overwriting our own command buffer)
    FrameData& frame = g_Frames[g_FrameIndex];
    vkWaitForFences(g_Device, 1, &frame.Fence, VK_TRUE, UINT64_MAX);
    vkResetFences(g_Device, 1, &frame.Fence);
    
    // 2. Begin Command Buffer
    vkResetCommandPool(g_Device, frame.CommandPool, 0);
    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(frame.CommandBuffer, &beginInfo);
    
    // 3. Render ImGui to Overlay Image
    {
        VkRenderPassBeginInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        info.renderPass = g_OverlayRenderPass;
        info.framebuffer = g_OverlayData.Framebuffer;
        info.renderArea.extent.width = g_SwapChainData.Width;
        info.renderArea.extent.height = g_SwapChainData.Height;
        VkClearValue clearColor = { 0.0f, 0.0f, 0.0f, 0.0f };
        info.clearValueCount = 1;
        info.pClearValues = &clearColor;
        vkCmdBeginRenderPass(frame.CommandBuffer, &info, VK_SUBPASS_CONTENTS_INLINE);
        
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RenderOverlay();
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.CommandBuffer);
        
        vkCmdEndRenderPass(frame.CommandBuffer);
    }
    
    // 4. Blit Overlay to SwapChain Image (Composition)
    uint32_t imageIndex = pPresentInfo->pImageIndices[0];
    if (imageIndex < g_SwapChainData.Images.size()) {
        VkImage swapchainImage = g_SwapChainData.Images[imageIndex];
        
        // Barrier: Overlay (COLOR_ATTACHMENT -> TRANSFER_SRC)
        VkImageMemoryBarrier barrierOverlay = {};
        barrierOverlay.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrierOverlay.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrierOverlay.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrierOverlay.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierOverlay.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierOverlay.image = g_OverlayData.Image;
        barrierOverlay.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrierOverlay.subresourceRange.levelCount = 1;
        barrierOverlay.subresourceRange.layerCount = 1;
        barrierOverlay.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrierOverlay.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        
        // Barrier: Swapchain (PRESENT_SRC/UNDEFINED -> TRANSFER_DST)
        VkImageMemoryBarrier barrierSwap = {};
        barrierSwap.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrierSwap.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; // Likely state
        barrierSwap.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrierSwap.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierSwap.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrierSwap.image = swapchainImage;
        barrierSwap.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrierSwap.subresourceRange.levelCount = 1;
        barrierSwap.subresourceRange.layerCount = 1;
        barrierSwap.srcAccessMask = 0; 
        barrierSwap.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        
        vkCmdPipelineBarrier(frame.CommandBuffer, 
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            0, 0, nullptr, 0, nullptr, 1, &barrierOverlay);
            
        vkCmdPipelineBarrier(frame.CommandBuffer, 
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 
            0, 0, nullptr, 0, nullptr, 1, &barrierSwap);
        
        // Blit
        VkImageBlit blit = {};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcOffsets[1].x = g_SwapChainData.Width;
        blit.srcOffsets[1].y = g_SwapChainData.Height;
        blit.srcOffsets[1].z = 1;
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.layerCount = 1;
        blit.dstOffsets[1].x = g_SwapChainData.Width;
        blit.dstOffsets[1].y = g_SwapChainData.Height;
        blit.dstOffsets[1].z = 1;
        
        vkCmdBlitImage(frame.CommandBuffer, 
            g_OverlayData.Image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            swapchainImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit, VK_FILTER_LINEAR);
            
        // Barrier: Swapchain (TRANSFER_DST -> PRESENT_SRC)
        barrierSwap.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrierSwap.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrierSwap.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrierSwap.dstAccessMask = 0; // Memory visible to presentation engine
        
        // Barrier: Overlay (TRANSFER_SRC -> COLOR_ATTACHMENT for next frame)
        barrierOverlay.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrierOverlay.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrierOverlay.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrierOverlay.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        
        vkCmdPipelineBarrier(frame.CommandBuffer, 
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 
            0, 0, nullptr, 0, nullptr, 1, &barrierSwap);
            
        vkCmdPipelineBarrier(frame.CommandBuffer, 
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 
            0, 0, nullptr, 0, nullptr, 1, &barrierOverlay);
    }
    
    vkEndCommandBuffer(frame.CommandBuffer);
    
    // 5. Submit
    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores; // Wait for game render
    VkPipelineStageFlags waitStages[] = { VK_PIPELINE_STAGE_TRANSFER_BIT }; // Wait before blit
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &frame.CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &frame.RenderCompleteSemaphore; // Signal overlay render
    
    vkQueueSubmit(queue, 1, &submitInfo, frame.Fence);
    
    // 6. Call Original Present
    VkPresentInfoKHR newPresentInfo = *pPresentInfo;
    newPresentInfo.waitSemaphoreCount = 1;
    newPresentInfo.pWaitSemaphores = &frame.RenderCompleteSemaphore;
    
    g_FrameIndex = (g_FrameIndex + 1) % g_Frames.size();
    
    return g_fpQueuePresentKHR(queue, &newPresentInfo);
}

void InitVulkanHook() {
    HMODULE hMod = GetModuleHandleA("vulkan-1.dll");
    if (hMod) {
        void* proc = (void*)GetProcAddress(hMod, "vkCreateInstance");
        if (proc) {
            MH_CreateHook(proc, (LPVOID)hook_vkCreateInstance, (LPVOID*)&g_fpCreateInstance);
            MH_EnableHook(proc);
            Log("Vulkan Hook Installed.");
        }
    }
}
