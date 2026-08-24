/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Headless stubs for symbols that normally come from Qt-dependent code.
 *
 * These stubs satisfy linker references from xenia-kernel and xenia-ui
 * objects that reference Qt-only implementations (FilePicker, cvars, etc).
 * In a libretro context these features are unused.
 */

#include <memory>
#include <filesystem>
#include "xenia/ui/file_picker.h"
#include "xenia/base/cvar.h"

// Stub: FilePicker::Create normally returns a Qt file picker.
// Libretro has no use for file dialogs.
namespace xe {
namespace ui {

std::unique_ptr<FilePicker> FilePicker::Create() { return nullptr; }

#ifdef _WIN32
// Defined in redist_installer_wx.cc (excluded): the app offers to download
// the Vulkan runtime. A libretro frontend's process either has vulkan-1.dll
// available (real Windows with GPU drivers, or winevulkan under Proton) or
// the Vulkan path is unusable anyway — report present and let device
// creation fail gracefully if not.
bool EnsureVulkanLoader(const std::filesystem::path& vulkan_dir) {
  return true;
}

// Agility runtime / debug layer installers relaunch the process on success —
// unacceptable inside a libretro frontend. Report "not installed" and let the
// D3D12 backend run against the inbox D3D12 (or vkd3d-proton under wine).
bool EnsureAgilityRuntime(const std::filesystem::path& d3d12_dir) {
  return false;
}

bool EnsureDebugLayer(const std::filesystem::path& d3d12_dir) {
  return false;
}

bool EnsureShaderCompilerRuntime(const std::filesystem::path& d3d12_dir) {
  return false;
}
#endif

}  // namespace ui
}  // namespace xe

// achievement_sound_path is now defined by ui/audio_helper.cc, which builds
// headless since the wx migration; the Qt version tag stub is likewise gone.

// mount_memory_unit used to live in app/xenia_main.cc, which this build
// excludes, so the core defined it here. emulator.cc defines it upstream now.
