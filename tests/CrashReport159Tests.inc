// Round 159: crash / freeze reports — consent, anonymiser, report JSON, size cap, GitHub issue URL (core/CrashReport.h)

namespace report159
{
inline lynx::crashreport::Anonymiser kauan (bool keepNames = false)
{
    lynx::crashreport::Anonymiser a;
    a.userNames = { "kauan", "Kauan Ferreira" };
    a.homeDirs = { "C:\\Users\\kauan" };
    a.machineNames = { "KAUAN-PC" };
    a.keepFileNames = keepNames;
    return a;
}
inline bool has (const std::string& text, const char* what) { return text.find (what) != std::string::npos; }
} // namespace report159

TEST (report159_consent)
{
    using namespace lynx::crashreport;
    CHECK (parseConsent ("always") == Consent::always);
    CHECK (parseConsent ("never\n") == Consent::never);
    CHECK (parseConsent ("ask") == Consent::ask);
    CHECK (parseConsent ("") == Consent::ask);       // no file yet: ask
    CHECK (parseConsent ("yes") == Consent::ask);    // anything unknown: ask, never "send"
    CHECK (std::string (consentName (Consent::always)) == "always" && std::string (consentName (Consent::never)) == "never");
    for (auto c : { Consent::ask, Consent::always, Consent::never })
        CHECK (parseConsent (consentName (c)) == c);
    CHECK (actionFor (Consent::ask) == Action::askUser);
    CHECK (actionFor (Consent::always) == Action::sendSilently);
    CHECK (actionFor (Consent::never) == Action::dontSend);
}

TEST (report159_anonymise_windows)
{
    using report159::has;
    const auto a = report159::kauan();
    // the home folder, any case and slash style
    CHECK (a.apply ("C:\\Users\\kauan\\AppData\\Roaming\\LYNX\\crash-history.log") == "C:\\Users\\<user>\\AppData\\Roaming\\LYNX\\crash-history.log");
    CHECK (a.apply ("c:/users/KAUAN/AppData/Roaming/LYNX") == "c:/users/<user>/AppData/Roaming/LYNX");
    // another account and another drive
    CHECK (a.apply ("D:\\Users\\maria.souza\\Music\\x.dll") == "D:\\Users\\<user>\\Music\\x.dll");
    CHECK (a.apply ("C:\\Users\\Public\\Documents\\VST3") == "C:\\Users\\Public\\Documents\\VST3");
    // project file names -> their extension, unknown folders -> <dir>
    CHECK (a.apply ("opening C:\\Users\\kauan\\Music\\Cliente Jose\\beat final 2.flp now") == "opening C:\\Users\\<user>\\Music\\<dir>\\<file>.flp now");
    // plugin modules stay readable
    CHECK (a.apply ("C:\\Program Files\\Common Files\\VST3\\FabFilter Pro-Q 3.vst3") == "C:\\Program Files\\Common Files\\VST3\\FabFilter Pro-Q 3.vst3");
    // the names anywhere: machine before user ("KAUAN-PC"), full name words
    CHECK (a.apply ("host KAUAN-PC user kauan") == "host <machine> user <user>");
    CHECK (a.apply ("Ferreira's AirPods") == "<user>'s AirPods");
    CHECK (a.apply ("Kauanzinho") == "Kauanzinho"); // whole words only
    // UNC shares
    CHECK (a.apply ("\\\\STUDIO-NAS\\samples\\kick.wav") == "\\\\<host>\\samples\\<file>.wav");
    // the frames of a crash file are untouched
    const std::string frames = "10: RegisterProcTableCallback + 0x18f073\nLYNX.exe+0x1a2b3c  (juce::Timer::callTimers)\n";
    CHECK (a.apply (frames) == frames);
    // keepFileNames: names and folders stay, the user still goes
    CHECK (report159::kauan (true).apply ("C:\\Users\\kauan\\Music\\Cliente Jose\\beat.flp") == "C:\\Users\\<user>\\Music\\Cliente Jose\\beat.flp");
    CHECK (! has (a.apply ("C:\\Users\\kauan\\Desktop\\a.wav, C:\\Users\\kauan\\Desktop\\b.wav"), "a.wav"));
}

TEST (report159_anonymise_onedrive_accents)
{
    const auto a = report159::kauan();
    // OneDrive's localised Desktop (accented), a project in a folder of the user's own
    CHECK (a.apply ("C:\\Users\\kauan\\OneDrive\\Área de Trabalho\\Músicas da Luísa\\ação.flp")
           == "C:\\Users\\<user>\\OneDrive\\Área de Trabalho\\<dir>\\<file>.flp");
    // OneDrive for Business: the organisation's name
    CHECK (a.apply ("C:\\Users\\kauan\\OneDrive - Nexbox Ltda\\Documentos\\voz.wav") == "C:\\Users\\<user>\\OneDrive - <org>\\Documentos\\<file>.wav");
    // an accented account name (not the known one)
    CHECK (a.apply ("C:\\Users\\João Silva\\Downloads\\x.mp3") == "C:\\Users\\<user>\\Downloads\\<file>.mp3");
    // invalid UTF-8 is repaired, not passed on
    const auto bad = a.apply (std::string ("abc\xC3(def", 8));
    CHECK (bad == "abc\xEF\xBF\xBD(def");
}

TEST (report159_anonymise_macos_linux)
{
    lynx::crashreport::Anonymiser a;
    a.userNames = { "kauan" };
    a.homeDirs = { "/home/kauan" };
    CHECK (a.apply ("/home/kauan/opt/LYNX/LYNX(+0x1234) [0x55d1]") == "/home/<user>/opt/LYNX/LYNX(+0x1234) [0x55d1]");
    CHECK (a.apply ("/Users/someone/Library/Audio/Plug-Ins/VST3/X.vst3") == "/Users/<user>/Library/Audio/Plug-Ins/VST3/X.vst3");
    CHECK (a.apply ("open /home/kauan/My Music/demo take.wav") == "open /home/<user>/My Music/<file>.wav");
    CHECK (a.apply ("saved ~/Projetos/Bandas/song.lynx") == "saved ~/Projetos/<dir>/<file>.lynx");
    CHECK (a.apply ("/mnt/d/Beats (old)/x [final].mp3") == "/mnt/d/<dir>/<file>.mp3");
    // prose after a home folder stays
    CHECK (a.apply ("/home/x is gone") == "/home/<user> is gone");
}

TEST (report159_anonymise_steps)
{
    const auto a = report159::kauan();
    CHECK (a.apply ("00:05:37 opening project Beat da Luísa (v2).flp") == "00:05:37 opening project <file>.flp");
    CHECK (a.apply ("00:05:38 loading plugin H-Delay Stereo (VST3, WaveShell1-VST3 16.0_x64.vst3)")
           == "00:05:38 loading plugin H-Delay Stereo (VST3, WaveShell1-VST3 16.0_x64.vst3)");
    CHECK (a.apply ("00:08:00 crashed: backtrace in autosave\\crash 2026-10-25 00-07-59.txt")
           == "00:08:00 crashed: backtrace in autosave\\crash 2026-10-25 00-07-59.txt");
    CHECK (a.apply ("Last step: RODEIOS.mp3") == "Last step: <file>.mp3");
    CHECK (lynx::crashreport::isContentExtension ("flp") && lynx::crashreport::isContentExtension ("wav") && ! lynx::crashreport::isContentExtension ("dll")
           && ! lynx::crashreport::isContentExtension ("txt"));
}

TEST (report159_payload)
{
    using namespace lynx::crashreport;
    using report159::has;
    std::uint8_t bytes[16];
    for (int i = 0; i < 16; ++i)
        bytes[i] = (std::uint8_t) (i * 37 + 11);
    ReportInput in;
    in.id = formatUuid (bytes);
    CHECK (validId (in.id));
    CHECK (! validId ("0F8B6A52-1C2D-4E3F-9A0B-1234567890AB") && ! validId ("x") && ! validId ("0f8b6a52-1c2d-1e3f-9a0b-1234567890ab"));
    in.kind = "crash";
    in.created = "2026-10-25T00:08:06-03:00";
    in.version = "0.1.16";
    in.build = "release";
    in.commit = "abc1234";
    in.language = "pt-BR";
    in.os = "Windows 11";
    in.osVersion = "24H2 (26100.4061)";
    in.cpu = "12th Gen Intel(R) Core(TM) i7-12650H";
    in.cpuCores = 16;
    in.gpus = { { "Intel(R) UHD Graphics", "32.0.101.7076" }, { "NVIDIA GeForce RTX 3050 Laptop GPU", "32.0.15.6094" } };
    in.screens = { { 1920, 1080, 1.25 } };
    in.audioDriver = "ASIO";
    in.audioDevice = "SSL 2 MKII ASIO Driver";
    in.sampleRate = 44100;
    in.bufferSize = 256;
    in.crashFile = "crash 2026-10-25 00-07-59.txt";
    in.crashText = "C:\\Users\\kauan\\AppData\\x\n\"quoted\"\ttab\x01";
    for (int i = 0; i < 80; ++i)
        in.steps.push_back ("00:05:" + std::to_string (i) + " step " + std::to_string (i));
    in.plugins = { { "Auto-Tune Pro", "Antares", "VST3", "10.3.1" } };
    in.settings = { { "gpuRendering", true }, { "pluginSandbox", false } };
    const auto json = buildJson (in, report159::kauan());
    CHECK (has (json, "\"schema\": 1"));
    CHECK (has (json, ("\"id\": \"" + in.id + "\"").c_str()));
    CHECK (has (json, "\"kind\": \"crash\""));
    CHECK (has (json, "\"test\": false"));
    CHECK (has (json, "C:\\\\Users\\\\<user>\\\\AppData")); // escaped backslashes, user masked
    CHECK (! has (json, "kauan"));
    CHECK (has (json, "\\\"quoted\\\"\\ttab\\u0001"));      // JSON escapes
    CHECK (has (json, "\"gpuRendering\": true") && has (json, "\"pluginSandbox\": false"));
    CHECK (has (json, "\"sampleRate\": 44100") && has (json, "\"bufferSize\": 256") && has (json, "\"scale\": 1.25"));
    CHECK (has (json, "RTX 3050") && has (json, "32.0.101.7076") && has (json, "Antares") && has (json, "10.3.1"));
    // the last 60 steps only
    CHECK (! has (json, "step 19\"") && has (json, "step 20\"") && has (json, "step 79\""));
    CHECK (! has (json, "\"user\"")); // no description, no e-mail: no user section
    // a manual report with an optional e-mail
    in.kind = "manual";
    in.userText = "It froze after I opened kauan's beat.flp";
    in.email = "someone@example.com";
    const auto manual = buildJson (in, report159::kauan());
    CHECK (has (manual, "\"email\": \"someone@example.com\"") && has (manual, "<file>.flp") && ! has (manual, "beat"));
    // the balanced braces / brackets (a rough well-formedness check without a JSON parser)
    int braces = 0, brackets = 0;
    bool inString = false;
    for (std::size_t i = 0; i < manual.size(); ++i)
    {
        const char c = manual[i];
        if (inString)
        {
            if (c == '\\')
                ++i;
            else if (c == '"')
                inString = false;
            continue;
        }
        braces += c == '{' ? 1 : c == '}' ? -1 : 0;
        brackets += c == '[' ? 1 : c == ']' ? -1 : 0;
        inString = c == '"';
    }
    CHECK (braces == 0 && brackets == 0 && ! inString);
}

TEST (report159_size_cap)
{
    using namespace lynx::crashreport;
    using report159::has;
    ReportInput in;
    in.id = "0f8b6a52-1c2d-4e3f-9a0b-1234567890ab";
    in.kind = "hang";
    in.version = "0.1.16";
    std::string big;
    for (int i = 0; i < 40000; ++i)
        big += "LYNX.exe+0x" + std::to_string (100000 + i) + "  (frame " + std::to_string (i) + ")\n"; // ~1.4 MB
    in.crashText = big;
    in.hangText = big;
    for (int i = 0; i < 400; ++i)
        in.plugins.push_back ({ "Plugin " + std::to_string (i), "Vendor", "VST3", "1.0" });
    const auto json = buildJson (in, {}, 256 * 1024);
    CHECK (json.size() <= 256 * 1024);
    CHECK (has (json, "crash text shortened"));
    CHECK (has (json, "(frame 0)") && has (json, "(frame 39999)")); // head and tail kept
    CHECK (has (json, "[... cut ...]"));
    const auto tiny = buildJson (in, {}, 8 * 1024);
    CHECK (tiny.size() <= 8 * 1024 + 4096); // the last resort: everything shortened
    CHECK (has (tiny, "everything shortened"));
    CHECK (buildJson (in, {}).size() <= defaultMaxBytes);
    // UTF-8 is never cut inside a character
    CHECK (truncateUtf8 ("ação", 2) == "a" && truncateUtf8 ("ação", 3) == "aç" && truncateUtf8 ("ab", 5) == "ab");
}

TEST (report159_github_url)
{
    using namespace lynx::crashreport;
    using report159::has;
    CHECK (percentEncode ("a b&c=d/é~_.-") == "a%20b%26c%3Dd%2F%C3%A9~_.-");
    CHECK (githubOsOption ("Windows 11") == "Windows 11" && githubOsOption ("Windows 10") == "Windows 10");
    CHECK (githubOsOption ("Mac OSX 15.1", "arm64") == "macOS (Apple silicon)" && githubOsOption ("Mac OSX 15.1", "x86_64") == "macOS (Intel)");
    CHECK (githubOsOption ("Linux", {}, true) == "Linux (AppImage)" && githubOsOption ("Linux") == "Linux (tar.gz)" && githubOsOption ("BeOS").empty());
    CHECK (githubKindOption ("crash") == "LYNX SHARE closed (crash)" && githubKindOption ("hang") == "LYNX SHARE froze (not responding)"
           && githubKindOption ("manual") == "Other");

    IssueFields f;
    f.title = "Crash: Auto-Tune Pro editor";
    f.version = "0.1.16";
    f.os = "Windows 11";
    f.kind = githubKindOption ("crash");
    f.steps = "1. Open the editor\n2. LYNX closes";
    const auto url = githubIssueUrl (f);
    CHECK (url.rfind ("https://github.com/Ka1-S/LYNX-SHARE/issues/new?template=bug_report.yml&", 0) == 0);
    CHECK (has (url, "&os=Windows%2011") && has (url, "&kind=LYNX%20SHARE%20closed%20%28crash%29") && has (url, "&version=0.1.16"));
    CHECK (has (url, "&steps=1.%20Open%20the%20editor%0A2.%20LYNX%20closes"));
    CHECK (! has (url, "&logs=")); // empty fields are left out

    // a huge log is cut to fit, with an ellipsis, never inside an escape or a character
    std::string log;
    for (int i = 0; i < 3000; ++i)
        log += "ação LYNX.exe+0x" + std::to_string (i) + "\n";
    f.logs = log;
    f.plugins = std::string (5000, 'p');
    const auto cut = githubIssueUrl (f, 8000);
    CHECK (cut.size() <= 8000);
    CHECK (cut.size() > 7000);                 // filled up, not emptied
    CHECK (has (cut, "%0A%E2%80%A6"));          // "\n…"
    CHECK (has (cut, "&os=Windows%2011"));     // the fixed fields survive
    for (std::size_t i = 0; i < cut.size(); ++i) // every % is followed by two hex digits
        if (cut[i] == '%')
            CHECK (i + 2 < cut.size() && std::isxdigit ((unsigned char) cut[i + 1]) && std::isxdigit ((unsigned char) cut[i + 2]));
    CHECK (githubIssueUrl (f, 2000).size() <= 2000);
}
