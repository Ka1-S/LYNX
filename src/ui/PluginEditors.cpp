#include "ui/SlotWatch.h"
#include "ui/PatcherEditor.h"
#include "ui/ControllerEditors.h"
#include "ui/Controllers164.h"
#include "ui/NewtoneEditor.h"
#include "ui/VisualEditors.h"
#include "ui/FlExtraEditors.h"
#include "ui/PluginEditors.h"

#include <cstring>
#include <map>
#include <array>
#include <optional>

#include "core/Effects.h"
#include "core/FlEffects.h"
#include "core/FruityShapers.h"
#include "core/GrossBeat.h"
#include "core/LynxTune.h"
#include "core/MaximusFx.h"
#include "core/MixSuite.h"
#include "core/EchoSuite.h"
#include "core/VoiceTools.h"
#include "core/ParametricEqFl.h"
#include "core/VocalPitch.h"
#include "core/FruityBasicFx.h"
#include "core/PluginLayout.h"
#include "core/StateParams.h"
#include "core/ConvolverFx.h"
#include "core/EquoFx.h"
#include "core/MultibandDelayFx.h"
#include "core/Vocodex.h"
#include "core/HardwareInsert.h"
#include "engine/Text.h"
#include "ui/DynamicsDisplays.h"
#include "ui/I18n.h"
#include "ui/Icons.h"
#include "ui/FacetEqEditor.h"
#include "ui/ProEditors.h"
#include "ui/ParametricEqEditor.h"
#include "core/EffectPresets.h"
#include "ui/PluginHeader.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WindowOwner.h"

namespace lynx
{
namespace
{
bool demoDisplays = false; // --plugui: live displays show a synthetic signal

using plugui::Item;
using plugui::LayoutItem;

constexpr float headerHeight = 26.0f; // the presets bar (not scaled with the body)

bool hasStyle (const LayoutItem& item, std::uint16_t flag) noexcept
{
    return (item.style & flag) != 0;
}

// a row of tabs drawn as a column (taller than wide: Maximus' band selector)
bool isVertical (const LayoutItem& item) noexcept
{
    return item.type == Item::tabs && item.h > item.w * 1.2f;
}

// --- the editor ----------------------------------------------------------------------------------

class NativeEffectEditor final : public juce::Component,
                                 public ui::HintSource,
                                 private juce::ChangeListener,
                                 private juce::Timer
{
public:
    NativeEffectEditor (AudioEngine& e, int t, int s, float initialScale, int initialPage)
        : engine (e),
          track (t),
          slot (s)
    {
        if (const auto* fx = current())
        {
            kind = fx->kind;
            layout = &plugui::layoutFor (kind);
            if (kind == EffectKind::ember) // round 165: this window only opens for the Classic voicing — its one-screen
            {                              // panel, not the 1110-unit multiband page around it
                static const auto classic = [] { auto l = plugui::layoutFor (EffectKind::ember); l.height = 440; return l; }();
                layout = &classic;
            }
            if (kind == EffectKind::maximus) // Soundgoodizer runs on the Maximus engine: its own page
                if (const auto blob = maximusfx::unpack (fx->state); blob && blob->build == hlfx::MaximusBuild::soundgoodizer)
                    layout = &plugui::soundgoodizerLayout();
            if (isSoundgoodizerApprox (*fx)) // without FL's preset files: the same page drives the approximation
            {
                sgApprox = true;
                layout = &plugui::soundgoodizerLayout();
            }
        }

        accent = skin::accentFor (sgApprox ? EffectKind::maximus : kind);
        if (kind == EffectKind::ember) // round 165: the Classic voicing's VU shows the output level, as the reference's
            vuMode = 2;
        anim.onFrame = [this] { repaint(); };
        page = initialPage;
        if (kind == EffectKind::equo && initialPage == 0)
            page = 3; // bank 1, volume (FL opens EQUO on its first bank)
        if (kind == EffectKind::maximus && stateBacked() && initialPage == 0)
            if (const auto* fx = current())
                page = maximusfx::selectedBand (fx->state); // FL reopens Maximus on the band selected when it was saved
        engine.addChangeListener (this);
        const auto& l = activeLayout();
        for (const auto& item : l.items) // live meters and graphs (mixing suite, LYNX Tune's pitch, round 108's dynamics): 30 Hz
            if (item.type == Item::display && item.display != nullptr
                && (std::strcmp (item.display, "grmeter") == 0 || std::strcmp (item.display, "grleds") == 0 || std::strcmp (item.display, "vumeter") == 0 || std::strcmp (item.display, "iometer") == 0 || std::strcmp (item.display, "outmeter") == 0 || std::strcmp (item.display, "limitpill") == 0 || std::strcmp (item.display, "gcmeter") == 0
                    || std::strcmp (item.display, "pitchgraph") == 0 || std::strcmp (item.display, "panpos") == 0 || std::strcmp (item.display, "hwping") == 0
                    || std::strcmp (item.display, "pitchtrace") == 0 || std::strcmp (item.display, "pitchring") == 0 || dynui::isLive (item.display)))
            {
                startTimerHz (30);
                break;
            }
        setSize ((int) std::ceil ((float) l.width * initialScale), (int) std::ceil ((float) l.height * initialScale + headerHeight + footerHeight()));
    }

    ~NativeEffectEditor() override
    {
        stopTimer();
        engine.removeChangeListener (this);
        ui::hideValueTip();
    }

    // the design size (for the window's aspect ratio)
    juce::Point<int> designSize() const { return { activeLayout().width, activeLayout().height }; }

    const EffectSlot* current() const
    {
        const auto& mixer = engine.getSong().mixer;

        if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
            return nullptr;

        const auto& fx = mixer[(std::size_t) track].effects[(std::size_t) slot];
        return layout == nullptr || fx.kind == kind ? &fx : nullptr;
    }

    // --- geometry ----------------------------------------------------------------------------------

    const plugui::Layout& activeLayout() const
    {
        static const plugui::Layout empty { "", 360, 120, {} };
        return layout != nullptr ? *layout : empty;
    }

    float scale() const
    {
        const auto& l = activeLayout();
        return juce::jmax (0.25f, juce::jmin ((float) getWidth() / (float) l.width, ((float) getHeight() - headerHeight - footerHeight()) / (float) l.height));
    }

    // LYNX's own engines show a one-line description of the control under the mouse at the bottom (the reference
    // reverbs' description line, round 129)
    bool hasHelpLine() const
    {
        return layout != nullptr && layout != &plugui::soundgoodizerLayout()
               && (std::find (plugui::ownLayoutKinds().begin(), plugui::ownLayoutKinds().end(), kind) != plugui::ownLayoutKinds().end()
                   || kind == EffectKind::lynxTune); // round 135: LYNX Tune's panel too
    }
    float footerHeight() const { return hasHelpLine() ? 20.0f : 0.0f; }

    void paintHelpLine (juce::Graphics& g)
    {
        using namespace theme;
        const auto area = juce::Rectangle<float> (0.0f, (float) getHeight() - footerHeight(), (float) getWidth(), footerHeight());
        g.setColour (colour::bg0.withAlpha (0.55f));
        g.fillRect (area);
        juce::String text;
        if (hoverItem >= 0 && hoverItem < (int) activeLayout().items.size())
        {
            const auto& item = activeLayout().items[(std::size_t) hoverItem];
            if (const auto p = paramOf (item); p >= 0)
            {
                const auto what = pluginheader::describe (kind, effectInfo (kind).params[(std::size_t) p].name);
                text = (item.label != nullptr && *item.label != 0 ? tr (item.label) : utf8 (effectInfo (kind).params[(std::size_t) p].name))
                       + (what.isNotEmpty() ? juce::String::fromUTF8 (" \xe2\x80\x94 ") + what : juce::String());
            }
        }
        // round 135: nothing under the mouse -> what the current mode is (the reference's summary of the algorithm)
        if (text.isEmpty())
            if (const auto* fx = current())
                text = pluginheader::modeLine (kind, fx->params, engine.getSong().bpm);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::small));
        g.drawFittedText (text, area.reduced (10.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    }

    // round 135: the MIX caption of a LYNX engine toggles the mix lock (the reference reverbs: click the MIX label)
    bool isMixItem (const LayoutItem& item) const
    {
        const auto p = paramOf (item);
        return (item.type == Item::knob || (item.type == Item::vslider && ! hasStyle (item, plugui::captionLeft))) && p >= 0 && hasHelpLine()
               && item.label != nullptr && ! hasStyle (item, plugui::noCaption) && juce::String (effectInfo (kind).params[(std::size_t) p].name) == "Mix";
    }
    juce::Rectangle<float> captionArea (const LayoutItem& item) const
    {
        const auto r = rectOf (item);
        const auto s = scale();
        if (item.type == Item::vslider)
            return r.withTop (r.getBottom() - 13.0f * s);
        if (hasStyle (item, plugui::captionAbove))
            return r.withHeight (14.0f * s);
        const auto circle = knobCircle (item);
        return r.withTop (circle.getBottom()).withHeight (13.0f * s);
    }

    juce::Rectangle<float> rectOf (const LayoutItem& item) const
    {
        const auto s = scale();
        return { item.x * s, headerHeight + item.y * s, item.w * s, item.h * s };
    }

    bool visible (const LayoutItem& item) const
    {
        const auto* fx = current();
        return fx != nullptr && plugui::isShown (kind, item, page, fx->params, &fx->state);
    }

    // wide option menus of LYNX's own panels get step arrows at both ends (round 114)
    bool hasStepArrows (const LayoutItem& item) const
    {
        return item.type == Item::menu && item.w >= 110.0f && kind != EffectKind::parametricEq && kind != EffectKind::fruitySend && kind != EffectKind::hardwareInsert
               && std::find (plugui::ownLayoutKinds().begin(), plugui::ownLayoutKinds().end(), kind) != plugui::ownLayoutKinds().end();
    }

    // the knob's circle inside its cell (the caption takes the rest)
    juce::Rectangle<float> knobCircle (const LayoutItem& item) const
    {
        auto r = rectOf (item);
        const auto s = scale();
        if (hasStyle (item, plugui::valueAbove))
            r.removeFromTop (16.0f * s);
        if (hasStyle (item, plugui::captionAbove))
        {
            r.removeFromTop (14.0f * s);
            r.removeFromBottom (13.0f * s);
        }
        else if (! hasStyle (item, plugui::noCaption))
            r.removeFromBottom ((hasStyle (item, plugui::showValue) ? 26.0f : 14.0f) * s);
        const auto d = juce::jmin (r.getWidth(), r.getHeight());
        return r.withSizeKeepingCentre (d, d).withY (r.getY());
    }

    int itemAt (juce::Point<float> p) const
    {
        const auto& items = activeLayout().items;

        for (int i = (int) items.size(); --i >= 0;)
        {
            const auto& item = items[(std::size_t) i];
            if (! visible (item) || item.type == Item::section || item.type == Item::label)
                continue;
            if (item.type == Item::display && rectOf (item).contains (p) && (juce::String (item.display) == "eq" || juce::String (item.display) == "xy"
                                                                    || juce::String (item.display) == "sgamount" || juce::String (item.display) == "sgpresets"
                                                                    || juce::String (item.display) == "prismbands" || juce::String (item.display) == "emberbands"
                                                                    || juce::String (item.display) == "tunekeys" || juce::String (item.display) == "tuneautokey"
                                                                    || juce::String (item.display) == "pitchring" || juce::String (item.display) == "waveshaper"
                                                                    || isBars (item) || isRhythmEdit (item) || isRhythmPatterns (item)))
                return i;
            if (item.type == Item::display || (item.param == nullptr && ! hasStyle (item, plugui::pageTabs)))
                continue;
            if (rectOf (item).contains (p))
                return i;
        }
        return -1;
    }

    int paramOf (const LayoutItem& item) const { return item.param != nullptr ? plugui::paramIndex (kind, item.param) : -1; }

    // --- values --------------------------------------------------------------------------------------

    // Controls stored in the slot's state (core/StateParams.h: Maximus' bands, Delay Bank's banks, band tables...)
    // edit those bytes instead of EffectSlot::params
    // the header's undo / A-B / preset bar (round 129): every editor whose setting is its parameters
    bool usesBar() const { return ! stateBacked() && layout != &plugui::soundgoodizerLayout() && layout != nullptr; }

    bool stateBacked() const { return plugui::usesStateParams (kind) && layout != &plugui::soundgoodizerLayout(); }
    bool isStateParam (int param) const { return stateBacked() && plugui::stateIndex (kind, param) >= 0; }
    const stateparams::Table& stateTable() const { return *stateparams::tableFor (kind); }

    float valueOf (int param) const
    {
        const auto* fx = current();
        if (isStateParam (param))
            return fx != nullptr ? plugui::stateValue01 (kind, param, fx->state) : 0.0f;
        return fx != nullptr && param >= 0 && param < maxEffectParams ? fx->params[(std::size_t) param] : 0.0f;
    }

    void setValue (int param, float value)
    {
        if (isStateParam (param))
        {
            const auto* fx = current();
            if (fx == nullptr)
                return;
            const auto s = plugui::stateIndex (kind, param);
            const auto raw = plugui::stateRaw (kind, param, value);
            if (stateTable().get (fx->state, s) == raw)
                return;
            engine.editSong ([t = track, sl = slot, bytes = stateTable().with (fx->state, s, raw)] (Song& song) {
                song.mixer[(std::size_t) t].effects[(std::size_t) sl].state = bytes;
            });
            return;
        }
        if (current() == nullptr || param < 0 || param >= maxEffectParams)
            return;
        if (kind == EffectKind::echoform) // round 135w: Filter link moves the low and high cut together
            if (const auto edits = echosuite::echoform::linkedEdit (current()->params, param, value); edits.size() > 1)
            {
                setParams (edits);
                return;
            }
        if (kind == EffectKind::ember) // round 165: the Classic voicing's Auto moves Output with Drive
            if (const auto edits = mixsuite::ember::linkedEdit (current()->params, param, value); edits.size() > 1)
            {
                setParams (edits);
                return;
            }
        if (usesBar())
            bar.willEdit();
        engine.editSong ([t = track, s = slot, param, v = juce::jlimit (0.0f, 1.0f, value)] (Song& song) {
            song.mixer[(std::size_t) t].effects[(std::size_t) s].params[(std::size_t) param] = v;
        });
    }

    juce::String valueText (int param, float value) const
    {
        const auto* fx = current();
        if (fx == nullptr || param < 0)
            return {};
        if (isStateParam (param))
            return utf8 (stateTable().text (fx->state, plugui::stateIndex (kind, param), plugui::stateRaw (kind, param, value)).c_str());
        if (kind == EffectKind::fruitySend && param == 3)
            return flbasic::sendTrack (value) > 0 ? insertName (flbasic::sendTrack (value)) : tr ("None");
        if (kind == EffectKind::hardwareInsert && (param == hwinsert::output || param == hwinsert::input))
            return hwChannelText (param == hwinsert::output, fx->params);
        return utf8 (formatEffectParam (kind, param, value, engine.getSong().bpm, fx->params).c_str());
    }

    // the hardware insert's output / input as the device names it ("Out 3 - Out 4", "In 2"; "None")
    juce::String hwChannelText (bool out, const EffectParams& p) const
    {
        const auto channel = hwinsert::channelOf (p[out ? hwinsert::output : hwinsert::input]);
        if (channel < 0)
            return tr ("None");
        if (out)
        {
            const auto mode = hwinsert::outputModeOf (p);
            const auto names = engine.getOutputChannelNames();
            auto text = juce::String::fromUTF8 (outputTargetName (channel, mode, names, false).c_str());
            return text.replace ("(mono)", tr ("(mono)")).replace ("(missing)", tr ("(missing)"));
        }
        const auto stereo = p[hwinsert::inputStereo] >= 0.5f;
        const auto names = engine.getInputChannelNames();
        return juce::String::fromUTF8 (inputSourceName (channel, stereo, names).c_str());
    }

    // the hardware insert's channel menus: the device's pairs, then every channel alone (as the mixer lists them)
    void showHwChannelMenu (bool out, juce::Rectangle<float> r)
    {
        const auto* fx = current();
        if (fx == nullptr)
            return;
        juce::PopupMenu menu;
        const auto chosen = hwinsert::channelOf (fx->params[out ? hwinsert::output : hwinsert::input]);
        const auto apply = [this] (std::vector<std::pair<int, float>> changes) {
            setParams (changes);
            engine.openHardwareInsertIo(); // (the device opens the channels: no trip to Audio settings)
        };
        if (out)
        {
            const auto mono = fx->params[hwinsert::outputMono] >= 0.5f;
            menu.addItem (tr ("None"), true, chosen < 0, [apply] { apply ({ { hwinsert::output, 0.0f } }); });
            const auto names = engine.getOutputChannelNames();
            if (names.empty())
                menu.addItem (tr ("No audio device is open"), false, false, [] {});
            for (const auto& t : outputTargets (names))
            {
                const auto isMono = t.mode != OutputMode::stereo;
                const auto busy = t.channel < 2 && engine.getSong().mixer[0].outputChannel == outputDefault; // the Master's own pair
                auto name = juce::String::fromUTF8 (t.name.c_str()) + (isMono ? " " + tr ("(mono)") : juce::String());
                if (busy)
                    name += "  " + tr ("(the Master plays here)");
                menu.addItem (name, true, chosen == t.channel && mono == isMono,
                              [apply, t, isMono] { apply ({ { hwinsert::output, hwinsert::channelValue (t.channel) }, { hwinsert::outputMono, isMono ? 1.0f : 0.0f } }); });
            }
        }
        else
        {
            const auto stereo = fx->params[hwinsert::inputStereo] >= 0.5f;
            menu.addItem (tr ("None"), true, chosen < 0, [apply] { apply ({ { hwinsert::input, 0.0f } }); });
            const auto names = engine.getInputChannelNames();
            if (names.empty())
                menu.addItem (tr ("Open the audio input"), true, false, [this] {
                    if (const auto result = engine.openAudioInput(); result.issue != AudioEngine::InputIssue::none)
                        ui::flashValueTip (*this, getLocalBounds().withHeight (40), tr ("No audio input could be opened (Options > Audio settings)"));
                    repaint();
                });
            for (const auto& source : inputSources (names))
                menu.addItem (juce::String::fromUTF8 (source.name.c_str()), true, chosen == source.channel && stereo == source.stereo,
                              [apply, source] { apply ({ { hwinsert::input, hwinsert::channelValue (source.channel) }, { hwinsert::inputStereo, source.stereo ? 1.0f : 0.0f } }); });
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (r.toNearestInt())));
    }

    // an insert's name as the mixer shows it
    juce::String insertName (int t) const
    {
        const auto& mixer = engine.getSong().mixer;
        const auto name = t > 0 && t < (int) mixer.size() ? juce::String::fromUTF8 (mixer[(std::size_t) t].name.c_str()) : juce::String();
        return name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (t) });
    }

    juce::String trackName() const
    {
        const auto& mixer = engine.getSong().mixer;
        if (track == 0)
            return tr ("Master");
        const auto name = track < (int) mixer.size() ? juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str()) : juce::String();
        return name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) });
    }

    ui::ValueSpec specFor (const LayoutItem& item) const
    {
        ui::ValueSpec spec;
        const auto param = paramOf (item);
        const auto& info = effectInfo (kind);

        if (isStateParam (param))
        {
            // not automatable (the values live in the state): drag, wheel, reset and type-in only
            const auto& p = stateTable().params[(std::size_t) plugui::stateIndex (kind, param)];
            spec.name = trackName() + " - " + utf8 (info.name) + " - " + utf8 (p.name.c_str());
            spec.defaultValue = p.maximum > p.minimum ? (double) (p.defaultValue - p.minimum) / (double) (p.maximum - p.minimum) : 0.0;
            spec.pixelsForRange = item.type == Item::hslider ? juce::jmax (60.0, (double) rectOf (item).getWidth())
                                  : item.type == Item::vslider ? juce::jmax (60.0, (double) rectOf (item).getHeight())
                                                               : 180.0;
            if (item.steps > 0)
                spec.interval = 1.0 / item.steps;
            else if (p.maximum - p.minimum <= 16)
                spec.interval = 1.0 / juce::jmax (1, p.maximum - p.minimum);
            spec.toText = [this, param] (double v) { return valueText (param, (float) v); };
            spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
            return spec;
        }

        if (param < 0 || param >= (int) info.params.size())
            return spec;

        spec.name = trackName() + " - " + utf8 (info.name) + " - " + utf8 (info.params[(std::size_t) param].name);
        spec.defaultValue = info.params[(std::size_t) param].defaultValue;
        spec.pixelsForRange = item.type == Item::hslider ? juce::jmax (60.0, (double) rectOf (item).getWidth())
                              : item.type == Item::vslider ? juce::jmax (60.0, (double) rectOf (item).getHeight())
                                                           : 180.0;
        if (item.steps > 0)
            spec.interval = 1.0 / item.steps;
        spec.toText = [this, param] (double v) { return valueText (param, (float) v); };
        spec.fromText = ui::inverseOf (spec.toText, 0.0, 1.0);
        spec.target = AutomationTarget { AutomationTargetKind::effectParam, track, slot, param };
        return spec;
    }

    // --- painting ------------------------------------------------------------------------------------

    // this editor's style (round 126: one per plugin, alternating; Settings may force one)
    int styleKey() const { return skin::styleKey (kind, layout == &plugui::soundgoodizerLayout()); }
    skin::Style editorStyle() const { return skin::styleFor (kind, layout == &plugui::soundgoodizerLayout()); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const skin::StyleScope styleScope (editorStyle());
        skin::canvas (g, getLocalBounds().toFloat(), accent);
        const auto* fx = current();

        if (fx == nullptr)
        {
            g.setColour (colour::textSecondary);
            g.setFont (font (type::body));
            g.drawText (tr ("The effect was removed from this slot."), getLocalBounds(), juce::Justification::centred);
            return;
        }

        paintHeader (g, *fx);

        const auto& items = activeLayout().items;
        for (int i = 0; i < (int) items.size(); ++i)
            if (visible (items[(std::size_t) i]))
                paintItem (g, items[(std::size_t) i], i, *fx);
        if (hasHelpLine())
            paintHelpLine (g);
    }

    juce::Rectangle<float> headerArea() const { return { 0.0f, 0.0f, (float) getWidth(), headerHeight }; }
    juce::Rectangle<float> menuButton() const { return { 4.0f, 3.0f, 22.0f, 20.0f }; }
    juce::Rectangle<float> nextButton() const { return { (float) getWidth() - 26.0f, 3.0f, 20.0f, 20.0f }; }
    juce::Rectangle<float> prevButton() const { return nextButton().translated (-22.0f, 0.0f); }
    juce::Rectangle<float> presetBox() const
    {
        const auto w = juce::jlimit (90.0f, 220.0f, (float) getWidth() * 0.32f);
        return { prevButton().getX() - w - 4.0f, 3.0f, w, 20.0f };
    }

    void paintHeader (juce::Graphics& g, const EffectSlot& fx)
    {
        using namespace theme;
        const auto area = headerArea();
        skin::headerBar (g, area, accent);

        // options menu (FL's arrow at the left of the plugin's title)
        drawArrow (g, menuButton(), 0.0f, hoverHeader == 1 ? accent : colour::textSecondary);

        // LYNX's mark, bypass LED, name, where it is
        skin::brandMark (g, juce::Rectangle<float> (menuButton().getRight() + 2.0f, 0.0f, 18.0f, area.getHeight()));
        const auto ledC = juce::Point<float> (menuButton().getRight() + 30.0f, area.getCentreY());
        skin::powerLed (g, ledC, 7.0f, fx.enabled);
        auto text = area.withLeft (ledC.x + 10.0f).withRight (presetBox().getX() - 6.0f);
        if (usesBar())
            text = bar.place (area).withLeft (ledC.x + 10.0f);
        const auto name = layout == &plugui::soundgoodizerLayout() ? juce::String ("Soundgoodizer") : utf8 (effectInfo (kind).name);
        const auto nameWidth = skin::titleText (g, text.withHeight (area.getHeight()), name, accent);
        text.removeFromLeft (nameWidth);
        g.setFont (font (type::small));
        g.setColour (colour::textSecondary);
        g.drawText ("(" + trackName() + ")", text.withTrimmedLeft (8.0f), juce::Justification::centredLeft, true);

        if (usesBar())
        {
            bar.paint (g, accent);
            return;
        }

        // presets: name, previous, next
        const auto box = presetBox();
        skin::valuePlate (g, box, radius::md);
        if (hoverHeader == 2)
        {
            g.setColour (accent.withAlpha (0.5f));
            g.drawRoundedRectangle (box.reduced (0.5f), radius::md, 1.0f);
        }
        g.setColour (presetName.isNotEmpty() ? colour::textPrimary : colour::textSecondary);
        ui::drawFittingText (g, presetName.isNotEmpty() ? presetName : tr ("(no preset)"), box.reduced (8.0f, 0.0f), juce::Justification::centredRight,
                             font (type::small, presetName.isNotEmpty()), 8.0f, 0.0f, 0.75f);
        drawArrow (g, prevButton(), juce::MathConstants<float>::pi, hoverHeader == 3 ? accent : colour::textSecondary);
        drawArrow (g, nextButton(), 0.0f, hoverHeader == 4 ? accent : colour::textSecondary);
    }

    static void drawArrow (juce::Graphics& g, juce::Rectangle<float> r, float angle, juce::Colour c)
    {
        juce::Path p;
        p.addTriangle (-3.0f, -5.0f, -3.0f, 5.0f, 4.0f, 0.0f);
        p.applyTransform (juce::AffineTransform::rotation (angle).translated (r.getCentreX(), r.getCentreY()));
        g.setColour (c);
        g.fillPath (p);
    }

    juce::Colour arcColour (const EffectSlot& fx, int colourIndex = -1) const
    {
        if (! fx.enabled)
            return theme::colour::textDisabled;
        return colourIndex >= 0 ? peqview::bandColour (colourIndex) : accent;
    }

    void caption (juce::Graphics& g, const LayoutItem& item, juce::Rectangle<float> r, juce::Justification j = juce::Justification::centred)
    {
        using namespace theme;
        if (item.label == nullptr || *item.label == 0)
            return;
        const auto locked = isMixItem (item) && pluginheader::isMixLocked (kind);
        if (locked) // round 135: the locked MIX caption is boxed in the accent colour
        {
            const auto w = juce::jmin (r.getWidth(), juce::GlyphArrangement::getStringWidth (font (juce::jmax (8.0f, 9.5f * scale()), true), caps (tr (item.label))) + 10.0f);
            g.setColour (accent.withAlpha (0.25f));
            g.fillRoundedRectangle (r.withSizeKeepingCentre (w, r.getHeight()), 3.0f);
            g.setColour (accent);
            g.drawRoundedRectangle (r.withSizeKeepingCentre (w, r.getHeight()), 3.0f, 1.0f);
        }
        g.setColour (locked ? theme::colour::textPrimary : hasStyle (item, plugui::dim) ? colour::textDisabled : skin::captionColour());
        ui::drawFittingText (g, caps (tr (item.label)), r, j, font (juce::jmax (8.0f, 9.5f * scale()), true), 7.0f); // (round 165: fit, no "...")
    }

    void paintItem (juce::Graphics& g, const LayoutItem& item, int index, const EffectSlot& fx)
    {
        using namespace theme;
        const auto r = rectOf (item);
        const auto s = scale();
        const auto param = paramOf (item);
        const auto value = valueOf (param);
        const auto hot = index == hoverItem || index == dragItem;
        const auto glow = index == dragItem ? 1.0f : anim.level (index);

        switch (item.type)
        {
            case Item::section:
            {
                skin::section (g, r, radius::lg * s, hasStyle (item, plugui::raised), fx.enabled ? accent : juce::Colours::transparentBlack);
                if (item.label != nullptr && *item.label != 0 && ! hasStyle (item, plugui::boxed))
                    skin::sectionTitle (g, r.withHeight (16.0f * s).translated (0.0f, 1.0f * s), caps (tr (item.label)), accent, juce::jmax (8.0f, 10.0f * s));
                break;
            }

            case Item::label:
                if (hasStyle (item, plugui::dim) && ! hasStyle (item, plugui::boxed) && item.h > 30.0f && item.label != nullptr)
                {
                    // the plugin's name, big and quiet (FL's logos are not reproduced)
                    g.setColour (colour::bg4);
                    ui::drawFittingText (g, item.label, r, hasStyle (item, plugui::alignLeft) ? juce::Justification::centredLeft : juce::Justification::centredRight,
                                         font (r.getHeight() * 0.62f, true), r.getHeight() * 0.25f, 0.0f, 0.8f); // (smaller, never "Wav...")
                    break;
                }
                if (hasStyle (item, plugui::boxed))
                    skin::section (g, r, radius::sm * s, true);
                caption (g, item, r, hasStyle (item, plugui::captionLeft | plugui::alignLeft) ? juce::Justification::centredLeft : juce::Justification::centred);
                break;

            case Item::value:
                if (hasStyle (item, plugui::boxed)) // a read-out plate (FL's value boxes)
                    skin::valuePlate (g, r, radius::sm * s);
                g.setColour (hot ? accent : (fx.enabled ? colour::textPrimary : colour::textDisabled));
                g.setFont (font (juce::jmax (8.0f, 11.0f * s)));
                g.drawFittedText (valueText (param, value), r.toNearestInt(),
                                  hasStyle (item, plugui::alignLeft) ? juce::Justification::centredLeft : juce::Justification::centred, 1, 0.7f);
                break;

            case Item::knob:
            {
                const auto circle = knobCircle (item);
                skin::KnobLook look;
                look.value = value;
                look.bipolar = hasStyle (item, plugui::bipolar);
                look.accent = item.colour >= 0 ? peqview::bandColour (item.colour) : accent;
                look.hover = glow;
                look.active = index == dragItem;
                look.enabled = fx.enabled;
                skin::knob (g, circle, look);
                auto below = r.withTop (circle.getBottom() + 1.0f * s);
                if (hasStyle (item, plugui::valueAbove))
                {
                    const auto box = r.withHeight (13.0f * s).reduced (juce::jmax (0.0f, (r.getWidth() - 40.0f * s) * 0.5f), 0.0f);
                    skin::valuePlate (g, box, radius::sm);
                    g.setColour (hot ? colour::textPrimary : colour::textSecondary);
                    g.setFont (font (juce::jmax (8.0f, 9.0f * s)));
                    g.drawFittedText (valueText (param, value), box.toNearestInt(), juce::Justification::centred, 1, 0.6f);
                }
                if (hasStyle (item, plugui::captionAbove))
                {
                    caption (g, item, r.withHeight (13.0f * s).expanded (10.0f * s, 0.0f));
                    g.setColour (hot ? look.accent.brighter (0.3f) : skin::valueColour (false));
                    g.setFont (font (juce::jmax (8.0f, 10.5f * s)));
                    g.drawFittedText (valueText (param, value), below.withHeight (13.0f * s).expanded (10.0f * s, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.7f);
                }
                else if (! hasStyle (item, plugui::noCaption))
                {
                    caption (g, item, below.withHeight (12.0f * s));
                    if (hasStyle (item, plugui::showValue))
                    {
                        g.setColour (hot ? look.accent.brighter (0.3f) : skin::valueColour (false));
                        g.setFont (font (juce::jmax (8.0f, 10.5f * s)));
                        g.drawFittedText (valueText (param, value), below.withTrimmedTop (12.0f * s).withHeight (13.0f * s).toNearestInt(),
                                          juce::Justification::centred, 1, 0.7f);
                    }
                }
                break;
            }

            case Item::hslider:
            case Item::vslider:
            {
                const auto vertical = item.type == Item::vslider;
                auto area = r;
                if (item.label != nullptr && ! hasStyle (item, plugui::noCaption))
                {
                    if (hasStyle (item, plugui::captionLeft))
                        caption (g, item, area.removeFromLeft (area.getWidth() * 0.4f), juce::Justification::centredLeft);
                    else if (vertical)
                        caption (g, item, area.removeFromBottom (13.0f * s).expanded (18.0f * s, 0.0f));
                    else
                        caption (g, item, area.removeFromBottom (12.0f * s));
                }
                const auto thick = juce::jmax (3.0f, 4.0f * s);
                const auto trackRect = vertical ? area.withSizeKeepingCentre (thick, area.getHeight() - 10.0f * s)
                                                : area.withSizeKeepingCentre (area.getWidth() - 8.0f * s, thick);
                skin::slider (g, trackRect, value, vertical, hasStyle (item, plugui::bipolar), item.colour >= 0 ? peqview::bandColour (item.colour) : accent,
                              hot, fx.enabled, vertical ? juce::jmin (area.getWidth(), 20.0f * s) : juce::jmin (area.getHeight(), 16.0f * s), 9.0f * s);
                break;
            }

            case Item::radio:
            {
                const auto labels = plugui::optionLabels (item);
                const auto chosen = plugui::optionAt (item, value);
                const auto texts = optionTexts (item);
                if (item.columns > 0) // a grid of named cells (Gross Beat's slots)
                {
                    const auto cellAccent = item.colour >= 0 ? colour::info : accent;
                    g.setFont (font (juce::jmax (8.0f, 10.0f * s)));
                    for (int o = 0; o < (int) texts.size(); ++o)
                    {
                        const auto cell = gridCell (item, o, (int) texts.size()).reduced (0.5f);
                        g.setColour (o == chosen ? cellAccent.withAlpha (0.22f) : ((o / item.columns) % 2 == 0 ? colour::bg0 : colour::bg1));
                        g.fillRect (cell);
                        if (o == chosen)
                        {
                            g.setColour (fx.enabled ? cellAccent : colour::textSecondary);
                            g.drawRect (cell, 1.0f);
                        }
                        g.setColour (o == chosen ? colour::textPrimary : colour::textSecondary);
                        if (! drawWaveGlyph (g, cell.reduced (cell.getWidth() * 0.25f, cell.getHeight() * 0.28f), juce::String::fromUTF8 (labels[(std::size_t) o].c_str())))
                            g.drawFittedText (texts[(std::size_t) o], cell.reduced (2.0f, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
                    }
                    break;
                }
                const auto rowH = r.getHeight() / (float) juce::jmax (1, (int) labels.size());
                for (int o = 0; o < (int) labels.size(); ++o)
                {
                    const auto row = juce::Rectangle<float> (r.getX(), r.getY() + rowH * (float) o, r.getWidth(), rowH);
                    const auto dot = juce::Rectangle<float> (7.0f * s, 7.0f * s).withCentre ({ row.getX() + 5.0f * s, row.getCentreY() });
                    if (o == chosen && fx.enabled)
                    {
                        g.setColour (accent.withAlpha (0.3f));
                        g.fillEllipse (dot.expanded (2.5f * s));
                    }
                    g.setColour (o == chosen ? (fx.enabled ? accent : colour::textSecondary) : juce::Colour (0xff0b0c0f));
                    g.fillEllipse (dot);
                    if (o != chosen)
                    {
                        g.setColour (colour::borderStrong);
                        g.drawEllipse (dot, 1.0f);
                    }
                    g.setColour (o == chosen ? colour::textPrimary : (hot ? colour::textSecondary : colour::textDisabled));
                    g.setFont (font (juce::jmax (8.0f, 9.5f * s)));
                    g.drawFittedText (texts[(std::size_t) o], row.withTrimmedLeft (12.0f * s).toNearestInt(),
                                      juce::Justification::centredLeft, 1, 0.7f);
                }
                break;
            }

            case Item::tabs:
            {
                const auto labels = plugui::optionLabels (item);
                const auto texts = optionTexts (item);
                const auto chosen = hasStyle (item, plugui::pageTabs) ? (item.steps > 0 ? (page / item.steps) % juce::jmax (1, (int) labels.size()) : page)
                                                                      : plugui::optionAt (item, value);
                const auto vertical = isVertical (item);
                const auto count = (float) juce::jmax (1, (int) labels.size());
                const auto w = vertical ? r.getWidth() : r.getWidth() / count;
                const auto h = vertical ? r.getHeight() / count : r.getHeight();
                skin::valuePlate (g, r, radius::md);
                for (int o = 0; o < (int) labels.size(); ++o)
                {
                    const auto cell = (vertical ? juce::Rectangle<float> (r.getX(), r.getY() + h * (float) o, w, h)
                                                : juce::Rectangle<float> (r.getX() + w * (float) o, r.getY(), w, h)).reduced (1.0f);
                    if (vertical) // a band selector (Maximus): the band's colour on the left edge
                    {
                        g.setColour (peqview::bandColour (o).withAlpha (o == chosen ? 1.0f : 0.45f));
                        g.fillRect (cell.withWidth (juce::jmax (2.0f, 3.0f * s)));
                    }
                    if (o == chosen)
                        skin::button (g, vertical ? cell.withTrimmedLeft (juce::jmax (2.0f, 3.0f * s)) : cell, true, false,
                                      vertical ? peqview::bandColour (o) : (fx.enabled ? accent : colour::textSecondary), radius::sm);
                    g.setColour (o == chosen ? colour::textPrimary : colour::textSecondary);
                    g.setFont (font (juce::jmax (8.0f, 10.0f * s), true));
                    g.drawFittedText (caps (texts[(std::size_t) o]), cell.toNearestInt(),
                                      juce::Justification::centred, 1, 0.7f);
                }
                break;
            }

            case Item::toggle:
            {
                const auto on = (value >= 0.5f) != hasStyle (item, plugui::inverted) && ! hasStyle (item, plugui::momentary);
                if (hasStyle (item, plugui::boxed) || hasStyle (item, plugui::momentary)) // a small lit button with its letter (FL's T / L buttons)
                {
                    if (on)
                    {
                        const auto c = fx.enabled ? accent : colour::textSecondary;
                        g.setColour (c.withAlpha (0.3f));
                        g.fillRoundedRectangle (r.expanded (1.5f), radius::sm + 1.5f);
                        g.setGradientFill (juce::ColourGradient (c.brighter (0.2f), 0.0f, r.getY(), c.darker (0.15f), 0.0f, r.getBottom(), false));
                        g.fillRoundedRectangle (r, radius::sm);
                    }
                    else
                        skin::button (g, r, false, hot, accent, radius::sm);
                    g.setColour (on ? colour::onAccent : colour::textSecondary);
                    if (item.label != nullptr)
                        ui::drawFittingText (g, caps (tr (item.label)), r, juce::Justification::centred, font (juce::jmax (8.0f, r.getHeight() * 0.8f), true), 6.5f);
                    break;
                }
                const auto box = juce::Rectangle<float> (16.0f * s, 9.0f * s).withCentre ({ r.getX() + 9.0f * s, r.getCentreY() });
                skin::switchPill (g, box, on, accent, fx.enabled);
                auto text = r.withTrimmedLeft (21.0f * s);
                g.setColour (on ? colour::textPrimary : (hot ? colour::textSecondary : colour::textDisabled));
                if (item.label != nullptr)
                    ui::drawFittingText (g, caps (tr (item.label)), text, juce::Justification::centredLeft, font (juce::jmax (8.0f, 9.5f * s), true), 7.0f);
                break;
            }

            case Item::stepper:
            {
                auto box = r;
                if (item.label != nullptr && ! hasStyle (item, plugui::noCaption))
                    caption (g, item, box.removeFromTop (12.0f * s));
                skin::valuePlate (g, box, radius::sm);
                if (hot)
                {
                    g.setColour (accent.withAlpha (0.5f));
                    g.drawRoundedRectangle (box.reduced (0.5f), radius::sm, 1.0f);
                }
                auto arrows = box.removeFromRight (juce::jmin (12.0f * s, box.getWidth() * 0.4f));
                g.setColour (colour::textSecondary);
                drawSmallArrow (g, arrows.removeFromTop (arrows.getHeight() * 0.5f), true);
                drawSmallArrow (g, arrows, false);
                g.setColour (colour::textPrimary);
                g.setFont (font (juce::jmax (8.0f, 11.0f * s), true));
                g.drawFittedText (valueText (param, value), box.toNearestInt(), juce::Justification::centred, 1, 0.6f);
                break;
            }

            case Item::display:
                paintDisplay (g, item, r, fx);
                break;

            case Item::menu:
            {
                if (kind == EffectKind::fruitySend) // the destination insert (FL's tinted box; "…" = none)
                {
                    const auto target = flbasic::sendTrack (value);
                    g.setColour ((target > 0 ? accent : colour::accentMuted).withAlpha (hot ? 1.0f : 0.85f));
                    g.fillRoundedRectangle (r, radius::sm * s);
                    g.setColour (target > 0 ? colour::onAccent : colour::textSecondary);
                    g.setFont (font (juce::jmax (8.0f, 9.0f * s), true));
                    g.drawFittedText (target > 0 ? insertName (target) : juce::String (juce::CharPointer_UTF8 ("\xe2\x80\xa6")),
                                      r.reduced (2.0f * s, 0.0f).toNearestInt(), juce::Justification::centred, 2, 0.6f);
                    break;
                }
                const auto chosen = plugui::optionAt (item, value);
                const auto off = chosen == 0 && kind == EffectKind::parametricEq;
                if (item.colour >= 0)
                {
                    const auto c = peqview::bandColour (item.colour);
                    g.setColour ((fx.enabled && ! off ? c : c.withSaturation (0.0f).darker (0.6f)).withAlpha (hot ? 1.0f : 0.85f));
                    g.fillRoundedRectangle (r, radius::sm * s);
                }
                else
                    skin::selector (g, r, hot, radius::md * s);
                g.setColour (colour::bg0);
                if (kind == EffectKind::parametricEq)
                    drawBandShape (g, r.reduced (2.0f * s, 4.0f * s), chosen);
                else
                {
                    // round 114: readable mode names (light on the control, bigger), arrows to step on wide menus
                    const auto labels = plugui::optionLabels (item);
                    auto textArea = r;
                    if (hasStepArrows (item))
                    {
                        const auto edge = 18.0f * s;
                        for (const auto left : { true, false })
                        {
                            const auto a = left ? r.withWidth (edge) : r.withLeft (r.getRight() - edge);
                            const auto c0 = a.getCentre();
                            juce::Path tri;
                            const auto h = 4.0f * s;
                            if (left) tri.addTriangle (c0.x + h * 0.7f, c0.y - h, c0.x + h * 0.7f, c0.y + h, c0.x - h * 0.7f, c0.y);
                            else tri.addTriangle (c0.x - h * 0.7f, c0.y - h, c0.x - h * 0.7f, c0.y + h, c0.x + h * 0.7f, c0.y);
                            g.setColour (a.contains (mouseAt) ? colour::textPrimary : colour::textSecondary);
                            g.fillPath (tri);
                        }
                        textArea = r.reduced (edge, 0.0f);
                    }
                    g.setColour (colour::textPrimary);
                    g.setFont (font (juce::jmax (9.0f, juce::jmin (12.0f * s, r.getHeight() * 0.55f)), true));
                    if (kind == EffectKind::lynxTune && param == lynxtune::scale && fx.params[lynxtune::moreScales] >= 0.25f)
                        g.drawFittedText (tr (fx.params[lynxtune::moreScales] >= 0.75f ? "Diminished" : "Whole tone"), textArea.toNearestInt(), juce::Justification::centred, 1, 0.6f);
                    else if (kind == EffectKind::hardwareInsert) // the device's channel names (round 164d)
                        g.drawFittedText (valueText (param, value), textArea.reduced (4.0f * s, 0.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
                    else if (chosen < (int) labels.size())
                        g.drawFittedText (tr (juce::String::fromUTF8 (labels[(std::size_t) chosen].c_str())), textArea.toNearestInt(), juce::Justification::centred, 1, 0.6f);
                }
                break;
            }
        }
    }

    // a small drawing of an EQ band's response (peak, shelves, cuts, band-pass, notch)
    static void drawBandShape (juce::Graphics& g, juce::Rectangle<float> r, int type)
    {
        juce::Path p;
        const auto x0 = r.getX(), x1 = r.getRight(), xm = r.getCentreX(), y0 = r.getY(), y1 = r.getBottom(), ym = r.getCentreY();
        switch (type)
        {
            case 1: p.startNewSubPath (x0, ym); p.quadraticTo (xm, y0 - (y1 - y0) * 0.3f, x1, ym); break;            // peak
            case 2: p.startNewSubPath (x0, y0); p.lineTo (xm - 2, y0); p.lineTo (xm + 2, ym); p.lineTo (x1, ym); break; // low shelf
            case 3: p.startNewSubPath (x0, ym); p.lineTo (xm - 2, ym); p.lineTo (xm + 2, y0); p.lineTo (x1, y0); break; // high shelf
            case 4: p.startNewSubPath (x0 + 2, y1); p.quadraticTo (xm - 1, y0, x1, y0); break;                          // low cut
            case 5: p.startNewSubPath (x0, y0); p.quadraticTo (xm + 1, y0, x1 - 2, y1); break;                          // high cut
            case 6: p.startNewSubPath (x0, y1); p.quadraticTo (xm, y0 - (y1 - y0) * 0.6f, x1, y1); break;                // band-pass
            case 7: p.startNewSubPath (x0, y0); p.quadraticTo (xm, y1 + (y1 - y0) * 0.6f, x1, y0); break;                // notch
            default: p.startNewSubPath (x0, ym); p.lineTo (x1, ym); break;                                             // off
        }
        g.strokePath (p, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // a small drawing of an LFO / oscillator wave named in English (false: not a wave name)
    static bool drawWaveGlyph (juce::Graphics& g, juce::Rectangle<float> r, const juce::String& name)
    {
        if (name != "Sine" && name != "Triangle" && name != "Square" && name != "Saw")
            return false;
        juce::Path p;
        constexpr int n = 32;
        for (int i = 0; i <= n; ++i)
        {
            const auto t = (float) i / (float) n;
            const auto y = name == "Sine" ? std::sin (t * juce::MathConstants<float>::twoPi)
                           : name == "Triangle" ? (t < 0.25f ? 4.0f * t : (t < 0.75f ? 2.0f - 4.0f * t : 4.0f * t - 4.0f))
                           : name == "Square" ? (t < 0.5f ? 1.0f : -1.0f)
                                              : 2.0f * t - 1.0f;
            const juce::Point<float> pt { r.getX() + r.getWidth() * t, r.getCentreY() - r.getHeight() * 0.5f * y };
            if (i == 0)
                p.startNewSubPath (pt);
            else
                p.lineTo (pt);
        }
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        return true;
    }

    static void drawSmallArrow (juce::Graphics& g, juce::Rectangle<float> r, bool up)
    {
        const auto c = r.getCentre();
        const auto w = juce::jmin (r.getWidth(), r.getHeight() * 2.0f) * 0.32f;
        juce::Path p;
        if (up)
            p.addTriangle (c.x - w, c.y + w * 0.5f, c.x + w, c.y + w * 0.5f, c.x, c.y - w * 0.5f);
        else
            p.addTriangle (c.x - w, c.y - w * 0.5f, c.x + w, c.y - w * 0.5f, c.x, c.y + w * 0.5f);
        g.fillPath (p);
    }

    // --- displays (graphs drawn from the parameters; FL's live analysers are not reproduced) -------------

    float param01 (const char* name) const { return valueOf (plugui::paramIndex (kind, name)); }

    // LYNX Tune's live display: the read-out on the left (note, cents, Hz, target, correction meter), the last 8 s of
    // sung (white) and corrected (accent) pitch on the right over the key's note lines
    void paintPitchDisplay (juce::Graphics& g, juce::Rectangle<float> r, const EffectSlot& fx, bool readout = true)
    {
        using namespace theme;
        const auto s = scale();
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        auto noteText = [] (int n) { return juce::String (names[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1); };

        std::vector<PitchPoint> history = pitchHistory;
        if (demoDisplays && history.empty()) // a sung phrase with vibrato and scoops, corrected toward the scale
            for (std::size_t i = 0; i < pitchHistoryLength; ++i)
            {
                const auto t = (float) i / 30.0f;
                const auto phrase = std::fmod (t, 2.7f);
                const auto base = t < 2.7f ? 64.0f : t < 5.4f ? 67.0f : 69.0f;
                const auto sung = phrase < 0.15f ? 0.0f : base - 0.6f * std::exp (-phrase * 5.0f) + 0.35f * std::sin (t * 20.0f) * juce::jmin (1.0f, phrase) + 0.18f;
                const auto shift = sung > 0.0f ? (base - sung) * (1.0f - std::exp (-phrase * 12.0f)) : 0.0f;
                history.push_back ({ sung, sung > 0.0f ? base : 0.0f, shift });
            }

        const auto last = history.empty() ? PitchPoint { 0.0f, 0.0f, 0.0f } : history.back();
        [[maybe_unused]] const auto voiced = last.sung > 0.5f;
        [[maybe_unused]] const auto ref = lynxtune::referenceHz (fx.params[(std::size_t) lynxtune::reference]);

        if (readout)
        {
            // --- read-out
            auto left = r.removeFromLeft (juce::jmin (176.0f * s, r.getWidth() * 0.34f)).reduced (8.0f * s, 6.0f * s);
            const auto noteArea = left.removeFromTop (left.getHeight() * 0.42f);
            g.setFont (font (juce::jmax (12.0f, 34.0f * s), true));
            g.setColour (voiced ? colour::textPrimary : colour::textDisabled);
            const auto nearest = (int) std::lround (last.sung);
            g.drawText (voiced ? noteText (nearest) : juce::String ("--"), noteArea.withTrimmedRight (noteArea.getWidth() * 0.4f), juce::Justification::centredLeft);
            g.setFont (font (juce::jmax (8.0f, 12.0f * s)));
            g.setColour (colour::textSecondary);
            if (voiced)
            {
                const auto cents = (int) std::lround ((last.sung - (float) nearest) * 100.0f);
                const auto hzNow = ref * std::exp2 ((last.sung - 69.0f) / 12.0f);
                const auto side = noteArea.withTrimmedLeft (noteArea.getWidth() * 0.6f);
                g.drawText ((cents > 0 ? "+" : "") + juce::String (cents) + " c", side.withHeight (side.getHeight() * 0.5f), juce::Justification::bottomLeft);
                g.drawText (juce::String (hzNow, 1) + " Hz", side.withTrimmedTop (side.getHeight() * 0.5f), juce::Justification::topLeft);
            }
            auto row = [&] (const juce::String& caption, const juce::String& value, juce::Colour c) {
                auto line = left.removeFromTop (juce::jmax (12.0f, 18.0f * s));
                g.setColour (colour::textDisabled);
                g.setFont (font (juce::jmax (7.0f, 10.0f * s), true));
                g.drawText (caption, line, juce::Justification::centredLeft);
                g.setColour (c);
                g.setFont (font (juce::jmax (8.0f, 12.0f * s), true));
                g.drawText (value, line, juce::Justification::centredRight);
            };
            row (caps (tr ("Target")), voiced && last.target > 0.5f ? noteText ((int) std::lround (last.target)) : juce::String ("--"), accent);
            const auto corrCents = (int) std::lround (last.shift * 100.0f);
            row (caps (tr ("Correction")), voiced ? (corrCents > 0 ? "+" : "") + juce::String (corrCents) + " c" : juce::String ("--"), colour::textPrimary);
            // the correction meter: ±100 cents from the centre
            auto bar = left.removeFromTop (juce::jmax (6.0f, 9.0f * s)).reduced (0.0f, 1.0f);
            skin::valuePlate (g, bar, radius::sm);
            g.setColour (colour::gridBar);
            g.drawVerticalLine ((int) bar.getCentreX(), bar.getY() - 2.0f, bar.getBottom() + 2.0f);
            if (voiced)
            {
                const auto amount = juce::jlimit (-1.0f, 1.0f, last.shift);
                const auto x0 = bar.getCentreX(), x1 = bar.getCentreX() + bar.getWidth() * 0.5f * amount;
                g.setColour ((fx.enabled ? accent : colour::textDisabled).withAlpha (0.9f));
                g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (x0, x1), bar.getY(), std::abs (x1 - x0) + 1.0f, bar.getHeight()), radius::sm);
            }

        }

        // --- graph
        auto graph = r.reduced (6.0f * s, 6.0f * s);
        skin::well (g, graph, radius::md * s, fx.enabled ? accent : juce::Colours::transparentBlack);
        // vertical range: ±7 semitones around the recent pitch (follows the voice)
        float centre = 0.0f;
        int count = 0;
        for (auto i = history.size() > 60 ? history.size() - 60 : 0; i < history.size(); ++i)
            if (history[i].sung > 0.5f) { centre += history[i].sung; ++count; }
        centre = count > 0 ? std::round (centre / (float) count) : (displayCentre > 0.0f ? displayCentre : 60.0f);
        displayCentre = displayCentre <= 0.0f ? centre : displayCentre + 0.15f * (centre - displayCentre);
        const auto span = 7.5f;
        auto yFor = [&] (float note) { return graph.getCentreY() - (note - displayCentre) / span * graph.getHeight() * 0.5f; };
        // note lanes: the scale's notes brighter, C labelled
        const auto mask = vocal::scaleMask ((vocal::Scale) lynxtune::scaleIndex (fx.params[(std::size_t) lynxtune::scale]),
                                            lynxtune::keyIndex (fx.params[(std::size_t) lynxtune::key]), 0xfff);
        g.saveState();
        g.reduceClipRegion (graph.toNearestInt());
        g.setFont (font (juce::jmax (7.0f, 9.0f * s)));
        for (int n = (int) std::floor (displayCentre - span) - 1; n <= (int) std::ceil (displayCentre + span) + 1; ++n)
        {
            const auto inScale = ((mask >> (((n % 12) + 12) % 12)) & 1) != 0;
            const auto on = fx.params[(std::size_t) (lynxtune::noteC + ((n % 12) + 12) % 12)] >= 0.5f;
            const auto y = yFor ((float) n);
            g.setColour (inScale && on ? colour::gridBar : colour::gridStep);
            g.drawHorizontalLine ((int) y, graph.getX(), graph.getRight());
            if (inScale && on)
            {
                g.setColour (colour::textDisabled);
                g.drawText (noteText (n), juce::Rectangle<float> (graph.getX() + 3.0f, y - 7.0f, 30.0f * s, 12.0f), juce::Justification::centredLeft);
            }
        }
        // the curves, newest on the right
        const auto stepX = graph.getWidth() / (float) (pitchHistoryLength - 1);
        const auto x0 = graph.getRight() - stepX * (float) (history.size() - 1);
        for (int which = 0; which < 2; ++which)
        {
            juce::Path p;
            bool open = false;
            for (std::size_t i = 0; i < history.size(); ++i)
            {
                const auto& h = history[i];
                if (h.sung <= 0.5f) { open = false; continue; }
                const auto x = x0 + stepX * (float) i;
                const auto y = yFor (which == 0 ? h.sung : h.sung + h.shift);
                if (! open) p.startNewSubPath (x, y);
                else p.lineTo (x, y);
                open = true;
            }
            if (which == 0)
            {
                g.setColour (colour::textSecondary.withAlpha (0.8f));
                g.strokePath (p, juce::PathStrokeType (1.2f * s));
            }
            else
            {
                skin::glowStroke (g, p, fx.enabled ? accent : colour::textDisabled, 2.0f * s, fx.enabled ? 1.0f : 0.0f);
                // round 126: the newest corrected pitch as a lit bead
                if (fx.enabled && ! history.empty() && history.back().sung > 0.5f)
                {
                    const juce::Point<float> head { x0 + stepX * (float) (history.size() - 1), yFor (history.back().sung + history.back().shift) };
                    skin::bloom (g, head, 14.0f * s, accent, 0.6f);
                    g.setColour (juce::Colours::white);
                    g.fillEllipse (juce::Rectangle<float> (5.0f * s, 5.0f * s).withCentre (head));
                }
            }
        }
        g.restoreState();
        if (history.empty() || std::none_of (history.begin(), history.end(), [] (const PitchPoint& h) { return h.sung > 0.5f; }))
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (juce::jmax (8.0f, 12.0f * s)));
            g.drawText (tr ("Play a voice or an instrument through the insert to see its pitch"), graph, juce::Justification::centred, true);
        }
        // legend
        g.setFont (font (juce::jmax (7.0f, 9.0f * s)));
        g.setColour (colour::textSecondary);
        g.drawText (tr ("input"), graph.removeFromTop (14.0f * s).removeFromRight (120.0f * s).withTrimmedRight (6.0f), juce::Justification::centredLeft);
        g.setColour (accent);
        g.drawText (tr ("corrected"), graph.withHeight (14.0f * s).withY (graph.getY() - 14.0f * s).removeFromRight (64.0f * s), juce::Justification::centredLeft);
    }

    // LYNX Tune's correction meter (round 120, the centre of the auto-mode workflow): a ring of segments from −100 to
    // +100 cents lit from the top toward the correction being applied, the target note's name in the middle and the
    // sung pitch's offset under it
    void paintPitchRing (juce::Graphics& g, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        PitchPoint last { 0.0f, 0.0f, 0.0f };
        if (tuneHold)
            last = holdPoint; // HOLD: the read-out frozen where it was clicked (round 128)
        else if (! pitchHistory.empty())
            last = pitchHistory.back();
        else if (demoDisplays)
            last = { 63.62f, 64.0f, 0.38f };
        const auto voiced = last.sung > 0.5f;
        const auto side = juce::jmin (r.getWidth(), r.getHeight());
        auto dial = r.withSizeKeepingCentre (side, side).reduced (4.0f * s);
        const auto c = dial.getCentre();
        const auto outer = dial.getWidth() * 0.5f, inner = outer * 0.74f;
        // round 126: the ring is the hero of the editor: a bloom of the accent behind it (stronger with the
        // correction), a deep bezel, a lit glass face
        const auto voicedCorr = voiced ? juce::jmin (1.0f, std::abs (last.shift)) : 0.0f;
        if (fx.enabled)
            skin::bloom (g, c, outer * 1.55f, accent, 0.5f + 0.3f * voicedCorr);
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillEllipse (dial.expanded (3.0f).translated (0.0f, 2.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff3a3e48), c.x, dial.getY() - 3.0f, juce::Colour (0xff111216), c.x, dial.getBottom() + 3.0f, false));
        g.fillEllipse (dial.expanded (3.0f));
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff050608), 0.0f, dial.getY(), juce::Colour (0xff121319), 0.0f, dial.getBottom(), false));
        g.fillEllipse (dial);
        if (fx.enabled && ! skin::reducedEffects())
        {
            juce::ColourGradient face (accent.withAlpha (0.10f + 0.12f * voicedCorr), c.x, c.y, accent.withAlpha (0.0f), c.x + inner, c.y, true);
            g.setGradientFill (face);
            g.fillEllipse (juce::Rectangle<float> (inner * 2.0f, inner * 2.0f).withCentre (c));
        }
        g.setColour (juce::Colours::black.withAlpha (0.7f));
        g.drawEllipse (dial.reduced (0.5f), 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawEllipse (juce::Rectangle<float> (inner * 2.0f - 6.0f, inner * 2.0f - 6.0f).withCentre (c), 1.0f);
        // 50 segments over 300° (−100 … +100 c), the top = 0
        constexpr int segments = 50;
        const auto sweep = juce::MathConstants<float>::pi * 5.0f / 3.0f;
        const auto corr = juce::jlimit (-1.0f, 1.0f, voiced ? last.shift : 0.0f);
        for (int k = 0; k < segments; ++k)
        {
            const auto u = ((float) k + 0.5f) / (float) segments * 2.0f - 1.0f; // −1 … +1
            const auto a = u * sweep * 0.5f;
            const auto lit = voiced && ((corr > 0.0f && u > 0.0f && u <= corr) || (corr < 0.0f && u < 0.0f && u >= corr));
            juce::Path seg;
            const auto w = sweep / (float) segments * 0.35f;
            seg.addCentredArc (c.x, c.y, (outer + inner) * 0.5f, (outer + inner) * 0.5f, 0.0f, a - w, a + w, true);
            const auto tipSeg = lit && std::abs (u) + 1.0f / (float) segments > std::abs (corr);
            if (lit && fx.enabled && ! skin::reducedEffects()) // a lit segment glows
            {
                g.setColour (accent.withAlpha (0.16f));
                g.strokePath (seg, juce::PathStrokeType ((outer - inner) * 1.5f));
                g.setColour (accent.withAlpha (0.3f));
                g.strokePath (seg, juce::PathStrokeType ((outer - inner) * 1.05f));
            }
            // lit: brighter toward the tip of the correction (the tip white-hot); unlit: dim glass LEDs
            const auto litColour = accent.interpolatedWith (juce::Colours::white, tipSeg ? 0.55f : 0.12f * std::abs (u));
            g.setColour (lit ? (fx.enabled ? litColour : colour::textDisabled) : juce::Colour (0xff22252c));
            g.strokePath (seg, juce::PathStrokeType ((outer - inner) * 0.8f));

        }
        g.setColour (colour::gridBar);
        g.drawLine (c.x, c.y - outer, c.x, c.y - inner, 1.5f);
        // scale marks
        g.setFont (font (juce::jmax (7.0f, 9.0f * s)));
        g.setColour (colour::textDisabled);
        for (const int m : { -100, -50, 0, 50, 100 })
        {
            const auto a = (float) m / 100.0f * sweep * 0.5f;
            const auto rr = outer + 9.0f * s;
            const juce::Point<float> pt { c.x + rr * std::sin (a), c.y - rr * std::cos (a) };
            g.drawText ((m > 0 ? "+" : "") + juce::String (m), juce::Rectangle<float> (28.0f * s, 12.0f * s).withCentre (pt), juce::Justification::centred);
        }
        // the note and the offset
        const auto target = (int) std::lround (last.target);
        g.setFont (font (juce::jmax (14.0f, inner * 0.66f), true));
        {
            const auto noteBox = juce::Rectangle<float> (inner * 1.6f, inner * 0.8f).withCentre (c.translated (0.0f, -inner * 0.08f));
            const auto noteName = voiced && last.target > 0.5f ? juce::String (names[((target % 12) + 12) % 12]) : juce::String ("--");
            if (voiced && fx.enabled && ! skin::reducedEffects()) // the note glows
            {
                g.setColour (accent.withAlpha (0.2f));
                for (const auto& o : { juce::Point<float> { -2.0f, 0.0f }, { 2.0f, 0.0f }, { 0.0f, -2.0f }, { 0.0f, 2.0f } })
                    g.drawText (noteName, noteBox.translated (o.x, o.y), juce::Justification::centred);
            }
            g.setColour (voiced ? (fx.enabled ? accent.interpolatedWith (juce::Colours::white, 0.3f) : colour::textSecondary) : colour::textDisabled);
            g.drawText (noteName, noteBox, juce::Justification::centred);
        }
        g.setFont (font (juce::jmax (8.0f, 11.0f * s)));
        g.setColour (colour::textSecondary);
        if (voiced)
        {
            const auto cents = (int) std::lround ((last.sung - (float) target) * 100.0f);
            g.drawText ((cents > 0 ? "+" : "") + juce::String (cents) + " c  " + juce::String (target / 12 - 1),
                        juce::Rectangle<float> (inner * 1.4f, 16.0f * s).withCentre (c.translated (0.0f, inner * 0.5f)), juce::Justification::centred);
        }
        else
            g.drawText (tr ("no pitch"), juce::Rectangle<float> (inner * 1.4f, 16.0f * s).withCentre (c.translated (0.0f, inner * 0.5f)), juce::Justification::centred);
        // HOLD (round 128): click the meter to freeze its read-out
        g.setFont (font (juce::jmax (7.0f, 9.0f * s), true));
        g.setColour (tuneHold ? accent : colour::textDisabled);
        g.drawText (tr ("HOLD"), juce::Rectangle<float> (inner, 12.0f * s).withCentre (c.translated (0.0f, inner * 0.78f)), juce::Justification::centred);
    }

    // --- LYNX Tune's keyboard (round 128; round 147: the Waves Tune Real-Time role's keyboard) ----------------------
    // A key edits its note name in every octave (the reference's Group Octaves, always on here): in BYPASS mode it
    // switches "pass uncorrected" (still a target, no correction), in REMOVE mode a click cycles the note's status like
    // the reference's — legal → illegal (to the nearest legal note) → illegal, pitch up → illegal, pitch down → legal.
    // LATCH off = the edit lasts only while the key is held (live use). Round 147: the view's navigation (− / + = fewer
    // / more octaves, ◀ / ▶ = scroll), the range bar over the keys (click or drag near a boundary: the notes outside
    // the range pass uncorrected), the range presets and Reset scale.
    int tuneFirstOctave = 2, tuneShownOctaves = 4; // the view: C2 … B5 by default (the editor's own, not saved)
    int tuneRangeDrag = -1;                       // 0 = the low boundary, 1 = the high one
    int tuneSendFlash = 0;                        // round 150: Send lit for a few repaints after a click
    enum TuneButton { tbBypass, tbRemove, tbReset, tbRange0, tbZoomOut = tbRange0 + lynxtune::rangePresets, tbZoomIn, tbLeft, tbRight, tbLatch,
                      tbSend, tbReceive, tbGroup, tbCount }; // round 150: Send / Receive; round 152: Group octaves
    juce::Rectangle<float> tuneKeysStrip (juce::Rectangle<float> r) const { return r.withHeight (22.0f * scale()); }
    juce::Rectangle<float> tuneKeysBed (juce::Rectangle<float> r) const { return r.withTrimmedTop (36.0f * scale()).reduced (2.0f * scale(), 2.0f * scale()); }
    juce::Rectangle<float> tuneRangeBar (juce::Rectangle<float> r) const
    {
        const auto s = scale();
        const auto bed = tuneKeysBed (r);
        return { bed.getX(), bed.getY() - 10.0f * s, bed.getWidth(), 8.0f * s };
    }
    juce::Rectangle<float> tuneModeButton (juce::Rectangle<float> r, int which) const
    {
        const auto s = scale();
        const auto strip = tuneKeysStrip (r).reduced (2.0f * s, 2.0f * s);
        auto at = [&] (float x, float w) { return juce::Rectangle<float> (strip.getX() + x * s, strip.getY(), w * s, strip.getHeight()); };
        // round 152: narrower buttons so Group octaves fits left of LATCH
        if (which == tbBypass) return at (0.0f, 58.0f);
        if (which == tbRemove) return at (62.0f, 58.0f);
        if (which == tbReset) return at (124.0f, 44.0f);
        if (which == tbSend) return at (172.0f, 40.0f);    // round 150
        if (which == tbReceive) return at (216.0f, 50.0f);
        if (which >= tbRange0 && which < tbZoomOut) return at (270.0f + 42.0f * (float) (which - tbRange0), 40.0f);
        if (which >= tbZoomOut && which <= tbRight) return at (568.0f + 22.0f * (float) (which - tbZoomOut), 20.0f);
        if (which == tbGroup) return at (658.0f, 46.0f); // round 152
        return strip.withLeft (strip.getRight() - 62.0f * s); // LATCH
    }
    int tuneVisibleWhites() const { return 7 * tuneShownOctaves; }
    int tuneFirstNote() const { return 12 * (tuneFirstOctave + 1); } // MIDI note of the first C shown (C4 = 60)
    float tuneNoteX (juce::Rectangle<float> bed, int note) const     // the left edge of a note's white key column
    {
        static const int whiteOf[12] { 0, 0, 1, 1, 2, 3, 3, 4, 4, 5, 5, 6 };
        const auto rel = note - tuneFirstNote();
        const auto oct = (int) std::floor ((double) rel / 12.0);
        const auto w = 7 * oct + whiteOf[((rel % 12) + 12) % 12];
        return bed.getX() + bed.getWidth() * (float) w / (float) tuneVisibleWhites();
    }
    // the MIDI note of the key under p, −1 = none
    int tuneKeyAt (juce::Rectangle<float> r, juce::Point<float> p) const
    {
        const auto bed = tuneKeysBed (r);
        if (! bed.contains (p))
            return -1;
        static const int whitePc[7] { 0, 2, 4, 5, 7, 9, 11 };
        const auto n = tuneVisibleWhites();
        const auto ww = bed.getWidth() / (float) n;
        if (p.y < bed.getY() + bed.getHeight() * 0.6f) // black keys first (on top)
            for (int w = 0; w < n; ++w)
            {
                const auto pc = whitePc[w % 7];
                if (pc == 4 || pc == 11)
                    continue;
                const auto bx = bed.getX() + ww * (float) (w + 1) - ww * 0.3f;
                if (p.x >= bx && p.x < bx + ww * 0.6f)
                    return tuneFirstNote() + 12 * (w / 7) + pc + 1;
            }
        const auto w = juce::jlimit (0, n - 1, (int) ((p.x - bed.getX()) / ww));
        return tuneFirstNote() + 12 * (w / 7) + whitePc[w % 7];
    }
    // the note under x in the range bar (the nearest white-key column), clamped to the view
    int tuneNoteAtX (juce::Rectangle<float> r, float x) const
    {
        const auto bed = tuneKeysBed (r);
        static const int whitePc[7] { 0, 2, 4, 5, 7, 9, 11 };
        const auto n = tuneVisibleWhites();
        const auto w = juce::jlimit (0, n - 1, (int) ((x - bed.getX()) / (bed.getWidth() / (float) n)));
        return tuneFirstNote() + 12 * (w / 7) + whitePc[w % 7];
    }
    void paintTuneKeys (juce::Graphics& g, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        const auto& p = fx.params;
        const auto sc = (vocal::Scale) lynxtune::effectiveScale (p[lynxtune::scale], p[lynxtune::moreScales]);
        const auto inScale = vocal::scaleMask (sc, lynxtune::keyIndex (p[lynxtune::key]), 0xfff);
        const auto lowNote = lynxtune::rangeNote (p[lynxtune::rangeLow]), highNote = lynxtune::rangeNote (p[lynxtune::rangeHigh]);
        // the strip: modes, Reset scale, the range presets, the view's navigation, latch
        for (int b = 0; b < tbCount; ++b)
        {
            const auto br = tuneModeButton (r, b);
            bool lit = false;
            juce::String name;
            if (b == tbBypass) { lit = ! tuneRemove; name = tr ("Bypass"); }
            else if (b == tbRemove) { lit = tuneRemove; name = tr ("Remove"); }
            else if (b == tbReset) name = tr ("Reset");
            else if (b == tbSend) { lit = tuneSendFlash > 0; name = tr ("Send"); }
            else if (b == tbReceive) { lit = p[lynxtune::receiveScale] >= 0.5f; name = tr ("Receive"); }
            else if (b == tbGroup) { lit = lynxtune::groupOctavesOn (p.data()); name = tr ("Group"); }
            else if (b >= tbRange0 && b < tbZoomOut)
            {
                const auto [lo, hi] = lynxtune::rangePresetNotes (b - tbRange0);
                lit = lo == lowNote && hi == highNote;
                name = tr (lynxtune::rangePresetName (b - tbRange0));
            }
            else if (b == tbZoomOut) name = "-";
            else if (b == tbZoomIn) name = "+";
            else if (b == tbLeft) name = "<";
            else if (b == tbRight) name = ">";
            else { lit = tuneLatch; name = tr ("Latch"); }
            skin::button (g, br, lit, br.contains (mouseAt), b == tbBypass ? juce::Colour (0xffe0a040) : accent, radius::sm * s);
            g.setColour (lit ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (juce::jmax (8.0f, 10.0f * s), true));
            g.drawText (name, br, juce::Justification::centred);
        }
        // the range bar: the notes inside the range lit, the two boundaries as handles
        const auto bed = tuneKeysBed (r);
        {
            const auto rb = tuneRangeBar (r);
            g.setColour (colour::bg0);
            g.fillRoundedRectangle (rb, 2.0f * s);
            const auto x0 = juce::jlimit (rb.getX(), rb.getRight(), tuneNoteX (bed, lowNote));
            const auto x1 = juce::jlimit (rb.getX(), rb.getRight(), tuneNoteX (bed, highNote + 1));
            if (x1 > x0)
            {
                g.setColour (accent.withAlpha (fx.enabled ? 0.55f : 0.25f));
                g.fillRoundedRectangle (juce::Rectangle<float> (x0, rb.getY(), x1 - x0, rb.getHeight()), 2.0f * s);
            }
            g.setColour (colour::textPrimary);
            for (const auto x : { x0, x1 })
                if (x > rb.getX() && x < rb.getRight())
                    g.fillRect (juce::Rectangle<float> (3.0f * s, rb.getHeight() + 4.0f * s).withCentre ({ x, rb.getCentreY() }));
        }
        // keys
        const auto n = tuneVisibleWhites();
        const auto ww = bed.getWidth() / (float) n;
        static const int whitePc[7] { 0, 2, 4, 5, 7, 9, 11 };
        const auto sungNote = pitchHistory.empty() || pitchHistory.back().sung < 0.5f ? -1 : (int) std::lround (pitchHistory.back().target);
        // round 152: each key's status in effect (its own when Group octaves is off)
        const auto mask = tuneMask (p);
        auto status = [&] (int note) { return lynxtune::noteStatus (p.data(), note, mask); };
        auto legalNote = [&] (int note) { const auto st = status (note); return st == lynxtune::KeyStatus::legal || st == lynxtune::KeyStatus::bypass; };
        auto keyColour = [&] (int note, bool black) {
            const auto bypassed = status (note) == lynxtune::KeyStatus::bypass;
            auto c = black ? juce::Colour (0xff1c1e24) : juce::Colour (0xffd9dbe0);
            if (! legalNote (note))
                c = black ? juce::Colour (0xff101114) : juce::Colour (0xff5a5d66); // removed / not in the scale
            else if (bypassed)
                c = black ? juce::Colour (0xff6b4d16) : juce::Colour (0xffe6be6e);
            if (note < lowNote || note > highNote) // outside the range: passes uncorrected
                c = c.interpolatedWith (colour::bg0, 0.55f);
            return c;
        };
        auto mark = [&] (juce::Rectangle<float> k, int note) {
            const auto pc = note % 12;
            const auto inRange = note >= lowNote && note <= highNote;
            const auto st = status (note);
            const bool own = ! lynxtune::groupOctavesOn (p.data()) && lynxtune::keyOverride (p.data(), note) != lynxtune::KeyStatus::follow;
            if (legalNote (note) && fx.enabled && inRange) // a target: the accent line at the key's top
            {
                g.setColour (st == lynxtune::KeyStatus::bypass ? juce::Colour (0xffe0a040) : accent);
                g.fillRect (k.withHeight (3.0f * s));
            }
            if (own) // round 152: a key with its own status (Group octaves off): an outline
            {
                g.setColour (accent.withAlpha (0.8f));
                g.drawRoundedRectangle (k.reduced (1.0f * s), 2.0f * s, 1.2f * s);
            }
            if (! legalNote (note))
            {
                const auto dir = st == lynxtune::KeyStatus::removedUp ? lynxtune::NoteDir::up
                                 : (st == lynxtune::KeyStatus::removedDown ? lynxtune::NoteDir::down : lynxtune::NoteDir::nearest);
                const auto x = juce::Rectangle<float> (7.0f * s, 7.0f * s).withCentre ({ k.getCentreX(), k.getY() + 10.0f * s });
                g.setColour (accent.withAlpha (0.9f));
                if (dir != lynxtune::NoteDir::nearest) // illegal, pitch up / down: an arrow
                {
                    juce::Path a;
                    const auto up = dir == lynxtune::NoteDir::up;
                    a.addTriangle (x.getX(), up ? x.getBottom() : x.getY(), x.getRight(), up ? x.getBottom() : x.getY(), x.getCentreX(), up ? x.getY() : x.getBottom());
                    g.fillPath (a);
                }
                else if (((inScale >> pc) & 1u) != 0 || own) // removed by hand: a cross
                {
                    g.drawLine (x.getX(), x.getY(), x.getRight(), x.getBottom(), 1.5f * s);
                    g.drawLine (x.getRight(), x.getY(), x.getX(), x.getBottom(), 1.5f * s);
                }
            }
            if (note == sungNote && fx.enabled)
            {
                g.setColour (accent.withAlpha (0.55f));
                g.fillEllipse (juce::Rectangle<float> (7.0f * s, 7.0f * s).withCentre ({ k.getCentreX(), k.getBottom() - 9.0f * s }));
            }
        };
        for (int w = 0; w < n; ++w)
        {
            const auto note = tuneFirstNote() + 12 * (w / 7) + whitePc[w % 7];
            const auto k = juce::Rectangle<float> (bed.getX() + ww * (float) w, bed.getY(), ww - 1.0f, bed.getHeight());
            g.setColour (keyColour (note, false));
            g.fillRoundedRectangle (k, 2.0f * s);
            mark (k, note);
            if (note % 12 == 0)
            {
                g.setColour (juce::Colour (0xff3a3d45));
                g.setFont (font (juce::jmax (7.0f, 9.0f * s)));
                g.drawText ("C" + juce::String (note / 12 - 1), k.withTrimmedBottom (2.0f * s).withTop (k.getBottom() - 14.0f * s), juce::Justification::centred);
            }
        }
        for (int w = 0; w < n; ++w)
        {
            const auto pc = whitePc[w % 7];
            if (pc == 4 || pc == 11)
                continue;
            const auto note = tuneFirstNote() + 12 * (w / 7) + pc + 1;
            const auto k = juce::Rectangle<float> (bed.getX() + ww * (float) (w + 1) - ww * 0.3f, bed.getY(), ww * 0.6f, bed.getHeight() * 0.6f);
            g.setColour (keyColour (note, true));
            g.fillRoundedRectangle (k, 2.0f * s);
            mark (k, note);
        }
    }
    void tuneSetRangeAt (juce::Rectangle<float> r, float x, bool pickNearer)
    {
        const auto* fx = current();
        if (fx == nullptr)
            return;
        const auto note = tuneNoteAtX (r, x);
        const auto lo = lynxtune::rangeNote (fx->params[lynxtune::rangeLow]), hi = lynxtune::rangeNote (fx->params[lynxtune::rangeHigh]);
        if (pickNearer)
            tuneRangeDrag = std::abs (note - lo) <= std::abs (note - hi) ? 0 : 1;
        if (tuneRangeDrag == 0)
            setParams ({ { lynxtune::rangeLow, lynxtune::rangeNoteValue (juce::jmin (note, hi)) } });
        else if (tuneRangeDrag == 1)
            setParams ({ { lynxtune::rangeHigh, lynxtune::rangeNoteValue (juce::jmax (note, lo)) } });
        const auto shown = tuneRangeDrag == 0 ? lynxtune::rangeLow : lynxtune::rangeHigh;
        ui::showValueTip (*this, tuneRangeBar (r).toNearestInt(), tr ("Range") + " " + juce::String (lynxtune::paramText (shown, current()->params[(std::size_t) shown])));
    }
    // round 152: the pitch classes that are targets (the scale on the key, minus the notes switched off)
    static std::uint16_t tuneMask (const EffectParams& p)
    {
        std::uint16_t notes = 0;
        for (int k = 0; k < 12; ++k)
            if (p[(std::size_t) (lynxtune::noteC + k)] >= 0.5f) notes = (std::uint16_t) (notes | (1u << k));
        const auto sc = (vocal::Scale) lynxtune::effectiveScale (p[lynxtune::scale], p[lynxtune::moreScales]);
        return (std::uint16_t) (vocal::scaleMask (sc, lynxtune::keyIndex (p[lynxtune::key]), notes) & notes);
    }
    void tuneKeyClick (juce::Rectangle<float> r, juce::Point<float> at)
    {
        const auto* fx = current();
        if (fx == nullptr)
            return;
        for (int b = 0; b < tbCount; ++b)
            if (tuneModeButton (r, b).contains (at))
            {
                if (b == tbLatch)
                    tuneLatch = ! tuneLatch;
                else if (b == tbSend) // round 150: this scale to every other LYNX Tune with Receive on
                {
                    engine.editSong ([t = track, sl = slot] (Song& song) { lynxtune::sendScaleToAll (song, t, sl); });
                    tuneSendFlash = 8;
                }
                else if (b == tbReceive) // on: take the scale sent last now, and the ones sent later
                {
                    const bool nowOn = fx->params[lynxtune::receiveScale] < 0.5f;
                    std::vector<std::pair<int, float>> c { { lynxtune::receiveScale, nowOn ? 1.0f : 0.0f } };
                    lynxtune::SharedScale sh;
                    if (nowOn && lynxtune::publishedScale (sh))
                        for (std::size_t i = 0; i < sh.values.size(); ++i)
                            c.push_back ({ lynxtune::sharedScaleList()[i], sh.values[i] });
                    setParams (c);
                }
                else if (b == tbBypass || b == tbRemove)
                    tuneRemove = b == tbRemove;
                else if (b == tbGroup) // round 152: Group octaves on / off (the keys' own statuses are kept)
                    setParams ({ { lynxtune::groupOctaves, lynxtune::groupOctavesOn (fx->params.data()) ? 0.0f : 1.0f } });
                else if (b == tbReset) // every note of the scale legal again, nothing bypassed, no direction
                {
                    std::vector<std::pair<int, float>> c;
                    for (int k = 0; k < lynxtune::keyMapParams; ++k) // round 152: and no key of its own
                        c.push_back ({ lynxtune::keyMap1 + k, 0.0f });
                    for (int k = 0; k < 12; ++k)
                    {
                        c.push_back ({ lynxtune::noteC + k, 1.0f });
                        c.push_back ({ lynxtune::bypassC + k, 0.0f });
                        c.push_back ({ lynxtune::noteDirC + k, 0.0f });
                    }
                    setParams (c);
                }
                else if (b >= tbRange0 && b < tbZoomOut)
                {
                    // a range preset: the keyboard's range, and the detector's input type that covers it
                    const auto k = b - tbRange0;
                    const auto [lo, hi] = lynxtune::rangePresetNotes (k);
                    std::vector<std::pair<int, float>> c { { lynxtune::rangeLow, lynxtune::rangeNoteValue (lo) }, { lynxtune::rangeHigh, lynxtune::rangeNoteValue (hi) } };
                    static constexpr float inputType[lynxtune::rangePresets] { 2.0f / 3.0f, 2.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 0.0f, 0.0f, -1.0f };
                    if (inputType[k] >= 0.0f)
                        c.push_back ({ lynxtune::range, inputType[k] });
                    setParams (c);
                }
                else if (b == tbZoomOut) tuneShownOctaves = juce::jmin (7, tuneShownOctaves + 1);
                else if (b == tbZoomIn) tuneShownOctaves = juce::jmax (2, tuneShownOctaves - 1);
                else if (b == tbLeft) tuneFirstOctave = juce::jmax (0, tuneFirstOctave - 1);
                else if (b == tbRight) tuneFirstOctave = juce::jmin (9 - tuneShownOctaves, tuneFirstOctave + 1);
                tuneFirstOctave = juce::jlimit (0, juce::jmax (0, 9 - tuneShownOctaves), tuneFirstOctave);
                repaint();
                return;
            }
        if (tuneRangeBar (r).expanded (0.0f, 2.0f * scale()).contains (at))
        {
            tuneSetRangeAt (r, at.x, true);
            return;
        }
        const auto note = tuneKeyAt (r, at);
        if (note < 0)
            return;
        if (! lynxtune::groupOctavesOn (fx->params.data())) // round 152: Group octaves off — this key alone
        {
            auto copy = fx->params;
            const auto st = lynxtune::noteStatus (copy.data(), note, tuneMask (copy));
            const auto next = tuneRemove ? lynxtune::nextRemoveStatus (st)
                                         : (st == lynxtune::KeyStatus::bypass ? lynxtune::KeyStatus::legal : lynxtune::KeyStatus::bypass);
            const auto mp = lynxtune::keyMap1 + juce::jlimit (0, lynxtune::keyMapParams - 1, (note - lynxtune::keyMapFirst) / lynxtune::keyMapPerParam);
            tuneHeldMap = tuneLatch ? -1 : mp;
            tuneHeldMapValue = copy[(std::size_t) mp];
            lynxtune::setKeyOverride (copy.data(), note, next);
            setParams ({ { mp, copy[(std::size_t) mp] } });
            return;
        }
        const auto pc = note % 12;
        const auto onP = lynxtune::noteC + pc, byP = lynxtune::bypassC + pc, dirP = lynxtune::noteDirC + pc;
        const auto on = fx->params[(std::size_t) onP] >= 0.5f, by = fx->params[(std::size_t) byP] >= 0.5f;
        const auto dir = lynxtune::noteDir (fx->params[(std::size_t) dirP]);
        tuneHeld = tuneLatch ? -1 : pc;
        tuneHeldValues = { fx->params[(std::size_t) onP], fx->params[(std::size_t) byP] };
        tuneHeldDir = fx->params[(std::size_t) dirP];
        if (tuneRemove)
        {
            // the reference's status cycle: legal → illegal → illegal, pitch up → illegal, pitch down → legal (a note
            // outside the scale stays illegal: only its direction cycles)
            const auto sc = (vocal::Scale) lynxtune::effectiveScale (fx->params[lynxtune::scale], fx->params[lynxtune::moreScales]);
            const bool scaleNote = ((vocal::scaleMask (sc, lynxtune::keyIndex (fx->params[lynxtune::key]), 0xfff) >> pc) & 1u) != 0;
            const auto nextDir = dir == lynxtune::NoteDir::nearest ? 0.5f : (dir == lynxtune::NoteDir::up ? 1.0f : 0.0f);
            if (scaleNote && on)
                setParams ({ { onP, 0.0f }, { byP, 0.0f }, { dirP, 0.0f } });
            else if (scaleNote && dir == lynxtune::NoteDir::down)
                setParams ({ { onP, 1.0f }, { byP, 0.0f }, { dirP, 0.0f } });
            else
                setParams ({ { onP, on ? 1.0f : 0.0f }, { byP, 0.0f }, { dirP, nextDir } });
        }
        else
            setParams ({ { onP, 1.0f }, { byP, by && on ? 0.0f : 1.0f }, { dirP, 0.0f } });
    }
    void tuneKeyRelease()
    {
        if (tuneRangeDrag >= 0)
        {
            tuneRangeDrag = -1;
            ui::hideValueTip();
        }
        if (tuneHeldMap >= 0) // round 152
        {
            setParams ({ { tuneHeldMap, tuneHeldMapValue } });
            tuneHeldMap = -1;
        }
        if (tuneHeld < 0)
            return;
        setParams ({ { lynxtune::noteC + tuneHeld, tuneHeldValues.first }, { lynxtune::bypassC + tuneHeld, tuneHeldValues.second },
                     { lynxtune::noteDirC + tuneHeld, tuneHeldDir } });
        tuneHeld = -1;
    }
    juce::String tuneKeysHint (juce::Rectangle<float> r, juce::Point<float> at) const
    {
        for (int b = 0; b < tbCount; ++b)
            if (tuneModeButton (r, b).contains (at))
            {
                if (b == tbBypass) return tr ("Bypass mode: a key switches its note between corrected and passed as sung");
                if (b == tbRemove) return tr ("Remove mode: a key cycles its note: legal, illegal (to the nearest note), illegal pitch up, illegal pitch down");
                if (b == tbReset) return tr ("Reset scale: every note of the scale legal again, nothing bypassed");
                if (b == tbSend) return tr ("Send: copy this key, scale and keyboard to every other LYNX Tune whose Receive is on");
                if (b == tbReceive) return tr ("Receive: take the scale another LYNX Tune sends (switching it on takes the last one sent)");
                if (b == tbGroup) return tr ("Group octaves: on = a key edits its note in every octave; off = each key keeps its own status");
                if (b >= tbRange0 && b < tbZoomOut) return tr ("Range preset: notes sung outside the range pass uncorrected (Generic: the whole keyboard)");
                if (b == tbZoomOut || b == tbZoomIn) return tr ("Keyboard view: show more / fewer octaves");
                if (b == tbLeft || b == tbRight) return tr ("Keyboard view: scroll down / up an octave");
                return tr ("Latch: off = a key's edit lasts only while it is held");
            }
        if (tuneRangeBar (r).expanded (0.0f, 2.0f * scale()).contains (at))
            return tr ("Range: drag a boundary; notes sung outside it pass uncorrected");
        if (current() != nullptr && ! lynxtune::groupOctavesOn (current()->params.data())) // round 152
            return tuneRemove ? tr ("Click a key: change the status of this key only") : tr ("Click a key: bypass / correct this key only");
        return tuneRemove ? tr ("Click a key: change that note's status in every octave") : tr ("Click a key: bypass / correct that note in every octave");
    }
    // the hardware insert's round-trip read-out (round 164d): the delay in samples and ms, then what the ping says or what
    // is missing for the insert to work
    void paintHwPing (juce::Graphics& g, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        g.setColour (colour::bg0);
        g.fillRoundedRectangle (r, radius::sm * s);
        auto area = r.reduced (8.0f * s, 6.0f * s);
        const auto samples = hwinsert::roundTripOf (fx.params[hwinsert::roundTrip]);
        const auto rate = engine.getTransport().sampleRate.load();
        g.setColour (samples > 0 ? colour::textPrimary : colour::textSecondary);
        g.setFont (font (juce::jmax (12.0f, 22.0f * s), true));
        g.drawText (juce::String (samples) + " " + tr ("smp"), area.removeFromTop (28.0f * s), juce::Justification::centred, false);
        g.setColour (colour::textSecondary);
        g.setFont (font (juce::jmax (8.0f, 11.0f * s)));
        g.drawText (rate > 0.0 ? juce::String (1000.0 * samples / rate, 2) + " ms" : juce::String(), area.removeFromTop (16.0f * s), juce::Justification::centred, false);

        // the state (newest first): the ping running or its outcome, then what the routing lacks
        const auto ping = liveMeter (hwinsert::pingState);
        const auto flags = juce::roundToInt (liveMeter (hwinsert::status) * 16.0f);
        const auto outCh = hwinsert::channelOf (fx.params[hwinsert::output]), inCh = hwinsert::channelOf (fx.params[hwinsert::input]);
        juce::String text;
        auto warn = false;
        if (engine.getTransport().sampleRate.load() <= 0.0 || engine.getOutputChannelNames().empty())
            text = tr ("No audio device is open"), warn = true;
        else if (outCh < 0 || inCh < 0)
            text = tr ("Choose the output and the input your gear is wired to"), warn = true;
        else if ((flags & 2) != 0)
            text = tr ("The output is not open on this device"), warn = true;
        else if ((flags & 4) != 0)
            text = tr ("The input is not open on this device"), warn = true;
        else if (ping > 0.2f && ping < 0.3f)
            text = tr ("Pinging...");
        else if (ping > 0.6f && ping < 0.7f)
            text = tr ("Ping failed: no click came back (check the cable and the return level)"), warn = true;
        else if (ping > 0.7f && ping < 0.8f)
            text = tr ("Ping failed: clicks were missing"), warn = true;
        else if (ping > 0.8f)
            text = tr ("Ping failed: the round trip was not stable"), warn = true;
        else if (samples == 0)
            text = tr ("Not measured yet: press Ping"), warn = true;
        else if (ping > 0.4f && ping < 0.6f)
            text = tr ("Measured: delay compensated");
        else
            text = tr ("Delay compensated (PDC)");
        g.setColour (warn ? juce::Colour (0xffe5a03b) : colour::textSecondary);
        g.setFont (font (juce::jmax (8.0f, 10.0f * s)));
        g.drawFittedText (text, area.toNearestInt(), juce::Justification::centred, 3, 0.8f);
    }

    // applies a ping's result once (the round trip becomes the slot's delay: undoable, saved with the project)
    void hwPingTick()
    {
        const auto serial = juce::roundToInt (liveMeter (hwinsert::pingSerial) * 1024.0f);
        if (hwSerial < 0)
        {
            hwSerial = serial;
            return;
        }
        if (serial == hwSerial)
            return;
        hwSerial = serial;
        if (std::abs (liveMeter (hwinsert::pingState) - 0.5f) < 0.01f)
        {
            const auto samples = hwinsert::roundTripOf (liveMeter (hwinsert::pingSamples));
            setValue (hwinsert::roundTrip, hwinsert::roundTripValue (samples));
            for (const auto& item : activeLayout().items)
                if (item.type == Item::display && item.display != nullptr && std::strcmp (item.display, "hwping") == 0)
                    ui::flashValueTip (*this, rectOf (item).toNearestInt(), tr ("Round trip: {0} samples", { juce::String (samples) }));
        }
        repaint();
    }
    int hwSerial = -1; // the last ping result seen (−1: not read yet)

    void paintAutoKey (juce::Graphics& g, juce::Rectangle<float> r)
    {
        using namespace theme;
        const auto s = scale();
        skin::button (g, r, autoKeyFlash > 0, r.contains (mouseAt), accent, radius::sm * s);
        g.setColour (colour::textPrimary);
        g.setFont (font (juce::jmax (8.0f, 10.0f * s), true));
        g.drawText (tr ("Auto Key"), r, juce::Justification::centred);
    }
    // LEARN (round 135): while LYNX Tune's "Learn scale" is on, every pitch class held on its MIDI channel joins the
    // custom scale (switching learning on starts a new scale)
    void learnScaleFromMidi()
    {
        const auto* fx = current();
        if (fx == nullptr || fx->kind != EffectKind::lynxTune)
            return;
        const bool on = fx->params[(std::size_t) lynxtune::learnMidi] >= 0.5f;
        if (! on)
        {
            learning = false;
            return;
        }
        if (! learning)
        {
            learning = true;
            learned = 0;
        }
        const auto held = (unsigned) std::lround (liveMeter (5) * 4096.0f) & 0xfffu;
        if ((held & ~learned) == 0)
            return;
        learned |= held;
        std::vector<std::pair<int, float>> changes { { lynxtune::scale, (float) (int) vocal::Scale::custom / (float) (vocal::scaleCount - 1) },
                                                     { lynxtune::moreScales, 0.0f } };
        for (int pc = 0; pc < 12; ++pc)
        {
            changes.push_back ({ lynxtune::noteC + pc, ((learned >> pc) & 1u) != 0 ? 1.0f : 0.0f });
            changes.push_back ({ lynxtune::bypassC + pc, 0.0f });
        }
        setParams (changes);
    }
    bool learning = false;
    unsigned learned = 0;

    // AUTO KEY: the key and major / minor scale of what was sung while the editor was open (time per pitch class)
    void autoKey (juce::Rectangle<float> r)
    {
        double total = 0.0;
        for (const auto x : keyHistogram)
            total += x;
        if (total < 30.0) // about a second of voiced singing
        {
            ui::flashValueTip (*this, r.toNearestInt(), tr ("Play the vocal for a few seconds first"));
            return;
        }
        const auto [k, mode] = vocal::detectKey (keyHistogram);
        if (k < 0)
            return;
        std::vector<std::pair<int, float>> changes { { lynxtune::key, (float) k / 11.0f },
                                                     { lynxtune::scale, (float) (int) mode / (float) (vocal::scaleCount - 1) },
                                                     { lynxtune::moreScales, 0.0f } };
        for (int pc = 0; pc < 12; ++pc)
        {
            changes.push_back ({ lynxtune::noteC + pc, 1.0f });
            changes.push_back ({ lynxtune::bypassC + pc, 0.0f });
        }
        setParams (changes);
        autoKeyFlash = 20;
        static const char* names[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        ui::flashValueTip (*this, r.toNearestInt(), juce::String (names[k]) + " " + tr (mode == vocal::Scale::major ? "Major" : "Minor"));
    }

    // the live meter `output` of this slot's effect (published by the audio thread; 0 when not playing)
    float liveMeter (int output) const
    {
        const auto* times = engine.getProcessTimes();
        const auto key = (std::size_t) (track * maxEffectSlots + slot) * ProcessTimes::meterOutputs + (std::size_t) juce::jlimit (0, ProcessTimes::meterOutputs - 1, output);
        return times != nullptr && key < times->effectMeters.size() ? times->effectMeters[key].load (std::memory_order_relaxed) : 0.0f;
    }

    // the captions of a radio's options (Gross Beat: the slot names saved in the state)
    std::vector<juce::String> optionTexts (const LayoutItem& item) const
    {
        std::vector<juce::String> out;
        const auto labels = plugui::optionLabels (item);
        // the whole list is one translation ("Mono|Stereo|Ping pong|Off"); single words are translated alone
        const auto whole = juce::StringArray::fromTokens (tr (juce::String::fromUTF8 (item.options != nullptr ? item.options : "")), "|", "");
        for (std::size_t o = 0; o < labels.size(); ++o)
            out.push_back (whole.size() == (int) labels.size() ? whole[(int) o] : tr (juce::String::fromUTF8 (labels[o].c_str())));
        if (kind == EffectKind::grossBeat && item.columns > 0)
            if (const auto* fx = current())
            {
                const auto names = grossbeat::slotNames (fx->state, std::strcmp (item.param, "Volume slot") == 0);
                for (std::size_t i = 0; i < names.size() && i < out.size(); ++i)
                    if (! names[i].empty())
                        out[i] = juce::String::fromUTF8 (names[i].c_str());
            }
        return out;
    }

    // the cell of option `o` in a radio grid
    juce::Rectangle<float> gridCell (const LayoutItem& item, int o, int count) const
    {
        const auto r = rectOf (item);
        const auto rows = (count + item.columns - 1) / item.columns;
        const auto w = r.getWidth() / (float) item.columns, h = r.getHeight() / (float) juce::jmax (1, rows);
        return { r.getX() + w * (float) (o % item.columns), r.getY() + h * (float) (o / item.columns), w, h };
    }

    void paintDisplay (juce::Graphics& g, const LayoutItem& item, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        const juce::String id (item.display != nullptr ? item.display : "");
        if (id != "faderscale") // round 137: the fader scale is bare text beside its fader
            skin::well (g, r, radius::md * s, fx.enabled ? accent : juce::Colours::transparentBlack);
        const auto curveColour = fx.enabled ? accent : colour::textDisabled;
        const auto inner = r.reduced (6.0f * s);
        if (dynui::paint (g, id, r, s, fx, [this] (int k) { return liveMeter (k); }, dynHistory)) // the dynamics suite
            return;

        auto grid = [&] (int columns, int rows) {
            g.setColour (colour::gridBeat);
            for (int i = 1; i < columns; ++i)
                g.drawVerticalLine ((int) (inner.getX() + inner.getWidth() * (float) i / (float) columns), inner.getY(), inner.getBottom());
            for (int i = 1; i < rows; ++i)
                g.drawHorizontalLine ((int) (inner.getY() + inner.getHeight() * (float) i / (float) rows), inner.getX(), inner.getRight());
        };

        auto strokeCurve = [&] (const std::function<float (float)>& f, juce::Rectangle<float> area, bool fillBelow) {
            juce::Path p;
            const auto steps = juce::jmax (16, (int) area.getWidth() / 2);
            for (int i = 0; i <= steps; ++i)
            {
                const auto x = (float) i / (float) steps;
                const auto y = juce::jlimit (0.0f, 1.0f, f (x));
                const juce::Point<float> pt { area.getX() + area.getWidth() * x, area.getBottom() - area.getHeight() * y };
                if (i == 0)
                    p.startNewSubPath (pt);
                else
                    p.lineTo (pt);
            }
            if (fillBelow)
            {
                auto fill = p;
                fill.lineTo (area.getRight(), area.getBottom());
                fill.lineTo (area.getX(), area.getBottom());
                fill.closeSubPath();
                g.setGradientFill (juce::ColourGradient (curveColour.withAlpha (0.28f), 0.0f, area.getY(), curveColour.withAlpha (0.02f), 0.0f, area.getBottom(), false));
                g.fillPath (fill);
            }
            skin::glowStroke (g, p, curveColour, juce::jmax (1.5f, 2.0f * s), fx.enabled ? 1.0f : 0.0f);
        };

        if (id == "limiter" || id == "compressor" || id == "dynamics")
        {
            // static transfer curve: input level (x, −60…0 dB) to output level (y)
            grid (6, 4);
            const auto threshold01 = id == "limiter" ? param01 ("Ceiling") : param01 (id == "compressor" ? "Threshold" : "Comp. threshold");
            const auto ratio01 = id == "limiter" ? 1.0f : param01 ("Ratio");
            const auto t = juce::jlimit (0.05f, 1.0f, threshold01);
            const auto ratio = 1.0f + ratio01 * 19.0f;
            strokeCurve ([t, ratio] (float x) { return x <= t ? x : t + (x - t) / ratio; }, inner, true);
            g.setColour (colour::gridBar);
            g.drawHorizontalLine ((int) (inner.getBottom() - inner.getHeight() * t), inner.getX(), inner.getRight());
        }
        else if (id == "shaper") // the feedback saturation: drive from the level, knee and symmetry
        {
            grid (4, 4);
            const auto drive = 1.0f + 5.0f * param01 (item.param != nullptr ? item.param : "Distortion");
            const auto knee = 0.5f + 3.0f * param01 ("Hardness");
            const auto bias = (param01 ("Symmetry") - 0.5f) * 0.6f;
            strokeCurve ([drive, knee, bias] (float x) {
                const auto u = (x * 2.0f - 1.0f) * drive + bias;
                const auto y = u / std::pow (1.0f + std::pow (std::abs (u), knee * 2.0f), 1.0f / (knee * 2.0f));
                return 0.5f + 0.5f * y;
            }, inner, false);
        }
        else if (id == "multiband")
        {
            // the three bands between the crossovers
            grid (8, 4);
            const auto low = param01 ("Low crossover"), high = param01 ("High crossover");
            const juce::Colour bandColours[] = { peqview::bandColour (0), peqview::bandColour (2), peqview::bandColour (5) };
            const float edges[] = { 0.0f, juce::jlimit (0.05f, 0.95f, low), juce::jlimit (0.05f, 0.95f, juce::jmax (low + 0.02f, high)), 1.0f };
            for (int b = 0; b < 3; ++b)
            {
                const auto band = juce::Rectangle<float> (inner.getX() + inner.getWidth() * edges[b], inner.getY() + inner.getHeight() * 0.35f,
                                                          inner.getWidth() * (edges[b + 1] - edges[b]), inner.getHeight() * 0.65f);
                g.setColour (bandColours[b].withAlpha (fx.enabled ? 0.18f : 0.06f));
                g.fillRect (band);
                g.setColour (bandColours[b].withAlpha (fx.enabled ? 0.9f : 0.3f));
                g.drawHorizontalLine ((int) band.getY(), band.getX(), band.getRight());
            }
        }
        else if (id == "envelope") // decay shape of a reverb / delay tail
        {
            grid (8, 4);
            const auto decay = 0.15f + param01 (item.param != nullptr ? item.param : "Decay") * 3.0f;
            strokeCurve ([decay] (float x) { return std::exp (-x * 4.0f / decay) * 0.9f; }, inner, true);
        }
        else if (id == "meter") // level meter well (FL's live meters; LYNX shows the levels in the mixer)
        {
            g.setColour (colour::gridBeat);
            for (int i = 1; i < 12; ++i)
                g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * (float) i / 12.0f), r.getX() + 2.0f, r.getRight() - 2.0f);
        }
        else if (id == "clip") // hard / soft clipping at the threshold
        {
            grid (4, 4);
            const auto t = juce::jlimit (0.02f, 1.0f, param01 (item.param != nullptr ? item.param : "Threshold"));
            const auto soft = param01 ("Type") >= 0.5f;
            const auto pre = 1.0f + 30.0f * param01 ("Pre amp") * param01 ("Pre amp");
            const auto post = 0.25f + 0.75f * param01 ("Post gain");
            strokeCurve ([t, soft, pre, post] (float x) {
                const auto u = x * pre;
                return post * (soft ? t * std::tanh (u / t) : juce::jmin (u, t));
            }, inner, false);
        }
        else if (id == "softclip") // linear up to the threshold, then a soft knee into the ceiling
        {
            grid (4, 4);
            const auto t = juce::jlimit (0.05f, 0.98f, 0.3f + 0.7f * param01 ("Threshold"));
            const auto post = 0.4f + 0.6f * param01 ("Post");
            strokeCurve ([t, post] (float x) {
                const auto y = x <= t ? x : t + (1.0f - t) * std::tanh ((x - t) / (1.0f - t));
                return y * post;
            }, inner, false);
        }
        else if (id == "sgamount" || id == "sgpresets")
        {
            paintSoundgoodizer (g, id, r, fx);
            return;
        }
        else if (id == "geminipan") // Gemini Doubler: where the voices and the direct signal sit (half circle, gain = radius)
        {
            using namespace voicetools::gemini;
            const auto c = juce::Point<float> (inner.getCentreX(), inner.getBottom() - 4.0f * s);
            const auto radius = juce::jmin (inner.getWidth() * 0.5f, inner.getHeight()) - 8.0f * s;
            g.setColour (colour::gridBeat);
            for (int k = 1; k <= 3; ++k)
            {
                juce::Path arc;
                arc.addCentredArc (c.x, c.y, radius * (float) k / 3.0f, radius * (float) k / 3.0f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
                g.strokePath (arc, juce::PathStrokeType (1.0f));
            }
            for (const auto deg : { -45.0f, 45.0f })
            {
                const auto a = juce::degreesToRadians (deg);
                g.drawLine (c.x, c.y, c.x + radius * std::sin (a), c.y - radius * std::cos (a), 1.0f);
            }
            // gain −48 … 0 dB → radius 0 … 1; the dot's angle is its pan (±45° = the hard sides)
            auto dot = [&] (float pan01, float gain01, bool active, juce::Colour col, const juce::String& text) {
                const auto a = juce::degreesToRadians ((float) panDegrees (pan01)); // ±45° = the diagonals
                const auto rr = radius * juce::jlimit (0.05f, 1.0f, gain01);
                const auto pt = juce::Point<float> (c.x + rr * std::sin (a), c.y - rr * std::cos (a));
                g.setColour (active ? col : colour::textDisabled);
                g.fillEllipse (juce::Rectangle<float> (14.0f * s, 14.0f * s).withCentre (pt));
                g.setColour (colour::bg0);
                g.setFont (font (9.0f * s, true));
                g.drawText (text, juce::Rectangle<float> (14.0f * s, 14.0f * s).withCentre (pt), juce::Justification::centred, false);
            };
            dot (param01 ("Direct pan"), param01 ("Direct gain"), param01 ("Direct") >= 0.5f, colour::textPrimary, "D");
            dot (param01 ("V1 pan"), param01 ("V1 gain"), param01 ("V1 on") >= 0.5f, curveColour, "1");
            dot (param01 ("V2 pan"), param01 ("V2 gain"), param01 ("V2 on") >= 0.5f, curveColour.withRotatedHue (0.12f), "2");
            // round 135w: voices 3 and 4 (shown when on)
            if (param01 ("V3 on") >= 0.5f)
                dot (param01 ("V3 pan"), param01 ("V3 gain"), true, curveColour.withRotatedHue (0.24f), "3");
            if (param01 ("V4 on") >= 0.5f)
                dot (param01 ("V4 pan"), param01 ("V4 gain"), true, curveColour.withRotatedHue (0.36f), "4");
        }
        else if (id == "geminimap") // Gemini Doubler: each voice's delay (across, 0 … 100 ms) against its detune (up, ±100 c)
        {
            grid (5, 4);
            g.setColour (colour::gridBar);
            g.drawHorizontalLine ((int) inner.getCentreY(), inner.getX(), inner.getRight());
            g.setColour (colour::textSecondary);
            g.setFont (font (8.5f * s));
            for (int k = 0; k <= 5; ++k)
                g.drawText (juce::String (k * 20), juce::Rectangle<float> (inner.getX() + inner.getWidth() * (float) k / 5.0f - 12.0f * s, inner.getBottom() - 11.0f * s, 24.0f * s, 10.0f * s),
                            juce::Justification::centred, false);
            // round 137: the detune scale up the left side (+100 / 0 / −100 cents)
            static const std::pair<float, const char*> centsMarks[] { { 0.0f, "+100" }, { 0.5f, "0" }, { 1.0f, "-100" } };
            for (const auto& [frac, text] : centsMarks)
                g.drawText (text, juce::Rectangle<float> (inner.getX() + 1.0f * s, inner.getY() + inner.getHeight() * frac - (frac > 0.9f ? 22.0f : frac > 0.1f ? 5.0f : 0.0f) * s, 30.0f * s, 10.0f * s),
                            juce::Justification::centredLeft, false);
            for (int k = 0; k < 4; ++k) // round 135w: four voices (3 and 4 only when on)
            {
                const juce::String pre = "V" + juce::String (k + 1) + " ";
                if (k >= 2 && param01 ((pre + "on").toRawUTF8()) < 0.5f)
                    continue;
                const auto x = inner.getX() + inner.getWidth() * param01 ((pre + "delay").toRawUTF8());
                const auto y = inner.getBottom() - inner.getHeight() * param01 ((pre + "detune").toRawUTF8());
                const auto col = curveColour.withRotatedHue (0.12f * (float) k);
                g.setColour ((param01 ((pre + "on").toRawUTF8()) >= 0.5f ? col : colour::textDisabled));
                // the LFO's reach on the pitch as a bar through the dot
                const auto depth = std::abs (param01 ((pre + "depth").toRawUTF8()) - 0.5f) * 2.0f * inner.getHeight();
                if (depth > 1.0f)
                    g.fillRect (juce::Rectangle<float> (3.0f * s, depth).withCentre ({ x, y }));
                g.fillEllipse (juce::Rectangle<float> (12.0f * s, 12.0f * s).withCentre ({ x, y }));
            }
        }
        else if (id == "geminieq") // Gemini Doubler: the voices' low / high shelves
        {
            using namespace voicetools::gemini;
            grid (4, 4);
            const bool lo = param01 ("Low shelf") >= 0.5f, hi = param01 ("High shelf") >= 0.5f;
            // round 165w: the law's range over the height (measured: −88 … +12 dB; round 124's: −30 … +12) and its shapes
            const bool measured = param01 ("EQ law") >= 0.5f;
            const auto floorDb = measured ? -88.0 : -30.0, spanDb = measured ? 100.0 : 42.0;
            const auto lg = lo ? shelfDb (param01 ("Low gain"), measured) : 0.0, hg = hi ? shelfDb (param01 ("High gain"), measured) : 0.0;
            const auto lf = freqHz (param01 ("Low freq")), hf = freqHz (param01 ("High freq"));
            strokeCurve ([=] (float x) {
                const auto f = 20.0 * std::pow (1000.0, (double) x);
                const auto db = shelfResponseDb (f, lf, lg, false, measured) + shelfResponseDb (f, hf, hg, true, measured);
                return (float) std::clamp ((db - floorDb) / spanDb, 0.0, 1.0);
            }, inner, true);
            // round 137: the L / H markers (drag: frequency / gain; double-click: on / off) and the frequency labels
            g.setFont (font (8.5f * s));
            g.setColour (colour::textSecondary);
            static const std::pair<double, const char*> freqMarks[] { { 62.0, "62" }, { 250.0, "250" }, { 1000.0, "1K" }, { 4000.0, "4K" } };
            for (const auto& [hz, text] : freqMarks)
            {
                const auto x = inner.getX() + inner.getWidth() * (float) (std::log (hz / 20.0) / std::log (1000.0));
                g.drawText (text, juce::Rectangle<float> (x - 12.0f * s, inner.getBottom() - 11.0f * s, 24.0f * s, 10.0f * s), juce::Justification::centred, false);
            }
            const auto zero = inner.getBottom() - inner.getHeight() * (float) (-floorDb / spanDb);
            g.drawText ("0", juce::Rectangle<float> (inner.getX() + 1.0f * s, zero - 5.0f * s, 14.0f * s, 10.0f * s), juce::Justification::centredLeft, false);
            for (const auto& h : geminiHandles (item))
            {
                const bool isLow = h.on == lowOn, onNow = valueOf (h.on) >= 0.5f;
                const auto box = juce::Rectangle<float> (14.0f * s, 14.0f * s).withCentre (h.at);
                g.setColour (onNow ? curveColour : colour::textDisabled);
                g.fillEllipse (box);
                g.setColour (colour::bg0);
                g.setFont (font (9.0f * s, true));
                g.drawText (isLow ? "L" : "H", box, juce::Justification::centred, false);
            }
        }
        else if (id == "xy") // a pad: `param` across, `param2` up (drag the point)
        {
            grid (4, 4);
            const auto x = inner.getX() + inner.getWidth() * param01 (item.param);
            const auto y = inner.getBottom() - inner.getHeight() * param01 (item.param2);
            g.setColour (curveColour);
            g.drawEllipse (juce::Rectangle<float> (10.0f * s, 10.0f * s).withCentre ({ x, y }), juce::jmax (1.5f, 2.0f * s));
        }
        else if (id == "voices") // the chorus voices spread across the stereo field (a static picture)
        {
            g.setColour (colour::gridBeat);
            g.drawVerticalLine ((int) inner.getCentreX(), inner.getY(), inner.getBottom());
            const auto spread = 0.2f + 0.8f * param01 ("Movement");
            for (int i = 0; i < 8; ++i)
            {
                const auto t = (float) i / 7.0f;
                const auto x = inner.getCentreX() + (t - 0.5f) * inner.getWidth() * spread;
                g.setColour (curveColour.withAlpha (0.35f + 0.5f * (1.0f - std::abs (t - 0.5f))));
                g.fillRoundedRectangle (juce::Rectangle<float> (3.0f * s, inner.getHeight() * (0.3f + 0.4f * std::abs (std::sin ((float) i * 1.7f)))).withCentre ({ x, inner.getCentreY() }), 1.5f);
            }
        }
        else if (id == "panpad") // pan (x) and volume (y) as a crosshair
        {
            grid (6, 6);
            const auto x = inner.getX() + inner.getWidth() * param01 ("Pan");
            const auto y = inner.getBottom() - inner.getHeight() * param01 ("Volume");
            g.setColour (curveColour);
            g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
            g.drawHorizontalLine ((int) y, inner.getX(), inner.getRight());
            g.fillEllipse (juce::Rectangle<float> (7.0f * s, 7.0f * s).withCentre ({ x, y }));
        }
        else if (id == "waveshaper") // round 165: FL's point editor (the map: input across, level up)
        {
            grid (8, 8);
            g.setColour (colour::gridBar);
            g.drawLine ({ inner.getBottomLeft(), inner.getTopRight() }, 1.0f); // the identity, for reference
            const auto ws = wsState();
            const auto& table = wsTable (ws.points, wsUnipolar());
            strokeCurve ([&table, uni = wsUnipolar()] (float x) { return flshape::waveShaperLevelAt (table, uni, x); }, inner, false);
            const auto times = flshape::waveShaperTimes (ws.points);
            // tension handles (hollow, between the points), then the points
            for (std::size_t i = 1; i < ws.points.size(); ++i)
                if (const auto h = wsHandlePos (inner, ws, times, (int) i); h.x > -1.0f)
                {
                    const auto hot = (int) i == wsHoverHandle || (int) i == wsTension;
                    g.setColour (hot ? colour::textPrimary : curveColour.withAlpha (0.85f));
                    g.drawEllipse (juce::Rectangle<float> (8.0f * s, 8.0f * s).withCentre (h), juce::jmax (1.0f, 1.3f * s));
                }
            for (std::size_t i = 0; i < ws.points.size(); ++i)
            {
                const auto at = wsPos (inner, times[i], ws.points[i].y);
                const auto hot = (int) i == wsHoverPoint || (int) i == wsPoint;
                g.setColour (colour::bg0);
                g.fillEllipse (juce::Rectangle<float> (11.0f * s, 11.0f * s).withCentre (at));
                g.setColour (hot ? colour::textPrimary : curveColour);
                g.fillEllipse (juce::Rectangle<float> (7.0f * s, 7.0f * s).withCentre (at));
            }
        }
        else if (id == "room") // nested walls: the room size and the decay
        {
            const auto size = 0.25f + 0.7f * param01 (item.param != nullptr ? item.param : "Room size");
            const auto c = inner.getCentre();
            for (int i = 0; i < 6; ++i)
            {
                const auto k = size * (1.0f - 0.14f * (float) i);
                const auto box = juce::Rectangle<float> (inner.getWidth() * k, inner.getHeight() * k * 0.8f).withCentre (c);
                g.setColour (curveColour.withAlpha (0.75f - 0.11f * (float) i));
                g.drawRoundedRectangle (box, 4.0f * s, juce::jmax (1.0f, 1.5f * s));
            }
        }
        else if (id == "maxcurve") // Maximus: the selected band's drawn compression curve (input → output, 0.5 = 0 dBFS)
        {
            grid (4, 4);
            const auto band = juce::jlimit (0, 3, page);
            const auto c = fx.enabled ? peqview::bandColour (band) : colour::textDisabled;
            g.setColour (colour::gridBar);
            g.drawLine ({ inner.getBottomLeft(), inner.getTopRight() }, 1.0f);
            g.drawVerticalLine ((int) inner.getCentreX(), inner.getY(), inner.getBottom());
            g.drawHorizontalLine ((int) inner.getCentreY(), inner.getX(), inner.getRight());
            const auto pts = maximusfx::curvePoints (fx.state, band);
            if (pts.size() >= 2)
            {
                juce::Path p;
                auto at = [&] (const std::pair<float, float>& q) { return juce::Point<float> (inner.getX() + inner.getWidth() * q.first, inner.getBottom() - inner.getHeight() * q.second); };
                p.startNewSubPath (at (pts.front()));
                for (std::size_t i = 1; i < pts.size(); ++i)
                    p.lineTo (at (pts[i]));
                auto fill = p;
                fill.lineTo (at (pts.back()).withY (inner.getBottom()));
                fill.lineTo (at (pts.front()).withY (inner.getBottom()));
                fill.closeSubPath();
                g.setColour (c.withAlpha (0.14f));
                g.fillPath (fill);
                g.setColour (c);
                g.strokePath (p, juce::PathStrokeType (juce::jmax (1.5f, 2.0f * s)));
                for (const auto& q : pts)
                    g.fillEllipse (juce::Rectangle<float> (5.0f * s, 5.0f * s).withCentre (at (q)));
            }
            g.setColour (colour::textSecondary);
            g.setFont (font (juce::jmax (8.0f, 9.0f * s)));
            g.drawText (tr ("Out"), inner.withHeight (12.0f * s).toNearestInt(), juce::Justification::topLeft, false);
            g.drawText (tr ("In"), inner.withTop (inner.getBottom() - 12.0f * s).toNearestInt(), juce::Justification::bottomRight, false);
        }
        else if (id == "maxbands") // Maximus: the three bands over 20 Hz…20 kHz with their crossovers and the low cut
        {
            const auto raw = [&] (int p) { return maximusfx::stateParam (fx.state, p).value_or (maximusfx::stateParams()[(std::size_t) p].defaultValue); };
            const auto xOf = [&] (double hz) { return inner.getX() + inner.getWidth() * (float) (std::log (juce::jlimit (20.0, 20000.0, hz) / 20.0) / std::log (1000.0)); };
            auto lo = 30.0 * std::pow (533.33, raw (0) / 65536.0), hi = 30.0 * std::pow (533.33, raw (2) / 65536.0);
            if (lo > hi)
                std::swap (lo, hi);
            const float edges[4] { inner.getX(), xOf (lo), xOf (hi), inner.getRight() };
            static const char* names[3] { "Low", "Mid", "High" };
            for (int b = 0; b < 3; ++b)
            {
                const auto mode = raw (7 + 13 * b);
                const auto area = juce::Rectangle<float> (edges[b], inner.getY(), edges[b + 1] - edges[b], inner.getHeight());
                const auto c = peqview::bandColour (b);
                g.setColour (c.withAlpha (! fx.enabled || mode >= 2 ? 0.05f : (b == page ? 0.24f : 0.12f)));
                g.fillRect (area);
                g.setColour (b == page ? colour::textPrimary : colour::textSecondary);
                g.setFont (font (juce::jmax (8.0f, 10.0f * s), true));
                g.drawFittedText (caps (tr (names[b])) + (mode == 2 ? " · " + tr ("Muted") : mode == 3 ? " · " + tr ("Off") : juce::String()),
                                  area.reduced (2.0f * s).withHeight (14.0f * s).toNearestInt(), juce::Justification::centred, 1, 0.6f);
            }
            g.setColour (colour::textSecondary);
            for (int k = 1; k <= 2; ++k)
                g.drawVerticalLine ((int) edges[k], inner.getY(), inner.getBottom());
            if (raw (6) >= 0) // low cut
            {
                const auto x = xOf (5.0 * std::pow (16.0, raw (6) / 65536.0));
                g.setColour (accent.withAlpha (0.6f));
                g.drawVerticalLine ((int) x, inner.getY(), inner.getBottom());
            }
            g.setColour (colour::textDisabled);
            g.setFont (font (juce::jmax (8.0f, 9.0f * s)));
            for (const auto hz : { 100.0, 1000.0, 10000.0 })
                g.drawText (hz >= 1000.0 ? juce::String ((int) (hz / 1000.0)) + "k" : juce::String ((int) hz),
                            juce::Rectangle<float> (30.0f * s, 12.0f * s).withCentre ({ xOf (hz), inner.getBottom() - 6.0f * s }).toNearestInt(),
                            juce::Justification::centred, false);
        }
        else if (id == "eq")
        {
            paintEq (g, r, fx);
            return;
        }
        else if (id == "grossbeat" || id == "gbinfo" || id == "gbjog")
        {
            paintGrossBeat (g, id, r, fx);
            return;
        }
        else if (id == "eqinfo") // the selected band, or how to use the graph
        {
            g.setColour (colour::bg1);
            g.fillRect (r);
            g.setFont (font (juce::jmax (9.0f, 11.0f * s)));
            if (eqSelected >= 0)
            {
                g.setColour (peqview::bandColour (eqSelected));
                g.drawFittedText (peqview::bandText (fx.params, eqSelected), r.reduced (4.0f * s, 0.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.8f);
            }
            else
            {
                g.setColour (colour::textSecondary);
                g.drawFittedText (tr ("Drag a handle (frequency and gain); mouse wheel = Q; right-click = type; double-click = 0 dB"),
                                  r.reduced (4.0f * s, 0.0f).toNearestInt(), juce::Justification::centredLeft, 2, 0.8f);
            }
            return;
        }
        else if (id == "filter")
        {
            grid (8, 4);
            const auto cutoff = param01 ("Cutoff");
            const auto reso = param01 ("Resonance");
            strokeCurve ([cutoff, reso] (float x) {
                const auto d = (x - cutoff) * 8.0f;
                const auto bump = reso * 0.35f * std::exp (-d * d);
                return 0.6f + bump - (x > cutoff ? (x - cutoff) * 2.2f : 0.0f);
            }, inner, true);
        }
        else if (id == "pitchgraph") // LYNX Tune: detected pitch, target, correction and the scrolling pitch graph
        {
            paintPitchDisplay (g, r, fx);
            return;
        }
        else if (id == "pitchtrace") // LYNX Tune (round 120): the scrolling pitch graph alone
        {
            paintPitchDisplay (g, r, fx, false);
            return;
        }
        else if (id == "pitchring") // LYNX Tune (round 120): the correction meter
        {
            paintPitchRing (g, r, fx);
            return;
        }
        else if (id == "tunekeys") // LYNX Tune (round 128): the keyboard with BYPASS / REMOVE and LATCH
        {
            paintTuneKeys (g, r, fx);
            return;
        }
        else if (id == "tuneautokey") // LYNX Tune (round 128): AUTO KEY
        {
            paintAutoKey (g, r);
            return;
        }
        else if (id == "hwping") // the hardware insert (round 164d): the round trip, the ping's state, what is missing
        {
            paintHwPing (g, r, fx);
            return;
        }
        else if (id == "grleds") // round 165: a ladder of reduction LEDs (Ridge Strip: the console's 3 / 6 / 10 / 14 / 20 dB)
        {
            const auto db = juce::jlimit (0.0f, 1.0f, liveMeter (item.steps)) * 30.0f;
            auto area = r.reduced (1.0f * s);
            const auto caption = area.removeFromBottom (14.0f * s);
            static const int steps[] { 20, 14, 10, 6, 3 };
            const auto h = area.getHeight() / 5.0f;
            g.setFont (font (juce::jmax (7.0f, 8.5f * s)));
            for (int k = 0; k < 5; ++k)
            {
                const auto cell = juce::Rectangle<float> (area.getX(), area.getY() + h * (float) k, area.getWidth(), h);
                const auto led = cell.withWidth (cell.getHeight() * 0.62f).withSizeKeepingCentre (cell.getHeight() * 0.5f, cell.getHeight() * 0.5f)
                                     .withX (cell.getX() + 2.0f * s);
                const bool lit = fx.enabled && db >= (float) steps[k] - 0.05f;
                g.setColour (lit ? accent : colour::gridBeat);
                g.fillEllipse (led);
                if (lit)
                {
                    g.setColour (accent.withAlpha (0.35f));
                    g.drawEllipse (led.expanded (1.5f * s), 1.0f * s);
                }
                g.setColour (colour::textSecondary);
                g.drawText (juce::String (steps[k]), cell.withTrimmedLeft (led.getRight() - cell.getX() + 2.0f * s), juce::Justification::centredLeft, false);
            }
            if (item.label != nullptr && *item.label != 0)
            {
                g.setColour (colour::textSecondary);
                g.drawText (caps (tr (item.label)), caption, juce::Justification::centred, false);
            }
            return;
        }
        else if (id == "grmeter") // live gain reduction of the mixing suite (0..30 dB, from the right like a VU's GR)
        {
            const auto amount = juce::jlimit (0.0f, 1.0f, liveMeter (item.steps));
            const auto bar = r.reduced (1.5f * s);
            if (bar.getHeight() > 2.0f * bar.getWidth()) // a tall meter (Quell): the reduction grows downwards, 0 dB at the top
            {
                const auto well = bar.reduced (0.0f, 4.0f * s).withTrimmedBottom (14.0f * s);
                g.setColour (colour::gridBeat);
                g.setFont (font (8.5f * s));
                for (int db = 0; db <= 30; db += 6)
                {
                    const auto y = well.getY() + well.getHeight() * (float) db / 30.0f;
                    g.setColour (colour::gridBeat);
                    g.drawHorizontalLine ((int) y, well.getX(), well.getRight());
                    g.setColour (colour::textSecondary);
                    g.drawText (juce::String (-db), juce::Rectangle<float> (well.getX(), y - 10.0f * s, 22.0f * s, 10.0f * s), juce::Justification::centredLeft, false);
                }
                g.setColour ((fx.enabled ? accent : colour::textDisabled).withAlpha (0.85f));
                g.fillRoundedRectangle (well.withTrimmedLeft (well.getWidth() * 0.35f).withHeight (well.getHeight() * amount), radius::sm);
                // round 137: the read-out holds the deepest reduction (the reference's peak box; click the meter = reset)
                grHold = juce::jmax (grHold, amount);
                g.setColour (colour::textSecondary);
                g.drawText ((grHold > 0.0005f ? juce::String (-grHold * 30.0f, 1) : juce::String ("0.0")) + " dB", bar.withTop (bar.getBottom() - 14.0f * s), juce::Justification::centred, false);
                return;
            }
            g.setColour (colour::gridBeat);
            for (int db = 3; db < 30; db += 3)
                g.drawVerticalLine ((int) (bar.getRight() - bar.getWidth() * (float) db / 30.0f), bar.getY(), bar.getBottom());
            g.setColour ((fx.enabled ? accent : colour::textDisabled).withAlpha (0.85f));
            g.fillRoundedRectangle (bar.withLeft (bar.getRight() - bar.getWidth() * amount), radius::sm);
            if (item.label != nullptr && *item.label != 0)
            {
                g.setColour (colour::textSecondary);
                g.setFont (font (juce::jmax (7.0f, juce::jmin (10.0f * s, bar.getHeight() * 0.95f)), true));
                g.drawText (caps (tr (item.label)) + "  " + juce::String (amount * 30.0f, 1) + " dB", bar.reduced (3.0f * s, 0.0f),
                            juce::Justification::centredLeft, false);
            }
            return;
        }
        else if (id == "faderscale") // round 137: a 0 … 100 scale beside a tall fader (the reference prints it on both sides)
        {
            auto area = r.withTrimmedBottom (13.0f * s);
            const auto top = area.getY() + 5.0f * s, bottom = area.getBottom() - 5.0f * s;
            g.setFont (font (juce::jmax (7.0f, 8.5f * s)));
            for (int k = 0; k <= 100; k += 10)
            {
                const auto y = bottom - (bottom - top) * (float) k / 100.0f;
                g.setColour (colour::gridBeat);
                g.drawHorizontalLine ((int) y, area.getRight() - 6.0f * s, area.getRight());
                g.setColour (colour::textSecondary);
                g.drawText (juce::String (k), juce::Rectangle<float> (area.getX(), y - 5.0f * s, area.getWidth() - 8.0f * s, 10.0f * s), juce::Justification::centredRight, false);
            }
            return;
        }
        else if (id == "panpos") // round 135: where the panner has the sound now (the rhythmic panners' L … C … R strip)
        {
            const auto at = juce::jlimit (0.0f, 1.0f, liveMeter (1));
            constexpr int segments = 29;
            const auto area = r.reduced (2.0f * s);
            g.setColour (colour::bg0);
            g.fillRoundedRectangle (area, 3.0f * s);
            const auto w = area.getWidth() / (float) segments;
            for (int k = 0; k < segments; ++k)
            {
                const auto centre = ((float) k + 0.5f) / (float) segments;
                const auto d = std::abs (centre - at) * (float) segments;
                const auto glow = juce::jlimit (0.0f, 1.0f, 1.0f - d / 2.5f);
                const auto cell = juce::Rectangle<float> (area.getX() + w * (float) k, area.getY(), w, area.getHeight()).reduced (1.0f * s, 3.0f * s);
                g.setColour (accent.withAlpha (0.10f + 0.90f * glow * glow));
                g.fillRoundedRectangle (cell, 1.5f * s);
            }
            g.setColour (colour::textSecondary.withAlpha (0.5f));
            g.drawVerticalLine ((int) area.getCentreX(), area.getY(), area.getBottom());
            return;
        }
        else if (id == "iometer") // round 129: input and output peak meters with peak holds and read-outs (−48 … 0 dBFS)
        {
            // the originals' level meters (SSL E-Channel's IN / OUT meter, RComp's input and output, Doubler's output)
            const auto fracOf = [] (float linear) { return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (juce::jmax (1e-6f, linear)) + 48.0f) / 48.0f); };
            const auto base = item.steps > 0 ? item.steps : 2; // round 135w: the engine's meter outputs (LYNX Tune: 3 / 4)
            ioMeters[0].update (fracOf (liveMeter (base)), 0.02f);
            ioMeters[1].update (fracOf (liveMeter (base + 1)), 0.02f);
            ioClip = ioClip || liveMeter (base + 1) >= 0.999f; // round 135w: the clip lamp (Doubler's), click to reset
            const auto labelH = 13.0f * s, readH = 13.0f * s;
            auto area = r.reduced (2.0f * s);
            const auto top = area.removeFromTop (labelH), bottom = area.removeFromBottom (readH);
            const auto scaleW = 18.0f * s;
            auto bars = area.withTrimmedLeft (scaleW);
            g.setFont (font (juce::jmax (7.0f, 8.0f * s)));
            for (int db = 0; db >= -48; db -= 12)
            {
                const auto y = bars.getY() + bars.getHeight() * (float) -db / 48.0f;
                g.setColour (colour::gridBeat);
                g.drawHorizontalLine ((int) y, bars.getX(), bars.getRight());
                g.setColour (colour::textSecondary);
                g.drawText (juce::String (db), juce::Rectangle<float> (area.getX(), y - 6.0f * s, scaleW - 2.0f * s, 12.0f * s), juce::Justification::centredRight, false);
            }
            const auto w = bars.getWidth() / 2.0f;
            const char* names[] = { TR_NOOP ("In"), TR_NOOP ("Out") };
            if (kind == EffectKind::hardwareInsert) // round 164d: what goes to the gear and what comes back
                names[0] = TR_NOOP ("Send"), names[1] = TR_NOOP ("Return");
            for (int k = 0; k < 2; ++k)
            {
                const auto bar = juce::Rectangle<float> (bars.getX() + w * (float) k, bars.getY(), w, bars.getHeight()).reduced (3.0f * s, 0.0f);
                skin::meter (g, bar, ioMeters[(std::size_t) k].level, ioMeters[(std::size_t) k].peak, true);
                const auto column = juce::Rectangle<float> (bar.getX() - 4.0f * s, 0.0f, bar.getWidth() + 8.0f * s, 0.0f);
                g.setColour (colour::textSecondary);
                g.setFont (font (juce::jmax (7.0f, 8.5f * s), true));
                if (k == 1 && ioClip) // round 135w: the output clipped since the last click
                {
                    g.setColour (juce::Colour (0xffe5383b));
                    g.fillRoundedRectangle (column.withY (top.getY()).withHeight (labelH).reduced (4.0f * s, 1.0f * s), radius::sm);
                    g.setColour (colour::textPrimary);
                }
                g.drawText (caps (tr (names[k])), column.withY (top.getY()).withHeight (labelH), juce::Justification::centred, false);
                const auto peakDb = ioMeters[(std::size_t) k].peak * 48.0f - 48.0f;
                g.setColour (peakDb > -0.1f ? juce::Colour (0xffe5383b) : colour::textPrimary);
                g.setFont (font (juce::jmax (7.0f, 8.5f * s)));
                g.drawText (ioMeters[(std::size_t) k].peak > 0.001f ? juce::String (peakDb, 1) : juce::String ("-inf"),
                            column.withY (bottom.getY()).withHeight (readH), juce::Justification::centred, false);
            }
            return;
        }
        else if (id == "gcmeter") // round 152: RComp's bipolar gain-change meter (+6 … 0 … −12, deeper reductions compressed)
        {
            // the engine's control output 5 = the block's signed gain change in dB (negative = reduction, positive = the
            // upward expansion below 1 : 1); the bar grows from the 0 dB line, the read-out holds the largest change
            const auto change = liveMeter (5);
            gcLevel = std::abs (change) > std::abs (gcLevel) ? change : gcLevel + 0.25f * (change - gcLevel);
            if (std::abs (change) > std::abs (gcHold))
                gcHold = change;
            static constexpr float dbs[] { 9.0f, 6.0f, 3.0f, 0.0f, -3.0f, -6.0f, -12.0f, -24.0f, -30.0f };
            static constexpr float fracs[] { 0.0f, 0.08f, 0.20f, 0.38f, 0.53f, 0.66f, 0.80f, 0.94f, 1.0f };
            const auto fracOf = [] (float db)
            {
                db = juce::jlimit (-30.0f, 9.0f, db);
                for (int k = 0; k + 1 < 9; ++k)
                    if (db >= dbs[k + 1])
                        return fracs[k] + (fracs[k + 1] - fracs[k]) * (dbs[k] - db) / (dbs[k] - dbs[k + 1]);
                return 1.0f;
            };
            const auto readH = 14.0f * s;
            auto area = r.reduced (2.0f * s);
            const auto bottom = area.removeFromBottom (readH);
            g.setColour (colour::bg3);
            g.fillRoundedRectangle (area, radius::sm * s);
            const auto scaleW = 20.0f * s;
            const auto well = area.withTrimmedLeft (scaleW).reduced (3.0f * s, 4.0f * s);
            g.setFont (font (juce::jmax (7.0f, 8.0f * s)));
            for (const auto db : { 6, 3, 0, -3, -6, -12, -24 })
            {
                const auto y = well.getY() + well.getHeight() * fracOf ((float) db);
                g.setColour (db == 0 ? colour::textSecondary.withAlpha (0.6f) : colour::gridBeat);
                g.drawHorizontalLine ((int) y, well.getX(), well.getRight());
                g.setColour (colour::textSecondary);
                g.drawText (juce::String (db), juce::Rectangle<float> (area.getX(), y - 5.0f * s, scaleW - 2.0f * s, 10.0f * s), juce::Justification::centredRight, false);
            }
            const auto y0 = well.getY() + well.getHeight() * fracOf (0.0f), y1 = well.getY() + well.getHeight() * fracOf (gcLevel);
            g.setColour ((fx.enabled ? accent : colour::textDisabled).withAlpha (gcLevel > 0.0f ? 0.55f : 0.85f));
            g.fillRoundedRectangle (juce::Rectangle<float> (well.getX() + well.getWidth() * 0.2f, std::min (y0, y1), well.getWidth() * 0.6f, std::abs (y1 - y0)), radius::sm);
            g.setColour (colour::textPrimary);
            g.setFont (font (juce::jmax (7.0f, 9.0f * s)));
            g.drawText (std::abs (gcHold) > 0.05f ? juce::String (gcHold, 1) : juce::String ("0.0"), bottom, juce::Justification::centred, false);
            return;
        }
        else if (id == "limitpill") // round 135w: the output limiter's activity pill (RComp's), lit while it limits
        {
            const auto active = liveMeter (4) > 0.001f;
            g.setColour (active ? juce::Colour (0xffe5383b) : colour::bg3);
            g.fillRoundedRectangle (r.reduced (1.0f * s), r.getHeight() * 0.5f);
            return;
        }
        else if (id == "outmeter") // round 135w: an output peak meter with a clip lamp and the highest peak (click: reset)
        {
            // H-Delay's output meter (0 … −48 dB) and peak lamp, DeEsser's output meter (−30 … 0, `steps` = 30)
            const auto range = (float) (item.steps > 0 ? item.steps : 48);
            const auto peakNow = liveMeter (3);
            const auto fracOf = [range] (float linear) { return juce::jlimit (0.0f, 1.0f, (20.0f * std::log10 (juce::jmax (1e-6f, linear)) + range) / range); };
            outMeter.update (fracOf (peakNow), 0.02f);
            outMax = juce::jmax (outMax, peakNow);
            outClip = outClip || peakNow >= 0.999f;
            const auto lampH = 10.0f * s, readH = 13.0f * s;
            auto area = r.reduced (2.0f * s);
            const auto lamp = area.removeFromTop (lampH).withSizeKeepingCentre (juce::jmin (area.getWidth(), 16.0f * s), lampH - 2.0f * s);
            area.removeFromTop (2.0f * s);
            const auto bottom = area.removeFromBottom (readH);
            g.setColour (outClip ? juce::Colour (0xffe5383b) : colour::bg3);
            g.fillRoundedRectangle (lamp, radius::sm);
            const auto scaleW = juce::jmin (16.0f * s, area.getWidth() * 0.45f);
            auto bar = area.withTrimmedLeft (scaleW).reduced (2.0f * s, 0.0f);
            g.setFont (font (juce::jmax (6.5f, 7.5f * s)));
            const int stepDb = range > 36.0f ? 12 : 6;
            for (int db = 0; db >= -(int) range; db -= stepDb)
            {
                const auto y = bar.getY() + bar.getHeight() * (float) -db / range;
                g.setColour (colour::gridBeat);
                g.drawHorizontalLine ((int) y, bar.getX(), bar.getRight());
                g.setColour (colour::textSecondary);
                g.drawText (juce::String (db), juce::Rectangle<float> (area.getX(), y - 5.0f * s, scaleW - 1.0f * s, 10.0f * s), juce::Justification::centredRight, false);
            }
            skin::meter (g, bar, outMeter.level, outMeter.peak, true);
            const auto maxDb = 20.0f * std::log10 (juce::jmax (1e-6f, outMax));
            g.setColour (maxDb > -0.1f ? juce::Colour (0xffe5383b) : colour::textPrimary);
            g.setFont (font (juce::jmax (7.0f, 8.5f * s)));
            g.drawText (outMax > 1e-5f ? juce::String (maxDb, 1) : juce::String ("-inf"), bottom, juce::Justification::centred, false);
            return;
        }
        else if (id == "taptempo") // round 129: a tap pad (H-Delay's TAP): tap twice or more to set the delay time
        {
            const auto lit = juce::Time::getMillisecondCounter() - lastTap < 150;
            skin::button (g, r, lit, false, accent, radius::sm * s);
            g.setColour (lit ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (juce::jmax (7.0f, 10.0f * s), true));
            g.drawText (tapBpm > 0.0 ? caps (tr ("Tap")) + "  " + juce::String (tapBpm, 1) : caps (tr ("Tap")), r, juce::Justification::centred, false);
            return;
        }
        else if (id == "vusel") // round 129: the VU display selector, INPUT | GR | OUTPUT (the reference's VU DISPLAY knob)
        {
            static constexpr int order[] = { 1, 0, 2 };
            const char* names[] = { TR_NOOP ("Input"), TR_NOOP ("GR"), TR_NOOP ("Output") };
            const auto w = r.getWidth() / 3.0f;
            for (int k = 0; k < 3; ++k)
            {
                const auto cell = juce::Rectangle<float> (r.getX() + w * (float) k, r.getY(), w, r.getHeight()).reduced (1.5f * s, 0.0f);
                skin::button (g, cell, vuMode == order[k], false, accent, radius::sm * s);
                g.setColour (vuMode == order[k] ? colour::textPrimary : colour::textSecondary);
                g.setFont (font (juce::jmax (7.0f, 9.5f * s), true));
                g.drawText (caps (tr (names[k])), cell, juce::Justification::centred, false);
            }
            return;
        }
        else if (id == "vumeter") // round 114: an analog-style VU needle showing the gain reduction (the optical leveler)
        {
            // gain reduction (needle down from 0 VU) or the output level (0 VU = −18 dBFS), like the reference's selector
            // round 129: input / gain reduction / output, like the reference's VU DISPLAY selector (the "vusel" item)
            const auto level = liveMeter (vuMode == 1 ? 2 : 3);
            const auto target = vuMode != 0 ? -juce::jlimit (-3.0f, 30.0f, -(20.0f * std::log10 (juce::jmax (1e-6f, level)) + 18.0f))
                                            : -juce::jlimit (0.0f, 1.0f, liveMeter (item.steps)) * 30.0f;
            vuNeedle += (-target - vuNeedle) * 0.35f; // a meter's inertia (~300 ms at the 30 Hz refresh)
            const auto face = r.reduced (2.0f * s);
            g.setColour (juce::Colour (0xffd9cfb4).withAlpha (fx.enabled ? 1.0f : 0.5f));
            g.fillRoundedRectangle (face, radius::lg * s);
            g.setColour (colour::bg0);
            g.drawRoundedRectangle (face, radius::lg * s, 2.0f * s);
            const juce::Point<float> pivot { face.getCentreX(), face.getBottom() + face.getHeight() * 0.18f };
            const auto radiusV = face.getHeight() * 0.92f;
            const auto fracOf = [] (float db) { // the VU scale: −20 … +3 on a linear-voltage law
                const auto lo = std::pow (10.0f, -1.0f), hi = std::pow (10.0f, 3.0f / 20.0f);
                return juce::jlimit (0.0f, 1.0f, (std::pow (10.0f, db / 20.0f) - lo) / (hi - lo));
            };
            const auto angleOf = [&] (float db) { return juce::MathConstants<float>::pi * (-0.27f + 0.54f * fracOf (db)); };
            g.setFont (font (juce::jmax (7.0f, 9.0f * s), true));
            for (const auto db : { -20.0f, -10.0f, -7.0f, -5.0f, -3.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f, 3.0f })
            {
                const auto a = angleOf (db);
                const juce::Point<float> dir { std::sin (a), -std::cos (a) };
                g.setColour (db > 0.0f ? juce::Colour (0xffb3261e) : juce::Colour (0xff2a2622));
                g.drawLine ({ pivot + dir * (radiusV * 0.86f), pivot + dir * (radiusV * 0.95f) }, 1.4f * s);
                if (db == -20.0f || db == -10.0f || db == -5.0f || db == -3.0f || db == 0.0f || db == 3.0f)
                    g.drawText (db > 0.0f ? "+" + juce::String ((int) db) : juce::String ((int) db),
                                juce::Rectangle<float> (24.0f * s, 12.0f * s).withCentre (pivot + dir * (radiusV * 1.04f)).toNearestInt(), juce::Justification::centred, false);
            }
            g.setColour (juce::Colour (0xffb3261e).withAlpha (0.8f));
            juce::Path redArc;
            redArc.addCentredArc (pivot.x, pivot.y, radiusV * 0.9f, radiusV * 0.9f, 0.0f, angleOf (0.0f), angleOf (3.0f), true);
            g.strokePath (redArc, juce::PathStrokeType (3.0f * s));
            g.setColour (juce::Colour (0xff2a2622));
            g.setFont (font (juce::jmax (8.0f, 11.0f * s), true));
            g.drawText ("VU", face.withTop (face.getY() + face.getHeight() * 0.5f).withHeight (16.0f * s).toNearestInt(), juce::Justification::centred, false);
            g.setFont (font (juce::jmax (7.0f, 8.5f * s)));
            g.drawText ((vuMode == 2 ? tr ("Output") : vuMode == 1 ? tr ("Input") : tr ("Gain reduction")) + "  " + juce::String (-vuNeedle, 1) + (vuMode != 0 ? " VU" : " dB"), face.withTop (face.getBottom() - 16.0f * s).toNearestInt(), juce::Justification::centred, false);
            // the needle
            const auto a = angleOf (-vuNeedle);
            const juce::Point<float> dir { std::sin (a), -std::cos (a) };
            g.saveState();
            g.reduceClipRegion (face.toNearestInt());
            g.setColour (juce::Colour (0xff141210));
            g.drawLine ({ pivot + dir * (radiusV * 0.25f), pivot + dir * (radiusV * 0.97f) }, 1.6f * s);
            g.restoreState();
            return;
        }
        else if (id == "ridgeeq") // the strip's EQ + filters (20 Hz .. 20 kHz, ±20 dB)
        {
            grid (10, 4);
            const auto params = fx.params;
            strokeCurve ([&params] (float x) {
                const auto hz = 20.0 * std::pow (1000.0, (double) x);
                return 0.5f + (float) mixsuite::ridge::responseDb (params, 48000.0, hz) / 40.0f;
            }, inner, false);
        }
        else if (id == "ridgecomp") // compressor's static curve (−60 .. 0 dB in and out)
        {
            grid (6, 4);
            const auto params = fx.params;
            strokeCurve ([&params] (float x) {
                return 1.0f + (float) mixsuite::ridge::compressorCurveDb (params, (double) (x - 1.0f) * 60.0) / 60.0f;
            }, inner, true);
        }
        else if (id == "hushband") // the detector's filter (high-pass or band) at the frequency, 1 .. 20 kHz
        {
            grid (8, 4);
            const auto f0 = mixsuite::hush::hz (param01 ("Frequency"));
            const auto band = param01 ("Band detector") >= 0.5f;
            strokeCurve ([f0, band] (float x) {
                const auto u = 1000.0 * std::pow (20.0, (double) x) / f0;
                const auto u2 = u * u;
                const auto mag = band ? (u / 1.4) / std::sqrt ((1.0 - u2) * (1.0 - u2) + u2 / (1.4 * 1.4))
                                      : u2 / std::sqrt ((1.0 - u2) * (1.0 - u2) + 2.0 * u2);
                return 0.9f + (float) (20.0 * std::log10 (std::max (1e-6, mag))) / 40.0f;
            }, inner, true);
        }
        else if (id == "embercurve") // the style's transfer curve at the drive (x = ±1: the peaks of a −12 dBFS sine)
        {
            grid (4, 4);
            const auto style = mixsuite::ember::styleOf (param01 ("Style"));
            const auto drive = 0.25f * (float) std::pow (10.0, mixsuite::ember::driveDb (param01 ("Drive")) / 20.0);
            const auto norm = 1.0f / juce::jmax (0.05f, std::abs (mixsuite::ember::shape (style, drive)));
            strokeCurve ([style, drive, norm] (float x) {
                return 0.5f + 0.5f * juce::jlimit (-1.0f, 1.0f, mixsuite::ember::shape (style, (x * 2.0f - 1.0f) * drive) * norm);
            }, inner, false);
        }
        else if (id == "echotrain" || id == "lattice") // the echoes / taps on a time axis: left up, right down
        {
            grid (8, 2);
            const auto params = fx.params;
            const auto bpm = engine.getSong().bpm > 1.0 ? engine.getSong().bpm : 120.0;
            struct Tick { double ms; double level; bool right; };
            std::vector<Tick> ticks;
            double window = 50.0;
            if (id == "echotrain")
            {
                using namespace echosuite::echoform;
                const auto st = styleOf (params[style]);
                auto tL = sideMs (params, false, bpm), tR = sideMs (params, true, bpm);
                if (st == Style::single || st == Style::quad)
                    tR = tL;
                else if (st == Style::ratio)
                    tR = tL * (0.01 + 0.99 * (double) params[ratio]);
                const auto gain = juce::jlimit (0.0, 1.0, feedbackGain (params[feedback]));
                window = juce::jmax (50.0, 4.5 * juce::jmax (tL, tR));
                if (params[rhythm] >= 0.5f) // round 137: the Rhythm pattern, three passes of it (left and right alike)
                {
                    double slot0 = 0.0;
                    const auto taps = rhythmTaps (params, bpm, &slot0);
                    const auto length = rhythmLengthMs (params, bpm);
                    window = 3.0 * length * 1.02;
                    auto level = 1.0;
                    for (int pass = 0; pass < 3; ++pass, level *= gain)
                        for (int c = 0; c < 2; ++c)
                        {
                            if (pass > 0)
                                ticks.push_back ({ length * pass, slot0 * level, c == 1 });
                            for (const auto& t : taps)
                                ticks.push_back ({ length * pass + t.ms, t.level * level, c == 1 });
                        }
                }
                else if (st == Style::pingPong)
                {
                    auto t = tL, level = 1.0;
                    for (int k = 0; k < 24 && t <= window; ++k)
                    {
                        ticks.push_back ({ t, level, (k % 2) == 1 });
                        t += (k % 2) == 0 ? tR : tL;
                        level *= params[pairs] >= 0.5f ? ((k % 2) == 1 ? gain : 1.0) : std::sqrt (gain); // round 119: pairs
                    }
                }
                else
                    for (int c = 0; c < 2; ++c)
                    {
                        const auto t = c == 0 ? tL : tR;
                        auto level = 1.0;
                        for (int k = 1; k <= 24 && t > 0.0 && t * k <= window; ++k, level *= gain)
                        {
                            if (st == Style::quad)
                                for (int q = 1; q < 4; ++q)
                                    ticks.push_back ({ t * (k - 1 + q / 4.0), 0.5 * level, c == 1 });
                            ticks.push_back ({ t * k, st == Style::quad ? 0.5 * level : level, c == 1 });
                        }
                    }
            }
            else
            {
                using namespace echosuite::lattice;
                const auto n = tapCount (params[taps]);
                double longest = 1.0;
                for (int k = 0; k < n; ++k)
                    for (int c = 0; c < 2; ++c)
                    {
                        const auto t = tapMs (params, k, n, c == 1);
                        longest = juce::jmax (longest, t);
                        ticks.push_back ({ t, std::abs (tapLevel (params, k, n)) * std::sqrt ((double) n), c == 1 });
                    }
                window = longest * 1.15;
            }
            const auto mid = inner.getCentreY();
            g.setColour (colour::gridBar);
            g.drawHorizontalLine ((int) mid, inner.getX(), inner.getRight());
            g.setColour (curveColour);
            for (const auto& t : ticks)
            {
                if (t.level < 0.01)
                    continue;
                const auto x = inner.getX() + inner.getWidth() * (float) (t.ms / window);
                const auto h = inner.getHeight() * 0.48f * (float) juce::jlimit (0.0, 1.0, t.level);
                g.fillRect (juce::Rectangle<float> (x - 1.0f * s, t.right ? mid : mid - h, juce::jmax (1.5f, 2.0f * s), h));
            }
            g.setColour (colour::textDisabled);
            g.setFont (font (juce::jmax (8.0f, 9.0f * s), true));
            g.drawText (juce::String (window >= 1000.0 ? juce::String (window / 1000.0, 2) + " s" : juce::String ((int) window) + " ms"),
                        inner.reduced (2.0f * s), juce::Justification::bottomRight);            if (id == "echotrain") // round 137: the delay read-out (H-Delay's central display): the note and its tempo, or the ms
            {
                using namespace echosuite::echoform;
                const auto kindL = syncKind (params[syncL]);
                juce::String text;
                if (kindL == 0)
                    text = juce::String (juce::roundToInt (sideMs (params, false, bpm))) + " ms";
                else
                    text = juce::String (formatEffectParam (EffectKind::echoform, noteL, params[noteL], bpm)) + (kindL == 2 ? " D" : kindL == 3 ? " T" : "")
                           + "  " + juce::String (tempoFor (params, bpm), 1) + " BPM";
                g.setColour (colour::textSecondary);
                g.drawText (text, inner.reduced (2.0f * s), juce::Justification::topRight);
            }
        }
        else if (paintFxDisplay (g, id, item, inner, fx))
        {
        }
        else // a plain well with the plugin's name (FL's scopes / animations are not reproduced)
        {
            g.setColour (colour::bg2);
            g.setFont (font (juce::jmax (10.0f, juce::jmin (28.0f, r.getHeight() * 0.22f)), true));
            g.drawFittedText (caps (utf8 (effectInfo (kind).name)), r.toNearestInt(), juce::Justification::centred, 1, 0.8f);
        }

        if (item.label != nullptr && *item.label != 0)
        {
            g.setColour (colour::textDisabled);
            g.setFont (font (juce::jmax (8.0f, 9.0f * s), true));
            g.drawText (caps (tr (item.label)), inner.withHeight (12.0f * s), juce::Justification::topLeft);
        }
    }

#include "ui/PluginEditorsFx.inc"

    // --- Gross Beat: the selected slots' envelopes ------------------------------------------------------

    void paintGrossBeat (juce::Graphics& g, const juce::String& id, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        std::vector<std::uint8_t> flState;
        int ppq = 96;
        std::optional<grossbeat::State> state;
        if (grossbeat::unpack (fx.state, flState, ppq))
            state = grossbeat::parseState (flState.data(), flState.size());
        const auto timeSlot = grossbeat::rawFromParam (0, param01 ("Time slot"));
        const auto volumeSlot = grossbeat::rawFromParam (1, param01 ("Volume slot"));

        if (id == "gbjog") // a dial with the beat positions (FL's scratch indicator)
        {
            const auto c = r.getCentre();
            const auto radius = juce::jmin (r.getWidth(), r.getHeight()) * 0.4f;
            g.setColour (colour::bg3);
            g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
            for (int i = 0; i < 12; ++i)
            {
                const auto a = juce::MathConstants<float>::twoPi * (float) i / 12.0f;
                g.setColour (i == 0 ? (fx.enabled ? accent : colour::textSecondary) : colour::textDisabled);
                g.fillEllipse (juce::Rectangle<float> (4.0f * s, 4.0f * s).withCentre ({ c.x + std::sin (a) * radius * 0.85f, c.y - std::cos (a) * radius * 0.85f }));
            }
            return;
        }

        if (id == "gbinfo")
        {
            juce::String text = tr ("Time: {0}", { "Slot " + juce::String (timeSlot + 1) });
            if (state)
            {
                const auto& slot = state->slots[(std::size_t) timeSlot];
                if (! slot.name.empty())
                    text = tr ("Time: {0}", { juce::String::fromUTF8 (slot.name.c_str()) });
                static const char* const syncs[] = { "off", "1/4 beat", "1/2 beat", "1 beat", "2 beats", "4 beats" };
                const auto sync = [] (int v) { return juce::String (syncs[juce::jlimit (0, 5, v)]); };
                text << "     " << tr ("Hold: {0}", { slot.hold > 0 ? juce::String (slot.hold) : tr ("Off") }) << "   "
                     << tr ("Trig sync: {0}", { sync (slot.triggerSync) }) << "   " << tr ("Pos sync: {0}", { sync (slot.positionSync) });
            }
            g.setColour (colour::textSecondary);
            g.setFont (font (juce::jmax (9.0f, 11.0f * s)));
            g.drawFittedText (text, r.reduced (6.0f * s, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
            return;
        }

        // the editor area: beat grid, then the time slot's envelope (accent) and the volume slot's (blue)
        g.setColour (colour::bg0);
        g.fillRoundedRectangle (r, radius::md * s);
        const auto inner = r.reduced (8.0f * s);
        g.setColour (colour::gridBeat);
        for (int i = 1; i < 16; ++i)
            g.drawVerticalLine ((int) (inner.getX() + inner.getWidth() * (float) i / 16.0f), inner.getY(), inner.getBottom());
        g.setColour (colour::gridBar);
        for (int i = 1; i < 4; ++i)
            g.drawVerticalLine ((int) (inner.getX() + inner.getWidth() * (float) i / 4.0f), inner.getY(), inner.getBottom());
        for (int i = 1; i < 8; ++i)
            g.drawHorizontalLine ((int) (inner.getY() + inner.getHeight() * (float) i / 8.0f), inner.getX(), inner.getRight());
        const auto drawSlot = [&] (const grossbeat::Slot& slot, juce::Colour c, bool time) {
            if (slot.points.empty()) // an empty slot does nothing: time plays straight on, volume stays at full
            {
                g.setColour (c.withAlpha (0.5f));
                if (time)
                    g.drawLine (inner.getX(), inner.getY(), inner.getRight(), inner.getY() + inner.getHeight() * 0.5f, juce::jmax (1.0f, 1.5f * s));
                else
                    g.drawHorizontalLine ((int) inner.getY() + 1, inner.getX(), inner.getRight());
                return;
            }
            double length = 0.0;
            for (const auto& p : slot.points)
                length += p.dx;
            length = juce::jmax (length, 1.0e-6);
            juce::Path path;
            double x = 0.0;
            for (std::size_t i = 0; i < slot.points.size(); ++i)
            {
                x += slot.points[i].dx;
                const juce::Point<float> pt { inner.getX() + inner.getWidth() * (float) (x / length),
                                              inner.getBottom() - inner.getHeight() * juce::jlimit (0.0f, 1.0f, slot.points[i].y) };
                if (i == 0)
                    path.startNewSubPath (pt);
                else
                    path.lineTo (pt);
                g.setColour (c);
                g.fillEllipse (juce::Rectangle<float> (5.0f * s, 5.0f * s).withCentre (pt));
            }
            g.setColour (c);
            g.strokePath (path, juce::PathStrokeType (juce::jmax (1.5f, 2.0f * s)));
        };
        drawSlot (state ? state->slots[(std::size_t) (grossbeat::slotsPerKind + volumeSlot)] : grossbeat::Slot {}, colour::info.withAlpha (fx.enabled ? 0.9f : 0.3f), false);
        drawSlot (state ? state->slots[(std::size_t) timeSlot] : grossbeat::Slot {}, (fx.enabled ? accent : colour::textDisabled), true);
    }

    // --- the parametric EQ's graph (FL's Parametric EQ 2 arrangement) -----------------------------------

    static constexpr double eqRange = 18.0; // FL's graph spans ±18 dB

    // rows above the graph (octaves, band names), frequencies under it, dB scale at its right
    juce::Rectangle<float> eqGraph (juce::Rectangle<float> r) const
    {
        const auto s = scale();
        return r.withTrimmedTop (29.0f * s).withTrimmedBottom (16.0f * s).withTrimmedRight (22.0f * s);
    }

    void paintEq (juce::Graphics& g, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        const auto graph = eqGraph (r);
        g.setFont (font (juce::jmax (7.5f, 9.0f * s), true));

        // octave marks (C1…C9) and the usual band names, on plates like FL's
        const auto octaveRow = juce::Rectangle<float> (graph.getX(), r.getY() + 1.0f * s, graph.getWidth(), 11.0f * s);
        for (int o = 1; o <= 9; ++o)
        {
            const auto x = peqview::xForHz (graph, 32.703 * std::pow (2.0, o - 1));
            const auto next = peqview::xForHz (graph, 32.703 * std::pow (2.0, o));
            const auto plate = juce::Rectangle<float> (x, octaveRow.getY(), juce::jmin (next, graph.getRight()) - x - 1.0f, octaveRow.getHeight());
            g.setColour (colour::bg2);
            g.fillRect (plate);
            g.setColour (colour::textDisabled);
            g.drawText ("C" + juce::String (o), plate.withTrimmedLeft (2.0f), juce::Justification::centredLeft, false);
        }
        struct Range { double lo, hi; const char* name; };
        static const Range ranges[] = { { 20, 60, TR_NOOP ("Sub") }, { 60, 250, TR_NOOP ("Bass") }, { 250, 500, TR_NOOP ("Low mid") },
                                        { 500, 2000, TR_NOOP ("Mid") }, { 2000, 4000, TR_NOOP ("High mid") }, { 4000, 6000, TR_NOOP ("Prs") },
                                        { 6000, 20000, TR_NOOP ("Treble") } };
        for (const auto& range : ranges)
        {
            const auto x0 = peqview::xForHz (graph, range.lo), x1 = peqview::xForHz (graph, range.hi);
            const auto plate = juce::Rectangle<float> (x0, r.getY() + 15.0f * s, x1 - x0 - 1.0f, 11.0f * s);
            g.setColour (colour::bg3);
            g.fillRect (plate);
            g.setColour (colour::textSecondary);
            g.drawFittedText (caps (tr (range.name)), plate.toNearestInt(), juce::Justification::centred, 1, 0.6f);
        }

        drawParametricEqCurve (g, graph, fx.params, juce::jmax (8000.0, engine.getTransport().sampleRate.load()), fx.enabled, eqSelected, eqRange, false);

        // frequencies under the graph, dB at its right
        g.setColour (colour::textDisabled);
        for (auto [hz, text] : { std::pair { 20.0, "20" }, { 50.0, "50" }, { 100.0, "100" }, { 200.0, "200" }, { 500.0, "500" },
                                 { 1000.0, "1k" }, { 2000.0, "2k" }, { 5000.0, "5k" }, { 10000.0, "10k" } })
            g.drawText (text, juce::Rectangle<float> (peqview::xForHz (graph, hz) + 2.0f, graph.getBottom() + 2.0f * s, 40.0f * s, 11.0f * s),
                        juce::Justification::centredLeft, false);
        for (int db = -18; db <= 18; db += 6)
            g.drawText ((db > 0 ? "+" : "") + juce::String (db),
                        juce::Rectangle<float> (graph.getRight() + 1.0f, peqview::yForDb (graph, db, eqRange) - 6.0f * s, 21.0f * s, 12.0f * s),
                        juce::Justification::centredRight, false);

        // the band handles
        for (int b = 0; b < peq::bands; ++b)
        {
            if (peq::type (fx.params[(std::size_t) peq::param (b, 0)]) == peq::BandType::off)
                continue;
            const auto centre = peqview::handlePosition (graph, fx.params, b, eqRange);
            const auto d = juce::jmax (12.0f, 16.0f * s);
            const auto h = juce::Rectangle<float> (d, d).withCentre (centre);
            const auto c = peqview::bandColour (b);
            if (b == eqSelected)
            {
                g.setColour (c.withAlpha (0.25f));
                g.fillEllipse (h.expanded (4.0f));
            }
            g.setColour (colour::bg0.withAlpha (0.7f));
            g.fillEllipse (h);
            g.setColour (c.withAlpha (fx.enabled ? 1.0f : 0.4f));
            g.drawEllipse (h.reduced (1.0f), juce::jmax (1.5f, 2.0f * s));
            g.setFont (font (juce::jmax (8.0f, 9.5f * s), true));
            g.drawText (juce::String (b + 1), h, juce::Justification::centred);
        }
    }

    void setParams (const std::vector<std::pair<int, float>>& changes)
    {
        if (current() == nullptr)
            return;
        if (usesBar())
            bar.willEdit();
        engine.editSong ([t = track, sl = slot, changes] (Song& song) {
            auto& params = song.mixer[(std::size_t) t].effects[(std::size_t) sl].params;
            for (auto [p, v] : changes)
                if (p >= 0 && p < maxEffectParams)
                    params[(std::size_t) p] = juce::jlimit (0.0f, 1.0f, v);
        });
    }

    bool eqMouseDown (const juce::MouseEvent& e, const LayoutItem& item)
    {
        const auto* fx = current();
        const auto graph = eqGraph (rectOf (item));
        const auto band = peqview::bandAt (graph, fx->params, e.position, eqRange);
        eqSelected = band;
        repaint();
        if (band < 0)
            return true;
        if (e.mods.isPopupMenu())
        {
            peqview::showBandMenu (*this, fx->params, band, [this] (std::vector<std::pair<int, float>> changes) { setParams (changes); });
            return true;
        }
        eqDragOffset = peqview::handlePosition (graph, fx->params, band, eqRange) - e.position;
        eqDragging = true;
        return true;
    }

    void eqMouseDrag (const juce::MouseEvent& e)
    {
        const auto* fx = current();
        if (fx == nullptr || eqSelected < 0 || dragItem < 0)
            return;
        const auto graph = eqGraph (rectOf (activeLayout().items[(std::size_t) dragItem]));
        const auto p = e.position + eqDragOffset;
        std::vector<std::pair<int, float>> changes { { peq::param (eqSelected, 1), peq::hzValue (peqview::hzForX (graph, p.x)) } };
        if (peqview::hasGain (fx->params, eqSelected))
            changes.push_back ({ peq::param (eqSelected, 2), peq::gainValue (peqview::dbForY (graph, p.y, eqRange)) });
        setParams (changes);
        ui::showValueTip (*this, juce::Rectangle<int> (6, 6).withCentre (e.getPosition()), peqview::bandText (fx->params, eqSelected));
    }

    // --- Soundgoodizer: Amount and the preset A–D live in the slot's state ---------------------------------

    std::optional<maximusfx::Blob> soundgoodizerBlob() const
    {
        const auto* fx = current();
        return fx != nullptr ? maximusfx::unpack (fx->state) : std::nullopt;
    }

    // the approximation keeps the plugin's own state (version, preset, amount); edits rebuild its chain
    std::optional<maximusfx::SoundgoodizerState> soundgoodizerApproxState() const
    {
        const auto* fx = current();
        return fx != nullptr && sgApprox ? maximusfx::parseSoundgoodizer (fx->state) : std::nullopt;
    }

    void writeSoundgoodizerApprox (int preset, int amount)
    {
        engine.editSong ([t = track, sl = slot, rebuilt = soundgoodizerApproxSlot (preset, amount)] (Song& song) {
            auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) sl];
            fx.branches = rebuilt.branches;
            fx.state = rebuilt.state;
        });
    }

    float soundgoodizerAmount() const
    {
        if (sgApprox)
        {
            const auto sg = soundgoodizerApproxState();
            return sg ? (float) sg->amount / 1000.0f : 0.6f;
        }
        const auto blob = soundgoodizerBlob();
        return blob && blob->mix >= 0 ? (float) blob->mix / 1000.0f : 0.6f;
    }

    void writeSoundgoodizer (const maximusfx::Blob& blob)
    {
        engine.editSong ([t = track, sl = slot, bytes = maximusfx::pack (blob)] (Song& song) {
            song.mixer[(std::size_t) t].effects[(std::size_t) sl].state = bytes;
        });
    }

    void setSoundgoodizerAmount (float v)
    {
        if (sgApprox)
        {
            if (const auto sg = soundgoodizerApproxState())
                writeSoundgoodizerApprox (sg->preset, juce::roundToInt (v * 1000.0f));
            return;
        }
        if (auto blob = soundgoodizerBlob())
        {
            blob->mix = juce::roundToInt (v * 1000.0f);
            writeSoundgoodizer (*blob);
        }
    }

    const std::vector<std::uint8_t>& soundgoodizerPresetBytes (int preset)
    {
        auto& bytes = sgPresets[(std::size_t) preset];
        if (bytes.empty())
        {
            const auto roots = findFlStudioRoots();
            juce::MemoryBlock data;
            if (! roots.factoryData.empty()
                && juce::File (juce::String::fromUTF8 (roots.factoryData.c_str()))
                       .getChildFile ("Plugins/Fruity/Effects/Soundgoodizer/Data/Presets")
                       .getChildFile (maximusfx::soundgoodizerPresetFile (preset))
                       .loadFileAsData (data))
            {
                const auto* b = static_cast<const std::uint8_t*> (data.getData());
                bytes.assign (b, b + data.getSize());
            }
        }
        return bytes;
    }

    int soundgoodizerPreset()
    {
        if (sgApprox)
        {
            const auto sg = soundgoodizerApproxState();
            return sg ? sg->preset : -1;
        }
        const auto blob = soundgoodizerBlob();
        for (int p = 0; blob && p < 4; ++p)
            if (const auto& bytes = soundgoodizerPresetBytes (p); ! bytes.empty() && bytes == blob->state)
                return p;
        return -1;
    }

    void setSoundgoodizerPreset (int preset)
    {
        if (sgApprox)
        {
            if (const auto sg = soundgoodizerApproxState())
                writeSoundgoodizerApprox (preset, sg->amount);
            return;
        }
        auto blob = soundgoodizerBlob();
        const auto& bytes = soundgoodizerPresetBytes (preset);
        if (! blob || bytes.empty())
            return;
        blob->state = bytes;
        writeSoundgoodizer (*blob);
    }

    void paintSoundgoodizer (juce::Graphics& g, const juce::String& id, juce::Rectangle<float> r, const EffectSlot& fx)
    {
        using namespace theme;
        const auto s = scale();
        if (id == "sgamount")
        {
            const auto amount = soundgoodizerAmount();
            drawKnob (g, r, amount, false, fx.enabled ? accent : colour::textDisabled, sgDragging);
            const auto body = r.reduced (r.getWidth() * 0.16f);
            g.setGradientFill (juce::ColourGradient (colour::bg6, body.getCentreX(), body.getY(), colour::bg4, body.getCentreX(), body.getBottom(), false));
            g.fillEllipse (body);
            g.setColour (colour::borderStrong);
            g.drawEllipse (body, 1.0f);
            g.setColour (colour::textPrimary);
            g.setFont (font (juce::jmax (10.0f, 18.0f * s), true));
            g.drawText (juce::String (amount * 100.0f, 0) + "%", body, juce::Justification::centred, false);
            return;
        }
        const auto chosen = soundgoodizerPreset();
        for (int p = 0; p < 4; ++p)
        {
            const auto cell = juce::Rectangle<float> (r.getX() + r.getWidth() * (float) p / 4.0f, r.getY(), r.getWidth() / 4.0f, r.getHeight());
            const auto led = juce::Rectangle<float> (8.0f * s, 8.0f * s).withCentre ({ cell.getCentreX(), cell.getY() + 8.0f * s });
            g.setColour (p == chosen ? (fx.enabled ? accent : colour::textSecondary) : colour::ledOff);
            g.fillEllipse (led);
            g.setColour (p == chosen ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (juce::jmax (10.0f, 20.0f * s), true));
            g.drawText (juce::String::charToString ((juce::juce_wchar) ('A' + p)), cell.withTrimmedTop (16.0f * s), juce::Justification::centred, false);
        }
    }

    // a pad sets two parameters at once
    // --- round 137: Gemini Doubler's graphs are editable (the reference: "Gain, Pan, Delay and Detune can be controlled
    // using the graphs"): drag a voice on the pan / gain half circle or on the delay / detune map, drag the EQ's L / H
    // markers (frequency across, gain up), double-click a marker = that shelf on / off
    static bool isGeminiGraph (const LayoutItem& item)
    {
        return item.type == Item::display && item.display != nullptr
               && (std::strcmp (item.display, "geminipan") == 0 || std::strcmp (item.display, "geminimap") == 0 || std::strcmp (item.display, "geminieq") == 0);
    }
    // the handles of a graph: (parameter across / around, parameter up / out, position on screen)
    struct GeminiHandle { int px = -1, py = -1, on = -1; juce::Point<float> at; };
    std::vector<GeminiHandle> geminiHandles (const LayoutItem& item) const
    {
        using namespace voicetools::gemini;
        std::vector<GeminiHandle> out;
        const auto s = scale();
        const auto inner = rectOf (item).reduced (6.0f * s);
        const juce::String id (item.display);
        if (id == "geminipan")
        {
            const auto c = juce::Point<float> (inner.getCentreX(), inner.getBottom() - 4.0f * s);
            const auto radius = juce::jmin (inner.getWidth() * 0.5f, inner.getHeight()) - 8.0f * s;
            auto add = [&] (int panP, int gainP, int onP) {
                const auto a = juce::degreesToRadians ((float) panDegrees (valueOf (panP)));
                const auto rr = radius * juce::jlimit (0.05f, 1.0f, valueOf (gainP));
                out.push_back ({ panP, gainP, onP, { c.x + rr * std::sin (a), c.y - rr * std::cos (a) } });
            };
            add (directPan, directGain, directOn);
            for (int v = 0; v < voices; ++v)
                if (v < 2 || valueOf (voiceParam (v, vOn)) >= 0.5f)
                    add (voiceParam (v, vPan), voiceParam (v, vGain), voiceParam (v, vOn));
        }
        else if (id == "geminimap")
        {
            for (int v = 0; v < voices; ++v)
                if (v < 2 || valueOf (voiceParam (v, vOn)) >= 0.5f)
                    out.push_back ({ voiceParam (v, vDelay), voiceParam (v, vDetune), voiceParam (v, vOn),
                                     { inner.getX() + inner.getWidth() * valueOf (voiceParam (v, vDelay)),
                                       inner.getBottom() - inner.getHeight() * valueOf (voiceParam (v, vDetune)) } });
        }
        else
        {
            auto add = [&] (int freqP, int gainP, int onP) {
                const auto hz = freqHz (valueOf (freqP));
                const auto x = (float) (std::log (juce::jlimit (20.0, 20000.0, hz) / 20.0) / std::log (1000.0));
                const bool measured = valueOf (eqLaw) >= 0.5f; // round 165w: the height is the gain's own scale
                const auto y = valueOf (onP) >= 0.5f ? valueOf (gainP) : shelfValue (0.0, measured);
                out.push_back ({ freqP, gainP, onP, { inner.getX() + inner.getWidth() * x, inner.getBottom() - inner.getHeight() * y } });
            };
            add (lowFreq, lowGain, lowOn);
            add (highFreq, highGain, highOn);
        }
        return out;
    }
    int geminiNearest (const LayoutItem& item, juce::Point<float> p) const
    {
        const auto handles = geminiHandles (item);
        int best = -1;
        auto bestD = 18.0f * scale();
        for (int k = 0; k < (int) handles.size(); ++k)
            if (const auto d = handles[(std::size_t) k].at.getDistanceFrom (p); d < bestD)
                best = k, bestD = d;
        return best;
    }
    void geminiMouse (const juce::MouseEvent& e, const LayoutItem& item)
    {
        using namespace voicetools::gemini;
        const auto handles = geminiHandles (item);
        if (geminiHandle < 0 || geminiHandle >= (int) handles.size())
            return;
        const auto& h = handles[(std::size_t) geminiHandle];
        const auto s = scale();
        const auto inner = rectOf (item).reduced (6.0f * s);
        const juce::String id (item.display);
        float x = 0.0f, y = 0.0f;
        if (id == "geminipan")
        {
            const auto c = juce::Point<float> (inner.getCentreX(), inner.getBottom() - 4.0f * s);
            const auto radius = juce::jmax (1.0f, juce::jmin (inner.getWidth() * 0.5f, inner.getHeight()) - 8.0f * s);
            const auto deg = juce::jlimit (-45.0f, 45.0f, juce::radiansToDegrees (std::atan2 (e.position.x - c.x, juce::jmax (0.001f, c.y - e.position.y))));
            x = (deg + 45.0f) / 90.0f;
            y = juce::jlimit (0.0f, 1.0f, e.position.getDistanceFrom (c) / radius);
        }
        else if (id == "geminimap")
        {
            x = juce::jlimit (0.0f, 1.0f, (e.position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
            y = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - e.position.y) / juce::jmax (1.0f, inner.getHeight()));
        }
        else
        {
            const auto fx = juce::jlimit (0.0f, 1.0f, (e.position.x - inner.getX()) / juce::jmax (1.0f, inner.getWidth()));
            x = freqValue (20.0 * std::pow (1000.0, (double) fx));
            y = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - e.position.y) / juce::jmax (1.0f, inner.getHeight()));
        }
        std::vector<std::pair<int, float>> changes { { h.px, x }, { h.py, y } };
        if (id == "geminieq" && valueOf (h.on) < 0.5f) // dragging a marker turns its shelf on
            changes.push_back ({ h.on, 1.0f });
        setParams (changes);
        ui::showValueTip (*this, rectOf (item).toNearestInt(), valueText (h.px, x) + " / " + valueText (h.py, y));
    }
    int geminiHandle = -1;
    float grHold = 0.0f; // round 137: the tall reduction meter's held value
    float gcLevel = 0.0f, gcHold = 0.0f; // round 152: the bipolar gain-change meter (dB, signed) and its held change
    bool geminiDragging = false;

    void xyMouse (const juce::MouseEvent& e, const LayoutItem& item)
    {
        const auto inner = rectOf (item).reduced (6.0f * scale());
        const auto px = plugui::paramIndex (kind, item.param), py = plugui::paramIndex (kind, item.param2);
        const auto x = juce::jlimit (0.0f, 1.0f, (e.position.x - inner.getX()) / inner.getWidth());
        const auto y = juce::jlimit (0.0f, 1.0f, (inner.getBottom() - e.position.y) / inner.getHeight());
        setParams ({ { px, x }, { py, y } });
        ui::showValueTip (*this, rectOf (item).toNearestInt(), valueText (px, x) + " / " + valueText (py, y));
    }

    // --- Fruity WaveShaper's curve (round 165: FL's point editor) ---------------------------------------
    // FL's gestures: right-click adds a point (hold and drag to place it), left-drag moves one (Shift keeps its
    // level, Ctrl its position), Alt+click or right-click › Delete removes it, the hollow handle between two points
    // bends the segment (Ctrl: fine; right-click: straight again); right-click a point for its shape and value.
    static bool isWaveShaperGraph (const LayoutItem& item) { return item.type == Item::display && juce::String (item.display) == "waveshaper"; }

    bool wsUnipolar() const { return valueOf (plugui::paramIndex (kind, "Unipolar")) >= 0.5f; }

    flshape::WaveShaperState wsState() const
    {
        const auto* fx = current();
        if (fx == nullptr)
            return flshape::waveShaperForEditing ({});
        auto s = flshape::waveShaperForEditing (fx->state, fx->params.data());
        if (wsDragState)
            s.points = wsDragPoints; // shown at once while the edit travels to the engine
        return s;
    }

    const std::vector<std::int16_t>& wsTable (const std::vector<flshape::WsPoint>& points, bool unipolar) const
    {
        // the curve as the engine shapes it (one table per change of the points)
        auto same = wsCacheUni == unipolar && wsCachePoints.size() == points.size();
        for (std::size_t i = 0; same && i < points.size(); ++i)
            same = wsCachePoints[i].dt == points[i].dt && wsCachePoints[i].y == points[i].y && wsCachePoints[i].tension == points[i].tension
                   && wsCachePoints[i].mode == points[i].mode;
        if (! same || wsCacheTable.empty())
        {
            wsCachePoints = points;
            wsCacheUni = unipolar;
            wsCacheTable = flshape::shaperTable (points, unipolar);
        }
        return wsCacheTable;
    }

    juce::Rectangle<float> wsArea (const LayoutItem& item) const { return rectOf (item).reduced (6.0f * scale()); }

    static juce::Point<float> wsPos (juce::Rectangle<float> area, float x, float y)
    {
        return { area.getX() + area.getWidth() * x, area.getBottom() - area.getHeight() * y };
    }

    // the tension handle of the segment that ends at point i (x < 0: none — a hold, or too narrow to grab)
    juce::Point<float> wsHandlePos (juce::Rectangle<float> area, const flshape::WaveShaperState& ws, const std::vector<float>& times, int i) const
    {
        if (i < 1 || i >= (int) ws.points.size() || ws.points[(std::size_t) i].mode == 2)
            return { -2.0f, -2.0f };
        const auto x0 = times[(std::size_t) i - 1], x1 = times[(std::size_t) i];
        if ((x1 - x0) * area.getWidth() < 16.0f * scale())
            return { -2.0f, -2.0f };
        const auto xm = 0.5f * (x0 + x1);
        return wsPos (area, xm, flshape::waveShaperLevelAt (wsTable (ws.points, wsUnipolar()), wsUnipolar(), xm));
    }

    int wsPointAt (const LayoutItem& item, juce::Point<float> p) const
    {
        const auto ws = wsState();
        const auto times = flshape::waveShaperTimes (ws.points);
        const auto area = wsArea (item);
        int best = -1;
        auto bestD = 8.0f * scale();
        for (std::size_t i = 0; i < ws.points.size(); ++i)
            if (const auto d = wsPos (area, times[i], ws.points[i].y).getDistanceFrom (p); d <= bestD)
                best = (int) i, bestD = d;
        return best;
    }

    int wsHandleAt (const LayoutItem& item, juce::Point<float> p) const
    {
        const auto ws = wsState();
        const auto times = flshape::waveShaperTimes (ws.points);
        const auto area = wsArea (item);
        for (int i = 1; i < (int) ws.points.size(); ++i)
            if (const auto h = wsHandlePos (area, ws, times, i); h.x > -1.0f && h.getDistanceFrom (p) <= 7.0f * scale())
                return i;
        return -1;
    }

    juce::Point<float> wsMouseValue (const LayoutItem& item, juce::Point<float> p) const
    {
        const auto area = wsArea (item);
        return { juce::jlimit (0.0f, 1.0f, (p.x - area.getX()) / juce::jmax (1.0f, area.getWidth())),
                 juce::jlimit (0.0f, 1.0f, (area.getBottom() - p.y) / juce::jmax (1.0f, area.getHeight())) };
    }

    void wsWrite (std::vector<flshape::WsPoint> points)
    {
        const auto* fx = current();
        if (fx == nullptr)
            return;
        auto s = flshape::waveShaperForEditing (fx->state, fx->params.data());
        s.points = std::move (points);
        s.unipolar = wsUnipolar();
        if (usesBar())
            bar.willEdit();
        engine.editSong ([t = track, sl = slot, bytes = flshape::writeWaveShaper (s, fx->state)] (Song& song) {
            song.mixer[(std::size_t) t].effects[(std::size_t) sl].state = bytes;
        });
        repaint();
    }

    juce::String wsValueText (float y) const
    {
        // the level as FL's hint shows it: percent of the map (bipolar 0…100 %, unipolar −100…100 %)
        return wsUnipolar() ? juce::String (juce::roundToInt ((y * 2.0f - 1.0f) * 100.0f)) + "%" : juce::String (juce::roundToInt (y * 100.0f)) + "%";
    }

    void wsShowTip (const LayoutItem& item, float x, float y)
    {
        ui::showValueTip (*this, rectOf (item).toNearestInt(), tr ("In") + " " + juce::String (juce::roundToInt (x * 100.0f)) + "%  " + tr ("Out") + " " + wsValueText (y));
    }

    // mouse down on the graph; true = a drag follows
    bool wsMouseDown (const juce::MouseEvent& e, const LayoutItem& item)
    {
        auto points = wsState().points;
        const auto point = wsPointAt (item, e.position);
        const auto handle = point < 0 ? wsHandleAt (item, e.position) : -1;
        const auto v = wsMouseValue (item, e.position);
        wsDownMods = e.mods;

        if (e.mods.isPopupMenu())
        {
            if (point >= 0)
                wsPointMenu (point);
            else if (handle >= 0) // straight again
            {
                points[(std::size_t) handle].tension = 0.0f;
                wsWrite (points);
            }
            else // FL: a new point at the mouse, dragged while the button is held
            {
                wsPoint = flshape::addWaveShaperPoint (points, v.x, v.y);
                wsDragPoints = points;
                wsDragState = true;
                wsWrite (points);
                wsShowTip (item, v.x, v.y);
                return true;
            }
            return false;
        }
        if (point >= 0 && e.mods.isAltDown()) // FL: Alt+click deletes
        {
            if (flshape::removeWaveShaperPoint (points, point))
                wsWrite (points);
            return false;
        }
        if (point >= 0)
        {
            wsPoint = point;
            wsDragPoints = points;
            wsDragState = true;
            const auto t = flshape::waveShaperTimes (points);
            wsShowTip (item, t[(std::size_t) point], points[(std::size_t) point].y);
            return true;
        }
        if (handle >= 0)
        {
            // which way the tension bends this segment: the handle follows the mouse up and down
            wsTension = handle;
            wsTensionStart = points[(std::size_t) handle].tension;
            auto bent = points;
            bent[(std::size_t) handle].tension = juce::jlimit (-1.0f, 1.0f, wsTensionStart + (wsTensionStart > 0.8f ? -0.1f : 0.1f));
            const auto times = flshape::waveShaperTimes (points);
            const auto xm = 0.5f * (times[(std::size_t) handle - 1] + times[(std::size_t) handle]);
            const auto uni = wsUnipolar();
            const auto a = flshape::waveShaperLevelAt (flshape::shaperTable (points, uni), uni, xm);
            const auto b = flshape::waveShaperLevelAt (flshape::shaperTable (bent, uni), uni, xm);
            const auto up = (b - a) * (bent[(std::size_t) handle].tension - wsTensionStart);
            wsTensionSign = up < 0.0f ? -1.0f : 1.0f;
            wsDragPoints = points;
            wsDragState = true;
            return true;
        }
        return false;
    }

    void wsMouseDrag (const juce::MouseEvent& e, const LayoutItem& item)
    {
        if (! wsDragState)
            return;
        auto points = wsDragPoints;
        if (wsTension >= 0 && wsTension < (int) points.size())
        {
            const auto dy = -(float) e.getDistanceFromDragStartY() / ((e.mods.isCtrlDown() ? 600.0f : 150.0f) * scale());
            points[(std::size_t) wsTension].tension = juce::jlimit (-1.0f, 1.0f, wsTensionStart + wsTensionSign * dy);
            ui::showValueTip (*this, rectOf (item).toNearestInt(), tr ("Tension") + " " + juce::String (juce::roundToInt (points[(std::size_t) wsTension].tension * 100.0f)) + "%");
        }
        else if (wsPoint >= 0 && wsPoint < (int) points.size())
        {
            const auto t = flshape::waveShaperTimes (points);
            auto v = wsMouseValue (item, e.position);
            // FL: Shift keeps the level, Ctrl keeps the position
            if (e.mods.isShiftDown() && ! wsDownMods.isShiftDown())
                v.y = points[(std::size_t) wsPoint].y;
            if (e.mods.isCtrlDown())
                v.x = t[(std::size_t) wsPoint];
            flshape::moveWaveShaperPoint (points, wsPoint, v.x, v.y);
            const auto moved = flshape::waveShaperTimes (points);
            wsShowTip (item, moved[(std::size_t) wsPoint], points[(std::size_t) wsPoint].y);
        }
        else
            return;
        wsDragPoints = points;
        wsWrite (points);
    }

    void wsMouseUp()
    {
        wsDragState = false;
        wsPoint = wsTension = -1;
        ui::hideValueTip();
        repaint();
    }

    void wsPointMenu (int point)
    {
        const auto ws = wsState();
        if (point < 0 || point >= (int) ws.points.size())
            return;
        const auto& p = ws.points[(std::size_t) point];
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Out") + " " + wsValueText (p.y));
        const auto inner = point > 0 && point + 1 < (int) ws.points.size();
        menu.addItem (tr ("Delete"), inner, false, [this, point] {
            auto pts = wsState().points;
            if (flshape::removeWaveShaperPoint (pts, point))
                wsWrite (pts);
        });
        if (point > 0) // the shape of the segment that ends here (FL's modes)
        {
            menu.addSectionHeader (tr ("Mode"));
            for (const auto& m : flshape::waveShaperModes())
                menu.addItem (tr (m.name), true, p.mode == m.mode, [this, point, mode = m.mode] {
                    auto pts = wsState().points;
                    if (point < (int) pts.size())
                    {
                        pts[(std::size_t) point].mode = mode;
                        wsWrite (pts);
                    }
                });
        }
        menu.addSeparator();
        menu.addItem (tr ("Copy value"), [y = p.y] { wsCopiedValue = y; });
        menu.addItem (tr ("Paste value"), wsCopiedValue >= 0.0f, false, [this, point] {
            auto pts = wsState().points;
            if (point < (int) pts.size())
            {
                pts[(std::size_t) point].y = wsCopiedValue;
                wsWrite (pts);
            }
        });
        menu.addItem (tr ("Type in value..."), [this, point] { wsTypeValue (point); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void wsTypeValue (int point)
    {
        const auto ws = wsState();
        if (point < 0 || point >= (int) ws.points.size())
            return;
        const auto uni = wsUnipolar();
        const auto y = ws.points[(std::size_t) point].y;
        auto* window = new juce::AlertWindow (tr ("Point value"), uni ? tr ("Percent (-100 to 100)") : tr ("Percent (0 to 100)"), juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("value", juce::String (uni ? (y * 2.0f - 1.0f) * 100.0f : y * 100.0f, 1));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<NativeEffectEditor> self (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create ([self, window, point, uni] (int result) {
                                     if (result != 1 || self == nullptr)
                                         return;
                                     const auto typed = window->getTextEditorContents ("value").getFloatValue() / 100.0f;
                                     auto pts = self->wsState().points;
                                     if (point < (int) pts.size())
                                     {
                                         pts[(std::size_t) point].y = juce::jlimit (0.0f, 1.0f, uni ? (typed + 1.0f) * 0.5f : typed);
                                         self->wsWrite (pts);
                                     }
                                 }),
                                 true);
    }

    juce::String wsHint (const LayoutItem& item, juce::Point<float> p) const
    {
        if (wsPointAt (item, p) >= 0)
            return tr ("Point: [LMB] drag moves ([Shift] keeps the level, [Ctrl] the position)  [Alt][LMB] delete  [RMB] shape, value, delete");
        if (wsHandleAt (item, p) >= 0)
            return tr ("Tension: [LMB] drag up / down bends the segment ([Ctrl] fine)  [RMB] straight");
        return tr ("Shaping curve: [RMB] add a point (hold and drag to place it)  [LMB] drag a point  [Alt][LMB] delete a point");
    }

    int wsPoint = -1, wsTension = -1, wsHoverPoint = -1, wsHoverHandle = -1;
    float wsTensionStart = 0.0f, wsTensionSign = 1.0f;
    bool wsDragState = false;
    std::vector<flshape::WsPoint> wsDragPoints;
    juce::ModifierKeys wsDownMods;
    mutable std::vector<flshape::WsPoint> wsCachePoints;
    mutable std::vector<std::int16_t> wsCacheTable;
    mutable bool wsCacheUni = false;
    static inline float wsCopiedValue = -1.0f;

    // --- mouse -----------------------------------------------------------------------------------------

    int headerItemAt (juce::Point<float> p) const
    {
        if (! headerArea().contains (p))
            return 0;
        if (menuButton().expanded (2.0f).contains (p))
            return 1;
        if (usesBar())
            return bar.contains (p) ? 5 : 0;
        if (presetBox().contains (p))
            return 2;
        if (prevButton().contains (p))
            return 3;
        if (nextButton().contains (p))
            return 4;
        return 0;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        const auto h = headerItemAt (p);
        const auto i = h == 0 ? itemAt (p) : -1;
        if (i >= 0 && (hasStepArrows (activeLayout().items[(std::size_t) i]) || isRhythmEdit (activeLayout().items[(std::size_t) i])))
        {
            mouseAt = p; // the menu's arrows light under the mouse
            repaint();
        }
        if (usesBar() && bar.hover (p))
            repaint();
        if (h != hoverHeader || i != hoverItem)
        {
            hoverHeader = h;
            hoverItem = i;
            anim.setHot (i);
            repaint();
        }
        if (i >= 0 && isWaveShaperGraph (activeLayout().items[(std::size_t) i])) // round 165: points and handles light up
        {
            const auto& item = activeLayout().items[(std::size_t) i];
            const auto point = wsPointAt (item, p), handle = point < 0 ? wsHandleAt (item, p) : -1;
            if (point != wsHoverPoint || handle != wsHoverHandle)
            {
                wsHoverPoint = point;
                wsHoverHandle = handle;
                repaint();
            }
            setMouseCursor (point >= 0 ? juce::MouseCursor::DraggingHandCursor
                                       : handle >= 0 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::CrosshairCursor);
            return;
        }
        if (wsHoverPoint >= 0 || wsHoverHandle >= 0)
        {
            wsHoverPoint = wsHoverHandle = -1;
            repaint();
        }
        setMouseCursor (i >= 0 || h > 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override
    {
        bar.hoverExit();
        hoverHeader = 0;
        hoverItem = -1;
        anim.setHot (-1);
        mouseAt = { -1.0f, -1.0f };
        repaint();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (current() == nullptr)
            return;

        if (const auto h = headerItemAt (e.position); h > 0)
        {
            if (h == 5)
            {
                const pluginheader::PresetBar::FlPresets fl { [this] (juce::PopupMenu& m) { addFlPresets (m); },
                                                              [this] (int delta) { stepPreset (delta); }, [this] { return presetName; } };
                bar.click (bar.partAt (e.position), *this, &fl);
                return;
            }
            if (h == 1 || h == 2 || e.mods.isPopupMenu())
                showOptionsMenu (h == 2);
            else
                stepPreset (h == 3 ? -1 : 1);
            return;
        }

        const auto index = itemAt (e.position);
        if (index < 0)
            return;
        bar.beginGesture();

        const auto& item = activeLayout().items[(std::size_t) index];
        const auto r = rectOf (item);

        if (! e.mods.isPopupMenu() && isMixItem (item) && captionArea (item).contains (e.position))
        {
            pluginheader::toggleMixLock (kind);
            repaint();
            return;
        }

        if (hasStyle (item, plugui::pageTabs))
        {
            const auto n = juce::jmax (1, (int) plugui::optionLabels (item).size());
            const auto along = isVertical (item) ? (e.position.y - r.getY()) / r.getHeight() : (e.position.x - r.getX()) / r.getWidth();
            const auto o = juce::jlimit (0, n - 1, (int) (along * (float) n));
            if (item.steps > 0) // one digit of the page (EQUO: target ×1, bank ×3)
                page += (o - (page / item.steps) % n) * item.steps;
            else
                page = o;
            hoverItem = -1;
            repaint();
            return;
        }

        if (isWaveShaperGraph (item)) // round 165: FL's point editor
        {
            if (wsMouseDown (e, item))
                dragItem = index;
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "tunekeys")
        {
            tuneKeyClick (rectOf (item), e.position);
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "tuneautokey")
        {
            autoKey (rectOf (item));
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "pitchring") // HOLD
        {
            tuneHold = ! tuneHold;
            if (tuneHold && ! pitchHistory.empty())
                holdPoint = pitchHistory.back();
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "vumeter") // the VU's selector: gain reduction / output / input
        {
            vuMode = (vuMode + 1) % 3;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "iometer") // round 135w: reset the peaks and the clip lamp
        {
            ioMeters = {};
            ioClip = false;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "gcmeter") // round 152: reset the held change
        {
            gcHold = 0.0f;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "grmeter") // round 137: reset the held reduction
        {
            grHold = 0.0f;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "outmeter") // round 135w: reset the peak and the clip lamp
        {
            outMax = 0.0f;
            outClip = false;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "taptempo") // round 129: the tap pad
        {
            const auto now = juce::Time::getMillisecondCounter();
            if (now - lastTap > 2000)
                tapIntervals.clear();
            else
            {
                tapIntervals.push_back ((double) (now - lastTap));
                if (tapIntervals.size() > 4)
                    tapIntervals.erase (tapIntervals.begin());
                double sum = 0.0;
                for (const auto t : tapIntervals) sum += t;
                const auto msAvg = sum / (double) tapIntervals.size();
                tapBpm = 60000.0 / msAvg;
                const auto timeParam = plugui::paramIndex (kind, item.param), syncParam = plugui::paramIndex (kind, item.param2);
                std::vector<std::pair<int, float>> changes;
                if (const auto sync = presets::valueFromText (kind, syncParam, "ms"); sync && syncParam >= 0)
                    changes.push_back ({ syncParam, *sync });
                if (const auto t = presets::valueFromText (kind, timeParam, juce::String (msAvg, 1).toStdString() + " ms"); t && timeParam >= 0)
                    changes.push_back ({ timeParam, *t });
                bar.beginGesture();
                setParams (changes);
            }
            lastTap = now;
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "vusel") // round 129: INPUT | GR | OUTPUT
        {
            const auto r = rectOf (item);
            static constexpr int order[] = { 1, 0, 2 };
            vuMode = order[juce::jlimit (0, 2, (int) ((e.position.x - r.getX()) / r.getWidth() * 3.0f))];
            repaint();
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "sgpresets")
        {
            const auto r = rectOf (item);
            setSoundgoodizerPreset (juce::jlimit (0, 3, (int) ((e.position.x - r.getX()) / r.getWidth() * 4.0f)));
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "sgamount")
        {
            dragItem = index;
            sgDragging = true;
            sgStart = soundgoodizerAmount();
            ui::showValueTip (*this, rectOf (item).toNearestInt(), juce::String (sgStart * 100.0f, 0) + "%");
            return;
        }
        if (item.type == Item::display && (juce::String (item.display) == "prismbands" || juce::String (item.display) == "emberbands"))
        {
            // band displays (round 108): drag a crossover line, or click a band to edit it (its page)
            const auto* fx = current();
            if (const auto p = dynui::crossoverNear (*fx, r, e.position.x, 6.0f * scale()); p >= 0)
            {
                dragItem = index;
                crossoverDrag = p;
                return;
            }
            if (! activeLayout().pages.empty())
            {
                page = juce::jlimit (0, (int) activeLayout().pages.size() - 1, dynui::bandAt (*fx, r, e.position.x));
                repaint();
            }
            return;
        }
        if (isGeminiGraph (item)) // round 137
        {
            geminiHandle = geminiNearest (item, e.position);
            if (geminiHandle >= 0)
            {
                dragItem = index;
                geminiDragging = true;
                bar.beginGesture();
            }
            return;
        }
        if (item.type == Item::display && juce::String (item.display) == "xy")
        {
            dragItem = index;
            xyDragging = true;
            xyMouse (e, item);
            return;
        }
        if (isRhythmPatterns (item)) // round 144: the rhythm editor's pattern menu
        {
            rhythmPatternsMenu (item);
            return;
        }
        if (isRhythmEdit (item))
        {
            if (e.mods.isPopupMenu())
            {
                rhythmPatternsMenu (item);
                return;
            }
            dragItem = index;
            rhythmMouse (e, item, true);
            return;
        }
        if (isBars (item))
        {
            dragItem = index;
            barsDragging = true;
            barsMouse (e, item, true);
            return;
        }
        if (item.type == Item::display) // the EQ graph
        {
            if (eqMouseDown (e, item) && eqDragging)
                dragItem = index;
            return;
        }

        const auto param = paramOf (item);
        if (param < 0)
            return;

        const auto spec = specFor (item);

        if (item.type == Item::menu && ! ui::isResetClick (e))
        {
            if (kind == EffectKind::parametricEq && item.colour >= 0)
            {
                eqSelected = item.colour;
                repaint();
                peqview::showBandMenu (*this, current()->params, item.colour, [this] (std::vector<std::pair<int, float>> changes) { setParams (changes); });
                return;
            }
            if (kind == EffectKind::hardwareInsert && (param == hwinsert::output || param == hwinsert::input))
            {
                showHwChannelMenu (param == hwinsert::output, r);
                return;
            }
            if (kind == EffectKind::fruitySend) // FL's list: no destination, then every other insert by name
            {
                juce::PopupMenu menu;
                const auto chosen = flbasic::sendTrack (valueOf (param));
                menu.addItem (tr ("None"), true, chosen <= 0, [this, param] { setValue (param, 0.0f); });
                const auto& mixer = engine.getSong().mixer;
                for (int t = 1; t < (int) mixer.size(); ++t)
                    if (t != track)
                        menu.addItem (juce::String (t) + "  " + insertName (t), engine.getSong().canRoute (track, t) || t == chosen, t == chosen,
                                      [this, param, t] { setValue (param, flbasic::sendTrackValue (t)); });
                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (r.toNearestInt())));
                return;
            }
            if (kind == EffectKind::lynxTune && param == lynxtune::scale) // round 128: + whole tone, diminished
            {
                const auto* fx = current();
                const auto more = fx != nullptr ? juce::roundToInt (fx->params[lynxtune::moreScales] * 2.0f) : 0;
                juce::PopupMenu menu;
                const auto labels = plugui::optionLabels (item);
                const auto chosen = plugui::optionAt (item, valueOf (param));
                for (int o = 0; o < (int) labels.size(); ++o)
                    menu.addItem (optionTexts (item)[(std::size_t) o], true, more == 0 && o == chosen,
                                  [this, param, v = plugui::optionValue (item, o)] { setParams ({ { param, v }, { lynxtune::moreScales, 0.0f } }); });
                menu.addSeparator();
                menu.addItem (tr ("Whole tone"), true, more == 1, [this] { setParams ({ { lynxtune::moreScales, 0.5f } }); });
                menu.addItem (tr ("Diminished"), true, more == 2, [this] { setParams ({ { lynxtune::moreScales, 1.0f } }); });
                menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (r.toNearestInt())));
                return;
            }
            const auto labels = plugui::optionLabels (item);
            const auto chosen = plugui::optionAt (item, valueOf (param));
            // round 114: wide mode menus step with the arrows at their ends (the reference reverbs' ◀ ▶), the middle opens the list
            if (hasStepArrows (item) && ! labels.empty())
            {
                const auto edge = 18.0f * scale();
                const auto dir = e.position.x < r.getX() + edge ? -1 : e.position.x > r.getRight() - edge ? 1 : 0;
                if (dir != 0)
                {
                    const auto n = (int) labels.size();
                    const auto next = (chosen + dir + n) % n;
                    setValue (param, plugui::optionValue (item, next));
                    ui::flashValueTip (*this, r.toNearestInt(), optionTexts (item)[(std::size_t) next]);
                    return;
                }
            }
            juce::PopupMenu menu;
            for (int o = 0; o < (int) labels.size(); ++o)
                menu.addItem (optionTexts (item)[(std::size_t) o], true, o == chosen,
                              [this, param, v = plugui::optionValue (item, o)] { setValue (param, v); });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (r.toNearestInt())));
            return;
        }

        if (e.mods.isPopupMenu())
        {
            ui::showValueMenu (*this, &engine, spec, valueOf (param), [this, param] (double v) { setValue (param, (float) v); });
            return;
        }

        if (ui::isResetClick (e))
        {
            setValue (param, (float) spec.defaultValue);
            ui::flashValueTip (*this, r.toNearestInt(), spec.text (spec.defaultValue));
            return;
        }

        switch (item.type)
        {
            case Item::radio:
            case Item::tabs:
            {
                const auto n = juce::jmax (1, (int) plugui::optionLabels (item).size());
                auto o = item.type == Item::radio || isVertical (item) ? (int) ((e.position.y - r.getY()) / r.getHeight() * (float) n)
                                                  : (int) ((e.position.x - r.getX()) / r.getWidth() * (float) n);
                if (item.type == Item::radio && item.columns > 0)
                    for (int k = 0; k < n; ++k)
                        if (gridCell (item, k, n).contains (e.position))
                            o = k;
                setValue (param, plugui::optionValue (item, juce::jlimit (0, n - 1, o)));
                return;
            }
            case Item::toggle:
                setValue (param, valueOf (param) >= 0.5f ? 0.0f : 1.0f);
                return;
            case Item::stepper:
                if (e.position.x > r.getRight() - juce::jmin (12.0f * scale(), r.getWidth() * 0.4f))
                {
                    const auto up = e.position.y < r.getY() + (item.label != nullptr ? 12.0f * scale() : 0.0f) + (r.getBottom() - r.getY()) * 0.5f;
                    const auto step = item.steps > 0 ? 1.0f / (float) item.steps : 0.01f;
                    setValue (param, valueOf (param) + (up ? step : -step));
                    ui::flashValueTip (*this, r.toNearestInt(), valueText (param, valueOf (param)));
                    return;
                }
                break;
            case Item::hslider:
            case Item::vslider:
            case Item::knob:
            case Item::value:
            case Item::menu:
            case Item::section:
            case Item::label:
            case Item::display:
                break;
        }

        dragItem = index;
        drag.begin (e, valueOf (param));
        ui::showValueTip (*this, r.toNearestInt(), spec.text (valueOf (param)));
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (wsDragState && dragItem >= 0) // round 165: the WaveShaper's curve
        {
            wsMouseDrag (e, activeLayout().items[(std::size_t) dragItem]);
            return;
        }
        if (crossoverDrag >= 0 && dragItem >= 0)
        {
            const auto r = rectOf (activeLayout().items[(std::size_t) dragItem]);
            const auto v = dynui::crossoverValueAt (*current(), r, e.position.x);
            setValue (crossoverDrag, v);
            ui::showValueTip (*this, r.toNearestInt(), valueText (crossoverDrag, v));
            return;
        }
        if (eqDragging)
        {
            eqMouseDrag (e);
            return;
        }
        if (sgDragging && dragItem >= 0)
        {
            const auto v = juce::jlimit (0.0f, 1.0f, sgStart - (float) e.getDistanceFromDragStartY() / (e.mods.isCtrlDown() ? 800.0f : 200.0f));
            setSoundgoodizerAmount (v);
            ui::showValueTip (*this, rectOf (activeLayout().items[(std::size_t) dragItem]).toNearestInt(), juce::String (v * 100.0f, 0) + "%");
            return;
        }
        if (xyDragging && dragItem >= 0)
        {
            xyMouse (e, activeLayout().items[(std::size_t) dragItem]);
            return;
        }
        if (geminiDragging && dragItem >= 0) // round 137
        {
            geminiMouse (e, activeLayout().items[(std::size_t) dragItem]);
            return;
        }
        if (barsDragging && dragItem >= 0)
        {
            barsMouse (e, activeLayout().items[(std::size_t) dragItem], false);
            return;
        }
        if (rhythmDrag >= 0 && dragItem >= 0) // round 144
        {
            rhythmMouse (e, activeLayout().items[(std::size_t) dragItem], false);
            return;
        }
        if (tuneRangeDrag >= 0) // round 147: LYNX Tune's range boundary
        {
            for (const auto& it : activeLayout().items)
                if (it.type == Item::display && juce::String (it.display) == "tunekeys")
                    tuneSetRangeAt (rectOf (it), e.position.x, false);
            return;
        }
        if (dragItem < 0 || ! drag.isActive())
            return;
        const auto& item = activeLayout().items[(std::size_t) dragItem];
        const auto param = paramOf (item);
        const auto spec = specFor (item);
        auto v = drag.update (e, spec);
        if (item.type == Item::hslider) // horizontal sliders follow the mouse sideways too
        {
            const auto r = rectOf (item);
            v = juce::jlimit (0.0, 1.0, (double) ((e.position.x - r.getX() - 4.0f * scale()) / juce::jmax (1.0f, r.getWidth() - 8.0f * scale())));
            if (e.mods.isCtrlDown())
                v = drag.update (e, spec);
        }
        setValue (param, (float) v);
        ui::showValueTip (*this, rectOf (item).toNearestInt(), spec.text (v));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        eqDragging = false;
        if (wsDragState) // round 165
        {
            wsMouseUp();
            dragItem = -1;
        }
        tuneKeyRelease(); // LYNX Tune's keyboard without LATCH: the edit lasts while the key is held
        if (crossoverDrag >= 0)
        {
            crossoverDrag = -1;
            dragItem = -1;
            ui::hideValueTip();
        }
        if (geminiDragging) // round 137
        {
            geminiDragging = false;
            geminiHandle = -1;
            dragItem = -1;
            ui::hideValueTip();
        }
        if (rhythmDrag >= 0 || (dragItem >= 0 && isRhythmEdit (activeLayout().items[(std::size_t) dragItem]))) // round 144
        {
            rhythmDrag = -1;
            dragItem = -1;
            ui::hideValueTip();
            repaint();
        }
        if (xyDragging || sgDragging || barsDragging)
        {
            xyDragging = sgDragging = barsDragging = false;
            dragItem = -1;
            ui::hideValueTip();
        }
        if (dragItem >= 0)
        {
            drag.end();
            dragItem = -1;
            ui::hideValueTip();
            repaint();
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto index = itemAt (e.position);
        if (index < 0)
            return;
        const auto& item = activeLayout().items[(std::size_t) index];
        if (isRhythmEdit (item)) // round 144: add / remove a tap
        {
            rhythmDoubleClick (e, item);
            return;
        }
        if (isRhythmPatterns (item) || isWaveShaperGraph (item))
            return;
        if (isGeminiGraph (item)) // round 137: an EQ marker = its shelf on / off
        {
            if (juce::String (item.display) == "geminieq")
                if (const auto k = geminiNearest (item, e.position); k >= 0)
                {
                    const auto h = geminiHandles (item)[(std::size_t) k];
                    setParams ({ { h.on, valueOf (h.on) >= 0.5f ? 0.0f : 1.0f } });
                }
            return;
        }
        if (item.type == Item::display) // EQ graph: the band under the mouse back to 0 dB
        {
            if (const auto* fx = current(); fx != nullptr)
                if (const auto b = peqview::bandAt (eqGraph (rectOf (item)), fx->params, e.position, eqRange); b >= 0)
                    setParams ({ { peq::param (b, 2), 0.5f } });
            return;
        }
        if (item.type == Item::radio || item.type == Item::tabs || item.type == Item::toggle || item.type == Item::menu || paramOf (item) < 0)
            return;
        const auto spec = specFor (item);
        setValue (paramOf (item), (float) spec.defaultValue);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        const auto index = itemAt (e.position);
        if (index < 0)
            return;
        const auto& item = activeLayout().items[(std::size_t) index];
        if (isWaveShaperGraph (item))
            return;
        if (item.type == Item::display) // EQ graph: Q / bandwidth of the band under the mouse (or the selected one)
        {
            const auto* fx = current();
            if (fx == nullptr)
                return;
            auto b = peqview::bandAt (eqGraph (rectOf (item)), fx->params, e.position, eqRange);
            if (b < 0)
                b = eqSelected;
            if (b < 0)
                return;
            eqSelected = b;
            const auto q = fx->params[(std::size_t) peq::param (b, 3)];
            // wheel up = narrower; FL's width grows the other way
            const auto step = peq::flLaw (fx->params) ? -wheel.deltaY * 0.15f : wheel.deltaY * 0.15f;
            setParams ({ { peq::param (b, 3), q + step } });
            return;
        }
        const auto param = paramOf (item);
        if (param < 0)
            return;
        auto spec = specFor (item);
        if (item.type == Item::radio || item.type == Item::tabs)
        {
            const auto n = (int) plugui::optionLabels (item).size();
            const auto o = plugui::optionAt (item, valueOf (param)) + (wheel.deltaY > 0 ? (item.type == Item::radio ? -1 : 1) : (item.type == Item::radio ? 1 : -1));
            setValue (param, plugui::optionValue (item, juce::jlimit (0, n - 1, o)));
            return;
        }
        if (item.type == Item::stepper && item.steps > 1000 && wheel.deltaY != 0.0f) // a fine count (the hardware insert's delay in samples): one per notch, Shift ×10
        {
            const auto step = (e.mods.isShiftDown() ? 10.0f : 1.0f) / (float) item.steps;
            setValue (param, juce::jlimit (0.0f, 1.0f, valueOf (param) + (wheel.deltaY > 0.0f ? step : -step)));
            ui::flashValueTip (*this, rectOf (item).toNearestInt(), valueText (param, valueOf (param)));
            return;
        }
        if (const auto v = ui::wheelValue (valueOf (param), e, wheel, spec))
        {
            setValue (param, (float) *v);
            ui::flashValueTip (*this, rectOf (item).toNearestInt(), spec.text (*v));
        }
    }

    ui::Hint hintAt (juce::Point<int> position) override
    {
        const auto p = position.toFloat();
        switch (headerItemAt (p))
        {
            case 1: return { tr ("Options: presets, reset, bypass"), {}, false, true };
            case 2: return { tr ("Preset: click for the list of FL presets of this plugin"), presetName, false, true };
            case 3: return { tr ("Previous preset"), {}, false, false };
            case 4: return { tr ("Next preset"), {}, false, false };
            case 5: return { bar.hintFor (bar.partAt (p)), bar.presetLabel(), false, true };
            default: break;
        }
        const auto index = itemAt (p);
        if (index < 0)
            return {};
        const auto& item = activeLayout().items[(std::size_t) index];
        if (item.type == Item::display && juce::String (item.display) == "sgamount")
            return { tr ("Amount: drag up or down (Ctrl: fine)"), juce::String (soundgoodizerAmount() * 100.0f, 0) + "%", false, false };
        if (item.type == Item::display && juce::String (item.display) == "sgpresets")
            return { sgApprox ? tr ("Soundgoodizer preset A to D (approximated: FL Studio's preset files were not found)")
                              : tr ("Soundgoodizer preset A to D (read from your FL installation)"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "vumeter")
            return { tr ("VU meter: click to show the gain reduction, the output or the input level"), vuMode == 2 ? tr ("Output") : vuMode == 1 ? tr ("Input") : tr ("Gain reduction"), false, false };
        if (item.type == Item::display && juce::String (item.display) == "taptempo")
            return { tr ("Tap tempo: click in time (two taps or more) to set the delay"), tapBpm > 0.0 ? juce::String (tapBpm, 1) + " BPM" : juce::String(), false, false };
        if (item.type == Item::display && juce::String (item.display) == "iometer")
            return { tr ("Input and output level (peak, dBFS); the numbers hold the highest peak; OUT turns red when the output clips; click to reset"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "gcmeter")
            return { tr ("Gain change (dB): down = reduction, up = the expansion's boost below 1:1; the number holds the largest change; click to reset"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "limitpill")
            return { tr ("Output limiter: lit while it keeps the output under 0 dBFS"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "outmeter")
            return { tr ("Output level (peak, dBFS): the lamp lights when the output clips, the number holds the highest peak; click to reset"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "vusel")
            return { tr ("VU display: what the meter shows (input level, gain reduction, output level)"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "xy")
            return { tr ("Drag the point: both values at once"), {}, false, false };
        if (item.type == Item::display && juce::String (item.display) == "tunekeys") // round 147
            return { tuneKeysHint (rectOf (item), p), {}, false, false };
        if (isRhythmEdit (item)) // round 144
            return { tr ("Rhythm taps: drag up / down = level, sideways = time; double-click = add or remove a tap; right-click = patterns"), {}, false, false };
        if (isRhythmPatterns (item))
            return { tr ("Load a tap pattern (LYNX's own), or go back to even spacing"), {}, false, false };
        if (isWaveShaperGraph (item)) // round 165
            return { wsHint (item, p), {}, false, true };
        if (item.type == Item::display)
            return { tr ("Drag a handle (frequency and gain); mouse wheel = Q; right-click = type; double-click = 0 dB"), {}, false, true };
        if (hasStyle (item, plugui::pageTabs))
            return { tr ("Page: switch the controls shown"), {}, false, false };
        if (isMixItem (item) && captionArea (item).contains (p))
            return { tr ("MIX label: click to keep the Mix while presets change (mix lock)"), pluginheader::isMixLocked (kind) ? tr ("locked") : tr ("unlocked"), false, false };
        const auto spec = specFor (item);
        return ui::valueHint (spec, valueOf (paramOf (item)));
    }

    // --- presets ---------------------------------------------------------------------------------------

    juce::Array<juce::File> presetFiles() const
    {
        juce::Array<juce::File> all;
        const auto roots = findFlStudioRoots();
        if (roots.data.empty() || layout == &plugui::soundgoodizerLayout()) // Soundgoodizer: its presets A–D are on the page
            return all;
        const auto base = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin presets/Effects");
        for (const auto& plugin : flPluginsForEffect (kind))
        {
            auto files = base.getChildFile (juce::String::fromUTF8 (plugin.c_str())).findChildFiles (juce::File::findFiles, true, "*.fst");
            std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName().compareNatural (b.getFullPathName()) < 0; });
            all.addArray (files);
        }
        return all;
    }

    void loadPreset (const juce::File& f)
    {
        juce::MemoryBlock data;
        if (! f.loadFileAsData (data))
            return;
        const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
        auto fx = effectFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), engine.getSong().bpm);
        if (! fx || fx->kind != kind)
            return;
        presetName = f.getFileNameWithoutExtension();
        if (usesBar())
        {
            bar.beginGesture();
            bar.willEdit();
            fx->presetName = presetName.toStdString();
        }
        engine.editSong ([t = track, s = slot, fx = *fx] (Song& song) {
            auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) s];
            const auto enabled = target.enabled;
            const auto mix = target.mix;
            target = fx;
            target.enabled = enabled;
            target.mix = mix;
        });
        repaint();
    }

    void stepPreset (int delta)
    {
        const auto files = presetFiles();
        if (files.isEmpty())
            return;
        auto at = -1;
        for (int i = 0; i < files.size(); ++i)
            if (files[i].getFileNameWithoutExtension() == presetName)
                at = i;
        at = at < 0 ? (delta > 0 ? 0 : files.size() - 1) : (at + delta + files.size()) % files.size();
        loadPreset (files[at]);
    }

    // FL's factory presets of this plugin, in folders (the header's preset menu)
    void addFlPresets (juce::PopupMenu& presets)
    {
        const auto files = presetFiles();
        std::map<juce::String, juce::PopupMenu> folders;
        for (const auto& f : files)
            folders[f.getParentDirectory().getFileName()].addItem (f.getFileNameWithoutExtension(), true, f.getFileNameWithoutExtension() == presetName,
                                                                   [this, f] { loadPreset (f); });
        if (folders.size() == 1)
            presets = folders.begin()->second;
        else
            for (auto& [name, menu] : folders)
                presets.addSubMenu (name, menu);
    }

    void showOptionsMenu (bool presetsOnly)
    {
        juce::PopupMenu presets;
        const auto files = presetFiles();
        std::map<juce::String, juce::PopupMenu> folders;
        const auto roots = findFlStudioRoots();
        for (const auto& f : files)
        {
            const auto folder = f.getParentDirectory().getFileName();
            auto& menu = folders[folder];
            menu.addItem (f.getFileNameWithoutExtension(), true, f.getFileNameWithoutExtension() == presetName, [this, f] { loadPreset (f); });
        }
        if (folders.size() == 1)
            presets = folders.begin()->second;
        else
            for (auto& [name, menu] : folders)
                presets.addSubMenu (name, menu);

        if (presetsOnly)
        {
            if (presets.getNumItems() == 0)
                presets.addItem (tr ("No FL presets found for this plugin"), false, false, [] {});
            presets.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (presetBox().toNearestInt())));
            return;
        }

        juce::PopupMenu menu;
        if (presets.getNumItems() > 0)
            menu.addSubMenu (tr ("Presets"), presets);
        skin::addStyleMenu (menu, styleKey(), [safe = juce::Component::SafePointer<NativeEffectEditor> (this)] {
            if (safe != nullptr) safe->repaint();
        });
        menu.addItem (tr ("Reset to defaults"), [this] {
            const auto& info = effectInfo (kind);
            presetName = {};
            if (usesBar())
            {
                bar.beginGesture();
                bar.willEdit();
            }
            auto bytes = current() != nullptr ? current()->state : std::vector<std::uint8_t> {};
            if (stateBacked())
            {
                const auto& table = stateTable();
                for (int p = 0; p < (int) table.params.size(); ++p)
                    bytes = table.with (bytes, p, table.params[(std::size_t) p].defaultValue);
            }
            engine.editSong ([t = track, s = slot, info, bytes, state = stateBacked()] (Song& song) {
                auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) s];
                for (std::size_t i = 0; i < info.params.size() && i < fx.params.size(); ++i)
                    fx.params[i] = info.params[i].defaultValue;
                fx.presetName.clear();
                if (state)
                    fx.state = bytes;
            });
        });
        // switches that FL keeps out of the panel (in its own menus): on / off here
        if (! activeLayout().menuSwitches.empty())
        {
            menu.addSeparator();
            for (const auto* name : activeLayout().menuSwitches)
                if (const auto p = plugui::paramIndex (kind, name); p >= 0)
                {
                    const auto on = valueOf (p) >= 0.5f;
                    menu.addItem (utf8 (name), true, on, [this, p, on] { setValue (p, on ? 0.0f : 1.0f); });
                }
            menu.addSeparator();
        }
        const auto* fx = current();
        menu.addItem (tr ("Bypass"), true, fx != nullptr && ! fx->enabled, [this] {
            engine.editSong ([t = track, s = slot] (Song& song) {
                auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) s];
                target.enabled = ! target.enabled;
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withTargetScreenArea (localAreaToGlobal (menuButton().toNearestInt())));
    }

private:
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    void timerCallback() override
    {
        if (! isShowing()) return; // a hidden editor costs nothing
        learnScaleFromMidi();
        if (kind == EffectKind::hardwareInsert)
            hwPingTick();
        // an idle display (silent track: meters unchanged for longer than the longest history) is not redrawn
        float meters = 0.0f;
        for (int k = 0; k < 6; ++k) meters += liveMeter (k) * (float) (k + 1);
        quietFrames = std::abs (meters - lastMeters) > 1e-6f ? 0 : quietFrames + 1;
        lastMeters = meters;
        if (quietFrames > 300 && ! demoDisplays) return;
        bool history = false;
        for (const auto& item : activeLayout().items)
        {
            if (item.type == Item::display && item.display != nullptr && (std::strcmp (item.display, "pitchgraph") == 0 || std::strcmp (item.display, "pitchtrace") == 0
                                                                       || (std::strcmp (item.display, "pitchring") == 0 && ! pitchTraced())) && visible (item))
            {
                if (std::strcmp (item.display, "pitchring") == 0 || std::strcmp (item.display, "pitchtrace") == 0)
                {
                    // round 128: what is sung, for AUTO KEY (time per pitch class, fading over ~30 s)
                    for (auto& x : keyHistogram)
                        x *= 0.999;
                    if (const auto sungNote = liveMeter (0) * 127.0f; sungNote > 0.5f)
                        keyHistogram[(std::size_t) (((int) std::lround (sungNote) % 12 + 12) % 12)] += 1.0;
                    if (autoKeyFlash > 0)
                        --autoKeyFlash;
                    if (tuneSendFlash > 0 && --tuneSendFlash == 0) // round 150
                        repaint();
                }
                // LYNX Tune's pitch history (30 points a second)
                pitchHistory.push_back ({ liveMeter (0) * 127.0f, liveMeter (1) * 127.0f, (liveMeter (2) - 0.5f) * 4.0f });
                if (pitchHistory.size() > pitchHistoryLength)
                    pitchHistory.erase (pitchHistory.begin(), pitchHistory.begin() + (std::ptrdiff_t) (pitchHistory.size() - pitchHistoryLength));
                repaint (rectOf (item).getSmallestIntegerContainer());
            }
            if (item.type == Item::display && item.display != nullptr && (std::strcmp (item.display, "grmeter") == 0 || std::strcmp (item.display, "grleds") == 0 || std::strcmp (item.display, "vumeter") == 0 || std::strcmp (item.display, "iometer") == 0 || std::strcmp (item.display, "outmeter") == 0 || std::strcmp (item.display, "limitpill") == 0 || std::strcmp (item.display, "gcmeter") == 0 || std::strcmp (item.display, "pitchring") == 0 || dynui::isLive (item.display)) && visible (item))
            {
                history = history || std::strcmp (item.display, "grhistory") == 0;
                repaint (rectOf (item).getSmallestIntegerContainer());
            }
        }
        if (history)
            dynHistory.push (liveMeter (0), liveMeter (2), liveMeter (3));
    }

    AudioEngine& engine;
    int track, slot;
    EffectKind kind = EffectKind::eq;
    const plugui::Layout* layout = nullptr;
    int page = 0;
    int hoverItem = -1, hoverHeader = 0, dragItem = -1;
    juce::Colour accent { skin::colour::crimson }; // the plugin family's accent (ui/PluginSkin.h)
    skin::Animator anim;                            // hover fades (runs only while something moves)
    int quietFrames = 1000; // frames since the live meters last moved (the displays start at rest)
    float lastMeters = 0.0f;
    ui::ValueDrag drag;
    juce::String presetName;
    pluginheader::PresetBar bar { engine, track, slot }; // undo / redo, A / B, presets (round 129)
    int eqSelected = -1;
    bool eqDragging = false, xyDragging = false, sgDragging = false;
    struct PitchPoint { float sung, target, shift; }; // MIDI note numbers (0 = none) and the applied correction in semitones
    static constexpr std::size_t pitchHistoryLength = 240; // 8 s
    std::vector<PitchPoint> pitchHistory;
    // LYNX Tune (round 128): HOLD, the keyboard's mode / latch, AUTO KEY's histogram
    bool tuneHold = false, tuneRemove = true, tuneLatch = true;
    int tuneHeldMap = -1;          // round 152: the key-map parameter held while the key is down (latch off)
    float tuneHeldMapValue = 0.0f;
    PitchPoint holdPoint { 0.0f, 0.0f, 0.0f };
    int tuneHeld = -1, autoKeyFlash = 0;
    std::pair<float, float> tuneHeldValues { 1.0f, 0.0f };
    float tuneHeldDir = 0.0f; // round 147
    std::array<double, 12> keyHistogram {};
    // is the scrolling pitch trace on the current page? (it then feeds the history; else the ring does)
    bool pitchTraced() const
    {
        for (const auto& item : activeLayout().items)
            if (item.type == Item::display && item.display != nullptr && std::strcmp (item.display, "pitchtrace") == 0 && visible (item))
                return true;
        return false;
    }
    float displayCentre = 0.0f;
    float sgStart = 0.0f;
    std::array<std::vector<std::uint8_t>, 4> sgPresets {}; // the installation's preset files A–D (read on demand)
    bool sgApprox = false; // a Soundgoodizer approximation container (no FL preset files) shown on the Soundgoodizer page
    juce::Point<float> eqDragOffset;
    dynui::History dynHistory; // the dynamics suite's scrolling gain-reduction graph
    int crossoverDrag = -1;    // the crossover parameter being dragged on a band display
    float vuNeedle = 0.0f;     // the VU display's needle (dB below 0 VU, with inertia)
    std::array<skin::Ballistics, 2> ioMeters {};
    bool ioClip = false;         // the "iometer" display's clip lamp (round 135w)
    skin::Ballistics outMeter;   // the "outmeter" display (round 135w): level, highest peak since the last click, clip lamp
    float outMax = 0.0f;
    bool outClip = false;
    juce::uint32 lastTap = 0;          // the tap pad (round 129)
    std::vector<double> tapIntervals;
    double tapBpm = 0.0; // the "iometer" display: input and output (round 129)
    int vuMode = 0;            // what the VU shows: 0 gain reduction, 1 input level, 2 output level (0 VU = −18 dBFS)
    juce::Point<float> mouseAt { -1.0f, -1.0f };
};

// --- the window ------------------------------------------------------------------------------------

class NativeEffectWindow final : public juce::DocumentWindow
{
public:
    NativeEffectWindow (AudioEngine& engine, int track, int slot, const juce::String& title)
        : DocumentWindow (title, theme::colour::bg1, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          gone (engine, track, slot, [this] { closeButtonPressed(); })
    {
        setUsingNativeTitleBar (true);
        auto* editor = new NativeEffectEditor (engine, track, slot, 1.25f, 0);
        const auto design = editor->designSize();
        setContentOwned (editor, true);
        setResizable (true, false);
        // keep FL's proportions while resizing (the presets bar has a fixed height)
        if (auto* c = getConstrainer())
        {
            c->setMinimumSize (design.x / 2, design.y / 2);
            c->setMaximumSize (design.x * 3, design.y * 3 + 200);
        }
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
        skin::playOpenAnimation (*editor, editor->editorStyle()); // round 126: fade / scale in
    }

    void closeButtonPressed() override;

private:
    ui::SlotGoneWatcher gone;
};

std::map<std::pair<int, int>, std::unique_ptr<NativeEffectWindow>>& windows()
{
    static std::map<std::pair<int, int>, std::unique_ptr<NativeEffectWindow>> open;
    return open;
}

void NativeEffectWindow::closeButtonPressed()
{
    for (auto& [key, window] : windows())
        if (window.get() == this)
        {
            juce::MessageManager::callAsync ([k = key] { windows().erase (k); });
            return;
        }
}
} // namespace

void setNativeEditorDemo (bool on)
{
    demoDisplays = on;
}

std::unique_ptr<juce::Component> makeNativeEffectEditor (AudioEngine& engine, int track, int slot, float scale, int page)
{
    return std::make_unique<NativeEffectEditor> (engine, track, slot, scale, page);
}

namespace NativeEffectWindows
{
bool canOpen (EffectKind kind)
{
    return kind != EffectKind::plugin && kind != EffectKind::parallel;
}

bool canOpen (const EffectSlot& slot)
{
    return canOpen (slot.kind) || isSoundgoodizerApprox (slot);
}

void open (AudioEngine& engine, int track, int slot)
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return;

    const auto& fxSlot = mixer[(std::size_t) track].effects[(std::size_t) slot];
    const auto kind = fxSlot.kind;
    if (kind == EffectKind::patcher) // its node graph editor (REA_rodada141)
    {
        PatcherWindows::openSlot (engine, track, slot);
        return;
    }
    if (VisualWindows::handles (kind)) // Wave Candy / Visualizer: displays fed by the slot's tap (round 164c)
    {
        VisualWindows::open (engine, track, slot);
        return;
    }
    if (FlExtraWindows::handles (kind)) // Big Clock, Tuner, HTML NoteBook (round 165g)
    {
        FlExtraWindows::open (engine, track, slot);
        return;
    }
    if (! canOpen (fxSlot))
        return;
    if (kind == EffectKind::formulaController) // the formula, its knobs and a monitor (REA round 133)
    {
        FormulaControllerWindows::open (engine, track, slot);
        return;
    }
    if (Controller164Windows::handles (kind)) // the X-Y / X-Y-Z pad, the Control Surface's panel (round 164)
    {
        Controller164Windows::open (engine, track, slot);
        return;
    }
    if (NewtoneWindows::handles (kind)) // load a sample, edit its notes / warp markers, hear it (round 142)
    {
        NewtoneWindows::open (engine, track, slot);
        return;
    }
    // (the Parametric EQ opens here, in its layout editor: ParametricEqWindows::open forwards to this function, so
    // forwarding back to it was an endless loop — every way of opening the Parametric EQ froze LYNX; round 157a)
    if (kind == EffectKind::facetEq) // its own editor (analyser, nodes, dynamics; REA round 103)
    {
        FacetEqWindows::open (engine, track, slot);
        return;
    }
    // round 136: Ember in its Classic voicing opens the layout editor (the Classic controls); the Pro editor is Ember's own
    const bool emberClassic = kind == EffectKind::ember && fxSlot.params.size() > (std::size_t) mixsuite::ember::voicing
                              && fxSlot.params[(std::size_t) mixsuite::ember::voicing] > 0.5f;
    if (ProEditorWindows::handles (kind) && ! emberClassic) // display-first editors of the dynamics / saturation engines (REA round 114)
    {
        ProEditorWindows::open (engine, track, slot);
        return;
    }

    auto& open = windows();
    const auto key = std::make_pair (track, slot);

    if (auto it = open.find (key); it != open.end())
    {
        if (it->second->getContentComponent() != nullptr && static_cast<NativeEffectEditor*> (it->second->getContentComponent())->current() != nullptr)
        {
            it->second->toFront (true);
            return;
        }
        open.erase (it); // the slot now holds another effect
    }

    const auto name = track == 0 ? tr ("Master") : juce::String::fromUTF8 (mixer[(std::size_t) track].name.c_str());
    const auto title = utf8 (isSoundgoodizerApprox (fxSlot) ? "Soundgoodizer" : effectInfo (kind).name) + " - " + (name.isNotEmpty() ? name : tr ("Insert {0}", { juce::String (track) })) + " ("
                       + tr ("slot {0}", { juce::String (slot + 1) }) + ")";
    open[key] = std::make_unique<NativeEffectWindow> (engine, track, slot, title);
}

void closeAll()
{
    windows().clear();
    FormulaControllerWindows::closeAll();
    Controller164Windows::closeAll();
    NewtoneWindows::closeAll();
    VisualWindows::closeAll();
    FlExtraWindows::closeAll();
    FacetEqWindows::closeAll();
    ProEditorWindows::closeAll();
}
} // namespace NativeEffectWindows
} // namespace lynx
