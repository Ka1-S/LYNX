#pragma once

// Note names as FL's Settings › General › Note names / Black notes (REA round 165f [DFM TMIDIForm: "English",
// "Germanic", "Solfege (do, re, mi, ...)"; "Sharps - #", "Flats - ♭"]). The choice is global (the UI reads it).
// Octave numbers stay FL's: key 60 = C5.

#include <atomic>
#include <string>

namespace lynx::notenames
{
enum class Style
{
    english,
    germanic, // B natural is H, B flat is B
    solfege   // do, re, mi, fa, sol, la, si
};

inline std::atomic<int> currentStyle { 0 };
inline std::atomic<bool> currentFlats { false };

inline Style style() noexcept { return (Style) currentStyle.load (std::memory_order_relaxed); }
inline bool flats() noexcept { return currentFlats.load (std::memory_order_relaxed); }

// The name of pitch class 0..11 (0 = C).
inline std::string pitchName (int pitchClass, Style s, bool useFlats)
{
    static const char* english[2][12] = { { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" },
                                          { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B" } };
    static const char* germanic[2][12] = { { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "B", "H" },
                                           { "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "B", "H" } };
    static const char* solfege[2][12] = { { "Do", "Do#", "Re", "Re#", "Mi", "Fa", "Fa#", "Sol", "Sol#", "La", "La#", "Si" },
                                          { "Do", "Reb", "Re", "Mib", "Mi", "Fa", "Solb", "Sol", "Lab", "La", "Sib", "Si" } };
    const auto pc = ((pitchClass % 12) + 12) % 12;
    const auto f = useFlats ? 1 : 0;
    switch (s)
    {
        case Style::germanic: return germanic[f][pc];
        case Style::solfege: return solfege[f][pc];
        default: return english[f][pc];
    }
}

inline std::string pitchName (int pitchClass) { return pitchName (pitchClass, style(), flats()); }

// "C5" for key 60 (FL's octave numbering), in the current style.
inline std::string keyName (int key) { return pitchName (key) + std::to_string (key / 12); }
} // namespace lynx::notenames
