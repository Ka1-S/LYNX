#pragma once

// The Voltage Controller channel's settings (round 164, core/VoltageController.h).

#include "engine/AudioEngine.h"

namespace lynx
{
std::unique_ptr<juce::Component> makeVoltageControllerEditor (AudioEngine& engine, int channel);
std::unique_ptr<juce::Component> makeVoltageControllerEditor (AudioEngine& engine, std::function<ChannelDef* (Song&)> locate); // (a Patcher node)
}
