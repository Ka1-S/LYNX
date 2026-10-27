#include "core/FlExtraFx.h"

// Round 165g (docs/analise/REA_rodada165g.md): Fruity Mute 2, Fruity Big Clock, Tuner, Fruity HTML NoteBook.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "core/Effects.h"

namespace lynx::flextra
{
namespace
{
std::optional<std::int32_t> i32At (const std::vector<std::uint8_t>& s, std::size_t at)
{
    if (at + 4 > s.size())
        return std::nullopt;
    std::int32_t v = 0;
    std::memcpy (&v, s.data() + at, 4);
    return v;
}

// Delphi's Round (banker's rounding at .5), as FL's plugins round a control value to a step
long roundHalfEven (double x) noexcept
{
    return (long) std::nearbyint (x);
}
} // namespace

// ================================================================================================ Fruity Mute 2
namespace mute2
{
bool soundOn (float muteValue) noexcept
{
    return ! (muteValue < 0.5f);
}

Channel channelOf (float value) noexcept
{
    const auto k = roundHalfEven ((double) (value / (1.0f / 3.0f)));
    return (Channel) std::clamp<long> (k, 0, 2);
}

float channelValue (Channel c) noexcept
{
    return (float) (int) c / 3.0f;
}

std::string paramText (int param, float value)
{
    if (param == mute)
        return soundOn (value) ? "On" : "Off";
    if (param == channel)
    {
        switch (channelOf (value))
        {
            case Channel::left: return "L";
            case Channel::both: return "L + R";
            case Channel::right: return "R";
        }
    }
    return {};
}

bool fromFl (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto version = i32At (state, 0);
    const auto m = i32At (state, 4), c = i32At (state, 8);
    if (! version || *version > 1 || ! m || ! c)
        return false;
    // the controls' raw values: the switch is 0 / 1, the channel selector 0…2 (FL's value = raw / 3 for the selector,
    // whose control spans 0…3 with the last step unused) [FL render, REA_rodada165g §1]
    slot.params[mute] = *m != 0 ? 1.0f : 0.0f;
    slot.params[channel] = channelValue ((Channel) std::clamp (*c, 0, 2));
    return true;
}

void process (float* left, float* right, int n, float muteValue, float channelVal) noexcept
{
    if (soundOn (muteValue))
        return;
    const auto c = channelOf (channelVal);
    if (c != Channel::right)
        std::fill (left, left + n, 0.0f);
    if (c != Channel::left)
        std::fill (right, right + n, 0.0f);
}

namespace
{
class Mute2Effect final : public Effect
{
public:
    void prepare (double) noexcept override {}
    void reset() noexcept override {}
    void process (float* left, float* right, int n, const EffectParams& params, double) noexcept override
    {
        mute2::process (left, right, n, params[mute], params[channel]);
    }
};
} // namespace

std::unique_ptr<Effect> make()
{
    return std::make_unique<Mute2Effect>();
}
} // namespace mute2

// ================================================================================================ Fruity Big Clock
namespace bigclock
{
Display displayOf (float value) noexcept
{
    return (Display) std::clamp ((int) std::lround (value * (displays - 1)), 0, displays - 1);
}

float displayValue (Display d) noexcept
{
    return (float) (int) d / (float) (displays - 1);
}

const char* displayName (Display d) noexcept
{
    switch (d)
    {
        case Display::barBeatTick: return "Bar:Beat:Tick";
        case Display::minSecHundredths: return "Min:Sec:Hundredths";
        case Display::barStepTick: return "Bar:Step:Tick";
    }
    return "";
}

std::string paramText (int param, float value)
{
    return param == display ? displayName (displayOf (value)) : std::string();
}

std::string clockText (const Song& song, double ticks, int beatsPerBar, Display d)
{
    ticks = std::max (0.0, ticks);
    char text[64] {};
    if (d == Display::minSecHundredths)
    {
        const auto seconds = song.secondsForTicks ((std::int64_t) ticks);
        const auto whole = (long) seconds;
        std::snprintf (text, sizeof text, "%ld:%02ld:%02d", whole / 60, whole % 60, std::clamp ((int) ((seconds - (double) whole) * 100.0), 0, 99));
        return text;
    }

    // the bar and the ticks into it (song mode: the Playlist's bars with time signature changes)
    long bar = 1;
    double intoBar = 0.0;
    if (song.mode == PlayMode::song)
    {
        const auto b = song.barAt ((std::int64_t) ticks);
        bar = b.number;
        intoBar = ticks - (double) b.tick;
    }
    else
    {
        const auto perBar = (double) ppq * std::max (1, beatsPerBar);
        bar = (long) (ticks / perBar) + 1;
        intoBar = std::fmod (ticks, perBar);
    }
    constexpr double flTicksPerQuarter = 96.0; // FL's default project resolution
    const auto toFl = flTicksPerQuarter / (double) ppq;
    if (d == Display::barBeatTick)
    {
        const auto beat = (long) (intoBar / ppq);
        const auto tick = (long) ((intoBar - (double) beat * ppq) * toFl);
        std::snprintf (text, sizeof text, "%ld:%02ld:%02ld", bar, beat + 1, tick);
    }
    else
    {
        const auto step = (long) (intoBar / ticksPerStep);
        const auto tick = (long) ((intoBar - (double) step * ticksPerStep) * toFl);
        std::snprintf (text, sizeof text, "%ld:%02ld:%02ld", bar, step + 1, tick);
    }
    return text;
}
} // namespace bigclock

// ================================================================================================ Tuner
namespace tuner
{
double referenceHz (float value) noexcept
{
    return 400.0 + 80.0 * (double) std::clamp (value, 0.0f, 1.0f);
}

float referenceValue (double hz) noexcept
{
    return (float) std::clamp ((hz - 400.0) / 80.0, 0.0, 1.0);
}

bool bassOn (float value) noexcept
{
    return value >= 0.5f;
}

Visual visualOf (float value) noexcept
{
    return value >= 0.5f ? Visual::dial : Visual::basic;
}

std::string paramText (int param, float value)
{
    char text[32] {};
    switch (param)
    {
        case reference: std::snprintf (text, sizeof text, "%.1f Hz", referenceHz (value)); return text;
        case reactivity:
            if (std::abs (value - 0.5f) < 0.01f)
                return "Average";
            std::snprintf (text, sizeof text, "%s %d%%", value < 0.5f ? "Slow" : "Fast", (int) std::lround (std::abs (value - 0.5f) * 200.0f));
            return text;
        case bass: return bassOn (value) ? "On" : "Off";
        case visual: return visualOf (value) == Visual::dial ? "Dial" : "Basic";
        default: return {};
    }
}

bool fromFl (const std::vector<std::uint8_t>& state, EffectSlot& slot)
{
    const auto version = i32At (state, 0);
    const auto ref = i32At (state, 4), react = i32At (state, 8), b = i32At (state, 12), vis = i32At (state, 16);
    if (! version || *version > 1 || ! ref || ! react || ! b || ! vis)
        return false;
    slot.params[reference] = std::clamp ((float) *ref / 6000.0f, 0.0f, 1.0f);
    slot.params[reactivity] = std::clamp (((float) *react + 1.0f) * 0.5f, 0.0f, 1.0f);
    slot.params[bass] = *b != 0 ? 1.0f : 0.0f;
    slot.params[visual] = *vis >= 2 ? 1.0f : 0.0f;
    return true;
}

const char* noteName (int midiNote) noexcept
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[((midiNote % 12) + 12) % 12];
}

int octaveOf (int midiNote) noexcept
{
    return (int) std::floor ((double) midiNote / 12.0) - 1;
}

// YIN (de Cheveigné & Kawahara 2002) on the full-rate signal: difference function, cumulative mean normalised
// difference, absolute threshold, parabolic interpolation. Runs on the caller's thread (the editor's timer).
struct Analyzer::Impl
{
    double sr = 48000.0, a4 = 440.0;
    float react = 1.0f;
    bool bass = false;
    std::vector<float> ring; // power of two
    std::size_t written = 0, sinceLast = 0;
    std::vector<float> frame, d;
    double smoothedNote = -1.0;
    int unvoiced = 0;
    std::array<double, 5> recent {};
    int recentCount = 0;

    double fmin() const { return bass ? 21.83 : 43.65; }                                  // F0 / F1
    double fmax() const { return 2637.0 * std::pow (8372.0 / 2637.0, (double) react); } // E7 (slow) … C10 (fast)
    std::size_t hop() const { return (std::size_t) std::max (256.0, sr * (bass ? 0.1 : 0.0213 + 0.02 * (1.0 - react))); }
};

Analyzer::Analyzer() : impl (std::make_unique<Impl>()) { prepare (48000.0); }
Analyzer::~Analyzer() = default;

void Analyzer::prepare (double sampleRate)
{
    impl->sr = sampleRate > 0.0 ? sampleRate : 48000.0;
    impl->ring.assign (1u << 15, 0.0f);
    impl->written = impl->sinceLast = 0;
    impl->smoothedNote = -1.0;
    impl->recentCount = 0;
    shown = {};
}

void Analyzer::setOptions (double referenceHzValue, float reactivityValue, bool bassValue)
{
    impl->a4 = referenceHzValue;
    impl->react = std::clamp (reactivityValue, 0.0f, 1.0f);
    impl->bass = bassValue;
}

void Analyzer::push (const float* mono, int n) noexcept
{
    auto& m = *impl;
    const auto mask = m.ring.size() - 1;
    for (int i = 0; i < n; ++i)
        m.ring[(m.written + (std::size_t) i) & mask] = mono[i];
    m.written += (std::size_t) std::max (0, n);
    m.sinceLast += (std::size_t) std::max (0, n);
    if (m.sinceLast < m.hop())
        return;
    m.sinceLast = 0;

    // the newest window + the longest lag
    const auto tauMax = std::min ((int) std::ceil (m.sr / m.fmin()) + 2, (int) (m.ring.size() / 2) - 4);
    const auto tauMin = std::max (2, (int) std::floor (m.sr / m.fmax()));
    const auto window = std::clamp (tauMax, 256, 1536);
    const auto total = (std::size_t) (window + tauMax + 2);
    if (m.written < total)
        return;
    m.frame.resize (total);
    for (std::size_t i = 0; i < total; ++i)
        m.frame[i] = m.ring[(m.written - total + i) & mask];

    double energy = 0.0;
    for (int j = 0; j < window; ++j)
        energy += (double) m.frame[(std::size_t) j] * m.frame[(std::size_t) j];
    const auto rms = (float) std::sqrt (energy / window);

    m.d.assign ((std::size_t) tauMax + 2, 1.0f);
    double running = 0.0;
    int best = -1;
    float bestValue = 1.0f;
    for (int tau = 1; tau <= tauMax + 1; ++tau)
    {
        double sum = 0.0;
        const float* a = m.frame.data();
        const float* b = m.frame.data() + tau;
        for (int j = 0; j < window; ++j)
        {
            const auto diff = (double) a[j] - (double) b[j];
            sum += diff * diff;
        }
        running += sum;
        m.d[(std::size_t) tau] = running > 0.0 ? (float) (sum * tau / running) : 1.0f;
    }
    constexpr float threshold = 0.15f;
    for (int tau = tauMin; tau <= tauMax; ++tau)
        if (m.d[(std::size_t) tau] < threshold)
        {
            while (tau + 1 <= tauMax && m.d[(std::size_t) tau + 1] < m.d[(std::size_t) tau])
                ++tau;
            best = tau;
            bestValue = m.d[(std::size_t) tau];
            break;
        }
    if (best < 0) // no dip under the threshold: the deepest one, if clear enough
        for (int tau = tauMin; tau <= tauMax; ++tau)
            if (m.d[(std::size_t) tau] < bestValue)
            {
                bestValue = m.d[(std::size_t) tau];
                best = tau;
            }

    const auto voiced = best > 0 && bestValue < 0.3f && rms > 0.0005f;
    if (! voiced)
    {
        // keep the last reading a moment (a note's tail), then clear the display
        if (++m.unvoiced * (double) m.hop() / m.sr > 0.35)
        {
            shown.valid = false;
            m.smoothedNote = -1.0;
            m.recentCount = 0;
        }
        shown.level = rms;
        return;
    }
    m.unvoiced = 0;

    // parabolic interpolation of the dip
    auto tau = (double) best;
    if (best > 1 && best < tauMax)
    {
        const auto s0 = (double) m.d[(std::size_t) best - 1], s1 = (double) m.d[(std::size_t) best], s2 = (double) m.d[(std::size_t) best + 1];
        const auto den = s0 + s2 - 2.0 * s1;
        if (std::abs (den) > 1e-12)
            tau += std::clamp (0.5 * (s0 - s2) / den, -1.0, 1.0);
    }
    // short periods (high notes): the interpolation's error is a fraction of a sample, cents at C8 — refine on the dip
    // of a multiple of the period and divide (the error shrinks with the multiple)
    if (tau < 64.0)
    {
        const auto multiple = std::min ((int) (128.0 / tau), (int) ((tauMax - 2) / tau));
        if (multiple >= 2)
        {
            auto at = (int) std::lround (tau * multiple);
            for (int k = at - 2; k <= at + 2; ++k)
                if (k > 1 && k < tauMax && m.d[(std::size_t) k] < m.d[(std::size_t) at])
                    at = k;
            const auto s0 = (double) m.d[(std::size_t) at - 1], s1 = (double) m.d[(std::size_t) at], s2 = (double) m.d[(std::size_t) at + 1];
            const auto den = s0 + s2 - 2.0 * s1;
            if (s1 < 0.3 && std::abs (den) > 1e-12)
                tau = ((double) at + std::clamp (0.5 * (s0 - s2) / den, -1.0, 1.0)) / multiple;
        }
    }
    const auto hz = m.sr / tau;
    const auto note = 69.0 + 12.0 * std::log2 (hz / m.a4);

    // reactivity: slow takes the median of the last estimates (ignores brief fluctuations) and averages longer
    m.recent[(std::size_t) (m.recentCount++ % (int) m.recent.size())] = note;
    auto target = note;
    if (m.react < 0.5f && m.recentCount >= 3)
    {
        const auto count = std::min (m.recentCount, (int) m.recent.size());
        std::array<double, 5> sorted = m.recent;
        std::sort (sorted.begin(), sorted.begin() + count);
        target = sorted[(std::size_t) count / 2];
    }
    const auto timeConstant = 0.03 + 0.27 * (1.0 - (double) m.react); // seconds
    const auto alpha = 1.0 - std::exp (-(double) m.hop() / (m.sr * timeConstant));
    if (m.smoothedNote < 0.0 || std::abs (target - m.smoothedNote) > 0.75) // a new note: jump
        m.smoothedNote = target;
    else
        m.smoothedNote += alpha * (target - m.smoothedNote);

    shown.valid = true;
    shown.note = (int) std::lround (m.smoothedNote);
    shown.cents = std::clamp ((m.smoothedNote - shown.note) * 100.0, -50.0, 50.0);
    shown.hz = m.a4 * std::pow (2.0, (m.smoothedNote - 69.0) / 12.0);
    shown.level = rms;
}
} // namespace tuner

// ================================================================================================ Fruity HTML NoteBook
namespace htmlnotebook
{
std::string address (const std::vector<std::uint8_t>& state)
{
    if (state.size() < 5)
        return {};
    const auto n = (std::size_t) state[4];
    if (5 + n > state.size())
        return {};
    return std::string (state.begin() + 5, state.begin() + 5 + (std::ptrdiff_t) n);
}

std::vector<std::uint8_t> stateFor (const std::string& text)
{
    const auto n = std::min<std::size_t> (text.size(), 255);
    std::vector<std::uint8_t> s { 1, 0, 0, 0, (std::uint8_t) n };
    s.insert (s.end(), text.begin(), text.begin() + (std::ptrdiff_t) n);
    return s;
}

bool isWebAddress (const std::string& a)
{
    auto lower = a;
    std::transform (lower.begin(), lower.end(), lower.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return lower.rfind ("http://", 0) == 0 || lower.rfind ("https://", 0) == 0 || lower.rfind ("www.", 0) == 0;
}

namespace
{
std::string lowerAscii (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
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

std::string decodeEntities (const std::string& s)
{
    std::string out;
    out.reserve (s.size());
    for (std::size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] != '&')
        {
            out += s[i];
            continue;
        }
        const auto end = s.find (';', i);
        if (end == std::string::npos || end - i > 10)
        {
            out += '&';
            continue;
        }
        const auto name = s.substr (i + 1, end - i - 1);
        std::uint32_t cp = 0;
        if (! name.empty() && name[0] == '#')
            cp = (std::uint32_t) std::strtoul (name.c_str() + (name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? 2 : 1), nullptr,
                                               name.size() > 1 && (name[1] == 'x' || name[1] == 'X') ? 16 : 10);
        else
        {
            static const std::pair<const char*, std::uint32_t> named[] = { { "amp", '&' },     { "lt", '<' },       { "gt", '>' },
                                                                          { "quot", '"' },    { "apos", '\'' },    { "nbsp", 0xA0 },
                                                                          { "copy", 0xA9 },   { "reg", 0xAE },     { "trade", 0x2122 },
                                                                          { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 },
                                                                          { "laquo", 0xAB },  { "raquo", 0xBB },   { "bull", 0x2022 } };
            for (const auto& [n, c] : named)
                if (name == n)
                    cp = c;
        }
        if (cp == 0)
        {
            out += '&';
            continue;
        }
        appendUtf8 (out, cp == 0xA0 ? (std::uint32_t) ' ' : cp);
        i = end;
    }
    return out;
}

std::string attribute (const std::string& tag, const char* name)
{
    const auto lower = lowerAscii (tag);
    const auto key = std::string (name) + "=";
    for (auto at = lower.find (key); at != std::string::npos; at = lower.find (key, at + 1))
    {
        if (at > 0 && ! std::isspace ((unsigned char) lower[at - 1]))
            continue;
        auto v = at + key.size();
        if (v >= tag.size())
            return {};
        const auto q = tag[v];
        if (q == '"' || q == '\'')
        {
            const auto e = tag.find (q, v + 1);
            return decodeEntities (tag.substr (v + 1, e == std::string::npos ? std::string::npos : e - v - 1));
        }
        auto e = v;
        while (e < tag.size() && ! std::isspace ((unsigned char) tag[e]) && tag[e] != '>')
            ++e;
        return decodeEntities (tag.substr (v, e - v));
    }
    return {};
}
} // namespace

Page parse (const std::string& html)
{
    Page page;
    Block current;
    bool bold = false, italic = false, pre = false, inTitle = false;
    int skip = 0; // inside <script>, <style>, <head> parts that are not shown
    std::string link;

    auto flush = [&] {
        // trim the block's runs; drop empty blocks
        if (! pre)
        {
            while (! current.runs.empty() && (current.runs.front().text.empty() || current.runs.front().text == " "))
                current.runs.erase (current.runs.begin());
            if (! current.runs.empty() && ! current.runs.front().text.empty() && current.runs.front().text[0] == ' ')
                current.runs.front().text.erase (0, 1);
            while (! current.runs.empty() && ! current.runs.back().text.empty() && current.runs.back().text.back() == ' ')
                current.runs.back().text.pop_back();
        }
        if (! current.runs.empty() || current.kind == Block::Kind::rule)
            page.blocks.push_back (current);
        current = {};
    };
    auto addText = [&] (std::string text) {
        if (text.empty())
            return;
        if (inTitle)
        {
            page.title += decodeEntities (text);
            return;
        }
        if (skip > 0)
            return;
        if (! pre)
        {
            std::string collapsed;
            for (const auto c : text)
            {
                const auto space = c == ' ' || c == '\n' || c == '\r' || c == '\t';
                if (space)
                {
                    if (collapsed.empty() || collapsed.back() != ' ')
                        collapsed += ' ';
                }
                else
                    collapsed += c;
            }
            text = collapsed;
            const auto lastSpace = ! current.runs.empty() && ! current.runs.back().text.empty() && current.runs.back().text.back() == ' ';
            if ((current.runs.empty() || lastSpace) && ! text.empty() && text[0] == ' ')
                text.erase (0, 1);
            if (text.empty())
                return;
        }
        text = decodeEntities (text);
        if (! current.runs.empty() && current.runs.back().bold == bold && current.runs.back().italic == italic && current.runs.back().link == link)
            current.runs.back().text += text;
        else
            current.runs.push_back ({ text, bold, italic, link });
    };

    std::size_t i = 0;
    while (i < html.size())
    {
        const auto lt = html.find ('<', i);
        addText (html.substr (i, lt == std::string::npos ? std::string::npos : lt - i));
        if (lt == std::string::npos)
            break;
        if (html.compare (lt, 4, "<!--") == 0)
        {
            const auto end = html.find ("-->", lt + 4);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        const auto gt = html.find ('>', lt);
        if (gt == std::string::npos)
            break;
        const auto tag = html.substr (lt + 1, gt - lt - 1);
        i = gt + 1;
        const auto closing = ! tag.empty() && tag[0] == '/';
        auto name = lowerAscii (tag.substr (closing ? 1 : 0));
        name = name.substr (0, name.find_first_of (" \t\r\n/"));

        if (name == "script" || name == "style" || name == "noscript")
        {
            if (! closing)
            {
                // skip to the closing tag
                const auto end = lowerAscii (html).find ("</" + name, i);
                i = end == std::string::npos ? html.size() : html.find ('>', end) + 1;
                if (i == 0)
                    i = html.size();
            }
            continue;
        }
        if (name == "title")
        {
            inTitle = ! closing;
            continue;
        }
        if (name == "b" || name == "strong")
            bold = ! closing;
        else if (name == "i" || name == "em")
            italic = ! closing;
        else if (name == "a")
            link = closing ? std::string() : attribute (tag, "href");
        else if (name == "br")
        {
            if (pre)
                addText ("\n");
            else
                flush();
        }
        else if (name == "hr")
        {
            flush();
            current.kind = Block::Kind::rule;
            flush();
        }
        else if (name == "h1" || name == "h2" || name == "h3" || name == "h4" || name == "h5" || name == "h6")
        {
            flush();
            if (! closing)
                current.kind = name == "h1" ? Block::Kind::heading1 : name == "h2" ? Block::Kind::heading2 : Block::Kind::heading3;
        }
        else if (name == "li")
        {
            flush();
            if (! closing)
                current.kind = Block::Kind::listItem;
        }
        else if (name == "pre")
        {
            flush();
            pre = ! closing;
            if (pre)
                current.kind = Block::Kind::preformatted;
        }
        else if (name == "p" || name == "div" || name == "ul" || name == "ol" || name == "table" || name == "tr" || name == "blockquote"
                 || name == "body" || name == "section" || name == "article" || name == "header" || name == "footer" || name == "center"
                 || name == "dd" || name == "dt")
            flush();
        else if (name == "td" || name == "th")
            addText (" ");
        else if (name == "img" && ! closing)
        {
            const auto alt = attribute (tag, "alt");
            if (! alt.empty())
                addText ("[" + alt + "]");
        }
    }
    flush();
    return page;
}
} // namespace htmlnotebook

std::string inertPluginNote (const std::string& flPluginName)
{
    if (flPluginName == "Razer Chroma")
        return "Razer Chroma (keyboard / mouse lighting; not reproduced, the slot is left out)";
    if (flPluginName == "ReWired")
        return "ReWired (ReWire is discontinued; the channel is left silent)";
    if (flPluginName == "FL Studio Mobile Rack FX")
        return "FL Studio Mobile Rack FX (its modules are not reproduced; the slot is left out)";
    if (flPluginName == "FL Studio Mobile" || flPluginName == "FL Studio Mobile Rack")
        return flPluginName + " (its instruments are not reproduced; played with a LYNX synth preset)";
    if (flPluginName == "Fruity Video Player")
        return "Fruity Video Player (the video is not played; the channel is left silent)";
    return {};
}
} // namespace lynx::flextra
