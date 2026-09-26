/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * On-screen keyboard. See libretro_keyboard.h.
 */

#include "libretro_keyboard.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "libretro_font.h"
#include "xenia/kernel/xam/headless_keyboard.h"

namespace xe {
namespace libretro_keyboard {

namespace {

namespace xam = xe::kernel::xam;

// Buttons as the Xbox pad names them; the core maps RetroPad B to Xbox A,
// RetroPad A to Xbox B, Y to X and X to Y (libretro_hid.cc).
enum Button {
  kUp, kDown, kLeft, kRight, kA, kB, kX, kY, kStart, kBack, kButtonCount
};
constexpr unsigned kRetroIds[kButtonCount] = {
    RETRO_DEVICE_ID_JOYPAD_UP,    RETRO_DEVICE_ID_JOYPAD_DOWN,
    RETRO_DEVICE_ID_JOYPAD_LEFT,  RETRO_DEVICE_ID_JOYPAD_RIGHT,
    RETRO_DEVICE_ID_JOYPAD_B,     RETRO_DEVICE_ID_JOYPAD_A,
    RETRO_DEVICE_ID_JOYPAD_Y,     RETRO_DEVICE_ID_JOYPAD_X,
    RETRO_DEVICE_ID_JOYPAD_START, RETRO_DEVICE_ID_JOYPAD_SELECT};

constexpr int kColumns = 10;
constexpr int kCharRows = 4;
constexpr int kActionRow = kCharRows;  // Shift, Space, Del, Done
constexpr int kActions = 4;
const char* const kRows[2][kCharRows] = {
    {"1234567890", "qwertyuiop", "asdfghjkl'", "zxcvbnm,.-"},
    {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL\"", "ZXCVBNM;:_"}};
const char* const kActionLabels[kActions] = {"Shift", "Space", "Del", "Done"};

// Frames a held direction waits before repeating, then between repeats.
constexpr int kRepeatDelay = 24;
constexpr int kRepeatRate = 5;

std::mutex mutex;
Mode mode = Mode::kAskPrefilled;

bool active = false;
xam::HeadlessKeyboardRequest request;
xam::HeadlessKeyboardDone done;
std::string text;
int row = 1, col = 0;
bool shift = false;
bool held[kButtonCount] = {};
int held_frames[kButtonCount] = {};
bool wait_for_release = false;
unsigned frame = 0;

// ---- prompt state (mutex held) ---------------------------------------------

void Finish(bool accepted) {
  if (!active) return;
  active = false;
  wait_for_release = true;
  auto callback = std::move(done);
  std::string answer = text;
  done = nullptr;
  // The callback writes guest memory and completes the title's request. It
  // never calls back into this module, so running it under our lock is safe.
  if (callback) callback(accepted, answer);
}

void Type(char c) {
  if (text.size() < request.max_length) text.push_back(c);
}

void Backspace() {
  if (!text.empty()) text.pop_back();
}

void Press() {
  if (row < kCharRows) {
    Type(kRows[shift ? 1 : 0][row][col]);
    return;
  }
  switch (col) {
    case 0: shift = !shift; break;
    case 1: Type(' '); break;
    case 2: Backspace(); break;
    case 3: Finish(true); break;
  }
}

void Move(int dr, int dc) {
  if (dr) {
    int next = std::clamp(row + dr, 0, kActionRow);
    if (next != row) {
      // The action row has four wide keys under ten narrow ones.
      if (next == kActionRow) {
        col = std::min(col * kActions / kColumns, kActions - 1);
      } else if (row == kActionRow) {
        col = std::min(col * kColumns / kActions + 1, kColumns - 1);
      }
      row = next;
    }
  }
  if (dc) {
    int width = row == kActionRow ? kActions : kColumns;
    col = (col + dc + width) % width;
  }
}

// True on the frame a button goes down, and at the repeat rate while held.
bool Pressed(Button b, bool repeat) {
  if (!held[b]) return false;
  int f = held_frames[b];
  if (f == 1) return true;
  return repeat && f > kRepeatDelay && (f - kRepeatDelay) % kRepeatRate == 0;
}

// ---- drawing -----------------------------------------------------------------

struct Canvas {
  uint8_t* pixels;
  int w, h;
  size_t pitch;
  bool bgra;

  void Put(int x, int y, uint8_t r, uint8_t g, uint8_t b) const {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint8_t* p = pixels + size_t(y) * pitch + size_t(x) * 4;
    if (bgra) {
      p[0] = b; p[1] = g; p[2] = r;
    } else {
      p[0] = r; p[1] = g; p[2] = b;
    }
  }
  void Fill(int x, int y, int fw, int fh, uint8_t r, uint8_t g,
            uint8_t b) const {
    int x0 = std::max(x, 0), y0 = std::max(y, 0);
    int x1 = std::min(x + fw, w), y1 = std::min(y + fh, h);
    for (int yy = y0; yy < y1; ++yy) {
      for (int xx = x0; xx < x1; ++xx) Put(xx, yy, r, g, b);
    }
  }
  void Dim() const {
    for (int y = 0; y < h; ++y) {
      uint8_t* p = pixels + size_t(y) * pitch;
      for (int x = 0; x < w * 4; ++x) p[x] = uint8_t(p[x] * 3 / 8);
    }
  }
  // Draws text at scale s; returns the x after the last glyph.
  int Text(int x, int y, const std::string& s, int scale, uint8_t r,
           uint8_t g, uint8_t b) const {
    for (char ch : s) {
      unsigned char c = static_cast<unsigned char>(ch);
      if (c < kFontFirst || c > kFontLast) c = '?';
      const uint8_t* glyph = kFontGlyphs[c - kFontFirst];
      for (int gy = 0; gy < kFontHeight; ++gy) {
        for (int gx = 0; gx < kFontWidth; ++gx) {
          if (glyph[gy] & (0x80 >> gx)) {
            Fill(x + gx * scale, y + gy * scale, scale, scale, r, g, b);
          }
        }
      }
      x += kFontWidth * scale;
    }
    return x;
  }
};

// Layout in units that get multiplied by the scale.
constexpr int kKeyW = 28, kKeyH = 26, kGap = 4, kMargin = 14;
constexpr int kGridW = kColumns * kKeyW + (kColumns - 1) * kGap;
constexpr int kPanelW = kGridW + 2 * kMargin;
constexpr int kLine = 20;
constexpr int kDescLines = 3;
constexpr int kFieldH = 28;
constexpr int kPanelH = kMargin + kLine + kDescLines * kLine + 6 + kFieldH +
                        10 + (kCharRows + 1) * (kKeyH + kGap) + kLine +
                        kMargin;

std::vector<std::string> Wrap(const std::string& s, size_t width,
                              size_t max_lines) {
  std::vector<std::string> lines;
  size_t pos = 0;
  while (pos < s.size() && lines.size() < max_lines) {
    size_t end = std::min(pos + width, s.size());
    if (end < s.size()) {
      size_t space = s.rfind(' ', end);
      if (space != std::string::npos && space > pos) end = space;
    }
    std::string line = s.substr(pos, end - pos);
    if (lines.size() + 1 == max_lines && end < s.size()) {
      line = line.substr(0, width > 3 ? width - 3 : width) + "...";
    }
    lines.push_back(line);
    pos = end;
    while (pos < s.size() && s[pos] == ' ') ++pos;
  }
  return lines;
}

// ASCII only: the glyphs cover printable ASCII.
std::string Printable(const std::string& s) {
  std::string out;
  for (unsigned char c : s) {
    if (c == '\n' || c == '\r' || c == '\t') {
      out.push_back(' ');
    } else if (c >= kFontFirst && c <= kFontLast) {
      out.push_back(char(c));
    } else if (c < 0x80) {
      continue;
    } else if ((c & 0xC0) != 0x80) {
      out.push_back('?');  // one per UTF-8 sequence
    }
  }
  return out;
}

// ---- handler / keyboard callback -------------------------------------------

bool HandleRequest(const xam::HeadlessKeyboardRequest& req,
                   xam::HeadlessKeyboardDone callback) {
  std::lock_guard<std::mutex> lock(mutex);
  if (mode == Mode::kAutoFill || active) return false;
  request = req;
  done = std::move(callback);
  text = Printable(mode == Mode::kAskPrefilled ? req.suggested_text
                                               : req.default_text);
  if (text.size() > request.max_length) text.resize(request.max_length);
  row = 1;
  col = 0;
  shift = false;
  active = true;
  return true;
}

void RETRO_CALLCONV OnKey(bool down, unsigned keycode, uint32_t character,
                          uint16_t) {
  if (!down) return;
  std::lock_guard<std::mutex> lock(mutex);
  if (!active) return;
  switch (keycode) {
    case RETROK_BACKSPACE: Backspace(); return;
    case RETROK_RETURN:
    case RETROK_KP_ENTER: Finish(true); return;
    case RETROK_ESCAPE: Finish(false); return;
    default: break;
  }
  if (character >= kFontFirst && character <= kFontLast) {
    Type(char(character));
  }
}

}  // namespace

void SetMode(Mode m) {
  std::lock_guard<std::mutex> lock(mutex);
  mode = m;
}

void Install(retro_environment_t environ_cb) {
  xam::SetHeadlessKeyboardHandler(HandleRequest);
  static struct retro_keyboard_callback keyboard = {OnKey};
  environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard);
}

bool SuppressGuestInput() {
  std::lock_guard<std::mutex> lock(mutex);
  return active || wait_for_release;
}

void Update(retro_input_state_t input_state) {
  std::lock_guard<std::mutex> lock(mutex);
  ++frame;
  bool any = false;
  for (int b = 0; b < kButtonCount; ++b) {
    bool down = input_state &&
                input_state(0, RETRO_DEVICE_JOYPAD, 0, kRetroIds[b]) != 0;
    held[b] = down;
    held_frames[b] = down ? held_frames[b] + 1 : 0;
    any |= down;
  }
  // The left stick navigates too.
  if (input_state) {
    int16_t ax = input_state(0, RETRO_DEVICE_ANALOG,
                             RETRO_DEVICE_INDEX_ANALOG_LEFT,
                             RETRO_DEVICE_ID_ANALOG_X);
    int16_t ay = input_state(0, RETRO_DEVICE_ANALOG,
                             RETRO_DEVICE_INDEX_ANALOG_LEFT,
                             RETRO_DEVICE_ID_ANALOG_Y);
    const int16_t kDeadzone = 16000;
    auto stick = [&](Button b, bool down) {
      if (down && !held[b]) {
        held[b] = true;
        held_frames[b] = held_frames[b] ? held_frames[b] : 1;
      }
      any |= down;
    };
    stick(kLeft, ax < -kDeadzone);
    stick(kRight, ax > kDeadzone);
    stick(kUp, ay < -kDeadzone);
    stick(kDown, ay > kDeadzone);
  }
  if (!active) {
    if (!any) wait_for_release = false;
    return;
  }
  if (Pressed(kUp, true)) Move(-1, 0);
  if (Pressed(kDown, true)) Move(1, 0);
  if (Pressed(kLeft, true)) Move(0, -1);
  if (Pressed(kRight, true)) Move(0, 1);
  if (Pressed(kA, true)) Press();
  if (Pressed(kB, true)) Backspace();
  if (Pressed(kX, true)) Type(' ');
  if (Pressed(kY, false)) shift = !shift;
  if (Pressed(kStart, false)) Finish(true);
  if (Pressed(kBack, false)) Finish(false);
}

void DrawOverlay(uint8_t* pixels, uint32_t width, uint32_t height,
                 size_t pitch, bool bgra) {
  std::lock_guard<std::mutex> lock(mutex);
  if (!active || !pixels) return;
  Canvas c{pixels, int(width), int(height), pitch, bgra};
  int s = std::max(1, std::min(int(width) / (kPanelW + 20),
                               int(height) / (kPanelH + 20)));
  c.Dim();

  int px = (c.w - kPanelW * s) / 2, py = (c.h - kPanelH * s) / 2;
  c.Fill(px - 2 * s, py - 2 * s, (kPanelW + 4) * s, (kPanelH + 4) * s, 16,
         124, 16);
  c.Fill(px, py, kPanelW * s, kPanelH * s, 28, 28, 34);

  const size_t chars = size_t(kGridW / kFontWidth);
  int x = px + kMargin * s, y = py + kMargin * s;
  std::string title = Printable(request.title);
  if (title.empty()) title = "Enter text";
  c.Text(x, y, Wrap(title, chars, 1)[0], s, 255, 255, 255);
  y += kLine * s;
  auto desc = Wrap(Printable(request.description), chars, kDescLines);
  for (int i = 0; i < kDescLines; ++i) {
    if (i < int(desc.size())) c.Text(x, y, desc[i], s, 190, 190, 200);
    y += kLine * s;
  }
  y += 6 * s;

  // Text field, scrolled to keep the end in view, with a blinking cursor.
  c.Fill(x, y, kGridW * s, kFieldH * s, 8, 8, 10);
  std::string shown = text;
  const size_t fit = chars - 2;
  if (shown.size() > fit) shown = shown.substr(shown.size() - fit);
  int tx = c.Text(x + 6 * s, y + 6 * s, shown, s, 255, 255, 255);
  if ((frame / 30) % 2 == 0) {
    c.Fill(tx, y + 6 * s + (kFontHeight - 2) * s, kFontWidth * s, 2 * s, 16,
           200, 16);
  }
  y += (kFieldH + 10) * s;

  for (int r = 0; r <= kActionRow; ++r) {
    int keys = r == kActionRow ? kActions : kColumns;
    int key_w = r == kActionRow ? (kGridW - (kActions - 1) * kGap) / kActions
                                : kKeyW;
    for (int k = 0; k < keys; ++k) {
      int kx = x + k * (key_w + kGap) * s;
      bool selected = r == row && k == col;
      bool lit = r == kActionRow && k == 0 && shift;
      if (selected) {
        c.Fill(kx, y, key_w * s, kKeyH * s, 16, 124, 16);
      } else {
        c.Fill(kx, y, key_w * s, kKeyH * s, lit ? 40 : 58, lit ? 90 : 58,
               lit ? 40 : 66);
      }
      std::string label = r == kActionRow
                              ? std::string(kActionLabels[k])
                              : std::string(1, kRows[shift ? 1 : 0][r][k]);
      int lw = int(label.size()) * kFontWidth;
      c.Text(kx + (key_w - lw) * s / 2, y + (kKeyH - kFontHeight) * s / 2,
             label, s, 255, 255, 255);
    }
    y += (kKeyH + kGap) * s;
  }
  c.Text(x, y + 2 * s, "A type  B del  X space  Y shift  START ok", s, 150,
         150, 160);
}

void Cancel() {
  std::lock_guard<std::mutex> lock(mutex);
  Finish(false);
  wait_for_release = false;
}

}  // namespace libretro_keyboard
}  // namespace xe
