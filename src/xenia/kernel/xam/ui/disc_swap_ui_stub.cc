/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Stub file for libretro build - provides empty implementations of DiscSwapUI

#include "xenia/kernel/xam/ui/disc_swap_ui.h"

#include <utility>

#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

DiscSwapUI::DiscSwapUI(xe::ui::ImGuiDrawer* imgui_drawer,
                       xe::hid::InputSystem* input_system,
                       const std::string& message,
                       const std::vector<DiscInfo>& discs, bool show_error,
                       std::string title, std::string list_prompt,
                       bool allow_browse)
    : XamGamepadDialog(imgui_drawer, input_system),
      title_(std::move(title)),
      message_(message),
      error_message_(show_error ? message : ""),
      list_prompt_(std::move(list_prompt)),
      show_error_(show_error),
      allow_browse_(allow_browse),
      discs_(discs) {
  // Stub implementation - never drawn, always cancelled.
}

void DiscSwapUI::OnDraw(ImGuiIO& io) {
  // Stub implementation - do nothing
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
