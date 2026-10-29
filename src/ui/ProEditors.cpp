#include "ui/SlotWatch.h"
#include "ui/ProEditors.h"

#include <cmath>
#include <map>

#include "core/ApexLimiter.h"
#include "core/DynamicsSuite.h"
#include "core/FacetEq.h"
#include "core/MixSuite.h"
#include "ui/DynamicsDisplays.h"
#include "ui/ParametricEqEditor.h"
#include "ui/ProEditorBase.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
using namespace pro;
using namespace theme;

// a synthetic programme for the --plugui snapshots: a beat (kick on the beat, snare on the off-beat) over a
// sustained part, 10 ms columns, deterministic
float demoInDb (int i)
{
    const auto t = (double) i * 0.01;
    const auto beat = std::fmod (t, 0.5), off = std::fmod (t + 0.25, 0.5);
    const auto noise = (float) ((std::sin ((double) i * 12.9898) * 43758.5453) - std::floor (std::sin ((double) i * 12.9898) * 43758.5453));
    const auto body = -20.0 + 3.0 * std::sin (t * 0.9) + 2.5 * (double) noise;
    const auto kick = 13.0 * std::exp (-beat / 0.05);
    const auto snare = 9.0 * std::exp (-off / 0.035);
    return (float) juce::jmin (-0.5, body + juce::jmax (kick, snare));
}

juce::Colour inColour() { return juce::Colour (0xff7d8597); }
juce::Colour outColour() { return colour::info; }

// --- the scrolling level display shared by the limiter, compressor, gate and de-esser ----------------------
// dB → y over a display area: a linear part from 0 to `knee` dB over the top 70 % and the rest down to `floor`
struct LevelScale
{
    float knee = -15.0f, floor = -48.0f, top = 0.0f;
    float y (float db, juce::Rectangle<float> a) const
    {
        const auto d = juce::jlimit (floor, top + 6.0f, db) - top;
        const auto k = knee - top;
        const auto f = d >= k ? 0.7f * d / k : 0.7f + 0.3f * (d - k) / (floor - knee);
        return a.getY() + a.getHeight() * f;
    }
    float db (float yy, juce::Rectangle<float> a) const
    {
        const auto f = (yy - a.getY()) / juce::jmax (1.0f, a.getHeight());
        const auto k = knee - top;
        return f <= 0.7f ? top + k * f / 0.7f : knee + (floor - knee) * (f - 0.7f) / 0.3f;
    }
};

void scaleLabels (juce::Graphics& g, const LevelScale& s, juce::Rectangle<float> a, float labelX, std::initializer_list<float> marks, bool lines = true)
{
    g.setFont (font (type::caption));
    for (const auto m : marks)
    {
        const auto y = s.y (m, a);
        if (lines)
        {
            g.setColour (colour::gridBeat);
            g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
        }
        g.setColour (colour::textDisabled);
        g.drawText (m == 0.0f ? juce::String ("0 dB") : juce::String ((int) m), juce::Rectangle<float> (labelX, y - 7.0f, 40.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }
}

// =============================================================================================================
// Apex Limiter — the reference limiter's workflow: the scrolling display is the window (input after gain in grey,
// output in blue, gain reduction hanging from the top in red, the biggest reductions tagged), the GAIN slider on
// its left edge, the output / gain-reduction meters on the right, the style / look-ahead / release / link panel at
// the bottom left (folds away), true-peak and the output ceiling in the bottom bar.
class ApexEditor final : public ProEditor
{
public:
    ApexEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::apexLimiter, d, 950, 600) { fillDemo(); }

private:
    enum { idPanel = 1, idGain, idCeiling, idTp, idAdvanced, idLoudness, idLoudReset };
    bool panelOpen = true, advancedOpen = false;
    static constexpr const char* styles = "Clean|Glue|Punch|Dynamic|Dense|Loud|Bus|Safe";

    // the display / meter scale chosen with the meter-scale menu (round 128): −16 / −32 / −48 dB, K-12 / K-14 / K-20
    int meterScale() const { return apex::meterScale (v (apex::pMeterScale)); }
    LevelScale scaleNow() const
    {
        switch (meterScale())
        {
            case 0: return { -6.0f, -16.0f };
            case 1: return { -12.0f, -32.0f };
            case 3: return { -24.0f, -60.0f };
            case 4: return { -26.0f, -62.0f };
            case 5: return { -32.0f, -68.0f };
            default: return { -15.0f, -48.0f };
        }
    }
    float kOffset() const { const auto m = meterScale(); return m == 3 ? 12.0f : m == 4 ? 14.0f : m == 5 ? 20.0f : 0.0f; }
    float outHold = -120.0f, grHold = 0.0f;
    int holdAge = 0;

    // round 135: the display modes (Fast = 10 ms a pixel, Slow = 30 ms, Slow down = recent fast and the past
    // compressed, Infinite = 0.5 s a pixel from a long history, Off = meters only) and the loudness panel
    int displayMode() const { return apex::displayMode (v (apex::pDisplayMode)); }
    bool loudnessShown() const { return v (apex::pLoudnessShown) >= 0.5f; }
    bool truePeakMeter() const { return v (apex::pPeakMeter) >= 0.5f; }
    static constexpr int longSpan = 50; // history columns (10 ms) per column of the infinite history
    std::vector<Column> longHistory = std::vector<Column> ((std::size_t) historyLength);
    int longPos = 0, longCount = 0, seenPos = 0;
    Column longAcc;
    static void merge (Column& into, const Column& c)
    {
        into.in = juce::jmax (into.in, c.in);
        into.out = juce::jmax (into.out, c.out);
        into.gr = juce::jmax (into.gr, c.gr);
    }
    // the columns drawn, newest first (one per pixel)
    std::vector<Column> view (int n) const
    {
        std::vector<Column> out ((std::size_t) juce::jmax (0, n));
        const auto mode = displayMode();
        if (mode == 0)
        {
            for (int k = 0; k < n; ++k) out[(std::size_t) k] = columnBack (k);
            return out;
        }
        if (mode == 3 && ! demo)
        {
            for (int k = 0; k < n; ++k)
            {
                auto c = longHistory[(std::size_t) ((longPos - 1 - k + historyLength * 4) % historyLength)];
                if (k == 0) merge (c, longAcc);
                out[(std::size_t) k] = c;
            }
            return out;
        }
        // Slow: 3 columns a pixel; Slow down: 1 column a pixel at the right edge, the span growing to the left
        // (demo snapshots show the infinite mode as Slow)
        double start = 0.0;
        for (int k = 0; k < n; ++k)
        {
            const double span = mode == 2 ? 1.0 + 3.0 * ((double) k / juce::jmax (1, n)) * ((double) k / juce::jmax (1, n)) : 3.0;
            const auto a = (int) start, b = juce::jmax (a + 1, (int) (start + span));
            Column c { -120.0f, -120.0f, 0.0f };
            for (int j = a; j < b && j < historyLength; ++j) merge (c, columnBack (j));
            out[(std::size_t) k] = c;
            start += span;
        }
        return out;
    }
    juce::Rectangle<float> loudnessArea() const { return bodyArea().withTrimmedLeft (bodyArea().getWidth() - 196.0f).reduced (6.0f, 8.0f); }

    juce::String title() const override { return "Apex Limiter"; }
    juce::String gestures() const override { return tr ("Drag GAIN on the left  -  drag the red ceiling line  -  wheel on the display: gain"); }
    bool usesHistory() const override { return true; }
    float grDb() const override { return liveMeter (0) * 30.0f; }

    double gainDb() const { return apex::gainDb (v (0)); }
    double ceilingDb() const { return apex::ceilingDb (v (1)); }

    juce::Rectangle<float> display() const { return bodyArea().withTrimmedRight (104.0f + (loudnessShown() ? 196.0f : 0.0f)); }
    juce::Rectangle<float> plot() const { return display().withTrimmedTop (14.0f).withTrimmedBottom (6.0f); }
    juce::Rectangle<float> gainTrack() const
    {
        const auto p = plot();
        return { p.getX() + 14.0f, p.getY() + 6.0f, 34.0f, p.getHeight() * 0.62f };
    }
    juce::Rectangle<float> gainBox() const
    {
        const auto tr_ = gainTrack();
        const auto y = tr_.getBottom() - tr_.getHeight() * v (0);
        return { tr_.getX() - 4.0f, y - 11.0f, 50.0f, 22.0f };
    }
    juce::Rectangle<float> panel() const
    {
        const auto p = plot();
        return { p.getX() + 8.0f, p.getBottom() - 104.0f, 560.0f, 96.0f };
    }

    Column demoColumn (int i) const override
    {
        // gain into the ceiling, instant attack and the release recovering ~0.25 dB per column
        float gr = 0.0f;
        for (int k = 0; k < 80 && i - k >= 0; ++k)
            gr = juce::jmax (gr, demoInDb (i - k) + (float) gainDb() - (float) ceilingDb() - 0.25f * (float) k);
        const auto in = demoInDb (i);
        return { in, in + (float) gainDb() - gr, gr };
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        out.push_back (custom (gainTrack().expanded (8.0f, 14.0f), idGain, 0));
        const auto d = plot();
        out.push_back (custom ({ d.getX() + 60.0f, scaleNow().y ((float) ceilingDb(), d) - 5.0f, d.getWidth() - 60.0f, 10.0f }, idCeiling, 1,
                               tr ("Output ceiling: drag up or down (the bottom bar's Out does the same)")));
        out.push_back (button ({ d.getRight() - 46.0f, d.getBottom() - 26.0f, 40.0f, 20.0f }, idTp, "TP", v (5) >= 0.5f,
                               tr ("True peak: the ceiling also holds between the samples (4x interpolation)")));
        out.push_back (button ({ d.getRight() - 232.0f, d.getY() + 18.0f, 84.0f, 20.0f }, idLoudness, tr ("Loudness"), loudnessShown(),
                               tr ("Loudness meter: momentary, short term and integrated LUFS, loudness range, peak")));
        {
            auto dm = menu ({ d.getRight() - 210.0f, d.getBottom() - 26.0f, 86.0f, 20.0f }, apex::pDisplayMode, {}, "Fast|Slow|Slow down|Infinite|Off");
            dm.hint = tr ("Display speed: Fast, Slow, Slow down (the past compressed), Infinite (the whole take), Off (meters only)");
            out.push_back (dm);
        }
        if (loudnessShown())
        {
            const auto la = loudnessArea();
            auto tgt = menu ({ la.getRight() - 84.0f, la.getY() + 4.0f, 80.0f, 20.0f }, apex::pLoudnessTarget, {}, "-23 LUFS|-18 LUFS|-16 LUFS|-14 LUFS|-11 LUFS|-9 LUFS");
            tgt.hint = tr ("Loudness target: the line on the loudness bars");
            out.push_back (tgt);
            out.push_back (tabs ({ la.getX() + 4.0f, la.getBottom() - 54.0f, la.getWidth() - 8.0f, 22.0f }, apex::pPeakMeter, "Sample peak|True peak"));
            out.push_back (button ({ la.getX() + 4.0f, la.getBottom() - 26.0f, la.getWidth() - 8.0f, 22.0f }, idLoudReset, tr ("Reset"), false,
                                   tr ("Reset the integrated loudness, the range and the peak hold")));
        }
        if (panelOpen)
        {
            const auto p = panel();
            out.push_back (button ({ p.getX() - 2.0f, p.getY() + 6.0f, 16.0f, 18.0f }, idPanel, juce::String::fromUTF8 ("\xc2\xab"), false, tr ("Hide the knobs")));
            out.push_back (menu ({ p.getX() + 16.0f, p.getY() + 40.0f, 96.0f, 26.0f }, apex::pStyle, {}, styles));
            out.push_back (knob ({ p.getX() + 122.0f, p.getY() + 22.0f, 68.0f, 72.0f }, apex::pLookahead, tr ("Lookahead")));
            out.push_back (knob ({ p.getX() + 192.0f, p.getY() + 22.0f, 68.0f, 72.0f }, apex::pAttack, tr ("Attack")));
            out.push_back (knob ({ p.getX() + 262.0f, p.getY() + 22.0f, 68.0f, 72.0f }, apex::pRelease, tr ("Release")));
            out.push_back (knob ({ p.getX() + 346.0f, p.getY() + 22.0f, 80.0f, 72.0f }, apex::pLink, tr ("Transients")));
            out.push_back (knob ({ p.getX() + 428.0f, p.getY() + 22.0f, 80.0f, 72.0f }, apex::pLinkRelease, tr ("Release")));
            out.push_back (button ({ p.getRight() - 30.0f, p.getY() + 4.0f, 26.0f, p.getHeight() - 8.0f }, idAdvanced,
                                   juce::String::fromUTF8 (advancedOpen ? "\xc2\xab" : "\xc2\xbb"), advancedOpen,
                                   tr ("Advanced: DC offset filter, unity gain, audition limiting, side-chain triggering")));
            if (advancedOpen)
            {
                const auto ad = advanced();
                out.push_back (toggle ({ ad.getX() + 10.0f, ad.getY() + 22.0f, ad.getWidth() - 20.0f, 17.0f }, apex::pDcFilter, tr ("DC offset filter")));
                out.push_back (toggle ({ ad.getX() + 10.0f, ad.getY() + 40.0f, ad.getWidth() - 20.0f, 17.0f }, apex::pUnity, tr ("Unity gain")));
                out.push_back (toggle ({ ad.getX() + 10.0f, ad.getY() + 58.0f, ad.getWidth() - 20.0f, 17.0f }, apex::pAudition, tr ("Audition limiting")));
                auto sc = toggle ({ ad.getX() + 10.0f, ad.getY() + 76.0f, ad.getWidth() - 20.0f, 17.0f }, apex::pSidechain, tr ("Side-chain triggering"));
                sc.hint = tr ("The limiter reacts to the slot's side-chain input (mixer: right-click the slot - Side chain)");
                out.push_back (sc);
            }
        }
        else
            out.push_back (button ({ 0.0f, plot().getBottom() - 120.0f, 22.0f, 110.0f }, idPanel, juce::String::fromUTF8 ("\xc2\xbb"), false, tr ("Show the knobs: style, look-ahead, attack, release, channel linking")));
        // the meter scale next to the TP button (bottom right of the display)
        {
            const auto d = plot();
            auto ms = menu ({ d.getRight() - 120.0f, d.getBottom() - 26.0f, 70.0f, 20.0f }, apex::pMeterScale, {}, "-16 dB|-32 dB|-48 dB|K-12|K-14|K-20");
            ms.hint = tr ("Meter scale of the display and the meters");
            out.push_back (ms);
        }
        const auto b = barArea();
        const auto cy = b.getCentreY();
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 160.0f, 22.0f }, apex::pTruePeak, tr ("True peak limiting"), "Off|On"));
        out.push_back (barMenu ({ 176.0f, cy - 11.0f, 130.0f, 22.0f }, apex::pDither, tr ("Dither"), "Off|16 bits|18 bits|20 bits|22 bits|24 bits"));
        out.push_back (barMenu ({ 312.0f, cy - 11.0f, 160.0f, 22.0f }, apex::pShaping, tr ("Noise shaping"), "None|Basic|Optimized|Weighted"));
        out.push_back (barMenu ({ 478.0f, cy - 11.0f, 150.0f, 22.0f }, apex::pOversampling, tr ("Oversampling"), "Off|2x|4x|8x"));
        out.push_back (barValue ({ b.getRight() - 150.0f, cy - 11.0f, 140.0f, 22.0f }, 1, tr ("Out")));
        out.push_back (barValue ({ b.getRight() - 300.0f, cy - 11.0f, 140.0f, 22.0f }, 0, tr ("Gain")));
    }

    juce::Rectangle<float> advanced() const
    {
        const auto pn = panel();
        return { pn.getRight() + 6.0f, pn.getY(), 170.0f, pn.getHeight() };
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idPanel) panelOpen = ! panelOpen;
        if (c.id == idAdvanced) advancedOpen = ! advancedOpen;
        if (c.id == idTp) set ({ { 5, v (5) >= 0.5f ? 0.0f : 1.0f } });
        if (c.id == idLoudness) set ({ { apex::pLoudnessShown, loudnessShown() ? 0.0f : 1.0f } });
        if (c.id == idLoudReset) set ({ { apex::pLoudnessReset, v (apex::pLoudnessReset) >= 0.5f ? 0.0f : 1.0f } }, false);
        repaint();
    }

    void customDown (const Ctl& c, const juce::MouseEvent& e) override
    {
        if (ui::isResetClick (e))
        {
            set ({ { c.param, (float) specFor (c.param).defaultValue } });
            return;
        }
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, specFor (c.param), v (c.param), [this, p = c.param] (double x) { set ({ { p, (float) x } }); });
            return;
        }
        customDrag (c, e);
    }

    void customDrag (const Ctl& c, const juce::MouseEvent& e) override
    {
        if (c.id == idGain)
        {
            const auto tr_ = gainTrack();
            auto x = (tr_.getBottom() - e.position.y) / tr_.getHeight();
            if (e.mods.isCtrlDown()) x = v (0) + (x - v (0)) * 0.25f;
            set ({ { 0, juce::jlimit (0.0f, 1.0f, x) } });
            ui::showValueTip (*this, gainBox().toNearestInt(), tr ("Gain") + " " + text (0));
        }
        else if (c.id == idCeiling)
        {
            const auto db = juce::jlimit (-30.0f, 0.0f, scaleNow().db (e.position.y, plot()));
            set ({ { 1, (float) (db / 30.0 + 1.0) } });
            ui::showValueTip (*this, juce::Rectangle<int> (60, 16).withCentre (e.position.toInt()), tr ("Out") + " " + text (1));
        }
    }

    bool bodyWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! plot().contains (e.position)) return false;
        const auto spec = specFor (0);
        if (const auto x = ui::wheelValue (v (0), e, w, spec))
        {
            set ({ { 0, (float) *x } });
            ui::flashValueTip (*this, gainBox().toNearestInt(), tr ("Gain") + " " + spec.text (*x));
        }
        return true;
    }

    ui::Hint bodyHint (juce::Point<float> p) override
    {
        ui::Hint h;
        if (plot().contains (p))
            h.text = tr ("Grey: the input after the gain; blue: the output; red from the top: the gain reduction. Wheel: gain");
        return h;
    }

    void tick() override
    {
        const auto& c = columnBack (0);
        if (++holdAge > 60) { outHold = -120.0f; grHold = 0.0f; holdAge = 0; }
        outHold = juce::jmax (outHold, truePeakMeter() && ! demo ? dbOf (liveMeter (3)) : c.out);
        grHold = juce::jmax (grHold, c.gr);
        // the infinite history: one column per 0.5 s (the loudest of its 50 columns)
        for (int guard = 0; seenPos != historyPos && guard < historyLength; ++guard)
        {
            merge (longAcc, history[(std::size_t) seenPos]);
            seenPos = (seenPos + 1) % historyLength;
            if (++longCount >= longSpan)
            {
                longHistory[(std::size_t) longPos] = longAcc;
                longPos = (longPos + 1) % historyLength;
                longAcc = { -120.0f, -120.0f, 0.0f };
                longCount = 0;
            }
        }
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        const auto d = display();
        const auto p = plot();
        displayBackground (g, bodyArea());
        // the history, newest at the right edge
        const auto n = displayMode() == 4 ? 0 : (int) p.getWidth();
        const auto cols = view (n);
        const auto gain = (float) gainDb();
        juce::Path inPath, outPath, grPath, outLine;
        inPath.startNewSubPath (p.getRight(), p.getBottom());
        outPath.startNewSubPath (p.getRight(), p.getBottom());
        grPath.startNewSubPath (p.getRight(), p.getY());
        for (int k = 0; k < n; ++k)
        {
            const auto& c = cols[(std::size_t) k];
            const auto x = p.getRight() - (float) k;
            inPath.lineTo (x, scaleNow().y (c.in + gain, p));
            outPath.lineTo (x, scaleNow().y (c.out, p));
            grPath.lineTo (x, scaleNow().y (-c.gr, p));
            if (k == 0) outLine.startNewSubPath (x, scaleNow().y (c.out, p));
            else outLine.lineTo (x, scaleNow().y (c.out, p));
        }
        inPath.lineTo (p.getX(), p.getBottom());
        outPath.lineTo (p.getX(), p.getBottom());
        grPath.lineTo (p.getX(), p.getY());
        for (auto* path : { &inPath, &outPath, &grPath }) path->closeSubPath();
        fillLevel (g, inPath, inColour(), p, 0.55f, 0.22f);
        fillLevel (g, outPath, outColour(), p, 0.7f, 0.18f);
        g.setColour (colour::textPrimary.withAlpha (0.6f));
        g.strokePath (outLine, juce::PathStrokeType (1.0f)); // (no glow on the 100-column-a-second history: it is redrawn at 30 Hz)
        fillLevel (g, grPath, colour::accent, p, fx.enabled ? 0.8f : 0.25f, fx.enabled ? 0.35f : 0.1f);
        drawScale (g, p, d.getRight() + 6.0f);
        if (n == 0)
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (type::small));
            g.drawText (tr ("Display off"), p.withTrimmedBottom (p.getHeight() * 0.5f), juce::Justification::centred, false);
        }

        // the biggest reductions, tagged (one tag per 1.5 s at most)
        g.setFont (font (type::caption, true));
        for (int k0 = 20; k0 + 150 < n; k0 += 150)
        {
            int best = -1;
            float bestGr = 1.0f;
            for (int k = k0; k < k0 + 150; ++k)
                if (cols[(std::size_t) k].gr > bestGr) { bestGr = cols[(std::size_t) k].gr; best = k; }
            if (best < 0) continue;
            const auto x = p.getRight() - (float) best;
            const auto r = juce::Rectangle<float> (46.0f, 16.0f).withCentre ({ x, scaleNow().y (-bestGr, p) + 12.0f });
            g.setColour (colour::bg3.withAlpha (0.92f));
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (colour::accent);
            g.drawRoundedRectangle (r, 3.0f, 1.0f);
            g.setColour (colour::textPrimary);
            g.drawText (juce::String (-bestGr, 1) + " dB", r, juce::Justification::centred, false);
        }

        // the ceiling line (red) with its value at the right
        const auto cy = scaleNow().y ((float) ceilingDb(), p);
        g.setColour (colour::accent);
        g.drawHorizontalLine ((int) cy, p.getX(), p.getRight());
        g.setFont (font (type::caption, true));
        g.drawText (tr ("Out") + " " + text (1), juce::Rectangle<float> (p.getRight() - 130.0f, cy + 2.0f, 124.0f, 14.0f), juce::Justification::centredRight, false);

        // the GAIN slider on the left edge
        const auto tr_ = gainTrack();
        g.setColour (colour::bg2.withAlpha (0.75f));
        g.fillRoundedRectangle (tr_.expanded (6.0f, 8.0f).withTrimmedBottom (-18.0f), 6.0f);
        g.setColour (colour::accent.withAlpha (0.18f));
        g.fillRect (tr_.withTop (gainBox().getCentreY()).reduced (14.0f, 0.0f));
        g.setColour (colour::knobTrack);
        g.fillRect (tr_.withSizeKeepingCentre (3.0f, tr_.getHeight()));
        const auto box = gainBox();
        const auto hot = box.expanded (6.0f).contains (mouse) || (dragCtl == -2 && pressed.id == idGain);
        g.setColour (hot ? colour::bg6 : colour::bg5);
        g.fillRoundedRectangle (box, 4.0f);
        g.setColour (hot ? colour::accent : colour::borderStrong);
        g.drawRoundedRectangle (box, 4.0f, 1.0f);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::small, true));
        g.drawText ((gainDb() >= 0.05 ? "+" : "") + juce::String (gainDb(), 1), box, juce::Justification::centred, false);
        caption (g, juce::Rectangle<float> (tr_.getX() - 10.0f, tr_.getBottom() + 4.0f, 54.0f, 14.0f), tr ("Gain"));

        // the meters: output and gain reduction
        const auto m = juce::Rectangle<float> (d.getRight() + 46.0f, p.getY(), 52.0f, p.getHeight());
        const auto outR = m.withWidth (22.0f);
        const auto grR = m.withTrimmedLeft (28.0f);
        const auto newest = columnBack (0);
        const auto outY = scaleNow().y (truePeakMeter() && ! demo ? dbOf (liveMeter (3)) : newest.out, p);
        skin::meterSolid (g, outR, (outR.getBottom() - outY) / outR.getHeight(), outColour(), false);
        skin::meterSolid (g, grR, (scaleNow().y (-newest.gr, p) - grR.getY()) / grR.getHeight(), colour::accent, true);
        g.setColour (colour::bg0.withAlpha (0.5f));
        for (auto y = m.getY() + 3.0f; y < m.getBottom(); y += 3.0f) g.drawHorizontalLine ((int) y, m.getX(), m.getRight());
        g.setFont (font (type::caption, true));
        g.setColour (colour::ledOn);
        g.drawText (outHold > -100.0f ? juce::String (outHold, 1) : "-inf", outR.withY (d.getY()).withHeight (14.0f).expanded (6.0f, 0.0f), juce::Justification::centred, false);
        g.setColour (colour::accent);
        g.drawText (juce::String (-grHold, 1), grR.withY (d.getY()).withHeight (14.0f).expanded (6.0f, 0.0f), juce::Justification::centred, false);

        // the knob panel
        if (panelOpen)
        {
            const auto pn = panel();
            drawPanel (g, pn);
            caption (g, { pn.getX() + 16.0f, pn.getY() + 8.0f, 96.0f, 14.0f }, tr ("Style"));
            caption (g, { pn.getX() + 120.0f, pn.getY() + 8.0f, 212.0f, 14.0f }, tr ("Envelope"));
            caption (g, { pn.getX() + 340.0f, pn.getY() + 8.0f, 176.0f, 14.0f }, tr ("Channel linking"));
            g.setColour (colour::border);
            g.drawVerticalLine ((int) pn.getX() + 116, pn.getY() + 10.0f, pn.getBottom() - 10.0f);
            g.drawVerticalLine ((int) pn.getX() + 336, pn.getY() + 10.0f, pn.getBottom() - 10.0f);
            if (advancedOpen)
            {
                const auto ad = advanced();
                drawPanel (g, ad);
                caption (g, { ad.getX(), ad.getY() + 6.0f, ad.getWidth(), 14.0f }, tr ("Advanced"));
            }
        }
        // latency (constant: the maximum look-ahead, + the oversampler)
        {
            // on a dark plate at the top left of the display, right of the GAIN slider
            const auto lr = juce::Rectangle<float> (p.getX() + 64.0f, p.getY() + 2.0f, 132.0f, 15.0f);
            g.setColour (colour::bg0.withAlpha (0.7f));
            g.fillRoundedRectangle (lr, 3.0f);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            g.drawText (tr ("latency {0} samples", { juce::String (apex::latencyFor (48000.0, apex::oversamplingFactor (v (apex::pOversampling)))) }),
                        lr.reduced (5.0f, 0.0f), juce::Justification::centredLeft, true);
        }
        if (loudnessShown()) paintLoudness (g);
    }

    // the loudness panel (round 135): short-term and momentary bars with the target line, integrated / range / peak
    void paintLoudness (juce::Graphics& g)
    {
        const auto la = loudnessArea();
        drawPanel (g, la);
        caption (g, { la.getX() + 6.0f, la.getY() + 7.0f, 90.0f, 14.0f }, tr ("Loudness"), juce::Justification::centredLeft);
        auto lufs = [this] (int k, float demoValue) { return demo ? demoValue : liveMeter (k); };
        const auto momentary = lufs (1, -11.8f), shortTerm = lufs (4, -12.6f), integrated = lufs (5, -13.1f), range = lufs (6, 5.4f), peak = lufs (7, -0.3f);
        const auto target = (float) apex::loudnessTarget (v (apex::pLoudnessTarget));
        // the bars: −48 … 0 LUFS
        const auto bars = juce::Rectangle<float> (la.getX() + 10.0f, la.getY() + 34.0f, 64.0f, la.getHeight() - 34.0f - 64.0f);
        auto bar = [&] (juce::Rectangle<float> r, float l, const juce::String& name) {
            const auto f = juce::jlimit (0.0f, 1.0f, (l + 48.0f) / 48.0f);
            skin::meterSolid (g, r.withTrimmedBottom (14.0f), f, l > target + 1.0f ? colour::accent : outColour(), false);
            caption (g, r.withTop (r.getBottom() - 13.0f), name);
        };
        bar (bars.withWidth (26.0f), shortTerm, "S");
        bar (bars.withTrimmedLeft (34.0f).withWidth (26.0f), momentary, "M");
        const auto meterH = bars.getHeight() - 14.0f;
        const auto ty = bars.getY() + meterH * (1.0f - juce::jlimit (0.0f, 1.0f, (target + 48.0f) / 48.0f));
        g.setColour (colour::textPrimary.withAlpha (0.8f));
        g.drawHorizontalLine ((int) ty, bars.getX() - 3.0f, bars.getRight() + 3.0f);
        g.setFont (font (type::caption));
        g.setColour (colour::textDisabled);
        for (int m = 0; m >= -48; m -= 12)
            g.drawText (juce::String (m), juce::Rectangle<float> (bars.getRight() + 2.0f, bars.getY() + meterH * (float) -m / 48.0f - 6.0f, 22.0f, 12.0f), juce::Justification::centredLeft, false);
        // the read-outs
        auto readout = [&] (float y, const juce::String& name, float value, const juce::String& unit, bool big) {
            const auto x = bars.getRight() + 26.0f, w = la.getRight() - x - 6.0f;
            g.setColour (colour::textDisabled);
            g.setFont (font (type::caption));
            g.drawText (name, juce::Rectangle<float> (x, y, w, 12.0f), juce::Justification::centredLeft, false);
            g.setColour (colour::textPrimary);
            g.setFont (font (big ? type::title : type::small, true));
            g.drawText (value <= -100.0f ? juce::String ("-inf") : juce::String (value, 1) + " " + unit, juce::Rectangle<float> (x, y + 12.0f, w, big ? 22.0f : 16.0f),
                        juce::Justification::centredLeft, false);
        };
        auto y = bars.getY();
        readout (y, tr ("Integrated"), integrated, "LUFS", true); y += 40.0f;
        readout (y, tr ("Short term"), shortTerm, "LUFS", false); y += 32.0f;
        readout (y, tr ("Momentary"), momentary, "LUFS", false); y += 32.0f;
        readout (y, tr ("Range"), range, "LU", false); y += 32.0f;
        readout (y, truePeakMeter() ? tr ("True peak") : tr ("Peak"), peak, truePeakMeter() ? "dBTP" : "dB", false);
    }

    void paintCustom (juce::Graphics&, const Ctl&) override {}

    // the dB marks of the chosen meter scale (K scales are labelled relative to their 0 VU)
    void drawScale (juce::Graphics& g, juce::Rectangle<float> p, float labelX) const
    {
        const auto sc = scaleNow();
        const auto k = kOffset();
        std::vector<float> marks;
        switch (meterScale())
        {
            case 0: marks = { 0.0f, -2.0f, -4.0f, -6.0f, -9.0f, -12.0f, -16.0f }; break;
            case 1: marks = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -18.0f, -24.0f, -32.0f }; break;
            case 2: marks = { 0.0f, -3.0f, -6.0f, -9.0f, -12.0f, -15.0f, -24.0f, -36.0f }; break;
            default: for (float m = 8.0f; m >= -36.0f; m -= (m > -12.0f ? 4.0f : 12.0f)) marks.push_back (m - k); break;
        }
        g.setFont (font (type::caption));
        for (const auto m : marks)
        {
            const auto y = sc.y (m, p);
            g.setColour (colour::gridBeat);
            g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
            g.setColour (colour::textDisabled);
            const auto label = k > 0.0f ? (m + k > 0.0f ? "+" : "") + juce::String ((int) std::lround (m + k)) : (m == 0.0f ? juce::String ("0 dB") : juce::String ((int) m));
            g.drawText (label, juce::Rectangle<float> (labelX, y - 7.0f, 40.0f, 14.0f), juce::Justification::centredLeft, false);
        }
    }
};

// =============================================================================================================
// Hush De-esser — the reference de-esser's workflow: the scrolling waveform on top with the reduction marked in
// red and the threshold line, the big THRESHOLD and RANGE knobs with the listen button, the frequency slider with
// the detection region under them, the mode buttons in the middle, release / output on the right, the input and
// reduction meters at the right edge.
class HushEditor final : public ProEditor
{
public:
    HushEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::hushDeEsser, d, 720, 500)
    {
        fillDemo();
        if (! displayShown()) setSize (720, 500 - (int) waveH);
    }

private:
    enum { idThreshold = 1, idFreq, idMeterThreshold, idDisplay, idExternal, idOutMeter, idGrMeter, idLink };
    float grHoldDb = 0.0f;     // round 144: the attenuation meter's infinite peak hold (DeEsser's Attn meter), click to reset
    float outPeakDb = -100.0f; // round 135w: the output meter's highest peak since the last click (DeEsser's read-out)
    static constexpr float waveH = 194.0f; // the waveform display (DISPLAY hides it and the window shrinks)
    bool displayShown() const { return v (P ("Display")) >= 0.5f; }
    // which handle of the frequency slider is being dragged: 0 = high-pass (Frequency), 1 = low-pass
    int dragHandle = 0;
    static float freqPos (double hz) { return (float) (std::log (hz / 2000.0) / std::log (10.0)); } // 2 .. 20 kHz
    // round 165x: the extended law's high-pass reaches 20 kHz (the old law and the classic voicing: 16 kHz)
    bool ext() const { return v (P ("Range law")) >= 0.5f && v (P ("Voicing")) < 0.5f; }
    double hpHz() const { return mixsuite::hush::hpHz (v (pFreq()), ext()); }
    float hpPos() const { return freqPos (hpHz()); }
    // round 165x: the 0 … 200 % link knob as one 0 … 1 position (Stereo link fills the first half, Link only the second)
    float linkPos() const { return 0.5f * (v (P ("Stereo link")) + (v (P ("Stereo link")) >= 0.999f ? v (P ("Link only")) : 0.0f)); }
    void setLinkPos (double u)
    {
        u = juce::jlimit (0.0, 1.0, u);
        set ({ { P ("Stereo link"), (float) juce::jmin (1.0, 2.0 * u) }, { P ("Link only"), (float) juce::jmax (0.0, 2.0 * u - 1.0) } });
    }
    juce::String linkText() const
    {
        const auto u = linkPos();
        if (u <= 0.5f)
            return juce::String (juce::roundToInt (200.0f * u)) + " %";
        return "100 %, " + juce::String (juce::roundToInt (200.0f * u - 100.0f)) + " % " + (v (P ("Link only side")) >= 0.5f ? tr ("side only") : tr ("mid only"));
    }
    ui::ValueSpec linkSpec() const
    {
        auto spec = ui::percentSpec (tr ("Stereo link"), 0.5);
        spec.toText = [] (double u) { return u <= 0.5 ? juce::String (juce::roundToInt (200.0 * u)) + " %" : juce::String (juce::roundToInt (200.0 * u)) + " % (100 % + " + juce::String (juce::roundToInt (200.0 * u - 100.0)) + " %)"; };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        return spec;
    }
    float lpPos() const { return v (P ("Low-pass freq.")) >= 0.999f ? 1.0f : freqPos (mixsuite::hush::lowPassHz (v (P ("Low-pass freq.")))); }

    juce::String title() const override { return "Hush De-esser"; }
    juce::String gestures() const override { return tr ("Drag the threshold line or the meter's arrow  -  drag the frequency slider") ; }
    bool usesHistory() const override { return true; }
    float grDb() const override { return liveMeter (0) * 30.0f; }

    int pFreq() const { return P ("Frequency"); }
    int pThr() const { return P ("Threshold"); }
    juce::Rectangle<float> wave() const
    {
        // round 135w: 142 px on the right for the IN / GR / OUT meters and their scale (was 112: IN / GR)
        return displayShown() ? bodyArea().withTrimmedRight (142.0f).withHeight (190.0f).reduced (0.0f, 4.0f)
                              : bodyArea().withTrimmedRight (142.0f).withHeight (0.0f).translated (0.0f, -4.0f);
    }
    // round 135w: the input meter moved left to make room for the output meter (In | GR | Out)
    juce::Rectangle<float> meters() const { return { (float) getWidth() - 100.0f, headerH + 18.0f, 26.0f, bodyArea().getHeight() - 26.0f }; }
    juce::Rectangle<float> outMeter() const { return meters().translated (56.0f, 0.0f).withWidth (16.0f).withTrimmedTop (12.0f).withTrimmedBottom (16.0f); }
    juce::Rectangle<float> freqTrack() const { return { 34.0f, wave().getBottom() + 182.0f, 250.0f, 24.0f }; }

    double thresholdDb() const { return mixsuite::hush::thresholdDb (v (pThr())); }

    // mirrored waveform: the height follows the amplitude (linear), like a waveform view; 0 dBFS at the edges
    float waveY (float db, bool upper) const
    {
        const auto a = wave();
        const auto f = juce::jlimit (0.0f, 1.0f, std::pow (10.0f, db / 20.0f));
        return upper ? a.getCentreY() - a.getHeight() * 0.48f * f : a.getCentreY() + a.getHeight() * 0.48f * f;
    }
    float meterY (float db) const
    {
        const auto m = meters();
        return m.getBottom() - m.getHeight() * juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
    }

    Column demoColumn (int i) const override
    {
        // sibilant bursts every 0.8 s over speech
        const auto t = (double) i * 0.01;
        const auto s = std::fmod (t, 0.8);
        const auto in = demoInDb (i) - 4.0f + (float) (s > 0.42 && s < 0.56 ? 7.0 : 0.0);
        const auto over = in - (float) thresholdDb();
        const auto gr = juce::jlimit (0.0f, (float) mixsuite::hush::rangeDb (v (P ("Range"))), over > 0.0f ? over * 0.8f : 0.0f);
        return { in, in - gr, gr };
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        const auto w = wave();
        const auto y0 = w.getBottom() + 10.0f;
        // threshold line on the waveform (both halves)
        if (displayShown())
        {
            out.push_back (custom ({ w.getX(), waveY ((float) thresholdDb(), true) - 5.0f, w.getWidth(), 10.0f }, idThreshold, pThr(), tr ("Threshold: drag up or down")));
            out.push_back (custom ({ w.getX(), waveY ((float) thresholdDb(), false) - 5.0f, w.getWidth(), 10.0f }, idThreshold, pThr(), tr ("Threshold: drag up or down")));
        }
        auto thr = knob ({ 44.0f, y0 + 4.0f, 110.0f, 142.0f }, pThr(), tr ("Threshold"), 2);
        thr.colour = colour::warning;
        out.push_back (thr);
        auto rng = knob ({ 170.0f, y0 + 4.0f, 110.0f, 142.0f }, P ("Range"), tr ("Range"), 2);
        rng.colour = colour::warning;
        out.push_back (rng);
        out.push_back (custom (freqTrack().expanded (0.0f, 4.0f), idFreq, pFreq(), tr ("Drag the left handle: where the band starts (high-pass); the right one: where it ends (low-pass, far right = off)")));
        auto aud = toggle ({ freqTrack().getCentreX() - 50.0f, freqTrack().getBottom() + 8.0f, 100.0f, 22.0f }, P ("Listen"), tr ("Audition"));
        aud.hint = tr ("Listen: hear only what the de-esser takes away (the detection band)");
        out.push_back (aud);
        // middle: display, mode, processing, side chain
        const auto mx = 304.0f;
        out.push_back (button ({ mx + 40.0f, y0 - 2.0f, 110.0f, 20.0f }, idDisplay, tr ("Display"), displayShown(), tr ("Show or hide the scrolling display")));
        auto md = tabs ({ mx, y0 + 40.0f, 190.0f, 28.0f }, P ("Allround"), "Single vocal|Allround", { 0.0f, 1.0f });
        md.hint = tr ("Single vocal: one voice; Allround: a mix or other material (the sibilance must also stand out from the whole signal)");
        out.push_back (md);
        out.push_back (tabs ({ mx, y0 + 96.0f, 190.0f, 28.0f }, P ("Wideband"), "Wide band|Split band", { 1.0f, 0.0f }));
        out.push_back (custom ({ mx + 90.0f, y0 + 148.0f, 100.0f, 26.0f }, idExternal, P ("External SC"), tr ("Side chain: detect this signal (In) or another track (Ext)")));
        // right: stereo link (with MID) and look-ahead (with its power button)
        if (ext())
        {
            // round 165x: the reference's link knob runs 0 … 200 %: up to 100 % the channels share their detection (100 % =
            // the mid), past it a share of the processing acts on the mid (or side) signal only — one knob over Stereo link
            // and Link only
            out.push_back (custom ({ 512.0f, y0 + 2.0f, 84.0f, 90.0f }, idLink, P ("Stereo link"),
                                   tr ("Stereo link: up to 100 % the channels share their detection; past 100 % a share of the processing acts on the mid (or side) signal only")));
            auto ms = tabs ({ 516.0f, y0 + 96.0f, 76.0f, 18.0f }, P ("Link only side"), "Mid|Side", { 0.0f, 1.0f });
            ms.hint = tr ("Which signal the processing past 100 % link acts on alone: the mid or the side");
            out.push_back (ms);
        }
        else
        {
            out.push_back (knob ({ 512.0f, y0 + 2.0f, 84.0f, 90.0f }, P ("Stereo link"), tr ("Stereo link")));
            auto mid = toggle ({ 522.0f, y0 + 96.0f, 64.0f, 18.0f }, P ("Link mid"), "MID");
            mid.hint = tr ("Mid: detect on the middle of the stereo image only");
            out.push_back (mid);
        }
        out.push_back (knob ({ 512.0f, y0 + 120.0f, 84.0f, 90.0f }, P ("Lookahead time"), tr ("Lookahead")));
        auto la = toggle ({ 522.0f, y0 + 214.0f, 64.0f, 18.0f }, P ("Lookahead"), tr ("On"));
        la.hint = tr ("Look-ahead on / off");
        out.push_back (la);
        // the meter's threshold arrow
        out.push_back (custom ({ meters().getX() - 12.0f, meterY ((float) thresholdDb()) - 7.0f, 12.0f, 14.0f }, idMeterThreshold, pThr(), tr ("Threshold: drag up or down")));
        // round 135w: the output meter (−30 … 0 dBFS, the reference's), its peak read-out and clip lamp: click to reset
        // round 144: the reduction meter's held peak: click to reset (the reference's Attn meter)
        out.push_back (custom (meters().translated (32.0f, 0.0f).withWidth (14.0f).expanded (3.0f, 0.0f), idGrMeter, -1,
                               tr ("Gain reduction (dB); the number and the line hold the deepest reduction; click to reset")));
        out.push_back (custom (outMeter().expanded (4.0f, 28.0f), idOutMeter, -1, tr ("Output level (peak, dBFS): the lamp lights when the output clips, the number holds the highest peak; click to reset")));
        const auto b = barArea();
        const auto cy = b.getCentreY();
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 130.0f, 22.0f }, P ("Band detector"), tr ("Detection"), "High-pass|Band", { 0.0f, 1.0f }));
        out.push_back (barValue ({ 146.0f, cy - 11.0f, 110.0f, 22.0f }, P ("Release"), tr ("Release")));
        // round 135: oversampling (the reference's bottom bar; Hush's own voicing)
        out.push_back (barMenu ({ 262.0f, cy - 11.0f, 112.0f, 22.0f }, P ("Oversampling"), tr ("Oversampling"), "Off|2x|4x|8x"));
        // round 148: the measured single-vocal detector (Vocal voicing)
        auto vocal = barMenu ({ 380.0f, cy - 11.0f, 118.0f, 22.0f }, P ("Vocal voicing"), tr ("Detector"), "Hush|Vocal", { 0.0f, 1.0f });
        vocal.hint = tr ("Vocal: the measured single-vocal detector (sibilants that stand out from the voice)");
        out.push_back (vocal);
        out.push_back (barValue ({ b.getRight() - 214.0f, cy - 11.0f, 100.0f, 22.0f }, P ("Input"), tr ("In")));
        out.push_back (barValue ({ b.getRight() - 108.0f, cy - 11.0f, 100.0f, 22.0f }, P ("Output"), tr ("Out")));
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idDisplay)
        {
            const auto show = ! displayShown();
            set ({ { P ("Display"), show ? 1.0f : 0.0f } }, false);
            setSize (720, show ? 500 : 500 - (int) waveH);
        }
        repaint();
    }

    void showSideChainMenu()
    {
        const auto* fx = current();
        if (fx == nullptr) return;
        juce::PopupMenu menu;
        const auto ext = v (P ("External SC")) >= 0.5f;
        menu.addItem (tr ("In: detect this signal"), true, ! ext, [this] { set ({ { P ("External SC"), 0.0f } }); });
        juce::PopupMenu from;
        const auto assign = [this] (int source) {
            const auto p = P ("External SC");
            engine.editSong ([tk = track, sl = slot, source, p] (Song& s) {
                auto& target = s.mixer[(std::size_t) tk].effects[(std::size_t) sl];
                target.sidechain = source;
                target.params[(std::size_t) p] = source >= 0 ? 1.0f : 0.0f;
            });
        };
        const auto& song = engine.getSong();
        for (int t = 1; t < (int) song.mixer.size(); ++t)
            if (t != track)
                from.addItem (juce::String (t) + " " + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()), true, ext && fx->sidechain == t, [assign, t] { assign (t); });
        menu.addSubMenu (tr ("Ext: side chain from"), from);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void customDown (const Ctl& c, const juce::MouseEvent& e) override
    {
        if (c.id == idOutMeter)
        {
            outPeakDb = -100.0f;
            repaint();
            return;
        }
        if (c.id == idGrMeter) // round 144
        {
            grHoldDb = 0.0f;
            repaint();
            return;
        }
        if (c.id == idLink) // round 165x: the 0 … 200 % link knob
        {
            if (ui::isResetClick (e))
                setLinkPos (0.5);
            else if (e.mods.isPopupMenu())
                ui::showValueMenu (*this, nullptr, linkSpec(), linkPos(), [this] (double x) { setLinkPos (x); });
            else
                valueDrag.begin (e, linkPos());
            return;
        }
        if (c.id == idExternal)
        {
            const auto* fx = current();
            const auto ext = e.position.x >= c.r.getCentreX();
            if (ext && (fx == nullptr || fx->sidechain < 0 || e.mods.isPopupMenu())) showSideChainMenu();
            else set ({ { c.param, ext ? 1.0f : 0.0f } });
            return;
        }
        if (c.id == idFreq)
        {
            const auto x = (e.position.x - freqTrack().getX()) / freqTrack().getWidth();
            dragHandle = std::abs (x - lpPos()) < std::abs (x - hpPos()) ? 1 : 0;
        }
        if (ui::isResetClick (e))
        {
            const auto p = c.id == idFreq && dragHandle == 1 ? P ("Low-pass freq.") : c.param;
            set ({ { p, (float) specFor (p).defaultValue } });
            return;
        }
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, specFor (c.param), v (c.param), [this, p = c.param] (double x) { set ({ { p, (float) x } }); });
            return;
        }
        customDrag (c, e);
    }

    void customDrag (const Ctl& c, const juce::MouseEvent& e) override
    {
        if (c.id == idOutMeter || c.id == idGrMeter)
            return;
        if (c.id == idLink)
        {
            if (valueDrag.isActive())
            {
                setLinkPos (valueDrag.update (e, linkSpec()));
                ui::showValueTip (*this, c.r.toNearestInt(), linkText());
            }
            return;
        }
        if (c.id == idFreq)
        {
            const auto tr_ = freqTrack();
            const auto x = juce::jlimit (0.0, 1.0, (double) ((e.position.x - tr_.getX()) / tr_.getWidth()));
            const auto hz = 2000.0 * std::pow (10.0, x);
            if (dragHandle == 1)
            {
                const auto lp = P ("Low-pass freq.");
                set ({ { lp, x >= 0.985 ? 1.0f : invert ([] (float y) { return mixsuite::hush::lowPassHz (y); }, juce::jmax (hz, hpHz())) } });
                ui::showValueTip (*this, tr_.toNearestInt(), tr ("Low pass") + " " + text (lp));
            }
            else
            {
                set ({ { pFreq(), mixsuite::hush::hpValue (juce::jmin (hz, ext() ? 20000.0 : 16000.0), ext()) } });
                ui::showValueTip (*this, tr_.toNearestInt(), tr ("Frequency") + " " + text (pFreq()));
            }
            return;
        }
        float db;
        if (c.id == idMeterThreshold) db = (meters().getBottom() - e.position.y) / meters().getHeight() * 60.0f - 60.0f;
        else
        {
            const auto a = wave();
            const auto f = std::abs (e.position.y - a.getCentreY()) / (a.getHeight() * 0.48f);
            db = 20.0f * std::log10 (juce::jlimit (0.001f, 1.0f, f));
        }
        set ({ { pThr(), invert ([] (float x) { return mixsuite::hush::thresholdDb (x); }, juce::jlimit (-60.0f, 0.0f, db)) } });
        ui::showValueTip (*this, juce::Rectangle<int> (80, 16).withCentre (e.position.toInt()), tr ("Threshold") + " " + text (pThr()));
    }

    ui::Hint bodyHint (juce::Point<float> p) override
    {
        ui::Hint h;
        if (wave().contains (p))
            h.text = tr ("The voice scrolls by; red: what the de-esser takes away. Drag the dashed line to set the threshold");
        return h;
    }

    void paintCustom (juce::Graphics& g, const Ctl& c) override
    {
        if (c.id == idFreq)
        {
            const auto tr_ = freqTrack();
            const auto band = v (P ("Band detector")) >= 0.5f;
            const auto x1 = tr_.getX() + tr_.getWidth() * juce::jlimit (0.0f, 1.0f, hpPos());
            const auto x2 = tr_.getX() + tr_.getWidth() * juce::jlimit (0.0f, 1.0f, lpPos());
            g.setColour (colour::bg1);
            g.fillRoundedRectangle (tr_, 4.0f);
            // the detection region: between the two handles (band mode: a band around the left one, cut at the right one)
            g.setColour (colour::warning.withAlpha (0.35f));
            const auto left = band ? x1 - 34.0f : x1;
            const auto right = band ? juce::jmin (x1 + 34.0f, x2) : x2;
            g.fillRect (juce::Rectangle<float> (left, tr_.getY() + 2.0f, juce::jmax (1.0f, right - left), tr_.getHeight() - 4.0f).getIntersection (tr_));
            g.setColour (colour::borderStrong);
            g.drawRoundedRectangle (tr_, 4.0f, 1.0f);
            const auto hot = c.r.contains (mouse) || (dragCtl == -2 && pressed.id == idFreq);
            for (const auto [x, k] : { std::pair<float, int> { x1, 0 }, std::pair<float, int> { x2, 1 } })
            {
                juce::Path handle;
                handle.addTriangle (x - 9.0f, tr_.getBottom() - 1.0f, x + 9.0f, tr_.getBottom() - 1.0f, x, tr_.getY() + 2.0f);
                g.setColour (hot && dragHandle == k ? colour::textPrimary : colour::textSecondary);
                g.fillPath (handle);
            }
            g.setFont (font (type::small, true));
            g.setColour (colour::warning);
            g.drawText (text (pFreq()), juce::Rectangle<float> (tr_.getX() - 6.0f, tr_.getBottom() + 10.0f, 70.0f, 14.0f), juce::Justification::centredLeft, false);
            const auto lpOff = v (P ("Low-pass freq.")) >= 0.999f;
            g.drawText (lpOff ? juce::String ("20 kHz") : text (P ("Low-pass freq.")), juce::Rectangle<float> (tr_.getRight() - 64.0f, tr_.getBottom() + 10.0f, 70.0f, 14.0f),
                        juce::Justification::centredRight, false);
        }
        else if (c.id == idLink)
        {
            const auto d = juce::jmin (c.r.getWidth() - 10.0f, c.r.getHeight() - 34.0f);
            const auto knobR = juce::Rectangle<float> (c.r.getCentreX() - d / 2.0f, c.r.getY() + 2.0f, d, d);
            const auto hover = c.r.contains (mouse), active = valueDrag.isActive() && pressed.id == idLink;
            skin::KnobLook look;
            look.value = linkPos();
            look.accent = accent;
            look.hover = hover ? 1.0f : 0.0f;
            look.active = active;
            look.enabled = current() == nullptr || current()->enabled;
            skin::knob (g, knobR, look);
            g.setColour (skin::captionColour().interpolatedWith (colour::textPrimary, hover ? 0.5f : 0.0f));
            g.setFont (font (type::caption, true));
            g.drawText (caps (tr ("Stereo link")), juce::Rectangle<float> (c.r.getX() - 14.0f, knobR.getBottom() + 3.0f, c.r.getWidth() + 28.0f, 14.0f),
                        juce::Justification::centred, false);
            g.setColour (skin::valueColour (active || hover));
            g.setFont (font (type::small, false));
            g.drawText (linkText(), juce::Rectangle<float> (c.r.getX() - 24.0f, knobR.getBottom() + 17.0f, c.r.getWidth() + 48.0f, 14.0f),
                        juce::Justification::centred, false);
        }
        else if (c.id == idExternal)
        {
            const auto ext = v (c.param) >= 0.5f;
            const auto half = c.r.withWidth (c.r.getWidth() / 2.0f);
            skin::button (g, half, ! ext, half.contains (mouse), accent, radius::sm);
            skin::button (g, half.translated (half.getWidth(), 0.0f), ext, half.translated (half.getWidth(), 0.0f).contains (mouse), accent, radius::sm);
            g.setColour (colour::textPrimary);
            g.setFont (font (type::small, true));
            g.drawText (tr ("In"), half, juce::Justification::centred, false);
            g.drawText (tr ("Ext"), half.translated (half.getWidth(), 0.0f), juce::Justification::centred, false);
            caption (g, { c.r.getX() - 92.0f, c.r.getY(), 88.0f, c.r.getHeight() }, tr ("Side chain"), juce::Justification::centredRight);
        }
        else if (c.id == idMeterThreshold)
        {
            juce::Path a;
            const auto r = c.r;
            a.addTriangle (r.getX(), r.getY(), r.getX(), r.getBottom(), r.getRight(), r.getCentreY());
            g.setColour (colour::warning);
            g.fillPath (a);
        }
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        const auto w = wave();
        displayBackground (g, bodyArea());
        if (displayShown())
            paintWave (g, fx, w);
        paintControlsArea (g, w);
    }

    void paintWave (juce::Graphics& g, const EffectSlot& fx, juce::Rectangle<float> w)
    {
        g.setColour (colour::bg1);
        g.fillRect (w);
        g.setColour (colour::gridBeat);
        g.drawHorizontalLine ((int) w.getCentreY(), w.getX(), w.getRight());
        const auto n = (int) w.getWidth();
        juce::Path inP, outP;
        for (int k = 0; k < n; ++k)
        {
            const auto& c = columnBack (k);
            const auto x = w.getRight() - (float) k;
            inP.addRectangle (x, waveY (c.in, true), 1.0f, juce::jmax (1.0f, waveY (c.in, false) - waveY (c.in, true)));
            outP.addRectangle (x, waveY (c.out, true), 1.0f, juce::jmax (1.0f, waveY (c.out, false) - waveY (c.out, true)));
        }
        g.setColour (colour::accent.withAlpha (fx.enabled ? 0.8f : 0.25f)); // the reduction = input − output
        g.fillPath (inP);
        g.setGradientFill (juce::ColourGradient (accent.withAlpha (0.95f), 0.0f, w.getY(), juce::Colour (0xffc8ccd6).withAlpha (0.9f), 0.0f, w.getCentreY(), true));
        g.fillPath (outP);
        // threshold lines
        const auto dashes = std::array<float, 2> { 6.0f, 4.0f };
        g.setColour (colour::warning.withAlpha (0.9f));
        for (const auto upper : { true, false })
        {
            const auto y = waveY ((float) thresholdDb(), upper);
            g.drawDashedLine (juce::Line<float> (w.getX(), y, w.getRight(), y), dashes.data(), 2, 1.2f);
        }
        g.setFont (font (type::caption, true));
        g.drawText (tr ("Threshold") + " " + text (pThr()), juce::Rectangle<float> (w.getX() + 6.0f, waveY ((float) thresholdDb(), true) - 16.0f, 160.0f, 14.0f),
                    juce::Justification::centredLeft, false);
    }

    void paintControlsArea (juce::Graphics& g, juce::Rectangle<float> w)
    {
        // the control area
        const auto area = juce::Rectangle<float> (0.0f, w.getBottom() + 4.0f, w.getWidth(), barArea().getY() - w.getBottom() - 8.0f);
        g.setColour (colour::bg2);
        g.fillRoundedRectangle (area.reduced (4.0f, 0.0f), 8.0f);
        caption (g, { 304.0f, w.getBottom() + 34.0f, 190.0f, 14.0f }, tr ("Mode"));
        caption (g, { 304.0f, w.getBottom() + 90.0f, 190.0f, 14.0f }, tr ("Processing"));

        // meters: input level and reduction, with the dB scale between them
        const auto m = meters();
        const auto newest = columnBack (0);
        drawMeter (g, m, newest.in, -60.0f, colour::meterLow.withAlpha (0.85f));
        // round 147: the detector's energy (what the threshold is compared with, DeEsser's Energy Detector) as a
        // blue bar inside the input meter, against the same scale as the threshold arrow
        if (const auto det = liveMeter (1); det > 0.0f)
        {
            const auto y = juce::jlimit (m.getY(), m.getBottom(), meterY (det * 60.0f - 60.0f));
            g.setColour (juce::Colour (0xff4aa3df).withAlpha (0.9f));
            g.fillRect (juce::Rectangle<float> (m.getCentreX() - 3.0f, y, 6.0f, m.getBottom() - y));
        }
        const auto grR = m.translated (32.0f, 0.0f).withWidth (14.0f);
        g.setColour (colour::bg1);
        g.fillRect (grR);
        g.setColour (colour::accent);
        g.fillRect (grR.withHeight (grR.getHeight() * juce::jlimit (0.0f, 1.0f, newest.gr / 24.0f)));
        // round 144: the held peak (a line on the bar and the number under it), as the reference's Attn meter
        grHoldDb = juce::jmax (grHoldDb, newest.gr);
        if (grHoldDb > 0.05f)
        {
            g.setColour (colour::textPrimary);
            g.fillRect (grR.withY (grR.getY() + grR.getHeight() * juce::jlimit (0.0f, 1.0f, grHoldDb / 24.0f) - 1.0f).withHeight (2.0f));
        }
        g.setFont (font (type::caption));
        g.setColour (colour::textSecondary);
        g.drawText (juce::String (grHoldDb, 1), juce::Rectangle<float> (grR.getX() - 8.0f, grR.getBottom() - 13.0f, grR.getWidth() + 16.0f, 12.0f), juce::Justification::centred, false);
        g.setFont (font (type::caption));
        g.setColour (colour::textDisabled);
        for (const auto db : { 0.0f, -6.0f, -12.0f, -24.0f, -36.0f, -48.0f })
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (m.getX() - 34.0f, meterY (db) - 6.0f, 20.0f, 12.0f), juce::Justification::centredRight, false);
        caption (g, { m.getX() - 6.0f, m.getY() - 16.0f, 38.0f, 14.0f }, tr ("In"));
        caption (g, { grR.getX() - 10.0f, m.getY() - 16.0f, 34.0f, 14.0f }, "GR");
        // round 135w: the output meter, −30 … 0 dBFS, a clip lamp above it and the highest peak under it
        const auto o = outMeter();
        outPeakDb = juce::jmax (outPeakDb, newest.out);
        drawMeter (g, o, newest.out, -30.0f, colour::meterLow.withAlpha (0.85f));
        const auto lamp = juce::Rectangle<float> (o.getX(), o.getY() - 10.0f, o.getWidth(), 6.0f);
        g.setColour (outPeakDb > -0.1f ? juce::Colour (0xffe5383b) : colour::bg1);
        g.fillRoundedRectangle (lamp, 2.0f);
        caption (g, { o.getX() - 10.0f, m.getY() - 16.0f, 36.0f, 14.0f }, tr ("Out"));
        g.setFont (font (type::caption));
        g.setColour (outPeakDb > -0.1f ? juce::Colour (0xffe5383b) : colour::textSecondary);
        g.drawText (outPeakDb > -99.0f ? juce::String (outPeakDb, 1) : juce::String ("-inf"), juce::Rectangle<float> (o.getX() - 10.0f, o.getBottom() + 2.0f, o.getWidth() + 20.0f, 12.0f),
                    juce::Justification::centred, false);
    }
};

// =============================================================================================================
// Crest Compressor — the reference compressor's workflow: the display is the window (input in grey, output in
// blue, the gain reduction as a red line from the top, the threshold as a dashed line that can be dragged), the
// knee / transfer curve slides out from the left edge, the knobs float over the bottom of the display (THRESHOLD,
// RATIO, the style button, ATTACK, RELEASE; KNEE, RANGE, LOOKAHEAD, HOLD under them; make-up gain and mix on the
// right), and the side chain opens below the display.
class CrestEditor final : public ProEditor
{
public:
    CrestEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::crestCompressor, d, 860, 560) { fillDemo(); }

private:
    enum { idThreshold = 1, idKnee, idSideChain, idInMeter, idScNode0 = 10 };
    // the display's level scale: the meter-scale setting (9 … 90 dB) is its floor, the upper 70 % shows its top 42 %
    LevelScale sc() const
    {
        const auto range = (float) dyn::crest::meterRangeDb (v (dyn::crest::meterScale));
        return { -range * 30.0f / 72.0f, -range };
    }
    bool kneeOpen = false, scOpen = false;
    static constexpr float scH = 170.0f;
    int scDrag = -1;

    juce::String title() const override { return "Crest Compressor"; }
    juce::String gestures() const override { return tr ("Drag the dashed threshold line  -  KNEE on the left: the curve  -  SIDE CHAIN: the detector"); }
    bool usesHistory() const override { return true; }
    float grDb() const override { return liveMeter (0) * 30.0f; }

    juce::Rectangle<float> main() const { return bodyArea().withTrimmedBottom (scOpen ? scH : 0.0f); }
    juce::Rectangle<float> plot() const { return main().withTrimmedRight (100.0f).withTrimmedTop (10.0f).withTrimmedBottom (8.0f); }
    juce::Rectangle<float> scPanel() const { return { 0.0f, main().getBottom(), (float) getWidth(), scH }; }
    juce::Rectangle<float> kneeArea() const { auto p = plot(); return { p.getX(), p.getY(), juce::jmin (330.0f, p.getWidth() * 0.42f), p.getHeight() * 0.62f }; }
    double thresholdDb() const { return dyn::crest::thresholdDb (v (dyn::crest::threshold)); }

    Column demoColumn (int i) const override
    {
        const auto* fx = current();
        const auto in = demoInDb (i) + 6.0f;
        float gr = 0.0f;
        if (fx != nullptr)
            for (int k = 0; k < 30 && i - k >= 0; ++k)
                gr = juce::jmax (gr, (float) -dyn::crest::staticGainDb (fx->params, demoInDb (i - k) + 6.0f) - 0.4f * (float) k);
        return { in, in - gr, gr };
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        using namespace dyn::crest;
        const auto p = plot();
        out.push_back (custom ({ p.getX() + 24.0f, sc().y ((float) thresholdDb(), p) - 5.0f, p.getWidth() - 24.0f, 10.0f }, idThreshold, threshold,
                               tr ("Threshold: drag up or down")));
        out.push_back (button ({ kneeOpen ? kneeArea().getRight() : p.getX(), p.getY() + 80.0f, 20.0f, 64.0f }, idKnee,
                               juce::String::fromUTF8 (kneeOpen ? "\xc2\xab" : "\xc2\xbb"), kneeOpen, tr ("Knee: show the transfer curve")));
        // the knob row over the bottom of the display
        const auto by = p.getBottom() - 190.0f;
        out.push_back (knob ({ 96.0f, by, 104.0f, 118.0f }, threshold, tr ("Threshold"), 2));
        out.push_back (toggle ({ 108.0f, by - 26.0f, 80.0f, 20.0f }, autoThreshold, tr ("Auto")));
        out.push_back (knob ({ 214.0f, by, 104.0f, 118.0f }, ratio, tr ("Ratio"), 2));
        out.push_back (menu ({ 330.0f, by + 54.0f, 104.0f, 28.0f }, style, {}, "Clean|Allround|Punch|Upward|Two-way|Vari-mu|Classic|Opto|Vocal|Bus|Pumping"));
        out.push_back (menu ({ 342.0f, by + 90.0f, 80.0f, 20.0f }, character, tr ("Character"), "Off|Warm|Crisp|Bright"));
        out.push_back (knob ({ 446.0f, by, 104.0f, 118.0f }, attack, tr ("Attack"), 2));
        out.push_back (knob ({ 564.0f, by, 104.0f, 118.0f }, release, tr ("Release"), 2));
        out.push_back (toggle ({ 576.0f, by - 26.0f, 80.0f, 20.0f }, autoRelease, tr ("Auto")));
        out.push_back (knob ({ 680.0f, by - 6.0f, 62.0f, 82.0f }, P ("Output"), tr ("Gain"), 1, true));
        out.push_back (toggle ({ 670.0f, by + 76.0f, 82.0f, 20.0f }, autoGain, tr ("Auto gain")));
        out.push_back (knob ({ 680.0f, by + 100.0f, 62.0f, 82.0f }, dryGain, tr ("Dry")));
        // the display's scale (bottom right, like the reference's dB box) and the input meter switch
        out.push_back (menu ({ p.getRight() - 16.0f, p.getBottom() - 20.0f, 60.0f, 20.0f }, meterScale, {}, "9 dB|18 dB|36 dB|72 dB|90 dB"));
        out.push_back (button ({ p.getRight() + 42.0f, main().getY(), 18.0f, 12.0f }, idInMeter, "IN", v (inputMeter) >= 0.5f,
                               tr ("Input meter: show or hide")));
        const auto sy = p.getBottom() - 64.0f;
        out.push_back (knob ({ 110.0f, sy, 76.0f, 64.0f }, knee, tr ("Knee"), 0));
        out.push_back (knob ({ 228.0f, sy, 76.0f, 64.0f }, range, tr ("Range"), 0));
        out.push_back (knob ({ 460.0f, sy, 76.0f, 64.0f }, lookahead, tr ("Lookahead"), 0));
        out.push_back (knob ({ 578.0f, sy, 76.0f, 64.0f }, hold, tr ("Hold"), 0));
        out.push_back (button ({ 330.0f, sy + 34.0f, 104.0f, 24.0f }, idSideChain, tr ("Side chain"), scOpen, tr ("Open the side chain: detector EQ, level, external input, stereo link")));
        if (scOpen)
        {
            const auto s = scPanel();
            out.push_back (toggle ({ 14.0f, s.getY() + 14.0f, 104.0f, 22.0f }, scListen, tr ("Listen")));
            out.push_back (tabs ({ 14.0f, s.getY() + 44.0f, 104.0f, 24.0f }, scExternal, "Int|Ext"));
            out.push_back (knob ({ 130.0f, s.getY() + 12.0f, 70.0f, 82.0f }, scLevel, tr ("Level"), 1, true));
            out.push_back (knob ({ 130.0f, s.getY() + 92.0f, 70.0f, 70.0f }, stereoLink, tr ("Stereo link"), 0));
            out.push_back (menu ({ 14.0f, s.getY() + 108.0f, 104.0f, 22.0f }, linkMode, tr ("Link mode"), "Stereo|Mid only|Side only|Mid > side|Side > mid"));
            for (int b = 0; b < scBands; ++b)
                out.push_back (menu ({ 216.0f, s.getY() + 16.0f + 46.0f * (float) b, 92.0f, 22.0f }, scParam (b, 0), {},
                                     "Off|Bell|Low shelf|High shelf|Low cut|High cut|Notch|Band pass|Tilt shelf"));
            out.push_back (custom (scEqArea(), idScNode0, -1, tr ("Detector EQ: drag a node (frequency / gain), wheel = Q")));
            // round 135: the wet (compressed) and dry paths' stereo balance, at the panel's right end
            out.push_back (knob ({ s.getRight() - 136.0f, s.getY() + 14.0f, 62.0f, 82.0f }, wetPan, tr ("Wet pan"), 0, true));
            out.push_back (knob ({ s.getRight() - 70.0f, s.getY() + 14.0f, 62.0f, 82.0f }, dryPan, tr ("Dry pan"), 0, true));
            // round 137: the trigger (the reference's side-chain Host Sync / MIDI inputs): the detector hears a pulse
            const auto tx = s.getRight() - 268.0f;
            out.push_back (menu ({ tx, s.getY() + 16.0f, 120.0f, 22.0f }, trigger, tr ("Trigger"), "Off|Host sync|MIDI"));
            const auto trig = triggerOf (v (trigger));
            if (trig == Trigger::hostSync)
            {
                out.push_back (menu ({ tx, s.getY() + 46.0f, 120.0f, 22.0f }, triggerSync, tr ("Note"), "1/1|1/2|1/4|1/8|1/16"));
                out.push_back (knob ({ tx, s.getY() + 74.0f, 58.0f, 78.0f }, triggerRate, tr ("Rate"), 0));
                out.push_back (knob ({ tx + 62.0f, s.getY() + 74.0f, 58.0f, 78.0f }, triggerLength, tr ("Length"), 0));
            }
            else if (trig == Trigger::midi)
                out.push_back (knob ({ tx + 20.0f, s.getY() + 50.0f, 80.0f, 92.0f }, triggerChannel, tr ("Channel"), 0));
        }
        const auto b = barArea();
        const auto cy = b.getCentreY();
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 150.0f, 22.0f }, oversampling, tr ("Oversampling"), "Off|2x|4x|8x"));
        out.push_back (barMenu ({ 165.0f, cy - 11.0f, 140.0f, 22.0f }, maxLookahead, tr ("Lookahead"), "Off|1 ms|2 ms|5 ms|10 ms|20 ms"));
        out.push_back (barMenu ({ 310.0f, cy - 11.0f, 125.0f, 22.0f }, characterPre, tr ("Character"), "Post|Pre"));
        out.push_back (barValue ({ 440.0f, cy - 11.0f, 110.0f, 22.0f }, mix, tr ("Mix")));
        out.push_back (barValue ({ b.getRight() - 300.0f, cy - 11.0f, 140.0f, 22.0f }, P ("Input"), tr ("In")));
        out.push_back (barValue ({ b.getRight() - 150.0f, cy - 11.0f, 140.0f, 22.0f }, P ("Output"), tr ("Out")));
    }

    juce::Rectangle<float> scEqArea() const
    {
        const auto s = scPanel();
        return { 320.0f, s.getY() + 10.0f, s.getWidth() - 334.0f - 140.0f - 132.0f, s.getHeight() - 20.0f }; // round 137: − the trigger column
    }
    float scX (double hz) const
    {
        const auto a = scEqArea();
        return a.getX() + a.getWidth() * (float) (std::log (juce::jlimit (20.0, 20000.0, hz) / 20.0) / std::log (1000.0));
    }
    float scY (double db) const
    {
        const auto a = scEqArea();
        return a.getCentreY() - a.getHeight() * 0.5f * juce::jlimit (-1.0f, 1.0f, (float) db / 24.0f);
    }
    juce::Point<float> scNode (int b) const
    {
        using namespace dyn::crest;
        return { scX (facet::hz (v (scParam (b, 1)))), scY (facet::gainDb (v (scParam (b, 2)))) };
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idKnee) kneeOpen = ! kneeOpen;
        if (c.id == idInMeter) set ({ { dyn::crest::inputMeter, v (dyn::crest::inputMeter) >= 0.5f ? 0.0f : 1.0f } });
        if (c.id == idSideChain)
        {
            scOpen = ! scOpen;
            setSize (getWidth(), getHeight() + (int) (scOpen ? scH : -scH));
        }
        repaint();
    }

    void customDown (const Ctl& c, const juce::MouseEvent& e) override
    {
        if (c.id == idScNode0)
        {
            scDrag = -1;
            for (int b = 0; b < dyn::crest::scBands; ++b)
                if (facet::shape (v (dyn::crest::scParam (b, 0))) != facet::Shape::off && scNode (b).getDistanceFrom (e.position) < 10.0f)
                    scDrag = b;
            return;
        }
        if (ui::isResetClick (e)) { set ({ { c.param, (float) specFor (c.param).defaultValue } }); return; }
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, specFor (c.param), v (c.param), [this, p = c.param] (double x) { set ({ { p, (float) x } }); });
            return;
        }
        customDrag (c, e);
    }

    void customDrag (const Ctl& c, const juce::MouseEvent& e) override
    {
        using namespace dyn::crest;
        if (c.id == idScNode0)
        {
            if (scDrag < 0) return;
            const auto a = scEqArea();
            const auto hz = 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, (double) ((e.position.x - a.getX()) / a.getWidth())));
            const auto db = juce::jlimit (-24.0, 24.0, (double) ((a.getCentreY() - e.position.y) / (a.getHeight() * 0.5f)) * 24.0);
            set ({ { scParam (scDrag, 1), facet::hzValue (hz) }, { scParam (scDrag, 2), facet::gainValue (db) } });
            ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (scNode (scDrag).toInt()), text (scParam (scDrag, 1)) + "  " + text (scParam (scDrag, 2)));
            return;
        }
        const auto db = juce::jlimit (-60.0f, 0.0f, sc().db (e.position.y, plot()));
        set ({ { threshold, invert ([] (float x) { return dyn::crest::thresholdDb (x); }, db) } });
        ui::showValueTip (*this, juce::Rectangle<int> (80, 16).withCentre (e.position.toInt()), tr ("Threshold") + " " + text (threshold));
    }

    bool bodyWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! scOpen || ! scEqArea().contains (e.position)) return false;
        for (int b = 0; b < dyn::crest::scBands; ++b)
            if (scNode (b).getDistanceFrom (e.position) < 14.0f)
            {
                const auto p = dyn::crest::scParam (b, 3);
                if (const auto x = ui::wheelValue (v (p), e, w, specFor (p))) set ({ { p, (float) *x } });
                return true;
            }
        return false;
    }

    void paintCustom (juce::Graphics& g, const Ctl& c) override
    {
        if (c.id != idScNode0) return;
        const auto* fx = current();
        if (fx == nullptr) return;
        dynui::History none;
        dynui::paint (g, "crestsceq", c.r, 1.0f, *fx, [] (int) { return 0.0f; }, none);
        for (int b = 0; b < dyn::crest::scBands; ++b)
        {
            if (facet::shape (v (dyn::crest::scParam (b, 0))) == facet::Shape::off) continue;
            const auto n = scNode (b);
            g.setColour (peqview::bandColour (b));
            g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (n));
            g.setColour (colour::bg0);
            g.setFont (font (type::caption, true));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (12.0f, 12.0f).withCentre (n), juce::Justification::centred, false);
        }
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        const auto m = main();
        const auto p = plot();
        displayBackground (g, bodyArea());
        {
            // the marks of the chosen scale: six steps over the upper part, two more down to the floor
            const auto s = sc();
            const auto step = -s.knee / 5.0f;
            const auto lower = std::round ((s.knee + s.floor) * 0.5f / 3.0f) * 3.0f; // one mark between the knee and the floor
            const float marks[] { 0.0f, -step, -2.0f * step, -3.0f * step, -4.0f * step, s.knee, lower };
            g.setFont (font (type::caption));
            for (const auto mk : marks)
            {
                const auto y = s.y (mk, p);
                g.setColour (colour::gridBeat);
                g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
                g.setColour (colour::textDisabled);
                const auto label = mk == 0.0f ? juce::String ("0 dB")
                                   : std::abs (mk) < 10.0f && std::abs (mk - std::round (mk)) > 0.05f ? juce::String (mk, 1) : juce::String (juce::roundToInt (mk));
                g.drawText (label, juce::Rectangle<float> (p.getRight() + 8.0f, y - 6.0f, 34.0f, 12.0f), juce::Justification::centredLeft, false);
            }
        }
        const auto n = (int) p.getWidth();
        juce::Path inP, outLine, grLine;
        inP.startNewSubPath (p.getRight(), p.getBottom());
        for (int k = 0; k < n; ++k)
        {
            const auto& c = columnBack (k);
            const auto x = p.getRight() - (float) k;
            inP.lineTo (x, sc().y (c.in, p));
            const juce::Point<float> o { x, sc().y (c.out, p) }, r { x, sc().y (-c.gr, p) };
            if (k == 0) { outLine.startNewSubPath (o); grLine.startNewSubPath (r); }
            else { outLine.lineTo (o); grLine.lineTo (r); }
        }
        inP.lineTo (p.getX(), p.getBottom());
        inP.closeSubPath();
        fillLevel (g, inP, inColour(), p, 0.42f, 0.12f);
        g.setColour (outColour().withAlpha (0.9f));
        g.strokePath (outLine, juce::PathStrokeType (1.2f));
        g.setColour (colour::accent.withAlpha (fx.enabled ? 1.0f : 0.3f));
        g.strokePath (grLine, juce::PathStrokeType (2.0f));
        // threshold
        const auto ty = sc().y ((float) thresholdDb(), p);
        const auto dashes = std::array<float, 2> { 7.0f, 5.0f };
        g.setColour (colour::textSecondary);
        g.drawDashedLine (juce::Line<float> (p.getX(), ty, p.getRight(), ty), dashes.data(), 2, 1.2f);
        g.setFont (font (type::caption, true));
        g.drawText (text (dyn::crest::threshold), juce::Rectangle<float> (p.getRight() - 90.0f, ty - 16.0f, 84.0f, 14.0f), juce::Justification::centredRight, false);
        // a darker band under the knobs keeps them readable over the history
        const auto knobs = juce::Rectangle<float> (p.getX(), p.getBottom() - 206.0f, p.getWidth(), 206.0f);
        g.setGradientFill (juce::ColourGradient (colour::bg0.withAlpha (0.0f), 0.0f, knobs.getY(), colour::bg0.withAlpha (0.85f), 0.0f, knobs.getY() + 60.0f, false));
        g.fillRect (knobs);
        // the transfer curve, slid out from the left
        if (kneeOpen)
        {
            const auto k = kneeArea();
            drawPanel (g, k.expanded (2.0f));
            dynui::paint (g, "crestcurve", k, 1.0f, fx, [this] (int i) { return liveMeter (i); }, none);
        }
        // meters: input, output and gain reduction
        const auto mr = juce::Rectangle<float> (p.getRight() + 46.0f, p.getY(), 50.0f, p.getHeight());
        const auto nw = columnBack (0);
        if (v (dyn::crest::inputMeter) >= 0.5f)
            drawMeter (g, mr.withWidth (14.0f), nw.in, -60.0f, colour::meterLow.withAlpha (0.8f));
        drawMeter (g, mr.withX (mr.getX() + 17.0f).withWidth (14.0f), nw.out, -60.0f, outColour());
        const auto gr = mr.withX (mr.getX() + 34.0f).withWidth (14.0f);
        skin::meterSolid (g, gr, (sc().y (-nw.gr, p) - gr.getY()) / juce::jmax (1.0f, gr.getHeight()), colour::accent, true);
        g.setFont (font (type::caption));
        g.setColour (colour::textDisabled);
        g.drawText ("OUT  GR", mr.withY (m.getY()).withHeight (12.0f).withTrimmedLeft (17.0f).expanded (6.0f, 0.0f), juce::Justification::centred, false);
        if (scOpen)
        {
            const auto s = scPanel();
            g.setColour (colour::bg2);
            g.fillRect (s);
            g.setColour (colour::border);
            g.drawHorizontalLine ((int) s.getY(), 0.0f, s.getRight());
            for (int b = 0; b < dyn::crest::scBands; ++b)
                caption (g, { 216.0f, s.getY() + 40.0f + 46.0f * (float) b, 92.0f, 12.0f }, tr ("Band {0}", { juce::String (b + 1) }));
        }
    }

    dynui::History none;
};

// =============================================================================================================
// Sentry Gate — the reference gate's workflow: THRESHOLD (big) with RATIO and RANGE on the left, the display in the
// middle (the input level scrolling behind the transfer curve, the threshold dashed — drag it), the style button and
// ATTACK / RELEASE on the right with HOLD, KNEE and LOOKAHEAD beside them, the EXPERT strip (side chain, filters,
// upward expansion, dry, output) opening below.
class SentryEditor final : public ProEditor
{
public:
    SentryEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::sentryGate, d, 730, 380) { fillDemo(); }

private:
    enum { idThreshold = 1, idExpert, idFilters };
    bool expertOpen = false;
    static constexpr float expertH = 240.0f;
    int filterDrag = -1;

    juce::String title() const override { return "Sentry Gate"; }
    juce::String gestures() const override { return tr ("Drag the dashed threshold  -  EXPERT: side chain and filters"); }
    bool usesHistory() const override { return true; }
    float grDb() const override { return liveMeter (0) * 30.0f; }

    juce::Rectangle<float> main() const { return bodyArea().withTrimmedBottom (expertOpen ? expertH : 0.0f); }
    juce::Rectangle<float> disp() const { const auto m = main(); return { 196.0f, m.getY() + 12.0f, 270.0f, m.getHeight() - 58.0f }; }
    juce::Rectangle<float> expert() const { return { 0.0f, main().getBottom(), (float) getWidth(), expertH }; }
    double thresholdDb() const { return dyn::sentry::thresholdDb (v (dyn::sentry::threshold)); }
    float yDb (float db) const { const auto d = disp(); return d.getY() + d.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 66.0f); }
    float xDb (float db) const { const auto d = disp(); return d.getX() + d.getWidth() * juce::jlimit (0.0f, 1.0f, (db + 66.0f) / 66.0f); }
    juce::Rectangle<float> filterTrack() const { const auto e = expert(); return { 240.0f, e.getY() + 60.0f, 220.0f, 22.0f }; }

    Column demoColumn (int i) const override
    {
        const auto* fx = current();
        const auto in = demoInDb (i) - 8.0f;
        const auto g = fx != nullptr ? (float) -dyn::sentry::staticGainDb (fx->params, in) : 0.0f;
        return { in, in - g, juce::jmax (0.0f, g) };
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        using namespace dyn::sentry;
        const auto m = main();
        const auto d = disp();
        out.push_back (custom ({ d.getX(), yDb ((float) thresholdDb()) - 5.0f, d.getWidth(), 10.0f }, idThreshold, threshold, tr ("Threshold: drag up or down")));
        out.push_back (custom ({ xDb ((float) thresholdDb()) - 5.0f, d.getY(), 10.0f, d.getHeight() }, idThreshold, threshold, tr ("Threshold: drag left or right")));
        out.push_back (knob ({ 30.0f, m.getY() + 18.0f, 130.0f, 152.0f }, threshold, tr ("Threshold"), 2));
        out.push_back (knob ({ 10.0f, m.getY() + 182.0f, 84.0f, 104.0f }, ratio, tr ("Ratio")));
        out.push_back (knob ({ 98.0f, m.getY() + 182.0f, 84.0f, 104.0f }, range, tr ("Range")));
        out.push_back (menu ({ 530.0f, m.getY() + 12.0f, 100.0f, 26.0f }, style, {}, "Classic|Clean|Vocal|Guitar|Upward"));
        if (v (ducking) >= 0.5f)
            out.push_back (toggle ({ 530.0f, m.getY() + 39.0f, 100.0f, 14.0f }, ducking, tr ("Ducking")));
        out.push_back (knob ({ 530.0f, m.getY() + 54.0f, 100.0f, 108.0f }, attack, tr ("Attack")));
        out.push_back (knob ({ 530.0f, m.getY() + 178.0f, 100.0f, 108.0f }, release, tr ("Release")));
        out.push_back (knob ({ 644.0f, m.getY() + 8.0f, 76.0f, 88.0f }, hold, tr ("Hold"), 0));
        out.push_back (knob ({ 644.0f, m.getY() + 102.0f, 76.0f, 88.0f }, knee, tr ("Knee"), 0));
        out.push_back (knob ({ 644.0f, m.getY() + 196.0f, 76.0f, 88.0f }, lookahead, tr ("Lookahead"), 0));
        out.push_back (toggle ({ 646.0f, m.getY() + 290.0f, 72.0f, 20.0f }, lookaheadOn, tr ("On")));
        out.push_back (button ({ d.getCentreX() - 60.0f, m.getBottom() - 30.0f, 120.0f, 24.0f }, idExpert, tr ("Expert"), expertOpen,
                               tr ("Expert: side chain (internal / external, gain and mix per channel), side-chain filters, upward expansion, ducking, wet / dry level and pan")));
        if (expertOpen)
        {
            const auto e = expert();
            out.push_back (knob ({ 14.0f, e.getY() + 14.0f, 84.0f, 100.0f }, upThreshold, tr ("Up threshold")));
            out.push_back (knob ({ 104.0f, e.getY() + 14.0f, 84.0f, 100.0f }, upRatio, tr ("Up ratio")));
            out.push_back (tabs ({ filterTrack().getCentreX() - 70.0f, e.getY() + 18.0f, 140.0f, 24.0f }, scExternal, "Int|Ext"));
            out.push_back (custom (filterTrack().expanded (8.0f, 6.0f), idFilters, -1, tr ("Side-chain filters: drag the left handle (high pass) or the right one (low pass)")));
            out.push_back (toggle ({ filterTrack().getCentreX() - 50.0f, filterTrack().getBottom() + 14.0f, 100.0f, 22.0f }, scListen, tr ("Audition")));
            out.push_back (knob ({ 530.0f, e.getY() + 14.0f, 84.0f, 100.0f }, dry, tr ("Dry")));
            out.push_back (knob ({ 630.0f, e.getY() + 14.0f, 84.0f, 100.0f }, P ("Output"), tr ("Output"), 1, true));
            // round 128: the side-chain gain and mix of each detector channel (left / right, or mid / side), ducking,
            // the wet and dry pans
            const bool msMode = v (channelMode) >= 0.5f;
            const auto ry = e.getY() + 130.0f;
            out.push_back (knob ({ 10.0f, ry, 70.0f, 96.0f }, scGainL, msMode ? tr ("Gain M") : tr ("Gain L"), 1, true));
            out.push_back (knob ({ 80.0f, ry, 70.0f, 96.0f }, scMixL, msMode ? tr ("Mix M") : tr ("Mix L"), 0, true));
            out.push_back (knob ({ 160.0f, ry, 70.0f, 96.0f }, scGainR, msMode ? tr ("Gain S") : tr ("Gain R"), 1, true));
            out.push_back (knob ({ 230.0f, ry, 70.0f, 96.0f }, scMixR, msMode ? tr ("Mix S") : tr ("Mix R"), 0, true));
            out.push_back (toggle ({ 330.0f, ry + 36.0f, 110.0f, 22.0f }, ducking, tr ("Ducking")));
            out.push_back (knob ({ 530.0f, ry, 84.0f, 96.0f }, wetPan, tr ("Wet pan"), 0, true));
            out.push_back (knob ({ 630.0f, ry, 84.0f, 96.0f }, dryPan, tr ("Dry pan"), 0, true));
        }
        const auto b = barArea();
        const auto cy = b.getCentreY();
        // round 135: the oversampling menu (Pro-G's bottom bar); the look-ahead switch stays under its knob
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 140.0f, 22.0f }, oversampling, tr ("Oversampling"), "Off|2x|4x|8x"));
        out.push_back (barMenu ({ 155.0f, cy - 11.0f, 145.0f, 22.0f }, scExternal, tr ("Side chain"), "Internal|External"));
        out.push_back (barMenu ({ 305.0f, cy - 11.0f, 150.0f, 22.0f }, channelMode, tr ("Channel mode"), "Left/Right|Mid/Side"));
        out.push_back (barValue ({ b.getRight() - 275.0f, cy - 11.0f, 128.0f, 22.0f }, input, tr ("In")));
        out.push_back (barValue ({ b.getRight() - 142.0f, cy - 11.0f, 132.0f, 22.0f }, P ("Output"), tr ("Out")));
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idExpert)
        {
            expertOpen = ! expertOpen;
            setSize (getWidth(), getHeight() + (int) (expertOpen ? expertH : -expertH));
        }
        repaint();
    }

    void customDown (const Ctl& c, const juce::MouseEvent& e) override
    {
        using namespace dyn::sentry;
        if (c.id == idFilters)
        {
            const auto t = filterTrack();
            const auto xh = t.getX() + t.getWidth() * v (highPass), xl = t.getX() + t.getWidth() * v (lowPass);
            filterDrag = std::abs (e.position.x - xh) < std::abs (e.position.x - xl) ? highPass : lowPass;
            if (ui::isResetClick (e)) { set ({ { filterDrag, filterDrag == highPass ? 0.0f : 1.0f } }); filterDrag = -1; return; }
            customDrag (c, e);
            return;
        }
        if (ui::isResetClick (e)) { set ({ { c.param, (float) specFor (c.param).defaultValue } }); return; }
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, specFor (c.param), v (c.param), [this, p = c.param] (double x) { set ({ { p, (float) x } }); });
            return;
        }
        customDrag (c, e);
    }

    void customDrag (const Ctl& c, const juce::MouseEvent& e) override
    {
        using namespace dyn::sentry;
        if (c.id == idFilters)
        {
            if (filterDrag < 0) return;
            const auto t = filterTrack();
            set ({ { filterDrag, juce::jlimit (0.0f, 1.0f, (e.position.x - t.getX()) / t.getWidth()) } });
            ui::showValueTip (*this, t.toNearestInt(), text (filterDrag));
            return;
        }
        const auto d = disp();
        const auto vertical = c.r.getWidth() < 20.0f; // the vertical line moves left / right
        const auto db = vertical ? (e.position.x - d.getX()) / d.getWidth() * 66.0f - 66.0f : -(e.position.y - d.getY()) / d.getHeight() * 66.0f;
        set ({ { threshold, invert ([] (float x) { return dyn::sentry::thresholdDb (x); }, juce::jlimit (-60.0f, 0.0f, db)) } });
        ui::showValueTip (*this, juce::Rectangle<int> (80, 16).withCentre (e.position.toInt()), tr ("Threshold") + " " + text (threshold));
    }

    void paintCustom (juce::Graphics& g, const Ctl& c) override
    {
        using namespace dyn::sentry;
        if (c.id != idFilters) return;
        const auto t = filterTrack();
        const auto xh = t.getX() + t.getWidth() * v (highPass), xl = t.getX() + t.getWidth() * v (lowPass);
        g.setColour (colour::bg1);
        g.fillRoundedRectangle (t, 4.0f);
        g.setColour (colour::info.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (xh, t.getY() + 2.0f, juce::jmax (0.0f, xl - xh), t.getHeight() - 4.0f));
        for (const auto x : { xh, xl })
        {
            g.setColour (colour::textPrimary);
            g.fillRoundedRectangle (juce::Rectangle<float> (8.0f, t.getHeight() + 6.0f).withCentre ({ x, t.getCentreY() }), 2.0f);
        }
        g.setFont (font (type::small));
        g.setColour (colour::warning);
        g.drawText (text (highPass), juce::Rectangle<float> (t.getX() - 30.0f, t.getBottom() + 4.0f, 80.0f, 14.0f), juce::Justification::centredLeft, false);
        g.drawText (text (lowPass), juce::Rectangle<float> (t.getRight() - 50.0f, t.getBottom() + 4.0f, 80.0f, 14.0f), juce::Justification::centredRight, false);
        caption (g, { t.getX(), t.getY() - 16.0f, t.getWidth(), 14.0f }, tr ("Side-chain filters"));
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        using namespace dyn::sentry;
        const auto d = disp();
        skin::canvas (g, bodyArea(), accent);
        skin::well (g, d.expanded (4.0f), 6.0f);
        // the input level scrolling behind (newest at the right), on the vertical dB scale
        const auto n = (int) d.getWidth();
        juce::Path in;
        in.startNewSubPath (d.getRight(), d.getBottom());
        for (int k = 0; k < n; ++k)
            in.lineTo (d.getRight() - (float) k, yDb (columnBack (k / 2).in));
        in.lineTo (d.getX(), d.getBottom());
        in.closeSubPath();
        fillLevel (g, in, inColour(), d, 0.45f, 0.12f);
        // grid and labels
        g.setFont (font (type::caption));
        for (int db = 0; db >= -60; db -= 10)
        {
            g.setColour (colour::gridBeat);
            g.drawHorizontalLine ((int) yDb ((float) db), d.getX(), d.getRight());
            g.drawVerticalLine ((int) xDb ((float) db), d.getY(), d.getBottom());
            g.setColour (colour::textDisabled);
            g.drawText (juce::String (db), juce::Rectangle<float> (d.getRight() + 6.0f, yDb ((float) db) - 6.0f, 26.0f, 12.0f), juce::Justification::centredLeft, false);
            g.drawText (juce::String (db), juce::Rectangle<float> (xDb ((float) db) - 14.0f, d.getBottom() + 4.0f, 28.0f, 12.0f), juce::Justification::centred, false);
        }
        // the transfer curve (input → output)
        juce::Path curve;
        for (int i = 0; i <= 132; ++i)
        {
            const auto inDb = -66.0f + (float) i * 0.5f;
            const auto outDb = inDb + (float) staticGainDb (fx.params, inDb);
            const juce::Point<float> pt { xDb (inDb), yDb (outDb) };
            if (i == 0) curve.startNewSubPath (pt);
            else curve.lineTo (pt);
        }
        if (! fx.enabled)
            skin::glowStroke (g, curve, colour::textDisabled, 2.2f, 0.0f);
        else
        {
            g.setColour (accent.withAlpha (0.25f));
            g.strokePath (curve, juce::PathStrokeType (6.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (colour::textPrimary);
            g.strokePath (curve, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        // the live input on the curve
        const auto now = columnBack (0).in;
        if (now > -66.0f)
        {
            g.setColour (colour::accent);
            g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ xDb (now), yDb (now + (float) staticGainDb (fx.params, now)) }));
        }
        // threshold
        const auto dashes = std::array<float, 2> { 5.0f, 4.0f };
        const auto tx = xDb ((float) thresholdDb()), ty = yDb ((float) thresholdDb());
        g.setColour (colour::warning.withAlpha (0.85f));
        g.drawDashedLine (juce::Line<float> (d.getX(), ty, d.getRight(), ty), dashes.data(), 2, 1.0f);
        g.drawDashedLine (juce::Line<float> (tx, d.getY(), tx, d.getBottom()), dashes.data(), 2, 1.0f);
        // meter: input level and gain reduction
        const auto mr = juce::Rectangle<float> (d.getRight() + 32.0f, d.getY(), 10.0f, d.getHeight());
        g.setColour (colour::bg0);
        g.fillRect (mr);
        g.setColour (colour::meterLow.withAlpha (0.85f));
        g.fillRect (mr.withTop (yDb (now)));
        g.setColour (colour::accent);
        g.fillRect (mr.translated (12.0f, 0.0f).withBottom (d.getY() + d.getHeight() * juce::jlimit (0.0f, 1.0f, columnBack (0).gr / 66.0f)));
        if (expertOpen)
        {
            const auto e = expert();
            g.setColour (colour::bg2);
            g.fillRect (e);
            g.setColour (colour::border);
            g.drawHorizontalLine ((int) e.getY(), 0.0f, e.getRight());
            caption (g, { 14.0f, e.getY() + 2.0f, 174.0f, 12.0f }, tr ("Upward expansion"));
            caption (g, { filterTrack().getCentreX() - 70.0f, e.getY() + 2.0f, 140.0f, 12.0f }, tr ("Side chain"));
            caption (g, { 10.0f, e.getY() + 118.0f, 290.0f, 12.0f }, v (channelMode) >= 0.5f ? tr ("Side-chain mid / side") : tr ("Side-chain left / right"));
            caption (g, { 530.0f, e.getY() + 118.0f, 184.0f, 12.0f }, tr ("Wet / dry balance"));
            g.setColour (colour::border.withAlpha (0.5f));
            g.drawHorizontalLine ((int) (e.getY() + 114.0f), 10.0f, e.getRight() - 10.0f);
        }
    }
};
// =============================================================================================================
// Prism Multiband — the reference multiband's workflow: the spectrum is the window, each band a coloured region
// with a node on the 0 dB line — drag it up / down for the range (how far the band may move), left / right to move
// the band; drag a region's edge to move a crossover; the band's live gain change fills the region; click a band
// for its floating panel (threshold, range, attack, release, output, mode, ratio, knee).
class PrismEditor final : public ProEditor
{
public:
    PrismEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::prismMultiband, d, 920, 600)
    {
        specArea = plot();
        specMinHz = 20.0;
        specMaxHz = 20000.0;
        fillDemo();
    }

private:
    enum { idPrev = 1, idNext, idClose, idRange, idAnalyzer, idClassic, idExpert, idClassicDet, idSplit, idFree, idSolo0 = 20, idMute0 = 30 };
    int selected = 0, hoverBand = -1, dragBand = -1, dragEdge = -1;
    bool expert = false; // round 147: the band panel's expert part (side-chain filtering, stereo link), view only
    double rangeView = 12.0;
    int analyzer = 3; // 0 off, 1 pre, 2 post, 3 pre + post (view only)
    juce::Point<float> dragStart;
    float dragX0[dyn::prism::bands - 1] {};
    float dragLo0 = 0.0f, dragHi0 = 0.0f; // round 150: the free band's edges when its drag started

    juce::String title() const override { return "Prism Multiband"; }
    juce::String gestures() const override { return tr ("Drag a node: range (up / down), band (left / right)  -  drag an edge: crossover  -  double-click: new band  -  wheel: threshold"); }
    bool usesSpectrum() const override { return analyzer > 0; }

    juce::Rectangle<float> plot() const { return bodyArea().withTrimmedRight (74.0f).withTrimmedTop (8.0f).withTrimmedBottom (24.0f); }
    juce::Rectangle<float> panelArea() const
    {
        const auto p = plot();
        const auto w = expert ? 820.0f : 600.0f;
        return { p.getCentreX() - w * 0.5f, p.getBottom() - 176.0f, w, 166.0f };
    }

    static int bp (int b, int f) { return dyn::prism::param (b, f); }
    bool bandOn (int b) const { return v (bp (b, dyn::prism::fEnabled)) >= 0.5f; }
    // round 135: up to six bands (the band count); a double-click splits a band, the band menu removes one
    int nb() const
    {
        const auto* fx = current();
        return fx != nullptr ? dyn::prism::bandCount (fx->params) : dyn::prism::legacyBands;
    }
    double xoHz (int i) const
    {
        const auto* fx = current();
        return fx != nullptr ? dyn::prism::crossovers (fx->params)[(std::size_t) i] : dyn::prism::crossoverHz (v (dyn::prism::crossoverParam (i)));
    }
    float edgeX (int i) const // 0 … band count
    {
        const auto p = plot();
        if (i <= 0) return p.getX();
        if (i >= nb()) return p.getRight();
        return xForHz (xoHz (i - 1), p);
    }
    // round 150: the free layout (Pro-MB's): each band has its own edges, the room between bands passes untouched
    bool isFree() const { return v (dyn::prism::bandLayout) >= 0.5f; }
    dyn::prism::Layout layout() const
    {
        const auto* fx = current();
        if (fx != nullptr) return dyn::prism::layoutOf (fx->params);
        EffectParams p {};
        const auto& in = effectInfo (EffectKind::prismMultiband);
        for (std::size_t k = 0; k < in.params.size(); ++k) p[k] = in.params[k].defaultValue;
        return dyn::prism::layoutOf (p);
    }
    float edgeXOfHz (double hz) const
    {
        const auto p = plot();
        if (hz <= 0.0) return p.getX();
        return juce::jlimit (p.getX(), p.getRight(), xForHz (hz, p));
    }
    float bandLoX (int b) const
    {
        if (! isFree()) return edgeX (b);
        const auto L = layout();
        return L.segmentOf[(std::size_t) b] < 0 ? plot().getRight() : edgeXOfHz (L.loHz[(std::size_t) b]);
    }
    float bandHiX (int b) const
    {
        if (! isFree()) return edgeX (b + 1);
        const auto L = layout();
        if (L.segmentOf[(std::size_t) b] < 0) return plot().getRight();
        return L.hiHz[(std::size_t) b] <= 0.0 ? plot().getRight() : edgeXOfHz (L.hiHz[(std::size_t) b]);
    }
    // every per-band parameter of band b (fields, look-ahead, pan)
    static std::vector<int> bandParams (int b)
    {
        using namespace dyn::prism;
        std::vector<int> out;
        for (int f = 0; f < fieldsPerBand; ++f) out.push_back (bp (b, f));
        out.push_back (bandLookaheadParam (b));
        out.push_back (bandPanParam (b));
        // round 147 / 150: the expert panel, the free layout's edges / slopes and the side-chain input go with the band
        for (const int base : { (int) bandLink0, (int) bandLinkMode0, (int) bandScFree0, (int) bandScLow0, (int) bandScHigh0,
                                (int) bandLo0, (int) bandHi0, (int) bandLoSlope0, (int) bandHiSlope0, (int) bandScInput0 })
            out.push_back (base + b);
        return out;
    }
    // round 150: a new free band around `hz` (an octave wide, inside the gap there), the defaults of a band
    void addFreeBand (double hz)
    {
        using namespace dyn::prism;
        const auto n = nb();
        if (n >= bands) return;
        const auto L = layout();
        double lo = hz / std::sqrt (2.0), hi = hz * std::sqrt (2.0);
        for (int b = 0; b < n; ++b) // stay inside the gap
        {
            if (L.segmentOf[(std::size_t) b] < 0) continue;
            const auto blo = L.loHz[(std::size_t) b], bhi = L.hiHz[(std::size_t) b] > 0.0 ? L.hiHz[(std::size_t) b] : 30000.0;
            if (bhi <= hz) lo = std::max (lo, bhi);
            if (blo >= hz) hi = std::min (hi, blo);
        }
        if (hi < lo * 1.1) return;
        std::vector<std::pair<int, float>> c;
        const auto& in = effectInfo (kind);
        for (const auto k : bandParams (n)) c.push_back ({ k, in.params[(std::size_t) k].defaultValue });
        c.push_back ({ bandLo0 + n, edgeValue (lo) });
        c.push_back ({ bandHi0 + n, edgeValue (hi) });
        c.push_back ({ bp (n, fEnabled), 1.0f });
        c.push_back ({ bandCountParam, bandCountValue (n + 1) });
        set (c);
        selected = n;
    }
    // round 150: switching the layout keeps the sound — the free bands take the split's crossovers and slopes, the
    // split takes the free bands' edges (between two bands: the middle of their gap)
    void setLayout (bool free)
    {
        using namespace dyn::prism;
        if (free == isFree()) return;
        const auto L = layout();
        std::vector<std::pair<int, float>> c;
        const auto n = nb();
        if (free)
        {
            for (int b = 0; b < n; ++b)
            {
                c.push_back ({ bandLo0 + b, edgeValue (L.loHz[(std::size_t) b] > 0.0 ? L.loHz[(std::size_t) b] : 30.0) });
                c.push_back ({ bandHi0 + b, edgeValue (L.hiHz[(std::size_t) b] > 0.0 ? L.hiHz[(std::size_t) b] : 30000.0) });
                c.push_back ({ bandLoSlope0 + b, slopeValue (L.loSlope[(std::size_t) b]) });
                c.push_back ({ bandHiSlope0 + b, slopeValue (L.hiSlope[(std::size_t) b]) });
            }
            c.push_back ({ bandLayout, 1.0f });
        }
        else
        {
            for (int k = 0; k + 1 < n; ++k)
            {
                const auto a = L.hiHz[(std::size_t) k] > 0.0 ? L.hiHz[(std::size_t) k] : 18000.0;
                const auto b = L.loHz[(std::size_t) (k + 1)] > 0.0 ? L.loHz[(std::size_t) (k + 1)] : a;
                c.push_back ({ crossoverParam (k), crossoverValue (std::sqrt (a * b)) });
            }
            c.push_back ({ bandLayout, 0.0f });
        }
        set (c);
    }
    // split band b at `hz`: the bands above move up one, the new band (b + 1) starts as a copy of b
    void splitBand (int b, double hz)
    {
        using namespace dyn::prism;
        const auto n = nb();
        if (n >= bands || b < 0 || b >= n) return;
        const auto lo = b > 0 ? xoHz (b - 1) * 1.08 : 30.0, hi = b < n - 1 ? xoHz (b) / 1.08 : 18000.0;
        if (lo >= hi) return;
        std::vector<std::pair<int, float>> c;
        for (int k = n - 1; k >= b; --k)
        {
            const auto from = bandParams (k), to = bandParams (k + 1);
            for (std::size_t j = 0; j < from.size(); ++j) c.push_back ({ to[j], v (from[j]) });
        }
        for (int k = n - 2; k >= b; --k) c.push_back ({ crossoverParam (k + 1), v (crossoverParam (k)) });
        c.push_back ({ crossoverParam (b), crossoverValue (juce::jlimit (lo, hi, hz)) });
        c.push_back ({ bandCountParam, bandCountValue (n + 1) });
        set (c);
        selected = b + 1;
    }
    // remove band b: the bands above move down one, its neighbour takes its range
    void removeBand (int b)
    {
        using namespace dyn::prism;
        const auto n = nb();
        if (n <= 1 || b < 0 || b >= n) return;
        std::vector<std::pair<int, float>> c;
        for (int k = b; k < n - 1; ++k)
        {
            const auto from = bandParams (k + 1), to = bandParams (k);
            for (std::size_t j = 0; j < from.size(); ++j) c.push_back ({ to[j], v (from[j]) });
        }
        const auto gone = b < n - 1 ? b : b - 1; // the crossover that disappears
        for (int k = gone; k < n - 2; ++k) c.push_back ({ crossoverParam (k), v (crossoverParam (k + 1)) });
        c.push_back ({ bandCountParam, bandCountValue (n - 1) });
        set (c);
        selected = juce::jmin (b, n - 2);
    }
    float gainY (double db) const
    {
        const auto p = plot();
        return p.getY() + p.getHeight() * 0.30f - p.getHeight() * 0.25f * juce::jlimit (-1.15f, 1.15f, (float) (db / rangeView));
    }
    double dbAtY (float y) const
    {
        const auto p = plot();
        return (p.getY() + p.getHeight() * 0.30f - y) / (p.getHeight() * 0.25f) * rangeView;
    }
    float specY (float db) const
    {
        const auto p = plot();
        return p.getY() + p.getHeight() * juce::jlimit (0.0f, 1.0f, -db / 100.0f);
    }
    juce::Point<float> node (int b) const
    {
        return { 0.5f * (bandLoX (b) + bandHiX (b)), gainY (dyn::prism::rangeDb (v (bp (b, dyn::prism::fRange)))) };
    }
    int nodeAt (juce::Point<float> p) const
    {
        for (int b = 0; b < nb(); ++b)
            if (node (b).getDistanceFrom (p) < 11.0f) return b;
        return -1;
    }
    int edgeAt (juce::Point<float> p) const
    {
        if (! plot().contains (p)) return -1;
        if (isFree()) // round 150: 2b + 1 = band b's low edge, 2b + 2 its high edge
        {
            for (int b = 0; b < nb(); ++b)
            {
                if (std::abs (bandLoX (b) - p.x) < 5.0f && bandLoX (b) > plot().getX() + 1.0f) return 2 * b + 1;
                if (std::abs (bandHiX (b) - p.x) < 5.0f && bandHiX (b) < plot().getRight() - 1.0f) return 2 * b + 2;
            }
            return -1;
        }
        for (int i = 1; i < nb(); ++i)
            if (std::abs (edgeX (i) - p.x) < 5.0f) return i;
        return -1;
    }
    int bandAtX (float x) const // round 150: −1 in a gap of the free layout
    {
        if (isFree())
        {
            for (int b = 0; b < nb(); ++b)
                if (x >= bandLoX (b) && x < bandHiX (b)) return b;
            return -1;
        }
        for (int b = 0; b < nb(); ++b)
            if (x < edgeX (b + 1)) return b;
        return nb() - 1;
    }

    double liveDb (int b) const
    {
        if (demo) return b == 1 ? -4.5 : b == 2 ? -2.0 : b == 3 ? 1.5 : -1.0;
        return bandOn (b) ? (double) liveMeter (dyn::prism::bandMeter (b)) * 30.0 : 0.0;
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        using namespace dyn::prism;
        const auto p = plot();
        for (int b = 0; b < nb(); ++b)
        {
            const auto x = bandLoX (b) + 4.0f;
            if (bandHiX (b) - bandLoX (b) < 40.0f) continue;
            out.push_back (button ({ x, p.getY() + 4.0f, 18.0f, 16.0f }, idSolo0 + b, "S", v (bp (b, fSolo)) >= 0.5f, tr ("Solo the band")));
            out.push_back (button ({ x, p.getY() + 22.0f, 18.0f, 16.0f }, idMute0 + b, "M", ! bandOn (b), tr ("Bypass the band (the band passes untouched)")));
        }
        out.push_back (button ({ p.getRight() + 8.0f, p.getY() + 2.0f, 50.0f, 20.0f }, idRange, juce::String ((int) rangeView) + " dB", false,
                               tr ("Gain scale: click to switch between 3, 6, 12 and 30 dB")));
        if (selected >= 0)
        {
            const auto a = panelArea();
            const auto b = selected;
            auto add = [&] (Ctl c) { c.colour = peqview::bandColour (b); out.push_back (c); };
            add (toggle ({ a.getX() + 12.0f, a.getY() + 10.0f, 64.0f, 22.0f }, bp (b, fEnabled), tr ("On")));
            add (toggle ({ a.getX() + 12.0f, a.getY() + 38.0f, 64.0f, 22.0f }, bp (b, fSolo), tr ("Solo")));
            add (knob ({ a.getX() + 86.0f, a.getY() + 8.0f, 88.0f, 104.0f }, bp (b, fThreshold), tr ("Threshold"), 2));
            add (knob ({ a.getX() + 182.0f, a.getY() + 8.0f, 88.0f, 104.0f }, bp (b, fRange), tr ("Range"), 2, true));
            add (knob ({ a.getX() + 286.0f, a.getY() + 14.0f, 70.0f, 90.0f }, bp (b, fAttack), tr ("Attack")));
            add (knob ({ a.getX() + 362.0f, a.getY() + 14.0f, 70.0f, 90.0f }, bp (b, fRelease), tr ("Release")));
            add (knob ({ a.getX() + 446.0f, a.getY() + 14.0f, 70.0f, 90.0f }, bp (b, fLevel), tr ("Output"), 1, true));
            add (tabs ({ a.getX() + 86.0f, a.getY() + 128.0f, 184.0f, 26.0f }, bp (b, fMode), "Compress|Expand"));
            add (knob ({ a.getX() + 290.0f, a.getY() + 112.0f, 66.0f, 54.0f }, bp (b, fRatio), tr ("Ratio"), 0));
            add (knob ({ a.getX() + 366.0f, a.getY() + 112.0f, 66.0f, 54.0f }, bp (b, fKnee), tr ("Knee"), 0));
            add (knob ({ a.getX() + 446.0f, a.getY() + 112.0f, 66.0f, 54.0f }, bandLookaheadParam (b), tr ("Lookahead"), 0));
            add (knob ({ a.getX() + 522.0f, a.getY() + 112.0f, 66.0f, 54.0f }, bandPanParam (b), tr ("Pan"), 0, true)); // round 135: the band's output balance
            // round 144: the crossover slope (one for every band) and the side-chain input, in the band panel
            out.push_back (menu ({ a.getX() + 522.0f, a.getY() + 36.0f, 70.0f, 22.0f }, slope, tr ("Slope"), "12 dB|24 dB|48 dB"));
            out.push_back (menu ({ a.getX() + 522.0f, a.getY() + 76.0f, 70.0f, 22.0f }, scExternal, tr ("Side chain"), "Int|Ext"));
            out.push_back (tabs ({ a.getX() + 12.0f, a.getY() + 72.0f, 64.0f, 22.0f }, panMode, "L/R|M/S")); // round 144: what PAN balances
            // round 147: the expert part (the reference's »): side-chain filtering and the stereo link of this band
            out.push_back (button ({ a.getX() + 12.0f, a.getY() + 104.0f, 64.0f, 22.0f }, idExpert, expert ? tr ("Expert <<") : tr ("Expert >>"), expert,
                                   tr ("Show the band's expert settings: side-chain filtering (band or a free range) and the stereo link")));
            if (expert)
            {
                const auto x0 = a.getX() + 600.0f;
                add (tabs ({ x0 + 10.0f, a.getY() + 26.0f, 100.0f, 22.0f }, bandScFree0 + b, "Band|Free"));
                add (knob ({ x0 + 4.0f, a.getY() + 52.0f, 56.0f, 66.0f }, bandScLow0 + b, tr ("Low"), 0));
                add (knob ({ x0 + 58.0f, a.getY() + 52.0f, 56.0f, 66.0f }, bandScHigh0 + b, tr ("High"), 0));
                // the band's own slopes (Pro-MB's per-band slopes): the free layout's continuous 6 … 48 dB/oct, or the
                // split layout's lower / upper crossover's slope (or the global one)
                if (isFree())
                {
                    add (menu ({ x0 + 4.0f, a.getY() + 122.0f, 54.0f, 20.0f }, bandLoSlope0 + b, tr ("Low slope"), "6|12|18|24|30|36|42|48"));
                    add (menu ({ x0 + 60.0f, a.getY() + 122.0f, 54.0f, 20.0f }, bandHiSlope0 + b, tr ("High slope"), "6|12|18|24|30|36|42|48"));
                }
                else
                {
                    if (b > 0)
                        add (menu ({ x0 + 4.0f, a.getY() + 122.0f, 54.0f, 20.0f }, xoverSlope0 + b - 1, tr ("Low slope"), "Global|12|24|48"));
                    if (b < nb() - 1)
                        add (menu ({ x0 + 60.0f, a.getY() + 122.0f, 54.0f, 20.0f }, xoverSlope0 + b, tr ("High slope"), "Global|12|24|48"));
                }
                add (knob ({ x0 + 126.0f, a.getY() + 22.0f, 84.0f, 92.0f }, bandLink0 + b, tr ("Link"), 1));
                add (tabs ({ x0 + 132.0f, a.getY() + 122.0f, 74.0f, 22.0f }, bandLinkMode0 + b, "Mid|Side"));
                // round 150: the band's own side-chain input (the reference's per band choice)
                add (menu ({ x0 + 4.0f, a.getY() + 144.0f, 110.0f, 20.0f }, bandScInput0 + b, tr ("Input"), "Global|Plug-in|External"));
            }
            out.push_back (button ({ a.getRight() - 72.0f, a.getY() + 8.0f, 20.0f, 20.0f }, idPrev, "<", false, tr ("Previous / next band")));
            out.push_back (button ({ a.getRight() - 50.0f, a.getY() + 8.0f, 20.0f, 20.0f }, idNext, ">", false, tr ("Previous / next band")));
            out.push_back (button ({ a.getRight() - 26.0f, a.getY() + 8.0f, 20.0f, 20.0f }, idClose, "x", false, tr ("Close the band panel (click on the empty display does the same)")));
        }
        const auto bar = barArea();
        const auto cy = bar.getCentreY();
        static const char* const analyzerNames[] { TR_NOOP ("Off"), TR_NOOP ("Pre"), TR_NOOP ("Post"), TR_NOOP ("Pre + Post") };
        // round 144: the reference's bottom bar order — processing mode, oversampling, look-ahead, analyser; the
        // crossover slope and the side-chain input moved into the band panel (where the reference has them)
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 150.0f, 22.0f }, processingMode, tr ("Phase"), "Linear|Dynamic|Minimum"));
        out.push_back (barMenu ({ 166.0f, cy - 11.0f, 134.0f, 22.0f }, oversampling, tr ("Oversampling"), "Off|2x|4x"));
        out.push_back (barValue ({ 306.0f, cy - 11.0f, 124.0f, 22.0f }, lookahead, tr ("Lookahead")));
        out.push_back (button ({ 436.0f, cy - 11.0f, 150.0f, 22.0f }, idAnalyzer, tr ("Analyzer:") + " " + tr (analyzerNames[analyzer]) + (specFrozen ? " *" : ""), false,
                               tr ("Spectrum analyzer: off, pre, post or pre + post; speed; freeze")));
        // round 147: a project saved before the measured Dynamic phase keeps its classic engine; one click updates it
        // round 150: the layout (split = contiguous bands on crossovers; free = Pro-MB's bands anywhere, the gaps pass
        // untouched) and the old-project indicators, in the column right of the display (the bottom bar is full)
        {
            const auto x = p.getRight() + 6.0f;
            out.push_back (button ({ x, p.getY() + 28.0f, 30.0f, 20.0f }, idSplit, tr ("Split"), ! isFree(),
                                   tr ("Split layout: the bands sit side by side on crossovers")));
            out.push_back (button ({ x + 32.0f, p.getY() + 28.0f, 30.0f, 20.0f }, idFree, tr ("Free"), isFree(),
                                   tr ("Free layout (as Pro-MB): each band has its own edges and slopes; the room between bands passes untouched")));
            auto y = p.getY() + 54.0f;
            // round 147: a project saved before the measured Dynamic phase keeps its classic engine; one click updates it
            if (v (dynamicEngine) < 0.5f && phaseOf (v (processingMode)) == Phase::dynamic)
            {
                out.push_back (button ({ x, y, 62.0f, 20.0f }, idClassic, tr ("Classic"), true,
                                       tr ("This project's Dynamic phase runs the engine it was saved with (before round 147). Click to use the measured one (Pro-MB's response).")));
                y += 24.0f;
            }
            if (v (detectorLaw) < 0.5f || v (rev152) < 0.5f)
                out.push_back (button ({ x, y, 62.0f, 20.0f }, idClassicDet, tr ("Old det."), true,
                                       tr ("This project's bands use the detector they were saved with (before round 152). Click to use the measured one (Pro-MB's attack, release and range).")));
        }
        out.push_back (barValue ({ bar.getRight() - 312.0f, cy - 11.0f, 92.0f, 22.0f }, mix, tr ("Mix")));
        out.push_back (barValue ({ bar.getRight() - 216.0f, cy - 11.0f, 100.0f, 22.0f }, input, tr ("In")));
        out.push_back (barValue ({ bar.getRight() - 112.0f, cy - 11.0f, 104.0f, 22.0f }, output, tr ("Out")));
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        using namespace dyn::prism;
        if (c.id == idPrev) selected = (selected + nb() - 1) % nb();
        else if (c.id == idNext) selected = (selected + 1) % nb();
        else if (c.id == idClose) selected = -1;
        else if (c.id == idRange) rangeView = rangeView >= 30.0 ? 3.0 : rangeView >= 12.0 ? 30.0 : rangeView >= 6.0 ? 12.0 : 6.0;
        else if (c.id == idAnalyzer) { showAnalyzerMenu(); return; }
        else if (c.id == idClassic) set ({ { dynamicEngine, 1.0f } });
        else if (c.id == idClassicDet) set ({ { detectorLaw, 1.0f }, { rev152, 1.0f } });
        else if (c.id == idSplit || c.id == idFree) setLayout (c.id == idFree);
        else if (c.id == idExpert) expert = ! expert;
        else if (c.id >= idSolo0 && c.id < idSolo0 + bands) set ({ { bp (c.id - idSolo0, fSolo), v (bp (c.id - idSolo0, fSolo)) >= 0.5f ? 0.0f : 1.0f } });
        else if (c.id >= idMute0 && c.id < idMute0 + bands) set ({ { bp (c.id - idMute0, fEnabled), bandOn (c.id - idMute0) ? 0.0f : 1.0f } });
        repaint();
    }

    // the analyser's options, as the reference's analyser menu: what it shows, its speed, freeze
    void showAnalyzerMenu()
    {
        static const char* const names[] { TR_NOOP ("Off"), TR_NOOP ("Pre"), TR_NOOP ("Post"), TR_NOOP ("Pre + Post") };
        static const char* const speeds[] { TR_NOOP ("Very slow"), TR_NOOP ("Slow"), TR_NOOP ("Medium"), TR_NOOP ("Fast"), TR_NOOP ("Very fast") };
        static const float falls[] { 0.3f, 0.75f, 1.5f, 3.0f, 6.0f };
        juce::PopupMenu m;
        for (int k = 0; k < 4; ++k)
            m.addItem (tr (names[k]), true, analyzer == k, [this, k] { analyzer = k; repaint(); });
        m.addSeparator();
        juce::PopupMenu sp;
        for (int k = 0; k < 5; ++k)
            sp.addItem (tr (speeds[k]), true, specFall == falls[k], [this, k] { specFall = falls[k]; });
        m.addSubMenu (tr ("Speed"), sp);
        // round 144: the reference's resolution and tilt choices
        static const char* const resolutions[] { TR_NOOP ("Low"), TR_NOOP ("Medium"), TR_NOOP ("High"), TR_NOOP ("Maximum") };
        juce::PopupMenu res;
        for (int k = 0; k < 4; ++k)
            res.addItem (tr (resolutions[k]), true, specResolution == k, [this, k] { specResolution = k; });
        m.addSubMenu (tr ("Resolution"), res);
        juce::PopupMenu tilt;
        for (const double t : { 0.0, 1.5, 3.0, 4.5, 6.0 })
            tilt.addItem (juce::String (t, 1) + " dB/oct", true, specTilt == t, [this, t] { specTilt = t; });
        m.addSubMenu (tr ("Tilt"), tilt);
        m.addItem (tr ("Freeze"), true, specFrozen, [this] { specFrozen = ! specFrozen; repaint(); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition()); // round 152 (checked by hand): at the mouse, not under the window
    }

    void showBandMenu (int b)
    {
        using namespace dyn::prism;
        juce::PopupMenu m;
        m.addItem (tr ("On"), true, bandOn (b), [this, b] { set ({ { bp (b, fEnabled), bandOn (b) ? 0.0f : 1.0f } }); });
        m.addItem (tr ("Solo"), true, v (bp (b, fSolo)) >= 0.5f, [this, b] { set ({ { bp (b, fSolo), v (bp (b, fSolo)) >= 0.5f ? 0.0f : 1.0f } }); });
        m.addSeparator();
        m.addItem (tr ("Compress"), true, v (bp (b, fMode)) < 0.5f, [this, b] { set ({ { bp (b, fMode), 0.0f } }); });
        m.addItem (tr ("Expand"), true, v (bp (b, fMode)) >= 0.5f, [this, b] { set ({ { bp (b, fMode), 1.0f } }); });
        m.addSeparator();
        m.addItem (tr ("Reset the band"), [this, b] {
            std::vector<std::pair<int, float>> c;
            const auto& in = effectInfo (kind);
            for (const auto k : bandParams (b)) c.push_back ({ k, in.params[(std::size_t) k].defaultValue });
            set (c);
        });
        m.addSeparator();
        if (! isFree())
            m.addItem (tr ("Split the band"), nb() < bands, false, [this, b] { splitBand (b, std::sqrt ((b > 0 ? xoHz (b - 1) : 30.0) * (b < nb() - 1 ? xoHz (b) : 18000.0))); });
        m.addItem (tr ("Remove the band"), nb() > 1, false, [this, b] { removeBand (b); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition()); // round 152 (checked by hand): at the mouse, not under the window
    }

    bool bodyDown (const juce::MouseEvent& e) override
    {
        using namespace dyn::prism;
        const auto p = e.position;
        if (selected >= 0 && panelArea().contains (p)) return false;
        if (! plot().contains (p)) return false;
        dragBand = dragEdge = -1;
        if (const auto b = nodeAt (p); b >= 0)
        {
            selected = b;
            if (e.mods.isPopupMenu()) { showBandMenu (b); return false; }
            if (e.mods.isAltDown()) { set ({ { bp (b, fEnabled), 0.0f } }); return false; }
            dragBand = b;
            dragStart = p;
            for (int i = 0; i < nb() - 1; ++i) dragX0[i] = edgeX (i + 1);
            dragLo0 = bandLoX (b);
            dragHi0 = bandHiX (b);
            return true;
        }
        if (const auto i = edgeAt (p); i > 0 && ! e.mods.isPopupMenu())
        {
            dragEdge = i;
            return true;
        }
        const auto b = bandAtX (p.x);
        if (b < 0) // round 150: a gap of the free layout
        {
            if (e.mods.isPopupMenu() && nb() < bands)
            {
                const auto hz = hzForX (p.x, plot());
                juce::PopupMenu m;
                m.addItem (tr ("New band here"), [this, hz] { addFreeBand (hz); });
                m.showMenuAsync (juce::PopupMenu::Options().withMousePosition()); // round 152 (checked by hand): at the mouse, not under the window
                return false;
            }
            selected = -1;
            repaint();
            return false;
        }
        if (e.mods.isPopupMenu()) { showBandMenu (b); return false; }
        selected = selected == b ? -1 : b; // a click on a band opens its panel, a second click closes it
        repaint();
        return false;
    }

    void moveEdge (int i, float x, std::vector<std::pair<int, float>>& c) const
    {
        using namespace dyn::prism;
        if (isFree()) // round 150: one band's own edge (2b + 1 low, 2b + 2 high), kept inside the display and the band
        {
            const auto b = (i - 1) / 2;
            const bool high = (i - 1) % 2 != 0;
            const auto pl = plot();
            const auto xx = high ? juce::jlimit (bandLoX (b) + 12.0f, pl.getRight() - 2.0f, x) : juce::jlimit (pl.getX() + 2.0f, bandHiX (b) - 12.0f, x);
            c.push_back ({ (high ? bandHi0 : bandLo0) + b, edgeValue (juce::jlimit (30.0, 30000.0, hzForX (xx, pl))) });
            return;
        }
        const auto lo = i > 1 ? edgeX (i - 1) + 12.0f : plot().getX() + 4.0f;
        const auto hi = i < nb() - 1 ? edgeX (i + 1) - 12.0f : plot().getRight() - 4.0f;
        const auto hz = hzForX (juce::jlimit (lo, hi, x), plot());
        c.push_back ({ crossoverParam (i - 1), crossoverValue (juce::jlimit (30.0, 18000.0, hz)) });
    }

    void bodyDrag (const juce::MouseEvent& e) override
    {
        using namespace dyn::prism;
        std::vector<std::pair<int, float>> c;
        if (dragEdge > 0)
        {
            moveEdge (dragEdge, e.position.x, c);
            set (c);
            const auto prm = isFree() ? ((dragEdge - 1) % 2 != 0 ? bandHi0 : bandLo0) + (dragEdge - 1) / 2 : crossoverParam (dragEdge - 1);
            ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (e.position.toInt()), tr ("Crossover") + " " + text (prm));
            return;
        }
        if (dragBand < 0) return;
        const auto b = dragBand;
        const auto d = e.position - dragStart;
        const auto fine = e.mods.isCtrlDown() ? 0.25f : 1.0f;
        if (std::abs (d.y) > 2.0f || ! e.mods.isShiftDown())
            c.push_back ({ bp (b, fRange), (float) ((juce::jlimit (-30.0, 30.0, dbAtY (dragStart.y + d.y * fine)) + 30.0) / 60.0) });
        if (std::abs (d.x) > 3.0f && ! (e.mods.isShiftDown() && std::abs (d.y) > std::abs (d.x)))
        {
            // move the band: its edges follow the mouse (the outer bands move one edge)
            if (isFree()) // round 150: the free band's own edges (an edge at the display's end stays there)
            {
                const auto pl = plot();
                if (dragLo0 > pl.getX() + 1.0f)
                    c.push_back ({ bandLo0 + b, edgeValue (juce::jlimit (30.0, 30000.0, hzForX (dragLo0 + d.x * fine, pl))) });
                if (dragHi0 < pl.getRight() - 1.0f)
                    c.push_back ({ bandHi0 + b, edgeValue (juce::jlimit (30.0, 30000.0, hzForX (dragHi0 + d.x * fine, pl))) });
            }
            else if (b > 0) c.push_back ({ crossoverParam (b - 1), crossoverValue (juce::jlimit (30.0, 18000.0, hzForX (dragX0[b - 1] + d.x * fine, plot()))) });
            if (b < nb() - 1) c.push_back ({ crossoverParam (b), crossoverValue (juce::jlimit (30.0, 18000.0, hzForX (dragX0[b] + d.x * fine, plot()))) });
        }
        set (c);
        ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (node (b).toInt()), tr ("Band {0}", { juce::String (b + 1) }) + "  " + tr ("Range") + " " + text (bp (b, fRange)));
    }

    bool bodyDoubleClick (const juce::MouseEvent& e) override
    {
        using namespace dyn::prism;
        if (! plot().contains (e.position)) return false;
        // round 135: double-click on a node = on / off; on the empty display = a new band there (the band under the
        // mouse splits at that frequency), as the reference creates bands anywhere
        if (isFree()) // round 150: on a node = on / off; in a gap = a new band there (up to six)
        {
            if (const auto nodeB = nodeAt (e.position); nodeB >= 0)
            {
                set ({ { bp (nodeB, fEnabled), bandOn (nodeB) ? 0.0f : 1.0f } });
                selected = nodeB;
            }
            else if (bandAtX (e.position.x) < 0)
                addFreeBand (hzForX (e.position.x, plot()));
            return true;
        }
        if (const auto nodeB = nodeAt (e.position); nodeB >= 0 || nb() >= bands)
        {
            const auto b = nodeB >= 0 ? nodeB : bandAtX (e.position.x);
            set ({ { bp (b, fEnabled), bandOn (b) ? 0.0f : 1.0f } });
            selected = b;
            return true;
        }
        splitBand (bandAtX (e.position.x), hzForX (e.position.x, plot()));
        return true;
    }

    bool bodyWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        using namespace dyn::prism;
        if (! plot().contains (e.position)) return false;
        auto b = nodeAt (e.position);
        if (b < 0) b = selected;
        if (b < 0) return true;
        const auto prm = bp (b, fThreshold);
        if (const auto x = ui::wheelValue (v (prm), e, w, specFor (prm)))
        {
            set ({ { prm, (float) *x } });
            ui::flashValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (node (b).toInt()), tr ("Threshold") + " " + text (prm));
        }
        return true;
    }

    void bodyMove (juce::Point<float> p) override { hoverBand = nodeAt (p); }

    ui::Hint bodyHint (juce::Point<float> p) override
    {
        ui::Hint h;
        if (! plot().contains (p)) return h;
        if (nodeAt (p) >= 0)
            h.text = tr ("Band node: drag up / down = range, left / right = move the band (Shift: one direction, Ctrl: fine); wheel = threshold; Alt-click = bypass; right-click = menu");
        else if (edgeAt (p) > 0)
            h.text = tr ("Crossover: drag left or right");
        else if (isFree() && bandAtX (p.x) < 0)
            h.text = tr ("Free layout: this range passes untouched; double-click = a new band here (up to six)");
        else
            h.text = tr ("Click a band to open its panel; double-click = a new band here (up to six); right-click = band menu");
        h.hasMenu = true;
        return h;
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        using namespace dyn::prism;
        if (selected >= nb()) selected = nb() - 1; // a band removed (or a smaller preset): keep the panel on a band
        const auto p = plot();
        displayBackground (g, bodyArea());
        frequencyLabels (g, p, p.getBottom() + 12.0f);
        // the analyser
        if (analyzer == 1 || analyzer == 3)
            paintSpectrum (g, specPre, [this] (float db) { return specY (db); }, inColour().withAlpha (0.18f), inColour().withAlpha (0.4f));
        if (analyzer >= 2)
            paintSpectrum (g, specPost, [this] (float db) { return specY (db); }, outColour().withAlpha (0.12f), outColour().withAlpha (0.5f));
        // gain scale labels (right) and the 0 dB line
        g.setFont (font (type::caption));
        for (const auto f : { -1.0, -0.5, 0.0, 0.5, 1.0 })
        {
            const auto db = f * rangeView;
            const auto y = gainY (db);
            g.setColour (f == 0.0 ? colour::gridBar : colour::gridBeat);
            g.drawHorizontalLine ((int) y, p.getX(), p.getRight());
            g.setColour (colour::warning.withAlpha (0.8f));
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), juce::Rectangle<float> (p.getRight() + 8.0f, y - 7.0f, 30.0f, 14.0f), juce::Justification::centredLeft, false);
        }
        for (const auto db : { -20.0f, -40.0f, -60.0f, -80.0f })
        {
            g.setColour (colour::textDisabled);
            g.drawText (juce::String ((int) db), juce::Rectangle<float> (p.getRight() + 40.0f, specY (db) - 7.0f, 30.0f, 14.0f), juce::Justification::centredLeft, false);
        }
        // the bands
        double gains[bands] {};
        for (int b = 0; b < nb(); ++b)
        {
            const auto c = peqview::bandColour (b);
            const auto region = juce::Rectangle<float> (bandLoX (b), p.getY(), juce::jmax (0.0f, bandHiX (b) - bandLoX (b)), p.getHeight());
            const auto on = bandOn (b);
            g.setColour (c.withAlpha (on ? (b == selected ? 0.16f : 0.08f) : 0.02f));
            g.fillRect (region);
            gains[b] = on ? liveDb (b) + levelDb (v (bp (b, fLevel))) : 0.0;
            if (on)
            {
                // the range the band may move (dashed) and its live gain change (filled)
                const auto ry = gainY (rangeDb (v (bp (b, fRange))));
                const auto dashes = std::array<float, 2> { 4.0f, 4.0f };
                g.setColour (c.withAlpha (0.6f));
                g.drawDashedLine (juce::Line<float> (region.getX(), ry, region.getRight(), ry), dashes.data(), 2, 1.0f);
                const auto ly = gainY (liveDb (b));
                g.setColour (c.withAlpha (0.35f));
                g.fillRect (region.withTop (juce::jmin (ly, gainY (0.0))).withBottom (juce::jmax (ly, gainY (0.0))));
            }
        }
        const auto edgeLine = [&] (float x) {
            g.setColour (std::abs (mouse.x - x) < 5.0f && p.contains (mouse) ? colour::textPrimary : colour::textSecondary.withAlpha (0.5f));
            const auto dashes = std::array<float, 2> { 3.0f, 3.0f };
            g.drawDashedLine (juce::Line<float> (x, p.getY(), x, p.getBottom()), dashes.data(), 2, 1.0f);
        };
        if (isFree()) // round 150: each band's own edges
        {
            for (int b = 0; b < nb(); ++b)
            {
                if (bandLoX (b) > p.getX() + 1.0f) edgeLine (bandLoX (b));
                if (bandHiX (b) < p.getRight() - 1.0f) edgeLine (bandHiX (b));
            }
        }
        else
            for (int i = 1; i < nb(); ++i)
                edgeLine (edgeX (i));
        // the summed response with the live band gains
        juce::Path resp;
        for (int i = 0; i <= 300; ++i)
        {
            const auto x = p.getX() + p.getWidth() * (float) i / 300.0f;
            const auto db = responseDb (fx.params, gains, 48000.0, hzForX (x, p));
            if (i == 0) resp.startNewSubPath (x, gainY (db));
            else resp.lineTo (x, gainY (db));
        }
        skin::glowStroke (g, resp, fx.enabled ? colour::warning : colour::textDisabled, 2.0f, fx.enabled ? 1.0f : 0.0f);
        // the nodes
        for (int b = 0; b < nb(); ++b)
        {
            const auto n = node (b);
            const auto c = peqview::bandColour (b);
            const auto r = juce::Rectangle<float> (b == hoverBand || b == selected ? 16.0f : 13.0f, b == hoverBand || b == selected ? 16.0f : 13.0f).withCentre (n);
            g.setColour (bandOn (b) ? c : c.withAlpha (0.3f));
            g.fillEllipse (r);
            g.setColour (b == selected ? colour::textPrimary : colour::bg0);
            g.drawEllipse (r, 1.5f);
        }
        // the hovered node's read-out
        if (hoverBand >= 0 && dragCtl == -1)
        {
            const auto n = node (hoverBand);
            auto r = juce::Rectangle<float> (n.x - 54.0f, n.y + 14.0f, 108.0f, 48.0f).constrainedWithin (p);
            // round 152 (checked by hand): the open band panel hid the read-out under the node; then it goes above
            if (selected >= 0 && r.intersects (panelArea()))
                r = juce::Rectangle<float> (n.x - 54.0f, n.y - 62.0f, 108.0f, 48.0f).constrainedWithin (p);
            g.setColour (colour::bg3.withAlpha (0.95f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (colour::textPrimary);
            g.setFont (font (type::small, true));
            g.drawText (tr ("Band {0}", { juce::String (hoverBand + 1) }), r.withHeight (16.0f).translated (0.0f, 2.0f), juce::Justification::centred, false);
            g.setFont (font (type::caption));
            g.drawText (tr ("Range") + " " + text (bp (hoverBand, fRange)), r.withTrimmedTop (17.0f).withHeight (14.0f), juce::Justification::centred, false);
            g.drawText (tr ("Threshold") + " " + text (bp (hoverBand, fThreshold)), r.withTrimmedTop (31.0f).withHeight (14.0f), juce::Justification::centred, false);
        }
        // the band panel
        if (selected >= 0)
        {
            const auto a = panelArea();
            const auto c = peqview::bandColour (selected);
            drawPanel (g, a, c);
            g.setColour (c);
            g.fillRoundedRectangle (juce::Rectangle<float> (a.getX() + 14.0f, a.getY() - 1.0f, 46.0f, 3.0f), 1.5f);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::small, true));
            g.drawText (tr ("Band {0}", { juce::String (selected + 1) }), juce::Rectangle<float> (a.getRight() - 160.0f, a.getY() + 8.0f, 86.0f, 20.0f), juce::Justification::centredRight, false);
            caption (g, { a.getX() + 86.0f, a.getY() + 114.0f, 184.0f, 12.0f }, tr ("Mode"));
            if (expert) // round 147: the expert part's captions and its separator
            {
                const auto x0 = a.getX() + 600.0f;
                g.setColour (colour::border);
                g.drawVerticalLine ((int) x0, a.getY() + 10.0f, a.getBottom() - 10.0f);
                caption (g, { x0 + 6.0f, a.getY() + 10.0f, 110.0f, 12.0f }, tr ("Side chain"));
                caption (g, { x0 + 120.0f, a.getY() + 10.0f, 96.0f, 12.0f }, tr ("Stereo link"));
            }
        }
        // output meter
        const auto m = juce::Rectangle<float> ((float) getWidth() - 14.0f, p.getY(), 8.0f, p.getHeight());
        drawMeter (g, m, dbOf (liveMeter (5)), -60.0f, colour::meterLow.withAlpha (0.85f));
    }
};

// =============================================================================================================
// Ember Saturator — the reference saturator's workflow: the spectrum on top with the bands side by side (drag a
// crossover line to move it, drag a band's handle up / down for its drive, double-click to split a band in two),
// the selected band's panel floating below (style, mix, the big DRIVE, the tone faders, level, remove), global
// settings in the bottom bar.
class EmberEditor final : public ProEditor
{
public:
    EmberEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::ember, d, 960, 540)
    {
        specArea = disp();
        specMinHz = 20.0;
        specMaxHz = 20000.0;
        fillDemo();
    }

private:
    enum { idRemove = 1, idPrev, idNext, idPower, idSolo, idMute, idBands, idMod };
    int selected = 0, dragHandle = -1, dragEdge = -1, hoverHandle = -1;
    bool showMod = false; // round 152: the panel shows the modulation (LFOs, follower, four slots) instead of the band
    int xp (int b, int f) const { return mixsuite::ember::bandExtra (b, f); } // round 128: a band's power, state, tone, feedback, dynamics
    bool powered (int b) const { return v (xp (b, 0)) >= 0.5f; }
    mixsuite::ember::BandState state (int b) const { return mixsuite::ember::bandState (v (xp (b, 1))); }

    juce::String title() const override { return "Ember Saturator"; }
    juce::String gestures() const override { return tr ("Double-click: split a band  -  drag a band's handle: drive  -  drag a line: crossover"); }
    bool usesSpectrum() const override { return true; }

    juce::Rectangle<float> disp() const { return bodyArea().withTrimmedBottom (24.0f).withTrimmedTop (4.0f); }
    juce::Rectangle<float> panelArea() const
    {
        const auto d = disp();
        return { d.getCentreX() - 420.0f, d.getBottom() - 176.0f, 840.0f, 168.0f };
    }
    int count() const
    {
        // round 150: up to six bands (Bands + More bands)
        const auto more = juce::jlimit (0, 2, juce::roundToInt (v (mixsuite::ember::bandsMore) * 2.0f));
        return more > 0 ? mixsuite::ember::firstBands + more : mixsuite::ember::bandCount (v (mixsuite::ember::bands));
    }
    int chr (int b) const { return mixsuite::ember::characterIndex (v (mixsuite::ember::characterParam (b))); }
    // round 135: the Decapitator-role strip along the top of the display (voice row, PUNISH, THUMP, STEEP, the wide
    // filter ranges) and the in / out meter at its right; the band handles start under it
    static constexpr float handleTop = 64.0f;
    juce::Rectangle<float> stripArea() const { const auto d = disp(); return { d.getX() + 8.0f, d.getY() + 6.0f, d.getWidth() - 16.0f, 24.0f }; }
    juce::Rectangle<float> vuArea() const { const auto d = disp(); return { d.getRight() - 64.0f, d.getY() + 38.0f, 56.0f, 120.0f }; }
    bool wide() const { return v (mixsuite::ember::wideCuts) >= 0.5f; }
    int fp (int b, int f) const { return mixsuite::ember::bandParam (b, f); }
    double xoHz (int i) const { return mixsuite::ember::crossoverHz (v (mixsuite::ember::crossoverParam (i))); }
    float edgeX (int i) const
    {
        const auto d = disp();
        if (i <= 0) return d.getX();
        if (i >= count()) return d.getRight();
        return xForHz (xoHz (i - 1), d);
    }
    float handleY (int b) const
    {
        const auto d = disp();
        return d.getY() + handleTop + (panelArea().getY() - 30.0f - d.getY() - handleTop) * (1.0f - v (fp (b, 1)));
    }
    juce::Rectangle<float> handle (int b) const { return juce::Rectangle<float> (38.0f, 16.0f).withCentre ({ 0.5f * (edgeX (b) + edgeX (b + 1)), handleY (b) }); }
    int handleAt (juce::Point<float> p) const
    {
        for (int b = 0; b < count(); ++b)
            if (handle (b).expanded (4.0f).contains (p)) return b;
        return -1;
    }
    int edgeAt (juce::Point<float> p) const
    {
        for (int i = 1; i < count(); ++i)
            if (std::abs (edgeX (i) - p.x) < 5.0f) return i;
        return -1;
    }
    int bandAtX (float x) const
    {
        for (int b = 0; b < count(); ++b)
            if (x < edgeX (b + 1)) return b;
        return count() - 1;
    }
    float specY (float db) const { const auto d = disp(); return d.getY() + d.getHeight() * juce::jlimit (0.0f, 1.0f, (-db - 12.0f) / 90.0f); }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        using namespace mixsuite::ember;
        if (selected >= count()) return;
        const auto a = panelArea();
        const auto b = selected;
        auto add = [&] (Ctl c) { c.colour = peqview::bandColour (b); out.push_back (c); };
        // round 152 (Saturn 2's modulation, measured laws): MOD swaps the band panel for the modulation panel
        out.push_back (button ({ a.getRight() - 146.0f, a.getY() + 10.0f, 66.0f, 20.0f }, idMod, tr ("Mod"), showMod,
                               tr ("Modulation: two LFOs and an envelope follower moving up to four targets")));
        if (showMod)
        {
            out.push_back (knob ({ a.getX() + 10.0f, a.getY() + 40.0f, 60.0f, 74.0f }, lfo1Rate, tr ("LFO 1")));
            out.push_back (menu ({ a.getX() + 10.0f, a.getY() + 122.0f, 60.0f, 22.0f }, lfo1Shape, {}, tr ("Sine|Triangle|Square|Ease in|Ease out")));
            out.push_back (knob ({ a.getX() + 76.0f, a.getY() + 40.0f, 60.0f, 74.0f }, lfo2Rate, tr ("LFO 2")));
            out.push_back (menu ({ a.getX() + 76.0f, a.getY() + 122.0f, 60.0f, 22.0f }, lfo2Shape, {}, tr ("Sine|Triangle|Square|Ease in|Ease out")));
            out.push_back (knob ({ a.getX() + 150.0f, a.getY() + 40.0f, 60.0f, 74.0f }, envAttack, tr ("Attack")));
            out.push_back (knob ({ a.getX() + 214.0f, a.getY() + 40.0f, 60.0f, 74.0f }, envRelease, tr ("Release")));
            juce::String targets;
            for (int t = 0; t < modTargets; ++t) targets << (t > 0 ? "|" : "") << tr (modTargetName (t));
            for (int k = 0; k < modSlots; ++k)
            {
                const auto y = a.getY() + 38.0f + 31.0f * (float) k;
                out.push_back (menu ({ a.getX() + 300.0f, y, 96.0f, 24.0f }, mod1Source + 3 * k, {}, tr ("Off|LFO 1|LFO 2|Envelope")));
                out.push_back (menu ({ a.getX() + 402.0f, y, 140.0f, 24.0f }, mod1Target + 3 * k, {}, targets));
                out.push_back (barValue ({ a.getX() + 548.0f, y, 120.0f, 24.0f }, mod1Amount + 3 * k, tr ("Amount")));
            }
            return;
        }
        // round 152: the band's own crossover slope (its upper edge; Global = the bottom bar's Crossover) and the
        // processing mode (linear phase: zero-phase bands, 64 ms latency at 48 kHz)
        if (b < count() - 1)
            add (menu ({ a.getX() + 10.0f, a.getY() + 36.0f, 60.0f, 20.0f }, crossSlope1 + b, {}, tr ("Global") + "|6|12|24|36|48"));
        out.push_back (toggle ({ a.getRight() - 146.0f, a.getY() + 32.0f, 136.0f, 18.0f }, processing, tr ("Linear phase")));
        // Saturn's band panel order (round 128): power, solo / mute, style | MIX, FEEDBACK, FREQ, DYNAMICS | DRIVE |
        // the band's TONE (bass, mid, treble, presence) | LEVEL; the whole effect's auto gain and dry / wet on the right
        out.push_back (button ({ a.getX() + 70.0f, a.getY() + 12.0f, 32.0f, 22.0f }, idPower, tr ("On"), powered (b), tr ("Band on / off: a band switched off passes clean")));
        out.push_back (button ({ a.getX() + 106.0f, a.getY() + 12.0f, 22.0f, 22.0f }, idSolo, "S", state (b) == BandState::solo, tr ("Solo the band")));
        out.push_back (button ({ a.getX() + 130.0f, a.getY() + 12.0f, 22.0f, 22.0f }, idMute, "M", state (b) == BandState::mute, tr ("Mute the band")));
        add (menu ({ a.getX() + 160.0f, a.getY() + 10.0f, 92.0f, 26.0f }, fp (b, 0), {}, "Tube|Tape|Transistor|Fuzz"));
        // round 150: the band's character (the 28 voicings measured on the reference's styles; Off = the Style menu)
        {
            juce::String opts = tr ("Off");
            for (int k = 1; k <= characters; ++k) opts << "|" << tr (characterName (k));
            add (menu ({ a.getX() + 256.0f, a.getY() + 10.0f, 92.0f, 26.0f }, characterParam (b), {}, opts));
        }
        add (knob ({ a.getX() + 10.0f, a.getY() + 60.0f, 66.0f, 92.0f }, fp (b, 2), tr ("Mix")));
        add (knob ({ a.getX() + 78.0f, a.getY() + 60.0f, 66.0f, 92.0f }, xp (b, 6), tr ("Feedback")));
        add (knob ({ a.getX() + 146.0f, a.getY() + 60.0f, 66.0f, 92.0f }, xp (b, 7), tr ("Freq")));
        add (knob ({ a.getX() + 214.0f, a.getY() + 60.0f, 70.0f, 92.0f }, xp (b, 8), tr ("Dynamics"), 1, true));
        add (knob ({ a.getX() + 290.0f, a.getY() + 40.0f, 124.0f, 128.0f }, fp (b, 1), tr ("Drive"), 2));
        // round 147 (Saturn 2): the drive pan beside the drive (the far channel's drive shrinks), the band's pan under
        // its level; both act on left / right or on mid / side (the bottom bar's Channel)
        add (knob ({ a.getX() + 352.0f, a.getY() + 2.0f, 74.0f, 52.0f }, bandDrivePanParam (b), tr ("Drive pan"), 0, true));
        add (fader ({ a.getX() + 432.0f, a.getY() + 14.0f, 42.0f, 148.0f }, xp (b, 2), tr ("Bass"), true));
        add (fader ({ a.getX() + 476.0f, a.getY() + 14.0f, 42.0f, 148.0f }, xp (b, 3), tr ("Mid"), true));
        add (fader ({ a.getX() + 520.0f, a.getY() + 14.0f, 42.0f, 148.0f }, xp (b, 4), tr ("Treble"), true));
        add (fader ({ a.getX() + 564.0f, a.getY() + 14.0f, 42.0f, 148.0f }, xp (b, 5), tr ("Presence"), true));
        add (knob ({ a.getX() + 612.0f, a.getY() + 8.0f, 70.0f, 78.0f }, fp (b, 3), tr ("Level"), 1, true));
        add (knob ({ a.getX() + 612.0f, a.getY() + 88.0f, 70.0f, 78.0f }, bandPanParam (b), tr ("Pan"), 1, true));
        if (count() > 1)
        {
            out.push_back (button ({ a.getRight() - 72.0f, a.getY() + 10.0f, 20.0f, 20.0f }, idPrev, "<", false, tr ("Previous / next band")));
            out.push_back (button ({ a.getRight() - 50.0f, a.getY() + 10.0f, 20.0f, 20.0f }, idNext, ">", false, tr ("Previous / next band")));
            out.push_back (button ({ a.getRight() - 26.0f, a.getY() + 10.0f, 20.0f, 20.0f }, idRemove, "x", false, tr ("Remove the band (its neighbour takes its range)")));
        }
        out.push_back (toggle ({ a.getRight() - 146.0f, a.getY() + 52.0f, 136.0f, 22.0f }, autoGain, tr ("Auto gain")));
        out.push_back (knob ({ a.getRight() - 148.0f, a.getY() + 84.0f, 66.0f, 80.0f }, mix, tr ("Dry / wet"), 0));
        out.push_back (knob ({ a.getRight() - 78.0f, a.getY() + 84.0f, 66.0f, 80.0f }, output, tr ("Output"), 0, true)); // round 135: on the panel
        // round 135: the strip (one click per voicing, the reference's switches)
        const auto s = stripArea();
        out.push_back (tabs ({ s.getX(), s.getY(), 372.0f, s.getHeight() }, voice, tr ("Style menu|Tape pre|Console|Grit|Triode|Pentode"),
                             { 0.0f, 0.2f, 0.4f, 0.6f, 0.8f, 1.0f }));
        out.push_back (toggle ({ s.getX() + 376.0f, s.getY(), 76.0f, s.getHeight() }, punish, tr ("Punish")));
        out.push_back (toggle ({ s.getX() + 456.0f, s.getY(), 70.0f, s.getHeight() }, thump, tr ("Thump")));
        out.push_back (toggle ({ s.getX() + 530.0f, s.getY(), 64.0f, s.getHeight() }, steep, tr ("Steep")));
        out.push_back (toggle ({ s.getX() + 598.0f, s.getY(), 92.0f, s.getHeight() }, wideCuts, tr ("Wide cuts")));
        // round 150 (Saturn 2): the whole effect's input / output pan (left / right, or mid / side with Channel M/S);
        // they end left of the IN / OUT meter
        out.push_back (barValue ({ s.getX() + 698.0f, s.getY(), 86.0f, s.getHeight() }, inputPan, tr ("In pan")));
        out.push_back (barValue ({ s.getX() + 788.0f, s.getY(), 86.0f, s.getHeight() }, outputPan, tr ("Out pan")));
        const auto bar = barArea();
        const auto cy = bar.getCentreY();
        out.push_back (barMenu ({ 10.0f, cy - 11.0f, 150.0f, 22.0f }, xoverSlope, tr ("Crossover"), "12 dB/oct|24 dB/oct|48 dB/oct", { 0.0f, 0.5f, 1.0f }));
        out.push_back (barMenu ({ 166.0f, cy - 11.0f, 130.0f, 22.0f }, quality, tr ("Oversampling"), "2x|4x|8x", { 0.0f, 0.5f, 1.0f }));
        out.push_back (button ({ 302.0f, cy - 11.0f, 90.0f, 22.0f }, idBands, tr ("Bands") + " " + juce::String (count()), false,
                               tr ("Number of bands (1 to 6)")));
        out.push_back (barValue ({ 398.0f, cy - 11.0f, 90.0f, 22.0f }, inputGain, tr ("In")));
        out.push_back (barValue ({ 494.0f, cy - 11.0f, 80.0f, 22.0f }, tone, tr ("Tilt")));
        out.push_back (barValue ({ 580.0f, cy - 11.0f, 110.0f, 22.0f }, wide() ? lowCutWide : lowCut, tr ("Low cut")));
        out.push_back (barValue ({ 696.0f, cy - 11.0f, 120.0f, 22.0f }, wide() ? highCutWide : highCut, tr ("High cut")));
        // round 147 (Saturn 2's channel mode): the bands work on left / right or on mid / side
        out.push_back (barMenu ({ 822.0f, cy - 11.0f, 128.0f, 22.0f }, channelMode, tr ("Channel"), "L/R|M/S", { 0.0f, 1.0f }));
    }

    // band settings as a block (style, drive, mix, level + round 128's power, state, tone, feedback, dynamics) to move
    // bands around when splitting / removing
    static constexpr int blockSize = 4 + mixsuite::ember::extraFields + 3; // round 147: + drive pan, pan; round 150: + character
    int blockParam (int b, int k) const
    {
        if (k == blockSize - 3) return mixsuite::ember::bandDrivePanParam (b);
        if (k == blockSize - 2) return mixsuite::ember::bandPanParam (b);
        if (k == blockSize - 1) return mixsuite::ember::characterParam (b);
        return k < 4 ? fp (b, k) : xp (b, k - 4);
    }
    std::array<float, blockSize> bandValues (int b) const
    {
        std::array<float, blockSize> out {};
        for (int k = 0; k < blockSize; ++k) out[(std::size_t) k] = v (blockParam (b, k));
        return out;
    }

    void splitAt (float x)
    {
        using namespace mixsuite::ember;
        const auto n = count();
        if (n >= maxBands) return;
        const auto hz = juce::jlimit (30.0, 18000.0, hzForX (x, disp()));
        const auto at = bandAtX (x); // the band split in two: band at + 1 is the new upper half
        std::vector<double> xo;
        std::vector<float> sl; // round 152: each crossover's own slope moves with it
        for (int i = 0; i < n - 1; ++i) { xo.push_back (xoHz (i)); sl.push_back (v (crossSlope1 + i)); }
        xo.insert (xo.begin() + at, hz);
        sl.insert (sl.begin() + at, at < (int) sl.size() ? sl[(std::size_t) at] : 0.0f);
        std::vector<std::array<float, blockSize>> vals;
        for (int b = 0; b < n; ++b) vals.push_back (bandValues (b));
        vals.insert (vals.begin() + at + 1, vals[(std::size_t) at]);
        apply (xo, vals, sl);
        selected = at + 1;
    }

    void removeBand (int b)
    {
        const auto n = count();
        if (n <= 1) return;
        std::vector<double> xo;
        std::vector<float> sl;
        for (int i = 0; i < n - 1; ++i) { xo.push_back (xoHz (i)); sl.push_back (v (mixsuite::ember::crossSlope1 + i)); }
        xo.erase (xo.begin() + juce::jmin (b, n - 2)); // the line on its right (the last band: on its left)
        sl.erase (sl.begin() + juce::jmin (b, n - 2));
        std::vector<std::array<float, blockSize>> vals;
        for (int k = 0; k < n; ++k)
            if (k != b) vals.push_back (bandValues (k));
        apply (xo, vals, sl);
        selected = juce::jmin (b, n - 2);
    }

    void apply (const std::vector<double>& xo, const std::vector<std::array<float, blockSize>>& vals, const std::vector<float>& slopes = {})
    {
        using namespace mixsuite::ember;
        std::vector<std::pair<int, float>> c;
        const auto n = (int) vals.size();
        const auto [bv, mv] = bandCountValues (n);
        c.push_back ({ bands, bv });
        c.push_back ({ bandsMore, mv });
        for (int i = 0; i < (int) xo.size(); ++i) c.push_back ({ crossoverParam (i), crossoverValue (xo[(std::size_t) i]) });
        for (int i = 0; i < (int) slopes.size() && i < maxBands - 1; ++i) c.push_back ({ crossSlope1 + i, slopes[(std::size_t) i] });
        for (int b = 0; b < n; ++b)
            for (int f = 0; f < blockSize; ++f) c.push_back ({ blockParam (b, f), vals[(std::size_t) b][(std::size_t) f] });
        set (c);
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        using mixsuite::ember::BandState;
        if (c.id == idRemove) removeBand (selected);
        else if (c.id == idPower) set ({ { xp (selected, 0), powered (selected) ? 0.0f : 1.0f } });
        else if (c.id == idSolo) set ({ { xp (selected, 1), state (selected) == BandState::solo ? 0.0f : 0.5f } });
        else if (c.id == idMute) set ({ { xp (selected, 1), state (selected) == BandState::mute ? 0.0f : 1.0f } });
        else if (c.id == idPrev) selected = (selected + count() - 1) % count();
        else if (c.id == idNext) selected = (selected + 1) % count();
        else if (c.id == idBands) showBandsMenu();
        else if (c.id == idMod) showMod = ! showMod; // round 152
        repaint();
    }

    // round 150: 1 … 6 bands; more bands split the top band, fewer remove the top ones
    void showBandsMenu()
    {
        juce::PopupMenu m;
        for (int k = 1; k <= mixsuite::ember::maxBands; ++k)
            m.addItem (juce::String (k), true, count() == k, [this, k] { setBandCount (k); });
        m.showMenuAsync (juce::PopupMenu::Options().withMousePosition()); // round 152 (checked by hand): at the mouse, not under the window
    }

    void setBandCount (int target)
    {
        while (count() > target) removeBand (count() - 1);
        while (count() < target)
        {
            const auto n = count();
            const auto lo = n > 1 ? edgeX (n - 1) : disp().getX();
            const auto before = n;
            splitAt (0.5f * (lo + disp().getRight()));
            if (count() == before) break;
        }
        repaint();
    }

    bool bodyDown (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        dragHandle = dragEdge = -1;
        if (selected < count() && panelArea().contains (p)) return false;
        if (! disp().contains (p)) return false;
        if (const auto h = handleAt (p); h >= 0)
        {
            selected = h;
            if (e.mods.isAltDown()) { removeBand (h); return false; }
            if (ui::isResetClick (e)) { set ({ { fp (h, 1), 0.0f } }); return false; }
            dragHandle = h;
            return true;
        }
        if (const auto i = edgeAt (p); i > 0) { dragEdge = i; return true; }
        selected = bandAtX (p.x);
        repaint();
        return false;
    }

    void bodyDrag (const juce::MouseEvent& e) override
    {
        using namespace mixsuite::ember;
        const auto d = disp();
        if (dragHandle >= 0)
        {
            const auto f = 1.0f - (e.position.y - d.getY() - handleTop) / (panelArea().getY() - 30.0f - d.getY() - handleTop);
            set ({ { fp (dragHandle, 1), juce::jlimit (0.0f, 1.0f, f) } });
            ui::showValueTip (*this, handle (dragHandle).toNearestInt(), tr ("Drive") + " " + text (fp (dragHandle, 1)));
            return;
        }
        if (dragEdge > 0)
        {
            const auto lo = edgeX (dragEdge - 1) + 14.0f, hi = edgeX (dragEdge + 1) - 14.0f;
            const auto hz = juce::jlimit (30.0, 18000.0, hzForX (juce::jlimit (lo, hi, e.position.x), d));
            set ({ { crossoverParam (dragEdge - 1), crossoverValue (hz) } });
            ui::showValueTip (*this, juce::Rectangle<int> (16, 16).withCentre (e.position.toInt()), tr ("Crossover") + " " + text (crossoverParam (dragEdge - 1)));
        }
    }

    bool bodyDoubleClick (const juce::MouseEvent& e) override
    {
        if (! disp().contains (e.position) || (selected < count() && panelArea().contains (e.position))) return false;
        if (const auto h = handleAt (e.position); h >= 0) { removeBand (h); return true; }
        splitAt (e.position.x);
        return true;
    }

    bool bodyWheel (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        const auto h = handleAt (e.position);
        if (h < 0) return false;
        const auto prm = fp (h, 1);
        if (const auto x = ui::wheelValue (v (prm), e, w, specFor (prm))) set ({ { prm, (float) *x } });
        return true;
    }

    void bodyMove (juce::Point<float> p) override { hoverHandle = handleAt (p); }

    ui::Hint bodyHint (juce::Point<float> p) override
    {
        ui::Hint h;
        if (! disp().contains (p)) return h;
        if (handleAt (p) >= 0) h.text = tr ("Band handle: drag up / down = drive, wheel = drive, Alt-click or double-click = remove the band");
        else if (edgeAt (p) > 0) h.text = tr ("Crossover: drag left or right");
        else h.text = tr ("Click a band to edit it; double-click to split it in two (up to six bands)");
        return h;
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        using namespace mixsuite::ember;
        const auto d = disp();
        displayBackground (g, bodyArea());
        frequencyLabels (g, d, d.getBottom() + 12.0f);
        paintSpectrum (g, specPost, [this] (float db) { return specY (db); }, inColour().withAlpha (0.14f), inColour().withAlpha (0.45f));
        const auto n = count();
        for (int b = 0; b < n; ++b)
        {
            const auto c = peqview::bandColour (b);
            const auto region = juce::Rectangle<float> (edgeX (b), d.getY(), edgeX (b + 1) - edgeX (b), d.getHeight());
            g.setColour (c.withAlpha (b == selected ? 0.12f : 0.04f));
            g.fillRect (region);
            // the drive level line across the band, its handle and the band's name / drive
            const auto h = handle (b);
            g.setColour (c.withAlpha (0.5f));
            g.drawHorizontalLine ((int) h.getCentreY(), region.getX(), region.getRight());
            const auto hot = b == hoverHandle || b == dragHandle;
            g.setColour (hot ? c.brighter (0.3f) : c);
            g.fillRoundedRectangle (h, 4.0f);
            g.setColour (colour::bg0);
            g.drawRoundedRectangle (h, 4.0f, 1.0f);
            g.setColour (colour::textPrimary);
            g.setFont (font (type::caption, true));
            // round 135: band 1 shows the voicing when one is chosen (it replaces the style)
            const auto vk = mixsuite::ember::voiceIndex (v (mixsuite::ember::voice));
            juce::String tag = caps (chr (b) > 0 ? tr (mixsuite::ember::characterName (chr (b)))
                                                 : (b == 0 && vk > 0 && n == 1 ? tr (mixsuite::ember::voiceName (vk)) : text (fp (b, 0)))) + "  " + text (fp (b, 1));
            if (! powered (b)) tag = tr ("OFF") + "  " + tag;
            else if (state (b) == mixsuite::ember::BandState::solo) tag = "S  " + tag;
            else if (state (b) == mixsuite::ember::BandState::mute) tag = "M  " + tag;
            g.drawText (tag, juce::Rectangle<float> (region.getX() + 2.0f, h.getY() - 18.0f, region.getWidth() - 4.0f, 14.0f), juce::Justification::centred, true);
        }
        // round 135: the in / out level meter (the reference's VU: what the drive does to the level)
        {
            const auto m = vuArea();
            g.setColour (colour::bg0.withAlpha (0.7f));
            g.fillRoundedRectangle (m.expanded (4.0f, 16.0f).withTrimmedBottom (-2.0f), 4.0f);
            const auto bars = m.withTrimmedTop (2.0f).withTrimmedBottom (14.0f);
            const auto in = dbOf (liveMeter (2)), out = dbOf (liveMeter (3));
            drawMeter (g, bars.withWidth (bars.getWidth() * 0.5f - 2.0f), in, -48.0f, inColour());
            drawMeter (g, bars.withTrimmedLeft (bars.getWidth() * 0.5f + 2.0f), out, -48.0f, colour::accent);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            g.drawText (tr ("IN"), juce::Rectangle<float> (bars.getX(), m.getY() - 14.0f, bars.getWidth() * 0.5f, 12.0f), juce::Justification::centred, false);
            g.drawText (tr ("OUT"), juce::Rectangle<float> (bars.getCentreX(), m.getY() - 14.0f, bars.getWidth() * 0.5f, 12.0f), juce::Justification::centred, false);
            g.drawText (juce::String (out > -100.0f ? out : -99.0f, 1), juce::Rectangle<float> (m.getX() - 4.0f, bars.getBottom() + 1.0f, m.getWidth() + 8.0f, 12.0f), juce::Justification::centred, false);
        }
        for (int i = 1; i < n; ++i)
        {
            const auto x = edgeX (i);
            const auto hot = std::abs (mouse.x - x) < 5.0f && d.contains (mouse);
            g.setColour (hot || dragEdge == i ? colour::textPrimary : colour::textSecondary.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, d.getY(), d.getBottom());
            const auto knobR = juce::Rectangle<float> (16.0f, 16.0f).withCentre ({ x, d.getY() + 44.0f }); // round 135: under the strip
            g.setColour (colour::bg5);
            g.fillRoundedRectangle (knobR, 3.0f);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            g.drawText (text (mixsuite::ember::crossoverParam (i - 1)), juce::Rectangle<float> (x + 10.0f, d.getY() + 37.0f, 70.0f, 14.0f), juce::Justification::centredLeft, false);
        }
        if (selected < n)
        {
            const auto a = panelArea();
            const auto c = peqview::bandColour (selected);
            drawPanel (g, a, c);
            g.setColour (c);
            g.fillRoundedRectangle (juce::Rectangle<float> (a.getX() + 14.0f, a.getY() - 1.0f, 46.0f, 3.0f), 1.5f);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::small, true));
            if (showMod) // round 152: the modulation panel's captions
            {
                g.drawText (tr ("Modulation"), juce::Rectangle<float> (a.getX() + 12.0f, a.getY() + 10.0f, 120.0f, 26.0f), juce::Justification::centredLeft, false);
                caption (g, { a.getX() + 150.0f, a.getY() + 24.0f, 124.0f, 12.0f }, tr ("Envelope"));
                caption (g, { a.getX() + 300.0f, a.getY() + 20.0f, 368.0f, 12.0f }, tr ("Source  /  target  /  amount"));
                return;
            }
            g.drawText (tr ("Band {0}", { juce::String (selected + 1) }), juce::Rectangle<float> (a.getX() + 12.0f, a.getY() + 10.0f, 58.0f, 26.0f), juce::Justification::centredLeft, false);
            caption (g, { a.getX() + 432.0f, a.getY() + 2.0f, 174.0f, 12.0f }, tr ("Tone"));
            // the curve of the band's style, as a small preview
            const auto cv = juce::Rectangle<float> (a.getX() + 228.0f, a.getY() + 10.0f, 0.0f, 0.0f);
            juce::ignoreUnused (cv, fx);
        }
    }
};

// =============================================================================================================
// Aether — the reference "air" processor's workflow: the output level on an arc meter fills the top; under it two
// big knobs side by side (MID AIR = the presence exciter, HIGH AIR = the air shelf) with the link button between
// them, the power switch at the bottom left and the small TRIM at the bottom right. LYNX's extra settings (the two
// frequencies, the exciter's character, mix) sit in the bottom bar.
class AetherEditor final : public ProEditor
{
public:
    AetherEditor (AudioEngine& e, int t, int s, bool d) : ProEditor (e, t, s, EffectKind::aether, d, 720, 520) { fillDemo(); }

private:
    enum { idLink = 1, idPower };
    bool linked = false;

    juce::String title() const override { return "Aether"; }
    juce::String gestures() const override { return tr ("Link: both knobs move together"); }
    bool usesHistory() const override { return true; }

    // the dynamic air model (round 124) has its own two amounts; the classic mode drives the exciter and the shelf
    bool dynamicModel() const { return v (P ("Dynamic")) >= 0.5f; }
    int pMid() const { return dynamicModel() ? P ("Mid air") : P ("Harmonics"); }
    int pHigh() const { return dynamicModel() ? P ("High air") : P ("Air"); }
    juce::Rectangle<float> display() const { return bodyArea().withHeight (206.0f); }
    juce::Rectangle<float> panel() const { return bodyArea().withTrimmedTop (206.0f); }

    Column demoColumn (int i) const override
    {
        const auto in = demoInDb (i) - 4.0f;
        return { in, in + 1.5f, 0.0f };
    }

    void linkChanges (std::vector<std::pair<int, float>>& changes) const override
    {
        if (! linked || changes.size() != 1) return;
        const auto [p, x] = changes.front();
        if (p != pMid() && p != pHigh()) return;
        const auto other = p == pMid() ? pHigh() : pMid();
        changes.push_back ({ other, juce::jlimit (0.0f, 1.0f, v (other) + (x - v (p))) });
    }

    void layoutControls (std::vector<Ctl>& out) const override
    {
        const auto a = panel();
        const auto cy = a.getY() + 12.0f;
        out.push_back (knob ({ 125.0f, cy + 10.0f, 150.0f, 184.0f }, pMid(), tr ("Mid air"), 2));
        out.push_back (knob ({ 445.0f, cy + 10.0f, 150.0f, 184.0f }, pHigh(), tr ("High air"), 2));
        out.push_back (button ({ 330.0f, cy + 70.0f, 60.0f, 28.0f }, idLink, tr ("Link"), linked,
                               tr ("Link: move both knobs together (keeps their difference)")));
        out.push_back (custom ({ 28.0f, a.getBottom() - 70.0f, 48.0f, 48.0f }, idPower, -1, tr ("Power: switch the effect on or off")));
        out.push_back (knob ({ 626.0f, a.getBottom() - 92.0f, 72.0f, 86.0f }, P ("Output"), tr ("Trim"), 0, true));
        const auto b = barArea();
        const auto y = b.getCentreY() - 11.0f;
        out.push_back (barValue ({ 8.0f, y, 110.0f, 22.0f }, P ("Dynamic"), tr ("Dynamic")));
        if (dynamicModel())
        {
            out.push_back (barValue ({ 490.0f, y, 110.0f, 22.0f }, P ("Mix"), tr ("Mix")));
            out.push_back (barValue ({ 604.0f, y, 110.0f, 22.0f }, P ("Output"), tr ("Trim")));
            return;
        }
        out.push_back (barValue ({ 122.0f, y, 120.0f, 22.0f }, P ("Exciter freq."), tr ("Mid freq")));
        out.push_back (barValue ({ 246.0f, y, 120.0f, 22.0f }, P ("Character"), tr ("Even / odd")));
        out.push_back (barValue ({ 370.0f, y, 116.0f, 22.0f }, P ("Air freq."), tr ("High freq")));
        out.push_back (barValue ({ 490.0f, y, 110.0f, 22.0f }, P ("Mix"), tr ("Mix")));
        out.push_back (barValue ({ 604.0f, y, 110.0f, 22.0f }, P ("Output"), tr ("Trim")));
    }

    void buttonPressed (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idLink) { linked = ! linked; repaint(); }
    }
    void customDown (const Ctl& c, const juce::MouseEvent&) override
    {
        if (c.id == idPower) toggleEnabled();
    }

    void paintCustom (juce::Graphics& g, const Ctl& c) override
    {
        if (c.id != idPower) return;
        const auto* fx = current();
        const auto on = fx != nullptr && fx->enabled;
        const auto r = c.r.reduced (2.0f);
        g.setColour (c.r.contains (mouse) ? colour::bg5 : colour::bg4);
        g.fillEllipse (r);
        g.setColour (on ? colour::accent : colour::borderStrong);
        g.drawEllipse (r, 1.5f);
        juce::Path glyph;
        const auto cx = r.getCentreX(), cy = r.getCentreY(), rr = r.getWidth() * 0.24f;
        glyph.addCentredArc (cx, cy, rr, rr, 0.0f, 0.75f, juce::MathConstants<float>::twoPi - 0.75f, true);
        glyph.startNewSubPath (cx, cy - rr * 1.3f);
        glyph.lineTo (cx, cy - rr * 0.2f);
        g.setColour (on ? colour::accent : colour::textDisabled);
        g.strokePath (glyph, juce::PathStrokeType (2.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        caption (g, c.r.translated (0.0f, -18.0f).withHeight (14.0f).expanded (20.0f, 0.0f), tr ("Power"));
    }

    // the arc meter's scale: compressed at the bottom, open near 0 dB, like a programme meter
    static float arcFrac (float db)
    {
        static constexpr std::array<std::pair<float, float>, 8> marks { { { -120.0f, 0.0f }, { -96.0f, 0.08f }, { -48.0f, 0.26f }, { -24.0f, 0.46f },
                                                                          { -12.0f, 0.64f }, { -6.0f, 0.78f }, { -3.0f, 0.87f }, { 0.0f, 0.95f } } };
        if (db <= marks.front().first) return 0.0f;
        if (db >= 0.0f) return juce::jmin (1.0f, 0.95f + db * 0.0167f);
        for (std::size_t k = 1; k < marks.size(); ++k)
            if (db <= marks[k].first)
            {
                const auto [d0, f0] = marks[k - 1];
                const auto [d1, f1] = marks[k];
                return f0 + (f1 - f0) * (db - d0) / (d1 - d0);
            }
        return 0.95f;
    }

    void paintBody (juce::Graphics& g, const EffectSlot& fx) override
    {
        const auto d = display();
        displayBackground (g, bodyArea());
        juce::ColourGradient bg (colour::bg1.brighter (0.06f), d.getCentreX(), d.getY(), colour::bg0, d.getCentreX(), d.getBottom(), false);
        g.setGradientFill (bg);
        g.fillRect (d);

        // the output level (peak of the last 100 ms, with a hold)
        auto level = -120.0f, hold = -120.0f;
        for (int k = 0; k < 10; ++k) level = juce::jmax (level, columnBack (k).out);
        for (int k = 0; k < 150; ++k) hold = juce::jmax (hold, columnBack (k).out);
        if (! fx.enabled) level = hold = -120.0f;

        const juce::Point<float> centre { d.getCentreX(), d.getBottom() + 40.0f };
        const auto radius = d.getHeight() * 0.86f;
        const auto a0 = -0.36f * juce::MathConstants<float>::pi, a1 = 0.36f * juce::MathConstants<float>::pi;
        const auto angleOf = [&] (float db) { return a0 + (a1 - a0) * arcFrac (db); };
        const auto at = [&] (float angle, float rad) { return centre + juce::Point<float> { std::sin (angle), -std::cos (angle) } * rad; };

        // the track and the lit part (blue, red above −6 dB)
        juce::Path track;
        track.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, a1, true);
        g.setColour (juce::Colour (0xff08090b));
        g.strokePath (track, juce::PathStrokeType (13.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (colour::bg4);
        g.strokePath (track, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (level > -119.0f)
        {
            const auto split = angleOf (-6.0f), now = angleOf (level);
            juce::Path lit;
            lit.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, a0, juce::jmin (now, split), true);
            g.setColour (accent.withAlpha (0.14f));
            g.strokePath (lit, juce::PathStrokeType (24.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setGradientFill (juce::ColourGradient (colour::info, centre.x - radius, centre.y, accent, centre.x + radius * 0.4f, centre.y - radius, false));
            g.strokePath (lit, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            if (now > split)
            {
                juce::Path hot;
                hot.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, split, now, true);
                g.setColour (colour::accent);
                g.strokePath (hot, juce::PathStrokeType (10.0f, juce::PathStrokeType::curved, juce::PathStrokeType::butt));
            }
        }
        if (hold > -119.0f)
        {
            g.setColour (hold > -6.0f ? colour::accent : colour::textPrimary);
            g.fillEllipse (juce::Rectangle<float> (9.0f, 9.0f).withCentre (at (angleOf (hold), radius)));
        }
        // the red zone and the scale
        juce::Path zone;
        zone.addCentredArc (centre.x, centre.y, radius + 12.0f, radius + 12.0f, 0.0f, angleOf (-6.0f), a1, true);
        g.setColour (colour::accent.withAlpha (0.6f));
        g.strokePath (zone, juce::PathStrokeType (2.0f));
        g.setFont (font (type::caption, true));
        for (const auto db : { -120.0f, -96.0f, -48.0f, -24.0f, -12.0f, -6.0f, -3.0f, 0.0f })
        {
            const auto angle = angleOf (db);
            g.setColour (db >= -6.0f ? colour::accent : colour::textSecondary);
            g.drawLine ({ at (angle, radius + 8.0f), at (angle, radius + 15.0f) }, 1.4f);
            g.drawText (db <= -120.0f ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : juce::String ((int) db),
                        juce::Rectangle<float> (40.0f, 14.0f).withCentre (at (angle, radius + 26.0f)), juce::Justification::centred, false);
        }
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        g.drawText (level > -119.0f ? juce::String (level, 1) + " dB" : juce::String::fromUTF8 ("-\xe2\x88\x9e dB"),
                    juce::Rectangle<float> (160.0f, 22.0f).withCentre ({ centre.x, d.getBottom() - 46.0f }), juce::Justification::centred, false);
        caption (g, juce::Rectangle<float> (160.0f, 14.0f).withCentre ({ centre.x, d.getBottom() - 26.0f }), tr ("Output peak"));

        // the knob panel
        const auto a = panel();
        g.setColour (colour::bg2);
        g.fillRect (a);
        g.setColour (colour::border);
        g.drawHorizontalLine ((int) a.getY(), 0.0f, a.getRight());
        g.setColour (colour::textDisabled);
        g.setFont (font (type::caption));
        const auto ly = a.getY() + 150.0f;
        g.drawText ("0", juce::Rectangle<float> (104.0f, ly, 30.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("100", juce::Rectangle<float> (268.0f, ly, 30.0f, 14.0f), juce::Justification::centredLeft, false);
        g.drawText ("0", juce::Rectangle<float> (424.0f, ly, 30.0f, 14.0f), juce::Justification::centredRight, false);
        g.drawText ("15 dB", juce::Rectangle<float> (588.0f, ly, 40.0f, 14.0f), juce::Justification::centredLeft, false);
    }
};

// --- windows -------------------------------------------------------------------------------------------------
std::unique_ptr<ProEditor> make (AudioEngine& engine, int track, int slot, bool demo)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return nullptr;
    switch (mixer[(std::size_t) track].effects[(std::size_t) slot].kind)
    {
        case EffectKind::apexLimiter: return std::make_unique<ApexEditor> (engine, track, slot, demo);
        case EffectKind::hushDeEsser: return std::make_unique<HushEditor> (engine, track, slot, demo);
        case EffectKind::crestCompressor: return std::make_unique<CrestEditor> (engine, track, slot, demo);
        case EffectKind::sentryGate: return std::make_unique<SentryEditor> (engine, track, slot, demo);
        case EffectKind::prismMultiband: return std::make_unique<PrismEditor> (engine, track, slot, demo);
        case EffectKind::ember: return std::make_unique<EmberEditor> (engine, track, slot, demo);
        case EffectKind::aether: return std::make_unique<AetherEditor> (engine, track, slot, demo);
        default: return nullptr;
    }
}

// The editor drawn at the window's size (round 128: the originals' windows can be resized): the layout is fixed, the
// whole editor is scaled (mouse positions follow the transform), the window keeps the editor's proportions.
class ScaledHolder final : public juce::Component
{
public:
    explicit ScaledHolder (std::unique_ptr<ProEditor> e) : editor (std::move (e))
    {
        baseW = juce::jmax (1, editor->getWidth());
        baseH = juce::jmax (1, editor->getHeight());
        addAndMakeVisible (*editor);
        setSize (baseW, baseH);
    }
    void resized() override
    {
        const auto s = juce::jmin ((float) getWidth() / (float) baseW, (float) getHeight() / (float) baseH);
        editor->setBounds (0, 0, baseW, baseH);
        editor->setTransform (juce::AffineTransform::scale (s));
    }
    std::unique_ptr<ProEditor> editor;
    int baseW = 1, baseH = 1;
};

class ProEditorWindow final : public juce::DocumentWindow
{
public:
    ProEditorWindow (std::unique_ptr<ProEditor> editor, const juce::String& title, AudioEngine& engine, int track, int slot)
        : DocumentWindow (title, colour::bg1, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          gone (engine, track, slot, [this] { closeButtonPressed(); })
    {
        setUsingNativeTitleBar (true);
        content = editor.get();
        auto holder = std::make_unique<ScaledHolder> (std::move (editor));
        const auto w = holder->baseW, h = holder->baseH;
        setContentOwned (holder.release(), true);
        setResizable (true, false);
        if (auto* c = getConstrainer())
        {
            c->setFixedAspectRatio ((double) w / (double) h);
            c->setMinimumSize (juce::roundToInt (w * 0.7), juce::roundToInt (h * 0.7));
            c->setMaximumSize (w * 2, h * 2);
        }
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
        if (content != nullptr) skin::playOpenAnimation (*content, content->editorStyle()); // round 126: fade / scale in
    }
    void closeButtonPressed() override;
    ProEditor* content = nullptr;

private:
    ui::SlotGoneWatcher gone;
};

std::map<std::pair<int, int>, std::unique_ptr<ProEditorWindow>>& windows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<ProEditorWindow>> open;
    return open;
}

void ProEditorWindow::closeButtonPressed()
{
    for (auto& [key, window] : windows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { windows().erase (k); });
            return;
        }
}
} // namespace

namespace ProEditorWindows
{
bool handles (EffectKind kind) noexcept
{
    return kind == EffectKind::apexLimiter || kind == EffectKind::hushDeEsser || kind == EffectKind::crestCompressor
           || kind == EffectKind::sentryGate || kind == EffectKind::prismMultiband || kind == EffectKind::ember
           || kind == EffectKind::aether;
}

void open (AudioEngine& engine, int track, int slot)
{
    auto& open = windows();
    const auto key = std::make_pair (track, slot);
    if (auto it = open.find (key); it != open.end())
    {
        if (it->second->content != nullptr && it->second->content->current() != nullptr)
        {
            it->second->toFront (true);
            return;
        }
        open.erase (it); // the slot now holds another effect
    }
    auto editor = make (engine, track, slot, false);
    if (editor == nullptr) return;
    const auto& mixer = engine.getSong().mixer;
    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    const auto title = utf8 (effectInfo (mixer[(std::size_t) track].effects[(std::size_t) slot].kind).name) + " - "
                       + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " (" + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    open[key] = std::make_unique<ProEditorWindow> (std::move (editor), title, engine, track, slot);
}

void closeAll() { windows().clear(); }
} // namespace ProEditorWindows

std::unique_ptr<juce::Component> makeProEditor (AudioEngine& engine, int track, int slot, bool demo)
{
    return make (engine, track, slot, demo);
}
} // namespace lynx
