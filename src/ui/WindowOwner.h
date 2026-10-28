#pragma once

#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

namespace lynx
{
// Secondary windows (plugin, channel settings, EQ, automation) always stay above the LYNX main window,
// without staying above other programs: on Windows, the main window becomes their "owner".
void setMainWindowForOwnership (juce::Component* mainWindow);
void keepAboveMainWindow (juce::Component& window); // call after the window is visible; sets the LYNX icon
// (it stays owned when JUCE recreates the native window later, e.g. setResizable after it is shown)
// The main window's native handle (HWND / X11 Window id; nullptr without one): a plugin's window in a separate
// process is kept above it (round 162d).
void* mainWindowHandle();

// Keys a secondary window does not use (no component in it took them) go to the main window's global shortcuts, as in
// FL (round 165q / issue #12: Space, F12 or a user's key did nothing while the Undo history, Settings, a plugin window...
// had the focus). keepAboveMainWindow installs the forwarding on the window; the main window sets the handler. A window
// that handles every key itself (a detached panel) sets the property "lynxOwnKeys".
void setSecondaryWindowKeyHandler (std::function<bool (const juce::KeyPress&)> handler);

// Before a drag (a sample from the Browser...): JUCE finds the drop target in its own list of windows, where the main
// window comes first after any click in it, while the windows kept above it (channel settings, a plugin...) are on top
// on the screen. This puts those windows back above the main window in that list, in their screen order, so a drop on
// one of them lands there and not on the panel under it (issue #12: a sample dropped on the Sampler's waveform
// replaced the sample of the Channel Rack row under the window).
void matchWindowOrderToScreen();

// Makes the main window the active one (round 165q): a closing secondary window that is active calls it first, so
// the activation does not go to another program.
void activateMainWindow();

// True when `window` is currently owned by the main window (Windows; elsewhere: keepAboveMainWindow was called).
bool isOwnedByMainWindow (juce::Component& window);
// True when `window` is above the main window in the z-order (Windows; elsewhere: isOwnedByMainWindow).
bool isAboveMainWindow (juce::Component& window);

// Keeps a top-level window reachable (round 163, UI zoom 150%+): a resizable window larger than its screen shrinks
// to it, and the window moves so its title bar and close button are on the screen. keepAboveMainWindow does it the
// first time; the UI scale change does it for every LYNX window.
void fitWindowOnScreen (juce::Component& window);
void fitAllWindowsOnScreen();

// Round 164: the content of a fixed-size window (a tool window, a dialog) that is taller or wider than the usable
// screen (UI zoom 150%+, a small laptop screen) goes into a scrolling viewport that fits, so its buttons stay
// reachable; a content that fits is returned as it is. The viewport follows the content's later size changes.
std::unique_ptr<juce::Component> fitContentToScreen (std::unique_ptr<juce::Component> content);

// --windowtest only: makes `window` the foreground window even when LYNX was started in the background.
void forceForegroundForTest (juce::Component& window);

// Native window chrome in the LYNX colours (dark title bar on Windows). keepAboveMainWindow calls it.
void applyWindowChrome (juce::Component& window);

// GPU rendering (round 157a). JUCE 8 draws every Windows window with Direct2D (the GPU driver's D3D11 path); a
// driver that crashes there (Intel igd10um64xe.dll on a hybrid-graphics laptop) takes LYNX down. Off = every LYNX
// window draws with GDI (the CPU), as JUCE 7 did. Saved in gpu-rendering.txt ("0" = off; default on). The change
// applies to open windows at once (a watcher re-applies it to new windows twice a second).
bool gpuRenderingEnabled();
void setGpuRenderingEnabled (bool on);
// starts the watcher (the app calls it once at start, after reading the setting)
void startRenderingWatch();
// the window's native peer uses the chosen renderer now (keepAboveMainWindow calls it)
void applyRenderingChoice (juce::Component& window);

// Dialogs with a text field (rename and color, the next empty pattern's name, preset and insert names...) open with
// the field focused and its text selected, so typing replaces the name at once, as in FL (round 165q). JUCE's
// AlertWindow gives its buttons an explicit focus order, so the dialog's focus went to its first button: the letters
// typed did nothing and Enter pressed OK. Installed once for the whole app (a Desktop focus listener): the first time
// the focus enters a dialog, it moves to the dialog's first editable text field when it landed anywhere else.
void installDialogTextFocus();
void removeDialogTextFocus();
// The dialog's first editable, visible text field (top to bottom, then left to right), or nullptr.
juce::TextEditor* firstTextField (juce::Component& dialog);

// The LYNX logo (256 px, with transparency).
juce::Image logoImage();

// The CPU time this process has used so far, in seconds (user + kernel; --editorcpu).
double processCpuSeconds();
} // namespace lynx
