#pragma once

#include <array>
#include <optional>
#include <deque>
#include <map>
#include <tuple>
#include <utility>
#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>

#include "core/AutomationDisplay.h"
#include "core/HeldNotes.h"
#include "core/Preferences.h"
#include "core/LiveEventQueue.h"
#include "core/LiveInput.h"
#include "core/HardwareOutput.h"
#include "core/Recorder.h"
#include "core/TakeRecording.h"
#include "core/WorkerPool.h"
#include "core/SerialJobs.h"
#include "engine/MidiSyncIo.h"
#include "core/RackRenderer.h"
#include "core/RtStats.h"
#include "core/ScoreLog.h"
#include "core/FlpImport.h"
#include "core/History.h"
#include "core/NativeEngines.h"
#include "core/PluginWarmup.h"
#include "core/SnapshotExchange.h"
#include "core/Song.h"
#include "engine/PluginHost.h"
#include "engine/SampleLibrary.h"
#include "engine/StreamPrefetcher.h"
#include "engine/Transport.h"

namespace lynx
{
class AudioLogger;

// Owns the audio device, the processing graph and the document (Song).
// Sits between the driver and the AudioProcessorPlayer to advance the transport
// after each block, so every node sees the same start position.
//
// The Song is touched only by the UI thread; each edit publishes an immutable
// snapshot (RackState) to the audio thread.
// Folders of the most recent FL Studio installation (empty if none).
FlPathRoots findFlStudioRoots();
// The user's FL installation as the native FL engines read it (round 142): its folders and this PC's file system
const natives::FlAccess& flStudioAccess(); // (found once per session)

// Start-up trace hook (round 165i): Main's LYNX_TRACE_STARTUP log receives the engine's own steps (null: off)
inline void (*startupMark) (const char* step) = nullptr;

class AudioEngine final : private juce::AudioIODeviceCallback,
                          private juce::MidiInputCallback,
                          public juce::ChangeBroadcaster,
                          private juce::Timer
{
public:
    explicit AudioEngine (bool openAudioDevice = true);
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    Transport& getTransport() noexcept { return transport; }
    const Transport& getTransport() const noexcept { return transport; }
    SampleLibrary& getSampleLibrary() noexcept { return samples; }
    MeterBank& getMeters() noexcept { return meters; }
    // Tools > Audio logger (REA round 153): records everything the Master plays to a WAV in the background.
    AudioLogger& getAudioLogger();
    PluginHost& getPluginHost() noexcept { return plugins; }

    // Third-party plugins from the last imported .flp that were not found among the scanned ones.
    const juce::StringArray& getMissingPlugins() const noexcept { return missingPlugins; }
    // "plugin → file (how it was found)" for the plugins of the last imported .flp
    const juce::StringArray& getMatchedPlugins() const noexcept { return matchedPlugins; }
    // plugins found whose saved settings could not be carried over (VST2 <-> VST3): "name (format)"
    const juce::StringArray& getPluginsWithoutState() const noexcept { return pluginsWithoutState; }
    // What the last imported .flp converted, approximated or left out.
    const FlpImportReport& getImportReport() const noexcept { return importReport; }
    // The last imported .flp's plugins that were not found, with what uses them and what LYNX put in their place.
    const std::vector<MissingPlugin>& getMissingPluginDetails() const noexcept { return missingPluginDetails; }

    // Live instances (to open the plugin window). nullptr if not a plugin.
    std::shared_ptr<Instrument> getChannelInstrument (int channel) const;
    std::shared_ptr<Effect> getEffect (int track, int slot) const;

    // Copies the plugins' current state into the Song (before saving).
    void capturePluginStates();
    // Offline render mode for the hosted plugins (setNonRealtime), around renderOffline.
    void setPluginsRenderMode (bool offline);
    // Every hosted plugin of the song that has not crashed (channels, inserts, inside Patchers); `notifyRender` is the
    // plugin's "Notify about rendering mode" option. Message thread or the render thread, while nothing else edits.
    void forEachHostedPlugin (const std::function<void (juce::AudioPluginInstance&, bool notifyRender)>& visit) const;
    // The render's plugin warm-up (renderOffline, round 155): silent discarded blocks until the hosted plugins settle,
    // then a new PDC plan if a latency changed and LYNX's own units reset. Returns blocks == 0 when the song has no
    // hosted plugin. `latency` receives the delay compensation's total afterwards.
    warmup::Result warmUpPlugins (const warmup::Settings& settings, double sampleRate, int blockSize,
                                  const std::function<bool (double)>& progress, int& latency);

    void play() noexcept;

    // Performance mode (round 165m, core/Performance.h): launch / release / stop zone clips (message thread). The
    // clips start on the next block, quantized by their track's Performance settings; nothing launches while stopped.
    void performanceCommand (const perf::Command& command) noexcept { performanceLive->commands.push (command); }
    const perf::Live& performanceState() const noexcept { return *performanceLive; }
    perf::Live& performanceStateForSnapshot() noexcept { return *performanceLive; } // --perfshot: a sample of what plays
    void stop() noexcept; // also cancels a count-in or a wait for the first note

    // Recording options (docs/WORKFLOW_FL.md P2: recording set). UI thread.
    struct RecordOptions
    {
        int countInBars = 0;            // 0 = off
        bool waitForInput = false;      // playback starts at the first played note
        RecordMode mode = RecordMode::overdub;
        std::int64_t quantise = 0;      // record quantise in ticks (<= 1: as played); follows the global grid
        bool placeSongClip = true;      // song mode: a take puts a clip of the pattern where recording started
        // recording filter (FL's record button right-click, REA_rodada88): what a take records
        bool recordNotes = true;        // notes played (MIDI and PC keyboard)
        bool recordAudio = true;        // audio of the armed inserts' inputs
        bool askWhatToRecord = true;    // the record button asks first ("What would you like to record?")
        // FL's record menu "Options" (REA round 134): arming recording also starts playback; the armed inserts are
        // disarmed when a take stops
        bool startsPlayback = false;
        bool disarmOnStop = false;
        // FL's record menu "Quantize" section (REA round 139); `quantise` is the grid. quantiseStarts = "Note start
        // time", noteEnd = "Note end time" (quantize) / "Leave note duration" (keepLength) / neither (keepEnd),
        // toStepSequencer = "Record to step sequencer"
        bool quantiseStarts = true;
        NoteEnd noteEnd = NoteEnd::keepLength;
        bool toStepSequencer = false;
        // FL's "Enable recording markers": the Playlist's "Start recording" / "Stop recording" markers limit what a
        // take keeps in song mode (notes started outside are dropped, audio clips are cut to the ranges)
        bool recordingMarkers = false;
        // FL's recording filter "Automation" (REA round 143): moved parameters become automation (song mode: automation
        // clips; pattern mode: the pattern's events); Quantize › "Automations" puts their points on the snap
        bool recordAutomation = true;
        // FL's recording filter "Clips" (REA round 146 B5, round 165m): Performance mode's launched clips are written
        // into the arrangement where they played
        bool recordClips = true;
        bool quantiseAutomation = false;
        // round 163 (Pro Tools / Nuendo): punch in/out on the Playlist's loop region (instead of loop recording; the
        // song plays straight through it), pre-roll before the punch-in (or before the position recording starts at)
        // and post-roll after the punch-out (then playback stops). Bars; 0 = off.
        bool punchLoopRegion = false;
        int preRollBars = 0;
        int postRollBars = 0;
    };
    // The range this recording keeps (pre-roll or punch), while one is active. UI thread.
    std::optional<PunchRange> getActivePunch() const noexcept { return activePunch; }
    const RecordOptions& getRecordOptions() const noexcept { return recordOptions; }
    void setRecordOptions (const RecordOptions& options);

    // Play pressed: with recording on, honours wait-for-input and the count-in; otherwise play().
    void startPlayback();
    // The song's initial values (FL's initialized controls) back on their targets, in song mode; play() and a
    // seek while playing do it (REA round 146). UI thread.
    void resetToInitialValues() noexcept;
    bool isWaitingForInput() const noexcept { return waitingForInput.load(); }
    bool isCountingIn() const noexcept { return transport.countIn.load() > 0.0; }

    // Per-insert record arm: each armed insert records its own audio input (Mixer › insert › input source) into
    // its own take (arming one turns audio recording on; disarming the last one turns it off). Not saved.
    void setInsertArmed (int track, bool armed);
    bool isInsertArmed (int track) const noexcept;
    const std::vector<int>& getArmedInserts() const noexcept { return armedInserts; }
    void rewind() noexcept;
    void seek (double ticks) noexcept;

    // Preview of an audio file (browser). An invalid/empty file stops the preview.
    void previewFile (const juce::File& file);
    // The Browser's preview volume (0..1.25, 1 = default; core/ValueMath.h previewGain). Applies to the preview
    // playing now too. UI thread.
    void setPreviewVolume (double volume) noexcept;
    double getPreviewVolume() const noexcept { return previewVolume; }
    // Plays a sound made in memory through the same preview output (the start-up sound). UI thread.
    void previewSample (std::shared_ptr<const SampleData> sample, bool loop = false, float gain = 0.8f, bool audioEditor = false);
    // The Audio Editor's playback volume (its toolbar knob, round 162): 0..1.25 like the Browser's, 1 = unity.
    void setEditorVolume (double volume) noexcept;
    double getEditorVolume() const noexcept { return editorVolume; }
    // The frame of the previewed sample being played, -1 when the preview is silent.
    double getPreviewPosition() const noexcept;

    // Note played live (PC keyboard, preview). UI thread.
    // `record` = goes into the recording if recording is on (Piano Roll preview doesn't).
    void sendLiveNote (int channel, int key, float velocity, bool on, bool record = false);
    // The keys held right now on MIDI inputs and the typing piano (as played, before the scale snap), ascending; for
    // the Piano Roll's chord readout (any thread).
    std::vector<int> heldInputKeys() const;

    // FL's score logger (REA round 165f): every note played on a MIDI input or the typing keyboard, recording or
    // not, for Tools › Dump score log to selected pattern. Any thread (a short spin lock; never the audio thread).
    std::vector<scorelog::Entry> scoreLogSnapshot() const;
    bool scoreLogEmpty() const;
    void clearScoreLog();

    // Scale snap for played notes (MIDI input and the PC keyboard): scale index into notetools::scales()
    // (0 = off) and its root (0..11). Any thread.
    void setInputScale (int scale, int root) noexcept;
    int snapInputKey (int key) const noexcept;

    // All notes off on every channel (double-click on Stop). UI thread.
    void panic();

    // Note recording (MIDI and PC keyboard) into the current pattern while playing. UI thread.
    void setRecording (bool shouldRecord);
    bool isRecording() const noexcept { return transport.recording.load(); }

    // Audio recording from the interface input: with this on, recording (Rec + Play) writes the
    // input to a WAV and, on stop, the take becomes a Playlist clip (latency-compensated).
    // Enabling it opens the device input (only then does Windows show the mic as in use).
    void setAudioRecordingArmed (bool armed);
    bool isAudioRecordingArmed() const noexcept { return audioArmed; }
    juce::String getLastRecordingError() const { return recordingError; }
    // Where takes are written and how they are named (FL's project data folder, REA round 134): `folder` is
    // "<project folder>/Audio/Recorded" for a saved project (the global recordings folder otherwise), files are
    // "<projectStem>_<yyyy-mm-dd hh-mm-ss>_<insert name>.wav". UI thread.
    void setRecordingTarget (const juce::File& folder, const juce::String& projectStem);
    juce::File getRecordingFolder() const { return recordingFolder; }
    static juce::File defaultRecordingFolder();
    // The take files written since LYNX started (or since forgetRecordings), oldest first. UI thread.
    const std::vector<juce::File>& getSessionRecordings() const noexcept { return sessionRecordings; }
    void forgetRecordings (const std::vector<juce::File>& files);

    // --- hardware recording (round 163, docs/analise/REA_rodada163c.md) ---
    // Options › Audio › Recording, saved in <settings>/recording.txt.
    struct RecordingSettings
    {
        takes::SampleFormat format = takes::SampleFormat::pcm24; // 24-bit or 32-bit float WAV; the device's rate
        bool trackTakeNames = true;  // "<Insert>_Take<N>.wav" (off: FL's "<project>_<date time>_<insert>.wav")
        int manualOffset = 0;        // samples added to the compensation (negative: earlier)
        bool useMeasured = true;     // a ping measured for this device setup replaces the driver's figures
        std::map<std::string, int> measured; // setupKey -> round trip (samples)
        int pingOutput = 0, pingInput = 0;   // device channels the ping uses (output pair's first, input)
    };
    const RecordingSettings& getRecordingSettings() const noexcept { return recordingSettings; }
    void setRecordingSettings (const RecordingSettings& settings); // saves
    // The current device setup's key (takes::setupKey) and its latency figures (pdc = the Master's PDC now).
    std::string currentSetupKey() const;
    takes::Latency currentLatency() const;
    // The latency ping: clicks on output channel pair `pingOutput` (and the next), listened for on input channel
    // `pingInput` through a loopback. Needs the transport stopped and the input open. Returns an error (empty: started).
    juce::String startLatencyPing();
    bool isPinging() const noexcept { return ping.isRunning(); }
    void cancelLatencyPing() noexcept { ping.cancel(); }
    // After isPinging() turned false: the result (stored for this setup when ok).
    takes::LatencyPing::Result finishLatencyPing();
    // Record safe (round 163): a safe insert can't be armed (undoable, saved with the project).
    void setInsertRecordSafe (int track, bool safe);
    bool isInsertRecordSafe (int track) const noexcept
    {
        return track > 0 && track < (int) song.mixer.size() && song.mixer[(std::size_t) track].recordSafe;
    }
    // Frames lost while recording (the disk could not keep up; they became silence, the timing kept) since the
    // last take started.
    juce::int64 getLostRecordingFrames() const noexcept { return lostFrames; }
    // Interrupted takes (crash while recording): repaired WAVs and where they belonged. Scans the journals in the
    // given folders (the default recordings folder always). UI thread, at start-up.
    std::vector<takes::Journal> recoverInterruptedTakes (const std::vector<juce::File>& folders);
    // Puts recovered takes on the Playlist at their recorded positions (undoable). Returns how many were placed.
    int placeRecoveredTakes (const std::vector<takes::Journal>& recovered);
    // Leaves them as files only (their journals are removed).
    void forgetInterruptedTakes (const std::vector<takes::Journal>& recovered);

    // --- external audio input through the mixer (REA_rodada88) ---
    // Names of the open device's input channels (all of them, open or not); empty without an input device.
    std::vector<std::string> getInputChannelNames() const;
    // Problems the UI explains to the user (translated there).
    enum class InputIssue
    {
        none,
        noDevice,          // no audio device open at all
        noInputDevice,     // the driver has no input device
        noInputOpen,       // recording with no input open
        armedWithoutInput, // armed inserts, none with an input source
        insertWithoutInput,// one armed insert (track) has no input source
        lowDisk,           // little room left on the recordings drive (detail = minutes)
        recordSafe,        // the insert (track) is record safe: not armed
        framesLost,        // the disk could not keep up: frames became silence (detail = how many)
        failed             // the driver refused (detail = its message) or a file could not be written
    };
    struct RecordingNotice
    {
        InputIssue issue = InputIssue::none;
        int track = 0;
        juce::String detail;
    };
    // Opens the input of the current driver: `deviceName` (separate-input drivers; empty = the one chosen in
    // Settings, else the default), all of its channels. UI thread.
    RecordingNotice openAudioInput (const juce::String& deviceName = {});
    bool hasAudioInput() const;
    // Closes the device input again when nothing needs it (no insert takes an input, no armed recording, no hardware
    // insert returning from it): a capture the Audio editor opened for one recording no longer keeps the input — and
    // Windows' microphone-in-use icon — on until LYNX quits, as in FL (round 165q). The input device's name is kept
    // for the next openAudioInput(). UI thread.
    void releaseAudioInputIfUnused();
    bool audioInputNeeded() const;
    // Sets an insert's input source (undoable; -1 = none) and opens the device input when needed.
    void setInsertInput (int track, int channel, bool stereo);
    void setInsertMonitor (int track, InputMonitor mode);

    // --- hardware output targets (REA round 156) ---
    // Names of the open device's output channels (all of them, open or not); empty without a device.
    std::vector<std::string> getOutputChannelNames() const;
    // Whether device output channel `c` is open (gets sound) right now.
    bool isOutputChannelOpen (int channel) const;
    // Sets a track's hardware output target (undoable; outputNone / outputDefault / a device channel) and opens
    // the device's output channels it needs (no trip to Audio settings). The Master's default is its first pair.
    void setTrackOutput (int track, int channel, OutputMode mode);
    // Opens the device output channels the song's targets need (that the device has). UI thread.
    void openTargetedOutputs();
    // The hardware inserts' channels (round 164d): opens their outputs and, when one returns from an input, the device
    // input. UI thread.
    void openHardwareInsertIo();
    // Every hardware insert of the song (round 165c): a mixer slot's, or a node inside a Patcher (mixer-slot or generator
    // Patcher; `nodes` = the node index at each Patcher level). UI thread.
    struct HardwareInsertRef
    {
        bool channel = false; // a Channel Rack generator Patcher (index = channel), else a mixer slot (index = track)
        int index = 0, slot = 0;
        std::vector<int> nodes;
        bool operator== (const HardwareInsertRef&) const = default;
    };
    std::vector<HardwareInsertRef> getHardwareInserts() const;
    static EffectSlot* locateHardwareInsert (Song& song, const HardwareInsertRef& ref);
    std::shared_ptr<Effect> runningHardwareInsert (const HardwareInsertRef& ref) const;
    // The routed, active inserts whose round trip was measured for another device setup (device, rate or buffer size)
    std::vector<HardwareInsertRef> staleHardwareInserts() const;
    // Presses their Ping buttons (the clicks go out on their sends; the results are applied as they come back)
    void pingHardwareInserts (const std::vector<HardwareInsertRef>& refs);
    // After the device setup changed with stale inserts in the song: those inserts, once (the UI offers "Ping now")
    std::vector<HardwareInsertRef> takeHardwareInsertNotice() { return std::exchange (hwNotice, {}); }
    // Input level of an insert's source since the last call (0..1, per side). UI thread.
    // (the Mixer's strips take it 30 times a second; other views peek at the last value taken)
    float takeInputPeak (int track, int side) noexcept
    {
        const auto v = liveInput.meters.takePeak (track, side);
        if (track >= 0 && track < maxMixerTracks && (side == 0 || side == 1))
            lastInputPeaks[(std::size_t) (track * 2 + side)] = v;
        return v;
    }
    float peekInputPeak (int track, int side) const noexcept
    {
        return track >= 0 && track < maxMixerTracks && (side == 0 || side == 1) ? lastInputPeaks[(std::size_t) (track * 2 + side)] : 0.0f;
    }
    // A message for the user about recording (e.g. an armed insert without an input), once. UI thread.
    RecordingNotice takeRecordingNotice() { return std::exchange (recordingNotice, {}); }

    // MIDI learn: the next CC received becomes a controller linked to `target`.
    void learnMidiController (const AutomationTarget& target);
    bool isLearningMidi() const noexcept { return learning.kind != AutomationTargetKind::none; }
    // Global links (FL's "Override global link...", REA round 146): links in the MIDI settings that every project
    // follows unless it links the same target or controller itself. The next CC received becomes `target`'s.
    void learnGlobalLink (const AutomationTarget& target);
    void removeGlobalLink (const AutomationTarget& target);
    std::optional<MidiLink> globalLinkOf (const AutomationTarget& target) const;
    std::vector<MidiLink> getGlobalLinks() const;

    // FL's "Last tweaked" (REA round 143: FLEngine keeps the last two moved parameters, Tools › Last tweaked /
    // Next to last tweaked, Add › Automation for last tweaked parameter). Every control that can be automated reports
    // its moves here (UI thread); `held` = the touch lasts until tweakReleased (a plugin's own editor's gesture),
    // otherwise while the control keeps moving. While recording with the "Automation" filter, touched parameters are
    // written as automation.
    void noteTweaked (const AutomationTarget& target, bool held = false);
    void tweakReleased (const AutomationTarget& target);
    // 0 = the last tweaked parameter, 1 = the one before it.
    std::optional<AutomationTarget> getLastTweaked (int which = 0) const;
    // The target's current value 0..1 (a hosted plugin's parameter and MIDI controls included); < 0 when it is gone.
    float currentTargetValue (const AutomationTarget& target) const;
    // Parameter `param` of a hosted plugin (a channel's instrument or a slot's effect) as an automation target.
    std::optional<AutomationTarget> targetOfPlugin (const juce::AudioPluginInstance& plugin, int param) const;
    // Number of points recorded as automation since recording started (tests, hint bar).
    int getRecordedAutomationPoints() const noexcept { return recordedAutomationPoints; }
    // Names a new recorded automation (the UI sets it to its readable target names).
    std::function<std::string (const AutomationTarget&)> nameAutomation;

    // MIDI inputs: every device is enabled and plays the selected channels unless the MIDI settings say otherwise
    // (Settings › MIDI: device off, one MIDI channel only, a fixed Channel Rack channel; velocity curve; pickup).
    juce::StringArray getActiveMidiInputs() const;
    // Counts the messages received from the enabled MIDI inputs (the toolbar's MIDI activity light).
    std::uint32_t getMidiActivity() const noexcept { return midiActivity.load (std::memory_order_relaxed); }
    // Closes and opens again every enabled MIDI input (the MIDI light's click; Settings › MIDI › Refresh).
    void reconnectMidiInputs();
    const prefs::MidiPrefs& getMidiPrefs() const noexcept { return midiPrefs; }
    // Applies them (and saves them in %APPDATA%\LYNX\midi-settings.txt when `save`).
    void setMidiPrefs (const prefs::MidiPrefs& prefs, bool save = true);
    // master sync out / external clock in / all notes off (round 165f)
    MidiSyncIo& getMidiSync() noexcept { return midiSync; }

    // How many undo steps are kept (Settings › General).
    void setUndoLevels (int levels);
    int getUndoLevels() const noexcept { return (int) maxUndoSteps; }

    // Document (UI thread)
    const Song& getSong() const noexcept { return song; }
    // The values automation gives the controls at the playhead while the song plays (issue #12: knobs and faders move
    // with their automation, as in FL). Refreshed at most once per UI frame, on the UI thread only; the audio thread
    // does no extra work for it. An editor shows `shown (target, its Song value)` and repaints when the generation
    // changes.
    const AutomationDisplay& getAutomationDisplay();
    // the time the last refresh took (ms), for --perf style measurements
    double getAutomationDisplayMs() const noexcept { return automationDisplayCostMs; }
    void setSong (Song newSong); // new/opened project: clears the history

    // Edits the song and publishes it to audio. `undoable` edits go into the history;
    // all edits made while the same mouse click is held down
    // (dragging a note, turning a knob) become a single step.
    void editSong (const std::function<void (Song&)>& edit, bool undoable = true);

    // Changes since the last save/open (for the "*" in the title and the "save changes?" prompt).
    bool isModified() const noexcept { return modified; }
    void markSaved() noexcept { modified = false; sendChangeMessage(); }
    void markModified() noexcept { modified = true; sendChangeMessage(); } // e.g. a backup was opened in place of the project

    bool canUndo() const noexcept { return ! undoStack.empty(); }
    bool canRedo() const noexcept { return ! redoStack.empty(); }
    void undo();
    void redo();

    // Undo history as a list of states, oldest first: the undo steps, the current state, then the redo
    // steps. Each entry is named after what changed from the previous one (core/History.h).
    int getHistorySize() const noexcept { return (int) (undoStack.size() + 1 + redoStack.size()); }
    int getHistoryPosition() const noexcept { return (int) undoStack.size(); }
    ChangeLabel getHistoryLabel (int index) const; // index 0 = the oldest state kept
    void jumpToHistory (int index);

    // Channel selected in the Channel Rack (target of the Piano Roll and keyboard). UI thread.
    int getSelectedChannel() const noexcept { return selectedChannel; }
    void setSelectedChannel (int channel); // selects only this channel

    // Multi-selection in the Channel Rack (sorted). The selected channels all receive live MIDI input;
    // `primary` (one of them) is the Piano Roll / keyboard target above.
    const std::vector<int>& getSelectedChannels() const noexcept { return selectedChannels; }
    void setChannelSelection (std::vector<int> channels, int primary);

    // Imports a .flp, locating the FL installation to find the samples.
    // Returns false and fills `error` on failure. `relocations` (saved sample path → file to use) are the samples
    // the user located; with `asEdit` the result replaces the song as one undoable step instead of a new project.
    bool importFlpFile (const juce::File& file, juce::String& error, const std::map<std::string, std::string>& relocations = {},
                        bool asEdit = false);

    // The same import in steps, so a window can show progress while it runs (round 112): `prepareFlpImport` reads,
    // parses and imports the project and decodes its samples in parallel — no plugin is touched, so it may run on any
    // thread; `preloadPlugins` creates the project's hosted plugins one at a time (message thread; call it until it
    // returns false); `finishFlpImport` adopts the result as the song (message thread). Phase times go to core/LoadTrace.
    struct PreparedFlp
    {
        bool ok = false;
        juce::String error;
        Song song;
        FlpImportReport report;
        std::vector<std::shared_ptr<const SampleData>> decoded; // kept alive until the rack is built
        std::vector<PluginRef> pluginQueue;                     // hosted plugins to create, in rack order
        std::size_t pluginsDone = 0;
        bool adopted = false;
        juce::StringArray missing, matched, withoutState, substituted;
        struct Use
        {
            std::string name, user;
            bool instrument = false;
        };
        std::vector<Use> uses;
    };
    static void prepareFlpImport (const juce::File& file, const std::map<std::string, std::string>& relocations, SampleLibrary& library,
                                  PreparedFlp& out);
    // one hosted plugin of the queue (message thread); false when the queue is done
    bool preloadNextPlugin (PreparedFlp& prepared);
    // Before importing the open project again: leaves out of `prepared`'s plugin queue those the song already runs
    // with the same settings (the rack keeps them).
    void keepRunningPlugins (PreparedFlp& prepared);
    bool finishFlpImport (PreparedFlp&& prepared, bool asEdit);

    // --- real-time bounce (round 165c: Export › "Render through the hardware in real time") ---
    // The song plays through the audio device from `startTick` (song mode; past its end and through the loop region,
    // without starting over) while the Master's output is recorded: hardware inserts send to and return from their gear
    // as they do while playing. Message thread; returns an error (empty: started).
    juce::String startRealtimeBounce (double startTick);
    // What the Master played since the last call, from the first sample the song played (one consumer thread).
    int pullRealtimeBounce (std::vector<float>& left, std::vector<float>& right) { return meters.bounceTap.pull (left, right); }
    bool isBouncing() const noexcept { return bouncing; }
    // While bouncing: the transport still plays (false: the user stopped it), the delay compensation's total at the
    // start, the device's dropouts and the frames the tap lost since the start.
    bool bounceStillPlaying() const noexcept { return transport.playing.load(); }
    int bounceLatency() const noexcept { return bounceStartLatency; }
    int bounceDropouts() const;
    // Stops playback and puts the transport back as it was (message thread).
    void finishRealtimeBounce();
    // Renders without an audio device. Writes to `output` if not nullptr.
    // After `seconds`, continues for up to `tailSeconds` while there is still sound.
    // `progress(0..1)` returns false to cancel. Returns the peak (used by the self-test),
    // or -1 if cancelled.
    struct RenderOptions
    {
        double seconds = 10.0;
        double sampleRate = 44100.0;
        juce::AudioFormatWriter* output = nullptr;
        std::function<bool (double)> progress; // 0..1; false cancels
        double tailSeconds = 0.0;
        int trimSamples = 0;  // PDC: discards this much from the start and renders the same amount extra at the end
        int ditherBits = 0;   // > 0: TPDF dither (with noise shaping unless ditherShaping is false) for this bit depth
        bool ditherShaping = true;
        // FL's "HQ for all plugins": render qualities and HQ switches on (off: the playback quality)
        bool hqAllPlugins = true;
        // receives every rendered block after the PDC trim (left, right, frames), like `output` does
        std::function<void (const float*, const float*, int)> capture;
        // trim the delay compensation's latency as the plugins report it after the warm-up (adds to trimSamples)
        bool trimPdc = false;
        // Export "Plugin warm-up" (round 155, core/PluginWarmup.h): with hosted plugins in the song, silent blocks are
        // processed and thrown away before the render until the plugins have settled
        bool pluginWarmUp = true;
        warmup::Settings warmUp;
        // false: samples above 0 dBFS are clamped to ±1 before the dither / the file (integer formats, Ogg)
        bool keepOvers = false;
    };

    float renderOffline (const RenderOptions& options);

    // timings of the last renderOffline (diagnostics): the audio graph and the file writing
    struct RenderStats
    {
        double graphMs = 0.0, writeMs = 0.0;
        warmup::Result warmUp; // the plugin warm-up before the render (blocks == 0: none ran)
        int trimmedSamples = 0; // the PDC trim used
    };
    RenderStats getLastRenderStats() const noexcept { return lastRenderStats; }
    float renderOffline (double seconds, double sampleRate, juce::AudioFormatWriter* output = nullptr)
    {
        RenderOptions o;
        o.seconds = seconds;
        o.sampleRate = sampleRate;
        o.output = output;
        return renderOffline (o);
    }

    int getNumGraphConnections() const { return (int) graph.getConnections().size(); }

    // Time spent in each instrument and effect (the Plugin performance monitor); nullptr without the rack node.
    ProcessTimes* getProcessTimes() noexcept;

    // Latest output samples (mono), for the oscilloscope. UI thread.
    void copyScope (float* destination, int count) const noexcept;
    static constexpr int scopeSize = 2048;

    // Real-time health of the audio callback (REA round 87): callbacks over their deadline, late device calls, load.
    const RtStats& getRtStats() const noexcept { return rtStats; }
    RtStats& getRtStats() noexcept { return rtStats; }

    // What takes the most audio CPU: channels and insert effects, as a share of real time over the last ~2 s of
    // playback, heaviest first. UI thread.
    struct UnitLoad
    {
        juce::String name;
        double load = 0.0;      // CPU time / audio time (summed over the worker threads)
        double longestMs = 0.0; // the longest single call in the window
    };
    const std::vector<UnitLoad>& getHeaviestUnits() const noexcept { return heaviestUnits; }

    // How regularly the message thread runs the engine's 30 Hz timer (a saturated UI delays it). UI thread.
    struct UiPulse
    {
        int calls = 0;
        double maxGapMs = 0.0;
    };
    UiPulse takeUiPulse() noexcept { return std::exchange (uiPulse, {}); }

    // Writes `state` (a createStateXml() taken earlier; nullptr = none was saved) back as the saved device and stops
    // saving later changes: a test run (--rtstress --type ...) leaves the user's device as it found it.
    void restoreAudioDeviceState (const juce::XmlElement* state);
    // Device, buffer, deadline and the callback statistics in a few lines (CPU meter hint, glitch log, --rtstress).
    juce::String describeRealtime (bool withUnits = true) const;

    // Settings › Audio "Driver settings…": shows the device's own panel (ASIO) and, when the driver says its settings
    // changed, reopens it with them. The device object is replaced (its driver re-read), so the old pointer is never
    // used after the close (0.1.17 / 0.1.18 called the deleted device: issues #12 and #13), and the buffer size the
    // panel chose is kept (the reopen asks for the driver's preferred size instead of the size LYNX had before, which
    // silently undid an ASIO4ALL / FL ASIO buffer change). Returns true when the device was reopened. Message thread.
    static bool showDriverPanel (juce::AudioDeviceManager& devices);
    // --selftest "driver_panel": showDriverPanel with a fake device whose panel changes its buffer (and one whose panel
    // changes nothing). "OK" or what went wrong.
    static juce::String driverPanelSelfTest();
    // %APPDATA%\LYNX\audio-glitches.log: a line each time callbacks run over their deadline (at most every 2 s).
    static juce::File glitchLogFile();

    // Total latency compensated by PDC (at the Master output), in samples.
    int getLatencySamples() const noexcept { return lastPublishedShape.pdc.total; }
    // The last published PDC plan (per-track latencies: outLat, inAlign). Message thread.
    const PdcPlan& getPdcPlan() const noexcept { return lastPublishedShape.pdc; }

private:
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;
    void timerCallback() override;
    void handleIncomingMidiMessage (juce::MidiInput* source, const juce::MidiMessage& message) override;
    void enableAllMidiInputs();
    void saveAudioDeviceState();
    void updateRecording();
    void updateAudioRecording();
    void updateAutomationRecording (bool active);
    void startAudioTake();
    void finishAudioTake();
    double estimatedPositionNow() const noexcept;
    AutomationDisplay automationDisplay;
    std::uint64_t songStamp = 1;            // + 1 at every publishSong (the display's tables follow the Song)
    double automationDisplayAtMs = -1.0e9;  // when it was last refreshed
    double automationDisplayCostMs = 0.0;

    void buildInitialGraph();
    void choosePreferredDriver(); // a first start on Linux: PipeWire, then JACK, then ALSA (round 161)
    void chooseSafeBufferSize();
    void updateRealtimeStats(); // timer: unit loads over a window, glitch log
    RtStats rtStats;
    std::vector<UnitLoad> heaviestUnits;
    std::vector<std::uint64_t> lastChannelNs, lastEffectNs;
    std::uint64_t lastProcessedSamples = 0;
    double lastUnitsWindowMs = 0.0, lastUiPulseMs = 0.0;
    UiPulse uiPulse;
    int latencyRepublishes = 0;   // PDC re-plans because a unit's latency changed (diagnostics)
    juce::String lastLatencyChange;
    // hardware inserts (round 165c): ping results applied to the slots (whether an editor is open or not), and the device
    // setup they were measured for
    void applyHardwareInsertPings();
    std::map<const void*, int> hwPingSerials; // running insert -> the last ping result seen
    std::string lastSetupKey;
    std::vector<HardwareInsertRef> hwNotice;
    int hwTicks = 0;
    bool bouncing = false;
    int bounceStartLatency = 0, bounceStartXruns = 0;
    double bounceReturnTick = 0.0;
    bool bounceRestoreMode = false;
    bool checkHardwareInserts = false;
    std::uint64_t loggedOver = 0, loggedLate = 0;
    double lastLogMs = 0.0;
    class PreviewPlayer* preview = nullptr; // graph node (owned by the graph)
    double previewVolume = 1.0;
    double editorVolume = 1.0;
    void publishSong();
    void adoptPrepared (PreparedFlp& prepared); // the plugins of a prepared import found / substituted (message thread)

    std::unique_ptr<WorkerPool> workers; // extra renderer threads (before the graph, which uses them)
    juce::AudioDeviceManager deviceManager;
    juce::AudioProcessorGraph graph;
    juce::AudioProcessorPlayer player;
    Transport transport;
    MidiSyncIo midiSync; // (before the MIDI callbacks and the audio callback use it)
    SampleLibrary samples;
    LiveEventQueue liveEvents;   // UI -> audio
    LiveEventQueue midiEvents;   // MIDI thread -> audio
    InputHeldNotes midiHeld;     // MIDI thread: what each held MIDI key started
    std::atomic<std::uint64_t> heldKeyBits[2] {}; // keys held on MIDI inputs / the typing piano (chord readout)
    std::atomic<std::uint8_t> heldKeyCount[128] {};
    void markHeldKey (int key, bool on) noexcept;
    KeyLedger playedKeys;        // UI thread: the keys the typing piano started (after the scale snap)
    SpscQueue<RecordedEvent, 1024> midiRecordQueue; // MIDI thread -> UI (recording)
    // a controller's pitch wheel while recording (MIDI thread -> UI): key = its MIDI channel 0…15, velocity = 0..1;
    // written as the channel's pitch bend automation like a moved knob (issue #12: the bend was heard, not recorded)
    SpscQueue<RecordedEvent, 2048> bendRecordQueue;
    std::map<int, float> liveBends; // UI thread: the last recorded bend of each channel (its "current value")
    void takeRecordedBends();

    struct ControllerEvent
    {
        int cc = 0, channel = 0, value = 0;
    };
    SpscQueue<ControllerEvent, 1024> controllerQueue; // MIDI thread -> UI (controllers)
    // the CCs a MIDI link (project or global) uses: they move their parameter and are not also played into the
    // selected channel's instrument (round 162); written by applyControllers, read by the MIDI thread
    std::array<std::atomic<std::uint64_t>, 2> linkedCcs {};
    AutomationTarget learning;
    bool learningGlobal = false;
    bool saveGlobalLinks = false; // only the engine with an audio device (the app) writes the settings file
    void applyControllers();
    std::vector<RecordedEvent> uiRecorded;           // PC keyboard -> recording
    NoteRecorder recorder;
    // last tweaked parameters and automation recording (REA round 143)
    std::array<AutomationTarget, 2> lastTweaked {};
    struct Touch
    {
        AutomationTarget target;
        double lastMs = 0.0;
        bool held = false;
        std::vector<RecordedValue> pending; // sampled, not yet written
    };
    std::vector<Touch> touches;
    int automationCommitCountdown = 0;
    bool automationTakeHasUndoStep = false;
    std::int64_t automationLoopLength = 0;
    int automationPattern = 0;
    int recordedAutomationPoints = 0;
    bool automationOverrideActive = false; // touched targets override their automation in the renderer
    // Song mode, no loop region: once a parameter moved while recording, the song plays on past its end (no wrap to
    // the start) until recording stops, and the take's clip grows with the playhead (issue #12: it cut itself)
    bool automationLinear = false;
    std::int64_t lastAutomationTick = -1; // the song position of the take's last update (song mode)
    std::vector<AutomationWrite> takeWrites; // per target: where this take writes (its own clip grows)
    bool takeHasUndoStep = false;
    bool perfTakeHasUndoStep = false; // the first clip a performance recording writes is the take's undo step
    void updatePerformanceRecording();
    RecordOptions recordOptions;
    std::vector<int> takeClearedChannels; // replace mode: channels already cleared in this take
    double takeStartPosition = 0.0;
    std::atomic<bool> waitingForInput { false };
    void startOnInput() noexcept; // MIDI or UI thread: the first note while waiting starts playback
    std::vector<int> armedInserts;
    juce::String releasedInputDevice; // the input device releaseAudioInputIfUnused closed (reopened by name)
    double lastPlayingTick = 0.0;
    std::atomic<int> inputScale { 0 }, inputScaleRoot { 0 };
    std::atomic<int> liveChannel { 0 };              // selected channel, read by the MIDI thread
    static constexpr int maxLiveChannels = 16;       // other selected channels that also get MIDI input
    std::array<std::atomic<int>, maxLiveChannels> extraLiveChannels {};
    std::atomic<int> extraLiveCount { 0 };
    void publishSelection();
    std::atomic<double> blockTimeMs { 0.0 };         // when positionTicks was last updated
    std::array<std::atomic<float>, scopeSize> scope {}; // ring buffer of the output (written by audio)
    std::atomic<int> scopeWrite { 0 };
    juce::MidiDeviceListConnection midiDevicesChanged;
    std::vector<std::string> lastMidiInputIds;       // the MIDI inputs available at the last check (UI thread)
    int midiCheckTicks = 0;
    std::atomic<std::uint32_t> midiActivity { 0 };
    prefs::MidiPrefs midiPrefs;                      // UI thread
    juce::SpinLock midiPrefsLock;                    // guards midiPrefsShared (UI writes, MIDI thread reads)
    prefs::MidiPrefs midiPrefsShared;
    void logPlayedNote (int channel, int key, float velocity, bool on);
    mutable juce::SpinLock scoreLogLock;             // MIDI thread and UI thread write, the UI reads
    scorelog::Log scoreLog;
    std::map<std::tuple<int, int, int, int, int, int>, prefs::PickupState> pickups; // (cc, channel, target) -> takeover
    // settings / log files written off the message thread (issue #13's freezes); declared before the saver so it
    // outlives it, and its destructor writes what is still queued
    SerialJobs fileJobs;
    struct DeviceStateSaver;
    std::unique_ptr<DeviceStateSaver> deviceStateSaver;

    // audio recording
    bool audioArmed = false;
    juce::String recordingError;
    juce::File recordingFolder = defaultRecordingFolder();
    juce::String recordingStem;
    std::vector<juce::File> sessionRecordings;
    juce::TimeSliceThread recordThread { "LYNX recording" };
    RecordingNotice recordingNotice;
    void notify (InputIssue issue, int track = 0, const juce::String& detail = {});
    // One take being recorded (round 163): the audio thread pushes into `stream`, the recording thread drains it
    // into `writer` (crash-safe WAV). Shared by the UI (start/finish), the audio thread (raw pointer under
    // takeLock) and the recording thread (drainer list under drainLock).
    struct Take
    {
        int track = 0;      // insert it plays through (0 = a new free insert)
        int first = 0;      // device input channel
        int channels = 0;   // 1 or 2
        juce::File file;
        std::unique_ptr<takes::TakeStream> stream;
        takes::WavTakeWriter writer;
        std::atomic<bool> failed { false };
        std::vector<float> scratch;
        juce::CriticalSection drainMutex; // the recording thread vs the UI's final drain
        // recording thread: writes what arrived; false when the file could not be written
        bool drain();
    };
    static constexpr int maxTakes = 16;
    std::array<Take*, maxTakes> takes {};          // guarded by takeLock (the audio thread's view)
    int takeCount = 0;
    std::vector<std::shared_ptr<Take>> openTakes;  // UI thread (owner)
    juce::CriticalSection drainLock;               // the recording thread's list
    std::vector<std::shared_ptr<Take>> draining;
    struct Drainer;
    std::unique_ptr<Drainer> drainer;
    juce::SpinLock takeLock;                       // the audio thread only tries (never waits)
    LiveInput liveInput;
    std::unique_ptr<perf::Live> performanceLive = std::make_unique<perf::Live>(); // Performance mode (round 165m)
    std::atomic<int> performanceMidiChannel { 0 }; // MidiPrefs::performanceChannel (UI writes, MIDI thread reads)
    std::atomic<bool> performanceMidiOn { false }; // Performance mode is on with a zone (UI writes, MIDI thread reads)
    perf::MidiMapping performanceMidi;             // MIDI thread only
    HardwareOutput hwOutput;                              // inserts' hardware outputs (renderer -> callback)
    std::array<int, HardwareOutput::maxChannels> outputSlot {}; // device output channel -> callback output index (-1: closed)
    std::atomic<int> masterOutChannel { outputDefault };  // the Master's target (UI -> audio)
    std::atomic<int> masterOutMode { 0 };
    std::vector<float> masterScratch = std::vector<float> (2 * HardwareOutput::capacity, 0.0f);
    void routeOutputs (float* const* outputs, int numOutputs, int n) noexcept; // audio thread
    std::array<float, maxMixerTracks * 2> lastInputPeaks {};
    std::array<int, LiveInput::maxChannels> inputSlot {}; // device input channel -> callback input index (-1: closed)
    std::atomic<double> takeStartTick { -1.0 };    // position of the first recorded sample
    std::atomic<juce::int64> takeFrames { 0 };
    std::atomic<double> takeTicks { 0.0 };         // ticks the transport moved while recording (not wrapped)
    std::array<std::atomic<juce::int64>, 256> takeWraps {}; // take frames at which the loop jumped back (loop recording)
    std::atomic<int> takeWrapCount { 0 };
    double takeSampleRate = 44100.0;
    double takeTicksPerSample = 0.0;
    int takeLatency = 0;                           // takes::Latency::total() at the take's start, in samples
    std::optional<PunchRange> activePunch;         // pre-roll / punch range of the recording in progress (UI thread)
    std::int64_t punchStopAt = -1;                 // post-roll end: playback stops there (-1: none)
    bool linearRecording() const noexcept
    {
        return song.mode == PlayMode::song && (! song.hasLoop() || recordOptions.punchLoopRegion);
    }
    // the audio thread's view of the above while takes record (false when none is open)
    void updateLinearSong() noexcept
    {
        const auto on = (takeCount > 0 || automationLinear) && linearRecording();
        liveInput.linearSong.store (on || bouncing);
        liveInput.noLoopRegion.store ((on && song.hasLoop()) || bouncing);
    }
    takes::LatencyPing ping;                       // audio thread while running (UI starts it, reads the result)
    juce::int64 lostFrames = 0;
    std::array<int, 2> pingOutputs { -1, -1 };     // callback output slots the click goes to
    int pingInput = -1;                            // callback input slot listened to
    RecordingSettings recordingSettings;
    juce::File journalFor (const juce::File& wav) const { return wav.withFileExtension ("wav.lynxrec"); }
    void writeJournals();
    bool journalsWritten = false;
    MeterBank meters;
    std::unique_ptr<AudioLogger> audioLogger; // after `meters` (it reads meters.logTap): destroyed first
    PluginHost plugins;
    juce::StringArray missingPlugins, matchedPlugins, pluginsWithoutState;
    FlpImportReport importReport;
    std::vector<MissingPlugin> missingPluginDetails;
    RenderStats lastRenderStats;
    class ChannelRackProcessor* rackProcessor = nullptr; // graph node (owner: the graph)

    Song song;
    int selectedChannel = 0;
    std::vector<int> selectedChannels { 0 };
    bool modified = false;
    std::unique_ptr<StreamPrefetcher> prefetcher; // only with an audio device (playing live)

    std::size_t maxUndoSteps = prefs::defaultUndoLevels;
    std::deque<Song> undoStack, redoStack;
    juce::Time lastUndoPress; // mouse click that already produced an undo step
    SnapshotExchange<RackState> rackExchange;
    RackState lastPublishedShape; // channels + instruments of the last snapshot, to reuse voices

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace lynx
