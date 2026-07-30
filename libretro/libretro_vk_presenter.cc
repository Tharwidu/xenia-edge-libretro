/*
 * VulkanPresenter wrapper for libretro - isolates Vulkan headers from libretro.cpp
 */

#include "libretro_vk_presenter.h"

#include "xenia/ui/vulkan/vulkan_presenter.h"

bool libretro_vk_capture_gpu_blit(xe::ui::Presenter* presenter,
                                   const void*& data_out,
                                   uint32_t& width_out,
                                   uint32_t& height_out,
                                   bool& is_bgra_out) {
    is_bgra_out = false;
    if (!presenter) return false;
    auto* vk_presenter =
        dynamic_cast<xe::ui::vulkan::VulkanPresenter*>(presenter);
    if (!vk_presenter) return false;
    return vk_presenter->CaptureGuestOutputGPUBlit(data_out, width_out,
                                                    height_out, is_bgra_out);
}
