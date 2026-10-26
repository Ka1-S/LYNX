#pragma once

#include <memory>
#include <optional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "core/ThemePalette.h"

// User themes (round 162d, issues #8 / #9): Options › Theme. The theme lives in theme.lynxtheme in the settings folder
// (the same JSON as an exported .lynxtheme file) and applies live: the colour tokens of ui/Theme.h change, the stock
// widgets' colours follow, colours components set from the old tokens are replaced and every window repaints.
namespace lynx::themeui
{
constexpr const char* fileExtension = ".lynxtheme";

const themes::Spec& current();
// Applies a theme to the running app (and saves it as the user's theme when `save`).
void apply (const themes::Spec& spec, bool save = true);
// Reads the saved theme and sets the tokens (call once at start, before the first window).
void loadSaved();

// .lynxtheme files (JSON: name, accent, background, workspace, image, fit, imageOpacity, dim).
juce::String toJson (const themes::Spec& spec);
std::optional<themes::Spec> fromJson (const juce::String& json);
bool saveFile (const themes::Spec& spec, const juce::File& file);
std::optional<themes::Spec> loadFile (const juce::File& file);
juce::File userThemesFolder(); // <settings>/Themes

// Something that caches drawings in theme colours can listen (sent on the message thread after a change).
juce::ChangeBroadcaster& changes();

// The workspace backdrop (the area behind docked/floating panels): the theme's colour and optional image.
void paintWorkspaceBackdrop (juce::Graphics& g, juce::Rectangle<int> area);

// The Settings page.
std::unique_ptr<juce::Component> makeThemePage();
} // namespace lynx::themeui
