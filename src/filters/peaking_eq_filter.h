#ifndef PEAKING_EQ_FILTER_H
#define PEAKING_EQ_FILTER_H

#include "../soloud/include/soloud.h"

class PeakingEq;

/// A single-band peaking (bell) equalizer.
///
/// The response at full wet is identical to the RBJ audio-EQ-cookbook peaking
/// filter (the same one `ma_peak2` implements), but it is realised as
///
///     y[n] = x[n] + mix * bp[n],   mix = wet * (linearGain - 1)
///
/// where `bp` is a bandpass with *gain-independent* poles. Writing the filter
/// this way is what makes enabling/disabling the band free of clicks: the dry
/// and wet signals share the same `x[n]` term, so `mix` can be swept from 0 to
/// its target without ever changing the recursive coefficients, and therefore
/// without any discontinuity in the filter state. See the derivation in
/// `updateTargetCoefficients`.
class PeakingEqInstance : public SoLoud::FilterInstance {
public:
  explicit PeakingEqInstance(PeakingEq *aParent);
  virtual ~PeakingEqInstance();

  virtual void filterChannel(float *aBuffer, unsigned int aSamples,
                             float aSamplerate, SoLoud::time aTime,
                             unsigned int aChannel, unsigned int aChannels);

private:
  /// Recomputes [mTargetB0], [mTargetA1], [mTargetA2] and [mLinearGain] from
  /// the current frequency/gain/Q parameter values.
  void updateTargetCoefficients(double aSamplerate);

  PeakingEq *mParent;

  /// Transposed-direct-form-II state of the bandpass, per channel. Kept in
  /// double so that low centre frequencies (30 Hz and below at 48 kHz) don't
  /// suffer the coefficient-quantisation error a float biquad would have.
  double mS1[MAX_CHANNELS];
  double mS2[MAX_CHANNELS];

  /// Coefficients the filter is heading towards.
  double mTargetB0;
  double mTargetA1;
  double mTargetA2;
  double mTargetMix;

  /// Coefficients actually in use, one-pole smoothed towards the targets once
  /// per sample. These are the values at the *start* of the current block;
  /// every channel runs the same trajectory from here so the channels can't
  /// drift apart.
  double mB0;
  double mA1;
  double mA2;
  double mMix;

  /// Per-sample smoothing coefficient, derived from the sample rate.
  double mSmoothing;

  /// Parameter values the current coefficients were computed from, used to
  /// notice changes that arrive through a fader (which writes `mParam`
  /// directly and never goes through `setFilterParameter`).
  double mCachedFrequency;
  double mCachedGain;
  double mCachedQ;

  /// Linear peak gain, i.e. 10^(gainDb/20).
  double mLinearGain;

  double mSamplerate;

  /// False until the first block has run, so that the first block snaps to its
  /// coefficients instead of sliding up from silence.
  bool mPrimed;
};

class PeakingEq : public SoLoud::Filter {
public:
  enum FILTERATTRIBUTE {
    /// Dry/wet mix, 0..1. Fade this to switch the band in and out without a
    /// click; at 0 the filter is bit-exact bypass.
    WET = 0,
    /// Centre frequency of the bell, in Hz.
    FREQUENCY = 1,
    /// Peak gain in dB. Positive boosts, negative cuts.
    GAIN = 2,
    /// Q of the bell. Higher is narrower.
    Q = 3
  };

  PeakingEq();

  virtual int getParamCount();
  virtual const char *getParamName(unsigned int aParamIndex);
  virtual unsigned int getParamType(unsigned int aParamIndex);
  virtual float getParamMax(unsigned int aParamIndex);
  virtual float getParamMin(unsigned int aParamIndex);
  virtual SoLoud::FilterInstance *createInstance();

  float mWet;
  float mFrequency;
  float mGain;
  float mQ;
};

#endif // PEAKING_EQ_FILTER_H
