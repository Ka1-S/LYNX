#include "ui/Toolbar.h"

#include <juce_dsp/juce_dsp.h>

#include "core/Timecode.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/Icons.h"
#include "ui/Theme.h"

namespace lynx::toolbar
{
namespace
{
juce::String barBeatStep (const Song& song, double ticks, int beatsPerBar)
{
    const auto beats = ticks / ppq;
    int bar = 0, beat = 0;

    if (song.mode == PlayMode::song) // the Playlist ruler applies (with time signature changes)
    {
        const auto current = song.barAt ((std::int64_t) ticks);
        bar = current.number;
        beat = (int) ((ticks - (double) current.tick) / ppq) + 1;
    }
    else
    {
        const auto perBar = juce::jmax (1, beatsPerBar);
        bar = (int) (beats / perBar) + 1;
        beat = (int) std::fmod (beats, (double) perBar) + 1;
    }

    const auto step = (int) (std::fmod (beats, 1.0) * stepsPerBeat) + 1;
    return juce::String::formatted ("%d : %d : %d", bar, beat, step);
}

double loopLength (const Song& song)
{
    if (song.patterns.empty())
        return 1.0;

    const auto length = song.mode == PlayMode::song
                            ? song.songLength()
                            : song.patterns[(std::size_t) juce::jlimit (0, (int) song.patterns.size() - 1, song.currentPattern)].effectiveLength (song.beatsPerBar);
    return (double) juce::jmax<std::int64_t> (1, length);
}

double playheadTicks (AudioEngine& engine)
{
    return std::fmod (juce::jmax (0.0, engine.getTransport().positionTicks.load()), loopLength (engine.getSong()));
}

juce::String centsText (double cents)
{
    const auto c = juce::roundToInt (cents);
    return (c > 0 ? "+" : "") + juce::String (c) + " " + tr ("cents");
}
} // namespace

// --- song position -------------------------------------------------------------------------------

SongPositionBar::SongPositionBar (AudioEngine& e) : engine (e)
{
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

double SongPositionBar::length() const { return loopLength (engine.getSong()); }
double SongPositionBar::position() const { return playheadTicks (engine); }

juce::Rectangle<float> SongPositionBar::track() const
{
    return getLocalBounds().toFloat().reduced (5.0f, 0.0f).withSizeKeepingCentre ((float) getWidth() - 10.0f, 4.0f);
}

void SongPositionBar::update()
{
    const auto fraction = position() / length();

    if (std::abs (fraction - shownFraction) * getWidth() >= 0.5)
    {
        shownFraction = fraction;
        repaint();
    }
}

void SongPositionBar::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto t = track();
    const auto& song = engine.getSong();
    const auto active = dragging || isMouseOver();

    g.setColour (colour::bg0);
    g.fillRoundedRectangle (t.expanded (0.0f, 1.0f), 2.5f);

    // the Playlist loop (Song mode)
    if (song.mode == PlayMode::song && song.hasLoop())
    {
        const auto x0 = t.getX() + t.getWidth() * (float) ((double) song.loopStart / length());
        const auto x1 = t.getX() + t.getWidth() * (float) juce::jmin (1.0, (double) song.loopEnd / length());
        g.setColour (colour::info.withAlpha (0.35f));
        g.fillRect (juce::Rectangle<float> (x0, t.getY() - 1.0f, juce::jmax (1.0f, x1 - x0), t.getHeight() + 2.0f));
    }

    const auto fraction = (float) juce::jlimit (0.0, 1.0, position() / length());
    const auto x = t.getX() + t.getWidth() * fraction;
    g.setColour (engine.getTransport().playing.load() ? colour::accent : colour::accent.withMultipliedSaturation (0.55f).withMultipliedBrightness (0.8f));
    g.fillRoundedRectangle (t.withRight (juce::jmax (t.getX() + 1.0f, x)), 2.0f);

    const auto radius = active ? 5.0f : 3.5f;
    g.setColour (active ? colour::textPrimary : colour::textSecondary);
    g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre ({ x, t.getCentreY() }));
}

void SongPositionBar::seekTo (int x)
{
    const auto t = track();
    const auto fraction = juce::jlimit (0.0, 1.0, (double) ((float) x - t.getX()) / juce::jmax (1.0f, t.getWidth()));
    auto ticks = fraction * length();

    if (! juce::ModifierKeys::currentModifiers.isAltDown()) // Alt: no snap (as everywhere)
        ticks = std::round (ticks / ticksPerStep) * ticksPerStep;

    engine.seek (juce::jlimit (0.0, length() - 1.0, ticks));
    update();
    repaint();
}

void SongPositionBar::mouseDown (const juce::MouseEvent& e)
{
    if (! e.mods.isLeftButtonDown())
        return;

    dragging = true;
    seekTo (e.x);
}

void SongPositionBar::mouseDrag (const juce::MouseEvent& e)
{
    if (dragging)
        seekTo (e.x);
}

void SongPositionBar::mouseUp (const juce::MouseEvent&)
{
    dragging = false;
    repaint();
}

void SongPositionBar::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;

    if (delta == 0.0f)
        return;

    const auto bar = (double) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    const auto current = std::round (position() / bar) * bar;
    engine.seek (juce::jlimit (0.0, juce::jmax (0.0, length() - bar), current + (delta > 0.0f ? -bar : bar) * (wheel.isReversed ? -1.0 : 1.0)));
    update();
}

ui::Hint SongPositionBar::hintAt (juce::Point<int>)
{
    const auto& song = engine.getSong();
    ui::Hint h;
    h.text = (song.mode == PlayMode::song ? tr ("Song position") : tr ("Pattern position"))
             + ": " + tr ("[LMB] click or drag to move the playhead  [Wheel] one bar  [Alt] no snap");
    const auto bars = juce::jmax (1, juce::roundToInt (length() / ((double) ppq * juce::jmax (1, song.beatsPerBar))));
    h.value = tr ("Bar {0} of {1}", { barBeatStep (song, position(), song.beatsPerBar).upToFirstOccurrenceOf (" ", false, false), juce::String (bars) });
    return h;
}

// --- time display --------------------------------------------------------------------------------

juce::String TimeDisplay::text() const
{
    const auto& song = engine.getSong();
    const auto ticks = playheadTicks (engine);

    if (mode == Mode::bars)
        return barBeatStep (song, ticks, engine.getTransport().beatsPerBar.load());

    const auto seconds = song.secondsForTicks ((std::int64_t) ticks);
    if (mode == Mode::smpte)
        return juce::String (timecode::format (seconds, smpteRate));
    const auto whole = (int) seconds;
    return juce::String::formatted ("%d : %02d : %02d", whole / 60, whole % 60, (int) ((seconds - whole) * 100.0));
}

void TimeDisplay::update()
{
    if (const auto t = text(); t != shown)
    {
        shown = t;
        repaint();
    }
}

void TimeDisplay::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto area = getLocalBounds();
    g.setColour (colour::bg1);
    g.fillRoundedRectangle (area.toFloat(), radius::md);

    if (isMouseOver())
    {
        g.setColour (colour::borderStrong);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), radius::md, 1.0f);
    }

    g.setColour (colour::textPrimary);
    g.setFont (monoFont (juce::jmin (17.0f, (float) area.getHeight() * 0.45f)));
    g.drawText (shown.isEmpty() ? text() : shown, area.withTrimmedBottom (11), juce::Justification::centred, false);
    g.setColour (colour::textDisabled);
    g.setFont (font (type::caption - 1.0f));
    g.drawText (mode == Mode::clock   ? tr ("MIN : SEC : CS")
                : mode == Mode::smpte ? "SMPTE " + juce::String (timecode::rates[smpteRate].name)
                                      : tr ("BAR : BEAT : STEP"), area.withTrimmedTop (area.getHeight() - 14).withTrimmedBottom (2),
                juce::Justification::centred, false);
}

void TimeDisplay::mouseUp (const juce::MouseEvent& e)
{
    if (! getLocalBounds().contains (e.getPosition()))
        return;
    if (e.mods.isPopupMenu())
    {
        showMenu();
        return;
    }

    mode = mode == Mode::bars ? Mode::clock : mode == Mode::clock ? Mode::smpte : Mode::bars;
    shown = {};
    update();
    repaint();
}

void TimeDisplay::showMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Mode"));
    auto set = [this] (Mode m, int rate) {
        mode = m;
        if (rate >= 0)
            smpteRate = rate;
        shown = {};
        update();
        repaint();
    };
    menu.addItem (tr ("Bar : Beat : Step"), true, mode == Mode::bars, [set] { set (Mode::bars, -1); });
    menu.addItem (tr ("Minute : Second : Centisecond"), true, mode == Mode::clock, [set] { set (Mode::clock, -1); });
    for (int i = 0; i < timecode::rateCount; ++i)
        menu.addItem ("SMPTE " + juce::String (timecode::rates[i].name) + " fps", true, mode == Mode::smpte && smpteRate == i, [set, i] { set (Mode::smpte, i); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

ui::Hint TimeDisplay::hintAt (juce::Point<int>)
{
    ui::Hint h;
    h.text = (mode == Mode::clock   ? tr ("Time: minutes : seconds : hundredths")
              : mode == Mode::smpte ? tr ("Time: SMPTE time code (hours : minutes : seconds : frames)")
                                    : tr ("Time: bar : beat : step"))
             + "  " + tr ("[LMB] next mode (bars, minutes, SMPTE)  [RMB] mode and frame rate");
    h.value = text();
    return h;
}

// --- master volume and pitch ---------------------------------------------------------------------

MasterKnob::MasterKnob (AudioEngine& e, Kind k) : engine (e), kind (k)
{
    setRepaintsOnMouseActivity (true);
}

bool MasterKnob::available() const
{
    return kind == Kind::pitch || ! engine.getSong().mixer.empty();
}

double MasterKnob::value() const
{
    const auto& song = engine.getSong();

    if (kind == Kind::pitch)
        return song.masterPitch;

    return song.mixer.empty() ? (double) unityFader : (double) song.mixer.front().volume;
}

void MasterKnob::setValue (double v)
{
    const auto s = spec();
    v = juce::jlimit (s.minimum, s.maximum, v);

    if (! available() || std::abs (v - value()) < 1e-9)
        return;

    if (kind == Kind::pitch)
        engine.editSong ([v] (Song& song) { song.masterPitch = (float) v; });
    else
        engine.editSong ([v] (Song& song) {
            if (! song.mixer.empty())
                song.mixer.front().volume = (float) v;
        });

    update();
}

ui::ValueSpec MasterKnob::spec() const
{
    if (kind == Kind::volume)
    {
        auto s = ui::faderSpec (tr ("Master volume"), unityFader);
        s.target = AutomationTarget { AutomationTargetKind::mixerVolume, 0 };
        return s;
    }

    ui::ValueSpec s;
    s.name = tr ("Master pitch");
    s.minimum = -1200.0;
    s.maximum = 1200.0;
    s.defaultValue = 0.0;
    s.pixelsForRange = 360.0;
    s.toText = [] (double v) { return centsText (v); };
    s.fromText = [] (const juce::String& t) -> std::optional<double> {
        const auto number = t.retainCharacters ("+-0123456789.");
        if (number.isEmpty())
            return std::nullopt;
        return number.getDoubleValue();
    };
    return s;
}

void MasterKnob::update()
{
    if (const auto v = value(); v != shown)
    {
        shown = v;
        repaint();
    }
}

void MasterKnob::paint (juce::Graphics& g)
{
    using namespace theme;
    auto area = getLocalBounds().toFloat();
    const auto knob = area.removeFromLeft (juce::jmin (area.getHeight(), 26.0f));
    const auto s = spec();
    const auto v = value();
    const auto hover = isMouseOver() || drag.isActive();
    const auto value01 = (float) ((v - s.minimum) / (s.maximum - s.minimum));
    drawKnob (g, knob, value01, kind == Kind::pitch, available() ? colour::knobArc : colour::textDisabled, hover && available());

    area.removeFromLeft (5.0f);
    auto caption = area.removeFromTop (area.getHeight() / 2.0f);
    g.setColour (colour::textDisabled);
    g.setFont (font (type::caption - 1.0f, true));
    g.drawText (kind == Kind::volume ? tr ("MASTER") : tr ("PITCH"), caption.withTrimmedTop (1.0f), juce::Justification::bottomLeft, true);
    g.setColour (drag.isActive() ? colour::textPrimary : colour::textSecondary);
    g.setFont (font (type::caption));
    g.drawText (available() ? s.text (v) : tr ("no mixer"), area, juce::Justification::topLeft, true);
}

void MasterKnob::mouseDown (const juce::MouseEvent& e)
{
    if (! available())
        return;

    if (e.mods.isPopupMenu())
    {
        ui::showValueMenu (*this, kind == Kind::volume ? &engine : nullptr, spec(), value(), [this] (double v) { setValue (v); });
        return;
    }

    if (ui::isResetClick (e))
    {
        setValue (spec().defaultValue);
        return;
    }

    drag.begin (e, value());
    repaint();
}

void MasterKnob::mouseDrag (const juce::MouseEvent& e)
{
    if (! drag.isActive())
        return;

    auto v = drag.update (e, spec());

    if (kind == Kind::pitch && ! e.mods.isShiftDown()) // semitone steps; Shift: free (as FL's master pitch)
        v = std::round (v / 100.0) * 100.0;

    setValue (v);
}

void MasterKnob::mouseUp (const juce::MouseEvent&)
{
    drag.end();
    repaint();
}

void MasterKnob::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (available() && ! e.mods.isPopupMenu() && ! ui::isResetClick (e))
        ui::typeValue (*this, spec(), value(), [this] (double v) { setValue (v); });
}

void MasterKnob::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (! available())
        return;

    if (kind == Kind::pitch)
    {
        const auto delta = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : -wheel.deltaX;
        if (delta != 0.0f)
            setValue (value() + (delta > 0.0f ? 1.0 : -1.0) * (wheel.isReversed ? -1.0 : 1.0) * (e.mods.isCtrlDown() ? 1.0 : 100.0));
        return;
    }

    if (const auto v = ui::wheelValue (value(), e, wheel, spec()))
        setValue (*v);
}

ui::Hint MasterKnob::hintAt (juce::Point<int>)
{
    if (! available())
        return { tr ("Master volume: this project has no mixer"), {}, false, false };

    const auto gesture = kind == Kind::volume
                             ? tr ("level of everything you hear (the Master insert's fader)") + "  " + ui::valueGestures()
                             : tr ("tunes the built-in instruments  [LMB] drag (semitones)  [Shift] cents  [Wheel] semitone  [Ctrl][Wheel] cent  [Alt][LMB] reset  [RMB] menu");
    return ui::valueHint (spec(), value(), gesture);
}

// --- CPU, memory, polyphony ----------------------------------------------------------------------

void PerformanceMeter::setValues (double cpu01, std::int64_t memoryBytes, int polyphony)
{
    cpu = cpu01;
    cpuSmoothed = cpuSmoothed <= 0.0 ? cpu01 : cpuSmoothed * 0.8 + cpu01 * 0.2;
    memory = memoryBytes;
    voices = polyphony;
    peakVoices = juce::jmax (peakVoices, polyphony);
    repaint();
}

void PerformanceMeter::setRealtime (std::uint64_t over, std::uint64_t late, double peak, const juce::StringArray& units, int device)
{
    const auto count = RtStats::underrunCount (over, late, device);
    if (over > overDeadline || late > lateCalls || count > underruns)
        lastGlitchAt = juce::jmax<juce::uint32> (1, juce::Time::getMillisecondCounter());
    overDeadline = over;
    lateCalls = late;
    deviceUnderruns = device;
    underruns = count;
    peakLoad = peak;
    heaviest = units;
    repaint();
}

void PerformanceMeter::paint (juce::Graphics& g)
{
    using namespace theme;
    auto area = getLocalBounds();
    auto row1 = area.removeFromTop (area.getHeight() / 2).reduced (0, 4);
    auto row2 = area.reduced (0, 4);
    const juce::Font caption (font (type::caption - 1.0f, true)), number (font (type::caption));

    // CPU: caption, bar, percent
    g.setFont (caption);
    g.setColour (colour::textDisabled);
    g.drawText (tr ("CPU"), row1.removeFromLeft (30), juce::Justification::centredLeft, false);
    g.setFont (number);
    g.setColour (colour::textSecondary);
    g.drawText (juce::String (juce::roundToInt (cpuSmoothed * 100.0)) + "%", row1.removeFromRight (34), juce::Justification::centredRight, false);
    if (underruns > 0) // FL's underrun counter, next to the percentage
    {
        g.setColour (colour::meterClip);
        g.drawText (underruns > 999 ? juce::String ("999+") : juce::String ((juce::int64) underruns), row1.removeFromRight (26),
                    juce::Justification::centredRight, false);
    }
    const auto bar = row1.reduced (2, 3).toFloat();
    g.setColour (colour::bg0);
    g.fillRoundedRectangle (bar, 2.0f);
    const auto load = (float) juce::jlimit (0.0, 1.0, cpuSmoothed);
    g.setColour (load < 0.6f ? colour::meterLow : load < 0.85f ? colour::meterMid : colour::meterClip);
    if (load > 0.0f)
        g.fillRoundedRectangle (bar.withWidth (juce::jmax (2.0f, bar.getWidth() * load)), 2.0f);
    // the highest callback load since the last update (a thin mark), and a red edge for a few seconds after a
    // callback missed its deadline (an audible crackle)
    if (peakLoad > 0.0)
    {
        const auto x = bar.getX() + bar.getWidth() * (float) juce::jlimit (0.0, 1.0, peakLoad);
        g.setColour (peakLoad < 0.85 ? colour::textSecondary : colour::meterClip);
        g.fillRect (juce::Rectangle<float> (x - 1.0f, bar.getY(), 2.0f, bar.getHeight()));
    }
    if (lastGlitchAt != 0 && juce::Time::getMillisecondCounter() - lastGlitchAt < 5000)
    {
        g.setColour (colour::meterClip);
        g.drawRoundedRectangle (bar.expanded (1.0f), 2.0f, 1.0f);
    }

    // memory and polyphony
    auto left = row2.removeFromLeft (row2.getWidth() * 58 / 100);
    g.setFont (caption);
    g.setColour (colour::textDisabled);
    g.drawText (tr ("RAM"), left.removeFromLeft (30), juce::Justification::centredLeft, false);
    g.setFont (number);
    g.setColour (colour::textSecondary);
    g.drawText (memory > 0 ? juce::String (juce::roundToInt ((double) memory / (1024.0 * 1024.0))) + " MB" : juce::String ("-"), left,
                juce::Justification::centredLeft, false);
    g.setFont (caption);
    g.setColour (colour::textDisabled);
    g.drawText (tr ("POLY"), row2.removeFromLeft (32), juce::Justification::centredLeft, false);
    g.setFont (number);
    g.setColour (voices > 0 ? colour::textPrimary : colour::textSecondary);
    g.drawText (juce::String (voices), row2, juce::Justification::centredRight, false);
}

// --- MIDI activity light ---------------------------------------------------------------------------

void MidiActivityLight::update (std::uint32_t count, const juce::StringArray& activeInputs)
{
    const auto wasLit = litFrames > 0;
    if (seenCount && count != lastCount)
        litFrames = 6; // ~0.2 s after the last message
    else if (litFrames > 0)
        --litFrames;
    seenCount = true;
    lastCount = count;

    if (activeInputs != inputs)
    {
        inputs = activeInputs;
        setTooltip (hintAt ({}).text);
    }

    if (wasLit != (litFrames > 0))
        repaint();
}

void MidiActivityLight::paint (juce::Graphics& g)
{
    const auto size = juce::jmin ((float) getWidth(), (float) getHeight()) * 0.36f;
    const auto led = getLocalBounds().toFloat().withSizeKeepingCentre (size, size);
    const auto lit = litFrames > 0;

    if (lit) // a soft glow around the lit light
    {
        g.setColour (theme::colour::ledOn.withAlpha (0.25f));
        g.fillEllipse (led.expanded (size * 0.35f));
    }
    g.setColour (lit ? theme::colour::ledOn : (inputs.isEmpty() ? theme::colour::bg4 : theme::colour::ledOff));
    g.fillEllipse (led);
    g.setColour (theme::colour::borderStrong);
    g.drawEllipse (led, 1.0f);
}

void MidiActivityLight::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onReconnect != nullptr)
        onReconnect();
}

ui::Hint MidiActivityLight::hintAt (juce::Point<int>)
{
    ui::Hint h;
    h.text = tr ("MIDI input: blinks when a connected device sends MIDI. Click to reconnect the MIDI inputs.");
    h.text << "  " << (inputs.isEmpty() ? tr ("No MIDI input is open.") : tr ("Open: {0}", { inputs.joinIntoString (", ") }));
    return h;
}

ui::Hint PerformanceMeter::hintAt (juce::Point<int> p)
{
    ui::Hint h;

    if (p.y < getHeight() / 2)
    {
        h.text = tr ("CPU: how much of the audio deadline the processing takes; near 100% the sound breaks up");
        if (underruns > 0)
            h.text << "  " << tr ("{0} underruns (the sound broke up; red number)", { juce::String ((juce::int64) underruns) });
        if (overDeadline > 0 || lateCalls > 0)
            h.text << "  " << tr ("{0} audio blocks missed the deadline (crackles), the device called late {1} times; log: audio-glitches.log",
                                  { juce::String ((juce::int64) overDeadline), juce::String ((juce::int64) lateCalls) });
        if (! heaviest.isEmpty())
            h.text << "  " << tr ("Heaviest: {0}", { heaviest.joinIntoString (", ") });
        if (underruns > 0 && driverTip.isNotEmpty())
            h.text << "  " << tr ("Tip: Windows Audio (shared) drops out more easily; choose the ASIO driver \"{0}\" in Options > Audio settings.",
                                  { driverTip });
        h.value = juce::String (cpu * 100.0, 1) + "%" + (peakLoad > 0.0 ? "  (" + tr ("peak {0}%", { juce::String (juce::roundToInt (peakLoad * 100.0)) }) + ")" : juce::String());
    }
    else if (p.x < getWidth() * 58 / 100)
    {
        h.text = tr ("RAM: memory used by LYNX SHARE (samples, plugins, undo history)");
        h.value = juce::String ((double) memory / (1024.0 * 1024.0), 0) + " MB";
    }
    else
    {
        h.text = tr ("Polyphony: notes sounding now in the song and from the keyboard (voices still releasing are not counted)");
        h.value = juce::String (voices) + "  (" + tr ("max {0}", { juce::String (peakVoices) }) + ")";
    }

    return h;
}

// --- output monitor ------------------------------------------------------------------------------

void OutputMonitor::update()
{
    if (! isShowing() || getWidth() <= 0)
        return;

    engine.copyScope (samples.data(), (int) samples.size());
    float now = 0.0f;
    for (auto s : samples)
        now = juce::jmax (now, std::abs (s));

    peak = juce::jmax (now, peak * 0.82f); // fast attack, smooth fall

    if (now >= held || holdFrames <= 0)
    {
        held = now;
        holdFrames = 30; // about a second at 30 Hz
    }
    else
        --holdFrames;

    if (now >= 0.999f)
        clipFrames = 60;
    else if (clipFrames > 0)
        --clipFrames;

    // silence after everything has fallen to zero (meter, hold, clip, the long view's history, the spectrum bands)
    // looks the same frame after frame: stop repainting at 30 Hz while nothing plays (REA round 116, idle CPU)
    silentFrames = now > 0.0f ? 0 : juce::jmin (silentFrames + 1, 1 << 20);
    if (silentFrames > (int) history.size() + 90)
    {
        peak = held = 0.0f;
        bands.fill (0.0f);
        return;
    }

    // long view: the level scrolls from right to left
    std::rotate (history.begin(), history.begin() + 1, history.end());
    history.back() = now;

    if (mode == spectrum)
    {
        // Hann-windowed FFT of the last 1024 samples, folded into log-spaced bands (20 Hz..20 kHz)
        static juce::dsp::FFT fft (10);
        std::array<float, 2048> data {};
        for (std::size_t i = 0; i < samples.size(); ++i)
            data[i] = samples[i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (samples.size() - 1)));
        fft.performFrequencyOnlyForwardTransform (data.data(), true);

        const auto rate = juce::jmax (8000.0, engine.getTransport().sampleRate.load());
        for (std::size_t b = 0; b < bands.size(); ++b)
        {
            const auto f0 = 20.0 * std::pow (1000.0, (double) b / (double) bands.size());
            const auto f1 = 20.0 * std::pow (1000.0, (double) (b + 1) / (double) bands.size());
            auto i0 = (int) (f0 / rate * 1024.0), i1 = juce::jmax (i0 + 1, (int) (f1 / rate * 1024.0));
            i0 = juce::jlimit (1, 511, i0);
            i1 = juce::jlimit (i0 + 1, 512, i1);
            float m = 0.0f;
            for (auto i = i0; i < i1; ++i)
                m = juce::jmax (m, data[(std::size_t) i]);
            const auto db = juce::Decibels::gainToDecibels (m / 256.0f, -72.0f);
            const auto level = juce::jlimit (0.0f, 1.0f, (db + 72.0f) / 72.0f);
            bands[b] = juce::jmax (level, bands[b] * 0.85f); // fast up, smooth fall
        }
    }

    repaint();
}

juce::String OutputMonitor::modeName (int m)
{
    switch (m)
    {
        case longView: return tr ("Long view (level over time)");
        case spectrum: return tr ("Spectrum (low frequencies on the left)");
        default: return tr ("Oscilloscope (waveform)");
    }
}

void OutputMonitor::setMode (int newMode)
{
    mode = juce::jlimit (0, modeCount - 1, newMode);
    bands.fill (0.0f);
    repaint();
    if (onModeChanged)
        onModeChanged();
}

void OutputMonitor::mouseDown (const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Output display"));
        for (int m = 0; m < modeCount; ++m)
            menu.addItem (modeName (m), true, mode == m, [this, m] { setMode (m); });
        menu.addSeparator();
        menu.addItem (tr ("Reset the peak hold and clip light"), [this] {
            held = 0.0f;
            clipFrames = holdFrames = 0;
            repaint();
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
        return;
    }

    if (e.mods.isLeftButtonDown())
        setMode ((mode + 1) % modeCount);
}

void OutputMonitor::paint (juce::Graphics& g)
{
    using namespace theme;
    auto area = getLocalBounds().toFloat();
    g.setColour (colour::bg0);
    g.fillRoundedRectangle (area, radius::md);

    // peak meter on the right: -60..0 dB, the top segment red while it clipped recently
    auto meter = area.removeFromRight (8.0f).reduced (1.5f, 4.0f);
    auto level = [] (float gain) { return juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (gain, -60.0f) + 60.0f) / 60.0f); };
    g.setColour (colour::bg3);
    g.fillRoundedRectangle (meter, 1.5f);
    const auto filled = meter.withTop (meter.getBottom() - meter.getHeight() * level (peak));
    g.setGradientFill (juce::ColourGradient::vertical (colour::meterMid, meter.getY() + meter.getHeight() * 0.1f, colour::meterLow,
                                                       meter.getY() + meter.getHeight() * 0.4f));
    g.fillRect (filled);
    if (held > 0.0f)
    {
        g.setColour (colour::textSecondary);
        g.fillRect (meter.withHeight (1.0f).withY (meter.getBottom() - meter.getHeight() * level (held)));
    }
    if (clipFrames > 0)
    {
        g.setColour (colour::meterClip);
        g.fillRoundedRectangle (meter.withHeight (4.0f), 1.0f);
    }

    auto scope = area.reduced (4.0f, 3.0f);
    const auto lineColour = (clipFrames > 0 ? colour::meterClip : colour::accent).withAlpha (0.9f);

    if (mode == longView) // the level of the last seconds, newest on the right, mirrored around the centre
    {
        g.setColour (colour::border);
        g.drawHorizontalLine ((int) scope.getCentreY(), scope.getX(), scope.getRight());
        const auto columns = juce::jmax (1, (int) scope.getWidth());
        g.setColour (lineColour.withAlpha (0.75f));
        for (int x = 0; x < columns; ++x)
        {
            const auto index = (std::size_t) (x * (int) history.size() / columns);
            const auto h = juce::jmax (0.5f, level (history[index]) * scope.getHeight() * 0.5f);
            g.fillRect (scope.getX() + (float) x, scope.getCentreY() - h, 1.0f, h * 2.0f);
        }
        return;
    }

    if (mode == spectrum) // bars per log-spaced band
    {
        const auto w = scope.getWidth() / (float) bands.size();
        g.setColour (lineColour.withAlpha (0.8f));
        for (std::size_t b = 0; b < bands.size(); ++b)
        {
            const auto h = bands[b] * scope.getHeight();
            g.fillRect (scope.getX() + (float) b * w, scope.getBottom() - h, juce::jmax (1.0f, w - 0.5f), h);
        }
        return;
    }

    // oscilloscope
    g.setColour (colour::border);
    g.drawHorizontalLine ((int) scope.getCentreY(), scope.getX(), scope.getRight());
    juce::Path wave;
    const auto mid = scope.getCentreY();
    const auto half = scope.getHeight() * 0.47f;
    const auto width = juce::jmax (1, (int) scope.getWidth());

    for (int x = 0; x < width; ++x)
    {
        const auto index = (std::size_t) (x * (int) samples.size() / width);
        const auto y = mid - juce::jlimit (-1.0f, 1.0f, samples[index]) * half;

        if (x == 0)
            wave.startNewSubPath (scope.getX(), y);
        else
            wave.lineTo (scope.getX() + (float) x, y);
    }

    g.setColour (lineColour);
    g.strokePath (wave, juce::PathStrokeType (1.3f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

ui::Hint OutputMonitor::hintAt (juce::Point<int>)
{
    ui::Hint h;
    h.text = tr ("Output: {0} and the peak level of what you hear (the meter's top turns red when it clipped)  [LMB] next display  [RMB] display menu",
                 { modeName (mode).toLowerCase() });
    h.hasMenu = true;
    h.value = held <= 1.0e-5f ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (held), 1) + " dB";
    return h;
}

// --- customize -------------------------------------------------------------------------------------

CustomizePanel::CustomizePanel (const toolbarlayout::Layout& l) : layout (l)
{
    setWantsKeyboardFocus (true);
    upButton.setButtonText (tr ("Move up"));
    downButton.setButtonText (tr ("Move down"));
    resetButton.setButtonText (tr ("Reset to default"));
    upButton.onClick = [this] { move (-1); };
    downButton.onClick = [this] { move (1); };
    resetButton.onClick = [this] {
        layout = toolbarlayout::Layout::makeDefault();
        iconSizeBox.setSelectedId ((int) layout.iconSize + 1, juce::dontSendNotification);
        scaleBox.setSelectedId (layout.uiScale, juce::dontSendNotification);
        rebuild();
        changed();
    };

    iconSizeBox.addItem (tr ("Small icons"), 1);
    iconSizeBox.addItem (tr ("Medium icons"), 2);
    iconSizeBox.addItem (tr ("Large icons"), 3);
    iconSizeBox.setSelectedId ((int) layout.iconSize + 1, juce::dontSendNotification);
    iconSizeBox.onChange = [this] {
        layout.iconSize = (toolbarlayout::IconSize) (iconSizeBox.getSelectedId() - 1);
        changed();
    };

    for (auto percent : toolbarlayout::uiScales)
        scaleBox.addItem (juce::String (percent) + "%", percent);
    if (scaleBox.indexOfItemId (layout.uiScale) < 0)
        scaleBox.addItem (juce::String (layout.uiScale) + "%", layout.uiScale);
    scaleBox.setSelectedId (layout.uiScale, juce::dontSendNotification);
    scaleBox.onChange = [this] {
        layout.uiScale = toolbarlayout::clampScale (scaleBox.getSelectedId());
        changed();
    };

    for (auto* c : std::initializer_list<juce::Component*> { &upButton, &downButton, &resetButton, &iconSizeBox, &scaleBox })
        addAndMakeVisible (c);

    ui::setHint (iconSizeBox, tr ("Size of the toolbar icons and of the editors' tool icons (Piano Roll, Playlist)"));
    ui::setHint (scaleBox, tr ("Scales the whole interface: text, icons and windows. Useful on high-resolution screens"));
    rebuild();
    setSize (460, 2 * 16 + 24 + toolbarlayout::groupCount * rowHeight + rowHeight + 16 + 2 * 34 + 12 + 30);
}

juce::Rectangle<int> CustomizePanel::listArea() const
{
    return getLocalBounds().reduced (16).withTrimmedTop (24).withHeight ((toolbarlayout::groupCount + 1) * rowHeight).withTrimmedRight (140);
}

void CustomizePanel::rebuild()
{
    checks.clear();

    for (const auto& entry : layout.entries)
    {
        auto* check = checks.add (new juce::ToggleButton (tr (toolbarlayout::groupName (entry.group))));
        check->setToggleState (entry.shown, juce::dontSendNotification);
        check->setWantsKeyboardFocus (false);
        const auto group = entry.group;
        check->onClick = [this, check, group] {
            layout.setShown (group, check->getToggleState());
            select (group);
            changed();
        };
        // the transport can be hidden, but it is the last group to drop when the window is narrow
        addAndMakeVisible (check);
    }

    resized();
    repaint();
}

void CustomizePanel::select (toolbarlayout::Group g)
{
    selected = g;
    const auto at = layout.indexOf (g);
    upButton.setEnabled (at > 0);
    downButton.setEnabled (at >= 0 && at < (int) layout.entries.size() - 1);
    repaint();
}

void CustomizePanel::changed()
{
    if (onChange)
        onChange (layout);
}

void CustomizePanel::move (int delta)
{
    if (layout.move (selected, delta))
    {
        rebuild();
        select (selected);
        changed();
    }
}

void CustomizePanel::resized()
{
    auto list = listArea();
    list.removeFromTop (rowHeight); // the fixed first row: the menu and the hint bar

    for (auto* check : checks)
        check->setBounds (list.removeFromTop (rowHeight).reduced (8, 3));

    auto side = getLocalBounds().reduced (16).withTrimmedTop (24 + rowHeight).withLeft (listArea().getRight() + 12);
    upButton.setBounds (side.removeFromTop (28));
    side.removeFromTop (6);
    downButton.setBounds (side.removeFromTop (28));

    auto bottom = getLocalBounds().reduced (16).withTop (listArea().getBottom() + 16);
    auto row = bottom.removeFromTop (28);
    row.removeFromLeft (110);
    iconSizeBox.setBounds (row.removeFromLeft (140));
    bottom.removeFromTop (6);
    row = bottom.removeFromTop (28);
    row.removeFromLeft (110);
    scaleBox.setBounds (row.removeFromLeft (140));
    bottom.removeFromTop (12);
    resetButton.setBounds (bottom.removeFromTop (28).removeFromLeft (160));
    select (selected);
}

void CustomizePanel::paint (juce::Graphics& g)
{
    using namespace theme;
    g.fillAll (colour::bg2);
    auto area = getLocalBounds().reduced (16);
    g.setFont (font (type::small, true));
    g.setColour (colour::textSecondary);
    g.drawText (tr ("Toolbar groups, left to right (uncheck to hide):"), area.removeFromTop (20), juce::Justification::centredLeft);

    auto list = listArea();
    g.setColour (colour::bg1);
    g.fillRoundedRectangle (list.toFloat(), radius::md);

    auto first = list.removeFromTop (rowHeight).reduced (8, 3);
    g.setFont (font (type::body));
    g.setColour (colour::textDisabled);
    g.drawText (tr ("Menu and hint bar (always first)"), first.withTrimmedLeft (28), juce::Justification::centredLeft);

    const auto at = layout.indexOf (selected);
    if (at >= 0)
    {
        g.setColour (colour::accent.withAlpha (0.18f));
        g.fillRoundedRectangle (list.withY (list.getY() + at * rowHeight).withHeight (rowHeight).reduced (2).toFloat(), radius::md);
    }

    auto bottom = getLocalBounds().reduced (16).withTop (listArea().getBottom() + 16);
    g.setColour (colour::textSecondary);
    g.drawText (tr ("Icon size"), bottom.removeFromTop (28).withWidth (104), juce::Justification::centredLeft);
    bottom.removeFromTop (6);
    g.drawText (tr ("UI scale"), bottom.removeFromTop (28).withWidth (104), juce::Justification::centredLeft);
}

void CustomizePanel::mouseDown (const juce::MouseEvent& e)
{
    auto list = listArea();
    list.removeFromTop (rowHeight);

    if (list.contains (e.getPosition()))
    {
        const auto row = (e.y - list.getY()) / rowHeight;
        if (row >= 0 && row < (int) layout.entries.size())
            select (layout.entries[(std::size_t) row].group);
    }
}

bool CustomizePanel::keyPressed (const juce::KeyPress& key)
{
    const auto up = key.getKeyCode() == juce::KeyPress::upKey, down = key.getKeyCode() == juce::KeyPress::downKey;
    if (! up && ! down)
        return false;

    if (key.getModifiers().isAltDown())
        move (up ? -1 : 1);
    else
    {
        const auto at = juce::jlimit (0, (int) layout.entries.size() - 1, layout.indexOf (selected) + (up ? -1 : 1));
        select (layout.entries[(std::size_t) at].group);
    }
    return true;
}

// --- edit mode ---------------------------------------------------------------------------------------

Icon EditOverlay::groupIcon (toolbarlayout::Group g)
{
    using toolbarlayout::Group;
    switch (g)
    {
        case Group::transport: return Icon::play;
        case Group::master: return Icon::tuningFork;
        case Group::record: return Icon::metronome;
        case Group::snap: return Icon::magnet;
        case Group::windows: return Icon::playlist;
        case Group::file: return Icon::save;
        case Group::meters: return Icon::settings;
        case Group::output: return Icon::listen;
        case Group::count_: break;
    }
    return Icon::none;
}

EditOverlay::EditOverlay (Host h, int height) : host (std::move (h)), toolbarHeight (height)
{
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::DraggingHandCursor);

    for (int p = 0; p < toolbarlayout::presetCount; ++p)
        presetBox.addItem (tr (toolbarlayout::presetName ((toolbarlayout::Preset) p)), p + 1);
    presetBox.setTextWhenNothingSelected (tr ("Custom"));
    presetBox.onChange = [this] {
        const auto id = presetBox.getSelectedId();
        if (id > 0)
            apply (toolbarlayout::applyPreset (host.layout(), (toolbarlayout::Preset) (id - 1)));
    };
    ui::setHint (presetBox, tr ("Toolbar presets: which panels show (LYNX SHARE's own layouts)"), false);
    resetButton.setButtonText (tr ("Reset"));
    resetButton.onClick = [this] {
        auto l = toolbarlayout::Layout::makeDefault();
        const auto now = host.layout();
        l.iconSize = now.iconSize;
        l.uiScale = now.uiScale;
        apply (l);
    };
    ui::setHint (resetButton, tr ("Every panel back, in the default order"));
    doneButton.setButtonText (tr ("Done"));
    doneButton.onClick = [this] {
        if (host.done)
            host.done();
    };
    ui::setHint (doneButton, tr ("Leave the toolbar's edit mode  [Esc]"));

    for (auto* c : std::initializer_list<juce::Component*> { &presetBox, &resetButton, &doneButton })
    {
        c->setMouseCursor (juce::MouseCursor::NormalCursor);
        addAndMakeVisible (c);
    }
    layoutChanged();
}

void EditOverlay::setToolbarHeight (int height)
{
    toolbarHeight = height;
    resized();
    repaint();
}

void EditOverlay::layoutChanged()
{
    const auto p = toolbarlayout::matchingPreset (host.layout());
    presetBox.setSelectedId (p < toolbarlayout::presetCount ? p + 1 : 0, juce::dontSendNotification);
    repaint();
}

void EditOverlay::resized()
{
    auto row = trayArea().reduced (12, 0).removeFromBottom (40).withSizeKeepingCentre (getWidth() - 24, 28);
    doneButton.setBounds (row.removeFromRight (90));
    row.removeFromRight (8);
    resetButton.setBounds (row.removeFromRight (90));
    row.removeFromRight (8);
    presetBox.setBounds (row.removeFromRight (190));
}

juce::Rectangle<int> EditOverlay::chipArea() const
{
    auto area = trayArea().reduced (12, 0);
    area.removeFromTop (22);
    area.removeFromBottom (6);
    return area.withTrimmedRight (390 + 12).withHeight (juce::jmin (area.getHeight(), 60));
}

juce::Rectangle<int> EditOverlay::chipBounds (int index) const
{
    const auto area = chipArea();
    const auto w = juce::jlimit (60, 132, area.getWidth() / toolbarlayout::groupCount);
    return { area.getX() + index * w, area.getY(), w - 6, area.getHeight() };
}

int EditOverlay::panelAt (juce::Point<int> p) const
{
    if (p.y >= toolbarHeight)
        return -1;
    const auto panels = host.panels();
    for (int g = 0; g < toolbarlayout::groupCount; ++g)
        if (panels[(std::size_t) g].contains (p))
            return g;
    return -1;
}

int EditOverlay::chipAt (juce::Point<int> p) const
{
    for (int g = 0; g < toolbarlayout::groupCount; ++g)
        if (chipBounds (g).contains (p))
            return g;
    return -1;
}

int EditOverlay::dropX() const
{
    if (! dragging || pressedGroup < 0 || dragPos.y >= toolbarHeight || dragPos.y < 0)
        return -1;
    const auto panels = host.panels();
    const auto layout = host.layout();
    std::vector<std::pair<toolbarlayout::Group, int>> shown;
    int lastRight = -1;
    for (const auto& e : layout.entries)
        if (const auto& r = panels[(std::size_t) e.group]; ! r.isEmpty())
        {
            shown.push_back ({ e.group, r.getCentreX() });
            if (e.group != (toolbarlayout::Group) pressedGroup)
                lastRight = juce::jmax (lastRight, r.getRight());
        }
    const auto before = toolbarlayout::dropBefore (shown, (toolbarlayout::Group) pressedGroup, dragPos.x);
    if (before >= toolbarlayout::groupCount)
        return lastRight >= 0 ? lastRight + 4 : host.fixedArea().getRight() + 8;
    return panels[(std::size_t) before].getX() - 5;
}

void EditOverlay::apply (toolbarlayout::Layout layout)
{
    if (host.setLayout)
        host.setLayout (layout);
    layoutChanged();
}

void EditOverlay::move (toolbarlayout::Group g, int delta)
{
    // among the panels on the toolbar: before the previous one, or before the one after the next
    auto layout = host.layout();
    std::vector<toolbarlayout::Group> shown;
    for (const auto& e : layout.entries)
        if (e.shown)
            shown.push_back (e.group);
    const auto at = (int) (std::find (shown.begin(), shown.end(), g) - shown.begin());
    if (at >= (int) shown.size())
        return;
    if (delta < 0 && at > 0)
        layout.placeBefore (g, &shown[(std::size_t) at - 1]);
    else if (delta > 0 && at + 1 < (int) shown.size())
    {
        if (at + 2 < (int) shown.size())
            layout.placeBefore (g, &shown[(std::size_t) at + 2]);
        else
            layout.placeBefore (g, nullptr);
    }
    apply (layout);
}

void EditOverlay::showPanelMenu (toolbarlayout::Group g, bool inTray)
{
    const auto layout = host.layout();
    const auto shown = layout.isShown (g);
    juce::PopupMenu menu;
    menu.addSectionHeader (tr (toolbarlayout::groupName (g)));
    if (shown)
    {
        menu.addItem (tr ("Remove from the toolbar"), [this, g] {
            auto l = host.layout();
            l.setShown (g, false);
            apply (l);
        });
        menu.addItem (tr ("Move left"), ! inTray, false, [this, g] { move (g, -1); });
        menu.addItem (tr ("Move right"), ! inTray, false, [this, g] { move (g, 1); });
    }
    else
        menu.addItem (tr ("Add to the toolbar"), [this, g] {
            auto l = host.layout();
            l.setShown (g, true);
            apply (l);
        });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void EditOverlay::paint (juce::Graphics& g)
{
    using namespace theme;
    const auto layout = host.layout();
    const auto panels = host.panels();

    // the toolbar: a wash over it, each panel outlined with its name
    g.setColour (colour::bg0.withAlpha (0.45f));
    g.fillRect (0, 0, getWidth(), toolbarHeight);

    if (const auto fixed = host.fixedArea(); ! fixed.isEmpty())
    {
        g.setColour (colour::bg0.withAlpha (0.35f));
        g.fillRoundedRectangle (fixed.toFloat().reduced (1.0f), radius::md);
    }

    for (int i = 0; i < toolbarlayout::groupCount; ++i)
    {
        const auto r = panels[(std::size_t) i].toFloat().reduced (1.5f, 3.0f);
        if (r.isEmpty())
            continue;
        const auto isDragged = dragging && ! pressedInTray && pressedGroup == i;
        const auto hot = hoverPanel == i && ! dragging;
        g.setColour (isDragged ? colour::accent.withAlpha (0.10f) : colour::bg4.withAlpha (hot ? 0.35f : 0.15f));
        g.fillRoundedRectangle (r, radius::md);
        g.setColour (isDragged || hot ? colour::accent : colour::borderStrong);
        g.drawRoundedRectangle (r, radius::md, isDragged ? 1.5f : 1.0f);

        // the name, on a small tab at the top left
        const auto name = tr (toolbarlayout::groupName ((toolbarlayout::Group) i));
        const juce::Font f (font (type::caption - 1.0f, true));
        const auto w = juce::jmin ((int) r.getWidth() - 6, juce::GlyphArrangement::getStringWidthInt (f, name) + 10);
        const auto tab = juce::Rectangle<float> (r.getX() + 3.0f, r.getY() + 2.0f, (float) w, 13.0f);
        g.setColour (hot || isDragged ? colour::accent : colour::bg6);
        g.fillRoundedRectangle (tab, 3.0f);
        g.setColour (colour::onAccent);
        g.setFont (f);
        g.drawText (name, tab, juce::Justification::centred, true);
    }

    // where a dragged panel lands
    if (const auto x = dropX(); x >= 0)
    {
        g.setColour (colour::accent);
        g.fillRoundedRectangle ((float) x - 1.5f, 4.0f, 3.0f, (float) toolbarHeight - 8.0f, 1.5f);
    }

    // the tray
    const auto tray = trayArea();
    const auto overTray = dragging && ! pressedInTray && tray.contains (dragPos);
    g.setColour (colour::bg2);
    g.fillRect (tray);
    g.setColour (overTray ? colour::accent : colour::border);
    g.fillRect (tray.withHeight (overTray ? 2 : 1));

    g.setFont (font (type::small, true));
    g.setColour (overTray ? colour::accentHover : colour::textSecondary);
    g.drawText (overTray ? tr ("Release to remove this panel from the toolbar")
                         : tr ("Edit toolbar: drag panels to move them, drag a panel here to remove it, drag one from here onto the toolbar to add it"),
                tray.reduced (12, 0).withHeight (22), juce::Justification::centredLeft, true);

    for (int i = 0; i < toolbarlayout::groupCount; ++i)
    {
        const auto group = (toolbarlayout::Group) i;
        const auto shown = layout.isShown (group);
        const auto r = chipBounds (i).toFloat();
        const auto hot = hoverChip == i && ! dragging;
        g.setColour (shown ? colour::bg3 : colour::bg4);
        g.fillRoundedRectangle (r, radius::md);
        g.setColour (hot ? colour::accent : shown ? colour::border : colour::borderStrong);
        g.drawRoundedRectangle (r.reduced (0.5f), radius::md, 1.0f);

        auto inner = r.reduced (6.0f, 5.0f);
        const auto iconColour = shown ? colour::textDisabled : colour::textPrimary;
        icons::draw (g, groupIcon (group), inner.removeFromTop (18.0f).withSizeKeepingCentre (18.0f, 18.0f), iconColour);
        g.setFont (font (type::caption - 1.0f));
        g.setColour (shown ? colour::textDisabled : colour::textPrimary);
        g.drawFittedText (tr (toolbarlayout::groupName (group)), inner.toNearestInt(), juce::Justification::centred, 2, 0.85f);
        if (shown) // a tick: it is on the toolbar
        {
            g.setColour (colour::ledOn.withAlpha (0.8f));
            icons::draw (g, Icon::check, { r.getRight() - 15.0f, r.getY() + 3.0f, 12.0f, 12.0f }, colour::ledOn.withAlpha (0.8f));
        }
    }

    // the dragged panel follows the mouse
    if (dragging && dragImage.isValid())
    {
        g.setOpacity (0.85f);
        g.drawImageAt (dragImage, dragPos.x - grabOffset.x, dragPos.y - grabOffset.y);
    }
}

void EditOverlay::mouseMove (const juce::MouseEvent& e)
{
    const auto p = panelAt (e.getPosition()), c = chipAt (e.getPosition());
    if (p != hoverPanel || c != hoverChip)
    {
        hoverPanel = p;
        hoverChip = c;
        repaint();
    }
}

void EditOverlay::mouseExit (const juce::MouseEvent&)
{
    hoverPanel = hoverChip = -1;
    repaint();
}

void EditOverlay::mouseDown (const juce::MouseEvent& e)
{
    dragging = false;
    pressedGroup = panelAt (e.getPosition());
    pressedInTray = false;
    if (pressedGroup < 0)
    {
        pressedGroup = chipAt (e.getPosition());
        pressedInTray = pressedGroup >= 0;
    }

    if (pressedGroup >= 0 && e.mods.isPopupMenu())
    {
        showPanelMenu ((toolbarlayout::Group) pressedGroup, pressedInTray);
        pressedGroup = -1;
    }
}

void EditOverlay::mouseDrag (const juce::MouseEvent& e)
{
    if (pressedGroup < 0 || ! e.mods.isLeftButtonDown())
        return;

    if (! dragging && e.getDistanceFromDragStart() > 4)
    {
        dragging = true;
        const auto from = pressedInTray ? chipBounds (pressedGroup) : host.panels()[(std::size_t) pressedGroup];
        grabOffset = e.getMouseDownPosition() - from.getPosition();
        dragImage = ! pressedInTray && host.snapshot ? host.snapshot (from) : juce::Image();
        if (! dragImage.isValid()) // a chip: its own picture
        {
            dragImage = juce::Image (juce::Image::ARGB, from.getWidth(), from.getHeight(), true);
            juce::Graphics ig (dragImage);
            ig.setColour (theme::colour::bg4);
            ig.fillRoundedRectangle (dragImage.getBounds().toFloat(), theme::radius::md);
            ig.setColour (theme::colour::accent);
            ig.drawRoundedRectangle (dragImage.getBounds().toFloat().reduced (0.5f), theme::radius::md, 1.0f);
            icons::draw (ig, groupIcon ((toolbarlayout::Group) pressedGroup), { 6.0f, 5.0f, 18.0f, 18.0f }, theme::colour::textPrimary);
            ig.setFont (theme::font (theme::type::caption));
            ig.setColour (theme::colour::textPrimary);
            ig.drawFittedText (tr (toolbarlayout::groupName ((toolbarlayout::Group) pressedGroup)), dragImage.getBounds().reduced (6).withTrimmedTop (18),
                               juce::Justification::centred, 2);
        }
    }

    if (dragging)
    {
        dragPos = e.getPosition();
        repaint();
    }
}

void EditOverlay::mouseUp (const juce::MouseEvent& e)
{
    if (! dragging || pressedGroup < 0)
    {
        pressedGroup = -1;
        dragging = false;
        return;
    }

    const auto group = (toolbarlayout::Group) pressedGroup;
    const auto at = e.getPosition();
    auto layout = host.layout();

    if (at.y >= 0 && at.y < toolbarHeight) // onto the toolbar: shown, before the panel right of the drop point
    {
        const auto panels = host.panels();
        std::vector<std::pair<toolbarlayout::Group, int>> shown;
        for (const auto& entry : layout.entries)
            if (! panels[(std::size_t) entry.group].isEmpty())
                shown.push_back ({ entry.group, panels[(std::size_t) entry.group].getCentreX() });
        const auto before = toolbarlayout::dropBefore (shown, group, at.x);
        layout.setShown (group, true);
        if (before >= toolbarlayout::groupCount)
            layout.placeBefore (group, nullptr);
        else
        {
            const auto target = (toolbarlayout::Group) before;
            layout.placeBefore (group, &target);
        }
    }
    else if (! pressedInTray && trayArea().contains (at)) // into the tray: removed
        layout.setShown (group, false);

    dragging = false;
    pressedGroup = -1;
    dragImage = {};
    apply (layout);
    repaint();
}

void EditOverlay::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto c = chipAt (e.getPosition()); c >= 0)
    {
        auto layout = host.layout();
        layout.setShown ((toolbarlayout::Group) c, ! layout.isShown ((toolbarlayout::Group) c));
        apply (layout);
    }
}

bool EditOverlay::keyPressed (const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey)
    {
        if (host.done)
            host.done();
        return true;
    }
    return false;
}

ui::Hint EditOverlay::hintAt (juce::Point<int> p)
{
    ui::Hint h;
    h.hasMenu = true;
    if (const auto panel = panelAt (p); panel >= 0)
    {
        h.text = tr (toolbarlayout::groupName ((toolbarlayout::Group) panel)) + ": "
                 + tr ("[LMB] drag to move it  drag it into the tray to remove it  [RMB] menu");
        return h;
    }
    if (const auto chip = chipAt (p); chip >= 0)
    {
        const auto shown = host.layout().isShown ((toolbarlayout::Group) chip);
        h.text = tr (toolbarlayout::groupName ((toolbarlayout::Group) chip)) + ": "
                 + (shown ? tr ("on the toolbar  [2xLMB] remove it  [RMB] menu") : tr ("drag it onto the toolbar to add it there  [2xLMB] add it at the end  [RMB] menu"));
        return h;
    }
    if (p.y < toolbarHeight)
        h.text = tr ("Edit toolbar: the menu and the hint bar stay first  [Esc] done");
    else
        h.text = tr ("Edit toolbar: drag panels to and from the toolbar  [Esc] done");
    h.hasMenu = false;
    return h;
}
} // namespace lynx::toolbar

// The process memory comes from the OS; its headers go last (windows.h defines macros such as `small`).
#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #define PSAPI_VERSION 2 // GetProcessMemoryInfo from kernel32 (K32GetProcessMemoryInfo): no extra library
 #include <windows.h>
 #include <psapi.h>
#endif

std::int64_t lynx::toolbar::PerformanceMeter::processMemoryBytes()
{
#if JUCE_WINDOWS
    PROCESS_MEMORY_COUNTERS counters {};
    if (GetProcessMemoryInfo (GetCurrentProcess(), &counters, sizeof (counters)))
        return (std::int64_t) counters.WorkingSetSize;
    return 0;
#elif JUCE_LINUX
    // resident pages, from /proc
    const auto fields = juce::StringArray::fromTokens (juce::File ("/proc/self/statm").loadFileAsString(), " ", "");
    return fields.size() > 1 ? (std::int64_t) fields[1].getLargeIntValue() * 4096 : 0;
#else
    return 0;
#endif
}
