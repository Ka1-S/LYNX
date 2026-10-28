#include <juce_gui_basics/juce_gui_basics.h>
#include "platform/Paths.h"

#include "core/LoadTrace.h"
#include "core/Effects.h"
#include "core/HardwareInsert.h"
#include "core/EchoSuite.h"
#include "core/LynxTune.h"
#include "core/FlEffects.h"
#include "core/FlpImport.h"
#include "core/NoteEdit.h"
#include "core/MaximusFx.h"
#include "core/Harmor.h"
#include "flp/Flp.h"
#include "core/SynthPresets.h"
#include "core/FlWrapper.h"
#include "engine/AudioEngine.h"
#include "engine/AudioLogger.h"
#include "engine/DllAsioDevice.h"
#include "engine/PipeWireDevice.h"
#include "platform/LinuxRealtime.h"
#include "engine/Exporter.h"
#include "engine/MidiFileIo.h"
#include "engine/PluginHost.h"
#include "engine/PluginSandbox.h"
#include "engine/ProjectFile.h"
#include "engine/SampleLibrary.h"
#include "engine/Text.h"
#include "platform/FileTypes.h"
#include "ui/PatcherEditor.h"
#include "ui/VisualEditors.h"
#include "ui/FlExtraEditors.h"
#include "ui/PluginPresetMenu.h"
#include "ui/PluginWindow.h"
#include "ui/ClipProperties.h"
#include "ui/NewProjectDialog.h"
#include "ui/PluginMonitorWindow.h"
#include "ui/TouchKeyboard.h"
#include "ui/AutomationUi.h"
#include "ui/PluginSkin.h"
#include "ui/ChannelSettings.h"
#include "ui/ControllerEditors.h"
#include "ui/NewtoneEditor.h"
#include "core/NewtoneFx.h"
#include "core/FormulaController.h"
#include "core/MidiControls.h"
#include "core/FlSynths.h"
#include "ui/Dialogs.h"
#include "core/RackOps.h"
#include "core/Layer.h"
#include "core/XyController.h"
#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "ui/Controllers164.h"
#include "ui/ExportDialog.h"
#include "ui/I18n.h"
#include "ui/MainComponent.h"
#include "engine/ProjectPackage.h"
#include "ui/SetupWizard.h"
#include "engine/FlStudio.h"
#include "ui/HistoryPanel.h"
#include "ui/MissingFilesDialog.h"
#include "ui/SettingsDialog.h"
#include "ui/PluginManager.h"
#include "ui/ShortcutManager.h"
#include "ui/NoteToolDialogs.h"
#include "ui/EventEditor.h"
#include "ui/AudioEditor.h"
#include "ui/TempoTapper.h"
#include "core/EventEdit.h"
#include "core/FacetEq.h"
#include "ui/FacetEqEditor.h"
#include "core/RackRenderer.h"
#include "ui/ProEditors.h"
#include "ui/ParametricEqEditor.h"
#include "ui/PluginEditors.h"
#include "core/PluginLayout.h"
#include "core/Vfx.h"
#include "ui/Splash.h"
#include "ui/Theme.h"
#include "ui/ThemeSettings.h"
#include "ui/WaveformView.h"
#include "ui/WindowOwner.h"
#include "ui/Toolbar.h"
#include "ui/ToolWindow.h"
#include "platform/CrashGuard.h"
#include "ui/CrashReporter.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace
{
lynx::AudioEngine* crashEngine = nullptr;

// Startup trace (REA round 116): with LYNX_TRACE_STARTUP set, the time of each startup step since the C++ runtime
// started is written to %APPDATA%\LYNX\startup.log (the loader's own time before that is not included).
struct StartupTrace
{
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now(); // static initialisation: no JUCE yet, standard library only
    const bool on = std::getenv ("LYNX_TRACE_STARTUP") != nullptr;
    juce::String text;
    clock::time_point last = t0;

    double seconds() const { return std::chrono::duration<double> (clock::now() - t0).count(); } // since the process started

    void mark (const char* step)
    {
        if (! on)
            return;
        const auto now = clock::now();
        const auto ms = [] (clock::duration d) { return std::chrono::duration<double, std::milli> (d).count(); };
        text << juce::String (ms (now - t0), 1).paddedLeft (' ', 8) << " ms  +" << juce::String (ms (now - last), 1).paddedLeft (' ', 7) << "  " << step << "\n";
        last = now;
        lynx::platform::settingsFile ("startup.log").replaceWithText (text);
    }
};
StartupTrace startupTrace;

// Crashed: try to save the project and write the backtrace (like FL's "emergency save").
void onCrash (void* exceptionPointers)
{
    const auto folder = lynx::platform::settingsFile ("autosave");
    folder.createDirectory();
    const auto stamp = juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S");
    // JUCE's backtrace (named frames only), then every frame of the faulting thread as module+offset (round 157a: a
    // release LYNX.exe has no symbols, so its own frames were missing from the file)
    folder.getChildFile ("crash " + stamp + ".txt").replaceWithText (juce::SystemStats::getStackBacktrace() + "\n--- frames (module+offset)\n"
                                                                    + juce::String::fromUTF8 (lynx::crashguard::exceptionBacktrace (exceptionPointers).c_str()));
    lynx::crashguard::note (("crashed: backtrace in autosave\\crash " + stamp + ".txt").toStdString());

    if (crashEngine != nullptr)
    {
        juce::String error;
        lynx::ProjectFile::save (crashEngine->getSong(), folder.getChildFile ("recovered " + stamp + lynx::ProjectFile::extension), error);
    }
}
juce::File sessionsFolder()
{
    return lynx::PluginHost::settingsFolder().getChildFile ("sessions");
}

// Sessions of earlier runs whose process is gone without closing (a fail-fast, a crash, a forced end): each is appended
// to %APPDATA%\LYNX\crash-history.log and its file removed. Returns the last step of the newest one ("" when none).
juce::StringArray uncleanSteps; // the newest unclean session's steps (without their times)
juce::String uncleanSessionText; // the newest unclean session's whole file (round 159: the crash report reads it)

// What the newest unclean session tells (round 157a): the plugin whose editor was opening / shown when it ended, and
// whether its crash file blames a graphics driver
struct UncleanCause
{
    juce::String editorPlugin; // the last steps were that plugin's editor
    bool gpuDriver = false;    // the crash file's top frames are in a GPU driver / Direct3D / Direct2D module
    juce::String driverModule;
};

UncleanCause uncleanCause()
{
    UncleanCause cause;
    juce::File crashFile;
    for (int i = uncleanSteps.size(); --i >= 0 && i >= uncleanSteps.size() - 4;)
    {
        const auto& step = uncleanSteps[i];
        if (step.startsWith ("crashed: backtrace in "))
            crashFile = lynx::platform::settingsFolder().getChildFile (step.fromFirstOccurrenceOf ("backtrace in ", false, false).replaceCharacter ('\\', '/'));
        for (const auto* prefix : { "opening the editor of ", "editor open: ", "editor shown: " })
            if (cause.editorPlugin.isEmpty() && step.startsWith (prefix))
                cause.editorPlugin = step.fromFirstOccurrenceOf (prefix, false, false).trim();
        if (step.startsWith ("closing the editor of ") || step.startsWith ("loaded plugin") || step.startsWith ("opening project"))
            break; // older steps than these did not end the session
    }
    if (crashFile.existsAsFile())
    {
        // the first frames after the crash handler's own (the "--- frames" part lists the faulting thread)
        auto text = crashFile.loadFileAsString();
        if (text.contains ("--- frames"))
            text = text.fromFirstOccurrenceOf ("--- frames", false, false);
        auto lines = juce::StringArray::fromLines (text);
        int looked = 0;
        for (const auto& line : lines)
        {
            const auto module = line.fromFirstOccurrenceOf (": ", false, false).upToFirstOccurrenceOf ("+", false, false).trim().toLowerCase();
            if (module.isEmpty() || ! module.endsWith (".dll"))
                continue;
            if (++looked > 12)
                break;
            for (const auto* driver : { "igd", "ig9icd", "ig7icd", "igxelp", "nvwgf2um", "nvoglv", "nvd3dum", "atidxx", "atio6axx", "amdxx", "aticfx",
                                        "d3d11.dll", "d3d10warp", "dxgi.dll", "d2d1.dll", "opengl32.dll", "dcomp.dll" })
                if (module.startsWith (driver))
                {
                    cause.gpuDriver = true;
                    cause.driverModule = module;
                    break;
                }
            if (cause.gpuDriver)
                break;
        }
    }
    return cause;
}

juce::String collectUncleanSessions()
{
    juce::String lastStep;
    juce::Time newest;
    for (const auto& file : sessionsFolder().findChildFiles (juce::File::findFiles, false, "session-*.txt"))
    {
        const auto text = file.loadFileAsString();
        juce::StringPairArray values;
        juce::StringArray steps;
        for (const auto& line : juce::StringArray::fromLines (text))
        {
            if (line.startsWith ("step="))
                steps.add (line.fromFirstOccurrenceOf ("=", false, false));
            else if (line.contains ("="))
                values.set (line.upToFirstOccurrenceOf ("=", false, false), line.fromFirstOccurrenceOf ("=", false, false));
        }
        const auto pid = (std::uint32_t) values["pid"].getLargeIntValue();
        const auto start = (std::uint64_t) values["start"].getLargeIntValue();
        if (pid == lynx::crashguard::processId() || lynx::crashguard::processRunning (pid, start))
            continue; // this run, or another LYNX window still open

        lynx::PluginHost::settingsFolder().getChildFile ("crash-history.log")
            .appendText (juce::Time::getCurrentTime().toISO8601 (true) + "  previous session ended without closing\n" + text + "\n");
        file.deleteFile();

        if (const auto modified = file.getLastModificationTime(); lastStep.isEmpty() || modified > newest)
        {
            newest = modified;
            lastStep = steps.isEmpty() ? juce::String ("start-up") : steps[steps.size() - 1].fromFirstOccurrenceOf (" ", false, false);
            uncleanSessionText = text;
            uncleanSteps.clear();
            for (const auto& step : steps)
                uncleanSteps.add (step.fromFirstOccurrenceOf (" ", false, false));
        }
    }
    return lastStep;
}
} // namespace

class LynxApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override { return JUCE_APPLICATION_VERSION_STRING; }
    // the plugin scan runs copies of LYNX as child processes
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise (const juce::String& commandLine) override
    {
        // Windows, LYNX.exe in a folder with accents: run again from its ASCII short path (Waves editors crash otherwise)
        if (int code = 0; lynx::platform::relaunchFromAsciiPath (code))
        {
            relaunched = true;
            setApplicationReturnValue (code);
            quit();
            return;
        }

        // a crash in any mode (UI, command line) writes its backtrace to %APPDATA%\LYNX\autosave
        juce::SystemStats::setApplicationCrashHandler (onCrash);

        // child process of a sandboxed plugin: no window or audio of its own
        if (lynx::runPluginSandboxWorker (commandLine))
        {
            lynx::crashguard::quietCrashReports();
            sandboxChild = true;
            return;
        }

        const auto args = juce::StringArray::fromTokens (commandLine, true);

        // interface language: saved in %APPDATA%\LYNX\language.txt; --lang pt-BR overrides it for this run
        lynx::i18n::initialise (args);

        if (args.contains ("--showcase"))
        {
            // LYNX's own demo project (README screenshots): --showcase output.lynx [pattern]
            const auto i = args.indexOf ("--showcase");
            runShowcase (args[i + 1].unquoted(), args[i + 2].getIntValue());
            return;
        }

        if (args.contains ("--sandboxtest"))
        {
            const auto i = args.indexOf ("--sandboxtest");
            runSandboxTest (args[i + 1].unquoted(), args[i + 2].unquoted());
            return;
        }

        if (args.contains ("--rtstress"))
        {
            // LYNX.exe --rtstress project.(flp|lynx) report.txt [seconds] [--buffer N] [--type "driver type"] [--device "output"]
            // [--no-window] [--record take.wav]: the real app (window, audio device) plays the project and reports the callbacks
            // that missed their deadline (REA round 87); --type/--device pick another driver for this run only (round 97);
            // --record also records the input during the run, like Rec + Play, and moves the take to take.wav (round 106)
            const auto i = args.indexOf ("--rtstress");
            const auto seconds = args[i + 3].getDoubleValue();
            const auto buffer = args.contains ("--buffer") ? args[args.indexOf ("--buffer") + 1].getIntValue() : 0;
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            const auto option = [&args] (const char* name) { return args.contains (name) ? args[args.indexOf (name) + 1].unquoted() : juce::String(); };
            runRealtimeStress (juce::File (args[i + 1].unquoted()), juce::File (args[i + 2].unquoted()), seconds > 0.0 ? seconds : 30.0, buffer,
                               option ("--type"), option ("--device"), option ("--record"));
            return;
        }

        if (args.contains ("--rectest"))
        {
            // LYNX --rectest report.txt [seconds] [--type T] [--device D] [--input I] [--loop] [--buffer N]
            // (round 163): records a click track through a loopback (cable, interface loopback or PipeWire's
            // pw-loopback): pings the round trip, plays a click on every beat from an audio clip, records it on Insert 1,
            // stops, and checks every recorded click's song position on the Playlist against its beat (samples).
            // --loop: loop recording over bars 2-4 (take lanes). --punch: punch in/out on bars 3-4 with 1 bar of pre-roll
            // and 1 of post-roll (playback stops by itself after the post-roll).
            const auto i = args.indexOf ("--rectest");
            const auto option = [&args] (const char* name) { return args.contains (name) ? args[args.indexOf (name) + 1].unquoted() : juce::String(); };
            const auto seconds = args.size() > i + 2 && ! args[i + 2].startsWith ("--") ? args[i + 2].getDoubleValue() : 20.0;
            runRecordTest (juce::File (args[i + 1].unquoted()), seconds > 0.0 ? seconds : 20.0, option ("--type"), option ("--device"), option ("--input"),
                           args.contains ("--loop") ? 1 : args.contains ("--punch") ? 2 : 0, option ("--pdc").getIntValue(), option ("--buffer").getIntValue());
            return;
        }

        if (args.contains ("--hwinserttest"))
        {
            // LYNX --hwinserttest report.txt [seconds] [--type T] [--device D] [--input I] [--buffer N] (round 164d): a
            // hardware insert through a loopback cable (output pair 1-2 -> input pair 1-2; the Master plays nowhere, so
            // the cable carries the send only). The same click track plays on two inserts: Insert 1 through the
            // hardware insert (panned left), Insert 2 dry (panned right). The insert's own ping measures the round
            // trip, PDC delays the dry path by it, and every click of the Master's left side must sit on the same
            // sample as the right side's.
            const auto i = args.indexOf ("--hwinserttest");
            const auto option = [&args] (const char* name) { return args.contains (name) ? args[args.indexOf (name) + 1].unquoted() : juce::String(); };
            const auto seconds = args.size() > i + 2 && ! args[i + 2].startsWith ("--") ? args[i + 2].getDoubleValue() : 10.0;
            runHwInsertTest (juce::File (args[i + 1].unquoted()), seconds > 0.0 ? seconds : 10.0, option ("--type"), option ("--device"), option ("--input"),
                             option ("--buffer").getIntValue());
            return;
        }

        if (args.contains ("--uiperf"))
        {
            // LYNX --uiperf project.(flp|lynx) report.txt: the heavy UI cases in the real window, every frame timed (round 161)
            const auto i = args.indexOf ("--uiperf");
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            runUiPerf (juce::File (args[i + 1].unquoted()), juce::File (args[i + 2].unquoted()));
            return;
        }

        if (args.contains ("--selftest"))
        {
            const auto i = args.indexOf ("--selftest");
            runSelfTest (args[i + 1].unquoted(), args[i + 2].unquoted());
            return;
        }

        if (args.contains ("--scan-one"))
        {
            // child process of the plugin scan (see PluginHost::scan)
            const auto i = args.indexOf ("--scan-one");
            // no crash dialogs from the plugin being examined; and once the result is written, the process ends without
            // unloading it: a Waves shell crashed in its own teardown at exit (Intel GPU driver fail-fast, 0xC0000409),
            // which showed Windows' crash dialog and made the scan reject the whole shell (round 111)
            lynx::crashguard::quietCrashReports();
            lynx::PluginHost::scanOneFile (args[i + 1].unquoted(), args[i + 2].unquoted(), juce::File (args[i + 3].unquoted()));
            lynx::crashguard::exitNow (0);
        }

        if (args.contains ("--paths"))
        {
            // diagnostics: folders LYNX searches
            const auto programs = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
            juce::String report;
            report << "programs=" << programs.getFullPathName() << "\n";
            for (const auto& dir : programs.getChildFile ("Image-Line").findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
                report << "fl=" << dir.getFullPathName() << " packs=" << (int) dir.getChildFile ("Data").getChildFile ("Patches").getChildFile ("Packs").isDirectory() << "\n";
            juce::File (args[args.indexOf ("--paths") + 1].unquoted()).replaceWithText (report);
            quit();
            return;
        }

        if (args.contains ("--fldb"))
        {
            // diagnostics: LYNX.exe --fldb report.txt [project.flp]: the FL Studio installation, its plugin database,
            // the suggested plugin folders and, for a project, how each of its plugins resolves (read-only)
            const auto i = args.indexOf ("--fldb");
            const auto fl = lynx::findFlStudio();
            lynx::PluginHost host (false);
            juce::String report;
            report << "install=" << fl.installDir.getFullPathName() << "\nuserdata=" << fl.userData.getFullPathName() << "\n";
            for (const auto& f : fl.vstFolders)
                report << "flvstfolder=" << f << "\n";
            report << "dbfiles=" << host.flPluginDatabaseFiles() << " entries=" << (int) host.flPluginDatabase().entries().size() << "\n";
            for (const auto& e : host.flPluginDatabase().entries())
                report << "  entry: " << juce::String::fromUTF8 (e.name.c_str()) << " | " << juce::String::fromUTF8 (e.path.c_str()) << " | role=" << e.role << "\n";
            const auto suggested = host.suggestedFolders();
            for (int k = 0; k < suggested.getNumPaths(); ++k)
                report << "folder=" << suggested[k].getFullPathName() << "\n";
            if (const juce::File project (args[i + 2].unquoted()); project.existsAsFile())
            {
                juce::MemoryBlock data;
                project.loadFileAsData (data);
                try
                {
                    for (const auto& use : lynx::flpForeignPlugins (flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() })))
                    {
                        juce::String how;
                        const auto d = host.findForeign (use.plugin, use.instrument, &how);
                        const auto* entry = host.flPluginDatabase().match (use.plugin, use.instrument);
                        report << "plugin: " << juce::String::fromUTF8 (use.plugin.name.c_str()) << " -> "
                               << (d ? d->fileOrIdentifier + " (" + how + ")" : juce::String ("not known"))
                               << (entry != nullptr ? " | fl db: " + juce::String::fromUTF8 (entry->path.c_str()) : juce::String()) << "\n";
                    }
                    const auto uses = lynx::flpForeignPlugins (flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() }));
                    const auto candidates = host.filesToScanFor (uses);
                    report << "scan_candidates=" << candidates.joinIntoString (";") << "\n";
                    if (args.contains ("--scan"))
                    {
                        // the targeted scan an open would run (child processes; the user's plugin list is not touched)
                        report << "scanned_new=" << host.scanFiles (candidates, {}) << "\n";
                        for (const auto& use : uses)
                        {
                            juce::String how;
                            const auto d = host.findForeign (use.plugin, use.instrument, &how);
                            report << "after_scan: " << juce::String::fromUTF8 (use.plugin.name.c_str()) << " -> "
                                   << (d ? d->fileOrIdentifier + " (" + how + ")" : juce::String ("not known")) << "\n";
                        }
                    }
                }
                catch (const flp::Error& e)
                {
                    report << "error=" << e.what() << "\n";
                }
            }
            juce::File (args[i + 1].unquoted()).replaceWithText (report);
            quit();
            return;
        }

        if (args.contains ("--devices") || args.contains ("--dispositivos")) // --dispositivos: legacy alias
        {
            // diagnostics: driver types and output devices JUCE offers (without opening any)
            juce::AudioDeviceManager manager;
            juce::String report;
            manager.getAvailableDeviceTypes();
            if (auto pipeWire = lynx::makePipeWireDeviceType()) // Linux: LYNX's own PipeWire client (round 161)
                manager.addAudioDeviceType (std::move (pipeWire));
           #if JUCE_LINUX
            report << lynx::describePipeWire() << "\n";
           #endif

            for (auto* type : manager.getAvailableDeviceTypes())
            {
                type->scanForDevices();
                report << "type=" << type->getTypeName() << "\n";
                for (const auto& name : type->getDeviceNames (false))
                    report << "  output=" << name << "\n";
                for (const auto& name : type->getDeviceNames (true))
                    report << "  input=" << name << "\n";
            }

            const auto flag = args.contains ("--devices") ? args.indexOf ("--devices") : args.indexOf ("--dispositivos");
            juce::File (args[flag + 1].unquoted()).replaceWithText (report);
            quit();
            return;
        }

        if (args.contains ("--devicetest"))
        {
            // LYNX.exe --devicetest "type" "device" report.txt ["input device"] [--buffer N] [--roundtrip]: opens that output device (and, with the
            // 4th argument, an input: "*" = the same device / the driver's default input), receives 1 s of callbacks
            // writing silence, closes it and reports rate, block size, latency, callbacks and the input channels with
            // their peak level (driver check, no sound). --roundtrip: clicks out, found again on input 1 through a loopback
            // cable / PipeWire link: the round-trip latency (3 s)
            const auto i = args.indexOf ("--devicetest");
            const auto typeName = args[i + 1].unquoted(), deviceName = args[i + 2].unquoted();
            auto inputName = args.size() > i + 4 && ! args[i + 4].startsWith ("--") ? args[i + 4].unquoted() : juce::String();
            const auto wantInput = inputName.isNotEmpty();
            juce::String report;
            const juce::File reportFile (args[i + 3].unquoted());
            auto step = [&] (const juce::String& line) { report << line << "\n"; reportFile.replaceWithText (report); }; // progress survives a driver that ends the process
            step ("step=start");
            juce::AudioDeviceManager manager;
            manager.getAvailableDeviceTypes();
            if (auto pipeWire = lynx::makePipeWireDeviceType())
                manager.addAudioDeviceType (std::move (pipeWire));
            std::unique_ptr<juce::AudioIODevice> device;
            for (auto* type : manager.getAvailableDeviceTypes())
                if (type->getTypeName() == typeName)
                {
                    type->scanForDevices();
                    if (wantInput && type->hasSeparateInputsAndOutputs())
                    {
                        const auto inputs = type->getDeviceNames (true);
                        if (inputName == "*" || ! inputs.contains (inputName))
                        {
                            const auto index = type->getDefaultDeviceIndex (true);
                            inputName = juce::isPositiveAndBelow (index, inputs.size()) ? inputs[index] : juce::String();
                        }
                        for (const auto& n : inputs)
                            step ("input_device=" + n);
                        step ("input_chosen=" + inputName);
                    }
                    else if (wantInput)
                        inputName = deviceName;
                    step ("step=create");
                    device.reset (type->createDevice (deviceName, type->hasSeparateInputsAndOutputs() ? inputName : juce::String()));
                    step ("step=created");
                }
            struct Silence final : juce::AudioIODeviceCallback
            {
                std::atomic<int> calls { 0 }, inputs { 0 };
                std::array<std::atomic<float>, 64> peaks {};
                // --roundtrip (round 161): a click every quarter second on the outputs, found again on input 1 through a
                // loopback; the delays in samples
                bool roundtrip = false;
                int period = 12000;
                std::int64_t position = 0, lastClick = -1;
                std::vector<int> delays;
                void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int channels, int n,
                                                       const juce::AudioIODeviceCallbackContext&) override
                {
                    for (int c = 0; c < channels; ++c)
                        if (out[c] != nullptr)
                            juce::FloatVectorOperations::clear (out[c], n);
                    if (roundtrip)
                    {
                        for (int i = 0; i < n; ++i)
                        {
                            if (numIn > 0 && in[0] != nullptr && lastClick >= 0 && std::abs (in[0][i]) > 0.5f)
                            {
                                if (delays.size() < delays.capacity())
                                    delays.push_back ((int) (position + i - lastClick));
                                lastClick = -1;
                            }
                            if ((position + i) % period == 0 && position + i > 0)
                            {
                                for (int c = 0; c < channels; ++c)
                                    if (out[c] != nullptr)
                                        out[c][i] = 0.9f;
                                lastClick = position + i;
                            }
                        }
                        position += n;
                    }
                    inputs.store (numIn);
                    for (int c = 0; c < juce::jmin (numIn, 64); ++c)
                        if (in[c] != nullptr)
                        {
                            const auto range = juce::FloatVectorOperations::findMinAndMax (in[c], n);
                            const auto peak = juce::jmax (std::abs (range.getStart()), std::abs (range.getEnd()));
                            if (peak > peaks[(std::size_t) c].load())
                                peaks[(std::size_t) c].store (peak);
                        }
                    ++calls;
                }
                void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
                void audioDeviceStopped() override {}
            } silence;
            bool ok = false;
            if (device == nullptr)
                step ("error=device not found");
            else
            {
                step ("step=open");
                juce::BigInteger outputs;
                outputs.setRange (0, 2, true);
                const auto rates = device->getAvailableSampleRates();
                const auto rate = device->getCurrentSampleRate() > 0 ? device->getCurrentSampleRate()
                                  : rates.contains (48000.0) ? 48000.0 : rates.isEmpty() ? 44100.0 : rates[0];
                step ("rates=" + juce::String (rates.size()) + " chosen=" + juce::String (rate) + " default_block=" + juce::String (device->getDefaultBufferSize()));
                juce::BigInteger inputs;
                if (wantInput)
                {
                    const auto names = device->getInputChannelNames();
                    step ("input_channels=" + juce::String (names.size()));
                    for (int c = 0; c < names.size(); ++c)
                        step ("  in" + juce::String (c + 1) + "=" + names[c]);
                    inputs.setRange (0, juce::jmin (64, names.size()), true);
                }
                // --buffer N: ask for that block size (REA round 97 latency survey); default: the driver's default
                const auto wantedBlock = args.contains ("--buffer") ? args[args.indexOf ("--buffer") + 1].getIntValue() : 0;
                const auto error = device->open (inputs, outputs, rate, wantedBlock > 0 ? wantedBlock : device->getDefaultBufferSize());
                if (error.isNotEmpty())
                    step ("error=" + error);
                else
                {
                    step ("step=start_callbacks");
                    silence.roundtrip = args.contains ("--roundtrip");
                    silence.period = juce::jmax (1, (int) (rate / 4));
                    silence.delays.reserve (64);
                    device->start (&silence);
                    juce::Thread::sleep (silence.roundtrip ? 3000 : 1000);
                    device->stop();
                    if (silence.roundtrip)
                    {
                        auto d = silence.delays;
                        std::sort (d.begin(), d.end());
                        report << "roundtrip_clicks=" << (int) d.size() << "\n";
                        if (! d.empty())
                            report << "roundtrip_samples=min " << d.front() << " median " << d[d.size() / 2] << " max " << d.back() << "\n"
                                   << "roundtrip_ms=" << juce::String (1000.0 * d[d.size() / 2] / device->getCurrentSampleRate(), 2) << "\n";
                    }
                    report << "rate=" << device->getCurrentSampleRate() << "\n"
                           << "block=" << device->getCurrentBufferSizeSamples() << "\n"
                           << "output_latency=" << device->getOutputLatencyInSamples() << "\n"
                           << "callbacks=" << silence.calls.load() << "\n";
                    if (wantInput)
                    {
                        report << "input_latency=" << device->getInputLatencyInSamples() << "\n"
                               << "active_inputs=" << device->getActiveInputChannels().countNumberOfSetBits() << "\n"
                               << "callback_inputs=" << silence.inputs.load() << "\n";
                        for (int c = 0; c < juce::jmin (64, silence.inputs.load()); ++c)
                            report << "  peak_in" << (c + 1) << "=" << juce::Decibels::gainToDecibels (silence.peaks[(std::size_t) c].load(), -150.0f) << " dB\n";
                    }
                    ok = silence.calls.load() > 0 && (! wantInput || silence.inputs.load() > 0);
                    device->close();
                }
            }
            step (juce::String ("result=") + (ok ? "OK" : "FAILED"));
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--scantest"))
        {
            // LYNX.exe --scantest folder report.txt: scan (in child processes) without touching the user's list
            const auto i = args.indexOf ("--scantest");
            lynx::PluginHost host (false);
            const auto found = host.scan (juce::FileSearchPath (args[i + 1].unquoted()), {});
            juce::String report;
            report << "found=" << found << "\n";

            for (const auto& t : host.getKnownPlugins().getTypes())
                report << "  " << (t.isInstrument ? "instrument: " : "effect: ") << t.name << "\n";

            for (const auto& f : host.getKnownPlugins().getBlacklistedFiles())
                report << "  blacklisted: " << f << "\n";

            juce::File (args[i + 2].unquoted()).replaceWithText (report);
            setApplicationReturnValue (found > 0 ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--vst2test"))
        {
            const auto i = args.indexOf ("--vst2test");
            runVst2Test (args[i + 1].unquoted(), args[i + 2].unquoted());
            return;
        }

        if (args.contains ("--wrappertest"))
        {
            const auto i = args.indexOf ("--wrappertest");
            runWrapperTest (args[i + 1].unquoted(), args[i + 2].unquoted());
            return;
        }

        if (args.contains ("--plugintest"))
        {
            const auto i = args.indexOf ("--plugintest");
            // optional 3rd argument: the plugin's name inside a shell (e.g. "CLA-2A Stereo" in a WaveShell)
            const auto only = i + 3 < args.size() && ! args[i + 3].startsWith ("--") ? args[i + 3].unquoted() : juce::String();
            runPluginTest (args[i + 1].unquoted(), args[i + 2].unquoted(), only);
            return;
        }

        if (args.contains ("--editortest"))
        {
            // LYNX.exe --editortest plugin.vst3 report.txt ["name"]: opens the plugin's own editor in a window for a few
            // seconds, then closes it (a plugin whose UI crashes the host ends the process before the report says OK)
            const auto i = args.indexOf ("--editortest");
            const auto only = i + 3 < args.size() && ! args[i + 3].startsWith ("--") ? args[i + 3].unquoted() : juce::String();
            runEditorTest (args[i + 1].unquoted(), args[i + 2].unquoted(), only);
            return;
        }

        if (args.contains ("--plugwintest"))
        {
            // LYNX --plugwintest plugin report.txt ["name"] [seconds]: round 162d. Puts the plugin in a channel (an
            // instrument) or on insert 1 (an effect) of an offline engine, opens its plugin window (wrapper header,
            // presets), checks a preset round trip, keeps the window `seconds` on screen (default 15: time for xdotool /
            // screenshots), then removes the plugin from the song and reports whether its window closed by itself.
            const auto i = args.indexOf ("--plugwintest");
            const auto only = i + 3 < args.size() && ! args[i + 3].startsWith ("--") && ! args[i + 3].containsOnly ("0123456789") ? args[i + 3].unquoted() : juce::String();
            auto seconds = 15;
            for (int k = i + 3; k < juce::jmin (args.size(), i + 5); ++k)
                if (args[k].containsOnly ("0123456789"))
                    seconds = args[k].getIntValue();
            runPlugWinTest (args[i + 1].unquoted(), args[i + 2].unquoted(), only, seconds);
            return;
        }

        if (args.contains ("--fittest"))
        {
            // LYNX.exe --fittest report.txt [percent]: round 163 — at that UI zoom (default 150) a window taller than
            // the screen and a big resizable window that grows with a zoom change keep their title bar and close
            // button on the screen. The windows show for about two seconds.
            const auto i = args.indexOf ("--fittest");
            runFitTest (args[i + 1].unquoted(), i + 2 < args.size() ? args[i + 2].getIntValue() : 150);
            return;
        }

        if (args.contains ("--windowtest"))
        {
            // LYNX.exe --windowtest report.txt: opens the Quick-add palette, the Audio editor and a plugin-like window
            // over the main window and checks that each one stays in front, owned by the main window, with the
            // keyboard focus (issue #4). The windows appear on screen for a few seconds.
            runWindowTest (args[args.indexOf ("--windowtest") + 1].unquoted());
            return;
        }

        if (args.contains ("--pluginstyle")) // round 126: force one style for this run (glow, neon, hardware, vivid; auto = per plugin)
        {
            const auto i = args.indexOf ("--pluginstyle");
            if (i + 1 < args.size())
            {
                const auto name = args[i + 1].unquoted();
                lynx::skin::setForcedStyle (name.equalsIgnoreCase ("auto") ? -1 : (int) lynx::skin::styleFromName (name));
            }
        }

        if (args.contains ("--plugui"))
        {
            // LYNX.exe --plugui <folder>: the native effect editors at FL's size (one PNG per editor and page), each
            // loaded with the FL plugin's Default preset when the FL installation has one
            const auto i = args.indexOf ("--plugui");
            runPluginEditorSnapshots (args[i + 1].unquoted());
            return;
        }

        if (args.contains ("--editorcpu"))
        {
            // LYNX.exe --editorcpu report.txt: opens built-in plugin editors one at a time in a small window (no audio
            // device: a silent, idle editor) and writes the process CPU each costs while idle and while its display
            // animates (demo signal) — round 122's check that an idle editor burns no CPU
            const auto i = args.indexOf ("--editorcpu");
            runEditorCpu (args[i + 1].unquoted());
            return;
        }

        if (args.contains ("--flextrashot"))
        {
            // LYNX.exe --flextrashot <folder>: pictures of the editors of round 165g (Big Clock, Tuner, HTML NoteBook, Mute 2)
            const auto i = args.indexOf ("--flextrashot");
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            lynx::AudioEngine shotEngine (false);
            const auto ok = lynx::runFlExtraShots (shotEngine, juce::File (args[i + 1].unquoted()));
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--visualtest"))
        {
            // LYNX.exe --visualtest <folder> [project.flp]: the visual plugins (round 164c) — the audio path's CPU, every
            // Wave Candy mode and Visualizer scene drawn from a test signal (PNG + the editor's CPU each), and the video
            // export of a project (default: FL's first ZGameEditor demo) with ffmpeg and as images. Writes report.txt.
            const auto i = args.indexOf ("--visualtest");
            juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
            const auto project = i + 2 < args.size() && ! args[i + 2].startsWith ("--") ? juce::File (args[i + 2].unquoted()) : juce::File();
            lynx::runVisualTest (juce::File (args[i + 1].unquoted()), project, [this] (bool ok) {
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
            return;
        }

        if (args.contains ("--menu-entry"))
        {
            // LYNX.AppImage --menu-entry on|off: puts the AppImage in the applications menu (and .lynx files on it) or
            // takes it out again, without opening a window; the same as the switch in Options › General (round 161)
            const auto i = args.indexOf ("--menu-entry");
            const auto on = args[i + 1].unquoted() != "off";
            const auto ok = lynx::runningAsAppImage() && lynx::setAppImageMenuEntry (on);
            std::fprintf (ok ? stdout : stderr, "%s\n", ok ? (on ? "LYNX added to the applications menu" : "LYNX removed from the applications menu")
                                                         : "only the LYNX AppImage on Linux can do this");
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--send-test-report"))
        {
            // LYNX.exe --send-test-report out.txt [--dry]: builds a crash report marked "test" from the newest crash /
            // hang file and this computer's facts, writes its JSON to out.txt.json and (without --dry) sends it to
            // lynx-share.com (LYNX_CRASH_ENDPOINT overrides); out.txt gets "id=…" and "result=OK" or the error (round 159)
            const auto i = args.indexOf ("--send-test-report");
            const juce::File out (args[i + 1].unquoted());
            lynx::crashreporter::Session session;
            session.version = getApplicationVersion();
            for (const auto* pattern : { "crash *.txt", "hang *.txt" })
            {
                auto files = lynx::platform::settingsFile ("autosave").findChildFiles (juce::File::findFiles, false, pattern);
                std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
                if (! files.isEmpty())
                    (juce::String (pattern).startsWith ("crash") ? session.crashFile : session.hangFile) = files[0];
            }
            session.steps = { "00:00:01 test report (LYNX.exe --send-test-report)", "00:00:02 opening project " + out.getFileName() };
            auto input = lynx::crashreporter::makeInput (session, session.crashFile.existsAsFile() ? "crash" : "manual", nullptr);
            input.test = true;
            const auto json = lynx::crashreporter::buildJson (input, false);
            out.withFileExtension (out.getFileExtension() + ".json").replaceWithText (json);
            const auto error = args.contains ("--dry") ? juce::String() : lynx::crashreporter::sendNow (json);
            out.replaceWithText ("id=" + juce::String::fromUTF8 (input.id.c_str()) + "\nendpoint=" + lynx::crashreporter::endpoint() + "\nbytes="
                                 + juce::String (json.getNumBytesAsUTF8()) + "\nresult=" + (error.isEmpty() ? juce::String ("OK") : error) + "\n");
            setApplicationReturnValue (error.isEmpty() ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--symbolize"))
        {
            // LYNX.exe --symbolize "crash … .txt" out.txt: names the LYNX.exe+offset frames of a user's crash / hang file
            // (needs this build's LYNX.pdb next to LYNX.exe — the release's "LYNX-symbols" CI artifact; round 157a)
            const auto i = args.indexOf ("--symbolize");
            const auto text = juce::File (args[i + 1].unquoted()).loadFileAsString();
            juce::File (args[i + 2].unquoted()).replaceWithText (juce::String::fromUTF8 (lynx::crashguard::symbolizeOwnFrames (text.toStdString()).c_str()));
            quit();
            return;
        }

        if (args.contains ("--opentest"))
        {
            // LYNX.exe --opentest report.txt [--visible]: every built-in effect and instrument window opened as the mixer /
            // Channel rack open them, with default, extreme, random and old-project states; a watchdog fails the test on
            // a hang (round 157a: opening the Parametric EQ froze LYNX). Windows stay off-screen without --visible.
            const auto i = args.indexOf ("--opentest");
            runOpenTest (args[i + 1].unquoted(), args.contains ("--visible"), [this] (bool ok) {
                setApplicationReturnValue (ok ? 0 : 1);
                quit();
            });
            return;
        }

        if (args.contains ("--fxfuzz"))
        {
            // LYNX.exe --fxfuzz report.txt [rounds] [--drags]: every built-in effect in its editor, parameters swept (and
            // dragged with the mouse), audio rendered at several rates; a crash ends the process (round 111)
            const auto i = args.indexOf ("--fxfuzz");
            const auto rounds = args.size() > i + 2 ? juce::jmax (1, args[i + 2].getIntValue()) : 6;
            // --drags: also mouse drags over the editors (their menus may flash on screen: not for the selftest)
            const auto ok = runEffectFuzz (rounds, args[i + 1].unquoted(), args.contains ("--drags"));
            setApplicationReturnValue (ok ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--controllershot"))
        {
            // LYNX.exe --controllershot prefix: the built-in controllers' editors (round 133) as PNGs
            const auto i = args.indexOf ("--controllershot");
            runControllerShot (args[i + 1].unquoted());
            quit();
            return;
        }

        if (args.contains ("--vfxshot"))
        {
            // LYNX.exe --vfxshot prefix: the VFX note effects' editors (round 164b), on channels and in a Patcher, as PNGs
            const auto i = args.indexOf ("--vfxshot");
            runVfxShot (args[i + 1].unquoted());
            quit();
            return;
        }

        if (args.contains ("--patchershot"))
        {
            // LYNX.exe --patchershot prefix [project]: every Patcher's editor (round 141) as PNGs (a demo one without a project)
            const auto i = args.indexOf ("--patchershot");
            runPatcherShot (args[i + 1].unquoted(), i + 2 < args.size() ? args[i + 2].unquoted() : juce::String());
            quit();
            return;
        }

        if (args.contains ("--snapshot"))
        {
            const auto i = args.indexOf ("--snapshot");
            runSnapshot (args[i + 1].unquoted(), args[i + 2].unquoted());
            return;
        }

        if (args.contains ("--stems"))
        {
            // LYNX.exe --stems project base.wav: one WAV per insert that has sound (like "Export stems")
            const auto i = args.indexOf ("--stems");
            juce::String error, report;
            int written = 0;
            {
                lynx::AudioEngine loader (false);
                const juce::File input (args[i + 1].unquoted()), base (args[i + 2].unquoted());
                auto ok = input.hasFileExtension ("flp") ? loader.importFlpFile (input, error) : false;

                if (! input.hasFileExtension ("flp"))
                    if (auto song = lynx::ProjectFile::load (input, error))
                    {
                        loader.setSong (std::move (*song));
                        ok = true;
                    }

                const auto& song = loader.getSong();

                for (int t = 1; ok && t < (int) song.mixer.size(); ++t)
                {
                    if (std::none_of (song.channels.begin(), song.channels.end(), [t] (const lynx::ChannelDef& c) { return c.mixerTrack == t; }))
                        continue;

                    const auto file = base.getSiblingFile (base.getFileNameWithoutExtension() + " - " + juce::String (t) + ".wav");
                    lynx::ExportSettings settings;
                    lynx::ExportJob job (lynx::makeBounceSong (song, t), settings, file);

                    if (job.run ({}, error))
                    {
                        ++written;
                        report << file.getFileName() << " peak=" << job.getPeak() << "\n";
                    }
                }
            }

            report << "stems=" << written << "\n";
            juce::File (args[i + 2].unquoted() + ".txt").replaceWithText (report);
            setApplicationReturnValue (written > 0 ? 0 : 1);
            quit();
            return;
        }

        if (args.contains ("--export"))
        {
            const auto i = args.indexOf ("--export");
            const auto insert = args.contains ("--insert") ? args[args.indexOf ("--insert") + 1].getIntValue() : 0;
            runExport (args[i + 1].unquoted(), args[i + 2].unquoted(), args.contains ("--pattern"), insert);
            return;
        }

        if (args.contains ("--render"))
        {
            const auto i = args.indexOf ("--render");
            runRender (args[i + 1].unquoted(), args[i + 2].unquoted(), args[i + 3].getDoubleValue());
            return;
        }

        // session breadcrumb: a fail-fast (no crash handler runs) still leaves the last steps behind (round 111)
        uncleanLastStep = collectUncleanSessions();
        sessionsFolder().createDirectory();
        lynx::crashguard::beginSession (
            sessionsFolder().getChildFile ("session-" + juce::String (lynx::crashguard::processId()) + ".txt").getFullPathName().toStdString(),
            ("LYNX session\nversion=" + getApplicationVersion() + "\npid=" + juce::String (lynx::crashguard::processId())
             + "\nstart=" + juce::String ((juce::int64) lynx::crashguard::processStartTime())
             + "\nstarted=" + juce::Time::getCurrentTime().toISO8601 (true) + "\n").toStdString());
        startHangWatch();

        startupTrace.mark ("initialise (normal start)");
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lynx::themeui::loadSaved(); // the user's theme (Options › Theme, round 162d)
        startupOptions = lynx::StartupOptions::load();
        lynx::registerFileTypes();
        startupTrace.mark ("options + file types");
        // Double-clicking a .lynx/.flp opens LYNX with the file as an argument.
        if (args.size() > 0 && juce::File::isAbsolutePath (args[0].unquoted()))
            fileToOpen = juce::File (args[0].unquoted());
        // LYNX --opentime project.(flp|lynx)|- report.txt [idle seconds] (round 165i): the normal start-up (splash, audio
        // device, window) opens the project, waits until it is loaded and LYNX has gone quiet, measures the idle CPU and
        // memory, writes the times and quits normally (a benchmark that killed LYNX left "closed unexpectedly" behind)
        if (args.contains ("--opentime"))
        {
            const auto i = args.indexOf ("--opentime");
            const auto project = args[i + 1].unquoted();
            fileToOpen = project != "-" && juce::File::isAbsolutePath (project) ? juce::File (project) : juce::File();
            openTimeReport = juce::File (args[i + 2].unquoted());
            if (args.size() > i + 3 && ! args[i + 3].startsWith ("--"))
                openTimeIdleSeconds = juce::jlimit (1, 120, args[i + 3].getIntValue());
        }

        if (! startupOptions.splash)
        {
            startEngine();
            createMainWindow();
            mainWindow->reveal();
            showUncleanNotice();
            startupTrace.mark ("window revealed");
            openStartupFile();
            startupTrace.mark ("startup file opened");
            startOpenTimeProbe();
            juce::MessageManager::callAsync ([] { startupTrace.mark ("first message loop turn"); });
            return;
        }

        // the splash first, so it is on screen while the audio device opens; the main window is built
        // once the pepper has landed (the scene is still for a moment then) and shown when the splash ends
        splash = std::make_unique<lynx::SplashScreen>();
        splash->showOnDesktop();

        juce::Timer::callAfterDelay (40, [this] {
            startEngine();

            if (splash == nullptr) // quit meanwhile
                return;

            splash->setStatus (lynx::tr ("Loading the workspace..."));
            splash->startClock();
            // the minimum counts from the process start (the audio device took part of it already)
            splashDuration = lynx::SplashScreen::remainingDuration (startupTrace.seconds());
            splash->setBarDuration (splashDuration);

            juce::Timer::callAfterDelay (1050, [this] {
                if (splash == nullptr || engine == nullptr)
                    return;

                createMainWindow();
                const auto wait = juce::jmax (0.0, splashDuration - splash->getElapsed());

                juce::Timer::callAfterDelay ((int) (wait * 1000.0), [this] {
                    if (splash == nullptr || mainWindow == nullptr)
                        return;

                    mainWindow->reveal();
                    startupTrace.mark ("window revealed (after the splash)");
                    splash->toFront (false);
                    splash->finish ([this] { splash = nullptr; });
                    showUncleanNotice();
                    openStartupFile();
                    startupTrace.mark ("startup file opened");
                    startOpenTimeProbe();
                    juce::MessageManager::callAsync ([] { startupTrace.mark ("first message loop turn"); });
                    // first run: language, audio, plugin folders (FL Studio's included) and a background scan
                    if (! lynx::setupWizardDone() && lynx::platform::testMode())
                        lynx::platform::testModeLog ("setup wizard");
                    else if (! lynx::setupWizardDone())
                        mainWindow->showSetupWizard();
                });
            });
        });
    }

    void startEngine()
    {
        startupTrace.mark ("before the audio engine");
        engine = std::make_unique<lynx::AudioEngine>();
        startupTrace.mark ("audio engine + device");
        crashEngine = engine.get();
        lynx::crashreporter::watchAudioDevice (engine->getDeviceManager()); // (the session file names the device: crash reports)
        addDriverFileDeviceType();

        if (startupOptions.sound)
            engine->previewSample (lynx::makeStartupSample());
        startupTrace.mark ("startup sound");

        // a project given on the command line: its slow plugin modules (a Waves shell) start now, on their own
        // thread, while the splash and the main window come up (round 121)
        if (fileToOpen.hasFileExtension ("flp"))
            if (juce::MemoryBlock data; fileToOpen.loadFileAsData (data))
                try
                {
                    engine->getPluginHost().startModulesFor (lynx::flpForeignPlugins (flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() })));
                    startupTrace.mark ("plugin modules started ahead");
                }
                catch (const flp::Error&)
                {
                }
    }

    void createMainWindow()
    {
        mainWindow = std::make_unique<MainWindow> (getApplicationName(), *engine);
        lynx::setMainWindowForOwnership (mainWindow.get());
        lynx::applyRenderingChoice (*mainWindow); // (GPU rendering off: GDI; round 157a)
        lynx::startRenderingWatch();
        startupTrace.mark ("main window built");
    }

    // the previous run ended without closing (see collectUncleanSessions): say so once, with its last step — and, as
    // Settings › General › "Crash reports" says (round 159), ask whether to send a report, send it unasked, or not
    void showUncleanNotice()
    {
        if (lynx::platform::testMode()) // LYNX_TEST_MODE: no prompt, no report sent (written to test-mode.log)
        {
            if (uncleanLastStep.isNotEmpty())
                lynx::platform::testModeLog ("\"closed unexpectedly\" notice (last step: " + uncleanLastStep + ")");
            return;
        }
        if (uncleanLastStep.isEmpty())
        {
            lynx::crashreporter::retryPending(); // (reports an earlier run could not send: once more)
            return;
        }
        auto message = lynx::tr ("The last session ended without closing normally. Last step: {0}. Backup copies of your project are in "
                                 "LYNX's autosave folder; details in crash-history.log.",
                                 { uncleanLastStep });
        uncleanLastStep.clear();
        const auto cause = uncleanCause();
        // round 157a: a plugin's editor was opening — it crashed (often in the graphics driver, inside the plugin's own
        // OpenGL window): offer to keep that plugin in its own process, where a crash takes only the plugin down
        juce::String separatePlugin;
        if (cause.editorPlugin.isNotEmpty() && ! lynx::pluginSandboxEnabled() && ! lynx::pluginAlwaysSeparate (cause.editorPlugin))
            separatePlugin = cause.editorPlugin;
        // LYNX's own drawing crashed in the graphics driver (Direct2D / Direct3D): draw with the CPU from now on
        else if (cause.gpuDriver && lynx::gpuRenderingEnabled())
        {
            lynx::setGpuRenderingEnabled (false);
            message << "\n\n"
                    << lynx::tr ("The crash was in the graphics driver ({0}). LYNX now draws its windows without the GPU; you can turn GPU "
                                 "rendering back on in Options > General settings (after a driver update, for example).",
                                 { cause.driverModule });
        }

        const auto session = lynx::crashreporter::parseSession (uncleanSessionText);
        auto* devices = engine != nullptr ? &engine->getDeviceManager() : nullptr;
        const auto action = lynx::crashreport::actionFor (lynx::crashreporter::consent());
        lynx::crashreporter::retryPending();
        if (action == lynx::crashreport::Action::askUser)
        {
            if (separatePlugin.isNotEmpty())
                message << "\n\n"
                        << lynx::tr ("It happened while the window of {0} was opening{1}.",
                                     { separatePlugin, cause.gpuDriver ? lynx::tr (" (in the graphics driver, {0})", { cause.driverModule }) : juce::String() });
            lynx::crashreporter::showStartupDialog (session, message, separatePlugin, [separatePlugin] { lynx::setPluginAlwaysSeparate (separatePlugin, true); },
                                                    devices);
            return;
        }
        if (action == lynx::crashreport::Action::sendSilently)
        {
            const auto input = lynx::crashreporter::makeInput (session, session.kind(), devices);
            lynx::crashreporter::send (lynx::crashreporter::buildJson (input, false), juce::String::fromUTF8 (input.id.c_str()));
            message << "\n\n" << lynx::tr ("A crash report is being sent to help fix it (Options > General settings > Crash reports).");
        }
        if (separatePlugin.isNotEmpty())
        {
            juce::AlertWindow::showOkCancelBox (
                juce::MessageBoxIconType::WarningIcon, lynx::tr ("LYNX closed unexpectedly"),
                message + "\n\n"
                    + lynx::tr ("It happened while the window of {0} was opening{1}. Run {0} in a separate process from now on? If it "
                                "crashes again, only the plugin stops and LYNX keeps running (Plugin wrapper menu: \"Always run in a "
                                "separate process\").",
                                { separatePlugin, cause.gpuDriver ? lynx::tr (" (in the graphics driver, {0})", { cause.driverModule }) : juce::String() }),
                lynx::tr ("Run separately"), lynx::tr ("No"), nullptr,
                juce::ModalCallbackFunction::create ([separatePlugin] (int result) {
                    if (result != 0)
                        lynx::setPluginAlwaysSeparate (separatePlugin, true);
                }));
            return;
        }
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, lynx::tr ("LYNX closed unexpectedly"), message);
    }

    // --opentime: after the start-up file was handed to the window
    void startOpenTimeProbe()
    {
        if (openTimeReport == juce::File())
            return;

        struct Probe final : juce::Timer
        {
            LynxApplication& app;
            juce::File report, project;
            int idleSeconds;
            int phase = 0; // 0 loading, 1 settling, 2 idle measurement
            double revealedS = 0.0, loadedS = -1.0, settledS = -1.0, cpuAtLoaded = 0.0;
            double lastCpu = 0.0, lastT = 0.0, quietSince = -1.0, idleCpu0 = 0.0, idleT0 = 0.0;

            Probe (LynxApplication& a, juce::File r, juce::File p, int idle)
                : app (a), report (std::move (r)), project (std::move (p)), idleSeconds (idle)
            {
                revealedS = startupTrace.seconds();
                startTimer (250);
            }

            bool loaded() const
            {
                if (project == juce::File())
                    return true;
                return app.mainWindow != nullptr
                    && app.mainWindow->getName().containsIgnoreCase (project.getFileNameWithoutExtension());
            }

            void finish (const juce::String& result)
            {
                stopTimer();
                const auto now = startupTrace.seconds();
                const auto cpu = lynx::processCpuSeconds();
                juce::String text;
                text << "project=" << (project == juce::File() ? juce::String ("-") : project.getFullPathName()) << "\n"
                     << "window_s=" << juce::String (revealedS, 3) << "\n"
                     << "loaded_s=" << juce::String (loadedS, 3) << "\n"
                     << "settled_s=" << juce::String (settledS, 3) << "\n"
                     << "cpu_at_loaded_s=" << juce::String (cpuAtLoaded, 3) << "\n"
                     << "idle_s=" << juce::String (now - idleT0, 2) << "\n"
                     << "idle_cpu_pct=" << juce::String (phase == 2 && now > idleT0 ? 100.0 * (cpu - idleCpu0) / (now - idleT0) : -1.0, 1) << "\n"
                     << "working_set_mb=" << juce::String ((double) lynx::toolbar::PerformanceMeter::processMemoryBytes() / 1048576.0, 0) << "\n"
                     << "cpu_total_s=" << juce::String (cpu, 3) << "\n"
                     << "result=" << result << "\n";
                report.replaceWithText (text);
                app.setApplicationReturnValue (result == "OK" ? 0 : 1);
                app.quit();
            }

            void timerCallback() override
            {
                const auto now = startupTrace.seconds();
                const auto cpu = lynx::processCpuSeconds();
                if (phase == 0)
                {
                    if (loaded())
                    {
                        loadedS = now;
                        cpuAtLoaded = cpu;
                        phase = 1;
                        lastCpu = cpu;
                        lastT = now;
                    }
                    else if (now - revealedS > 240.0)
                        finish ("TIMEOUT loading");
                    return;
                }
                if (phase == 1)
                {
                    // quiet = under 10 % of one core for 1.5 s (the harness's rule), at most 30 s after loading
                    const auto use = now > lastT ? 100.0 * (cpu - lastCpu) / (now - lastT) : 0.0;
                    lastCpu = cpu;
                    lastT = now;
                    if (use < 10.0)
                    {
                        if (quietSince < 0.0)
                            quietSince = now - 0.25;
                        if (now - quietSince >= 1.5)
                            settledS = quietSince;
                    }
                    else
                        quietSince = -1.0;
                    if (settledS >= 0.0 || now - loadedS > 30.0)
                    {
                        phase = 2;
                        idleCpu0 = cpu;
                        idleT0 = now;
                    }
                    return;
                }
                if (now - idleT0 >= (double) idleSeconds)
                    finish ("OK");
            }
        };
        perf = std::make_unique<Probe> (*this, openTimeReport, fileToOpen, openTimeIdleSeconds);
    }

    void openStartupFile()
    {
        if (fileToOpen != juce::File() && mainWindow != nullptr)
            mainWindow->openFile (fileToOpen);
    }

    void shutdown() override
    {
        if (relaunched)
            return; // the copy that ran owns the session (its crash notes stay for the next start)

        lynx::crashguard::note ("closing");
        lynx::shutdownPluginSandboxWorker();
        splash = nullptr;
        mainWindow = nullptr; // the UI goes before the engine
        crashEngine = nullptr;
        engine = nullptr;
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        heartbeat.stopTimer();
        lynx::crashreporter::shutdown(); // (an upload in flight gets a moment, never long)
        lynx::crashguard::stopHangWatch();
        lynx::crashguard::endSession(); // a clean exit: no breadcrumb left behind

        // everything is saved and closed: a process that loaded third-party plugins ends here, before the DLLs' own
        // teardown runs (some crash in it at exit, round 111)
        if (sandboxChild || lynx::PluginHost::pluginsWereLoaded())
            lynx::crashguard::exitNow (getApplicationReturnValue());
    }

    void systemRequestedQuit() override
    {
        if (mainWindow != nullptr)
            mainWindow->confirmDiscard ([] { juce::JUCEApplication::quit(); });
        else
            quit();
    }

private:
    // round 157a: a frozen UI (Windows' "not responding") writes the message thread's stack to autosave\hang <time>.txt
    // and a session step, so a freeze can be told apart from a crash and located. The timer is the heartbeat.
    struct Heartbeat final : juce::Timer
    {
        void timerCallback() override { lynx::crashguard::heartbeat(); }
    } heartbeat;

    void startHangWatch()
    {
        const auto folder = lynx::platform::settingsFile ("autosave");
        folder.createDirectory();
        // keep the newest hang files only
        auto old = folder.findChildFiles (juce::File::findFiles, false, "hang *.txt");
        std::sort (old.begin(), old.end(), [] (const juce::File& a, const juce::File& b) { return a.getLastModificationTime() > b.getLastModificationTime(); });
        for (int i = 20; i < old.size(); ++i)
            old.getReference (i).deleteFile();
        lynx::crashguard::startHangWatch (folder.getFullPathName().replaceCharacter ('\\', '/').toStdString());
        heartbeat.startTimerHz (5);
    }

    bool sandboxChild = false;
    bool relaunched = false; // relaunchFromAsciiPath ran LYNX from its short path: this process only waited
    juce::String uncleanLastStep; // last step of a previous run that ended without closing

    // LYNX.exe --selftest report.txt: renders 2 s without a sound card and writes the result.
    // Exit code 0 = audio came out, 1 = silence.
    void runSelfTest (const juce::String& reportPath, const juce::String& demoProjectPath)
    {
        lynx::AudioEngine offline (false);
        const auto peak = offline.renderOffline (2.0, 48000.0);
        const auto audioOk = peak > 0.01f && peak < 10.0f;

        // project format round trip, with accents in the name
        auto original = lynx::Song::makeDefault();
        original.channels[0].name = "Bumbo ação";
        original.playlist.push_back ({ 0, 2, 3840, 1920, 960 });
        original.mode = lynx::PlayMode::song;
        original.mixer[3].effects.push_back (lynx::makeEffectSlot (lynx::EffectKind::delay));
        original.mixer[3].effects[0].params[1] = 0.25f;
        lynx::Automation automation;
        automation.name = "Pan";
        automation.target = { lynx::AutomationTargetKind::effectParam, 3, 0, 1 };
        automation.points = { { 0, 0.25f, 0.5f }, { 3840, 1.0f, 0.0f } };
        original.automations.push_back (automation);
        lynx::Clip automationClip;
        automationClip.kind = lynx::ClipKind::automation;
        automationClip.automation = 0;
        automationClip.track = 4;
        automationClip.length = 3840;
        original.playlist.push_back (automationClip);
        original.loopStart = 960;
        original.loopEnd = 7680;
        original.playlistTracks.resize (3);
        original.playlistTracks[2].colour = 0xFF2A6FDB; // a track colour (round 90)
        original.playlistTracks[2].grouped = true;      // track groups and rack filter groups (round 104)
        original.playlistTracks[1].collapsed = true;
        original.channelGroups = { "Drums", "Keys" };
        original.channels[1].group = 1;
        juce::String error;

        if (demoProjectPath.isNotEmpty()) // sample project for checking the UI with --snapshot
            lynx::ProjectFile::save (original, juce::File (demoProjectPath), error);

        const auto reloaded = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (original), error);
        const auto projectOk = reloaded && reloaded->channels.size() == original.channels.size()
                               && reloaded->channels[0].name == original.channels[0].name
                               && reloaded->patterns[0].notes.size() == original.patterns[0].notes.size()
                               && reloaded->patterns[0].notes[3].start == original.patterns[0].notes[3].start
                               && reloaded->mode == lynx::PlayMode::song && reloaded->playlist.size() == 2
                               && reloaded->playlist[1].kind == lynx::ClipKind::automation && reloaded->playlist[1].automation == 0
                               && reloaded->automations.size() == 1 && reloaded->automations[0].target == automation.target
                               && reloaded->automations[0].points.size() == 2 && reloaded->automations[0].points[0].tension == 0.5f
                               && reloaded->automations[0].points[1].tick == 3840
                               && reloaded->loopStart == 960 && reloaded->loopEnd == 7680
                               && reloaded->playlistTracks.size() == 3 && reloaded->playlistTracks[2].colour == 0xFF2A6FDB
                               && reloaded->playlistTracks[1].colour == 0
                               && reloaded->playlistTracks[2].grouped && reloaded->playlistTracks[1].collapsed && ! reloaded->playlistTracks[1].grouped
                               && reloaded->channelGroups == original.channelGroups && reloaded->channels[1].group == 1 && reloaded->channels[0].group == -1
                               && reloaded->playlist[0].offset == 960 && reloaded->playlist[0].track == 2
                               && reloaded->mixer[3].effects.size() == 1
                               && reloaded->mixer[3].effects[0].kind == lynx::EffectKind::delay
                               && reloaded->mixer[3].effects[0].params[1] == 0.25f;
        // round trip of the newer features: synth/sampler with modulation, EQs, utility, markers
        auto rich = lynx::Song::makeDefault();
        auto& synth = rich.channels[4].synth;
        synth.osc[1] = { lynx::Waveform::square, -1, 7.0f, 0.4f, 5, -0.25f };
        synth.filterType = 2;
        synth.mono = true;
        synth.portaBeats = 0.125f;
        synth.am = true;
        synth.filterEnv = { 3.0f, 0.01f, 0.02f, 0.3f, 0.25f, 0.4f };
        synth.lfo[lynx::lfoPitch] = { 0.5f, 0.25f, true, 1, 0.1f, 0.2f };
        auto& sampler = rich.channels[0].sampler;
        sampler.cutoff = 1200.0f;
        sampler.resonance = 0.3f;
        sampler.filterType = 1;
        sampler.pitchEnv = { -12.0f, 0.0f, 0.0f, 0.05f, 0.0f, 0.1f };
        sampler.lfo[lynx::lfoVolume] = { 0.5f, 0.5f, false, 2, 0.0f, 0.0f };
        rich.mixer[2].eq.gainDb = { 3.0f, -2.5f, 1.0f };
        rich.mixer[2].eq.hz[1] = 2200.0f;
        auto peqSlot = lynx::makeEffectSlot (lynx::EffectKind::parametricEq);
        peqSlot.params[(std::size_t) lynx::peq::param (2, 2)] = lynx::peq::gainValue (6.0);
        peqSlot.params[(std::size_t) (lynx::peq::slopeParam + 0)] = lynx::peq::slopeValue (3);
        rich.mixer[2].effects.push_back (peqSlot);
        auto utilitySlot = lynx::makeEffectSlot (lynx::EffectKind::utility);
        utilitySlot.params[1] = 0.2f;
        rich.mixer[2].effects.push_back (utilitySlot);
        auto vocodexSlot = lynx::makeEffectSlot (lynx::EffectKind::vocodex); // FL's raw state travels as base64
        for (int i = 0; i < 300; ++i)
            vocodexSlot.state.push_back ((std::uint8_t) (i * 37));
        vocodexSlot.sidechain = 1;
        vocodexSlot.sidechain2 = 2;
        rich.mixer[3].effects.push_back (vocodexSlot);
        rich.addMarker ({ 3840, "Refrão", 0, 4 });
        rich.addMarker ({ 7680, {}, 7, 8 });
        const auto richBack = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (rich), error);
        auto paramsClose = [] (const lynx::EffectParams& a, const lynx::EffectParams& b) {
            for (std::size_t i = 0; i < a.size(); ++i)
                if (std::abs (a[i] - b[i]) > 1.0e-3f)
                    return false;
            return true;
        };
        const auto richOk = richBack && richBack->channels[4].synth == synth && richBack->channels[0].sampler == sampler
                            && richBack->mixer[2].eq == rich.mixer[2].eq && richBack->mixer[2].effects.size() == 2
                            && richBack->mixer[2].effects[0].kind == lynx::EffectKind::parametricEq
                            && paramsClose (richBack->mixer[2].effects[0].params, peqSlot.params)
                            && richBack->mixer[2].effects[1].kind == lynx::EffectKind::utility
                            && paramsClose (richBack->mixer[2].effects[1].params, utilitySlot.params)
                            && richBack->mixer[3].effects.size() == 1 && richBack->mixer[3].effects[0].kind == lynx::EffectKind::vocodex
                            && richBack->mixer[3].effects[0].state == vocodexSlot.state && richBack->mixer[3].effects[0].sidechain2 == 2
                            && richBack->markers() == rich.markers();

        // hardware output targets (REA round 156): saved only when set; old projects (no field) keep the defaults
        bool hwOutOk = false;
        {
            auto song = lynx::Song::makeDefault();
            const auto plain = lynx::ProjectFile::toJson (song);
            song.mixer[0].outputChannel = lynx::outputNone;
            song.mixer[1].outputChannel = 2;
            song.mixer[1].outputMode = lynx::OutputMode::monoRight;
            song.mixer[2].outputChannel = 6;
            song.mixer[3].outputChannel = 4;
            song.mixer[3].outputMode = lynx::OutputMode::monoSum;
            const auto back = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (song), error);
            const auto plainBack = lynx::ProjectFile::fromJson (plain, error);
            hwOutOk = back && back->mixer[0].outputChannel == lynx::outputNone && back->mixer[1].outputChannel == 2
                      && back->mixer[1].outputMode == lynx::OutputMode::monoRight && back->mixer[2].outputChannel == 6
                      && back->mixer[2].outputMode == lynx::OutputMode::stereo && back->mixer[3].outputMode == lynx::OutputMode::monoSum
                      && back->mixer[4].outputChannel == lynx::outputDefault && ! plain.contains ("\"output\"") && plainBack
                      && plainBack->mixer[0].outputChannel == lynx::outputDefault && plainBack->mixer[1].outputChannel == lynx::outputDefault;
        }

        // LYNX substitutes (round 98): a missing plugin's slot plays a built-in equivalent and the project keeps the
        // original reference byte for byte (.flp record and LYNX plugin state)
        bool substituteOk = false;
        {
            auto song = lynx::Song::makeDefault();
            lynx::ForeignPlugin vv; // an .flp VST3 record (ValhallaVintageVerb's class ID), opaque state
            vv.format = lynx::ForeignPlugin::Format::vst3;
            const std::uint8_t cid[] = { 0x76, 0x54, 0x53, 0x56, 0x65, 0x65, 0x76, 0x33, 0x61, 0x6c, 0x68, 0x61, 0x6c, 0x6c, 0x61, 0x76 };
            std::copy (std::begin (cid), std::end (cid), vv.classId.begin());
            vv.hasClassId = true;
            vv.name = "ValhallaVintageVerb";
            vv.path = "C:\\x\\ValhallaVintageVerb.vst3";
            for (int i = 0; i < 777; ++i)
                vv.state.push_back ((std::uint8_t) (i * 13 + 1));
            lynx::EffectSlot fromFlp;
            fromFlp.kind = lynx::EffectKind::plugin;
            fromFlp.plugin.foreign = vv;
            lynx::EffectSlot fromLynx; // a LYNX reference whose plugin isn't installed here
            fromLynx.kind = lynx::EffectKind::plugin;
            fromLynx.plugin.id = "VST3-FabFilter Pro-Q 3-1a2b3c4d-5e6f7a8b";
            fromLynx.plugin.name = "FabFilter Pro-Q 3";
            fromLynx.plugin.state = "AAECAwQFBgcICQ==";
            const auto flpRef = fromFlp.plugin, lynxRef = fromLynx.plugin;
            song.mixer[1].effects = { fromFlp, fromLynx };
            const auto substitutesOn = lynx::pluginSubstitutesEnabled();
            const auto changed = offline.getPluginHost().applySubstitutes (song);
            auto& fx = song.mixer[1].effects;
            // only the LYNX reference is substituted here (an .flp record is the import's job); do the .flp one directly
            const auto flpSubstituted = lynx::substituteEffectSlot (fx[0], lynx::pluginIdentity (vv));
            const auto back = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (song), error);
            substituteOk = flpSubstituted && fx[0].kind == lynx::EffectKind::epochVerb
                           && (! substitutesOn || (changed == 1 && fx[1].kind == lynx::EffectKind::facetEq && fx[1].substituteFor == lynxRef))
                           && back && back->mixer[1].effects.size() == 2 && back->mixer[1].effects[0].kind == lynx::EffectKind::epochVerb
                           && back->mixer[1].effects[0].substituteFor == flpRef && back->mixer[1].effects[0].substituteFor.foreign.state == vv.state
                           && back->mixer[1].effects[1].kind == fx[1].kind && back->mixer[1].effects[1].substituteFor == fx[1].substituteFor;
        }

        // undo/redo
        offline.editSong ([] (lynx::Song& s) { s.bpm = 99.0; });
        offline.editSong ([] (lynx::Song& s) { s.bpm = 101.0; });
        offline.undo();
        const auto afterUndo = offline.getSong().bpm;
        offline.redo();
        const auto undoOk = afterUndo == 99.0 && offline.getSong().bpm == 101.0 && offline.canUndo() && ! offline.canRedo()
                            && offline.isModified();

        // recording from the PC keyboard: a one-beat note lands in the pattern, in a single undo step
        const auto notesBefore = offline.getSong().patterns[0].notes.size();
        offline.setRecording (true);
        offline.play();
        offline.getTransport().positionTicks.store (960.0);
        offline.sendLiveNote (2, 72, 0.9f, true, true);
        offline.getTransport().positionTicks.store (1920.0);
        offline.sendLiveNote (2, 72, 0.0f, false, true);
        offline.setRecording (false);
        offline.stop();
        const auto& recorded = offline.getSong().patterns[0].notes;
        const auto recordOk = recorded.size() == notesBefore + 1
                              && std::any_of (recorded.begin(), recorded.end(), [] (const lynx::Note& n) {
                                     return n.key == 72 && n.channel == 2 && n.length == 960 && n.start >= 960 && n.start < 3840;
                                 });
        offline.undo();
        const auto recordUndoOk = offline.getSong().patterns[0].notes.size() == notesBefore;

        // MIDI: exporting the pattern and importing it back gives the same notes
        bool midiOk = false;
        {
            auto source = lynx::Song::makeDefault();
            source.patterns[0].notes.push_back ({ 1000, 480, 4, 67, 0.5f });
            source.patterns[0].sortNotes();
            const auto midiFile = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lynx-selftest.mid");
            juce::String midiError;
            const auto saved = lynx::MidiFileIo::savePattern (source, 0, midiFile, midiError);
            const auto song = saved ? lynx::MidiFileIo::loadSong (midiFile, midiError) : std::nullopt;
            midiFile.deleteFile();

            if (song && song->patterns.size() == 1 && song->patterns[0].notes.size() == source.patterns[0].notes.size()
                && std::abs (song->bpm - source.bpm) < 0.01)
            {
                midiOk = true;
                const auto& a = source.patterns[0].notes;
                const auto& b = song->patterns[0].notes;
                for (std::size_t i = 0; i < a.size(); ++i)
                    midiOk = midiOk && a[i].start == b[i].start && a[i].key == b[i].key && a[i].length == b[i].length;
            }
        }

        // project package (REA round 118): a project with a sample exported as .zip, extracted and opened again,
        // finds its sample inside the extracted folder
        bool packageOk = false;
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lynx-selftest-package");
            dir.deleteRecursively();
            dir.getChildFile ("src").createDirectory();
            const auto wav = dir.getChildFile ("src/hit.wav");
            {
                juce::WavAudioFormat format;
                juce::AudioBuffer<float> hit (1, 480);
                for (int i = 0; i < 480; ++i)
                    hit.setSample (0, i, 0.5f * std::sin (0.1f * (float) i));
                std::unique_ptr<juce::OutputStream> out (wav.createOutputStream().release());
                if (auto* w = format.createWriterFor (out.get(), 48000.0, 1, 16, {}, 0))
                {
                    out.release();
                    std::unique_ptr<juce::AudioFormatWriter> writer (w);
                    writer->writeFromAudioSampleBuffer (hit, 0, 480);
                }
            }
            auto source = lynx::Song::makeDefault();
            lynx::ChannelDef c;
            c.name = "Hit";
            c.instrument = lynx::InstrumentKind::sampler;
            c.samplePath = wav.getFullPathName().toStdString();
            source.channels.push_back (c);
            lynx::ProjectPackage::Result packed;
            juce::String packageError;
            const auto zip = dir.getChildFile ("Beat.zip");
            if (wav.existsAsFile() && lynx::ProjectPackage::exportZip (source, "Beat", zip, packed, packageError) && packed.files == 1)
            {
                const auto project = lynx::ProjectPackage::extractZip (zip, dir.getChildFile ("out"), packageError);
                const auto song = project.existsAsFile() ? lynx::ProjectFile::load (project, packageError) : std::nullopt;
                if (song && ! song->channels.empty())
                {
                    const juce::File back (juce::String::fromUTF8 (song->channels.back().samplePath.c_str()));
                    packageOk = back.existsAsFile() && back.isAChildOf (project.getParentDirectory()) && back.getSize() == wav.getSize();
                }
            }
            dir.deleteRecursively();
        }

        // WavPack: a .wv from the FL packs (if installed) opens through the sample loader
        juce::String wavpack = "NO_FL";
        {
            const auto roots = lynx::findFlStudioRoots();
            const auto packs = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Packs");
            const auto found = packs.isDirectory() ? packs.findChildFiles (juce::File::findFiles, true, "*.wv") : juce::Array<juce::File> {};
            if (! found.isEmpty())
            {
                lynx::SampleLibrary library;
                const auto sample = library.load (found.getFirst().getFullPathName());
                float peakWv = 0.0f;
                if (sample != nullptr)
                    for (std::size_t i = 0; i < sample->frames(); ++i)
                        peakWv = std::max (peakWv, std::abs (sample->leftData()[i]));
                wavpack = sample != nullptr && sample->frames() > 0 && peakWv > 1e-4f ? "OK" : "ERROR";
            }
        }

        // every built-in effect's editor and engine with extreme settings (round 111): a crash or NaN output fails here
        const auto effectsOk = runEffectFuzz (4, {}); // (no mouse drags: nothing may open on screen)

        // FL's Patcher (round 141): a mixer slot and a generator channel with their graphs survive save / load, and the
        // generator one plays through the rack
        bool patcherOk = false;
        {
            auto song = lynx::Song::makeDefault();
            song.mixer.resize (4);
            auto slot = lynx::makeEffectSlot (lynx::EffectKind::patcher);
            lynx::PatchNode reverb, gain;
            reverb.effect = lynx::makeEffectSlot (lynx::EffectKind::utility); // (exact defaults: params are saved to 4 decimals)
            reverb.label = "Room";
            reverb.x = 120.5f;
            reverb.y = 40.0f;
            gain.effect = lynx::makeEffectSlot (lynx::EffectKind::utility);
            gain.effect.enabled = false;
            slot.patcher.nodes = { reverb, gain };
            lynx::PatchWire a, b, c, control;
            a.to = 0;
            b.from = 0, b.to = 1, b.gain = 0.5f;
            c.from = 1;
            control.kind = lynx::PatchWire::Kind::control, control.from = 0, control.to = 1, control.toPort = 0, control.curve = { 0.0f, 0.25f, 1.0f };
            slot.patcher.wires = { a, b, c, control };
            slot.patcher.outputTracks = { 3 };
            song.mixer[1].effects = { slot };
            lynx::ChannelDef ch;
            ch.name = "Patched";
            ch.instrument = lynx::InstrumentKind::patcher;
            lynx::PatchNode synth;
            synth.generator = true;
            synth.channel.instrument = lynx::InstrumentKind::synth;
            synth.channel.name = "Synth";
            ch.patcher.nodes = { synth };
            lynx::PatchWire notes, out;
            notes.kind = lynx::PatchWire::Kind::midi, notes.to = 0;
            out.from = 0;
            ch.patcher.wires = { notes, out };
            // round 162: the I/O nodes' MIDI pins and a MIDI Out node between them
            lynx::PatchNode midiOutNode;
            midiOutNode.generator = true;
            midiOutNode.channel.instrument = lynx::InstrumentKind::midiOut;
            lynx::midictl::writeMidiOut (midiOutNode.channel.pluginState, { 4, 0, 0, -1, -1 });
            ch.patcher.nodes.push_back (midiOutNode);
            lynx::PatchWire portIn, portOut;
            portIn.kind = portOut.kind = lynx::PatchWire::Kind::midi;
            portIn.fromPort = lynx::patchMidiPortPin (7), portIn.to = 1;
            portOut.from = 1, portOut.to = lynx::patchToHost, portOut.toPort = 9;
            ch.patcher.wires.push_back (portIn);
            ch.patcher.wires.push_back (portOut);
            ch.patcher.fromMidiPorts = { 2, 7 };
            ch.patcher.toMidiPorts = { 9, 11 };
            ch.patcher.fromNotesHidden = true;
            song.channels.push_back (ch);
            const auto back = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (song), error);
            patcherOk = back && back->mixer.size() >= 2 && back->mixer[1].effects.size() == 1
                        && back->mixer[1].effects[0].kind == lynx::EffectKind::patcher && back->mixer[1].effects[0].patcher == slot.patcher
                        && back->channels.back().instrument == lynx::InstrumentKind::patcher && back->channels.back().patcher.nodes.size() == 2
                        && back->channels.back().patcher.fromMidiPorts == ch.patcher.fromMidiPorts
                        && back->channels.back().patcher.toMidiPorts == ch.patcher.toMidiPorts && back->channels.back().patcher.fromNotesHidden
                        && back->channels.back().patcher.nodes[1].channel.pluginState == midiOutNode.channel.pluginState
                        && back->channels.back().patcher.nodes[0].generator
                        && back->channels.back().patcher.nodes[0].channel.instrument == lynx::InstrumentKind::synth
                        && back->channels.back().patcher.wires == ch.patcher.wires;
        }

        // FL engines in a native project (round 142, issue #5): every engine this PC can make survives save / load and
        // the instruments play through the rack
        juce::String nativesResult = "OK";
        int nativesMade = 0, nativesSilent = 0;
        {
            auto song = lynx::Song::makeDefault();
            song.channels.clear();
            song.patterns[0].notes.clear();
            song.mixer.resize (3);
            const auto& fl = lynx::flStudioAccess();
            for (const auto& e : lynx::natives::instrumentEngines())
                if (lynx::natives::available (e, fl))
                    if (auto c = lynx::natives::makeChannel (e.kind, fl))
                    {
                        c->mixerTrack = 1;
                        song.channels.push_back (*c);
                    }
            for (const auto& e : lynx::natives::effectEngines())
                if (auto fx = lynx::natives::makeSlot (e.kind, fl))
                    song.mixer[2].effects.push_back (*fx);
            nativesMade = (int) (song.channels.size() + song.mixer[2].effects.size());
            const auto back = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (song), error);
            auto same = back && back->channels.size() == song.channels.size() && back->mixer.size() >= 3
                        && back->mixer[2].effects.size() == song.mixer[2].effects.size();
            for (std::size_t i = 0; same && i < song.channels.size(); ++i)
            {
                const auto& a = song.channels[i];
                const auto& b = back->channels[i];
                same = a.instrument == b.instrument && a.pluginState == b.pluginState && a.samplePath == b.samplePath && a.zones.size() == b.zones.size()
                       && a.modelParams.size() == b.modelParams.size();
                if (! same)
                    nativesResult = "ERROR channel " + juce::String::fromUTF8 (a.name.c_str());
            }
            for (std::size_t i = 0; same && i < song.mixer[2].effects.size(); ++i)
            {
                const auto& a = song.mixer[2].effects[i];
                const auto& b = back->mixer[2].effects[i];
                same = a.kind == b.kind && a.state == b.state;
                for (std::size_t p = 0; same && p < a.params.size(); ++p)
                    same = std::abs (a.params[p] - b.params[p]) < 1.0e-3f;
                if (! same)
                    nativesResult = juce::String ("ERROR effect ") + lynx::effectInfo (a.kind).name;
            }
            if (! back)
                nativesResult = "ERROR " + error;
            // each instrument plays a note alone (the granulizer needs a sample: it is silent here)
            for (std::size_t i = 0; same && i < song.channels.size(); ++i)
            {
                auto one = song;
                one.channels = { song.channels[i] };
                one.channels[0].mixerTrack = 1;
                one.mixer[2].effects.clear();
                one.patterns[0].notes = { { 0, 1920, 0, song.channels[i].instrument == lynx::InstrumentKind::drumaxx ? 48 : 60, 0.8f } };
                lynx::AudioEngine player (false);
                player.setSong (std::move (one));
                player.dispatchPendingMessages();
                if (! (player.renderOffline (1.0, 48000.0) > 1.0e-4f) && song.channels[i].instrument != lynx::InstrumentKind::granulizer)
                    ++nativesSilent;
            }
            if (nativesSilent > 0 && nativesResult == "OK")
                nativesResult = "SILENT " + juce::String (nativesSilent);
        }
        const auto nativesOk = nativesResult == "OK";

        const auto ok = audioOk && projectOk && richOk && hwOutOk && substituteOk && patcherOk && nativesOk && undoOk && recordOk && recordUndoOk && midiOk && packageOk
                        && wavpack != "ERROR" && effectsOk;

        juce::String report;
        report << "connections=" << offline.getNumGraphConnections() << "\n"
               << "peak=" << peak << "\n"
               << "project=" << (projectOk ? "OK" : "ERROR " + error) << "\n"
               << "project_features=" << (richOk ? "OK" : "ERROR") << "\n"
               << "hardware_outputs=" << (hwOutOk ? "OK" : "ERROR") << "\n"
               << "plugin_substitutes=" << (substituteOk ? "OK" : "ERROR") << "\n"
               << "patcher=" << (patcherOk ? "OK" : "ERROR") << "\n"
               << "native_engines=" << nativesResult << " (" << nativesMade << " made)\n"
               << "undo=" << (undoOk ? "OK" : "ERROR") << "\n"
               << "recording=" << (recordOk && recordUndoOk ? "OK" : "ERROR") << "\n"
               << "midi=" << (midiOk ? "OK" : "ERROR") << "\n"
               << "package=" << (packageOk ? "OK" : "ERROR") << "\n"
               << "wavpack=" << wavpack << "\n"
               << "effects=" << (effectsOk ? "OK" : "NONFINITE") << "\n"
               << "result=" << (ok ? "OK" : "FAILED") << "\n";

        if (reportPath.isNotEmpty())
            juce::File (reportPath).replaceWithText (report);

        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // Every built-in effect in its native editor: random / extreme parameters, mouse drags over the editor, the editor
    // painted, audio rendered at 44.1 / 48 / 96 kHz. Returns false when an effect output NaN / inf (muted by the
    // renderer's guard). The report lists each effect. Used by --fxfuzz and (a short run) by --selftest.
    bool runEffectFuzz (int rounds, const juce::String& reportPath, bool drags = false)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lynx::setNativeEditorDemo (true);
        juce::String report;
        auto allFinite = true;
        {
            lynx::AudioEngine offline (false);
            juce::Random random (111);
            const auto value = [&] { const auto r = random.nextInt (6); return r == 0 ? 0.0f : r == 1 ? 1.0f : random.nextFloat(); };

            for (int k = 0; k <= (int) lynx::lastEffectKind; ++k)
            {
                const auto kind = (lynx::EffectKind) k;
                if (! lynx::NativeEffectWindows::canOpen (kind) || kind == lynx::EffectKind::patcher) // (the Patcher's editor is a graph window)
                    continue;
                const auto before = lynx::RackRenderer::nonFiniteEffectBlocks();
                const auto& info = lynx::effectInfo (kind);
                // (FL engines: a fresh instance as the menus make it, from the user's FL installation; round 142)
                auto fx = lynx::natives::newEffect (kind, lynx::flStudioAccess()).value_or (lynx::makeEffectSlot (kind));
                int dragged = 0; // parameter changes made by the mouse drags

                for (int round = 0; round < rounds; ++round)
                {
                    if (round > 0)
                        for (std::size_t p = 0; p < info.params.size(); ++p)
                            fx.params[p] = round == 1 ? 0.0f : round == 2 ? 1.0f : value();
                    auto song = lynx::Song::makeDefault(); // its demo beat feeds the effect
                    song.mixer.resize (2);
                    song.mixer[0].name = "Master";
                    song.mixer[1].name = "Insert 1";
                    song.mixer[1].effects.assign (1, fx);
                    for (auto& channel : song.channels)
                        channel.mixerTrack = 1;
                    offline.setSong (std::move (song));
                    offline.dispatchPendingMessages();

                    const auto pages = juce::jmax (1, (int) lynx::plugui::layoutFor (kind).pages.size());
                    for (int page = 0; page < pages; ++page)
                    {
                        auto editor = kind == lynx::EffectKind::facetEq ? lynx::makeFacetEqEditor (offline, 1, 0, false)
                                                                        : lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, page);
                        if (editor == nullptr || editor->getWidth() <= 0 || editor->getHeight() <= 0)
                            continue;
                        juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                        const auto paint = [&] {
                            juce::Graphics g (image);
                            editor->paintEntireComponent (g, false);
                        };
                        paint();

                        // drags from a grid of points, as a user moving every control (plain left button)
                        if (round >= 3 && drags)
                        {
                            const auto source = juce::Desktop::getInstance().getMainMouseSource();
                            const auto step = juce::jmax (12, juce::jmin (editor->getWidth(), editor->getHeight()) / 8);
                            for (int y = step / 2; y < editor->getHeight(); y += step)
                                for (int x = step / 2; x < editor->getWidth(); x += step)
                                {
                                    auto* target = editor->getComponentAt (x, y);
                                    if (target == nullptr)
                                        continue;
                                    const auto at = target->getLocalPoint (editor.get(), juce::Point<float> ((float) x, (float) y));
                                    const auto now = juce::Time::getCurrentTime();
                                    const auto event = [&] (juce::Point<float> p, juce::ModifierKeys mods) {
                                        return juce::MouseEvent (source, p, mods, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, target, target, now, at, now, 1, false);
                                    };
                                    const auto down = juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier);
                                    const juce::Component::SafePointer<juce::Component> alive (target);
                                    target->mouseDown (event (at, down));
                                    const auto dy = (float) (random.nextBool() ? 400 : -400), dx = (float) (random.nextInt (401) - 200);
                                    if (alive != nullptr)
                                        target->mouseDrag (event (at + juce::Point<float> (dx * 0.5f, dy * 0.5f), down));
                                    if (alive != nullptr)
                                        target->mouseDrag (event (at + juce::Point<float> (dx, dy), down));
                                    if (alive != nullptr)
                                        target->mouseUp (event (at + juce::Point<float> (dx, dy), juce::ModifierKeys()));
                                    juce::PopupMenu::dismissAllActiveMenus();
                                    offline.dispatchPendingMessages();
                                }
                            paint();
                        }
                    }

                    for (const auto rate : { 44100.0, 48000.0, 96000.0 })
                        offline.renderOffline (0.05, rate);
                    if (round == 0 || round >= 3) // keep what the drags set for the next round
                        if (offline.getSong().mixer.size() > 1 && ! offline.getSong().mixer[1].effects.empty())
                        {
                            const auto& now = offline.getSong().mixer[1].effects[0];
                            for (std::size_t p = 0; p < fx.params.size(); ++p)
                                dragged += now.params[p] != fx.params[p] ? 1 : 0;
                            dragged += now.state != fx.state ? 1 : 0;
                            fx = now;
                        }
                }

                const auto broken = lynx::RackRenderer::nonFiniteEffectBlocks() - before;
                allFinite = allFinite && broken == 0;
                report << info.name << ": " << (broken == 0 ? juce::String ("OK") : "non-finite output in " + juce::String ((juce::int64) broken) + " blocks")
                       << " (" << dragged << " changes by mouse)\n";
                if (reportPath.isNotEmpty())
                    juce::File (reportPath).replaceWithText (report + "(running)\n"); // what was reached, if it crashes
            }
        }
        lynx::setNativeEditorDemo (false);
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        report << "result=" << (allFinite ? "OK" : "NONFINITE") << "\n";
        if (reportPath.isNotEmpty())
            juce::File (reportPath).replaceWithText (report);
        return allFinite;
    }

    // --opentest (round 157a): see the command line. Runs on the message loop (the editors' timers, animations and
    // native windows are real); one step per timer tick.
    void runOpenTest (const juce::String& reportPath, bool visible, std::function<void (bool)> done)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        struct OpenTest final : juce::Timer
        {
            juce::String reportPath, report;
            bool visible = false, ok = true;
            std::function<void (bool)> done;
            std::unique_ptr<lynx::AudioEngine> engine;
            std::vector<std::pair<juce::String, std::function<void()>>> steps;
            std::size_t next = 0;
            std::atomic<std::int64_t> lastStep { 0 };
            std::atomic<bool> finished { false };
            std::thread watchdog;
            juce::String current;
            std::mutex currentLock;
            int opened = 0, painted = 0;
            // LYNX_OPENTEST_TIMEOUT (seconds): longer for sanitizer builds
            const std::int64_t timeoutMs = 1000 * juce::jmax (5, juce::SystemStats::getEnvironmentVariable ("LYNX_OPENTEST_TIMEOUT", "20").getIntValue());

            static std::int64_t now() { return (std::int64_t) juce::Time::getMillisecondCounter(); }

            void write (const juce::String& tail = {}) { juce::File (reportPath).replaceWithText (report + tail); }

            ~OpenTest() override
            {
                finished = true;
                if (watchdog.joinable())
                    watchdog.join();
            }

            void start()
            {
                lastStep = now();
                // the UI hang watch runs during the whole test (5 s): a slow step leaves its stack next to the report
                const auto folder = juce::File (reportPath).getParentDirectory().getChildFile ("opentest-hang");
                folder.deleteRecursively();
                folder.createDirectory();
                lynx::crashguard::startHangWatch (folder.getFullPathName().replaceCharacter ('\\', '/').toStdString(), 5000);
                watchdog = std::thread ([this] {
                    while (! finished)
                    {
                        std::this_thread::sleep_for (std::chrono::milliseconds (200));
                        if (now() - lastStep.load() > timeoutMs)
                        {
                            juce::String at;
                            {
                                const std::lock_guard<std::mutex> l (currentLock);
                                at = current;
                            }
                            juce::File (reportPath).replaceWithText (report + "HANG at " + at + " (no progress for " + juce::String (timeoutMs / 1000) + " s; stack in opentest-hang)\nresult=HANG\n");
                            lynx::crashguard::exitNow (3);
                        }
                    }
                });
                startTimer (30);
            }

            // every LYNX window but none of the main one (there is none here): moved off-screen, painted once into an image
            void touchWindows (bool resize, bool large)
            {
                auto& desktop = juce::Desktop::getInstance();
                for (int i = desktop.getNumComponents(); --i >= 0;)
                    if (auto* c = desktop.getComponent (i); c != nullptr && c->isVisible())
                    {
                        if (! visible && c->getScreenX() > -3000)
                            c->setTopLeftPosition (-6000, -6000);
                        if (resize)
                            if (auto* w = dynamic_cast<juce::ResizableWindow*> (c))
                            {
                                const auto b = w->getBounds();
                                w->setSize (large ? b.getWidth() * 2 : juce::jmax (40, b.getWidth() / 3), large ? b.getHeight() * 2 : juce::jmax (40, b.getHeight() / 3));
                            }
                        if (c->getWidth() > 0 && c->getHeight() > 0 && c->getWidth() < 8000 && c->getHeight() < 8000)
                        {
                            juce::Image image (juce::Image::ARGB, c->getWidth(), c->getHeight(), true);
                            juce::Graphics g (image);
                            c->paintEntireComponent (g, false);
                            ++painted;
                            // LYNX_OPENTEST_SHOTS=folder: each window as first painted (before the resizes), for a look
                            if (const auto shots = juce::SystemStats::getEnvironmentVariable ("LYNX_OPENTEST_SHOTS", {}); shots.isNotEmpty() && ! resize)
                            {
                                juce::FileOutputStream out (juce::File (shots).getChildFile (juce::String (painted).paddedLeft ('0', 4) + ".png"));
                                if (out.openedOk())
                                {
                                    out.setPosition (0), out.truncate();
                                    juce::PNGImageFormat().writeImageToStream (c->createComponentSnapshot (c->getLocalBounds()), out);
                                }
                            }
                        }
                    }
            }

            void timerCallback() override
            {
                if (next >= steps.size())
                {
                    stopTimer();
                    lynx::NativeEffectWindows::closeAll();
                    lynx::closeChannelSettings();
                    // the UI hang watch itself: the message thread spins past the stall time; the watcher must write the stack
                    {
                        const auto before = lynx::crashguard::lastHangFile();
                        for (const auto until = now() + 7000; now() < until;)
                            lastStep = now(); // (busy: no heartbeat; the test's own watchdog stays quiet)
                        for (const auto until = now() + 1500; now() < until && lynx::crashguard::lastHangFile() == before;)
                            std::this_thread::sleep_for (std::chrono::milliseconds (20));
                        lynx::crashguard::stopHangWatch();
                        const juce::File hang (juce::String::fromUTF8 (lynx::crashguard::lastHangFile().c_str()));
                        const auto text = hang.loadFileAsString();
                        const auto frames = juce::StringArray::fromLines (text).size() - 2;
                       #if JUCE_WINDOWS
                        const auto hangOk = lynx::crashguard::lastHangFile() != before && hang.existsAsFile() && text.contains ("LYNX.exe+0x") && frames >= 3;
                       #else
                        const auto hangOk = lynx::crashguard::lastHangFile() != before && hang.existsAsFile() && frames >= 3;
                       #endif
                        report << "hang watch: " << (hangOk ? "OK" : "FAIL") << " (" << hang.getFileName() << ", " << frames << " lines"
                               << (before.empty() ? juce::String() : "; a step stalled > 5 s earlier: " + juce::String::fromUTF8 (before.c_str())) << ")\n";
                        ok = ok && hangOk;
                    }
                    report << "windows opened: " << opened << ", paints: " << painted << "\nresult=" << (ok ? "OK" : "FAIL") << "\n";
                    write();
                    finished = true;
                    if (done)
                        done (ok);
                    return;
                }
                auto& [label, fn] = steps[next++];
                {
                    const std::lock_guard<std::mutex> l (currentLock);
                    current = label;
                }
                fn();
                lastStep = now();
                lynx::crashguard::heartbeat();
            }
        };

        auto* test = new OpenTest();
        test->reportPath = reportPath;
        test->visible = visible;
        test->engine = std::make_unique<lynx::AudioEngine> (false);
        test->done = [test, done] (bool ok) {
            juce::MessageManager::callAsync ([test, done, ok] {
                delete test;
                juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
                done (ok);
            });
        };
        auto& engine = *test->engine;
        lynx::setNativeEditorDemo (true); // live displays animate without an audio device

        const auto songWith = [] (const lynx::EffectSlot& fx) {
            auto song = lynx::Song::makeDefault();
            song.mixer.resize (2);
            song.mixer[0].name = "Master";
            song.mixer[1].name = "Insert 1";
            song.mixer[1].effects.assign (1, fx);
            for (auto& channel : song.channels)
                channel.mixerTrack = 1;
            return song;
        };
        // an older project's slot: its parameter list cut to `count` and read back by the project reader (the
        // parameters a newer LYNX added after them take their defaults)
        const auto oldState = [songWith] (const lynx::EffectSlot& fx, std::size_t count) -> std::optional<lynx::Song> {
            auto json = juce::JSON::parse (lynx::ProjectFile::toJson (songWith (fx)));
            if (auto* mixer = json["mixer"].getArray(); mixer != nullptr && mixer->size() > 1)
                if (auto* effects = (*mixer)[1]["effects"].getArray(); effects != nullptr && ! effects->isEmpty())
                    if (auto* params = (*effects)[0]["params"].getArray())
                        params->removeRange ((int) count, params->size());
            juce::String error;
            return lynx::ProjectFile::fromJson (juce::JSON::toString (json), error);
        };

        auto& steps = test->steps;
        juce::Random random (157);
        for (int k = 0; k <= (int) lynx::lastEffectKind; ++k)
        {
            const auto kind = (lynx::EffectKind) k;
            if (! lynx::NativeEffectWindows::canOpen (kind) || kind == lynx::EffectKind::patcher)
                continue;
            const auto& info = lynx::effectInfo (kind);
            const auto base = lynx::natives::newEffect (kind, lynx::flStudioAccess()).value_or (lynx::makeEffectSlot (kind));
            const auto n = info.params.size();
            std::vector<std::pair<juce::String, std::function<std::optional<lynx::Song>()>>> variants;
            variants.push_back ({ "default", [=] { return std::optional<lynx::Song> (songWith (base)); } });
            for (const auto value : { 0.0f, 1.0f })
                variants.push_back ({ value == 0.0f ? "all 0" : "all 1", [=] {
                                         auto fx = base;
                                         for (std::size_t p = 0; p < n; ++p) fx.params[p] = value;
                                         return std::optional<lynx::Song> (songWith (fx));
                                     } });
            const auto seed = random.nextInt64();
            variants.push_back ({ "random", [=] {
                                     juce::Random r (seed);
                                     auto fx = base;
                                     for (std::size_t p = 0; p < n; ++p) fx.params[p] = r.nextFloat();
                                     return std::optional<lynx::Song> (songWith (fx));
                                 } });
            std::vector<std::size_t> cuts { n / 2 };
            for (const auto c : { (std::size_t) 90, (std::size_t) 114, (std::size_t) 128 }) // (Facet's 90 → 114 params; the 128 → 160 slot)
                if (c < n) cuts.push_back (c);
            for (const auto c : cuts)
                variants.push_back ({ "old project (" + juce::String ((int) c) + " of " + juce::String ((int) n) + " params)", [=] { return oldState (base, c); } });

            for (const auto& [variant, makeSong] : variants)
            {
                const auto label = juce::String::fromUTF8 (info.name) + " / " + variant;
                steps.push_back ({ label + ": open", [test, &engine, label, makeSong = makeSong] {
                                      auto song = makeSong();
                                      if (! song)
                                      {
                                          test->report << "FAIL " << label << ": project reader refused the state\n";
                                          test->ok = false;
                                          return;
                                      }
                                      engine.setSong (std::move (*song));
                                      engine.dispatchPendingMessages();
                                      lynx::NativeEffectWindows::open (engine, 1, 0);
                                      ++test->opened;
                                      test->touchWindows (false, false);
                                  } });
                steps.push_back ({ label + ": run", [test, &engine] {
                                      for (const auto rate : { 44100.0, 48000.0, 96000.0 })
                                          engine.renderOffline (0.03, rate);
                                      test->touchWindows (false, false);
                                  } });
                steps.push_back ({ label + ": small", [test] { test->touchWindows (true, false); } });
                steps.push_back ({ label + ": large", [test] { test->touchWindows (true, true); } });
                steps.push_back ({ label + ": close", [test, label] {
                                      lynx::NativeEffectWindows::closeAll();
                                      test->report << label << ": OK\n";
                                      test->write ("(running)\n");
                                  } });
            }
        }

        // instruments: the channel window the Channel rack opens (Patcher: its graph window, not here)
        for (int k = 0; k <= (int) lynx::InstrumentKind::soundfont; ++k)
        {
            const auto kind = (lynx::InstrumentKind) k;
            if (kind == lynx::InstrumentKind::plugin || kind == lynx::InstrumentKind::patcher)
                continue;
            for (const auto* variant : { "default", "old project (saved and read back)", "odd settings" })
            {
                if (juce::String (variant) == "odd settings" && kind != lynx::InstrumentKind::soundfont)
                    continue;
                const auto label = "instrument " + juce::String (k) + " / " + variant;
                steps.push_back ({ label + ": open", [test, &engine, kind, label, variant = juce::String (variant), old = juce::String (variant) != "default"] {
                                      auto song = lynx::Song::makeDefault();
                                      auto channel = lynx::natives::makeChannel (kind, lynx::flStudioAccess());
                                      lynx::ChannelDef def;
                                      def.instrument = kind;
                                      def.name = "Test";
                                      song.channels.push_back (channel ? *channel : def);
                                      if (kind == lynx::InstrumentKind::soundfont && variant != "default")
                                      {
                                          // a real font when FL's is there; NaN / huge / truncated settings
                                          const auto& roots = lynx::flStudioAccess().roots;
                                          if (! roots.data.empty())
                                              song.channels.back().samplePath = (juce::File (juce::String::fromUTF8 (roots.data.c_str()))
                                                                                     .getChildFile ("Patches/Soundfonts/STR_Ensemble.sf2"))
                                                                                    .getFullPathName().toStdString();
                                          if (variant == "odd settings")
                                              song.channels.back().modelParams = { NAN, 1.0e9f, -1.0e9f, 0.5f, 7.0f };
                                      }
                                      if (old)
                                      {
                                          juce::String error;
                                          if (auto again = lynx::ProjectFile::fromJson (lynx::ProjectFile::toJson (song), error))
                                              song = std::move (*again);
                                      }
                                      const auto index = (int) song.channels.size() - 1;
                                      engine.setSong (std::move (song));
                                      engine.dispatchPendingMessages();
                                      lynx::openChannelSettings (engine, index);
                                      ++test->opened;
                                      test->touchWindows (false, false);
                                  } });
                steps.push_back ({ label + ": run", [test, &engine] {
                                      engine.renderOffline (0.03, 48000.0);
                                      test->touchWindows (false, false);
                                  } });
                steps.push_back ({ label + ": small", [test] { test->touchWindows (true, false); } });
                steps.push_back ({ label + ": large", [test] { test->touchWindows (true, true); } });
                steps.push_back ({ label + ": close", [test, label] {
                                      lynx::closeChannelSettings();
                                      test->report << label << ": OK\n";
                                      test->write ("(running)\n");
                                  } });
            }
        }
        test->start();
    }

    void runPlugWinTest (const juce::String& pluginPath, const juce::String& reportPath, const juce::String& only, int seconds)
    {
        struct Test final : juce::Timer
        {
            juce::String reportPath, report;
            std::unique_ptr<lynx::AudioEngine> engine;
            bool instrument = false;
            int phase = 0, ticks = 0, seconds = 15;
            juce::AudioPluginInstance* instance = nullptr;
            juce::AudioProcessorParameter* checked = nullptr;
            std::unique_ptr<juce::DocumentWindow> mainWindow; // stands for LYNX's main window (window ownership)
            std::function<void (bool)> done;

            void step (const juce::String& line)
            {
                report << line << "\n";
                juce::File (reportPath).replaceWithText (report);
            }

            juce::AudioPluginInstance* find()
            {
                if (instrument)
                {
                    if (auto* p = dynamic_cast<lynx::PluginInstrument*> (engine->getChannelInstrument (0).get()))
                        return &p->getInstance();
                }
                else if (auto* p = dynamic_cast<lynx::PluginEffect*> (engine->getEffect (1, 0).get()))
                    return &p->getInstance();
                return nullptr;
            }

            void timerCallback() override
            {
                ++ticks;
                if (phase == 0) // waiting for the engine to host the plugin
                {
                    instance = find();
                    if (instance == nullptr)
                    {
                        if (ticks > 200)
                        {
                            step ("result=FAILED (not hosted)");
                            stopTimer();
                            done (false);
                        }
                        return;
                    }
                    step ("hosted=" + instance->getName() + " format=" + instance->getPluginDescription().pluginFormatName);
                    step ("programs=" + juce::String (instance->getNumPrograms()));
                    for (int p = 0; p < juce::jmin (5, instance->getNumPrograms()); ++p)
                        step ("program" + juce::String (p) + "=" + instance->getProgramName (p));
                    step ("save_formats=" + lynx::pluginpresetui::saveWildcard (*instance));

                    // preset round trip: save, change the first parameter, load, compare
                    juce::AudioProcessorParameter* param = nullptr;
                    for (auto* candidate : instance->getParameters())
                        if (candidate->isAutomatable() && ! candidate->isMetaParameter() && candidate != instance->getBypassParameter())
                        {
                            param = candidate;
                            break;
                        }
                    if (param != nullptr)
                    {
                        step ("param=" + param->getName (64));
                        checked = param;
                        const auto file = lynx::pluginpresetui::folderFor (instance->getName()).getChildFile ("plugwintest.lynxpreset");
                        juce::String error;
                        const auto before = param->getValue();
                        const auto saved = lynx::pluginpresetui::save (*instance, file, "plugwintest", error);
                        param->setValueNotifyingHost (before > 0.5f ? 0.1f : 0.9f);
                        const auto changed = param->getValue();
                        const auto loaded = saved && lynx::pluginpresetui::load (*instance, file, error);
                        step ("preset_roundtrip saved=" + juce::String ((int) saved) + " loaded=" + juce::String ((int) loaded) + " param0 before="
                              + juce::String (before, 4) + " changed=" + juce::String (changed, 4) + " after=" + juce::String (param->getValue(), 4)
                              + (error.isNotEmpty() ? " error=" + error : juce::String()));
                        for (const auto* ext : { "fxp", "fxb", "vstpreset" })
                        {
                            juce::String e2;
                            const auto f2 = file.withFileExtension (ext);
                            const auto ok2 = lynx::pluginpresetui::save (*instance, f2, "plugwintest", e2) && lynx::pluginpresetui::load (*instance, f2, e2);
                            step (juce::String ("preset_") + ext + "=" + juce::String ((int) ok2) + (e2.isNotEmpty() ? " (" + e2 + ")" : juce::String()));
                            f2.deleteFile();
                        }
                        step ("browser_lists=" + juce::String (lynx::pluginpresetui::userPresets (instance->getName()).size()));
                    }

                    std::shared_ptr<void> owner = instrument ? std::shared_ptr<void> (engine->getChannelInstrument (0))
                                                             : std::shared_ptr<void> (engine->getEffect (1, 0));
                    lynx::PluginWindows::open (owner, *instance, instance->getName(),
                                               { engine.get(), [this] (lynx::Song& song) -> lynx::PluginRef* {
                                                    if (instrument)
                                                        return song.channels.empty() ? nullptr : &song.channels[0].plugin;
                                                    return song.mixer.size() < 2 || song.mixer[1].effects.empty() ? nullptr : &song.mixer[1].effects[0].plugin;
                                                }, instrument });
                    owner.reset();
                    step ("window_open=" + juce::String ((int) lynx::PluginWindows::isOpen (*instance)));
                    phase = 1;
                    ticks = 0;
                    return;
                }
                if (phase == 1 && ticks == 20 && checked != nullptr)
                    step ("param_after_1s=" + juce::String (checked->getValue(), 4));
                if (phase == 1 && ticks >= seconds * 20) // remove the plugin from the song
                {
                    engine->editSong ([this] (lynx::Song& song) {
                        if (instrument)
                            song.channels.clear();
                        else
                            song.mixer[1].effects.clear();
                    });
                    step ("removed");
                    phase = 2;
                    ticks = 0;
                    return;
                }
                if (phase == 2 && ticks >= 40)
                {
                    const auto open = lynx::PluginWindows::isOpen (*instance);
                    step ("window_open_after_remove=" + juce::String ((int) open));
                    step (open ? "result=FAILED" : "result=OK");
                    stopTimer();
                    lynx::PluginWindows::closeAll();
                    done (! open);
                }
            }
        };

        auto* test = new Test();
        test->reportPath = reportPath;
        test->seconds = seconds;
        test->done = [this, test] (bool ok) {
            setApplicationReturnValue (ok ? 0 : 1);
            juce::MessageManager::callAsync ([this, test] {
                lynx::setMainWindowForOwnership (nullptr);
                test->mainWindow = nullptr;
                test->engine = nullptr;
                delete test;
                quit();
            });
        };
        test->engine = std::make_unique<lynx::AudioEngine> (false);
        test->mainWindow = std::make_unique<juce::DocumentWindow> ("LYNX test main window", juce::Colours::darkgrey, 0);
        test->mainWindow->setBounds (20, 20, 300, 200);
        test->mainWindow->setVisible (true);
        lynx::setMainWindowForOwnership (test->mainWindow.get());
        auto& host = test->engine->getPluginHost();
        test->step ("plugins_in_file=" + juce::String (host.addPluginFile (juce::File (pluginPath))));
        std::optional<juce::PluginDescription> chosen;
        for (const auto& type : host.getKnownPlugins().getTypes())
            if ((juce::File (type.fileOrIdentifier).getFileName() == juce::File (pluginPath).getFileName() || type.fileOrIdentifier == pluginPath)
                && (only.isEmpty() || type.name == only))
            {
                chosen = type;
                break;
            }
        if (! chosen)
        {
            test->step ("result=FAILED (no plugin)");
            test->done (false);
            return;
        }
        test->instrument = chosen->isInstrument;
        lynx::PluginRef ref;
        ref.id = chosen->createIdentifierString().toStdString();
        ref.name = chosen->name.toStdString();
        test->engine->editSong ([&] (lynx::Song& song) {
            if (test->instrument)
            {
                lynx::ChannelDef c;
                c.name = ref.name;
                c.instrument = lynx::InstrumentKind::plugin;
                c.plugin = ref;
                song.channels.push_back (c);
            }
            else
            {
                while (song.mixer.size() < 2)
                    song.mixer.emplace_back();
                lynx::EffectSlot slot;
                slot.kind = lynx::EffectKind::plugin;
                slot.plugin = ref;
                song.mixer[1].effects.push_back (slot);
            }
        });
        test->step ("plugin=" + chosen->name + " instrument=" + juce::String ((int) test->instrument));
        test->startTimerHz (20);
    }

    void runEditorTest (const juce::String& pluginPath, const juce::String& reportPath, const juce::String& only)
    {
        struct EditorTest final : juce::Timer
        {
            juce::String reportPath, report;
            std::unique_ptr<lynx::PluginHost> host;
            std::unique_ptr<juce::AudioPluginInstance> plugin;
            std::unique_ptr<juce::DocumentWindow> window;
            std::function<void (bool)> done;

            void step (const juce::String& line)
            {
                report << line << "\n";
                juce::File (reportPath).replaceWithText (report); // written at each step: a crash leaves the last one
            }

            void timerCallback() override
            {
                stopTimer();
                window = nullptr;
                step ("step=closed");
                plugin = nullptr;
                step ("result=OK");
                done (true);
            }
        };

        auto* test = new EditorTest();
        test->reportPath = reportPath;
        test->done = [this, test] (bool ok)
        {
            setApplicationReturnValue (ok ? 0 : 1);
            juce::MessageManager::callAsync ([this, test] { delete test; quit(); });
        };
        test->host = std::make_unique<lynx::PluginHost> (false);
        test->step ("plugins_in_file=" + juce::String (test->host->addPluginFile (juce::File (pluginPath))));
        test->step ("exe=" + juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());

        for (const auto& type : test->host->getKnownPlugins().getTypes())
        {
            if (juce::File (type.fileOrIdentifier).getFileName() != juce::File (pluginPath).getFileName()
                || (only.isNotEmpty() && type.name != only))
                continue;

            juce::String error;
            test->plugin = lynx::createRightInstance (test->host->getFormats(), type, 48000.0, 512, error);
            test->step ("plugin=" + type.name + (test->plugin == nullptr ? " error=" + error : juce::String()));
            break;
        }

        if (test->plugin == nullptr)
        {
            test->step ("result=FAILED");
            test->done (false);
            return;
        }

        lynx::preparePlugin (*test->plugin, 48000.0, 512);
        test->step ("step=create_editor");
        test->window = std::make_unique<juce::DocumentWindow> (test->plugin->getName(), juce::Colours::black, juce::DocumentWindow::closeButton);
        test->window->setUsingNativeTitleBar (true);
        if (auto* editor = test->plugin->createEditorIfNeeded())
            test->window->setContentOwned (editor, true);
        else
            test->step ("no_editor");
        test->step ("step=show");
        test->window->centreWithSize (test->window->getWidth(), test->window->getHeight());
        test->window->setVisible (true);
        test->step ("step=shown");
        test->startTimer (4000);
    }

    void runFitTest (const juce::String& reportPath, int percent)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        struct FitTest final : juce::Timer
        {
            juce::String path, report;
            bool ok = true;
            int step = 0, percent = 150;
            std::unique_ptr<juce::DocumentWindow> tall, big;
            std::function<void()> done;

            // `whole`: the window must fit entirely (a resizable one); else its title bar and close button must be on the screen
            void check (const juce::String& what, juce::Component& w, bool whole)
            {
                juce::BorderSize<int> frame;
                if (auto* peer = w.getPeer())
                    if (const auto f = peer->getFrameSizeIfPresent())
                        frame = *f;
                auto outer = frame.addedTo (w.getBounds());
                const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect (outer);
                const auto area = display != nullptr ? display->userArea : outer;
               #if JUCE_LINUX
                // round 164: a compositor that draws the frame itself (WSLg's Weston) reports its invisible shadow in
                // the frame (59 px on top, 38 around at 100%): only what is visible is checked, give or take 4 px
                const auto shadow = frame.getLeft() >= 16 ? frame.getLeft() : 0;
                outer = outer.reduced (shadow);
                const auto slack = shadow > 0 ? 4 : 0;
               #else
                const auto slack = 0;
               #endif
                const auto passed = outer.getY() >= area.getY() - slack && outer.getRight() <= area.getRight() + slack && outer.getX() >= area.getX() - slack
                                    && (! whole || outer.getBottom() <= area.getBottom() + 1 + slack);
                report << (passed ? "ok   " : "FAIL ") << what << " window=" << outer.toString() << " (frame " << frame.getTop() << ","
                       << frame.getLeft() << ") screen=" << area.toString() << "\n";
                ok = ok && passed;
            }

            void start()
            {
                report << "scale=" << percent << "%\n";
                juce::Desktop::getInstance().setGlobalScaleFactor ((float) percent / 100.0f);
                // a tool window taller than the screen, centred (its title bar would be above the screen)
                tall = std::make_unique<juce::DocumentWindow> ("LYNX fit test - tall", lynx::theme::colour::bg2, juce::DocumentWindow::closeButton);
                tall->setUsingNativeTitleBar (true);
                auto* content = new juce::Component();
                const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
                content->setSize (500, area.getHeight() + 200);
                tall->setContentOwned (content, true);
                tall->setCentrePosition (area.getCentre());
                tall->setVisible (true);
                lynx::keepAboveMainWindow (*tall);
                // LYNX_FITTEST_MS: how long each window stays (default 700 ms; longer to take screenshots of them)
                startTimer (juce::jlimit (300, 60000, juce::SystemStats::getEnvironmentVariable ("LYNX_FITTEST_MS", "700").getIntValue()));
            }

            void timerCallback() override
            {
                ++step;
                if (step == 1)
                {
                    check ("tall tool window: title bar and close button on the screen", *tall, false);
                    tall.reset();
                    // round 164: a fixed-size LYNX tool window whose content is taller than the screen: it scrolls
                    auto content = std::make_unique<juce::Component>();
                    const auto screen = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
                    content->setSize (520, screen.getHeight() + 300);
                    lynx::openToolWindow (std::move (content), "LYNX fit test - fixed size", "fittest-fixed");
                    return;
                }
                if (step == 2)
                {
                    if (auto* w = lynx::findToolWindow ("fittest-fixed"))
                    {
                        check ("fixed-size tool window taller than the screen: fits (its content scrolls)", *w, true);
                        const auto scrolls = w->getContentComponent() != nullptr && w->getContentComponent()->getNumChildComponents() > 0
                                             && dynamic_cast<juce::Viewport*> (w->getContentComponent()->getChildComponent (0)) != nullptr;
                        report << (scrolls ? "ok   " : "FAIL ") << "its content is in a scrolling viewport\n";
                        ok = ok && scrolls;
                    }
                    else
                    {
                        report << "FAIL fixed-size tool window not found\n";
                        ok = false;
                    }
                    lynx::closeAllToolWindows();
                    // a resizable window that fits at 100%, then the zoom goes up (the main window's case)
                    juce::Desktop::getInstance().setGlobalScaleFactor (1.0f);
                    big = std::make_unique<juce::DocumentWindow> ("LYNX fit test - zoom", lynx::theme::colour::bg2, juce::DocumentWindow::allButtons);
                    big->setUsingNativeTitleBar (true);
                    big->setResizable (true, false);
                    big->setResizeLimits (400, 300, 10000, 10000);
                    const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
                    big->setBounds (area.getX() + 40, area.getY() + 60, area.getWidth() * 9 / 10 - 40, area.getHeight() * 9 / 10 - 60);
                    big->setVisible (true);
                    return;
                }
                if (step == 3)
                {
                    juce::Desktop::getInstance().setGlobalScaleFactor ((float) percent / 100.0f);
                    juce::Timer::callAfterDelay (200, [] { lynx::fitAllWindowsOnScreen(); });
                    return;
                }
                if (step == 4)
                {
                    check ("resizable window after the zoom change: fits the screen", *big, true);
                    big.reset();
                    report << "result=" << (ok ? "OK" : "FAILED") << "\n";
                    juce::File (path).replaceWithText (report);
                    stopTimer();
                    if (done)
                        done();
                }
            }
        };
        static std::unique_ptr<FitTest> test;
        test = std::make_unique<FitTest>();
        test->path = reportPath;
        test->percent = juce::jlimit (50, 300, percent);
        test->done = [this] { juce::MessageManager::callAsync ([this] { test.reset(); quit(); }); };
        test->start();
    }

    void runWindowTest (const juce::String& reportPath)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        struct WindowTest final : juce::Timer
        {
            juce::String reportPath, report;
            bool ok = true;
            int stepIndex = 0;
            std::unique_ptr<lynx::AudioEngine> engine;
            lynx::MainComponent* ui = nullptr;
            std::unique_ptr<juce::DocumentWindow> main, pluginLike;
            juce::TextEditor* pluginField = nullptr;
            std::function<void()> done;

            void line (const juce::String& text)
            {
                report << text << "\n";
                juce::File (reportPath).replaceWithText (report);
            }

            void check (const juce::String& what, bool passed)
            {
                line ((passed ? "ok   " : "FAIL ") + what);
                ok = ok && passed;
            }

            bool isAbove (juce::Component* w) { return w != nullptr && lynx::isAboveMainWindow (*w); }

            bool focusIn (juce::Component* w)
            {
                auto* f = juce::Component::getCurrentlyFocusedComponent();
                return w != nullptr && f != nullptr && (f == w || w->isParentOf (f));
            }

            void timerCallback() override
            {
                switch (stepIndex++)
                {
                    case 0:
                        line ("foreground=" + juce::String (juce::Process::isForegroundProcess() ? 1 : 0));
                        ui->openQuickAddForTest();
                        break;
                    case 1:
                    {
                        auto* w = lynx::findToolWindow ("quickadd");
                        check ("quick-add palette open", w != nullptr);
                        check ("quick-add palette active", w != nullptr && w->isActiveWindow());
                        check ("quick-add palette owned by the main window", w != nullptr && lynx::isOwnedByMainWindow (*w));
                        check ("quick-add palette above the main window", isAbove (w));
                        auto* f = juce::Component::getCurrentlyFocusedComponent();
                        check ("quick-add search box has the keyboard focus", focusIn (w) && dynamic_cast<juce::TextEditor*> (f) != nullptr && f->hasKeyboardFocus (false) && w->isActiveWindow());
                        lynx::closeAllToolWindows();
                        lynx::openAudioEditor (*engine);
                        break;
                    }
                    case 2:
                    {
                        auto* w = lynx::findToolWindow ("audioeditor");
                        check ("audio editor open", w != nullptr);
                        check ("audio editor active", w != nullptr && w->isActiveWindow());
                        check ("audio editor owned by the main window", w != nullptr && lynx::isOwnedByMainWindow (*w));
                        check ("audio editor above the main window", isAbove (w));
                        lynx::forceForegroundForTest (*main); // the user clicks the main window
                        break;
                    }
                    case 3:
                    {
                        auto* w = lynx::findToolWindow ("audioeditor");
                        check ("audio editor stays above the main window after clicking it", isAbove (w));
                        lynx::closeAllToolWindows();
                        // a plugin editor's window: its own controls keep the keyboard focus
                        pluginLike = std::make_unique<juce::DocumentWindow> ("Plugin", juce::Colours::black, juce::DocumentWindow::closeButton);
                        pluginLike->setUsingNativeTitleBar (true);
                        auto* field = new juce::TextEditor();
                        field->setSize (300, 120);
                        pluginLike->setContentOwned (field, true);
                        pluginLike->centreWithSize (300, 120);
                        pluginLike->setVisible (true);
                        lynx::keepAboveMainWindow (*pluginLike);
                        pluginLike->toFront (true);
                        field->grabKeyboardFocus();
                        pluginField = field;
                        break;
                    }
                    case 4:
                        check ("plugin window active", pluginLike->isActiveWindow());
                        check ("plugin window keeps the keyboard focus", pluginField->hasKeyboardFocus (false) && pluginLike->isActiveWindow());
                        check ("plugin window above the main window", isAbove (pluginLike.get()));
                        pluginLike = nullptr;
                        lynx::forceForegroundForTest (*main);
                        juce::Component::unfocusAllComponents();
                        break;
                    case 5:
                        check ("main window takes the keyboard focus back when it is active", ui->hasKeyboardFocus (true));
                        line (ok ? "result=OK" : "result=FAILED");
                        stopTimer();
                        done();
                        break;
                    default:
                        break;
                }
            }
        };

        auto* test = new WindowTest();
        test->reportPath = reportPath;
        test->engine = std::make_unique<lynx::AudioEngine> (false);
        test->main = std::make_unique<juce::DocumentWindow> ("LYNX window test", lynx::theme::window, juce::DocumentWindow::allButtons);
        test->main->setUsingNativeTitleBar (true);
        test->ui = new lynx::MainComponent (*test->engine, false);
        test->main->setContentOwned (test->ui, true);
        test->main->setResizable (true, true);
        test->main->centreWithSize (1280, 760);
        test->main->setVisible (true);
        lynx::setMainWindowForOwnership (test->main.get());
        lynx::forceForegroundForTest (*test->main);
        test->main->toFront (true);
        test->done = [this, test] {
            setApplicationReturnValue (test->ok ? 0 : 1);
            juce::MessageManager::callAsync ([this, test] {
                lynx::closeAllToolWindows();
                lynx::setMainWindowForOwnership (nullptr);
                test->main = nullptr;
                delete test;
                quit();
            });
        };
        test->startTimer (1500);
    }

    // LYNX.exe --plugintest plugin.vst3 report.txt ["name"]: loads the plugin (or the one named in a shell), plays notes if it is an
    // instrument (and runs audio through it if it is an effect) and measures the audio.
    void runPluginTest (const juce::String& pluginPath, const juce::String& reportPath, const juce::String& only = {})
    {
        juce::String report;
        bool ok = false;
        {
            lynx::AudioEngine offline (false);
            auto& host = offline.getPluginHost();
            const auto added = host.addPluginFile (juce::File (pluginPath));
            report << "plugins_in_file=" << added << "\n";

            juce::PluginDescription instrument, effect;
            bool hasInstrument = false, hasEffect = false;

            for (const auto& type : host.getKnownPlugins().getTypes())
            {
                if (juce::File (type.fileOrIdentifier).getFileName() != juce::File (pluginPath).getFileName()
                    || (only.isNotEmpty() && type.name != only))
                    continue;

                report << "  " << (type.isInstrument ? "instrument: " : "effect: ") << type.name << " (" << type.manufacturerName
                       << ") in " << type.fileOrIdentifier << "\n";

                if (type.isInstrument && ! hasInstrument) { instrument = type; hasInstrument = true; }
                if (! type.isInstrument && ! hasEffect) { effect = type; hasEffect = true; }
            }

            auto song = lynx::Song::makeDefault();

            if (hasInstrument)
            {
                auto& c = song.channels[4]; // the "Synth" channel becomes the plugin
                c.instrument = lynx::InstrumentKind::plugin;
                c.plugin.id = instrument.createIdentifierString().toStdString();
                c.plugin.name = instrument.name.toStdString();
                c.volume = 1.0f;

                for (int i = 0; i < 4; ++i) // one chord per beat
                    for (int key : { 60, 64, 67 })
                        song.patterns[0].notes.push_back ({ (std::int64_t) i * lynx::ppq, lynx::ppq / 2, 4, key, 0.9f });
                song.patterns[0].sortNotes();
            }

            if (hasEffect)
            {
                lynx::EffectSlot slot;
                slot.kind = lynx::EffectKind::plugin;
                slot.plugin.id = effect.createIdentifierString().toStdString();
                slot.plugin.name = effect.name.toStdString();
                song.mixer[0].effects.push_back (slot);
            }

            const auto effectSlot = (int) song.mixer[0].effects.size() - 1; // (the default master may hold effects already)
            offline.setSong (song);

            // after a render: the rack (and its plugins) is built when the engine is prepared
            const auto peak = offline.renderOffline (3.0, 48000.0);
            const auto* loadedInstrument = dynamic_cast<lynx::PluginInstrument*> (offline.getChannelInstrument (4).get());
            const auto* loadedEffect = dynamic_cast<lynx::PluginEffect*> (offline.getEffect (0, effectSlot).get());
            report << "instrument_loaded=" << (loadedInstrument != nullptr ? "yes" : "no") << "\n"
                   << "effect_loaded=" << (loadedEffect != nullptr ? "yes" : "no") << "\n";
            if (host.getLoadFailures().size() > 0) // why: the host's own message (license, missing file, load failure)
                report << "load_failures=" << host.getLoadFailures().joinIntoString (" | ") << "\n";
            report << "peak=" << peak << "\n";
            // plugins that crashed inside a call, were caught and switched off (round 111)
            report << "plugin_crashes=" << lynx::pluginCrashCount() << "\n";
            for (const auto& c : lynx::pluginCrashes())
                report << "  crashed: " << c.plugin << " (" << lynx::crashguard::describe (c.code) << ")\n";

            offline.capturePluginStates();
            const auto stateSize = hasInstrument ? offline.getSong().channels[4].plugin.state.size() : (size_t) 0;
            report << "state_bytes_base64=" << (int) stateSize << "\n";

            ok = (hasInstrument || hasEffect) && (! hasInstrument || loadedInstrument != nullptr)
                 && (! hasEffect || loadedEffect != nullptr) && peak > 0.001f && peak < 10.0f;
        }

        report << "result=" << (ok ? "OK" : "FAILED") << "\n";
        juce::File (reportPath).replaceWithText (report);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // LYNX.exe --vst2test "LYNX Test Synth.dll" report.txt (tools/testplugin/lynx_test_vst2.c): LYNX's own VST2 host.
    // Scans the DLL, imports it the way an .flp holds it (Fruity Wrapper VST2 state with a chunk, REA_rodada89 §2),
    // renders with two saved gains (the level must follow the restored state) and checks the plugin saw the song tempo.
    void runVst2Test (const juce::String& pluginPath, const juce::String& reportPath)
    {
        juce::String report;
        bool ok = false;
        {
            lynx::AudioEngine offline (false);
            auto& host = offline.getPluginHost();
            report << "plugins_in_file=" << host.addPluginFile (juce::File (pluginPath)) << "\n";
            std::optional<juce::PluginDescription> vst2;
            for (const auto& type : host.getKnownPlugins().getTypes())
                if (type.pluginFormatName == "VST")
                    vst2 = type;

            auto render = [&] (float gain, float& tempoSeen, int& adopted) {
                lynx::FlVst2State state;
                state.flags = 8;
                state.chunk = { 'L', 'X', 'V', '2', 0, 0, 0, 0 };
                std::memcpy (state.chunk.data() + 4, &gain, 4);
                auto song = lynx::Song::makeDefault();
                song.bpm = 137.0;
                auto& f = song.channels[4].plugin.foreign;
                f.format = lynx::ForeignPlugin::Format::vst2;
                f.vst2Id = (std::uint32_t) vst2->uniqueId;
                f.name = "Some other name"; // found by the VST2 ID
                f.state = lynx::writeFlVst2State (state);
                song.channels[4].volume = 1.0f;
                song.patterns[0].notes.clear();
                song.patterns[0].notes.push_back ({ 0, 4 * lynx::ppq, 4, 69, 1.0f });
                for (std::size_t c = 0; c < song.channels.size(); ++c)
                    if (c != 4)
                        song.channels[c].muted = true;
                adopted = host.adoptFlPlugins (song);
                offline.setSong (song);
                const auto peak = offline.renderOffline (1.0, 48000.0);
                offline.capturePluginStates();
                juce::MemoryBlock saved;
                saved.fromBase64Encoding (juce::String (offline.getSong().channels[4].plugin.state));
                lynx::FlVst2State back;
                tempoSeen = 0.0f;
                if (lynx::parseFlVst2State (std::vector<std::uint8_t> ((const std::uint8_t*) saved.getData(), (const std::uint8_t*) saved.getData() + saved.getSize()), back)
                    && back.chunk.size() >= 12)
                    std::memcpy (&tempoSeen, back.chunk.data() + 8, 4);
                return peak;
            };

            if (vst2)
            {
                report << "plugin=" << vst2->name << " id=" << vst2->uniqueId << " instrument=" << (int) vst2->isInstrument << "\n";
                float tempoA = 0, tempoB = 0;
                int adoptedA = 0, adoptedB = 0;
                const auto quiet = render (0.1f, tempoA, adoptedA);
                const auto loud = render (0.4f, tempoB, adoptedB);
                report << "adopted=" << adoptedA << "," << adoptedB << " peak_gain_0.1=" << quiet << " peak_gain_0.4=" << loud
                       << " tempo_seen=" << tempoB * 1000.0f << "\n";
                ok = adoptedA == 1 && adoptedB == 1 && quiet > 0.01f && std::abs (loud / quiet - 4.0f) < 0.2f
                     && std::abs (tempoB * 1000.0f - 137.0f) < 0.01f;
            }
        }

        report << "result=" << (ok ? "OK" : "FAILED") << "\n";
        juce::File (reportPath).replaceWithText (report);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // LYNX.exe --wrappertest plugin.vst3 report.txt: builds a VST3 state the way FL's Fruity
    // Wrapper stores it (REA_rodada5 §3.2), runs it through the import (adoptFlPlugins) and checks
    // that the plugin comes back with the changed parameter.
    void runWrapperTest (const juce::String& pluginPath, const juce::String& reportPath)
    {
        juce::String report;
        bool ok = false;
        {
            lynx::AudioEngine offline (false);
            auto& host = offline.getPluginHost();
            host.addPluginFile (juce::File (pluginPath));
            const juce::PluginDescription* vst3 = nullptr;
            const auto types = host.getKnownPlugins().getTypes(); // copy: the pointer below points into it

            for (const auto& type : types)
                if (type.pluginFormatName == "VST3" && type.isInstrument)
                    vst3 = &type;

            if (vst3 != nullptr)
            {
                juce::String error;
                auto plugin = host.getFormats().createPluginInstance (*vst3, 48000.0, 512, error);
                auto* parameter = plugin != nullptr && ! plugin->getParameters().isEmpty() ? plugin->getParameters()[0] : nullptr;

                if (parameter != nullptr)
                {
                    const auto target = parameter->getValue() > 0.5f ? 0.13f : 0.87f;
                    parameter->setValueNotifyingHost (target);
                    juce::MemoryBlock juceState;
                    plugin->getStateInformation (juceState);
                    report << "parameter=" << parameter->getName (64) << " target=" << target << "\n";

                    // JUCE state -> FL container: u32 1 + records 1 (flags), 3 (component), 2 (controller)
                    std::vector<std::uint8_t> flState;
                    auto put32 = [&] (std::uint32_t v) { for (int b = 0; b < 4; ++b) flState.push_back ((std::uint8_t) (v >> (8 * b))); };
                    auto record = [&] (std::uint32_t id, const juce::MemoryBlock& data) {
                        put32 (id);
                        put32 ((std::uint32_t) data.getSize());
                        put32 (0);
                        flState.insert (flState.end(), (const std::uint8_t*) data.getData(), (const std::uint8_t*) data.getData() + data.getSize());
                    };
                    put32 (1);
                    record (1, juce::MemoryBlock (64, true));

                    // record 4 (REA_rodada22 §2.3): slots 0/1 hold the ParamIDs of parameters 2 and 0, so FL's
                    // automations k = 0, 1, 2 mean positions 2, 0 and 1 (the first parameter without a slot)
                    juce::MemoryBlock slots;
                    const auto paramId = [&] (int i) {
                        const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*> (plugin->getParameters()[i]);
                        return hosted != nullptr ? (std::uint32_t) hosted->getParameterID().getLargeIntValue() : 0u;
                    };
                    const bool reorder = plugin->getParameters().size() >= 3;
                    if (reorder)
                    {
                        for (auto v : { 2u, paramId (2), paramId (0) })
                            slots.append (&v, 4);
                        report << "slot_param_ids=" << (juce::int64) paramId (2) << "," << (juce::int64) paramId (0) << "\n";
                    }

                    if (auto xml = juce::AudioProcessor::getXmlFromBinary (juceState.getData(), (int) juceState.getSize()))
                        for (auto [tag, id] : { std::pair { "IComponent", 3u }, { "IEditController", 2u } })
                            if (auto* child = xml->getChildByName (tag))
                            {
                                juce::MemoryBlock data;
                                data.fromBase64Encoding (child->getAllSubText());
                                record (id, data);
                                report << tag << "_bytes=" << (int) data.getSize() << "\n";
                            }
                    if (reorder)
                        record (4, slots);

                    plugin.reset();

                    auto song = lynx::Song::makeDefault();
                    auto& f = song.channels[4].plugin.foreign;
                    f.format = lynx::ForeignPlugin::Format::vst3;
                    f.name = vst3->name.toStdString(); // no class ID: find it by name (fallback)
                    f.state = flState;
                    {
                        std::vector<std::uint8_t> comp, ctrl;
                        report << "split=" << (int) lynx::splitFlVst3State (flState, comp, ctrl) << " component=" << (int) comp.size()
                               << " saved_slots=" << (int) lynx::flVst3ParamSlots (flState).size() << "\n";
                    }
                    for (int k = 0; k < 3; ++k)
                    {
                        lynx::Automation a;
                        a.target = { lynx::AutomationTargetKind::channelParam, 4, 0, k };
                        song.automations.push_back (a);
                    }
                    const auto adopted = host.adoptFlPlugins (song);
                    report << "adopted=" << adopted << "\n";
                    juce::String mapped;
                    for (const auto& a : song.automations)
                        if (a.target.kind == lynx::AutomationTargetKind::channelParam && a.target.index == 4)
                            mapped << (mapped.isEmpty() ? "" : ",") << a.target.param;
                    report << "slot_positions=" << mapped << "\n";
                    const auto slotsOk = ! reorder || mapped == "2,0,1";

                    offline.setSong (song);

                    if (auto* loaded = dynamic_cast<lynx::PluginInstrument*> (offline.getChannelInstrument (4).get()))
                    {
                        const auto value = loaded->getInstance().getParameters()[0]->getValue();
                        report << "restored_value=" << value << "\n";
                        ok = adopted == 1 && std::abs (value - target) < 0.02f && slotsOk;
                    }
                }
            }
        }

        report << "result=" << (ok ? "OK" : "FAILED") << "\n";
        juce::File (reportPath).replaceWithText (report);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    void runShowcase (const juce::String& outputPath, int currentPattern)
    {
        using namespace lynx;
        Song song;
        song.bpm = 124.0;
        song.mixer = Song::defaultMixer();
        const auto presets = synthPresets();
        auto preset = [&] (const char* name) {
            for (const auto& [n, sset] : presets)
                if (n == name)
                    return sset;
            return SynthSettings {};
        };
        auto channel = [&] (const char* name, InstrumentKind kind, float volume, int track, std::uint32_t colour) {
            ChannelDef c;
            c.name = name;
            c.instrument = kind;
            c.volume = volume;
            c.mixerTrack = track;
            c.colour = colour;
            song.channels.push_back (c);
            return (int) song.channels.size() - 1;
        };
        const auto kick = channel ("Kick", InstrumentKind::kick, 0.9f, 1, 0xffd9534f);
        const auto clap = channel ("Clap", InstrumentKind::clap, 0.75f, 2, 0xfff0ad4e);
        const auto hat = channel ("Hat", InstrumentKind::hat, 0.6f, 3, 0xfff7d774);
        const auto snare = channel ("Snare", InstrumentKind::snare, 0.7f, 4, 0xffe8835a);
        const auto bass = channel ("Reese Bass", InstrumentKind::synth, 0.8f, 5, 0xff5bc0de);
        song.channels[(std::size_t) bass].synth = preset ("Reese Bass"); // preset name as defined in core/SynthPresets
        const auto pad = channel ("Pad", InstrumentKind::synth, 0.7f, 6, 0xff9b7fd6);
        song.channels[(std::size_t) pad].synth = preset ("Pad");
        const auto pluck = channel ("Pluck", InstrumentKind::synth, 0.72f, 7, 0xff5cb85c);
        song.channels[(std::size_t) pluck].synth = preset ("Pluck");
        const auto lead = channel ("Supersaw", InstrumentKind::synth, 0.6f, 8, 0xff4fc3a1);
        song.channels[(std::size_t) lead].synth = preset ("Supersaw");

        auto addPattern = [&] (const char* name, std::uint32_t colour) {
            auto& p = song.patterns.emplace_back();
            p.name = name;
            p.colour = colour;
            return (int) song.patterns.size() - 1;
        };
        auto note = [&] (int pattern, int ch, double beat, double beats, int key, float velocity = 0.8f) {
            Note n;
            n.channel = ch;
            n.start = (std::int64_t) (beat * ppq);
            n.length = (std::int64_t) (beats * ppq);
            n.key = key;
            n.velocity = velocity;
            song.patterns[(std::size_t) pattern].notes.push_back (n);
        };

        const auto beat = addPattern ("Beat", 0xffd9534f);
        for (int s = 0; s < 16; s += 4)
            song.patterns[(std::size_t) beat].toggleStep (kick, s);
        for (int s : { 4, 12 })
            song.patterns[(std::size_t) beat].toggleStep (clap, s);
        for (int s = 2; s < 16; s += 4)
            song.patterns[(std::size_t) beat].toggleStep (hat, s);
        for (int s : { 7, 15 })
            song.patterns[(std::size_t) beat].toggleStep (snare, s);

        const auto groove = addPattern ("Groove", 0xfff0ad4e);
        for (int s = 0; s < 16; ++s)
            song.patterns[(std::size_t) groove].toggleStep (hat, s);
        for (int s : { 4, 12 })
            song.patterns[(std::size_t) groove].toggleStep (clap, s);

        // Am – F – C – G, one bar each
        const int roots[4] = { 45, 41, 48, 43 };
        const int chords[4][3] = { { 57, 60, 64 }, { 53, 57, 60 }, { 55, 60, 64 }, { 55, 59, 62 } };
        const auto bassline = addPattern ("Bass", 0xff5bc0de);
        for (int bar = 0; bar < 4; ++bar)
            for (int e = 0; e < 8; ++e)
                if (e != 3)
                    note (bassline, bass, bar * 4 + e * 0.5, 0.4, roots[bar] - 12 + (e == 6 ? 12 : 0), e % 2 == 0 ? 0.9f : 0.65f);
        const auto chordsPattern = addPattern ("Chords", 0xff9b7fd6);
        for (int bar = 0; bar < 4; ++bar)
            for (int k = 0; k < 3; ++k)
                note (chordsPattern, pad, bar * 4, 3.9, chords[bar][k], 0.7f);
        const auto melody = addPattern ("Melody", 0xff5cb85c);
        const int line[] = { 69, 72, 76, 74, 72, 69, 67, 69, 65, 69, 72, 74, 72, 69, 67, 64,
                             72, 76, 79, 76, 74, 72, 74, 76, 71, 74, 79, 78, 76, 74, 71, 67 };
        for (int k = 0; k < 32; ++k)
            if (k % 8 != 7)
                note (melody, pluck, k * 0.5, k % 4 == 3 ? 0.9 : 0.4, line[k], k % 2 == 0 ? 0.85f : 0.65f);
        const auto hook = addPattern ("Hook", 0xff4fc3a1);
        for (int bar = 0; bar < 4; ++bar)
            for (int k = 0; k < 3; ++k)
                note (hook, lead, bar * 4 + k * 1.5, k == 2 ? 1.0 : 1.4, chords[bar][2] + 12 - k * 3, 0.8f);

        // Playlist: intro, verse, drop, breakdown, final drop
        song.playlistTracks = { { "Drums" }, { "Groove" }, { "Bass" }, { "Chords" }, { "Melody" }, { "Hook" }, { "Automation" } };
        const auto bar = (std::int64_t) ppq * 4;
        auto clip = [&] (int pattern, int track, int fromBar, int bars) {
            const auto length = song.patterns[(std::size_t) pattern].effectiveLength (song.beatsPerBar);
            for (int b = fromBar; b < fromBar + bars;)
            {
                Clip c;
                c.pattern = pattern;
                c.track = track;
                c.start = b * bar;
                c.length = length;
                song.playlist.push_back (c);
                b += (int) std::max<std::int64_t> (1, length / bar);
            }
        };
        clip (chordsPattern, 3, 0, 8);
        clip (melody, 4, 4, 4);
        clip (beat, 0, 8, 16);
        clip (groove, 1, 12, 12);
        clip (bassline, 2, 8, 8);
        clip (melody, 4, 8, 8);
        clip (chordsPattern, 3, 16, 4);
        clip (bassline, 2, 20, 4);
        clip (hook, 5, 20, 4);
        clip (melody, 4, 20, 4);
        song.addMarker ({ 0, "Intro" });
        song.addMarker ({ 8 * bar, "Drop" });
        song.addMarker ({ 16 * bar, "Breakdown" });
        song.addMarker ({ 20 * bar, "Final" });

        // automation: the pad volume opens up during the intro
        Automation a;
        a.name = "Pad Volume";
        a.target = { AutomationTargetKind::mixerVolume, 6, 0, 0 };
        a.points = { { 0, 0.15f, 0.4f }, { 8 * bar, 0.8f, 0.0f } };
        song.automations.push_back (a);
        Clip ac;
        ac.kind = ClipKind::automation;
        ac.automation = 0;
        ac.track = 6;
        ac.start = 0;
        ac.length = 8 * bar;
        song.playlist.push_back (ac);

        // mixer: names, colours and effects (bass compressor sidechained to the kick)
        const char* names[] = { "Master", "Kick", "Clap", "Hat", "Snare", "Bass", "Pad", "Pluck", "Lead" };
        for (int t = 0; t < 9; ++t)
        {
            song.mixer[(std::size_t) t].name = names[t];
            if (t > 0)
                song.mixer[(std::size_t) t].colour = song.channels[(std::size_t) t - 1].colour;
        }
        auto fx = [&] (int track, EffectKind kind) -> EffectSlot& {
            song.mixer[(std::size_t) track].effects.push_back (makeEffectSlot (kind));
            return song.mixer[(std::size_t) track].effects.back();
        };
        {
            // kick EQ: cut below 30 Hz, weight at 60 Hz, remove the "boxiness" at 350 Hz, brightness at 4 kHz
            auto& eq = fx (1, EffectKind::parametricEq);
            auto band = [&] (int b, peq::BandType type, double hz, double db, double q) {
                eq.params[(std::size_t) peq::param (b, 0)] = peq::typeValue (type);
                eq.params[(std::size_t) peq::param (b, 1)] = peq::hzValue (hz);
                eq.params[(std::size_t) peq::param (b, 2)] = peq::gainValue (db);
                eq.params[(std::size_t) peq::param (b, 3)] = peq::qValue (q);
            };
            band (0, peq::BandType::lowCut, 30.0, 0.0, 0.7);
            band (1, peq::BandType::peak, 62.0, 5.0, 1.2);
            band (2, peq::BandType::peak, 350.0, -6.0, 2.0);
            band (3, peq::BandType::peak, 4200.0, 3.5, 1.5);
            band (4, peq::BandType::highCut, 14000.0, 0.0, 0.7);
            for (int b = 5; b < peq::bands; ++b)
                eq.params[(std::size_t) peq::param (b, 0)] = peq::typeValue (peq::BandType::off);
        }
        fx (1, EffectKind::distortion).mix = 0.3f;
        auto& comp = fx (5, EffectKind::compressor);
        comp.params[0] = 0.55f;
        comp.params[1] = 0.8f;
        comp.sidechain = 1;
        fx (5, EffectKind::parametricEq);
        fx (6, EffectKind::chorus);
        fx (6, EffectKind::reverb);
        fx (7, EffectKind::delay);
        fx (7, EffectKind::reverb).mix = 0.4f;
        fx (8, EffectKind::effector);
        fx (8, EffectKind::delay);
        fx (0, EffectKind::parametricEq);
        auto& limiter = fx (0, EffectKind::limiter);
        limiter.params = { 11.7f / 12.0f, 0.1f, 0.5f };
        song.mixer[1].volume = 0.78f;
        song.mixer[5].volume = 0.72f;
        song.mixer[6].volume = 0.6f;
        song.mixer[6].pan = -0.2f;
        song.mixer[7].pan = 0.25f;

        song.mode = PlayMode::song;
        song.currentPattern = std::clamp (currentPattern, 0, (int) song.patterns.size() - 1);
        juce::String error;
        const auto ok = ProjectFile::save (song, juce::File (outputPath), error);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // LYNX.exe --sandboxtest plugin.vst3 report.txt: opens the instrument in a separate process, plays a
    // note, changes a parameter, checks the state (back in an in-process plugin) and makes the child hang
    // to check that LYNX keeps going (silence, without waiting longer than the timeout).
    // --sandboxtest: plays an instrument (a note) or an effect (a 440 Hz tone through it); render() adds into l / r
    struct TestVoice
    {
        explicit TestVoice (std::unique_ptr<juce::AudioPluginInstance> p)
        {
            if (p->acceptsMidi() && p->getTotalNumInputChannels() == 0)
                instrument = std::make_unique<lynx::PluginInstrument> (std::move (p), 512);
            else
                effect = std::move (p);
        }
        void noteOn()
        {
            if (instrument != nullptr)
                instrument->noteOn (60, 0.9f);
        }
        void render (float* l, float* r, int n, float gainLeft, float gainRight)
        {
            if (instrument != nullptr)
            {
                instrument->render (l, r, n, gainLeft, gainRight);
                return;
            }
            const auto channels = juce::jmax (2, effect->getTotalNumInputChannels(), effect->getTotalNumOutputChannels());
            juce::AudioBuffer<float> block (channels, 512);
            juce::MidiBuffer midi;
            for (int done = 0; done < n; done += 512)
            {
                const auto k = juce::jmin (512, n - done);
                block.setSize (channels, k, false, false, true);
                for (int i = 0; i < k; ++i)
                {
                    const auto v = 0.25f * (float) std::sin (phase);
                    phase += 2.0 * juce::MathConstants<double>::pi * 440.0 / 48000.0;
                    for (int c = 0; c < channels; ++c)
                        block.setSample (c, i, v);
                }
                effect->processBlock (block, midi);
                for (int i = 0; i < k; ++i)
                {
                    l[done + i] += gainLeft * block.getSample (0, i);
                    r[done + i] += gainRight * block.getSample (1, i);
                }
            }
        }
        std::unique_ptr<lynx::PluginInstrument> instrument;
        std::unique_ptr<juce::AudioPluginInstance> effect;
        double phase = 0.0;
    };

    void runSandboxTest (const juce::String& pluginPath, const juce::String& reportPath)
    {
        juce::String report;
        bool ok = false;
        lynx::setPluginSandboxAlerts (false);
        const juce::File log (reportPath + ".log");
        auto step = [&] (const char* what) { log.appendText (juce::String (what) + "\n"); };
        log.deleteFile();
        {
            lynx::PluginHost host (false);
            host.addPluginFile (juce::File (pluginPath));
            const auto types = host.getKnownPlugins().getTypes(); // getTypes() returns a copy
            // the file's instrument, else its first effect (round 161: effects too, e.g. a Windows effect through yabridge)
            const juce::PluginDescription* instrument = nullptr;
            for (const auto& type : types)
                if (type.isInstrument)
                    instrument = &type;
            for (const auto& type : types)
                if (instrument == nullptr && ! type.isInstrument)
                    instrument = &type;

            step ("host");
            juce::String error;
            auto remote = instrument != nullptr ? lynx::createSandboxedPlugin (*instrument, 48000.0, 512, {}, error) : nullptr;
            report << "opened=" << (remote != nullptr ? 1 : 0) << " error=" << error << "\n";

            step ("created");
            if (remote != nullptr)
            {
                auto& proxy = *remote;
                TestVoice voice (std::move (remote));
                std::vector<float> l (48000), r (48000);
                auto peakOf = [&] {
                    float peak = 0.0f;
                    for (std::size_t k = 0; k < l.size(); ++k)
                        peak = std::max ({ peak, std::abs (l[k]), std::abs (r[k]) });
                    return peak;
                };

                const auto start = juce::Time::getMillisecondCounterHiRes();
                voice.noteOn();
                voice.render (l.data(), r.data(), 48000, 1.0f, 1.0f);
                const auto renderMs = juce::Time::getMillisecondCounterHiRes() - start;
                const auto peak = peakOf();
                step ("played");
                if (juce::JUCEApplication::getCommandLineParameters().contains ("--expect-crash"))
                {
                    // a plugin that crashes while playing (round 161 test plugin): LYNX notices, mutes it and keeps running
                    const auto dead = lynx::isSandboxedPluginDead (proxy);
                    std::fill (l.begin(), l.end(), 0.0f);
                    std::fill (r.begin(), r.end(), 0.0f);
                    voice.render (l.data(), r.data(), 4096, 1.0f, 1.0f);
                    report << "peak=" << peak << " render_1s_ms=" << renderMs << " dead=" << (dead ? 1 : 0) << " peak_after=" << peakOf() << "\n";
                    ok = dead && peak > 1e-4f && renderMs < 2000.0 && peakOf() == 0.0f;
                    report << "result=" << (ok ? "OK" : "FAILED") << "\n";
                    juce::File (reportPath).replaceWithText (report);
                    setApplicationReturnValue (ok ? 0 : 1);
                    quit();
                    return;
                }
                report << "plugin=" << proxy.getName() << (instrument->isInstrument ? " (instrument)" : " (effect)") << " parameters="
                       << proxy.getParameters().size() << " peak=" << peak << " render_1s_ms=" << renderMs << "\n";

                if (juce::JUCEApplication::getCommandLineParameters().contains ("--editor") && proxy.hasEditor())
                {
                    // the plugin's own window, opened by the child process (4 s on screen); the child must stay responsive
                    std::unique_ptr<juce::AudioProcessorEditor> editor (proxy.createEditorIfNeeded());
                    juce::Thread::sleep (4000);
                    juce::MemoryBlock state;
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    proxy.getStateInformation (state);
                    report << "editor_shown=1 state_with_editor_ms=" << (juce::Time::getMillisecondCounterHiRes() - t0) << " dead=" << (lynx::isSandboxedPluginDead (proxy) ? 1 : 0) << "\n";
                    editor.reset();
                }

                // parameter -> state -> in-process plugin
                bool stateOk = false;
                if (! proxy.getParameters().isEmpty())
                {
                    auto* p = proxy.getParameters()[0];
                    const auto target = p->getValue() > 0.5f ? 0.13f : 0.87f;
                    p->setValue (target);
                    std::fill (l.begin(), l.end(), 0.0f);
                    voice.render (l.data(), r.data(), 512, 1.0f, 1.0f); // carries the change to the child
                    juce::MemoryBlock state;
                    proxy.getStateInformation (state);
                    if (auto local = host.getFormats().createPluginInstance (*instrument, 48000.0, 512, error))
                    {
                        local->setStateInformation (state.getData(), (int) state.getSize());
                        const auto back = local->getParameters()[0]->getValue();
                        // the same steps without the sandbox: some plugins quantise the value or keep it out of their state
                        auto control = target;
                        if (auto a = host.getFormats().createPluginInstance (*instrument, 48000.0, 512, error))
                        {
                            a->enableAllBuses();
                            a->prepareToPlay (48000.0, 512);
                            a->getParameters()[0]->setValue (target);
                            juce::AudioBuffer<float> block (juce::jmax (2, a->getTotalNumInputChannels(), a->getTotalNumOutputChannels()), 512);
                            block.clear();
                            juce::MidiBuffer none;
                            a->processBlock (block, none);
                            juce::MemoryBlock direct;
                            a->getStateInformation (direct);
                            if (auto b = host.getFormats().createPluginInstance (*instrument, 48000.0, 512, error))
                            {
                                b->setStateInformation (direct.getData(), (int) direct.getSize());
                                control = b->getParameters()[0]->getValue();
                            }
                            a->releaseResources();
                        }
                        report << "state_bytes=" << (int) state.getSize() << " target=" << target << " in_process=" << back << " in_process_control=" << control << "\n";
                        stateOk = std::abs (back - control) < 0.02f;
                    }
                }

                step ("state");
                // hung child: the block returns in ~0.5 s, muted, and the plugin is marked dead
                lynx::hangSandboxedPluginForTest (proxy);
                juce::Thread::sleep (200);
                std::fill (l.begin(), l.end(), 1.0f);
                std::fill (r.begin(), r.end(), 1.0f);
                const auto hangStart = juce::Time::getMillisecondCounterHiRes();
                voice.render (l.data(), r.data(), 4096, 0.0f, 0.0f);
                std::fill (l.begin(), l.end(), 0.0f);
                std::fill (r.begin(), r.end(), 0.0f);
                voice.render (l.data(), r.data(), 4096, 1.0f, 1.0f);
                const auto hangMs = juce::Time::getMillisecondCounterHiRes() - hangStart;
                const auto dead = lynx::isSandboxedPluginDead (proxy);
                const auto hangPeak = peakOf();
                report << "hung_ms=" << hangMs << " dead=" << (dead ? 1 : 0) << " peak_after=" << hangPeak << "\n";

                // crashed child (round 161): a second instance aborts; LYNX notices, mutes it and keeps running
                bool crashOk = false;
                if (auto second = lynx::createSandboxedPlugin (*instrument, 48000.0, 512, {}, error))
                {
                    auto& proxy2 = *second;
                    TestVoice voice2 (std::move (second));
                    voice2.noteOn();
                    voice2.render (l.data(), r.data(), 512, 1.0f, 1.0f);
                    lynx::crashSandboxedPluginForTest (proxy2);
                    const auto crashStart = juce::Time::getMillisecondCounterHiRes();
                    while (! lynx::isSandboxedPluginDead (proxy2) && juce::Time::getMillisecondCounterHiRes() - crashStart < 5000.0)
                    {
                        voice2.render (l.data(), r.data(), 512, 1.0f, 1.0f);
                        juce::Thread::sleep (5);
                    }
                    const auto crashMs = juce::Time::getMillisecondCounterHiRes() - crashStart;
                    std::fill (l.begin(), l.end(), 0.0f);
                    std::fill (r.begin(), r.end(), 0.0f);
                    voice2.render (l.data(), r.data(), 4096, 1.0f, 1.0f);
                    const auto crashDead = lynx::isSandboxedPluginDead (proxy2);
                    report << "crash_ms=" << crashMs << " crash_dead=" << (crashDead ? 1 : 0) << " crash_peak_after=" << peakOf() << "\n";
                    crashOk = crashDead && crashMs < 3000.0;
                }
                step ("crash");

                ok = peak > 1e-4f && stateOk && dead && hangMs < 1500.0 && hangPeak == 0.0f && crashOk;
            }
        }

        report << "result=" << (ok ? "OK" : "FAILED") << "\n";
        juce::File (reportPath).replaceWithText (report);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // --uiperf (round 161): opens the project in the normal window, plays it, and drives the heavy UI cases of round 151
    // through the window's own event path — the default layout, the Mixer maximised (meters while playing, then
    // scrolled), the Playlist maximised (scroll down, scroll sideways, zoom in and out) and the Channel rack scrolled.
    // Every frame is painted right after its event (ComponentPeer::performAnyPendingRepaintsNow) and timed; the report
    // has, per case, the time to show it and the frames' mean / p95 / max, how many took longer than 16 ms, and the
    // process CPU. The same on every OS, so Linux (X11, XWayland) and Windows numbers can be put side by side.
    void runUiPerf (const juce::File& project, const juce::File& report)
    {
        startupOptions = lynx::StartupOptions::load();
        engine = std::make_unique<lynx::AudioEngine>();
        crashEngine = engine.get();
        createMainWindow();
        mainWindow->reveal();

        juce::String error;
        auto ok = false;
        const auto loadStart = juce::Time::getMillisecondCounterHiRes();
        if (project.hasFileExtension ("flp"))
            ok = engine->importFlpFile (project, error);
        else if (auto song = lynx::ProjectFile::load (project, error))
        {
            engine->setSong (std::move (*song));
            ok = true;
        }
        const auto loadMs = juce::Time::getMillisecondCounterHiRes() - loadStart;
        if (! ok)
        {
            report.replaceWithText ("project: " + project.getFullPathName() + "\nresult=ERROR " + error + "\n");
            setApplicationReturnValue (1);
            quit();
            return;
        }

        struct Step
        {
            juce::String name;
            std::function<void()> setup;
            std::function<void (int)> action; // per frame; none: the step watches the window for `frames` ms
            int frames;
        };

        struct UiPerf final : juce::Timer
        {
            LynxApplication& app;
            juce::File report;
            juce::String text;
            std::vector<Step> steps;
            std::size_t step = 0;
            int frame = -1;
            std::vector<double> times, rendered; // whole frames (event + paint + blit, when this timer paints) / the window's paints
            double setupMs = 0.0, wallStart = 0.0;
            std::clock_t cpuStart = 0;

            UiPerf (LynxApplication& a, juce::File r) : app (a), report (std::move (r)) {}

            juce::ComponentPeer* peer() const { return app.mainWindow != nullptr ? app.mainWindow->getPeer() : nullptr; }

            double paintNow() const
            {
                const auto t0 = juce::Time::getMillisecondCounterHiRes();
                if (auto* p = peer())
                    p->performAnyPendingRepaintsNow();
                return juce::Time::getMillisecondCounterHiRes() - t0;
            }

            void timerCallback() override
            {
                if (step >= steps.size())
                {
                    stopTimer();
                    finish();
                    return;
                }
                auto& s = steps[step];
                if (frame < 0)
                {
                    const auto t0 = juce::Time::getMillisecondCounterHiRes();
                    if (s.setup)
                        s.setup();
                    paintNow();
                    setupMs = juce::Time::getMillisecondCounterHiRes() - t0;
                    times.clear();
                    rendered.clear();
                    wallStart = juce::Time::getMillisecondCounterHiRes();
                    cpuStart = std::clock();
                    frame = 0;
                    return;
                }
                if (s.action)
                    s.action (frame);
                if (const auto ms = paintNow(); ms > 0.25) // (nothing to paint: not a frame)
                    times.push_back (ms);
                ++frame;
                if (s.action ? frame < s.frames : juce::Time::getMillisecondCounterHiRes() - wallStart < (double) s.frames)
                    return;

                const auto wall = juce::Time::getMillisecondCounterHiRes() - wallStart;
                const auto cpu = 1000.0 * (double) (std::clock() - cpuStart) / CLOCKS_PER_SEC;
                text << s.name << ": show " << juce::String (setupMs, 1) << " ms\n"
                     << "    painted frames: " << stats (rendered) << "\n"
                     << "    timed here (event + paint + blit): " << stats (times)
                     << "\n    CPU " << juce::String (wall > 0.0 ? 100.0 * cpu / wall : 0.0, 0) << "% (process, " << juce::String (wall / 1000.0, 1) << " s)\n";
                juce::File (report).replaceWithText (text);
                ++step;
                frame = -1;
            }

            static juce::String stats (std::vector<double> sorted)
            {
                std::sort (sorted.begin(), sorted.end());
                const auto at = [&sorted] (double q) { return sorted.empty() ? 0.0 : sorted[(std::size_t) juce::jlimit (0, (int) sorted.size() - 1, (int) (q * (double) sorted.size()))]; };
                double sum = 0.0;
                int over = 0;
                for (auto t : sorted)
                {
                    sum += t;
                    over += t > 16.0 ? 1 : 0;
                }
                return juce::String ((int) sorted.size()) + ", mean " + juce::String (sorted.empty() ? 0.0 : sum / (double) sorted.size(), 1)
                       + " ms, p95 " + juce::String (at (0.95), 1) + " ms, max " + juce::String (sorted.empty() ? 0.0 : sorted.back(), 1)
                       + " ms, >16 ms " + juce::String (over);
            }

            void finish()
            {
                // the cost of asking where the mouse is (X11: a round trip to the display server)
                const auto q0 = juce::Time::getMillisecondCounterHiRes();
                for (int i = 0; i < 100; ++i)
                    juce::ignoreUnused (juce::Desktop::getInstance().getMainMouseSource().getScreenPosition());
                text << "mouse position query: " << juce::String ((juce::Time::getMillisecondCounterHiRes() - q0) / 100.0, 3) << " ms\n";
                app.mainWindow->onFrame = nullptr;
                app.engine->stop();
                text << "result=OK\n";
                report.replaceWithText (text);
                app.setApplicationReturnValue (0);
                app.quit();
            }
        };

        auto* ui = &mainWindow->getContent();
        auto wheelAt = [this] (juce::Component& target, float dx, float dy, juce::ModifierKeys mods) {
            auto* peer = mainWindow->getPeer();
            if (peer == nullptr || ! target.isShowing())
                return;
            juce::ModifierKeys::currentModifiers = mods;
            const auto local = peer->globalToLocal (target.localPointToGlobal (target.getLocalBounds().getCentre()).toFloat());
            juce::MouseWheelDetails wheel { dx, dy, false, false, false };
            peer->handleMouseWheel (juce::MouseInputSource::InputSourceType::mouse, local, juce::Time::currentTimeMillis(), wheel);
            juce::ModifierKeys::currentModifiers = {};
        };

        auto run = std::make_unique<UiPerf> (*this, report);
        mainWindow->onFrame = [r = run.get()] (double ms) { if (r->frame >= 0) r->rendered.push_back (ms); };
        run->text << "project: " << project.getFullPathName() << "\n"
                   << "LYNX " << getApplicationVersion() << ", " << juce::SystemStats::getOperatingSystemName() << ", "
                   << juce::SystemStats::getCpuModel() << ", window " << mainWindow->getWidth() << "x" << mainWindow->getHeight()
                   << ", scale " << juce::String (juce::Desktop::getInstance().getDisplays().getPrimaryDisplay() != nullptr
                                                     ? juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->scale : 1.0, 2)
                   << ", audio device " << (engine->getDeviceManager().getCurrentAudioDevice() != nullptr ? engine->getDeviceManager().getCurrentAudioDevice()->getName() : juce::String ("none")) << "\n"
                   << "load " << juce::String (loadMs / 1000.0, 2) << " s\n";
        const auto none = juce::ModifierKeys();
        run->steps.push_back ({ "desktop, playing", [this, ui] { ui->showDesktopForSnapshot (false); engine->rewind(); engine->play(); }, {}, 3000 });
        run->steps.push_back ({ "mixer maximised, playing", [ui] { ui->selectTabForSnapshot (3); }, {}, 3000 });
        run->steps.push_back ({ "mixer scroll", {}, [wheelAt, ui, none] (int f) { wheelAt (ui->getMixerForSnapshot(), 0.0f, f < 30 ? -0.25f : 0.25f, none); }, 60 });
        run->steps.push_back ({ "playlist maximised, playing", [ui] { ui->selectTabForSnapshot (2); }, {}, 3000 });
        run->steps.push_back ({ "playlist scroll down/up", [this] { engine->stop(); },
                                 [wheelAt, ui, none] (int f) { wheelAt (ui->getPlaylistForSnapshot(), 0.0f, f < 40 ? -0.15f : 0.15f, none); }, 80 });
        run->steps.push_back ({ "playlist scroll sideways", {},
                                 [wheelAt, ui, none] (int f) { wheelAt (ui->getPlaylistForSnapshot(), f < 40 ? -0.15f : 0.15f, 0.0f, none); }, 80 });
        run->steps.push_back ({ "whole window repainted", {}, [this] (int) { mainWindow->repaint(); }, 40 });
        run->steps.push_back ({ "channel rack scroll", [ui] { ui->selectTabForSnapshot (0); },
                                 [wheelAt, ui, none] (int f) { wheelAt (ui->getRackViewportForSnapshot(), 0.0f, f < 30 ? -0.25f : 0.25f, none); }, 60 });
        // let the window draw the project first
        perf = std::move (run);
        juce::Timer::callAfterDelay (1500, [this] { if (perf != nullptr) perf->startTimer (16); });
    }

    // --rtstress: opens the project in the normal window with the saved audio device, plays it from the start for
    // `seconds` and writes the real-time statistics (deadline misses, late device calls, load histogram, heaviest units)
    void runRealtimeStress (const juce::File& project, const juce::File& report, double seconds, int bufferSize,
                            const juce::String& deviceType, const juce::String& deviceName, const juce::String& recordTo = {})
    {
        startupOptions = lynx::StartupOptions::load();
        engine = std::make_unique<lynx::AudioEngine>();
        crashEngine = engine.get();
        // the user's saved device stays as it was: this run's choices are undone before quitting
        savedDeviceState = engine->getDeviceManager().createStateXml();
        auto& devices = engine->getDeviceManager();
        const auto driverFile = addDriverFileDeviceType();
        const auto type = deviceType.isEmpty() && driverFile ? juce::String (lynx::dllAsioTypeName) : deviceType;
        if (type.isNotEmpty() && devices.getCurrentAudioDeviceType() != type)
            devices.setCurrentAudioDeviceType (type, true);
        if (bufferSize > 0 || deviceName.isNotEmpty())
        {
            auto setup = devices.getAudioDeviceSetup();
            if (bufferSize > 0)
                setup.bufferSize = bufferSize;
            if (deviceName.isNotEmpty())
                setup.outputDeviceName = deviceName;
            const auto error = devices.setAudioDeviceSetup (setup, true);
            if (error.isNotEmpty())
                report.replaceWithText ("device error: " + error + "\n");
        }
        createMainWindow();
        if (! juce::JUCEApplication::getCommandLineParameters().contains ("--no-window")) // --no-window: is it the UI?
            mainWindow->reveal();

        juce::String error;
        auto ok = false;
        const auto loadStart = juce::Time::getMillisecondCounterHiRes();
        if (project.hasFileExtension ("flp"))
            ok = engine->importFlpFile (project, error);
        else if (auto song = lynx::ProjectFile::load (project, error))
        {
            engine->setSong (std::move (*song));
            ok = true;
        }
        const auto loadMs = juce::Time::getMillisecondCounterHiRes() - loadStart;

        if (! ok)
        {
            report.replaceWithText ("project: " + project.getFullPathName() + "\nresult=ERROR " + error + "\n");
            restoreSavedDeviceState();
            setApplicationReturnValue (1);
            quit();
            return;
        }

        if (recordTo.isNotEmpty())
            engine->setAudioRecordingArmed (true); // opens the input of the driver

        // let the window draw the project, then play from the start with fresh statistics
        juce::Timer::callAfterDelay (1500, [this, project, report, seconds, loadMs, recordTo] {
            if (engine == nullptr)
                return;
            engine->stop();
            engine->rewind();
            engine->getRtStats().reset();
            engine->takeUiPulse();
            if (recordTo.isNotEmpty())
                engine->setRecording (true);
            // --log folder: the audio logger records what the Master plays during the run (realtime vs export checks)
            const auto& cli = juce::JUCEApplication::getCommandLineParameterArray();
            if (const auto at = cli.indexOf ("--log"); at >= 0 && at + 1 < cli.size())
                engine->getAudioLogger().start (juce::File (cli[at + 1].unquoted()), engine->getDeviceManager().getCurrentAudioDevice() != nullptr
                                                                                     ? engine->getDeviceManager().getCurrentAudioDevice()->getCurrentSampleRate() : 44100.0);
            engine->play();
            juce::Timer::callAfterDelay ((int) (seconds * 1000.0), [this, project, report, seconds, loadMs, recordTo] {
                if (engine == nullptr)
                    return;
                engine->stop();
                juce::String recording;
                if (engine->getAudioLogger().isLogging())
                {
                    recording << "  logged: " << engine->getAudioLogger().currentFile().getFullPathName() << "\n";
                    engine->getAudioLogger().stop();
                }
                if (recordTo.isNotEmpty())
                    recording = finishStressTake (juce::File (recordTo), seconds);
                const auto& cli = juce::JUCEApplication::getCommandLineParameterArray();
                const auto s = engine->getRtStats().snapshot();
                juce::String text;
                text << "project: " << project.getFullPathName() << "\n"
                     << "LYNX " << getApplicationVersion() << ", " << juce::SystemStats::getOperatingSystemName() << ", "
                     << juce::SystemStats::getNumCpus() << " logical CPUs (" << juce::SystemStats::getNumPhysicalCpus() << " cores), "
                     << juce::SystemStats::getCpuModel() << "\n"
                     << "load " << juce::String (loadMs / 1000.0, 1) << " s, played " << juce::String (seconds, 0) << " s\n"
                     << engine->describeRealtime();
                const auto pulse = engine->takeUiPulse();
                text << "  UI: the 30 Hz engine timer ran " << pulse.calls << " times (" << juce::String (pulse.calls / seconds, 1)
                     << "/s), longest gap " << juce::String (pulse.maxGapMs, 0) << " ms" << (mainWindow == nullptr || ! mainWindow->isVisible() ? " (window hidden)" : "") << "\n";
                text << "  load histogram (share of the deadline: callbacks):";
                for (std::size_t b = 0; b < s.histogram.size(); ++b)
                    if (s.histogram[b] > 0)
                        text << " " << (int) (b * 10) << (b + 1 == s.histogram.size() ? juce::String ("+") : "-" + juce::String ((int) (b * 10 + 10))) << "%:" << (juce::int64) s.histogram[b];
                text << "\n";
                text << recording;
                // --settingsshot file.png (round 161): Options > Audio settings of this run's device, as a picture
                if (const auto at = cli.indexOf ("--settingsshot"); at >= 0 && at + 1 < cli.size())
                {
                    auto page = lynx::makeSettingsPanel (*engine, lynx::SettingsPage::audio);
                    juce::Image image (juce::Image::ARGB, page->getWidth(), page->getHeight(), true);
                    {
                        juce::Graphics g (image);
                        g.fillAll (lynx::theme::panel);
                        page->paintEntireComponent (g, false);
                    }
                    const juce::File shot (cli[at + 1].unquoted());
                    shot.deleteFile();
                    if (auto out = shot.createOutputStream())
                        juce::PNGImageFormat().writeImageToStream (image, *out);
                }
                text << "result=" << (s.callbacks == 0 ? "ERROR no audio callbacks" : s.overBudget == 0 && s.lateCalls == 0 ? "OK" : "GLITCHES") << "\n";
                report.replaceWithText (text);
                restoreSavedDeviceState();
                setApplicationReturnValue (s.callbacks == 0 ? 1 : 0);
                quit();
            });
        });
    }

    // --rectest (round 163): see the command line above. Writes report lines step by step (a driver that ends the
    // process leaves what was done).
    void runRecordTest (const juce::File& report, double seconds, const juce::String& deviceType, const juce::String& deviceName,
                        const juce::String& inputName, int mode, int pdcSamples, int bufferSize)
    {
        engine = std::make_unique<lynx::AudioEngine>();
        savedDeviceState = engine->getDeviceManager().createStateXml();
        auto text = std::make_shared<juce::String>();
        auto step = [report, text] (const juce::String& line) { *text << line << "\n"; report.replaceWithText (*text); };
        auto finish = [this, step] (int code) {
            restoreSavedDeviceState();
            setApplicationReturnValue (code);
            quit();
        };
        auto& devices = engine->getDeviceManager();
        if (deviceType.isNotEmpty() && devices.getCurrentAudioDeviceType() != deviceType)
            devices.setCurrentAudioDeviceType (deviceType, true);
        {
            auto setup = devices.getAudioDeviceSetup();
            if (deviceName.isNotEmpty())
                setup.outputDeviceName = deviceName;
            if (bufferSize > 0)
                setup.bufferSize = bufferSize;
            if (const auto error = devices.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                step ("device_error=" + error);
        }
        if (const auto r = engine->openAudioInput (inputName.isNotEmpty() ? inputName : deviceName); r.issue != lynx::AudioEngine::InputIssue::none)
            step ("input_error=" + juce::String ((int) r.issue) + " " + r.detail);
        // takes go next to the report, not into the user's recordings folder
        engine->setRecordingTarget (report.getSiblingFile (report.getFileNameWithoutExtension() + "-takes"), {});
        auto* device = devices.getCurrentAudioDevice();
        if (device == nullptr || engine->getInputChannelNames().empty())
        {
            step ("result=ERROR no device / no input");
            finish (1);
            return;
        }
        const auto rate = device->getCurrentSampleRate();
        step ("device=" + device->getTypeName() + " | " + device->getName() + " | " + juce::String (rate) + " Hz | block " + juce::String (device->getCurrentBufferSizeSamples())
              + " | driver latency in " + juce::String (device->getInputLatencyInSamples()) + " out " + juce::String (device->getOutputLatencyInSamples()));

        // a click (the ping's pulse) on every beat at 120 bpm: beat k at sample k * rate / 2
        const auto beats = (int) std::ceil (seconds * 2.0) + 8;
        const auto clickFile = report.getSiblingFile (report.getFileNameWithoutExtension() + "-clicks.wav");
        {
            lynx::takes::WavTakeWriter w;
            w.open (clickFile.getFullPathName().toStdString(), rate, 1, lynx::takes::SampleFormat::float32);
            const auto total = (juce::int64) std::llround ((double) beats * rate / 2.0);
            std::vector<float> block (4096);
            for (juce::int64 at = 0; at < total; at += (juce::int64) block.size())
            {
                std::fill (block.begin(), block.end(), 0.0f);
                for (int k = 0; k < (int) block.size(); ++k)
                {
                    const auto f = at + k;
                    const auto beat = (juce::int64) std::floor ((double) f / (rate / 2.0));
                    const auto rel = f - (juce::int64) std::llround ((double) beat * rate / 2.0);
                    static constexpr float shape[] { 0.25f, 0.75f, 1.0f, 0.75f, 0.25f };
                    if (rel >= 0 && rel < 5)
                        block[(std::size_t) k] = 0.5f * shape[rel];
                }
                const float* d[] { block.data() };
                w.write (d, (int) std::min<juce::int64> ((juce::int64) block.size(), total - at));
            }
            w.close();
        }

        lynx::Song song = lynx::Song::makeDefault();
        song.bpm = 120.0;
        song.mode = lynx::PlayMode::song;
        song.playlist.clear();
        lynx::ChannelDef c;
        c.name = "Clicks";
        c.instrument = lynx::InstrumentKind::audioClip;
        c.samplePath = clickFile.getFullPathName().toStdString();
        c.mixerTrack = 0;
        c.volume = 1.0f;
        song.channels.push_back (c);
        lynx::Clip clip;
        clip.kind = lynx::ClipKind::audio;
        clip.channel = (int) song.channels.size() - 1;
        clip.track = 0;
        clip.start = 0;
        clip.length = (std::int64_t) beats * lynx::ppq;
        song.playlist.push_back (clip);
        if (mode == 1) // loop recording, bars 2-4
        {
            song.loopStart = 4 * lynx::ppq;
            song.loopEnd = 16 * lynx::ppq;
        }
        else if (mode == 2) // punch in/out, bars 3-4
        {
            song.loopStart = 8 * lynx::ppq;
            song.loopEnd = 16 * lynx::ppq;
        }
        if (song.mixer.size() > 1)
        {
            song.mixer[1].name = "Loop In";
            song.mixer[1].monitor = lynx::InputMonitor::off; // (the loopback must not hear itself)
        }
        engine->setSong (std::move (song));
        engine->setInsertInput (1, 0, false);
        juce::ignoreUnused (pdcSamples);

        // 1) the ping
        if (const auto error = engine->startLatencyPing(); error.isNotEmpty())
            step ("ping_error=" + error);
        juce::Timer::callAfterDelay (3500, [this, step, finish, seconds, rate, report, mode] {
            if (engine == nullptr)
                return;
            if (engine->isPinging())
                engine->cancelLatencyPing();
            const auto r = engine->finishLatencyPing();
            step ("ping=" + juce::String (r.ok ? "ok" : "FAILED " + juce::String (r.problem)) + " samples " + juce::String (r.samples) + " spread " + juce::String (r.spread)
                  + " found " + juce::String (r.found) + "/" + juce::String (r.sent) + " peak " + juce::String (juce::Decibels::gainToDecibels (r.peak, -150.0f), 1) + " dB");
            const auto l = engine->currentLatency();
            step ("compensation=" + juce::String (l.total()) + " (round trip " + juce::String (l.roundTrip()) + ", manual " + juce::String (l.manual) + ", pdc " + juce::String (l.pdc) + ")");

            // 2) record
            engine->setInsertArmed (1, true);
            auto o = engine->getRecordOptions();
            o.recordAudio = true;
            o.countInBars = 0;
            o.waitForInput = false;
            o.punchLoopRegion = mode == 2;
            o.preRollBars = mode == 2 ? 1 : 0;
            o.postRollBars = mode == 2 ? 1 : 0;
            engine->setRecordOptions (o);
            engine->stop();
            engine->rewind();
            engine->setRecording (true);
            juce::Timer::callAfterDelay (300, [this, step, finish, seconds, rate, report, mode] {
                if (engine == nullptr)
                    return;
                engine->startPlayback(); // (as the Play button: pre-roll / punch)
                juce::Timer::callAfterDelay ((int) (seconds * 1000.0), [this, step, finish, seconds, rate, report, mode] {
                    if (engine == nullptr)
                        return;
                    if (mode == 2)
                        step (juce::String ("stopped_by_post_roll=") + (engine->getTransport().playing.load() ? "no" : "yes"));
                    engine->stop();
                    juce::Timer::callAfterDelay (500, [this, step, finish, seconds, rate, report, mode] {
                        if (engine == nullptr)
                            return;
                        engine->setRecording (false);
                        const auto& s = engine->getSong();
                        const auto tps = 120.0 * lynx::ppq / (60.0 * rate);
                        int take = -1;
                        for (int ch = 0; ch < (int) s.channels.size(); ++ch)
                            if (s.channels[(std::size_t) ch].instrument == lynx::InstrumentKind::audioClip && s.channels[(std::size_t) ch].name != "Clicks")
                                take = ch;
                        if (take < 0)
                        {
                            step ("result=ERROR no take (" + engine->getLastRecordingError() + ")");
                            finish (1);
                            return;
                        }
                        std::vector<std::vector<float>> audio;
                        double takeRate = 0.0;
                        const auto& path = s.channels[(std::size_t) take].samplePath;
                        lynx::takes::readWav (path, audio, &takeRate);
                        const auto frames = audio.empty() ? (juce::int64) 0 : (juce::int64) audio[0].size();
                        step ("take=" + juce::String::fromUTF8 (path.c_str()) + " frames " + juce::String (frames) + " (" + juce::String ((double) frames / rate, 3)
                              + " s of " + juce::String (seconds, 1) + " s played) lost " + juce::String (engine->getLostRecordingFrames()));
                        // the clicks in the file (peaks above -12 dBFS, one per 1/8 s at most)
                        std::vector<juce::int64> peaks;
                        if (! audio.empty())
                            for (juce::int64 f = 1; f + 1 < frames; ++f)
                            {
                                const auto x = std::abs (audio[0][(std::size_t) f]);
                                if (x > 0.25f && x >= std::abs (audio[0][(std::size_t) f - 1]) && x > std::abs (audio[0][(std::size_t) f + 1])
                                    && (peaks.empty() || f - peaks.back() > (juce::int64) (rate / 8)))
                                    peaks.push_back (f);
                            }
                        // every clip of the take: each click inside it against the nearest beat
                        int clips = 0, checked = 0, muted = 0;
                        double worst = 0.0, sum = 0.0;
                        for (const auto& c : s.playlist)
                        {
                            if (c.kind != lynx::ClipKind::audio || c.channel != take)
                                continue;
                            ++clips;
                            muted += c.muted ? 1 : 0;
                            const auto skip = ((double) c.offset + (double) c.offsetFraction) / tps; // file samples before the clip start
                            for (const auto f : peaks)
                            {
                                const auto intoClip = (double) f - skip;
                                if (intoClip < 0.0 || intoClip * tps >= (double) c.length)
                                    continue;
                                const auto songTick = (double) c.start + intoClip * tps - 2.0 * tps; // (the pulse's peak is its 3rd sample)
                                const auto beat = std::round (songTick / lynx::ppq) * lynx::ppq;
                                const auto error = (songTick - beat) / tps;
                                worst = std::max (worst, std::abs (error));
                                sum += error;
                                ++checked;
                            }
                            step ("clip track " + juce::String (c.track) + " start " + juce::String ((juce::int64) c.start) + " length " + juce::String ((juce::int64) c.length)
                                  + " offset " + juce::String ((juce::int64) c.offset) + juce::String (c.offsetFraction >= 0.0f ? "+" : "") + juce::String (c.offsetFraction, 4)
                                  + (c.muted ? " (muted lane)" : ""));
                        }
                        step ("clicks_in_file=" + juce::String ((int) peaks.size()) + " checked=" + juce::String (checked) + " clips=" + juce::String (clips)
                              + " muted_lanes=" + juce::String (muted));
                        step ("placement_error_samples=max " + juce::String (worst, 3) + " mean " + juce::String (checked > 0 ? sum / checked : 0.0, 3));
                        auto ok = checked > 0 && worst <= 1.0 && engine->getLostRecordingFrames() == 0;
                        if (mode == 2) // one clip, exactly the punch range, its 8 beats
                        {
                            const auto& sp = engine->getSong();
                            bool exact = false;
                            for (const auto& c : sp.playlist)
                                if (c.kind == lynx::ClipKind::audio && c.channel == take)
                                    exact = c.start == sp.loopStart && c.start + c.length == sp.loopEnd;
                            step (juce::String ("punch_clip_matches_range=") + (exact ? "yes" : "no"));
                            ok = ok && clips == 1 && exact && checked == 8;
                        }
                        step (juce::String ("result=") + (ok ? "OK" : "FAILED"));
                        finish (ok ? 0 : 1);
                    });
                });
            });
        });
    }

    // --hwinserttest (round 164d): see the command line above.
    void runHwInsertTest (const juce::File& report, double seconds, const juce::String& deviceType, const juce::String& deviceName,
                          const juce::String& inputName, int bufferSize)
    {
        engine = std::make_unique<lynx::AudioEngine>();
        savedDeviceState = engine->getDeviceManager().createStateXml();
        auto text = std::make_shared<juce::String>();
        auto step = [report, text] (const juce::String& line) { *text << line << "\n"; report.replaceWithText (*text); };
        auto finish = [this] (int code) {
            if (engine != nullptr)
                engine->getMeters().capture.setSource (lynx::AudioCapture::off);
            restoreSavedDeviceState();
            setApplicationReturnValue (code);
            quit();
        };
        auto& devices = engine->getDeviceManager();
        if (deviceType.isNotEmpty() && devices.getCurrentAudioDeviceType() != deviceType)
            devices.setCurrentAudioDeviceType (deviceType, true);
        {
            auto setup = devices.getAudioDeviceSetup();
            if (deviceName.isNotEmpty())
                setup.outputDeviceName = deviceName;
            if (bufferSize > 0)
                setup.bufferSize = bufferSize;
            if (const auto error = devices.setAudioDeviceSetup (setup, true); error.isNotEmpty())
                step ("device_error=" + error);
        }
        if (const auto r = engine->openAudioInput (inputName.isNotEmpty() ? inputName : deviceName); r.issue != lynx::AudioEngine::InputIssue::none)
            step ("input_error=" + juce::String ((int) r.issue) + " " + r.detail);
        auto* device = devices.getCurrentAudioDevice();
        if (device == nullptr || engine->getInputChannelNames().empty())
        {
            step ("result=ERROR no device / no input");
            finish (1);
            return;
        }
        const auto rate = device->getCurrentSampleRate();
        step ("device=" + device->getTypeName() + " | " + device->getName() + " | " + juce::String (rate) + " Hz | block " + juce::String (device->getCurrentBufferSizeSamples()));

        // the click track: the ping's pulse on every beat at 120 bpm
        const auto beats = (int) std::ceil (seconds * 2.0) + 8;
        const auto clickFile = report.getSiblingFile (report.getFileNameWithoutExtension() + "-clicks.wav");
        {
            lynx::takes::WavTakeWriter w;
            w.open (clickFile.getFullPathName().toStdString(), rate, 1, lynx::takes::SampleFormat::float32);
            const auto total = (juce::int64) std::llround ((double) beats * rate / 2.0);
            std::vector<float> block (4096);
            for (juce::int64 at = 0; at < total; at += (juce::int64) block.size())
            {
                std::fill (block.begin(), block.end(), 0.0f);
                for (int k = 0; k < (int) block.size(); ++k)
                {
                    const auto f = at + k;
                    const auto beat = (juce::int64) std::floor ((double) f / (rate / 2.0));
                    const auto rel = f - (juce::int64) std::llround ((double) beat * rate / 2.0);
                    static constexpr float shape[] { 0.25f, 0.75f, 1.0f, 0.75f, 0.25f };
                    if (rel >= 0 && rel < 5)
                        block[(std::size_t) k] = 0.5f * shape[rel];
                }
                const float* d[] { block.data() };
                w.write (d, (int) std::min<juce::int64> ((juce::int64) block.size(), total - at));
            }
            w.close();
        }

        lynx::Song song = lynx::Song::makeDefault();
        song.bpm = 120.0;
        song.mode = lynx::PlayMode::song;
        song.playlist.clear();
        song.channels.clear();
        for (int k = 0; k < 2; ++k)
        {
            lynx::ChannelDef c;
            c.name = k == 0 ? "Wet" : "Dry";
            c.instrument = lynx::InstrumentKind::audioClip;
            c.samplePath = clickFile.getFullPathName().toStdString();
            c.mixerTrack = k + 1;
            c.volume = 1.0f;
            song.channels.push_back (c);
            lynx::Clip clip;
            clip.kind = lynx::ClipKind::audio;
            clip.channel = k;
            clip.track = k;
            clip.start = 0;
            clip.length = (std::int64_t) beats * lynx::ppq;
            song.playlist.push_back (clip);
        }
        song.mixer[0].outputChannel = lynx::outputNone; // (the cable carries the send only)
        for (auto& fx : song.mixer[0].effects)          // (no Master effects: the default limiter's PDC would hide nothing, but keep it plain)
            fx.enabled = false;
        song.mixer[1].pan = -1.0f;
        song.mixer[2].pan = 1.0f;
        auto hw = lynx::makeEffectSlot (lynx::EffectKind::hardwareInsert);
        hw.params[lynx::hwinsert::output] = lynx::hwinsert::channelValue (0);
        hw.params[lynx::hwinsert::input] = lynx::hwinsert::channelValue (0);
        hw.params[lynx::hwinsert::inputStereo] = 1.0f;
        song.mixer[1].effects.clear();
        song.mixer[1].effects.push_back (hw);
        engine->setSong (std::move (song));
        const auto meter = [this] (int output) {
            const auto* times = engine->getProcessTimes();
            const auto key = (std::size_t) (1 * lynx::maxEffectSlots + 0) * lynx::ProcessTimes::meterOutputs + (std::size_t) output;
            return times != nullptr && key < times->effectMeters.size() ? times->effectMeters[key].load() : 0.0f;
        };

        // 1) the insert's ping (as its Ping button: a momentary value flipped)
        juce::Timer::callAfterDelay (500, [this, report, step, finish, seconds, rate, meter] {
            if (engine == nullptr)
                return;
            engine->editSong ([] (lynx::Song& s) { s.mixer[1].effects[0].params[lynx::hwinsert::ping] = 1.0f; });
            juce::Timer::callAfterDelay (3500, [this, report, step, finish, seconds, rate, meter] {
                if (engine == nullptr)
                    return;
                const auto state = meter (lynx::hwinsert::pingState);
                const auto samples = lynx::hwinsert::roundTripOf (meter (lynx::hwinsert::pingSamples));
                step ("ping_state=" + juce::String (state, 3) + (std::abs (state - 0.5f) < 0.01f ? " (ok)" : " (FAILED)") + " round_trip=" + juce::String (samples)
                      + " status_flags=" + juce::String (juce::roundToInt (meter (lynx::hwinsert::status) * 16.0f)));
                if (std::abs (state - 0.5f) >= 0.01f)
                {
                    step ("result=FAILED ping");
                    finish (1);
                    return;
                }
                engine->editSong ([samples] (lynx::Song& s) { s.mixer[1].effects[0].params[lynx::hwinsert::roundTrip] = lynx::hwinsert::roundTripValue (samples); });

                // 2) play, capturing the Master
                juce::Timer::callAfterDelay (500, [this, report, step, finish, seconds, rate, samples] {
                    if (engine == nullptr)
                        return;
                    step ("pdc_total=" + juce::String (engine->getLatencySamples()) + " (expected " + juce::String (samples) + ")");
                    // saved in the project: the slot comes back the same (routing, round trip), and its latency with it — also an
                    // odd round trip (12345 samples: the project keeps it to the sample)
                    {
                        auto odd = engine->getSong();
                        odd.mixer[1].effects[0].params[lynx::hwinsert::roundTrip] = lynx::hwinsert::roundTripValue (12345);
                        juce::String error;
                        auto same = true;
                        for (const auto* song : { &engine->getSong(), (const lynx::Song*) &odd })
                        {
                            const auto file = report.getSiblingFile (report.getFileNameWithoutExtension() + (song == &odd ? "-odd.lynx" : ".lynx"));
                            const auto saved = lynx::ProjectFile::save (*song, file, error);
                            const auto loaded = saved ? lynx::ProjectFile::load (file, error) : std::nullopt;
                            same = same && loaded && loaded->mixer.size() > 1 && ! loaded->mixer[1].effects.empty()
                                   && loaded->mixer[1].effects[0].kind == lynx::EffectKind::hardwareInsert
                                   && loaded->mixer[1].effects[0].params == song->mixer[1].effects[0].params
                                   && lynx::makeEffect (loaded->mixer[1].effects[0])->latencySamples() == (song == &odd ? 12345 : samples);
                        }
                        const auto file = report.getSiblingFile (report.getFileNameWithoutExtension() + ".lynx");
                        step (juce::String ("project_round_trip=") + (same ? "same" : "DIFFERENT " + error) + " (" + file.getFileName() + ")");
                        if (! same)
                        {
                            step ("result=FAILED project");
                            finish (1);
                            return;
                        }
                    }
                    auto left = std::make_shared<std::vector<float>>(), right = std::make_shared<std::vector<float>>();
                    engine->stop();
                    engine->rewind();
                    engine->getMeters().capture.setSource (0);
                    engine->play();
                    auto pulls = std::make_shared<int> ((int) (seconds * 5.0));
                    auto pull = std::make_shared<std::function<void()>>();
                    *pull = [this, report, step, finish, seconds, rate, samples, left, right, pulls, pull] {
                        if (engine == nullptr)
                            return;
                        engine->getMeters().capture.pull (*left, *right);
                        if (*pulls == 10) // the insert's editor while it plays (next to the report)
                            for (const auto scale : { 1.0f, 1.5f })
                            {
                                auto editor = lynx::makeNativeEffectEditor (*engine, 1, 0, scale, 0);
                                juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                                {
                                    juce::Graphics g (image);
                                    g.fillAll (lynx::theme::panel);
                                    editor->paintEntireComponent (g, false);
                                }
                                const auto file = report.getSiblingFile (report.getFileNameWithoutExtension() + (scale > 1.0f ? "-editor150.png" : "-editor.png"));
                                file.deleteFile();
                                juce::PNGImageFormat png;
                                if (auto stream = file.createOutputStream())
                                    png.writeImageToStream (image, *stream);
                            }
                        if (--*pulls > 0)
                        {
                            juce::Timer::callAfterDelay (200, *pull);
                            return;
                        }
                        engine->stop();
                        const auto dropped = engine->getMeters().capture.droppedFrames();
                        engine->getMeters().capture.setSource (lynx::AudioCapture::off);
                        // the clicks of each side (peaks above -12 dBFS, one per 1/8 s at most)
                        auto peaksOf = [rate] (const std::vector<float>& x) {
                            std::vector<juce::int64> peaks;
                            for (std::size_t f = 1; f + 1 < x.size(); ++f)
                                if (std::abs (x[f]) > 0.25f && std::abs (x[f]) >= std::abs (x[f - 1]) && std::abs (x[f]) > std::abs (x[f + 1])
                                    && (peaks.empty() || (juce::int64) f - peaks.back() > (juce::int64) (rate / 8)))
                                    peaks.push_back ((juce::int64) f);
                            return peaks;
                        };
                        const auto wet = peaksOf (*left), dry = peaksOf (*right);
                        int pairs = 0;
                        juce::int64 worst = 0;
                        double gainSum = 0.0, residual = 0.0, energy = 0.0;
                        for (const auto w : wet)
                        {
                            auto best = std::min_element (dry.begin(), dry.end(), [w] (juce::int64 a, juce::int64 b) { return std::abs (a - w) < std::abs (b - w); });
                            if (best == dry.end() || std::abs (*best - w) > (juce::int64) (rate / 8))
                                continue;
                            ++pairs;
                            worst = std::max (worst, std::abs (*best - w));
                            gainSum += (*left)[(std::size_t) w] / (*right)[(std::size_t) *best];
                        }
                        const auto gain = pairs > 0 ? gainSum / pairs : 1.0;
                        // dry clicks with no wet click beside them (a dropout of the cable / device), listed
                        juce::String unmatched;
                        for (const auto d : dry)
                            if (std::none_of (wet.begin(), wet.end(), [d, rate] (juce::int64 w) { return std::abs (w - d) <= (juce::int64) (rate / 8); }))
                                unmatched << " " << (juce::int64) d << " (" << juce::String ((*left)[(std::size_t) d], 3) << ")";
                        if (unmatched.isNotEmpty())
                            step ("dry_clicks_without_wet=" + unmatched);
                        // the null test: left − gain · right around each click (both sides aligned → only the cable's colour remains)
                        for (const auto w : wet)
                            for (juce::int64 f = std::max<juce::int64> (0, w - 16); f < std::min<juce::int64> ((juce::int64) left->size(), w + 16); ++f)
                            {
                                const auto d = (double) (*left)[(std::size_t) f] - gain * (double) (*right)[(std::size_t) f];
                                residual += d * d;
                                energy += (double) (*left)[(std::size_t) f] * (*left)[(std::size_t) f];
                            }
                        const auto nullDb = energy > 0.0 ? 10.0 * std::log10 (std::max (1e-30, residual / energy)) : 0.0;
                        step ("captured=" + juce::String ((int) left->size()) + " frames dropped=" + juce::String (dropped) + " wet_clicks=" + juce::String ((int) wet.size())
                              + " dry_clicks=" + juce::String ((int) dry.size()) + " pairs=" + juce::String (pairs));
                        step ("alignment_error_samples=max " + juce::String ((int) worst) + " cable_gain=" + juce::String (gain, 4) + " null_residual_db=" + juce::String (nullDb, 1));
                        auto* dev = engine->getDeviceManager().getCurrentAudioDevice();
                        const auto xruns = dev != nullptr ? dev->getXRunCount() : -1;
                        step ("device_xruns=" + juce::String (xruns) + " (a dropout loses what was on the cable then: its clicks are excused)");
                        const auto excused = xruns > 0 ? 2 * xruns : 0;
                        const auto ok = pairs >= (int) (seconds * 2.0) - 2 - excused && worst == 0 && dropped == 0 && engine->getLatencySamples() == samples;
                        step (juce::String ("result=") + (ok ? "OK" : "FAILED"));
                        finish (ok ? 0 : 1);
                    };
                    juce::Timer::callAfterDelay (200, *pull);
                });
            });
        });
    }

    // --rtstress --record: stops recording (the take becomes a clip of the song, as in the app), moves the take's WAV to
    // `target` and describes it: length against the time played, channels, peak, the latencies the driver reported.
    juce::String finishStressTake (const juce::File& target, double seconds)
    {
        juce::String text;
        auto* device = engine->getDeviceManager().getCurrentAudioDevice();
        text << "  recording: input " << (device != nullptr ? device->getActiveInputChannels().countNumberOfSetBits() : 0) << " channel(s)";
        if (device != nullptr)
            text << ", input latency " << device->getInputLatencyInSamples() << " + output latency " << device->getOutputLatencyInSamples() << " samples";
        engine->setRecording (false); // closes the take: a clip of the song
        juce::File take;
        for (const auto& c : engine->getSong().channels)
            if (c.instrument == lynx::InstrumentKind::audioClip)
                take = juce::File (juce::String::fromUTF8 (c.samplePath.c_str()));
        if (! take.existsAsFile())
            return text + "\n  recording: no take (" + engine->getLastRecordingError() + ")\n";
        target.deleteFile();
        if (! take.moveFileTo (target))
            take.copyFileTo (target);
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        if (std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (target)); reader != nullptr)
        {
            juce::AudioBuffer<float> audio ((int) reader->numChannels, (int) reader->lengthInSamples);
            reader->read (&audio, 0, (int) reader->lengthInSamples, 0, true, true);
            const auto peak = audio.getMagnitude (0, audio.getNumSamples());
            const auto length = (double) reader->lengthInSamples / reader->sampleRate;
            text << "\n  recording: take " << juce::String (length, 3) << " s of " << juce::String (seconds, 0) << " s played, "
                 << (int) reader->numChannels << " ch @ " << (int) reader->sampleRate << " Hz, peak "
                 << juce::String (juce::Decibels::gainToDecibels (peak, -150.0f), 1) << " dBFS -> " << target.getFullPathName() << "\n";
        }
        else
            text << "\n  recording: take unreadable (" << target.getFullPathName() << ")\n";
        return text;
    }

    // LYNX.exe --render project.(flp|lynx) output.wav [seconds]
    // Exports the current pattern to 24-bit WAV (for an imported .flp, the one with the most notes).
    void runRender (const juce::String& inputPath, const juce::String& outputPath, double seconds)
    {
        lynx::AudioEngine offline (false);
        const juce::File input (inputPath);
        juce::String error, report;
        bool ok = false;

        if (input.hasFileExtension ("flp"))
            ok = offline.importFlpFile (input, error);
        else if (auto song = lynx::ProjectFile::load (input, error))
        {
            offline.setSong (std::move (*song));
            ok = true;
        }

        int samplers = 0, samplersLoaded = 0;

        if (ok)
        {
            for (const auto& c : offline.getSong().channels)
                if (c.instrument == lynx::InstrumentKind::sampler || c.instrument == lynx::InstrumentKind::audioClip)
                {
                    ++samplers;
                    samplersLoaded += offline.getSampleLibrary().load (juce::String::fromUTF8 (c.samplePath.c_str())) != nullptr;
                }

            const juce::File output (outputPath);
            output.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatWriter> writer (
                wav.createWriterFor (output.createOutputStream().release(), 44100.0, 2, 24, {}, 0));

            if (writer == nullptr)
            {
                ok = false;
                error = lynx::utf8 ("could not create ") + outputPath;
            }
            else
            {
                const auto peak = offline.renderOffline (seconds > 0.0 ? seconds : 10.0, 44100.0, writer.get());
                report << "peak=" << peak << "\n";
                ok = peak > 0.001f;
            }
        }

        report << "samples=" << samplersLoaded << "/" << samplers << "\n"
               << "result=" << (ok ? "OK" : "ERROR " + error) << "\n";
        juce::File (outputPath + ".txt").replaceWithText (report);

        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // LYNX.exe --export project.(flp|lynx) output.(wav|flac|ogg|mp3) [--pattern] [--insert N] [--rate N]
    // Same path as "Export" in the UI: the whole song (or the current pattern) with its tail.
    void runExport (const juce::String& inputPath, const juce::String& outputPath, bool patternOnly, int insert)
    {
        juce::String error, report;
        bool ok = false;
        const auto started = juce::Time::getMillisecondCounterHiRes();
        lynx::loadtrace::begin (inputPath.toStdString()); // phase times (round 112): report lines + load.log
        {
            lynx::AudioEngine loader (false);
            const juce::File input (inputPath);

            // --find-plugins: first the targeted scan opening the project in the app runs (its plugins not known yet)
            if (input.hasFileExtension ("flp") && juce::JUCEApplication::getCommandLineParameters().contains ("--find-plugins"))
            {
                juce::MemoryBlock data;
                if (input.loadFileAsData (data))
                    try
                    {
                        const auto uses = lynx::flpForeignPlugins (flp::parseProject ({ static_cast<const std::uint8_t*> (data.getData()), data.getSize() }));
                        auto& host = loader.getPluginHost();
                        const auto listed = std::chrono::steady_clock::now();
                        const auto files = host.filesToScanFor (uses);
                        lynx::loadtrace::add ("plugin_scan_list", lynx::loadtrace::msSince (listed));
                        report << "scanned=" << files.joinIntoString (";") << "\n";
                        const auto scanned = std::chrono::steady_clock::now();
                        host.scanFiles (files, {});
                        lynx::loadtrace::add ("plugin_scan", lynx::loadtrace::msSince (scanned), files.size());
                    }
                    catch (const flp::Error&)
                    {
                    }
            }

            if (input.hasFileExtension ("flp"))
                ok = loader.importFlpFile (input, error);
            else if (auto song = lynx::ProjectFile::load (input, error))
            {
                loader.setSong (std::move (*song));
                ok = true;
            }

            if (ok)
            {
                const juce::File output (outputPath);
                lynx::ExportSettings settings;
                settings.wholeSong = ! patternOnly;
                settings.format = output.hasFileExtension ("flac") ? lynx::ExportSettings::Format::flac24
                                  : output.hasFileExtension ("ogg") ? lynx::ExportSettings::Format::ogg
                                                                    : lynx::ExportSettings::Format::wav24;
                // --insert N: only that insert's output (what "Render to audio" records)
                if (juce::JUCEApplication::getCommandLineParameters().contains ("--no-effects")
                    || juce::JUCEApplication::getCommandLineParameters().contains ("--sem-efeitos")) // performance diagnostics; --sem-efeitos: legacy alias
                    loader.editSong ([] (lynx::Song& s) {
                        for (auto& t : s.mixer)
                            t.effects.clear();
                    }, false);

                if (juce::JUCEApplication::getCommandLineParameters().contains ("--no-automation")
                    || juce::JUCEApplication::getCommandLineParameters().contains ("--sem-automacao")) // --sem-automacao: legacy alias
                    loader.editSong ([] (lynx::Song& s) { s.automations.clear(); }, false);

                const auto loaded = juce::Time::getMillisecondCounterHiRes();
                // .mp3: the Export window's MP3 (320 kbps); everything else as before
                auto options = settings.toOptions();
                // --no-warmup: the export's plugin warm-up off (round 155: compare the start of the file)
                if (juce::JUCEApplication::getCommandLineParameters().contains ("--no-warmup"))
                    options.pluginWarmUp = false;
                // --rate N: render at another sample rate (round 157b: samples resampled as at a 48 kHz device)
                if (const auto args = juce::JUCEApplication::getCommandLineParameterArray(); args.contains ("--rate"))
                    if (const auto rate = args[args.indexOf ("--rate") + 1].getDoubleValue(); rate >= 8000.0 && rate <= 384000.0)
                        options.sampleRate = rate;
                if (output.hasFileExtension ("mp3"))
                {
                    options.format = lynx::exportopts::Format::mp3;
                    options.mp3Kbps = 320;
                }
                lynx::ExportJob job (insert > 0 ? lynx::makeBounceSong (loader.getSong(), insert) : loader.getSong(), options, output);
                const auto prepared = juce::Time::getMillisecondCounterHiRes();
                lynx::loadtrace::end();
                int lastPercent = -1;
                ok = job.run ([&lastPercent] (double p) {
                    lastPercent = (int) (p * 100.0);
                    return true;
                }, error);
                auto joined = [] (const std::map<std::string, int>& items) {
                    juce::StringArray names;
                    for (const auto& [name, count] : items)
                        names.add (juce::String::fromUTF8 (name.c_str()) + "x" + juce::String (count));
                    return names.joinIntoString (", ");
                };
                const auto& importReport = loader.getImportReport();
                report << "converted=" << joined (importReport.converted) << "\n"
                       << "approximated=" << joined (importReport.approximated) << "\n"
                       << "unsupported=" << joined (importReport.unsupported) << "\n"
                       << "dropped_effects=" << joined (importReport.droppedEffects) << "\n"
                       << "partial_effects=" << joined (importReport.partialEffects) << "\n"
                       << "missing_samples=" << importReport.missingSamples << "\n";
                // third-party plugins: how each was found, or what plays in its place (diagnostics for level differences)
                for (const auto& line : loader.getMatchedPlugins())
                    report << "plugin_found=" << line << "\n";
                for (const auto& m : loader.getMissingPluginDetails())
                    report << "plugin_missing=" << juce::String::fromUTF8 (m.name.c_str()) << " -> "
                           << juce::String::fromUTF8 (m.replacement.c_str()).replace ("{0}", juce::String::fromUTF8 (m.replacementArg.c_str())) << "\n";
                {
                    const auto& song = loader.getSong();
                    for (std::size_t t = 0; t < song.mixer.size(); ++t)
                    {
                        const auto& track = song.mixer[t];
                        if (track.effects.empty() && track.sends.empty())
                            continue;
                        report << "insert " << (int) t << " vol=" << juce::String (track.volume / lynx::unityFader, 3)
                               << (track.muted ? " muted" : "") << (track.toMaster ? "" : " noMaster");
                        for (const auto& s : track.sends)
                            report << " send>" << s.target << "@" << juce::String (s.level, 3);
                        report << " |";
                        for (const auto& fx : track.effects)
                            report << " [" << (fx.kind == lynx::EffectKind::plugin ? juce::String::fromUTF8 (fx.plugin.name.c_str())
                                                                                 : juce::String::fromUTF8 (lynx::effectInfo (fx.kind).name))
                                   << (fx.isSubstitute() ? " (substitute)" : "") << (fx.enabled ? "" : " off")
                                   << (fx.mix < 0.999f ? " mix=" + juce::String (fx.mix, 2) : juce::String()) << "]";
                        report << "\n";
                    }
                }
                report << juce::String (lynx::loadtrace::reportLines());
                lynx::PluginHost::settingsFolder().getChildFile ("load.log").replaceWithText (juce::String (lynx::loadtrace::table()));
                report << "open_ms=" << juce::roundToInt (loaded - started) << "\n"
                       << "prepare_ms=" << juce::roundToInt (prepared - loaded) << "\n"
                       << "render_ms=" << juce::roundToInt (juce::Time::getMillisecondCounterHiRes() - prepared) << "\n"
                       << "graph_ms=" << juce::roundToInt (job.getEngine().getLastRenderStats().graphMs) << "\n"
                       << "write_ms=" << juce::roundToInt (job.getEngine().getLastRenderStats().writeMs) << "\n"
                       << "warmup_blocks=" << job.getEngine().getLastRenderStats().warmUp.blocks << "\n"
                       << "warmup_ms=" << juce::roundToInt (job.getEngine().getLastRenderStats().warmUp.wallSeconds * 1000.0) << "\n"
                       << "warmup_changes=" << job.getEngine().getLastRenderStats().warmUp.signatureChanges << "\n"
                       << "warmup_timed_out=" << (job.getEngine().getLastRenderStats().warmUp.timedOut ? 1 : 0) << "\n"
                       << "pdc_trim=" << job.getEngine().getLastRenderStats().trimmedSamples << "\n"
                       << "song_seconds=" << job.getMusicSeconds() << "\n"
                       << "peak=" << job.getPeak() << "\n"
                       << "final_progress=" << lastPercent << "\n"
                       << "bytes=" << output.getSize() << "\n";
                ok = ok && job.getPeak() > 0.001f;
            }
        }

        report << "result=" << (ok ? "OK" : "ERROR " + error) << "\n";
        juce::File (outputPath + ".txt").replaceWithText (report);
        setApplicationReturnValue (ok ? 0 : 1);
        quit();
    }

    // LYNX.exe --snapshot prefix [project]: draws the window off-screen and writes
    // prefix-rack.png, -pianoroll.png and -playlist.png (to check the UI without touching the screen).
    // --editorcpu: one editor after another; each phase runs 2 s to settle and 8 s measured
    struct EditorCpuRun final : juce::Timer
    {
        struct Case { lynx::EffectKind kind; const char* name; int type; bool demo; }; // type 0 native, 1 display-first, 2 Facet EQ
        std::vector<Case> cases;
        std::size_t at = 0;
        int phase = 0;
        double t0 = 0.0, c0 = 0.0;
        juce::String report;
        juce::File out;
        std::unique_ptr<lynx::AudioEngine> engine;
        std::unique_ptr<juce::DocumentWindow> window;
        std::function<void()> done;

        void start()
        {
            engine = std::make_unique<lynx::AudioEngine> (false);
            report << "editor\tmode\tcpu % of one core\n";
            open();
            startTimer (250);
        }
        void open()
        {
            window.reset();
            if (at >= cases.size()) { out.replaceWithText (report); stopTimer(); if (done) done(); return; }
            const auto& c = cases[at];
            lynx::Song song;
            song.mixer.resize (2);
            song.mixer[0].name = "Master";
            song.mixer[1].name = "Insert 1";
            song.mixer[1].effects.push_back (lynx::makeEffectSlot (c.kind));
            engine->setSong (std::move (song));
            engine->dispatchPendingMessages();
            lynx::setNativeEditorDemo (c.demo);
            auto editor = c.type == 1 ? lynx::makeProEditor (*engine, 1, 0, c.demo) : c.type == 2 ? lynx::makeFacetEqEditor (*engine, 1, 0, c.demo)
                                                                                                   : lynx::makeNativeEffectEditor (*engine, 1, 0, 1.0f, 0);
            window = std::make_unique<juce::DocumentWindow> (juce::String ("LYNX editor CPU - ") + c.name, lynx::theme::colour::bg1, 0);
            window->setUsingNativeTitleBar (true);
            window->setContentOwned (editor.release(), true);
            const auto area = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay()->userArea;
            window->setTopLeftPosition (area.getRight() - window->getWidth() - 8, area.getBottom() - window->getHeight() - 8);
            window->setVisible (true);
            phase = 0;
            t0 = juce::Time::getMillisecondCounterHiRes();
        }
        void timerCallback() override
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            if (phase == 0 && now - t0 > 2000.0) { phase = 1; t0 = now; c0 = lynx::processCpuSeconds(); return; }
            if (phase == 1 && now - t0 > 8000.0)
            {
                const auto cpu = (lynx::processCpuSeconds() - c0) / ((now - t0) * 0.001) * 100.0;
                const auto& c = cases[at];
                report << c.name << "\t" << (c.demo ? "animating" : "idle") << "\t" << juce::String (cpu, 2) << "\n";
                ++at;
                open();
            }
        }
    };
    std::unique_ptr<EditorCpuRun> editorCpu;

    void runEditorCpu (const juce::String& path)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        editorCpu = std::make_unique<EditorCpuRun>();
        editorCpu->out = juce::File (path);
        using K = lynx::EffectKind;
        for (const auto demo : { false, true })
            for (const auto& c : std::vector<EditorCpuRun::Case> { { K::facetEq, "Facet EQ", 2, demo }, { K::crestCompressor, "Crest Compressor", 1, demo },
                                                                  { K::apexLimiter, "Apex Limiter", 1, demo }, { K::prismMultiband, "Prism Multiband", 1, demo },
                                                                  { K::epochVerb, "Epoch Verb", 0, demo }, { K::glowLeveler, "Glow Leveler", 0, demo },
                                                                  { K::fruityLimiter, "Fruity Limiter", 0, demo }, { K::lynxTune, "LYNX Tune", 0, demo } })
                editorCpu->cases.push_back (c);
        editorCpu->done = [this] { juce::MessageManager::callAsync ([this] { editorCpu.reset(); quit(); }); };
        editorCpu->start();
    }

    void runPluginEditorSnapshots (const juce::String& folder)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        {
            const juce::File out (folder);
            out.createDirectory();
            lynx::AudioEngine offline (false);
            const auto roots = lynx::findFlStudioRoots();
            const auto presets = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin presets/Effects");

            auto kinds = lynx::plugui::flLayoutKinds();
            kinds.push_back (lynx::EffectKind::maximus); // Soundgoodizer (below)
            for (const auto own : lynx::plugui::ownLayoutKinds()) // LYNX's own panels (mixing suite, round 96)
                kinds.push_back (own);
            kinds.push_back (lynx::EffectKind::apexLimiter); // round 114: display-first editor
            kinds.push_back (lynx::EffectKind::ridgeStrip); // round 145: the second pass shows the Vocal strip voicing
            kinds.push_back (lynx::EffectKind::compressor); // round 152: the RComp role's panel (vintage voicing, RComp ranges)
            const auto database = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin database/Effects");

            auto maximusPasses = 0; // Maximus has its own page (first pass); the second pass shows Soundgoodizer
            auto ridgePasses = 0;
            for (const auto kind : kinds)
            {
                auto fx = lynx::makeEffectSlot (kind);
                juce::String name (lynx::effectInfo (kind).name);
                for (const auto& plugin : lynx::flPluginsForEffect (kind))
                {
                    const auto dir = presets.getChildFile (juce::String::fromUTF8 (plugin.c_str()));
                    auto file = dir.getChildFile ("Default.fst");
                    if (! file.existsAsFile()) // plugins without presets: their entry of FL's plugin database
                        for (const auto& f : database.findChildFiles (juce::File::findFiles, true, juce::String::fromUTF8 (plugin.c_str()) + ".fst"))
                            file = f;
                    if (! file.existsAsFile())
                    {
                        auto files = dir.findChildFiles (juce::File::findFiles, true, "*.fst");
                        std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) { return a.getFullPathName().compareNatural (b.getFullPathName()) < 0; });
                        file = files.isEmpty() ? juce::File() : files[0];
                    }
                    juce::MemoryBlock data;
                    if (file.existsAsFile() && file.loadFileAsData (data))
                    {
                        const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
                        if (auto loaded = lynx::effectFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), 130.0); loaded && loaded->kind == kind)
                            fx = *loaded;
                    }
                    name = juce::String::fromUTF8 (plugin.c_str());
                    break;
                }

                if (kind == lynx::EffectKind::maximus && ++maximusPasses == 2) // Soundgoodizer: preset A of the installation, Amount 60 %
                {
                    juce::MemoryBlock data;
                    juce::File (juce::String::fromUTF8 (roots.factoryData.c_str())).getChildFile ("Plugins/Fruity/Effects/Soundgoodizer/Data/Presets/a.dat").loadFileAsData (data);
                    lynx::maximusfx::Blob blob;
                    blob.build = lynx::hlfx::MaximusBuild::soundgoodizer;
                    blob.mix = 600;
                    const auto* b = static_cast<const std::uint8_t*> (data.getData());
                    blob.state.assign (b, b + data.getSize());
                    fx = lynx::makeEffectSlot (kind);
                    fx.state = lynx::maximusfx::pack (blob);
                    name = "Soundgoodizer";
                }

                if (kind == lynx::EffectKind::ridgeStrip && ++ridgePasses == 2) // round 145: her Vocalstrip 2 setting
                {
                    for (const auto& [param, value] : { std::pair<const char*, float> { "Vocal strip", 1.0f }, { "Compander", 1.0f },
                                                        { "Compander threshold", 0.3083f }, { "Compander ratio", 0.6929f }, { "Compander attack", 0.2543f },
                                                        { "Compander release", 0.5316f }, { "Make-up", 0.2917f }, { "Soft knee", 1.0f },
                                                        { "Expander threshold", 0.2198f }, { "Vocal input", 0.6792f }, { "Vocal output", 0.3938f },
                                                        { "Process order", 0.4783f }, { "De-esser", 1.0f } })
                        fx.params[(std::size_t) lynx::plugui::paramIndex (kind, param)] = value;
                    name = "Ridge Strip (Vocal strip)";
                }
                else if (kind == lynx::EffectKind::ridgeStrip) // a vocal setting, so the graphs show something
                    for (const auto& [param, value] : { std::pair<const char*, float> { "HPF", 0.3f }, { "LMF gain", 0.35f }, { "HF gain", 0.7f },
                                                        { "Ratio", 0.3f }, { "Comp. threshold", 0.55f } })
                        fx.params[(std::size_t) lynx::plugui::paramIndex (kind, param)] = value;
                if (kind == lynx::EffectKind::compressor) // round 152: the vintage voicing with the reference's ranges and meter
                {
                    for (const auto& [param, value] : { std::pair<const char*, float> { "Voicing", 1.0f }, { "Ranges", 1.0f }, { "Limiter", 1.0f },
                                                        { "Threshold", 0.6f }, { "RC ratio", 0.4f } })
                        fx.params[(std::size_t) lynx::plugui::paramIndex (kind, param)] = value;
                    name = "Compressor (RComp role)";
                }
                if (kind == lynx::EffectKind::apexLimiter) // round 114: gain into the ceiling, so the display shows reduction
                    fx.params[0] = 0.33f;
                if (kind == lynx::EffectKind::ember)
                {
                    fx.params[(std::size_t) lynx::plugui::paramIndex (kind, "Drive")] = 0.5f;
                    fx.params[(std::size_t) lynx::plugui::paramIndex (kind, "Bands")] = 1.0f; // round 108: the band rows
                    fx.params[(std::size_t) lynx::plugui::paramIndex (kind, "B3 drive")] = 0.6f;
                }
                if (kind == lynx::EffectKind::crestCompressor) // round 108: a curve with a knee and a side-chain low cut
                    for (const auto& [param, value] : { std::pair<const char*, float> { "Threshold", 0.55f }, { "Knee", 0.15f },
                                                        { "SC1 shape", 0.5f }, { "SC1 freq.", 0.3f } })
                        fx.params[(std::size_t) lynx::plugui::paramIndex (kind, param)] = value;

                lynx::Song song;
                song.mixer.resize (2);
                song.mixer[0].name = "Master";
                song.mixer[1].name = "Insert 1";
                song.mixer[1].effects.push_back (fx);
                offline.setSong (std::move (song));
                offline.dispatchPendingMessages();

                if (lynx::ProEditorWindows::handles (kind)) // round 114: the display-first editors, with a demo signal
                {
                    auto editor = lynx::makeProEditor (offline, 1, 0, true);
                    juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                    {
                        juce::Graphics g (image);
                        editor->paintEntireComponent (g, false);
                    }
                    const auto file = out.getChildFile (name + ".png");
                    file.deleteFile();
                    juce::PNGImageFormat png;
                    if (auto stream = file.createOutputStream())
                        png.writeImageToStream (image, *stream);
                    continue;
                }

                const auto pages = name == "Soundgoodizer" ? 1 : juce::jmax (1, (int) lynx::plugui::layoutFor (kind).pages.size());
                for (int page = 0; page < pages; ++page)
                {
                    auto editor = lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, page);
                    juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                    {
                        juce::Graphics g (image);
                        g.fillAll (lynx::theme::panel);
                        editor->paintEntireComponent (g, false);
                    }
                    const auto file = out.getChildFile (name + (page > 0 ? "-p" + juce::String (page + 1) : juce::String()) + ".png");
                    file.deleteFile();
                    juce::PNGImageFormat png;
                    if (auto stream = file.createOutputStream())
                        png.writeImageToStream (image, *stream);
                }
            }

            // round 137: Echoform in its Rhythm mode (EchoBoy's role) with the Present era
            {
                namespace ef = lynx::echosuite::echoform;
                auto fx = lynx::makeEffectSlot (lynx::EffectKind::echoform);
                fx.params[ef::rhythm] = 1.0f;
                fx.params[ef::rhythmRepeats] = 5.0f / 15.0f;
                fx.params[ef::rhythmDecay] = 0.25f;
                fx.params[ef::rhythmTap1 + 2] = 0.45f;
                fx.params[ef::feedback] = 0.25f;
                fx.params[ef::era] = 0.5f;
                for (const auto& [param, value] : ef::rhythmPresetEdits (0)) // round 144: a pattern in the rhythm editor (Grid)
                    fx.params[(std::size_t) param] = value;
                fx.params[ef::rhythmShape] = 0.5f; // swell
                lynx::Song song;
                song.mixer.resize (2);
                song.mixer[0].name = "Master";
                song.mixer[1].name = "Insert 1";
                song.mixer[1].effects.push_back (fx);
                offline.setSong (std::move (song));
                offline.dispatchPendingMessages();
                auto editor = lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, 0);
                juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                {
                    juce::Graphics g (image);
                    g.fillAll (lynx::theme::panel);
                    editor->paintEntireComponent (g, false);
                }
                const auto file = out.getChildFile ("Echoform-rhythm.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto stream = file.createOutputStream())
                    png.writeImageToStream (image, *stream);
            }

            // round 150: Echoform with a voicing (the Stereo / L/R offset controls on the Rhythm row)
            {
                namespace ef = lynx::echosuite::echoform;
                auto fx = lynx::makeEffectSlot (lynx::EffectKind::echoform);
                fx.params[ef::voicing] = ef::voicingValue (ef::voicingForEchoBoyStyle (19));
                fx.params[ef::style] = ef::styleValue (ef::Style::dual);
                lynx::Song song;
                song.mixer.resize (2);
                song.mixer[0].name = "Master";
                song.mixer[1].name = "Insert 1";
                song.mixer[1].effects.push_back (fx);
                offline.setSong (std::move (song));
                offline.dispatchPendingMessages();
                auto editor = lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, 0);
                juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                {
                    juce::Graphics g (image);
                    g.fillAll (lynx::theme::panel);
                    editor->paintEntireComponent (g, false);
                }
                const auto file = out.getChildFile ("Echoform-voicing.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto stream = file.createOutputStream())
                    png.writeImageToStream (image, *stream);
            }

            // round 150: Auto-pan's Rhythm voicing with its Tweak row (PanMan's role)
            {
                auto fx = lynx::makeEffectSlot (lynx::EffectKind::autoPan);
                fx.params[lynx::autopan::count] = 1.0f;          // the Rhythm voicing
                fx.params[lynx::autopan::count + 13] = 0.75f;   // offset mod +90°
                lynx::Song song;
                song.mixer.resize (2);
                song.mixer[0].name = "Master";
                song.mixer[1].name = "Insert 1";
                song.mixer[1].effects.push_back (fx);
                offline.setSong (std::move (song));
                offline.dispatchPendingMessages();
                auto editor = lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, 0);
                juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                {
                    juce::Graphics g (image);
                    g.fillAll (lynx::theme::panel);
                    editor->paintEntireComponent (g, false);
                }
                const auto file = out.getChildFile ("Auto-pan-tweak.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto stream = file.createOutputStream())
                    png.writeImageToStream (image, *stream);
            }

            // round 152: Foil Plate's one-knob Plate view (the Little Plate role)
            {
                auto fx = lynx::makeEffectSlot (lynx::EffectKind::foilPlate);
                fx.params[14] = 1.0f; // the measured plate voicing
                fx.params[15] = 1.0f; // View = Plate
                lynx::Song song;
                song.mixer.resize (2);
                song.mixer[0].name = "Master";
                song.mixer[1].name = "Insert 1";
                song.mixer[1].effects.push_back (fx);
                offline.setSong (std::move (song));
                offline.dispatchPendingMessages();
                auto editor = lynx::makeNativeEffectEditor (offline, 1, 0, 1.0f, 0);
                juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                {
                    juce::Graphics g (image);
                    g.fillAll (lynx::theme::panel);
                    editor->paintEntireComponent (g, false);
                }
                const auto file = out.getChildFile ("Foil Plate-plate.png");
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto stream = file.createOutputStream())
                    png.writeImageToStream (image, *stream);
            }

            // LYNX's own editors with live displays (round 103): Facet EQ (a mixing curve, one dynamic band, demo analyser)
            // and LYNX Tune (demo pitch phrase)
            {
                lynx::setNativeEditorDemo (true);
                using namespace lynx::facet;
                auto eq = lynx::makeEffectSlot (lynx::EffectKind::facetEq);
                const struct { Shape s; double f, g, q; int slope; double range; } bandsDemo[] = {
                    { Shape::lowCut, 32, 0, 1, 24, 0 }, { Shape::bell, 110, 3, 1.2, 12, 0 }, { Shape::bell, 380, -3.5, 1.6, 12, 0 },
                    { Shape::bell, 3200, 0, 2.5, 12, -6 }, { Shape::highShelf, 9000, 2.5, 0.8, 12, 0 }
                };
                for (int b = 0; b < 5; ++b)
                {
                    eq.params[(std::size_t) param (b, 0)] = shapeValue (bandsDemo[b].s);
                    eq.params[(std::size_t) param (b, 1)] = hzValue (bandsDemo[b].f);
                    eq.params[(std::size_t) param (b, 2)] = gainValue (bandsDemo[b].g);
                    eq.params[(std::size_t) param (b, 3)] = qValue (bandsDemo[b].q);
                    eq.params[(std::size_t) param (b, 4)] = slopeValue (bandsDemo[b].slope);
                    eq.params[(std::size_t) (rangeParam0 + b)] = rangeValue (bandsDemo[b].range);
                }
                eq.params[(std::size_t) phaseParam] = phaseValue (Phase::natural);
                const auto own = [&] (const lynx::EffectSlot& fx, const juce::String& file, bool facetEditor, int page = 0) {
                    lynx::Song song;
                    song.mixer.resize (2);
                    song.mixer[0].name = "Master";
                    song.mixer[1].name = "Vocal";
                    song.mixer[1].effects.push_back (fx);
                    offline.setSong (std::move (song));
                    offline.dispatchPendingMessages();
                    auto editor = facetEditor ? lynx::makeFacetEqEditor (offline, 1, 0, true) : lynx::makeNativeEffectEditor (offline, 1, 0, 1.25f, page);
                    juce::Image image (juce::Image::ARGB, editor->getWidth(), editor->getHeight(), true);
                    {
                        juce::Graphics g (image);
                        g.fillAll (lynx::theme::panel);
                        editor->paintEntireComponent (g, false);
                    }
                    const auto f = out.getChildFile (file);
                    f.deleteFile();
                    juce::PNGImageFormat png;
                    if (auto stream = f.createOutputStream())
                        png.writeImageToStream (image, *stream);
                };
                own (eq, "Facet EQ.png", true);
                own (lynx::makeEffectSlot (lynx::EffectKind::lynxTune), "LYNX Tune.png", false);
                {
                    // round 128: the ADVANCED page, with E bypassed and A removed on the keyboard and a created vibrato
                    auto tune = lynx::makeEffectSlot (lynx::EffectKind::lynxTune);
                    tune.params[(std::size_t) (lynx::lynxtune::bypassC + 4)] = 1.0f;
                    tune.params[(std::size_t) (lynx::lynxtune::noteC + 9)] = 0.0f;
                    tune.params[lynx::lynxtune::vibratoShape] = 1.0f / 3.0f;
                    own (tune, "LYNX Tune-p2.png", false, 1);
                }
                own (lynx::makeEffectSlot (lynx::EffectKind::lynxTune), "LYNX Tune-p3.png", false, 2); // round 135w: Real-time
                lynx::setNativeEditorDemo (false);
            }

            // generators in FL's arrangement: the channel's plugin page with the FL preset's settings
            const auto generators = juce::File (juce::String::fromUTF8 (roots.data.c_str())).getChildFile ("Patches/Plugin presets/Generators");
            for (auto [folder, preset] : std::vector<std::pair<const char*, const char*>> { { "3x Osc", "Default.fst" }, { "FL Keys", "Grand Piano.fst" }, { "BooBass", "" }, { "Sytrus", "Default.fst" }, { "Harmor", "Default.fst" } })
            {
                juce::MemoryBlock data;
                if (juce::String (folder) == "Harmor") // the first preset whose state the engine reads
                {
                    auto files = generators.getChildFile (folder).findChildFiles (juce::File::findFiles, true, "*.fst");
                    files.sort();
                    for (const auto& f : files)
                    {
                        juce::MemoryBlock block;
                        if (! f.loadFileAsData (block))
                            continue;
                        const auto* b = static_cast<const std::uint8_t*> (block.getData());
                        try
                        {
                            const auto p = flp::parseProject (std::vector<std::uint8_t> (b, b + block.getSize()), true);
                            if (! p.channels.empty() && lynx::harmorStateUsable (p.channels.front().pluginState))
                            {
                                preset = nullptr;
                                data = block;
                                break;
                            }
                        }
                        catch (const std::exception&)
                        {
                        }
                    }
                }
                auto file = preset == nullptr ? juce::File() : juce::String (preset).isEmpty() ? database.getSiblingFile ("Generators").getChildFile ("Misc").getChildFile (juce::String (folder) + ".fst")
                                                            : generators.getChildFile (folder).getChildFile (preset);
                if (! file.existsAsFile())
                {
                    auto files = generators.getChildFile (folder).findChildFiles (juce::File::findFiles, true, "*.fst");
                    files.sort();
                    file = files.isEmpty() ? juce::File() : files[0];
                }
                if (data.isEmpty() && ! file.loadFileAsData (data))
                    continue;
                try
                {
                    const auto* bytes = static_cast<const std::uint8_t*> (data.getData());
                    auto project = flp::parseProject (std::vector<std::uint8_t> (bytes, bytes + data.getSize()), true);
                    if (project.channels.empty())
                        continue;
                    project.channels.front().type = flp::ChannelType::native;
                    lynx::FlpImportOptions options;
                    options.roots = roots;
                    options.fileExists = [] (const std::string& path) { return lynx::platform::localFile (path).existsAsFile(); };
                    options.readFile = [] (const std::string& path) {
                        juce::MemoryBlock block;
                        lynx::platform::localFile (path).loadFileAsData (block);
                        const auto* b = static_cast<const std::uint8_t*> (block.getData());
                        return std::vector<std::uint8_t> (b, b + block.getSize());
                    };
                    options.saveEmbedded = [out] (const std::vector<std::uint8_t>& wav, const std::string& name) {
                        const auto f = out.getChildFile ("samples").getNonexistentChildFile (juce::String::fromUTF8 (name.c_str()), ".wav");
                        f.getParentDirectory().createDirectory();
                        f.replaceWithData (wav.data(), wav.size());
                        return f.getFullPathName().toStdString();
                    };
                    auto song = lynx::importFlp (project, options);
                    // the page only needs the plugin's saved state (FL Keys' sample zones need the installed wavetables)
                    if (! song.channels.empty() && project.channels.front().pluginName == "BooBass" && song.channels.front().instrument != lynx::InstrumentKind::boobass)
                    {
                        song.channels.front().instrument = lynx::InstrumentKind::boobass; // (its sound needs the engine DLL; the page does not)
                        song.channels.front().modelParams.assign (3, 32768.0f);
                    }
                    if (! song.channels.empty() && project.channels.front().pluginName == "FL Keys")
                    {
                        song.channels.front().instrument = lynx::InstrumentKind::flkeys;
                        song.channels.front().pluginState = project.channels.front().pluginState;
                    }
                    offline.setSong (std::move (song));
                    offline.dispatchPendingMessages();
                    if (offline.getSong().channels.empty())
                        continue;
                    auto panel = lynx::makeChannelSettingsPanel (offline, 0);
                    juce::Image image (juce::Image::ARGB, panel->getWidth(), panel->getHeight(), true);
                    {
                        juce::Graphics g (image);
                        g.fillAll (lynx::theme::panel);
                        panel->paintEntireComponent (g, false);
                    }
                    const auto out2 = out.getChildFile (juce::String (folder) + ".png");
                    out2.deleteFile();
                    juce::PNGImageFormat png;
                    if (auto stream = out2.createOutputStream())
                        png.writeImageToStream (image, *stream);
                }
                catch (const std::exception&)
                {
                }
            }
        }
        quit();
    }

    void runControllerShot (const juce::String& prefix)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lynx::AudioEngine offline (false);
        lynx::Song song;
        song.mixer = lynx::Song::defaultMixer (2);
        song.mixer[1].name = "Insert 1";
        auto fx = lynx::makeEffectSlot (lynx::EffectKind::formulaController);
        lynx::formula::State formula;
        formula.formula = "a*Sin(SongTime*Pi*b*4)+c";
        fx.state = lynx::formula::writeState (formula);
        fx.params[0] = 0.5f;
        fx.params[1] = 0.25f;
        fx.params[2] = 0.5f;
        song.mixer[1].effects.push_back (fx);
        // round 164: X-Y (slot 2), X-Y-Z (slot 3) and a Control Surface (slot 4)
        {
            auto xyFx = lynx::makeEffectSlot (lynx::EffectKind::xyController);
            xyFx.params[(std::size_t) lynx::xy::level (0)] = 0.3f;
            xyFx.params[(std::size_t) lynx::xy::level (1)] = 0.7f;
            xyFx.params[(std::size_t) lynx::xy::tension (0)] = 0.8f;
            song.mixer[1].effects.push_back (xyFx);
            auto xyzFx = lynx::makeEffectSlot (lynx::EffectKind::xyzController);
            xyzFx.params[(std::size_t) lynx::xy::level (2)] = 0.8f;
            song.mixer[1].effects.push_back (xyzFx);
            auto panel = lynx::makeEffectSlot (lynx::EffectKind::controlSurface);
            lynx::SurfaceControl c;
            for (const auto& [kind, name, v, x, y] : std::vector<std::tuple<lynx::SurfaceControl::Kind, const char*, float, float, float>> {
                     { lynx::SurfaceControl::Kind::knob, "Cutoff", 0.65f, 20.0f, 20.0f },
                     { lynx::SurfaceControl::Kind::knob, "Reso", 0.3f, 90.0f, 20.0f },
                     { lynx::SurfaceControl::Kind::slider, "Mix", 0.8f, 170.0f, 20.0f },
                     { lynx::SurfaceControl::Kind::xy, "Space", 0.4f, 230.0f, 20.0f },
                     { lynx::SurfaceControl::Kind::button, "Drop", 0.0f, 20.0f, 110.0f },
                     { lynx::SurfaceControl::Kind::checkBox, "Bypass", 1.0f, 20.0f, 160.0f } })
            {
                c.kind = kind;
                c.name = name;
                c.value = v;
                c.valueY = 0.6f;
                c.x = x;
                c.y = y;
                lynx::surface::add (panel, c);
            }
            song.mixer[1].effects.push_back (panel);
        }
        lynx::ChannelDef env;
        env.name = "Envelope Controller";
        env.instrument = lynx::InstrumentKind::controller;
        auto ecData = lynx::defaultEnvelopeController();
        ecData[0].lfoOn = true;
        ecData[0].lfoTempo = true;
        ecData[0].lfoSpeed = 40898;
        ecData[0].maps[lynx::mapKeyboard].count = 2;
        ecData[0].maps[lynx::mapKeyboard].time = { 0.2f, 0.8f };
        ecData[0].maps[lynx::mapKeyboard].level = { 0.0f, 1.0f };
        ecData[0].maps[lynx::mapModX] = ecData[0].maps[lynx::mapKeyboard];
        env.controllerState = lynx::writeEnvelopeController (ecData);
        song.channels.push_back (env);
        lynx::ChannelDef out;
        out.name = "MIDI Out";
        out.instrument = lynx::InstrumentKind::midiOut;
        lynx::midictl::MidiOutSettings settings;
        settings.port = 1;
        lynx::midictl::writeMidiOut (out.pluginState, settings);
        song.channels.push_back (out);
        // a Layer (round 164) over three synths split across the keyboard, crossfading
        for (const auto* name : { "Bass", "Pad", "Lead" })
        {
            lynx::ChannelDef c;
            c.name = name;
            c.instrument = lynx::InstrumentKind::synth;
            song.channels.push_back (c);
        }
        {
            lynx::ChannelDef layer;
            layer.name = "Layer";
            layer.instrument = lynx::InstrumentKind::layer;
            song.channels.push_back (layer);
            const auto self = (int) song.channels.size() - 1;
            lynx::layer::setChildren (song, self, { 2, 3, 4 });
            song.channels[2].keyLow = 0, song.channels[2].keyHigh = 47;
            song.channels[3].keyLow = 36, song.channels[3].keyHigh = 83;
            song.channels[4].keyLow = 72, song.channels[4].keyHigh = 127;
            song.channels[(std::size_t) self].layer.crossfade = true;
            song.channels[(std::size_t) self].layer.fade = 0.35f;
        }
        {
            lynx::ChannelDef dash; // channel 6
            dash.name = "Dashboard";
            dash.instrument = lynx::InstrumentKind::dashboard;
            dash.patcher.surface = lynx::surface::starterControls();
            song.channels.push_back (dash);
        }
        {
            lynx::ChannelDef volt; // channel 7: FL's render-test settings (main L pitch, R gate × 0.5) with Out 2 on insert 3
            volt.name = "Voltage Controller";
            volt.instrument = lynx::InstrumentKind::voltage;
            lynx::voltage::Settings v;
            v.enabled = true;
            v.params[9] = 49152;
            v.unipolar[2] = true;
            v.tracks = { 3, 0, 0 };
            v.source[6] = lynx::voltage::Source::clock;
            volt.pluginState = lynx::voltage::write (v);
            song.channels.push_back (volt);
        }
        song.patterns.emplace_back();
        offline.setSong (std::move (song));
        offline.dispatchPendingMessages();
        auto shoot = [&] (std::unique_ptr<juce::Component> c, const juce::String& name) {
            if (c == nullptr)
                return;
            savePng (c->createComponentSnapshot (c->getLocalBounds()), prefix + name);
        };
        shoot (lynx::makeChannelSettingsPanel (offline, 0), "-envelope-controller.png");
        shoot (lynx::makeChannelSettingsPanel (offline, 1), "-midi-out.png");
        shoot (lynx::makeChannelSettingsPanel (offline, 5), "-layer.png");
        shoot (lynx::makeChannelFrame (offline, 3, -1), "-layer-child-misc.png"); // (the key-region ruler over the root keyboard)
        {
            const auto slotOf = [] (int k) -> lynx::SlotLocator {
                return [k] (lynx::Song& s) { return &s.mixer[1].effects[(std::size_t) k]; };
            };
            const lynx::AutomationTarget target { lynx::AutomationTargetKind::effectParam, 1, 1, 0 };
            shoot (lynx::makeXyControllerEditor (offline, slotOf (1), nullptr, target), "-xy-controller.png");
            shoot (lynx::makeXyControllerEditor (offline, slotOf (2), nullptr, target), "-xyz-controller.png");
            shoot (lynx::makeControlSurfaceEditor (offline, slotOf (3), target), "-control-surface.png");
            shoot (lynx::makeChannelSettingsPanel (offline, 6), "-dashboard.png");
            shoot (lynx::makeChannelSettingsPanel (offline, 7), "-voltage-controller.png");
        }
        // FL's Dashboard presets of this PC's FL Studio, imported (round 164f): one PNG each
        if (const auto fl = lynx::findFlStudio(); fl.installDir.isDirectory())
            for (const auto& file : fl.installDir.getChildFile ("Data/Patches/Plugin presets/Generators/Dashboard").findChildFiles (juce::File::findFiles, false, "*.fst"))
            {
                juce::MemoryBlock bytes;
                if (! file.loadFileAsData (bytes))
                    continue;
                const auto preset = flp::parseProject ({ (const std::uint8_t*) bytes.getData(), bytes.getSize() }, true);
                const auto dash = preset.channels.empty() ? std::nullopt : lynx::surface::parseFlDashboard (preset.channels[0].pluginState);
                if (! dash || dash->controls.empty())
                    continue;
                offline.editSong ([&] (lynx::Song& s) {
                    s.channels[6].name = file.getFileNameWithoutExtension().toStdString();
                    s.channels[6].patcher.surface = dash->controls;
                });
                offline.dispatchPendingMessages();
                shoot (lynx::makeChannelSettingsPanel (offline, 6), "-dashboard-fl-" + file.getFileNameWithoutExtension().replaceCharacter (' ', '-') + ".png");
            }
        // the Envelope Controller's pages (issue #7): one PNG per part of the dropdown
        if (auto ec = lynx::makeEnvelopeControllerEditor (offline, 0))
            for (int part = 0; part < 7; ++part)
            {
                for (auto* child : ec->getChildren())
                    if (auto* box = dynamic_cast<juce::ComboBox*> (child))
                        box->setSelectedItemIndex (part, juce::sendNotificationSync);
                savePng (ec->createComponentSnapshot (ec->getLocalBounds()), prefix + "-ec-part" + juce::String (part) + ".png");
            }
        lynx::FormulaControllerWindows::open (offline, 1, 0);
        for (int k = 0; k < juce::TopLevelWindow::getNumTopLevelWindows(); ++k)
            if (auto* w = dynamic_cast<juce::DocumentWindow*> (juce::TopLevelWindow::getTopLevelWindow (k)))
                if (w->getName().startsWith ("Formula") && w->getContentComponent() != nullptr)
                    savePng (w->getContentComponent()->createComponentSnapshot (w->getContentComponent()->getLocalBounds()), prefix + "-formula-controller.png");
        lynx::FormulaControllerWindows::closeAll();
    }

    void runVfxShot (const juce::String& prefix)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lynx::AudioEngine offline (false);
        lynx::MainComponent ui (offline, false);
        ui.setSize (1280, 720);
        auto song = lynx::Song::makeDefault();
        song.channels.clear();
        for (const auto kind : lynx::vfx::allKinds)
        {
            lynx::ChannelDef c;
            lynx::vfx::makeChannel (c, kind);
            if (kind == lynx::vfx::Kind::keyMapper) // a chord on every key: something to see in the grid
                for (const auto& p : lynx::vfx::builtInPresets (kind))
                    if (p.name == "Major triad")
                    {
                        auto st = *lynx::vfx::readState (c.pluginState);
                        st.fl = p.fl;
                        c.pluginState = lynx::vfx::writeState (st);
                    }
            song.channels.push_back (c);
        }
        // a Patcher: its notes through a Color Mapper to three synths
        lynx::ChannelDef patcher;
        patcher.name = "Patcher";
        patcher.instrument = lynx::InstrumentKind::patcher;
        lynx::PatchNode mapper;
        mapper.generator = true;
        lynx::vfx::makeChannel (mapper.channel, lynx::vfx::Kind::colorMapper);
        mapper.x = 260.0f;
        patcher.patcher.nodes.push_back (mapper);
        lynx::PatchWire notes;
        notes.kind = lynx::PatchWire::Kind::midi, notes.to = 0;
        patcher.patcher.wires.push_back (notes);
        for (int k = 0; k < 3; ++k)
        {
            lynx::PatchNode synth;
            synth.generator = true;
            synth.channel.instrument = lynx::InstrumentKind::synth;
            synth.channel.name = "Synth " + std::to_string (k + 1);
            synth.x = 520.0f, synth.y = (float) k * 90.0f;
            patcher.patcher.nodes.push_back (synth);
            lynx::PatchWire in, out;
            in.kind = lynx::PatchWire::Kind::midi, in.from = 0, in.fromPort = k, in.to = 1 + k;
            out.from = 1 + k, out.to = lynx::patchToHost;
            patcher.patcher.wires.push_back (in);
            patcher.patcher.wires.push_back (out);
        }
        patcher.patcher.outputX = 760.0f;
        song.channels.push_back (patcher);
        offline.setSong (std::move (song));
        offline.dispatchPendingMessages();
        for (int c = 0; c < lynx::vfx::kindCount; ++c)
            if (auto panel = lynx::makeChannelSettingsPanel (offline, c))
            {
                offline.dispatchPendingMessages();
                savePng (panel->createComponentSnapshot (panel->getLocalBounds()),
                         prefix + "-" + juce::String (lynx::vfx::flName (*lynx::vfx::kindOf (offline.getSong().channels[(std::size_t) c]))).replaceCharacter (' ', '-').toLowerCase() + ".png");
            }
        // with FL Studio installed: the Sequencer with FL's factory bank (to compare with FL's own editor) and
        // a Script with FL's "Snap To Scale"
        if (const auto roots = lynx::findFlStudioRoots(); ! roots.data.empty())
        {
            const auto data = juce::File (juce::String::fromUTF8 (roots.data.c_str()));
            const std::pair<lynx::vfx::Kind, juce::String> extra[] = {
                { lynx::vfx::Kind::sequencer, "Patches/Plugin database/Effects/Patcher/VFX Sequencer.fst" },
                { lynx::vfx::Kind::script, "Patches/Plugin presets/Effects/VFX Script/Snap To Scale.fst" },
            };
            for (const auto& [kind, path] : extra)
            {
                juce::MemoryBlock bytes;
                if (! data.getChildFile (path).loadFileAsData (bytes))
                    continue;
                const auto* b = static_cast<const std::uint8_t*> (bytes.getData());
                const auto fl = lynx::vfx::stateFromFlPreset (std::vector<std::uint8_t> (b, b + bytes.getSize()), kind);
                int index = -1;
                for (int c = 0; c < lynx::vfx::kindCount; ++c)
                    if (lynx::vfx::kindOf (offline.getSong().channels[(std::size_t) c]) == kind)
                        index = c;
                if (! fl || index < 0)
                    continue;
                offline.editSong ([&] (lynx::Song& s) {
                    auto st = *lynx::vfx::readState (s.channels[(std::size_t) index].pluginState);
                    st.fl = *fl;
                    s.channels[(std::size_t) index].pluginState = lynx::vfx::writeState (st);
                });
                offline.dispatchPendingMessages();
                if (auto panel = lynx::makeChannelSettingsPanel (offline, index))
                {
                    offline.dispatchPendingMessages();
                    savePng (panel->createComponentSnapshot (panel->getLocalBounds()),
                             prefix + "-" + juce::String (lynx::vfx::flName (kind)).replaceCharacter (' ', '-').toLowerCase() + "-fl.png");
                }
            }
        }
        lynx::PatcherWindows::openChannel (offline, lynx::vfx::kindCount);
        for (int k = 0; k < juce::TopLevelWindow::getNumTopLevelWindows(); ++k)
            if (auto* w = dynamic_cast<juce::DocumentWindow*> (juce::TopLevelWindow::getTopLevelWindow (k)))
                if (w->getName().startsWith (lynx::tr ("Patcher")) && w->getContentComponent() != nullptr)
                {
                    auto* content = w->getContentComponent();
                    savePng (content->createComponentSnapshot (content->getLocalBounds()), prefix + "-patcher.png");
                    delete w;
                    break;
                }
        lynx::PatcherWindows::shootMenus (offline, true, lynx::vfx::kindCount, 0, [&] (const juce::Image& image, const juce::String& name) {
            savePng (image, prefix + "-patcher-menu-" + name + ".png");
        });
    }

    void runPatcherShot (const juce::String& prefix, const juce::String& projectPath)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        lynx::AudioEngine offline (false);
        {
            lynx::MainComponent ui (offline, false);
            ui.setSize (1280, 720);
            if (projectPath.isNotEmpty())
                ui.openFile (juce::File (projectPath));
            else
            {
                auto song = lynx::Song::makeDefault();
                song.mixer.resize (4);
                auto slot = lynx::makeEffectSlot (lynx::EffectKind::patcher);
                slot.patcher.wires.clear();
                for (auto [kind, x, y] : { std::tuple { lynx::EffectKind::frequencySplitter, 200.0f, 60.0f }, { lynx::EffectKind::reverb, 420.0f, 0.0f },
                                           { lynx::EffectKind::fruityDelay3, 420.0f, 120.0f }, { lynx::EffectKind::peakController, 200.0f, 260.0f } })
                {
                    lynx::PatchNode n;
                    n.effect = lynx::makeEffectSlot (kind);
                    n.x = x;
                    n.y = y;
                    slot.patcher.nodes.push_back (n);
                }
                auto wire = [&] (int from, int fromPort, int to, int toPort, float gain = 1.0f, lynx::PatchWire::Kind kind = lynx::PatchWire::Kind::audio) {
                    lynx::PatchWire w;
                    w.kind = kind;
                    w.from = from, w.fromPort = fromPort, w.to = to, w.toPort = toPort, w.gain = gain;
                    slot.patcher.wires.push_back (w);
                };
                wire (lynx::patchFromHost, 0, 0, 0);
                wire (0, 1, 1, 0);
                wire (0, 3, 2, 0, 0.5f);
                wire (1, 0, lynx::patchToHost, 0);
                wire (2, 0, lynx::patchToHost, 0);
                wire (lynx::patchFromHost, 1, 3, 0);
                wire (3, 1, 2, 2, 1.0f, lynx::PatchWire::Kind::control);
                wire (0, 2, lynx::patchToHost, 1, 0.7f);
                slot.patcher.outputTracks = { 3 };
                slot.patcher.inputX = -20.0f;
                slot.patcher.outputX = 660.0f;
                song.mixer[1].effects = { slot };
                lynx::ChannelDef ch;
                ch.name = "Patcher";
                ch.instrument = lynx::InstrumentKind::patcher;
                lynx::PatchNode synth, chorus;
                synth.generator = true;
                synth.channel.instrument = lynx::InstrumentKind::synth;
                synth.channel.name = "Synth";
                synth.x = 220.0f;
                chorus.effect = lynx::makeEffectSlot (lynx::EffectKind::chorus);
                chorus.x = 420.0f;
                // round 162: a MIDI Out node between the "MIDI port 5" pin and "To LYNX" MIDI port 3, an Envelope Controller
                // on the notes moving the chorus
                lynx::PatchNode midiOut, envelope;
                midiOut.generator = envelope.generator = true;
                midiOut.channel.instrument = lynx::InstrumentKind::midiOut;
                midiOut.channel.name = "MIDI Out";
                lynx::midictl::writeMidiOut (midiOut.channel.pluginState, { 3, 0, 0, -1, -1 });
                midiOut.x = 220.0f, midiOut.y = 160.0f;
                envelope.channel.instrument = lynx::InstrumentKind::controller;
                envelope.channel.name = "Envelope Controller";
                envelope.channel.controllerState = lynx::writeEnvelopeController (lynx::defaultEnvelopeController());
                envelope.x = 220.0f, envelope.y = 260.0f;
                lynx::PatchNode osc; // round 164f: an FL engine node (its editor opens from the node)
                osc.generator = true;
                osc.channel = lynx::natives::makeChannel (lynx::InstrumentKind::threeosc, lynx::flStudioAccess()).value_or (lynx::ChannelDef {});
                osc.channel.instrument = lynx::InstrumentKind::threeosc;
                osc.channel.name = "3x Osc";
                osc.x = 420.0f, osc.y = 260.0f;
                ch.patcher.nodes = { synth, chorus, midiOut, envelope, osc };
                lynx::PatchWire notes, a, b, portIn, portOut, envNotes, envControl;
                notes.kind = lynx::PatchWire::Kind::midi, notes.to = 0;
                a.from = 0, a.to = 1;
                b.from = 1;
                portIn.kind = lynx::PatchWire::Kind::midi, portIn.fromPort = lynx::patchMidiPortPin (5), portIn.to = 2;
                portOut.kind = lynx::PatchWire::Kind::midi, portOut.from = 2, portOut.to = lynx::patchToHost, portOut.toPort = 3;
                envNotes.kind = lynx::PatchWire::Kind::midi, envNotes.to = 3;
                envControl.kind = lynx::PatchWire::Kind::control, envControl.from = 3, envControl.to = 1, envControl.toPort = 0;
                lynx::PatchWire oscNotes, oscOut;
                oscNotes.kind = lynx::PatchWire::Kind::midi, oscNotes.to = 4;
                oscOut.from = 4;
                ch.patcher.wires = { notes, a, b, portIn, portOut, envNotes, envControl, oscNotes, oscOut };
                ch.patcher.fromMidiPorts = { 1, 5 };
                ch.patcher.toMidiPorts = { 3 };
                ch.patcher.outputX = 640.0f;
                song.channels.push_back (ch);
                // LYNX_PATCHERSHOT_SAVE=file.lynx: the demo as a project (a GUI check of the same Patchers, round 164f)
                if (const auto save = juce::SystemStats::getEnvironmentVariable ("LYNX_PATCHERSHOT_SAVE", {}); save.isNotEmpty())
                {
                    if (! song.patterns.empty())
                        for (int k = 0; k < 4; ++k)
                            song.patterns[0].notes.push_back ({ (std::int64_t) k * lynx::ppq, lynx::ppq / 2, (int) song.channels.size() - 1, 60 + 4 * k, 0.8f });
                    juce::String error;
                    lynx::ProjectFile::save (song, juce::File (save), error);
                }
                offline.setSong (std::move (song));
            }
            offline.dispatchPendingMessages();
            const auto& song = offline.getSong();
            auto shootOpen = [&] (const juce::String& name) {
                for (int k = 0; k < juce::TopLevelWindow::getNumTopLevelWindows(); ++k)
                    if (auto* w = dynamic_cast<juce::DocumentWindow*> (juce::TopLevelWindow::getTopLevelWindow (k)))
                        if (w->getName().startsWith (lynx::tr ("Patcher")) && w->getContentComponent() != nullptr)
                        {
                            auto* c = w->getContentComponent();
                            savePng (c->createComponentSnapshot (c->getLocalBounds()), prefix + name);
                            delete w;
                            return;
                        }
            };
            int shots = 0;
            for (int t = 0; t < (int) song.mixer.size() && shots < 12; ++t)
                for (int s = 0; s < (int) song.mixer[(std::size_t) t].effects.size(); ++s)
                    if (song.mixer[(std::size_t) t].effects[(std::size_t) s].kind == lynx::EffectKind::patcher)
                    {
                        lynx::PatcherWindows::openSlot (offline, t, s);
                        shootOpen ("-insert" + juce::String (t) + "-slot" + juce::String (s) + ".png");
                        ++shots;
                        if (shots == 1) // its menus (round 162)
                            lynx::PatcherWindows::shootMenus (offline, false, t, s, [&] (const juce::Image& image, const juce::String& name) {
                                savePng (image, prefix + "-menu-" + name + ".png");
                            });
                    }
            for (int c = 0; c < (int) song.channels.size(); ++c)
                if (song.channels[(std::size_t) c].instrument == lynx::InstrumentKind::patcher)
                {
                    lynx::PatcherWindows::openChannel (offline, c);
                    shootOpen ("-channel" + juce::String (c) + ".png");
                    // each built-in generator node's editor, as a double-click opens it (round 164f)
                    for (int n = 0; n < (int) song.channels[(std::size_t) c].patcher.nodes.size(); ++n)
                        if (const auto& node = song.channels[(std::size_t) c].patcher.nodes[(std::size_t) n]; node.generator && lynx::hasNodeEditor (node.channel))
                            if (auto frame = lynx::makeNodeChannelFrame (offline, [c, n] (lynx::Song& sg) -> lynx::ChannelDef* {
                                    return c < (int) sg.channels.size() && n < (int) sg.channels[(std::size_t) c].patcher.nodes.size()
                                               ? &sg.channels[(std::size_t) c].patcher.nodes[(std::size_t) n].channel
                                               : nullptr;
                                }))
                            {
                                offline.dispatchPendingMessages();
                                savePng (frame->createComponentSnapshot (frame->getLocalBounds()), prefix + "-channel" + juce::String (c) + "-node" + juce::String (n) + ".png");
                            }
                    lynx::PatcherWindows::shootMenus (offline, true, c, 0, [&] (const juce::Image& image, const juce::String& name) {
                        savePng (image, prefix + "-channel" + juce::String (c) + "-menu-" + name + ".png");
                    });
                }
        }
    }

    void runSnapshot (const juce::String& prefix, const juce::String& projectPath)
    {
        juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);
        // LYNX_SNAPSHOT_THEME=file.lynxtheme: the pictures in that theme (round 162d)
        if (const auto themeFile = juce::SystemStats::getEnvironmentVariable ("LYNX_SNAPSHOT_THEME", {}); themeFile.isNotEmpty())
            if (const auto spec = lynx::themeui::loadFile (juce::File (themeFile)))
                lynx::themeui::apply (*spec, false);
        {
            lynx::AudioEngine offline (false);
            lynx::MainComponent ui (offline, false); // default layout and keys, nothing saved
            // LYNX_SNAPSHOT_SIZE=WxH (default 1280x720)
            const auto size = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("LYNX_SNAPSHOT_SIZE", "1280x720"), "x", "");
            ui.setSize (juce::jlimit (800, 3840, size[0].getIntValue()), juce::jlimit (450, 2160, size[1].getIntValue()));

            if (projectPath.isNotEmpty())
                ui.openFile (juce::File (projectPath));

            offline.dispatchPendingMessages(); // change notifications are asynchronous

            ui.waitForBrowserForSnapshot();

            // the workspace desktop: the default layout, then with the Piano Roll and Mixer floating too
            // (and a sample hint in the status strip, as when hovering a knob)
            ui.showDesktopForSnapshot (false);
            ui.setMetersForSnapshot (0.23, (std::int64_t) 412 << 20, 6);
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-desktop.png");

            // the first start: the setup wizard over the main window (round 140), also on a small screen (it scrolls)
            {
                const auto size = ui.getBounds();
                ui.showSetupWizard();
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-desktop-setup.png");
                ui.setSize (1024, 600);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-desktop-setup-small.png");
                ui.closeSetupWizardForSnapshot();
                ui.setBounds (size);
                offline.dispatchPendingMessages();
            }

            // the start-up desktop and the toolbar at the usual window sizes: the untouched default layout
            // follows the window (default rects, not scaled ones); panels drop from the toolbar by priority
            {
                const auto size = ui.getBounds();
                for (const auto& [w, h] : std::vector<std::pair<int, int>> { { 1280, 720 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 } })
                {
                    ui.setSize (w, h);
                    offline.dispatchPendingMessages();
                    saveSnapshot (ui, prefix + "-desktop-" + juce::String (w) + ".png");
                    saveArea (ui, ui.getToolbarBoundsForSnapshot(), prefix + "-toolbar-" + juce::String (w) + ".png");

                    if (w == 1920) // the rack scrolled to its last steps: the controls column stays in view (round 90)
                    {
                        ui.scrollRackForSnapshot (100000);
                        offline.dispatchPendingMessages();
                        saveSnapshot (ui, prefix + "-desktop-rack-scrolled.png");
                        ui.scrollRackForSnapshot (0);
                    }
                }
                // the toolbar with small and large icons, and a customized one (groups hidden and reordered)
                ui.setSize (1600, 900);
                for (auto [iconSize, name] : { std::pair { lynx::toolbarlayout::IconSize::small, "small" }, { lynx::toolbarlayout::IconSize::large, "large" } })
                {
                    auto layout = lynx::toolbarlayout::Layout::makeDefault();
                    layout.iconSize = iconSize;
                    ui.setToolbarLayoutForSnapshot (layout);
                    saveArea (ui, ui.getToolbarBoundsForSnapshot(), prefix + "-toolbar-" + name + "-1600.png");
                }
                {
                    auto layout = lynx::toolbarlayout::Layout::makeDefault();
                    layout.move (lynx::toolbarlayout::Group::windows, -3);
                    layout.setShown (lynx::toolbarlayout::Group::record, false);
                    layout.setShown (lynx::toolbarlayout::Group::meters, false);
                    ui.setToolbarLayoutForSnapshot (layout);
                    saveArea (ui, ui.getToolbarBoundsForSnapshot(), prefix + "-toolbar-custom-1600.png");
                }
                {
                    // the toolbar's edit mode (round 115): outlined panels and the tray of panels under it
                    ui.setToolbarLayoutForSnapshot (lynx::toolbarlayout::Layout::makeDefault());
                    auto layout = lynx::toolbarlayout::Layout::makeDefault();
                    layout.setShown (lynx::toolbarlayout::Group::meters, false);
                    ui.setToolbarLayoutForSnapshot (layout);
                    ui.setToolbarEditingForSnapshot (true);
                    offline.dispatchPendingMessages();
                    saveArea (ui, ui.getToolbarBoundsForSnapshot().withHeight (ui.getToolbarAndTrayHeightForSnapshot()), prefix + "-toolbar-edit-1600.png");
                    ui.setToolbarEditingForSnapshot (false);
                    // Ctrl+F11: only the menu and the hint bar
                    layout = lynx::toolbarlayout::Layout::makeDefault();
                    layout.visible = false;
                    ui.setToolbarLayoutForSnapshot (layout);
                    saveArea (ui, ui.getLocalBounds().withHeight (120), prefix + "-toolbar-hidden-1600.png");
                }
                ui.setToolbarLayoutForSnapshot (lynx::toolbarlayout::Layout::makeDefault());
                // quick tips: the toolbar and title bars with a few name bubbles, and every tip on one sheet
                offline.dispatchPendingMessages();
                saveQuickTips (ui, prefix);
                ui.setBounds (size);
                offline.dispatchPendingMessages();
            }

            // the hint bar: a value control while it is dragged (live value), a gesture line with mouse and key
            // glyphs, and a text too long for one line (two lines)
            {
                const auto toolbar = ui.getToolbarBoundsForSnapshot().withWidth (juce::jmin (ui.getWidth(), 900));
                const lynx::ui::Hint hints[] = {
                    { lynx::tr ("Volume") + " - " + lynx::tr ("drag (Ctrl: fine), wheel, Alt+click resets"), "-3.2 dB", true, true },
                    { lynx::tr ("Piano Roll: [LMB] add a note, drag to move  [RMB] delete  [Ctrl][LMB] drag to select  [Ctrl][Wheel] zoom"), {}, false, false },
                    { lynx::tr ("Global snap: the grid of the Piano Roll and Playlist (when they follow it) and the quantise of recorded notes  "
                                "[LMB] choose  [Wheel] step  [RMB] length of new notes  [Backspace] snap on/off  [Alt] while dragging: no snap"), {}, false, true },
                };
                for (int i = 0; i < 3; ++i)
                {
                    ui.setHintForSnapshot (hints[i], i == 0);
                    saveArea (ui, toolbar, prefix + "-hint-" + juce::String (i + 1) + ".png");
                }
            }

            ui.showDesktopForSnapshot (true);
            ui.setHintForSnapshot ({ lynx::tr ("Volume") + " - " + lynx::tr ("drag (Ctrl: fine), wheel, Alt+click resets"), "78%", true, true });
            saveSnapshot (ui, prefix + "-desktop-all.png");

            ui.selectTabForSnapshot (0);
            saveSnapshot (ui, prefix + "-rack.png");
            {
                // a pattern with piano-roll notes: those rows show a miniature instead of steps (round 90)
                int found = -1;
                const auto before = offline.getSong().currentPattern;
                {
                    const auto& song = offline.getSong();
                    for (int p = 0; p < (int) song.patterns.size() && found < 0; ++p)
                        for (int c = 0; c < (int) song.channels.size() && found < 0; ++c)
                            if (lynx::showsNotePreview (song.patterns[(std::size_t) p], c))
                                found = p;
                }
                if (found >= 0)
                {
                    offline.editSong ([found] (lynx::Song& s) { s.currentPattern = found; }, false);
                    offline.dispatchPendingMessages();
                    saveSnapshot (ui, prefix + "-rack-preview.png");
                    offline.editSong ([before] (lynx::Song& s) { s.currentPattern = before; }, false);
                    offline.dispatchPendingMessages();
                }
            }
            ui.selectTabForSnapshot (1);
            saveSnapshot (ui, prefix + "-pianoroll.png");
            if (const auto strip = ui.getToolStripAreaForSnapshot (false); ! strip.isEmpty())
                saveArea (ui, strip, prefix + "-pianoroll-tools.png");
            // the editors' mouse feel: the hovered note / clip edge (grip, outline) and its hint in the hint bar
            ui.setHintForSnapshot (ui.getPianoRollForSnapshot().hoverForSnapshot());
            saveSnapshot (ui, prefix + "-pianoroll-hover.png");
            ui.selectTabForSnapshot (2);
            saveSnapshot (ui, prefix + "-playlist.png");
            if (const auto strip = ui.getToolStripAreaForSnapshot (true); ! strip.isEmpty())
                saveArea (ui, strip, prefix + "-playlist-tools.png");
            ui.setHintForSnapshot (ui.getPlaylistForSnapshot().hoverForSnapshot());
            saveSnapshot (ui, prefix + "-playlist-hover.png");
            ui.setHintForSnapshot ({ lynx::tr ("Volume") + " - " + lynx::tr ("drag (Ctrl: fine), wheel, Alt+click resets"), "78%", true, true });
            // the Playlist with a time selection over bars 2-3 (it selects the clips it touches) and the Knife tool
            ui.getPlaylistForSnapshot().setTimeSelectionForSnapshot (1, 3);
            ui.getPlaylistForSnapshot().setToolForSnapshot (lynx::EditTool::knife);
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-playlist-timesel.png");
            ui.getPlaylistForSnapshot().setToolForSnapshot (lynx::EditTool::pencil);
            // round 104: a folded track group (FL's thin member rows) and the Picker's audio tab
            {
                int header = -1;
                for (int t = 0; t < (int) offline.getSong().playlistTracks.size() && header < 0; ++t)
                    if (lynx::noteedit::isGroupHeader (offline.getSong(), t))
                        header = t;
                if (header >= 0)
                {
                    offline.editSong ([header] (lynx::Song& s) { lynx::noteedit::setGroupCollapsed (s, header, true); }, false);
                    offline.dispatchPendingMessages();
                    saveSnapshot (ui, prefix + "-playlist-group-folded.png");
                    offline.editSong ([header] (lynx::Song& s) { lynx::noteedit::setGroupCollapsed (s, header, false); }, false);
                }
                ui.getPickerForSnapshot().setView (1);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-playlist-picker-audio.png");
                ui.getPickerForSnapshot().setView (0);
            }
            // round 104: the Channel Rack filtered to its first filter group
            if (! offline.getSong().channelGroups.empty())
            {
                ui.selectTabForSnapshot (0);
                ui.getRackForSnapshot().setFilterGroup (0);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-rack-filter.png");
                ui.getRackForSnapshot().setFilterGroup (-1);
            }
            ui.selectTabForSnapshot (3);
            // LYNX_SNAPSHOT_TRACK=n: shows the effects of that mixer track
            if (const auto track = juce::SystemStats::getEnvironmentVariable ("LYNX_SNAPSHOT_TRACK", {}); track.isNotEmpty())
            {
                ui.selectMixerTrackForSnapshot (track.getIntValue(),
                                                juce::SystemStats::getEnvironmentVariable ("LYNX_SNAPSHOT_SLOT", "-1").getIntValue());
                offline.dispatchPendingMessages();
            }
            saveSnapshot (ui, prefix + "-mixer.png");
            // round 163: the Mixer at short heights (UI zoom 150%+): the EQ folds, then the effects panel scrolls
            {
                const auto size = ui.getBounds();
                for (auto h : { 450, 560, 680, 820 })
                {
                    ui.setSize (size.getWidth(), h);
                    offline.dispatchPendingMessages();
                    saveSnapshot (ui, prefix + "-mixer163-" + juce::String (h) + ".png");
                }
                ui.setBounds (size);
                offline.dispatchPendingMessages();
            }

            // standalone windows, with the theme background
            auto saveWindow = [&prefix] (std::unique_ptr<juce::Component> c, const juce::String& name) {
                juce::Component frame;
                frame.setSize (c->getWidth(), c->getHeight());
                frame.setOpaque (true);
                frame.addAndMakeVisible (*c);
                c->setTopLeftPosition (0, 0);
                juce::Image image (juce::Image::ARGB, frame.getWidth(), frame.getHeight(), true);
                {
                    juce::Graphics g (image);
                    g.fillAll (lynx::theme::panel);
                    frame.paintEntireComponent (g, false);
                }
                const juce::File file (prefix + name);
                file.deleteFile();
                juce::PNGImageFormat png;
                if (auto out = file.createOutputStream())
                    png.writeImageToStream (image, *out);
            };

            saveWindow (lynx::makeExportPanel (offline), "-export.png");
            saveWindow (lynx::makeExportPanel (offline, 1), "-export-mp3.png");
            saveWindow (lynx::makeExportPanel (offline, 3), "-export-flac-stems.png");
            saveSplashFrames (prefix);
            saveIconSheet (prefix + "-icons.png", 1.0f);
            saveIconSheet (prefix + "-icons-150.png", 1.5f);
            saveWindow (lynx::MainComponent::makeHelpContent(), "-help.png");
            saveWindow (std::make_unique<lynx::toolbar::CustomizePanel> (lynx::toolbarlayout::Layout::makeDefault()), "-customize-toolbar.png");
            // two moved controls: Tools › Last tweaked and Add › Automation for last tweaked parameter show them (REA round 143)
            offline.noteTweaked ({ lynx::AutomationTargetKind::channelVolume, 0 });
            offline.noteTweaked ({ lynx::AutomationTargetKind::mixerVolume, 0 });
            saveMenus (ui, prefix + "-menus.png");
            {
                // the windows' own menus (title bars) and the toolbar's recording menu
                juce::StringArray names;
                std::vector<juce::PopupMenu> menus;
                for (auto [panel, name] : { std::pair { lynx::workspace::Panel::playlist, "Playlist" }, { lynx::workspace::Panel::channelRack, "Channel Rack" },
                                            { lynx::workspace::Panel::pianoRoll, "Piano Roll" }, { lynx::workspace::Panel::mixer, "Mixer" },
                                            { lynx::workspace::Panel::browser, "Browser" } })
                {
                    names.add (name);
                    menus.push_back (ui.getPanelMenuForSnapshot (panel));
                }
                names.add ("Recording");
                menus.push_back (ui.getRecordMenuForSnapshot());
                names.add ("Toolbar");
                menus.push_back (ui.getToolbarMenuForSnapshot());
                saveMenus (names, menus, prefix + "-panel-menus.png");
            }
            {
                // REA round 143: the piano roll Tools menu, a Playlist track menu and a mixer insert menu (FL's layouts)
                std::vector<juce::PopupMenu> menus { ui.getPianoRollForSnapshot().getToolsMenuForSnapshot(),
                                                     ui.getPlaylistForSnapshot().getTrackMenuForSnapshot (0),
                                                     ui.getMixerForSnapshot().getStripsForSnapshot().buildMenu (1) };
                saveMenus (juce::StringArray { "Piano roll tools", "Playlist track", "Mixer track" }, menus, prefix + "-menus143.png");
                // round 163: menu density, compact (default) and comfortable, on a Browser folder menu
                {
                    juce::PopupMenu folder;
                    folder.addSectionHeader ("Documents");
                    folder.addItem (1, "Expand all");
                    folder.addItem (2, "Collapse all");
                    folder.addSeparator();
                    folder.addItem (3, "Refresh");
                    folder.addSectionHeader ("System");
                    folder.addItem (4, "Open");
                    folder.addItem (5, "Copy the file path");
                    folder.addSectionHeader ("Tags");
                    folder.addItem (6, "Starred", true, true);
                    folder.addSeparator();
                    folder.addItem (7, "Add folder...");
                    const auto wasCompact = lynx::theme::menus::compact();
                    lynx::theme::menus::setCompact (true);
                    saveMenus (juce::StringArray { "Compact" }, { folder }, prefix + "-menu163-compact.png");
                    lynx::theme::menus::setCompact (false);
                    saveMenus (juce::StringArray { "Comfortable" }, { folder }, prefix + "-menu163-comfortable.png");
                    lynx::theme::menus::setCompact (wasCompact);
                }
                // and the clip menus of the first pattern, audio and automation clips of the project
                juce::StringArray clipNames;
                std::vector<juce::PopupMenu> clipMenus;
                for (auto [kind, name] : { std::pair { lynx::ClipKind::pattern, "Pattern clip" }, { lynx::ClipKind::audio, "Audio clip" },
                                           { lynx::ClipKind::automation, "Automation clip" } })
                    for (int c = 0; c < (int) offline.getSong().playlist.size(); ++c)
                        if (offline.getSong().playlist[(std::size_t) c].kind == kind)
                        {
                            clipNames.add (name);
                            clipMenus.push_back (ui.getPlaylistForSnapshot().getClipMenuForSnapshot (c));
                            break;
                        }
                if (! clipMenus.empty())
                    saveMenus (clipNames, clipMenus, prefix + "-menus143-clips.png");
            }

            auto saveAlert = [&] (std::unique_ptr<juce::AlertWindow> alert, const juce::String& name) {
                alert->setOpaque (true);
                saveWindow (std::move (alert), name);
            };
            saveAlert (ui.makeConfirmDiscardWindow(), "-confirm.png");
            saveAlert (lynx::MainComponent::makeAboutWindow(), "-about.png");
            saveAlert (lynx::makeNameAndColourDialog (lynx::tr ("Rename pattern"), "Pattern 2", lynx::lynxPalette()[6]), "-rename.png");
            saveAlert (lynx::makeRecordChoiceDialog(), "-record-dialog.png");
            saveAlert (lynx::makeSamplePitchDialog (7.25f, true, true), "-sample-pitch.png"); // round 163c
            saveAlert (lynx::makeMidiImportDialog(), "-midi-import-dialog.png"); // File › Import › MIDI file (REA round 143)
            saveWindow (lynx::makeSetupWizardContent (offline, {}, {}), "-setup.png");
            saveWindow (lynx::crashreporter::makeDialogForSnapshot (false), "-crash-report.png"); // round 159
            saveWindow (lynx::crashreporter::makeDialogForSnapshot (true), "-report-problem.png");
            saveWindow (lynx::makeMissingFilesPanel (lynx::missingFilesSnapshotInfo(), 0, 1), "-missing-files.png");
            saveWindow (lynx::makeMissingFilesPanel (lynx::missingFilesSnapshotInfo(), 1), "-missing-plugins.png");
            saveWindow (lynx::makeMissingFilesPanel (lynx::missingFilesSnapshotInfo(), 2), "-missing-found.png");
            {
                // the Settings pages, with credits like a project imported from FL would carry
                offline.editSong ([] (lynx::Song& s) {
                    s.info = { "Night Drive", "LYNX demo", "Synthwave", "https://lynx-share.com",
                               "Made with LYNX.\nDrums and bass from the Channel Rack, pads from the Browser.", false };
                }, false);
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::project), "-settings-project.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::info), "-settings-info.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::files), "-settings-files.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::general), "-settings-general.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::theme), "-settings-theme.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::audio), "-settings-audio.png");
                saveWindow (lynx::makeSettingsPanel (offline, lynx::SettingsPage::midi), "-settings-midi.png");
                saveWindow (lynx::makePluginManagerPanel (offline, false), "-plugin-manager.png");
                saveWindow (lynx::makeShortcutManagerPanel (false), "-shortcuts.png");
            }
            {
                // round 84: New project, Clip properties, Arrangements, Plugin performance monitor, Touch keyboard
                saveWindow (lynx::makeNewProjectPanel(), "-new-project.png");
                const auto& clips = offline.getSong().playlist;
                for (int i = 0; i < (int) clips.size(); ++i)
                    if (clips[(std::size_t) i].kind == lynx::ClipKind::pattern)
                    {
                        saveWindow (lynx::makeClipPropertiesPanel (offline, i, { i, (i + 1) % (int) clips.size() }), "-clip-properties.png");
                        break;
                    }
                for (int i = 0; i < (int) clips.size(); ++i)
                    if (clips[(std::size_t) i].kind == lynx::ClipKind::audio)
                    {
                        saveWindow (lynx::makeClipPropertiesPanel (offline, i, { i }), "-clip-properties-audio.png");
                        break;
                    }
                if (offline.getSong().arrangements.size() < 2)
                    offline.editSong ([] (lynx::Song& s) {
                        const auto current = s.currentArrangement;
                        s.addArrangement ("Radio edit", true);
                        s.addArrangement ("Intro loop");
                        s.selectArrangement (current);
                    }, false);
                saveWindow (lynx::makeArrangementsPanel (offline), "-arrangements.png");
                saveWindow (lynx::makePluginMonitorPanel (offline), "-plugin-monitor.png");
                saveWindow (lynx::makeTouchKeyboardPanel (offline), "-touch-keyboard.png");
            }
            {
                // round 142: the Newtone / Newtime editors on a synthetic sung line (A3 B3 C4 · E4), one note moved
                const auto wav = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lynx-snapshot-newtone.wav");
                {
                    juce::AudioBuffer<float> line (1, 44100 * 2);
                    const double hz[] = { 220.0, 246.94, 261.63, 0.0, 329.63 };
                    double ph = 0.0;
                    for (int i = 0; i < line.getNumSamples(); ++i)
                    {
                        const auto f0 = hz[std::min (4, i / 17640)];
                        ph += 2.0 * juce::MathConstants<double>::pi * f0 * (1.0 + 0.004 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * i / 44100.0)) / 44100.0;
                        float v = 0.0f;
                        for (int h = 1; f0 > 0.0 && h < 8; ++h)
                            v += (float) (0.3 / h * std::sin (h * ph));
                        line.setSample (0, i, v);
                    }
                    wav.deleteFile();
                    juce::WavAudioFormat format;
                    std::unique_ptr<juce::OutputStream> out (wav.createOutputStream().release());
                    if (auto* w = out != nullptr ? format.createWriterFor (out.get(), 44100.0, 1, 16, {}, 0) : nullptr)
                    {
                        out.release();
                        std::unique_ptr<juce::AudioFormatWriter> writer (w);
                        writer->writeFromAudioSampleBuffer (line, 0, line.getNumSamples());
                    }
                }
                if (const auto sample = offline.getSampleLibrary().load (wav.getFullPathName()))
                {
                    auto notes = lynx::newtonefx::analyseNotes (*sample);
                    if (notes.pitches.size() > 1)
                        notes.pitches[1].shift = 2.0;
                    lynx::newtonefx::WarpMap warp;
                    warp.frames = (std::int64_t) sample->frames();
                    warp.markers = { { 22050, 33075 } };
                    int at = 0;
                    offline.editSong ([&] (lynx::Song& song) {
                        if (song.mixer.size() < 2)
                            song.mixer.resize (2);
                        auto& fx = song.mixer[1].effects;
                        auto tone = lynx::makeEffectSlot (lynx::EffectKind::newtone);
                        tone.samplePath = wav.getFullPathName().toStdString();
                        tone.state = lynx::newtonefx::writeNative (notes);
                        auto time = lynx::makeEffectSlot (lynx::EffectKind::newtime);
                        time.samplePath = tone.samplePath;
                        time.state = lynx::newtonefx::writeWarp (warp);
                        at = (int) fx.size();
                        fx.push_back (tone);
                        fx.push_back (time);
                    }, false);
                    saveWindow (lynx::makeNewtoneEditor (offline, 1, at), "-newtone.png");
                    saveWindow (lynx::makeNewtoneEditor (offline, 1, at + 1), "-newtime.png");
                    offline.editSong ([at] (lynx::Song& song) {
                        auto& fx = song.mixer[1].effects;
                        fx.erase (fx.begin() + at, fx.begin() + at + 2);
                    }, false);
                }
            }

            if (! offline.getSong().automations.empty())
            {
                auto editor = lynx::makeAutomationEditor (offline, 0);
                editor->setSize (760, 300);
                saveWindow (std::move (editor), "-automation.png");
            }

            saveWindow (lynx::makeChannelSettingsPanel (offline, 0), "-sampler.png");
            // the channel window frame (REA round 143b): header strip + tabs; the Misc tab of the first channel and of a synth
            saveWindow (lynx::makeChannelFrame (offline, 0), "-channel-frame.png");
            saveWindow (lynx::makeChannelFrame (offline, 0, -1), "-channel-misc.png");

            for (int c = 0; c < (int) offline.getSong().channels.size(); ++c)
                if (offline.getSong().channels[(std::size_t) c].instrument == lynx::InstrumentKind::synth)
                {
                    saveWindow (lynx::makeChannelSettingsPanel (offline, c), "-synth.png");
                    saveWindow (lynx::makeChannelModulationPanel (offline, c), "-modulation.png");
                    break;
                }

            for (int c = 0; c < (int) offline.getSong().channels.size(); ++c)
                if (offline.getSong().channels[(std::size_t) c].instrument == lynx::InstrumentKind::controller)
                {
                    saveWindow (lynx::makeChannelSettingsPanel (offline, c), "-controller.png");
                    break;
                }

            for (const auto kind : { lynx::InstrumentKind::sakura, lynx::InstrumentKind::drumpad })
                for (int c = 0; c < (int) offline.getSong().channels.size(); ++c)
                    if (offline.getSong().channels[(std::size_t) c].instrument == kind)
                    {
                        saveWindow (lynx::makeChannelSettingsPanel (offline, c), kind == lynx::InstrumentKind::sakura ? "-sakura.png" : "-drumpad.png");
                        break;
                    }

            // parametric EQ: the one with the most pronounced curve (in an imported .flp, with FL's settings)
            {
                int bestTrack = -1, bestSlot = -1;
                double bestShape = -1.0;
                const auto& mixer = offline.getSong().mixer;

                for (int t = 0; t < (int) mixer.size(); ++t)
                    for (int s = 0; s < (int) mixer[(std::size_t) t].effects.size(); ++s)
                        if (const auto& fx = mixer[(std::size_t) t].effects[(std::size_t) s]; fx.kind == lynx::EffectKind::parametricEq)
                        {
                            double shape = 0.0;
                            for (double hz = 30.0; hz < 18000.0; hz *= 1.5)
                                shape += std::abs (lynx::peq::responseDb (fx.params, 48000.0, hz));
                            if (shape > bestShape)
                            {
                                bestShape = shape;
                                bestTrack = t;
                                bestSlot = s;
                            }
                        }

                if (bestTrack >= 0)
                    saveWindow (lynx::makeParametricEqEditor (offline, bestTrack, bestSlot), "-peq.png");
            }

            // Note inspector for the Piano Roll's notes (its selection, or all of the channel's notes)
            if (auto inspector = ui.getPianoRollForSnapshot().makeNoteInspectorForSnapshot())
                saveWindow (std::move (inspector), "-noteinspector.png");
            if (auto inspector = ui.getPianoRollForSnapshot().makeNoteInspectorForSnapshot (true))
                saveWindow (std::move (inspector), "-noteinspector-one.png");

            // workflow P2: note tool dialogs (they preview on the pattern; each one puts the notes back when destroyed)
            {
                auto& roll = ui.getPianoRollForSnapshot();
                const std::pair<lynx::NoteTool, const char*> tools[] = { { lynx::NoteTool::strum, "-tool-strum.png" },
                                                                         { lynx::NoteTool::arpeggiate, "-tool-arpeggiate.png" },
                                                                         { lynx::NoteTool::flip, "-tool-flip.png" },
                                                                         { lynx::NoteTool::randomise, "-tool-randomise.png" },
                                                                         { lynx::NoteTool::scaleLevels, "-tool-scalelevels.png" },
                                                                         { lynx::NoteTool::limit, "-tool-limit.png" },
                                                                         { lynx::NoteTool::quantize, "-tool-quantize.png" },
                                                                         { lynx::NoteTool::chop, "-tool-chop.png" },
                                                                         { lynx::NoteTool::flam, "-tool-flam.png" },
                                                                         { lynx::NoteTool::lfo, "-tool-lfo.png" },
                                                                         { lynx::NoteTool::claw, "-tool-claw.png" },
                                                                         { lynx::NoteTool::articulate, "-tool-articulate.png" },
                                                                         { lynx::NoteTool::riffMachine, "-tool-riff.png" },
                                                                         { lynx::NoteTool::chordProgression, "-tool-chords.png" } };
                for (const auto& [tool, name] : tools)
                    saveWindow (lynx::makeNoteToolPanel (offline, tool, roll.noteIndicesForSnapshot(), roll.gridForSnapshot()), name);
            }

            // round 83: the event editor, with sample events on the selected channel's volume (removed afterwards)
            {
                const lynx::AutomationTarget volume { lynx::AutomationTargetKind::channelVolume, offline.getSelectedChannel() };
                const auto before = offline.getSong().patterns[(std::size_t) offline.getSong().currentPattern].events;
                offline.editSong ([&] (lynx::Song& s) {
                    auto& lane = lynx::eventedit::laneFor (s.patterns[(std::size_t) s.currentPattern], volume);
                    lynx::eventedit::line (lane, 0, lynx::ppq * 2, 0.2f, 0.9f, lynx::ticksPerStep);
                    lynx::prtools::LfoOptions lfo;
                    lfo.startSpeed = 7 * 4096; // one beat
                    lynx::eventedit::applyLfo (lane, lynx::ppq * 2, lynx::ppq * 4, lfo, lynx::ticksPerStep);
                }, false);
                saveWindow (lynx::makeEventEditor (offline, volume), "-eventeditor.png");
                offline.editSong ([&] (lynx::Song& s) { s.patterns[(std::size_t) s.currentPattern].events = before; }, false);
            }

            // round 86: the audio editor with a synthetic drum loop (markers, a region, a loop), waveform and spectrogram,
            // and its menu items (-audioeditor-menu.txt)
            {
                namespace ae = lynx::audioedit;
                ae::Document d;
                d.title = "Drum loop";
                d.audio.sampleRate = 44100.0;
                const auto frames = (std::size_t) (44100 * 2);
                d.audio.channels.assign (2, std::vector<float> (frames, 0.0f));
                std::uint32_t seed = 7;
                for (int hit = 0; hit < 8; ++hit)
                {
                    const auto at = (std::size_t) hit * frames / 8;
                    const auto kick = hit % 2 == 0;
                    double phase = 0.0;
                    for (std::size_t i = 0; i < 12000 && at + i < frames; ++i)
                    {
                        seed = seed * 1664525u + 1013904223u;
                        const auto noise = (float) (seed >> 8) / 16777216.0f - 0.5f;
                        const auto t = (double) i / 44100.0;
                        phase += 2.0 * 3.141592653589793 * (kick ? 45.0 + 160.0 * std::exp (-t * 30.0) : 190.0) / 44100.0;
                        const auto body = (float) std::sin (phase) * (float) std::exp (-t * (kick ? 9.0 : 22.0));
                        const auto v = kick ? 0.85f * body : 0.35f * body + 0.6f * noise * (float) std::exp (-t * 18.0);
                        d.audio.channels[0][at + i] += v;
                        d.audio.channels[1][at + i] += kick ? v : v * 0.8f + 0.1f * noise * (float) std::exp (-t * 30.0);
                    }
                }
                ae::addMarker (d, (std::int64_t) frames / 8, "Snare");
                ae::addRegion (d, { (std::int64_t) frames / 2, (std::int64_t) frames * 3 / 4 }, "Fill");
                ae::setLoop (d, { 0, (std::int64_t) frames });
                auto wave = lynx::makeAudioEditorForSnapshot (offline, d, false);
                juce::File (prefix + "-audioeditor-menu.txt").replaceWithText (lynx::audioEditorMenuForSnapshot (*wave).joinIntoString ("\n") + "\n");
                saveWindow (std::move (wave), "-audioeditor.png");
                saveWindow (lynx::makeAudioEditorForSnapshot (offline, d, true), "-audioeditor-spectrum.png");
            }

            // the tempo tapper after five steady taps (round 132)
            saveWindow (lynx::makeTempoTapperForSnapshot (offline, 5, 128.0), "-tempotapper.png");

            // the Channel Rack's piano-roll view (round 132)
            ui.selectTabForSnapshot (0);
            ui.getRackForSnapshot().setPianoRollView (true);
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-rack-pianoroll-view.png");
            ui.getRackForSnapshot().setPianoRollView (false);

            // the Channel Rack with the step graph lane (velocity), and the Euclidean fill window
            ui.selectTabForSnapshot (0);
            ui.getRackForSnapshot().setGraphLevelForSnapshot (0);
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-rack-graph.png");
            ui.getRackForSnapshot().setGraphLevelForSnapshot (1);
            saveSnapshot (ui, prefix + "-rack-graph-pan.png");
            ui.getRackForSnapshot().toggleGraphLanes();
            saveWindow (ui.getRackForSnapshot().makeEuclideanPanel (0), "-euclidean.png");

            // the Mixer with three inserts selected and one armed for recording
            ui.selectTabForSnapshot (3);
            if (offline.getSong().mixer.size() > 4)
            {
                ui.getMixerForSnapshot().getStripsForSnapshot().setSelection ({ 1, 2, 3 }, 2);
                offline.setInsertArmed (3, true);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-mixer-multi.png");
                // the armed insert with an audio input (its input row in the side panel, the level bar on the strip)
                offline.editSong ([] (lynx::Song& s) { s.mixer[3].inputChannel = 0; });
                ui.getMixerForSnapshot().selectTrack (3);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-mixer-input.png");
                offline.undo();
                offline.setInsertArmed (3, false);
                ui.getMixerForSnapshot().getStripsForSnapshot().setSelection ({}, 0);
            }

            // the Mixer's routing lane: an insert with sends selected (send knobs and wires)
            if (offline.getSong().mixer.size() > 6)
            {
                int source = -1;
                for (int t = 1; t < (int) offline.getSong().mixer.size() && source < 0; ++t)
                    if (! offline.getSong().mixer[(std::size_t) t].sends.empty())
                        source = t;
                const auto temporary = source < 0;
                if (temporary) // the demo has no sends: add two for the picture
                {
                    source = 1;
                    offline.editSong ([] (lynx::Song& s) { s.setRoute (1, 5, true, 0.6f); s.setRoute (1, 6, true, 0.3f); s.setRoute (4, 1, true, 0.8f); });
                }
                ui.getMixerForSnapshot().selectTrack (source);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-mixer-sends.png");
                if (temporary)
                    offline.editSong ([] (lynx::Song& s) { s.setRoute (1, 5, false); s.setRoute (1, 6, false); s.setRoute (4, 1, false); });
                ui.getMixerForSnapshot().selectTrack (0);
            }

            // the Playlist with ripple editing on
            ui.selectTabForSnapshot (2);
            ui.getPlaylistForSnapshot().setRipple (true);
            saveSnapshot (ui, prefix + "-playlist-ripple.png");
            ui.getPlaylistForSnapshot().setRipple (false);

            // quick-add palette, and the Browser searching
            saveWindow (ui.makeQuickAddForSnapshot(), "-quickadd.png");
            ui.showDesktopForSnapshot (false);
            ui.getBrowserForSnapshot().setSearchForSnapshot ("kick");
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-browser-search.png");
            ui.getBrowserForSnapshot().setSearchForSnapshot ({});
            ui.getBrowserForSnapshot().selectFirstSampleForSnapshot(); // the tree with a sample selected and its waveform
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-browser-tree.png");
            // the Browser's PROJECT tab: the patterns, generators and audio files of the project in their colours
            ui.getBrowserForSnapshot().showProjectForSnapshot();
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-browser-project.png");
            ui.getBrowserForSnapshot().showPluginDatabaseForSnapshot(); // round 142: the Plugin database's FL engines
            offline.dispatchPendingMessages();
            saveSnapshot (ui, prefix + "-browser-plugins.png");
            ui.getBrowserForSnapshot().setTab (lynx::BrowserComponent::allTab);
            saveWindow (ui.getBrowserForSnapshot().makeFoldersPanelForSnapshot(), "-browser-folders.png"); // round 164
            {
                // round 164: the content filter leaves out the folders with nothing to list (a small library)
                const auto lib = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("lynx-snapshot-library");
                lib.deleteRecursively();
                for (const auto* path : { "Drums/Kicks/kick 1.wav", "Drums/Kicks/kick 2.wav", "Drums/snare.wav", "Keys/pad.sf2", "Presets/Synth/lead.fst",
                                          "Projects/beat.flp", "Projects/MIDI/chords.mid", "Notes/readme.txt", "Empty/Nothing/.keep" })
                    lib.getChildFile (path).create();
                ui.showDesktopForSnapshot (false);
                ui.getBrowserForSnapshot().showFilterForSnapshot (lib, lynx::browserview::ContentFilter::all);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-browser164-all.png");
                ui.getBrowserForSnapshot().showFilterForSnapshot (lib, lynx::browserview::ContentFilter::samples);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-browser164-samples.png");
                ui.getBrowserForSnapshot().showFilterForSnapshot (lib, lynx::browserview::ContentFilter::projects);
                offline.dispatchPendingMessages();
                saveSnapshot (ui, prefix + "-browser164-projects.png");
                lib.deleteRecursively();
                ui.getBrowserForSnapshot().resetViewForSnapshot();
            }

            // the undo history after a few edits
            offline.editSong ([] (lynx::Song& s) { s.bpm += 1.0; });
            offline.editSong ([] (lynx::Song& s) { if (! s.channels.empty()) s.channels[0].muted = ! s.channels[0].muted; });
            offline.editSong ([] (lynx::Song& s) { if (! s.patterns.empty()) s.patterns[(std::size_t) s.currentPattern].notes.push_back ({}); });
            offline.undo();
            saveWindow (lynx::makeHistoryPanel (offline), "-history.png");

            // spectral waveforms: a synthetic beat (kick, hats, a chord, a noise riser) as audio clips in spectral
            // colours, zoomed in, classic and stereo; the same file in the Sampler editor and the Browser preview
            {
                const juce::File wav (prefix + "-wavedemo.wav");
                writeWaveDemo (wav, offline.getSong().bpm);
                const auto path = wav.getFullPathName();
                const auto pathUtf8 = path.toStdString();
                int samplerChannel = -1, firstTrack = 0;
                offline.editSong ([&] (lynx::Song& s) {
                    for (const auto& clip : s.playlist)
                        firstTrack = std::max (firstTrack, clip.track + 1);
                    lynx::ChannelDef audio;
                    audio.name = "Beat (audio)";
                    audio.instrument = lynx::InstrumentKind::audioClip;
                    audio.samplePath = pathUtf8;
                    s.channels.push_back (audio);
                    const auto audioChannel = (int) s.channels.size() - 1;
                    lynx::ChannelDef sampler = audio;
                    sampler.name = "Beat (sampler)";
                    sampler.instrument = lynx::InstrumentKind::sampler;
                    sampler.sampler.regionStart = 0.06f;
                    sampler.sampler.regionEnd = 0.94f;
                    s.channels.push_back (sampler);
                    samplerChannel = (int) s.channels.size() - 1;

                    const auto bar = (std::int64_t) lynx::ppq * std::max (1, s.beatsPerBar);
                    const auto whole = (std::int64_t) std::llround (8.0 * s.bpm / 60.0 * lynx::ppq);
                    lynx::Clip clip;
                    clip.kind = lynx::ClipKind::audio;
                    clip.channel = audioChannel;
                    clip.track = firstTrack;
                    clip.length = whole;
                    s.playlist.push_back (clip);
                    clip.track = firstTrack + 1; // a slice of it, later in the song
                    clip.start = bar;
                    clip.offset = bar;
                    clip.length = 2 * bar;
                    s.playlist.push_back (clip);
                    // taller rows for the audio (tracks can be resized: P18)
                    if ((int) s.playlistTracks.size() < firstTrack + 2)
                        s.playlistTracks.resize ((std::size_t) firstTrack + 2);
                    s.playlistTracks[(std::size_t) firstTrack].height = 80;
                    s.playlistTracks[(std::size_t) firstTrack + 1].height = 60;
                });
                offline.dispatchPendingMessages();
                auto& cache = *lynx::ui::WaveformCache::getInstance();
                cache.find (path, [&offline, path] { return offline.getSampleLibrary().load (path); });
                cache.waitUntilIdle (30000);

                ui.selectTabForSnapshot (2);
                auto& playlist = ui.getPlaylistForSnapshot();
                playlist.setTimeSelectionForSnapshot (0, 0);
                {
                    // the Playlist alone (the floating Channel Rack would cover the clips)
                    auto saveSnapshot = [&playlist] (juce::Component&, const juce::String& file) { saveArea (playlist, playlist.getLocalBounds(), file); };
                    playlist.setViewForSnapshot (180.0, 0, std::max (0, firstTrack - 3));
                    saveSnapshot (ui, prefix + "-playlist-audio.png");
                    playlist.setViewForSnapshot (1400.0, 0, std::max (0, firstTrack - 3));
                    saveSnapshot (ui, prefix + "-playlist-audio-zoom.png");
                    lynx::ui::WaveformCache::stereo = true;
                    saveSnapshot (ui, prefix + "-playlist-audio-stereo.png");
                    lynx::ui::WaveformCache::stereo = false;
                    lynx::ui::WaveformCache::spectral = true;
                    playlist.setViewForSnapshot (180.0, 0, std::max (0, firstTrack - 3));
                    saveSnapshot (ui, prefix + "-playlist-audio-spectral.png");
                    lynx::ui::WaveformCache::spectral = false;
                    playlist.setViewForSnapshot (40.0, 0, 0);
                    saveSnapshot (ui, prefix + "-playlist-audio-far.png");

                    // LYNX_SNAPSHOT_AUDIO=a.wav;b.wav...: real audio as clips (default-height tracks, one tall,
                    // a fade, a reversed copy), at three zoom levels
                    if (const auto files = juce::StringArray::fromTokens (juce::SystemStats::getEnvironmentVariable ("LYNX_SNAPSHOT_AUDIO", {}), ";", "");
                        ! files.isEmpty())
                    {
                        const auto realTrack = firstTrack + 2;
                        std::vector<juce::String> paths;
                        for (const auto& f : files)
                            if (juce::File (f.trim()).existsAsFile())
                                paths.push_back (f.trim());

                        offline.editSong ([&] (lynx::Song& s) {
                            const auto bar = (std::int64_t) lynx::ppq * std::max (1, s.beatsPerBar);
                            const std::uint32_t colours[] = { 0, 0xffd9a441, 0xff6fb36a, 0xffb06fd0, 0xff5fb7c9, 0xffd06f6f };
                            for (std::size_t i = 0; i < paths.size(); ++i)
                            {
                                lynx::ChannelDef audio;
                                audio.name = juce::File (paths[i]).getFileNameWithoutExtension().toStdString();
                                audio.instrument = lynx::InstrumentKind::audioClip;
                                audio.samplePath = paths[i].toStdString();
                                audio.colour = colours[i % 6];
                                audio.sampler.stretchBeats = i % 2 == 0 ? 16.0f : 0.0f; // fitted to 4 bars, or as recorded
                                audio.sampler.keepPitch = true;
                                s.channels.push_back (audio);
                                lynx::Clip clip;
                                clip.kind = lynx::ClipKind::audio;
                                clip.channel = (int) s.channels.size() - 1;
                                clip.track = realTrack + (int) i;
                                clip.length = 4 * bar;
                                clip.fadeIn = i == 1 ? bar / 2 : 0;
                                s.playlist.push_back (clip);
                                clip.start = 4 * bar;
                                clip.fadeIn = 0;
                                clip.fadeOut = i == 0 ? bar : 0;
                                s.playlist.push_back (clip);
                            }
                            if ((int) s.playlistTracks.size() < realTrack + (int) paths.size())
                                s.playlistTracks.resize ((std::size_t) realTrack + paths.size());
                            if (! paths.empty())
                                s.playlistTracks[(std::size_t) realTrack].height = 90;
                        });
                        offline.dispatchPendingMessages();
                        for (const auto& path2 : paths)
                            cache.find (path2, [&offline, path2] { return offline.getSampleLibrary().load (path2); });
                        cache.waitUntilIdle (60000);
                        playlist.setViewForSnapshot (160.0, 0, std::max (0, realTrack - 4));
                        saveSnapshot (ui, prefix + "-playlist-real.png");
                        playlist.setViewForSnapshot (700.0, 0, std::max (0, realTrack - 1));
                        saveSnapshot (ui, prefix + "-playlist-real-zoom.png");
                        playlist.setViewForSnapshot (2400.0, 0, std::max (0, realTrack - 1));
                        saveSnapshot (ui, prefix + "-playlist-real-close.png");
                        playlist.setViewForSnapshot (50.0, 0, std::max (0, realTrack - 4));
                        saveSnapshot (ui, prefix + "-playlist-real-far.png");
                        // the clip menu of the first real clip
                        if (const auto menu = playlist.getClipMenuForSnapshot ((int) offline.getSong().playlist.size() - (int) paths.size() * 2); menu.getNumItems() > 0)
                            saveMenus (juce::StringArray { "Audio clip" }, std::vector<juce::PopupMenu> { menu }, prefix + "-clip-menu-audio.png");
                        if (const auto menu = playlist.getClipMenuForSnapshot (0); menu.getNumItems() > 0)
                            saveMenus (juce::StringArray { "Pattern clip" }, std::vector<juce::PopupMenu> { menu }, prefix + "-clip-menu-pattern.png");

                        // the clip menu's actions, run through the menu itself; results in -clip-actions.txt
                        {
                            juce::StringArray log;
                            auto clipOf = [&offline] (const std::string& channelName, int nth) {
                                const auto& sg = offline.getSong();
                                for (int i = 0; i < (int) sg.playlist.size(); ++i)
                                {
                                    const auto& c = sg.playlist[(std::size_t) i];
                                    if (c.kind == lynx::ClipKind::audio && c.channel >= 0 && sg.channels[(std::size_t) c.channel].name == channelName && nth-- == 0)
                                        return i;
                                }
                                return -1;
                            };
                            std::function<bool (const juce::PopupMenu&, const juce::String&)> run = [&run] (const juce::PopupMenu& m, const juce::String& text) {
                                for (juce::PopupMenu::MenuItemIterator it (m); it.next();)
                                {
                                    auto& item = it.getItem();
                                    if (item.subMenu != nullptr && run (*item.subMenu, text))
                                        return true;
                                    if (item.text.startsWith (text) && item.isEnabled && item.action)
                                    {
                                        item.action();
                                        return true;
                                    }
                                }
                                return false;
                            };
                            auto act = [&] (const std::string& name, int nth, const juce::String& text) {
                                const auto clip = clipOf (name, nth);
                                const auto ok = clip >= 0 && run (playlist.getClipMenuForSnapshot (clip), text);
                                offline.dispatchPendingMessages();
                                return ok;
                            };
                            auto channelOf = [&] (const std::string& name) -> const lynx::ChannelDef* {
                                for (const auto& c : offline.getSong().channels)
                                    if (c.name == name)
                                        return &c;
                                return nullptr;
                            };
                            auto check = [&log] (const juce::String& what, bool ok) { log.add ((ok ? "OK    " : "FAIL  ") + what); };
                            const auto names = [&] { std::vector<std::string> n; for (auto& p2 : paths) n.push_back (juce::File (p2).getFileNameWithoutExtension().toStdString()); return n; }();
                            if (names.size() >= 4)
                            {
                                const auto channelsBefore = offline.getSong().channels.size();
                                check ("Reverse", act (names[2], 0, lynx::tr ("Reverse")) && channelOf (names[2])->sampler.reverse);
                                offline.undo();
                                offline.dispatchPendingMessages();
                                check ("Reverse undone", ! channelOf (names[2])->sampler.reverse);
                                act (names[2], 0, lynx::tr ("Reverse"));
                                check ("Make unique", act (names[3], 1, lynx::tr ("Make unique")) && offline.getSong().channels.size() == channelsBefore + 1
                                                          && channelOf (names[3] + " #2") != nullptr);
                                check ("Fit to tempo, resample", act (names[1], 0, lynx::tr ("Fit to tempo, resample ({0})", { juce::String() }).upToFirstOccurrenceOf ("(", false, false))
                                                                     && channelOf (names[1])->sampler.stretchBeats > 0.0f && ! channelOf (names[1])->sampler.keepPitch);
                                const auto gainBefore = offline.getSong().playlist[(std::size_t) clipOf (names[0], 1)].gain;
                                check ("Normalize", act (names[0], 1, lynx::tr ("Normalize"))
                                                        && offline.getSong().playlist[(std::size_t) clipOf (names[0], 1)].gain != gainBefore);
                                const auto clipsBefore = offline.getSong().playlist.size();
                                check ("Split here", act (names[3], 0, lynx::tr ("Split here")) && offline.getSong().playlist.size() == clipsBefore + 1);
                                check ("Mute", act (names[0], 0, lynx::tr ("Mute")) && offline.getSong().playlist[(std::size_t) clipOf (names[0], 0)].muted);
                                check ("Copy + Paste here", act (names[0], 0, lynx::tr ("Copy")) && act (names[0], 1, lynx::tr ("Paste here"))
                                                                && offline.getSong().playlist.size() == clipsBefore + 2);
                                check ("Delete", act (names[0], 2, lynx::tr ("Delete")) && offline.getSong().playlist.size() == clipsBefore + 1);
                                check ("Chop to grid", act (names[3], 0, lynx::tr ("Chop to grid")) && offline.getSong().playlist.size() > clipsBefore + 1);
                            }
                            playlist.setViewForSnapshot (160.0, 0, std::max (0, realTrack - 4));
                            saveSnapshot (ui, prefix + "-playlist-real-actions.png");
                            juce::File (prefix + "-clip-actions.txt").replaceWithText (log.joinIntoString ("\n") + "\n");
                        }
                    }
                }

                saveWindow (lynx::makeChannelSettingsPanel (offline, samplerChannel), "-sampler-wave.png");

                ui.showDesktopForSnapshot (false);
                ui.getBrowserForSnapshot().showPreviewForSnapshot (wav);
                saveSnapshot (ui, prefix + "-browser-preview.png");
            }
        }
        juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
        quit();
    }

    // --snapshot: 8 s of a synthetic stereo beat whose parts sit in different bands (kick = low, chord = mid,
    // hats and the noise riser = high), to show the spectral waveform colours
    static void writeWaveDemo (const juce::File& file, double bpm)
    {
        constexpr double rate = 44100.0;
        const auto frames = (int) (8.0 * rate);
        juce::AudioBuffer<float> buffer (2, frames);
        buffer.clear();
        auto* l = buffer.getWritePointer (0);
        auto* r = buffer.getWritePointer (1);
        const auto beat = 60.0 / juce::jlimit (40.0, 300.0, bpm);
        std::uint32_t noise = 0x9e3779b9u;
        auto white = [&noise] {
            noise ^= noise << 13;
            noise ^= noise >> 17;
            noise ^= noise << 5;
            return (float) (noise & 0xffffff) / (float) 0x800000 - 1.0f;
        };
        float previous = 0.0f;

        for (int i = 0; i < frames; ++i)
        {
            const auto t = i / rate;
            const auto inBeat = std::fmod (t, beat);
            const auto bar = (int) (t / (beat * 4.0));
            float left = 0.0f, right = 0.0f;

            // kick on every beat: a falling sine
            {
                const auto phase = 2.0 * juce::MathConstants<double>::pi * (48.0 * inBeat + 4.0 * (1.0 - std::exp (-inBeat * 30.0)));
                const auto k = (float) (std::sin (phase) * std::exp (-inBeat * 7.0)) * 0.85f;
                left += k;
                right += k;
            }
            // hats on the off-beats: high-passed noise bursts, a little to the right
            {
                const auto h = std::fmod (t + beat * 0.5, beat);
                const auto n = white();
                const auto hp = (n - previous) * 0.5f;
                previous = n;
                const auto hat = hp * (float) std::exp (-h * 45.0) * 0.55f;
                left += hat * 0.6f;
                right += hat;
            }
            // a chord in the second and third bars (mid band)
            if (bar == 1 || bar == 2)
            {
                const auto env = (float) std::min (1.0, std::min (t - beat * 4.0, beat * 12.0 - t) * 8.0);
                float chord = 0.0f;
                for (const auto hz : { 523.25, 659.25, 783.99 })
                    chord += (float) (std::sin (2.0 * juce::MathConstants<double>::pi * hz * t) + 0.3 * std::sin (4.0 * juce::MathConstants<double>::pi * hz * t));
                left += chord * 0.09f * std::max (0.0f, env);
                right += chord * 0.09f * std::max (0.0f, env);
            }
            // a white-noise riser over the last two seconds (high band)
            if (t > 6.0)
            {
                const auto rise = (float) ((t - 6.0) / 2.0);
                left += white() * 0.4f * rise * rise;
                right += white() * 0.4f * rise * rise;
            }
            l[i] = left;
            r[i] = right;
        }

        buffer.applyGain (0.9f / juce::jmax (1.0e-6f, buffer.getMagnitude (0, frames)));
        file.deleteFile();
        juce::WavAudioFormat wav;
        if (std::unique_ptr<juce::AudioFormatWriter> writer { wav.createWriterFor (file.createOutputStream().release(), rate, 2, 24, {}, 0) })
            writer->writeFromAudioSampleBuffer (buffer, 0, frames);
    }

    // the menu bar's menus side by side, drawn with the LookAndFeel as they appear when opened
    static void saveMenus (lynx::MainComponent& ui, const juce::String& path)
    {
        const auto names = ui.getMenuNamesForSnapshot();
        std::vector<juce::PopupMenu> source;
        for (int m = 0; m < names.size(); ++m)
            source.push_back (ui.getMenuForSnapshot (m));
        saveMenus (names, source, path);
    }

    // menus side by side, each under its name
    static void saveMenus (const juce::StringArray& names, const std::vector<juce::PopupMenu>& source, const juce::String& path)
    {
        auto& lf = juce::LookAndFeel::getDefaultLookAndFeel();
        struct Laid
        {
            juce::PopupMenu menu;
            int width = 0, height = 0;
        };
        std::vector<Laid> menus;
        const auto border = lf.getPopupMenuBorderSize();
        for (int m = 0; m < names.size(); ++m)
        {
            Laid laid { source[(std::size_t) m] };
            int width = 160, height = 2 * border;

            for (juce::PopupMenu::MenuItemIterator it (laid.menu); it.next();)
            {
                const auto& item = it.getItem();
                int w = 0, h = 0;
                lf.getIdealPopupMenuItemSize (item.text, item.isSeparator, item.isSectionHeader ? -1 : 0, w, h);
                if (item.isSectionHeader) h += h / 2; // as JUCE does
                if (item.shortcutKeyDescription.isNotEmpty() || item.text.containsChar ('\t'))
                    w += 90;
                width = juce::jmax (width, w + 10);
                height += h;
            }

            laid.width = width;
            laid.height = height;
            menus.push_back (std::move (laid));
        }

        int totalWidth = 16, totalHeight = 0;
        for (const auto& l : menus)
        {
            totalWidth += l.width + 16;
            totalHeight = juce::jmax (totalHeight, l.height + 32);
        }

        juce::Image image (juce::Image::ARGB, totalWidth, totalHeight, true);
        {
            juce::Graphics g (image);
            g.fillAll (lynx::theme::colour::bg1);
            int x = 16;

            for (int m = 0; m < (int) menus.size(); ++m)
            {
                const auto& l = menus[(std::size_t) m];
                g.setColour (lynx::theme::colour::textSecondary);
                g.setFont (lynx::theme::font (lynx::theme::type::small, true));
                g.drawText (names[m], x, 0, l.width, 24, juce::Justification::centredLeft);
                {
                    juce::Graphics::ScopedSaveState state (g);
                    g.reduceClipRegion (x, 24, l.width, l.height);
                    g.setOrigin (x, 24);
                    lf.drawPopupMenuBackground (g, l.width, l.height);
                    int y = border;
                    bool first = true;

                    for (juce::PopupMenu::MenuItemIterator it (l.menu); it.next();)
                    {
                        const auto& item = it.getItem();
                        int w = 0, h = 0;
                        lf.getIdealPopupMenuItemSize (item.text, item.isSeparator, item.isSectionHeader ? -1 : 0, w, h);
                if (item.isSectionHeader) h += h / 2; // as JUCE does
                        const juce::Rectangle<int> r (0, y, l.width, h);
                        auto text = item.text, shortcut = item.shortcutKeyDescription;
                        if (text.containsChar ('\t'))
                        {
                            shortcut = text.fromFirstOccurrenceOf ("\t", false, false);
                            text = text.upToFirstOccurrenceOf ("\t", false, false);
                        }

                        if (item.isSectionHeader)
                            lf.drawPopupMenuSectionHeader (g, r, text);
                        else
                            lf.drawPopupMenuItem (g, r, item.isSeparator, item.isEnabled, first && ! item.isSeparator && m == 0, item.isTicked,
                                                  item.subMenu != nullptr, text, shortcut, item.image.get(), nullptr);
                        first = false;
                        y += h;
                    }
                }
                x += l.width + 16;
            }
        }

        const juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;
        if (auto out = file.createOutputStream())
            png.writeImageToStream (image, *out);
    }

    static void writePng (const juce::Image& image, const juce::String& path)
    {
        const juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;

        if (auto out = file.createOutputStream())
            png.writeImageToStream (image, *out);
    }

    // the splash screen: one settled frame, and a strip of frames through its animation
    static void saveSplashFrames (const juce::String& prefix)
    {
        lynx::SplashScreen splash;
        splash.setStatus (lynx::tr ("Loading the workspace..."));
        splash.setTimeForSnapshot (1.6);
        writePng (splash.createComponentSnapshot (splash.getLocalBounds()), prefix + "-splash.png");

        const double times[] = { 0.10, 0.22, 0.29, 0.40, 0.58, 0.75, 1.00, 1.30 };
        const auto w = lynx::SplashScreen::width / 2, h = lynx::SplashScreen::height / 2;
        juce::Image strip (juce::Image::ARGB, 4 * (w + 8) + 8, 2 * (h + 8) + 8, true);
        {
            juce::Graphics g (strip);
            g.fillAll (lynx::theme::colour::bg1);

            for (int i = 0; i < 8; ++i)
            {
                splash.setTimeForSnapshot (times[i]);
                const auto frame = splash.createComponentSnapshot (splash.getLocalBounds());
                g.drawImage (frame, 8 + (i % 4) * (w + 8), 8 + (i / 4) * (h + 8), w, h, 0, 0, frame.getWidth(), frame.getHeight());
            }
        }
        writePng (strip, prefix + "-splash-frames.png");
    }

    // every icon at 16, 20 and 24 px (at `scale`, e.g. 1.5 for a 150% display), in the toolbar's
    // colours: at rest, hovered and switched on
    static void saveIconSheet (const juce::String& path, float scale)
    {
        const int sizes[] = { 16, 20, 24 };
        const int cell = 34, columns = 3 * 3, labelWidth = 110;
        const auto w = labelWidth + columns * cell + 16, h = lynx::iconCount * cell + 16;
        juce::Image image (juce::Image::ARGB, juce::roundToInt ((float) w * scale), juce::roundToInt ((float) h * scale), true);
        {
        juce::Graphics g (image); // (drawing is flushed when it goes out of scope)
        g.addTransform (juce::AffineTransform::scale (scale));
        g.fillAll (lynx::theme::colour::bg3);
        g.setFont (lynx::theme::font (lynx::theme::type::small));

        for (int i = 1; i < lynx::iconCount; ++i)
        {
            const auto y = 8 + (i - 1) * cell;
            g.setColour (lynx::theme::colour::textDisabled);
            g.drawText (juce::String (i), 8, y, labelWidth - 8, cell, juce::Justification::centredLeft);

            for (int v = 0; v < 3; ++v)
                for (int s = 0; s < 3; ++s)
                {
                    const juce::Rectangle<float> box ((float) (labelWidth + (v * 3 + s) * cell), (float) y, (float) cell, (float) cell);
                    const auto bg = v == 2 ? lynx::theme::colour::accent : v == 1 ? lynx::theme::colour::bg5 : lynx::theme::colour::bg3;
                    g.setColour (bg);
                    g.fillRoundedRectangle (box.reduced (2.0f), 4.0f);
                    const auto fg = v == 2 ? lynx::theme::colour::onAccent : v == 1 ? lynx::theme::colour::textPrimary
                                                                                   : lynx::theme::colour::textSecondary.brighter (0.25f);
                    lynx::icons::draw (g, (lynx::Icon) i, box.withSizeKeepingCentre ((float) sizes[s], (float) sizes[s]), fg);
                }
        }
        }

        writePng (image, path);
    }

    // a part of a component (e.g. the main toolbar)
    static void saveArea (juce::Component& c, juce::Rectangle<int> area, const juce::String& path)
    {
        const juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;
        if (auto out = file.createOutputStream())
            png.writeImageToStream (c.createComponentSnapshot (area), *out);
    }

    static void savePng (const juce::Image& image, const juce::String& path)
    {
        const juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;
        if (auto out = file.createOutputStream())
            png.writeImageToStream (image, *out);
    }

    // -toolbar-tips.png: the top of the window with the bubbles of a few controls as they appear under the
    // mouse; -tips-sheet.png: every quick tip of the window, to check names, shortcuts and translations
    static void saveQuickTips (lynx::MainComponent& ui, const juce::String& prefix)
    {
        const auto tips = ui.getQuickTipsForSnapshot();
        const auto top = juce::Rectangle<int> (0, 0, ui.getWidth(), juce::jmin (ui.getHeight(), ui.getToolbarBoundsForSnapshot().getHeight() + 72));
        auto image = ui.createComponentSnapshot (top);
        {
            juce::Graphics g (image);
            const juce::String shown[] = { lynx::tr ("Pattern mode"), lynx::tr ("Play"), lynx::tr ("Metronome"), lynx::tr ("Mixer"), lynx::tr ("Save project"),
                                           lynx::tr ("Window options"), lynx::tr ("Close") };
            std::vector<juce::Rectangle<int>> used;
            for (const auto& [area, tip] : tips)
            {
                const auto name = tip.upToFirstOccurrenceOf ("\t", false, false);
                if (std::find (std::begin (shown), std::end (shown), name) == std::end (shown))
                    continue;
                const auto bubble = lynx::ui::quickTipBounds (tip, area, top);
                if (std::any_of (used.begin(), used.end(), [&] (const auto& r) { return r.intersects (bubble); }))
                    continue;
                used.push_back (bubble);
                g.setColour (juce::Colours::white.withAlpha (0.075f)); // the hovered control
                g.fillRoundedRectangle (area.toFloat().reduced (0.5f), lynx::theme::radius::md);
                lynx::ui::paintQuickTip (g, bubble.toFloat(), tip);
            }
        }
        savePng (image, prefix + "-toolbar-tips.png");

        // the sheet: bubbles in rows
        const int width = 900;
        int x = 12, y = 12;
        std::vector<std::pair<juce::Rectangle<int>, juce::String>> placed;
        for (const auto& [area, tip] : tips)
        {
            auto r = lynx::ui::quickTipSize (tip);
            if (x + r.getWidth() > width - 12)
            {
                x = 12;
                y += r.getHeight() + 10;
            }
            placed.emplace_back (r.withPosition (x, y), tip);
            x += r.getWidth() + 10;
        }
        juce::Image sheet (juce::Image::ARGB, width, y + 20 + 12, true);
        {
            juce::Graphics g (sheet);
            g.fillAll (lynx::theme::colour::bg3);
            for (const auto& [r, tip] : placed)
                lynx::ui::paintQuickTip (g, r.toFloat(), tip);
        }
        savePng (sheet, prefix + "-tips-sheet.png");
    }

    static void saveSnapshot (juce::Component& c, const juce::String& path)
    {
        const juce::File file (path);
        file.deleteFile();
        juce::PNGImageFormat png;

        if (auto out = file.createOutputStream())
            png.writeImageToStream (c.createComponentSnapshot (c.getLocalBounds()), *out);
    }

    class MainWindow final : public juce::DocumentWindow
    {
    public:
        MainWindow (const juce::String& name, lynx::AudioEngine& engine)
            : DocumentWindow (name, lynx::theme::window, DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            setIcon (lynx::logoImage());
            content = new lynx::MainComponent (engine);
            setContentOwned (content, true);
            setResizable (true, true);
            setResizeLimits (800, 400, 10000, 10000);
            // ~88% of the main display's usable area (at least the UI's design size, but never larger than the
            // display: the AppImage catalog test runs on an 800x600 screen)
            if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
            {
                const auto area = display->userArea;
                const auto fit = [] (int design, int available) {
                    return juce::jmin (available, juce::jmax (design, (int) (available * 0.88)));
                };
                setSize (fit (getWidth(), area.getWidth()), fit (getHeight(), area.getHeight()));
            }

            centreWithSize (getWidth(), getHeight());
        }

        // Shows the window (built hidden while the splash screen is up).
        void reveal()
        {
            setVisible (true);
            lynx::applyWindowChrome (*this);
            lynx::fitWindowOnScreen (*this); // a size saved at another UI zoom still fits (round 163)
            toFront (true);
            content->grabKeyboardFocus();
        }

        void openFile (const juce::File& file) { content->openFile (file); }
        lynx::MainComponent& getContent() { return *content; }
        void showSetupWizard() { content->showSetupWizard(); }
        void confirmDiscard (std::function<void()> then) { content->confirmDiscard (std::move (then)); }

        void closeButtonPressed() override { JUCEApplication::getInstance()->systemRequestedQuit(); }

        // --uiperf: the time of each frame the window paints (its own paint, then every child's, up to the last)
        std::function<void (double)> onFrame;
        void paint (juce::Graphics& g) override
        {
            if (onFrame)
                frameStart = juce::Time::getMillisecondCounterHiRes();
            DocumentWindow::paint (g);
        }
        void paintOverChildren (juce::Graphics& g) override
        {
            DocumentWindow::paintOverChildren (g);
            if (onFrame && frameStart > 0.0)
                onFrame (juce::Time::getMillisecondCounterHiRes() - frameStart);
            frameStart = 0.0;
        }

    private:
        lynx::MainComponent* content = nullptr; // owned by the window
        double frameStart = 0.0;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainWindow)
    };

    lynx::theme::LookAndFeel lookAndFeel;
    std::unique_ptr<lynx::AudioEngine> engine;
    std::unique_ptr<juce::XmlElement> savedDeviceState; // --rtstress: the user's device before this run's --type/--buffer

    // Hidden test mode --asiodll "<driver.dll>[|{CLSID}]" (REA round 97): an ASIO driver loaded straight from its file,
    // without registering it, listed as the driver type "ASIO driver file". True when one was added.
    bool addDriverFileDeviceType()
    {
        const auto args = juce::JUCEApplication::getCommandLineParameterArray();
        const auto i = args.indexOf ("--asiodll");
        if (i < 0 || i + 1 >= args.size() || engine == nullptr)
            return false;
        auto& devices = engine->getDeviceManager();
        devices.getAvailableDeviceTypes(); // creates the standard types first
        for (auto* t : devices.getAvailableDeviceTypes())
            if (t->getTypeName() == lynx::dllAsioTypeName)
                return true;
        auto type = lynx::makeDllAsioDeviceType (args[i + 1].unquoted());
        if (type == nullptr)
            return false;
        devices.addAudioDeviceType (std::move (type));
        return true;
    }
    void restoreSavedDeviceState()
    {
        if (engine != nullptr)
            engine->restoreAudioDeviceState (savedDeviceState.get());
    }
    std::unique_ptr<MainWindow> mainWindow;
    std::unique_ptr<juce::Timer> perf; // --uiperf
    std::unique_ptr<lynx::SplashScreen> splash;
    double splashDuration = lynx::SplashScreen::minimumDuration; // seconds of its clock (round 121: from the process start)
    lynx::StartupOptions startupOptions;
    juce::File fileToOpen;
    juce::File openTimeReport; // --opentime
    int openTimeIdleSeconds = 10;
};

START_JUCE_APPLICATION (LynxApplication)
