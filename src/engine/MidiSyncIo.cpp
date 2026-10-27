#include "engine/MidiSyncIo.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <thread>

#if JUCE_WINDOWS
extern "C" __declspec (dllimport) unsigned int __stdcall timeBeginPeriod (unsigned int);
extern "C" __declspec (dllimport) unsigned int __stdcall timeEndPeriod (unsigned int);
#endif

namespace lynx
{
namespace
{
void atomicMax (std::atomic<double>& a, double v) noexcept
{
    auto now = a.load (std::memory_order_relaxed);
    while (v > now && ! a.compare_exchange_weak (now, v, std::memory_order_relaxed))
    {
    }
}

void atomicAdd (std::atomic<double>& a, double v) noexcept
{
    auto now = a.load (std::memory_order_relaxed);
    while (! a.compare_exchange_weak (now, now + v, std::memory_order_relaxed))
    {
    }
}
} // namespace

MidiSyncIo::MidiSyncIo() : juce::Thread ("LYNX MIDI sync") {}

MidiSyncIo::~MidiSyncIo()
{
    stopThread (2000);
    const juce::ScopedLock lock (outputLock);
    for (auto& s : slots)
        s.device.reset();
}

void MidiSyncIo::configure (const prefs::MidiPrefs& prefs, bool devices, bool dryRun)
{
    dry.store (dryRun);
    {
        const juce::SpinLock::ScopedLockType lock (clockNameLock);
        clockName = juce::String::fromUTF8 (prefs.clockInput.c_str());
    }
    clockOffsetMs.store (juce::jlimit (-1000, 1000, prefs.clockOffsetMs));
    // (the receiver belongs to the MIDI thread: it resets itself on its next message)
    if (following.exchange (! prefs.clockInput.empty()) != ! prefs.clockInput.empty())
        resetReceiver.store (true);

    // the outputs that need a device: sync or notes off on
    std::vector<prefs::MidiOutputPrefs> wanted;
    for (const auto& o : prefs.outputs)
        if (o.masterSync || o.allNotesOff)
            wanted.push_back (o);
    if ((int) wanted.size() > maxOutputs)
        wanted.resize (maxOutputs);

    const auto available = devices ? juce::MidiOutput::getAvailableDevices() : juce::Array<juce::MidiDeviceInfo>();
    auto any = false;
    for (int i = 0; i < maxOutputs; ++i)
    {
        auto& slot = slots[(std::size_t) i];
        const prefs::MidiOutputPrefs* want = i < (int) wanted.size() ? &wanted[(std::size_t) i] : nullptr;
        const auto sameDevice = want != nullptr && slot.name == want->name && (slot.device != nullptr || ! devices);
        if (! sameDevice)
        {
            slot.active.store (false);
            std::unique_ptr<juce::MidiOutput> opened;
            if (want != nullptr && devices)
                for (const auto& d : available)
                    if (d.name.toStdString() == want->name)
                    {
                        opened = juce::MidiOutput::openDevice (d.identifier);
                        break;
                    }
            {
                const juce::ScopedLock lock (outputLock);
                slot.device = std::move (opened);
            }
            slot.name = want != nullptr ? want->name : std::string();
        }
        slot.sync = want != nullptr && want->masterSync;
        slot.notesOff = want != nullptr && want->allNotesOff;
        const auto type = (int) midisync::syncTypeFromKey (want != nullptr ? want->syncType : std::string());
        if (slot.type.exchange (type) != type || ! sameDevice)
            slot.generation.fetch_add (1);
        slot.offsetMs.store (want != nullptr ? juce::jlimit (-1000, 1000, want->offsetMs) : 0);
        slot.active.store (slot.sync && (slot.device != nullptr || ! devices));
        any = any || slot.active.load();
    }
    anySync.store (any);
    if ((devices || dryRun) && any && ! isThreadRunning())
        startThread (juce::Thread::Priority::highest);
}

void MidiSyncIo::sendAllNotesOff()
{
    const juce::ScopedLock lock (outputLock);
    for (auto& s : slots)
        if (s.notesOff && s.device != nullptr)
            for (int ch = 1; ch <= 16; ++ch)
            {
                s.device->sendMessageNow (juce::MidiMessage::controllerEvent (ch, 64, 0));
                s.device->sendMessageNow (juce::MidiMessage::allNotesOff (ch));
            }
}

void MidiSyncIo::deviceStarted (double rate, int bufferSize, int outputLatencySamples) noexcept
{
    sampleRate = rate > 0.0 ? rate : 44100.0;
    // the audio of a block computed now is heard one buffer plus the device's latency later: the sync messages for
    // its samples are due then
    latencyMs = 1000.0 * (bufferSize + std::max (0, outputLatencySamples)) / sampleRate;
    predictedMs = -1.0;
    for (auto& s : slots)
        s.generator.reset();
}

void MidiSyncIo::beginBlock (Transport& transport, double callbackMs, int numSamples) noexcept
{
    // the block clock: callbacks wake up with jitter, the samples don't. Follow the sample count and pull towards
    // the callback times slowly; resynchronise after a gap (xrun, device restart).
    if (predictedMs < 0.0 || std::abs (callbackMs - predictedMs) > 50.0)
        blockStartMs = callbackMs;
    else
        blockStartMs = predictedMs + 0.02 * (callbackMs - predictedMs);
    predictedMs = blockStartMs + 1000.0 * numSamples / sampleRate;

    if (! following.load (std::memory_order_relaxed))
    {
        midisync::Receiver::Command stale;
        double at = 0.0;
        while (clockIn.popCommand (stale, at)) // (nothing old is applied when following starts again)
        {
        }
        if (wasFollowing)
            transport.externalBpm.store (0.0, std::memory_order_relaxed);
        wasFollowing = false;
        waitingFirstPulse = false;
        appliedBpm.store (0.0, std::memory_order_relaxed);
        return;
    }
    wasFollowing = true;

    midisync::Receiver::Command command;
    double ticks = 0.0;
    while (clockIn.popCommand (command, ticks))
        switch (command)
        {
            case midisync::Receiver::Command::start:
            case midisync::Receiver::Command::resume:
                transport.playing.store (false, std::memory_order_relaxed);
                transport.positionTicks.store (ticks, std::memory_order_relaxed);
                waitingFirstPulse = true;
                break;
            case midisync::Receiver::Command::stop:
                transport.playing.store (false, std::memory_order_relaxed);
                waitingFirstPulse = false;
                break;
            case midisync::Receiver::Command::locate:
                if (! transport.playing.load (std::memory_order_relaxed))
                    transport.positionTicks.store (ticks, std::memory_order_relaxed);
                break;
            case midisync::Receiver::Command::none: break;
        }

    if (waitingFirstPulse && clockIn.running())
    {
        waitingFirstPulse = false;
        transport.playing.store (true, std::memory_order_relaxed);
    }

    const auto nowSeconds = blockStartMs * 0.001;
    const auto bpm = clockIn.receiving (nowSeconds) ? clockIn.bpm() : 0.0;
    auto applied = bpm;
    if (bpm > 0.0 && clockIn.running() && transport.playing.load (std::memory_order_relaxed))
    {
        // phase lock: the block's first sample is heard `latencyMs` from now; it should then be where the master is
        // (plus the user's offset: positive = LYNX later)
        const auto target = clockIn.ticksAt (nowSeconds + (latencyMs - clockOffsetMs.load (std::memory_order_relaxed)) * 0.001);
        const auto position = transport.positionTicks.load (std::memory_order_relaxed);
        const auto error = target - position;
        if (std::abs (error) > 480.0) // half a beat off (a jump on the master's side): relocate
            transport.positionTicks.store (target, std::memory_order_relaxed);
        else // pull in within ~0.5 s, at most ±5 % of the tempo
            applied = bpm * std::clamp (1.0 + error / (bpm * 16.0 * 0.5), 0.95, 1.05);
    }
    transport.externalBpm.store (applied, std::memory_order_relaxed);
    appliedBpm.store (bpm, std::memory_order_relaxed);
}

void MidiSyncIo::subBlock (double startTicks, double ticksPerSample, int numSamples, int offset, bool playing) noexcept
{
    if (! anySync.load (std::memory_order_relaxed))
        return;
    const auto t0 = juce::Time::getHighResolutionTicks();
    for (auto& slot : slots)
    {
        if (! slot.active.load (std::memory_order_relaxed))
            continue;
        if (const auto g = slot.generation.load (std::memory_order_acquire); g != slot.seenGeneration)
        {
            slot.seenGeneration = g;
            slot.generator.reset();
            slot.generator.setType ((midisync::SyncType) slot.type.load (std::memory_order_relaxed));
        }
        midisync::Block block;
        block.startTicks = startTicks;
        block.ticksPerSample = ticksPerSample;
        block.sampleRate = sampleRate;
        block.numSamples = numSamples;
        block.playing = playing;
        scratch.clear();
        const auto songSeconds = ticksPerSample > 0.0 ? startTicks / (ticksPerSample * sampleRate) : 0.0;
        slot.generator.process (block, songSeconds, scratch);
        const auto base = blockStartMs + latencyMs + slot.offsetMs.load (std::memory_order_relaxed);
        for (int i = 0; i < scratch.count; ++i)
        {
            const auto& m = scratch.items[(std::size_t) i];
            if (! slot.queue.push ({ base + 1000.0 * (offset + m.sample) / sampleRate, m }))
                droppedCount.fetch_add (1, std::memory_order_relaxed);
        }
    }
    const auto us = 1.0e6 * juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - t0);
    atomicAdd (blockSum, us);
    atomicMax (blockMax, us);
    blockN.fetch_add (1, std::memory_order_relaxed);
}

void MidiSyncIo::run()
{
#if JUCE_WINDOWS
    timeBeginPeriod (1); // 1 ms sleeps for this thread's waits
#endif
    std::array<std::deque<Timed>, maxOutputs> pending;
    while (! threadShouldExit())
    {
        auto any = false;
        for (int i = 0; i < maxOutputs; ++i)
        {
            Timed t;
            while (slots[(std::size_t) i].queue.pop (t))
                pending[(std::size_t) i].push_back (t);
            any = any || ! pending[(std::size_t) i].empty();
        }
        if (! any)
        {
            if (! anySync.load())
                wait (50);
            else
                std::this_thread::sleep_for (std::chrono::microseconds (500));
            continue;
        }

        auto now = juce::Time::getMillisecondCounterHiRes();
        auto next = now + 1000.0;
        {
            const juce::ScopedLock lock (outputLock);
            for (int i = 0; i < maxOutputs; ++i)
            {
                auto& queue = pending[(std::size_t) i];
                auto* device = slots[(std::size_t) i].device.get();
                while (! queue.empty() && queue.front().dueMs <= now + 0.05)
                {
                    const auto& m = queue.front().message;
                    if (device != nullptr || dry.load (std::memory_order_relaxed))
                    {
                        if (device != nullptr)
                            device->sendMessageNow (juce::MidiMessage (m.bytes.data(), (int) m.size));
                        now = juce::Time::getMillisecondCounterHiRes();
                        const auto late = std::max (0.0, now - queue.front().dueMs);
                        atomicAdd (lateSum, late);
                        atomicMax (lateMax, late);
                        lateN.fetch_add (1, std::memory_order_relaxed);
                        sentCount.fetch_add (1, std::memory_order_relaxed);
                    }
                    queue.pop_front();
                }
                if (! queue.empty())
                    next = std::min (next, queue.front().dueMs);
            }
        }
        // sleep until just before the next message, at most 1 ms (new messages arrive from the audio thread every
        // block), then in short steps (FL's sync thread polls with Sleep (1); REA 165f)
        const auto untilNext = next - juce::Time::getMillisecondCounterHiRes();
        if (untilNext > 0.3)
            std::this_thread::sleep_for (std::chrono::microseconds ((int) (std::min (untilNext - 0.25, 1.0) * 1000.0)));
        else if (untilNext > 0.02)
            std::this_thread::sleep_for (std::chrono::microseconds (50));
    }
#if JUCE_WINDOWS
    timeEndPeriod (1);
#endif
}

bool MidiSyncIo::handleInput (const juce::String& device, const juce::MidiMessage& m) noexcept
{
    if (! following.load (std::memory_order_relaxed))
        return false;
    {
        const juce::SpinLock::ScopedLockType lock (clockNameLock);
        if (device != clockName)
            return false;
    }
    if (resetReceiver.exchange (false))
        clockIn.reset();
    // (JUCE time-stamps input in seconds on Time::getMillisecondCounterHiRes()'s clock)
    return clockIn.feed (m.getRawData(), m.getRawDataSize(), m.getTimeStamp());
}

std::string MidiSyncIo::clockStatus() const
{
    return midisync::statusText (clockIn, juce::Time::getMillisecondCounterHiRes() * 0.001);
}

double MidiSyncIo::followedBpm() const noexcept
{
    return appliedBpm.load (std::memory_order_relaxed);
}

MidiSyncIo::Stats MidiSyncIo::stats() const
{
    // the last second's numbers (read and reset at most once a second)
    const auto now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastStatsMs < 1000.0)
        return lastStats;
    lastStatsMs = now;
    auto& self = const_cast<MidiSyncIo&> (*this);
    Stats s;
    s.sent = sentCount.load();
    s.dropped = droppedCount.load();
    const auto ln = self.lateN.exchange (0);
    s.meanLateMs = ln > 0 ? self.lateSum.exchange (0.0) / (double) ln : 0.0;
    s.maxLateMs = self.lateMax.exchange (0.0);
    const auto bn = self.blockN.exchange (0);
    s.meanBlockUs = bn > 0 ? self.blockSum.exchange (0.0) / (double) bn : 0.0;
    s.maxBlockUs = self.blockMax.exchange (0.0);
    lastStats = s;
    return s;
}
juce::String MidiSyncIo::selfTest (const juce::String& outputName, const juce::String& inputName)
{
    juce::String report;
    // 1. in memory: the generator's clock (time-stamped with ±0.5 ms of jitter) followed by a receiver
    {
        std::mt19937 random (165);
        std::uniform_real_distribution<double> jitter (-0.0005, 0.0005);
        for (const auto bpm : { 90.0, 120.0, 174.0 })
        {
            midisync::Generator g;
            midisync::Receiver r;
            const double rate = 48000.0, tps = bpm * 960.0 / (60.0 * rate);
            double ticks = 0.0;
            std::int64_t sample = 0;
            midisync::BlockMessages out;
            for (int b = 0; b < 48000 * 8 / 256; ++b)
            {
                midisync::Block block;
                block.startTicks = ticks;
                block.ticksPerSample = tps;
                block.sampleRate = rate;
                block.numSamples = 256;
                block.playing = true;
                out.clear();
                g.process (block, 0.0, out);
                for (int i = 0; i < out.count; ++i)
                    r.feed (out.items[(std::size_t) i].bytes.data(), out.items[(std::size_t) i].size,
                            (double) (sample + out.items[(std::size_t) i].sample) / rate + jitter (random));
                ticks += 256 * tps;
                sample += 256;
            }
            const auto positionError = r.ticksAt ((double) sample / rate) - ticks;
            report << "loopback bpm=" << bpm << " received=" << juce::String (r.bpm(), 3) << " error=" << juce::String (r.bpm() - bpm, 3)
                   << " position error ticks=" << juce::String (positionError, 2) << " running=" << (r.running() ? 1 : 0) << "\n";
        }
    }

    // 2. the sender thread at real time
    MidiSyncIo io;
    prefs::MidiPrefs p;
    prefs::MidiOutputPrefs o;
    o.name = outputName.isNotEmpty() ? outputName.toStdString() : std::string ("(dry run)");
    o.masterSync = true;
    p.setOutput (o);
    const auto real = outputName.isNotEmpty();
    io.configure (p, real, ! real);
    report << "output=" << (real ? outputName : juce::String ("(dry run, timed without a device)")) << " opened=" << (io.sending() ? 1 : 0) << "\n";
    for (const auto& d : juce::MidiOutput::getAvailableDevices())
        report << "  midi-out=" << d.name << "\n";
    for (const auto& d : juce::MidiInput::getAvailableDevices())
        report << "  midi-in=" << d.name << "\n";

    // the input side of a loopback: a receiver fed from the input callback
    struct In final : juce::MidiInputCallback
    {
        midisync::Receiver receiver;
        std::vector<double> times;
        std::mutex lock;
        void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m) override
        {
            receiver.feed (m.getRawData(), m.getRawDataSize(), m.getTimeStamp());
            if (m.getRawDataSize() > 0 && m.getRawData()[0] == 0xf8)
            {
                const std::lock_guard<std::mutex> g (lock);
                times.push_back (juce::Time::getMillisecondCounterHiRes());
            }
        }
    } in;
    std::unique_ptr<juce::MidiInput> input;
    if (inputName.isNotEmpty())
        for (const auto& d : juce::MidiInput::getAvailableDevices())
            if (d.name == inputName)
                input = juce::MidiInput::openDevice (d.identifier, &in);
    if (input != nullptr)
        input->start();

    if (io.sending())
    {
        Transport transport;
        transport.sampleRate.store (48000.0);
        transport.bpm.store (120.0);
        transport.playing.store (true);
        io.deviceStarted (48000.0, 256, 0);
        const auto start = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < 48000 * 5 / 256; ++b)
        {
            // wake up like an audio callback (with the scheduler's jitter), on the block's due time
            const auto due = start + b * 256.0 * 1000.0 / 48000.0;
            while (juce::Time::getMillisecondCounterHiRes() < due)
                std::this_thread::sleep_for (std::chrono::microseconds (200));
            io.beginBlock (transport, juce::Time::getMillisecondCounterHiRes(), 256);
            const auto tps = transport.ticksPerSample();
            io.subBlock (transport.positionTicks.load(), tps, 256, 0, true);
            transport.positionTicks.store (transport.positionTicks.load() + 256 * tps);
        }
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        io.lastStatsMs = 0.0;
        const auto s = io.stats();
        report << "sender: sent=" << (juce::int64) s.sent << " dropped=" << (juce::int64) s.dropped << " late mean ms=" << juce::String (s.meanLateMs, 3)
               << " max ms=" << juce::String (s.maxLateMs, 3) << " (5 s)\n";
        report << "audio thread per block: mean us=" << juce::String (s.meanBlockUs, 2) << " max us=" << juce::String (s.maxBlockUs, 2) << "\n";
    }
    if (input != nullptr)
    {
        input->stop();
        const std::lock_guard<std::mutex> g (in.lock);
        double sum = 0.0, sum2 = 0.0;
        const auto n = (int) in.times.size() - 1;
        for (int i = 0; i < n; ++i)
        {
            const auto d = in.times[(std::size_t) i + 1] - in.times[(std::size_t) i];
            sum += d;
            sum2 += d * d;
        }
        const auto mean = n > 0 ? sum / n : 0.0;
        report << "input=" << inputName << " clocks=" << (int) in.times.size() << " interval mean ms=" << juce::String (mean, 3)
               << " sd ms=" << juce::String (n > 0 ? std::sqrt (std::max (0.0, sum2 / n - mean * mean)) : 0.0, 3)
               << " received bpm=" << juce::String (in.receiver.bpm(), 2) << "\n";
    }
    else if (inputName.isNotEmpty())
        report << "input=" << inputName << " not found\n";
    return report;
}
} // namespace lynx
