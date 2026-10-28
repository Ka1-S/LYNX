#include "ui/PianoRollComponent.h"
#include "core/KeyMap.h"
#include "core/NoteNames.h"

#include "core/TransportEdit.h"
#include "ui/UiTrace.h"
#include "ui/TextCache.h"

#include "core/EditorPolish.h"
#include "core/EventEdit.h"
#include "core/NoteEdit.h"
#include "core/NoteTools.h"
#include "engine/MidiFileIo.h"
#include "ui/Dialogs.h"
#include "engine/Text.h"
#include "ui/AutomationUi.h"
#include "ui/EventEditor.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
bool isBlackKey (int key)
{
    const auto n = key % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// FL-style name: key 60 = C5.
juce::String keyName (int key)
{
    return juce::String (notenames::keyName (key)); // Settings › General › Note names (round 165f)
}

const Pattern& currentPattern (const Song& s)
{
    return s.patterns[(std::size_t) s.currentPattern];
}

Pattern& currentPattern (Song& s)
{
    return s.patterns[(std::size_t) s.currentPattern];
}

// "\t" + the key map's shortcut of `id` for a menu item (empty when unbound or no key map is connected)
juce::String keyText (const std::string& id)
{
    return PianoRollComponent::shortcutText ? PianoRollComponent::shortcutText (id) : juce::String();
}

struct NamedIntervals
{
    const char* name;
    std::vector<int> intervals;
};

const std::vector<NamedIntervals>& chords()
{
    static const std::vector<NamedIntervals> list {
        { TR_NOOP ("Note"), { 0 } }, { TR_NOOP ("Major"), { 0, 4, 7 } }, { TR_NOOP ("Minor"), { 0, 3, 7 } },     { "Dim", { 0, 3, 6 } },
        { "Aug", { 0, 4, 8 } },    { "Sus2", { 0, 2, 7 } },      { "Sus4", { 0, 5, 7 } },      { "7", { 0, 4, 7, 10 } },
        { "Maj7", { 0, 4, 7, 11 } }, { "m7", { 0, 3, 7, 10 } }, { "m9", { 0, 3, 7, 10, 14 } }, { TR_NOOP ("Octave"), { 0, 12 } },
    };
    return list;
}

const std::vector<notetools::ScaleInfo>& scales()
{
    return notetools::scales();
}

} // namespace


PianoRollComponent::PianoRollComponent (AudioEngine& e)
    : engine (e)
{
    auto sender = [this] (int channel, int key, float velocity, bool on) { engine.sendLiveNote (channel, key, velocity, on); };
    previewNotes.setSender (sender);
    listenNotes.setSender (sender);
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    for (auto* bar : { &hBar, &vBar })
    {
        bar->addListener (this);
        bar->setAutoHide (false);
        addAndMakeVisible (bar);
    }

    // the overview strip is the horizontal scroller (FL's Piano Roll has its preview bar on top)
    hBar.setVisible (false);
    overview.paintContent = [this] (juce::Graphics& g, juce::Rectangle<float> area, double total) { paintOverview (g, area, total); };
    overview.onChange = [this] (double start, double length) {
        const auto width = (double) gridArea().getWidth();
        pixelsPerTick = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, width / juce::jmax (1.0, length));
        scrollTicks = juce::jmax (0.0, start);
        updateScrollBars();
        repaint();
    };
    overview.onShowAll = [this] { zoomAll(); };
    addAndMakeVisible (overview);

    grid::fillEditorBox (snapBox);
    snapBox.setTooltip (tr ("Piano Roll grid: follows the global grid in the toolbar, or its own; hold Alt while dragging to ignore it"));
    snapBox.setWantsKeyboardFocus (false);
    snapBox.setMouseClickGrabsKeyboardFocus (false);
    snapBox.onChange = [this] { repaint(); };
    addAndMakeVisible (snapBox);

    tools.onChange = [this] (EditTool) {
        stopListening();
        updateCursor (juce::ModifierKeys::currentModifiers);
    };
    addAndMakeVisible (tools);

    channelBox.setTooltip (tr ("Channel being edited (marked: has notes in this pattern); \"All\" at the top edits every channel's notes at once. Wheel: channels with notes; Ctrl+wheel: all"));
    channelBox.setWantsKeyboardFocus (false);
    channelBox.setMouseClickGrabsKeyboardFocus (false);
    channelBox.onChange = [this] {
        if (channelBox.getSelectedId() == allChannelsId)
        {
            setAllChannels (true);
            return;
        }
        const auto channel = channelBox.getSelectedId() - 1;
        setAllChannels (false);
        if (channel >= 0 && channel < (int) engine.getSong().channels.size() && channel != engine.getSelectedChannel())
            engine.setSelectedChannel (channel);
    };
    channelBox.onStep = [this] (int direction, bool all) {
        setAllChannels (false);
        stepChannel (direction, ! all);
    };
    addAndMakeVisible (channelBox);

    ghostButton.setClickingTogglesState (true);
    ghostButton.setToggleState (showGhosts, juce::dontSendNotification);
    ghostButton.setOnColour (theme::colour::bg6);
    ghostButton.setIconColourWhenOn (theme::colour::textPrimary);
    ghostButton.setFlat (true);
    ghostButton.setTooltip (tr ("Ghost notes: show the other channels' notes (Alt+V) and, outlined, this channel's notes in the clips "
                                "playing at the same time; double right-click one to edit its channel; Ctrl+Alt+V makes them editable"));
    ui::setQuickTip (ghostButton, tr ("Ghost notes"), "Alt+V");
    ui::setQuickTip (moreButton, tr ("Note tools"));
    ui::setQuickTip (scaleSnapButton, tr ("Snap to scale"));
    ghostButton.onClick = [this] {
        showGhosts = ghostButton.getToggleState();
        repaint();
    };

    moreButton.setFlat (true);
    moreButton.setTooltip (tr ("Note operations, left-edge resizing and MIDI import/export"));
    moreButton.onClick = [this] { showMoreMenu(); };

    scaleSnapButton.setOnColour (theme::colour::bg6);
    scaleSnapButton.setIconColourWhenOn (theme::colour::accent);
    scaleSnapButton.setFlat (true);
    scaleSnapButton.setTooltip (tr ("Snap notes to the scale: new and moved notes, and incoming MIDI, land on the chosen scale"));
    scaleSnapButton.onClick = [this] { setScaleSnap (! scaleSnap); };

    // Follow (FL: "Auto scrolling"): the view pages along with the playhead while the pattern plays
    followButton.setOnColour (theme::colour::bg6);
    followButton.setIconColourWhenOn (theme::colour::accent);
    followButton.setFlat (true);
    followButton.setClickingTogglesState (false);
    followButton.setToggleState (EditorGlobals::get().isFollowing (true), juce::dontSendNotification);
    followButton.setTooltip (tr ("Follow: the Piano Roll scrolls along with the playhead while this pattern plays; off, you can look around freely"));
    ui::setQuickTip (followButton, tr ("Follow the playhead"));
    followButton.onClick = [this] { runAction ("pianoRoll.follow"); };

    for (auto* b : { &ghostButton, &scaleSnapButton, &moreButton, &followButton })
    {
        b->setWantsKeyboardFocus (false);
        b->setMouseClickGrabsKeyboardFocus (false);
        addAndMakeVisible (*b);
    }

    for (int i = 0; i < (int) chords().size(); ++i)
        chordBox.addItem (tr (chords()[(std::size_t) i].name), i + 1);
    chordBox.setSelectedId (1, juce::dontSendNotification);
    chordBox.setTooltip (tr ("Clicking creates this chord (the clicked note is the root)"));

    // scale: root x type
    for (int s = 0; s < (int) scales().size(); ++s)
    {
        if (s == 0)
        {
            scaleBox.addItem (tr (scales()[0].name), 1);
            continue;
        }

        for (int root = 0; root < 12; ++root)
            scaleBox.addItem (tr ("{0} {1}", { juce::String (notenames::pitchName (root)), tr (scales()[(std::size_t) s].name) }), s * 100 + root + 1);
    }
    scaleBox.setSelectedId (1, juce::dontSendNotification);
    scaleSnapButton.setEnabled (false); // until a scale is chosen
    scaleBox.setTooltip (tr ("Highlights the notes of a scale"));

    slideButton.setClickingTogglesState (true);
    slideButton.setTooltip (tr ("New notes are \"slide\" notes: the currently sounding note glides to them (808 glide)"));
    slideButton.setWantsKeyboardFocus (false);
    slideButton.setMouseClickGrabsKeyboardFocus (false);
    slideButton.setButtonText (tr ("Slide"));
    slideButton.setColour (juce::TextButton::buttonOnColourId, theme::colour::accent);
    addAndMakeVisible (slideButton);

    colourButton.setTooltip (tr ("Note colour: new notes get this colour. Click: choose it, colour the selection (Alt+C), "
                                 "select by colour (Shift+C)"));
    ui::setQuickTip (colourButton, tr ("Note colour"));
    colourButton.setWantsKeyboardFocus (false);
    colourButton.setMouseClickGrabsKeyboardFocus (false);
    colourButton.onClick = [this] { showColourMenu(); };
    addAndMakeVisible (colourButton);

    chordLabel.setJustificationType (juce::Justification::centred);
    chordLabel.setFont (theme::font (theme::type::caption, true));
    chordLabel.setColour (juce::Label::textColourId, theme::colour::textPrimary);
    chordLabel.setTooltip (tr ("Chord: the chord of the keys held on the MIDI keyboard or the typing piano, else of the selected notes"));
    chordLabel.setInterceptsMouseClicks (true, false);
    addAndMakeVisible (chordLabel);

    for (auto* box : { &chordBox, &scaleBox })
    {
        box->setWantsKeyboardFocus (false);
        box->setMouseClickGrabsKeyboardFocus (false);
        box->onChange = [this] {
            publishScale();
            scaleSnapButton.setEnabled (scaleIndex() > 0);
            repaint();
        };
        addAndMakeVisible (box);
    }

    laneBox.addItem (tr ("Velocity"), 1);
    laneBox.addItem (tr ("Pan"), 2);
    laneBox.addItem (tr ("Fine pitch"), 3);
    laneBox.setTooltip (tr ("What the bottom lane edits"));
    laneBox.setSelectedId (1, juce::dontSendNotification);
    laneBox.setWantsKeyboardFocus (false);
    laneBox.setMouseClickGrabsKeyboardFocus (false);
    laneBox.onChange = [this] { repaint(); };
    addAndMakeVisible (laneBox);

    setOpaque (true); // paints every pixel: nothing behind it is redrawn while scrolling

    engine.addChangeListener (this);
    EditorGlobals::get().addChangeListener (this);
    refreshChannelBox();
    startTimerHz (30);
}

PianoRollComponent::~PianoRollComponent()
{
    stopListening();
    previewNotes.releaseAll();
    EditorGlobals::get().removeChangeListener (this);
    engine.removeChangeListener (this);
}

juce::Rectangle<int> PianoRollComponent::gridArea() const
{
    return getLocalBounds().withTrimmedLeft (keyboardWidth).withTrimmedTop (headerHeight)
        .withTrimmedRight (scrollBarSize).withTrimmedBottom (velocityHeight);
}

juce::Rectangle<int> PianoRollComponent::velocityArea() const
{
    const auto grid = gridArea();
    return { grid.getX(), grid.getBottom(), grid.getWidth(), velocityHeight };
}

double PianoRollComponent::tickToX (double tick) const
{
    return keyboardWidth + (tick - scrollTicks) * pixelsPerTick;
}

double PianoRollComponent::xToTick (int x) const
{
    return (x - keyboardWidth) / pixelsPerTick + scrollTicks;
}

int PianoRollComponent::keyToY (int key) const
{
    return headerHeight + (127 - key) * rowHeight - scrollY;
}

int PianoRollComponent::yToKey (int y) const
{
    return juce::jlimit (0, 127, 127 - (y - headerHeight + scrollY) / rowHeight);
}

std::int64_t PianoRollComponent::gridTicks() const
{
    const auto bar = (std::int64_t) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    const auto& option = grid::resolve (snapBox);

    switch (option.kind)
    {
        case grid::Kind::none: return 1;
        case grid::Kind::ticks: return option.ticks;
        case grid::Kind::bar: return bar;
        case grid::Kind::screen:
        case grid::Kind::nearest: break;
    }

    return noteedit::screenStep (pixelsPerTick, bar);
}

std::vector<std::int64_t> PianoRollComponent::snapCandidates() const
{
    // edges of this channel's notes (except the ones being dragged) and of the visible ghosts
    std::vector<std::int64_t> out;
    const auto& notes = currentPattern (engine.getSong()).notes;

    for (int i = 0; i < (int) notes.size(); ++i)
    {
        const auto& n = notes[(std::size_t) i];

        if ((! edits (n) && ! showGhosts) || std::find (dragIndices.begin(), dragIndices.end(), i) != dragIndices.end())
            continue;

        out.push_back (n.start);
        out.push_back (n.start + n.length);
    }

    return out;
}

std::int64_t PianoRollComponent::snap (double tick, bool enabled) const
{
    if (! enabled)
        return (std::int64_t) std::floor (tick);

    if (grid::resolve (snapBox).kind == grid::Kind::nearest)
        return noteedit::snapNearest (tick, snapCandidates(), 10.0 / pixelsPerTick, gridTicks());

    return noteedit::snapDown (tick, gridTicks());
}

std::int64_t PianoRollComponent::newNoteLength() const
{
    const auto fixed = EditorGlobals::get().getDefaultNoteLength();
    return fixed > 0 ? fixed : lastLength;
}

std::int64_t PianoRollComponent::viewLengthTicks() const
{
    const auto& song = engine.getSong();
    const auto bar = (std::int64_t) ppq * song.beatsPerBar;
    // pattern + 4 bars of headroom to grow
    return currentPattern (song).effectiveLength (song.beatsPerBar) + 4 * bar;
}

void PianoRollComponent::updateScrollBars()
{
    const auto grid = gridArea();
    const auto total = (double) viewLengthTicks();
    hBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    hBar.setCurrentRange (scrollTicks, grid.getWidth() / pixelsPerTick, juce::dontSendNotification);
    vBar.setRangeLimits (0.0, 128.0 * rowHeight, juce::dontSendNotification);
    vBar.setCurrentRange (scrollY, grid.getHeight(), juce::dontSendNotification);
    scrollTicks = hBar.getCurrentRangeStart();
    scrollY = (int) vBar.getCurrentRangeStart();
    overview.minLength = grid.getWidth() / (120.0 / ticksPerStep);
    overview.setRange (total, scrollTicks, grid.getWidth() / pixelsPerTick);
}

void PianoRollComponent::setTitleBarInsets (int left, int right)
{
    if (left != titleInsetLeft || right != titleInsetRight)
    {
        titleInsetLeft = left;
        titleInsetRight = right;
        resized();
    }
}

void PianoRollComponent::resized()
{
    const auto h = toolbarHeight - 6;
    auto bar = juce::Rectangle<int> (4 + titleInsetLeft, 3, juce::jmax (0, getWidth() - 8 - titleInsetLeft - titleInsetRight), h);

    // groups that don't fit in a narrow window are hidden (the tools and the grid always show)
    auto place = [&bar] (juce::Component& c, int width, bool optional = false) {
        const auto fits = ! optional || width <= bar.getWidth();
        c.setVisible (fits);
        c.setBounds (fits ? bar.removeFromLeft (juce::jmin (width, bar.getWidth())) : juce::Rectangle<int>());
        bar.removeFromLeft (juce::jmin (6, bar.getWidth()));
    };

    place (channelBox, 150);
    place (tools, tools.getIdealWidth (h));
    place (snapBox, 128);
    place (ghostButton, h);
    place (followButton, h);
    place (slideButton, 54, true);
    place (colourButton, h, true);
    place (chordBox, 86, true);
    place (scaleBox, 150, true);
    if (scaleBox.isVisible())
        place (scaleSnapButton, h);
    else
        scaleSnapButton.setVisible (false);
    place (moreButton, h);
    place (chordLabel, 84, true);
    laneBox.setBounds (2, velocityArea().getY() + 4, keyboardWidth - 4, 20);
    overview.setBounds (keyboardWidth, toolbarHeight, getWidth() - keyboardWidth - scrollBarSize, overviewHeight);
    vBar.setBounds (getWidth() - scrollBarSize, headerHeight, scrollBarSize, getHeight() - headerHeight - velocityHeight);
    updateScrollBars();
}

void PianoRollComponent::scrollToNotes (bool fitPattern)
{
    const auto& song = engine.getSong();
    int low = 128, high = -1;

    for (const auto& n : currentPattern (song).notes)
        if (edits (n))
        {
            low = juce::jmin (low, n.key);
            high = juce::jmax (high, n.key);
        }

    refreshChannelBox();
    const auto centreKey = high >= 0 ? (low + high) / 2 : 66;
    scrollY = (127 - centreKey) * rowHeight - gridArea().getHeight() / 2;
    scrollTicks = 0.0;

    // FL opens the Piano Roll with the whole pattern across the view (at least one bar)
    if (fitPattern && gridArea().getWidth() > 0)
    {
        const auto bar = (double) ppq * juce::jmax (1, song.beatsPerBar);
        const auto length = juce::jmax (bar, (double) currentPattern (song).effectiveLength (song.beatsPerBar));
        pixelsPerTick = juce::jlimit (4.0 / ticksPerStep, 64.0 / ticksPerStep, gridArea().getWidth() / length);
    }
    updateScrollBars();
    repaint();
}

// --- selection ---------------------------------------------------------------

std::vector<int> PianoRollComponent::chordIntervals() const
{
    const auto index = juce::jlimit (0, (int) chords().size() - 1, chordBox.getSelectedId() - 1);
    auto intervals = chords()[(std::size_t) index].intervals;
    // FL 2026's top-down stamp (REA round 153): the clicked key is the chord's highest note
    if (chordTopDown && intervals.size() > 1)
    {
        const auto top = *std::max_element (intervals.begin(), intervals.end());
        for (auto& i : intervals)
            i -= top;
        std::reverse (intervals.begin(), intervals.end()); // the clicked (top) note first: it leads the drag
    }
    return intervals;
}

int PianoRollComponent::scaleIndex() const
{
    const auto id = scaleBox.getSelectedId();
    return id <= 1 ? 0 : id / 100;
}

int PianoRollComponent::scaleRoot() const
{
    const auto id = scaleBox.getSelectedId();
    return id <= 1 ? 0 : (id % 100) - 1;
}

bool PianoRollComponent::inScale (int key) const
{
    return scaleIndex() > 0 && notetools::inScale (key, scaleRoot(), scaleIndex());
}

int PianoRollComponent::snapKey (int key) const
{
    return scaleSnap ? notetools::snapToScale (key, scaleRoot(), scaleIndex()) : key;
}

void PianoRollComponent::setScaleSnap (bool on)
{
    scaleSnap = on;
    scaleSnapButton.setToggleState (on, juce::dontSendNotification);
    publishScale();
    repaint();
}

void PianoRollComponent::publishScale()
{
    engine.setInputScale (scaleSnap ? scaleIndex() : 0, scaleRoot());
}

bool PianoRollComponent::isSelected (int index) const
{
    return std::binary_search (selection.begin(), selection.end(), index);
}

std::vector<int> PianoRollComponent::channelNotes() const
{
    const auto& notes = currentPattern (engine.getSong()).notes;
    std::vector<int> out;

    for (int i = 0; i < (int) notes.size(); ++i)
        if (edits (notes[(std::size_t) i]))
            out.push_back (i);

    return out;
}

std::vector<int> PianoRollComponent::selectionOrAll() const
{
    return selection.empty() ? channelNotes() : selection;
}

void PianoRollComponent::setSelection (std::vector<int> indices)
{
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    selection = std::move (indices);
    repaint();
}

void PianoRollComponent::selectAll()
{
    setSelection (channelNotes());
}

void PianoRollComponent::editNotes (const std::function<void (Pattern&)>& edit, bool undoable)
{
    engine.editSong ([&edit] (Song& s) { edit (currentPattern (s)); }, undoable);
    knownNoteCount = currentPattern (engine.getSong()).notes.size();
}

void PianoRollComponent::sortNotes()
{
    auto tracked = selection;
    tracked.push_back (dragNote);
    // only reorders: no undo step of its own needed
    engine.editSong ([&tracked] (Song& s) { noteedit::sortTracking (currentPattern (s), tracked); }, false);
    dragNote = tracked.back();
    tracked.pop_back();
    setSelection (tracked);
}

juce::Rectangle<float> PianoRollComponent::stretchHandle() const
{
    // a tab on the right end of the selection, vertically centred on it (clamped into the grid)
    const auto& notes = currentPattern (engine.getSong()).notes;
    std::int64_t start = 0, end = 0;
    int top = -1, bottom = 128;
    bool any = false;

    for (auto i : selection)
    {
        if (i < 0 || i >= (int) notes.size() || ! edits (notes[(std::size_t) i]))
            continue;
        const auto& n = notes[(std::size_t) i];
        start = any ? juce::jmin (start, n.start) : n.start;
        end = any ? juce::jmax (end, n.start + n.length) : n.start + n.length;
        top = juce::jmax (top, n.key);
        bottom = juce::jmin (bottom, n.key);
        any = true;
    }

    if (! any || end <= start)
        return {};

    const auto grid = gridArea().toFloat();
    const auto x = (float) tickToX ((double) end);
    const auto y0 = (float) keyToY (top), y1 = (float) (keyToY (bottom) + rowHeight);
    const auto cy = juce::jlimit (grid.getY() + 10.0f, juce::jmax (grid.getY() + 10.0f, grid.getBottom() - 10.0f), (y0 + y1) * 0.5f);
    const auto r = juce::Rectangle<float> (x + 2.0f, cy - 9.0f, 9.0f, 18.0f);
    return grid.intersects (r) ? r : juce::Rectangle<float>();
}

void PianoRollComponent::drumPaintAt (juce::Point<int> p, bool snapOn)
{
    // one short hit per grid cell and key, on whatever key is under the mouse (FL's drum painting)
    const auto cell = juce::jmax<std::int64_t> (1, snapOn ? gridTicks() : ticksPerStep);
    const auto start = juce::jmax<std::int64_t> (0, snapOn ? snap (xToTick (p.x), true) : noteedit::snapDown (xToTick (p.x), cell));
    const auto key = snapKey (yToKey (p.y));

    if (start == lastDrumCell && key == lastDrumKey)
        return;

    lastDrumCell = start;
    lastDrumKey = key;
    const auto channel = engine.getSelectedChannel();

    if (polish::hasNoteInCell (currentPattern (engine.getSong()), channel, key, start, cell))
        return;

    Note n;
    n.start = start;
    n.length = cell;
    n.key = key;
    n.channel = channel;
    n.colour = noteColour;
    editNotes ([&n] (Pattern& pat) { noteedit::paste (pat, { n }, 0); });

    if (key != previewKey)
        previewOn (key, n.velocity);
}

// --- painting ------------------------------------------------------------------

void PianoRollComponent::paintOverview (juce::Graphics& g, juce::Rectangle<float> area, double total)
{
    // the channel's notes across the whole view length, pitch mapped onto the strip's height; the part past
    // the pattern's end is darker; bar lines every bar (every 4 when they get dense)
    using namespace theme::colour;
    const auto& song = engine.getSong();
    const auto& pattern = currentPattern (song);
    const auto channel = engine.getSelectedChannel();
    const auto scale = area.getWidth() / juce::jmax (1.0, total);
    const auto bar = (double) ppq * juce::jmax (1, song.beatsPerBar);
    const auto end = (float) (pattern.effectiveLength (song.beatsPerBar) * scale);

    g.setColour (bg2);
    g.fillRect (area.withWidth (juce::jmin (area.getWidth(), end)));

    const auto every = bar * scale < 6.0 ? 4 : 1;
    g.setColour (gridBar.withAlpha (0.6f));
    for (int b = every; b * bar < total; b += every)
        g.fillRect ((float) (area.getX() + b * bar * scale), area.getY(), 1.0f, area.getHeight());

    int low = 128, high = -1;
    for (const auto& n : pattern.notes)
        if (edits (n))
        {
            low = juce::jmin (low, n.key);
            high = juce::jmax (high, n.key);
        }

    if (high < 0)
        return;

    const auto span = (float) juce::jmax (12, high - low + 1);
    const auto mid = (low + high) / 2.0f;
    const auto inner = area.reduced (0.0f, 2.0f);
    auto colour = accent;
    if (channel < (int) song.channels.size() && song.channels[(std::size_t) channel].colour != 0)
        colour = theme::tame (juce::Colour (song.channels[(std::size_t) channel].colour));
    colour = colour.interpolatedWith (textPrimary, 0.35f); // reads on the dark strip at any channel colour

    for (const auto& n : pattern.notes)
    {
        if (! edits (n))
            continue;

        const auto y = inner.getCentreY() - ((float) n.key - mid) / span * inner.getHeight();
        const auto x = area.getX() + (float) (n.start * scale);
        g.setColour (n.muted ? colour.withAlpha (0.35f) : colour);
        g.fillRect (x, juce::jlimit (inner.getY(), inner.getBottom() - 2.0f, y - 1.0f),
                    juce::jmax (1.0f, (float) (n.length * scale) - 0.5f), 2.0f);
    }
}

void PianoRollComponent::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("PianoRollComponent paint", true, g.getClipBounds());
    const auto& song = engine.getSong();
    const auto& pattern = currentPattern (song);
    const auto channel = engine.getSelectedChannel();
    const auto grid = gridArea();
    const auto patternLength = pattern.effectiveLength (song.beatsPerBar);
    const auto bar = (std::int64_t) ppq * song.beatsPerBar;
    const auto snapGrid = gridTicks();

    // the overview follows the notes (edits, undo, another channel or pattern) and the view
    {
        auto signature = (std::size_t) song.currentPattern * 1000003u + (std::size_t) channel * 7919u + pattern.notes.size();
        for (const auto& n : pattern.notes)
            signature = signature * 31u + (std::size_t) (n.start * 131 + n.length * 7 + n.key + n.channel * 257 + (n.muted ? 1 : 0));
        if (signature != overviewSignature)
        {
            overviewSignature = signature;
            overview.repaint();
        }
        overview.setRange ((double) viewLengthTicks(), scrollTicks, grid.getWidth() / pixelsPerTick);
    }

    using namespace theme::colour;
    g.fillAll (bg1);
    g.setColour (bg3);
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (border);
    g.fillRect (0, toolbarHeight - 1, getWidth(), 1);

    auto drawVerticalGrid = [&] (juce::Rectangle<int> area) {
        // grid lines: bar > beat > snap (unless too dense)
        const auto step = snapGrid * pixelsPerTick >= 6.0 ? snapGrid : (std::int64_t) ppq;
        const auto first = (std::int64_t) std::floor (scrollTicks / (double) step);
        const auto last = (std::int64_t) std::ceil (xToTick (area.getRight()) / (double) step);

        for (auto i = first; i <= last; ++i)
        {
            const auto tick = i * step;
            const auto x = (float) tickToX ((double) tick);
            g.setColour (tick % bar == 0 ? gridBar : tick % ppq == 0 ? gridBeat : gridStep);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());
        }
    };

    // --- grid ------------------------------------------------------------
    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (grid);

        for (int key = 0; key < 128; ++key)
        {
            const auto y = keyToY (key);

            if (y > grid.getBottom() || y + rowHeight < grid.getY())
                continue;

            auto row = isBlackKey (key) ? rowBlack : rowWhite;
            if (inScale (key))
                row = row.interpolatedWith (info, 0.10f);
            if (key == hover.key && drag == Drag::none)
                row = row.interpolatedWith (textPrimary, 0.04f); // the row under the mouse
            g.setColour (row);
            g.fillRect (grid.getX(), y, grid.getWidth(), rowHeight);

            if (key % 12 == 0)
            {
                g.setColour (gridBeat);
                g.drawHorizontalLine (y + rowHeight - 1, (float) grid.getX(), (float) grid.getRight());
            }
        }

        drawVerticalGrid (grid);

        // outside the pattern is darkened
        const auto endX = (float) tickToX ((double) patternLength);
        g.setColour (bg0.withAlpha (0.55f));
        g.fillRect (juce::Rectangle<float> (endX, (float) grid.getY(), (float) grid.getRight() - endX, (float) grid.getHeight()));

        // ghost notes (Alt+V). This channel's notes in the other clips that play at the same time in the
        // Playlist: hollow outlines. The other channels of this pattern: solid grey (a bit brighter, with
        // their channel colour on the edge, when Ctrl+Alt+V makes them editable)
        if (showGhosts)
        {
            for (const auto& n : polish::clipGhostNotes (song, song.currentPattern, channel))
            {
                const auto r = juce::Rectangle<float> ((float) tickToX ((double) n.start), (float) keyToY (n.key) + 1,
                                                       juce::jmax (4.0f, (float) (n.length * pixelsPerTick) - 1.0f), (float) rowHeight - 2);
                if (r.getRight() < (float) grid.getX() || r.getX() > (float) grid.getRight())
                    continue;
                g.setColour (textPrimary.withAlpha (0.30f));
                g.drawRoundedRectangle (r.reduced (0.5f), 2.0f, 1.0f);
            }
        }

        for (const auto& n : pattern.notes)
        {
            if (edits (n) || ! showGhosts)
                continue;

            const auto r = juce::Rectangle<float> ((float) tickToX ((double) n.start), (float) keyToY (n.key) + 1,
                                                   (float) (n.length * pixelsPerTick), (float) rowHeight - 2);
            const auto hovered = &n - pattern.notes.data() == hover.ghost;
            g.setColour (textPrimary.withAlpha (hovered ? 0.16f : editGhosts ? 0.12f : 0.09f));
            g.fillRoundedRectangle (r, 2.0f);
            g.setColour (textPrimary.withAlpha (hovered ? 0.35f : editGhosts ? 0.22f : 0.12f));
            g.drawRoundedRectangle (r, 2.0f, 1.0f);

            if (editGhosts && n.channel >= 0 && n.channel < (int) song.channels.size() && song.channels[(std::size_t) n.channel].colour != 0)
            {
                g.setColour (theme::tame (juce::Colour (song.channels[(std::size_t) n.channel].colour)).withAlpha (0.7f));
                g.fillRect (r.withWidth (2.0f).reduced (0.0f, 1.0f));
            }
        }

        for (int i = 0; i < (int) pattern.notes.size(); ++i)
        {
            const auto& n = pattern.notes[(std::size_t) i];

            if (! edits (n))
                continue;

            const auto selected = isSelected (i);
            const auto hovered = i == hover.note && drag == Drag::none;
            const auto r = juce::Rectangle<float> ((float) tickToX ((double) n.start), (float) keyToY (n.key) + 1,
                                                   juce::jmax (4.0f, (float) (n.length * pixelsPerTick) - 1.0f), (float) rowHeight - 2);
            auto fill = selected ? accentHover.brighter (0.15f) : n.muted ? noteMuted : theme::noteGroupColour (n.colour, n.velocity);
            if (hovered)
                fill = fill.brighter (0.18f);
            g.setColour (fill);
            g.fillRoundedRectangle (r, 2.5f);
            g.setColour (selected ? textPrimary : hovered ? textPrimary.withAlpha (0.75f) : bg0.withAlpha (0.6f));
            g.drawRoundedRectangle (r, 2.5f, 1.0f);

            // "All" channels: whose note it is, by the channel's colour on its left edge (issue #12)
            if (allChannels && n.channel >= 0 && n.channel < (int) song.channels.size() && r.getWidth() > 5.0f)
            {
                const auto c = song.channels[(std::size_t) n.channel].colour;
                g.setColour ((c != 0 ? theme::tame (juce::Colour (c)) : textPrimary.withAlpha (0.5f)).withAlpha (0.9f));
                g.fillRect (r.withWidth (2.5f).reduced (0.0f, 1.5f).translated (1.0f, 0.0f));
            }

            // the edge under the mouse shows its grip
            if (hovered && (hover.zone == gesture::Zone::rightEdge || hover.zone == gesture::Zone::leftEdge))
            {
                const auto gripX = hover.zone == gesture::Zone::rightEdge ? r.getRight() - 3.0f : r.getX() + 1.0f;
                g.setColour (textPrimary);
                g.fillRoundedRectangle (gripX, r.getY() + 2.0f, 2.0f, juce::jmax (2.0f, r.getHeight() - 4.0f), 1.0f);
            }

            if (n.group != 0 && r.getWidth() > 10.0f) // grouped: a small link mark at the top right
            {
                g.setColour (bg0.withAlpha (0.7f));
                g.fillEllipse (r.getRight() - 6.0f, r.getY() + 1.5f, 3.5f, 3.5f);
            }

            if (n.porta) // porta: slanted stroke at the start, like the glide
            {
                g.setColour (bg0.withAlpha (0.65f));
                g.drawLine (r.getX() + 2.0f, r.getBottom() - 2.0f, r.getX() + 9.0f, r.getY() + 2.0f, 2.0f);
            }

            if (n.slide) // slide arrow at the start of the note
            {
                juce::Path arrow;
                arrow.addTriangle (r.getX() + 2.0f, r.getBottom() - 2.0f, r.getX() + 2.0f, r.getY() + 2.0f, r.getX() + 9.0f, r.getCentreY());
                g.setColour (bg0.withAlpha (0.65f));
                g.fillPath (arrow);
            }

            if (gesture::noteLabelFits (r.getWidth(), rowHeight)) // the note's name, readable on its colour
            {
                g.setColour (fill.getPerceivedBrightness() > 0.55f ? bg0.withAlpha (0.85f) : textPrimary.withAlpha (0.9f));
                g.setFont (theme::font (gesture::labelHeight (rowHeight) - 1.0f));
                textcache::drawText (g, keyName (n.key), r.reduced (n.slide || n.porta ? 11.0f : 4.0f, 0.0f), juce::Justification::centredLeft, true);
            }
        }

        // the selection's stretch handle: a tab at its right end with two grips
        if (const auto handle = stretchHandle(); ! handle.isEmpty() && (drag == Drag::none || drag == Drag::stretch))
        {
            const auto lit = hover.stretch || drag == Drag::stretch;
            g.setColour (lit ? accentHover : accent.withAlpha (0.85f));
            g.fillRoundedRectangle (handle, 3.0f);
            g.setColour (onAccent.withAlpha (0.9f));
            g.fillRect (handle.getCentreX() - 2.0f, handle.getY() + 5.0f, 1.0f, handle.getHeight() - 10.0f);
            g.fillRect (handle.getCentreX() + 1.0f, handle.getY() + 5.0f, 1.0f, handle.getHeight() - 10.0f);

            if (lit) // the stretched span, from the anchor to the handle
            {
                g.setColour (accent.withAlpha (0.5f));
                const auto x0 = (float) tickToX ((double) (drag == Drag::stretch ? stretchAnchor : 0));
                if (drag == Drag::stretch)
                    g.drawVerticalLine ((int) x0, (float) grid.getY(), (float) grid.getBottom());
                g.drawVerticalLine ((int) handle.getX() - 2, (float) grid.getY(), (float) grid.getBottom());
            }
        }

        if (! rubberBand.isEmpty())
        {
            g.setColour (selectionFill);
            g.fillRect (rubberBand);
            g.setColour (theme::colour::selection);
            g.drawRect (rubberBand);
        }

        if (drag == Drag::knife)
        {
            juce::Path cut;
            cut.startNewSubPath (knifeFrom.toFloat());
            cut.lineTo (knifeTo.toFloat());
            juce::Path dashed;
            const float dash[] = { 5.0f, 3.0f };
            juce::PathStrokeType (1.5f).createDashedStroke (dashed, cut, dash, 2);
            g.setColour (textPrimary);
            g.fillPath (dashed);
        }

        if (playheadTick >= 0.0)
        {
            g.setColour (playhead);
            g.drawVerticalLine ((int) tickToX (playheadTick), (float) grid.getY(), (float) grid.getBottom());
        }
    }

    // --- velocity lane -----------------------------------------------------
    {
        const auto lane = velocityArea();
        g.setColour (bg0);
        g.fillRect (lane);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (lane);
        drawVerticalGrid (lane);
        g.setColour (border);
        g.drawHorizontalLine (lane.getY(), (float) lane.getX(), (float) lane.getRight());

        const auto usable = (float) lane.getHeight() - 8.0f;
        const auto bipolar = laneIsBipolar();
        const auto base = bipolar ? (float) lane.getBottom() - 2.0f - usable * 0.5f : (float) lane.getBottom();

        if (bipolar)
        {
            g.setColour (textPrimary.withAlpha (0.15f));
            g.drawHorizontalLine ((int) base, (float) lane.getX(), (float) lane.getRight());
        }

        if (const auto target = eventLaneTarget()) // pattern events: held values, a dot on each event
        {
            const auto* events = eventedit::findLane (pattern, *target);
            if (events != nullptr && ! events->events.empty())
            {
                juce::Path held;
                const auto yOf = [&] (float v) { return (float) lane.getBottom() - 2.0f - usable * v; };
                auto x = (float) tickToX (0.0);
                auto y = yOf (events->events.front().value);
                held.startNewSubPath (x, y);
                for (const auto& ev : events->events)
                {
                    const auto ex = (float) tickToX ((double) ev.tick);
                    held.lineTo (ex, y);
                    y = yOf (ev.value);
                    held.lineTo (ex, y);
                }
                held.lineTo ((float) lane.getRight(), y);
                g.setColour (accent.withAlpha (0.85f));
                g.strokePath (held, juce::PathStrokeType (1.5f));
                for (const auto& ev : events->events)
                    g.fillEllipse ((float) tickToX ((double) ev.tick) - 2.5f, yOf (ev.value) - 2.5f, 5.0f, 5.0f);
            }
            else
            {
                g.setColour (textDisabled);
                g.setFont (theme::font (theme::type::small));
                textcache::drawText (g, tr ("No events yet: draw them here"), lane.reduced (8, 4), juce::Justification::centredLeft, true);
            }
        }
        // the lane's name in it, as FL writes it next to "Control" (round 149: the box on the left is narrow)
        g.setColour (theme::colour::textSecondary.withAlpha (0.8f));
        g.setFont (theme::font (theme::type::caption, true));
        textcache::drawText (g, laneBox.getText(), lane.reduced (8, 3).removeFromTop (14), juce::Justification::topLeft, true);

        for (int i = 0; i < (int) pattern.notes.size() && ! eventLaneTarget(); ++i)
        {
            const auto& n = pattern.notes[(std::size_t) i];

            if (! edits (n))
                continue;

            const auto x = (float) tickToX ((double) n.start);
            const auto top = (float) lane.getBottom() - 2.0f - usable * laneValue (n);
            auto colour = isSelected (i) ? accentHover.brighter (0.15f) : theme::noteGroupColour (n.colour, n.velocity);
            if (n.muted)
                colour = noteMuted;
            const auto hovered = i == hover.note && drag == Drag::none;
            g.setColour (hovered ? colour.brighter (0.3f) : colour.withAlpha (0.8f));
            g.fillRect (juce::Rectangle<float> (x, juce::jmin (top, base), hovered ? 3.0f : 2.0f, std::abs (base - top)));
            g.fillEllipse (x - 3.0f, top - 3.0f, 7.0f, 7.0f);
            if (hovered)
                g.drawEllipse (x - 5.0f, top - 5.0f, 11.0f, 11.0f, 1.0f);
        }

        if (drag == Drag::velocityLine) // the line being drawn
        {
            g.setColour (textPrimary.withAlpha (0.8f));
            g.drawLine (juce::Line<int> (knifeFrom, knifeTo).toFloat(), 1.5f);
        }
    }

    // --- time ruler --------------------------------------------------------
    {
        const auto ruler = juce::Rectangle<int> (keyboardWidth, rulerTop, grid.getWidth(), timelineHeight);
        g.setColour (bg2);
        g.fillRect (ruler);
        g.setColour (border);
        g.fillRect (ruler.withTop (ruler.getBottom() - 1));
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (ruler);
        g.setFont (theme::font (theme::type::caption));

        const auto firstBar = (std::int64_t) std::floor (scrollTicks / (double) bar);
        const auto lastBar = (std::int64_t) std::ceil (xToTick (grid.getRight()) / (double) bar);

        for (auto b = firstBar; b <= lastBar; ++b)
        {
            const auto x = (int) tickToX ((double) (b * bar));
            g.setColour (textSecondary);
            g.drawVerticalLine (x, (float) rulerTop + 10.0f, (float) headerHeight);
            textcache::drawText (g, juce::String (b + 1), x + 3, rulerTop + 2, 40, timelineHeight - 4, juce::Justification::centredLeft);
        }

        if (playheadTick >= 0.0)
        {
            const auto x = (float) tickToX (playheadTick);
            juce::Path tri;
            tri.addTriangle (x - 5, (float) rulerTop, x + 5, (float) rulerTop, x, (float) rulerTop + 8);
            g.setColour (playhead);
            g.fillPath (tri);
        }
    }

    // --- keyboard ------------------------------------------------------------
    // flat keys; every C is labelled with its octave, the other white keys too when the rows are tall
    // enough; the key under the mouse (and the one sounding) are lit
    {
        const auto keys = juce::Rectangle<int> (0, headerHeight, keyboardWidth, grid.getHeight());
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (keys);
        g.setColour (bg0);
        g.fillRect (keys);
        const auto fontHeight = gesture::labelHeight (rowHeight);
        const auto blackWidth = keyboardWidth * 3 / 5;
        static const std::map<int, std::string> noLabels;
        const auto selectedChannel = engine.getSelectedChannel();
        const auto& labels = selectedChannel >= 0 && selectedChannel < (int) song.channels.size()
                                 ? song.channels[(std::size_t) selectedChannel].keyLabels : noLabels;

        for (int key = 0; key < 128; ++key)
        {
            const auto y = keyToY (key);

            if (y > keys.getBottom() || y + rowHeight < keys.getY())
                continue;

            const auto black = isBlackKey (key);
            const auto pressed = key == previewKey;
            const auto hovered = key == hover.key && ! pressed;
            auto colour = black ? keyBlack : keyWhite;
            if (inScale (key))
                colour = colour.interpolatedWith (info, black ? 0.18f : 0.12f);
            if (hovered)
                colour = colour.interpolatedWith (accent, black ? 0.35f : 0.22f);
            if (pressed)
                colour = accent;

            const auto width = black ? blackWidth : keyboardWidth - 1;
            g.setColour (colour);
            g.fillRect (0, y, width, rowHeight);
            g.setColour (black ? bg0 : keyWhite.darker (0.3f));
            g.drawHorizontalLine (y + rowHeight - 1, 0.0f, (float) width);

            const auto isC = key % 12 == 0;

            if (isC || hovered || pressed || (! black && rowHeight >= 16))
            {
                g.setFont (theme::font (fontHeight, isC));
                g.setColour (pressed ? onAccent : black ? textPrimary : isC ? bg0 : bg0.withAlpha (0.55f));
                const auto area = black ? juce::Rectangle<int> (2, y, blackWidth - 4, rowHeight)
                                        : juce::Rectangle<int> (blackWidth, y, keyboardWidth - blackWidth - 5, rowHeight);
                textcache::drawText (g, keyName (key), area, black ? juce::Justification::centredLeft : juce::Justification::centredRight, false);
            }

            // a key label (FL 2026's custom key labels) replaces the name on the white part of the key
            if (const auto label = labels.find (key); label != labels.end() && rowHeight >= 8)
            {
                const auto area = juce::Rectangle<int> (black ? blackWidth + 2 : 3, y, keyboardWidth - (black ? blackWidth + 2 : 3) - 4, rowHeight);
                g.setColour (black ? keyWhite : colour);
                g.fillRect (area);
                g.setFont (theme::font (fontHeight, false));
                g.setColour (pressed ? onAccent : bg0);
                g.drawFittedText (juce::String::fromUTF8 (label->second.c_str()), area.reduced (1, 0), juce::Justification::centredLeft, 1, 0.8f);
            }
        }

        g.setColour (border);
        g.fillRect (keyboardWidth - 1, keys.getY(), 1, keys.getHeight());
    }

    // velocity lane label
    {
        const auto lane = velocityArea();
        g.setColour (bg2);
        g.fillRect (0, lane.getY(), keyboardWidth, lane.getHeight());
        juce::ignoreUnused (lane); // the property combo sits here
    }

    // top-left corner: channel name
    g.setColour (bg3);
    g.fillRect (0, 0, keyboardWidth, toolbarHeight);
    g.setColour (bg2);
    g.fillRect (0, toolbarHeight, keyboardWidth, headerHeight - toolbarHeight);
    g.setColour (border);
    g.fillRect (0, headerHeight - 1, keyboardWidth, 1);
    g.fillRect (keyboardWidth - 1, 0, 1, headerHeight);

    if (channel < (int) song.channels.size())
    {
        const auto& ch = song.channels[(std::size_t) channel];
        g.setColour (ch.colour != 0 ? theme::tame (juce::Colour (ch.colour)) : accent);
        g.fillRoundedRectangle (4.0f, (float) toolbarHeight + 6.0f, 3.0f, (float) (headerHeight - toolbarHeight) - 12.0f, 1.0f);
        g.setColour (textPrimary);
        g.setFont (theme::font (theme::type::caption, true));
        textcache::drawFittedText (g, juce::String::fromUTF8 (ch.name.c_str()), 11, toolbarHeight, keyboardWidth - 14, headerHeight - toolbarHeight,
                          juce::Justification::centredLeft, 2, 0.8f);
    }
}

// --- mouse -------------------------------------------------------------------

int PianoRollComponent::noteAt (int x, int y, bool& onRightEdge) const
{
    bool left = false;
    return noteAt (x, y, onRightEdge, left);
}

int PianoRollComponent::noteAt (int x, int y, bool& onRightEdge, bool& onLeftEdge) const
{
    // the same hit test as the hover (cursor and highlight), so a click does what the cursor showed
    const auto h = hoverAt ({ x, y });
    onRightEdge = h.zone == gesture::Zone::rightEdge;
    onLeftEdge = h.zone == gesture::Zone::leftEdge;
    return gridArea().contains (x, y) ? h.note : -1;
}

int PianoRollComponent::ghostAt (int x, int y) const
{
    if (! showGhosts)
        return -1;

    const auto& notes = currentPattern (engine.getSong()).notes;
    const auto key = yToKey (y);

    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto& n = notes[(std::size_t) i];

        if (! edits (n) && n.key == key && x >= tickToX ((double) n.start) && x < tickToX ((double) (n.start + n.length)))
            return i;
    }

    return -1;
}

float PianoRollComponent::keyVelocityAt (int x, int key) const
{
    // FL: a click near the left of a key plays it softly, at its right end at full velocity (black keys are shorter)
    return gesture::keyClickVelocity ((double) x, (double) (isBlackKey (key) ? keyboardWidth * 3 / 5 : keyboardWidth - 1));
}

void PianoRollComponent::previewOn (int key, float velocity)
{
    // one preview note at a time; it remembers its channel, so a channel switch meanwhile can't strand it
    previewNotes.releaseAll();
    previewKey = key;
    previewNotes.press (engine.getSelectedChannel(), key, velocity);
    repaint (0, headerHeight, keyboardWidth, getHeight());
}

void PianoRollComponent::previewOff()
{
    previewNotes.releaseAll();

    if (previewKey >= 0)
    {
        previewKey = -1;
        repaint (0, headerHeight, keyboardWidth, getHeight());
    }
}

void PianoRollComponent::visibilityChanged()
{
    if (! isShowing()) // hidden mid-gesture (window closed, tab switched): nothing keeps sounding
    {
        previewOff();
        stopListening();
    }
}

float PianoRollComponent::laneValue (const Note& n) const
{
    switch (laneBox.getSelectedId())
    {
        case 2: return (n.pan + 1.0f) * 0.5f;
        case 3: return (n.finePitch + 120.0f) / 240.0f;
        default: return n.velocity;
    }
}

void PianoRollComponent::setLaneValue (Note& n, float v) const
{
    v = juce::jlimit (0.0f, 1.0f, v);

    switch (laneBox.getSelectedId())
    {
        case 2: n.pan = std::abs (v - 0.5f) < 0.02f ? 0.0f : v * 2.0f - 1.0f; break;   // snaps to center
        case 3: n.finePitch = std::round (v * 240.0f - 120.0f); break;
        default: n.velocity = v; break;
    }
}

bool PianoRollComponent::laneIsBipolar() const
{
    return laneBox.getSelectedId() != 1;
}

void PianoRollComponent::applyVelocity (int fromX, int toX, int y)
{
    const auto lane = velocityArea();
    applyLaneValue (toX, juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - y) / (float) (lane.getHeight() - 8)), fromX);
}

void PianoRollComponent::applyLaneValue (int toX, float value, int fromX)
{
    const auto lo = juce::jmin (fromX, toX) - 3, hi = juce::jmax (fromX, toX) + 3;
    const auto& notes = currentPattern (engine.getSong()).notes;
    std::vector<int> hits;

    for (auto i : selectionOrAll())
    {
        const auto x = (int) tickToX ((double) notes[(std::size_t) i].start);

        if (x >= lo && x <= hi)
            hits.push_back (i);
    }

    if (! hits.empty())
        editNotes ([this, &hits, value] (Pattern& p) {
            for (auto i : hits)
                setLaneValue (p.notes[(std::size_t) i], value);
        });
}

void PianoRollComponent::togglePorta()
{
    const auto target = selectionOrAll();

    if (target.empty())
        return;

    // like mute: if any note lacks porta, enable it on all; otherwise disable
    const auto& notes = currentPattern (engine.getSong()).notes;
    const auto porta = std::any_of (target.begin(), target.end(), [&notes] (int i) { return ! notes[(std::size_t) i].porta; });
    editNotes ([&target, porta] (Pattern& p) {
        for (auto i : target)
            p.notes[(std::size_t) i].porta = porta;
    });
}

void PianoRollComponent::toggleMute()
{
    const auto target = selectionOrAll();

    if (target.empty())
        return;

    // if any is playing, mute all; otherwise unmute
    const auto& notes = currentPattern (engine.getSong()).notes;
    const auto mute = std::any_of (target.begin(), target.end(), [&notes] (int i) { return ! notes[(std::size_t) i].muted; });
    editNotes ([&target, mute] (Pattern& p) {
        for (auto i : target)
            p.notes[(std::size_t) i].muted = mute;
    });
}

void PianoRollComponent::paintNoteAt (std::int64_t start, int key, bool monophonic)
{
    // a brush stroke lays single notes (or the chosen chord) at the default length
    Note n;
    n.start = juce::jmax<std::int64_t> (0, start);
    n.length = paintLength;
    n.key = key;
    n.channel = engine.getSelectedChannel();
    n.slide = slideButton.getToggleState();
    n.colour = noteColour;

    std::vector<Note> stamp;
    for (auto interval : chordIntervals())
        if (n.key + interval <= 127 && n.key + interval >= 0)
        {
            auto chordNote = n;
            chordNote.key = snapKey (n.key + interval); // with scale snap the chord follows the scale
            if (std::none_of (stamp.begin(), stamp.end(), [&chordNote] (const Note& o) { return o.key == chordNote.key; }))
                stamp.push_back (chordNote);
        }

    editNotes ([&] (Pattern& p) {
        if (monophonic) // Shift: the new note replaces whatever this channel plays at that time
        {
            std::vector<int> overlapping;
            for (int i = 0; i < (int) p.notes.size(); ++i)
            {
                const auto& o = p.notes[(std::size_t) i];
                if (o.channel == n.channel && o.start < n.start + n.length && o.start + o.length > n.start)
                    overlapping.push_back (i);
            }
            noteedit::remove (p, overlapping);
        }

        noteedit::paste (p, stamp, 0);
    });
}

void PianoRollComponent::mouseDown (const juce::MouseEvent& e)
{
    if (engine.getSong().channels.empty())
        return;

    downMods = e.mods;
    startedOnEmpty = erasedSomething = holdExpired = false;

    const auto onKeyboard = e.x < keyboardWidth && e.y >= headerHeight && e.y < gridArea().getBottom();
    hover = {};
    gliding = false;

    if (e.mods.isMiddleButtonDown())
    {
        if (onKeyboard) // middle-drag on the keyboard zooms vertically around the clicked key
        {
            drag = Drag::keyZoom;
            keyZoomKey = yToKey (e.y);
            keyZoomStartRow = rowHeight;
            setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
            return;
        }

        drag = Drag::pan; // elsewhere it pans both ways, in every tool
        panStartTicks = scrollTicks;
        panStartY = scrollY;
        setMouseCursor (editorCursor (gesture::Cursor::pan));
        return;
    }

    if (onKeyboard)
    {
        if (e.mods.isCtrlDown()) // Ctrl+click / drag: selects the notes of a key (range)
        {
            drag = Drag::keySelect;
            selectionBefore = e.mods.isShiftDown() ? selection : std::vector<int> {};
            mouseDrag (e);
            return;
        }

        if (e.mods.isRightButtonDown()) // key labels (FL 2026: right-click a key > Rename)
        {
            showKeyMenu (yToKey (e.y));
            return;
        }

        if (e.mods.isLeftButtonDown())
        {
            drag = Drag::keyboard;
            previewOn (yToKey (e.y), keyVelocityAt (e.x, yToKey (e.y)));
        }
        return;
    }

    // ruler: Ctrl+drag selects the notes in a time range
    if (e.y >= rulerTop && e.y < headerHeight && e.x >= keyboardWidth && e.mods.isCtrlDown() && e.mods.isLeftButtonDown())
    {
        drag = Drag::rulerSelect;
        selectionBefore = e.mods.isShiftDown() ? selection : std::vector<int> {};
        return;
    }

    // ruler: click or drag = position (FL), Alt = off the grid
    if (e.y >= rulerTop && e.y < headerHeight && e.x >= keyboardWidth && e.mods.isLeftButtonDown() && ! e.mods.isShiftDown())
    {
        drag = Drag::seek;
        seekAt (e.x, ! e.mods.isAltDown());
        return;
    }

    if (velocityArea().contains (e.getPosition()) && eventLaneTarget())
    {
        // pattern events: drag draws one event per grid step, right-drag erases
        lastVelocityX = e.x;
        drag = e.mods.isRightButtonDown() ? Drag::eventErase : Drag::eventDraw;
        drawEvents (e.x, e.x, e.y, drag == Drag::eventErase);
        return;
    }

    if (velocityArea().contains (e.getPosition()))
    {
        const auto lane = velocityArea();
        const auto value = juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - e.y) / (float) (lane.getHeight() - 8));
        lastVelocityX = e.x;

        if (e.mods.isAltDown() && e.mods.isLeftButtonDown()) // Alt+click resets the notes under the mouse
        {
            drag = Drag::velocity;
            applyLaneValue (e.x, laneBox.getSelectedId() == 1 ? Note().velocity : 0.5f, e.x);
            drag = Drag::none;
            return;
        }

        laneOriginals = currentPattern (engine.getSong()).notes;

        if (e.mods.isRightButtonDown()) // right-drag draws a straight line of values
        {
            drag = Drag::velocityLine;
            laneSetValue = value;
            return;
        }

        if (e.mods.isShiftDown()) // Shift+drag: every note crossed gets the value of the press
        {
            drag = Drag::velocitySet;
            laneSetValue = value;
            applyLaneValue (e.x, value, e.x);
            return;
        }

        drag = Drag::velocity;
        applyVelocity (e.x, e.x, e.y);
        return;
    }

    if (! gridArea().contains (e.getPosition()))
        return;

    // the selection's stretch handle: drag scales the selected notes in time from the selection's start
    if (e.mods.isLeftButtonDown() && ! e.mods.isCtrlDown() && stretchHandle().expanded (2.0f).contains (e.position))
    {
        const auto& notes = currentPattern (engine.getSong()).notes;
        dragIndices.clear();
        dragOriginals.clear();

        for (auto i : selection)
            if (i >= 0 && i < (int) notes.size() && edits (notes[(std::size_t) i]))
            {
                dragIndices.push_back (i);
                dragOriginals.push_back (notes[(std::size_t) i]);
            }

        noteedit::span (dragOriginals, stretchAnchor, stretchEnd);
        dragNote = dragIndices.empty() ? -1 : dragIndices.front();
        drag = Drag::stretch;
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        return;
    }

    bool rightEdge = false, leftEdge = false;
    const auto hit = noteAt (e.x, e.y, rightEdge, leftEdge);
    const auto tool = tools.getTool();

    // editable ghost notes (Ctrl+Alt+V): a left press on one switches to its channel and grabs the note there
    if (hit < 0 && editGhosts && e.mods.isLeftButtonDown() && ! e.mods.isCtrlDown())
        if (const auto ghost = ghostAt (e.x, e.y); ghost >= 0)
        {
            engine.setSelectedChannel (currentPattern (engine.getSong()).notes[(std::size_t) ghost].channel);
            if (noteAt (e.x, e.y, rightEdge, leftEdge) >= 0) // now it is one of this channel's notes
                mouseDown (e);
            return;
        }

    // Zoom tool: right-click zooms out around the point (a click zooms in, a drag zooms to the range)
    if (tool == EditTool::zoom && e.mods.isRightButtonDown() && ! e.mods.isCtrlDown())
    {
        zoomOut (e.x);
        return;
    }

    // every tool: right click deletes (double right-click on a ghost note switches to its channel; with
    // editable ghost notes a right-click deletes the ghost note itself)
    if (e.mods.isRightButtonDown())
    {
        if (hit < 0 && editGhosts)
            if (const auto ghost = ghostAt (e.x, e.y); ghost >= 0)
            {
                setSelection ({});
                editNotes ([ghost] (Pattern& p) { noteedit::remove (p, { ghost }); });
                return;
            }

        if (hit < 0 && e.getNumberOfClicks() >= 2)
            if (const auto ghost = ghostAt (e.x, e.y); ghost >= 0)
            {
                engine.setSelectedChannel (currentPattern (engine.getSong()).notes[(std::size_t) ghost].channel);
                return;
            }

        drag = Drag::erase;
        startedOnEmpty = hit < 0;
        setSelection ({}); // indices change while deleting

        if (hit >= 0)
        {
            erasedSomething = true;
            editNotes ([hit] (Pattern& p) { noteedit::remove (p, { hit }); });
        }
        return;
    }

    // every tool: Ctrl+click on a note adds/removes it from the selection; Ctrl+drag selects (Shift adds)
    if (hit >= 0 && e.mods.isCtrlDown())
    {
        auto s = selection;

        if (isSelected (hit))
            s.erase (std::find (s.begin(), s.end(), hit));
        else
            s.push_back (hit);

        setSelection (s);
        return;
    }

    if ((hit < 0 && e.mods.isCtrlDown()) || tool == EditTool::lasso
        || (hit < 0 && e.mods.isShiftDown() && tool != EditTool::brush && tool != EditTool::drumPaint))
    {
        drag = Drag::select;
        dragNote = hit; // a click without dragging selects this note (Lasso)
        selectionBefore = e.mods.isShiftDown() ? selection : std::vector<int> {};
        rubberBand = {};
        setSelection (selectionBefore);
        return;
    }

    const auto snapOn = ! e.mods.isAltDown();

    switch (tool)
    {
        case EditTool::eraser:
            drag = Drag::erase;
            setSelection ({});
            if (hit >= 0)
                editNotes ([hit] (Pattern& p) { noteedit::remove (p, { hit }); });
            return;

        case EditTool::mute:
            drag = Drag::muting;
            touchedNotes.clear();
            if (hit >= 0)
            {
                muteTarget = ! currentPattern (engine.getSong()).notes[(std::size_t) hit].muted;
                touchedNotes.push_back (hit);
                editNotes ([hit, m = muteTarget] (Pattern& p) { p.notes[(std::size_t) hit].muted = m; });
            }
            else
                muteTarget = true;
            return;

        case EditTool::knife:
            drag = Drag::knife;
            knifeFrom = knifeTo = e.getPosition();
            repaint();
            return;

        case EditTool::zoom:
            drag = Drag::zoomRange;
            rubberBand = {};
            return;

        case EditTool::listen:
            drag = Drag::listen;
            listenAt (e.x);
            return;

        case EditTool::pencil:
        case EditTool::brush:
        case EditTool::drumPaint:
        case EditTool::lasso:
        case EditTool::slip:
            break;
    }

    // Pencil (and Brush on an existing note): Shift+drag on a note drags a copy
    if (hit >= 0 && e.mods.isShiftDown() && ! rightEdge && ! leftEdge)
    {
        const auto sources = isSelected (hit) ? selection : notetools::groupOf (currentPattern (engine.getSong()), hit);
        const auto grabbedAt = (std::size_t) (std::find (sources.begin(), sources.end(), hit) - sources.begin());
        std::vector<int> added;

        // the copy comes out in the same order as the originals (copy sorts by start)
        std::vector<Note> ordered;
        for (auto i : sources)
            ordered.push_back (currentPattern (engine.getSong()).notes[(std::size_t) i]);

        editNotes ([&] (Pattern& p) {
            added = noteedit::paste (p, ordered, 0);
            notetools::regroup (p, added);
        });
        setSelection (added);
        dragIndices = added;
        dragOriginals = ordered;
        dragNote = added[grabbedAt];
        drag = Drag::move;
        grabOffset = ordered[grabbedAt].start - snap (xToTick (e.x), snapOn);
        previewOn (ordered[grabbedAt].key, ordered[grabbedAt].velocity);
        return;
    }

    if (hit >= 0)
    {
        if (! isSelected (hit)) // a grouped note brings its group along (FL's note groups)
        {
            const auto members = notetools::groupOf (currentPattern (engine.getSong()), hit);
            setSelection (members.size() > 1 ? members : std::vector<int> {});
        }

        const auto& notes = currentPattern (engine.getSong()).notes;
        const auto& n = notes[(std::size_t) hit];
        dragNote = hit;
        dragIndices = selection.empty() ? std::vector<int> { hit } : selection;
        dragOriginals.clear();

        for (auto i : dragIndices)
            dragOriginals.push_back (notes[(std::size_t) i]);

        drag = rightEdge ? Drag::resize : leftEdge ? Drag::resizeLeft : Drag::move;
        grabOffset = n.start - snap (xToTick (e.x), snapOn);
        lastLength = n.length;
        previewOn (n.key, n.velocity);
        return;
    }

    // new note (or the chosen chord, with the clicked note as the root)
    setSelection ({});
    createdAt = juce::Time::getMillisecondCounter();

    if (tool == EditTool::drumPaint)
    {
        drag = Drag::drumPaint;
        lastDrumCell = -1;
        lastDrumKey = -1;
        drumPaintAt (e.getPosition(), snapOn);
        return;
    }

    if (tool == EditTool::brush)
    {
        drag = Drag::paint;
        paintLength = juce::jmax<std::int64_t> (1, newNoteLength());
        const auto start = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn));
        paintNoteAt (start, snapKey (yToKey (e.y)), e.mods.isShiftDown());
        nextPaintTick = start + paintLength;
        previewOn (snapKey (yToKey (e.y)), 0.78f);
        return;
    }

    Note n;
    n.start = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn));
    n.length = newNoteLength();
    n.key = snapKey (yToKey (e.y));
    n.channel = engine.getSelectedChannel();
    n.slide = slideButton.getToggleState();
    n.colour = noteColour;

    std::vector<Note> stamp;
    for (auto interval : chordIntervals())
        if (n.key + interval <= 127 && n.key + interval >= 0)
        {
            auto chordNote = n;
            chordNote.key = snapKey (n.key + interval); // with scale snap the chord follows the scale
            if (std::none_of (stamp.begin(), stamp.end(), [&chordNote] (const Note& o) { return o.key == chordNote.key; }))
                stamp.push_back (chordNote);
        }

    std::vector<int> added;
    editNotes ([&] (Pattern& p) { added = noteedit::paste (p, stamp, 0); });
    dragNote = added.front(); // the root leads the drag
    dragIndices = added;
    dragOriginals = stamp;

    if (added.size() > 1)
        setSelection (added);
    drag = Drag::move;
    grabOffset = 0;
    previewOn (n.key, n.velocity);
}

void PianoRollComponent::mouseDrag (const juce::MouseEvent& e)
{
    const auto snapOn = ! e.mods.isAltDown();
    // after the step below, the hint bar says what the gesture is doing
    const struct PostAfter
    {
        PianoRollComponent& roll;
        const juce::MouseEvent& event;
        ~PostAfter() { roll.postDragHint (event); }
    } postAfter { *this, e };

    switch (drag)
    {
        case Drag::none:
            return;

        case Drag::pan:
            scrollTicks = juce::jmax (0.0, panStartTicks - e.getDistanceFromDragStartX() / pixelsPerTick);
            scrollY = juce::jlimit (0, juce::jmax (0, 128 * rowHeight - gridArea().getHeight()), panStartY - e.getDistanceFromDragStartY());
            updateScrollBars();
            repaint();
            return;

        case Drag::keyboard:
        {
            const auto key = yToKey (e.y);
            if (key != previewKey)
                previewOn (key, keyVelocityAt (e.x, key));
            return;
        }

        case Drag::keyZoom:
        {
            // up = taller keys; the clicked key stays under the mouse
            const auto offset = e.getMouseDownY() - headerHeight;
            rowHeight = juce::jlimit (6, 32, keyZoomStartRow - e.getDistanceFromDragStartY() / 8);
            scrollY = juce::jmax (0, (127 - keyZoomKey) * rowHeight + rowHeight / 2 - offset);
            updateScrollBars();
            repaint();
            return;
        }

        case Drag::keySelect:
        {
            const auto k0 = yToKey (e.getMouseDownY()), k1 = yToKey (e.y);
            const auto& notes = currentPattern (engine.getSong()).notes;
            auto s = selectionBefore;
            for (auto i : channelNotes())
                if (const auto k = notes[(std::size_t) i].key; k >= juce::jmin (k0, k1) && k <= juce::jmax (k0, k1))
                    s.push_back (i);
            setSelection (s);
            return;
        }

        case Drag::rulerSelect:
        {
            const auto t0 = (double) snap (xToTick (juce::jmin (e.getMouseDownX(), e.x)), snapOn);
            const auto t1 = (double) snap (xToTick (juce::jmax (e.getMouseDownX(), e.x)) + (snapOn ? (double) gridTicks() : 0.0), snapOn);
            const auto& notes = currentPattern (engine.getSong()).notes;
            auto s = selectionBefore;
            for (auto i : channelNotes())
                if (const auto& n = notes[(std::size_t) i]; (double) n.start >= t0 && (double) n.start < t1)
                    s.push_back (i);
            rubberBand = juce::Rectangle<int>::leftTopRightBottom ((int) tickToX (t0), gridArea().getY(), (int) tickToX (t1), gridArea().getBottom());
            setSelection (s);
            return;
        }

        case Drag::velocity:
            applyVelocity (lastVelocityX, e.x, e.y);
            lastVelocityX = e.x;
            return;

        case Drag::eventDraw:
        case Drag::eventErase:
            drawEvents (lastVelocityX, e.x, e.y, drag == Drag::eventErase);
            lastVelocityX = e.x;
            return;

        case Drag::velocitySet:
            applyLaneValue (e.x, laneSetValue, lastVelocityX);
            lastVelocityX = e.x;
            return;

        case Drag::velocityLine:
        {
            // from the press to the mouse: notes in between follow the line, the others keep their value
            const auto lane = velocityArea();
            const auto value = juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - e.y) / (float) (lane.getHeight() - 8));
            const auto x0 = e.getMouseDownX(), x1 = e.x;
            const auto lo = juce::jmin (x0, x1) - 3, hi = juce::jmax (x0, x1) + 3;
            const auto targets = selectionOrAll();
            knifeFrom = { x0, juce::roundToInt ((float) lane.getBottom() - 2.0f - laneSetValue * (float) (lane.getHeight() - 8)) };
            knifeTo = e.getPosition();
            editNotes ([&] (Pattern& p) {
                for (auto i : targets)
                {
                    if ((std::size_t) i >= laneOriginals.size() || (std::size_t) i >= p.notes.size())
                        continue;
                    auto& n = p.notes[(std::size_t) i];
                    n = laneOriginals[(std::size_t) i];
                    const auto x = (int) tickToX ((double) n.start);
                    if (x >= lo && x <= hi)
                        setLaneValue (n, (float) gesture::lineValue (x0, laneSetValue, x1, value, x));
                }
            });
            repaint();
            return;
        }

        case Drag::select:
        case Drag::zoomRange:
        {
            if (e.getDistanceFromDragStart() < 3 && rubberBand.isEmpty())
                return;

            const auto area = gridArea();
            rubberBand = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition()).getIntersection (area);

            if (drag == Drag::zoomRange)
            {
                rubberBand = rubberBand.withY (area.getY()).withHeight (area.getHeight());
                repaint();
                return;
            }

            const auto t0 = xToTick (rubberBand.getX()), t1 = xToTick (rubberBand.getRight());
            const auto kTop = yToKey (rubberBand.getY()), kBottom = yToKey (rubberBand.getBottom());
            const auto& notes = currentPattern (engine.getSong()).notes;
            auto s = selectionBefore;

            for (auto i : channelNotes())
            {
                const auto& n = notes[(std::size_t) i];

                if ((double) (n.start + n.length) > t0 && (double) n.start < t1 && n.key <= kTop && n.key >= kBottom)
                    s.push_back (i);
            }

            setSelection (s);
            return;
        }

        case Drag::erase:
        {
            // right press-and-hold on empty grid, then drag: a temporary Knife
            if (e.mods.isRightButtonDown() && startedOnEmpty && ! erasedSomething && ! holdExpired)
            {
                if (e.getLengthOfMousePress() >= 350)
                {
                    drag = Drag::knife;
                    knifeFrom = e.getMouseDownPosition();
                    knifeTo = e.getPosition();
                    repaint();
                    return;
                }

                if (e.getDistanceFromDragStart() > 3)
                    holdExpired = true; // moved right away: it is an erase stroke
            }

            bool edge = false;
            const auto hit = noteAt (e.x, e.y, edge);

            if (hit >= 0)
            {
                erasedSomething = true;
                editNotes ([hit] (Pattern& p) { noteedit::remove (p, { hit }); });
            }
            return;
        }

        case Drag::muting:
        {
            bool edge = false;
            const auto hit = noteAt (e.x, e.y, edge);

            if (hit >= 0 && std::find (touchedNotes.begin(), touchedNotes.end(), hit) == touchedNotes.end())
            {
                touchedNotes.push_back (hit);
                editNotes ([hit, m = muteTarget] (Pattern& p) { p.notes[(std::size_t) hit].muted = m; });
            }
            return;
        }

        case Drag::knife:
            knifeTo = e.getPosition();
            repaint();
            return;

        case Drag::listen:
            listenAt (e.x);
            return;

        case Drag::seek:
            seekAt (e.x, snapOn);
            return;

        case Drag::drumPaint:
            drumPaintAt (e.getPosition(), snapOn);
            return;

        case Drag::stretch:
        {
            if (dragOriginals.empty() || stretchEnd <= stretchAnchor)
                return;

            // the new end of the selection: on the grid (the nearest line), or free with Alt
            const auto minimum = snapOn ? juce::jmax<std::int64_t> (1, gridTicks()) : (std::int64_t) 10;
            auto end = snapOn ? snap (xToTick (e.x) + (double) gridTicks() / 2.0, true) : (std::int64_t) xToTick (e.x);
            end = juce::jmax (stretchAnchor + minimum, end);
            const auto stretched = polish::stretchNotes (dragOriginals, stretchAnchor, stretchEnd, end);

            editNotes ([this, &stretched] (Pattern& p) {
                for (std::size_t j = 0; j < dragIndices.size() && j < stretched.size(); ++j)
                    if ((std::size_t) dragIndices[j] < p.notes.size())
                        p.notes[(std::size_t) dragIndices[j]] = stretched[j];
            });
            repaint();
            return;
        }

        case Drag::paint:
        {
            // lays notes to the right, one per default length, at the pitch under the mouse
            const auto tick = snap (xToTick (e.x), snapOn);
            for (int guard = 0; tick >= nextPaintTick && guard < 64; ++guard)
            {
                paintNoteAt (nextPaintTick, snapKey (yToKey (e.y)), e.mods.isShiftDown());
                nextPaintTick += paintLength;
            }

            if (snapKey (yToKey (e.y)) != previewKey)
                previewOn (snapKey (yToKey (e.y)), 0.78f);
            return;
        }

        case Drag::move:
        {
            const auto& grabbed = dragOriginals[(std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragNote) - dragIndices.begin())];
            auto deltaTicks = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn) + grabOffset) - grabbed.start;
            auto deltaKeys = snapKey (yToKey (e.y)) - grabbed.key;

            // locks: Shift pressed after the click keeps the pitch, Ctrl keeps the time
            if (e.mods.isShiftDown() && ! downMods.isShiftDown())
                deltaKeys = 0;
            if (e.mods.isCtrlDown() && ! downMods.isCtrlDown())
                deltaTicks = 0;

            const auto& current = currentPattern (engine.getSong()).notes[(std::size_t) dragNote];

            if (current.start == grabbed.start + deltaTicks && current.key == grabbed.key + deltaKeys)
                return;

            const auto oldKey = current.key;

            // from the original positions: the group keeps its shape even when hitting the limits
            editNotes ([this, &deltaTicks, &deltaKeys] (Pattern& p) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                    p.notes[(std::size_t) dragIndices[j]] = dragOriginals[j];

                noteedit::move (p, dragIndices, deltaTicks, deltaKeys);
            });

            const auto& moved = currentPattern (engine.getSong()).notes[(std::size_t) dragNote];

            if (moved.key != oldKey)
                previewOn (moved.key, moved.velocity);
            return;
        }

        case Drag::resize:
        {
            const auto& grabbed = dragOriginals[(std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragNote) - dragIndices.begin())];
            const auto minimum = snapOn ? gridTicks() : (std::int64_t) 10;
            const auto end = snapOn ? snap (xToTick (e.x) + (double) gridTicks() / 2.0, true) : (std::int64_t) xToTick (e.x);
            const auto delta = juce::jmax (minimum, end - grabbed.start) - grabbed.length;

            if (currentPattern (engine.getSong()).notes[(std::size_t) dragNote].length == grabbed.length + delta)
                return;

            lastLength = grabbed.length + delta;
            editNotes ([this, delta, minimum] (Pattern& p) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                    p.notes[(std::size_t) dragIndices[j]].length = juce::jmax (minimum, dragOriginals[j].length + delta);
            });
            return;
        }

        case Drag::resizeLeft:
        {
            // the start moves, the end stays (selected notes together)
            const auto& grabbed = dragOriginals[(std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragNote) - dragIndices.begin())];
            const auto minimum = snapOn ? gridTicks() : (std::int64_t) 10;
            auto delta = juce::jmax<std::int64_t> (0, snap (xToTick (e.x) + (snapOn ? (double) gridTicks() / 2.0 : 0.0), snapOn)) - grabbed.start;

            for (const auto& o : dragOriginals)
                delta = juce::jlimit (-o.start, o.length - juce::jmin (minimum, o.length), delta);

            if (currentPattern (engine.getSong()).notes[(std::size_t) dragNote].start == grabbed.start + delta)
                return;

            editNotes ([this, delta] (Pattern& p) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                {
                    auto& n = p.notes[(std::size_t) dragIndices[j]];
                    n.start = dragOriginals[j].start + delta;
                    n.length = dragOriginals[j].length - delta;
                }
            });
            lastLength = grabbed.length - delta;
            return;
        }
    }
}

void PianoRollComponent::postDragHint (const juce::MouseEvent& e)
{
    const auto& notes = currentPattern (engine.getSong()).notes;
    const auto count = juce::String ((int) juce::jmax<std::size_t> (1, dragIndices.size()));
    auto steps = [] (std::int64_t ticks) {
        const auto value = (double) ticks / ticksPerStep;
        return (value > 0 ? "+" : "") + juce::String (value, std::abs (value - std::round (value)) < 1e-9 ? 0 : 2);
    };
    ui::Hint hint;

    switch (drag)
    {
        case Drag::move:
        {
            const auto at = (std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragNote) - dragIndices.begin());
            if (dragNote < 0 || dragNote >= (int) notes.size() || at >= dragOriginals.size())
                return;
            const auto& now = notes[(std::size_t) dragNote];
            const auto& was = dragOriginals[at];
            hint.text = tr ("Moving {0} note(s): then [Shift] keeps the pitch  [Ctrl] keeps the time  [Alt] no snap", { count });
            hint.value = keyName (now.key) + "  " + juce::String (now.key - was.key >= 0 ? "+" : "") + juce::String (now.key - was.key) + " st  "
                         + steps (now.start - was.start) + " " + tr ("steps");
            break;
        }
        case Drag::resize:
        case Drag::resizeLeft:
            if (dragNote < 0 || dragNote >= (int) notes.size())
                return;
            hint.text = tr ("Resizing {0} note(s)  [Alt] no snap", { count });
            hint.value = juce::String ((double) notes[(std::size_t) dragNote].length / ticksPerStep, 2) + " " + tr ("steps");
            break;
        case Drag::eventDraw:
        case Drag::eventErase:
        {
            const auto lane = velocityArea();
            const auto value = juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - e.y) / (float) (lane.getHeight() - 8));
            hint.text = drag == Drag::eventErase ? tr ("Deleting events") : tr ("Drawing events: {0}", { laneBox.getText() });
            hint.value = juce::String (juce::roundToInt (value * 100.0f)) + "%";
            break;
        }
        case Drag::velocity:
        case Drag::velocitySet:
        case Drag::velocityLine:
        {
            const auto lane = velocityArea();
            const auto value = drag == Drag::velocitySet ? laneSetValue
                                                         : juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - e.y) / (float) (lane.getHeight() - 8));
            hint.text = tr ("Setting {0}", { laneBox.getText() });
            hint.value = juce::String (juce::roundToInt (value * 100.0f)) + "%";
            break;
        }
        case Drag::select:
        case Drag::keySelect:
        case Drag::rulerSelect:
            hint.text = tr ("Selecting  [Shift] adds to the selection");
            hint.value = tr ("{0} selected", { juce::String ((int) selection.size()) });
            break;
        case Drag::erase: hint.text = tr ("Deleting what the mouse crosses"); break;
        case Drag::muting: hint.text = tr ("Muting / unmuting what the mouse crosses"); break;
        case Drag::knife: hint.text = tr ("Cutting along the line  [Alt] no snap"); break;
        case Drag::paint: hint.text = tr ("Painting notes  [Alt] no snap"); break;
        case Drag::drumPaint: hint.text = tr ("Drum brush: one hit per grid cell, on the key under the mouse  [Alt] no snap"); break;
        case Drag::stretch:
        {
            std::int64_t from = 0, to = 0;
            std::vector<Note> now;
            for (auto i : dragIndices)
                if (i >= 0 && i < (int) notes.size())
                    now.push_back (notes[(std::size_t) i]);
            noteedit::span (now, from, to);
            hint.text = tr ("Stretching {0} note(s) in time  [Alt] no snap", { count });
            hint.value = "x" + juce::String ((double) (to - stretchAnchor) / (double) juce::jmax<std::int64_t> (1, stretchEnd - stretchAnchor), 2);
            break;
        }
        case Drag::keyZoom: hint.text = tr ("Key height"); hint.value = juce::String (rowHeight) + " px"; break;
        case Drag::keyboard:
            hint.text = tr ("Hearing the key: further right on a key = louder");
            hint.value = keyName (previewKey) + "  " + tr ("vel {0}", { juce::String (juce::roundToInt (keyVelocityAt (e.x, previewKey) * 127.0f)) });
            break;
        case Drag::pan:
        case Drag::seek: hint.text = tr ("Position  [Alt] no snap"); break;
        case Drag::listen:
        case Drag::zoomRange:
        case Drag::none: return;
    }

    ui::postHint (hint);
}

void PianoRollComponent::mouseUp (const juce::MouseEvent& e)
{
    ui::clearPostedHint();
    if (drag == Drag::knife)
        cutWithKnife (knifeFrom, knifeTo, ! e.mods.isAltDown());

    if (drag == Drag::zoomRange)
    {
        if (rubberBand.getWidth() > 4)
            zoomToTicks (xToTick (rubberBand.getX()), xToTick (rubberBand.getRight()));
        else
            zoomIn (e.x); // a click zooms in around the point (right-click zooms out)
    }

    if (drag == Drag::select && rubberBand.isEmpty() && dragNote >= 0)
    {
        // Lasso click on a note: selects it (Shift: adds / removes)
        auto s = selectionBefore;
        if (auto it = std::find (s.begin(), s.end(), dragNote); it != s.end())
            s.erase (it);
        else
            s.push_back (dragNote);
        setSelection (s);
    }

    if (drag == Drag::listen)
        stopListening();

    if (drag == Drag::move || drag == Drag::resize || drag == Drag::resizeLeft || drag == Drag::paint || drag == Drag::drumPaint
        || drag == Drag::stretch)
        sortNotes();

    previewOff();
    drag = Drag::none;
    dragNote = -1;
    dragIndices.clear();
    dragOriginals.clear();
    touchedNotes.clear();
    laneOriginals.clear();
    rubberBand = {};
    hover = {};
    updateHover (e.getPosition(), e.mods); // the cursor of what is under the mouse now
    repaint();
}

void PianoRollComponent::mouseMove (const juce::MouseEvent& e)
{
    updateHover (e.getPosition(), e.mods);
}

void PianoRollComponent::mouseExit (const juce::MouseEvent&)
{
    lastMouse = { -1, -1 };

    if (! (hover == Hover {}))
    {
        hover = {};
        repaint();
    }
}

void PianoRollComponent::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    // Ctrl / Shift / Alt change what a click would do: the cursor follows without moving the mouse
    if (drag == Drag::none && lastMouse.x >= 0)
        updateCursor (mods);
}

PianoRollComponent::Hover PianoRollComponent::hoverAt (juce::Point<int> p) const
{
    Hover h;

    if (p.x < keyboardWidth && p.y >= headerHeight && p.y < gridArea().getBottom())
    {
        h.key = yToKey (p.y);
        return h;
    }

    if (velocityArea().contains (p))
    {
        // the stem nearest to the mouse (within a few pixels)
        const auto& notes = currentPattern (engine.getSong()).notes;
        auto best = 5.0;
        for (auto i : channelNotes())
            if (const auto d = std::abs (tickToX ((double) notes[(std::size_t) i].start) - p.x); d < best)
            {
                best = d;
                h.note = i;
            }
        return h;
    }

    if (! gridArea().contains (p))
        return h;

    h.key = yToKey (p.y);

    if (stretchHandle().expanded (2.0f).contains (p.toFloat()))
    {
        h.stretch = true;
        return h;
    }

    const auto& notes = currentPattern (engine.getSong()).notes;
    gesture::EdgeRules rules;
    rules.allowLeft = resizeFromLeft;

    // back to front: the note drawn on top wins
    for (int i = (int) notes.size() - 1; i >= 0; --i)
    {
        const auto& n = notes[(std::size_t) i];

        if (! edits (n) || n.key != h.key)
            continue;

        if (const auto zone = gesture::zoneAt (p.x, tickToX ((double) n.start), tickToX ((double) (n.start + n.length)), rules);
            zone != gesture::Zone::outside)
        {
            h.note = i;
            h.zone = zone;
            return h;
        }
    }

    h.ghost = ghostAt (p.x, p.y);
    return h;
}

gesture::Target PianoRollComponent::targetAt (const Hover& h) const
{
    return h.note >= 0 ? gesture::targetFor (h.zone) : gesture::Target::empty;
}

void PianoRollComponent::updateHover (juce::Point<int> p, juce::ModifierKeys mods)
{
    lastMouse = p;
    const auto h = hoverAt (p);

    if (! (h == hover))
    {
        // repaint only what changed: the old and new note rows, the keyboard, the lane
        auto dirty = [this] (const Hover& o) {
            if (o.key >= 0)
                repaint (0, keyToY (o.key) - 1, getWidth(), rowHeight + 2);
            const auto& notes = currentPattern (engine.getSong()).notes;
            for (auto i : { o.note, o.ghost })
                if (i >= 0 && i < (int) notes.size())
                {
                    repaint (0, keyToY (notes[(std::size_t) i].key) - 1, getWidth(), rowHeight + 2);
                    repaint (velocityArea());
                }
        };
        dirty (hover);
        hover = h;
        dirty (hover);
    }

    if (drag == Drag::none)
        updateCursor (mods);
}

void PianoRollComponent::updateCursor (juce::ModifierKeys mods)
{
    const auto p = lastMouse;
    auto cursor = gesture::Cursor::normal;

    if (p.x < keyboardWidth && p.y >= headerHeight && p.y < gridArea().getBottom())
        cursor = mods.isCtrlDown() ? gesture::Cursor::select : gesture::Cursor::point;
    else if (velocityArea().contains (p))
        cursor = gesture::Cursor::draw;
    else if (gridArea().contains (p) && hover.stretch && ! mods.isCtrlDown())
        cursor = gesture::Cursor::resizeH;
    else if (gridArea().contains (p) && editGhosts && hover.note < 0 && hover.ghost >= 0 && ! mods.isCtrlDown())
        cursor = gesture::Cursor::move;
    else if (gridArea().contains (p))
    {
        const gesture::Mods m { mods.isShiftDown(), mods.isCtrlDown(), mods.isAltDown() };
        const auto tool = tools.getTool();
        cursor = gesture::cursorFor (gesture::leftAction (tool, targetAt (hover), m, false), tool);
    }
    else if (p.y >= rulerTop && p.y < headerHeight && p.x >= keyboardWidth && mods.isCtrlDown())
        cursor = gesture::Cursor::select;

    setMouseCursor (editorCursor (cursor));
}

ui::Hint PianoRollComponent::hintAt (juce::Point<int> p)
{
    ui::Hint hint;
    const auto mods = juce::ModifierKeys::currentModifiers;
    const auto& song = engine.getSong();
    const auto& notes = currentPattern (song).notes;
    const auto h = hoverAt (p);
    auto noteValue = [&] (int i) {
        const auto& n = notes[(std::size_t) i];
        return keyName (n.key) + "  " + juce::String (juce::roundToInt (n.velocity * 100.0f)) + "%";
    };

    if (p.x < keyboardWidth && p.y >= headerHeight && p.y < gridArea().getBottom())
    {
        hint.text = tr ("Keyboard: [LMB] hear the key (further right = louder)  [Ctrl][LMB] select its notes  [MMB] drag: taller / shorter keys");
        hint.value = keyName (h.key) + "  " + tr ("vel {0}", { juce::String (juce::roundToInt (keyVelocityAt (p.x, h.key) * 127.0f)) });
        return hint;
    }

    if (velocityArea().contains (p))
    {
        hint.text = tr ("{0} lane: [LMB] drag draws  [RMB] drag draws a line  [Shift][LMB] one value  [Alt][LMB] reset  [Alt][Wheel] over a note: adjust",
                        { laneBox.getText() });
        if (h.note >= 0)
            hint.value = noteValue (h.note);
        return hint;
    }

    if (p.y >= rulerTop && p.y < headerHeight && p.x >= keyboardWidth)
    {
        hint.text = tr ("Ruler: [LMB] position  [Alt] no snap  [Ctrl][LMB] drag selects the notes in a time range  [Ctrl][Wheel] zoom  [Shift][Wheel] scroll");
        return hint;
    }

    if (! gridArea().contains (p))
        return hint;

    if (h.stretch)
    {
        hint.text = tr ("Stretch handle: [LMB] drag stretches or squeezes the selected notes in time (from the selection's start)  [Alt] no snap");
        hint.value = tr ("{0} selected", { juce::String ((int) selection.size()) });
        return hint;
    }

    if (h.note < 0 && h.ghost >= 0)
    {
        const auto ghostChannel = notes[(std::size_t) h.ghost].channel;
        const auto name = ghostChannel >= 0 && ghostChannel < (int) song.channels.size()
                              ? juce::String::fromUTF8 (song.channels[(std::size_t) ghostChannel].name.c_str())
                              : juce::String();
        hint.text = editGhosts ? tr ("Ghost note of {0} (editable): [LMB] edit it (switches to {0})  [RMB] deletes it  [Ctrl][Alt][V] read-only ghosts",
                                     { name })
                               : tr ("Ghost note of {0}: double [RMB] edits that channel  [Alt][V] hides ghost notes  [Ctrl][Alt][V] makes them editable",
                                     { name });
        hint.value = keyName (notes[(std::size_t) h.ghost].key);
        return hint;
    }

    const gesture::Mods m { mods.isShiftDown(), mods.isCtrlDown(), mods.isAltDown() };
    hint.text = actionHint (gesture::leftAction (tools.getTool(), targetAt (h), m, false), false);
    hint.value = h.note >= 0 ? noteValue (h.note) : keyName (h.key);
    return hint;
}

void PianoRollComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (! gridArea().contains (e.getPosition()) || e.mods.isRightButtonDown() || e.mods.isCtrlDown())
        return;

    // the first click of this double click created the note: that was drawing, not opening it
    if (juce::Time::getMillisecondCounter() - createdAt < 800)
        return;

    bool edge = false;
    const auto hit = noteAt (e.x, e.y, edge);

    if (hit < 0)
        return;

    const auto indices = isSelected (hit) ? selection : std::vector<int> { hit };
    const auto& n = currentPattern (engine.getSong()).notes[(std::size_t) hit];
    const auto area = juce::Rectangle<int> ((int) tickToX ((double) n.start), keyToY (n.key), juce::jmax (8, (int) (n.length * pixelsPerTick)), rowHeight);
    showNoteInspector (engine, indices, hit, localAreaToGlobal (area));
}

std::unique_ptr<juce::Component> PianoRollComponent::makeNoteInspectorForSnapshot (bool singleNote)
{
    if (selection.empty())
        selectAll();

    if (selection.empty())
        return nullptr;

    return makeNoteInspector (engine, singleNote ? std::vector<int> { selection.front() } : selection, selection.front());
}

ui::Hint PianoRollComponent::hoverForSnapshot()
{
    rowHeight = 18;
    pixelsPerTick = 64.0 / ticksPerStep;
    scrollToNotes (false);
    const auto notes = channelNotes();

    if (notes.empty())
        return {};

    const auto& n = currentPattern (engine.getSong()).notes[(std::size_t) notes.front()];
    const auto at = juce::Point<int> ((int) tickToX ((double) (n.start + n.length)) - 2, keyToY (n.key) + rowHeight / 2);
    updateHover (at, {});
    return hintAt (at);
}

// --- tools ---------------------------------------------------------------------------

void PianoRollComponent::cutWithKnife (juce::Point<int> from, juce::Point<int> to, bool snapOn)
{
    const auto& notes = currentPattern (engine.getSong()).notes;
    std::vector<std::pair<int, std::int64_t>> cuts;

    if (from.getDistanceFrom (to) < 3)
    {
        // a click: cuts the note under the cursor
        bool edge = false;
        if (const auto hit = noteAt (from.x, from.y, edge); hit >= 0)
            cuts.push_back ({ hit, snap (xToTick (from.x) + (snapOn ? (double) gridTicks() / 2.0 : 0.0), snapOn) });
    }
    else
    {
        // a line: every note row it crosses is cut where the line passes
        const auto top = juce::jmin (from.y, to.y), bottom = juce::jmax (from.y, to.y);

        for (int i = 0; i < (int) notes.size(); ++i)
        {
            const auto& n = notes[(std::size_t) i];

            if (! edits (n))
                continue;

            const auto rowY = keyToY (n.key) + rowHeight / 2;

            if (rowY < top - rowHeight / 2 || rowY > bottom + rowHeight / 2)
                continue;

            const auto y = juce::jlimit (top, bottom, rowY);
            const auto x = from.y == to.y ? (double) from.x : from.x + (double) (y - from.y) * (to.x - from.x) / (double) (to.y - from.y);
            const auto tick = snap (xToTick ((int) std::round (x)) + (snapOn ? (double) gridTicks() / 2.0 : 0.0), snapOn);

            if (tick > n.start && tick < n.start + n.length)
                cuts.push_back ({ i, tick });
        }
    }

    if (cuts.empty())
        return;

    setSelection ({});
    editNotes ([&cuts] (Pattern& p) {
        for (auto [index, tick] : cuts)
            noteedit::split (p, index, tick);
    });
    sortNotes();
}

void PianoRollComponent::zoomToTicks (double from, double to)
{
    if (to - from < 1.0)
        return;

    pixelsPerTick = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, gridArea().getWidth() / (to - from));
    scrollTicks = juce::jmax (0.0, from);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::zoomIn (int anchorX)
{
    const auto anchor = xToTick (anchorX);
    pixelsPerTick = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, pixelsPerTick * 2.0);
    scrollTicks = juce::jmax (0.0, anchor - (anchorX - keyboardWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::zoomOut (int anchorX)
{
    const auto anchor = xToTick (anchorX);
    pixelsPerTick = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, pixelsPerTick / 2.0);
    scrollTicks = juce::jmax (0.0, anchor - (anchorX - keyboardWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::seekAt (int x, bool snapOn)
{
    const auto& song = engine.getSong();
    const auto tick = snapOn ? (double) snap (xToTick (x), true) : xToTick (x);
    const auto target = transportedit::patternSeekTarget (song, song.currentPattern, tick, engine.getTransport().positionTicks.load());
    if (target < 0.0)
    {
        // Song mode and no clip of this pattern: play the pattern on its own from there
        engine.editSong ([] (Song& s) { s.mode = PlayMode::pattern; }, false);
        engine.seek (juce::jmax (0.0, tick));
        return;
    }
    engine.seek (target);
}

void PianoRollComponent::listenAt (int x)
{
    // plays the channel's notes that sound at the tick under the mouse
    const auto tick = (std::int64_t) xToTick (x);
    const auto& notes = currentPattern (engine.getSong()).notes;
    const auto channel = engine.getSelectedChannel();
    std::vector<int> keys;
    std::vector<float> velocities;

    for (const auto& n : notes)
        if (n.channel == channel && ! n.muted && n.start <= tick && n.start + n.length > tick
            && std::find (keys.begin(), keys.end(), n.key) == keys.end())
        {
            keys.push_back (n.key);
            velocities.push_back (n.velocity);
        }

    for (auto k : listenKeys)
        if (std::find (keys.begin(), keys.end(), k) == keys.end())
            listenNotes.release (listenChannel, k);

    if (channel != listenChannel)
        listenNotes.releaseAll();

    for (std::size_t i = 0; i < keys.size(); ++i)
        if (! listenNotes.isHeld (channel, keys[i]))
            listenNotes.press (channel, keys[i], velocities[i]);

    listenKeys = keys;
    listenChannel = channel;
}

void PianoRollComponent::stopListening()
{
    listenNotes.releaseAll();
    listenKeys.clear();
}

void PianoRollComponent::refreshChannelBox()
{
    const auto& song = engine.getSong();
    std::vector<bool> hasNotes (song.channels.size(), false);

    for (const auto& n : currentPattern (song).notes)
        if (n.channel >= 0 && n.channel < (int) hasNotes.size())
            hasNotes[(std::size_t) n.channel] = true;

    juce::StringArray names;
    names.add (tr ("All")); // (issue #12: every channel's notes; drawn bold)
    for (std::size_t c = 0; c < song.channels.size(); ++c)
        names.add ((hasNotes[c] ? juce::String::fromUTF8 ("\xe2\x97\x8f ") : juce::String ("    ")) + juce::String::fromUTF8 (song.channels[c].name.c_str()));

    // only rebuilds when something changed (rebuilding closes an open menu)
    bool same = channelBox.getNumItems() == names.size();
    for (int i = 0; same && i < names.size(); ++i)
        same = channelBox.getItemText (i) == names[i];

    if (! same)
    {
        // the bold "All" at the top, as a custom item (a ComboBox item has no font of its own)
        struct BoldItem final : juce::PopupMenu::CustomComponent
        {
            explicit BoldItem (juce::String t) : text (std::move (t)) {}
            void getIdealSize (int& w, int& h) override
            {
                w = 160;
                h = 24;
            }
            void paint (juce::Graphics& g) override
            {
                if (isItemHighlighted())
                {
                    g.setColour (theme::colour::accent.withAlpha (0.35f));
                    g.fillRect (getLocalBounds());
                }
                g.setColour (theme::colour::textPrimary);
                g.setFont (theme::font (theme::type::body, true));
                g.drawText (text, getLocalBounds().withTrimmedLeft (12), juce::Justification::centredLeft, true);
            }
            juce::String text;
        };
        channelBox.clear (juce::dontSendNotification);
        juce::PopupMenu::Item all (names[0]);
        all.itemID = allChannelsId;
        all.customComponent = new BoldItem (names[0]);
        channelBox.getRootMenu()->addItem (std::move (all));
        channelBox.getRootMenu()->addSeparator();
        for (int i = 1; i < names.size(); ++i)
            channelBox.addItem (names[i], i);
    }

    channelBox.setSelectedId (allChannels ? allChannelsId : engine.getSelectedChannel() + 1, juce::dontSendNotification);
}

void PianoRollComponent::setAllChannels (bool on)
{
    if (allChannels == on)
        return;
    allChannels = on;
    selection.clear();
    stopListening();
    refreshChannelBox();
    repaint();
}

void PianoRollComponent::stepChannel (int direction, bool withNotesOnly)
{
    const auto& song = engine.getSong();
    const auto count = (int) song.channels.size();

    if (count == 0)
        return;

    std::vector<bool> hasNotes ((std::size_t) count, false);
    for (const auto& n : currentPattern (song).notes)
        if (n.channel >= 0 && n.channel < count)
            hasNotes[(std::size_t) n.channel] = true;

    auto channel = engine.getSelectedChannel();

    for (int step = 0; step < count; ++step)
    {
        channel += direction;

        if (channel < 0 || channel >= count)
            return; // stops at the ends

        if (! withNotesOnly || hasNotes[(std::size_t) channel])
        {
            engine.setSelectedChannel (channel);
            return;
        }
    }
}

void PianoRollComponent::runOperation (const std::function<std::vector<int> (Pattern&, const std::vector<int>&)>& op)
{
    const auto target = selectionOrAll();

    if (target.empty())
        return;

    const auto hadSelection = ! selection.empty();
    std::vector<int> result;
    editNotes ([&] (Pattern& p) { result = op (p, target); });
    setSelection (hadSelection ? result : std::vector<int> {});
    sortNotes();
}

void PianoRollComponent::showMoreMenu()
{
    const auto grid = gridTicks();
    const auto bar = (std::int64_t) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Note operations (selection, or all notes)"));
    menu.addItem (tr ("Chop to grid") + "\tCtrl+U", [this, grid] { runOperation ([grid] (Pattern& p, const std::vector<int>& t) { return noteedit::chop (p, t, grid); }); });
    menu.addItem (tr ("Join touching notes") + "\tCtrl+G", [this] { runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::join (p, t); return std::vector<int> {}; }); });
    menu.addItem (tr ("Make legato") + "\tCtrl+L", [this] { runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::legato (p, t); return t; }); });
    menu.addItem (tr ("Shrink to grid") + "\tShift+D", [this, grid] { runOperation ([grid] (Pattern& p, const std::vector<int>& t) { noteedit::shrinkToGrid (p, t, grid); return t; }); });
    menu.addItem (tr ("Quantise starts") + keyText ("pianoRoll.quantizeStarts"), [this] { quantizeOperation (0); });
    menu.addItem (tr ("Quantise starts 50% (halfway to the grid)") + keyText ("pianoRoll.quantizeHalf"), [this] { quantizeOperation (2); });
    menu.addItem (tr ("Quantise starts and lengths") + keyText ("pianoRoll.quickQuantize"), [this] { quantizeOperation (1); });
    menu.addItem (tr ("Rotate left within the bar") + "\tCtrl+Shift+Left", [this, grid, bar] { runOperation ([grid, bar] (Pattern& p, const std::vector<int>& t) { noteedit::rotate (p, t, grid, bar, -1); return t; }); });
    menu.addItem (tr ("Rotate right within the bar") + "\tCtrl+Shift+Right", [this, grid, bar] { runOperation ([grid, bar] (Pattern& p, const std::vector<int>& t) { noteedit::rotate (p, t, grid, bar, 1); return t; }); });
    menu.addSeparator();
    menu.addSubMenu (tr ("Tools"), toolsMenu());
    menu.addSubMenu (tr ("Select"), selectMenu());
    {
        juce::PopupMenu zoom;
        zoom.addItem (tr ("Zoom in") + "\tPageUp", [this] { runAction ("pianoRoll.zoomIn"); });
        zoom.addItem (tr ("Zoom out") + "\tPageDown", [this] { runAction ("pianoRoll.zoomOut"); });
        zoom.addItem (tr ("Zoom: one bar") + "\tShift+1", [this] { runAction ("pianoRoll.zoom1"); });
        zoom.addItem (tr ("Zoom: four bars") + "\tShift+2", [this] { runAction ("pianoRoll.zoom2"); });
        zoom.addItem (tr ("Zoom: sixteen bars") + "\tShift+3", [this] { runAction ("pianoRoll.zoom3"); });
        zoom.addItem (tr ("Zoom to show everything") + "\tShift+4", [this] { runAction ("pianoRoll.zoomAll"); });
        zoom.addItem (tr ("Zoom to the selection") + "\tShift+5", [this] { runAction ("pianoRoll.zoomSelection"); });
        zoom.addItem (tr ("Centre the view on the playhead") + "\tShift+0", [this] { runAction ("pianoRoll.centrePlayhead"); });
        zoom.addSeparator();
        zoom.addItem (tr ("Taller keys") + "\tCtrl+Alt+Wheel", [this] { rowHeight = juce::jmin (32, rowHeight + 2); updateScrollBars(); repaint(); });
        zoom.addItem (tr ("Shorter keys"), [this] { rowHeight = juce::jmax (6, rowHeight - 2); updateScrollBars(); repaint(); });
        menu.addSubMenu (tr ("Zoom"), zoom);
    }
    menu.addSeparator();
    menu.addItem (tr ("Snap notes to the scale"), scaleIndex() > 0, scaleSnap, [this] { setScaleSnap (! scaleSnap); });
    menu.addItem (tr ("Chord stamp: the clicked key is the top note"), true, chordTopDown, [this] { chordTopDown = ! chordTopDown; });
    menu.addItem (tr ("Move the notes onto the scale"), scaleIndex() > 0, false, [this] {
        const auto root = scaleRoot(), scale = scaleIndex();
        runOperation ([root, scale] (Pattern& p, const std::vector<int>& t) { notetools::snapNotesToScale (p, t, root, scale); return t; });
    });
    menu.addSeparator();
    menu.addItem (tr ("Resize notes from the left edge too") + "\tCtrl+Alt+Home", true, resizeFromLeft, [this] { resizeFromLeft = ! resizeFromLeft; });
    menu.addItem (tr ("Show ghost notes") + "\tAlt+V", true, showGhosts,
                  [this] { ghostButton.setToggleState (! ghostButton.getToggleState(), juce::sendNotificationSync); });
    menu.addItem (tr ("Ghost notes are editable") + "\tCtrl+Alt+V", true, editGhosts, [this] {
        editGhosts = ! editGhosts;
        if (editGhosts && ! showGhosts)
            ghostButton.setToggleState (true, juce::sendNotificationSync);
        repaint();
    });
    menu.addSeparator();
    menu.addSubMenu ("MIDI", midiMenu());
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&moreButton));
}

juce::PopupMenu PianoRollComponent::toolsMenu()
{
    // FL's piano roll Tools menu (REA round 143, EXEC capture): its groups and keys, in its order
    juce::PopupMenu menu;
    const auto grid = gridTicks();
    auto tool = [this, &menu] (NoteTool t, const char* key) {
        menu.addItem (tr (noteToolTitle (t)) + "..." + (key != nullptr ? juce::String ("\t") + key : juce::String()), [this, t] { openNoteTool (t); });
    };
    menu.addSectionHeader (tr ("Built-in"));
    tool (NoteTool::riffMachine, "Alt+E");
    menu.addItem (tr ("Generate chord progression...") + "\tAlt+P", [this] { openNoteTool (NoteTool::chordProgression); });
    menu.addSeparator();
    menu.addItem (tr ("Quick legato") + "\tCtrl+L", [this] { runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::legato (p, t); return t; }); });
    tool (NoteTool::articulate, "Alt+L");
    menu.addSeparator();
    menu.addItem (tr ("Quick quantize") + keyText ("pianoRoll.quickQuantize"), [this] { quantizeOperation (1); });
    menu.addItem (tr ("Quick quantize start times") + keyText ("pianoRoll.quantizeStarts"), [this] { quantizeOperation (0); });
    menu.addItem (tr ("Quick quantize 50% (halfway to the grid)") + keyText ("pianoRoll.quantizeHalf"), [this] { quantizeOperation (2); });
    tool (NoteTool::quantize, "Alt+Q");
    menu.addSeparator();
    menu.addItem (tr ("Quick chop") + "\tCtrl+U", [this, grid] { runOperation ([grid] (Pattern& p, const std::vector<int>& t) { return noteedit::chop (p, t, grid); }); });
    tool (NoteTool::chop, "Alt+U");
    menu.addSeparator();
    menu.addItem (tr ("Glue notes") + "\tCtrl+G", [this] { runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::join (p, t); return std::vector<int> {}; }); });
    menu.addSeparator();
    tool (NoteTool::arpeggiate, "Alt+A");
    tool (NoteTool::strum, "Alt+S");
    tool (NoteTool::flam, "Alt+F");
    tool (NoteTool::claw, "Alt+W");
    tool (NoteTool::limit, "Alt+K");
    tool (NoteTool::flip, "Alt+Y");
    tool (NoteTool::randomise, "Alt+R");
    menu.addSeparator();
    tool (NoteTool::scaleLevels, "Alt+X");
    tool (NoteTool::lfo, "Alt+O");

    // FL's second column ("Scripts": Python tools shipped with FL); LYNX has its own built-in versions
    menu.addSectionHeader (tr ("More tools"));
    tool (NoteTool::euclidean, nullptr);
    tool (NoteTool::humanize, nullptr);
    tool (NoteTool::noteRepeat, nullptr);
    {
        juce::PopupMenu velocity;
        auto choose = [this] (std::function<std::vector<int> (const Pattern&, const std::vector<int>&)> pick) {
            const auto& song = engine.getSong();
            setSelection (pick (song.patterns[(std::size_t) song.currentPattern], channelNotes()));
        };
        velocity.addItem (tr ("Loudest quarter"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectLoudest (p, t, 0.25f, true); }); });
        velocity.addItem (tr ("Quietest quarter"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectLoudest (p, t, 0.25f, false); }); });
        velocity.addSeparator();
        for (const auto& [label, lo, hi] : { std::tuple { "75-100%", 0.75f, 1.0f }, { "50-75%", 0.5f, 0.75f }, { "25-50%", 0.25f, 0.5f }, { "0-25%", 0.0f, 0.25f } })
            velocity.addItem (tr ("Velocity {0}", { juce::String (label) }), [choose, lo = lo, hi = hi] {
                choose ([lo, hi] (const Pattern& p, const std::vector<int>& t) { return notetools::selectByVelocity (p, t, lo, hi); });
            });
        if (! selection.empty())
        {
            const auto& notes = engine.getSong().patterns[(std::size_t) engine.getSong().currentPattern].notes;
            const auto v = selection.front() < (int) notes.size() ? notes[(std::size_t) selection.front()].velocity : 0.0f;
            velocity.addSeparator();
            velocity.addItem (tr ("Same velocity as the selected note ({0}%)", { juce::String (juce::roundToInt (v * 100.0f)) }), [choose, v] {
                choose ([v] (const Pattern& p, const std::vector<int>& t) { return notetools::selectByVelocity (p, t, v - 0.005f, v + 0.005f); });
            });
        }
        menu.addSubMenu (tr ("Select by velocity"), velocity);
    }
    menu.addSeparator();
    menu.addItem (tr ("Edit events..."), [this] {
        openEventEditor (engine, { AutomationTargetKind::channelVolume, engine.getSelectedChannel() });
    });

    return menu;
}

void PianoRollComponent::openNoteTool (NoteTool tool)
{
    const auto target = selectionOrAll();

    if (target.empty() && ! noteToolGenerates (tool))
        return;

    showNoteTool (engine, tool, target, gridTicks(), [safe = juce::Component::SafePointer<PianoRollComponent> (this)] {
        if (safe != nullptr)
            safe->setSelection ({});
    });
}

void PianoRollComponent::zoomAroundCentre (double factor)
{
    const auto grid = gridArea();
    const auto centre = xToTick (grid.getCentreX());
    pixelsPerTick = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, pixelsPerTick * factor);
    scrollTicks = juce::jmax (0.0, centre - (grid.getCentreX() - keyboardWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::zoomToBars (int bars)
{
    const auto bar = (double) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    const auto from = std::floor (scrollTicks / bar) * bar;
    zoomToTicks (from, from + bars * bar);
}

void PianoRollComponent::zoomAll()
{
    const auto& song = engine.getSong();
    zoomToTicks (0.0, (double) currentPattern (song).effectiveLength (song.beatsPerBar));
}

void PianoRollComponent::zoomToSelection()
{
    const auto target = selectionOrAll();

    if (target.empty())
        return zoomAll();

    const auto& notes = currentPattern (engine.getSong()).notes;
    std::int64_t from = INT64_MAX, to = 0;
    int low = 127, high = 0;

    for (auto i : target)
    {
        const auto& n = notes[(std::size_t) i];
        from = juce::jmin (from, n.start);
        to = juce::jmax (to, n.start + n.length);
        low = juce::jmin (low, n.key);
        high = juce::jmax (high, n.key);
    }

    const auto margin = (double) ticksPerStep;
    zoomToTicks (juce::jmax (0.0, (double) from - margin), (double) to + margin);
    scrollY = juce::jmax (0, (127 - (low + high) / 2) * rowHeight - gridArea().getHeight() / 2);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::centreOnPlayhead()
{
    const auto& song = engine.getSong();
    const auto length = (double) juce::jmax<std::int64_t> (1, currentPattern (song).effectiveLength (song.beatsPerBar));
    auto tick = transportedit::patternPlayhead (song, song.currentPattern, engine.getTransport().positionTicks.load());
    if (tick < 0.0)
        tick = std::fmod (engine.getTransport().positionTicks.load(), length);
    scrollTicks = juce::jmax (0.0, tick - gridArea().getWidth() / pixelsPerTick / 2.0);
    updateScrollBars();
    repaint();
}

void PianoRollComponent::ColourSwatch::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (3.0f);
    g.setColour (theme::noteGroupColour (colourIndex, 0.85f));
    g.fillRoundedRectangle (r, 3.0f);
    g.setColour (over || down ? theme::colour::textPrimary : theme::colour::bg0.withAlpha (0.7f));
    g.drawRoundedRectangle (r, 3.0f, 1.0f);
    g.setColour (theme::colour::bg0.withAlpha (0.85f));
    g.setFont (theme::font (theme::type::caption, true));
    g.drawText (juce::String (colourIndex + 1), r, juce::Justification::centred, false);
}

void PianoRollComponent::colourSelection (int colour)
{
    const auto target = selection;
    if (target.empty())
        return;
    editNotes ([&] (Pattern& p) { notetools::setColour (p, target, colour); });
    repaint();
}

void PianoRollComponent::selectByColour (int colour)
{
    setSelection (notetools::selectByColour (currentPattern (engine.getSong()), channelNotes(), colour));
}

void PianoRollComponent::showColourMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader (tr ("Colour for new notes"));
    for (int c = 0; c < noteColourCount; ++c)
        menu.addColouredItem (c + 1, tr ("Colour {0}", { juce::String (c + 1) }), theme::noteGroupColour (c, 0.9f), true, c == noteColour);
    menu.addSeparator();
    menu.addItem (1000, tr ("Colour the selected notes with it") + "\tAlt+C", ! selection.empty());
    {
        juce::PopupMenu colourAs;
        for (int c = 0; c < noteColourCount; ++c)
            colourAs.addColouredItem (2001 + c, tr ("Colour {0}", { juce::String (c + 1) }), theme::noteGroupColour (c, 0.9f), true, false);
        menu.addSubMenu (tr ("Colour the selected notes"), colourAs, ! selection.empty());
    }
    {
        juce::PopupMenu select;
        const auto& pattern = currentPattern (engine.getSong());
        for (int c = 0; c < noteColourCount; ++c)
            select.addColouredItem (3001 + c, tr ("Colour {0}", { juce::String (c + 1) }), theme::noteGroupColour (c, 0.9f),
                                    ! notetools::selectByColour (pattern, channelNotes(), c).empty(), false);
        menu.addSubMenu (tr ("Select by colour") + "\tShift+C", select);
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&colourButton), [safe = juce::Component::SafePointer (this)] (int id) {
        if (safe == nullptr || id <= 0)
            return;
        if (id <= noteColourCount)
        {
            safe->noteColour = id - 1;
            safe->colourButton.colourIndex = id - 1;
            safe->colourButton.repaint();
        }
        else if (id == 1000)
            safe->colourSelection (safe->noteColour);
        else if (id > 2000 && id <= 2000 + noteColourCount)
            safe->colourSelection (id - 2001);
        else if (id > 3000 && id <= 3000 + noteColourCount)
            safe->selectByColour (id - 3001);
    });
}

juce::PopupMenu PianoRollComponent::selectMenu()
{
    // FL's piano roll Select submenu (its manual): all, invert, odd, random, muted, overlapping, stacked, by colour
    juce::PopupMenu menu;
    auto choose = [this] (std::function<std::vector<int> (const Pattern&, const std::vector<int>&)> pick) {
        setSelection (pick (currentPattern (engine.getSong()), channelNotes()));
    };
    menu.addItem (tr ("All") + "\tCtrl+A", [this] { selectAll(); });
    menu.addItem (tr ("Invert selection") + "\tShift+I", [this] { runAction ("pianoRoll.invertSelection"); });
    menu.addItem (tr ("Deselect") + "\tEsc", ! selection.empty(), false, [this] { setSelection ({}); });
    menu.addSeparator();
    menu.addItem (tr ("Odd notes (every other one)"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectOdd (p, t); }); });
    menu.addItem (tr ("Even notes"), [choose] {
        choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::invertSelection (t, notetools::selectOdd (p, t)); });
    });
    menu.addItem (tr ("Random (half of the notes)"), [choose] {
        const auto seed = (std::uint32_t) juce::Random::getSystemRandom().nextInt();
        choose ([seed] (const Pattern& p, const std::vector<int>& t) { return notetools::selectRandom (p, t, 0.5f, seed); });
    });
    menu.addItem (tr ("Muted notes"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectMuted (p, t); }); });
    menu.addItem (tr ("Overlapping notes"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectOverlapping (p, t); }); });
    menu.addItem (tr ("Stacked notes (hidden duplicates)"), [choose] { choose ([] (const Pattern& p, const std::vector<int>& t) { return notetools::selectStacked (p, t); }); });
    menu.addItem (tr ("Same colour as the selection") + "\tShift+C", [this] { runAction ("pianoRoll.selectByColour"); });
    menu.addSeparator();
    menu.addItem (tr ("Group the selected notes") + "\tShift+G", selection.size() > 1, false, [this] { runAction ("pianoRoll.groupNotes"); });
    menu.addItem (tr ("Ungroup the selected notes") + "\tAlt+G", ! selection.empty(), false, [this] { runAction ("pianoRoll.ungroupNotes"); });
    return menu;
}

void PianoRollComponent::showKeyMenu (int key)
{
    const auto channel = engine.getSelectedChannel();
    if (channel < 0 || channel >= (int) engine.getSong().channels.size() || key < 0 || key > 127)
        return;
    const auto& labels = engine.getSong().channels[(std::size_t) channel].keyLabels;
    const auto current = labels.find (key);
    auto setLabels = [this, channel] (std::function<void (std::map<int, std::string>&)> edit) {
        engine.editSong ([channel, edit] (Song& s) {
            if (channel < (int) s.channels.size())
                edit (s.channels[(std::size_t) channel].keyLabels);
        });
        repaint();
    };

    juce::PopupMenu menu;
    menu.addSectionHeader (keyName (key));
    menu.addItem (tr ("Rename this key..."), [key, setLabels, text = current != labels.end() ? current->second : std::string()] {
        auto* window = new juce::AlertWindow (tr ("Key label"), tr ("The name shown on {0} for this channel", { keyName (key) }), juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("label", juce::String::fromUTF8 (text.c_str()));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([window, key, setLabels] (int result) {
                                     if (result != 1)
                                         return;
                                     const auto label = window->getTextEditorContents ("label").trim().toStdString();
                                     setLabels ([key, label] (std::map<int, std::string>& l) {
                                         if (label.empty())
                                             l.erase (key);
                                         else
                                             l[key] = label;
                                     });
                                 }),
                                 true);
    });
    menu.addItem (tr ("Remove this key's label"), current != labels.end(), false,
                  [setLabels, key] { setLabels ([key] (std::map<int, std::string>& l) { l.erase (key); }); });
    menu.addSeparator();
    menu.addItem (tr ("Label the keys with the General MIDI drum map"), [setLabels] {
        setLabels ([] (std::map<int, std::string>& l) {
            for (int k = 35; k <= 81; ++k)
                if (const auto* name = notetools::generalMidiDrumName (k))
                    l[k] = name;
        });
    });
    menu.addItem (tr ("Remove all key labels of this channel"), ! labels.empty(), false,
                  [setLabels] { setLabels ([] (std::map<int, std::string>& l) { l.clear(); }); });
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void PianoRollComponent::updateChordReadout()
{
    auto keys = engine.heldInputKeys();
    if (keys.size() < 2)
    {
        keys.clear();
        const auto& notes = currentPattern (engine.getSong()).notes;
        for (auto i : selection)
            if (i >= 0 && i < (int) notes.size())
                keys.push_back (notes[(std::size_t) i].key);
    }
    const auto chord = prtools::detectChord (keys, notenames::flats());
    juce::String text;
    if (chord)
    {
        // the root and the bass in the chosen note names (Settings › General, round 165f): "Do m7/Mib"
        auto name = juce::String (chord->name);
        auto quality = name.fromFirstOccurrenceOf (juce::String (chord->name).substring (0, name.length() > 1 && (name[1] == '#' || name[1] == 'b') ? 2 : 1),
                                                   false, false).upToFirstOccurrenceOf ("/", false, false);
        text = juce::String (notenames::pitchName (chord->root)) + quality;
        if (name.contains ("/"))
            text << "/" << notenames::pitchName (chord->bass);
    }
    if (chordLabel.getText() != text)
        chordLabel.setText (text, juce::dontSendNotification);
}

bool PianoRollComponent::runAction (const std::string& id)
{
    auto is = [&id] (const char* name) { return id == name; };

    if (is ("pianoRoll.arpeggiate")) openNoteTool (NoteTool::arpeggiate);
    else if (is ("pianoRoll.strum")) openNoteTool (NoteTool::strum);
    else if (is ("pianoRoll.flip")) openNoteTool (NoteTool::flip);
    else if (is ("pianoRoll.randomise")) openNoteTool (NoteTool::randomise);
    else if (is ("pianoRoll.scaleLevels")) openNoteTool (NoteTool::scaleLevels);
    else if (is ("pianoRoll.limit")) openNoteTool (NoteTool::limit);
    else if (is ("pianoRoll.quantize")) openNoteTool (NoteTool::quantize);
    else if (is ("pianoRoll.chop")) openNoteTool (NoteTool::chop);
    else if (is ("pianoRoll.flam")) openNoteTool (NoteTool::flam);
    else if (is ("pianoRoll.claw")) openNoteTool (NoteTool::claw);
    else if (is ("pianoRoll.lfo")) openNoteTool (NoteTool::lfo);
    else if (is ("pianoRoll.articulate")) openNoteTool (NoteTool::articulate);
    else if (is ("pianoRoll.riffMachine")) openNoteTool (NoteTool::riffMachine);
    else if (is ("pianoRoll.chordProgression")) openNoteTool (NoteTool::chordProgression);
    else if (is ("pianoRoll.zoomIn")) zoomAroundCentre (1.25);
    else if (is ("pianoRoll.zoomOut")) zoomAroundCentre (1.0 / 1.25);
    else if (is ("pianoRoll.zoom1")) zoomToBars (1);
    else if (is ("pianoRoll.zoom2")) zoomToBars (4);
    else if (is ("pianoRoll.zoom3")) zoomToBars (16);
    else if (is ("pianoRoll.zoomAll")) zoomAll();
    else if (is ("pianoRoll.zoomSelection")) zoomToSelection();
    else if (is ("pianoRoll.centrePlayhead")) centreOnPlayhead();
    else if (is ("pianoRoll.follow")) EditorGlobals::get().setFollowing (true, ! EditorGlobals::get().isFollowing (true));
    else if (is ("pianoRoll.quantizeStarts")) quantizeOperation (0);
    else if (is ("pianoRoll.quickQuantize")) quantizeOperation (1);
    else if (is ("pianoRoll.quantizeHalf")) quantizeOperation (2);
    else if (is ("pianoRoll.colourSelected")) colourSelection (noteColour);
    else if (is ("pianoRoll.selectByColour"))
    {
        // the colour of the selected note (FL: the selection's colour), else the current note colour
        const auto& notes = currentPattern (engine.getSong()).notes;
        selectByColour (! selection.empty() && selection.front() < (int) notes.size() ? notes[(std::size_t) selection.front()].colour : noteColour);
    }
    else if (is ("pianoRoll.invertSelection")) setSelection (notetools::invertSelection (channelNotes(), selection));
    else if (is ("pianoRoll.groupNotes") || is ("pianoRoll.ungroupNotes"))
    {
        const auto target = selection;
        if (target.empty())
            return true;
        const auto group = is ("pianoRoll.groupNotes");
        editNotes ([&] (Pattern& p) {
            if (group)
                notetools::groupNotes (p, target);
            else
                notetools::ungroupNotes (p, target);
        });
        repaint();
    }
    else
        return false;

    return true;
}

void PianoRollComponent::wheelNoteProperty (int note, float deltaY, bool fine)
{
    // the hovered note, or the whole selection when the hovered note is part of it
    const auto targets = isSelected (note) ? selection : std::vector<int> { note };
    const auto now = juce::Time::getMillisecondCounter();
    const auto newBurst = now - lastWheelEdit > 700; // one undo step per burst of wheel moves
    lastWheelEdit = now;
    editNotes ([&] (Pattern& p) {
        for (auto i : targets)
        {
            auto& n = p.notes[(std::size_t) i];
            setLaneValue (n, (float) gesture::wheelStepValue (laneValue (n), deltaY, fine));
        }
    }, newBurst);
}

void PianoRollComponent::glideBy (double ticks, double pixels)
{
    if (! gliding)
    {
        glideX.jumpTo (scrollTicks);
        glideY.jumpTo (scrollY);
        gliding = true;
    }

    const auto grid = gridArea();
    glideX.target = juce::jlimit (0.0, juce::jmax (0.0, (double) viewLengthTicks() - grid.getWidth() / pixelsPerTick), glideX.target + ticks);
    glideY.target = juce::jlimit (0.0, juce::jmax (0.0, 128.0 * rowHeight - grid.getHeight()), glideY.target + pixels);
    glideFrame();
}

void PianoRollComponent::glideFrame()
{
    if (! gliding)
        return;

    // someone else moved the view (scroll bar, zoom, pan): the glide gives way
    if (std::abs (scrollTicks - glideX.current) > 1e-6 || std::abs ((double) scrollY - std::round (glideY.current)) > 0.5)
    {
        gliding = false;
        return;
    }

    scrollTicks = glideX.step (0.45, 0.5 / pixelsPerTick);
    scrollY = (int) std::round (glideY.step (0.45, 1.0));
    glideY.current = scrollY;
    updateScrollBars();
    glideX.current = scrollTicks; // the scroll bar may have clamped it
    gliding = glideX.isMoving() || glideY.isMoving();
    repaint();
}

void PianoRollComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto ctrl = e.mods.isCtrlDown(), alt = e.mods.isAltDown(), shift = e.mods.isShiftDown();
    const auto overGrid = gridArea().contains (e.getPosition()) || velocityArea().contains (e.getPosition());

    // Alt+wheel over a note: its velocity (or the lane's property); Ctrl+Alt: fine steps
    if (alt && ! shift && overGrid && wheel.deltaY != 0.0f)
        if (const auto note = hoverAt (e.getPosition()).note; note >= 0)
        {
            wheelNoteProperty (note, wheel.deltaY, ctrl);
            return;
        }

    if (shift && alt) // nudge the selection by the grid
    {
        if (wheel.deltaY != 0.0f && ! selection.empty())
        {
            const auto now = juce::Time::getMillisecondCounter();
            const auto newBurst = now - lastWheelEdit > 700;
            lastWheelEdit = now;
            const auto ticks = (wheel.deltaY > 0 ? 1 : -1) * gridTicks();
            editNotes ([this, ticks] (Pattern& p) {
                auto deltaTicks = ticks;
                auto deltaKeys = 0;
                noteedit::move (p, selection, deltaTicks, deltaKeys);
            }, newBurst);
            sortNotes();
        }
        return;
    }

    gliding = false;

    if (ctrl && alt)
    {
        // vertical zoom keeping the key under the mouse in place
        const auto key = yToKey (e.y);
        const auto offset = e.y - keyToY (key);
        rowHeight = juce::jlimit (6, 32, rowHeight + (wheel.deltaY > 0 ? 2 : -2));
        scrollY = juce::jmax (0, (127 - key) * rowHeight + offset - (e.y - headerHeight));
    }
    else if (ctrl)
    {
        // zoom keeping the tick under the mouse in place
        const auto anchorPixels = (double) juce::jmax (0, e.x - keyboardWidth);
        const auto zoomed = juce::jlimit (2.0 / ticksPerStep, 120.0 / ticksPerStep, pixelsPerTick * gesture::wheelZoomFactor (wheel.deltaY, wheel.isSmooth));
        scrollTicks = gesture::zoomAnchored (scrollTicks, pixelsPerTick, zoomed, anchorPixels);
        pixelsPerTick = zoomed;
    }
    else
    {
        // plain wheel: up/down (Shift: left/right); a horizontal wheel or trackpad swipe scrolls sideways
        const auto vertical = gesture::wheelScrollPixels (wheel.deltaY, wheel.isSmooth, 3.0 * rowHeight);
        const auto sideways = gesture::wheelScrollPixels (wheel.deltaX, wheel.isSmooth, 60.0);
        if (shift)
            glideBy (gesture::wheelScrollPixels (wheel.deltaY, wheel.isSmooth, 80.0) / pixelsPerTick, 0.0);
        else
            glideBy (sideways / pixelsPerTick, vertical);
        return;
    }

    updateScrollBars();
    repaint();
}

// --- keyboard ----------------------------------------------------------------

void PianoRollComponent::deleteSelection()
{
    if (selection.empty())
        return;

    const auto doomed = selection;
    setSelection ({});
    editNotes ([&doomed] (Pattern& p) { noteedit::remove (p, doomed); });
}

void PianoRollComponent::copySelection()
{
    clipboard = noteedit::copy (currentPattern (engine.getSong()), selectionOrAll());
}

void PianoRollComponent::paste()
{
    if (clipboard.empty())
        return;

    // as in FL: pastes at the same position, already selected for dragging
    std::vector<int> added;
    const auto channel = engine.getSelectedChannel();
    editNotes ([&added, channel] (Pattern& p) {
        added = noteedit::paste (p, clipboard, 0, channel);
        notetools::regroup (p, added); // copies of grouped notes form groups of their own
    });
    setSelection (added);
    sortNotes();
}

void PianoRollComponent::duplicateSelection()
{
    const auto source = selectionOrAll();

    if (source.empty())
        return;

    std::vector<int> added;
    const auto grid = gridTicks();
    editNotes ([&added, &source, grid] (Pattern& p) {
        added = noteedit::duplicate (p, source, grid);
        notetools::regroup (p, added);
    });
    setSelection (added);
    sortNotes();
}

void PianoRollComponent::quantizeOperation (int kind)
{
    const auto grid = gridTicks();
    runOperation ([grid, kind] (Pattern& p, const std::vector<int>& t) {
        if (kind == 1)
            noteedit::quantizeFull (p, t, grid);
        else
            transportedit::quantizeNoteStarts (p, t, grid, kind == 2 ? 0.5 : 1.0);
        return t;
    });
}

void PianoRollComponent::nudge (std::int64_t ticks, int keys)
{
    if (selection.empty())
        return;

    editNotes ([this, &ticks, &keys] (Pattern& p) { noteedit::move (p, selection, ticks, keys); });

    if (keys != 0 && selection.size() == 1)
    {
        const auto& n = currentPattern (engine.getSong()).notes[(std::size_t) selection[0]];
        previewOn (n.key, n.velocity);
        juce::Timer::callAfterDelay (150, [safe = juce::Component::SafePointer<PianoRollComponent> (this)] {
            if (safe != nullptr)
                safe->previewOff();
        });
    }

    sortNotes();
}

bool PianoRollComponent::handleKey (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const auto cmd = mods.isCommandDown();
    const auto alt = mods.isAltDown();
    const auto shift = mods.isShiftDown();
    const auto code = key.getKeyCode();
    const auto letter = keys::letterOfKeyCode (code); // (0 for F-keys, numpad, arrows: issue #12)
    const auto grid = gridTicks();
    const auto bar = (std::int64_t) ppq * juce::jmax (1, engine.getSong().beatsPerBar);

    if (code == juce::KeyPress::backspaceKey)
    {
        EditorGlobals::get().toggleGrid();
        return true;
    }

    if (code == juce::KeyPress::deleteKey)
    {
        deleteSelection();
        return true;
    }

    if (code == juce::KeyPress::escapeKey)
    {
        setSelection ({});
        return true;
    }

    if (alt && cmd && letter == 'V') // editable ghost notes
    {
        editGhosts = ! editGhosts;
        if (editGhosts && ! showGhosts)
            ghostButton.setToggleState (true, juce::sendNotificationSync);
        repaint();
        return true;
    }

    if (alt && ! cmd && letter == 'V')
    {
        ghostButton.setToggleState (! ghostButton.getToggleState(), juce::sendNotificationSync);
        return true;
    }

    if (cmd && alt && code == juce::KeyPress::homeKey)
    {
        resizeFromLeft = ! resizeFromLeft;
        return true;
    }

    if (cmd && letter == 'A')
    {
        selectAll();
        return true;
    }

    if (cmd && letter == 'C')
    {
        copySelection();
        return true;
    }

    if (cmd && letter == 'X')
    {
        copySelection();
        deleteSelection();
        return true;
    }

    if (cmd && letter == 'V')
    {
        paste();
        return true;
    }

    if (cmd && letter == 'B')
    {
        duplicateSelection();
        return true;
    }

    // quick note operations: one undo step each, on the selection (or all notes). The quantize ones are key map
    // actions (pianoRoll.quickQuantize / quantizeStarts / quantizeHalf) so they can be reassigned.
    if (cmd && letter == 'U')
    {
        runOperation ([grid] (Pattern& p, const std::vector<int>& t) { return noteedit::chop (p, t, grid); });
        return true;
    }

    if (cmd && letter == 'G')
    {
        runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::join (p, t); return std::vector<int> {}; });
        return true;
    }

    if (cmd && letter == 'L')
    {
        runOperation ([] (Pattern& p, const std::vector<int>& t) { noteedit::legato (p, t); return t; });
        return true;
    }

    if (shift && ! cmd && ! alt && letter == 'D')
    {
        runOperation ([grid] (Pattern& p, const std::vector<int>& t) { noteedit::shrinkToGrid (p, t, grid); return t; });
        return true;
    }

    if (cmd && shift && (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey))
    {
        const auto steps = code == juce::KeyPress::rightKey ? 1 : -1;
        runOperation ([grid, bar, steps] (Pattern& p, const std::vector<int>& t) { noteedit::rotate (p, t, grid, bar, steps); return t; });
        return true;
    }

    if (cmd && letter == 'M')
    {
        toggleMute();
        return true;
    }

    if (cmd && letter == 'P')
    {
        togglePorta();
        return true;
    }

    if (! cmd && ! alt && ! selection.empty())
    {
        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
        {
            nudge (0, (code == juce::KeyPress::upKey ? 1 : -1) * (shift ? 12 : 1));
            return true;
        }

        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            const auto amount = shift ? bar : grid;
            nudge ((code == juce::KeyPress::rightKey ? 1 : -1) * amount, 0);
            return true;
        }
    }

    // (the tools and the other single letters are key map actions now: runKeyAction, issue #12)
    return false;
}

bool PianoRollComponent::runKeyAction (const std::string& id)
{
    for (const auto& t : editTools())
        if (t.tool != EditTool::slip && id == "pianoRoll.tool." + std::string (editToolId (t.tool)))
        {
            tools.setTool (t.tool);
            return true;
        }

    if (id == "pianoRoll.slide") slideButton.setToggleState (! slideButton.getToggleState(), juce::dontSendNotification);
    else if (id == "pianoRoll.previousChannel") stepChannel (-1, false);
    else if (id == "pianoRoll.nextChannel") stepChannel (1, false);
    else if (id == "pianoRoll.previousChannelWithNotes") stepChannel (-1, true);
    else if (id == "pianoRoll.nextChannelWithNotes") stepChannel (1, true);
    else return false;
    return true;
}

void PianoRollComponent::refreshToolKeys (const std::function<juce::String (const std::string&)>& keyOf)
{
    tools.refreshKeys ([&keyOf] (EditTool t) { return keyOf ("pianoRoll.tool." + std::string (editToolId (t))); });
}

juce::PopupMenu PianoRollComponent::midiMenu()
{
    juce::PopupMenu menu;
    menu.addItem (tr ("Import .mid into this channel..."), [this] {
        chooser = std::make_unique<juce::FileChooser> (tr ("Import MIDI"), juce::File(), "*.mid;*.midi");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc) {
            if (! fc.getResult().existsAsFile())
                return;

            juce::String error;
            const auto file = fc.getResult();
            const auto midi = MidiFileIo::parseFile (file, error);

            if (! midi)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "MIDI", error);
                return;
            }

            // FL's "Import MIDI data" window with Blend (REA round 146b): which tracks / channels / controller
            static midiimport::Options remembered;
            juce::Component::SafePointer<PianoRollComponent> safe (this);
            showMidiImportDialog (file.getFileName(), *midi, remembered, true, [safe, midi = *midi] (const midiimport::Options& o) {
                remembered = o;
                if (safe == nullptr)
                    return;
                std::vector<int> added;
                std::string err;
                const auto channel = safe->engine.getSelectedChannel();
                safe->engine.editSong ([&] (Song& s) { added = midiimport::importIntoChannel (s, channel, midi, o, err); });
                if (added.empty())
                {
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "MIDI", juce::String::fromUTF8 (err.c_str()));
                    return;
                }
                safe->knownNoteCount = currentPattern (safe->engine.getSong()).notes.size();
                safe->setSelection (added);
                safe->scrollToNotes();
                safe->repaint();
            });
        });
    });
    menu.addItem (tr ("Export pattern as .mid..."), [this] {
        const auto& song = engine.getSong();
        const auto name = juce::String::fromUTF8 (song.patterns[(std::size_t) song.currentPattern].name.c_str());
        chooser = std::make_unique<juce::FileChooser> (tr ("Export MIDI"), juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (name + ".mid"),
                                                       "*.mid");
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [this] (const juce::FileChooser& fc) {
            auto file = fc.getResult();

            if (file == juce::File())
                return;

            if (! file.hasFileExtension ("mid;midi"))
                file = file.withFileExtension (".mid");

            juce::String error;

            if (! MidiFileIo::savePattern (engine.getSong(), engine.getSong().currentPattern, file, error))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "MIDI", error);
        });
    });
    return menu;
}

// --- other -------------------------------------------------------------------

void PianoRollComponent::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hBar)
        scrollTicks = start;
    else
        scrollY = (int) start;

    repaint();
}

void PianoRollComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // pattern/channel switched or the pattern changed externally (undo, another window): the selection no longer applies
    const auto& song = engine.getSong();
    const auto count = currentPattern (song).notes.size();

    if (song.currentPattern != knownPattern || engine.getSelectedChannel() != knownChannel || count != knownNoteCount)
    {
        if (engine.getSelectedChannel() != knownChannel)
        {
            stopListening();
            if (knownChannel >= 0)
                allChannels = false; // another channel picked (Channel Rack, keys): back to it alone
        }

        knownPattern = song.currentPattern;
        knownChannel = engine.getSelectedChannel();
        knownNoteCount = count;

        if (drag == Drag::none)
            selection.clear();
    }

    followButton.setToggleState (EditorGlobals::get().isFollowing (true), juce::dontSendNotification);
    refreshChannelBox();
    rebuildLaneBox();
    updateScrollBars();
    repaint();
}

std::optional<AutomationTarget> PianoRollComponent::eventLaneTarget() const
{
    const auto k = laneBox.getSelectedId() - eventLaneIdBase;
    if (k >= 0 && k < (int) eventTargets.size())
        return eventTargets[(std::size_t) k];
    return std::nullopt;
}

void PianoRollComponent::rebuildLaneBox (std::optional<AutomationTarget> shown, bool force)
{
    // the pattern's event lanes, plus the one shown (it may have no events yet)
    if (! shown)
        shown = eventLaneTarget();
    std::vector<AutomationTarget> targets;
    for (const auto& lane : currentPattern (engine.getSong()).events)
        if (lane.target.kind != AutomationTargetKind::none && ! lane.events.empty())
            targets.push_back (lane.target);
    if (shown && std::find (targets.begin(), targets.end(), *shown) == targets.end())
        targets.push_back (*shown);
    if (targets == eventTargets && ! force)
        return;

    eventTargets = targets;
    const auto previous = laneBox.getSelectedId();
    laneBox.clear (juce::dontSendNotification);
    laneBox.addItem (tr ("Velocity"), 1);
    laneBox.addItem (tr ("Pan"), 2);
    laneBox.addItem (tr ("Fine pitch"), 3);
    if (! eventTargets.empty())
        laneBox.addSectionHeading (tr ("Pattern events"));
    for (int k = 0; k < (int) eventTargets.size(); ++k)
        laneBox.addItem (describeAutomationTarget (engine, eventTargets[(std::size_t) k]), eventLaneIdBase + k);
    auto id = previous >= 1 && previous <= 3 ? previous : 1;
    if (shown)
        id = eventLaneIdBase + (int) (std::find (eventTargets.begin(), eventTargets.end(), *shown) - eventTargets.begin());
    laneBox.setSelectedId (id, juce::dontSendNotification);
}

void PianoRollComponent::showEventLane (const AutomationTarget& target)
{
    rebuildLaneBox (target, true);
    repaint();
}

void PianoRollComponent::drawEvents (int fromX, int toX, int y, bool erase)
{
    const auto target = eventLaneTarget();
    if (! target)
        return;
    const auto lane = velocityArea();
    const auto value = juce::jlimit (0.0f, 1.0f, (float) (lane.getBottom() - 2 - y) / (float) (lane.getHeight() - 8));
    const auto step = std::max<std::int64_t> (1, gridTicks());
    const auto t0 = std::max<std::int64_t> (0, (std::int64_t) std::floor (xToTick (juce::jmin (fromX, toX)) / (double) step) * step);
    const auto t1 = std::max (t0 + step, (std::int64_t) std::floor (xToTick (juce::jmax (fromX, toX)) / (double) step) * step + step);
    const auto pattern = engine.getSong().currentPattern;
    engine.editSong ([&] (Song& s) {
        if (pattern < 0 || pattern >= (int) s.patterns.size())
            return;
        auto& p = s.patterns[(std::size_t) pattern];
        if (erase)
        {
            if (auto* l = eventedit::findLane (p, *target))
                eventedit::erase (*l, t0, t1);
            eventedit::removeEmptyLanes (p);
        }
        else
            eventedit::fill (eventedit::laneFor (p, *target), t0, t1, value, step);
    });
    repaint (velocityArea());
}

void PianoRollComponent::timerCallback()
{
    if (channelBox.getSelectedId() != (allChannels ? allChannelsId : engine.getSelectedChannel() + 1))
        refreshChannelBox();
    updateChordReadout();

    auto& transport = engine.getTransport();
    auto tick = -1.0;

    // the playhead shows while the edited pattern plays: Pattern mode, or a clip of it in Song mode (round 162)
    if (transport.playing.load() && isShowing())
    {
        const auto& song = engine.getSong();
        tick = transportedit::patternPlayhead (song, song.currentPattern, transport.positionTicks.load());
    }

    if (tick != playheadTick)
    {
        auto dirty = [this] (double t) {
            if (t >= 0.0)
                repaint ((int) tickToX (t) - 7, 0, 14, getHeight());
        };

        dirty (playheadTick);
        playheadTick = tick;

        // Follow: a page further when the playhead leaves the view (off: the view stays where it is)
        const auto grid = gridArea();
        const auto newStart = transportedit::followViewStart (scrollTicks, grid.getWidth() / pixelsPerTick, tick, 20.0 / pixelsPerTick,
                                                              20.0 / pixelsPerTick);

        if (EditorGlobals::get().isFollowing (true) && tick >= 0.0 && newStart != scrollTicks && drag == Drag::none)
        {
            scrollTicks = newStart;
            updateScrollBars();
            repaint();
        }
        else
            dirty (tick);
    }
}
} // namespace lynx
