/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_audio_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "xenia/apu/conversion.h"

namespace xe {
namespace apu {
namespace libretro {

/* ==== BEGIN MIXER CORE ====================================================
 * Extracted verbatim by libretro/tests/run_tests.sh - see the matching note
 * in libretro_audio_driver.h. Standard library only, plus
 * xe::threading::Semaphore::Release().
 * ========================================================================= */

uint64_t LibretroAudioNowMs() {
  using namespace std::chrono;
  return static_cast<uint64_t>(
      duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
          .count());
}

// ---- Ring buffer ----------------------------------------------------------

size_t LibretroAudioRingBuffer::Push(const float* data, size_t count) {
  const size_t wp = write_pos_.load(std::memory_order_relaxed);
  const size_t rp = read_pos_.load(std::memory_order_acquire);

  // Drop-newest: never advance read_pos_ from the producer. See the class
  // comment for why overwriting the oldest samples is not safe here.
  const size_t used = wp - rp;
  const size_t space = (used >= kCapacity) ? 0 : (kCapacity - used);
  const size_t to_write = std::min(count, space);

  for (size_t i = 0; i < to_write; ++i) {
    buffer_[(wp + i) % kCapacity] = data[i];
  }
  write_pos_.store(wp + to_write, std::memory_order_release);
  return to_write;
}

size_t LibretroAudioRingBuffer::MixInto(float* out, size_t max_samples) {
  const size_t rp = read_pos_.load(std::memory_order_relaxed);
  const size_t wp = write_pos_.load(std::memory_order_acquire);
  const size_t avail = std::min(wp - rp, kCapacity);
  const size_t to_read = std::min(avail, max_samples);

  // Sum rather than assign, so callers can accumulate several streams.
  for (size_t i = 0; i < to_read; ++i) {
    out[i] += buffer_[(rp + i) % kCapacity];
  }
  read_pos_.store(rp + to_read, std::memory_order_release);
  return to_read;
}

size_t LibretroAudioRingBuffer::Available() const {
  const size_t wp = write_pos_.load(std::memory_order_acquire);
  const size_t rp = read_pos_.load(std::memory_order_relaxed);
  const size_t used = wp - rp;
  return std::min(used, kCapacity);
}

void LibretroAudioRingBuffer::Clear() {
  read_pos_.store(write_pos_.load(std::memory_order_acquire),
                  std::memory_order_release);
}

// ---- Mixer ----------------------------------------------------------------

LibretroAudioStream* LibretroAudioMixer::AcquireStream(
    xe::threading::Semaphore* semaphore, uint32_t frame_samples) {
  for (size_t i = 0; i < kMaxStreams; ++i) {
    bool expected = false;
    if (streams_[i].in_use_.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      streams_[i].ring_.Clear();
      streams_[i].semaphore_ = semaphore;
      streams_[i].frame_samples_ = frame_samples ? frame_samples : 512;
      streams_[i].consumed_accum_ = 0;
      streams_[i].last_push_ms_.store(LibretroAudioNowMs(),
                                      std::memory_order_release);
      return &streams_[i];
    }
  }
  return nullptr;  // all slots taken
}

void LibretroAudioMixer::ReleaseStream(LibretroAudioStream* stream) {
  if (!stream) return;
  stream->semaphore_ = nullptr;
  stream->ring_.Clear();
  stream->in_use_.store(false, std::memory_order_release);
}

size_t LibretroAudioMixer::Pop(int16_t* out, size_t max_samples) {
  if (!out || !max_samples) return 0;

  const uint64_t now = LibretroAudioNowMs();
  size_t limit = std::min(max_samples, kMixChunk);
  bool any_live = false;

  // Gate on the slowest LIVE stream so the streams stay time-aligned. A
  // stream that has gone quiet past the idle timeout is treated as silent and
  // does not gate - otherwise one dormant client stalls everyone.
  for (size_t i = 0; i < kMaxStreams; ++i) {
    LibretroAudioStream& s = streams_[i];
    if (!s.in_use_.load(std::memory_order_acquire)) continue;

    const size_t avail = s.ring_.Available();
    const uint64_t last = s.last_push_ms_.load(std::memory_order_acquire);
    const bool idle = (now - last) > kIdleTimeoutMs;

    if (idle && avail == 0) continue;  // silent: contributes nothing, gates nothing

    any_live = true;
    limit = std::min(limit, avail);
  }

  if (!any_live || limit == 0) return 0;

  for (size_t i = 0; i < limit; ++i) mix_[i] = 0.0f;

  for (size_t i = 0; i < kMaxStreams; ++i) {
    LibretroAudioStream& s = streams_[i];
    if (!s.in_use_.load(std::memory_order_acquire)) continue;

    const size_t mixed = s.ring_.MixInto(mix_, limit);
    if (!mixed) continue;

    // Credit the client semaphore once per frame actually consumed. XAudio2
    // does this in OnBufferEnd - when the host has played the buffer - not on
    // submit. Releasing on submit frees a slot the instant the guest hands a
    // frame over, so the audio worker never sees a full queue and there is no
    // back-pressure at all.
    s.consumed_accum_ += mixed;
    while (s.frame_samples_ && s.consumed_accum_ >= s.frame_samples_) {
      s.consumed_accum_ -= s.frame_samples_;
      if (s.semaphore_) {
        s.semaphore_->Release(1, nullptr);
      }
    }
  }

  // Clamp AFTER summing, so the sum of several streams cannot wrap.
  for (size_t i = 0; i < limit; ++i) {
    const float s = std::max(-1.0f, std::min(1.0f, mix_[i]));
    out[i] = static_cast<int16_t>(s * 32767.0f);
  }
  return limit;
}

void LibretroAudioMixer::Clear() {
  for (size_t i = 0; i < kMaxStreams; ++i) {
    streams_[i].ring_.Clear();
    streams_[i].consumed_accum_ = 0;
  }
}

size_t LibretroAudioMixer::ActiveStreamCount() const {
  size_t n = 0;
  for (size_t i = 0; i < kMaxStreams; ++i) {
    if (streams_[i].in_use_.load(std::memory_order_acquire)) ++n;
  }
  return n;
}

/* ==== END MIXER CORE ==================================================== */

// ---- LibretroAudioDriver --------------------------------------------------

// Largest stereo block one submitted frame can produce: 2 channels carry 768
// samples each, 6 channels carry 256.
static constexpr size_t kMaxStereoSamples = 768 * 2;

LibretroAudioDriver::LibretroAudioDriver(LibretroAudioStream* stream,
                                         uint32_t frequency, uint32_t channels,
                                         bool need_format_conversion)
    : stream_(stream),
      frame_frequency_(frequency),
      frame_channels_(channels),
      need_format_conversion_(need_format_conversion) {
  // Same mapping as the XAudio2 driver: the frame size depends on the channel
  // count, and 2-channel frames are three times longer per channel.
  switch (frame_channels_) {
    case 2:
      channel_samples_ = 768;
      break;
    case 6:
    default:
      channel_samples_ = kChannelSamplesDefault;  // 256
      break;
  }
}

LibretroAudioDriver::~LibretroAudioDriver() = default;

bool LibretroAudioDriver::Initialize() { return true; }

void LibretroAudioDriver::Shutdown() {}

void LibretroAudioDriver::SubmitFrame(float* samples) {
  if (!stream_ || !samples) return;

  // Liveness is marked even when paused or when the push is dropped: a client
  // that is submitting but backlogged is still live, and must keep gating the
  // mix rather than being written off as silent.
  stream_->MarkSubmitted();
  if (paused_) return;

  float stereo[kMaxStereoSamples];
  const size_t out_samples =
      static_cast<size_t>(channel_samples_) * 2;
  if (out_samples > kMaxStereoSamples) return;

  if (need_format_conversion_) {
    // Xenia's own render clients: 6-channel sequential big-endian.
    conversion::sequential_6_BE_to_interleaved_2_LE(stereo, samples,
                                                    channel_samples_);
  } else if (frame_channels_ == 2) {
    // Already interleaved stereo little-endian - AudioMediaPlayer (XMP)
    // takes this path. Decoding it as planar big-endian 5.1 produces noise.
    std::memcpy(stereo, samples, sizeof(float) * out_samples);
  } else {
    // Interleaved little-endian with more than two channels: take the front
    // pair rather than reinterpreting the layout.
    for (size_t i = 0; i < channel_samples_; ++i) {
      stereo[i * 2 + 0] = samples[i * frame_channels_ + 0];
      stereo[i * 2 + 1] = samples[i * frame_channels_ + 1];
    }
  }

  if (volume_ != 1.0f) {
    for (size_t i = 0; i < out_samples; ++i) {
      stereo[i] *= volume_;
    }
  }

  // Never block here. SubmitFrame is called from AudioSystem::SubmitFrame with
  // global_critical_region_ held, which the kernel, GPU and guest threads also
  // take - sleeping inside it stalls the whole emulator. Push() already
  // handles a full ring by dropping.
  stream_->Push(stereo, out_samples);
}

void LibretroAudioDriver::Pause() { paused_ = true; }
void LibretroAudioDriver::Resume() { paused_ = false; }
void LibretroAudioDriver::SetVolume(float volume) { volume_ = volume; }

// ---- LibretroAudioSystem --------------------------------------------------

LibretroAudioSystem::LibretroAudioSystem(cpu::Processor* processor)
    : AudioSystem(processor) {}

LibretroAudioSystem::~LibretroAudioSystem() = default;

X_STATUS LibretroAudioSystem::CreateDriver(size_t index,
                                            xe::threading::Semaphore* semaphore,
                                            AudioDriver** out_driver) {
  // Render clients: 6-channel sequential big-endian, converted to stereo.
  auto stream =
      mixer_.AcquireStream(semaphore, AudioDriver::kChannelSamplesDefault * 2);
  if (!stream) {
    return X_STATUS_UNSUCCESSFUL;
  }

  auto driver = new LibretroAudioDriver(stream, 48000, 6, true);
  if (!driver->Initialize()) {
    mixer_.ReleaseStream(stream);
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

AudioDriver* LibretroAudioSystem::CreateDriver(
    xe::threading::Semaphore* semaphore, uint32_t frequency,
    uint32_t channels, bool need_format_conversion) {
  // AudioMediaPlayer takes this overload and passes a real format. The
  // previous implementation discarded all three arguments and always decoded
  // as planar big-endian 5.1, which is noise for 2-channel interleaved input.
  const uint32_t channel_samples =
      (channels == 2) ? 768 : AudioDriver::kChannelSamplesDefault;

  auto stream = mixer_.AcquireStream(semaphore, channel_samples * 2);
  if (!stream) {
    return nullptr;
  }

  auto driver = new LibretroAudioDriver(stream, frequency, channels,
                                        need_format_conversion);
  if (!driver->Initialize()) {
    mixer_.ReleaseStream(stream);
    delete driver;
    return nullptr;
  }
  return driver;
}

void LibretroAudioSystem::DestroyDriver(AudioDriver* driver) {
  if (!driver) return;
  // Hand the mixer slot back, or a title that cycles drivers exhausts the
  // eight slots and later clients get no audio at all.
  if (auto* lr = static_cast<LibretroAudioDriver*>(driver)) {
    mixer_.ReleaseStream(lr->stream());
  }
  driver->Shutdown();
  delete driver;
}

}  // namespace libretro
}  // namespace apu
}  // namespace xe
