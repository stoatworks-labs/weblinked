#include <vector>

#include "core/audio_fifo.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// Writes `frames` sample-frames where channel c, frame f carries the value
/// start + f + c*1000, so a mis-ordered or mis-strided read is unmistakable.
void writeRamp(AudioFifo& fifo, int channels, int frames, int start) {
  std::vector<std::vector<float>> planes(static_cast<size_t>(channels));
  std::vector<const float*> pointers(static_cast<size_t>(channels));
  for (int c = 0; c < channels; ++c) {
    planes[static_cast<size_t>(c)].resize(static_cast<size_t>(frames));
    for (int f = 0; f < frames; ++f) {
      planes[static_cast<size_t>(c)][static_cast<size_t>(f)] =
          static_cast<float>(start + f + c * 1000);
    }
    pointers[static_cast<size_t>(c)] = planes[static_cast<size_t>(c)].data();
  }
  fifo.write(pointers.data(), frames);
}

}  // namespace

WEBLINKED_TEST(audio_fifo_round_trips_planar) {
  AudioFifo fifo;
  fifo.configure(2, 48000, 4800);
  CHECK_EQ(fifo.channels(), 2);
  CHECK_EQ(fifo.sampleRate(), 48000);

  writeRamp(fifo, 2, 480, 0);
  CHECK_EQ(fifo.availableFrames(), 480);

  std::vector<float> left(480), right(480);
  float* planes[2] = {left.data(), right.data()};
  const int read = fifo.readPlanar(planes, 480);

  CHECK_EQ(read, 480);
  CHECK_EQ(fifo.availableFrames(), 0);
  CHECK_NEAR(left[0], 0.0, 1e-6);
  CHECK_NEAR(left[479], 479.0, 1e-6);
  CHECK_NEAR(right[0], 1000.0, 1e-6);
  CHECK_NEAR(right[479], 1479.0, 1e-6);
}

WEBLINKED_TEST(audio_fifo_interleaves_in_channel_order) {
  AudioFifo fifo;
  fifo.configure(2, 48000, 1000);
  writeRamp(fifo, 2, 4, 0);

  std::vector<float> interleaved(8);
  CHECK_EQ(fifo.readInterleaved(interleaved.data(), 4), 4);

  // L0 R0 L1 R1 ...
  CHECK_NEAR(interleaved[0], 0.0, 1e-6);
  CHECK_NEAR(interleaved[1], 1000.0, 1e-6);
  CHECK_NEAR(interleaved[2], 1.0, 1e-6);
  CHECK_NEAR(interleaved[3], 1001.0, 1e-6);
  CHECK_NEAR(interleaved[6], 3.0, 1e-6);
  CHECK_NEAR(interleaved[7], 1003.0, 1e-6);
}

WEBLINKED_TEST(audio_fifo_wraps_around_the_ring) {
  AudioFifo fifo;
  fifo.configure(1, 48000, 100);

  // Fill, drain, then write across the wrap point.
  writeRamp(fifo, 1, 80, 0);
  std::vector<float> scratch(80);
  float* planes[1] = {scratch.data()};
  CHECK_EQ(fifo.readPlanar(planes, 80), 80);

  writeRamp(fifo, 1, 60, 500);  // write cursor is at 80, capacity 100
  CHECK_EQ(fifo.availableFrames(), 60);

  std::vector<float> out(60);
  float* outPlanes[1] = {out.data()};
  CHECK_EQ(fifo.readPlanar(outPlanes, 60), 60);
  CHECK_NEAR(out[0], 500.0, 1e-6);
  CHECK_NEAR(out[19], 519.0, 1e-6);   // last sample before the wrap
  CHECK_NEAR(out[20], 520.0, 1e-6);   // first sample after it
  CHECK_NEAR(out[59], 559.0, 1e-6);
}

WEBLINKED_TEST(audio_fifo_underrun_pads_with_silence_and_counts) {
  AudioFifo fifo;
  fifo.configure(1, 48000, 1000);
  writeRamp(fifo, 1, 10, 1);

  std::vector<float> out(20, -1.0f);
  float* planes[1] = {out.data()};
  const int read = fifo.readPlanar(planes, 20);

  CHECK_EQ(read, 10);                 // only 10 were real
  CHECK_NEAR(out[9], 10.0, 1e-6);
  CHECK_NEAR(out[10], 0.0, 1e-6);     // silence, not stale data
  CHECK_NEAR(out[19], 0.0, 1e-6);
  CHECK_EQ(fifo.stats().underruns, 1);
}

WEBLINKED_TEST(audio_fifo_overrun_keeps_the_newest_audio) {
  AudioFifo fifo;
  fifo.configure(1, 48000, 100);

  writeRamp(fifo, 1, 80, 0);
  writeRamp(fifo, 1, 80, 1000);  // 160 written into a 100-frame ring

  CHECK_EQ(fifo.availableFrames(), 100);
  CHECK(fifo.stats().overruns >= 1);

  std::vector<float> out(100);
  float* planes[1] = {out.data()};
  CHECK_EQ(fifo.readPlanar(planes, 100), 100);

  // The newest 100 sample-frames survive: the tail of the first write plus all
  // of the second. Keeping the newest is the whole point — an operator cares
  // about what is happening now, not what happened two seconds ago.
  CHECK_NEAR(out[99], 1079.0, 1e-6);
  CHECK_NEAR(out[20], 1000.0, 1e-6);
  CHECK_NEAR(out[19], 79.0, 1e-6);
}

WEBLINKED_TEST(audio_fifo_discards_only_the_tail_of_an_oversized_packet) {
  AudioFifo fifo;
  fifo.configure(1, 48000, 50);
  writeRamp(fifo, 1, 200, 0);  // four times the capacity in one packet

  CHECK_EQ(fifo.availableFrames(), 50);
  std::vector<float> out(50);
  float* planes[1] = {out.data()};
  CHECK_EQ(fifo.readPlanar(planes, 50), 50);
  CHECK_NEAR(out[0], 150.0, 1e-6);
  CHECK_NEAR(out[49], 199.0, 1e-6);
}

WEBLINKED_TEST(audio_fifo_reconfigure_discards_and_resets_stats) {
  AudioFifo fifo;
  fifo.configure(2, 48000, 100);
  writeRamp(fifo, 2, 50, 0);
  CHECK_EQ(fifo.availableFrames(), 50);

  // The browser's stream can restart with different parameters mid-session.
  fifo.configure(1, 44100, 200);
  CHECK_EQ(fifo.channels(), 1);
  CHECK_EQ(fifo.sampleRate(), 44100);
  CHECK_EQ(fifo.availableFrames(), 0);
  CHECK_EQ(fifo.stats().framesWritten, 0);
}

WEBLINKED_TEST(audio_fifo_is_safe_before_configuration) {
  // The engine may tick before the page has produced any audio at all.
  AudioFifo fifo;
  std::vector<float> out(64, 7.0f);
  float* planes[1] = {out.data()};
  CHECK_EQ(fifo.readPlanar(planes, 64), 0);
  CHECK_EQ(fifo.readInterleaved(out.data(), 64), 0);
  CHECK_EQ(fifo.availableFrames(), 0);
}
