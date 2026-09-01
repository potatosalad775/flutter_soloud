// Standalone correctness tests for the single-band peaking equalizer.
//
// Build & run from the flutter_soloud repo root:
//
//   c++ -std=c++17 -O2 -Wall -DNOMINMAX \
//       -I src/soloud/include \
//       -o /tmp/peaking_eq_test \
//       test/peaking_eq_test.cpp src/filters/peaking_eq_filter.cpp \
//       src/soloud/src/core/soloud_filter.cpp \
//       src/soloud/src/core/soloud_fader.cpp \
//   && /tmp/peaking_eq_test
//
// Or use the convenience wrapper:
//
//   ./test/run_peaking_eq_test.sh
//
// Unlike limiter_test.cpp, this links the real FilterInstance and Fader rather
// than stubbing them: the whole point of this filter is how it behaves while a
// parameter fade is running, so the genuine fader has to drive it.
//
// What is covered:
//   1. At full wet the magnitude response is the RBJ cookbook peaking filter,
//      i.e. the same curve ma_peak2 produced before the migration.
//   2. At wet = 0 the filter is bit-exact bypass.
//   3. Fading the band in adds no audible broadband energy, with a hard bypass
//      switch as the control to show the metric can detect a click at all.
//   4. eqTrainer's round transition -- retune, then immediately fade in --
//      lands on the new band rather than the old one.
//   5. The coefficient smoothing stays stable under aggressive retuning.

#include "../src/filters/peaking_eq_filter.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <vector>

namespace {

const double kPi = 3.14159265358979323846;
const double kFs = 48000.0;
const unsigned int kBlock = 512;

struct Rbj {
  double b0, b1, b2, a0, a1, a2;
};

Rbj rbjPeaking(double freq, double gainDb, double q, double fs) {
  Rbj c;
  const double a = pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kPi * freq / fs;
  const double alpha = sin(w0) / (2.0 * q);
  c.b0 = 1.0 + alpha * a;
  c.b1 = -2.0 * cos(w0);
  c.b2 = 1.0 - alpha * a;
  c.a0 = 1.0 + alpha / a;
  c.a1 = -2.0 * cos(w0);
  c.a2 = 1.0 - alpha / a;
  return c;
}

double rbjMagnitudeDb(const Rbj &c, double freq, double fs) {
  const std::complex<double> z1 =
      std::polar(1.0, -2.0 * kPi * freq / fs);
  const std::complex<double> z2 = z1 * z1;
  const std::complex<double> num = c.b0 + c.b1 * z1 + c.b2 * z2;
  const std::complex<double> den = c.a0 + c.a1 * z1 + c.a2 * z2;
  return 20.0 * log10(std::abs(num / den));
}

// Magnitude of the filter's own impulse response at `freq`, via a direct DTFT.
double measuredMagnitudeDb(const std::vector<float> &impulse, double freq,
                           double fs) {
  std::complex<double> acc(0.0, 0.0);
  for (size_t n = 0; n < impulse.size(); n++) {
    acc += (double)impulse[n] * std::polar(1.0, -2.0 * kPi * freq * n / fs);
  }
  return 20.0 * log10(std::abs(acc));
}

// Runs the filter over `samples`, calling it in blocks so parameter fades
// advance the way they do on the audio thread.
void run(PeakingEqInstance *instance, float *buffer, size_t samples,
         double startTime) {
  size_t offset = 0;
  double t = startTime;
  while (offset < samples) {
    const unsigned int n =
        (unsigned int)std::min((size_t)kBlock, samples - offset);
    instance->filterChannel(buffer + offset, n, (float)kFs, t, 0, 1);
    offset += n;
    t += (double)n / kFs;
  }
}

// What a click actually is: broadband energy. The test signal is a pure tone,
// so probing frequencies far away from it measures exactly the energy the
// transition added and nothing else. A Hann window keeps the tone's own
// spectral leakage into those probes far below anything audible.
//
// Time-domain metrics don't work here. A first difference just measures how
// loud and how high-pitched the tone is, and a second difference degrades the
// same way once the tone gets high enough that consecutive samples differ a
// lot on their own.
// The window is centred on `center` so the event being measured sits where the
// Hann window has full gain. Putting it at the edge instead would attenuate it
// by 40 dB and make even a hard switch look clean.
double broadbandLeakDb(const std::vector<float> &v, size_t center, size_t len) {
  const size_t from = center > len / 2 ? center - len / 2 : 0;
  if (from + len > v.size())
    len = v.size() - from;

  std::vector<double> windowed(len);
  for (size_t n = 0; n < len; n++) {
    const double w = 0.5 * (1.0 - cos(2.0 * kPi * n / (double)(len - 1)));
    windowed[n] = (double)v[from + n] * w;
  }

  double worst = 0.0;
  for (double frac = 0.30; frac <= 0.90; frac += 0.05) {
    const double probe = frac * kFs * 0.5;
    std::complex<double> acc(0.0, 0.0);
    for (size_t n = 0; n < len; n++) {
      acc += windowed[n] * std::polar(1.0, -2.0 * kPi * probe * n / kFs);
    }
    worst = std::max(worst, std::abs(acc) / (double)len);
  }
  return 20.0 * log10(worst + 1e-30);
}

int gFailures = 0;

void check(bool ok, const char *what) {
  printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok)
    gFailures++;
}

// ---------------------------------------------------------------------------

void testResponseMatchesRbj(double gainDb, double freq, double q) {
  PeakingEq parent;
  parent.mFrequency = (float)freq;
  parent.mGain = (float)gainDb;
  parent.mQ = (float)q;
  PeakingEqInstance *instance = (PeakingEqInstance *)parent.createInstance();

  std::vector<float> impulse(1 << 16, 0.0f);
  impulse[0] = 1.0f;
  run(instance, impulse.data(), impulse.size(), 0.0);

  const Rbj ref = rbjPeaking(freq, gainDb, q, kFs);

  double worst = 0.0;
  double worstAt = 0.0;
  for (double f = 20.0; f < kFs * 0.45; f *= 1.05) {
    const double diff =
        fabs(measuredMagnitudeDb(impulse, f, kFs) - rbjMagnitudeDb(ref, f, kFs));
    if (diff > worst) {
      worst = diff;
      worstAt = f;
    }
  }

  // Also confirm the peak really lands at the requested gain and frequency.
  const double peak = measuredMagnitudeDb(impulse, freq, kFs);

  printf("gain %+6.1f dB, f0 %7.1f Hz, Q %.2f -> max deviation from RBJ "
         "%.2e dB (at %.0f Hz), peak %+.4f dB\n",
         gainDb, freq, q, worst, worstAt, peak);
  check(worst < 1e-3, "magnitude response matches RBJ cookbook");
  check(fabs(peak - gainDb) < 1e-3, "peak gain lands on the requested dB");

  delete instance;
}

void testTransitionIsClickFree(double gainDb, double freq, double q,
                               int fadeMs) {
  PeakingEq parent;
  parent.mFrequency = (float)freq;
  parent.mGain = (float)gainDb;
  parent.mQ = (float)q;
  parent.mWet = 0.0f;

  PeakingEqInstance *instance = (PeakingEqInstance *)parent.createInstance();
  instance->setFilterParameter(PeakingEq::WET, 0.0f);

  const size_t total = (size_t)(kFs * 1.0);
  const size_t switchAt = (size_t)(kFs * 0.5);

  // A steady tone at the band centre: the worst case, since that is where the
  // wet and dry signals differ most. The phase is chosen so the switch lands on
  // the crest of the wave rather than on a zero crossing, which is where a hard
  // switch does the most damage -- and where, at these round sample counts, it
  // would otherwise land and look deceptively clean.
  const double phase = kPi / 2.0 - 2.0 * kPi * freq * (double)switchAt / kFs;
  std::vector<float> signal(total);
  for (size_t n = 0; n < total; n++) {
    signal[n] = (float)(0.5 * sin(2.0 * kPi * freq * n / kFs + phase));
  }
  std::vector<float> faded = signal;

  // First half dry.
  run(instance, faded.data(), switchAt, 0.0);
  // Then fade the band in over `fadeMs`.
  const double t = (double)switchAt / kFs;
  instance->fadeFilterParameter(PeakingEq::WET, 1.0f, fadeMs / 1000.0, t);
  run(instance, faded.data() + switchAt, total - switchAt, t);

  // Control: the same switch done the way a bypass flag does it, jumping
  // straight from the dry signal to the fully filtered one.
  std::vector<float> hard = signal;
  PeakingEq parent2;
  parent2.mFrequency = (float)freq;
  parent2.mGain = (float)gainDb;
  parent2.mQ = (float)q;
  PeakingEqInstance *wetOnly = (PeakingEqInstance *)parent2.createInstance();
  run(wetOnly, hard.data(), total, 0.0);
  for (size_t n = 0; n < switchAt; n++) {
    hard[n] = signal[n];
  }

  // Express the added broadband energy relative to the tone itself, which is
  // what decides whether anyone can hear it.
  const size_t fadeSamples = (size_t)(kFs * fadeMs / 1000.0);
  const size_t window = std::max((size_t)2048, fadeSamples * 2);
  double level = 0.0;
  for (size_t n = (size_t)(kFs * 0.8); n < total; n++) {
    level = std::max(level, (double)fabs(faded[n]));
  }
  const double ref = 20.0 * log10(level);
  // Each window is centred on its own event: the middle of the ramp for the
  // fade, the instant of the switch for the control.
  const double duringFade =
      broadbandLeakDb(faded, switchAt + fadeSamples / 2, window) - ref;
  const double duringHard = broadbandLeakDb(hard, switchAt, window) - ref;

  printf("gain %+6.1f dB, f0 %7.1f Hz, %d ms fade -> broadband during fade "
         "%.1f dB below signal, hard switch %.1f dB below signal "
         "(%.0f dB worse)\n",
         gainDb, freq, fadeMs, -duringFade, -duringHard,
         duringHard - duringFade);

  check(duringFade < -80.0, "fade adds nothing audible");
  check(duringHard > duringFade + 20.0, "control: hard switch does");

  delete instance;
  delete wetOnly;
}

void testBypassIsExact() {
  PeakingEq parent;
  parent.mFrequency = 1000.0f;
  parent.mGain = 12.0f;
  parent.mQ = 1.0f;
  parent.mWet = 0.0f;
  PeakingEqInstance *instance = (PeakingEqInstance *)parent.createInstance();
  instance->setFilterParameter(PeakingEq::WET, 0.0f);

  std::vector<float> signal(4096);
  for (size_t n = 0; n < signal.size(); n++) {
    signal[n] = (float)(0.5 * sin(2.0 * kPi * 997.0 * n / kFs));
  }
  std::vector<float> out = signal;
  run(instance, out.data(), out.size(), 0.0);

  double worst = 0.0;
  for (size_t n = 0; n < signal.size(); n++) {
    worst = std::max(worst, (double)fabs(out[n] - signal[n]));
  }
  printf("wet = 0 -> max deviation from input %.3e\n", worst);
  check(worst == 0.0, "wet = 0 is bit-exact bypass");

  delete instance;
}

// eqTrainer's round transition: retune the band and switch it on in the same
// breath. The new band must be the one that comes up, not the old one.
void testRetuneWhileSilentThenFadeIn() {
  PeakingEq parent;
  parent.mFrequency = 200.0f;
  parent.mGain = 15.0f;
  parent.mQ = 1.0f;
  parent.mWet = 0.0f;
  PeakingEqInstance *instance = (PeakingEqInstance *)parent.createInstance();
  instance->setFilterParameter(PeakingEq::WET, 0.0f);

  const size_t total = (size_t)(kFs * 0.6);
  const size_t switchAt = (size_t)(kFs * 0.2);

  std::vector<float> signal(total);
  for (size_t n = 0; n < total; n++) {
    // Two tones, one at the old centre and one at the new, so picking the
    // wrong band is unmissable.
    signal[n] = (float)(0.25 * sin(2.0 * kPi * 200.0 * n / kFs) +
                        0.25 * sin(2.0 * kPi * 5000.0 * n / kFs));
  }
  std::vector<float> out = signal;

  run(instance, out.data(), switchAt, 0.0);

  const double t = (double)switchAt / kFs;
  instance->setFilterParameter(PeakingEq::FREQUENCY, 5000.0f);
  instance->setFilterParameter(PeakingEq::GAIN, 12.0f);
  instance->fadeFilterParameter(PeakingEq::WET, 1.0f, 0.020, t);
  run(instance, out.data() + switchAt, total - switchAt, t);

  // Reference: the same band, configured from the start and always fully wet.
  PeakingEq refParent;
  refParent.mFrequency = 5000.0f;
  refParent.mGain = 12.0f;
  refParent.mQ = 1.0f;
  PeakingEqInstance *ref = (PeakingEqInstance *)refParent.createInstance();
  std::vector<float> refOut = signal;
  run(ref, refOut.data(), total, 0.0);

  double worst = 0.0;
  for (size_t n = (size_t)(kFs * 0.5); n < total; n++) {
    worst = std::max(worst, (double)fabs(out[n] - refOut[n]));
  }

  const size_t fadeSamples = (size_t)(kFs * 0.02);
  const size_t window = std::max((size_t)2048, fadeSamples * 2);
  double level = 0.0;
  for (size_t n = (size_t)(kFs * 0.5); n < total; n++) {
    level = std::max(level, (double)fabs(out[n]));
  }
  const double duringFade =
      broadbandLeakDb(out, switchAt + fadeSamples / 2, window) -
      20.0 * log10(level);

  printf("retune 200 Hz/+15 dB -> 5000 Hz/+12 dB then fade in -> settles to "
         "%.3e from reference, broadband during fade %.1f dB below signal\n",
         worst, -duringFade);
  check(worst < 1e-5, "settles onto the newly requested band");
  check(duringFade < -80.0, "retune + fade adds nothing audible");

  delete instance;
  delete ref;
}

void testStabilityUnderSweep() {
  // Sweeping the centre frequency while the band is fully wet exercises the
  // coefficient smoothing, which must stay stable and bounded.
  PeakingEq parent;
  parent.mFrequency = 60.0f;
  parent.mGain = 15.0f;
  parent.mQ = 8.0f;
  PeakingEqInstance *instance = (PeakingEqInstance *)parent.createInstance();

  std::vector<float> signal((size_t)(kFs * 2.0));
  for (size_t n = 0; n < signal.size(); n++) {
    signal[n] = (float)(0.3 * sin(2.0 * kPi * 220.0 * n / kFs) +
                        0.3 * sin(2.0 * kPi * 3000.0 * n / kFs));
  }

  double peak = 0.0;
  bool finite = true;
  size_t offset = 0;
  double t = 0.0;
  int step = 0;
  while (offset < signal.size()) {
    const unsigned int n =
        (unsigned int)std::min((size_t)kBlock, signal.size() - offset);
    // Jump the band around aggressively, every block.
    const float f = (step % 2) ? 12000.0f : 40.0f;
    instance->setFilterParameter(PeakingEq::FREQUENCY, f);
    instance->filterChannel(signal.data() + offset, n, (float)kFs, t, 0, 1);
    for (unsigned int i = 0; i < n; i++) {
      const float v = signal[offset + i];
      if (!std::isfinite(v))
        finite = false;
      peak = std::max(peak, (double)fabs(v));
    }
    offset += n;
    t += (double)n / kFs;
    step++;
  }

  printf("aggressive frequency jumps -> peak %.3f, all finite: %s\n", peak,
         finite ? "yes" : "no");
  check(finite, "stays finite under aggressive parameter jumps");
  check(peak < 20.0, "stays bounded under aggressive parameter jumps");

  delete instance;
}

} // namespace

int main() {
  printf("== response ==\n");
  testResponseMatchesRbj(12.0, 1000.0, 1.0);
  testResponseMatchesRbj(-12.0, 1000.0, 1.0);
  testResponseMatchesRbj(15.0, 60.0, 4.0);
  testResponseMatchesRbj(-6.0, 8000.0, 0.7);
  testResponseMatchesRbj(3.0, 30.0, 2.0);
  testResponseMatchesRbj(-15.0, 14000.0, 10.0);

  printf("\n== bypass ==\n");
  testBypassIsExact();

  printf("\n== transition ==\n");
  testTransitionIsClickFree(15.0, 1000.0, 1.0, 20);
  testTransitionIsClickFree(-15.0, 1000.0, 1.0, 20);
  testTransitionIsClickFree(15.0, 100.0, 1.0, 20);
  testTransitionIsClickFree(12.0, 4000.0, 2.0, 5);

  printf("\n== round transition ==\n");
  testRetuneWhileSilentThenFadeIn();

  printf("\n== stability ==\n");
  testStabilityUnderSweep();

  printf("\n%s (%d failure(s))\n", gFailures ? "FAILED" : "ALL PASSED",
         gFailures);
  return gFailures ? 1 : 0;
}
