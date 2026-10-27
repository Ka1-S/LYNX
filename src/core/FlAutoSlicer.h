#pragma once

// The "auto slicing" of Image-Line's plugin framework (the slicer Fruity Granulizer runs when its slice mode is
// "auto"), rebuilt from the behaviour decoded in docs/analise/REA_rodada165g-pitch.md §1 (clean-room; checked against
// the installation's DLL run unmodified: identical slice positions on every test sample).
//
// Outline: a normalised mono copy at 44.1 kHz, four passes of a gentle high-pass, 128-point Hamming-windowed spectra
// every 16 samples, the RMS of a low band (bins 1–5) and a high band (bins 6–63), a peak-hold + release envelope per
// band whose frame-to-frame rise gives an onset strength, peaks of that strength picked at least 60 ms apart, then each
// onset moved forward to the first sample louder than 5 % of the peak and back to the nearest zero crossing (≤ 20 ms).

#include <cstdint>
#include <vector>

namespace lynx::flslice
{
// Slice starts (frames of the sample, ascending). `right` = nullptr for a mono sample. Data are the sample's frames as
// FL reads them (16-bit samples as value / 32768).
std::vector<std::int64_t> autoSlice (const float* left, const float* right, std::int64_t frames, double sampleRate);
} // namespace lynx::flslice
