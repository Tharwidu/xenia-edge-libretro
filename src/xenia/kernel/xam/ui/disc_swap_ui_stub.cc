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
#include "xenia/ui/imgui_dialog.h"

namespace xe {
namespace kernel {
namespace xam {
namespace ui {

DiscSwapUI::DiscSwapUI(xe::ui::ImGuiDrawer* imgui_drawer,
                       const std::string& message,
                       const std::vector<DiscInfo>& discs, bool show_error)
    : XamDialog(imgui_drawer), 
      discs_(discs), 
      show_error_(show_error),
      result_(DiscSwapResult::kCancelled) {
  // Stub implementation - always cancelled
  title_ = "Disc Swap (Libretro Stub)";
  has_opened_ = false;
  message_ = message;
  error_message_ = show_error ? message : "";
}

void DiscSwapUI::OnDraw(ImGuiIO& io) {
  // Stub implementation - do nothing
}

}  // namespace ui
}  // namespace xam
}  // namespace kernel
}  // namespace xe
