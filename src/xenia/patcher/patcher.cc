/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/patcher/patcher.h"

// Upstream patch files ship every patch with is_enabled = false, so a user who
// downloads one has to open the TOML and edit it before anything happens. That
// is a poor fit for a libretro core, where the natural gesture is to drop a
// file in a folder and have it take effect - and to delete it to undo.
//
// This makes the file the switch. It is opt-in rather than the default because
// a patch file is not necessarily one patch: Sonic Unleashed ships seven in a
// single file, including Disable Shadow Maps, Disable Depth of Field and Aspect
// Ratio. Turning all of those on merely because the file exists would surprise
// people. Titles with a single patch - the common case - behave exactly as
// expected.
DEFINE_bool(
    patch_all_in_file, false,
    "Apply every patch in a patch file, ignoring each patch's is_enabled "
    "flag. Lets a patch file be enabled by dropping it in and disabled by "
    "deleting it, with no TOML editing. Off means honour is_enabled, which "
    "is what standalone xenia does.",
    "General");

namespace xe {
namespace patcher {

Patcher::Patcher(std::filesystem::path patches_dir) {
  is_any_patch_applied_ = false;
  patch_db_ = new PatchDB(std::move(patches_dir));
}

void Patcher::ApplyPatchesForTitle(Memory* memory, const uint32_t title_id,
                                   const std::optional<uint64_t> hash) {
  patch_db_->LoadPatches();
  const auto title_patches = patch_db_->GetTitlePatches(title_id, hash);

  for (const PatchFileEntry& patchFile : title_patches) {
    for (const PatchInfoEntry& patchEntry : patchFile.patch_info) {
      if (!patchEntry.is_enabled && !cvars::patch_all_in_file) {
        continue;
      }
      XELOGE("Patcher: Applying patch for: {}({:08X}) - {}",
             patchFile.title_name, patchFile.title_id, patchEntry.patch_name);
      ApplyPatch(memory, &patchEntry);
    }
  }
}

void Patcher::ApplyPatch(Memory* memory, const PatchInfoEntry* patch) {
  for (const PatchDataEntry& patch_data_entry : patch->patch_data) {
    uint32_t old_address_protect = 0;
    uint8_t* address = memory->TranslateVirtual(patch_data_entry.address);
    xe::BaseHeap* heap = memory->LookupHeap(patch_data_entry.address);
    if (!heap) {
      continue;
    }

    heap->QueryProtect(patch_data_entry.address, &old_address_protect);

    heap->Protect(patch_data_entry.address,
                  (uint32_t)patch_data_entry.data.alloc_size,
                  kMemoryProtectRead | kMemoryProtectWrite);

    std::memcpy(address, patch_data_entry.data.patch_data.data(),
                patch_data_entry.data.alloc_size);

    // Restore previous protection
    heap->Protect(patch_data_entry.address,
                  (uint32_t)patch_data_entry.data.alloc_size,
                  old_address_protect);

    is_any_patch_applied_ = true;
  }
}

}  // namespace patcher
}  // namespace xe
