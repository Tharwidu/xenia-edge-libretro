/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <filesystem>
#include <memory>
#include <thread>

#include "libretro.h"
#include "libretro_vulkan.h"
#ifdef _WIN32
#include "libretro_d3d12.h"
#include <timeapi.h>  // timeBeginPeriod/timeEndPeriod (link: winmm)
#endif
#include "libretro_core_options.h"

// Xenia headers
#include "xenia/xbox.h"
#include "xenia/config.h"
#include "xenia/emulator.h"
#include "xenia/memory.h"
#include "xenia/base/clock.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#ifdef _WIN32
#include "xenia/base/main_win.h"
#endif
#include "xenia/apu/apu_flags.h"
#include "xenia/gpu/gpu_flags.h"
#include "xenia/gpu/graphics_system.h"

#include "xenia/base/filesystem.h"

// CVars declared in .cc files - DECLARE for direct assignment.
DECLARE_bool(use_50Hz_mode);
DECLARE_bool(apply_title_update);
DECLARE_string(xma_decoder);
DECLARE_int32(log_level);

// New cvars for expanded core options
DECLARE_int32(draw_resolution_scale_x);
DECLARE_int32(draw_resolution_scale_y);
DECLARE_uint32(framerate_limit);
DECLARE_string(readback_resolve);
DECLARE_bool(store_shaders);
DECLARE_bool(half_pixel_offset);
DECLARE_bool(gpu_allow_invalid_fetch_constants);
DECLARE_bool(use_fuzzy_alpha_epsilon);
DECLARE_uint32(internal_display_resolution);
DECLARE_bool(widescreen);
DECLARE_int32(video_standard);
DECLARE_uint32(kernel_display_gamma_type);
DECLARE_bool(use_dedicated_xma_thread);
DECLARE_bool(enable_xmp);
DECLARE_int32(xmp_default_volume);
DECLARE_bool(apply_patches);
DECLARE_int32(license_mask);
DECLARE_int32(headless_messagebox_button);
DECLARE_int32(user_language);
DECLARE_int32(user_country);
DECLARE_bool(protect_zero);
DECLARE_bool(clear_memory_page_state);
DECLARE_bool(disable_context_promotion);
#ifdef _WIN32
#include "xenia/gpu/d3d12/d3d12_graphics_system.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#endif
#include "xenia/gpu/vulkan/vulkan_graphics_system.h"
#include "xenia/ui/presenter.h"
#include "libretro_vk_presenter.h"
#ifdef _WIN32
#include "libretro_d3d12_presenter.h"
#endif
#include "xenia/vfs/virtual_file_system.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/profile_manager.h"

// Single-header MPEG-1 decoder for the optional boot splash video.
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"
#include "libretro_audio_driver.h"
#include "libretro_hid.h"

// CVars from xenia_main.cc - libretro core replaces main entry point.
DEFINE_string(apu, "libretro", "Audio system.", "APU");
DEFINE_string(gpu, "d3d12", "Graphics system.", "GPU");
DEFINE_string(hid, "nop", "Input system.", "HID");

DEFINE_path(storage_root, "", "Root path for persistent internal data storage.",
            "Storage");
DEFINE_path(content_root, "", "Root path for guest content storage.",
            "Storage");
DEFINE_path(cache_root, "", "Root path for cache files.", "Storage");

DEFINE_bool(mount_scratch, false, "Enable scratch mount", "Storage");
DEFINE_bool(mount_cache, true, "Enable cache mount", "Storage");

// win32_high_resolution_timer / win32_mmcss are now defined upstream in
// base/main_win.cc, which the libretro build links.

DEFINE_transient_bool(portable, false, "Portable mode.", "General");
DEFINE_bool(discord, false, "Enable Discord rich presence", "General");

/* ================================================================== */
/*  Core state                                                         */
/* ================================================================== */
struct xenia_core_state {
    retro_video_refresh_t      video_cb;
    retro_audio_sample_t       audio_cb;
    retro_audio_sample_batch_t audio_batch_cb;
    retro_input_poll_t         input_poll_cb;
    retro_input_state_t        input_state_cb;
    retro_environment_t        environ_cb;

    int16_t *audio_buffer;
    size_t   audio_buffer_size;
    double   audio_sample_rate;

    char game_path[4096];
    char system_dir[4096];
    char save_dir[4096];

    char graphics_backend[32];
    bool vsync_enabled;
    bool audio_enabled;
    bool pal_mode;

    retro_log_printf_t log_cb;
};

static struct xenia_core_state core_state;
static std::unique_ptr<xe::Emulator> xenia_emulator;
static xe::apu::libretro::LibretroAudioRingBuffer *audio_ring = nullptr;
static xe::hid::libretro_hid::LibretroInputDriver *lr_input_driver = nullptr;
static xe::gpu::GraphicsSystem *lr_graphics = nullptr;
static bool game_loaded = false;

// Software frame capture buffer (fallback path)
static xe::ui::RawImage captured_frame;
// Reusable XRGB8888 buffer for the software present path so no allocation
// happens per frame.
static std::vector<uint32_t> sw_frame_buf;

/* ================================================================== */
/*  Vulkan HW render state                                             */
/* ================================================================== */
#define VK_MAX_SYNC 8

// Frontend resources (frontend VkDevice from retro_hw_render_interface_vulkan)
struct VulkanFrameResources {
    VkImage image;
    VkDeviceMemory image_memory;
    VkImageView image_view;
    VkBuffer staging_buffer;
    VkDeviceMemory staging_memory;
    void *staging_mapped;
    VkCommandPool cmd_pool;
    VkCommandBuffer cmd;
    uint32_t width;
    uint32_t height;
};

static const struct retro_hw_render_interface_vulkan *vk_hw = nullptr;
static struct retro_hw_render_callback hw_render_cb;
static VulkanFrameResources vk_frames[VK_MAX_SYNC] = {};
static struct retro_vulkan_image vk_current_image = {};
static bool vulkan_hw_render_active = false;

#ifdef _WIN32
/* ================================================================== */
/*  D3D12 HW render state                                              */
/* ================================================================== */
struct D3D12FrameResources {
    ID3D12Resource *texture;
    ID3D12Resource *upload_buffer;
    void *upload_mapped;
    ID3D12CommandAllocator *cmd_alloc;
    ID3D12GraphicsCommandList *cmd_list;
    ID3D12Fence *fence;
    HANDLE fence_event;
    UINT64 fence_value;
    uint32_t width;
    uint32_t height;
};

static const struct retro_hw_render_interface_d3d12 *d3d12_hw = nullptr;
static constexpr int D3D12_NUM_FRAMES = 2;
static D3D12FrameResources d3d12_frames[D3D12_NUM_FRAMES] = {};
static int d3d12_frame_idx = 0;
static bool d3d12_hw_render_active = false;
#else
static bool d3d12_hw_render_active = false;
#endif

/* ================================================================== */
/*  Logging                                                            */
/* ================================================================== */
static void xenia_log(enum retro_log_level level, const char *fmt, ...) {
    if (!core_state.log_cb) return;
    va_list va;
    va_start(va, fmt);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    core_state.log_cb(level, "[Xenia] %s", buf);
}

/* ================================================================== */
/*  Vulkan HW render helpers                                           */
/* ================================================================== */
static uint32_t vk_find_memory_type(VkPhysicalDevice gpu,
                                     uint32_t type_bits,
                                     VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(gpu, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) &&
            (props.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    return UINT32_MAX;
}

/* ------------------------------------------------------------------ */
/*  Frontend-side Vulkan frame resources                               */
/* ------------------------------------------------------------------ */
static void vk_destroy_frame(VulkanFrameResources &f) {
    if (!vk_hw) return;
    VkDevice dev = vk_hw->device;
    if (f.image_view)      { vkDestroyImageView(dev, f.image_view, nullptr);   f.image_view = VK_NULL_HANDLE; }
    if (f.image)           { vkDestroyImage(dev, f.image, nullptr);            f.image = VK_NULL_HANDLE; }
    if (f.image_memory)    { vkFreeMemory(dev, f.image_memory, nullptr);       f.image_memory = VK_NULL_HANDLE; }
    if (f.staging_mapped && f.staging_memory) {
        vkUnmapMemory(dev, f.staging_memory);
        f.staging_mapped = nullptr;
    }
    if (f.staging_buffer)  { vkDestroyBuffer(dev, f.staging_buffer, nullptr);  f.staging_buffer = VK_NULL_HANDLE; }
    if (f.staging_memory)  { vkFreeMemory(dev, f.staging_memory, nullptr);     f.staging_memory = VK_NULL_HANDLE; }
    if (f.cmd_pool)        { vkDestroyCommandPool(dev, f.cmd_pool, nullptr);   f.cmd_pool = VK_NULL_HANDLE; }
    f.cmd = VK_NULL_HANDLE;
    f.width = f.height = 0;
}

static bool vk_create_frame(VulkanFrameResources &f,
                             uint32_t w, uint32_t h) {
    VkDevice dev = vk_hw->device;
    VkResult res;

    // Command pool
    VkCommandPoolCreateInfo pool_info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = vk_hw->queue_index;
    res = vkCreateCommandPool(dev, &pool_info, nullptr, &f.cmd_pool);
    if (res != VK_SUCCESS) return false;

    VkCommandBufferAllocateInfo alloc_info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc_info.commandPool = f.cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    res = vkAllocateCommandBuffers(dev, &alloc_info, &f.cmd);
    if (res != VK_SUCCESS) return false;

    // Image: TILING_OPTIMAL, SAMPLED|TRANSFER_DST|TRANSFER_SRC, MUTABLE_FORMAT
    VkImageCreateInfo img_info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    img_info.flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    img_info.imageType = VK_IMAGE_TYPE_2D;
    img_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    img_info.extent = {w, h, 1};
    img_info.mipLevels = 1;
    img_info.arrayLayers = 1;
    img_info.samples = VK_SAMPLE_COUNT_1_BIT;
    img_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    img_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    img_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    img_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    res = vkCreateImage(dev, &img_info, nullptr, &f.image);
    if (res != VK_SUCCESS) return false;

    VkMemoryRequirements img_reqs;
    vkGetImageMemoryRequirements(dev, f.image, &img_reqs);
    VkMemoryAllocateInfo img_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    img_alloc.allocationSize = img_reqs.size;
    img_alloc.memoryTypeIndex = vk_find_memory_type(
        vk_hw->gpu, img_reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (img_alloc.memoryTypeIndex == UINT32_MAX) return false;
    res = vkAllocateMemory(dev, &img_alloc, nullptr, &f.image_memory);
    if (res != VK_SUCCESS) return false;
    vkBindImageMemory(dev, f.image, f.image_memory, 0);

    // Image view
    VkImageViewCreateInfo view_info = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = f.image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY,
                            VK_COMPONENT_SWIZZLE_IDENTITY};
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    res = vkCreateImageView(dev, &view_info, nullptr, &f.image_view);
    if (res != VK_SUCCESS) return false;

    // Staging buffer: host-visible, persistently mapped
    VkDeviceSize buf_size = (VkDeviceSize)w * h * 4;
    VkBufferCreateInfo buf_info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    buf_info.size = buf_size;
    buf_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    res = vkCreateBuffer(dev, &buf_info, nullptr, &f.staging_buffer);
    if (res != VK_SUCCESS) return false;

    VkMemoryRequirements buf_reqs;
    vkGetBufferMemoryRequirements(dev, f.staging_buffer, &buf_reqs);
    VkMemoryAllocateInfo buf_alloc = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    buf_alloc.allocationSize = buf_reqs.size;
    buf_alloc.memoryTypeIndex = vk_find_memory_type(
        vk_hw->gpu, buf_reqs.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (buf_alloc.memoryTypeIndex == UINT32_MAX) return false;
    res = vkAllocateMemory(dev, &buf_alloc, nullptr, &f.staging_memory);
    if (res != VK_SUCCESS) return false;
    vkBindBufferMemory(dev, f.staging_buffer, f.staging_memory, 0);
    vkMapMemory(dev, f.staging_memory, 0, VK_WHOLE_SIZE, 0, &f.staging_mapped);

    f.width = w;
    f.height = h;
    return true;
}

static void vk_destroy_all_frames(void) {
    for (int i = 0; i < VK_MAX_SYNC; i++)
        vk_destroy_frame(vk_frames[i]);
}

static void vulkan_context_reset(void) {
    const struct retro_hw_render_interface *iface = nullptr;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) ||
        !iface ||
        iface->interface_type != RETRO_HW_RENDER_INTERFACE_VULKAN ||
        iface->interface_version < RETRO_HW_RENDER_INTERFACE_VULKAN_VERSION) {
        xenia_log(RETRO_LOG_ERROR,
                  "Failed to get Vulkan HW render interface (type=%d ver=%u)\n",
                  iface ? iface->interface_type : -1,
                  iface ? iface->interface_version : 0);
        vulkan_hw_render_active = false;
        return;
    }
    vk_hw = (const struct retro_hw_render_interface_vulkan *)iface;
    vulkan_hw_render_active = true;
    xenia_log(RETRO_LOG_INFO,
              "Vulkan HW render interface acquired (ver %u, device %p)\n",
              vk_hw->interface_version, (void *)vk_hw->device);
}

static void vulkan_context_destroy(void) {
    if (vk_hw) {
        vkDeviceWaitIdle(vk_hw->device);
        vk_destroy_all_frames();
    }
    vk_hw = nullptr;
    vulkan_hw_render_active = false;
    memset(&vk_current_image, 0, sizeof(vk_current_image));
}

#ifdef _WIN32
/* ================================================================== */
/*  D3D12 HW render helpers                                            */
/* ================================================================== */
static void d3d12_destroy_frame(D3D12FrameResources &f) {
    if (f.upload_mapped && f.upload_buffer) {
        f.upload_buffer->Unmap(0, nullptr);
        f.upload_mapped = nullptr;
    }
    if (f.cmd_list)   { f.cmd_list->Release();   f.cmd_list = nullptr; }
    if (f.cmd_alloc)  { f.cmd_alloc->Release();  f.cmd_alloc = nullptr; }
    if (f.fence)      { f.fence->Release();      f.fence = nullptr; }
    if (f.fence_event){ CloseHandle(f.fence_event); f.fence_event = nullptr; }
    if (f.upload_buffer) { f.upload_buffer->Release(); f.upload_buffer = nullptr; }
    if (f.texture)    { f.texture->Release();    f.texture = nullptr; }
    f.fence_value = 0;
    f.width = f.height = 0;
}

static bool d3d12_create_frame(D3D12FrameResources &f,
                                ID3D12Device *device,
                                uint32_t w, uint32_t h) {
    HRESULT hr;

    // Command allocator + list
    hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&f.cmd_alloc));
    if (FAILED(hr)) return false;

    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        f.cmd_alloc, nullptr,
        IID_PPV_ARGS(&f.cmd_list));
    if (FAILED(hr)) return false;
    f.cmd_list->Close();

    // Fence
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f.fence));
    if (FAILED(hr)) return false;
    f.fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    f.fence_value = 0;

    // Texture: DEFAULT heap, COPY_DEST ??? COPY_SOURCE
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = w;
    tex_desc.Height = h;
    tex_desc.DepthOrArraySize = 1;
    tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    hr = device->CreateCommittedResource(
        &default_heap, D3D12_HEAP_FLAG_NONE,
        &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, IID_PPV_ARGS(&f.texture));
    if (FAILED(hr)) return false;

    // Upload buffer (row-pitch aligned)
    UINT64 upload_size = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, nullptr, nullptr, &upload_size);

    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = upload_size;
    buf_desc.Height = 1;
    buf_desc.DepthOrArraySize = 1;
    buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1;
    buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    hr = device->CreateCommittedResource(
        &upload_heap, D3D12_HEAP_FLAG_NONE,
        &buf_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr, IID_PPV_ARGS(&f.upload_buffer));
    if (FAILED(hr)) return false;

    // Persistently map
    D3D12_RANGE read_range = {0, 0};
    hr = f.upload_buffer->Map(0, &read_range, &f.upload_mapped);
    if (FAILED(hr)) return false;

    f.width = w;
    f.height = h;
    return true;
}

static void d3d12_context_reset(void) {
    const struct retro_hw_render_interface *iface = nullptr;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE, &iface) ||
        !iface ||
        iface->interface_type != RETRO_HW_RENDER_INTERFACE_D3D12 ||
        iface->interface_version < RETRO_HW_RENDER_INTERFACE_D3D12_VERSION) {
        xenia_log(RETRO_LOG_ERROR,
                  "Failed to get D3D12 HW render interface (type=%d ver=%u)\n",
                  iface ? iface->interface_type : -1,
                  iface ? iface->interface_version : 0);
        d3d12_hw_render_active = false;
        return;
    }
    d3d12_hw = (const struct retro_hw_render_interface_d3d12 *)iface;
    d3d12_hw_render_active = true;
    xenia_log(RETRO_LOG_INFO,
              "D3D12 HW render interface acquired (ver %u, device %p)\n",
              d3d12_hw->interface_version, (void *)d3d12_hw->device);
}

static void d3d12_destroy_all_frames(void) {
    for (int i = 0; i < D3D12_NUM_FRAMES; i++)
        d3d12_destroy_frame(d3d12_frames[i]);
    d3d12_frame_idx = 0;
}

static void d3d12_context_destroy(void) {
    d3d12_destroy_all_frames();
    d3d12_hw = nullptr;
    d3d12_hw_render_active = false;
}
#endif /* _WIN32 */

/* ================================================================== */
/*  Core options helper (operates on xenia_core_state)                 */
/* ================================================================== */
static const char *opt_get(const char *key) {
    struct retro_variable var = {key, nullptr};
    if (core_state.environ_cb &&
        core_state.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
        return var.value;
    return nullptr;
}

#ifdef _WIN32
// The D3D12 backend needs the Agility runtime (D3D12Core.dll) shipped in a
// D3D12/ folder next to the frontend executable. Detect it so we can prefer
// D3D12 (faster on demanding titles) when it's present and fall back to the
// self-contained Vulkan backend when it isn't, instead of failing to init.
static bool d3d12_runtime_available() {
    std::error_code ec;
    auto core_dll = xe::filesystem::GetExecutablePath().parent_path() /
                    "D3D12" / "D3D12Core.dll";
    return std::filesystem::exists(core_dll, ec);
}

// True if the primary GPU is AMD. Xenia's D3D12 backend currently fails on
// AMD (the same breakage standalone xenia has), so AMD users default to the
// Vulkan backend instead; they can force d3d12 via the core option if it is
// ever fixed. Uses DXGI directly (already linked) so no device is created.
static bool primary_gpu_is_amd() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return false;
    }
    bool is_amd = false;
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc)) &&
            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
            is_amd = (desc.VendorId == 0x1002);  // AMD/ATI PCI vendor ID
            adapter->Release();
            break;  // first hardware adapter is the primary
        }
        adapter->Release();
    }
    factory->Release();
    return is_amd;
}
#endif

static void apply_core_options(void) {
    const char *v;

    // =================================================================
    // Graphics
    // =================================================================

    // Render target path
    if ((v = opt_get(XENIA_OPT_RENDER_TARGET_PATH))) {
        cvars::render_target_path = v;
    }

    // Draw resolution scale (uniform X+Y, restart required)
    if ((v = opt_get(XENIA_OPT_DRAW_RESOLUTION_SCALE))) {
        int scale = atoi(v);
        if (scale >= 1 && scale <= 8) {
            cvars::draw_resolution_scale_x = scale;
            cvars::draw_resolution_scale_y = scale;
        }
    }

    // Anisotropic filtering override
    if ((v = opt_get(XENIA_OPT_ANISOTROPIC_FILTERING))) {
        cvars::anisotropic_override = atoi(v);
    }

    // Async shader compilation
    if ((v = opt_get(XENIA_OPT_ASYNC_SHADERS))) {
        cvars::async_shader_compilation = (strcmp(v, "enabled") == 0);
    }

    // Readback resolve
    if ((v = opt_get(XENIA_OPT_READBACK_RESOLVE))) {
        cvars::readback_resolve = v;
    }

    // Store shaders
    if ((v = opt_get(XENIA_OPT_STORE_SHADERS))) {
        cvars::store_shaders = (strcmp(v, "enabled") == 0);
    }

    // Half-pixel offset
    if ((v = opt_get(XENIA_OPT_HALF_PIXEL_OFFSET))) {
        cvars::half_pixel_offset = (strcmp(v, "enabled") == 0);
    }

    // GPU allow invalid fetch constants
    if ((v = opt_get(XENIA_OPT_GPU_INVALID_FETCH))) {
        cvars::gpu_allow_invalid_fetch_constants = (strcmp(v, "enabled") == 0);
    }

    // Fuzzy alpha epsilon (NVIDIA fix)
    if ((v = opt_get(XENIA_OPT_FUZZY_ALPHA_EPSILON))) {
        cvars::use_fuzzy_alpha_epsilon = (strcmp(v, "enabled") == 0);
    }

    // VSync / guest frame limiter
    if ((v = opt_get(XENIA_OPT_VSYNC))) {
        core_state.vsync_enabled = (strcmp(v, "enabled") == 0);
        SetGuestDisplayRefreshCap(core_state.vsync_enabled);
    }

    // Host framerate limit
    if ((v = opt_get(XENIA_OPT_FRAMERATE_LIMIT))) {
        uint64_t limit = (uint64_t)atoi(v);
        cvars::framerate_limit = limit;
    }

    // PAL 50Hz mode
    if ((v = opt_get(XENIA_OPT_50HZ_MODE))) {
        core_state.pal_mode = (strcmp(v, "enabled") == 0);
        cvars::use_50Hz_mode = core_state.pal_mode;
    }

    // =================================================================
    // Video
    // =================================================================

    // Internal display resolution (restart required)
    if ((v = opt_get(XENIA_OPT_INTERNAL_DISPLAY_RES))) {
        cvars::internal_display_resolution = (uint32_t)atoi(v);
    }

    // Widescreen
    if ((v = opt_get(XENIA_OPT_WIDESCREEN))) {
        cvars::widescreen = (strcmp(v, "enabled") == 0);
    }

    // Video standard (1=NTSC, 2=NTSC-J, 3=PAL)
    if ((v = opt_get(XENIA_OPT_VIDEO_STANDARD))) {
        cvars::video_standard = atoi(v);
    }

    // Display gamma type (0=linear, 1=sRGB, 2=BT.709)
    if ((v = opt_get(XENIA_OPT_DISPLAY_GAMMA))) {
        cvars::kernel_display_gamma_type = (uint32_t)atoi(v);
    }

    // =================================================================
    // Audio
    // =================================================================

    // Audio enabled (controls update_audio)
    if ((v = opt_get(XENIA_OPT_AUDIO_ENABLED)))
        core_state.audio_enabled = (strcmp(v, "enabled") == 0);

    // Mute (session-only volume; upstream removed the mute cvar)
    if ((v = opt_get(XENIA_OPT_MUTE))) {
        xe::apu::SetVolume((strcmp(v, "enabled") == 0) ? 0 : 100);
    }

    // XMA decoder (restart required)
    if ((v = opt_get(XENIA_OPT_XMA_DECODER))) {
        cvars::xma_decoder = v;
    }

    // Dedicated XMA thread
    if ((v = opt_get(XENIA_OPT_DEDICATED_XMA_THREAD))) {
        cvars::use_dedicated_xma_thread = (strcmp(v, "enabled") == 0);
    }

    // Enable XMP (music player)
    if ((v = opt_get(XENIA_OPT_ENABLE_XMP))) {
        cvars::enable_xmp = (strcmp(v, "enabled") == 0);
    }

    // XMP default volume
    if ((v = opt_get(XENIA_OPT_XMP_DEFAULT_VOLUME))) {
        cvars::xmp_default_volume = atoi(v);
    }

    // =================================================================
    // Emulation
    // =================================================================

    // Time scalar
    if ((v = opt_get(XENIA_OPT_TIME_SCALAR))) {
        double scalar = atof(v);
        if (scalar > 0.0) {
            xe::Clock::set_guest_time_scalar(scalar);
        }
    }

    // Title updates
    if ((v = opt_get(XENIA_OPT_TITLE_UPDATES))) {
        cvars::apply_title_update = (strcmp(v, "enabled") == 0);
    }

    // Apply game patches
    if ((v = opt_get(XENIA_OPT_APPLY_PATCHES))) {
        cvars::apply_patches = (strcmp(v, "enabled") == 0);
    }

    // License mask (0=None, 1=Full, -1=All)
    if ((v = opt_get(XENIA_OPT_LICENSE_MASK))) {
        cvars::license_mask = atoi(v);
    }

    // Headless message-box response (which button to auto-pick; -1 = default)
    if ((v = opt_get(XENIA_OPT_MSGBOX_BUTTON))) {
        cvars::headless_messagebox_button = atoi(v);
    }

    // User language (upstream now uses numeric XConfig language IDs)
    if ((v = opt_get(XENIA_OPT_USER_LANGUAGE))) {
        struct { const char* name; int id; } langs[] = {
            {"English", 1},  {"Japanese", 2},   {"German", 3},
            {"French", 4},   {"Spanish", 5},    {"Italian", 6},
            {"Korean", 7},   {"TChinese", 8},   {"Portuguese", 9},
            {"Polish", 11},  {"Russian", 12},   {"SChinese", 17},
        };
        for (auto& l : langs) {
            if (strcmp(v, l.name) == 0) { cvars::user_language = l.id; break; }
        }
    }

    // User country (upstream now uses numeric XConfig country IDs)
    if ((v = opt_get(XENIA_OPT_USER_COUNTRY))) {
        struct { const char* name; int id; } countries[] = {
            {"United States", 103}, {"Great Britain", 35}, {"Japan", 53},
            {"Germany", 24},        {"France", 34},        {"Spain", 31},
            {"Italy", 50},          {"Australia", 6},      {"Canada", 16},
        };
        for (auto& c : countries) {
            if (strcmp(v, c.name) == 0) { cvars::user_country = c.id; break; }
        }
    }

    // =================================================================
    // Compatibility
    // =================================================================

    // Protect zero page
    if ((v = opt_get(XENIA_OPT_PROTECT_ZERO))) {
        cvars::protect_zero = (strcmp(v, "enabled") == 0);
    }

    // Clear GPU memory page state
    if ((v = opt_get(XENIA_OPT_CLEAR_MEMORY_PAGE))) {
        cvars::clear_memory_page_state = (strcmp(v, "enabled") == 0);
    }

    // Disable context promotion
    if ((v = opt_get(XENIA_OPT_DISABLE_CTX_PROMOTION))) {
        cvars::disable_context_promotion = (strcmp(v, "enabled") == 0);
    }

    // Mount cache partition
    if ((v = opt_get(XENIA_OPT_MOUNT_CACHE))) {
        cvars::mount_cache = (strcmp(v, "enabled") == 0);
    }

    // Mount scratch partition
    if ((v = opt_get(XENIA_OPT_MOUNT_SCRATCH))) {
        cvars::mount_scratch = (strcmp(v, "enabled") == 0);
    }

    // =================================================================
    // Debug
    // =================================================================

    // Log level (0=error, 1=warning, 2=info, 3=debug)
    if ((v = opt_get(XENIA_OPT_LOG_LEVEL))) {
        int level = 2;
        if (strcmp(v, "error") == 0) level = 0;
        else if (strcmp(v, "warn") == 0) level = 1;
        else if (strcmp(v, "info") == 0) level = 2;
        else if (strcmp(v, "debug") == 0) level = 3;
        cvars::log_level = level;
    }
}

/* ================================================================== */
/*  Per-frame helpers                                                  */
/* ================================================================== */
static void update_audio(void) {
    if (!core_state.audio_enabled || !core_state.audio_buffer) return;
    if (!audio_ring || !core_state.audio_batch_cb) return;

    // Drain audio, capped at 2?? real-time (3200 samples max for 48kHz@60fps).
    constexpr size_t kChunkSamples = 1600;
    constexpr size_t kMaxDrain     = 3200;  // 2?? real-time
    size_t total = 0;

    while (total < kMaxDrain) {
        size_t want = kMaxDrain - total;
        if (want > kChunkSamples) want = kChunkSamples;
        size_t got = audio_ring->Pop(core_state.audio_buffer, want);
        if (got == 0) break;
        core_state.audio_batch_cb(core_state.audio_buffer, got / 2);
        total += got;
    }
}

// ---------------------------------------------------------------------------
// Optional boot splash: a user-supplied MPEG-1 file played through the
// software frame path while the game keeps booting underneath. Xenia has no
// console boot flow of its own (high-level emulation), so this is purely a
// cosmetic simulation. File: <system>/xenia/bootanim.mpg
// ---------------------------------------------------------------------------
static plm_t* splash_plm = nullptr;
static std::vector<uint8_t> splash_frame_buf;
static int splash_w = 0, splash_h = 0;
static bool splash_audio_ok = false;
static std::chrono::steady_clock::time_point splash_next_tick;
static std::chrono::steady_clock::time_point splash_t0;
static bool splash_pacing_init = false;
static bool splash_audio_done = false;
static double splash_video_time = -1.0;  // PTS of the frame currently shown
// Presentation clock in seconds: fed-audio duration while audio plays (the
// frontend's audio sync turns that into real time), wall-tick advanced after
// the audio ends or when there is none.
static double splash_clock = 0.0;
static int64_t splash_samples_fed = 0;
static double splash_sample_accum = 0.0;
// Decoded-PCM carry between runs (an MP2 frame rarely divides evenly into
// the per-run sample count).
static std::vector<int16_t> splash_pcm_carry;
static size_t splash_pcm_carry_pos = 0;
// Diagnostics reported when the splash ends, to tell buffer-full pacing
// (blocked writes: normal) from starvation (long tick gaps: the crackle).
static int splash_blocked_writes = 0;
static double splash_max_tick_gap = 0.0;
static std::chrono::steady_clock::time_point splash_last_tick;
// "Before Boot" splash mode: the game launch is deferred until the video
// ends (or is skipped), then performed from retro_run.
static bool launch_pending = false;
static retro_set_rumble_state_t pending_rumble_cb = nullptr;
static bool xenia_setup_and_launch(const char *path);

static void splash_stop(void) {
    if (splash_plm) {
        plm_destroy(splash_plm);
        splash_plm = nullptr;
        xenia_log(RETRO_LOG_INFO,
                  "Boot splash ended: %d blocked audio writes, max tick gap "
                  "%.1f ms\n",
                  splash_blocked_writes, splash_max_tick_gap * 1000.0);
    }
    splash_frame_buf.clear();
    splash_frame_buf.shrink_to_fit();
}

static void splash_try_start(void) {
    const char* v = opt_get(XENIA_OPT_BOOT_SPLASH);
    if (v && strcmp(v, "disabled") == 0) return;

    std::filesystem::path p =
        std::filesystem::path(core_state.system_dir) / "xenia" /
        "bootanim.mpg";
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return;

    splash_plm = plm_create_with_filename(p.string().c_str());
    if (!splash_plm) return;
    if (!plm_probe(splash_plm, 5000 * 1024)) {
        xenia_log(RETRO_LOG_WARN, "bootanim.mpg is not valid MPEG-1 PS\n");
        splash_stop();
        return;
    }
    plm_set_loop(splash_plm, 0);

    splash_w = plm_get_width(splash_plm);
    splash_h = plm_get_height(splash_plm);
    double fps = plm_get_framerate(splash_plm);
    if (splash_w < 16 || splash_h < 16 || splash_w > 3840 ||
        splash_h > 2160 || fps <= 0) {
        xenia_log(RETRO_LOG_WARN, "bootanim.mpg has unusable dimensions\n");
        splash_stop();
        return;
    }

    // Audio only when the stream matches the core's output rate; resampling
    // is not worth the complexity for a boot animation.
    splash_audio_ok =
        plm_get_num_audio_streams(splash_plm) > 0 &&
        plm_get_samplerate(splash_plm) == (int)core_state.audio_sample_rate;
    plm_set_audio_enabled(splash_plm, splash_audio_ok ? 1 : 0);
    if (!splash_audio_ok) {
        xenia_log(RETRO_LOG_INFO,
                  "Boot splash audio disabled (need %u Hz MP2; convert with "
                  "-ar %u)\n",
                  core_state.audio_sample_rate, core_state.audio_sample_rate);
    }

    splash_frame_buf.resize((size_t)splash_w * splash_h * 4);
    splash_pacing_init = false;
    xenia_log(RETRO_LOG_INFO, "Playing boot splash %dx%d @ %.2f fps\n",
              splash_w, splash_h, fps);
}

// Runs one splash frame. Returns false when the splash is not active (caller
// proceeds with normal video/audio).
static bool splash_run_frame(void) {
    if (!splash_plm) return false;

    // Start skips the splash.
    if (core_state.input_state_cb &&
        core_state.input_state_cb(0, RETRO_DEVICE_JOYPAD, 0,
                                  RETRO_DEVICE_ID_JOYPAD_START)) {
        splash_stop();
        return false;
    }

    // The splash behaves like a standard libretro core: every retro_run
    // outputs one video frame plus exactly one frame's worth of audio, and
    // the FRONTEND does all pacing (audio_sync blocks the write while its
    // buffer is full - the same mechanism that plays in-game audio cleanly).
    // No wall-clock scheduling of audio at all; the wall clock is only a
    // runaway guard for frontends that don't pace, and the tick sleep only
    // paces audio-less splashes. Video follows the fed-audio clock.
    using clock = std::chrono::steady_clock;
    if (!splash_pacing_init) {
        splash_pacing_init = true;
        splash_t0 = clock::now();
        splash_next_tick = splash_t0;
        splash_last_tick = splash_t0;
        splash_clock = 0.0;
        splash_samples_fed = 0;
        splash_sample_accum = 0.0;
        splash_pcm_carry.clear();
        splash_pcm_carry_pos = 0;
        splash_audio_done = !splash_audio_ok;
        splash_video_time = -1.0;
        splash_blocked_writes = 0;
        splash_max_tick_gap = 0.0;
    }
    {
        auto now = clock::now();
        double gap =
            std::chrono::duration<double>(now - splash_last_tick).count();
        if (gap > splash_max_tick_gap) splash_max_tick_gap = gap;
        splash_last_tick = now;
    }

    const double av_fps = core_state.pal_mode ? 50.0 : 60.0;
    double elapsed =
        std::chrono::duration<double>(clock::now() - splash_t0).count();

    bool paced_by_audio = false;
    if (!splash_audio_done && splash_clock <= elapsed + 0.1) {
        // Assemble this run's chunk (sample_rate / fps, fraction carried).
        splash_sample_accum += core_state.audio_sample_rate / av_fps;
        size_t need = (size_t)splash_sample_accum;  // stereo frames
        splash_sample_accum -= (double)need;
        static std::vector<int16_t> chunk;
        chunk.clear();
        while (need) {
            if (splash_pcm_carry_pos >= splash_pcm_carry.size()) {
                plm_samples_t* s = plm_decode_audio(splash_plm);
                if (!s) {
                    splash_audio_done = true;
                    break;
                }
                splash_pcm_carry.resize(s->count * 2);
                for (unsigned i = 0; i < s->count * 2; i++) {
                    float f = s->interleaved[i] * 32767.0f;
                    if (f > 32767.0f) f = 32767.0f;
                    if (f < -32768.0f) f = -32768.0f;
                    splash_pcm_carry[i] = (int16_t)f;
                }
                splash_pcm_carry_pos = 0;
            }
            size_t have = (splash_pcm_carry.size() - splash_pcm_carry_pos) / 2;
            size_t take = have < need ? have : need;
            chunk.insert(chunk.end(),
                         splash_pcm_carry.begin() + splash_pcm_carry_pos,
                         splash_pcm_carry.begin() + splash_pcm_carry_pos +
                             take * 2);
            splash_pcm_carry_pos += take * 2;
            need -= take;
        }
        if (!chunk.empty()) {
            auto write_start = clock::now();
            if (core_state.audio_batch_cb)
                core_state.audio_batch_cb(chunk.data(), chunk.size() / 2);
            if (std::chrono::duration<double>(clock::now() - write_start)
                    .count() > 0.004) {
                ++splash_blocked_writes;  // buffer-full pacing; expected.
            }
            splash_samples_fed += (int64_t)(chunk.size() / 2);
            splash_clock =
                (double)splash_samples_fed / core_state.audio_sample_rate;
            paced_by_audio = true;
        }
    }

    if (!paced_by_audio) {
        // No audio delivered this run (none in the stream, it ended, or the
        // runaway guard tripped): pace one tick ourselves.
        auto now = clock::now();
        if (now < splash_next_tick) {
            std::this_thread::sleep_until(splash_next_tick);
        }
        splash_next_tick += std::chrono::nanoseconds(
            core_state.pal_mode ? 20000000 : 16666667);
        if (splash_next_tick < clock::now() - std::chrono::milliseconds(200))
            splash_next_tick = clock::now();
        if (splash_audio_done) {
            splash_clock += 1.0 / av_fps;
        }
    } else {
        // Keep the fallback tick anchored so it doesn't burst when audio ends.
        splash_next_tick = clock::now();
    }

    // Decode video up to the presentation clock (typically 0 or 1 frames per
    // run; late frames are caught up by decoding through them).
    while (splash_video_time < splash_clock) {
        plm_frame_t* frame = plm_decode_video(splash_plm);
        if (!frame) {
            splash_stop();
            return false;
        }
        splash_video_time = frame->time;
        plm_frame_to_bgra(frame, splash_frame_buf.data(), splash_w * 4);
    }

    // Discard the booting game's audio so it doesn't burst in afterwards.
    if (audio_ring && core_state.audio_buffer) {
        for (int i = 0; i < 64; i++) {
            if (!audio_ring->Pop(core_state.audio_buffer, 1600)) break;
        }
    }

    core_state.video_cb(splash_frame_buf.data(), splash_w, splash_h,
                        splash_w * 4);
    return true;
}

// Pace software-mode retro_run to content rate. EmuVR's config turns vsync
// off, and audio sync only throttles while the core is actually delivering
// audio - during loads/silence retro_run free-spins, which turns the
// per-frame CaptureGuestOutput readback into a GPU flood.
// Under a normally throttled frontend the deadline is already past and this
// is a no-op.
static void pace_software_frame(void) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (now < next) {
        std::this_thread::sleep_until(next);
        now = clock::now();
    }
    next += std::chrono::nanoseconds(
        core_state.pal_mode ? 20000000 : 16683350);  // 50 / 59.94 Hz
    if (next < now - std::chrono::milliseconds(100)) next = now;
}

static void update_video(void) {
    // Capture the guest frame with the presenter's persistent-resource GPU
    // blit (the same path the HW-render present uses). The old
    // CaptureGuestOutput allocated a Vulkan readback buffer + command pool and
    // did a synchronous GPU wait EVERY frame, which serialized the GPU and
    // collapsed throughput on demanding titles (single-digit fps). The blit
    // path reuses resources across frames and returns a mapped R8G8B8A8 buffer.
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    bool got = false;
    if (lr_graphics && lr_graphics->presenter()) {
        if (strcmp(core_state.graphics_backend, "vulkan") == 0) {
            got = libretro_vk_capture_gpu_blit(lr_graphics->presenter(),
                                               blit_data, w, h);
        }
#ifdef _WIN32
        else {
            got = libretro_d3d12_capture_gpu_blit(lr_graphics->presenter(),
                                                  blit_data, w, h);
        }
#endif
    }

    if (got && blit_data && w > 0 && h > 0) {
        // R8G8B8A8 -> XRGB8888 (little-endian 0x00RRGGBB, B first): swap R/B
        // into the reusable output buffer.
        size_t count = static_cast<size_t>(w) * h;
        if (sw_frame_buf.size() < count) sw_frame_buf.resize(count);
        const uint32_t* src = static_cast<const uint32_t*>(blit_data);
        uint32_t* dst = sw_frame_buf.data();
        for (size_t i = 0; i < count; i++) {
            uint32_t v = src[i];
            dst[i] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                     ((v & 0x000000FFu) << 16);
        }
        core_state.video_cb(dst, w, h, w * 4);
        return;
    }

    // Fallback: blank frame to prevent RetroArch hang.
    uint32_t bw = 1280, bh = 720;
    core_state.video_cb(nullptr, bw, bh, bw * 4);
}

static void update_video_vulkan(void) {
    if (!vulkan_hw_render_active || !vk_hw) {
        update_video();
        return;
    }

    // Step 1: GPU blit capture (A2B10G10R10 ??? R8G8B8A8)
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_vk_capture_gpu_blit(lr_graphics->presenter(),
                                       blit_data, w, h) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }

    // Step 2: Upload to frontend Vulkan image
    uint32_t sync_idx = vk_hw->get_sync_index(vk_hw->handle);
    uint32_t sync_mask = vk_hw->get_sync_index_mask(vk_hw->handle);
    sync_idx &= sync_mask;
    if (sync_idx >= VK_MAX_SYNC) sync_idx = 0;

    vk_hw->wait_sync_index(vk_hw->handle);

    VulkanFrameResources &f = vk_frames[sync_idx];

    // Recreate resources if dimensions changed
    if (f.width != w || f.height != h) {
        vk_destroy_frame(f);
        if (!vk_create_frame(f, w, h)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Failed to create frontend Vulkan frame resources %ux%u\n", w, h);
            vulkan_hw_render_active = false;
            update_video();
            return;
        }
    }

    // Copy Xenia readback ??? frontend staging buffer
    memcpy(f.staging_mapped, blit_data, (size_t)w * h * 4);

    // Record commands: staging ??? image, transition to shader-read
    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo fe_begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    fe_begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &fe_begin);

    VkImageMemoryBarrier fe_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    fe_barrier.srcAccessMask = 0;
    fe_barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fe_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    fe_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fe_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fe_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    fe_barrier.image = f.image;
    fe_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &fe_barrier);

    VkBufferImageCopy fe_region = {};
    fe_region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    fe_region.imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage(cmd, f.staging_buffer, f.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &fe_region);

    fe_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    fe_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fe_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fe_barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &fe_barrier);

    vkEndCommandBuffer(cmd);

    vk_hw->lock_queue(vk_hw->handle);
    VkSubmitInfo fe_submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    fe_submit.commandBufferCount = 1;
    fe_submit.pCommandBuffers = &cmd;
    vkQueueSubmit(vk_hw->queue, 1, &fe_submit, VK_NULL_HANDLE);
    vk_hw->unlock_queue(vk_hw->handle);

    // Step 3: Pass image to frontend
    vk_current_image.image_view = f.image_view;
    vk_current_image.image_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&vk_current_image.create_info, 0, sizeof(vk_current_image.create_info));
    vk_current_image.create_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vk_current_image.create_info.image = f.image;
    vk_current_image.create_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vk_current_image.create_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    vk_current_image.create_info.components = {VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY,
                                                VK_COMPONENT_SWIZZLE_IDENTITY};
    vk_current_image.create_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    vk_hw->set_image(vk_hw->handle, &vk_current_image, 0, nullptr,
                     VK_QUEUE_FAMILY_IGNORED);

    core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0);
}

#ifdef _WIN32
static void update_video_d3d12(void) {
    if (!d3d12_hw_render_active || !d3d12_hw) {
        update_video();
        return;
    }

    // Step 1: Capture with persistent resources (no per-frame alloc)
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_d3d12_capture_gpu_blit(lr_graphics->presenter(),
                                          blit_data, w, h) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }

    // Step 2: Upload to frontend D3D12 texture
    ID3D12Device *device = d3d12_hw->device;

    // Pick next frame slot (double-buffered, avoid CPU-GPU stalls)
    D3D12FrameResources &f = d3d12_frames[d3d12_frame_idx];
    d3d12_frame_idx = (d3d12_frame_idx + 1) % D3D12_NUM_FRAMES;

    // Recreate resources if dimensions changed
    if (f.width != w || f.height != h) {
        // Wait for slot's prior work before destroying
        if (f.fence && f.fence_value > 0) {
            if (f.fence->GetCompletedValue() < f.fence_value) {
                f.fence->SetEventOnCompletion(f.fence_value, f.fence_event);
                WaitForSingleObject(f.fence_event, INFINITE);
            }
        }
        d3d12_destroy_frame(f);
        if (!d3d12_create_frame(f, device, w, h)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Failed to create D3D12 frame resources %ux%u\n", w, h);
            d3d12_hw_render_active = false;
            update_video();
            return;
        }
    }

    // Wait for slot's prior GPU work (2 frames ago, usually done)
    if (f.fence_value > 0) {
        if (f.fence->GetCompletedValue() < f.fence_value) {
            f.fence->SetEventOnCompletion(f.fence_value, f.fence_event);
            WaitForSingleObject(f.fence_event, INFINITE);
        }
    }

    // Copy pixels to upload buffer (row-pitch aligned)
    D3D12_RESOURCE_DESC tex_desc = f.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &layout, nullptr, nullptr, nullptr);

    uint8_t *dst = (uint8_t *)f.upload_mapped + layout.Offset;
    const uint8_t *src = (const uint8_t *)blit_data;
    uint32_t src_pitch = w * 4;
    uint32_t dst_pitch = layout.Footprint.RowPitch;
    for (uint32_t row = 0; row < h; row++) {
        memcpy(dst + row * dst_pitch, src + row * src_pitch, src_pitch);
    }

    // Record commands
    f.cmd_alloc->Reset();
    f.cmd_list->Reset(f.cmd_alloc, nullptr);

    // Transition: COPY_SOURCE ??? COPY_DEST (after first frame)
    if (f.fence_value > 0) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        f.cmd_list->ResourceBarrier(1, &barrier);
    }

    // Copy upload buffer ??? texture
    D3D12_TEXTURE_COPY_LOCATION dst_loc = {};
    dst_loc.pResource = f.texture;
    dst_loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst_loc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src_loc = {};
    src_loc.pResource = f.upload_buffer;
    src_loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src_loc.PlacedFootprint = layout;

    f.cmd_list->CopyTextureRegion(&dst_loc, 0, 0, 0, &src_loc, nullptr);

    // Transition: COPY_DEST ??? COPY_SOURCE (required)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = f.texture;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        f.cmd_list->ResourceBarrier(1, &barrier);
    }

    f.cmd_list->Close();

    // Execute on the frontend's command queue
    ID3D12CommandList *lists[] = { f.cmd_list };
    d3d12_hw->queue->ExecuteCommandLists(1, lists);

    // Signal fence
    f.fence_value++;
    d3d12_hw->queue->Signal(f.fence, f.fence_value);

    // Pass the texture to the frontend
    d3d12_hw->set_texture(d3d12_hw->handle, f.texture, DXGI_FORMAT_R8G8B8A8_UNORM);

    // Signal to RetroArch that a HW frame is ready
    core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, w, h, 0);
}
#endif /* _WIN32 */

/* ================================================================== */
/*  Xenia lifecycle (C++ internal)                                     */
/* ================================================================== */

#ifdef _WIN32
// SEH wrapper (no C++ objects on stack).
#pragma warning(push)
#pragma warning(disable: 4611)  // setjmp interaction
static xe::X_STATUS xenia_launch_path_seh(const std::filesystem::path &p) {
    __try {
        return xenia_emulator->LaunchPath(p);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        DWORD code = GetExceptionCode();
        xenia_log(RETRO_LOG_ERROR,
                  "SEH exception in LaunchPath! Code: 0x%08lX\n", code);
        return static_cast<xe::X_STATUS>(0x80000000 | code);
    }
}
#pragma warning(pop)
#else
static xe::X_STATUS xenia_launch_path_seh(const std::filesystem::path &p) {
    return xenia_emulator->LaunchPath(p);
}
#endif

static bool xenia_setup_and_launch(const char *path) {
    try {
        namespace fs = std::filesystem;
        fs::path storage = fs::path(core_state.save_dir);
        // Guest content (profiles, DLC, title updates) lives under
        // system/xenia - as content_root directly in the frontend's shared
        // system dir, its XUID-named folders (E0..../0000000000000000)
        // confused users browsing a dir many cores share.
        fs::path content = fs::path(core_state.system_dir) / "xenia";
        fs::path cache   = fs::path(core_state.save_dir) / "cache";
        fs::path cmdline = fs::path(path);

        std::error_code ec;
        fs::create_directories(cache, ec);
        fs::create_directories(storage, ec);
        fs::create_directories(content, ec);

        // Migrate content from builds that mounted the system dir root:
        // XUID-named folders (16 hex chars) are ours - move them under
        // system/xenia so existing profiles, saves and DLC keep working.
        {
            std::vector<fs::path> old_dirs;
            std::error_code iter_ec;
            for (const auto& entry : fs::directory_iterator(
                     fs::path(core_state.system_dir), iter_ec)) {
                if (!entry.is_directory(iter_ec)) continue;
                std::string name = entry.path().filename().string();
                if (name.size() != 16 ||
                    name.find_first_not_of("0123456789abcdefABCDEF") !=
                        std::string::npos) {
                    continue;
                }
                old_dirs.push_back(entry.path());
            }
            for (const auto& old_dir : old_dirs) {
                fs::path new_dir = content / old_dir.filename();
                std::error_code mig_ec;
                if (fs::exists(new_dir, mig_ec)) {
                    xenia_log(RETRO_LOG_WARN,
                              "Guest content exists both at %s and %s; using "
                              "the latter, merge or remove the former "
                              "manually\n",
                              old_dir.string().c_str(),
                              new_dir.string().c_str());
                    continue;
                }
                fs::rename(old_dir, new_dir, mig_ec);
                if (mig_ec) {
                    xenia_log(RETRO_LOG_WARN,
                              "Failed to move guest content %s to %s: %s\n",
                              old_dir.string().c_str(),
                              new_dir.string().c_str(),
                              mig_ec.message().c_str());
                } else {
                    xenia_log(RETRO_LOG_INFO,
                              "Moved guest content %s to %s\n",
                              old_dir.string().c_str(),
                              new_dir.string().c_str());
                }
            }
        }
        // Initialize Xenia logging first
#ifdef _WIN32
        xe::InitializeWin32App("xenia_libretro");
#else
        xe::InitializeLogging("xenia_libretro");
#endif

        xenia_emulator = std::make_unique<xe::Emulator>(
            cmdline, storage, content, cache);

        xe::X_STATUS status = xenia_emulator->Setup(
            /*display_window=*/nullptr,
            /*imgui_drawer=*/nullptr,
            /*require_cpu_backend=*/true,
            /*audio_system_factory=*/
            [](xe::cpu::Processor *processor)
                -> std::unique_ptr<xe::apu::AudioSystem> {
                auto sys = std::make_unique<xe::apu::libretro::LibretroAudioSystem>(processor);
                audio_ring = sys->ring_buffer();
                return sys;
            },
            /*graphics_system_factory=*/
            []() -> std::unique_ptr<xe::gpu::GraphicsSystem> {
                std::unique_ptr<xe::gpu::GraphicsSystem> gs;
                if (strcmp(core_state.graphics_backend, "vulkan") == 0) {
                    gs = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
                }
#ifdef _WIN32
                else {
                    gs = std::make_unique<xe::gpu::d3d12::D3D12GraphicsSystem>();
                }
#endif
                if (!gs) {
                    // Fallback to Vulkan if D3D12 unavailable
                    gs = std::make_unique<xe::gpu::vulkan::VulkanGraphicsSystem>();
                }
                lr_graphics = gs.get();
                return gs;
            },
            /*input_driver_factory=*/
            [](xe::ui::Window *window)
                -> std::vector<std::unique_ptr<xe::hid::InputDriver>> {
                std::vector<std::unique_ptr<xe::hid::InputDriver>> v;
                auto drv = std::make_unique<xe::hid::libretro_hid::LibretroInputDriver>(window, 0);
                lr_input_driver = drv.get();
                v.push_back(std::move(drv));
                return v;
            });

        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Emulator::Setup failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        // Upstream split subsystem creation out of Setup so per-game cvar
        // overrides can load first; without this graphics_system() is null.
        status = xenia_emulator->SetupSubsystems();
        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "Emulator::SetupSubsystems failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        // Sign in a profile before launch so the title sees a logged-in user
        // (saves and scores in profile-aware games, XBLA especially). The
        // standalone app does this through its profile dialog; headless we
        // reuse the first profile on disk or generate one.
        {
            const char* pv = opt_get(XENIA_OPT_AUTO_PROFILE);
            bool auto_profile = !pv || strcmp(pv, "disabled") != 0;
            auto* xam = xenia_emulator->kernel_state()
                            ? xenia_emulator->kernel_state()->xam_state()
                            : nullptr;
            auto* pm = xam ? xam->profile_manager() : nullptr;
            if (auto_profile && pm && !pm->IsAnyProfileSignedIn()) {
                const auto* accounts = pm->GetAccounts();
                if (accounts && !accounts->empty()) {
                    pm->Login(accounts->begin()->first, 0);
                    xenia_log(RETRO_LOG_INFO,
                              "Signed in existing profile (slot 0)\n");
                } else if (pm->CreateProfile("PlayerOne", true)) {
                    xenia_log(RETRO_LOG_INFO,
                              "Created and signed in profile 'PlayerOne'\n");
                } else {
                    xenia_log(RETRO_LOG_WARN,
                              "Failed to create default profile\n");
                }
            }
        }

        status = xenia_launch_path_seh(fs::path(path));

        if (XFAILED(status)) {
            xenia_log(RETRO_LOG_ERROR,
                      "LaunchPath failed 0x%08X\n", status);
            xenia_emulator.reset();
            return false;
        }

        game_loaded = true;
        return true;

    } catch (const std::exception &e) {
        xenia_log(RETRO_LOG_ERROR, "Exception in setup: %s\n", e.what());
        xenia_emulator.reset();
        return false;
    } catch (...) {
        xenia_log(RETRO_LOG_ERROR, "Unknown exception in setup!\n");
        xenia_emulator.reset();
        return false;
    }
}

static void xenia_shutdown(void) {
    if (xenia_emulator) {
        xenia_emulator->TerminateTitle();
        xenia_emulator->Shutdown();
        xenia_emulator.reset();
    }
    audio_ring = nullptr;
    lr_input_driver = nullptr;
    lr_graphics = nullptr;
    game_loaded = false;

    // Clean up Vulkan HW render resources (frontend side)
    if (vk_hw) {
        vkDeviceWaitIdle(vk_hw->device);
        vk_destroy_all_frames();
    }
    vk_hw = nullptr;
    vulkan_hw_render_active = false;
    memset(&vk_current_image, 0, sizeof(vk_current_image));

    // Clean up D3D12 HW render resources
#ifdef _WIN32
    d3d12_destroy_all_frames();
    d3d12_hw = nullptr;
#endif
    d3d12_hw_render_active = false;
}

/* ================================================================== */
/*  Libretro API (extern "C")                                          */
/* ================================================================== */
extern "C" {

RETRO_API unsigned retro_api_version(void) {
    return RETRO_API_VERSION;
}

RETRO_API void retro_set_environment(retro_environment_t cb) {
    core_state.environ_cb = cb;

    struct retro_log_callback log_cb;
    if (cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb)) {
        core_state.log_cb = log_cb.log;
    }

    // Publish core options (v2 with legacy SET_VARIABLES fallback for
    // frontends like EmuVR's RetroArch 1.7.5)
    xenia_publish_core_options(cb);

    // Publish input descriptors for 4 Xbox 360 controllers. All kMaxPorts
    // ports are polled in LibretroInputDriver::UpdateFromLibretro, so every
    // port needs labels - otherwise players 2-4 show generic RetroPad names.
    static const struct retro_input_descriptor descs[] = {
#define XENIA_PORT_DESCS(p) \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "A" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "B" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "X" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "Y" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Back" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "LB" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "RB" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "LT" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "RT" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "LS" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "RS" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "D-Pad Up" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "D-Pad Down" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "D-Pad Left" }, \
        { p, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
        { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" }, \
        { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" }, \
        { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" }, \
        { p, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },
        XENIA_PORT_DESCS(0)
        XENIA_PORT_DESCS(1)
        XENIA_PORT_DESCS(2)
        XENIA_PORT_DESCS(3)
#undef XENIA_PORT_DESCS
        { 0 },
    };
    cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)descs);

    // NOTE: deliberately no SET_CONTROLLER_INFO here. Declaring N controller
    // ports makes the frontend call retro_set_controller_port_device() for
    // each one, which LibretroInputDriver::SetPortDevice turns into
    // port_connected_ = true. That presents four live 360 pads to the title
    // while only profile slot 0 is signed in (see retro_load_game), and the
    // guest crashes during launch. Dolphin can declare four ports because it
    // genuinely emulates four; until this core handles multiple pads (and
    // offers a RETRO_DEVICE_NONE option per port, as PCSX2 does), one implicit
    // port is correct. The input descriptors above are labels only and carry
    // no connection semantics, so they stay.

    const char *dir = nullptr;
    if (cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
        snprintf(core_state.system_dir, sizeof(core_state.system_dir), "%s", dir);
    dir = nullptr;
    if (cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
        snprintf(core_state.save_dir, sizeof(core_state.save_dir), "%s", dir);

}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb)      { core_state.video_cb       = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb)        { core_state.audio_cb       = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { core_state.audio_batch_cb = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb)            { core_state.input_poll_cb  = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb)          { core_state.input_state_cb = cb; }

RETRO_API void retro_init(void) {
#ifdef _WIN32
    // The splash and software-present pacing sleep in ~17 ms ticks; without
    // this the Windows default 15.6 ms timer granularity makes each sleep
    // overshoot by up to a full tick. Per-process since Windows 10 2004, so
    // the frontend's own resolution is unaffected.
    timeBeginPeriod(1);
#endif
    core_state.audio_sample_rate  = 48000.0;
    core_state.audio_buffer_size  = 4096;   // int16 values (2048 stereo frames)
    core_state.audio_buffer       = static_cast<int16_t *>(
        calloc(core_state.audio_buffer_size, sizeof(int16_t)));
    memset(core_state.graphics_backend, 0, sizeof(core_state.graphics_backend));
    core_state.vsync_enabled    = true;
    core_state.audio_enabled    = true;
    core_state.pal_mode         = false;

}

RETRO_API void retro_deinit(void) {
    xenia_shutdown();

    free(core_state.audio_buffer);
    core_state.audio_buffer = nullptr;

#ifdef _WIN32
    timeEndPeriod(1);
#endif
}

RETRO_API void retro_get_system_info(struct retro_system_info *info) {
    memset(info, 0, sizeof(*info));
    info->library_name     = "Xenia Edge";
    info->library_version  = "0.1.0";
    info->need_fullpath    = true;
    info->valid_extensions = "iso|xex|zar|xcp|x360";
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    info->geometry.base_width   = 1280;
    info->geometry.base_height  = 720;
    info->geometry.max_width    = 3840;
    info->geometry.max_height   = 2160;
    info->geometry.aspect_ratio = 16.0f / 9.0f;
    info->timing.fps            = core_state.pal_mode ? 50.0 : 60.0;
    info->timing.sample_rate    = core_state.audio_sample_rate;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device) {
    if (lr_input_driver)
        lr_input_driver->SetPortDevice(port, device);
}

RETRO_API bool retro_load_game(const struct retro_game_info *info) {
    if (!info || !info->path) {
        xenia_log(RETRO_LOG_ERROR, "No game path supplied\n");
        return false;
    }
    snprintf(core_state.game_path, sizeof(core_state.game_path),
             "%s", info->path);

    // .x360 pointer files: a one-line text file whose content is the path of
    // the real game (absolute, or relative to the pointer file). Lets file
    // browsers and frontend scanners see extension-less content like GOD/XBLA
    // package headers without renaming the library.
    {
        size_t len = strlen(core_state.game_path);
        const char* ext = len > 5 ? core_state.game_path + len - 5 : "";
        bool is_ptr = false;
        if (ext[0] == '.' &&
            (ext[1] == 'x' || ext[1] == 'X') &&
            ext[2] == '3' && ext[3] == '6' && ext[4] == '0') {
            is_ptr = true;
        }
        if (is_ptr) {
            FILE* pf = fopen(core_state.game_path, "rb");
            if (!pf) {
                xenia_log(RETRO_LOG_ERROR, "Cannot open pointer file %s\n",
                          core_state.game_path);
                return false;
            }
            char line[sizeof(core_state.game_path)] = {0};
            if (!fgets(line, sizeof(line), pf)) line[0] = 0;
            fclose(pf);
            // Trim trailing whitespace/newline and optional quotes.
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' ||
                         line[n - 1] == ' ' || line[n - 1] == '\t'))
                line[--n] = 0;
            char* target = line;
            if (n >= 2 && target[0] == '"' && target[n - 1] == '"') {
                target[n - 1] = 0;
                target++;
            }
            if (!target[0]) {
                xenia_log(RETRO_LOG_ERROR, "Pointer file %s is empty\n",
                          core_state.game_path);
                return false;
            }
            std::error_code ptr_ec;
            std::filesystem::path resolved(target);
            if (resolved.is_relative()) {
                resolved = std::filesystem::path(core_state.game_path)
                               .parent_path() / resolved;
            }
            // Verify the target before handing it to the emulator. Pointer
            // files routinely hold absolute paths, so moving or renaming a
            // library silently breaks them; without this check the launch
            // fails deep inside LaunchPath as a bare 0xC00000BB
            // (STATUS_NOT_SUPPORTED) that says nothing about the real cause.
            if (!std::filesystem::exists(resolved, ptr_ec)) {
                xenia_log(RETRO_LOG_ERROR,
                          "Pointer file %s targets a path that does not "
                          "exist: %s\n",
                          core_state.game_path, resolved.string().c_str());
                xenia_log(RETRO_LOG_ERROR,
                          "Edit the pointer file so it contains the full path "
                          "to the content's current location.\n");
                return false;
            }
            snprintf(core_state.game_path, sizeof(core_state.game_path), "%s",
                     resolved.string().c_str());
            xenia_log(RETRO_LOG_INFO, "Pointer file resolved to: %s\n",
                      core_state.game_path);
        }
    }

    // Load xenia's own config file, if the user has one, BEFORE core options.
    // Only a few dozen of xenia's cvars are exposed as core options; everything
    // else - the targeted EDRAM/accuracy knobs, per-title workarounds, the
    // settings community configs are built around - was previously unreachable
    // from this core. Reading the config here makes all of them available while
    // keeping core options authoritative for the subset we do expose, since
    // apply_core_options() runs immediately after and overwrites them.
    //
    // SetupConfig() also writes the file back (config.cc SaveConfig), which
    // gives the user a fully self-documenting config to edit - the same
    // behaviour as standalone xenia. SaveConfig is a no-op until this call, so
    // this is the point where that becomes live.
    {
        std::error_code cfg_ec;
        std::filesystem::path cfg_dir =
            std::filesystem::path(core_state.system_dir) / "xenia";
        std::filesystem::create_directories(cfg_dir, cfg_ec);
        config::SetupConfig(cfg_dir);
        xenia_log(RETRO_LOG_INFO, "Config folder: %s\n",
                  cfg_dir.string().c_str());

        // Then layer this title's own config on top, if one exists. xenia
        // stores these as <config folder>/config/<TITLEID>.config.toml and
        // clears the previous title's overrides first, so each game gets
        // base config + its own settings and nothing leaks between titles.
        // The desktop app does this; the core never did, which left per-game
        // settings unreachable - and core options cannot fill the gap because
        // RetroArch 1.7.5 has no per-game core options, so anything set there
        // applies to every 360 title at once.
        uint32_t cfg_title_id =
            config::LoadGameConfigForFile(core_state.game_path);
        if (cfg_title_id) {
            xenia_log(RETRO_LOG_INFO,
                      "Loaded per-title config overrides for %08X\n",
                      cfg_title_id);
        }
    }

    // Apply any options set before load
    apply_core_options();

    // Pick graphics backend based on frontend's preferred HW context.
    unsigned preferred_hw = RETRO_HW_CONTEXT_NONE;
    bool have_preferred_hw = core_state.environ_cb(
        RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER, &preferred_hw);
    if (preferred_hw == RETRO_HW_CONTEXT_VULKAN) {
        strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
                sizeof(core_state.graphics_backend) - 1);
    } else if (!have_preferred_hw) {
        // Frontend predates GET_PREFERRED_HW_RENDER (e.g. RetroArch 1.7.5 /
        // EmuVR): no HW render negotiation is possible. Prefer the D3D12
        // backend (markedly faster on demanding titles) when its Agility
        // runtime is shipped alongside the frontend; otherwise use the
        // self-contained Vulkan backend (no extra DLLs, works under Wine too).
#ifdef _WIN32
        if (d3d12_runtime_available() && !primary_gpu_is_amd()) {
            strncpy(core_state.graphics_backend, XENIA_GRAPHICS_D3D12,
                    sizeof(core_state.graphics_backend) - 1);
        } else
#endif
        {
            strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
                    sizeof(core_state.graphics_backend) - 1);
        }
    } else {
        // D3D12, D3D11, OpenGL, or anything else -> use D3D12 backend, except
        // on AMD where the D3D12 backend is currently broken (use Vulkan).
#ifdef _WIN32
        if (primary_gpu_is_amd()) {
            strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
                    sizeof(core_state.graphics_backend) - 1);
        } else {
            strncpy(core_state.graphics_backend, XENIA_GRAPHICS_D3D12,
                    sizeof(core_state.graphics_backend) - 1);
        }
#else
        // Vulkan is the only backend that exists off Windows - there is no
        // D3D12 to fall back to. A frontend preferring OpenGL (RetroArch's
        // usual Linux default) must not steer us into a backend that cannot
        // be built here.
        strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
                sizeof(core_state.graphics_backend) - 1);
#endif
    }
    // Explicit backend option overrides the frontend-derived choice. The
    // frontend's HW render interface is only negotiated when its preferred
    // context matches, so a forced backend simply renders internally and
    // delivers software frames (the Wine/Proton-friendly path: Vulkan there
    // avoids the D3D12 Agility SDK requirement entirely).
    {
        const char* bopt = opt_get(XENIA_OPT_GPU_BACKEND);
#ifndef _WIN32
        // The d3d12 value is not offered in the option list off Windows, but a
        // stale options file (copied from a Windows install, or a frontend
        // core-override) can still carry it. There is no D3D12 here and the
        // safety net below is Windows-only, so refuse it rather than fail hard.
        if (bopt && strcmp(bopt, XENIA_GRAPHICS_D3D12) == 0) {
            xenia_log(RETRO_LOG_WARN,
                      "GPU Backend 'd3d12' is not available on this platform; "
                      "using Vulkan\n");
            bopt = XENIA_GRAPHICS_VULKAN;
        }
#endif
        if (bopt && strcmp(bopt, "auto") != 0) {
            strncpy(core_state.graphics_backend, bopt,
                    sizeof(core_state.graphics_backend) - 1);
            if (strcmp(bopt, XENIA_GRAPHICS_VULKAN) != 0)
                preferred_hw = RETRO_HW_CONTEXT_NONE;
        }
    }
#ifdef _WIN32
    // Safety net: D3D12 needs its Agility runtime next to the frontend AND an
    // OS/driver combination that actually delivers Shader Model 6.6. Probe the
    // full provider initialization once here; on any failure fall back to the
    // self-contained Vulkan backend so the core still works instead of dying
    // with a fatal error dialog.
    if (strcmp(core_state.graphics_backend, XENIA_GRAPHICS_D3D12) == 0) {
        const char* d3d12_fail_reason = nullptr;
        if (!d3d12_runtime_available()) {
            d3d12_fail_reason = "D3D12/D3D12Core.dll was not found next to the "
                                "frontend";
        } else if (!xe::ui::d3d12::D3D12Provider::Create(
                       /*fatal_on_failure=*/false)) {
            d3d12_fail_reason = "the Direct3D 12 graphics subsystem failed to "
                                "initialize (see the xenia log for details)";
        }
        if (d3d12_fail_reason) {
            xenia_log(RETRO_LOG_WARN,
                      "D3D12 selected but %s; falling back to Vulkan\n",
                      d3d12_fail_reason);
            strncpy(core_state.graphics_backend, XENIA_GRAPHICS_VULKAN,
                    sizeof(core_state.graphics_backend) - 1);
            preferred_hw = RETRO_HW_CONTEXT_NONE;
        }
    }
#endif
    // Also update the gpu cvar so internal Xenia code stays consistent
    cvars::gpu = core_state.graphics_backend;
    xenia_log(RETRO_LOG_INFO,
              "Frontend preferred HW context: %u -> using %s backend\n",
              preferred_hw, core_state.graphics_backend);

    // Request HW render from the frontend.
    memset(&hw_render_cb, 0, sizeof(hw_render_cb));
    hw_render_cb.depth = false;
    hw_render_cb.stencil = false;
    hw_render_cb.bottom_left_origin = false;
    hw_render_cb.cache_context = false;

    if (preferred_hw == RETRO_HW_CONTEXT_VULKAN) {
        hw_render_cb.context_type = RETRO_HW_CONTEXT_VULKAN;
        hw_render_cb.context_reset = vulkan_context_reset;
        hw_render_cb.context_destroy = vulkan_context_destroy;

        if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
            xenia_log(RETRO_LOG_WARN,
                      "Frontend rejected Vulkan HW render ??? falling back to software\n");
            vulkan_hw_render_active = false;
        }
    }
#ifdef _WIN32
    else if (preferred_hw == RETRO_HW_CONTEXT_D3D12) {
        hw_render_cb.context_type = RETRO_HW_CONTEXT_D3D12;
        hw_render_cb.context_reset = d3d12_context_reset;
        hw_render_cb.context_destroy = d3d12_context_destroy;

        if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render_cb)) {
            xenia_log(RETRO_LOG_WARN,
                      "Frontend rejected D3D12 HW render ??? falling back to software\n");
            d3d12_hw_render_active = false;
        }
    }
#endif

    // Set pixel format (needed for software fallback and D3D12).
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
    if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        xenia_log(RETRO_LOG_ERROR, "XRGB8888 not supported\n");
        return false;
    }

    // Acquire rumble interface from the frontend
    struct retro_rumble_interface rumble = {0};
    core_state.environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble);
    pending_rumble_cb = rumble.set_rumble_state;

    // "Before Boot" splash: play the video first and defer the launch to
    // retro_run for the authentic power-on sequence. "During Load" keeps
    // the old behavior (game boots underneath the video).
    splash_try_start();
    const char* sv = opt_get(XENIA_OPT_BOOT_SPLASH);
    bool splash_before = !sv || strcmp(sv, "loadmask") != 0;
    if (splash_plm && splash_before) {
        launch_pending = true;
        return true;
    }

    bool ok = xenia_setup_and_launch(core_state.game_path);

    // Now that the HID driver exists, give it the rumble callback
    if (ok && lr_input_driver && pending_rumble_cb)
        lr_input_driver->SetRumbleCallback(pending_rumble_cb);

    return ok;
}

RETRO_API void retro_unload_game(void) {
    splash_stop();
    launch_pending = false;
    xenia_shutdown();
}

RETRO_API unsigned retro_get_region(void) {
    return core_state.pal_mode ? RETRO_REGION_PAL : RETRO_REGION_NTSC;
}

RETRO_API bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) {
    return false;
}

RETRO_API size_t retro_serialize_size(void) {
    return 0; // Save-states not yet supported for an Xbox 360 emulator
}
RETRO_API bool retro_serialize(void *, size_t) { return false; }
RETRO_API bool retro_unserialize(const void *, size_t) { return false; }

RETRO_API void *retro_get_memory_data(unsigned id) {
    if (!xenia_emulator || !game_loaded) return nullptr;
    if (id == RETRO_MEMORY_SYSTEM_RAM && xenia_emulator->memory())
        return xenia_emulator->memory()->physical_membase();
    return nullptr;
}

RETRO_API size_t retro_get_memory_size(unsigned id) {
    if (id == RETRO_MEMORY_SYSTEM_RAM)
        return 512u * 1024u * 1024u;  // Xbox 360: 512 MB unified
    return 0;
}

RETRO_API void retro_reset(void) {
}

RETRO_API void retro_run(void) {
    // Poll input from the frontend
    if (core_state.input_poll_cb) core_state.input_poll_cb();

    // Feed libretro input state into Xenia's HID system
    if (lr_input_driver && core_state.input_state_cb)
        lr_input_driver->UpdateFromLibretro(core_state.input_state_cb);

    // Capture the latest frame via the appropriate video path.

    // Boot splash replaces game A/V while it plays (software mode only:
    // HW-render frontends ignore memory frames).
    bool hw_active = vulkan_hw_render_active;
#ifdef _WIN32
    hw_active = hw_active || d3d12_hw_render_active;
#endif
    if (splash_plm && hw_active) splash_stop();

    if (!splash_run_frame()) {
        // Deferred "Before Boot" launch: the splash just ended (or was
        // skipped/invalid); boot the game now. This retro_run blocks like a
        // normal content load; the frontend keeps showing the last frame.
        if (launch_pending) {
            launch_pending = false;
            if (!xenia_setup_and_launch(core_state.game_path)) {
                xenia_log(RETRO_LOG_ERROR, "Deferred game launch failed\n");
                core_state.environ_cb(RETRO_ENVIRONMENT_SHUTDOWN, NULL);
                return;
            }
            if (lr_input_driver && pending_rumble_cb)
                lr_input_driver->SetRumbleCallback(pending_rumble_cb);
            core_state.video_cb(NULL, 1280, 720, 1280 * 4);  // dupe frame
            return;
        }
        update_audio();
        if (vulkan_hw_render_active)
            update_video_vulkan();
#ifdef _WIN32
        else if (d3d12_hw_render_active)
            update_video_d3d12();
#endif
        else {
            pace_software_frame();
            update_video();
        }
    }

    // React to option changes
    bool vars_updated = false;
    if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE,
                              &vars_updated) && vars_updated) {
        apply_core_options();
    }
}

RETRO_API void retro_cheat_reset(void) {}
RETRO_API void retro_cheat_set(unsigned, bool, const char *) {}

} /* extern "C" */
