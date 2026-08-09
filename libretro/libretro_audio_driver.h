/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver
 * Copyright (C) 2024 Xenia Edge Contributors
 *
 * Implements xe::apu::AudioDriver and xe::apu::AudioSystem to capture
 * audio samples from Xenia and forward them to libretro's audio callback.
 *
 * A title may register more than one render driver client (XAudio2 gives each
 * its own source voice and mixes them into the master voice). Here every
 * client gets its own ring and LibretroAudioMixer SUMS them on drain. An
 * earlier version handed one shared ring to every driver, which concatenated
 * the clients into a single FIFO instead: two clients then produced two
 * seconds of audio per second of wall time, alternating 5.33 ms blocks of
 * each. See libretro/tests/ for the regression tests.
 */

#ifndef LIBRETRO_AUDIO_DRIVER_H
#define LIBRETRO_AUDIO_DRIVER_H

#include <atomic>
#include <cstdint>
#include <cstring>

#include "xenia/apu/audio_driver.h"
#include "xenia/apu/audio_system.h"

namespace xe {
namespace apu {
namespace libretro {

/* ==== BEGIN MIXER CORE ====================================================
 * Everything between these markers is extracted verbatim by
 * libretro/tests/run_tests.sh and compiled standalone, so the tests exercise
 * the shipped code rather than a copy of it. Keep this region free of Xenia
 * headers: it may use only the standard library and
 * xe::threading::Semaphore::Release(), which the tests stub out.
 * ========================================================================= */

// Milliseconds since an arbitrary fixed origin. Used only for stream liveness.
uint64_t LibretroAudioNowMs();

// A single-producer / single-consumer ring of float samples.
//
// The producer (a guest audio client, via SubmitFrame) only ever advances
// write_pos_; the consumer (update_audio, via LibretroAudioMixer::Pop) only
// ever advances read_pos_. Neither cursor is written by both sides, which is
// what makes this a genuine SPSC ring and why no CAS is needed.
//
// When the ring is full the producer drops the NEWEST samples. Dropping the
// oldest instead - by advancing read_pos_ from the producer - means writing
// into slots the consumer may be mixing at that instant, which is a real data
// race on the payload, not merely on the cursors. A full ring means the
// frontend has stopped consuming, and dropping is the right answer there.
class LibretroAudioRingBuffer {
 public:
  static constexpr size_t kCapacity = 48000 * 2;  // ~500 ms @ 48 kHz stereo

  // Producer side. Returns how many samples were accepted; a short return
  // means the ring was full and the remainder was dropped.
  size_t Push(const float* data, size_t count);

  // Consumer side. ADDS up to max_samples into out rather than overwriting it,
  // so several streams can be summed into one accumulator. Returns how many
  // samples were mixed.
  size_t MixInto(float* out, size_t max_samples);

  size_t Available() const;
  void Clear();

 private:
  float buffer_[kCapacity] = {};
  std::atomic<size_t> write_pos_{0};
  std::atomic<size_t> read_pos_{0};
};

// One guest client's ring plus the bookkeeping the mixer needs for it.
class LibretroAudioStream {
 public:
  // Producer side.
  void MarkSubmitted() {
    last_push_ms_.store(LibretroAudioNowMs(), std::memory_order_release);
  }
  size_t Push(const float* data, size_t count) {
    return ring_.Push(data, count);
  }

  size_t Available() const { return ring_.Available(); }
  bool in_use() const { return in_use_.load(std::memory_order_acquire); }

 private:
  friend class LibretroAudioMixer;

  LibretroAudioRingBuffer ring_;
  std::atomic<bool> in_use_{false};
  std::atomic<uint64_t> last_push_ms_{0};

  // Consumer-side only; no synchronisation needed.
  size_t consumed_accum_ = 0;
  uint32_t frame_samples_ = 512;

  xe::threading::Semaphore* semaphore_ = nullptr;
};

// Owns one ring per client and sums them into a single stereo stream.
class LibretroAudioMixer {
 public:
  // Matches xe::apu::AudioSystem::kMaximumClientCount.
  static constexpr size_t kMaxStreams = 8;

  // A stream that has not submitted for this long is treated as silent, so a
  // client that registers and never submits - or simply stops - cannot hold
  // the whole mix at zero forever.
  static constexpr uint64_t kIdleTimeoutMs = 100;

  // Largest block Pop() will assemble in one call.
  static constexpr size_t kMixChunk = 4096;

  // frame_samples is the stereo sample count of one submitted frame, used to
  // credit the client semaphore once per frame actually consumed.
  LibretroAudioStream* AcquireStream(xe::threading::Semaphore* semaphore,
                                     uint32_t frame_samples);
  void ReleaseStream(LibretroAudioStream* stream);

  // Sums every live stream, clamps AFTER summing, and converts to int16.
  // Returns the number of int16 samples written to out.
  size_t Pop(int16_t* out, size_t max_samples);

  void Clear();
  size_t ActiveStreamCount() const;

 private:
  LibretroAudioStream streams_[kMaxStreams];
  float mix_[kMixChunk] = {};
};

/* ==== END MIXER CORE ==================================================== */

// AudioDriver: captures SubmitFrame() into this client's ring (no real device).
class LibretroAudioDriver : public AudioDriver {
 public:
  LibretroAudioDriver(LibretroAudioStream* stream, uint32_t frequency,
                      uint32_t channels, bool need_format_conversion);
  ~LibretroAudioDriver() override;

  bool Initialize() override;
  void Shutdown() override;

  void SubmitFrame(float* samples) override;
  void Pause() override;
  void Resume() override;
  void SetVolume(float volume) override;

  uint32_t channel_samples() const { return channel_samples_; }
  LibretroAudioStream* stream() const { return stream_; }

 private:
  LibretroAudioStream* stream_ = nullptr;
  uint32_t frame_frequency_ = 48000;
  uint32_t frame_channels_ = 6;
  bool need_format_conversion_ = true;
  uint32_t channel_samples_ = kChannelSamplesDefault;
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

  LibretroAudioMixer* mixer() { return &mixer_; }

 protected:
  X_STATUS CreateDriver(size_t index, xe::threading::Semaphore* semaphore,
                         AudioDriver** out_driver) override;

 private:
  LibretroAudioMixer mixer_;
};

}  // namespace libretro
}  // namespace apu
}  // namespace xe

#endif  // LIBRETRO_AUDIO_DRIVER_H
