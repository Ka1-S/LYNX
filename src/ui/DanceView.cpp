#include "ui/DanceView.h"

#include <cmath>

#include "core/Instruments.h"
#include "engine/AudioEngine.h"
#include "ui/ChannelSettings.h"
#include "ui/ColourPicker.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace lynx
{
// ---------------------------------------------------------------------------------------------------------------
// The character: LYNX's own lynx (tufted ears, a bob tail, a shirt in the outfit colour), drawn from the pose with
// vector shapes. Body units: about 1.0 tall, y up, the origin between the feet.
// ---------------------------------------------------------------------------------------------------------------
namespace dancer
{
namespace
{
using P = juce::Point<float>;

const juce::Colour fur { 0xffe2b57f }, furDark { 0xffb9884f }, line { 0xff1d1612 }, cream { 0xfff6ead6 }, shorts { 0xff23262e },
    shoe { 0xfff1f1f1 }, nose { 0xffd9727a };

// a limb's direction for an angle (0 = straight down; positive turns towards the left of the picture)
P dir (float a) { return { -std::sin (a), -std::cos (a) }; }

struct Rig
{
    P feet;
    float h = 100.0f, sx = 1.0f, sy = 1.0f;
    P at (P b) const { return { feet.x + b.x * h * sx, feet.y - b.y * h * sy }; }
    float px (float units) const { return units * h; }
};

void limb (juce::Graphics& g, const Rig& r, P a, P b, P c, float width, juce::Colour fill)
{
    juce::Path p;
    p.startNewSubPath (r.at (a));
    p.lineTo (r.at (b));
    p.lineTo (r.at (c));
    const auto w = r.px (width);
    g.setColour (line);
    g.strokePath (p, juce::PathStrokeType (w + juce::jmax (2.0f, r.px (0.014f)), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.setColour (fill);
    g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

void blob (juce::Graphics& g, P centre, float rx, float ry, juce::Colour fill, float outline)
{
    const auto box = juce::Rectangle<float> (rx * 2.0f, ry * 2.0f).withCentre (centre);
    g.setColour (fill);
    g.fillEllipse (box);
    if (outline > 0.0f)
    {
        g.setColour (line);
        g.drawEllipse (box, outline);
    }
}
} // namespace

juce::Rectangle<float> reach (juce::Point<float> feet, float height)
{
    return { feet.x - height * 0.62f, feet.y - height * 1.42f, height * 1.24f, height * 1.52f };
}

void draw (juce::Graphics& g, juce::Point<float> feet, float height, const dance::Pose& pose, bool mirrored, juce::Colour outfit)
{
    // facing: a spin squeezes the body through its side (never to nothing); behind = the back of the head
    const auto facing = std::abs (pose.turn) < 0.14f ? (pose.turn < 0.0f ? -0.14f : 0.14f) : pose.turn;
    const auto back = pose.turn < 0.0f;
    Rig r { feet, height, facing * (mirrored ? -1.0f : 1.0f), pose.squash };
    const auto outline = juce::jmax (1.2f, height * 0.011f);

    // the shadow on the floor (smaller while in the air)
    {
        const auto lift = juce::jlimit (0.0f, 1.0f, pose.y * 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.28f * (1.0f - lift * 0.6f)));
        g.fillEllipse (juce::Rectangle<float> (height * 0.42f * (1.0f - lift * 0.4f), height * 0.07f).withCentre ({ feet.x + pose.x * height, feet.y }));
    }

    const P hip { pose.x + pose.hipShift, pose.y + 0.46f };
    const P up { std::sin (pose.lean), std::cos (pose.lean) }, side { std::cos (pose.lean), -std::sin (pose.lean) };
    const auto neck = hip + up * 0.30f;
    const auto shoulderL = neck - up * 0.035f - side * 0.085f, shoulderR = neck - up * 0.035f + side * 0.085f;
    const auto headUp = P { std::sin (pose.lean + pose.head), std::cos (pose.lean + pose.head) };
    const auto headSide = P { headUp.y, -headUp.x };
    const auto headC = neck + headUp * 0.105f;

    auto legPoints = [&] (int i) {
        const auto top = hip + side * (i == 0 ? -0.055f : 0.055f);
        const auto knee = top + dir (pose.hip[(std::size_t) i]) * 0.22f;
        const auto foot = knee + dir (pose.hip[(std::size_t) i] + pose.knee[(std::size_t) i]) * 0.21f;
        return std::array<P, 3> { top, knee, foot };
    };
    auto armPoints = [&] (int i) {
        const auto top = i == 0 ? shoulderL : shoulderR;
        const auto elbow = top + dir (pose.shoulder[(std::size_t) i] + pose.lean) * 0.15f;
        const auto hand = elbow + dir (pose.shoulder[(std::size_t) i] + pose.elbow[(std::size_t) i] + pose.lean) * 0.14f;
        return std::array<P, 3> { top, elbow, hand };
    };

    // the tail (a lynx's short bob), behind the body
    {
        const auto base = hip + side * 0.045f - up * 0.02f;
        const auto a = 2.0f + pose.tail * 0.5f; // out to the right and up
        const auto tip = base + dir (-a) * 0.11f;
        limb (g, r, base, (base + tip) * 0.5f, tip, 0.05f, furDark);
        blob (g, r.at (tip), r.px (0.022f), r.px (0.022f), line, 0.0f);
    }

    // legs, shoes
    for (int i = 0; i < 2; ++i)
    {
        const auto l = legPoints (i);
        limb (g, r, l[0], l[1], l[2], 0.06f, fur);
        const auto toe = r.at (l[2]) + P { (i == 0 ? -1.0f : 1.0f) * r.px (0.035f) * std::abs (r.sx), 0.0f };
        const auto shoeBox = juce::Rectangle<float> (r.px (0.11f) * std::abs (r.sx) + 4.0f, r.px (0.055f)).withCentre ((r.at (l[2]) + toe) * 0.5f);
        g.setColour (shoe);
        g.fillRoundedRectangle (shoeBox, shoeBox.getHeight() * 0.45f);
        g.setColour (line);
        g.drawRoundedRectangle (shoeBox, shoeBox.getHeight() * 0.45f, outline);
    }

    // the body: shorts, then the shirt
    {
        juce::Path pants;
        pants.startNewSubPath (r.at (hip - side * 0.095f + up * 0.05f));
        pants.lineTo (r.at (hip + side * 0.095f + up * 0.05f));
        pants.lineTo (r.at (legPoints (1)[0] + dir (pose.hip[1]) * 0.09f + side * 0.035f));
        pants.lineTo (r.at (hip + up * -0.01f));
        pants.lineTo (r.at (legPoints (0)[0] + dir (pose.hip[0]) * 0.09f - side * 0.035f));
        pants.closeSubPath();
        g.setColour (shorts);
        g.fillPath (pants);
        g.setColour (line);
        g.strokePath (pants, juce::PathStrokeType (outline, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        juce::Path shirt;
        shirt.startNewSubPath (r.at (hip - side * 0.09f + up * 0.04f));
        shirt.quadraticTo (r.at (hip - side * 0.115f + up * 0.17f), r.at (shoulderL - side * 0.02f));
        shirt.quadraticTo (r.at (neck + up * 0.02f), r.at (shoulderR + side * 0.02f));
        shirt.quadraticTo (r.at (hip + side * 0.115f + up * 0.17f), r.at (hip + side * 0.09f + up * 0.04f));
        shirt.closeSubPath();
        g.setColour (outfit);
        g.fillPath (shirt);
        // a stripe across the chest (LYNX's mark)
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (shirt);
            juce::Path stripe;
            stripe.startNewSubPath (r.at (hip - side * 0.2f + up * 0.16f));
            stripe.lineTo (r.at (hip + side * 0.2f + up * 0.2f));
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (stripe, juce::PathStrokeType (r.px (0.022f)));
        }
        g.setColour (line);
        g.strokePath (shirt, juce::PathStrokeType (outline, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // arms, paws
    for (int i = 0; i < 2; ++i)
    {
        const auto a = armPoints (i);
        limb (g, r, a[0], a[1], a[2], 0.05f, fur);
        blob (g, r.at (a[2]), r.px (0.034f), r.px (0.034f), cream, outline);
    }

    // the head: ears with tufts, the face
    {
        for (int i = 0; i < 2; ++i)
        {
            const auto s = i == 0 ? -1.0f : 1.0f;
            const auto baseIn = headC + headUp * 0.055f + headSide * (s * 0.02f);
            const auto baseOut = headC + headUp * 0.035f + headSide * (s * 0.085f);
            const auto tip = headC + headUp * 0.15f + headSide * (s * 0.07f);
            juce::Path ear;
            ear.startNewSubPath (r.at (baseIn));
            ear.lineTo (r.at (tip));
            ear.lineTo (r.at (baseOut));
            ear.closeSubPath();
            g.setColour (fur);
            g.fillPath (ear);
            g.setColour (line);
            g.strokePath (ear, juce::PathStrokeType (outline, juce::PathStrokeType::mitered));
            // the tuft
            juce::Path tuft;
            tuft.startNewSubPath (r.at (tip));
            tuft.lineTo (r.at (tip + headUp * 0.05f + headSide * (s * 0.01f)));
            g.strokePath (tuft, juce::PathStrokeType (juce::jmax (1.5f, r.px (0.014f)), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        const auto c = r.at (headC);
        blob (g, c, r.px (0.095f) * std::abs (r.sx), r.px (0.088f), fur, outline);
        // cheek ruffs (a lynx's side whiskers)
        for (int i = 0; i < 2; ++i)
        {
            const auto s = i == 0 ? -1.0f : 1.0f;
            juce::Path ruff;
            ruff.startNewSubPath (r.at (headC + headSide * (s * 0.08f) + headUp * 0.01f));
            ruff.lineTo (r.at (headC + headSide * (s * 0.125f) - headUp * 0.045f));
            ruff.lineTo (r.at (headC + headSide * (s * 0.06f) - headUp * 0.06f));
            ruff.closeSubPath();
            g.setColour (cream);
            g.fillPath (ruff);
            g.setColour (line);
            g.strokePath (ruff, juce::PathStrokeType (outline * 0.8f, juce::PathStrokeType::mitered));
        }
        if (! back)
        {
            // muzzle, eyes, nose
            blob (g, r.at (headC - headUp * 0.035f), r.px (0.05f) * std::abs (r.sx), r.px (0.032f), cream, 0.0f);
            for (int i = 0; i < 2; ++i)
            {
                const auto s = i == 0 ? -1.0f : 1.0f;
                const auto e = r.at (headC + headUp * 0.015f + headSide * (s * 0.038f));
                blob (g, e, r.px (0.014f) * std::abs (r.sx) + 0.6f, r.px (0.019f), line, 0.0f);
                g.setColour (juce::Colours::white);
                g.fillEllipse (juce::Rectangle<float> (r.px (0.008f), r.px (0.008f)).withCentre (e + P { -r.px (0.004f), -r.px (0.006f) }));
            }
            juce::Path n;
            const auto nc = r.at (headC - headUp * 0.022f);
            const auto nw = r.px (0.018f) * std::abs (r.sx) + 0.8f;
            n.addTriangle (nc.x - nw, nc.y - nw * 0.6f, nc.x + nw, nc.y - nw * 0.6f, nc.x, nc.y + nw * 0.6f);
            g.setColour (nose);
            g.fillPath (n);
        }
        else
        {
            // the back of the head: a darker patch
            blob (g, c + P { 0.0f, r.px (0.01f) }, r.px (0.06f) * std::abs (r.sx), r.px (0.05f), furDark, 0.0f);
        }
    }
}
} // namespace dancer

// ---------------------------------------------------------------------------------------------------------------
// The stage over the workspace
// ---------------------------------------------------------------------------------------------------------------
namespace
{
constexpr float baseHeight = 150.0f; // the dancer's height at size 1 (pixels)
constexpr double blendMs = 180.0;     // a move change blends over this long

float smoothStep (float t) { t = juce::jlimit (0.0f, 1.0f, t); return t * t * (3.0f - 2.0f * t); }

// the song's beats (playing), or a free clock at 120 BPM (stopped) — so the dancer keeps moving like FL's
struct DanceClock
{
    double lastMs = 0.0, freeBeats = 0.0;
    double beats (AudioEngine& engine)
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto dt = lastMs > 0.0 ? juce::jlimit (0.0, 0.25, (now - lastMs) * 0.001) : 0.0;
        lastMs = now;
        auto& t = engine.getTransport();
        if (t.playing.load())
        {
            freeBeats = t.positionInBeats();
            return freeBeats;
        }
        freeBeats += dt * 2.0;
        return freeBeats;
    }
};

void saveSettings (AudioEngine& engine, int channel, const dance::Settings& s, bool undoable)
{
    const auto state = dance::toState (s);
    engine.editSong ([channel, state] (Song& song) {
        if (channel >= 0 && channel < (int) song.channels.size() && song.channels[(std::size_t) channel].instrument == InstrumentKind::fruitydance)
            song.channels[(std::size_t) channel].pluginState = state;
    }, undoable);
}
} // namespace

DanceStage::DanceStage (AudioEngine& e) : engine (e)
{
    setInterceptsMouseClicks (true, false);
    setPaintingIsUnclipped (false);
    scan();
    startTimerHz (dancers.empty() ? 2 : 30);
}

DanceStage::~DanceStage() = default;

juce::Rectangle<float> DanceStage::boxOf (const Dancer& d) const
{
    const auto h = baseHeight * d.now.scale;
    const auto feet = juce::Point<float> ((float) d.now.x, (float) d.now.y);
    return dancer::reach (feet, h);
}

void DanceStage::scan()
{
    const auto& song = engine.getSong();
    std::map<int, bool> present;
    for (int c = 0; c < (int) song.channels.size(); ++c)
    {
        const auto& def = song.channels[(std::size_t) c];
        if (def.instrument != InstrumentKind::fruitydance)
            continue;
        present[c] = true;
        auto& d = dancers[c];
        d.channel = c;
        if (d.savedState != def.pluginState || d.lastMove < 0)
        {
            const auto fresh = d.lastMove < 0;
            const auto before = d.saved;
            const auto prev = d.now;
            d.savedState = def.pluginState;
            d.saved = dance::read (def.pluginState);
            d.now = d.saved;
            // a size / colour / place edit keeps the move (and the shown state) the notes picked
            if (! fresh && d.saved.move == before.move && d.saved.mirrored == before.mirrored)
            {
                d.now.move = prev.move;
                d.now.mirrored = prev.mirrored;
            }
            if (! fresh && d.saved.visible == before.visible)
                d.now.visible = prev.visible;
            if (fresh)
                d.lastMove = d.now.move;
        }
        auto inst = engine.getChannelInstrument (c);
        if (inst != d.instrument)
        {
            d.instrument = inst;
            d.seen = 0;
            if (auto* sig = dance::signalsOf (inst.get()))
            {
                int dummy[16];
                sig->pull (d.seen, dummy, 0); // (start from now)
            }
        }
    }
    for (auto it = dancers.begin(); it != dancers.end();)
    {
        if (present.count (it->first) == 0)
        {
            repaint (it->second.bounds.getSmallestIntegerContainer().expanded (4));
            it = dancers.erase (it);
        }
        else
            ++it;
    }
}

void DanceStage::timerCallback()
{
    if (--scanCountdown <= 0)
    {
        scan();
        scanCountdown = dancers.empty() ? 1 : 15; // twice a second
        const auto want = dancers.empty() ? 2 : 30;
        if (getTimerInterval() != 1000 / want)
            startTimerHz (want);
    }
    if (dancers.empty())
        return;

    const auto now = juce::Time::getMillisecondCounterHiRes();
    const auto dt = lastMs > 0.0 ? juce::jlimit (0.0, 0.25, (now - lastMs) * 0.001) : 0.0;
    lastMs = now;
    auto& t = engine.getTransport();
    const auto playing = t.playing.load();
    if (playing)
        freeBeats = t.positionInBeats();
    else
        freeBeats += dt * 2.0;
    wasPlaying = playing;

    for (auto& [channel, d] : dancers)
    {
        if (auto* sig = dance::signalsOf (d.instrument.get()))
        {
            int keys[16];
            const auto n = sig->pull (d.seen, keys, 16);
            for (int k = 0; k < n; ++k)
            {
                const auto a = dance::actionForKey (keys[k]);
                if (a.kind == dance::KeyAction::Kind::move)
                {
                    d.now.move = a.move;
                    d.now.mirrored = a.mirrored;
                }
                else if (a.kind == dance::KeyAction::Kind::hide)
                    d.now.visible = false;
                else if (a.kind == dance::KeyAction::Kind::show)
                    d.now.visible = true;
            }
        }
        const auto cycles = freeBeats * 0.5 * d.now.speed;
        const auto target = dance::pose (d.now.move, cycles, (std::uint32_t) channel * 7919u + 1u);
        if (d.now.move != d.lastMove)
        {
            d.from = d.shown;
            d.blendStart = now;
            d.lastMove = d.now.move;
        }
        if (d.blendStart >= 0.0 && now - d.blendStart < blendMs)
            d.shown = dance::Pose::mix (d.from, target, smoothStep ((float) ((now - d.blendStart) / blendMs)));
        else
        {
            d.blendStart = -1.0;
            d.shown = target;
        }
        const auto box = d.now.visible ? boxOf (d) : juce::Rectangle<float>();
        const auto dirty = box.isEmpty() ? d.bounds : d.bounds.isEmpty() ? box : d.bounds.getUnion (box);
        if (! dirty.isEmpty())
            repaint (dirty.getSmallestIntegerContainer().expanded (3));
        d.bounds = box;
    }
}

void DanceStage::paint (juce::Graphics& g)
{
    for (auto& [channel, d] : dancers)
    {
        if (! d.now.visible)
            continue;
        const auto h = baseHeight * d.now.scale;
        if (! g.clipRegionIntersects (boxOf (d).getSmallestIntegerContainer()))
            continue;
        dancer::draw (g, { (float) d.now.x, (float) d.now.y }, h, d.shown, d.now.mirrored, juce::Colour (d.now.colour));
    }
}

DanceStage::Dancer* DanceStage::dancerAt (juce::Point<float> p)
{
    for (auto it = dancers.rbegin(); it != dancers.rend(); ++it)
    {
        auto& d = it->second;
        if (! d.now.visible)
            continue;
        // the body, not the whole reach
        const auto h = baseHeight * d.now.scale;
        const auto body = juce::Rectangle<float> ((float) d.now.x - h * 0.22f, (float) d.now.y - h * 1.1f, h * 0.44f, h * 1.12f);
        if (body.contains (p))
            return &d;
    }
    return nullptr;
}

bool DanceStage::hitTest (int x, int y)
{
    return dancerAt ({ (float) x, (float) y }) != nullptr;
}

void DanceStage::mouseDown (const juce::MouseEvent& e)
{
    auto* d = dancerAt (e.position);
    if (d == nullptr)
        return;
    if (e.mods.isPopupMenu())
    {
        showMenu (*d);
        return;
    }
    dragging = d->channel;
    dragOffset = { d->now.x - e.x, d->now.y - e.y };
}

void DanceStage::mouseDrag (const juce::MouseEvent& e)
{
    auto it = dancers.find (dragging);
    if (it == dancers.end())
        return;
    auto& d = it->second;
    const auto old = d.bounds;
    d.now.x = juce::jlimit (0, juce::jmax (0, getWidth()), e.x + dragOffset.x);
    d.now.y = juce::jlimit (20, juce::jmax (20, getHeight()), e.y + dragOffset.y);
    d.bounds = boxOf (d);
    repaint (old.getUnion (d.bounds).getSmallestIntegerContainer().expanded (3));
}

void DanceStage::mouseUp (const juce::MouseEvent&)
{
    auto it = dancers.find (dragging);
    dragging = -1;
    if (it == dancers.end())
        return;
    auto& d = it->second;
    if (d.now.x == d.saved.x && d.now.y == d.saved.y)
        return;
    auto s = d.saved;
    s.x = d.now.x;
    s.y = d.now.y;
    save (it->first, s);
}

void DanceStage::save (int channel, const dance::Settings& s)
{
    saveSettings (engine, channel, s, false); // (the dancer's place is like a window's: not an undo step)
    if (auto it = dancers.find (channel); it != dancers.end())
    {
        it->second.saved = s;
        it->second.savedState = dance::toState (s);
    }
}

void DanceStage::showMenu (Dancer& d)
{
    juce::PopupMenu menu;
    const auto channel = d.channel;
    const auto& song = engine.getSong();
    if (channel >= 0 && channel < (int) song.channels.size())
        menu.addSectionHeader (juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str()));
    for (int m = 0; m < dance::moveCount; ++m)
        menu.addItem (tr (dance::moveName (m)), true, d.now.move == m, [this, channel, m] {
            if (auto it = dancers.find (channel); it != dancers.end())
            {
                auto s = it->second.saved;
                s.move = m;
                s.mirrored = it->second.now.mirrored;
                it->second.now.move = m;
                save (channel, s);
            }
        });
    menu.addSeparator();
    menu.addItem (tr ("Mirrored"), true, d.now.mirrored, [this, channel] {
        if (auto it = dancers.find (channel); it != dancers.end())
        {
            auto s = it->second.saved;
            s.mirrored = ! it->second.now.mirrored;
            s.move = it->second.now.move;
            it->second.now.mirrored = s.mirrored;
            save (channel, s);
        }
    });
    menu.addItem (tr ("Hide the dancer"), [this, channel] {
        if (auto it = dancers.find (channel); it != dancers.end())
        {
            auto s = it->second.saved;
            s.visible = false;
            it->second.now.visible = false;
            save (channel, s);
        }
    });
    menu.addItem (tr ("Settings..."), [this, channel] { openChannelSettings (engine, channel); });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

// ---------------------------------------------------------------------------------------------------------------
// The channel's editor
// ---------------------------------------------------------------------------------------------------------------
namespace
{
class DanceEditor final : public juce::Component,
                          private juce::Timer
{
public:
    DanceEditor (AudioEngine& e, int c) : engine (e), channel (c)
    {
        settings = current();
        shownPose = dance::pose (settings.move, 0.3, (std::uint32_t) channel * 7919u + 1u);
        for (int m = 0; m < dance::moveCount; ++m)
            moveBox.addItem (tr (dance::moveName (m)), m + 1);
        moveBox.onChange = [this] { change ([this] (dance::Settings& s) { s.move = moveBox.getSelectedId() - 1; }); };
        mirrored.setButtonText (tr ("Mirrored"));
        mirrored.onClick = [this] { change ([this] (dance::Settings& s) { s.mirrored = mirrored.getToggleState(); }); };
        shown.setButtonText (tr ("Show the dancer"));
        shown.onClick = [this] { change ([this] (dance::Settings& s) { s.visible = shown.getToggleState(); }); };
        size.setRange (0.25, 3.0, 0.01);
        size.setSkewFactorFromMidPoint (1.0);
        size.setDoubleClickReturnValue (true, 1.0);
        size.setTextValueSuffix (" x");
        size.onValueChange = [this] { change ([this] (dance::Settings& s) { s.scale = (float) size.getValue(); }); };
        speed.setRange (0.125, 4.0, 0.001);
        speed.setSkewFactorFromMidPoint (1.0);
        speed.setDoubleClickReturnValue (true, 1.0);
        speed.setTextValueSuffix (" x");
        speed.onValueChange = [this] { change ([this] (dance::Settings& s) { s.speed = (float) speed.getValue(); }); };
        for (auto* s : { &size, &speed })
        {
            s->setSliderStyle (juce::Slider::LinearHorizontal);
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
        }
        colour.setButtonText (tr ("Outfit colour..."));
        colour.onClick = [this] {
            const auto before = settings.colour;
            ui::showColourPickerPopup (colour.getScreenBounds(), tr ("Outfit colour"), juce::Colour (settings.colour),
                                       [this] (juce::Colour c) { settings.colour = c.getARGB() | 0xff000000u; repaint(); },
                                       [this] (juce::Colour c) { change ([c] (dance::Settings& s) { s.colour = c.getARGB() | 0xff000000u; }); },
                                       [this, before] { settings.colour = before; repaint(); });
        };
        place.setButtonText (tr ("Put the dancer back"));
        place.onClick = [this] {
            const dance::Settings defaults;
            change ([defaults] (dance::Settings& s) {
                s.x = defaults.x;
                s.y = defaults.y;
                s.visible = true;
            });
        };
        for (auto* c : std::initializer_list<juce::Component*> { &moveBox, &mirrored, &shown, &size, &speed, &colour, &place })
            addAndMakeVisible (c);
        load();
        setSize (640, 320);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (stage.toFloat(), 6.0f);
        // a floor line
        g.setColour (theme::colour::border);
        g.drawHorizontalLine (stage.getBottom() - 26, (float) stage.getX() + 12.0f, (float) stage.getRight() - 12.0f);
        {
            juce::Graphics::ScopedSaveState save (g);
            g.reduceClipRegion (stage);
            const auto h = juce::jmin ((float) stage.getHeight() * 0.68f, 190.0f);
            dancer::draw (g, { (float) stage.getCentreX(), (float) stage.getBottom() - 26.0f }, h, shownPose, settings.mirrored, juce::Colour (settings.colour));
        }
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::title));
        g.drawText (tr ("LYNX Dance"), header, juce::Justification::centredLeft);
        g.setFont (theme::font (theme::type::small));
        g.setColour (theme::colour::textSecondary);
        g.drawFittedText (tr ("Fruity Dance's role with LYNX's own dancer: it makes no sound. Its notes pick the move; drag the dancer to "
                              "place it, right-click it for its moves."),
                          header.translated (0, 22).withHeight (30), juce::Justification::topLeft, 2);
        for (const auto& [area, text] : { std::pair { moveLabel, tr ("Move") }, std::pair { sizeLabel, tr ("Size") }, std::pair { speedLabel, tr ("Speed") } })
        {
            g.setColour (theme::colour::textSecondary);
            g.drawText (text, area, juce::Justification::centredLeft);
        }
        g.setColour (theme::colour::textSecondary);
        g.drawFittedText (keyHelp(), keysArea, juce::Justification::topLeft, 6);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        stage = area.removeFromLeft (230);
        area.removeFromLeft (14);
        header = area.removeFromTop (22);
        area.removeFromTop (34);
        auto row = [&area] (int h = 26) { auto r = area.removeFromTop (h); area.removeFromTop (6); return r; };
        {
            auto r = row();
            moveLabel = r.removeFromLeft (60);
            moveBox.setBounds (r.removeFromLeft (180));
            r.removeFromLeft (10);
            mirrored.setBounds (r);
        }
        {
            auto r = row();
            sizeLabel = r.removeFromLeft (60);
            size.setBounds (r);
        }
        {
            auto r = row();
            speedLabel = r.removeFromLeft (60);
            speed.setBounds (r);
        }
        {
            auto r = row();
            shown.setBounds (r.removeFromLeft (160));
            colour.setBounds (r.removeFromLeft (130));
            r.removeFromLeft (8);
            place.setBounds (r.removeFromLeft (150));
        }
        keysArea = area;
    }

private:
    dance::Settings current() const
    {
        const auto& song = engine.getSong();
        if (channel < 0 || channel >= (int) song.channels.size())
            return {};
        return dance::read (song.channels[(std::size_t) channel].pluginState);
    }

    void load()
    {
        moveBox.setSelectedId (settings.move + 1, juce::dontSendNotification);
        mirrored.setToggleState (settings.mirrored, juce::dontSendNotification);
        shown.setToggleState (settings.visible, juce::dontSendNotification);
        size.setValue (settings.scale, juce::dontSendNotification);
        speed.setValue (settings.speed, juce::dontSendNotification);
    }

    void change (const std::function<void (dance::Settings&)>& edit)
    {
        auto s = current();
        edit (s);
        settings = s;
        saveSettings (engine, channel, s, true);
        repaint();
    }

    static juce::String keyName (int key) { return juce::MidiMessage::getMidiNoteName (key, true, true, 5); }

    juce::String keyHelp() const
    {
        juce::String moves;
        for (int m = 0; m < dance::moveCount; ++m)
            moves << (m > 0 ? ", " : "") << keyName (dance::firstMoveKey + m) << " " << tr (dance::moveName (m));
        return tr ("Keys (as in FL's Fruity Dance): {0}. From {1} down: the same moves mirrored. {2} hides the dancer, {3} shows it.",
                   { moves, keyName (dance::firstMirrorKey), keyName (dance::hideKey), keyName (dance::showKey) });
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;
        // the editor follows the saved settings (an undo, the stage's menu)
        if (const auto s = current(); s != settings)
        {
            settings = s;
            load();
        }
        const auto beats = clock.beats (engine);
        shownPose = dance::pose (settings.move, beats * 0.5 * settings.speed, (std::uint32_t) channel * 7919u + 1u);
        repaint (stage);
    }

    AudioEngine& engine;
    int channel;
    dance::Settings settings;
    dance::Pose shownPose;
    DanceClock clock;
    juce::ComboBox moveBox;
    juce::ToggleButton mirrored, shown;
    juce::Slider size, speed;
    juce::TextButton colour, place;
    juce::Rectangle<int> stage, header, moveLabel, sizeLabel, speedLabel, keysArea;
};
} // namespace

std::unique_ptr<juce::Component> makeDanceEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<DanceEditor> (engine, channel);
}
} // namespace lynx
