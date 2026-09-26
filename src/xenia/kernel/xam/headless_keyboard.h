/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Lets a windowless host (the libretro core) show the virtual keyboard its own
// way. Implemented by xam_ui_stub.cc, which only the headless build compiles.

#ifndef XENIA_KERNEL_XAM_HEADLESS_KEYBOARD_H_
#define XENIA_KERNEL_XAM_HEADLESS_KEYBOARD_H_

#include <cstdint>
#include <functional>
#include <string>

namespace xe {
namespace kernel {
namespace xam {

struct HeadlessKeyboardRequest {
  std::string title;
  std::string description;
  // What the title pre-filled the box with; may be empty.
  std::string default_text;
  // What the auto-fill would enter: default_text, else the configured
  // headless_keyboard_text, else the signed-in gamertag.
  std::string suggested_text;
  // Characters that fit, not counting the terminator.
  uint32_t max_length = 0;
  uint32_t flags = 0;
};

// Called exactly once, from any thread. accepted == false means cancelled.
using HeadlessKeyboardDone =
    std::function<void(bool accepted, const std::string& text)>;

// Returns false to decline, and the stub auto-fills as before. Returning true
// takes ownership of the request: done must be called once it is answered.
using HeadlessKeyboardHandler = std::function<bool(
    const HeadlessKeyboardRequest& request, HeadlessKeyboardDone done)>;

void SetHeadlessKeyboardHandler(HeadlessKeyboardHandler handler);

}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_HEADLESS_KEYBOARD_H_
