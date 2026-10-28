#include "core/VideoPlayer.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/Instruments.h"
#include "core/Song.h"

namespace lynx::video
{
namespace
{
struct Reader
{
    const std::vector<std::uint8_t>& b;
    std::size_t p = 0;
    bool ok = true;

    template <typename T>
    T get()
    {
        T v {};
        if (p + sizeof (T) > b.size())
        {
            ok = false;
            return v;
        }
        std::memcpy (&v, b.data() + p, sizeof (T));
        p += sizeof (T);
        return v;
    }
};

template <typename T>
void put (std::vector<std::uint8_t>& out, T v)
{
    const auto* p = reinterpret_cast<const std::uint8_t*> (&v);
    out.insert (out.end(), p, p + sizeof (T));
}

// UTF-16 ↔ UTF-8 (the path)
std::string utf8 (const std::vector<char16_t>& w)
{
    std::string s;
    for (std::size_t i = 0; i < w.size(); ++i)
    {
        std::uint32_t c = w[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < w.size() && w[i + 1] >= 0xdc00 && w[i + 1] < 0xe000)
            c = 0x10000 + ((c - 0xd800) << 10) + (std::uint32_t) (w[++i] - 0xdc00);
        if (c < 0x80)
            s += (char) c;
        else if (c < 0x800)
        {
            s += (char) (0xc0 | (c >> 6));
            s += (char) (0x80 | (c & 0x3f));
        }
        else if (c < 0x10000)
        {
            s += (char) (0xe0 | (c >> 12));
            s += (char) (0x80 | ((c >> 6) & 0x3f));
            s += (char) (0x80 | (c & 0x3f));
        }
        else
        {
            s += (char) (0xf0 | (c >> 18));
            s += (char) (0x80 | ((c >> 12) & 0x3f));
            s += (char) (0x80 | ((c >> 6) & 0x3f));
            s += (char) (0x80 | (c & 0x3f));
        }
    }
    return s;
}

std::vector<char16_t> utf16 (const std::string& s)
{
    std::vector<char16_t> w;
    for (std::size_t i = 0; i < s.size();)
    {
        const auto c0 = (unsigned char) s[i];
        std::uint32_t c = c0;
        int more = 0;
        if (c0 >= 0xf0) { c = c0 & 0x07; more = 3; }
        else if (c0 >= 0xe0) { c = c0 & 0x0f; more = 2; }
        else if (c0 >= 0xc0) { c = c0 & 0x1f; more = 1; }
        ++i;
        for (int k = 0; k < more && i < s.size(); ++k, ++i)
            c = (c << 6) | ((unsigned char) s[i] & 0x3f);
        if (c >= 0x10000)
        {
            c -= 0x10000;
            w.push_back ((char16_t) (0xd800 + (c >> 10)));
            w.push_back ((char16_t) (0xdc00 + (c & 0x3ff)));
        }
        else
            w.push_back ((char16_t) c);
    }
    return w;
}

constexpr char lynxTag[8] = { 'L', 'Y', 'N', 'X', 'v', 'p', '1', 0 };

AudioProvider& provider()
{
    static AudioProvider p;
    return p;
}
std::mutex& providerLock()
{
    static std::mutex m;
    return m;
}
} // namespace

const char* timeDisplayName (int mode) noexcept
{
    switch (mode)
    {
        case smpte23976: return "SMPTE (23.976 fps NONDROP)";
        case smpte24: return "SMPTE (24 fps NONDROP)";
        case smpte25: return "SMPTE (25 fps NONDROP)";
        case smpte2997drop: return "SMPTE (29.97 fps DROP)";
        case smpte30: return "SMPTE (30 fps NONDROP)";
        case smpteAuto: return "SMPTE (Auto)";
        case smpte2997: return "SMPTE (29.97 fps NONDROP)";
        case smpte48: return "SMPTE (48 fps NONDROP)";
        case smpte50: return "SMPTE (50 fps NONDROP)";
        case smpte5994drop: return "SMPTE (59.94 fps DROP)";
        case smpte5994: return "SMPTE (59.94 fps NONDROP)";
        case smpte60: return "SMPTE (60 fps NONDROP)";
        case smpte72: return "SMPTE (72 fps NONDROP)";
        case smpte120: return "SMPTE (120 fps NONDROP)";
        default: return "Hours:Minutes:Seconds:Ms";
    }
}

double timeDisplayFps (int mode, double videoFps) noexcept
{
    switch (mode)
    {
        case smpte23976: return 23.976;
        case smpte24: return 24.0;
        case smpte25: return 25.0;
        case smpte2997drop: case smpte2997: return 29.97;
        case smpte30: return 30.0;
        case smpteAuto: return videoFps > 0.0 ? videoFps : 0.0;
        case smpte48: return 48.0;
        case smpte50: return 50.0;
        case smpte5994drop: case smpte5994: return 59.94;
        case smpte60: return 60.0;
        case smpte72: return 72.0;
        case smpte120: return 120.0;
        default: return 0.0;
    }
}

bool timeDisplayDrops (int mode) noexcept
{
    return mode == smpte2997drop || mode == smpte5994drop;
}

std::string formatTime (double seconds, int mode, double videoFps)
{
    char buf[48];
    const auto negative = seconds < 0.0;
    auto t = std::abs (seconds);
    const auto fps = timeDisplayFps (mode, videoFps);
    const char* sign = negative ? "-" : "";
    if (fps <= 0.0)
    {
        // FL: whole milliseconds of the 100 ns time
        const auto ms = (std::int64_t) std::floor (t * 1000.0 + 1e-6);
        std::snprintf (buf, sizeof (buf), "%s%d:%02d:%02d:%03d", sign, (int) (ms / 3600000), (int) (ms / 60000 % 60),
                       (int) (ms / 1000 % 60), (int) (ms % 1000));
        return buf;
    }
    const auto drops = timeDisplayDrops (mode);
    const auto nominal = (std::int64_t) std::llround (fps);
    auto frame = (std::int64_t) std::floor (t * fps + 1e-6);
    if (drops)
    {
        // drop-frame: frame numbers 0 and 1 (2 and 3 at 59.94) are skipped every minute except every tenth
        const std::int64_t dropped = nominal == 60 ? 4 : 2;
        const auto perTen = nominal * 600 - dropped * 9;
        const auto perMinute = nominal * 60 - dropped;
        const auto tens = frame / perTen;
        auto rest = frame % perTen;
        frame += dropped * 9 * tens;
        if (rest > dropped)
            frame += dropped * ((rest - dropped) / perMinute);
    }
    const auto ff = frame % nominal;
    const auto total = frame / nominal;
    std::snprintf (buf, sizeof (buf), "%s%d:%02d:%02d%c%02d", sign, (int) (total / 3600), (int) (total / 60 % 60),
                   (int) (total % 60), drops ? ';' : ':', (int) ff);
    return buf;
}

std::vector<std::uint8_t> write (const Settings& s)
{
    std::vector<std::uint8_t> out;
    put<std::int32_t> (out, 3);
    std::uint32_t flags = 0;
    if (s.loop) flags |= 1;
    if (s.keepAspect) flags |= 2;
    flags |= (std::uint32_t) (std::clamp (s.transport, 0, 3) & 3) << 2;
    if (s.enableAudio) flags |= 0x10;
    if (s.ignoreTempo) flags |= 0x40;
    if (s.slave) flags |= 0x80;
    if (s.dock) flags |= 0x100;
    put<std::uint32_t> (out, flags);
    put<std::int32_t> (out, std::clamp (s.timeDisplay, 0, timeDisplayCount - 1));
    put<std::int32_t> (out, 3);
    put<std::int32_t> (out, std::clamp (s.volume, 0, 127));
    put<std::int32_t> (out, s.mute ? 1 : 0);
    put<std::int32_t> (out, s.play ? 1 : 0);
    const auto w = utf16 (s.path);
    if (w.size() < 0xff)
        out.push_back ((std::uint8_t) w.size());
    else
    {
        out.push_back (0xff);
        put<std::uint32_t> (out, (std::uint32_t) w.size());
    }
    for (const auto c : w)
        put<std::uint16_t> (out, (std::uint16_t) c);
    put<std::int32_t> (out, s.width);
    put<std::int32_t> (out, s.height);
    if (! w.empty())
    {
        put<std::int64_t> (out, s.start);
        put<std::int64_t> (out, s.end);
    }
    out.insert (out.end(), lynxTag, lynxTag + 8);
    put<std::int64_t> (out, s.delay);
    return out;
}

std::optional<Settings> read (const std::vector<std::uint8_t>& state)
{
    Reader r { state };
    const auto version = r.get<std::int32_t>();
    if (! r.ok || version < 1 || version > 3)
        return std::nullopt;
    Settings s;
    const auto flags = r.get<std::uint32_t>();
    if (version > 2)
        s.timeDisplay = std::clamp (r.get<std::int32_t>(), 0, timeDisplayCount - 1);
    auto count = 2;
    if (version > 2)
        count = r.get<std::int32_t>();
    if (! r.ok || count < 0 || count > 16)
        return std::nullopt;
    for (int i = 0; i < count; ++i)
    {
        const auto v = r.get<std::int32_t>();
        if (i == 0) s.volume = std::clamp (v, 0, 127);
        if (i == 1) s.mute = v != 0;
        if (i == 2) s.play = v != 0;
    }
    // the path: version 3 UTF-16 (byte length, 0xFF + int when longer); before, an ANSI string with the same lengths
    std::uint32_t length = r.get<std::uint8_t>();
    if (length == 0xff)
        length = r.get<std::uint32_t>();
    if (! r.ok || length > 32768)
        return std::nullopt;
    if (version > 2)
    {
        std::vector<char16_t> w;
        for (std::uint32_t i = 0; i < length; ++i)
            w.push_back ((char16_t) r.get<std::uint16_t>());
        s.path = utf8 (w);
    }
    else
        for (std::uint32_t i = 0; i < length; ++i)
            s.path += (char) r.get<std::uint8_t>();
    if (! r.ok)
        return std::nullopt;
    if (! s.path.empty() || version > 1)
    {
        s.width = r.get<std::int32_t>();
        s.height = r.get<std::int32_t>();
    }
    if (! s.path.empty())
    {
        s.start = std::max<std::int64_t> (0, r.get<std::int64_t>());
        s.end = std::max<std::int64_t> (0, r.get<std::int64_t>());
    }
    if (! r.ok)
        return std::nullopt;
    s.loop = (flags & 1) != 0;
    s.keepAspect = (flags & 2) != 0;
    s.transport = (int) ((flags >> 2) & 3);
    s.enableAudio = (flags & 0x30) != 0;
    s.ignoreTempo = (flags & 0x40) != 0;
    s.slave = version < 3 || (flags & 0x80) != 0;
    s.dock = (flags & 0x100) != 0;
    if (r.p + 16 <= state.size() && std::memcmp (state.data() + r.p, lynxTag, 8) == 0)
    {
        r.p += 8;
        s.delay = r.get<std::int64_t>();
    }
    return s;
}

Settings readOrDefault (const std::vector<std::uint8_t>& state)
{
    if (auto s = read (state))
        return *s;
    return {};
}

Info parseFfmpegReport (const std::string& report)
{
    Info info;
    if (const auto d = report.find ("Duration: "); d != std::string::npos)
    {
        int h = 0, m = 0;
        double sec = 0.0;
        if (std::sscanf (report.c_str() + d + 10, "%d:%d:%lf", &h, &m, &sec) == 3)
            info.duration = h * 3600.0 + m * 60.0 + sec;
    }
    std::size_t at = 0;
    while ((at = report.find ("Stream #", at)) != std::string::npos)
    {
        const auto lineEnd = report.find ('\n', at);
        const auto line = report.substr (at, lineEnd == std::string::npos ? std::string::npos : lineEnd - at);
        at = lineEnd == std::string::npos ? report.size() : lineEnd;
        if (line.find ("Audio:") != std::string::npos)
            info.hasAudio = true;
        if (line.find ("Video:") == std::string::npos || info.hasVideo)
            continue;
        if (line.find ("attached pic") != std::string::npos)
            continue;
        // "…, 1920x1080 [SAR 1:1 DAR 16:9], …, 29.97 fps, …" (the size: the first "<digits>x<digits>")
        for (std::size_t i = 0; i + 2 < line.size(); ++i)
        {
            if (line[i] < '1' || line[i] > '9' || (i > 0 && std::isalnum ((unsigned char) line[i - 1])))
                continue;
            int w = 0, h = 0, used = 0;
            if (std::sscanf (line.c_str() + i, "%dx%d%n", &w, &h, &used) == 2 && w > 0 && h > 0 && used > 2)
            {
                info.width = w;
                info.height = h;
                break;
            }
        }
        for (const char* key : { " fps", " tbr" })
            if (const auto f = line.find (key); f != std::string::npos && info.fps <= 0.0)
            {
                auto b = line.rfind (' ', f - 1);
                if (b != std::string::npos)
                    info.fps = std::atof (line.c_str() + b + 1);
            }
        info.hasVideo = info.width > 0;
    }
    return info;
}

double Clock::videoTime (double songSeconds, const Settings& s, double duration) noexcept
{
    const auto delay = (double) s.delay / (double) unitsPerSecond;
    auto t = songSeconds - delay;
    if (t < 0.0)
        return t;
    const auto start = (double) s.start / (double) unitsPerSecond;
    auto end = s.end > 0 ? (double) s.end / (double) unitsPerSecond : duration;
    if (duration > 0.0)
        end = std::min (end, duration);
    t += start;
    if (end > start && t >= end)
    {
        if (s.loop)
            t = start + std::fmod (t - start, end - start);
        else
            t = end;
    }
    return t;
}

void setAudioProvider (AudioProvider p)
{
    std::lock_guard<std::mutex> lock (providerLock());
    provider() = std::move (p);
}

namespace
{
std::shared_ptr<const AudioTrack> provide (const std::string& path, bool wait)
{
    AudioProvider p;
    {
        std::lock_guard<std::mutex> lock (providerLock());
        p = provider();
    }
    return p ? p (path, wait) : nullptr;
}

class VideoInstrument final : public Instrument, public Player
{
public:
    void prepare (double sampleRate) noexcept override { rate = sampleRate > 0.0 ? sampleRate : 44100.0; }
    void noteOn (int, float) noexcept override {}
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void transportStopped() noexcept override { songRunning.store (false, std::memory_order_relaxed); }
    void panic() noexcept override { ownStop(); }

    // UI thread
    void configure (const ChannelDef& def, double bpm) noexcept override
    {
        const auto s = readOrDefault (def.pluginState);
        loop.store (s.loop, std::memory_order_relaxed);
        enableAudio.store (s.enableAudio, std::memory_order_relaxed);
        ignoreTempo.store (s.ignoreTempo, std::memory_order_relaxed);
        slave.store (s.slave, std::memory_order_relaxed);
        gain.store (s.mute ? 0.0f : (float) s.volume / 100.0f, std::memory_order_relaxed);
        delay.store ((double) s.delay / (double) unitsPerSecond, std::memory_order_relaxed);
        start.store ((double) s.start / (double) unitsPerSecond, std::memory_order_relaxed);
        end.store ((double) s.end / (double) unitsPerSecond, std::memory_order_relaxed);
        if (bpm > 0.0)
            songBpm.store (bpm, std::memory_order_relaxed);
        if (s.path != path || (track == nullptr && ! s.path.empty()))
        {
            path = s.path;
            auto fresh = s.path.empty() || ! s.enableAudio ? nullptr : provide (s.path, offlineRendering());
            if (fresh != track)
            {
                if (track != nullptr)
                    retired.push_back (track); // (the audio thread may still read it; freed with the instrument)
                track = std::move (fresh);
                audio.store (track.get(), std::memory_order_release);
            }
        }
        else if (s.enableAudio && track == nullptr && ! path.empty())
        {
            track = provide (path, offlineRendering());
            audio.store (track.get(), std::memory_order_release);
        }
    }

    void setSongPosition (double ticks) noexcept override
    {
        const auto bpm = songBpm.load (std::memory_order_relaxed);
        const auto beatSeconds = ticks / (double) ppq * 60.0 / bpm;
        const auto moved = lastTicks >= 0.0 && ticks != lastTicks;
        const auto expected = lastTicks + lastBlock * lastTps;
        const auto jumped = ! moved || std::abs (ticks - expected) > std::max (1.0, lastBlock * lastTps * 0.5) || ticks < lastTicks;
        if (moved && lastBlock > 0)
            lastTps = (ticks - lastTicks) / (double) lastBlock;
        lastTicks = ticks;
        if (ignoreTempo.load (std::memory_order_relaxed) && moved && ! jumped)
            songSeconds += (double) lastBlock / rate; // real time
        else
            songSeconds = beatSeconds;
        songRunning.store (moved, std::memory_order_relaxed);
    }

    bool idle() const noexcept override
    {
        if (audio.load (std::memory_order_acquire) == nullptr || ! enableAudio.load (std::memory_order_relaxed)
            || gain.load (std::memory_order_relaxed) <= 0.0f)
            return true;
        return slave.load (std::memory_order_relaxed) ? ! songRunning.load (std::memory_order_relaxed) : ! ownRunning.load (std::memory_order_relaxed);
    }

    void render (float* left, float* right, int n, float gainLeft, float gainRight) noexcept override
    {
        lastBlock = n;
        const auto slaved = slave.load (std::memory_order_relaxed);
        if (! slaved && ownRequest.load (std::memory_order_acquire))
        {
            ownSeconds = ownTarget.load (std::memory_order_relaxed);
            ownRequest.store (false, std::memory_order_release);
        }
        const auto running = slaved ? songRunning.load (std::memory_order_relaxed) : ownRunning.load (std::memory_order_relaxed);
        Settings s;
        s.loop = loop.load (std::memory_order_relaxed);
        s.delay = (std::int64_t) std::llround (delay.load (std::memory_order_relaxed) * unitsPerSecond);
        s.start = (std::int64_t) std::llround (start.load (std::memory_order_relaxed) * unitsPerSecond);
        s.end = (std::int64_t) std::llround (end.load (std::memory_order_relaxed) * unitsPerSecond);
        const auto* a = audio.load (std::memory_order_acquire);
        const auto duration = a != nullptr && a->rate > 0.0 ? (double) a->frames() / a->rate : 0.0;
        const auto base = slaved ? songSeconds : ownSeconds;
        const auto t0 = slaved ? Clock::videoTime (base, s, duration) : base;
        now.store (t0, std::memory_order_relaxed);
        isRunning.store (running, std::memory_order_relaxed);
        if (! slaved && running)
            ownSeconds += (double) n / rate;
        if (! running || a == nullptr || ! enableAudio.load (std::memory_order_relaxed))
            return;
        const auto g = gain.load (std::memory_order_relaxed);
        if (g <= 0.0f)
            return;
        const auto frames = a->frames();
        const auto* data = a->samples.data();
        // per sample: the clock moves in real time inside the block (the loop / end applies at each sample)
        for (int i = 0; i < n; ++i)
        {
            const auto songT = base + (double) i / rate;
            const auto t = slaved ? Clock::videoTime (songT, s, duration) : songT;
            if (t < 0.0 || (! s.loop && s.end <= 0 && t >= duration))
                continue;
            const auto pos = t * a->rate;
            const auto k = (std::int64_t) pos;
            if (k < 0 || k + 1 >= frames)
                continue;
            const auto f = (float) (pos - (double) k);
            const auto l = (float) data[k * 2] + f * (float) (data[k * 2 + 2] - data[k * 2]);
            const auto r = (float) data[k * 2 + 1] + f * (float) (data[k * 2 + 3] - data[k * 2 + 1]);
            left[i] += l * (g * gainLeft / 32768.0f);
            right[i] += r * (g * gainRight / 32768.0f);
        }
    }

    double currentTime() const noexcept override { return now.load (std::memory_order_relaxed); }
    bool running() const noexcept override { return isRunning.load (std::memory_order_relaxed); }
    void ownPlay (double seconds) noexcept override
    {
        ownTarget.store (seconds, std::memory_order_relaxed);
        ownRequest.store (true, std::memory_order_release);
        ownRunning.store (true, std::memory_order_relaxed);
    }
    void ownPause() noexcept override { ownRunning.store (false, std::memory_order_relaxed); }
    void ownStop() noexcept override
    {
        ownRunning.store (false, std::memory_order_relaxed);
        ownTarget.store (start.load (std::memory_order_relaxed), std::memory_order_relaxed);
        ownRequest.store (true, std::memory_order_release);
    }

private:
    double rate = 44100.0;
    std::atomic<double> songBpm { 130.0 };
    std::atomic<bool> loop { false }, enableAudio { true }, ignoreTempo { false }, slave { true };
    std::atomic<float> gain { 1.0f };
    std::atomic<double> delay { 0.0 }, start { 0.0 }, end { 0.0 };
    std::atomic<const AudioTrack*> audio { nullptr };
    std::shared_ptr<const AudioTrack> track; // (UI thread)
    std::vector<std::shared_ptr<const AudioTrack>> retired;
    std::string path;

    // audio thread
    double lastTicks = -1.0, lastTps = 0.0, songSeconds = 0.0, ownSeconds = 0.0;
    int lastBlock = 0;
    std::atomic<bool> songRunning { false }, ownRunning { false }, ownRequest { false }, isRunning { false };
    std::atomic<double> ownTarget { 0.0 }, now { 0.0 };
};
} // namespace

std::unique_ptr<Instrument> makeInstrument()
{
    return std::make_unique<VideoInstrument>();
}

Player* playerOf (Instrument* instrument) noexcept
{
    return dynamic_cast<VideoInstrument*> (instrument);
}
} // namespace lynx::video
