#include "core/FruityDance.h"
#include "core/VideoPlayer.h"
#include "ui/ColourPicker.h"
#include "ui/PatcherEditor.h"
#include "ui/UiTrace.h"
#include "ui/TextCache.h"
#include "ui/ChannelRackComponent.h"
#include "core/Patcherize.h"
#include "ui/QuickPick.h"
#include "core/ControlSurface.h"
#include "core/VoltageController.h"

#include "core/MixerOps.h"

#include "ui/AutomationUi.h"
#include "core/NoteTools.h"
#include "core/FlSynths.h"
#include "core/MidiControls.h"
#include "ui/ToolWindow.h"
#include "core/Slicer.h"
#include "ui/ChannelSettings.h"

#include "engine/Text.h"
#include "ui/Dialogs.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/BrowserComponent.h"

#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "ui/AudioEditor.h"
#include "core/RackOps.h"
#include "core/SoundFont.h"
#include "core/Vfx.h"
#include "ui/SoundFontEditor.h"

namespace lynx
{
namespace
{
// English name (used for new channel names stored in the song)
juce::String instrumentName (InstrumentKind k)
{
    switch (k)
    {
        case InstrumentKind::kick: return "Kick";
        case InstrumentKind::snare: return "Snare";
        case InstrumentKind::hat: return "Hat";
        case InstrumentKind::clap: return "Clap";
        case InstrumentKind::synth: return "Synth";
        case InstrumentKind::sampler: return "Sampler";
        case InstrumentKind::audioClip: return "Audio clip";
        case InstrumentKind::plugin: return "Plugin";
        case InstrumentKind::controller: return "Controller";
        case InstrumentKind::sakura: return "Sakura";
        case InstrumentKind::drumpad: return "Drumpad";
        case InstrumentKind::boobass: return "BooBass";
        case InstrumentKind::drumsynth: return "DrumSynth Live";
        case InstrumentKind::harmless: return "Harmless";
        case InstrumentKind::sytrus: return "Sytrus";
        case InstrumentKind::harmor: return "Harmor";
        case InstrumentKind::gms: return "GMS";
        case InstrumentKind::morphine: return "Morphine";
        case InstrumentKind::ogun: return "Ogun";
        case InstrumentKind::minisynth: return "MiniSynth";
        case InstrumentKind::dx10: return "Fruity DX10";
        case InstrumentKind::kepler: return "Kepler";
        case InstrumentKind::poizone: return "PoiZone";
        case InstrumentKind::fruitkick: return "Fruit Kick";
        case InstrumentKind::bassdrum: return "BassDrum";
        case InstrumentKind::toxic: return "Toxic Biohazard";
        case InstrumentKind::threeosc: return "3x Osc";
        case InstrumentKind::autogun: return "Autogun";
        case InstrumentKind::transistorbass: return "Transistor Bass";
        case InstrumentKind::simsynth: return "SimSynth";
        case InstrumentKind::wasp: return "Wasp";
        case InstrumentKind::plucked: return "Plucked!";
        case InstrumentKind::beepmap: return "BeepMap";
        case InstrumentKind::sawer: return "Sawer";
        case InstrumentKind::flkeys: return "FL Keys";
        case InstrumentKind::granulizer: return "Fruity Granulizer";
        case InstrumentKind::drumaxx: return "Drumaxx";
        case InstrumentKind::slayer: return "FL Slayer";
        case InstrumentKind::fruitydance: return "Fruity Dance";
        case InstrumentKind::directwave: return "DirectWave";
        case InstrumentKind::midiOut: return "MIDI Out";
        case InstrumentKind::patcher: return "Patcher";
        case InstrumentKind::soundfont: return "LYNX Soundfont";
        case InstrumentKind::layer: return "Layer";
        case InstrumentKind::dashboard: return "Dashboard";
        case InstrumentKind::voltage: return "Voltage Controller";
        case InstrumentKind::vfx: return "VFX";
        case InstrumentKind::videoplayer: return "Fruity Video Player";
    }
    return "?";
}

// name shown in menus (translated)
juce::String instrumentLabel (InstrumentKind k)
{
    switch (k)
    {
        case InstrumentKind::kick: return tr ("Kick");
        case InstrumentKind::snare: return tr ("Snare");
        case InstrumentKind::hat: return tr ("Hat");
        case InstrumentKind::clap: return tr ("Clap");
        case InstrumentKind::synth: return tr ("Synth");
        case InstrumentKind::sampler: return tr ("Sampler");
        case InstrumentKind::audioClip: return tr ("Audio clip");
        case InstrumentKind::controller: return tr ("Envelope Controller");
        case InstrumentKind::midiOut: return tr ("MIDI Out");
        case InstrumentKind::layer: return tr ("Layer");
        case InstrumentKind::dashboard: return "Dashboard";
        case InstrumentKind::voltage: return "Voltage Controller";
        default: return instrumentName (k);
    }
}

constexpr InstrumentKind builtInKinds[] = { InstrumentKind::kick, InstrumentKind::snare, InstrumentKind::hat,
                                            InstrumentKind::clap, InstrumentKind::synth };

void setStep (Pattern& p, int channel, int step, bool on)
{
    if (p.hasStep (channel, step) != on)
        p.toggleStep (channel, step);
}

juce::String channelName (const Song& song, int row)
{
    return row >= 0 && row < (int) song.channels.size() ? juce::String::fromUTF8 (song.channels[(std::size_t) row].name.c_str()) : juce::String();
}
} // namespace

bool ChannelRackComponent::keepEditorsOpen = false;

ChannelRackComponent::ChannelRackComponent (AudioEngine& e)
    : engine (e)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);
    engine.addChangeListener (this);
    previews.setSender ([this] (int channel, int key, float velocity, bool on) { engine.sendLiveNote (channel, key, velocity, on); });
    updateSize();
    startTimerHz (30);
}

ChannelRackComponent::~ChannelRackComponent()
{
    previews.releaseAll();
    engine.removeChangeListener (this);
}

void ChannelRackComponent::moved()
{
    // the viewport scrolled the rack sideways: the controls column follows the visible left edge
    const auto x = juce::jmax (0, -getX());

    if (x != stickyX)
    {
        stickyX = x;
        repaint();
    }
}

void ChannelRackComponent::visibilityChanged()
{
    if (! isShowing())
        previews.releaseAll(); // hidden mid-gesture: nothing keeps sounding
}

int ChannelRackComponent::numSteps() const
{
    const auto& song = engine.getSong();

    if (song.patterns.empty())
        return 16;

    const auto& p = song.patterns[(std::size_t) song.currentPattern];
    const auto steps = (int) (p.effectiveLength (song.beatsPerBar) / ticksPerStep);
    return juce::jlimit (16, 512, steps);
}

int ChannelRackComponent::shownSteps() const
{
    return juce::jmin (512, numSteps() + ghostSteps);
}

bool ChannelRackComponent::isShown (int row) const
{
    const auto& channels = engine.getSong().channels;
    return filterGroup < 0 || row < 0 || row >= (int) channels.size() || channels[(std::size_t) row].group == filterGroup;
}

int ChannelRackComponent::visibleIndex (int row) const
{
    if (filterGroup < 0)
        return row;

    int index = 0;
    for (int r = 0; r < row && r < (int) engine.getSong().channels.size(); ++r)
        index += isShown (r) ? 1 : 0;
    return index + juce::jmax (0, row - (int) engine.getSong().channels.size());
}

int ChannelRackComponent::shownCount() const
{
    return visibleIndex ((int) engine.getSong().channels.size());
}

void ChannelRackComponent::setFilterGroup (int group)
{
    const auto groups = (int) engine.getSong().channelGroups.size();
    group = group >= 0 && group < groups ? group : -1;

    if (group == filterGroup)
        return;

    filterGroup = group;
    updateSize();
    repaint();

    if (onFilterChanged)
        onFilterChanged();
}

juce::String ChannelRackComponent::filterName() const
{
    const auto& groups = engine.getSong().channelGroups;
    return filterGroup >= 0 && filterGroup < (int) groups.size() ? juce::String::fromUTF8 (groups[(std::size_t) filterGroup].c_str()) : tr ("All");
}

void ChannelRackComponent::showFilterMenu (juce::Component* target)
{
    const auto& song = engine.getSong();
    juce::PopupMenu menu;
    menu.addItem (tr ("All"), true, filterGroup < 0, [this] { setFilterGroup (-1); });

    if (! song.channelGroups.empty())
        menu.addSeparator();

    for (int g = 0; g < (int) song.channelGroups.size(); ++g)
        menu.addItem (juce::String::fromUTF8 (song.channelGroups[(std::size_t) g].c_str()), true, filterGroup == g, [this, g] { setFilterGroup (g); });

    menu.addSeparator();
    menu.addItem (tr ("Group selected...") + "\tAlt+G", [this] { groupSelected(); });
    {
        juce::PopupMenu move;
        move.addItem (tr ("(no group)"), [this] { moveSelectedToGroup (-1); });
        for (int g = 0; g < (int) song.channelGroups.size(); ++g)
            move.addItem (juce::String::fromUTF8 (song.channelGroups[(std::size_t) g].c_str()), [this, g] { moveSelectedToGroup (g); });
        menu.addSubMenu (tr ("Move selected channels to"), move, ! engine.getSelectedChannels().empty());
    }
    menu.addItem (tr ("Rename this group..."), filterGroup >= 0, false, [this] {
        const auto group = filterGroup;
        auto* window = new juce::AlertWindow (tr ("Filter group name"), {}, juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("name", filterName());
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, group, window] (int result) {
                                     const auto name = window->getTextEditorContents ("name").trim().toStdString();
                                     if (result != 1 || name.empty())
                                         return;
                                     engine.editSong ([group, name] (Song& s) {
                                         if (group < (int) s.channelGroups.size())
                                             s.channelGroups[(std::size_t) group] = name;
                                     });
                                     if (onFilterChanged)
                                         onFilterChanged();
                                 }),
                                 true);
    });
    menu.addItem (tr ("Delete this group (keeps its channels)"), filterGroup >= 0, false, [this] {
        const auto group = filterGroup;
        setFilterGroup (-1);
        engine.editSong ([group] (Song& s) {
            if (group >= (int) s.channelGroups.size())
                return;
            s.channelGroups.erase (s.channelGroups.begin() + group);
            for (auto& c : s.channels)
                c.group = c.group == group ? -1 : c.group > group ? c.group - 1 : c.group;
        });
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

void ChannelRackComponent::moveSelectedToGroup (int group)
{
    engine.editSong ([group, rows = engine.getSelectedChannels()] (Song& s) {
        for (auto r : rows)
            if (r >= 0 && r < (int) s.channels.size())
                s.channels[(std::size_t) r].group = group >= 0 && group < (int) s.channelGroups.size() ? group : -1;
    });
    updateSize();
    repaint();
}

void ChannelRackComponent::groupSelected()
{
    // FL: Alt+G asks for the "Filter group name"; the selected channels move into the new group, which is shown
    auto* window = new juce::AlertWindow (tr ("Filter group name"), {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", tr ("Group {0}", { juce::String ((int) engine.getSong().channelGroups.size() + 1) }));
    window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result) {
                                 const auto name = window->getTextEditorContents ("name").trim().toStdString();
                                 if (result != 1 || name.empty())
                                     return;
                                 const auto group = (int) engine.getSong().channelGroups.size();
                                 engine.editSong ([name, rows = engine.getSelectedChannels()] (Song& s) {
                                     s.channelGroups.push_back (name);
                                     for (auto r : rows)
                                         if (r >= 0 && r < (int) s.channels.size())
                                             s.channels[(std::size_t) r].group = (int) s.channelGroups.size() - 1;
                                 });
                                 setFilterGroup (group);
                             }),
                             true);
}

void ChannelRackComponent::updateSize()
{
    const auto rows = shownCount() + 1; // +1 = "add channel" row
    // FL's ghost steps: when the rack is wider than the pattern, the free width shows faint steps after its end;
    // clicking one adds a step there and makes the pattern longer (issue #11)
    ghostSteps = 0;
    if (const auto* holder = getParentComponent())
        ghostSteps = juce::jmax (0, (holder->getWidth() - gridLeft() - 8) / stepWidth - numSteps());
    setSize (gridLeft() + shownSteps() * stepWidth + 8, rows * rowHeight + 8 + (graphRow() >= 0 ? laneHeight : 0));
}

int ChannelRackComponent::graphRow() const
{
    const auto count = (int) engine.getSong().channels.size();
    const auto row = graphLanes && count > 0 ? juce::jlimit (0, count - 1, engine.getSelectedChannel()) : -1;
    return row >= 0 && isShown (row) ? row : -1;
}

int ChannelRackComponent::rowY (int row) const
{
    const auto lane = graphRow();
    return 4 + visibleIndex (row) * rowHeight + (lane >= 0 && row > lane ? laneHeight : 0);
}

int ChannelRackComponent::rowAtY (int y) const
{
    if (y < 4)
        return -1;

    if (const auto lane = graphRow(); lane >= 0)
    {
        const auto laneTop = rowY (lane) + rowHeight;
        if (y >= laneTop && y < laneTop + laneHeight)
            return lane;
        if (y >= laneTop + laneHeight)
            y -= laneHeight;
    }

    const auto index = (y - 4) / rowHeight;

    if (filterGroup < 0)
        return index;

    // the index-th shown row (the "add channel" row after the last one)
    const auto count = (int) engine.getSong().channels.size();
    int seen = 0;
    for (int r = 0; r < count; ++r)
        if (isShown (r) && seen++ == index)
            return r;
    return count + (index - seen);
}

juce::Rectangle<int> ChannelRackComponent::laneBounds() const
{
    const auto lane = graphRow();
    return lane < 0 ? juce::Rectangle<int>() : juce::Rectangle<int> (0, rowY (lane) + rowHeight, getWidth(), laneHeight);
}

juce::Rectangle<int> ChannelRackComponent::rowBounds (int row) const
{
    return { 0, rowY (row), getWidth(), rowHeight };
}

juce::Rectangle<int> ChannelRackComponent::stepBounds (int row, int step) const
{
    return { gridLeft() + step * stepWidth + 1, rowY (row) + 4, stepWidth - 3, rowHeight - 8 };
}

ChannelRackComponent::Area ChannelRackComponent::areaAt (int x, int y, int& row) const
{
    const auto channels = (int) engine.getSong().channels.size();
    row = rowAtY (y);

    if (x < stickyX + gridLeft()) // the controls column (painted at the visible left edge)
        x -= stickyX;

    if (row < 0 || row > channels)
        return Area::none;

    if (laneBounds().contains (x, y))
        return x < gridLeft() ? Area::laneLevels : Area::lane;

    if (row == channels)
        return x < gridLeft() ? Area::addRow : Area::none;

    if (x < muteWidth)
        return Area::mute;
    if (x < muteWidth + knobWidth)
        return Area::pan;
    if (x < muteWidth + 2 * knobWidth)
        return Area::volume;
    if (x < nameLeft())
        return Area::route;
    if (x < selectLeft())
        return Area::name;
    if (x < gridLeft())
        return Area::select;

    return Area::grid;
}

bool ChannelRackComponent::cellHasNote (int channel, int step) const
{
    const auto& song = engine.getSong();
    const auto& p = song.patterns[(std::size_t) song.currentPattern];
    const auto from = (std::int64_t) step * ticksPerStep;
    const auto to = from + ticksPerStep;

    for (const auto& n : p.notes)
        if (n.channel == channel && n.start >= from && n.start < to)
            return true;

    return false;
}

void ChannelRackComponent::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("ChannelRackComponent paint", true, g.getClipBounds());
    using namespace theme::colour;
    g.fillAll (bg2);

    const auto& song = engine.getSong();

    if (song.patterns.empty())
        return;

    const auto& pattern = song.patterns[(std::size_t) song.currentPattern];
    const auto steps = shownSteps(), realSteps = numSteps();
    const auto clip = g.getClipBounds();
    const auto primary = engine.getSelectedChannel();
    const auto& selection = engine.getSelectedChannels();
    g.setFont (theme::font (theme::type::body));

    for (int row = 0; row < (int) song.channels.size(); ++row)
    {
        const auto y = rowY (row);

        if (! isShown (row) || y > clip.getBottom() || y + rowHeight < clip.getY())
            continue;

        const auto& channel = song.channels[(std::size_t) row];
        const auto selected = std::binary_search (selection.begin(), selection.end(), row);

        if (selected) // selected rows are subtly lifted
        {
            g.setColour (bg3);
            g.fillRect (0, y + 1, getWidth(), rowHeight - 2);
        }

        if (channel.instrument == InstrumentKind::audioClip)
        {
            g.setColour (textDisabled);
            g.setFont (theme::font (theme::type::small));
            textcache::drawText (g, tr ("audio clip — plays from the Playlist"), gridLeft() + 4, y, 400, rowHeight, juce::Justification::centredLeft);
            g.setFont (theme::font (theme::type::body));
            continue;
        }

        if (showsNotePreview (pattern, row, pianoRollView)) // piano-roll notes: a miniature instead of the step buttons
        {
            paintNotePreview (g, row, pattern, channel.colour);
            continue;
        }

        for (int step = 0; step < steps; ++step)
        {
            const auto r = stepBounds (row, step);

            if (r.getRight() < clip.getX() || r.getX() > clip.getRight())
                continue;

            const auto onGrid = pattern.hasStep (row, step);
            const auto any = onGrid || cellHasNote (row, step);

            const auto hoverStepHere = hoverArea == Area::grid && hoverRow == row && hoverStep == step && paintMode == PaintMode::none;
            const auto ghost = step >= realSteps && ! any; // after the pattern's end: faint until clicked
            auto fill = any ? (onGrid ? stepOn : stepNote) : theme::stepOff (step);
            if (ghost) // a faint outlined cell, filled while hovered
            {
                theme::fillRoundRect (g, r.toFloat(), theme::radius::sm, fill.withMultipliedAlpha (hoverStepHere ? 0.9f : 0.35f));
                g.setColour (fill.brighter (0.25f).withMultipliedAlpha (0.55f));
                g.drawRoundedRectangle (r.toFloat().reduced (0.5f), theme::radius::sm, 1.0f);
            }
            else
                theme::fillRoundRect (g, r.toFloat(), theme::radius::sm, hoverStepHere ? fill.brighter (any ? 0.15f : 0.35f) : fill);

            if (hoverStepHere)
            {
                g.setColour (textSecondary.withAlpha (0.7f));
                g.drawRoundedRectangle (r.toFloat().reduced (0.5f), theme::radius::sm, 1.0f);
            }
        }
    }

    paintLane (g);
    const auto addY = rowY ((int) song.channels.size());
    g.setColour (textDisabled);
    g.setFont (theme::font (theme::type::small));
    textcache::drawText (g, tr ("or drop an audio file here"), gridLeft() + 4, addY, 400, rowHeight, juce::Justification::centredLeft);

    if (playheadStep >= 0 && playheadStep < steps)
    {
        theme::fillRoundRect (g, juce::Rectangle<float> ((float) (gridLeft() + playheadStep * stepWidth), 2.0f, (float) stepWidth - 2.0f,
                                                        (float) addY - 2.0f), theme::radius::sm, playhead.withAlpha (0.28f));
    }

    // the controls column stays put while the steps scroll sideways (FL's rack keeps the channel buttons in
    // view): the viewport moves the whole rack, so the column is painted again at the visible left edge
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (stickyX, 0, gridLeft(), getHeight());
        g.setOrigin (stickyX, 0);
        g.setColour (bg2);
        g.fillRect (0, 0, gridLeft(), getHeight());
        const auto column = g.getClipBounds();

        for (int row = 0; row < (int) song.channels.size(); ++row)
        {
            const auto y = rowY (row);

            if (! isShown (row) || y > column.getBottom() || y + rowHeight < column.getY())
                continue;

            const auto& channel = song.channels[(std::size_t) row];
            const auto selected = std::binary_search (selection.begin(), selection.end(), row);
            const auto nameArea = juce::Rectangle<float> ((float) nameLeft(), (float) y + 3, (float) nameWidth, (float) rowHeight - 6);

            if (selected) // selected rows are subtly lifted
            {
                g.setColour (bg3);
                g.fillRect (0, y + 1, getWidth(), rowHeight - 2);
            }

            const auto hovered = [this, row] (Area a) { return hoverRow == row && hoverArea == a; };
            const auto led = juce::Rectangle<float> (9.0f, (float) y + rowHeight / 2.0f - 4.5f, 9.0f, 9.0f);
            g.setColour (theme::muteLed (channel.muted));
            g.fillEllipse (led);

            if (hovered (Area::mute)) // hover ring
            {
                g.setColour (textSecondary.withAlpha (0.8f));
                g.drawEllipse (led.expanded (2.5f), 1.0f);
            }

            const auto touched = knobDrag != Area::none && knobRow == row;
            theme::drawKnob (g, controlBounds (Area::pan, row).toFloat().reduced (1.0f, 2.0f), (channel.pan + 1.0f) / 2.0f, true,
                             touched && knobDrag == Area::pan ? accent : info, hovered (Area::pan) || (touched && knobDrag == Area::pan));
            theme::drawKnob (g, controlBounds (Area::volume, row).toFloat().reduced (1.0f, 2.0f), channel.volume, false,
                             touched && knobDrag == Area::volume ? accent : knobArc, hovered (Area::volume) || (touched && knobDrag == Area::volume));

            // small box with the mixer insert ("-" = straight to Master)
            const auto routeArea = controlBounds (Area::route, row).toFloat();
            theme::fillRoundRect (g, routeArea, theme::radius::sm, hovered (Area::route) || (touched && knobDrag == Area::route) ? bg4 : bg0);

            if (hovered (Area::route))
            {
                g.setColour (borderStrong);
                g.drawRoundedRectangle (routeArea.reduced (0.5f), theme::radius::sm, 1.0f);
            }

            g.setColour (channel.mixerTrack > 0 ? textPrimary : textDisabled);
            g.setFont (theme::font (theme::type::caption, true));
            textcache::drawText (g, channel.mixerTrack > 0 ? juce::String (channel.mixerTrack) : juce::String ("-"), routeArea.toNearestInt(),
                        juce::Justification::centred);
            g.setFont (theme::font (theme::type::body));

            // name button: tinted with the channel colour (from the .flp or user-picked), so channels are found by
            // colour as in FL; kept dark and toned down (LYNX look), with the colour strip at its left
            {
                auto plate = row == primary ? bg6 : hovered (Area::name) ? bg5 : bg4;
                if (channel.colour != 0)
                    plate = plate.interpolatedWith (theme::tame (juce::Colour (channel.colour)), row == primary ? 0.42f : 0.32f);
                theme::fillRoundRect (g, nameArea, theme::radius::md, plate);
            }

            if (row == primary)
            {
                g.setColour (accent);
                g.drawRoundedRectangle (nameArea.reduced (0.5f), theme::radius::md, 1.0f);
            }

            theme::fillRoundRect (g, nameArea.withWidth (4.0f).reduced (0.0f, 3.0f).translated (3.0f, 0.0f), 1.5f, channel.colour != 0 ? theme::tame (juce::Colour (channel.colour)) : bg6.brighter (0.2f));

            g.setColour (channel.muted ? textDisabled : textPrimary);
            textcache::drawText (g, juce::String::fromUTF8 (channel.name.c_str()), nameArea.withTrimmedLeft (14).withTrimmedRight (6).toNearestInt(),
                        juce::Justification::centredLeft, true);

            // selector: a slim pill, lit when the row is selected
            const auto pill = juce::Rectangle<float> ((float) selectLeft() + 3.0f, (float) y + 7.0f, (float) selectWidth - 6.0f, (float) rowHeight - 14.0f);
            theme::fillRoundRect (g, pill, 3.0f, selected ? (hovered (Area::select) ? accentHover : accent) : hovered (Area::select) ? textDisabled : ledOff);
        }

        paintLane (g);
        {
            const auto addArea = juce::Rectangle<float> ((float) nameLeft(), (float) addY + 3, (float) nameWidth, (float) rowHeight - 6);
            const float dash[] = { 3.0f, 3.0f };
            juce::Path outline, dashed;
            outline.addRoundedRectangle (addArea.reduced (0.5f), theme::radius::md);
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, outline, dash, 2);
            const auto addHovered = hoverArea == Area::addRow;
            g.setColour (addHovered ? textSecondary : borderStrong);
            g.fillPath (dashed);
            g.setColour (addHovered ? textPrimary : textSecondary);
            g.setFont (theme::font (theme::type::small));
            textcache::drawText (g, tr ("+ add channel"), addArea.toNearestInt(), juce::Justification::centred);
        }

        if (stickyX > 0) // steps pass under the column: a hairline marks its edge
        {
            g.setColour (border);
            g.fillRect (gridLeft() - 1, 0, 1, getHeight());
        }
    }
}

// The miniature of a channel's piano-roll notes across its step area (FL shows one once a channel has notes
// that steps can't show); the notes keep their time and length, pitch maps onto the row's height.
void ChannelRackComponent::paintNotePreview (juce::Graphics& g, int row, const Pattern& pattern, std::uint32_t channelColour)
{
    using namespace theme::colour;
    const auto steps = numSteps();
    const auto area = juce::Rectangle<float> ((float) gridLeft() + 1.0f, (float) rowY (row) + 4.0f, (float) (steps * stepWidth) - 3.0f,
                                              (float) rowHeight - 8.0f);
    const auto hoveredHere = hoverArea == Area::grid && hoverRow == row;
    theme::fillRoundRect (g, area, theme::radius::sm, hoveredHere ? bg4 : bg1);

    for (int beat = 4; beat < steps; beat += 4) // beat lines, bars a little stronger
    {
        g.setColour (beat % 16 == 0 ? border.brighter (0.2f) : border);
        g.fillRect (area.getX() + (float) (beat * stepWidth) - 1.5f, area.getY() + 2.0f, 1.0f, area.getHeight() - 4.0f);
    }

    const auto [low, high] = channelKeyRange (pattern, row);
    const auto span = juce::jmax (high - low, 6); // a single key sits mid-row, not on an edge
    const auto lowKey = low - (span - (high - low)) / 2;
    const auto inner = area.reduced (1.0f, 3.0f);
    const auto noteHeight = juce::jlimit (3.0f, 5.0f, inner.getHeight() / (float) (span + 1));
    const auto ticks = (float) (steps * ticksPerStep);
    const auto tint = channelColour != 0 ? theme::tame (juce::Colour (channelColour)).brighter (0.6f) : stepNote;

    for (const auto& n : pattern.notes)
    {
        if (n.channel != row)
            continue;

        const auto x = inner.getX() + inner.getWidth() * (float) n.start / ticks;
        if (x > inner.getRight())
            break;
        const auto w = juce::jmax (2.0f, inner.getWidth() * (float) n.length / ticks - 1.0f);
        const auto y = inner.getBottom() - noteHeight - (inner.getHeight() - noteHeight) * (float) (n.key - lowKey) / (float) span;
        g.setColour (n.muted ? textDisabled : tint);
        g.fillRect (juce::Rectangle<float> (x, y, juce::jmin (w, inner.getRight() - x), noteHeight));
    }
}

// --- the control contract for the knobs and the route box ----------------------------------------

ui::ValueSpec ChannelRackComponent::specFor (Area area, int row) const
{
    const auto name = channelName (engine.getSong(), row);

    if (area == Area::pan)
    {
        auto spec = ui::panSpec (name + " - " + tr ("Pan"));
        spec.target = AutomationTarget { AutomationTargetKind::channelPan, row };
        return spec;
    }

    if (area == Area::volume)
    {
        auto spec = ui::percentSpec (name + " - " + tr ("Volume"), 0.78);
        spec.target = AutomationTarget { AutomationTargetKind::channelVolume, row };
        return spec;
    }

    // route box: the mixer insert, as a number
    ui::ValueSpec spec;
    spec.name = name + " - " + tr ("Target mixer track");
    spec.maximum = maxMixerTracks - 1; // every insert, like FL (round 162: the inserts past the mixer's last one are added)
    spec.interval = 1.0;
    spec.pixelsForRange = 3.0 * juce::jmax (1.0, spec.maximum);
    spec.toText = [] (double v) { return v < 0.5 ? tr ("Master") : tr ("Insert {0}", { juce::String (juce::roundToInt (v)) }); };
    spec.fromText = [] (const juce::String& t) -> std::optional<double> {
        if (t.trim().startsWithIgnoreCase ("m"))
            return 0.0;
        const auto digits = t.retainCharacters ("0123456789");
        return digits.isEmpty() ? std::nullopt : std::optional<double> (digits.getDoubleValue());
    };
    return spec;
}

double ChannelRackComponent::valueOf (Area area, int row) const
{
    const auto& c = engine.getSong().channels[(std::size_t) row];
    return area == Area::pan ? c.pan : area == Area::volume ? c.volume : (double) c.mixerTrack;
}

void ChannelRackComponent::setValue (Area area, int row, double value)
{
    if (row < 0 || row >= (int) engine.getSong().channels.size())
        return;

    if (area == Area::pan)
        engine.editSong ([row, v = (float) value] (Song& s) { s.channels[(std::size_t) row].pan = v; });
    else if (area == Area::volume)
        engine.editSong ([row, v = (float) value] (Song& s) { s.channels[(std::size_t) row].volume = v; });
    else if (area == Area::route)
    {
        const auto track = juce::roundToInt (value);
        if (track != engine.getSong().channels[(std::size_t) row].mixerTrack)
            engine.editSong ([row, track] (Song& s) { mixerops::routeChannel (s, row, track); });
    }
}

juce::Rectangle<int> ChannelRackComponent::controlBounds (Area area, int row) const
{
    const auto y = rowY (row);

    switch (area)
    {
        case Area::pan: return { muteWidth, y, knobWidth, rowHeight };
        case Area::volume: return { muteWidth + knobWidth, y, knobWidth, rowHeight };
        case Area::route: return { muteWidth + 2 * knobWidth + 1, y + 7, routeWidth - 3, rowHeight - 14 };
        case Area::lane:
        {
            const auto lane = laneBounds();
            const auto step = juce::jmax (0, hoverStep);
            return { gridLeft() + step * stepWidth, lane.getY() + 6, stepWidth, lane.getHeight() - 12 };
        }
        default: return {};
    }
}

void ChannelRackComponent::showTip (Area area, int row, bool transient)
{
    if (row < 0 || row >= (int) engine.getSong().channels.size())
        return;

    const auto spec = specFor (area, row);
    const auto text = spec.text (valueOf (area, row));
    ui::postHint (ui::valueHint (spec, valueOf (area, row), ui::valueGestures()));

    const auto bounds = area == Area::lane ? controlBounds (area, row) : controlBounds (area, row).translated (stickyX, 0);

    if (transient)
        ui::flashValueTip (*this, bounds, text);
    else
        ui::showValueTip (*this, bounds, text);
}

void ChannelRackComponent::setHover (Area area, int row, int step)
{
    if (area == hoverArea && row == hoverRow && step == hoverStep)
        return;

    auto dirty = [this] (Area a, int r) {
        if (a == Area::lane || a == Area::laneLevels)
            repaint (laneBounds());
        else if (a == Area::addRow)
            repaint (rowBounds ((int) engine.getSong().channels.size()));
        else if (r >= 0)
            repaint (rowBounds (r));
    };

    dirty (hoverArea, hoverRow);
    hoverArea = area;
    hoverRow = row;
    hoverStep = step;
    dirty (hoverArea, hoverRow);
}

void ChannelRackComponent::mouseMove (const juce::MouseEvent& e)
{
    int row = 0;
    const auto area = areaAt (e.x, e.y, row);
    const auto step = area == Area::grid || area == Area::lane ? (e.x - gridLeft()) / stepWidth : -1;
    setHover (area, row, step >= 0 && step < (area == Area::grid ? shownSteps() : numSteps()) ? step : -1);
}

void ChannelRackComponent::mouseExit (const juce::MouseEvent&)
{
    setHover (Area::none, -1, -1);
}

void ChannelRackComponent::setSwingMixOfSelected (int channel)
{
    const auto& song = engine.getSong();

    if (channel < 0 || channel >= (int) song.channels.size())
        return;

    auto spec = ui::percentSpec (channelName (song, channel) + " - " + tr ("Swing mix"), 1.0);
    const auto rows = engine.getSelectedChannels();
    ui::typeValue (*this, spec, song.channels[(std::size_t) channel].swingMix, [this, rows, channel] (double v) {
        engine.editSong ([rows, channel, v] (Song& s) {
            auto targets = rows;
            if (std::find (targets.begin(), targets.end(), channel) == targets.end())
                targets = { channel };
            for (auto r : targets)
                if (r >= 0 && r < (int) s.channels.size())
                    s.channels[(std::size_t) r].swingMix = (float) v;
        });
    });
}

// --- selection -----------------------------------------------------------------------------------

void ChannelRackComponent::selectRows (std::vector<int> rows, int primary)
{
    engine.setChannelSelection (std::move (rows), primary);
    repaint();
}

void ChannelRackComponent::clickSelector (int row, const juce::ModifierKeys& mods)
{
    ChannelSelection selection;
    selection.rows = engine.getSelectedChannels();
    selection.anchor = selectionAnchor;

    if (mods.isShiftDown())
        selection.extendTo (row);
    else if (mods.isCtrlDown() || mods.isPopupMenu())
        selection.toggle (row);
    else
        selection.selectOnly (row);

    selectionAnchor = selection.anchor;
    const auto primary = selection.contains (row) ? row : selection.rows.front();
    selectRows (selection.rows, primary);
}

void ChannelRackComponent::selectRelative (int delta, bool extend)
{
    const auto count = (int) engine.getSong().channels.size();

    if (count == 0)
        return;

    const auto row = juce::jlimit (0, count - 1, engine.getSelectedChannel() + delta);

    if (extend)
    {
        ChannelSelection selection;
        selection.anchor = selectionAnchor;
        selection.extendTo (row);
        selectRows (selection.rows, row);
    }
    else
    {
        selectionAnchor = row;
        selectRows ({ row }, row);
    }
}

void ChannelRackComponent::selectAll()
{
    ChannelSelection selection;
    selection.selectAll ((int) engine.getSong().channels.size());
    selectRows (selection.rows, engine.getSelectedChannel());
}

bool ChannelRackComponent::moveSelected (int delta)
{
    auto rows = engine.getSelectedChannels();
    const auto primary = engine.getSelectedChannel();
    const auto primaryIndex = (int) (std::find (rows.begin(), rows.end(), primary) - rows.begin());
    bool moved = false;
    std::vector<int> after;

    engine.editSong ([&] (Song& s) {
        after = rows;
        moved = moveChannels (s, after, delta);
    });

    if (! moved)
        return false;

    const auto newPrimary = primaryIndex < (int) after.size() ? after[(std::size_t) primaryIndex] : after.front();
    openEditorRow = -1;
    selectionAnchor = juce::jlimit (0, (int) engine.getSong().channels.size() - 1, selectionAnchor + delta);
    selectRows (after, newPrimary);
    return true;
}

void ChannelRackComponent::cloneSelected()
{
    auto rows = engine.getSelectedChannels();
    std::vector<int> clones;

    engine.editSong ([&] (Song& s) {
        // from the bottom up, so earlier clones don't shift the rows still to clone
        for (auto it = rows.rbegin(); it != rows.rend(); ++it)
            cloneChannel (s, *it);
    });

    // each clone sits right below its original: original i (sorted) ends at rows[i] + i, its clone one below
    for (std::size_t i = 0; i < rows.size(); ++i)
        clones.push_back (rows[i] + (int) i + 1);

    if (! clones.empty())
        selectRows (clones, clones.front());
}

void ChannelRackComponent::deleteSelected()
{
    const auto rows = engine.getSelectedChannels();
    const auto count = (int) engine.getSong().channels.size();

    if (rows.empty() || (int) rows.size() >= count) // at least one channel stays
        return;

    for (auto r : rows)
        closeEditor (r);

    engine.editSong ([rows] (Song& s) { removeChannels (s, rows); });
    openEditorRow = -1;
    const auto row = juce::jlimit (0, juce::jmax (0, (int) engine.getSong().channels.size() - 1), rows.front());
    selectRows ({ row }, row);
}

void ChannelRackComponent::sendSelectedToInserts()
{
    const auto rows = engine.getSelectedChannels();
    engine.editSong ([rows] (Song& s) { sendChannelsToFreeInserts (s, rows); });
}

void ChannelRackComponent::toggleMute (int channel)
{
    if (channel >= 0 && channel < (int) engine.getSong().channels.size())
        engine.editSong ([channel] (Song& s) { s.channels[(std::size_t) channel].muted = ! s.channels[(std::size_t) channel].muted; });
}

void ChannelRackComponent::solo (int row)
{
    if (row < 0 || row >= (int) engine.getSong().channels.size())
        return;

    // only this channel plays; repeating on the already-soloed channel unmutes all
    engine.editSong ([row] (Song& s) {
        const auto alone = ! s.channels[(std::size_t) row].muted
                           && std::all_of (s.channels.begin(), s.channels.end(), [&] (const ChannelDef& c) {
                                  return &c == &s.channels[(std::size_t) row] || c.muted;
                              });

        for (std::size_t i = 0; i < s.channels.size(); ++i)
            s.channels[i].muted = alone ? false : i != (std::size_t) row;
    });
}

// --- channel editors ---------------------------------------------------------------------------

bool ChannelRackComponent::isEditorOpen (int channel)
{
    if (channel < 0 || channel >= (int) engine.getSong().channels.size())
        return false;

    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::plugin)
    {
        if (auto* plugin = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (channel).get()))
            return PluginWindows::isOpen (plugin->getInstance());
        return false;
    }

    return isChannelSettingsOpen (channel);
}

void ChannelRackComponent::closeEditor (int channel)
{
    if (channel < 0 || channel >= (int) engine.getSong().channels.size())
        return;

    if (engine.getSong().channels[(std::size_t) channel].instrument == InstrumentKind::plugin)
    {
        if (auto* plugin = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (channel).get()))
            PluginWindows::close (plugin->getInstance());
    }
    else
        closeChannelSettings (channel);
}

void ChannelRackComponent::toggleEditor (int channel, bool keepOthers)
{
    if (channel < 0 || channel >= (int) engine.getSong().channels.size())
        return;

    const auto kind = engine.getSong().channels[(std::size_t) channel].instrument;

    if (kind == InstrumentKind::patcher) // its node graph editor (REA_rodada141)
    {
        if (PatcherWindows::isChannelOpen (channel))
            PatcherWindows::closeChannel (channel);
        else
            PatcherWindows::openChannel (engine, channel);
        return;
    }
    if (kind != InstrumentKind::plugin && ! hasChannelSettings (kind))
        return; // built-in drums and audio clips have no editor window: the click only selects

    if (isEditorOpen (channel))
    {
        closeEditor (channel);
        if (openEditorRow == channel)
            openEditorRow = -1;
        return;
    }

    if (! keepOthers && ! keepEditorsOpen && openEditorRow != channel)
        closeEditor (openEditorRow);

    if (kind == InstrumentKind::plugin)
        openPluginWindow (channel);
    else
        openChannelSettings (engine, channel);

    openEditorRow = channel;
}

// --- step graph lanes (docs/WORKFLOW_FL.md P2, C4) --------------------------------------------------

juce::Rectangle<int> ChannelRackComponent::levelButtonBounds (int level) const
{
    const auto lane = laneBounds();
    const auto width = (gridLeft() - 24) / 3;
    return { 12 + (level % 3) * width, lane.getY() + 8 + (level / 3) * 24, width - 4, 20 };
}

void ChannelRackComponent::paintLane (juce::Graphics& g)
{
    using namespace theme::colour;
    const auto lane = laneBounds();

    if (lane.isEmpty() || ! g.getClipBounds().intersects (lane))
        return;

    const auto& song = engine.getSong();
    const auto& pattern = song.patterns[(std::size_t) song.currentPattern];
    const auto channel = graphRow();
    const auto level = (notetools::Level) graphLevel;

    g.setColour (bg0);
    g.fillRect (lane);
    g.setColour (border);
    g.drawHorizontalLine (lane.getY(), 0.0f, (float) getWidth());
    g.drawHorizontalLine (lane.getBottom() - 1, 0.0f, (float) getWidth());

    for (int l = 0; l < notetools::levelCount; ++l)
    {
        const auto r = levelButtonBounds (l).toFloat();
        theme::fillRoundRect (g, r, theme::radius::sm, l == graphLevel ? accent : bg4);
        g.setColour (l == graphLevel ? onAccent : textSecondary);
        g.setFont (theme::font (theme::type::caption, l == graphLevel));
        textcache::drawFittedText (g, tr (notetools::levelName ((notetools::Level) l)), r.toNearestInt(), juce::Justification::centred, 1, 0.8f);
    }

    g.setColour (textDisabled);
    g.setFont (theme::font (theme::type::caption));
    textcache::drawFittedText (g, tr ("Drag: set  ·  Right-drag: ramp  ·  Alt+click: reset"),
                      juce::Rectangle<int> (12, lane.getBottom() - 20, gridLeft() - 24, 16), juce::Justification::centredLeft, 1, 0.8f);

    const auto top = (float) lane.getY() + 6.0f, bottom = (float) lane.getBottom() - 6.0f;
    const auto bipolar = notetools::levelIsBipolar (level);
    const auto base = bipolar ? (top + bottom) * 0.5f : bottom;

    for (int step = 0; step < numSteps(); ++step)
    {
        const auto x = (float) (gridLeft() + step * stepWidth + 1);
        const auto w = (float) stepWidth - 3.0f;

        if (step % 4 == 0 && step > 0)
        {
            g.setColour (step % 16 == 0 ? gridBar : gridBeat);
            g.drawVerticalLine ((int) x - 2, top, bottom);
        }

        if (hoverArea == Area::lane && hoverStep == step)
        {
            g.setColour (bg3);
            g.fillRect (juce::Rectangle<float> (x, top, w, bottom - top));
        }

        const auto notes = notesInStep (pattern, channel, step);

        if (notes.empty())
        {
            g.setColour (theme::stepOff (step));
            g.fillRect (x + w * 0.5f - 1.0f, base - 1.0f, 2.0f, 2.0f);
            continue;
        }

        const auto v = notetools::levelValue (pattern.notes[(std::size_t) notes.front()], level);
        const auto y = bottom - v * (bottom - top);
        g.setColour ((level == notetools::Level::velocity ? accent : info).withAlpha (0.85f));
        g.fillRect (juce::Rectangle<float> (x + 2.0f, juce::jmin (y, base), w - 4.0f, juce::jmax (2.0f, std::abs (base - y))));
        g.setColour (textPrimary);
        g.fillRect (x + 1.0f, y - 1.0f, w - 2.0f, 2.0f);
    }

    if (bipolar)
    {
        g.setColour (gridBar);
        g.drawHorizontalLine ((int) base, (float) gridLeft(), (float) getWidth());
    }
}

float ChannelRackComponent::laneValueAt (int y) const
{
    const auto lane = laneBounds();
    const auto top = (float) lane.getY() + 6.0f, bottom = (float) lane.getBottom() - 6.0f;
    return juce::jlimit (0.0f, 1.0f, (bottom - (float) y) / juce::jmax (1.0f, bottom - top));
}

void ChannelRackComponent::setLaneValue (int step, float value)
{
    const auto channel = graphRow();
    const auto level = (notetools::Level) graphLevel;
    engine.editSong ([channel, step, value, level] (Song& s) {
        auto& p = s.patterns[(std::size_t) s.currentPattern];
        for (auto i : notesInStep (p, channel, step))
            notetools::setLevelValue (p.notes[(std::size_t) i], level, value);
    });
}

void ChannelRackComponent::applyLane (const juce::MouseEvent& e)
{
    const auto steps = numSteps();
    const auto step = juce::jlimit (0, steps - 1, (e.x - gridLeft()) / stepWidth);
    const auto value = laneValueAt (e.y);

    if (e.mods.isRightButtonDown()) // a straight ramp from the press to here
    {
        const auto from = juce::jmin (laneDownStep, step), to = juce::jmax (laneDownStep, step);
        const auto channel = graphRow();
        const auto level = (notetools::Level) graphLevel;
        const auto v0 = laneDownStep <= step ? laneDownValue : value, v1 = laneDownStep <= step ? value : laneDownValue;
        engine.editSong ([=] (Song& s) {
            auto& p = s.patterns[(std::size_t) s.currentPattern];
            for (int st = from; st <= to; ++st)
            {
                const auto t = to > from ? (float) (st - from) / (float) (to - from) : 0.0f;
                for (auto i : notesInStep (p, channel, st))
                    notetools::setLevelValue (p.notes[(std::size_t) i], level, v0 + (v1 - v0) * t);
            }
        });
        return;
    }

    setLaneValue (step, value);
}

void ChannelRackComponent::setPianoRollView (bool on)
{
    if (pianoRollView == on)
        return;
    pianoRollView = on;
    repaint();

    if (onFilterChanged) // the title's switches follow
        onFilterChanged();
}

void ChannelRackComponent::toggleGraphLanes()
{
    graphLanes = ! graphLanes;
    updateSize();
    repaint();

    if (onFilterChanged) // the title's graph switch follows
        onFilterChanged();
}

// --- fills, rotation and organisation ------------------------------------------------------------

void ChannelRackComponent::rotateSelected (int steps)
{
    const auto rows = engine.getSelectedChannels();
    engine.editSong ([rows, steps] (Song& s) {
        auto& p = s.patterns[(std::size_t) s.currentPattern];
        const auto length = patternSteps (s, p);
        for (auto r : rows)
            rotateSteps (p, r, steps, length);
    });
}

void ChannelRackComponent::fillSelected (int every)
{
    const auto rows = engine.getSelectedChannels();
    engine.editSong ([rows, every] (Song& s) {
        auto& p = s.patterns[(std::size_t) s.currentPattern];
        const auto length = patternSteps (s, p);
        for (auto r : rows)
            if (r < (int) s.channels.size() && s.channels[(std::size_t) r].instrument != InstrumentKind::audioClip)
                fillEverySteps (p, r, every, length);
    });
}

void ChannelRackComponent::sortChannels (ChannelSort by)
{
    engine.editSong ([by] (Song& s) { permuteChannels (s, channelSortOrder (s, by)); });
    openEditorRow = -1;
    selectRows ({ 0 }, 0);
}

void ChannelRackComponent::selectUnused()
{
    const auto unused = unusedChannels (engine.getSong());

    if (unused.empty())
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Select unused channels"),
                                                tr ("Every channel is used by a pattern or a Playlist clip."));
        return;
    }

    selectRows (unused, unused.front());
}

int ChannelRackComponent::addChannel (ChannelDef c, bool openEditor)
{
    int index = 0;
    if (filterGroup >= 0 && c.group < 0) // a new channel joins the group shown (FL)
        c.group = filterGroup;
    engine.editSong ([&] (Song& s) {
        c.mixerTrack = s.claimFreeInsert();
        s.channels.push_back (c);
        index = (int) s.channels.size() - 1;
    });
    selectionAnchor = index;
    selectRows ({ index }, index);

    if (openEditor && c.instrument == InstrumentKind::plugin)
    {
        openPluginWindow (index);
        openEditorRow = index;
    }
    else if (openEditor && natives::instrumentEngine (c.instrument) != nullptr) // an FL engine: its editor (round 142)
        openChannelSettings (engine, index);

    return index;
}

bool ChannelRackComponent::loadSampleIntoSelected (const juce::File& file)
{
    const auto row = engine.getSelectedChannel();
    const auto& song = engine.getSong();

    if (row < 0 || row >= (int) song.channels.size() || ! file.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
        return false;

    const auto kind = song.channels[(std::size_t) row].instrument;

    // only channels that play a sample (or the built-in drums, which become samplers) take a file
    if (kind != InstrumentKind::sampler && kind != InstrumentKind::kick && kind != InstrumentKind::snare && kind != InstrumentKind::hat
        && kind != InstrumentKind::clap && kind != InstrumentKind::granulizer)
        return false;

    assignSample (row, file);
    return true;
}

namespace
{
// Euclidean fill: pulses spread evenly over a length, rotated; previewed live on the pattern.
class EuclideanPanel final : public juce::Component
{
public:
    EuclideanPanel (AudioEngine& e, int c) : engine (e), channel (c)
    {
        const auto& song = engine.getSong();
        patternIndex = song.currentPattern;
        originals = song.patterns[(std::size_t) patternIndex].notes;
        steps = patternSteps (song, song.patterns[(std::size_t) patternIndex]);

        auto setup = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double lo, double hi, double v) {
            l.setText (name, juce::dontSendNotification);
            addAndMakeVisible (l);
            s.setSliderStyle (juce::Slider::LinearHorizontal);
            s.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
            s.setRange (lo, hi, 1.0);
            s.setValue (v, juce::dontSendNotification);
            s.onValueChange = [this] { preview(); };
            addAndMakeVisible (s);
        };
        setup (pulses, pulsesLabel, tr ("Pulses"), 0, 32, 5);
        setup (length, lengthLabel, tr ("Length (steps)"), 1, 32, 16);
        setup (rotation, rotationLabel, tr ("Rotation"), -16, 16, 0);
        title.setText (tr ("Euclidean fill: {0}", { juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str()) }), juce::dontSendNotification);
        title.setFont (theme::font (theme::type::title, true));
        addAndMakeVisible (title);

        for (auto* b : { &accept, &cancel })
            addAndMakeVisible (b);
        accept.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        accept.onClick = [this] {
            restore();
            const auto rhythm = current();
            engine.editSong ([this, rhythm] (Song& s) { setStepRhythm (s.patterns[(std::size_t) patternIndex], channel, rhythm, steps); });
            done = true;
            closeToolWindowOf (*this);
        };
        cancel.onClick = [this] { closeToolWindowOf (*this); };
        setSize (380, 330);
        preview();
    }

    ~EuclideanPanel() override
    {
        if (! done)
            restore();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        // the rhythm as a ring of steps, the first step at the top
        const auto rhythm = current();
        const auto ring = juce::Rectangle<float> (0, 150, (float) getWidth(), 120).withSizeKeepingCentre (110, 110);
        const auto n = (int) rhythm.size();

        for (int i = 0; i < n; ++i)
        {
            const auto angle = juce::MathConstants<float>::twoPi * (float) i / (float) n;
            const auto centre = ring.getCentre() + juce::Point<float> (std::sin (angle), -std::cos (angle)) * 50.0f;
            const auto dot = juce::Rectangle<float> (12, 12).withCentre (centre);
            g.setColour (rhythm[(std::size_t) i] ? theme::colour::stepOn : theme::colour::stepOff);
            g.fillEllipse (dot);
            if (i == 0)
            {
                g.setColour (theme::colour::textPrimary);
                g.drawEllipse (dot.expanded (2.0f), 1.0f);
            }
        }
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12, 10);
        title.setBounds (area.removeFromTop (24));
        area.removeFromTop (6);
        for (auto [s, l] : { std::pair { &pulses, &pulsesLabel }, std::pair { &length, &lengthLabel }, std::pair { &rotation, &rotationLabel } })
        {
            auto row = area.removeFromTop (30);
            l->setBounds (row.removeFromLeft (120));
            s->setBounds (row);
        }
        auto buttons = area.removeFromBottom (28);
        accept.setBounds (buttons.removeFromRight (100));
        buttons.removeFromRight (8);
        cancel.setBounds (buttons.removeFromRight (90));
    }

private:
    std::vector<bool> current() const
    {
        return euclideanRhythm ((int) pulses.getValue(), (int) length.getValue(), (int) rotation.getValue());
    }

    void restore()
    {
        if (patternIndex < (int) engine.getSong().patterns.size())
            engine.editSong ([this] (Song& s) { s.patterns[(std::size_t) patternIndex].notes = originals; }, false);
    }

    void preview()
    {
        const auto rhythm = current();
        if (patternIndex < (int) engine.getSong().patterns.size())
            engine.editSong ([this, rhythm] (Song& s) {
                auto& p = s.patterns[(std::size_t) patternIndex];
                p.notes = originals;
                setStepRhythm (p, channel, rhythm, steps);
            }, false);
        repaint();
    }

    AudioEngine& engine;
    int channel, patternIndex = 0, steps = 16;
    std::vector<Note> originals;
    bool done = false;
    juce::Label title, pulsesLabel, lengthLabel, rotationLabel;
    juce::Slider pulses, length, rotation;
    juce::TextButton accept { tr ("Accept") }, cancel { tr ("Cancel") };
};
} // namespace

std::unique_ptr<juce::Component> ChannelRackComponent::makeEuclideanPanel (int channel)
{
    return std::make_unique<EuclideanPanel> (engine, juce::jlimit (0, juce::jmax (0, (int) engine.getSong().channels.size() - 1), channel));
}

void ChannelRackComponent::showEuclideanFill (int channel)
{
    if (channel >= 0 && channel < (int) engine.getSong().channels.size())
        openToolWindow (makeEuclideanPanel (channel), tr ("Euclidean fill"), "euclidean");
}

// --- mouse -------------------------------------------------------------------------------------

void ChannelRackComponent::mouseDown (const juce::MouseEvent& e)
{
    int row = 0;
    const auto area = areaAt (e.x, e.y, row);
    const auto& song = engine.getSong();

    switch (area)
    {
        case Area::none:
            return;

        case Area::addRow:
            showAddChannelMenu();
            return;

        case Area::laneLevels:
            for (int l = 0; l < notetools::levelCount; ++l)
                if (levelButtonBounds (l).contains (e.getPosition().translated (-stickyX, 0)))
                {
                    graphLevel = l;
                    repaint();
                }
            return;

        case Area::lane:
        {
            const auto step = juce::jlimit (0, numSteps() - 1, (e.x - gridLeft()) / stepWidth);

            if (e.mods.isAltDown() || e.mods.isMiddleButtonDown())
            {
                setLaneValue (step, notetools::levelDefault ((notetools::Level) graphLevel));
                return;
            }

            laneDownStep = step;
            laneDownValue = laneValueAt (e.y);
            applyLane (e);
            return;
        }

        case Area::mute:
            if (e.mods.isPopupMenu() || e.mods.isCtrlDown())
                solo (row);
            else
                toggleMute (row);
            return;

        case Area::pan:
        case Area::volume:
        case Area::route:
        {
            const auto spec = specFor (area, row);

            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu menu;
                menu.addSectionHeader (spec.name + ": " + spec.text (valueOf (area, row)));
                ui::addValueItems (menu, *this, &engine, spec, valueOf (area, row), [this, area, row] (double v) { setValue (area, row, v); });

                if (area == Area::route)
                {
                    menu.addSeparator();
                    menu.addItem (tr ("Next free insert"), [this, row] {
                        engine.editSong ([row] (Song& s) { s.channels[(std::size_t) row].mixerTrack = s.claimFreeInsert(); });
                    });
                    menu.addItem (tr ("Send the selected channels to free inserts") + "\tCtrl+L", [this] { sendSelectedToInserts(); });

                    if (onOpenMixerInsert)
                        menu.addItem (tr ("Show this insert in the Mixer"), [this, row] {
                            if (row < (int) engine.getSong().channels.size())
                                onOpenMixerInsert (engine.getSong().channels[(std::size_t) row].mixerTrack);
                        });
                }

                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
                return;
            }

            if (ui::isResetClick (e))
            {
                setValue (area, row, spec.defaultValue);
                showTip (area, row, true);
                return;
            }

            knobDrag = area;
            knobRow = row;
            drag.begin (e, valueOf (area, row));
            showTip (area, row, false);
            repaint();
            return;
        }

        case Area::name:
            if (e.mods.isPopupMenu())
            {
                if (! std::binary_search (engine.getSelectedChannels().begin(), engine.getSelectedChannels().end(), row))
                    clickSelector (row, {});
                else
                    selectRows (engine.getSelectedChannels(), row);
                showChannelMenu (row);
                return;
            }

            if (e.mods.isShiftDown())
            {
                clickSelector (row, e.mods);
                return;
            }

            if (! std::binary_search (engine.getSelectedChannels().begin(), engine.getSelectedChannels().end(), row)
                || engine.getSelectedChannels().size() > 1)
            {
                selectionAnchor = row;
                selectRows ({ row }, row);
            }

            if (e.mods.isCtrlDown()) // audition without opening anything
            {
                previews.press (row, 60, 0.8f); // stops on release
                return;
            }

            // the editor opens on release, unless the button is dragged (that reorders the channels)
            nameDownRow = row;
            nameDragged = false;
            nameDownAlt = e.mods.isAltDown();
            nameDragY = e.position.y;
            return;

        case Area::select:
            clickSelector (row, e.mods);

            if (e.mods.isPopupMenu()) // right-drag adds the rows it crosses
            {
                selectDownRow = row;
                selectDragAdds = true;
                selectDragBase = engine.getSelectedChannels();
            }
            else if (! e.mods.isShiftDown() && ! e.mods.isCtrlDown()) // left-drag selects a range
            {
                selectDownRow = row;
                selectDragAdds = false;
            }
            return;

        case Area::grid:
        {
            const auto step = (e.x - gridLeft()) / stepWidth;
            const auto& pattern = song.patterns[(std::size_t) song.currentPattern];

            if (showsNotePreview (pattern, row, pianoRollView)) // the miniature opens the Piano Roll (steps would break the melody)
            {
                if (! e.mods.isPopupMenu())
                {
                    selectionAnchor = row;
                    selectRows ({ row }, row);
                    if (onOpenPianoRoll)
                        onOpenPianoRoll();
                }
                return;
            }

            if (e.mods.isRightButtonDown())
                paintMode = PaintMode::remove;
            else
                paintMode = pattern.hasStep (row, step) ? PaintMode::remove : PaintMode::add;

            lastPaintedRow = lastPaintedStep = -1;
            applyPaint (e);
            return;
        }
    }
}

void ChannelRackComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (knobDrag != Area::none && drag.isActive())
    {
        setValue (knobDrag, knobRow, drag.update (e, specFor (knobDrag, knobRow)));
        showTip (knobDrag, knobRow, false);
        return;
    }

    if (laneDownStep >= 0)
    {
        applyLane (e);
        hoverStep = juce::jlimit (0, numSteps() - 1, (e.x - gridLeft()) / stepWidth);
        hoverArea = Area::lane;
        const auto v = laneValueAt (e.y);
        const auto bipolar = notetools::levelIsBipolar ((notetools::Level) graphLevel);
        const auto shown = juce::roundToInt ((bipolar ? v * 2.0f - 1.0f : v) * 100.0f);
        const auto valueText = (bipolar && shown > 0 ? "+" : "") + juce::String (shown) + "%";
        const auto levelText = tr (notetools::levelName ((notetools::Level) graphLevel));
        ui::showValueTip (*this, controlBounds (Area::lane, graphRow()), levelText + ": " + valueText);
        ui::Hint live;
        live.text = channelName (engine.getSong(), graphRow()) + " - " + levelText;
        live.value = valueText;
        ui::postHint (live);
        repaint (laneBounds());
        return;
    }

    if (nameDownRow >= 0)
    {
        if (! nameDragged && std::abs (e.position.y - nameDragY) < 4.0f)
            return;

        nameDragged = true;
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        const auto count = (int) engine.getSong().channels.size();
        const auto target = juce::jlimit (0, count - 1, rowAtY (e.y));

        // move the selection one row at a time until the grabbed channel is under the mouse
        for (int guard = 0; guard < count && engine.getSelectedChannel() != target; ++guard)
            if (! moveSelected (target < engine.getSelectedChannel() ? -1 : 1))
                break;
        return;
    }

    if (selectDownRow >= 0)
    {
        const auto count = (int) engine.getSong().channels.size();
        const auto row = juce::jlimit (0, juce::jmax (0, count - 1), rowAtY (e.y));
        std::vector<int> rows;

        for (int r = juce::jmin (selectDownRow, row); r <= juce::jmax (selectDownRow, row); ++r)
            rows.push_back (r);

        if (selectDragAdds)
        {
            rows.insert (rows.end(), selectDragBase.begin(), selectDragBase.end());
            std::sort (rows.begin(), rows.end());
            rows.erase (std::unique (rows.begin(), rows.end()), rows.end());
        }
        else
            selectionAnchor = selectDownRow;

        if (rows != engine.getSelectedChannels())
            selectRows (rows, row);
        return;
    }

    if (paintMode != PaintMode::none)
        applyPaint (e);
}

void ChannelRackComponent::mouseUp (const juce::MouseEvent& e)
{
    previews.releaseAll(); // the audition (Ctrl+click) and the steps' previews stop with the button

    if (nameDownRow >= 0 && ! nameDragged && e.mouseWasClicked())
        toggleEditor (nameDownRow, nameDownAlt);

    if (nameDragged)
        setMouseCursor (juce::MouseCursor::NormalCursor);

    nameDownRow = -1;
    nameDragged = false;
    selectDownRow = -1;
    selectDragBase.clear();
    ui::hideValueTip();
    ui::clearPostedHint();
    paintMode = PaintMode::none;
    laneDownStep = -1;

    if (knobDrag != Area::none)
        repaint();

    knobDrag = Area::none;
    drag.end();
}

void ChannelRackComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    int row = 0;
    const auto area = areaAt (e.x, e.y, row);

    if (area == Area::pan || area == Area::volume)
    {
        setValue (area, row, specFor (area, row).defaultValue);
        showTip (area, row, true);
    }
    else if (area == Area::route)
    {
        if (onOpenMixerInsert)
            onOpenMixerInsert (engine.getSong().channels[(std::size_t) row].mixerTrack);
        else
            engine.editSong ([row] (Song& s) { s.channels[(std::size_t) row].mixerTrack = s.claimFreeInsert(); });
    }
    else if (area == Area::select && ! e.mods.isPopupMenu())
        selectAll();
    else if (area == Area::name && ! e.mods.isAnyModifierKeyDown() && onOpenPianoRoll)
        onOpenPianoRoll();
}

void ChannelRackComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    int row = 0;
    const auto area = areaAt (e.x, e.y, row);

    if (area == Area::pan || area == Area::volume || area == Area::route)
    {
        if (const auto v = ui::wheelValue (valueOf (area, row), e, wheel, specFor (area, row)))
        {
            setValue (area, row, *v);
            showTip (area, row, true);
        }
        return;
    }

    if ((area == Area::name || area == Area::select) && e.mods.isShiftDown()) // Shift+wheel moves the channel
    {
        const auto delta = (wheel.deltaY != 0.0f ? wheel.deltaY : -wheel.deltaX) > 0.0f ? -1 : 1;
        selectRows ({ row }, row);
        moveSelected (delta);
        return;
    }

    juce::Component::mouseWheelMove (e, wheel); // scroll the viewport
}

ui::Hint ChannelRackComponent::hintAt (juce::Point<int> p)
{
    int row = 0;
    const auto area = areaAt (p.x, p.y, row);
    const auto& song = engine.getSong();
    ui::Hint h;

    if (row >= 0 && row < (int) song.channels.size() && (area == Area::pan || area == Area::volume || area == Area::route))
    {
        const auto active = knobDrag != Area::none && drag.isActive();
        const auto shownArea = active ? knobDrag : area;
        const auto shownRow = active ? knobRow : row;
        h = ui::valueHint (specFor (shownArea, shownRow), valueOf (shownArea, shownRow),
                           shownArea == Area::route ? tr ("[LMB] drag or [Wheel] routes  [2xLMB] show in the Mixer  [RMB] next free insert")
                                                    : ui::valueGestures());
        return h;
    }

    const auto name = channelName (song, row);

    switch (area)
    {
        case Area::mute:
            h.text = name + " - " + tr ("Click: mute.  Ctrl+click or right-click: solo");
            break;
        case Area::name:
            h.text = name + " - " + tr ("[LMB] open/close the editor  [Alt]+[LMB] keep others open  [Ctrl]+[LMB] audition  [2xLMB] Piano Roll  [LMB] drag up/down reorders  [RMB] channel menu");
            // sample channels: where the pitch is (round 163, "getting to the sample pitching")
            if (row >= 0 && row < (int) song.channels.size()
                && (song.channels[(std::size_t) row].instrument == InstrumentKind::sampler || song.channels[(std::size_t) row].instrument == InstrumentKind::audioClip))
                h.text << "  " << tr ("(pitch: [RMB] › Pitch...)");
            h.hasMenu = true;
            // FL's hint bar names what the channel is ("Audio Clip", the plugin's name) on the right (REA round 143b)
            if (row >= 0 && row < (int) song.channels.size())
            {
                const auto& c = song.channels[(std::size_t) row];
                h.value = c.instrument == InstrumentKind::plugin && ! c.plugin.name.empty() ? juce::String::fromUTF8 (c.plugin.name.c_str())
                                                                                             : instrumentLabel (c.instrument);
            }
            break;
        case Area::select:
            h.text = name + " - " + tr ("Select: [LMB] one  [Ctrl]+[LMB] or [RMB] add/remove  [Shift]+[LMB] range  [LMB] drag range  [RMB] drag adds  [2xLMB] all  [Alt]+[Up]/[Down] moves the selection");
            break;
        case Area::grid:
            if (! song.patterns.empty() && showsNotePreview (song.patterns[(std::size_t) song.currentPattern], row, pianoRollView))
                h.text = name + " - " + tr ("Piano Roll notes: [LMB] opens them in the Piano Roll");
            else
                h.text = name + " - " + (hoverStep >= numSteps() ? tr ("After the pattern's end: [LMB] adds a step and makes the pattern longer")
                                                                : tr ("Steps: [LMB] drag draws (starting on a lit step erases)  [RMB] drag erases"));
            // and the step's position as bar:step:tick, like FL's hint bar
            if (hoverStep >= 0)
                h.value = juce::String (stepPositionText ((std::int64_t) hoverStep * ticksPerStep, song.beatsPerBar));
            break;
        case Area::addRow:
            h.text = tr ("Add a channel (or drop audio files on the rack)");
            break;
        case Area::laneLevels:
            h.text = tr ("Graph lane: choose what the bars edit (Ctrl+K hides the lane)");
            break;
        case Area::lane:
            h.text = name + " - " + tr ("Graph lane: drag sets the steps, right-drag draws a ramp, Alt+click resets");
            break;
        default:
            break;
    }

    return h;
}

void ChannelRackComponent::applyPaint (const juce::MouseEvent& e)
{
    const auto& song = engine.getSong();
    const auto row = rowAtY (e.y);
    const auto step = (e.x - gridLeft()) / stepWidth;

    if (laneBounds().contains (e.getPosition()))
        return;

    if (row < 0 || row >= (int) song.channels.size() || step < 0 || step >= shownSteps() || e.x < stickyX + gridLeft()
        || song.channels[(std::size_t) row].instrument == InstrumentKind::audioClip
        || showsNotePreview (song.patterns[(std::size_t) song.currentPattern], row, pianoRollView)) // drag-paint skips note rows
        return;

    if (row == lastPaintedRow && step == lastPaintedStep)
        return;

    lastPaintedRow = row;
    lastPaintedStep = step;
    const auto on = paintMode == PaintMode::add;

    // FL: setting a step is silent, stopped or playing (issue #11; Ctrl+click on the channel button auditions it)
    engine.editSong ([row, step, on] (Song& s) {
        auto& p = s.patterns[(std::size_t) s.currentPattern];
        setStep (p, row, step, on);
        // a ghost step of a pattern with a set length: the length grows to the bar holding it (automatic lengths
        // follow the notes by themselves)
        if (on)
            p.lengthTicks = lengthWithStep (p.lengthTicks, step, ticksPerStep, (std::int64_t) ppq * std::max (1, s.beatsPerBar));
    });
}

// --- menus -------------------------------------------------------------------------------------

void ChannelRackComponent::showChannelMenu (int channel)
{
    const auto& c = engine.getSong().channels[(std::size_t) channel];
    juce::PopupMenu instruments;

    for (auto kind : builtInKinds)
        instruments.addItem (instrumentLabel (kind), true, c.instrument == kind, [this, channel, kind] {
            engine.editSong ([channel, kind] (Song& s) { s.channels[(std::size_t) channel].instrument = kind; });
        });

    instruments.addSeparator();
    instruments.addItem (tr ("Sampler (choose file)..."), [this, channel] { chooseSample (channel); });

    juce::PopupMenu plugins;
    addPluginItems (plugins, engine, true, [this, channel] (PluginRef ref) {
        engine.editSong ([channel, ref] (Song& s) {
            auto& c = s.channels[(std::size_t) channel];
            c.instrument = InstrumentKind::plugin;
            c.plugin = ref;
            c.name = ref.name;
        });
        openPluginWindow (channel);
    }, this);
    instruments.addSubMenu (tr ("Plugin (VST/VST3/CLAP)"), plugins);

    // FL's layout (REA round 104): two columns, the channel on the left (editors, name and colour, sample,
    // insert / replace / clone / delete) and its steps on the right (clipboard, fills, rotation); LYNX's own
    // items (settings windows, arpeggiator, organisation) follow in their place
    juce::PopupMenu menu;
    ui::addItemHeader (menu, tr ("Channel"), juce::String::fromUTF8 (c.name.c_str()), c.colour != 0 ? juce::Colour (c.colour) : juce::Colour());
    const auto selected = (int) engine.getSelectedChannels().size();
    const auto count = (int) engine.getSong().channels.size();

    menu.addItem (tr ("Piano Roll") + "\tF7", [this] {
        if (onOpenPianoRoll)
            onOpenPianoRoll();
    });
    menu.addItem (tr ("Graph editor") + "\tCtrl+K", true, graphLanes, [this] { toggleGraphLanes(); });
    menu.addSeparator();
    menu.addItem (tr ("Rename and color...") + "\tF2", [this, channel] { rename (channel); });
    menu.addSubMenu (tr ("Change color"), ui::colourMenu (c.colour, [this, channel] (std::uint32_t colour) {
                         if (channel < (int) engine.getSong().channels.size())
                             engine.editSong ([channel, colour] (Song& s) { s.channels[(std::size_t) channel].colour = colour; });
                     }, juce::Rectangle<int> (juce::Desktop::getMousePosition().x, juce::Desktop::getMousePosition().y, 1, 1)));
    menu.addItem (tr ("Random color"), [this, channel] {
        const auto& palette = lynxPalette();
        const auto colour = palette[(std::size_t) juce::Random::getSystemRandom().nextInt ((int) palette.size())];
        engine.editSong ([channel, colour] (Song& s) { s.channels[(std::size_t) channel].colour = colour; });
    });
    menu.addSeparator();
    menu.addItem (tr ("Load sample..."), c.instrument != InstrumentKind::audioClip, false, [this, channel] { chooseSample (channel); });
    {
        // "Cut itself": a new note cuts the channel's own sounding note (its cut group and "cut by" are its own)
        const auto self = 1000 + channel;
        const auto cutsItself = c.cutGroup != 0 && c.cutGroup == c.cutBy;
        menu.addItem (tr ("Cut itself"), true, cutsItself, [this, channel, self, cutsItself] {
            engine.editSong ([channel, self, cutsItself] (Song& s) {
                auto& ch = s.channels[(std::size_t) channel];
                ch.cutGroup = ch.cutBy = cutsItself ? 0 : self;
            });
        });
    }
    menu.addSeparator();
    {
        juce::PopupMenu insert;
        addInstrumentItems (insert, channel);
        menu.addSubMenu (tr ("Insert"), insert);
    }
    menu.addSubMenu (tr ("Replace"), instruments);
    // FL's Patcherize (round 165m): the channel's instrument into a new Patcher, same sound
    menu.addItem (tr ("Patcherize"), selected <= 1 && patcherize::canPatcherizeChannel (engine.getSong(), channel), false, [this, channel] {
        engine.capturePluginStates(); // (the plugin's current settings go with it)
        closeEditor (channel);
        engine.editSong ([channel] (Song& s) { patcherize::patcherizeChannel (s, channel); });
    });
    menu.addItem (selected > 1 ? tr ("Clone {0} channels", { juce::String (selected) }) : tr ("Clone") + "\tAlt+C", [this] { cloneSelected(); });

    if (selected > 1)
        menu.addItem (tr ("Delete {0} channels", { juce::String (selected) }) + "\tAlt+Del", selected < count, false, [this] { deleteSelected(); });
    else
        menu.addItem (tr ("Delete...") + "\tAlt+Del", count > 1, false, [this, channel] {
            closeEditor (channel);
            engine.editSong ([channel] (Song& s) { s.removeChannel (channel); });
            openEditorRow = -1;
        });

    // LYNX: the channel's settings windows and its arpeggiator
    menu.addSeparator();
    if (c.instrument == InstrumentKind::sampler)
    {
        menu.addItem (tr ("Sampler settings (pitch, envelope, reverse)..."),
                      [this, channel] { openChannelSettings (engine, channel); });
        menu.addItem (tr ("Pitch... (semitones, cents, keep length)"), ! c.samplePath.empty() || ! c.zones.empty(), false,
                      [this, channel] { showSamplePitchDialog (engine, channel); });
        menu.addItem (tr ("Edit the sample in the audio editor..."), ! c.samplePath.empty(), false, [this, channel] { openAudioEditorForChannel (engine, channel); });

        if (c.sampler.slices.empty())
            menu.addItem (tr ("Slice sample (slicer): one slice per key and a pattern with the loop"), [this, channel] { sliceChannel (channel); });
        else
            menu.addItem (tr ("Undo slices ({0})", { juce::String ((int) c.sampler.slices.size()) }), [this, channel] {
                engine.editSong ([channel] (Song& s) { s.channels[(std::size_t) channel].sampler.slices.clear(); });
            });
    }
    else if (c.instrument == InstrumentKind::audioClip)
        menu.addItem (tr ("Pitch... (semitones, cents, keep length)"), ! c.samplePath.empty(), false,
                      [this, channel] { showSamplePitchDialog (engine, channel); });
    else if (c.instrument == InstrumentKind::synth)
        menu.addItem (tr ("Synth settings (oscillators, envelope, filter)..."), [this, channel] { openChannelSettings (engine, channel); });
    else if (hasChannelSettings (c.instrument))
        menu.addItem (tr ("Channel settings..."), [this, channel] { openChannelSettings (engine, channel); });
    // FL's wrench tab of the channel window, for every channel (REA round 143b)
    menu.addItem (tr ("Miscellaneous functions (polyphony, cut groups, arpeggiator)..."), [this, channel] { openChannelMisc (engine, channel); });

    if (c.instrument == InstrumentKind::plugin)
    {
        menu.addItem (tr ("Open plugin editor"), [this, channel] { openPluginWindow (channel); });

        auto* live = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (channel).get());
        menu.addSubMenu (tr ("Processing"), pluginProcessingMenu (engine, c.plugin, live != nullptr ? &live->getInstance() : nullptr,
                                                                [channel] (Song& song) -> PluginRef* {
                                                                    if (channel >= (int) song.channels.size())
                                                                        return nullptr;
                                                                    return &song.channels[(std::size_t) channel].plugin;
                                                                }));

        if (live != nullptr)
            addPluginParameterItems (menu, engine, live->getInstance(), { AutomationTargetKind::channelParam, channel, 0, 0 });
    }
    {
        // channel arpeggiator
        const auto arp = c.arp;
        auto set = [this, channel] (std::function<void (ArpSettings&)> change) {
            engine.editSong ([channel, change] (Song& s) { change (s.channels[(std::size_t) channel].arp); });
        };
        juce::PopupMenu arpMenu;
        const char* directions[] = { TR_NOOP ("Off"), TR_NOOP ("Up"), TR_NOOP ("Down"), TR_NOOP ("Up and down"), TR_NOOP ("Random") };

        for (int d = 0; d < 5; ++d)
            arpMenu.addItem (tr (directions[d]), true, arp.direction == d, [set, d] { set ([d] (ArpSettings& a) { a.direction = d; }); });

        arpMenu.addSeparator();
        juce::PopupMenu range, speed, gate, repeat;

        for (int o = 1; o <= 4; ++o)
            range.addItem (o == 1 ? tr ("1 octave") : tr ("{0} octaves", { juce::String (o) }), true, arp.range == o, [set, o] { set ([o] (ArpSettings& a) { a.range = o; }); });

        const std::pair<const char*, float> speeds[] = { { "1/4", 1.0f }, { "1/8", 0.5f }, { TR_NOOP ("1/8 triplet"), 1.0f / 3.0f },
                                                          { "1/16", 0.25f }, { TR_NOOP ("1/16 triplet"), 1.0f / 6.0f }, { "1/32", 0.125f } };
        for (const auto& [name, beats] : speeds)
            speed.addItem (tr (name), true, std::abs (arp.stepBeats - beats) < 1e-4f, [set, b = beats] { set ([b] (ArpSettings& a) { a.stepBeats = b; }); });

        for (int g : { 25, 50, 75, 100 })
            gate.addItem (juce::String (g) + "%", true, std::abs (arp.gate - g / 100.0f) < 1e-4f, [set, g] { set ([g] (ArpSettings& a) { a.gate = g / 100.0f; }); });

        for (int n : { 1, 2, 3, 4 })
            repeat.addItem (juce::String (n) + "x", true, arp.repeat == n, [set, n] { set ([n] (ArpSettings& a) { a.repeat = n; }); });

        arpMenu.addSubMenu (tr ("Range"), range);
        arpMenu.addSubMenu (tr ("Speed"), speed);
        arpMenu.addSubMenu (tr ("Gate"), gate);
        arpMenu.addSubMenu (tr ("Repeat"), repeat);
        menu.addSubMenu (arp.enabled() ? tr ("Arpeggiator (on)") : tr ("Arpeggiator"), arpMenu);
    }

    // --- second column: the channel's steps in the current pattern ---
    menu.addColumnBreak();
    {
        auto notesOf = [this, channel] {
            std::vector<Note> out;
            const auto& song = engine.getSong();
            for (const auto& n : song.patterns[(std::size_t) song.currentPattern].notes)
                if (n.channel == channel)
                    out.push_back (n);
            return out;
        };
        auto clear = [channel] (Song& s) {
            auto& notes = s.patterns[(std::size_t) s.currentPattern].notes;
            notes.erase (std::remove_if (notes.begin(), notes.end(), [channel] (const Note& n) { return n.channel == channel; }), notes.end());
        };
        const auto hasNotes = ! notesOf().empty();
        menu.addItem (tr ("Cut"), hasNotes, false, [this, notesOf, clear] {
            stepClipboard = notesOf();
            engine.editSong (clear);
        });
        menu.addItem (tr ("Copy"), hasNotes, false, [notesOf] { stepClipboard = notesOf(); });
        menu.addItem (tr ("Paste"), ! stepClipboard.empty(), false, [this, channel, clear] {
            engine.editSong ([channel, clear, notes = stepClipboard] (Song& s) {
                clear (s);
                auto& target = s.patterns[(std::size_t) s.currentPattern].notes;
                for (auto n : notes)
                {
                    n.channel = channel;
                    target.push_back (n);
                }
                std::stable_sort (target.begin(), target.end(), [] (const Note& a, const Note& b) { return a.start < b.start; });
            });
        });
    }
    menu.addSeparator();
    menu.addItem (tr ("Fill each 2 steps"), [this] { fillSelected (2); });
    menu.addItem (tr ("Fill each 4 steps"), [this] { fillSelected (4); });
    menu.addItem (tr ("Fill each 8 steps"), [this] { fillSelected (8); });
    menu.addItem (tr ("Advanced fill..."), [this, channel] { showEuclideanFill (channel); });
    menu.addSeparator();
    menu.addItem (tr ("Rotate left") + "\tShift+Ctrl+Left", [this] { rotateSelected (-1); });
    menu.addItem (tr ("Rotate right") + "\tShift+Ctrl+Right", [this] { rotateSelected (1); });
    menu.addSeparator();
    menu.addItem (tr ("Swing mix ({0})...", { juce::String (juce::roundToInt (c.swingMix * 100.0f)) + "%" }),
                  [this, channel] { setSwingMixOfSelected (channel); });
    menu.addItem (tr ("Clear notes in this pattern"), [this, channel] {
        engine.editSong ([channel] (Song& s) {
            auto& notes = s.patterns[(std::size_t) s.currentPattern].notes;
            notes.erase (std::remove_if (notes.begin(), notes.end(), [channel] (const Note& n) { return n.channel == channel; }),
                         notes.end());
        });
    });

    // organisation (acts on the selection; FL keeps these in the rack's options menu)
    menu.addSeparator();
    menu.addItem (tr ("Move up") + "\tAlt+Up", engine.getSelectedChannels().front() > 0, false, [this] { moveSelected (-1); });
    menu.addItem (tr ("Move down") + "\tAlt+Down", engine.getSelectedChannels().back() < count - 1, false, [this] { moveSelected (1); });
    menu.addItem (tr ("Send to free mixer inserts") + "\tCtrl+L", [this] { sendSelectedToInserts(); });
    {
        juce::PopupMenu sort;
        sort.addItem (tr ("By name"), [this] { sortChannels (ChannelSort::name); });
        sort.addItem (tr ("By color"), [this] { sortChannels (ChannelSort::colour); });
        sort.addItem (tr ("By mixer insert"), [this] { sortChannels (ChannelSort::insert); });
        sort.addItem (tr ("By type"), [this] { sortChannels (ChannelSort::type); });
        menu.addSubMenu (tr ("Sort channels"), sort);
    }
    menu.addItem (tr ("Select unused channels"), [this] { selectUnused(); });
    menu.addItem (tr ("Group selected...") + "\tAlt+G", [this] { groupSelected(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::String flEngineName (const natives::InstrumentEngine& e)
{
    const auto name = juce::String::fromUTF8 (e.name);
    if (! natives::available (e, flStudioAccess()))
        return tr ("{0} (requires FL Studio)", { name });
    return e.beta ? tr ("{0} (beta)", { name }) : name;
}

void addFlEngineItems (juce::PopupMenu& menu, std::function<void (ChannelDef)> add)
{
    for (const auto& e : natives::instrumentEngines())
        menu.addItem (flEngineName (e), natives::available (e, flStudioAccess()), false, [kind = e.kind, add] {
            if (auto c = natives::makeChannel (kind, flStudioAccess()))
                add (*c);
            else
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("FL engines"),
                                                        tr ("This engine needs the data files of an FL Studio installation, which were not found or could not be read."));
        });
}

void ChannelRackComponent::addInstrumentItems (juce::PopupMenu& menu, int insertAt)
{
    // a new channel of `kind`, at the end (insertAt < 0) or at a row
    auto add = [this, insertAt] (ChannelDef c) {
        int index = 0;
        if (filterGroup >= 0 && c.group < 0)
            c.group = filterGroup;
        engine.editSong ([&] (Song& s) {
            c.mixerTrack = s.claimFreeInsert();
            index = insertAt < 0 ? (s.channels.push_back (c), (int) s.channels.size() - 1) : insertChannel (s, insertAt, c);
        });
        selectionAnchor = index;
        selectRows ({ index }, index);
        return index;
    };

    for (auto kind : builtInKinds)
        menu.addItem (instrumentLabel (kind), [add, kind] {
            ChannelDef c;
            c.name = instrumentName (kind).toStdString();
            c.instrument = kind;
            add (c);
        });

    menu.addSeparator();
    // built-in controllers (round 133, issue #3): no sound of their own
    menu.addItem (tr ("Envelope Controller"), [this, add] {
        ChannelDef c;
        c.name = tr ("Envelope Controller").toStdString();
        c.instrument = InstrumentKind::controller;
        c.controllerState = writeEnvelopeController (defaultEnvelopeController());
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    menu.addItem (tr ("MIDI Out"), [this, add] {
        ChannelDef c;
        c.name = "MIDI Out";
        c.instrument = InstrumentKind::midiOut;
        midictl::MidiOutSettings settings;
        // FL gives each new MIDI Out the next MIDI channel, skipping the drum channel 10
        int count = 0;
        for (const auto& ch : engine.getSong().channels)
            count += ch.instrument == InstrumentKind::midiOut;
        settings.channel = count % 15 >= 9 ? count % 15 + 1 : count % 15;
        midictl::writeMidiOut (c.pluginState, settings);
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's Layer (round 164): plays its children (its settings open: select channels, then Set children)
    menu.addItem (tr ("Layer"), [this, add] {
        ChannelDef c;
        c.name = tr ("Layer").toStdString();
        c.instrument = InstrumentKind::layer;
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's Dashboard (round 164): a panel of knobs / sliders / buttons, each an output to link to any knob
    menu.addItem ("Dashboard", [this, add] {
        ChannelDef c;
        c.name = "Dashboard";
        c.instrument = InstrumentKind::dashboard;
        c.patcher.surface = surface::starterControls();
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's Voltage Controller (round 164): its notes as control voltages for modular gear
    menu.addItem ("Voltage Controller", [this, add] {
        ChannelDef c;
        c.name = "Voltage Controller";
        c.instrument = InstrumentKind::voltage;
        c.pluginState = voltage::write (voltage::Settings {});
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's Fruity Dance (round 165e): LYNX's own dancer over the workspace, its notes pick the moves (no sound)
    menu.addItem ("Fruity Dance", [this, add] {
        ChannelDef c;
        c.name = "Fruity Dance";
        c.instrument = InstrumentKind::fruitydance;
        c.pluginState = dance::toState (dance::Settings {});
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's Fruity Video Player (round 165g): a video in sync with the song, decoded by the user's ffmpeg
    menu.addItem ("Fruity Video Player", [this, add] {
        ChannelDef c;
        c.name = "Fruity Video Player";
        c.instrument = InstrumentKind::videoplayer;
        c.pluginState = video::write (video::Settings {});
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    // FL's own instrument engines (round 142, issue #5): their editor opens
    {
        juce::PopupMenu engines;
        addFlEngineItems (engines, [this, add] (ChannelDef c) {
            const auto added = add (c);
            openChannelSettings (engine, added);
        });
        menu.addSubMenu (tr ("FL engines"), engines);
    }
    // FL's VFX note processors (round 164b): no sound of their own, their outputs go out on MIDI ports
    {
        juce::PopupMenu vfxMenu;
        for (const auto kind : vfx::allKinds)
            vfxMenu.addItem (vfx::flName (kind), [this, add, kind] {
                ChannelDef c;
                vfx::makeChannel (c, kind);
                const auto added = add (c);
                openChannelSettings (engine, added);
            });
        menu.addSubMenu (tr ("VFX (note effects)"), vfxMenu);
    }
    // FL's Patcher as a generator (REA_rodada141): an empty graph, its editor opens
    menu.addItem (tr ("Patcher"), [this, add] {
        ChannelDef c;
        c.name = "Patcher";
        c.instrument = InstrumentKind::patcher;
        const auto added = add (c);
        PatcherWindows::openChannel (engine, added);
    });
    // LYNX Soundfont (round 158, issue #6): SF2 / SF3 banks with a preset search; its editor opens to pick a font
    menu.addItem ("LYNX Soundfont", [this, add] {
        const auto added = add (sf::makeChannel());
        openChannelSettings (engine, added);
    });
    menu.addSeparator();

    if (insertAt < 0)
        menu.addItem (tr ("Sampler (choose file)..."), [this] { chooseSample (-1); });

    juce::PopupMenu plugins;
    addPluginItems (plugins, engine, true, [this, add] (PluginRef ref) {
        ChannelDef c;
        c.name = ref.name;
        c.instrument = InstrumentKind::plugin;
        c.plugin = ref;
        const auto added = add (c);
        openPluginWindow (added); // the plugin opens when added
        openEditorRow = added;
    }, this, [this, add] (ChannelDef c) {
        const auto added = add (c);
        openChannelSettings (engine, added);
    });
    menu.addSubMenu (tr ("Plugin (VST/VST3/CLAP)"), plugins);
}

void ChannelRackComponent::showAddChannelMenu (int insertAt)
{
    juce::PopupMenu menu;
    // FL's renamed plugins first (issue #11): "1_Synth" renamed in the Browser's Plugin database is added by typing 1
    std::vector<quickpick::Pick> picks;
    if (insertAt < 0)
        picks = quickpick::renamedGenerators (engine, [safe = juce::Component::SafePointer<ChannelRackComponent> (this)] (ChannelDef c) {
            if (safe != nullptr)
                safe->addChannel (std::move (c), true);
        });
    quickpick::addPicks (menu, picks);
    addInstrumentItems (menu, insertAt);
    quickpick::showMenu (menu, juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), std::move (picks));
}

void ChannelRackComponent::chooseSample (int channel)
{
    fileChooser = std::make_unique<juce::FileChooser> (tr ("Choose sample"), juce::File(), engine.getSampleLibrary().wildcard());
    fileChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, channel] (const juce::FileChooser& chooser) {
                                  if (chooser.getResult().existsAsFile())
                                      assignSample (channel, chooser.getResult());
                              });
}

void ChannelRackComponent::assignSample (int channel, const juce::File& file)
{
    if (engine.getSampleLibrary().load (file.getFullPathName()) == nullptr)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Sample"),
                                                tr ("Could not read {0}", { file.getFileName() }));
        return;
    }

    const auto path = file.getFullPathName().toStdString();
    const auto name = file.getFileNameWithoutExtension().toStdString();

    engine.editSong ([channel, path, name] (Song& s) {
        const auto isNew = channel < 0 || channel >= (int) s.channels.size();
        const auto freeInsert = s.claimFreeInsert();
        auto& c = isNew ? s.channels.emplace_back() : s.channels[(std::size_t) channel];

        if (isNew)
            c.mixerTrack = freeInsert;
        c.samplePath = path;
        if (! isNew && c.instrument == InstrumentKind::granulizer) // FL's Granulizer keeps its engine and plays the file (round 142)
            return;
        c.instrument = InstrumentKind::sampler;
        c.name = name;
    });

    if (channel < 0)
        engine.setSelectedChannel ((int) engine.getSong().channels.size() - 1);
}

void ChannelRackComponent::openPluginWindow (int channel)
{
    auto instrument = engine.getChannelInstrument (channel);

    if (auto* plugin = dynamic_cast<PluginInstrument*> (instrument.get()))
    {
        if (plugin->hasCrashed()) // (not called again: its window would call into it)
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Plugin"),
                                                    tr ("This plugin crashed and was switched off. Save the project and open it again to reload it."));
            return;
        }
        PluginWindows::open (instrument, plugin->getInstance(),
                             juce::String::fromUTF8 (engine.getSong().channels[(std::size_t) channel].name.c_str()),
                             { &engine, [channel] (Song& song) -> PluginRef* {
                                  return channel < (int) song.channels.size() ? &song.channels[(std::size_t) channel].plugin : nullptr;
                              }, true });
        return;
    }

    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Plugin"),
                                            tr ("This plugin could not be loaded (it is not installed or has not been scanned)."));
}

void ChannelRackComponent::rename (int channel)
{
    const auto& c = engine.getSong().channels[(std::size_t) channel];
    showNameAndColourDialog (tr ("Rename channel"), juce::String::fromUTF8 (c.name.c_str()), c.colour,
                             [this, channel] (const juce::String& name, std::uint32_t colour) {
                                 if (channel < (int) engine.getSong().channels.size())
                                     engine.editSong ([channel, n = name.toStdString(), colour] (Song& s) {
                                         s.channels[(std::size_t) channel].name = n;
                                         s.channels[(std::size_t) channel].colour = colour;
                                     });
                             });
}

void ChannelRackComponent::sliceChannel (int channel)
{
    const auto& song = engine.getSong();
    const auto& c = song.channels[(std::size_t) channel];
    const auto sample = engine.getSampleLibrary().load (juce::String::fromUTF8 (c.samplePath.c_str()));

    if (sample == nullptr || sample->frames() == 0)
        return;

    const auto slices = detectSlices (*sample, 0.5f);
    const auto seconds = (double) sample->frames() / sample->sampleRate;
    const auto ticksPerSecond = song.bpm * ppq / 60.0;
    const auto name = c.name + " (slices)";

    engine.editSong ([&] (Song& s) {
        s.channels[(std::size_t) channel].sampler.slices = slices;

        // pattern that plays the slices in their original order and timing (at the project BPM)
        auto& p = s.patterns.emplace_back();
        p.name = name;

        for (std::size_t i = 0; i < slices.size(); ++i)
        {
            const auto start = slices[i] * seconds;
            const auto end = i + 1 < slices.size() ? slices[i + 1] * seconds : seconds;
            Note n;
            n.start = (std::int64_t) std::llround (start * ticksPerSecond);
            n.length = std::max<std::int64_t> (1, (std::int64_t) std::llround ((end - start) * ticksPerSecond));
            n.key = 60 + (int) i;
            n.channel = channel;
            p.notes.push_back (n);
        }

        p.sortNotes();
        s.currentPattern = (int) s.patterns.size() - 1;
    });
}

bool ChannelRackComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;sf2;sf3"))
            return true;

    return false;
}

void ChannelRackComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    int row = 0;
    const auto area = areaAt (x, y, row);
    auto target = (area == Area::none || area == Area::addRow) ? -1 : row;

    for (const auto& f : files)
    {
        const juce::File file (f);

        if (file.hasFileExtension ("sf2;sf3"))
        {
            // a SoundFont: into the LYNX Soundfont channel it was dropped on, else a new channel
            openSoundFontFile (engine, file, target);
            target = -1;
            continue;
        }
        if (! file.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
            continue;

        assignSample (target, file);
        target = -1; // the remaining files become new channels
    }
}

void ChannelRackComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    updateSize();
    repaint();
}

void ChannelRackComponent::timerCallback()
{
    auto& transport = engine.getTransport();
    auto step = -1;

    if (transport.playing.load() && engine.getSong().mode == PlayMode::pattern)
    {
        const auto& song = engine.getSong();
        const auto& p = song.patterns[(std::size_t) song.currentPattern];
        const auto length = (double) p.effectiveLength (song.beatsPerBar);
        step = (int) (std::fmod (transport.positionTicks.load(), length) / ticksPerStep);
    }

    if (step != playheadStep)
    {
        auto dirty = [this] (int s) {
            if (s >= 0)
                repaint (gridLeft() + s * stepWidth, 0, stepWidth, getHeight());
        };
        dirty (playheadStep);
        playheadStep = step;
        dirty (playheadStep);
    }
}

bool ChannelRackComponent::isInterestedInDragSource (const SourceDetails& details)
{
    std::string path;
    int bank = 0, program = 0;
    return BrowserComponent::isBrowserDrag (details) || BrowserComponent::isGeneratorDrag (details)
           || parseSoundFontDrag (details.description.toString(), path, bank, program);
}

void ChannelRackComponent::itemDropped (const SourceDetails& details)
{
    // a generator dragged from the Browser's Plugin database (round 162): a new channel with it, its window open
    if (BrowserComponent::isGeneratorDrag (details))
    {
        if (auto c = BrowserComponent::draggedGenerator (details))
            addChannel (std::move (*c), true);
        return;
    }

    // a preset dragged from a LYNX Soundfont list: onto a LYNX Soundfont channel it loads there, else a new channel
    std::string path;
    int bank = 0, program = 0;
    if (parseSoundFontDrag (details.description.toString(), path, bank, program))
    {
        int row = 0;
        const auto area = areaAt (details.localPosition.x, details.localPosition.y, row);
        const auto& song = engine.getSong();
        if (area != Area::none && area != Area::addRow && row >= 0 && row < (int) song.channels.size()
            && song.channels[(std::size_t) row].instrument == InstrumentKind::soundfont)
            loadSoundFontInto (engine, row, path, bank, program);
        else
            addSoundFontChannel (engine, path, bank, program);
        return;
    }
    if (const auto file = BrowserComponent::draggedFile (details); file.existsAsFile())
        filesDropped ({ file.getFullPathName() }, details.localPosition.x, details.localPosition.y);
}
} // namespace lynx
