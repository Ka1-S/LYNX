#pragma once

// MIDI sync devices (REA round 165f, docs/analise/REA_rodada165f-midisync.md): master sync (MIDI clock + SPP, or
// MTC) to the output devices that have "Send master sync", "Send all notes off" on init and after an export, and
// "External clock sync" from one input.
// Threads: the audio thread runs the generators and pushes time-stamped messages into one lock-free queue per output
// (it never touches a device); a sender thread sends each message when it is due; the MIDI thread feeds the clock
// receiver; the message thread opens and closes devices.

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <memory>
#include <string>

#include "core/LiveEventQueue.h"
#include "core/MidiSync.h"
#include "core/Preferences.h"
#include "engine/Transport.h"

namespace lynx
{
class MidiSyncIo final : private juce::Thread
{
public:
    static constexpr int maxOutputs = 8;

    MidiSyncIo();
    ~MidiSyncIo() override;

    // message thread: opens the outputs that send sync or notes off, closes the others, sets the clock input. With
    // `devices` false nothing is opened (tests, --snapshot).
    // `dryRun`: no device is opened but the sender runs and times every message as if it sent it (--midisync-test).
    void configure (const prefs::MidiPrefs& prefs, bool devices = true, bool dryRun = false);
    // message / render thread: CC 123 (All Notes Off) and CC 64 = 0 on the 16 channels of every output with the option
    void sendAllNotesOff();

    // --- audio thread ---
    void deviceStarted (double sampleRate, int bufferSize, int outputLatencySamples) noexcept;
    // Before the block: an external clock's commands and tempo move the transport. `callbackMs` = the callback's
    // Time::getMillisecondCounterHiRes().
    void beginBlock (Transport& transport, double callbackMs, int numSamples) noexcept;
    // Each part of the block (split at loop ends): the master sync messages for it. `offset` = its first sample in
    // the block.
    void subBlock (double startTicks, double ticksPerSample, int numSamples, int offset, bool playing) noexcept;
    bool sending() const noexcept { return anySync.load (std::memory_order_relaxed); }

    // --- MIDI thread: true if the message was the followed clock's (consumed) ---
    bool handleInput (const juce::String& device, const juce::MidiMessage& m) noexcept;

    // --- UI ---
    std::string clockStatus() const;             // "No clock" / "Receiving clock 120.0 BPM" (English; the UI translates)
    double followedBpm() const noexcept;         // the external tempo LYNX plays at now (0 = not following)
    struct Stats
    {
        std::uint64_t sent = 0, dropped = 0;
        double meanLateMs = 0.0, maxLateMs = 0.0;   // send time − due time, last second
        double meanBlockUs = 0.0, maxBlockUs = 0.0; // audio-thread cost of the sync per block, last second
    };
    Stats stats() const;

    // tests / --midisync-test: the receiver
    midisync::Receiver& receiver() noexcept { return clockIn; }
    // LYNX --midisync-test report.txt ["output device"] ["input device"]: the generator → receiver loopback in memory,
    // then 5 s of simulated audio callbacks (48 kHz, 256 frames) sending MIDI clock through the sender thread (to the
    // output device, or timed without one), and with an input (a loopback cable / virtual port) the clock received.
    static juce::String selfTest (const juce::String& outputName, const juce::String& inputName);

private:
    void run() override;

    struct Timed
    {
        double dueMs = 0.0;
        midisync::Message message;
    };
    struct Slot
    {
        std::atomic<bool> active { false };  // the audio thread generates for it
        std::atomic<int> type { 0 };
        std::atomic<int> offsetMs { 0 };
        std::atomic<std::uint32_t> generation { 0 };
        std::uint32_t seenGeneration = 0;    // audio thread
        midisync::Generator generator;       // audio thread
        SpscQueue<Timed, 2048> queue;        // audio → sender
        std::string name;                    // message thread
        bool notesOff = false, sync = false; // message thread
        std::unique_ptr<juce::MidiOutput> device; // under outputLock
    };
    std::array<Slot, maxOutputs> slots;
    juce::CriticalSection outputLock;        // the devices (message, sender, render threads; never the audio thread)
    std::atomic<bool> anySync { false }, dry { false };

    // timing: the device block clock mapped to milliseconds (smoothed: callbacks wake up with jitter)
    double sampleRate = 44100.0, latencyMs = 0.0;
    double blockStartMs = 0.0, predictedMs = -1.0;
    midisync::BlockMessages scratch;

    // external clock
    midisync::Receiver clockIn;
    juce::SpinLock clockNameLock;
    juce::String clockName;                  // under clockNameLock
    std::atomic<bool> following { false };
    std::atomic<int> clockOffsetMs { 0 };
    bool waitingFirstPulse = false;          // audio thread: Start / Continue received, play on the first pulse
    bool wasFollowing = false;
    std::atomic<double> appliedBpm { 0.0 };

    // stats (sender thread / audio thread writes, UI reads)
    std::atomic<std::uint64_t> sentCount { 0 }, droppedCount { 0 };
    std::atomic<double> lateSum { 0.0 }, lateMax { 0.0 }, blockSum { 0.0 }, blockMax { 0.0 };
    std::atomic<std::uint64_t> lateN { 0 }, blockN { 0 };
    mutable Stats lastStats;
    mutable double lastStatsMs = 0.0;
};
} // namespace lynx
