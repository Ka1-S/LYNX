#include "ui/SlotWatch.h"
#include "ui/FacetEqEditor.h"
#include "core/ShownEffectSlot.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <map>

#include "core/Effects.h"
#include "core/FacetEq.h"
#include "core/RackRenderer.h"
#include "engine/Text.h"
#include "ui/I18n.h"
#include "ui/PluginHeader.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
using namespace facet;

constexpr double minHz = 10.0, maxHz = 30000.0;      // the graph's frequency axis (the bands' whole range)
constexpr double specTop = 0.0; // analyser scale top (dBFS, tilted around 1 kHz); the bottom is the Analyzer Range setting
constexpr int fftOrder = 13, fftSize = 1 << fftOrder;

juce::Colour bandColour (int band)
{
    // one hue per band at the same lightness; the first is LYNX's red
    static const juce::Colour colours[] = { juce::Colour (0xffef5b5f), juce::Colour (0xfff2a46b), juce::Colour (0xffe8cf6a),
                                            juce::Colour (0xff86d993), juce::Colour (0xff62cfd0), juce::Colour (0xff7aa9f5),
                                            juce::Colour (0xffb59af2), juce::Colour (0xffef7fc0) };
    return colours[band & 7];
}

// display names of facet::Shape (core names, translated here)
const char* const shapeNames[] = { TR_NOOP ("Off"), TR_NOOP ("Bell"), TR_NOOP ("Low shelf"), TR_NOOP ("High shelf"), TR_NOOP ("Low cut"),
                                   TR_NOOP ("High cut"), TR_NOOP ("Notch"), TR_NOOP ("Band pass"), TR_NOOP ("Tilt shelf"),
                                   TR_NOOP ("Flat tilt"), TR_NOOP ("All pass") }; // round 135: the last two
const char* const phaseNames[] = { TR_NOOP ("Zero latency"), TR_NOOP ("Natural phase"), TR_NOOP ("Linear phase") };
const char* const analyzerShort[] = { TR_NOOP ("Off"), TR_NOOP ("Pre"), TR_NOOP ("Post"), TR_NOOP ("Pre + Post") };
const char* const analyzerNames[] = { TR_NOOP ("Analyzer off"), TR_NOOP ("Analyzer: pre"), TR_NOOP ("Analyzer: post"), TR_NOOP ("Analyzer: pre + post") };

juce::String noteName (double hz)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const auto n = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const auto k = (int) std::lround (n);
    const auto cents = (int) std::lround ((n - k) * 100.0);
    return juce::String (names[((k % 12) + 12) % 12]) + juce::String (k / 12 - 1) + (cents == 0 ? juce::String() : (cents > 0 ? " +" : " ") + juce::String (cents));
}

juce::String hzText (double hz)
{
    return hz < 1000.0 ? juce::String (hz, hz < 100.0 ? 1 : 0) + " Hz" : juce::String (hz / 1000.0, hz < 10000.0 ? 2 : 1) + " kHz";
}

// a band's stereo placement for display: Mid / Side stay as they are (the usual terms in every language)
juce::String placementLabel (facet::Placement p)
{
    return p == facet::Placement::mid || p == facet::Placement::side ? juce::String (facet::placementName (p)) : tr (facet::placementName (p));
}

// the power symbol (an open ring with a bar), for the band power buttons
void drawPowerGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
{
    const auto d = juce::jmin (r.getWidth(), r.getHeight()) * 0.62f;
    const auto ctr = r.getCentre();
    juce::Path ring;
    ring.addCentredArc (ctr.x, ctr.y + d * 0.06f, d * 0.5f, d * 0.5f, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
    g.setColour (c);
    g.strokePath (ring, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    g.drawLine (ctr.x, ctr.y - d * 0.62f, ctr.x, ctr.y - d * 0.05f, 1.5f);
}

class FacetEqEditor final : public juce::Component,
                            public ui::HintSource,
                            private juce::ChangeListener,
                            private juce::Timer
{
public:
    FacetEqEditor (AudioEngine& e, int t, int s, bool demoSpectrum)
        : engine (e),
          track (t),
          slot (s),
          demo (demoSpectrum),
          fft (fftOrder)
    {
        fftData.assign ((std::size_t) fftSize * 2, 0.0f);
        window.resize ((std::size_t) fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[(std::size_t) i] = 0.5f - 0.5f * std::cos (2.0f * juce::MathConstants<float>::pi * (float) i / (float) fftSize);
        if (const auto* fx = current())
            for (int b = 0; b < bands; ++b)
                if (shape (fx->params[(std::size_t) param (b, 0)]) != Shape::off) { selected = b; break; }
        engine.addChangeListener (this);
        setWantsKeyboardFocus (true);
        setSize (960, 560);
        if (demo)
        {
            for (int b = bands; --b >= 0;) // show a dynamic band, if any
                if (current() != nullptr && isDynamic (current()->params, b)) { selected = b; break; }
            updateSpectrum();
        }
        claimTap();
        startTimerHz (30);
    }

    ~FacetEqEditor() override
    {
        stopTimer();
        engine.removeChangeListener (this);
        if (auto* times = engine.getProcessTimes(); times != nullptr && times->analyzer.key.load() == tapKey())
            times->analyzer.key.store (-1);
        // solo is a listening aid: it never stays on once the editor is gone
        if (const auto* fx = current(); fx != nullptr && soloBand (fx->params[(std::size_t) soloParam]) >= 0)
            engine.editSong ([tk = track, sl = slot] (Song& song) { song.mixer[(std::size_t) tk].effects[(std::size_t) sl].params[(std::size_t) soloParam] = 0.0f; }, false);
        ui::hideValueTip();
    }

    // the slot as shown: automated band handles and knobs at their automation's value while the song plays (issue #11)
    const EffectSlot* current() const { return shownSlot.get (engine.getAutomationDisplay(), stored(), track, slot, juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown()); }

    const EffectSlot* stored() const
    {
        const auto& mixer = engine.getSong().mixer;
        if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
            return nullptr;
        const auto& fx = mixer[(std::size_t) track].effects[(std::size_t) slot];
        return fx.kind == EffectKind::facetEq ? &fx : nullptr;
    }

    // --- geometry ------------------------------------------------------------------------------------

    // the whole window is the graph (curves over the analyser); a slim title bar on top, the global bar at the bottom,
    // and the selected band's controls float over the bottom of the graph
    juce::Rectangle<float> headerArea() const { return { 0.0f, 0.0f, (float) getWidth(), 30.0f }; }
    juce::Rectangle<float> barArea() const { return { 0.0f, (float) getHeight() - 30.0f, (float) getWidth(), 30.0f }; }
    juce::Rectangle<float> graphArea() const { return { 0.0f, 30.0f, (float) getWidth(), barArea().getY() - 30.0f }; }
    juce::Rectangle<float> plotArea() const { return graphArea().withTrimmedRight (64.0f).withTrimmedBottom (20.0f).withTrimmedTop (6.0f); }
    juce::Rectangle<float> panelArea() const
    {
        const auto w = hasGain (bandShape (selected)) ? 690.0f : 368.0f;
        constexpr float h = 118.0f;
        return { juce::jmax (4.0f, plotArea().getCentreX() - w / 2.0f), plotArea().getBottom() - h - 4.0f, w, h };
    }
    bool panelShown() const { return bandOn (selected); }
    // the small read-out box next to a hovered node (frequency, gain, Q, solo, remove)
    juce::Rectangle<float> tipArea (int b) const
    {
        const auto n = nodePosition (b);
        auto r = juce::Rectangle<float> (n.x - 70.0f, n.y - 74.0f, 140.0f, 58.0f);
        if (r.getY() < plotArea().getY()) r.setY (n.y + 16.0f);
        return r.constrainedWithin (plotArea());
    }

    float xForHz (double f) const
    {
        const auto a = plotArea();
        return a.getX() + a.getWidth() * (float) (std::log (juce::jlimit (minHz, maxHz, f) / minHz) / std::log (maxHz / minHz));
    }
    double hzForX (float x) const
    {
        const auto a = plotArea();
        return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth())));
    }
    float yForDb (double db) const
    {
        const auto a = plotArea();
        const auto rangeDb = viewRange();
        return a.getCentreY() - a.getHeight() * 0.5f * (float) (juce::jlimit (-rangeDb * 1.1, rangeDb * 1.1, db) / rangeDb) * 0.92f;
    }
    double dbForY (float y) const
    {
        const auto a = plotArea();
        return (double) ((a.getCentreY() - y) / (a.getHeight() * 0.5f * 0.92f)) * viewRange();
    }
    float yForSpectrum (double db) const
    {
        const auto a = plotArea();
        const auto specBottom = specFloor();
        return a.getY() + a.getHeight() * (float) ((specTop - juce::jlimit (specBottom, specTop, db)) / (specTop - specBottom));
    }

    float param01 (int p) const
    {
        const auto* fx = current();
        return fx != nullptr && p >= 0 && p < maxEffectParams ? fx->params[(std::size_t) p] : 0.0f;
    }

    Shape bandShape (int b) const { return shape (param01 (param (b, 0))); }
    // display settings (saved with the slot, round 128): the graph's ± dB range, the analyser's floor, speed and tilt
    double viewRange() const { return displayRangeDb (param01 (displayRangeParam)); }
    double specFloor() const { return -analyzerRangeDb (param01 (specRangeParam)); }
    bool bandPowered (int b) const { return param01 (powerParam0 + b) < 0.5f; }
    Placement bandPlacement (int b) const { return placement (param01 (placementParam0 + b)); }
    bool bandOn (int b) const { return b >= 0 && b < bands && bandShape (b) != Shape::off; }

    // the live dynamic gain of a band (dB), from the audio thread
    double liveDynamic (int b) const
    {
        const auto* times = engine.getProcessTimes();
        const auto key = (std::size_t) (track * maxEffectSlots + slot) * ProcessTimes::meterOutputs + (std::size_t) b;
        if (times == nullptr || key >= times->effectMeters.size()) return 0.0;
        return (double) times->effectMeters[key].load (std::memory_order_relaxed) * 60.0;
    }

    juce::Point<float> nodePosition (int b) const
    {
        const auto f = hz (param01 (param (b, 1)));
        const auto db = hasGain (bandShape (b)) ? gainDb (param01 (param (b, 2))) * gainScale (param01 (scaleParam)) : 0.0;
        return { xForHz (f), yForDb (db) };
    }

    // the range handle of a dynamic band: at static gain + range
    juce::Point<float> rangeHandle (int b) const
    {
        const auto scale = gainScale (param01 (scaleParam));
        const auto db = (gainDb (param01 (param (b, 2))) + rangeDb_ (b)) * scale;
        return { nodePosition (b).x, yForDb (db) };
    }

    double rangeDb_ (int b) const { return facet::rangeDb (param01 (rangeParam0 + b)); }

    int nodeAt (juce::Point<float> p) const
    {
        int best = -1;
        float bestDistance = 11.0f;
        for (int b = 0; b < bands; ++b)
            if (bandOn (b))
                if (const auto d = nodePosition (b).getDistanceFrom (p); d < bestDistance)
                {
                    best = b;
                    bestDistance = d;
                }
        return best;
    }

    int rangeHandleAt (juce::Point<float> p) const
    {
        for (int b = 0; b < bands; ++b)
            if (bandOn (b) && hasGain (bandShape (b)) && b == selected && rangeHandle (b).getDistanceFrom (p) < 8.0f)
                return b;
        return -1;
    }

    // --- controls of the bottom strip and the header -------------------------------------------------

    enum class Ctl { none, shapeMenu, freq, gain, q, slopeMenu, range, threshold, solo, dynamic, remove, attack, release,
                     phase, analyzer, detector, output, scale, scaleDb, prevBand, nextBand, close, piano, tipSolo, tipRemove,
                     power, placementMenu, tipPower, autoGain, pan,
                     spectral, spectralTilt, match }; // round 152
    struct Control { Ctl type; juce::Rectangle<float> r; int param = -1; int index = 0; };

    std::vector<Control> controls() const
    {
        std::vector<Control> out;
        // the bottom bar: piano / frequency scale, processing mode, analyser, detector … gain scale and output on the right
        const auto bar = barArea();
        const auto cy = bar.getCentreY();
        out.push_back ({ Ctl::piano, { 8.0f, cy - 10.0f, 26.0f, 20.0f } });
        out.push_back ({ Ctl::phase, { 44.0f, cy - 11.0f, 150.0f, 22.0f }, phaseParam });
        out.push_back ({ Ctl::analyzer, { 202.0f, cy - 11.0f, 170.0f, 22.0f }, analyzerParam });
        out.push_back ({ Ctl::detector, { 380.0f, cy - 11.0f, 150.0f, 22.0f }, detectorParam });
        out.push_back ({ Ctl::match, { 536.0f, cy - 11.0f, 48.0f, 22.0f } }); // round 152: EQ match
        out.push_back ({ Ctl::output, { bar.getRight() - 100.0f, cy - 11.0f, 92.0f, 22.0f }, outputParam });
        out.push_back ({ Ctl::scale, { bar.getRight() - 190.0f, cy - 11.0f, 84.0f, 22.0f }, scaleParam });
        out.push_back ({ Ctl::pan, { bar.getRight() - 282.0f, cy - 11.0f, 86.0f, 22.0f }, panParam });
        out.push_back ({ Ctl::autoGain, { bar.getRight() - 372.0f, cy - 11.0f, 84.0f, 22.0f }, autoGainParam });
        // the graph's dB range (top right: ±3 / 6 / 12 / 30)
        const auto plot = plotArea();
        out.push_back ({ Ctl::scaleDb, { plot.getRight() + 6.0f, plot.getY() + 2.0f, 50.0f, 18.0f } });

        // the hovered node's read-out: solo and remove
        if (tipBand >= 0 && bandOn (tipBand) && dragging == Drag::none)
        {
            const auto t = tipArea (tipBand);
            out.push_back ({ Ctl::tipPower, { t.getRight() - 62.0f, t.getY() + 4.0f, 18.0f, 18.0f }, -1, tipBand });
            out.push_back ({ Ctl::tipSolo, { t.getRight() - 42.0f, t.getY() + 4.0f, 18.0f, 18.0f }, -1, tipBand });
            out.push_back ({ Ctl::tipRemove, { t.getRight() - 22.0f, t.getY() + 4.0f, 18.0f, 18.0f }, -1, tipBand });
        }

        // the floating band panel: shape | FREQ | GAIN | dynamics | Q | band navigation, solo, remove
        if (panelShown())
        {
            const auto p = panelArea();
            const auto b = selected;
            const auto sh = bandShape (b);
            const auto y = p.getY();
            auto x = p.getX();
            out.push_back ({ Ctl::power, { x + 10.0f, y + 4.0f, 24.0f, 24.0f }, powerParam0 + b });
            out.push_back ({ Ctl::shapeMenu, { x + 12.0f, y + 30.0f, 108.0f, 26.0f }, param (b, 0) });
            if (sh == Shape::lowCut || sh == Shape::highCut || sh == Shape::lowShelf || sh == Shape::highShelf || sh == Shape::tiltShelf)
                out.push_back ({ Ctl::slopeMenu, { x + 12.0f, y + 62.0f, 108.0f, 22.0f }, param (b, 4) });
            x += 126.0f;
            out.push_back ({ Ctl::freq, { x, y + 8.0f, 70.0f, 104.0f }, param (b, 1) });
            x += 72.0f;
            if (hasGain (sh))
            {
                out.push_back ({ Ctl::gain, { x, y + 2.0f, 96.0f, 112.0f }, param (b, 2) });
                x += 104.0f;
                out.push_back ({ Ctl::dynamic, { x + 4.0f, y + 8.0f, 86.0f, 20.0f }, rangeParam0 + b });
                // round 152: Pro-Q 4's spectral dynamics (right-click: density) and its tilt
                if (isDynamic (current()->params, b))
                {
                    out.push_back ({ Ctl::spectral, { x + 94.0f, y + 8.0f, 70.0f, 20.0f }, spectralParam0 + b });
                    if (isSpectral (current()->params, b))
                        out.push_back ({ Ctl::spectralTilt, { x + 168.0f, y + 8.0f, 46.0f, 20.0f }, spectralTiltParam0 + b });
                }
                if (isDynamic (current()->params, b))
                {
                    out.push_back ({ Ctl::range, { x - 2.0f, y + 34.0f, 56.0f, 80.0f }, rangeParam0 + b });
                    out.push_back ({ Ctl::threshold, { x + 54.0f, y + 34.0f, 56.0f, 80.0f }, thresholdParam0 + b });
                    out.push_back ({ Ctl::attack, { x + 110.0f, y + 34.0f, 52.0f, 80.0f }, attackParam });
                    out.push_back ({ Ctl::release, { x + 162.0f, y + 34.0f, 52.0f, 80.0f }, releaseParam });
                }
                x += 218.0f;
            }
            out.push_back ({ Ctl::q, { x, y + 8.0f, 70.0f, 104.0f }, param (b, 3) });
            x += 78.0f;
            out.push_back ({ Ctl::prevBand, { x + 2.0f, y + 6.0f, 20.0f, 20.0f } });
            out.push_back ({ Ctl::nextBand, { x + 44.0f, y + 6.0f, 20.0f, 20.0f } });
            out.push_back ({ Ctl::close, { x + 66.0f, y + 6.0f, 20.0f, 20.0f } });
            out.push_back ({ Ctl::solo, { x + 4.0f, y + 34.0f, 80.0f, 22.0f }, soloParam });
            out.push_back ({ Ctl::placementMenu, { x + 4.0f, y + 60.0f, 80.0f, 22.0f }, placementParam0 + b });
            out.push_back ({ Ctl::remove, { x + 4.0f, y + 86.0f, 80.0f, 22.0f } });
        }
        return out;
    }

    const Control* controlAt (juce::Point<float> p, std::vector<Control>& all) const
    {
        all = controls();
        for (const auto& c : all)
            if (c.r.contains (p)) return &c;
        return nullptr;
    }

    juce::Rectangle<float> knobCircle (const Control& c) const
    {
        const auto d = juce::jmin (c.r.getWidth() - 12.0f, c.r.getHeight() - 44.0f);
        return { c.r.getCentreX() - d / 2.0f, c.r.getY() + 4.0f, d, d };
    }

    juce::String trackName() const
    {
        const auto& mixer = engine.getSong().mixer;
        if (track == 0) return tr ("Master");
        const auto name = track < (int) mixer.size() ? juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str()) : juce::String();
        return name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) });
    }

    ui::ValueSpec specFor (int prm) const
    {
        ui::ValueSpec spec;
        const auto& in = info();
        if (prm < 0 || prm >= (int) in.params.size()) return spec;
        spec.name = trackName() + " - Facet EQ - " + utf8 (in.params[(std::size_t) prm].name);
        spec.defaultValue = in.params[(std::size_t) prm].defaultValue;
        spec.pixelsForRange = 200.0;
        spec.toText = [prm] (double v) { return utf8 (paramText (prm, (float) v).c_str()); };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        spec.target = AutomationTarget { AutomationTargetKind::effectParam, track, slot, prm };
        return spec;
    }

    juce::String valueText (int prm) const { return utf8 (paramText (prm, param01 (prm)).c_str()); }

    // --- editing -------------------------------------------------------------------------------------

    void set (std::vector<std::pair<int, float>> changes, bool undoable = true)
    {
        if (current() == nullptr) return;
        if (undoable) bar.willEdit();
        engine.editSong ([tk = track, sl = slot, changes] (Song& song) {
            auto& params = song.mixer[(std::size_t) tk].effects[(std::size_t) sl].params;
            for (const auto& [p, v] : changes)
                if (p >= 0 && p < maxEffectParams) params[(std::size_t) p] = juce::jlimit (0.0f, 1.0f, v);
        }, undoable);
    }

    void addBand (juce::Point<float> at)
    {
        for (int b = 0; b < bands; ++b)
            if (! bandOn (b))
            {
                const auto f = hzForX (at.x);
                // the ends of the spectrum get cuts, as a quick start; anything else is a bell at the clicked gain
                const auto sh = f < 25.0 ? Shape::lowCut : f > 18000.0 ? Shape::highCut : Shape::bell;
                const auto scale = juce::jmax (0.01, gainScale (param01 (scaleParam)));
                set ({ { param (b, 0), shapeValue (sh) }, { param (b, 1), hzValue (f) }, { param (b, 2), sh == Shape::bell ? gainValue (dbForY (at.y) / scale) : 0.5f },
                       { param (b, 3), qValue (1.0) }, { param (b, 4), slopeValue (sh == Shape::bell ? 12 : 24) }, { rangeParam0 + b, 0.5f },
                       { powerParam0 + b, 0.0f }, { placementParam0 + b, 0.0f } });
                selected = b;
                return;
            }
    }

    void removeBand (int b)
    {
        std::vector<std::pair<int, float>> c { { param (b, 0), 0.0f }, { rangeParam0 + b, 0.5f } };
        if (soloBand (param01 (soloParam)) == b) c.push_back ({ soloParam, 0.0f });
        set (c);
        if (selected == b)
        {
            selected = -1;
            for (int k = 0; k < bands; ++k)
                if (k != b && bandOn (k)) { selected = k; break; }
        }
    }

    void toggleSolo (int b)
    {
        set ({ { soloParam, soloBand (param01 (soloParam)) == b ? 0.0f : soloValue (b) } }, false);
    }

    void showBandMenu (int b)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Band {0}", { juce::String (b + 1) }));
        const auto sh = bandShape (b);
        for (int k = 1; k < shapeCount; ++k)
            menu.addItem (tr (shapeNames[k]), true, (int) sh == k, [this, b, k] { set ({ { param (b, 0), shapeValue ((Shape) k) } }); });
        if (sh == Shape::lowCut || sh == Shape::highCut || sh == Shape::lowShelf || sh == Shape::highShelf || sh == Shape::tiltShelf)
        {
            juce::PopupMenu slopes;
            for (const auto d : facet::slopes)
                slopes.addItem (juce::String (d) + " dB/oct", true, slopeDb (param01 (param (b, 4))) == d, [this, b, d] { set ({ { param (b, 4), slopeValue (d) } }); });
            menu.addSubMenu (tr ("Slope"), slopes);
        }
        if (hasGain (sh))
        {
            juce::PopupMenu dyn;
            for (const auto r : { 0.0, -3.0, -6.0, -12.0, 3.0, 6.0 })
                dyn.addItem (r == 0.0 ? tr ("Off") : juce::String (r > 0 ? "+" : "") + juce::String ((int) r) + " dB", true, std::abs (rangeDb_ (b) - r) < 0.05,
                             [this, b, r] { set ({ { rangeParam0 + b, rangeValue (r) } }); });
            menu.addSubMenu (tr ("Dynamic range"), dyn);
        }
        juce::PopupMenu where;
        for (int k = 0; k < placementCount; ++k)
            where.addItem (placementLabel ((Placement) k), true, (int) bandPlacement (b) == k, [this, b, k] { set ({ { placementParam0 + b, placementValue ((Placement) k) } }); });
        menu.addSubMenu (tr ("Stereo placement"), where);
        menu.addSeparator();
        menu.addItem (tr ("Band on"), true, bandPowered (b), [this, b] { set ({ { powerParam0 + b, bandPowered (b) ? 1.0f : 0.0f } }); });
        menu.addItem (tr ("Solo band"), true, soloBand (param01 (soloParam)) == b, [this, b] { toggleSolo (b); });
        menu.addItem (tr ("Reset gain"), hasGain (sh), false, [this, b] { set ({ { param (b, 2), 0.5f } }); });
        menu.addItem (tr ("Remove band"), [this, b] { removeBand (b); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showDetectorMenu()
    {
        juce::PopupMenu menu;
        const auto* fx = current();
        if (fx == nullptr) return;
        const auto side = sidechainDetector (param01 (detectorParam));
        menu.addItem (tr ("Detect the EQ's input"), true, ! side, [this] { set ({ { detectorParam, 0.0f } }); });
        menu.addItem (tr ("Detect the sidechain"), true, side, [this] { set ({ { detectorParam, 1.0f } }); });
        juce::PopupMenu from;
        const auto assign = [this] (int source) {
            engine.editSong ([tk = track, sl = slot, source] (Song& s) {
                auto& target = s.mixer[(std::size_t) tk].effects[(std::size_t) sl];
                target.sidechain = source;
                if (source >= 0) target.params[(std::size_t) detectorParam] = 1.0f;
            });
        };
        from.addItem (tr ("None"), true, fx->sidechain < 0, [assign] { assign (-1); });
        const auto& song = engine.getSong();
        for (int t = 1; t < (int) song.mixer.size(); ++t)
            if (t != track)
                from.addItem (juce::String (t) + " " + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()), true, fx->sidechain == t, [assign, t] { assign (t); });
        menu.addSubMenu (tr ("Sidechain from"), from);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // round 152: the spectral band's density (right-click on Spectral)
    void showDensityMenu (int b)
    {
        if (b < 0) return;
        juce::PopupMenu menu;
        const auto now = param01 (densityParam0 + b);
        for (const int pct : { 0, 25, 50, 75, 100 })
            menu.addItem (tr ("Density {0} %", { juce::String (pct) }), true, std::abs (now - pct / 100.0f) < 0.01f,
                          [this, b, pct] { set ({ { densityParam0 + b, pct / 100.0f } }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // round 152: EQ match — capture the reference (play it through this slot), capture this track, then match
    enum class Capture { none, reference, current };
    juce::String matchStateText() const
    {
        const auto secs = [] (int frames) { return juce::String (frames / 30); }; // the editor's 30 Hz frames
        if (capture == Capture::reference) return tr ("reference {0} s", { secs (refFrames) });
        if (capture == Capture::current) return tr ("this track {0} s", { secs (curFrames) });
        if (refFrames > 0 && curFrames > 0) return tr ("ready");
        if (refFrames > 0) return tr ("reference captured");
        return tr ("off");
    }
    void showMatchMenu()
    {
        juce::PopupMenu menu;
        if (analyzer (param01 (analyzerParam)) == Analyzer::off)
            menu.addItem (tr ("(turn the analyzer on to capture spectra)"), false, false, [] {});
        menu.addItem (capture == Capture::reference ? tr ("Stop capturing the reference") : tr ("Capture the reference (play it through this slot now)"), true,
                      capture == Capture::reference, [this] {
                          if (capture == Capture::reference) capture = Capture::none;
                          else { capture = Capture::reference; matchRef.clear(); refFrames = 0; }
                          repaint();
                      });
        menu.addItem (capture == Capture::current ? tr ("Stop capturing this track") : tr ("Capture this track (play it now)"), true,
                      capture == Capture::current, [this] {
                          if (capture == Capture::current) capture = Capture::none;
                          else { capture = Capture::current; matchCur.clear(); curFrames = 0; }
                          repaint();
                      });
        menu.addSeparator();
        const bool ready = refFrames > 0 && curFrames > 0 && capture == Capture::none;
        for (const int amount : { 100, 75, 50, 25 })
            menu.addItem (tr ("Match {0} %", { juce::String (amount) }), ready, false, [this, amount] { runMatch (amount / 100.0); });
        juce::PopupMenu bandsMenu;
        for (const int nb : { 2, 4, 6, 8 })
            bandsMenu.addItem (tr ("Up to {0} bands", { juce::String (nb) }), true, matchBands == nb, [this, nb] { matchBands = nb; });
        menu.addSubMenu (tr ("Bands"), bandsMenu);
        juce::PopupMenu smooth;
        for (const int den : { 12, 6, 3, 1 })
            smooth.addItem (tr ("Smoothing 1/{0} octave", { juce::String (den) }), true, matchSmoothing == den, [this, den] { matchSmoothing = den; });
        menu.addSubMenu (tr ("Smoothing"), smooth);
        menu.addItem (tr ("Clear the captures"), refFrames > 0 || curFrames > 0, false, [this] {
            capture = Capture::none; matchRef.clear(); matchCur.clear(); refFrames = curFrames = 0; repaint();
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }
    void runMatch (double amount)
    {
        const auto* fx = current();
        if (fx == nullptr || matchRef.size() != (std::size_t) (fftSize / 2) || matchCur.size() != matchRef.size()) return;
        const auto sr = displayRate();
        constexpr int n = 240;
        std::vector<double> f (n), cur (n), ref (n);
        for (int i = 0; i < n; ++i)
        {
            f[(std::size_t) i] = 20.0 * std::pow (1000.0, (double) i / (n - 1));
            // the averaged power over the bins of this point's 1/24 octave
            const auto band = [&] (const std::vector<double>& acc, int frames) {
                const auto k0 = juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (f[(std::size_t) i] * std::pow (2.0, -1.0 / 48.0) * fftSize / sr));
                const auto k1 = juce::jlimit (k0, fftSize / 2 - 1, (int) std::ceil (f[(std::size_t) i] * std::pow (2.0, 1.0 / 48.0) * fftSize / sr));
                double s = 0.0;
                for (int k = k0; k <= k1; ++k) s += acc[(std::size_t) k];
                return 10.0 * std::log10 (s / (k1 - k0 + 1) / juce::jmax (1, frames) + 1e-20);
            };
            cur[(std::size_t) i] = band (matchCur, curFrames);
            ref[(std::size_t) i] = band (matchRef, refFrames);
        }
        std::vector<std::pair<int, float>> changes;
        if (facet::matchEq (fx->params, f.data(), cur.data(), ref.data(), n, amount, matchBands, 1.0 / matchSmoothing, changes) > 0)
            set (changes); // one undo step
        repaint();
    }

    void showAnalyzerMenu()
    {
        juce::PopupMenu menu;
        const auto a = (int) analyzer (param01 (analyzerParam));
        for (int k = 0; k < 4; ++k)
            menu.addItem (tr (analyzerNames[k]), true, a == k, [this, k] { set ({ { analyzerParam, analyzerValue ((Analyzer) k) } }, false); });
        // round 128: the analyser's range, speed, tilt and freeze (Pro-Q's analyser settings)
        menu.addSeparator();
        juce::PopupMenu range, speed, tilt;
        for (int k = 0; k < 3; ++k)
            range.addItem (juce::String (60 + 30 * k) + " dB", true, (int) analyzerRangeDb (param01 (specRangeParam)) == 60 + 30 * k,
                           [this, k] { set ({ { specRangeParam, (float) k / 2.0f } }, false); });
        static const char* speeds[] = { TR_NOOP ("Very slow"), TR_NOOP ("Slow"), TR_NOOP ("Medium"), TR_NOOP ("Fast"), TR_NOOP ("Very fast") };
        for (int k = 0; k < 5; ++k)
        {
            speed.addItem (tr (speeds[k]), true, analyzerSpeed (param01 (specSpeedParam)) == k, [this, k] { set ({ { specSpeedParam, (float) k / 4.0f } }, false); });
            tilt.addItem (juce::String (1.5 * k, 1) + " dB/oct", true, std::abs (analyzerTiltDb (param01 (specTiltParam)) - 1.5 * k) < 0.01,
                          [this, k] { set ({ { specTiltParam, (float) k / 4.0f } }, false); });
        }
        menu.addSubMenu (tr ("Range"), range);
        menu.addSubMenu (tr ("Speed"), speed);
        menu.addSubMenu (tr ("Tilt"), tilt);
        menu.addItem (tr ("Freeze"), true, frozen, [this] { frozen = ! frozen; repaint(); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showShapeMenu (int b)
    {
        juce::PopupMenu menu;
        const auto sh = bandShape (b);
        for (int k = 1; k < shapeCount; ++k)
            menu.addItem (tr (shapeNames[k]), true, (int) sh == k, [this, b, k] { set ({ { param (b, 0), shapeValue ((Shape) k) } }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showPlacementMenu (int b)
    {
        juce::PopupMenu menu;
        for (int k = 0; k < placementCount; ++k)
            menu.addItem (placementLabel ((Placement) k), true, (int) bandPlacement (b) == k, [this, b, k] { set ({ { placementParam0 + b, placementValue ((Placement) k) } }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // right-click on OUT: the value menu plus the output options (polarity, pan mode)
    void showOutputMenu()
    {
        juce::PopupMenu menu;
        const auto spec = specFor (outputParam);
        menu.addSectionHeader (spec.name + ": " + spec.text (param01 (outputParam)));
        ui::addValueItems (menu, *this, &engine, spec, param01 (outputParam), [this] (double v) { set ({ { outputParam, (float) v } }); });
        menu.addSeparator();
        menu.addItem (tr ("Invert polarity"), true, param01 (invertParam) >= 0.5f, [this] { set ({ { invertParam, param01 (invertParam) >= 0.5f ? 0.0f : 1.0f } }); });
        menu.addItem (tr ("Auto gain"), true, param01 (autoGainParam) >= 0.5f, [this] { set ({ { autoGainParam, param01 (autoGainParam) >= 0.5f ? 0.0f : 1.0f } }); });
        juce::PopupMenu mode;
        mode.addItem (tr ("Pan: left / right"), true, ! panMidSide (param01 (panModeParam)), [this] { set ({ { panModeParam, 0.0f } }); });
        mode.addItem (tr ("Pan: mid / side"), true, panMidSide (param01 (panModeParam)), [this] { set ({ { panModeParam, 1.0f } }); });
        menu.addSubMenu (tr ("Pan mode"), mode);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showSlopeMenu (int b)
    {
        juce::PopupMenu menu;
        for (const auto d : facet::slopes)
            menu.addItem (juce::String (d) + " dB/oct", true, slopeDb (param01 (param (b, 4))) == d, [this, b, d] { set ({ { param (b, 4), slopeValue (d) } }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // --- mouse ---------------------------------------------------------------------------------------

    void showPhaseMenu()
    {
        juce::PopupMenu menu;
        const auto m = (int) phase (param01 (phaseParam));
        for (int k = 0; k < phaseCount; ++k)
            menu.addItem (tr (phaseNames[k]) + (k == 0 ? juce::String() : "  (" + tr ("latency {0} samples", { juce::String (latencyFor ((Phase) k, displayRate(), facet::resolution (param01 (resolutionParam)))) }) + ")"),
                          true, m == k, [this, k] { set ({ { phaseParam, phaseValue ((Phase) k) } }); });
        // round 144: the linear-phase resolution (the reference's Low … Maximum): a longer FIR reaches lower
        // frequencies, at the cost of latency and CPU
        menu.addSectionHeader (tr ("Linear phase resolution"));
        static const char* const resolutions[] { TR_NOOP ("Low resolution"), TR_NOOP ("Medium resolution"), TR_NOOP ("High resolution"), TR_NOOP ("Very high resolution"), TR_NOOP ("Maximum resolution") };
        const auto res = facet::resolution (param01 (resolutionParam));
        for (int k = 0; k < facet::resolutionCount; ++k)
            menu.addItem (tr (resolutions[k]) + "  (" + tr ("latency {0} samples", { juce::String (latencyFor (Phase::linear, displayRate(), k)) }) + ")",
                          true, res == k, [this, k] { set ({ { resolutionParam, facet::resolutionValue (k) } }); });
        // round 135: the character (the reference's Clean / Subtle / Warm)
        menu.addSectionHeader (tr ("Character"));
        static const char* const characters[] { TR_NOOP ("Clean"), TR_NOOP ("Subtle"), TR_NOOP ("Warm") };
        const auto ch = facet::character (param01 (characterParam));
        for (int k = 0; k < 3; ++k)
            menu.addItem (tr (characters[k]), true, ch == k, [this, k] { set ({ { characterParam, (float) k / 2.0f } }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // right-click on the graph away from the nodes: every band at once
    void showGraphMenu()
    {
        juce::PopupMenu menu;
        menu.addItem (tr ("Invert all gains"), [this] {
            std::vector<std::pair<int, float>> c;
            for (int b = 0; b < bands; ++b)
                if (bandOn (b)) c.push_back ({ param (b, 2), 1.0f - param01 (param (b, 2)) });
            set (c);
        });
        menu.addItem (tr ("Remove all bands"), [this] {
            std::vector<std::pair<int, float>> c { { soloParam, 0.0f } };
            for (int b = 0; b < bands; ++b) { c.push_back ({ param (b, 0), 0.0f }); c.push_back ({ rangeParam0 + b, 0.5f }); }
            set (c);
            selected = -1;
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void stepBand (int dir)
    {
        for (int k = 1; k <= bands; ++k)
        {
            const auto b = ((selected < 0 ? 0 : selected) + dir * k + bands * 2) % bands;
            if (bandOn (b)) { selected = b; repaint(); return; }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        dragging = Drag::none;
        // the preset bar in the header (round 129's undo / redo, A / B, presets; round 128 wires it into Facet)
        if (const auto part = bar.partAt (p); part != pluginheader::Part::none)
        {
            bar.click (part, *this);
            return;
        }
        bar.beginGesture();
        if (e.mods.isPopupMenu() && headerArea().contains (p)) // round 126: the editor's style
        {
            juce::PopupMenu m;
            skin::addStyleMenu (m, skin::styleKey (EffectKind::facetEq), [safe = juce::Component::SafePointer<juce::Component> (this)] {
                if (safe != nullptr) safe->repaint();
            });
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
            return;
        }
        std::vector<Control> all;
        if (const auto* c = controlAt (p, all))
        {
            const auto type = c->type;
            const auto prm = c->param;
            const auto index = c->index;
            if (e.mods.isPopupMenu())
            {
                if (type == Ctl::analyzer) { showAnalyzerMenu(); return; }
                if (type == Ctl::detector) { showDetectorMenu(); return; }
                if (type == Ctl::phase) { showPhaseMenu(); return; }
                if (type == Ctl::output) { showOutputMenu(); return; }
                if (type == Ctl::placementMenu) { showPlacementMenu (selected); return; }
                if (type == Ctl::spectral) { showDensityMenu (selected); return; }
                if (type == Ctl::match) { showMatchMenu(); return; }
                if (type == Ctl::spectralTilt) return;
                if (prm >= 0 && type != Ctl::solo && type != Ctl::dynamic && type != Ctl::shapeMenu && type != Ctl::slopeMenu && type != Ctl::power
                    && type != Ctl::autoGain)
                    ui::showValueMenu (*this, &engine, specFor (prm), param01 (prm), [this, prm] (double v) { set ({ { prm, (float) v } }); });
                return;
            }
            switch (type)
            {
                case Ctl::shapeMenu: showShapeMenu (selected); return;
                case Ctl::slopeMenu: showSlopeMenu (selected); return;
                case Ctl::remove: removeBand (selected); return;
                case Ctl::solo: toggleSolo (selected); return;
                case Ctl::tipSolo: toggleSolo (index); return;
                case Ctl::tipPower: case Ctl::power: { const auto b = type == Ctl::power ? selected : index; set ({ { powerParam0 + b, bandPowered (b) ? 1.0f : 0.0f } }); return; }
                case Ctl::autoGain: set ({ { prm, param01 (prm) >= 0.5f ? 0.0f : 1.0f } }); return;
                case Ctl::placementMenu: showPlacementMenu (selected); return;
                case Ctl::tipRemove: removeBand (index); tipBand = -1; return;
                case Ctl::prevBand: stepBand (-1); return;
                case Ctl::nextBand: stepBand (1); return;
                case Ctl::close: selected = -1; repaint(); return;
                case Ctl::piano: piano = ! piano; repaint(); return;
                case Ctl::dynamic: set ({ { prm, isDynamic (current()->params, selected) ? 0.5f : rangeValue (-6.0) } }); return;
                case Ctl::spectral: case Ctl::spectralTilt: set ({ { prm, param01 (prm) >= 0.5f ? 0.0f : 1.0f } }); return;
                case Ctl::match: showMatchMenu(); return;
                case Ctl::phase: showPhaseMenu(); return;
                case Ctl::analyzer: showAnalyzerMenu(); return;
                case Ctl::detector: showDetectorMenu(); return;
                case Ctl::scaleDb:
                {
                    const auto r = viewRange();
                    set ({ { displayRangeParam, displayRangeValue (r >= 30.0 ? 3.0 : r >= 12.0 ? 30.0 : r >= 6.0 ? 12.0 : 6.0) } }, false);
                    return;
                }
                default: break;
            }
            if (prm >= 0)
            {
                if (ui::isResetClick (e))
                {
                    set ({ { prm, (float) specFor (prm).defaultValue } });
                    return;
                }
                dragging = Drag::control;
                dragParam = prm;
                dragRect = c->r;
                valueDrag.begin (e, param01 (prm));
            }
            return;
        }

        if (panelShown() && panelArea().contains (p)) return; // the panel's background
        if (! plotArea().contains (p)) return;
        if (const auto rb = rangeHandleAt (p); rb >= 0 && ! e.mods.isPopupMenu())
        {
            dragging = Drag::range;
            dragBand = rb;
            return;
        }
        const auto b = nodeAt (p);
        if (b >= 0)
        {
            selected = b;
            if (e.mods.isPopupMenu()) { showBandMenu (b); return; }
            if (e.mods.isAltDown()) { removeBand (b); return; } // Alt-click deletes, as in the common EQs
            if (e.mods.isMiddleButtonDown()) { set ({ { param (b, 2), 0.5f } }); return; }
            dragging = Drag::node;
            dragBand = b;
            dragOffset = nodePosition (b) - p;
            dragStart = p;
            repaint();
            return;
        }
        if (e.mods.isPopupMenu()) { showGraphMenu(); return; }
        selected = -1; // a click on the empty graph closes the band panel
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging == Drag::control && dragParam >= 0)
        {
            const auto spec = specFor (dragParam);
            const auto v = valueDrag.update (e, spec);
            set ({ { dragParam, (float) v } });
            ui::showValueTip (*this, dragRect.toNearestInt(), spec.text (v));
            return;
        }
        if (dragging == Drag::node && bandOn (dragBand))
        {
            auto p = e.position + dragOffset;
            const auto b = dragBand;
            std::vector<std::pair<int, float>> c;
            const auto fine = e.mods.isCtrlDown() || e.mods.isCommandDown();
            if (fine) // fine: a quarter of the mouse movement
                p = nodePosition (b) + (p - nodePosition (b)) * 0.25f;
            const auto d = e.position - dragStart;
            const auto lockX = e.mods.isShiftDown() && std::abs (d.y) > std::abs (d.x); // Shift: only the main direction
            const auto lockY = e.mods.isShiftDown() && ! lockX;
            if (! lockX) c.push_back ({ param (b, 1), hzValue (hzForX (p.x)) });
            if (hasGain (bandShape (b)) && ! lockY)
            {
                const auto scale = juce::jmax (0.01, gainScale (param01 (scaleParam)));
                c.push_back ({ param (b, 2), gainValue (juce::jlimit (-30.0, 30.0, dbForY (p.y) / scale)) });
            }
            set (c);
            ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (nodePosition (b).toInt()), bandSummary (b));
            return;
        }
        if (dragging == Drag::range && bandOn (dragBand))
        {
            const auto b = dragBand;
            const auto scale = juce::jmax (0.01, gainScale (param01 (scaleParam)));
            const auto r = dbForY (e.position.y) / scale - gainDb (param01 (param (b, 2)));
            set ({ { rangeParam0 + b, rangeValue (juce::jlimit (-30.0, 30.0, r)) } });
            ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (rangeHandle (b).toInt()), tr ("Range") + " " + valueText (rangeParam0 + b));
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragging = Drag::none;
        valueDrag.end();
        ui::hideValueTip();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! plotArea().contains (e.position)) return;
        if (const auto b = nodeAt (e.position); b >= 0) removeBand (b);
        else addBand (e.position);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        std::vector<Control> all;
        if (const auto* c = controlAt (e.position, all); c != nullptr && c->param >= 0 && c->type != Ctl::phase && c->type != Ctl::power
            && c->type != Ctl::autoGain && c->type != Ctl::placementMenu && c->type != Ctl::spectral && c->type != Ctl::spectralTilt && c->type != Ctl::match)
        {
            const auto prm = c->param;
            const auto spec = specFor (prm);
            if (const auto v = ui::wheelValue (param01 (prm), e, wheel, spec))
            {
                set ({ { prm, (float) *v } });
                ui::flashValueTip (*this, c->r.toNearestInt(), spec.text (*v));
            }
            return;
        }
        // over the graph: the Q of the band under the mouse (or the selected one)
        if (! plotArea().contains (e.position)) return;
        auto b = nodeAt (e.position);
        if (b < 0) b = selected;
        if (! bandOn (b) || wheel.deltaY == 0.0f) return;
        const auto step = (e.mods.isCtrlDown() ? 0.04 : 0.15) * (wheel.deltaY > 0 ? 1.0 : -1.0);
        const auto q0 = q (param01 (param (b, 3)));
        set ({ { param (b, 3), qValue (juce::jlimit (0.025, 40.0, q0 * std::exp (step * 2.0))) } });
        ui::flashValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (nodePosition (b).toInt()), "Q " + valueText (param (b, 3)));
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        mouse = e.position;
        bar.hover (e.position);
        const auto h = nodeAt (e.position);
        if (h != hover) hover = h;
        // the read-out box stays while the mouse is on its node or inside the box (to reach solo / remove)
        if (h >= 0) tipBand = h;
        else if (tipBand >= 0 && ! (bandOn (tipBand) && tipArea (tipBand).expanded (4.0f).contains (e.position))) tipBand = -1;
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        mouse = { -1.0f, -1.0f };
        bar.hoverExit();
        hover = -1;
        tipBand = -1;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if ((key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey) && bandOn (selected))
        {
            removeBand (selected);
            return true;
        }
        if (key.getTextCharacter() == 's' && bandOn (selected))
        {
            toggleSolo (selected);
            return true;
        }
        return false;
    }

    juce::String bandSummary (int b) const
    {
        juce::String t = tr (shapeNames[(int) bandShape (b)]) + "  " + hzText (hz (param01 (param (b, 1))));
        if (hasGain (bandShape (b))) t << "  " << valueText (param (b, 2));
        t << "  Q " << valueText (param (b, 3));
        return t;
    }

    ui::Hint hintAt (juce::Point<int> position) override
    {
        ui::Hint h;
        const auto p = position.toFloat();
        if (const auto part = bar.partAt (p); part != pluginheader::Part::none)
            return { bar.hintFor (part), bar.presetLabel(), false, true };
        std::vector<Control> all;
        if (const auto* c = controlAt (p, all))
        {
            switch (c->type)
            {
                case Ctl::phase: h.text = tr ("Processing: zero latency (minimum phase), natural phase (the analog curve's phase) or linear phase (no phase shift); the latency is reported to PDC"); h.hasMenu = true; return h;
                case Ctl::analyzer: h.text = tr ("Spectrum analyzer: off, pre, post or pre + post"); h.hasMenu = true; return h;
                case Ctl::piano: h.text = tr ("Frequency scale: numbers or a piano keyboard"); return h;
                case Ctl::prevBand: case Ctl::nextBand: h.text = tr ("Previous / next band"); return h;
                case Ctl::close: h.text = tr ("Close the band panel (click on the empty graph does the same)"); return h;
                case Ctl::tipSolo: h.text = tr ("Solo: hear only this band's region (S)"); return h;
                case Ctl::tipRemove: h.text = tr ("Remove the band (Delete)"); return h;
                case Ctl::tipPower: case Ctl::power: h.text = tr ("Band on / off: a band switched off keeps its settings"); return h;
                case Ctl::placementMenu: h.text = tr ("Stereo placement: the band works on both channels, the left or right one, or the mid or side signal"); h.hasMenu = true; return h;
                case Ctl::autoGain: h.text = tr ("Auto gain: takes back the EQ curve's overall level change so louder never fools the ear"); return h;
                case Ctl::output: h.text = tr ("Output level - right-click: polarity, auto gain, pan mode"); h.value = valueText (outputParam); h.hasMenu = true; return h;
                case Ctl::detector: h.text = tr ("What the dynamic bands listen to: the EQ's input or a sidechain track"); h.hasMenu = true; return h;
                case Ctl::scaleDb: h.text = tr ("Graph scale: click to switch between 3, 6, 12 and 30 dB"); return h;
                case Ctl::remove: h.text = tr ("Remove the band (Delete)"); return h;
                case Ctl::solo: h.text = tr ("Solo: hear only this band's region (S)"); return h;
                case Ctl::dynamic: h.text = tr ("Dynamic EQ: the band's gain moves by up to its range when its region goes over the threshold"); return h;
                case Ctl::spectral: h.text = tr ("Spectral dynamics: the band works frequency by frequency, only where the sound goes over the threshold (adds 5120 samples of latency) - right-click: density"); h.hasMenu = true; return h;
                case Ctl::spectralTilt: h.text = tr ("Tilt: the spectral threshold slopes 4.5 dB/oct so the band treats a natural (pink) spectrum evenly"); return h;
                case Ctl::match: h.text = tr ("EQ match: capture a reference spectrum and this track's, then let Facet place bands that make this track sound like the reference") + " (" + matchStateText() + ")"; h.hasMenu = true; return h;
                case Ctl::shapeMenu: h.text = tr ("Band shape"); h.hasMenu = true; return h;
                case Ctl::slopeMenu: h.text = tr ("Slope"); h.hasMenu = true; return h;
                default: break;
            }
            if (c->param >= 0)
                return ui::valueHint (specFor (c->param), param01 (c->param));
            return h;
        }
        if (plotArea().contains (p))
        {
            if (const auto b = nodeAt (p); b >= 0)
                h.text = tr ("Band {0}", { juce::String (b + 1) }) + ": " + bandSummary (b) + " - "
                         + tr ("drag = frequency and gain (Ctrl fine, Shift one direction only); wheel = Q; Alt-click or double-click = remove; right-click = menu");
            else if (rangeHandleAt (p) >= 0)
                h.text = tr ("Dynamic range: drag up or down");
            else
                h.text = tr ("Double-click to add a band; mouse wheel = Q of the selected band");
            h.hasMenu = true;
        }
        return h;
    }

    // --- painting ------------------------------------------------------------------------------------

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const skin::StyleScope styleScope (skin::styleFor (EffectKind::facetEq)); // round 126: one style per plugin
        g.fillAll (colour::bg0);
        const auto* fx = current();
        if (fx == nullptr)
        {
            g.setColour (colour::textSecondary);
            g.setFont (font (type::body));
            g.drawText (tr ("The effect was removed from this slot."), getLocalBounds(), juce::Justification::centred);
            return;
        }
        auto all = controls();
        paintGraph (g, *fx);
        paintHeader (g, *fx);
        paintBar (g, *fx, all);
        if (panelShown()) paintPanel (g, *fx, all);
        if (tipBand >= 0 && bandOn (tipBand) && dragging == Drag::none) paintTip (g, *fx, tipBand);
    }

    void paintHeader (juce::Graphics& g, const EffectSlot& fx)
    {
        using namespace theme;
        const auto h = headerArea();
        skin::headerBar (g, h, colour::accent);
        skin::brandMark (g, juce::Rectangle<float> (8.0f, 0.0f, 20.0f, h.getHeight()));
        skin::powerLed (g, { 36.0f, h.getCentreY() }, 7.0f, fx.enabled);
        const auto w = skin::titleText (g, juce::Rectangle<float> (46.0f, 0.0f, 120.0f, h.getHeight()), "Facet EQ", colour::accent);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::small));
        g.drawText (trackName() + "  -  " + tr ("slot {0}", { juce::String (slot + 1) }), juce::Rectangle<float> (58.0f + w, 0.0f, 300.0f, h.getHeight()),
                    juce::Justification::centredLeft, true);
        // the preset bar (undo / redo, A / B, presets) on the right, the graph's gestures between
        const auto left = bar.place (h.withTrimmedLeft (w + 300.0f));
        bar.paint (g, colour::accent);
        g.setColour (colour::textDisabled);
        g.setFont (font (type::caption));
        g.drawText (tr ("Double-click: new band  -  drag: frequency / gain  -  wheel: Q  -  right-click: menu"),
                    left.withTrimmedRight (8.0f), juce::Justification::centredRight, true);
    }

    void paintBar (juce::Graphics& g, const EffectSlot& fx, const std::vector<Control>& all)
    {
        using namespace theme;
        const auto bar = barArea();
        skin::bottomBar (g, bar);
        for (const auto& c : all)
        {
            const auto hover = c.r.contains (mouse);
            if (c.type == Ctl::piano)
            {
                g.setColour (piano ? colour::accent : (hover ? colour::textPrimary : colour::textSecondary));
                const auto k = c.r.reduced (4.0f, 4.0f);
                for (int i = 0; i < 4; ++i)
                    g.drawRect (juce::Rectangle<float> (k.getX() + k.getWidth() * (float) i / 4.0f, k.getY(), k.getWidth() / 4.0f, k.getHeight()), 1.0f);
                for (int i = 1; i < 4; ++i)
                    g.fillRect (juce::Rectangle<float> (k.getX() + k.getWidth() * (float) i / 4.0f - 1.5f, k.getY(), 3.0f, k.getHeight() * 0.55f));
            }
            else if (c.type == Ctl::match) // round 152: lit while it captures
            {
                const auto on = capture != Capture::none;
                skin::button (g, c.r, on, hover, colour::accent);
                g.setColour (on || (refFrames > 0 && curFrames > 0) ? colour::textPrimary : colour::textSecondary);
                g.setFont (font (type::small, on));
                g.drawText (tr ("Match"), c.r, juce::Justification::centred);
            }
            else if (c.type == Ctl::phase || c.type == Ctl::analyzer || c.type == Ctl::detector)
            {
                juce::String caption, value;
                if (c.type == Ctl::phase)
                {
                    caption = tr ("Processing:");
                    value = tr (phaseNames[(int) phase (param01 (phaseParam))]);
                    static const char* const characters[] { TR_NOOP ("Clean"), TR_NOOP ("Subtle"), TR_NOOP ("Warm") };
                    if (const auto ch = facet::character (param01 (characterParam)); ch > 0) value << juce::String::fromUTF8 (" · ") << tr (characters[ch]);
                    static const char* const resShort[] { TR_NOOP ("Low resolution"), TR_NOOP ("Medium resolution"), TR_NOOP ("High resolution"), TR_NOOP ("Very high resolution"), TR_NOOP ("Maximum resolution") };
                    if (phase (param01 (phaseParam)) == Phase::linear) value << juce::String::fromUTF8 (" · ") << tr (resShort[facet::resolution (param01 (resolutionParam))]); // round 144
                }
                else if (c.type == Ctl::analyzer) { caption = tr ("Analyzer:"); value = tr (analyzerShort[(int) analyzer (param01 (analyzerParam))]); }
                else
                {
                    caption = tr ("Detector:");
                    const auto side = sidechainDetector (param01 (detectorParam));
                    value = side ? tr ("Sidechain") : tr ("Input");
                    if (side && fx.sidechain > 0 && fx.sidechain < (int) engine.getSong().mixer.size()) value << " " << juce::String (fx.sidechain);
                }
                if (hover)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.06f));
                    g.fillRoundedRectangle (c.r, radius::md);
                }
                g.setFont (font (type::small));
                g.setColour (colour::textDisabled);
                const auto cw = juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), caption) + 6.0f;
                g.drawText (caption, c.r.withWidth (cw).translated (6.0f, 0.0f), juce::Justification::centredLeft);
                g.setColour (colour::textPrimary);
                g.setFont (font (type::small, true));
                g.drawText (value + juce::String::fromUTF8 (" \xe2\x96\xbe"), c.r.withTrimmedLeft (cw + 6.0f), juce::Justification::centredLeft, true);
            }
            else if (c.type == Ctl::autoGain)
            {
                const auto on = param01 (autoGainParam) >= 0.5f;
                skin::button (g, c.r, on, hover, colour::accent);
                g.setColour (on ? colour::textPrimary : colour::textSecondary);
                g.setFont (font (type::small, on));
                g.drawText (tr ("Auto gain"), c.r, juce::Justification::centred);
            }
            else if (c.type == Ctl::output || c.type == Ctl::scale || c.type == Ctl::pan)
            {
                skin::valuePlate (g, c.r, radius::md);
                if (hover)
                {
                    g.setColour (colour::accent.withAlpha (0.5f));
                    g.drawRoundedRectangle (c.r.reduced (0.5f), radius::md, 1.0f);
                }
                g.setColour (colour::textDisabled);
                g.setFont (font (type::caption));
                g.drawText (c.type == Ctl::output ? tr ("OUT") : c.type == Ctl::pan ? (panMidSide (param01 (panModeParam)) ? tr ("M/S") : tr ("PAN")) : tr ("SCALE"),
                            c.r.reduced (6.0f, 0.0f), juce::Justification::centredLeft);
                g.setColour (colour::textPrimary);
                g.setFont (font (type::small, true));
                g.drawText (valueText (c.param), c.r.reduced (6.0f, 0.0f), juce::Justification::centredRight);
            }
        }
        // latency of the processing mode (PDC compensates it)
        auto lat = latencyFor (phase (param01 (phaseParam)), displayRate(), facet::resolution (param01 (resolutionParam)));
        for (int b = 0; b < bands; ++b) // round 152: a spectral band adds the STFT's latency
            if (isSpectral (fx.params, b)) { lat += spectralLatency (displayRate()); break; }
        if (lat > 0)
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (type::caption));
            g.drawFittedText (tr ("{0} smp", { juce::String (lat) }), juce::Rectangle<float> (588.0f, bar.getY(), bar.getRight() - 376.0f - 588.0f, bar.getHeight()).toNearestInt(),
                              juce::Justification::centred, 1, 0.7f);
        }
    }

    // the read-out box of a hovered node
    void paintTip (juce::Graphics& g, const EffectSlot& fx, int b)
    {
        using namespace theme;
        const auto r = tipArea (b);
        skin::floatingPanel (g, r, bandColour (b), radius::lg);
        g.setFont (font (type::small, true));
        g.setColour (colour::textPrimary);
        auto t = r.reduced (8.0f, 4.0f);
        g.drawText (hzText (hz (param01 (param (b, 1)))), t.removeFromTop (17.0f).withTrimmedRight (60.0f), juce::Justification::centredLeft);
        g.setFont (font (type::small));
        g.drawText (hasGain (bandShape (b)) ? valueText (param (b, 2)) : tr (shapeNames[(int) bandShape (b)]), t.removeFromTop (16.0f), juce::Justification::centredLeft);
        g.setColour (colour::textSecondary);
        const auto pl = bandPlacement (b);
        g.drawText ("Q " + valueText (param (b, 3)) + (pl != Placement::stereo ? "   " + placementLabel (pl) : juce::String()),
                    t.removeFromTop (16.0f), juce::Justification::centredLeft);
        // power, solo (headphones) and remove (×)
        const auto pw = juce::Rectangle<float> (r.getRight() - 62.0f, r.getY() + 4.0f, 18.0f, 18.0f);
        drawPowerGlyph (g, pw, bandPowered (b) ? bandColour (b) : (pw.contains (mouse) ? colour::textPrimary : colour::textDisabled));
        const auto solo = juce::Rectangle<float> (r.getRight() - 42.0f, r.getY() + 4.0f, 18.0f, 18.0f);
        const auto rm = juce::Rectangle<float> (r.getRight() - 22.0f, r.getY() + 4.0f, 18.0f, 18.0f);
        const auto soloOn = soloBand (fx.params[(std::size_t) soloParam]) == b;
        g.setColour (soloOn ? colour::warning : (solo.contains (mouse) ? colour::textPrimary : colour::textSecondary));
        juce::Path phones;
        const auto pc = solo.reduced (3.0f);
        phones.addCentredArc (pc.getCentreX(), pc.getCentreY() + 1.0f, pc.getWidth() * 0.45f, pc.getHeight() * 0.45f, 0.0f,
                              -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
        g.strokePath (phones, juce::PathStrokeType (1.5f));
        g.fillRoundedRectangle (juce::Rectangle<float> (pc.getX() - 0.5f, pc.getCentreY(), 3.5f, 6.0f), 1.0f);
        g.fillRoundedRectangle (juce::Rectangle<float> (pc.getRight() - 3.0f, pc.getCentreY(), 3.5f, 6.0f), 1.0f);
        g.setColour (rm.contains (mouse) ? colour::accent : colour::textSecondary);
        g.drawLine (rm.getX() + 5.0f, rm.getY() + 5.0f, rm.getRight() - 5.0f, rm.getBottom() - 5.0f, 1.5f);
        g.drawLine (rm.getRight() - 5.0f, rm.getY() + 5.0f, rm.getX() + 5.0f, rm.getBottom() - 5.0f, 1.5f);
    }

    double displayRate() const
    {
        const auto* times = engine.getProcessTimes();
        const auto sr = times != nullptr ? times->analyzer.sampleRate.load (std::memory_order_relaxed) : 48000.0;
        return sr > 8000.0 ? sr : 48000.0;
    }

    void paintGraph (juce::Graphics& g, const EffectSlot& fx)
    {
        using namespace theme;
        const auto area = graphArea();
        const auto plot = plotArea();
        skin::canvas (g, area, colour::accent);

        // grid: frequencies (numbers or a piano keyboard along the bottom)
        g.setFont (font (type::caption));
        for (double f : { 20.0, 30.0, 40.0, 50.0, 60.0, 70.0, 80.0, 90.0, 100.0, 200.0, 300.0, 400.0, 500.0, 600.0, 700.0, 800.0, 900.0, 1000.0, 2000.0,
                          3000.0, 4000.0, 5000.0, 6000.0, 7000.0, 8000.0, 9000.0, 10000.0, 20000.0 })
        {
            const auto major = f == 20.0 || f == 50.0 || f == 100.0 || f == 200.0 || f == 500.0 || f == 1000.0 || f == 2000.0 || f == 5000.0 || f == 10000.0 || f == 20000.0;
            const auto x = xForHz (f);
            g.setColour (major ? colour::gridBeat.brighter (0.15f) : colour::gridStep);
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            if (major && ! piano)
            {
                g.setColour (colour::textDisabled);
                g.drawText (f >= 1000.0 ? juce::String ((int) (f / 1000.0)) + "k" : juce::String ((int) f),
                            juce::Rectangle<float> (x - 20.0f, plot.getBottom() + 3.0f, 40.0f, 14.0f), juce::Justification::centred);
            }
        }
        if (piano) // one key per semitone; C labelled
        {
            const auto strip = juce::Rectangle<float> (plot.getX(), plot.getBottom() + 2.0f, plot.getWidth(), 16.0f);
            for (int n = 12; n < 136; ++n)
            {
                const auto f0 = 440.0 * std::pow (2.0, (n - 69.5) / 12.0), f1 = 440.0 * std::pow (2.0, (n - 68.5) / 12.0);
                const auto x0 = xForHz (f0), x1 = xForHz (f1);
                if (x1 < strip.getX() || x0 > strip.getRight()) continue;
                const auto pc = n % 12;
                const auto black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                g.setColour (black ? colour::keyBlack : colour::keyWhite.withAlpha (0.75f));
                g.fillRect (juce::Rectangle<float> (x0, strip.getY(), juce::jmax (1.0f, x1 - x0 - 0.5f), strip.getHeight()));
                if (pc == 0 && x1 - x0 > 2.0f)
                {
                    g.setColour (colour::bg0);
                    g.drawText ("C" + juce::String (n / 12 - 1), juce::Rectangle<float> (x0 - 2.0f, strip.getY(), 30.0f, strip.getHeight()), juce::Justification::centredLeft);
                }
            }
        }
        // gain lines with their labels (warm), the analyser's dBFS beside them (grey)
        const auto rangeDb = viewRange();
        const auto step = rangeDb <= 3.0 ? 1.0 : rangeDb <= 6.0 ? 2.0 : rangeDb <= 12.0 ? 3.0 : 6.0;
        for (double db = -rangeDb; db <= rangeDb + 0.01; db += step)
        {
            const auto y = yForDb (db);
            g.setColour (std::abs (db) < 0.01 ? colour::gridBar : colour::gridBeat);
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (std::abs (db) < 0.01 ? colour::textPrimary : colour::accentHover.withAlpha (0.85f));
            if (y > plot.getY() + 22.0f)
                g.drawText ((db > 0 ? "+" : "") + juce::String (db, step < 1.5 && std::abs (db - std::round (db)) > 0.01 ? 1 : 0),
                        juce::Rectangle<float> (plot.getRight() + 4.0f, y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight);
        }
        g.setColour (colour::textDisabled);
        for (double db = -10.0; db >= specFloor() + 5.0; db -= 10.0)
        {
            const auto y = yForSpectrum (db);
            if (y > plot.getY() + 24.0f)
                g.drawText (juce::String ((int) db), juce::Rectangle<float> (plot.getRight() + 34.0f, y - 7.0f, 28.0f, 14.0f), juce::Justification::centredRight);
        }
        {
            const auto r = juce::Rectangle<float> (plot.getRight() + 6.0f, plot.getY() + 2.0f, 50.0f, 18.0f);
            g.setColour (r.contains (mouse) ? colour::bg5 : colour::bg4);
            g.fillRoundedRectangle (r, radius::md);
            g.setColour (colour::accentHover);
            g.setFont (font (type::small, true));
            g.drawText (juce::String ((int) rangeDb) + " dB", r, juce::Justification::centred);
        }

        // analyser (behind the curves)
        const auto mode = analyzer (param01 (analyzerParam));
        if (mode != Analyzer::off && ! specPre.empty())
        {
            auto spectrumPath = [&] (const std::vector<float>& db, bool closed) {
                juce::Path p;
                const auto n = (int) db.size();
                for (int i = 0; i < n; ++i)
                {
                    const auto x = plot.getX() + plot.getWidth() * (float) i / (float) (n - 1);
                    const auto y = yForSpectrum (db[(std::size_t) i]);
                    if (i == 0) p.startNewSubPath (x, closed ? plot.getBottom() : y);
                    if (i == 0 && closed) p.lineTo (x, y);
                    else p.lineTo (x, y);
                }
                if (closed)
                {
                    p.lineTo (plot.getRight(), plot.getBottom());
                    p.closeSubPath();
                }
                return p;
            };
            g.saveState();
            g.reduceClipRegion (plot.toNearestInt());
            if (mode == Analyzer::pre || mode == Analyzer::both)
            {
                g.setGradientFill (juce::ColourGradient (colour::bg5.withAlpha (0.7f), 0.0f, plot.getY(), colour::bg5.withAlpha (0.25f), 0.0f, plot.getBottom(), false));
                g.fillPath (spectrumPath (specPre, true));
            }
            if (mode == Analyzer::post || mode == Analyzer::both)
            {
                const auto p = spectrumPath (specPost, true);
                // round 126: the analyser glows — a deep fill of the accent rising to a lit edge
                juce::ColourGradient fillGrad (colour::accent.withAlpha (0.34f), 0.0f, plot.getY() + plot.getHeight() * 0.25f, colour::accent.withAlpha (0.02f), 0.0f,
                                               plot.getBottom(), false);
                fillGrad.addColour (0.5, colour::accent.darker (0.3f).withAlpha (0.14f));
                g.setGradientFill (fillGrad);
                g.fillPath (p);
                const auto edge = spectrumPath (specPost, false);
                if (! skin::reducedEffects())
                {
                    g.setColour (colour::accent.withAlpha (0.18f));
                    g.strokePath (edge, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                g.setColour (colour::accent.brighter (0.2f).withAlpha (0.85f));
                g.strokePath (edge, juce::PathStrokeType (1.2f));
            }
            g.restoreState();
        }

        // curves: each band (its own colour, faint), the dynamic span, then the sum
        const auto columns = juce::jmax (64, (int) plot.getWidth() / 2);
        std::vector<double> freqs ((std::size_t) columns + 1), sum ((std::size_t) columns + 1, outputDb (fx.params[(std::size_t) outputParam])), band ((std::size_t) columns + 1),
            span ((std::size_t) columns + 1);
        for (int i = 0; i <= columns; ++i)
            freqs[(std::size_t) i] = hzForX (plot.getX() + plot.getWidth() * (float) i / (float) columns);
        const auto sr = displayRate();
        const auto solo = soloBand (fx.params[(std::size_t) soloParam]);
        g.saveState();
        g.reduceClipRegion (plot.toNearestInt());
        for (int b = 0; b < bands; ++b)
        {
            if (! bandOn (b)) continue;
            const auto dyn = isDynamic (fx.params, b);
            const auto live = dyn ? liveDynamic (b) : 0.0;
            bandCurveDb (fx.params, b, sr, freqs.data(), columns + 1, band.data(), live);
            for (int i = 0; i <= columns; ++i) sum[(std::size_t) i] += band[(std::size_t) i];
            const auto colour = bandColour (b);
            const auto isSel = b == selected || b == hover;
            juce::Path p;
            for (int i = 0; i <= columns; ++i)
            {
                const auto x = plot.getX() + plot.getWidth() * (float) i / (float) columns;
                const auto y = yForDb (band[(std::size_t) i]);
                if (i == 0) p.startNewSubPath (x, y);
                else p.lineTo (x, y);
            }
            if (isSel)
            {
                auto fill = p;
                fill.lineTo (plot.getRight(), yForDb (0.0));
                fill.lineTo (plot.getX(), yForDb (0.0));
                fill.closeSubPath();
                g.setGradientFill (juce::ColourGradient (colour.withAlpha (solo == b ? 0.36f : 0.22f), 0.0f, plot.getY(), colour.withAlpha (solo == b ? 0.18f : 0.06f), 0.0f,
                                                         plot.getBottom(), false));
                g.fillPath (fill);
            }
            if (dyn) // the range the band can move through: static curve ↔ static + range
            {
                std::vector<double> lo ((std::size_t) columns + 1), hi ((std::size_t) columns + 1);
                bandCurveDb (fx.params, b, sr, freqs.data(), columns + 1, lo.data(), 0.0);
                bandCurveDb (fx.params, b, sr, freqs.data(), columns + 1, hi.data(), rangeDb_ (b));
                juce::Path area2;
                for (int i = 0; i <= columns; ++i)
                {
                    const auto x = plot.getX() + plot.getWidth() * (float) i / (float) columns;
                    if (i == 0) area2.startNewSubPath (x, yForDb (lo[0]));
                    else area2.lineTo (x, yForDb (lo[(std::size_t) i]));
                }
                for (int i = columns; i >= 0; --i)
                    area2.lineTo (plot.getX() + plot.getWidth() * (float) i / (float) columns, yForDb (hi[(std::size_t) i]));
                area2.closeSubPath();
                g.setColour (colour.withAlpha (0.12f));
                g.fillPath (area2);
            }
            g.setColour (colour.withAlpha (isSel ? 0.9f : 0.45f));
            g.strokePath (p, juce::PathStrokeType (isSel ? 1.6f : 1.0f));
        }

        // the sum
        juce::Path total;
        for (int i = 0; i <= columns; ++i)
        {
            const auto x = plot.getX() + plot.getWidth() * (float) i / (float) columns;
            const auto y = yForDb (sum[(std::size_t) i]);
            if (i == 0) total.startNewSubPath (x, y);
            else total.lineTo (x, y);
        }
        const auto curveColour = fx.enabled ? colour::textPrimary : colour::textDisabled;
        if (fx.enabled)
        {
            // round 126: the area between the sum and 0 dB lit in the accent, the sum a white-hot line in a red glow
            auto area0 = total;
            area0.lineTo (plot.getRight(), yForDb (0.0));
            area0.lineTo (plot.getX(), yForDb (0.0));
            area0.closeSubPath();
            g.setColour (colour::accent.withAlpha (0.16f));
            g.fillPath (area0);
            if (! skin::reducedEffects())
            {
                g.setColour (colour::accent.withAlpha (0.12f));
                g.strokePath (total, juce::PathStrokeType (12.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            g.setColour (colour::accent.withAlpha (0.4f));
            g.strokePath (total, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.setColour (curveColour);
        g.strokePath (total, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.restoreState();

        // nodes
        for (int b = 0; b < bands; ++b)
        {
            if (! bandOn (b)) continue;
            const auto pos = nodePosition (b);
            const auto colour = bandColour (b);
            const auto isSel = b == selected;
            if (hasGain (bandShape (b)) && isDynamic (fx.params, b))
            {
                // the live position and the range handle
                const auto scale = gainScale (param01 (scaleParam));
                const auto liveY = yForDb ((gainDb (param01 (param (b, 2))) + liveDynamic (b)) * scale);
                const auto rh = rangeHandle (b);
                g.setColour (colour.withAlpha (0.5f));
                g.drawLine (pos.x, pos.y, rh.x, rh.y, 1.0f);
                juce::Path diamond;
                diamond.addPolygon (rh, 4, 5.0f, juce::MathConstants<float>::pi * 0.25f);
                g.setColour (isSel ? colour : colour.withAlpha (0.6f));
                g.fillPath (diamond);
                g.setColour (colour.withAlpha (0.9f));
                g.drawEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ pos.x, liveY }), 1.5f);
            }
            const auto r = isSel ? 9.0f : 7.5f;
            skin::bloom (g, pos, r * (isSel ? 3.4f : 2.4f), colour, isSel || b == hover ? 0.55f : 0.35f); // round 126: every node glows
            if (isSel)
            {
                g.setColour (colour.withAlpha (0.3f));
                g.drawEllipse (juce::Rectangle<float> (r * 3.2f, r * 3.2f).withCentre (pos), 1.5f);
            }
            const auto nodeColour = soloBand (fx.params[(std::size_t) soloParam]) == b ? colour::warning : colour;
            const auto node = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos);
            g.setColour (juce::Colours::black.withAlpha (0.45f));
            g.fillEllipse (node.translated (0.0f, 1.5f).expanded (1.0f));
            if (bandPowered (b))
            {
                g.setGradientFill (juce::ColourGradient (nodeColour.brighter (0.35f), pos.x, node.getY(), nodeColour.darker (0.15f), pos.x, node.getBottom(), false));
                g.fillEllipse (node);
                g.setColour (juce::Colours::white.withAlpha (0.35f));
                g.drawEllipse (node.reduced (0.5f), 1.0f);
                g.setColour (colour::bg0);
            }
            else // switched off: a hollow node in the band's colour (round 128)
            {
                g.setColour (colour::bg1);
                g.fillEllipse (node);
                g.setColour (nodeColour.withAlpha (0.8f));
                g.drawEllipse (node.reduced (0.75f), 1.5f);
            }
            g.setFont (font (type::caption, true));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (pos), juce::Justification::centred);
            if (const auto pl = bandPlacement (b); pl != Placement::stereo) // L / R / M / S beside the node
            {
                static const char* tags[] = { "", "L", "R", "M", "S" };
                const auto tag = juce::Rectangle<float> (13.0f, 12.0f).withCentre ({ pos.x + r + 7.0f, pos.y - r - 2.0f });
                g.setColour (colour::bg0.withAlpha (0.85f));
                g.fillRoundedRectangle (tag, 3.0f);
                g.setColour (colour);
                g.setFont (font (type::caption, true));
                g.drawText (tags[(int) pl], tag, juce::Justification::centred);
            }
            if (b == hover || isSel)
            {
                g.setColour (colour::textPrimary);
                g.drawEllipse (juce::Rectangle<float> (r * 2.0f + 3.0f, r * 2.0f + 3.0f).withCentre (pos), 1.2f);
            }
        }

        // what is under the mouse: frequency, note, gain of the curve
        if (plot.contains (mouse))
        {
            const auto f = hzForX (mouse.x);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            g.drawText (hzText (f) + "  " + noteName (f) + "   " + juce::String (dbForY (mouse.y), 1) + " dB",
                        juce::Rectangle<float> (plot.getX() + 8.0f, plot.getY() + 6.0f, 260.0f, 14.0f), juce::Justification::centredLeft);
        }
        bool any = false;
        for (int b = 0; b < bands; ++b) any = any || bandOn (b);
        if (! any)
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (type::body));
            g.drawText (tr ("Double-click to add a band"), plot, juce::Justification::centred);
        }
    }

    // the selected band's floating panel (over the bottom of the graph)
    void paintPanel (juce::Graphics& g, const EffectSlot& fx, const std::vector<Control>& all)
    {
        using namespace theme;
        const auto panel = panelArea();
        const auto colourB = bandColour (selected);
        skin::floatingPanel (g, panel, colourB);
        g.setColour (colourB.withAlpha (0.35f));
        g.fillRoundedRectangle (juce::Rectangle<float> (panel.getX() + 12.0f, panel.getY() - 2.0f, 50.0f, 7.0f), 3.5f);
        g.setColour (colourB);
        g.fillRoundedRectangle (juce::Rectangle<float> (panel.getX() + 14.0f, panel.getY() - 1.0f, 46.0f, 3.0f), 1.5f);
        const auto solo = soloBand (fx.params[(std::size_t) soloParam]);

        auto drawButton = [&] (juce::Rectangle<float> r, const juce::String& text, bool on, juce::Colour onColour) {
            skin::button (g, r, on, r.contains (mouse), onColour);
            g.setColour (on ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (type::small, on));
            g.drawText (text, r.reduced (6.0f, 0.0f), juce::Justification::centred, true);
        };
        auto drawArrow = [&] (juce::Rectangle<float> r, bool left) {
            juce::Path a;
            const auto c = r.getCentre();
            if (left) a.addTriangle (c.x + 3.0f, c.y - 5.0f, c.x + 3.0f, c.y + 5.0f, c.x - 3.0f, c.y);
            else a.addTriangle (c.x - 3.0f, c.y - 5.0f, c.x - 3.0f, c.y + 5.0f, c.x + 3.0f, c.y);
            g.setColour (r.contains (mouse) ? colour::textPrimary : colour::textSecondary);
            g.fillPath (a);
        };

        // the band's power button in its colour (round 128: Pro-Q's band power)
        {
            const auto pr = juce::Rectangle<float> (20.0f, 20.0f).withCentre ({ panel.getX() + 22.0f, panel.getY() + 16.0f });
            const auto on = bandPowered (selected);
            g.setColour (on ? colourB : colour::bg2);
            g.fillEllipse (pr);
            if (! on)
            {
                g.setColour (colourB.withAlpha (0.7f));
                g.drawEllipse (pr.reduced (0.75f), 1.5f);
            }
            drawPowerGlyph (g, pr.reduced (2.0f), on ? colour::bg0 : (pr.contains (mouse) ? colour::textPrimary : colour::textSecondary));
        }
        g.setColour (colour::textSecondary);
        g.setFont (font (type::small));
        g.drawText (tr ("Band {0}", { juce::String (selected + 1) }), juce::Rectangle<float> (panel.getX() + 36.0f, panel.getY() + 6.0f, 90.0f, 20.0f), juce::Justification::centredLeft);

        for (const auto& c : all)
        {
            switch (c.type)
            {
                case Ctl::shapeMenu:
                    drawButton (c.r, tr (shapeNames[(int) bandShape (selected)]) + juce::String::fromUTF8 (" \xe2\x96\xbe"), true, colourB);
                    break;
                case Ctl::slopeMenu:
                    drawButton (c.r, juce::String (slopeDb (param01 (c.param))) + " dB/oct" + juce::String::fromUTF8 (" \xe2\x96\xbe"), false, colourB);
                    break;
                case Ctl::remove: drawButton (c.r, tr ("Remove"), false, colour::accent); break;
                case Ctl::solo: drawButton (c.r, tr ("Solo"), solo == selected, colour::warning); break;
                case Ctl::placementMenu:
                    drawButton (c.r, placementLabel (bandPlacement (selected)) + juce::String::fromUTF8 (" \xe2\x96\xbe"), bandPlacement (selected) != Placement::stereo, colourB);
                    break;
                case Ctl::dynamic: drawButton (c.r, tr ("Dynamic"), isDynamic (fx.params, selected), colourB); break;
                case Ctl::spectral: drawButton (c.r, tr ("Spectral"), isSpectral (fx.params, selected), colourB); break;
                case Ctl::spectralTilt: drawButton (c.r, tr ("Tilt"), param01 (c.param) >= 0.5f, colourB); break;
                case Ctl::prevBand: drawArrow (c.r, true); break;
                case Ctl::nextBand: drawArrow (c.r, false); break;
                case Ctl::close:
                {
                    g.setColour (c.r.contains (mouse) ? colour::textPrimary : colour::textSecondary);
                    const auto r = c.r.reduced (5.0f);
                    g.drawLine (r.getX(), r.getY(), r.getRight(), r.getBottom(), 1.5f);
                    g.drawLine (r.getRight(), r.getY(), r.getX(), r.getBottom(), 1.5f);
                    // the band number between the arrows
                    g.setColour (colour::textPrimary);
                    g.setFont (font (type::small, true));
                    g.drawText (juce::String (selected + 1), c.r.translated (-44.0f, 0.0f).withWidth (22.0f), juce::Justification::centred);
                    break;
                }
                case Ctl::freq: case Ctl::gain: case Ctl::q: case Ctl::range: case Ctl::threshold: case Ctl::attack: case Ctl::release:
                {
                    static const std::map<Ctl, const char*> captions { { Ctl::freq, TR_NOOP ("FREQ") }, { Ctl::gain, TR_NOOP ("GAIN") }, { Ctl::q, "Q" },
                                                                       { Ctl::range, TR_NOOP ("RANGE") }, { Ctl::threshold, TR_NOOP ("THRESHOLD") },
                                                                       { Ctl::attack, TR_NOOP ("ATTACK") }, { Ctl::release, TR_NOOP ("RELEASE") } };
                    const auto bipolar = c.type == Ctl::gain || c.type == Ctl::range;
                    const auto active = dragging == Drag::control && dragParam == c.param;
                    const auto global = c.type == Ctl::attack || c.type == Ctl::release;
                    const auto knob = knobCircle (c);
                    skin::KnobLook look;
                    look.value = param01 (c.param);
                    look.bipolar = bipolar;
                    look.accent = global ? colour::accent : colourB;
                    look.hover = c.r.contains (mouse) ? 1.0f : 0.0f;
                    look.active = active;
                    look.enabled = fx.enabled;
                    skin::knob (g, knob, look);
                    if (c.type == Ctl::gain && isDynamic (fx.params, selected))
                    {
                        // the dynamic range as an outer arc from the gain, and the live position as a dot
                        const auto scale = (float) (juce::MathConstants<float>::pi * 1.5f);
                        const auto start = -juce::MathConstants<float>::pi * 0.75f;
                        const auto g0 = param01 (param (selected, 2));
                        const auto g1 = juce::jlimit (0.0f, 1.0f, gainValue (gainDb (g0) + rangeDb_ (selected)));
                        const auto rr = knob.getWidth() * 0.5f + 4.0f;
                        juce::Path arc;
                        arc.addCentredArc (knob.getCentreX(), knob.getCentreY(), rr, rr, 0.0f, start + scale * juce::jmin (g0, g1), start + scale * juce::jmax (g0, g1), true);
                        g.setColour (colourB.withAlpha (0.8f));
                        g.strokePath (arc, juce::PathStrokeType (2.0f));
                        const auto live = juce::jlimit (0.0f, 1.0f, gainValue (gainDb (g0) + liveDynamic (selected)));
                        const auto ang = start + scale * live;
                        g.setColour (colour::textPrimary);
                        g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ knob.getCentreX() + rr * std::sin (ang), knob.getCentreY() - rr * std::cos (ang) }));
                    }
                    g.setColour (active ? colourB.brighter (0.3f) : skin::captionColour());
                    g.setFont (font (type::caption, true));
                    g.drawText (tr (captions.at (c.type)), c.r.withY (c.r.getBottom() - 34.0f).withHeight (14.0f), juce::Justification::centred);
                    g.setColour (skin::valueColour (c.r.contains (mouse) || active));
                    g.setFont (font (type::small));
                    g.drawText (valueText (c.param), c.r.withTop (c.r.getBottom() - 20.0f), juce::Justification::centred, true);
                    break;
                }
                default: break;
            }
        }
        if (hasGain (bandShape (selected)) && ! isDynamic (fx.params, selected))
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (type::caption));
            g.drawFittedText (tr ("Dynamic EQ: the gain follows the level of the band's region (threshold, range)"),
                              juce::Rectangle<float> (panel.getX() + 306.0f, panel.getY() + 40.0f, 200.0f, 60.0f).toNearestInt(), juce::Justification::centredLeft, 3);
        }
    }

private:
    enum class Drag { none, node, range, control };

    int tapKey() const { return track * maxEffectSlots + slot; }

    void claimTap()
    {
        if (auto* times = engine.getProcessTimes()) times->analyzer.key.store (tapKey());
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (current() == nullptr) selected = -1;
        repaint();
    }

    void timerCallback() override
    {
        if (! isShowing()) return; // a hidden editor costs nothing
        if (shownSlot.changed (engine.getAutomationDisplay(), track, slot)) // the bands and knobs follow their automation
            repaint();
        updateSpectrum();
        // redraw the graph only when the analyser or a dynamic band moved (an idle editor stops drawing)
        double signature = 0.0;
        for (const auto* v : { &specPre, &specPost })
            for (const auto x : *v) signature += x;
        for (int b = 0; b < bands; ++b) signature += liveDynamic (b) * (b + 1);
        if (signature != lastSignature || demo) repaint (graphArea().toNearestInt());
        lastSignature = signature;
    }
    double lastSignature = 0.0;

    void updateSpectrum()
    {
        const auto columns = juce::jmax (64, (int) plotArea().getWidth() / 3);
        if ((int) specPre.size() != columns)
        {
            specPre.assign ((std::size_t) columns, (float) specFloor());
            specPost.assign ((std::size_t) columns, (float) specFloor());
        }
        if (analyzer (param01 (analyzerParam)) == Analyzer::off || frozen) return;
        if (demo)
        {
            for (int i = 0; i < columns; ++i)
            {
                const auto f = hzForX (plotArea().getX() + plotArea().getWidth() * (float) i / (float) (columns - 1));
                // a mix's typical spectrum: pink-ish slope, a kick bump, a presence region, roll-off
                const auto db = -40.0 - 9.0 * std::log2 (juce::jmax (20.0, f) / 1000.0) * 0.5 + 9.0 * std::exp (-std::pow (std::log2 (f / 60.0), 2.0))
                                + 4.0 * std::sin (f * 0.0021) - (f > 14000.0 ? (f - 14000.0) * 0.004 : 0.0) + tiltDb (f);
                specPre[(std::size_t) i] = (float) db;
                const auto* fx = current();
                specPost[(std::size_t) i] = (float) (db + (fx != nullptr ? responseDb (fx->params, 48000.0, f) : 0.0));
            }
            return;
        }
        auto* times = engine.getProcessTimes();
        if (times == nullptr) return;
        auto& tap = times->analyzer;
        if (tap.key.load (std::memory_order_relaxed) != tapKey())
        {
            if (hasKeyboardFocus (true) || isMouseOver (true)) claimTap(); // the editor in use takes the analyser
            decay();
            return;
        }
        const auto written = tap.written.load (std::memory_order_acquire);
        if (written == lastWritten) { decay(); return; }
        lastWritten = written;
        const auto sr = displayRate();
        for (int which = 0; which < 2; ++which)
        {
            auto& ring = which == 0 ? tap.pre : tap.post;
            std::fill (fftData.begin(), fftData.end(), 0.0f);
            for (int i = 0; i < fftSize; ++i)
                fftData[(std::size_t) i] = ring[(std::size_t) ((written - (std::uint32_t) fftSize + (std::uint32_t) i) & (ProcessTimes::AnalyzerTap::size - 1))]
                                               .load (std::memory_order_relaxed) * window[(std::size_t) i];
            fft.performFrequencyOnlyForwardTransform (fftData.data(), true);
            if (which == 0 && capture != Capture::none) // round 152: EQ match — the input's long-term spectrum
            {
                auto& acc = capture == Capture::reference ? matchRef : matchCur;
                if (acc.size() != (std::size_t) (fftSize / 2)) acc.assign ((std::size_t) (fftSize / 2), 0.0);
                for (int k = 0; k < fftSize / 2; ++k) acc[(std::size_t) k] += (double) fftData[(std::size_t) k] * fftData[(std::size_t) k];
                ++(capture == Capture::reference ? refFrames : curFrames);
            }
            auto& out = which == 0 ? specPre : specPost;
            const auto norm = 4.0f / (float) fftSize; // Hann (coherent gain ½) and one side
            for (int i = 0; i < columns; ++i)
            {
                const auto f0 = hzForX (plotArea().getX() + plotArea().getWidth() * ((float) i - 0.5f) / (float) (columns - 1));
                const auto f1 = hzForX (plotArea().getX() + plotArea().getWidth() * ((float) i + 0.5f) / (float) (columns - 1));
                auto k0 = (int) std::floor (f0 * fftSize / sr), k1 = (int) std::ceil (f1 * fftSize / sr);
                k0 = juce::jlimit (1, fftSize / 2 - 1, k0);
                k1 = juce::jlimit (k0, fftSize / 2 - 1, k1);
                float m = 0.0f;
                for (int k = k0; k <= k1; ++k) m = juce::jmax (m, fftData[(std::size_t) k]);
                const auto f = hzForX (plotArea().getX() + plotArea().getWidth() * (float) i / (float) (columns - 1));
                const auto db = (float) (20.0 * std::log10 (m * norm + 1e-9) + tiltDb (f));
                auto& v = out[(std::size_t) i];
                v = db > v ? db : juce::jmax (db, v - fallPerFrame()); // fast up, falling at the analyser speed
            }
        }
    }

    double tiltDb (double f) const { return analyzerTiltDb (param01 (specTiltParam)) * std::log2 (juce::jmax (10.0, f) / 1000.0); }
    // dB per 30 Hz frame the analyser falls: very slow 0.3 … very fast 6 (medium 1.5 = ~45 dB/s, the old speed)
    float fallPerFrame() const
    {
        static constexpr float fall[] { 0.3f, 0.75f, 1.5f, 3.0f, 6.0f };
        return fall[analyzerSpeed (param01 (specSpeedParam))];
    }

    void decay()
    {
        if (frozen) return;
        for (auto* v : { &specPre, &specPost })
            for (auto& x : *v) x = juce::jmax ((float) specFloor(), x - fallPerFrame());
    }

    AudioEngine& engine;
    int track, slot;
    mutable ShownEffectSlot shownSlot;
    bool demo = false;
    int selected = -1, hover = -1, tipBand = -1, dragBand = -1, dragParam = -1;
    bool piano = false;
    bool frozen = false; // analyser freeze (editor only)
    Capture capture = Capture::none; // round 152: EQ match
    std::vector<double> matchRef, matchCur;
    int refFrames = 0, curFrames = 0, matchBands = 8, matchSmoothing = 6;
    pluginheader::PresetBar bar { engine, track, slot };
    Drag dragging = Drag::none;
    juce::Point<float> dragOffset, dragStart, mouse { -1.0f, -1.0f };
    juce::Rectangle<float> dragRect;
    ui::ValueDrag valueDrag;
    juce::dsp::FFT fft;
    std::vector<float> fftData, window, specPre, specPost;
    std::uint32_t lastWritten = 0;
};

class FacetEqWindow final : public juce::DocumentWindow
{
public:
    FacetEqWindow (AudioEngine& engine, int track, int slot, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg1, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          gone (engine, track, slot, [this] { closeButtonPressed(); })
    {
        setUsingNativeTitleBar (true);
        setContentOwned (new FacetEqEditor (engine, track, slot, false), true);
        setResizable (true, false);
        if (auto* c = getConstrainer())
        {
            c->setMinimumSize (860, 440);
            c->setMaximumSize (2400, 1400);
        }
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
        if (auto* c = getContentComponent()) skin::playOpenAnimation (*c, skin::styleFor (EffectKind::facetEq)); // round 126: fade / scale in
    }

    void closeButtonPressed() override;

private:
    ui::SlotGoneWatcher gone;
};

std::map<std::pair<int, int>, std::unique_ptr<FacetEqWindow>>& windows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<FacetEqWindow>> open;
    return open;
}

void FacetEqWindow::closeButtonPressed()
{
    for (auto& [key, window] : windows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { windows().erase (k); });
            return;
        }
}
} // namespace

void drawFacetEqCurve (juce::Graphics& g, juce::Rectangle<float> area, const EffectParams& params, double sampleRate, bool enabled)
{
    using namespace theme;
    g.setColour (colour::bg0);
    g.fillRoundedRectangle (area, radius::md);
    const auto xFor = [&] (double f) { return area.getX() + area.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0)); };
    g.setColour (colour::gridBeat);
    for (double f : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) xFor (f), area.getY(), area.getBottom());
    g.setColour (colour::gridBar);
    g.drawHorizontalLine ((int) area.getCentreY(), area.getX(), area.getRight());
    const int n = juce::jmax (32, (int) area.getWidth() / 2);
    std::vector<double> freqs ((std::size_t) n + 1), sum ((std::size_t) n + 1, outputDb (params[(std::size_t) outputParam])), band ((std::size_t) n + 1);
    for (int i = 0; i <= n; ++i) freqs[(std::size_t) i] = 20.0 * std::pow (1000.0, (double) i / n);
    for (int b = 0; b < bands; ++b)
    {
        if (shape (params[(std::size_t) param (b, 0)]) == Shape::off) continue;
        bandCurveDb (params, b, sampleRate, freqs.data(), n + 1, band.data());
        for (int i = 0; i <= n; ++i) sum[(std::size_t) i] += band[(std::size_t) i];
    }
    juce::Path p;
    for (int i = 0; i <= n; ++i)
    {
        const auto x = area.getX() + area.getWidth() * (float) i / (float) n;
        const auto y = area.getCentreY() - area.getHeight() * 0.45f * (float) (juce::jlimit (-24.0, 24.0, sum[(std::size_t) i]) / 24.0);
        if (i == 0) p.startNewSubPath (x, y);
        else p.lineTo (x, y);
    }
    g.setColour (enabled ? colour::accent : colour::textDisabled);
    g.strokePath (p, juce::PathStrokeType (2.0f));
    for (int b = 0; b < bands; ++b)
    {
        const auto sh = shape (params[(std::size_t) param (b, 0)]);
        if (sh == Shape::off) continue;
        const auto f = juce::jlimit (20.0, 20000.0, hz (params[(std::size_t) param (b, 1)]));
        const auto db = hasGain (sh) ? gainDb (params[(std::size_t) param (b, 2)]) * gainScale (params[(std::size_t) scaleParam]) : 0.0;
        g.setColour (bandColour (b));
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ xFor (f), area.getCentreY() - area.getHeight() * 0.45f * (float) (juce::jlimit (-24.0, 24.0, db) / 24.0) }));
    }
}

std::unique_ptr<juce::Component> makeFacetEqEditor (AudioEngine& engine, int track, int slot, bool demo)
{
    return std::make_unique<FacetEqEditor> (engine, track, slot, demo);
}

namespace FacetEqWindows
{
void open (AudioEngine& engine, int track, int slot)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;
    auto& open = windows();
    const auto key = std::make_pair (track, slot);
    if (auto it = open.find (key); it != open.end())
    {
        if (auto* editor = dynamic_cast<FacetEqEditor*> (it->second->getContentComponent()); editor != nullptr && editor->current() != nullptr)
        {
            it->second->toFront (true);
            return;
        }
        open.erase (it);
    }
    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    const auto title = juce::String ("Facet EQ - ") + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " ("
                       + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    open[key] = std::make_unique<FacetEqWindow> (engine, track, slot, title);
}

void closeAll()
{
    windows().clear();
}
} // namespace FacetEqWindows
} // namespace lynx
