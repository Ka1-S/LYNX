#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/Effects.h"
#include "core/FixedBlock.h"
#include "core/FlPluginDb.h"
#include "core/PluginSubstitutes.h"
#include "core/Instruments.h"
#include "core/MidiControls.h"
#include "core/ModuleWarmup.h"

namespace lynx
{
// FL's "Notify about rendering mode" (wrapper option, on by default): tells the plugin an export is running
// (VST3 kOffline process mode, VST2 offline process level) so it can use its render quality. Re-prepares the plugin
// when the mode changes; call only while it is not processing (offline renders, before/after the render loop).
void setPluginRenderMode (juce::AudioPluginInstance& plugin, bool offline, int blockSize);

// Block length of "Use fixed size buffers" (PluginRef::fixedBuffers): 2 ms as FL's wrapper, or the audio device's
// buffer with "Process maximum size buffers" (flFixedBlockSize; set when the device starts, 512 / 44.1 kHz until
// then). Plugins created afterwards use it.
int fixedPluginBlock (bool maximumSize) noexcept;
void setFixedPluginBlock (int deviceBlock, double sampleRate) noexcept;

// Adapters: a JUCE plugin seen by the core as an Instrument/Effect.
// Notes become MIDI at the start of the next rendered chunk; since the core
// renders in chunks that start exactly at each event, timing is sample-accurate.
// Hosted plugins that crashed during this session (newest last; read from the UI thread): the plugin's name and the
// exception (platform/CrashGuard.h). `count` only grows, so the UI notices a new one by comparing it.
struct PluginCrash
{
    std::string plugin;
    std::uint32_t code = 0;
};
int pluginCrashCount() noexcept;
std::vector<PluginCrash> pluginCrashes();

// FL's wrapper options a hosted plugin follows while it runs (REA round 151; PluginRef has them, saved per plugin)
struct HostedOptions
{
    bool releaseVelocity = true, sendBendRange = false, modXAftertouch = false, allNotesOffOnStop = true, resetOnFlush = true,
         sendLoopPosition = true;
    int bendRange = 12;
    static HostedOptions of (const PluginRef& ref) noexcept
    {
        return { ref.releaseVelocity, ref.sendBendRange, ref.modXAftertouch, ref.allNotesOffOnStop, ref.resetOnFlush, ref.sendLoopPosition, ref.bendRange };
    }
};

class PluginInstrument final : public Instrument
{
public:
    // fixedBlock > 0: FL's "Use fixed size buffers" — the plugin always gets blocks of that length (+ that much latency)
    PluginInstrument (std::unique_ptr<juce::AudioPluginInstance> instance, int maxBlockSize, int fixedBlock = 0, HostedOptions options = {});
    ~PluginInstrument() override;

    juce::AudioPluginInstance& getInstance() noexcept { return *plugin; }

    void prepare (double) noexcept override {} // prepared at creation (off the audio thread)
    void noteOn (int key, float velocity) noexcept override;
    void noteOff (int key) noexcept override;
    void allNotesOff() noexcept override;
    void setNextNoteModulation (float modX, float) noexcept override { nextModX = modX; }
    void setNextNoteRelease (float velocity) noexcept override { nextRelease = velocity; }
    void transportStopped() noexcept override;
    void panic() noexcept override;
    void render (float* left, float* right, int n, float gainLeft, float gainRight) noexcept override;
    void setParameter (int index, float value) noexcept override;
    int latencySamples() const noexcept override
    {
        return crashed.load (std::memory_order_relaxed) ? 0 : plugin->getLatencySamples() + (fixed != nullptr ? fixed->latency() : 0);
    }

    // The plugin crashed inside a call (caught by platform/CrashGuard.h): it is never called again, the slot plays
    // through (effect) or stays silent (instrument) and the UI is told (pluginCrashes()).
    bool hasCrashed() const noexcept { return crashed.load (std::memory_order_relaxed); }
    // The plugin's state for saving (base64, as capturePluginState), or nullopt after a crash (keep the saved one).
    std::optional<std::string> captureState();
    // Sets a parameter from the UI / controllers (setValueNotifyingHost), guarded like the audio calls.
    void setParameterFromUi (int index, float value);
    void setRenderMode (bool offline);
    void resetForExport(); // UI thread, before an export (the audio is not running through it)
    bool usesFixedBlocks() const noexcept { return fixed != nullptr; }
    // The MIDI "parameters" (core/MidiControls.h): CC / aftertouch / pitch bend values last asked for.
    float midiControlValue (int index) const noexcept { return midiControls.value (index); }
    void noteOnMidi (int key, float velocity, int midiChannel) noexcept override;
    void noteOffMidi (int key, int midiChannel) noexcept override;
    void midiMessage (const std::uint8_t* bytes, int size) noexcept override { midi.addEvent (bytes, size, 0); }
    int takeMidiOutput (MidiOutNote* out, int max) noexcept override;

private:
    void failed (std::uint32_t code) noexcept;
    void flushMidiControls() noexcept;
    void collectMidiOutput (const juce::MidiBuffer& buffer) noexcept; // the plugin's MIDI output (wrapper Output port)
    bool producesMidi = false;
    std::array<MidiOutNote, 256> midiOut {};
    int midiOutCount = 0;
    HostedOptions options;
    float nextModX = 0.5f, nextRelease = 0.5f;
    bool bendRangeSent = false;
    std::atomic<bool> resetPending { false };     // panic with "Reset plugin": reset at the next render (audio thread)
    std::array<std::array<std::uint64_t, 2>, 16> heldKeys {}; // per MIDI channel: the notes on (for stop without All Notes Off)
    void hold (int channel, int key, bool on) noexcept;

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    midictl::State midiControls;
    int blockSize;
    std::string name;
    std::atomic<bool> crashed { false };
    std::unique_ptr<FixedBlock> fixed;
    std::vector<float> scratch; // fixed blocks: the delayed output before it is mixed in
    juce::MidiBuffer pending; // fixed blocks: MIDI waiting for the block being collected, at its offsets in it
};

class PluginEffect final : public Effect
{
public:
    // fixedBlock > 0: FL's "Use fixed size buffers" — the plugin always gets blocks of that length (+ that much latency)
    PluginEffect (std::unique_ptr<juce::AudioPluginInstance> instance, int maxBlockSize, int fixedBlock = 0, HostedOptions options = {});
    ~PluginEffect() override;

    juce::AudioPluginInstance& getInstance() noexcept { return *plugin; }

    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double bpm) noexcept override;
    void setParameter (int index, float value) noexcept override;
    int latencySamples() const noexcept override
    {
        return crashed.load (std::memory_order_relaxed) ? 0 : plugin->getLatencySamples() + (fixed != nullptr ? fixed->latency() : 0);
    }

    // The plugin crashed inside a call (caught by platform/CrashGuard.h): it is never called again, the slot plays
    // through (effect) or stays silent (instrument) and the UI is told (pluginCrashes()).
    bool hasCrashed() const noexcept { return crashed.load (std::memory_order_relaxed); }
    // The plugin's state for saving (base64, as capturePluginState), or nullopt after a crash (keep the saved one).
    std::optional<std::string> captureState();
    // Sets a parameter from the UI / controllers (setValueNotifyingHost), guarded like the audio calls.
    void setParameterFromUi (int index, float value);
    void setRenderMode (bool offline);
    void resetForExport(); // UI thread, before an export (the audio is not running through it)
    bool usesFixedBlocks() const noexcept { return fixed != nullptr; }
    float midiControlValue (int index) const noexcept { return midiControls.value (index); }

private:
    void failed (std::uint32_t code) noexcept;
    void flushMidiControls() noexcept;

    std::unique_ptr<juce::AudioPluginInstance> plugin;
    juce::AudioBuffer<float> buffer;
    juce::MidiBuffer midi;
    midictl::State midiControls;
    int blockSize;
    std::string name;
    std::atomic<bool> crashed { false };
    std::unique_ptr<FixedBlock> fixed;
    HostedOptions options;
};

// Plugin formats, known-plugin list (saved in AppData) and instance creation.
// UI thread, except the scan functions (any thread; one scan at a time) and the background scan.
class PluginHost
{
public:
    // persistent = read and write the list in %APPDATA%\LYNX (false in tests).
    explicit PluginHost (bool persistent = true);
    ~PluginHost();

    juce::KnownPluginList& getKnownPlugins() noexcept { return known; }
    juce::AudioPluginFormatManager& getFormats() noexcept { return formats; }

    static juce::File settingsFolder();          // %APPDATA%\LYNX
    // Did this process load any third-party plugin module (instance or in-process scan)? (then it ends without
    // unloading them, platform/CrashGuard.h exitNow)
    static bool pluginsWereLoaded() noexcept;
    static juce::FileSearchPath defaultSearchPath(); // default VST3 folders

    // The folders LYNX scans: the user's list (setup wizard / %APPDATA%\LYNX\plugin-folders.txt), else suggestedFolders().
    juce::FileSearchPath pluginFolders() const;
    void setPluginFolders (const juce::FileSearchPath& folders);
    // The standard VST3/CLAP folders plus the folders FL Studio uses on this PC (its configured VST folders and
    // the folders of the plugins in its plugin database), existing ones only.
    juce::FileSearchPath suggestedFolders();

    // FL Studio's plugin database on this PC (read once, on first use; empty without FL Studio). Thread-safe.
    const FlPluginDb& flPluginDatabase();
    int flPluginDatabaseFiles() { flPluginDatabase(); return flDbFiles; }

    // Scans the folders for plugins. `progress(0..1, file)` returns false to cancel.
    // Each file is examined in a separate process (LYNX.exe --scan-one): a plugin that
    // hangs or crashes the process doesn't take LYNX down; it is blacklisted and skipped
    // in later scans. Incremental: files already listed and unchanged since are skipped.
    int scan (const juce::FileSearchPath& paths, const std::function<bool (float, const juce::String&)>& progress);
    // The same for a given list of plugin files (targeted scan).
    int scanFiles (const juce::StringArray& files, const std::function<bool (float, const juce::String&)>& progress);

    // Background scan of pluginFolders() (or `paths`): never blocks the caller; poll backgroundScan() for progress.
    struct ScanState
    {
        bool running = false;
        float progress = 0.0f;
        juce::String file;  // being examined
        int found = 0;      // new plugins of the last (or current) background scan
        int finished = 0;   // how many background scans finished since start (changes when one ends)
    };
    void startBackgroundScan (const juce::FileSearchPath& paths);
    void startBackgroundScan() { startBackgroundScan (pluginFolders()); }
    ScanState backgroundScan() const;
    void cancelBackgroundScan();

    // Background scan of these files only (Plugin manager: "Try again", "Add plugin file").
    void startBackgroundScanOfFiles (const juce::StringArray& files);

    // Plugin manager: forgets a plugin file (its plugins and its failed-scan mark, so a later scan examines it
    // again), or everything (before a full rescan). UI thread, not while a scan runs.
    void forgetFile (const juce::String& file);
    void forgetAll();

    // Favourite plugins (identifier strings), listed first by the plugin menus and the quick-add palette.
    // Saved in %APPDATA%\LYNX\plugin-favourites.txt. UI thread.
    const std::set<std::string>& favourites() const noexcept { return favouriteIds; }
    void setFavourite (const std::string& id, bool favourite);

    // Names the user gave plugins (FL's renamed Plugin database entries, issue #11; core/Preferences.h): the
    // Browser's Plugin database shows them and the slot / channel menus list them first, picked by their first
    // character. Saved in %APPDATA%\LYNX\plugin-names.txt. UI thread.
    const std::map<std::string, std::string>& pluginNames() const noexcept { return userNames; }
    void setPluginName (const std::string& key, const std::string& name); // "" = the original name again

    // Scan child mode: describes a file's plugins into `output` (XML). Returns the number found.
    static int scanOneFile (const juce::String& formatName, const juce::String& file, const juce::File& output);

    // (the parent waits core/PluginScanPolicy.h's time for each file; the child first reports a VST3's class count)

    // Adds a specific file (e.g. a .vst3 outside the default folders; an .lv2 bundle adds the plugins its manifest lists).
    int addPluginFile (const juce::File& file);

    // Lets the LV2 format find plugins by URI in every LV2 folder LYNX scans (a host that is not `keep` skips this at
    // construction: the sandbox child calls it before opening an LV2 plugin; round 161).
    void findLv2Plugins();

    std::unique_ptr<juce::PluginDescription> find (const juce::String& id) const;

    // Third-party plugins from an imported .flp (ForeignPlugin): finds the same plugin among the
    // scanned ones (VST3 by class ID, CLAP by id; then the file FL Studio's plugin database lists for it; then
    // VST2 by its ID; by name as a fallback) and carries the state over (VST3: JUCE's format; VST2: FL's layout as is).
    // Matched channels become plugin channels; unmatched effect slots are dropped. Returns how many were matched.
    // `matched` gets one "name → file (how)" line per plugin found.
    // `withoutState` gets the plugins found whose saved settings couldn't be carried over (VST2 <-> VST3 swap).
    // Effect slots whose plugin is missing become their LYNX substitute (core/PluginSubstitutes.h) when
    // pluginSubstitutesEnabled(); `substituted` gets one "name -> LYNX effect" line each (they are not in `missing`).
    int adoptFlPlugins (Song& song, juce::StringArray* missing = nullptr, juce::StringArray* matched = nullptr,
                        juce::StringArray* withoutState = nullptr, juce::StringArray* substituted = nullptr);

    // A .lynx song just loaded: substitute slots whose original plugin is now installed go back to it (always: the
    // original is preferred); plugin effect slots whose plugin isn't installed get their LYNX substitute (when
    // pluginSubstitutesEnabled()). Returns how many slots changed; `restored` / `substituted` count each kind.
    int applySubstitutes (Song& song, int* restored = nullptr, int* substituted = nullptr);

    // What a substitute is matched on for a saved reference: its .flp record when it has one, else the format, name
    // and (decoded) state of the LYNX reference.
    static PluginIdentity identityOf (const PluginRef& ref);
    std::optional<juce::PluginDescription> findForeign (const ForeignPlugin& plugin, bool instrument, juce::String* how = nullptr);

    // Plugin files worth a targeted scan so a project's plugins can be found: for each plugin not known yet, the
    // file FL's database or the project names, and same-named VST3/CLAP files in pluginFolders(). Cheap (no loading).
    juce::StringArray filesToScanFor (const std::vector<FlpPluginUse>& uses);

    // Plugins of the song that are known but failed to load ("name: reason"; they play silence / are bypassed).
    // Kept until clearLoadFailures (a new project). Any thread.
    juce::StringArray getLoadFailures() const;
    void clearLoadFailures();

    std::shared_ptr<Instrument> makeInstrument (const PluginRef& ref, double sampleRate, juce::String* error = nullptr);
    std::shared_ptr<Effect> makeEffect (const PluginRef& ref, double sampleRate, juce::String* error = nullptr);

    static constexpr int maxBlockSize = 4096;

    // Opening a project (round 112): its hosted plugins are created one at a time before the rack is built, so a
    // progress window can say which one is loading; makeInstrument/makeEffect then take the ready instance (same id,
    // state and rate) instead of creating another. A failure is kept too, so a broken plugin is tried once. The rate
    // is the one the rack will be built at (also used by the VST3 automation remap, which reuses the same instance).
    // Message thread.
    void setPreloadRate (double sampleRate) noexcept { preloadRate = sampleRate; }
    // `wanted`: how many ready instances of this exact plugin+state the rack needs so far (one the VST3 automation
    // remap already created counts, so it is not created twice)
    void preload (const PluginRef& ref, int wanted = 1);
    void clearPreloaded();
    int preloadedCount() const;
    // Called on the message thread just before a hosted plugin is created (the progress panel names it and repaints).
    std::function<void (const juce::String& name)> onInstantiate;

    // Slow modules started ahead, on a thread of their own (round 121, core/ModuleWarmup.h): a Waves shell's first
    // factory call waits ~10 s for the vendor's service. Call as soon as an open knows its plugins (any thread);
    // the panel then waits for `modulesStarting()` without blocking. Not with the plugin sandbox (the child loads it).
    void startModulesFor (const std::vector<FlpPluginUse>& uses);
    void startModulesFor (const std::vector<PluginRef>& refs);
    static modulewarm::Waiting modulesStarting();
    // "Continue without it": the modules still starting are left out of this open (their slots get substitutes)
    static void skipStartingModules();
    static void newOpen(); // a new project open: forgets the skips
    // (the modules started ahead stay loaded for the session: a process that loaded plugins ends without their
    // teardown, round 111)

    // Targeted scans on open (round 112): a project plugin that one targeted scan could not find is not searched
    // again on every open (it walked the plugin folders and rescanned files each time); a full scan, or a week,
    // forgets this. Files a scan described with no plugin at all are skipped until they change on disk.
    bool recentlyMissed (const FlpPluginUse& use) const;
    void rememberMisses (const std::vector<FlpPluginUse>& uses);
    void forgetMisses();

private:
    std::unique_ptr<juce::AudioPluginInstance> instantiate (const PluginRef& ref, double sampleRate, juce::String* error);
    std::unique_ptr<juce::AudioPluginInstance> createInstance (const PluginRef& ref, double sampleRate, juce::String* error);
    static std::string preloadKey (const PluginRef& ref, double sampleRate);
    struct Preloaded
    {
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::String error;
    };
    std::map<std::string, std::vector<Preloaded>> preloaded;
    mutable std::mutex preloadLock;
    double preloadRate = 44100.0;
    bool emptyScanIsCurrent (const juce::String& file);
    // a plugin described from this file: the same path, or a module inside this bundle folder (round 165i)
    static bool sameModule (const juce::PluginDescription& d, const juce::String& file);
    bool fileStillThere (const juce::String& file); // cached until clearPreloaded (one check per file per open)
    std::map<juce::String, bool> existsCache;
    std::mutex existsLock;
    void rememberEmptyScan (const juce::String& file);
    void save();
    bool scanOne (juce::AudioPluginFormat& format, const juce::String& file);

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList known;
    bool persistent = true;

    std::mutex scanLock, saveLock, flDbLock;
    std::atomic<bool> stopScan { false };
    FlPluginDb flDb;
    int flDbFiles = 0;
    bool flDbLoaded = false;

    std::set<std::string> favouriteIds;
    std::map<std::string, std::string> userNames;

    class BackgroundScan;
    std::unique_ptr<BackgroundScan> background;
    // reads FL's plugin database a few seconds after start, off the message thread: it took 2 s (hundreds of small
    // .fst / .nfo files) the first time Settings opened, and froze the window (issue #13 investigation)
    std::atomic<bool> warmupQuit { false };
    std::thread flDbWarmup;
    mutable std::mutex failuresLock;
    juce::StringArray loadFailures;
    mutable std::mutex stateLock;
    ScanState state;
};

// Settings → "Substitute missing plugins with LYNX equivalents" (default on; %APPDATA%\LYNX\substitutes.txt).
bool pluginSubstitutesEnabled();
void setPluginSubstitutesEnabled (bool enabled);

// createPluginInstance, checked: a VST3 shell that hands out another of its plugins than the one asked for (a Waves
// shell's first instance in a process, round 121) is created once more.
std::unique_ptr<juce::AudioPluginInstance> createRightInstance (juce::AudioPluginFormatManager& formats, const juce::PluginDescription& description,
                                                                double sampleRate, int blockSize, juce::String& error);

// Stereo layout (if the plugin accepts it), rate and block size; calls prepareToPlay.
void preparePlugin (juce::AudioPluginInstance& plugin, double sampleRate, int blockSize);

// Current plugin state as base64 (for saving in the project).
std::string capturePluginState (juce::AudioPluginInstance& plugin);
} // namespace lynx
