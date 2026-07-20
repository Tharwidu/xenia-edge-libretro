# xenia-libretro: the libretro core, a shared library wrapping the emulator
# with a headless UI (no wx/GTK). Enabled via -DXENIA_BUILD_LIBRETRO=ON, which
# also swaps the xam UI implementations for stubs and strips wx from xenia-ui.

add_library(xenia-libretro SHARED
  libretro.cpp
  libretro_audio_driver.cc
  libretro_graphics_system.cc
  libretro_headless_stubs.cc
  libretro_hid.cc
  libretro_vk_presenter.cc
)
if(WIN32)
  target_sources(xenia-libretro PRIVATE libretro_d3d12_presenter.cc)
endif()

set_target_properties(xenia-libretro PROPERTIES
  OUTPUT_NAME "xenia_edge_libretro"
  PREFIX ""
)

target_include_directories(xenia-libretro PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
)

target_link_libraries(xenia-libretro PRIVATE
  xenia-apu
  xenia-apu-nop
  xenia-apu-sdl
  xenia-base
  xenia-core
  xenia-cpu
  xenia-gpu
  xenia-gpu-null
  xenia-gpu-vulkan
  xenia-helper-sdl
  xenia-hid
  xenia-hid-nop
  xenia-kernel
  xenia-patcher
  xenia-ui
  xenia-ui-vulkan
  xenia-vfs
  aes_128
  capstone
  fmt
  dxbc
  glslang-spirv
  imgui
  libavcodec
  libavutil
  mspack
  snappy
  xxhash
)

if(XE_TARGET_X86_64)
  target_link_libraries(xenia-libretro PRIVATE xenia-cpu-backend-x64)
endif()

if(WIN32)
  target_link_libraries(xenia-libretro PRIVATE
    xenia-gpu-d3d12 xenia-ui-d3d12
    d3d12 dxgi d3dcompiler vulkan-1)
  if(DEFINED ENV{VULKAN_SDK})
    target_include_directories(xenia-libretro PRIVATE "$ENV{VULKAN_SDK}/Include")
    target_link_directories(xenia-libretro PRIVATE "$ENV{VULKAN_SDK}/Lib")
  endif()
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # The libretro VK presenter calls Vulkan entry points directly; xenia itself
  # only loads Vulkan dynamically.
  target_link_libraries(xenia-libretro PRIVATE vulkan)
  # $ORIGIN lets a libSDL3.so.0 shipped next to the core resolve when the
  # frontend dlopens the core from its cores directory (SDL3 is deliberately
  # a shared library on Linux; see third_party/CMakeLists.txt).
  set_target_properties(xenia-libretro PROPERTIES BUILD_RPATH "$ORIGIN")
  # Static libs resolve circular deps via a link group, mirroring the
  # executable flags set in the top-level CMakeLists. --no-undefined makes
  # missing symbols fail the link instead of surfacing at dlopen time.
  target_link_options(xenia-libretro PRIVATE -Wl,--start-group -Wl,--no-undefined)
endif()

xe_target_defaults(xenia-libretro)
