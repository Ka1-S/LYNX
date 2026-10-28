#include "ui/LayerEditor.h"

#include <algorithm>

#include "core/Layer.h"
#include "ui/ControllerKnob.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"

namespace lynx
{
namespace
{
juce::String text (const std::string& s)
{
    return juce::String::fromUTF8 (s.c_str());
}

juce::String keyName (int key)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    key = juce::jlimit (0, 127, key);
    return juce::String (names[key % 12]) + juce::String (key / 12);
}

juce::Colour childColour (const ChannelDef& c, int index)
{
    if (c.colour != 0)
        return theme::tame (juce::Colour (c.colour));
    return theme::noteGroupHue (1 + index % 15);
}

// What the editor edits (round 165c): a Channel Rack Layer (`channel`), or a Layer node inside a Patcher whose children
// are the nodes on its Children pin (`channel` −1: no Set / Show / Group / Delete children, its own Level / Pan / Pitch)
struct LayerSource
{
    std::function<ChannelDef* (Song&)> layer;
    std::function<std::vector<ChannelDef*> (Song&)> children;
    int channel = -1;

    static LayerSource rack (int c)
    {
        LayerSource s;
        s.channel = c;
        s.layer = [c] (Song& song) -> ChannelDef* {
            return c >= 0 && c < (int) song.channels.size() && song.channels[(std::size_t) c].instrument == InstrumentKind::layer ? &song.channels[(std::size_t) c]
                                                                                                                                : nullptr;
        };
        s.children = [c] (Song& song) {
            std::vector<ChannelDef*> out;
            for (const auto k : layer::validChildren (song, c))
                out.push_back (&song.channels[(std::size_t) k]);
            return out;
        };
        return s;
    }
};

class LayerEditor final : public juce::Component,
                          private juce::ChangeListener
{
public:
    LayerEditor (AudioEngine& e, LayerSource s) : engine (e), source (std::move (s)), channel (source.channel)
    {
        const auto onChannel = channel >= 0;
        auto button = [this] (juce::TextButton& b, const juce::String& title, const juce::String& hint, std::function<void()> click) {
            b.setButtonText (title);
            ui::setHint (b, hint);
            b.onClick = std::move (click);
            addAndMakeVisible (b);
        };
        if (onChannel)
        button (setChildren, tr ("Set children"), tr ("The channels selected in the Channel Rack become this Layer's children (select the ones to keep and press again to remove one)"),
                [this] {
                    const auto selected = engine.getSelectedChannels();
                    engine.editSong ([ch = channel, selected] (Song& s) { layer::setChildren (s, ch, selected); });
                });
        if (onChannel)
        button (showChildren, tr ("Show children"), tr ("Selects this Layer's children in the Channel Rack"), [this] {
            const auto kids = layer::validChildren (engine.getSong(), channel);
            if (! kids.empty())
                engine.setChannelSelection (kids, kids.front());
        });
        button (menuButton, tr ("Layering") + juce::String::fromUTF8 (" \xe2\x96\xbe"), tr ("Split, reset, group or delete the children"), [this] { showMenu(); });

        auto toggle = [this] (juce::ToggleButton& t, const juce::String& title, const juce::String& hint, std::function<void (LayerSettings&, bool)> set) {
            t.setButtonText (title);
            ui::setHint (t, hint);
            t.onClick = [this, &t, set] {
                editLayer ([on = t.getToggleState(), set] (ChannelDef& l) { set (l.layer, on); });
            };
            addAndMakeVisible (t);
        };
        toggle (random, tr ("Random"), tr ("Random: each note plays one child picked at random (varied hits); off = every child plays"),
                [] (LayerSettings& l, bool on) {
                    l.random = on;
                    if (on)
                        l.sequential = false;
                });
        toggle (sequential, tr ("Sequential"), tr ("Sequential: the children play in turn, one per note (round robin)"), [] (LayerSettings& l, bool on) {
            l.sequential = on;
            if (on)
                l.random = false;
        });
        toggle (crossfade, tr ("Crossfade"), tr ("Crossfade: the Fade knob moves from the first child to the last, blending neighbours"),
                [] (LayerSettings& l, bool on) { l.crossfade = on; });

        auto spec = ui::percentSpec (tr ("Layer - Fade (crossfade position)"), 0.0);
        spec.toText = [this] (double v) { return fadeText (v); };
        fade = std::make_unique<ctlui::Knob> (&engine, spec, [this] { return (double) settings().fade; },
                                              [this] (double v) { editLayer ([v] (ChannelDef& l) { l.layer.fade = (float) juce::jlimit (0.0, 1.0, v); }); });
        fade->shortName = tr ("Fade");
        addAndMakeVisible (*fade);

        if (! onChannel) // a node has no channel strip: its own level, pan and pitch here (what a Layer channel's header holds)
        {
            nodeHint.setText (tr ("Cable its Children pin to the generators it plays"), juce::dontSendNotification);
            nodeHint.setFont (theme::font (theme::type::caption));
            nodeHint.setColour (juce::Label::textColourId, theme::colour::textSecondary);
            nodeHint.setJustificationType (juce::Justification::topLeft);
            addAndMakeVisible (nodeHint);
            auto knob = [this] (std::unique_ptr<ctlui::Knob>& k, ui::ValueSpec spec, const juce::String& shortName, std::function<double (const ChannelDef&)> get,
                                std::function<void (ChannelDef&, double)> set) {
                k = std::make_unique<ctlui::Knob> (&engine, spec,
                                                   [this, get] {
                                                       const auto* l = source.layer (const_cast<Song&> (engine.getSong()));
                                                       return l != nullptr ? get (*l) : 0.0;
                                                   },
                                                   [this, set] (double v) { editLayer ([set, v] (ChannelDef& l) { set (l, v); }); });
                k->shortName = shortName;
                addAndMakeVisible (*k);
            };
            auto level = ui::percentSpec (tr ("Layer - Level"), 0.78);
            knob (levelKnob, level, tr ("Level"), [] (const ChannelDef& l) { return (double) l.volume; },
                  [] (ChannelDef& l, double v) { l.volume = (float) juce::jlimit (0.0, 1.0, v); });
            auto pan = ui::panSpec (tr ("Layer - Pan"));
            knob (panKnob, pan, tr ("Pan"), [] (const ChannelDef& l) { return (double) l.pan; },
                  [] (ChannelDef& l, double v) { l.pan = (float) juce::jlimit (-1.0, 1.0, v); });
            ui::ValueSpec pitch;
            pitch.name = tr ("Layer - Pitch");
            pitch.minimum = -24.0, pitch.maximum = 24.0, pitch.defaultValue = 0.0;
            pitch.toText = [] (double v) { return (v > 0.0 ? "+" : "") + juce::String (v, 2) + " st"; };
            pitch.fromText = ui::inverseOf (pitch.toText, -24.0, 24.0);
            knob (pitchKnob, pitch, tr ("Pitch"), [] (const ChannelDef& l) { return (double) l.sampler.pitch; },
                  [] (ChannelDef& l, double v) { l.sampler.pitch = (float) juce::jlimit (-24.0, 24.0, v); });
        }

        engine.addChangeListener (this);
        refresh();
        setSize (600, 360);
    }

    ~LayerEditor() override { engine.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg2);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::title, true));
        g.drawText (tr ("Layer"), 14, 8, 200, 22, juce::Justification::centredLeft);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        g.drawText (tr ("Makes no sound: every note it gets plays its children"), 90, 8, getWidth() - 104, 22, juce::Justification::centredRight);

        const auto kids = children();
        const auto& l = settings();

        // children list
        auto list = listArea();
        fillRoundRect (g, list.toFloat(), radius::md, colour::bg0);
        g.setFont (font (type::caption, true));
        g.setColour (colour::textSecondary);
        g.drawText (caps (tr ("Children")) + "  " + juce::String ((int) kids.size()), list.removeFromTop (20).reduced (8, 0), juce::Justification::centredLeft);
        if (kids.empty())
        {
            g.setFont (font (type::small));
            g.setColour (colour::textDisabled);
            g.drawFittedText (channel >= 0 ? tr ("No children yet: select channels in the Channel Rack, then press Set children.")
                                           : tr ("No children yet: cable the Children pin to the generators this Layer plays."),
                              list.reduced (10), juce::Justification::centred, 3);
        }
        rows.clear();
        for (int i = 0; i < (int) kids.size() && list.getHeight() >= rowHeight; ++i)
        {
            const auto& c = *kids[(std::size_t) i];
            auto r = list.removeFromTop (rowHeight).reduced (4, 1);
            rows.push_back ({ r, i });
            const auto selected = channel >= 0 && engine.getSelectedChannel() == childChannel (i);
            if (selected)
                fillRoundRect (g, r.toFloat(), radius::sm, colour::accentMuted);
            else if (r.contains (hoverPoint))
                fillRoundRect (g, r.toFloat(), radius::sm, colour::bg3);
            fillRoundRect (g, r.removeFromLeft (6).reduced (0, 3).toFloat(), 2.0f, childColour (c, i));
            r.removeFromLeft (6);
            // the crossfade level this child plays at (or full)
            const auto gain = l.crossfade ? layer::crossfadeGain (i, (int) kids.size(), l.fade) : 1.0f;
            auto meter = r.removeFromRight (70).reduced (4, 7);
            fillRoundRect (g, meter.toFloat(), 2.0f, colour::bg4);
            fillRoundRect (g, meter.withWidth (juce::roundToInt ((float) meter.getWidth() * gain)).toFloat(), 2.0f,
                           gain > 0.0f ? colour::accent : colour::bg4);
            auto region = r.removeFromRight (90);
            g.setFont (font (type::small));
            g.setColour (c.muted ? colour::textDisabled : colour::textPrimary);
            g.drawText (text (c.name), r, juce::Justification::centredLeft, true);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption));
            g.drawText (! c.hasKeyRegion() ? tr ("all keys")
                        : c.keyLow == c.keyHigh ? keyName (c.keyLow)
                                                : keyName (c.keyLow) + juce::String::fromUTF8 ("\xe2\x80\x93") + keyName (c.keyHigh),
                        region, juce::Justification::centredRight);
        }

        // keyboard map: the children's key regions over the keys, the Layer's root marked
        drawKeyMap (g, keyMapArea(), kids);
    }

    void resized() override
    {
        auto controls = getLocalBounds().reduced (14).withTrimmedTop (32);
        controls = controls.removeFromLeft (180);
        if (channel < 0)
        {
            nodeHint.setBounds (controls.removeFromTop (34));
            controls.removeFromTop (4);
        }
        for (auto* b : { &setChildren, &showChildren, &menuButton })
            if (b->isVisible())
            {
                b->setBounds (controls.removeFromTop (28));
                controls.removeFromTop (6);
            }
        controls.removeFromTop (4);
        // the three modes on the left, the crossfade's Fade knob beside them (clear of the key map below)
        auto modes = controls.removeFromTop (78);
        fade->setBounds (modes.removeFromRight (66).withSizeKeepingCentre (66, 70));
        for (auto* t : { &random, &sequential, &crossfade })
        {
            t->setBounds (modes.removeFromTop (24));
            modes.removeFromTop (2);
        }
        if (levelKnob != nullptr)
        {
            controls.removeFromTop (6);
            auto knobs = controls.removeFromTop (64);
            for (auto* k : { levelKnob.get(), panKnob.get(), pitchKnob.get() })
                k->setBounds (knobs.removeFromLeft (60).withSizeKeepingCentre (58, 64));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        hoverPoint = e.getPosition();
        repaint (listArea());
    }
    void mouseExit (const juce::MouseEvent&) override
    {
        hoverPoint = { -1, -1 };
        repaint (listArea());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (keyMapArea().contains (e.getPosition()))
        {
            // right-click a key: the Layer's root (FL's preview keyboard)
            if (const auto k = keyAt (e.position, keyMapArea()); k >= 0 && e.mods.isPopupMenu())
                editLayer ([k] (ChannelDef& l) { l.misc.rootNote = k; });
            return;
        }
        for (const auto& [r, index] : rows)
            if (r.contains (e.getPosition()))
            {
                const auto child = childChannel (index);
                if (e.mods.isPopupMenu())
                {
                    juce::PopupMenu m;
                    if (channel >= 0)
                        m.addItem (tr ("Remove from this Layer"), [this, child] {
                            engine.editSong ([ch = channel, child] (Song& s) {
                                auto& kids = s.channels[(std::size_t) ch].layer.children;
                                kids.erase (std::remove (kids.begin(), kids.end(), child), kids.end());
                            });
                        });
                    m.addItem (tr ("Whole keyboard for this child"), [this, i = index] {
                        editChildren ([i] (ChannelDef&, const std::vector<ChannelDef*>& kids) {
                            if (i < (int) kids.size())
                            {
                                kids[(std::size_t) i]->keyLow = 0;
                                kids[(std::size_t) i]->keyHigh = 127;
                            }
                        });
                    });
                    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
                }
                else if (channel >= 0)
                    engine.setSelectedChannel (child);
                return;
            }
    }

private:
    static constexpr int rowHeight = 24;

    const LayerSettings& settings() const
    {
        static const LayerSettings none;
        const auto* l = source.layer (const_cast<Song&> (engine.getSong()));
        return l != nullptr ? l->layer : none;
    }
    std::vector<const ChannelDef*> children() const
    {
        const auto kids = source.children (const_cast<Song&> (engine.getSong()));
        return { kids.begin(), kids.end() };
    }
    // a child's channel index (a Channel Rack Layer; −1 on a node)
    int childChannel (int index) const
    {
        if (channel < 0)
            return -1;
        const auto kids = layer::validChildren (engine.getSong(), channel);
        return index >= 0 && index < (int) kids.size() ? kids[(std::size_t) index] : -1;
    }
    void editLayer (std::function<void (ChannelDef&)> change)
    {
        engine.editSong ([l = source.layer, change = std::move (change)] (Song& s) {
            if (auto* d = l (s))
                change (*d);
        });
    }
    void editChildren (std::function<void (ChannelDef&, const std::vector<ChannelDef*>&)> change)
    {
        engine.editSong ([l = source.layer, c = source.children, change = std::move (change)] (Song& s) {
            if (auto* d = l (s))
                change (*d, c (s));
        });
    }

    juce::Rectangle<int> listArea() const { return getLocalBounds().reduced (14).withTrimmedTop (32).withTrimmedLeft (196).withTrimmedBottom (78); }
    juce::Rectangle<int> keyMapArea() const { return getLocalBounds().reduced (14).removeFromBottom (66); }

    juce::String fadeText (double v) const
    {
        const auto n = (int) children().size();
        if (n <= 1)
            return juce::String (juce::roundToInt (v * 100.0)) + "%";
        const auto p = v * (n - 1);
        const auto a = (int) std::floor (p + 1e-9);
        if (std::abs (p - std::round (p)) < 0.02)
            return tr ("child {0}", { juce::String ((int) std::round (p) + 1) });
        return juce::String (a + 1) + "+" + juce::String (a + 2);
    }

    static bool isBlack (int k) noexcept
    {
        const auto n = k % 12;
        return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
    }
    // the keyboard map shows C1…C10 (12…120): 64 white keys plus the last C
    static constexpr int firstKey = 12, lastKey = 120;
    static int whiteIndex (int k) noexcept
    {
        int n = 0;
        for (int i = firstKey; i < k; ++i)
            n += isBlack (i) ? 0 : 1;
        return n;
    }
    static int whiteCount() noexcept { return whiteIndex (lastKey) + 1; }
    static juce::Rectangle<float> keyRect (int k, juce::Rectangle<int> area, float keysTop)
    {
        const auto w = (float) area.getWidth() / (float) whiteCount();
        const auto h = (float) area.getBottom() - keysTop;
        if (isBlack (k))
            return { (float) area.getX() + (float) whiteIndex (k) * w - w * 0.32f, keysTop, w * 0.64f, h * 0.6f };
        return { (float) area.getX() + (float) whiteIndex (k) * w, keysTop, w - 1.0f, h };
    }
    int keyAt (juce::Point<float> p, juce::Rectangle<int> area) const
    {
        const auto keysTop = (float) area.getY() + 26.0f;
        for (int k = firstKey; k <= lastKey; ++k)
            if (isBlack (k) && keyRect (k, area, keysTop).contains (p))
                return k;
        for (int k = firstKey; k <= lastKey; ++k)
            if (! isBlack (k) && keyRect (k, area, keysTop).withY ((float) area.getY()).withBottom ((float) area.getBottom()).contains (p))
                return k;
        return -1;
    }

    void drawKeyMap (juce::Graphics& g, juce::Rectangle<int> area, const std::vector<const ChannelDef*>& kids) const
    {
        using namespace theme;
        const auto keysTop = (float) area.getY() + 26.0f;
        const auto* self = source.layer (const_cast<Song&> (engine.getSong()));
        const auto root = self != nullptr ? self->misc.rootNote : 60;
        // one lane per child above the keys (up to 4 lanes; more children share them)
        const auto lanes = juce::jlimit (1, 4, (int) kids.size());
        const auto laneH = 22.0f / (float) lanes;
        for (int i = 0; i < (int) kids.size(); ++i)
        {
            const auto& c = *kids[(std::size_t) i];
            const auto lo = juce::jlimit (firstKey, lastKey, c.keyLow), hi = juce::jlimit (firstKey, lastKey, c.keyHigh);
            const auto x0 = keyRect (lo, area, keysTop).getX(), x1 = keyRect (hi, area, keysTop).getRight();
            const auto y = (float) area.getY() + (float) (i % lanes) * laneH;
            fillRoundRect (g, { x0, y + 1.0f, juce::jmax (3.0f, x1 - x0), laneH - 2.0f }, 2.0f, childColour (c, i));
        }
        for (int k = firstKey; k <= lastKey; ++k)
            if (! isBlack (k))
            {
                const auto r = keyRect (k, area, keysTop);
                g.setColour (k == root ? colour::accent : juce::Colour (0xffe6e8ec));
                g.fillRect (r);
                if (k % 12 == 0)
                {
                    g.setColour (k == root ? colour::onAccent : juce::Colour (0xff5a5f6a));
                    g.setFont (font (8.5f));
                    g.drawText ("C" + juce::String (k / 12), r.withTrimmedTop (r.getHeight() - 11.0f).expanded (6.0f, 0.0f).toNearestInt(),
                                juce::Justification::centred);
                }
            }
        for (int k = firstKey; k <= lastKey; ++k)
            if (isBlack (k))
            {
                g.setColour (k == root ? colour::accent : colour::bg0);
                g.fillRect (keyRect (k, area, keysTop));
            }
    }

    void showMenu()
    {
        juce::PopupMenu m;
        const auto has = ! children().empty();
        m.addItem (tr ("Split children (one key each, from the root key)"), has, false,
                   [this] { editChildren ([] (ChannelDef& l, const std::vector<ChannelDef*>& kids) { layer::splitChildren (l, kids); }); });
        m.addItem (tr ("Reset children (whole keyboard, root on C5)"), has, false,
                   [this] { editChildren ([] (ChannelDef&, const std::vector<ChannelDef*>& kids) { layer::resetChildren (kids); }); });
        if (channel < 0) // (a node's children are nodes: no channel groups, no channels to delete)
        {
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
            return;
        }
        m.addItem (tr ("Group children..."), has, false, [this] { groupChildren(); });
        m.addItem (tr ("Delete children (removes the channels)"), has, false, [this] {
            const auto kids = layer::validChildren (engine.getSong(), channel);
            juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, tr ("Delete children"),
                                                tr ("Delete the {0} child channels of this Layer from the project?", { juce::String ((int) kids.size()) }),
                                                tr ("Delete"), tr ("Cancel"), nullptr,
                                                juce::ModalCallbackFunction::create ([this, kids] (int result) {
                                                    if (result != 1)
                                                        return;
                                                    engine.editSong ([kids] (Song& s) {
                                                        auto sorted = kids;
                                                        std::sort (sorted.rbegin(), sorted.rend());
                                                        for (const auto k : sorted)
                                                            s.removeChannel (k);
                                                    });
                                                }));
        });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&menuButton));
    }

    void groupChildren()
    {
        auto* window = new juce::AlertWindow (tr ("Filter group name"), {}, juce::MessageBoxIconType::NoIcon);
        const auto& song = engine.getSong();
        window->addTextEditor ("name", channel < (int) song.channels.size() ? text (song.channels[(std::size_t) channel].name) : tr ("Layer"));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window] (int result) {
                                     const auto name = window->getTextEditorContents ("name").trim().toStdString();
                                     if (result != 1 || name.empty())
                                         return;
                                     engine.editSong ([ch = channel, name] (Song& s) {
                                         auto it = std::find (s.channelGroups.begin(), s.channelGroups.end(), name);
                                         const auto group = it != s.channelGroups.end() ? (int) (it - s.channelGroups.begin())
                                                                                        : (s.channelGroups.push_back (name), (int) s.channelGroups.size() - 1);
                                         for (const auto k : layer::validChildren (s, ch))
                                             s.channels[(std::size_t) k].group = group;
                                         s.channels[(std::size_t) ch].group = group;
                                     });
                                 }),
                                 true);
    }

    void refresh()
    {
        const auto& l = settings();
        random.setToggleState (l.random, juce::dontSendNotification);
        sequential.setToggleState (l.sequential, juce::dontSendNotification);
        crossfade.setToggleState (l.crossfade, juce::dontSendNotification);
        fade->setEnabled (l.crossfade);
        fade->setAlpha (l.crossfade ? 1.0f : 0.4f);
        for (auto* k : { levelKnob.get(), panKnob.get(), pitchKnob.get() })
            if (k != nullptr)
                k->repaint();
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    AudioEngine& engine;
    LayerSource source;
    int channel;
    juce::TextButton setChildren, showChildren, menuButton;
    juce::ToggleButton random, sequential, crossfade;
    std::unique_ptr<ctlui::Knob> fade, levelKnob, panKnob, pitchKnob;
    juce::Label nodeHint;
    std::vector<std::pair<juce::Rectangle<int>, int>> rows;
    juce::Point<int> hoverPoint { -1, -1 };
};
} // namespace

std::unique_ptr<juce::Component> makeLayerEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<LayerEditor> (engine, LayerSource::rack (channel));
}

std::unique_ptr<juce::Component> makeLayerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate,
                                                  std::function<std::vector<ChannelDef*> (Song&)> children)
{
    LayerSource s;
    s.layer = [locate = std::move (locate)] (Song& song) -> ChannelDef* {
        auto* c = locate (song);
        return c != nullptr && c->instrument == InstrumentKind::layer ? c : nullptr;
    };
    s.children = std::move (children);
    return std::make_unique<LayerEditor> (engine, std::move (s));
}
} // namespace lynx
