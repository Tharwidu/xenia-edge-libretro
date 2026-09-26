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
// Unleashed stalls after Start on XamShowDeviceSelectorUI /
// XamShowMessageBoxUI).
//
// These implementations mirror the `cvars::headless` branches of the real
// xam_ui.cc: auto-complete each dialog with a sensible default and drive the
// XN_SYS_UI on/off notification so waiting titles proceed. They intentionally
// avoid the UI-thread dispatch helpers (xeXamDispatchHeadlessAsync), which need
// a display window that does not exist here.
//
// Where upstream's headless branch answers with nothing at all, this build
// answers with what the windowed dialog would have been seeded with (the
// message box's steered button, the keyboard's gamertag), because a title that
// gets an empty answer tends to ask again forever.

#include <algorithm>
#include <chrono>
#include <cstring>
#include <functional>
#include <initializer_list>
#include <thread>
#include <vector>

#include "xenia/base/byte_order.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/string.h"
#include "xenia/base/string_util.h"
#include "xenia/base/threading.h"
#include "xenia/base/utf8.h"
#include "xenia/kernel/guest_scheduler.h"
#include "xenia/kernel/kernel.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/headless_keyboard.h"
#include "xenia/kernel/xam/profile_manager.h"
#include "xenia/kernel/xam/xam_content_device.h"
#include "xenia/kernel/xam/xam_private.h"
#include "xenia/kernel/xevent.h"
#include "xenia/kernel/xthread.h"
#include "xenia/memory.h"
#include "xenia/xbox.h"

// Headless: games (e.g. Sonic Unleashed) pop a "no save data - save / don't
// save / select device" message box that a windowed Xenia shows and waits on.
// We must answer it blind; -1 uses the game's own default/focused button, or
// set a 0-based index to force a specific choice per game.
DEFINE_int32(headless_messagebox_button, -1,
             "Which message-box button the headless (libretro) core auto-picks "
             "(-1 = the game's default focused button).",
             "HID");

// Headless: a title that opens the virtual keyboard with an empty box (e.g.
// Skate 2 asking for a name) is waiting for the player to type. Empty back is
// not an answer, so the core types the signed-in gamertag; set this to type
// something else instead, globally or per title.
DEFINE_string(headless_keyboard_text, "",
              "Text the headless (libretro) core enters when a title opens the "
              "virtual keyboard with no default (empty = the signed-in "
              "profile's gamertag).",
              "HID");

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

// True if the lowercase haystack contains any of the needles.
static bool ContainsAnyOf(const std::string& haystack_lower,
                          std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (haystack_lower.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// The two boot-prompt families that hang headless when answered with the
// game's default button:
// - Save/storage prompts (e.g. Sonic Unleashed's "no save data" box, buttons
//   [Storage device select | Save | Don't save]): the default opens a nested
//   storage-device selector that dead-ends invisibly; "Save" proceeds (the
//   device selector auto-picks a device headless).
// - Xbox LIVE / online prompts ("connect to Xbox LIVE or play offline?"):
//   the default is the sign-in path, which walks into profile-configuration
//   UI that can't be shown.
// Returns the index of the button that keeps the game moving, or -1 when the
// prompt isn't recognized (caller keeps the game's default button).
static int32_t FindAutoButton(const std::string& title_lower,
                              const std::string& text_lower,
                              const std::vector<std::string>& buttons_lower) {
  if (buttons_lower.size() < 2) {
    return -1;
  }
  const std::string context = title_lower + " " + text_lower;

  if (ContainsAnyOf(context, {"save", "storage", "memory unit"})) {
    // An affirmative "save" button, skipping negated ("don't save") and
    // selector ("storage device select") wordings. Exact/leading "save"
    // outranks phrases that merely mention saving.
    for (size_t i = 0; i < buttons_lower.size(); ++i) {
      const std::string& label = buttons_lower[i];
      if (label.rfind("save", 0) == 0 &&
          !ContainsAnyOf(label, {"select", "choose", "change"})) {
        return static_cast<int32_t>(i);
      }
    }
    for (size_t i = 0; i < buttons_lower.size(); ++i) {
      const std::string& label = buttons_lower[i];
      if (label.find("save") != std::string::npos &&
          !ContainsAnyOf(label, {"don't", "do not", "not save", "select",
                                 "choose", "change"})) {
        return static_cast<int32_t>(i);
      }
    }
    for (size_t i = 0; i < buttons_lower.size(); ++i) {
      const std::string& label = buttons_lower[i];
      if (label == "yes" || ContainsAnyOf(label, {"continue", "proceed"})) {
        return static_cast<int32_t>(i);
      }
    }
    // Not conclusive; a prompt can mention both saving and online, so fall
    // through to the online check.
  }

  if (ContainsAnyOf(context,
                    {"xbox live", "sign in", "sign-in", "signin", "online",
                     "network", "internet", "multiplayer"})) {
    // Strongest match first: an explicitly "offline"-worded button, then the
    // usual decline wordings.
    for (size_t i = 0; i < buttons_lower.size(); ++i) {
      if (ContainsAnyOf(buttons_lower[i], {"offline", "hors ligne", "sin conex",
                                           "senza connessione"})) {
        return static_cast<int32_t>(i);
      }
    }
    for (size_t i = 0; i < buttons_lower.size(); ++i) {
      const std::string& label = buttons_lower[i];
      if (label == "no" || label == "non" || label == "nein" ||
          ContainsAnyOf(label, {"continue", "don't", "do not", "without",
                                "later", "skip", "cancel", "not now"})) {
        return static_cast<int32_t>(i);
      }
    }
  }
  return -1;
}

using HostFinish = std::function<X_RESULT()>;
static bool RunHostPrompt(
    const std::function<bool(std::function<void(HostFinish)>)>& start,
    uint32_t overlapped, X_RESULT* sync_result);
static HeadlessMessageBoxHandler headless_message_box_handler;

void SetHeadlessMessageBoxHandler(HeadlessMessageBoxHandler handler) {
  headless_message_box_handler = std::move(handler);
}

static dword_result_t ShowMessageBoxUi(
    dword_t user_index, lpu16string_t title_ptr, lpu16string_t text_ptr,
    dword_t button_count, lpdword_t button_ptrs, dword_t active_button,
    dword_t flags, pointer_t<MESSAGEBOX_RESULT> result_ptr,
    pointer_t<XAM_OVERLAPPED> overlapped) {
  // Log what the title is asking, since headless we answer it blind.
  std::string title = title_ptr ? xe::to_utf8(title_ptr.value()) : "";
  std::string text = text_ptr ? xe::to_utf8(text_ptr.value()) : "";
  std::string buttons;
  std::vector<std::string> labels;
  std::vector<std::string> buttons_lower;
  for (uint32_t i = 0; i < button_count; ++i) {
    auto b = xe::load_and_swap<std::u16string>(
        kernel_state()->memory()->TranslateVirtual(button_ptrs[i]));
    std::string label = xe::to_utf8(b);
    buttons += (i ? " | " : "") + label;
    labels.push_back(label);
    buttons_lower.push_back(xe::utf8::lower_ascii(label));
  }

  // Default to the game's focused button; a per-game override lets the user
  // steer prompts that need a specific choice (e.g. "continue without saving").
  uint32_t chosen = static_cast<uint32_t>(active_button);
  const bool forced =
      cvars::headless_messagebox_button >= 0 &&
      cvars::headless_messagebox_button < static_cast<int32_t>(button_count);
  if (forced) {
    chosen = static_cast<uint32_t>(cvars::headless_messagebox_button);
  } else {
    // In auto mode, steer recognized save/storage and Xbox LIVE / online
    // prompts to a choice that keeps the game moving - the game's own
    // default on both leads into UI that headless can't show.
    int32_t auto_button =
        FindAutoButton(xe::utf8::lower_ascii(title),
                       xe::utf8::lower_ascii(text), buttons_lower);
    if (auto_button >= 0) {
      chosen = static_cast<uint32_t>(auto_button);
    }
  }
  XELOGI(
      "Headless message box: title='{}' text='{}' buttons=[{}] active={} -> "
      "answering button {}",
      title, text, buttons, uint32_t(active_button), chosen);

  // Let the player choose, starting on that answer, unless a button was
  // forced for this title.
  if (!forced && headless_message_box_handler && button_count) {
    HeadlessMessageBoxRequest request;
    request.title = title;
    request.text = text;
    request.buttons = labels;
    request.suggested_button = std::min(chosen, uint32_t(button_count) - 1);
    X_RESULT host_result = X_ERROR_SUCCESS;
    if (RunHostPrompt(
            [&](std::function<void(HostFinish)> complete) {
              return headless_message_box_handler(
                  request, [complete, result_ptr](bool accepted,
                                                  uint32_t button) {
                    complete([=]() -> X_RESULT {
                      if (!accepted) {
                        return X_ERROR_CANCELLED;
                      }
                      result_ptr->ButtonPressed = button;
                      return X_ERROR_SUCCESS;
                    });
                  });
            },
            overlapped.guest_address(), &host_result)) {
      return host_result;
    }
  }

  return DispatchHeadless(
      [result_ptr, chosen]() -> X_RESULT {
        result_ptr->ButtonPressed = chosen;
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

// Gamertag of the profile signed in to `user_index`, falling back to slot 0 -
// the slot the core auto-signs-in at boot. Empty when there is no profile.
static std::string SignedInGamertag(uint32_t user_index) {
  auto* xam = kernel_state() ? kernel_state()->xam_state() : nullptr;
  auto* profiles = xam ? xam->profile_manager() : nullptr;
  if (!profiles) {
    return "";
  }
  // GetProfile understands the sentinel indices a title can pass (any, latest,
  // none) as well as a real slot, and returns null when nobody is signed in
  // there; slot 0 is the one the core signs in at boot.
  UserProfile* profile = profiles->GetProfile(static_cast<uint8_t>(user_index));
  if (!profile) {
    profile = profiles->GetProfile(static_cast<uint8_t>(0));
  }
  return profile ? profile->name() : "";
}

static HeadlessKeyboardHandler headless_keyboard_handler;

void SetHeadlessKeyboardHandler(HeadlessKeyboardHandler handler) {
  headless_keyboard_handler = std::move(handler);
}

// A prompt answered by the host's own UI. start() hands the request over and
// returns false if the host declines; otherwise it calls complete() exactly
// once, from any thread, with a finish() that writes the answer into guest
// memory and returns the result. A synchronous call parks its guest thread
// until then. An overlapped one is marked pending on the requesting guest
// thread (the context its completion routine is queued to) and completed when
// the host answers, without holding the kernel dispatch thread meanwhile.
static bool RunHostPrompt(
    const std::function<bool(std::function<void(HostFinish)>)>& start,
    uint32_t overlapped, X_RESULT* sync_result) {
  if (!overlapped) {
    struct State {
      xe::threading::Fence fence;
      HostFinish finish;
    };
    auto state = std::make_shared<State>();
    if (!start([state](HostFinish finish) {
          state->finish = std::move(finish);
          state->fence.Signal();
        })) {
      return false;
    }
    kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
    GuestScheduler::WaitOnFence(state->fence);
    *sync_result = state->finish();
    NotifyUiShownBriefly();
    return true;
  }

  auto* ptr = kernel_state()->memory()->TranslateVirtual(overlapped);
  XOverlappedSetResult(ptr, X_ERROR_IO_PENDING);
  XOverlappedSetContext(ptr, XThread::GetCurrentThreadHandle());
  if (X_HANDLE event_handle = XOverlappedGetEvent(ptr)) {
    auto ev = kernel_state()->object_table()->LookupObject<XObject>(
        event_handle);
    if (ev && ev->type() == XObject::Type::Event) {
      ev.get<XEvent>()->Reset();
    }
  }
  if (!start([overlapped](HostFinish finish) {
        if (!kernel_state()) {
          return;
        }
        X_RESULT result = finish();
        kernel_state()->CompleteOverlappedEx(overlapped, result, result, 0);
        NotifyUiShownBriefly();
      })) {
    XOverlappedSetResult(ptr, X_ERROR_SUCCESS);
    return false;
  }
  kernel_state()->BroadcastNotification(kXNotificationSystemUI, true);
  *sync_result = X_ERROR_IO_PENDING;
  return true;
}

// Hands a keyboard prompt to the host's UI. Returns false when there is none
// or it declines, so the caller auto-fills instead.
static bool ShowHostKeyboard(const HeadlessKeyboardRequest& request,
                             char16_t* buffer, uint32_t buffer_length,
                             uint32_t overlapped, X_RESULT* sync_result) {
  if (!headless_keyboard_handler) {
    return false;
  }
  return RunHostPrompt(
      [&](std::function<void(HostFinish)> complete) {
        return headless_keyboard_handler(
            request, [complete, buffer, buffer_length](
                         bool accepted, const std::string& text) {
              complete([=]() -> X_RESULT {
                if (!accepted) {
                  return X_ERROR_CANCELLED;
                }
                std::u16string text16 = xe::to_utf16(text);
                if (text16.empty()) {
                  std::memset(buffer, 0, size_t(buffer_length) * 2);
                } else {
                  string_util::copy_and_swap_truncating(buffer, text16,
                                                        buffer_length);
                }
                return X_ERROR_SUCCESS;
              });
            });
      },
      overlapped, sync_result);
}

dword_result_t XamShowKeyboardUI_entry(
    dword_t user_index, dword_t flags, lpu16string_t default_text,
    lpu16string_t title, lpu16string_t description, lpu16string_t buffer,
    dword_t buffer_length, pointer_t<XAM_OVERLAPPED> overlapped) {
  if (!buffer) {
    return X_ERROR_INVALID_PARAMETER;
  }

  // Whatever the title pre-filled the box with is what a player would see and
  // usually keep, so it wins.
  std::u16string text = default_text ? default_text.value() : std::u16string();
  const bool prefilled = !text.empty();
  if (!prefilled) {
    // Nothing pre-filled: the title wants the player to type a name (Skate 2's
    // name prompt is the reported case) and handing back the empty string it
    // started with just re-opens the keyboard. Type the gamertag, which is
    // what the windowed build seeds its dialog with.
    std::string entered = cvars::headless_keyboard_text;
    if (entered.empty()) {
      entered = SignedInGamertag(user_index);
    }
    text = xe::to_utf16(entered);
  }

  XELOGI(
      "Headless keyboard: title='{}' description='{}' flags={:08X} -> entering "
      "'{}' ({})",
      title ? xe::to_utf8(title.value()) : "",
      description ? xe::to_utf8(description.value()) : "", uint32_t(flags),
      xe::to_utf8(text), prefilled ? "title's own default" : "auto-filled");

  HeadlessKeyboardRequest request;
  request.title = title ? xe::to_utf8(title.value()) : "";
  request.description = description ? xe::to_utf8(description.value()) : "";
  request.default_text = default_text ? xe::to_utf8(default_text.value()) : "";
  request.suggested_text = xe::to_utf8(text);
  request.max_length = buffer_length ? uint32_t(buffer_length) - 1 : 0;
  request.flags = flags;
  X_RESULT host_result = X_ERROR_SUCCESS;
  if (ShowHostKeyboard(request, buffer, buffer_length, overlapped.guest_address(),
                       &host_result)) {
    return host_result;
  }

  auto buffer_size = static_cast<size_t>(buffer_length) * 2;
  return DispatchHeadless(
      [text, buffer, buffer_length, buffer_size]() -> X_RESULT {
        if (text.empty()) {
          std::memset(buffer, 0, buffer_size);
        } else {
          string_util::copy_and_swap_truncating(buffer, text, buffer_length);
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
                                            qword_t offer_id,
                                            dword_t offer_type,
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

// The XNA launcher asks for an indie game to run. Upstream picks one through
// a dialog and returns false without a window, which is what headless does.
bool xeXamChooseIndieGame(std::string* file_name, uint32_t* device_id,
                          std::string* display_name) {
  XELOGW("xeXamChooseIndieGame: no picker in the libretro core");
  return false;
}

void RegisterUIExports(xe::cpu::ExportResolver* export_resolver,
                       xe::kernel::KernelState* kernel_state) {
  // Individual exports self-register via the DECLARE_XAM_EXPORT macros above.
}

}  // namespace xam
}  // namespace kernel
}  // namespace xe
