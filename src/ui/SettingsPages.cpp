#include "ui/SettingsPages.h"
#include "platform/Paths.h"
#include "platform/FileTypes.h"

#include "core/ToolbarLayout.h"
#include "engine/AudioEngine.h"
#include "engine/PluginHost.h"
#include "platform/LinuxRealtime.h"
#include "ui/AutomationUi.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"

namespace lynx
{
using namespace settingsui;

juce::File autosaveFolder()
{
    return lynx::platform::settingsFile ("autosave");
}

namespace
{
constexpr int labelWidth = 190;
constexpr int rowHeight = 26;
constexpr int rowGap = 5; // round 163: tighter (was 8)
constexpr int sectionHeight = 24;

void styleCombo (juce::ComboBox& box)
{
    box.setColour (juce::ComboBox::outlineColourId, theme::colour::borderStrong);
}

void styleToggle (juce::ToggleButton& t, const juce::String& text)
{
    t.setButtonText (text);
}

// Lays out rows top to bottom inside a page of a given width.
struct Rows
{
    juce::Rectangle<int> area;
    int y = 0;

    explicit Rows (int width) : area (20, 14, width - 40, 100000), y (14) {}

    void section (juce::Component& title, bool first = false)
    {
        if (! first)
            y += 10;
        title.setBounds (area.getX(), y, area.getWidth(), sectionHeight);
        y += sectionHeight + rowGap;
    }

    // label + control (control width 0 = to the right edge)
    juce::Rectangle<int> row (juce::Label& label, int controlWidth = 260, int height = rowHeight)
    {
        label.setBounds (area.getX(), y, labelWidth, height);
        auto r = juce::Rectangle<int> (area.getX() + labelWidth, y, controlWidth > 0 ? controlWidth : area.getWidth() - labelWidth, height);
        y += height + rowGap;
        return r;
    }

    juce::Rectangle<int> full (int height = rowHeight)
    {
        auto r = juce::Rectangle<int> (area.getX(), y, area.getWidth(), height);
        y += height + rowGap;
        return r;
    }

    juce::Rectangle<int> indented (int height = rowHeight)
    {
        auto r = juce::Rectangle<int> (area.getX() + labelWidth, y, area.getWidth() - labelWidth, height);
        y += height + rowGap;
        return r;
    }

    int bottom() const { return y + 14; }
};

// ================================================================================================================
// General
// ================================================================================================================
class GeneralPage final : public ScrollingPage
{
public:
    GeneralPage (AudioEngine& e, SettingsHooks h, bool keep) : engine (e), hooks (std::move (h)), persistent (keep)
    {
        if (hooks.getGeneral)
            options = hooks.getGeneral();
        else
            options.language = "en";

        for (auto* c : std::initializer_list<juce::Component*> { &languageTitle, &displayTitle, &startupTitle, &editingTitle, &backupTitle,
                                                                 &pluginsTitle, &languageLabel, &languageBox, &languageNote, &scaleLabel, &scaleBox, &menuLabel, &menuBox,
                                                                 &wavesLabel, &wavesBox, &stereoWaves, &pluginStyleLabel, &pluginStyleBox, &reducedEffects, &gpuRendering, &splash, &startupSound, &scanAtStart, &menuEntry,
                                                                 &typingPiano, &keepEditors, &rememberSeek, &editInLabel, &editInBox, &undoLabel,
                                                                 &undoBox, &autosaveLabel, &autosaveBox, &keepLabel, &keepBox, &openBackups, &unusedTakes,
                                                                 &sandbox, &sandboxNote, &substitutes, &substitutesNote, &managePlugins, &manageShortcuts, &shortcutsNote,
                                                                 &reportsTitle, &crashLabel, &crashBox, &crashNote })
            add (*c);

        styleLabel (languageLabel, tr ("Language"));
        styleNote (languageNote, tr ("A new language is applied when LYNX restarts."));
        int id = 1;
        for (const auto& l : i18n::languages())
        {
            languageBox.addItem (juce::String::fromUTF8 (l.nativeName), id);
            if (options.language == l.id)
                languageBox.setSelectedId (id, juce::dontSendNotification);
            ++id;
        }
        languageBox.onChange = [this] {
            const auto index = languageBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (index, (int) i18n::languages().size()))
                change ([&] { options.language = i18n::languages()[(std::size_t) index].id; });
        };

        styleLabel (scaleLabel, tr ("Interface size"));
        for (auto percent : toolbarlayout::uiScales)
            scaleBox.addItem (juce::String (percent) + "%", percent);
        scaleBox.setSelectedId (options.uiScale, juce::dontSendNotification);
        scaleBox.onChange = [this] { change ([&] { options.uiScale = scaleBox.getSelectedId(); }); };

        styleLabel (menuLabel, tr ("Menu density"));
        menuBox.addItem (tr ("Compact"), 1);
        menuBox.addItem (tr ("Comfortable"), 2);
        menuBox.setSelectedId (options.compactMenus ? 1 : 2, juce::dontSendNotification);
        menuBox.onChange = [this] { change ([&] { options.compactMenus = menuBox.getSelectedId() == 1; }); };

        styleLabel (wavesLabel, tr ("Waveforms"));
        wavesBox.addItem (tr ("Clean (the clip colour)"), 1);
        wavesBox.addItem (tr ("Spectral colours (warm = bass, cool = treble)"), 2);
        wavesBox.setSelectedId (options.waveSpectral ? 2 : 1, juce::dontSendNotification);
        wavesBox.onChange = [this] { change ([&] { options.waveSpectral = wavesBox.getSelectedId() == 2; }); };
        toggle (stereoWaves, tr ("Stereo (left and right channels apart)"), options.waveStereo, [this] (bool on) { options.waveStereo = on; });
        styleLabel (pluginStyleLabel, tr ("Plugin style"));
        pluginStyleBox.addItem (tr ("Automatic (each plugin its own style)"), 1);
        for (int i = 0; i < skin::styleCount; ++i)
            pluginStyleBox.addItem (tr ("{0} for every plugin", { tr (skin::styleName ((skin::Style) i)) }), i + 2);
        pluginStyleBox.setSelectedId (juce::jlimit (-1, skin::styleCount - 1, options.pluginStyle) + 2, juce::dontSendNotification);
        pluginStyleBox.onChange = [this] { change ([&] { options.pluginStyle = pluginStyleBox.getSelectedId() - 2; }); };
        toggle (reducedEffects, tr ("Reduced effects in plugin editors (no glows or animations, for slower graphics cards)"), options.reducedEffects,
                [this] (bool on) { options.reducedEffects = on; });
        toggle (gpuRendering, tr ("Draw with the GPU (Direct2D). Turn off if LYNX crashes in the graphics driver or the screen flickers"),
                options.gpuRendering, [this] (bool on) { options.gpuRendering = on; });
       #if ! JUCE_WINDOWS
        gpuRendering.setVisible (false);
       #endif

        toggle (splash, tr ("Show the splash screen at startup"), options.splash, [this] (bool on) { options.splash = on; });
        toggle (startupSound, tr ("Play the startup sound"), options.startupSound, [this] (bool on) { options.startupSound = on; });
        toggle (scanAtStart, tr ("Look for new plugins in the background at every start"), options.scanAtStart,
                [this] (bool on) { options.scanAtStart = on; });
        // the AppImage on Linux (round 161): its menu entry and the .lynx file type, in the user's ~/.local/share
        toggle (menuEntry, tr ("Show LYNX in the applications menu and open .lynx files with it"), options.menuEntry,
                [this] (bool on) { options.menuEntry = on; });
        menuEntry.setVisible (runningAsAppImage());

        toggle (typingPiano, tr ("Typing keyboard as a piano"), options.typingPiano, [this] (bool on) { options.typingPiano = on; });
        toggle (keepEditors, tr ("Keep channel editors open (a channel button click doesn't close the previous editor)"), options.keepEditorsOpen,
                [this] (bool on) { options.keepEditorsOpen = on; });
        toggle (rememberSeek, tr ("Remember seek time: Stop goes back to the position clicked on the Playlist ruler"), options.rememberSeek,
                [this] (bool on) { options.rememberSeek = on; });

        styleLabel (editInLabel, tr ("Double-click on a pattern clip opens"));
        const char* choices[] = { TR_NOOP ("Piano Roll when it has piano-roll notes, else Channel Rack"), TR_NOOP ("Channel Rack"), TR_NOOP ("Piano Roll") };
        for (int i = 0; i < 3; ++i)
            editInBox.addItem (tr (choices[i]), i + 1);
        editInBox.setSelectedId (options.editPatternsIn + 1, juce::dontSendNotification);
        editInBox.onChange = [this] { change ([&] { options.editPatternsIn = editInBox.getSelectedId() - 1; }); };

        styleLabel (undoLabel, tr ("Maximum undo levels"));
        for (auto n : { 25, 50, 100, 200, 500, 1000 })
            undoBox.addItem (juce::String (n), n);
        selectNearest (undoBox, options.undoLevels);
        undoBox.onChange = [this] { change ([&] { options.undoLevels = undoBox.getSelectedId(); }); };

        styleLabel (autosaveLabel, tr ("Save a backup copy"));
        for (auto minutes : prefs::autosaveIntervals())
            autosaveBox.addItem (minutes == 0 ? tr ("Never")
                                 : minutes == 1 ? tr ("Every minute")
                                                : tr ("Every {0} minutes", { juce::String (minutes) }),
                                 minutes + 1);
        autosaveBox.setSelectedId (prefs::clampAutosaveMinutes (options.autosaveMinutes) + 1, juce::dontSendNotification);
        autosaveBox.onChange = [this] {
            change ([&] { options.autosaveMinutes = autosaveBox.getSelectedId() - 1; });
            keepBox.setEnabled (options.autosaveMinutes > 0);
        };

        styleLabel (keepLabel, tr ("Backups kept per project"));
        for (auto n : { 1, 3, 5, 10, 20, 50 })
            keepBox.addItem (juce::String (n), n);
        selectNearest (keepBox, options.autosaveKeep);
        keepBox.setEnabled (options.autosaveMinutes > 0);
        keepBox.onChange = [this] { change ([&] { options.autosaveKeep = keepBox.getSelectedId(); }); };
        openBackups.onClick = [] {
            autosaveFolder().createDirectory();
            autosaveFolder().startAsProcess();
        };

        toggle (unusedTakes, tr ("When a project closes, offer to delete the recordings it no longer uses"), options.askUnusedRecordings,
                [this] (bool on) { options.askUnusedRecordings = on; });

        toggle (sandbox, tr ("Run plugins in a separate process (protects LYNX from crashes)"), options.pluginSandbox,
                [this] (bool on) { options.pluginSandbox = on; });
        styleNote (sandboxNote, tr ("Applies to plugins opened from now on; reopen the project to move the others."));
        toggle (substitutes, tr ("Substitute missing plugins with LYNX equivalents"), options.pluginSubstitutes,
                [this] (bool on) { options.pluginSubstitutes = on; });
        styleNote (substitutesNote, tr ("A project keeps the original plugin and uses it again once it is installed."));
        managePlugins.onClick = [this] {
            if (hooks.managePlugins)
                hooks.managePlugins();
        };
        manageShortcuts.onClick = [this] {
            if (hooks.manageShortcuts)
                hooks.manageShortcuts();
        };
        styleNote (shortcutsNote, tr ("See, search and change every keyboard shortcut."));

        styleLabel (crashLabel, tr ("Crash reports"));
        crashBox.addItem (tr ("Ask after a crash or freeze"), 1);
        crashBox.addItem (tr ("Always send"), 2);
        crashBox.addItem (tr ("Never send"), 3);
        crashBox.setSelectedId (juce::jlimit (0, 2, options.crashReports) + 1, juce::dontSendNotification);
        crashBox.onChange = [this] { change ([&] { options.crashReports = crashBox.getSelectedId() - 1; }); };
        styleNote (crashNote, tr ("A report holds LYNX's version, your system and audio device, the crash or freeze file and the last steps, without "
                                  "your name, file names or projects. It goes to lynx-share.com only with your consent (Help > Report a problem... "
                                  "sends one by hand)."));

        for (auto* b : { &languageBox, &scaleBox, &menuBox, &wavesBox, &pluginStyleBox, &editInBox, &undoBox, &autosaveBox, &keepBox, &crashBox })
            styleCombo (*b);
        juce::ignoreUnused (engine, persistent);
    }

private:
    int layout (int width) override
    {
        Rows r (width);
        r.section (languageTitle, true);
        languageBox.setBounds (r.row (languageLabel, 220));
        languageNote.setBounds (r.indented (16).translated (0, -3));
        scaleBox.setBounds (r.row (scaleLabel, 120));
        menuBox.setBounds (r.row (menuLabel, 160));
        wavesBox.setBounds (r.row (wavesLabel, 0));
        stereoWaves.setBounds (r.indented (24));
        pluginStyleBox.setBounds (r.row (pluginStyleLabel, 300));
        reducedEffects.setBounds (r.full (24));
       #if JUCE_WINDOWS
        gpuRendering.setBounds (r.full (24));
       #endif

        r.section (startupTitle);
        for (auto* t : { &splash, &startupSound, &scanAtStart })
            t->setBounds (r.full (24));
        if (menuEntry.isVisible())
            menuEntry.setBounds (r.full (24));

        r.section (editingTitle);
        for (auto* t : { &typingPiano, &keepEditors, &rememberSeek })
            t->setBounds (r.full (24));
        editInBox.setBounds (r.row (editInLabel, 0));
        undoBox.setBounds (r.row (undoLabel, 120));

        r.section (backupTitle);
        {
            auto row = r.row (autosaveLabel, 0);
            autosaveBox.setBounds (row.removeFromLeft (220));
            row.removeFromLeft (8);
            openBackups.setBounds (row.removeFromLeft (juce::jmin (row.getWidth(), 170)));
        }
        keepBox.setBounds (r.row (keepLabel, 120));
        unusedTakes.setBounds (r.full (24));

        r.section (pluginsTitle);
        sandbox.setBounds (r.full (24));
        sandboxNote.setBounds (r.full (16).translated (0, -3));
        substitutes.setBounds (r.full (24));
        substitutesNote.setBounds (r.full (16).translated (0, -3));
        {
            auto row = r.full();
            managePlugins.setBounds (row.removeFromLeft (180));
            row.removeFromLeft (8);
            manageShortcuts.setBounds (row.removeFromLeft (180));
            row.removeFromLeft (12);
            shortcutsNote.setBounds (row.withTrimmedTop (6));
        }

        r.section (reportsTitle);
        crashBox.setBounds (r.row (crashLabel, 260));
        crashNote.setBounds (r.full (46).translated (0, -3));
        displayTitle.setVisible (false);
        return r.bottom();
    }

    void toggle (juce::ToggleButton& t, const juce::String& text, bool value, std::function<void (bool)> set)
    {
        styleToggle (t, text);
        t.setToggleState (value, juce::dontSendNotification);
        t.onClick = [this, &t, set = std::move (set)] { change ([&] { set (t.getToggleState()); }); };
    }

    static void selectNearest (juce::ComboBox& box, int value)
    {
        auto best = box.getItemId (0);
        for (int i = 0; i < box.getNumItems(); ++i)
            if (std::abs (box.getItemId (i) - value) < std::abs (best - value))
                best = box.getItemId (i);
        box.setSelectedId (best, juce::dontSendNotification);
    }

    void change (const std::function<void()>& fn)
    {
        fn();
        if (hooks.setGeneral)
            hooks.setGeneral (options);
    }

    AudioEngine& engine;
    SettingsHooks hooks;
    bool persistent;
    GeneralOptions options;
    SectionTitle languageTitle { tr ("Language and display") }, displayTitle, startupTitle { tr ("Startup") }, editingTitle { tr ("Editing") },
        backupTitle { tr ("Backup") }, pluginsTitle { tr ("Plugins and shortcuts") }, reportsTitle { tr ("Crash reports") };
    juce::Label languageLabel, languageNote, scaleLabel, menuLabel, wavesLabel, pluginStyleLabel, editInLabel, undoLabel, autosaveLabel, keepLabel, sandboxNote, substitutesNote, shortcutsNote, crashLabel, crashNote;
    juce::ComboBox crashBox, languageBox, scaleBox, menuBox, wavesBox, pluginStyleBox, editInBox, undoBox, autosaveBox, keepBox;
    juce::ToggleButton stereoWaves, reducedEffects, gpuRendering, splash, startupSound, scanAtStart, menuEntry, typingPiano, keepEditors, rememberSeek, sandbox, substitutes, unusedTakes;
    juce::TextButton openBackups { tr ("Open backup folder") }, managePlugins { tr ("Plugin manager...") }, manageShortcuts { tr ("Manage shortcuts...") };
};

// ================================================================================================================
// Audio
// ================================================================================================================
class AudioPage final : public ScrollingPage,
                        private juce::ChangeListener,
                        private juce::Timer
{
public:
    AudioPage (AudioEngine& e) : engine (e), devices (e.getDeviceManager())
    {
        for (auto* c : std::initializer_list<juce::Component*> { &deviceTitle, &statusTitle, &recordTitle, &driverLabel, &driverBox, &driverNote, &outputLabel,
                                                                 &outputBox, &inputLabel, &inputBox, &rateLabel, &rateBox, &bufferLabel, &bufferBox,
                                                                 &bufferNote, &panelButton, &testButton, &status, &load, &outputChannels, &realtime, &recordInput, &recordNote, &inputChannels,
                                                                 &formatLabel, &formatBox, &takeNames, &latencyTitle, &latencyText, &pingOutLabel, &pingOutBox,
                                                                 &pingInLabel, &pingInBox, &measureButton, &pingNote, &useMeasured, &offsetLabel, &offsetSlider, &offsetNote })
            add (*c);
        setupRecordingControls();

        styleLabel (driverLabel, tr ("Driver"));
        styleLabel (outputLabel, tr ("Output device"));
        styleLabel (inputLabel, tr ("Input device"));
        styleLabel (rateLabel, tr ("Sample rate"));
        styleLabel (bufferLabel, tr ("Buffer size"));
        styleNote (driverNote, {});
        styleNote (bufferNote, tr ("Smaller buffers answer faster when you play; larger ones give the CPU more room (fewer clicks)."));
        styleNote (realtime, {});
        for (auto* l : { &status, &load })
        {
            styleLabel (*l, {}, false);
            l->setJustificationType (juce::Justification::centredLeft);
        }
        for (auto* b : { &driverBox, &outputBox, &inputBox, &rateBox, &bufferBox })
        {
            styleCombo (*b);
            b->setTextWhenNoChoicesAvailable (tr ("(open an output device first)"));
            b->setTextWhenNothingSelected (tr ("(choose)"));
        }

        driverBox.onChange = [this] {
            if (updating)
                return;
            auto& types = devices.getAvailableDeviceTypes();
            const auto index = driverBox.getSelectedId() - 1;
            if (juce::isPositiveAndBelow (index, types.size()))
                devices.setCurrentAudioDeviceType (types[index]->getTypeName(), true);
            refresh();
        };
        outputBox.onChange = [this] {
            apply ([this] (juce::AudioDeviceManager::AudioDeviceSetup& s) { s.outputDeviceName = outputBox.getText(); });
        };
        inputBox.onChange = [this] {
            if (updating)
                return;
            if (inputBox.getSelectedId() <= 1)
                apply ([] (juce::AudioDeviceManager::AudioDeviceSetup& s) {
                    s.inputDeviceName = {};
                    s.useDefaultInputChannels = false;
                    s.inputChannels.clear();
                });
            else
            {
                // every channel of the device opens: each mixer insert picks its own (pair or single channel)
                const auto result = engine.openAudioInput (inputBox.getText());
                error = result.issue == AudioEngine::InputIssue::failed ? result.detail : juce::String();
                refresh();
            }
        };
        rateBox.onChange = [this] {
            apply ([this] (juce::AudioDeviceManager::AudioDeviceSetup& s) { s.sampleRate = (double) rateBox.getSelectedId(); });
        };
        bufferBox.onChange = [this] {
            apply ([this] (juce::AudioDeviceManager::AudioDeviceSetup& s) { s.bufferSize = bufferBox.getSelectedId(); });
        };
        panelButton.onClick = [this] {
            if (auto* device = devices.getCurrentAudioDevice(); device != nullptr && device->hasControlPanel())
            {
                // the driver's own window (ASIO): the device is reopened afterwards with what was set there
                devices.closeAudioDevice();
                device->showControlPanel();
                devices.restartLastAudioDevice();
                refresh();
            }
        };
        testButton.onClick = [this] { devices.playTestSound(); };

        styleToggle (recordInput, tr ("Record input audio"));
        styleNote (recordNote, tr ("Choose each mixer insert's input in the Mixer (the input box above its effect slots), arm it, then Rec + Play. "
                                   "This switch records the first two input channels into a new insert when nothing is armed."));
        recordInput.onClick = [this] { engine.setAudioRecordingArmed (recordInput.getToggleState()); };

        devices.addChangeListener (this);
        refresh();
        startTimerHz (2);
    }

    ~AudioPage() override { devices.removeChangeListener (this); }

private:
    int layout (int width) override
    {
        Rows r (width);
        r.section (deviceTitle, true);
        driverBox.setBounds (r.row (driverLabel, 320));
        driverNote.setBounds (r.indented (46).translated (0, -3));
        outputBox.setBounds (r.row (outputLabel, 0));
        inputBox.setBounds (r.row (inputLabel, 0));
        rateBox.setBounds (r.row (rateLabel, 160));
        bufferBox.setBounds (r.row (bufferLabel, 260));
        bufferNote.setBounds (r.indented (18).translated (0, -3));
        {
            auto row = r.indented();
            panelButton.setBounds (row.removeFromLeft (170));
            row.removeFromLeft (8);
            testButton.setBounds (row.removeFromLeft (150));
        }
        r.section (statusTitle);
        status.setBounds (r.full (22));
        load.setBounds (r.full (22));
        outputChannels.setBounds (r.full (22));
       #if JUCE_LINUX
        realtime.setBounds (r.full (realtimeDenied ? 50 : 22));
       #endif
        r.section (recordTitle);
        inputChannels.setBounds (r.full (34));
        recordInput.setBounds (r.full (24));
        recordNote.setBounds (r.full (32).translated (0, -3));
        formatBox.setBounds (r.row (formatLabel, 320));
        takeNames.setBounds (r.full (24));
        r.section (latencyTitle);
        latencyText.setBounds (r.full (50));
        pingOutBox.setBounds (r.row (pingOutLabel, 260));
        pingInBox.setBounds (r.row (pingInLabel, 260));
        measureButton.setBounds (r.indented().withWidth (240));
        pingNote.setBounds (r.full (48).translated (0, -2));
        useMeasured.setBounds (r.full (24));
        offsetSlider.setBounds (r.row (offsetLabel, 200));
        offsetNote.setBounds (r.full (32).translated (0, -3));
        return r.bottom();
    }

    void apply (const std::function<void (juce::AudioDeviceManager::AudioDeviceSetup&)>& change)
    {
        if (updating)
            return;
        auto setup = devices.getAudioDeviceSetup();
        change (setup);
        error = devices.setAudioDeviceSetup (setup, true);
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        auto* type = devices.getCurrentDeviceTypeObject();
        auto* device = devices.getCurrentAudioDevice();
        const auto setup = devices.getAudioDeviceSetup();

        driverBox.clear (juce::dontSendNotification);
        auto& types = devices.getAvailableDeviceTypes();
        for (int i = 0; i < types.size(); ++i)
        {
            driverBox.addItem (driverDisplayName (types[i]->getTypeName()), i + 1);
            if (types[i] == type)
                driverBox.setSelectedId (i + 1, juce::dontSendNotification);
        }

        outputBox.clear (juce::dontSendNotification);
        inputBox.clear (juce::dontSendNotification);
        inputBox.addItem (tr ("(none)"), 1);
        inputBox.setSelectedId (1, juce::dontSendNotification);
        if (type != nullptr)
        {
            if (type->getDeviceNames (false).isEmpty())
                type->scanForDevices(); // (a driver type not opened yet this session)
            const auto outs = type->getDeviceNames (false);
            for (int i = 0; i < outs.size(); ++i)
            {
                outputBox.addItem (outs[i], i + 1);
                if (outs[i] == setup.outputDeviceName)
                    outputBox.setSelectedId (i + 1, juce::dontSendNotification);
            }
            if (type->hasSeparateInputsAndOutputs())
            {
                const auto ins = type->getDeviceNames (true);
                for (int i = 0; i < ins.size(); ++i)
                {
                    inputBox.addItem (ins[i], i + 2);
                    if (ins[i] == setup.inputDeviceName && ! setup.inputChannels.isZero())
                        inputBox.setSelectedId (i + 2, juce::dontSendNotification);
                }
            }
            else if (setup.outputDeviceName.isNotEmpty())
            {
                // ASIO: one device does both
                inputBox.addItem (setup.outputDeviceName, 2);
                if (! setup.inputChannels.isZero())
                    inputBox.setSelectedId (2, juce::dontSendNotification);
            }
        }

        rateBox.clear (juce::dontSendNotification);
        bufferBox.clear (juce::dontSendNotification);
        if (device != nullptr)
        {
            const auto rate = device->getCurrentSampleRate();
            for (auto r : device->getAvailableSampleRates())
                rateBox.addItem (juce::String ((int) r) + " Hz", (int) r);
            rateBox.setSelectedId ((int) rate, juce::dontSendNotification);

            const auto kind = prefs::driverKind (device->getTypeName().toStdString());
            std::vector<int> offered;
            for (auto b : device->getAvailableBufferSizes())
                offered.push_back (b);
            for (auto b : prefs::usableBufferSizes (kind, offered, device->getDefaultBufferSize()))
                bufferBox.addItem (tr ("{0} samples ({1} ms)", { juce::String (b), juce::String (prefs::bufferLatencyMs (b, rate), 1) }), b);
            bufferBox.setSelectedId (device->getCurrentBufferSizeSamples(), juce::dontSendNotification);
        }
        for (auto* b : { &outputBox, &inputBox, &rateBox, &bufferBox })
            b->setEnabled (b->getNumItems() > 0);
        driverNote.setText (driverExplanation (type != nullptr ? type->getTypeName() : juce::String(), device), juce::dontSendNotification);
        panelButton.setEnabled (device != nullptr && device->hasControlPanel());
        testButton.setEnabled (device != nullptr);
        recordInput.setToggleState (engine.isAudioRecordingArmed(), juce::dontSendNotification);
        refreshRecording();
        {
            // what the mixer inserts can take their input from
            const auto names = engine.getInputChannelNames();
            juce::StringArray list;
            for (const auto& n : names)
                list.add (juce::String::fromUTF8 (n.c_str()));
            styleNote (inputChannels, names.empty() ? tr ("Input channels: none open. Choose an input device above to record a microphone or an instrument.")
                                                    : tr ("Input channels ({0}): {1}", { juce::String ((int) names.size()), list.joinIntoString (", ") }));
        }
        {
            // the device's outputs by name (PipeWire / JACK / ASIO name their channels)
            juce::StringArray outs;
            if (device != nullptr)
            {
                const auto names = device->getOutputChannelNames();
                const auto active = device->getActiveOutputChannels();
                for (int c = 0; c < names.size(); ++c)
                    outs.add (names[c] + (active[c] ? juce::String() : " (" + tr ("off") + ")"));
            }
            styleNote (outputChannels, outs.isEmpty() ? juce::String()
                                                      : tr ("Output channels ({0}): {1}", { juce::String (outs.size()), outs.joinIntoString (", ") }));
        }
        updateStatus();
    }

    void updateStatus()
    {
        auto* device = devices.getCurrentAudioDevice();
        if (device == nullptr)
        {
            status.setText (error.isNotEmpty() ? error : tr ("No audio device is open: choose an output device."), juce::dontSendNotification);
            status.setColour (juce::Label::textColourId, theme::colour::warning);
            load.setText ({}, juce::dontSendNotification);
            return;
        }
        const auto rate = device->getCurrentSampleRate();
        const auto latency = prefs::bufferLatencyMs (device->getOutputLatencyInSamples() + device->getCurrentBufferSizeSamples(), rate);
        status.setText (tr ("{0} is running: {1} Hz, output latency {2} ms",
                            { device->getName(), juce::String ((int) rate), juce::String (latency, 1) }),
                        juce::dontSendNotification);
        status.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        const auto xruns = device->getXRunCount();
        const auto rt = engine.getRtStats().snapshot();
        const auto dot = juce::String::fromUTF8 ("   \xc2\xb7   ");
        load.setText (tr ("Audio CPU load: {0}%", { juce::String (juce::roundToInt (devices.getCpuUsage() * 100.0)) })
                          + (xruns > 0 ? dot + tr ("{0} dropouts (buffer underruns) so far", { juce::String (xruns) }) : juce::String())
                          + (rt.overBudget > 0 ? dot + tr ("{0} blocks over the deadline (max {1}%)", { juce::String ((juce::int64) rt.overBudget), juce::String (juce::roundToInt (rt.maxLoad * 100.0)) })
                                               : juce::String()),
                      juce::dontSendNotification);
        load.setColour (juce::Label::textColourId, xruns > 0 || rt.overBudget > 0 ? theme::colour::warning : theme::colour::textSecondary);
       #if JUCE_LINUX
        updateRealtime();
       #endif
    }

    // Linux: whether the audio thread got real-time priority (round 161), and how to get it when it did not
    void updateRealtime()
    {
        const auto state = lynx::rt::status();
        const auto denied = state == lynx::rt::Status::denied;
        if (state == lynx::rt::Status::unknown)
            realtime.setText ({}, juce::dontSendNotification);
        else if (denied)
            realtime.setText (tr ("Audio thread: normal priority, so the sound breaks up more easily under load. To allow real-time priority, "
                                  "install rtkit (most desktops have it) or add yourself to the \"audio\" group with an rtprio limit "
                                  "(/etc/security/limits.d), then log in again. LYNX never needs root."),
                              juce::dontSendNotification);
        else
            realtime.setText (tr ("Audio thread: real-time priority ({0})", { lynx::rt::describe() }), juce::dontSendNotification);
        realtime.setColour (juce::Label::textColourId, denied ? theme::colour::warning : theme::colour::textSecondary);
        if (denied != realtimeDenied)
        {
            realtimeDenied = denied;
            resized();
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
    void timerCallback() override
    {
        updateStatus();
        if (measuring && ! engine.isPinging())
        {
            measuring = false;
            const auto r = engine.finishLatencyPing();
            pingResult = r.ok ? tr ("Measured: {0} samples ({1} ms) round trip, {2} of {3} clicks, level {4} dB.",
                                    { juce::String (r.samples), juce::String (1000.0 * r.samples / juce::jmax (1.0, sampleRate()), 2), juce::String (r.found),
                                      juce::String (r.sent), juce::String (juce::Decibels::gainToDecibels (r.peak, -100.0f), 1) })
                              : r.problem == "no signal" ? tr ("No click came back. Connect the output to the input (cable or loopback) and try again.")
                              : r.problem == "unstable"  ? tr ("The clicks came back at different times ({0} samples apart): the driver is not stable at this buffer size. Nothing was stored.",
                                                               { juce::String (r.spread) })
                                                         : tr ("Only {0} of {1} clicks came back: raise the input level or use a cable loopback. Nothing was stored.",
                                                               { juce::String (r.found), juce::String (r.sent) });
            refreshRecording();
        }
    }

    double sampleRate() const
    {
        auto* device = devices.getCurrentAudioDevice();
        return device != nullptr && device->getCurrentSampleRate() > 0.0 ? device->getCurrentSampleRate() : 48000.0;
    }

    // Recording format, file names and the latency compensation (round 163)
    void setupRecordingControls()
    {
        styleLabel (formatLabel, tr ("File format"));
        styleCombo (formatBox);
        formatBox.addItem (tr ("WAV 24-bit (the device's sample rate)"), 1);
        formatBox.addItem (tr ("WAV 32-bit float (the device's sample rate)"), 2);
        formatBox.onChange = [this] {
            if (updating)
                return;
            auto s = engine.getRecordingSettings();
            s.format = formatBox.getSelectedId() == 2 ? takes::SampleFormat::float32 : takes::SampleFormat::pcm24;
            engine.setRecordingSettings (s);
        };
        styleToggle (takeNames, tr ("Name takes after the track (\"Bass DI_Take3.wav\"); off: project, date and time as FL does"));
        takeNames.onClick = [this] {
            auto s = engine.getRecordingSettings();
            s.trackTakeNames = takeNames.getToggleState();
            engine.setRecordingSettings (s);
        };
        styleNote (latencyText, {});
        styleLabel (pingOutLabel, tr ("Ping output"));
        styleLabel (pingInLabel, tr ("Ping input"));
        for (auto* b : { &pingOutBox, &pingInBox })
        {
            styleCombo (*b);
            b->setTextWhenNoChoicesAvailable (tr ("(open an input device first)"));
        }
        pingOutBox.onChange = [this] {
            if (updating)
                return;
            auto s = engine.getRecordingSettings();
            s.pingOutput = juce::jmax (0, pingOutBox.getSelectedId() - 1);
            engine.setRecordingSettings (s);
        };
        pingInBox.onChange = [this] {
            if (updating)
                return;
            auto s = engine.getRecordingSettings();
            s.pingInput = juce::jmax (0, pingInBox.getSelectedId() - 1);
            engine.setRecordingSettings (s);
        };
        measureButton.onClick = [this] {
            if (measuring)
            {
                engine.cancelLatencyPing();
                return;
            }
            const auto problem = engine.startLatencyPing();
            if (problem.isNotEmpty())
            {
                pingResult = tr ("Can't measure: {0}", { tr (problem) });
                refreshRecording();
                return;
            }
            measuring = true;
            pingResult = tr ("Measuring... (8 clicks)");
            refreshRecording();
        };
        styleNote (pingNote, tr ("Connect the ping output to the ping input with a cable (or a loopback in your interface's mixer) and turn your speakers "
                                 "down: LYNX plays 8 clicks and times their way back. The result is kept for this driver, device, sample rate and buffer size."));
        styleToggle (useMeasured, tr ("Use the measured round trip instead of the driver's figures"));
        useMeasured.onClick = [this] {
            auto s = engine.getRecordingSettings();
            s.useMeasured = useMeasured.getToggleState();
            engine.setRecordingSettings (s);
        };
        styleLabel (offsetLabel, tr ("Fine adjust (samples)"));
        offsetSlider.setSliderStyle (juce::Slider::IncDecButtons);
        offsetSlider.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 24);
        offsetSlider.setRange (-4800.0, 4800.0, 1.0);
        offsetSlider.onValueChange = [this] {
            if (updating)
                return;
            auto s = engine.getRecordingSettings();
            s.manualOffset = (int) offsetSlider.getValue();
            engine.setRecordingSettings (s);
        };
        styleNote (offsetNote, tr ("Moves every new take: positive = earlier on the Playlist. Leave it at 0 after a measurement."));
    }

    void refreshRecording()
    {
        const juce::ScopedValueSetter<bool> guard (updating, true);
        const auto& s = engine.getRecordingSettings();
        formatBox.setSelectedId (s.format == takes::SampleFormat::float32 ? 2 : 1, juce::dontSendNotification);
        takeNames.setToggleState (s.trackTakeNames, juce::dontSendNotification);
        useMeasured.setToggleState (s.useMeasured, juce::dontSendNotification);
        offsetSlider.setValue (s.manualOffset, juce::dontSendNotification);

        pingOutBox.clear (juce::dontSendNotification);
        pingInBox.clear (juce::dontSendNotification);
        if (auto* device = devices.getCurrentAudioDevice())
        {
            const auto outs = device->getOutputChannelNames();
            for (int c = 0; c + 1 < outs.size(); c += 2)
                pingOutBox.addItem (outs[c] + " - " + outs[c + 1], c + 1);
            if (outs.size() == 1)
                pingOutBox.addItem (outs[0], 1);
            pingOutBox.setSelectedId (s.pingOutput + 1, juce::dontSendNotification);
            const auto ins = engine.getInputChannelNames();
            for (int c = 0; c < (int) ins.size(); ++c)
                pingInBox.addItem (juce::String::fromUTF8 (ins[(std::size_t) c].c_str()), c + 1);
            pingInBox.setSelectedId (s.pingInput + 1, juce::dontSendNotification);
        }
        measureButton.setButtonText (measuring ? tr ("Stop measuring") : tr ("Measure the round trip"));
        measureButton.setEnabled (pingInBox.getNumItems() > 0);

        const auto l = engine.currentLatency();
        const auto rate = sampleRate();
        const auto ms = [rate] (int samples) { return juce::String (1000.0 * samples / rate, 2); };
        auto text = tr ("Takes are moved back by {0} samples ({1} ms) so they sit where you played them:", { juce::String (l.total()), ms (l.total()) }) + " ";
        text += l.measured >= 0 ? tr ("measured round trip {0}", { juce::String (l.measured) })
                                : tr ("driver's input {0} + output {1}", { juce::String (l.driverInput), juce::String (l.driverOutput) });
        if (l.manual != 0)
            text += ", " + tr ("fine adjust {0}", { juce::String (l.manual) });
        if (l.pdc != 0)
            text += ", " + tr ("plugin delay compensation {0}", { juce::String (l.pdc) });
        text += ".";
        if (pingResult.isNotEmpty())
            text += "\n" + pingResult;
        latencyText.setText (text, juce::dontSendNotification);
    }

    // The driver types with what they mean (JUCE's type names stay the identity in the saved setup).
    static juce::String driverDisplayName (const juce::String& typeName)
    {
        switch (prefs::driverKind (typeName.toStdString()))
        {
            case prefs::DriverKind::wasapiShared:     return tr ("Windows Audio (shared)");
            case prefs::DriverKind::wasapiLowLatency: return tr ("Windows Audio (shared, low-latency period)");
            case prefs::DriverKind::wasapiExclusive:  return tr ("Windows Audio (exclusive, lowest latency)");
            case prefs::DriverKind::directSound:      return tr ("DirectSound (legacy)");
            case prefs::DriverKind::asio:             return "ASIO";
            case prefs::DriverKind::lynxAsio:         return tr ("LYNX ASIO (built in)");
            case prefs::DriverKind::coreAudio:        return "Core Audio";
            case prefs::DriverKind::alsa:             return "ALSA";
            case prefs::DriverKind::jack:             return tr ("JACK / PipeWire");
            case prefs::DriverKind::pipewire:         return tr ("PipeWire (native)");
            case prefs::DriverKind::other:            break;
        }
        return typeName;
    }

    // What the selected driver type does, and for shared mode the period this device mixes at (REA round 97).
    static juce::String driverExplanation (const juce::String& typeName, juce::AudioIODevice* device)
    {
        const auto rate = device != nullptr && device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate() : 48000.0;
        const auto period = device != nullptr ? device->getDefaultBufferSize() : 0;
        switch (prefs::driverKind (typeName.toStdString()))
        {
            case prefs::DriverKind::wasapiShared:
                return tr ("Other programs keep their sound. Windows mixes every program in steps of {0} ms on this device, so the buffer cannot go below that. "
                           "For lower latency choose the exclusive mode or an ASIO driver.",
                           { juce::String (prefs::bufferLatencyMs (period > 0 ? period : 480, rate), 1) });
            case prefs::DriverKind::wasapiLowLatency:
                return period > 0 && device != nullptr && device->getAvailableBufferSizes().size() <= 1
                           ? tr ("This device does not support a shorter period in shared mode (it stays at {0} ms). Use the exclusive mode or an ASIO driver for lower latency.",
                                 { juce::String (prefs::bufferLatencyMs (period, rate), 1) })
                           : tr ("Shared like the normal mode, but asks Windows for a shorter mixing period. Only some drivers support it.");
            case prefs::DriverKind::wasapiExclusive:
                return tr ("LYNX uses the device alone: the lowest latency without ASIO, but other programs go silent while LYNX plays.");
            case prefs::DriverKind::directSound:
                return tr ("Old Windows audio path with high latency. Use it only when the others fail.");
            case prefs::DriverKind::asio:
                return tr ("Low-latency drivers. LYNX ASIO, FL Studio ASIO and ASIO4ALL work with any device; a manufacturer's ASIO driver works with its own device.");
            case prefs::DriverKind::lynxAsio:
                return tr ("LYNX's own ASIO driver, with any device: playback and recording (microphone) at low latency. \"Driver settings...\" chooses shared or exclusive mode, the output and input devices and shows the latency. "
                           "For other programs, run \"Register LYNX ASIO\" in LYNX's folder once.");
            case prefs::DriverKind::coreAudio:
                return tr ("macOS's own audio system: low latency with any device, shared with other programs.");
            case prefs::DriverKind::alsa:
                return tr ("Talks to the sound card directly. Choose \"default\" or \"pipewire\" to share the device with other programs; a \"hw:\" device gives the lowest latency but takes the card for LYNX alone.");
            case prefs::DriverKind::jack:
                return tr ("Low latency, shared with other programs and routable. On current distributions PipeWire answers as JACK (package pipewire-jack); otherwise start a JACK server first.");
            case prefs::DriverKind::pipewire:
                return tr ("LYNX talks to PipeWire directly: low latency, shared with other programs. \"System default\" follows the output chosen in "
                           "the system's sound settings (also when a device is plugged in or removed); LYNX appears as \"LYNX\" in pw-link, qpwgraph or Helvum.");
            case prefs::DriverKind::other:
                break;
        }
        return {};
    }

    AudioEngine& engine;
    juce::AudioDeviceManager& devices;
    bool updating = false;
    juce::String error;
    SectionTitle deviceTitle { tr ("Audio device") }, statusTitle { tr ("Status") }, recordTitle { tr ("Recording") };
    juce::Label driverLabel, driverNote, outputLabel, inputLabel, rateLabel, bufferLabel, bufferNote, status, load, outputChannels, realtime, recordNote, inputChannels;
    bool realtimeDenied = false;
    juce::ComboBox driverBox, outputBox, inputBox, rateBox, bufferBox;
    juce::TextButton panelButton { tr ("Driver settings...") }, testButton { tr ("Play a test sound") };
    juce::ToggleButton recordInput;
    // recording format and latency compensation (round 163)
    SectionTitle latencyTitle { tr ("Recording latency compensation") };
    juce::Label formatLabel, latencyText, pingOutLabel, pingInLabel, pingNote, offsetLabel, offsetNote;
    juce::ComboBox formatBox, pingOutBox, pingInBox;
    juce::ToggleButton takeNames, useMeasured;
    juce::TextButton measureButton { tr ("Measure the round trip") };
    juce::Slider offsetSlider;
    bool measuring = false;
    juce::String pingResult;
};

// ================================================================================================================
// MIDI
// ================================================================================================================
// One input device: on/off, which MIDI channel it listens to and what it plays.
class MidiInputRow final : public juce::Component
{
public:
    MidiInputRow (const juce::String& deviceName, const prefs::MidiInputPrefs& p, const std::vector<std::string>& channels,
                  std::function<void (const prefs::MidiInputPrefs&)> changed)
        : prefsNow (p), onChange (std::move (changed))
    {
        enabled.setButtonText (deviceName);
        enabled.setToggleState (p.enabled, juce::dontSendNotification);
        enabled.onClick = [this] {
            prefsNow.enabled = enabled.getToggleState();
            update();
        };
        addAndMakeVisible (enabled);

        channel.addItem (tr ("Any MIDI channel"), 1);
        for (int c = 1; c <= 16; ++c)
            channel.addItem (tr ("MIDI channel {0}", { juce::String (c) }), c + 1);
        channel.setSelectedId (p.midiChannel + 1, juce::dontSendNotification);
        channel.onChange = [this] {
            prefsNow.midiChannel = channel.getSelectedId() - 1;
            update();
        };

        plays.addItem (tr ("Plays the selected channels"), 1);
        for (int i = 0; i < (int) channels.size(); ++i)
            plays.addItem (tr ("Plays {0}", { juce::String (i + 1) + "  " + juce::String::fromUTF8 (channels[(std::size_t) i].c_str()) }), i + 2);
        if (p.target >= (int) channels.size()) // a channel this project doesn't have (kept)
            plays.addItem (tr ("Plays channel {0}", { juce::String (p.target + 1) }), p.target + 2);
        plays.setSelectedId (p.target + 2, juce::dontSendNotification);
        plays.onChange = [this] {
            prefsNow.target = plays.getSelectedId() - 2;
            update();
        };
        for (auto* b : { &channel, &plays })
        {
            styleCombo (*b);
            addAndMakeVisible (b);
        }
        update (false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        plays.setBounds (r.removeFromRight (230));
        r.removeFromRight (8);
        channel.setBounds (r.removeFromRight (150));
        r.removeFromRight (8);
        enabled.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colour::bg1);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::radius::md);
    }

private:
    void update (bool notify = true)
    {
        channel.setEnabled (prefsNow.enabled);
        plays.setEnabled (prefsNow.enabled);
        if (notify && onChange)
            onChange (prefsNow);
    }

    prefs::MidiInputPrefs prefsNow;
    std::function<void (const prefs::MidiInputPrefs&)> onChange;
    juce::ToggleButton enabled;
    juce::ComboBox channel, plays;
};

// One output device (FL's Options › MIDI "Output" panel, round 165f): master sync (MIDI clock or MTC) with its
// offset, and all notes off on init and after an export.
class MidiOutputRow final : public juce::Component
{
public:
    MidiOutputRow (const juce::String& deviceName, const prefs::MidiOutputPrefs& p, std::function<void (const prefs::MidiOutputPrefs&)> changed)
        : prefsNow (p), onChange (std::move (changed))
    {
        name.setText (deviceName, juce::dontSendNotification);
        name.setFont (theme::font (theme::type::body));
        name.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (name);

        sync.setButtonText (tr ("Send master sync"));
        sync.setTooltip (tr ("Send master synchronization to this device"));
        sync.setToggleState (p.masterSync, juce::dontSendNotification);
        sync.onClick = [this] {
            prefsNow.masterSync = sync.getToggleState();
            update();
        };
        for (int t = 0; t < midisync::syncTypeCount; ++t)
            type.addItem (tr (midisync::syncTypeName ((midisync::SyncType) t)), t + 1);
        type.setTooltip (tr ("MIDI clock: 24 pulses per quarter note, Start / Continue / Stop and Song Position Pointer. "
                             "MTC: MIDI Time Code quarter frames and a full frame when the position moves."));
        type.setSelectedId ((int) midisync::syncTypeFromKey (p.syncType) + 1, juce::dontSendNotification);
        type.onChange = [this] {
            prefsNow.syncType = midisync::syncTypeKey ((midisync::SyncType) (type.getSelectedId() - 1));
            update();
        };
        styleCombo (type);
        offsetLabel.setText (tr ("Offset (ms)"), juce::dontSendNotification);
        offsetLabel.setFont (theme::font (theme::type::small));
        offsetLabel.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        offset.setSliderStyle (juce::Slider::IncDecButtons);
        offset.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 56, 24);
        offset.setRange (-1000.0, 1000.0, 1.0);
        offset.setValue (p.offsetMs, juce::dontSendNotification);
        offset.setTooltip (tr ("Master sync offset (milliseconds): positive sends the sync later, negative earlier"));
        offset.setDoubleClickReturnValue (true, 0.0);
        offset.onValueChange = [this] {
            prefsNow.offsetMs = (int) offset.getValue();
            update();
        };
        notesOff.setButtonText (tr ("Send all notes off"));
        notesOff.setTooltip (tr ("Send all notes off to this device on init and after an export"));
        notesOff.setToggleState (p.allNotesOff, juce::dontSendNotification);
        notesOff.onClick = [this] {
            prefsNow.allNotesOff = notesOff.getToggleState();
            update();
        };
        for (auto* c : std::initializer_list<juce::Component*> { &sync, &type, &offsetLabel, &offset, &notesOff })
            addAndMakeVisible (c);
        update (false);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 4);
        auto top = r.removeFromTop (24);
        notesOff.setBounds (top.removeFromRight (170));
        name.setBounds (top);
        r.removeFromTop (4);
        auto bottom = r.removeFromTop (26);
        sync.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (8);
        type.setBounds (bottom.removeFromLeft (190));
        bottom.removeFromLeft (12);
        offsetLabel.setBounds (bottom.removeFromLeft (76));
        offset.setBounds (bottom.removeFromLeft (110));
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (theme::colour::bg1);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), theme::radius::md);
    }

private:
    void update (bool notify = true)
    {
        type.setEnabled (prefsNow.masterSync);
        offset.setEnabled (prefsNow.masterSync);
        if (notify && onChange)
            onChange (prefsNow);
    }

    prefs::MidiOutputPrefs prefsNow;
    std::function<void (const prefs::MidiOutputPrefs&)> onChange;
    juce::Label name, offsetLabel;
    juce::ToggleButton sync, notesOff;
    juce::ComboBox type;
    juce::Slider offset;
};

class MidiPage final : public ScrollingPage,
                       private juce::ChangeListener,
                       private juce::Timer,
                       private juce::ListBoxModel
{
public:
    MidiPage (AudioEngine& e, SettingsHooks h, bool keep) : engine (e), hooks (std::move (h)), persistent (keep)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &inputsTitle, &notesTitle, &controllersTitle, &inputsNote, &refresh, &noDevices,
                                                                 &velocityLabel, &velocityBox, &velocityNote, &typeLabel, &typeValue, &pickup,
                                                                 &pickupNote, &linksLabel, &links, &removeLink, &removeAll, &outputsTitle,
                                                                 &outputsNote, &noOutputs, &syncTitle, &clockLabel, &clockBox, &clockOffsetLabel,
                                                                 &clockOffset, &clockStatusLabel, &clockStatus, &clockNote })
            add (*c);
        styleNote (outputsNote, tr ("Send master sync drives drum machines, synths and other software from LYNX's transport: MIDI clock "
                                    "with Song Position Pointer, or MIDI Time Code."));
        styleLabel (noOutputs, tr ("No MIDI output devices found."));
        styleLabel (clockLabel, tr ("External clock sync"));
        clockBox.setTooltip (tr ("LYNX follows the MIDI clock of this input: its tempo, Start / Continue / Stop and Song Position Pointer"));
        clockBox.onChange = [this] {
            if (loading)
                return;
            auto p = engine.getMidiPrefs();
            const auto id = clockBox.getSelectedId();
            p.clockInput = id > 1 && id - 2 < clockInputs.size() ? clockInputs[id - 2].toStdString() : std::string();
            // (FL asks "The device isn't enabled" → Enable device: the clock input is turned on with it)
            if (! p.clockInput.empty() && ! p.input (p.clockInput).enabled)
            {
                auto in = p.input (p.clockInput);
                in.enabled = true;
                p.setInput (in);
                set (p);
                rebuildInputs();
                return;
            }
            set (p);
        };
        styleCombo (clockBox);
        styleLabel (clockOffsetLabel, tr ("External sync offset (milliseconds)"));
        clockOffset.setSliderStyle (juce::Slider::IncDecButtons);
        clockOffset.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 80, 24);
        clockOffset.setRange (-1000.0, 1000.0, 1.0);
        clockOffset.setDoubleClickReturnValue (true, 0.0);
        clockOffset.setTooltip (tr ("Positive plays LYNX later than the clock, negative earlier"));
        clockOffset.onValueChange = [this] {
            if (loading)
                return;
            auto p = engine.getMidiPrefs();
            p.clockOffsetMs = (int) clockOffset.getValue();
            set (p);
        };
        styleLabel (clockStatusLabel, tr ("Status"));
        styleLabel (clockStatus, tr ("No clock"), false);
        styleNote (clockNote, tr ("Choosing an input turns it on in Input devices above. Song time will match the clock."));
        startTimerHz (4);
        styleNote (inputsNote, tr ("Each input device can be turned off, listen to one MIDI channel only and play a fixed channel of the "
                                   "Channel Rack instead of the selected ones."));
        styleLabel (noDevices, tr ("No MIDI input devices found. Connect one and refresh the list."));
        refresh.onClick = [this] {
            if (persistent)
                engine.reconnectMidiInputs(); // also wakes a device that stopped sending
            rebuildInputs();
        };

        styleLabel (velocityLabel, tr ("Velocity"));
        velocityBox.addItem (tr ("As played"), 1);
        velocityBox.addItem (tr ("Soft (light playing sounds louder)"), 2);
        velocityBox.addItem (tr ("Hard (needs harder playing)"), 3);
        velocityBox.addItem (tr ("Fixed (every note at full velocity)"), 4);
        velocityBox.onChange = [this] {
            auto p = engine.getMidiPrefs();
            p.velocity = (prefs::VelocityCurve) (velocityBox.getSelectedId() - 1);
            set (p);
        };
        styleCombo (velocityBox);
        styleNote (velocityNote, tr ("For notes from MIDI devices (the typing keyboard always plays at the Piano Roll's velocity)."));

        styleLabel (typeLabel, tr ("Controller type"));
        typeLabel.setJustificationType (juce::Justification::topLeft);
        styleLabel (typeValue, tr ("Generic controller: link a knob or fader with a control's right-click menu > Link to MIDI controller."), false);
        typeValue.setMinimumHorizontalScale (1.0f);
        typeValue.setJustificationType (juce::Justification::topLeft);
        styleToggle (pickup, tr ("Pickup (takeover mode)"));
        pickup.onClick = [this] {
            auto p = engine.getMidiPrefs();
            p.pickup = pickup.getToggleState();
            set (p);
        };
        styleNote (pickupNote, tr ("A controller moves a parameter only after reaching its current value, so nothing jumps."));

        styleLabel (linksLabel, tr ("Links (this project, then global)"));
        linksLabel.setJustificationType (juce::Justification::topLeft);
        links.setModel (this);
        links.setRowHeight (24);
        links.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        links.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        links.setOutlineThickness (1);
        removeLink.onClick = [this] {
            const auto row = links.getSelectedRow();
            const auto projectLinks = (int) engine.getSong().midiLinks.size();
            if (juce::isPositiveAndBelow (row, projectLinks))
                engine.editSong ([row] (Song& s) { s.midiLinks.erase (s.midiLinks.begin() + row); });
            else if (const auto global = engine.getGlobalLinks(); juce::isPositiveAndBelow (row - projectLinks, (int) global.size()))
                engine.removeGlobalLink (global[(std::size_t) (row - projectLinks)].target);
        };
        removeAll.onClick = [this] { engine.editSong ([] (Song& s) { s.midiLinks.clear(); }); };

        if (! persistent)
        {
            // --snapshot: devices of a typical setup, and two links
            sampleDevices = { "Arturia KeyLab 49", "Akai MPD218" };
            sampleOutputs = { "Roland TR-8S", "Korg volca sample" };
            auto p = engine.getMidiPrefs();
            prefs::MidiOutputPrefs drums;
            drums.name = "Roland TR-8S";
            drums.masterSync = true;
            drums.allNotesOff = true;
            p.setOutput (drums);
            p.clockInput = "Akai MPD218";
            engine.setMidiPrefs (p, false);
            engine.editSong ([] (Song& s) {
                AutomationTarget volume;
                volume.kind = AutomationTargetKind::mixerVolume;
                volume.index = 0;
                AutomationTarget pan;
                pan.kind = AutomationTargetKind::channelPan;
                pan.index = 0;
                s.midiLinks = { { 7, -1, volume }, { 10, 0, pan } };
            }, false);
        }

        engine.addChangeListener (this);
        rebuildInputs();
        load();
    }

    ~MidiPage() override
    {
        stopTimer();
        engine.removeChangeListener (this);
    }

private:
    int layout (int width) override
    {
        Rows r (width);
        r.section (inputsTitle, true);
        inputsNote.setBounds (r.full (32).translated (0, -4));
        if (rows.empty())
            noDevices.setBounds (r.full (24));
        for (auto& row : rows)
            row->setBounds (r.full (34).translated (0, -4));
        refresh.setBounds (r.full().removeFromLeft (150));

        r.section (outputsTitle);
        outputsNote.setBounds (r.full (32).translated (0, -4));
        if (outputRows.empty())
            noOutputs.setBounds (r.full (24));
        for (auto& row : outputRows)
            row->setBounds (r.full (66).translated (0, -4));

        r.section (syncTitle);
        clockBox.setBounds (r.row (clockLabel, 300));
        clockOffset.setBounds (r.row (clockOffsetLabel, 200));
        clockStatus.setBounds (r.row (clockStatusLabel, 0));
        clockNote.setBounds (r.indented (16).translated (0, -3));

        r.section (notesTitle);
        velocityBox.setBounds (r.row (velocityLabel, 300));
        velocityNote.setBounds (r.indented (16).translated (0, -3));

        r.section (controllersTitle);
        {
            auto row = r.row (typeLabel, 0, 36);
            typeLabel.setBounds (typeLabel.getBounds().withTrimmedTop (6));
            typeValue.setBounds (row.withTrimmedTop (6));
        }
        pickup.setBounds (r.full (24));
        pickupNote.setBounds (r.full (16).translated (0, -3));
        {
            auto row = r.row (linksLabel, 0, 110);
            linksLabel.setBounds (linksLabel.getBounds().withTrimmedTop (4));
            auto buttons = row.removeFromRight (120);
            row.removeFromRight (8);
            links.setBounds (row);
            removeLink.setBounds (buttons.removeFromTop (26));
            buttons.removeFromTop (6);
            removeAll.setBounds (buttons.removeFromTop (26));
        }
        return r.bottom();
    }

    void set (const prefs::MidiPrefs& p)
    {
        if (loading)
            return;
        engine.setMidiPrefs (p, persistent);
        if (hooks.midiChanged)
            hooks.midiChanged();
    }

    void load()
    {
        const juce::ScopedValueSetter<bool> guard (loading, true);
        const auto& p = engine.getMidiPrefs();
        velocityBox.setSelectedId ((int) p.velocity + 1, juce::dontSendNotification);
        pickup.setToggleState (p.pickup, juce::dontSendNotification);
        {
            auto id = 1;
            for (int i = 0; i < clockInputs.size(); ++i)
                if (clockInputs[i].toStdString() == p.clockInput)
                    id = i + 2;
            clockBox.setSelectedId (id, juce::dontSendNotification);
        }
        clockOffset.setValue (p.clockOffsetMs, juce::dontSendNotification);
        links.updateContent();
        links.repaint();
        removeLink.setEnabled (links.getSelectedRow() >= 0);
        removeAll.setEnabled (! engine.getSong().midiLinks.empty());
    }

    // the project's links, then the global ones (settings; FL's "Override global link...", REA round 146)
    int linkRows() const { return (int) (engine.getSong().midiLinks.size() + engine.getGlobalLinks().size()); }

    void rebuildInputs()
    {
        rows.clear();
        juce::StringArray names;
        if (persistent)
            for (const auto& d : juce::MidiInput::getAvailableDevices())
                names.add (d.name);
        else
            names = sampleDevices;

        std::vector<std::string> channels;
        for (const auto& c : engine.getSong().channels)
            channels.push_back (c.name);

        for (const auto& name : names)
        {
            auto row = std::make_unique<MidiInputRow> (name, engine.getMidiPrefs().input (name.toStdString()), channels,
                                                       [this] (const prefs::MidiInputPrefs& in) {
                                                           auto p = engine.getMidiPrefs();
                                                           p.setInput (in);
                                                           set (p);
                                                       });
            add (*row);
            rows.push_back (std::move (row));
        }
        noDevices.setVisible (rows.empty());

        // the clock input list: off, then the inputs (a saved one missing now stays listed)
        clockInputs = names;
        if (const auto saved = juce::String::fromUTF8 (engine.getMidiPrefs().clockInput.c_str()); saved.isNotEmpty() && ! clockInputs.contains (saved))
            clockInputs.add (saved);
        clockBox.clear (juce::dontSendNotification);
        clockBox.addItem (tr ("Off"), 1);
        for (int i = 0; i < clockInputs.size(); ++i)
            clockBox.addItem (clockInputs[i], i + 2);

        outputRows.clear();
        juce::StringArray outputs;
        if (persistent)
            for (const auto& d : juce::MidiOutput::getAvailableDevices())
                outputs.add (d.name);
        else
            outputs = sampleOutputs;
        for (const auto& name : outputs)
        {
            auto row = std::make_unique<MidiOutputRow> (name, engine.getMidiPrefs().output (name.toStdString()), [this] (const prefs::MidiOutputPrefs& out) {
                auto p = engine.getMidiPrefs();
                p.setOutput (out);
                set (p);
            });
            add (*row);
            outputRows.push_back (std::move (row));
        }
        noOutputs.setVisible (outputRows.empty());
        load();
        resized();
    }

    void timerCallback() override
    {
        // FL's "Status: off / ready / running [Beat n] / error" (REA 165f), with the tempo received
        const auto& p = engine.getMidiPrefs();
        auto& sync = engine.getMidiSync();
        const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        juce::String text;
        if (p.clockInput.empty())
            text = tr ("Off");
        else if (! persistent)
            text = tr ("Running: beat {0}, {1} BPM", { "17", "120.0" }); // --snapshot
        else if (! engine.getActiveMidiInputs().contains (juce::String::fromUTF8 (p.clockInput.c_str())))
            text = tr ("Error: the input is not open");
        else if (! sync.receiver().receiving (now))
            text = tr ("Ready, no clock");
        else if (sync.receiver().tooFast())
            text = tr ("Clock is too fast!");
        else if (sync.receiver().running())
            text = tr ("Running: beat {0}, {1} BPM", { juce::String ((int) (engine.getTransport().positionInBeats()) + 1),
                                                       juce::String (sync.receiver().bpm(), 1) });
        else
            text = tr ("Receiving clock {0} BPM", { juce::String (sync.receiver().bpm(), 1) });
        if (clockStatus.getText() != text)
            clockStatus.setText (text, juce::dontSendNotification);
    }

    // links list
    int getNumRows() override { return juce::jmax (1, linkRows()); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        auto list = engine.getSong().midiLinks;
        const auto projectLinks = (int) list.size();
        for (const auto& l : engine.getGlobalLinks())
            list.push_back (l);
        if (list.empty() || row >= (int) list.size())
        {
            g.setFont (theme::font (theme::type::small));
            g.setColour (theme::colour::textDisabled);
            g.drawText (tr ("(none yet)"), 10, 0, width - 20, height, juce::Justification::centredLeft);
            return;
        }
        if (selected)
        {
            g.setColour (theme::colour::accentMuted);
            g.fillRect (0, 0, width, height);
        }
        const auto& link = list[(std::size_t) row];
        g.setFont (theme::font (theme::type::body));
        g.setColour (theme::colour::info);
        const auto source = "CC " + juce::String (link.cc) + (link.channel >= 0 ? juce::String::fromUTF8 (" \xc2\xb7 ") + tr ("ch. {0}", { juce::String (link.channel + 1) }) : juce::String());
        g.drawText (source, 10, 0, 110, height, juce::Justification::centredLeft);
        g.setColour (theme::colour::textPrimary);
        g.drawText (describeAutomationTarget (engine, link.target) + (row >= projectLinks ? "  (" + tr ("global") + ")" : juce::String()), 124, 0,
                    width - 134, height, juce::Justification::centredLeft, true);
    }

    void selectedRowsChanged (int) override { removeLink.setEnabled (links.getSelectedRow() >= 0 && linkRows() > 0); }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (engine.getSong().channels.size() != channelCount)
        {
            channelCount = engine.getSong().channels.size();
            rebuildInputs();
        }
        load();
    }

    AudioEngine& engine;
    SettingsHooks hooks;
    bool persistent;
    bool loading = false;
    std::size_t channelCount = 0;
    juce::StringArray sampleDevices;
    std::vector<std::unique_ptr<MidiInputRow>> rows;
    std::vector<std::unique_ptr<MidiOutputRow>> outputRows;
    juce::StringArray sampleOutputs, clockInputs;
    SectionTitle outputsTitle { tr ("Output devices") }, syncTitle { tr ("External sync") };
    juce::Label outputsNote, noOutputs, clockLabel, clockOffsetLabel, clockStatusLabel, clockStatus, clockNote;
    juce::ComboBox clockBox;
    juce::Slider clockOffset;
    SectionTitle inputsTitle { tr ("Input devices") }, notesTitle { tr ("Notes") }, controllersTitle { tr ("Controllers") };
    juce::Label inputsNote, noDevices, velocityLabel, velocityNote, typeLabel, typeValue, pickupNote, linksLabel;
    juce::TextButton refresh { tr ("Refresh device list") }, removeLink { tr ("Remove link") }, removeAll { tr ("Remove all") };
    juce::ComboBox velocityBox;
    juce::ToggleButton pickup;
    juce::ListBox links;
};
} // namespace

std::unique_ptr<juce::Component> makeGeneralPage (AudioEngine& engine, const SettingsHooks& hooks, bool persistent)
{
    return std::make_unique<GeneralPage> (engine, hooks, persistent);
}

std::unique_ptr<juce::Component> makeAudioPage (AudioEngine& engine, bool persistent)
{
    juce::ignoreUnused (persistent);
    return std::make_unique<AudioPage> (engine);
}

std::unique_ptr<juce::Component> makeMidiPage (AudioEngine& engine, const SettingsHooks& hooks, bool persistent)
{
    return std::make_unique<MidiPage> (engine, hooks, persistent);
}
} // namespace lynx
