#pragma once

// The metronome as FL Studio 2026 plays it (REA round 165z-a2, measured with FL's Tools › Audio logger):
//  - five sounds, in the order of FL's right-click "Metronome sound" menu: Hi-hat, Tick, Beep, Cowbell, Pulse; the
//    factory default is Tick (FL's "Metronome number" = 1). They are FL's own sample files (Data\System\Metronome.wav,
//    Metronome2..5.wav, 44.1 kHz), read from the user's FL install at run time; without FL, LYNX synthesises sounds
//    of the same length and peak (fallbackSound).
//  - a beat plays the sample at its pitch × 0.6233 (−4.11 dB); the downbeat (first beat of a bar) plays it two
//    semitones up (linear interpolation) × 0.9404 (−0.53 dB); a mono file goes to both sides at the same gain.
//  - the click goes into one mixer track (Settings › Audio › Mixer › "Metronome track": "---" = the Master, or an
//    insert) and through its effects and fader. It is never in an export.
// No JUCE here: the engine's Metronome (engine/Metronome.cpp) and the tests share it.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace lynx::metronome
{
constexpr int soundCount = 5;   // Hi-hat, Tick, Beep, Cowbell, Pulse
constexpr int defaultSound = 1; // Tick
constexpr float beatGain = 0.6233f;
constexpr float downbeatGain = 0.9404f;
constexpr double downbeatPitch = 1.122462048309373; // 2^(2/12): two semitones up
constexpr int masterTrack = 0;                      // FL's "---"

// The menu names (translated by the UI), in FL's order.
inline const char* soundName (int sound) noexcept
{
    static constexpr const char* names[soundCount] = { "Hi-hat", "Tick", "Beep", "Cowbell", "Pulse" };
    return names[std::clamp (sound, 0, soundCount - 1)];
}

// FL's file for the sound, in its Data\System folder.
inline const char* soundFileName (int sound) noexcept
{
    static constexpr const char* files[soundCount] = { "Metronome.wav", "Metronome2.wav", "Metronome3.wav", "Metronome4.wav", "Metronome5.wav" };
    return files[std::clamp (sound, 0, soundCount - 1)];
}

inline int clampSound (int sound) noexcept { return sound >= 0 && sound < soundCount ? sound : defaultSound; }

// ui-options.txt before round 165z-a2 kept "metronomeSound" as 0 LYNX click, 1 hi-hat, 2 tick, 3 beep, 4 cowbell,
// 5 pulse (the default 0). The LYNX click is gone: it becomes FL's default, Tick; the others keep their sound.
inline int soundFromLegacy (int legacy) noexcept
{
    return legacy >= 1 && legacy <= soundCount ? legacy - 1 : defaultSound;
}

// The mixer track the click goes into: the setting (0 = "---", the Master; n = insert n), the Master when that insert
// does not exist in this song.
inline int targetTrack (int setting, int mixerTracks) noexcept
{
    return setting > 0 && setting < mixerTracks ? setting : masterTrack;
}

// One sound: its frames (a mono file has right == left) and their rate.
struct Sample
{
    std::vector<float> left, right;
    double rate = 44100.0;
    bool fromFl = false; // FL's file (else LYNX's fallback)
    int frames() const noexcept { return (int) left.size(); }
};

// LYNX's stand-in for FL's file when FL is not installed: the same length and peak, synthesised.
inline Sample fallbackSound (int sound, double rate = 44100.0)
{
    struct Shape
    {
        double seconds, hz;
        float peak;
    };
    // the length and peak of FL's files: 4612, 657, 3000, 9467 and 67 frames at 44.1 kHz
    static constexpr Shape shapes[soundCount] = { { 4612.0 / 44100.0, 0.0, 0.8f },
                                                  { 657.0 / 44100.0, 2500.0, 1.0f },
                                                  { 3000.0 / 44100.0, 1000.0, 0.645f },
                                                  { 9467.0 / 44100.0, 587.0, 1.0f },
                                                  { 67.0 / 44100.0, 0.0, 0.488f } };
    sound = clampSound (sound);
    const auto& shape = shapes[sound];
    const auto frames = std::max (2, (int) std::lround (shape.seconds * rate));
    Sample s;
    s.rate = rate;
    s.left.resize ((std::size_t) frames);
    const auto twoPi = 6.283185307179586;
    std::uint32_t noise = 0x9e3779b9u;
    float hp1 = 0.0f, hp2 = 0.0f, lastNoise = 0.0f, lastHp = 0.0f;
    for (int i = 0; i < frames; ++i)
    {
        const auto t = (float) i / (float) frames; // 0 → 1 over the sound
        const auto phase = twoPi * shape.hz * (double) i / rate;
        float v = 0.0f;
        switch (sound)
        {
            case 0: // hi-hat: high-passed noise, a fast decay
            {
                noise ^= noise << 13;
                noise ^= noise >> 17;
                noise ^= noise << 5;
                const auto n = (float) (std::int32_t) noise * (1.0f / 2147483648.0f);
                hp1 = 0.35f * (hp1 + n - lastNoise);
                lastNoise = n;
                hp2 = 0.35f * (hp2 + hp1 - lastHp);
                lastHp = hp1;
                v = hp2 * std::exp (-6.0f * t);
                break;
            }
            case 1: // tick: a very short, bright sine
                v = (float) std::sin (phase) * std::exp (-5.0f * t);
                break;
            case 2: // beep: a flat sine with soft edges
                v = (float) std::sin (phase) * std::min ({ 1.0f, t / 0.05f, (1.0f - t) / 0.05f });
                break;
            case 3: // cowbell: two square partials (× 1.44), ringing decay
            {
                const auto a = std::sin (phase) >= 0.0 ? 1.0f : -1.0f;
                const auto b = std::sin (phase * 1.44) >= 0.0 ? 1.0f : -1.0f;
                v = (a + b) * std::exp (-4.0f * t);
                break;
            }
            default: // pulse: one raised-cosine bump
                v = 0.5f * (1.0f - (float) std::cos (twoPi * t));
                break;
        }
        s.left[(std::size_t) i] = v;
    }
    float peak = 0.0f;
    for (const auto v : s.left)
        peak = std::max (peak, std::abs (v));
    if (peak > 0.0f)
        for (auto& v : s.left)
            v *= shape.peak / peak;
    s.right = s.left;
    return s;
}

// One click playing: a beat or a downbeat of a Sample, resampled to the output rate. A new click cuts the one before.
class Voice
{
public:
    void setOutputRate (double rate) noexcept { outputRate = rate > 0.0 ? rate : 44100.0; }

    void trigger (const Sample* s, bool downbeat) noexcept
    {
        sample = s != nullptr && s->frames() > 0 ? s : nullptr;
        position = 0.0;
        if (sample == nullptr)
            return;
        step = sample->rate / outputRate * (downbeat ? downbeatPitch : 1.0);
        gain = downbeat ? downbeatGain : beatGain;
    }

    void stop() noexcept { sample = nullptr; }
    bool playing() const noexcept { return sample != nullptr; }

    // Adds the next frame × `level`; false once the sound has ended.
    bool next (float level, float& l, float& r) noexcept
    {
        if (sample == nullptr)
            return false;
        const auto i = (int) position;
        const auto frames = sample->frames();
        if (i >= frames)
        {
            sample = nullptr;
            return false;
        }
        const auto frac = (float) (position - (double) i);
        const auto at = [frames] (const std::vector<float>& v, int k) { return k < frames ? v[(std::size_t) k] : 0.0f; };
        const auto g = gain * level;
        l += g * (at (sample->left, i) + (at (sample->left, i + 1) - at (sample->left, i)) * frac);
        r += g * (at (sample->right, i) + (at (sample->right, i + 1) - at (sample->right, i)) * frac);
        position += step;
        return true;
    }

private:
    const Sample* sample = nullptr;
    double outputRate = 44100.0, position = 0.0, step = 1.0;
    float gain = beatGain;
};
} // namespace lynx::metronome
