#pragma once

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

// True when `window` is currently owned by the main window (Windows; elsewhere: keepAboveMainWindow was called).
bool isOwnedByMainWindow (juce::Component& window);
// True when `window` is above the main window in the z-order (Windows; elsewhere: isOwnedByMainWindow).
bool isAboveMainWindow (juce::Component& window);

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

// The LYNX logo (256 px, with transparency).
juce::Image logoImage();

// The CPU time this process has used so far, in seconds (user + kernel; --editorcpu).
double processCpuSeconds();
} // namespace lynx
