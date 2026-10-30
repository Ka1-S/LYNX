#pragma once

#include <cctype>
#include <string>
#include <vector>

// Maps an OS locale name to one of LYNX's interface languages (header-only, no JUCE: unit-tested).
namespace lynx::i18n
{
// "pt-BR", "pt_BR.UTF-8", "zh-Hans-CN", "zh_TW", "de-AT", "ja_JP.UTF-8@euro"... -> "pt-BR", "zh-Hans", "zh-Hant", "de",
// "ja"; empty when the language is not in `available` (ids such as "en", "pt-BR", "zh-Hans").
inline std::string languageForLocale (const std::string& localeName, const std::vector<std::string>& available)
{
    std::vector<std::string> parts (1);
    for (const char c : localeName)
    {
        if (c == '.' || c == '@' || c == ':')
            break;
        if (c == '-' || c == '_')
            parts.emplace_back();
        else
            parts.back() += (char) std::tolower ((unsigned char) c);
    }
    auto has = [&] (const std::string& id) {
        for (const auto& a : available)
            if (a == id)
                return true;
        return false;
    };
    auto pick = [&] (const std::string& id) { return has (id) ? id : std::string(); };
    const auto& lang = parts[0];
    if (lang.empty())
        return {};
    if (lang == "zh")
    {
        for (std::size_t i = 1; i < parts.size(); ++i)
        {
            if (parts[i] == "hant" || parts[i] == "tw" || parts[i] == "hk" || parts[i] == "mo")
                return pick ("zh-Hant");
            if (parts[i] == "hans" || parts[i] == "cn" || parts[i] == "sg")
                return pick ("zh-Hans");
        }
        return pick ("zh-Hans");
    }
    if (lang == "pt")
        return pick ("pt-BR"); // the only Portuguese table: closer than English for pt-PT too
    if (lang == "in")
        return pick ("id"); // the old ISO code for Indonesian (Java/Android)
    for (const auto& a : available)
    {
        std::string lower;
        for (const char c : a)
            lower += (char) std::tolower ((unsigned char) c);
        if (lower == lang)
            return a;
    }
    return {};
}
} // namespace lynx::i18n
