#pragma once

// FL Studio's small legacy Fruity effects as exact LYNX engines, rebuilt in docs/analise/REA_rodada165k.md
// (clean-room: the plug-ins' layouts, formulas and behaviour in our own words; no Image-Line code or data in this
// repository). All five are Delphi plug-ins whose kernels run on the x87 in 80-bit precision; LYNX does the same
// (long double, 64-bit precision, round to nearest) so their output is bit-identical on x86.
//
// Every parameter is FL's control over its range (value = (raw − min) / (max − min)), so the LYNX editor and FL
// automation drive the same values FL's ProcessParam receives:
//
//   Fruity Phase Inverter (EffectKind::flPhaseInverter): [0] Invert 0…1024 (1024): None / Left / Right — a third of
//     the range each (FL rounds 3·x − ½ to the nearest integer).
//   Fruity Center (EffectKind::flCenter): [0] On 0/1 (1) — a DC-removing two-pole high-pass.
//   Fruity Bass Boost (EffectKind::flBassBoost): [0] Frequency 0…1024 (512: 20…100 Hz in whole Hz), [1] Amount
//     0…1024 (768) — a resonant low-pass (two poles) added to the dry signal.
//   Fruity Delay (v1) (EffectKind::flDelay1): [0] Input 0…1024 (338), [1] Feedback 0…1024 (512), [2] Cutoff 0…1024
//     (1024), [3] Tempo 0…1024 (0 = the host's; else 60…280 BPM), [4] Time 1…16 sixteenths (3), [5] Mode 0…1024 (0):
//     Normal / Inverted stereo / Ping-pong — a sixteenth-note delay line with a one-pole low-pass in its feedback
//     loop, the dry signal always at unity.
//   Fruity 7 Band EQ (EffectKind::fl7BandEq): [0…6] band gains −1800…1800 (0 = ±0 dB, ±18 dB): a low shelf at 63 Hz,
//     peaks at 250, 500, 1500, 3000 and 5000 Hz and a high shelf at 8 kHz, always designed for 44.1 kHz whatever
//     the rate (as FL's are). A band at 0 dB keeps running for 48 render calls (its tail), then is skipped.
//
// Host blocks longer than 512 frames are processed as 512-frame engine calls: the filters keep their states in
// 80-bit precision inside a call and as doubles between calls, as the plug-ins do.

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Effects.h"

namespace lynx::fllegacy
{
constexpr int phaseInverterParams = 1, centerParams = 1, bassBoostParams = 2, eq7Params = 7, delay1Params = 6;
constexpr int eq7Bands = 7;

// FL's raw controls; parse = the plug-in's saved state (record 0xD5): i32 version (1) + one i32 per control
struct PhaseInverterState { int invert = 1024; };
struct CenterState { int on = 1; };
struct BassBoostState { int freq = 512, amount = 768; };
struct Eq7State { std::array<int, eq7Bands> gain {}; };
struct Delay1State { std::array<int, delay1Params> raw { 338, 512, 1024, 0, 3, 0 }; }; // input fb cutoff tempo time mode

std::optional<PhaseInverterState> parsePhaseInverter (const std::vector<std::uint8_t>& state);
std::optional<CenterState> parseCenter (const std::vector<std::uint8_t>& state);
std::optional<BassBoostState> parseBassBoost (const std::vector<std::uint8_t>& state);
std::optional<Eq7State> parseEq7 (const std::vector<std::uint8_t>& state);
std::optional<Delay1State> parseDelay1 (const std::vector<std::uint8_t>& state);

void toParams (const PhaseInverterState& s, float* params) noexcept;
void toParams (const CenterState& s, float* params) noexcept;
void toParams (const BassBoostState& s, float* params) noexcept;
void toParams (const Eq7State& s, float* params) noexcept;
void toParams (const Delay1State& s, float* params) noexcept;
PhaseInverterState phaseInverterFromParams (const float* params) noexcept;
CenterState centerFromParams (const float* params) noexcept;
BassBoostState bassBoostFromParams (const float* params) noexcept;
Eq7State eq7FromParams (const float* params) noexcept;
Delay1State delay1FromParams (const float* params) noexcept;

std::string phaseInverterParamText (int param, float value01);
std::string centerParamText (int param, float value01);
std::string bassBoostParamText (int param, float value01);
std::string eq7ParamText (int param, float value01);
std::string delay1ParamText (int param, float value01);
extern const float eq7BandHz[eq7Bands];

// ------------------------------------------------------------------------------------------------------- engines
// 0 none, 1 left, 2 right
int phaseInverterMode (int raw) noexcept;

class CenterEngine
{
public:
    void prepare (int sampleRate) noexcept; // FL's sample-rate notification: coefficient, filter cleared
    void set (const CenterState& s) noexcept { on = s.on != 0; }
    void process (float* left, float* right, int n) noexcept; // one FL render call

private:
    bool on = true;
    float c = 4.567e-06f, q = 0.96f;
    double st[4] {}; // low-pass L/R, velocity L/R
};

class BassBoostEngine
{
public:
    void prepare (int sampleRate) noexcept { rate = sampleRate; }
    void set (const BassBoostState& s) noexcept; // FL's ProcessParam for both controls
    void process (float* left, float* right, int n) noexcept;

private:
    int rate = 44100;
    float c = 0, r = 0, gain = 0;
    double st[4] {};
};

class Eq7Engine
{
public:
    Eq7Engine() noexcept; // a fresh plug-in: every band at 0 dB, not running
    void setBand (int band, int raw) noexcept; // FL's ProcessParam for one band (designs it, restarts its tail count)
    void process (float* left, float* right, int n) noexcept;

private:
    struct Band
    {
        int type = 6;
        float freq = 0, bw = 0, gain = 0;
        float co[5] {};  // b0 b1 b2 a1 a2
        float h[8] {};   // x1 L/R, x2 L/R, y1 L/R, y2 L/R
        int counter = 0; // > 0: calls left before it stops; −1: running; 0: skipped
    };
    void design (Band& b) noexcept;
    std::array<Band, eq7Bands> bands {};
};

class Delay1Engine
{
public:
    Delay1Engine() noexcept = default;
    // a fresh plug-in at this rate (its default controls), at most maxBlock frames per process call
    void prepare (int sampleRate, int maxBlock) noexcept;
    void setHostTempo (float bpm) noexcept;       // the host's tempo (used while Tempo is 0)
    void setParam (int index, int raw) noexcept;  // FL's ProcessParam for one control
    void process (float* left, float* right, int n) noexcept; // one FL render call (n ≤ maxBlock), in place

private:
    void setLength() noexcept;
    float v[delay1Params] {};
    int rate = 44100, extra = 512;
    float hostTempo = 140.0f, limit = 0.001f;
    int tempoBpm = 0, sixteenth = 4725, steps = 3, mode = 0;
    float inTarget = 0, inCur = 0, fbTarget = 0, fbCur = 0, cutoff = 1.0f;
    double z28 = 0, z30 = 0; // the feedback low-pass states
    std::vector<float> buf;  // interleaved stereo ring: (delay + maxBlock) frames
    int length = 0, rd = 0, wr = 0;
};

std::unique_ptr<Effect> makePhaseInverter();
std::unique_ptr<Effect> makeCenter();
std::unique_ptr<Effect> makeBassBoost();
std::unique_ptr<Effect> makeEq7();
std::unique_ptr<Effect> makeDelay1();
} // namespace lynx::fllegacy
