cmake_minimum_required(VERSION 3.22)

# macOS: Big Sur or newer (universal builds pass -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64"); set before project()
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")

project(LYNX VERSION 0.1.19 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The same float results with every compiler and CPU: no fused multiply-add contraction (arm64 and newer x86 have
# FMA; the exact FL engines rely on the plain operation order, as the Windows build compiles them)
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

# FL Slayer's exact engine (x87 arithmetic under its own control word): with GCC 15 at -O1 and above, inlining in this
# file breaks it (some presets silent or NaN against the DLL goldens; Clang and GCC -O0 / -fno-inline are exact; root
# cause not isolated, REA round 117). Linux GCC builds compile it without inlining (Slayer about 1.7x slower there).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|i.86")
    set_source_files_properties(src/core/Slayer.cpp PROPERTIES COMPILE_OPTIONS "-fno-inline")
endif()

# ---------------------------------------------------------------------------
# JUCE-free core: model, sequencer, built-in instruments and .flp reader

add_library(lynx_core STATIC
    src/flp/Flp.cpp
    src/core/Song.cpp
    src/core/Scheduler.cpp
    src/core/MarkerPlan.cpp
    src/core/Instruments.cpp
    src/core/RackRenderer.cpp
    src/core/LoadTrace.cpp
    src/core/ModuleWarmup.cpp
    src/core/FlpImport.cpp
    src/core/Effects.cpp
    src/core/NoteEdit.cpp
    src/core/Recorder.cpp
    src/core/TakeRecording.cpp
    src/core/Pdc.cpp
    src/core/WorkerPool.cpp
    src/core/TimeStretch.cpp
    src/core/Arpeggiator.cpp
    src/core/SynthPresets.cpp
    src/core/Slicer.cpp
    src/core/Templates.cpp
    src/core/FlWrapper.cpp
    src/core/FlPluginDb.cpp
    src/core/MissingFiles.cpp
    src/core/PluginCatalog.cpp
    src/core/ProjectSafety.cpp
    src/core/PluginSubstitutes.cpp
    src/core/Preferences.cpp
    src/core/ThemePalette.cpp
    src/core/PluginLayout.cpp
    src/core/StateParams.cpp
    src/core/FlEffects.cpp
    src/core/Effector.cpp
    src/core/FlSamplers.cpp
    src/core/FlSynths.cpp
    src/core/FlSytrus.cpp
    src/core/FlGenerators.cpp
    src/core/PhysicalModels.cpp
    src/core/DrumSynthLive.cpp
    src/core/Harmless.cpp
    src/core/Harmor.cpp
    src/core/Gms.cpp
    src/core/Morphine.cpp
    src/core/FruitKick.cpp
    src/core/BassDrum.cpp
    src/core/Ogun.cpp
    src/core/MiniSynth.cpp
    src/core/SimSynth.cpp
    src/core/Wasp.cpp
    src/core/Granulizer.cpp
    src/core/Drumaxx.cpp
    src/core/Slayer.cpp
    src/core/Dx10.cpp
    src/core/Kepler.cpp
    src/core/TransistorBass.cpp
    src/core/PoiZone.cpp
    src/core/Toxic.cpp
    src/core/ToxicFx.cpp
    src/core/HarmlessFx.cpp
    src/core/SytrusEngine.cpp
    src/core/VintagePhaser.cpp
    src/core/EmphasizerFx.cpp
    src/core/FreqShifterFl.cpp
    src/core/LowLifterFl.cpp
    src/core/VisualFx.cpp
    src/core/FruityDance.cpp
    src/core/Reeverb2.cpp
    src/core/LuxeVerb.cpp
    src/core/IldspMath.cpp
    src/core/ConvolverFx.cpp
    src/core/Vocodex.cpp
    src/core/MaximusFx.cpp
    src/core/GrossBeat.cpp
    src/core/LovePhilter.cpp
    src/core/FruityDynamics.cpp
    src/core/ParametricEqFl.cpp
    src/core/ParametricEqLinear.cpp
    src/core/EquoFx.cpp
    src/core/MultibandDelayFx.cpp
    src/core/PitchShifterFl.cpp
    src/core/FruityShapers.cpp
    src/core/FruityDelay.cpp
    src/core/StereoFx.cpp
    src/core/FrequencySplitter.cpp
    src/core/DistructorFx.cpp
    src/core/ChorusFx.cpp
    src/core/FruityBasicFx.cpp
    src/core/FruityDelayBank.cpp
    src/core/FruityVocoder.cpp
    src/core/DelphiMath.cpp
    src/core/FruityMultiband.cpp
    src/core/FlangusFastDist.cpp
    src/core/FruityPhaser.cpp
    src/core/FruityFilters.cpp
    src/core/NewtoneFx.cpp
    src/core/MixSuite.cpp
    src/core/EmberClassic.cpp
    src/core/VoxStrip.cpp
    src/core/RhythmPan.cpp
    src/core/VoiceMatch.cpp
    src/core/EchoSuite.cpp
    src/core/VocalPitch.cpp
    src/core/LynxTune.cpp
    src/core/LynxVerbs.cpp
    src/core/FlPatcher.cpp
    src/core/Patcher.cpp
    src/core/Vfx.cpp
    src/core/VfxSequencer.cpp
    src/core/ThreeOsc.cpp
    src/core/Plucked.cpp
    src/core/SoundFont.cpp
    src/core/SoundFontLoad.cpp
    src/core/SoundFontIndex.cpp
    src/core/HardcoreFx.cpp
    src/core/FacetEq.cpp
    src/core/ApexLimiter.cpp
    src/core/DynamicsSuite.cpp
    src/core/VoiceTools.cpp
    src/core/MidiControls.cpp
    src/core/FormulaController.cpp
    src/core/VoiceShift.cpp
    src/core/NativeEngines.cpp
    src/core/Layer.cpp
    src/core/XyController.cpp
    src/core/ControlSurface.cpp
    src/core/VoltageController.cpp
    src/core/FabFilterStates.cpp
    src/core/FlResampler.cpp
    src/core/FlKeys.cpp
    src/core/DirectWave.cpp
    src/core/OggVorbis.cpp
    src/core/BeepMap.cpp
    src/core/Sawer.cpp
    src/core/Inflate.cpp
    src/core/WavPack.cpp)
target_include_directories(lynx_core PUBLIC src third_party/sse2neon)

# workflow shell (key map, workspace layout, rack operations)
target_sources(lynx_core PRIVATE
    src/core/KeyMap.cpp
    src/core/Workspace.cpp
    src/core/RackOps.cpp
    src/core/ChannelMiscFx.cpp
    src/core/MidiImport.cpp
    src/core/StartupSound.cpp)

# workflow P2 (note tools, mixer operations, history labels, browser search)
target_sources(lynx_core PRIVATE
    src/core/NoteTools.cpp
    src/core/PianoRollTools.cpp
    src/core/EventEdit.cpp
    src/core/AudioEdit.cpp
    src/core/FlAutoSlicer.cpp
    src/core/MixerOps.cpp
    src/core/History.cpp
    src/core/EffectPresets.cpp
    src/core/PluginPresets.cpp
    src/core/ParamHelp.cpp
    src/core/SearchQuery.cpp src/core/SearchIndex.cpp src/core/SearchService.cpp src/core/BrowserView.cpp)

# spectral waveforms: band analysis and multi-resolution peak cache
target_sources(lynx_core PRIVATE
    src/core/WaveAnalysis.cpp)

# export options + MP3, plugin performance monitor, clip properties / arrangements (round 84)
target_sources(lynx_core PRIVATE
    src/core/ExportOptions.cpp
    src/core/Mp3Encoder.cpp
    src/core/Mp3Decoder.cpp
    src/core/PluginMonitor.cpp
    src/core/ProjectContents.cpp
    src/core/PlaylistOps.cpp
    src/core/TempoDetect.cpp)

# crash / freeze reports: consent, anonymiser, report JSON, GitHub issue URL (round 159)
target_sources(lynx_core PRIVATE
    src/core/CrashReport.cpp)

# LAME's libmp3lame (LGPL; third_party/lame, encoder only, configured by third_party/lame/src/config.h)
file(GLOB LAME_SOURCES third_party/lame/src/*.c)
add_library(lame_encoder STATIC ${LAME_SOURCES})
target_include_directories(lame_encoder PUBLIC third_party/lame/include PRIVATE third_party/lame/src)
target_compile_definitions(lame_encoder PRIVATE HAVE_CONFIG_H)
target_compile_options(lame_encoder PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-w>)
target_link_libraries(lynx_core PUBLIC lame_encoder)

# WavPack decoder (BSD; read-only) for the .wv samples in the FL packs
file(GLOB WAVPACK_SOURCES third_party/wavpack/src/*.c)
add_library(wavpack_decoder STATIC ${WAVPACK_SOURCES})
target_include_directories(wavpack_decoder PUBLIC third_party/wavpack/include PRIVATE third_party/wavpack/src)
target_compile_definitions(wavpack_decoder PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:HAVE___BUILTIN_CLZ=1>)
target_link_libraries(lynx_core PUBLIC wavpack_decoder)

# libogg + libvorbis decoder (BSD; third_party/vorbis, mdct.c modified as FL builds it, REA_rodada77) for FL's
# Ogg-in-WAV samples; no FMA contraction so the float operation order stays libvorbis'
file(GLOB VORBIS_SOURCES third_party/vorbis/src/*.c)
list(FILTER VORBIS_SOURCES EXCLUDE REGEX "/lookup\\.c$") # (included by lsp.c)
add_library(vorbis_decoder STATIC ${VORBIS_SOURCES})
target_include_directories(vorbis_decoder PUBLIC third_party/vorbis/include PRIVATE third_party/vorbis/src)
target_compile_options(vorbis_decoder PRIVATE $<$<NOT:$<C_COMPILER_ID:MSVC>>:-ffp-contract=off -w>
    # every symbol gets a lynx_ prefix (JUCE links its own copy with the same C names)
    $<IF:$<C_COMPILER_ID:MSVC>,/FI${CMAKE_CURRENT_SOURCE_DIR}/third_party/vorbis/include/lynx_vorbis_prefix.h,-include${CMAKE_CURRENT_SOURCE_DIR}/third_party/vorbis/include/lynx_vorbis_prefix.h>)
target_link_libraries(lynx_core PUBLIC vorbis_decoder)

add_executable(lynx_tests tests/tests.cpp)
target_link_libraries(lynx_tests PRIVATE lynx_core)

add_executable(flpdump tools/flpdump.cpp)
target_link_libraries(flpdump PRIVATE lynx_core)

add_executable(renderwav tools/renderwav.cpp)
target_link_libraries(renderwav PRIVATE lynx_core)

enable_testing()
add_test(NAME lynx_tests COMMAND lynx_tests)

# ---------------------------------------------------------------------------
# App (JUCE)

option(LYNX_BUILD_APP "Build the app with JUCE" ON)

if(LYNX_BUILD_APP)
    include(FetchContent)
    FetchContent_Declare(
        JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)

    juce_add_gui_app(LYNX
        PRODUCT_NAME "LYNX"
        COMPANY_NAME "Ka1-S"
        COMPANY_COPYRIGHT "Copyright (C) 2026 Ka1-S. Licensed under the GNU AGPL-3.0."
        DESCRIPTION "LYNX digital audio workstation"
        # macOS bundle (Info.plist, .icns made from the 1024 px badge); ignored elsewhere
        BUNDLE_ID "com.ka1-s.lynx"
        ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/lynx_badge.png"
        ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/lynx_icon_256.png"
        MICROPHONE_PERMISSION_ENABLED TRUE
        MICROPHONE_PERMISSION_TEXT "LYNX records audio from your microphone or audio interface when you arm a track and press Record."
        DOCUMENT_EXTENSIONS lynx flp mid
        HARDENED_RUNTIME_ENABLED FALSE
        PLIST_TO_MERGE "<plist><dict><key>LSMinimumSystemVersion</key><string>11.0</string><key>NSHighResolutionCapable</key><true/></dict></plist>")

    target_sources(LYNX PRIVATE
        src/Main.cpp
        src/platform/FileTypes.cpp
        src/platform/CrashGuard.cpp
        src/platform/Paths.cpp
        src/platform/NativeEmbed.cpp
        src/engine/AudioEngine.cpp
        src/engine/ChannelRackProcessor.cpp
        src/engine/Metronome.cpp
        src/engine/SampleLibrary.cpp
    src/engine/WavPackFormat.cpp
    src/engine/Mp3Format.cpp
        src/engine/ProjectFile.cpp
        src/engine/ProjectPackage.cpp
        src/engine/PluginHost.cpp
        src/engine/FlStudio.cpp
        src/engine/PluginSandbox.cpp
        src/engine/Exporter.cpp
        src/engine/StreamPrefetcher.cpp
        src/engine/DllAsioDevice.cpp
        src/engine/PipeWireDevice.cpp
        src/platform/LinuxRealtime.cpp
        src/engine/ClapFormat.cpp
        src/engine/Vst2Format.cpp
        src/engine/MidiFileIo.cpp
        src/engine/AudioLogger.cpp
        src/engine/PreviewPlayer.cpp
        src/ui/MainComponent.cpp
        src/ui/ChannelRackComponent.cpp
        src/ui/PianoRollComponent.cpp
        src/ui/PlaylistComponent.cpp
        src/ui/MixerComponent.cpp
        src/ui/QuickPick.cpp
        src/ui/PluginWindow.cpp
        src/ui/PluginPresetMenu.cpp
        src/ui/ProjectLoading.cpp
        src/ui/WindowOwner.cpp
        src/ui/ParametricEqEditor.cpp
        src/ui/FacetEqEditor.cpp
        src/ui/ProEditors.cpp
        src/ui/PluginHeader.cpp
        src/ui/PluginSkin.cpp
        src/ui/PluginEditors.cpp
        src/ui/PatcherEditor.cpp
        src/ui/DynamicsDisplays.cpp
        src/ui/ExportDialog.cpp
        src/ui/AutomationUi.cpp
        src/ui/ControllerEditors.cpp
        src/ui/VisualEditors.cpp
        src/ui/VisualGl.cpp
        src/ui/DanceView.cpp
        src/ui/NewtoneEditor.cpp
        src/ui/ChannelSettings.cpp
        src/ui/LayerEditor.cpp
        src/ui/Controllers164.cpp
        src/ui/VoltageEditor.cpp
        src/ui/SoundFontEditor.cpp
        src/ui/VfxEditors.cpp
        src/ui/BrowserComponent.cpp
        src/ui/Theme.cpp
        src/ui/ThemeSettings.cpp
        src/ui/ColourPicker.cpp
        src/ui/Icons.cpp
        src/ui/I18n.cpp
        src/ui/EditorTools.cpp)

    # workflow shell: workspace desktop, control contract, dialogs
    target_sources(LYNX PRIVATE
        src/ui/Workspace.cpp
        src/ui/ValueControl.cpp
        src/ui/Dialogs.cpp
        src/ui/SetupWizard.cpp
        src/ui/Splash.cpp)

    # main window chrome: hint bar and toolbar widgets
    target_sources(LYNX PRIVATE
        src/ui/HintBar.cpp
        src/ui/Toolbar.cpp)

    # spectral waveforms: background analysis cache and drawing
    target_sources(LYNX PRIVATE
        src/ui/WaveformView.cpp)

    # workflow P2: note tool dialogs, tool windows, undo history, quick-add palette
    target_sources(LYNX PRIVATE
        src/ui/NoteToolDialogs.cpp
        src/ui/EventEditor.cpp
        src/ui/AudioEditor.cpp
        src/ui/ToolWindow.cpp
        src/ui/TempoTapper.cpp
        src/ui/HistoryPanel.cpp
        src/ui/MissingFilesDialog.cpp
        src/ui/SettingsDialog.cpp
        src/ui/SettingsPages.cpp
        src/ui/PluginManager.cpp
        src/ui/ShortcutManager.cpp
        src/ui/NewProjectDialog.cpp
        src/ui/ClipProperties.cpp
        src/ui/PluginMonitorWindow.cpp
        src/ui/TouchKeyboard.cpp
        src/ui/QuickAddPalette.cpp
        src/ui/PluginPicker.cpp
        src/ui/CrashReporter.cpp)

    target_include_directories(LYNX PRIVATE third_party/clap/include)
    # the commit the build comes from (crash reports name it, round 159)
    execute_process(COMMAND git rev-parse --short=10 HEAD WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                    OUTPUT_VARIABLE LYNX_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/lynx_build/lynx_build.h "#pragma once\n#define LYNX_GIT_COMMIT \"${LYNX_GIT_COMMIT}\"\n")
    target_include_directories(LYNX PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/lynx_build)

    target_compile_definitions(LYNX PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_PLUGINHOST_VST3=1
        # MP3 is read by LYNX (src/engine/Mp3Format, minimp3; round 157b), not by JUCE's MP3AudioFormat
        JUCE_USE_MP3AUDIOFORMAT=0
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LYNX,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LYNX,JUCE_VERSION>")

    # per-platform audio drivers and plugin formats (docs/PLATFORMS.md)
    if(WIN32)
        target_include_directories(LYNX PRIVATE third_party/asiosdk/common)
        target_compile_definitions(LYNX PRIVATE JUCE_ASIO=1) # ASIO SDK headers in third_party/asiosdk (GPLv3 option)
    elseif(APPLE)
        target_compile_definitions(LYNX PRIVATE JUCE_PLUGINHOST_AU=1)
    else()
        # ALSA (linked) + JACK (loaded at run time; PipeWire serves JACK clients through pipewire-jack)
        target_compile_definitions(LYNX PRIVATE JUCE_ALSA=1 JUCE_JACK=1 JUCE_PLUGINHOST_LV2=1)
        # LYNX's own PipeWire driver (round 161): vendored MIT headers, libpipewire-0.3.so.0 loaded at run time
        target_include_directories(LYNX SYSTEM PRIVATE third_party/pipewire/include/pipewire-0.3 third_party/pipewire/include/spa-0.2)
        target_link_libraries(LYNX PRIVATE ${CMAKE_DL_LIBS})
    endif()

    target_link_libraries(LYNX
        PRIVATE
            lynx_core
            juce::juce_audio_utils
            juce::juce_audio_processors
            juce::juce_dsp
            juce::juce_opengl
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_lto_flags)
endif()

if(LYNX_BUILD_APP AND UNIX AND NOT APPLE)
    # one binary for many distributions: the C++ runtime goes inside (glibc and the desktop libraries stay shared)
    target_link_options(LYNX PRIVATE -static-libstdc++ -static-libgcc)
endif()

# LYNX ASIO: universal ASIO driver on WASAPI (REA round 97), an in-process COM DLL registered with regsvr32
if(WIN32)
    add_library(lynx-asio SHARED asio/LynxAsio.cpp asio/lynx-asio.def)
    target_include_directories(lynx-asio PRIVATE third_party/asiosdk/common)
    target_compile_definitions(lynx-asio PRIVATE UNICODE _UNICODE)
    target_link_libraries(lynx-asio PRIVATE ole32 uuid avrt user32 gdi32 advapi32)
endif()
