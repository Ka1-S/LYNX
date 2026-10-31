#include "ui/ParametricEqEditor.h"
#include "ui/PluginEditors.h"
#include "core/ParametricEqFl.h"


#include "core/Effects.h"
#include "core/RackRenderer.h"
#include "core/SpectrumHeat.h"
#include "engine/Text.h"
#include "platform/Paths.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
constexpr double minHz = 20.0, maxHz = 20000.0;

juce::Colour bandColour (int band)
{
    // one hue per band, at the same lightness so none dominates the red curve
    static const juce::Colour colours[] = { juce::Colour (0xfff2767b), juce::Colour (0xfff2a46b), juce::Colour (0xffe8cf6a),
                                            juce::Colour (0xff86d993), juce::Colour (0xff62cfd0), juce::Colour (0xff7aa9f5),
                                            juce::Colour (0xffb59af2) };
    return colours[band % 7];
}

float xForHz (juce::Rectangle<float> area, double hz)
{
    return area.getX() + area.getWidth() * (float) (std::log (hz / minHz) / std::log (maxHz / minHz));
}

double hzForX (juce::Rectangle<float> area, float x)
{
    return minHz * std::pow (maxHz / minHz, juce::jlimit (0.0, 1.0, (double) ((x - area.getX()) / area.getWidth())));
}

float yForDb (juce::Rectangle<float> area, double db, double rangeDb)
{
    if (! std::isfinite (db)) // (a NaN point would reach the path renderer)
        db = 0.0;
    return area.getCentreY() - area.getHeight() * 0.5f * (float) (juce::jlimit (-rangeDb, rangeDb, db) / rangeDb);
}

double dbForY (juce::Rectangle<float> area, float y, double rangeDb)
{
    return juce::jlimit (-18.0, 18.0, (double) ((area.getCentreY() - y) / (area.getHeight() * 0.5f)) * rangeDb);
}

// display names of peq::BandType (core names, translated here)
const char* const bandTypeNames[] = { TR_NOOP ("Off"), TR_NOOP ("Peak"), TR_NOOP ("Low shelf"), TR_NOOP ("High shelf"),
                                      TR_NOOP ("Low cut"), TR_NOOP ("High cut"), TR_NOOP ("Band-pass"), TR_NOOP ("Notch") };

juce::String bandTypeName (peq::BandType t)
{
    const auto i = (int) t;
    return i >= 0 && i < (int) std::size (bandTypeNames) ? tr (bandTypeNames[i]) : juce::String::fromUTF8 (peq::typeName (t));
}

bool hasGain (peq::BandType t)
{
    return t == peq::BandType::peak || t == peq::BandType::lowShelf || t == peq::BandType::highShelf;
}

} // namespace

void drawParametricEqCurve (juce::Graphics& g, juce::Rectangle<float> area, const EffectParams& params, double sampleRate,
                            bool enabled, int highlightBand, double rangeDb, bool labels, const peqview::Analyser* analyser)
{
    using namespace theme;
    skin::well (g, area, radius::md);
    if (analyser != nullptr) // round 166f: the heatmap behind the grid
        analyser->paintHeat (g, area);

    // grid: frequency decades and ±6/12/18 dB
    g.setColour (colour::gridBeat);
    for (double hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        g.drawVerticalLine ((int) xForHz (area, hz), area.getY(), area.getBottom());
    for (double db : { -18.0, -12.0, -6.0, 6.0, 12.0, 18.0 })
        if (std::abs (db) < rangeDb)
        g.drawHorizontalLine ((int) yForDb (area, db, rangeDb), area.getX(), area.getRight());
    g.setColour (colour::gridBar);
    g.drawHorizontalLine ((int) yForDb (area, 0.0, rangeDb), area.getX(), area.getRight());

    if (labels && area.getWidth() > 300.0f)
    {
        g.setColour (colour::textDisabled);
        g.setFont (font (10.0f));
        for (auto [hz, label] : { std::pair { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
            g.drawText (label, (int) xForHz (area, hz) + 3, (int) area.getBottom() - 14, 30, 12, juce::Justification::centredLeft);
    }

    if (analyser != nullptr) // round 166f: the spectrum behind the curve
        analyser->paintSpectrum (g, area);

    // curve
    juce::Path curve;
    const auto steps = juce::jmax (32, (int) area.getWidth() / 2);

    for (int i = 0; i <= steps; ++i)
    {
        const auto x = area.getX() + area.getWidth() * (float) i / (float) steps;
        const auto y = yForDb (area, peq::responseDb (params, sampleRate, hzForX (area, x)), rangeDb);

        if (i == 0)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);
    }

    auto fill = curve;
    fill.lineTo (area.getRight(), area.getCentreY());
    fill.lineTo (area.getX(), area.getCentreY());
    fill.closeSubPath();
    const auto curveColour = enabled ? (highlightBand >= 0 ? bandColour (highlightBand) : colour::accent) : colour::textDisabled;
    g.setGradientFill (juce::ColourGradient (curveColour.withAlpha (0.22f), area.getX(), area.getY(), curveColour.withAlpha (0.04f), area.getX(),
                                             area.getCentreY(), false));
    g.fillPath (fill);
    skin::glowStroke (g, curve, curveColour, 2.0f, enabled ? 0.8f : 0.0f);
}

std::unique_ptr<juce::Component> makeParametricEqEditor (AudioEngine& engine, int track, int slot)
{
    return makeNativeEffectEditor (engine, track, slot, 1.25f);
}

namespace ParametricEqWindows
{
void open (AudioEngine& engine, int track, int slot)
{
    NativeEffectWindows::open (engine, track, slot);
}

void closeAll()
{
    NativeEffectWindows::closeAll();
}
} // namespace ParametricEqWindows

namespace peqview
{
juce::Colour bandColour (int band) { return lynx::bandColour (band); }
float xForHz (juce::Rectangle<float> area, double hz) { return lynx::xForHz (area, hz); }
double hzForX (juce::Rectangle<float> area, float x) { return lynx::hzForX (area, x); }
float yForDb (juce::Rectangle<float> area, double db, double rangeDb) { return lynx::yForDb (area, db, rangeDb); }
double dbForY (juce::Rectangle<float> area, float y, double rangeDb) { return lynx::dbForY (area, y, rangeDb); }
juce::String bandTypeName (int type) { return lynx::bandTypeName ((peq::BandType) type); }

bool hasGain (const EffectParams& params, int band)
{
    return lynx::hasGain (peq::type (params[(std::size_t) peq::param (band, 0)]));
}

juce::Point<float> handlePosition (juce::Rectangle<float> area, const EffectParams& params, int band, double rangeDb)
{
    const auto hz = peq::hz (params[(std::size_t) peq::param (band, 1)]);
    const auto db = hasGain (params, band) ? peq::gainDb (params[(std::size_t) peq::param (band, 2)]) : 0.0;
    return { xForHz (area, hz), yForDb (area, db, rangeDb) };
}

int bandAt (juce::Rectangle<float> area, const EffectParams& params, juce::Point<float> p, double rangeDb)
{
    int best = -1;
    float bestDistance = 12.0f;
    for (int b = 0; b < peq::bands; ++b)
        if (peq::type (params[(std::size_t) peq::param (b, 0)]) != peq::BandType::off)
            if (const auto d = handlePosition (area, params, b, rangeDb).getDistanceFrom (p); d < bestDistance)
            {
                best = b;
                bestDistance = d;
            }
    return best;
}

void showBandMenu (juce::Component& target, const EffectParams& params, int band, std::function<void (std::vector<std::pair<int, float>>)> set)
{
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Band {0}", { juce::String (band + 1) }));
    const auto currentType = peq::type (params[(std::size_t) peq::param (band, 0)]);

    for (int t = 0; t < peq::bandTypes; ++t)
        menu.addItem (lynx::bandTypeName ((peq::BandType) t), true, (int) currentType == t, [set, band, t] {
            set ({ { peq::param (band, 0), peq::typeValue ((peq::BandType) t) } });
        });

    if (peq::flLaw (params) && ! (peqfl::lawOf (params) == peqfl::Law::peq1))
    {
        // FL's Parametric EQ 2 orders (Steep 8 … 2 … Gentle 8) apply to every band type
        menu.addSeparator();
        auto p = params;
        for (int order = 0; order < 7; ++order)
        {
            p[(std::size_t) (peq::slopeParam + band)] = (float) order / 6.0f;
            const auto current = (int) std::lround (params[(std::size_t) (peq::slopeParam + band)] * 6.0f) == order;
            menu.addItem (juce::String::fromUTF8 (peq::orderText (p, band).c_str()), true, current,
                          [set, band, order] { set ({ { peq::slopeParam + band, (float) order / 6.0f } }); });
        }
    }
    else if (! peq::flLaw (params) && (currentType == peq::BandType::lowCut || currentType == peq::BandType::highCut))
    {
        menu.addSeparator();
        for (int stages = 1; stages <= 4; ++stages)
            menu.addItem (juce::String (12 * stages) + " dB/oct", true, peq::slopeStages (params[(std::size_t) (peq::slopeParam + band)]) == stages,
                          [set, band, stages] { set ({ { peq::slopeParam + band, peq::slopeValue (stages) } }); });
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target).withMousePosition());
}

juce::String bandText (const EffectParams& p, int band)
{
    const auto t = peq::type (p[(std::size_t) peq::param (band, 0)]);
    juce::String text;
    text << tr ("Band {0}: {1}", { juce::String (band + 1), lynx::bandTypeName (t) }) << "   "
         << juce::String::fromUTF8 (formatEffectParam (EffectKind::parametricEq, peq::param (band, 1), p[(std::size_t) peq::param (band, 1)], 120.0).c_str());
    if (lynx::hasGain (t))
        text << "   " << juce::String::fromUTF8 (formatEffectParam (EffectKind::parametricEq, peq::param (band, 2), p[(std::size_t) peq::param (band, 2)], 120.0).c_str());
    // FL's modes (imported Parametric EQ 2): bandwidth and FL's order for every band type
    if (peq::flLaw (p))
        text << "   " << juce::String::fromUTF8 (peq::widthText (p, band).c_str()) << "   " << juce::String::fromUTF8 (peq::orderText (p, band).c_str());
    else if (t == peq::BandType::lowCut || t == peq::BandType::highCut)
        text << "   " << juce::String::fromUTF8 (peq::orderText (p, band).c_str());
    else
        text << "   " << juce::String::fromUTF8 (peq::widthText (p, band).c_str());
    return text;
}
} // namespace peqview
// --- round 166f (issue #16): FL's Parametric EQ 2 spectrum and heatmap --------------------------------------------

namespace peqview
{
namespace
{
juce::File displayFile() { return platform::settingsFile ("peq2-display.txt"); }
} // namespace

DisplaySettings& displaySettings()
{
    static DisplaySettings settings = [] {
        DisplaySettings d;
        for (const auto& line : juce::StringArray::fromLines (displayFile().loadFileAsString()))
        {
            const auto key = line.upToFirstOccurrenceOf ("=", false, false).trim(), value = line.fromFirstOccurrenceOf ("=", false, false).trim();
            if (key == "spectrum") d.spectrum = value.getIntValue() != 0;
            else if (key == "heatmap") d.heatmap = value.getIntValue() != 0;
            else if (key == "position") d.position = juce::jlimit (0, 2, value.getIntValue());
        }
        return d;
    }();
    return settings;
}

void saveDisplaySettings()
{
    const auto& d = displaySettings();
    displayFile().replaceWithText ("spectrum=" + juce::String ((int) d.spectrum) + "\nheatmap=" + juce::String ((int) d.heatmap) + "\nposition="
                                   + juce::String (d.position) + "\n");
}

void addDisplayItems (juce::PopupMenu& menu, std::function<void()> changed)
{
    auto& d = displaySettings();
    auto apply = [changed] (std::function<void (DisplaySettings&)> f) {
        f (displaySettings());
        saveDisplaySettings();
        if (changed) changed();
    };
    menu.addSectionHeader (tr ("Display"));
    menu.addItem (tr ("Spectrum"), true, d.spectrum, [apply] { apply ([] (DisplaySettings& s) { s.spectrum = ! s.spectrum; }); });
    menu.addItem (tr ("Heatmap (hot and cold levels)"), true, d.heatmap, [apply] { apply ([] (DisplaySettings& s) { s.heatmap = ! s.heatmap; }); });
    juce::PopupMenu position;
    static const char* const names[] { TR_NOOP ("Full"), TR_NOOP ("Top"), TR_NOOP ("Bottom") };
    for (int p = 0; p < 3; ++p)
        position.addItem (tr (names[p]), true, d.position == p, [apply, p] { apply ([p] (DisplaySettings& s) { s.position = p; s.heatmap = true; }); });
    menu.addSubMenu (tr ("Heatmap position"), position);
}

Analyser::Analyser()
{
    data.assign ((std::size_t) fftSize * 2, 0.0f);
    window.resize ((std::size_t) fftSize);
    for (int i = 0; i < fftSize; ++i)
        window[(std::size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (fftSize - 1));
}

bool Analyser::update (AudioEngine& engine, int track, int slot, bool claim, juce::Rectangle<float> graph)
{
    const auto& settings = displaySettings();
    const auto columns = juce::jlimit (32, 600, (int) graph.getWidth() / 2);
    if ((int) line.size() != columns)
    {
        line.assign ((std::size_t) columns, -200.0f);
        heat.assign ((std::size_t) columns, -200.0);
    }
    auto* times = engine.getProcessTimes();
    if (times == nullptr || (! settings.spectrum && ! settings.heatmap)) return false;
    auto& tap = times->analyzer;
    const auto key = track * maxEffectSlots + slot;
    if (tap.key.load (std::memory_order_relaxed) != key)
    {
        if (claim) tap.key.store (key);
        bool moved = false;
        for (auto& v : line) if (v > -200.0f) { v = juce::jmax (-200.0f, v - 1.5f); moved = true; }
        return moved;
    }
    const auto written = tap.written.load (std::memory_order_acquire);
    if (written == lastWritten)
    {
        bool moved = false;
        for (auto& v : line) if (v > -200.0f) { v = juce::jmax (-200.0f, v - 1.5f); moved = true; }
        return moved;
    }
    lastWritten = written;
    any = true;
    const auto srNow = tap.sampleRate.load (std::memory_order_relaxed);
    const auto sr = srNow > 8000.0 ? srNow : 48000.0;
    std::fill (data.begin(), data.end(), 0.0f);
    for (int i = 0; i < fftSize; ++i)
        data[(std::size_t) i] = tap.post[(std::size_t) ((written - (std::uint32_t) fftSize + (std::uint32_t) i) & (ProcessTimes::AnalyzerTap::size - 1))]
                                    .load (std::memory_order_relaxed) * window[(std::size_t) i];
    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    const auto norm = 4.0f / (float) fftSize; // Hann (coherent gain ½), one side: a full-scale sine = 0 dB
    for (int c = 0; c < columns; ++c)
    {
        const auto f0 = hzForX (graph, graph.getX() + graph.getWidth() * ((float) c - 0.5f) / (float) (columns - 1));
        const auto f1 = hzForX (graph, graph.getX() + graph.getWidth() * ((float) c + 0.5f) / (float) (columns - 1));
        auto k0 = (int) std::floor (f0 * fftSize / sr), k1 = (int) std::ceil (f1 * fftSize / sr);
        k0 = juce::jlimit (1, fftSize / 2 - 1, k0);
        k1 = juce::jlimit (k0, fftSize / 2 - 1, k1);
        float m = 0.0f;
        for (int k = k0; k <= k1; ++k) m = juce::jmax (m, data[(std::size_t) k]);
        const auto db = 20.0f * std::log10 (m * norm + 1e-10f);
        auto& v = line[(std::size_t) c];
        v = db > v ? db : juce::jmax (db, v - 1.5f); // fast up, falling ~45 dB/s
        spectrumheat::Average a { heat[(std::size_t) c] > -199.0 ? std::pow (10.0, heat[(std::size_t) c] / 10.0) : 0.0 };
        a.add (db + spectrumheat::tilt (std::sqrt (f0 * f1)), 30.0, 1.0); // tilted: the heatmap evens out music's slope
        heat[(std::size_t) c] = a.db();
    }
    return true;
}

void Analyser::paintHeat (juce::Graphics& g, juce::Rectangle<float> graph) const
{
    const auto& settings = displaySettings();
    if (! settings.heatmap || ! any || heat.empty()) return;
    const auto pos = (spectrumheat::Position) settings.position;
    const auto n = (int) heat.size();
    const auto w = graph.getWidth() / (float) n;
    double loudest = -200.0;
    for (const auto v : heat) loudest = std::max (loudest, v);
    g.saveState();
    g.reduceClipRegion (graph.toNearestInt());
    for (int c = 0; c < n; ++c)
    {
        const auto h = spectrumheat::relative01 (heat[(std::size_t) c], loudest);
        if (h <= 0.02) continue;
        const auto rgb = spectrumheat::colour (h);
        const auto colour = juce::Colour (rgb.r, rgb.g, rgb.b);
        const auto alpha = (float) (0.25 + 0.65 * h);
        const auto x = graph.getX() + (float) c * w - w * 0.5f;
        const auto strip = juce::Rectangle<float> (x, graph.getY(), w + 0.6f, graph.getHeight());
        if (pos == spectrumheat::Position::full)
        {
            g.setColour (colour.withAlpha (alpha));
            g.fillRect (strip);
        }
        else
        {
            const auto top = pos == spectrumheat::Position::top;
            const auto edge = top ? graph.getY() : graph.getBottom();
            const auto fade = graph.getY() + graph.getHeight() * (top ? 0.6f : 0.4f);
            g.setGradientFill (juce::ColourGradient (colour.withAlpha (alpha), x, edge, colour.withAlpha (0.0f), x, fade, false));
            g.fillRect (strip);
        }
    }
    g.restoreState();
}

void Analyser::paintSpectrum (juce::Graphics& g, juce::Rectangle<float> graph) const
{
    if (! displaySettings().spectrum || ! any || line.empty()) return;
    // FL's spectrum scale: the analyser's −96 … 0 dB over the graph's height, tilted 4.5 dB/oct around 1 kHz (a mix's
    // pink slope drawn level, as the analysers do)
    const auto n = (int) line.size();
    juce::Path p;
    for (int c = 0; c < n; ++c)
    {
        const auto x = graph.getX() + graph.getWidth() * (float) c / (float) (n - 1);
        const auto f = hzForX (graph, x);
        const auto db = (double) line[(std::size_t) c] + 4.5 * std::log2 (juce::jmax (20.0, f) / 1000.0);
        const auto y = graph.getBottom() - graph.getHeight() * (float) spectrumheat::heat01 (db + 12.0);
        if (c == 0) { p.startNewSubPath (x, graph.getBottom()); p.lineTo (x, y); }
        else p.lineTo (x, y);
    }
    auto edge = p;
    p.lineTo (graph.getRight(), graph.getBottom());
    p.closeSubPath();
    g.saveState();
    g.reduceClipRegion (graph.toNearestInt());
    g.setGradientFill (juce::ColourGradient (juce::Colours::white.withAlpha (0.16f), 0.0f, graph.getY(), juce::Colours::white.withAlpha (0.05f), 0.0f,
                                             graph.getBottom(), false));
    g.fillPath (p);
    g.setColour (juce::Colours::white.withAlpha (0.45f));
    g.strokePath (edge, juce::PathStrokeType (1.0f));
    g.restoreState();
}
} // namespace peqview
} // namespace lynx
