#include "core/TakeRecording.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>

#if defined(_WIN32)
 #include <io.h>
#else
 #include <unistd.h>
#endif

namespace lynx::takes
{
std::string setupKey (const std::string& type, const std::string& output, const std::string& input, double sampleRate, int block)
{
    std::ostringstream s;
    s << type << '|' << output << '|' << input << '|' << (long long) std::llround (sampleRate) << '|' << block;
    return s.str();
}

// --- placement ------------------------------------------------------------------------------------------------------

namespace
{
// A part whose audio starts at unwrapped tick `uFrom` (>= exact) and ends at `uTo`, placed at song tick `sFrom`
// (fractional): the clip starts on the next whole tick and skips the audio before it.
bool makePart (const Placement& p, double uFrom, double uTo, double sFrom, Part& out)
{
    const auto clipStart = std::max<std::int64_t> (0, (std::int64_t) std::ceil (sFrom - 1.0e-7));
    const auto skipTicks = (uFrom - p.exactStart) + ((double) clipStart - sFrom);
    const auto sTo = sFrom + (uTo - uFrom);
    const auto clipEnd = (std::int64_t) std::floor (sTo + 1.0e-7);

    if (clipEnd <= clipStart || skipTicks < -1.0e-6)
        return false;

    out.start = clipStart;
    out.length = clipEnd - clipStart;
    out.offset = (std::int64_t) std::llround (skipTicks);
    out.offsetFraction = (float) (skipTicks - (double) out.offset);
    return true;
}
} // namespace

Placement place (double firstTick, double takeTicks, double ticksPerSample, int latency)
{
    Placement p;
    if (! (takeTicks > 0.0) || ! (ticksPerSample > 0.0))
        return p;

    p.exactStart = firstTick - (double) latency * ticksPerSample;
    p.ticks = takeTicks;
    p.latencyTicks = (double) latency * ticksPerSample;
    Part part;
    if (makePart (p, p.exactStart, p.exactStart + takeTicks, p.exactStart, part))
    {
        p.start = part.start;
        p.length = part.length;
        p.offset = part.offset;
        p.offsetFraction = part.offsetFraction;
    }
    return p;
}

std::vector<Part> punch (const Placement& p, const std::vector<PunchRange>& ranges)
{
    if (! p.valid())
        return {};
    if (ranges.empty())
        return { { p.start, p.length, p.offset, p.offsetFraction } };

    std::vector<Part> out;
    for (const auto& r : ranges)
    {
        const auto from = std::max (p.start, r.in), to = std::min (p.start + p.length, r.out);
        if (to > from)
            out.push_back ({ from, to - from, p.offset + (from - p.start), p.offsetFraction });
    }
    return out;
}


std::vector<Part> loopPasses (const Placement& p, std::int64_t loopStart, std::int64_t loopEnd, std::int64_t minimum, std::vector<double> wraps)
{
    if (loopEnd <= loopStart || ! (p.ticks > 0.0))
        return punch (p, {});

    // The file's tick axis: its first sample is at exactStart, the transport position of take tick w is
    // exactStart + latencyTicks + w. The transport jumped to loopStart at each wrap: what was heard from the wrap's
    // sample + the latency on is the song from loopStart on.
    const auto firstTick = p.exactStart + p.latencyTicks;
    if (wraps.empty()) // an exact loop: the first wrap where the transport reaches loopEnd, then every L
        for (auto w = (double) loopEnd - firstTick; w < p.ticks; w += (double) (loopEnd - loopStart))
            if (w > 0.0)
                wraps.push_back (w);

    std::vector<Part> out;
    Part part;
    const auto end = p.exactStart + p.ticks;
    // first pass: from the take's start (the pre-roll before the loop left out)
    {
        auto from = p.exactStart, songFrom = p.exactStart;
        if (songFrom < (double) loopStart)
            from += (double) loopStart - songFrom, songFrom = (double) loopStart;
        const auto to = std::min (end, wraps.empty() ? end : firstTick + wraps.front());
        if (to > from && makePart (p, from, to, songFrom, part))
            out.push_back (part);
    }
    for (std::size_t k = 0; k < wraps.size(); ++k)
    {
        const auto from = firstTick + wraps[k];
        const auto to = std::min (end, k + 1 < wraps.size() ? firstTick + wraps[k + 1] : end);
        if (to > from && makePart (p, from, to, (double) loopStart, part))
            out.push_back (part);
    }

    if (out.size() > 1 && minimum > 0)
        out.erase (std::remove_if (out.begin(), out.end(), [minimum] (const Part& x) { return x.length < minimum; }), out.end());
    return out;
}

int takeTrack (const Song& song, int insert, std::int64_t start, std::int64_t end)
{
    const auto freeAt = [&] (int track) {
        return std::none_of (song.playlist.begin(), song.playlist.end(), [&] (const Clip& c) { return c.track == track && c.start < end && c.end() > start; });
    };
    if (insert > 0)
        for (int t = 0; t < (int) song.playlistTracks.size(); ++t)
        {
            const auto& pt = song.playlistTracks[(std::size_t) t];
            if (pt.mode == PlaylistTrackMode::audio && pt.mixerTrack == insert && freeAt (t))
                return t;
        }
    int next = 0;
    for (const auto& c : song.playlist)
        next = std::max (next, c.track + 1);
    // (a track linked to another insert or a channel is not reused for this take)
    while (next < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) next].mode != PlaylistTrackMode::unassigned)
        ++next;
    return next;
}

void linkTakeTrack (Song& song, int track, int insert)
{
    if (track < 0 || insert <= 0 || insert >= (int) song.mixer.size())
        return;
    if ((int) song.playlistTracks.size() <= track)
        song.playlistTracks.resize ((std::size_t) track + 1);
    auto& pt = song.playlistTracks[(std::size_t) track];
    if (pt.mode != PlaylistTrackMode::unassigned)
        return;
    pt.mode = PlaylistTrackMode::audio;
    pt.mixerTrack = insert;
    if (pt.name.empty())
    {
        const auto& name = song.mixer[(std::size_t) insert].name;
        pt.name = name.empty() ? "Insert " + std::to_string (insert) : name;
    }
}

void placeTakeClips (Song& song, const std::vector<TakeFile>& files, const std::vector<Part>& parts)
{
    if (parts.empty())
        return;
    // loop passes: parts that overlap each other in song time
    auto lanes = false;
    for (std::size_t a = 0; a < parts.size() && ! lanes; ++a)
        for (std::size_t b = a + 1; b < parts.size(); ++b)
            if (parts[a].start < parts[b].start + parts[b].length && parts[b].start < parts[a].start + parts[a].length)
                lanes = true;

    for (const auto& f : files)
    {
        ChannelDef c;
        c.name = f.name;
        c.instrument = InstrumentKind::audioClip;
        c.samplePath = f.path;
        c.mixerTrack = f.insert > 0 && f.insert < (int) song.mixer.size() ? f.insert : song.nextFreeInsert();
        song.channels.push_back (c);
        const auto channel = (int) song.channels.size() - 1;
        const auto insert = c.mixerTrack;

        const auto add = [&] (const Part& part, int track, bool muted) {
            Clip clip;
            clip.kind = ClipKind::audio;
            clip.channel = channel;
            clip.track = track;
            clip.start = part.start;
            clip.length = part.length;
            clip.offset = part.offset;
            clip.offsetFraction = part.offsetFraction;
            clip.muted = muted;
            song.playlist.push_back (clip);
        };

        if (! lanes)
        {
            // punch parts: one track for all of them (they don't overlap)
            std::int64_t from = parts.front().start, to = 0;
            for (const auto& part : parts)
                from = std::min (from, part.start), to = std::max (to, part.start + part.length);
            const auto track = takeTrack (song, insert, from, to);
            for (const auto& part : parts)
                add (part, track, false);
            linkTakeTrack (song, track, insert);
            continue;
        }

        // the last pass on top, playing; the earlier ones below it, muted, newest first
        int top = -1, previous = -1;
        auto contiguous = true;
        for (auto k = (int) parts.size() - 1; k >= 0; --k)
        {
            const auto& part = parts[(std::size_t) k];
            const auto track = k + 1 == (int) parts.size() ? takeTrack (song, insert, part.start, part.start + part.length)
                                                           : takeTrack (song, -1, part.start, part.start + part.length);
            add (part, track, k + 1 != (int) parts.size());
            if (top < 0)
            {
                top = track;
                linkTakeTrack (song, track, insert);
            }
            else
                contiguous = contiguous && track == previous + 1;
            previous = track;
        }
        // fold the lanes under the take's track (Playlist track groups)
        if (contiguous && previous > top)
        {
            if ((int) song.playlistTracks.size() <= previous)
                song.playlistTracks.resize ((std::size_t) previous + 1);
            song.playlistTracks[(std::size_t) top].grouped = false;
            song.playlistTracks[(std::size_t) top].collapsed = true;
            for (auto t = top + 1; t <= previous; ++t)
            {
                auto& pt = song.playlistTracks[(std::size_t) t];
                pt.grouped = true;
                if (pt.name.empty())
                    pt.name = song.playlistTracks[(std::size_t) top].name + " (take " + std::to_string ((int) parts.size() - (t - top)) + ")";
            }
        }
    }
}

int compTrackOf (const Song& song, int track)
{
    if (track <= 0 || track >= (int) song.playlistTracks.size() || ! song.playlistTracks[(std::size_t) track].grouped)
        return -1;
    for (auto t = track - 1; t >= 0; --t)
        if (! song.playlistTracks[(std::size_t) t].grouped)
            return t;
    return -1;
}

bool promoteTake (Song& song, int clipIndex, int compTrack, std::int64_t from, std::int64_t to)
{
    if (clipIndex < 0 || clipIndex >= (int) song.playlist.size() || compTrack < 0)
        return false;
    const auto source = song.playlist[(std::size_t) clipIndex];
    if (source.kind != ClipKind::audio)
        return false;
    from = std::max (from, source.start);
    to = std::min (to, source.end());
    if (to <= from)
        return false;

    // what plays on the comp track in [from, to) goes; clips across an edge are cut there
    std::vector<Clip> kept;
    for (auto& c : song.playlist)
    {
        if (c.track != compTrack || c.end() <= from || c.start >= to)
        {
            kept.push_back (c);
            continue;
        }
        if (c.start < from) // the part before the range stays
        {
            Clip head = c;
            head.length = from - c.start;
            head.fadeOut = c.kind == ClipKind::audio ? compFadeTicks : 0;
            kept.push_back (head);
        }
        if (c.end() > to) // the part after it too
        {
            Clip tail = c;
            tail.start = to;
            tail.length = c.end() - to;
            tail.offset = c.offset + (to - c.start);
            tail.fadeIn = c.kind == ClipKind::audio ? compFadeTicks : 0;
            kept.push_back (tail);
        }
    }

    Clip piece = source;
    piece.track = compTrack;
    piece.start = from;
    piece.length = to - from;
    piece.offset = source.offset + (from - source.start);
    piece.muted = false;
    piece.fadeIn = from > source.start ? compFadeTicks : source.fadeIn;
    piece.fadeOut = to < source.end() ? compFadeTicks : source.fadeOut;
    kept.push_back (piece);
    song.playlist = std::move (kept);
    return true;
}

// --- latency ping ---------------------------------------------------------------------------------------------------

namespace
{
// a short raised-cosine pulse: its peak (index 2) is the click's time
constexpr std::array<float, 5> clickShape { 0.25f, 0.75f, 1.0f, 0.75f, 0.25f };
constexpr int clickPeak = 2;
} // namespace

void LatencyPing::start (int numClicks, int clickPeriod, float clickLevel)
{
    running.store (false);
    clicks = std::clamp (numClicks, 1, maxClicks);
    period = std::max (256, clickPeriod);
    level = std::clamp (clickLevel, 0.01f, 1.0f);
    position = 0;
    sent = 0;
    lastClick = -1;
    best = 0.0f;
    bestAt = -1;
    found = 0;
    noiseSum = 0.0;
    noiseCount = 0;
    running.store (true, std::memory_order_release);
}

void LatencyPing::process (const float* in, float* const* outs, int numOuts, int n) noexcept
{
    if (! running.load (std::memory_order_acquire))
        return;

    for (int i = 0; i < n; ++i)
    {
        const auto at = position + i;
        const auto x = in != nullptr ? std::abs (in[i]) : 0.0f;

        // the first period: the input's noise floor
        if (at < period)
        {
            noiseSum += (double) x * x;
            ++noiseCount;
        }

        // listening for the last click: its loudest sample
        if (lastClick >= 0 && x > best)
        {
            best = x;
            bestAt = at;
        }

        if (at > 0 && at % period == 0)
        {
            // the window of the previous click closes
            if (lastClick >= 0)
            {
                const auto noise = noiseCount > 0 ? (float) std::sqrt (noiseSum / (double) noiseCount) : 0.0f;
                if (best > std::max (0.002f, 8.0f * noise) && found < maxClicks)
                {
                    delays[(std::size_t) found] = (int) (bestAt - (lastClick + clickPeak));
                    peaks[(std::size_t) found] = best;
                    ++found;
                }
                lastClick = -1;
                best = 0.0f;
                bestAt = -1;
            }

            if (sent < clicks)
            {
                lastClick = at;
                ++sent;
            }
            else
            {
                running.store (false, std::memory_order_release);
                position = at;
                return;
            }
        }

        // the click itself (it may continue into the next block)
        if (lastClick >= 0 && at - lastClick < (std::int64_t) clickShape.size())
            for (int c = 0; c < numOuts; ++c)
                if (outs[c] != nullptr)
                    outs[c][i] += level * clickShape[(std::size_t) (at - lastClick)];
    }
    position += n;
}

LatencyPing::Result LatencyPing::result() const
{
    Result r;
    r.sent = sent;
    r.found = found;
    r.noise = noiseCount > 0 ? (float) std::sqrt (noiseSum / (double) noiseCount) : 0.0f;
    if (found == 0)
    {
        r.problem = "no signal";
        return r;
    }
    std::vector<int> d (delays.begin(), delays.begin() + found);
    std::sort (d.begin(), d.end());
    r.samples = d[d.size() / 2];
    r.spread = d.back() - d.front();
    std::vector<float> pk (peaks.begin(), peaks.begin() + found);
    std::sort (pk.begin(), pk.end());
    r.peak = pk[pk.size() / 2];
    if (found < std::max (3, (sent * 3) / 4))
        r.problem = "clicks missing";
    else if (r.spread > 4)
        r.problem = "unstable";
    else if (r.samples < 0)
        r.problem = "no signal";
    r.ok = r.problem.empty();
    return r;
}

// --- take stream ----------------------------------------------------------------------------------------------------

TakeStream::TakeStream (int channels, int capacityFrames)
    : numChannels (std::clamp (channels, 1, 2))
{
    std::uint64_t c = 1;
    while (c < (std::uint64_t) std::max (1024, capacityFrames))
        c <<= 1;
    capacity = c;
    mask = c - 1;
    ring.assign ((std::size_t) (capacity * (std::uint64_t) numChannels), 0.0f);
}

void TakeStream::push (const float* const* data, int n) noexcept
{
    if (n <= 0)
        return;

    const auto w = writeIndex.load (std::memory_order_relaxed);
    const auto r = readIndex.load (std::memory_order_acquire);
    auto space = (std::int64_t) (capacity - (w - r));

    // frames lost earlier go in as a gap right here, before anything newer
    if (pendingGap > 0)
    {
        const auto gw = gapWrite.load (std::memory_order_relaxed);
        if (gw - gapRead.load (std::memory_order_acquire) < maxGaps && space > 0)
        {
            gaps[(std::size_t) (gw % maxGaps)] = { w, pendingGap };
            gapWrite.store (gw + 1, std::memory_order_release);
            pendingGap = 0;
        }
        else
            space = 0; // keep the order: these frames are lost too
    }

    const auto count = (int) std::min<std::int64_t> (n, std::max<std::int64_t> (0, space));
    for (int c = 0; c < numChannels; ++c)
    {
        auto* dest = ring.data() + (std::size_t) c * capacity;
        const auto* src = data != nullptr ? data[c] : nullptr;
        for (int i = 0; i < count; ++i)
            dest[(std::size_t) ((w + (std::uint64_t) i) & mask)] = src != nullptr ? src[i] : 0.0f;
    }
    writeIndex.store (w + (std::uint64_t) count, std::memory_order_release);

    if (count < n)
    {
        pendingGap += n - count;
        lost.fetch_add (n - count, std::memory_order_relaxed);
    }
    pushed.fetch_add (n, std::memory_order_release);
}

void TakeStream::pushSilence (int n) noexcept
{
    push (nullptr, n);
}

void TakeStream::finish() noexcept
{
    finishedPending.store (pendingGap, std::memory_order_release);
    pendingGap = 0;
}

bool TakeStream::empty() const noexcept
{
    return writeIndex.load (std::memory_order_acquire) == readIndex.load (std::memory_order_relaxed) && zerosLeft == 0
           && gapRead.load (std::memory_order_relaxed) == gapWrite.load (std::memory_order_acquire)
           && finishedPending.load (std::memory_order_acquire) == 0;
}

int TakeStream::pull (float* const* out, int maxFrames) noexcept
{
    int done = 0;
    while (done < maxFrames)
    {
        if (zerosLeft > 0)
        {
            const auto k = (int) std::min<std::int64_t> (zerosLeft, maxFrames - done);
            for (int c = 0; c < numChannels; ++c)
                std::fill (out[c] + done, out[c] + done + k, 0.0f);
            zerosLeft -= k;
            done += k;
            continue;
        }

        const auto r = readIndex.load (std::memory_order_relaxed);
        const auto w = writeIndex.load (std::memory_order_acquire);
        auto limit = w;
        const auto gr = gapRead.load (std::memory_order_relaxed);
        if (gr != gapWrite.load (std::memory_order_acquire))
        {
            const auto& g = gaps[(std::size_t) (gr % maxGaps)];
            if (g.at == r)
            {
                zerosLeft = g.frames;
                gapRead.store (gr + 1, std::memory_order_release);
                continue;
            }
            limit = std::min (limit, g.at);
        }

        const auto available = (int) std::min<std::uint64_t> (limit - r, (std::uint64_t) (maxFrames - done));
        if (available <= 0)
        {
            // the audio thread let go of this take: what it lost at the very end
            if (r == w && gr == gapWrite.load (std::memory_order_acquire))
                if (const auto rest = finishedPending.exchange (0, std::memory_order_acq_rel); rest > 0)
                {
                    zerosLeft = rest;
                    continue;
                }
            break;
        }
        for (int c = 0; c < numChannels; ++c)
        {
            const auto* src = ring.data() + (std::size_t) c * capacity;
            for (int i = 0; i < available; ++i)
                out[c][done + i] = src[(std::size_t) ((r + (std::uint64_t) i) & mask)];
        }
        readIndex.store (r + (std::uint64_t) available, std::memory_order_release);
        done += available;
    }
    return done;
}

// --- WAV file -------------------------------------------------------------------------------------------------------

namespace
{
void put16 (unsigned char* p, std::uint32_t v) { p[0] = (unsigned char) v; p[1] = (unsigned char) (v >> 8); }
void put32 (unsigned char* p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = (unsigned char) (v >> (8 * i));
}
std::uint32_t get32 (const unsigned char* p) { return (std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16 | (std::uint32_t) p[3] << 24; }
std::uint16_t get16 (const unsigned char* p) { return (std::uint16_t) (p[0] | p[1] << 8); }

void syncToDisk (std::FILE* f)
{
    std::fflush (f);
#if defined(_WIN32)
    _commit (_fileno (f));
#else
    fsync (fileno (f));
#endif
}

std::int64_t fileSize (std::FILE* f)
{
#if defined(_WIN32)
    _fseeki64 (f, 0, SEEK_END);
    return _ftelli64 (f);
#else
    fseeko (f, 0, SEEK_END);
    return (std::int64_t) ftello (f);
#endif
}

void seekTo (std::FILE* f, std::int64_t at)
{
#if defined(_WIN32)
    _fseeki64 (f, at, SEEK_SET);
#else
    fseeko (f, (off_t) at, SEEK_SET);
#endif
}

std::FILE* openFile (const std::string& path, const char* mode)
{
#if defined(_WIN32)
    // UTF-8 path -> wide
    std::wstring wide;
    for (std::size_t i = 0; i < path.size();)
    {
        auto c = (unsigned char) path[i];
        std::uint32_t cp = c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (extra > 0)
            cp = c & (0x3Fu >> extra);
        for (int k = 1; k <= extra && i + (std::size_t) k < path.size(); ++k)
            cp = (cp << 6) | ((unsigned char) path[i + (std::size_t) k] & 0x3Fu);
        i += (std::size_t) extra + 1;
        if (cp >= 0x10000)
        {
            cp -= 0x10000;
            wide.push_back ((wchar_t) (0xD800 + (cp >> 10)));
            wide.push_back ((wchar_t) (0xDC00 + (cp & 0x3FF)));
        }
        else
            wide.push_back ((wchar_t) cp);
    }
    std::wstring m;
    for (const char* p = mode; *p != 0; ++p)
        m.push_back ((wchar_t) *p);
    return _wfopen (wide.c_str(), m.c_str());
#else
    return std::fopen (path.c_str(), mode);
#endif
}

constexpr int headerSize = 44;
} // namespace

WavTakeWriter::~WavTakeWriter()
{
    close();
}

bool WavTakeWriter::open (const std::string& path, double sampleRate, int channels, SampleFormat f, double syncSeconds)
{
    close();
    numChannels = std::clamp (channels, 1, 2);
    format = f;
    rate = sampleRate > 0.0 ? sampleRate : 48000.0;
    written = sinceSync = 0;
    syncFrames = std::max<std::int64_t> (1024, (std::int64_t) (syncSeconds * rate));
    file = openFile (path, "wb");
    if (file == nullptr)
    {
        problem = "could not create the file";
        return false;
    }
    std::setvbuf (file, nullptr, _IOFBF, 1 << 16);
    if (! writeHeader())
    {
        problem = "could not write the file";
        std::fclose (file);
        file = nullptr;
        return false;
    }
    return true;
}

bool WavTakeWriter::writeHeader()
{
    const auto bytes = format == SampleFormat::pcm24 ? 3 : 4;
    const auto data = (std::uint64_t) written * (std::uint64_t) (bytes * numChannels);
    const auto data32 = (std::uint32_t) std::min<std::uint64_t> (data, 0xFFFFFFFFull - headerSize);
    unsigned char h[headerSize] {};
    std::memcpy (h, "RIFF", 4);
    put32 (h + 4, data32 + headerSize - 8);
    std::memcpy (h + 8, "WAVEfmt ", 8);
    put32 (h + 16, 16);
    put16 (h + 20, format == SampleFormat::pcm24 ? 1 : 3);
    put16 (h + 22, (std::uint32_t) numChannels);
    put32 (h + 24, (std::uint32_t) std::llround (rate));
    put32 (h + 28, (std::uint32_t) std::llround (rate) * (std::uint32_t) (bytes * numChannels));
    put16 (h + 32, (std::uint32_t) (bytes * numChannels));
    put16 (h + 34, (std::uint32_t) (bytes * 8));
    std::memcpy (h + 36, "data", 4);
    put32 (h + 40, data32);
    seekTo (file, 0);
    const auto ok = std::fwrite (h, 1, headerSize, file) == (std::size_t) headerSize;
    seekTo (file, headerSize + (std::int64_t) data);
    return ok;
}

bool WavTakeWriter::write (const float* const* data, int n)
{
    if (file == nullptr || n <= 0)
        return file != nullptr;
    const auto bytes = format == SampleFormat::pcm24 ? 3 : 4;
    buffer.resize ((std::size_t) n * (std::size_t) (bytes * numChannels));
    auto* p = buffer.data();
    for (int i = 0; i < n; ++i)
        for (int c = 0; c < numChannels; ++c)
        {
            auto x = data[c] != nullptr ? data[c][i] : 0.0f;
            if (format == SampleFormat::float32)
            {
                std::uint32_t u;
                std::memcpy (&u, &x, 4);
                put32 (p, u);
                p += 4;
            }
            else
            {
                if (! (x == x))
                    x = 0.0f;
                const auto v = (std::int32_t) std::lrint (std::clamp ((double) x, -1.0, 1.0) * 8388607.0);
                p[0] = (unsigned char) v;
                p[1] = (unsigned char) (v >> 8);
                p[2] = (unsigned char) (v >> 16);
                p += 3;
            }
        }
    if (std::fwrite (buffer.data(), 1, buffer.size(), file) != buffer.size())
    {
        problem = "the disk is full or the file could not be written";
        return false;
    }
    written += n;
    sinceSync += n;
    if (sinceSync >= syncFrames)
    {
        sinceSync = 0;
        if (! writeHeader())
            return false;
        syncToDisk (file);
    }
    return true;
}

bool WavTakeWriter::close()
{
    if (file == nullptr)
        return true;
    auto ok = writeHeader();
    syncToDisk (file);
    ok = std::fclose (file) == 0 && ok;
    file = nullptr;
    return ok;
}

void WavTakeWriter::abandon()
{
    if (file != nullptr)
        std::fclose (file);
    file = nullptr;
}

namespace
{
struct WavLayout
{
    int format = 0, channels = 0, bits = 0;
    double rate = 0.0;
    std::int64_t dataAt = -1;
    std::uint32_t dataSize = 0, riffSize = 0;
    std::int64_t dataSizeAt = -1;
};

bool parseLayout (std::FILE* f, WavLayout& w)
{
    unsigned char h[12];
    seekTo (f, 0);
    if (std::fread (h, 1, 12, f) != 12 || std::memcmp (h, "RIFF", 4) != 0 || std::memcmp (h + 8, "WAVE", 4) != 0)
        return false;
    w.riffSize = get32 (h + 4);
    std::int64_t at = 12;
    for (int guard = 0; guard < 64; ++guard)
    {
        unsigned char c[8];
        seekTo (f, at);
        if (std::fread (c, 1, 8, f) != 8)
            return false;
        const auto size = get32 (c + 4);
        if (std::memcmp (c, "fmt ", 4) == 0)
        {
            unsigned char fmt[16];
            if (size < 16 || std::fread (fmt, 1, 16, f) != 16)
                return false;
            w.format = get16 (fmt);
            w.channels = get16 (fmt + 2);
            w.rate = (double) get32 (fmt + 4);
            w.bits = get16 (fmt + 14);
            if (w.format == 0xFFFE && size >= 40) // extensible: the sub-format's first two bytes
            {
                unsigned char ext[24];
                if (std::fread (ext, 1, 24, f) == 24)
                    w.format = get16 (ext + 8);
            }
        }
        else if (std::memcmp (c, "data", 4) == 0)
        {
            w.dataSizeAt = at + 4;
            w.dataAt = at + 8;
            w.dataSize = size;
            return w.channels > 0 && w.bits > 0;
        }
        at += 8 + (std::int64_t) size + (size & 1);
    }
    return false;
}
} // namespace

bool repairWav (const std::string& path, std::int64_t* frames, bool* changed)
{
    if (changed != nullptr)
        *changed = false;
    auto* f = openFile (path, "r+b");
    if (f == nullptr)
        return false;
    WavLayout w;
    if (! parseLayout (f, w))
    {
        std::fclose (f);
        return false;
    }
    const auto size = fileSize (f);
    const auto align = (std::int64_t) w.channels * (w.bits / 8);
    auto data = std::max<std::int64_t> (0, size - w.dataAt);
    data = std::min<std::int64_t> (data, 0xFFFFFFFFll - w.dataAt);
    if (align > 0)
        data -= data % align;
    auto ok = true;
    if ((std::int64_t) w.dataSize != data || (std::int64_t) w.riffSize != data + w.dataAt - 8)
    {
        unsigned char b[4];
        put32 (b, (std::uint32_t) data);
        seekTo (f, w.dataSizeAt);
        ok = std::fwrite (b, 1, 4, f) == 4;
        put32 (b, (std::uint32_t) (data + w.dataAt - 8));
        seekTo (f, 4);
        ok = std::fwrite (b, 1, 4, f) == 4 && ok;
        if (changed != nullptr)
            *changed = true;
    }
    if (frames != nullptr)
        *frames = align > 0 ? data / align : 0;
    ok = std::fclose (f) == 0 && ok;
    return ok;
}

bool readWav (const std::string& path, std::vector<std::vector<float>>& channels, double* sampleRate)
{
    auto* f = openFile (path, "rb");
    if (f == nullptr)
        return false;
    WavLayout w;
    if (! parseLayout (f, w) || ! (w.format == 1 || w.format == 3) || ! (w.bits == 16 || w.bits == 24 || w.bits == 32))
    {
        std::fclose (f);
        return false;
    }
    const auto bytes = w.bits / 8;
    const auto frames = (std::int64_t) w.dataSize / (bytes * w.channels);
    std::vector<unsigned char> raw ((std::size_t) (frames * bytes * w.channels));
    seekTo (f, w.dataAt);
    const auto got = std::fread (raw.data(), 1, raw.size(), f);
    std::fclose (f);
    if (got != raw.size())
        return false;
    channels.assign ((std::size_t) w.channels, std::vector<float> ((std::size_t) frames));
    const auto* p = raw.data();
    for (std::int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < w.channels; ++c)
        {
            float x = 0.0f;
            if (w.format == 3 && bytes == 4)
            {
                const auto u = get32 (p);
                std::memcpy (&x, &u, 4);
            }
            else if (bytes == 3)
            {
                auto v = (std::int32_t) ((std::uint32_t) p[0] | (std::uint32_t) p[1] << 8 | (std::uint32_t) p[2] << 16);
                if (v & 0x800000)
                    v -= 0x1000000;
                x = (float) v / 8388607.0f;
            }
            else if (bytes == 2)
                x = (float) (std::int16_t) get16 (p) / 32767.0f;
            else
                x = (float) ((double) (std::int32_t) get32 (p) / 2147483647.0);
            channels[(std::size_t) c][(std::size_t) i] = x;
            p += bytes;
        }
    if (sampleRate != nullptr)
        *sampleRate = w.rate;
    return true;
}

// --- journal, names -----------------------------------------------------------------------------------------------

std::string journalText (const Journal& j)
{
    std::ostringstream s;
    s.precision (17);
    s << "lynxrec=1\nwav=" << j.wav << "\nfirstTick=" << j.firstTick << "\nticksPerSample=" << j.ticksPerSample << "\nsampleRate=" << j.sampleRate
      << "\nlatency=" << j.latency << "\ninsert=" << j.insert << "\nplaylistTrack=" << j.playlistTrack << "\nproject=" << j.project << "\n";
    return s.str();
}

bool parseJournal (const std::string& text, Journal& j)
{
    std::istringstream in (text);
    std::string line;
    bool any = false;
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = line.substr (0, eq), v = line.substr (eq + 1);
        try
        {
            if (key == "lynxrec")
                any = true;
            else if (key == "wav")
                j.wav = v;
            else if (key == "firstTick")
                j.firstTick = std::stod (v);
            else if (key == "ticksPerSample")
                j.ticksPerSample = std::stod (v);
            else if (key == "sampleRate")
                j.sampleRate = std::stod (v);
            else if (key == "latency")
                j.latency = std::stoi (v);
            else if (key == "insert")
                j.insert = std::stoi (v);
            else if (key == "playlistTrack")
                j.playlistTrack = std::stoi (v);
            else if (key == "project")
                j.project = v;
        }
        catch (...)
        {
            return false;
        }
    }
    return any && ! j.wav.empty() && j.ticksPerSample > 0.0;
}

std::string takeName (const std::string& trackName, int take)
{
    std::string name;
    for (const auto ch : trackName)
    {
        const auto c = (unsigned char) ch;
        name.push_back (c < 32 || std::strchr ("<>:\"/\\|?*", ch) != nullptr ? '_' : ch);
    }
    while (! name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    while (! name.empty() && name.front() == ' ')
        name.erase (name.begin());
    if (name.empty())
        name = "Audio";
    return name + "_Take" + std::to_string (std::max (1, take));
}

double minutesLeft (std::int64_t bytesFree, double sampleRate, int channels, SampleFormat format) noexcept
{
    const auto perSecond = sampleRate * std::max (1, channels) * (format == SampleFormat::pcm24 ? 3 : 4);
    return perSecond > 0.0 ? (double) std::max<std::int64_t> (0, bytesFree) / perSecond / 60.0 : 0.0;
}
} // namespace lynx::takes
