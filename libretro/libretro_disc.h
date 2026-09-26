/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Multi-disc support: .m3u playlists, libretro disk control, and answering a
 * title's disc swap request (XamSwapDisc) without a window.
 *
 * A 360 title names the disc it wants, so unlike a console with a tray the
 * core can often insert the right one itself:
 *   - with an .m3u playlist loaded, always;
 *   - otherwise, when automatic disc swap is enabled, from the "(Disc N)"
 *     sibling of the loaded content (the naming Redump/No-Intro sets use).
 * A candidate whose package header records a different disc or title is never
 * picked. When nothing is found, or automatic swap is off, the request waits
 * for the frontend's disk control: eject, choose the disc, close the tray.
 */

#ifndef LIBRETRO_DISC_H
#define LIBRETRO_DISC_H

#include <cstdint>
#include <filesystem>

#include "libretro.h"

namespace xe {
namespace libretro_disc {

using LogFn = void (*)(enum retro_log_level level, const char* fmt, ...);

// Where the module logs. Content loading runs before xenia's logger exists,
// so it goes through the frontend's log like the rest of retro_load_game.
void SetLogger(LogFn log);

// Registers the disk control interface (the ext one where the frontend has
// it, the legacy one for RetroArch 1.7.5). Call from retro_set_environment:
// the frontend picks the initial disc before retro_load_game.
void RegisterDiskControl(retro_environment_t environ_cb);

// Follows an .x360 pointer file (a one-line text file naming the real
// content, absolute or relative to itself); other paths pass through.
// Returns false, having logged why, when the target cannot be used.
bool ResolveContentPath(const std::filesystem::path& in,
                        std::filesystem::path* out);

// Sets up the disc list for newly loaded content: an .m3u becomes the list,
// anything else a list of one. Returns false, having logged why, when an
// .m3u names nothing usable. *boot_path receives the disc to launch.
bool LoadContent(const std::filesystem::path& content,
                 std::filesystem::path* boot_path);

void SetAutomatic(bool enabled);

// Answers a title's request for disc_number (1-based). Runs on the
// requesting guest thread and may park it until the frontend inserts a disc.
// retry is set when the disc returned last time was rejected.
std::filesystem::path Resolve(uint32_t disc_number, bool retry,
                              uint32_t title_id);

// Wakes a request parked on the frontend so the emulator can tear down, and
// makes later requests return nothing. Call before shutting the emulator
// down; LoadContent re-arms it.
void Shutdown();

}  // namespace libretro_disc
}  // namespace xe

#endif  // LIBRETRO_DISC_H
