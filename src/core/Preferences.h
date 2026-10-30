#pragma once

// The logic behind the Settings window's General, Audio and MIDI pages and the Plugin manager (FL Studio's
// Settings pages and plugin manager, interpreted). No JUCE, so it can be tested in isolation: the UI reads and
// writes the files and devices, these functions decide what the settings mean.

#include <map>
#include <set>
#include <string>
#include <vector>

namespace lynx::prefs
{
// --- "key=value" settings files (ui-options.txt, midi-settings.txt) -------------------------------------------------
// Lines "key=value" (spaces around both trimmed); blank lines and lines without '=' are ignored, the last of a
// repeated key wins.
std::map<std::string, std::string> parseKeyValues (const std::string& text);

// --- autosave (General page) ------------------------------------------------------------------------------------
// The choices for "Save a backup copy every": 0 = never, else minutes.
const std::vector<int>& autosaveIntervals();
int clampAutosaveMinutes (int minutes); // the nearest choice
constexpr int defaultAutosaveMinutes = 2;
constexpr int defaultAutosaveKeep = 10;
int clampAutosaveKeep (int keep); // 1..100

// Backup file name (without extension) of project `stem` saved at a date/time: "Night Drive (2026-10-16 19.50.03)".
// Sorting these names sorts them by time.
std::string backupName (const std::string& stem, int year, int month, int day, int hour, int minute, int second);
// True when `name` (without extension) is a backup of `stem` made by backupName, or the plain "stem" of older versions.
bool isBackupOf (const std::string& name, const std::string& stem);
// The backups of `stem` among `names` that go when only the `keep` newest are kept (oldest first).
std::vector<std::string> backupsToRemove (const std::vector<std::string>& names, const std::string& stem, int keep);

// --- undo levels (General page) ---------------------------------------------------------------------------------
constexpr int defaultUndoLevels = 100;
int clampUndoLevels (int levels); // 10..1000

// --- audio (Audio page) -----------------------------------------------------------------------------------------
// The latency one buffer adds, in milliseconds.
double bufferLatencyMs (int bufferSamples, double sampleRate);

// The buffer a fresh install starts with (REA round 87): about 10.7 ms (512 samples at 44.1/48 kHz, 1024 at 88.2/96 kHz)
// so heavy projects do not crackle out of the box. The smallest offered size at or above that; the current size if it
// is already larger; the largest offered one if none reaches it. 0 when nothing is offered.
int safeBufferSize (const std::vector<int>& available, int current, double sampleRate);

// What kind of driver a JUCE device type name is (REA round 97). Windows offers three WASAPI modes:
//  shared        - other programs keep their sound; Windows mixes every stream at its engine period (10 ms on most
//                  devices), so a smaller buffer cannot lower the latency, it only adds bursts;
//  lowLatency    - shared, but asks Windows for a shorter engine period (IAudioClient3); only drivers that support it
//                  go below 10 ms (many USB class-driver devices stay at 480 samples);
//  exclusive     - LYNX alone on the device (other programs go silent); the device's own minimum period (often 3 ms).
enum class DriverKind
{
    wasapiShared,
    wasapiLowLatency,
    wasapiExclusive,
    directSound,
    asio,
    lynxAsio, // LYNX ASIO loaded by LYNX itself, without registration (REA round 106)
    coreAudio, // macOS
    alsa,      // Linux
    jack,      // Linux (JACK, or PipeWire through pipewire-jack)
    pipewire,  // Linux: LYNX's native PipeWire client (round 161)
    other
};
DriverKind driverKind (const std::string& typeName);

// The buffer sizes worth offering for a device of that kind. In classic shared mode the sizes below the engine period
// `devicePeriod` (the device's default buffer) are dropped: Windows still mixes in whole periods, so they would not
// lower the latency and only risk clicks. Other kinds (low-latency mode lists real engine periods): `offered` unchanged.
std::vector<int> usableBufferSizes (DriverKind kind, const std::vector<int>& offered, int devicePeriod);

// After dropouts on a Windows shared-mode or DirectSound device (round 160): the ASIO driver worth suggesting, from the
// ASIO type's device names. The interface's own driver first (e.g. "SSL ASIO Driver 1"), then LYNX ASIO, then any;
// wrappers (ASIO4ALL, FL Studio ASIO, Generic Low Latency) only when nothing else is there. Empty when the current
// type already is ASIO / exclusive / not Windows, or no ASIO driver exists.
std::string suggestedAsioDriver (const std::string& currentTypeName, const std::vector<std::string>& asioDevices);

// --- Linux audio (round 161) ------------------------------------------------------------------------------------
// The driver type a first start opens on Linux, from the types with whether they found devices: PipeWire (LYNX's own
// client), then JACK (a running server), then ALSA, then whatever has devices. Empty when nothing has devices.
std::string preferredLinuxDriver (const std::vector<std::pair<std::string, bool>>& typesWithDevices);

// PipeWire's node.latency request for a buffer: "256/48000".
std::string pipewireLatency (int frames, int sampleRate);

// A readable channel name for a PipeWire audio position: "FL" -> "Left", "FR" -> "Right", "MONO" -> "Mono",
// "AUX3" -> "Aux 4", "RL" -> "Rear left"...; unknown positions stay as they are; empty -> "Channel <index + 1>".
std::string pipewireChannelName (const std::string& position, int index);

// The node name in a "default.audio.sink"-style metadata value ({"name":"alsa_output.pci-0000_00_1f.3.analog-stereo"}).
// Empty when there is none.
std::string pipewireDefaultName (const std::string& json);

// Device names as the device list shows them: each description once; a repeated one gets " (2)", " (3)"...
std::vector<std::string> uniqueDeviceNames (const std::vector<std::string>& descriptions);

// --- MIDI (MIDI page) -------------------------------------------------------------------------------------------
// How a played note's velocity reaches the instrument.
enum class VelocityCurve
{
    linear, // as played
    soft,   // light playing sounds louder
    hard,   // needs harder playing
    fixed   // every note at full velocity
};
float mapVelocity (float velocity, VelocityCurve curve) noexcept; // 0..1 -> 0..1 (0 stays 0: a note-off)

struct MidiInputPrefs
{
    std::string name;     // the device name (as the system shows it)
    bool enabled = true;  // new devices are on
    int midiChannel = 0;  // 0 = any (omni), 1..16 = only that channel
    int target = -1;      // -1 = the selected channels of the Channel Rack, else that Channel Rack channel (0-based)

    bool operator== (const MidiInputPrefs&) const = default;
};

// A MIDI controller link kept in the settings, for every project (FL's "global" links of the Mapping\Generic
// folder, made with a parameter's "Override global link...", form TMIDIInputForm_Generic "Global link settings";
// REA round 146). A project's own link of the same target or controller wins.
struct GlobalLink
{
    int cc = 0, channel = -1;                // channel -1 = any
    int kind = 0, index = 0, slot = 0, param = 0; // the AutomationTarget (kind = its enum value)
    bool operator== (const GlobalLink&) const = default;
};

// An output device (FL's Options › MIDI "Output" panel, REA round 165f): master sync and all-notes-off.
struct MidiOutputPrefs
{
    std::string name;          // the device name
    bool masterSync = false;   // "Send master sync"
    std::string syncType = "clock"; // midisync::syncTypeKey: MIDI clock + SPP, or an MTC rate
    int offsetMs = 0;          // "Master sync offset (milliseconds)", -1000…1000 (later = positive)
    bool allNotesOff = false;  // "Send all notes off" on init and after an export

    bool operator== (const MidiOutputPrefs&) const = default;
};

struct MidiPrefs
{
    std::vector<MidiInputPrefs> inputs; // the devices the user changed (others use the defaults)
    std::vector<MidiOutputPrefs> outputs; // the output devices the user changed
    std::string clockInput;             // "External clock sync": the input LYNX follows ("" = off)
    int clockOffsetMs = 0;              // "External sync offset (milliseconds)", -1000…1000
    std::vector<GlobalLink> globalLinks;
    VelocityCurve velocity = VelocityCurve::linear;
    bool pickup = false;                // a controller takes over a parameter only once it reaches its value
    // FL's Options › MIDI "Song marker jump (MIDI channel)": a note on this channel (1..16; 0 = off) jumps the song to
    // a marker (note 0 = the first marker...) instead of playing
    int songMarkerJumpChannel = 0;
    // FL's "MIDI channel used for playlist live mode" (round 165m): notes on this channel (1…16, 0 = off) launch the
    // Playlist's performance clips while Performance mode is on (core/Performance.h, perf::MidiMapping)
    int performanceChannel = 0;

    // The settings of a device (the defaults when it was never changed).
    MidiInputPrefs input (const std::string& name) const;
    void setInput (const MidiInputPrefs& input);
    MidiOutputPrefs output (const std::string& name) const;
    void setOutput (const MidiOutputPrefs& output);

    // Should a message on MIDI channel `channel` (1..16) from `device` be played?
    bool accepts (const std::string& device, int channel) const;

    std::string toText() const;
    static MidiPrefs fromText (const std::string& text);

    bool operator== (const MidiPrefs&) const = default;
};

// What to do with the MIDI input devices at a periodic check (issue #4: a controller that was not ready when LYNX
// started, or that came back after a reconnection, stayed silent until it was turned off and on in Settings).
struct MidiPort
{
    std::string id;   // the system's identifier
    std::string name; // the name MidiPrefs uses
};

struct MidiInputPlan
{
    std::vector<std::string> open;   // wanted and not open (it appeared, or opening failed before): open it
    std::vector<std::string> reopen; // open, but it disappeared since the last check and came back: close and open again
    std::vector<std::string> close;  // open but turned off in the settings
    bool operator== (const MidiInputPlan&) const = default;
};

// `available` = the devices now; `previous` = the identifiers available at the last check; `openIds` = the ones open.
MidiInputPlan planMidiInputs (const MidiPrefs& prefs, const std::vector<MidiPort>& available,
                              const std::vector<std::string>& previous, const std::vector<std::string>& openIds);

// Takeover ("pickup") of a parameter by a hardware controller: the controller changes the parameter only once its
// value reaches the parameter's (moving past it in either direction, or landing within `tolerance`), so a knob that
// sits elsewhere doesn't make the parameter jump. `previous` is the controller's last value (< 0 = none yet).
bool pickupReached (float parameter, float previous, float incoming, float tolerance = 1.5f / 127.0f) noexcept;

// One controller-to-parameter link under takeover: once caught, the controller keeps the parameter until something
// else moves the parameter away from what the controller set (then it has to reach it again).
struct PickupState
{
    bool caught = false;
    float lastController = -1.0f; // the controller's last value
    float lastSet = -1.0f;        // what the controller last set the parameter to
};
// Returns whether `incoming` should be applied to a parameter now at `parameter`.
bool pickupUpdate (PickupState& state, float parameter, float incoming, float tolerance = 1.5f / 127.0f) noexcept;

// --- plugin manager ---------------------------------------------------------------------------------------------
enum class PluginStatus
{
    ok,
    failed,     // the scan couldn't load it (it crashed, hung or isn't a plugin): skipped by later scans
    missingFile // listed, but its file is gone
};

struct PluginRow
{
    std::string name, vendor, format, category, file, id;
    bool instrument = false;
    PluginStatus status = PluginStatus::ok;
    bool favourite = false;
};

enum class PluginFilter { all, instruments, effects, favourites, failed };
enum class PluginSort { name, vendor, format, type, status };

// The rows to show (indices into `rows`): those passing `filter` whose name, vendor, format, category or file
// contain every word of `query` (case-insensitive), sorted by `sort` (then by name).
std::vector<int> visiblePlugins (const std::vector<PluginRow>& rows, const std::string& query, PluginFilter filter,
                                 PluginSort sort, bool descending = false);

// Favourite plugins: one identifier per line.
std::set<std::string> parseFavourites (const std::string& text);
std::string formatFavourites (const std::set<std::string>& ids);

// Plugin names the user gave (FL's renamed Plugin database entries, issue #11): "key<TAB>name" per line. The key is
// the plugin's identifier string, or "lynx:fx:<name>" / "lynx:gen:<name>" for LYNX's built-in ones. A name that
// starts with a character no other entry starts with ("1_Limiter") is picked by typing it in the slot menus.
std::map<std::string, std::string> parsePluginNames (const std::string& text);
std::string formatPluginNames (const std::map<std::string, std::string>& names);

// Type-ahead in a menu of `labels` (FL's menus): the one label starting with `typed` (case-insensitive, a renamed
// plugin's "1_" digit or letter); -1 when none or several do.
int uniqueTypeAhead (const std::vector<std::string>& labels, char typed);
} // namespace lynx::prefs
