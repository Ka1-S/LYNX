#pragma once

#include "engine/AudioEngine.h"

namespace lynx
{
// FL's "Create DirectWave instrument..." (the channel menu; REA_rodada165f-rack §1.5): the options window, then the
// channel is played key by key off the audio thread (the export engine, with progress and cancel), the takes are
// written as WAV files next to the renders and a new multisampled Sampler channel plays them.
void showAutoSamplerDialog (AudioEngine& engine, int channel);
} // namespace lynx
