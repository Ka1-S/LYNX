#include "ui/VisualEditors.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <array>
#include <map>
#include <thread>
#include <atomic>

#include "core/Effects.h"
#include "core/Patcher.h"
#include "engine/Exporter.h"
#include "platform/Paths.h"
#include "ui/ColourPicker.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"
#include "ui/SlotWatch.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

#if JUCE_WINDOWS
 #include <windows.h>
#else
 #include <fcntl.h>
 #include <signal.h>
 #include <sys/wait.h>
 #include <unistd.h>
#endif

namespace lynx
{
using namespace visualfx;
namespace wc = visualfx::wavecandy;

// ---------------------------------------------------------------------------------------------------------------
// Target
// ---------------------------------------------------------------------------------------------------------------
const EffectSlot* VisualTarget::current() const
{
    if (engine == nullptr || ! locate)
        return nullptr;
    return locate (const_cast<Song&> (engine->getSong())); // (read only)
}

void VisualTarget::edit (const std::function<void (EffectSlot&)>& change, bool undoable) const
{
    if (engine == nullptr || ! locate)
        return;
    engine->editSong ([loc = locate, change] (Song& song) {
        if (auto* fx = loc (song))
            change (*fx);
    }, undoable);
}

VisualTarget VisualTarget::mixerSlot (AudioEngine& engine, int track, int slot)
{
    VisualTarget t;
    t.engine = &engine;
    t.track = track;
    t.slot = slot;
    t.locate = [track, slot] (Song& song) -> EffectSlot* {
        if (track < 0 || track >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) track].effects.size())
            return nullptr;
        auto& fx = song.mixer[(std::size_t) track].effects[(std::size_t) slot];
        return isVisualKind (fx.kind) ? &fx : nullptr;
    };
    t.running = [&engine, track, slot] { return engine.getEffect (track, slot); };
    t.runningIn = [track, slot] (AudioEngine& e) { return e.getEffect (track, slot); };
    return t;
}

namespace
{
constexpr double twoPi = 6.283185307179586;

juce::Colour argb (std::uint32_t c)
{
    return juce::Colour (c);
}

// ---------------------------------------------------------------------------------------------------------------
// The audio a display reads: the slot's tap, or (demo) a built-in test signal
// ---------------------------------------------------------------------------------------------------------------
float demoSample (std::int64_t i, double rate, int ch)
{
    const auto t = (double) i / rate;
    const auto beat = std::fmod (t, 0.5);
    double v = 0.0;
    // kick: a falling sine
    if (beat < 0.25)
        v += 0.7 * std::sin (twoPi * (45.0 * beat + 90.0 * (1.0 - std::exp (-beat * 30.0)) / 30.0)) * std::exp (-beat * 9.0);
    // bass on the off-beat
    if (beat > 0.25)
        v += 0.18 * (2.0 * std::fmod (t * 55.0, 1.0) - 1.0) * std::exp (-(beat - 0.25) * 6.0);
    // a pad chord (wider in stereo), moving slowly
    const auto lfo = 0.5 + 0.5 * std::sin (twoPi * 0.2 * t);
    for (const auto hz : { 220.0, 277.18, 329.63, 440.0 })
        v += 0.06 * std::sin (twoPi * hz * (ch == 0 ? 1.0 : 1.003) * t + (ch == 0 ? 0.0 : lfo));
    // hats: noise bursts on the eighths
    const auto eighth = std::fmod (t, 0.25);
    if (eighth < 0.04)
    {
        auto h = (std::uint32_t) (i * 2654435761u + (std::uint32_t) ch * 97u);
        h ^= h >> 13;
        h *= 1274126177u;
        v += 0.08 * ((double) (h >> 8) / 16777216.0 - 0.5) * std::exp (-eighth * 90.0);
    }
    return (float) (v * 0.6);
}

class TapFeed
{
public:
    TapFeed (VisualTarget t, bool d) : target (std::move (t)), demo (d)
    {
        startMs = juce::Time::getMillisecondCounterHiRes();
    }

    // the newest `want` frames into left / right; returns how many of them are new since the last pull
    int pull (std::vector<float>& left, std::vector<float>& right, int want)
    {
        want = juce::jlimit (1, tapSize, want);
        left.resize ((std::size_t) want);
        right.resize ((std::size_t) want);
        if (demo)
        {
            // the test signal, generated once per sample into a ring (as the audio thread fills a tap)
            const auto now = (std::int64_t) ((juce::Time::getMillisecondCounterHiRes() - startMs) * 0.001 * rate);
            if (demoL.empty())
            {
                demoL.assign ((std::size_t) tapSize, 0.0f);
                demoR.assign ((std::size_t) tapSize, 0.0f);
            }
            for (auto at = juce::jmax (demoMade, now - tapSize); at < now; ++at)
            {
                demoL[(std::size_t) (at % tapSize)] = demoSample (at, rate, 0);
                demoR[(std::size_t) (at % tapSize)] = demoSample (at, rate, 1);
            }
            demoMade = now;
            for (int i = 0; i < want; ++i)
            {
                const auto at = now - want + i;
                left[(std::size_t) i] = at < 0 ? 0.0f : demoL[(std::size_t) (at % tapSize)];
                right[(std::size_t) i] = at < 0 ? 0.0f : demoR[(std::size_t) (at % tapSize)];
            }
            const auto fresh = (int) juce::jlimit<std::int64_t> (0, want, now - lastCount);
            lastCount = now;
            return fresh;
        }
        // the running effect (it changes when the slot is rebuilt; keeping the pointer keeps the tap alive)
        if (target.running)
            effect = target.running();
        auto* tap = tapOf (effect.get());
        if (tap == nullptr)
        {
            std::fill (left.begin(), left.end(), 0.0f);
            std::fill (right.begin(), right.end(), 0.0f);
            return 0;
        }
        rate = tap->sampleRate.load (std::memory_order_relaxed);
        const auto written = tap->read (left.data(), right.data(), want);
        auto fresh = (std::int64_t) written - (std::int64_t) lastCount;
        if (fresh < 0 || written < (std::uint64_t) lastCount) // another instance (rebuilt)
            fresh = (std::int64_t) written;
        lastCount = (std::int64_t) written;
        return (int) juce::jlimit<std::int64_t> (0, want, fresh);
    }

    double sampleRate() const noexcept { return rate; }

    // the knobs as the audio thread used them (automation), else the slot's
    float param (int i) const
    {
        if (! demo)
            if (auto* tap = tapOf (effect.get()); tap != nullptr && tap->written() > 0)
                return tap->param (i);
        const auto* fx = target.current();
        return fx != nullptr ? fx->params[(std::size_t) i] : effectInfo (EffectKind::waveCandy).params.size() > (std::size_t) i ? 0.0f : 0.0f;
    }

    std::array<float, shownParams> params() const
    {
        std::array<float, shownParams> p {};
        for (int i = 0; i < shownParams; ++i)
            p[(std::size_t) i] = param (i);
        return p;
    }

    VisualTarget target;
    bool demo = false;

private:
    std::shared_ptr<Effect> effect;
    std::int64_t lastCount = 0, demoMade = 0;
    std::vector<float> demoL, demoR;
    double rate = 48000.0, startMs = 0.0;
};

// ---------------------------------------------------------------------------------------------------------------
// A knob bound to one of the slot's parameters (LYNX's control contract; automatable on a mixer slot)
// ---------------------------------------------------------------------------------------------------------------
class ParamKnob final : public juce::Component
{
public:
    ParamKnob (const VisualTarget& t, EffectKind k, int p, juce::Colour a) : target (t), kind (k), param (p), accent (a)
    {
        const auto& info = effectInfo (kind);
        spec.name = tr (juce::String (info.params[(std::size_t) param].name));
        spec.defaultValue = info.params[(std::size_t) param].defaultValue;
        spec.toText = [k, p] (double v) { return juce::String (formatEffectParam (k, p, (float) v, 120.0)); };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        if (target.track >= 0)
            spec.target = AutomationTarget { AutomationTargetKind::effectParam, target.track, target.slot, param };
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, spec.name, true, spec.target.has_value());
    }

    double value() const
    {
        const auto* fx = target.current();
        return fx != nullptr ? (double) fx->params[(std::size_t) param] : spec.defaultValue;
    }

    void set (double v)
    {
        target.edit ([p = param, v] (EffectSlot& fx) { fx.params[(std::size_t) p] = (float) juce::jlimit (0.0, 1.0, v); });
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto label = area.removeFromBottom (13.0f);
        const auto valueArea = area.removeFromBottom (13.0f);
        const auto size = juce::jmin (area.getWidth(), area.getHeight());
        skin::KnobLook look;
        look.value = (float) value();
        look.accent = accent;
        look.hover = isMouseOverOrDragging() ? 1.0f : 0.0f;
        look.active = drag.isActive();
        skin::knob (g, juce::Rectangle<float> (size, size).withCentre (area.getCentre()), look);
        g.setFont (theme::font (theme::type::caption));
        g.setColour (skin::valueColour (isMouseOverOrDragging()));
        g.drawText (spec.text (value()), valueArea, juce::Justification::centred);
        g.setColour (skin::captionColour());
        g.drawText (spec.name, label, juce::Justification::centred);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, target.track >= 0 ? target.engine : nullptr, spec, value(), [this] (double v) { set (v); });
            return;
        }
        if (ui::isResetClick (e))
        {
            set (spec.defaultValue);
            return;
        }
        drag.begin (e, value());
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isActive())
            return;
        set (drag.update (e, spec));
        ui::showValueTip (*this, getLocalBounds(), spec.text (value()));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        drag.end();
        ui::hideValueTip();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override { set (spec.defaultValue); }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (const auto v = ui::wheelValue (value(), e, wheel, spec))
        {
            set (*v);
            ui::flashValueTip (*this, getLocalBounds(), spec.text (value()));
        }
    }

private:
    VisualTarget target;
    EffectKind kind;
    int param;
    juce::Colour accent;
    ui::ValueSpec spec;
    ui::ValueDrag drag;
};

// a flat tab / toggle button in the skin
class SkinButton final : public juce::Button
{
public:
    SkinButton (const juce::String& text, juce::Colour a) : juce::Button (text), accent (a) {}
    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        skin::button (g, getLocalBounds().toFloat().reduced (1.0f), getToggleState(), over || down, accent);
        g.setColour (getToggleState() ? theme::colour::textPrimary : theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::small, getToggleState()));
        g.drawFittedText (getButtonText(), getLocalBounds().reduced (4, 0), juce::Justification::centred, 1);
    }
    juce::Colour accent;
};

juce::String noteName (double hz)
{
    if (hz <= 0.0)
        return {};
    const auto midi = (int) std::lround (69.0 + 12.0 * std::log2 (hz / 440.0));
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return juce::String (names[((midi % 12) + 12) % 12]) + juce::String (midi / 12 - 1);
}

juce::String hzText (double hz)
{
    if (hz < 1000.0)
        return juce::String ((int) std::lround (hz)) + " Hz";
    const auto k = hz / 1000.0;
    return (std::abs (k - std::round (k)) < 0.005 ? juce::String ((int) std::lround (k)) : juce::String (k, k >= 10.0 ? 1 : 2)) + " kHz";
}

// ---------------------------------------------------------------------------------------------------------------
// Wave Candy's display
// ---------------------------------------------------------------------------------------------------------------
class WaveCandyDisplay final : public juce::Component,
                               private juce::Timer
{
public:
    explicit WaveCandyDisplay (TapFeed& f) : feed (f)
    {
        setOpaque (true);
        meter.prepare (48000.0);
        startTimerHz (40);
    }

    std::function<void (wc::Mode)> onModePicked;
    std::function<void()> onHoldToggled;

    wc::Mode shownMode() const { return wc::resolve (wc::modeOf (feed.param (wc::mode)), getWidth(), getHeight()); }

    void resized() override
    {
        spectrogram = {};
        persistence = {};
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.fillAll (juce::Colour (0xff07080b));
        const auto mode = shownMode();
        switch (mode)
        {
            case wc::Mode::oscilloscope: paintScope (g, area); break;
            case wc::Mode::spectrum: paintSpectrum (g, area); break;
            case wc::Mode::spectrogram: paintSpectrogram (g, area); break;
            case wc::Mode::peakMeter: paintMeter (g, area); break;
            case wc::Mode::vectorscope: paintVector (g, area); break;
            case wc::Mode::automatic: break;
        }
        if (frozen())
        {
            g.setColour (theme::colour::warning);
            g.setFont (theme::font (theme::type::small, true));
            g.drawText (tr ("HOLD"), area.reduced (8.0f).removeFromTop (16.0f), juce::Justification::topRight);
        }
        if (! hoverText.isEmpty())
        {
            g.setFont (theme::font (theme::type::small));
            const auto w = (float) juce::GlyphArrangement::getStringWidthInt (theme::font (theme::type::small), hoverText) + 14.0f;
            auto box = juce::Rectangle<float> (w, 20.0f).withPosition (hover.toFloat() + juce::Point<float> (12.0f, -26.0f));
            box = box.constrainedWithin (area.reduced (2.0f));
            g.setColour (juce::Colour (0xe0101218));
            g.fillRoundedRectangle (box, 4.0f);
            g.setColour (theme::colour::textPrimary);
            g.drawText (hoverText, box, juce::Justification::centred);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hover = e.getPosition();
        updateHover();
        repaint();
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        hoverText.clear();
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isMiddleButtonDown())
        {
            if (onHoldToggled)
                onHoldToggled();
            return;
        }
        if (! e.mods.isPopupMenu())
            return;
        juce::PopupMenu menu;
        const auto current = wc::modeOf (feed.param (wc::mode));
        for (int m = 0; m < wc::modeCount; ++m)
            menu.addItem (tr (wc::modeName ((wc::Mode) m)), true, current == (wc::Mode) m, [this, m] {
                if (onModePicked)
                    onModePicked ((wc::Mode) m);
            });
        menu.addSeparator();
        menu.addItem (tr ("Hold (middle-click)"), true, frozen(), [this] {
            if (onHoldToggled)
                onHoldToggled();
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

private:
    bool frozen() const { return feed.param (wc::hold) >= 0.5f; }
    juce::Colour traceColour (float shift = 0.0f) const
    {
        return juce::Colour::fromHSV (std::fmod (feed.param (wc::colour) + shift + 1.0f, 1.0f), 0.78f, 1.0f, 1.0f);
    }
    float gain() const { return juce::Decibels::decibelsToGain ((float) wc::gainDb (feed.param (wc::gain))); }

    // x → frequency on the spectrum's axis (Scale: 0 linear … 1 logarithmic)
    double freqAt (float t) const
    {
        const auto scale = (double) feed.param (wc::specScale);
        const auto logF = 20.0 * std::pow (1000.0, (double) t);
        const auto linF = 20.0 + (double) t * 19980.0;
        return std::exp (std::log (linF) + (std::log (logF) - std::log (linF)) * scale);
    }
    float posOf (double hz) const // inverse of freqAt (search)
    {
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 24; ++i)
        {
            const auto mid = 0.5f * (lo + hi);
            (freqAt (mid) < hz ? lo : hi) = mid;
        }
        return 0.5f * (lo + hi);
    }

    // the spectrum at frequency hz (dB, weighted and with the gain), the loudest bin between hz and hz2
    float levelAt (double hz, double hz2) const
    {
        if (db.size() < 2)
            return -140.0f;
        const auto binHz = feed.sampleRate() / (double) fftN;
        const auto last = (double) db.size() - 1.0;
        auto k0 = juce::jlimit (0.0, last, hz / binHz), k1 = juce::jlimit (0.0, last, hz2 / binHz);
        if (k1 < k0)
            std::swap (k0, k1);
        float v = -140.0f;
        if (std::floor (k1) >= std::ceil (k0))
            for (auto k = (std::size_t) std::ceil (k0); k <= (std::size_t) std::floor (k1); ++k)
                v = juce::jmax (v, db[k]);
        else
        {
            const auto i = (std::size_t) k0;
            v = i + 1 < db.size() ? db[i] + (db[i + 1] - db[i]) * (float) (k0 - (double) i) : db[i];
        }
        const auto weighting = 3.0 * (double) feed.param (wc::specWeighting);
        return v + (float) (weighting * std::log2 (juce::jmax (1.0, std::sqrt (hz * hz2)) / 1000.0)) + (float) wc::gainDb (feed.param (wc::gain));
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;
        const auto mode = shownMode();
        if (frozen())
        {
            if (mode != lastMode)
                repaint();
            lastMode = mode;
            return;
        }
        lastMode = mode;
        const auto rate = feed.sampleRate();
        // 1. the newest audio
        int want = 4096;
        if (mode == wc::Mode::oscilloscope)
        {
            scopeSpan = juce::jlimit (32, tapSize / 2, (int) (wc::windowMs (feed.param (wc::oscWindow)) * 0.001 * rate));
            want = scopeSpan * 2;
        }
        else if (mode == wc::Mode::spectrum || mode == wc::Mode::spectrogram)
            want = fftN = wc::fftSize (feed.param (wc::specResolution));
        else if (mode == wc::Mode::peakMeter)
            want = juce::jlimit (256, tapSize, (int) (rate * 0.05));
        const auto fresh = feed.pull (l, r, want);

        // 2. silence (stopped, nothing playing): once the display has settled it is not redrawn — an idle editor
        //    costs nothing (the meter decays and the spectrogram scrolls out first)
        bool silent = true;
        for (int i = want - fresh; i < want && silent; ++i)
            silent = std::abs (l[(std::size_t) i]) < 1.0e-5f && std::abs (r[(std::size_t) i]) < 1.0e-5f;
        silentFrames = silent ? silentFrames + 1 : 0;
        const auto settle = mode == wc::Mode::oscilloscope ? 3
                            : mode == wc::Mode::spectrogram ? 80 // (2 s of silence scrolled in, then the picture stays)
                            : mode == wc::Mode::peakMeter ? (juce::jmax (meter.peakDb (0), meter.peakDb (1), meter.levelDb (0), meter.levelDb (1))
                                                                     > (float) wc::meterFloorDb (feed.param (wc::meterScale)) - 1.0f
                                                                 ? 1 << 30
                                                                 : 3)
                                                          : 90;
        if (silentFrames > settle && hoverText.isEmpty())
            return;

        // 3. the mode's analysis
        switch (mode)
        {
            case wc::Mode::oscilloscope: break;
            case wc::Mode::spectrum:
            case wc::Mode::spectrogram:
            {
                mono.resize ((std::size_t) fftN);
                for (int i = 0; i < fftN; ++i)
                    mono[(std::size_t) i] = 0.5f * (l[(std::size_t) i] + r[(std::size_t) i]);
                analysis::spectrumDb (mono.data(), fftN, db, work);
                if (mode == wc::Mode::spectrogram)
                    scrollSpectrogram();
                break;
            }
            case wc::Mode::peakMeter:
            {
                if (meterRate != rate)
                {
                    meter.prepare (rate);
                    meterRate = rate;
                }
                const auto g = gain();
                const auto n = fresh > 0 ? fresh : (int) (rate / 40.0); // nothing new (no audio): time passes in silence
                scratchL.assign ((std::size_t) n, 0.0f);
                scratchR.assign ((std::size_t) n, 0.0f);
                if (fresh > 0)
                    for (int i = 0; i < n; ++i)
                    {
                        scratchL[(std::size_t) i] = l[(std::size_t) (want - n + i)] * g;
                        scratchR[(std::size_t) i] = r[(std::size_t) (want - n + i)] * g;
                    }
                meter.process (scratchL.data(), scratchR.data(), n, wc::meterModeOf (feed.param (wc::meterMode)), wc::holdSeconds (feed.param (wc::meterHold)),
                               wc::decayDbPerSecond (feed.param (wc::meterDecay)));
                break;
            }
            case wc::Mode::vectorscope: drawVectorPoints (fresh); break;
            case wc::Mode::automatic: break;
        }
        if (! hoverText.isEmpty())
            updateHover();
        repaint();
    }

    // ---- oscilloscope ------------------------------------------------------------------------------------------
    void paintScope (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto h = area.getHeight(), w = area.getWidth();
        g.setColour (juce::Colour (0xff1a1d24));
        for (int k = 1; k < 8; ++k)
            g.drawVerticalLine ((int) (w * k / 8.0f), 0.0f, h);
        g.drawHorizontalLine ((int) (h * 0.25f), 0.0f, w);
        g.drawHorizontalLine ((int) (h * 0.75f), 0.0f, w);
        g.setColour (juce::Colour (0xff2a2e38));
        g.drawHorizontalLine ((int) (h * 0.5f), 0.0f, w);
        if (l.size() < (std::size_t) scopeSpan * 2)
            return;
        std::vector<float> m ((std::size_t) scopeSpan * 2);
        for (std::size_t i = 0; i < m.size(); ++i)
            m[i] = l[i] + r[i];
        const auto start = feed.param (wc::oscSync) >= 0.5f ? analysis::triggerPoint (m.data(), (int) m.size(), scopeSpan) : scopeSpan;
        const auto sep = feed.param (wc::oscSeparation);
        const auto amp = h * 0.45f * (1.0f - 0.5f * sep) * gain();
        for (int ch = 1; ch >= 0; --ch)
        {
            const auto& x = ch == 0 ? l : r;
            const auto centre = h * 0.5f + (ch == 0 ? -1.0f : 1.0f) * sep * h * 0.25f;
            const auto colour = traceColour (ch == 0 ? 0.0f : 0.08f);
            juce::Path p;
            const auto columns = juce::jmax (2, (int) w);
            if (scopeSpan <= columns * 2)
            {
                for (int i = 0; i < scopeSpan; ++i)
                {
                    const auto px = (float) i / (float) (scopeSpan - 1) * w;
                    const auto py = centre - juce::jlimit (-1.5f, 1.5f, x[(std::size_t) (start + i)]) * amp;
                    i == 0 ? p.startNewSubPath (px, py) : p.lineTo (px, py);
                }
                g.setColour (colour.withAlpha (0.18f));
                g.strokePath (p, juce::PathStrokeType (4.0f));
                g.setColour (colour);
                g.strokePath (p, juce::PathStrokeType (1.6f));
            }
            else
            {
                // more samples than pixels: each column's min…max
                g.setColour (colour);
                for (int c = 0; c < columns; ++c)
                {
                    const auto i0 = start + c * scopeSpan / columns, i1 = start + (c + 1) * scopeSpan / columns;
                    float lo = 1e9f, hi = -1e9f;
                    for (int i = juce::jmax (start, i0 - 1); i < juce::jmax (i0 + 1, i1); ++i) // (from the previous column's last: no gaps)
                    {
                        lo = juce::jmin (lo, x[(std::size_t) i]);
                        hi = juce::jmax (hi, x[(std::size_t) i]);
                    }
                    const auto top = centre - juce::jlimit (-1.5f, 1.5f, hi) * amp, bottom = centre - juce::jlimit (-1.5f, 1.5f, lo) * amp;
                    g.fillRect ((float) c, top, 1.0f, juce::jmax (1.2f, bottom - top));
                }
            }
        }
        g.setColour (theme::colour::textDisabled);
        g.setFont (theme::font (theme::type::caption));
        g.drawText (juce::String (wc::windowMs (feed.param (wc::oscWindow)), 1) + " ms", area.reduced (6.0f), juce::Justification::bottomRight);
    }

    // ---- spectrum ----------------------------------------------------------------------------------------------
    void paintFreqGrid (juce::Graphics& g, juce::Rectangle<float> area, bool vertical)
    {
        g.setFont (theme::font (theme::type::caption));
        float lastEdge = -1e9f; // labels that would overlap the previous one are left out
        for (const auto hz : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const auto t = posOf (hz);
            const auto label = hzText (hz);
            const auto labelW = (float) juce::GlyphArrangement::getStringWidthInt (theme::font (theme::type::caption), label) + 6.0f;
            g.setColour (vertical ? juce::Colours::white.withAlpha (0.10f) : juce::Colour (0xff1a1d24));
            if (vertical)
            {
                const auto y = area.getBottom() - t * area.getHeight();
                g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
                if (area.getBottom() - y < lastEdge + 14.0f)
                    continue;
                lastEdge = area.getBottom() - y;
                g.setColour (theme::colour::textSecondary.withAlpha (0.8f));
                g.drawText (label, juce::Rectangle<float> (area.getX() + 4.0f, y - 13.0f, labelW, 12.0f), juce::Justification::left);
            }
            else
            {
                const auto x = area.getX() + t * area.getWidth();
                g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                if (x < lastEdge)
                    continue;
                lastEdge = x + 3.0f + labelW;
                g.setColour (theme::colour::textDisabled);
                g.drawText (label, juce::Rectangle<float> (x + 3.0f, area.getBottom() - 14.0f, labelW, 12.0f), juce::Justification::left);
            }
        }
    }

    void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto range = (float) wc::rangeDb (feed.param (wc::specRange));
        g.setFont (theme::font (theme::type::caption));
        for (float d = 0.0f; d > -range; d -= range > 70.0f ? 20.0f : 10.0f)
        {
            const auto y = area.getY() - d / range * area.getHeight();
            g.setColour (juce::Colour (0xff1a1d24));
            g.drawHorizontalLine ((int) y, area.getX(), area.getRight());
            g.setColour (theme::colour::textDisabled);
            g.drawText (juce::String ((int) d) + " dB", juce::Rectangle<float> (area.getRight() - 52.0f, y + 1.0f, 48.0f, 12.0f), juce::Justification::right);
        }
        paintFreqGrid (g, area, false);
        if (db.empty())
            return;
        const auto columns = juce::jmax (2, (int) (area.getWidth() / 3.0f)); // a point every 3 px (cheap to stroke, still smooth)
        if ((int) curve.size() != columns)
            curve.assign ((std::size_t) columns, -140.0f);
        const auto fall = (float) wc::specFallDbPerFrame (feed.param (wc::specSpeed));
        juce::Path line, fill;
        for (int c = 0; c < columns; ++c)
        {
            const auto t0 = (float) c / (float) columns, t1 = (float) (c + 1) / (float) columns;
            const auto v = levelAt (freqAt (t0), freqAt (t1));
            auto& shown = curve[(std::size_t) c];
            if (! frozen())
                shown = juce::jmax (v, shown - fall);
            const auto x = area.getX() + t0 * area.getWidth();
            const auto y = juce::jlimit (area.getY(), area.getBottom(), area.getY() - shown / range * area.getHeight());
            if (c == 0)
            {
                line.startNewSubPath (x, y);
                fill.startNewSubPath (x, area.getBottom());
            }
            line.lineTo (x, y);
            fill.lineTo (x, y);
        }
        fill.lineTo (area.getRight(), area.getBottom());
        fill.closeSubPath();
        const auto colour = traceColour();
        // (two plain strokes and a flat fill: the 4-stroke skin glow cost ~3x the CPU at 40 frames a second)
        g.setColour (colour.withAlpha (0.16f));
        g.fillPath (fill);
        const juce::PathStrokeType wide (4.5f, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
        const juce::PathStrokeType thin (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
        g.setColour (colour.withAlpha (0.22f));
        g.strokePath (line, wide);
        g.setColour (colour);
        g.strokePath (line, thin);
    }

    // ---- spectrogram -------------------------------------------------------------------------------------------
    juce::Colour heat (float v) const // 0..1 → colour (dark → trace hue → white)
    {
        v = juce::jlimit (0.0f, 1.0f, v);
        const auto hue = feed.param (wc::colour);
        if (v < 0.6f)
            return juce::Colour::fromHSV (std::fmod (hue + 0.7f - v * 0.9f + 2.0f, 1.0f), 0.9f, v / 0.6f * 0.9f, 1.0f);
        return juce::Colour::fromHSV (hue, (1.0f - (v - 0.6f) / 0.4f) * 0.85f, 0.9f + (v - 0.6f) * 0.25f, 1.0f);
    }

    void scrollSpectrogram()
    {
        const auto w = getWidth(), h = getHeight();
        if (w < 4 || h < 4)
            return;
        if (! spectrogram.isValid() || spectrogram.getWidth() != w || spectrogram.getHeight() != h)
        {
            spectrogram = juce::Image (juce::Image::RGB, w, h, true);
            juce::Graphics (spectrogram).fillAll (juce::Colour (0xff07080b));
        }
        const auto px = wc::spectrogramPixelsPerFrame (feed.param (wc::specSpeed));
        spectrogram.moveImageSection (0, 0, px, 0, w - px, h);
        const auto range = (float) wc::rangeDb (feed.param (wc::specRange));
        juce::Image::BitmapData data (spectrogram, w - px, 0, px, h, juce::Image::BitmapData::writeOnly);
        for (int y = 0; y < h; ++y)
        {
            const auto t0 = 1.0f - (float) (y + 1) / (float) h, t1 = 1.0f - (float) y / (float) h;
            const auto v = levelAt (freqAt (t0), freqAt (t1));
            const auto c = heat ((v + range) / range);
            for (int x = 0; x < px; ++x)
                data.setPixelColour (x, y, c);
        }
    }

    void paintSpectrogram (juce::Graphics& g, juce::Rectangle<float> area)
    {
        if (spectrogram.isValid())
            g.drawImageAt (spectrogram, 0, 0);
        paintFreqGrid (g, area, true);
    }

    // ---- peak meter --------------------------------------------------------------------------------------------
    void paintMeter (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto floor = (float) wc::meterFloorDb (feed.param (wc::meterScale));
        auto y01 = [&] (float dbValue) { return juce::jlimit (0.0f, 1.0f, (dbValue - floor) / (0.0f - floor)); }; // floor … 0 dB
        auto inner = area.reduced (12.0f, 14.0f);
        inner.removeFromTop (16.0f); // the read-out
        const auto scaleW = 40.0f;
        auto scale = inner.removeFromLeft (scaleW);
        inner.removeFromBottom (22.0f);
        scale.removeFromBottom (22.0f);
        const auto mode = wc::meterModeOf (feed.param (wc::meterMode));
        g.setFont (theme::font (theme::type::caption));
        const auto step = floor < -60.0f ? 12.0f : 6.0f;
        for (float d = 0.0f; d >= floor - 0.01f; d -= step)
        {
            const auto y = inner.getBottom() - y01 (d) * inner.getHeight();
            g.setColour (juce::Colour (0xff1a1d24));
            g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
            g.setColour (theme::colour::textDisabled);
            g.drawText (juce::String ((int) d), juce::Rectangle<float> (scale.getX(), y - 6.0f, scaleW - 6.0f, 12.0f), juce::Justification::right);
        }
        const auto gap = 10.0f;
        const auto barW = (inner.getWidth() - gap) / 2.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            const auto bar = juce::Rectangle<float> (inner.getX() + (float) ch * (barW + gap), inner.getY(), barW, inner.getHeight());
            g.setColour (juce::Colour (0xff111318));
            g.fillRoundedRectangle (bar, 3.0f);
            const auto level = y01 (meter.levelDb (ch));
            const auto lit = bar.withTop (bar.getBottom() - level * bar.getHeight());
            juce::ColourGradient grad (traceColour(), 0.0f, bar.getBottom(), theme::colour::meterClip, 0.0f, bar.getY(), false);
            grad.addColour (0.82, theme::colour::meterMid);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (lit, 3.0f);
            const auto peakY = bar.getBottom() - y01 (meter.peakDb (ch)) * bar.getHeight();
            g.setColour (meter.peakDb (ch) > 0.0f ? theme::colour::meterClip : theme::colour::textPrimary);
            g.fillRect (bar.getX(), peakY - 1.0f, bar.getWidth(), 2.0f);
            g.setColour (theme::colour::textSecondary);
            g.drawText (ch == 0 ? "L" : "R", juce::Rectangle<float> (bar.getX(), bar.getBottom() + 4.0f, bar.getWidth(), 14.0f), juce::Justification::centred);
        }
        // the read-out: the mode and the highest value (LUFS: the momentary loudness)
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::small, true));
        const auto value = mode == analysis::MeterMode::lufs ? meter.loudness() : meter.maxDb();
        g.drawText (juce::String (wc::meterModeName (mode)) + "  " + (value <= -139.0f ? juce::String ("-inf") : juce::String (value, 1)),
                    area.reduced (8.0f).removeFromTop (16.0f), juce::Justification::topLeft);
    }

    // ---- vectorscope -------------------------------------------------------------------------------------------
    void drawVectorPoints (int fresh)
    {
        const auto w = getWidth(), h = getHeight();
        if (w < 8 || h < 8)
            return;
        if (! persistence.isValid() || persistence.getWidth() != w || persistence.getHeight() != h)
            persistence = juce::Image (juce::Image::ARGB, w, h, true);
        {
            // the trail fades (Persistence)
            juce::Image::BitmapData data (persistence, juce::Image::BitmapData::readWrite);
            const auto keep = (int) (255.0f * (0.35f + 0.6f * feed.param (wc::vecPersistence)));
            for (int y = 0; y < h; ++y)
            {
                auto* p = data.getLinePointer (y);
                for (int x = 0; x < w * 4; ++x)
                    p[x] = (juce::uint8) ((p[x] * keep) >> 8);
            }
        }
        const auto n = juce::jlimit (0, (int) l.size(), fresh > 0 ? fresh : 0);
        if (n == 0)
            return;
        const auto size = (float) juce::jmin (w, h) * 0.46f * gain();
        const auto cx = w * 0.5f, cy = h * 0.5f;
        const auto c = traceColour();
        const auto dot = 1 + (int) (feed.param (wc::vecThickness) * 3.0f);
        juce::Image::BitmapData data (persistence, juce::Image::BitmapData::readWrite);
        const auto start = (int) l.size() - n;
        for (int i = start; i < (int) l.size(); ++i)
        {
            const auto L = l[(std::size_t) i], R = r[(std::size_t) i];
            const auto x = (int) (cx + (R - L) * 0.7071f * size), y = (int) (cy - (L + R) * 0.7071f * size);
            for (int dy = 0; dy < dot; ++dy)
                for (int dx = 0; dx < dot; ++dx)
                {
                    const auto px = x + dx, py = y + dy;
                    if (px < 0 || py < 0 || px >= w || py >= h)
                        continue;
                    auto* pixel = (juce::PixelARGB*) data.getPixelPointer (px, py);
                    // additive: each sample adds light
                    const auto add = [] (juce::uint8 a, int b) { return (juce::uint8) juce::jmin (255, (int) a + b); };
                    pixel->setARGB (255, add (pixel->getRed(), c.getRed() / 5), add (pixel->getGreen(), c.getGreen() / 5), add (pixel->getBlue(), c.getBlue() / 5));
                }
        }
        // correlation of the newest block
        double lr = 0.0, ll = 0.0, rr = 0.0;
        for (int i = start; i < (int) l.size(); ++i)
        {
            lr += (double) l[(std::size_t) i] * r[(std::size_t) i];
            ll += (double) l[(std::size_t) i] * l[(std::size_t) i];
            rr += (double) r[(std::size_t) i] * r[(std::size_t) i];
        }
        const auto target = ll > 1e-9 && rr > 1e-9 ? (float) (lr / std::sqrt (ll * rr)) : 0.0f;
        correlation += (target - correlation) * 0.3f;
    }

    void paintVector (juce::Graphics& g, juce::Rectangle<float> area)
    {
        const auto c = area.getCentre();
        const auto size = juce::jmin (area.getWidth(), area.getHeight()) * 0.46f;
        g.setColour (juce::Colour (0xff1a1d24));
        for (const auto f : { 0.5f, 1.0f })
        {
            g.drawLine (c.x - size * f * 0.7071f, c.y - size * f * 0.7071f, c.x + size * f * 0.7071f, c.y + size * f * 0.7071f);
            g.drawLine (c.x + size * f * 0.7071f, c.y - size * f * 0.7071f, c.x - size * f * 0.7071f, c.y + size * f * 0.7071f);
        }
        g.drawVerticalLine ((int) c.x, c.y - size, c.y + size);
        g.drawHorizontalLine ((int) c.y, c.x - size, c.x + size);
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textDisabled);
        g.drawText ("L", juce::Rectangle<float> (c.x - size * 0.75f - 10.0f, c.y - size * 0.75f - 14.0f, 20.0f, 12.0f), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (c.x + size * 0.75f - 10.0f, c.y - size * 0.75f - 14.0f, 20.0f, 12.0f), juce::Justification::centred);
        g.drawText ("M", juce::Rectangle<float> (c.x - 10.0f, c.y - size - 14.0f, 20.0f, 12.0f), juce::Justification::centred);
        if (persistence.isValid())
            g.drawImageAt (persistence, 0, 0);
        // the correlation bar (−1 … +1)
        auto bar = area.reduced (16.0f).removeFromBottom (8.0f);
        g.setColour (juce::Colour (0xff111318));
        g.fillRoundedRectangle (bar, 3.0f);
        const auto x = bar.getCentreX() + correlation * bar.getWidth() * 0.5f;
        g.setColour (correlation < 0.0f ? theme::colour::meterClip : theme::colour::ledOn);
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (juce::jmin (x, bar.getCentreX()), bar.getY(), juce::jmax (x, bar.getCentreX()), bar.getBottom()));
        g.setColour (theme::colour::textDisabled);
        g.drawText ("-1", bar.translated (0.0f, -13.0f).withHeight (12.0f), juce::Justification::left);
        g.drawText ("+1", bar.translated (0.0f, -13.0f).withHeight (12.0f), juce::Justification::right);
    }

    void updateHover()
    {
        const auto w = (float) juce::jmax (1, getWidth()), h = (float) juce::jmax (1, getHeight());
        switch (shownMode())
        {
            case wc::Mode::spectrum:
            {
                const auto t = hover.x / w;
                const auto hz = freqAt (t);
                const auto range = (float) wc::rangeDb (feed.param (wc::specRange));
                hoverText = hzText (hz) + "  " + noteName (hz) + "  " + juce::String (-hover.y / h * range, 1) + " dB";
                break;
            }
            case wc::Mode::spectrogram:
            {
                const auto t = 1.0f - hover.y / h;
                const auto hz = freqAt (t);
                hoverText = hzText (hz) + "  " + noteName (hz) + "  " + juce::String (levelAt (freqAt (t - 1.0f / h), hz), 1) + " dB";
                break;
            }
            case wc::Mode::oscilloscope:
            {
                const auto ms = hover.x / w * wc::windowMs (feed.param (wc::oscWindow));
                const auto level = (0.5f - hover.y / h) / 0.45f / gain();
                hoverText = juce::String (ms, 1) + " ms  " + juce::String (juce::Decibels::gainToDecibels (std::abs (level)), 1) + " dB";
                break;
            }
            default: hoverText.clear(); break;
        }
    }

    TapFeed& feed;
    std::vector<float> l, r, mono, db, curve, scratchL, scratchR;
    std::vector<std::complex<float>> work;
    int fftN = 4096, scopeSpan = 1024;
    analysis::Meter meter;
    double meterRate = 0.0;
    juce::Image spectrogram, persistence;
    float correlation = 0.0f;
    juce::Point<int> hover;
    juce::String hoverText;
    wc::Mode lastMode = wc::Mode::automatic;
    int silentFrames = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// Wave Candy's editor
// ---------------------------------------------------------------------------------------------------------------
class WaveCandyEditor final : public juce::Component,
                              private juce::ChangeListener
{
public:
    WaveCandyEditor (const VisualTarget& t, bool demo) : target (t), feed (t, demo), display (feed)
    {
        accent = juce::Colour (0xff36c6ff);
        addAndMakeVisible (display);
        display.onModePicked = [this] (wc::Mode m) { setMode (m); };
        display.onHoldToggled = [this] { toggleHold(); };
        for (int m = 0; m < wc::modeCount; ++m)
        {
            auto b = std::make_unique<SkinButton> (tr (wc::modeName ((wc::Mode) m)), accent);
            b->onClick = [this, m] { setMode ((wc::Mode) m); };
            addAndMakeVisible (*b);
            tabs.push_back (std::move (b));
        }
        modeButton.onClick = [this] {
            juce::PopupMenu menu;
            const auto* fx = target.current();
            const auto current = fx != nullptr ? wc::modeOf (fx->params[wc::mode]) : wc::Mode::spectrum;
            for (int m = 0; m < wc::modeCount; ++m)
                menu.addItem (tr (wc::modeName ((wc::Mode) m)), true, current == (wc::Mode) m, [this, m] { setMode ((wc::Mode) m); });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&modeButton));
        };
        addChildComponent (modeButton);
        holdButton.setButtonText (tr ("Hold"));
        holdButton.onClick = [this] { toggleHold(); };
        ui::setHint (holdButton, tr ("Freeze the display (also: middle-click it)"));
        addAndMakeVisible (holdButton);
        for (int p = 0; p < wc::count; ++p)
        {
            if (p == wc::mode || p == wc::hold)
                continue;
            knobs[p] = std::make_unique<ParamKnob> (target, EffectKind::waveCandy, p, accent);
            addChildComponent (*knobs[p]);
        }
        if (target.engine != nullptr)
            target.engine->addChangeListener (this);
        refreshControls();
        setSize (760, 480);
    }

    ~WaveCandyEditor() override
    {
        if (target.engine != nullptr)
            target.engine->removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        skin::canvas (g, getLocalBounds().toFloat(), accent);
        auto header = getLocalBounds().removeFromTop (36).toFloat();
        skin::headerBar (g, header, accent);
        skin::titleText (g, header.reduced (12.0f, 6.0f), "Wave Candy", accent);
        auto controls = getLocalBounds().removeFromBottom (controlsHeight).toFloat();
        skin::bottomBar (g, controls);
        g.setColour (skin::captionColour());
        g.setFont (theme::font (theme::type::caption));
        if (getWidth() >= 560)
            g.drawText (tr ("Visual - shows the sound, does not change it"), header.reduced (12.0f, 0.0f), juce::Justification::centredRight);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        area.removeFromTop (36);
        auto controls = area.removeFromBottom (controlsHeight).reduced (8, 6);
        display.setBounds (area.reduced (8, 6)); // first: Auto picks its mode from the display's size
        auto tabRow = controls.removeFromTop (26);
        holdButton.setBounds (tabRow.removeFromRight (70));
        tabRow.removeFromRight (8);
        // narrow: one button with the mode's menu instead of the tabs
        const auto narrow = tabRow.getWidth() < 6 * 84;
        modeButton.setVisible (narrow);
        const auto tabW = juce::jmin (118, tabRow.getWidth() / juce::jmax (1, (int) tabs.size()));
        for (auto& b : tabs)
        {
            b->setVisible (! narrow);
            b->setBounds (tabRow.removeFromLeft (tabW));
        }
        modeButton.setBounds (controls.getX(), tabRow.getY(), juce::jmin (200, holdButton.getX() - controls.getX() - 8), 26);
        controls.removeFromTop (6);
        layoutKnobs (controls);
    }

private:
    static constexpr int controlsHeight = 120;

    std::vector<int> knobsFor (wc::Mode m) const
    {
        switch (m)
        {
            case wc::Mode::oscilloscope: return { wc::gain, wc::oscWindow, wc::oscSeparation, wc::oscSync, wc::colour };
            case wc::Mode::spectrum: return { wc::gain, wc::specSpeed, wc::specRange, wc::specScale, wc::specWeighting, wc::specResolution, wc::colour };
            case wc::Mode::spectrogram: return { wc::gain, wc::specSpeed, wc::specRange, wc::specScale, wc::specWeighting, wc::specResolution, wc::colour };
            case wc::Mode::peakMeter: return { wc::gain, wc::meterMode, wc::meterHold, wc::meterDecay, wc::meterScale, wc::colour };
            case wc::Mode::vectorscope: return { wc::gain, wc::vecPersistence, wc::vecThickness, wc::colour };
            case wc::Mode::automatic: break;
        }
        return { wc::gain, wc::colour };
    }

    void layoutKnobs (juce::Rectangle<int> row)
    {
        const auto shown = knobsFor (display.shownMode());
        for (auto& [p, k] : knobs)
            k->setVisible (std::find (shown.begin(), shown.end(), p) != shown.end());
        const auto w = juce::jmin (76, row.getWidth() / juce::jmax (1, (int) shown.size()));
        for (const auto p : shown)
            knobs[p]->setBounds (row.removeFromLeft (w));
    }

    void refreshControls()
    {
        const auto* fx = target.current();
        const auto mode = fx != nullptr ? wc::modeOf (fx->params[wc::mode]) : wc::Mode::spectrum;
        for (int m = 0; m < (int) tabs.size(); ++m)
            tabs[(std::size_t) m]->setToggleState ((int) mode == m, juce::dontSendNotification);
        modeButton.setButtonText (tr (wc::modeName (mode)) + juce::String::fromUTF8 (" \xe2\x96\xbe"));
        holdButton.setToggleState (fx != nullptr && fx->params[wc::hold] >= 0.5f, juce::dontSendNotification);
        holdButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::warning.darker (0.4f));
        resized();
        repaint();
    }

    void setMode (wc::Mode m)
    {
        target.edit ([m] (EffectSlot& fx) { fx.params[wc::mode] = wc::modeValue (m); });
        refreshControls();
    }

    void toggleHold()
    {
        target.edit ([] (EffectSlot& fx) { fx.params[wc::hold] = fx.params[wc::hold] >= 0.5f ? 0.0f : 1.0f; }, false);
        refreshControls();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        const auto* fx = target.current();
        const auto key = fx != nullptr ? std::make_pair (fx->params[wc::mode], fx->params[wc::hold]) : std::make_pair (-1.0f, -1.0f);
        if (key != lastKey)
        {
            lastKey = key;
            refreshControls();
        }
        for (auto& [p, k] : knobs)
            if (k->isVisible())
                k->repaint();
    }

    VisualTarget target;
    TapFeed feed;
    WaveCandyDisplay display;
    juce::Colour accent;
    std::vector<std::unique_ptr<SkinButton>> tabs;
    juce::TextButton holdButton, modeButton;
    std::map<int, std::unique_ptr<ParamKnob>> knobs;
    std::pair<float, float> lastKey { -2.0f, -2.0f };
};
} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Scene renderer
// ---------------------------------------------------------------------------------------------------------------
struct SceneRenderer::State
{
    struct Ring
    {
        double born = 0.0;
        float strength = 1.0f;
    };
    std::vector<ParticleField> particles;
    std::vector<std::vector<Ring>> rings;
    std::vector<float> spin;
    std::map<std::string, juce::Image> images;
    std::map<std::size_t, std::pair<juce::String, juce::Image>> layerImages; // per layer slot: key + picture
    double lastTime = -1.0;

    // a picture of `area`'s size drawn by `paint` once, again only when `key` changes
    const juce::Image& cached (std::size_t slot, const juce::String& key, juce::Rectangle<float> area,
                               const std::function<void (juce::Graphics&, juce::Rectangle<float>)>& paint, bool opaque = false)
    {
        auto& entry = layerImages[slot];
        if (entry.first != key || ! entry.second.isValid())
        {
            const auto w = juce::jmax (1, (int) std::round (area.getWidth())), h = juce::jmax (1, (int) std::round (area.getHeight()));
            // (an opaque picture is copied, not blended, when drawn)
            entry.second = juce::Image (opaque ? juce::Image::RGB : juce::Image::ARGB, w, h, true, juce::SoftwareImageType());
            juce::Graphics g (entry.second);
            paint (g, juce::Rectangle<float> ((float) w, (float) h));
            entry.first = key;
        }
        return entry.second;
    }

    const juce::Image& image (const std::string& path)
    {
        auto it = images.find (path);
        if (it != images.end())
            return it->second;
        juce::Image img;
        const auto file = platform::localFile (path);
        if (file.existsAsFile())
            img = juce::ImageFileFormat::loadFrom (file);
        if (img.isValid() && (img.getWidth() > 2048 || img.getHeight() > 2048)) // keep drawing cheap
        {
            const auto s = 2048.0f / (float) juce::jmax (img.getWidth(), img.getHeight());
            img = img.rescaled (juce::jmax (1, (int) (img.getWidth() * s)), juce::jmax (1, (int) (img.getHeight() * s)), juce::Graphics::mediumResamplingQuality);
        }
        if (images.size() > 16)
            images.clear();
        return images[path] = img;
    }
};

SceneRenderer::SceneRenderer() : state (std::make_unique<State>()) {}
SceneRenderer::~SceneRenderer() = default;

void SceneRenderer::reset()
{
    state->particles.clear();
    state->rings.clear();
    state->spin.clear();
    state->lastTime = -1.0;
}

void SceneRenderer::advance (const scene::Scene& s, const Frame& f, float aspect)
{
    const auto n = s.layers.size();
    state->particles.resize (n);
    state->rings.resize (n);
    state->spin.resize (n, 0.0f);
    const auto speedKnob = (float) std::pow (4.0, (double) f.params[visualizer::speed] * 2.0 - 1.0);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto& layer = s.layers[i];
        if (! layer.enabled)
            continue;
        switch (layer.type)
        {
            case scene::LayerType::particles: state->particles[i].step (f, layer, aspect); break;
            case scene::LayerType::rings:
            {
                auto& list = state->rings[i];
                if (f.onset || (layer.band != scene::Band::beat && f.bandValue (layer.band) > 0.75f && (list.empty() || f.time - list.back().born > 0.25)))
                    list.push_back ({ f.time, 0.4f + 0.6f * juce::jmax (f.bass, f.level) });
                const auto life = 0.6 + layer.speed * 1.4;
                list.erase (std::remove_if (list.begin(), list.end(), [&] (const State::Ring& r) { return f.time - r.born > life; }), list.end());
                while ((int) list.size() > juce::jmax (1, layer.count))
                    list.erase (list.begin());
                break;
            }
            case scene::LayerType::circle:
            case scene::LayerType::image:
                state->spin[i] += (float) f.dt * (layer.type == scene::LayerType::circle ? (layer.speed - 0.0f) : layer.amount) * 1.2f * speedKnob
                                  * (1.0f + f.bandValue (layer.band) * layer.react);
                break;
            default: break;
        }
    }
    state->lastTime = f.time;
}

void SceneRenderer::draw (juce::Graphics& g, juce::Rectangle<float> area, const scene::Scene& s, const Frame& f)
{
    g.saveState();
    g.reduceClipRegion (area.toNearestInt());
    g.setColour (juce::Colours::black);
    g.fillRect (area);
    const auto hueShift = f.params[visualizer::hueShift];
    auto colour = [&] (std::uint32_t c, float alpha) {
        auto col = argb (c);
        if (hueShift > 0.001f)
            col = col.withRotatedHue (hueShift);
        return col.withMultipliedAlpha (juce::jlimit (0.0f, 1.0f, alpha));
    };
    const auto zoom = 0.5f + f.params[visualizer::zoom];
    if (std::abs (zoom - 1.0f) > 0.001f)
        g.addTransform (juce::AffineTransform::scale (zoom, zoom, area.getCentreX(), area.getCentreY()));
    const auto W = area.getWidth(), H = area.getHeight(), minDim = juce::jmin (W, H);
    auto at = [&] (float x, float y) { return juce::Point<float> (area.getX() + x * W, area.getY() + y * H); };
    auto spectrumAt = [&] (float t) { // 0..1 across the 64 bands, interpolated
        const auto pos = juce::jlimit (0.0f, 62.999f, t * 63.0f);
        const auto i = (int) pos;
        return f.spectrum[(std::size_t) i] + (f.spectrum[(std::size_t) i + 1] - f.spectrum[(std::size_t) i]) * (pos - (float) i);
    };
    auto shape = [] (float v, float react) { return std::pow (juce::jlimit (0.0f, 1.0f, v), 1.6f - react * 1.1f); };

    for (std::size_t i = 0; i < s.layers.size(); ++i)
    {
        const auto& L = s.layers[i];
        if (! L.enabled || L.opacity <= 0.0f)
            continue;
        const auto band = f.bandValue (L.band);
        const auto a = L.opacity;
        switch (L.type)
        {
            case scene::LayerType::background:
            {
                bool drew = false;
                if (! L.path.empty())
                    if (const auto& img = state->image (L.path); img.isValid())
                    {
                        // cover the frame; the band pushes it in a little
                        const auto pulse = 1.0f + band * L.react * 0.08f;
                        const auto sc = juce::jmax (W / (float) img.getWidth(), H / (float) img.getHeight()) * pulse;
                        const auto w = (float) img.getWidth() * sc, h = (float) img.getHeight() * sc;
                        g.setOpacity (a);
                        g.drawImage (img, juce::Rectangle<float> (w, h).withCentre (area.getCentre()));
                        g.setOpacity (1.0f);
                        drew = true;
                    }
                // the gradient and the glow are drawn once into images (a blit per frame costs a fraction of a gradient fill)
                const auto size = area.toNearestInt().getWidth() * 100003 + area.toNearestInt().getHeight();
                if (! drew)
                {
                    auto& cached = state->cached (i * 2, juce::String::toHexString ((juce::int64) L.colourA) + juce::String::toHexString ((juce::int64) L.colourB)
                                                             + juce::String (a) + juce::String (hueShift) + "/" + juce::String (size),
                                                  area, [&] (juce::Graphics& cg, juce::Rectangle<float> r) {
                                                      cg.setGradientFill (juce::ColourGradient (colour (L.colourA, a), r.getX(), r.getY(),
                                                                                                colour (L.colourB, a), r.getRight(), r.getBottom(), false));
                                                      cg.fillRect (r);
                                                  },
                                                  a >= 0.999f && argb (L.colourA).isOpaque() && argb (L.colourB).isOpaque());
                    g.drawImageAt (cached, (int) std::round (area.getX()), (int) std::round (area.getY()));
                }
                if (L.react > 0.0f && band > 0.004f)
                {
                    auto& glow = state->cached (i * 2 + 1, juce::String::toHexString ((juce::int64) L.colourB) + juce::String (hueShift) + "/" + juce::String (size),
                                                area, [&] (juce::Graphics& cg, juce::Rectangle<float> r) {
                                                    cg.setGradientFill (juce::ColourGradient (colour (L.colourB, 1.0f), r.getCentreX(), r.getCentreY(),
                                                                                              colour (L.colourB, 0.0f), r.getCentreX(),
                                                                                              r.getY() - r.getHeight() * 0.2f, true));
                                                    cg.fillRect (r);
                                                });
                    g.setOpacity (juce::jlimit (0.0f, 1.0f, band * L.react * 0.6f * a));
                    g.drawImageAt (glow, (int) std::round (area.getX()), (int) std::round (area.getY()));
                    g.setOpacity (1.0f);
                }
                break;
            }
            case scene::LayerType::bars:
            {
                const auto count = juce::jlimit (4, 256, L.count);
                const auto slot = W / (float) count;
                const auto barW = juce::jmax (1.0f, slot * (1.0f - L.amount * 0.8f));
                const auto base = area.getY() + L.y * H;
                const auto maxH = L.size * H * (L.mirror ? 0.5f : 1.0f);
                juce::ColourGradient grad (colour (L.colourA, a), 0.0f, base, colour (L.colourB, a), 0.0f, base - maxH, false);
                g.setGradientFill (grad);
                for (int b = 0; b < count; ++b)
                {
                    const auto v = shape (spectrumAt ((float) b / (float) (count - 1)), L.react);
                    const auto h = juce::jmax (1.0f, v * maxH);
                    const auto x = area.getX() + slot * (float) b + (slot - barW) * 0.5f;
                    g.fillRect (x, base - h, barW, h);
                }
                if (L.mirror)
                {
                    juce::ColourGradient down (colour (L.colourA, a * 0.45f), 0.0f, base, colour (L.colourB, 0.0f), 0.0f, base + maxH, false);
                    g.setGradientFill (down);
                    for (int b = 0; b < count; ++b)
                    {
                        const auto v = shape (spectrumAt ((float) b / (float) (count - 1)), L.react);
                        const auto h = juce::jmax (1.0f, v * maxH);
                        g.fillRect (area.getX() + slot * (float) b + (slot - barW) * 0.5f, base, barW, h);
                    }
                }
                break;
            }
            case scene::LayerType::circle:
            {
                const auto c = at (L.x, L.y);
                const auto radius = L.size * minDim * 0.5f * (1.0f + f.beat * 0.08f * L.react);
                const auto count = juce::jlimit (8, 360, L.count);
                const auto spin = state->spin.size() > i ? state->spin[i] : 0.0f;
                juce::Path rays;
                for (int b = 0; b < count; ++b)
                {
                    // the spectrum mirrored round the circle (low frequencies at the top, both sides)
                    const auto t = (float) b / (float) count;
                    const auto v = shape (spectrumAt (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f), L.react);
                    const auto angle = t * juce::MathConstants<float>::twoPi + spin;
                    const auto len = 2.0f + v * radius * 0.9f;
                    const auto dir = juce::Point<float> (std::sin (angle), -std::cos (angle));
                    rays.startNewSubPath (c + dir * radius);
                    rays.lineTo (c + dir * (radius + len));
                }
                const auto thick = juce::jmax (1.5f, juce::MathConstants<float>::twoPi * radius / (float) count * 0.55f);
                if (L.additive)
                {
                    g.setColour (colour (L.colourB, a * 0.25f));
                    g.strokePath (rays, juce::PathStrokeType (thick * 2.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                g.setColour (colour (L.colourA, a));
                g.strokePath (rays, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (colour (L.colourA, a * 0.6f));
                g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c), juce::jmax (1.0f, thick * 0.5f));
                break;
            }
            case scene::LayerType::waveform:
            {
                const auto centre = area.getY() + L.y * H;
                const auto amp = L.size * H * 0.5f * (0.6f + band * L.react);
                const auto x0 = area.getX() + (L.x - 0.5f) * W;
                juce::Path p;
                for (int k = 0; k < 256; ++k)
                {
                    const auto v = 0.5f * (f.waveL[(std::size_t) k] + f.waveR[(std::size_t) k]);
                    const auto px = x0 + (float) k / 255.0f * W, py = centre - juce::jlimit (-1.2f, 1.2f, v) * amp;
                    k == 0 ? p.startNewSubPath (px, py) : p.lineTo (px, py);
                }
                const auto thick = 1.0f + L.amount * 7.0f;
                if (L.additive)
                {
                    g.setColour (colour (L.colourA, a * 0.18f));
                    g.strokePath (p, juce::PathStrokeType (thick * 4.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                g.setColour (colour (L.colourA, a));
                g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                if (L.mirror)
                {
                    p.applyTransform (juce::AffineTransform::verticalFlip (centre * 2.0f));
                    g.setColour (colour (L.colourB, a * 0.6f));
                    g.strokePath (p, juce::PathStrokeType (thick, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                }
                break;
            }
            case scene::LayerType::particles:
            {
                if (state->particles.size() <= i)
                    break;
                const auto dotBase = juce::jmax (1.0f, minDim * 0.012f * (0.3f + L.size * 2.0f));
                for (const auto& p : state->particles[i].particles())
                {
                    const auto alpha = juce::jlimit (0.0f, 1.0f, p.life * 1.4f) * a;
                    const auto col = colour (L.colourA, 1.0f).interpolatedWith (colour (L.colourB, 1.0f), p.hue).withMultipliedAlpha (alpha);
                    const auto d = dotBase * p.size * (1.0f + band * L.react * 0.8f);
                    const auto pos = at (p.x, p.y);
                    if (L.additive && d > 3.5f)
                    {
                        g.setColour (col.withMultipliedAlpha (0.25f));
                        g.fillEllipse (juce::Rectangle<float> (d * 2.6f, d * 2.6f).withCentre (pos));
                    }
                    g.setColour (col);
                    if (d < 2.5f) // a dot that small looks the same as a square and costs far less
                        g.fillRect (juce::Rectangle<float> (d, d).withCentre (pos));
                    else
                        g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (pos));
                }
                break;
            }
            case scene::LayerType::rings:
            {
                if (state->rings.size() <= i)
                    break;
                const auto c = at (L.x, L.y);
                const auto life = 0.6 + L.speed * 1.4;
                for (const auto& r : state->rings[i])
                {
                    const auto age = (float) ((f.time - r.born) / life);
                    if (age < 0.0f || age > 1.0f)
                        continue;
                    const auto radius = (0.05f + age * L.size) * minDim * 0.75f;
                    const auto alpha = (1.0f - age) * r.strength * a;
                    const auto thick = juce::jmax (1.0f, minDim * 0.012f * (1.0f - age) * (0.5f + L.amount));
                    if (L.additive)
                    {
                        g.setColour (colour (L.colourA, alpha * 0.25f));
                        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c), thick * 3.0f);
                    }
                    g.setColour (colour (L.colourA, alpha));
                    g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c), thick);
                }
                break;
            }
            case scene::LayerType::image:
            {
                if (L.path.empty())
                    break;
                const auto& img = state->image (L.path);
                if (! img.isValid())
                    break;
                const auto pulse = 1.0f + band * L.react * 0.3f;
                const auto box = L.size * minDim * pulse;
                const auto sc = box / (float) juce::jmax (img.getWidth(), img.getHeight());
                const auto spin = state->spin.size() > i ? state->spin[i] : 0.0f;
                const auto c = at (L.x, L.y);
                g.setOpacity (a);
                g.drawImageTransformed (img, juce::AffineTransform::translation (-img.getWidth() * 0.5f, -img.getHeight() * 0.5f)
                                                 .scaled (sc)
                                                 .rotated (spin)
                                                 .translated (c.x, c.y));
                g.setOpacity (1.0f);
                break;
            }
            case scene::LayerType::text:
            {
                if (L.text.empty())
                    break;
                const auto text = juce::String::fromUTF8 (L.text.c_str());
                const auto lines = juce::StringArray::fromLines (text);
                const auto pulse = 1.0f + band * L.react * 0.25f;
                const auto size = juce::jmax (6.0f, L.size * H * 0.35f * pulse);
                g.setFont (juce::FontOptions (size, juce::Font::bold));
                const auto lineH = size * 1.15f;
                const auto c = at (L.x, L.y);
                auto box = juce::Rectangle<float> (W * 1.5f, lineH * (float) lines.size()).withCentre (c);
                if (L.amount > 0.01f)
                {
                    // glow: the text drawn round itself in the second colour
                    const auto spread = size * 0.05f * L.amount * (1.0f + band * L.react);
                    g.setColour (colour (L.colourB, a * 0.22f));
                    for (const auto& d : { juce::Point<float> (-1, 0), juce::Point<float> (1, 0), juce::Point<float> (0, -1), juce::Point<float> (0, 1),
                                           juce::Point<float> (-0.7f, -0.7f), juce::Point<float> (0.7f, 0.7f), juce::Point<float> (0.7f, -0.7f),
                                           juce::Point<float> (-0.7f, 0.7f) })
                        g.drawFittedText (text, box.translated (d.x * spread * 2.0f, d.y * spread * 2.0f).toNearestInt(), juce::Justification::centred,
                                          lines.size(), 1.0f);
                }
                g.setColour (colour (L.colourA, a));
                g.drawFittedText (text, box.toNearestInt(), juce::Justification::centred, lines.size(), 1.0f);
                break;
            }
            case scene::LayerType::flash:
            {
                const auto alpha = band * L.react * a;
                if (alpha > 0.004f)
                {
                    g.setColour (colour (L.colourA, alpha));
                    g.fillRect (area);
                }
                break;
            }
        }
    }
    g.restoreState();
    const auto brightness = f.params[visualizer::brightness];
    if (brightness < 0.999f)
    {
        g.setColour (juce::Colours::black.withAlpha (1.0f - brightness));
        g.fillRect (area);
    }
}

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// The Visualizer's live preview (also the full-screen view)
// ---------------------------------------------------------------------------------------------------------------
class ScenePreview final : public juce::Component,
                           private juce::Timer
{
public:
    ScenePreview (TapFeed& f, std::function<const scene::Scene&()> s, bool fullScreen = false) : feed (f), sceneOf (std::move (s)), full (fullScreen)
    {
        setOpaque (true);
        startTimerHz (fullScreen ? 60 : 30);
        lastMs = juce::Time::getMillisecondCounterHiRes();
    }

    std::function<void()> onDoubleClick;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black);
        const auto& scene = sceneOf();
        // the scene's aspect, letterboxed
        auto area = getLocalBounds().toFloat();
        const auto aspect = (float) scene.width / (float) juce::jmax (1, scene.height);
        auto frameArea = area.getWidth() / area.getHeight() > aspect ? area.withSizeKeepingCentre (area.getHeight() * aspect, area.getHeight())
                                                                     : area.withSizeKeepingCentre (area.getWidth(), area.getWidth() / aspect);
        renderer.draw (g, frameArea, scene, analyzer.last());
        if (! full)
        {
            g.setColour (theme::colour::border);
            g.drawRect (frameArea);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (onDoubleClick)
            onDoubleClick();
    }

    void restart()
    {
        renderer.reset();
        analyzer.reset();
    }

private:
    void timerCallback() override
    {
        if (! isShowing())
            return;
        // silent for a few seconds (stopped): a third of the frame rate, so an open idle Visualizer costs little
        if (quietFrames > 90 && ++skipped % 3 != 0)
            return;
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto dt = juce::jlimit (0.001, 0.25, (now - lastMs) * 0.001);
        lastMs = now;
        time += dt;
        const auto fresh = feed.pull (l, r, 4096);
        bool silent = true;
        for (int i = 4096 - fresh; i < 4096 && silent; ++i)
            silent = std::abs (l[(std::size_t) i]) < 1.0e-5f && std::abs (r[(std::size_t) i]) < 1.0e-5f;
        quietFrames = silent ? quietFrames + 1 : 0;
        const auto& scene = sceneOf();
        const auto& f = analyzer.analyze (l.data(), r.data(), (int) l.size(), feed.sampleRate(), time, dt, feed.params());
        renderer.advance (scene, f, (float) scene.width / (float) juce::jmax (1, scene.height));
        repaint();
    }

    TapFeed& feed;
    std::function<const scene::Scene&()> sceneOf;
    bool full = false;
    FrameAnalyzer analyzer;
    SceneRenderer renderer;
    std::vector<float> l, r;
    double lastMs = 0.0, time = 0.0;
    int quietFrames = 0, skipped = 0;
};

// full screen on the display the editor is on; Esc, a click or a double-click closes it
class FullScreenView final : public juce::Component,
                             private juce::Timer
{
public:
    FullScreenView (TapFeed& f, std::function<const scene::Scene&()> s, juce::Rectangle<int> screen) : preview (f, std::move (s), true)
    {
        addAndMakeVisible (preview);
        preview.setInterceptsMouseClicks (false, false);
        setWantsKeyboardFocus (true);
        setOpaque (true);
        setBounds (screen);
        addToDesktop (juce::ComponentPeer::windowAppearsOnTaskbar);
        setAlwaysOnTop (true);
        setVisible (true);
        toFront (true);
        grabKeyboardFocus();
        startTimer (2500);
    }

    std::function<void()> onClose;

    void resized() override { preview.setBounds (getLocalBounds()); }
    void paintOverChildren (juce::Graphics& g) override
    {
        if (! showHint)
            return;
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        auto box = getLocalBounds().toFloat().removeFromTop (40.0f).withSizeKeepingCentre (420.0f, 30.0f).translated (0.0f, 10.0f);
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (juce::Colours::white);
        g.setFont (theme::font (theme::type::body));
        g.drawText (tr ("Esc or a click closes the full screen"), box, juce::Justification::centred);
    }
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::spaceKey || key == juce::KeyPress::returnKey)
            close();
        return true;
    }
    void mouseDown (const juce::MouseEvent&) override { close(); }

private:
    void timerCallback() override
    {
        stopTimer();
        showHint = false;
        repaint();
    }
    void close()
    {
        if (onClose)
            juce::MessageManager::callAsync ([cb = onClose] { cb(); });
    }

    ScenePreview preview;
    bool showHint = true;
};

// ---------------------------------------------------------------------------------------------------------------
// ffmpeg pipe
// ---------------------------------------------------------------------------------------------------------------
class FfmpegPipe
{
public:
    ~FfmpegPipe() { close(); }

    bool open (const juce::File& exe, const juce::StringArray& args, const juce::File& log)
    {
#if JUCE_WINDOWS
        SECURITY_ATTRIBUTES sa { sizeof (sa), nullptr, TRUE };
        HANDLE readEnd = nullptr;
        if (! CreatePipe (&readEnd, &writeEnd, &sa, 1 << 20))
            return false;
        SetHandleInformation (writeEnd, HANDLE_FLAG_INHERIT, 0);
        logHandle = CreateFileW (log.getFullPathName().toWideCharPointer(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        STARTUPINFOW si {};
        si.cb = sizeof (si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = readEnd;
        si.hStdOutput = logHandle;
        si.hStdError = logHandle;
        juce::String cmd = exe.getFullPathName().quoted();
        for (const auto& a : args)
            cmd << " " << (a.containsAnyOf (" \t\"") ? a.replace ("\"", "\\\"").quoted() : a);
        std::wstring line (cmd.toWideCharPointer());
        PROCESS_INFORMATION pi {};
        const auto ok = CreateProcessW (nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle (readEnd);
        if (! ok)
        {
            CloseHandle (writeEnd);
            writeEnd = nullptr;
            return false;
        }
        CloseHandle (pi.hThread);
        process = pi.hProcess;
        return true;
#else
        int fds[2];
        if (pipe (fds) != 0)
            return false;
        const auto logFd = ::open (log.getFullPathName().toRawUTF8(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        std::vector<std::string> argv { exe.getFullPathName().toStdString() };
        for (const auto& a : args)
            argv.push_back (a.toStdString());
        child = fork();
        if (child == 0)
        {
            dup2 (fds[0], 0);
            if (logFd >= 0)
            {
                dup2 (logFd, 1);
                dup2 (logFd, 2);
            }
            ::close (fds[1]);
            std::vector<char*> raw;
            for (auto& s : argv)
                raw.push_back (s.data());
            raw.push_back (nullptr);
            execv (raw[0], raw.data());
            _exit (127);
        }
        ::close (fds[0]);
        if (logFd >= 0)
            ::close (logFd);
        if (child < 0)
        {
            ::close (fds[1]);
            return false;
        }
        signal (SIGPIPE, SIG_IGN);
        fd = fds[1];
        return true;
#endif
    }

    bool write (const void* data, std::size_t bytes)
    {
        auto* p = static_cast<const char*> (data);
        while (bytes > 0)
        {
#if JUCE_WINDOWS
            DWORD done = 0;
            if (writeEnd == nullptr || ! WriteFile (writeEnd, p, (DWORD) juce::jmin<std::size_t> (bytes, 1 << 24), &done, nullptr) || done == 0)
                return false;
#else
            const auto done = ::write (fd, p, bytes);
            if (done <= 0)
                return false;
#endif
            p += done;
            bytes -= (std::size_t) done;
        }
        return true;
    }

    // closes the input and waits for ffmpeg; its exit code (−1: could not wait)
    int close()
    {
#if JUCE_WINDOWS
        if (writeEnd != nullptr)
        {
            CloseHandle (writeEnd);
            writeEnd = nullptr;
        }
        int code = -1;
        if (process != nullptr)
        {
            WaitForSingleObject (process, 120000);
            DWORD exit = 1;
            GetExitCodeProcess (process, &exit);
            code = (int) exit;
            CloseHandle (process);
            process = nullptr;
        }
        if (logHandle != nullptr && logHandle != INVALID_HANDLE_VALUE)
        {
            CloseHandle (logHandle);
            logHandle = nullptr;
        }
        return code;
#else
        if (fd >= 0)
        {
            ::close (fd);
            fd = -1;
        }
        int code = -1;
        if (child > 0)
        {
            int status = 0;
            if (waitpid (child, &status, 0) == child && WIFEXITED (status))
                code = WEXITSTATUS (status);
            child = -1;
        }
        return code;
#endif
    }

private:
#if JUCE_WINDOWS
    HANDLE writeEnd = nullptr, process = nullptr, logHandle = nullptr;
#else
    int fd = -1;
    pid_t child = -1;
#endif
};
} // namespace

juce::File findFfmpeg()
{
    const auto remembered = platform::settingsFile ("ffmpeg-location.txt");
    if (remembered.existsAsFile())
    {
        const juce::File f (remembered.loadFileAsString().trim());
        if (f.existsAsFile())
            return f;
    }
#if JUCE_WINDOWS
    const juce::String name = "ffmpeg.exe";
    const auto sep = ';';
#else
    const juce::String name = "ffmpeg";
    const auto sep = ':';
#endif
    juce::StringArray dirs;
    dirs.addTokens (juce::SystemStats::getEnvironmentVariable ("PATH", {}), juce::String::charToString (sep), "\"");
#if JUCE_WINDOWS
    const auto local = juce::SystemStats::getEnvironmentVariable ("LOCALAPPDATA", {});
    const auto user = juce::SystemStats::getEnvironmentVariable ("USERPROFILE", {});
    const auto programs = juce::SystemStats::getEnvironmentVariable ("ProgramFiles", "C:\\Program Files");
    dirs.addArray ({ local + "\\Microsoft\\WinGet\\Links", "C:\\ProgramData\\chocolatey\\bin", user + "\\scoop\\shims", "C:\\ffmpeg\\bin",
                     programs + "\\ffmpeg\\bin", juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFullPathName() });
#else
    dirs.addArray ({ "/usr/bin", "/usr/local/bin", "/opt/homebrew/bin", "/snap/bin",
                     juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory().getFullPathName() });
#endif
    for (const auto& d : dirs)
        if (d.isNotEmpty() && juce::File::isAbsolutePath (d))
            if (const auto f = juce::File (d).getChildFile (name); f.existsAsFile())
                return f;
    return {};
}

void setFfmpegLocation (const juce::File& exe)
{
    platform::settingsFile ("ffmpeg-location.txt").replaceWithText (exe.getFullPathName());
}

namespace
{
// ---------------------------------------------------------------------------------------------------------------
// Video export
// ---------------------------------------------------------------------------------------------------------------
struct VideoSettings
{
    int width = 1920, height = 1080, fps = 30;
    int crf = 18;
    juce::File output;   // .mp4 (with ffmpeg) or the base name of the frames folder (without)
    juce::File ffmpeg;   // empty: PNG images + WAV
    double maxSeconds = 0.0; // > 0: only the start of the song (the self-test)
};

// The video of a song: its audio rendered as the Export window renders it, then each frame drawn and piped to ffmpeg
// (or written as a PNG). Built on the UI thread (the export engine creates its plugins there), run() on any thread.
class VideoJob
{
public:
    VideoJob (const VisualTarget& t, const scene::Scene& s, VideoSettings v) : scene (s), settings (std::move (v))
    {
        if (const auto* fx = t.current())
            for (int i = 0; i < shownParams; ++i)
                slotParams[(std::size_t) i] = fx->params[(std::size_t) i];
        exportopts::Options o;
        o.sampleRate = 48000.0;
        o.format = exportopts::Format::wav;
        o.wavBits = 24;
        o.maxTailSeconds = 4.0;
        wav = juce::File::createTempFile (".wav");
        job = std::make_unique<ExportJob> (t.engine->getSong(), o, wav);
        runningIn = t.runningIn;
        if (t.runningIn)
            effect = t.runningIn (job->getEngine());
        if (tapOf (effect.get()) != nullptr)
        {
            capture.reserve ((std::size_t) ((job->getMusicSeconds() + o.maxTailSeconds + 4.0) * o.sampleRate));
            setCapture (effect.get(), &capture);
        }
    }

    ~VideoJob()
    {
        if (effect != nullptr)
            setCapture (effect.get(), nullptr);
        wav.deleteFile();
    }

    // progress (0..1, what it is doing) returns false to stop
    bool run (const std::function<bool (double, const juce::String&)>& progress)
    {
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::String renderError;
        const auto ok = job->run ([&] (double p) { return progress (p * 0.3, tr ("Rendering the audio...")); }, renderError);
        if (! ok)
        {
            error = renderError.isNotEmpty() ? renderError : tr ("Cancelled");
            return false;
        }
        renderMs = juce::Time::getMillisecondCounterHiRes() - t0;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (wav));
        if (reader == nullptr || reader->lengthInSamples <= 0)
        {
            error = tr ("The song rendered no audio.");
            return false;
        }
        // what the effect heard (its own input; the capture ends where the WAV ends), else the song's mix
        const auto length = (std::size_t) reader->lengthInSamples;
        double rate = reader->sampleRate;
        std::vector<float> left, right;
        captureNote = effect == nullptr ? juce::String ("no effect") : tapOf (effect.get()) == nullptr ? juce::String ("not a visual effect")
                    : "captured " + juce::String ((juce::int64) capture.used) + " of " + juce::String ((juce::int64) length) + ", processed "
                          + juce::String ((juce::int64) capture.seen) + (runningIn && runningIn (job->getEngine()) != effect ? ", REPLACED" : "") + ", peak "
                          + juce::String (capture.used > 0 ? juce::FloatVectorOperations::findMaximum (capture.left.data(), (int) capture.used) : 0.0f, 3)
                          + (capture.overflow ? " (overflow)" : "");
        // the capture starts with the render (play() comes first) and stops where the transport stops (the song's end;
        // the tail is not "playing"): it lines up with the WAV after the start trim, and the WAV fills the tail
        const auto trim = (std::size_t) juce::jmax<juce::int64> (0, (juce::int64) job->getEngine().getLastRenderStats().trimmedSamples);
        juce::AudioBuffer<float> buffer (2, (int) length);
        reader->read (&buffer, 0, (int) length, 0, true, true);
        left.assign (buffer.getReadPointer (0), buffer.getReadPointer (0) + length);
        right.assign (buffer.getReadPointer (1), buffer.getReadPointer (1) + length);
        if (effect != nullptr && ! capture.overflow && capture.used > trim)
        {
            const auto n = juce::jmin (length, capture.used - trim);
            // (--visualtest: where the capture lines up with the WAV — block envelopes, ±400 blocks of 512)
            if (diagnose)
            {
                auto envelope = [] (const float* x, std::size_t count) {
                    std::vector<float> e (count / 512);
                    for (std::size_t b = 0; b < e.size(); ++b)
                    {
                        float sum = 0.0f;
                        for (std::size_t k = 0; k < 512; ++k)
                            sum += x[b * 512 + k] * x[b * 512 + k];
                        e[b] = std::sqrt (sum / 512.0f);
                    }
                    return e;
                };
                const auto ec = envelope (capture.left.data(), capture.used), ew = envelope (left.data(), length);
                double best = -1.0;
                int bestLag = 0;
                for (int lag = -400; lag <= 400; ++lag)
                {
                    double sum = 0.0, a2 = 0.0, b2 = 0.0;
                    for (std::size_t b = 0; b < ec.size(); ++b)
                    {
                        const auto w = (long) b + lag;
                        if (w < 0 || w >= (long) ew.size())
                            continue;
                        sum += (double) ec[b] * ew[(std::size_t) w];
                        a2 += (double) ec[b] * ec[b];
                        b2 += (double) ew[(std::size_t) w] * ew[(std::size_t) w];
                    }
                    const auto r = a2 > 0.0 && b2 > 0.0 ? sum / std::sqrt (a2 * b2) : 0.0;
                    if (r > best)
                    {
                        best = r;
                        bestLag = lag;
                    }
                }
                captureNote << ", WAV peak " << juce::String (juce::FloatVectorOperations::findMaximum (left.data(), (int) length), 3)
                            << ", capture block 0 = WAV block " << bestLag << " (r=" << juce::String (best, 3) << "), trim " << (int) trim;
            }
            std::copy (capture.left.begin() + (long) trim, capture.left.begin() + (long) (trim + n), left.begin());
            std::copy (capture.right.begin() + (long) trim, capture.right.begin() + (long) (trim + n), right.begin());
            captureOffset = trim;
            usedCapture = true;
        }
        reader.reset();
        auto seconds = (double) left.size() / rate;
        if (settings.maxSeconds > 0.0)
            seconds = juce::jmin (seconds, settings.maxSeconds);
        const auto frameCount = juce::jmax (1, (int) std::ceil (seconds * settings.fps));

        FfmpegPipe pipe;
        juce::File framesFolder;
        log = settings.output.getSiblingFile (settings.output.getFileNameWithoutExtension() + "-ffmpeg.log");
        const auto useFfmpeg = settings.ffmpeg.existsAsFile();
        if (useFfmpeg)
        {
            juce::StringArray args { "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "bgra", "-s",
                                     juce::String (settings.width) + "x" + juce::String (settings.height), "-r", juce::String (settings.fps), "-i", "-",
                                     "-i", wav.getFullPathName(), "-map", "0:v", "-map", "1:a", "-c:v", "libx264", "-preset", "medium", "-crf",
                                     juce::String (settings.crf), "-pix_fmt", "yuv420p", "-c:a", "aac", "-b:a", "320k", "-shortest", "-movflags",
                                     "+faststart", settings.output.getFullPathName() };
            if (! pipe.open (settings.ffmpeg, args, log))
            {
                error = tr ("Could not start ffmpeg ({0}).", { settings.ffmpeg.getFullPathName() });
                return false;
            }
        }
        else
        {
            framesFolder = settings.output.getParentDirectory().getChildFile (settings.output.getFileNameWithoutExtension() + " frames");
            framesFolder.createDirectory();
        }
        const auto status = useFfmpeg ? tr ("Drawing and encoding the frames...") : tr ("Drawing the frames (PNG images)...");
        FrameAnalyzer analyzer;
        SceneRenderer renderer;
        juce::Image image (juce::Image::ARGB, settings.width, settings.height, true, juce::SoftwareImageType());
        std::vector<juce::uint8> pixels ((std::size_t) settings.width * 4 * (std::size_t) settings.height);
        std::size_t paramAt = 0;
        const auto aspect = (float) settings.width / (float) settings.height;
        juce::PNGImageFormat png;
        const auto t1 = juce::Time::getMillisecondCounterHiRes();
        for (int k = 0; k < frameCount; ++k)
        {
            if (k % 4 == 0 && ! progress (0.3 + 0.7 * (double) k / frameCount, status))
            {
                pipe.close();
                settings.output.deleteFile();
                error = tr ("Cancelled");
                return false;
            }
            const auto t = (double) k / settings.fps;
            const auto end = (std::size_t) juce::jlimit<double> (1.0, (double) left.size(), (t + 1.0 / settings.fps) * rate);
            // the knobs as automated at that moment (else as the slot has them)
            auto params = slotParams;
            if (usedCapture && capture.paramsUsed > 0)
            {
                const auto at = end + captureOffset;
                while (paramAt + 1 < capture.paramsUsed && capture.params[paramAt + 1].at <= at)
                    ++paramAt;
                params = capture.params[paramAt].values;
            }
            const auto& f = analyzer.analyze (left.data(), right.data(), (int) end, rate, t, 1.0 / settings.fps, params);
            renderer.advance (scene, f, aspect);
            {
                juce::Graphics g (image);
                renderer.draw (g, image.getBounds().toFloat(), scene, f);
            }
            if (! useFfmpeg)
            {
                juce::FileOutputStream out (framesFolder.getChildFile (juce::String::formatted ("frame_%06d.png", k + 1)));
                if (! out.openedOk() || ! png.writeImageToStream (image, out))
                {
                    error = tr ("Could not write the images in {0}.", { framesFolder.getFullPathName() });
                    return false;
                }
                continue;
            }
            {
                const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);
                const auto rowBytes = (std::size_t) settings.width * 4;
                for (int y = 0; y < settings.height; ++y)
                    std::memcpy (pixels.data() + (std::size_t) y * rowBytes, data.getLinePointer (y), rowBytes);
            }
            if (! pipe.write (pixels.data(), pixels.size()))
            {
                pipe.close();
                error = tr ("ffmpeg stopped (see {0}).", { log.getFullPathName() });
                return false;
            }
        }
        drawMs = juce::Time::getMillisecondCounterHiRes() - t1;
        frames = frameCount;
        if (! useFfmpeg)
        {
            // the audio next to the images, and how to join them
            wav.copyFileTo (framesFolder.getChildFile ("audio.wav"));
            framesFolder.getChildFile ("how to make the video.txt")
                .replaceWithText ("LYNX did not find ffmpeg, so it wrote the video's frames as PNG images and its sound as audio.wav.\n"
                                  "Install ffmpeg (https://ffmpeg.org) and run this in this folder:\n\n" "ffmpeg -framerate "
                                  + juce::String (settings.fps) + " -i frame_%06d.png -i audio.wav -c:v libx264 -pix_fmt yuv420p -crf "
                                  + juce::String (settings.crf) + " -c:a aac -b:a 320k -shortest video.mp4\n");
            result = framesFolder;
            return true;
        }
        progress (1.0, tr ("Finishing the video..."));
        const auto code = pipe.close();
        if (code != 0 || ! settings.output.existsAsFile())
        {
            error = tr ("ffmpeg could not make the video (see {0}).", { log.getFullPathName() });
            return false;
        }
        log.deleteFile();
        result = settings.output;
        return true;
    }

    scene::Scene scene;
    VideoSettings settings;
    juce::File result, log;
    juce::String error, captureNote;
    int frames = 0;
    bool usedCapture = false, diagnose = false;
    double renderMs = 0.0, drawMs = 0.0;

private:
    std::array<float, shownParams> slotParams {};
    std::function<std::shared_ptr<Effect> (AudioEngine&)> runningIn;
    std::unique_ptr<ExportJob> job;
    std::shared_ptr<Effect> effect;
    Capture capture;
    std::size_t captureOffset = 0;
    juce::File wav;
};

class VideoExportThread final : public juce::ThreadWithProgressWindow
{
public:
    VideoExportThread (const VisualTarget& t, const scene::Scene& s, VideoSettings v)
        : juce::ThreadWithProgressWindow (tr ("Exporting video..."), true, true), job (t, s, std::move (v))
    {
    }

    void run() override
    {
        job.run ([this] (double p, const juce::String& status) {
            setProgress (p);
            if (status != lastStatus)
                setStatusMessage (lastStatus = status);
            return ! threadShouldExit();
        });
    }

    void threadComplete (bool cancelled) override
    {
        const auto& s = job.settings;
        if (job.result != juce::File())
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Video exported"),
                                                    job.result.getFullPathName() + "\n\n"
                                                        + tr ("{0} frames, {1} x {2}, {3} fps", { juce::String (job.frames), juce::String (s.width),
                                                                                                juce::String (s.height), juce::String (s.fps) }));
            job.result.revealToUser();
        }
        else if (! cancelled && job.error.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Could not export the video"), job.error);
        juce::MessageManager::callAsync ([this] { delete this; });
    }

private:
    VideoJob job;
    juce::String lastStatus;
};

class VideoExportPanel final : public juce::Component
{
public:
    VideoExportPanel (const VisualTarget& t, const scene::Scene& s, std::function<void (const scene::Scene&)> remember)
        : target (t), scene (s), onRemember (std::move (remember))
    {
        struct Size { int w, h; const char* name; };
        static const Size sizes[] = { { 1280, 720, "1280 x 720 (HD)" }, { 1920, 1080, "1920 x 1080 (Full HD)" }, { 2560, 1440, "2560 x 1440" },
                                      { 3840, 2160, "3840 x 2160 (4K)" }, { 1080, 1080, "1080 x 1080 (square)" }, { 1080, 1920, "1080 x 1920 (vertical)" } };
        int id = 1;
        for (const auto& sz : sizes)
        {
            size.addItem (sz.name, id);
            if (sz.w == scene.width && sz.h == scene.height)
                size.setSelectedId (id, juce::dontSendNotification);
            dims.push_back ({ sz.w, sz.h });
            ++id;
        }
        if (size.getSelectedId() == 0)
        {
            size.addItem (juce::String (scene.width) + " x " + juce::String (scene.height), id);
            dims.push_back ({ scene.width, scene.height });
            size.setSelectedId (id, juce::dontSendNotification);
        }
        for (const auto f : { 24, 25, 30, 50, 60 })
            fps.addItem (juce::String (f) + " fps", f);
        fps.setSelectedId (scene.fps, juce::dontSendNotification);
        if (fps.getSelectedId() == 0)
            fps.setSelectedId (30, juce::dontSendNotification);
        quality.addItem (tr ("Good (smaller file)"), 23);
        quality.addItem (tr ("High"), 18);
        quality.addItem (tr ("Very high (large file)"), 14);
        quality.setSelectedId (18, juce::dontSendNotification);
        for (auto* c : { &size, &fps, &quality })
            addAndMakeVisible (*c);
        for (auto* b : { &locate, &exportButton, &cancel })
            addAndMakeVisible (*b);
        addAndMakeVisible (status);
        status.setFont (theme::font (theme::type::small));
        status.setJustificationType (juce::Justification::topLeft);
        locate.setButtonText (tr ("Locate ffmpeg..."));
        locate.onClick = [this] { chooseFfmpeg(); };
        exportButton.setButtonText (tr ("Export..."));
        exportButton.onClick = [this] { chooseOutput(); };
        cancel.setButtonText (tr ("Cancel"));
        cancel.onClick = [this] { closeWindow(); };
        ffmpeg = findFfmpeg();
        updateStatus();
        setSize (460, 250);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (theme::type::body));
        g.drawText (tr ("Size"), 14, 16, 100, 24, juce::Justification::centredLeft);
        g.drawText (tr ("Frame rate"), 14, 48, 100, 24, juce::Justification::centredLeft);
        g.drawText (tr ("Quality"), 14, 80, 100, 24, juce::Justification::centredLeft);
    }

    void resized() override
    {
        size.setBounds (120, 16, 320, 24);
        fps.setBounds (120, 48, 120, 24);
        quality.setBounds (120, 80, 200, 24);
        status.setBounds (14, 114, 432, 76);
        locate.setBounds (14, getHeight() - 40, 130, 26);
        cancel.setBounds (getWidth() - 100, getHeight() - 40, 86, 26);
        exportButton.setBounds (getWidth() - 196, getHeight() - 40, 86, 26);
    }

private:
    void updateStatus()
    {
        status.setText (ffmpeg.existsAsFile() ? tr ("ffmpeg: {0}\nThe video is saved as an MP4 file (H.264 + AAC) with the song's sound.", { ffmpeg.getFullPathName() })
                                              : tr ("ffmpeg was not found. LYNX will save the frames as PNG images and the sound as a WAV file "
                                                    "(with the command that joins them). Install ffmpeg or locate it to get an MP4 directly."),
                        juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, ffmpeg.existsAsFile() ? theme::colour::textPrimary : theme::colour::warning);
    }

    void chooseFfmpeg()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Locate ffmpeg"), juce::File(),
#if JUCE_WINDOWS
                                                       "ffmpeg.exe");
#else
                                                       "*");
#endif
        juce::Component::SafePointer<VideoExportPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile())
                return;
            setFfmpegLocation (fc.getResult());
            safe->ffmpeg = fc.getResult();
            safe->updateStatus();
        });
    }

    void chooseOutput()
    {
        const auto& song = target.engine->getSong();
        const auto title = song.info.title.empty() ? juce::String ("LYNX video") : juce::String::fromUTF8 (song.info.title.c_str());
        const auto start = juce::File::getSpecialLocation (juce::File::userMoviesDirectory).getChildFile (juce::File::createLegalFileName (title)
                                                                                                         + (ffmpeg.existsAsFile() ? ".mp4" : ""));
        chooser = std::make_unique<juce::FileChooser> (tr ("Export video"), start, ffmpeg.existsAsFile() ? "*.mp4" : "*");
        juce::Component::SafePointer<VideoExportPanel> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;
            safe->start (fc.getResult());
        });
    }

    void start (juce::File out)
    {
        VideoSettings v;
        const auto pick = juce::jlimit (0, (int) dims.size() - 1, size.getSelectedId() - 1);
        v.width = dims[(std::size_t) pick].first;
        v.height = dims[(std::size_t) pick].second;
        v.fps = fps.getSelectedId();
        v.crf = quality.getSelectedId();
        v.ffmpeg = ffmpeg;
        v.output = ffmpeg.existsAsFile() ? out.withFileExtension ("mp4") : out;
        scene.width = v.width;
        scene.height = v.height;
        scene.fps = v.fps;
        if (onRemember)
            onRemember (scene);
        auto* thread = new VideoExportThread (target, scene, v); // deletes itself
        closeWindow();
        thread->launchThread();
    }

    void closeWindow()
    {
        if (auto* dw = findParentComponentOfClass<juce::DialogWindow>())
            dw->exitModalState (0);
    }

    VisualTarget target;
    scene::Scene scene;
    std::function<void (const scene::Scene&)> onRemember;
    juce::ComboBox size, fps, quality;
    juce::TextButton locate, exportButton, cancel;
    juce::Label status;
    juce::File ffmpeg;
    std::vector<std::pair<int, int>> dims;
    std::unique_ptr<juce::FileChooser> chooser;
};

// ---------------------------------------------------------------------------------------------------------------
// The Visualizer's editor
// ---------------------------------------------------------------------------------------------------------------
class LayerList final : public juce::Component
{
public:
    std::function<const scene::Scene&()> sceneOf;
    std::function<void (int)> onSelect;
    std::function<void (int)> onToggle;
    int selected = 0;

    void paint (juce::Graphics& g) override
    {
        const auto& s = sceneOf();
        g.setColour (juce::Colour (0xff101216));
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        // the top of the list = the top layer (drawn last)
        for (int row = 0; row < (int) s.layers.size(); ++row)
        {
            const auto index = (int) s.layers.size() - 1 - row;
            const auto& l = s.layers[(std::size_t) index];
            auto r = rowBounds (row);
            if (index == selected)
            {
                g.setColour (theme::colour::accentMuted);
                g.fillRoundedRectangle (r.toFloat(), 3.0f);
            }
            auto eye = r.removeFromLeft (24);
            g.setColour (l.enabled ? theme::colour::ledOn : theme::colour::ledOff);
            g.fillEllipse (eye.toFloat().withSizeKeepingCentre (9.0f, 9.0f));
            auto swatch = r.removeFromRight (34).reduced (4, 6);
            g.setColour (argb (l.colourA));
            g.fillRect (swatch.removeFromLeft (swatch.getWidth() / 2));
            g.setColour (argb (l.colourB));
            g.fillRect (swatch);
            g.setColour (l.enabled ? theme::colour::textPrimary : theme::colour::textDisabled);
            g.setFont (theme::font (theme::type::small));
            auto label = tr (scene::typeName (l.type));
            if (l.type == scene::LayerType::text && ! l.text.empty())
                label << ": " << juce::String::fromUTF8 (l.text.c_str()).upToFirstOccurrenceOf ("\n", false, false);
            g.drawText (label, r, juce::Justification::centredLeft, true);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto& s = sceneOf();
        const auto row = e.y / rowH;
        if (row < 0 || row >= (int) s.layers.size())
            return;
        const auto index = (int) s.layers.size() - 1 - row;
        if (e.x < 24)
        {
            if (onToggle)
                onToggle (index);
            return;
        }
        if (onSelect)
            onSelect (index);
    }

private:
    static constexpr int rowH = 26;
    juce::Rectangle<int> rowBounds (int row) const { return { 2, row * rowH + 2, getWidth() - 4, rowH - 2 }; }
};

class VisualizerEditor final : public juce::Component,
                               private juce::ChangeListener
{
public:
    VisualizerEditor (const VisualTarget& t, bool demo) : target (t), feed (t, demo), preview (feed, [this]() -> const scene::Scene& { return scene; })
    {
        accent = juce::Colour (0xffb04cff);
        readScene();
        addAndMakeVisible (preview);
        preview.onDoubleClick = [this] { openFullScreen(); };
        ui::setHint (preview, tr ("The scene, live (double-click: full screen)"));

        list.sceneOf = [this]() -> const scene::Scene& { return scene; };
        list.onSelect = [this] (int i) {
            selected = i;
            list.selected = i;
            list.repaint();
            loadLayerControls();
        };
        list.onToggle = [this] (int i) {
            change ([i] (scene::Scene& s) { s.layers[(std::size_t) i].enabled = ! s.layers[(std::size_t) i].enabled; });
        };
        addAndMakeVisible (list);

        auto button = [this] (juce::TextButton& b, const juce::String& text, const juce::String& hint, std::function<void()> click) {
            b.setButtonText (text);
            b.onClick = std::move (click);
            ui::setHint (b, hint);
            addAndMakeVisible (b);
        };
        button (addLayer, tr ("Add"), tr ("Add a layer"), [this] { showAddMenu(); });
        button (removeLayer, tr ("Remove"), tr ("Remove the selected layer"), [this] {
            if (scene.layers.size() <= 1)
                return;
            const auto i = (std::size_t) selected;
            change ([i] (scene::Scene& s) { if (i < s.layers.size()) s.layers.erase (s.layers.begin() + (long) i); });
            select (juce::jmin (selected, (int) scene.layers.size() - 1));
        });
        button (upLayer, tr ("Up"), tr ("Move the layer up (drawn later, on top)"), [this] { move (1); });
        button (downLayer, tr ("Down"), tr ("Move the layer down"), [this] { move (-1); });
        button (duplicateLayer, tr ("Copy"), tr ("Duplicate the layer"), [this] {
            const auto i = (std::size_t) selected;
            change ([i] (scene::Scene& s) { if (i < s.layers.size()) s.layers.insert (s.layers.begin() + (long) i + 1, s.layers[i]); });
            select (selected + 1);
        });
        button (presets, tr ("Scenes"), tr ("LYNX's scenes and FL ZGameEditor presets"), [this] { showPresetMenu(); });
        button (fullScreen, tr ("Full screen"), tr ("Show the scene on the whole screen (Esc closes)"), [this] { openFullScreen(); });
        button (exportVideo, tr ("Export video..."), tr ("Render the song to a video with this scene"), [this] { openExport(); });

        // the selected layer's settings
        for (int t2 = 0; t2 < scene::layerTypeCount; ++t2)
            typeBox.addItem (tr (scene::typeName ((scene::LayerType) t2)), t2 + 1);
        typeBox.onChange = [this] {
            const auto t3 = (scene::LayerType) (typeBox.getSelectedId() - 1);
            changeLayer ([t3] (scene::Layer& l) { l.type = t3; });
            loadLayerControls();
        };
        for (int b = 0; b < scene::bandCount; ++b)
            bandBox.addItem (tr (scene::bandName ((scene::Band) b)), b + 1);
        bandBox.onChange = [this] {
            const auto b = (scene::Band) (bandBox.getSelectedId() - 1);
            changeLayer ([b] (scene::Layer& l) { l.band = b; });
        };
        ui::setHint (bandBox, tr ("What the layer reacts to"));
        for (auto* c : { &typeBox, &bandBox })
            addAndMakeVisible (*c);
        struct SliderDef { const char* name; float scene::Layer::* field; const char* hint; };
        static const SliderDef defs[] = {
            { TR_NOOP ("Opacity"), &scene::Layer::opacity, TR_NOOP ("How much of the layer shows") },
            { TR_NOOP ("X"), &scene::Layer::x, TR_NOOP ("Horizontal position") },
            { TR_NOOP ("Y"), &scene::Layer::y, TR_NOOP ("Vertical position") },
            { TR_NOOP ("Size"), &scene::Layer::size, TR_NOOP ("Size (bar height, radius, text size, image size)") },
            { TR_NOOP ("React"), &scene::Layer::react, TR_NOOP ("How much the sound moves it") },
            { TR_NOOP ("Speed"), &scene::Layer::speed, TR_NOOP ("Motion: rotation, particle speed, ring life") },
            { TR_NOOP ("Amount"), &scene::Layer::amount, TR_NOOP ("Bars: gap; waveform: thickness; particles: density; text: glow; image: spin") },
        };
        for (const auto& d : defs)
        {
            auto s = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox);
            s->setRange (0.0, 1.0);
            s->setColour (juce::Slider::trackColourId, accent);
            s->setColour (juce::Slider::thumbColourId, accent.brighter (0.3f));
            s->setDoubleClickReturnValue (true, d.field == &scene::Layer::opacity ? 1.0 : 0.5);
            s->onValueChange = [this, field = d.field, slider = s.get()] {
                if (loading)
                    return;
                const auto v = (float) slider->getValue();
                changeLayer ([field, v] (scene::Layer& l) { l.*field = v; });
            };
            ui::setHint (*s, tr (d.hint));
            addAndMakeVisible (*s);
            sliders.push_back ({ tr (d.name), d.field, std::move (s) });
        }
        count.setRange (1, 512, 1);
        count.setSliderStyle (juce::Slider::IncDecButtons);
        count.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 50, 22);
        count.onValueChange = [this] {
            if (! loading)
                changeLayer ([v = (int) count.getValue()] (scene::Layer& l) { l.count = v; });
        };
        ui::setHint (count, tr ("How many bars, rays, particles or rings"));
        addAndMakeVisible (count);
        for (auto* b : { &mirror, &additive })
        {
            addAndMakeVisible (*b);
            b->onClick = [this] {
                if (! loading)
                    changeLayer ([m = mirror.getToggleState(), a = additive.getToggleState()] (scene::Layer& l) {
                        l.mirror = m;
                        l.additive = a;
                    });
            };
        }
        mirror.setButtonText (tr ("Mirror"));
        additive.setButtonText (tr ("Glow"));
        for (int k = 0; k < 2; ++k)
        {
            auto& b = k == 0 ? colourA : colourB;
            b.onClick = [this, k] { pickColour (k); };
            ui::setHint (b, k == 0 ? tr ("Main colour") : tr ("Second colour (gradient, glow)"));
            addAndMakeVisible (b);
        }
        text.setMultiLine (true, false);
        text.setReturnKeyStartsNewLine (true);
        text.onTextChange = [this] {
            if (! loading)
                changeLayer ([v = text.getText().toStdString()] (scene::Layer& l) { l.text = v; });
        };
        addAndMakeVisible (text);
        button (chooseImage, tr ("Image..."), tr ("Choose a picture (PNG, JPG)"), [this] { pickImage(); });
        button (clearImage, tr ("No image"), tr ("Remove the picture"), [this] { changeLayer ([] (scene::Layer& l) { l.path.clear(); }); loadLayerControls(); });

        for (int p = 0; p < visualizer::count; ++p)
        {
            knobs.push_back (std::make_unique<ParamKnob> (target, EffectKind::visualizer, p, accent));
            addAndMakeVisible (*knobs.back());
        }
        if (target.engine != nullptr)
            target.engine->addChangeListener (this);
        select (juce::jmin (1, (int) scene.layers.size() - 1));
        setSize (1040, 720);
    }

    ~VisualizerEditor() override
    {
        full.reset();
        if (target.engine != nullptr)
            target.engine->removeChangeListener (this);
    }

    void paint (juce::Graphics& g) override
    {
        skin::canvas (g, getLocalBounds().toFloat(), accent);
        auto header = getLocalBounds().removeFromTop (40).toFloat();
        skin::headerBar (g, header, accent);
        skin::titleText (g, header.reduced (12.0f, 7.0f), "Visualizer", accent);
        skin::bottomBar (g, getLocalBounds().removeFromBottom (knobRow).toFloat());
        g.setFont (theme::font (theme::type::caption, true));
        g.setColour (skin::captionColour());
        g.drawText (tr ("LAYERS"), layersTitle, juce::Justification::centredLeft);
        g.drawText (tr ("SELECTED LAYER"), propsTitle, juce::Justification::centredLeft);
        g.setFont (theme::font (theme::type::small));
        for (const auto& s : sliders)
            if (s.slider->isVisible())
                g.drawText (s.name, s.slider->getBounds().withX (propsX).withWidth (70), juce::Justification::centredLeft);
        for (auto* c : { (juce::Component*) &bandBox, (juce::Component*) &count, (juce::Component*) &colourA })
            if (c->isVisible())
                g.drawText (c == &bandBox ? tr ("Reacts to") : c == &count ? tr ("Count") : tr ("Colours"), c->getBounds().withX (propsX).withWidth (70),
                            juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (40).reduced (8, 7);
        header.removeFromLeft (140);
        exportVideo.setBounds (header.removeFromRight (120));
        header.removeFromRight (6);
        fullScreen.setBounds (header.removeFromRight (100));
        header.removeFromRight (6);
        presets.setBounds (header.removeFromRight (90));

        auto knobsArea = area.removeFromBottom (knobRow).reduced (10, 6);
        for (auto& k : knobs)
            k->setBounds (knobsArea.removeFromLeft (78));

        area.reduce (8, 6);
        // right: the selected layer's settings (only the rows its type uses)
        auto props = area.removeFromRight (290);
        area.removeFromRight (10);
        propsX = props.getX();
        propsTitle = props.removeFromTop (18);
        auto line = [&props] (int h = 24) {
            auto r = props.removeFromTop (h);
            props.removeFromTop (5);
            return r;
        };
        typeBox.setBounds (line());
        {
            auto r = line();
            r.removeFromLeft (74);
            colourA.setBounds (r.removeFromLeft (60));
            r.removeFromLeft (6);
            colourB.setBounds (r.removeFromLeft (60));
        }
        bandBox.setBounds (line().withTrimmedLeft (74));
        for (auto& s : sliders)
            if (s.slider->isVisible())
                s.slider->setBounds (line (22).withTrimmedLeft (74));
        if (count.isVisible())
            count.setBounds (line().withTrimmedLeft (74).withWidth (120));
        if (mirror.isVisible() || additive.isVisible())
        {
            auto r = line();
            if (mirror.isVisible())
                mirror.setBounds (r.removeFromLeft (110));
            additive.setBounds (r.removeFromLeft (110));
        }
        if (chooseImage.isVisible())
        {
            auto r = line (26);
            chooseImage.setBounds (r.removeFromLeft (150));
            r.removeFromLeft (6);
            clearImage.setBounds (r.removeFromLeft (100));
        }
        text.setBounds (props.removeFromTop (juce::jmin (90, props.getHeight())));

        // left: the preview at the scene's aspect, the layers under it
        const auto aspect = (double) scene.width / (double) juce::jmax (1, scene.height);
        const auto previewH = juce::jlimit (120, juce::jmax (120, area.getHeight() - 150), (int) (area.getWidth() / aspect));
        preview.setBounds (area.removeFromTop (previewH));
        area.removeFromTop (6);
        layersTitle = area.removeFromTop (18);
        auto buttons = area.removeFromRight (96);
        area.removeFromRight (6);
        list.setBounds (area);
        for (auto* b : { &addLayer, &removeLayer, &duplicateLayer, &upLayer, &downLayer })
        {
            b->setBounds (buttons.removeFromTop (24));
            buttons.removeFromTop (3);
        }
    }

private:
    static constexpr int knobRow = 96;

    struct LabelledSlider
    {
        juce::String name;
        float scene::Layer::* field;
        std::unique_ptr<juce::Slider> slider;
    };

    void readScene()
    {
        const auto* fx = target.current();
        auto s = fx != nullptr ? scene::fromState (fx->state) : std::nullopt;
        scene = s ? *s : scene::defaultScene();
        if (scene.layers.empty())
            scene = scene::defaultScene();
    }

    void change (const std::function<void (scene::Scene&)>& edit)
    {
        edit (scene);
        const auto bytes = scene::toState (scene);
        target.edit ([bytes] (EffectSlot& fx) { fx.state = bytes; });
        selected = juce::jlimit (0, juce::jmax (0, (int) scene.layers.size() - 1), selected);
        list.selected = selected;
        list.repaint();
    }

    void changeLayer (const std::function<void (scene::Layer&)>& edit)
    {
        if (selected < 0 || selected >= (int) scene.layers.size())
            return;
        const auto i = (std::size_t) selected;
        change ([i, edit] (scene::Scene& s) { edit (s.layers[i]); });
        colourA.repaint();
    }

    void select (int i)
    {
        selected = juce::jlimit (0, juce::jmax (0, (int) scene.layers.size() - 1), i);
        list.selected = selected;
        list.repaint();
        loadLayerControls();
    }

    void move (int by)
    {
        const auto i = selected, j = selected + by;
        if (j < 0 || j >= (int) scene.layers.size())
            return;
        change ([i, j] (scene::Scene& s) { std::swap (s.layers[(std::size_t) i], s.layers[(std::size_t) j]); });
        select (j);
    }

    void loadLayerControls()
    {
        if (selected < 0 || selected >= (int) scene.layers.size())
            return;
        const juce::ScopedValueSetter<bool> busy (loading, true);
        const auto& l = scene.layers[(std::size_t) selected];
        typeBox.setSelectedId ((int) l.type + 1, juce::dontSendNotification);
        bandBox.setSelectedId ((int) l.band + 1, juce::dontSendNotification);
        for (auto& s : sliders)
            s.slider->setValue (l.*(s.field), juce::dontSendNotification);
        count.setValue (l.count, juce::dontSendNotification);
        mirror.setToggleState (l.mirror, juce::dontSendNotification);
        additive.setToggleState (l.additive, juce::dontSendNotification);
        if (text.getText().toStdString() != l.text)
            text.setText (juce::String::fromUTF8 (l.text.c_str()), false);
        colourA.setColour (juce::TextButton::buttonColourId, argb (l.colourA));
        colourB.setColour (juce::TextButton::buttonColourId, argb (l.colourB));
        // what this type uses
        using T = scene::LayerType;
        const auto t = l.type;
        auto show = [] (juce::Component& c, bool on) { c.setVisible (on); };
        show (count, t == T::bars || t == T::circle || t == T::particles || t == T::rings);
        show (mirror, t == T::bars || t == T::waveform);
        show (additive, t == T::circle || t == T::waveform || t == T::particles || t == T::rings);
        show (text, t == T::text);
        show (chooseImage, t == T::image || t == T::background);
        show (clearImage, (t == T::image || t == T::background) && ! l.path.empty());
        for (auto& s : sliders)
        {
            const auto f = s.field;
            bool on = true;
            if (f == &scene::Layer::x)
                on = t != T::background && t != T::bars && t != T::flash;
            else if (f == &scene::Layer::y)
                on = t != T::background && t != T::flash;
            else if (f == &scene::Layer::size)
                on = t != T::background && t != T::flash;
            else if (f == &scene::Layer::speed)
                on = t == T::circle || t == T::particles || t == T::rings;
            else if (f == &scene::Layer::amount)
                on = t == T::bars || t == T::waveform || t == T::particles || t == T::rings || t == T::text || t == T::image;
            s.slider->setVisible (on);
        }
        chooseImage.setButtonText (l.path.empty() ? tr ("Image...") : juce::File (juce::String::fromUTF8 (l.path.c_str())).getFileName());
        resized();
        repaint();
    }

    void showAddMenu()
    {
        juce::PopupMenu menu;
        for (int t = 0; t < scene::layerTypeCount; ++t)
            menu.addItem (tr (scene::typeName ((scene::LayerType) t)), [this, t] {
                scene::Layer l;
                l.type = (scene::LayerType) t;
                if (l.type == scene::LayerType::text)
                    l.text = "LYNX";
                if (l.type == scene::LayerType::flash)
                {
                    l.band = scene::Band::beat;
                    l.opacity = 0.4f;
                }
                const auto at = (std::size_t) juce::jlimit (0, (int) scene.layers.size(), selected + 1);
                change ([l, at] (scene::Scene& s) { s.layers.insert (s.layers.begin() + (long) at, l); });
                select ((int) at);
            });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addLayer));
    }

    void showPresetMenu()
    {
        juce::PopupMenu menu;
        for (const auto& p : scene::presets())
            menu.addItem (tr (juce::String (p.name)), [this, p] {
                change ([p, keep = scene] (scene::Scene& s) {
                    s = p;
                    s.width = keep.width;
                    s.height = keep.height;
                    s.fps = keep.fps;
                });
                select (juce::jmin (1, (int) scene.layers.size() - 1));
                preview.restart();
            });
        menu.addSeparator();
        menu.addItem (tr ("Import an FL ZGameEditor Visualizer preset (.fst)..."), [this] {
            chooser = std::make_unique<juce::FileChooser> (tr ("ZGameEditor Visualizer preset"), juce::File(), "*.fst");
            juce::Component::SafePointer<VisualizerEditor> safe (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
                if (safe == nullptr || ! fc.getResult().existsAsFile())
                    return;
                juce::MemoryBlock data;
                fc.getResult().loadFileAsData (data);
                const auto* p = static_cast<const std::uint8_t*> (data.getData());
                if (auto s = scene::fromZge (std::vector<std::uint8_t> (p, p + data.getSize())))
                {
                    s->name = fc.getResult().getFileNameWithoutExtension().toStdString();
                    safe->change ([s] (scene::Scene& target) { target = *s; });
                    safe->select (juce::jmin (1, (int) safe->scene.layers.size() - 1));
                }
                else
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Visualizer"),
                                                            tr ("This file is not a ZGameEditor Visualizer preset."));
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presets));
    }

    void pickColour (int which)
    {
        if (selected < 0 || selected >= (int) scene.layers.size())
            return;
        const auto& l = scene.layers[(std::size_t) selected];
        auto picker = std::make_unique<ui::ColourPicker> (argb (which == 0 ? l.colourA : l.colourB), true);
        picker->onChange = [this, which] (juce::Colour c) {
            changeLayer ([which, c] (scene::Layer& layer) { (which == 0 ? layer.colourA : layer.colourB) = c.getARGB(); });
            (which == 0 ? colourA : colourB).setColour (juce::TextButton::buttonColourId, c);
        };
        picker->setSize (ui::ColourPicker::preferredWidth, picker->preferredHeight());
        juce::CallOutBox::launchAsynchronously (std::move (picker), (which == 0 ? colourA : colourB).getScreenBounds(), nullptr);
    }

    void pickImage()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Choose a picture"), juce::File(), "*.png;*.jpg;*.jpeg;*.gif");
        juce::Component::SafePointer<VisualizerEditor> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile())
                return;
            safe->changeLayer ([path = fc.getResult().getFullPathName().toStdString()] (scene::Layer& l) { l.path = path; });
            safe->loadLayerControls();
        });
    }

    void openFullScreen()
    {
        if (full != nullptr)
            return;
        const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (getScreenBounds());
        const auto screen = display != nullptr ? display->totalArea : juce::Rectangle<int> (0, 0, 1280, 720);
        full = std::make_unique<FullScreenView> (feed, [this]() -> const scene::Scene& { return scene; }, screen);
        juce::Component::SafePointer<VisualizerEditor> safe (this);
        full->onClose = [safe] {
            if (safe != nullptr)
                safe->full.reset();
        };
    }

    void openExport()
    {
        juce::DialogWindow::LaunchOptions o;
        o.dialogTitle = tr ("Export video");
        o.content.setOwned (new VideoExportPanel (target, scene, [safe = juce::Component::SafePointer<VisualizerEditor> (this)] (const scene::Scene& s) {
            if (safe != nullptr)
                safe->change ([s] (scene::Scene& target) {
                    target.width = s.width;
                    target.height = s.height;
                    target.fps = s.fps;
                });
        }));
        o.dialogBackgroundColour = theme::colour::bg2;
        o.useNativeTitleBar = true;
        o.resizable = false;
        o.componentToCentreAround = this;
        o.launchAsync();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        // undo / redo / a preset loaded elsewhere: take the slot's scene
        const auto* fx = target.current();
        if (fx == nullptr)
            return;
        if (const auto s = scene::fromState (fx->state); s && ! (*s == scene))
        {
            scene = *s;
            select (selected);
        }
        for (auto& k : knobs)
            k->repaint();
    }

    VisualTarget target;
    TapFeed feed;
    scene::Scene scene;
    juce::Colour accent;
    ScenePreview preview;
    LayerList list;
    int selected = 0, propsX = 0;
    bool loading = false;
    juce::Rectangle<int> layersTitle, propsTitle;
    juce::TextButton addLayer, removeLayer, upLayer, downLayer, duplicateLayer, presets, fullScreen, exportVideo, chooseImage, clearImage;
    juce::TextButton colourA, colourB;
    juce::ComboBox typeBox, bandBox;
    std::vector<LabelledSlider> sliders;
    juce::Slider count;
    juce::ToggleButton mirror, additive;
    juce::TextEditor text;
    std::vector<std::unique_ptr<ParamKnob>> knobs;
    std::unique_ptr<FullScreenView> full;
    std::unique_ptr<juce::FileChooser> chooser;
};

std::unique_ptr<juce::Component> makeEditorFor (const VisualTarget& target, EffectKind kind, bool demo)
{
    if (kind == EffectKind::waveCandy)
        return std::make_unique<WaveCandyEditor> (target, demo);
    return std::make_unique<VisualizerEditor> (target, demo);
}

// ---------------------------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------------------------
class VisualWindow final : public juce::DocumentWindow
{
public:
    VisualWindow (const VisualTarget& target, EffectKind kind, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg2, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (makeEditorFor (target, kind, false).release(), true);
        setResizable (true, false);
        setResizeLimits (kind == EffectKind::waveCandy ? 300 : 900, kind == EffectKind::waveCandy ? 260 : 620, 4000, 3000);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
        if (target.track >= 0 && target.engine != nullptr)
            gone = std::make_unique<ui::SlotGoneWatcher> (*target.engine, target.track, target.slot, [this] { closeButtonPressed(); });
    }

    void closeButtonPressed() override;

private:
    std::unique_ptr<ui::SlotGoneWatcher> gone;
};

std::map<juce::String, std::unique_ptr<VisualWindow>>& visualWindows()
{
    static std::map<juce::String, std::unique_ptr<VisualWindow>> open;
    return open;
}

void VisualWindow::closeButtonPressed()
{
    for (auto& [key, window] : visualWindows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { visualWindows().erase (k); });
            return;
        }
}

void openWindow (const VisualTarget& target, EffectKind kind, const juce::String& title, const juce::String& key)
{
    auto& open = visualWindows();
    if (auto it = open.find (key); it != open.end())
    {
        it->second->toFront (true);
        return;
    }
    open[key] = std::make_unique<VisualWindow> (target, kind, title);
}
} // namespace

namespace VisualWindows
{
bool handles (EffectKind kind)
{
    return isVisualKind (kind);
}

void open (AudioEngine& engine, int track, int slot)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;
    const auto kind = mixer[(std::size_t) track].effects[(std::size_t) slot].kind;
    if (! isVisualKind (kind))
        return;
    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    const auto title = juce::String (effectInfo (kind).name) + " - " + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " ("
                       + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    openWindow (VisualTarget::mixerSlot (engine, track, slot), kind, title,
                "mixer-" + juce::String (track) + "-" + juce::String (slot) + "-" + juce::String ((int) kind));
}

void openNode (const VisualTarget& target, const juce::String& title, const juce::String& key)
{
    const auto* fx = target.current();
    if (fx == nullptr)
        return;
    openWindow (target, fx->kind, title, key);
}

void closeAll()
{
    visualWindows().clear();
}
} // namespace VisualWindows

std::unique_ptr<juce::Component> makeVisualEditor (AudioEngine& engine, int track, int slot, bool demo)
{
    auto target = VisualTarget::mixerSlot (engine, track, slot);
    const auto* fx = target.current();
    return makeEditorFor (target, fx != nullptr ? fx->kind : EffectKind::visualizer, demo);
}

// ---------------------------------------------------------------------------------------------------------------
// --visualtest
// ---------------------------------------------------------------------------------------------------------------
namespace
{
// the audio thread's cost of a visual slot: 60 s of stereo noise + tones through it in 256-frame blocks
juce::String audioPathCost()
{
    juce::String out;
    constexpr double rate = 48000.0;
    constexpr int block = 256, seconds = 60;
    std::vector<float> l (block), r (block);
    auto run = [&] (EffectKind kind, bool armed) {
        auto fx = makeEffect (kind);
        Capture capture;
        if (armed)
        {
            capture.reserve ((std::size_t) (rate * seconds) + block);
            setCapture (fx.get(), &capture);
        }
        fx->prepare (rate);
        fx->setPlaying (true);
        EffectParams p {};
        const auto& info = effectInfo (kind);
        for (std::size_t i = 0; i < info.params.size(); ++i)
            p[i] = info.params[i].defaultValue;
        std::uint32_t seed = 1;
        const auto blocks = (int) (rate * seconds) / block;
        double best = 1e30;
        for (int pass = 0; pass < 3; ++pass)
        {
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            for (int b = 0; b < blocks; ++b)
            {
                for (int i = 0; i < block; ++i)
                {
                    seed = seed * 1664525u + 1013904223u;
                    l[(std::size_t) i] = ((float) (seed >> 9) / 8388608.0f - 1.0f) * 0.3f;
                    r[(std::size_t) i] = l[(std::size_t) i] * 0.5f + 0.2f * std::sin ((float) (b * block + i) * 0.05f);
                }
                fx->process (l.data(), r.data(), block, p, 120.0);
            }
            best = juce::jmin (best, juce::Time::getMillisecondCounterHiRes() - t0);
            if (armed)
                break; // the capture is full after one pass
        }
        if (armed)
            setCapture (fx.get(), nullptr);
        return best;
    };
    // the reference: the same loop generating the signal, through a Fruity Balance-like pass-through (balance)
    const auto base = run (EffectKind::fruityBalance, false);
    for (const auto kind : { EffectKind::waveCandy, EffectKind::visualizer })
        for (const auto armed : { false, true })
        {
            const auto ms = run (kind, armed);
            out << "audio_path " << effectInfo (kind).name << (armed ? " (export capture armed)" : "") << ": " << juce::String (ms, 1) << " ms for "
                << seconds << " s = " << juce::String (ms / (seconds * 10.0), 4) << " % of real time (reference loop " << juce::String (base, 1)
                << " ms; added " << juce::String ((ms - base) / (seconds * 10.0), 4) << " %)\n";
        }
    return out;
}

// the drawing cost of each layer type alone and of each built-in scene (1280 x 720, demo signal, 120 frames)
juce::String drawingCost()
{
    juce::String out;
    juce::Image image (juce::Image::ARGB, 1280, 720, true, juce::SoftwareImageType());
    std::vector<float> l (8192), r (8192);
    auto run = [&] (const scene::Scene& sc) {
        FrameAnalyzer analyzer;
        SceneRenderer renderer;
        std::array<float, shownParams> params {};
        for (int i = 0; i < visualizer::count; ++i)
            params[(std::size_t) i] = effectInfo (EffectKind::visualizer).params[(std::size_t) i].defaultValue;
        double ms = 0.0;
        for (int k = 0; k < 120; ++k)
        {
            const auto at = (std::int64_t) k * 1600;
            for (int i = 0; i < 4096; ++i)
            {
                l[(std::size_t) i] = demoSample (at + i, 48000.0, 0);
                r[(std::size_t) i] = demoSample (at + i, 48000.0, 1);
            }
            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            const auto& f = analyzer.analyze (l.data(), r.data(), 4096, 48000.0, k / 30.0, 1.0 / 30.0, params);
            renderer.advance (sc, f, 1280.0f / 720.0f);
            juce::Graphics g (image);
            renderer.draw (g, image.getBounds().toFloat(), sc, f);
            ms += juce::Time::getMillisecondCounterHiRes() - t0;
        }
        return ms / 120.0;
    };
    for (int t = 0; t < scene::layerTypeCount; ++t)
    {
        scene::Scene sc;
        scene::Layer layer;
        layer.type = (scene::LayerType) t;
        layer.text = "LYNX";
        layer.additive = true;
        layer.count = 64;
        sc.layers.push_back (layer);
        out << "draw layer " << scene::typeName (layer.type) << ": " << juce::String (run (sc), 2) << " ms per 1280x720 frame\n";
    }
    for (const auto& preset : scene::presets())
        out << "draw scene " << preset.name << ": " << juce::String (run (preset), 2) << " ms per 1280x720 frame\n";
    return out;
}

class VisualTestRun final : private juce::Timer
{
public:
    struct Case
    {
        juce::String name;
        EffectKind kind;
        std::function<void (EffectSlot&)> setup;
        int w, h;
        bool demo = true;
    };

    VisualTestRun (juce::File f, juce::File p, std::function<void (bool)> d) : folder (std::move (f)), project (std::move (p)), done (std::move (d))
    {
        folder.createDirectory();
        report << audioPathCost() << drawingCost();
        for (int m = 1; m < wc::modeCount; ++m)
            cases.push_back ({ juce::String ("wavecandy-") + wc::modeName ((wc::Mode) m), EffectKind::waveCandy,
                               [m] (EffectSlot& fx) { fx.params[wc::mode] = wc::modeValue ((wc::Mode) m); }, 760, 480 });
        cases.push_back ({ "wavecandy-auto-tall", EffectKind::waveCandy, [] (EffectSlot& fx) { fx.params[wc::mode] = wc::modeValue (wc::Mode::automatic); },
                           300, 720 });
        for (const auto& preset : scene::presets())
            cases.push_back ({ juce::String ("visualizer-") + juce::String (preset.name), EffectKind::visualizer,
                               [preset] (EffectSlot& fx) { fx.state = scene::toState (preset); }, 1040, 720 });
        // idle (no audio reaches the slot): a settled display is not redrawn
        for (auto c : std::vector<Case> (cases.begin(), cases.end()))
            if (c.name == "wavecandy-Spectrum" || c.name == "wavecandy-Spectrogram" || c.name == "wavecandy-Peak meter" || c.name == "visualizer-Neon bars")
            {
                c.name += " (idle)";
                c.demo = false;
                cases.push_back (c);
            }
        if (juce::SystemStats::getEnvironmentVariable ("LYNX_VISUALTEST_VIDEO_ONLY", {}).isNotEmpty())
            cases.clear();
        engine = std::make_unique<AudioEngine> (false);
        report << "case\tcpu % of one core (animating: demo signal; idle: no audio)\n";
        open();
        startTimer (100);
    }

private:
    juce::String slug (juce::String s) const { return s.toLowerCase().replaceCharacters (" /\\", "---"); }

    void open()
    {
        window.reset();
        if (at >= cases.size())
        {
            stopTimer();
            exportVideos();
            return;
        }
        const auto& c = cases[at];
        Song song;
        song.mixer.resize (2);
        song.mixer[0].name = "Master";
        song.mixer[1].name = "Insert 1";
        auto fx = makeEffectSlot (c.kind);
        c.setup (fx);
        song.mixer[1].effects.push_back (fx);
        engine->setSong (std::move (song));
        engine->dispatchPendingMessages();
        window = std::make_unique<juce::DocumentWindow> ("LYNX visual test - " + c.name, theme::colour::bg1, 0);
        window->setUsingNativeTitleBar (true);
        window->setContentOwned (makeVisualEditor (*engine, 1, 0, c.demo).release(), true);
        window->getContentComponent()->setSize (c.w, c.h);
        window->setContentComponentSize (c.w, c.h);
        window->setTopLeftPosition (40, 40);
        window->setVisible (true);
        phase = 0;
        t0 = juce::Time::getMillisecondCounterHiRes();
    }

    void timerCallback() override
    {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        if (phase == 0 && now - t0 > 2500.0)
        {
            const auto& c = cases[at];
            auto* content = window->getContentComponent();
            const auto image = content->createComponentSnapshot (content->getLocalBounds(), true, 1.0f);
            juce::FileOutputStream out (folder.getChildFile (slug (c.name.replace (" (idle)", "-idle")) + ".png"));
            if (out.openedOk())
            {
                out.setPosition (0);
                out.truncate();
                juce::PNGImageFormat().writeImageToStream (image, out);
            }
            phase = 1;
            t0 = now;
            c0 = processCpuSeconds();
            return;
        }
        if (phase == 3)
        {
            if (now - t0 > 3000.0)
            {
                stopTimer();
                // (diagnostics: a plain audio export as the Export window runs it — a worker thread, the message loop free)
                exportopts::Options o;
                o.sampleRate = 48000.0;
                plainJob = std::make_unique<ExportJob> (engine->getSong(), o, folder.getChildFile ("plain.wav"));
                finished = false;
                worker = std::thread ([this] {
                    juce::String error;
                    jobResult = plainJob->run ([] (double) { return true; }, error);
                    finished = true;
                });
                phase = 4;
                startTimer (100);
                return;
            }
            return;
        }
        if (phase == 4)
        {
            if (! finished)
                return;
            stopTimer();
            worker.join();
            plainJob.reset();
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (folder.getChildFile ("plain.wav")));
            juce::int64 first = -1;
            if (reader != nullptr)
            {
                juce::AudioBuffer<float> b (2, (int) reader->lengthInSamples);
                reader->read (&b, 0, (int) reader->lengthInSamples, 0, true, true);
                for (int k = 0; k < b.getNumSamples() && first < 0; ++k)
                    if (std::abs (b.getSample (0, k)) > 1.0e-3f)
                        first = k;
            }
            report << "plain export (worker thread): " << (jobResult ? "OK" : "ERROR") << ", first sound at " << juce::String ((double) first / 48000.0, 3) << " s\n";
            videoOk = videoOk && jobResult && first >= 0 && first < 4800; // (this project starts at once: before round 164c the
                                                                        // Export window's thread lost its first ~0.05–2 s)
            {
                nextVideo();
            }
            return;
        }
        if (phase == 2)
        {
            if (finished)
            {
                stopTimer();
                videoDone();
            }
            return;
        }
        if (phase == 1 && now - t0 > 5000.0)
        {
            const auto cpu = (processCpuSeconds() - c0) / ((now - t0) * 0.001) * 100.0;
            report << cases[at].name << "\t" << juce::String (cpu, 2) << "\n";
            ++at;
            open();
        }
    }

    // the video export of a project (an FL ZGameEditor demo by default): with ffmpeg, and as images without it
    void exportVideos()
    {
        auto file = project;
        if (! file.existsAsFile())
        {
            const auto roots = findFlStudioRoots();
            const auto dir = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Demo projects/Visual/ZGameEditor Visualizer");
            auto found = dir.findChildFiles (juce::File::findFiles, false, "*.flp");
            found.sort();
            if (! found.isEmpty())
                file = found[0];
        }
        juce::String error;
        if (! file.existsAsFile() || ! engine->importFlpFile (file, error))
        {
            report << "video: no project (" << error << ")\n";
            finish (true);
            return;
        }
        engine->dispatchPendingMessages();
        report << "video project: " << file.getFileName() << "\n";
        // the first visual slot in the mixer
        VisualTarget target;
        const auto& song = engine->getSong();
        for (int t = 0; t < (int) song.mixer.size() && target.engine == nullptr; ++t)
            for (int s = 0; s < (int) song.mixer[(std::size_t) t].effects.size(); ++s)
                if (song.mixer[(std::size_t) t].effects[(std::size_t) s].kind == EffectKind::visualizer)
                {
                    target = VisualTarget::mixerSlot (*engine, t, s);
                    report << "visualizer on insert " << t << " slot " << (s + 1) << "\n";
                    break;
                }
        if (target.engine == nullptr)
        {
            report << "video: the project has no Visualizer\n";
            finish (false);
            return;
        }
        const auto* fx = target.current();
        auto sc = scene::fromState (fx->state).value_or (scene::defaultScene());
        report << "scene: " << (int) sc.layers.size() << " layers, " << sc.width << "x" << sc.height << " @ " << sc.fps << "\n";
        for (const auto& l : sc.layers)
            report << "  layer " << scene::typeName (l.type) << (l.text.empty() ? "" : " \"" + juce::String::fromUTF8 (l.text.c_str()) + "\"") << "\n";
        videoTarget = target;
        videoScene = sc;
        // (as a person would: the project has been open a moment before the export)
        phase = 3;
        t0 = juce::Time::getMillisecondCounterHiRes();
        startTimer (100);
    }

    // one export on a worker thread; the timer waits for it (the export engine was built here, on the message thread)
    void nextVideo()
    {
        while (videoStep < 3)
        {
            const auto withFfmpeg = videoStep == 0;
            const auto onMaster = videoStep == 2; // the capture's alignment: a Visualizer last on the master hears the WAV itself
            ++videoStep;
            if (onMaster)
            {
                auto slot = makeEffectSlot (EffectKind::visualizer);
                slot.state = scene::toState (videoScene);
                engine->editSong ([slot] (Song& song) {
                    if (! song.mixer.empty())
                        song.mixer[0].effects.push_back (slot);
                }, false);
                engine->dispatchPendingMessages();
                videoTarget = VisualTarget::mixerSlot (*engine, 0, (int) engine->getSong().mixer[0].effects.size() - 1);
            }
            VideoSettings v;
            v.width = 1280;
            v.height = 720;
            v.fps = 30;
            v.maxSeconds = withFfmpeg ? 12.0 : onMaster ? 0.5 : 2.0;
            v.ffmpeg = withFfmpeg ? findFfmpeg() : juce::File();
            if (withFfmpeg && ! v.ffmpeg.existsAsFile())
            {
                report << "video (ffmpeg): ffmpeg not found, skipped\n";
                continue;
            }
            v.output = folder.getChildFile (withFfmpeg ? "video.mp4" : onMaster ? "master" : "video");
            videoLabel = withFfmpeg ? "ffmpeg" : onMaster ? "master check" : "images";
            job = std::make_unique<VideoJob> (videoTarget, videoScene, v);
            job->diagnose = true;
            finished = false;
            worker = std::thread ([this] {
                jobResult = job->run ([] (double, const juce::String&) { return true; });
                finished = true;
            });
            phase = 2;
            t0 = juce::Time::getMillisecondCounterHiRes();
            startTimer (100);
            return;
        }
        stopTimer();
        finish (videoOk);
    }

    void videoDone()
    {
        worker.join();
        report << "video (" << videoLabel << "): " << (jobResult ? "OK " : "ERROR ") << job->error << " -> " << job->result.getFullPathName()
               << " frames=" << job->frames << " capture=" << (job->usedCapture ? 1 : 0) << " (" << job->captureNote << ")" << " render_ms=" << juce::roundToInt (job->renderMs)
               << " draw_ms=" << juce::roundToInt (job->drawMs) << " ms_per_frame=" << juce::String (job->frames > 0 ? job->drawMs / job->frames : 0.0, 2)
               << " bytes="
               << (job->result.existsAsFile() ? job->result.getSize() : (juce::int64) job->result.getNumberOfChildFiles (juce::File::findFiles)) << "\n";
        videoOk = videoOk && jobResult;
        job.reset();
        nextVideo();
    }

    void finish (bool ok)
    {
        report << "result=" << (ok ? "OK" : "ERROR") << "\n";
        folder.getChildFile ("report.txt").replaceWithText (report);
        juce::MessageManager::callAsync ([d = done, ok] { d (ok); });
    }

    juce::File folder, project;
    std::function<void (bool)> done;
    std::vector<Case> cases;
    std::size_t at = 0;
    int phase = 0;
    double t0 = 0.0, c0 = 0.0;
    juce::String report;
    std::unique_ptr<AudioEngine> engine;
    std::unique_ptr<juce::DocumentWindow> window;
    VisualTarget videoTarget;
    scene::Scene videoScene;
    std::unique_ptr<VideoJob> job;
    std::unique_ptr<ExportJob> plainJob;
    std::thread worker;
    std::atomic<bool> finished { false };
    bool jobResult = false, videoOk = true;
    int videoStep = 0;
    juce::String videoLabel;
};
} // namespace

void runVisualTest (const juce::File& folder, const juce::File& project, std::function<void (bool)> done)
{
    static std::unique_ptr<VisualTestRun> running;
    running = std::make_unique<VisualTestRun> (folder, project, [d = std::move (done)] (bool ok) {
        juce::MessageManager::callAsync ([] { running.reset(); });
        d (ok);
    });
}
} // namespace lynx
