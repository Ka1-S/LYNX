#pragma once

// Transmitter (EffectKind::transmitter), FL Studio 2026's transient / sustain splitter, rebuilt clean-room from the
// behaviour decoded from FL's Transmitter_x64.dll front end and the engine in Shared/ildsp_x64.dll
// (docs/analise/REA_rodada165g-fltrans165.md). No Image-Line code here: an own implementation of the behaviour,
// measured against FL's engine (EXEC goldens).
//
// Signal flow: the detection signal (the input, mono (L + R) / 2) drives an 8-band onset detector whose envelope
// e ∈ [0, 1] splits the input, delayed by the lookahead, into transient = x·e and sustain = x·(1 − e). Each part goes
// through a tilt filter and one section effect (Saturation, Bounce, Diffusion, Blur, Delay or Off), then
// output = transient · level · on + sustain · level · on.
//
// LYNX's parameters (0…1 each) are FL's 42 controls over their ranges: [0…32] raw / 10000, [33…41] the switches and
// selectors (raw / their maximum). See controlRange().
//
// No JUCE here (tests/FlTrans165Tests.inc).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace lynx
{
class Effect;
} // namespace lynx

namespace lynx::transmitter
{
constexpr int count = 42;

enum Param : int
{
    sensitivity = 0, hold, release, transientLevel, sustainLevel, transientTilt, sustainTilt,
    tDiffMix, tDiffFeedback, tDiffDelay, sDiffMix, sDiffFeedback, sDiffDelay,
    tSatMix, tSatDrive, tSatWarmth, sSatMix, sSatDrive, sSatWarmth,
    tBounceMix, tBounceAmpCurve, tBounceDelayCurve, sBounceMix, sBounceAmpCurve, sBounceDelayCurve,
    tDelayMix, tDelayTime, tDelaySynced, tDelayFeedback, sDelayMix, sDelayTime, sDelaySynced, sDelayFeedback,
    lookahead, stereoLink, mode, transientOn, sustainOn, transientEffect, sustainEffect, tDelaySync, sDelaySync
};

// the section effects (FL's selector order)
enum Section : int { saturation = 0, bounce, diffusion, blur, delay, off };

struct Range { int lo, hi; };
Range controlRange (int k);               // FL's raw control range
int rawFromParam (int k, float v);        // LYNX 0…1 → FL raw (rounded)
float paramFromRaw (int k, int raw);      // FL raw → LYNX 0…1
float engineValue (int k, int raw);       // FL's front-end mapping: raw control → the engine's value (ms, dB, …)
std::array<float, count> defaults();      // FL's Default preset
std::string paramText (int k, float v);   // FL's value texts

// FL's state (0xD5): version 3, 42 raw controls, send / sidechain tracks, editor bytes
struct State
{
    std::array<int, count> raw {};
    int transientSend = 0, sustainSend = 0, sidechain = 0;
};
std::optional<State> parse (const std::vector<std::uint8_t>& data);

// the engine, per sample rate (stereo; float, like FL's)
class Engine
{
public:
    Engine();
    ~Engine();
    void prepare (double sampleRate);
    void setTempo (double bpm);
    void setRaw (int k, int raw);          // one FL control (raw value)
    void setValue (int k, float value);    // one engine value (FL's engine units: ms, dB, …)
    void setParams (const float* params);  // LYNX 0…1 parameters (all 42)
    // in-place capable; detection = the input (no sidechain yet)
    void process (const float* inL, const float* inR, float* outL, float* outR, int n);
    int latency() const;
    struct Impl;

private:
    std::unique_ptr<Impl> impl;
};

std::unique_ptr<Effect> make();
} // namespace lynx::transmitter
