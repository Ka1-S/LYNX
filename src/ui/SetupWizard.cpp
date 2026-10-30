#include "ui/SetupWizard.h"

#include <juce_audio_utils/juce_audio_utils.h>

#include "engine/AudioEngine.h"
#include "engine/FlStudio.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "platform/Paths.h"
#include "platform/FileTypes.h"

namespace lynx
{
namespace
{
juce::File setupFile()
{
    return PluginHost::settingsFolder().getChildFile ("setup-done.txt");
}

juce::File scanAtStartFile()
{
    return PluginHost::settingsFolder().getChildFile ("scan-at-start.txt");
}

class SectionLabel final : public juce::Label
{
public:
    explicit SectionLabel (const juce::String& text)
    {
        setText (text, juce::dontSendNotification);
        setFont (theme::font (theme::type::title, true));
        setColour (juce::Label::textColourId, theme::colour::textPrimary);
    }
};

class SetupWizard final : public juce::Component,
                          private juce::ListBoxModel,
                          private juce::ComponentListener,
                          private juce::AsyncUpdater,
                          private juce::Timer
{
public:
    SetupWizard (AudioEngine& e, std::function<void (const juce::String&)> language, std::function<void()> close)
        : engine (e),
          onLanguage (std::move (language)),
          onClose (std::move (close)),
          devices (e.getDeviceManager(), 0, 0, 2, 2, false, false, true, true)
    {
        auto& host = engine.getPluginHost();

        title.setText (tr ("Welcome to LYNX SHARE"), juce::dontSendNotification);
        title.setFont (theme::font (theme::type::display, true));
        title.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        intro.setText (tr ("A few things to get started. Everything here can be changed later (Options > Setup wizard)."),
                       juce::dontSendNotification);
        intro.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        intro.setJustificationType (juce::Justification::topLeft);
        intro.setMinimumHorizontalScale (1.0f);
        for (auto* c : std::initializer_list<juce::Component*> { &title, &intro, &languageLabel, &languageBox, &audioLabel, &devices, &flLabel,
                                                                 &flStatus, &foldersLabel, &folders, &addFolder, &removeFolder, &resetFolders,
                                                                 &scanNow, &progressBar, &scanStatus, &scanAtStart })
            body.addAndMakeVisible (c);
        // the Linux AppImage (round 161): an applications-menu entry and the .lynx file type, on by default
        body.addChildComponent (menuEntry);
        menuEntry.setVisible (runningAsAppImage());
        menuEntry.setToggleState (true, juce::dontSendNotification);
        viewport.setViewedComponent (&body, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (10);
        addAndMakeVisible (viewport);
        addAndMakeVisible (skip);
        addAndMakeVisible (finish);
        devices.addComponentListener (this);
        // language
        int id = 1;
        for (const auto& l : i18n::languages())
        {
            languageBox.addItem (juce::String::fromUTF8 (l.nativeName), id);
            if (i18n::current() == l.id)
                languageBox.setSelectedId (id, juce::dontSendNotification);
            ++id;
        }

        // FL Studio
        const auto fl = findFlStudio();
        if (fl.found())
        {
            const auto& db = host.flPluginDatabase();
            if (db.empty())
                flStatus.setText (tr ("{0} found (its plugin database lists no third-party plugins yet). .flp projects find their "
                                      "samples in FL Studio's folders; their plugins are found by scanning.", { fl.name() }),
                                  juce::dontSendNotification);
            else
                flStatus.setText (tr ("{0} found. Its plugin database lists {1} plugin file(s); .flp projects use the same plugin files FL Studio uses, "
                                      "and their samples are looked for in FL Studio's folders.",
                                      { fl.name(), juce::String ((int) db.entries().size()) }),
                                  juce::dontSendNotification);
        }
        else
            flStatus.setText (tr ("FL Studio was not found on this PC. .flp projects still open; their plugins are found by scanning."),
                              juce::dontSendNotification);
        flStatus.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        flStatus.setMinimumHorizontalScale (1.0f);
        flStatus.setJustificationType (juce::Justification::topLeft);

        // plugin folders, pre-filled with the standard ones and FL Studio's
        const auto current = host.pluginFolders();
        for (int i = 0; i < current.getNumPaths(); ++i)
            folderList.add (current[i].getFullPathName());
        folders.setModel (this);
        folders.setRowHeight (22);
        folders.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        addFolder.onClick = [this] { chooseFolder(); };
        removeFolder.onClick = [this] {
            if (const auto row = folders.getSelectedRow(); row >= 0)
            {
                folderList.remove (row);
                folders.updateContent();
                folders.repaint();
            }
        };
        resetFolders.onClick = [this] {
            const auto suggested = engine.getPluginHost().suggestedFolders();
            folderList.clear();
            for (int i = 0; i < suggested.getNumPaths(); ++i)
                folderList.add (suggested[i].getFullPathName());
            folders.updateContent();
            folders.repaint();
        };

        // scan
        scanNow.onClick = [this] {
            saveFolders();
            engine.getPluginHost().startBackgroundScan();
            scanStarted = true;
        };
        scanAtStart.setToggleState (backgroundScanAtStart(), juce::dontSendNotification);
        scanStatus.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        scanStatus.setJustificationType (juce::Justification::topLeft);
        scanStatus.setMinimumHorizontalScale (1.0f);
        progressBar.setColour (juce::ProgressBar::foregroundColourId, theme::colour::accent);
        progressBar.setPercentageDisplay (false);

        skip.onClick = [this] { skipSetup(); };
        finish.onClick = [this] { finishSetup(); };
        finish.setColour (juce::TextButton::buttonColourId, theme::colour::accent);

        updateScanStatus();
        startTimerHz (10);
        setSize (680, getIdealHeight (680));
    }

    ~SetupWizard() override
    {
        cancelPendingUpdate();
        devices.removeComponentListener (this);
        folders.setModel (nullptr);
    }

    // Runs when the content's natural height changed by itself (the device selector grew or shrank).
    std::function<void()> onIdealHeightChanged;

    // The height that shows everything without scrolling at this width.
    int getIdealHeight (int width)
    {
        return layoutBody (width) + footerHeight;
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg0);
        g.fillRect (0, getHeight() - footerHeight, getWidth(), 1);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto footer = r.removeFromBottom (footerHeight).reduced (20, 10);
        finish.setBounds (footer.removeFromRight (120));
        footer.removeFromRight (10);
        skip.setBounds (footer.removeFromRight (120));

        viewport.setBounds (r);
        auto width = r.getWidth();
        auto height = layoutBody (width);
        if (height > r.getHeight()) // a scroll bar: lay out again beside it
        {
            width -= viewport.getScrollBarThickness();
            height = layoutBody (width);
        }
        body.setSize (width, height);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
        {
            skipSetup();
            return true;
        }
        return false;
    }

    void skipSetup()
    {
        markSetupWizardDone(); // the wizard doesn't come back by itself
        if (onClose)
            onClose();
    }

private:
    static constexpr int footerHeight = 50;

    // A wrapping label's height for `width` (the intro and the status texts are longer in pt-BR).
    static int textHeight (const juce::Label& label, int width, int minimum)
    {
        juce::AttributedString text;
        text.append (label.getText(), label.getFont());
        juce::TextLayout layout;
        layout.createLayout (text, (float) juce::jmax (1, width - label.getBorderSize().getLeftAndRight()));
        return juce::jmax (minimum, (int) std::ceil (layout.getHeight()) + label.getBorderSize().getTopAndBottom() + 2);
    }

    // Places the content for `width` and returns its height. The device selector is given the width first and then
    // keeps the height it chose for itself, so nothing below it can overlap it.
    int layoutBody (int width)
    {
        const juce::ScopedValueSetter<bool> busy (layingOut, true);
        auto r = juce::Rectangle<int> (0, 0, width, 100000).reduced (20, 0);
        r.removeFromTop (16);
        const auto w = r.getWidth();
        title.setBounds (r.removeFromTop (30));
        intro.setBounds (r.removeFromTop (textHeight (intro, w, 22)));
        r.removeFromTop (8);

        auto row = r.removeFromTop (26);
        languageLabel.setBounds (row.removeFromLeft (150));
        languageBox.setBounds (row.removeFromLeft (juce::jmin (220, row.getWidth())));
        r.removeFromTop (10);

        audioLabel.setBounds (r.removeFromTop (24));
        devices.setBounds (r.getX(), r.getY(), w, 200); // its resized() sets its own height
        r.removeFromTop (juce::jmax (0, devices.getHeight() - 18)); // it ends with an empty row of its own
        r.removeFromTop (6);

        flLabel.setBounds (r.removeFromTop (24));
        flStatus.setBounds (r.removeFromTop (textHeight (flStatus, w, 22)));
        r.removeFromTop (10);

        foldersLabel.setBounds (r.removeFromTop (24));
        auto list = r.removeFromTop (118);
        auto buttons = list.removeFromRight (juce::jmin (150, w / 3));
        folders.setBounds (list.withTrimmedRight (8));
        addFolder.setBounds (buttons.removeFromTop (26));
        buttons.removeFromTop (6);
        removeFolder.setBounds (buttons.removeFromTop (26));
        buttons.removeFromTop (6);
        resetFolders.setBounds (buttons.removeFromTop (26));
        r.removeFromTop (10);

        row = r.removeFromTop (26);
        scanNow.setBounds (row.removeFromLeft (150));
        row.removeFromLeft (10);
        progressBar.setBounds (row);
        r.removeFromTop (4);
        scanStatus.setBounds (r.removeFromTop (textHeight (scanStatus, w, 22)));
        scanAtStart.setBounds (r.removeFromTop (24));
        if (menuEntry.isVisible())
            menuEntry.setBounds (r.removeFromTop (24));
        r.removeFromTop (12);
        return r.getY();
    }

    void componentMovedOrResized (juce::Component&, bool, bool wasResized) override
    {
        if (wasResized && ! layingOut)
            triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        if (onIdealHeightChanged)
            onIdealHeightChanged(); // the owner resizes this (which lays it out again)
        else
            resized();
    }

private:
    int getNumRows() override { return folderList.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (selected)
            g.fillAll (theme::colour::accentMuted);
        g.setColour (theme::colour::textPrimary);
        g.setFont (theme::font (theme::type::body));
        g.drawText (folderList[row], 8, 0, width - 16, height, juce::Justification::centredLeft, true);
    }

    void chooseFolder()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Plugin folder"), juce::File(), "");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& c) {
            if (const auto folder = c.getResult(); folder.isDirectory())
            {
                folderList.addIfNotAlreadyThere (folder.getFullPathName(), true);
                folders.updateContent();
                folders.repaint();
            }
        });
    }

    void saveFolders()
    {
        juce::FileSearchPath paths;
        for (const auto& f : folderList)
            paths.add (juce::File (f));
        engine.getPluginHost().setPluginFolders (paths);
        setBackgroundScanAtStart (scanAtStart.getToggleState());
    }

    void finishSetup()
    {
        saveFolders();
        markSetupWizardDone();
        if (runningAsAppImage())
            setAppImageMenuEntry (menuEntry.getToggleState());
        if (! scanStarted)
            engine.getPluginHost().startBackgroundScan(); // keeps going after the window closes
        const auto chosen = languageBox.getSelectedId() - 1;
        const auto& languages = i18n::languages();
        const auto language = chosen >= 0 && chosen < (int) languages.size() ? juce::String (languages[(std::size_t) chosen].id) : i18n::saved();
        auto languageCallback = onLanguage;
        auto close = onClose;
        if (close)
            close(); // deletes this
        if (language != i18n::saved() && languageCallback)
            languageCallback (language);
    }

    void updateScanStatus()
    {
        const auto s = engine.getPluginHost().backgroundScan();
        progress = s.running ? s.progress : (s.finished > 0 ? 1.0 : 0.0);
        const auto count = engine.getPluginHost().getKnownPlugins().getNumTypes();
        if (s.running)
            scanStatus.setText (tr ("Scanning in the background: {0}", { s.file }), juce::dontSendNotification);
        else if (s.finished > 0)
            scanStatus.setText (tr ("Scan finished: {0} new, {1} plugins known.", { juce::String (s.found), juce::String (count) }), juce::dontSendNotification);
        else
            scanStatus.setText (tr ("{0} plugins known. Scanning runs in the background; you can close this window.", { juce::String (count) }),
                                juce::dontSendNotification);
        scanNow.setEnabled (! s.running);
        progressBar.setVisible (s.running || s.finished > 0);
    }

    void timerCallback() override { updateScanStatus(); }

    AudioEngine& engine;
    std::function<void (const juce::String&)> onLanguage;
    std::function<void()> onClose;

    juce::Component body; // the scrolling content
    juce::Viewport viewport;
    bool layingOut = false;
    juce::Label title, intro, flStatus, scanStatus;
    SectionLabel languageLabel { tr ("Language") }, audioLabel { tr ("Audio output") }, flLabel { "FL Studio" },
        foldersLabel { tr ("Plugin folders ({0})", { platform::pluginFormatNames() }) };
    juce::ComboBox languageBox;
    juce::AudioDeviceSelectorComponent devices;
    juce::StringArray folderList;
    juce::ListBox folders;
    juce::TextButton addFolder { tr ("Add folder...") }, removeFolder { tr ("Remove") }, resetFolders { tr ("Suggested folders") };
    juce::TextButton scanNow { tr ("Scan now") }, skip { tr ("Skip") }, finish { tr ("Finish") };
    double progress = 0.0;
    juce::ProgressBar progressBar { progress };
    juce::ToggleButton scanAtStart { tr ("Look for new plugins in the background at every start") };
    juce::ToggleButton menuEntry { tr ("Show LYNX SHARE in the applications menu and open .lynx files with it") };
    std::unique_ptr<juce::FileChooser> chooser;
    bool scanStarted = false;
};

// The wizard on top of the main window (round 140; it was a separate window before, so the first start showed
// only the wizard and not the DAW): the DAW stays visible, dimmed, behind a centred panel. Escape = Skip.
class WizardOverlay final : public juce::Component
{
public:
    WizardOverlay (AudioEngine& engine, std::function<void (const juce::String&)> onLanguage, std::function<void()> close)
        : wizard (engine, std::move (onLanguage), std::move (close))
    {
        setWantsKeyboardFocus (true);
        wizard.onIdealHeightChanged = [this] { resized(); };
        addAndMakeVisible (wizard);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colours::black.withAlpha (0.45f));
        const auto panel = wizard.getBounds().toFloat();
        for (int i = 1; i <= 6; ++i) // a soft shadow
        {
            g.setColour (juce::Colours::black.withAlpha (0.08f));
            g.drawRoundedRectangle (panel.expanded ((float) i), 4.0f + (float) i, 2.0f);
        }
        g.setColour (theme::colour::accent.withAlpha (0.6f));
        g.drawRect (wizard.getBounds().expanded (1), 1);
    }

    void resized() override
    {
        const auto area = getLocalBounds().reduced (16);
        const auto width = juce::jmin (680, area.getWidth());
        const auto height = juce::jmin (wizard.getIdealHeight (width), area.getHeight());
        wizard.setBounds (area.withSizeKeepingCentre (width, height));
        wizard.resized(); // getIdealHeight laid it out without a scroll bar
        repaint();
    }

    void visibilityChanged() override
    {
        if (isShowing())
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<juce::Component> (this)] {
                if (safe != nullptr && safe->isShowing())
                    safe->grabKeyboardFocus();
            });
    }

    // modal for the window behind it: its keys (the typing piano, shortcuts) don't reach the DAW
    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::escapeKey)
            wizard.skipSetup();
        return true;
    }
    bool keyStateChanged (bool) override { return true; }

private:
    SetupWizard wizard;
};
} // namespace

std::unique_ptr<juce::Component> makeSetupWizardContent (AudioEngine& engine, std::function<void (const juce::String&)> onLanguage,
                                                         std::function<void()> onClose)
{
    return std::make_unique<SetupWizard> (engine, std::move (onLanguage), std::move (onClose));
}

std::unique_ptr<juce::Component> makeSetupWizardOverlay (AudioEngine& engine, std::function<void (const juce::String&)> onLanguage,
                                                         std::function<void()> onClose)
{
    return std::make_unique<WizardOverlay> (engine, std::move (onLanguage), std::move (onClose));
}

bool setupWizardDone()
{
    return setupFile().existsAsFile();
}

void markSetupWizardDone()
{
    if (! setupFile().existsAsFile())
        setupFile().replaceWithText (juce::Time::getCurrentTime().toISO8601 (true));
}

bool backgroundScanAtStart()
{
    return scanAtStartFile().loadFileAsString().trim() != "0";
}

void setBackgroundScanAtStart (bool on)
{
    scanAtStartFile().replaceWithText (on ? "1" : "0");
}
} // namespace lynx
