/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Core Options v2 Configuration
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#ifndef LIBRETRO_CORE_OPTIONS_H
#define LIBRETRO_CORE_OPTIONS_H

#include "libretro.h"

#include <stdlib.h>
#include <string.h>

// Forward declaration
struct xenia_core_state;

// Core option keys ??? Graphics
#define XENIA_OPT_RENDER_TARGET_PATH    "xenia_render_target_path"
#define XENIA_OPT_DRAW_RESOLUTION_SCALE "xenia_draw_resolution_scale"
#define XENIA_OPT_ANISOTROPIC_FILTERING "xenia_anisotropic_filtering"
#define XENIA_OPT_LSTICK_DEADZONE       "xenia_left_stick_deadzone"
#define XENIA_OPT_RSTICK_DEADZONE       "xenia_right_stick_deadzone"
#define XENIA_OPT_ASYNC_SHADERS         "xenia_async_shader_compilation"
#define XENIA_OPT_READBACK_RESOLVE      "xenia_readback_resolve"
#define XENIA_OPT_GPU_BACKEND           "xenia_gpu_backend"
#define XENIA_OPT_AUTO_PROFILE          "xenia_auto_profile"
#define XENIA_OPT_MSGBOX_BUTTON         "xenia_messagebox_button"
#define XENIA_OPT_BOOT_SPLASH           "xenia_boot_splash"
#define XENIA_OPT_STORE_SHADERS         "xenia_store_shaders"
#define XENIA_OPT_HALF_PIXEL_OFFSET     "xenia_half_pixel_offset"
#define XENIA_OPT_GPU_INVALID_FETCH     "xenia_gpu_allow_invalid_fetch_constants"
#define XENIA_OPT_FUZZY_ALPHA_EPSILON   "xenia_use_fuzzy_alpha_epsilon"
#define XENIA_OPT_VSYNC                 "xenia_vsync"
#define XENIA_OPT_FRAMERATE_LIMIT       "xenia_framerate_limit"
#define XENIA_OPT_50HZ_MODE            "xenia_50hz_mode"

// Core option keys ??? Video
#define XENIA_OPT_INTERNAL_DISPLAY_RES  "xenia_internal_display_resolution"
#define XENIA_OPT_WIDESCREEN            "xenia_widescreen"
#define XENIA_OPT_VIDEO_STANDARD        "xenia_video_standard"
#define XENIA_OPT_DISPLAY_GAMMA         "xenia_display_gamma"

// Core option keys ??? Audio
#define XENIA_OPT_AUDIO_ENABLED         "xenia_audio_enabled"
#define XENIA_OPT_MUTE                  "xenia_mute"
#define XENIA_OPT_XMA_DECODER           "xenia_xma_decoder"
#define XENIA_OPT_DEDICATED_XMA_THREAD  "xenia_dedicated_xma_thread"
#define XENIA_OPT_ENABLE_XMP            "xenia_enable_xmp"
#define XENIA_OPT_XMP_DEFAULT_VOLUME    "xenia_xmp_default_volume"

// Core option keys ??? Emulation
#define XENIA_OPT_TIME_SCALAR           "xenia_time_scalar"
#define XENIA_OPT_TITLE_UPDATES         "xenia_title_updates"
#define XENIA_OPT_APPLY_PATCHES         "xenia_apply_patches"
#define XENIA_OPT_LICENSE_MASK          "xenia_license_mask"
#define XENIA_OPT_USER_LANGUAGE         "xenia_user_language"
#define XENIA_OPT_USER_COUNTRY          "xenia_user_country"

// Core option keys ??? Compatibility
#define XENIA_OPT_PROTECT_ZERO          "xenia_protect_zero"
#define XENIA_OPT_CLEAR_MEMORY_PAGE     "xenia_clear_memory_page_state"
#define XENIA_OPT_DISABLE_CTX_PROMOTION "xenia_disable_context_promotion"
#define XENIA_OPT_MOUNT_CACHE           "xenia_mount_cache"
#define XENIA_OPT_MOUNT_SCRATCH         "xenia_mount_scratch"
#define XENIA_OPT_INCOMPATIBLE_TU       "xenia_allow_incompatible_title_update"
#define XENIA_OPT_STACK_SIZE_HACK       "xenia_stack_size_multiplier_hack"

// Core option keys ??? Backend tuning
#define XENIA_OPT_VK_SPARSE_MEMORY      "xenia_vulkan_sparse_shared_memory"
#define XENIA_OPT_TILED_SHARED_MEMORY   "xenia_tiled_shared_memory"
#define XENIA_OPT_D3D12_BINDLESS        "xenia_d3d12_bindless"
#define XENIA_OPT_READBACK_SYNC         "xenia_readback_resolve_sync"

// Core option keys ??? Audio / input additions
#define XENIA_OPT_VOLUME                "xenia_volume"
#define XENIA_OPT_APU_QUEUED_FRAMES     "xenia_apu_max_queued_frames"
#define XENIA_OPT_VIBRATION             "xenia_vibration"
#define XENIA_OPT_AVPACK                "xenia_avpack"

// Core option keys ??? Debug
#define XENIA_OPT_LOG_LEVEL             "xenia_log_level"

// Core option value constants
#define XENIA_GRAPHICS_D3D12   "d3d12"
#define XENIA_GRAPHICS_VULKAN  "vulkan"

#define XENIA_LOG_LEVEL_ERROR  "error"
#define XENIA_LOG_LEVEL_WARN   "warn"
#define XENIA_LOG_LEVEL_INFO   "info"
#define XENIA_LOG_LEVEL_DEBUG  "debug"

/*
 * retro_core_option_v2_definition fields:
 *   key, desc, desc_categorized, info, info_categorized,
 *   category_key, values[RETRO_NUM_CORE_OPTION_VALUES_MAX], default_value
 */
static struct retro_core_option_v2_definition xenia_core_options_v2_defs[] = {
    /* ================================================================ */
    /* --- Graphics ---                                                  */
    /* ================================================================ */
    {
        XENIA_OPT_RENDER_TARGET_PATH,
        "Render Target Path",
        "RT Path",
        "Select render target emulation mode.\n"
        "Auto: use the config file, so a per-title config can set this.\n"
        "Performance: host render targets with fixed-function blending.\n"
        "Accuracy: pixel shader interlock / rasterizer-ordered views. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",        "Auto (from config)" },
            { "performance", "Performance" },
            { "accuracy",    "Accuracy" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_DRAW_RESOLUTION_SCALE,
        "Draw Resolution Scale (Restart)",
        "Resolution Scale",
        "Scale the internal rendering resolution. Higher values improve "
        "image quality but require more GPU power. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "1", "1x (Native 720p)" },
            { "2", "2x (1440p)" },
            { "3", "3x (2160p/4K)" },
            { "4", "4x" },
            { "5", "5x" },
            { "6", "6x" },
            { "7", "7x" },
            { "8", "8x" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_ANISOTROPIC_FILTERING,
        "Anisotropic Filtering",
        "Anisotropic",
        "Override anisotropic filtering level for all textures. "
        "'Default' uses the game's own settings.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "-1", "Default (Game)" },
            { "0",  "Off" },
            { "1",  "2x" },
            { "2",  "4x" },
            { "3",  "8x" },
            { "4",  "16x" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_ASYNC_SHADERS,
        "Async Shader Compilation",
        "Async Shaders",
        "Compile shaders in background threads. Reduces stutter but may "
        "cause brief rendering artifacts. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_GPU_BACKEND,
        "GPU Backend",
        "Backend",
        "Graphics backend for Xenia's internal rendering.\n"
        "Auto: follow the frontend's preferred context; Vulkan when the\n"
        "frontend cannot express one (e.g. RetroArch 1.7.5 / EmuVR).\n"
        "Vulkan: best under Wine/Proton; no Agility SDK requirement.\n"
        "D3D12: Windows only; requires the DirectX 12 Agility runtime. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",   "Auto" },
            { "vulkan", "Vulkan" },
#ifdef _WIN32
            { "d3d12",  "Direct3D 12" },
#endif
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_READBACK_RESOLVE,
        "Readback Resolve",
        "Readback",
        "Controls which render-to-texture resolves are copied back into "
        "guest RAM.\n"
        "Auto: use the config file, so a per-title config can set this.\n"
        "Fast: copy only resolves the CPU reads back (xenia's default).\n"
        "All: copy every resolve - slower, needed by a few titles.\n"
        "None: disable readback completely - much faster where it works, "
        "but some titles render incorrectly without it.",
        NULL,
        "Graphics",
        {
            { "auto", "Auto (from config)" },
            { "fast", "Fast" },
            { "all",  "All (Slower)" },
            { "none", "None (Fastest)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_STORE_SHADERS,
        "Store Shaders",
        "Shader Cache",
        "Store compiled shaders persistently to avoid recompilation stutter "
        "on subsequent runs. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_HALF_PIXEL_OFFSET,
        "Half-Pixel Offset",
        "Half-Pixel",
        "Enable D3D9-style half-pixel offset. Correct for most games. "
        "Disable if you see shifted/blurry rendering.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_GPU_INVALID_FETCH,
        "Allow Invalid Fetch Constants",
        "Invalid Fetch",
        "Allow texture/vertex fetch constants with invalid type. May fix "
        "crashes in some games but is generally unsafe.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_FUZZY_ALPHA_EPSILON,
        "Fuzzy Alpha Epsilon (NVIDIA Fix)",
        "Fuzzy Alpha",
        "Use approximate alpha comparison to prevent flickering on "
        "NVIDIA GPUs.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_VSYNC,
        "V-Sync (Guest Frame Limiter)",
        "V-Sync",
        "Cap guest vblank rate to 60Hz (NTSC) or 50Hz (PAL). Disabling "
        "allows the emulator to run as fast as possible.",
        NULL,
        "Graphics",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_FRAMERATE_LIMIT,
        "Host Framerate Limit",
        "FPS Limit",
        "Limit the host rendering framerate. 0 = unlimited.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "0",   "Unlimited" },
            { "30",  "30 FPS" },
            { "60",  "60 FPS" },
            { "120", "120 FPS" },
            { "144", "144 FPS" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_50HZ_MODE,
        "PAL 50Hz Mode",
        "50Hz Mode",
        "Run at 50Hz instead of 60Hz for PAL region games. Requires restart.",
        NULL,
        "Graphics",
        {
            { "auto",     "Auto (from config)" },
            { "disabled", "Disabled (60Hz NTSC)" },
            { "enabled",  "Enabled (50Hz PAL)" },
            { NULL, NULL }
        },
        "auto"
    },
    /* ================================================================ */
    /* --- Video ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_INTERNAL_DISPLAY_RES,
        "Internal Display Resolution (Restart)",
        "Display Resolution",
        "Allow games that support multiple resolutions to render at a "
        "specific resolution. Not all games support this. Requires restart.",
        NULL,
        "Video",
        {
            { "auto",     "Auto (from config)" },
            { "0",  "640x480" },
            { "1",  "640x576" },
            { "2",  "720x480" },
            { "3",  "720x576" },
            { "4",  "800x600" },
            { "5",  "848x480" },
            { "6",  "1024x768" },
            { "7",  "1152x864" },
            { "8",  "1280x720 (Default)" },
            { "9",  "1280x768" },
            { "10", "1280x1024" },
            { "11", "1360x768" },
            { "12", "1440x900" },
            { "13", "1680x1050" },
            { "14", "1920x540" },
            { "15", "1920x1080" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_WIDESCREEN,
        "Widescreen (16:9)",
        "Widescreen",
        "Toggle between 16:9 widescreen and 4:3 standard aspect ratio. Requires restart.",
        NULL,
        "Video",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "16:9 Widescreen" },
            { "disabled", "4:3 Standard" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_VIDEO_STANDARD,
        "Video Standard",
        "Signal",
        "Select the video signal standard. Affects region detection. Requires restart.",
        NULL,
        "Video",
        {
            { "auto",     "Auto (from config)" },
            { "1", "NTSC" },
            { "2", "NTSC-J (Japan)" },
            { "3", "PAL" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_DISPLAY_GAMMA,
        "Display Gamma",
        "Gamma",
        "Select display gamma curve.\n"
        "BT.709 (HDTV) is closest to Xbox 360 on a modern display.",
        NULL,
        "Video",
        {
            { "auto",     "Auto (from config)" },
            { "0", "Linear" },
            { "1", "sRGB (CRT)" },
            { "2", "BT.709 (HDTV)" },
            { NULL, NULL }
        },
        "auto"
    },
    /* ================================================================ */
    /* --- Audio ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_AUDIO_ENABLED,
        "Audio Output",
        NULL,
        "Enable or disable audio processing and output. Requires restart.",
        NULL,
        "Audio",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_MUTE,
        "Mute Audio",
        "Mute",
        "Mute all audio output while keeping audio processing active.",
        NULL,
        "Audio",
        {
            { "disabled", "Disabled" },
            { "enabled",  "Enabled (Muted)" },
            { NULL, NULL }
        },
        "disabled"
    },
    {
        XENIA_OPT_XMA_DECODER,
        "XMA Decoder (Restart)",
        "XMA Decoder",
        "Select the XMA audio decoder implementation. Try a different "
        "option if audio is broken in a specific game. Requires restart.",
        NULL,
        "Audio",
        {
            { "auto",     "Auto (from config)" },
            { "old",    "Old (Default)" },
            { "new",    "New" },
            { "master", "Master" },
            { "fake",   "Fake (Silence)" },
            { NULL, NULL }
        },
        "old"
    },
    {
        XENIA_OPT_DEDICATED_XMA_THREAD,
        "Dedicated XMA Thread",
        "XMA Thread",
        "Use a dedicated thread for XMA audio decoding. May improve "
        "audio performance. Requires restart.",
        NULL,
        "Audio",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_ENABLE_XMP,
        "Music Player (XMP)",
        "XMP",
        "Enable the Xbox Music Player for background music playback.",
        NULL,
        "Audio",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_XMP_DEFAULT_VOLUME,
        "Music Player Volume",
        "XMP Volume",
        "Default music volume if the game doesn't set it.",
        NULL,
        "Audio",
        {
            { "auto",     "Auto (from config)" },
            { "0",   "0%" },
            { "10",  "10%" },
            { "20",  "20%" },
            { "30",  "30%" },
            { "40",  "40%" },
            { "50",  "50%" },
            { "60",  "60%" },
            { "70",  "70% (Default)" },
            { "80",  "80%" },
            { "90",  "90%" },
            { "100", "100%" },
            { NULL, NULL }
        },
        "auto"
    },
    /* ================================================================ */
    /* --- Emulation ---                                                 */
    /* ================================================================ */
    {
        XENIA_OPT_TIME_SCALAR,
        "Emulation Speed",
        "Speed",
        "Control emulation speed. 1.0x is normal speed.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "0.25", "0.25x" },
            { "0.5",  "0.5x" },
            { "1.0",  "1.0x (Normal)" },
            { "2.0",  "2.0x" },
            { "4.0",  "4.0x" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_TITLE_UPDATES,
        "Apply Title Updates",
        "Title Updates",
        "Apply title update patches if available in the content directory. Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_LSTICK_DEADZONE,
        "Left Stick Deadzone",
        "Left Deadzone",
        "Ignore left stick movement below this fraction of full travel. "
        "0% uses the stick exactly as the pad reports it, which is xenia's "
        "default and correct for a healthy pad; raise it only to hide drift on "
        "a worn stick, since a deadzone always costs fine control.",
        NULL,
        "Input",
        {
            { "auto",     "Auto (from config)" },
            { "0", "0%%" },
            { "5", "5%%" },
            { "10", "10%%" },
            { "15", "15%%" },
            { "20", "20%%" },
            { "25", "25%%" },
            { "30", "30%%" },
            { "35", "35%%" },
            { "40", "40%%" },
            { "45", "45%%" },
            { "50", "50%%" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_RSTICK_DEADZONE,
        "Right Stick Deadzone",
        "Right Deadzone",
        "Ignore right stick movement below this fraction of full travel. "
        "0% uses the stick exactly as the pad reports it, which is xenia's "
        "default and correct for a healthy pad; raise it only to hide drift on "
        "a worn stick, since a deadzone always costs fine control.",
        NULL,
        "Input",
        {
            { "auto",     "Auto (from config)" },
            { "0", "0%%" },
            { "5", "5%%" },
            { "10", "10%%" },
            { "15", "15%%" },
            { "20", "20%%" },
            { "25", "25%%" },
            { "30", "30%%" },
            { "35", "35%%" },
            { "40", "40%%" },
            { "45", "45%%" },
            { "50", "50%%" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_APPLY_PATCHES,
        "Apply Game Patches",
        "Patches",
        "Whether patch files in the patches folder are used, and what switches "
        "the individual patches inside them on.\n"
        "Disabled: ignore every patch file. Nothing is deleted, so this turns "
        "them all off in one go and back on again later.\n"
        "Enabled: honour each patch's is_enabled flag, the same as standalone "
        "xenia - you edit the .toml to choose.\n"
        "Whole file: apply every patch in the file, so dropping a file in "
        "enables it and deleting the file disables it, with no editing. Note "
        "some files carry several patches - Sonic Unleashed ships seven, "
        "including Disable Shadow Maps and Aspect Ratio. Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",    "Enabled (honour is_enabled)" },
            { "whole_file", "Whole file (drop in to enable)" },
            { "disabled",   "Disabled (ignore all patch files)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_LICENSE_MASK,
        "License Mask",
        "License",
        "Set license mask for activated content (DLC, full version).\n"
        "None: no licenses. Full: first license. All: all licenses. Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "1",  "Full" },
            { "0",  "None" },
            { "-1", "All" },
            { NULL, NULL }
        },
        "1"
    },
    {
        XENIA_OPT_BOOT_SPLASH,
        "Boot Splash Video",
        "Boot Splash",
        "Play an optional user-supplied boot animation (press Start to "
        "skip). Xenia is a high-level emulator with no console boot flow of "
        "its own.\n"
        "Before Boot: authentic sequence; the game starts loading after the "
        "video ends.\n"
        "During Load: the game boots underneath the video, masking load "
        "time.\n"
        "Place an MPEG-1 file at <system>/xenia/bootanim.mpg. Convert any "
        "video with: ffmpeg -i in.mp4 -c:v mpeg1video -q:v 4 -c:a mp2 "
        "-ar 48000 bootanim.mpg Requires restart.",
        NULL,
        "Emulation",
        {
            { "enabled",  "Before Boot" },
            { "loadmask", "During Load" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_AUTO_PROFILE,
        "Auto Sign-In Profile",
        "Auto Profile",
        "Sign in an Xbox profile at boot so games see a logged-in user "
        "(required for saves and scores in many titles, especially XBLA).\n"
        "Signs in the first existing profile, or creates one named "
        "'PlayerOne' on first run. Profiles are generated by the emulator "
        "and stored in the save directory. Requires restart.",
        NULL,
        "Emulation",
        {
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "enabled"
    },
    {
        XENIA_OPT_MSGBOX_BUTTON,
        "Message Box Response",
        "Message Box",
        "How the headless core answers a game's message-box prompt (e.g. the "
        "'no save data - save / don't save / select device' window or the "
        "'connect to Xbox LIVE / play offline' question some games show, "
        "which a desktop emulator would let you click).\n"
        "Auto: recognized save/storage prompts answer 'Save', Xbox LIVE / "
        "online prompts answer the offline choice (their default buttons "
        "lead into menus that cannot be shown); anything else gets the "
        "game's own default button.\n"
        "Button 0..3: force that choice (0-based) - only needed for a title "
        "whose prompt Auto answers wrongly (check the core log to see the "
        "prompt and buttons).",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "-1", "Auto (recommended)" },
            { "0",  "Button 0" },
            { "1",  "Button 1" },
            { "2",  "Button 2" },
            { "3",  "Button 3" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_USER_LANGUAGE,
        "User Language",
        "Language",
        "Set the emulated console language. Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "English",    "English" },
            { "Japanese",   "Japanese" },
            { "German",     "German" },
            { "French",     "French" },
            { "Spanish",    "Spanish" },
            { "Italian",    "Italian" },
            { "Korean",     "Korean" },
            { "TChinese",   "Traditional Chinese" },
            { "Portuguese", "Portuguese" },
            { "SChinese",   "Simplified Chinese" },
            { "Polish",     "Polish" },
            { "Russian",    "Russian" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_USER_COUNTRY,
        "User Country",
        "Country",
        "Set the emulated console country/region. Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "United States", "United States" },
            { "Great Britain", "Great Britain" },
            { "Japan",         "Japan" },
            { "Germany",       "Germany" },
            { "France",        "France" },
            { "Spain",         "Spain" },
            { "Italy",         "Italy" },
            { "Australia",     "Australia" },
            { "Canada",        "Canada" },
            { "Brazil",        "Brazil" },
            { "Korea",         "Korea" },
            { "China",         "China" },
            { "Mexico",        "Mexico" },
            { "Netherlands",   "Netherlands" },
            { "Russia",        "Russia" },
            { "Sweden",        "Sweden" },
            { "Poland",        "Poland" },
            { "Portugal",      "Portugal" },
            { "Taiwan",        "Taiwan" },
            { "Hong Kong",     "Hong Kong" },
            { NULL, NULL }
        },
        "auto"
    },
    /* ================================================================ */
    /* --- Compatibility ---                                             */
    /* ================================================================ */
    {
        XENIA_OPT_PROTECT_ZERO,
        "Protect Zero Page",
        "Protect Zero",
        "Protect the zero page from reads and writes. Disable if a game "
        "crashes on startup. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_CLEAR_MEMORY_PAGE,
        "Clear GPU Memory Page State",
        "Clear GPU Cache",
        "Refresh state of memory pages for GPU written data. Disable for "
        "a minor performance boost, but may break rendering. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_DISABLE_CTX_PROMOTION,
        "Disable Context Promotion",
        "No Ctx Promotion",
        "Disable context promotion CPU optimization. May be needed for "
        "some sports games, but reduces performance. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto",     "Auto (from config)" },
            { "disabled", "Disabled (Normal)" },
            { "enabled",  "Enabled (Sports Fix)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_MOUNT_CACHE,
        "Mount Cache Partition",
        "Cache Mount",
        "Enable cache partition mount. Required by some games. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_MOUNT_SCRATCH,
        "Mount Scratch Partition",
        "Scratch Mount",
        "Enable scratch partition mount. Required by some games. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto",     "Auto (from config)" },
            { "disabled", "Disabled" },
            { "enabled",  "Enabled" },
            { NULL, NULL }
        },
        "auto"
    },
    /* ================================================================ */
    /* --- Debug ---                                                     */
    /* ================================================================ */
    {
        XENIA_OPT_LOG_LEVEL,
        "Log Level",
        "Level",
        "Set the verbosity of logging output.",
        NULL,
        "Debug",
        {
            { "auto",     "Auto (from config)" },
            { XENIA_LOG_LEVEL_ERROR, "Error Only" },
            { XENIA_LOG_LEVEL_WARN,  "Warning" },
            { XENIA_LOG_LEVEL_INFO,  "Info" },
            { XENIA_LOG_LEVEL_DEBUG, "Debug" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_VOLUME,
        "Master Volume",
        "Volume",
        "Overall output level. Mute overrides this while it is on.",
        NULL,
        "Audio",
        {
            { "auto", "Auto (from config)" },
            { "0",   "0% (Silent)" }, { "10",  "10%" }, { "20", "20%" },
            { "30",  "30%" },  { "40",  "40%" },  { "50", "50%" },
            { "60",  "60%" },  { "70",  "70%" },  { "80", "80%" },
            { "90",  "90%" },  { "100", "100%" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_APU_QUEUED_FRAMES,
        "Audio Buffered Frames (Restart)",
        "Audio Buffer",
        "How many audio frames may be queued ahead. Lower cuts latency; too "
        "low crackles under load. Requires restart.",
        NULL,
        "Audio",
        {
            { "auto", "Auto (from config)" },
            { "2", "2 (Lowest latency)" }, { "4",  "4" },  { "6",  "6" },
            { "8", "8 (Default)" },        { "12", "12" }, { "16", "16" },
            { "24", "24 (Most stable)" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_VIBRATION,
        "Controller Vibration",
        "Vibration",
        "Send rumble to the pad. The frontend may also have its own rumble "
        "switch, and both have to be on.",
        NULL,
        "Input",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_AVPACK,
        "AV Pack (Restart)",
        "AV Pack",
        "Which cable the console reports as connected, which is how a title "
        "decides what video modes it may offer. HDMI suits almost everything; "
        "the PAL-60 entries exist for titles that only offer 50Hz otherwise. "
        "Requires restart.",
        NULL,
        "Video",
        {
            { "auto", "Auto (from config)" },
            { "8", "HDMI (Default)" },
            { "4", "HDMI + Audio" },
            { "3", "480p Component (HD)" },
            { "6", "VGA" },
            { "0", "PAL-60 Component (SD)" },
            { "2", "PAL-60 SCART" },
            { "5", "PAL-60 Composite / S-Video" },
            { "7", "TV PAL-60" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_INCOMPATIBLE_TU,
        "Allow Mismatched Title Updates (Restart)",
        "Mismatched TU",
        "Apply a title update whose signature does not match the game. Needed "
        "for some update/region combinations; disable to be strict. "
        "Requires restart.",
        NULL,
        "Emulation",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_STACK_SIZE_HACK,
        "Stack Size Multiplier Hack (Restart)",
        "Stack Hack",
        "Multiplies guest thread stack sizes. A workaround for titles that "
        "misbehave around setjmp/longjmp - leave at 1 unless a specific game "
        "is known to need it. Requires restart.",
        NULL,
        "Compatibility",
        {
            { "auto", "Auto (from config)" },
            { "1", "1x (Default)" }, { "2", "2x" }, { "4", "4x" },
            { "8", "8x" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_VK_SPARSE_MEMORY,
        "Vulkan Sparse Shared Memory (Restart)",
        "VK Sparse Mem",
        "Use sparse binding for shared memory. Disable if the Vulkan backend "
        "fails to start or misrenders on a driver with weak sparse support. "
        "Requires restart.",
        NULL,
        "Backend Tuning",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    {
        XENIA_OPT_TILED_SHARED_MEMORY,
        "Tiled Shared Memory (Restart)",
        "Tiled Mem",
        "Use tiled/sparse resources for the guest's large address space. "
        "Disable if textures corrupt or the GPU backend refuses to start. "
        "Requires restart.",
        NULL,
        "Backend Tuning",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
#ifdef _WIN32
    // D3D12 is a Windows-only backend here, so this option would be inert
    // everywhere else. Listing it only where it does something keeps the menu
    // honest, and its cvar does not exist in a Linux build anyway.
    {
        XENIA_OPT_D3D12_BINDLESS,
        "D3D12 Bindless Resources (Restart)",
        "D3D12 Bindless",
        "Use bindless resources on the Direct3D 12 backend where the hardware "
        "supports them. Usually faster; disable to rule it out when "
        "diagnosing D3D12 rendering faults. No effect on Vulkan. "
        "Requires restart.",
        NULL,
        "Backend Tuning",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
#endif  // _WIN32
    {
        XENIA_OPT_READBACK_SYNC,
        "Readback Resolve Sync",
        "Readback Sync",
        "Stall the GPU after each readback copy so guest memory is correct "
        "before the game reads it. Disabling is faster but can show stale "
        "data. Only matters when Readback Resolve is not None.",
        NULL,
        "Backend Tuning",
        {
            { "auto",     "Auto (from config)" },
            { "enabled",  "Enabled" },
            { "disabled", "Disabled" },
            { NULL, NULL }
        },
        "auto"
    },
    /* Terminator */
    { NULL, NULL, NULL, NULL, NULL, NULL, {{0}}, NULL }
};

static struct retro_core_option_v2_category xenia_core_option_categories[] = {
    { "Graphics", "Graphics Settings",
      "GPU backend, resolution scale, render target path, filtering." },
    { "Video", "Video Settings",
      "Display resolution, aspect ratio, video standard, gamma." },
    { "Audio", "Audio Settings",
      "Audio output, XMA decoder, music player." },
    { "Emulation", "Emulation Settings",
      "Speed, language, country, licenses, patches." },
    { "Compatibility", "Compatibility Settings",
      "Per-game hacks, memory, cache/scratch mounts." },
    { "Debug", "Debug Settings",
      "Log verbosity." },
    { NULL, NULL, NULL }
};

static struct retro_core_options_v2 xenia_core_options_v2_def = {
    xenia_core_option_categories,
    xenia_core_options_v2_defs
};

/* Publish core options with a legacy fallback. Frontends before options v2
 * (RetroArch <= 1.9.11, including EmuVR's 1.7.5) return false for
 * SET_CORE_OPTIONS_V2 and would silently end up with all-default options;
 * convert the v2 definitions to the v0 SET_VARIABLES format
 * ("Label; default|value1|value2", default listed first) for them. */
static void xenia_publish_core_options(retro_environment_t cb) {
    unsigned version = 0;
    if (!cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version))
        version = 0;
    if (version >= 2) {
        cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &xenia_core_options_v2_def);
        return;
    }

    #define XENIA_NUM_OPTS \
        (sizeof(xenia_core_options_v2_defs) / sizeof(xenia_core_options_v2_defs[0]))
    static struct retro_variable vars[XENIA_NUM_OPTS];
    static char var_bufs[XENIA_NUM_OPTS][2048];
    size_t out = 0;

    for (size_t i = 0; xenia_core_options_v2_defs[i].key; i++) {
        const struct retro_core_option_v2_definition *def =
            &xenia_core_options_v2_defs[i];
        char *buf = var_bufs[out];
        size_t cap = sizeof(var_bufs[out]);
        size_t pos = (size_t)snprintf(buf, cap, "%s; ",
                                      def->desc ? def->desc : def->key);

        if (def->default_value && pos < cap) {
            pos += (size_t)snprintf(buf + pos, cap - pos, "%s",
                                    def->default_value);
        }
        for (size_t v = 0; v < RETRO_NUM_CORE_OPTION_VALUES_MAX &&
                           def->values[v].value; v++) {
            if (def->default_value &&
                !strcmp(def->values[v].value, def->default_value)) {
                continue;
            }
            if (pos < cap) {
                pos += (size_t)snprintf(buf + pos, cap - pos, "|%s",
                                        def->values[v].value);
            }
        }

        vars[out].key = def->key;
        vars[out].value = buf;
        out++;
    }
    vars[out].key = NULL;
    vars[out].value = NULL;
    #undef XENIA_NUM_OPTS

    cb(RETRO_ENVIRONMENT_SET_VARIABLES, vars);
}

#endif /* LIBRETRO_CORE_OPTIONS_H */
