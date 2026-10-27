#pragma once

// FL Studio's VFX plugins (round 164b, docs/analise/REA_rodada164b.md): note-level processors that sit between the
// notes of a Patcher ("From LYNX" events) or of a channel and the instruments they play. Each one takes voices
// (note on / off with FL's voice properties: velocity, pan, fine pitch, Mod X / Y, release, note colour) and sends
// voices out on one or more outputs, possibly changing them over time (VFX Envelope) or making new ones on the
// beat (VFX Sequencer).
//
// LYNX keeps each plugin's settings in FL's own state format (the .fst / .flp 0xD5 record), so FL's presets and
// projects load as they are; ChannelDef::pluginState holds a small LYNX header (kind, channel output port) and then
// that state. Behaviour rebuilt from FL's plugins (REA, clean-room: our own code).

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lynx
{
struct ChannelDef;

namespace vfx
{
enum class Kind : std::uint8_t
{
    keyMapper,
    colorMapper,
    envelope,
    keyboardSplitter,
    levelScaler,
    sequencer,
    script
};
constexpr int kindCount = 7;
constexpr std::array<Kind, kindCount> allKinds { Kind::colorMapper, Kind::envelope,    Kind::keyMapper, Kind::keyboardSplitter,
                                                 Kind::levelScaler, Kind::script, Kind::sequencer };

// FL's plugin name ("VFX Key Mapper"), which LYNX shows too
const char* flName (Kind kind) noexcept;
std::optional<Kind> kindOfFlName (std::string_view name) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// Voices and events

// FL's voice properties (the note's own, as the Piano roll / the channel give them)
struct Voice
{
    int key = 60;            // 0…127 (MIDI; 60 = C4, FL's C5)
    float velocity = 100.0f / 128.0f; // 0..1
    float pan = 0.0f;        // −1..1
    float cents = 0.0f;      // fine pitch, cents
    float modX = 0.5f, modY = 0.5f; // 0..1, 0.5 = none
    float release = 0.5f;    // release velocity 0..1
    int colour = 0;          // note colour group 0…15 (= MIDI channel)
    bool porta = false;
};

// A voice property a processor changes while the voice sounds (VFX Envelope)
enum class Prop : std::uint8_t
{
    velocity, // 0..1 (the voice's volume)
    pitch,    // cents from the voice's own key
    pan,      // −1..1
    modX,     // 0..1
    modY      // 0..1
};

struct Event
{
    enum class Type : std::uint8_t { on, off, prop };
    Type type = Type::on;
    int offset = 0; // sample within the block
    int output = 0; // output port 0… (inputs: 0)
    int id = 0;     // voice id: an off / prop names the on it belongs to (inputs: the host's key + colour)
    Voice voice;    // on: the whole voice; off / prop: key and colour
    Prop prop = Prop::velocity;
    float value = 0.0f;
};

// The id of a host voice (an input event's id): its key and colour
int inputId (int key, int colour) noexcept;

// Fixed-capacity event list (never allocates on the audio thread; extra events are dropped)
class EventList
{
public:
    static constexpr int capacity = 1024;
    void clear() noexcept { count = 0; }
    void add (const Event& e) noexcept
    {
        if (count < capacity)
            events[(std::size_t) count++] = e;
    }
    int size() const noexcept { return count; }
    Event& operator[] (int i) noexcept { return events[(std::size_t) i]; }
    const Event& operator[] (int i) const noexcept { return events[(std::size_t) i]; }
    void sortByOffset() noexcept; // stable
    void removeFirst (int n) noexcept;

private:
    std::array<Event, capacity> events {};
    int count = 0;
};

struct Transport
{
    double sampleRate = 44100.0;
    double bpm = 140.0;
    double ticks = 0.0; // song position (LYNX ticks, 960 per quarter) at the block's start
    bool playing = false;
};

// ---------------------------------------------------------------------------------------------------------------
// Settings (FL's state, parsed)

struct KeyMapperSettings
{
    int version = 2;
    bool enabled = true;
    int baseKey = 0;     // labels only (0 = C)
    int offset = 0;      // transposition of the input, −12…12
    int multimode = 0;   // 0 all, 1 all (gain-compensated), 2 random, 3 cycle up, 4 cycle down
    int slide = 0;       // pitch slide steps (0 = none)
    // [input pitch class] output notes, relative to the input octave's C (row i → i = no change)
    std::array<std::vector<int>, 12> rows;
    std::vector<std::uint8_t> extra; // fields of newer versions LYNX keeps as they are
    bool operator== (const KeyMapperSettings&) const = default;
};
KeyMapperSettings defaultKeyMapper();
std::optional<KeyMapperSettings> parseKeyMapper (const std::vector<std::uint8_t>& fl);
std::vector<std::uint8_t> writeKeyMapper (const KeyMapperSettings& s);

struct ColorMapperSettings
{
    std::uint32_t header = 0;
    std::array<int, 16> port {};   // [note colour] the voice output 0…15 it goes to
    std::array<int, 16> colour {}; // [note colour] the colour it leaves with
    bool operator== (const ColorMapperSettings&) const = default;
};
ColorMapperSettings defaultColorMapper();
std::optional<ColorMapperSettings> parseColorMapper (const std::vector<std::uint8_t>& fl);
std::vector<std::uint8_t> writeColorMapper (const ColorMapperSettings& s);

// VFX Level Scaler: per voice property (tab) a transfer curve: Center, Multiply, Offset, Tension, Humanize (+ mode)
struct LevelScalerSettings
{
    enum Tab { velocity, release, finePitch, pan, modX, modY, tabCount };
    enum Field { center, multiply, offset, tension, humanize, humanizeMode, fieldCount };
    std::uint32_t version = 1;
    int selectedTab = 0;                 // (the editor's)
    std::array<std::int32_t, 36> p {};   // p[6 · tab + field], FL's units (Center 0…8192, Multiply ±16384 = ×±2, …)
    int& at (int tab, int field) noexcept { return p[(std::size_t) (tab * 6 + field)]; }
    int at (int tab, int field) const noexcept { return p[(std::size_t) (tab * 6 + field)]; }
    bool identity (int tab) const noexcept; // (passes the value untouched)
    bool operator== (const LevelScalerSettings&) const = default;
};
LevelScalerSettings defaultLevelScaler();
void resetLevelScalerTab (LevelScalerSettings& s, int tab);
std::optional<LevelScalerSettings> parseLevelScaler (const std::vector<std::uint8_t>& fl);
std::vector<std::uint8_t> writeLevelScaler (const LevelScalerSettings& s);
// The curve: x in lo…hi → out in lo…hi; `random` 0…1 places it in the humanize range (FL's T(tab, x, lo, hi, rnd))
double levelScale (const LevelScalerSettings& s, int tab, double x, double lo, double hi, double random) noexcept;

// ---------------------------------------------------------------------------------------------------------------
// Presets

// LYNX's own presets of a kind (scales, chords… built from music theory, so they exist without FL Studio)
struct Preset
{
    std::string folder, name;
    std::vector<std::uint8_t> fl;
};
std::vector<Preset> builtInPresets (Kind kind);
// The state in an FL preset file (.fst: the plugin's name and its 0xD5 record) when it is one of `kind`
std::optional<std::vector<std::uint8_t>> stateFromFlPreset (const std::vector<std::uint8_t>& fileBytes, Kind kind);

// ---------------------------------------------------------------------------------------------------------------
// A VFX node / channel

// The default state of a new plugin of that kind (FL's "Default" preset)
std::vector<std::uint8_t> defaultState (Kind kind);
// How many voice outputs it has (FL's Patcher pins 1…N) and how many FL shows by default
int outputCount (Kind kind) noexcept;
int outputsShown (Kind kind) noexcept;
std::string outputName (Kind kind, int output);

// LYNX's channel / node state: "LVFX", the kind, the channel's output MIDI port (a VFX channel in the Channel Rack
// sends output k on MIDI port `port + k`), then FL's state.
struct State
{
    Kind kind = Kind::keyMapper;
    int port = 0;
    std::vector<std::uint8_t> fl;
};
std::optional<State> readState (const std::vector<std::uint8_t>& pluginState);
std::vector<std::uint8_t> writeState (const State& state);
// A channel / node of that kind with its default settings
void makeChannel (ChannelDef& channel, Kind kind);
std::optional<Kind> kindOf (const ChannelDef& channel);

// The processor. Audio thread: process() never allocates. UI thread: configure() hands new settings over.
class Processor
{
public:
    virtual ~Processor() = default;
    virtual Kind kind() const noexcept = 0;
    virtual void prepare (double sampleRate) noexcept { rate = sampleRate; }
    // the held voices end (each sent out as an "off")
    virtual void releaseAll (EventList& out) noexcept = 0;
    // One block of n samples: the input events (sorted by offset) in, the output events out (any order).
    virtual void process (const Event* in, int count, int n, const Transport& transport, EventList& out) noexcept = 0;
    // It holds voices or plays on its own (a sequencer): the host keeps running it while no note arrives
    virtual bool busy() const noexcept { return false; }
    // New FL state (UI thread); taken at the next block. False if it does not parse.
    virtual bool configure (const std::vector<std::uint8_t>& flState) = 0;

protected:
    double rate = 44100.0;
};
std::unique_ptr<Processor> makeProcessor (Kind kind, const std::vector<std::uint8_t>& flState);
} // namespace vfx
} // namespace lynx
