#pragma once

// FL Studio's Sytrus rebuilt from the behaviour decoded in docs/analise/REA_rodada31.md (clean-room: ported from the
// document and from our own numerical reference model, which was checked against the installation's DLL on the 268
// Sytrus channels of the demo projects). One instance = one Sytrus channel, driven by the plugin's raw saved state
// (ChannelDef::pluginState): 6 operators (16 384-point wavetables, noise, pluck, band limit, CENTER/DECLICK, GLOBAL
// phase), the FM/RM matrix with its per-sample order, articulators (envelopes, LFOs, maps), unison (4 modes, sub layer,
// OSC grouping), portamento, 3 filter units (13 types, waveshaper), voice end and declick ramps, and the post-voice chain
// (oversampling decimators, effects unit/chorus, 3 delays, the shared reverb, 3-band EQ, DC stage).
//
// Rendering settings, as FL: while LYNX exports (offline render) a channel uses the state's *render* oversampling
// order and envelope-accuracy tick (main ints 0x310/0x308) and the 16 384-point band-limited tables — what FL uses when
// it exports a project (§1.2, §7.2; most demo patches then run 8× oversampled); during live playback it uses the live
// pair (0x30C/0x304) and 4 096-point tables, as FL plays them.

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/Instruments.h"

namespace lynx
{
namespace sytrus
{
struct Patch; // parsed state + wavetables (immutable, shared)

// Parses a Sytrus plugin state (record 0xD5 of an FL project, or an .fst's state). nullptr if the state cannot be
// read or is older than the layout the engine knows (state version < 17).
std::shared_ptr<const Patch> parsePatch (const std::vector<std::uint8_t>& state);

// true if parsePatch would succeed (cheap: no wavetables are built)
bool stateUsable (const std::vector<std::uint8_t>& state);

// The "Main settings" page of Sytrus (REA_rodada74 §3): master level / LFO / pitch, the global volume and filter
// envelope scales, unison, Mod X/Y, PORTA and the quality settings (oversampling order and envelope accuracy, live and
// export). `index` is the main-block int (offset / 4; −1 = the PORTA flag byte), low…high the plugin control's range.
struct Setting
{
    const char* name;
    int index, low, high;
};
const std::vector<Setting>& settings();
// the raw value of settings()[setting] in a state, or nothing if the state cannot be read
std::optional<int> settingValue (const std::vector<std::uint8_t>& state, int setting);
// writes it (clamped to the control's range) and re-stores the state; false if the state cannot be read
bool setSetting (std::vector<std::uint8_t>& state, int setting, int value);

// Note of the engine (per FL note).
struct NoteParams
{
    float key = 60.0f;      // MIDI key (60 = C5)
    float cents = 0.0f;     // channel + note pitch offset (cents)
    float velocity = 0.78125f; // FL velocity / 128 (voice event 3)
    float releaseVelocity = 1.0f; // voice event 5 (release-knob multiplier when the patch asks for it)
    float pan = 0.0f;       // note pan −1…1 (adds to Final.Pan)
    int colour = 0;         // note colour (14 inverts PORTA)
    std::int64_t id = 0;    // caller's handle (release by id)
};

// The engine for one instance. Everything is allocated by prepare()/setPatch(); render never allocates except
// (rarely) the band-limited table cache when more distinct tables are sounding than were reserved.
class Engine
{
public:
    Engine();
    ~Engine();
    Engine (const Engine&) = delete;
    Engine& operator= (const Engine&) = delete;

    // renderMode: FL's export settings (true) or the live ones. maxBlock: largest render() length handled in one
    // pass (longer calls are split).
    void prepare (double sampleRate, bool renderMode = true, int maxBlock = 512);
    // new patch: every voice stops, the post chain is rebuilt (UI thread or audio thread between renders)
    void setPatch (std::shared_ptr<const Patch> patch);
    const Patch* patch() const noexcept { return current.get(); }
    void setTempo (double bpm) noexcept;
    void setMaxPolyphony (int groups) noexcept { maxPoly = groups; }

    void noteOn (const NoteParams& note) noexcept;
    void release (std::int64_t id) noexcept;   // FL's Voice_Release for every voice of that note
    void releaseKey (int key) noexcept;        // releases the held notes of a key
    // a slide note (issue #14): the unreleased voices glide to `toCents` (Final.Pitch, cents from C5) over `samples`
    void slideTo (double toCents, float samples) noexcept;
    void killAll() noexcept;                   // every voice ramps out (FL's kill)
    void reset() noexcept;                     // silence now: voices and effect tails cleared (FPD_Flush)

    // Adds n host-rate samples. finalVol / finalPan = FL's Final.Vol / Final.Pan of the channel (per call).
    void render (float* left, float* right, int n, float finalVol, float finalPan) noexcept;

    bool idle() const noexcept; // no voice, and the effects have been silent for 2 s
    int activeVoices() const noexcept;
    // the patch follows the plugin's running time (GLOBAL LFOs, GLOBAL operator phase): render() must keep being
    // called while no voice sounds, as FL calls Gen_Render every block
    bool followsRunningTime() const noexcept;

    // test hooks: replace an operator's 16 384-point table (e.g. the DLL's own), Delphi RandSeed for random maps
    void overrideTable (int op, const std::vector<float>& table);
    void setRandSeed (std::uint32_t seed) noexcept;

    struct Impl;

private:
    std::shared_ptr<const Patch> current;
    std::unique_ptr<Impl> impl;
    int maxPoly = 1 << 20;
};
} // namespace sytrus

std::unique_ptr<Instrument> makeSytrusInstrument();
} // namespace lynx
