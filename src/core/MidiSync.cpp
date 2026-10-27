#include "core/MidiSync.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace lynx::midisync
{
namespace
{
constexpr double ticksPerQuarter = 960.0;
constexpr double ticksPerPulse = ticksPerQuarter / 24.0; // 40
constexpr double ticksPerSixteenth = ticksPerQuarter / 4.0;
} // namespace

const char* syncTypeKey (SyncType type) noexcept
{
    switch (type)
    {
        case SyncType::midiClock: return "clock";
        case SyncType::mtc24: return "mtc24";
        case SyncType::mtc25: return "mtc25";
        case SyncType::mtc30Drop: return "mtc30drop";
        case SyncType::mtc30: return "mtc30";
        case SyncType::mtc2997Drop: return "mtc2997drop";
        case SyncType::mtc2997: return "mtc2997";
    }
    return "clock";
}

SyncType syncTypeFromKey (const std::string& key) noexcept
{
    for (int i = 0; i < syncTypeCount; ++i)
        if (key == syncTypeKey ((SyncType) i))
            return (SyncType) i;
    return SyncType::midiClock;
}

const char* syncTypeName (SyncType type) noexcept
{
    switch (type)
    {
        case SyncType::midiClock: return "MIDI clock";
        case SyncType::mtc24: return "MTC 24 fps";
        case SyncType::mtc25: return "MTC 25 fps";
        case SyncType::mtc30Drop: return "MTC 30 fps (drop)";
        case SyncType::mtc30: return "MTC 30 fps (non-drop)";
        case SyncType::mtc2997Drop: return "MTC 29.97 fps (drop)";
        case SyncType::mtc2997: return "MTC 29.97 fps (non-drop)";
    }
    return "MIDI clock";
}

MtcRate mtcRate (SyncType type) noexcept
{
    switch (type)
    {
        case SyncType::mtc24: return { 24.0, 24, false, 0 };
        case SyncType::mtc25: return { 25.0, 25, false, 1 };
        case SyncType::mtc30Drop: return { 30.0, 30, true, 2 };
        case SyncType::mtc30: return { 30.0, 30, false, 3 };
        case SyncType::mtc2997Drop: return { 30000.0 / 1001.0, 30, true, 2 };
        case SyncType::mtc2997: return { 30000.0 / 1001.0, 30, false, 3 };
        case SyncType::midiClock: break;
    }
    return { 25.0, 25, false, 1 };
}

Timecode frameToTimecode (std::int64_t frame, const MtcRate& rate) noexcept
{
    frame = std::max<std::int64_t> (0, frame);
    const auto n = (std::int64_t) rate.nominal;
    if (rate.drop)
    {
        // 30-frame numbering with labels 0 and 1 dropped at each minute except every tenth: 17 982 frames per 10 min
        const std::int64_t perTen = 10 * 60 * n - 9 * 2, perMinute = 60 * n - 2;
        const auto tens = frame / perTen, rest = frame % perTen;
        frame += 18 * tens + (rest > 1 ? 2 * ((rest - 2) / perMinute) : 0);
    }
    Timecode tc;
    tc.frames = (int) (frame % n);
    tc.seconds = (int) (frame / n % 60);
    tc.minutes = (int) (frame / (n * 60) % 60);
    tc.hours = (int) (frame / (n * 3600) % 24);
    return tc;
}

std::int64_t timecodeToFrame (const Timecode& tc, const MtcRate& rate) noexcept
{
    const auto n = (std::int64_t) rate.nominal;
    auto frame = ((std::int64_t) tc.hours * 3600 + tc.minutes * 60 + tc.seconds) * n + tc.frames;
    if (rate.drop)
    {
        const auto minutes = (std::int64_t) tc.hours * 60 + tc.minutes;
        frame -= 2 * (minutes - minutes / 10);
    }
    return frame;
}

std::uint8_t quarterFrameData (const Timecode& tc, int piece, int rateCode) noexcept
{
    int value = 0;
    switch (piece & 7)
    {
        case 0: value = tc.frames & 0x0f; break;
        case 1: value = (tc.frames >> 4) & 0x01; break;
        case 2: value = tc.seconds & 0x0f; break;
        case 3: value = (tc.seconds >> 4) & 0x03; break;
        case 4: value = tc.minutes & 0x0f; break;
        case 5: value = (tc.minutes >> 4) & 0x03; break;
        case 6: value = tc.hours & 0x0f; break;
        case 7: value = ((tc.hours >> 4) & 0x01) | ((rateCode & 3) << 1); break;
        default: break;
    }
    return (std::uint8_t) (((piece & 7) << 4) | value);
}

void fullFrame (const Timecode& tc, int rateCode, std::uint8_t* out) noexcept
{
    const std::uint8_t m[10] { 0xf0, 0x7f, 0x7f, 0x01, 0x01, (std::uint8_t) (((rateCode & 3) << 5) | (tc.hours & 0x1f)),
                               (std::uint8_t) (tc.minutes & 0x3f), (std::uint8_t) (tc.seconds & 0x3f), (std::uint8_t) (tc.frames & 0x1f), 0xf7 };
    std::copy (m, m + 10, out);
}

int sppFor (double ticks) noexcept
{
    const auto sixteenths = std::ceil (std::max (0.0, ticks) / ticksPerSixteenth - 1.0e-6);
    return (int) std::min (16383.0, sixteenths);
}

// --- generator ---------------------------------------------------------------------------------------------------
void Generator::setType (SyncType t) noexcept
{
    if (t != syncType)
    {
        syncType = t;
        reset();
    }
}

void Generator::reset() noexcept
{
    wasPlaying = false;
    nextPulseTicks = 0.0;
    expectedTicks = 0.0;
    nextQuarter = 0;
    mtcSeconds = 0.0;
}

void Generator::process (const Block& block, double songSeconds, BlockMessages& out) noexcept
{
    if (block.numSamples <= 0)
        return;
    // a jump the caller didn't flag: the block doesn't start where the last one ended (loop wrap, a click on the
    // timeline, an external clock's relocate)
    auto b = block;
    b.jumped = block.jumped || (block.playing && wasPlaying && std::abs (block.startTicks - expectedTicks) > 0.5);
    if (syncType == SyncType::midiClock)
        processClock (b, out);
    else
        processMtc (b, songSeconds, out);
    expectedTicks = block.startTicks + block.numSamples * block.ticksPerSample;
}

void Generator::processClock (const Block& block, BlockMessages& out) noexcept
{
    if (! block.playing)
    {
        if (wasPlaying)
            out.add (0, { 0xfc }); // Stop
        wasPlaying = false;
        return;
    }

    if (! wasPlaying || block.jumped)
    {
        if (wasPlaying)
            out.add (0, { 0xfc }); // a jump while playing: stop, relocate, continue
        if (block.startTicks < 0.5)
        {
            out.add (0, { 0xfa }); // Start (from the song's beginning)
            nextPulseTicks = 0.0;
        }
        else
        {
            const auto spp = sppFor (block.startTicks);
            out.add (0, { 0xf2, (std::uint8_t) (spp & 0x7f), (std::uint8_t) ((spp >> 7) & 0x7f) });
            out.add (0, { 0xfb }); // Continue: the first clock after it is at the SPP
            nextPulseTicks = spp * ticksPerSixteenth;
        }
        wasPlaying = true;
    }

    if (block.ticksPerSample <= 0.0)
        return;
    const auto end = block.startTicks + block.numSamples * block.ticksPerSample;
    // (a pulse is due at the first sample whose position reaches it)
    while (nextPulseTicks < end - 1.0e-9)
    {
        const auto at = std::ceil ((nextPulseTicks - block.startTicks) / block.ticksPerSample - 1.0e-9);
        out.add (std::clamp ((int) at, 0, block.numSamples - 1), { 0xf8 });
        nextPulseTicks += ticksPerPulse;
        if (out.overflow)
            break;
    }
}

void Generator::processMtc (const Block& block, double songSeconds, BlockMessages& out) noexcept
{
    if (! block.playing)
    {
        wasPlaying = false; // MTC: the quarter frames stop, the receiver stops with them
        return;
    }
    const auto rate = mtcRate (syncType);
    const auto quarter = 1.0 / (rate.fps * 4.0);

    if (! wasPlaying || block.jumped)
    {
        mtcSeconds = std::max (0.0, songSeconds);
        const auto frame = (std::int64_t) std::floor (mtcSeconds * rate.fps + 1.0e-9);
        Message m;
        m.size = 10;
        fullFrame (frameToTimecode (frame, rate), rate.code, m.bytes.data());
        if (out.count < (int) out.items.size())
            out.items[(std::size_t) out.count++] = m;
        // the quarter frames resume at the next even frame (piece 0 of a sequence starts on an even frame)
        auto first = (std::int64_t) std::ceil (mtcSeconds * rate.fps - 1.0e-9);
        if (first % 2 != 0)
            ++first;
        nextQuarter = first * 4;
        wasPlaying = true;
    }

    const auto blockSeconds = block.numSamples / block.sampleRate;
    while (nextQuarter * quarter < mtcSeconds + blockSeconds - 1.0e-12)
    {
        const auto at = std::ceil ((nextQuarter * quarter - mtcSeconds) * block.sampleRate - 1.0e-6);
        const auto piece = (int) (nextQuarter % 8);
        const auto tc = frameToTimecode ((nextQuarter / 8) * 2, rate);
        out.add (std::clamp ((int) at, 0, block.numSamples - 1), { 0xf1, quarterFrameData (tc, piece, rate.code) });
        ++nextQuarter;
        if (out.overflow)
            break;
    }
    mtcSeconds += blockSeconds;
}

// --- receiver ----------------------------------------------------------------------------------------------------
double fitBpm (const double* t, int count) noexcept
{
    if (count < 2)
        return 0.0;
    // least squares slope of time against pulse index
    double meanX = (count - 1) * 0.5, meanY = 0.0;
    for (int i = 0; i < count; ++i)
        meanY += t[i];
    meanY /= count;
    double sxy = 0.0, sxx = 0.0;
    for (int i = 0; i < count; ++i)
    {
        sxy += (i - meanX) * (t[i] - meanY);
        sxx += (i - meanX) * (i - meanX);
    }
    const auto secondsPerPulse = sxx > 0.0 ? sxy / sxx : 0.0;
    return secondsPerPulse > 0.0 ? 60.0 / (secondsPerPulse * 24.0) : 0.0;
}

void Receiver::reset() noexcept
{
    timeCount = timeHead = changeVotes = 0;
    shownBpm = 0.0;
    armed = false;
    positionTicks = sppTicks = 0.0;
    tempo.store (0.0);
    lastPulse.store (-1.0);
    lastPulseTicks.store (0.0);
    pulseSeconds.store (0.0);
    isRunning.store (false);
    fast.store (false);
}

void Receiver::pushCommand (Command c, double ticks) noexcept
{
    commands.push ({ c, ticks });
}

bool Receiver::popCommand (Command& c, double& ticks) noexcept
{
    Pending p;
    if (! commands.pop (p))
        return false;
    c = p.command;
    ticks = p.ticks;
    return true;
}

bool Receiver::receiving (double now) const noexcept
{
    const auto last = lastPulse.load (std::memory_order_relaxed);
    return last >= 0.0 && now - last < 0.5;
}

double Receiver::ticksAt (double seconds) const noexcept
{
    const auto base = lastPulseTicks.load (std::memory_order_relaxed);
    const auto last = lastPulse.load (std::memory_order_relaxed);
    const auto period = pulseSeconds.load (std::memory_order_relaxed);
    if (! running() || last < 0.0 || period <= 0.0)
        return base;
    // (extrapolated up to 0.1 s past the last pulse, at least one pulse: enough for the output latency, and a master
    // that stops sending without a Stop doesn't run LYNX away)
    return base + ticksPerPulse * std::clamp ((seconds - last) / period, 0.0, std::max (1.0, 0.1 / period));
}

void Receiver::pulse (double seconds) noexcept
{
    // tempo: the last `window` + 1 pulse times in a ring, oldest first when fitted
    constexpr int size = window + 1;
    times[(std::size_t) timeHead] = seconds;
    timeHead = (timeHead + 1) % size;
    timeCount = std::min (timeCount + 1, size);

    std::array<double, size> ordered {};
    for (int i = 0; i < timeCount; ++i)
        ordered[(std::size_t) i] = times[(std::size_t) ((timeHead - timeCount + i + 2 * size) % size)];

    // a tempo change: the mean of the last 6 intervals is more than 4 % off the fitted period on two pulses running
    // (one late message makes one interval long and the next short, which the 6-interval mean absorbs). The fit then
    // restarts from those 7 pulses, so the new tempo is followed within a few pulses instead of the whole window.
    constexpr int recent = 6;
    if (timeCount > recent + 4)
    {
        const auto fitted = fitBpm (ordered.data(), timeCount - recent);
        const auto mean = (ordered[(std::size_t) (timeCount - 1)] - ordered[(std::size_t) (timeCount - 1 - recent)]) / recent;
        const auto period = fitted > 0.0 ? 60.0 / (fitted * 24.0) : 0.0;
        changeVotes = period > 0.0 && std::abs (mean / period - 1.0) > 0.04 ? changeVotes + 1 : 0;
        if (changeVotes >= 2)
        {
            for (int i = 0; i <= recent; ++i)
                times[(std::size_t) i] = ordered[(std::size_t) (timeCount - 1 - recent + i)];
            timeCount = recent + 1;
            timeHead = timeCount % size;
            for (int i = 0; i < timeCount; ++i)
                ordered[(std::size_t) i] = times[(std::size_t) i];
            shownBpm = 0.0;
            changeVotes = 0;
        }
    }

    const auto estimate = fitBpm (ordered.data(), timeCount);
    if (estimate > 0.0)
    {
        fast.store (estimate > maxBpm, std::memory_order_relaxed);
        // hysteresis: a steady clock with jitter shows one steady tempo; the period used for the position follows the
        // estimate directly
        // (FL's receiver: the fitted tempo rounded to 0.1 BPM and kept within 10…522 BPM, its tempo range; REA 165f)
        if (shownBpm <= 0.0 || std::abs (estimate - shownBpm) > hysteresis)
            shownBpm = std::round (estimate * 10.0) / 10.0;
        tempo.store (std::clamp (shownBpm, minBpm, maxBpm), std::memory_order_relaxed);
        pulseSeconds.store (60.0 / (estimate * 24.0), std::memory_order_relaxed);
    }

    if (armed)
    {
        armed = false;
        positionTicks = sppTicks; // the first pulse after Start / Continue is at the song position
        isRunning.store (true, std::memory_order_relaxed);
    }
    else if (isRunning.load (std::memory_order_relaxed))
        positionTicks += ticksPerPulse;
    lastPulseTicks.store (positionTicks, std::memory_order_relaxed);
    lastPulse.store (seconds, std::memory_order_relaxed);
}

bool Receiver::feed (const std::uint8_t* bytes, int size, double seconds) noexcept
{
    if (size <= 0)
        return false;
    switch (bytes[0])
    {
        case 0xf8:
            pulse (seconds);
            return true;
        case 0xfa: // Start: from the beginning, on the next pulse
            sppTicks = 0.0;
            armed = true;
            pushCommand (Command::start, 0.0);
            return true;
        case 0xfb: // Continue: from the last Song Position Pointer, on the next pulse
            armed = true;
            pushCommand (Command::resume, sppTicks);
            return true;
        case 0xfc:
            armed = false;
            isRunning.store (false, std::memory_order_relaxed);
            lastPulseTicks.store (positionTicks, std::memory_order_relaxed);
            sppTicks = positionTicks; // a Continue without an SPP resumes where it stopped
            pushCommand (Command::stop, positionTicks);
            return true;
        case 0xf2:
            if (size < 3)
                return true;
            sppTicks = ((bytes[1] & 0x7f) | ((bytes[2] & 0x7f) << 7)) * ticksPerSixteenth;
            positionTicks = sppTicks;
            lastPulseTicks.store (positionTicks, std::memory_order_relaxed);
            pushCommand (Command::locate, sppTicks);
            return true;
        default:
            return false;
    }
}

std::string statusText (const Receiver& r, double now)
{
    if (! r.receiving (now) || r.bpm() <= 0.0)
        return "No clock";
    if (r.tooFast())
        return "Clock is too fast!";
    char text[64];
    std::snprintf (text, sizeof (text), "Receiving clock %.1f BPM%s", r.bpm(), r.running() ? ", playing" : "");
    return text;
}
} // namespace lynx::midisync
