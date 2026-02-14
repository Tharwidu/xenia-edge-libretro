/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * Implements xe::apu::AudioDriver and xe::apu::AudioSystem to capture
 * audio samples from Xenia and forward them to libretro's audio callback.
 */

#ifndef LIBRETRO_AUDIO_DRIVER_H
#define LIBRETRO_AUDIO_DRIVER_H

#include <atomic>
#include <cstring>
#include <mutex>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"

namespace xe {
namespace apu {
namespace libretro {

// Ring buffer: Xenia float samples ??? libretro int16_t stereo pairs.
class LibretroAudioRingBuffer {
 public:
  static constexpr size_t kCapacity = 48000 * 2;  // ~500ms @ 48kHz stereo

  void Push(const float* data, size_t count);
  size_t Pop(int16_t* out, size_t max_samples);
  size_t Available() const;
  void Clear();

 private:
  float buffer_[kCapacity] = {};
  std::atomic<size_t> write_pos_{0};
  std::atomic<size_t> read_pos_{0};
};

// AudioDriver: captures SubmitFrame() to ring buffer (no real device).
class LibretroAudioDriver : public AudioDriver {
 public:
  explicit LibretroAudioDriver(xe::threading::Semaphore* semaphore,
                                LibretroAudioRingBuffer* ring);
  ~LibretroAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;

  void SubmitFrame(float* samples) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override;

  // Called from retro_run to release the audio worker semaphore,
  // providing backpressure to match real-time playback rate.
  void SignalConsumerReady();

 private:
  xe::threading::Semaphore* semaphore_ = nullptr;
  LibretroAudioRingBuffer* ring_ = nullptr;
  float volume_ = 1.0f;
  bool paused_ = false;
};

// AudioSystem that creates LibretroAudioDriver instances.
class LibretroAudioSystem : public AudioSystem {
 public:
  explicit LibretroAudioSystem(cpu::Processor* processor);
  ~LibretroAudioSystem() override;

  static bool IsAvailable() { return true; }

  std::string name() const override { return "libretro"; }

  AudioDriver* CreateDriver(xe::threading::Semaphore* semaphore,
                             uint32_t frequency, uint32_t channels,
                             bool need_format_conversion) override;
  void DestroyDriver(AudioDriver* driver) override;

  LibretroAudioRingBuffer* ring_buffer() { return &ring_buffer_; }

 protected:
  X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                         AudioDriver** out_driver) override;

 private:
  LibretroAudioRingBuffer ring_buffer_;
};

}  // namespace libretro
}  // namespace apu
}  // namespace xe

#endif  // LIBRETRO_AUDIO_DRIVER_H
