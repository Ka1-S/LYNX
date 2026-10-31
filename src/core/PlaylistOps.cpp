#include "core/PlaylistOps.h"

#include <algorithm>
#include <cstdio>
#include <limits>

namespace lynx::clipprops
{
namespace
{
bool validSource (const Song& song, ClipKind kind, int source)
{
    switch (kind)
    {
        case ClipKind::pattern: return source >= 0 && source < (int) song.patterns.size();
        case ClipKind::audio:
            return source >= 0 && source < (int) song.channels.size() && song.channels[(std::size_t) source].instrument == InstrumentKind::audioClip;
        case ClipKind::automation: return source >= 0 && source < (int) song.automations.size();
    }
    return false;
}
} // namespace

bool read (const Song& song, int clip, Props& out)
{
    if (clip < 0 || clip >= (int) song.playlist.size())
        return false;
    const auto& c = song.playlist[(std::size_t) clip];
    Props p;
    p.kind = c.kind;
    p.track = c.track;
    p.start = c.start;
    p.length = c.length;
    p.offset = c.offset;
    p.gain = c.gain;
    p.pan = c.pan;
    p.fadeIn = c.fadeIn;
    p.fadeOut = c.fadeOut;
    p.muted = c.muted;

    if (c.kind == ClipKind::pattern && validSource (song, c.kind, c.pattern))
    {
        const auto& pat = song.patterns[(std::size_t) c.pattern];
        p.source = c.pattern;
        p.name = pat.name;
        p.colour = pat.colour;
    }
    else if (c.kind == ClipKind::audio && c.channel >= 0 && c.channel < (int) song.channels.size())
    {
        const auto& ch = song.channels[(std::size_t) c.channel];
        p.source = c.channel;
        p.name = ch.name;
        p.colour = ch.colour;
        p.pitch = ch.sampler.pitch;
        p.stretchBeats = ch.sampler.stretchBeats;
        p.keepPitch = ch.sampler.keepPitch;
    }
    else if (c.kind == ClipKind::automation && validSource (song, c.kind, c.automation))
    {
        p.source = c.automation;
        p.name = song.automations[(std::size_t) c.automation].name;
    }
    out = p;
    return true;
}

bool apply (Song& song, int clip, const Props& p)
{
    if (clip < 0 || clip >= (int) song.playlist.size())
        return false;
    auto& c = song.playlist[(std::size_t) clip];

    if (p.kind == c.kind && validSource (song, c.kind, p.source))
    {
        if (c.kind == ClipKind::pattern)
            c.pattern = p.source;
        else if (c.kind == ClipKind::audio)
            c.channel = p.source;
        else
            c.automation = p.source;
    }

    c.track = std::clamp (p.track, 0, 499);
    c.start = std::max<std::int64_t> (0, p.start);
    c.length = std::max<std::int64_t> (1, p.length);
    if (c.offset != p.offset)
        c.offsetFraction = 0.0f; // an offset typed in ticks drops the imported sub-tick rest
    c.offset = std::max<std::int64_t> (0, p.offset);
    c.muted = p.muted;

    if (c.kind == ClipKind::audio)
    {
        c.gain = std::clamp (p.gain, 0.0f, 4.0f);
        c.pan = std::clamp (p.pan, -1.0f, 1.0f);
        c.fadeIn = std::clamp<std::int64_t> (p.fadeIn, 0, c.length);
        c.fadeOut = std::clamp<std::int64_t> (p.fadeOut, 0, c.length - c.fadeIn);
    }

    // the source's name and colour (and the audio channel's pitch / stretch)
    if (c.kind == ClipKind::pattern && validSource (song, c.kind, c.pattern))
    {
        auto& pat = song.patterns[(std::size_t) c.pattern];
        if (! p.name.empty())
            pat.name = p.name;
        pat.colour = p.colour;
    }
    else if (c.kind == ClipKind::audio && c.channel >= 0 && c.channel < (int) song.channels.size())
    {
        auto& ch = song.channels[(std::size_t) c.channel];
        if (! p.name.empty())
            ch.name = p.name;
        ch.colour = p.colour;
        ch.sampler.pitch = std::clamp (p.pitch, -24.0f, 24.0f);
        ch.sampler.stretchBeats = std::max (0.0f, p.stretchBeats);
        ch.sampler.keepPitch = p.keepPitch; // (round 163: also "keep length" for a pitched clip without stretch)
    }
    else if (c.kind == ClipKind::automation && validSource (song, c.kind, c.automation) && ! p.name.empty())
        song.automations[(std::size_t) c.automation].name = p.name;

    return true;
}

void changeMute (Song& song, const std::vector<int>& clips, MuteChange change)
{
    for (auto i : clips)
        if (i >= 0 && i < (int) song.playlist.size())
        {
            auto& m = song.playlist[(std::size_t) i].muted;
            switch (change)
            {
                case MuteChange::muteAll: m = true; break;
                case MuteChange::unmuteAll: m = false; break;
                case MuteChange::flip: m = ! m; break;
                case MuteChange::keep: break;
            }
        }
}

void changeGain (Song& song, const std::vector<int>& clips, float value, bool relative)
{
    for (auto i : clips)
        if (i >= 0 && i < (int) song.playlist.size() && song.playlist[(std::size_t) i].kind == ClipKind::audio)
        {
            auto& g = song.playlist[(std::size_t) i].gain;
            g = std::clamp (relative ? g * value : value, 0.0f, 4.0f);
        }
}

namespace
{
std::string bbt (std::int64_t ticks, int beatsPerBar, int base)
{
    const auto beat = (std::int64_t) ppq;
    const auto bar = beat * std::max (1, beatsPerBar);
    const auto t = std::max<std::int64_t> (0, ticks);
    char text[48];
    std::snprintf (text, sizeof (text), "%lld:%lld:%03lld", (long long) (t / bar + base), (long long) ((t % bar) / beat + base), (long long) (t % beat));
    return text;
}

std::int64_t parseBbt (const std::string& text, int beatsPerBar, int base)
{
    long long parts[3] = { 0, base, 0 };
    int count = 0;
    std::string token;
    auto flush = [&] {
        if (token.empty() || count >= 3)
            return false;
        for (auto c : token)
            if (c < '0' || c > '9')
                return false;
        parts[count++] = std::stoll (token);
        token.clear();
        return true;
    };
    for (auto c : text)
    {
        if (c == ' ')
            continue;
        if (c == ':' || c == '.')
        {
            if (! flush())
                return -1;
        }
        else
            token += c;
    }
    if (! flush())
        return -1;
    if (parts[0] < base || parts[1] < base || parts[1] - base >= std::max (1, beatsPerBar) || parts[2] >= ppq)
        return -1;
    const auto beat = (std::int64_t) ppq;
    return (parts[0] - base) * beat * std::max (1, beatsPerBar) + (parts[1] - base) * beat + parts[2];
}
} // namespace

std::string formatPosition (std::int64_t ticks, int beatsPerBar) { return bbt (ticks, beatsPerBar, 1); }
std::string formatLength (std::int64_t ticks, int beatsPerBar) { return bbt (ticks, beatsPerBar, 0); }
std::int64_t parsePosition (const std::string& text, int beatsPerBar) { return parseBbt (text, beatsPerBar, 1); }
std::int64_t parseLength (const std::string& text, int beatsPerBar) { return parseBbt (text, beatsPerBar, 0); }

std::vector<std::pair<int, std::string>> sourcesFor (const Song& song, ClipKind kind)
{
    std::vector<std::pair<int, std::string>> out;
    if (kind == ClipKind::pattern)
        for (int i = 0; i < (int) song.patterns.size(); ++i)
        {
            if (song.patterns[(std::size_t) i].name == "(tail)")
                continue;
            out.emplace_back (i, song.patterns[(std::size_t) i].name);
        }
    else if (kind == ClipKind::audio)
    {
        for (int i = 0; i < (int) song.channels.size(); ++i)
            if (song.channels[(std::size_t) i].instrument == InstrumentKind::audioClip)
                out.emplace_back (i, song.channels[(std::size_t) i].name);
    }
    else
        for (int i = 0; i < (int) song.automations.size(); ++i)
            out.emplace_back (i, song.automations[(std::size_t) i].name);
    return out;
}
} // namespace lynx::clipprops

namespace lynx::arrangeops
{
const std::vector<Clip>& clipsOf (const Song& song, int index)
{
    static const std::vector<Clip> none;
    if (index == song.currentArrangement)
        return song.playlist;
    if (index < 0 || index >= (int) song.arrangements.size())
        return none;
    return song.arrangements[(std::size_t) index].clips;
}

std::int64_t lengthOf (const Song& song, int index)
{
    std::int64_t end = 0;
    for (const auto& c : clipsOf (song, index))
        end = std::max (end, c.end());
    return end;
}

std::int64_t positionTick (const Song& song, MergePosition position, std::int64_t currentTick)
{
    switch (position)
    {
        case MergePosition::start: return 0;
        case MergePosition::current: return std::max<std::int64_t> (0, currentTick);
        case MergePosition::end:
        {
            const auto end = lengthOf (song, song.currentArrangement);
            const auto bar = (std::int64_t) ppq * std::max (1, song.beatsPerBar);
            return (end + bar - 1) / bar * bar; // the next bar line
        }
    }
    return 0;
}

void clearRange (std::vector<Clip>& clips, std::int64_t from, std::int64_t to)
{
    if (to <= from)
        return;
    std::vector<Clip> out;
    for (auto c : clips)
    {
        if (c.end() <= from || c.start >= to)
        {
            out.push_back (c);
            continue;
        }
        if (c.start < from) // keep the head
        {
            auto head = c;
            splitClip (head, from);
            out.push_back (head);
        }
        if (c.end() > to) // keep the end
        {
            auto copy = c;
            out.push_back (splitClip (copy, to));
        }
    }
    clips = std::move (out);
}

bool merge (Song& song, int source, MergeMode mode, std::int64_t at)
{
    if (source == song.currentArrangement || source < 0 || source >= (int) song.arrangements.size())
        return false;

    const auto incoming = song.arrangements[(std::size_t) source].clips; // copy: the playlist may grow
    const auto length = lengthOf (song, source);
    at = std::max<std::int64_t> (0, at);

    if (mode == MergeMode::insert && length > 0)
    {
        // what crosses the position is split; everything after it moves later
        std::vector<Clip> moved;
        for (auto c : song.playlist)
        {
            if (c.start < at && c.end() > at)
            {
                auto rest = splitClip (c, at);
                rest.start += length;
                moved.push_back (c);
                moved.push_back (rest);
            }
            else
            {
                if (c.start >= at)
                    c.start += length;
                moved.push_back (c);
            }
        }
        song.playlist = std::move (moved);
        for (auto& m : song.markers())
            if (m.tick >= at)
                m.tick += length;
    }
    else if (mode == MergeMode::replace)
        clearRange (song.playlist, at, at + length);

    for (auto c : incoming)
    {
        c.start += at;
        song.playlist.push_back (c);
    }

    std::stable_sort (song.playlist.begin(), song.playlist.end(), [] (const Clip& a, const Clip& b) {
        return a.start != b.start ? a.start < b.start : a.track < b.track;
    });
    return true;
}

void move (Song& song, int from, int to)
{
    const auto n = (int) song.arrangements.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to)
        return;
    auto item = std::move (song.arrangements[(std::size_t) from]);
    song.arrangements.erase (song.arrangements.begin() + from);
    song.arrangements.insert (song.arrangements.begin() + to, std::move (item));

    auto& cur = song.currentArrangement;
    if (cur == from)
        cur = to;
    else if (from < cur && to >= cur)
        --cur;
    else if (from > cur && to <= cur)
        ++cur;
}

void rename (Song& song, int index, const std::string& name)
{
    if (index >= 0 && index < (int) song.arrangements.size() && ! name.empty())
        song.arrangements[(std::size_t) index].name = name;
}

int duplicate (Song& song, int index)
{
    if (index < 0 || index >= (int) song.arrangements.size())
        return -1;
    Arrangement copy = song.arrangements[(std::size_t) index];
    if (index == song.currentArrangement)
    {
        copy.clips = song.playlist;
        copy.loopStart = song.loopStart;
        copy.loopEnd = song.loopEnd;
    }
    copy.name += " (copy)";
    song.arrangements.insert (song.arrangements.begin() + index + 1, std::move (copy));
    if (song.currentArrangement > index)
        ++song.currentArrangement;
    return index + 1;
}
} // namespace lynx::arrangeops

namespace lynx::trackmodes
{
namespace
{
PlaylistTrack* trackAt (Song& song, int track)
{
    if (track < 0 || track >= 500)
        return nullptr;
    if ((int) song.playlistTracks.size() <= track)
        song.playlistTracks.resize ((std::size_t) track + 1);
    return &song.playlistTracks[(std::size_t) track];
}

bool linkedElsewhere (const Song& song, int mixer, int exceptTrack)
{
    for (int i = 0; i < (int) song.playlistTracks.size(); ++i)
        if (i != exceptTrack && song.playlistTracks[(std::size_t) i].mode != PlaylistTrackMode::unassigned
            && song.playlistTracks[(std::size_t) i].mixerTrack == mixer)
            return true;
    return false;
}

std::string trackName (const Song& song, int track)
{
    if (track < (int) song.playlistTracks.size() && ! song.playlistTracks[(std::size_t) track].name.empty())
        return song.playlistTracks[(std::size_t) track].name;
    return "Track " + std::to_string (track + 1);
}
} // namespace

int freeMixerTrack (Song& song)
{
    const auto isFree = [&song] (int t) {
        const auto& m = song.mixer[(std::size_t) t];
        if (! m.effects.empty() || linkedElsewhere (song, t, -1))
            return false;
        for (const auto& c : song.channels)
            if (c.mixerTrack == t)
                return false;
        for (int from = 1; from < (int) song.mixer.size(); ++from)
            if (from != t)
                for (const auto& s : song.mixer[(std::size_t) from].sends)
                    if (s.target == t)
                        return false;
        return true;
    };
    for (int t = 1; t < (int) song.mixer.size(); ++t)
        if (isFree (t))
            return t;
    if (song.mixer.empty() || (int) song.mixer.size() >= maxMixerTracks)
        return -1;
    song.mixer.emplace_back().name = "Insert " + std::to_string (song.mixer.size());
    return (int) song.mixer.size() - 1;
}

bool makeAudioTrack (Song& song, int track)
{
    if (track < 0 || track >= 500)
        return false;
    auto mixer = track < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) track].mode == PlaylistTrackMode::audio
                     ? song.playlistTracks[(std::size_t) track].mixerTrack : -1;
    if (mixer <= 0)
        mixer = freeMixerTrack (song);
    if (mixer <= 0)
        return false;
    auto* t = trackAt (song, track);
    t->mode = PlaylistTrackMode::audio;
    t->mixerTrack = mixer;
    t->channel = -1;
    auto& m = song.mixer[(std::size_t) mixer];
    m.name = trackName (song, track);
    if (t->colour != 0)
        m.colour = t->colour;
    // the audio clips already on the track play through it from now on
    std::vector<int> onTrack;
    for (int i = 0; i < (int) song.playlist.size(); ++i)
        if (song.playlist[(std::size_t) i].track == track)
            onTrack.push_back (i);
    followAudioTracks (song, onTrack);
    return true;
}

bool makeInstrumentTrack (Song& song, int track, int channel)
{
    if (track < 0 || track >= 500 || channel < 0 || channel >= (int) song.channels.size())
        return false;
    const auto kind = song.channels[(std::size_t) channel].instrument;
    if (kind == InstrumentKind::audioClip || kind == InstrumentKind::controller)
        return false; // FL: "Only generators can be used as instrument tracks."
    // a channel belongs to one instrument track
    for (int i = 0; i < (int) song.playlistTracks.size(); ++i)
        if (i != track && song.playlistTracks[(std::size_t) i].mode == PlaylistTrackMode::instrument
            && song.playlistTracks[(std::size_t) i].channel == channel)
            unassign (song, i);
    auto mixer = song.channels[(std::size_t) channel].mixerTrack;
    if (mixer <= 0 || mixer >= (int) song.mixer.size() || linkedElsewhere (song, mixer, track))
        mixer = freeMixerTrack (song);
    if (mixer <= 0)
        return false;
    auto* t = trackAt (song, track);
    t->mode = PlaylistTrackMode::instrument;
    t->channel = channel;
    t->mixerTrack = mixer;
    song.channels[(std::size_t) channel].mixerTrack = mixer;
    if (t->name.empty())
        t->name = song.channels[(std::size_t) channel].name;
    return true;
}

int instrumentTrackPattern (Song& song, int track)
{
    if (track < 0 || track >= (int) song.playlistTracks.size())
        return -1;
    const auto& t = song.playlistTracks[(std::size_t) track];
    if (t.mode != PlaylistTrackMode::instrument || t.channel < 0 || t.channel >= (int) song.channels.size())
        return -1;
    const auto channel = t.channel;
    const auto& name = song.channels[(std::size_t) channel].name;
    for (int p = 0; p < (int) song.patterns.size(); ++p)
    {
        const auto& pattern = song.patterns[(std::size_t) p];
        if (pattern.name == name && std::all_of (pattern.notes.begin(), pattern.notes.end(), [channel] (const Note& n) { return n.channel == channel; }))
            return p;
    }
    Pattern made;
    made.name = name;
    made.colour = song.channels[(std::size_t) channel].colour;
    song.patterns.push_back (std::move (made));
    return (int) song.patterns.size() - 1;
}

void unassign (Song& song, int track)
{
    if (track < 0 || track >= (int) song.playlistTracks.size())
        return;
    auto& t = song.playlistTracks[(std::size_t) track];
    t.mode = PlaylistTrackMode::unassigned;
    t.mixerTrack = -1;
    t.channel = -1;
}

std::string describe (const Song& song, int track)
{
    if (track < 0 || track >= (int) song.playlistTracks.size())
        return {};
    const auto& t = song.playlistTracks[(std::size_t) track];
    if (t.mode == PlaylistTrackMode::instrument)
        return "(Instrument track)";
    if (t.mode == PlaylistTrackMode::audio)
        return "(Audio track, linked to mixer track " + std::to_string (t.mixerTrack) + ")";
    return {};
}

int armableInsert (const Song& song, int track)
{
    if (track < 0 || track >= (int) song.playlistTracks.size())
        return -1;
    const auto& t = song.playlistTracks[(std::size_t) track];
    return t.mode == PlaylistTrackMode::audio && t.mixerTrack > 0 && t.mixerTrack < (int) song.mixer.size() ? t.mixerTrack : -1;
}

namespace
{
int audioMixerOf (const Song& song, int track)
{
    if (track < 0 || track >= (int) song.playlistTracks.size())
        return -1;
    const auto& t = song.playlistTracks[(std::size_t) track];
    return t.mode == PlaylistTrackMode::audio && t.mixerTrack > 0 && t.mixerTrack < (int) song.mixer.size() ? t.mixerTrack : -1;
}

// the mixer track a touched clip asks for its channel, or -1 when it needs nothing (not an audio clip on an audio
// track, or its channel already plays there)
int wantedMixer (const Song& song, const Clip& clip)
{
    const auto mixer = audioMixerOf (song, clip.track);
    if (clip.kind != ClipKind::audio || mixer <= 0 || clip.channel < 0 || clip.channel >= (int) song.channels.size()
        || song.channels[(std::size_t) clip.channel].mixerTrack == mixer)
        return -1;
    return mixer;
}

// FL's test: another clip (any arrangement) of the same channel on an audio track linked to another mixer track
bool sharedWithOtherAudioTrack (const Song& song, const Clip& clip, int mixer)
{
    bool shared = false;
    song.forEachClipList ([&] (const std::vector<Clip>& clips) {
        for (const auto& other : clips)
            if (&other != &clip && other.kind == ClipKind::audio && other.channel == clip.channel)
                if (const auto m = audioMixerOf (song, other.track); m > 0 && m != mixer)
                    shared = true;
    });
    return shared;
}
} // namespace

std::vector<int> audioTrackConflicts (const Song& song, const std::vector<int>& touched)
{
    std::vector<int> channels;
    for (const auto i : touched)
    {
        if (i < 0 || i >= (int) song.playlist.size())
            continue;
        const auto& clip = song.playlist[(std::size_t) i];
        const auto mixer = wantedMixer (song, clip);
        if (mixer > 0 && std::find (channels.begin(), channels.end(), clip.channel) == channels.end()
            && sharedWithOtherAudioTrack (song, clip, mixer))
            channels.push_back (clip.channel);
    }
    return channels;
}

int followAudioTracks (Song& song, const std::vector<int>& touched, bool makeUnique)
{
    int changed = 0;
    std::vector<int> done; // (FL: the touched clips of a channel it asked about are handled together)
    for (const auto i : touched)
    {
        if (i < 0 || i >= (int) song.playlist.size() || std::find (done.begin(), done.end(), i) != done.end())
            continue;
        const auto clip = song.playlist[(std::size_t) i];
        const auto mixer = wantedMixer (song, clip);
        if (mixer <= 0)
            continue;
        if (! sharedWithOtherAudioTrack (song, song.playlist[(std::size_t) i], mixer))
        {
            song.channels[(std::size_t) clip.channel].mixerTrack = mixer; // every clip of the channel follows (FL)
            ++changed;
            continue;
        }
        // "Make clip unique?": every touched clip of this channel gets the copy (Yes) or stays as it is (No)
        auto copy = -1;
        if (makeUnique)
        {
            auto def = song.channels[(std::size_t) clip.channel];
            def.mixerTrack = mixer;
            song.channels.push_back (def);
            copy = (int) song.channels.size() - 1;
        }
        for (const auto j : touched)
            if (j >= 0 && j < (int) song.playlist.size() && song.playlist[(std::size_t) j].channel == clip.channel
                && song.playlist[(std::size_t) j].kind == ClipKind::audio && std::find (done.begin(), done.end(), j) == done.end())
            {
                done.push_back (j);
                if (copy >= 0)
                {
                    song.playlist[(std::size_t) j].channel = copy;
                    ++changed;
                }
            }
    }
    return changed;
}

bool makeConsolidateSong (const Song& song, int track, bool fromSongStart, Song& out, std::int64_t& start, std::int64_t& musicLength)
{
    std::vector<Clip> clips;
    for (const auto& c : song.playlist)
        if (c.track == track && ! c.muted)
            clips.push_back (c);
    if (clips.empty())
        return false;

    start = fromSongStart ? 0 : std::numeric_limits<std::int64_t>::max();
    std::int64_t end = 0;
    for (const auto& c : clips)
    {
        if (! fromSongStart)
            start = std::min (start, c.start);
        end = std::max (end, c.end());
    }
    // the automation clips of the other tracks play too (round 149): the tempo map (a song with tempo changes renders
    // at its own tempi) and what they automate; the stretch before `start` is cut off
    for (const auto& c : song.playlist)
        if (c.track != track && c.kind == ClipKind::automation && ! c.muted && c.end() > start && ! song.isTrackMuted (c.track))
        {
            auto a = c;
            if (a.start < start)
            {
                a.offset += start - a.start;
                a.length -= start - a.start;
                a.start = start;
            }
            clips.push_back (a);
        }
    for (auto& c : clips)
        c.start -= start;
    musicLength = end - start;

    out = song;
    out.bpm = song.tempoAt ((double) start); // the tempo where the render starts (without a tempo clip there)
    out.playlist = std::move (clips);
    for (auto& t : out.playlistTracks)
        t.muted = false;
    for (auto& t : out.mixer)
        t.solo = false;
    auto& master = out.mixer[0];
    master.effects.clear();
    master.volume = unityFader;
    master.pan = 0.0f;
    master.muted = false;
    out.mode = PlayMode::song;
    out.loopStart = out.loopEnd = 0;
    return true;
}

int applyConsolidate (Song& song, int track, const std::string& samplePath, std::int64_t start, std::int64_t length)
{
    ChannelDef c;
    c.name = trackName (song, track) + " (consolidated)";
    c.instrument = InstrumentKind::audioClip;
    c.samplePath = samplePath;
    c.volume = 1.0f;
    const auto mixer = track < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) track].mode == PlaylistTrackMode::audio
                           ? song.playlistTracks[(std::size_t) track].mixerTrack : 0;
    c.mixerTrack = mixer > 0 && mixer < (int) song.mixer.size() ? mixer : 0;
    song.channels.push_back (c);

    song.playlist.erase (std::remove_if (song.playlist.begin(), song.playlist.end(), [track] (const Clip& clip) { return clip.track == track && ! clip.muted; }),
                         song.playlist.end());
    Clip clip;
    clip.kind = ClipKind::audio;
    clip.channel = (int) song.channels.size() - 1;
    clip.track = track;
    clip.start = start;
    clip.length = std::max<std::int64_t> (1, length);
    song.playlist.push_back (clip);
    std::stable_sort (song.playlist.begin(), song.playlist.end(), [] (const Clip& a, const Clip& b) { return a.start < b.start; });
    return clip.channel;
}
} // namespace lynx::trackmodes
