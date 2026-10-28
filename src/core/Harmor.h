#pragma once

// FL Studio's Harmor rebuilt as the additive engine decoded in docs/analise/REA_rodada34.md (clean-room; a 1:1 port of
// our own reference model r34/py/hmodel.py, which was checked against renders of the installation's DLL). Every
// enabled part (A, B) × unison voice is a voice record with its own bank of up to 520 sine partials (self-oscillation,
// Sub 1/3/4, 516 harmonics); every control block (2 ms) the record rebuilds its target amplitudes from the two timbres
// through the part's user-ordered module chain (local EQ, prism, harmonizer, filters + resonance, phaser, pluck, blur,
// protection, clip, volume), then the subs-below fade, self-oscillation, top fade, global EQ and auto-gain, with
// Harmor's 43 articulators per part (point-curve envelopes, LFOs, key/velocity/X/Y/Z/random/unison maps), unison
// (classic/blurred/uniform/random/Hz), legato/portamento/mono, stereo tremolo and the master volume.
//
// The channel keeps the plugin's raw saved state (record 0xD5, as FL wrote it) in ChannelDef::pluginState.
//
// REA round 75 (REA_rodada75) adds image synthesis (the part's gain and frequency planes read along a time position:
// speed, time offset with smoothing, loop / ping-pong / crossfade, interpolation curves, gain pixel scale and mix, the
// frequency modes Oct/Hz/Wide Hz/Unison with the local mask, formant shift with the per-column pitch track and the
// formant-mutation curve, the record ending with a one-shot image), arpeggio envelope markers (chord order, the
// note playing each segment, the release that waits for the next marker), the noise resonance (type 109), the
// "pluck replaces blur decay" option, phaser and harmonizer masks, timbre blend modes 1–5 (5: pluck blend), the global
// envelope scales, the harmonic-randomness module and the per-block X/Y/Z smoothing.
//
// Fallbacks (harmorFallbacks() lists the ones a state needs): resynthesis-mode images (a sample analysed at load time)
// and images with a per-column gain table (+0x100) are ignored (the part plays its timbres), unknown filter/resonance/
// phaser shapes and clip modes > 4 switch that stage off, blend modes > 5 play as mode 0. FL's IPP vector functions
// run at 11-bit accuracy at precision "Average" (Exp/Expm1/Div_32f_A11); LYNX computes them exactly.
//
// Effects rack (REA_rodada38): distortion, chorus, delay, reverb and the Maximus compressor in the state's unit order
// (core/HarmlessFx.h, hlfx::HarmorRack), plugged into the engine's `harmor::FxHook` slot; the compressor needs Harmor's
// own preset System\Compression\<type>.fst from the user's FL installation (FlpImport stores its Maximus state in
// ChannelDef::modelParams: size, then the bytes).
//
// Random draws: the plugin's Mersenne Twister is seeded with a fixed value per instance (FL seeds it from the clock).
// Polyphony: a pool of 6 notes' worth of records (16…96, sized when the instrument is prepared); the oldest are stolen.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Instruments.h"

namespace lynx
{
// true when the exact engine can play this Harmor state (version ≥ 16 with articulators and curves, parsed fully)
bool harmorStateUsable (const std::vector<std::uint8_t>& state);

// An old state (version 11…15: the first releases' presets, FL's Default.fst included) in the current layout, as
// FL's Harmor re-saves it after loading (REA_rodada165g-flbeta165s §1); nullopt when the state is not an old one (or unreadable).
// The engine plays old states directly; the importer stores the upgraded bytes so the settings panel can edit them.
std::optional<std::vector<std::uint8_t>> harmorUpgradeState (const std::vector<std::uint8_t>& state);

// the round-34 fallbacks this state relies on (empty: everything it uses is modelled), for logs and reports
std::vector<std::string> harmorFallbacks (const std::vector<std::uint8_t>& state);

// Byte offset of a raw integer parameter inside the state (part 0/1: part parameter 0…385; part −1: global 0…52, then
// 53…58 = the effects rack's unit order),
// or −1 when the state doesn't hold it. Used by the channel settings panel to edit the state in place.
int harmorParamOffset (const std::vector<std::uint8_t>& state, int part, int index);

// the rack compressor type of a state (global misc +52; 0 = off, n = Harmor's System\Compression\<n>.fst)
int harmorCompressorType (const std::vector<std::uint8_t>& state);

std::unique_ptr<Instrument> makeHarmorInstrument();

namespace harmor
{
// The effects rack's slot: every host block, before the master volume (where FL runs its rack), with the records'
// dry mix (left/right, processed in place) and their FX-send mix (fxL/fxR, which joins the chain at the rack's unit 4);
// voices: records existed in this block. LYNX plugs Harmor's rack (hlfx::HarmorRack) in here.
struct FxHook
{
    virtual ~FxHook() = default;
    virtual bool enabled() const noexcept = 0;
    virtual bool quiet() const noexcept = 0;
    virtual void process (float* left, float* right, const float* fxL, const float* fxR, int n, bool voices) noexcept = 0;
    virtual void advance (int n) noexcept = 0;
};

// One note for the reference renderer (the verification harness drives the engine exactly like our Python model).
struct RefNote
{
    int start = 0, key = 60;
    double velocity = 1.0, finalVol = 1.0, finalPan = 0.0;
    int release = -1; // sample of the note-off (−1: held)
};

// Renders `notes` from `state` like hmodel.render (host blocks of `block` samples split at every event) with the
// state's effects rack (compressorState: the Maximus state of Harmor's System\Compression\<type>.fst; empty = the
// compressor stays off). `mt` (625 words: index, then the 624-word state) seeds the random draws like the DLL's
// Mersenne Twister; empty = LYNX's fixed per-instance seed. Returns interleaved stereo float samples (2·length),
// empty if the state is unusable.
std::vector<float> renderReference (const std::vector<std::uint8_t>& state, const std::vector<RefNote>& notes, double sampleRate,
                                    int length, int block = 512, const std::vector<std::uint32_t>& mt = {}, double bpm = 120.0,
                                    const std::vector<std::uint8_t>& compressorState = {});

// IPP's 11-bit Exp_32f_A11 / Ln_32f_A11 as FL's DLL computes them on the AVX2 code path (Harmor's precision
// "Average"; REA_rodada165g-flbeta165h §3), exposed for the tests
float ippExpA11Model (float x) noexcept;
float ippLnA11Model (float x) noexcept;
} // namespace harmor
} // namespace lynx
