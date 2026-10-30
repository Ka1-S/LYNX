#pragma once

// FL's "Burn MIDI to › Current pattern / New pattern" (the channel menu; REA_rodada165f-rack §2): what a channel really
// plays in a pattern — its loop repeats, the swing, Misc › Time shift and gate, the arpeggiator and the echo delay (fat
// mode too) — written as Piano Roll notes. The notes are made by the same arpeggiator / echo code the audio thread runs,
// fed offline (one "sample" per tick), so the burnt notes are what playback plays. No JUCE.

#include <vector>

#include "core/Song.h"

namespace lynx::channelburn
{
// What a burn writes out for a channel (and so what is worth resetting afterwards, FL's "Reset after burning to MIDI").
struct Baked
{
    bool arpeggiator = false;
    bool echo = false;
    bool swing = false;  // the song's swing reaches the channel (swing ≠ 0 and its swing mix ≠ 0)
    bool time = false;   // Misc › Time shift or gate
    bool loop = false;   // the channel loops in the pattern (Channel Rack looping)
    bool any() const noexcept { return arpeggiator || echo || swing || time || loop; }
};
Baked whatBurns (const Song& song, int pattern, int channel);

// The notes `channel` plays in one pass of pattern `pattern`, from its start (notes starting at or after the pattern's
// end — late echoes — are left out). Empty when the channel has no notes there.
std::vector<Note> playedNotes (const Song& song, int pattern, int channel);

// Which settings the burn switches off afterwards (FL asks; LYNX adds Time and always turns the channel's loop off).
struct Reset
{
    bool arpeggiator = true;
    bool echo = true;
    bool swing = true;
    bool time = true;
};

// Burns the channel's notes of the current pattern: into that pattern (its notes are replaced) or into a new pattern
// inserted after it and selected (the original stays). Returns false when the channel has no notes to burn.
bool burnMidi (Song& song, int channel, bool toNewPattern, const Reset& reset);
} // namespace lynx::channelburn
