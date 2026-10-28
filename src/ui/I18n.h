#pragma once

#include <initializer_list>
#include <vector>

#include <juce_core/juce_core.h>

// Interface languages. English is the source language: every user-visible string is written in
// English in the code and wrapped in tr(); the other languages are JUCE LocalisedStrings tables
// embedded in the executable (resources/i18n/*.txt -> ui/i18n/<id>.h + ui/I18nData.h, see tools/make_i18n.py).
//
// The tests scan the sources for string literals passed to tr() / trc() / TR_NOOP() and check that every one
// has a non-empty entry in each translation file.

// Marks a literal for translation without translating it (for tables translated later with tr()).
#define TR_NOOP(text) text

namespace lynx
{
// Translates an English UI string (UTF-8) into the current language.
juce::String tr (const char* englishUtf8);
juce::String tr (const juce::String& english);

// Same, then replaces {0}, {1}... with `args` (the translation may reorder them).
juce::String tr (const char* englishUtf8, std::initializer_list<juce::String> args);

// A short word that needs its own translation in one place: the key is "@context: text"
// (e.g. trc ("@plugin status: OK")); without a translation the text after ": " is shown.
juce::String trc (const char* contextAndEnglishUtf8);

namespace i18n
{
struct Language
{
    const char* id;         // "en", "pt-BR"
    const char* nativeName; // as shown in the Language menu
};

const std::vector<Language>& languages();

juce::String current();                       // id of the language in use
juce::String saved();                         // id stored in the settings (default "en")
void save (const juce::String& id);           // applied at the next start
void initialise (const juce::StringArray& commandLine); // --lang <id> overrides the saved setting

// The embedded table for a language id (empty for English).
juce::String tableText (const juce::String& id);
bool hasTable (const juce::String& id);

// The language id for an OS locale name ("de-AT", "pt_BR.UTF-8", "zh-Hant-TW"...); empty when LYNX lacks it.
juce::String fromLocale (const juce::String& localeName);
juce::String osDefault(); // the OS interface language when LYNX has it, else "en"

// Translations added in 0.1.20 (shown with "New translation - suggestions welcome").
bool isNewTranslation (const juce::String& id);
} // namespace i18n
} // namespace lynx
