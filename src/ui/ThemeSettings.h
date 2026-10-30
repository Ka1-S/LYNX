#pragma once

#include <functional>
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

// .lynxtheme files (JSON: name, accent, background, workspace, image, fit, imageOpacity, dim, startupSound).
juce::String toJson (const themes::Spec& spec);
std::optional<themes::Spec> fromJson (const juce::String& json);
bool saveFile (const themes::Spec& spec, const juce::File& file);
std::optional<themes::Spec> loadFile (const juce::File& file);
juce::File userThemesFolder(); // <settings>/Themes

// Issue #12: workspace images live in a "Backgrounds" folder next to LYNX.exe (a portable LYNX keeps them when moved;
// the settings folder's "Backgrounds" when LYNX's own folder can't be written; the test profile's under
// LYNX_PROFILE_DIR). A theme names an image there by its relative path.
juce::File backgroundsFolder();
juce::File imageFile (const std::string& image); // the theme's image path as a file (relative: in backgroundsFolder)
std::string imagePathFor (const juce::File& file); // how a theme stores this image (relative when in the folder)
// Asks for a workspace image (PNG, JPEG, GIF), copies it into backgroundsFolder() unless it is there, and calls `done`
// with the file (not called when cancelled or unreadable). Options › Theme and FL's View › Background use it.
void chooseWorkspaceImage (std::function<void (const juce::File&)> done);
// FL's View › Background (audit round 165z-a): "Set image wallpaper..." applies and saves the chosen image at once.
void setImageWallpaper();
void removeImageWallpaper();
// Options › Theme › Startup sound: plays a startup sound now (path empty = LYNX's own). The main window sets it.
extern std::function<void (const juce::String& path)> previewStartupSound;

// Something that caches drawings in theme colours can listen (sent on the message thread after a change).
juce::ChangeBroadcaster& changes();

// The workspace backdrop (the area behind docked/floating panels): the theme's colour and optional image.
void paintWorkspaceBackdrop (juce::Graphics& g, juce::Rectangle<int> area);

// The Settings page.
std::unique_ptr<juce::Component> makeThemePage();
} // namespace lynx::themeui
