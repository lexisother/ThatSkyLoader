#include <cstdio>
#include <vector>
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <vk_layer_dispatch_table.h>
#include "vulkan/vk_platform.h"
#include "vulkan/vulkan_core.h"

#include "include/layer.h"
#include "include/menu.hpp"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <imgui_impl_win32.h>

#include <assert.h>
#include <string.h>
#include <iostream>
#include <mutex>
#include <map>
#include <algorithm>
#include <memory>

/**
 * @brief Export macro for Vulkan layer functions
 */
#define VK_LAYER_EXPORT extern "C" __declspec(dllexport)

/**
 * @brief Helper macro to iterate through Vulkan struct chains
 */
#define vk_foreach_struct(__iter, __start) \
  for (struct VkBaseOutStructure *__iter = (struct VkBaseOutStructure *)(__start); \
    __iter; __iter = __iter->pNext)

// Global mutex for thread safety
std::mutex global_lock;
using scoped_lock = std::lock_guard<std::mutex>;

/**
 * @brief Extract the dispatch table key from a Vulkan dispatchable object
 * @param inst The Vulkan dispatchable object
 * @return Pointer to use as key in dispatch tables
 */
template<typename DispatchableType>
void* GetKey(DispatchableType inst) {
    return *(void**)inst;
}

// Forward declarations
struct QueueData;

/**
 * @brief Instance data structure to store Vulkan instance information
 */
struct InstanceData {
    VkLayerInstanceDispatchTable vtable;
    VkInstance instance;
};

/**
 * @brief Device data structure to store Vulkan device information
 */
struct DeviceData {
    InstanceData* instance = nullptr;
    PFN_vkSetDeviceLoaderData set_device_loader_data = nullptr;
    VkLayerDispatchTable vtable = {};
    VkDevice device = VK_NULL_HANDLE;
    QueueData* graphic_queue = nullptr;
    std::vector<QueueData*> queues;
};

/**
 * @brief Queue data structure to store Vulkan queue information
 */
struct QueueData {
    DeviceData* device;
    VkQueue queue;
};

// Global dispatch tables
std::map<void*, DeviceData> g_device_dispatch;
std::map<void*, QueueData> g_queue_data;
std::map<void*, VkLayerInstanceDispatchTable> instance_dispatch;
std::map<void*, VkLayerDispatchTable> device_dispatch;

/**
 * @brief Get device data for a given Vulkan device
 * @param key The device key
 * @return Pointer to device data
 */
DeviceData* GetDeviceData(void* key) {
    scoped_lock l(global_lock);
    return &g_device_dispatch[GetKey(key)];
}

/**
 * @brief Get queue data for a given Vulkan queue
 * @param key The queue key
 * @return Pointer to queue data
 */
QueueData* GetQueueData(void* key) {
    scoped_lock l(global_lock);
    return &g_queue_data[key];
}

/**
 * @brief Create a new queue data object
 * @param queue The Vulkan queue
 * @param device_data The device data
 * @return Pointer to the new queue data
 */
static QueueData* new_queue_data(VkQueue queue, DeviceData* device_data) {
    QueueData* data = GetQueueData(queue);
    data->device = device_data;
    data->queue = queue;
    device_data->graphic_queue = data;
    return data;
}

/**
 * @brief Map queues to device data
 * @param data The device data
 * @param pCreateInfo The device create info
 */
static void DeviceMapQueues(DeviceData* data, const VkDeviceCreateInfo* pCreateInfo) {
    for (uint32_t i = 0; i < pCreateInfo->queueCreateInfoCount; i++) {
        for (uint32_t j = 0; j < pCreateInfo->pQueueCreateInfos[i].queueCount; j++) {
            VkQueue queue;
            data->vtable.GetDeviceQueue(data->device,
                pCreateInfo->pQueueCreateInfos[i].queueFamilyIndex,
                j, &queue);

            VkResult result = data->set_device_loader_data(data->device, queue);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to set device loader data: " << result << std::endl;
            }
            
            data->queues.push_back(new_queue_data(queue, data));
        }
    }
}

/**
 * @brief Get device chain info from create info
 * @param pCreateInfo The device create info
 * @param func The layer function
 * @return Pointer to the device chain info or nullptr if not found
 */
static VkLayerDeviceCreateInfo* get_device_chain_info(const VkDeviceCreateInfo* pCreateInfo, VkLayerFunction func) {
    vk_foreach_struct(item, pCreateInfo->pNext) {
        if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            ((VkLayerDeviceCreateInfo*)item)->function == func) {
            return (VkLayerDeviceCreateInfo*)item;
        }
    }
    return nullptr;
}

// Vulkan resource handles
static VkAllocationCallbacks* g_Allocator = nullptr;
static VkInstance g_Instance = VK_NULL_HANDLE;

// Made external for mod loader access
VkPhysicalDevice g_PhysicalDevice = VK_NULL_HANDLE;
VkDevice g_FakeDevice = VK_NULL_HANDLE;
VkDevice g_Device = VK_NULL_HANDLE;
VkQueue g_GraphicsQueue = VK_NULL_HANDLE;
VkCommandBuffer g_CommandBuffer = VK_NULL_HANDLE;
VkDescriptorPool g_DescriptorPool = VK_NULL_HANDLE;

// Queue family information
static uint32_t g_QueueFamily = static_cast<uint32_t>(-1);
static std::vector<VkQueueFamilyProperties> g_QueueFamilies;

// Rendering resources
static VkPipelineCache g_PipelineCache = VK_NULL_HANDLE;
static uint32_t g_MinImageCount = 1;
static VkRenderPass g_RenderPass = VK_NULL_HANDLE;
static ImGui_ImplVulkanH_Frame g_Frames[8] = {};
static ImGui_ImplVulkanH_FrameSemaphores g_FrameSemaphores[8] = {};

// Window information
static HWND g_Hwnd = nullptr;
static VkExtent2D g_ImageExtent = {};

static void CleanupDeviceVulkan( );
static void CleanupRenderTarget( );
static VkResult RenderImGui_Vulkan(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static bool DoesQueueSupportGraphic(VkQueue queue, VkQueue* pGraphicQueue);

/**
 * @brief Create a Vulkan device for the mod loader
 * @return true if successful, false otherwise
 */
static bool CreateDeviceVK() {
    VkResult result = VK_SUCCESS;
    
    // Create Vulkan Instance
    {
        VkInstanceCreateInfo create_info = {};
        constexpr const char* instance_extension = "VK_KHR_surface";

        create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = &instance_extension;

        // Create Vulkan Instance without any debug feature
        result = vkCreateInstance(&create_info, g_Allocator, &g_Instance);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to create Vulkan instance: " << result << std::endl;
            return false;
        }
        std::cout << "[+] Vulkan: g_Instance: 0x" << g_Instance << std::endl;
    }

    // Select GPU
    {
        uint32_t gpu_count = 0;
        result = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, nullptr);
        if (result != VK_SUCCESS || gpu_count == 0) {
            std::cerr << "[ERROR] Failed to enumerate physical devices or no devices found" << std::endl;
            return false;
        }

        std::vector<VkPhysicalDevice> gpus(gpu_count);
        result = vkEnumeratePhysicalDevices(g_Instance, &gpu_count, gpus.data());
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to get physical devices: " << result << std::endl;
            return false;
        }

        // Log all available GPUs
        std::cout << "--------- Available GPUs ---------" << std::endl;
        for (int i = 0; i < static_cast<int>(gpu_count); ++i) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(gpus[i], &properties);
            
            // Get device type as string
            std::string deviceType;
            switch (properties.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: deviceType = "Integrated GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: deviceType = "Discrete GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: deviceType = "Virtual GPU"; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: deviceType = "CPU"; break;
                default: deviceType = "Unknown"; break;
            }
            
            std::cout << "GPU " << i << ": " << properties.deviceName << ", Type: " << deviceType
                      << ", Vendor ID: 0x" << std::hex << properties.vendorID << std::dec
                      << ", Device ID: 0x" << std::hex << properties.deviceID << std::dec << std::endl;
        }
        std::cout << "----------------------------------" << std::endl;

        // GPU selection with ranking system
        // Priority: 1. Discrete GPU, 2. Integrated GPU, 3. Virtual GPU, 4. CPU, 5. Other
        struct GPURanking {
            int index;
            int rank; // Lower is better
        };
        
        std::vector<GPURanking> rankedGPUs;
        
        for (int i = 0; i < static_cast<int>(gpu_count); ++i) {
            VkPhysicalDeviceProperties properties;
            vkGetPhysicalDeviceProperties(gpus[i], &properties);
            
            GPURanking ranking{i, 999}; // Default rank
            
            switch (properties.deviceType) {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: ranking.rank = 1; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: ranking.rank = 2; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: ranking.rank = 3; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: ranking.rank = 4; break;
                default: ranking.rank = 5; break;
            }
            
            rankedGPUs.push_back(ranking);
        }
        
        // Sort GPUs by rank (lower rank = better)
        std::sort(rankedGPUs.begin(), rankedGPUs.end(), 
            [](const GPURanking& a, const GPURanking& b) { return a.rank < b.rank; });
        
        // Select best GPU (first in sorted list)
        int use_gpu = rankedGPUs[0].index;
        
        // Get properties of selected GPU for logging
        VkPhysicalDeviceProperties selectedProperties;
        vkGetPhysicalDeviceProperties(gpus[use_gpu], &selectedProperties);
        std::string selectedType;
        switch (selectedProperties.deviceType) {
            case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: selectedType = "Integrated GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: selectedType = "Discrete GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: selectedType = "Virtual GPU"; break;
            case VK_PHYSICAL_DEVICE_TYPE_CPU: selectedType = "CPU"; break;
            default: selectedType = "Unknown"; break;
        }
        
        g_PhysicalDevice = gpus[use_gpu];
        std::cout << "[+] Selected GPU: " << selectedProperties.deviceName 
                  << " (" << selectedType << ")" << std::endl;
        std::cout << "[+] Vulkan: g_PhysicalDevice: 0x" << g_PhysicalDevice << std::endl;
    }

    // Select graphics queue family
    {
        uint32_t count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, nullptr);
        if (count == 0) {
            std::cerr << "[ERROR] No queue families found" << std::endl;
            return false;
        }
        
        g_QueueFamilies.resize(count);
        vkGetPhysicalDeviceQueueFamilyProperties(g_PhysicalDevice, &count, g_QueueFamilies.data());
        
        // Find a queue family that supports graphics operations
        g_QueueFamily = static_cast<uint32_t>(-1); // Invalid value
        for (uint32_t i = 0; i < count; ++i) {
            if (g_QueueFamilies[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                g_QueueFamily = i;
                break;
            }
        }
        
        if (g_QueueFamily == static_cast<uint32_t>(-1)) {
            std::cerr << "[ERROR] No graphics queue family found" << std::endl;
            return false;
        }

        std::cout << "[+] Vulkan: g_QueueFamily: " << g_QueueFamily << std::endl;
    }

    // Create Logical Device (with 1 queue)
    {
        constexpr const char* device_extension = "VK_KHR_swapchain";
        constexpr const float queue_priority = 1.0f;

        VkDeviceQueueCreateInfo queue_info = {};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = g_QueueFamily;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;

        VkDeviceCreateInfo create_info = {};
        create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        create_info.queueCreateInfoCount = 1;
        create_info.pQueueCreateInfos = &queue_info;
        create_info.enabledExtensionCount = 1;
        create_info.ppEnabledExtensionNames = &device_extension;

        result = vkCreateDevice(g_PhysicalDevice, &create_info, g_Allocator, &g_FakeDevice);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to create logical device: " << result << std::endl;
            return false;
        }

        std::cout << "[+] Vulkan: g_FakeDevice: 0x" << g_FakeDevice << std::endl;
    }

    return true;
}

/**
 * @brief Create render target resources for ImGui rendering
 * @param device The Vulkan device
 * @param swapchain The swapchain to create render targets for
 */
static void CreateRenderTarget(VkDevice device, VkSwapchainKHR swapchain) {
    VkResult result = VK_SUCCESS;
    
    // Get swapchain images
    uint32_t uImageCount = 0;
    result = vkGetSwapchainImagesKHR(device, swapchain, &uImageCount, nullptr);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to get swapchain image count: " << result << std::endl;
        return;
    }
    
    // Limit image count to our array size
    if (uImageCount > 8) {
        std::cerr << "[WARNING] Swapchain has more images than we can handle, limiting to 8" << std::endl;
        uImageCount = 8;
    }

    // Get the actual swapchain images
    VkImage backbuffers[8] = {};
    result = vkGetSwapchainImagesKHR(device, swapchain, &uImageCount, backbuffers);
    if (result != VK_SUCCESS) {
        std::cerr << "[ERROR] Failed to get swapchain images: " << result << std::endl;
        return;
    }
    
    g_MinImageCount = uImageCount;
    
    // Create resources for each swapchain image
    for (uint32_t i = 0; i < uImageCount; ++i) {
        g_Frames[i].Backbuffer = backbuffers[i];

        ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
        ImGui_ImplVulkanH_FrameSemaphores* fsd = &g_FrameSemaphores[i];
        
        // Create command pool
        {
            VkCommandPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = g_QueueFamily;

            result = vkCreateCommandPool(device, &info, g_Allocator, &fd->CommandPool);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create command pool: " << result << std::endl;
                return;
            }
        }
        
        // Allocate command buffer
        {
            VkCommandBufferAllocateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandPool = fd->CommandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;

            result = vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to allocate command buffer: " << result << std::endl;
                return;
            }
        }
        
        // Create fence
        {
            VkFenceCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            
            result = vkCreateFence(device, &info, g_Allocator, &fd->Fence);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create fence: " << result << std::endl;
                return;
            }
        }
        
        // Create semaphores
        {
            VkSemaphoreCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            
            result = vkCreateSemaphore(device, &info, g_Allocator, &fsd->ImageAcquiredSemaphore);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create image acquired semaphore: " << result << std::endl;
                return;
            }
            
            result = vkCreateSemaphore(device, &info, g_Allocator, &fsd->RenderCompleteSemaphore);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create render complete semaphore: " << result << std::endl;
                return;
            }
        }
    }

    // Create the Render Pass
    if (g_RenderPass == VK_NULL_HANDLE) {
        VkAttachmentDescription attachment = {};
        attachment.format = VK_FORMAT_B8G8R8A8_UNORM;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

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

        result = vkCreateRenderPass(device, &info, g_Allocator, &g_RenderPass);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to create render pass: " << result << std::endl;
            return;
        }
    }

    // Create Image Views
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = VK_FORMAT_B8G8R8A8_UNORM;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;
        
        for (uint32_t i = 0; i < uImageCount; ++i) {
            ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
            info.image = fd->Backbuffer;

            result = vkCreateImageView(device, &info, g_Allocator, &fd->BackbufferView);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create image view: " << result << std::endl;
                return;
            }
        }
    }

    // Create Framebuffers
    {
        // Default to 4K resolution if we don't have valid dimensions
        const uint32_t width = (g_ImageExtent.width > 0) ? g_ImageExtent.width : 3840;
        const uint32_t height = (g_ImageExtent.height > 0) ? g_ImageExtent.height : 2160;
        
        VkImageView attachment[1];
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = g_RenderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachment;
        info.layers = 1;
        info.width = width;
        info.height = height;
        
        for (uint32_t i = 0; i < uImageCount; ++i) {
            ImGui_ImplVulkanH_Frame* fd = &g_Frames[i];
            attachment[0] = fd->BackbufferView;

            result = vkCreateFramebuffer(device, &info, g_Allocator, &fd->Framebuffer);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to create framebuffer: " << result << std::endl;
                return;
            }
        }
    }

    // Create Descriptor Pool if it doesn't exist yet
    if (!g_DescriptorPool) {
        constexpr VkDescriptorPoolSize pool_sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
            {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000}
        };

        VkDescriptorPoolCreateInfo pool_info = {};
        pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pool_info.maxSets = 1000 * IM_ARRAYSIZE(pool_sizes);
        pool_info.poolSizeCount = static_cast<uint32_t>(IM_ARRAYSIZE(pool_sizes));
        pool_info.pPoolSizes = pool_sizes;

        result = vkCreateDescriptorPool(device, &pool_info, g_Allocator, &g_DescriptorPool);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to create descriptor pool: " << result << std::endl;
            return;
        }
    }
}

VK_LAYER_EXPORT VkResult VKAPI_CALL ModLoader_CreateInstance(
    const VkInstanceCreateInfo*                 pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkInstance*                                 pInstance)
{
  VkLayerInstanceCreateInfo *layerCreateInfo = (VkLayerInstanceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerInstanceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  PFN_vkGetInstanceProcAddr gpa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateInstance createFunc = (PFN_vkCreateInstance)gpa(VK_NULL_HANDLE, "vkCreateInstance");

  VkResult ret = createFunc(pCreateInfo, pAllocator, pInstance);

  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerInstanceDispatchTable dispatchTable;
  dispatchTable.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)gpa(*pInstance, "vkGetInstanceProcAddr");
  dispatchTable.DestroyInstance = (PFN_vkDestroyInstance)gpa(*pInstance, "vkDestroyInstance");
  //dispatchTable.EnumerateDeviceExtensionProperties = (PFN_vkEnumerateDeviceExtensionProperties)gpa(*pInstance, "vkEnumerateDeviceExtensionProperties");

  // store the table by key
  {
    scoped_lock l(global_lock);
    instance_dispatch[GetKey(*pInstance)] = dispatchTable;
  }
  return VK_SUCCESS;
}  

VK_LAYER_EXPORT void VKAPI_CALL ModLoader_DestroyInstance(VkInstance instance, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  instance_dispatch.erase(GetKey(instance));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL ModLoader_CreateDevice(
    VkPhysicalDevice                            physicalDevice,
    const VkDeviceCreateInfo*                   pCreateInfo,
    const VkAllocationCallbacks*                pAllocator,
    VkDevice*                                   pDevice)
{

  VkLayerDeviceCreateInfo *layerCreateInfo = (VkLayerDeviceCreateInfo *)pCreateInfo->pNext;

  // step through the chain of pNext until we get to the link info
  while(layerCreateInfo && (layerCreateInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                            layerCreateInfo->function != VK_LAYER_LINK_INFO))
  {
    layerCreateInfo = (VkLayerDeviceCreateInfo *)layerCreateInfo->pNext;
  }

  if(layerCreateInfo == NULL)
  {
    // No loader instance create info
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  
  PFN_vkGetInstanceProcAddr gipa = layerCreateInfo->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  PFN_vkGetDeviceProcAddr gdpa = layerCreateInfo->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  // move chain on for next layer
  layerCreateInfo->u.pLayerInfo = layerCreateInfo->u.pLayerInfo->pNext;

  PFN_vkCreateDevice createFunc = (PFN_vkCreateDevice)gipa(VK_NULL_HANDLE, "vkCreateDevice");

  VkResult ret = createFunc(physicalDevice, pCreateInfo, pAllocator, pDevice);

  
  // fetch our own dispatch table for the functions we need, into the next layer
  VkLayerDispatchTable dispatchTable;
  dispatchTable.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gdpa(*pDevice, "vkGetDeviceProcAddr");
  dispatchTable.DestroyDevice = (PFN_vkDestroyDevice)gdpa(*pDevice, "vkDestroyDevice");
  dispatchTable.QueuePresentKHR = (PFN_vkQueuePresentKHR)gdpa(*pDevice, "vkQueuePresentKHR");
  dispatchTable.CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)gdpa(*pDevice, "vkCreateSwapchainKHR");
 
  dispatchTable.GetDeviceQueue = (PFN_vkGetDeviceQueue)gdpa(*pDevice, "vkGetDeviceQueue");

  GetDeviceData(*pDevice)->vtable = dispatchTable;
  GetDeviceData(*pDevice)->device = *pDevice;


  VkLayerDeviceCreateInfo *load_data_info = get_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
  GetDeviceData(*pDevice)->set_device_loader_data = load_data_info->u.pfnSetDeviceLoaderData;


  DeviceMapQueues(GetDeviceData(*pDevice), pCreateInfo);
  // store the table by key
  {
    scoped_lock l(global_lock);
    device_dispatch[GetKey(*pDevice)] = dispatchTable;
  }

  return VK_SUCCESS;
}

VK_LAYER_EXPORT void VKAPI_CALL ModLoader_DestroyDevice(VkDevice device, const VkAllocationCallbacks* pAllocator)
{
  scoped_lock l(global_lock);
  device_dispatch.erase(GetKey(device));
}

VK_LAYER_EXPORT VkResult VKAPI_CALL ModLoader_QueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo){
  if(!g_Hwnd) return device_dispatch[GetKey(queue)].QueuePresentKHR(queue, pPresentInfo);
  
  return RenderImGui_Vulkan(queue, pPresentInfo);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL ModLoader_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {

  CleanupRenderTarget( );
  g_ImageExtent = pCreateInfo->imageExtent;

  return device_dispatch[GetKey(device)].CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
}

VK_LAYER_EXPORT VkResult VKAPI_CALL ModLoader_AcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence, uint32_t* pImageIndex) {

  //g_Device = device;
  std::cout << "why am I being called?\n";

  return device_dispatch[GetKey(device)].AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
}
#define GETPROCADDR(func) if(!strcmp(pName, "vk" #func)) return (PFN_vkVoidFunction)&ModLoader_##func;

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL ModLoader_GetDeviceProcAddr(VkDevice device, const char *pName)
{
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);
  GETPROCADDR(QueuePresentKHR);
  GETPROCADDR(CreateSwapchainKHR);
  
  {
    scoped_lock l(global_lock);
    return device_dispatch[GetKey(device)].GetDeviceProcAddr(device, pName);
  }
}

VK_LAYER_EXPORT PFN_vkVoidFunction VKAPI_CALL ModLoader_GetInstanceProcAddr(VkInstance instance, const char *pName)
{
  // instance chain functions we intercept
  GETPROCADDR(GetInstanceProcAddr);
  GETPROCADDR(CreateInstance);
  GETPROCADDR(DestroyInstance);
  
  // device chain functions we intercept
  GETPROCADDR(GetDeviceProcAddr);
  GETPROCADDR(CreateDevice);
  GETPROCADDR(DestroyDevice);

  {
    scoped_lock l(global_lock);
    return instance_dispatch[GetKey(instance)].GetInstanceProcAddr(instance, pName);
  }
}

void layer::setup(HWND hwnd){

  if (!CreateDeviceVK( )) {
    std::cout << "[!] CreateDeviceVK() failed.\n";
    return;
  }
  g_Hwnd = hwnd;
}

/**
 * @brief Clean up render target resources
 */
static void CleanupRenderTarget() {
    // Skip cleanup if device is not valid
    if (g_Device == VK_NULL_HANDLE) {
        return;
    }
    
    // Clean up frame resources
    for (uint32_t i = 0; i < RTL_NUMBER_OF(g_Frames); ++i) {
        // Wait for any pending operations to complete
        if (g_Frames[i].Fence != VK_NULL_HANDLE) {
            vkWaitForFences(g_Device, 1, &g_Frames[i].Fence, VK_TRUE, 1000000000); // 1 second timeout
            vkDestroyFence(g_Device, g_Frames[i].Fence, g_Allocator);
            g_Frames[i].Fence = VK_NULL_HANDLE;
        }
        
        // Free command buffers
        if (g_Frames[i].CommandBuffer != VK_NULL_HANDLE && g_Frames[i].CommandPool != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(g_Device, g_Frames[i].CommandPool, 1, &g_Frames[i].CommandBuffer);
            g_Frames[i].CommandBuffer = VK_NULL_HANDLE;
        }
        
        // Destroy command pools
        if (g_Frames[i].CommandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(g_Device, g_Frames[i].CommandPool, g_Allocator);
            g_Frames[i].CommandPool = VK_NULL_HANDLE;
        }
        
        // Destroy image views
        if (g_Frames[i].BackbufferView != VK_NULL_HANDLE) {
            vkDestroyImageView(g_Device, g_Frames[i].BackbufferView, g_Allocator);
            g_Frames[i].BackbufferView = VK_NULL_HANDLE;
        }
        
        // Destroy framebuffers
        if (g_Frames[i].Framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(g_Device, g_Frames[i].Framebuffer, g_Allocator);
            g_Frames[i].Framebuffer = VK_NULL_HANDLE;
        }
        
        // Note: We don't destroy the backbuffer images as they are owned by the swapchain
        g_Frames[i].Backbuffer = VK_NULL_HANDLE;
    }

    // Clean up frame semaphores
    for (uint32_t i = 0; i < RTL_NUMBER_OF(g_FrameSemaphores); ++i) {
        if (g_FrameSemaphores[i].ImageAcquiredSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(g_Device, g_FrameSemaphores[i].ImageAcquiredSemaphore, g_Allocator);
            g_FrameSemaphores[i].ImageAcquiredSemaphore = VK_NULL_HANDLE;
        }
        if (g_FrameSemaphores[i].RenderCompleteSemaphore != VK_NULL_HANDLE) {
            vkDestroySemaphore(g_Device, g_FrameSemaphores[i].RenderCompleteSemaphore, g_Allocator);
            g_FrameSemaphores[i].RenderCompleteSemaphore = VK_NULL_HANDLE;
        }
    }
    
    // Destroy render pass
    if (g_RenderPass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(g_Device, g_RenderPass, g_Allocator);
        g_RenderPass = VK_NULL_HANDLE;
    }
}

/**
 * @brief Clean up all Vulkan resources
 */
static void CleanupDeviceVulkan() {
    // First clean up render target resources
    CleanupRenderTarget();

    // Clean up descriptor pool
    if (g_DescriptorPool != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(g_Device, g_DescriptorPool, g_Allocator);
        g_DescriptorPool = VK_NULL_HANDLE;
    }
    
    // Clean up pipeline cache
    if (g_PipelineCache != VK_NULL_HANDLE && g_Device != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(g_Device, g_PipelineCache, g_Allocator);
        g_PipelineCache = VK_NULL_HANDLE;
    }
    
    // Clean up fake device if it exists
    if (g_FakeDevice != VK_NULL_HANDLE) {
        vkDestroyDevice(g_FakeDevice, g_Allocator);
        g_FakeDevice = VK_NULL_HANDLE;
    }
    
    // Clean up instance
    if (g_Instance != VK_NULL_HANDLE) {
        vkDestroyInstance(g_Instance, g_Allocator);
        g_Instance = VK_NULL_HANDLE;
    }

    // Reset other global state
    g_ImageExtent = {};
    g_Device = VK_NULL_HANDLE;
    g_PhysicalDevice = VK_NULL_HANDLE;
    g_GraphicsQueue = VK_NULL_HANDLE;
    g_CommandBuffer = VK_NULL_HANDLE;
    g_MinImageCount = 1;
    g_QueueFamily = static_cast<uint32_t>(-1);
    g_QueueFamilies.clear();
}

/**
 * @brief Render ImGui interface using Vulkan
 * @param queue The Vulkan queue to submit rendering commands to
 * @param pPresentInfo The presentation info structure
 * @return VkResult indicating success or failure
 */
static VkResult RenderImGui_Vulkan(VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    if (!queue || !pPresentInfo) {
        std::cerr << "[ERROR] Invalid queue or present info" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    // Get queue data and device
    QueueData* queue_data = GetQueueData(queue);
    if (!queue_data || !queue_data->device) {
        std::cerr << "[ERROR] Failed to get queue data or device" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    g_Device = queue_data->device->device;
    if (g_Device == VK_NULL_HANDLE) {
        std::cerr << "[ERROR] Invalid device handle" << std::endl;
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result = VK_SUCCESS;
    VkQueue graphicQueue = queue_data->device->graphic_queue->queue;
    const bool queueSupportsGraphic = DoesQueueSupportGraphic(queue, &graphicQueue);
    
    // Initialize ImGui context
    Menu::InitializeContext(g_Hwnd);

    // Process each swapchain
    for (uint32_t i = 0; i < pPresentInfo->swapchainCount; ++i) {
        VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[i];
        
        // Create render target if needed
        if (g_Frames[0].Framebuffer == VK_NULL_HANDLE) {
            CreateRenderTarget(g_Device, swapchain);
        }
        
        uint32_t image_index = pPresentInfo->pImageIndices[i];
        if (image_index >= 8) {
            std::cerr << "[ERROR] Image index out of bounds: " << image_index << std::endl;
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        
        ImGui_ImplVulkanH_Frame* fd = &g_Frames[image_index];
        ImGui_ImplVulkanH_FrameSemaphores* fsd = &g_FrameSemaphores[image_index];
        
        // Wait for and reset fence
        result = vkWaitForFences(g_Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to wait for fence: " << result << std::endl;
            return result;
        }
        
        result = vkResetFences(g_Device, 1, &fd->Fence);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to reset fence: " << result << std::endl;
            return result;
        }
        
        // Reset and begin command buffer
        result = vkResetCommandBuffer(fd->CommandBuffer, 0);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to reset command buffer: " << result << std::endl;
            return result;
        }

        VkCommandBufferBeginInfo beginInfo = {};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        result = vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to begin command buffer: " << result << std::endl;
            return result;
        }
        
        // Begin render pass
        VkRenderPassBeginInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = g_RenderPass;
        renderPassInfo.framebuffer = fd->Framebuffer;
        
        // Use default size if image extent is not set
        if (g_ImageExtent.width == 0 || g_ImageExtent.height == 0) {
            renderPassInfo.renderArea.extent.width = 3840;  // 4K default
            renderPassInfo.renderArea.extent.height = 2160;
        } else {
            renderPassInfo.renderArea.extent = g_ImageExtent;
        }

        vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Initialize ImGui Vulkan implementation if needed
        if (!ImGui::GetIO().BackendRendererUserData) {
            ImGui_ImplVulkan_InitInfo init_info = {};
            init_info.Instance = g_Instance;
            init_info.PhysicalDevice = g_PhysicalDevice;
            init_info.Device = g_Device;
            init_info.QueueFamily = g_QueueFamily;
            init_info.Queue = graphicQueue;
            init_info.PipelineCache = g_PipelineCache;
            init_info.DescriptorPool = g_DescriptorPool;
            init_info.RenderPass = g_RenderPass;
            init_info.Subpass = 0;
            init_info.MinImageCount = g_MinImageCount;
            init_info.ImageCount = g_MinImageCount;
            init_info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
            init_info.Allocator = g_Allocator;
            
            ImGui_ImplVulkan_Init(&init_info);
            ImGui_ImplVulkan_CreateFontsTexture();
        }

        // Prepare ImGui frame
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Render menu
        Menu::Render();

        // Finalize ImGui rendering
        ImGui::Render();
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->CommandBuffer);

        // End render pass and command buffer
        vkCmdEndRenderPass(fd->CommandBuffer);
        
        result = vkEndCommandBuffer(fd->CommandBuffer);
        if (result != VK_SUCCESS) {
            std::cerr << "[ERROR] Failed to end command buffer: " << result << std::endl;
            return result;
        }

        // Submit command buffer and present
        uint32_t waitSemaphoresCount = i == 0 ? pPresentInfo->waitSemaphoreCount : 0;
        
        if (waitSemaphoresCount == 0 && !queueSupportsGraphic) {
            // Handle case where queue doesn't support graphics operations
            constexpr VkPipelineStageFlags stages_wait = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
            
            // First submission to signal render complete
            {
                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.pWaitDstStageMask = &stages_wait;
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = &fsd->RenderCompleteSemaphore;

                result = vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
                if (result != VK_SUCCESS) {
                    std::cerr << "[ERROR] Failed to submit to queue: " << result << std::endl;
                    return result;
                }
            }
            
            // Second submission with command buffer
            {
                VkSubmitInfo submitInfo = {};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &fd->CommandBuffer;
                submitInfo.pWaitDstStageMask = &stages_wait;
                submitInfo.waitSemaphoreCount = 1;
                submitInfo.pWaitSemaphores = &fsd->RenderCompleteSemaphore;
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = &fsd->ImageAcquiredSemaphore;

                result = vkQueueSubmit(graphicQueue, 1, &submitInfo, fd->Fence);
                if (result != VK_SUCCESS) {
                    std::cerr << "[ERROR] Failed to submit to graphics queue: " << result << std::endl;
                    return result;
                }
            }
        } else {
            // Standard submission path
            std::vector<VkPipelineStageFlags> stages_wait(waitSemaphoresCount, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

            VkSubmitInfo submitInfo = {};
            submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            submitInfo.commandBufferCount = 1;
            submitInfo.pCommandBuffers = &fd->CommandBuffer;
            submitInfo.pWaitDstStageMask = stages_wait.data();
            submitInfo.waitSemaphoreCount = waitSemaphoresCount;
            submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &fsd->ImageAcquiredSemaphore;
            
            result = vkQueueSubmit(graphicQueue, 1, &submitInfo, fd->Fence);
            if (result != VK_SUCCESS) {
                std::cerr << "[ERROR] Failed to submit to graphics queue: " << result << std::endl;
                return result;
            }

            // Present the swapchain image
            VkPresentInfoKHR present_info = *pPresentInfo;
            present_info.swapchainCount = 1;
            present_info.pSwapchains = &swapchain;
            present_info.pImageIndices = &image_index;
            present_info.pWaitSemaphores = &fsd->ImageAcquiredSemaphore;
            present_info.waitSemaphoreCount = 1;

            VkResult chain_result = queue_data->device->vtable.QueuePresentKHR(queue, &present_info);
            
            // Store result if requested
            if (pPresentInfo->pResults) {
                pPresentInfo->pResults[i] = chain_result;
            }
            
            // Update overall result if needed
            if (chain_result != VK_SUCCESS && result == VK_SUCCESS) {
                result = chain_result;
            }
        }
    }
    
    return result;
}

/**
 * @brief Check if a queue supports graphics operations and optionally find a graphics queue
 * @param queue The queue to check
 * @param pGraphicQueue Optional pointer to store a graphics queue if found
 * @return true if the queue supports graphics operations, false otherwise
 */
static bool DoesQueueSupportGraphic(VkQueue queue, VkQueue* pGraphicQueue) {
    if (queue == VK_NULL_HANDLE || g_Device == VK_NULL_HANDLE) {
        return false;
    }
    
    bool queueSupportsGraphics = false;
    
    // Iterate through all queue families
    for (uint32_t i = 0; i < g_QueueFamilies.size(); ++i) {
        const VkQueueFamilyProperties& family = g_QueueFamilies[i];
        
        // Skip if this family doesn't support graphics operations
        const bool familySupportsGraphics = (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0;
        
        // Iterate through all queues in this family
        for (uint32_t j = 0; j < family.queueCount; ++j) {
            VkQueue currentQueue = VK_NULL_HANDLE;
            vkGetDeviceQueue(g_Device, i, j, &currentQueue);
            
            if (currentQueue == VK_NULL_HANDLE) {
                continue;
            }

            // If we need a graphics queue and this family supports graphics,
            // store the first graphics queue we find if none has been stored yet
            if (pGraphicQueue && familySupportsGraphics && *pGraphicQueue == VK_NULL_HANDLE) {
                *pGraphicQueue = currentQueue;
            }

            // Check if this is the queue we're looking for and if it supports graphics
            if (queue == currentQueue && familySupportsGraphics) {
                queueSupportsGraphics = true;
                // We can break here if we've already found a graphics queue or don't need one
                if (!pGraphicQueue || *pGraphicQueue != VK_NULL_HANDLE) {
                    return true;
                }
            }
        }
    }
    return queueSupportsGraphics;
}