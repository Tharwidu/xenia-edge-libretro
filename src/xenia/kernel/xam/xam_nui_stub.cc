/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Stub file for libretro build - provides empty implementations of NUI exports

#include "xenia/kernel/xam/xam_private.h"
#include "xenia/cpu/export_resolver.h"
#include "xenia/kernel/kernel_state.h"

namespace xe {
namespace kernel {
namespace xam {

void RegisterNUIExports(xe::cpu::ExportResolver* export_resolver,
                        xe::kernel::KernelState* kernel_state) {
  // Empty implementation for libretro build
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
