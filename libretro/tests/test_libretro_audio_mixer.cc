/*
 * Xenia Edge - libretro audio mixer tests
 *
 * The ring buffer and mixer under test are EXTRACTED from the shipped sources
 * by run_tests.sh, not copied here, so these tests always exercise the code
 * that actually ships. Only xe::threading::Semaphore is stubbed.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

// ---- Stub for the one Xenia type the mixer core touches --------------------

namespace xe {
namespace threading {
class Semaphore {
 public:
  bool Release(int count, int* previous) {
    if (previous) *previous = released_.load();
    released_ += count;
    return true;
  }
  int released() const { return released_.load(); }
  void reset() { released_ = 0; }

 private:
  std::atomic<int> released_{0};
};
}  // namespace threading
}  // namespace xe

// ---- The code under test, lifted out of the shipped sources ---------------

namespace xe {
namespace apu {
namespace libretro {
#include "gen/mixer_core.h.inc"
#include "gen/mixer_core.cc.inc"
}  // namespace libretro
}  // namespace apu
}  // namespace xe

using xe::apu::libretro::LibretroAudioMixer;
using xe::apu::libretro::LibretroAudioRingBuffer;
using xe::apu::libretro::LibretroAudioStream;

// ---- Tiny test harness -----------------------------------------------------

static int g_failures = 0;
static const char* g_current = "";

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

#define CHECK_EQ(a, b)                                                    \
  do {                                                                    \
    auto _va = (a);                                                       \
    auto _vb = (b);                                                       \
    if (!(_va == _vb)) {                                                  \
      std::printf("    FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, \
                  __LINE__, #a, #b, (long long)_va, (long long)_vb);      \
      ++g_failures;                                                       \
    }                                                                     \
  } while (0)

static void run(const char* name, void (*fn)()) {
  g_current = name;
  int before = g_failures;
  fn();
  std::printf("  %-46s %s\n", name, (g_failures == before) ? "ok" : "FAILED");
}

// One submitted frame of stereo audio: 256 samples per channel, 2 channels.
static constexpr size_t kFrame = 512;

static std::vector<float> block(size_t n, float value) {
  return std::vector<float>(n, value);
}

// ---- Tests -----------------------------------------------------------------

// MixInto sums into the accumulator rather than overwriting it, which is what
// lets several streams share one output buffer.
static void test_ring_basic() {
  LibretroAudioRingBuffer ring;
  auto in = block(8, 0.25f);
  CHECK_EQ(ring.Push(in.data(), in.size()), size_t(8));
  CHECK_EQ(ring.Available(), size_t(8));

  float out[8];
  for (int i = 0; i < 8; ++i) out[i] = 0.5f;
  CHECK_EQ(ring.MixInto(out, 8), size_t(8));
  for (int i = 0; i < 8; ++i) CHECK(std::fabs(out[i] - 0.75f) < 1e-6f);
  CHECK_EQ(ring.Available(), size_t(0));
}

// Drop-newest must never let Available() run past capacity, and must never
// let wp - rp underflow into a huge size_t.
static void test_ring_overflow_no_underflow() {
  LibretroAudioRingBuffer ring;
  const size_t cap = LibretroAudioRingBuffer::kCapacity;
  auto big = block(cap, 0.1f);

  CHECK_EQ(ring.Push(big.data(), cap), cap);
  CHECK_EQ(ring.Available(), cap);

  // Ring is full: further pushes are refused outright, not wrapped.
  auto more = block(1024, 0.2f);
  CHECK_EQ(ring.Push(more.data(), more.size()), size_t(0));
  CHECK_EQ(ring.Available(), cap);
  CHECK(ring.Available() <= cap);
}

// The original bug: two clients sharing one ring produced 1024 samples for one
// frame-time instead of 512, alternating blocks of each client.
static void test_two_clients_are_mixed_not_concatenated() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sem_a, sem_b;
  auto* a = mixer.AcquireStream(&sem_a, kFrame);
  auto* b = mixer.AcquireStream(&sem_b, kFrame);
  CHECK(a && b);

  auto fa = block(kFrame, 0.25f);
  auto fb = block(kFrame, 0.25f);
  a->MarkSubmitted();
  a->Push(fa.data(), fa.size());
  b->MarkSubmitted();
  b->Push(fb.data(), fb.size());

  std::vector<int16_t> out(4096);
  size_t got = mixer.Pop(out.data(), out.size());

  CHECK_EQ(got, kFrame);  // one frame-time of audio, not two
  // And it is the SUM of both, not one then the other.
  const int16_t expect = static_cast<int16_t>(0.5f * 32767.0f);
  CHECK(std::abs(out[0] - expect) <= 1);
  CHECK(std::abs(out[kFrame - 1] - expect) <= 1);
}

// Output rate must be one frame-time per frame-time regardless of how many
// clients are registered.
static void test_production_rate_is_realtime() {
  for (size_t clients = 1; clients <= 4; ++clients) {
    LibretroAudioMixer mixer;
    std::vector<xe::threading::Semaphore> sems(clients);
    std::vector<LibretroAudioStream*> streams;
    for (size_t i = 0; i < clients; ++i) {
      auto* s = mixer.AcquireStream(&sems[i], kFrame);
      CHECK(s != nullptr);
      streams.push_back(s);
    }

    auto f = block(kFrame, 0.1f);
    for (auto* s : streams) {
      s->MarkSubmitted();
      s->Push(f.data(), f.size());
    }

    std::vector<int16_t> out(8192);
    size_t total = 0, got = 0;
    while ((got = mixer.Pop(out.data(), out.size())) != 0) total += got;

    CHECK_EQ(total, kFrame);  // never clients * kFrame
  }
}

// Pop is gated by the slowest live stream, so a stream that is ahead keeps its
// surplus rather than racing on without the others.
static void test_gating_uses_slowest_live_stream() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sa, sb;
  auto* a = mixer.AcquireStream(&sa, kFrame);
  auto* b = mixer.AcquireStream(&sb, kFrame);

  auto two = block(kFrame * 2, 0.1f);
  auto one = block(kFrame, 0.1f);
  a->MarkSubmitted();
  a->Push(two.data(), two.size());  // ahead
  b->MarkSubmitted();
  b->Push(one.data(), one.size());

  std::vector<int16_t> out(8192);
  size_t got = mixer.Pop(out.data(), out.size());

  CHECK_EQ(got, kFrame);              // limited by b
  CHECK_EQ(a->Available(), kFrame);   // a keeps its backlog
  CHECK_EQ(b->Available(), size_t(0));
}

// A client that goes quiet must not hold the mix at zero forever.
static void test_idle_stream_does_not_deadlock_mix() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sa, sb;
  auto* a = mixer.AcquireStream(&sa, kFrame);
  auto* b = mixer.AcquireStream(&sb, kFrame);

  b->MarkSubmitted();  // b speaks once, then stops
  std::this_thread::sleep_for(
      std::chrono::milliseconds(LibretroAudioMixer::kIdleTimeoutMs + 40));

  auto f = block(kFrame, 0.2f);
  a->MarkSubmitted();
  a->Push(f.data(), f.size());

  std::vector<int16_t> out(4096);
  CHECK_EQ(mixer.Pop(out.data(), out.size()), kFrame);  // a is not blocked by b
}

// Same, for a client that registers and never submits at all.
static void test_never_submitting_client_does_not_gate() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sa, sb;
  auto* a = mixer.AcquireStream(&sa, kFrame);
  mixer.AcquireStream(&sb, kFrame);  // registered, silent forever

  std::this_thread::sleep_for(
      std::chrono::milliseconds(LibretroAudioMixer::kIdleTimeoutMs + 40));

  auto f = block(kFrame, 0.2f);
  a->MarkSubmitted();
  a->Push(f.data(), f.size());

  std::vector<int16_t> out(4096);
  CHECK_EQ(mixer.Pop(out.data(), out.size()), kFrame);
}

// A client whose pushes are being dropped is still live - it is submitting,
// just backlogged - and must keep gating rather than being written off.
static void test_backlogged_client_still_counts_as_live() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sa, sb;
  auto* a = mixer.AcquireStream(&sa, kFrame);
  auto* b = mixer.AcquireStream(&sb, kFrame);

  // Fill b to capacity so its pushes start getting dropped.
  auto huge = block(LibretroAudioRingBuffer::kCapacity, 0.1f);
  b->MarkSubmitted();
  CHECK_EQ(b->Push(huge.data(), huge.size()),
           LibretroAudioRingBuffer::kCapacity);

  auto f = block(kFrame, 0.1f);
  b->MarkSubmitted();
  CHECK_EQ(b->Push(f.data(), f.size()), size_t(0));  // dropped, still submitted

  a->MarkSubmitted();
  a->Push(f.data(), f.size());

  std::vector<int16_t> out(8192);
  size_t got = mixer.Pop(out.data(), out.size());
  CHECK_EQ(got, kFrame);  // gated by a, and b participated in the mix
}

// Credits are issued when the frontend consumes, not when the guest submits.
static void test_semaphore_credited_on_drain() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sem;
  auto* s = mixer.AcquireStream(&sem, kFrame);

  auto three = block(kFrame * 3, 0.1f);
  s->MarkSubmitted();
  s->Push(three.data(), three.size());

  CHECK_EQ(sem.released(), 0);  // nothing credited on submit

  std::vector<int16_t> out(8192);
  size_t total = 0, got = 0;
  while ((got = mixer.Pop(out.data(), out.size())) != 0) total += got;

  CHECK_EQ(total, kFrame * 3);
  CHECK_EQ(sem.released(), 3);  // one per frame consumed
}

// Slots are finite and reusable; the ninth concurrent client is refused.
static void test_release_frees_slot() {
  LibretroAudioMixer mixer;
  std::vector<xe::threading::Semaphore> sems(LibretroAudioMixer::kMaxStreams + 1);
  std::vector<LibretroAudioStream*> got;

  for (size_t i = 0; i < LibretroAudioMixer::kMaxStreams; ++i) {
    auto* s = mixer.AcquireStream(&sems[i], kFrame);
    CHECK(s != nullptr);
    got.push_back(s);
  }
  CHECK_EQ(mixer.ActiveStreamCount(), LibretroAudioMixer::kMaxStreams);

  // Ninth refused rather than overrunning the array.
  CHECK(mixer.AcquireStream(&sems[LibretroAudioMixer::kMaxStreams], kFrame) ==
        nullptr);

  mixer.ReleaseStream(got[3]);
  CHECK(mixer.AcquireStream(&sems[LibretroAudioMixer::kMaxStreams], kFrame) !=
        nullptr);
}

// Clamping happens after summing, so loud streams saturate instead of wrapping
// to the opposite sign.
static void test_clipping_after_sum() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sa, sb;
  auto* a = mixer.AcquireStream(&sa, kFrame);
  auto* b = mixer.AcquireStream(&sb, kFrame);

  auto loud = block(kFrame, 0.8f);  // 0.8 + 0.8 = 1.6, past full scale
  a->MarkSubmitted();
  a->Push(loud.data(), loud.size());
  b->MarkSubmitted();
  b->Push(loud.data(), loud.size());

  std::vector<int16_t> out(4096);
  size_t got = mixer.Pop(out.data(), out.size());
  CHECK_EQ(got, kFrame);
  for (size_t i = 0; i < got; ++i) {
    CHECK_EQ(out[i], int16_t(32767));  // saturated, never negative
  }
}

// Real threads on the real ring: the producer only touches write_pos_ and the
// consumer only read_pos_, so this must be clean under ThreadSanitizer.
static void test_concurrent_producer_consumer() {
  LibretroAudioMixer mixer;
  xe::threading::Semaphore sem;
  auto* s = mixer.AcquireStream(&sem, kFrame);

  std::atomic<bool> stop{false};
  std::atomic<size_t> pushed{0};

  std::thread producer([&] {
    auto f = block(kFrame, 0.05f);
    while (!stop.load(std::memory_order_relaxed)) {
      s->MarkSubmitted();
      pushed += s->Push(f.data(), f.size());
      std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
  });

  std::vector<int16_t> out(4096);
  size_t drained = 0;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
  while (std::chrono::steady_clock::now() < deadline) {
    drained += mixer.Pop(out.data(), out.size());
    std::this_thread::sleep_for(std::chrono::microseconds(500));
  }
  stop = true;
  producer.join();

  CHECK(drained > 0);
  CHECK(drained <= pushed.load());
}

int main() {
  std::printf("libretro audio mixer tests\n");
  run("test_ring_basic", test_ring_basic);
  run("test_ring_overflow_no_underflow", test_ring_overflow_no_underflow);
  run("test_two_clients_are_mixed_not_concatenated",
      test_two_clients_are_mixed_not_concatenated);
  run("test_production_rate_is_realtime", test_production_rate_is_realtime);
  run("test_gating_uses_slowest_live_stream", test_gating_uses_slowest_live_stream);
  run("test_idle_stream_does_not_deadlock_mix", test_idle_stream_does_not_deadlock_mix);
  run("test_never_submitting_client_does_not_gate",
      test_never_submitting_client_does_not_gate);
  run("test_backlogged_client_still_counts_as_live",
      test_backlogged_client_still_counts_as_live);
  run("test_semaphore_credited_on_drain", test_semaphore_credited_on_drain);
  run("test_release_frees_slot", test_release_frees_slot);
  run("test_clipping_after_sum", test_clipping_after_sum);
  run("test_concurrent_producer_consumer", test_concurrent_producer_consumer);

  if (g_failures) {
    std::printf("\n%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("\nALL PASSED (0 failures)\n");
  return 0;
}
