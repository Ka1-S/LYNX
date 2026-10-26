#include "ui/ColourPicker.h"
#include "ui/ClipProperties.h"
#include "ui/UiTrace.h"
#include "ui/TextCache.h"
#include "ui/PlaylistComponent.h"

#include "core/TransportEdit.h"

#include "core/ClipOps.h"

#include "core/EditorPolish.h"

#include "core/RackOps.h"

#include "core/NoteEdit.h"
#include "core/PlaylistOps.h"
#include "core/TempoDetect.h"
#include "ui/ExportDialog.h"
#include "engine/Text.h"
#include "ui/BrowserComponent.h"
#include "ui/AutomationUi.h"
#include "ui/ChannelSettings.h"
#include "ui/Dialogs.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/WaveformView.h"
#include "ui/AudioEditor.h"

#include <map>
#include <unordered_map>

namespace lynx
{
namespace
{
constexpr float clipTitleHeight = 13.0f;

// --- clip look -------------------------------------------------------------------------------------
// One recipe for every clip kind (studied from FL's Playlist, drawn with LYNX's own code and palette): a muted
// header bar holding a small kind glyph and the name, a body in a deep tint of the clip colour, and the
// content (waveform, notes, curve) in a light tint of it; a thin dark edge separates neighbours. Few elements,
// one hue per clip, so a busy Playlist stays calm and every clip reads at a glance.

enum class ClipGlyph { pattern, audio, automation };

// Text colour readable on a clip's header bar.
juce::Colour clipTitleText (juce::Colour band)
{
    return band.getPerceivedBrightness() > 0.62f ? theme::colour::bg0 : juce::Colours::white.withAlpha (0.92f);
}

// the header keeps a hint of the clip colour, greyed so the names read and the bodies carry the colour
juce::Colour clipHeaderColour (juce::Colour base) { return base.interpolatedWith (juce::Colour (0xff4b525a), 0.5f); }
juce::Colour clipBodyColour (juce::Colour base) { return base.interpolatedWith (theme::colour::bg1, 0.72f); }
juce::Colour clipContentColour (juce::Colour base) { return base.interpolatedWith (juce::Colours::white, 0.7f); }

float clipHeaderHeight (juce::Rectangle<float> r) { return juce::jmin (clipTitleHeight, juce::jmax (0.0f, r.getHeight() - 8.0f)); }

// The kind glyph at the header's left (the clip menu opens from it): LYNX's own small shapes.
void paintClipGlyph (juce::Graphics& g, juce::Rectangle<float> box, ClipGlyph glyph, juce::Colour colour)
{
    g.setColour (colour);
    const auto c = box.getCentre();
    switch (glyph)
    {
        case ClipGlyph::pattern: // three note bars, stepped
            for (int i = 0; i < 3; ++i)
                g.fillRect (juce::Rectangle<float> (c.x - 4.0f + (float) i * 2.0f, c.y - 3.5f + (float) i * 3.0f, 5.0f, 1.5f));
            break;
        case ClipGlyph::audio: // a small mirrored waveform
        {
            const float heights[] = { 2.0f, 6.0f, 9.0f, 5.0f, 3.0f };
            for (int i = 0; i < 5; ++i)
                g.fillRect (juce::Rectangle<float> (c.x - 4.5f + (float) i * 2.0f, c.y - heights[i] * 0.5f, 1.2f, heights[i]));
            break;
        }
        case ClipGlyph::automation: // a rising curve
        {
            juce::Path p;
            p.startNewSubPath (c.x - 4.5f, c.y + 3.5f);
            p.quadraticTo (c.x + 1.0f, c.y + 3.5f, c.x + 4.5f, c.y - 3.5f);
            g.strokePath (p, juce::PathStrokeType (1.3f));
            break;
        }
    }
}

// Clip body with the header bar, glyph and name; returns the body below the header.
juce::Rectangle<float> paintClipFrame (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour base, const juce::String& name,
                                       ClipGlyph glyph = ClipGlyph::pattern)
{
    using namespace theme;
    const uitrace::Part tally (uitrace::part::frame);
    const auto title = r.withHeight (clipHeaderHeight (r));

    // zoomed far out (a clip a few pixels wide, often a thousand of them on screen): plain rectangles, the same
    // colours. Rounded shapes and gradients cost a path each on the GPU renderer (round 151: 1 350 clips of
    // Thief's chopped vocals took 20-40 ms a frame), and at this size they look the same.
    if (r.getWidth() < 8.0f)
    {
        g.setColour (clipBodyColour (base));
        g.fillRect (r);
        if (title.getHeight() >= 4.0f)
        {
            g.setColour (clipHeaderColour (base));
            g.fillRect (title);
        }
        return r.withTrimmedTop (title.getHeight());
    }

    g.setColour (clipBodyColour (base));
    g.fillRoundedRectangle (r, radius::sm);

    if (title.getHeight() >= 4.0f)
    {
        // the header band: rounded top corners, square bottom; a lighter top half gives it the slight sheen of a
        // gradient (round 151: drawn with the renderer's rounded-rectangle and rectangle primitives, not a path and
        // a gradient brush, which cost a GPU geometry each for every clip)
        const auto header = clipHeaderColour (base);
        g.setColour (header);
        g.fillRoundedRectangle (title, radius::sm);
        g.fillRect (title.withTrimmedTop (juce::jmin (radius::sm, title.getHeight() * 0.5f)));
        g.setColour (header.brighter (0.06f));
        g.fillRect (title.reduced (juce::jmin (radius::sm, title.getWidth() * 0.5f), 0.0f).withHeight (title.getHeight() * 0.45f).withTrimmedTop (1.0f));
        g.setColour (theme::colour::bg0.withAlpha (0.35f)); // the header's lower edge
        g.fillRect (title.getX(), title.getBottom() - 1.0f, title.getWidth(), 1.0f);

        if (r.getWidth() > 18.0f && title.getHeight() >= 9.0f)
        {
            auto text = title.withTrimmedLeft (3.0f).withTrimmedRight (2.0f);
            if (r.getWidth() >= 40.0f) // room for the glyph (where the clip menu opens)
                paintClipGlyph (g, text.removeFromLeft (13.0f), glyph, clipTitleText (header).withMultipliedAlpha (0.8f));
            const uitrace::Part tallyTitle (uitrace::part::title);
            g.setColour (clipTitleText (header));
            g.setFont (font (type::caption, true));
            textcache::drawText (g, name, text.withTrimmedLeft (2.0f).toNearestInt(), juce::Justification::centredLeft, true);
        }
    }

    return r.withTrimmedTop (title.getHeight());
}

void paintClipOutline (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour base, bool selected, bool current)
{
    using namespace theme;
    const uitrace::Part tally (uitrace::part::outline);
    if (r.getWidth() < 8.0f && ! selected) // tiny clip (see paintClipFrame): a dark right edge keeps neighbours apart
    {
        g.setColour (current ? colour::textPrimary.withAlpha (0.55f) : colour::bg0.withAlpha (0.85f));
        g.fillRect (r.getRight() - 1.0f, r.getY(), 1.0f, r.getHeight());
        return;
    }
    if (selected)
    {
        g.setColour (colour::selection);
        g.drawRoundedRectangle (r.reduced (0.5f), radius::sm, 2.0f);
    }
    else
    {
        g.setColour (current ? colour::textPrimary.withAlpha (0.55f) : colour::bg0.withAlpha (0.85f));
        g.drawRoundedRectangle (r.reduced (0.5f), radius::sm, current ? 1.2f : 1.0f);
    }
}

} // namespace

// Zoomed far out, a song of chopped audio shows a thousand clips a few pixels wide. Painted one by one, each is a
// handful of rounded paths (frame, header, waveform, outline), and the GPU renderer pays per path: Thief's Playlist
// took 33-47 ms a frame to scroll (round 151). These clips are gathered here, rectangles per colour and waveforms per
// colour as one path, and filled together after the other clips: a few calls a frame. At this size they look the
// same as the full recipe.
struct PlaylistComponent::SmallClips
{
    static constexpr float maxWidth = 12.0f;
    std::map<juce::uint32, juce::RectangleList<float>> bodies, contents, edges;
    std::map<juce::uint32, juce::Path> waves;

    void fill (juce::Graphics& g)
    {
        // (rectangle by rectangle: the GPU renderer turns a rectangle list into a geometry, which costs more)
        for (auto* layer : { &bodies, &contents })
            for (auto& [argb, list] : *layer)
            {
                g.setColour (juce::Colour (argb));
                for (const auto& r : list)
                    g.fillRect (r);
            }
        const uitrace::Part tally (uitrace::part::batchWaves);
        for (auto& [argb, path] : waves)
        {
            g.setColour (juce::Colour (argb));
            g.fillPath (path);
        }
        for (auto& [argb, list] : edges)
        {
            g.setColour (juce::Colour (argb));
            for (const auto& r : list)
                g.fillRect (r);
        }
    }
};

PlaylistComponent::PlaylistComponent (AudioEngine& e)
    : engine (e)
{
    setWantsKeyboardFocus (false);
    setMouseClickGrabsKeyboardFocus (false);

    for (auto* bar : { &hBar, &vBar })
    {
        bar->addListener (this);
        bar->setAutoHide (false);
        addAndMakeVisible (bar);
    }

    // the overview strip is the horizontal scroller (FL's Playlist has its preview bar on top)
    hBar.setVisible (false);
    overview.paintContent = [this] (juce::Graphics& g, juce::Rectangle<float> area, double total) { paintOverview (g, area, total); };
    overview.onChange = [this] (double start, double length) {
        const auto width = (double) gridArea().getWidth();
        pixelsPerTick = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), width / juce::jmax (1.0, length));
        scrollTicks = juce::jmax (0.0, start);
        updateScrollBars();
        repaint();
    };
    overview.onShowAll = [this] { zoomAll(); };
    addAndMakeVisible (overview);

    grid::fillEditorBox (snapBox);
    snapBox.setTooltip (tr ("Playlist grid: follows the global grid in the toolbar, or its own; hold Alt while dragging to ignore it"));
    snapBox.setWantsKeyboardFocus (false);
    snapBox.setMouseClickGrabsKeyboardFocus (false);
    snapBox.onChange = [this] { repaint(); };
    addAndMakeVisible (snapBox);
    tools.onChange = [this] (EditTool) { updateCursor (juce::ModifierKeys::currentModifiers); };
    setOpaque (true); // paints every pixel: nothing behind it is redrawn while scrolling
    addAndMakeVisible (tools);

    // Follow (FL: "Auto scrolling"): the view pages along with the playhead in Song mode; off, it stays put
    followButton.setOnColour (theme::colour::bg6);
    followButton.setIconColourWhenOn (theme::colour::accent);
    followButton.setFlat (true);
    followButton.setClickingTogglesState (false);
    followButton.setToggleState (EditorGlobals::get().isFollowing (false), juce::dontSendNotification);
    followButton.setTooltip (tr ("Follow: the Playlist scrolls along with the playhead while the song plays; off, you can look around freely"));
    ui::setQuickTip (followButton, tr ("Follow the playhead"));
    followButton.onClick = [this] { runAction ("playlist.follow"); };
    followButton.setWantsKeyboardFocus (false);
    followButton.setMouseClickGrabsKeyboardFocus (false);
    addAndMakeVisible (followButton);

    arrangementBox.setTooltip (tr ("Arrangement (as in FL, a project can have several)"));
    arrangementBox.setWantsKeyboardFocus (false);
    arrangementBox.setMouseClickGrabsKeyboardFocus (false);
    arrangementBox.onChange = [this] { arrangementChosen(); };
    addAndMakeVisible (arrangementBox);
    refreshArrangementBox();
    refreshTrackLayout();

    engine.addChangeListener (this);
    EditorGlobals::get().addChangeListener (this);
    ui::WaveformCache::getInstance()->addChangeListener (this); // a waveform became ready, or the settings changed
    startTimerHz (30);
}

namespace
{
enum ArrangementAction
{
    newArrangement = 1001,
    cloneArrangement,
    renameArrangementAction,
    removeArrangementAction,
    manageArrangements
};
} // namespace

void PlaylistComponent::refreshArrangementBox()
{
    const auto& song = engine.getSong();
    juce::StringArray names;

    for (const auto& a : song.arrangements)
        names.add (juce::String::fromUTF8 (a.name.c_str()));

    // only rebuilds when something changed (rebuilding closes the open menu)
    bool same = arrangementBox.getNumItems() == names.size() + 4;
    for (int i = 0; same && i < names.size(); ++i)
        same = arrangementBox.getItemText (i) == names[i];

    if (! same)
    {
        arrangementBox.clear (juce::dontSendNotification);

        for (int i = 0; i < names.size(); ++i)
            arrangementBox.addItem (names[i], i + 1);

        arrangementBox.addSeparator();
        arrangementBox.addItem (tr ("New arrangement"), newArrangement);
        arrangementBox.addItem (tr ("Clone this one"), cloneArrangement);
        arrangementBox.addItem (tr ("Rename..."), renameArrangementAction);
        arrangementBox.addItem (tr ("Remove this one"), removeArrangementAction);
        arrangementBox.addSeparator();
        arrangementBox.addItem (tr ("Arrangements and merging..."), manageArrangements);
        arrangementBox.setItemEnabled (removeArrangementAction, names.size() > 1);
    }

    arrangementBox.setSelectedId (song.currentArrangement + 1, juce::dontSendNotification);
}

void PlaylistComponent::arrangementChosen()
{
    const auto id = arrangementBox.getSelectedId();
    const auto& song = engine.getSong();
    setSelection ({});

    if (id >= 1 && id <= (int) song.arrangements.size())
    {
        if (id - 1 != song.currentArrangement)
            engine.editSong ([index = id - 1] (Song& s) { s.selectArrangement (index); }, false);
        return;
    }

    switch (id)
    {
        case newArrangement:
            engine.editSong ([] (Song& s) { s.addArrangement ("Arrangement " + std::to_string (s.arrangements.size() + 1)); });
            break;
        case cloneArrangement:
            engine.editSong ([] (Song& s) {
                s.addArrangement (s.arrangements[(std::size_t) s.currentArrangement].name + " (copy)", true);
            });
            break;
        case renameArrangementAction:
            renameArrangement();
            break;
        case manageArrangements:
            showArrangements (engine);
            break;
        case removeArrangementAction:
            engine.editSong ([] (Song& s) { s.removeArrangement (s.currentArrangement); });
            break;
        default:
            break;
    }

    refreshArrangementBox();
}

void PlaylistComponent::renameArrangement()
{
    const auto index = engine.getSong().currentArrangement;
    auto* window = new juce::AlertWindow (tr ("Rename arrangement"), {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", juce::String::fromUTF8 (engine.getSong().arrangements[(std::size_t) index].name.c_str()));
    window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, index, window] (int result) {
                                 const auto name = window->getTextEditorContents ("name").trim().toStdString();

                                 if (result == 1 && ! name.empty() && index < (int) engine.getSong().arrangements.size())
                                     engine.editSong ([index, name] (Song& s) { s.arrangements[(std::size_t) index].name = name; });
                             }),
                             true);
}

PlaylistComponent::~PlaylistComponent()
{
    EditorGlobals::get().removeChangeListener (this);
    engine.removeChangeListener (this);
    if (auto* cache = ui::WaveformCache::getInstanceWithoutCreating())
        cache->removeChangeListener (this);
}

juce::Colour PlaylistComponent::patternColour (const Song& song, int pattern)
{
    if (pattern >= 0 && pattern < (int) song.patterns.size() && song.patterns[(std::size_t) pattern].colour != 0)
        return juce::Colour (song.patterns[(std::size_t) pattern].colour);

    // hues spread by the golden ratio: neighboring patterns get clearly different colors
    const auto hue = std::fmod (0.08f + (float) pattern * 0.618034f, 1.0f);
    return juce::Colour::fromHSV (hue, 0.45f, 0.75f, 1.0f);
}

juce::Rectangle<int> PlaylistComponent::gridArea() const
{
    return getLocalBounds().withTrimmedLeft (headerWidth).withTrimmedTop (headerHeight)
        .withTrimmedRight (scrollBarSize);
}

double PlaylistComponent::tickToX (double tick) const { return headerWidth + (tick - scrollTicks) * pixelsPerTick; }
double PlaylistComponent::xToTick (int x) const { return (x - headerWidth) / pixelsPerTick + scrollTicks; }
int PlaylistComponent::heightOf (int track) const
{
    const auto& tracks = engine.getSong().playlistTracks;
    if (noteedit::isFoldedAway (engine.getSong(), track))
        return foldedHeight;
    const auto h = track >= 0 && track < (int) tracks.size() ? tracks[(std::size_t) track].height : 0;
    return h > 0 ? polish::clampTrackHeight (h) : trackHeight;
}

void PlaylistComponent::refreshTrackLayout()
{
    const auto count = numTracks();
    trackTops.assign ((std::size_t) count + 1, 0);

    for (int t = 0; t < count; ++t)
        trackTops[(std::size_t) t + 1] = trackTops[(std::size_t) t] + heightOf (t);
}

int PlaylistComponent::tracksHeight() const
{
    return trackTops.empty() ? numTracks() * trackHeight : trackTops.back();
}

int PlaylistComponent::trackToY (int track) const
{
    // past the cached rows (the list grew during a drag): default heights
    const auto last = (int) trackTops.size() - 1;
    const auto top = trackTops.empty() ? track * trackHeight
                     : track <= last   ? trackTops[(std::size_t) juce::jmax (0, track)]
                                       : trackTops.back() + (track - last) * trackHeight;
    return headerHeight + top - scrollY;
}

int PlaylistComponent::yToTrack (int y) const
{
    const auto offset = y - headerHeight + scrollY;

    if (trackTops.size() < 2)
        return juce::jlimit (0, numTracks() - 1, offset / trackHeight);

    const auto it = std::upper_bound (trackTops.begin(), trackTops.end(), offset);
    const auto index = (int) (it - trackTops.begin()) - 1;
    return juce::jlimit (0, numTracks() - 1, index);
}

int PlaylistComponent::dividerAt (juce::Point<int> p) const
{
    // the bottom edge of a track header (4 px band) resizes that track
    if (p.x >= headerWidth || p.y < headerHeight || p.y >= gridArea().getBottom())
        return -1;

    const auto track = yToTrack (p.y);

    for (auto t : { track - 1, track })
        if (t >= 0 && ! noteedit::isFoldedAway (engine.getSong(), t) && std::abs (p.y - (trackToY (t) + heightOf (t))) <= 3)
            return t;

    return -1;
}

void PlaylistComponent::setTrackHeights (std::vector<int> tracks, int height)
{
    engine.editSong ([tracks, height] (Song& s) {
        for (auto t : tracks)
        {
            if (t < 0)
                continue;
            if ((int) s.playlistTracks.size() <= t)
                s.playlistTracks.resize ((std::size_t) t + 1);
            s.playlistTracks[(std::size_t) t].height = height == trackHeight ? 0 : polish::clampTrackHeight (height);
        }
    });
    refreshTrackLayout();
    updateScrollBars();
    repaint();
}

int PlaylistComponent::numTracks() const
{
    int highest = 0;

    for (const auto& c : engine.getSong().playlist)
        highest = juce::jmax (highest, c.track);

    return juce::jlimit (16, 500, highest + 8); // always a few free tracks at the bottom
}

std::int64_t PlaylistComponent::gridTicks() const
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

std::vector<std::int64_t> PlaylistComponent::snapCandidates() const
{
    // edges of the other clips ("Nearest note" in the Playlist snaps to clip edges)
    std::vector<std::int64_t> out;
    const auto& playlist = engine.getSong().playlist;

    for (int i = 0; i < (int) playlist.size(); ++i)
        if (std::find (dragIndices.begin(), dragIndices.end(), i) == dragIndices.end())
        {
            out.push_back (playlist[(std::size_t) i].start);
            out.push_back (playlist[(std::size_t) i].end());
        }

    return out;
}

std::int64_t PlaylistComponent::snap (double tick, bool enabled) const
{
    if (! enabled)
        return (std::int64_t) std::floor (tick);

    const auto& option = grid::resolve (snapBox);
    const auto step = gridTicks();

    if (option.kind == grid::Kind::nearest)
        return noteedit::snapNearest (tick, snapCandidates(), 10.0 / pixelsPerTick, step);

    // whole bars respect time signature changes
    if (step == (std::int64_t) ppq * juce::jmax (1, engine.getSong().beatsPerBar))
        return engine.getSong().barAt ((std::int64_t) std::max (0.0, std::floor (tick))).tick;

    return noteedit::snapDown (tick, step);
}

void PlaylistComponent::updateScrollBars()
{
    const auto& song = engine.getSong();
    const auto grid = gridArea();
    const auto bar = (double) ppq * song.beatsPerBar;
    const auto total = juce::jmax ((double) song.songLength() + 16 * bar, grid.getWidth() / pixelsPerTick);

    hBar.setRangeLimits (0.0, total, juce::dontSendNotification);
    hBar.setCurrentRange (scrollTicks, grid.getWidth() / pixelsPerTick, juce::dontSendNotification);
    vBar.setRangeLimits (0.0, (double) tracksHeight(), juce::dontSendNotification);
    vBar.setCurrentRange (scrollY, grid.getHeight(), juce::dontSendNotification);
    scrollTicks = hBar.getCurrentRangeStart();
    scrollY = (int) vBar.getCurrentRangeStart();
    overview.minLength = grid.getWidth() / (800.0 / (4 * ppq));
    overview.setRange (total, scrollTicks, grid.getWidth() / pixelsPerTick);
}

void PlaylistComponent::setTitleBarInsets (int left, int right)
{
    if (left != titleInsetLeft || right != titleInsetRight)
    {
        titleInsetLeft = left;
        titleInsetRight = right;
        resized();
    }
}

void PlaylistComponent::resized()
{
    const auto h = toolbarHeight - 6;
    arrangementBox.setBounds (4 + titleInsetLeft, 3, juce::jmin (150, getWidth() / 5), h);
    tools.setBounds (arrangementBox.getRight() + 8, 3, tools.getIdealWidth (h), h);
    snapBox.setBounds (tools.getRight() + 8, 3, 128, h);
    followButton.setBounds (snapBox.getRight() + 6, 3, h, h);
    overview.setBounds (headerWidth, toolbarHeight, getWidth() - headerWidth - scrollBarSize, overviewHeight);
    vBar.setBounds (getWidth() - scrollBarSize, headerHeight, scrollBarSize, getHeight() - headerHeight);
    updateScrollBars();

    if (fitSongPending)
        showSongAfterOpen();
}

void PlaylistComponent::showSongAfterOpen()
{
    const auto width = gridArea().getWidth();
    fitSongPending = width < 100;

    if (fitSongPending)
        return;

    scrollY = 0;
    scrollTicks = 0.0;
    const auto length = (double) engine.getSong().songLength();

    if (length > width / pixelsPerTick)
        zoomAll(); // (also updates the scroll bars)
    else
    {
        updateScrollBars();
        repaint();
    }
}

void PlaylistComponent::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("PlaylistComponent paint", true, g.getClipBounds());
    if (trackTops.size() != (std::size_t) numTracks() + 1) // the track list grew or shrank since the last change message
        refreshTrackLayout();

    const auto& song = engine.getSong();
    const auto grid = gridArea();
    const auto bar = (std::int64_t) ppq * song.beatsPerBar;
    const auto tracks = numTracks();

    // the overview follows the clips (edits, undo, arrangement switches) and the view
    {
        auto signature = song.playlist.size() * 1000003u + (std::size_t) song.currentArrangement;
        for (const auto& c : song.playlist)
            signature = signature * 31u + (std::size_t) (c.start * 131 + c.length * 7 + c.track * 257 + c.pattern + (c.muted ? 1 : 0));
        if (signature != overviewSignature)
        {
            overviewSignature = signature;
            overview.repaint();
        }
        updateScrollBars();
    }

    using namespace theme;
    g.fillAll (colour::bg1);

    // --- tracks and grid ---------------------------------------------------
    {
        const uitrace::Scope traceGrid ("Playlist lanes+clips", false);
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (grid);

        for (int t = 0; t < tracks; ++t)
        {
            const auto y = trackToY (t);
            const auto trackHeight = heightOf (t);

            if (y > grid.getBottom() || y + trackHeight < grid.getY())
                continue;
            const uitrace::Part tally (uitrace::part::lanes);

            auto lane = t % 2 == 0 ? colour::bg2 : colour::bg2.interpolatedWith (colour::bg1, 0.5f);
            if (t == hover.track && drag == Drag::none)
                lane = lane.interpolatedWith (colour::textPrimary, 0.025f); // the track under the mouse
            g.setColour (lane);
            g.fillRect (grid.getX(), y, grid.getWidth(), trackHeight);
            g.setColour (colour::bg0);
            g.drawHorizontalLine (y + trackHeight - 1, (float) grid.getX(), (float) grid.getRight());
        }

        const auto beatPixels = ppq * pixelsPerTick;

        for (const auto& b : song.barsBetween ((std::int64_t) std::max (0.0, scrollTicks), (std::int64_t) xToTick (grid.getRight()) + 1))
        {
            g.setColour (colour::gridBar);
            g.drawVerticalLine ((int) tickToX ((double) b.tick), (float) grid.getY(), (float) grid.getBottom());

            if (beatPixels < 8.0)
                continue; // zoomed far out: bar lines only

            g.setColour (colour::gridBeat);
            for (auto tick = b.tick + ppq; tick < b.tick + b.length; tick += ppq)
                g.drawVerticalLine ((int) tickToX ((double) tick), (float) grid.getY(), (float) grid.getBottom());
        }

        // loop region
        if (song.hasLoop())
        {
            const auto lx0 = (float) tickToX ((double) song.loopStart), lx1 = (float) tickToX ((double) song.loopEnd);
            g.setColour (colour::accent.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (lx0, (float) grid.getY(), lx1 - lx0, (float) grid.getHeight()));
            g.setColour (colour::accent.withAlpha (0.45f));
            g.drawVerticalLine ((int) lx0, (float) grid.getY(), (float) grid.getBottom());
            g.drawVerticalLine ((int) lx1, (float) grid.getY(), (float) grid.getBottom());
        }

        // end of song
        if (! song.playlist.empty())
        {
            const auto endX = (float) tickToX ((double) song.songLength());
            g.setColour (colour::bg0.withAlpha (0.45f));
            g.fillRect (juce::Rectangle<float> (endX, (float) grid.getY(), (float) grid.getRight() - endX, (float) grid.getHeight()));
        }

        g.setFont (font (type::caption));

        // muted clips (the Mute tool) are veiled
        auto veilIfMuted = [&g] (const Clip& clip, juce::Rectangle<float> area) {
            if (clip.muted)
            {
                g.setColour (colour::bg1.withAlpha (0.62f));
                g.fillRoundedRectangle (area, radius::sm);
                g.setColour (colour::textDisabled);
                g.drawLine (area.getX() + 2.0f, area.getBottom() - 2.0f, area.getRight() - 2.0f, area.getY() + 2.0f, 1.0f);
            }
        };

        SmallClips smallClips;
        const auto physicalScale = g.getInternalContext().getPhysicalPixelScaleFactor();

        for (int i = 0; i < (int) song.playlist.size(); ++i)
        {
            const auto& c = song.playlist[(std::size_t) i];
            const auto x0 = (float) tickToX ((double) c.start);
            const auto x1 = (float) tickToX ((double) c.end());
            const auto y = (float) trackToY (c.track);
            const auto trackHeight = heightOf (c.track);

            if (x1 < grid.getX() || x0 > grid.getRight() || y > grid.getBottom() || y + trackHeight < grid.getY())
                continue;

            const auto r = juce::Rectangle<float> (x0, y + 1.0f, juce::jmax (3.0f, x1 - x0 - 1.0f), (float) trackHeight - 3.0f);

            if (! g.getClipBounds().toFloat().intersects (r.expanded (1.0f)))
                continue; // outside the area being repainted

            if (noteedit::isFoldedAway (song, c.track)) // a folded group: each member's clips as a thin bar (FL)
            {
                const auto barColour = c.kind == ClipKind::audio        ? colour::audioClip
                                       : c.kind == ClipKind::automation ? colour::automation
                                                                        : tame (patternColour (song, c.pattern));
                g.setColour (barColour.withAlpha (c.muted ? 0.35f : 0.9f));
                g.fillRect (juce::Rectangle<float> (x0, y + 2.0f, juce::jmax (2.0f, x1 - x0 - 1.0f), (float) foldedHeight - 4.0f));
                if (isSelected (i))
                {
                    g.setColour (colour::textPrimary);
                    g.drawRect (juce::Rectangle<float> (x0, y + 1.0f, juce::jmax (2.0f, x1 - x0 - 1.0f), (float) foldedHeight - 2.0f), 1.0f);
                }
                continue;
            }

            if (r.getWidth() < SmallClips::maxWidth && c.kind != ClipKind::automation && ! c.muted && ! isSelected (i))
            {
                addSmallClip (smallClips, physicalScale, c, r, c.kind == ClipKind::pattern && c.pattern == song.currentPattern);
                continue;
            }

            if (c.kind == ClipKind::automation)
            {
                paintAutomationClip (g, c, r);
                veilIfMuted (c, r);

                if (isSelected (i))
                    paintClipOutline (g, r, colour::automation, true, false);
                continue;
            }

            if (c.kind == ClipKind::audio)
            {
                paintAudioClip (g, c, r);
                veilIfMuted (c, r);

                if (isSelected (i))
                    paintClipOutline (g, r, colour::audioClip, true, false);
                continue;
            }

            const auto base = tame (patternColour (song, c.pattern));
            const auto current = c.pattern == song.currentPattern;
            const auto selected = isSelected (i);

            // body + name strip
            const auto clipBody = paintClipFrame (g, r, base, juce::String::fromUTF8 (song.patterns[(std::size_t) c.pattern].name.c_str()));

            // note thumbnail: the pattern's notes as small rounded bars in a light tint of the clip colour,
            // spread over the body's height by pitch (a few keys of padding so single notes don't hug the edges)
            const auto& notes = song.patterns[(std::size_t) c.pattern].notes;

            if (! notes.empty() && r.getWidth() > 6 && clipBody.getHeight() >= 4.0f)
            {
                const uitrace::Part tally (uitrace::part::notes);
                int low = 127, high = 0;
                for (const auto& n : notes)
                {
                    low = juce::jmin (low, n.key);
                    high = juce::jmax (high, n.key);
                }

                const auto body = clipBody.reduced (1.0f, clipBody.getHeight() >= 12.0f ? 3.0f : 1.0f);
                const auto span = juce::jmax (6, high - low + 2); // a small pitch range stays centred
                const auto bottomKey = (low + high) / 2.0f - span / 2.0f;
                const auto rows = (float) span + 1.0f;
                const auto noteHeight = juce::jlimit (1.5f, 2.5f, body.getHeight() / rows);
                const auto patternLength = juce::jmax<std::int64_t> (1, song.patterns[(std::size_t) c.pattern].effectiveLength (song.beatsPerBar));
                g.setColour (clipContentColour (base));

                juce::Graphics::ScopedSaveState clip (g);
                g.reduceClipRegion (body.toNearestInt());
                const auto visible = g.getClipBounds().toFloat();
                // small notes as plain rectangles (a rounded one is a path each on the GPU renderer; at 2 px the
                // rounding does not show), notes outside the repainted area skipped
                const auto rounded = noteHeight > 2.0f && r.getWidth() > 60.0f;

                // the pattern repeats along a clip longer than it (FL loops pattern clips that are stretched)
                for (auto loopStart = c.start - c.offset % patternLength; loopStart < c.end(); loopStart += patternLength)
                {
                    if (tickToX ((double) loopStart) > visible.getRight())
                        break;
                    if (tickToX ((double) (loopStart + patternLength)) < visible.getX())
                        continue;
                    for (const auto& n : notes)
                    {
                        const auto at = loopStart + n.start;
                        if (at + n.length <= c.start || at >= c.end())
                            continue;
                        const auto nx = (float) tickToX ((double) at);
                        const auto nw = juce::jmax (rounded ? 2.0f : 1.0f, (float) (n.length * pixelsPerTick) - 1.0f);
                        if (nx > visible.getRight() || nx + nw < visible.getX())
                            continue;
                        const auto ny = body.getBottom() - ((float) n.key - bottomKey + 0.5f) / rows * body.getHeight() - noteHeight * 0.5f;
                        if (rounded)
                            g.fillRoundedRectangle (nx, ny, nw, noteHeight, juce::jmin (1.0f, noteHeight * 0.5f));
                        else
                            g.fillRect (nx, ny, nw, noteHeight);
                    }
                }
            }

            veilIfMuted (c, r);
            paintClipOutline (g, r, base, selected, current);
        }

        {
            const uitrace::Part tally (uitrace::part::batch);
            smallClips.fill (g);
        }

        // the clip under the mouse: outline, the grip of the edge it is on, its menu button
        if (hover.clip >= 0 && hover.clip < (int) song.playlist.size() && drag == Drag::none)
        {
            const auto r = clipBounds (hover.clip).toFloat();
            g.setColour (colour::textPrimary.withAlpha (isSelected (hover.clip) ? 0.0f : 0.55f));
            g.drawRoundedRectangle (r.reduced (0.5f), radius::sm, 1.2f);

            if (hover.target == gesture::Target::leftEdge || hover.target == gesture::Target::rightEdge)
            {
                const auto x = hover.target == gesture::Target::rightEdge ? r.getRight() - 4.0f : r.getX() + 1.0f;
                g.setColour (colour::textPrimary);
                g.fillRoundedRectangle (x, r.getY() + clipTitleHeight + 3.0f, 3.0f, juce::jmax (4.0f, r.getHeight() - clipTitleHeight - 6.0f), 1.5f);
            }

            if (const auto button = clipMenuButton (hover.clip); ! button.isEmpty())
            {
                const auto b = button.toFloat();
                g.setColour (hover.target == gesture::Target::clipMenu ? colour::bg0.withAlpha (0.75f) : colour::bg0.withAlpha (0.45f));
                g.fillRoundedRectangle (b, 2.0f);
                icons::draw (g, Icon::chevronDown, b.reduced (1.0f), colour::textPrimary);
            }
        }

        if (! rubberBand.isEmpty())
        {
            g.setColour (colour::selectionFill);
            g.fillRect (rubberBand);
            g.setColour (colour::selection);
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
            g.setColour (colour::textPrimary);
            g.fillPath (dashed);
        }

        if (playheadTick >= 0.0)
        {
            g.setColour (colour::playhead);
            g.fillRect (juce::Rectangle<float> ((float) tickToX (playheadTick) - 0.5f, (float) grid.getY(), 1.5f, (float) grid.getHeight()));
        }
    }

    // --- ruler (drawn in its own coordinates, below the toolbar) -----------------
    {
        const uitrace::Scope traceRuler ("Playlist ruler", false);
        juce::Graphics::ScopedSaveState state (g);
        g.setOrigin (0, rulerTop);
        const auto ruler = juce::Rectangle<int> (headerWidth, 0, grid.getWidth(), timelineHeight);
        g.setColour (colour::bg2);
        g.fillRect (ruler);
        g.setColour (colour::border);
        g.fillRect (ruler.withTop (ruler.getBottom() - 1));
        g.reduceClipRegion (ruler);
        g.setFont (font (type::small));

        if (song.hasLoop()) // the time selection
        {
            const auto lx0 = (float) tickToX ((double) song.loopStart), lx1 = (float) tickToX ((double) song.loopEnd);
            g.setColour (colour::accent.withAlpha (0.16f));
            g.fillRect (juce::Rectangle<float> (lx0, 0.0f, lx1 - lx0, (float) timelineHeight));
            g.setColour (colour::accent.withAlpha (0.75f));
            g.fillRect (juce::Rectangle<float> (lx0, 0.0f, lx1 - lx0, 4.0f));
        }

        const auto barPixels = bar * pixelsPerTick;
        const auto every = barPixels < 30.0 ? (barPixels < 10.0 ? 16 : 4) : 1; // numbers spaced further apart when zoomed out

        for (const auto& b : song.barsBetween ((std::int64_t) std::max (0.0, scrollTicks), (std::int64_t) xToTick (grid.getRight()) + 1))
        {
            if ((b.number - 1) % every != 0)
                continue;

            const auto x = (int) tickToX ((double) b.tick);
            g.setColour (colour::textDisabled);
            g.drawVerticalLine (x, 12.0f, (float) timelineHeight);
            g.setColour (colour::textSecondary);
            textcache::drawText (g, juce::String (b.number), x + 3, 2, 40, timelineHeight - 4, juce::Justification::centredLeft);
        }

        // markers: small flag with the name; time signature changes show the fraction
        g.setFont (font (type::caption, true));

        for (const auto& m : song.markers())
        {
            const auto x = (float) tickToX ((double) m.tick);

            if (x < (float) ruler.getX() - 200.0f || x > (float) ruler.getRight())
                continue;

            auto label = juce::String::fromUTF8 (m.name.c_str());
            if (const auto fraction = juce::String (m.numerator) + "/" + juce::String (m.denominator); m.isSignature() && label != fraction)
                label = fraction + (label.isEmpty() ? "" : " " + label);
            if (label.isEmpty())
                label = "*";

            const auto width = (float) juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), label) + 8.0f;
            const auto tag = juce::Rectangle<float> (x, (float) timelineHeight - 11.0f, width, 11.0f);
            // recording markers (Start / Stop recording) in the accent colour, as the record button
            const auto punch = m.action == Marker::punchIn || m.action == Marker::punchOut;
            g.setColour (m.isSignature() ? colour::info : punch ? colour::accent : colour::warning);
            g.fillRoundedRectangle (tag, 2.0f);
            g.drawVerticalLine ((int) x, 0.0f, (float) timelineHeight);
            g.setColour (colour::bg0);
            textcache::drawText (g, label, tag.reduced (3.0f, 0.0f), juce::Justification::centredLeft, false);
        }

        if (playheadTick >= 0.0)
        {
            const auto x = (float) tickToX (playheadTick);
            juce::Path tri;
            tri.addTriangle (x - 6, 0, x + 6, 0, x, 9);
            g.setColour (colour::playhead);
            g.fillPath (tri);
        }
    }

    // --- track headers -----------------------------------------------------
    // FL's layout: the name on the left, a group header's fold arrow and the mute LED on the right; grouped
    // tracks sit indented on a block in their header's colour; members of a folded group are thin rows
    {
        const uitrace::Scope traceHeaders ("Playlist headers", false);
        const auto headers = juce::Rectangle<int> (0, headerHeight, headerWidth, grid.getHeight());
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (headers);
        g.setColour (colour::bg2);
        g.fillRect (headers);
        g.setFont (font (type::small));

        auto colourOf = [&song] (int t) { return t >= 0 && t < (int) song.playlistTracks.size() ? song.playlistTracks[(std::size_t) t].colour : 0u; };

        // group blocks first (behind the headers): from the header's top to the last member's bottom
        for (int t = 0; t < tracks; ++t)
        {
            if (! noteedit::isGroupHeader (song, t))
                continue;
            const auto members = noteedit::groupMembers (song, t);
            const auto last = members.empty() ? t : members.back();
            const auto top = trackToY (t), bottom = trackToY (last) + heightOf (last);
            if (top > headers.getBottom() || bottom < headers.getY())
                continue;
            auto block = colour::bg3;
            if (const auto c = colourOf (t); c != 0)
                block = block.interpolatedWith (tame (juce::Colour (c)), 0.28f);
            g.setColour (block);
            g.fillRoundedRectangle (juce::Rectangle<float> (1.0f, (float) top + 1.0f, (float) headerWidth - 2.0f, (float) (bottom - top) - 2.0f), radius::md);
        }

        for (int t = 0; t < tracks; ++t)
        {
            const auto y = trackToY (t);
            const auto trackHeight = heightOf (t);

            if (y > headers.getBottom() || y + trackHeight < headers.getY())
                continue;

            if (noteedit::isFoldedAway (song, t))
                continue; // the group block shows through: a thin row, as FL draws a folded group

            const auto muted = song.isTrackMuted (t);
            const auto custom = t < (int) song.playlistTracks.size() && ! song.playlistTracks[(std::size_t) t].name.empty();
            const auto hovered = hover.header && hover.track == t;
            const auto member = noteedit::groupHeaderOf (song, t) >= 0 && ! noteedit::isGroupHeader (song, t);
            const auto groupHead = noteedit::isGroupHeader (song, t);
            const auto left = member ? (float) groupIndent : 2.0f;
            auto plate = muted ? colour::bg2 : colour::bg3;
            const auto trackColour = colourOf (t);
            if (trackColour != 0) // the track's colour (FL projects colour their track headers by group)
                plate = plate.interpolatedWith (tame (juce::Colour (trackColour)), muted ? 0.22f : (groupHead ? 0.62f : 0.5f));
            if (hovered)
                plate = plate.interpolatedWith (colour::bg5, 0.6f);
            const auto plateArea = juce::Rectangle<float> (left, (float) y + 1.0f, (float) headerWidth - 2.0f - left, (float) trackHeight - 3.0f);
            g.setColour (plate);
            g.fillRoundedRectangle (plateArea, radius::sm);

            if (t == currentTrack) // the selected track (FL marks it next to the LED)
            {
                g.setColour (colour::accent);
                g.fillRoundedRectangle (plateArea.withWidth (3.0f).reduced (0.0f, 3.0f), 1.5f);
            }

            const auto led = trackLed (t).toFloat();
            g.setColour (muteLed (muted));
            g.fillEllipse (led.withSizeKeepingCentre (8.0f, 8.0f));
            if (hovered && hover.led) // the LED is a button: ring it
            {
                g.setColour (colour::textPrimary.withAlpha (0.7f));
                g.drawEllipse (led.withSizeKeepingCentre (12.0f, 12.0f), 1.0f);
            }

            auto textRight = led.getX() - 2.0f;
            if (const auto arrow = trackArrow (t); ! arrow.isEmpty())
            {
                const auto collapsed = t < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) t].collapsed;
                const auto a = arrow.toFloat().withSizeKeepingCentre (8.0f, 5.0f);
                juce::Path tri;
                if (collapsed) // folded: points down (FL), open: up
                    tri.addTriangle (a.getX(), a.getY(), a.getRight(), a.getY(), a.getCentreX(), a.getBottom());
                else
                    tri.addTriangle (a.getX(), a.getBottom(), a.getRight(), a.getBottom(), a.getCentreX(), a.getY());
                g.setColour (hovered && hover.arrow ? colour::textPrimary : colour::textSecondary);
                g.fillPath (tri);
                textRight = (float) arrow.getX();
            }

            g.setColour (muted ? colour::textDisabled : (custom ? colour::textPrimary : colour::textSecondary));
            g.setFont (font (type::small, groupHead));
            const auto textLeft = (int) left + 6;
            textcache::drawFittedText (g, custom ? juce::String::fromUTF8 (song.playlistTracks[(std::size_t) t].name.c_str())
                                     : tr ("Track {0}", { juce::String (t + 1) }),
                              textLeft, y, juce::jmax (8, (int) textRight - textLeft - 2), trackHeight, juce::Justification::centredLeft,
                              trackHeight >= 30 ? 2 : 1, 0.8f);

            // the divider under the header: lit while hovered or dragged (drag resizes the track)
            if (hover.divider == t || (drag == Drag::trackResize && (resizeTrack == t || resizeAll)))
            {
                g.setColour (colour::accent);
                g.fillRoundedRectangle (juce::Rectangle<float> (6.0f, (float) (y + trackHeight) - 2.0f, (float) headerWidth - 12.0f, 3.0f), 1.5f);
            }
        }
    }

    // toolbar band, and the corner left of the ruler
    g.setColour (colour::bg3);
    g.fillRect (0, 0, getWidth(), toolbarHeight);
    g.setColour (colour::bg2);
    g.fillRect (0, toolbarHeight, headerWidth, headerHeight - toolbarHeight);
    g.setColour (colour::border);
    g.fillRect (0, toolbarHeight - 1, getWidth(), 1);
    g.fillRect (0, headerHeight - 1, headerWidth, 1);
    g.fillRect (headerWidth - 1, toolbarHeight, 1, getHeight() - toolbarHeight);
    uitrace::flushParts ("Playlist", trace.elapsedMs());
}

void PlaylistComponent::paintOverview (juce::Graphics& g, juce::Rectangle<float> area, double total)
{
    // every clip of the arrangement in miniature (its colour, its track's row), bar lines, the song's end
    using namespace theme;
    const auto& song = engine.getSong();
    const auto scale = area.getWidth() / juce::jmax (1.0, total);
    const auto bar = (double) ppq * juce::jmax (1, song.beatsPerBar);

    g.setColour (colour::bg2);
    g.fillRect (area.withWidth (juce::jmin (area.getWidth(), (float) (song.songLength() * scale))));

    const auto every = bar * scale < 3.0 ? 16 : bar * scale < 6.0 ? 4 : 1;
    g.setColour (colour::gridBar.withAlpha (0.5f));
    for (int b = every; b * bar < total; b += every)
        g.fillRect ((float) (area.getX() + b * bar * scale), area.getY(), 1.0f, area.getHeight());

    int rows = 1;
    for (const auto& c : song.playlist)
        rows = juce::jmax (rows, c.track + 1);

    const auto inner = area.reduced (0.0f, 1.0f);
    const auto rowHeight = inner.getHeight() / (float) rows;

    for (const auto& c : song.playlist)
    {
        auto base = colour::automation;
        if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size())
            base = tame (patternColour (song, c.pattern));
        else if (c.kind == ClipKind::audio && c.channel >= 0 && c.channel < (int) song.channels.size())
            base = tame (audioClipColour (song.channels[(std::size_t) c.channel]));

        const auto y = inner.getY() + rowHeight * (float) c.track;
        g.setColour (c.muted ? base.withAlpha (0.35f) : base);
        g.fillRect ((float) (area.getX() + c.start * scale), y, juce::jmax (1.0f, (float) (c.length * scale) - 0.5f),
                    juce::jmax (1.0f, rowHeight - (rowHeight >= 3.0f ? 0.5f : 0.0f)));
    }
}

void PlaylistComponent::paintOverChildren (juce::Graphics& g)
{
    if (! ripple)
        return;

    // ripple editing is on: a small tag in the grid's top-right corner
    const auto grid = gridArea();
    const auto tag = juce::Rectangle<float> ((float) grid.getRight() - 70.0f, (float) grid.getY() + 4.0f, 64.0f, 18.0f);
    g.setColour (theme::colour::accentMuted);
    g.fillRoundedRectangle (tag, theme::radius::sm);
    g.setColour (theme::colour::accent);
    g.drawRoundedRectangle (tag.reduced (0.5f), theme::radius::sm, 1.0f);
    g.setFont (theme::font (theme::type::caption, true));
    textcache::drawText (g, tr ("Ripple"), tag.toNearestInt(), juce::Justification::centred);
}

void PlaylistComponent::addSmallClip (SmallClips& batch, float physicalScale, const Clip& c, juce::Rectangle<float> r, bool current)
{
    const auto& song = engine.getSong();
    juce::Colour base;
    if (c.kind == ClipKind::audio)
    {
        if (c.channel < 0 || c.channel >= (int) song.channels.size())
            return;
        base = audioClipColour (song.channels[(std::size_t) c.channel]);
    }
    else
        base = theme::tame (patternColour (song, c.pattern));

    const auto title = r.withHeight (clipHeaderHeight (r));
    const auto body = r.withTrimmedTop (title.getHeight());
    batch.bodies[clipBodyColour (base).getARGB()].addWithoutMerging (body);
    if (title.getHeight() >= 4.0f)
        batch.bodies[clipHeaderColour (base).getARGB()].addWithoutMerging (title);
    batch.edges[(current ? theme::colour::textPrimary.withAlpha (0.55f) : theme::colour::bg0.withAlpha (0.85f)).getARGB()].addWithoutMerging (
        { r.getRight() - 1.0f, r.getY(), 1.0f, r.getHeight() });

    if (c.kind == ClipKind::audio && body.getWidth() >= 3.0f && body.getHeight() >= 3.0f)
    {
        const auto& channel = song.channels[(std::size_t) c.channel];
        const auto path = juce::String::fromUTF8 (channel.samplePath.c_str());
        const auto summary = ui::WaveformCache::getInstance()->find (path, [this, path] { return engine.getSampleLibrary().load (path); });
        if (summary == nullptr)
            return;
        // the same mapping as paintAudioClip
        const auto wave = body.reduced (0.0f, body.getHeight() >= 14.0f ? 2.0f : 1.0f);
        const auto total = (double) summary->frames;
        const auto perTick = clipops::framesPerTick (channel.sampler, total, summary->sampleRate, song.bpm);
        const auto startTick = (double) c.offset + (double) (wave.getX() - r.getX()) / pixelsPerTick;
        const auto forward = ! channel.sampler.reverse;
        const auto startFrame = forward ? startTick * perTick : total - startTick * perTick;
        const auto colour = clipContentColour (base);
        auto style = ui::currentWaveStyle (colour, juce::jmin (ui::normalisingGain (*summary), 4.0f) * c.gain);
        if (! style.spectral && c.gain > 1.0f)
            style.gain = juce::jmin (style.gain, 8.0f);
        if (! style.spectral)
            ui::appendWaveformPath (batch.waves[colour.getARGB()], physicalScale, wave, wave, *summary, startFrame,
                                    (forward ? perTick : -perTick) / pixelsPerTick, style);
        return;
    }

    if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size() && body.getHeight() >= 4.0f)
    {
        // the notes as one-pixel-wide marks at their pitch (the same placement as the full clip)
        const auto& notes = song.patterns[(std::size_t) c.pattern].notes;
        if (notes.empty())
            return;
        int low = 127, high = 0;
        for (const auto& n : notes)
        {
            low = juce::jmin (low, n.key);
            high = juce::jmax (high, n.key);
        }
        const auto inner = body.reduced (0.0f, body.getHeight() >= 12.0f ? 3.0f : 1.0f);
        const auto span = juce::jmax (6, high - low + 2);
        const auto bottomKey = (low + high) / 2.0f - span / 2.0f;
        const auto rows = (float) span + 1.0f;
        const auto noteHeight = juce::jlimit (1.5f, 2.5f, inner.getHeight() / rows);
        const auto patternLength = juce::jmax<std::int64_t> (1, song.patterns[(std::size_t) c.pattern].effectiveLength (song.beatsPerBar));
        auto& marks = batch.contents[clipContentColour (base).getARGB()];
        for (auto loopStart = c.start - c.offset % patternLength; loopStart < c.end(); loopStart += patternLength)
            for (const auto& n : notes)
            {
                const auto at = loopStart + n.start;
                if (at + n.length <= c.start || at >= c.end())
                    continue;
                const auto nx = juce::jmax (inner.getX(), (float) tickToX ((double) at));
                const auto nr = juce::jmin (inner.getRight() - 1.0f, nx + juce::jmax (1.0f, (float) (n.length * pixelsPerTick) - 1.0f));
                if (nr <= nx)
                    continue;
                const auto ny = inner.getBottom() - ((float) n.key - bottomKey + 0.5f) / rows * inner.getHeight() - noteHeight * 0.5f;
                marks.addWithoutMerging ({ nx, ny, nr - nx, noteHeight });
            }
    }
}

void PlaylistComponent::paintAudioClip (juce::Graphics& g, const Clip& c, juce::Rectangle<float> r)
{
    const uitrace::Scope trace ("Playlist audio clip", false);
    const auto& song = engine.getSong();

    if (c.channel < 0 || c.channel >= (int) song.channels.size())
        return;

    const auto& channel = song.channels[(std::size_t) c.channel];
    const auto colour = audioClipColour (channel);
    const auto name = juce::String::fromUTF8 (channel.name.c_str());
    const auto body = paintClipFrame (g, r, colour, name, ClipGlyph::audio);

    // the waveform (ui/WaveformView, analysed in the background): the clean style fills the body with the
    // peak envelope mirrored around the centre, in a light tint of the clip colour
    const auto path = juce::String::fromUTF8 (channel.samplePath.c_str());
    const auto summary = ui::WaveformCache::getInstance()->find (path, [this, path] { return engine.getSampleLibrary().load (path); });

    if (summary != nullptr && body.getHeight() >= 3.0f && body.getWidth() >= 3.0f) // (narrower: the body colour says enough)
    {
        const auto wave = body.reduced (0.0f, body.getHeight() >= 14.0f ? 2.0f : 1.0f);
        const auto total = (double) summary->frames;
        const auto perTick = clipops::framesPerTick (channel.sampler, total, summary->sampleRate, song.bpm);
        // r's left edge is the clip start, i.e. tick `offset` of the audio (counted from the file's end when
        // the channel is reversed)
        const auto startTick = (double) c.offset + (double) (wave.getX() - r.getX()) / pixelsPerTick;
        const auto forward = ! channel.sampler.reverse;
        const auto startFrame = forward ? startTick * perTick : total - startTick * perTick;
        auto style = ui::currentWaveStyle (clipContentColour (colour), juce::jmin (ui::normalisingGain (*summary), 4.0f) * c.gain);
        if (! style.spectral && c.gain > 1.0f) // louder clip gain: the shape may hit the top, which is honest
            style.gain = juce::jmin (style.gain, 8.0f);
        const uitrace::Part tally (uitrace::part::waveform);
        ui::paintWaveform (g, wave, *summary, startFrame, (forward ? perTick : -perTick) / pixelsPerTick, style);
    }

    paintClipOutline (g, r, colour, false, false);

    // fades: a shaded ramp over the body and a handle at the header's corner
    if (c.fadeIn > 0 || c.fadeOut > 0)
    {
        const auto inX = r.getX() + (float) (c.fadeIn * pixelsPerTick);
        const auto outX = r.getRight() - (float) (c.fadeOut * pixelsPerTick);
        const auto shade = theme::colour::bg0.withAlpha (0.45f);
        const auto line = clipContentColour (colour).withAlpha (0.9f);

        if (c.fadeIn > 0)
        {
            juce::Path p;
            p.startNewSubPath (body.getX(), body.getY());
            p.lineTo (inX, body.getY());
            p.lineTo (body.getX(), body.getBottom());
            p.closeSubPath();
            g.setColour (shade);
            g.fillPath (p);
            g.setColour (line);
            g.drawLine (body.getX(), body.getBottom(), inX, body.getY(), 1.2f);
        }
        if (c.fadeOut > 0)
        {
            juce::Path p;
            p.startNewSubPath (outX, body.getY());
            p.lineTo (body.getRight(), body.getY());
            p.lineTo (body.getRight(), body.getBottom());
            p.closeSubPath();
            g.setColour (shade);
            g.fillPath (p);
            g.setColour (line);
            g.drawLine (outX, body.getY(), body.getRight(), body.getBottom(), 1.2f);
        }
    }
}

juce::Colour PlaylistComponent::audioClipColour (const ChannelDef& channel)
{
    return channel.colour != 0 ? theme::tame (juce::Colour (channel.colour)) : theme::colour::audioClip;
}

void PlaylistComponent::paintAutomationClip (juce::Graphics& g, const Clip& c, juce::Rectangle<float> r)
{
    const auto& song = engine.getSong();

    if (c.automation < 0 || c.automation >= (int) song.automations.size())
        return;

    const auto& automation = song.automations[(std::size_t) c.automation];
    const auto colour = automation.target.kind == AutomationTargetKind::none ? theme::colour::textDisabled
                                                                              : theme::colour::automation.withMultipliedSaturation (0.75f).withMultipliedBrightness (0.8f);
    paintClipFrame (g, r, colour, juce::String::fromUTF8 (automation.name.c_str()), ClipGlyph::automation);
    paintClipOutline (g, r, colour, false, false);

    // automation curve over the clip's range
    const auto body = r.withTrimmedTop (14.0f).reduced (1.0f, 2.0f);
    const auto clip = g.getClipBounds().toFloat();
    juce::Path curve;
    bool started = false;

    for (auto x = std::max (body.getX(), clip.getX()); x <= std::min (body.getRight(), clip.getRight()); x += 2.0f)
    {
        const auto value = automation.valueAt ((double) c.offset + xToTick ((int) x) - (double) c.start);
        const auto y = body.getBottom() - value * body.getHeight();

        if (! started)
            curve.startNewSubPath (x, y);
        else
            curve.lineTo (x, y);

        started = true;
    }

    g.setColour (automation.target.kind == AutomationTargetKind::none ? theme::colour::textSecondary : theme::colour::automation.brighter (0.3f));
    g.strokePath (curve, juce::PathStrokeType (1.5f));

    // the points inside the clip, as handles (edited right here)
    if (r.getWidth() > 24.0f)
        for (const auto& p : automation.points)
        {
            const auto songTick = (double) (c.start + p.tick - c.offset);
            if (songTick < (double) c.start || songTick > (double) c.end())
                continue;
            const auto x = (float) tickToX (songTick);
            const auto y = body.getBottom() - p.value * body.getHeight();
            g.setColour (theme::colour::bg0);
            g.fillEllipse (x - 3.5f, y - 3.5f, 7.0f, 7.0f);
            g.setColour (theme::colour::automation.brighter (0.5f));
            g.fillEllipse (x - 2.5f, y - 2.5f, 5.0f, 5.0f);
        }
}

juce::Rectangle<float> PlaylistComponent::automationBody (const Clip& c) const
{
    const auto x0 = (float) tickToX ((double) c.start), x1 = (float) tickToX ((double) c.end());
    const auto y = (float) trackToY (c.track);
    const auto r = juce::Rectangle<float> (x0, y + 1.0f, juce::jmax (3.0f, x1 - x0 - 1.0f), (float) heightOf (c.track) - 3.0f);
    return r.withTrimmedTop (juce::jmin (14.0f, r.getHeight() * 0.4f)).reduced (1.0f, 2.0f);
}

int PlaylistComponent::automationPointAt (int clip, juce::Point<int> pos) const
{
    const auto& song = engine.getSong();

    if (clip < 0 || clip >= (int) song.playlist.size() || song.playlist[(std::size_t) clip].kind != ClipKind::automation)
        return -1;

    const auto& c = song.playlist[(std::size_t) clip];
    if (c.automation < 0 || c.automation >= (int) song.automations.size())
        return -1;

    const auto body = automationBody (c);
    const auto& points = song.automations[(std::size_t) c.automation].points;

    for (int i = (int) points.size() - 1; i >= 0; --i)
    {
        const auto songTick = (double) (c.start + points[(std::size_t) i].tick - c.offset);
        if (songTick < (double) c.start || songTick > (double) c.end())
            continue;
        const juce::Point<float> at ((float) tickToX (songTick), body.getBottom() - points[(std::size_t) i].value * body.getHeight());
        if (at.getDistanceFrom (pos.toFloat()) <= 6.0f)
            return i;
    }

    return -1;
}

bool PlaylistComponent::isOnAutomationCurve (int clip, juce::Point<int> pos) const
{
    const auto& song = engine.getSong();

    if (clip < 0 || clip >= (int) song.playlist.size() || song.playlist[(std::size_t) clip].kind != ClipKind::automation)
        return false;

    const auto& c = song.playlist[(std::size_t) clip];
    if (c.automation < 0 || c.automation >= (int) song.automations.size())
        return false;

    const auto body = automationBody (c);
    if (! body.expanded (0.0f, 4.0f).contains (pos.toFloat()))
        return false;

    const auto value = song.automations[(std::size_t) c.automation].valueAt ((double) c.offset + xToTick (pos.x) - (double) c.start);
    return std::abs (body.getBottom() - value * body.getHeight() - (float) pos.y) <= 5.0f;
}

int PlaylistComponent::fadeHandleAt (int index, juce::Point<int> p) const
{
    const auto& c = engine.getSong().playlist[(std::size_t) index];

    if (c.kind != ClipKind::audio)
        return 0;

    const auto y = trackToY (c.track) + 1 + 15;
    const auto inX = tickToX ((double) c.start) + c.fadeIn * pixelsPerTick;
    const auto outX = tickToX ((double) c.end()) - c.fadeOut * pixelsPerTick;

    if (std::abs (p.y - y) > 6)
        return 0;
    if (std::abs (p.x - (inX + 2.0)) <= 6.0)
        return 1;
    if (std::abs (p.x - (outX - 2.0)) <= 6.0)
        return 2;
    return 0;
}

int PlaylistComponent::clipAt (int x, int y, bool& onRightEdge) const
{
    bool left = false;
    return clipAt (x, y, onRightEdge, left);
}

int PlaylistComponent::clipAt (int x, int y, bool& onRightEdge, bool& onLeftEdge) const
{
    const auto& playlist = engine.getSong().playlist;
    const auto track = yToTrack (y);
    onRightEdge = onLeftEdge = false;

    for (int i = (int) playlist.size() - 1; i >= 0; --i)
    {
        const auto& c = playlist[(std::size_t) i];

        if (c.track != track)
            continue;

        // the left edge only on clips wide enough to still be grabbed in the middle
        gesture::EdgeRules rules;
        rules.minimumForLeft = 24.0;

        if (const auto zone = gesture::zoneAt (x, tickToX ((double) c.start), tickToX ((double) c.end()), rules); zone != gesture::Zone::outside)
        {
            onRightEdge = zone == gesture::Zone::rightEdge;
            onLeftEdge = zone == gesture::Zone::leftEdge;
            return i;
        }
    }

    return -1;
}

void PlaylistComponent::seekTo (int x)
{
    const auto tick = juce::jmax (0.0, (double) snap (xToTick (x), true));

    if (engine.getSong().mode != PlayMode::song)
        engine.editSong ([] (Song& s) { s.mode = PlayMode::song; }, false);

    engine.seek (tick);
    playheadTick = tick;
    repaint();
}

bool PlaylistComponent::isSelected (int index) const
{
    return std::binary_search (selection.begin(), selection.end(), index);
}

void PlaylistComponent::setSelection (std::vector<int> indices)
{
    std::sort (indices.begin(), indices.end());
    indices.erase (std::unique (indices.begin(), indices.end()), indices.end());
    selection = std::move (indices);
    knownClipCount = engine.getSong().playlist.size();
    repaint();
}

void PlaylistComponent::sortClips()
{
    auto tracked = selection;
    engine.editSong ([&tracked] (Song& s) {
        noteedit::sortByStartTracking (s.playlist, tracked);
        trackmodes::followAudioTracks (s); // audio clips on an audio track play through its mixer track (round 146a)
    }, false);
    setSelection (tracked);
}

Clip PlaylistComponent::makeBrushClip (int track, std::int64_t start) const
{
    // what is chosen in the Picker: the current pattern, an audio clip or an automation clip
    const auto& song = engine.getSong();
    Clip c;
    c.pattern = song.currentPattern;
    c.track = track;
    c.start = juce::jmax<std::int64_t> (0, start);
    c.length = song.patterns[(std::size_t) song.currentPattern].effectiveLength (song.beatsPerBar);

    if (brush.kind == ClipKind::audio && brush.index >= 0 && brush.index < (int) song.channels.size()
        && song.channels[(std::size_t) brush.index].instrument == InstrumentKind::audioClip)
    {
        c.kind = ClipKind::audio;
        c.channel = brush.index;
        c.pattern = 0;

        // audio length in ticks at the current BPM
        if (auto data = engine.getSampleLibrary().load (juce::String::fromUTF8 (song.channels[(std::size_t) brush.index].samplePath.c_str())))
            c.length = juce::jmax<std::int64_t> (ppq / 4, (std::int64_t) ((double) data->frames() / data->sampleRate * song.bpm / 60.0 * ppq));
    }
    else if (brush.kind == ClipKind::automation && brush.index >= 0 && brush.index < (int) song.automations.size())
    {
        c.kind = ClipKind::automation;
        c.automation = brush.index;
        c.pattern = 0;
        c.length = (std::int64_t) ppq * juce::jmax (1, song.beatsPerBar) * 4; // 4 bars
    }

    return c;
}

void PlaylistComponent::setTimeSelection (std::int64_t from, std::int64_t to)
{
    engine.editSong ([from, to] (Song& s) {
        s.loopStart = from;
        s.loopEnd = to;
        s.mode = PlayMode::song;
    }, false);

    setSelection (to > from ? noteedit::clipsInTime (engine.getSong().playlist, from, to) : std::vector<int> {});
}

void PlaylistComponent::setTimeSelectionForSnapshot (int fromBar, int toBar)
{
    const auto bar = (std::int64_t) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    setTimeSelection (fromBar * bar, toBar * bar);
}

void PlaylistComponent::setViewForSnapshot (double pixelsPerBar, int firstBar, int firstTrack)
{
    const auto bar = (double) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    pixelsPerTick = pixelsPerBar / bar;
    scrollTicks = firstBar * bar;
    scrollY = juce::jmax (0, firstTrack) * trackHeight;
    updateScrollBars();
    repaint();
}

ui::Hint PlaylistComponent::hoverForSnapshot()
{
    const auto& playlist = engine.getSong().playlist;
    const auto grid = gridArea();

    // the last clip fully in view (the floating Channel Rack tends to cover the first ones)
    for (int i = (int) playlist.size() - 1; i >= 0; --i)
        if (const auto r = clipBounds (i); grid.contains (r) && r.getWidth() >= 40 && playlist[(std::size_t) i].kind == ClipKind::pattern)
        {
            const auto at = juce::Point<int> (r.getRight() - 2, r.getCentreY() + 4);
            updateHover (at, {});
            return hintAt (at);
        }

    return {};
}

void PlaylistComponent::mouseDown (const juce::MouseEvent& e)
{
    downMods = e.mods;
    startedOnEmpty = erasedSomething = holdExpired = false;
    hover = {};
    gliding = false;

    if (e.mods.isMiddleButtonDown() && e.x >= headerWidth) // middle-drag pans, in every tool
    {
        drag = Drag::pan;
        panStartTicks = scrollTicks;
        panStartY = scrollY;
        setMouseCursor (editorCursor (gesture::Cursor::pan));
        return;
    }

    // track header divider: drag resizes the track above it (Shift: every track)
    if (const auto divider = dividerAt (e.getPosition()); divider >= 0 && e.mods.isLeftButtonDown())
    {
        drag = Drag::trackResize;
        resizeTrack = divider;
        resizeAll = e.mods.isShiftDown();
        resizeStartHeight = heightOf (divider);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }

    // track header: the LED mutes (Ctrl+click or right-click on it: solo), a group header's arrow folds the
    // group, a click elsewhere selects the track; right-click elsewhere: track menu
    if (e.x < headerWidth && e.y >= headerHeight && e.y < gridArea().getBottom())
    {
        const auto track = yToTrack (e.y);

        if (noteedit::isFoldedAway (engine.getSong(), track)) // a thin row of a folded group: only its menu
        {
            if (e.mods.isPopupMenu())
                showTrackMenu (track);
            return;
        }

        const auto onLed = trackLed (track).expanded (3).contains (e.getPosition());
        const auto onArrow = trackArrow (track).contains (e.getPosition());

        if (onLed && (e.mods.isPopupMenu() || e.mods.isCtrlDown()))
            engine.editSong ([track, count = numTracks()] (Song& s) { noteedit::soloTrack (s, track, count); });
        else if (e.mods.isPopupMenu())
            showTrackMenu (track);
        else if (onLed)
            editTrack (track, [] (PlaylistTrack& t) { t.muted = ! t.muted; });
        else if (onArrow)
            engine.editSong ([track] (Song& s) {
                const auto folded = track < (int) s.playlistTracks.size() && s.playlistTracks[(std::size_t) track].collapsed;
                noteedit::setGroupCollapsed (s, track, ! folded);
            });
        else if (! noteedit::isFoldedAway (engine.getSong(), track))
        {
            currentTrack = track;
            repaint (0, headerHeight, headerWidth, getHeight() - headerHeight);
        }
        return;
    }

    // ruler: click = position; Ctrl+drag = time selection (Ctrl+click or double click clears it)
    if (e.y >= rulerTop && e.y < headerHeight && e.x >= headerWidth)
    {
        if (e.mods.isPopupMenu())
        {
            showRulerMenu (e.x);
            return;
        }

        if (e.getNumberOfClicks() >= 2)
        {
            setTimeSelection (0, 0);
            return;
        }

        loopAnchor = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), ! e.mods.isAltDown()));

        // Shift+drag inside the time selection moves it with what it holds (Ctrl+Shift: a copy)
        if (const auto& song = engine.getSong(); e.mods.isShiftDown() && song.hasLoop() && xToTick (e.x) >= (double) song.loopStart
                                                    && xToTick (e.x) < (double) song.loopEnd)
        {
            drag = Drag::timeMove;
            timeMoveOriginals = song.playlist;
            timeMoveFrom = song.loopStart;
            timeMoveTo = song.loopEnd;
            timeMoveAnchor = loopAnchor;
            timeMoveCopy = e.mods.isCtrlDown();
            return;
        }

        if (e.mods.isCtrlDown())
        {
            drag = Drag::loop;
            return;
        }

        drag = Drag::seek;
        seekTo (e.x);
        EditorGlobals::get().seekMark = (std::int64_t) juce::jmax (0.0, (double) snap (xToTick (e.x), true));
        return;
    }

    if (! gridArea().contains (e.getPosition()))
        return;

    bool rightEdge = false, leftEdge = false;
    const auto hit = clipAt (e.x, e.y, rightEdge, leftEdge);
    const auto tool = tools.getTool();

    // Ctrl+right-click on empty space: zoom to show everything; again: back to the previous view
    if (e.mods.isRightButtonDown() && e.mods.isCtrlDown() && hit < 0)
    {
        if (zoomBeforeAll > 0.0)
        {
            pixelsPerTick = zoomBeforeAll;
            scrollTicks = scrollBeforeAll;
            zoomBeforeAll = 0.0;
            updateScrollBars();
            repaint();
        }
        else
        {
            zoomBeforeAll = pixelsPerTick;
            scrollBeforeAll = scrollTicks;
            zoomAll();
        }
        return;
    }

    // the menu button in a clip's title strip
    if (hit >= 0 && e.mods.isLeftButtonDown() && ! e.mods.isCtrlDown() && clipMenuButton (hit).contains (e.getPosition()))
    {
        showClipMenu (hit);
        return;
    }

    // Zoom tool: right-click zooms out around the point (a click zooms in, a drag zooms to the range)
    if (tool == EditTool::zoom && e.mods.isRightButtonDown() && ! e.mods.isAltDown())
    {
        zoomOut (e.x);
        return;
    }

    // Alt+right-click on a clip: hear it while the button is held
    if (e.mods.isRightButtonDown() && e.mods.isAltDown() && hit >= 0)
    {
        startPreview (hit, e.x);
        return;
    }

    if (e.mods.isRightButtonDown())
    {
        // on an automation point: delete that point
        if (const auto point = automationPointAt (hit, e.getPosition()); point >= 0)
        {
            const auto automation = engine.getSong().playlist[(std::size_t) hit].automation;
            engine.editSong ([automation, point] (Song& s) {
                auto& points = s.automations[(std::size_t) automation].points;
                if (points.size() > 1 && point < (int) points.size())
                    points.erase (points.begin() + point);
            });
            return;
        }

        // on the clip menu icon (top left): the clip menu; anywhere else on a clip: delete, as in FL
        // (press and hold on empty space: Knife)
        if (hit >= 0 && clipMenuButton (hit).contains (e.getPosition()))
        {
            showClipMenu (hit);
            return;
        }

        // double right-click on a clip: the first click deleted it; bring it back muted (or unmuted)
        if (e.getNumberOfClicks() >= 2 && lastClipEraseTime != 0 && lastClipErasePos.getDistanceFrom (e.getPosition()) < 6
            && juce::Time::getMillisecondCounter() - lastClipEraseTime < 1000)
        {
            lastClipEraseTime = 0;
            engine.undo();
            bool r = false, l = false;
            if (const auto clip = clipAt (e.x, e.y, r, l); clip >= 0)
                engine.editSong ([clip] (Song& s) { s.playlist[(std::size_t) clip].muted = ! s.playlist[(std::size_t) clip].muted; });
            return;
        }

        drag = Drag::erase;
        startedOnEmpty = hit < 0;
        setSelection ({}); // indices change while deleting
        lastClipEraseTime = 0;

        if (hit >= 0)
        {
            erasedSomething = true;
            lastClipEraseTime = juce::Time::getMillisecondCounter();
            lastClipErasePos = e.getPosition();
            engine.editSong ([hit] (Song& s) { s.playlist.erase (s.playlist.begin() + hit); });
        }
        return;
    }

    // every tool: Ctrl+click on a clip adds/removes it from the selection; Ctrl+drag selects (Shift adds)
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
        || (hit < 0 && e.mods.isShiftDown() && tool != EditTool::brush))
    {
        drag = Drag::select;
        dragClip = hit; // a Lasso click without dragging selects this clip
        selectionBefore = e.mods.isShiftDown() ? selection : std::vector<int> {};
        rubberBand = {};
        setSelection (selectionBefore);
        return;
    }

    const auto snapOn = ! e.mods.isAltDown();

    auto grabClips = [&] (Drag kind) {
        if (! isSelected (hit))
            setSelection ({});

        const auto& playlist = engine.getSong().playlist;
        dragClip = hit;
        dragIndices = selection.empty() ? std::vector<int> { hit } : selection;
        dragOriginals.clear();

        for (auto i : dragIndices)
            dragOriginals.push_back (playlist[(std::size_t) i]);

        drag = kind;
        grabOffset = playlist[(std::size_t) hit].start - snap (xToTick (e.x), snapOn);
    };

    switch (tool)
    {
        case EditTool::eraser:
            drag = Drag::erase;
            setSelection ({});
            if (hit >= 0)
                engine.editSong ([hit] (Song& s) { s.playlist.erase (s.playlist.begin() + hit); });
            return;

        case EditTool::mute:
            drag = Drag::muting;
            touchedClips.clear();
            muteTarget = true;
            if (hit >= 0)
            {
                muteTarget = ! engine.getSong().playlist[(std::size_t) hit].muted;
                touchedClips.push_back (hit);
                engine.editSong ([hit, m = muteTarget] (Song& s) { s.playlist[(std::size_t) hit].muted = m; });
            }
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
            seekTo (e.x);
            engine.play();
            return;

        case EditTool::slip:
            if (hit >= 0)
                grabClips (Drag::slip);
            return;

        case EditTool::pencil:
        case EditTool::brush:
        case EditTool::drumPaint:
        case EditTool::lasso:
            break;
    }

    // automation clip: drag a point, or click on the curve to add one (Pencil and Brush)
    if (hit >= 0 && ! e.mods.isShiftDown() && (tool == EditTool::pencil || tool == EditTool::brush))
    {
        auto point = automationPointAt (hit, e.getPosition());

        if (point < 0 && isOnAutomationCurve (hit, e.getPosition()))
        {
            const auto& c = engine.getSong().playlist[(std::size_t) hit];
            AutomationPoint p;
            p.tick = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn) - c.start + c.offset);
            const auto body = automationBody (c);
            p.value = juce::jlimit (0.0f, 1.0f, (body.getBottom() - (float) e.y) / juce::jmax (1.0f, body.getHeight()));
            engine.editSong ([a = c.automation, p] (Song& s) {
                auto& points = s.automations[(std::size_t) a].points;
                points.push_back (p);
                std::stable_sort (points.begin(), points.end(), [] (auto& l, auto& r) { return l.tick < r.tick; });
            });
            point = automationPointAt (hit, e.getPosition());
        }

        if (point >= 0)
        {
            setSelection ({});
            drag = Drag::autoPoint;
            autoClip = hit;
            autoPoint = point;
            return;
        }
    }

    // audio clip fade handles (drawn at the top corners of the body)
    if (hit >= 0)
        if (const auto fade = fadeHandleAt (hit, e.getPosition()); fade != 0)
        {
            setSelection ({});
            dragClip = hit;
            dragIndices = { hit };
            drag = fade == 1 ? Drag::fadeIn : Drag::fadeOut;
            return;
        }

    // Alt+Shift+drag inside a clip: slip its content
    if (hit >= 0 && e.mods.isAltDown() && e.mods.isShiftDown() && ! rightEdge && ! leftEdge)
    {
        grabClips (Drag::slip);
        return;
    }

    // Shift+drag on a clip body drags a copy (on an edge, Shift trims like a plain edge drag)
    if (hit >= 0 && e.mods.isShiftDown() && ! rightEdge && ! leftEdge)
    {
        const auto sources = isSelected (hit) ? selection : std::vector<int> { hit };
        const auto grabbedAt = (std::size_t) (std::find (sources.begin(), sources.end(), hit) - sources.begin());
        std::vector<Clip> copies;
        std::vector<int> added;

        for (auto i : sources)
            copies.push_back (engine.getSong().playlist[(std::size_t) i]);

        // Shift+V held: the copies get their own pattern / automation (FL's "make unique" while cloning)
        const auto unique = juce::KeyPress::isKeyCurrentlyDown ('V') || juce::KeyPress::isKeyCurrentlyDown ('v');

        engine.editSong ([&] (Song& s) {
            for (const auto& c : copies)
            {
                added.push_back ((int) s.playlist.size());
                s.playlist.push_back (c);
            }

            if (unique)
                polish::makeClipsUnique (s, added);
        });

        if (unique)
            for (std::size_t j = 0; j < added.size(); ++j)
                copies[j] = engine.getSong().playlist[(std::size_t) added[j]];
        setSelection (added);
        dragIndices = added;
        dragOriginals = copies;
        dragClip = added[grabbedAt];
        drag = Drag::move;
        grabOffset = copies[grabbedAt].start - snap (xToTick (e.x), snapOn);
        return;
    }

    if (hit >= 0)
    {
        grabClips (rightEdge ? Drag::resize : leftEdge ? Drag::trimLeft : Drag::move);

        if (drag == Drag::resize && ripple)
        {
            setSelection ({}); // a ripple resize works on the grabbed clip only
            dragIndices = { hit };
            dragOriginals = { engine.getSong().playlist[(std::size_t) hit] };
            rippleOriginals = engine.getSong().playlist;
        }

        const auto& c = engine.getSong().playlist[(std::size_t) hit];

        if (c.kind == ClipKind::pattern && c.pattern != engine.getSong().currentPattern)
            engine.editSong ([p = c.pattern] (Song& s) { s.currentPattern = p; }, false);
        return;
    }

    // paints what is chosen in the Picker (the current pattern, an audio clip or an automation clip)
    setSelection ({});
    // an instrument track places its own pattern (FL: named after the channel, made on the first click; round 151)
    if (const auto track = yToTrack (e.y); brush.kind == ClipKind::pattern && track >= 0 && track < (int) engine.getSong().playlistTracks.size()
                                           && engine.getSong().playlistTracks[(std::size_t) track].mode == PlaylistTrackMode::instrument)
    {
        engine.editSong ([track] (Song& s) {
            if (const auto p = trackmodes::instrumentTrackPattern (s, track); p >= 0)
                s.currentPattern = p;
        });
        engine.setSelectedChannel (engine.getSong().playlistTracks[(std::size_t) track].channel); // its piano roll is the channel's
    }

    const auto c = makeBrushClip (yToTrack (e.y), snap (xToTick (e.x), snapOn));

    if (c.kind == ClipKind::pattern)
        brush = {};

    engine.editSong ([c] (Song& s) {
        s.playlist.push_back (c);
        s.mode = PlayMode::song; // someone arranging wants to hear the song
    });
    dragClip = (int) engine.getSong().playlist.size() - 1;
    dragIndices = { dragClip };
    dragOriginals = { c };
    grabOffset = 0;

    if (tool == EditTool::brush)
    {
        drag = Drag::paint;
        paintClip = c;
        nextPaintTick = c.end();
        return;
    }

    drag = Drag::move;
}

void PlaylistComponent::mouseDrag (const juce::MouseEvent& e)
{
    if (drag == Drag::trackResize)
    {
        const auto height = polish::clampTrackHeight (resizeStartHeight + e.getDistanceFromDragStartY());
        std::vector<int> tracks;
        if (resizeAll)
            for (int t = 0; t < numTracks(); ++t)
                tracks.push_back (t);
        else
            tracks.push_back (resizeTrack);

        if (heightOf (resizeTrack) != height)
            setTrackHeights (tracks, height);

        ui::postHint ({ resizeAll ? tr ("Resizing every track") : tr ("Resizing the track"), juce::String (height) + " px", false, false });
        return;
    }

    if (drag == Drag::preview)
        return;

    const auto snapOn = ! e.mods.isAltDown();
    // after the step below, the hint bar says what the gesture is doing
    const struct PostAfter
    {
        PlaylistComponent& list;
        ~PostAfter() { list.postDragHint(); }
    } postAfter { *this };

    switch (drag)
    {
        case Drag::none:
        case Drag::trackResize:
        case Drag::preview:
            return;

        case Drag::pan:
            scrollTicks = juce::jmax (0.0, panStartTicks - e.getDistanceFromDragStartX() / pixelsPerTick);
            scrollY = juce::jmax (0, panStartY - e.getDistanceFromDragStartY());
            updateScrollBars();
            repaint();
            return;

        case Drag::autoPoint:
        {
            const auto& song = engine.getSong();
            if (autoClip < 0 || autoClip >= (int) song.playlist.size())
                return;
            const auto& c = song.playlist[(std::size_t) autoClip];
            if (c.automation < 0 || c.automation >= (int) song.automations.size())
                return;
            const auto& points = song.automations[(std::size_t) c.automation].points;
            if (autoPoint < 0 || autoPoint >= (int) points.size())
                return;

            // the point stays between its neighbours; Shift pressed after the click keeps the time, Ctrl the value
            auto tick = snap (xToTick (e.x), snapOn) - c.start + c.offset;
            const auto lo = autoPoint > 0 ? points[(std::size_t) autoPoint - 1].tick : 0;
            const auto hi = autoPoint + 1 < (int) points.size() ? points[(std::size_t) autoPoint + 1].tick : std::numeric_limits<std::int64_t>::max();
            tick = juce::jlimit (lo, hi, tick);
            const auto body = automationBody (c);
            auto value = juce::jlimit (0.0f, 1.0f, (body.getBottom() - (float) e.y) / juce::jmax (1.0f, body.getHeight()));

            if (e.mods.isShiftDown() && ! downMods.isShiftDown())
                tick = points[(std::size_t) autoPoint].tick;
            if (e.mods.isCtrlDown() && ! downMods.isCtrlDown())
                value = points[(std::size_t) autoPoint].value;

            engine.editSong ([a = c.automation, i = autoPoint, tick, value] (Song& s) {
                auto& p = s.automations[(std::size_t) a].points[(std::size_t) i];
                p.tick = tick;
                p.value = value;
            });
            return;
        }

        case Drag::timeMove:
        {
            const auto here = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn));
            const auto delta = juce::jmax (-timeMoveFrom, here - timeMoveAnchor);
            std::vector<int> moved;
            engine.editSong ([&] (Song& s) {
                s.playlist = timeMoveOriginals;
                moved = noteedit::moveTimeRange (s.playlist, timeMoveFrom, timeMoveTo, delta, timeMoveCopy);
                s.loopStart = timeMoveFrom + delta;
                s.loopEnd = timeMoveTo + delta;
            });
            setSelection (moved);
            return;
        }

        case Drag::seek:
        case Drag::listen:
            seekTo (e.x);
            return;

        case Drag::loop:
        {
            const auto here = juce::jmax<std::int64_t> (0, snap (xToTick (e.x) + (snapOn ? gridTicks() / 2.0 : 0.0), snapOn));
            setTimeSelection (juce::jmin (loopAnchor, here), juce::jmax (loopAnchor, here));
            return;
        }

        case Drag::fadeIn:
        case Drag::fadeOut:
        {
            const auto& clip = engine.getSong().playlist[(std::size_t) dragClip];
            const auto tick = snapOn ? snap (xToTick (e.x) + gridTicks() / 2.0, true) : (std::int64_t) xToTick (e.x);
            auto length = drag == Drag::fadeIn ? tick - clip.start : clip.end() - tick;
            length = juce::jlimit<std::int64_t> (0, clip.length, length);
            const auto in = drag == Drag::fadeIn;

            if ((in ? clip.fadeIn : clip.fadeOut) == std::min (length, clip.length - (in ? clip.fadeOut : clip.fadeIn)))
                return;

            engine.editSong ([i = dragClip, length, in] (Song& s) {
                auto& c = s.playlist[(std::size_t) i];
                (in ? c.fadeIn : c.fadeOut) = std::min (length, c.length - (in ? c.fadeOut : c.fadeIn));
            });
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
            const auto tr0 = yToTrack (rubberBand.getY()), tr1 = yToTrack (rubberBand.getBottom());
            const auto& playlist = engine.getSong().playlist;
            auto s = selectionBefore;

            for (int i = 0; i < (int) playlist.size(); ++i)
            {
                const auto& c = playlist[(std::size_t) i];

                if ((double) c.end() > t0 && (double) c.start < t1 && c.track >= tr0 && c.track <= tr1)
                    s.push_back (i);
            }

            setSelection (s);
            return;
        }

        case Drag::erase:
        {
            // right press-and-hold on empty space, then drag: a temporary Knife
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
                    holdExpired = true;
            }

            bool edge = false;
            const auto hit = clipAt (e.x, e.y, edge);

            if (hit >= 0)
            {
                erasedSomething = true;
                engine.editSong ([hit] (Song& s) { s.playlist.erase (s.playlist.begin() + hit); });
            }
            return;
        }

        case Drag::muting:
        {
            bool edge = false;
            const auto hit = clipAt (e.x, e.y, edge);

            if (hit >= 0 && std::find (touchedClips.begin(), touchedClips.end(), hit) == touchedClips.end())
            {
                touchedClips.push_back (hit);
                engine.editSong ([hit, m = muteTarget] (Song& s) { s.playlist[(std::size_t) hit].muted = m; });
            }
            return;
        }

        case Drag::knife:
            knifeTo = e.getPosition();
            repaint();
            return;

        case Drag::paint:
        {
            // copies of the painted clip, end to end, on the track under the mouse
            const auto tick = snap (xToTick (e.x), snapOn);
            const auto length = juce::jmax<std::int64_t> (1, paintClip.length);

            for (int guard = 0; tick >= nextPaintTick && guard < 64; ++guard)
            {
                auto c = paintClip;
                c.start = nextPaintTick;
                c.track = yToTrack (e.y);
                engine.editSong ([c] (Song& s) { s.playlist.push_back (c); });
                nextPaintTick += length;
            }
            return;
        }

        case Drag::slip:
        {
            // the content moves by the (snapped) mouse movement; the clips stay put
            const auto delta = snap (xToTick (e.x), snapOn) - snap (xToTick (e.getMouseDownX()), snapOn);
            const auto grabbedAt = (std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragClip) - dragIndices.begin());
            auto probe = dragOriginals[grabbedAt];
            noteedit::slip (probe, delta);

            if (engine.getSong().playlist[(std::size_t) dragClip].offset == probe.offset)
                return;

            engine.editSong ([this, delta] (Song& s) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                {
                    auto c = dragOriginals[j];
                    noteedit::slip (c, delta);
                    s.playlist[(std::size_t) dragIndices[j]].offset = c.offset;
                }
            });
            return;
        }

        case Drag::move:
        {
            const auto grabbedAt = (std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragClip) - dragIndices.begin());
            const auto& grabbed = dragOriginals[grabbedAt];
            auto deltaTicks = juce::jmax<std::int64_t> (0, snap (xToTick (e.x), snapOn) + grabOffset) - grabbed.start;
            auto deltaTracks = yToTrack (e.y) - grabbed.track;

            // the group keeps its shape: clamp by the leftmost / topmost clip
            for (const auto& c : dragOriginals)
            {
                deltaTicks = juce::jmax (deltaTicks, -c.start);
                deltaTracks = juce::jlimit (-c.track, 499 - c.track, deltaTracks);
            }

            const auto& current = engine.getSong().playlist[(std::size_t) dragClip];

            if (current.start == grabbed.start + deltaTicks && current.track == grabbed.track + deltaTracks)
                return;

            engine.editSong ([this, deltaTicks, deltaTracks] (Song& s) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                {
                    auto& c = s.playlist[(std::size_t) dragIndices[j]];
                    c.start = dragOriginals[j].start + deltaTicks;
                    c.track = dragOriginals[j].track + deltaTracks;
                }
            });
            return;
        }

        case Drag::resize:
        {
            const auto grabbedAt = (std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragClip) - dragIndices.begin());
            const auto& grabbed = dragOriginals[grabbedAt];
            const auto minimum = snapOn ? juce::jmax<std::int64_t> (10, gridTicks()) : (std::int64_t) 10;
            const auto end = snapOn ? snap (xToTick (e.x) + gridTicks() / 2.0, true) : (std::int64_t) xToTick (e.x);
            const auto delta = juce::jmax (minimum, end - grabbed.start) - grabbed.length;

            if (engine.getSong().playlist[(std::size_t) dragClip].length == grabbed.length + delta)
                return;

            if (ripple && ! rippleOriginals.empty()) // the later clips on the track follow the end
            {
                engine.editSong ([this, length = juce::jmax (minimum, grabbed.length + delta)] (Song& s) {
                    s.playlist = rippleOriginals;
                    noteedit::rippleResize (s.playlist, dragClip, length);
                });
                return;
            }

            engine.editSong ([this, delta, minimum] (Song& s) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                {
                    auto& c = s.playlist[(std::size_t) dragIndices[j]];
                    c.length = juce::jmax (minimum, dragOriginals[j].length + delta);
                    c.fadeIn = juce::jmin (c.fadeIn, c.length);
                    c.fadeOut = juce::jmin (c.fadeOut, c.length - c.fadeIn);
                }
            });
            return;
        }

        case Drag::trimLeft:
        {
            // the start moves, the content stays where it is (the offset follows)
            const auto grabbedAt = (std::size_t) (std::find (dragIndices.begin(), dragIndices.end(), dragClip) - dragIndices.begin());
            const auto& grabbed = dragOriginals[grabbedAt];
            const auto minimum = snapOn ? juce::jmax<std::int64_t> (10, gridTicks()) : (std::int64_t) 10;
            const auto start = snapOn ? snap (xToTick (e.x) + gridTicks() / 2.0, true) : (std::int64_t) xToTick (e.x);
            const auto delta = start - grabbed.start;
            auto probe = grabbed;
            noteedit::trimStart (probe, grabbed.start + delta, minimum);

            if (engine.getSong().playlist[(std::size_t) dragClip].start == probe.start)
                return;

            engine.editSong ([this, delta, minimum] (Song& s) {
                for (std::size_t j = 0; j < dragIndices.size(); ++j)
                {
                    auto c = dragOriginals[j];
                    noteedit::trimStart (c, c.start + delta, minimum);
                    s.playlist[(std::size_t) dragIndices[j]] = c;
                }
            });
            return;
        }
    }
}

void PlaylistComponent::postDragHint()
{
    const auto& song = engine.getSong();
    const auto count = juce::String ((int) juce::jmax<std::size_t> (1, dragIndices.size()));
    auto position = [&song] (std::int64_t tick) {
        const auto bar = song.barAt (juce::jmax<std::int64_t> (0, tick));
        const auto beat = (tick - bar.tick) / ppq + 1;
        return juce::String (bar.number) + ":" + juce::String (beat);
    };
    const auto clip = dragClip >= 0 && dragClip < (int) song.playlist.size() ? &song.playlist[(std::size_t) dragClip] : nullptr;
    ui::Hint hint;

    switch (drag)
    {
        case Drag::move:
            if (clip == nullptr)
                return;
            hint.text = tr ("Moving {0} clip(s)  [Alt] no snap", { count });
            hint.value = position (clip->start) + "  " + tr ("Track {0}", { juce::String (clip->track + 1) });
            break;
        case Drag::resize:
        case Drag::trimLeft:
            if (clip == nullptr)
                return;
            hint.text = tr ("Resizing {0} clip(s)  [Alt] no snap", { count });
            hint.value = position (drag == Drag::resize ? clip->end() : clip->start);
            break;
        case Drag::slip: hint.text = tr ("Sliding the content inside the clip  [Alt] no snap"); break;
        case Drag::fadeIn:
        case Drag::fadeOut: hint.text = tr ("Setting the fade  [Alt] no snap"); break;
        case Drag::autoPoint: hint.text = tr ("Moving the point: then [Shift] keeps time  [Ctrl] keeps value"); break;
        case Drag::select:
            hint.text = tr ("Selecting  [Shift] adds to the selection");
            hint.value = tr ("{0} selected", { juce::String ((int) selection.size()) });
            break;
        case Drag::loop:
            hint.text = tr ("Time selection");
            hint.value = position (song.loopStart) + " - " + position (song.loopEnd);
            break;
        case Drag::erase: hint.text = tr ("Deleting what the mouse crosses"); break;
        case Drag::muting: hint.text = tr ("Muting / unmuting what the mouse crosses"); break;
        case Drag::knife: hint.text = tr ("Cutting along the line  [Alt] no snap"); break;
        case Drag::paint: hint.text = tr ("Painting clips  [Alt] no snap"); break;
        case Drag::timeMove: hint.text = tr ("Moving the time selection with its clips"); break;
        case Drag::seek:
        case Drag::listen:
        case Drag::pan:
        case Drag::zoomRange:
        case Drag::trackResize:
        case Drag::preview:
        case Drag::none: return;
    }

    ui::postHint (hint);
}

void PlaylistComponent::mouseUp (const juce::MouseEvent& e)
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

    if (drag == Drag::select && rubberBand.isEmpty() && dragClip >= 0)
    {
        auto s = selectionBefore;
        if (auto it = std::find (s.begin(), s.end(), dragClip); it != s.end())
            s.erase (it);
        else
            s.push_back (dragClip);
        setSelection (s);
    }

    // Ctrl+click on the ruler without dragging clears the time selection
    if (drag == Drag::loop && e.getDistanceFromDragStart() < 3)
        setTimeSelection (0, 0);

    if (drag == Drag::listen)
        engine.stop();

    if (drag == Drag::preview)
        stopPreview();

    resizeTrack = -1;

    if (drag == Drag::move || drag == Drag::resize || drag == Drag::trimLeft || drag == Drag::paint || drag == Drag::timeMove)
        sortClips();

    rippleOriginals.clear();
    timeMoveOriginals.clear();
    autoClip = autoPoint = -1;

    drag = Drag::none;
    dragClip = -1;
    dragIndices.clear();
    dragOriginals.clear();
    touchedClips.clear();
    rubberBand = {};
    knownClipCount = engine.getSong().playlist.size();
    hover = {};
    updateHover (e.getPosition(), e.mods); // the cursor of what is under the mouse now
    repaint();
}

void PlaylistComponent::cutWithKnife (juce::Point<int> from, juce::Point<int> to, bool snapOn)
{
    const auto& playlist = engine.getSong().playlist;
    std::vector<std::pair<int, std::int64_t>> cuts;
    const auto round = snapOn ? gridTicks() / 2.0 : 0.0;

    if (from.getDistanceFrom (to) < 3)
    {
        bool edge = false;
        if (const auto hit = clipAt (from.x, from.y, edge); hit >= 0)
            cuts.push_back ({ hit, snap (xToTick (from.x) + round, snapOn) });
    }
    else
    {
        // every track the line crosses is cut where the line passes its middle
        const auto top = juce::jmin (from.y, to.y), bottom = juce::jmax (from.y, to.y);

        for (int i = 0; i < (int) playlist.size(); ++i)
        {
            const auto& c = playlist[(std::size_t) i];
            const auto trackHeight = heightOf (c.track);
            const auto rowY = trackToY (c.track) + trackHeight / 2;

            if (rowY < top - trackHeight / 2 || rowY > bottom + trackHeight / 2)
                continue;

            const auto y = juce::jlimit (top, bottom, rowY);
            const auto x = from.y == to.y ? (double) from.x : from.x + (double) (y - from.y) * (to.x - from.x) / (double) (to.y - from.y);
            const auto tick = snap (xToTick ((int) std::round (x)) + round, snapOn);

            if (tick > c.start && tick < c.end())
                cuts.push_back ({ i, tick });
        }
    }

    if (cuts.empty())
        return;

    setSelection ({});
    engine.editSong ([&cuts] (Song& s) {
        for (auto [index, tick] : cuts)
        {
            auto rest = splitClip (s.playlist[(std::size_t) index], tick);
            s.playlist.push_back (rest);
        }
    });
    sortClips();
}

void PlaylistComponent::zoomToTicks (double from, double to)
{
    if (to - from < 1.0)
        return;

    pixelsPerTick = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), gridArea().getWidth() / (to - from));
    scrollTicks = juce::jmax (0.0, from);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::zoomIn (int anchorX)
{
    const auto anchor = xToTick (anchorX);
    pixelsPerTick = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), pixelsPerTick * 2.0);
    scrollTicks = juce::jmax (0.0, anchor - (anchorX - headerWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::zoomOut (int anchorX)
{
    const auto anchor = xToTick (anchorX);
    pixelsPerTick = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), pixelsPerTick / 2.0);
    scrollTicks = juce::jmax (0.0, anchor - (anchorX - headerWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::insertTimeSelection()
{
    const auto& song = engine.getSong();

    if (! song.hasLoop())
        return;

    setSelection ({});
    engine.editSong ([from = song.loopStart, to = song.loopEnd] (Song& s) { noteedit::insertTime (s.playlist, s.markers(), from, to - from); });
    sortClips();
}

void PlaylistComponent::deleteTimeSelection()
{
    const auto& song = engine.getSong();

    if (! song.hasLoop())
        return;

    setSelection ({});
    engine.editSong ([from = song.loopStart, to = song.loopEnd] (Song& s) { noteedit::deleteTime (s.playlist, s.markers(), from, to); });
    sortClips();
}

juce::Rectangle<int> PlaylistComponent::trackLed (int track) const
{
    const auto h = heightOf (track);
    return { headerWidth - 17, trackToY (track) + h / 2 - 6, 12, 12 };
}

juce::Rectangle<int> PlaylistComponent::trackArrow (int track) const
{
    if (! noteedit::isGroupHeader (engine.getSong(), track))
        return {};

    const auto led = trackLed (track);
    return { led.getX() - 15, led.getY() - 1, 13, led.getHeight() + 2 };
}

void PlaylistComponent::editTrack (int track, std::function<void (PlaylistTrack&)> change)
{
    if (track < 0)
        return;

    engine.editSong ([track, change] (Song& s) {
        if ((int) s.playlistTracks.size() <= track)
            s.playlistTracks.resize ((std::size_t) track + 1);
        change (s.playlistTracks[(std::size_t) track]);
    });
}

// The track menu in FL's order (REA round 104): name and colour, size, grouping, its clips, insert / clone /
// delete, move; track modes and Consolidate since round 146a. FL items LYNX has no counterpart for (icons, performance
// settings, ghost notes) are left out.
void PlaylistComponent::showTrackMenu (int track)
{
    buildTrackMenu (track).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::PopupMenu PlaylistComponent::buildTrackMenu (int track)
{
    const auto& song = engine.getSong();
    const auto count = numTracks();
    const auto info = track < (int) song.playlistTracks.size() ? song.playlistTracks[(std::size_t) track] : PlaylistTrack {};
    juce::PopupMenu menu;
    ui::addItemHeader (menu, tr ("Playlist track"), ! info.name.empty() ? juce::String::fromUTF8 (info.name.c_str()) : tr ("Track {0}", { juce::String (track + 1) }),
                       info.colour != 0 ? juce::Colour (info.colour) : juce::Colour());
    menu.addItem (tr ("Rename and color..."), [this, track] { renameTrack (track); });
    menu.addSubMenu (tr ("Change color"), ui::colourMenu (info.colour, [this, track] (std::uint32_t colour) {
                         editTrack (track, [colour] (PlaylistTrack& t) { t.colour = colour; });
                     }, juce::Rectangle<int> (juce::Desktop::getMousePosition().x, juce::Desktop::getMousePosition().y, 1, 1)));
    menu.addItem (tr ("Random color"), [this, track] {
        const auto& palette = lynxPalette();
        const auto c = palette[(std::size_t) juce::Random::getSystemRandom().nextInt ((int) palette.size())];
        editTrack (track, [c] (PlaylistTrack& t) { t.colour = c; });
    });
    menu.addItem (tr ("Auto name"), [this, track] {
        // the name of the source of the track's first clip (pattern, audio or automation channel)
        const auto& s = engine.getSong();
        const Clip* first = nullptr;
        for (const auto& c : s.playlist)
            if (c.track == track && (first == nullptr || c.start < first->start))
                first = &c;
        if (first == nullptr)
            return;
        std::string name;
        if (first->kind == ClipKind::pattern && first->pattern >= 0 && first->pattern < (int) s.patterns.size())
            name = s.patterns[(std::size_t) first->pattern].name;
        else if (first->kind == ClipKind::audio && first->channel >= 0 && first->channel < (int) s.channels.size())
            name = s.channels[(std::size_t) first->channel].name;
        else if (first->kind == ClipKind::automation && first->automation >= 0 && first->automation < (int) s.automations.size())
            name = s.automations[(std::size_t) first->automation].name;
        if (! name.empty())
            editTrack (track, [name] (PlaylistTrack& t) { t.name = name; });
    });
    menu.addItem (tr ("Reset"), [this, track] {
        editTrack (track, [] (PlaylistTrack& t) {
            t.name.clear();
            t.colour = 0;
            t.height = 0;
            t.muted = false;
        });
    });
    menu.addSeparator();
    {
        // FL's "Track mode" (round 146a): unassigned, an audio track (linked to a free mixer track) or an instrument
        // track (a channel and its mixer track)
        juce::PopupMenu modes, instruments;
        modes.addItem (tr ("Unassigned"), true, info.mode == PlaylistTrackMode::unassigned,
                       [this, track] { engine.editSong ([track] (Song& s) { trackmodes::unassign (s, track); }); });
        modes.addItem (tr ("Audio track"), true, info.mode == PlaylistTrackMode::audio, [this, track] {
            bool ok = false;
            engine.editSong ([track, &ok] (Song& s) { ok = trackmodes::makeAudioTrack (s, track); });
            if (! ok)
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Can't create new audio track"),
                                                        tr ("There are no available mixer tracks."));
        });
        for (int c = 0; c < (int) song.channels.size(); ++c)
        {
            const auto kind = song.channels[(std::size_t) c].instrument;
            if (kind == InstrumentKind::audioClip || kind == InstrumentKind::controller)
                continue;
            instruments.addItem (juce::String::fromUTF8 (song.channels[(std::size_t) c].name.c_str()), true,
                                 info.mode == PlaylistTrackMode::instrument && info.channel == c, [this, track, c] {
                                     bool ok = false;
                                     engine.editSong ([track, c, &ok] (Song& s) { ok = trackmodes::makeInstrumentTrack (s, track, c); });
                                     if (! ok)
                                         juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Can't create new instrument track"),
                                                                                 tr ("There are no available mixer tracks."));
                                 });
        }
        modes.addSubMenu (tr ("Instrument track"), instruments, instruments.getNumItems() > 0, nullptr, info.mode == PlaylistTrackMode::instrument);
        menu.addSubMenu (tr ("Track mode"), modes);
    }
    menu.addSeparator();
    {
        // FL's sizes are relative to the default row; dragging the divider under a header sets any size (P18)
        juce::PopupMenu size, all;
        const auto current = heightOf (track);

        for (int percent : { 33, 50, 100, 150, 200 })
        {
            const auto h = polish::clampTrackHeight ((int) std::lround (trackHeight * percent / 100.0));
            size.addItem (juce::String (percent) + "%", true, current == h, [this, track, h] { setTrackHeights ({ track }, h); });
            all.addItem (juce::String (percent) + "%", [this, count, h] {
                std::vector<int> tracks;
                for (int t = 0; t < count; ++t)
                    tracks.push_back (t);
                setTrackHeights (tracks, h);
            });
        }
        size.addSeparator();
        size.addSubMenu (tr ("Every track"), all);
        menu.addSubMenu (tr ("Size"), size);
    }
    menu.addSeparator();
    {
        const auto header = noteedit::groupHeaderOf (song, track);
        const auto member = info.grouped && track > 0;
        const auto folded = header >= 0 && header < (int) song.playlistTracks.size() && song.playlistTracks[(std::size_t) header].collapsed;
        menu.addItem (tr ("Group with above track"), track > 0 && ! member, member, [this, track] {
            engine.editSong ([track] (Song& s) { noteedit::groupWithAbove (s, track); });
        });
        menu.addItem (tr ("Ungroup"), member, false, [this, track] { engine.editSong ([track] (Song& s) { noteedit::ungroupTrack (s, track); }); });
        menu.addItem (tr ("Auto color group"), header >= 0, false, [this, track] { engine.editSong ([track] (Song& s) { noteedit::colourGroup (s, track); }); });
        menu.addItem (tr ("Collapse grouped tracks"), header >= 0, folded, [this, track, folded] {
            engine.editSong ([track, folded] (Song& s) { noteedit::setGroupCollapsed (s, track, ! folded); });
        });
    }
    menu.addSeparator();
    {
        // the sources of the track's clips: choosing one makes it what the pencil paints (FL's "Current clip source")
        juce::PopupMenu sources;
        std::vector<std::pair<ClipKind, int>> seen;
        for (const auto& c : song.playlist)
        {
            if (c.track != track)
                continue;
            const auto key = std::pair { c.kind, c.kind == ClipKind::pattern ? c.pattern : c.kind == ClipKind::audio ? c.channel : c.automation };
            if (std::find (seen.begin(), seen.end(), key) != seen.end())
                continue;
            seen.push_back (key);
            juce::String name;
            if (key.first == ClipKind::pattern && key.second >= 0 && key.second < (int) song.patterns.size())
                name = juce::String::fromUTF8 (song.patterns[(std::size_t) key.second].name.c_str());
            else if (key.first == ClipKind::automation && key.second >= 0 && key.second < (int) song.automations.size())
                name = juce::String::fromUTF8 (song.automations[(std::size_t) key.second].name.c_str());
            else if (key.second >= 0 && key.second < (int) song.channels.size())
                name = juce::String::fromUTF8 (song.channels[(std::size_t) key.second].name.c_str());
            const auto ticked = brush.kind == key.first && (key.first == ClipKind::pattern ? song.currentPattern == key.second : brush.index == key.second);
            sources.addItem (name, true, ticked, [this, key] {
                brush.kind = key.first;
                brush.index = key.first == ClipKind::pattern ? -1 : key.second;
                if (key.first == ClipKind::pattern)
                    engine.editSong ([p = key.second] (Song& s) { s.currentPattern = p; }, false);
                engine.sendChangeMessage();
            });
        }
        menu.addSubMenu (tr ("Current clip source"), sources, ! seen.empty());
        auto muteAll = [this, track] (bool mute) {
            engine.editSong ([track, mute] (Song& s) {
                for (auto& c : s.playlist)
                    if (c.track == track)
                        c.muted = mute;
            });
        };
        const auto hasClips = ! seen.empty();
        {
            // FL's "Consolidate track(s)" (round 146a): the track's clips rendered into one audio clip
            juce::PopupMenu consolidate;
            consolidate.addItem (tr ("From track start"), hasClips, false, [this, track] { consolidatePlaylistTrack (engine, track, false); });
            consolidate.addItem (tr ("From song start"), hasClips, false, [this, track] { consolidatePlaylistTrack (engine, track, true); });
            menu.addSubMenu (tr ("Consolidate track"), consolidate, hasClips);
        }
        menu.addItem (tr ("Merge clips"), hasClips, false, [this, track] {
            setSelection ({});
            engine.editSong ([track] (Song& s) { clipops::mergeTrackClips (s, track); });
            sortClips();
        });
        menu.addItem (tr ("Mute all clips"), hasClips, false, [muteAll] { muteAll (true); });
        menu.addItem (tr ("Unmute all clips"), hasClips, false, [muteAll] { muteAll (false); });
    }
    menu.addSeparator();
    menu.addItem (tr ("Insert one"), [this, track] {
        setSelection ({});
        engine.editSong ([track] (Song& s) { noteedit::insertTrack (s, track); });
    });
    menu.addItem (tr ("Clone"), [this, track] {
        // a copy of the track (name, colour, size, group) and its clips, right below it
        setSelection ({});
        engine.editSong ([track] (Song& s) {
            noteedit::insertTrack (s, track + 1);
            if (track < (int) s.playlistTracks.size())
            {
                auto copy = s.playlistTracks[(std::size_t) track];
                copy.collapsed = false;
                if (noteedit::isGroupHeader (s, track) || copy.grouped)
                    copy.grouped = true; // stays in the same group
                s.playlistTracks[(std::size_t) track + 1] = copy;
            }
            std::vector<Clip> copies;
            for (const auto& c : s.playlist)
                if (c.track == track)
                {
                    copies.push_back (c);
                    copies.back().track = track + 1;
                }
            s.playlist.insert (s.playlist.end(), copies.begin(), copies.end());
        });
        sortClips();
    });
    menu.addItem (tr ("Delete"), [this, track] {
        setSelection ({});
        engine.editSong ([track] (Song& s) { noteedit::removeTrack (s, track); });
    });
    menu.addSeparator();
    menu.addItem (tr ("Move up"), track > 0, false, [this, track] { engine.editSong ([track] (Song& s) { noteedit::swapTracks (s, track, track - 1); }); });
    menu.addItem (tr ("Move down"), track + 1 < count, false, [this, track] { engine.editSong ([track] (Song& s) { noteedit::swapTracks (s, track, track + 1); }); });
    return menu;
}

void PlaylistComponent::renameTrack (int track)
{
    const auto& tracks = engine.getSong().playlistTracks;
    const auto current = track < (int) tracks.size() ? juce::String::fromUTF8 (tracks[(std::size_t) track].name.c_str()) : juce::String();
    const auto colour = track < (int) tracks.size() ? tracks[(std::size_t) track].colour : 0u;
    showNameAndColourDialog (tr ("Rename track"), current.isNotEmpty() ? current : tr ("Track {0}", { juce::String (track + 1) }), colour,
                             [this, track] (const juce::String& newName, std::uint32_t newColour) {
                                 const auto name = newName.trim().toStdString();
                                 engine.editSong ([track, name, newColour] (Song& s) {
                                     if ((int) s.playlistTracks.size() <= track)
                                         s.playlistTracks.resize ((std::size_t) track + 1);
                                     s.playlistTracks[(std::size_t) track].name = name;
                                     s.playlistTracks[(std::size_t) track].colour = newColour;
                                 });
                             });
}

void PlaylistComponent::showRulerMenu (int x)
{
    const auto& song = engine.getSong();
    const auto tick = juce::jmax<std::int64_t> (0, snap (xToTick (x), true));

    // marker near the click (up to 6 px)
    int near = -1;
    for (int i = 0; i < (int) song.markers().size(); ++i)
        if (std::abs (tickToX ((double) song.markers()[(std::size_t) i].tick) - x) <= 6.0)
            near = i;

    juce::PopupMenu menu;
    menu.addItem (tr ("Add marker here..."), [this, tick] { editMarkerName (tick, -1); });

    juce::PopupMenu signatures;
    for (auto [n, d] : { std::pair { 2, 4 }, { 3, 4 }, { 4, 4 }, { 5, 4 }, { 6, 4 }, { 7, 4 }, { 6, 8 }, { 7, 8 }, { 9, 8 }, { 12, 8 } })
        signatures.addItem (juce::String (n) + "/" + juce::String (d), [this, tick, n = n, d = d] {
            engine.editSong ([tick, n, d] (Song& s) { s.addMarker ({ tick, {}, n, d }); });
        });
    menu.addSubMenu (tr ("Time signature change here"), signatures);
    // FL's marker actions "Start recording" / "Stop recording" (punch in / out; Record menu › Enable recording markers)
    menu.addItem (tr ("Add a Start recording marker here"), [this, tick] {
        engine.editSong ([tick] (Song& s) {
            Marker m;
            m.tick = tick;
            m.name = tr ("Start recording").toStdString();
            m.action = Marker::punchIn;
            s.addMarker (m);
        });
    });
    menu.addItem (tr ("Add a Stop recording marker here"), [this, tick] {
        engine.editSong ([tick] (Song& s) {
            Marker m;
            m.tick = tick;
            m.name = tr ("Stop recording").toStdString();
            m.action = Marker::punchOut;
            s.addMarker (m);
        });
    });

    if (near >= 0)
    {
        menu.addSeparator();
        menu.addItem (tr ("Rename marker..."), [this, near] { editMarkerName (0, near); });
        if (! song.markers()[(std::size_t) near].isSignature())
        {
            juce::PopupMenu action;
            const auto current = song.markers()[(std::size_t) near].action;
            for (auto [value, text] : { std::pair { 0, TR_NOOP ("None (plain marker)") }, { Marker::punchIn, TR_NOOP ("Start recording") },
                                        { Marker::punchOut, TR_NOOP ("Stop recording") } })
                action.addItem (tr (text), true, current == value, [this, near, value = value] {
                    engine.editSong ([near, value] (Song& s) {
                        if (near < (int) s.markers().size())
                            s.markers()[(std::size_t) near].action = value;
                    });
                });
            menu.addSubMenu (tr ("Marker action"), action);
        }
        menu.addItem (tr ("Remove marker"), [this, near] {
            engine.editSong ([near] (Song& s) {
                if (near < (int) s.markers().size())
                    s.markers().erase (s.markers().begin() + near);
            });
        });
    }

    if (song.hasLoop())
    {
        menu.addSeparator();
        menu.addItem (tr ("Remove loop"), [this] { engine.editSong ([] (Song& s) { s.loopStart = s.loopEnd = 0; }, false); });
    }

    menu.addSeparator();
    {
        // FL's Select submenu (REA round 153)
        juce::PopupMenu select;
        const auto overlapping = clipops::overlappingClips (song), stacked = clipops::stackedClips (song), muted = clipops::mutedClips (song);
        select.addItem (tr ("Overlapping clips ({0})", { juce::String ((int) overlapping.size()) }), ! overlapping.empty(), false,
                        [this, overlapping] { setSelection (overlapping); });
        select.addItem (tr ("Stacked clips: hidden duplicates ({0})", { juce::String ((int) stacked.size()) }), ! stacked.empty(), false,
                        [this, stacked] { setSelection (stacked); });
        select.addItem (tr ("Muted clips ({0})", { juce::String ((int) muted.size()) }), ! muted.empty(), false, [this, muted] { setSelection (muted); });
        menu.addSubMenu (tr ("Select"), select);
    }
    menu.addItem (tr ("Ripple editing (resizing and deleting move the later clips)") + "\tShift+S", true, ripple, [this] { setRipple (! ripple); });
    {
        juce::PopupMenu zoom;
        zoom.addItem (tr ("Zoom in") + "\tPageUp", [this] { runAction ("playlist.zoomIn"); });
        zoom.addItem (tr ("Zoom out") + "\tPageDown", [this] { runAction ("playlist.zoomOut"); });
        zoom.addItem (tr ("Zoom: one bar") + "\tShift+1", [this] { runAction ("playlist.zoom1"); });
        zoom.addItem (tr ("Zoom: four bars") + "\tShift+2", [this] { runAction ("playlist.zoom2"); });
        zoom.addItem (tr ("Zoom: sixteen bars") + "\tShift+3", [this] { runAction ("playlist.zoom3"); });
        zoom.addItem (tr ("Zoom to show everything") + "\tShift+4", [this] { runAction ("playlist.zoomAll"); });
        zoom.addItem (tr ("Zoom to the selection") + "\tShift+5", [this] { runAction ("playlist.zoomSelection"); });
        zoom.addItem (tr ("Centre the view on the playhead") + "\tShift+0", [this] { runAction ("playlist.centrePlayhead"); });
        menu.addSubMenu (tr ("Zoom"), zoom);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

void PlaylistComponent::editMarkerName (std::int64_t tick, int existing)
{
    const auto& markers = engine.getSong().markers();
    const auto current = existing >= 0 && existing < (int) markers.size() ? juce::String::fromUTF8 (markers[(std::size_t) existing].name.c_str())
                                                                          : tr ("Marker");
    auto* window = new juce::AlertWindow (existing >= 0 ? tr ("Rename marker") : tr ("New marker"), {}, juce::MessageBoxIconType::NoIcon);
    window->addTextEditor ("name", current);
    window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, tick, existing, window] (int result) {
                                 if (result != 1)
                                     return;

                                 const auto name = window->getTextEditorContents ("name").trim().toStdString();
                                 engine.editSong ([tick, existing, name] (Song& s) {
                                     if (existing < 0)
                                         s.addMarker ({ tick, name, 0, 4 });
                                     else if (existing < (int) s.markers().size())
                                         s.markers()[(std::size_t) existing].name = name;
                                 });
                             }),
                             true);
}

void PlaylistComponent::showClipMenu (int index)
{
    auto menu = buildClipMenu (index, getMouseXYRelative().x);
    if (menu.getNumItems() > 0)
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
}

juce::PopupMenu PlaylistComponent::getClipMenuForSnapshot (int index)
{
    if (index < 0 || index >= (int) engine.getSong().playlist.size())
        return {};
    const auto& c = engine.getSong().playlist[(std::size_t) index];
    return buildClipMenu (index, (int) tickToX ((double) c.start + (double) c.length / 2.0));
}

juce::PopupMenu PlaylistComponent::buildClipMenu (int index, int mouseX)
{
    const auto& song = engine.getSong();
    juce::PopupMenu menu;
    if (index < 0 || index >= (int) song.playlist.size())
        return menu;

    const auto clip = song.playlist[(std::size_t) index];
    const auto targets = isSelected (index) ? selection : std::vector<int> { index };
    const auto many = targets.size() > 1;

    // --- the source: name, colour, uniqueness -------------------------------------------------------
    juce::String sourceName;
    if (clip.kind == ClipKind::pattern && clip.pattern >= 0 && clip.pattern < (int) song.patterns.size())
        sourceName = juce::String::fromUTF8 (song.patterns[(std::size_t) clip.pattern].name.c_str());
    else if (clip.kind == ClipKind::audio && clip.channel >= 0 && clip.channel < (int) song.channels.size())
        sourceName = juce::String::fromUTF8 (song.channels[(std::size_t) clip.channel].name.c_str());
    else if (clip.kind == ClipKind::automation && clip.automation >= 0 && clip.automation < (int) song.automations.size())
        sourceName = juce::String::fromUTF8 (song.automations[(std::size_t) clip.automation].name.c_str());
    {
        // the title band in the clip's colour
        auto colour = theme::colour::automation;
        auto kind = tr ("Automation clip");
        if (clip.kind == ClipKind::pattern)
        {
            colour = patternColour (song, clip.pattern);
            kind = tr ("Pattern clip");
        }
        else if (clip.kind == ClipKind::audio && clip.channel >= 0 && clip.channel < (int) song.channels.size())
        {
            colour = audioClipColour (song.channels[(std::size_t) clip.channel]);
            kind = tr ("Audio clip");
        }
        ui::addItemHeader (menu, many ? kind + " (" + juce::String ((int) targets.size()) + ")" : kind, sourceName, colour);
    }
    menu.addItem (tr ("Properties..."), [this, index, targets] {
        showClipProperties (engine, index, targets, [safe = juce::Component::SafePointer<PlaylistComponent> (this)] {
            if (safe != nullptr)
                safe->sortClips();
        });
    });

    if (clip.kind == ClipKind::pattern && onRenamePattern)
        menu.addItem (tr ("Rename and color..."), [this, p = clip.pattern] { onRenamePattern (p); });
    else if (clip.kind == ClipKind::audio)
        menu.addItem (tr ("Rename and color..."), [this, ch = clip.channel] {
            const auto& c = engine.getSong().channels[(std::size_t) ch];
            showNameAndColourDialog (tr ("Rename audio clip"), juce::String::fromUTF8 (c.name.c_str()), c.colour,
                                     [this, ch] (const juce::String& name, std::uint32_t colour) {
                                         if (ch < (int) engine.getSong().channels.size())
                                             engine.editSong ([ch, n = name.toStdString(), colour] (Song& s) {
                                                 s.channels[(std::size_t) ch].name = n;
                                                 s.channels[(std::size_t) ch].colour = colour;
                                             });
                                     });
        });

    // FL's "Change color..." / "Random color" (the clip's source: its pattern, or its audio channel)
    if (clip.kind != ClipKind::automation)
    {
        auto setColour = [this, kind = clip.kind, src = clip.kind == ClipKind::pattern ? clip.pattern : clip.channel] (std::uint32_t colour) {
            engine.editSong ([kind, src, colour] (Song& s) {
                if (kind == ClipKind::pattern && src >= 0 && src < (int) s.patterns.size())
                    s.patterns[(std::size_t) src].colour = colour;
                else if (kind == ClipKind::audio && src >= 0 && src < (int) s.channels.size())
                    s.channels[(std::size_t) src].colour = colour;
            });
        };
        const auto current = clip.kind == ClipKind::pattern ? (clip.pattern >= 0 && clip.pattern < (int) song.patterns.size() ? song.patterns[(std::size_t) clip.pattern].colour : 0u)
                                                            : (clip.channel >= 0 && clip.channel < (int) song.channels.size() ? song.channels[(std::size_t) clip.channel].colour : 0u);
        menu.addSubMenu (tr ("Change color"), ui::colourMenu (current, setColour, juce::Rectangle<int> (juce::Desktop::getMousePosition().x, juce::Desktop::getMousePosition().y, 1, 1)));
        menu.addItem (tr ("Random color"), [setColour] {
            const auto& palette = lynxPalette();
            setColour (palette[(std::size_t) juce::Random::getSystemRandom().nextInt ((int) palette.size())]);
        });
    }
    menu.addSeparator();

    // FL's "Select source pattern / channel": the clip plays another source (patterns; audio clip channels; automations)
    {
        juce::PopupMenu sources;
        if (clip.kind == ClipKind::pattern)
            for (int p = 0; p < (int) song.patterns.size(); ++p)
                sources.addItem (juce::String::fromUTF8 (song.patterns[(std::size_t) p].name.c_str()), true, p == clip.pattern,
                                 [this, targets, p] { engine.editSong ([targets, p] (Song& s) { clipops::setClipSource (s, targets, ClipKind::pattern, p); }); });
        else if (clip.kind == ClipKind::audio)
        {
            for (int ch = 0; ch < (int) song.channels.size(); ++ch)
                if (song.channels[(std::size_t) ch].instrument == InstrumentKind::audioClip)
                    sources.addItem (juce::String::fromUTF8 (song.channels[(std::size_t) ch].name.c_str()), true, ch == clip.channel,
                                     [this, targets, ch] { engine.editSong ([targets, ch] (Song& s) { clipops::setClipSource (s, targets, ClipKind::audio, ch); }); });
        }
        else
            for (int a = 0; a < (int) song.automations.size(); ++a)
                sources.addItem (juce::String::fromUTF8 (song.automations[(std::size_t) a].name.c_str()), true, a == clip.automation,
                                 [this, targets, a] { engine.editSong ([targets, a] (Song& s) { clipops::setClipSource (s, targets, ClipKind::automation, a); }); });
        menu.addSubMenu (clip.kind == ClipKind::pattern ? tr ("Select source pattern") : tr ("Select source channel"), sources, sources.getNumItems() > 1);
    }
    if (clip.kind == ClipKind::audio && clip.channel >= 0 && clip.channel < (int) song.channels.size())
        menu.addItem (tr ("Channel settings"), [this, ch = clip.channel] { openChannelSettings (engine, ch); });

    if (clip.kind == ClipKind::pattern)
        menu.addItem (tr ("Make unique") + "\tShift+V+drag", [this, targets] {
            engine.editSong ([targets] (Song& s) { polish::makeClipsUnique (s, targets); });
        });
    else if (clip.kind == ClipKind::audio)
        menu.addItem (tr ("Make unique"), [this, targets] {
            engine.editSong ([targets] (Song& s) { clipops::makeAudioUnique (s, targets); });
        });
    else if (clip.kind == ClipKind::automation)
        menu.addItem (tr ("Make unique"), [this, index] {
            engine.editSong ([index] (Song& s) {
                auto& c = s.playlist[(std::size_t) index];
                auto copy = s.automations[(std::size_t) c.automation];
                copy.name += " #2";
                s.automations.push_back (std::move (copy));
                c.automation = (int) s.automations.size() - 1;
            });
        });

    menu.addItem (tr ("Select all clips of this source"), [this, index] { setSelection (clipops::sameSource (engine.getSong(), index)); });
    menu.addSeparator();

    // --- cutting and joining -------------------------------------------------------------------------
    const auto splitAt = snap (xToTick (mouseX) + gridTicks() / 2.0, true);
    menu.addItem (tr ("Split here"), splitAt > clip.start && splitAt < clip.end(), false, [this, index, splitAt] {
        setSelection ({});
        engine.editSong ([index, splitAt] (Song& s) {
            if (index < (int) s.playlist.size() && splitAt > s.playlist[(std::size_t) index].start && splitAt < s.playlist[(std::size_t) index].end())
                s.playlist.push_back (splitClip (s.playlist[(std::size_t) index], splitAt));
        });
        sortClips();
    });
    {
        // FL's "Chop" submenu: to the grid, or in bars / beats / parts of a beat
        juce::PopupMenu chop;
        auto chopBy = [this, targets] (std::int64_t grid) {
            setSelection ({});
            engine.editSong ([targets, grid] (Song& s) {
                for (auto i : targets) // chopping adds clips at the end: indices below stay valid
                    noteedit::chopClip (s.playlist, i, grid);
            });
            sortClips();
        };
        chop.addItem (tr ("Chop to grid"), [chopBy, grid = gridTicks()] { chopBy (grid); });
        chop.addSeparator();
        const auto bar = (std::int64_t) ppq * juce::jmax (1, song.beatsPerBar);
        const std::pair<const char*, std::int64_t> sizes[] = { { TR_NOOP ("Chop in bars"), bar },
                                                               { TR_NOOP ("Chop in beats"), (std::int64_t) ppq },
                                                               { TR_NOOP ("Chop in half beats"), (std::int64_t) ppq / 2 },
                                                               { TR_NOOP ("Chop in quarter beats"), (std::int64_t) ppq / 4 },
                                                               { TR_NOOP ("Chop in 8th beats"), (std::int64_t) ppq / 8 } };
        for (const auto& [label, ticks] : sizes)
            chop.addItem (tr (label), [chopBy, t = ticks] { chopBy (t); });
        menu.addSubMenu (tr ("Chop"), chop);
    }
    menu.addItem (tr ("Merge clips"), clipops::canMerge (song, targets), false, [this, targets] {
        setSelection ({});
        engine.editSong ([targets] (Song& s) { clipops::mergeClips (s, targets); });
        sortClips();
    });

    // --- audio ---------------------------------------------------------------------------------------
    if (clip.kind == ClipKind::audio && clip.channel >= 0 && clip.channel < (int) song.channels.size())
    {
        menu.addSeparator();
        const auto& channel = song.channels[(std::size_t) clip.channel];
        const auto path = juce::String::fromUTF8 (channel.samplePath.c_str());
        const auto sample = engine.getSampleLibrary().load (path);
        const auto frames = sample != nullptr ? (double) sample->frames() : 0.0;
        const auto rate = sample != nullptr ? sample->sampleRate : 44100.0;

        menu.addItem (tr ("Reverse"), sample != nullptr, channel.sampler.reverse, [this, index, frames, rate] {
            engine.editSong ([index, frames, rate] (Song& s) {
                const auto ch = s.playlist[(std::size_t) index].channel;
                clipops::reverseAudio (s, index, clipops::fileTicks (s.channels[(std::size_t) ch].sampler, frames, rate, s.bpm));
            });
        });

        menu.addItem (tr ("Normalize"), [this, targets] { normalizeClips (targets); });

        juce::PopupMenu gains;
        for (auto db : { 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f })
        {
            const auto g = juce::Decibels::decibelsToGain (db);
            gains.addItem ((db > 0 ? "+" : "") + juce::String (db, 0) + " dB", true, std::abs (clip.gain - g) < 0.01f, [this, targets, g] {
                engine.editSong ([targets, g] (Song& s) {
                    for (auto i : targets)
                        if (i < (int) s.playlist.size() && s.playlist[(std::size_t) i].kind == ClipKind::audio)
                            s.playlist[(std::size_t) i].gain = g;
                });
            });
        }
        menu.addSubMenu (tr ("Clip volume"), gains);

        // stretching the channel's file to a whole number of beats at the project tempo
        juce::PopupMenu fit;
        const auto seconds = frames / juce::jmax (1.0, rate);
        const auto beats = clipops::fitBeats (seconds, song.bpm);
        auto fitTo = [this, index, frames, rate] (float toBeats, bool keepPitch) {
            engine.editSong ([index, frames, rate, toBeats, keepPitch] (Song& s) {
                const auto ch = s.playlist[(std::size_t) index].channel;
                auto& sampler = s.channels[(std::size_t) ch].sampler;
                const auto before = clipops::fileTicks (sampler, frames, rate, s.bpm);
                sampler.stretchBeats = toBeats;
                sampler.keepPitch = keepPitch && toBeats > 0.0f;
                const auto after = clipops::fileTicks (sampler, frames, rate, s.bpm);
                if (before <= 0 || after <= 0)
                    return;
                // every clip of the file keeps showing the same audio: offsets and lengths scale with it
                const auto k = (double) after / (double) before;
                for (auto& c : s.playlist)
                    if (c.kind == ClipKind::audio && c.channel == ch)
                    {
                        const auto exact = ((double) c.offset + (double) c.offsetFraction) * k;
                        c.offset = (std::int64_t) std::llround (exact);
                        c.offsetFraction = (float) (exact - (double) c.offset);
                        c.length = juce::jmax<std::int64_t> (1, (std::int64_t) std::llround ((double) c.length * k));
                        c.fadeIn = (std::int64_t) std::llround ((double) c.fadeIn * k);
                        c.fadeOut = (std::int64_t) std::llround ((double) c.fadeOut * k);
                    }
            });
            sortClips();
        };
        if (beats > 0.0f)
        {
            const auto label = juce::String (beats, 0) + " " + (beats == 1.0f ? tr ("beat") : tr ("beats"));
            fit.addItem (tr ("Fit to tempo, keep pitch ({0})", { label }), true, channel.sampler.stretchBeats == beats && channel.sampler.keepPitch,
                         [fitTo, beats] { fitTo (beats, true); });
            fit.addItem (tr ("Fit to tempo, resample ({0})", { label }), true, channel.sampler.stretchBeats == beats && ! channel.sampler.keepPitch,
                         [fitTo, beats] { fitTo (beats, false); });
            fit.addItem (tr ("Half as long"), channel.sampler.stretchBeats > 0.0f, false,
                         [fitTo, b = channel.sampler.stretchBeats, kp = channel.sampler.keepPitch] { fitTo (juce::jmax (0.25f, b * 0.5f), kp); });
            fit.addItem (tr ("Twice as long"), channel.sampler.stretchBeats > 0.0f, false,
                         [fitTo, b = channel.sampler.stretchBeats, kp = channel.sampler.keepPitch] { fitTo (juce::jmin (1024.0f, b * 2.0f), kp); });
            fit.addSeparator();
        }
        fit.addItem (tr ("Original speed (no stretch)"), true, channel.sampler.stretchBeats <= 0.0f, [fitTo] { fitTo (0.0f, false); });
        menu.addSubMenu (tr ("Fit to tempo"), fit, sample != nullptr);

        // FL's "Detect tempo" (REA round 153; LYNX's own detector, core/TempoDetect): a range to fold the result
        // into, then fit the clip to the project tempo or take its tempo for the project
        {
            juce::PopupMenu detect;
            struct Range { double low, high; };
            for (const auto r : { Range { 70.0, 140.0 }, Range { 90.0, 180.0 }, Range { 60.0, 200.0 } })
                detect.addItem (tr ("{0}-{1} BPM", { juce::String (r.low, 0), juce::String (r.high, 0) }), [this, sample, fitTo, r] {
                    if (sample == nullptr)
                        return;
                    tempodetect::Options options;
                    options.minBpm = r.low;
                    options.maxBpm = r.high;
                    const auto found = tempodetect::detect (sample->leftData(), sample->isStereo() ? sample->rightData() : nullptr,
                                                            sample->frames(), sample->sampleRate, options);
                    if (found.bpm <= 0.0)
                    {
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Detect tempo"),
                                                                tr ("No steady tempo was found in this sample."));
                        return;
                    }
                    const auto songBpm = engine.getSong().bpm;
                    auto* window = new juce::AlertWindow (tr ("Detect tempo"),
                                                          tr ("Detected tempo: {0} BPM ({1} beats in the sample). The project plays at {2} BPM.",
                                                              { juce::String (found.bpm, 2), juce::String (found.beats, 2), juce::String (songBpm, 2) }),
                                                          juce::MessageBoxIconType::NoIcon);
                    window->addButton (tr ("Fit the clip to the project tempo"), 1);
                    window->addButton (tr ("Use it as the project tempo"), 2);
                    window->addButton (tr ("Use it, rounded to a whole BPM"), 3);
                    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
                    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, found, fitTo] (int result) {
                                                 if (result == 1)
                                                     fitTo ((float) (std::round (found.beats * 4.0) / 4.0), true);
                                                 else if (result == 2 || result == 3)
                                                 {
                                                     const auto bpm = juce::jlimit (minTempo, maxTempo, result == 3 ? std::round (found.bpm) : found.bpm);
                                                     engine.editSong ([bpm] (Song& s) { s.bpm = bpm; });
                                                 }
                                             }),
                                             true);
                });
            menu.addSubMenu (tr ("Detect tempo"), detect, sample != nullptr);
        }

        if (clip.fadeIn > 0 || clip.fadeOut > 0)
            menu.addItem (tr ("Remove fades"), [this, targets] {
                engine.editSong ([targets] (Song& s) {
                    for (auto i : targets)
                        if (i < (int) s.playlist.size())
                            s.playlist[(std::size_t) i].fadeIn = s.playlist[(std::size_t) i].fadeOut = 0;
                });
            });

        menu.addItem (tr ("Open in a new Sampler channel"), sample != nullptr, false, [this, ch = clip.channel] {
            if (onOpenInSampler)
                onOpenInSampler (ch);
        });
        menu.addItem (tr ("Edit in the audio editor..."), sample != nullptr, false, [this, ch = clip.channel] { openAudioEditorForChannel (engine, ch); });
        menu.addItem (tr ("Show the file in Explorer"), juce::File (path).existsAsFile(), false, [path] { juce::File (path).revealToUser(); });
    }

    // --- pattern -------------------------------------------------------------------------------------
    if (clip.kind == ClipKind::pattern)
    {
        menu.addSeparator();
        menu.addItem (tr ("Open in Channel Rack"), [this, p = clip.pattern] {
            engine.editSong ([p] (Song& s) { if (p < (int) s.patterns.size()) s.currentPattern = p; }, false);
            if (onOpenPattern)
                onOpenPattern();
        });
        menu.addItem (tr ("Open in Piano Roll"), [this, p = clip.pattern] {
            engine.editSong ([p] (Song& s) { if (p < (int) s.patterns.size()) s.currentPattern = p; }, false);
            if (onOpenPianoRoll)
                onOpenPianoRoll();
        });
    }

    // --- automation ----------------------------------------------------------------------------------
    if (clip.kind == ClipKind::automation && clip.automation >= 0 && clip.automation < (int) song.automations.size())
    {
        menu.addSeparator();
        menu.addItem (tr ("Edit points..."), [this, a = clip.automation] { openAutomationEditor (engine, a); });
        menu.addItem (tr ("Delete automation (all of its clips)"), [this, a = clip.automation] {
            setSelection ({});
            engine.editSong ([a] (Song& s) { s.removeAutomation (a); });
        });
    }

    // --- mute and the clipboard ----------------------------------------------------------------------
    menu.addSeparator();
    menu.addItem (clip.muted ? tr ("Unmute") : tr ("Mute"), [this, targets, m = ! clip.muted] {
        engine.editSong ([targets, m] (Song& s) {
            for (auto i : targets)
                if (i < (int) s.playlist.size())
                    s.playlist[(std::size_t) i].muted = m;
        });
    });
    menu.addItem (tr ("Cut") + "\tCtrl+X", [this, targets] {
        setSelection (targets);
        copySelection();
        deleteSelection();
    });
    menu.addItem (tr ("Copy") + "\tCtrl+C", [this, targets] {
        setSelection (targets);
        copySelection();
    });
    menu.addItem (tr ("Paste here") + "\tCtrl+V", ! clipboard.empty(), false, [this, at = snap (xToTick (mouseX), true)] { pasteClips (at); });
    menu.addItem ((many ? tr ("Delete selected") : tr ("Delete")) + "\tDel", [this, targets] {
        setSelection (targets);
        deleteSelection();
    });

    return menu;
}

void PlaylistComponent::normalizeClips (const std::vector<int>& targets)
{
    // clip gain so that the peak of the played segment reaches 0 dB (at most +24 dB)
    const auto& song = engine.getSong();
    std::vector<std::pair<int, float>> gainsFor;

    for (auto i : targets)
    {
        if (i < 0 || i >= (int) song.playlist.size())
            continue;
        const auto& c = song.playlist[(std::size_t) i];

        if (c.kind != ClipKind::audio || c.channel < 0 || c.channel >= (int) song.channels.size())
            continue;

        const auto& channel = song.channels[(std::size_t) c.channel];
        const auto sample = engine.getSampleLibrary().load (juce::String::fromUTF8 (channel.samplePath.c_str()));

        if (sample == nullptr || sample->frames() == 0)
            continue;

        const auto total = (double) sample->frames();
        const auto perTick = clipops::framesPerTick (channel.sampler, total, sample->sampleRate, song.bpm);
        auto from = juce::jlimit (0.0, total, (double) c.offset * perTick);
        auto to = juce::jlimit (from, total, (double) (c.offset + c.length) * perTick);
        if (channel.sampler.reverse) // the clip's offset counts from the file's end
        {
            const auto f = total - to, t = total - from;
            from = f;
            to = t;
        }
        float peak = 0.0f;

        for (auto* data : { sample->leftData(), sample->rightData() })
            if (data != nullptr)
                for (auto f = (std::size_t) from; f < (std::size_t) to; ++f)
                    peak = juce::jmax (peak, std::abs (data[f]));

        if (peak > 1.0e-6f)
            gainsFor.push_back ({ i, juce::jmin (1.0f / peak, juce::Decibels::decibelsToGain (24.0f)) });
    }

    if (! gainsFor.empty())
        engine.editSong ([gainsFor] (Song& s) {
            for (auto [i, g] : gainsFor)
                if (i < (int) s.playlist.size())
                    s.playlist[(std::size_t) i].gain = g;
        });
}

void PlaylistComponent::visibilityChanged()
{
    // hidden while a clip preview or the Listen tool is sounding (no mouse up will come here): stop it
    if (! isShowing() && (drag == Drag::preview || drag == Drag::listen))
    {
        if (drag == Drag::preview)
            stopPreview();
        else
            engine.stop();
        drag = Drag::none;
    }
}

void PlaylistComponent::startPreview (int clip, int x)
{
    const auto& song = engine.getSong();

    if (clip < 0 || clip >= (int) song.playlist.size())
        return;

    const auto c = song.playlist[(std::size_t) clip];
    previewPositionBefore = engine.getTransport().positionTicks.load();
    previewPatternBefore = -1;
    previewRestoreSong = false;
    drag = Drag::preview;
    const auto at = juce::jlimit ((double) c.start, (double) c.end(), xToTick (x));

    if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size())
    {
        // a pattern clip plays its pattern alone, from the clicked point
        const auto length = (double) juce::jmax<std::int64_t> (1, song.patterns[(std::size_t) c.pattern].effectiveLength (song.beatsPerBar));
        previewRestoreSong = song.mode == PlayMode::song;
        previewPatternBefore = song.currentPattern;
        engine.editSong ([p = c.pattern] (Song& s) {
            s.mode = PlayMode::pattern;
            s.currentPattern = p;
        }, false);
        engine.seek (std::fmod (at - (double) c.start + (double) c.offset, length));
    }
    else
    {
        if (song.mode != PlayMode::song)
            engine.editSong ([] (Song& s) { s.mode = PlayMode::song; }, false);
        engine.seek (at);
    }

    engine.play();
    setMouseCursor (editorCursor (gesture::Cursor::listen));
    ui::postHint ({ tr ("Previewing the clip: release the mouse to stop"), {}, false, false }, 60000);
}

void PlaylistComponent::stopPreview()
{
    engine.stop();

    if (previewPatternBefore >= 0)
        engine.editSong ([this] (Song& s) {
            if (previewPatternBefore < (int) s.patterns.size())
                s.currentPattern = previewPatternBefore;
            if (previewRestoreSong)
                s.mode = PlayMode::song;
        }, false);

    engine.seek (previewPositionBefore);
    previewPatternBefore = -1;
    ui::clearPostedHint();
}

void PlaylistComponent::setRipple (bool on)
{
    ripple = on;
    repaint();
}

void PlaylistComponent::rippleDeleteSelection()
{
    if (selection.empty())
        return;

    auto doomed = selection;
    setSelection ({});
    engine.editSong ([&doomed] (Song& s) { noteedit::rippleDelete (s.playlist, doomed); });
}

void PlaylistComponent::zoomAroundCentre (double factor)
{
    const auto grid = gridArea();
    const auto centre = xToTick (grid.getCentreX());
    pixelsPerTick = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), pixelsPerTick * factor);
    scrollTicks = juce::jmax (0.0, centre - (grid.getCentreX() - headerWidth) / pixelsPerTick);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::zoomToBars (int bars)
{
    const auto bar = (double) ppq * juce::jmax (1, engine.getSong().beatsPerBar);
    const auto from = std::floor (scrollTicks / bar) * bar;
    zoomToTicks (from, from + bars * bar);
}

void PlaylistComponent::zoomAll()
{
    zoomToTicks (0.0, (double) engine.getSong().songLength());
}

void PlaylistComponent::zoomToSelection()
{
    const auto& song = engine.getSong();

    if (song.hasLoop() && selection.empty())
        return zoomToTicks ((double) song.loopStart, (double) song.loopEnd);

    if (selection.empty())
        return zoomAll();

    std::int64_t from = INT64_MAX, to = 0;
    int top = 1 << 20;
    for (auto i : selection)
        if (i >= 0 && i < (int) song.playlist.size())
        {
            from = juce::jmin (from, song.playlist[(std::size_t) i].start);
            to = juce::jmax (to, song.playlist[(std::size_t) i].end());
            top = juce::jmin (top, song.playlist[(std::size_t) i].track);
        }

    zoomToTicks ((double) from, (double) to);
    scrollY = juce::jmax (0, trackToY (top) - headerHeight + scrollY);
    updateScrollBars();
    repaint();
}

void PlaylistComponent::centreOnPlayhead()
{
    const auto& song = engine.getSong();
    const auto tick = std::fmod (engine.getTransport().positionTicks.load(), (double) juce::jmax<std::int64_t> (1, song.songLength()));
    scrollTicks = juce::jmax (0.0, tick - gridArea().getWidth() / pixelsPerTick / 2.0);
    updateScrollBars();
    repaint();
}

bool PlaylistComponent::runAction (const std::string& id)
{
    auto is = [&id] (const char* name) { return id == name; };

    if (is ("playlist.zoomIn")) zoomAroundCentre (1.25);
    else if (is ("playlist.zoomOut")) zoomAroundCentre (1.0 / 1.25);
    else if (is ("playlist.zoom1")) zoomToBars (1);
    else if (is ("playlist.zoom2")) zoomToBars (4);
    else if (is ("playlist.zoom3")) zoomToBars (16);
    else if (is ("playlist.zoomAll")) zoomAll();
    else if (is ("playlist.zoomSelection")) zoomToSelection();
    else if (is ("playlist.centrePlayhead")) centreOnPlayhead();
    else if (is ("playlist.rippleDelete")) rippleDeleteSelection();
    else if (is ("playlist.ripple")) setRipple (! ripple);
    else if (is ("playlist.follow")) EditorGlobals::get().setFollowing (false, ! EditorGlobals::get().isFollowing (false));
    else if (is ("playlist.quantizeStarts")) quantizeClips (1.0);
    else if (is ("playlist.quantizeHalf")) quantizeClips (0.5);
    else
        return false;

    return true;
}

void PlaylistComponent::quantizeClips (double strength)
{
    std::vector<int> targets = selection;

    if (targets.empty())
        for (int i = 0; i < (int) engine.getSong().playlist.size(); ++i)
            targets.push_back (i);

    if (targets.empty())
        return;

    const auto grid = gridTicks();
    engine.editSong ([&targets, grid, strength] (Song& s) { transportedit::quantizeClipStarts (s.playlist, targets, grid, strength); });
    sortClips();
}

void PlaylistComponent::deleteSelection()
{
    if (selection.empty())
        return;

    auto doomed = selection;
    setSelection ({});
    engine.editSong ([&doomed] (Song& s) {
        std::sort (doomed.rbegin(), doomed.rend());

        for (auto i : doomed)
            if (i >= 0 && i < (int) s.playlist.size())
                s.playlist.erase (s.playlist.begin() + i);
    });
}

void PlaylistComponent::copySelection()
{
    clipboard.clear();

    for (auto i : selection)
        clipboard.push_back (engine.getSong().playlist[(std::size_t) i]);
}

void PlaylistComponent::pasteClips (std::int64_t at)
{
    if (clipboard.empty())
        return;

    // pastes at `at` (the clip menu's click), else at the playback cursor (or where the first clip was),
    // keeping tracks and spacing
    std::int64_t first = clipboard.front().start;
    for (const auto& c : clipboard)
        first = juce::jmin (first, c.start);

    const auto target = at >= 0 ? at : playheadTick >= 0.0 ? snap (playheadTick, true) : first;
    std::vector<int> added;

    engine.editSong ([&added, target, first] (Song& s) {
        for (auto c : clipboard)
        {
            if (c.kind == ClipKind::pattern && c.pattern >= (int) s.patterns.size())
                continue;
            if (c.kind == ClipKind::audio && c.channel >= (int) s.channels.size())
                continue;
            if (c.kind == ClipKind::automation && c.automation >= (int) s.automations.size())
                continue;

            c.start = c.start - first + target;
            added.push_back ((int) s.playlist.size());
            s.playlist.push_back (c);
        }
    });
    setSelection (added);
    sortClips();
}

void PlaylistComponent::duplicateSelection()
{
    const auto& song = engine.getSong();

    // with a time selection: repeats it right after itself (the selected clips that start inside it, or
    // all the clips that do), and the time selection moves on so the next Ctrl+B continues
    if (song.hasLoop())
    {
        const auto from = song.loopStart, to = song.loopEnd;
        std::vector<int> sources;

        for (auto i : selection)
            if (song.playlist[(std::size_t) i].start >= from && song.playlist[(std::size_t) i].start < to)
                sources.push_back (i);

        std::vector<int> added;
        engine.editSong ([&] (Song& s) {
            added = noteedit::repeatTime (s.playlist, from, to, sources);
            s.loopStart = to;
            s.loopEnd = to + (to - from);
        });
        setSelection (added);
        sortClips();
        return;
    }

    if (selection.empty())
        return;

    const auto bar = (std::int64_t) ppq * juce::jmax (1, song.beatsPerBar);
    std::int64_t from = INT64_MAX, to = 0;

    for (auto i : selection)
    {
        from = juce::jmin (from, song.playlist[(std::size_t) i].start);
        to = juce::jmax (to, song.playlist[(std::size_t) i].end());
    }

    const auto offset = juce::jmax (bar, (to - from + bar - 1) / bar * bar); // like FL's Ctrl+B
    const auto source = selection;
    std::vector<int> added;

    engine.editSong ([&added, &source, offset] (Song& s) {
        for (auto i : source)
        {
            auto c = s.playlist[(std::size_t) i];
            c.start += offset;
            added.push_back ((int) s.playlist.size());
            s.playlist.push_back (c);
        }
    });
    setSelection (added);
    sortClips();
}

bool PlaylistComponent::handleKey (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();
    const auto cmd = mods.isCommandDown();
    const auto code = key.getKeyCode();
    const auto letter = (int) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) code);

    if (code == juce::KeyPress::backspaceKey)
    {
        EditorGlobals::get().toggleGrid();
        return true;
    }

    // Ctrl+Ins / Ctrl+Del: insert / remove the time selection's length across all tracks
    if (cmd && code == juce::KeyPress::insertKey)
    {
        insertTimeSelection();
        return true;
    }

    if (cmd && code == juce::KeyPress::deleteKey)
    {
        deleteTimeSelection();
        return true;
    }

    if (code == juce::KeyPress::deleteKey && ! mods.isShiftDown())
    {
        if (ripple)
            rippleDeleteSelection();
        else
            deleteSelection();
        return true;
    }

    if (code == juce::KeyPress::escapeKey)
    {
        setSelection ({});
        return true;
    }

    if (cmd && letter == 'A')
    {
        std::vector<int> all ((std::size_t) engine.getSong().playlist.size());
        for (std::size_t i = 0; i < all.size(); ++i)
            all[i] = (int) i;
        setSelection (all);
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
        pasteClips();
        return true;
    }

    if (cmd && letter == 'B')
    {
        duplicateSelection();
        return true;
    }

    // single letters: only while the typing piano is off (otherwise they play notes)
    if (! cmd && ! mods.isAltDown() && ! mods.isShiftDown() && ! EditorGlobals::get().typingPiano)
        for (const auto& t : editTools())
            if (letter == t.key && t.tool != EditTool::drumPaint)
            {
                tools.setTool (t.tool);
                return true;
            }

    return false;
}

void PlaylistComponent::mouseMove (const juce::MouseEvent& e)
{
    updateHover (e.getPosition(), e.mods);
}

void PlaylistComponent::mouseExit (const juce::MouseEvent&)
{
    lastMouse = { -1, -1 };

    if (! (hover == Hover {}))
    {
        hover = {};
        repaint();
    }
}

void PlaylistComponent::modifierKeysChanged (const juce::ModifierKeys& mods)
{
    if (drag == Drag::none && lastMouse.x >= 0)
        updateCursor (mods);
}

juce::Rectangle<int> PlaylistComponent::clipBounds (int index) const
{
    const auto& c = engine.getSong().playlist[(std::size_t) index];
    const auto x0 = (float) tickToX ((double) c.start), x1 = (float) tickToX ((double) c.end());
    return juce::Rectangle<float> (x0, (float) trackToY (c.track) + 1.0f, juce::jmax (3.0f, x1 - x0 - 1.0f), (float) heightOf (c.track) - 3.0f)
        .getSmallestIntegerContainer();
}

juce::Rectangle<int> PlaylistComponent::clipMenuButton (int index) const
{
    const auto r = clipBounds (index);

    if (r.getWidth() < 40)
        return {};

    return { r.getX() + 2, r.getY() + 1, 13, (int) clipTitleHeight - 2 };
}

PlaylistComponent::Hover PlaylistComponent::hoverAt (juce::Point<int> p) const
{
    Hover h;

    if (p.x < headerWidth && p.y >= headerHeight && p.y < gridArea().getBottom())
    {
        h.header = true;
        h.track = yToTrack (p.y);
        h.divider = dividerAt (p);
        const auto folded = noteedit::isFoldedAway (engine.getSong(), h.track);
        h.led = ! folded && h.divider < 0 && trackLed (h.track).expanded (3).contains (p);
        h.arrow = ! folded && h.divider < 0 && trackArrow (h.track).contains (p);
        return h;
    }

    if (! gridArea().contains (p))
        return h;

    h.track = yToTrack (p.y);
    bool rightEdge = false, leftEdge = false;
    h.clip = clipAt (p.x, p.y, rightEdge, leftEdge);

    if (h.clip < 0)
        return h;

    const auto& c = engine.getSong().playlist[(std::size_t) h.clip];
    h.target = rightEdge ? gesture::Target::rightEdge : leftEdge ? gesture::Target::leftEdge : gesture::Target::item;

    if (clipMenuButton (h.clip).contains (p))
        h.target = gesture::Target::clipMenu;
    else if (fadeHandleAt (h.clip, p) != 0)
        h.target = gesture::Target::fadeHandle;
    else if (c.kind == ClipKind::automation && automationPointAt (h.clip, p) >= 0)
        h.target = gesture::Target::autoPoint;
    else if (c.kind == ClipKind::automation && isOnAutomationCurve (h.clip, p))
        h.target = gesture::Target::autoCurve;

    return h;
}

void PlaylistComponent::updateHover (juce::Point<int> p, juce::ModifierKeys mods)
{
    lastMouse = p;
    const auto h = hoverAt (p);

    if (! (h == hover))
    {
        // repaint the old and new track rows (header and lane)
        for (auto t : { hover.track, h.track })
            if (t >= 0)
                repaint (0, trackToY (t) - 4, getWidth(), heightOf (t) + 8);
        hover = h;
    }

    if (drag == Drag::none)
        updateCursor (mods);
}

void PlaylistComponent::updateCursor (juce::ModifierKeys mods)
{
    const auto p = lastMouse;
    auto cursor = gesture::Cursor::normal;

    if (hover.header && hover.divider >= 0)
    {
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        return;
    }
    if (hover.header)
        cursor = hover.led || hover.arrow ? gesture::Cursor::point : gesture::Cursor::normal;
    else if (gridArea().contains (p))
    {
        const gesture::Mods m { mods.isShiftDown(), mods.isCtrlDown(), mods.isAltDown() };
        const auto tool = tools.getTool();
        cursor = gesture::cursorFor (gesture::leftAction (tool, hover.target, m, true), tool);
    }
    else if (p.y >= rulerTop && p.y < headerHeight && p.x >= headerWidth)
        cursor = mods.isCtrlDown() ? gesture::Cursor::select : gesture::Cursor::point;

    setMouseCursor (editorCursor (cursor));
}

ui::Hint PlaylistComponent::hintAt (juce::Point<int> p)
{
    ui::Hint hint;
    const auto mods = juce::ModifierKeys::currentModifiers;
    const auto& song = engine.getSong();
    const auto h = hoverAt (p);
    auto trackName = [&song] (int t) {
        return t < (int) song.playlistTracks.size() && ! song.playlistTracks[(std::size_t) t].name.empty()
                   ? juce::String::fromUTF8 (song.playlistTracks[(std::size_t) t].name.c_str())
                   : tr ("Track {0}", { juce::String (t + 1) });
    };

    if (h.header && h.divider >= 0)
    {
        hint.text = tr ("Track divider: [LMB] drag resizes this track  [Shift][LMB] drag: all tracks  [2xLMB] default height");
        hint.value = juce::String (heightOf (h.divider)) + " px";
        return hint;
    }

    if (h.header)
    {
        hint.text = h.led     ? tr ("Track: [LMB] mute / unmute  [Ctrl][LMB] or [RMB] solo")
                    : h.arrow ? tr ("Track group: [LMB] collapse / expand the grouped tracks")
                              : tr ("Track: [LMB] select  [RMB] menu  [2xLMB] open the clip source  [Shift][Wheel] move up / down");
        hint.value = trackName (h.track);
        if (const auto mode = trackmodes::describe (song, h.track); ! mode.empty()) // FL's "(Instrument track)" / "(Audio track, ...)"
            hint.value += " " + (mode.rfind ("(Audio", 0) == 0 ? tr ("(Audio track, linked to mixer track {0})", { juce::String (song.playlistTracks[(std::size_t) h.track].mixerTrack) })
                                                                 : tr ("(Instrument track)"));
        hint.hasMenu = ! h.led && ! h.arrow;
        return hint;
    }

    if (p.y >= rulerTop && p.y < headerHeight && p.x >= headerWidth)
    {
        hint.text = tr ("Ruler: [LMB] song position  [Ctrl][LMB] drag: time selection (loop)  [Shift][LMB] drag in it: move  [2xLMB] clear  [RMB] markers");
        hint.hasMenu = true;
        return hint;
    }

    if (! gridArea().contains (p))
        return hint;

    const gesture::Mods m { mods.isShiftDown(), mods.isCtrlDown(), mods.isAltDown() };
    hint.text = actionHint (gesture::leftAction (tools.getTool(), h.target, m, true), true);

    if (h.clip >= 0)
    {
        const auto onHeader = p.y < clipBounds (h.clip).getY() + (int) clipTitleHeight;
        if (h.target == gesture::Target::clipMenu)
            hint.text = tr ("Clip menu: [LMB] or [RMB] opens it (make unique, rename and colour, chop, select source...)");
        else if (onHeader && ! m.ctrl)
            hint.text << "  " << tr ("The icon at the top left opens the clip menu (make unique, rename...)");
        else if (h.target == gesture::Target::item && ! m.ctrl)
            hint.text << "  " << tr ("[Shift][V] + drag: a unique copy  [Alt][RMB] hold: hear it");
        const auto& c = song.playlist[(std::size_t) h.clip];
        hint.hasMenu = true;
        if (c.kind == ClipKind::pattern && c.pattern >= 0 && c.pattern < (int) song.patterns.size())
            hint.value = juce::String::fromUTF8 (song.patterns[(std::size_t) c.pattern].name.c_str());
        else if (c.kind == ClipKind::audio && c.channel >= 0 && c.channel < (int) song.channels.size())
            hint.value = juce::String::fromUTF8 (song.channels[(std::size_t) c.channel].name.c_str());
        else if (c.kind == ClipKind::automation && c.automation >= 0 && c.automation < (int) song.automations.size())
            hint.value = juce::String::fromUTF8 (song.automations[(std::size_t) c.automation].name.c_str());
    }
    else
        hint.value = trackName (h.track);

    return hint;
}

void PlaylistComponent::mouseDoubleClick (const juce::MouseEvent& e)
{
    // double-click a track divider: back to the default height
    if (const auto divider = dividerAt (e.getPosition()); divider >= 0)
    {
        setTrackHeights ({ divider }, trackHeight);
        return;
    }

    // double-click a track header: open the source of the track's first clip (FL); empty tracks: rename
    if (e.x < headerWidth && e.y >= headerHeight && e.y < gridArea().getBottom() && e.mods.isLeftButtonDown())
    {
        const auto track = yToTrack (e.y);

        if (trackLed (track).expanded (3).contains (e.getPosition()) || trackArrow (track).contains (e.getPosition()))
            return;

        const auto& playlist = engine.getSong().playlist;
        int first = -1;
        for (int i = 0; i < (int) playlist.size(); ++i)
            if (playlist[(std::size_t) i].track == track && (first < 0 || playlist[(std::size_t) i].start < playlist[(std::size_t) first].start))
                first = i;

        if (first >= 0)
            openClipSource (first, e.mods.isAltDown());
        else
            renameTrack (track);
        return;
    }

    bool edge = false;
    const auto hit = gridArea().contains (e.getPosition()) ? clipAt (e.x, e.y, edge) : -1;

    if (hit < 0)
        return;

    openClipSource (hit, e.mods.isAltDown());
}

void PlaylistComponent::openClipSource (int hit, bool other)
{
    if (hit < 0 || hit >= (int) engine.getSong().playlist.size())
        return;

    const auto clip = engine.getSong().playlist[(std::size_t) hit];

    if (clip.kind == ClipKind::pattern && clip.pattern != engine.getSong().currentPattern
        && clip.pattern < (int) engine.getSong().patterns.size())
        engine.editSong ([p = clip.pattern] (Song& s) { s.currentPattern = p; }, false);

    if (clip.kind == ClipKind::automation)
        openAutomationEditor (engine, clip.automation);
    else if (clip.kind == ClipKind::pattern)
    {
        auto pianoRoll = editPatternsIn == 2
                         || (editPatternsIn == 0 && clip.pattern < (int) engine.getSong().patterns.size()
                             && hasPianoRollNotes (engine.getSong().patterns[(std::size_t) clip.pattern]));
        if (other)
            pianoRoll = ! pianoRoll;

        if (pianoRoll && onOpenPianoRoll)
            onOpenPianoRoll();
        else if (! pianoRoll && onOpenPattern)
            onOpenPattern();
    }
}

bool PlaylistComponent::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (const auto& f : files)
        if (juce::File (f).hasFileExtension ("wav;aif;aiff;flac;ogg;mp3"))
            return true;

    return false;
}

namespace
{
// An audio clip of `file` at `start` on `track` (one undo step); `reuse`: an existing audio-clip channel of the same
// file plays it instead of a new one. False when the file can't be read.
bool addAudioClip (AudioEngine& engine, const juce::File& file, std::int64_t start, int track, bool reuse)
{
    if (! file.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;wv"))
        return false;

    const auto data = engine.getSampleLibrary().load (file.getFullPathName());

    if (data == nullptr)
        return false;

    // audio length converted to ticks at the current BPM
    const auto seconds = (double) data->frames() / data->sampleRate;
    const auto length = juce::jmax<std::int64_t> (ppq / 4, (std::int64_t) (seconds * engine.getSong().bpm / 60.0 * ppq));
    const auto path = file.getFullPathName().toStdString();
    const auto name = file.getFileNameWithoutExtension().toStdString();

    engine.editSong ([=] (Song& s) {
        int channel = -1;
        if (reuse)
            for (int c = 0; c < (int) s.channels.size(); ++c)
                if (s.channels[(std::size_t) c].instrument == InstrumentKind::audioClip && s.channels[(std::size_t) c].samplePath == path)
                    channel = c;

        if (channel < 0)
        {
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::audioClip;
            c.samplePath = path;
            c.mixerTrack = s.nextFreeInsert();
            s.channels.push_back (c);
            channel = (int) s.channels.size() - 1;
        }

        Clip clip;
        clip.kind = ClipKind::audio;
        clip.channel = channel;
        clip.track = track;
        clip.start = start;
        clip.length = length;
        s.playlist.push_back (clip);
        s.mode = PlayMode::song;
    });
    return true;
}
} // namespace

void PlaylistComponent::filesDropped (const juce::StringArray& files, int x, int y)
{
    auto start = juce::jmax<std::int64_t> (0, snap (xToTick (x), true));
    auto track = yToTrack (y);

    for (const auto& f : files)
        if (addAudioClip (engine, juce::File (f), start, track, false))
            ++track; // multiple files: one below the other
}

void PlaylistComponent::addAudioClipAtPlayhead (const juce::File& file)
{
    const auto& song = engine.getSong();
    const auto start = song.mode == PlayMode::song ? juce::jmax<std::int64_t> (0, snap ((std::int64_t) engine.getTransport().positionTicks.load(), false))
                                                   : std::int64_t (0);
    // the first track with nothing at the start
    int track = 0;
    for (;; ++track)
    {
        const auto busy = std::any_of (song.playlist.begin(), song.playlist.end(), [&] (const Clip& c) {
            return c.track == track && c.start <= start && c.end() > start;
        });
        if (! busy)
            break;
    }
    addAudioClip (engine, file, start, track, true);
}

void PlaylistComponent::glideBy (double ticks, double pixels)
{
    if (! gliding)
    {
        glideX.jumpTo (scrollTicks);
        glideY.jumpTo (scrollY);
        gliding = true;
    }

    const auto grid = gridArea();
    glideX.target = juce::jlimit (0.0, juce::jmax (0.0, hBar.getMaximumRangeLimit() - grid.getWidth() / pixelsPerTick), glideX.target + ticks);
    glideY.target = juce::jlimit (0.0, juce::jmax (0.0, (double) tracksHeight() - grid.getHeight()), glideY.target + pixels);
    glideFrame();
}

void PlaylistComponent::glideFrame()
{
    if (! gliding)
        return;

    // someone else moved the view (scroll bar, zoom, pan, playback follow): the glide gives way
    if (std::abs (scrollTicks - glideX.current) > 1e-6 || std::abs ((double) scrollY - std::round (glideY.current)) > 0.5)
    {
        gliding = false;
        return;
    }

    scrollTicks = glideX.step (0.45, 0.5 / pixelsPerTick);
    scrollY = (int) std::round (glideY.step (0.45, 1.0));
    glideY.current = scrollY;
    updateScrollBars();
    glideX.current = scrollTicks;
    gliding = glideX.isMoving() || glideY.isMoving();
    repaint();
}

void PlaylistComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto ctrl = e.mods.isCtrlDown(), alt = e.mods.isAltDown(), shift = e.mods.isShiftDown();

    // Shift+wheel over a track header: moves the track up / down
    if (shift && ! ctrl && e.x < headerWidth && e.y >= headerHeight && wheel.deltaY != 0.0f)
    {
        const auto track = yToTrack (e.y);
        const auto other = track + (wheel.deltaY > 0 ? -1 : 1);

        if (other >= 0 && other < numTracks())
        {
            setSelection ({});
            engine.editSong ([track, other] (Song& s) { noteedit::swapTracks (s, track, other); });
            // the mouse follows the track, so the next wheel step moves it further
            refreshTrackLayout();
            juce::Desktop::setMousePosition (localPointToGlobal (juce::Point<int> (e.x, trackToY (other) + heightOf (other) / 2)));
        }
        return;
    }

    if (shift && alt) // nudge the selected clips by the grid
    {
        if (wheel.deltaY != 0.0f && ! selection.empty())
        {
            const auto now = juce::Time::getMillisecondCounter();
            const auto newBurst = now - lastWheelEdit > 700; // one undo step per burst
            lastWheelEdit = now;
            const auto ticks = (wheel.deltaY > 0 ? 1 : -1) * gridTicks();
            engine.editSong ([this, ticks] (Song& s) {
                auto delta = ticks;
                for (auto i : selection)
                    delta = juce::jmax (delta, -s.playlist[(std::size_t) i].start);
                for (auto i : selection)
                    s.playlist[(std::size_t) i].start += delta;
            }, newBurst);
            sortClips();
        }
        return;
    }

    if (ctrl)
    {
        gliding = false;
        const auto anchorPixels = (double) juce::jmax (0, e.x - headerWidth);
        const auto zoomed = juce::jlimit (4.0 / (4 * ppq), 800.0 / (4 * ppq), pixelsPerTick * gesture::wheelZoomFactor (wheel.deltaY, wheel.isSmooth));
        scrollTicks = gesture::zoomAnchored (scrollTicks, pixelsPerTick, zoomed, anchorPixels);
        pixelsPerTick = zoomed;
        zoomBeforeAll = 0.0;
        updateScrollBars();
        repaint();
        return;
    }

    // plain wheel: up/down (Shift: left/right); a horizontal wheel or trackpad swipe scrolls sideways
    if (shift)
        glideBy (gesture::wheelScrollPixels (wheel.deltaY, wheel.isSmooth, 120.0) / pixelsPerTick, 0.0);
    else
        glideBy (gesture::wheelScrollPixels (wheel.deltaX, wheel.isSmooth, 80.0) / pixelsPerTick,
                 gesture::wheelScrollPixels (wheel.deltaY, wheel.isSmooth, 2.0 * trackHeight));
}

void PlaylistComponent::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hBar)
        scrollTicks = start;
    else
        scrollY = (int) start;

    repaint();
}

void PlaylistComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == ui::WaveformCache::getInstanceWithoutCreating())
    {
        repaint();
        return;
    }

    // the clip list changed externally (undo, pattern removal...): the selection no longer applies
    const auto count = engine.getSong().playlist.size();

    if (count != knownClipCount && drag == Drag::none)
    {
        knownClipCount = count;
        selection.clear();
    }

    // switching pattern (top bar, Channel Rack) sets the pencil back to the pattern
    if (engine.getSong().currentPattern != knownCurrentPattern)
    {
        knownCurrentPattern = engine.getSong().currentPattern;
        brush = {};
    }

    followButton.setToggleState (EditorGlobals::get().isFollowing (false), juce::dontSendNotification);
    refreshArrangementBox();
    refreshTrackLayout();
    updateScrollBars();
    repaint();
}

void PlaylistComponent::timerCallback()
{
    auto& transport = engine.getTransport();
    const auto& song = engine.getSong();
    auto tick = -1.0;

    if (song.mode == PlayMode::song && (transport.playing.load() || transport.positionTicks.load() > 0.0))
        tick = std::fmod (transport.positionTicks.load(), (double) song.songLength());

    if (tick != playheadTick && isShowing())
    {
        // only the cursor column (old and new) is repainted: large projects stay cheap
        auto dirty = [this] (double t) {
            if (t >= 0.0)
                repaint ((int) tickToX (t) - 8, 0, 16, getHeight());
        };

        dirty (playheadTick);
        playheadTick = tick;

        // Follow: if the cursor leaves the screen while playing, scrolls one page (off: the view stays put)
        const auto grid = gridArea();
        const auto newStart = transportedit::followViewStart (scrollTicks, grid.getWidth() / pixelsPerTick, tick, 20.0 / pixelsPerTick,
                                                              20.0 / pixelsPerTick);

        if (EditorGlobals::get().isFollowing (false) && transport.playing.load() && newStart != scrollTicks)
        {
            scrollTicks = newStart;
            updateScrollBars();
            repaint();
        }
        else
            dirty (tick);
    }
}

bool PlaylistComponent::isInterestedInDragSource (const SourceDetails& details)
{
    return BrowserComponent::isBrowserDrag (details) || BrowserComponent::draggedPattern (details) >= 0;
}

void PlaylistComponent::itemDropped (const SourceDetails& details)
{
    // a pattern from the Browser's "Current project": a clip of it where it is dropped
    if (const auto pattern = BrowserComponent::draggedPattern (details); pattern >= 0 && pattern < (int) engine.getSong().patterns.size())
    {
        const auto start = juce::jmax<std::int64_t> (0, snap (xToTick (details.localPosition.x), true));
        const auto track = juce::jmax (0, yToTrack (details.localPosition.y));
        engine.editSong ([=] (Song& s) {
            Clip clip;
            clip.kind = ClipKind::pattern;
            clip.pattern = pattern;
            clip.track = track;
            clip.start = start;
            clip.length = juce::jmax<std::int64_t> (ppq, s.patterns[(std::size_t) pattern].effectiveLength (s.beatsPerBar));
            s.playlist.push_back (clip);
            s.mode = PlayMode::song;
        });
        return;
    }

    if (const auto file = BrowserComponent::draggedFile (details); file.existsAsFile())
        filesDropped ({ file.getFullPathName() }, details.localPosition.x, details.localPosition.y);
}
// ---------------------------------------------------------------------------

PatternPicker::PatternPicker (AudioEngine& e, PlaylistComponent& p)
    : engine (e),
      playlist (p)
{
    engine.addChangeListener (this);
    seenBrush = playlist.brush.kind;
}

PatternPicker::~PatternPicker()
{
    engine.removeChangeListener (this);
}

void PatternPicker::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // the pencil's source changed elsewhere (a clip clicked, the track menu): show its kind, as FL does
    if (playlist.brush.kind != seenBrush)
    {
        seenBrush = playlist.brush.kind;
        view = seenBrush == ClipKind::audio ? 1 : seenBrush == ClipKind::automation ? 2 : 0;
        scroll = 0;
    }
    repaint();
}

void PatternPicker::setView (int newView)
{
    view = juce::jlimit (0, 2, newView);
    scroll = 0;
    repaint();
}

std::vector<PatternPicker::Row> PatternPicker::rows() const
{
    const auto& song = engine.getSong();
    std::vector<Row> list;

    if (view == 0)
        for (int i = 0; i < (int) song.patterns.size(); ++i)
            list.push_back ({ ClipKind::pattern, i, juce::String::fromUTF8 (song.patterns[(std::size_t) i].name.c_str()),
                              PlaylistComponent::patternColour (song, i) });

    if (view == 1)
        for (int c = 0; c < (int) song.channels.size(); ++c)
            if (song.channels[(std::size_t) c].instrument == InstrumentKind::audioClip)
                list.push_back ({ ClipKind::audio, c, juce::String::fromUTF8 (song.channels[(std::size_t) c].name.c_str()),
                                  PlaylistComponent::audioClipColour (song.channels[(std::size_t) c]) });

    if (view == 2)
        for (int a = 0; a < (int) song.automations.size(); ++a)
            list.push_back ({ ClipKind::automation, a, juce::String::fromUTF8 (song.automations[(std::size_t) a].name.c_str()), theme::colour::automation });

    return list;
}

juce::Rectangle<int> PatternPicker::tabBounds (int tab) const
{
    const auto w = (getWidth() - 8) / 3;
    return { 4 + tab * w, 2, w, headerHeight - 4 };
}

int PatternPicker::listBottom() const
{
    return view == 0 ? getHeight() - footerHeight : getHeight();
}

juce::Rectangle<int> PatternPicker::addBounds() const
{
    return view == 0 ? juce::Rectangle<int> (4, getHeight() - footerHeight + 2, getWidth() - 8, footerHeight - 4) : juce::Rectangle<int>();
}

int PatternPicker::rowAt (int y) const
{
    if (y < headerHeight || y >= listBottom())
        return -1;

    const auto index = (y - headerHeight) / rowHeight + scroll;
    return index < (int) rows().size() ? index : -1;
}

void PatternPicker::paint (juce::Graphics& g)
{
    const auto& song = engine.getSong();
    const auto list = rows();
    using namespace theme;
    g.fillAll (colour::bg2);
    g.setColour (colour::bg3);
    g.fillRect (0, 0, getWidth(), headerHeight);
    g.setColour (colour::border);
    g.fillRect (0, headerHeight - 1, getWidth(), 1);

    // the three tabs: patterns, audio clips, automation clips
    const Icon tabIcons[] = { Icon::pattern, Icon::audioFile, Icon::automate };
    for (int t = 0; t < 3; ++t)
    {
        const auto r = tabBounds (t).toFloat();
        if (t == view || t == hoverTab)
        {
            g.setColour (t == view ? colour::bg5 : colour::bg4);
            g.fillRoundedRectangle (r, radius::sm);
        }
        icons::draw (g, tabIcons[t], r.withSizeKeepingCentre (16.0f, 16.0f), t == view ? colour::accent : t == hoverTab ? colour::textPrimary : colour::textSecondary);
    }

    {
        juce::Graphics::ScopedSaveState state (g);
        g.reduceClipRegion (0, headerHeight, getWidth(), listBottom() - headerHeight);

        for (int i = scroll; i < (int) list.size(); ++i)
        {
            const auto y = headerHeight + (i - scroll) * rowHeight;

            if (y > listBottom())
                break;

            const auto& row = list[(std::size_t) i];
            const auto r = juce::Rectangle<int> (4, y + 2, getWidth() - 8, rowHeight - 4);
            const auto current = row.kind == ClipKind::pattern ? (playlist.brush.kind == ClipKind::pattern && row.index == song.currentPattern)
                                                               : (playlist.brush.kind == row.kind && playlist.brush.index == row.index);
            const auto rowColour = tame (row.colour);
            const auto box = r.toFloat();
            auto fill = rowColour.interpolatedWith (colour::bg3, current ? 0.25f : 0.55f);
            if (i == hoverRow && ! current)
                fill = fill.interpolatedWith (colour::bg5, 0.35f);
            g.setColour (fill);
            g.fillRoundedRectangle (box, radius::sm);

            // a faint miniature of the pattern's notes behind the name (FL's picker shows the same)
            if (row.kind == ClipKind::pattern && row.index < (int) song.patterns.size())
            {
                const auto& pattern = song.patterns[(std::size_t) row.index];
                if (! pattern.notes.empty())
                {
                    int low = 127, high = 0;
                    for (const auto& n : pattern.notes)
                    {
                        low = juce::jmin (low, n.key);
                        high = juce::jmax (high, n.key);
                    }
                    const auto body = box.reduced (3.0f, 3.0f);
                    const auto span = (float) juce::jmax (6, high - low + 2);
                    const auto length = (double) juce::jmax<std::int64_t> (1, pattern.effectiveLength (song.beatsPerBar));
                    g.setColour (clipContentColour (rowColour).withAlpha (0.35f));
                    for (const auto& n : pattern.notes)
                    {
                        const auto nx = body.getX() + (float) (n.start / length) * body.getWidth();
                        const auto nw = juce::jmax (1.5f, (float) (n.length / length) * body.getWidth());
                        const auto ny = body.getBottom() - ((float) (n.key - low) + 1.0f) / span * body.getHeight();
                        g.fillRect (nx, ny, juce::jmin (nw, body.getRight() - nx), 1.5f);
                    }
                }
            }

            if (current) // the current source: the accent strip
            {
                g.setColour (colour::accent);
                g.fillRoundedRectangle (box.withWidth (3.0f), 1.5f);
            }

            g.setColour (clipTitleText (fill));
            g.setFont (font (type::small, current));
            textcache::drawText (g, row.name, r.withTrimmedLeft (9).withTrimmedRight (4), juce::Justification::centredLeft, true);
        }
    }

    if (const auto add = addBounds(); ! add.isEmpty())
    {
        g.setColour (hoverAdd ? colour::bg4 : colour::bg3);
        g.fillRoundedRectangle (add.toFloat(), radius::sm);
        icons::draw (g, Icon::plus, add.toFloat().withSizeKeepingCentre (14.0f, 14.0f), hoverAdd ? colour::textPrimary : colour::textSecondary);
    }

    g.setColour (colour::border);
    g.drawVerticalLine (getWidth() - 1, 0.0f, (float) getHeight());
}

void PatternPicker::mouseDown (const juce::MouseEvent& e)
{
    for (int t = 0; t < 3; ++t)
        if (tabBounds (t).contains (e.getPosition()))
        {
            setView (t);
            return;
        }

    if (addBounds().contains (e.getPosition()))
    {
        if (onNewPattern)
            onNewPattern();
        return;
    }

    const auto list = rows();
    const auto i = rowAt (e.y);

    if (i < 0)
        return;

    const auto& row = list[(std::size_t) i];
    playlist.brush = { row.kind, row.index };
    seenBrush = row.kind;

    if (row.kind == ClipKind::pattern && row.index != engine.getSong().currentPattern)
        engine.editSong ([p = row.index] (Song& s) { s.currentPattern = p; }, false);

    if (e.mods.isPopupMenu() && row.kind == ClipKind::pattern && onPatternMenu)
        onPatternMenu();

    repaint();
}

void PatternPicker::mouseDoubleClick (const juce::MouseEvent& e)
{
    const auto list = rows();
    const auto i = rowAt (e.y);

    if (i >= 0 && list[(std::size_t) i].kind == ClipKind::pattern && onOpenPattern)
        onOpenPattern();
}

void PatternPicker::mouseMove (const juce::MouseEvent& e)
{
    int tab = -1;
    for (int t = 0; t < 3; ++t)
        if (tabBounds (t).contains (e.getPosition()))
            tab = t;
    const auto row = rowAt (e.y);
    const auto add = addBounds().contains (e.getPosition());

    if (tab != hoverTab || row != hoverRow || add != hoverAdd)
    {
        hoverTab = tab;
        hoverRow = row;
        hoverAdd = add;
        repaint();
    }
}

void PatternPicker::mouseExit (const juce::MouseEvent&)
{
    hoverTab = hoverRow = -1;
    hoverAdd = false;
    repaint();
}

ui::Hint PatternPicker::hintAt (juce::Point<int> p)
{
    ui::Hint hint;
    const char* tabs[] = { TR_NOOP ("Picker: patterns"), TR_NOOP ("Picker: audio clips"), TR_NOOP ("Picker: automation clips") };

    for (int t = 0; t < 3; ++t)
        if (tabBounds (t).contains (p))
        {
            hint.text = tr (tabs[t]) + "  " + tr ("[LMB] show");
            return hint;
        }

    if (addBounds().contains (p))
    {
        hint.text = tr ("New pattern");
        return hint;
    }

    const auto list = rows();
    if (const auto i = rowAt (p.y); i >= 0)
    {
        hint.value = list[(std::size_t) i].name;
        hint.text = list[(std::size_t) i].kind == ClipKind::pattern
                        ? tr ("Pattern: [LMB] paint it with the pencil  [2xLMB] open  [RMB] menu")
                        : tr ("Clip source: [LMB] paint it with the pencil");
        hint.hasMenu = list[(std::size_t) i].kind == ClipKind::pattern;
    }

    return hint;
}

void PatternPicker::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const auto rows = (int) this->rows().size();
    const auto visible = juce::jmax (1, (listBottom() - headerHeight) / rowHeight);
    scroll = juce::jlimit (0, juce::jmax (0, rows - visible), scroll - (int) std::round (wheel.deltaY * 6.0f));
    repaint();
}
} // namespace lynx
