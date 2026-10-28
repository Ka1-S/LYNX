#pragma once

// Test profile and test mode (round 165i). Benchmarks and automated GUI runs must not touch the user's real LYNX
// profile, and nothing may wait for a click on the user's desktop:
//   LYNX_PROFILE_DIR=<absolute folder>  every user-data path of LYNX goes there: settings, autosave, sessions and crash
//                                       markers / history, plugin list and scan files, caches (waveforms, PCM and
//                                       time-stretch caches), and the Documents\LYNX folders (templates, mixer states…)
//   LYNX_TEST_MODE=1                    no prompts: the "closed unexpectedly" notice / crash-report question, the
//                                       Missing files dialog, the setup wizard, the project info shown on open and the
//                                       background plugin scan at start are skipped and written to test-mode.log
// The parsing lives here (no JUCE) so the tests can check it; platform/Paths.cpp applies it.

#include <cctype>
#include <optional>
#include <string>

namespace lynx::testprofile
{
// The profile folder named by LYNX_PROFILE_DIR's value: only an absolute path counts (a relative one would depend on
// the working directory); trailing separators are dropped.
inline std::optional<std::string> profileDirFrom (const char* value)
{
    if (value == nullptr)
        return std::nullopt;
    std::string s (value);
    while (! s.empty() && std::isspace ((unsigned char) s.back()))
        s.pop_back();
    std::size_t start = 0;
    while (start < s.size() && std::isspace ((unsigned char) s[start]))
        ++start;
    s = s.substr (start);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        s = s.substr (1, s.size() - 2);
    const auto absolute = (! s.empty() && (s[0] == '/' || s[0] == '\\'))
                       || (s.size() >= 3 && std::isalpha ((unsigned char) s[0]) && s[1] == ':' && (s[2] == '\\' || s[2] == '/'));
    if (! absolute)
        return std::nullopt;
    while (s.size() > 1 && (s.back() == '/' || s.back() == '\\') && ! (s.size() == 3 && s[1] == ':'))
        s.pop_back();
    return s;
}

// LYNX_TEST_MODE's value: on for anything but empty, "0", "false", "no", "off"
inline bool testModeFrom (const char* value)
{
    if (value == nullptr)
        return false;
    std::string s;
    for (const char* p = value; *p != 0; ++p)
        if (! std::isspace ((unsigned char) *p))
            s += (char) std::tolower ((unsigned char) *p);
    return ! (s.empty() || s == "0" || s == "false" || s == "no" || s == "off");
}
} // namespace lynx::testprofile
