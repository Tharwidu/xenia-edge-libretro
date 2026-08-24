/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <algorithm>
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
DECLARE_bool(patch_all_in_file);

// Frontend capabilities queried once at init. Each has a working fallback, so
// an older frontend - RetroArch 1.7.5, which EmuVR ships - simply keeps the
// previous behaviour.
static bool g_input_bitmasks = false;   // all buttons in one call per port
static bool g_can_dupe = false;         // NULL frame means "repeat last"
DECLARE_int32(license_mask);
DECLARE_int32(headless_messagebox_button);
DECLARE_int32(user_language);

// Unlike every other cvar this core reaches for, the stick deadzones are
// DEFINE_double'd *inside* `namespace xe { namespace hid {` in
// input_system.cc, so they live at xe::hid::cvars rather than the global
// cvars. Declaring them at file scope compiles cleanly and then fails at link.
namespace xe {
namespace hid {
DECLARE_double(left_stick_deadzone_percentage);
DECLARE_double(right_stick_deadzone_percentage);
// Same file, same trap - vibration is defined inside xe::hid too.
DECLARE_bool(vibration);
}  // namespace hid
}  // namespace xe
// Everything below is defined at global scope, so a plain declaration resolves.
DECLARE_uint32(volume);
DECLARE_uint32(apu_max_queued_frames);
DECLARE_int32(avpack);
DECLARE_bool(allow_incompatible_title_update);
DECLARE_bool(vulkan_sparse_shared_memory);
DECLARE_bool(tiled_shared_memory);
DECLARE_bool(readback_resolve_sync);
#ifdef _WIN32
// Defined in d3d12_command_processor.cc, and xenia-gpu-d3d12-headless is built
// only `if os.istarget("windows")` - declaring it unconditionally compiles
// everywhere and then fails to link on Linux.
DECLARE_bool(d3d12_bindless);
#endif
DECLARE_int32(user_country);
DECLARE_bool(protect_zero);
DECLARE_bool(clear_memory_page_state);
// Declared for the effective-settings report only - these are not exposed as
// core options, so a config file is the only way to set them and the log is
// the only way to confirm one took.
DECLARE_uint32(texture_cache_memory_limit_render_to_texture);
DECLARE_bool(elide_e0_check);
DECLARE_bool(inline_loadclock);
DECLARE_uint32(align_all_basic_blocks);
DECLARE_bool(enable_rmw_context_merging);
DECLARE_bool(ignore_thread_priorities);
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
// apu/gpu and the mount switches moved into emulator.cc upstream, so defining
// them here now collides at link; declare those and keep the ones the app
// still owns. The core sets apu/gpu itself once it has chosen its backends.
DECLARE_string(apu);
DECLARE_string(gpu);
DEFINE_string(hid, "nop", "Input system.", "HID");

DEFINE_path(storage_root, "", "Root path for persistent internal data storage.",
            "Storage");
DEFINE_path(content_root, "", "Root path for guest content storage.",
            "Storage");
DEFINE_path(cache_root, "", "Root path for cache files.", "Storage");

DECLARE_bool(mount_scratch);
DECLARE_bool(mount_cache);

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
static xe::apu::libretro::LibretroAudioMixer *audio_mixer = nullptr;
static xe::hid::libretro_hid::LibretroInputDriver *lr_input_driver = nullptr;
static xe::gpu::GraphicsSystem *lr_graphics = nullptr;
static bool game_loaded = false;

// Upper bound on the video the frontend must be prepared to receive. Declared
// once in retro_get_system_av_info and never changed afterwards - raising it
// later would need SET_SYSTEM_AV_INFO, which reinitializes the video driver.
// The frontend may size buffers from these, so they are a real cost and not
// something to inflate "just in case"; 4K covers every 360 display mode with
// room for a 2x-3x resolution scale, and report_geometry clamps beyond that.
static constexpr uint32_t kMaxGeometryWidth  = 3840;
static constexpr uint32_t kMaxGeometryHeight = 2160;

// Last geometry handed to the frontend, so report_geometry only calls out when
// something actually changed. Cleared on shutdown: these outlive a single
// title, and a stale match would make the first frame of the next game skip its
// report and sit on whatever av_info declared.
static uint32_t last_geometry_w = 0, last_geometry_h = 0;

// Dimensions of the last SOFTWARE frame actually handed to the frontend.
//
// A duped frame (NULL data) must repeat these exactly, because RetroArch keeps
// the previous pixel buffer but overwrites the size it is read at:
//
//     void video_driver_cached_frame_publish(data, width, height, pitch) {
//        if (data) frame_cache_data = data;   /* pixels: only when non-NULL */
//        frame_cache_width  = width;          /* but these always change    */
//        frame_cache_height = height;
//        frame_cache_pitch  = pitch;
//     }
//
// So passing a hardcoded 1280x720 alongside NULL tells the frontend to read the
// previous frame's buffer at the wrong size. With dynamic geometry that buffer
// is often smaller - Halo Reach renders 1152x720 - and 720 rows at a 1280*4
// pitch reads ~368 KB past a 1152*720*4 allocation.
//
// Harmless until 2026-08-03 only because GET_CAN_DUPE was being clobbered, so
// the fast-forward path never ran. Fixing that probe made this reachable.
//
// Seeded to 1280x720 so the very first frame, before anything has been
// delivered, behaves as it always did.
static uint32_t last_frame_w = 1280, last_frame_h = 720;

// Repeat the previous frame. Never pass literal dimensions to a NULL frame.
static inline void emit_dupe_frame(void) {
    core_state.video_cb(nullptr, last_frame_w, last_frame_h,
                        static_cast<size_t>(last_frame_w) * 4);
}
static uint32_t last_geometry_aspect_x = 0, last_geometry_aspect_y = 0;

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

// is_bgra selects the image format so it matches whatever byte order the
// capture blit produced - they must agree, the upload is a straight memcpy.
static bool vk_create_frame(VulkanFrameResources &f,
                             uint32_t w, uint32_t h, bool is_bgra) {
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
    img_info.format =
        is_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
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
    view_info.format =
        is_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
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

// True when an option is set to "auto", meaning: do not write the cvar, leave
// whatever the config decided.
//
// The frontend always hands back a value - its own default if the user never
// touched the option - so an unconditional assignment here overwrites the
// config every single launch. That made every cvar we expose impossible to set
// per title, and a perfectly valid config line for one of them would silently
// do nothing.
//
// RetroArch 1.7.5 does have per-game core options - a hand-written
// config/<core name>/<game>.opt is read, verified 2026-07-30 - but it REPLACES
// the global options file rather than merging with it, so anything it omits
// falls back to the core default. That makes it a poor per-title mechanism for
// a core with hundreds of cvars, and the config file the practical one. Either
// way the config has to be able to win.
static bool opt_is_auto(const char *v) {
    return v && strcmp(v, "auto") == 0;
}

#ifdef _WIN32
// Win32 API surface for the environment report below (GetModuleHandleA,
// GetProcAddress, WideCharToMultiByte). It arrives transitively via dxgi.h
// today, but through xenia's wrapper rather than raw windows.h so the
// project's NOMINMAX / lean-and-mean defines apply.
#include "xenia/base/platform_win.h"

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

// True when this process is running under Wine/Proton rather than real Windows.
// wine_get_version is exported by Wine's ntdll and by nothing else, so its
// presence is the standard way to tell. Cached because the answer cannot
// change, and because the backend policy below asks more than once. The Wine
// version string, when there is one, comes back through *version_out.
static bool running_under_wine(const char** version_out = nullptr) {
    static bool checked = false;
    static bool is_wine = false;
    static const char* version = nullptr;
    if (!checked) {
        checked = true;
        if (HMODULE ntdll = GetModuleHandleA("ntdll.dll")) {
            typedef const char*(CDECL * wine_get_version_t)(void);
            auto wine_get_version = reinterpret_cast<wine_get_version_t>(
                reinterpret_cast<void*>(
                    GetProcAddress(ntdll, "wine_get_version")));
            if (wine_get_version) {
                is_wine = true;
                version = wine_get_version();
            }
        }
    }
    if (version_out) *version_out = version;
    return is_wine;
}

// Report the graphics environment once, before a backend is chosen.
//
// When D3D12 failed under Proton the core logged a single line naming the
// backend and nothing else - not the adapter, not whether we were even running
// under Wine - so the failure was undiagnosable and the only way forward was to
// route around it onto Vulkan. Everything here is read-only: adapter properties
// come from DXGI without creating a device, and Wine is detected by looking for
// an export that only exists there.
static void log_graphics_environment(void) {
    // Under Proton, D3D12 is vkd3d-proton rather than Microsoft's
    // implementation, which is exactly the case where xenia's D3D12 backend
    // has been failing.
    const char* wine_version = nullptr;
    if (running_under_wine(&wine_version)) {
        xenia_log(RETRO_LOG_INFO, "Host: Wine/Proton %s\n",
                  wine_version ? wine_version : "(unknown version)");
    } else {
        xenia_log(RETRO_LOG_INFO, "Host: native Windows\n");
    }

    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        xenia_log(RETRO_LOG_WARN, "DXGI unavailable; no adapter info\n");
        return;
    }
    IDXGIAdapter1* adapter = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &adapter) == S_OK; i++) {
        DXGI_ADAPTER_DESC1 desc;
        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            char name[128] = {0};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name,
                                sizeof(name) - 1, nullptr, nullptr);
            xenia_log(RETRO_LOG_INFO,
                      "GPU %u: %s (vendor %04X, device %04X, %llu MB%s)\n", i,
                      name, desc.VendorId, desc.DeviceId,
                      (unsigned long long)(desc.DedicatedVideoMemory >> 20),
                      (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ? ", software"
                                                                : "");
        }
        adapter->Release();
    }
    factory->Release();

    xenia_log(RETRO_LOG_INFO, "D3D12 Agility runtime present: %s\n",
              d3d12_runtime_available() ? "yes" : "no");
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

// Why the automatic backend choice must steer away from D3D12 on this host, or
// nullptr when it needn't. This is for defects that cannot be detected by
// probing the D3D12 runtime itself - anything that CAN be probed belongs in the
// backend probe further down, which is causal rather than correlational and so
// stops being wrong the moment the host is fixed. An explicit
// xenia_gpu_backend=d3d12 still overrides this: it is a default, not a ban.
//
// The core ships into four deployment contexts, and the backend default has to
// be right in all of them:
//
//   1. Linux, native build, upstream frontend  - no D3D12 code is compiled at
//      all (everything here is under _WIN32), so Vulkan is the only option.
//      Decided at compile time; nothing below runs.
//   2. Windows, native build, upstream frontend - D3D12.
//   3. Windows, native, under RetroArch 1.7.5   - D3D12.
//   4. Linux, under Wine/Proton, under RA 1.7.5 - must be Vulkan.
//
// Cases 3 and 4 are the awkward pair: they are the *same Windows DLL* on the
// *same frontend*, so nothing at compile time or in the frontend handshake can
// separate them. A host-side check is the only thing that can, which is why one
// exists below despite describing the environment rather than the defect.
//
// A capability probe would be preferable and is not possible here. The Wine
// blocker is not a missing feature that init can test - measured 2026-07-29,
// supply a real dxilconv.dll and the probe reports "available", D3D12
// initialises fully (SM 6.6, ROV, binding tier 3, tiled tier 4, swap chain with
// tearing), and the title still never renders. vkd3d-proton's dxil-spirv cannot
// digest the DXIL dxilconv produces from xenia's hand-built DXBC transfer pixel
// shaders: Proton 10.0 aborts on assert "arg != 0" (dxil-spirv/ir.hpp:113);
// Proton Experimental 11.0 has that assert fixed and instead deadlocks at the
// same shader with every thread at 0% CPU. A deadlock seconds into real draws
// cannot be probed at init, and cannot be probed safely at all.
//
// This is cheap to get wrong in the Vulkan direction and expensive to get wrong
// in the D3D12 direction: defaulting case 4 to D3D12 hangs the emulator, while
// defaulting it to Vulkan costs image quality but still runs.
//
// CORRECTED 2026-08-02, after the first native-Windows measurements this
// project has ever had. Two earlier claims here were wrong:
//
//   "D3D12 is ~2x Vulkan on demanding titles" - FALSE. Measured on Halo Reach,
//   RTX 3060, native Windows, both backends warm: Vulkan 30.0 fps mean with 0%
//   of samples dipping, D3D12 29.5 with 4%. Fable II, Viva Pinata and Zuma
//   agree - every title sits at its own native rate on both backends. There is
//   no performance argument for D3D12 at all.
//
//   "Defaulting case 4 to Vulkan costs approximately nothing" - FALSE, and it
//   was measured with the wrong instrument. Frame rate was never the axis.
//   D3D12 renders geometry and effects that Vulkan consistently DROPS, across
//   every title tested and on upstream standalone xenia with this core removed
//   entirely. The two backends run different render-target emulation paths
//   (Vulkan fbo/fsi, D3D12 rtv/rov) and the Vulkan one loses render-to-texture
//   work. It fails silently: nothing is logged, the frame rate is unaffected,
//   the picture is just missing pieces.
//
// So the preference for D3D12 on Windows stands, but for correctness rather
// than speed - which is the stronger reason. And case 4 is a real quality
// penalty for Wine/Proton users, not the free choice this comment used to
// claim. That does not reopen the D3D12-under-Proton track: the blocker is
// still downstream in vkd3d-proton and still not ours to fix. It does mean the
// cost of that closure is higher than recorded, and it belongs in the docs.
//
// Not permanent, and not a ban: the bug is upstream in vkd3d-proton and already
// partly fixed between Proton 10 and 11. xenia_gpu_backend=d3d12 still forces
// it, which is how this gets re-tested as vkd3d improves. When it is fixed,
// the right replacement is a vkd3d-proton version check rather than a host
// check, so the default self-unlocks.
static const char* d3d12_auto_unusable_reason() {
    if (primary_gpu_is_amd()) {
        return "xenia's D3D12 backend is broken on AMD (standalone xenia has "
               "the same breakage)";
    }
    // Context 4. Keyed on the host because cases 3 and 4 are indistinguishable
    // any other way, not because Wine is unsupported.
    if (running_under_wine()) {
        return "vkd3d-proton cannot translate the DXIL that dxilconv produces "
               "for xenia's transfer pixel shaders (asserts on Proton 10, "
               "deadlocks on 11); Vulkan already runs at full speed on Linux";
    }
    return nullptr;
}
#endif

static void apply_core_options(void) {
    const char *v;

    // =================================================================
    // Graphics
    // =================================================================

    // Render target path. "auto" leaves the cvar alone so the base config and
    // any per-title config decide - see opt_is_auto().
    if ((v = opt_get(XENIA_OPT_RENDER_TARGET_PATH)) && !opt_is_auto(v)) {
        cvars::render_target_path = v;
    }

    // Draw resolution scale (uniform X+Y, restart required)
    if ((v = opt_get(XENIA_OPT_DRAW_RESOLUTION_SCALE)) && !opt_is_auto(v)) {
        int scale = atoi(v);
        if (scale >= 1 && scale <= 8) {
            cvars::draw_resolution_scale_x = scale;
            cvars::draw_resolution_scale_y = scale;
        }
    }

    // Anisotropic filtering override
    if ((v = opt_get(XENIA_OPT_ANISOTROPIC_FILTERING)) && !opt_is_auto(v)) {
        cvars::anisotropic_override = atoi(v);
    }

    // Async shader compilation
    if ((v = opt_get(XENIA_OPT_ASYNC_SHADERS)) && !opt_is_auto(v)) {
        cvars::async_shader_compilation = (strcmp(v, "enabled") == 0);
    }

    // Readback resolve. "auto" defers to the config, which is how a single
    // title can run "none" (much faster where it works - Fable II went from
    // 22 to 30 guest fps) while others keep the safer default.
    if ((v = opt_get(XENIA_OPT_READBACK_RESOLVE)) && !opt_is_auto(v)) {
        cvars::readback_resolve = v;
    }

    // Store shaders
    if ((v = opt_get(XENIA_OPT_STORE_SHADERS)) && !opt_is_auto(v)) {
        cvars::store_shaders = (strcmp(v, "enabled") == 0);
    }

    // Half-pixel offset
    if ((v = opt_get(XENIA_OPT_HALF_PIXEL_OFFSET)) && !opt_is_auto(v)) {
        cvars::half_pixel_offset = (strcmp(v, "enabled") == 0);
    }

    // GPU allow invalid fetch constants
    if ((v = opt_get(XENIA_OPT_GPU_INVALID_FETCH)) && !opt_is_auto(v)) {
        cvars::gpu_allow_invalid_fetch_constants = (strcmp(v, "enabled") == 0);
    }

    // Fuzzy alpha epsilon (NVIDIA fix)
    if ((v = opt_get(XENIA_OPT_FUZZY_ALPHA_EPSILON)) && !opt_is_auto(v)) {
        cvars::use_fuzzy_alpha_epsilon = (strcmp(v, "enabled") == 0);
    }

    // VSync / guest frame limiter
    if ((v = opt_get(XENIA_OPT_VSYNC))) {
        core_state.vsync_enabled = (strcmp(v, "enabled") == 0);
        SetGuestDisplayRefreshCap(core_state.vsync_enabled);
    }

    // Host framerate limit
    if ((v = opt_get(XENIA_OPT_FRAMERATE_LIMIT)) && !opt_is_auto(v)) {
        uint64_t limit = (uint64_t)atoi(v);
        cvars::framerate_limit = limit;
    }

    // PAL 50Hz mode. Unlike the other deferring options this one is mirrored
    // into core state, because pal_mode picks the rate declared in av_info and
    // the rate the software pacer targets. On "auto" the cvar is left for the
    // config to decide - but pal_mode still has to follow it, or the config
    // could select 50Hz while the frontend was told 60 and the pacer and the
    // guest would disagree for the whole session. So read it back rather than
    // skipping the mirror.
    if ((v = opt_get(XENIA_OPT_50HZ_MODE))) {
        if (!opt_is_auto(v)) {
            cvars::use_50Hz_mode = (strcmp(v, "enabled") == 0);
        }
        core_state.pal_mode = cvars::use_50Hz_mode;
    }

    // =================================================================
    // Video
    // =================================================================

    // Internal display resolution (restart required)
    if ((v = opt_get(XENIA_OPT_INTERNAL_DISPLAY_RES)) && !opt_is_auto(v)) {
        cvars::internal_display_resolution = (uint32_t)atoi(v);
    }

    // Widescreen
    if ((v = opt_get(XENIA_OPT_WIDESCREEN)) && !opt_is_auto(v)) {
        cvars::widescreen = (strcmp(v, "enabled") == 0);
    }

    // Video standard (1=NTSC, 2=NTSC-J, 3=PAL)
    if ((v = opt_get(XENIA_OPT_VIDEO_STANDARD)) && !opt_is_auto(v)) {
        cvars::video_standard = atoi(v);
    }

    // Display gamma type (0=linear, 1=sRGB, 2=BT.709)
    if ((v = opt_get(XENIA_OPT_DISPLAY_GAMMA)) && !opt_is_auto(v)) {
        cvars::kernel_display_gamma_type = (uint32_t)atoi(v);
    }

    // AV pack - which cable the console claims, which gates the video modes a
    // title will offer (restart required)
    if ((v = opt_get(XENIA_OPT_AVPACK)) && !opt_is_auto(v)) {
        cvars::avpack = atoi(v);
    }

    // =================================================================
    // Audio
    // =================================================================

    // Audio enabled (controls update_audio)
    if ((v = opt_get(XENIA_OPT_AUDIO_ENABLED)))
        core_state.audio_enabled = (strcmp(v, "enabled") == 0);

    // Master volume. Deliberately applied BEFORE mute so mute stays an
    // override rather than the two fighting over the same value.
    if ((v = opt_get(XENIA_OPT_VOLUME)) && !opt_is_auto(v)) {
        cvars::volume = (uint32_t)atoi(v);
    }

    // Mute (session-only volume; upstream removed the mute cvar). Restores to
    // the configured volume rather than a hardcoded 100, or turning mute off
    // would quietly discard the user's volume setting.
    if ((v = opt_get(XENIA_OPT_MUTE))) {
        xe::apu::SetVolume((strcmp(v, "enabled") == 0) ? 0
                                                       : (int)cvars::volume);
    }

    // Audio buffering depth (restart required)
    if ((v = opt_get(XENIA_OPT_APU_QUEUED_FRAMES)) && !opt_is_auto(v)) {
        cvars::apu_max_queued_frames = (uint32_t)atoi(v);
    }

    // XMA decoder (restart required)
    if ((v = opt_get(XENIA_OPT_XMA_DECODER)) && !opt_is_auto(v)) {
        cvars::xma_decoder = v;
    }

    // Dedicated XMA thread
    if ((v = opt_get(XENIA_OPT_DEDICATED_XMA_THREAD)) && !opt_is_auto(v)) {
        cvars::use_dedicated_xma_thread = (strcmp(v, "enabled") == 0);
    }

    // Enable XMP (music player)
    if ((v = opt_get(XENIA_OPT_ENABLE_XMP)) && !opt_is_auto(v)) {
        cvars::enable_xmp = (strcmp(v, "enabled") == 0);
    }

    // XMP default volume
    if ((v = opt_get(XENIA_OPT_XMP_DEFAULT_VOLUME)) && !opt_is_auto(v)) {
        cvars::xmp_default_volume = atoi(v);
    }

    // =================================================================
    // Emulation
    // =================================================================

    // Time scalar
    if ((v = opt_get(XENIA_OPT_TIME_SCALAR)) && !opt_is_auto(v)) {
        double scalar = atof(v);
        if (scalar > 0.0) {
            xe::Clock::set_guest_time_scalar(scalar);
        }
    }

    // Title updates
    if ((v = opt_get(XENIA_OPT_TITLE_UPDATES)) && !opt_is_auto(v)) {
        cvars::apply_title_update = (strcmp(v, "enabled") == 0);
    }

    // Stick deadzones. Xenia stores these as a 0..1 fraction and only applies
    // them when strictly between 0 and 1, so 0 means "no deadzone" - which is
    // the default and the right answer for a pad that is not worn.
    if ((v = opt_get(XENIA_OPT_LSTICK_DEADZONE)) && !opt_is_auto(v)) {
        xe::hid::cvars::left_stick_deadzone_percentage = atoi(v) / 100.0;
    }
    if ((v = opt_get(XENIA_OPT_RSTICK_DEADZONE)) && !opt_is_auto(v)) {
        xe::hid::cvars::right_stick_deadzone_percentage = atoi(v) / 100.0;
    }

    // Controller vibration. Defined inside xe::hid like the deadzones above.
    if ((v = opt_get(XENIA_OPT_VIBRATION)) && !opt_is_auto(v)) {
        xe::hid::cvars::vibration = (strcmp(v, "enabled") == 0);
    }

    // Apply game patches
    // One control, three states. "disabled" stops patch files being read at
    // all - which is how you turn everything off without deleting files -
    // while the other two differ only in what switches on the individual
    // patches inside a file. Kept on the original option key so existing
    // configs saying "enabled" or "disabled" keep working unchanged.
    if ((v = opt_get(XENIA_OPT_APPLY_PATCHES)) && !opt_is_auto(v)) {
        const bool whole_file = (strcmp(v, "whole_file") == 0);
        cvars::apply_patches = whole_file || (strcmp(v, "enabled") == 0);
        cvars::patch_all_in_file = whole_file;
    }

    // License mask (0=None, 1=Full, -1=All)
    if ((v = opt_get(XENIA_OPT_LICENSE_MASK)) && !opt_is_auto(v)) {
        cvars::license_mask = atoi(v);
    }

    // Headless message-box response (which button to auto-pick; -1 = default)
    if ((v = opt_get(XENIA_OPT_MSGBOX_BUTTON)) && !opt_is_auto(v)) {
        cvars::headless_messagebox_button = atoi(v);
    }

    // User language (upstream now uses numeric XConfig language IDs)
    if ((v = opt_get(XENIA_OPT_USER_LANGUAGE)) && !opt_is_auto(v)) {
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
    if ((v = opt_get(XENIA_OPT_USER_COUNTRY)) && !opt_is_auto(v)) {
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
    if ((v = opt_get(XENIA_OPT_PROTECT_ZERO)) && !opt_is_auto(v)) {
        cvars::protect_zero = (strcmp(v, "enabled") == 0);
    }

    // Clear GPU memory page state
    if ((v = opt_get(XENIA_OPT_CLEAR_MEMORY_PAGE)) && !opt_is_auto(v)) {
        cvars::clear_memory_page_state = (strcmp(v, "enabled") == 0);
    }

    // Disable context promotion
    if ((v = opt_get(XENIA_OPT_DISABLE_CTX_PROMOTION)) && !opt_is_auto(v)) {
        cvars::disable_context_promotion = (strcmp(v, "enabled") == 0);
    }

    // Mount cache partition
    if ((v = opt_get(XENIA_OPT_MOUNT_CACHE)) && !opt_is_auto(v)) {
        cvars::mount_cache = (strcmp(v, "enabled") == 0);
    }

    // Mount scratch partition
    if ((v = opt_get(XENIA_OPT_MOUNT_SCRATCH)) && !opt_is_auto(v)) {
        cvars::mount_scratch = (strcmp(v, "enabled") == 0);
    }

    // Allow a title update whose signature does not match the game
    if ((v = opt_get(XENIA_OPT_INCOMPATIBLE_TU)) && !opt_is_auto(v)) {
        cvars::allow_incompatible_title_update = (strcmp(v, "enabled") == 0);
    }


    // =================================================================
    // Backend tuning
    //
    // Each of these is on by default in xenia because it is the faster or more
    // capable path. They are exposed to be turned OFF: they are the first
    // things to rule out when a backend will not start or renders wrongly on a
    // particular driver, and without options that means editing a TOML.
    // =================================================================

    if ((v = opt_get(XENIA_OPT_VK_SPARSE_MEMORY)) && !opt_is_auto(v)) {
        cvars::vulkan_sparse_shared_memory = (strcmp(v, "enabled") == 0);
    }

    if ((v = opt_get(XENIA_OPT_TILED_SHARED_MEMORY)) && !opt_is_auto(v)) {
        cvars::tiled_shared_memory = (strcmp(v, "enabled") == 0);
    }

#ifdef _WIN32
    if ((v = opt_get(XENIA_OPT_D3D12_BINDLESS)) && !opt_is_auto(v)) {
        cvars::d3d12_bindless = (strcmp(v, "enabled") == 0);
    }
#endif

    if ((v = opt_get(XENIA_OPT_READBACK_SYNC)) && !opt_is_auto(v)) {
        cvars::readback_resolve_sync = (strcmp(v, "enabled") == 0);
    }

    // =================================================================
    // Debug
    // =================================================================

    // Log level (0=error, 1=warning, 2=info, 3=debug)
    if ((v = opt_get(XENIA_OPT_LOG_LEVEL)) && !opt_is_auto(v)) {
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
    if (!audio_mixer || !core_state.audio_buffer) return;

    // Drain even when audio output is disabled, discarding what comes out.
    // The drain is what credits each client's semaphore, so skipping it
    // starves the guest's audio worker of slots and it stops calling the
    // title's audio callback altogether.
    const bool deliver =
        core_state.audio_enabled && core_state.audio_batch_cb != nullptr;

    // Drain audio, capped at 2?? real-time (3200 samples max for 48kHz@60fps).
    constexpr size_t kChunkSamples = 1600;
    constexpr size_t kMaxDrain     = 3200;  // 2?? real-time
    size_t total = 0;

    while (total < kMaxDrain) {
        size_t want = kMaxDrain - total;
        if (want > kChunkSamples) want = kChunkSamples;
        size_t got = audio_mixer->Pop(core_state.audio_buffer, want);
        if (got == 0) break;
        if (deliver) {
            core_state.audio_batch_cb(core_state.audio_buffer, got / 2);
        }
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
    if (audio_mixer && core_state.audio_buffer) {
        for (int i = 0; i < 64; i++) {
            if (!audio_mixer->Pop(core_state.audio_buffer, 1600)) break;
        }
    }

    last_frame_w = splash_w;
    last_frame_h = splash_h;
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

    // Pace to exactly the rate advertised in retro_get_system_av_info. This
    // used to be hardcoded to 59.94 Hz while av_info declared 60.0, so the
    // pacer and the frontend's audio sync disagreed by ~17 us per frame and
    // slowly walked apart until something had to give - a periodic hitch with
    // no obvious cause. Derive both from one place so they cannot drift again.
    const double target_fps = core_state.pal_mode ? 50.0 : 60.0;
    const auto frame_period = std::chrono::nanoseconds(
        int64_t(1000000000.0 / target_fps + 0.5));

    auto now = clock::now();
    if (now < next) {
        // A plain sleep_until, deliberately. A yield-spin on the last
        // millisecond or so was tried here to guard against coarse OS timer
        // granularity under Wine, and measured worse: host frame rate fell
        // from a pinned 60.0 to 47.2 fps on Viva Pinata, because the spin
        // takes CPU away from an emulator that wants all of it. Plain
        // sleep_until was already holding exactly 60.0, so there was no
        // granularity problem to solve.
        std::this_thread::sleep_until(next);
        now = clock::now();
    }

    next += frame_period;
    // Allow a bounded amount of catch-up. Clamping straight to `now` was also
    // measured worse for the same reason - with no credit for a frame that ran
    // long, every overrun is absorbed into the next deadline and the average
    // rate sags below target. Letting the deadline trail by a couple of frames
    // keeps the long-run average honest, while still refusing the unbounded
    // backlog the old 100 ms clamp permitted, where one hitch was followed by a
    // burst of frames delivered as fast as they could be produced.
    const auto max_lag = frame_period * 2;
    if (next < now - max_lag) {
        next = now - max_lag;
    }
}

// Tell the frontend the guest's output size and display aspect when either
// changes.
//
// retro_get_system_av_info can only answer once, at load, and it has to answer
// before the guest has booted far enough to have configured its scaler - so it
// declares 1280x720 16:9 and would otherwise stay there for the whole session.
// 360 titles are not all 720p16:9: 4:3 titles exist, sub-HD framebuffers are
// common (the console's scaler hides it on real hardware), and a title may
// change mode at runtime. Without this the frontend has no way to know, and
// anything not matching the declared ratio is silently stretched.
//
// The ratio deliberately does NOT come from the frame's own dimensions. The
// 360 scales its framebuffer to the display mode in hardware, so a 1024x600
// buffer can be a 16:9 picture and dividing width by height would give 1.71 and
// a subtly wrong image. xenia already models this: the guest declares its
// scaled output through VdInitializeScalerCommandBuffer, and
// CalculateScaledAspectRatio turns that into the ratio the picture would have
// on a physical TV, honouring the console's widescreen setting. That is the
// number the frontend wants.
//
// Must be called from retro_run - the environment call requires it.
static void report_geometry(uint32_t w, uint32_t h) {
    if (!w || !h) return;

    uint32_t aspect_x = 0, aspect_y = 0;
    if (lr_graphics) {
        auto aspect = lr_graphics->GetScaledAspectRatio();
        aspect_x = aspect.first;
        aspect_y = aspect.second;
    }

    // Nothing moved - and this is the common case, every frame of a stable
    // title, so it stays ahead of the environment call.
    if (w == last_geometry_w && h == last_geometry_h &&
        aspect_x == last_geometry_aspect_x &&
        aspect_y == last_geometry_aspect_y) {
        return;
    }

    // base must fit inside the max declared in av_info, which cannot be raised
    // without a full SET_SYSTEM_AV_INFO reinit. Resolution scaling can push the
    // guest output past 4K (up to 7x per axis), so clamp rather than send an
    // out-of-range geometry. The frame itself is still delivered at its real
    // size; only the nominal geometry is capped.
    uint32_t base_w = std::min(w, kMaxGeometryWidth);
    uint32_t base_h = std::min(h, kMaxGeometryHeight);
    if (base_w != w || base_h != h) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            xenia_log(RETRO_LOG_WARN,
                      "Guest output %ux%u exceeds the declared maximum %ux%u; "
                      "reporting clamped geometry. Lower the resolution scale "
                      "if the picture is wrong.\n",
                      w, h, kMaxGeometryWidth, kMaxGeometryHeight);
        }
    }

    struct retro_game_geometry geom = {};
    geom.base_width  = base_w;
    geom.base_height = base_h;
    // max_* are documented as ignored by SET_GEOMETRY; filled in for clarity.
    geom.max_width   = kMaxGeometryWidth;
    geom.max_height  = kMaxGeometryHeight;
    // 0 tells the frontend to derive the ratio from base_width/base_height.
    // Close to unreachable in practice, and deliberately kept anyway: the
    // presenter treats a zero aspect as "guest output inactive"
    // (GuestOutputProperties::IsActive) and hands out no image, so having a
    // frame at all implies a non-zero aspect was set when it was refreshed.
    // The read here is of the current value rather than the one attached to
    // this frame, so a mode change between the two could still show a zero,
    // and square pixels beat a ratio we invented.
    geom.aspect_ratio = (aspect_x && aspect_y)
                            ? float(aspect_x) / float(aspect_y)
                            : 0.0f;

    if (!core_state.environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &geom)) {
        // Old or minimal frontends may not have it. Nothing to fall back to -
        // the declared av_info geometry stands - so say so once and stop
        // trying, rather than repeating the call on every mode change.
        static bool unsupported_logged = false;
        if (!unsupported_logged) {
            unsupported_logged = true;
            xenia_log(RETRO_LOG_INFO,
                      "Frontend: SET_GEOMETRY unsupported; geometry stays at "
                      "the values declared in av_info\n");
        }
        return;
    }

    xenia_log(RETRO_LOG_INFO, "Geometry: %ux%u, display aspect %u:%u (%.4f)\n",
              base_w, base_h, aspect_x, aspect_y,
              geom.aspect_ratio ? geom.aspect_ratio
                                : float(base_w) / float(base_h));

    last_geometry_w = w;
    last_geometry_h = h;
    last_geometry_aspect_x = aspect_x;
    last_geometry_aspect_y = aspect_y;
}

static void update_video(void) {
    // Skip the whole capture when the frontend has told us the frame will not
    // be shown. Fast-forward drops most frames on the floor, and video can be
    // disabled outright; either way the readback, the blit and the upload are
    // pure waste. Passing NULL repeats the previous frame, which is what a
    // frontend expects for a dropped frame - only valid if it advertised
    // GET_CAN_DUPE, so fall through to a real capture when it did not.
    if (g_can_dupe) {
        bool ff = false;
        if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &ff) &&
            ff) {
            emit_dupe_frame();
            return;
        }
        int av_enable = 0;
        if (core_state.environ_cb(RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE,
                                  &av_enable) &&
            !(av_enable & 1)) {  // bit 0 = video enabled
            emit_dupe_frame();
            return;
        }
    }

    // Capture the guest frame with the presenter's persistent-resource GPU
    // blit (the same path the HW-render present uses). The old
    // CaptureGuestOutput allocated a Vulkan readback buffer + command pool and
    // did a synchronous GPU wait EVERY frame, which serialized the GPU and
    // collapsed throughput on demanding titles (single-digit fps). The blit
    // path reuses resources across frames and returns a mapped R8G8B8A8 buffer.
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    bool got = false;
    bool is_bgra = false;
    if (lr_graphics && lr_graphics->presenter()) {
        if (strcmp(core_state.graphics_backend, "vulkan") == 0) {
            got = libretro_vk_capture_gpu_blit(lr_graphics->presenter(),
                                               blit_data, w, h, is_bgra);
        }
#ifdef _WIN32
        else {
            got = libretro_d3d12_capture_gpu_blit(lr_graphics->presenter(),
                                                  blit_data, w, h, is_bgra);
        }
#endif
    }

    if (got && blit_data && w > 0 && h > 0) {
        report_geometry(w, h);
        if (is_bgra) {
            // The capture already produced XRGB8888, so hand the mapped
            // readback straight to the frontend. This is the normal path: it
            // skips a whole-frame scalar channel swap - 921,600 iterations at
            // 720p, every single frame - that used to sit between the GPU and
            // the frontend for no reason, because the blit was converting
            // formats anyway and could simply convert to the right one.
            last_frame_w = w;
            last_frame_h = h;
            core_state.video_cb(blit_data, w, h, w * 4);
            return;
        }
        // Fallback only, for a device that cannot blit into B8G8R8A8:
        // R8G8B8A8 -> XRGB8888 (little-endian 0x00RRGGBB, B first).
        size_t count = static_cast<size_t>(w) * h;
        if (sw_frame_buf.size() < count) sw_frame_buf.resize(count);
        const uint32_t* src = static_cast<const uint32_t*>(blit_data);
        uint32_t* dst = sw_frame_buf.data();
        for (size_t i = 0; i < count; i++) {
            uint32_t v = src[i];
            dst[i] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) |
                     ((v & 0x000000FFu) << 16);
        }
        last_frame_w = w;
        last_frame_h = h;
        core_state.video_cb(dst, w, h, w * 4);
        return;
    }

    // Fallback: repeat the previous frame rather than hang the frontend. Same
    // rule as every other NULL frame - the size must match the buffer the
    // frontend is still holding, not a literal.
    emit_dupe_frame();
}

static void update_video_vulkan(void) {
    if (!vulkan_hw_render_active || !vk_hw) {
        update_video();
        return;
    }

    // Step 1: GPU blit capture (A2B10G10R10 ??? R8G8B8A8)
    const void* blit_data = nullptr;
    uint32_t w = 0, h = 0;
    bool is_bgra = false;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_vk_capture_gpu_blit(lr_graphics->presenter(),
                                       blit_data, w, h, is_bgra) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }
    report_geometry(w, h);

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
        if (!vk_create_frame(f, w, h, is_bgra)) {
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
    vk_current_image.create_info.format =
        is_bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
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
    bool is_bgra = false;
    if (!lr_graphics || !lr_graphics->presenter() ||
        !libretro_d3d12_capture_gpu_blit(lr_graphics->presenter(),
                                          blit_data, w, h, is_bgra) ||
        !blit_data || w == 0 || h == 0) {
        core_state.video_cb(RETRO_HW_FRAME_BUFFER_VALID, 0, 0, 0);
        return;
    }
    report_geometry(w, h);

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
                audio_mixer = sys->mixer();
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
    audio_mixer = nullptr;
    lr_input_driver = nullptr;
    lr_graphics = nullptr;
    game_loaded = false;
    last_geometry_w = last_geometry_h = 0;
    last_geometry_aspect_x = last_geometry_aspect_y = 0;
    // Back to the seed, so a dupe issued before the next title's first frame
    // cannot describe the previous title's buffer.
    last_frame_w = 1280;
    last_frame_h = 720;

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

    // Frontend capability probes. Both degrade silently: the fallback is
    // exactly what the core did before, so RetroArch 1.7.5 is unaffected.
    //
    // A capability, once advertised, LATCHES. retro_set_environment is called
    // more than once, and not every call carries a callback that answers these
    // queries. Measured on RetroArch 1.22.2, 2026-08-02: the first call answers
    // both (the frontend's own trace logs "GET_CAN_DUPE: true" beside it), then
    // two later calls - after video/audio/display init - answer neither and log
    // no [Environ] trace at all, because the callback in force there refuses
    // them. Plain assignment made the last call win, so both flags ended up
    // false for the entire session.
    //
    // That silently disabled two of the things this core advertises: the
    // bitmask input path never ran, and update_video's fast-forward/video-off
    // skip never ran. Both fail closed, so nothing looked broken - the core
    // just quietly did the slower thing everywhere.
    //
    // Latching is the safe direction. A frontend does not revoke these
    // mid-session, and the fallback for a false negative costs performance
    // while a false positive would cost correctness - so only ever upgrade.
    {
        const bool had_bitmasks = g_input_bitmasks;
        const bool had_dupe     = g_can_dupe;

        // GET_INPUT_BITMASKS and GET_CAN_DUPE have OPPOSITE contracts, and
        // treating them the same way is what silently disabled bitmasks on
        // every frontend before c1bdc2093:
        //
        //   GET_INPUT_BITMASKS - "@param data Ignored." The RETURN VALUE is
        //       the answer. Testing a bool the frontend never writes leaves it
        //       false forever, so the bitmask path had never once run anywhere.
        //   GET_CAN_DUPE       - "@param[out] data bool*." The return value
        //       only says the call exists; the answer is written into data.
        //       So this one genuinely does need both.
        if (cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL)) {
            g_input_bitmasks = true;
        }

        bool flag = false;
        if (cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &flag) && flag) {
            g_can_dupe = true;
        }

        // Log the first probe unconditionally, then only on change. Logging
        // purely on change would go silent on a frontend that supports
        // neither - RetroArch 1.7.5 answers no to both, nothing ever moves off
        // the initial false, and we would lose the diagnostic exactly where it
        // is most worth having. After that, staying quiet stops the repeat
        // calls emitting contradictory lines; three lines disagreeing is what
        // made this bug read as noise for a whole session.
        static bool logged_once = false;
        if (!logged_once || g_input_bitmasks != had_bitmasks ||
            g_can_dupe != had_dupe) {
            logged_once = true;
            xenia_log(RETRO_LOG_INFO,
                      "Frontend: input bitmasks %s, frame duping %s\n",
                      g_input_bitmasks ? "yes" : "no",
                      g_can_dupe ? "yes" : "no");
        }
    }

    // Tell the frontend this core is demanding, so it can size its own
    // buffering sensibly rather than assuming a light 2D core.
    {
        unsigned level = 15;
        cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
    }

    // Take the guest's language from the frontend when the user has not pinned
    // one. Xenia's user_language is the 360's own enum and happens to match the
    // order libretro uses for the languages the 360 shipped with, so the common
    // ones map directly; anything else falls back to English rather than
    // guessing. Only a default - the core option still wins.
    {
        unsigned lang = 0;
        if (cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &lang)) {
            static const struct { unsigned retro; int x360; } kLangMap[] = {
                { RETRO_LANGUAGE_ENGLISH,             1 },
                { RETRO_LANGUAGE_JAPANESE,            2 },
                { RETRO_LANGUAGE_GERMAN,              3 },
                { RETRO_LANGUAGE_FRENCH,              4 },
                { RETRO_LANGUAGE_SPANISH,             5 },
                { RETRO_LANGUAGE_ITALIAN,             6 },
                { RETRO_LANGUAGE_KOREAN,              7 },
                { RETRO_LANGUAGE_CHINESE_TRADITIONAL, 8 },
                { RETRO_LANGUAGE_PORTUGUESE_BRAZIL,   9 },
                { RETRO_LANGUAGE_PORTUGUESE_PORTUGAL, 9 },
                { RETRO_LANGUAGE_POLISH,             11 },
                { RETRO_LANGUAGE_RUSSIAN,            12 },
                { RETRO_LANGUAGE_SWEDISH,            13 },
                { RETRO_LANGUAGE_TURKISH,            14 },
                { RETRO_LANGUAGE_NORWEGIAN,          15 },
                { RETRO_LANGUAGE_DUTCH,              16 },
                { RETRO_LANGUAGE_CHINESE_SIMPLIFIED, 17 },
            };
            for (const auto& m : kLangMap) {
                if (m.retro == lang) {
                    cvars::user_language = m.x360;
                    break;
                }
            }
        }
    }

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
    info->library_version  = "0.2.1";
    info->need_fullpath    = true;
    info->valid_extensions = "iso|xex|zar|xcp|x360";
    info->block_extract    = false;
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info) {
    memset(info, 0, sizeof(*info));
    // The opening guess only. It has to be answered before the guest has run,
    // so the real values are unknowable here - 720p 16:9 is the most common
    // 360 mode and therefore the cheapest wrong answer. report_geometry
    // corrects it from the first guest frame onwards.
    info->geometry.base_width   = 1280;
    info->geometry.base_height  = 720;
    info->geometry.max_width    = kMaxGeometryWidth;
    info->geometry.max_height   = kMaxGeometryHeight;
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
        // settings unreachable. Per-game core options do exist on 1.7.5 (see
        // opt_is_auto), but they reach only the ~38 options we expose and a
        // .opt replaces the global file wholesale, so this is the mechanism
        // that can actually carry per-title settings.
        // Report this through the core's own logger rather than relying on
        // xenia's. This runs before the emulator exists, so xenia's logging is
        // not up yet and its XELOGI lines about the config are dropped. Note
        // LoadGameConfigForFile returns the title id even when no config file
        // was found, so the return value alone says nothing about whether any
        // override was applied - check for the file explicitly.
        uint32_t cfg_title_id =
            config::LoadGameConfigForFile(core_state.game_path);
        if (!cfg_title_id) {
            xenia_log(RETRO_LOG_INFO,
                      "No title id for %s; per-title config skipped\n",
                      core_state.game_path);
        } else {
            char tid[16];
            snprintf(tid, sizeof(tid), "%08X", cfg_title_id);
            std::filesystem::path per_title =
                cfg_dir / "config" / (std::string(tid) + ".config.toml");
            std::error_code pt_ec;
            if (std::filesystem::exists(per_title, pt_ec)) {
                xenia_log(RETRO_LOG_INFO,
                          "Applied per-title config for %s: %s\n", tid,
                          per_title.string().c_str());
            } else {
                xenia_log(RETRO_LOG_INFO,
                          "Title %s has no per-title config (looked for %s)\n",
                          tid, per_title.string().c_str());
            }
        }
    }

    // Apply any options set before load
    apply_core_options();

    // Report what the settings actually resolved to.
    //
    // Three layers feed these: xenia's built-in defaults, the config files
    // (base then per-title), and core options - and only the last of those is
    // visible in the frontend log. That has repeatedly cost real time: a config
    // value silently overwritten by a core option looks identical to a config
    // that was never read, and a setting that changed nothing looks identical
    // to one that was ignored. Printing the resolved values once removes the
    // guesswork from every future "did that take effect?".
    // The graphics backend is deliberately absent here: it is not chosen until
    // after this point, and is logged by the selection code itself.
    xenia_log(RETRO_LOG_INFO, "Effective settings:\n");
    xenia_log(RETRO_LOG_INFO, "  render_target_path = %s\n",
              cvars::render_target_path.c_str());
    xenia_log(RETRO_LOG_INFO, "  readback_resolve = %s\n",
              cvars::readback_resolve.c_str());
    xenia_log(RETRO_LOG_INFO, "  draw_resolution_scale = %dx%d\n",
              cvars::draw_resolution_scale_x, cvars::draw_resolution_scale_y);
    xenia_log(RETRO_LOG_INFO, "  texture_cache_rt_limit = %u MB\n",
              cvars::texture_cache_memory_limit_render_to_texture);
    xenia_log(RETRO_LOG_INFO,
              "  async_shaders = %s, store_shaders = %s, "
              "clear_memory_page_state = %s\n",
              cvars::async_shader_compilation ? "on" : "off",
              cvars::store_shaders ? "on" : "off",
              cvars::clear_memory_page_state ? "on" : "off");
    xenia_log(RETRO_LOG_INFO,
              "  half_pixel_offset = %s, invalid_fetch = %s, "
              "fuzzy_alpha = %s\n",
              cvars::half_pixel_offset ? "on" : "off",
              cvars::gpu_allow_invalid_fetch_constants ? "on" : "off",
              cvars::use_fuzzy_alpha_epsilon ? "on" : "off");
    xenia_log(RETRO_LOG_INFO,
              "  cpu: elide_e0=%s inline_loadclock=%s rmw_merge=%s "
              "align_blocks=%u ignore_thread_prio=%s\n",
              cvars::elide_e0_check ? "on" : "off",
              cvars::inline_loadclock ? "on" : "off",
              cvars::enable_rmw_context_merging ? "on" : "off",
              cvars::align_all_basic_blocks,
              cvars::ignore_thread_priorities ? "on" : "off");
    xenia_log(RETRO_LOG_INFO, "  apply_patches = %s, framerate_limit = %u\n",
              cvars::apply_patches ? "on" : "off", cvars::framerate_limit);

    // Describe the graphics environment before choosing a backend, so a failed
    // choice can be diagnosed from the log rather than by bisecting settings.
#ifdef _WIN32
    log_graphics_environment();
#endif

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
        const char* no_d3d12 =
            d3d12_runtime_available() ? d3d12_auto_unusable_reason() : nullptr;
        if (no_d3d12) {
            xenia_log(RETRO_LOG_INFO,
                      "Defaulting to the Vulkan backend: %s\n", no_d3d12);
        }
        if (d3d12_runtime_available() && !no_d3d12) {
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
        // on hosts whose D3D12 implementation can't run it (AMD drivers,
        // vkd3d-proton), where Vulkan is the working choice.
#ifdef _WIN32
        const char* no_d3d12 = d3d12_auto_unusable_reason();
        if (no_d3d12) {
            xenia_log(RETRO_LOG_INFO,
                      "Defaulting to the Vulkan backend: %s\n", no_d3d12);
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
        std::unique_ptr<xe::ui::d3d12::D3D12Provider> d3d12_probe;
        if (!d3d12_runtime_available()) {
            d3d12_fail_reason = "D3D12/D3D12Core.dll was not found next to the "
                                "frontend";
        } else {
            d3d12_probe =
                xe::ui::d3d12::D3D12Provider::Create(/*fatal_on_failure=*/false);
            if (!d3d12_probe) {
                d3d12_fail_reason =
                    "the Direct3D 12 graphics subsystem failed to "
                    "initialize (see the xenia log for details)";
            } else {
                // Report the capabilities the provider actually came back with.
                // Initialization succeeding tells us very little on its own -
                // under Proton it succeeds and the title still dies at launch.
                // These are the properties xenia's D3D12 backend depends on, so
                // comparing this line between native Windows and vkd3d-proton
                // localises the difference instead of guessing at it.
                const uint16_t sm = d3d12_probe->GetHighestShaderModel();
                xenia_log(RETRO_LOG_INFO,
                          "D3D12 probe OK: %s, shader model %u.%u%s\n",
                          d3d12_probe->GetAdapterDescription().c_str(),
                          unsigned(sm >> 4), unsigned(sm & 0xF),
                          sm >= 0x66 ? "" : " (below the 6.6 xenia wants)");
                xenia_log(RETRO_LOG_INFO,
                          "D3D12 caps: ROV=%s, PS stencil ref=%s, "
                          "barycentrics=%s, tiled tier=%d, binding tier=%d\n",
                          d3d12_probe->AreRasterizerOrderedViewsSupported()
                              ? "yes" : "no",
                          d3d12_probe->IsPSSpecifiedStencilReferenceSupported()
                              ? "yes" : "no",
                          d3d12_probe->AreBarycentricsSupported() ? "yes" : "no",
                          int(d3d12_probe->GetTiledResourcesTier()),
                          int(d3d12_probe->GetResourceBindingTier()));

                // The caps above were where the Proton failure was hunted for,
                // and they say nothing - vkd3d-proton reports the same or
                // better than native. This is the line that matters. dxilconv
                // is an in-box Windows DLL with no redistributable source, and
                // without it the host render target path cannot build its
                // transfer pixel shaders, so the title dies at its first real
                // draws having logged nothing but a debug-level note. Treat a
                // missing converter as a failed probe unless the pixel shader
                // interlock path is both selected and supported, since that
                // path doesn't use the transfer shaders.
                const bool have_dxilconv =
                    d3d12_probe->IsDxbcConverterAvailable();
                const bool interlock_usable =
                    cvars::render_target_path == "accuracy" &&
                    d3d12_probe->AreRasterizerOrderedViewsSupported();
                xenia_log(RETRO_LOG_INFO,
                          "D3D12 dxilconv (DXBC->DXIL transfer shaders): %s\n",
                          have_dxilconv ? "available"
                                        : "MISSING - host render target path "
                                          "cannot work");
                if (!have_dxilconv) {
                    if (interlock_usable) {
                        xenia_log(RETRO_LOG_WARN,
                                  "Staying on D3D12 without dxilconv because "
                                  "render_target_path=accuracy uses the pixel "
                                  "shader interlock path, which does not need "
                                  "it\n");
                    } else {
                        d3d12_fail_reason =
                            "dxilconv.dll is unavailable, so the host render "
                            "target path cannot build its transfer shaders "
                            "(an in-box Windows component; absent under "
                            "Wine/Proton)";
                    }
                }
            }
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
    // Also update the gpu cvar so internal Xenia code stays consistent, and
    // name the audio system we actually install, since the emulator reports
    // both cvars as the active backends.
    cvars::gpu = core_state.graphics_backend;
    cvars::apu = "libretro";
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

// Frame pacing report.
//
// This core had no performance signal of any kind - not a frame time, not an
// fps figure - which made "it feels sluggish" impossible to act on and left
// tuning to guesswork. retro_run is called once per presented frontend frame,
// so the interval between calls is the real end-to-end frame time.
//
// Summarised every 5s rather than per frame to keep the log usable. Frames
// slower than 1.5x the title's own measured cadence are counted separately,
// because an average can look healthy while regular hitches make it feel bad.
static void report_frame_pacing(void) {
    using clock = std::chrono::steady_clock;
    static clock::time_point last_frame{};
    static clock::time_point window_start{};
    static double total_ms = 0.0;
    static double worst_ms = 0.0;
    static uint32_t frames = 0;
    static uint32_t slow_frames = 0;

    const clock::time_point now = clock::now();
    if (last_frame.time_since_epoch().count() == 0) {
        last_frame = window_start = now;
        return;
    }

    const double ms =
        std::chrono::duration<double, std::milli>(now - last_frame).count();
    last_frame = now;

    // A gap this large is a load, a pause or the frontend stalling, not a
    // rendered frame - it would swamp the averages, so restart the window.
    if (ms > 1000.0) {
        window_start = now;
        total_ms = worst_ms = 0.0;
        frames = slow_frames = 0;
        return;
    }

    const double display_ms = 1000.0 / (core_state.pal_mode ? 50.0 : 60.0);

    // What counts as "late" is the title's own cadence, not the display's. The
    // core renders synchronously inside retro_run, so a 30 Hz title paces the
    // host loop at ~33 ms - measuring that against 16.7 ms called every single
    // frame late (Fable II and Viva Pinata both reported "151 of 151"), which
    // is noise, not a diagnosis. Target the observed guest rate instead, taken
    // from the previous window so it is a measurement rather than a guess, and
    // never faster than the display can present nor slower than 15 fps, so a
    // stalled window can't relax the bar until it stops meaning anything.
    static double target_ms = 0.0;
    if (target_ms <= 0.0) target_ms = display_ms;

    total_ms += ms;
    frames++;
    if (ms > worst_ms) worst_ms = ms;
    if (ms > target_ms * 1.5) slow_frames++;

    // Guest present count, so the game's own rate can be told apart from the
    // host's. A 30 Hz title presenting 30 while the host paints 60 is correct
    // and healthy; the same title presenting 18 is the emulator falling behind.
    // Without both numbers "sluggish" is unattributable.
    static uint64_t last_guest_count = 0;
    uint64_t guest_count = 0;
    if (lr_graphics && lr_graphics->presenter()) {
        guest_count = lr_graphics->presenter()->guest_output_refresh_count();
    }

    const double window_s =
        std::chrono::duration<double>(now - window_start).count();
    if (window_s >= 5.0 && frames > 0) {
        const double avg_ms = total_ms / frames;
        const double host_fps = 1000.0 / avg_ms;
        if (guest_count >= last_guest_count) {
            const double guest_fps =
                double(guest_count - last_guest_count) / window_s;
            xenia_log(RETRO_LOG_INFO,
                      "Frame pacing: host %.1f fps (%.1f ms avg, worst %.1f "
                      "ms) | guest %.1f fps | %u of %u host frames over "
                      "%.1f ms\n",
                      host_fps, avg_ms, worst_ms, guest_fps, slow_frames,
                      frames, target_ms * 1.5);

            // Re-aim at what this title actually runs at, for the next window.
            // A window with no guest presents at all is a load screen or a
            // menu, not a cadence - keep the previous target rather than
            // relaxing the bar to nothing. Clamped to the display rate at one
            // end and 15 fps at the other so neither a fast nor a stalled
            // window can move the bar somewhere it stops meaning anything.
            if (guest_fps >= 1.0) {
                const double aim = 1000.0 / guest_fps;
                target_ms = aim < display_ms ? display_ms
                                             : (aim > 1000.0 / 15.0
                                                    ? 1000.0 / 15.0
                                                    : aim);
            }
        }
        last_guest_count = guest_count;
        window_start = now;
        total_ms = worst_ms = 0.0;
        frames = slow_frames = 0;
    }
}

RETRO_API void retro_run(void) {
    report_frame_pacing();

    // Poll input from the frontend
    if (core_state.input_poll_cb) core_state.input_poll_cb();

    // Feed libretro input state into Xenia's HID system
    if (lr_input_driver && core_state.input_state_cb)
        lr_input_driver->UpdateFromLibretro(core_state.input_state_cb,
                                            g_input_bitmasks);

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
            emit_dupe_frame();
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
