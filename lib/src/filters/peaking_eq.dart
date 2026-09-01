// ignore_for_file: public_member_api_docs

import 'package:flutter_soloud/src/filters/filters.dart';
import 'package:flutter_soloud/src/sound_handle.dart';
import 'package:flutter_soloud/src/sound_hash.dart';

/// Parameters of the single-band peaking (bell) equalizer.
///
/// The response is the RBJ audio-EQ-cookbook peaking filter, but implemented so
/// that [wet] only scales a bandpass sitting alongside the dry signal. Fading
/// [wet] therefore switches the band in and out without touching the recursive
/// coefficients, which is what keeps the transition free of clicks. Fade it
/// rather than setting it, and the change is inaudible:
///
/// ```dart
/// SoLoud.instance.filters.peakingEqFilter.wet
///     .fadeFilterParameter(to: 1, time: const Duration(milliseconds: 20));
/// ```
enum PeakingEqEnum {
  wet,
  frequency,
  gain,
  q;

  final List<double> _mins = const [0, 1, -60, 0.01];
  final List<double> _maxs = const [1, 22000, 60, 100];
  final List<double> _defs = const [1, 1000, 0, 1];

  double get min => _mins[index];
  double get max => _maxs[index];
  double get def => _defs[index];

  @override
  String toString() => switch (this) {
    PeakingEqEnum.wet => 'Wet',
    PeakingEqEnum.frequency => 'Frequency',
    PeakingEqEnum.gain => 'Gain',
    PeakingEqEnum.q => 'Q',
  };
}

abstract class _PeakingEqInternal extends FilterBase {
  const _PeakingEqInternal(SoundHash? soundHash, int? busId)
    : super(FilterType.peakingEq, soundHash, busId);

  PeakingEqEnum get queryWet => PeakingEqEnum.wet;
  PeakingEqEnum get queryFrequency => PeakingEqEnum.frequency;
  PeakingEqEnum get queryGain => PeakingEqEnum.gain;
  PeakingEqEnum get queryQ => PeakingEqEnum.q;
}

class PeakingEqSingle extends _PeakingEqInternal {
  const PeakingEqSingle(super.soundHash, super.busId);

  /// Dry/wet mix, 0..1. Fade this to bring the band in and out.
  FilterParam wet({SoundHandle? soundHandle}) => FilterParam(
    soundHandle,
    super.busId,
    filterType,
    PeakingEqEnum.wet.index,
    PeakingEqEnum.wet.min,
    PeakingEqEnum.wet.max,
  );

  /// Centre frequency of the bell, in Hz.
  FilterParam frequency({SoundHandle? soundHandle}) => FilterParam(
    soundHandle,
    super.busId,
    filterType,
    PeakingEqEnum.frequency.index,
    PeakingEqEnum.frequency.min,
    PeakingEqEnum.frequency.max,
  );

  /// Peak gain in dB. Positive boosts, negative cuts.
  FilterParam gain({SoundHandle? soundHandle}) => FilterParam(
    soundHandle,
    super.busId,
    filterType,
    PeakingEqEnum.gain.index,
    PeakingEqEnum.gain.min,
    PeakingEqEnum.gain.max,
  );

  /// Q of the bell. Higher is narrower.
  FilterParam q({SoundHandle? soundHandle}) => FilterParam(
    soundHandle,
    super.busId,
    filterType,
    PeakingEqEnum.q.index,
    PeakingEqEnum.q.min,
    PeakingEqEnum.q.max,
  );
}

class PeakingEqGlobal extends _PeakingEqInternal {
  const PeakingEqGlobal() : super(null, null);

  /// Dry/wet mix, 0..1. Fade this to bring the band in and out.
  FilterParam get wet => FilterParam(
    null,
    null,
    filterType,
    PeakingEqEnum.wet.index,
    PeakingEqEnum.wet.min,
    PeakingEqEnum.wet.max,
  );

  /// Centre frequency of the bell, in Hz.
  FilterParam get frequency => FilterParam(
    null,
    null,
    filterType,
    PeakingEqEnum.frequency.index,
    PeakingEqEnum.frequency.min,
    PeakingEqEnum.frequency.max,
  );

  /// Peak gain in dB. Positive boosts, negative cuts.
  FilterParam get gain => FilterParam(
    null,
    null,
    filterType,
    PeakingEqEnum.gain.index,
    PeakingEqEnum.gain.min,
    PeakingEqEnum.gain.max,
  );

  /// Q of the bell. Higher is narrower.
  FilterParam get q => FilterParam(
    null,
    null,
    filterType,
    PeakingEqEnum.q.index,
    PeakingEqEnum.q.min,
    PeakingEqEnum.q.max,
  );
}
