/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * On-screen keyboard for a title's text prompts (XamShowKeyboardUI).
 *
 * libretro gives a core no way to ask the frontend for text, so the core
 * draws its own panel over the game's frame and reads the controller (and
 * the keyboard, where the frontend passes it through) until the player
 * accepts or cancels. Drawn into the CPU copy of the frame every video path
 * already makes, so it looks the same on every backend and frontend.
 */

#ifndef LIBRETRO_KEYBOARD_H
#define LIBRETRO_KEYBOARD_H

#include <cstddef>
#include <cstdint>

#include "libretro.h"

namespace xe {
namespace libretro_keyboard {

enum class Mode {
  kAutoFill,      // answer at once with the suggested text (old behaviour)
  kAskPrefilled,  // show the keyboard with the suggested text entered
  kAskEmpty,      // show the keyboard with the title's own default only
};

void SetMode(Mode mode);

// Message boxes: true shows them as a list of the title's buttons, starting
// on the button the auto-answer would pick; false auto-answers (old
// behaviour).
void SetAskMessageBoxes(bool ask);

// Registers the prompt handler with the XAM stub and the keyboard callback
// with the frontend.
void Install(retro_environment_t environ_cb);

// True while a prompt is open, and until every button used on it is let go,
// so the press that closed it does not reach the game.
bool SuppressGuestInput();

// Once per retro_run, on the frontend thread.
void Update(retro_input_state_t input_state);

// Paints the prompt over a 32-bit frame. bgra: bytes are B,G,R,X per pixel
// (XRGB8888); otherwise R,G,B,X.
void DrawOverlay(uint8_t* pixels, uint32_t width, uint32_t height,
                 size_t pitch, bool bgra);

// Answers an open prompt as cancelled. Call before the emulator tears down.
void Cancel();

}  // namespace libretro_keyboard
}  // namespace xe

#endif  // LIBRETRO_KEYBOARD_H
