/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Installing content that ships on a title's other discs.
 *
 * Some multi-disc titles keep data on a disc the game never boots from:
 * Grand Theft Auto V's install disc carries four content packages under
 * \content\0000000000000000\545408A7\00000002, which a console copies to its
 * hard drive before the play disc will run. Standalone Xenia does the same
 * through its "Install Content" menu; a frontend has no such menu, and
 * letting the game run its own installer after a disc swap crashes the
 * title. So before launch, packages found under the booted title's id on its
 * other discs (ISO or GOD) are copied into the content store, once.
 */

#ifndef LIBRETRO_DISC_INSTALL_H
#define LIBRETRO_DISC_INSTALL_H

#include <filesystem>
#include <string>

#include "libretro_disc.h"

namespace xe {
namespace libretro_disc_install {

// Looks for packages on the loaded title's other discs that content_root
// lacks and, when there are any, starts copying them on a worker thread.
// Returns true while that copy runs. Call after libretro_disc::LoadContent.
bool Start(const std::filesystem::path& content_root,
           libretro_disc::LogFn log);

// True until the copy started by Start has finished or failed.
bool Running();

// One line of progress for the on-screen message.
std::string Progress();

// Stops after the file being copied and waits for the worker. Safe to call
// when nothing runs.
void Cancel();

}  // namespace libretro_disc_install
}  // namespace xe

#endif  // LIBRETRO_DISC_INSTALL_H
