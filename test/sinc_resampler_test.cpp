// Standalone tests for the windowed-sinc polyphase resampler.
//
// Build & run with test/run_sinc_resampler_test.sh
//
// The point of this filter is that it does not damage the top octave the way
// linear interpolation did, so the assertions are about actual frequency
// response, not just "it produced some samples".

#include "../src/mixeroutput/sinc_resampler.h"

#include <cmath>
#include <cstdio>
#include <vector>

using soloud_offline::SincResampler;

namespace {

int gFailures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    printf("  FAIL: %s\n", what);
    gFailures++;
  }
}

/// Amplitude of the component at [freq], by direct correlation.
double amplitudeAt(const std::vector<float> &x, double freq, double rate) {
  double re = 0.0;
  double im = 0.0;
  for (size_t n = 0; n < x.size(); ++n) {
    const double t = static_cast<double>(n) / rate;
    re += x[n] * std::cos(2.0 * M_PI * freq * t);
    im += x[n] * std::sin(2.0 * M_PI * freq * t);
  }
  return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(x.size());
}

std::vector<float> sineWave(double freq, int rate, double amplitude = 0.5) {
  std::vector<float> v(static_cast<size_t>(rate));
  for (int i = 0; i < rate; ++i) {
    v[static_cast<size_t>(i)] = static_cast<float>(
        amplitude * std::sin(2.0 * M_PI * freq * i / rate));
  }
  return v;
}

/// Run a whole buffer through in one call, tail included.
std::vector<float> resampleAll(const std::vector<float> &in, int inRate,
                               int outRate, int channels = 1) {
  SincResampler r;
  if (!r.init(inRate, outRate, channels)) {
    return {};
  }
  std::vector<float> out;
  std::vector<float> tail;
  r.process(in.data(), in.size(), out);
  r.flush(tail);
  out.insert(out.end(), tail.begin(), tail.end());
  return out;
}

double dbRelative(double measured, double reference) {
  return 20.0 * std::log10(measured / reference);
}

void testPassthrough() {
  printf("passthrough when the rates match\n");
  SincResampler r;
  check(r.init(48000, 48000, 2), "init at equal rates");
  check(r.isPassthrough(), "reports passthrough");

  const std::vector<float> in = {0.1f, -0.2f, 0.3f, -0.4f};
  std::vector<float> out;
  r.process(in.data(), in.size(), out);
  check(out == in, "output is the input, untouched");
}

void testDcGain() {
  printf("unity DC gain\n");
  const std::vector<float> in(44100, 1.0f);
  const std::vector<float> out = resampleAll(in, 44100, 48000);
  check(!out.empty(), "produced output");

  // Sample in the steady state, away from the start-up and tail ramps.
  const double mid = out[out.size() / 2];
  check(std::abs(mid - 1.0) < 1e-4, "steady-state DC is 1.0");
}

void testPassbandFlatness() {
  printf("passband flatness through the audible range\n");
  struct {
    double freq;
    double maxDroopDb;
  } cases[] = {
      {1000.0, 0.05},
      {10000.0, 0.05},
      {16000.0, 0.05},
      {19000.0, 0.05},
      // 20 kHz sits in the transition band the 2% cutoff pull-back creates.
      {20000.0, 0.40},
  };

  for (const auto &c : cases) {
    const std::vector<float> out =
        resampleAll(sineWave(c.freq, 44100), 44100, 48000);
    const double amp = amplitudeAt(out, c.freq, 48000);
    const double droop = -dbRelative(amp, 0.5);
    printf("  %6.0f Hz: %+.4f dB\n", c.freq, -droop);
    check(droop < c.maxDroopDb, "droop within budget");
  }
}

void testImageRejection() {
  printf("image rejection above the passband\n");
  // Upsampling 44.1 -> 48 images a component at f to (44100 - f).
  const double f = 21000.0;
  const std::vector<float> out = resampleAll(sineWave(f, 44100), 44100, 48000);
  const double signal = amplitudeAt(out, f, 48000);
  const double image = amplitudeAt(out, 44100.0 - f, 48000);
  const double db = dbRelative(image + 1e-15, signal);
  printf("  image at %.0f Hz is %.1f dB down\n", 44100.0 - f, db);
  // Linear interpolation managed only -1.7 dB here.
  check(db < -20.0, "image at least 20 dB down");
}

void testChunkingIsSeamless() {
  printf("chunk boundaries are seamless\n");
  std::vector<float> in(44100);
  for (size_t i = 0; i < in.size(); ++i) {
    in[i] = 0.3f * static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
  }

  const std::vector<float> once = resampleAll(in, 44100, 48000);

  SincResampler r;
  r.init(44100, 48000, 1);
  std::vector<float> chunked;
  std::vector<float> tmp;
  for (size_t off = 0; off < in.size(); off += 1000) {
    const size_t n = std::min<size_t>(1000, in.size() - off);
    r.process(in.data() + off, n, tmp);
    chunked.insert(chunked.end(), tmp.begin(), tmp.end());
  }
  r.flush(tmp);
  chunked.insert(chunked.end(), tmp.begin(), tmp.end());

  check(once.size() == chunked.size(), "same length either way");
  if (once.size() == chunked.size()) {
    double maxDiff = 0.0;
    for (size_t i = 0; i < once.size(); ++i) {
      maxDiff = std::max(maxDiff, std::abs(static_cast<double>(once[i]) -
                                           static_cast<double>(chunked[i])));
    }
    printf("  max difference: %.3e\n", maxDiff);
    check(maxDiff == 0.0, "chunked output is bit-identical to single-shot");
  }
}

void testOutputLength() {
  printf("output length follows the ratio\n");
  const std::vector<float> in(44100, 0.0f);
  const std::vector<float> out = resampleAll(in, 44100, 48000);
  printf("  %zu frames in -> %zu out (ideal 48000)\n", in.size(), out.size());
  // The delay line is flushed, so nothing should be lost off the tail.
  check(out.size() >= 47990 && out.size() <= 48010, "length within a frame");
}

void testStereoChannelsStayIndependent() {
  printf("stereo channels do not bleed into each other\n");
  const size_t frames = 44100;
  std::vector<float> in(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    in[i * 2 + 0] =
        0.5f * static_cast<float>(std::sin(2.0 * M_PI * 1000.0 * i / 44100.0));
    in[i * 2 + 1] = 0.0f;  // right channel is silent
  }

  const std::vector<float> out = resampleAll(in, 44100, 48000, 2);
  check(!out.empty(), "produced output");

  double leftPeak = 0.0;
  double rightPeak = 0.0;
  for (size_t i = 0; i + 1 < out.size(); i += 2) {
    leftPeak = std::max(leftPeak, std::abs(static_cast<double>(out[i])));
    rightPeak = std::max(rightPeak, std::abs(static_cast<double>(out[i + 1])));
  }
  printf("  left peak %.4f, right peak %.3e\n", leftPeak, rightPeak);
  check(leftPeak > 0.49, "left channel survives");
  check(rightPeak < 1e-6, "right channel stays silent");
}

void testRejectsBadArguments() {
  printf("invalid configurations are refused\n");
  SincResampler r;
  check(!r.init(0, 48000, 1), "zero input rate");
  check(!r.init(44100, 0, 1), "zero output rate");
  check(!r.init(44100, 48000, 0), "zero channels");
  check(!r.init(44100, 48000, 3), "more channels than supported");
}

}  // namespace

int main() {
  testPassthrough();
  testDcGain();
  testPassbandFlatness();
  testImageRejection();
  testChunkingIsSeamless();
  testOutputLength();
  testStereoChannelsStayIndependent();
  testRejectsBadArguments();

  if (gFailures != 0) {
    printf("\n%d FAILURE(S)\n", gFailures);
    return 1;
  }
  printf("\nAll sinc resampler tests passed.\n");
  return 0;
}
