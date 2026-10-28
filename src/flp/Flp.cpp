#include "flp/Flp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace flp
{
namespace
{
// Event IDs (names describe the meaning, they do not follow any API)
enum : std::uint32_t
{
    evSwing = 0x0B,
    evPanLaw = 0x17,
    evTimeSignatureNumerator = 0x11,
    evChannelSwingMix = 0x61,
    evLayerChild = 0x5E,  // u16: a Layer channel's child (one event per child; REA_rodada164a §1)
    evLayerFlags = 0x90,  // u32: a Layer channel's options (REA_rodada164a §1)
    evChannelTimeShift = 0x59,  // u16: Misc › Time › Shift, FL's time-knob value (REA round 146b)
    evChannelEcho = 0xD1,       // i32 feedback, pan, pitch, echoes, time (REA round 146b)
    evChannelEchoModXY = 0x8A,  // u16 mod X, u16 mod Y of the echoes (128 = none)
    evChannelTracking = 0xE4,   // i32 mid, pan, mod X, mod Y: velocity tracking, then key tracking
    evChannelCutGroups = 0x84,
    evSampleRootNote = 0x87,
    evChannelFineTune = 0x8E, // REA round 151 [DATA: FL 2026's Misc › Fine tune +15 / +78 cents saved as 15 / 78]
    evFileFlags = 0xA9,
    evChannelParams = 0xD7,
    evChannelEnvelope = 0xDA,
    evPluginState = 0xD5,       // plugin blob (channel generator or slot effect)
    evChannelPolyphony = 0xDD,
    evChannelType = 0x15,
    evChannelEnabled = 0x00,    // u8: 0 = the channel is switched off in the Channel Rack
    evPluginNew = 0xD4,         // plugin record: i32 mixer insert (or channel), i32 slot, i32 kind (2 = effect), ...
    evChannelMixerTrackOld = 0x16, // u8, old files
    evNewChannel = 0x40,
    evNewPattern = 0x41,
    evPatternLoopOn = 0x1A,        // u8: the Channel Rack's Loop button (REA_rodada165f-rack §1)
    evPatternLoopMode = 0xA2,      // u32: loop type 1..3
    evPatternLoopChannel = 0xA0,   // u32: FL channel of the next 0xA1
    evPatternLoopSetting = 0xA1,   // i32: its loop point
    evTempoCoarse = 0x42,       // legacy: integer BPM
    evArrangement = 0x63,
    evPerformanceMode = 0x1F,   // u8: Playlist › Options › Performance mode on (round 165m, all 6 demos in "Performance mode" = 1)
    evChannelMixerTrack = 0x68,    // u16
    evPatternColour = 0x96,     // u32 0x00BBGGRR; 0x565148 = default gray
    evTempo = 0x9C,             // BPM * 1000
    evChannelNameOld = 0xC0,    // FL <= 11
    evPatternName = 0xC1,
    evTitle = 0xC2,
    evComments = 0xC3,
    evSamplePath = 0xC4,
    evUrl = 0xC5,
    evVersion = 0xC7,
    evPluginInternalName = 0xC9,
    evChannelName = 0xCB,
    evMixerInsertName = 0xCC,
    evGenre = 0xCE,
    evArtist = 0xCF,
    evPatternNotes = 0xE0,
    evInitControls = 0xD8,      // 12 B records {u32 tick, u32 controlID, i32 value}: reset at song start
    evPatternEvents = 0xDF,     // same records: the current pattern's events (event editor)
    evControlValues = 0xE1,     // 12 B records {u32 ?, u32 controlID, i32 value}
    evAutomationLink = 0xE3,    // 20 B records {u32 channel<<16, u32 0, u32 controlID, u32 flags, u32 ?}
    evAutomationPoints = 0xEA,  // header 21 B + 24 B per point + 112 B
    evMixerRoutes = 0xEB,       // one byte per destination track
    evChannelLevels = 0xDB,
    evChannelLevelAdjust = 0xE5, // i32 panAdj, i32 volAdj, i32 ?, i32 modXAdj, i32 modYAdj
    evChannelColour = 0x80,
    evChannelFilterGroup = 0x91, // u32: index into the 0xE7 names (the Channel Rack's filter group, REA round 104)
    evFilterGroupName = 0xE7,    // UTF-16 text: one per filter group, in order
    evMixerInsertInput = 0x9A,  // i32 "Audio input source" of the insert opened by the last 0xEC: -1 none, else FL's list index (REA round 157c)
    evMixerInsertOutput = 0x93, // i32 "Audio output target" of the insert opened by the last 0xEC: -1 none, 0 first pair (REA round 156)
    evMixerInsertColour = 0x95, // u32 0x00BBGGRR; comes before the 0xEC that opens the insert
    evMixerSlotIndex = 0x62,    // u16: closes the effect slot described by the preceding events     // u32 0x00BBGGRR (also appears in mixer slots)     // i32 pan, i32 volume, i32 pitch (cents), ...
    evPlaylistTrack = 0xEE,     // u32 index (1..500), u32 colour, u32 icon, u8 enabled, ...
    evPlaylistTrackName = 0xEF,
    evPlaylistItems = 0xE9,
    evMixerInsertParams = 0xEC, // opens a new mixer insert: f32 manual latency (ms), u32 flags, f32 input latency (ms)
    evMixerInsertLatencyBypass = 0x31, // u8 "Bypass track latency compensation" of the insert opened by the last 0xEC (REA round 165f-mixer)
    evMixerInsertRecordSource = 0xA5,  // u32 record source point 0..5 of the insert opened by the last 0xEC (REA round 165f-mixer)
    evArrangementName = 0xF1,
    evTimeMarker = 0x94,          // u32 type << 24 | position
    evTimeMarkerNumerator = 0x21, // u8
    evTimeMarkerDenominator = 0x22,
    evTimeMarkerName = 0xCD,
};

constexpr std::size_t noteRecordSize = 24;
constexpr std::size_t automationHeaderSize = 21;
constexpr std::size_t automationPointSize = 24;
constexpr std::uint16_t playlistPatternBase = 20480;

constexpr std::uint32_t version (std::uint32_t major, std::uint32_t minor, std::uint32_t patch)
{
    return major * 1'000'000u + minor * 1'000u + patch;
}

template <typename T>
T readLE (std::span<const std::uint8_t> s, std::size_t offset)
{
    T v {};
    std::memcpy (&v, s.data() + offset, sizeof (T)); // x86/ARM are little-endian
    return v;
}

void appendUtf8 (std::string& out, std::uint32_t cp)
{
    if (cp < 0x80)
        out += (char) cp;
    else if (cp < 0x800)
    {
        out += (char) (0xC0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += (char) (0xE0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
    else
    {
        out += (char) (0xF0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3F));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
}

std::string decodeUtf16 (std::span<const std::uint8_t> s)
{
    std::string out;
    out.reserve (s.size() / 2);

    for (std::size_t i = 0; i + 1 < s.size(); i += 2)
    {
        std::uint32_t cp = readLE<std::uint16_t> (s, i);

        if (cp == 0)
            break;

        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < s.size())
        {
            const std::uint32_t low = readLE<std::uint16_t> (s, i + 2);

            if (low >= 0xDC00 && low < 0xE000)
            {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }

        appendUtf8 (out, cp);
    }

    return out;
}

std::string decodeAnsi (std::span<const std::uint8_t> s)
{
    std::string out;

    for (auto b : s)
    {
        if (b == 0)
            break;

        appendUtf8 (out, b); // treat as Latin-1
    }

    return out;
}

template <typename T>
T& findOrAdd (std::vector<T>& items, int index)
{
    for (auto& item : items)
        if (item.index == index)
            return item;

    auto& item = items.emplace_back();
    item.index = index;
    return item;
}
} // namespace

// ---------------------------------------------------------------------------

EventReader::EventReader (std::span<const std::uint8_t> file)
    : bytes (file)
{
    if (bytes.size() < 22 || std::memcmp (bytes.data(), "FLhd", 4) != 0)
        throw Error ("not a .flp file (missing FLhd header)");

    const auto headerSize = readLE<std::uint32_t> (bytes, 4);

    if (headerSize < 6 || 8 + (std::size_t) headerSize + 8 > bytes.size())
        throw Error ("invalid FLhd header");

    hdr.format = readLE<std::uint16_t> (bytes, 8);
    hdr.channelCount = readLE<std::uint16_t> (bytes, 10);
    hdr.ppq = readLE<std::uint16_t> (bytes, 12);

    pos = 8 + headerSize;

    if (std::memcmp (bytes.data() + pos, "FLdt", 4) != 0)
        throw Error ("missing FLdt chunk");

    pos += 8; // the declared FLdt size is not reliable; we read to the end of the file
}

bool EventReader::next (Event& out)
{
    if (pos >= bytes.size())
        return false;

    auto need = [this] (std::size_t n)
    {
        if (pos + n > bytes.size())
            throw Error ("file truncated at offset " + std::to_string (pos));
    };

    out = {};
    out.id = bytes[pos++];

    if (out.id == 0xAC) // escape: 32-bit extended ID
    {
        need (4);
        out.id = readLE<std::uint32_t> (bytes, pos);
        pos += 4;
    }

    switch (out.sizeClass())
    {
        case 0:
            need (1);
            out.value = bytes[pos];
            pos += 1;
            break;

        case 1:
            need (2);
            out.value = readLE<std::uint16_t> (bytes, pos);
            pos += 2;
            break;

        case 2:
            need (4);
            out.value = readLE<std::uint32_t> (bytes, pos);
            pos += 4;
            break;

        default:
        {
            std::uint64_t size = 0;

            for (int shift = 0;; shift += 7)
            {
                need (1);

                if (shift > 28)
                    throw Error ("invalid size varint at offset " + std::to_string (pos));

                const auto b = bytes[pos++];
                size |= (std::uint64_t) (b & 0x7F) << shift;

                if ((b & 0x80) == 0)
                    break;
            }

            need ((std::size_t) size);
            out.data = bytes.subspan (pos, (std::size_t) size);
            pos += (std::size_t) size;
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------

std::uint32_t parseVersionNumber (const std::string& text)
{
    std::uint32_t parts[3] = {};
    int part = 0;

    for (char c : text)
    {
        if (c == '.')
        {
            if (++part == 3)
                break;
        }
        else if (c >= '0' && c <= '9')
            parts[part] = parts[part] * 10 + (std::uint32_t) (c - '0');
        else
            break;
    }

    return version (parts[0], parts[1], parts[2]);
}

double Channel::envelopeTime (int v) noexcept
{
    return v < 101 ? 0.0 : 0.064 * (std::pow (1000.0, v / 65536.0) - 1.0);
}

ControlId normalizeControlId (std::uint32_t raw)
{
    // before 24.2.99 the mixer class was 0x2... (FL adds 0x50000000 on load) and the
    // sends were at 0x1F40 + destination. Tracks >= 64 overflow into 0x3... (same arithmetic).
    if ((raw >> 28) == 2 || (raw >> 28) == 3)
    {
        raw += 0x50000000u;
        const auto p = raw & 0xFFFF;

        if (p >= 0x1F40 && p < 0x1FC0)
            raw = (raw & 0xFFFF0000u) | (std::uint32_t) (ControlId::mixerSendBase + (int) (p - 0x1F40));
    }

    return { raw };
}

std::size_t playlistRecordSize (std::uint32_t versionNumber, std::size_t payloadSize, std::uint32_t flagsA9)
{
    // REA_rodada2 §4.2: before 20.99.1 the record is 32 B; after it the file flags (0xA9) choose the layout:
    // bit 1 off -> 60 B, bit 1 on and bit 3 off -> 80 B (24.2 ... 25.2), bits 1 and 3 on -> 88 B (25.2.99, 26.x).
    // Files without 0xA9 fall back to the sizes seen in the demos for their version.
    std::size_t preferred = 32;

    if (versionNumber >= version (20, 99, 1))
    {
        if ((flagsA9 & 2) != 0)
            preferred = (flagsA9 & 8) != 0 ? 88 : 80;
        else if (flagsA9 != 0)
            preferred = 60;
        else
            preferred = versionNumber < version (24, 2, 99) ? 60 : 80;
    }

    if (payloadSize % preferred == 0)
        return preferred;

    for (std::size_t size : { 88u, 80u, 60u, 32u })
        if (payloadSize % size == 0)
            return size;

    return 0;
}

const Pattern* Project::findPattern (int index) const noexcept
{
    auto it = std::find_if (patterns.begin(), patterns.end(), [index] (auto& p) { return p.index == index; });
    return it != patterns.end() ? &*it : nullptr;
}

const Channel* Project::findChannel (int index) const noexcept
{
    auto it = std::find_if (channels.begin(), channels.end(), [index] (auto& c) { return c.index == index; });
    return it != channels.end() ? &*it : nullptr;
}

std::string readSavedVersion (std::span<const std::uint8_t> head)
{
    auto u32 = [&head] (std::size_t at) {
        return (std::uint32_t) head[at] | (std::uint32_t) head[at + 1] << 8 | (std::uint32_t) head[at + 2] << 16 | (std::uint32_t) head[at + 3] << 24;
    };
    if (head.size() < 22 || std::memcmp (head.data(), "FLhd", 4) != 0)
        return {};
    std::size_t p = 8 + (std::size_t) u32 (4);
    if (p + 8 > head.size() || std::memcmp (head.data() + p, "FLdt", 4) != 0)
        return {};
    p += 8;
    for (int count = 0; count < 64 && p < head.size(); ++count)
    {
        const auto id = head[p++];
        if (id < 0x40)
            p += 1;
        else if (id < 0x80)
            p += 2;
        else if (id < 0xC0)
            p += 4;
        else
        {
            std::size_t n = 0;
            for (int shift = 0; p < head.size() && shift < 35; shift += 7)
            {
                const auto b = head[p++];
                n |= (std::size_t) (b & 0x7F) << shift;
                if ((b & 0x80) == 0)
                    break;
            }
            if (p + n > head.size())
                return {};
            if (id == evVersion)
            {
                std::string v;
                for (std::size_t i = 0; i < n && head[p + i] != 0; ++i)
                    v += (char) head[p + i];
                return v;
            }
            p += n;
        }
    }
    return {};
}

Project parseProject (std::span<const std::uint8_t> file, bool channelPreset)
{
    EventReader reader (file);
    Project project;
    project.header = reader.header();

    if (project.header.format != 0 && ! channelPreset)
        project.warnings.push_back ("format " + std::to_string (project.header.format) + " is not a project");

    // Events are contextual: a NewChannel/NewPattern/Arrangement determines
    // who the following events belong to, until the next selector.
    enum class Section { global, channel, arrangement, mixer } section = Section::global;
    int currentChannel = -1, currentPattern = -1, currentArrangement = -1;

    if (channelPreset)
    {
        section = Section::channel;
        currentChannel = 0;
    }
    bool hasFineTempo = false;
    bool hasTimeSignature = false;
    std::string pendingInsertName;
    std::string pendingSlotPlugin;
    std::vector<std::uint8_t> pendingSlotState;
    int pendingSlotIndex = -1; // from 0xD4: FL <= 11 files have no 0x62, the next slot/route/insert closes the slot
    int pendingSlotFlags = -1; // from 0xD4: the wrapper flags (u16 at +16)
    auto closeOldSlot = [&] {
        if (pendingSlotIndex >= 0 && ! pendingSlotPlugin.empty() && ! project.mixer.empty())
            project.mixer.back().slots.push_back ({ pendingSlotIndex, pendingSlotPlugin, std::move (pendingSlotState), pendingSlotFlags });
        if (pendingSlotIndex >= 0)
        {
            pendingSlotPlugin.clear();
            pendingSlotState.clear();
        }
        pendingSlotIndex = -1;
        pendingSlotFlags = -1;
    };
    std::uint32_t pendingInsertColour = 0;

    auto text = [&] (const Event& e)
    {
        return project.versionNumber > version (11, 5, 4) ? decodeUtf16 (e.data) : decodeAnsi (e.data);
    };
    auto channel = [&]() -> Channel& { return findOrAdd (project.channels, currentChannel); };
    auto pattern = [&]() -> Pattern& { return findOrAdd (project.patterns, currentPattern); };
    auto arrangement = [&]() -> Arrangement& { return findOrAdd (project.arrangements, currentArrangement); };

    Event e;

    while (reader.next (e))
    {
        switch (e.id)
        {
            case evVersion:
                project.version = decodeAnsi (e.data); // always ASCII: comes before we know the version
                project.versionNumber = parseVersionNumber (project.version);
                break;

            case evPerformanceMode:
                project.performanceMode = e.value != 0;
                break;

            case evTempo:
                project.tempo = e.value / 1000.0;
                hasFineTempo = true;
                break;

            case evSwing:
                project.swing = (int) e.value;
                break;

            case evPanLaw:
                project.panLaw = (int) e.value;
                break;

            case evFileFlags:
                project.flagsA9 = e.value;
                break;

            case evChannelSwingMix:
                if (section == Section::channel)
                    channel().swingMix = (int) e.value;
                break;

            case evLayerChild:
                if (section == Section::channel)
                    channel().layerChildren.push_back ((int) e.value);
                break;

            case evLayerFlags:
                if (section == Section::channel)
                    channel().layerFlags = (int) e.value;
                break;

            case evChannelTimeShift:
                if (section == Section::channel)
                    channel().timeShift = (int) e.value;
                break;

            case evChannelEchoModXY:
                if (section == Section::channel)
                {
                    channel().echoModX = (int) (e.value & 0xFFFF);
                    channel().echoModY = (int) ((e.value >> 16) & 0xFFFF);
                }
                break;

            case evChannelEcho:
                if (section == Section::channel && e.data.size() >= 20)
                {
                    auto& c = channel();
                    c.echoFeedback = readLE<std::int32_t> (e.data, 0);
                    c.echoPan = readLE<std::int32_t> (e.data, 4);
                    c.echoPitch = readLE<std::int32_t> (e.data, 8);
                    c.echoes = readLE<std::int32_t> (e.data, 12);
                    c.echoTime = readLE<std::int32_t> (e.data, 16);
                }
                break;

            case evChannelTracking:
                if (section == Section::channel && e.data.size() >= 16)
                {
                    auto& c = channel();
                    if (c.trackingCount < 2)
                    {
                        auto& t = c.tracking[(std::size_t) c.trackingCount++];
                        t.present = true;
                        t.mid = readLE<std::int32_t> (e.data, 0);
                        t.pan = readLE<std::int32_t> (e.data, 4);
                        t.modX = readLE<std::int32_t> (e.data, 8);
                        t.modY = readLE<std::int32_t> (e.data, 12);
                    }
                }
                break;

            case evChannelCutGroups:
                if (section == Section::channel)
                {
                    channel().cutGroup = (int) (e.value & 0xFFFF);
                    channel().cutBy = (int) (e.value >> 16);
                }
                break;

            case evSampleRootNote:
                if (section == Section::channel)
                    channel().rootNote = (int) e.value;
                break;

            case evChannelFineTune:
                if (section == Section::channel)
                    channel().fineTune = (int) (std::int32_t) e.value;
                break;

            case evChannelPolyphony:
                if (section == Section::channel && e.data.size() >= 4)
                {
                    auto& c = channel();
                    c.maxVoices = readLE<std::int32_t> (e.data, 0);

                    if (e.data.size() >= 9)
                    {
                        c.slideTime = readLE<std::int32_t> (e.data, 4);
                        c.monoFlag = (e.data[8] & 1) != 0;
                        c.portaFlag = (e.data[8] & 2) != 0;
                    }
                }
                break;

            case evChannelEnvelope:
                if (section == Section::channel && e.data.size() >= 32)
                {
                    auto& c = channel();

                    if (c.envelopeCount < 5)
                    {
                        auto& env = c.envelopes[(std::size_t) c.envelopeCount];
                        env.present = true;
                        env.flags = readLE<std::int32_t> (e.data, 0);
                        env.enabled = readLE<std::int32_t> (e.data, 4) != 0;
                        env.delay = readLE<std::int32_t> (e.data, 8);
                        env.attack = readLE<std::int32_t> (e.data, 12);
                        env.hold = readLE<std::int32_t> (e.data, 16);
                        env.decay = readLE<std::int32_t> (e.data, 20);
                        env.sustain = readLE<std::int32_t> (e.data, 24);
                        env.release = readLE<std::int32_t> (e.data, 28);
                        env.rawSize = (int) std::min<std::size_t> (e.data.size(), env.raw.size());
                        std::copy_n (e.data.begin(), env.rawSize, env.raw.begin());

                        if (e.data.size() >= 36)
                            env.amount = readLE<std::int32_t> (e.data, 32);

                        if (e.data.size() >= 0x38)
                        {
                            env.lfoDelay = readLE<std::int32_t> (e.data, 0x24);
                            env.lfoAttack = readLE<std::int32_t> (e.data, 0x28);
                            env.lfoAmount = readLE<std::int32_t> (e.data, 0x2C);
                            env.lfoSpeed = readLE<std::int32_t> (e.data, 0x30);
                            env.lfoShape = readLE<std::int32_t> (e.data, 0x34);
                        }
                    }

                    ++c.envelopeCount;
                }
                break;

            case evChannelParams:
                if (section == Section::channel && e.data.size() >= 0x70)
                {
                    auto& c = channel();
                    // Misc tab (REA round 146b [EXEC: FL 2026 saves with one control changed, and renders]): delay
                    // flags (byte 10: 2 ping pong, 4 fat mode), main pitch (11), arpeggiator (40 direction, 44 range,
                    // 48 chord, 52 time, 56 gate, 60 slide, 92 repeat), full porta (62), add to key (63), time gate
                    // (u16 at 64), pitch range (112)
                    c.delayFlags = e.data[10];
                    c.mainPitch = e.data[11] != 0;
                    c.arpDirection = readLE<std::int32_t> (e.data, 40);
                    c.arpRange = readLE<std::int32_t> (e.data, 44);
                    c.arpChord = readLE<std::int32_t> (e.data, 48);
                    c.arpTime = readLE<std::int32_t> (e.data, 52);
                    c.arpGate = readLE<std::int32_t> (e.data, 56);
                    c.arpSlide = e.data[60] != 0;
                    c.fullPorta = e.data[62] != 0;
                    c.addToKey = e.data[63] != 0;
                    c.timeGate = readLE<std::uint16_t> (e.data, 64);
                    // key region (+0x44 low, +0x48 high, inclusive; default 0 / 256 = every key) and the Layer's Fade
                    // (+0x4C, 0…256) [REA_rodada164a DECOMP 0xD29370, 0xFF8F20]
                    c.keyRegionLow = readLE<std::int32_t> (e.data, 0x44);
                    c.keyRegionHigh = readLE<std::int32_t> (e.data, 0x48);
                    c.layerFade = readLE<std::int32_t> (e.data, 0x4C);
                    c.arpRepeat = readLE<std::int32_t> (e.data, 92);
                    if (e.data.size() >= 116)
                        c.pitchRange = readLE<std::int32_t> (e.data, 112);
                    c.stretchTimeRaw = readLE<std::uint32_t> (e.data, 0x60);
                    c.stretchPitch = readLE<std::int32_t> (e.data, 0x64);
                    c.stretchMul = readLE<std::int32_t> (e.data, 0x68);
                    c.stretchMode = readLE<std::int32_t> (e.data, 0x6C);

                    if (e.data.size() >= 0x98)
                    {
                        c.sampleStart = readLE<double> (e.data, 0x80);
                        c.sampleEnd = readLE<double> (e.data, 0x88);
                        c.startOffset = readLE<double> (e.data, 0x90);
                    }
                }
                break;

            case evTimeSignatureNumerator:
                if (! hasTimeSignature && e.value >= 1 && e.value <= 32)
                {
                    project.beatsPerBar = (int) e.value;
                    hasTimeSignature = true; // the following ones belong to patterns
                }
                break;

            case evTempoCoarse:
                if (! hasFineTempo)
                    project.tempo = e.value;
                break;

            case evTitle: project.title = text (e); break;
            case evComments: project.comments = text (e); break;
            case evArtist: project.artist = text (e); break;
            case evGenre: project.genre = text (e); break;
            case evUrl: project.url = text (e); break;

            case evNewPattern:
                currentPattern = (int) e.value;
                pattern();
                break;

            case evPatternName:
                if (currentPattern >= 0)
                    pattern().name = text (e);
                break;

            case evPatternLoopOn:
                if (currentPattern >= 0)
                    pattern().loopOn = e.value != 0;
                break;

            case evPatternLoopMode:
                if (currentPattern >= 0 && e.value >= 1 && e.value <= 3)
                    pattern().loopMode = (int) e.value;
                break;

            case evPatternLoopChannel:
                if (currentPattern >= 0)
                    pattern().channelLoops.emplace_back ((int) e.value, 0);
                break;

            case evPatternLoopSetting:
                if (currentPattern >= 0 && ! pattern().channelLoops.empty())
                    pattern().channelLoops.back().second = (int) (std::int32_t) e.value;
                break;

            case evPatternColour:
                if (currentPattern >= 0)
                    pattern().colour = ((e.value & 0xFF) << 16) | (e.value & 0xFF00) | ((e.value >> 16) & 0xFF);
                break;

            case evPatternNotes:
            {
                if (currentPattern < 0)
                    break;

                if (e.data.size() % noteRecordSize != 0)
                {
                    project.warnings.push_back ("notes of pattern " + std::to_string (currentPattern)
                                                + " have an unexpected size; ignored");
                    break;
                }

                auto& notes = pattern().notes;

                for (std::size_t o = 0; o < e.data.size(); o += noteRecordSize)
                {
                    Note n;
                    n.position = readLE<std::uint32_t> (e.data, o + 0);
                    n.flags = readLE<std::uint16_t> (e.data, o + 4);
                    n.channel = readLE<std::uint16_t> (e.data, o + 6);
                    n.length = readLE<std::uint32_t> (e.data, o + 8);
                    n.key = readLE<std::uint16_t> (e.data, o + 12);
                    n.group = readLE<std::uint16_t> (e.data, o + 14);
                    n.finePitch = e.data[o + 16];
                    n.repeats = e.data[o + 17];
                    n.extra = e.data[o + 19];
                    n.release = e.data[o + 18];
                    n.pan = e.data[o + 20];
                    n.velocity = e.data[o + 21];
                    n.modX = e.data[o + 22];
                    n.modY = e.data[o + 23];
                    notes.push_back (n);
                }
                break;
            }

            case evNewChannel:
                section = Section::channel;
                currentChannel = (int) e.value;
                channel();
                break;

            case evChannelEnabled:
                if (section == Section::channel)
                    channel().enabled = e.value != 0;
                break;

            case evPluginNew:
                // 13 × u32; the u16 at +16 holds the wrapper flags (PyFLP's WrapperEvent; REA round 139: 0x10 generator,
                // 0x20 "Smart disable", 0x40 threaded, 0x100 hide settings)
                if (section == Section::mixer && e.data.size() >= 8)
                {
                    pendingSlotIndex = readLE<std::int32_t> (e.data, 4);
                    pendingSlotFlags = e.data.size() >= 18 ? (int) readLE<std::uint16_t> (e.data, 16) : -1;
                }
                else if (section == Section::channel && e.data.size() >= 18)
                    channel().wrapperFlags = (int) readLE<std::uint16_t> (e.data, 16);
                break;

            case evChannelType:
                if (section == Section::channel)
                    channel().type = e.value <= 5 ? (ChannelType) e.value : ChannelType::unknown;
                break;

            case evChannelMixerTrackOld:
                if (section == Section::channel && channel().mixerTrack < 0)
                    channel().mixerTrack = (int) e.value;
                break;

            case evChannelMixerTrack:
                if (section == Section::channel)
                    channel().mixerTrack = (int) e.value;
                break;

            case evChannelName:
                if (section == Section::channel)
                    channel().name = text (e);
                break;

            case evChannelNameOld:
                if (section == Section::channel && channel().name.empty())
                    channel().name = text (e);
                break;

            case evPluginInternalName:
                // appears in channels (generator) and in mixer slots (effect, closed by 0x62)
                if (section == Section::channel && channel().pluginName.empty())
                    channel().pluginName = text (e);
                else if (section == Section::mixer)
                {
                    closeOldSlot();
                    pendingSlotPlugin = text (e);
                }
                break;

            case evMixerSlotIndex:
                if (section == Section::mixer && ! project.mixer.empty() && ! pendingSlotPlugin.empty())
                    project.mixer.back().slots.push_back ({ (int) e.value, pendingSlotPlugin, std::move (pendingSlotState), pendingSlotFlags });
                pendingSlotPlugin.clear();
                pendingSlotState.clear();
                pendingSlotIndex = -1;
                pendingSlotFlags = -1;
                break;

            case evPluginState:
                if (section == Section::channel && channel().pluginState.empty())
                    channel().pluginState.assign (e.data.begin(), e.data.end());
                else if (section == Section::mixer)
                    pendingSlotState.assign (e.data.begin(), e.data.end());
                break;

            case evSamplePath:
                if (section == Section::channel)
                    channel().samplePath = text (e);
                break;

            case evArrangement:
                section = Section::arrangement;
                currentArrangement = (int) e.value;
                arrangement();
                break;

            case evArrangementName:
                if (section == Section::arrangement)
                    arrangement().name = text (e);
                break;

            case evTimeMarker:
                if (currentArrangement < 0)
                {
                    currentArrangement = 0;
                    arrangement();
                }
                arrangement().markers.push_back ({ e.value & 0xFFFFFFu, (int) (e.value >> 24), 4, 4, {} });
                break;

            case evTimeMarkerNumerator:
            case evTimeMarkerDenominator:
                if (currentArrangement >= 0 && ! arrangement().markers.empty())
                    (e.id == evTimeMarkerNumerator ? arrangement().markers.back().numerator : arrangement().markers.back().denominator)
                        = (int) e.value;
                break;

            case evTimeMarkerName:
                if (currentArrangement >= 0 && ! arrangement().markers.empty())
                    arrangement().markers.back().name = text (e);
                break;

            case evPlaylistItems:
            {
                if (currentArrangement < 0)
                {
                    currentArrangement = 0;
                    arrangement(); // old files do not have the arrangement event
                }

                const auto recordSize = playlistRecordSize (project.versionNumber, e.data.size(), project.flagsA9);

                if (recordSize == 0)
                {
                    project.warnings.push_back ("playlist items with unexpected size; ignored");
                    break;
                }

                auto& items = arrangement().items;
                // The playlist went from 199 to 500 tracks in FL 12.9.
                const int trackCount = project.versionNumber < version (12, 9, 0) ? 199 : 500;

                for (std::size_t o = 0; o + recordSize <= e.data.size(); o += recordSize)
                {
                    PlaylistItem item;
                    item.position = readLE<std::uint32_t> (e.data, o + 0);
                    const auto base = readLE<std::uint16_t> (e.data, o + 4);
                    const auto index = readLE<std::uint16_t> (e.data, o + 6);
                    item.length = readLE<std::uint32_t> (e.data, o + 8);
                    const auto reversedTrack = readLE<std::uint16_t> (e.data, o + 12);
                    item.flags = readLE<std::uint16_t> (e.data, o + 18);
                    item.startOffset = readLE<float> (e.data, o + 24);
                    item.endOffset = readLE<float> (e.data, o + 28);
                    item.patternStartTick = readLE<std::int32_t> (e.data, o + 24);
                    item.patternEndTick = readLE<std::int32_t> (e.data, o + 28);

                    if (recordSize >= 60)
                    {
                        // REA_rodada2 §4.2 (+0x24 fade-in, +0x2C fade-out, +0x34 clip gain); fades in ms
                        // (e.g. 923.08 = 2 beats at 130 BPM in a 26.1 project).
                        const auto finiteOr = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };
                        item.fadeInMs = std::max (0.0f, finiteOr (readLE<float> (e.data, o + 0x24), 0.0f));
                        item.fadeOutMs = std::max (0.0f, finiteOr (readLE<float> (e.data, o + 0x2C), 0.0f));
                        item.gain = std::max (0.0f, finiteOr (readLE<float> (e.data, o + 0x34), 1.0f));
                    }

                    const auto patternBase = base != 0 ? base : playlistPatternBase;

                    if (index > patternBase)
                    {
                        item.kind = PlaylistItem::Kind::pattern;
                        item.index = index - patternBase;
                    }
                    else
                    {
                        item.kind = PlaylistItem::Kind::channelClip;
                        item.index = index;
                    }

                    item.track = reversedTrack < trackCount ? trackCount - 1 - reversedTrack : (int) reversedTrack;
                    items.push_back (item);
                }
                break;
            }

            case evChannelFilterGroup:
                if (section == Section::channel)
                    channel().filterGroup = (int) (std::int32_t) e.value;
                break;

            case evFilterGroupName:
                project.filterGroups.push_back (text (e));
                break;

            case evChannelColour:
                if (section == Section::channel)
                    channel().colour = ((e.value & 0xFF) << 16) | (e.value & 0xFF00) | ((e.value >> 16) & 0xFF);
                break;

            case evChannelLevels:
                if (section == Section::channel && e.data.size() >= 12)
                {
                    auto& c = channel();
                    c.hasLevels = true;
                    c.pan = readLE<std::int32_t> (e.data, 0);
                    c.volume = readLE<std::int32_t> (e.data, 4);
                    c.pitchCents = readLE<std::int32_t> (e.data, 8);

                    if (e.data.size() >= 24)
                    {
                        c.modX = readLE<std::int32_t> (e.data, 12);
                        c.modY = readLE<std::int32_t> (e.data, 16);
                        c.filterType = readLE<std::int32_t> (e.data, 20);
                    }
                }
                break;

            case evChannelLevelAdjust:
                if (section == Section::channel && e.data.size() >= 8)
                {
                    auto& c = channel();
                    // files older than 5.3.7 store these at 1/100 scale (REA_rodada3 §2)
                    const auto scale = project.versionNumber != 0 && project.versionNumber < version (5, 3, 7) ? 100 : 1;
                    c.hasLevelAdjust = true;
                    c.panAdj = readLE<std::int32_t> (e.data, 0) * scale;
                    c.volAdj = readLE<std::int32_t> (e.data, 4) * scale;
                }
                break;

            case evPlaylistTrack:
                if (currentArrangement >= 0 && e.data.size() >= 13)
                {
                    PlaylistTrackInfo t;
                    t.index = (int) readLE<std::uint32_t> (e.data, 0) - 1;
                    const auto c = readLE<std::uint32_t> (e.data, 4);
                    t.colour = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
                    t.enabled = e.data[12] != 0;
                    // FL 20+ layout (70 bytes; REA round 104): f32 size at 13, u8 "grouped with above" at 46,
                    // u8 "group expanded" at 61 (cleared when FL collapses the group)
                    if (e.data.size() >= 17)
                    {
                        const auto h = readLE<float> (e.data, 13);
                        if (std::isfinite (h) && h > 0.05f && h < 10.0f)
                            t.heightScale = h;
                    }
                    if (e.data.size() >= 46)
                    {
                        t.motion = (int) readLE<std::uint32_t> (e.data, 22);
                        t.press = (int) readLE<std::uint32_t> (e.data, 26);
                        t.triggerSync = (int) readLE<std::uint32_t> (e.data, 30);
                        t.queue = readLE<std::uint32_t> (e.data, 34) != 0;
                        t.tolerant = readLE<std::uint32_t> (e.data, 38) != 0;
                        t.positionSync = (int) readLE<std::uint32_t> (e.data, 42);
                    }
                    if (e.data.size() >= 62)
                    {
                        t.grouped = e.data[46] != 0;
                        t.expanded = e.data[61] != 0;
                        t.mode = (int) readLE<std::uint32_t> (e.data, 49);
                        t.mixerTrack = readLE<std::int32_t> (e.data, 53);
                        t.channel = readLE<std::int32_t> (e.data, 57);
                    }
                    arrangement().tracks.push_back (t);
                }
                break;

            case evPlaylistTrackName:
                if (currentArrangement >= 0 && ! arrangement().tracks.empty())
                    arrangement().tracks.back().name = text (e);
                break;

            case evInitControls:
            case evPatternEvents:
            {
                std::vector<ControlEvent> list;
                for (std::size_t o = 0; o + 12 <= e.data.size(); o += 12)
                    list.push_back ({ readLE<std::uint32_t> (e.data, o), normalizeControlId (readLE<std::uint32_t> (e.data, o + 4)),
                                      readLE<std::int32_t> (e.data, o + 8) });
                if (e.id == evInitControls)
                    project.initControls.insert (project.initControls.end(), list.begin(), list.end());
                else if (currentPattern >= 0)
                    pattern().controlEvents.insert (pattern().controlEvents.end(), list.begin(), list.end());
                break;
            }

            case evControlValues:
                for (std::size_t o = 0; o + 12 <= e.data.size(); o += 12)
                    project.controls.push_back ({ normalizeControlId (readLE<std::uint32_t> (e.data, o + 4)),
                                                  readLE<std::int32_t> (e.data, o + 8) });
                break;

            case evAutomationLink:
                for (std::size_t o = 0; o + 20 <= e.data.size(); o += 20)
                    if ((readLE<std::uint32_t> (e.data, o + 12) & 8) != 0) // 8 = automation clip (4 = remote control)
                    {
                        const auto source = readLE<std::uint32_t> (e.data, o);
                        const auto target = normalizeControlId (readLE<std::uint32_t> (e.data, o + 8));

                        if ((source & 0xFFFFu) >= 0x8000u) // output of a controller, not a clip
                            project.controllerLinks.push_back ({ normalizeControlId (source), target });
                        else
                            project.automationLinks.push_back ({ (int) (source >> 16), target });
                    }
                break;

            case evAutomationPoints:
            {
                if (section != Section::channel || e.data.size() < automationHeaderSize)
                    break;

                const auto count = readLE<std::uint32_t> (e.data, 17);

                if (automationHeaderSize + (std::size_t) count * automationPointSize > e.data.size())
                {
                    project.warnings.push_back ("automation of channel " + std::to_string (currentChannel) + " truncated; ignored");
                    break;
                }

                auto& points = channel().automation;
                points.clear();
                double position = 0.0;

                for (std::uint32_t i = 0; i < count; ++i)
                {
                    const auto o = automationHeaderSize + (std::size_t) i * automationPointSize;
                    AutomationPoint p;
                    position += readLE<double> (e.data, o); // distance to the previous point
                    p.position = position;
                    p.value = readLE<double> (e.data, o + 8);
                    p.tension = readLE<float> (e.data, o + 16);
                    p.curve = e.data[o + 20];
                    points.push_back (p);
                }
                break;
            }

            case evMixerRoutes:
                if (section == Section::mixer)
                    closeOldSlot();
                if (section == Section::mixer && ! project.mixer.empty())
                {
                    auto& insert = project.mixer.back();
                    insert.hasRoutes = true;
                    insert.routes.clear();

                    for (std::size_t i = 0; i < e.data.size(); ++i)
                    {
                        if (e.data[i] == 0)
                            continue;

                        // old files had fewer tracks; the extra ones ("Current") become 500/501
                        auto target = (int) i;
                        if (project.versionNumber < version (8, 5, 0) && target > 64)
                            target += 35;
                        else if (project.versionNumber < version (12, 9, 0) && target > 103)
                            target += 397;
                        else if (project.versionNumber < version (24, 2, 99) && target > 125)
                            target += 375;

                        insert.routes.push_back (target);
                    }
                }
                break;

            case evMixerInsertOutput:
                if (section == Section::mixer && ! project.mixer.empty())
                    project.mixer.back().output = (int) (std::int32_t) e.value;
                break;

            case evMixerInsertLatencyBypass:
                if (section == Section::mixer && ! project.mixer.empty())
                    project.mixer.back().latencyBypass = (int) std::min<std::uint32_t> (e.value, 2);
                break;

            case evMixerInsertRecordSource:
                if (section == Section::mixer && ! project.mixer.empty())
                    project.mixer.back().recordSource = (int) std::min<std::uint32_t> (e.value, 5);
                break;

            case evMixerInsertInput:
                if (section == Section::mixer && ! project.mixer.empty())
                    project.mixer.back().input = std::max (-1, (int) (std::int32_t) e.value);
                break;

            case evMixerInsertColour:
                pendingInsertColour = ((e.value & 0xFF) << 16) | (e.value & 0xFF00) | ((e.value >> 16) & 0xFF);
                break;

            case evMixerInsertName:
                // The name comes before the event that opens the insert it belongs to
                // (confirmed by cross-checking with the channel routing in the demos).
                pendingInsertName = text (e);
                break;

            case evMixerInsertParams:
            {
                if (section == Section::mixer)
                    closeOldSlot();
                section = Section::mixer;
                auto& insert = project.mixer.emplace_back();
                insert.index = (int) project.mixer.size() - 1;
                insert.name = std::move (pendingInsertName);
                pendingInsertName.clear();
                insert.colour = pendingInsertColour;
                pendingInsertColour = 0;

                if (e.data.size() >= 8)
                    insert.flags = readLE<std::uint32_t> (e.data, 4);
                if (e.data.size() >= 4) // FL's writer FUN_00f3d620: the track's manual latency in ms (REA round 165f-mixer)
                {
                    const auto bits = readLE<std::uint32_t> (e.data, 0);
                    float ms = 0.0f;
                    std::memcpy (&ms, &bits, sizeof ms);
                    insert.latencyMs = std::isfinite (ms) ? ms : 0.0f;
                }
                break;
            }

            default:
                break;
        }
    }

    if (section == Section::mixer)
        closeOldSlot();

    // the stretch time is int in files without bit 0 of 0xA9, float in newer ones
    for (auto& ch : project.channels)
    {
        if ((project.flagsA9 & 1) == 0)
            ch.stretchTime = (float) (std::int32_t) ch.stretchTimeRaw;
        else
            std::memcpy (&ch.stretchTime, &ch.stretchTimeRaw, sizeof (float));

        if (! std::isfinite (ch.stretchTime) || ch.stretchTime < 0.0f)
            ch.stretchTime = 0.0f;
    }

    for (auto& p : project.patterns)
        std::stable_sort (p.notes.begin(), p.notes.end(), [] (auto& a, auto& b) { return a.position < b.position; });

    return project;
}
} // namespace flp
