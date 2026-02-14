/*
 * Xenia Edge - Xbox 360 Emulator Libretro Core
 * Libretro Audio Driver Implementation
 * Copyright (C) 2024 Xenia Edge Contributors
 */

#include "libretro_audio_driver.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "xenia/apu/conversion.h"

namespace xe {
namespace apu {
namespace libretro {

// ---- Ring buffer ----------------------------------------------------------

void LibretroAudioRingBuffer::Push(const float* data, size_t count) {
  size_t wp = write_pos_.load(std::memory_order_relaxed);
  size_t rp = read_pos_.load(std::memory_order_acquire);

  // Drop oldest samples if buffer would overflow.
  size_t used = wp - rp;
  if (used + count > kCapacity) {
    read_pos_.store(wp + count - kCapacity, std::memory_order_release);
  }

  for (size_t i = 0; i < count; ++i) {
    buffer_[(wp + i) % kCapacity] = data[i];
  }
  write_pos_.store(wp + count, std::memory_order_release);
}

size_t LibretroAudioRingBuffer::Pop(int16_t* out, size_t max_samples) {
  size_t rp = read_pos_.load(std::memory_order_relaxed);
  size_t wp = write_pos_.load(std::memory_order_acquire);
  size_t avail = std::min(wp - rp, kCapacity);
  size_t to_read = std::min(avail, max_samples);

  for (size_t i = 0; i < to_read; ++i) {
    float s = buffer_[(rp + i) % kCapacity];
    s = std::max(-1.0f, std::min(1.0f, s));
    out[i] = static_cast<int16_t>(s * 32767.0f);
  }
  read_pos_.store(rp + to_read, std::memory_order_release);
  return to_read;
}

size_t LibretroAudioRingBuffer::Available() const {
  size_t wp = write_pos_.load(std::memory_order_acquire);
  size_t rp = read_pos_.load(std::memory_order_relaxed);
  return wp - rp;
}

void LibretroAudioRingBuffer::Clear() {
  read_pos_.store(0, std::memory_order_relaxed);
  write_pos_.store(0, std::memory_order_relaxed);
}

// ---- LibretroAudioDriver --------------------------------------------------

LibretroAudioDriver::LibretroAudioDriver(
    xe::threading::Semaphore* semaphore, LibretroAudioRingBuffer* ring)
    : semaphore_(semaphore), ring_(ring) {}

LibretroAudioDriver::~LibretroAudioDriver() = default;

bool LibretroAudioDriver::Initialize() { return true; }

void LibretroAudioDriver::Shutdown() {}

void LibretroAudioDriver::SubmitFrame(float* samples) {
  if (paused_ || !ring_) return;

  // Xenia submits 6-channel SEQUENTIAL BIG-ENDIAN ??? convert to stereo.
  constexpr size_t kSamplesPerChannel = kChannelSamplesDefault;  // 256
  float stereo[kSamplesPerChannel * 2];

  conversion::sequential_6_BE_to_interleaved_2_LE(
      stereo, samples, kSamplesPerChannel);

  // Apply volume
  if (volume_ != 1.0f) {
    for (size_t i = 0; i < kSamplesPerChannel * 2; ++i) {
      stereo[i] *= volume_;
    }
  }

  // Block if ring buffer >75% full (backpressure for real-time).
  constexpr size_t kThreshold = LibretroAudioRingBuffer::kCapacity * 3 / 4;
  int wait_loops = 0;
  while (ring_->Available() > kThreshold) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (++wait_loops > 100) break;  // safety: don't block forever
  }

  ring_->Push(stereo, kSamplesPerChannel * 2);

  // Release the semaphore so Xenia's audio worker can continue
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
}

void LibretroAudioDriver::SignalConsumerReady() {
  if (semaphore_) {
    semaphore_->Release(1, nullptr);
  }
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
  auto driver = new LibretroAudioDriver(semaphore, &ring_buffer_);
  if (!driver->Initialize()) {
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

AudioDriver* LibretroAudioSystem::CreateDriver(
    xe::threading::Semaphore* semaphore, uint32_t frequency,
    uint32_t channels, bool need_format_conversion) {
  return new LibretroAudioDriver(semaphore, &ring_buffer_);
}

void LibretroAudioSystem::DestroyDriver(AudioDriver* driver) {
  if (driver) {
    driver->Shutdown();
    delete driver;
  }
}

}  // namespace libretro
}  // namespace apu
}  // namespace xe
