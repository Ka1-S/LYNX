#pragma once

// The Video Player's editor and the ffmpeg services behind it (round 165g, core/VideoPlayer.h): the video, decoded by
// the user's own ffmpeg (found as for the video export, ui/VisualEditors findFfmpeg), drawn at the song's time; FL's
// controls (time slider, start / end times, Loop, Play / Stop, Slave playback to host, Mute, Volume, the options menu);
// a detachable video window; the sound track decoded once per video for the instrument.

#include <functional>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

namespace lynx
{
class AudioEngine;
struct ChannelDef;
struct Song;

namespace videoview
{
// Installs the sound-track provider (core/VideoPlayer.h) for `engine`: decodes in the background and republishes the
// song when a track is ready. Call once at start-up.
void install (AudioEngine& engine);
// ffmpeg's report of a video (cached per path; empty when ffmpeg or the file is missing)
juce::String report (const juce::String& path);
} // namespace videoview

// `channel`: the Channel Rack channel (−1 in a Patcher: `locate` finds the node)
std::unique_ptr<juce::Component> makeVideoPlayerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate, int channel);
std::unique_ptr<juce::Component> makeVideoPlayerEditor (AudioEngine& engine, int channel);
} // namespace lynx
