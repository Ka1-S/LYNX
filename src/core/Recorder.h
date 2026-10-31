#pragma once

// Recording of notes played live (MIDI or PC keyboard) into the current pattern.
// Runs on the UI thread; events arrive with the absolute transport position.

#include <cstdint>
#include <map>
#include <vector>

#include "core/Song.h"

namespace lynx
{
struct RecordedEvent
{
    double tick = 0.0; // absolute transport position (may go past the end of the loop)
    int channel = 0;
    int key = 60;
    float velocity = 0.8f;
    bool on = true;
};

// Where an absolute transport position falls within the current pattern: in pattern mode,
// the position within the loop; in song mode, within the current pattern's clip that is
// playing there (or, without a clip, as if it were the pattern loop).
std::int64_t patternPositionForTick (const Song& song, double tick, std::int64_t loopLength);

// --- recording options (docs/WORKFLOW_FL.md P2: recording set) ---------------------------------

enum class RecordMode : std::uint8_t
{
    overdub, // recorded notes are added to what the pattern has
    replace  // a channel's first recorded note of a take clears that channel's notes in the pattern
};

// Commits finished notes of a take into `pattern`: starts quantised to the nearest multiple of `grid`
// (grid <= 1: as played; lengths kept). In replace mode, `clearedChannels` remembers the channels already
// cleared during this take, so later notes of the same take stay.
void commitTake (Pattern& pattern, std::vector<Note> notes, RecordMode mode, std::int64_t grid, std::vector<int>& clearedChannels);

// FL's record menu "Quantize" section (REA round 139, FLEngine FUN_012ca7b0; settings QuantizeNoteStartTime,
// QuantizeNoteEndTime 0/1/2, RecToSS):
//   starts         "Note start time": starts to the nearest multiple of `grid`;
//   end            what happens to the note's end: keepEnd (neither item: the end stays where it was played),
//                  quantize ("Note end time": the end to the nearest grid line, at least one grid long),
//                  keepLength ("Leave note duration": the played length after the moved start);
//   stepSequencer  "Record to step sequencer": starts to the step grid and every note a step note (one step long;
//                  the other items do not apply).
enum class NoteEnd : std::uint8_t { keepEnd, quantize, keepLength };
struct TakeQuantize
{
    std::int64_t grid = 0; // ticks (<= 1: no grid)
    bool starts = true;
    NoteEnd end = NoteEnd::keepLength;
    bool stepSequencer = false;
};
void quantizeTake (std::vector<Note>& notes, const TakeQuantize& q);
void commitTake (Pattern& pattern, std::vector<Note> notes, RecordMode mode, const TakeQuantize& q, std::vector<int>& clearedChannels);

// FL's record menu "Disarm on stop" (round 166e, measured in FL 2026): when playback stops after playing with the
// record switch on, the switch goes off. update() is called with the transport's state; true = switch it off now.
struct RecordOffOnStop
{
    bool recordedWhilePlaying = false;
    bool update (bool playing, bool recording, bool enabled) noexcept
    {
        if (! recording)
        {
            recordedWhilePlaying = false;
            return false;
        }
        if (playing)
        {
            recordedWhilePlaying = true;
            return false;
        }
        const auto stopped = recordedWhilePlaying;
        recordedWhilePlaying = false;
        return stopped && enabled;
    }
};

// FL's "Enable recording markers" (REA round 139, setting EnablePunchInOutMarkers): the Playlist's "Start recording"
// markers open a range and the next "Stop recording" closes it (no stop: to the end; a stop with no start before it:
// from the song start). No such markers = no ranges (everything is recorded).
struct PunchRange
{
    std::int64_t in = 0, out = 0; // [in, out) ticks
};
std::vector<PunchRange> punchRanges (const std::vector<Marker>& markers);
bool inPunchRanges (const std::vector<PunchRange>& ranges, double tick) noexcept;
// The parts of a recorded audio clip [start, start + length) that fall inside the ranges, as (start, length, offset)
// with `offset` the source offset of each part (the clip's own offset added).
struct ClipPart
{
    std::int64_t start = 0, length = 0, offset = 0;
};
std::vector<ClipPart> punchParts (const std::vector<PunchRange>& ranges, std::int64_t start, std::int64_t length, std::int64_t offset);

// Song mode: makes sure a clip of `pattern` plays at `tick`. If none does, one is placed at the start of
// that bar, on the first track that is free there. Returns true when a clip was added.
bool placePatternClipAt (Song& song, int pattern, std::int64_t tick);

// Length of a count-in of `bars` bars at the song position `tick` (bars follow the time signature there).
std::int64_t countInLength (const Song& song, int bars, std::int64_t tick = 0);

// --- automation recording (REA round 143) ---------------------------------------------------------------
// FL's recording filter "Automation" (FLEngine RecMenu: Recfilter1Menu, bit 1 of the setting RecordingFilter2; bits 2
// notes, 4 audio, 8 clips): while recording, the parameters that are moved are written as automation. Song mode: into
// the automation clip of that parameter playing there (a new automation clip when none plays); pattern mode: into the
// current pattern's events (the event editor's lanes). The record menu's Quantize › "Automations"
// (RecQuantizeAutomationsMenu, setting QuantizeAutomations; FUN_012ca680) puts the points on the snap.
struct RecordedValue
{
    double tick = 0.0; // absolute transport position, as RecordedEvent
    float value = 0.0f; // 0..1
};

// The song position of a recorded transport position (song mode loops over the loop region or the whole song).
// `linear`: the song played on past its end (a take recorded in Song mode without a loop region): no wrap.
std::int64_t songTickForRecorded (const Song& song, double tick, bool linear = false);

// One parameter's part of a take. `continuing` = the points follow the previous batch of the same gesture (what lies
// between them is replaced too). Returns the automation written (or -1); `automation` is a hint: the automation this
// take created for the target (its clip grows with the take).
struct AutomationWrite
{
    AutomationTarget target;
    std::string name;           // for a new automation
    int automation = -1;        // in/out: the automation this take writes (song mode)
    std::int64_t lastLocal = -1; // in/out: the last point written (the automation's or the pattern's ticks)
    bool createdByTake = false; // in/out
};
// Song mode.
void commitAutomationPoints (Song& song, AutomationWrite& w, std::vector<RecordedValue> points, std::int64_t grid, bool linear = false);
// While recording goes on (song mode): the take's own clip reaches the playhead at `tick` (it keeps growing, holding
// the last value, until recording stops — FL); `finished`: the take ended, the clip ends at the end of that bar.
void growTakeClip (Song& song, const AutomationWrite& w, std::int64_t tick, bool finished);
// Pattern mode: into `pattern`'s event lane for the target (one event per tick; held until the next one).
void commitEventPoints (Song& song, int pattern, AutomationWrite& w, std::vector<RecordedValue> points, std::int64_t loopLength,
                        std::int64_t grid);
// Removes the points between two of the same value (no change of the curve) in [from, to].
void thinAutomation (std::vector<AutomationPoint>& points, std::int64_t from, std::int64_t to);

class NoteRecorder
{
public:
    // Starts a take. `loopLength` = pattern loop length at the start of the take
    // (does not change while the recorded notes grow the pattern).
    void begin (std::int64_t loopLength);
    bool isActive() const noexcept { return active; }

    // Delivers an event; when it's the "off" of a held note, the finished note goes to finished().
    void add (const Song& song, const RecordedEvent& e);

    // Closes the notes still held at position `tick` (playback stopped) and ends the take.
    void end (const Song& song, double tick);

    // Notes completed since the last call.
    std::vector<Note> takeFinished();

private:
    struct Held
    {
        double tick;
        float velocity;
    };

    void finish (const Song& song, int channel, int key, const Held& held, double endTick);

    bool active = false;
    std::int64_t loop = 0;
    std::map<std::pair<int, int>, Held> held; // (channel, key)
    std::vector<Note> finished;
};
} // namespace lynx
