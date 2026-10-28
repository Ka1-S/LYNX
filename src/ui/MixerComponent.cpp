#include "platform/Paths.h"
#include "ui/ColourPicker.h"
#include "ui/MixerComponent.h"
#include "ui/UiTrace.h"
#include "ui/TextCache.h"

#include "core/FlEffects.h"
#include "core/ProjectContents.h"
#include "core/FruityVocoder.h"
#include "core/LiveInput.h"
#include "core/HardwareOutput.h"
#include "core/MixerOps.h"
#include "core/Templates.h"
#include "core/RackOps.h"
#include "ui/AutomationUi.h"
#include "ui/BrowserComponent.h"
#include "ui/ExportDialog.h"

#include <complex>

#include "core/Biquad.h"
#include "core/Effects.h"
#include "engine/Text.h"
#include "engine/ProjectFile.h"
#include <numeric>
#include "ui/FacetEqEditor.h"
#include "ui/ParametricEqEditor.h"
#include "ui/PluginEditors.h"
#include "ui/PluginWindow.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "core/LynxTune.h"
#include "core/VocalPitch.h"
#include "ui/Icons.h"
#include "ui/ValueControl.h"
#include "ui/Theme.h"
#include "ui/AudioEditor.h"
#include "ui/Dialogs.h"
#include "ui/QuickPick.h"

namespace lynx
{
namespace
{
// strip layout, top to bottom: header (arm, number, name), knobs (pan, stereo), mute/solo, meters +
// fader, the dB read-out, then the routing lane (send switch, send knob, wires)
constexpr int headerHeight = 36;
constexpr int knobsHeight = 32;
constexpr int buttonsHeight = 20;
constexpr int readoutHeight = 16;
constexpr int switchHeight = 16;
constexpr int sendKnobHeight = 26;
constexpr int wiresHeight = 14;
constexpr int laneHeight = switchHeight + sendKnobHeight + wiresHeight + 4;
constexpr int meterWidth = 6;
constexpr int masterScaleWidth = 17; // the dB numbers beside the master meter (round 162, issue #9)

// Fader 0..1 -> dB, with 0.8 = 0 dB (as in FL).
juce::String faderToDb (float volume)
{
    if (volume <= 0.0001f)
        return juce::String::fromUTF8 ("-\xe2\x88\x9e");

    const auto db = juce::Decibels::gainToDecibels (faderGain (volume));
    return (db > 0 ? "+" : "") + juce::String (db, 1);
}

// Meter level 0..1 on a dB scale (-60..+6).
float meterPosition (float gain)
{
    const auto db = juce::Decibels::gainToDecibels (gain, -60.0f);
    return juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
}

// Stereo separation: -1 = widest, 0 = normal, +1 = mono (MixerTrack::stereo).
ui::ValueSpec stereoSpec (const juce::String& name)
{
    ui::ValueSpec spec;
    spec.name = name;
    spec.minimum = -1.0;
    spec.maximum = 1.0;
    spec.defaultValue = 0.0;
    spec.toText = [] (double v) {
        const auto percent = juce::roundToInt (std::abs (v) * 100.0);
        if (percent == 0)
            return tr ("Normal width");
        if (v > 0.0 && percent >= 100)
            return tr ("Mono");
        return v < 0.0 ? tr ("{0}% wider", { juce::String (percent) }) : tr ("{0}% narrower", { juce::String (percent) });
    };
    spec.fromText = [] (const juce::String& t) -> std::optional<double> {
        const auto lower = t.toLowerCase().trim();
        if (lower.startsWith ("mono"))
            return 1.0;
        if (lower.startsWith ("n") && ! lower.containsAnyOf ("0123456789"))
            return 0.0;
        const auto digits = lower.initialSectionContainingOnly ("+-0123456789.,").replaceCharacter (',', '.');
        if (digits.isEmpty() || digits.containsOnly ("+-."))
            return std::nullopt;
        auto v = digits.getDoubleValue() / 100.0;
        if (lower.contains ("wid") || lower.contains ("larg")) // "50% wider" / "50% mais largo"
            v = -std::abs (v);
        return v;
    };
    return spec;
}

ui::ValueSpec sendSpec (const juce::String& name)
{
    // FL's send knob: the fader curve (0 dB at 80 %, +5.6 dB at the top); the value is the knob position
    auto spec = ui::faderSpec (name, unityFader);
    spec.pixelsForRange = 120.0;
    return spec;
}
} // namespace

MixerStrips::MixerStrips (AudioEngine& e)
    : engine (e)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    engine.addChangeListener (this);
    updateSize();
    startTimerHz (30);
}

MixerStrips::~MixerStrips()
{
    engine.removeChangeListener (this);
}

void MixerStrips::setVisibleHeight (int height)
{
    visibleHeight = juce::jmax (300, height);
    updateSize();
}

void MixerStrips::updateSize()
{
    const auto tracks = (int) engine.getSong().mixer.size();
    layoutStrips();
    setSize (contentWidth, visibleHeight);
    levels.resize ((std::size_t) juce::jmax (1, tracks) * 2);
}

void MixerStrips::layoutStrips()
{
    const auto& song = engine.getSong();
    const auto tracks = (int) song.mixer.size();
    stripXs.assign ((std::size_t) tracks + 1, 0);
    // "Show only used inserts" (round 163): unused inserts take no room, except those brought back with "+" (so a new
    // insert shows; selecting another strip doesn't move the strips under the mouse)
    hidden.assign ((std::size_t) tracks + 1, false);
    if (showUsedOnly)
        for (int t = 1; t < tracks; ++t)
            hidden[(std::size_t) t] = revealed.count (t) == 0 && ! song.insertInUse (t);
    dockGaps.clear();
    separatorLines.clear();

    int x = 4;
    bool any = false, afterMaster = false;
    for (auto dock : { MixerDock::left, MixerDock::middle, MixerDock::right })
    {
        const auto d = (int) dock;
        blockStart[d] = x;
        bool first = true;
        for (int t = 0; t < tracks; ++t)
        {
            const auto& track = song.mixer[(std::size_t) t];
            if (track.dock != dock)
                continue;
            if (hidden[(std::size_t) t])
            {
                stripXs[(std::size_t) t] = hiddenX;
                continue;
            }
            if (any && first && dock == MixerDock::right)
            {
                // the gap between two blocks belongs to the pinned block: the right block's is before its strips,
                // the left block's after them (below)
                dockGaps.push_back ({ x + dockGap / 2 - 2, dock });
                x += dockGap;
            }
            else if (! first && track.separator && t > 0)
            {
                separatorLines.push_back ({ x + separatorGap / 2 - 3, dock });
                x += separatorGap;
            }
            else if (afterMaster)
                x += 8;
            stripXs[(std::size_t) t] = x;
            x += stripWidthOf (t);
            any = true;
            first = false;
            afterMaster = t == 0;
        }
        if (dock == MixerDock::middle) // the "+" button ends the middle block
        {
            if (afterMaster)
                x += 8;
            stripXs[(std::size_t) tracks] = x;
            x += stripWidth;
            afterMaster = false;
            any = true;
        }
        blockEnd[d] = x;
        if (dock == MixerDock::left && ! first) // the left block carries its gap to the middle
        {
            dockGaps.push_back ({ x + dockGap / 2 - 2, dock });
            x += dockGap;
            blockEnd[d] = x;
            afterMaster = false;
        }
    }
    contentWidth = x + 8;
}

MixerDock MixerStrips::dockOf (int track) const noexcept
{
    const auto& mixer = engine.getSong().mixer;
    return track >= 0 && track < (int) mixer.size() ? mixer[(std::size_t) track].dock : MixerDock::middle;
}

int MixerStrips::pinOffset (MixerDock dock) const noexcept
{
    auto* parent = getParentComponent();
    if (parent == nullptr || dock == MixerDock::middle)
        return 0;
    const auto scroll = juce::jmax (0, -getX());
    const auto visible = parent->getWidth();
    if (dock == MixerDock::left)
        return blockEnd[0] > blockStart[0] ? juce::jmin (scroll, juce::jmax (0, contentWidth - blockEnd[0])) : 0;
    return blockEnd[2] > blockStart[2] ? juce::jmin (0, scroll + visible - contentWidth) : 0;
}

int MixerStrips::stripX (int track) const noexcept
{
    if (track >= 0 && track < (int) stripXs.size())
        return stripXs[(std::size_t) track] + pinOffset (dockOf (track));
    return track == 0 ? 4 : 4 + masterWidth + 8 + (track - 1) * stripWidth;
}

bool MixerStrips::isInterestedInDragSource (const SourceDetails& details)
{
    return BrowserComponent::isEffectDrag (details);
}

void MixerStrips::itemDropped (const SourceDetails& details)
{
    const auto t = trackAt (details.localPosition.x);
    if (t < 0 || t >= (int) engine.getSong().mixer.size() || onEffectDropped == nullptr)
        return;
    if (auto fx = BrowserComponent::draggedEffect (details))
        onEffectDropped (t, std::move (*fx));
}

int MixerStrips::trackAt (int x) const noexcept
{
    const auto tracks = (int) engine.getSong().mixer.size();

    // the pinned blocks lie over the scrolled middle: they are hit first
    for (auto dock : { MixerDock::left, MixerDock::right, MixerDock::middle })
        for (int t = 0; t <= tracks; ++t) // t == tracks = "+" button
            if (dockOf (t) == dock && x >= stripX (t) && x < stripX (t) + stripWidthOf (t))
                return t;

    return -1;
}

int MixerStrips::laneTop() const noexcept
{
    return getHeight() - laneHeight;
}

juce::Rectangle<int> MixerStrips::headerArea (int t) const
{
    return { stripX (t), 0, stripWidthOf (t) - 4, headerHeight };
}

juce::Rectangle<int> MixerStrips::armArea (int t) const
{
    return { stripX (t) + 4, 6, 13, 13 };
}

juce::Rectangle<int> MixerStrips::inputArea (int t) const
{
    // a thin level bar under the header (inserts with an input source)
    return { stripX (t) + 6, headerHeight - 4, stripWidthOf (t) - 16, 3 };
}

juce::Rectangle<int> MixerStrips::panArea (int t) const
{
    const auto w = stripWidthOf (t) - 4;
    return { stripX (t), headerHeight, w / 2, knobsHeight };
}

juce::Rectangle<int> MixerStrips::stereoArea (int t) const
{
    const auto w = stripWidthOf (t) - 4;
    return { stripX (t) + w / 2, headerHeight, w - w / 2, knobsHeight };
}

juce::Rectangle<int> MixerStrips::muteArea (int t) const
{
    return { stripX (t) + 4, headerHeight + knobsHeight, 24, buttonsHeight };
}

juce::Rectangle<int> MixerStrips::soloArea (int t) const
{
    return { stripX (t) + stripWidthOf (t) - 32, headerHeight + knobsHeight, 24, buttonsHeight };
}

juce::Rectangle<int> MixerStrips::faderArea (int t) const
{
    const auto top = headerHeight + knobsHeight + buttonsHeight + 8;
    return { stripX (t) + 5, top, stripWidthOf (t) - 14, juce::jmax (40, laneTop() - readoutHeight - 4 - top) };
}

juce::Rectangle<int> MixerStrips::meterArea (int t) const
{
    return faderArea (t).withWidth (meterWidth * 2 + 2);
}

juce::Rectangle<int> MixerStrips::faderRail (int t) const
{
    return faderArea (t).withTrimmedLeft (meterWidth * 2 + 6 + (t == 0 ? masterScaleWidth : 0));
}

juce::Rectangle<int> MixerStrips::routeArea (int t) const
{
    return { stripX (t) + 5, laneTop() + 2, stripWidthOf (t) - 14, switchHeight };
}

juce::Rectangle<int> MixerStrips::sendArea (int t) const
{
    return { stripX (t), laneTop() + 2 + switchHeight, stripWidthOf (t) - 4, sendKnobHeight };
}

MixerStrips::Part MixerStrips::partAt (int t, juce::Point<int> p) const
{
    const auto& song = engine.getSong();

    if (t == (int) song.mixer.size())
        return Part::add;

    if (t < 0 || t > (int) song.mixer.size())
        return Part::none;

    const auto sending = selected > 0 && t != selected && selected < (int) song.mixer.size();

    if (t > 0 && armArea (t).expanded (1).contains (p))
        return Part::arm;

    if (t > 0 && t < (int) engine.getSong().mixer.size() && engine.getSong().mixer[(std::size_t) t].inputChannel >= 0
        && inputArea (t).expanded (0, 2).contains (p))
        return Part::input;
    if (headerArea (t).contains (p))
        return Part::header;
    if (panArea (t).contains (p))
        return Part::pan;
    if (stereoArea (t).contains (p))
        return Part::stereo;
    if (muteArea (t).contains (p))
        return Part::mute;
    if (t > 0 && soloArea (t).contains (p))
        return Part::solo;
    if (sending && routeArea (t).contains (p) && (song.routes (selected, t) || song.canRoute (selected, t)))
        return Part::route;
    if (sending && sendArea (t).contains (p) && song.routes (selected, t))
        return Part::send;
    if (meterArea (t).contains (p))
        return Part::meter;
    if (faderRail (t).expanded (0, 6).contains (p))
        return Part::fader;

    return Part::body;
}

juce::Rectangle<int> MixerStrips::tipArea (Part part, int t) const
{
    switch (part)
    {
        case Part::pan: return panArea (t);
        case Part::stereo: return stereoArea (t);
        case Part::send: return sendArea (t);
        case Part::route: return routeArea (t);
        case Part::fader:
        {
            const auto rail = faderRail (t);
            const auto& mixer = engine.getSong().mixer;
            const auto v = t >= 0 && t < (int) mixer.size() ? mixer[(std::size_t) t].volume : 0.0f;
            const auto capY = rail.getBottom() - (int) (v * (float) rail.getHeight());
            return { rail.getX(), capY - 6, rail.getWidth(), 12 };
        }
        default: return {};
    }
}

void MixerStrips::showTip (Part part, int t, bool transient)
{
    if (t < 0 || t >= (int) engine.getSong().mixer.size())
        return;

    const auto spec = specFor (part, t);
    const auto text = spec.text (valueOf (part, t));
    ui::postHint (ui::valueHint (spec, valueOf (part, t), ui::valueGestures()));

    if (transient)
        ui::flashValueTip (*this, tipArea (part, t), text);
    else
        ui::showValueTip (*this, tipArea (part, t), text);
}

void MixerStrips::setHover (Part part, int t)
{
    if (part == hoverPart && t == hoverTrack)
        return;

    auto dirty = [this] (int track) {
        if (track >= 0)
            repaint (stripX (track) - 2, 0, stripWidthOf (track) + 4, getHeight());
    };

    dirty (hoverTrack);
    hoverPart = part;
    hoverTrack = t;
    dirty (hoverTrack);
}

void MixerStrips::mouseMove (const juce::MouseEvent& e)
{
    const auto t = trackAt (e.x);
    setHover (t >= 0 ? partAt (t, e.getPosition()) : Part::none, t);
}

void MixerStrips::mouseExit (const juce::MouseEvent&)
{
    setHover (Part::none, -1);
}

void MixerStrips::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("MixerStrips paint", true, g.getClipBounds());
    using namespace theme;
    const auto& song = engine.getSong();
    const auto tracks = (int) song.mixer.size();
    const auto clip = g.getClipBounds();
    g.fillAll (colour::bg1);

    // the routing lane runs under all strips
    g.setColour (colour::bg0.withAlpha (0.55f));
    g.fillRect (0, laneTop() + switchHeight + sendKnobHeight + 2, getWidth(), wiresHeight + 2);

    // FL's separators (a line on the left of the track) and the gaps between the dock blocks (round 146a), drawn
    // per block so the pinned left / right blocks cover the scrolled middle (round 149)
    auto paintDividers = [&] (MixerDock dock) {
        const auto shift = pinOffset (dock);
        g.setColour (colour::borderStrong);
        for (auto [lx, d] : separatorLines)
            if (d == dock)
                theme::fillRoundRect (g, { (float) (lx + shift), 8.0f, 2.0f, (float) laneTop() - 14.0f }, 1.0f, colour::borderStrong);
        g.setColour (colour::bg0);
        for (auto [gx, d] : dockGaps)
            if (d == dock)
                g.fillRect (gx + shift - 3, 0, 6, getHeight());
    };

    for (auto pass : { MixerDock::middle, MixerDock::left, MixerDock::right })
    {
    if (pass == MixerDock::middle)
    {
        // "+" button
        if (tracks < maxMixerTracks)
        {
            const auto add = juce::Rectangle<int> (stripX (tracks), 2, stripWidth - 4, 60).toFloat();
            const auto addHovered = hoverPart == Part::add;
            theme::fillRoundRect (g, add, radius::md, addHovered ? colour::bg4 : colour::bg2);
            g.setColour (addHovered ? colour::borderStrong : colour::border);
            g.drawRoundedRectangle (add.reduced (0.5f), radius::md, 1.0f);
            icons::draw (g, Icon::plus, add.withSizeKeepingCentre (22.0f, 22.0f), addHovered ? colour::textPrimary : colour::textSecondary);
        }
    }
    else if (const auto shift = pinOffset (pass); shift != 0) // a pinned block hides what scrolls under it
    {
        const auto d = (int) pass;
        g.setColour (colour::bg1);
        g.fillRect (blockStart[d] + shift, 0, blockEnd[d] - blockStart[d], laneTop());
        g.setColour (colour::bg0.withAlpha (0.55f));
        g.fillRect (blockStart[d] + shift, laneTop(), blockEnd[d] - blockStart[d], getHeight() - laneTop());
    }

    for (int t = 0; t < tracks; ++t)
    {
        if (song.mixer[(std::size_t) t].dock != pass || isHidden (t))
            continue;

        const auto x = stripX (t);
        const auto w = stripWidthOf (t);

        if (x > clip.getRight() || x + w < clip.getX())
            continue;

        const auto& track = song.mixer[(std::size_t) t];
        const auto strip = juce::Rectangle<int> (x, 2, w - 4, laneTop() - 2);
        const auto isSelected = t == selected;
        const auto inSelection = std::binary_search (selection.begin(), selection.end(), t);
        const auto stripHovered = hoverTrack == t;
        const auto hovered = [this, t] (Part p) { return hoverTrack == t && hoverPart == p; };
        const auto dragging = [this, t] (Part p) { return dragTrack == t && dragPart == p; };

        // the strip is tinted with the insert's colour (FL tints whole strips, so groups read at a glance);
        // kept dark so meters, knobs and text keep their contrast
        auto plate = isSelected || inSelection ? colour::bg4 : (t == 0 ? colour::bg3 : stripHovered ? colour::bg3 : colour::bg2);
        if (track.colour != 0)
            plate = plate.interpolatedWith (tame (juce::Colour (track.colour)), isSelected ? 0.26f : 0.2f);
        theme::fillRoundRect (g, strip.toFloat(), radius::md, plate);

        if (track.colour != 0) // thin colour band at the top
        {
            theme::fillRoundRect (g, strip.toFloat().withHeight (3.0f).reduced (6.0f, 0.0f), 1.5f, tame (juce::Colour (track.colour)));
        }

        if (inSelection && ! isSelected)
        {
            g.setColour (colour::accent.withAlpha (0.3f));
            g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), radius::md, 1.0f);
        }

        if (isSelected)
        {
            g.setColour (colour::accent.withAlpha (0.55f));
            g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), radius::md, 1.0f);
            theme::fillRoundRect (g, strip.toFloat().withHeight (3.0f).reduced (6.0f, 0.0f), 1.5f, colour::accent);
        }

        if (headerDragged && headerDragTrack == t)
        {
            g.setColour (colour::accent);
            g.drawRoundedRectangle (strip.toFloat().reduced (0.5f), radius::md, 1.5f);
        }

        // record arm: the input recording goes through this insert
        if (t > 0)
        {
            const auto armed = engine.isInsertArmed (t);
            const auto r = armArea (t).toFloat().reduced (2.0f);
            g.setColour (armed ? colour::record : hovered (Part::arm) ? colour::bg5 : colour::bg0);
            g.fillEllipse (r);
            g.setColour (armed ? colour::record.brighter (0.3f) : hovered (Part::arm) ? colour::textSecondary : colour::borderStrong);
            g.drawEllipse (r, 1.0f);
            if (track.recordSafe) // record safe: a slash through the arm button
            {
                g.setColour (colour::textDisabled);
                g.drawLine (r.getX() + 1.0f, r.getBottom() - 1.0f, r.getRight() - 1.0f, r.getY() + 1.0f, 1.2f);
            }

            // the input level (inserts with an audio input source)
            if (track.inputChannel >= 0 && (std::size_t) t < inputLevels.size())
            {
                const auto bar = inputArea (t).toFloat();
                theme::fillRoundRect (g, bar, 1.0f, colour::bg0);
                const auto level = inputLevels[(std::size_t) t];
                const auto fill = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (level, -60.0f) + 60.0f) / 60.0f);
                theme::fillRoundRect (g, bar.withWidth (bar.getWidth() * fill), 1.0f, level >= 0.99f ? colour::record : colour::meterLow);
            }
        }

        // header: number and effect count on one line, the name (once) below
        g.setColour (isSelected ? colour::accent : colour::textSecondary);
        g.setFont (font (type::caption, isSelected));
        textcache::drawText (g, t == 0 ? juce::String ("M") : juce::String (t), x, 6, w - 4, 13, juce::Justification::centred);

        if (! track.effects.empty())
        {
            const auto on = mixerops::anyEffectEnabled (song, t);
            g.setColour (on ? colour::info : colour::textDisabled);
            g.setFont (font (9.0f, true));
            textcache::drawText (g, "FX" + juce::String ((int) track.effects.size()), x + w - 30, 6, 23, 13, juce::Justification::centredRight);
        }

        g.setColour (track.muted ? colour::textDisabled : (hovered (Part::header) ? colour::textPrimary : colour::textPrimary.withAlpha (0.9f)));
        g.setFont (font (type::small, isSelected));
        textcache::drawFittedText (g, juce::String::fromUTF8 (track.name.c_str()), x + 3, 20, w - 10, 14, juce::Justification::centred, 1, 0.7f);

        // compensated latency (PDC) on the Master
        if (t == 0 && engine.getLatencySamples() > 0)
        {
            const auto ms = 1000.0 * engine.getLatencySamples() / juce::jmax (1.0, engine.getTransport().sampleRate.load());
            g.setColour (colour::textSecondary);
            g.setFont (font (10.0f));
            textcache::drawFittedText (g, "PDC " + juce::String (ms, 1) + " ms", soloArea (t).withX (x + 30).withWidth (w - 36),
                              juce::Justification::centredLeft, 1, 0.8f); // the Master has no solo: use its spot
        }

        // pan and stereo separation
        drawKnob (g, panArea (t).toFloat().withSizeKeepingCentre (26.0f, 26.0f), (track.pan + 1.0f) * 0.5f, true,
                  dragging (Part::pan) ? colour::accent : colour::info, hovered (Part::pan) || dragging (Part::pan));
        drawKnob (g, stereoArea (t).toFloat().withSizeKeepingCentre (22.0f, 22.0f), (track.stereo + 1.0f) * 0.5f, true,
                  dragging (Part::stereo) ? colour::accent : colour::knobArc, hovered (Part::stereo) || dragging (Part::stereo));

        // mute and solo
        {
            const auto m = muteArea (t).toFloat().withSizeKeepingCentre (12.0f, 12.0f);
            g.setColour (muteLed (track.muted));
            g.fillEllipse (m);
            if (! track.muted || hovered (Part::mute))
            {
                g.setColour (hovered (Part::mute) ? colour::textSecondary : colour::ledOn.withAlpha (0.25f));
                g.drawEllipse (m.expanded (2.0f), 1.0f);
            }

            if (t > 0)
            {
                const auto s = soloArea (t).toFloat().reduced (2.0f);
                theme::fillRoundRect (g, s, radius::sm, track.solo ? colour::accent : hovered (Part::solo) ? colour::bg5 : colour::bg0);
                g.setColour (track.solo ? colour::onAccent : hovered (Part::solo) ? colour::textPrimary : colour::textSecondary);
                g.setFont (font (type::caption, true));
                textcache::drawText (g, "S", s.toNearestInt(), juce::Justification::centred);
            }
        }

        // meters + fader
        {
            const auto area = faderArea (t);
            const auto meters = meterArea (t);
            const auto fader = faderRail (t);

            for (int ch = 0; ch < 2; ++ch)
            {
                const auto bar = juce::Rectangle<int> (meters.getX() + ch * (meterWidth + 2), meters.getY(), meterWidth, meters.getHeight());
                theme::fillRoundRect (g, bar.toFloat(), 1.5f, colour::bg0);

                const auto index = (std::size_t) t * 2 + (std::size_t) ch;
                const auto meter = index < levels.size() ? levels[index] : meters::PeakHold {};
                const auto h = (int) (meterPosition (meter.level) * (float) bar.getHeight());
                const auto lit = bar.withTop (bar.getBottom() - h);
                juce::ColourGradient gradient (colour::meterClip, (float) bar.getX(), (float) bar.getY(), colour::meterLow, (float) bar.getX(),
                                               (float) bar.getBottom(), false);
                gradient.addColour (1.0 - meterPosition (juce::Decibels::decibelsToGain (0.0f)), colour::meterClip); // above 0 dB: clip zone
                gradient.addColour (1.0 - meterPosition (juce::Decibels::decibelsToGain (-6.0f)), colour::meterMid);
                gradient.addColour (1.0 - meterPosition (juce::Decibels::decibelsToGain (-18.0f)), colour::meterLow);
                g.setGradientFill (gradient);
                g.fillRect (lit);

                if (meter.peak > 0.001f) // peak hold line
                {
                    const auto peakY = bar.getBottom() - (int) (meterPosition (meter.peak) * (float) bar.getHeight());
                    g.setColour (meter.peak >= 1.0f ? colour::meterClip : meter.peak >= 0.5f ? colour::meterMid : colour::textPrimary.withAlpha (0.85f));
                    g.fillRect (bar.getX(), juce::jmax (bar.getY(), peakY - 1), bar.getWidth(), 2);
                }

                if (meter.clipped) // clip latch: stays until the meter is clicked
                {
                    g.setColour (colour::meterClip);
                    g.fillRect (bar.withHeight (4));
                }
            }

            if (hovered (Part::meter))
            {
                g.setColour (colour::textSecondary.withAlpha (0.5f));
                g.drawRect (meters.expanded (1), 1);
            }

            // the master's dB scale (FL prints it beside the master meter): as many values as fit without overlapping
            if (t == 0)
            {
                const auto scaleFont = font (9.0f);
                const auto textHeight = 10.0f;
                const auto left = (float) meters.getRight() + 1.0f;
                g.setFont (scaleFont);

                for (auto db : lynx::meters::scaleMarks ((float) meters.getHeight(), textHeight + 1.0f))
                {
                    const auto y = (float) meters.getBottom() - lynx::meters::scalePosition ((float) db) * (float) meters.getHeight();
                    g.setColour (db == 0 ? colour::textPrimary : colour::textSecondary);
                    g.fillRect (juce::Rectangle<float> (left, y - 0.5f, 2.0f, 1.0f));
                    const auto box = juce::Rectangle<float> (left + 2.0f, y - textHeight * 0.5f, (float) masterScaleWidth - 2.0f, textHeight)
                                         .withY (juce::jlimit ((float) meters.getY(), (float) meters.getBottom() - textHeight, y - textHeight * 0.5f));
                    textcache::drawText (g, (db > 0 ? "+" : "") + juce::String (db), box, juce::Justification::centredRight, false);
                }
            }

            // fader track and 0 dB mark
            const auto railX = (float) fader.getCentreX();
            const auto capY = (float) fader.getBottom() - track.volume * (float) fader.getHeight();
            theme::fillRoundRect (g, { railX - 2.0f, (float) fader.getY(), 4.0f, (float) fader.getHeight() }, 2.0f, colour::bg0);
            theme::fillRoundRect (g, { railX - 2.0f, capY, 4.0f, juce::jmax (0.0f, (float) fader.getBottom() - capY) }, 2.0f, colour::bg6.brighter (0.15f).withAlpha (track.muted ? 0.4f : 1.0f));
            const auto unityY = (float) fader.getBottom() - unityFader * (float) fader.getHeight();
            g.setColour (colour::textDisabled);
            g.drawHorizontalLine ((int) unityY, (float) fader.getX(), railX - 5.0f);
            g.drawHorizontalLine ((int) unityY, railX + 5.0f, (float) fader.getRight());

            const auto capActive = hovered (Part::fader) || dragging (Part::fader);
            const auto capWidth = juce::jmin ((float) fader.getWidth() + 2.0f, 26.0f);
            const auto cap = juce::Rectangle<float> (capWidth, 12.0f).withCentre ({ railX, capY });
            theme::fillRoundRect (g, cap.translated (0.0f, 1.5f), radius::sm, juce::Colours::black.withAlpha (0.35f));
            // same cap as the LookAndFeel's linear faders
            theme::fillRoundRect (g, cap, radius::sm, capActive ? juce::Colour (0xffffffff) : juce::Colour (0xffd4d7dd));
            g.setColour ((dragging (Part::fader) ? colour::accent : colour::bg0).withAlpha (0.8f));
            g.fillRect (cap.withSizeKeepingCentre (capWidth - 8.0f, 1.5f));

            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            textcache::drawText (g, faderToDb (track.volume) + " dB", x, area.getBottom() + 3, w - 4, readoutHeight - 2, juce::Justification::centred);

            // a hardware output target (REA round 156, optional): a small badge at the fader's top right
            if (mixeroutput::isCustom (engine, t))
            {
                const auto text = mixeroutput::label (engine, t, true);
                const auto badgeLeft = (float) meterArea (t).getRight() + 3.0f + (t == 0 ? (float) masterScaleWidth : 0.0f);
                const auto badge = juce::Rectangle<float> (badgeLeft, (float) area.getY(), (float) (x + w - 6) - badgeLeft, 12.0f);
                theme::fillRoundRect (g, badge, 3.0f, colour::bg0.withAlpha (0.85f));
                g.setColour (text.endsWith ("?") ? colour::textDisabled : colour::info);
                g.setFont (font (9.0f, true));
                textcache::drawFittedText (g, text, badge.toNearestInt(), juce::Justification::centred, 1, 0.7f);
            }
        }

        // routing lane: send switch from the selected insert to this one, and its level knob
        if (selected > 0 && t != selected && selected < tracks)
        {
            const auto r = routeArea (t).toFloat();
            const auto on = song.routes (selected, t);
            const auto possible = on || song.canRoute (selected, t);
            const auto feedsSelected = t > 0 && song.routes (t, selected);

            if (possible)
            {
                theme::fillRoundRect (g, r, radius::sm, on ? colour::accent : hovered (Part::route) ? colour::bg5 : colour::bg0);
                if (hovered (Part::route))
                {
                    g.setColour (on ? colour::accentHover : colour::textSecondary);
                    g.drawRoundedRectangle (r.reduced (0.5f), radius::sm, 1.0f);
                }
                g.setColour (on ? colour::onAccent : colour::textSecondary);
                g.setFont (font (10.0f, on));
                textcache::drawText (g, juce::String::fromUTF8 ("\xe2\x86\x92 ") + (t == 0 ? juce::String ("M") : juce::String (t)), r.toNearestInt(),
                            juce::Justification::centred);

                // the route's level knob (FL's route knob), on the routes to inserts and to Master
                if (on)
                    drawKnob (g, sendArea (t).toFloat().withSizeKeepingCentre (22.0f, 22.0f), faderPosition (song.routeLevel (selected, t)), false,
                              dragging (Part::send) ? colour::accent : colour::accentHover, hovered (Part::send) || dragging (Part::send));
            }
            else if (feedsSelected) // this insert sends into the selected one
            {
                theme::fillRoundRect (g, r, radius::sm, colour::info.withAlpha (0.18f));
                g.setColour (colour::info);
                g.setFont (font (10.0f, true));
                textcache::drawText (g, juce::String::fromUTF8 ("\xe2\x86\x92 ") + juce::String (selected), r.toNearestInt(), juce::Justification::centred);
            }
        }
    }
    paintDividers (pass);
    }

    paintRouting (g);
}

// Wires in the lane under the strips: from the selected insert to where it sends (accent), and from the
// inserts that feed it (blue). The Master output is drawn dimmer.
void MixerStrips::paintRouting (juce::Graphics& g)
{
    using namespace theme;
    const auto& song = engine.getSong();
    const auto tracks = (int) song.mixer.size();

    if (selected <= 0 || selected >= tracks)
        return;

    const auto top = (float) (laneTop() + switchHeight + sendKnobHeight + 3);
    const auto bottom = top + (float) wiresHeight - 2.0f;
    const auto centre = [this] (int t) { return (float) stripX (t) + (float) (stripWidthOf (t) - 4) * 0.5f; };

    auto wire = [&] (int from, int to, juce::Colour c, float thickness) {
        const auto x0 = centre (from), x1 = centre (to);
        juce::Path p;
        p.startNewSubPath (x0, top);
        p.cubicTo (x0, bottom + 2.0f, x1, bottom + 2.0f, x1, top);
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillEllipse (x1 - 2.5f, top - 2.5f, 5.0f, 5.0f);
    };

    const auto& from = song.mixer[(std::size_t) selected];

    for (int t = 1; t < tracks; ++t)
        if (t != selected && song.routes (t, selected))
            wire (t, selected, colour::info.withAlpha (0.85f), 1.5f);

    if (from.toMaster)
        wire (selected, 0, colour::accent.withAlpha (0.4f), 1.0f);

    for (const auto& send : from.sends)
        if (send.target > 0 && send.target < tracks)
            wire (selected, send.target, colour::accent.withAlpha (0.5f + 0.5f * juce::jlimit (0.0f, 1.0f, send.level)), 1.8f);

    g.setColour (colour::accent);
    g.fillEllipse (centre (selected) - 3.0f, top - 3.0f, 6.0f, 6.0f);
}

void MixerStrips::mouseDown (const juce::MouseEvent& e)
{
    const auto t = trackAt (e.x);

    if (t < 0)
        return;

    const auto part = partAt (t, e.getPosition());
    headerDragTrack = -1;
    headerDragged = false;

    if (part == Part::add)
    {
        if (showUsedOnly) // the first unused insert (hidden) comes back before a new one is added
            if (const auto free = engine.getSong().nextFreeInsert(); free > 0 && isHidden (free))
            {
                revealed.insert (free);
                updateSize();
                selectTrack (free);
                return;
            }
        const auto added = (int) engine.getSong().mixer.size();
        engine.editSong ([] (Song& s) {
            if (s.mixer.empty())
                s.mixer = Song::defaultMixer (0);

            if ((int) s.mixer.size() >= maxMixerTracks)
                return;
            auto& track = s.mixer.emplace_back();
            track.name = "Insert " + std::to_string (s.mixer.size() - 1);
        });
        if (added < (int) engine.getSong().mixer.size())
        {
            revealed.insert (added); // shown even while unused
            updateSize();
            selectTrack (added);
        }
        return;
    }

    if (part == Part::arm)
    {
        if (e.mods.isPopupMenu())
            mixerinput::showMenu (engine, t, this);
        else
            mixerinput::toggleArm (engine, t, this);
        repaint();
        return;
    }

    if (part == Part::input)
    {
        mixerinput::showMenu (engine, t, this);
        return;
    }

    if (part == Part::meter && ! e.mods.isPopupMenu()) // clears the clip latch and the peak hold
    {
        for (int ch = 0; ch < 2; ++ch)
            if (const auto i = (std::size_t) t * 2 + (std::size_t) ch; i < levels.size())
                levels[i].resetClip();
        repaint (meterArea (t).expanded (2));
        return;
    }

    if (part == Part::route && e.mods.isPopupMenu())
    {
        showSendMenu (t);
        return;
    }

    if (part == Part::send)
    {
        const auto from = selected;
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            const auto spec = specFor (Part::send, t);
            menu.addSectionHeader (spec.name + ": " + spec.text (valueOf (Part::send, t)));
            ui::addValueItems (menu, *this, nullptr, spec, valueOf (Part::send, t), [this, t] (double v) { setValue (Part::send, t, v); });
            menu.addSeparator();
            menu.addItem (t == 0 ? tr ("Remove the route to Master") : tr ("Remove this send"),
                          [this, from, t] { engine.editSong ([from, t] (Song& s) { s.setRoute (from, t, false); }); });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
            return;
        }

        if (ui::isResetClick (e))
        {
            setValue (Part::send, t, unityFader); // 0 dB
            showTip (Part::send, t, true);
            return;
        }

        dragPart = Part::send;
        dragTrack = t;
        drag.begin (e, valueOf (Part::send, t));
        showTip (Part::send, t, false);
        return;
    }

    if (part == Part::mute && (e.mods.isPopupMenu() || e.mods.isCtrlDown())) // solo on the same LED
    {
        soloExclusive (t);
        return;
    }

    if (part == Part::route)
    {
        // the selection is the send source: don't change it
        const auto from = selected;
        const auto on = engine.getSong().routes (from, t);
        engine.editSong ([from, t, on] (Song& s) {
            s.setRoute (from, t, ! on);
            if (! on)
                s.setRouteLevel (from, t, 1.0f); // a new route starts at 0 dB
        });

        if (! on) // just enabled (at 0 dB): dragging adjusts the level, like the route knob
        {
            dragPart = Part::send;
            dragTrack = t;
            drag.begin (e, unityFader);
        }
        return;
    }

    if ((part == Part::body || part == Part::header) && ! e.mods.isPopupMenu() && (e.mods.isCtrlDown() || e.mods.isShiftDown()) && t > 0)
    {
        auto s = selection;
        if (e.mods.isShiftDown())
        {
            s.clear();
            for (int i = juce::jmin (selected, t); i <= juce::jmax (selected, t); ++i)
                s.push_back (i);
        }
        else if (auto it = std::find (s.begin(), s.end(), t); it != s.end() && s.size() > 1)
            s.erase (it);
        else
            s.push_back (t);

        setSelection (s, std::find (s.begin(), s.end(), t) != s.end() ? t : s.front());
        return;
    }

    // a plain click on a track outside the selection selects only it; inside, the group stays
    if (! std::binary_search (selection.begin(), selection.end(), t))
        setSelection ({ t }, t);
    else if (selected != t)
        setSelection (selection, t);

    if (part == Part::pan || part == Part::fader || part == Part::stereo)
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, part == Part::stereo ? nullptr : &engine, specFor (part, t), valueOf (part, t),
                               [this, part, t] (double v) { setValue (part, t, v); });
            return;
        }

        if (ui::isResetClick (e))
        {
            setValue (part, t, specFor (part, t).defaultValue);
            showTip (part, t, true);
            return;
        }
    }

    if (e.mods.isPopupMenu())
    {
        showMenu (t);
        return;
    }

    const auto& track = engine.getSong().mixer[(std::size_t) t];

    switch (part)
    {
        case Part::mute:
            engine.editSong ([t] (Song& s) { s.mixer[(std::size_t) t].muted = ! s.mixer[(std::size_t) t].muted; });
            return;

        case Part::solo:
            engine.editSong ([t] (Song& s) { s.mixer[(std::size_t) t].solo = ! s.mixer[(std::size_t) t].solo; });
            return;

        case Part::header:
            if (t > 0)
                headerDragTrack = t; // dragging sideways reorders
            return;

        case Part::stereo:
            dragPart = part;
            dragTrack = t;
            dragStartValue = track.stereo;
            drag.begin (e, dragStartValue);
            groupStart.clear();
            showTip (part, t, false);
            return;

        case Part::pan:
        case Part::fader:
            dragPart = part;
            dragTrack = t;
            dragStartValue = part == Part::pan ? track.pan : track.volume;

            if (part == Part::fader && ! e.mods.isCtrlDown()) // clicking the track jumps the fader there
            {
                const auto area = faderRail (t);
                const auto capY = area.getBottom() - track.volume * (float) area.getHeight();

                if (std::abs ((float) e.y - capY) > 9.0f)
                {
                    dragStartValue = juce::jlimit (0.0f, 1.0f, (float) (area.getBottom() - e.y) / (float) area.getHeight());
                    engine.editSong ([t, v = dragStartValue] (Song& s) { s.mixer[(std::size_t) t].volume = v; });
                }
            }

            drag.begin (e, dragStartValue);
            groupStart.clear();
            groupGrab = dragStartValue;
            if (selection.size() > 1) // the selected strips follow, keeping their offsets
                for (auto s : selection)
                    if (s != t && s < (int) engine.getSong().mixer.size())
                        groupStart.push_back ({ s, part == Part::pan ? engine.getSong().mixer[(std::size_t) s].pan
                                                                      : engine.getSong().mixer[(std::size_t) s].volume });
            showTip (part, t, false);
            return;

        case Part::arm:
        case Part::route:
        case Part::send:
        case Part::meter:
        case Part::body:
        case Part::none:
        case Part::add:
            return;
    }
}

void MixerStrips::mouseDrag (const juce::MouseEvent& e)
{
    if (headerDragTrack > 0)
    {
        if (! headerDragged && std::abs (e.getDistanceFromDragStartX()) < 6)
            return;

        headerDragged = true;
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        const auto count = (int) engine.getSong().mixer.size();
        const auto target = trackAt (e.x);
        if (target < 1 || target >= count)
            return;

        for (int guard = 0; guard < count && selected != target; ++guard)
            if (! moveSelected (target < selected ? -1 : 1))
                break;

        headerDragTrack = selected;
        repaint();
        return;
    }

    if (dragPart == Part::none)
        return;

    const auto t = dragTrack;

    setValue (dragPart, t, drag.update (e, specFor (dragPart, t)));
    showTip (dragPart, t, false);
}

ui::ValueSpec MixerStrips::specFor (Part part, int t) const
{
    const auto& song = engine.getSong();
    const auto nameOf = [&song] (int track) {
        return track >= 0 && track < (int) song.mixer.size() ? juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str()) : juce::String();
    };
    const auto name = nameOf (t);

    if (part == Part::pan)
    {
        auto spec = ui::panSpec (name + " - " + tr ("Pan"));
        spec.pixelsForRange = 150.0;
        spec.target = AutomationTarget { AutomationTargetKind::mixerPan, t };
        return spec;
    }

    if (part == Part::stereo)
        return stereoSpec (name + " - " + tr ("Stereo separation"));

    if (part == Part::send)
        return sendSpec (nameOf (selected) + juce::String::fromUTF8 (" \xe2\x86\x92 ") + name + " - " + (t == 0 ? tr ("Level to Master") : tr ("Send level")));

    auto spec = ui::faderSpec (name + " - " + tr ("Volume"), unityFader);
    spec.pixelsForRange = juce::jmax (60, faderRail (juce::jmax (0, t)).getHeight());
    spec.target = AutomationTarget { AutomationTargetKind::mixerVolume, t };
    return spec;
}

double MixerStrips::valueOf (Part part, int t) const
{
    const auto& song = engine.getSong();
    const auto& track = song.mixer[(std::size_t) t];

    if (part == Part::send)
    {
        return faderPosition (song.routeLevel (selected, t));
    }

    return part == Part::pan ? track.pan : part == Part::stereo ? track.stereo : track.volume;
}

void MixerStrips::setValue (Part part, int t, double value)
{
    if (t < 0 || t >= (int) engine.getSong().mixer.size())
        return;

    if (part == Part::send)
    {
        const auto from = selected;
        engine.editSong ([from, t, v = faderGain ((float) value)] (Song& s) { s.setRouteLevel (from, t, v); });
        return;
    }

    if (part == Part::stereo)
    {
        engine.editSong ([t, v = (float) value] (Song& s) { s.mixer[(std::size_t) t].stereo = v; });
        return;
    }

    // during a drag of a selected strip, the other selected strips move by the same amount
    const auto delta = (float) value - groupGrab;
    const auto group = dragTrack == t && dragPart == part ? groupStart : std::vector<std::pair<int, float>> {};

    if (part == Part::pan)
        engine.editSong ([t, v = (float) value, group, delta] (Song& s) {
            s.mixer[(std::size_t) t].pan = v;
            for (auto [other, start] : group)
                if (other < (int) s.mixer.size())
                    s.mixer[(std::size_t) other].pan = juce::jlimit (-1.0f, 1.0f, start + delta);
        });
    else if (part == Part::fader)
        engine.editSong ([t, v = (float) value, group, delta] (Song& s) {
            s.mixer[(std::size_t) t].volume = v;
            for (auto [other, start] : group)
                if (other < (int) s.mixer.size())
                    s.mixer[(std::size_t) other].volume = juce::jlimit (0.0f, 1.0f, start + delta);
        });
}

void MixerStrips::setSelection (std::vector<int> tracks, int primary)
{
    const auto count = (int) engine.getSong().mixer.size();
    tracks.erase (std::remove_if (tracks.begin(), tracks.end(), [count] (int t) { return t < 0 || t >= count; }), tracks.end());
    primary = juce::jlimit (0, juce::jmax (0, count - 1), primary);
    tracks.push_back (primary);
    std::sort (tracks.begin(), tracks.end());
    tracks.erase (std::unique (tracks.begin(), tracks.end()), tracks.end());
    selection = tracks;
    const auto changed = primary != selected;
    selected = primary;
    repaint();

    if (changed && onSelectionChanged)
        onSelectionChanged (selected);
}

bool MixerStrips::moveSelected (int delta)
{
    auto moved = selection;
    bool ok = false;
    engine.editSong ([&] (Song& s) { ok = mixerops::moveTracks (s, moved, delta); });

    if (ok)
        setSelection (moved, juce::jlimit (1, (int) engine.getSong().mixer.size() - 1, selected + (delta < 0 ? -1 : 1)));

    return ok;
}

void MixerStrips::linkSelectedChannels (bool startingHere)
{
    const auto channels = engine.getSelectedChannels();
    engine.editSong ([channels, t = selected, startingHere] (Song& s) { mixerops::linkChannels (s, channels, t, startingHere); });
}

void MixerStrips::selectLinkedChannels()
{
    const auto channels = mixerops::channelsOn (engine.getSong(), selected);

    if (! channels.empty())
        engine.setChannelSelection (channels, channels.front());
}

void MixerStrips::soloExclusive (int t)
{
    if (t <= 0)
        return;

    // solo only this track; again on the only soloed track clears the solo
    engine.editSong ([t] (Song& s) {
        const auto alone = s.mixer[(std::size_t) t].solo
                           && std::none_of (s.mixer.begin() + 1, s.mixer.end(), [&] (const MixerTrack& m) { return &m != &s.mixer[(std::size_t) t] && m.solo; });
        for (std::size_t i = 1; i < s.mixer.size(); ++i)
            s.mixer[i].solo = ! alone && (int) i == t;
    });
}

void MixerStrips::showSendMenu (int t)
{
    const auto& song = engine.getSong();
    const auto from = selected;

    if (from <= 0 || from == t)
        return;

    const auto fromName = juce::String::fromUTF8 (song.mixer[(std::size_t) from].name.c_str());
    juce::PopupMenu menu;
    menu.addSectionHeader (fromName + juce::String::fromUTF8 (" \xe2\x86\x92 ") + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()));
    const auto on = song.routes (from, t);
    menu.addItem (on ? tr ("Remove this send") : tr ("Send here"), on || song.canRoute (from, t), false, [this, from, t, on] {
        engine.editSong ([from, t, on] (Song& s) { s.setRoute (from, t, ! on); });
    });
    menu.addItem (tr ("Send only here"), on || song.canRoute (from, t), false, [this, from, t] {
        engine.editSong ([from, t] (Song& s) { mixerops::routeTo (s, { from }, t, true); });
    });
    menu.addItem (tr ("Sidechain only (no audio): the compressors here listen to it"), t > 0 && (on || song.canRoute (from, t)), false, [this, from, t] {
        int linked = 0;
        engine.editSong ([from, t, &linked] (Song& s) { linked = mixerops::sidechainTo (s, from, t); });
        if (linked == 0)
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Sidechain"),
                                                    tr ("The link was made; add a compressor to this insert to use it."));
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void MixerStrips::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto t = trackAt (e.x);

    if (t > 0 && t < (int) engine.getSong().mixer.size() && e.mods.isShiftDown())
        if (const auto part = partAt (t, e.getPosition()); part == Part::body || part == Part::header)
        {
            if (! std::binary_search (selection.begin(), selection.end(), t))
                setSelection ({ t }, t);
            moveSelected ((wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) > 0.0f ? -1 : 1);
            return;
        }

    if (t >= 0 && t < (int) engine.getSong().mixer.size())
        if (const auto part = partAt (t, e.getPosition()); part == Part::pan || part == Part::fader || part == Part::stereo || part == Part::send)
        {
            if (const auto v = ui::wheelValue (valueOf (part, t), e, wheel, specFor (part, t)))
            {
                setValue (part, t, *v);
                showTip (part, t, true);
            }
            return;
        }

    juce::Component::mouseWheelMove (e, wheel);
}

ui::Hint MixerStrips::hintAt (juce::Point<int> p)
{
    const auto active = dragPart == Part::pan || dragPart == Part::fader || dragPart == Part::stereo || dragPart == Part::send;
    const auto t = active ? dragTrack : trackAt (p.x);
    const auto& song = engine.getSong();
    ui::Hint h;

    if (t < 0 || t >= (int) song.mixer.size())
    {
        if (t == (int) song.mixer.size())
            h.text = tr ("Add a mixer insert");
        return h;
    }

    const auto part = active ? dragPart : partAt (t, p);
    const auto name = juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str());
    const auto gesture = ui::valueGestures();

    // the channels that play into this insert (FL shows them by the insert; LYNX in the hint)
    auto feeds = [&song, t, tracks = (int) song.mixer.size()] {
        juce::StringArray names;
        for (const auto& c : song.channels)
            if (juce::jlimit (0, tracks - 1, c.mixerTrack) == t)
                names.add (juce::String::fromUTF8 (c.name.c_str()));
        if (names.isEmpty())
            return juce::String();
        const auto shown = names.size() > 6 ? names[0] + ", " + names[1] + ", " + names[2] + " +" + juce::String (names.size() - 3)
                                            : names.joinIntoString (", ");
        return "   " + tr ("Channels: {0}", { shown });
    };

    switch (part)
    {
        case Part::pan:
        case Part::fader:
        case Part::stereo:
        case Part::send:
            return ui::valueHint (specFor (part, t), valueOf (part, t), gesture);
        case Part::mute: h.text = name + " - " + tr ("Click: mute.  Right-click or Ctrl+click: solo"); break;
        case Part::arm: h.text = name + " - " + tr ("Record arm: Rec + Play records this insert's audio input into a Playlist clip  [RMB] input and monitoring"); h.hasMenu = true; break;
        case Part::input: h.text = name + " - " + tr ("Input level of this insert's audio input  [LMB] choose the input"); h.hasMenu = true; break;
        case Part::solo: h.text = name + " - " + tr ("Solo"); break;
        case Part::meter: h.text = name + " - " + tr ("Peak meter (left/right) with peak hold  [LMB] clears the clip light"); break;
        case Part::route: h.text = name + " - " + tr ("Send from the selected insert to this one (drag after enabling: level; right-click: sidechain, only here)"); h.hasMenu = true; break;
        case Part::add: h.text = tr ("Add a mixer insert"); break;
        case Part::header:
            h.text = name + " - " + tr ("[LMB] selects  [Ctrl]+[LMB] adds  [Shift]+[LMB] range  [LMB] drag sideways reorders  [RMB] insert menu") + feeds();
            h.hasMenu = true;
            break;
        case Part::body:
        case Part::none:
            h.text = name + " - " + tr ("Click selects the insert (Ctrl: add, Shift: range), right-click opens its menu; Alt+Left/Right moves the selection") + feeds();
            h.hasMenu = true;
            break;
    }

    return h;
}

void MixerStrips::mouseUp (const juce::MouseEvent&)
{
    if (headerDragged)
        setMouseCursor (juce::MouseCursor::NormalCursor);

    if (dragPart != Part::none || headerDragged)
        repaint();

    headerDragTrack = -1;
    headerDragged = false;
    dragPart = Part::none;
    dragTrack = -1;
    drag.end();
    ui::hideValueTip();
    ui::clearPostedHint();
}

void MixerStrips::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto t = trackAt (e.x);

    if (t < 0 || t >= (int) engine.getSong().mixer.size())
        return;

    const auto part = partAt (t, e.getPosition());

    if (part == Part::fader || part == Part::pan || part == Part::stereo || part == Part::send)
    {
        setValue (part, t, specFor (part, t).defaultValue);
        showTip (part, t, true);
    }
}

void MixerStrips::selectTrack (int track)
{
    selected = juce::jlimit (0, juce::jmax (0, (int) engine.getSong().mixer.size() - 1), track);
    selection = { selected };
    repaint();

    if (onSelectionChanged)
        onSelectionChanged (selected);
}

namespace
{
// The 3 knobs of each band of the track's built-in EQ (gain, frequency, width).
class TrackEqEditor final : public juce::Component
{
public:
    TrackEqEditor (AudioEngine& e, int t)
        : engine (e),
          track (t)
    {
        const juce::String bands[] = { tr ("Low"), tr ("Mid"), tr ("High") };
        const auto& eq = engine.getSong().mixer[(std::size_t) track].eq;

        for (int b = 0; b < 3; ++b)
        {
            titles[(std::size_t) b].setText (bands[b], juce::dontSendNotification);
            titles[(std::size_t) b].setJustificationType (juce::Justification::centred);
            addAndMakeVisible (titles[(std::size_t) b]);

            auto& gain = knobs[(std::size_t) b][0];
            setup (gain, -18.0, 18.0, 0.1, " dB", eq.gainDb[(std::size_t) b]);
            gain.setDoubleClickReturnValue (true, 0.0);

            auto& hz = knobs[(std::size_t) b][1];
            setup (hz, 10.0, 16000.0, 1.0, " Hz", eq.hz[(std::size_t) b]);
            hz.setSkewFactorFromMidPoint (1000.0);

            auto& width = knobs[(std::size_t) b][2];
            setup (width, 0.0, 1.0, 0.01, "", eq.width[(std::size_t) b]);
            width.setTooltip (b == 1 ? tr ("Peak width (Q)") : tr ("Shelf slope"));
        }

        setSize (3 * 90, 3 * 74 + 24);
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (20);

        for (int b = 0; b < 3; ++b)
        {
            titles[(std::size_t) b].setBounds (header.removeFromLeft (90));
            auto column = area.removeFromLeft (90);

            for (auto& k : knobs[(std::size_t) b])
                k.setBounds (column.removeFromTop (74));
        }
    }

private:
    void setup (juce::Slider& s, double lo, double hi, double step, const char* suffix, float value)
    {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 72, 16);
        s.setRange (lo, hi, step);
        s.setTextValueSuffix (suffix);
        s.setValue (value, juce::dontSendNotification);
        s.onValueChange = [this] { apply(); };
        addAndMakeVisible (s);
    }

    void apply()
    {
        TrackEq eq;

        for (std::size_t b = 0; b < 3; ++b)
        {
            eq.gainDb[b] = (float) knobs[b][0].getValue();
            eq.hz[b] = (float) knobs[b][1].getValue();
            eq.width[b] = (float) knobs[b][2].getValue();
        }

        engine.editSong ([t = track, eq] (Song& s) {
            if (t < (int) s.mixer.size())
                s.mixer[(std::size_t) t].eq = eq;
        });
    }

    AudioEngine& engine;
    int track;
    std::array<juce::Label, 3> titles;
    std::array<std::array<juce::Slider, 3>, 3> knobs; // [band][gain, frequency, width]
};
} // namespace

void MixerStrips::showTrackEq (int t)
{
    auto editor = std::make_unique<TrackEqEditor> (engine, t);
    juce::CallOutBox::launchAsynchronously (std::move (editor), getScreenBounds().withSize (10, 10).translated (getMouseXYRelative().x, getMouseXYRelative().y), nullptr);
}

namespace
{
juce::File mixerStatesFolder()
{
    return lynx::platform::documentsFolder().getChildFile ("LYNX/Mixer states");
}

juce::Array<juce::File> savedMixerStates()
{
    auto files = mixerStatesFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + ProjectFile::mixerTrackStateExtension);
    files.sort();
    return files;
}

std::unique_ptr<juce::FileChooser>& mixerStateChooser()
{
    static std::unique_ptr<juce::FileChooser> chooser;
    return chooser;
}
} // namespace

bool loadMixerTrackState (AudioEngine& engine, int track, const juce::File& file, bool asNewInsert)
{
    juce::String error;
    auto state = ProjectFile::mixerTrackStateFromJson (file.loadFileAsString(), error);
    if (! state || track < 0 || track >= (int) engine.getSong().mixer.size())
        return false;
    if (state->name.empty())
        state->name = file.getFileNameWithoutExtension().toStdString();
    bool ok = true;
    engine.editSong ([&] (Song& s) {
        auto at = track;
        if (asNewInsert)
        {
            at = juce::jmax (1, track + 1);
            ok = mixerops::insertTrack (s, at);
        }
        if (ok)
            ProjectFile::applyMixerTrackState (s.mixer[(std::size_t) at], *state, asNewInsert);
    });
    return ok;
}

bool saveMixerTrackState (AudioEngine& engine, int track, const juce::File& file)
{
    if (track < 0 || track >= (int) engine.getSong().mixer.size())
        return false;
    engine.capturePluginStates(); // the plugins' current settings go into the state
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (ProjectFile::mixerTrackStateToJson (engine.getSong().mixer[(std::size_t) track]));
}

void MixerStrips::showMenu (int t)
{
    buildMenu (t).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::PopupMenu MixerStrips::buildMenu (int t)
{
    const auto& song = engine.getSong();
    const auto channel = engine.getSelectedChannel();
    juce::PopupMenu menu;
    if (t < 0 || t >= (int) song.mixer.size())
        return menu;
    // the tracks the "selected track(s)" items act on: the selection when the clicked track is in it
    const auto targets = std::find (selection.begin(), selection.end(), t) != selection.end() ? selection : std::vector<int> { t };
    const auto trackName = juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str());

    // the title band in the insert's colour (FL tints the whole menu)
    ui::addItemHeader (menu, t == 0 ? tr ("Master") : tr ("Mixer track {0}", { juce::String (t) }), trackName,
                       song.mixer[(std::size_t) t].colour != 0 ? juce::Colour (song.mixer[(std::size_t) t].colour) : juce::Colour());

    // FL's first section: name and colour (REA_rodada115)
    menu.addItem (tr ("Rename and color...") + "\tF2", [this, t] {
        const auto& track = engine.getSong().mixer[(std::size_t) t];
        showNameAndColourDialog (tr ("Rename track"), juce::String::fromUTF8 (track.name.c_str()), track.colour,
                                 [this, t] (const juce::String& name, std::uint32_t colour) {
                                     if (t < (int) engine.getSong().mixer.size())
                                         engine.editSong ([t, n = name.trim().toStdString(), colour] (Song& s) {
                                             if (! n.empty())
                                                 s.mixer[(std::size_t) t].name = n;
                                             s.mixer[(std::size_t) t].colour = colour;
                                         });
                                 });
    });
    menu.addSubMenu (tr ("Change color"), ui::colourMenu (song.mixer[(std::size_t) t].colour, [this, t] (std::uint32_t colour) {
                         if (t < (int) engine.getSong().mixer.size())
                             engine.editSong ([t, colour] (Song& s) { s.mixer[(std::size_t) t].colour = colour; });
                     }, juce::Rectangle<int> (juce::Desktop::getMousePosition().x, juce::Desktop::getMousePosition().y, 1, 1)));
    menu.addItem (tr ("Random color"), [this, t] {
        const auto& palette = lynxPalette();
        const auto colour = palette[(std::size_t) juce::Random::getSystemRandom().nextInt ((int) palette.size())];
        engine.editSong ([t, colour] (Song& s) { s.mixer[(std::size_t) t].colour = colour; });
    });
    menu.addSeparator();

    // FL's "File" submenu: the track's state (LYNX's own .lynxtrack files)
    {
        juce::PopupMenu file;
        file.addItem (tr ("Open mixer track state..."), [this, t] {
            auto& chooser = mixerStateChooser();
            chooser = std::make_unique<juce::FileChooser> (tr ("Open mixer track state"), mixerStatesFolder(),
                                                           juce::String ("*") + ProjectFile::mixerTrackStateExtension);
            juce::Component::SafePointer<MixerStrips> safe (this);
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, t] (const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult().existsAsFile())
                    loadMixerTrackState (safe->engine, t, fc.getResult(), false);
            });
        });
        file.addItem (tr ("Save mixer track state as...") + "\tCtrl+Shift+S", [this, t, trackName] {
            auto& chooser = mixerStateChooser();
            const auto name = juce::File::createLegalFileName (trackName.isNotEmpty() ? trackName : tr ("Mixer track {0}", { juce::String (t) }));
            chooser = std::make_unique<juce::FileChooser> (tr ("Save mixer track state"), mixerStatesFolder().getChildFile (name + ProjectFile::mixerTrackStateExtension),
                                                           juce::String ("*") + ProjectFile::mixerTrackStateExtension);
            juce::Component::SafePointer<MixerStrips> safe (this);
            mixerStatesFolder().createDirectory();
            chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe, t] (const juce::FileChooser& fc) {
                if (safe != nullptr && fc.getResult() != juce::File())
                    saveMixerTrackState (safe->engine, t, fc.getResult().withFileExtension (ProjectFile::mixerTrackStateExtension));
            });
        });
        file.addItem (tr ("Browse states"), [] {
            mixerStatesFolder().createDirectory();
            mixerStatesFolder().startAsProcess();
        });
        const auto states = savedMixerStates();
        if (! states.isEmpty())
        {
            file.addSectionHeader (tr ("States"));
            for (const auto& f : states)
                file.addItem (f.getFileNameWithoutExtension(), [this, t, f] { loadMixerTrackState (engine, t, f, false); });
        }
        menu.addSubMenu (tr ("File"), file);
    }
    if (t > 0)
    {
        const auto& mt = song.mixer[(std::size_t) t];
        const auto current = mt.inputChannel >= 0
                                 ? juce::String::fromUTF8 (inputSourceName (mt.inputChannel, mt.inputStereo, engine.getInputChannelNames()).c_str())
                                 : tr ("none");
        menu.addItem (tr ("Audio input: {0}...", { current }), [this, t] { mixerinput::showMenu (engine, t, this); });
    }
    menu.addItem (tr ("Open audio editor"), [this] { openAudioEditor (engine); });
    menu.addItem (tr ("Reset selected track(s) to default"), [this, targets] { engine.editSong ([targets] (Song& s) { mixerops::resetToDefault (s, targets); }); });

    // --- FL's "Routing" section -----------------------------------------------------------------------
    menu.addSectionHeader (tr ("Routing"));
    {
        juce::PopupMenu channels;
        channels.addItem (tr ("Route selected channels to this track") + "\tCtrl+L", [this, t] {
            setSelection (selection, t);
            linkSelectedChannels (false);
        });
        channels.addItem (tr ("Route selected channels starting from this track") + "\tCtrl+Shift+L", [this, t] {
            setSelection (selection, t);
            linkSelectedChannels (true);
        });
        if (channel < (int) song.channels.size())
            channels.addItem (tr ("Route channel \"{0}\" here", { juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str()) }),
                              [this, channel, t] { engine.editSong ([channel, t] (Song& s) { s.channels[(std::size_t) channel].mixerTrack = t; }); });
        menu.addSubMenu (tr ("Channel routing"), channels);
    }
    {
        juce::PopupMenu select;
        select.addItem (tr ("Channels routed to this track") + "\tAlt+L", ! mixerops::channelsOn (song, t).empty(), false, [this, t] {
            setSelection (selection, t);
            selectLinkedChannels();
        });
        select.addSeparator();
        const auto destinations = mixerops::destinationsOf (song, t);
        const auto sources = mixerops::sourcesOf (song, t);
        select.addItem (tr ("Tracks this one is routed to"), ! destinations.empty(), false, [this, t, destinations] { setSelection (destinations, t); });
        select.addItem (tr ("Tracks routed to this one"), ! sources.empty(), false, [this, t, sources] { setSelection (sources, t); });
        select.addSeparator();
        select.addItem (tr ("Group"), t > 0, false, [this, t] {
            auto group = mixerops::groupOf (engine.getSong(), t);
            std::sort (group.begin(), group.end());
            setSelection (group, t);
        });
        select.addItem (tr ("All"), [this, t] {
            std::vector<int> all ((std::size_t) engine.getSong().mixer.size());
            std::iota (all.begin(), all.end(), 0);
            setSelection (all, t);
        });
        menu.addSubMenu (tr ("Select"), select);
    }
    menu.addItem (tr ("Reset selected track(s) routing"), [this, targets] { engine.editSong ([targets] (Song& s) { mixerops::resetRouting (s, targets); }); });
    menu.addItem (tr ("Unroute tracks routed to the selected track(s)"), [this, targets] { engine.editSong ([targets] (Song& s) { mixerops::unrouteInto (s, targets); }); });
    {
        // route the other selected inserts into this one
        std::vector<int> sources;
        for (auto s : selection)
            if (s > 0 && s != t)
                sources.push_back (s);

        if (! sources.empty())
        {
            menu.addItem (tr ("Route the selected inserts here"), [this, sources, t] {
                engine.editSong ([sources, t] (Song& s) { mixerops::routeTo (s, sources, t, false); });
            });
            menu.addItem (tr ("Route the selected inserts only here"), [this, sources, t] {
                engine.editSong ([sources, t] (Song& s) { mixerops::routeTo (s, sources, t, true); });
            });
        }
    }
    if (selected > 0 && selected != t && (t == 0 || song.canRoute (selected, t) || song.routes (selected, t)))
        menu.addItem (tr ("Send \"{0}\" only to this track", { juce::String::fromUTF8 (song.mixer[(std::size_t) selected].name.c_str()) }),
                      [this, from = selected, t] {
                          engine.editSong ([from, t] (Song& s) {
                              s.mixer[(std::size_t) from].sends.clear();
                              s.mixer[(std::size_t) from].toMaster = false;
                              s.setRoute (from, t, true);
                          });
                      });
    menu.addSeparator();
    {
        const auto inserts = [targets] {
            std::vector<int> v;
            for (auto x : targets)
                if (x > 0)
                    v.push_back (x);
            return v;
        }();
        menu.addItem (tr ("Solo selected track(s)") + "\tS", ! inserts.empty(), false, [this, inserts] {
            engine.editSong ([inserts] (Song& s) {
                for (std::size_t i = 1; i < s.mixer.size(); ++i)
                    s.mixer[i].solo = std::find (inserts.begin(), inserts.end(), (int) i) != inserts.end();
            });
        });
        menu.addItem (tr ("Arm selected track(s)"), ! inserts.empty(), false, [this, inserts] {
            for (auto x : inserts)
                if (! engine.isInsertArmed (x))
                    mixerinput::toggleArm (engine, x, this);
        });
        menu.addItem (tr ("Disarm selected track(s)"), ! inserts.empty(), false, [this, inserts] {
            for (auto x : inserts)
                if (engine.isInsertArmed (x))
                    engine.setInsertArmed (x, false);
        });
    }

    // --- LYNX's sound items (FL has them on the strip itself) -------------------------------------------
    menu.addSectionHeader (tr ("Sound"));
    {
        const auto& track = song.mixer[(std::size_t) t];
        juce::PopupMenu stereo;
        const std::pair<const char*, float> widths[] = { { TR_NOOP ("Mono"), 1.0f }, { TR_NOOP ("Narrower"), 0.5f }, { TR_NOOP ("Normal"), 0.0f },
                                                          { TR_NOOP ("Wider"), -0.5f }, { TR_NOOP ("Very wide"), -1.0f } };

        for (const auto& [label, value] : widths)
            stereo.addItem (tr (label), true, std::abs (track.stereo - value) < 0.01f,
                            [this, t, v = value] { engine.editSong ([t, v] (Song& s) { s.mixer[(std::size_t) t].stereo = v; }); });

        menu.addSubMenu (tr ("Stereo separation"), stereo);
        if (! song.cleanMixer)
            menu.addItem (tr ("Track EQ..."), [this, t] { showTrackEq (t); });
        menu.addItem (tr ("Invert phase"), true, track.invertPhase, [this, t] {
            engine.editSong ([t] (Song& s) { s.mixer[(std::size_t) t].invertPhase = ! s.mixer[(std::size_t) t].invertPhase; });
        });
        menu.addItem (tr ("Swap left/right"), true, track.swapChannels, [this, t] {
            engine.editSong ([t] (Song& s) { s.mixer[(std::size_t) t].swapChannels = ! s.mixer[(std::size_t) t].swapChannels; });
        });
        if (t > 0)
            menu.addItem (tr ("Render to audio (freeze)..."), [this, t] { bounceInsertToAudio (engine, t); });
    }

    // --- FL's "Group" section (REA round 146a) -----------------------------------------------------------
    if (t > 0)
    {
        menu.addSectionHeader (tr ("Group"));
        const auto on = song.mixer[(std::size_t) t].separator;
        menu.addItem (tr ("Separator"), true, on, [this, targets, on] {
            engine.editSong ([targets, on] (Song& s) { mixerops::setSeparator (s, targets, ! on); });
        });
        menu.addItem (tr ("Create group"), [this, targets, t] { engine.editSong ([targets, t] (Song& s) { mixerops::createGroup (s, targets, t); }); });
        menu.addItem (tr ("Auto color group"), song.mixer[(std::size_t) t].colour != 0, false,
                      [this, t] { engine.editSong ([t] (Song& s) { mixerops::autoColorGroup (s, t); }); });
    }

    // --- FL's "Layout" section --------------------------------------------------------------------------
    {
        menu.addSectionHeader (tr ("Layout"));
        juce::PopupMenu dockMenu;
        const auto dock = song.mixer[(std::size_t) t].dock;
        const std::pair<const char*, MixerDock> docks[] = { { TR_NOOP ("Left"), MixerDock::left }, { TR_NOOP ("Middle"), MixerDock::middle },
                                                            { TR_NOOP ("Right"), MixerDock::right } };
        for (const auto& [label, d] : docks)
            dockMenu.addItem (tr (label), true, dock == d, [this, targets, d = d] {
                engine.editSong ([targets, d] (Song& s) { mixerops::dockTracks (s, targets, d); });
            });
        menu.addSubMenu (tr ("Dock to"), dockMenu);
    }
    if (t > 0)
    {
        menu.addItem (tr ("Move left") + "\tAlt+Left", t > 1, false, [this, t] {
            setSelection ({ t }, t);
            moveSelected (-1);
        });
        menu.addItem (tr ("Move right") + "\tAlt+Right", t + 1 < (int) song.mixer.size(), false, [this, t] {
            setSelection ({ t }, t);
            moveSelected (1);
        });
        menu.addSeparator();
        menu.addItem (tr ("Delete") + "\tDel", [this, t] {
            engine.editSong ([t] (Song& s) { mixerops::deleteTrack (s, t); });
            setSelection ({ t }, t);
        });
        const auto room = (int) song.mixer.size() < maxMixerTracks;
        menu.addItem (tr ("Insert one"), room, false, [this, t] {
            engine.editSong ([t] (Song& s) { mixerops::insertTrack (s, t); });
            setSelection ({ t }, t);
        });
        menu.addItem (tr ("Insert multiple..."), room, false, [this, t] {
            auto* window = new juce::AlertWindow (tr ("Insert multiple"), tr ("How many blank inserts?"), juce::MessageBoxIconType::NoIcon);
            window->addTextEditor ("count", "2");
            window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            juce::Component::SafePointer<MixerStrips> safe (this);
            window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, t, window] (int result) {
                                         const auto n = juce::jlimit (0, maxMixerTracks, window->getTextEditorContents ("count").getIntValue());
                                         if (safe != nullptr && result == 1 && n > 0)
                                             safe->engine.editSong ([t, n] (Song& s) { mixerops::insertTracks (s, t, n); });
                                     }),
                                     true);
        });
        juce::PopupMenu fromState;
        for (const auto& f : savedMixerStates())
            fromState.addItem (f.getFileNameWithoutExtension(), room, false, [this, t, f] { loadMixerTrackState (engine, t - 1, f, true); });
        menu.addSubMenu (tr ("Insert from state"), fromState, room && fromState.getNumItems() > 0);

        // FL's last section, "CPU" (REA round 134)
        menu.addSectionHeader (tr ("CPU"));
        menu.addItem (tr ("Allow threaded processing"), true, song.mixer[(std::size_t) t].threaded, [this, t] {
            engine.editSong ([t] (Song& s) { s.mixer[(std::size_t) t].threaded = ! s.mixer[(std::size_t) t].threaded; });
        });
    }

    menu.addSectionHeader (tr ("Mixer"));
    mixeroptions::addItems (menu, engine, false);

    return menu;
}

void MixerStrips::changeListenerCallback (juce::ChangeBroadcaster*)
{
    selected = juce::jlimit (0, juce::jmax (0, (int) engine.getSong().mixer.size() - 1), selected);
    const auto count = (int) engine.getSong().mixer.size();
    selection.erase (std::remove_if (selection.begin(), selection.end(), [count] (int t) { return t >= count; }), selection.end());
    if (! std::binary_search (selection.begin(), selection.end(), selected))
        selection = { selected };
    updateSize();
    repaint();
}

void MixerStrips::timerCallback()
{
    auto& meters = engine.getMeters();
    const auto showing = isShowing();
    const auto& mixer = engine.getSong().mixer;
    inputLevels.resize (mixer.size(), 0.0f);

    for (std::size_t t = 1; t < mixer.size(); ++t)
    {
        const auto peak = mixer[t].inputChannel >= 0 ? juce::jmax (engine.takeInputPeak ((int) t, 0), engine.takeInputPeak ((int) t, 1)) : 0.0f;
        auto level = juce::jmax (peak, inputLevels[t] * 0.8f);
        level = level < 1e-4f ? 0.0f : level;
        if (std::abs (level - inputLevels[t]) > 1e-4f)
        {
            inputLevels[t] = level;
            if (showing)
                repaint (inputArea ((int) t).expanded (1));
        }
    }

    for (std::size_t i = 0; i < levels.size(); ++i)
    {
        auto& m = levels[i];
        const auto before = m;
        m.tick (meters.takePeak ((int) (i / 2), (int) (i % 2))); // ~20 dB/s falloff at 30 Hz, 1.5 s peak hold

        // only the meter that changed is repainted
        if (showing && (std::abs (m.level - before.level) > 1e-4f || std::abs (m.peak - before.peak) > 1e-4f || m.clipped != before.clipped))
            repaint (meterArea ((int) (i / 2)).expanded (1));
    }
}

// ---------------------------------------------------------------------------

namespace
{
constexpr int panelHeader = 30;
constexpr int inputRowHeight = 30; // the input row (arm, audio input source) above the slots
constexpr int slotsTop = panelHeader + inputRowHeight;
constexpr int slotHeight = 24;
const EffectKind allEffects[] = { EffectKind::eq,     EffectKind::parametricEq, EffectKind::facetEq, EffectKind::compressor, EffectKind::limiter, EffectKind::apexLimiter,
                                      EffectKind::delay,  EffectKind::reverb,       EffectKind::chorus,     EffectKind::filter,
                                      EffectKind::distortion, EffectKind::utility, EffectKind::volumeGate,
                                      EffectKind::pitchShift, EffectKind::freqShift, EffectKind::lofi, EffectKind::effector, EffectKind::crossover, EffectKind::frequencySplitter, EffectKind::autoPan, EffectKind::vocoder, EffectKind::pitchCorrect,
                                      EffectKind::ridgeStrip, EffectKind::glowLeveler, EffectKind::hushDeEsser, EffectKind::aether, EffectKind::ember, // LYNX's mixing suite
                                      EffectKind::lynxTune, // LYNX's own pitch corrector
                                      EffectKind::epochVerb, EffectKind::atrium, EffectKind::foilPlate, EffectKind::nebula, EffectKind::halo, EffectKind::horizon, // LYNX's reverbs
                                      EffectKind::echoform, EffectKind::spiralEcho, EffectKind::orbit, EffectKind::latticeMod, // LYNX's delay / modulation suite
                                      EffectKind::crestCompressor, EffectKind::prismMultiband, EffectKind::sentryGate, // LYNX's dynamics suite
                                      EffectKind::quellDenoiser, EffectKind::geminiDoubler, EffectKind::voiceShift, // LYNX's voice tools
                                      EffectKind::formulaController, EffectKind::peakController, EffectKind::xyController, EffectKind::xyzController,
                                      EffectKind::controlSurface, // controllers (link their output to any knob)
                                      EffectKind::patcher, // FL's Patcher: a graph of effects / instruments / plugins (REA_rodada141)
                                      EffectKind::hardwareInsert, // outboard gear through device outputs / inputs, pinged round trip (round 164d)
                                      EffectKind::fruityLimiter, EffectKind::fruityCompressor,
                                      EffectKind::fruityWaveShaper, EffectKind::fruitySoftClipper,
                                      EffectKind::reeverb2, EffectKind::reeverb1,
                                      EffectKind::fruityDelay2, EffectKind::fruityDelay3,
                                      EffectKind::stereoShaper, EffectKind::stereoEnhancer, EffectKind::transientProcessor,
                                      EffectKind::spreader, EffectKind::fruityChorus, EffectKind::hyperChorus, EffectKind::vintageChorus,
                                      EffectKind::fruityPanOMatic, EffectKind::fruityBalance, EffectKind::fruityBloodOverdrive, EffectKind::fruitySend,
                                      EffectKind::fruityDelayBank, EffectKind::fruityMultiband,
                                      EffectKind::fruityFastDist, EffectKind::fruityFlangus, EffectKind::fruityFlanger, EffectKind::fruityPhaser, EffectKind::luxeVerb,
                                      EffectKind::fruityFastLP, EffectKind::fruityFreeFilter, EffectKind::fruityFilter,
                                      EffectKind::hardcore,
                                      EffectKind::multibandDelay, EffectKind::emphasizer, EffectKind::flFrequencyShifter, EffectKind::flLowLifter,
                                      EffectKind::waveCandy, EffectKind::visualizer, // visual plugins (round 164c)
                                      EffectKind::fruityMute2, EffectKind::bigClock, EffectKind::tuner, EffectKind::htmlNotebook, // round 165g
                                      // FL engines that need more than their parameters (round 142, issue #5): a new one starts from
                                      // the plugin's Default preset of the user's FL installation (core/NativeEngines.h)
                                      EffectKind::maximus, EffectKind::vocodex, EffectKind::equo, EffectKind::distructor, EffectKind::lovePhilter,
                                      EffectKind::grossBeat, EffectKind::convolver, EffectKind::flPitchShifter, EffectKind::vintagePhaser,
                                      EffectKind::squeeze, EffectKind::rhythmGain, EffectKind::newtone, EffectKind::newtime };
} // namespace

const std::vector<EffectKind>& addableEffects()
{
    static const std::vector<EffectKind> list (std::begin (allEffects), std::end (allEffects));
    return list;
}

bool addableEffectAvailable (EffectKind kind)
{
    const auto* e = natives::effectEngine (kind);
    return e == nullptr || natives::available (*e, flStudioAccess());
}

juce::String addableEffectName (EffectKind kind)
{
    const auto name = juce::String::fromUTF8 (effectInfo (kind).name);
    // LYNX's own pitch / frequency effects sit next to FL's look-alikes (Pitch Shifter, Frequency Shifter, Pitcher)
    // in the lists: tell them apart (round 162, issue #7 review)
    if (kind == EffectKind::pitchShift || kind == EffectKind::freqShift || kind == EffectKind::pitchCorrect)
        return tr ("{0} (LYNX)", { name });
    if (const auto* e = natives::effectEngine (kind))
    {
        if (! natives::available (*e, flStudioAccess()))
            return tr ("{0} (requires FL Studio)", { name });
        if (e->beta)
            return tr ("{0} (beta)", { name });
    }
    return name;
}

std::optional<EffectSlot> newAddableEffect (EffectKind kind, double bpm)
{
    return natives::newEffect (kind, flStudioAccess(), bpm);
}

EffectsPanel::EffectsPanel (AudioEngine& e)
    : engine (e)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    engine.addChangeListener (this);
    startTimerHz (30);
}

EffectsPanel::~EffectsPanel()
{
    engine.removeChangeListener (this);
}

juce::Rectangle<int> EffectsPanel::armBounds() const
{
    return { 8, panelHeader + 2, 24, inputRowHeight - 8 };
}

juce::Rectangle<int> EffectsPanel::inputBoxBounds() const
{
    return { 38, panelHeader + 2, getWidth() - 46, inputRowHeight - 8 };
}

void EffectsPanel::timerCallback()
{
    // the input level of the shown insert (the strips read their own); decays like a meter
    const auto& mixer = engine.getSong().mixer;
    const auto hasInput = track >= 0 && track < (int) mixer.size() && mixer[(std::size_t) track].inputChannel >= 0;
    auto changed = false;

    for (int side = 0; side < 2; ++side)
    {
        const auto peak = hasInput ? engine.peekInputPeak (track, side) : 0.0f;
        const auto level = juce::jmax (peak, inputLevel[(std::size_t) side] * 0.8f);
        changed = changed || std::abs (level - inputLevel[(std::size_t) side]) > 1e-4f;
        inputLevel[(std::size_t) side] = level < 1e-4f ? 0.0f : level;
    }

    if (changed && isShowing())
        repaint (inputBoxBounds());

    // another effect selected (its controls need another height): fold / unfold the EQ, scroll (round 164)
    if (const auto* s = slots(); s != nullptr && isShowing())
    {
        const auto kind = selectedSlot >= 0 && selectedSlot < (int) s->size() ? (int) (*s)[(std::size_t) selectedSlot].kind : -1;
        if (selectedSlot != fittedSlot || kind != fittedKind)
        {
            fittedSlot = selectedSlot;
            fittedKind = kind;
            refit();
        }
    }
}

void EffectsPanel::setTrack (int t)
{
    track = t;
    const auto* s = slots();
    selectedSlot = s != nullptr && ! s->empty() ? 0 : -1;
    refit();
    repaint();
}

const std::vector<EffectSlot>* EffectsPanel::slots() const
{
    const auto& mixer = engine.getSong().mixer;
    return track >= 0 && track < (int) mixer.size() ? &mixer[(std::size_t) track].effects : nullptr;
}

juce::Rectangle<int> EffectsPanel::mixBounds (int slot) const
{
    return slotBounds (slot).withTrimmedRight (20).removeFromRight (24);
}

juce::Rectangle<int> EffectsPanel::ledBounds (int slot) const
{
    return slotBounds (slot).removeFromRight (20);
}

juce::Rectangle<int> EffectsPanel::arrowBounds (int slot) const
{
    return slotBounds (slot).removeFromLeft (18);
}

// --- the insert's built-in EQ and its output (FL's layout under the slots, REA round 104) ---------

namespace
{
constexpr int eqGraphHeight = 84, eqKnobsHeight = 46, outputRowHeight = 30;
constexpr float eqMinHz = 20.0f, eqMaxHz = 20000.0f, eqRangeDb = 18.0f;
const char* const eqBandNames[] = { TR_NOOP ("Low shelf"), TR_NOOP ("Peak"), TR_NOOP ("High shelf") };

juce::Colour eqBandColour (int band)
{
    return band == 0 ? juce::Colour (0xFFE0A33Au) : band == 1 ? juce::Colour (0xFF5FC27Eu) : juce::Colour (0xFF5B9BE6u);
}

// stored 0..1 <-> the band's value
float eqToValue (const TrackEq& eq, int band, int what)
{
    const auto b = (std::size_t) band;
    if (what == 0)
        return (eq.gainDb[b] + eqRangeDb) / (2.0f * eqRangeDb);
    if (what == 1)
        return std::log (eq.hz[b] / 10.0f) / std::log (1600.0f);
    return eq.width[b];
}

void eqFromValue (TrackEq& eq, int band, int what, float v)
{
    const auto b = (std::size_t) band;
    v = juce::jlimit (0.0f, 1.0f, v);
    if (what == 0)
        eq.gainDb[b] = std::round ((v * 2.0f * eqRangeDb - eqRangeDb) * 10.0f) / 10.0f;
    else if (what == 1)
        eq.hz[b] = std::round (10.0f * std::pow (1600.0f, v));
    else
        eq.width[b] = v;
}

// the EQ's response at `hz` (dB), with the filters the mixer runs (RackRenderer::applyTrackEq)
float eqResponseDb (const TrackEq& eq, double hz, double sampleRate)
{
    Biquad bands[3];
    bands[0].shelf (sampleRate, std::min ((double) eq.hz[0], sampleRate * 0.45), eq.gainDb[0], false, TrackEq::shelfSlope (eq.width[0]));
    bands[1].peaking (sampleRate, std::min ((double) eq.hz[1], sampleRate * 0.45), TrackEq::peakQ (eq.width[1]), eq.gainDb[1]);
    bands[2].shelf (sampleRate, std::min ((double) eq.hz[2], sampleRate * 0.45), eq.gainDb[2], true, TrackEq::shelfSlope (eq.width[2]));
    const auto w = juce::MathConstants<double>::twoPi * hz / sampleRate;
    const std::complex<double> z1 = std::polar (1.0, -w), z2 = std::polar (1.0, -2.0 * w);
    double db = 0.0;
    for (int b = 0; b < 3; ++b)
    {
        if (eq.gainDb[(std::size_t) b] == 0.0f)
            continue; // a band at 0 dB is not processed (like FL)
        const auto& f = bands[b];
        const auto h = (f.b0 + f.b1 * z1 + f.b2 * z2) / (1.0 + f.a1 * z1 + f.a2 * z2);
        db += 20.0 * std::log10 (std::max (1e-9, std::abs (h)));
    }
    return (float) db;
}
} // namespace

constexpr int eqFoldBarHeight = 20;

bool EffectsPanel::eqShown() const
{
    return ! engine.getSong().cleanMixer; // clean mixer (round 163): no built-in EQ to show
}

int EffectsPanel::belowHeight (bool folded) const
{
    if (! eqShown()) // clean mixer: only the output row
        return outputRowHeight + 8;
    return outputRowHeight + eqFoldBarHeight + (folded ? 4 : eqGraphHeight + eqKnobsHeight + 14);
}

int EffectsPanel::bottomTop() const
{
    return getHeight() - belowHeight (eqFolded);
}

juce::Rectangle<int> EffectsPanel::eqFoldBounds() const
{
    if (! eqShown())
        return {};
    return { 8, bottomTop(), getWidth() - 16, eqFoldBarHeight };
}

int EffectsPanel::neededHeight (bool folded) const
{
    // the slots and the selected effect's controls (round 164: its knobs and their names and values too, they were
    // cut by the EQ at medium heights): when they don't fit with the EQ open, it folds; then the panel scrolls
    return selectedEffectBottom() + 6 + belowHeight (folded);
}


void EffectsPanel::fitToView (int w, int h, int scrollBar)
{
    viewWidth = w;
    viewHeight = h;
    scrollBarThickness = scrollBar;
    refit();
}

void EffectsPanel::refit()
{
    if (viewWidth <= 0 || viewHeight <= 0)
        return;
    const auto folded = eqFoldChoice == 2 || (eqFoldChoice == 0 && viewHeight < neededHeight (false));
    const auto need = neededHeight (folded);
    const auto scrolls = viewHeight < need;
    const auto changed = folded != eqFolded;
    eqFolded = folded;
    const juce::Rectangle<int> size (viewWidth - (scrolls ? scrollBarThickness : 0), juce::jmax (viewHeight, need));
    if (getWidth() != size.getWidth() || getHeight() != size.getHeight())
        setSize (size.getWidth(), size.getHeight());
    else if (changed)
        repaint();
}

juce::Rectangle<int> EffectsPanel::eqGraphBounds() const
{
    return { 8, bottomTop() + eqFoldBarHeight, getWidth() - 16 - 3 * 18 - 6, eqGraphHeight };
}

juce::Rectangle<int> EffectsPanel::eqFaderBounds (int band) const
{
    const auto graph = eqGraphBounds();
    return { graph.getRight() + 6 + band * 18, graph.getY(), 16, graph.getHeight() };
}

juce::Rectangle<int> EffectsPanel::eqKnobBounds (int band, int what) const
{
    const auto fader = eqFaderBounds (band);
    return { fader.getCentreX() - 11, eqGraphBounds().getBottom() + 2 + (what - 1) * 22, 22, 22 };
}

juce::Rectangle<int> EffectsPanel::outputBounds() const
{
    return { 34, getHeight() - outputRowHeight - 2, getWidth() - 42, outputRowHeight - 8 };
}

juce::Point<float> EffectsPanel::eqHandle (int band) const
{
    const auto graph = eqGraphBounds().toFloat().reduced (4.0f);
    const auto& eq = engine.getSong().mixer[(std::size_t) track].eq;
    const auto hz = juce::jlimit (eqMinHz, eqMaxHz, eq.hz[(std::size_t) band]);
    const auto x = graph.getX() + graph.getWidth() * std::log (hz / eqMinHz) / std::log (eqMaxHz / eqMinHz);
    const auto y = graph.getCentreY() - graph.getHeight() * 0.5f * eq.gainDb[(std::size_t) band] / eqRangeDb;
    return { x, y };
}

int EffectsPanel::eqControlAt (juce::Point<int> p) const
{
    if (slots() == nullptr || eqFolded || ! eqShown())
        return 0;
    for (int b = 0; b < 3; ++b)
    {
        if (eqFaderBounds (b).contains (p))
            return eqParamBase - b * 3;
        for (int what = 1; what <= 2; ++what)
            if (eqKnobBounds (b, what).contains (p))
                return eqParamBase - b * 3 - what;
    }
    if (eqGraphBounds().contains (p))
    {
        // the nearest handle (by distance in the graph): FL's "Tweak band N"
        int best = 0;
        float bestDistance = 1e9f;
        for (int b = 0; b < 3; ++b)
            if (const auto d = eqHandle (b).getDistanceFrom (p.toFloat()); d < bestDistance)
            {
                bestDistance = d;
                best = b;
            }
        return eqPointBase - best;
    }
    return 0;
}

void EffectsPanel::setEqFromGraph (int band, juce::Point<int> p)
{
    const auto graph = eqGraphBounds().toFloat().reduced (4.0f);
    const auto fx = juce::jlimit (0.0f, 1.0f, (p.x - graph.getX()) / graph.getWidth());
    const auto hz = juce::jlimit (10.0f, 16000.0f, std::round (eqMinHz * std::pow (eqMaxHz / eqMinHz, fx)));
    const auto db = juce::jlimit (-eqRangeDb, eqRangeDb, std::round ((graph.getCentreY() - p.y) / (graph.getHeight() * 0.5f) * eqRangeDb * 10.0f) / 10.0f);
    engine.editSong ([t = track, band, hz, db] (Song& s) {
        if (t < (int) s.mixer.size())
        {
            s.mixer[(std::size_t) t].eq.hz[(std::size_t) band] = hz;
            s.mixer[(std::size_t) t].eq.gainDb[(std::size_t) band] = db;
        }
    });
}

void EffectsPanel::paintEq (juce::Graphics& g)
{
    using namespace theme;
    const auto& song = engine.getSong();
    if (track < 0 || track >= (int) song.mixer.size())
        return;
    const auto& eq = song.mixer[(std::size_t) track].eq;
    const auto top = bottomTop();
    g.setColour (colour::border);
    g.fillRect (8, top, getWidth() - 16, 1);

    // the title bar: click folds / unfolds the EQ (it folds by itself when the mixer is too short for it)
    if (eqShown())
    {
        const auto bar = eqFoldBounds().toFloat();
        const auto active = eq.gainDb[0] != 0.0f || eq.gainDb[1] != 0.0f || eq.gainDb[2] != 0.0f;
        auto chevron = bar.withWidth (14.0f).withSizeKeepingCentre (12.0f, 12.0f);
        {
            juce::Graphics::ScopedSaveState state (g);
            if (eqFolded)
                g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, chevron.getCentreX(), chevron.getCentreY()));
            icons::draw (g, Icon::chevronDown, chevron, colour::textSecondary);
        }
        g.setFont (font (type::small, true));
        g.setColour (colour::textSecondary);
        textcache::drawText (g, tr ("EQ"), bar.withTrimmedLeft (18.0f).toNearestInt(), juce::Justification::centredLeft);
        if (eqFolded && active) // a folded EQ that still changes the sound says so
        {
            g.setColour (colour::accent);
            g.setFont (font (type::caption));
            textcache::drawText (g, tr ("active"), bar.withTrimmedLeft (44.0f).toNearestInt(), juce::Justification::centredLeft);
        }
    }
    if (! eqFolded && eqShown())
    {
        // the response graph: frequency (log) across, ±18 dB up and down
        const auto graph = eqGraphBounds().toFloat();
        theme::fillRoundRect (g, graph, radius::sm, colour::bg1);
        g.setColour (colour::border);
        g.drawRoundedRectangle (graph.reduced (0.5f), radius::sm, 1.0f);
        const auto inner = graph.reduced (4.0f);
        g.setColour (colour::gridBeat);
        for (float hz : { 100.0f, 1000.0f, 10000.0f })
        {
            const auto x = inner.getX() + inner.getWidth() * std::log (hz / eqMinHz) / std::log (eqMaxHz / eqMinHz);
            g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
        }
        g.drawHorizontalLine ((int) inner.getCentreY(), inner.getX(), inner.getRight());

        juce::Path curve;
        const auto sampleRate = juce::jmax (8000.0, engine.getTransport().sampleRate.load());
        for (int i = 0; i <= (int) inner.getWidth(); i += 2)
        {
            const auto hz = eqMinHz * std::pow (eqMaxHz / eqMinHz, (float) i / inner.getWidth());
            const auto db = juce::jlimit (-eqRangeDb, eqRangeDb, eqResponseDb (eq, hz, sampleRate));
            const auto y = inner.getCentreY() - inner.getHeight() * 0.5f * db / eqRangeDb;
            if (i == 0)
                curve.startNewSubPath (inner.getX(), y);
            else
                curve.lineTo (inner.getX() + (float) i, y);
        }
        g.setColour (eq.active() ? colour::accent : colour::textSecondary.withAlpha (0.6f));
        g.strokePath (curve, juce::PathStrokeType (1.5f));

        for (int b = 0; b < 3; ++b)
        {
            const auto hot = hoverEq == eqPointBase - b || dragParam == eqPointBase - b;
            const auto handle = eqHandle (b);
            g.setColour (eqBandColour (b).withAlpha (hot ? 1.0f : 0.8f));
            g.fillEllipse (juce::Rectangle<float> (hot ? 10.0f : 8.0f, hot ? 10.0f : 8.0f).withCentre (handle));
        }

        // the gain faders (one per band) and the frequency / width knobs under them
        for (int b = 0; b < 3; ++b)
        {
            const auto fader = eqFaderBounds (b).toFloat();
            const auto hot = hoverEq == eqParamBase - b * 3 || dragParam == eqParamBase - b * 3;
            theme::fillRoundRect (g, fader.withSizeKeepingCentre (3.0f, fader.getHeight() - 6.0f), 1.5f, colour::bg0);
            g.setColour (colour::border);
            g.drawHorizontalLine ((int) fader.getCentreY(), fader.getX() + 2.0f, fader.getRight() - 2.0f);
            const auto v = eqToValue (eq, b, 0);
            const auto y = fader.getBottom() - 6.0f - v * (fader.getHeight() - 12.0f);
            theme::fillRoundRect (g, juce::Rectangle<float> (fader.getWidth() - 2.0f, 7.0f).withCentre ({ fader.getCentreX(), y }), 2.0f, hot ? colour::textPrimary : colour::textSecondary);
            g.setColour (eqBandColour (b));
            g.fillRect (juce::Rectangle<float> (fader.getWidth() - 6.0f, 1.5f).withCentre ({ fader.getCentreX(), y }));

            for (int what = 1; what <= 2; ++what)
            {
                const auto code = eqParamBase - b * 3 - what;
                drawKnob (g, eqKnobBounds (b, what).toFloat().reduced (2.0f), eqToValue (eq, b, what), false, eqBandColour (b),
                          hoverEq == code || dragParam == code);
            }
        }

        g.setColour (colour::textSecondary);
        g.setFont (font (type::title));
        textcache::drawText (g, tr ("Equalizer"), 10, eqGraphBounds().getBottom() + 4, eqKnobBounds (0, 1).getX() - 40, 20, juce::Justification::centredLeft);
        g.setFont (font (type::caption));
        g.setColour (colour::textDisabled);
        textcache::drawText (g, tr ("Freq"), eqKnobBounds (0, 1).getX() - 34, eqKnobBounds (0, 1).getY(), 32, 22, juce::Justification::centredRight);
        textcache::drawText (g, tr ("Width"), eqKnobBounds (0, 2).getX() - 34, eqKnobBounds (0, 2).getY(), 32, 22, juce::Justification::centredRight);
    }

    // the audio output target
    auto box = outputBounds().toFloat();
    g.setColour (colour::border);
    g.fillRect (8, (int) box.getY() - 5, getWidth() - 16, 1);
    icons::draw (g, Icon::exportAudio, juce::Rectangle<float> (10.0f, box.getY(), 18.0f, box.getHeight()).withSizeKeepingCentre (16.0f, 16.0f),
                 colour::textSecondary);
    theme::fillRoundRect (g, box, radius::sm, hoverOutput ? colour::bg4 : colour::bg3);
    g.setColour (hoverOutput ? colour::borderStrong : colour::border);
    g.drawRoundedRectangle (box.reduced (0.5f), radius::sm, 1.0f);
    g.setColour (track == 0 ? colour::textPrimary : colour::textSecondary);
    g.setFont (font (type::body));
    textcache::drawText (g, outputName(), box.withTrimmedLeft (8.0f).withTrimmedRight (18.0f).toNearestInt(), juce::Justification::centredLeft, true);
    icons::draw (g, Icon::chevronDown, box.removeFromRight (18.0f).reduced (4.0f).translated (-2.0f, 0.0f), colour::textSecondary);
}

juce::String EffectsPanel::outputName() const
{
    if (track == 0 && const_cast<AudioEngine&> (engine).getDeviceManager().getCurrentAudioDevice() == nullptr)
        return tr ("(no audio device)");
    return mixeroutput::label (engine, track);
}

void EffectsPanel::showOutputMenu()
{
    mixeroutput::showMenu (engine, track, this, localAreaToGlobal (outputBounds()));
}

namespace mixeroptions
{
std::function<void()> changed;
std::function<void (bool)> setShowUsedOnly;

namespace
{
void notifyChanged()
{
    if (changed)
        changed();
}

void askNumber (const juce::String& title, const juce::String& text, int value, int lo, int hi, std::function<void (int)> done)
{
    auto* w = new juce::AlertWindow (title, text, juce::MessageBoxIconType::NoIcon);
    w->addTextEditor ("n", juce::String (value));
    w->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    w->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    w->enterModalState (true, juce::ModalCallbackFunction::create ([w, lo, hi, done = std::move (done)] (int result) {
        if (result == 1)
            done (juce::jlimit (lo, hi, w->getTextEditorContents ("n").trim().getIntValue()));
    }), true);
}
} // namespace

void addItems (juce::PopupMenu& menu, AudioEngine& engine, bool newProjects)
{
    const auto& song = engine.getSong();
    menu.addItem (tr ("Clean mixer: no built-in EQ on the inserts (this project)"), true, song.cleanMixer, [&engine] {
        const auto on = ! engine.getSong().cleanMixer;
        engine.editSong ([on] (Song& s) { s.cleanMixer = on; });
        const auto& mixer = engine.getSong().mixer;
        const auto withEq = std::count_if (mixer.begin(), mixer.end(), [] (const MixerTrack& t) { return t.eq.active(); });
        if (on && withEq > 0) // nothing is lost: switching it off brings the EQ back as it was
            ui::postHint ({ tr ("Clean mixer: the EQ of {0} insert(s) is bypassed and hidden (kept in the project)", { juce::String ((int) withEq) }), {}, false, false }, 5000);
    });
    menu.addItem (tr ("Show only used inserts"), true, MixerStrips::showUsedOnly, [] {
        const auto on = ! MixerStrips::showUsedOnly;
        if (setShowUsedOnly)
            setShowUsedOnly (on);
        else
            MixerStrips::showUsedOnly = on;
        notifyChanged();
    });
    menu.addItem (tr ("Remove unused inserts at the end"), song.mixer.size() > 1 && ! song.insertInUse ((int) song.mixer.size() - 1), false, [&engine] {
        int removed = 0;
        engine.editSong ([&removed] (Song& s) { removed = mixerops::removeUnusedTrailingInserts (s); });
        ui::postHint ({ tr ("Removed {0} unused insert(s)", { juce::String (removed) }), {}, false, false }, 3000);
    });

    if (! newProjects)
        return;

    auto& d = newProjectDefaults();
    menu.addSectionHeader (tr ("New projects"));
    {
        juce::PopupMenu inserts;
        bool listed = false;
        for (int n : { 0, 1, 2, 4, 8, 12, 16, 24, 32, 48, 64, 100, 125 })
        {
            listed = listed || n == d.inserts;
            inserts.addItem (n == NewProjectDefaults::defaultInserts ? tr ("{0} (default)", { juce::String (n) }) : juce::String (n), true, n == d.inserts, [n] {
                newProjectDefaults().inserts = n;
                notifyChanged();
            });
        }
        inserts.addItem (listed ? tr ("Other...") : tr ("Other ({0})...", { juce::String (d.inserts) }), true, ! listed, [] {
            askNumber (tr ("Inserts in new projects"),
                       tr ("How many mixer inserts a new project starts with (0 to {0}). More are added when you need them.", { juce::String (maxMixerTracks - 1) }),
                       newProjectDefaults().inserts, 0, maxMixerTracks - 1, [] (int n) {
                           newProjectDefaults().inserts = n;
                           notifyChanged();
                       });
        });
        menu.addSubMenu (tr ("Inserts in new projects ({0})", { juce::String (d.inserts) }), inserts);
    }
    {
        juce::PopupMenu chain;
        const std::pair<const char*, MasterChain> chains[] = { { TR_NOOP ("Empty (nothing on the Master)"), MasterChain::empty },
                                                               { TR_NOOP ("Soft clipper only"), MasterChain::clipper },
                                                               { TR_NOOP ("LYNX default (limiter)"), MasterChain::lynxDefault } };
        for (const auto& [label, c] : chains)
            chain.addItem (tr (label), true, d.master == c, [c = c] {
                newProjectDefaults().master = c;
                notifyChanged();
            });
        menu.addSubMenu (tr ("Master chain in new projects"), chain);
    }
    menu.addItem (tr ("Clean mixer in new projects (no built-in EQ)"), true, d.cleanMixer, [] {
        newProjectDefaults().cleanMixer = ! newProjectDefaults().cleanMixer;
        notifyChanged();
    });
    {
        juce::PopupMenu tracks;
        for (int n : { 4, 8, 16, 24, 32, 64, 100 })
            tracks.addItem (n == 16 ? tr ("{0} (default)", { juce::String (n) }) : juce::String (n), true, n == d.playlistTracks, [n] {
                newProjectDefaults().playlistTracks = n;
                notifyChanged();
            });
        menu.addSubMenu (tr ("Playlist tracks shown ({0})", { juce::String (d.playlistTracks) }), tracks);
    }
}
} // namespace mixeroptions

namespace mixeroutput
{
namespace
{
juce::String modeSuffix (OutputMode mode)
{
    switch (mode)
    {
        case OutputMode::stereo: return {};
        case OutputMode::monoSum: return " " + tr ("(mono)");
        case OutputMode::monoLeft: return " " + tr ("(mono, L)");
        case OutputMode::monoRight: return " " + tr ("(mono, R)");
    }
    return {};
}
} // namespace

bool isCustom (const AudioEngine& engine, int track)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size())
        return false;
    const auto channel = mixer[(std::size_t) track].outputChannel;
    return track == 0 ? channel != outputDefault : channel >= 0;
}

juce::String label (const AudioEngine& engine, int track, bool compact)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size())
        return {};

    const auto& mt = mixer[(std::size_t) track];
    auto channel = mt.outputChannel;
    auto mode = mt.outputMode;
    if (track == 0 && channel == outputDefault)
        channel = 0, mode = OutputMode::stereo;
    if (channel < 0)
        return tr ("(none)");

    const auto names = engine.getOutputChannelNames();
    const auto missing = ! outputAvailable (channel, mode, (int) names.size())
                         || ! engine.isOutputChannelOpen (channel) || (mode == OutputMode::stereo && ! engine.isOutputChannelOpen (channel + 1));

    if (compact) // the strip badge: the channel numbers only
    {
        auto text = juce::String::fromUTF8 ("\xe2\x86\x92 ") + juce::String (channel + 1);
        if (mode == OutputMode::stereo)
            text << "-" << (channel + 2);
        else
            text << (mode == OutputMode::monoLeft ? " L" : mode == OutputMode::monoRight ? " R" : " m");
        return missing ? text + " ?" : text;
    }

    auto nameOf = [&] (int c) {
        return c < (int) names.size() ? juce::String::fromUTF8 (names[(std::size_t) c].c_str()) : tr ("Out {0}", { juce::String (c + 1) });
    };
    auto text = mode == OutputMode::stereo ? nameOf (channel) + " - " + nameOf (channel + 1) : nameOf (channel);
    text += modeSuffix (mode);
    if (missing)
        text += " " + tr ("(missing)");
    return text;
}

void showMenu (AudioEngine& engine, int track, juce::Component* target, juce::Rectangle<int> screenArea)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size())
        return;

    const auto& mt = mixer[(std::size_t) track];
    const auto names = engine.getOutputChannelNames();
    const auto custom = isCustom (engine, track);
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Audio output target"));

    auto set = [&engine, track] (int channel, OutputMode mode) {
        return [&engine, track, channel, mode] { engine.setTrackOutput (track, channel, mode); };
    };

    if (track == 0)
    {
        const auto first = names.size() >= 2 ? juce::String::fromUTF8 (names[0].c_str()) + " - " + juce::String::fromUTF8 (names[1].c_str())
                                             : tr ("first output pair");
        menu.addItem (first + "  " + tr ("(default)"), true, mt.outputChannel == outputDefault, set (outputDefault, OutputMode::stereo));
        menu.addItem (tr ("(none)"), true, mt.outputChannel == outputNone, set (outputNone, OutputMode::stereo));
    }
    else
        menu.addItem (tr ("(none) - through the Master only"), true, ! custom, set (outputDefault, OutputMode::stereo));

    if (custom && (track > 0 || mt.outputChannel >= 0))
        menu.addItem (label (engine, track), true, true, nullptr); // the current target (also when missing)

    // the optional direct outputs: stereo pairs, then every channel alone
    juce::PopupMenu direct;
    direct.addItem (track == 0 ? tr ("Optional: send the Master to other outputs of your audio interface")
                               : tr ("Optional: also send this insert straight to an output of your audio interface, e.g. a mono output"),
                    false, false, nullptr);
    if (names.empty())
        direct.addItem (tr ("No audio device is open"), false, false, nullptr);

    const auto targets = outputTargets (names);
    auto pairsHeader = false, monoHeader = false;
    for (const auto& t : targets)
    {
        const auto name = juce::String::fromUTF8 (t.name.c_str());
        if (t.mode == OutputMode::stereo)
        {
            if (! std::exchange (pairsHeader, true))
                direct.addSectionHeader (tr ("Stereo pair"));
            direct.addItem (name, true, mt.outputChannel == t.channel && mt.outputMode == OutputMode::stereo, set (t.channel, OutputMode::stereo));
            continue;
        }

        if (! std::exchange (monoHeader, true))
            direct.addSectionHeader (tr ("Mono (one output)"));
        const auto here = mt.outputChannel == t.channel && mt.outputMode != OutputMode::stereo;
        juce::PopupMenu modes;
        modes.addItem (tr ("Sum of L + R (mono)"), true, here && mt.outputMode == OutputMode::monoSum, set (t.channel, OutputMode::monoSum));
        modes.addItem (tr ("Left side only"), true, here && mt.outputMode == OutputMode::monoLeft, set (t.channel, OutputMode::monoLeft));
        modes.addItem (tr ("Right side only"), true, here && mt.outputMode == OutputMode::monoRight, set (t.channel, OutputMode::monoRight));
        direct.addSubMenu (name, modes, true, nullptr, here);
    }

    if (! names.empty() && names.size() <= 2)
        direct.addItem (tr ("This audio device has 2 outputs; an interface with more outputs lists all of them here"), false, false, nullptr);

    menu.addSeparator();
    menu.addSubMenu (track == 0 ? tr ("Hardware output") : tr ("Send to hardware output (optional)"), direct, true, nullptr, custom && mt.outputChannel >= 0);
    if (custom)
        menu.addItem (track == 0 ? tr ("Back to the default output") : tr ("Back to Master only"), set (outputDefault, OutputMode::stereo));

    menu.addSeparator();
    menu.addItem (tr ("Audio settings..."), [] {
        if (mixerinput::openAudioSettings)
            mixerinput::openAudioSettings();
    });

    auto options = juce::PopupMenu::Options();
    if (target != nullptr)
        options = options.withTargetComponent (target);
    if (! screenArea.isEmpty())
        options = options.withTargetScreenArea (screenArea);
    else if (target != nullptr)
        options = options.withMousePosition();
    menu.showMenuAsync (options);
}
} // namespace mixeroutput

juce::Rectangle<int> EffectsPanel::slotBounds (int slot) const
{
    return { 8, slotsTop + slot * slotHeight, getWidth() - 16, slotHeight - 3 };
}

namespace
{
// parameters shown as knobs: in the rhythmic envelope, only those of the points in use
int shownParams (const EffectSlot& slot)
{
    const auto all = (int) effectInfo (slot.kind).params.size();
    if (slot.kind == EffectKind::rhythmGain)
        return std::min (all, 2 + 3 * (int) std::lround (slot.params[1] * 12.0f));
    if (slot.kind == EffectKind::lynxTune) // the note switches are the keyboard strip above the knobs
        return std::min (all, (int) lynxtune::noteC);
    return all;
}
} // namespace

juce::Rectangle<int> EffectsPanel::knobBounds (int param) const
{
    return knobBoundsIn (param, bottomTop());
}

juce::Rectangle<int> EffectsPanel::knobBoundsIn (int param, int areaBottom) const
{
    auto top = slotsTop + maxEffectSlots * slotHeight + 40;
    // many parameters (Effector: 12): 4 columns and shorter rows to fit the height
    int count = 0;
    if (const auto* s = slots(); ! gateSelected() && s != nullptr && selectedSlot >= 0 && selectedSlot < (int) s->size())
        count = shownParams ((*s)[(std::size_t) selectedSlot]);
    const auto columns = count > 9 ? 4 : 3;
    const auto w = (getWidth() - 16) / columns;
    const auto rows = juce::jmax (1, (count + columns - 1) / columns);
    const auto rowHeight = count > 9 ? juce::jlimit (64, 86, (areaBottom - top - 4) / rows) : 86;

    // volume gate: the 32 steps are bars (gateStepsBounds); the knobs are mix/attack/release
    if (gateSelected())
    {
        if (param < 32)
            return {};
        param -= 32;
        top = gateStepsBounds().getBottom() + 22;
    }
    if (vocoderSelected()) // the band levels above the knobs
        top = bandLevelsBounds().getBottom() + 22;
    if (tuneSelected()) // LYNX Tune's note keyboard above the knobs
        top = tuneKeysBounds().getBottom() + 22;

    return { 8 + (param % columns) * w, top + (param / columns) * rowHeight, w, rowHeight - 4 };
}

int EffectsPanel::selectedEffectBottom() const
{
    const auto slotsBottom = slotsTop + maxEffectSlots * slotHeight;
    auto bottom = slotsBottom + 64; // the effect's title row
    const auto* s = slots();
    if (s == nullptr || selectedSlot < 0 || selectedSlot >= (int) s->size())
        return bottom;
    const auto& slot = (*s)[(std::size_t) selectedSlot];
    if (slot.kind == EffectKind::plugin || isSoundgoodizerApprox (slot))
        return juce::jmax (bottom, pluginButtonBounds().getBottom() + 34 + 40);
    if (slot.kind == EffectKind::parametricEq || slot.kind == EffectKind::facetEq)
        return juce::jmax (bottom, pluginButtonBounds().getBottom() + 10 + 120);
    if (slot.kind == EffectKind::parallel)
        return juce::jmax (bottom, knobBoundsIn (0, 0).getY() - 28 + 20 + 22 * (int) slot.branches.size());
    const auto count = gateSelected() ? 35 : shownParams (slot);
    for (int p = 0; p < count; ++p)
        if (const auto r = knobBoundsIn (p, 0); ! r.isEmpty()) // (tight grids at their shortest rows)
            bottom = juce::jmax (bottom, r.getY() + juce::jlimit (30, 44, r.getHeight() - 30) + 32); // knob, name, value
    return bottom;
}

bool EffectsPanel::gateSelected() const
{
    const auto* s = slots();
    return s != nullptr && selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::volumeGate;
}

juce::Rectangle<int> EffectsPanel::gateStepsBounds() const
{
    return { 10, slotsTop + maxEffectSlots * slotHeight + 40, getWidth() - 20, 90 };
}

void EffectsPanel::setGateStep (juce::Point<int> p)
{
    const auto area = gateStepsBounds();
    const auto step = juce::jlimit (0, 31, (int) ((p.x - area.getX()) * 32 / juce::jmax (1, area.getWidth())));
    const auto value = juce::jlimit (0.0f, 1.0f, (float) (area.getBottom() - p.y) / (float) area.getHeight());
    engine.editSong ([t = track, slot = selectedSlot, step, value] (Song& song) {
        song.mixer[(std::size_t) t].effects[(std::size_t) slot].params[(std::size_t) step] = value;
    });
}

bool EffectsPanel::vocoderSelected() const
{
    const auto* s = slots();
    return s != nullptr && selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::vocoder;
}

bool EffectsPanel::tuneSelected() const
{
    const auto* s = slots();
    return s != nullptr && selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::lynxTune;
}

juce::Rectangle<int> EffectsPanel::tuneKeysBounds() const
{
    return { 10, slotsTop + maxEffectSlots * slotHeight + 40, getWidth() - 20, 54 };
}

namespace
{
// LYNX Tune's keyboard strip: one octave, white keys in a row, black keys on top (C … B → key rectangles)
juce::Rectangle<float> tuneKeyRect (juce::Rectangle<float> area, int pc)
{
    static constexpr int whiteIndex[12] { 0, -1, 1, -1, 2, 3, -1, 4, -1, 5, -1, 6 };
    static constexpr float blackAt[12] { 0, 1.0f, 0, 2.0f, 0, 0, 4.0f, 0, 5.0f, 0, 6.0f, 0 };
    const auto w = area.getWidth() / 7.0f;
    if (whiteIndex[pc] >= 0)
        return { area.getX() + whiteIndex[pc] * w, area.getY(), w, area.getHeight() };
    return { area.getX() + blackAt[pc] * w - w * 0.3f, area.getY(), w * 0.6f, area.getHeight() * 0.58f };
}

int tuneKeyAt (juce::Rectangle<float> area, juce::Point<float> p)
{
    for (int pc = 0; pc < 12; ++pc) // black keys first: they lie on top
        if ((pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10) && tuneKeyRect (area, pc).contains (p))
            return pc;
    for (int pc = 0; pc < 12; ++pc)
        if (tuneKeyRect (area, pc).contains (p))
            return pc;
    return -1;
}
} // namespace

void EffectsPanel::toggleTuneNote (juce::Point<int> p)
{
    const auto pc = tuneKeyAt (tuneKeysBounds().toFloat(), p.toFloat());
    if (pc < 0)
        return;
    engine.editSong ([t = track, slot = selectedSlot, pc] (Song& song) {
        auto& v = song.mixer[(std::size_t) t].effects[(std::size_t) slot].params[(std::size_t) (lynxtune::noteC + pc)];
        v = v >= 0.5f ? 0.0f : 1.0f;
    });
}

juce::Rectangle<int> EffectsPanel::bandLevelsBounds() const
{
    return { 10, slotsTop + maxEffectSlots * slotHeight + 40, getWidth() - 20, 70 };
}

namespace
{
// the Fruity Vocoder's state for the slot's current band count (levels kept while the count matches, as FL does)
fvocoder::State vocoderState (const EffectSlot& slot)
{
    auto s = fvocoder::parse (slot.state).value_or (fvocoder::State());
    const auto bands = 4 << std::clamp ((int) std::lround (slot.params[0] * 5.0f), 0, 5);
    if (s.bands != bands)
    {
        s.bands = bands;
        s.levels.fill (1.0f);
    }
    s.order = 1 + std::clamp ((int) std::lround (slot.params[1] * 2.0f), 0, 2);
    s.listen = std::clamp ((int) std::lround (slot.params[2] * 2.0f), 0, 2);
    s.swap = slot.params[3] > 0.5f;
    for (int c = 0; c < fvocoder::controls; ++c)
        s.c[(std::size_t) c] = fvocoder::rawFromKnob (slot.params[(std::size_t) (4 + c)], c, s.c[(std::size_t) c]);
    return s;
}

constexpr float maxBandLevel = 2.0f; // the strip spans 0…2 (1 = unchanged)
} // namespace

void EffectsPanel::setBandLevel (juce::Point<int> p)
{
    const auto* s = slots();
    if (s == nullptr || ! vocoderSelected())
        return;
    const auto st = vocoderState ((*s)[(std::size_t) selectedSlot]);
    const auto area = bandLevelsBounds();
    const auto band = juce::jlimit (0, st.bands - 1, (int) ((p.x - area.getX()) * st.bands / juce::jmax (1, area.getWidth())));
    auto level = juce::jlimit (0.0f, maxBandLevel, maxBandLevel * (float) (area.getBottom() - p.y) / (float) area.getHeight());
    if (std::abs (level - 1.0f) < 0.04f)
        level = 1.0f; // snaps to the unchanged level
    engine.editSong ([t = track, slot = selectedSlot, band, level] (Song& song) {
        auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) slot];
        auto state = vocoderState (fx);
        state.levels[(std::size_t) band] = level;
        fx.state = fvocoder::serialise (state);
    });
}

void EffectsPanel::showBandLevelMenu()
{
    juce::PopupMenu menu;
    auto apply = [this] (float (*level) (int, int)) {
        engine.editSong ([t = track, slot = selectedSlot, level] (Song& song) {
            auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) slot];
            auto state = vocoderState (fx);
            for (int k = 0; k < state.bands; ++k)
                state.levels[(std::size_t) k] = level (k, state.bands);
            fx.state = fvocoder::serialise (state);
        });
    };
    menu.addItem (tr ("Reset band levels"), [apply] { apply ([] (int, int) { return 1.0f; }); });
    menu.addItem (tr ("Brighter (tilt up)"), [apply] { apply ([] (int k, int n) { return 0.5f + (float) k / (float) std::max (1, n - 1); }); });
    menu.addItem (tr ("Darker (tilt down)"), [apply] { apply ([] (int k, int n) { return 1.5f - (float) k / (float) std::max (1, n - 1); }); });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void EffectsPanel::showGatePatterns()
{
    // ready-made patterns (like Gross Beat's slots): value of step i of 32 (1/8 beat each)
    struct Pattern
    {
        const char* name;
        float (*level) (int);
    };
    static const Pattern patterns[] = {
        { TR_NOOP ("All open"), [] (int) { return 1.0f; } },
        { TR_NOOP ("1/2 beat gate"), [] (int i) { return (i % 4) < 2 ? 1.0f : 0.0f; } },
        { TR_NOOP ("1 beat gate"), [] (int i) { return (i % 8) < 4 ? 1.0f : 0.0f; } },
        { TR_NOOP ("Offbeat"), [] (int i) { return (i % 8) >= 4 ? 1.0f : 0.0f; } },
        { TR_NOOP ("Sidechain (ramp per beat)"), [] (int i) { return std::min (1.0f, 0.15f + (float) (i % 8) / 5.0f); } },
        { TR_NOOP ("Trance gate"), [] (int i) { return (i % 4) == 3 ? 0.0f : (i % 16) >= 12 ? 0.4f : 1.0f; } },
        { TR_NOOP ("Fade-in over the bar"), [] (int i) { return (float) (i + 1) / 32.0f; } },
        { TR_NOOP ("Fade-out over the bar"), [] (int i) { return 1.0f - (float) i / 32.0f; } },
    };

    juce::PopupMenu menu;
    for (const auto& p : patterns)
        menu.addItem (tr (p.name), [this, level = p.level] {
            engine.editSong ([t = track, slot = selectedSlot, level] (Song& song) {
                auto& params = song.mixer[(std::size_t) t].effects[(std::size_t) slot].params;
                for (int i = 0; i < 32; ++i)
                    params[(std::size_t) i] = level (i);
            });
        });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

int EffectsPanel::knobAt (juce::Point<int> p) const
{
    const auto* s = slots();

    if (s == nullptr || selectedSlot < 0 || selectedSlot >= (int) s->size())
        return -1;

    if ((*s)[(std::size_t) selectedSlot].kind == EffectKind::parametricEq || (*s)[(std::size_t) selectedSlot].kind == EffectKind::facetEq)
        return -1; // edited in its own window

    const auto count = shownParams ((*s)[(std::size_t) selectedSlot]);

    for (int i = gateSelected() ? 32 : 0; i < count; ++i)
        if (knobBounds (i).contains (p))
            return i;

    return -1;
}

void EffectsPanel::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("EffectsPanel paint", true, g.getClipBounds());
    using namespace theme;
    g.fillAll (colour::bg2);
    g.setColour (colour::border);
    g.drawVerticalLine (0, 0.0f, (float) getHeight());

    const auto* s = slots();

    if (s == nullptr)
        return;

    const auto& song = engine.getSong();
    g.setColour (colour::textPrimary);
    g.setFont (font (type::title, true));
    textcache::drawText (g, tr ("Effects: {0}", { juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str()) }), 10, 4,
                getWidth() - 70, panelHeader - 8, juce::Justification::centredLeft, true);

    // input row: arm switch (inserts) and the audio input source with its level (the Master has one too, as in FL;
    // REA round 143)
    if (track >= 0 && track < (int) song.mixer.size())
    {
        const auto& mt = song.mixer[(std::size_t) track];
        if (track > 0)
        {
            const auto armed = engine.isInsertArmed (track);
            const auto a = armBounds().toFloat();
            theme::fillRoundRect (g, a, radius::sm, armed ? colour::record.withAlpha (0.25f) : hoverInput == 1 ? colour::bg5 : colour::bg3);
            const auto dot = a.withSizeKeepingCentre (11.0f, 11.0f);
            g.setColour (armed ? colour::record : colour::bg0);
            g.fillEllipse (dot);
            g.setColour (armed ? colour::record.brighter (0.3f) : hoverInput == 1 ? colour::textSecondary : colour::borderStrong);
            g.drawEllipse (dot, 1.0f);
            if (mt.recordSafe)
            {
                g.setColour (colour::textDisabled);
                g.drawLine (dot.getX() + 1.0f, dot.getBottom() - 1.0f, dot.getRight() - 1.0f, dot.getY() + 1.0f, 1.2f);
            }
        }

        auto box = inputBoxBounds().toFloat();
        theme::fillRoundRect (g, box, radius::sm, hoverInput == 2 ? colour::bg4 : colour::bg3);
        g.setColour (hoverInput == 2 ? colour::borderStrong : colour::border);
        g.drawRoundedRectangle (box.reduced (0.5f), radius::sm, 1.0f);

        // the level, as a thin bar along the bottom of the box (left over right)
        if (mt.inputChannel >= 0)
        {
            const auto bar = box.reduced (4.0f, 0.0f).withTop (box.getBottom() - 5.0f).withHeight (3.0f);
            g.setColour (colour::bg0);
            g.fillRect (bar);
            for (int side = 0; side < 2; ++side)
            {
                const auto db = juce::Decibels::gainToDecibels (inputLevel[(std::size_t) side], -60.0f);
                const auto fill = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 60.0f);
                g.setColour (inputLevel[(std::size_t) side] >= 0.99f ? colour::record : colour::meterLow);
                g.fillRect (bar.withHeight (1.5f).translated (0.0f, side * 1.5f).withWidth (bar.getWidth() * fill));
            }
        }

        icons::draw (g, Icon::mic, box.withWidth (18.0f).reduced (2.0f).translated (3.0f, -1.0f), mt.inputChannel >= 0 ? colour::textPrimary : colour::textDisabled);
        const auto names = engine.getInputChannelNames();
        const auto label = mt.inputChannel >= 0 ? juce::String::fromUTF8 (inputSourceName (mt.inputChannel, mt.inputStereo, names).c_str())
                                                : tr ("No audio input");
        g.setColour (mt.inputChannel >= 0 ? colour::textPrimary : colour::textSecondary);
        g.setFont (font (type::body));
        textcache::drawText (g, label, box.withTrimmedLeft (24.0f).withTrimmedRight (18.0f).translated (0.0f, -1.0f).toNearestInt(), juce::Justification::centredLeft, true);
        icons::draw (g, Icon::chevronDown, box.removeFromRight (18.0f).reduced (4.0f).translated (-2.0f, -1.0f), colour::textSecondary);
    }

    // the insert's effects switch
    if (! s->empty())
    {
        const auto on = mixerops::anyEffectEnabled (song, track);
        const auto r = fxSwitchBounds().toFloat();
        theme::fillRoundRect (g, r, radius::sm, on ? colour::bg5 : colour::bg0);
        g.setColour (muteLed (! on));
        g.fillEllipse (r.withWidth (r.getHeight()).reduced (6.0f).translated (3.0f, 0.0f));
        g.setColour (on ? colour::textPrimary : colour::textDisabled);
        g.setFont (font (type::caption, true));
        textcache::drawText (g, "FX", r.withTrimmedLeft (r.getHeight()).toNearestInt(), juce::Justification::centredLeft);
    }

    g.setFont (font (type::body));

    // the 10 slots, as FL draws them: options arrow, name ("Slot N" when empty), mix knob, LED
    for (int i = 0; i < maxEffectSlots; ++i)
    {
        const auto r = slotBounds (i);
        const auto filled = i < (int) s->size();
        const auto isSelected = i == selectedSlot && filled;
        const auto slotHovered = i == hoverSlot && dragSlot < 0;
        theme::fillRoundRect (g, r.toFloat(), radius::sm, isSelected ? colour::bg5 : filled ? (slotHovered ? colour::bg4 : colour::bg3) : (slotHovered ? colour::bg3 : colour::bg2.brighter (0.03f)));
        g.setColour (slotHovered ? colour::borderStrong : colour::border);
        g.drawRoundedRectangle (r.toFloat().reduced (0.5f), radius::sm, 1.0f);

        if (isSelected)
        {
            theme::fillRoundRect (g, r.toFloat().withWidth (3.0f).reduced (0.0f, 3.0f), 1.5f, colour::accent);
        }

        {
            const auto a = arrowBounds (i).toFloat().withSizeKeepingCentre (5.0f, 8.0f).translated (1.0f, 0.0f);
            juce::Path tri;
            tri.addTriangle (a.getX(), a.getY(), a.getRight(), a.getCentreY(), a.getX(), a.getBottom());
            g.setColour (slotHovered && hoverArrow ? colour::textPrimary : colour::textSecondary.withAlpha (filled ? 1.0f : 0.6f));
            g.fillPath (tri);
        }

        const auto ledArea = ledBounds (i).toFloat().withSizeKeepingCentre (8.0f, 8.0f);
        const auto textArea = r.withTrimmedLeft (20).withTrimmedRight (46);

        if (filled)
        {
            const auto& slot = (*s)[(std::size_t) i];
            g.setColour (muteLed (! slot.enabled));
            g.fillEllipse (ledArea);
            if (slotHovered && hoverLed)
            {
                g.setColour (colour::textSecondary);
                g.drawEllipse (ledArea.expanded (2.0f), 1.0f);
            }
            g.setColour (slot.enabled ? colour::textPrimary : colour::textDisabled);
            // a LYNX substitute shows the plugin it stands in for, with a "LYNX" badge
            const auto substitute = slot.isSubstitute();
            auto name = quickpick::slotName (engine, slot); // (a plugin renamed in the Plugin database: its new name, round 165c)
            if (slot.sidechain > 0 && slot.sidechain < (int) engine.getSong().mixer.size())
                name << juce::String::fromUTF8 (" \xe2\x86\x90 ") << juce::String::fromUTF8 (engine.getSong().mixer[(std::size_t) slot.sidechain].name.c_str());
            auto text = textArea;
            if (substitute)
            {
                const auto badge = text.removeFromRight (34).withSizeKeepingCentre (32, 14).toFloat();
                const auto playing = engine.getSong().slotActive (slot); // off while the project's substitutes are bypassed
                theme::fillRoundRect (g, badge, 3.0f, colour::info.withAlpha (playing ? 0.25f : 0.12f));
                g.setColour (playing ? colour::info : colour::textDisabled);
                const auto keep = g.getCurrentFont();
                g.setFont (font (type::caption, true));
                textcache::drawText (g, "LYNX", badge, juce::Justification::centred);
                g.setFont (keep);
                g.setColour (slot.enabled ? colour::textPrimary : colour::textDisabled);
            }
            textcache::drawText (g, name, text, juce::Justification::centredLeft);

            // slot's dry/wet knob
            const auto mixActive = (hoverParam == mixParam && hoverSlot == i) || (dragParam == mixParam && selectedSlot == i);
            drawKnob (g, mixBounds (i).toFloat().withSizeKeepingCentre (18.0f, 18.0f), slot.mix, false,
                      slot.enabled ? (dragParam == mixParam && selectedSlot == i ? colour::accent : colour::info) : colour::textDisabled, mixActive);
        }
        else
        {
            // an empty slot keeps its controls in place (dimmed), as FL shows them
            g.setColour (muteLed (false).withAlpha (0.3f));
            g.fillEllipse (ledArea);
            drawKnob (g, mixBounds (i).toFloat().withSizeKeepingCentre (18.0f, 18.0f), 1.0f, false, colour::textDisabled.withAlpha (0.5f), false);
            g.setColour (slotHovered ? colour::textSecondary : colour::textDisabled);
            g.setFont (juce::Font (font (type::body)).italicised());
            textcache::drawText (g, tr ("Slot {0}", { juce::String (i + 1) }), textArea, juce::Justification::centredLeft);
            g.setFont (font (type::body));
        }
    }

    paintEq (g);
    g.reduceClipRegion (0, 0, getWidth(), bottomTop() - 6); // the selected effect's knobs stay above the EQ

    if (dragSlot >= 0 && dropSlot >= 0 && dropSlot != dragSlot) // where the dragged slot lands
    {
        const auto r = slotBounds (dropSlot);
        const auto y = dropSlot > dragSlot ? r.getBottom() + 1 : r.getY() - 2;
        g.setColour (colour::accent);
        g.fillRect (r.getX(), y, r.getWidth(), 2);
    }

    if (selectedSlot < 0 || selectedSlot >= (int) s->size())
        return;

    const auto& slot = (*s)[(std::size_t) selectedSlot];

    auto drawButton = [&g] (juce::Rectangle<int> button, const juce::String& text, Icon icon) {
        theme::fillRoundRect (g, button.toFloat(), radius::md, colour::bg4);
        const auto f = juce::Font (font (type::body));
        const auto textWidth = juce::GlyphArrangement::getStringWidth (f, text);
        auto content = button.toFloat().withSizeKeepingCentre (juce::jmin ((float) button.getWidth() - 12.0f, textWidth + 22.0f), (float) button.getHeight());
        icons::draw (g, icon, content.removeFromLeft (16.0f).withSizeKeepingCentre (16.0f, 16.0f), colour::textSecondary);
        content.removeFromLeft (6.0f);
        g.setColour (colour::textPrimary);
        g.setFont (f);
        textcache::drawFittedText (g, text, content.toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    };

    if (slot.kind == EffectKind::plugin)
    {
        const auto button = pluginButtonBounds();
        drawButton (button, tr ("Open plugin interface"), Icon::plugin);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawText (g, quickpick::slotName (engine, slot), button.translated (0, 34), juce::Justification::centred);
        return;
    }

    if (slot.kind == EffectKind::parametricEq)
    {
        const auto button = pluginButtonBounds();
        drawButton (button, tr ("Open parametric EQ"), Icon::settings);
        const auto curve = juce::Rectangle<int> (button.getX(), button.getBottom() + 10, button.getWidth(), 120).toFloat();
        drawParametricEqCurve (g, curve, slot.params, juce::jmax (8000.0, engine.getTransport().sampleRate.load()), slot.enabled);
        return;
    }

    if (isSoundgoodizerApprox (slot)) // approximated without FL's preset files: its page edits Preset and Amount
    {
        const auto button = pluginButtonBounds();
        drawButton (button, tr ("Open Soundgoodizer"), Icon::settings);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawFittedText (g, tr ("Approximation (FL Studio's Soundgoodizer presets were not found on this computer)"),
                          button.translated (0, 34).withHeight (40), juce::Justification::centredTop, 2);
        return;
    }

    if (slot.kind == EffectKind::facetEq)
    {
        const auto button = pluginButtonBounds();
        drawButton (button, tr ("Open Facet EQ"), Icon::settings);
        const auto curve = juce::Rectangle<int> (button.getX(), button.getBottom() + 10, button.getWidth(), 120).toFloat();
        drawFacetEqCurve (g, curve, slot.params, juce::jmax (8000.0, engine.getTransport().sampleRate.load()), slot.enabled);
        return;
    }

    if (slot.kind == EffectKind::parallel) // container: list the branches
    {
        auto y = knobBounds (0).getY() - 28;
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        textcache::drawText (g, juce::String::fromUTF8 (effectInfo (slot.kind).name), 10, y, getWidth() - 20, 20, juce::Justification::centredLeft);
        g.setFont (font (type::small));
        for (std::size_t b = 0; b < slot.branches.size(); ++b)
        {
            y += 22;
            juce::String chain;
            if (slot.branches[b].chain.empty())
                chain = tr ("dry");
            for (std::size_t c = 0; c < slot.branches[b].chain.size(); ++c)
                chain += (c > 0 ? " -> " : "") + juce::String::fromUTF8 (effectInfo (slot.branches[b].chain[c].kind).name);
            const auto line = tr ("Branch {0} (x{1}): {2}", { juce::String ((int) b + 1), juce::String (slot.branches[b].gain, 2), chain });
            g.setColour (colour::textSecondary);
            textcache::drawText (g, line, 14, y, getWidth() - 28, 20, juce::Justification::centredLeft, true);
        }
        return;
    }

    const auto& info = effectInfo (slot.kind);
    const auto gate = slot.kind == EffectKind::volumeGate;
    const auto vocoder = slot.kind == EffectKind::vocoder;
    g.setColour (colour::textPrimary);
    g.setFont (font (type::title, true));
    const auto tune = slot.kind == EffectKind::lynxTune;
    const auto editorButton = ! gate && ! vocoder && NativeEffectWindows::canOpen (slot.kind);
    const auto titleY = (gate ? gateStepsBounds().getY() : vocoder ? bandLevelsBounds().getY() : tune ? tuneKeysBounds().getY() : knobBounds (0).getY()) - 28;
    const auto titleRight = getWidth() - 10 - (editorButton ? editorButtonBounds().getWidth() + 6 : 0);
    textcache::drawText (g, juce::String::fromUTF8 (info.name), 10, titleY, titleRight - 10, 20, juce::Justification::centredLeft, true);
    if (slot.isSubstitute()) // which missing plugin this stands in for (the project keeps it)
    {
        const auto titleWidth = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), juce::String::fromUTF8 (info.name));
        g.setColour (colour::info);
        g.setFont (font (type::caption));
        const auto original = juce::String::fromUTF8 (substituteOriginalName (slot).c_str());
        textcache::drawText (g, engine.getSong().substitutesBypassed ? tr ("LYNX substitute for {0} (bypassed)", { original }) : tr ("LYNX substitute for {0}", { original }),
                    10 + titleWidth + 10, titleY,
                    juce::jmax (0, titleRight - 20 - titleWidth), 20, juce::Justification::centredLeft, true);
    }

    if (editorButton) // the effect's own window (FL's plugin window)
    {
        const auto b = editorButtonBounds().toFloat();
        const auto hover = getMouseXYRelative().toFloat();
        theme::fillRoundRect (g, b, radius::md, b.contains (hover) ? colour::bg5 : colour::bg4);
        icons::draw (g, Icon::plugin, b.withWidth (22.0f).withSizeKeepingCentre (13.0f, 13.0f).translated (4.0f, 0.0f), colour::textSecondary);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::small));
        textcache::drawFittedText (g, tr ("Editor"), b.withTrimmedLeft (24.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
    }

    if (tune) // the scale's notes on one octave; a note switched off is never a target
    {
        const auto area = tuneKeysBounds().toFloat();
        const auto scaleMask = vocal::scaleMask ((vocal::Scale) lynxtune::scaleIndex (slot.params[lynxtune::scale]), lynxtune::keyIndex (slot.params[lynxtune::key]), 0xfff);
        const auto custom = lynxtune::scaleIndex (slot.params[lynxtune::scale]) == (int) vocal::Scale::custom;
        for (const auto black : { false, true })
            for (int pc = 0; pc < 12; ++pc)
            {
                const auto isBlack = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
                if (isBlack != black)
                    continue;
                const auto r = tuneKeyRect (area, pc).reduced (1.0f);
                const auto on = slot.params[(std::size_t) (lynxtune::noteC + pc)] >= 0.5f;
                const auto inScale = custom || (scaleMask & (1u << pc)) != 0;
                auto fill = isBlack ? colour::bg0 : colour::bg2;
                if (inScale && on)
                    fill = (slot.enabled ? colour::accent : colour::textDisabled).withAlpha (isBlack ? 0.85f : 0.65f);
                theme::fillRoundRect (g, r, 2.0f, fill);
                g.setColour (colour::gridBar);
                g.drawRoundedRectangle (r, 2.0f, 1.0f);
                if (inScale && ! on) // in the scale but switched off
                {
                    g.setColour (colour::textSecondary);
                    g.drawLine (r.getX() + 3.0f, r.getBottom() - 3.0f, r.getRight() - 3.0f, r.getBottom() - 14.0f, 1.2f);
                }
                if (! isBlack)
                {
                    static const char* names[12] { "C", "", "D", "", "E", "F", "", "G", "", "A", "", "B" };
                    g.setColour (inScale && on ? colour::textPrimary : colour::textSecondary);
                    g.setFont (font (type::caption));
                    textcache::drawText (g, names[pc], r.withTrimmedTop (r.getHeight() - 16.0f), juce::Justification::centred);
                }
            }
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawFittedText (g, tr ("Target notes: click a key to switch it off or on"), tuneKeysBounds().withY (tuneKeysBounds().getBottom() + 2).withHeight (14),
                          juce::Justification::centredLeft, 1, 0.8f);
    }

    if (vocoder) // the analysis bands' levels, low to high (FL's band level strip)
    {
        const auto st = vocoderState (slot);
        const auto area = bandLevelsBounds().toFloat();
        theme::fillRoundRect (g, area, radius::sm, colour::bg0);
        const auto w = area.getWidth() / (float) st.bands;
        const auto unity = area.getBottom() - area.getHeight() / maxBandLevel;
        for (int k = 0; k < st.bands; ++k)
        {
            const auto h = area.getHeight() * juce::jlimit (0.0f, 1.0f, st.levels[(std::size_t) k] / maxBandLevel);
            g.setColour ((slot.enabled ? colour::accent : colour::textDisabled).withAlpha (k % 2 == 0 ? 1.0f : 0.8f));
            g.fillRect (area.getX() + (float) k * w + (w > 4.0f ? 1.0f : 0.0f), area.getBottom() - h, juce::jmax (1.0f, w - (w > 4.0f ? 2.0f : 0.0f)), h);
        }
        g.setColour (colour::gridBar);
        g.drawHorizontalLine ((int) unity, area.getX(), area.getRight());
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawFittedText (g, tr ("Band levels: draw them; right-click = presets"), area.withY (area.getBottom() + 2).withHeight (14).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);
    }

    if (gate) // 32 steps = 1 bar of 4 beats
    {
        const auto area = gateStepsBounds().toFloat();
        theme::fillRoundRect (g, area, radius::sm, colour::bg0);
        const auto w = area.getWidth() / 32.0f;
        for (int i = 0; i < 32; ++i)
        {
            const auto h = area.getHeight() * juce::jlimit (0.0f, 1.0f, slot.params[(std::size_t) i]);
            g.setColour ((slot.enabled ? colour::accent : colour::textDisabled).withAlpha ((i / 8) % 2 == 0 ? 1.0f : 0.7f));
            g.fillRect (area.getX() + i * w + 1.0f, area.getBottom() - h, w - 2.0f, h);
        }
        g.setColour (colour::gridBar);
        for (int beat = 1; beat < 4; ++beat)
            g.drawVerticalLine ((int) (area.getX() + beat * 8 * w), area.getY(), area.getBottom());
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawFittedText (g, tr ("1 bar: draw the steps; right-click = patterns"), area.withY (area.getBottom() + 2).withHeight (14).toNearestInt(),
                          juce::Justification::centredLeft, 1, 0.8f);
    }

    for (int p = gate ? 32 : 0; p < shownParams (slot); ++p)
    {
        const auto r = knobBounds (p);
        const auto value = slot.params[(std::size_t) p];
        // 38 px knob; in tight grids (Effector) it shrinks to fit name and value below
        const auto knobHeight = juce::jlimit (30, 44, r.getHeight() - 30);
        drawKnob (g, r.withHeight (knobHeight).toFloat(), value, false, slot.enabled ? colour::knobArc : colour::textDisabled,
                  dragParam == p || (hoverParam == p && hoverSlot < 0));

        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        textcache::drawFittedText (g, juce::String::fromUTF8 (info.params[(std::size_t) p].name), r.getX(), r.getY() + knobHeight + 2, r.getWidth(), 14,
                          juce::Justification::centred, 1, 0.8f);
        g.setColour (colour::textPrimary);
        textcache::drawText (g, juce::String::fromUTF8 (formatEffectParam (slot.kind, p, value, song.bpm, slot.params).c_str()), r.getX(), r.getY() + knobHeight + 16,
                    r.getWidth(), 14, juce::Justification::centred);
    }
}

void EffectsPanel::mouseDown (const juce::MouseEvent& e)
{
    const auto* s = slots();

    if (s == nullptr)
        return;

    if (eqFoldBounds().contains (e.getPosition()) && e.mods.isLeftButtonDown())
    {
        eqFoldChoice = eqFolded ? 1 : 2;
        refit();
        repaint();
        return;
    }

    // the built-in EQ and the output target (bottom section)
    if (outputBounds().contains (e.getPosition()))
    {
        showOutputMenu();
        return;
    }

    if (const auto eqControl = eqControlAt (e.getPosition()); eqControl != 0)
    {
        if (isEqPoint (eqControl))
        {
            dragParam = eqControl;
            setEqFromGraph (eqPointBase - eqControl, e.getPosition());
            return;
        }
        if (e.mods.isPopupMenu())
            ui::showValueMenu (*this, &engine, specFor (eqControl, -1), valueOf (eqControl, -1), [this, eqControl] (double v) { setValue (eqControl, -1, v); });
        else if (ui::isResetClick (e))
            setValue (eqControl, -1, specFor (eqControl, -1).defaultValue);
        else
        {
            dragParam = eqControl;
            drag.begin (e, valueOf (eqControl, -1));
            showTip (eqControl, -1, false);
        }
        return;
    }

    if (track > 0 && armBounds().contains (e.getPosition()))
    {
        if (e.mods.isPopupMenu())
            mixerinput::showMenu (engine, track, this);
        else
            mixerinput::toggleArm (engine, track, this);
        repaint();
        return;
    }

    if (track >= 0 && inputBoxBounds().contains (e.getPosition()))
    {
        mixerinput::showMenu (engine, track, this);
        return;
    }

    if (! s->empty() && fxSwitchBounds().contains (e.getPosition()))
    {
        if (e.mods.isPopupMenu())
            toggleAllWindows();
        else
        {
            const auto on = ! mixerops::anyEffectEnabled (engine.getSong(), track);
            engine.editSong ([t = track, on] (Song& song) { mixerops::setAllEffectsEnabled (song, t, on); });
        }
        return;
    }

    for (int i = 0; i < maxEffectSlots; ++i)
    {
        const auto r = slotBounds (i);

        if (! r.contains (e.getPosition()))
            continue;

        if (i >= (int) s->size())
        {
            showAddMenu (i);
            return;
        }

        if (arrowBounds (i).contains (e.getPosition())) // FL's slot options arrow
        {
            selectedSlot = i;
            repaint();
            showSlotMenu (i);
            return;
        }

        selectedSlot = i;
        repaint();

        if (mixBounds (i).contains (e.getPosition()))
        {
            if (e.mods.isPopupMenu())
                ui::showValueMenu (*this, &engine, specFor (mixParam, i), valueOf (mixParam, i), [this, i] (double v) { setValue (mixParam, i, v); });
            else if (ui::isResetClick (e))
            {
                setValue (mixParam, i, 1.0);
                showTip (mixParam, i, true);
            }
            else
            {
                dragParam = mixParam;
                dragStart = (*s)[(std::size_t) i].mix;
                drag.begin (e, dragStart);
                showTip (mixParam, i, false);
            }
            return;
        }

        if (e.mods.isPopupMenu())
            showSlotMenu (i);
        else if (ledBounds (i).contains (e.getPosition())) // LED: bypass
            engine.editSong ([t = track, i] (Song& song) {
                auto& slot = song.mixer[(std::size_t) t].effects[(std::size_t) i];
                slot.enabled = ! slot.enabled;
            });
        else
            dragSlot = i; // dragging the name reorders the slot
        return;
    }

    if (selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::plugin
        && pluginButtonBounds().contains (e.getPosition()))
    {
        openPluginWindow (selectedSlot);
        return;
    }

    if (selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::parametricEq
        && pluginButtonBounds().withHeight (170).contains (e.getPosition()))
    {
        ParametricEqWindows::open (engine, track, selectedSlot);
        return;
    }

    if (selectedSlot >= 0 && selectedSlot < (int) s->size() && isSoundgoodizerApprox ((*s)[(std::size_t) selectedSlot])
        && pluginButtonBounds().contains (e.getPosition()))
    {
        NativeEffectWindows::open (engine, track, selectedSlot);
        return;
    }

    if (selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind == EffectKind::facetEq
        && pluginButtonBounds().withHeight (170).contains (e.getPosition()))
    {
        FacetEqWindows::open (engine, track, selectedSlot);
        return;
    }

    if (selectedSlot >= 0 && selectedSlot < (int) s->size() && (*s)[(std::size_t) selectedSlot].kind != EffectKind::volumeGate
        && ! vocoderSelected() && NativeEffectWindows::canOpen ((*s)[(std::size_t) selectedSlot].kind)
        && editorButtonBounds().contains (e.getPosition()))
    {
        openEditor (selectedSlot);
        return;
    }

    if (vocoderSelected() && bandLevelsBounds().contains (e.getPosition()))
    {
        if (e.mods.isPopupMenu())
            showBandLevelMenu();
        else
        {
            dragParam = bandLevelParam;
            setBandLevel (e.getPosition());
        }
        return;
    }

    if (tuneSelected() && tuneKeysBounds().contains (e.getPosition()))
    {
        toggleTuneNote (e.getPosition());
        return;
    }

    if (gateSelected() && gateStepsBounds().contains (e.getPosition()))
    {
        if (e.mods.isPopupMenu())
            showGatePatterns();
        else
        {
            dragParam = gateStepParam;
            setGateStep (e.getPosition());
        }
        return;
    }

    dragParam = knobAt (e.getPosition());

    if (dragParam >= 0 && e.mods.isPopupMenu())
    {
        const auto p = dragParam;
        ui::showValueMenu (*this, &engine, specFor (p, selectedSlot), valueOf (p, selectedSlot),
                           [this, p, slot = selectedSlot] (double v) { setValue (p, slot, v); });
        dragParam = -1;
        return;
    }

    if (dragParam >= 0 && ui::isResetClick (e))
    {
        setValue (dragParam, selectedSlot, specFor (dragParam, selectedSlot).defaultValue);
        showTip (dragParam, selectedSlot, true);
        dragParam = -1;
        return;
    }

    if (dragParam >= 0)
    {
        dragStart = (*s)[(std::size_t) selectedSlot].params[(std::size_t) dragParam];
        drag.begin (e, dragStart);
        showTip (dragParam, selectedSlot, false);
    }
}

ui::ValueSpec EffectsPanel::specFor (int param, int slot) const
{
    const auto& song = engine.getSong();
    ui::ValueSpec spec;
    spec.pixelsForRange = 150.0;

    if (isEqParam (param)) // the built-in EQ (stored 0..1 here: gain ±18 dB, frequency 10 Hz..16 kHz log, width)
    {
        const auto band = (eqParamBase - param) / 3, what = (eqParamBase - param) % 3;
        const juce::String whatName = what == 0 ? tr ("level") : what == 1 ? tr ("frequency") : tr ("width");
        spec.name = juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str()) + " - " + tr ("EQ band {0} ({1})", { juce::String (band + 1), tr (eqBandNames[band]) })
                    + " " + whatName;
        TrackEq defaults;
        spec.defaultValue = eqToValue (defaults, band, what);
        spec.toText = [band, what] (double v) {
            TrackEq eq;
            eqFromValue (eq, band, what, (float) v);
            const auto b = (std::size_t) band;
            return what == 0 ? juce::String (eq.gainDb[b], 1) + " dB"
                   : what == 1 ? (eq.hz[b] >= 1000.0f ? juce::String (eq.hz[b] / 1000.0f, 2) + " kHz" : juce::String ((int) eq.hz[b]) + " Hz")
                               : juce::String (eq.width[b], 2);
        };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        return spec;
    }

    if (const auto* s = slots(); s == nullptr || slot < 0 || slot >= (int) s->size())
        return spec;

    const auto& fx = (*slots())[(std::size_t) slot];
    const auto& info = effectInfo (fx.kind);
    const auto trackName = juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str());

    if (param == mixParam)
    {
        spec = ui::percentSpec (trackName + " - " + utf8 (info.name) + " - " + tr ("Mix"), 1.0);
        spec.target = AutomationTarget { AutomationTargetKind::effectMix, track, slot };
        return spec;
    }

    if (param < 0 || param >= (int) info.params.size())
        return spec;

    spec.name = trackName + " - " + utf8 (info.name) + " - " + utf8 (info.params[(std::size_t) param].name);
    spec.defaultValue = info.params[(std::size_t) param].defaultValue;
    spec.toText = [kind = fx.kind, param, bpm = song.bpm, context = fx.params] (double v) { return utf8 (formatEffectParam (kind, param, (float) v, bpm, context).c_str()); };
    spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
    spec.target = AutomationTarget { AutomationTargetKind::effectParam, track, slot, param };
    return spec;
}

double EffectsPanel::valueOf (int param, int slot) const
{
    if (isEqParam (param))
        return track >= 0 && track < (int) engine.getSong().mixer.size()
                   ? eqToValue (engine.getSong().mixer[(std::size_t) track].eq, (eqParamBase - param) / 3, (eqParamBase - param) % 3)
                   : 0.0;

    const auto* s = slots();

    if (s == nullptr || slot < 0 || slot >= (int) s->size())
        return 0.0;

    const auto& fx = (*s)[(std::size_t) slot];
    return param == mixParam ? fx.mix : (param >= 0 && param < (int) fx.params.size() ? fx.params[(std::size_t) param] : 0.0f);
}

void EffectsPanel::setValue (int param, int slot, double value)
{
    if (isEqParam (param))
    {
        engine.editSong ([t = track, band = (eqParamBase - param) / 3, what = (eqParamBase - param) % 3, v = (float) value] (Song& song) {
            if (t < (int) song.mixer.size())
                eqFromValue (song.mixer[(std::size_t) t].eq, band, what, v);
        });
        return;
    }

    const auto* s = slots();

    if (s == nullptr || slot < 0 || slot >= (int) s->size())
        return;

    if (param == mixParam)
        engine.editSong ([t = track, slot, v = (float) value] (Song& song) { song.mixer[(std::size_t) t].effects[(std::size_t) slot].mix = v; });
    else if (param >= 0)
        engine.editSong ([t = track, slot, param, v = (float) value] (Song& song) {
            song.mixer[(std::size_t) t].effects[(std::size_t) slot].params[(std::size_t) param] = v;
        });
}

int EffectsPanel::mixKnobAt (juce::Point<int> p) const
{
    for (int i = 0; slots() != nullptr && i < (int) slots()->size(); ++i)
        if (mixBounds (i).contains (p))
            return i;

    return -1;
}

void EffectsPanel::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (auto eqControl = eqControlAt (e.getPosition()); eqControl != 0)
    {
        if (isEqPoint (eqControl)) // the wheel over a handle changes the band's width
            eqControl = eqParamBase - (eqPointBase - eqControl) * 3 - 2;
        if (const auto v = ui::wheelValue (valueOf (eqControl, -1), e, wheel, specFor (eqControl, -1)))
        {
            setValue (eqControl, -1, *v);
            showTip (eqControl, -1, true);
        }
        return;
    }

    if (const auto slot = mixKnobAt (e.getPosition()); slot >= 0)
    {
        if (const auto v = ui::wheelValue (valueOf (mixParam, slot), e, wheel, specFor (mixParam, slot)))
        {
            setValue (mixParam, slot, *v);
            showTip (mixParam, slot, true);
        }
        return;
    }

    if (const auto p = knobAt (e.getPosition()); p >= 0)
    {
        if (const auto v = ui::wheelValue (valueOf (p, selectedSlot), e, wheel, specFor (p, selectedSlot)))
        {
            setValue (p, selectedSlot, *v);
            showTip (p, selectedSlot, true);
        }
        return;
    }

    // the wheel over a slot's name moves it up or down
    if (const auto slot = slotAt (e.getPosition()); slot >= 0)
    {
        const auto to = slot + ((wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) > 0.0f ? -1 : 1);
        bool moved = false;
        engine.editSong ([t = track, slot, to, &moved] (Song& song) { moved = mixerops::moveEffectSlot (song, t, slot, to); });
        if (moved)
            selectedSlot = to;
        repaint();
        return;
    }

    juce::Component::mouseWheelMove (e, wheel);
}

ui::Hint EffectsPanel::hintAt (juce::Point<int> p)
{
    const auto gesture = ui::valueGestures();

    if (isEqParam (dragParam))
        return ui::valueHint (specFor (dragParam, -1), valueOf (dragParam, -1), gesture);

    if (const auto eqControl = eqControlAt (p); eqControl != 0)
    {
        if (isEqParam (eqControl))
            return ui::valueHint (specFor (eqControl, -1), valueOf (eqControl, -1), gesture);
        ui::Hint h;
        const auto band = eqPointBase - eqControl;
        h.text = tr ("EQ: [LMB] drag the band's frequency and level  [Wheel] width  [2xLMB] back to 0 dB");
        h.value = tr ("Band {0}", { juce::String (band + 1) }) + " - " + tr (eqBandNames[band]);
        return h;
    }

    if (slots() != nullptr && eqFoldBounds().contains (p))
    {
        ui::Hint h;
        h.text = eqFolded ? tr ("Insert EQ (folded): [LMB] unfold it") : tr ("Insert EQ: [LMB] fold it (it folds by itself when the mixer is short)");
        return h;
    }

    if (outputBounds().contains (p))
    {
        ui::Hint h;
        h.text = track == 0 ? tr ("Audio output target: the outputs of your audio device the Master plays on  [LMB] choose")
                            : tr ("Audio output target: through the Master by default; optional: also straight to an output of your audio interface (a mono output too)  [LMB] choose");
        h.value = outputName();
        h.hasMenu = true;
        return h;
    }

    if (dragParam == mixParam || (dragParam >= 0 && drag.isActive()))
        return ui::valueHint (specFor (dragParam, dragParam == mixParam ? juce::jmax (0, selectedSlot) : selectedSlot),
                              valueOf (dragParam, dragParam == mixParam ? juce::jmax (0, selectedSlot) : selectedSlot), gesture);

    if (const auto slot = mixKnobAt (p); slot >= 0)
        return ui::valueHint (specFor (mixParam, slot), valueOf (mixParam, slot), gesture);

    if (const auto param = knobAt (p); param >= 0)
        return ui::valueHint (specFor (param, selectedSlot), valueOf (param, selectedSlot), gesture);

    ui::Hint h;
    if (const auto* s = slots(); s != nullptr && selectedSlot >= 0 && selectedSlot < (int) s->size()
                                 && NativeEffectWindows::canOpen ((*s)[(std::size_t) selectedSlot].kind) && editorButtonBounds().contains (p))
    {
        h.text = tr ("Open the effect's window (its full controls, as in FL); double-clicking the slot does the same");
        return h;
    }
    if (track > 0 && armBounds().contains (p))
    {
        h.text = tr ("Record arm: Rec + Play records this insert's audio input into a Playlist clip  [RMB] input and monitoring");
        h.hasMenu = true;
        return h;
    }
    if (track >= 0 && inputBoxBounds().contains (p))
    {
        h.text = track == 0 ? tr ("Audio input source: the microphone or interface input the Master plays (through its effects)  [LMB] choose")
                            : tr ("Audio input source: the microphone or interface input this insert plays and records  [LMB] choose");
        h.hasMenu = true;
        const auto& mt = engine.getSong().mixer[(std::size_t) track];
        if (mt.inputChannel >= 0)
            h.value = juce::String::fromUTF8 (inputSourceName (mt.inputChannel, mt.inputStereo, engine.getInputChannelNames()).c_str());
        return h;
    }
    if (fxSwitchBounds().contains (p) && slots() != nullptr && ! slots()->empty())
    {
        h.text = tr ("All effects of this insert on/off; right-click opens or closes their plugin windows");
        h.hasMenu = true;
        return h;
    }

    for (int i = 0; i < maxEffectSlots; ++i)
        if (slotBounds (i).contains (p))
        {
            const auto used = slots() != nullptr && i < (int) slots()->size();
            h.text = used ? (arrowBounds (i).contains (p) ? tr ("Effect slot options: replace, move, delete...")
                                                          : tr ("Effect slot: click edits, double-click opens its window, the LED bypasses, drag or wheel reorders, right-click: options"))
                          : tr ("Empty slot: click to add an effect");
            h.hasMenu = used;
        }

    return h;
}

void EffectsPanel::mouseDrag (const juce::MouseEvent& e)
{
    if (dragSlot >= 0)
    {
        if (e.getDistanceFromDragStart() > 4)
        {
            const auto count = slots() != nullptr ? (int) slots()->size() : 0;
            const auto target = juce::jlimit (0, juce::jmax (0, count - 1), (e.y - slotsTop) / slotHeight);
            if (target != dropSlot)
            {
                dropSlot = target;
                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
                repaint();
            }
        }
        return;
    }

    if (isEqPoint (dragParam))
    {
        setEqFromGraph (eqPointBase - dragParam, e.getPosition());
        return;
    }

    if (isEqParam (dragParam))
    {
        setValue (dragParam, -1, drag.update (e, specFor (dragParam, -1)));
        showTip (dragParam, -1, false);
        return;
    }

    if (dragParam == gateStepParam)
    {
        setGateStep (e.getPosition());
        return;
    }

    if (dragParam == bandLevelParam)
    {
        setBandLevel (e.getPosition());
        return;
    }

    if (dragParam == mixParam)
    {
        setValue (mixParam, selectedSlot, drag.update (e, specFor (mixParam, selectedSlot)));
        showTip (mixParam, selectedSlot, false);
        return;
    }

    if (dragParam < 0)
        return;

    setValue (dragParam, selectedSlot, drag.update (e, specFor (dragParam, selectedSlot)));
    showTip (dragParam, selectedSlot, false);
}

void EffectsPanel::showTip (int param, int slot, bool transient)
{
    if (isEqParam (param))
    {
        const auto band = (eqParamBase - param) / 3, what = (eqParamBase - param) % 3;
        const auto area = what == 0 ? eqFaderBounds (band) : eqKnobBounds (band, what);
        const auto spec = specFor (param, -1);
        ui::postHint (ui::valueHint (spec, valueOf (param, -1), ui::valueGestures()));
        if (transient)
            ui::flashValueTip (*this, area, spec.text (valueOf (param, -1)));
        else
            ui::showValueTip (*this, area, spec.text (valueOf (param, -1)));
        return;
    }

    const auto* s = slots();

    if (s == nullptr || slot < 0 || slot >= (int) s->size())
        return;

    const auto area = param == mixParam ? mixBounds (slot) : knobBounds (param).withHeight (juce::jlimit (30, 44, knobBounds (param).getHeight() - 30));
    const auto text = specFor (param, slot).text (valueOf (param, slot));
    ui::postHint (ui::valueHint (specFor (param, slot), valueOf (param, slot), ui::valueGestures()));

    if (transient)
        ui::flashValueTip (*this, area, text);
    else
        ui::showValueTip (*this, area, text);
}

void EffectsPanel::mouseMove (const juce::MouseEvent& e)
{
    const auto p = e.getPosition();
    auto slot = slotAt (p);
    auto param = -1;
    auto led = false;

    for (int i = 0; i < maxEffectSlots; ++i)
        if (slotBounds (i).contains (p))
            slot = i;

    if (const auto m = mixKnobAt (p); m >= 0)
    {
        slot = m;
        param = mixParam;
    }
    else if (slot < 0)
        param = knobAt (p);
    else
        led = p.x < slotBounds (slot).getX() + 22;

    const auto input = track > 0 && armBounds().contains (p) ? 1 : track >= 0 && inputBoxBounds().contains (p) ? 2 : 0;
    const auto eq = eqControlAt (p);
    const auto output = outputBounds().contains (p);
    auto arrow = false;
    for (int i = 0; i < maxEffectSlots; ++i)
        if (arrowBounds (i).contains (p))
        {
            slot = i;
            arrow = true;
        }
    if (slot >= 0)
        led = ledBounds (slot).contains (p);

    if (slot != hoverSlot || param != hoverParam || led != hoverLed || input != hoverInput || eq != hoverEq || output != hoverOutput || arrow != hoverArrow)
    {
        hoverEq = eq;
        hoverOutput = output;
        hoverArrow = arrow;
        hoverInput = input;
        hoverSlot = slot;
        hoverParam = param;
        hoverLed = led;
        repaint();
    }
}

void EffectsPanel::mouseExit (const juce::MouseEvent&)
{
    if (hoverSlot >= 0 || hoverParam != -1 || hoverInput != 0 || hoverEq != 0 || hoverOutput || hoverArrow)
    {
        hoverEq = 0;
        hoverOutput = hoverArrow = false;
        hoverInput = 0;
        hoverSlot = hoverParam = -1;
        hoverLed = false;
        repaint();
    }
}

void EffectsPanel::mouseUp (const juce::MouseEvent&)
{
    if (dragSlot >= 0 && dropSlot >= 0 && dropSlot != dragSlot)
    {
        bool moved = false;
        engine.editSong ([t = track, from = dragSlot, to = dropSlot, &moved] (Song& song) { moved = mixerops::moveEffectSlot (song, t, from, to); });
        if (moved)
            selectedSlot = dropSlot;
    }

    if (dragSlot >= 0)
        setMouseCursor (juce::MouseCursor::NormalCursor);

    dragSlot = dropSlot = -1;
    repaint();
    dragParam = -1;
    drag.end();
    ui::hideValueTip();
    ui::clearPostedHint();
}

void EffectsPanel::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (const auto eqControl = eqControlAt (e.getPosition()); eqControl != 0)
    {
        if (isEqPoint (eqControl)) // the band back to 0 dB
            setValue (eqParamBase - (eqPointBase - eqControl) * 3, -1, specFor (eqParamBase, -1).defaultValue);
        else
            setValue (eqControl, -1, specFor (eqControl, -1).defaultValue);
        return;
    }

    for (int i = 0; slots() != nullptr && i < (int) slots()->size(); ++i)
        if (mixBounds (i).contains (e.getPosition()))
        {
            engine.editSong ([t = track, i] (Song& song) { song.mixer[(std::size_t) t].effects[(std::size_t) i].mix = 1.0f; });
            return;
        }
        else if (slotBounds (i).contains (e.getPosition()) && ! arrowBounds (i).contains (e.getPosition()) && ! ledBounds (i).contains (e.getPosition()))
        {
            openEditor (i); // double-click on the name: the effect's window (as clicking a slot in FL)
            return;
        }

    const auto p = knobAt (e.getPosition());

    if (p < 0)
        return;

    const auto kind = (*slots())[(std::size_t) selectedSlot].kind;
    const auto value = effectInfo (kind).params[(std::size_t) p].defaultValue;
    engine.editSong ([t = track, slot = selectedSlot, p, value] (Song& song) {
        song.mixer[(std::size_t) t].effects[(std::size_t) slot].params[(std::size_t) p] = value;
    });
}

bool EffectsPanel::isInterestedInDragSource (const SourceDetails& details)
{
    return (BrowserComponent::isBrowserDrag (details) && BrowserComponent::draggedFile (details).hasFileExtension ("fst"))
           || BrowserComponent::isEffectDrag (details);
}

void EffectsPanel::itemDropped (const SourceDetails& details)
{
    if (BrowserComponent::isEffectDrag (details))
    {
        if (auto fx = BrowserComponent::draggedEffect (details))
            dropEffect (std::move (*fx), details.localPosition);
        return;
    }
    dropPreset (BrowserComponent::draggedFile (details), details.localPosition);
}

bool EffectsPanel::isInterestedInFileDrag (const juce::StringArray& files)
{
    return files.size() == 1 && juce::File (files[0]).hasFileExtension ("fst");
}

void EffectsPanel::filesDropped (const juce::StringArray& files, int x, int y)
{
    if (! files.isEmpty())
        dropPreset (juce::File (files[0]), { x, y });
}

bool EffectsPanel::dropPreset (const juce::File& file, juce::Point<int> position)
{
    juce::MemoryBlock data;

    if (slots() == nullptr || ! file.loadFileAsData (data))
        return false;

    const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
    const auto fx = effectFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), engine.getSong().bpm);

    if (! fx)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Preset"),
                                                tr ("LYNX can't build an effect from this preset."));
        return false;
    }

    return dropEffect (*fx, position);
}

bool EffectsPanel::dropEffect (EffectSlot fx, juce::Point<int> position)
{
    if (slots() == nullptr)
        return false;

    auto target = -1;
    for (int i = 0; i < (int) slots()->size(); ++i)
        if (slotBounds (i).contains (position))
            target = i;

    if (target < 0 && (int) slots()->size() >= maxEffectSlots)
        return false;

    engine.editSong ([t = track, target, fx = std::move (fx)] (Song& song) {
        auto& effects = song.mixer[(std::size_t) t].effects;
        if (target >= 0 && target < (int) effects.size())
            effects[(std::size_t) target] = fx;
        else
            effects.push_back (fx);
    });
    selectedSlot = target >= 0 ? target : (int) slots()->size() - 1;
    repaint();
    return true;
}

juce::Rectangle<int> EffectsPanel::fxSwitchBounds() const
{
    return { getWidth() - 58, 6, 48, panelHeader - 12 };
}

int EffectsPanel::slotAt (juce::Point<int> p) const
{
    const auto* s = slots();
    for (int i = 0; s != nullptr && i < (int) s->size(); ++i)
        if (slotBounds (i).contains (p) && ! mixBounds (i).contains (p) && ! ledBounds (i).contains (p) && ! arrowBounds (i).contains (p))
            return i;
    return -1;
}

void EffectsPanel::toggleAllWindows()
{
    // opens every plugin effect window of the insert, or closes them all when one is open
    const auto* s = slots();
    if (s == nullptr)
        return;

    bool anyOpen = false;
    for (int i = 0; i < (int) s->size(); ++i)
        if (auto* p = dynamic_cast<PluginEffect*> (engine.getEffect (track, i).get()))
            anyOpen = anyOpen || PluginWindows::isOpen (p->getInstance());

    for (int i = 0; i < (int) s->size(); ++i)
        if (auto* p = dynamic_cast<PluginEffect*> (engine.getEffect (track, i).get()))
        {
            if (anyOpen)
                PluginWindows::close (p->getInstance());
            else
                openPluginWindow (i);
        }
}

juce::Rectangle<int> EffectsPanel::pluginButtonBounds() const
{
    return { 20, slotsTop + maxEffectSlots * slotHeight + 30, getWidth() - 40, 30 };
}

void EffectsPanel::openEditor (int slot)
{
    const auto* s = slots();
    if (s == nullptr || slot < 0 || slot >= (int) s->size())
        return;
    const auto kind = (*s)[(std::size_t) slot].kind;
    if (kind == EffectKind::plugin)
        openPluginWindow (slot);
    else if (kind == EffectKind::parametricEq)
        ParametricEqWindows::open (engine, track, slot);
    else if (NativeEffectWindows::canOpen ((*s)[(std::size_t) slot]))
        NativeEffectWindows::open (engine, track, slot);
}

juce::Rectangle<int> EffectsPanel::editorButtonBounds() const
{
    // on the title row of the selected effect's knobs
    return { getWidth() - 96, (tuneSelected() ? tuneKeysBounds().getY() : knobBounds (0).getY()) - 29, 86, 20 };
}

void EffectsPanel::openPluginWindow (int slot)
{
    auto effect = engine.getEffect (track, slot);

    if (auto* plugin = dynamic_cast<PluginEffect*> (effect.get()))
    {
        if (plugin->hasCrashed()) // (not called again: its window would call into it)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Plugin"),
                                                    tr ("This plugin crashed and was switched off. Save the project and open it again to reload it."));
            return;
        }
        const auto& s = engine.getSong().mixer[(std::size_t) track].effects[(std::size_t) slot];
        PluginWindows::open (effect, plugin->getInstance(), juce::String::fromUTF8 (s.plugin.name.c_str()),
                             { &engine, [t = track, slot] (Song& song) -> PluginRef* {
                                  if (t >= (int) song.mixer.size() || slot >= (int) song.mixer[(std::size_t) t].effects.size())
                                      return nullptr;
                                  return &song.mixer[(std::size_t) t].effects[(std::size_t) slot].plugin;
                              } });
        return;
    }

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Plugin"),
                                            tr ("This plugin could not be loaded (it is not installed or has not been scanned)."));
}

void EffectsPanel::showAddMenu (int)
{
    juce::PopupMenu menu;

    // FL's renamed plugins first (issue #11): "1_EQ" renamed in the Browser's Plugin database is added by typing 1
    auto picks = quickpick::renamedEffects (engine, [this] (EffectSlot fx) {
        const auto t = track;
        bool added = false;
        engine.editSong ([t, &fx, &added] (Song& song) {
            if (t < 0 || t >= (int) song.mixer.size())
                return;
            auto& effects = song.mixer[(std::size_t) t].effects;
            if ((int) effects.size() < maxEffectSlots)
            {
                effects.push_back (std::move (fx));
                added = true;
            }
        });
        if (! added)
            return;
        selectedSlot = (int) slots()->size() - 1;
        if ((*slots())[(std::size_t) selectedSlot].kind == EffectKind::plugin)
            openPluginWindow (selectedSlot);
        repaint();
    });
    quickpick::addPicks (menu, picks);

    for (auto kind : allEffects)
        menu.addItem (addableEffectName (kind), addableEffectAvailable (kind), false, [this, kind] {
            const auto made = newAddableEffect (kind, engine.getSong().bpm);
            if (! made)
                return;
            engine.editSong ([t = track, fx = *made] (Song& song) {
                auto& effects = song.mixer[(std::size_t) t].effects;
                if ((int) effects.size() < maxEffectSlots)
                    effects.push_back (fx);
            });
            selectedSlot = (int) slots()->size() - 1;
            repaint();
        });

    // LYNX equivalents of well-known third-party effects, with the same default preset a substitute gets
    juce::PopupMenu equivalents;
    for (const auto& entry : pluginSubstitutes())
        if (! entry.instrument)
            equivalents.addItem (juce::String::fromUTF8 (entry.product.c_str()) + juce::String::fromUTF8 (" \xe2\x86\x92 ") + juce::String::fromUTF8 (effectInfo (entry.effect).name),
                                 [this, entry] {
                                     engine.editSong ([t = track, entry] (Song& song) {
                                         auto& effects = song.mixer[(std::size_t) t].effects;
                                         if ((int) effects.size() < maxEffectSlots)
                                             effects.push_back (makeSubstituteSlot (entry));
                                     });
                                     selectedSlot = (int) slots()->size() - 1;
                                     repaint();
                                 });
    if (equivalents.getNumItems() > 0)
        menu.addSubMenu (tr ("LYNX equivalents"), equivalents);

    juce::PopupMenu plugins;
    addPluginItems (plugins, engine, false, [this] (PluginRef ref) {
        engine.editSong ([t = track, ref] (Song& song) {
            auto& effects = song.mixer[(std::size_t) t].effects;
            if ((int) effects.size() >= maxEffectSlots)
                return;
            EffectSlot slot;
            slot.kind = EffectKind::plugin;
            slot.plugin = ref;
            effects.push_back (slot);
        });
        selectedSlot = (int) slots()->size() - 1;
        openPluginWindow (selectedSlot);
        repaint();
    }, this);
    menu.addSeparator();
    menu.addSubMenu (tr ("Plugin (VST/VST3/CLAP)"), plugins);
    menu.addSeparator();
    menu.addItem (tr ("Audio editor (record this insert)..."), [this] { openAudioEditorForInsert (engine, track); });

    quickpick::showMenu (menu, juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), std::move (picks));
}

void EffectsPanel::showSlotMenu (int slot)
{
    juce::PopupMenu replace;

    for (auto kind : allEffects)
        replace.addItem (addableEffectName (kind), addableEffectAvailable (kind), false, [this, slot, kind] {
            if (const auto made = newAddableEffect (kind, engine.getSong().bpm))
                engine.editSong ([t = track, slot, fx = *made] (Song& song) { song.mixer[(std::size_t) t].effects[(std::size_t) slot] = fx; });
        });

    const auto count = (int) slots()->size();
    juce::PopupMenu menu;
    menu.addSubMenu (tr ("Replace with"), replace);
    menu.addItem (tr ("Audio editor (record this insert)..."), [this] { openAudioEditorForInsert (engine, track); });

    // FL presets (Plugin presets\Effects\<plugin>) that map to this effect
    {
        juce::PopupMenu presets;
        const auto roots = findFlStudioRoots();
        const auto base = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin presets/Effects");

        for (const auto& plugin : flPluginsForEffect ((*slots())[(std::size_t) slot].kind))
        {
            juce::PopupMenu sub;
            auto files = base.getChildFile (juce::String::fromUTF8 (plugin.c_str())).findChildFiles (juce::File::findFiles, true, "*.fst");
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; });

            for (const auto& f : files)
                sub.addItem (f.getFileNameWithoutExtension(), [this, slot, f] {
                    juce::MemoryBlock data;
                    if (! f.loadFileAsData (data))
                        return;
                    const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
                    if (auto fx = effectFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), engine.getSong().bpm))
                        engine.editSong ([t = track, slot, fx = *fx] (Song& song) {
                            auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) slot];
                            const auto enabled = target.enabled;
                            const auto mix = target.mix;
                            target = fx;
                            target.enabled = enabled;
                            target.mix = mix;
                        });
                });

            if (sub.getNumItems() > 0)
                presets.addSubMenu (juce::String::fromUTF8 (plugin.c_str()), sub);
        }

        if (presets.getNumItems() > 0)
            menu.addSubMenu (tr ("FL presets"), presets);
    }

    // side inputs: another track's output feeds the compressor's detector, or the vocoder's modulator/carrier
    const auto kind = (*slots())[(std::size_t) slot].kind;
    auto sideMenu = [&] (const juce::String& title, bool second) {
        juce::PopupMenu side;
        const auto& fx = (*slots())[(std::size_t) slot];
        const auto current = second ? fx.sidechain2 : fx.sidechain;
        auto assign = [this, slot, second] (int source) {
            engine.editSong ([tk = track, slot, second, source] (Song& s) {
                auto& target = s.mixer[(std::size_t) tk].effects[(std::size_t) slot];
                (second ? target.sidechain2 : target.sidechain) = source;
                if (target.kind == EffectKind::vocoder && source >= 0)
                    target.params[16] = 1.0f; // separate inputs (otherwise L = modulator, R = carrier)
            });
        };
        side.addItem (tr ("None"), true, current < 0, [assign] { assign (-1); });
        const auto& song = engine.getSong();
        for (int t = 1; t < (int) song.mixer.size(); ++t)
            if (t != track)
                side.addItem (juce::String (t) + " " + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str()), true, current == t,
                              [assign, t] { assign (t); });
        menu.addSubMenu (title, side);
    };
    if (kind == EffectKind::compressor || kind == EffectKind::fruityLimiter || kind == EffectKind::fruityCompressor || kind == EffectKind::facetEq)
        sideMenu (tr ("Sidechain from"), false);
    if (kind == EffectKind::vocoder || kind == EffectKind::vocodex)
    {
        sideMenu (tr ("Modulator from"), false);
        sideMenu (tr ("Carrier from"), true);
    }
    if (kind == EffectKind::newtone || kind == EffectKind::newtime)
    {
        // the sample the plugin plays in slave mode (REA_rodada73): FL's edits are kept in the state but not reproduced
        const auto& fx = (*slots())[(std::size_t) slot];
        const auto current = juce::File (juce::String::fromUTF8 (fx.samplePath.c_str()));
        juce::PopupMenu source;
        source.addItem (fx.samplePath.empty() ? tr ("No sample") : current.getFileName(), false, false, [] {});
        source.addItem (tr ("Choose a sample..."), [this, slot, current] {
            sampleChooser = std::make_unique<juce::FileChooser> (tr ("Source sample"), current.existsAsFile() ? current : juce::File(),
                                                                 "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
            sampleChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                        [this, slot] (const juce::FileChooser& fc) {
                                            const auto file = fc.getResult();
                                            if (file == juce::File())
                                                return;
                                            engine.editSong ([t = track, slot, path = file.getFullPathName().toStdString()] (Song& s) {
                                                if (t < (int) s.mixer.size() && slot < (int) s.mixer[(std::size_t) t].effects.size())
                                                    s.mixer[(std::size_t) t].effects[(std::size_t) slot].samplePath = path;
                                            });
                                        });
        });
        source.addItem (tr ("Remove the sample"), ! fx.samplePath.empty(), false, [this, slot] {
            engine.editSong ([t = track, slot] (Song& s) {
                if (t < (int) s.mixer.size() && slot < (int) s.mixer[(std::size_t) t].effects.size())
                    s.mixer[(std::size_t) t].effects[(std::size_t) slot].samplePath.clear();
            });
        });
        menu.addSubMenu (tr ("Source sample"), source);
    }
    {
        // A/B compare: keep a copy of the settings, then swap between it and the current ones
        const auto key = std::make_pair (track, slot);
        const auto hasSpare = spare.count (key) > 0 && spare[key].kind == (*slots())[(std::size_t) slot].kind;
        juce::PopupMenu ab;
        ab.addItem (tr ("Store the current settings as B"), [this, key, slot] {
            spare[key] = (*slots())[(std::size_t) slot];
        });
        ab.addItem (tr ("Swap with B"), hasSpare, false, [this, key, slot] {
            const auto stored = spare[key];
            spare[key] = (*slots())[(std::size_t) slot];
            engine.editSong ([t = track, slot, stored] (Song& song) {
                auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) slot];
                const auto enabled = target.enabled;
                target = stored;
                target.enabled = enabled;
            });
        });
        ab.addItem (tr ("Forget B"), hasSpare, false, [this, key] { spare.erase (key); });
        menu.addSubMenu (hasSpare ? tr ("A/B compare (B stored)") : tr ("A/B compare"), ab);
    }
    if ((*slots())[(std::size_t) slot].isSubstitute()) // A/B of every substitute of the project at once (round 113)
        menu.addItem (tr ("Bypass all LYNX substitutes of this project"), true, engine.getSong().substitutesBypassed, [this] {
            engine.editSong ([] (Song& song) { song.substitutesBypassed = ! song.substitutesBypassed; });
            repaint();
        });
    // FL's slot menu "CPU" section (REA round 139): Allow threaded processing (plugins), Smart disable
    menu.addSectionHeader (tr ("CPU"));
    if ((*slots())[(std::size_t) slot].kind == EffectKind::plugin)
        menu.addItem (tr ("Allow threaded processing"), true, (*slots())[(std::size_t) slot].plugin.threaded, [this, slot] {
            engine.editSong ([t = track, slot] (Song& song) {
                if (t < (int) song.mixer.size() && slot < (int) song.mixer[(std::size_t) t].effects.size())
                    song.mixer[(std::size_t) t].effects[(std::size_t) slot].plugin.threaded = ! song.mixer[(std::size_t) t].effects[(std::size_t) slot].plugin.threaded;
            });
        });
    // FL's per-plugin "Smart disable": skipped while its input and output are silent
    menu.addItem (tr ("Smart disable"), true, (*slots())[(std::size_t) slot].smartDisable, [this, slot] {
        engine.editSong ([t = track, slot] (Song& song) {
            if (t < (int) song.mixer.size() && slot < (int) song.mixer[(std::size_t) t].effects.size())
                song.mixer[(std::size_t) t].effects[(std::size_t) slot].smartDisable = ! song.mixer[(std::size_t) t].effects[(std::size_t) slot].smartDisable;
        });
    });
    menu.addSeparator();
    menu.addItem (tr ("Move up"), slot > 0, false, [this, slot] {
        engine.editSong ([t = track, slot] (Song& song) { song.swapEffectSlots (t, slot, slot - 1); });
        selectedSlot = slot - 1;
    });
    menu.addItem (tr ("Move down"), slot < count - 1, false, [this, slot] {
        engine.editSong ([t = track, slot] (Song& song) { song.swapEffectSlots (t, slot, slot + 1); });
        selectedSlot = slot + 1;
    });
    if ((*slots())[(std::size_t) slot].kind == EffectKind::plugin)
    {
        auto* live = dynamic_cast<PluginEffect*> (engine.getEffect (track, slot).get());
        menu.addSubMenu (tr ("Processing"), pluginProcessingMenu (engine, (*slots())[(std::size_t) slot].plugin, live != nullptr ? &live->getInstance() : nullptr,
                                                                [t = track, slot] (Song& song) -> PluginRef* {
                                                                    if (t >= (int) song.mixer.size() || slot >= (int) song.mixer[(std::size_t) t].effects.size())
                                                                        return nullptr;
                                                                    return &song.mixer[(std::size_t) t].effects[(std::size_t) slot].plugin;
                                                                }));
    }
    if ((*slots())[(std::size_t) slot].kind == EffectKind::plugin)
    {
        if (auto* p = dynamic_cast<PluginEffect*> (engine.getEffect (track, slot).get()))
            addPluginParameterItems (menu, engine, p->getInstance(), { AutomationTargetKind::effectParam, track, slot, 0 });
    }

    menu.addSeparator();
    menu.addItem (tr ("Delete") + "\tDel", [this, slot] { deleteSlot (slot); });

    // FL: with the slot's menu open, Delete removes the plugin (issue #11)
    quickpick::showMenu (menu, juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), {},
                         [safe = juce::Component::SafePointer<EffectsPanel> (this), slot] (const juce::KeyPress& key) {
                             if (! key.isKeyCode (juce::KeyPress::deleteKey) || safe == nullptr)
                                 return false;
                             juce::MessageManager::callAsync ([safe, slot] {
                                 if (safe != nullptr)
                                     safe->deleteSlot (slot);
                             });
                             return true;
                         });
}

void EffectsPanel::deleteSlot (int slot)
{
    const auto* s = slots();
    if (s == nullptr || slot < 0 || slot >= (int) s->size())
        return;
    engine.editSong ([t = track, slot] (Song& song) { song.removeEffectSlot (t, slot); });
    selectedSlot = juce::jmin (slot, (int) slots()->size() - 1);
    repaint();
}

void EffectsPanel::changeListenerCallback (juce::ChangeBroadcaster*)
{
    const auto* s = slots();

    if (s == nullptr)
        track = 0;
    else if (selectedSlot >= (int) s->size())
        selectedSlot = (int) s->size() - 1;

    refit(); // the clean mixer (round 163) changes the height the panel needs
    repaint();
}

// ---------------------------------------------------------------------------

namespace mixerinput
{
std::function<void()> openAudioSettings;

namespace
{
juce::String issueText (const AudioEngine::RecordingNotice& notice)
{
    using I = AudioEngine::InputIssue;
    switch (notice.issue)
    {
        case I::noDevice: return tr ("No audio device is open. Choose one in Settings > Audio.");
        case I::noInputDevice:
            return tr ("No audio input device was found. Connect a microphone or an audio interface, then choose it as the input device in Settings > Audio.");
        case I::noInputOpen: return tr ("No audio input is open, so nothing can be recorded. Choose an input device in Settings > Audio.");
        case I::armedWithoutInput:
            return tr ("Please select an audio input first: the armed insert has none. Click the input box above the insert's effect slots in the Mixer (or right-click its arm button).");
        case I::insertWithoutInput: return tr ("Insert {0} is armed but has no audio input, so it records nothing.", { juce::String (notice.track) });
        case I::failed: return tr ("The audio input could not be used: {0}", { notice.detail });
        case I::lowDisk:
            return tr ("The recordings drive is almost full: about {0} minute(s) of recording left. Free some space or choose another project folder.",
                       { notice.detail });
        case I::recordSafe:
            return tr ("Insert {0} is record safe, so it can't be armed. Right-click its arm button and turn \"Record safe\" off first.",
                       { juce::String (notice.track) });
        case I::framesLost:
            return tr ("The disk could not keep up while recording: {0} sample(s) became silence (the timing of the take was kept). "
                       "Record to a faster drive or close other programs that use the disk.", { notice.detail });
        case I::none: break;
    }
    return {};
}

bool aboutTheDevice (AudioEngine::InputIssue issue)
{
    using I = AudioEngine::InputIssue;
    return issue == I::noDevice || issue == I::noInputDevice || issue == I::noInputOpen || issue == I::failed;
}
} // namespace

void explain (const AudioEngine::RecordingNotice& notice)
{
    if (notice.issue == AudioEngine::InputIssue::none)
        return;

    using I = AudioEngine::InputIssue;
    const auto aboutRecording = notice.issue == I::lowDisk || notice.issue == I::recordSafe || notice.issue == I::framesLost;
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle (aboutRecording ? tr ("Recording") : tr ("Audio input needed"))
                       .withMessage (issueText (notice));

    if (aboutTheDevice (notice.issue) && openAudioSettings)
    {
        options = options.withButton (tr ("Audio settings...")).withButton (tr ("Close"));
        juce::AlertWindow::showAsync (options, [] (int result) {
            if (result == 1 && openAudioSettings)
                openAudioSettings();
        });
    }
    else
        juce::AlertWindow::showAsync (options.withButton (tr ("OK")), nullptr);
}

// Round 157 ([DATA] the 0.1.15 projects of a singer who found that "the effects do nothing" on her voice): her
// microphone was the input of both Insert 1 (with the effects) and the Master. The Master plays its input straight
// away, without any insert's effects, so the dry voice covered the processed one. When an input is chosen and the
// Master plays the same device channel as an insert, LYNX says so and offers to take it off the Master.
void warnDoubleMonitoring (AudioEngine& engine, int track)
{
    const auto& mixer = engine.getSong().mixer;
    if (mixer.empty() || track < 0 || track >= (int) mixer.size())
        return;
    auto channels = [] (const MixerTrack& t) {
        std::pair<int, int> r { t.inputChannel, t.inputChannel + (t.inputStereo ? 1 : 0) };
        return r;
    };
    auto overlaps = [&] (const MixerTrack& a, const MixerTrack& b) {
        if (a.inputChannel < 0 || b.inputChannel < 0)
            return false;
        const auto [a0, a1] = channels (a);
        const auto [b0, b1] = channels (b);
        return a0 <= b1 && b0 <= a1;
    };
    const auto& master = mixer[0];
    if (master.inputChannel < 0 || master.monitor == InputMonitor::off)
        return;
    int insert = -1;
    for (int t = 1; t < (int) mixer.size() && insert < 0; ++t)
        if ((track == 0 || t == track) && overlaps (master, mixer[(std::size_t) t]))
            insert = t;
    if (insert < 0)
        return;
    const auto name = juce::String::fromUTF8 (mixer[(std::size_t) insert].name.c_str());
    auto options = juce::MessageBoxOptions()
                       .withIconType (juce::MessageBoxIconType::WarningIcon)
                       .withTitle (tr ("The same input plays twice"))
                       .withMessage (tr ("This audio input is the input of the Master and of {0}. The Master plays it straight away, without "
                                         "{0}'s effects, so you hear the dry sound on top of the processed one (the effects seem to do nothing).\n\n"
                                         "To hear it through {0}'s effects, take it off the Master.", { name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (insert) }) }))
                       .withButton (tr ("Take it off the Master"))
                       .withButton (tr ("Keep both"));
    juce::AlertWindow::showAsync (options, [&engine] (int result) {
        if (result == 1)
            engine.setInsertInput (0, -1, true);
    });
}

void showMenu (AudioEngine& engine, int track, juce::Component* target, std::function<void()> chosen)
{
    if (track < 0 || track >= (int) engine.getSong().mixer.size())
        return;

    // the device input opens on demand (only then does Windows show the microphone as in use)
    if (! engine.hasAudioInput())
        if (const auto result = engine.openAudioInput(); result.issue != AudioEngine::InputIssue::none)
        {
            explain (result);
            return;
        }

    const auto& mt = engine.getSong().mixer[(std::size_t) track];
    const auto names = engine.getInputChannelNames();
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Audio input source"));
    menu.addItem (tr ("(none)"), true, mt.inputChannel < 0, [&engine, track] { engine.setInsertInput (track, -1, true); });

    const auto sources = inputSources (names);
    if (sources.empty())
        menu.addItem (tr ("The audio device has no open input channels"), false, false, nullptr);

    for (const auto& source : sources)
    {
        if (! source.stereo && source.channel == 0 && names.size() > 1)
            menu.addSeparator(); // pairs, then single channels
        const auto ticked = mt.inputChannel == source.channel && mt.inputStereo == source.stereo;
        menu.addItem (juce::String::fromUTF8 (source.name.c_str()) + (source.stereo ? "" : "  " + tr ("(mono)")), true, ticked,
                      [&engine, track, source, chosen] {
                          engine.setInsertInput (track, source.channel, source.stereo);
                          warnDoubleMonitoring (engine, track);
                          if (chosen)
                              chosen();
                      });
    }

    menu.addSectionHeader (tr ("Monitor external input"));
    const std::pair<InputMonitor, const char*> modes[] = { { InputMonitor::off, TR_NOOP ("Off") },
                                                          { InputMonitor::whenArmed, TR_NOOP ("When armed") },
                                                          { InputMonitor::automatic, TR_NOOP ("Auto (when armed, not while playing back)") },
                                                          { InputMonitor::on, TR_NOOP ("On") } };
    for (const auto& [mode, label] : modes)
    {
        // the Master has no record arm: its input plays unless monitoring is off
        if (track == 0 && (mode == InputMonitor::whenArmed || mode == InputMonitor::automatic))
            continue;
        const auto ticked = track == 0 ? (mode == InputMonitor::off) == (mt.monitor == InputMonitor::off) : mt.monitor == mode;
        menu.addItem (tr (label), true, ticked, [&engine, track, mode = mode] { engine.setInsertMonitor (track, mode); });
    }

    if (track > 0) // record safe (round 163): the insert can't be armed by accident
    {
        menu.addSeparator();
        menu.addItem (tr ("Record safe (can't be armed)"), true, mt.recordSafe,
                      [&engine, track, safe = mt.recordSafe] { engine.setInsertRecordSafe (track, ! safe); });
    }

    menu.addSeparator();
    menu.addItem (tr ("Audio settings (input device)..."), openAudioSettings != nullptr, false, [] {
        if (openAudioSettings)
            openAudioSettings();
    });

    auto options = juce::PopupMenu::Options();
    if (target != nullptr)
        options = options.withTargetComponent (target).withMousePosition();
    menu.showMenuAsync (options);
}

void toggleArm (AudioEngine& engine, int track, juce::Component* target)
{
    if (engine.isInsertArmed (track))
    {
        engine.setInsertArmed (track, false);
        return;
    }

    const auto& mixer = engine.getSong().mixer;
    if (track <= 0 || track >= (int) mixer.size())
        return;

    if (mixer[(std::size_t) track].inputChannel >= 0)
    {
        engine.setInsertArmed (track, true);
        if (const auto notice = engine.takeRecordingNotice(); notice.issue != AudioEngine::InputIssue::none)
            explain (notice);
        return;
    }

    // FL: "Audio input needed - Please select an audio input first." Here the input menu opens right away
    ui::postHint ({ tr ("Audio input needed: choose this insert's input, then it is armed"), {}, false, false }, 4000);
    showMenu (engine, track, target, [&engine, track] { engine.setInsertArmed (track, true); });
}
} // namespace mixerinput

// ---------------------------------------------------------------------------

MixerComponent::MixerComponent (AudioEngine& e)
    : engine (e),
      strips (e),
      effects (e)
{
    viewport.setViewedComponent (&strips, false);
    viewport.setScrollBarsShown (false, true);
    addAndMakeVisible (viewport);
    effectsView.setViewedComponent (&effects, false);
    effectsView.setScrollBarsShown (true, false);
    effectsView.setScrollBarThickness (10);
    addAndMakeVisible (effectsView);
    strips.onSelectionChanged = [this] (int track) { effects.setTrack (track); };
    strips.onEffectDropped = [this] (int track, EffectSlot fx) {
        strips.selectTrack (track);
        effects.setTrack (track);
        if (! addEffectToSelected (std::move (fx)))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Plugin database"),
                                                    tr ("The selected insert has no free effect slot."));
    };
}

bool MixerComponent::runAction (const std::string& id)
{
    if (id == "mixer.moveLeft")
        strips.moveSelected (-1);
    else if (id == "mixer.moveRight")
        strips.moveSelected (1);
    else if (id == "mixer.linkChannels")
        strips.linkSelectedChannels (false);
    else if (id == "mixer.linkChannelsFrom")
        strips.linkSelectedChannels (true);
    else if (id == "mixer.selectLinkedChannels")
        strips.selectLinkedChannels();
    else if (id == "mixer.deleteEffect") // FL: Delete on the selected effect slot (issue #11)
        effects.deleteSlot (effects.getSelectedSlot());
    else
        return false;

    return true;
}

bool MixerComponent::addEffectToSelected (EffectSlot slot)
{
    const auto t = strips.getSelectedTrack();
    bool added = false;
    engine.editSong ([t, &slot, &added] (Song& s) {
        if (s.mixer.empty())
            s.mixer = Song::defaultMixer (0);
        if (t < (int) s.mixer.size() && (int) s.mixer[(std::size_t) t].effects.size() < maxEffectSlots)
        {
            s.mixer[(std::size_t) t].effects.push_back (std::move (slot));
            added = true;
        }
    });

    if (added)
    {
        effects.setTrack (t);
        effects.selectSlot ((int) engine.getSong().mixer[(std::size_t) t].effects.size() - 1);
    }

    return added;
}

void MixerComponent::selectFirstTrackWithEffects()
{
    const auto& mixer = engine.getSong().mixer;

    for (int t = 0; t < (int) mixer.size(); ++t)
        if (! mixer[(std::size_t) t].effects.empty())
        {
            strips.selectTrack (t);
            return;
        }
}

void MixerComponent::selectTrack (int track)
{
    strips.selectTrack (track);

    // scroll the strip into view
    const auto t = strips.getSelectedTrack();
    const auto x = strips.stripLeft (t);
    const auto w = t == 0 ? MixerStrips::masterWidth : MixerStrips::stripWidth;
    const auto view = viewport.getViewArea();

    if (x < view.getX() || x + w > view.getRight())
        viewport.setViewPosition (juce::jmax (0, x - view.getWidth() / 2), 0);
}

void MixerComponent::resized()
{
    auto area = getLocalBounds();
    effectsView.setBounds (area.removeFromRight (effectsWidth));
    effects.fitToView (effectsView.getWidth(), effectsView.getHeight(), effectsView.getScrollBarThickness());
    viewport.setBounds (area);
    strips.setVisibleHeight (area.getHeight() - viewport.getScrollBarThickness());
}
} // namespace lynx
