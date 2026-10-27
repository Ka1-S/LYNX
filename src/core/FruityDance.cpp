#include "core/FruityDance.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>

#include "core/Instruments.h"

namespace lynx::dance
{
namespace
{
constexpr float twoPi = 6.28318530718f;

const char* moveIds[] = { "freestyle", "waiting", "stepping", "jumping", "zombie", "waving", "hula", "windmill", "flail", "spin", "held" };

std::uint32_t mixBits (std::uint32_t x) noexcept
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float smooth (float t) noexcept { return t * t * (3.0f - 2.0f * t); }
} // namespace

const char* moveName (int move) noexcept
{
    switch (std::clamp (move, 0, moveCount - 1))
    {
        case freestyle: return "Freestyle";
        case waiting: return "Waiting";
        case stepping: return "Stepping";
        case jumping: return "Jumping";
        case zombie: return "Zombie";
        case waving: return "Waving";
        case hula: return "Hula";
        case windmill: return "Windmill";
        case flail: return "Flailing";
        case spin: return "Spinning";
        case held: return "Pose";
    }
    return "Freestyle";
}

std::vector<std::uint8_t> toState (const Settings& s)
{
    std::ostringstream o;
    char colour[16];
    std::snprintf (colour, sizeof (colour), "%08x", s.colour);
    o << "LYNX dance 1\nmove=" << moveIds[std::clamp (s.move, 0, moveCount - 1)] << "\nmirrored=" << (s.mirrored ? 1 : 0)
      << "\nvisible=" << (s.visible ? 1 : 0) << "\nscale=" << s.scale << "\nspeed=" << s.speed << "\nx=" << s.x << "\ny=" << s.y
      << "\ncolour=" << colour << "\n";
    const auto t = o.str();
    return { t.begin(), t.end() };
}

std::optional<Settings> fromState (const std::vector<std::uint8_t>& state)
{
    const std::string text (state.begin(), state.end());
    if (text.rfind ("LYNX dance", 0) != 0)
        return std::nullopt;
    Settings s;
    std::istringstream in (text);
    std::string line;
    std::getline (in, line);
    auto num = [] (const std::string& v, float lo, float hi, float fallback) {
        const auto f = std::strtof (v.c_str(), nullptr);
        return std::isfinite (f) ? std::clamp (f, lo, hi) : fallback;
    };
    while (std::getline (in, line))
    {
        if (! line.empty() && line.back() == '\r')
            line.pop_back();
        const auto eq = line.find ('=');
        if (eq == std::string::npos)
            continue;
        const auto key = line.substr (0, eq), value = line.substr (eq + 1);
        if (key == "move")
        {
            for (int m = 0; m < moveCount; ++m)
                if (value == moveIds[m])
                    s.move = m;
        }
        else if (key == "mirrored")
            s.mirrored = value != "0";
        else if (key == "visible")
            s.visible = value != "0";
        else if (key == "scale")
            s.scale = num (value, 0.25f, 3.0f, 1.0f);
        else if (key == "speed")
            s.speed = num (value, 0.125f, 4.0f, 1.0f);
        else if (key == "x")
            s.x = std::clamp (std::atoi (value.c_str()), -10000, 10000);
        else if (key == "y")
            s.y = std::clamp (std::atoi (value.c_str()), -10000, 10000);
        else if (key == "colour")
            s.colour = (std::uint32_t) std::strtoul (value.c_str(), nullptr, 16) | 0xff000000u;
    }
    return s;
}

std::optional<Settings> fromFl (const std::vector<std::uint8_t>& state)
{
    if (state.size() < 40)
        return std::nullopt;
    auto i32 = [&state] (std::size_t at) {
        std::int32_t v = 0;
        std::memcpy (&v, state.data() + at, 4);
        return v;
    };
    const auto version = i32 (0);
    if (version < 1 || version > 16)
        return std::nullopt;
    Settings s;
    // params 0 … 6 (FL: move, mirrored, shown, scale /64, speed, x, y)
    s.move = std::clamp ((int) i32 (4), 0, moveCount - 1);
    s.mirrored = i32 (8) > 0;
    s.visible = i32 (12) > 0;
    s.scale = std::clamp ((float) i32 (16) / 64.0f, 0.25f, 3.0f);
    // FL's speed: 0.5 × (1 + |v| × 3 / 96) (or divided by it): LYNX keeps the ratio to the default 0.5
    const auto v = i32 (20);
    const auto d = 1.0f + (float) std::abs (v) * 3.0f / 96.0f;
    s.speed = std::clamp (v < 0 ? 1.0f / d : d, 0.125f, 4.0f);
    s.x = std::clamp ((int) i32 (24), -10000, 10000);
    s.y = std::clamp ((int) i32 (28), -10000, 10000);
    return s;
}

Settings read (const std::vector<std::uint8_t>& state)
{
    if (auto s = fromState (state))
        return *s;
    if (auto s = fromFl (state))
        return *s;
    return {};
}

KeyAction actionForKey (int key) noexcept
{
    KeyAction a;
    if (key >= firstMoveKey && key < firstMoveKey + moveCount)
    {
        a.kind = KeyAction::Kind::move;
        a.move = key - firstMoveKey;
    }
    else if (key <= firstMirrorKey && key > firstMirrorKey - moveCount)
    {
        a.kind = KeyAction::Kind::move;
        a.move = firstMirrorKey - key;
        a.mirrored = true;
    }
    else if (key == hideKey)
        a.kind = KeyAction::Kind::hide;
    else if (key == showKey)
        a.kind = KeyAction::Kind::show;
    return a;
}

Pose Pose::mix (const Pose& a, const Pose& b, float t) noexcept
{
    auto m = [t] (float x, float y) { return x + (y - x) * t; };
    Pose p;
    p.x = m (a.x, b.x);
    p.y = m (a.y, b.y);
    p.lean = m (a.lean, b.lean);
    p.head = m (a.head, b.head);
    p.turn = m (a.turn, b.turn);
    p.squash = m (a.squash, b.squash);
    p.hipShift = m (a.hipShift, b.hipShift);
    p.tail = m (a.tail, b.tail);
    for (int i = 0; i < 2; ++i)
    {
        p.shoulder[(std::size_t) i] = m (a.shoulder[(std::size_t) i], b.shoulder[(std::size_t) i]);
        p.elbow[(std::size_t) i] = m (a.elbow[(std::size_t) i], b.elbow[(std::size_t) i]);
        p.hip[(std::size_t) i] = m (a.hip[(std::size_t) i], b.hip[(std::size_t) i]);
        p.knee[(std::size_t) i] = m (a.knee[(std::size_t) i], b.knee[(std::size_t) i]);
    }
    return p;
}

int freestyleMove (std::int64_t cycleIndex, std::uint32_t seed) noexcept
{
    // two cycles per choice; Stepping … Spinning
    const auto h = mixBits ((std::uint32_t) (cycleIndex / 2) * 2654435761u ^ seed);
    return stepping + (int) (h % (std::uint32_t) (spin - stepping + 1));
}

Pose pose (int move, double cycles, std::uint32_t seed) noexcept
{
    if (move == freestyle)
    {
        const auto index = (std::int64_t) std::floor (cycles);
        const auto now = freestyleMove (index, seed);
        const auto before = freestyleMove (index - 1, seed);
        const auto within = (float) (cycles - std::floor (cycles));
        if (now != before && (index % 2) == 0 && within < 0.25f) // a short blend into the new move
            return Pose::mix (pose (before, cycles, seed), pose (now, cycles, seed), smooth (within / 0.25f));
        return pose (now, cycles, seed);
    }
    const auto phase = (float) (cycles - std::floor (cycles)); // 0..1 within the cycle
    const auto s = std::sin (twoPi * phase), c = std::cos (twoPi * phase);
    const auto s2 = std::sin (2.0f * twoPi * phase);
    Pose p;
    // rest: arms down a little out, legs straight
    p.shoulder = { 0.25f, -0.25f };
    p.elbow = { 0.15f, -0.15f };
    p.hip = { 0.08f, -0.08f };
    switch (move)
    {
        case waiting:
        {
            const auto slow = (float) std::sin (twoPi * cycles * 0.25);
            p.y = 0.006f * s;
            p.lean = 0.03f * slow;
            p.head = 0.08f * slow;
            p.tail = 0.3f * s;
            p.shoulder = { 0.18f, -0.18f };
            // a foot taps on the beat
            p.knee = { 0.0f, phase < 0.3f ? -0.35f * std::sin (twoPi * phase / 0.6f) : 0.0f };
            p.hip[1] = phase < 0.3f ? 0.15f * std::sin (twoPi * phase / 0.6f) : -0.08f;
            break;
        }
        case stepping:
        {
            p.hipShift = 0.07f * s;
            p.x = 0.04f * s;
            p.y = 0.02f * std::abs (s2);
            p.lean = -0.08f * s;
            p.head = 0.1f * s;
            p.hip = { 0.12f + 0.18f * std::max (0.0f, s), -0.12f + 0.18f * std::min (0.0f, s) };
            p.knee = { -0.5f * std::max (0.0f, s), 0.5f * std::max (0.0f, -s) };
            p.shoulder = { 0.3f - 0.45f * s, -0.3f - 0.45f * s };
            p.elbow = { 0.6f, -0.6f };
            p.tail = -0.5f * s;
            break;
        }
        case jumping:
        {
            const auto up = std::abs (std::sin (0.5f * twoPi * phase * 2.0f)); // two hops a cycle
            const auto hop = up * up;
            p.y = 0.16f * hop;
            p.squash = 1.0f - 0.12f * (1.0f - up) * (1.0f - up);
            p.shoulder = { 0.5f + 2.2f * hop, -0.5f - 2.2f * hop };
            p.elbow = { 0.3f * (1.0f - hop), -0.3f * (1.0f - hop) };
            p.hip = { 0.2f, -0.2f };
            p.knee = { -0.9f * (1.0f - up), 0.9f * (1.0f - up) };
            p.tail = 0.8f * hop;
            break;
        }
        case zombie:
        {
            p.x = 0.03f * s;
            p.lean = 0.12f + 0.05f * s;
            p.head = 0.35f + 0.1f * c;
            p.shoulder = { 1.5f + 0.1f * s, -1.5f + 0.1f * c }; // arms out in front
            p.elbow = { 0.05f, -0.05f };
            p.hip = { 0.1f + 0.2f * std::max (0.0f, s), -0.1f + 0.2f * std::min (0.0f, s) };
            p.knee = { -0.3f * std::max (0.0f, s), 0.3f * std::max (0.0f, -s) };
            p.y = -0.01f;
            p.tail = 0.1f * s;
            break;
        }
        case waving:
        {
            p.lean = 0.06f * s;
            p.head = -0.12f + 0.08f * s;
            p.shoulder = { 2.6f, -0.9f }; // left arm up, right on the hip
            p.elbow = { 0.5f + 0.6f * s2, -1.9f };
            p.hipShift = 0.03f * s;
            p.y = 0.012f * std::abs (s);
            p.tail = 0.6f * s2;
            break;
        }
        case hula:
        {
            p.hipShift = 0.09f * s;
            p.y = 0.015f * c;
            p.lean = -0.12f * s;
            p.head = 0.12f * s;
            p.shoulder = { 1.5f + 0.25f * s, -1.5f + 0.25f * s }; // arms out, waving
            p.elbow = { 0.6f * c, -0.6f * c };
            p.knee = { -0.25f, 0.25f };
            p.hip = { 0.15f, -0.15f };
            p.tail = 0.9f * s;
            break;
        }
        case windmill:
        {
            const auto a = twoPi * phase;
            p.shoulder = { a, -1.2f + 0.8f * std::sin (a + 1.5f) }; // one arm circles
            p.elbow = { 0.0f, -0.4f };
            p.lean = 0.1f * std::sin (a);
            p.y = 0.03f * std::abs (std::sin (a));
            p.hip = { 0.2f, -0.2f };
            p.knee = { -0.3f * std::abs (s), 0.3f * std::abs (s) };
            p.tail = std::sin (a);
            break;
        }
        case flail:
        {
            // fast and a bit random: arms and legs everywhere
            auto jitter = [seed, cycles] (int k) {
                const auto t = (float) (cycles * 4.0 + k * 0.37);
                const auto i = (std::uint32_t) std::floor (t);
                const auto f = smooth (t - std::floor (t));
                auto r = [seed, k] (std::uint32_t n) { return (float) (mixBits (n * 7919u + (std::uint32_t) k * 104729u + seed) & 0xffff) / 65535.0f * 2.0f - 1.0f; };
                return r (i) + (r (i + 1) - r (i)) * f;
            };
            p.shoulder = { 1.5f + 1.3f * jitter (1), -1.5f + 1.3f * jitter (2) };
            p.elbow = { 1.0f * jitter (3), 1.0f * jitter (4) };
            p.hip = { 0.4f * jitter (5), 0.4f * jitter (6) };
            p.knee = { -0.6f * std::abs (jitter (7)), 0.6f * std::abs (jitter (8)) };
            p.lean = 0.15f * jitter (9);
            p.head = 0.3f * jitter (10);
            p.y = 0.05f * std::abs (jitter (11));
            p.x = 0.04f * jitter (12);
            p.tail = 1.2f * jitter (13);
            break;
        }
        case spin:
        {
            p.turn = std::cos (twoPi * phase);
            p.shoulder = { 1.55f, -1.55f }; // arms out
            p.elbow = { 0.1f * s, -0.1f * s };
            p.head = 0.15f * s;
            p.hip = { 0.05f, -0.05f };
            p.y = 0.01f * std::abs (s2);
            p.tail = 1.0f;
            break;
        }
        case held:
        {
            p.shoulder = { 2.4f, -2.4f }; // arms up in a V
            p.elbow = { -0.1f, 0.1f };
            p.hip = { 0.3f, -0.3f };
            p.head = -0.1f;
            p.y = 0.005f * s;
            p.tail = 0.4f;
            break;
        }
        default: break;
    }
    return p;
}

// ---------------------------------------------------------------------------------------------------------------
// The silent instrument
// ---------------------------------------------------------------------------------------------------------------
void Signals::push (int key) noexcept
{
    const auto n = count.load (std::memory_order_relaxed);
    ring[n % ring.size()].store (key, std::memory_order_relaxed);
    count.store (n + 1, std::memory_order_release);
}

int Signals::pull (std::uint32_t& seen, int* keys, int max) const noexcept
{
    const auto n = count.load (std::memory_order_acquire);
    auto from = seen;
    if (n - from > (std::uint32_t) ring.size())
        from = n - (std::uint32_t) ring.size();
    int got = 0;
    for (auto i = from; i != n && got < max; ++i)
        keys[got++] = ring[i % ring.size()].load (std::memory_order_relaxed);
    seen = n;
    return got;
}

namespace
{
class DanceInstrument final : public Instrument
{
public:
    void prepare (double) noexcept override {}
    void noteOn (int key, float) noexcept override { signals.push (key); }
    void noteOff (int) noexcept override {}
    void allNotesOff() noexcept override {}
    void render (float*, float*, int, float, float) noexcept override {}

    Signals signals;
};
} // namespace

std::unique_ptr<Instrument> makeInstrument()
{
    return std::make_unique<DanceInstrument>();
}

Signals* signalsOf (Instrument* instrument) noexcept
{
    auto* d = dynamic_cast<DanceInstrument*> (instrument);
    return d != nullptr ? &d->signals : nullptr;
}
} // namespace lynx::dance
