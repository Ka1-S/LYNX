#pragma once

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/AudioEngine.h"
#include "ui/SettingsPages.h"

namespace lynx
{
// The Settings window (FL Studio's Settings pages, interpreted): Audio (device, driver, sample rate, buffer, status),
// MIDI (input devices, velocity, controllers), General (language, display, startup, editing, backup, plugins and
// shortcuts), Files (the folders LYNX searches for missing samples, the Browser's folders and the plugin folders),
// Project (tempo, time signature, swing, master pitch) and Info (title, author, genre, web link, comments).
// Project and Info edit the song: every change is an undo step. The others edit the app's settings at once.
enum class SettingsPage { project, info, files, general, audio, midi, theme };

void showSettings (AudioEngine& engine, SettingsPage page, SettingsHooks hooks = {});

// The content, for --snapshot mode (`persistent` false: sample folders and devices, nothing saved).
std::unique_ptr<juce::Component> makeSettingsPanel (AudioEngine& engine, SettingsPage page, bool persistent = false, SettingsHooks hooks = {});
} // namespace lynx
