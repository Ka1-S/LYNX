#pragma once

// Clip properties and arrangement management (FL's "Clip properties" and "Merge arrangement" windows, interpreted;
// REA_rodada84). Plain edits on a Song, each run inside AudioEngine::editSong (one undo step). No JUCE.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::clipprops
{
// What the Clip properties window shows and edits. Name and colour belong to the clip's source (the pattern, the audio
// clip's channel or the automation), so they change every clip of that source; pitch and stretch are the audio
// channel's (every clip of that file).
struct Props
{
    ClipKind kind = ClipKind::pattern;
    std::string name;
    std::uint32_t colour = 0; // 0 = automatic
    int source = -1;          // pattern / audio channel / automation index
    int track = 0;
    std::int64_t start = 0, length = 0, offset = 0;
    float gain = 1.0f;        // audio
    float pan = 0.0f;         // audio: -1..1
    std::int64_t fadeIn = 0, fadeOut = 0; // audio
    bool muted = false;
    float pitch = 0.0f;       // audio: semitones (-24..24)
    float stretchBeats = 0.0f; // audio: 0 = the file's own speed
    bool keepPitch = false;

    bool operator== (const Props&) const = default;
};

// The clip's properties; false when `clip` does not exist.
bool read (const Song& song, int clip, Props& out);

// Applies `p` to the clip (start/length/offset are clamped: length ≥ 1 tick, offset ≥ 0, start ≥ 0; fades fit in
// the clip). A source that does not exist, or of another kind, is ignored. False when nothing could be applied.
bool apply (Song& song, int clip, const Props& p);

// For several clips at once (FL's "Mute all / Unmute all / Flip state"):
enum class MuteChange
{
    keep,
    muteAll,
    unmuteAll,
    flip
};
void changeMute (Song& song, const std::vector<int>& clips, MuteChange change);
// Audio clips' gain: set to `value`, or multiply (`relative`).
void changeGain (Song& song, const std::vector<int>& clips, float value, bool relative);

// Positions and lengths as the window shows them, "bar:beat:tick" (ticks at LYNX's PPQ); a position counts from 1 ("1:1:000" is the start), a length from 0 ("2:0:000" = two bars). Time signature
// changes from markers are not applied (bars of `beatsPerBar` beats).
std::string formatPosition (std::int64_t ticks, int beatsPerBar);
std::string formatLength (std::int64_t ticks, int beatsPerBar);
// The inverse; also accepts "bar" or "bar:beat". -1 when the text is not a time.
std::int64_t parsePosition (const std::string& text, int beatsPerBar);
std::int64_t parseLength (const std::string& text, int beatsPerBar);

// Sources a clip of this kind can switch to: their names (patterns / audio channels / automations).
std::vector<std::pair<int, std::string>> sourcesFor (const Song& song, ClipKind kind);
} // namespace lynx::clipprops

namespace lynx::arrangeops
{
enum class MergeMode
{
    insert,  // the source's clips go in at the position; what follows moves later by the source's length
    merge,   // laid over what is there
    replace  // what is there over the source's length is removed first
};

enum class MergePosition
{
    start,
    current, // the playback position
    end      // after the last clip
};

// The clips of arrangement `index` (the playlist for the current one).
const std::vector<Clip>& clipsOf (const Song& song, int index);

// End of the last clip of arrangement `index` (0 when empty).
std::int64_t lengthOf (const Song& song, int index);

// The tick where a merge goes. `currentTick` = the playback position; `bar` rounds the end up to a bar.
std::int64_t positionTick (const Song& song, MergePosition position, std::int64_t currentTick);

// Removes what the current arrangement has in [from, to) (clips crossing the edges are cut) — without moving the rest.
void clearRange (std::vector<Clip>& clips, std::int64_t from, std::int64_t to);

// Merges arrangement `source` into the current one at `at`. False when `source` is the current one or missing.
bool merge (Song& song, int source, MergeMode mode, std::int64_t at);

// Moves arrangement `from` to `to` (the current one stays current).
void move (Song& song, int from, int to);

// Renames (an empty name is ignored).
void rename (Song& song, int index, const std::string& name);

// Duplicates arrangement `index` after itself (not selected); returns the new index.
int duplicate (Song& song, int index);
} // namespace lynx::arrangeops

// FL's Playlist track modes and "Consolidate track(s)" (REA round 146a).
namespace lynx::trackmodes
{
// A mixer track nothing uses yet (no channel routed to it, no track linked to it, no effects, nothing sent into it),
// from insert 1 on; a new insert is added when there is none and the mixer has room. -1 when the mixer is full
// (FL: "Can't create new audio or instrument track: no available mixer tracks").
int freeMixerTrack (Song& song);
// "Track mode › Audio track": links the track to a free mixer track (named and coloured after the track). False when
// no mixer track is free.
bool makeAudioTrack (Song& song, int track);
// "Track mode › Instrument track": the track belongs to `channel` (an instrument, not an audio clip or automation
// channel) and to its mixer track (a free one when the channel plays on the Master or on a track already linked).
bool makeInstrumentTrack (Song& song, int track, int channel);
// "Track mode › Unassigned".
void unassign (Song& song, int track);
// The pattern the pencil places on an instrument track (REA round 151 [EXEC FL 2026: drawing on an instrument track
// places a pattern named after its channel, made on the first click, empty and one bar long; every later click
// places the same pattern; its piano roll is that channel's]): the pattern named after the track's channel whose
// notes (if any) are all that channel's, else a new one (the channel's colour). -1 when the track is not an
// instrument track.
int instrumentTrackPattern (Song& song, int track);
// The text FL's hint bar adds for a track header: "(Audio track, linked to mixer track N)" / "(Instrument track)";
// empty for an unassigned track.
std::string describe (const Song& song, int track);
// The mixer track an audio track's record button arms (FL 2026, issue #11: the header of an audio track has "Audio input
// source", "Recording / Monitoring" and "Arm/disarm recording" buttons that act on its linked mixer track; unassigned and
// instrument tracks have none). -1 when `track` is not an audio track linked to an existing insert.
int armableInsert (const Song& song, int track);
// Audio clips on an audio track play through its mixer track: each one's channel is routed there; a channel that
// also plays clips elsewhere gets a copy of its own first (FL: "make this instance unique and then route it to this
// audio track"). Returns how many clips were changed.
int followAudioTracks (Song& song);

// The copy of the song rendered by "Consolidate track(s)": only the (unmuted) clips of `track` play, moved so the render
// starts at `start` (the track's first clip, or 0 with `fromSongStart`); the Master without effects, at unity.
// `musicLength` receives the length of the music from `start` (ticks). False when the track has no clips.
bool makeConsolidateSong (const Song& song, int track, bool fromSongStart, Song& out, std::int64_t& start, std::int64_t& musicLength);
// After the render: a new audio clip channel with `samplePath` (named "<track> (consolidated)", on the track's mixer
// track for an audio track, else the Master) and one clip of `length` ticks at `start` on the track, in place of the
// track's clips. Returns the new channel.
int applyConsolidate (Song& song, int track, const std::string& samplePath, std::int64_t start, std::int64_t length);
} // namespace lynx::trackmodes
