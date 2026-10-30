#include "ui/SoundFontEditor.h"

#include <algorithm>
#include <atomic>
#include <thread>

#include "core/SoundFont.h"
#include "core/SoundFontIndex.h"
#include "engine/PluginHost.h"
#include "core/SearchQuery.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"

namespace lynx
{
namespace
{
constexpr const char* dragPrefix = "lynx-sf-preset|";

juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }
std::string std8 (const juce::String& s) { return s.toStdString(); }

// a parameter's shown value, translated (the numbers kept)
juce::String shownValue (int index, float v)
{
    if (index == sf::pPolyphony)
        return tr ("{0} voices", { juce::String ((int) v) });
    if ((index == sf::pReverbTrack || index == sf::pChorusTrack) && v >= 1.0f)
        return tr ("Insert {0}", { juce::String ((int) v) });
    return tr (u8 (sf::displayValue (index, v)));
}

// a preset category, translated ("Bass" is the EQ band in the shared table: the instrument family is "Baixo")
juce::String categoryText (const std::string& c)
{
    if (c == "Bass")
        return i18n::current() == "en" ? juce::String ("Bass") : tr ("Bass guitar");
    return tr (u8 (c));
}

juce::Colour accentColour() { return skin::colour::azure; } // (sampler-like generators: azure)

// What the editor edits: a Channel Rack channel, or a LYNX Soundfont node inside a Patcher (round 165c: found through
// `locate`, channel −1 — no automation target, the preview note is rendered off-line instead of played on a channel)
struct SfTarget
{
    std::function<ChannelDef* (Song&)> locate;
    int channel = -1;

    static SfTarget rack (int c)
    {
        return { [c] (Song& s) -> ChannelDef* { return c >= 0 && c < (int) s.channels.size() ? &s.channels[(std::size_t) c] : nullptr; }, c };
    }
};

const ChannelDef* channelOf (AudioEngine& engine, const SfTarget& target)
{
    const auto* c = target.locate ? target.locate (const_cast<Song&> (engine.getSong())) : nullptr;
    return c != nullptr && c->instrument == InstrumentKind::soundfont ? c : nullptr;
}

float paramOf (AudioEngine& engine, const SfTarget& target, int index)
{
    if (const auto* c = channelOf (engine, target))
        return sf::readParams (c->modelParams)[(std::size_t) index];
    return sf::paramInfo (index).def;
}

void editTarget (AudioEngine& engine, const SfTarget& target, std::function<void (ChannelDef&)> change, bool undoable = true)
{
    engine.editSong ([locate = target.locate, change = std::move (change)] (Song& s) {
        auto* c = locate ? locate (s) : nullptr;
        if (c != nullptr && c->instrument == InstrumentKind::soundfont)
            change (*c);
    }, undoable);
}

void setParam (AudioEngine& engine, const SfTarget& target, int index, float value, bool undoable = true)
{
    editTarget (engine, target, [index, value] (ChannelDef& c) {
        auto params = sf::readParams (c.modelParams);
        params[(std::size_t) index] = value;
        c.modelParams.assign (params.begin(), params.end());
    }, undoable);
}

void loadInto (AudioEngine& engine, const SfTarget& target, const std::string& path, int bank, int program)
{
    editTarget (engine, target, [path, bank, program] (ChannelDef& c) {
        c.samplePath = path;
        auto params = sf::readParams (c.modelParams);
        params[sf::pBank] = (float) juce::jlimit (0, 128, bank);
        params[sf::pProgram] = (float) juce::jlimit (0, 127, program);
        c.modelParams.assign (params.begin(), params.end());
    });
}

ui::ValueSpec specFor (const SfTarget& target, int index, const juce::String& channelName)
{
    const auto& info = sf::paramInfo (index);
    ui::ValueSpec spec;
    spec.name = channelName + " - " + tr (info.name);
    spec.minimum = info.min;
    spec.maximum = info.max;
    spec.defaultValue = info.def;
    spec.interval = info.integer ? 1.0 : 0.0;
    spec.pixelsForRange = info.integer ? juce::jlimit (60.0, 300.0, 6.0 * (info.max - info.min)) : 150.0;
    spec.toText = [index] (double v) { return shownValue (index, (float) v); };
    spec.fromText = ui::inverseOf (spec.toText, info.min, info.max);
    if (target.channel >= 0 && index != sf::pBank && index != sf::pProgram && index != sf::pReverbTrack && index != sf::pChorusTrack)
        spec.target = AutomationTarget { AutomationTargetKind::channelParam, target.channel, 0, index };
    return spec;
}

juce::String channelLabel (AudioEngine& engine, const SfTarget& target)
{
    if (const auto* c = channelOf (engine, target); c != nullptr && ! c->name.empty())
        return u8 (c->name);
    return "LYNX Soundfont";
}

// --- controls ----------------------------------------------------------------------------------------------------------

// A knob of one parameter, drawn with the plugin skin; LYNX's control contract (drag, wheel, reset, value menu)
class SfKnob final : public juce::Component, public ui::HintSource
{
public:
    SfKnob (AudioEngine& e, SfTarget c, int p, juce::String caption) : engine (e), channel (c), param (p), label (std::move (caption))
    {
        setRepaintsOnMouseActivity (true);
    }

    ui::ValueSpec spec() const { return specFor (channel, param, channelLabel (engine, channel)); }
    double value() const { return paramOf (engine, channel, param); }
    void set (double v) { setParam (engine, channel, param, (float) v); repaint(); }

    ui::Hint hintAt (juce::Point<int>) override { return ui::valueHint (spec(), value(), ui::valueGestures()); }

    void paint (juce::Graphics& g) override
    {
        const auto s = spec();
        auto area = getLocalBounds().toFloat();
        const auto caption = area.removeFromBottom (13.0f);
        const auto valueArea = area.removeFromBottom (12.0f);
        const auto size = juce::jmin (area.getWidth(), area.getHeight());
        skin::KnobLook look;
        look.value = (float) juce::jlimit (0.0, 1.0, (value() - s.minimum) / juce::jmax (1e-9, s.maximum - s.minimum));
        look.bipolar = param == sf::pLfoAmount && value() > 0.0;
        look.accent = accentColour();
        look.hover = isMouseOver() ? 1.0f : 0.0f;
        look.active = drag.isActive();
        skin::knob (g, juce::Rectangle<float> (size, size).withCentre (area.getCentre()), look);
        g.setFont (theme::font (10.0f));
        g.setColour (skin::valueColour (isMouseOverOrDragging()));
        g.drawFittedText (s.text (value()), valueArea.toNearestInt(), juce::Justification::centred, 1, 0.7f);
        g.setColour (skin::captionColour());
        g.setFont (theme::font (9.5f, true));
        g.drawFittedText (theme::caps (label), caption.toNearestInt(), juce::Justification::centred, 1, 0.7f);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, spec(), value(), [this] (double v) { set (v); });
            return;
        }
        if (ui::isResetClick (e))
        {
            set (spec().defaultValue);
            return;
        }
        drag.begin (e, value());
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! drag.isActive())
            return;
        const auto s = spec();
        set (drag.update (e, s));
        ui::reportTweak (s);
        ui::showValueTip (*this, getLocalBounds(), s.text (value()));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        drag.end();
        ui::hideValueTip();
    }
    void mouseDoubleClick (const juce::MouseEvent&) override { set (spec().defaultValue); }
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (const auto v = ui::wheelValue (value(), e, w, spec()))
        {
            set (*v);
            ui::flashValueTip (*this, getLocalBounds(), spec().text (value()));
        }
    }

private:
    AudioEngine& engine;
    SfTarget channel;
    int param;
    juce::String label;
    ui::ValueDrag drag;
};

// An on / off switch of one parameter
class SfToggle final : public juce::Component, public juce::SettableTooltipClient
{
public:
    SfToggle (AudioEngine& e, SfTarget c, int p, juce::String caption, juce::String tip) : engine (e), channel (c), param (p), label (std::move (caption))
    {
        setTooltip (tip);
        setRepaintsOnMouseActivity (true);
    }
    void paint (juce::Graphics& g) override
    {
        const auto on = paramOf (engine, channel, param) >= 0.5f;
        auto r = getLocalBounds().toFloat();
        skin::switchPill (g, r.removeFromLeft (26.0f).withSizeKeepingCentre (24.0f, 13.0f), on, accentColour(), true);
        r.removeFromLeft (5.0f);
        g.setColour (on ? theme::colour::textPrimary : theme::colour::textSecondary);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (label, r.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
    void mouseDown (const juce::MouseEvent&) override
    {
        setParam (engine, channel, param, paramOf (engine, channel, param) >= 0.5f ? 0.0f : 1.0f);
        repaint();
    }

private:
    AudioEngine& engine;
    SfTarget channel;
    int param;
    juce::String label;
};

// A selector of one integer parameter (a menu of its values)
class SfChoice final : public juce::Component, public juce::SettableTooltipClient
{
public:
    SfChoice (AudioEngine& e, SfTarget c, int p, juce::String caption, std::function<juce::PopupMenu (std::function<void (int)>)> custom = {})
        : engine (e), channel (c), param (p), label (std::move (caption)), customMenu (std::move (custom))
    {
        setRepaintsOnMouseActivity (true);
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto cap = r.removeFromTop (12.0f);
        g.setColour (skin::captionColour());
        g.setFont (theme::font (9.5f, true));
        g.drawFittedText (theme::caps (label), cap.toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
        skin::selector (g, r.reduced (0.0f, 1.0f), isMouseOver());
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (shownValue (param, paramOf (engine, channel, param)) + juce::String::fromUTF8 ("  \xe2\x96\xbe"),
                          r.reduced (6.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);
    }
    void mouseDown (const juce::MouseEvent&) override
    {
        const auto pick = [this] (int v) {
            setParam (engine, channel, param, (float) v);
            repaint();
        };
        juce::PopupMenu menu;
        if (customMenu)
            menu = customMenu (pick);
        else
        {
            const auto& info = sf::paramInfo (param);
            const auto current = (int) paramOf (engine, channel, param);
            for (int v = (int) info.min; v <= (int) info.max; ++v)
                menu.addItem (shownValue (param, (float) v), true, v == current, [pick, v] { pick (v); });
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

private:
    AudioEngine& engine;
    SfTarget channel;
    int param;
    juce::String label;
    std::function<juce::PopupMenu (std::function<void (int)>)> customMenu;
};

// --- the preset lists --------------------------------------------------------------------------------------------------

struct Row
{
    std::string path, font, name, category;
    int bank = 0, program = 0;
};

class PresetList final : public juce::ListBox, private juce::ListBoxModel
{
public:
    std::function<void (const Row&)> onPreview, onLoad;
    std::function<void (const Row&)> onMenu;
    bool showFont = false;

    PresetList()
    {
        setModel (this);
        setRowHeight (20);
        setColour (juce::ListBox::backgroundColourId, skin::colour::wellTop);
        setColour (juce::ListBox::outlineColourId, theme::colour::border);
        setOutlineThickness (1);
        setMultipleSelectionEnabled (false);
    }

    void setRows (std::vector<Row> r, const std::string& currentPath, int bank, int program)
    {
        rows = std::move (r);
        updateContent();
        int select = -1;
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].path == currentPath && rows[i].bank == bank && rows[i].program == program)
                select = (int) i;
        if (select >= 0)
            selectRow (select, true, true);
        else
            deselectAllRows();
        repaint();
    }

    const std::vector<Row>& all() const { return rows; }
    int rowCount() const { return (int) rows.size(); }

private:
    int getNumRows() override { return (int) rows.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        const auto& r = rows[(std::size_t) row];
        if (selected)
            g.fillAll (accentColour().withAlpha (0.28f));
        else if (row % 2 == 1)
            g.fillAll (juce::Colours::white.withAlpha (0.02f));
        auto area = juce::Rectangle<int> (0, 0, width, height).reduced (6, 0);
        g.setFont (theme::monoFont (11.0f));
        g.setColour (theme::colour::textSecondary);
        g.drawText (juce::String::formatted ("%03d:%03d", r.bank, r.program), area.removeFromLeft (58), juce::Justification::centredLeft, false);
        const auto catWidth = juce::jmin (110, width / 4);
        auto cat = area.removeFromRight (catWidth);
        g.setFont (theme::font (10.5f));
        g.setColour (theme::colour::textDisabled);
        g.drawFittedText (categoryText (r.category), cat, juce::Justification::centredRight, 1, 0.7f);
        g.setColour (selected ? theme::colour::textPrimary : skin::valueColour (false));
        g.setFont (theme::font (12.0f));
        if (showFont)
        {
            const auto nameWidth = area.getWidth() * 3 / 5;
            g.drawFittedText (u8 (r.name), area.removeFromLeft (nameWidth), juce::Justification::centredLeft, 1, 0.75f);
            g.setColour (theme::colour::textDisabled);
            g.setFont (theme::font (10.5f));
            g.drawFittedText (u8 (r.font), area.reduced (4, 0), juce::Justification::centredLeft, 1, 0.7f);
        }
        else
            g.drawFittedText (u8 (r.name), area, juce::Justification::centredLeft, 1, 0.75f);
    }

    void listBoxItemClicked (int row, const juce::MouseEvent& e) override
    {
        if (row < 0 || row >= (int) rows.size())
            return;
        if (e.mods.isPopupMenu())
        {
            if (onMenu)
                onMenu (rows[(std::size_t) row]);
            return;
        }
        if (onPreview)
            onPreview (rows[(std::size_t) row]);
    }

    void load (int row)
    {
        if (row >= 0 && row < (int) rows.size() && onLoad)
            onLoad (rows[(std::size_t) row]);
    }
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override { load (row); }
    void returnKeyPressed (int row) override { load (row); }

    juce::var getDragSourceDescription (const juce::SparseSet<int>& selected) override
    {
        if (selected.size() == 0 || selected[0] >= (int) rows.size())
            return {};
        const auto& r = rows[(std::size_t) selected[0]];
        return soundFontDragDescription (r.path, r.bank, r.program);
    }

    juce::String getTooltipForRow (int row) override
    {
        if (row < 0 || row >= (int) rows.size())
            return {};
        const auto& r = rows[(std::size_t) row];
        return u8 (r.name) + " (" + u8 (r.font) + ", " + juce::String (r.bank) + ":" + juce::String (r.program) + ")\n"
               + tr ("Click: preview. Double-click: load into this channel. Drag onto the Channel rack: a new channel.");
    }

    std::vector<Row> rows;
};

// --- the editor ----------------------------------------------------------------------------------------------------

class SoundFontEditor final : public juce::Component,
                              public juce::DragAndDropContainer,
                              private juce::Timer
{
public:
    SoundFontEditor (AudioEngine& e, SfTarget c) : engine (e), channel (std::move (c))
    {
        ensureSoundFontIndex();

        load.setButtonText (tr ("Load SoundFont..."));
        load.setTooltip (tr ("Open a .sf2 / .sf3 file"));
        load.onClick = [this] { chooseFont(); };
        addAndMakeVisible (load);

        for (auto* b : { &prev, &next })
        {
            b->setTooltip (b == &prev ? tr ("Previous preset") : tr ("Next preset"));
            b->onClick = [this, b] { stepPreset (b == &prev ? -1 : 1); };
            addAndMakeVisible (*b);
        }
        prev.setButtonText (juce::String::fromUTF8 ("\xe2\x97\x80"));
        next.setButtonText (juce::String::fromUTF8 ("\xe2\x96\xb6"));
        patch.setTooltip (tr ("The preset (bank:patch): click for the list of this SoundFont's presets"));
        patch.onClick = [this] { showPatchMenu(); };
        addAndMakeVisible (patch);

        const auto knob = [this] (int p, const char* caption) {
            knobs.push_back (std::make_unique<SfKnob> (engine, channel, p, tr (caption)));
            addAndMakeVisible (*knobs.back());
            return knobs.back().get();
        };
        attack = knob (sf::pAttack, TR_NOOP ("Attack"));
        decay = knob (sf::pDecay, TR_NOOP ("Decay"));
        sustain = knob (sf::pSustain, TR_NOOP ("Sustain"));
        release = knob (sf::pRelease, TR_NOOP ("Release"));
        lfoDelay = knob (sf::pLfoDelay, TR_NOOP ("Delay"));
        lfoAmount = knob (sf::pLfoAmount, TR_NOOP ("Amount"));
        lfoSpeed = knob (sf::pLfoSpeed, TR_NOOP ("Speed"));
        cutoff = knob (sf::pCutoff, TR_NOOP ("Cutoff"));
        cutMod = knob (sf::pCutoffMod, TR_NOOP ("Env mod"));
        resonance = knob (sf::pResonance, TR_NOOP ("Resonance"));
        reverbSend = knob (sf::pReverbAmount, TR_NOOP ("Level"));
        chorusSend = knob (sf::pChorusAmount, TR_NOOP ("Level"));
        volume = knob (sf::pVolume, TR_NOOP ("Volume"));
        bend = knob (sf::pBendRange, TR_NOOP ("Bend range"));
        poly = knob (sf::pPolyphony, TR_NOOP ("Polyphony"));
        transpose = knob (sf::pTranspose, TR_NOOP ("Transpose"));
        modWheel = knob (sf::pModWheel, TR_NOOP ("Mod wheel"));

        const auto toggle = [this] (int p, const char* caption, const char* tip) {
            toggles.push_back (std::make_unique<SfToggle> (engine, channel, p, tr (caption), tr (tip)));
            addAndMakeVisible (*toggles.back());
            return toggles.back().get();
        };
        reverbOverride = toggle (sf::pReverbOverride, TR_NOOP ("Level"), TR_NOOP ("On: the knob scales the preset's send (100 % = as the preset); off: the preset's own send"));
        reverbBuiltIn = toggle (sf::pReverbBuiltIn, TR_NOOP ("Built-in"), TR_NOOP ("Use the built-in reverb"));
        chorusOverride = toggle (sf::pChorusOverride, TR_NOOP ("Level"), TR_NOOP ("On: the knob scales the preset's send (100 % = as the preset); off: the preset's own send"));
        chorusBuiltIn = toggle (sf::pChorusBuiltIn, TR_NOOP ("Built-in"), TR_NOOP ("Use the built-in chorus"));
        velFilter = toggle (sf::pVelocityToCutoff, TR_NOOP ("Velocity to cutoff"), TR_NOOP ("Softer notes are darker (the SoundFont default modulator)"));
        emu = toggle (sf::pEmuEnvelope, TR_NOOP ("EMU volume envelope"), TR_NOOP ("The attack of EMU sound cards (curved in decibels)"));
        loops = toggle (sf::pUseLoops, TR_NOOP ("Use loops"), TR_NOOP ("Play the samples' loops (off: every sample plays once)"));

        const auto choice = [this] (int p, const char* caption) {
            choices.push_back (std::make_unique<SfChoice> (engine, channel, p, tr (caption)));
            addAndMakeVisible (*choices.back());
            return choices.back().get();
        };
        resampling = choice (sf::pResampling, TR_NOOP ("Resampling"));
        // "send to" a mixer insert (FL: SEND TO): the dry send into that insert, for a reverb of the user's own
        const auto sendMenu = [this] (int p) {
            return [this, p] (std::function<void (int)> pick) {
                juce::PopupMenu menu;
                const auto current = (int) paramOf (engine, channel, p);
                menu.addItem (tr ("None"), true, current < 0, [pick] { pick (-1); });
                const auto& song = engine.getSong();
                for (int t = 0; t < juce::jmin (126, (int) song.mixer.size()); ++t)
                {
                    const auto name = t == 0 ? tr ("Master")
                                             : song.mixer[(std::size_t) t].name.empty() ? tr ("Insert {0}", { juce::String (t) })
                                                                                        : juce::String (t) + ": " + juce::String::fromUTF8 (song.mixer[(std::size_t) t].name.c_str());
                    menu.addItem (name, true, current == t, [pick, t] { pick (t); });
                }
                return menu;
            };
        };
        for (auto [p, slot] : { std::pair { sf::pReverbTrack, &reverbTrack }, std::pair { sf::pChorusTrack, &chorusTrack } })
        {
            choices.push_back (std::make_unique<SfChoice> (engine, channel, p, tr ("Send to"), sendMenu (p)));
            choices.back()->setTooltip (tr ("Also send the effect's input to a mixer insert (put your own reverb or chorus there)"));
            addAndMakeVisible (*choices.back());
            *slot = choices.back().get();
        }
        mode = choice (sf::pProgramMode, TR_NOOP ("Program mode"));
        curve = choice (sf::pVelocityCurve, TR_NOOP ("Velocity curve"));
        resampling->setTooltip (tr ("How transposed samples are interpolated: linear (crunchy) to 16-point sinc (extreme HQ)"));
        mode->setTooltip (tr ("Automatic: drum kits (bank 128) play their samples out; Instrument: notes stop at the note-off; Drums: one-shot"));
        curve->setTooltip (tr ("How velocity sets the level (SoundFont: the standard curve)"));

        // presets: this SoundFont / search everything
        for (auto* b : { &fontTab, &searchTab })
        {
            b->setClickingTogglesState (true);
            b->setRadioGroupId (158);
            b->onClick = [this] { showList (searchTab.getToggleState()); };
            addAndMakeVisible (*b);
        }
        fontTab.setButtonText (tr ("This SoundFont"));
        searchTab.setButtonText (tr ("Search all"));
        searchTab.setTooltip (tr ("Every preset of every SoundFont in your folders"));
        search.setTextToShowWhenEmpty (tr ("Search presets: piano, 808, pad..."), theme::colour::textDisabled);
        search.onTextChange = [this] {
            if (! searchTab.getToggleState() && search.getText().isNotEmpty())
                searchTab.setToggleState (true, juce::sendNotificationSync);
            refreshLists();
        };
        search.onReturnKey = [this] {
            if (list.rowCount() > 0)
            {
                list.selectRow (0);
                list.grabKeyboardFocus();
            }
        };
        addAndMakeVisible (search);
        folders.setButtonText (tr ("Folders..."));
        folders.setTooltip (tr ("The folders searched for SoundFonts (and a rescan)"));
        folders.onClick = [this] { showFoldersMenu(); };
        addAndMakeVisible (folders);
        status.setFont (theme::font (10.5f));
        status.setColour (juce::Label::textColourId, theme::colour::textDisabled);
        addAndMakeVisible (status);

        list.onPreview = [this] (const Row& r) {
            if (r.path == currentPath())
            {
                // this font: the channel takes the preset (as FL's patch selector), and a short note plays
                loadInto (engine, channel, r.path, r.bank, r.program);
                juce::Timer::callAfterDelay (60, [safe = juce::Component::SafePointer<SoundFontEditor> (this)] {
                    if (safe != nullptr)
                        safe->playNote();
                });
            }
            else
                previewSoundFontPreset (engine, r.path, r.bank, r.program);
            refreshHeader();
        };
        list.onLoad = [this] (const Row& r) {
            loadInto (engine, channel, r.path, r.bank, r.program);
            refreshAll();
        };
        list.onMenu = [this] (const Row& r) {
            juce::PopupMenu m;
            m.addItem (channel.channel >= 0 ? tr ("Load into this channel") : tr ("Load into this node"), [this, r] {
                loadInto (engine, channel, r.path, r.bank, r.program);
                refreshAll();
            });
            m.addItem (tr ("Load into a new channel"), [this, r] { addSoundFontChannel (engine, r.path, r.bank, r.program); });
            m.addItem (tr ("Preview"), [this, r] { previewSoundFontPreset (engine, r.path, r.bank, r.program); });
            m.addSeparator();
            m.addItem (tr ("Show in folder"), [r] { juce::File (u8 (r.path)).revealToUser(); });
            m.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
        };
        addAndMakeVisible (list);

        fontTab.setToggleState (true, juce::dontSendNotification);
        setSize (800, 500);
        refreshAll();
        startTimerHz (5);
    }

    ~SoundFontEditor() override
    {
        stopTimer();
        if (noteKey >= 0)
            noteOff();
    }

    void paint (juce::Graphics& g) override
    {
        const auto accent = accentColour();
        skin::canvas (g, getLocalBounds().toFloat(), accent);
        // title + description line
        auto top = getLocalBounds().reduced (12, 0).removeFromTop (descriptionBottom).toFloat();
        top.removeFromTop (headerHeight);
        g.setColour (theme::colour::textSecondary);
        g.setFont (theme::font (11.0f));
        g.drawFittedText (tr ("LYNX Soundfont: plays SoundFont 2 / 3 banks. Search presets across all your SoundFonts, click to preview, double-click to load."),
                          top.toNearestInt(), juce::Justification::centredLeft, 1, 0.75f);
        // the font's name next to the load button
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText (fontTitle, fontNameArea, juce::Justification::centredLeft, 1, 0.7f);

        for (const auto& [area, title] : sections)
        {
            skin::section (g, area, theme::radius::lg, false, accent);
            skin::sectionTitle (g, area.withHeight (16.0f).translated (0.0f, 1.0f), theme::caps (title), accent);
        }
        skin::well (g, listArea.toFloat().expanded (2.0f), theme::radius::lg, accent);
    }

    void resized() override
    {
        sections.clear();
        auto area = getLocalBounds().reduced (12, 8);
        auto header = area.removeFromTop (headerHeight - 8);
        load.setBounds (header.removeFromLeft (130).reduced (0, 4));
        header.removeFromLeft (8);
        next.setBounds (header.removeFromRight (26).reduced (0, 4));
        patch.setBounds (header.removeFromRight (juce::jmin (260, header.getWidth() / 2)).reduced (2, 4));
        prev.setBounds (header.removeFromRight (26).reduced (0, 4));
        fontNameArea = header.reduced (4, 0);
        area.removeFromTop (descriptionBottom - headerHeight);
        area.removeFromTop (4);

        // controls on the left, presets on the right (below each other when narrow)
        const auto narrow = getWidth() < 700;
        auto controls = narrow ? area.removeFromTop (juce::jmin (area.getHeight() / 2 + 120, 380)) : area.removeFromLeft (juce::jmin (470, area.getWidth() * 3 / 5));
        if (! narrow)
            area.removeFromLeft (10);
        else
            area.removeFromTop (8);
        layoutControls (controls);
        layoutPresets (area);
    }

private:
    static constexpr int headerHeight = 40, descriptionBottom = 58;

    void layoutControls (juce::Rectangle<int> r)
    {
        const auto rowH = juce::jlimit (70, 96, (r.getHeight() - 70) / 3);
        const auto half = [] (juce::Rectangle<int>& row) {
            auto left = row.removeFromLeft (row.getWidth() / 2 - 4);
            row.removeFromLeft (8);
            return left;
        };
        const auto place = [this] (juce::Rectangle<int> box, const juce::String& title, std::initializer_list<juce::Component*> items) {
            sections.push_back ({ box.toFloat(), title });
            auto inner = box.reduced (6, 0).withTrimmedTop (18).withTrimmedBottom (4);
            const auto n = (int) items.size();
            const auto w = inner.getWidth() / juce::jmax (1, n);
            for (auto* c : items)
                c->setBounds (inner.removeFromLeft (w).reduced (2, 0));
        };
        auto row1 = r.removeFromTop (rowH);
        place (half (row1), tr ("Envelope"), { attack, decay, sustain, release });
        place (row1, tr ("Pitch LFO"), { lfoDelay, lfoAmount, lfoSpeed, modWheel });
        r.removeFromTop (6);
        auto row2 = r.removeFromTop (rowH);
        place (half (row2), tr ("Filter"), { cutoff, cutMod, resonance });
        place (row2, tr ("Voice"), { volume, bend, poly, transpose });
        r.removeFromTop (6);
        auto row3 = r.removeFromTop (rowH);
        const auto fx = [this] (juce::Rectangle<int> box, const juce::String& title, juce::Component* send, juce::Component* over, juce::Component* builtIn, juce::Component* track) {
            sections.push_back ({ box.toFloat(), title });
            auto inner = box.reduced (6, 0).withTrimmedTop (18).withTrimmedBottom (4);
            send->setBounds (inner.removeFromLeft (juce::jmin (64, inner.getWidth() / 3)));
            inner.removeFromLeft (4);
            auto boxes = inner.removeFromLeft (inner.getWidth() / 2);
            const auto h = juce::jmin (22, boxes.getHeight() / 2);
            over->setBounds (boxes.removeFromTop (h));
            builtIn->setBounds (boxes.removeFromTop (h));
            inner.removeFromLeft (4);
            track->setBounds (inner.withSizeKeepingCentre (inner.getWidth(), juce::jmin (34, inner.getHeight())));
        };
        fx (half (row3), tr ("Reverb"), reverbSend, reverbOverride, reverbBuiltIn, reverbTrack);
        fx (row3, tr ("Chorus"), chorusSend, chorusOverride, chorusBuiltIn, chorusTrack);
        r.removeFromTop (6);
        // options
        sections.push_back ({ r.toFloat(), tr ("Options") });
        auto inner = r.reduced (8, 0).withTrimmedTop (18).withTrimmedBottom (4);
        auto line = inner.removeFromTop (juce::jmin (34, inner.getHeight() / 2));
        const auto w = line.getWidth() / 3;
        resampling->setBounds (line.removeFromLeft (w).reduced (3, 0));
        mode->setBounds (line.removeFromLeft (w).reduced (3, 0));
        curve->setBounds (line.reduced (3, 0));
        inner.removeFromTop (2);
        const auto tw = inner.getWidth() / 3;
        const auto th = juce::jmin (20, inner.getHeight());
        velFilter->setBounds (inner.removeFromLeft (tw).withHeight (th).reduced (3, 0));
        emu->setBounds (inner.removeFromLeft (tw).withHeight (th).reduced (3, 0));
        loops->setBounds (inner.withHeight (th).reduced (3, 0));
    }

    void layoutPresets (juce::Rectangle<int> r)
    {
        auto tabs = r.removeFromTop (24);
        fontTab.setBounds (tabs.removeFromLeft (tabs.getWidth() / 2).reduced (1, 0));
        searchTab.setBounds (tabs.reduced (1, 0));
        r.removeFromTop (6);
        auto bar = r.removeFromTop (24);
        folders.setBounds (bar.removeFromRight (80));
        bar.removeFromRight (4);
        search.setBounds (bar);
        r.removeFromTop (6);
        status.setBounds (r.removeFromBottom (18));
        r.removeFromBottom (4);
        listArea = r;
        list.setBounds (r);
    }

    void noteOff()
    {
        if (noteKey >= 0 && channel.channel >= 0)
            engine.sendLiveNote (channel.channel, noteKey, 0.0f, false);
    }

    std::string currentPath() const
    {
        if (const auto* c = channelOf (engine, channel))
            return c->samplePath;
        return {};
    }

    void chooseFont()
    {
        const auto start = currentPath().empty() ? juce::File::getSpecialLocation (juce::File::userDocumentsDirectory) : juce::File (u8 (currentPath()));
        chooser = std::make_unique<juce::FileChooser> (tr ("Load SoundFont"), start, "*.sf2;*.sf3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [safe = juce::Component::SafePointer<SoundFontEditor> (this)] (const juce::FileChooser& fc) {
                                  if (safe == nullptr || fc.getResult() == juce::File())
                                      return;
                                  const auto path = std8 (fc.getResult().getFullPathName());
                                  int bank = 0, program = 0;
                                  if (! firstSoundFontPreset (path, bank, program))
                                  {
                                      juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Load SoundFont"),
                                                                              tr ("This file is not a SoundFont LYNX SHARE can read."));
                                      return;
                                  }
                                  loadInto (safe->engine, safe->channel, path, bank, program);
                                  sf::PresetIndex::shared().addFont (path);
                                  safe->refreshAll();
                              });
    }

    void stepPreset (int delta)
    {
        if (fontRows.empty())
            return;
        const auto bank = (int) paramOf (engine, channel, sf::pBank), program = (int) paramOf (engine, channel, sf::pProgram);
        int at = 0;
        for (std::size_t i = 0; i < fontRows.size(); ++i)
            if (fontRows[i].bank == bank && fontRows[i].program == program)
                at = (int) i;
        at = (at + delta + (int) fontRows.size()) % (int) fontRows.size();
        loadInto (engine, channel, currentPath(), fontRows[(std::size_t) at].bank, fontRows[(std::size_t) at].program);
        refreshAll();
    }

    void showPatchMenu()
    {
        juce::PopupMenu menu;
        const auto bank = (int) paramOf (engine, channel, sf::pBank), program = (int) paramOf (engine, channel, sf::pProgram);
        if (fontRows.empty())
            menu.addItem (tr ("(no SoundFont loaded)"), false, false, nullptr);
        for (const auto& r : fontRows)
            menu.addItem (juce::String::formatted ("%03d:%03d  ", r.bank, r.program) + u8 (r.name), true, r.bank == bank && r.program == program,
                          [this, r] {
                              loadInto (engine, channel, r.path, r.bank, r.program);
                              refreshAll();
                          });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patch).withMaximumNumColumns (3));
    }

    void showFoldersMenu()
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("SoundFont folders"));
        for (const auto& f : soundFontFolders())
            menu.addItem (u8 (f), false, false, nullptr);
        menu.addSeparator();
        menu.addItem (tr ("Add folder..."), [this] {
            chooser = std::make_unique<juce::FileChooser> (tr ("Add a SoundFont folder"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [] (const juce::FileChooser& fc) {
                                      if (fc.getResult() == juce::File())
                                          return;
                                      auto lines = juce::StringArray::fromLines (soundFontFoldersFile().loadFileAsString());
                                      lines.removeEmptyStrings();
                                      lines.addIfNotAlreadyThere (fc.getResult().getFullPathName());
                                      soundFontFoldersFile().getParentDirectory().createDirectory();
                                      soundFontFoldersFile().replaceWithText (lines.joinIntoString ("\n") + "\n");
                                      ensureSoundFontIndex (true);
                                  });
        });
        juce::PopupMenu remove;
        for (const auto& line : juce::StringArray::fromLines (soundFontFoldersFile().loadFileAsString()))
            if (line.trim().isNotEmpty())
                remove.addItem (line.trim(), [line] {
                    auto lines = juce::StringArray::fromLines (soundFontFoldersFile().loadFileAsString());
                    lines.removeString (line);
                    lines.removeEmptyStrings();
                    soundFontFoldersFile().replaceWithText (lines.joinIntoString ("\n") + "\n");
                    ensureSoundFontIndex (true);
                });
        menu.addSubMenu (tr ("Remove folder"), remove, remove.getNumItems() > 0);
        menu.addItem (tr ("Rescan now"), [] { ensureSoundFontIndex (true); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&folders));
    }

    void playNote()
    {
        const auto drums = (int) paramOf (engine, channel, sf::pBank) == 128;
        if (noteKey >= 0)
            noteOff();
        if (channel.channel < 0) // a Patcher node: the note is rendered off-line (the node plays only what is cabled to it)
        {
            previewSoundFontPreset (engine, currentPath(), (int) paramOf (engine, channel, sf::pBank), (int) paramOf (engine, channel, sf::pProgram));
            return;
        }
        noteKey = drums ? 38 : 60;
        engine.sendLiveNote (channel.channel, noteKey, 0.8f, true);
        noteOffAt = juce::Time::getMillisecondCounter() + 450;
    }

    void showList (bool all)
    {
        searchMode = all;
        list.showFont = all;
        refreshLists();
    }

    void refreshHeader()
    {
        const auto path = currentPath();
        const auto bank = (int) paramOf (engine, channel, sf::pBank), program = (int) paramOf (engine, channel, sf::pProgram);
        juce::String name;
        for (const auto& r : fontRows)
            if (r.bank == bank && r.program == program)
                name = u8 (r.name);
        patch.setButtonText (juce::String::formatted ("%03d:%03d  ", bank, program) + (name.isEmpty() ? juce::String ("--") : name));
        if (path.empty())
            fontTitle = tr ("No SoundFont loaded");
        else if (handle != nullptr && handle->finished() && handle->font() == nullptr)
            fontTitle = tr ("{0} (could not be loaded: {1})", { juce::File (u8 (path)).getFileName(), tr (u8 (handle->error())) });
        else if (handle != nullptr && ! handle->finished())
            fontTitle = tr ("{0} (loading...)", { juce::File (u8 (path)).getFileName() });
        else
            fontTitle = handle != nullptr && handle->font() != nullptr && ! handle->font()->name.empty()
                            ? u8 (handle->font()->name) + "  (" + juce::File (u8 (path)).getFileName() + ")"
                            : juce::File (u8 (path)).getFileName();
        repaint (fontNameArea);
    }

    void refreshLists()
    {
        const auto path = currentPath();
        const auto bank = (int) paramOf (engine, channel, sf::pBank), program = (int) paramOf (engine, channel, sf::pProgram);
        if (! searchMode)
        {
            auto rows = fontRows;
            const auto query = std8 (search.getText());
            if (! query.empty())
            {
                std::vector<Row> kept;
                const auto q = search::parse (query);
                for (const auto& r : rows)
                    if (search::matches (q, r.name + " | " + r.category))
                        kept.push_back (r);
                rows = std::move (kept);
            }
            list.setRows (std::move (rows), path, bank, program);
        }
        else
        {
            std::vector<Row> rows;
            for (const auto& h : sf::PresetIndex::shared().search (std8 (search.getText()), 2000))
                rows.push_back ({ h.path, h.fontName, h.presetName, h.category, h.bank, h.program });
            list.setRows (std::move (rows), path, bank, program);
        }
        const auto& index = sf::PresetIndex::shared();
        const auto fonts = index.fonts().size();
        status.setText (searchMode ? tr ("{0} presets in {1} SoundFonts", { juce::String ((int) index.presetCount()), juce::String ((int) fonts) })
                                         + (index.busy() ? tr (" (indexing...)") : juce::String()) + "  -  "
                                         + tr ("{0} shown", { juce::String (list.rowCount()) })
                                   : tr ("{0} presets", { juce::String ((int) fontRows.size()) }),
                        juce::dontSendNotification);
    }

    void refreshAll()
    {
        const auto path = currentPath();
        if (path != loadedPath)
        {
            loadedPath = path;
            handle = path.empty() ? nullptr : sf::FontLibrary::instance().get (path);
            fontRows.clear();
            rowsFromFont = false;
        }
        if (! rowsFromFont && handle != nullptr && handle->font() != nullptr)
        {
            fontRows.clear();
            for (const auto& p : handle->font()->presets)
                fontRows.push_back ({ path, handle->font()->name, p.name, sf::category (p.bank, p.program, p.name), p.bank, p.program });
            rowsFromFont = true;
        }
        refreshHeader();
        refreshLists();
        for (auto* c : getChildren())
            c->repaint();
    }

    void timerCallback() override
    {
        if (noteKey >= 0 && juce::Time::getMillisecondCounter() >= noteOffAt)
        {
            noteOff();
            noteKey = -1;
        }
        if (channelOf (engine, channel) == nullptr)
            return;
        const auto gen = sf::PresetIndex::shared().generation();
        const auto loading = handle != nullptr && ! rowsFromFont && handle->finished();
        const auto busy = sf::PresetIndex::shared().busy();
        if (gen != lastGeneration || loading || currentPath() != loadedPath || busy != wasBusy
            || (paramOf (engine, channel, sf::pBank) != lastBank || paramOf (engine, channel, sf::pProgram) != lastProgram))
        {
            lastGeneration = gen;
            wasBusy = busy;
            lastBank = paramOf (engine, channel, sf::pBank), lastProgram = paramOf (engine, channel, sf::pProgram);
            refreshAll();
        }
    }

    AudioEngine& engine;
    SfTarget channel;
    juce::TextButton load, prev, next, patch, fontTab, searchTab, folders;
    juce::TextEditor search;
    juce::Label status;
    PresetList list;
    std::vector<std::unique_ptr<SfKnob>> knobs;
    std::vector<std::unique_ptr<SfToggle>> toggles;
    std::vector<std::unique_ptr<SfChoice>> choices;
    SfKnob *attack {}, *decay {}, *sustain {}, *release {}, *lfoDelay {}, *lfoAmount {}, *lfoSpeed {}, *cutoff {}, *cutMod {}, *resonance {},
        *reverbSend {}, *chorusSend {}, *volume {}, *bend {}, *poly {}, *transpose {}, *modWheel {};
    SfChoice *reverbTrack {}, *chorusTrack {};
    SfToggle *reverbOverride {}, *reverbBuiltIn {}, *chorusOverride {}, *chorusBuiltIn {}, *velFilter {}, *emu {}, *loops {};
    SfChoice *resampling {}, *mode {}, *curve {};
    std::vector<std::pair<juce::Rectangle<float>, juce::String>> sections;
    juce::Rectangle<int> fontNameArea, listArea;
    juce::String fontTitle;
    std::unique_ptr<juce::FileChooser> chooser;
    std::shared_ptr<sf::FontHandle> handle;
    std::string loadedPath;
    std::vector<Row> fontRows;
    bool rowsFromFont = false, searchMode = false, wasBusy = false;
    int lastGeneration = -1, noteKey = -1;
    float lastBank = -1, lastProgram = -1;
    juce::uint32 noteOffAt = 0;
};
} // namespace

std::unique_ptr<juce::Component> makeSoundFontEditor (AudioEngine& engine, int channel)
{
    return std::make_unique<SoundFontEditor> (engine, SfTarget::rack (channel));
}

std::unique_ptr<juce::Component> makeSoundFontEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate)
{
    return std::make_unique<SoundFontEditor> (engine, SfTarget { std::move (locate), -1 });
}

juce::File soundFontFoldersFile()
{
    return PluginHost::settingsFolder().getChildFile ("soundfont-folders.txt");
}

std::vector<std::string> soundFontFolders()
{
    std::vector<std::string> out;
    const auto add = [&out] (const juce::File& f) {
        if (f.isDirectory())
        {
            const auto p = std8 (f.getFullPathName());
            if (std::find (out.begin(), out.end(), p) == out.end())
                out.push_back (p);
        }
    };
    const auto& roots = flStudioAccess().roots;
    if (! roots.data.empty())
        add (juce::File (u8 (roots.data)).getChildFile ("Patches").getChildFile ("Soundfonts"));
    if (! roots.userData.empty())
        add (juce::File (u8 (roots.userData)));
    for (const auto& line : juce::StringArray::fromLines (soundFontFoldersFile().loadFileAsString()))
        if (line.trim().isNotEmpty())
            add (juce::File (line.trim()));
    add (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory));
    add (juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    add (juce::File::getSpecialLocation (juce::File::userDesktopDirectory));
    add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"));
    for (const auto& line : juce::StringArray::fromLines (PluginHost::settingsFolder().getChildFile ("browser-folders.txt").loadFileAsString()))
        if (line.trim().isNotEmpty())
            add (juce::File (line.trim()));
    return out;
}

void ensureSoundFontIndex (bool force)
{
    static bool cacheSet = false;
    auto& index = sf::PresetIndex::shared();
    if (! cacheSet)
    {
        index.setCacheFile (std8 (PluginHost::settingsFolder().getChildFile ("soundfont-index.txt").getFullPathName()));
        cacheSet = true;
    }
    index.setFolders (soundFontFolders(), force);
}

void previewSoundFontPreset (AudioEngine& engine, const std::string& path, int bank, int program)
{
    static std::atomic<int> latest { 0 };
    const auto ticket = ++latest;
    std::thread ([&engine, path, bank, program, ticket] {
        auto handle = sf::FontLibrary::instance().getNow (path);
        if (handle->font() == nullptr || ticket != latest.load())
            return;
        constexpr double rate = 48000.0;
        auto inst = sf::makeInstrument (handle);
        inst->prepare (rate);
        inst->configure (sf::makeChannel (path, bank, program), 120.0);
        auto sample = std::make_shared<SampleData>();
        sample->sampleRate = rate;
        const auto total = (int) (rate * 1.6), held = (int) (rate * 0.7);
        sample->left.assign ((std::size_t) total, 0.0f);
        sample->right.assign ((std::size_t) total, 0.0f);
        const auto key = bank == 128 ? 38 : 60;
        inst->noteOn (key, 0.8f);
        for (int at = 0; at < total; at += 256)
        {
            if (at >= held && at - 256 < held)
                inst->noteOff (key);
            inst->render (sample->left.data() + at, sample->right.data() + at, std::min (256, total - at), 1.0f, 1.0f);
        }
        juce::MessageManager::callAsync ([&engine, sample, ticket] {
            if (ticket == latest.load())
                engine.previewSample (sample);
        });
    }).detach();
}

int addSoundFontChannel (AudioEngine& engine, const std::string& path, int bank, int program, int insertAt)
{
    auto c = sf::makeChannel (path, bank, program);
    if (! path.empty())
    {
        // the channel is named after the preset
        const auto info = sf::scanFontFile (path);
        for (const auto& p : info.presets)
            if (p.bank == bank && p.program == program && ! p.name.empty())
                c.name = p.name;
    }
    int index = 0;
    engine.editSong ([&] (Song& s) {
        c.mixerTrack = s.claimFreeInsert();
        if (insertAt < 0 || insertAt > (int) s.channels.size())
        {
            s.channels.push_back (c);
            index = (int) s.channels.size() - 1;
        }
        else
        {
            s.channels.insert (s.channels.begin() + insertAt, c);
            index = insertAt;
        }
    });
    engine.setSelectedChannel (index);
    if (! path.empty())
        sf::PresetIndex::shared().addFont (path);
    return index;
}

void loadSoundFontInto (AudioEngine& engine, int channel, const std::string& path, int bank, int program)
{
    loadInto (engine, SfTarget::rack (channel), path, bank, program);
}

bool firstSoundFontPreset (const std::string& path, int& bank, int& program)
{
    const auto info = sf::scanFontFile (path);
    if (! info.valid)
        return false;
    bank = 0, program = 0;
    bool first = true;
    for (const auto& p : info.presets)
        if (first || p.bank < bank || (p.bank == bank && p.program < program))
            bank = p.bank, program = p.program, first = false;
    return true;
}

bool openSoundFontFile (AudioEngine& engine, const juce::File& file, int channel)
{
    const auto path = std8 (file.getFullPathName());
    int bank = 0, program = 0;
    if (! firstSoundFontPreset (path, bank, program))
        return false;
    if (channelOf (engine, SfTarget::rack (channel)) != nullptr)
    {
        loadInto (engine, SfTarget::rack (channel), path, bank, program);
        sf::PresetIndex::shared().addFont (path);
    }
    else
        addSoundFontChannel (engine, path, bank, program);
    return true;
}

juce::String soundFontDragDescription (const std::string& path, int bank, int program)
{
    return juce::String (dragPrefix) + juce::String (bank) + "|" + juce::String (program) + "|" + u8 (path);
}

bool parseSoundFontDrag (const juce::String& description, std::string& path, int& bank, int& program)
{
    if (! description.startsWith (dragPrefix))
        return false;
    const auto rest = description.substring ((int) std::strlen (dragPrefix));
    bank = rest.upToFirstOccurrenceOf ("|", false, false).getIntValue();
    const auto rest2 = rest.fromFirstOccurrenceOf ("|", false, false);
    program = rest2.upToFirstOccurrenceOf ("|", false, false).getIntValue();
    path = std8 (rest2.fromFirstOccurrenceOf ("|", false, false));
    return ! path.empty();
}
} // namespace lynx
