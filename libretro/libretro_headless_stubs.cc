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

// cvar from notification_widget_qt.cc (excluded).
DEFINE_path(achievement_sound_path, "",
            "Path to achievement unlock sound (unused in libretro).", "UI");

// Qt version tag stub: xam_ui.cc needs qt_version_tag_6_10 from Qt6Core.dll.
#ifdef _WIN32
extern "C" {
  static const char qt_version_tag_6_10_stub_data = 0;
  const char* __imp_qt_version_tag_6_10 = &qt_version_tag_6_10_stub_data;
}
#endif
