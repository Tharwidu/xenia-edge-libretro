/*
 * Thin wrapper around D3D12Presenter for the libretro core.
 * Isolates the D3D12 presenter header dependency from libretro.cpp.
 */
#ifndef LIBRETRO_D3D12_PRESENTER_H
#define LIBRETRO_D3D12_PRESENTER_H

#include <cstdint>

// Forward declare
namespace xe {
namespace ui { class Presenter; }
}

// GPU blit capture: R10G10B10A2 readback + 10bpc???8bpc via D3D12Presenter.
bool libretro_d3d12_capture_gpu_blit(xe::ui::Presenter* presenter,
                                      const void*& data_out,
                                      uint32_t& width_out,
                                      uint32_t& height_out);

#endif // LIBRETRO_D3D12_PRESENTER_H
