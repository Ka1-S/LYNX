#pragma once

// FL Studio plugins LYNX lacked until round 165g (docs/analise/REA_rodada165g.md), rebuilt clean-room from FL's
// manual and the behaviour decoded from the installation's DLLs (no Image-Line code or data in this repository):
//
//  - Fruity Mute 2 (EffectKind::fruityMute2), exact: [0] Mute — FL's control value, sound passes at ≥ 0.5 and is
//    silenced below (FL's labels: On = sound, Off = muted); [1] Channel — round(value · 3) clamped to 2: L, L + R, R
//    (the side(s) silenced while muted). No ramp: FL switches at the block.
//  - Fruity Big Clock (EffectKind::bigClock): no sound; shows the song position in big digits. [0] Display: Bar:Beat:Tick,
//    Min:Sec:Hundredths or Bar:Step:Tick.
//  - Tuner (EffectKind::tuner): no sound; a chromatic tuner for monophonic input. [0] Reference 400–480 Hz,
//    [1] Reactivity (slow … fast), [2] Bass (one octave lower, slower), [3] Visual (Basic, Dial). The detection is LYNX's
//    own (YIN, core/VocalPitch.h) and runs on the UI thread from the slot's tap: the audio thread only copies the signal.
//  - Fruity HTML NoteBook (EffectKind::htmlNotebook): no sound; shows a page (FL's state: the page's address). LYNX shows
//    local .html files as formatted text and opens web addresses in the user's browser (it never fetches pages itself).
//
// No JUCE here (tests/FlPlugins165Tests.inc).

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx
{
class Effect;
}

namespace lynx::flextra
{
// ------------------------------------------------------------------------------------------------ Fruity Mute 2
namespace mute2
{
enum Param { mute = 0, channel = 1, count = 2 };
enum class Channel { left = 0, both = 1, right = 2 };

bool soundOn (float muteValue) noexcept;        // FL: muted = value < 0.5
Channel channelOf (float value) noexcept;       // FL: round(value / (1/3)), clamped to 2
float channelValue (Channel c) noexcept;        // the value that selects c (c / 3)
std::string paramText (int param, float value);
// FL's state (record 0xD5): i32 version (1), then both controls' raw values (i32 each)
bool fromFl (const std::vector<std::uint8_t>& state, EffectSlot& slot);
std::unique_ptr<Effect> make();
// one block, as FL's plugin processes it (exposed for the tests)
void process (float* left, float* right, int n, float muteValue, float channelValue) noexcept;
} // namespace mute2

// ------------------------------------------------------------------------------------------------ Fruity Big Clock
namespace bigclock
{
enum Param { display = 0, count = 1 };
enum class Display { barBeatTick = 0, minSecHundredths = 1, barStepTick = 2 };
constexpr int displays = 3;
Display displayOf (float value) noexcept;
float displayValue (Display d) noexcept;
const char* displayName (Display d) noexcept; // English, for the menus
std::string paramText (int param, float value);
// The clock's digits at song position `ticks` (LYNX ticks, ppq per quarter note). Ticks are shown in FL's default
// resolution (96 per quarter note): Bar:Beat:Tick → "bar:beat:tick" (tick 0–95 of the beat), Bar:Step:Tick →
// "bar:step:tick" (tick 0–23 of the sixteenth), Min:Sec:Hundredths → "m:ss:cc". Bars follow the song's time signature
// changes in song mode; `beatsPerBar` applies otherwise.
std::string clockText (const Song& song, double ticks, int beatsPerBar, Display d);
} // namespace bigclock

// ------------------------------------------------------------------------------------------------ Tuner
namespace tuner
{
enum Param { reference = 0, reactivity = 1, bass = 2, visual = 3, count = 4 };
enum class Visual { basic = 0, dial = 1 };
double referenceHz (float value) noexcept;   // 400 + 80 · value
float referenceValue (double hz) noexcept;
bool bassOn (float value) noexcept;
Visual visualOf (float value) noexcept;
std::string paramText (int param, float value);
// FL's state: i32 version (1), reference (0…6000 = 400…480 Hz), reactivity (−1 slow … 1 fast), bass (0/1),
// visual (1 = Basic, 2 = Dial)
bool fromFl (const std::vector<std::uint8_t>& state, EffectSlot& slot);

struct Reading
{
    bool valid = false;  // a pitch is shown
    double hz = 0.0;     // detected frequency
    int note = 69;       // nearest MIDI note for the reference
    double cents = 0.0;  // offset from that note, −50 … +50
    float level = 0.0f;  // RMS of the analysed signal
};
const char* noteName (int midiNote) noexcept; // "C", "C#", ... (sharps, as FL's default)
int octaveOf (int midiNote) noexcept;         // C4 = 60

// The tuner's detection: feed it the slot's signal (mono mix), read the smoothed reading. Reactivity: fast follows
// within ~30 ms and detects up to C10; slow averages ~300 ms, ignores brief fluctuations and detects up to E7. Bass
// lowers the range by an octave (F0) and needs ~100 ms per estimate.
class Analyzer
{
public:
    Analyzer();
    ~Analyzer();
    void prepare (double sampleRate);
    void setOptions (double referenceHz, float reactivity, bool bass);
    void push (const float* mono, int n) noexcept;
    Reading reading() const noexcept { return shown; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Reading shown;
};
} // namespace tuner

// ------------------------------------------------------------------------------------------------ Fruity HTML NoteBook
namespace htmlnotebook
{
// FL's state: i32 version (1), then the page's address as a Pascal short string (length byte + bytes)
std::string address (const std::vector<std::uint8_t>& state);
std::vector<std::uint8_t> stateFor (const std::string& address);
bool isWebAddress (const std::string& address);

// A page as LYNX shows it: blocks of text (headings, paragraphs, list items, preformatted) with links
struct Block
{
    enum class Kind { heading1, heading2, heading3, paragraph, listItem, preformatted, rule } kind = Kind::paragraph;
    struct Run
    {
        std::string text;
        bool bold = false, italic = false;
        std::string link; // "" = none
    };
    std::vector<Run> runs;
};
struct Page
{
    std::string title;
    std::vector<Block> blocks;
};
// A small, forgiving HTML reader: scripts and styles skipped, entities decoded, whitespace collapsed (UTF-8 in/out)
Page parse (const std::string& html);
} // namespace htmlnotebook

// FL plugins imported as inert slots / channels (no sound to reproduce here): the import report's note
// ("" = not one of them)
std::string inertPluginNote (const std::string& flPluginName);
} // namespace lynx::flextra
