#pragma once

// FL's "Create DirectWave instrument..." (the channel menu, form TDWPRenderForm; REA_rodada165f-rack §4): a sampling robot
// that plays a channel's instrument (any plugin or LYNX engine) key by key and velocity by velocity, records each note
// and makes a multisampled sampler channel of the takes. This is the pure part — the plan of the notes, the song that
// plays them, cutting the render into takes and the sampler zones; the UI renders it with the export engine off the
// audio thread. Our own code from FL's form and options. No JUCE.

#include <cstdint>
#include <string>
#include <vector>

#include "core/Song.h"

namespace lynx::autosample
{
struct Options
{
    int lowKey = 36, highKey = 83; // FL's defaults (C3 … B6 in FL's names)
    int keysPerZone = 4;           // a take every N keys; the zone covers them
    int rootOffset = 2;            // the key played (the zone's root) sits this far above the zone's lowest key
    int velocityLayers = 1;        // takes per key, velocities spread up to maxVelocity
    int maxVelocity = 127;
    double holdSeconds = 2.0;      // how long each note is held ("Release after")
    double maxSeconds = 4.0;       // after the release, the longest a take records ("Max length")
    bool stopOnSilence = true;     // FL's "Stop on: Silence" (else "Max length": every take is hold + max long)
    int channels = 0;              // 0 stereo, 1 mono (merged), 2 mono (left only), 3 mono (right only)
    bool float32 = false;          // 32-bit float samples (else 16-bit int)
    bool declickIn = false, declickOut = true;
    bool normalize = false;        // one gain for every take, so the velocity layers keep their levels
    bool insertEffects = false;    // record through the channel's mixer insert effects
    bool masterEffects = false;    // and the Master's
};

struct Take
{
    int key = 60, velocity = 100;  // what is played (MIDI 0..127)
    int lowKey = 60, highKey = 60; // the zone
    int lowVelocity = 0, highVelocity = 127;
    std::int64_t startTick = 0;    // in the render song
};

struct Plan
{
    std::vector<Take> takes;
    std::int64_t slotTicks = 0, holdTicks = 0; // the time each take gets in the render, the note's length
    double bpm = 120.0;
};

// The options in range (keys 0..127, low <= high, 1..128 keys per zone, 1..16 layers, velocity 1..127, 0.05..60 s).
Options clamped (Options o);

// The takes, in key order then velocity order, at `bpm` (the render plays at the song's tempo).
Plan plan (const Options& options, double bpm);

// Seconds the render lasts.
double renderSeconds (const Plan& plan) noexcept;

// The song that plays the takes: only `channel` (at 0 dB, centred, its loops / arpeggio as they are), a pattern with
// one note per take and a Playlist clip of it; the mixer insert / Master effects only when asked.
Song makeRenderSong (const Song& song, int channel, const Plan& plan, const Options& options);

// One take's samples, cut from the render.
struct Cut
{
    std::vector<float> left, right; // right empty for mono
};
// Cuts the render (`frames` long at `sampleRate`) into the takes: from each note's start to the end of its sound
// (silence below −90 dBFS for 50 ms, or the slot's end) or hold + max length; declick, mono and normalize as asked.
std::vector<Cut> cutTakes (const float* left, const float* right, int frames, double sampleRate, const Plan& plan, const Options& options);

// The sampler zones of the takes (paths[i] = the file of take i).
std::vector<SampleZone> zones (const Plan& plan, const std::vector<std::string>& paths);

// The sampler channel that plays them: the source channel's name + " (sampled)", its volume and pan, and its mixer insert
// unless the takes already went through the insert's effects; notes fade out over a short release after their end.
ChannelDef makeSamplerChannel (const ChannelDef& source, std::vector<SampleZone> zones, const Options& options);

// A file name for a take: "<base> <key name><octave> v<velocity>" (keys as FL names them, C5 = 60).
std::string takeFileName (const std::string& base, const Take& take);
} // namespace lynx::autosample
