#!/usr/bin/env bash
# Cross build of LYNX.exe (Windows x64) from Linux/WSL using Zig (clang + MinGW-w64).
# Needs neither Visual Studio nor CMake. The official build is still CMakeLists.txt.
#
# Variables: ZIG (path to zig), JUCE_DIR (JUCE 8 checkout), OUT (output folder), JOBS
set -euo pipefail
cd "$(dirname "$0")/.."

ZIG="${ZIG:-$HOME/opt/zig-x86_64-linux-0.14.1/zig}"
JUCE_DIR="${JUCE_DIR:-$HOME/opt/juce-8.0.4}"
OUT="${OUT:-build-win}"
JOBS="${JOBS:-$(nproc)}"
TARGET=x86_64-windows-gnu

# Single version source: project(LYNX VERSION x.y.z) in CMakeLists.txt
VERSION="$(sed -n 's/^project(LYNX VERSION \([0-9]*\.[0-9]*\.[0-9]*\).*/\1/p' CMakeLists.txt)"
[[ -n "$VERSION" ]] || { echo "could not read the version from CMakeLists.txt" >&2; exit 1; }

mkdir -p "$OUT/juce" "$OUT/app" "$OUT/shim"

# MinGW headers are lowercase; JUCE includes some with capitals (on Windows it does not matter).
echo '#include <dbghelp.h>' > "$OUT/shim/Dbghelp.h"

MODULES=(juce_core juce_data_structures juce_events juce_graphics juce_gui_basics juce_gui_extra
         juce_audio_basics juce_audio_devices juce_audio_formats juce_audio_processors juce_audio_utils juce_dsp)

{
    echo '#pragma once'
    echo '#define JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED 1'
    echo '#define JUCE_STANDALONE_APPLICATION 1'
    echo '#define JUCE_USE_CURL 0'
    echo '#define JUCE_WEB_BROWSER 0'
    echo '#define JUCE_USE_WINDOWS_MEDIA_FORMAT 0'
    echo '#define JUCE_PLUGINHOST_VST3 1'
    echo '#define JUCE_ASIO 1'
    echo '#define JUCE_USE_MP3AUDIOFORMAT 0' # MP3: src/engine/Mp3Format (minimp3), round 157b
    echo '#define JUCE_APPLICATION_NAME_STRING "LYNX"'
    echo "#define JUCE_APPLICATION_VERSION_STRING \"$VERSION\""
    for m in "${MODULES[@]}"; do echo "#define JUCE_MODULE_AVAILABLE_$m 1"; done
} > "$OUT/JuceConfig.h.new"
# only replace the file if it changed, so the module cache is not invalidated
cmp -s "$OUT/JuceConfig.h.new" "$OUT/JuceConfig.h" 2>/dev/null && rm "$OUT/JuceConfig.h.new" || mv "$OUT/JuceConfig.h.new" "$OUT/JuceConfig.h"

COMMON=(-target "$TARGET" -std=c++20 -O2 -DNDEBUG=1 -fno-sanitize=undefined
        -include "$OUT/JuceConfig.h" -include cmake/mingw/mingw_compat.h
        -I"$OUT/shim" -I"$JUCE_DIR/modules" -I"$JUCE_DIR/modules/juce_audio_processors/format_types/VST3_SDK" -Ithird_party/clap/include -Ithird_party/wavpack/include -Ithird_party/vorbis/include -Ithird_party/lame/include -Ithird_party/asiosdk/common -Ithird_party/sse2neon -Isrc)

export ZIG OUT JUCE_DIR TARGET
export COMMON_STR="${COMMON[*]}"

echo "Compiling JUCE modules (cached)..."
for m in "${MODULES[@]}"; do echo "$m"; done | xargs -P "$JOBS" -I{} bash -c '
    m={}; obj="$OUT/juce/$m.o"
    if [[ ! -f "$obj" || "$OUT/JuceConfig.h" -nt "$obj" ]]; then
        echo "  JUCE $m"
        extra=""
        # JUCE ASIO uses MSVC __try/__except: clang accepts it with -fms-extensions, but on MinGW it does not catch
        # (a driver crashing while being opened/closed is not contained in this build; the MSVC build keeps the guard)
        [[ "$m" == juce_audio_devices ]] && extra="-fms-extensions -DJUCE_DONT_AUTOLINK_TO_WIN32_LIBRARIES=1"
        $ZIG c++ $COMMON_STR $extra -w -c "$JUCE_DIR/modules/$m/$m.cpp" -o "$obj"
    fi'

# Extra units that JUCE's CMake compiles along with the modules
for f in juce_core/juce_core_CompilationTime.cpp juce_graphics/juce_graphics_Harfbuzz.cpp; do
    obj="$OUT/juce/$(basename "$f" .cpp).o"
    if [[ ! -f "$obj" || "$OUT/JuceConfig.h" -nt "$obj" ]]; then
        echo "  JUCE $(basename "$f")"
        "$ZIG" c++ $COMMON_STR -w -c "$JUCE_DIR/modules/$f" -o "$obj"
    fi
done
obj="$OUT/juce/juce_graphics_Sheenbidi.o"
if [[ ! -f "$obj" ]]; then
    echo "  JUCE juce_graphics_Sheenbidi.c"
    "$ZIG" cc -target "$TARGET" -O2 -w -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 -I"$JUCE_DIR/modules" -c "$JUCE_DIR/modules/juce_graphics/juce_graphics_Sheenbidi.c" -o "$obj"
fi

APP_SOURCES=(src/Main.cpp
             src/engine/AudioEngine.cpp src/engine/ChannelRackProcessor.cpp src/engine/Metronome.cpp
             src/engine/SampleLibrary.cpp src/engine/WavPackFormat.cpp src/engine/Mp3Format.cpp src/engine/ProjectFile.cpp src/engine/ProjectPackage.cpp src/engine/PluginHost.cpp src/engine/PluginSandbox.cpp src/engine/Exporter.cpp src/engine/StreamPrefetcher.cpp src/engine/DllAsioDevice.cpp src/engine/PipeWireDevice.cpp src/platform/LinuxRealtime.cpp src/engine/ClapFormat.cpp src/engine/Vst2Format.cpp src/engine/MidiFileIo.cpp src/engine/AudioLogger.cpp src/engine/PreviewPlayer.cpp
             src/ui/MainComponent.cpp src/ui/ChannelRackComponent.cpp src/ui/PianoRollComponent.cpp src/ui/PlaylistComponent.cpp src/ui/MixerComponent.cpp src/ui/PluginWindow.cpp src/ui/PluginPresetMenu.cpp src/ui/ProjectLoading.cpp src/ui/WindowOwner.cpp src/ui/ParametricEqEditor.cpp src/ui/FacetEqEditor.cpp src/ui/ProEditors.cpp src/ui/PluginHeader.cpp src/ui/PluginSkin.cpp src/ui/ExportDialog.cpp src/ui/AutomationUi.cpp src/ui/ChannelSettings.cpp src/ui/BrowserComponent.cpp src/ui/Theme.cpp src/ui/Icons.cpp src/ui/I18n.cpp src/ui/EditorTools.cpp
             src/core/Song.cpp src/core/Scheduler.cpp src/core/Instruments.cpp src/core/RackRenderer.cpp src/core/LoadTrace.cpp src/core/ModuleWarmup.cpp
             src/core/FlpImport.cpp src/core/Effects.cpp src/core/NoteEdit.cpp src/core/Recorder.cpp src/core/TakeRecording.cpp src/core/Pdc.cpp src/core/WorkerPool.cpp src/core/TimeStretch.cpp src/core/Arpeggiator.cpp src/core/SynthPresets.cpp src/core/Slicer.cpp src/core/Templates.cpp src/core/FlWrapper.cpp src/core/FlEffects.cpp src/core/Effector.cpp src/core/VintagePhaser.cpp src/core/EmphasizerFx.cpp src/core/FreqShifterFl.cpp src/core/LowLifterFl.cpp src/core/Reeverb2.cpp src/core/LuxeVerb.cpp src/core/IldspMath.cpp src/core/ConvolverFx.cpp src/core/EquoFx.cpp src/core/PitchShifterFl.cpp src/core/SimSynth.cpp src/core/Wasp.cpp src/core/Granulizer.cpp src/core/Drumaxx.cpp src/core/FruityFilters.cpp src/core/NewtoneFx.cpp src/core/VocalPitch.cpp src/core/LynxTune.cpp src/core/LynxVerbs.cpp src/core/MixSuite.cpp src/core/VoiceMatch.cpp src/core/EchoSuite.cpp src/core/LovePhilter.cpp src/core/DistructorFx.cpp src/core/MultibandDelayFx.cpp src/core/FrequencySplitter.cpp src/core/Slayer.cpp src/core/FlangusFastDist.cpp src/core/FruityPhaser.cpp src/core/FruityDelayBank.cpp src/core/FruityMultiband.cpp src/core/Vocodex.cpp src/core/FruityVocoder.cpp src/core/DelphiMath.cpp src/core/MaximusFx.cpp src/core/ParametricEqFl.cpp src/core/ParametricEqLinear.cpp src/core/FruityBasicFx.cpp src/core/TransistorBass.cpp src/core/GrossBeat.cpp src/core/StereoFx.cpp src/core/FruityDelay.cpp src/core/FruityShapers.cpp src/core/ChorusFx.cpp src/core/FruityDynamics.cpp src/core/FlSamplers.cpp src/core/FlSynths.cpp src/core/FlSytrus.cpp src/core/FlGenerators.cpp src/core/PhysicalModels.cpp src/core/DrumSynthLive.cpp src/core/SytrusEngine.cpp src/core/Harmless.cpp src/core/HarmlessFx.cpp src/core/Harmor.cpp src/core/Gms.cpp src/core/Morphine.cpp src/core/Ogun.cpp src/core/Toxic.cpp src/core/ToxicFx.cpp src/core/FruitKick.cpp src/core/BassDrum.cpp src/core/MiniSynth.cpp src/core/Dx10.cpp src/core/Kepler.cpp src/core/PoiZone.cpp src/core/FlPatcher.cpp src/core/Patcher.cpp src/core/Vfx.cpp src/core/VfxSequencer.cpp src/core/Inflate.cpp src/core/WavPack.cpp src/flp/Flp.cpp src/core/EmberClassic.cpp src/core/RhythmPan.cpp src/core/VoxStrip.cpp
             src/platform/MingwFixes.cpp)

# workflow shell: core (key map, workspace layout, rack operations) and UI (workspace desktop, control contract)
APP_SOURCES+=(src/core/KeyMap.cpp src/core/Workspace.cpp src/core/RackOps.cpp src/core/ChannelMiscFx.cpp src/core/MidiImport.cpp src/ui/Workspace.cpp src/ui/ValueControl.cpp src/ui/Dialogs.cpp)
# Windows file association (.lynx) and its icon
APP_SOURCES+=(src/platform/FileTypes.cpp src/platform/Paths.cpp src/platform/NativeEmbed.cpp)
# SEH guard around calls into hosted plugins (round 111): the only unit built with -fms-extensions (__try / __except)
APP_SOURCES+=(src/platform/CrashGuard.cpp)
# workflow P2: core (note tools, mixer operations, history labels, browser search)
APP_SOURCES+=(src/core/NoteTools.cpp src/core/PianoRollTools.cpp src/core/EventEdit.cpp src/core/AudioEdit.cpp src/core/MixerOps.cpp src/core/History.cpp src/core/SearchQuery.cpp src/core/SearchIndex.cpp src/core/SearchService.cpp src/core/BrowserView.cpp src/core/EffectPresets.cpp src/core/PluginPresets.cpp src/core/ParamHelp.cpp)
# workflow P2: UI (note tool dialogs, tool windows, undo history, quick-add palette)
APP_SOURCES+=(src/ui/NoteToolDialogs.cpp src/ui/EventEditor.cpp src/ui/AudioEditor.cpp src/ui/ToolWindow.cpp src/ui/TempoTapper.cpp src/ui/HistoryPanel.cpp src/ui/QuickAddPalette.cpp)
# start-up: splash screen and its procedurally synthesised sound
APP_SOURCES+=(src/core/StartupSound.cpp src/ui/Splash.cpp)
# main window chrome: hint bar and toolbar widgets
APP_SOURCES+=(src/ui/HintBar.cpp src/ui/Toolbar.cpp)
# FL's 3x Osc engine (REA round 47)
APP_SOURCES+=(src/core/ThreeOsc.cpp)
# onboarding: FL Studio integration (plugin database, sample relocation), background scan, setup wizard
APP_SOURCES+=(src/core/FlPluginDb.cpp src/engine/FlStudio.cpp src/ui/SetupWizard.cpp)
APP_SOURCES+=(src/core/PluginCatalog.cpp src/ui/PluginPicker.cpp)
APP_SOURCES+=(src/core/MissingFiles.cpp src/core/ProjectSafety.cpp src/ui/MissingFilesDialog.cpp src/ui/SettingsDialog.cpp)
# LYNX substitutes for missing third-party plugins (round 98)
APP_SOURCES+=(src/core/PluginSubstitutes.cpp)
# settings pages (General/Audio/MIDI), plugin manager, shortcut manager (round 82)
APP_SOURCES+=(src/core/ThemePalette.cpp src/ui/ThemeSettings.cpp src/ui/ColourPicker.cpp)
APP_SOURCES+=(src/core/Preferences.cpp src/ui/SettingsPages.cpp src/ui/PluginManager.cpp src/ui/ShortcutManager.cpp)
# spectral waveforms: band analysis, background cache and drawing
APP_SOURCES+=(src/core/WaveAnalysis.cpp src/ui/WaveformView.cpp)
# FL's Plucked!, Sawer and BeepMap engines (REA round 65)
APP_SOURCES+=(src/core/Plucked.cpp src/core/BeepMap.cpp src/core/Sawer.cpp)
# LYNX Soundfont: SF2 / SF3 player with a preset search (round 158, issue #6)
APP_SOURCES+=(src/core/SoundFont.cpp src/core/SoundFontLoad.cpp src/core/SoundFontIndex.cpp src/ui/SoundFontEditor.cpp)
APP_SOURCES+=(src/ui/VfxEditors.cpp)
# FL's sampler interpolation (REA round 69)
APP_SOURCES+=(src/core/FlResampler.cpp src/core/FlKeys.cpp)
# FL's Hardcore (REA round 71)
APP_SOURCES+=(src/core/HardcoreFx.cpp)
# LYNX's Facet EQ and Apex Limiter (round 99)
APP_SOURCES+=(src/core/FacetEq.cpp src/core/ApexLimiter.cpp)
# LYNX's dynamics suite and FabFilter state translation (round 108)
APP_SOURCES+=(src/core/DynamicsSuite.cpp src/core/FabFilterStates.cpp src/ui/DynamicsDisplays.cpp)
# LYNX's voice tools: Quell Denoiser, Gemini Doubler (round 119)
APP_SOURCES+=(src/core/VoiceTools.cpp)
# MIDI CC automation of hosted plugins, Formula Controller (round 133, issue #3)
APP_SOURCES+=(src/core/MidiControls.cpp src/core/FormulaController.cpp src/ui/ControllerEditors.cpp)
APP_SOURCES+=(src/core/VoiceShift.cpp)
# FL engines usable in native projects (round 142, issue #5)
APP_SOURCES+=(src/core/NativeEngines.cpp src/ui/NewtoneEditor.cpp)
APP_SOURCES+=(src/core/Layer.cpp src/ui/LayerEditor.cpp) # FL's Layer channel (round 164)
APP_SOURCES+=(src/core/XyController.cpp src/core/FlCurve.cpp src/core/ControlSurface.cpp src/ui/Controllers164.cpp) # X-Y (-Z) Controller, Control Surface, Dashboard (round 164)
APP_SOURCES+=(src/core/VoltageController.cpp src/ui/VoltageEditor.cpp) # Voltage Controller (round 164)
# FL's DirectWave (REA round 72)
APP_SOURCES+=(src/core/DirectWave.cpp)
# Ogg Vorbis decoding as FL does (REA round 77)
APP_SOURCES+=(src/core/OggVorbis.cpp)
# export options + MP3, plugin performance monitor, clip properties / arrangements, new project, touch keyboard (round 84)
APP_SOURCES+=(src/core/ExportOptions.cpp src/core/Mp3Encoder.cpp src/core/Mp3Decoder.cpp src/core/PluginMonitor.cpp src/core/PlaylistOps.cpp src/core/ProjectContents.cpp src/core/TempoDetect.cpp)
APP_SOURCES+=(src/ui/NewProjectDialog.cpp src/ui/ClipProperties.cpp src/ui/PluginMonitorWindow.cpp src/ui/TouchKeyboard.cpp)
# native plugin editors in FL's arrangement (round 93)
APP_SOURCES+=(src/core/PluginLayout.cpp src/core/StateParams.cpp src/ui/PluginEditors.cpp src/ui/PatcherEditor.cpp)
# crash / freeze reports: consent, anonymiser, upload, GitHub form (round 159)
APP_SOURCES+=(src/core/CrashReport.cpp src/ui/CrashReporter.cpp)
# visual plugins: Wave Candy, Visualizer, video export (round 164c)
APP_SOURCES+=(src/core/VisualFx.cpp src/ui/VisualEditors.cpp)

echo "WavPack (decoder, C)..."
mkdir -p "$OUT/wavpack"
for f in third_party/wavpack/src/*.c; do
    obj="$OUT/wavpack/$(basename "$f" .c).o"
    if [[ ! -f "$obj" || "$f" -nt "$obj" ]]; then
        "$ZIG" cc -target "$TARGET" -O2 -w -DHAVE___BUILTIN_CLZ=1 -Ithird_party/wavpack/include -Ithird_party/wavpack/src -c "$f" -o "$obj"
    fi
done

echo "libogg + libvorbis (decoder, C)..."
mkdir -p "$OUT/vorbis"
for f in third_party/vorbis/src/*.c; do
    [[ "$(basename "$f")" == lookup.c ]] && continue # (included by lsp.c)
    obj="$OUT/vorbis/$(basename "$f" .c).o"
    if [[ ! -f "$obj" || "$f" -nt "$obj" ]]; then
        "$ZIG" cc -target "$TARGET" -O2 -w -ffp-contract=off -include third_party/vorbis/include/lynx_vorbis_prefix.h -Ithird_party/vorbis/include -Ithird_party/vorbis/src -c "$f" -o "$obj"
    fi
done

echo "LAME libmp3lame (MP3 encoder, C)..."
mkdir -p "$OUT/lame"
for f in third_party/lame/src/*.c; do
    obj="$OUT/lame/$(basename "$f" .c).o"
    if [[ ! -f "$obj" || "$f" -nt "$obj" ]]; then
        "$ZIG" cc -target "$TARGET" -O2 -w -DHAVE_CONFIG_H -Ithird_party/lame/include -Ithird_party/lame/src -c "$f" -o "$obj"
    fi
done

# the commit the build comes from (crash reports name it)
COMMIT="$(git rev-parse --short=10 HEAD 2>/dev/null || true)"
[[ -n "$(git status --porcelain --untracked-files=no 2>/dev/null)" ]] && COMMIT="$COMMIT-dirty"
printf '#pragma once\n#define LYNX_GIT_COMMIT "%s"\n' "$COMMIT" > "$OUT/lynx_build.h"

echo "Compiling the app..."
printf '%s\n' "${APP_SOURCES[@]}" | xargs -P "$JOBS" -I{} bash -c '
    src={}; obj="$OUT/app/$(echo "$src" | tr / _).o"
    extra=""; [[ "$src" == src/platform/CrashGuard.cpp ]] && extra="-fms-extensions"
    [[ "$src" == src/ui/CrashReporter.cpp ]] && extra="-iquote $OUT"
    $ZIG c++ $COMMON_STR $extra -Wall -Wextra -c "$src" -o "$obj"'

echo "Resources (manifest, icons, version $VERSION)..."
OUT_ABS="$(cd "$OUT" && pwd)"
IFS=. read -r VMAJ VMIN VPAT <<< "$VERSION"
{
    echo '#pragma once'
    echo "#define LYNX_VERSION_MAJOR $VMAJ"
    echo "#define LYNX_VERSION_MINOR $VMIN"
    echo "#define LYNX_VERSION_PATCH $VPAT"
    echo "#define LYNX_VERSION_STR \"$VERSION\""
} > "$OUT/lynx_version.h"
( cd resources && "$ZIG" rc /i "$OUT_ABS" /fo "$OUT_ABS/LYNX.res" LYNX.rc )

echo "Linking..."
"$ZIG" c++ -target "$TARGET" -O2 -o "$OUT/LYNX.exe" "$OUT"/juce/*.o "$OUT"/app/*.o "$OUT"/wavpack/*.o "$OUT"/vorbis/*.o "$OUT"/lame/*.o "$OUT/LYNX.res" \
    -Wl,--subsystem,windows \
    -lole32 -loleaut32 -luuid -lwinmm -lws2_32 -lshlwapi -lversion -lwininet -limm32 -lcomdlg32 \
    -lgdi32 -luser32 -lcomctl32 -lshell32 -ladvapi32 -lrpcrt4 -ldbghelp -lsetupapi -luxtheme -ldwmapi \
    -ld2d1 -ldcomp -ldwrite -ldxgi -ld3d11 -ldxguid -lwsock32 -lopengl32 -lpropsys -lmfplat -lmfuuid -lksuser -lavrt

ls -la "$OUT/LYNX.exe"

# LYNX ASIO driver (REA round 97), shipped next to LYNX.exe
OUT="$OUT" ZIG="$ZIG" scripts/build-lynx-asio.sh
