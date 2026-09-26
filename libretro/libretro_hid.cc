/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro HID (Input Driver) Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_hid.h"

#include <array>
#include <cstring>
#include <string>

#include "xenia/base/byte_order.h"
#include "xenia/base/clock.h"
#include "xenia/ui/virtual_key.h"

namespace xe {
namespace hid {
namespace libretro_hid {

// ---- LibretroInputDriver --------------------------------------------------

LibretroInputDriver::LibretroInputDriver(xe::ui::Window* window,
                                         size_t window_z_order)
    : InputDriver(window, window_z_order) {
  // Mark all ports connected (RetroArch always has port 0).
  for (size_t i = 0; i < kMaxPorts; ++i) {
    states_[i].connected = (i == 0);
  }
}

LibretroInputDriver::~LibretroInputDriver() = default;

X_STATUS LibretroInputDriver::Setup() { return X_STATUS_SUCCESS; }

InputType LibretroInputDriver::GetInputType() const {
  return InputType::Controller;
}

X_RESULT LibretroInputDriver::GetCapabilities(uint32_t user_index,
                                               uint32_t flags,
                                               X_INPUT_CAPABILITIES* out_caps) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!states_[user_index].connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::memset(out_caps, 0, sizeof(*out_caps));
  out_caps->type = 0x01;      // XINPUT_DEVTYPE_GAMEPAD
  out_caps->sub_type = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
  // be<T> auto byte-swaps - assign native values
  out_caps->flags = X_INPUT_CAPS_FFB_SUPPORTED;

  // Report full gamepad capability
  out_caps->gamepad.buttons = 0xFFFF;
  out_caps->gamepad.left_trigger = 0xFF;
  out_caps->gamepad.right_trigger = 0xFF;
  out_caps->gamepad.thumb_lx = 32767;
  out_caps->gamepad.thumb_ly = 32767;
  out_caps->gamepad.thumb_rx = 32767;
  out_caps->gamepad.thumb_ry = 32767;
  out_caps->vibration.left_motor_speed = 0xFFFF;
  out_caps->vibration.right_motor_speed = 0xFFFF;

  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::GetState(uint32_t user_index,
                                        X_INPUT_STATE* out_state) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  const auto& s = states_[user_index];
  if (!s.connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::memset(out_state, 0, sizeof(*out_state));
  // be<T> auto byte-swaps - assign native values
  out_state->packet_number = s.packet_number;

  // Map libretro joypad buttons -> Xbox 360 button bitmask
  uint16_t btns = 0;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_UP])    btns |= X_INPUT_GAMEPAD_DPAD_UP;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_DOWN])  btns |= X_INPUT_GAMEPAD_DPAD_DOWN;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_LEFT])  btns |= X_INPUT_GAMEPAD_DPAD_LEFT;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_RIGHT]) btns |= X_INPUT_GAMEPAD_DPAD_RIGHT;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_START]) btns |= X_INPUT_GAMEPAD_START;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_SELECT])btns |= X_INPUT_GAMEPAD_BACK;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_L3])    btns |= X_INPUT_GAMEPAD_LEFT_THUMB;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_R3])    btns |= X_INPUT_GAMEPAD_RIGHT_THUMB;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_L])     btns |= X_INPUT_GAMEPAD_LEFT_SHOULDER;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_R])     btns |= X_INPUT_GAMEPAD_RIGHT_SHOULDER;
  // Libretro RetroPad uses SNES positional layout:
  //   B=south  A=east  Y=west  X=north
  // Xbox 360:
  //   A=south  B=east  X=west  Y=north
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_B])     btns |= X_INPUT_GAMEPAD_A;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_A])     btns |= X_INPUT_GAMEPAD_B;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_Y])     btns |= X_INPUT_GAMEPAD_X;
  if (s.buttons[RETRO_DEVICE_ID_JOYPAD_X])     btns |= X_INPUT_GAMEPAD_Y;

  out_state->gamepad.buttons = btns;

  // Triggers: libretro 0..+0x7FFF ??? Xbox 0..255
  out_state->gamepad.left_trigger =
      static_cast<uint8_t>((s.left_trigger * 255) / 0x7FFF);
  out_state->gamepad.right_trigger =
      static_cast<uint8_t>((s.right_trigger * 255) / 0x7FFF);

  // Sticks: libretro -0x7FFF..+0x7FFF ??? Xbox -32768..+32767 (Y axis inverted).
  int16_t lx = s.left_stick_x;
  int16_t ly = static_cast<int16_t>(-s.left_stick_y);
  int16_t rx = s.right_stick_x;
  int16_t ry = static_cast<int16_t>(-s.right_stick_y);

  if (lx > -X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
      lx <  X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) lx = 0;
  if (ly > -X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE &&
      ly <  X_INPUT_GAMEPAD_LEFT_THUMB_DEADZONE) ly = 0;
  if (rx > -X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
      rx <  X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) rx = 0;
  if (ry > -X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE &&
      ry <  X_INPUT_GAMEPAD_RIGHT_THUMB_DEADZONE) ry = 0;

  out_state->gamepad.thumb_lx = lx;
  out_state->gamepad.thumb_ly = ly;
  out_state->gamepad.thumb_rx = rx;
  out_state->gamepad.thumb_ry = ry;

  return X_ERROR_SUCCESS;
}

X_RESULT LibretroInputDriver::SetState(uint32_t user_index,
                                        X_INPUT_VIBRATION* vibration) {
  if (user_index >= kMaxPorts) return X_ERROR_DEVICE_NOT_CONNECTED;

  std::lock_guard<std::mutex> lock(state_mutex_);
  if (!states_[user_index].connected) return X_ERROR_DEVICE_NOT_CONNECTED;

  if (rumble_cb_ && vibration) {
    // be<T> auto-converts to native on read
    uint16_t strong = vibration->left_motor_speed;
    uint16_t weak   = vibration->right_motor_speed;
    rumble_cb_(static_cast<unsigned>(user_index),
               RETRO_RUMBLE_STRONG, strong);
    rumble_cb_(static_cast<unsigned>(user_index),
               RETRO_RUMBLE_WEAK, weak);
  }
  return X_ERROR_SUCCESS;
}

namespace {

// Matches the SDL driver: repeat after 400 ms, then every 100 ms; sticks
// count as pressed past 0x4E00, triggers past 0x1F.
constexpr uint32_t kRepeatDelay = 400;
constexpr uint32_t kRepeatRate = 100;
constexpr int16_t kThumbThreshold = 0x4E00;
constexpr uint8_t kTriggerThreshold = 0x1F;

// Bit i of the keyfield is event kVkLookup[i]; the order is also the order
// in which simultaneous changes are reported.
constexpr std::array<xe::ui::VirtualKey, 34> kVkLookup = {
    // 0-15: the real buttons, in X_INPUT_GAMEPAD bit order
    xe::ui::VirtualKey::kXInputPadDpadUp,
    xe::ui::VirtualKey::kXInputPadDpadDown,
    xe::ui::VirtualKey::kXInputPadDpadLeft,
    xe::ui::VirtualKey::kXInputPadDpadRight,
    xe::ui::VirtualKey::kXInputPadStart,
    xe::ui::VirtualKey::kXInputPadBack,
    xe::ui::VirtualKey::kXInputPadLThumbPress,
    xe::ui::VirtualKey::kXInputPadRThumbPress,
    xe::ui::VirtualKey::kXInputPadLShoulder,
    xe::ui::VirtualKey::kXInputPadRShoulder,
    xe::ui::VirtualKey::kXInputPadGuide,
    xe::ui::VirtualKey::kNone,
    xe::ui::VirtualKey::kXInputPadA,
    xe::ui::VirtualKey::kXInputPadB,
    xe::ui::VirtualKey::kXInputPadX,
    xe::ui::VirtualKey::kXInputPadY,
    // 16-17: triggers
    xe::ui::VirtualKey::kXInputPadLTrigger,
    xe::ui::VirtualKey::kXInputPadRTrigger,
    // 18-25: left stick directions
    xe::ui::VirtualKey::kXInputPadLThumbUp,
    xe::ui::VirtualKey::kXInputPadLThumbDown,
    xe::ui::VirtualKey::kXInputPadLThumbRight,
    xe::ui::VirtualKey::kXInputPadLThumbLeft,
    xe::ui::VirtualKey::kXInputPadLThumbUpLeft,
    xe::ui::VirtualKey::kXInputPadLThumbUpRight,
    xe::ui::VirtualKey::kXInputPadLThumbDownRight,
    xe::ui::VirtualKey::kXInputPadLThumbDownLeft,
    // 26-33: right stick directions
    xe::ui::VirtualKey::kXInputPadRThumbUp,
    xe::ui::VirtualKey::kXInputPadRThumbDown,
    xe::ui::VirtualKey::kXInputPadRThumbRight,
    xe::ui::VirtualKey::kXInputPadRThumbLeft,
    xe::ui::VirtualKey::kXInputPadRThumbUpLeft,
    xe::ui::VirtualKey::kXInputPadRThumbUpRight,
    xe::ui::VirtualKey::kXInputPadRThumbDownRight,
    xe::ui::VirtualKey::kXInputPadRThumbDownLeft,
};

// Triggers and stick directions as extra "buttons" above bit 15.
uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) {
  uint64_t f = 0;
  f |= uint64_t(gamepad.left_trigger > kTriggerThreshold) << 16;
  f |= uint64_t(gamepad.right_trigger > kTriggerThreshold) << 17;
  int16_t thumb_x = gamepad.thumb_lx;
  int16_t thumb_y = gamepad.thumb_ly;
  for (size_t i = 0; i <= 8; i += 8) {
    uint64_t u = thumb_y > kThumbThreshold;
    uint64_t d = thumb_y < ~kThumbThreshold;
    uint64_t r = thumb_x > kThumbThreshold;
    uint64_t l = thumb_x < ~kThumbThreshold;
    if (u && l) { u = l = 0; f |= uint64_t(1) << (22 + i); }
    if (u && r) { u = r = 0; f |= uint64_t(1) << (23 + i); }
    if (d && r) { d = r = 0; f |= uint64_t(1) << (24 + i); }
    if (d && l) { d = l = 0; f |= uint64_t(1) << (25 + i); }
    f |= u << (18 + i);
    f |= d << (19 + i);
    f |= r << (20 + i);
    f |= l << (21 + i);
    thumb_x = gamepad.thumb_rx;
    thumb_y = gamepad.thumb_ry;
  }
  return f;
}

}  // namespace

X_RESULT LibretroInputDriver::GetKeystroke(uint32_t user_index, uint32_t flags,
                                            X_INPUT_KEYSTROKE* out_keystroke) {
  const bool user_any = user_index == XUserIndexAny;
  if (!out_keystroke || (user_index >= kMaxPorts && !user_any)) {
    return X_ERROR_BAD_ARGUMENTS;
  }
  const uint32_t first = user_any ? 0 : user_index;
  const uint32_t last = user_any ? uint32_t(kMaxPorts) : user_index + 1;
  for (uint32_t port = first; port < last; ++port) {
    X_INPUT_STATE state;
    if (GetState(port, &state) != X_ERROR_SUCCESS) {
      if (user_any) continue;
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    const uint64_t buttons =
        uint64_t(uint16_t(state.gamepad.buttons)) |
        AnalogToKeyfield(state.gamepad);

    std::lock_guard<std::mutex> lock(keystroke_mutex_);
    KeystrokeState& ks = keystroke_states_[port];
    auto emit = [&](uint8_t idx, uint16_t event_flags) {
      out_keystroke->virtual_key = uint16_t(kVkLookup[idx]);
      out_keystroke->unicode = 0;
      out_keystroke->user_index = uint8_t(port);
      out_keystroke->hid_code = 0;
      out_keystroke->flags = event_flags;
    };

    const uint32_t now = uint32_t(Clock::QueryGuestUptimeMillis());
    if (ks.repeat_state == RepeatState::Waiting &&
        ks.repeat_time + kRepeatDelay < now) {
      ks.repeat_state = RepeatState::Repeating;
    }
    if (ks.repeat_state == RepeatState::Repeating &&
        ks.repeat_time + kRepeatRate < now) {
      ks.repeat_time = now;
      emit(ks.repeat_butt_idx,
           X_INPUT_KEYSTROKE_KEYDOWN | X_INPUT_KEYSTROKE_REPEAT);
      return X_ERROR_SUCCESS;
    }

    const uint64_t changed = buttons ^ ks.buttons;
    if (!changed) continue;
    // Ups before downs, as XInput does: a stick moving from UPLEFT to LEFT
    // releases UPLEFT before LEFT goes down.
    for (int pass = 0; pass < 2; ++pass) {
      const bool clear_pass = pass == 0;
      for (uint8_t i = 0; i < uint8_t(kVkLookup.size()); ++i) {
        const uint64_t bit = uint64_t(1) << i;
        if (!(changed & bit) || kVkLookup[i] == xe::ui::VirtualKey::kNone) {
          continue;
        }
        const bool pressed = (buttons & bit) != 0;
        if (clear_pass && !pressed) {
          emit(i, X_INPUT_KEYSTROKE_KEYUP);
          ks.buttons &= ~bit;
          ks.repeat_state = RepeatState::Idle;
          return X_ERROR_SUCCESS;
        }
        if (!clear_pass && pressed) {
          emit(i, X_INPUT_KEYSTROKE_KEYDOWN);
          ks.buttons |= bit;
          ks.repeat_state = RepeatState::Waiting;
          ks.repeat_butt_idx = i;
          ks.repeat_time = now;
          return X_ERROR_SUCCESS;
        }
      }
    }
  }
  return X_ERROR_EMPTY;
}

void LibretroInputDriver::UpdateFromLibretro(
    retro_input_state_t input_state_cb, bool use_bitmasks) {
  if (!input_state_cb) return;

  std::lock_guard<std::mutex> lock(state_mutex_);

  for (size_t port = 0; port < kMaxPorts; ++port) {
    auto& s = states_[port];

    // Read digital buttons - one call for all sixteen where the frontend
    // supports it, sixteen calls where it does not.
    bool any_input = false;
    if (use_bitmasks) {
      const int16_t mask = input_state_cb(
          static_cast<unsigned>(port), RETRO_DEVICE_JOYPAD, 0,
          RETRO_DEVICE_ID_JOYPAD_MASK);
      for (int id = 0; id < 16; ++id) {
        s.buttons[id] = (mask & (1 << id)) ? 1 : 0;
      }
      if (mask) any_input = true;
    } else {
      for (int id = 0; id < 16; ++id) {
        s.buttons[id] = input_state_cb(
            static_cast<unsigned>(port), RETRO_DEVICE_JOYPAD, 0, id);
        if (s.buttons[id]) any_input = true;
      }
    }

    // Read analog sticks
    s.left_stick_x = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    s.left_stick_y = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    s.right_stick_x = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    s.right_stick_y = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);

    if (s.left_stick_x || s.left_stick_y ||
        s.right_stick_x || s.right_stick_y)
      any_input = true;

    // Read analog triggers (L2/R2): try analog first, fallback to digital.
    int16_t lt = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_L2);
    int16_t rt = input_state_cb(
        static_cast<unsigned>(port), RETRO_DEVICE_ANALOG,
        RETRO_DEVICE_INDEX_ANALOG_BUTTON, RETRO_DEVICE_ID_JOYPAD_R2);

    if (lt == 0 && s.buttons[RETRO_DEVICE_ID_JOYPAD_L2])
      lt = 0x7FFF;  // digital fallback
    if (rt == 0 && s.buttons[RETRO_DEVICE_ID_JOYPAD_R2])
      rt = 0x7FFF;

    s.left_trigger = lt;
    s.right_trigger = rt;
    if (lt || rt) any_input = true;

    // A port is connected if the frontend assigned a device to it.
    s.connected = port_connected_[port];

    if (any_input && s.connected) s.packet_number++;
  }
}

std::vector<InputDeviceInfo> LibretroInputDriver::EnumerateDevices() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  std::vector<InputDeviceInfo> out;
  for (size_t port = 0; port < kMaxPorts; ++port) {
    if (!port_connected_[port]) continue;
    InputDeviceInfo info;
    info.driver_slot = static_cast<uint8_t>(port);
    // Stable per-port id so the binding survives disconnect/reconnect.
    info.stable_id = "libretro_port_" + std::to_string(port);
    info.display_name = "RetroPad " + std::to_string(port + 1);
    info.subtype = 0x01;  // XINPUT_DEVSUBTYPE_GAMEPAD
    info.preferred_slot = static_cast<int8_t>(port);
    info.auto_bind = true;
    out.push_back(std::move(info));
  }
  return out;
}

void LibretroInputDriver::SetRumbleCallback(retro_set_rumble_state_t cb) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  rumble_cb_ = cb;
}

void LibretroInputDriver::SetPortDevice(unsigned port, unsigned device) {
  if (port >= kMaxPorts) return;
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    // RETRO_DEVICE_NONE (0) = disconnected, other = connected.
    const bool connected = (device != RETRO_DEVICE_NONE);
    changed = (port_connected_[port] != connected);
    port_connected_[port] = connected;
    states_[port].connected = connected;
    if (!connected) {
      // Clear state for disconnected port
      std::memset(&states_[port], 0, sizeof(states_[port]));
    }
  }
  // Connectivity changed: ask InputSystem to re-run its binding pass so the
  // port attaches to (or vacates) a guest slot. Released outside the lock —
  // the callback may reconcile on the caller's thread.
  if (changed) NotifyDevicesChanged();
}

// ---- Factory --------------------------------------------------------------

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order) {
  return std::make_unique<LibretroInputDriver>(window, window_z_order);
}

}  // namespace libretro_hid
}  // namespace hid
}  // namespace xe
