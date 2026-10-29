#include "ui/I18n.h"

#include <algorithm>

#include "core/Inflate.h"
#include "engine/PluginHost.h"
#include "ui/I18nData.h"
#include "ui/I18nLocale.h"

namespace lynx
{
namespace
{
juce::String& currentId()
{
    static juce::String id { "en" };
    return id;
}

juce::File settingsFile()
{
    return PluginHost::settingsFolder().getChildFile ("language.txt");
}

bool isKnown (const juce::String& id)
{
    for (const auto& l : i18n::languages())
        if (id == l.id)
            return true;
    return false;
}
} // namespace

juce::String tr (const juce::String& english)
{
    return juce::translate (english);
}

juce::String tr (const char* englishUtf8)
{
    return juce::translate (juce::String::fromUTF8 (englishUtf8));
}

juce::String trc (const char* contextAndEnglishUtf8)
{
    const auto key = juce::String::fromUTF8 (contextAndEnglishUtf8);
    const auto text = juce::translate (key);
    if (text != key || ! key.startsWithChar ('@'))
        return text;
    return key.fromFirstOccurrenceOf (": ", false, false);
}

juce::String tr (const char* englishUtf8, std::initializer_list<juce::String> args)
{
    auto text = tr (englishUtf8);
    int i = 0;
    for (const auto& a : args)
        text = text.replace ("{" + juce::String (i++) + "}", a);
    return text;
}

namespace i18n
{
const std::vector<Language>& languages()
{
    // English first, then alphabetical by English name; each language in its own name (UTF-8)
    static const std::vector<Language> list {
        { "en", "English" },
        { "zh-Hans", "\xe7\xae\x80\xe4\xbd\x93\xe4\xb8\xad\xe6\x96\x87" },             // 简体中文
        { "zh-Hant", "\xe7\xb9\x81\xe9\xab\x94\xe4\xb8\xad\xe6\x96\x87" },             // 繁體中文
        { "nl", "Nederlands" },
        { "fr", "Fran\xc3\xa7" "ais" },
        { "de", "Deutsch" },
        { "id", "Bahasa Indonesia" },
        { "it", "Italiano" },
        { "ja", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" },                                 // 日本語
        { "ko", "\xed\x95\x9c\xea\xb5\xad\xec\x96\xb4" },                                 // 한국어
        { "pl", "Polski" },
        { "pt-BR", "Portugu\xc3\xaas (Brasil)" },
        { "ru", "\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9" },        // Русский
        { "es", "Espa\xc3\xb1ol" },
        { "tr", "T\xc3\xbcrk\xc3\xa7" "e" },
    };
    // only the languages whose table is embedded (a table lands together with its entry)
    static const std::vector<Language> available = [] {
        std::vector<Language> out;
        for (const auto& l : list)
            if (juce::String (l.id) == "en" || hasTable (l.id))
                out.push_back (l);
        return out;
    }();
    return available;
}

bool isNewTranslation (const juce::String& id)
{
    return id != "en" && id != "pt-BR";
}

juce::String fromLocale (const juce::String& localeName)
{
    std::vector<std::string> ids;
    for (const auto& l : languages())
        ids.emplace_back (l.id);
    return juce::String (languageForLocale (localeName.toStdString(), ids));
}

juce::String osDefault()
{
    // Windows: the display language ("de-DE", "zh-TW"); macOS: the first preferred language ("zh-Hans-CN");
    // Linux: LANGUAGE / LC_ALL / LC_MESSAGES / LANG ("ja_JP.UTF-8")
   #if JUCE_LINUX || JUCE_BSD
    for (const char* var : { "LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG" })
        if (const auto value = juce::SystemStats::getEnvironmentVariable (var, {}); value.isNotEmpty() && value != "C" && value != "POSIX")
        {
            // LANGUAGE may be a list ("de:en"): its first entry
            const auto id = fromLocale (value);
            return id.isNotEmpty() ? id : juce::String ("en");
        }
   #endif
    for (const auto& name : { juce::SystemStats::getDisplayLanguage(),
                              juce::SystemStats::getUserLanguage() + "-" + juce::SystemStats::getUserRegion() })
        if (const auto id = fromLocale (name); id.isNotEmpty())
            return id;
    return "en";
}

juce::String current()
{
    return currentId();
}

juce::String saved()
{
    const auto id = settingsFile().loadFileAsString().trim();
    return isKnown (id) ? id : juce::String ("en");
}

void save (const juce::String& id)
{
    if (isKnown (id))
        settingsFile().replaceWithText (id);
}

bool hasTable (const juce::String& id)
{
    for (const auto& t : data::tables)
        if (id == t.id)
            return true;
    return false;
}

juce::String tableText (const juce::String& id)
{
    for (const auto& t : data::tables)
        if (id == t.id)
            if (const auto text = inflateZlib (t.zlib, t.zlibSize, false, t.textSize + 16); text && text->size() == t.textSize)
                return juce::String::fromUTF8 ((const char*) text->data(), (int) text->size());
    return {};
}

void initialise (const juce::StringArray& args)
{
    auto id = saved();
    const auto flag = args.indexOf ("--lang");

    if (flag >= 0 && isKnown (args[flag + 1].unquoted()))
        id = args[flag + 1].unquoted();
    else if (! settingsFile().existsAsFile() && ! PluginHost::settingsFolder().getChildFile ("setup-done.txt").existsAsFile()
             && juce::SystemStats::getEnvironmentVariable ("LYNX_TEST_MODE", {}) != "1"
             && ! std::any_of (args.begin(), args.end(), [] (const juce::String& a) { return a.startsWith ("--"); }))
    {
        // the very first start of the app (no language chosen, setup wizard never finished): the OS language when
        // LYNX has it; the wizard's Language box starts there and the choice is kept
        id = osDefault();
        save (id);
    }

    currentId() = id;
    const auto table = tableText (id);
    juce::LocalisedStrings::setCurrentMappings (table.isEmpty() ? nullptr : new juce::LocalisedStrings (table, false));
}
} // namespace i18n
} // namespace lynx
