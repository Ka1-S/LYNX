#include "ui/PluginPresetMenu.h"

#include "engine/PluginHost.h"
#include "engine/Vst2Format.h"
#include "platform/Paths.h"
#include "ui/I18n.h"
#include "ui/PluginWindow.h"
#include "ui/Theme.h"

namespace lynx::pluginpresetui
{
namespace
{
namespace pp = pluginpresets;

juce::String pluginIdOf (juce::AudioPluginInstance& plugin)
{
    return plugin.getPluginDescription().createIdentifierString();
}

std::vector<std::uint8_t> bytesOf (const juce::MemoryBlock& m)
{
    const auto* b = static_cast<const std::uint8_t*> (m.getData());
    return { b, b + m.getSize() };
}

// VST3's .vstpreset through JUCE's VST3 host (in-process VST3 only)
struct Vst3Access final : juce::ExtensionsVisitor
{
    std::function<void (const VST3Client&)> fn;
    void visitVST3Client (const VST3Client& c) override
    {
        if (fn)
            fn (c);
    }
};

bool isVst3 (juce::AudioPluginInstance& plugin)
{
    bool found = false;
    Vst3Access v;
    v.fn = [&found] (const auto&) { found = true; };
    plugin.getExtensions (v);
    return found;
}

void setParams (juce::AudioPluginInstance& plugin, const std::vector<float>& values)
{
    const auto& params = plugin.getParameters();
    for (int i = 0; i < juce::jmin (params.size(), (int) values.size()); ++i)
        params[i]->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, values[(std::size_t) i]));
}

std::vector<float> paramValues (juce::AudioPluginInstance& plugin)
{
    std::vector<float> out;
    for (auto* p : plugin.getParameters())
        out.push_back (p->getValue());
    return out;
}

bool loadFx (juce::AudioPluginInstance& plugin, const std::vector<std::uint8_t>& bytes, juce::String& error)
{
    const auto fx = pp::parseFx (bytes);
    if (! fx)
    {
        error = tr ("This file is not a VST2 preset (.fxp / .fxb) LYNX SHARE can read.");
        return false;
    }
    const auto info = vst2::info (plugin);
    if (info && fx->pluginId != 0 && fx->pluginId != info->uniqueId)
    {
        error = tr ("This preset was saved by another plugin.");
        return false;
    }
    using K = pp::FxFile::Kind;
    if (fx->isChunk())
    {
        if (! info || ! vst2::setChunk (plugin, fx->chunk, fx->kind == K::chunkProgram))
        {
            error = tr ("Only a VST2 plugin running inside LYNX SHARE can read this preset (it holds the plugin's own data). "
                        "Turn off \"Make bridged\" / the separate process for it, or save a LYNX SHARE preset instead.");
            return false;
        }
        if (fx->kind == K::chunkBank && fx->currentProgram > 0 && fx->currentProgram < plugin.getNumPrograms())
            plugin.setCurrentProgram (fx->currentProgram);
        return true;
    }
    const auto expected = plugin.getParameters().size();
    if (fx->programs.empty() || (int) fx->programs.front().params.size() != expected)
    {
        error = tr ("This preset has {0} parameters, the plugin {1}.", { juce::String (fx->programs.empty() ? 0 : (int) fx->programs.front().params.size()), juce::String (expected) });
        return false;
    }
    if (fx->kind == K::paramBank && plugin.getNumPrograms() > 1)
    {
        for (int p = 0; p < juce::jmin (plugin.getNumPrograms(), (int) fx->programs.size()); ++p)
        {
            plugin.setCurrentProgram (p);
            setParams (plugin, fx->programs[(std::size_t) p].params);
        }
        plugin.setCurrentProgram (juce::jlimit (0, plugin.getNumPrograms() - 1, fx->currentProgram));
    }
    else
        setParams (plugin, fx->programs.front().params);
    return true;
}

bool saveFx (juce::AudioPluginInstance& plugin, bool bank, const juce::String& name, std::vector<std::uint8_t>& out, juce::String& error)
{
    const auto info = vst2::info (plugin);
    if (! info)
    {
        error = tr ("Only VST2 plugins running inside LYNX SHARE save .fxp / .fxb files.");
        return false;
    }
    pp::FxFile fx;
    fx.pluginId = info->uniqueId;
    fx.pluginVersion = info->version;
    fx.currentProgram = juce::jmax (0, plugin.getCurrentProgram());
    if (info->chunks)
    {
        fx.kind = bank ? pp::FxFile::Kind::chunkBank : pp::FxFile::Kind::chunkProgram;
        fx.programName = name.toStdString();
        fx.bankPrograms = info->numPrograms;
        if (! vst2::getChunk (plugin, fx.chunk, ! bank))
        {
            error = tr ("The plugin returned no data.");
            return false;
        }
    }
    else if (! bank)
    {
        fx.kind = pp::FxFile::Kind::paramProgram;
        fx.programs.push_back ({ name.toStdString(), paramValues (plugin) });
    }
    else
    {
        fx.kind = pp::FxFile::Kind::paramBank;
        const auto was = plugin.getCurrentProgram();
        for (int p = 0; p < juce::jmax (1, plugin.getNumPrograms()); ++p)
        {
            if (plugin.getNumPrograms() > 1)
                plugin.setCurrentProgram (p);
            fx.programs.push_back ({ plugin.getProgramName (p).toStdString(), paramValues (plugin) });
        }
        if (plugin.getNumPrograms() > 1)
            plugin.setCurrentProgram (was);
    }
    out = pp::writeFx (fx);
    return true;
}
} // namespace

juce::File presetsRoot()
{
    return platform::settingsFile ("Presets").getChildFile ("Plugins");
}

juce::File folderFor (const juce::String& pluginName)
{
    return presetsRoot().getChildFile (juce::String::fromUTF8 (pp::folderNameFor (pluginName.toStdString()).c_str()));
}

bool isPresetFile (const juce::File& file)
{
    return file.hasFileExtension ("lynxpreset;fxp;fxb;vstpreset");
}

juce::Array<juce::File> userPresets (const juce::String& pluginName)
{
    auto files = folderFor (pluginName).findChildFiles (juce::File::findFiles, false, "*.lynxpreset;*.fxp;*.fxb;*.vstpreset");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getFileName().compareNatural (b.getFileName()) < 0;
    });
    return files;
}

std::optional<pp::Preset> readLynxPreset (const juce::File& file)
{
    if (! file.hasFileExtension ("lynxpreset") || file.getSize() > 64 * 1024 * 1024)
        return std::nullopt;
    return pp::fromText (file.loadFileAsString().toStdString());
}

bool load (juce::AudioPluginInstance& plugin, const juce::File& file, juce::String& error)
{
    if (file.hasFileExtension ("lynxpreset"))
    {
        const auto preset = readLynxPreset (file);
        if (! preset)
        {
            error = tr ("This file is not a plugin preset.");
            return false;
        }
        const auto id = pluginIdOf (plugin).toStdString();
        if (! preset->pluginId.empty() && preset->pluginId != id && preset->pluginName != plugin.getName().toStdString())
        {
            error = tr ("This preset belongs to {0}.", { juce::String::fromUTF8 (preset->pluginName.c_str()) });
            return false;
        }
        plugin.setStateInformation (preset->state.data(), (int) preset->state.size());
        return true;
    }

    juce::MemoryBlock data;
    if (! file.loadFileAsData (data) || data.getSize() == 0)
    {
        error = tr ("The file could not be read.");
        return false;
    }
    if (file.hasFileExtension ("fxp;fxb"))
        return loadFx (plugin, bytesOf (data), error);
    if (file.hasFileExtension ("vstpreset"))
    {
        bool ok = false, vst3 = false;
        Vst3Access v;
        v.fn = [&] (const juce::ExtensionsVisitor::VST3Client& c) {
            vst3 = true;
            ok = c.setPreset (data);
        };
        plugin.getExtensions (v);
        if (! ok)
            error = vst3 ? tr ("The plugin did not accept this preset (saved by another plugin?).")
                         : tr ("Only VST3 plugins running inside LYNX SHARE read .vstpreset files.");
        return ok;
    }
    error = tr ("Unknown preset format.");
    return false;
}

bool save (juce::AudioPluginInstance& plugin, const juce::File& file, const juce::String& presetName, juce::String& error)
{
    std::vector<std::uint8_t> bytes;
    if (file.hasFileExtension ("fxp;fxb"))
    {
        if (! saveFx (plugin, file.hasFileExtension ("fxb"), presetName, bytes, error))
            return false;
    }
    else if (file.hasFileExtension ("vstpreset"))
    {
        bool vst3 = false;
        juce::MemoryBlock data;
        Vst3Access v;
        v.fn = [&] (const juce::ExtensionsVisitor::VST3Client& c) {
            vst3 = true;
            data = c.getPreset();
        };
        plugin.getExtensions (v);
        if (! vst3 || data.getSize() == 0)
        {
            error = tr ("Only VST3 plugins running inside LYNX SHARE save .vstpreset files.");
            return false;
        }
        bytes = bytesOf (data);
    }
    else
    {
        juce::MemoryBlock state;
        plugin.getStateInformation (state);
        pp::Preset p;
        p.name = presetName.toStdString();
        p.pluginName = plugin.getName().toStdString();
        p.pluginId = pluginIdOf (plugin).toStdString();
        p.format = plugin.getPluginDescription().pluginFormatName.toStdString();
        p.state = bytesOf (state);
        const auto text = pp::toText (p);
        bytes.assign (text.begin(), text.end());
    }
    file.getParentDirectory().createDirectory();
    if (! file.replaceWithData (bytes.data(), bytes.size()))
    {
        error = tr ("The file could not be written.");
        return false;
    }
    return true;
}

juce::String saveWildcard (juce::AudioPluginInstance& plugin)
{
    juce::String w = "*.lynxpreset";
    if (vst2::info (plugin))
        w << ";*.fxp;*.fxb";
    if (isVst3 (plugin))
        w << ";*.vstpreset";
    return w;
}

bool applyFromBrowser (AudioEngine& engine, const juce::File& file, juce::String& message)
{
    const auto preset = readLynxPreset (file);
    // the plugin this file is for: from a .lynxpreset, or the folder it is in (<Plugins>/<plugin name>/)
    const auto folderName = file.getParentDirectory().getFileName();
    auto matches = [&] (juce::AudioPluginInstance& p) {
        if (preset)
            return preset->pluginId == pluginIdOf (p).toStdString() || preset->pluginName == p.getName().toStdString();
        return folderName == juce::String::fromUTF8 (pp::folderNameFor (p.getName().toStdString()).c_str());
    };
    std::vector<juce::AudioPluginInstance*> candidates = PluginWindows::openPlugins();
    if (auto* p = dynamic_cast<PluginInstrument*> (engine.getChannelInstrument (engine.getSelectedChannel()).get()))
        candidates.push_back (&p->getInstance());
    for (auto* p : candidates)
        if (p != nullptr && matches (*p))
        {
            juce::String error;
            if (load (*p, file, error))
            {
                engine.markModified();
                return true;
            }
            message = error;
            return false;
        }
    // a .vstpreset / .fxp from somewhere else: the last plugin window used, whatever it is
    if (! preset && ! candidates.empty() && candidates.front() != nullptr && ! file.isAChildOf (presetsRoot()))
    {
        juce::String error;
        if (load (*candidates.front(), file, error))
        {
            engine.markModified();
            return true;
        }
        message = error;
        return false;
    }
    if (preset)
        if (const auto d = engine.getPluginHost().find (juce::String::fromUTF8 (preset->pluginId.c_str())); d != nullptr && d->isInstrument)
        {
            PluginRef ref;
            ref.id = preset->pluginId;
            ref.name = preset->pluginName;
            ref.state = juce::MemoryBlock (preset->state.data(), preset->state.size()).toBase64Encoding().toStdString();
            engine.editSong ([&] (Song& s) {
                ChannelDef c;
                c.name = preset->name.empty() ? preset->pluginName : preset->name;
                c.instrument = InstrumentKind::plugin;
                c.plugin = ref;
                c.mixerTrack = s.nextFreeInsert();
                s.channels.push_back (c);
            });
            engine.setSelectedChannel ((int) engine.getSong().channels.size() - 1);
            return true;
        }
    message = preset ? tr ("Open {0} first (its plugin window), then double-click the preset.", { juce::String::fromUTF8 (preset->pluginName.c_str()) })
                     : tr ("Open the plugin first (its plugin window), then double-click the preset.");
    return false;
}

// --- the bar ---------------------------------------------------------------------------------------------------------

PresetBar::PresetBar (juce::AudioPluginInstance& p, std::function<void()> changed) : plugin (p), onChanged (std::move (changed))
{
    prev.setTooltip (tr ("Previous preset"));
    next.setTooltip (tr ("Next preset"));
    prev.onClick = [this] { step (-1); };
    next.onClick = [this] { step (1); };
    addAndMakeVisible (prev);
    addAndMakeVisible (next);
    setRepaintsOnMouseActivity (true);
    setTooltip (tr ("Presets: the plugin's programs and your presets; save, load and export preset files"));
    if (plugin.getNumPrograms() > 1)
    {
        currentProgram = plugin.getCurrentProgram();
        current = plugin.getProgramName (currentProgram);
    }
}

void PresetBar::resized()
{
    auto r = getLocalBounds();
    prev.setBounds (r.removeFromLeft (24));
    r.removeFromLeft (2);
    next.setBounds (r.removeFromLeft (24));
}

void PresetBar::paint (juce::Graphics& g)
{
    using namespace theme;
    auto r = getLocalBounds().withTrimmedLeft (54).toFloat();
    g.setColour (isMouseOver() ? colour::bg5 : colour::bg4);
    g.fillRoundedRectangle (r, radius::md);
    g.setColour (current.isEmpty() ? colour::textSecondary : colour::textPrimary);
    g.setFont (font (type::body));
    g.drawFittedText (current.isEmpty() ? tr ("Presets") : current, r.reduced (8, 0).withTrimmedRight (12).toNearestInt(),
                      juce::Justification::centredLeft, 1);
    g.setColour (colour::textSecondary);
    const auto c = juce::Point<float> (r.getRight() - 10, r.getCentreY());
    juce::Path arrow;
    arrow.addTriangle (c.x - 4, c.y - 2, c.x + 4, c.y - 2, c.x, c.y + 3);
    g.fillPath (arrow);
}

void PresetBar::mouseUp (const juce::MouseEvent& e)
{
    if (e.x >= 54 && ! e.mouseWasDraggedSinceMouseDown())
        showMenu();
}

std::vector<PresetBar::Entry> PresetBar::entries() const
{
    std::vector<Entry> out;
    auto& p = const_cast<juce::AudioPluginInstance&> (plugin);
    if (p.getNumPrograms() > 1)
        for (int i = 0; i < p.getNumPrograms(); ++i)
        {
            auto name = p.getProgramName (i);
            out.push_back ({ i, {}, name.isEmpty() ? tr ("Program {0}", { juce::String (i + 1) }) : name });
        }
    for (const auto& f : userPresets (plugin.getName()))
        out.push_back ({ -1, f, f.getFileNameWithoutExtension() });
    return out;
}

void PresetBar::step (int delta)
{
    const auto list = entries();
    if (list.empty())
        return;
    int at = -1;
    for (int i = 0; i < (int) list.size(); ++i)
        if ((currentProgram >= 0 && list[(std::size_t) i].program == currentProgram && currentFile == juce::File())
            || (currentFile != juce::File() && list[(std::size_t) i].file == currentFile))
            at = i;
    const auto n = (int) list.size();
    at = at < 0 ? (delta > 0 ? 0 : n - 1) : ((at + delta) % n + n) % n;
    apply (list[(std::size_t) at]);
}

void PresetBar::apply (const Entry& entry)
{
    if (entry.program >= 0)
    {
        plugin.setCurrentProgram (entry.program);
        currentProgram = entry.program;
        currentFile = juce::File();
        setName (entry.name);
        return;
    }
    juce::String error;
    if (! load (plugin, entry.file, error))
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Load preset"), error, {}, this);
        return;
    }
    currentFile = entry.file;
    currentProgram = -1;
    setName (entry.name);
}

void PresetBar::setName (const juce::String& name)
{
    current = name;
    repaint();
    if (onChanged)
        onChanged();
}

void PresetBar::showMenu()
{
    juce::PopupMenu menu;
    const auto list = entries();
    juce::PopupMenu programs;
    int factory = 0;
    for (const auto& e : list)
        if (e.program >= 0)
        {
            ++factory;
            programs.addItem (e.name, true, currentFile == juce::File() && e.program == currentProgram, [this, e] { apply (e); });
        }
    if (factory > 0)
    {
        if (factory <= 32)
        {
            menu.addSectionHeader (tr ("Plugin presets"));
            for (juce::PopupMenu::MenuItemIterator it (programs); it.next();)
                menu.addItem (it.getItem());
        }
        else
            menu.addSubMenu (tr ("Plugin presets ({0})", { juce::String (factory) }), programs);
    }
    bool users = false;
    for (const auto& e : list)
        if (e.program < 0)
        {
            if (! users)
                menu.addSectionHeader (tr ("Your presets"));
            users = true;
            menu.addItem (e.name, true, e.file == currentFile, [this, e] { apply (e); });
        }
    menu.addSeparator();
    menu.addItem (tr ("Save preset..."), [this] { saveAs(); });
    menu.addItem (tr ("Load preset file..."), [this] { loadFile(); });
    menu.addItem (tr ("Export preset file..."), [this] { exportFile(); });
    if (currentFile.existsAsFile())
        menu.addItem (tr ("Delete preset \"{0}\"", { currentFile.getFileNameWithoutExtension() }), [this] {
            currentFile.deleteFile();
            currentFile = juce::File();
            setName ({});
        });
    menu.addItem (tr ("Show the presets folder"), [this] {
        const auto folder = folderFor (plugin.getName());
        folder.createDirectory();
        folder.revealToUser();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
}

void PresetBar::saveAs()
{
    auto* window = new juce::AlertWindow (tr ("Save preset"), tr ("Name of the preset:"), juce::MessageBoxIconType::NoIcon, this);
    window->addTextEditor ("name", current.isEmpty() ? tr ("My preset") : current);
    window->addButton (tr ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<PresetBar> safe (this);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window] (int result) {
        const auto name = window->getTextEditorContents ("name").trim();
        if (result != 1 || name.isEmpty() || safe == nullptr)
            return;
        const auto file = folderFor (safe->plugin.getName()).getChildFile (juce::File::createLegalFileName (name) + pp::extension);
        juce::String error;
        if (! save (safe->plugin, file, name, error))
        {
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Save preset"), error);
            return;
        }
        safe->currentFile = file;
        safe->currentProgram = -1;
        safe->setName (name);
    }), true);
}

void PresetBar::loadFile()
{
    chooser = std::make_unique<juce::FileChooser> (tr ("Load preset"), folderFor (plugin.getName()), "*.lynxpreset;*.fxp;*.fxb;*.vstpreset");
    juce::Component::SafePointer<PresetBar> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
        if (safe != nullptr && fc.getResult().existsAsFile())
            safe->apply ({ -1, fc.getResult(), fc.getResult().getFileNameWithoutExtension() });
    });
}

void PresetBar::exportFile()
{
    const auto start = folderFor (plugin.getName()).getChildFile (juce::File::createLegalFileName (current.isEmpty() ? plugin.getName() : current) + pp::extension);
    chooser = std::make_unique<juce::FileChooser> (tr ("Export preset"), start, saveWildcard (plugin));
    juce::Component::SafePointer<PresetBar> safe (this);
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe] (const juce::FileChooser& fc) {
                              auto file = fc.getResult();
                              if (safe == nullptr || file == juce::File())
                                  return;
                              if (! isPresetFile (file))
                                  file = file.withFileExtension ("lynxpreset");
                              juce::String error;
                              if (! save (safe->plugin, file, file.getFileNameWithoutExtension(), error))
                                  juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Export preset"), error);
                          });
}
} // namespace lynx::pluginpresetui
