/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// Headless UI exports for the libretro build.
//
// The full xam_ui.cc is excluded from this build because it depends on the
// ImGui drawer / dialog stack, which the libretro core (headless, no display
// window) does not create. That left every XAM UI export unregistered, so any
// title that opened a message box, storage-device selector, keyboard, or
// sign-in dialog hit an "undefined extern call" and soft-locked (e.g. Sonic
// Unleashed stalls after Start on XamShowDeviceSelectorUI / XamShowMessageBoxUI).
//
// These implementations mirror the `cvars::headless` branches of the real
// xam_ui.cc: auto-complete each dialog with a sensible default and drive the
// XN_SYS_UI on/off notification so waiting titles proceed. They intentionally
// avoid the UI-thread dispatch helpers (xeXamDispatchHeadlessAsync), which need
// a display window that does not exist here.

#include <chrono>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/xbox.h"

namespace xe {
namespace kernel {
namespace xam {

// Window-free equivalent of xeXamDispatchHeadless: run the completion callback
// (immediately or via the kernel's deferred-overlapped path) while toggling the
// system-UI notification around it.
static X_RESULT DispatchHeadless(std::function<X_RESULT()> run,
                                 uint32_t overlapped) {
  auto pre = []() {
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
    xe::threading::Sleep(std::chrono::milliseconds(25));
  };
  auto post = []() {
    std::thread([]() {
      xe::threading::Sleep(std::chrono::milliseconds(200));
      kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
    }).detach();
  };
  if (!overlapped) {
    pre();
    X_RESULT result = run();
    post();
    return result;
  }
  kernel_state()->CompleteOverlappedDeferred(std::move(run), overlapped, pre,
                                             post);
  return X_ERROR_IO_PENDING;
}

static void NotifyUiShownBriefly() {
  kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  std::thread([]() {
    xe::threading::Sleep(std::chrono::milliseconds(200));
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, false);
  }).detach();
}

dword_result_t XamIsUIActive_entry() { return 0; }
DECLARE_XAM_EXPORT2(XamIsUIActive, kUI, kImplemented, kHighFrequency);

static dword_result_t ShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  // Auto-pick the focused button, exactly like the headless path upstream.
  return DispatchHeadless(
      [result_ptr, active_button]() -> X_RESULT {
        result_ptr->ButtonPressed = static_cast<uint32_t>(active_button);
        return X_ERROR_SUCCESS;
      },
      overlapped);
}

dword_result_t XamShowMessageBoxUI_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return ShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                          button_ptrs, active_button, flags, result_ptr,
                          overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUI, kUI, kImplemented);

dword_result_t XamShowMessageBoxUIEx_entry(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, dword_t unknown_unused,
    pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  return ShowMessageBoxUi(user_index, title_ptr, text_ptr, button_count,
                          button_ptrs, active_button, flags, result_ptr,
                          overlapped);
}
DECLARE_XAM_EXPORT1(XamShowMessageBoxUIEx, kUI, kImplemented);

dword_result_t XNotifyQueueUI_entry(dword_t exnq, dword_t dwUserIndex,
                                    qword_t qwAreas,
                                    lpu16string_t displayText_ptr,
                                    lpvoid_t contextData) {
  if (displayText_ptr) {
    XELOGI("XNotifyQueueUI: {}", xe::to_utf8(displayText_ptr.value()));
  }
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XNotifyQueueUI, kUI, kSketchy);

dword_result_t XamShowKeyboardUI_entry(
    dword_t user_index, dword_t flags, lpu16string_t default_text,
    lpu16string_t title, lpu16string_t description, lpu16string_t buffer,
    dword_t buffer_length, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;
  return DispatchHeadless(
      [default_text, buffer, buffer_length, buffer_size]() -> X_RESULT {
        // Headless: accept the provided default text unchanged.
        if (!default_text) {
          std::memset(buffer, 0, buffer_size);
        } else {
          string_util::copy_and_swap_truncating(buffer, default_text.value(),
                                                 buffer_length);
        }
        return X_ERROR_SUCCESS;
      },
      overlapped);
}
DECLARE_XAM_EXPORT1(XamShowKeyboardUI, kUI, kImplemented);

dword_result_t XamShowDeviceSelectorUI_entry(
    dword_t user_index, dword_t content_type, dword_t content_flags,
    qword_t total_requested, lpdword_t device_id_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!overlapped) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if ((user_index >= XUserMaxUserCount && user_index != XUserIndexAny) ||
      (content_flags & 0x83F00008) != 0 || !device_id_ptr) {
    return X_ERROR_INVALID_PARAMETER;
  }

  std::vector<const DummyDeviceInfo*> devices = ListStorageDevices();
  // Default to the first storage device (HDD), matching headless upstream.
  return DispatchHeadless(
      [device_id_ptr, devices]() -> X_RESULT {
        if (devices.empty()) {
          return X_ERROR_CANCELLED;
        }
        *device_id_ptr = static_cast<uint32_t>(devices.front()->device_id);
        return X_ERROR_SUCCESS;
      },
      overlapped);
}
DECLARE_XAM_EXPORT1(XamShowDeviceSelectorUI, kUI, kImplemented);

void XamShowDirtyDiscErrorUI_entry(dword_t user_index) {
  // Upstream headless calls exit(1) here; for a libretro core we must never
  // tear down the host process. Log and let the title continue/handle it.
  XELOGE("XamShowDirtyDiscErrorUI: disc read error reported by title");
}
DECLARE_XAM_EXPORT1(XamShowDirtyDiscErrorUI, kUI, kImplemented);

dword_result_t XamShowPartyUI_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowPartyUI, kNone, kStub);

dword_result_t XamShowCommunitySessionsUI_entry(unknown_t r3, unknown_t r4) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowCommunitySessionsUI, kNone, kStub);

dword_result_t XamSetDashContext_entry(dword_t value,
                                       const ppc_context_t& ctx) {
  ctx->kernel_state->dash_context_ = value;
  kernel_state()->BroadcastNotification(kXNotificationSystemDashContextChanged,
                                        0);
  return 0;
}
DECLARE_XAM_EXPORT1(XamSetDashContext, kNone, kImplemented);

dword_result_t XamGetDashContext_entry(const ppc_context_t& ctx) {
  return ctx->kernel_state->dash_context_;
}
DECLARE_XAM_EXPORT1(XamGetDashContext, kNone, kImplemented);

dword_result_t XamShowMarketplaceUIEx_entry(dword_t user_index, dword_t ui_type,
                                            qword_t offer_id, dword_t offer_type,
                                            dword_t content_category,
                                            unknown_t unk6, unknown_t unk7,
                                            dword_t title_id) {
  // No marketplace headless; report as if the user backed out.
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUIEx, kUI, kSketchy);

dword_result_t XamShowMarketplaceUI_entry(dword_t user_index, dword_t ui_type,
                                          qword_t offer_id, dword_t offer_type,
                                          dword_t content_category,
                                          dword_t title_id) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceUI, kUI, kSketchy);

dword_result_t XamShowMarketplaceDownloadItemsUI_entry(
    dword_t user_index, dword_t ui_type, lpqword_t offers, dword_t num_offers,
    lpdword_t hresult_ptr, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (user_index >= XUserMaxUserCount || !offers || num_offers > 6) {
    return X_ERROR_INVALID_PARAMETER;
  }
  if (overlapped) {
    kernel_state()->CompleteOverlappedImmediate(overlapped,
                                                X_ERROR_FUNCTION_FAILED);
    return X_ERROR_IO_PENDING;
  }
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowMarketplaceDownloadItemsUI, kUI, kSketchy);

dword_result_t XamShowForcedNameChangeUI_entry(dword_t user_index) {
  return X_ERROR_FUNCTION_FAILED;
}
DECLARE_XAM_EXPORT1(XamShowForcedNameChangeUI, kUI, kImplemented);

// Sign-in dialogs: the libretro core auto-signs-in a profile before the title
// boots, so report the UI as shown and immediately dismissed and succeed.
static X_RESULT ShowSigninUi(uint32_t users_needed) {
  if (users_needed != 1 && users_needed != 2 && users_needed != 4) {
    return X_ERROR_INVALID_PARAMETER;
  }
  NotifyUiShownBriefly();
  return X_ERROR_SUCCESS;
}

dword_result_t XamShowSigninUI_entry(dword_t users_needed, dword_t flags) {
  return ShowSigninUi(users_needed);
}
DECLARE_XAM_EXPORT1(XamShowSigninUI, kUserProfiles, kImplemented);

dword_result_t XamShowSigninUIEx_entry(
    dword_t users_needed, dword_t flags,
    pointer_t<XAM_OVERLAPPED> overlapped_ptr) {
  X_RESULT result = ShowSigninUi(users_needed);
  if (overlapped_ptr) {
    kernel_state()->CompleteOverlappedImmediate(overlapped_ptr, result);
    return X_ERROR_IO_PENDING;
  }
  return result;
}
DECLARE_XAM_EXPORT1(XamShowSigninUIEx, kUserProfiles, kSketchy);

dword_result_t XamShowNuiSigninUI_entry(dword_t unk, dword_t user_index,
                                        dword_t flags) {
  return ShowSigninUi(1);
}
DECLARE_XAM_EXPORT1(XamShowNuiSigninUI, kUserProfiles, kSketchy);

dword_result_t XamShowSigninUIp_entry(dword_t user_index, dword_t users_needed,
                                      dword_t flags) {
  return ShowSigninUi(users_needed);
}
DECLARE_XAM_EXPORT1(XamShowSigninUIp, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUIEx_entry(dword_t user_index, dword_t flag,
                                              lpstring_t unkn2_ptr) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUIEx, kUserProfiles, kImplemented);

dword_result_t XamShowCreateProfileUI_entry(dword_t user_index, dword_t flag) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowCreateProfileUI, kUserProfiles, kImplemented);

dword_result_t XamShowAchievementsUI_entry(dword_t user_index,
                                           dword_t title_id) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowAchievementsUI, kUserProfiles, kImplemented);

dword_result_t XamShowGamerCardUI_entry(dword_t user_index) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowGamerCardUI, kUserProfiles, kImplemented);

dword_result_t XamShowEditProfileUI_entry(dword_t user_index) {
  return X_ERROR_SUCCESS;
}
DECLARE_XAM_EXPORT1(XamShowEditProfileUI, kUserProfiles, kImplemented);

void RegisterUIExports(xe::cpu::ExportResolver* export_resolver,
                       xe::kernel::KernelState* kernel_state) {
  // Individual exports self-register via the DECLARE_XAM_EXPORT macros above.
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
