#include "ui/WindowOwner.h"
#include "ui/PluginWindow.h"
#include "ui/PluginPicker.h"

#include <algorithm>
#include <map>

#include "core/FlWrapper.h"
#include "core/LastTweaked.h"
#include "engine/PluginSandbox.h"
#include "engine/Text.h"
#include "platform/CrashGuard.h"
#include "ui/I18n.h"
#include "platform/Paths.h"
#include "ui/Theme.h"

namespace lynx
{
namespace
{
void toggleWrapperOption (AudioEngine& engine, const std::function<PluginRef* (Song&)>& find, juce::AudioPluginInstance* instance,
                          bool PluginRef::*option, bool recreates);

// FL's plugin wrapper settings page (REA rounds 146a / 149), in LYNX's style with the tabs and groups of FL's wrapper
// form (Fruity Wrapper's DFM [DATA], FL 2026's page [EXEC]): Settings (Info with the latency, MIDI ports), Processing
// (Options) and Troubleshooting (Compatibility: the fixed buffers; Location). Options that re-create the plugin keep
// its state and close this window (open it again from the channel or the slot).
class WrapperSettingsPage final : public juce::Component, private juce::ChangeListener
{
public:
    WrapperSettingsPage (AudioEngine& e, std::function<PluginRef* (Song&)> f, juce::AudioPluginInstance& p, bool channel)
        : engine (e), find (std::move (f)), plugin (p)
    {
        const char* tabNames[] = { TR_NOOP ("Settings"), TR_NOOP ("Processing"), TR_NOOP ("Troubleshooting") };
        for (int t = 0; t < 3; ++t)
        {
            auto* b = tabs.add (new juce::TextButton (tr (tabNames[t])));
            b->setClickingTogglesState (false); // LYNX's tab look (as the channel window's): the shown one filled
            b->onClick = [this, t] { showTab (t); };
            addAndMakeVisible (b);
        }
        const auto& options = wrapperOptions();
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            auto* t = toggles.add (new juce::ToggleButton (tr (options[i].label)));
            t->setTooltip (tr (options[i].hint));
            t->setColour (juce::ToggleButton::textColourId, theme::colour::textPrimary);
            t->setColour (juce::ToggleButton::tickColourId, theme::colour::accent);
            t->onClick = [this, i] {
                auto* en = &engine;
                auto fn = find;
                auto* inst = &plugin;
                const auto& o = wrapperOptions()[i];
                juce::MessageManager::callAsync ([en, fn, inst, member = o.member, re = o.recreates] { toggleWrapperOption (*en, fn, inst, member, re); });
            };
            addChildComponent (t);
        }
        for (auto* box : { &inPort, &outPort })
        {
            box->addItem (juce::String::fromUTF8 ("\xe2\x80\x94"), 1);
            for (int i = 0; i < 256; ++i)
                box->addItem (juce::String (i), i + 2);
            addChildComponent (box);
        }
        for (int st = 1; st <= 48; ++st)
            bendRange.addItem (juce::String (st), st);
        bendRange.setTooltip (tr ("The pitch bend range sent to the plugin (semitones)"));
        bendRange.onChange = [this] {
            engine.editSong ([fn = find, v = bendRange.getSelectedId()] (Song& song) {
                if (auto* ref = fn (song); ref != nullptr && v > 0)
                    ref->bendRange = v;
            });
        };
        addChildComponent (bendRange);
        inPort.setTooltip (tr ("MIDI input port number"));
        inPort.onChange = [this] { setPort (inPort.getSelectedId() - 2, false); };
        // FL's Output port: the notes the plugin sends out play the plugins on that input port (instruments in the
        // Channel rack that send MIDI)
        outPort.setTooltip (tr ("MIDI output port number: the notes this plugin sends play the plugins whose input port it is"));
        outPort.onChange = [this] { setPort (outPort.getSelectedId() - 2, true); };
        outPort.setEnabled (channel && plugin.producesMidi());
        // FL's Info › Latency: what the plugin reports, or set by hand (menu: Reset, Set in ms / samples / beats)
        latency.setTooltip (tr ("Latency used by the delay compensation: what the plugin reports, or a value set by hand"));
        latency.onClick = [this] { showLatencyMenu(); };
        addChildComponent (latency);
        locate.setButtonText (tr ("Locate"));
        locate.setTooltip (tr ("Show the plugin's file in the file manager"));
        locate.onClick = [this] { juce::File (plugin.getPluginDescription().fileOrIdentifier).revealToUser(); };
        addChildComponent (locate);
        engine.addChangeListener (this);
        refresh();
        showTab (0);
        setSize (520, preferredHeight());
    }

    ~WrapperSettingsPage() override { engine.removeChangeListener (this); }

    int preferredHeight() const { return 410; }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        g.fillAll (colour::bg1);
        g.setColour (colour::textSecondary);
        g.setFont (font (type::caption));
        g.drawText (tr ("version {0}", { plugin.getPluginDescription().version }), getWidth() - 200, 8, 184, tabHeight, juce::Justification::centredRight);
        for (const auto& [area, title] : groups)
        {
            g.setColour (colour::bg2);
            g.fillRoundedRectangle (area.toFloat(), radius::md);
            g.setColour (colour::textSecondary);
            g.setFont (font (type::caption, true));
            g.drawText (caps (title), area.withHeight (headerHeight).reduced (10, 0), juce::Justification::centredLeft);
        }
        g.setFont (font (type::body));
        for (const auto& [area, text, primary] : labels)
        {
            g.setColour (primary ? colour::textPrimary : colour::textSecondary);
            g.drawFittedText (text, area, juce::Justification::centredLeft, 1);
        }
    }

    void resized() override
    {
        groups.clear();
        labels.clear();
        auto area = getLocalBounds().reduced (10, 8);
        auto bar = area.removeFromTop (tabHeight);
        for (auto* t : tabs)
            t->setBounds (bar.removeFromLeft (juce::jmax (80, t->getBestWidthForHeight (tabHeight) + 16)));
        area.removeFromTop (8);
        for (auto* c : getChildren())
            if (! tabs.contains (dynamic_cast<juce::TextButton*> (c)))
                c->setVisible (false);

        auto group = [this] (juce::Rectangle<int>& from, int height, const juce::String& title) {
            auto r = from.removeFromTop (height);
            from.removeFromTop (8);
            groups.push_back ({ r, title });
            return r.withTrimmedTop (headerHeight).reduced (10, 4);
        };
        auto label = [this] (juce::Rectangle<int> r, const juce::String& text, bool primary = false) { labels.push_back ({ r, text, primary }); };
        auto show = [] (juce::Component& c, juce::Rectangle<int> r) {
            c.setBounds (r);
            c.setVisible (true);
        };

        if (tab == 0)
        {
            const auto d = plugin.getPluginDescription();
            auto info = group (area, headerHeight + 6 * rowHeight + 8, tr ("Info"));
            const std::pair<juce::String, juce::String> lines[] = {
                { tr ("Name"), d.name + (d.manufacturerName.isNotEmpty() ? " (" + d.manufacturerName + ")" : juce::String()) },
                { tr ("Type"), d.pluginFormatName + (d.isInstrument ? "  " + tr ("instrument") : "  " + tr ("effect")) },
                { tr ("Category"), d.category },
                { tr ("Inputs / outputs"), juce::String (plugin.getTotalNumInputChannels()) + " / " + juce::String (plugin.getTotalNumOutputChannels()) },
                { tr ("Parameters"), juce::String (plugin.getParameters().size()) },
            };
            for (const auto& [name, value] : lines)
            {
                auto row = info.removeFromTop (rowHeight);
                label (row.removeFromLeft (110), name);
                label (row, value, true);
            }
            auto row = info.removeFromTop (rowHeight);
            label (row.removeFromLeft (110), tr ("Latency"));
            show (latency, row.removeFromLeft (260).reduced (0, 2));

            int midiRows = 0;
            for (const auto& o : wrapperOptions())
                midiRows += juce::String (o.section) == "MIDI" ? 1 : 0;
            auto midi = group (area, headerHeight + (1 + midiRows) * rowHeight + 8, tr ("MIDI"));
            auto line = midi.removeFromTop (rowHeight);
            label (line.removeFromLeft (80), tr ("Input port"));
            show (inPort, line.removeFromLeft (70).reduced (0, 2));
            line.removeFromLeft (30);
            label (line.removeFromLeft (90), tr ("Output port"));
            show (outPort, line.removeFromLeft (70).reduced (0, 2));
            // FL's MIDI options (round 151): release velocity, the pitch bend range RPN (with its value), MOD X as
            // polyphonic aftertouch, All Notes Off on stop
            for (std::size_t i = 0; i < wrapperOptions().size(); ++i)
                if (juce::String (wrapperOptions()[i].section) == "MIDI")
                {
                    auto row = midi.removeFromTop (rowHeight);
                    if (wrapperOptions()[i].member == &PluginRef::sendBendRange)
                        show (bendRange, row.removeFromRight (70).reduced (0, 2));
                    show (*toggles[(int) i], row);
                }
        }
        else
        {
            const juce::String section = tab == 1 ? "Processing" : "Compatibility";
            int rows = 0;
            for (const auto& o : wrapperOptions())
                rows += section == o.section ? 1 : 0;
            auto box = group (area, headerHeight + rows * rowHeight + 8, tr (tab == 1 ? "Options" : "Compatibility"));
            for (std::size_t i = 0; i < wrapperOptions().size(); ++i)
                if (section == wrapperOptions()[i].section)
                    show (*toggles[(int) i], box.removeFromTop (rowHeight).withTrimmedLeft (wrapperOptions()[i].needsFixed ? 22 : 0));
            if (tab == 2)
            {
                auto where = group (area, headerHeight + rowHeight + 8, tr ("Location"));
                show (locate, where.removeFromRight (90).withHeight (rowHeight).reduced (0, 2));
                label (where.removeFromLeft (50).withHeight (rowHeight), tr ("Path"));
                label (where.withHeight (rowHeight), plugin.getPluginDescription().fileOrIdentifier, true);
            }
        }
        repaint();
    }

private:
    static constexpr int tabHeight = 24, headerHeight = 24, rowHeight = 24;

    void showTab (int t)
    {
        tab = t;
        for (int k = 0; k < tabs.size(); ++k)
            tabs[k]->setToggleState (k == t, juce::dontSendNotification);
        resized();
    }

    void setPort (int value, bool output)
    {
        engine.editSong ([fn = find, value, output] (Song& song) {
            if (auto* ref = fn (song))
                (output ? ref->midiOutPort : ref->midiInPort) = value;
        });
    }

    double rate() const { return plugin.getSampleRate() > 0 ? plugin.getSampleRate() : 44100.0; }

    void setLatencyMs (float ms)
    {
        engine.editSong ([fn = find, ms] (Song& song) {
            if (auto* ref = fn (song))
                ref->latencyOverrideMs = ms;
        });
    }

    void showLatencyMenu()
    {
        // FL's latency menu [EXEC]: Reset, Set in ms, Set in samples, Set in beats (beats at the song's tempo)
        juce::PopupMenu menu;
        menu.addItem (tr ("Reset"), [this] { setLatencyMs (-1.0f); });
        auto ask = [this] (int unit) {
            auto* ref = find (const_cast<Song&> (engine.getSong()));
            const auto bpm = juce::jmax (10.0, (double) engine.getSong().bpm);
            const auto ms = ref != nullptr && ref->latencyOverrideMs >= 0.0f ? (double) ref->latencyOverrideMs
                                                                              : plugin.getLatencySamples() * 1000.0 / rate();
            const auto current = unit == 0 ? ms : unit == 1 ? ms * rate() / 1000.0 : ms * bpm / 60000.0;
            const char* titles[] = { TR_NOOP ("Latency in ms"), TR_NOOP ("Latency in samples"), TR_NOOP ("Latency in beats") };
            auto* window = new juce::AlertWindow (tr ("Latency"), tr (titles[unit]), juce::MessageBoxIconType::NoIcon);
            window->addTextEditor ("value", juce::String (current, unit == 1 ? 0 : 3));
            window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            juce::Component::SafePointer<WrapperSettingsPage> safe (this);
            window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, unit, bpm] (int result) {
                if (result != 1 || safe == nullptr)
                    return;
                const auto v = juce::jmax (0.0, window->getTextEditorContents ("value").getDoubleValue());
                const auto ms = unit == 0 ? v : unit == 1 ? v * 1000.0 / safe->rate() : v * 60000.0 / bpm;
                safe->setLatencyMs ((float) juce::jlimit (0.0, 2000.0, ms));
            }), true);
        };
        menu.addItem (tr ("Set in ms..."), [ask] { ask (0); });
        menu.addItem (tr ("Set in samples..."), [ask] { ask (1); });
        menu.addItem (tr ("Set in beats..."), [ask] { ask (2); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&latency));
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }

    void refresh()
    {
        auto* ref = find (const_cast<Song&> (engine.getSong()));
        if (ref == nullptr)
            return;
        const auto& options = wrapperOptions();
        for (std::size_t i = 0; i < options.size(); ++i)
        {
            toggles[(int) i]->setToggleState (ref->*(options[i].member), juce::dontSendNotification);
            toggles[(int) i]->setEnabled (! options[i].needsFixed || ref->fixedBuffers);
        }
        // with Options › "Run plugins in a separate process" every plugin already runs bridged
        for (std::size_t i = 0; i < options.size(); ++i)
            if (options[i].member == &PluginRef::separateProcess && pluginSandboxEnabled())
                toggles[(int) i]->setToggleState (true, juce::dontSendNotification), toggles[(int) i]->setEnabled (false);
        inPort.setSelectedId (ref->midiInPort + 2, juce::dontSendNotification);
        bendRange.setSelectedId (ref->bendRange, juce::dontSendNotification);
        bendRange.setEnabled (ref->sendBendRange);
        outPort.setSelectedId (ref->midiOutPort + 2, juce::dontSendNotification);
        const auto reported = plugin.getLatencySamples();
        const auto text = ref->latencyOverrideMs >= 0.0f
                              ? tr ("{0} ms (set by hand)", { juce::String (ref->latencyOverrideMs, 2) })
                              : (reported > 0 ? tr ("{0} samples ({1} ms)", { juce::String (reported), juce::String (reported * 1000.0 / rate(), 1) })
                                              : tr ("none"));
        latency.setButtonText (text + juce::String::fromUTF8 ("  \xe2\x96\xbe"));
    }

    struct Label
    {
        juce::Rectangle<int> area;
        juce::String text;
        bool primary = false;
    };

    AudioEngine& engine;
    std::function<PluginRef* (Song&)> find;
    juce::AudioPluginInstance& plugin;
    juce::OwnedArray<juce::TextButton> tabs;
    juce::OwnedArray<juce::ToggleButton> toggles;
    juce::ComboBox inPort, outPort, bendRange;
    juce::TextButton latency, locate;
    int tab = 0;
    std::vector<std::pair<juce::Rectangle<int>, juce::String>> groups;
    std::vector<Label> labels;
};

// The window's content with FL's wrapper header: the plugin's editor, or the wrapper settings page (gear).
class WrappedEditor final : public juce::Component
{
public:
    WrappedEditor (juce::AudioProcessorEditor* e, AudioEngine& engine, std::function<PluginRef* (Song&)> find, juce::AudioPluginInstance& plugin,
                   bool channel)
        : editor (e), page (engine, std::move (find), plugin, channel)
    {
        for (auto* b : { &pluginTab, &settingsTab })
        {
            b->setClickingTogglesState (false); // LYNX's tab look (as the channel window's): the shown one filled
            addAndMakeVisible (b);
        }
        pluginTab.setTooltip (tr ("The plugin's own editor"));
        settingsTab.setTooltip (tr ("Plugin wrapper settings: processing, bridging and MIDI options"));
        pluginTab.onClick = [this] { show (false); };
        settingsTab.onClick = [this] { show (true); };
        addAndMakeVisible (editor.get());
        addChildComponent (page);
        pluginTab.setToggleState (true, juce::dontSendNotification);
        fitEditor();
    }

    ~WrappedEditor() override
    {
        editor = nullptr; // the editor goes first (the plugin may be released right after the window)
    }

    bool isEditorResizable() const { return editor != nullptr && editor->isResizable(); }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colour::bg0);
        g.fillRect (getLocalBounds().removeFromTop (headerHeight));
    }

    void resized() override
    {
        auto area = getLocalBounds();
        auto header = area.removeFromTop (headerHeight).reduced (4, 3);
        pluginTab.setBounds (header.removeFromLeft (80));
        header.removeFromLeft (4);
        settingsTab.setBounds (header.removeFromLeft (130));
        page.setBounds (area);
        if (editor != nullptr && ! settings)
        {
            const juce::ScopedValueSetter<bool> guard (laying, true);
            if (editor->isResizable())
                editor->setBounds (area);
            else
                editor->setTopLeftPosition (area.getPosition());
        }
    }

    void childBoundsChanged (juce::Component* child) override
    {
        if (child == editor.get() && ! laying && ! settings)
            fitEditor();
    }

private:
    static constexpr int headerHeight = 30;

    void fitEditor()
    {
        if (editor != nullptr)
            setSize (juce::jmax (220, editor->getWidth()), editor->getHeight() + headerHeight);
    }

    void show (bool showSettings)
    {
        settings = showSettings;
        pluginTab.setToggleState (! settings, juce::dontSendNotification);
        settingsTab.setToggleState (settings, juce::dontSendNotification);
        page.setVisible (settings);
        if (editor != nullptr)
            editor->setVisible (! settings);
        if (settings)
            setSize (juce::jmax (getWidth(), page.getWidth()), juce::jmax (getHeight(), page.preferredHeight() + headerHeight));
        else
            fitEditor();
        resized();
    }

    std::unique_ptr<juce::AudioProcessorEditor> editor;
    WrapperSettingsPage page;
    juce::TextButton pluginTab { tr ("Plugin") }, settingsTab { juce::String::fromUTF8 ("\xe2\x9a\x99 ") + tr ("Wrapper settings") };
    bool settings = false, laying = false;
};

class PluginWindow final : public juce::DocumentWindow, private juce::AudioProcessorListener, private juce::Timer
{
public:
    PluginWindow (std::shared_ptr<void> keepAlive, juce::AudioPluginInstance& p, const juce::String& title, PluginWindows::WrapperContext context)
        : DocumentWindow (title, theme::colour::bg1, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
          owner (std::move (keepAlive)),
          plugin (p)
    {
        setUsingNativeTitleBar (true);
        useClassicRendering();

        crashguard::note (("opening the editor of " + plugin.getName()).toStdString()); // session breadcrumb (round 111)
        juce::AudioProcessorEditor* editor = plugin.createEditorIfNeeded();
        if (editor == nullptr)
            editor = new juce::GenericAudioProcessorEditor (plugin); // plugin without a UI
        crashguard::note (("editor open: " + plugin.getName()).toStdString());

        const auto resizable = editor->isResizable();
        if (context.engine != nullptr && context.find) // FL's wrapper header and settings page (round 146a)
            setContentOwned (new WrappedEditor (editor, *context.engine, std::move (context.find), plugin, context.channel), true);
        else
            setContentOwned (editor, true);
        setResizable (resizable, false);
        centreWithSize (getWidth(), getHeight());
        setVisible (true);
        keepAboveMainWindow (*this);
        crashguard::note (("editor shown: " + plugin.getName()).toStdString()); // (its OpenGL / GPU setup runs when shown)
        plugin.addListener (this); // the editor's gestures: Last tweaked, automation recording (REA round 143)
        startTimerHz (10);         // its moves without gestures (round 162d)
    }

    // round 157a: the window that holds a third-party editor draws LYNX's few pixels with GDI, not Direct2D (JUCE 8's
    // default). A plugin's OpenGL child window then sits in a classic redirected top-level window, as in every other
    // host — a Direct2D / DXGI swap-chain parent is where Intel's OpenGL driver crashed in SetPixelFormat (her
    // Auto-Tune Pro editor, 2026-10-25). (Re-applied whenever the native window is re-created.)
    void addToDesktop (int styleFlags, void* nativeWindowToAttachTo) override
    {
        DocumentWindow::addToDesktop (styleFlags, nativeWindowToAttachTo);
        useClassicRendering();
    }

    void useClassicRendering()
    {
        getProperties().set ("lynxGdi", true);
       #if JUCE_WINDOWS
        if (auto* peer = getPeer())
            if (const auto engines = peer->getAvailableRenderingEngines(); engines.size() > 1 && peer->getCurrentRenderingEngine() != 0)
                peer->setCurrentRenderingEngine (0); // "Software Renderer" (GDI)
       #endif
    }

    ~PluginWindow() override
    {
        crashguard::note (("closing the editor of " + plugin.getName()).toStdString());
        stopTimer();
        plugin.removeListener (this);
        // the UI must be destroyed before the plugin (which may die together with `owner`)
        clearContentComponent();
    }

    void closeButtonPressed() override;

    juce::AudioPluginInstance* key() const noexcept { return &plugin; }

private:
    // round 162d (issue #8): many plugins move their parameters without begin/end gestures, so their knobs never
    // became "Last tweaked". A change made on the UI thread while this window is active or under the mouse, when it
    // is the only parameter that moved since the last tick, counts as a tweak (core/LastTweaked.h; automation and
    // links run on the audio thread, preset loads move many parameters at once).
    void audioProcessorParameterChanged (juce::AudioProcessor*, int index, float) override
    {
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstance()->isThisTheMessageThread())
            changes.changed (index);
    }
    void timerCallback() override
    {
        const auto mouse = juce::Desktop::getInstance().getMainMouseSource().getScreenPosition().roundToInt();
        const auto active = isActiveWindow() || (isShowing() && getScreenBounds().contains (mouse));
        if (const auto moved = changes.tick (active); moved >= 0 && PluginWindows::onGesture)
        {
            PluginWindows::onGesture (plugin, moved, true);
            PluginWindows::onGesture (plugin, moved, false);
        }
    }
    void audioProcessorChanged (juce::AudioProcessor*, const ChangeDetails&) override {}
    void audioProcessorParameterChangeGestureBegin (juce::AudioProcessor*, int index) override { gesture (index, true); }
    void audioProcessorParameterChangeGestureEnd (juce::AudioProcessor*, int index) override { gesture (index, false); }
    void gesture (int index, bool starting)
    {
        if (juce::MessageManager::getInstanceWithoutCreating() != nullptr && juce::MessageManager::getInstance()->isThisTheMessageThread())
            changes.gesture (index, starting);
        auto* p = &plugin;
        juce::MessageManager::callAsync ([p, index, starting] {
            if (PluginWindows::onGesture)
                PluginWindows::onGesture (*p, index, starting);
        });
    }

    std::shared_ptr<void> owner;
    juce::AudioPluginInstance& plugin;
    lasttweak::ChangeFilter changes;
};

} // namespace

std::function<void (const juce::AudioPluginInstance&, int, bool)> PluginWindows::onGesture;

namespace
{
std::map<juce::AudioPluginInstance*, std::unique_ptr<PluginWindow>>& windows()
{
    static std::map<juce::AudioPluginInstance*, std::unique_ptr<PluginWindow>> open;
    return open;
}

void PluginWindow::closeButtonPressed()
{
    const auto* k = key();
    // close after leaving this callback (the window deletes itself)
    juce::MessageManager::callAsync ([k] { windows().erase (const_cast<juce::AudioPluginInstance*> (k)); });
}

class ScanThread final : public juce::ThreadWithProgressWindow
{
public:
    ScanThread (AudioEngine& e, std::function<void (int)> done)
        : juce::ThreadWithProgressWindow (tr ("Scanning for plugins ({0})...", { platform::pluginFormatNames() }), true, true),
          engine (e),
          onDone (std::move (done))
    {
    }

    // called on the UI thread when the scan finishes (or is cancelled)
    void threadComplete (bool) override
    {
        if (onDone)
            onDone (found);

        juce::MessageManager::callAsync ([this] { delete this; });
    }

    void run() override
    {
        found = engine.getPluginHost().scan (engine.getPluginHost().pluginFolders(), [this] (float progress, const juce::String& file) {
            setProgress (progress);
            setStatusMessage (juce::File (file).getFileName());
            return ! threadShouldExit();
        });
    }

    int found = 0;

private:
    AudioEngine& engine;
    std::function<void (int)> onDone;
};
} // namespace

namespace PluginWindows
{
void open (std::shared_ptr<void> owner, juce::AudioPluginInstance& plugin, const juce::String& title, WrapperContext context)
{
    auto& open = windows();

    if (auto it = open.find (&plugin); it != open.end())
    {
        it->second->toFront (true);
        return;
    }

    open[&plugin] = std::make_unique<PluginWindow> (std::move (owner), plugin, title, std::move (context));
}

void closeAll()
{
    windows().clear();
}

bool isOpen (const juce::AudioPluginInstance& plugin)
{
    return windows().count (const_cast<juce::AudioPluginInstance*> (&plugin)) > 0;
}

void close (const juce::AudioPluginInstance& plugin)
{
    windows().erase (const_cast<juce::AudioPluginInstance*> (&plugin));
}
} // namespace PluginWindows

void scanPluginsWithProgress (AudioEngine& engine, std::function<void (int)> onDone)
{
    (new ScanThread (engine, std::move (onDone)))->launchThread(); // deletes itself when done
}

namespace
{
void toggleWrapperOption (AudioEngine& engine, const std::function<PluginRef* (Song&)>& find, juce::AudioPluginInstance* instance,
                          bool PluginRef::*option, bool recreates)
{
    if (recreates)
    {
        if (instance != nullptr && PluginWindows::isOpen (*instance))
            PluginWindows::close (*instance);
        engine.capturePluginStates(); // the new instance starts from the current settings
    }
    engine.editSong ([find, option] (Song& song) {
        if (auto* ref = find (song))
            ref->*option = ! (ref->*option);
    });
}
} // namespace

juce::PopupMenu pluginProcessingMenu (AudioEngine& engine, const PluginRef& current, juce::AudioPluginInstance* instance,
                                      std::function<PluginRef* (Song&)> find)
{
    auto toggle = [&engine, find, instance] (bool PluginRef::*option) {
        if (instance != nullptr)
            PluginWindows::close (*instance);
        engine.capturePluginStates(); // the new instance starts from the current settings
        engine.editSong ([find, option] (Song& song) {
            if (auto* ref = find (song))
                ref->*option = ! (ref->*option);
        });
    };

    juce::PopupMenu menu;
    menu.addItem (tr ("Use fixed size buffers (adds latency)"), true, current.fixedBuffers, [toggle] { toggle (&PluginRef::fixedBuffers); });
    // FL's options of fixed buffers (REA round 134): 2 ms blocks, or the whole device buffer
    menu.addItem (tr ("Process maximum size buffers (more latency, only when necessary)"), current.fixedBuffers, current.fixedBuffers && current.maxSizeBuffers,
                  [toggle] { toggle (&PluginRef::maxSizeBuffers); });
    // FL's "Use maximum buffer size from host" (REA round 139): the plugin is prepared for the host's largest block
    // instead of the fixed block (for plugins that need it to size their buffers)
    menu.addItem (tr ("Use maximum buffer size from host"), current.fixedBuffers, current.fixedBuffers && current.hostMaxBuffer,
                  [toggle] { toggle (&PluginRef::hostMaxBuffer); });
    // with Options › "Run plugins in a separate process" on, every plugin already runs in one
    const auto all = pluginSandboxEnabled();
    menu.addItem (all ? tr ("Run in a separate process (on for all plugins in Options)") : tr ("Run in a separate process"), ! all,
                  all || current.separateProcess, [toggle] { toggle (&PluginRef::separateProcess); });
    // round 157a: this plugin always in its own process, in every project (e.g. after its editor crashed LYNX)
    if (! all && ! current.name.empty())
    {
        const auto name = juce::String::fromUTF8 (current.name.c_str());
        const auto always = pluginAlwaysSeparate (name);
        menu.addItem (tr ("Always run {0} in a separate process", { name }), true, always, [&engine, find, instance, name, always] {
            if (instance != nullptr)
                PluginWindows::close (*instance);
            engine.capturePluginStates();
            setPluginAlwaysSeparate (name, ! always);
            // this instance follows at once (a changed option re-creates it); others when their project opens
            engine.editSong ([find, on = ! always] (Song& song) { if (auto* ref = find (song)) ref->separateProcess = on; });
        });
    }
    // FL's "Allow threaded processing" (on by default): off keeps this plugin on the audio thread itself, for plugins
    // that misbehave when called from several threads. Nothing is re-created.
    menu.addItem (tr ("Allow threaded processing"), true, current.threaded, [&engine, find] {
        engine.editSong ([find] (Song& song) {
            if (auto* ref = find (song))
                ref->threaded = ! ref->threaded;
        });
    });

    // FL's wrapper "Allow smart disable" (on by default): off = never skipped in silence, even with the slot's
    // "Smart disable" on (for plugins that misbehave when they are not called)
    menu.addItem (tr ("Allow smart disable"), true, current.allowSmartDisable, [&engine, find] {
        engine.editSong ([find] (Song& song) {
            if (auto* ref = find (song))
                ref->allowSmartDisable = ! ref->allowSmartDisable;
        });
    });

    // FL's wrapper "MIDI input port": the MIDI Out channels on this port play the plugin (REA_rodada133 §4)
    juce::PopupMenu ports;
    auto setPort = [&engine, find] (int port) {
        engine.editSong ([find, port] (Song& song) {
            if (auto* ref = find (song))
                ref->midiInPort = port;
        });
    };
    ports.addItem (tr ("None"), true, current.midiInPort < 0, [setPort] { setPort (-1); });
    for (int port = 0; port < 32; ++port)
        ports.addItem (tr ("Port {0}", { juce::String (port) }), true, current.midiInPort == port, [setPort, port] { setPort (port); });
    menu.addSubMenu (current.midiInPort >= 0 ? tr ("MIDI input port ({0})", { juce::String (current.midiInPort) }) : tr ("MIDI input port"), ports);
    return menu;
}

void addPluginItems (juce::PopupMenu& menu, AudioEngine& engine, bool instruments, std::function<void (PluginRef)> onChosen, juce::Component* owner)
{
    // FL's way (round 151): the favourites right here, every other plugin in the picker ("More plugins...", FL's
    // list of every generator / effect with its categories, search and favourite stars). The old menu listed all
    // of them: hundreds of items in four columns on a PC with many plugins.
    auto types = engine.getPluginHost().getKnownPlugins().getTypes();
    std::sort (types.begin(), types.end(), [] (const auto& a, const auto& b) { return a.name.compareIgnoreCase (b.name) < 0; });
    const auto& favourites = engine.getPluginHost().favourites();
    int kind = 0;
    for (const auto& type : types)
    {
        if (type.isInstrument != instruments)
            continue;
        ++kind;
        PluginRef ref;
        ref.id = type.createIdentifierString().toStdString();
        ref.name = type.name.toStdString();
        if (favourites.count (ref.id) == 0)
            continue;
        menu.addItem (juce::String::fromUTF8 ("\xe2\x98\x85 ") + type.name + " (" + type.manufacturerName + ")  [" + type.pluginFormatName + "]",
                      [ref, onChosen] { onChosen (ref); });
    }
    if (menu.getNumItems() > 0)
        menu.addSeparator();

    if (kind == 0)
        menu.addItem (tr ("(no plugins found; use \"Scan for plugins\")"), false, false, [] {});
    else
        menu.addItem (instruments ? tr ("More plugins... ({0} instruments)", { juce::String (kind) }) : tr ("More plugins... ({0} effects)", { juce::String (kind) }),
                      [&engine, instruments, onChosen, safe = juce::Component::SafePointer<juce::Component> (owner), hasOwner = owner != nullptr] {
                          showPluginPicker (engine, instruments, [onChosen, safe, hasOwner] (PluginRef ref) {
                              if (! hasOwner || safe != nullptr)
                                  onChosen (ref);
                          });
                      });

    menu.addSeparator();
    menu.addItem (tr ("Scan for plugins ({0})...", { platform::pluginFormatNames() }), [&engine] { engine.getPluginHost().startBackgroundScan(); });
}
} // namespace lynx
