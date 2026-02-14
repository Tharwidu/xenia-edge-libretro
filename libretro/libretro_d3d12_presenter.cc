/*
 * D3D12Presenter wrapper for libretro - isolates D3D12 headers from libretro.cpp
 */

#include "libretro_d3d12_presenter.h"

#include "xenia/ui/d3d12/d3d12_presenter.h"

bool libretro_d3d12_capture_gpu_blit(xe::ui::Presenter* presenter,
                                      const void*& data_out,
                                      uint32_t& width_out,
                                      uint32_t& height_out) {
    if (!presenter) return false;
    auto* d3d12_presenter =
        dynamic_cast<xe::ui::d3d12::D3D12Presenter*>(presenter);
    if (!d3d12_presenter) return false;
    return d3d12_presenter->CaptureGuestOutputGPUBlit(data_out, width_out,
                                                       height_out);
}
