#pragma once

#include <memory>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

#include "engine/AudioEngine.h"

namespace lynx
{
// Window with a plugin's own UI. Keeps the plugin alive while
// it is open (even if the channel is removed), and only one window per plugin.
namespace PluginWindows
{
// Where the plugin's wrapper options live (REA round 146a: FL's plugin wrapper settings page). With an engine, the
// window gets FL's wrapper header: the plugin's own editor and a "Wrapper settings" page with its processing, bridging
// and MIDI options and what the plugin reports. `find` locates the PluginRef in the song (nullptr: gone).
struct WrapperContext
{
    AudioEngine* engine = nullptr;
    std::function<PluginRef* (Song&)> find;
    bool channel = false; // a Channel rack plugin (the MIDI output port applies)
};
// `owner` is the adapter (PluginInstrument/PluginEffect) that holds the plugin.
void open (std::shared_ptr<void> owner, juce::AudioPluginInstance& plugin, const juce::String& title, WrapperContext context = {});
void closeAll();
// Closes now the windows whose plugin is no longer in the song (removed, replaced, its channel deleted); a timer
// also does it on its own while a window is open.
void closeOrphans();
// The plugins with an open window, the one used last first (the Browser applies a preset to it).
std::vector<juce::AudioPluginInstance*> openPlugins();
bool isOpen (const juce::AudioPluginInstance& plugin);
void close (const juce::AudioPluginInstance& plugin);
// A parameter gesture in an open plugin editor (`starting` = begin, else end), on the UI thread; the plugin is only
// compared, it may be gone (REA round 143: Last tweaked, automation recording).
extern std::function<void (const juce::AudioPluginInstance&, int, bool)> onGesture;
} // namespace PluginWindows

// Scans the VST3 folders with a progress window. `onDone(newCount)` runs on the UI thread.
void scanPluginsWithProgress (AudioEngine& engine, std::function<void (int)> onDone);

// FL's wrapper processing options for one hosted plugin ("Use fixed size buffers", "Make bridged"): toggling one
// keeps the plugin's current state, closes its window and creates it again. `find` locates the PluginRef in the song
// (nullptr: gone); `instance` is the running plugin (may be null).
juce::PopupMenu pluginProcessingMenu (AudioEngine& engine, const PluginRef& current, juce::AudioPluginInstance* instance,
                                      std::function<PluginRef* (Song&)> find);

// Menus of known plugins (instruments or effects): the favourites, then "More plugins..." (the plugin picker) and
// the scan. `onChosen` receives the reference; from the picker (a window of its own, chosen later) it is only
// called while `owner` (the component whose menu this is) still exists.
// `onBuiltIn`: the picker lists LYNX's built-in note effects too (core/Vfx.h) and hands a new channel of one over.
void addPluginItems (juce::PopupMenu& menu, AudioEngine& engine, bool instruments, std::function<void (PluginRef)> onChosen,
                     juce::Component* owner = nullptr, std::function<void (ChannelDef)> onBuiltIn = nullptr);
} // namespace lynx
