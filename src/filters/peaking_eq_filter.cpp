#include "peaking_eq_filter.h"

#include <cmath>

namespace {

/// Time constant of the per-sample parameter smoothing. Short enough that a
/// toggle still feels instantaneous, long enough that the step it replaces is
/// inaudible.
const double kSmoothingSeconds = 0.008;

/// Below this the wet path contributes nothing audible, so the coefficients can
/// be retargeted instantly instead of sliding. `mix` is `wet * (gain - 1)`, so
/// even at the maximum gain this corresponds to a wet of a few parts per
/// million.
const double kInaudibleMix = 1e-4;

const double kPi = 3.14159265358979323846;

double clampDouble(double aValue, double aMin, double aMax) {
  if (aValue < aMin)
    return aMin;
  if (aValue > aMax)
    return aMax;
  return aValue;
}

} // namespace

PeakingEqInstance::PeakingEqInstance(PeakingEq *aParent) {
  mParent = aParent;

  initParams(4);
  mParam[PeakingEq::WET] = aParent->mWet;
  mParam[PeakingEq::FREQUENCY] = aParent->mFrequency;
  mParam[PeakingEq::GAIN] = aParent->mGain;
  mParam[PeakingEq::Q] = aParent->mQ;

  for (int i = 0; i < MAX_CHANNELS; i++) {
    mS1[i] = 0.0;
    mS2[i] = 0.0;
  }

  mTargetB0 = mTargetA1 = mTargetA2 = mTargetMix = 0.0;
  mB0 = mA1 = mA2 = mMix = 0.0;
  mSmoothing = 1.0;
  mCachedFrequency = mCachedGain = mCachedQ = 0.0;
  mLinearGain = 1.0;
  mSamplerate = 0.0;
  mPrimed = false;
}

PeakingEqInstance::~PeakingEqInstance() {}

void PeakingEqInstance::updateTargetCoefficients(double aSamplerate) {
  // Start from the RBJ audio-EQ-cookbook peaking filter:
  //
  //   A     = 10^(gainDb/40)          (so the peak gain is A^2)
  //   w0    = 2*pi*f0/fs
  //   alpha = sin(w0) / (2*Q)
  //
  //   b = [1 + alpha*A, -2*cos(w0), 1 - alpha*A]
  //   a = [1 + alpha/A, -2*cos(w0), 1 - alpha/A]
  //
  // Subtracting 1 from H(z) = b(z)/a(z), the -2*cos(w0) terms cancel and what
  // is left factors cleanly:
  //
  //   H(z) - 1 = alpha*(A - 1/A) * (1 - z^-2) / a(z)
  //            = (A^2 - 1) * [ (alpha/A) * (1 - z^-2) / a(z) ]
  //
  // The bracketed term is a bandpass whose coefficients depend only on the
  // frequency, Q and gain *magnitude* -- never on how much of the effect is
  // mixed in. So H(z) = 1 + (peakGain - 1) * B(z) exactly, and scaling that
  // second term by `wet` gives a continuous morph between bypass and the full
  // cookbook response while the recursive part of the filter runs undisturbed.
  const double fs = aSamplerate > 0.0 ? aSamplerate : 44100.0;

  // Keep the centre safely below Nyquist: sin(w0) collapses as w0 approaches
  // pi, which would make alpha (and with it the whole bell) degenerate.
  const double frequency =
      clampDouble(mParam[PeakingEq::FREQUENCY], 1.0, fs * 0.49);
  const double q = clampDouble(mParam[PeakingEq::Q], 0.01, 100.0);
  const double gainDb = clampDouble(mParam[PeakingEq::GAIN], -60.0, 60.0);

  const double a = pow(10.0, gainDb / 40.0);
  const double w0 = 2.0 * kPi * frequency / fs;
  const double alpha = sin(w0) / (2.0 * q);
  const double cosW0 = cos(w0);

  const double a0 = 1.0 + alpha / a;
  mTargetA1 = (-2.0 * cosW0) / a0;
  mTargetA2 = (1.0 - alpha / a) / a0;
  mTargetB0 = (alpha / a) / a0;

  mLinearGain = a * a;

  mCachedFrequency = mParam[PeakingEq::FREQUENCY];
  mCachedGain = mParam[PeakingEq::GAIN];
  mCachedQ = mParam[PeakingEq::Q];
}

void PeakingEqInstance::filterChannel(float *aBuffer, unsigned int aSamples,
                                      float aSamplerate, SoLoud::time aTime,
                                      unsigned int aChannel,
                                      unsigned int aChannels) {
  if (aChannel >= MAX_CHANNELS)
    return;

  if (aChannel == 0) {
    // Advance fades/oscillations once per callback, not once per channel.
    updateParams(aTime);

    const bool samplerateChanged = mSamplerate != (double)aSamplerate;
    if (samplerateChanged) {
      mSamplerate = (double)aSamplerate;
      mSmoothing =
          mSamplerate > 0.0
              ? 1.0 - exp(-1.0 / (kSmoothingSeconds * mSamplerate))
              : 1.0;
    }

    // Faders write mParam directly, so changes are detected by comparison
    // rather than by a dirty flag set in setFilterParameter.
    if (samplerateChanged || !mPrimed ||
        mParam[PeakingEq::FREQUENCY] != mCachedFrequency ||
        mParam[PeakingEq::GAIN] != mCachedGain ||
        mParam[PeakingEq::Q] != mCachedQ) {
      updateTargetCoefficients(mSamplerate);
    }

    mTargetMix = (mLinearGain - 1.0) * clampDouble(mParam[PeakingEq::WET], 0.0, 1.0);

    // Retarget instantly while the wet path is silent. eqTrainer moves the
    // band and then enables it in the same breath at the start of a round; if
    // the coefficients were still sliding towards the new centre frequency
    // when the mix came up, the first few milliseconds would be the *previous*
    // round's band. Nothing is audible at this point, so the jump is free.
    if (!mPrimed ||
        (fabs(mMix) < kInaudibleMix && fabs(mTargetMix) < kInaudibleMix)) {
      mB0 = mTargetB0;
      mA1 = mTargetA1;
      mA2 = mTargetA2;
      mMix = mTargetMix;
      mPrimed = true;
    }
  }

  // Every channel walks the same smoothing trajectory, starting from the
  // block-entry values, so they stay in lockstep. The write-back below is the
  // same for each channel.
  double b0 = mB0;
  double a1 = mA1;
  double a2 = mA2;
  double mix = mMix;

  const double targetB0 = mTargetB0;
  const double targetA1 = mTargetA1;
  const double targetA2 = mTargetA2;
  const double targetMix = mTargetMix;
  const double smoothing = mSmoothing;

  double s1 = mS1[aChannel];
  double s2 = mS2[aChannel];

  for (unsigned int i = 0; i < aSamples; i++) {
    b0 += (targetB0 - b0) * smoothing;
    a1 += (targetA1 - a1) * smoothing;
    a2 += (targetA2 - a2) * smoothing;
    mix += (targetMix - mix) * smoothing;

    // Transposed direct form II, with the bandpass numerator [b0, 0, -b0].
    // Smoothing the denominator this way is unconditionally stable: the
    // stability region of a biquad is a triangle, and one-pole smoothing only
    // ever produces convex combinations of two points already inside it.
    const double x = (double)aBuffer[i];
    const double bp = b0 * x + s1;
    s1 = s2 - a1 * bp;
    s2 = -b0 * x - a2 * bp;

    aBuffer[i] = (float)(x + mix * bp);
  }

  mS1[aChannel] = s1;
  mS2[aChannel] = s2;

  mB0 = b0;
  mA1 = a1;
  mA2 = a2;
  mMix = mix;
}

PeakingEq::PeakingEq() {
  mWet = 1.0f;
  mFrequency = 1000.0f;
  mGain = 0.0f;
  mQ = 1.0f;
}

int PeakingEq::getParamCount() { return 4; }

const char *PeakingEq::getParamName(unsigned int aParamIndex) {
  switch (aParamIndex) {
  case WET:
    return "Wet";
  case FREQUENCY:
    return "Frequency";
  case GAIN:
    return "Gain";
  case Q:
    return "Q";
  default:
    return "Unknown";
  }
}

unsigned int PeakingEq::getParamType(unsigned int /*aParamIndex*/) {
  return FLOAT_PARAM;
}

float PeakingEq::getParamMax(unsigned int aParamIndex) {
  switch (aParamIndex) {
  case WET:
    return 1.0f;
  case FREQUENCY:
    return 22000.0f;
  case GAIN:
    return 60.0f;
  case Q:
    return 100.0f;
  default:
    return 1.0f;
  }
}

float PeakingEq::getParamMin(unsigned int aParamIndex) {
  switch (aParamIndex) {
  case WET:
    return 0.0f;
  case FREQUENCY:
    return 1.0f;
  case GAIN:
    return -60.0f;
  case Q:
    return 0.01f;
  default:
    return 0.0f;
  }
}

SoLoud::FilterInstance *PeakingEq::createInstance() {
  return new PeakingEqInstance(this);
}
