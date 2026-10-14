#include "render/vk/Device.h"

#include "core/Clock.h"
#include "core/Log.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(__ANDROID__)
#include <swappy/swappyVk.h>
#endif

namespace vd::vk {
namespace {

constexpr const char* kValidationLayer = "VK_LAYER_KHRONOS_validation";

bool hasInstanceLayer(const char* name) {
    u32 count = 0;
    vkEnumerateInstanceLayerProperties(&count, nullptr);
    std::vector<VkLayerProperties> layers(count);
    vkEnumerateInstanceLayerProperties(&count, layers.data());
    for (const VkLayerProperties& layer : layers) {
        if (std::strcmp(layer.layerName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool hasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const VkExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

std::vector<VkExtensionProperties> instanceExtensions() {
    u32 count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    return extensions;
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice gpu) {
    u32 count = 0;
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> extensions(count);
    vkEnumerateDeviceExtensionProperties(gpu, nullptr, &count, extensions.data());
    return extensions;
}

// Graphics queues can present on Android and Windows; the surface check happens when one exists.
bool findGraphicsQueueFamily(VkPhysicalDevice gpu, u32& family, u32& timestampValidBits) {
    u32 count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &count, families.data());
    for (u32 i = 0; i < count; ++i) {
        if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            family = i;
            timestampValidBits = families[i].timestampValidBits;
            return true;
        }
    }
    return false;
}

int deviceTypeScore(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 3;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 2;
    default: return 1;
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT /*types*/,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data,
                                             void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        VD_LOG_ERROR("[vulkan] %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        VD_LOG_WARN("[vulkan] %s", data->pMessage);
    }
    return VK_FALSE;
}

// VMA's device memory allocations (whole blocks, rare) with their cost: on Adreno a new block can take tens of
// milliseconds, which shows up as a single long frame.
PFN_vkAllocateMemory g_allocateMemory = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL timedAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                   const VkAllocationCallbacks* callbacks, VkDeviceMemory* memory) {
    const Clock::time_point start = Clock::now();
    const VkResult result = g_allocateMemory(device, info, callbacks, memory);
    VD_LOG_INFO("GPU memory block: %llu KB, type %u, %.2f ms%s",
                static_cast<unsigned long long>(info->allocationSize >> 10), info->memoryTypeIndex,
                secondsBetween(start, Clock::now()) * 1000.0, result == VK_SUCCESS ? "" : " (failed)");
    return result;
}

} // namespace

Result<void> Device::create(const DeviceOptions& options) {
    VD_VK_TRY(volkInitialize());
    if (volkGetInstanceVersion() < VK_API_VERSION_1_1) {
        return Error{"Vulkan 1.1 is not available on this system"};
    }
    VD_TRY(createInstance(options));
    VD_TRY(pickPhysicalDevice());
    VD_TRY(createLogicalDevice());

    VmaAllocatorCreateInfo allocatorInfo{};
    allocatorInfo.physicalDevice = m_physicalDevice;
    allocatorInfo.device = m_device;
    allocatorInfo.instance = m_instance;
    allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_1;
    // Small blocks: VMA's default grows to 256 MB, and allocating one mid-game took up to 62 ms on Adreno 650
    // (a 32 MB block: ~1 ms). A few dozen blocks are far below maxMemoryAllocationCount.
    allocatorInfo.preferredLargeHeapBlockSize = 32ull << 20;
    VmaVulkanFunctions functions{};
    VD_VK_TRY(vmaImportVulkanFunctionsFromVolk(&allocatorInfo, &functions));
    g_allocateMemory = functions.vkAllocateMemory;
    functions.vkAllocateMemory = timedAllocateMemory;
    allocatorInfo.pVulkanFunctions = &functions;
    VD_VK_TRY(vmaCreateAllocator(&allocatorInfo, &m_allocator));
    createStreamPools();
    m_pipelineCachePath = options.pipelineCachePath;
    createPipelineCache();

    const u32 api = m_properties.apiVersion;
    VD_LOG_INFO("GPU: %s | Vulkan %u.%u.%u | driver 0x%08x | timestamps: %s | ASTC: %s", m_properties.deviceName,
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), VK_API_VERSION_PATCH(api),
                m_properties.driverVersion, m_timestampValidBits ? "yes" : "no",
                m_astcSupported ? "yes" : "no (CPU decode fallback)");
    return {};
}

Result<void> Device::createInstance(const DeviceOptions& options) {
    std::vector<const char*> layers;
    std::vector<const char*> extensions{VK_KHR_SURFACE_EXTENSION_NAME};
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    extensions.push_back(VK_KHR_WIN32_SURFACE_EXTENSION_NAME);
#endif

    if (options.enableValidation) {
        if (hasInstanceLayer(kValidationLayer)) {
            layers.push_back(kValidationLayer);
            VD_LOG_INFO("Vulkan validation layer enabled");
        } else {
            VD_LOG_WARN("Vulkan validation requested but %s is not installed (install the Vulkan SDK)",
                        kValidationLayer);
        }
        if (hasExtension(instanceExtensions(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            m_debugUtils = true;
        }
    }

    VkApplicationInfo appInfo{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appInfo.pApplicationName = "VanillaDroid";
    appInfo.applicationVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.pEngineName = "vd";
    appInfo.engineVersion = VK_MAKE_API_VERSION(0, 0, 1, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &appInfo;
    instanceInfo.enabledLayerCount = static_cast<u32>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();
    VD_VK_TRY(vkCreateInstance(&instanceInfo, nullptr, &m_instance));
    volkLoadInstance(m_instance);

    if (m_debugUtils) {
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        messengerInfo.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messengerInfo.pfnUserCallback = debugCallback;
        VD_VK_TRY(vkCreateDebugUtilsMessengerEXT(m_instance, &messengerInfo, nullptr, &m_debugMessenger));
    }
    return {};
}

Result<void> Device::pickPhysicalDevice() {
    u32 count = 0;
    VD_VK_TRY(vkEnumeratePhysicalDevices(m_instance, &count, nullptr));
    std::vector<VkPhysicalDevice> gpus(count);
    VD_VK_TRY(vkEnumeratePhysicalDevices(m_instance, &count, gpus.data()));

    int bestScore = 0;
    for (VkPhysicalDevice gpu : gpus) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(gpu, &properties);
        u32 family = 0;
        u32 timestampBits = 0;
        if (properties.apiVersion < VK_API_VERSION_1_1 || !findGraphicsQueueFamily(gpu, family, timestampBits) ||
            !hasExtension(deviceExtensions(gpu), VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        const int score = deviceTypeScore(properties.deviceType);
        if (score > bestScore) {
            bestScore = score;
            m_physicalDevice = gpu;
            m_properties = properties;
            m_queueFamily = family;
            m_timestampValidBits = timestampBits;
        }
    }
    if (m_physicalDevice == VK_NULL_HANDLE) {
        return Error{"No GPU with Vulkan 1.1, a graphics queue and swapchain support"};
    }
    return {};
}

Result<void> Device::createLogicalDevice() {
    std::vector<VkExtensionProperties> available = deviceExtensions(m_physicalDevice);
    std::vector<const char*> extensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};

#if defined(__ANDROID__)
    // Swappy may want extra extensions (e.g. VK_GOOGLE_display_timing) for precise pacing.
    u32 swappyCount = 0;
    SwappyVk_determineDeviceExtensions(m_physicalDevice, static_cast<u32>(available.size()), available.data(),
                                       &swappyCount, nullptr);
    std::vector<std::array<char, VK_MAX_EXTENSION_NAME_SIZE>> swappyNames(swappyCount);
    std::vector<char*> swappyPointers(swappyCount);
    for (u32 i = 0; i < swappyCount; ++i) {
        swappyPointers[i] = swappyNames[i].data();
    }
    SwappyVk_determineDeviceExtensions(m_physicalDevice, static_cast<u32>(available.size()), available.data(),
                                       &swappyCount, swappyPointers.data());
    for (u32 i = 0; i < swappyCount; ++i) {
        if (std::strcmp(swappyPointers[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME) != 0) {
            extensions.push_back(swappyPointers[i]);
        }
    }
#endif

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = m_queueFamily;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;

    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(m_physicalDevice, &supported);
    VkPhysicalDeviceFeatures enabled{};
    enabled.textureCompressionASTC_LDR = supported.textureCompressionASTC_LDR;
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    m_astcSupported = supported.textureCompressionASTC_LDR == VK_TRUE;
    m_anisotropySupported = supported.samplerAnisotropy == VK_TRUE;

    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<u32>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    deviceInfo.pEnabledFeatures = &enabled;
    VD_VK_TRY(vkCreateDevice(m_physicalDevice, &deviceInfo, nullptr, &m_device));

    // Direct device dispatch: skips the loader trampoline on every call.
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

#if defined(__ANDROID__)
    SwappyVk_setQueueFamilyIndex(m_device, m_queue, m_queueFamily);
#endif
    for (const char* extension : extensions) {
        VD_LOG_DEBUG("Device extension: %s", extension);
    }
    return {};
}

void Device::createStreamPools() {
    // Blocks large enough for the biggest baked model (Stormwind, 32 MB). 384 MB in all: the benchmark routes
    // grew past 320 MB once.
    constexpr VkDeviceSize kBlockSize = 48ull << 20;
    constexpr size_t kReservedBlocks = 8;
    // Memory types VMA would pick for typical streamed resources.
    VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bufferInfo.size = 65536;
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.extent = {256, 256, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    u32 bufferType = 0;
    u32 imageType = 0;
    if (vmaFindMemoryTypeIndexForBufferInfo(m_allocator, &bufferInfo, &allocInfo, &bufferType) != VK_SUCCESS ||
        vmaFindMemoryTypeIndexForImageInfo(m_allocator, &imageInfo, &allocInfo, &imageType) != VK_SUCCESS) {
        VD_LOG_WARN("No memory type for the streaming pools: blocks will be allocated while playing");
        return;
    }
    auto createPool = [&](u32 type, size_t blocks) {
        VmaPoolCreateInfo poolInfo{};
        poolInfo.memoryTypeIndex = type;
        poolInfo.blockSize = kBlockSize;
        poolInfo.minBlockCount = blocks; // allocated now, behind the loading screen
        VmaPool pool = VK_NULL_HANDLE;
        if (vmaCreatePool(m_allocator, &poolInfo, &pool) != VK_SUCCESS) {
            VD_LOG_WARN("Streaming pool (memory type %u, %zu blocks) not created", type, blocks);
            return static_cast<VmaPool>(VK_NULL_HANDLE);
        }
        return pool;
    };
    if (bufferType == imageType) {
        m_bufferPool = createPool(bufferType, kReservedBlocks);
        m_imagePool = m_bufferPool;
    } else {
        m_imagePool = createPool(imageType, kReservedBlocks * 6 / 10); // textures dominate
        m_bufferPool = createPool(bufferType, kReservedBlocks * 4 / 10);
    }
    VD_LOG_INFO("GPU memory reserved for streaming: %llu MB (memory types %u/%u)",
                static_cast<unsigned long long>(kReservedBlocks * kBlockSize >> 20), bufferType, imageType);
}

VkResult Device::createStreamedBuffer(const VkBufferCreateInfo& info, VkBuffer* buffer,
                                      VmaAllocation* allocation) const {
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (m_bufferPool != VK_NULL_HANDLE) {
        allocInfo.pool = m_bufferPool;
        if (vmaCreateBuffer(m_allocator, &info, &allocInfo, buffer, allocation, nullptr) == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        allocInfo.pool = VK_NULL_HANDLE;
    }
    return vmaCreateBuffer(m_allocator, &info, &allocInfo, buffer, allocation, nullptr);
}

VkResult Device::createStreamedImage(const VkImageCreateInfo& info, VkImage* image, VmaAllocation* allocation) const {
    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    if (m_imagePool != VK_NULL_HANDLE) {
        allocInfo.pool = m_imagePool;
        if (vmaCreateImage(m_allocator, &info, &allocInfo, image, allocation, nullptr) == VK_SUCCESS) {
            return VK_SUCCESS;
        }
        allocInfo.pool = VK_NULL_HANDLE;
    }
    return vmaCreateImage(m_allocator, &info, &allocInfo, image, allocation, nullptr);
}

void Device::createPipelineCache() {
    // Reuse the previous run's cache only if it was written by this exact GPU + driver: some drivers
    // do not cope well with foreign data.
    std::vector<u8> data;
    if (!m_pipelineCachePath.empty()) {
        if (std::FILE* file = std::fopen(m_pipelineCachePath.c_str(), "rb")) {
            std::fseek(file, 0, SEEK_END);
            const long size = std::ftell(file);
            std::fseek(file, 0, SEEK_SET);
            data.resize(size > 0 ? static_cast<usize>(size) : 0);
            if (std::fread(data.data(), 1, data.size(), file) != data.size()) {
                data.clear();
            }
            std::fclose(file);
        }
    }
    VkPipelineCacheHeaderVersionOne header{};
    if (data.size() >= sizeof(header)) {
        std::memcpy(&header, data.data(), sizeof(header));
    }
    const bool valid = data.size() >= sizeof(header) && header.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                       header.vendorID == m_properties.vendorID && header.deviceID == m_properties.deviceID &&
                       std::memcmp(header.pipelineCacheUUID, m_properties.pipelineCacheUUID, VK_UUID_SIZE) == 0;
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    if (valid) {
        info.initialDataSize = data.size();
        info.pInitialData = data.data();
    }
    if (vkCreatePipelineCache(m_device, &info, nullptr, &m_pipelineCache) != VK_SUCCESS) {
        m_pipelineCache = VK_NULL_HANDLE; // pipelines still work, just without a cache
    }
    VD_LOG_INFO("Pipeline cache: %s", valid ? "loaded" : (data.empty() ? "new" : "stale, rebuilt"));
}

void Device::savePipelineCache() const {
    if (m_pipelineCache == VK_NULL_HANDLE || m_pipelineCachePath.empty()) {
        return;
    }
    usize size = 0;
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;
    }
    std::vector<u8> data(size);
    if (vkGetPipelineCacheData(m_device, m_pipelineCache, &size, data.data()) != VK_SUCCESS) {
        return;
    }
    // Temp file + rename: a crash while saving never leaves a truncated cache behind.
    const std::string temp = m_pipelineCachePath + ".tmp";
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file) {
        return;
    }
    const bool written = std::fwrite(data.data(), 1, size, file) == size;
    std::fclose(file);
    if (written) {
        std::remove(m_pipelineCachePath.c_str());
        std::rename(temp.c_str(), m_pipelineCachePath.c_str());
    }
}

void Device::destroy() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        savePipelineCache();
        if (m_pipelineCache != VK_NULL_HANDLE) {
            vkDestroyPipelineCache(m_device, m_pipelineCache, nullptr);
        }
        if (m_imagePool != VK_NULL_HANDLE && m_imagePool != m_bufferPool) {
            vmaDestroyPool(m_allocator, m_imagePool);
        }
        if (m_bufferPool != VK_NULL_HANDLE) {
            vmaDestroyPool(m_allocator, m_bufferPool);
        }
        m_imagePool = VK_NULL_HANDLE;
        m_bufferPool = VK_NULL_HANDLE;
        if (m_allocator != VK_NULL_HANDLE) {
            vmaDestroyAllocator(m_allocator);
        }
#if defined(__ANDROID__)
        SwappyVk_destroyDevice(m_device);
#endif
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_debugMessenger != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_debugMessenger, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
    *this = Device{};
}

} // namespace vd::vk
