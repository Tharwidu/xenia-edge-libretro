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

}  // namespace ui
}  // namespace xe

// achievement_sound_path is now defined by ui/audio_helper.cc, which builds
// headless since the wx migration; the Qt version tag stub is likewise gone.

// mount_memory_unit is defined in app/xenia_main.cc (excluded here) but
// referenced from emulator.cc; the other xenia_main cvars libretro needs are
// already defined in libretro.cpp.
DEFINE_bool(mount_memory_unit, false, "Enable memory unit (MU) mount",
            "Storage");
