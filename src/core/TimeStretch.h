#pragma once

// Pitch-preserving time-stretch, done off the audio thread once per (sample, ratio, method), like FL's "offline"
// modes (docs/analise/REA_rodada3.md §5). Round 160 (docs/analise/REA_rodada160.md) replaced the earlier basic WSOLA
// with Signalsmith Stretch (MIT, third_party/signalsmith) plus LYNX's own transient handling:
//  - ratios within 0.1 % of 1 (e.g. FL's recorded takes, whose stretch time is their own length) are resampled with a
//    windowed sinc instead — at most 1.7 cents of pitch, no stretch artefacts at all;
//  - otherwise the spectral stretcher runs, and the attacks it detects are
//    spliced back unstretched (no smeared or doubled hits, no pre-echo).

#include <memory>
#include <vector>

#include "core/Instruments.h"

namespace lynx
{
// `ratio` = output duration / input duration (2 = twice as long, same pitch).
std::shared_ptr<SampleData> timeStretch (const SampleData& input, double ratio, StretchMethod method = StretchMethod::general);

// FL's stretch mode (channel record 0xD7 +0x6C, REA_rodada3 §5.2) → LYNX's method. 0 (Resample) has no
// pitch-preserving stretch; callers keep resampling for it.
StretchMethod stretchMethodForFlMode (int flMode) noexcept;

// A voice's real-time pitch shifter (round 162, issue #9: a sampler stretched keeping its pitch follows the keys like
// FL's Stretch modes — the note transposes the sample and its length stays). Signalsmith Stretch at a 1:1 time ratio
// with a transposition factor. The voice primes it with primeLength() frames of its source when the note starts, so
// the output is aligned with the source (no latency); afterwards each process() call takes n source frames and returns
// n shifted frames. allocate() on the UI thread; start()/process() never allocate.
class VoicePitchShifter
{
public:
    VoicePitchShifter();
    ~VoicePitchShifter();
    VoicePitchShifter (const VoicePitchShifter&) = delete;
    VoicePitchShifter& operator= (const VoicePitchShifter&) = delete;

    void allocate (double sampleRate, StretchMethod method);
    bool ready() const noexcept;
    int primeLength() const noexcept;
    // `l`/`r`: primeLength() frames of the source from the note's start (r may equal l)
    void start (const float* l, const float* r, double factor) noexcept;
    void process (const float* inL, const float* inR, float* outL, float* outR, int n, double factor) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace stretchdetail
{
// Exposed for the tests: attack positions (frames) found in a mono signal, strongest-first filtering already applied.
std::vector<long> detectAttacks (const float* mono, long frames, double sampleRate, double minRiseDb);
// The ratio band that is resampled instead of stretched.
constexpr double resampleTolerance = 0.001;

// The spectral stretcher's analysis block and interval, and the attack splicing (0 dB = none).
struct Preset
{
    double blockSeconds, intervalSeconds, attackRiseDb, holdSeconds;
};
Preset presetFor (StretchMethod method) noexcept;
// The spectral stretch with a given preset (no near-unity shortcut).
std::shared_ptr<SampleData> stretchWith (const SampleData& input, double ratio, const Preset& preset);
} // namespace stretchdetail
} // namespace lynx
