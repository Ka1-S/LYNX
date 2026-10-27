#pragma once

// FL's Fruity Voltage Controller (docs/analise/REA_rodada164a.md §6): a Channel Rack generator that turns the notes it
// plays into control voltages for modular hardware through a DC-coupled audio interface. Eight outputs — the main
// stereo pair (the channel's own audio) and three more pairs added to chosen mixer inserts — each carry one source
// (pitch, gate, trigger, velocity, volume, pan, Mod X, Mod Y, clock or nothing) with its offset, scale and polarity:
// out = range (scale · (source + offset)); full scale 1.0 = 10 V. Clean-room: our own code from FL's decompiled
// behaviour and state format; LYNX keeps FL's state bytes as the channel's settings (ChannelDef::pluginState).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace lynx
{
class Instrument;
}

namespace lynx::voltage
{
enum class Source : std::uint8_t { pitch, gate, trigger, velocity, volume, pan, modX, modY, clock, none };
constexpr int sourceCount = 10;
constexpr int outputs = 8; // L main, R main, L out 2, R out 2, … L out 4, R out 4

struct Settings
{
    // FL's parameters: 0…7 offsets (0…65536, default 32768), 8…15 scales (0…65536, default 65536 = × 1)
    std::array<std::int32_t, 16> params { 32768, 32768, 32768, 32768, 32768, 32768, 32768, 32768,
                                          65536, 65536, 65536, 65536, 65536, 65536, 65536, 65536 };
    std::array<Source, outputs> source { Source::pitch, Source::gate, Source::velocity, Source::pan,
                                         Source::modX, Source::modY, Source::none, Source::none };
    bool enabled = false;          // FL's default: off (silence) until switched on
    int range = 0;                 // 0 = ±10 V, 1 = ±5 V, 2 = 0…10 V
    int pitchStandard = 0;         // 0 = 1 V / octave, 1 = 1.2 V / octave
    std::array<bool, outputs> unipolar {};
    std::array<int, 3> tracks {};  // mixer inserts of outputs 2…4 (0 = off)
    int clockPpq = 24;             // clock pulses per quarter note
    int pulseLength = 15;          // trigger / clock pulse length, samples (1…500)
    double pitchFactor = 1.0;      // pitch scaling (calibration)
    std::array<std::uint8_t, outputs> voice {}; // FL v3 "mod source" per output (kept; LYNX follows the last note)

    bool operator== (const Settings&) const = default;
};

// FL's state (versions 1…3; LYNX writes 3): nullopt when it does not parse.
std::optional<Settings> parse (const std::vector<std::uint8_t>& state);
std::vector<std::uint8_t> write (const Settings& settings);

// One output's value (0…1 full scale = 10 V, before the range) for a source value.
float offsetOf (const Settings& s, int output) noexcept;
float scaleOf (const Settings& s, int output) noexcept;
// The range transform (±10 V: clip to ±1; ±5 V: half, clip; 0…10 V: half, + 0.5, clip to 0…1).
float applyRange (int range, float v) noexcept;
// Pitch source for a note (key + cents): 0 at C5 (key 60); 1 V / octave = 1/120 per semitone.
float pitchValue (const Settings& s, double key) noexcept;

const char* sourceName (Source source) noexcept;
const char* outputName (int output) noexcept; // "Main L", "Out 2 R"…

std::unique_ptr<Instrument> makeInstrument();
} // namespace lynx::voltage
