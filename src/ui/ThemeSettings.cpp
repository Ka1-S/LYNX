#include "ui/ThemeSettings.h"

#include <map>

#include <juce_audio_formats/juce_audio_formats.h>

#include "platform/Paths.h"
#include "ui/ColourPicker.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/WindowOwner.h"

namespace lynx::themeui
{
namespace
{
themes::Spec& state()
{
    static themes::Spec spec;
    return spec;
}

juce::File savedFile()
{
    return platform::settingsFile ("theme" + juce::String (fileExtension));
}

// Colours that components set on themselves from the old tokens (setColour (…, theme::colour::accent)) follow the
// new theme: every colour property equal to an old token becomes the matching new one.
void remapComponentColours (juce::Component& c, const std::map<std::uint32_t, std::uint32_t>& remap)
{
    std::vector<std::pair<int, juce::Colour>> changes;
    const auto& props = c.getProperties();
    for (int i = 0; i < props.size(); ++i)
    {
        const auto name = props.getName (i).toString();
        if (! name.startsWith ("jcclr_"))
            continue;
        const auto argb = (std::uint32_t) (int) props.getValueAt (i);
        if (auto it = remap.find (argb); it != remap.end())
            changes.emplace_back (name.substring (6).getHexValue32(), juce::Colour (it->second));
    }
    for (const auto& [id, colour] : changes)
        c.setColour (id, colour);
    for (auto* child : c.getChildren())
        remapComponentColours (*child, remap);
}

void refreshOpenWindows (const themes::Palette& before, const themes::Palette& after)
{
    if (auto* look = dynamic_cast<theme::LookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
        look->refreshColours();

    std::map<std::uint32_t, std::uint32_t> remap;
    const auto* a = reinterpret_cast<const std::uint32_t*> (&before);
    const auto* b = reinterpret_cast<const std::uint32_t*> (&after);
    static_assert (sizeof (themes::Palette) % sizeof (std::uint32_t) == 0);
    for (std::size_t i = 0; i < sizeof (themes::Palette) / sizeof (std::uint32_t); ++i)
        if (a[i] != b[i])
            remap.emplace (a[i], b[i]); // (a value two tokens share maps to the first one's new value)

    auto& desktop = juce::Desktop::getInstance();
    for (int i = 0; i < desktop.getNumComponents(); ++i)
        if (auto* top = desktop.getComponent (i))
        {
            if (! remap.empty())
                remapComponentColours (*top, remap);
            applyWindowChrome (*top);
            top->sendLookAndFeelChange();
            top->repaint();
        }
}

// the workspace image, decoded once and scaled to the area it was last drawn in
struct Backdrop
{
    juce::String path;
    juce::Image source, scaled;
    juce::Rectangle<int> scaledFor;
    themes::ImageFit scaledFit = themes::ImageFit::fill;
    float scaledOpacity = -1.0f, scaledDim = -1.0f;
    std::uint32_t scaledColour = 0;
};

Backdrop& backdrop()
{
    static Backdrop b;
    return b;
}
} // namespace

const themes::Spec& current()
{
    return state();
}

juce::ChangeBroadcaster& changes()
{
    static juce::ChangeBroadcaster broadcaster;
    return broadcaster;
}

void apply (const themes::Spec& spec, bool save)
{
    const auto before = theme::currentPalette();
    state() = spec;
    const auto after = themes::derivePalette (spec.accent, spec.background);
    theme::setPalette (after);
    refreshOpenWindows (before, after);
    if (save)
    {
        if (spec == themes::Spec {})
            savedFile().deleteFile(); // the default look: nothing to keep
        else
            saveFile (spec, savedFile());
    }
    changes().sendChangeMessage();
}

void loadSaved()
{
    if (auto spec = loadFile (savedFile()))
    {
        state() = *spec;
        theme::setPalette (themes::derivePalette (spec->accent, spec->background));
        if (auto* look = dynamic_cast<theme::LookAndFeel*> (&juce::LookAndFeel::getDefaultLookAndFeel()))
            look->refreshColours();
    }
}

juce::String toJson (const themes::Spec& spec)
{
    auto* o = new juce::DynamicObject();
    o->setProperty ("format", "LYNX theme");
    o->setProperty ("version", 1);
    o->setProperty ("name", juce::String::fromUTF8 (spec.name.c_str()));
    o->setProperty ("accent", juce::String (themes::toHex (spec.accent)));
    o->setProperty ("background", juce::String (themes::toHex (spec.background)));
    o->setProperty ("workspace", spec.workspace == 0 ? juce::String ("auto") : juce::String (themes::toHex (spec.workspace)));
    o->setProperty ("image", juce::String::fromUTF8 (spec.image.c_str()));
    o->setProperty ("fit", juce::String (themes::imageFitId (spec.fit)));
    o->setProperty ("imageOpacity", spec.imageOpacity);
    o->setProperty ("dim", spec.dim);
    if (! spec.startupSound.empty())
        o->setProperty ("startupSound", juce::String::fromUTF8 (spec.startupSound.c_str()));
    return juce::JSON::toString (juce::var (o));
}

std::optional<themes::Spec> fromJson (const juce::String& json)
{
    const auto v = juce::JSON::parse (json);
    if (! v.isObject())
        return std::nullopt;
    themes::Spec spec;
    std::uint32_t argb = 0;
    if (! themes::parseHex (v["accent"].toString().toStdString(), argb))
        return std::nullopt; // not a theme
    spec.accent = argb;
    if (themes::parseHex (v["background"].toString().toStdString(), argb))
        spec.background = argb;
    if (themes::parseHex (v["workspace"].toString().toStdString(), argb))
        spec.workspace = argb;
    if (v["name"].toString().isNotEmpty())
        spec.name = v["name"].toString().toStdString();
    spec.image = v["image"].toString().toStdString();
    spec.fit = themes::imageFitFromId (v["fit"].toString().toStdString());
    if (! v["imageOpacity"].isVoid())
        spec.imageOpacity = juce::jlimit (0.0f, 1.0f, (float) v["imageOpacity"]);
    if (! v["dim"].isVoid())
        spec.dim = juce::jlimit (0.0f, 1.0f, (float) v["dim"]);
    spec.startupSound = v["startupSound"].toString().toStdString();
    return spec;
}

bool saveFile (const themes::Spec& spec, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (toJson (spec));
}

std::optional<themes::Spec> loadFile (const juce::File& file)
{
    if (! file.existsAsFile() || file.getSize() > 64 * 1024)
        return std::nullopt;
    return fromJson (file.loadFileAsString());
}

juce::File userThemesFolder()
{
    return platform::settingsFile ("Themes");
}

std::function<void (const juce::String&)> previewStartupSound;

juce::File backgroundsFolder()
{
    static const juce::File folder = [] {
        if (const auto profile = platform::profileOverride(); profile != juce::File())
            return profile.getChildFile ("Backgrounds");
        // (a probe file: Windows reports Program Files as writable and then virtualises the writes)
        const auto beside = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("Backgrounds");
        if (beside.isDirectory() || beside.createDirectory().wasOk())
        {
            const auto probe = beside.getChildFile (".lynx-write-test");
            const auto ok = probe.replaceWithText ("x") && probe.existsAsFile();
            probe.deleteFile();
            if (ok)
                return beside;
        }
        return platform::settingsFile ("Backgrounds");
    }();
    folder.createDirectory();
    return folder;
}

juce::File imageFile (const std::string& image)
{
    if (image.empty())
        return {};
    const auto path = juce::String::fromUTF8 (image.c_str());
    return juce::File::isAbsolutePath (path) ? juce::File (path) : backgroundsFolder().getChildFile (path);
}

std::string imagePathFor (const juce::File& file)
{
    const auto folder = backgroundsFolder();
    if (file.isAChildOf (folder))
        return file.getRelativePathFrom (folder).replaceCharacter ('\\', '/').toStdString();
    return file.getFullPathName().toStdString();
}

void paintWorkspaceBackdrop (juce::Graphics& g, juce::Rectangle<int> area)
{
    const auto& spec = state();
    const auto colour = juce::Colour (themes::workspaceColour (spec));
    g.setColour (colour);
    g.fillRect (area);
    if (spec.image.empty() || area.isEmpty())
        return;

    auto& b = backdrop();
    const auto file = imageFile (spec.image);
    const auto path = file.getFullPathName();
    if (b.path != path)
    {
        b = {};
        b.path = path;
        if (file.existsAsFile() && file.getSize() < 64 * 1024 * 1024)
            b.source = juce::ImageFileFormat::loadFrom (file);
    }
    if (! b.source.isValid())
        return;

    // the image as it is drawn, made again only when the area or the settings change (a big photo scaled on every
    // repaint would cost the CPU renderers a lot)
    if (b.scaledFor != area || b.scaledFit != spec.fit || b.scaledOpacity != spec.imageOpacity || b.scaledDim != spec.dim
        || b.scaledColour != colour.getARGB() || ! b.scaled.isValid())
    {
        b.scaledFor = area;
        b.scaledFit = spec.fit;
        b.scaledOpacity = spec.imageOpacity;
        b.scaledDim = spec.dim;
        b.scaledColour = colour.getARGB();
        b.scaled = juce::Image (juce::Image::ARGB, juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), true);
        juce::Graphics ig (b.scaled);
        ig.setImageResamplingQuality (juce::Graphics::highResamplingQuality);
        const auto local = b.scaled.getBounds().toFloat();
        const auto src = b.source.getBounds().toFloat();
        ig.setOpacity (juce::jlimit (0.0f, 1.0f, spec.imageOpacity));
        switch (spec.fit)
        {
            case themes::ImageFit::fill:
            {
                const auto scale = juce::jmax (local.getWidth() / src.getWidth(), local.getHeight() / src.getHeight());
                ig.drawImageTransformed (b.source,
                                         juce::AffineTransform::scale (scale).translated ((local.getWidth() - src.getWidth() * scale) * 0.5f,
                                                                                          (local.getHeight() - src.getHeight() * scale) * 0.5f));
                break;
            }
            case themes::ImageFit::fit:
                ig.drawImageWithin (b.source, 0, 0, b.scaled.getWidth(), b.scaled.getHeight(), juce::RectanglePlacement::centred);
                break;
            case themes::ImageFit::stretch:
                ig.drawImageWithin (b.source, 0, 0, b.scaled.getWidth(), b.scaled.getHeight(), juce::RectanglePlacement::stretchToFit);
                break;
            case themes::ImageFit::tile:
                ig.setTiledImageFill (b.source, 0, 0, juce::jlimit (0.0f, 1.0f, spec.imageOpacity));
                ig.fillAll();
                break;
            case themes::ImageFit::centre:
                ig.drawImageAt (b.source, (b.scaled.getWidth() - b.source.getWidth()) / 2, (b.scaled.getHeight() - b.source.getHeight()) / 2);
                break;
        }
        if (spec.dim > 0.0f)
        {
            ig.setOpacity (1.0f);
            ig.setColour (colour.withAlpha (juce::jlimit (0.0f, 1.0f, spec.dim)));
            ig.fillAll();
        }
    }
    g.drawImageAt (b.scaled, area.getX(), area.getY());
}

// --- Options › Theme ---------------------------------------------------------------------------------------------------
namespace
{
using namespace settingsui;

// a colour well: the colour, its name and hex; the selected one is edited by the picker below
class ColourWell final : public juce::Button
{
public:
    explicit ColourWell (juce::String label) : juce::Button (label) { setClickingTogglesState (false); }

    juce::Colour colour;
    juce::String note; // e.g. "automatic"

    void paintButton (juce::Graphics& g, bool over, bool) override
    {
        using namespace theme;
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (getToggleState() ? colour::bg5 : over ? colour::bg4 : colour::bg3);
        g.fillRoundedRectangle (r, radius::md);
        if (getToggleState())
        {
            g.setColour (colour::accent);
            g.drawRoundedRectangle (r.reduced (0.5f), radius::md, 1.5f);
        }
        auto inner = getLocalBounds().reduced (8, 6);
        const auto swatch = inner.removeFromLeft (inner.getHeight()).toFloat();
        g.setColour (this->colour);
        g.fillRoundedRectangle (swatch, radius::sm);
        g.setColour (colour::borderStrong);
        g.drawRoundedRectangle (swatch, radius::sm, 1.0f);
        inner.removeFromLeft (8);
        g.setColour (colour::textPrimary);
        g.setFont (font (type::body, true));
        g.drawText (getButtonText(), inner.removeFromTop (inner.getHeight() / 2), juce::Justification::bottomLeft, true);
        g.setColour (colour::textSecondary);
        g.setFont (monoFont (type::small));
        g.drawText (note.isNotEmpty() ? note : juce::String (themes::toHex (this->colour.getARGB())), inner, juce::Justification::topLeft, true);
    }
};

class ThemePage final : public ScrollingPage, private juce::Timer
{
public:
    ThemePage() : spec (current())
    {
        for (auto* c : std::initializer_list<juce::Component*> { &presetTitle, &presetBox, &importButton, &exportButton, &savePreset, &resetButton,
                                                                 &presetNote, &coloursTitle, &accentWell, &backgroundWell, &workspaceWell, &autoWorkspace,
                                                                 &picker, &contrastNote, &imageTitle, &imageButton, &removeImage, &imageName, &fitLabel,
                                                                 &fitBox, &opacityLabel, &opacitySlider, &dimLabel, &dimSlider, &imageListLabel, &imageList,
                                                                 &openBackgrounds, &imageNote, &soundTitle, &soundName, &soundButton, &soundPlay,
                                                                 &soundDefault, &soundNote })
            add (*c);

        // the Backgrounds folder next to LYNX.exe (issue #12): its images in a list, the folder a click away
        styleLabel (imageListLabel, tr ("Backgrounds folder"));
        imageList.onChange = [this] {
            const auto i = imageList.getSelectedItemIndex();
            if (i >= 0 && i < backgroundFiles.size())
                edit ([path = imagePathFor (backgroundFiles[i])] (themes::Spec& s) { s.image = path; });
        };
        openBackgrounds.onClick = [] { backgroundsFolder().startAsProcess(); };
        openBackgrounds.setTooltip (tr ("Open the Backgrounds folder: images put there show in the list (refreshed when this page opens)"));
        styleNote (imageNote, tr ("Images you choose are copied into the Backgrounds folder next to LYNX.exe, so a portable LYNX SHARE keeps them, "
                                  "and a saved theme preset keeps its image."));

        // Startup sound (issue #12): any audio file instead of LYNX's own sound
        soundButton.onClick = [this] { chooseSound(); };
        soundPlay.onClick = [this] {
            if (previewStartupSound)
                previewStartupSound (juce::String::fromUTF8 (spec.startupSound.c_str()));
        };
        soundDefault.onClick = [this] { edit ([] (themes::Spec& s) { s.startupSound.clear(); }); };
        styleNote (soundNote, tr ("Played when LYNX SHARE starts (if Settings › General › \"Play the startup sound\" is on). Default: LYNX SHARE's own sound."));

        presetBox.onChange = [this] { choosePreset(); };
        importButton.onClick = [this] { importTheme(); };
        exportButton.onClick = [this] { exportTheme (false); };
        savePreset.onClick = [this] { exportTheme (true); };
        resetButton.onClick = [this] { edit ([] (themes::Spec& s) { s = themes::Spec {}; }); };
        styleNote (presetNote, tr ("Themes change the accent color, the background and the workspace behind the panels. Your own presets are "
                                   "saved as .lynxtheme files and can be shared."));
        importButton.setTooltip (tr ("Load a .lynxtheme file"));
        exportButton.setTooltip (tr ("Save the current theme as a .lynxtheme file"));
        savePreset.setTooltip (tr ("Keep the current theme in the preset list"));

        for (auto* w : { &accentWell, &backgroundWell, &workspaceWell })
            w->onClick = [this, w] { select (w); };
        autoWorkspace.setButtonText (tr ("Automatic workspace color (follows the background)"));
        autoWorkspace.onClick = [this] {
            const auto on = autoWorkspace.getToggleState();
            edit ([on] (themes::Spec& s) { s.workspace = on ? 0u : themes::workspaceColour (s); });
        };
        picker.onChange = [this] (juce::Colour c) {
            const auto argb = c.getARGB();
            edit ([this, argb] (themes::Spec& s) {
                if (selected == &accentWell)
                    s.accent = argb;
                else if (selected == &backgroundWell)
                    s.background = argb;
                else
                    s.workspace = argb;
            }, false);
        };

        imageButton.onClick = [this] { chooseImage(); };
        removeImage.onClick = [this] { edit ([] (themes::Spec& s) { s.image.clear(); }); };
        styleLabel (fitLabel, tr ("Image fit"));
        const juce::String fits[] = { tr ("Fill (crop to cover)"), tr ("Fit (whole image)"), tr ("Stretch"), tr ("Tile"), tr ("Center") };
        for (int i = 0; i < themes::imageFitCount; ++i)
            fitBox.addItem (fits[i], i + 1);
        fitBox.onChange = [this] { edit ([this] (themes::Spec& s) { s.fit = (themes::ImageFit) juce::jlimit (0, themes::imageFitCount - 1, fitBox.getSelectedId() - 1); }); };
        styleLabel (opacityLabel, tr ("Image opacity"));
        styleLabel (dimLabel, tr ("Dim"));
        for (auto* s : { &opacitySlider, &dimSlider })
        {
            s->setRange (0.0, 100.0, 1.0);
            s->setTextValueSuffix ("%");
            s->setTextBoxStyle (juce::Slider::TextBoxRight, false, 52, 20);
        }
        opacitySlider.onValueChange = [this] { edit ([v = (float) opacitySlider.getValue() / 100.0f] (themes::Spec& s) { s.imageOpacity = v; }, false); };
        dimSlider.onValueChange = [this] { edit ([v = (float) dimSlider.getValue() / 100.0f] (themes::Spec& s) { s.dim = v; }, false); };
        styleNote (imageName, {});

        refresh();
        select (&accentWell);
    }

    ~ThemePage() override
    {
        if (isTimerRunning()) // a change still waiting for its save
            flush();
    }

private:
    int layout (int width) override
    {
        auto area = juce::Rectangle<int> (0, 0, width, 10000).reduced (20, 14);
        const auto top = area.getY();
        auto row = [&area] (int h) {
            auto r = area.removeFromTop (h);
            area.removeFromTop (6);
            return r;
        };
        presetTitle.setBounds (row (24));
        {
            auto r = row (28);
            presetBox.setBounds (r.removeFromLeft (juce::jmin (200, r.getWidth() / 3)));
            r.removeFromLeft (8);
            const auto w = (r.getWidth() - 12) / 4;
            for (auto* b : { &importButton, &exportButton, &savePreset, &resetButton })
            {
                b->setBounds (r.removeFromLeft (w));
                r.removeFromLeft (4);
            }
        }
        presetNote.setBounds (row (32));

        coloursTitle.setBounds (row (24));
        {
            auto r = row (44);
            const auto w = (r.getWidth() - 16) / 3;
            accentWell.setBounds (r.removeFromLeft (w));
            r.removeFromLeft (8);
            backgroundWell.setBounds (r.removeFromLeft (w));
            r.removeFromLeft (8);
            workspaceWell.setBounds (r);
        }
        picker.setBounds (row (picker.preferredHeight()).withWidth (juce::jmin (area.getWidth(), 520)));
        autoWorkspace.setBounds (row (24));
        contrastNote.setBounds (row (18));

        imageTitle.setBounds (row (24));
        {
            auto r = row (28);
            imageButton.setBounds (r.removeFromLeft (150));
            r.removeFromLeft (8);
            removeImage.setBounds (r.removeFromLeft (90));
            r.removeFromLeft (10);
            imageName.setBounds (r.withTrimmedTop (6));
        }
        {
            auto r = row (26);
            imageListLabel.setBounds (r.removeFromLeft (130));
            imageList.setBounds (r.removeFromLeft (juce::jmin (300, r.getWidth() - 140)));
            r.removeFromLeft (8);
            openBackgrounds.setBounds (r.removeFromLeft (juce::jmin (130, r.getWidth())));
        }
        imageNote.setBounds (row (32));
        for (auto [label, control] : { std::pair<juce::Component*, juce::Component*> { &fitLabel, &fitBox }, { &opacityLabel, &opacitySlider }, { &dimLabel, &dimSlider } })
        {
            auto r = row (26);
            label->setBounds (r.removeFromLeft (130));
            control->setBounds (r.removeFromLeft (juce::jmin (300, r.getWidth())));
        }

        soundTitle.setBounds (row (24));
        {
            auto r = row (28);
            soundButton.setBounds (r.removeFromLeft (150));
            r.removeFromLeft (8);
            soundPlay.setBounds (r.removeFromLeft (80));
            r.removeFromLeft (8);
            soundDefault.setBounds (r.removeFromLeft (90));
            r.removeFromLeft (10);
            soundName.setBounds (r.withTrimmedTop (6));
        }
        soundNote.setBounds (row (32));
        return area.getY() - top + 28;
    }

    // a change from a control: applied at once (live), saved a moment later
    void edit (const std::function<void (themes::Spec&)>& change, bool refreshPicker = true)
    {
        change (spec);
        apply (spec, false);
        refresh (refreshPicker);
        startTimer (600);
    }

    void timerCallback() override { flush(); }

    void flush()
    {
        stopTimer();
        apply (spec, true);
    }

    void select (ColourWell* well)
    {
        selected = well;
        for (auto* w : { &accentWell, &backgroundWell, &workspaceWell })
            w->setToggleState (w == well, juce::dontSendNotification);
        picker.setOriginal (well->colour);
        picker.setColour (well->colour, false);
    }

    void refresh (bool refreshPicker = true)
    {
        accentWell.colour = juce::Colour (spec.accent);
        backgroundWell.colour = juce::Colour (spec.background);
        workspaceWell.colour = juce::Colour (themes::workspaceColour (spec));
        workspaceWell.note = spec.workspace == 0 ? tr ("Automatic") : juce::String();
        autoWorkspace.setToggleState (spec.workspace == 0, juce::dontSendNotification);
        if (refreshPicker && selected != nullptr)
            picker.setColour (selected->colour, false);
        for (auto* w : { &accentWell, &backgroundWell, &workspaceWell })
            w->repaint();

        const auto palette = themes::derivePalette (spec.accent, spec.background);
        const auto ratio = themes::contrastRatio (palette.onAccent, palette.accent);
        contrastNote.setText (tr ("Text on the accent: {0} (contrast {1}:1). Text on panels: contrast {2}:1.",
                                  { palette.onAccent == 0xffffffff ? tr ("white") : tr ("dark"), juce::String (ratio, 1),
                                    juce::String (themes::contrastRatio (palette.textPrimary, palette.bg2), 1) }),
                              juce::dontSendNotification);
        contrastNote.setFont (theme::font (theme::type::small));
        contrastNote.setColour (juce::Label::textColourId, theme::colour::textSecondary);

        // the preset list: built-in, then the user's own
        presetBox.clear (juce::dontSendNotification);
        presetFiles.clear();
        int id = 1, chosen = 0;
        for (const auto& t : themes::builtInThemes())
        {
            presetBox.addItem (juce::String::fromUTF8 (t.name.c_str()) + (id == 1 ? " " + tr ("(default)") : juce::String()), id);
            if (t.accent == spec.accent && t.background == spec.background && chosen == 0)
                chosen = id;
            ++id;
        }
        auto files = userThemesFolder().findChildFiles (juce::File::findFiles, false, juce::String ("*") + fileExtension);
        files.sort();
        if (! files.isEmpty())
            presetBox.addSeparator();
        for (const auto& f : files)
            if (auto t = loadFile (f))
            {
                presetFiles[id] = f;
                presetBox.addItem (juce::String::fromUTF8 (t->name.c_str()), id);
                if (*t == spec)
                    chosen = id;
                ++id;
            }
        if (chosen != 0)
            presetBox.setSelectedId (chosen, juce::dontSendNotification);
        else
            presetBox.setText (tr ("Custom"), juce::dontSendNotification);

        imageName.setText (spec.image.empty() ? tr ("No image") : imageFile (spec.image).getFileName(), juce::dontSendNotification);
        // the Backgrounds folder's images
        backgroundFiles = backgroundsFolder().findChildFiles (juce::File::findFiles, false, "*.png;*.jpg;*.jpeg;*.gif");
        backgroundFiles.sort();
        imageList.clear (juce::dontSendNotification);
        for (int i = 0; i < backgroundFiles.size(); ++i)
        {
            imageList.addItem (backgroundFiles[i].getFileName(), i + 1);
            if (! spec.image.empty() && backgroundFiles[i] == imageFile (spec.image))
                imageList.setSelectedId (i + 1, juce::dontSendNotification);
        }
        imageList.setTextWhenNothingSelected (backgroundFiles.isEmpty() ? tr ("(no images in the folder yet)") : tr ("Pick an image..."));
        imageList.setEnabled (! backgroundFiles.isEmpty());
        soundName.setText (spec.startupSound.empty() ? tr ("LYNX SHARE's own sound") : juce::File (juce::String::fromUTF8 (spec.startupSound.c_str())).getFileName(),
                           juce::dontSendNotification);
        soundDefault.setEnabled (! spec.startupSound.empty());
        removeImage.setEnabled (! spec.image.empty());
        fitBox.setSelectedId ((int) spec.fit + 1, juce::dontSendNotification);
        opacitySlider.setValue (std::round (spec.imageOpacity * 100.0f), juce::dontSendNotification);
        dimSlider.setValue (std::round (spec.dim * 100.0f), juce::dontSendNotification);
        for (auto* c : std::initializer_list<juce::Component*> { &fitBox, &opacitySlider, &dimSlider })
            c->setEnabled (! spec.image.empty());
    }

    void choosePreset()
    {
        const auto id = presetBox.getSelectedId();
        if (id <= 0)
            return;
        const auto& builtIn = themes::builtInThemes();
        if (id <= (int) builtIn.size())
        {
            const auto chosen = builtIn[(std::size_t) id - 1];
            // a preset sets the colours; the workspace image stays
            edit ([&chosen] (themes::Spec& s) {
                s.name = chosen.name;
                s.accent = chosen.accent;
                s.background = chosen.background;
                s.workspace = chosen.workspace;
            });
        }
        else if (auto it = presetFiles.find (id); it != presetFiles.end())
            if (auto t = loadFile (it->second))
                edit ([&t] (themes::Spec& s) { s = *t; });
        select (selected);
    }

    void importTheme()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Import theme"), userThemesFolder(), juce::String ("*") + fileExtension);
        juce::Component::SafePointer<ThemePage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile())
                return;
            if (auto t = loadFile (fc.getResult()))
            {
                safe->edit ([&t] (themes::Spec& s) { s = *t; });
                safe->select (safe->selected);
            }
            else
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Import theme"), tr ("This file is not a LYNX SHARE theme."));
        });
    }

    void exportTheme (bool asPreset)
    {
        juce::Component::SafePointer<ThemePage> safe (this);
        if (asPreset)
        {
            auto* window = new juce::AlertWindow (tr ("Save theme preset"), tr ("Name of the preset:"), juce::MessageBoxIconType::NoIcon);
            window->addTextEditor ("name", spec.name == "LYNX" ? tr ("My theme") : juce::String::fromUTF8 (spec.name.c_str()));
            window->addButton (tr ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
            window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
            window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window] (int result) {
                                         const auto name = window->getTextEditorContents ("name").trim();
                                         if (result != 1 || name.isEmpty() || safe == nullptr)
                                             return;
                                         safe->spec.name = name.toStdString();
                                         saveFile (safe->spec, userThemesFolder().getChildFile (juce::File::createLegalFileName (name) + fileExtension));
                                         safe->flush();
                                         safe->refresh();
                                     }),
                                     true);
            keepAboveMainWindow (*window);
            return;
        }
        chooser = std::make_unique<juce::FileChooser> (tr ("Export theme"),
                                                       userThemesFolder().getChildFile (juce::File::createLegalFileName (juce::String::fromUTF8 (spec.name.c_str())) + fileExtension),
                                                       juce::String ("*") + fileExtension);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || fc.getResult() == juce::File())
                return;
            saveFile (safe->spec, fc.getResult().withFileExtension (fileExtension));
            safe->refresh();
        });
    }

    void chooseImage()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Workspace image"), backgroundsFolder(), "*.png;*.jpg;*.jpeg;*.gif");
        juce::Component::SafePointer<ThemePage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile())
                return;
            if (! juce::ImageFileFormat::loadFrom (fc.getResult()).isValid())
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Workspace image"), tr ("LYNX SHARE can't read this image (PNG, JPEG or GIF)."));
                return;
            }
            // kept in the Backgrounds folder (issue #12): a copy, unless it is there already (the same file under that
            // name is reused)
            auto file = fc.getResult();
            const auto folder = backgroundsFolder();
            if (! file.isAChildOf (folder))
            {
                auto copy = folder.getChildFile (file.getFileName());
                if (copy.existsAsFile() && ! copy.hasIdenticalContentTo (file))
                    copy = folder.getNonexistentChildFile (file.getFileNameWithoutExtension(), file.getFileExtension(), true);
                if (copy.existsAsFile() || file.copyFileTo (copy))
                    file = copy;
            }
            safe->edit ([path = imagePathFor (file)] (themes::Spec& s) { s.image = path; });
        });
    }

    void chooseSound()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Startup sound"), juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        juce::Component::SafePointer<ThemePage> safe (this);
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe] (const juce::FileChooser& fc) {
            if (safe == nullptr || ! fc.getResult().existsAsFile())
                return;
            // a short sound: it plays in full at every start
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            const std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (fc.getResult()));
            if (reader == nullptr || reader->sampleRate <= 0.0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Startup sound"), tr ("LYNX SHARE can't read this audio file."));
                return;
            }
            if ((double) reader->lengthInSamples / reader->sampleRate > 30.0)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Startup sound"),
                                                        tr ("Choose a sound of 30 seconds or less: it plays in full every time LYNX SHARE starts."));
                return;
            }
            safe->edit ([path = fc.getResult().getFullPathName().toStdString()] (themes::Spec& s) { s.startupSound = path; });
            if (previewStartupSound)
                previewStartupSound (fc.getResult().getFullPathName());
        });
    }

    themes::Spec spec;
    SectionTitle presetTitle { tr ("Theme") }, coloursTitle { tr ("Colors") }, imageTitle { tr ("Workspace image") };
    juce::ComboBox presetBox;
    juce::TextButton importButton { tr ("Import...") }, exportButton { tr ("Export...") }, savePreset { tr ("Save preset...") },
        resetButton { tr ("Default") }, imageButton { tr ("Choose image...") }, removeImage { tr ("Remove") };
    juce::Label presetNote, contrastNote, imageName, fitLabel, opacityLabel, dimLabel, imageListLabel, imageNote, soundName, soundNote;
    juce::ComboBox imageList;
    juce::Array<juce::File> backgroundFiles;
    juce::TextButton openBackgrounds { tr ("Open folder") }, soundButton { tr ("Choose sound...") }, soundPlay { tr ("Play") },
        soundDefault { tr ("Default") };
    SectionTitle soundTitle { tr ("Startup sound") };
    ColourWell accentWell { tr ("Accent color") }, backgroundWell { tr ("Background") }, workspaceWell { tr ("Workspace") };
    ColourWell* selected = nullptr;
    juce::ToggleButton autoWorkspace;
    ui::ColourPicker picker;
    juce::ComboBox fitBox;
    juce::Slider opacitySlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight }, dimSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
    std::map<int, juce::File> presetFiles;
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

std::unique_ptr<juce::Component> makeThemePage()
{
    return std::make_unique<ThemePage>();
}
} // namespace lynx::themeui
