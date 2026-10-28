#pragma once

#include "engine/AudioEngine.h"

namespace lynx
{
// "Export audio" window: format, sample rate and what to export; then asks for the file and
// renders on a background thread with a progress bar. `suggestedName` = name without extension. `format`: −1 = the
// format(s) of the last export (FL's File › Export › "Last exported format(s)"), 0 WAV, 1 MP3, 2 OGG, 3 FLAC = that
// format alone (FL's "Wave file...", "MP3 file"...; REA round 139).
void showExportDialog (AudioEngine& engine, const juce::File& suggestedFolder, const juce::String& suggestedName, int format = -1);
// FL's Mixer › Disk recording › "Render armed / selected tracks to wave file(s)..." (REA round 165f-mixer): the same
// window, set to one file per insert for `tracks` only (the Master in the list = also the full mix).
void showExportDialogForTracks (AudioEngine& engine, const juce::File& suggestedFolder, const juce::String& suggestedName,
                                const std::vector<int>& tracks);

// "Render to audio" (freeze): the insert becomes an audio clip on a new insert with the same
// routing, and the original is muted. Runs on a background thread with progress.
void bounceInsertToAudio (AudioEngine& engine, int track);
// FL's Playlist track menu "Consolidate track(s)" (REA round 146a): the track's clips are rendered (through the mixer,
// from the track's first clip or from the song start) into one audio clip that takes their place on the track.
void consolidatePlaylistTrack (AudioEngine& engine, int track, bool fromSongStart);

// Panel only (for --snapshot mode; `format`: 0 WAV, 1 MP3, 2 OGG, 3 FLAC with loop/stem options set). Nothing is saved.
std::unique_ptr<juce::Component> makeExportPanel (AudioEngine& engine, int format = 0);
// The panel of showExportDialogForTracks (for --snapshot).
std::unique_ptr<juce::Component> makeExportPanelForTracks (AudioEngine& engine, const std::vector<int>& tracks);
} // namespace lynx
