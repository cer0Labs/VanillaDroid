#pragma once

#include "core/Types.h"
#include "platform/NativeSurface.h"
#include "platform/PlatformInfo.h"
#include "render/vk/VkCommon.h"

#include <vector>

namespace vd::vk {

class Device;

// VkSurfaceKHR + VkSwapchainKHR. Follows the OS window: created and destroyed with it, rebuilt on
// resize, fold/unfold and rotation.
//
// Android specifics:
//  - Pre-rotation: images are allocated in the display's native orientation and the renderer rotates
//    clip space (see preRotation()), so the compositor never spends GPU/DPU time rotating our frames.
//  - Presentation goes through Swappy, which paces frames at exactly 60 fps on the 120 Hz panel.
class Swapchain {
public:
    Result<void> createSurface(const Device& device, const PlatformInfo& platform, const NativeSurface& surface);
    void destroySurface(const Device& device);
    void setNativeSize(u32 width, u32 height);

    // (Re)creates the swapchain for the current surface state. The GPU must be idle.
    // Returns false (not an error) when the surface currently has zero size.
    Result<bool> build(const Device& device);
    void destroy(const Device& device);

    // True when the surface size or transform no longer matches the swapchain.
    bool isStale(const Device& device) const;

    // Phase 10: frames of 60 fps per presented frame (1 = 60 fps, 2 = 30 fps), applied at once and kept across
    // rebuilds. Only Swappy paces it; without it (desktop) it is ignored.
    void setFrameInterval(const Device& device, u32 frames);
    u32 frameInterval() const { return m_frameInterval; }

    VkResult acquire(const Device& device, VkSemaphore signal, u32& imageIndex) const;
    VkResult present(const Device& device, VkSemaphore wait, u32 imageIndex) const;

    bool valid() const { return m_swapchain != VK_NULL_HANDLE; }
    VkFormat format() const { return m_format.format; }
    u32 imageCount() const { return static_cast<u32>(m_views.size()); }
    VkImageView imageView(u32 index) const { return m_views[index]; }

    // Framebuffer size, in the display's native orientation.
    VkExtent2D extent() const { return m_extent; }
    // Size as the player sees it (width/height swapped for 90/270 degree transforms).
    VkExtent2D logicalExtent() const;
    // 2x2 column-major matrix to apply to clip-space XY after all other transforms.
    void preRotation(f32 out[4]) const;

private:
    struct SurfaceState {
        VkExtent2D extent{};
        VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    };
    SurfaceState desiredState(const VkSurfaceCapabilitiesKHR& caps) const;
    void destroyViews(const Device& device);
    void setupFramePacing(const Device& device);

    PlatformInfo m_platform;
    NativeSurface m_native;
    VkSurfaceKHR m_surface = VK_NULL_HANDLE;
    VkSwapchainKHR m_swapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR m_format{};
    VkExtent2D m_extent{};
    VkSurfaceTransformFlagBitsKHR m_transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    std::vector<VkImage> m_images;
    std::vector<VkImageView> m_views;
    bool m_swappy = false;
    u32 m_frameInterval = 1;
};

} // namespace vd::vk
