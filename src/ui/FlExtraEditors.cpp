#include "ui/FlExtraEditors.h"

#include <algorithm>
#include <cmath>
#include <map>

#include <juce_audio_formats/juce_audio_formats.h>

#include "core/Effects.h"
#include "core/FlExtraFx.h"
#include "core/Scratcher.h"
#include "core/Transmitter.h"
#include "core/VisualFx.h"
#include "engine/AudioEngine.h"
#include "platform/Paths.h"
#include "ui/BrowserComponent.h"
#include "ui/I18n.h"
#include "ui/PluginEditors.h"
#include "ui/PluginSkin.h"
#include "ui/SlotWatch.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
namespace fx = flextra;

bool isExtraKind (EffectKind k)
{
    return k == EffectKind::bigClock || k == EffectKind::tuner || k == EffectKind::htmlNotebook || k == EffectKind::scratcher;
}

VisualTarget slotTarget (AudioEngine& engine, int track, int slot)
{
    VisualTarget t;
    t.engine = &engine;
    t.track = track;
    t.slot = slot;
    t.locate = [track, slot] (Song& song) -> EffectSlot* {
        if (track < 0 || track >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) track].effects.size())
            return nullptr;
        auto& e = song.mixer[(std::size_t) track].effects[(std::size_t) slot];
        return isExtraKind (e.kind) ? &e : nullptr;
    };
    t.running = [&engine, track, slot] { return engine.getEffect (track, slot); };
    t.runningIn = [track, slot] (AudioEngine& e) { return e.getEffect (track, slot); };
    return t;
}

// ------------------------------------------------------------------------------------------------------------
// A knob bound to one of the slot's parameters (LYNX's control contract; automatable on a mixer slot)
// ------------------------------------------------------------------------------------------------------------
class Knob final : public juce::Component
{
public:
    Knob (const VisualTarget& t, EffectKind k, int p) : target (t), param (p)
    {
        const auto& info = effectInfo (k);
        spec.name = tr (juce::String (info.params[(std::size_t) param].name));
        spec.defaultValue = info.params[(std::size_t) param].defaultValue;
        spec.toText = [k, p] (double v) { return tr (juce::String (formatEffectParam (k, p, (float) v, 120.0))); };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        if (target.track >= 0)
            spec.target = AutomationTarget { AutomationTargetKind::effectParam, target.track, target.slot, param };
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, spec.name, true, spec.target.has_value());
    }

    double value() const
    {
        const auto* e = target.current();
        return e != nullptr ? (double) e->params[(std::size_t) param] : spec.defaultValue;
    }

    void set (double v)
    {
        target.edit ([p = param, v] (EffectSlot& e) { e.params[(std::size_t) p] = (float) juce::jlimit (0.0, 1.0, v); });
        repaint();
        if (auto* parent = getParentComponent())
            parent->repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto label = area.removeFromBottom (13.0f);
        const auto valueArea = area.removeFromBottom (13.0f);
        const auto size = juce::jmin (area.getWidth(), area.getHeight());
        skin::KnobLook look;
        look.value = (float) value();
        look.accent = theme::colour::accent;
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
    int param;
    ui::ValueSpec spec;
    ui::ValueDrag drag;
};

// two-state switch bound to a parameter (0 / 1), with the label of each state
class Switch final : public juce::Component
{
public:
    Switch (const VisualTarget& t, int p, juce::String offText, juce::String onText, juce::String caption)
        : target (t), param (p), off (std::move (offText)), on (std::move (onText)), name (std::move (caption))
    {
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, name + ": " + off + " / " + on);
    }

    bool state() const
    {
        const auto* e = target.current();
        return e != nullptr && e->params[(std::size_t) param] >= 0.5f;
    }

    void paint (juce::Graphics& g) override
    {
        auto area = getLocalBounds().toFloat();
        const auto caption = area.removeFromBottom (13.0f);
        auto box = area.reduced (2.0f, 4.0f);
        const auto half = box.getWidth() / 2.0f;
        const auto s = state();
        skin::button (g, box.withWidth (half), ! s, isMouseOver(), theme::colour::accent);
        skin::button (g, box.withTrimmedLeft (half), s, isMouseOver(), theme::colour::accent);
        g.setFont (theme::font (theme::type::small, true));
        g.setColour (! s ? theme::colour::textPrimary : theme::colour::textSecondary);
        g.drawFittedText (off, box.withWidth (half).toNearestInt(), juce::Justification::centred, 1);
        g.setColour (s ? theme::colour::textPrimary : theme::colour::textSecondary);
        g.drawFittedText (on, box.withTrimmedLeft (half).toNearestInt(), juce::Justification::centred, 1);
        g.setFont (theme::font (theme::type::caption));
        g.setColour (skin::captionColour());
        g.drawText (name, caption, juce::Justification::centred);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        const auto v = e.position.x >= (float) getWidth() / 2.0f ? 1.0f : 0.0f;
        target.edit ([p = param, v] (EffectSlot& s) { s.params[(std::size_t) p] = v; });
        if (auto* parent = getParentComponent())
            parent->repaint();
    }

private:
    VisualTarget target;
    int param;
    juce::String off, on, name;
};

// ------------------------------------------------------------------------------------------------------------
// Fruity Big Clock
// ------------------------------------------------------------------------------------------------------------
class BigClockEditor final : public juce::Component, private juce::Timer
{
public:
    BigClockEditor (const VisualTarget& t, bool demoMode) : target (t), demo (demoMode)
    {
        setSize (420, 150);
        ui::setHint (*this, tr ("Song position") + "  " + tr ("[RMB] display: Bar:Beat:Tick, Min:Sec:Hundredths or Bar:Step:Tick"));
        if (! demo)
            startTimerHz (30);
        refresh();
    }

    void setDemoTicks (double ticks)
    {
        demoTicks = ticks;
        refresh();
    }

    fx::bigclock::Display display() const
    {
        const auto* e = target.current();
        return fx::bigclock::displayOf (e != nullptr ? e->params[fx::bigclock::display] : 0.0f);
    }

    void paint (juce::Graphics& g) override
    {
        const auto area = getLocalBounds().toFloat();
        g.setColour (theme::colour::bg0);
        g.fillRect (area);
        auto inner = area.reduced (8.0f);
        g.setColour (theme::colour::bg1);
        g.fillRoundedRectangle (inner, theme::radius::lg);
        g.setColour (theme::colour::border);
        g.drawRoundedRectangle (inner.reduced (0.5f), theme::radius::lg, 1.0f);

        const auto captionArea = inner.removeFromBottom (juce::jmax (14.0f, inner.getHeight() * 0.14f));
        const auto digits = inner.reduced (10.0f, 4.0f);
        // the biggest monospaced size that fits the box (the digits never jump while they change)
        const auto chars = (float) juce::jmax (8, shown.length());
        const auto size = juce::jmin (digits.getHeight() * 0.92f, digits.getWidth() / (chars * 0.62f));
        g.setFont (theme::monoFont (size).withStyle ("Bold"));
        g.setColour (theme::colour::textPrimary);
        g.drawText (shown, digits, juce::Justification::centred, false);

        g.setFont (theme::font (juce::jmin (13.0f, captionArea.getHeight() * 0.8f)));
        g.setColour (theme::colour::textDisabled);
        g.drawText (tr (fx::bigclock::displayName (display())).toUpperCase(), captionArea, juce::Justification::centred, false);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        if (! e.mods.isPopupMenu())
        {
            // a click steps to the next display, as FL's clock does
            setDisplay ((fx::bigclock::Display) (((int) display() + 1) % fx::bigclock::displays));
            return;
        }
        juce::PopupMenu m;
        for (int d = 0; d < fx::bigclock::displays; ++d)
            m.addItem (d + 1, tr (fx::bigclock::displayName ((fx::bigclock::Display) d)), true, (int) display() == d);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [safe = juce::Component::SafePointer (this)] (int r) {
            if (safe != nullptr && r > 0)
                safe->setDisplay ((fx::bigclock::Display) (r - 1));
        });
    }

private:
    void setDisplay (fx::bigclock::Display d)
    {
        target.edit ([d] (EffectSlot& s) { s.params[fx::bigclock::display] = fx::bigclock::displayValue (d); });
        refresh();
    }

    void timerCallback() override { refresh(); }

    void refresh()
    {
        if (target.engine == nullptr)
            return;
        const auto& song = target.engine->getSong();
        double ticks = demoTicks;
        if (! demo)
        {
            const auto length = (double) juce::jmax<std::int64_t> (1, song.mode == PlayMode::song ? song.songLength() : (std::int64_t) ppq * 4 * 1000);
            ticks = std::fmod (juce::jmax (0.0, target.engine->getTransport().positionTicks.load()), length);
        }
        const auto text = juce::String (fx::bigclock::clockText (song, ticks, target.engine->getTransport().beatsPerBar.load(), display()));
        if (text != shown || display() != shownDisplay)
        {
            shown = text;
            shownDisplay = display();
            repaint();
        }
    }

    VisualTarget target;
    bool demo;
    double demoTicks = 0.0;
    juce::String shown;
    fx::bigclock::Display shownDisplay = fx::bigclock::Display::barBeatTick;
};

// ------------------------------------------------------------------------------------------------------------
// Tuner
// ------------------------------------------------------------------------------------------------------------
class TunerEditor final : public juce::Component, private juce::Timer
{
public:
    TunerEditor (const VisualTarget& t, bool demoMode)
        : target (t), demo (demoMode), reference (t, EffectKind::tuner, fx::tuner::reference),
          reactivity (t, EffectKind::tuner, fx::tuner::reactivity),
          bass (t, fx::tuner::bass, tr ("Off"), tr ("On"), tr ("Bass")),
          visual (t, fx::tuner::visual, tr ("Basic"), tr ("Dial"), tr ("Visual"))
    {
        for (auto* c : std::initializer_list<juce::Component*> { &reference, &reactivity, &bass, &visual })
            addAndMakeVisible (*c);
        setSize (360, 330);
        analyzer.prepare (48000.0);
        if (demo)
            feedDemo (442.3, 1.0);
        else
            startTimerHz (30);
    }

    // a test tone straight into the analyser (snapshots)
    void feedDemo (double hz, double seconds)
    {
        applyOptions();
        analyzer.prepare (48000.0);
        applyOptions();
        std::vector<float> block (480);
        double phase = 0.0;
        for (int b = 0; b < (int) (seconds * 100.0); ++b)
        {
            for (auto& x : block)
            {
                x = (float) (0.4 * std::sin (phase) + 0.15 * std::sin (2.0 * phase) + 0.08 * std::sin (3.0 * phase));
                phase += 2.0 * juce::MathConstants<double>::pi * hz / 48000.0;
            }
            analyzer.push (block.data(), (int) block.size());
        }
        reading = analyzer.reading();
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        auto controls = area.removeFromBottom (78);
        const auto w = controls.getWidth() / 4;
        reference.setBounds (controls.removeFromLeft (w).reduced (4, 0));
        reactivity.setBounds (controls.removeFromLeft (w).reduced (4, 0));
        bass.setBounds (controls.removeFromLeft (w).reduced (4, 20).withTrimmedTop (-20));
        visual.setBounds (controls.reduced (4, 20).withTrimmedTop (-20));
        display = area.toFloat().withTrimmedBottom (6.0f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (display, theme::radius::lg);
        const auto* e = target.current();
        const auto dial = e != nullptr && fx::tuner::visualOf (e->params[fx::tuner::visual]) == fx::tuner::Visual::dial;
        if (dial)
            paintDial (g, display.reduced (10.0f));
        else
            paintBasic (g, display.reduced (10.0f));
    }

private:
    juce::Colour offsetColour (double cents) const
    {
        const auto a = std::abs (cents);
        return a <= 3.0 ? theme::colour::ledOn : a <= 15.0 ? theme::colour::meterMid : theme::colour::accent;
    }

    void paintNote (juce::Graphics& g, juce::Rectangle<float> area, float size) const
    {
        const auto name = reading.valid ? juce::String (fx::tuner::noteName (reading.note)) : juce::String ("-");
        const auto octave = reading.valid ? juce::String (fx::tuner::octaveOf (reading.note)) : juce::String();
        g.setColour (reading.valid ? theme::colour::textPrimary : theme::colour::textDisabled);
        g.setFont (theme::font (size, true));
        const auto nameWidth = juce::GlyphArrangement::getStringWidth (juce::Font (theme::font (size, true)), name);
        const auto octaveSize = size * 0.45f;
        const auto octaveWidth = octave.isEmpty() ? 0.0f : juce::GlyphArrangement::getStringWidth (juce::Font (theme::font (octaveSize, true)), octave);
        const auto x = area.getCentreX() - (nameWidth + octaveWidth) / 2.0f;
        g.drawText (name, juce::Rectangle<float> (x, area.getY(), nameWidth + 2.0f, area.getHeight()), juce::Justification::centredLeft, false);
        if (octave.isNotEmpty())
        {
            g.setFont (theme::font (octaveSize, true));
            g.setColour (theme::colour::textSecondary);
            g.drawText (octave, juce::Rectangle<float> (x + nameWidth, area.getCentreY(), octaveWidth + 4.0f, size * 0.45f),
                        juce::Justification::centredLeft, false);
        }
    }

    juce::String hzLine() const
    {
        if (! reading.valid)
            return "0 Hz   +0 " + tr ("cent");
        const auto c = (int) std::lround (reading.cents);
        return juce::String (reading.hz, 1) + " Hz   " + (c >= 0 ? "+" : "") + juce::String (c) + " " + tr ("cent");
    }

    void paintBasic (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        auto top = area.removeFromTop (area.getHeight() * 0.62f);
        paintNote (g, top, juce::jmin (top.getHeight() * 0.8f, 96.0f));
        g.setFont (theme::font (theme::type::body));
        g.setColour (theme::colour::textSecondary);
        g.drawText (hzLine(), area.removeFromTop (20.0f), juce::Justification::centred, false);

        // the offset bar: −50 … +50 cents, the centre marked, a line at the detected offset
        auto bar = area.reduced (14.0f, 8.0f).withSizeKeepingCentre (area.getWidth() - 28.0f, 18.0f);
        g.setColour (theme::colour::bg3);
        g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (theme::colour::borderStrong);
        for (int c = -50; c <= 50; c += 10)
        {
            const auto x = bar.getX() + bar.getWidth() * (float) (c + 50) / 100.0f;
            g.drawVerticalLine ((int) x, bar.getY() + (c == 0 ? 0.0f : 5.0f), bar.getBottom() - (c == 0 ? 0.0f : 5.0f));
        }
        if (reading.valid)
        {
            const auto x = bar.getX() + bar.getWidth() * (float) (reading.cents + 50.0) / 100.0f;
            g.setColour (offsetColour (reading.cents));
            g.fillRoundedRectangle (juce::Rectangle<float> (x - 2.0f, bar.getY() - 4.0f, 4.0f, bar.getHeight() + 8.0f), 2.0f);
        }
    }

    void paintDial (juce::Graphics& g, juce::Rectangle<float> area) const
    {
        const auto size = juce::jmin (area.getWidth(), area.getHeight());
        const auto face = juce::Rectangle<float> (size, size).withCentre (area.getCentre());
        const auto c = face.getCentre();
        const auto r = size / 2.0f;
        g.setColour (theme::colour::bg1);
        g.fillEllipse (face);
        g.setColour (theme::colour::borderStrong);
        g.drawEllipse (face.reduced (1.0f), 1.5f);
        // cent ticks around the face: −50 … +50 over the full circle, 0 at the top; majors every 10 cents
        for (int k = -50; k < 50; k += 2)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (float) k / 100.0f;
            const auto major = k % 10 == 0;
            const auto r0 = r * (major ? 0.80f : 0.86f), r1 = r * 0.93f;
            g.setColour (k == 0 ? theme::colour::ledOn : major ? theme::colour::textSecondary : theme::colour::textDisabled);
            g.drawLine (c.x + r0 * std::sin (angle), c.y - r0 * std::cos (angle), c.x + r1 * std::sin (angle), c.y - r1 * std::cos (angle),
                        major ? 2.0f : 1.0f);
        }
        if (reading.valid)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (float) reading.cents / 100.0f;
            const auto dot = juce::Point<float> (c.x + r * 0.87f * std::sin (angle), c.y - r * 0.87f * std::cos (angle));
            g.setColour (offsetColour (reading.cents));
            g.fillEllipse (juce::Rectangle<float> (r * 0.13f, r * 0.13f).withCentre (dot));
            g.setColour (offsetColour (reading.cents).withAlpha (0.35f));
            g.drawEllipse (face.reduced (r * 0.24f), 3.0f);
        }
        paintNote (g, juce::Rectangle<float> (r * 1.2f, r * 0.7f).withCentre (c.translated (0.0f, -r * 0.08f)), r * 0.55f);
        g.setFont (theme::font (juce::jmin (13.0f, r * 0.12f)));
        g.setColour (theme::colour::textSecondary);
        g.drawText (hzLine(), juce::Rectangle<float> (r * 1.4f, r * 0.16f).withCentre (c.translated (0.0f, r * 0.42f)), juce::Justification::centred, false);
    }

    void applyOptions()
    {
        const auto* e = target.current();
        const auto ref = e != nullptr ? fx::tuner::referenceHz (e->params[fx::tuner::reference]) : 440.0;
        const auto react = e != nullptr ? e->params[fx::tuner::reactivity] : 1.0f;
        const auto b = e != nullptr && fx::tuner::bassOn (e->params[fx::tuner::bass]);
        analyzer.setOptions (ref, react, b);
    }

    void timerCallback() override
    {
        if (! isShowing())
            return;
        applyOptions();
        auto running = target.running ? target.running() : nullptr;
        auto* tap = running != nullptr ? visualfx::tapOf (running.get()) : nullptr;
        if (tap == nullptr)
            return;
        const auto rate = tap->sampleRate.load();
        if (rate != preparedRate)
        {
            analyzer.prepare (rate);
            applyOptions();
            preparedRate = rate;
            lastRead = tap->written();
        }
        const auto written = tap->written();
        const auto fresh = (int) juce::jmin<std::uint64_t> (written - lastRead, (std::uint64_t) visualfx::tapSize / 2);
        lastRead = written;
        if (fresh > 0)
        {
            left.resize ((std::size_t) fresh);
            right.resize ((std::size_t) fresh);
            tap->read (left.data(), right.data(), fresh);
            for (int i = 0; i < fresh; ++i)
                left[(std::size_t) i] = 0.5f * (left[(std::size_t) i] + right[(std::size_t) i]);
            analyzer.push (left.data(), fresh);
        }
        const auto next = analyzer.reading();
        if (next.valid != reading.valid || next.note != reading.note || std::abs (next.cents - reading.cents) > 0.2
            || std::abs (next.hz - reading.hz) > 0.05)
        {
            reading = next;
            repaint (display.toNearestInt());
        }
    }

    VisualTarget target;
    bool demo;
    Knob reference, reactivity;
    Switch bass, visual;
    juce::Rectangle<float> display;
    fx::tuner::Analyzer analyzer;
    fx::tuner::Reading reading;
    double preparedRate = 0.0;
    std::uint64_t lastRead = 0;
    std::vector<float> left, right;
};

// ------------------------------------------------------------------------------------------------------------
// Fruity HTML NoteBook
// ------------------------------------------------------------------------------------------------------------
class PageView final : public juce::Component
{
public:
    std::function<void (const std::string& link)> onLink;

    void setPage (const fx::htmlnotebook::Page& p, const juce::String& message)
    {
        page = p;
        note = message;
        layoutFor (getWidth());
    }

    void layoutFor (int width)
    {
        width = juce::jmax (120, width);
        layouts.clear();
        float y = 10.0f;
        const auto textWidth = (float) width - 24.0f;
        if (note.isNotEmpty())
        {
            juce::AttributedString s;
            s.append (note, juce::Font (theme::font (theme::type::body)), theme::colour::textSecondary);
            s.setWordWrap (juce::AttributedString::byWord);
            Laid laid;
            laid.layout.createLayout (s, textWidth);
            laid.y = y;
            y += laid.layout.getHeight() + 10.0f;
            layouts.push_back (std::move (laid));
        }
        for (const auto& block : page.blocks)
        {
            Laid laid;
            laid.kind = block.kind;
            laid.y = y;
            if (block.kind == fx::htmlnotebook::Block::Kind::rule)
            {
                y += 14.0f;
                layouts.push_back (std::move (laid));
                continue;
            }
            const auto size = block.kind == fx::htmlnotebook::Block::Kind::heading1   ? 22.0f
                              : block.kind == fx::htmlnotebook::Block::Kind::heading2 ? 18.0f
                              : block.kind == fx::htmlnotebook::Block::Kind::heading3 ? 15.0f
                                                                                       : 13.0f;
            const auto heading = size > 13.0f;
            const auto mono = block.kind == fx::htmlnotebook::Block::Kind::preformatted;
            const auto indent = block.kind == fx::htmlnotebook::Block::Kind::listItem ? 16.0f : 0.0f;
            juce::AttributedString s;
            s.setWordWrap (mono ? juce::AttributedString::none : juce::AttributedString::byWord);
            for (const auto& run : block.runs)
            {
                auto font = juce::Font (mono ? theme::monoFont (size) : theme::font (size, heading || run.bold));
                if (run.italic)
                    font = font.italicised();
                const auto start = s.getText().length();
                const auto text = juce::String::fromUTF8 (run.text.c_str());
                s.append (text, font, run.link.empty() ? (heading ? theme::colour::textPrimary : theme::colour::textSecondary) : theme::colour::info);
                if (! run.link.empty())
                    laid.links.push_back ({ start, start + text.length(), run.link });
            }
            laid.indent = indent;
            laid.layout.createLayout (s, textWidth - indent);
            y += laid.layout.getHeight() + (heading ? 10.0f : 7.0f);
            layouts.push_back (std::move (laid));
        }
        setSize (width, (int) std::ceil (y + 10.0f));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg0);
        for (const auto& l : layouts)
        {
            if (l.kind == fx::htmlnotebook::Block::Kind::rule)
            {
                g.setColour (theme::colour::border);
                g.drawHorizontalLine ((int) l.y + 6, 12.0f, (float) getWidth() - 12.0f);
                continue;
            }
            if (l.kind == fx::htmlnotebook::Block::Kind::listItem)
            {
                g.setColour (theme::colour::textDisabled);
                g.fillEllipse (16.0f, l.y + 6.0f, 4.0f, 4.0f);
            }
            l.layout.draw (g, juce::Rectangle<float> (12.0f + l.indent, l.y, (float) getWidth() - 24.0f - l.indent, l.layout.getHeight()));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (linkAt (e.position).empty() ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor);
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (const auto link = linkAt (e.position); ! link.empty() && onLink)
            onLink (link);
    }

private:
    struct LinkRange
    {
        int from, to;
        std::string target;
    };
    struct Laid
    {
        juce::TextLayout layout;
        float y = 0.0f, indent = 0.0f;
        fx::htmlnotebook::Block::Kind kind = fx::htmlnotebook::Block::Kind::paragraph;
        std::vector<LinkRange> links;
    };

    std::string linkAt (juce::Point<float> p) const
    {
        for (const auto& l : layouts)
        {
            if (l.links.empty() || p.y < l.y || p.y > l.y + l.layout.getHeight())
                continue;
            const auto local = p - juce::Point<float> (12.0f + l.indent, l.y);
            for (int li = 0; li < l.layout.getNumLines(); ++li)
            {
                const auto& line = l.layout.getLine (li);
                for (const auto* run : line.runs)
                {
                    if (run == nullptr)
                        continue;
                    const auto range = run->getRunBoundsX();
                    const auto lineTop = line.leading + line.lineOrigin.y - line.ascent;
                    if (local.x < line.lineOrigin.x + range.getStart() || local.x > line.lineOrigin.x + range.getEnd() || local.y < lineTop
                        || local.y > lineTop + line.ascent + line.descent)
                        continue;
                    for (const auto& link : l.links)
                        if (run->stringRange.getStart() < link.to && run->stringRange.getEnd() > link.from)
                            return link.target;
                }
            }
        }
        return {};
    }

    fx::htmlnotebook::Page page;
    juce::String note;
    std::vector<Laid> layouts;
};

class HtmlNotebookEditor final : public juce::Component
{
public:
    explicit HtmlNotebookEditor (const VisualTarget& t) : target (t)
    {
        addAndMakeVisible (addressBox);
        addressBox.setFont (juce::Font (theme::font (theme::type::body)));
        addressBox.setColour (juce::TextEditor::backgroundColourId, theme::colour::bg0);
        addressBox.setColour (juce::TextEditor::textColourId, theme::colour::textPrimary);
        addressBox.setColour (juce::TextEditor::outlineColourId, theme::colour::borderStrong);
        addressBox.setTextToShowWhenEmpty (tr ("Page: a .html file or a web address"), theme::colour::textDisabled);
        addressBox.onReturnKey = [this] { setAddress (addressBox.getText().trim().toStdString()); };
        addressBox.onFocusLost = [this] {
            if (addressBox.getText().trim().toStdString() != current())
                setAddress (addressBox.getText().trim().toStdString());
        };
        for (auto* b : { &browse, &openButton, &reload })
            addAndMakeVisible (*b);
        browse.setTooltip (tr ("Choose the .html page to show"));
        openButton.setTooltip (tr ("Open the page in your web browser"));
        reload.setTooltip (tr ("Read the page again"));
        browse.onClick = [this] { choose(); };
        openButton.onClick = [this] { openInBrowser (current()); };
        reload.onClick = [this] { show(); };
        viewport.setViewedComponent (&view, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);
        view.onLink = [this] (const std::string& link) { follow (link); };
        setSize (560, 460);
        show();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (6);
        auto bar = area.removeFromTop (26);
        reload.setBounds (bar.removeFromRight (64));
        bar.removeFromRight (4);
        openButton.setBounds (bar.removeFromRight (110));
        bar.removeFromRight (4);
        browse.setBounds (bar.removeFromLeft (80));
        bar.removeFromLeft (4);
        addressBox.setBounds (bar);
        area.removeFromTop (6);
        viewport.setBounds (area);
        view.layoutFor (viewport.getMaximumVisibleWidth());
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

private:
    std::string current() const
    {
        const auto* e = target.current();
        return e != nullptr ? fx::htmlnotebook::address (e->state) : std::string();
    }

    void setAddress (const std::string& a)
    {
        target.edit ([a] (EffectSlot& s) { s.state = fx::htmlnotebook::stateFor (a); });
        show();
    }

    void choose()
    {
        const auto start = juce::File::isAbsolutePath (juce::String::fromUTF8 (current().c_str())) ? juce::File (juce::String::fromUTF8 (current().c_str()))
                                                                                                    : juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
        chooser = std::make_unique<juce::FileChooser> (tr ("Choose the .html page to show"), start, "*.html;*.htm");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer (this)] (const juce::FileChooser& c) {
                                  if (safe != nullptr && c.getResult() != juce::File())
                                      safe->setAddress (c.getResult().getFullPathName().toStdString());
                              });
    }

    static void openInBrowser (const std::string& a)
    {
        if (a.empty())
            return;
        const auto text = juce::String::fromUTF8 (a.c_str());
        if (fx::htmlnotebook::isWebAddress (a))
            juce::URL (text.startsWithIgnoreCase ("www.") ? "https://" + text : text).launchInDefaultBrowser();
        else
            juce::File (text).startAsProcess();
    }

    void follow (const std::string& link)
    {
        const auto text = juce::String::fromUTF8 (link.c_str());
        if (fx::htmlnotebook::isWebAddress (link) || text.startsWithIgnoreCase ("mailto:"))
        {
            openInBrowser (link);
            return;
        }
        if (text.startsWithChar ('#'))
            return;
        // a local page: relative to the shown one, opened in place
        const auto base = juce::File (juce::String::fromUTF8 (current().c_str()));
        const auto file = base.getParentDirectory().getChildFile (text.upToFirstOccurrenceOf ("#", false, false));
        if (file.existsAsFile() && (file.hasFileExtension ("html") || file.hasFileExtension ("htm")))
            setAddress (file.getFullPathName().toStdString());
        else if (file.existsAsFile())
            file.startAsProcess();
    }

    void show()
    {
        const auto a = current();
        addressBox.setText (juce::String::fromUTF8 (a.c_str()), false);
        openButton.setEnabled (! a.empty());
        fx::htmlnotebook::Page page;
        juce::String note;
        if (a.empty())
            note = tr ("No page. Choose a .html file with Browse, or type a web address and press Enter.");
        else if (fx::htmlnotebook::isWebAddress (a))
            note = tr ("A web page: LYNX does not load web pages itself. Use \"Open in browser\".");
        else
        {
            const juce::File file (juce::String::fromUTF8 (a.c_str()));
            if (! file.existsAsFile())
                note = tr ("The page was not found: {0}", { file.getFullPathName() });
            else
            {
                page = fx::htmlnotebook::parse (file.loadFileAsString().toStdString());
                if (page.blocks.empty())
                    note = tr ("The page has no text to show.");
            }
        }
        view.setPage (page, note);
        view.layoutFor (viewport.getMaximumVisibleWidth());
    }

    VisualTarget target;
    juce::TextEditor addressBox;
    juce::TextButton browse { tr ("Browse") }, openButton { tr ("Open in browser") }, reload { tr ("Reload") };
    juce::Viewport viewport;
    PageView view;
    std::unique_ptr<juce::FileChooser> chooser;
};

// ------------------------------------------------------------------------------------------------------------
// Fruity Scratcher (REA_rodada165g-flscratch165, core/Scratcher.h)
// ------------------------------------------------------------------------------------------------------------
// The turntable's three-way transport (Backwards / Stopped / Forwards)
class TransportSelector final : public juce::Component
{
public:
    explicit TransportSelector (const VisualTarget& t) : target (t)
    {
        setRepaintsOnMouseActivity (true);
        ui::setHint (*this, tr ("Transport") + ": " + tr ("Backwards") + " / " + tr ("Stopped") + " / " + tr ("Forwards"));
    }

    int direction() const
    {
        const auto* e = target.current();
        return scratcher::transportOf (e != nullptr ? e->params[scratcher::transport] : 0.5f);
    }

    void paint (juce::Graphics& g) override
    {
        auto box = getLocalBounds().toFloat().reduced (2.0f, 3.0f);
        const auto w = box.getWidth() / 3.0f;
        const juce::String labels[3] = { tr ("Backwards"), tr ("Stopped"), tr ("Forwards") };
        for (int i = 0; i < 3; ++i)
        {
            const auto cell = box.withX (box.getX() + w * (float) i).withWidth (w).reduced (1.0f, 0.0f);
            const auto lit = direction() == i - 1;
            skin::button (g, cell, lit, isMouseOver(), theme::colour::accent);
            g.setFont (theme::font (theme::type::small, true));
            g.setColour (lit ? theme::colour::textPrimary : theme::colour::textSecondary);
            g.drawFittedText (labels[i], cell.toNearestInt(), juce::Justification::centred, 1);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (! getLocalBounds().contains (e.getPosition()))
            return;
        const auto d = juce::jlimit (0, 2, (int) (e.position.x / ((float) getWidth() / 3.0f))) - 1;
        target.edit ([d] (EffectSlot& s) { s.params[scratcher::transport] = scratcher::transportValue (d); });
        if (auto* parent = getParentComponent())
            parent->repaint();
    }

private:
    VisualTarget target;
};

// (a sample dropped on the editor loads onto the platter, from the OS or from LYNX's Browser, so it never falls through
// to the Playlist as an audio clip)
class ScratcherEditor final : public juce::Component,
                              public juce::FileDragAndDropTarget,
                              public juce::DragAndDropTarget,
                              private juce::Timer
{
public:
    ScratcherEditor (const VisualTarget& t, bool demoMode)
        : target (t), demo (demoMode), transportSel (t),
          pan (t, EffectKind::scratcher, scratcher::pan), volume (t, EffectKind::scratcher, scratcher::volume),
          speed (t, EffectKind::scratcher, scratcher::speed), accel (t, EffectKind::scratcher, scratcher::accel),
          sens (t, EffectKind::scratcher, scratcher::sensitivity),
          smooth1 (t, EffectKind::scratcher, scratcher::smooth1), smooth2 (t, EffectKind::scratcher, scratcher::smooth2),
          smooth3 (t, EffectKind::scratcher, scratcher::smooth3), smooth4 (t, EffectKind::scratcher, scratcher::smooth4),
          hold (t, scratcher::hold, tr ("Off"), tr ("On"), tr ("Hold")), mute (t, scratcher::mute, tr ("Off"), tr ("On"), tr ("Mute"))
    {
        for (auto* c : std::initializer_list<juce::Component*> { &transportSel, &pan, &volume, &speed, &accel, &sens, &smooth1, &smooth2,
                                                                 &smooth3, &smooth4, &hold, &mute, &load })
            addAndMakeVisible (*c);
        load.setButtonText (tr ("Load sample..."));
        load.onClick = [this] { chooseSample(); };
        ui::setHint (load, tr ("The sample on the platter (or drop a file here)"));
        setSize (620, 360);
        refreshSample();
        if (! demo)
            startTimerHz (30);
    }

    void setDemoPosition (float turns)
    {
        demoPos = turns;
        repaint();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto top = area.removeFromTop (26);
        load.setBounds (top.removeFromRight (130));
        nameArea = top;
        area.removeFromTop (4);
        waveArea = area.removeFromTop (54);
        area.removeFromTop (8);
        transportSel.setBounds (area.removeFromBottom (30).withSizeKeepingCentre (300, 30));
        area.removeFromBottom (4);

        auto left = area.removeFromLeft (84);
        const auto knobH = juce::jmin (78, left.getHeight() / 3);
        pan.setBounds (left.removeFromTop (knobH));
        volume.setBounds (left.removeFromTop (knobH));
        auto switches = left;
        hold.setBounds (switches.removeFromTop (switches.getHeight() / 2));
        mute.setBounds (switches);

        auto right = area.removeFromRight (84);
        speed.setBounds (right.removeFromTop (knobH));
        accel.setBounds (right.removeFromTop (knobH));
        sens.setBounds (right.removeFromTop (knobH));

        auto smooth = area.removeFromRight (124);
        smoothArea = smooth;
        smooth.removeFromTop (16);
        const auto cell = juce::jmin (62, smooth.getHeight() / 2);
        auto row1 = smooth.removeFromTop (cell), row2 = smooth.removeFromTop (cell);
        smooth1.setBounds (row1.removeFromLeft (62));
        smooth2.setBounds (row1);
        smooth3.setBounds (row2.removeFromLeft (62));
        smooth4.setBounds (row2);

        const auto d = (float) juce::jmin (area.getWidth(), area.getHeight());
        platter = juce::Rectangle<float> (d, d).withCentre (area.toFloat().getCentre()).reduced (4.0f);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg0);

        // the sample's name
        g.setFont (theme::font (theme::type::small, true));
        g.setColour (sampleName.isEmpty() ? theme::colour::textDisabled : theme::colour::textPrimary);
        g.drawFittedText (sampleName.isEmpty() ? tr ("No sample: load one or drop a file") : sampleName, nameArea.reduced (2, 0),
                          juce::Justification::centredLeft, 1);

        // the waveform with the platter's position
        const auto wave = waveArea.toFloat();
        g.setColour (theme::colour::bg1);
        g.fillRoundedRectangle (wave, theme::radius::md);
        g.setColour (theme::colour::border);
        g.drawRoundedRectangle (wave.reduced (0.5f), theme::radius::md, 1.0f);
        if (! peaks.empty())
        {
            const auto mid = wave.getCentreY(), half = wave.getHeight() * 0.45f;
            g.setColour (theme::colour::accent.withAlpha (0.75f));
            const auto cols = (int) peaks.size();
            for (int x = 0; x < cols; ++x)
            {
                const auto [lo, hi] = peaks[(std::size_t) x];
                const auto px = wave.getX() + (float) x;
                g.drawVerticalLine ((int) px, mid - hi * half, mid - lo * half + 1.0f);
            }
            const auto px = wave.getX() + position() * wave.getWidth();
            g.setColour (theme::colour::textPrimary);
            g.drawVerticalLine ((int) px, wave.getY() + 2.0f, wave.getBottom() - 2.0f);
        }

        // the platter: a record with its grooves and a mark at the position (one turn = the whole sample)
        const auto c = platter.getCentre();
        const auto r = platter.getWidth() * 0.5f;
        g.setColour (juce::Colour (0xff15161a));
        g.fillEllipse (platter);
        g.setColour (juce::Colour (0xff2a2c33));
        for (float k = 0.42f; k < 0.97f; k += 0.055f)
            g.drawEllipse (juce::Rectangle<float> (2.0f * r * k, 2.0f * r * k).withCentre (c), 1.0f);
        g.setColour (theme::colour::border);
        g.drawEllipse (platter.reduced (0.5f), 1.5f);
        const auto label = juce::Rectangle<float> (r * 0.66f, r * 0.66f).withCentre (c);
        g.setColour (theme::colour::accent);
        g.fillEllipse (label);
        g.setColour (juce::Colour (0xff15161a));
        g.fillEllipse (juce::Rectangle<float> (r * 0.07f, r * 0.07f).withCentre (c));
        const auto angle = position() * juce::MathConstants<float>::twoPi;
        const juce::Point<float> dir { std::sin (angle), -std::cos (angle) };
        g.setColour (theme::colour::textPrimary);
        g.drawLine ({ c + dir * (r * 0.36f), c + dir * (r * 0.95f) }, 3.0f);
        if (holding)
        {
            g.setColour (theme::colour::accent.withAlpha (0.8f));
            g.drawEllipse (platter.reduced (2.0f), 3.0f);
        }

        if (dropHover)
        {
            g.setColour (theme::colour::accent);
            g.drawRect (getLocalBounds(), 2);
        }

        g.setFont (theme::font (theme::type::caption));
        g.setColour (skin::captionColour());
        g.drawText (tr ("Smoothing"), smoothArea.removeFromTop (16), juce::Justification::centred);
        const auto* e = target.current();
        const auto turns = e != nullptr ? scratcher::rotationsOf (e->params[scratcher::rotations]) - 9 : 0;
        if (turns != 0)
            g.drawText (tr ("Rotations") + " " + juce::String (turns > 0 ? "+" : "") + juce::String (turns),
                        platter.withHeight (16.0f).translated (0.0f, platter.getHeight() - 8.0f).toNearestInt(), juce::Justification::centred);
    }

    // drag the platter (or the waveform) to scratch: Hold is on while the button is down, as in FL [DECOMP 0x732630]
    void mouseDown (const juce::MouseEvent& e) override
    {
        const auto onPlatter = e.position.getDistanceFrom (platter.getCentre()) <= platter.getWidth() * 0.5f;
        if (! onPlatter && ! waveArea.contains (e.getPosition()))
            return;
        const auto* s = target.current();
        if (s == nullptr)
            return;
        holding = true;
        holdBefore = s->params[scratcher::hold];
        dragStartPos = s->params[scratcher::hold] >= 0.5f ? s->params[scratcher::wavePos] : position();
        dragTurns = scratcher::rotationsOf (s->params[scratcher::rotations]);
        dragFrom = e.position;
        // the platter is caught where it is: Wave position takes the motor's place, then Hold stops the motor
        target.edit ([p = dragStartPos] (EffectSlot& x) {
            x.params[scratcher::wavePos] = p;
            x.params[scratcher::hold] = 1.0f;
        }, false);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! holding)
            return;
        const auto* s = target.current();
        if (s == nullptr)
            return;
        // FL: 0.0025 · Sensitivity of a turn per pixel (vertical: up = forwards; on the waveform: horizontal) [DECOMP 0x732770]
        const auto onWave = waveArea.contains (dragFrom.toInt());
        const auto pixels = onWave ? e.position.x - dragFrom.x : dragFrom.y - e.position.y;
        const auto slow = e.mods.isCtrlDown() ? 0.25f : 1.0f;
        auto pos = dragStartPos + pixels * 0.0025f * s->params[scratcher::sensitivity] * slow;
        auto turns = dragTurns;
        // whole turns go to Rotations (0…18), as FL's hidden wheel counts them
        while (pos > 1.0f && turns < 18)
            pos -= 1.0f, ++turns;
        while (pos < 0.0f && turns > 0)
            pos += 1.0f, --turns;
        pos = juce::jlimit (0.0f, 1.0f, pos);
        target.edit ([pos, turns] (EffectSlot& x) {
            x.params[scratcher::wavePos] = pos;
            x.params[scratcher::rotations] = scratcher::valueOfRaw (scratcher::rotations, turns);
        }, false);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (! holding)
            return;
        holding = false;
        target.edit ([h = holdBefore] (EffectSlot& x) { x.params[scratcher::hold] = h; });
        repaint();
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return files.size() == 1 && scratcher::loadsDroppedFile (files[0].toStdString());
    }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        dropHover = false;
        if (isInterestedInFileDrag (files))
            useSample (juce::File (files[0]));
        repaint();
    }

    void fileDragEnter (const juce::StringArray&, int, int) override { setDropHover (true); }
    void fileDragExit (const juce::StringArray&) override { setDropHover (false); }

    // a sample dragged from LYNX's Browser
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return BrowserComponent::isBrowserDrag (details)
               && scratcher::loadsDroppedFile (BrowserComponent::draggedFile (details).getFullPathName().toStdString());
    }

    void itemDragEnter (const SourceDetails&) override { setDropHover (true); }
    void itemDragExit (const SourceDetails&) override { setDropHover (false); }

    void itemDropped (const SourceDetails& details) override
    {
        dropHover = false;
        const auto file = BrowserComponent::draggedFile (details);
        if (file.existsAsFile())
            useSample (file);
        repaint();
    }

private:
    void setDropHover (bool on)
    {
        dropHover = on;
        repaint();
    }

    float position() const
    {
        if (demo)
            return demoPos;
        const auto* e = target.current();
        if (e != nullptr && e->params[scratcher::hold] >= 0.5f && holding)
            return e->params[scratcher::wavePos];
        if (target.running)
            if (const auto fx = target.running())
                return fx->controlOutput (0);
        return e != nullptr ? e->params[scratcher::wavePos] : 0.0f;
    }

    void timerCallback() override
    {
        const auto* e = target.current();
        if (e != nullptr && e->samplePath != shownPath)
            refreshSample();
        repaint (platter.getSmallestIntegerContainer().expanded (4));
        repaint (waveArea);
    }

    void refreshSample()
    {
        const auto* e = target.current();
        shownPath = e != nullptr ? e->samplePath : std::string();
        peaks.clear();
        sampleName.clear();
        if (shownPath.empty() || target.engine == nullptr)
            return;
        const juce::File file (juce::String::fromUTF8 (shownPath.c_str()));
        sampleName = file.getFileName();
        const auto sample = target.engine->getSampleLibrary().load (file.getFullPathName());
        if (sample == nullptr)
        {
            sampleName = tr ("Could not read {0}", { file.getFileName() });
            return;
        }
        const auto cols = juce::jmax (1, waveArea.getWidth() > 0 ? waveArea.getWidth() : 600);
        const auto n = sample->frames();
        const auto* l = sample->leftData();
        const auto* rr = sample->rightData();
        peaks.assign ((std::size_t) cols, { 0.0f, 0.0f });
        for (int x = 0; x < cols && n > 0; ++x)
        {
            const auto a = (std::size_t) ((double) x * (double) n / cols), b = juce::jmax (a + 1, (std::size_t) ((double) (x + 1) * (double) n / cols));
            float lo = 0.0f, hi = 0.0f;
            for (auto i = a; i < b && i < n; ++i)
            {
                const auto v = 0.5f * (l[i] + rr[i]);
                lo = juce::jmin (lo, v);
                hi = juce::jmax (hi, v);
            }
            peaks[(std::size_t) x] = { lo, hi };
        }
    }

    void chooseSample()
    {
        if (target.engine == nullptr)
            return;
        const auto* e = target.current();
        const juce::File current (juce::String::fromUTF8 (e != nullptr ? e->samplePath.c_str() : ""));
        chooser = std::make_unique<juce::FileChooser> (tr ("Load sample"), current.existsAsFile() ? current : juce::File(),
                                                       target.engine->getSampleLibrary().wildcard());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<ScratcherEditor> (this)] (const juce::FileChooser& fc) {
                                  if (safe != nullptr && fc.getResult().existsAsFile())
                                      safe->useSample (fc.getResult());
                              });
    }

    void useSample (const juce::File& file)
    {
        if (target.engine == nullptr || target.engine->getSampleLibrary().load (file.getFullPathName()) == nullptr)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Load sample"), tr ("Could not read {0}", { file.getFileName() }));
            return;
        }
        target.edit ([path = file.getFullPathName().toStdString()] (EffectSlot& s) { s.samplePath = path; });
        refreshSample();
        repaint();
    }

    VisualTarget target;
    bool demo;
    float demoPos = 0.0f;
    TransportSelector transportSel;
    Knob pan, volume, speed, accel, sens, smooth1, smooth2, smooth3, smooth4;
    Switch hold, mute;
    juce::TextButton load;
    juce::Rectangle<int> nameArea, waveArea, smoothArea;
    juce::Rectangle<float> platter;
    std::vector<std::pair<float, float>> peaks;
    std::string shownPath;
    juce::String sampleName;
    std::unique_ptr<juce::FileChooser> chooser;
    bool holding = false, dropHover = false;
    float holdBefore = 0.0f, dragStartPos = 0.0f;
    int dragTurns = 9;
    juce::Point<float> dragFrom;
};

// ------------------------------------------------------------------------------------------------------------
// Windows
// ------------------------------------------------------------------------------------------------------------
class ExtraWindow final : public juce::DocumentWindow
{
public:
    ExtraWindow (const VisualTarget& target, EffectKind kind, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg2, DocumentWindow::closeButton)
    {
        setUsingNativeTitleBar (true);
        setContentOwned (makeFlExtraEditor (target, kind, false).release(), true);
        setResizable (true, false);
        if (kind == EffectKind::bigClock)
            setResizeLimits (200, 80, 3000, 1200);
        else if (kind == EffectKind::tuner)
            setResizeLimits (300, 300, 1200, 1200);
        else if (kind == EffectKind::scratcher)
            setResizeLimits (520, 320, 1600, 1200);
        else
            setResizeLimits (360, 240, 3000, 2400);
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

std::map<juce::String, std::unique_ptr<ExtraWindow>>& extraWindows()
{
    static std::map<juce::String, std::unique_ptr<ExtraWindow>> open;
    return open;
}

void ExtraWindow::closeButtonPressed()
{
    for (auto& [key, window] : extraWindows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { extraWindows().erase (k); });
            return;
        }
}

void openWindow (const VisualTarget& target, EffectKind kind, const juce::String& title, const juce::String& key)
{
    auto& open = extraWindows();
    if (auto it = open.find (key); it != open.end())
    {
        it->second->toFront (true);
        return;
    }
    open[key] = std::make_unique<ExtraWindow> (target, kind, title);
}
} // namespace

std::unique_ptr<juce::Component> makeFlExtraEditor (const VisualTarget& target, EffectKind kind, bool demo)
{
    switch (kind)
    {
        case EffectKind::bigClock: return std::make_unique<BigClockEditor> (target, demo);
        case EffectKind::tuner: return std::make_unique<TunerEditor> (target, demo);
        case EffectKind::scratcher: return std::make_unique<ScratcherEditor> (target, demo);
        default: return std::make_unique<HtmlNotebookEditor> (target);
    }
}

namespace FlExtraWindows
{
bool handles (EffectKind kind)
{
    return isExtraKind (kind);
}

void open (AudioEngine& engine, int track, int slot)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;
    const auto kind = mixer[(std::size_t) track].effects[(std::size_t) slot].kind;
    if (! isExtraKind (kind))
        return;
    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    const auto title = juce::String (effectInfo (kind).name) + " - " + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " ("
                       + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    openWindow (slotTarget (engine, track, slot), kind, title, "mixer-" + juce::String (track) + "-" + juce::String (slot) + "-" + juce::String ((int) kind));
}

void openNode (const VisualTarget& target, const juce::String& title, const juce::String& key)
{
    if (const auto* e = target.current(); e != nullptr && isExtraKind (e->kind))
        openWindow (target, e->kind, title, key);
}

void closeAll()
{
    extraWindows().clear();
}
} // namespace FlExtraWindows

// ------------------------------------------------------------------------------------------------------------
// --flextrashot
// ------------------------------------------------------------------------------------------------------------
bool runFlExtraShots (AudioEngine& engine, const juce::File& folder)
{
    folder.createDirectory();
    juce::String report;
    bool ok = true;
    auto save = [&] (juce::Component& c, const juce::String& name) {
        const auto image = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
        const auto file = folder.getChildFile (name + ".png");
        file.deleteFile();
        juce::FileOutputStream out (file);
        juce::PNGImageFormat png;
        const auto written = out.openedOk() && png.writeImageToStream (image, out);
        ok = ok && written;
        report << name << (written ? " ok\n" : " FAILED\n");
    };
    auto setSlot = [&] (EffectSlot fx) {
        Song song;
        song.mixer.resize (2);
        song.mixer[0].name = "Master";
        song.mixer[1].name = "Insert 1";
        song.bpm = 130.0;
        song.mixer[1].effects.push_back (std::move (fx));
        engine.setSong (std::move (song));
        engine.dispatchPendingMessages();
    };

    // Big Clock: the three displays at bar 12, beat 3, step 2 + 17 ticks (song mode)
    for (int d = 0; d < flextra::bigclock::displays; ++d)
    {
        auto e = makeEffectSlot (EffectKind::bigClock);
        e.params[flextra::bigclock::display] = flextra::bigclock::displayValue ((flextra::bigclock::Display) d);
        setSlot (e);
        BigClockEditor editor (slotTarget (engine, 1, 0), true);
        editor.setSize (420, 150);
        editor.setDemoTicks (11.0 * 4 * ppq + 2.0 * ppq + 1.0 * ticksPerStep + 170.0);
        save (editor, "bigclock-" + juce::String (d));
    }
    // Tuner: Basic and Dial on a 442.3 Hz tone (A4 +9 cents), and Dial on 82.0 Hz with Bass (E2 −17 cents)
    for (const auto& [name, vis, hz, bass] : std::vector<std::tuple<juce::String, float, double, float>> {
             { "tuner-basic", 0.0f, 442.3, 0.0f }, { "tuner-dial", 1.0f, 442.3, 0.0f }, { "tuner-dial-bass-e2", 1.0f, 81.6, 1.0f } })
    {
        auto e = makeEffectSlot (EffectKind::tuner);
        e.params[flextra::tuner::visual] = vis;
        e.params[flextra::tuner::bass] = bass;
        setSlot (e);
        TunerEditor editor (slotTarget (engine, 1, 0), true);
        editor.setSize (360, 330);
        editor.feedDemo (hz, 1.5);
        save (editor, name);
    }
    // HTML NoteBook: FL's own default page when installed, else a built-in sample page
    {
        auto page = juce::File ("C:\\Program Files\\Image-Line\\FL Studio 2026\\Plugins\\Fruity\\Effects\\Fruity HTML NoteBook\\DefaultHtml.html");
        if (! page.existsAsFile())
        {
            page = folder.getChildFile ("sample-page.html");
            page.replaceWithText ("<html><head><title>Notes</title></head><body><h1>Session notes</h1><p>Lead vocal: <b>take 3</b>, "
                                  "comp the chorus from <i>take 5</i>.</p><ul><li>Kick: tighten the low end</li><li>Bass: sidechain to the "
                                  "kick</li></ul><hr><p>Reference: <a href=\"https://example.com\">example.com</a></p></body></html>");
        }
        auto e = makeEffectSlot (EffectKind::htmlNotebook);
        e.state = flextra::htmlnotebook::stateFor (page.getFullPathName().toStdString());
        setSlot (e);
        HtmlNotebookEditor editor (slotTarget (engine, 1, 0));
        editor.setSize (560, 460);
        save (editor, "htmlnotebook");
        auto web = makeEffectSlot (EffectKind::htmlNotebook);
        web.state = flextra::htmlnotebook::stateFor ("http://www.image-line.com/documents/news.php");
        setSlot (web);
        HtmlNotebookEditor webEditor (slotTarget (engine, 1, 0));
        webEditor.setSize (560, 200);
        save (webEditor, "htmlnotebook-web");
    }
    // Mute 2: the plugin layout editor
    for (const auto& [name, mute, channel] : std::vector<std::tuple<juce::String, float, float>> { { "mute2-default", 1.0f, 1.0f / 3.0f },
                                                                                                    { "mute2-muted-left", 0.0f, 0.0f } })
    {
        auto e = makeEffectSlot (EffectKind::fruityMute2);
        e.params[flextra::mute2::mute] = mute;
        e.params[flextra::mute2::channel] = channel;
        setSlot (e);
        if (auto editor = makeNativeEffectEditor (engine, 1, 0))
            save (*editor, name);
        else
        {
            ok = false;
            report << name << " FAILED (no editor)\n";
        }
    }
    // Transmitter (fltrans165): the plugin layout editor with FL's Default preset (Saturation / Diffusion), then Bounce /
    // Delay (tempo-synced) and Blur / Off
    for (const auto& [name, effectT, effectS, sync] : std::vector<std::tuple<juce::String, int, int, float>> {
             { "transmitter-default", transmitter::saturation, transmitter::diffusion, 0.0f },
             { "transmitter-bounce-delay", transmitter::bounce, transmitter::delay, 1.0f },
             { "transmitter-blur-off", transmitter::blur, transmitter::off, 0.0f } })
    {
        auto e = makeEffectSlot (EffectKind::transmitter);
        e.params[transmitter::transientEffect] = transmitter::paramFromRaw (transmitter::transientEffect, effectT);
        e.params[transmitter::sustainEffect] = transmitter::paramFromRaw (transmitter::sustainEffect, effectS);
        e.params[transmitter::sDelaySync] = sync;
        setSlot (e);
        if (auto editor = makeNativeEffectEditor (engine, 1, 0))
            save (*editor, name);
        else
        {
            ok = false;
            report << name << " FAILED (no editor)\n";
        }
    }
    // Fruity Scratcher: FL's own DL Breaker loop when installed (else a generated beat), motor and hold views
    {
        auto loop = juce::File ("C:\\Program Files\\Image-Line\\FL Studio 2026\\Data\\Patches\\Packs\\Loops\\DL Breaker.wav");
        if (! loop.existsAsFile())
        {
            loop = folder.getChildFile ("scratcher-loop.wav");
            loop.deleteFile();
            juce::AudioBuffer<float> beat (2, 88200);
            beat.clear();
            for (int hit = 0; hit < 8; ++hit)
                for (int i = 0; i < 6000; ++i)
                {
                    const auto env = std::exp (-(float) i / (hit % 2 == 0 ? 1800.0f : 500.0f));
                    const auto v = 0.7f * env * (hit % 2 == 0 ? std::sin (2.0f * juce::MathConstants<float>::pi * 55.0f * (float) i / 44100.0f)
                                                              : (float) ((i * 7919) % 200 - 100) / 100.0f);
                    beat.setSample (0, hit * 11025 + i, v);
                    beat.setSample (1, hit * 11025 + i, v);
                }
            juce::WavAudioFormat wav;
            if (auto stream = loop.createOutputStream())
                if (std::unique_ptr<juce::AudioFormatWriter> w { wav.createWriterFor (stream.get(), 44100.0, 2, 16, {}, 0) })
                {
                    stream.release();
                    w->writeFromAudioSampleBuffer (beat, 0, beat.getNumSamples());
                }
        }
        for (const auto& [name, holdOn, transport, pos] : std::vector<std::tuple<juce::String, float, int, float>> {
                 { "scratcher-forwards", 0.0f, 1, 0.3f }, { "scratcher-hold", 1.0f, 0, 0.72f } })
        {
            auto e = makeEffectSlot (EffectKind::scratcher);
            e.samplePath = loop.getFullPathName().toStdString();
            e.params[scratcher::hold] = holdOn;
            e.params[scratcher::transport] = scratcher::transportValue (transport);
            e.params[scratcher::wavePos] = pos;
            setSlot (e);
            ScratcherEditor editor (slotTarget (engine, 1, 0), true);
            editor.setSize (620, 360);
            editor.setDemoPosition (pos);
            save (editor, name);
        }
        // drops: an OS file drop and a drag from the Browser load the sample (other files are refused, so they fall
        // through to whatever lies under the editor)
        {
            auto e = makeEffectSlot (EffectKind::scratcher);
            setSlot (e);
            ScratcherEditor editor (slotTarget (engine, 1, 0), false);
            editor.setSize (620, 360);
            auto samplePath = [&] {
                const auto& fx = engine.getSong().mixer[1].effects[0];
                return juce::String (fx.samplePath);
            };
            const auto refused = ! editor.isInterestedInFileDrag ({ folder.getChildFile ("song.flp").getFullPathName() })
                                 && ! editor.isInterestedInFileDrag ({ loop.getFullPathName(), loop.getFullPathName() });
            editor.filesDropped ({ loop.getFullPathName() }, 300, 180);
            const auto osDrop = editor.isInterestedInFileDrag ({ loop.getFullPathName() }) && samplePath() == loop.getFullPathName();
            engine.editSong ([] (Song& song) { song.mixer[1].effects[0].samplePath.clear(); }, false);
            const juce::DragAndDropTarget::SourceDetails browser (juce::String (BrowserComponent::dragDescription) + "|" + loop.getFullPathName(),
                                                                   nullptr, { 300, 180 });
            const auto interested = editor.isInterestedInDragSource (browser);
            editor.itemDropped (browser);
            const auto browserDrop = interested && samplePath() == loop.getFullPathName();
            ok = ok && refused && osDrop && browserDrop;
            report << "scratcher-drops " << (refused && osDrop && browserDrop ? "ok" : "FAILED") << " (refuses other files "
                   << (refused ? "yes" : "no") << ", OS drop " << (osDrop ? "loaded" : "not loaded") << ", Browser drag "
                   << (browserDrop ? "loaded" : "not loaded") << ")\n";
            save (editor, "scratcher-dropped");
        }
    }
    folder.getChildFile ("report.txt").replaceWithText (report);
    return ok;
}
} // namespace lynx
