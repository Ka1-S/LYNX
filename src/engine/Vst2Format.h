#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include <juce_audio_processors/juce_audio_processors.h>

namespace lynx
{
// VST2 plugin hosting. JUCE 8 only hosts VST2 with Steinberg's (no longer distributed) SDK, so this is a
// host of our own, written from the publicly documented VST 2.4 binary interface (the AEffect structure, the
// dispatcher opcodes and the host callback), as one more juce::AudioPluginFormat: scan, plugin list, plugin
// window, saved state, latency and parameter automation go through the same paths as VST3 and CLAP.
// The saved state is the layout FL Studio's wrapper writes (REA_rodada5 §3.1 / REA_rodada89 §2), so a
// VST2 found for an imported .flp gets the project's settings unchanged.
// Windows only (VST2 binaries are DLLs); elsewhere the format finds nothing.
class Vst2PluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "VST";

    juce::String getName() const override { return formatName; }

    void findAllTypesForFile (juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType (const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier (const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning (const juce::PluginDescription&) override;
    bool doesPluginStillExist (const juce::PluginDescription&) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins (const juce::FileSearchPath& directories, bool recursive, bool allowAsync) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation (const juce::PluginDescription&) const override { return false; }

private:
    void createPluginInstance (const juce::PluginDescription&, double initialSampleRate, int initialBufferSize,
                               PluginCreationCallback) override;
};

// Direct access to a hosted VST2 for its standard preset files (.fxp / .fxb, core/PluginPresets.h). Every function
// fails (nullopt / false) for a plugin that is not a VST2 hosted in this process.
namespace vst2
{
struct Info
{
    std::int32_t uniqueId = 0, version = 0, numPrograms = 0, numParams = 0;
    bool chunks = false; // the plugin saves an opaque chunk (else: its parameter values)
};
std::optional<Info> info (juce::AudioPluginInstance& plugin);
// effGetChunk / effSetChunk: `program` = the current program only, else the whole bank
bool getChunk (juce::AudioPluginInstance& plugin, std::vector<std::uint8_t>& out, bool program);
bool setChunk (juce::AudioPluginInstance& plugin, const std::vector<std::uint8_t>& data, bool program);
} // namespace vst2
} // namespace lynx
