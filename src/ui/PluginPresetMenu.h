#pragma once

#include <functional>
#include <optional>

#include <juce_audio_processors/juce_audio_processors.h>

#include "core/PluginPresets.h"
#include "engine/AudioEngine.h"

// Presets of hosted plugins from the plugin window (round 162d, issue #8; FL's wrapper: the presets menu and ◀ ▶):
// the plugin's own programs, the user's presets in <settings>/Presets/Plugins/<plugin>/ (LYNX's .lynxpreset for any
// format, VST2 .fxp / .fxb and VST3 .vstpreset dropped there too), Save preset, Load / Export a preset file. The same
// folder is the Browser's "Plugin presets" place.
namespace lynx::pluginpresetui
{
juce::File presetsRoot();                               // <settings>/Presets/Plugins
juce::File folderFor (const juce::String& pluginName);  // that plugin's folder (not created)
bool isPresetFile (const juce::File& file);             // .lynxpreset (of a plugin), .fxp, .fxb, .vstpreset
juce::Array<juce::File> userPresets (const juce::String& pluginName); // sorted by name

// The plugin a .lynxpreset was saved from (nullopt: not a plugin preset).
std::optional<pluginpresets::Preset> readLynxPreset (const juce::File& file);

// Applies a preset file to the plugin. False + `error` (a sentence for the user) when the file is not for this plugin
// or this plugin can't read that format (e.g. a VST2 .fxp chunk for a plugin in a separate process).
bool load (juce::AudioPluginInstance& plugin, const juce::File& file, juce::String& error);
// Writes the plugin's current settings in the file's format (by extension).
bool save (juce::AudioPluginInstance& plugin, const juce::File& file, const juce::String& presetName, juce::String& error);
// The formats `save` can write for this plugin ("*.lynxpreset;*.fxp;*.fxb" …).
juce::String saveWildcard (juce::AudioPluginInstance& plugin);

// Browser: a preset file was double-clicked. Applies it to the plugin it belongs to — an open plugin window's plugin
// (the last one used first), then the selected channel's plugin; a .lynxpreset of an instrument nobody has open
// becomes a new channel with that plugin. `message` receives what happened when nothing could take it.
bool applyFromBrowser (AudioEngine& engine, const juce::File& file, juce::String& message);

// The wrapper header's preset controls: ◀ ▶ and the preset name (click: the presets menu).
class PresetBar final : public juce::Component, public juce::SettableTooltipClient
{
public:
    PresetBar (juce::AudioPluginInstance& plugin, std::function<void()> changed);

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;

    int preferredWidth() const { return 250; }

private:
    struct Entry
    {
        int program = -1; // a factory program, or
        juce::File file;  // a user preset
        juce::String name;
    };
    std::vector<Entry> entries() const;
    void step (int delta);
    void apply (const Entry& entry);
    void showMenu();
    void saveAs();
    void loadFile();
    void exportFile();
    void setName (const juce::String& name);

    juce::AudioPluginInstance& plugin;
    std::function<void()> onChanged;
    juce::TextButton prev { juce::String::fromUTF8 ("\xe2\x97\x80") }, next { juce::String::fromUTF8 ("\xe2\x96\xb6") };
    juce::String current;
    juce::File currentFile;
    int currentProgram = -1;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace lynx::pluginpresetui
