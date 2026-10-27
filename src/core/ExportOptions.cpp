#include "core/ExportOptions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <sstream>

#include "core/Preferences.h"

namespace lynx::exportopts
{
const std::vector<int>& mp3Bitrates()
{
    static const std::vector<int> list { 96, 128, 160, 192, 224, 256, 320 };
    return list;
}

const std::vector<int>& oggBitrates()
{
    static const std::vector<int> list { 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 500 };
    return list;
}

const std::vector<double>& sampleRates()
{
    static const std::vector<double> list { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    return list;
}

const char* extension (Format format)
{
    switch (format)
    {
        case Format::mp3: return ".mp3";
        case Format::ogg: return ".ogg";
        case Format::flac: return ".flac";
        case Format::wav: break;
    }
    return ".wav";
}

int fileBits (const Options& o) noexcept
{
    switch (o.format)
    {
        case Format::wav: return o.wavBits;
        case Format::flac: return o.flacBits;
        case Format::mp3:
        case Format::ogg: break;
    }
    return 16;
}

bool usesDither (const Options& o) noexcept
{
    if (o.dither == DitherMode::none)
        return false;
    if (o.format == Format::wav)
        return o.wavBits == 16 || o.wavBits == 24;
    return o.format == Format::flac;
}

double mp3SampleRate (double rate) noexcept
{
    if (rate == 32000.0 || rate == 44100.0 || rate == 48000.0)
        return rate;
    return std::fmod (rate, 11025.0) == 0.0 ? 44100.0 : 48000.0;
}

namespace
{
template <typename T>
T nearest (const std::vector<T>& list, T value)
{
    auto best = list.front();
    for (auto v : list)
        if (std::abs ((double) v - (double) value) < std::abs ((double) best - (double) value))
            best = v;
    return best;
}

int toInt (const std::map<std::string, std::string>& kv, const char* key, int fallback)
{
    const auto it = kv.find (key);
    if (it == kv.end() || it->second.empty())
        return fallback;
    try
    {
        return std::stoi (it->second);
    }
    catch (...)
    {
        return fallback;
    }
}

double toDouble (const std::map<std::string, std::string>& kv, const char* key, double fallback)
{
    const auto it = kv.find (key);
    if (it == kv.end() || it->second.empty())
        return fallback;
    try
    {
        return std::stod (it->second);
    }
    catch (...)
    {
        return fallback;
    }
}
} // namespace

Options clamped (Options o)
{
    const Options d;
    if ((int) o.format < 0 || (int) o.format > 3) o.format = d.format;
    if (o.wavBits != 16 && o.wavBits != 24 && o.wavBits != 32) o.wavBits = d.wavBits;
    if (o.flacBits != 16 && o.flacBits != 24) o.flacBits = d.flacBits;
    o.flacLevel = std::clamp (o.flacLevel, 0, 8);
    o.mp3Kbps = nearest (mp3Bitrates(), o.mp3Kbps);
    o.oggKbps = nearest (oggBitrates(), o.oggKbps);
    o.sampleRate = nearest (sampleRates(), o.sampleRate);
    if ((int) o.dither < 0 || (int) o.dither > 2) o.dither = d.dither;
    if ((int) o.tail < 0 || (int) o.tail > 2) o.tail = d.tail;
    if (! (o.maxTailSeconds >= 0.0)) o.maxTailSeconds = d.maxTailSeconds;
    o.maxTailSeconds = std::min (o.maxTailSeconds, 60.0);
    if ((int) o.range < 0 || (int) o.range > 2) o.range = d.range;
    o.loops = std::clamp (o.loops, 1, 16);
    if ((int) o.stems < 0 || (int) o.stems > 2) o.stems = d.stems;
    if ((int) o.channels < 0 || (int) o.channels > 3) o.channels = d.channels;
    o.moreFormats &= 0xFu & ~(1u << (int) o.format);
    return o;
}

double lengthInBars (double seconds, double bpm, int beatsPerBar) noexcept
{
    return seconds <= 0.0 || bpm <= 0.0 ? 0.0 : seconds * bpm / 60.0 / (double) std::max (1, beatsPerBar);
}

std::int64_t estimatedBytes (const Options& o, double seconds) noexcept
{
    if (seconds <= 0.0)
        return 0;
    const auto channels = o.channels == Channels::stereo ? 2.0 : 1.0;
    std::int64_t total = 0; // every format the export writes (FL's disk space counts all ticked boxes)
    for (const auto f : { Format::wav, Format::mp3, Format::ogg, Format::flac })
        if (f == o.format || (o.moreFormats & (1u << (int) f)) != 0)
        switch (f)
        {
            case Format::wav: total += (std::int64_t) (seconds * o.sampleRate * channels * (o.wavBits / 8.0)) + 44; break;
            case Format::flac: total += (std::int64_t) (seconds * o.sampleRate * channels * (o.flacBits / 8.0) * 0.6); break; // ~60 % of PCM
            case Format::mp3: total += (std::int64_t) (seconds * o.mp3Kbps * 1000.0 / 8.0); break;
            case Format::ogg: total += (std::int64_t) (seconds * o.oggKbps * 1000.0 / 8.0); break;
        }
    return total;
}

std::vector<Format> formatsOf (const Options& o)
{
    std::vector<Format> out { o.format };
    for (const auto f : { Format::wav, Format::mp3, Format::ogg, Format::flac })
        if (f != o.format && (o.moreFormats & (1u << (int) f)) != 0)
            out.push_back (f);
    return out;
}

std::uint32_t flLastExportedMask (const Options& o) noexcept
{
    std::uint32_t mask = flFormatBit (o.format);
    for (const auto f : { Format::wav, Format::mp3, Format::ogg, Format::flac })
        if ((o.moreFormats & (1u << (int) f)) != 0)
            mask |= flFormatBit (f);
    return mask;
}

Options withFlLastExported (Options o, std::uint32_t mask)
{
    if (mask < 8 || mask > 0x3FF) // FL: anything else falls back to WAV
        mask = 8;
    std::vector<Format> chosen;
    for (const auto f : { Format::wav, Format::mp3, Format::ogg, Format::flac })
        if ((mask & flFormatBit (f)) != 0)
            chosen.push_back (f);
    if (chosen.empty()) // only formats LYNX writes elsewhere (MIDI, M4A): WAV
        chosen.push_back (Format::wav);
    o.format = chosen.front();
    o.moreFormats = 0;
    for (std::size_t i = 1; i < chosen.size(); ++i)
        o.moreFormats |= 1u << (int) chosen[i];
    return o;
}

Options withSingleFormat (Options o, Format f)
{
    o.format = f;
    o.moreFormats = 0;
    return o;
}

std::vector<Note> midiExportNotes (const Song& song)
{
    std::vector<Note> out;
    const auto trackMuted = [&song] (int track) {
        return track >= 0 && track < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) track].muted;
    };
    if (song.mode == PlayMode::song)
        for (const auto& c : song.playlist)
        {
            if (c.kind != ClipKind::pattern || c.muted || c.length <= 0 || trackMuted (c.track) || c.pattern < 0
                || c.pattern >= (int) song.patterns.size())
                continue;
            for (auto n : song.patterns[(std::size_t) c.pattern].notes)
            {
                if (n.muted || n.start < c.offset || n.start >= c.offset + c.length)
                    continue;
                n.start = c.start + (n.start - c.offset);
                n.length = std::max<std::int64_t> (1, std::min (n.length, c.end() - n.start));
                out.push_back (n);
            }
        }

    if (out.empty() && (song.mode != PlayMode::song || std::none_of (song.playlist.begin(), song.playlist.end(),
                                                                       [] (const Clip& c) { return c.kind == ClipKind::pattern; })))
        if (song.currentPattern >= 0 && song.currentPattern < (int) song.patterns.size())
            for (const auto& n : song.patterns[(std::size_t) song.currentPattern].notes)
                if (! n.muted)
                    out.push_back (n);

    std::stable_sort (out.begin(), out.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
    return out;
}

std::string toText (const Options& o)
{
    std::ostringstream s;
    s << "format=" << (int) o.format << "\n"
      << "wavBits=" << o.wavBits << "\n"
      << "flacBits=" << o.flacBits << "\n"
      << "flacLevel=" << o.flacLevel << "\n"
      << "mp3Kbps=" << o.mp3Kbps << "\n"
      << "oggKbps=" << o.oggKbps << "\n"
      << "sampleRate=" << (int) o.sampleRate << "\n"
      << "dither=" << (int) o.dither << "\n"
      << "tail=" << (int) o.tail << "\n"
      << "maxTail=" << o.maxTailSeconds << "\n"
      << "range=" << (int) o.range << "\n"
      << "loops=" << o.loops << "\n"
      << "stems=" << (int) o.stems << "\n"
      << "stemsMaster=" << (o.stemsIncludeMaster ? 1 : 0) << "\n"
      << "normalize=" << (o.normalize ? 1 : 0) << "\n"
      << "masterEffects=" << (o.masterEffects ? 1 : 0) << "\n"
      << "trimPdc=" << (o.trimPdc ? 1 : 0) << "\n"
      << "hqAllPlugins=" << (o.hqAllPlugins ? 1 : 0) << "\n"
      << "pluginWarmUp=" << (o.pluginWarmUp ? 1 : 0) << "\n"
      << "showWhenDone=" << (o.showWhenDone ? 1 : 0) << "\n"
      << "disableMaxPolyphony=" << (o.disableMaxPolyphony ? 1 : 0) << "\n"
      << "moreFormats=" << o.moreFormats << "\n"
      << "channels=" << (int) o.channels << "\n";
    return s.str();
}

Options fromText (const std::string& text)
{
    const auto kv = prefs::parseKeyValues (text);
    const Options d;
    Options o;
    o.format = (Format) toInt (kv, "format", (int) d.format);
    o.wavBits = toInt (kv, "wavBits", d.wavBits);
    o.flacBits = toInt (kv, "flacBits", d.flacBits);
    o.flacLevel = toInt (kv, "flacLevel", d.flacLevel);
    o.mp3Kbps = toInt (kv, "mp3Kbps", d.mp3Kbps);
    o.oggKbps = toInt (kv, "oggKbps", d.oggKbps);
    o.sampleRate = toDouble (kv, "sampleRate", d.sampleRate);
    o.dither = (DitherMode) toInt (kv, "dither", (int) d.dither);
    o.tail = (TailMode) toInt (kv, "tail", (int) d.tail);
    o.maxTailSeconds = toDouble (kv, "maxTail", d.maxTailSeconds);
    o.range = (Range) toInt (kv, "range", (int) d.range);
    o.loops = toInt (kv, "loops", d.loops);
    o.stems = (Stems) toInt (kv, "stems", (int) d.stems);
    o.stemsIncludeMaster = toInt (kv, "stemsMaster", d.stemsIncludeMaster ? 1 : 0) != 0;
    o.normalize = toInt (kv, "normalize", d.normalize ? 1 : 0) != 0;
    o.masterEffects = toInt (kv, "masterEffects", d.masterEffects ? 1 : 0) != 0;
    o.trimPdc = toInt (kv, "trimPdc", d.trimPdc ? 1 : 0) != 0;
    o.hqAllPlugins = toInt (kv, "hqAllPlugins", d.hqAllPlugins ? 1 : 0) != 0;
    o.pluginWarmUp = toInt (kv, "pluginWarmUp", d.pluginWarmUp ? 1 : 0) != 0;
    o.showWhenDone = toInt (kv, "showWhenDone", d.showWhenDone ? 1 : 0) != 0;
    o.disableMaxPolyphony = toInt (kv, "disableMaxPolyphony", d.disableMaxPolyphony ? 1 : 0) != 0;
    o.moreFormats = (std::uint32_t) std::clamp (toInt (kv, "moreFormats", 0), 0, 15);
    o.channels = (Channels) toInt (kv, "channels", (int) d.channels);
    return clamped (o);
}

bool hasSelection (const Song& song) noexcept
{
    return song.loopEnd > song.loopStart && song.loopStart >= 0;
}

Song makeRenderSong (const Song& song, const Options& o, std::int64_t tailTicks, std::int64_t& musicTicks, std::int64_t& loopTicks)
{
    Song base = song;
    std::int64_t length = 0;

    if (o.range == Range::selection && hasSelection (song))
    {
        const auto from = song.loopStart, to = song.loopEnd;
        std::vector<Clip> cropped;

        for (auto clip : song.playlist)
        {
            if (clip.end() <= from || clip.start >= to || clip.length <= 0)
                continue;
            if (clip.start < from)
                clip = splitClip (clip, from);
            if (clip.end() > to)
                splitClip (clip, to); // keeps the head
            clip.start -= from;
            cropped.push_back (clip);
        }

        base.playlist = std::move (cropped);
        base.mode = PlayMode::song;
        std::int64_t ignored = 0;
        base = makeExportSong (base, true, 0, ignored);
        length = to - from;

        if (base.playlist.empty()) // nothing in the selection: a silent pass of the right length
        {
            base.patterns.emplace_back().name = "(silence)";
            Clip silent;
            silent.pattern = (int) base.patterns.size() - 1;
            silent.length = length;
            base.playlist.push_back (silent);
        }
    }
    else
        base = makeExportSong (song, o.range == Range::song || (o.range == Range::selection), 0, length);

    length = std::max<std::int64_t> (1, length);
    loopTicks = length;

    // the range repeated: copies of the clips shifted by one pass each
    const auto loops = std::clamp (o.loops, 1, 16);
    if (loops > 1)
    {
        const auto once = base.playlist;
        for (int k = 1; k < loops; ++k)
            for (auto clip : once)
            {
                clip.start += length * k;
                base.playlist.push_back (clip);
            }
    }

    musicTicks = length * loops;

    // the tail: an empty pattern after the music (plus a bar of slack, like makeExportSong)
    if (tailTicks > 0)
    {
        base.patterns.emplace_back().name = "(tail)";
        Clip tail;
        tail.pattern = (int) base.patterns.size() - 1;
        tail.start = musicTicks;
        tail.length = tailTicks + (std::int64_t) ppq * std::max (1, base.beatsPerBar);
        base.playlist.push_back (tail);
    }

    if (! o.masterEffects && ! base.mixer.empty())
        base.mixer[0].effects.clear();

    if (o.disableMaxPolyphony) // FL: every channel's Max polyphony becomes 0 (no limit) for the render
        for (auto& c : base.channels)
            c.maxPolyphony = 0;

    // FL's export plays the song straight through: marker loops / skips / pauses and the Start / Loop markers act only
    // while playing live (REA_rodada165f-markers §1.3: FL's renders of every marker action match the plain song)
    for (auto& a : base.arrangements)
        for (auto& m : a.markers)
            if (m.action == Marker::markerLoop || m.action == Marker::markerSkip || m.action == Marker::markerPause
                || m.action == Marker::songLoop || m.action == Marker::start)
                m.action = Marker::none;

    base.mode = PlayMode::song;
    base.loopStart = base.loopEnd = 0;
    return base;
}

std::vector<int> stemTracks (const Song& song)
{
    std::vector<int> out;
    for (int t = 1; t < (int) song.mixer.size(); ++t)
    {
        auto used = std::any_of (song.channels.begin(), song.channels.end(), [t] (const ChannelDef& c) { return c.mixerTrack == t; });
        for (int src = 1; src < (int) song.mixer.size() && ! used; ++src)
            used = src != t && song.routes (src, t);
        if (used)
            out.push_back (t);
    }
    return out;
}

std::vector<int> stemChannels (const Song& song, Range range)
{
    std::set<int> used;
    auto addPattern = [&] (int p) {
        if (p < 0 || p >= (int) song.patterns.size())
            return;
        for (const auto& n : song.patterns[(std::size_t) p].notes)
            used.insert (n.channel);
    };

    if (range == Range::pattern || song.playlist.empty())
        addPattern (song.currentPattern);
    else
        for (const auto& c : song.playlist)
        {
            if (c.muted || song.isTrackMuted (c.track))
                continue;
            if (c.kind == ClipKind::pattern)
                addPattern (c.pattern);
            else if (c.kind == ClipKind::audio)
                used.insert (c.channel);
        }

    std::vector<int> out;
    for (auto c : used)
        if (c >= 0 && c < (int) song.channels.size() && ! song.channels[(std::size_t) c].muted)
            out.push_back (c);
    return out;
}

Song makeChannelStemSong (const Song& song, int channel)
{
    Song out = song;
    for (int c = 0; c < (int) out.channels.size(); ++c)
        if (c != channel)
            out.channels[(std::size_t) c].muted = true;
    return out;
}

std::string stemFileName (const std::string& base, int number, const std::string& name, const char* ext)
{
    std::string clean;
    for (auto ch : name)
        clean += std::string ("<>:\"/\\|?*").find (ch) != std::string::npos || (unsigned char) ch < 32 ? '_' : ch;
    while (! clean.empty() && (clean.back() == ' ' || clean.back() == '.'))
        clean.pop_back();
    return base + " - " + std::to_string (number) + (clean.empty() ? "" : " " + clean) + ext;
}

void applyTail (std::vector<float>& left, std::vector<float>& right, std::int64_t loopFrames, TailMode mode)
{
    const auto frames = (std::int64_t) left.size();
    if (mode == TailMode::leave || loopFrames <= 0 || frames <= loopFrames)
    {
        if (mode != TailMode::leave && loopFrames > 0 && frames < loopFrames) // render ended early: pad to the length
        {
            left.resize ((std::size_t) loopFrames, 0.0f);
            right.resize ((std::size_t) loopFrames, 0.0f);
        }
        return;
    }

    if (mode == TailMode::wrap)
        for (auto i = loopFrames; i < frames; ++i)
        {
            const auto j = (std::size_t) (i % loopFrames);
            left[j] += left[(std::size_t) i];
            if ((std::size_t) i < right.size())
                right[j] += right[(std::size_t) i];
        }

    left.resize ((std::size_t) loopFrames);
    right.resize ((std::size_t) loopFrames);
}

float normalizeGain (float peak) noexcept
{
    return peak > 1.0e-9f ? 1.0f / peak : 1.0f;
}

void toChannels (float* left, const float* right, int n, Channels mode) noexcept
{
    switch (mode)
    {
        case Channels::monoMerged:
            for (int i = 0; i < n; ++i)
                left[i] = 0.5f * (left[i] + right[i]);
            break;
        case Channels::monoRight:
            for (int i = 0; i < n; ++i)
                left[i] = right[i];
            break;
        case Channels::stereo:
        case Channels::monoLeft: break;
    }
}

std::string formatDuration (double seconds)
{
    const auto total = (long long) std::llround (std::max (0.0, seconds));
    const auto h = total / 3600, m = (total / 60) % 60, s = total % 60;
    char text[32];
    if (h > 0)
        std::snprintf (text, sizeof (text), "%lld:%02lld:%02lld", h, m, s);
    else
        std::snprintf (text, sizeof (text), "%lld:%02lld", m, s);
    return text;
}
} // namespace lynx::exportopts
