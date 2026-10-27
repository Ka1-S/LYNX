#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/Preferences.h"
#include "ui/Theme.h"

namespace lynx
{
class AudioEngine;

// The app-wide options of Settings › General. MainComponent owns most of them (they were Options menu items);
// the page reads them through SettingsHooks::getGeneral and hands every change back through setGeneral.
struct GeneralOptions
{
    juce::String language;      // i18n id ("en", "pt-BR")
    int uiScale = 100;          // percent
    bool compactMenus = true;   // menu density: compact (native-like rows) or comfortable (round 163)
    bool waveSpectral = false, waveStereo = false;
    bool splash = true, startupSound = true, scanAtStart = true;
    bool menuEntry = false;     // Linux AppImage: LYNX in the applications menu + the .lynx file type (round 161)
    bool typingPiano = false, keepEditorsOpen = false, rememberSeek = false;
    int editPatternsIn = 0;     // 0 = Piano Roll when it has notes, 1 = Channel Rack, 2 = Piano Roll
    int undoLevels = prefs::defaultUndoLevels;
    int autosaveMinutes = prefs::defaultAutosaveMinutes; // 0 = never
    int autosaveKeep = prefs::defaultAutosaveKeep;
    bool askUnusedRecordings = true; // offer to delete takes a project no longer uses when it closes
    bool pluginSandbox = false;
    bool pluginSubstitutes = true; // missing plugins play a LYNX equivalent (core/PluginSubstitutes.h)
    int crashReports = 0;          // crashreport::Consent: 0 ask, 1 always send, 2 never (round 159)
    bool reducedEffects = false;
    bool gpuRendering = true;      // Windows: Direct2D (JUCE 8's default) or GDI for every LYNX window (round 157a)
    int pluginStyle = -1;          // −1 automatic (each plugin its own style), else a skin::Style forced everywhere   // plugin editors without glows, blooms or the open animation (ui/PluginSkin.h)

    bool operator== (const GeneralOptions&) const = default;
};

// What the Settings window needs from the main window (all optional: the --snapshot mode passes none).
struct SettingsHooks
{
    std::function<void()> browserFoldersChanged;         // the Browser reloads its folder list
    std::function<GeneralOptions()> getGeneral;
    std::function<void (const GeneralOptions&)> setGeneral; // applies and saves
    std::function<void()> manageShortcuts, managePlugins;
    std::function<void()> midiChanged;                   // the MIDI settings were changed (the engine already has them)
};

// Settings pages (Settings window, ui/SettingsDialog.cpp)
std::unique_ptr<juce::Component> makeGeneralPage (AudioEngine& engine, const SettingsHooks& hooks, bool persistent);
std::unique_ptr<juce::Component> makeAudioPage (AudioEngine& engine, bool persistent);
std::unique_ptr<juce::Component> makeMidiPage (AudioEngine& engine, const SettingsHooks& hooks, bool persistent);

// The autosave folder (%APPDATA%\LYNX\autosave).
juce::File autosaveFolder();

namespace settingsui
{
// a section title with a hairline under it
class SectionTitle final : public juce::Component
{
public:
    explicit SectionTitle (juce::String t = {}) : text (std::move (t)) {}
    void setText (const juce::String& t) { text = t; repaint(); }

    void paint (juce::Graphics& g) override
    {
        g.setFont (theme::font (theme::type::small, true));
        g.setColour (theme::colour::textSecondary);
        g.drawText (text.toUpperCase(), 0, 0, getWidth(), getHeight() - 6, juce::Justification::bottomLeft);
        g.setColour (theme::colour::border);
        g.fillRect (0, getHeight() - 2, getWidth(), 1);
    }

private:
    juce::String text;
};

inline juce::Label& styleLabel (juce::Label& label, const juce::String& text, bool secondary = true)
{
    label.setText (text, juce::dontSendNotification);
    label.setFont (theme::font (theme::type::body));
    label.setColour (juce::Label::textColourId, secondary ? theme::colour::textSecondary : theme::colour::textPrimary);
    return label;
}

inline void styleNote (juce::Label& label, const juce::String& text)
{
    styleLabel (label, text);
    label.setFont (theme::font (theme::type::small));
    label.setColour (juce::Label::textColourId, theme::colour::textDisabled);
    label.setJustificationType (juce::Justification::topLeft);
}

inline void styleEditor (juce::TextEditor& editor)
{
    editor.setFont (theme::font (theme::type::body));
    editor.setIndents (8, 5);
}

// A vertical page that scrolls when the window is short: rows are laid out by `layout(width)`, which returns the
// content height.
class ScrollingPage : public juce::Component
{
public:
    ScrollingPage()
    {
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, false);
        viewport.setScrollBarThickness (10);
        addAndMakeVisible (viewport);
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        const auto width = getWidth() - (needsScroll() ? viewport.getScrollBarThickness() : 0);
        content.setSize (width, juce::jmax (getHeight(), layout (width)));
    }

protected:
    virtual int layout (int width) = 0;
    juce::Component& page() noexcept { return content; }

    void add (juce::Component& c) { content.addAndMakeVisible (c); }

private:
    bool needsScroll() { return layout (getWidth()) > getHeight(); }

    juce::Viewport viewport;
    juce::Component content;
};
} // namespace settingsui
} // namespace lynx
