/*
 * Thin wrapper around VulkanPresenter for the libretro core.
 * Isolates the Vulkan C++ header dependency from libretro.cpp.
 */
#ifndef LIBRETRO_VK_PRESENTER_H
#define LIBRETRO_VK_PRESENTER_H

#include <cstdint>

// Forward declare
namespace xe {
namespace ui { class Presenter; }
}

// GPU blit capture via VulkanPresenter. is_bgra_out true means the blit landed
// directly in libretro's XRGB8888 byte order and no CPU channel swap is owed.
bool libretro_vk_capture_gpu_blit(xe::ui::Presenter* presenter,
                                   const void*& data_out,
                                   uint32_t& width_out,
                                   uint32_t& height_out,
                                   bool& is_bgra_out);

#endif // LIBRETRO_VK_PRESENTER_H
