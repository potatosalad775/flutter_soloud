#include "sinc_resampler.h"

#include <algorithm>
#include <cmath>

namespace soloud_offline {

namespace {

/// Modified Bessel function of the first kind, order 0, by its power series.
/// Converges quickly for the arguments a Kaiser window needs.
double besselI0(double x) {
  double sum = 1.0;
  double term = 1.0;
  const double halfXSquared = (x * x) / 4.0;
  for (int k = 1; k < 64; ++k) {
    term *= halfXSquared / (static_cast<double>(k) * static_cast<double>(k));
    sum += term;
    if (term < sum * 1e-17) {
      break;
    }
  }
  return sum;
}

/// sin(pi * t) / (pi * t), with the removable singularity at 0 filled in.
double normalizedSinc(double t) {
  if (std::abs(t) < 1e-12) {
    return 1.0;
  }
  const double pit = M_PI * t;
  return std::sin(pit) / pit;
}

int gcdInt(int a, int b) {
  while (b != 0) {
    const int t = a % b;
    a = b;
    b = t;
  }
  return a;
}

}  // namespace

bool SincResampler::init(int inRate, int outRate, int channels) {
  m_valid = false;
  m_passthrough = false;
  m_taps.clear();
  m_hist.clear();
  m_framesIn = 0;
  m_outIndex = 0;

  if (inRate <= 0 || outRate <= 0 || channels < 1 || channels > 2) {
    return false;
  }

  m_channels = channels;
  m_valid = true;

  if (inRate == outRate) {
    m_passthrough = true;
    m_l = 1;
    m_m = 1;
    return true;
  }

  const int g = gcdInt(inRate, outRate);
  m_l = outRate / g;
  m_m = inRate / g;

  buildTaps();

  // Frames before the stream starts read as silence. Seeding the history with
  // a full kernel of zeros lets the very first output frame be computed with
  // the same expression as every other one.
  m_histStart = -(2 * static_cast<int64_t>(m_halfWidth));
  m_hist.assign(static_cast<size_t>(2 * m_halfWidth) * m_channels, 0.0f);
  m_framesIn = 0;
  return true;
}

void SincResampler::buildTaps() {
  const int tapsPerPhase = 2 * m_halfWidth;
  const size_t total = static_cast<size_t>(tapsPerPhase) * m_l;
  m_taps.assign(total, 0.0f);

  // Anti-alias at the lower of the two Nyquist limits, expressed in cycles per
  // input sample. Upsampling keeps the input's own band; downsampling has to
  // cut to the output's, or the discarded samples fold back.
  const double cutoff =
      0.5 * std::min(1.0, static_cast<double>(m_l) / static_cast<double>(m_m)) *
      kCutoffFraction;

  const double center = static_cast<double>(m_halfWidth);
  const double i0Beta = besselI0(kKaiserBeta);

  std::vector<double> raw(total, 0.0);
  for (int phase = 0; phase < m_l; ++phase) {
    for (int k = 0; k < tapsPerPhase; ++k) {
      // Position of this tap relative to the kernel centre, in input samples.
      // Tap (phase + k*L) of the prototype sits at (k + phase/L) input
      // samples from the kernel's start, and the centre is halfWidth in.
      const double offset = static_cast<double>(k) +
                            static_cast<double>(phase) /
                                static_cast<double>(m_l) -
                            center;

      // Kaiser window over the kernel's support, zero outside it.
      const double ratio = offset / center;
      double window = 0.0;
      if (ratio > -1.0 && ratio < 1.0) {
        window = besselI0(kKaiserBeta * std::sqrt(1.0 - ratio * ratio)) / i0Beta;
      }

      raw[static_cast<size_t>(phase) +
          static_cast<size_t>(k) * static_cast<size_t>(m_l)] =
          2.0 * cutoff * normalizedSinc(2.0 * cutoff * offset) * window;
    }
  }

  // Normalize each phase to unit DC gain. Doing it numerically rather than
  // from a closed form keeps gain exact whatever the window and cutoff are,
  // so a resampled file never comes back quieter or louder than its source.
  for (int phase = 0; phase < m_l; ++phase) {
    double sum = 0.0;
    for (int k = 0; k < tapsPerPhase; ++k) {
      sum += raw[static_cast<size_t>(phase) +
                 static_cast<size_t>(k) * static_cast<size_t>(m_l)];
    }
    const double scale = (std::abs(sum) > 1e-12) ? 1.0 / sum : 1.0;
    for (int k = 0; k < tapsPerPhase; ++k) {
      const size_t idx = static_cast<size_t>(phase) +
                         static_cast<size_t>(k) * static_cast<size_t>(m_l);
      m_taps[idx] = static_cast<float>(raw[idx] * scale);
    }
  }
}

void SincResampler::appendFrames(const float *input, size_t samples) {
  m_hist.insert(m_hist.end(), input, input + samples);
  m_framesIn += static_cast<int64_t>(samples) / m_channels;
}

void SincResampler::emitAvailable(std::vector<float> &output) {
  const int tapsPerPhase = 2 * m_halfWidth;
  const int64_t lastFrame = m_framesIn - 1;

  while (true) {
    const int64_t t = m_outIndex * static_cast<int64_t>(m_m);
    const int64_t idx = t / static_cast<int64_t>(m_l);
    const int phase = static_cast<int>(t % static_cast<int64_t>(m_l));

    // The kernel is centred half a width ahead of `idx`, so the newest frame
    // it touches is idx + halfWidth. Stop as soon as that is not in yet.
    const int64_t newest = idx + m_halfWidth;
    if (newest > lastFrame) {
      break;
    }

    for (int ch = 0; ch < m_channels; ++ch) {
      float acc = 0.0f;
      for (int k = 0; k < tapsPerPhase; ++k) {
        const int64_t frame = newest - k;
        const size_t pos =
            static_cast<size_t>((frame - m_histStart) * m_channels + ch);
        acc += m_taps[static_cast<size_t>(phase) +
                      static_cast<size_t>(k) * static_cast<size_t>(m_l)] *
               m_hist[pos];
      }
      output.push_back(acc);
    }

    ++m_outIndex;
  }

  // Drop history no future output can reach: the oldest frame the next output
  // needs is (idx + halfWidth) - (2 * halfWidth - 1).
  const int64_t nextT = m_outIndex * static_cast<int64_t>(m_m);
  const int64_t nextIdx = nextT / static_cast<int64_t>(m_l);
  const int64_t oldestNeeded = nextIdx + m_halfWidth - (tapsPerPhase - 1);
  if (oldestNeeded > m_histStart) {
    const size_t drop =
        static_cast<size_t>(oldestNeeded - m_histStart) * m_channels;
    if (drop < m_hist.size()) {
      m_hist.erase(m_hist.begin(), m_hist.begin() + static_cast<long>(drop));
      m_histStart = oldestNeeded;
    }
  }
}

void SincResampler::process(const float *input, size_t samples,
                            std::vector<float> &output) {
  output.clear();
  if (!m_valid || input == nullptr || samples == 0) {
    return;
  }

  if (m_passthrough) {
    output.assign(input, input + samples);
    return;
  }

  appendFrames(input, samples);
  emitAvailable(output);
}

void SincResampler::flush(std::vector<float> &output) {
  output.clear();
  if (!m_valid || m_passthrough) {
    return;
  }

  // Feed a kernel's worth of silence so the samples still sitting in the
  // delay line are shifted out. Without this the last ~24 input samples of
  // every file would simply never be emitted.
  const std::vector<float> silence(
      static_cast<size_t>(m_halfWidth) * m_channels, 0.0f);
  appendFrames(silence.data(), silence.size());
  emitAvailable(output);
}

}  // namespace soloud_offline
