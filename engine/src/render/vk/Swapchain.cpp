#include "render/vk/Swapchain.h"

#include "core/Log.h"
#include "render/vk/Device.h"

#include <algorithm>
#include <utility>

#if defined(__ANDROID__)
#include <swappy/swappyVk.h>
#endif

namespace vd::vk {
namespace {

bool isQuarterTurn(VkSurfaceTransformFlagBitsKHR transform) {
    return transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
           transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR;
}

const char* transformName(VkSurfaceTransformFlagBitsKHR transform) {
    switch (transform) {
    case VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR: return "identity";
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: return "rotate-90";
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: return "rotate-180";
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: return "rotate-270";
    default: return "other";
    }
}

// WoW's textures, vertex colors and blending were authored for gamma-space rendering, so the
// swapchain is UNORM (not SRGB) to keep the original look.
VkSurfaceFormatKHR chooseFormat(const std::vector<VkSurfaceFormatKHR>& formats) {
    for (VkFormat preferred : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
        for (const VkSurfaceFormatKHR& format : formats) {
            if (format.format == preferred && format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return format;
            }
        }
    }
    return formats.front();
}

VkCompositeAlphaFlagBitsKHR chooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported) {
    for (VkCompositeAlphaFlagBitsKHR mode :
         {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
          VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR}) {
        if (supported & mode) {
            return mode;
        }
    }
    return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
}

} // namespace

Result<void> Swapchain::createSurface(const Device& device, const PlatformInfo& platform,
                                      const NativeSurface& surface) {
    m_platform = platform;
    m_native = surface;

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
    VkAndroidSurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.window = static_cast<ANativeWindow*>(surface.window);
    VD_VK_TRY(vkCreateAndroidSurfaceKHR(device.instance(), &surfaceInfo, nullptr, &m_surface));
#elif defined(VK_USE_PLATFORM_WIN32_KHR)
    VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    surfaceInfo.hinstance = static_cast<HINSTANCE>(surface.instance);
    surfaceInfo.hwnd = static_cast<HWND>(surface.window);
    VD_VK_TRY(vkCreateWin32SurfaceKHR(device.instance(), &surfaceInfo, nullptr, &m_surface));
#else
#error "No Vulkan surface support for this platform"
#endif

    VkBool32 presentSupported = VK_FALSE;
    VD_VK_TRY(vkGetPhysicalDeviceSurfaceSupportKHR(device.physicalDevice(), device.queueFamily(), m_surface,
                                                   &presentSupported));
    if (!presentSupported) {
        return Error{"The graphics queue cannot present to this surface"};
    }

    u32 formatCount = 0;
    VD_VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(device.physicalDevice(), m_surface, &formatCount, nullptr));
    std::vector<VkSurfaceFormatKHR> formats(formatCount);
    VD_VK_TRY(
        vkGetPhysicalDeviceSurfaceFormatsKHR(device.physicalDevice(), m_surface, &formatCount, formats.data()));
    if (formats.empty()) {
        return Error{"Surface reports no formats"};
    }
    m_format = chooseFormat(formats);
    return {};
}

void Swapchain::destroySurface(const Device& device) {
    destroy(device);
    if (m_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device.instance(), m_surface, nullptr);
        m_surface = VK_NULL_HANDLE;
    }
    m_native = {};
}

void Swapchain::setNativeSize(u32 width, u32 height) {
    m_native.width = width;
    m_native.height = height;
}

Swapchain::SurfaceState Swapchain::desiredState(const VkSurfaceCapabilitiesKHR& caps) const {
    SurfaceState state;
    state.extent = caps.currentExtent;
    if (state.extent.width == UINT32_MAX) {
        // The platform lets the swapchain decide: use the window size.
        state.extent.width = std::clamp(m_native.width, caps.minImageExtent.width, caps.maxImageExtent.width);
        state.extent.height = std::clamp(m_native.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
#if defined(__ANDROID__)
    // Pre-rotation: allocate images in the display's native orientation.
    state.transform = caps.currentTransform;
    if (isQuarterTurn(state.transform)) {
        std::swap(state.extent.width, state.extent.height);
    }
#else
    state.transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
    return state;
}

bool Swapchain::isStale(const Device& device) const {
    if (m_swapchain == VK_NULL_HANDLE) {
        return true;
    }
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physicalDevice(), m_surface, &caps) != VK_SUCCESS) {
        return true;
    }
    const SurfaceState state = desiredState(caps);
    return state.transform != m_transform || state.extent.width != m_extent.width ||
           state.extent.height != m_extent.height;
}

Result<bool> Swapchain::build(const Device& device) {
    VkSurfaceCapabilitiesKHR caps{};
    VD_VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device.physicalDevice(), m_surface, &caps));
    const SurfaceState state = desiredState(caps);
    if (state.extent.width == 0 || state.extent.height == 0) {
        destroy(device);
        return false;
    }

    // Triple buffering is enough for 2 frames in flight; extra images only add latency and memory.
    u32 imageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount > 0) {
        imageCount = std::min(imageCount, caps.maxImageCount);
    }

    VkSwapchainKHR oldSwapchain = m_swapchain;
    destroyViews(device);
#if defined(__ANDROID__)
    if (m_swappy) {
        SwappyVk_destroySwapchain(device.device(), oldSwapchain);
        m_swappy = false;
    }
#endif

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = m_surface;
    info.minImageCount = imageCount;
    info.imageFormat = m_format.format;
    info.imageColorSpace = m_format.colorSpace;
    info.imageExtent = state.extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.preTransform = state.transform;
    info.compositeAlpha = chooseCompositeAlpha(caps.supportedCompositeAlpha);
    info.presentMode = VK_PRESENT_MODE_FIFO_KHR; // always available; Swappy paces on top of it
    info.clipped = VK_TRUE;
    info.oldSwapchain = oldSwapchain;
    const VkResult result = vkCreateSwapchainKHR(device.device(), &info, nullptr, &m_swapchain);
    if (oldSwapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device.device(), oldSwapchain, nullptr);
    }
    if (result != VK_SUCCESS) {
        m_swapchain = VK_NULL_HANDLE;
        return makeError("vkCreateSwapchainKHR", result);
    }
    m_extent = state.extent;
    m_transform = state.transform;

    u32 count = 0;
    VD_VK_TRY(vkGetSwapchainImagesKHR(device.device(), m_swapchain, &count, nullptr));
    m_images.resize(count);
    VD_VK_TRY(vkGetSwapchainImagesKHR(device.device(), m_swapchain, &count, m_images.data()));

    m_views.reserve(count);
    for (VkImage image : m_images) {
        VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = m_format.format;
        viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view = VK_NULL_HANDLE;
        VD_VK_TRY(vkCreateImageView(device.device(), &viewInfo, nullptr, &view));
        m_views.push_back(view);
    }

    VD_LOG_INFO("Swapchain: %ux%u (surface reports %ux%u), %s, %u images, format %d", m_extent.width,
                m_extent.height, caps.currentExtent.width, caps.currentExtent.height, transformName(m_transform),
                count, static_cast<int>(m_format.format));
    setupFramePacing(device);
    return true;
}

void Swapchain::setupFramePacing(const Device& device) {
#if defined(__ANDROID__)
    auto* env = static_cast<JNIEnv*>(m_platform.jniEnv);
    auto activity = static_cast<jobject>(m_platform.activity);
    uint64_t refreshNs = 0;
    if (env && activity &&
        SwappyVk_initAndGetRefreshCycleDuration(env, activity, device.physicalDevice(), device.device(),
                                                m_swapchain, &refreshNs)) {
        SwappyVk_setWindow(device.device(), m_swapchain, static_cast<ANativeWindow*>(m_native.window));
        // Phase 10: the rate is the engine's (60, or 30 from the dynamic quality and the battery saver). Swappy's own
        // auto swap interval dropped to 30 fps after a late frame and stayed there for seconds: at 30 fps the GPU
        // clocks down, its time grows to ~11 ms and CPU + GPU look too close to 16.7 ms to come back (twice in an
        // hour of soak on the Fold5, ~75 frames each).
        SwappyVk_setAutoSwapInterval(false);
        SwappyVk_setSwapIntervalNS(device.device(), m_swapchain,
                                   m_frameInterval > 1 ? SWAPPY_SWAP_30FPS : SWAPPY_SWAP_60FPS);
        m_swappy = true;
        VD_LOG_INFO("Swappy: display refresh %.1f Hz, pacing at %u fps",
                    refreshNs ? 1e9 / static_cast<double>(refreshNs) : 0.0, 60u / m_frameInterval);
    } else {
        VD_LOG_WARN("Swappy unavailable: presenting without frame pacing");
    }
#else
    (void)device;
#endif
}

void Swapchain::setFrameInterval(const Device& device, u32 frames) {
    frames = frames > 1 ? 2u : 1u;
    if (frames == m_frameInterval) {
        return;
    }
    m_frameInterval = frames;
#if defined(__ANDROID__)
    if (m_swappy) {
        SwappyVk_setSwapIntervalNS(device.device(), m_swapchain, frames > 1 ? SWAPPY_SWAP_30FPS : SWAPPY_SWAP_60FPS);
    }
#else
    (void)device;
#endif
    VD_LOG_INFO("Frame pacing: %u fps", 60u / frames);
}

void Swapchain::destroyViews(const Device& device) {
    for (VkImageView view : m_views) {
        vkDestroyImageView(device.device(), view, nullptr);
    }
    m_views.clear();
    m_images.clear();
}

void Swapchain::destroy(const Device& device) {
    destroyViews(device);
    if (m_swapchain != VK_NULL_HANDLE) {
#if defined(__ANDROID__)
        if (m_swappy) {
            SwappyVk_destroySwapchain(device.device(), m_swapchain);
            m_swappy = false;
        }
#endif
        vkDestroySwapchainKHR(device.device(), m_swapchain, nullptr);
        m_swapchain = VK_NULL_HANDLE;
    }
    m_extent = {};
}

VkResult Swapchain::acquire(const Device& device, VkSemaphore signal, u32& imageIndex) const {
    return vkAcquireNextImageKHR(device.device(), m_swapchain, UINT64_MAX, signal, VK_NULL_HANDLE, &imageIndex);
}

VkResult Swapchain::present(const Device& device, VkSemaphore wait, u32 imageIndex) const {
    VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &m_swapchain;
    info.pImageIndices = &imageIndex;
#if defined(__ANDROID__)
    if (m_swappy) {
        return SwappyVk_queuePresent(device.queue(), &info);
    }
#endif
    return vkQueuePresentKHR(device.queue(), &info);
}

VkExtent2D Swapchain::logicalExtent() const {
    return isQuarterTurn(m_transform) ? VkExtent2D{m_extent.height, m_extent.width} : m_extent;
}

void Swapchain::preRotation(f32 out[4]) const {
    // Rotation by the surface transform angle around +Z (matches Android's pre-rotation guide).
    switch (m_transform) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        out[0] = 0.0f, out[1] = 1.0f, out[2] = -1.0f, out[3] = 0.0f;
        break;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        out[0] = -1.0f, out[1] = 0.0f, out[2] = 0.0f, out[3] = -1.0f;
        break;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        out[0] = 0.0f, out[1] = -1.0f, out[2] = 1.0f, out[3] = 0.0f;
        break;
    default:
        out[0] = 1.0f, out[1] = 0.0f, out[2] = 0.0f, out[3] = 1.0f;
        break;
    }
}

} // namespace vd::vk
