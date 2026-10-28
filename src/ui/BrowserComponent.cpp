#include "ui/PluginPresetMenu.h"
#include "ui/QuickPick.h"
#include "ui/BrowserComponent.h"
#include "ui/UiTrace.h"

#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "core/FlpImport.h"
#include "core/ProjectContents.h"
#include "core/SearchIndex.h"
#include "core/SearchQuery.h"
#include "core/SearchService.h"
#include "core/ValueMath.h"
#include "core/Vfx.h"
#include "platform/Paths.h"

#include <mutex>
#include "engine/PluginHost.h"
#include "engine/Text.h"
#include "ui/HintBar.h"
#include "ui/I18n.h"
#include "ui/Icons.h"
#include "ui/PlaylistComponent.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"
#include "ui/WaveformView.h"
#include "ui/AudioEditor.h"
#include "ui/AutomationUi.h"
#include "ui/ChannelRackComponent.h"
#include "ui/MixerComponent.h"
#include "ui/SoundFontEditor.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
bool isAudio (const juce::File& f)
{
    return f.hasFileExtension ("wav;flac;ogg;mp3;aif;aiff;wv");
}

bool isListed (const juce::File& f)
{
    return f.hasFileExtension ("wav;flac;ogg;mp3;aif;aiff;wv;mid;midi;lynx;flp;fst;sf2;sf3");
}

constexpr int maxResults = 500;

search::FileSystem diskFileSystem()
{
    search::FileSystem fs;
    fs.separator = juce::File::getSeparatorChar() == '\\' ? '\\' : '/';
    fs.folderTime = [] (const std::string& path) -> std::int64_t {
        const juce::File folder (juce::String::fromUTF8 (path.c_str()));
        return folder.isDirectory() ? folder.getLastModificationTime().toMilliseconds() : -1;
    };
    fs.list = [] (const std::string& path) {
        search::FolderListing listing;
        const juce::File folder (juce::String::fromUTF8 (path.c_str()));
        if (! folder.isDirectory())
            return listing;
        listing.exists = true;
        listing.time = folder.getLastModificationTime().toMilliseconds();
        auto children = folder.findChildFiles (juce::File::findFilesAndDirectories, false, "*");
        children.sort();
        for (const auto& c : children)
        {
            if (c.isDirectory())
            {
                // junctions and links (Windows' "My Music" inside Documents points back to itself) are not followed
                if (! c.isSymbolicLink())
                    listing.subfolders.push_back (c.getFileName().toStdString());
            }
            else if (isListed (c))
                listing.files.push_back (c.getFileName().toStdString());
        }
        return listing;
    };
    return fs;
}

std::vector<std::string> toPaths (const juce::Array<juce::File>& folders)
{
    std::vector<std::string> paths;
    for (const auto& f : folders)
        paths.push_back (f.getFullPathName().toStdString());
    return paths;
}

Icon iconFor (const juce::File& f, bool folder)
{
    if (folder)
        return Icon::folder;
    if (isAudio (f))
        return Icon::audioFile;
    if (f.hasFileExtension ("mid;midi"))
        return Icon::pianoRoll;
    if (f.hasFileExtension ("lynx;flp"))
        return Icon::project;
    if (f.hasFileExtension ("fst;sf2;sf3;lynxpreset;fxp;fxb;vstpreset"))
        return Icon::preset;
    return Icon::file;
}

// The hint for a file or folder row: what the mouse can do with it.
juce::String rowHint (const juce::File& f, bool folder, bool expanded)
{
    if (folder)
        return (expanded ? tr ("Folder: [LMB] close") : tr ("Folder: [LMB] open")) + "  " + tr ("[RMB] menu (collapse all, show in the file manager)");
    if (isAudio (f))
        return tr ("Sample: [LMB] preview  [2xLMB] new channel  drag to the Channel Rack or the Playlist  [Enter] into the selected channel  "
                   "[Shift][Up] / [Shift][Down] swap through the files");
    if (f.hasFileExtension ("mid;midi"))
        return tr ("MIDI file: [2xLMB] open it as a project");
    if (f.hasFileExtension ("lynx;flp"))
        return tr ("Project: [2xLMB] open it");
    if (f.hasFileExtension ("fst"))
        return tr ("Preset: [2xLMB] new channel with it (FL synth presets)  drag onto a mixer insert: effect preset");
    if (f.hasFileExtension ("lynxpreset;fxp;fxb;vstpreset"))
        return tr ("Plugin preset: [2xLMB] load it into its plugin (an open plugin window, or the selected channel's plugin)");
    if (f.hasFileExtension ("sf2;sf3"))
        return tr ("SoundFont: [LMB] preview its first preset  [2xLMB] new LYNX Soundfont channel (or into the selected one)  drag to the Channel Rack");
    return {};
}

// A user / channel colour made readable as text and icons on the Browser's dark rows.
juce::Colour readable (juce::Colour c)
{
    return c.withMultipliedSaturation (0.8f).withBrightness (juce::jlimit (0.72f, 0.92f, c.getBrightness() + 0.15f));
}

// The Current project's sections, in FL's order (REA_rodada115).
enum class Category
{
    history,
    patterns,
    effects,
    generators,
    automation,
    initialized, // FL's "Initialized controls" (REA round 146)
    samples
};

constexpr Category categories[] = { Category::history, Category::patterns, Category::effects, Category::generators,
                                     Category::automation, Category::initialized, Category::samples };

juce::String categoryName (Category c)
{
    switch (c)
    {
        case Category::history: return tr ("History");
        case Category::patterns: return tr ("Patterns");
        case Category::effects: return tr ("Effects");
        case Category::generators: return tr ("Generators");
        case Category::automation: return tr ("Automation");
        case Category::initialized: return tr ("Initialized controls");
        case Category::samples: return tr ("Audio files");
    }
    return {};
}

juce::String categoryHint (Category c)
{
    switch (c)
    {
        case Category::history: return tr ("Undo history of this project: [LMB] go back to that step");
        case Category::patterns: return tr ("Patterns of this project, in their colours");
        case Category::effects: return tr ("Effects in use, with the insert they sit on");
        case Category::generators: return tr ("Generators in use (the Channel Rack's channels), with the insert they play into");
        case Category::automation: return tr ("Automation clips of this project");
        case Category::initialized: return tr ("Initialized controls: to be reset when the song starts (a control's menu > Init song with this position)");
        case Category::samples: return tr ("Audio files used by this project (samples, audio clips), in the colour of the channel that plays them");
    }
    return {};
}

Icon categoryIcon (Category c)
{
    switch (c)
    {
        case Category::history: return Icon::undo;
        case Category::patterns: return Icon::pattern;
        case Category::effects: return Icon::mixer;
        case Category::generators: return Icon::keyboard;
        case Category::automation: return Icon::automate;
        case Category::initialized: return Icon::automate;
        case Category::samples: return Icon::audioFile;
    }
    return Icon::folder;
}

// Each top-level folder has its own colour (FL colours its Browser's folders by kind; LYNX uses its own set).
juce::Colour rootColour (int index)
{
    static const juce::Colour colours[] = { juce::Colour (0xff6ea8fe), juce::Colour (0xff3ddc84), juce::Colour (0xffe58fd0),
                                            juce::Colour (0xffb39ddb), juce::Colour (0xffb39ddb), juce::Colour (0xfff5b942),
                                            juce::Colour (0xff4cc9c0), juce::Colour (0xfff2a65a), juce::Colour (0xff9fb7d0) };
    return index >= 0 && index < (int) std::size (colours) ? colours[index] : juce::Colour (0xff8fd18c);
}
} // namespace

namespace
{
// The plugin being dragged from the Plugin database (one drag at a time; message thread).
struct DraggedPlugin
{
    std::function<std::optional<ChannelDef>()> generator;
    std::function<std::optional<EffectSlot>()> effect;
};
DraggedPlugin& draggedPlugin()
{
    static DraggedPlugin d;
    return d;
}
} // namespace

// --- search: the process's search worker ------------------------------------------------------------

// The Browser's search index and its worker (core/SearchService.h, round 162): one per process while a Browser exists,
// the index kept in the settings folder between sessions. Its news reach the Browsers through two broadcasters (they
// are thread-safe and coalesce); the worker is joined (never killed) before they go.
struct BrowserComponent::SearchShared
{
    juce::ChangeBroadcaster statusChanged, indexChanged;
    std::unique_ptr<search::Service> service; // (last: goes first)

    ~SearchShared() { service.reset(); }

    static juce::File file() { return platform::settingsFile ("browser-index.txt"); }

    static std::shared_ptr<SearchShared> get()
    {
        static std::weak_ptr<SearchShared> current; // (message thread)
        if (auto s = current.lock())
            return s;
        auto s = std::make_shared<SearchShared>();
        search::Service::Callbacks cb;
        cb.load = [] {
            const auto f = file();
            juce::MemoryBlock data;
            if (! f.existsAsFile() || f.getSize() > 256 * 1024 * 1024 || ! f.loadFileAsData (data))
                return std::string();
            return std::string (static_cast<const char*> (data.getData()), data.getSize());
        };
        cb.save = [] (const std::string& text) {
            const auto f = file();
            const auto temp = f.getSiblingFile (f.getFileName() + ".tmp");
            if (temp.replaceWithData (text.data(), text.size()))
                temp.moveFileTo (f);
        };
        cb.statusChanged = [raw = s.get()] { raw->statusChanged.sendChangeMessage(); };
        cb.indexChanged = [raw = s.get()] { raw->indexChanged.sendChangeMessage(); };
        s->service = std::make_unique<search::Service> (diskFileSystem(), std::move (cb));
        current = s;
        return s;
    }
};

// A click anywhere outside the search box (another panel, the toolbar, the Browser's own list) takes the keyboard
// back from it, so Space plays / stops again (issue #9; FL ends typing in its search field the same way).
class BrowserComponent::FocusWatcher final : public juce::MouseListener
{
public:
    explicit FocusWatcher (BrowserComponent& b) : browser (b) { juce::Desktop::getInstance().addGlobalMouseListener (this); }
    ~FocusWatcher() override { juce::Desktop::getInstance().removeGlobalMouseListener (this); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto* clicked = e.eventComponent;
        if (! browser.isTypingInSearch() || clicked == nullptr)
            return;
        if (clicked == &browser.searchBox || browser.searchBox.isParentOf (clicked))
            return;
        // a menu (the box's own right-click menu: Paste...) keeps it
        if (clicked->getTopLevelComponent() != browser.getTopLevelComponent() && clicked->getTopLevelComponent()->isAlwaysOnTop()
            && dynamic_cast<juce::ResizableWindow*> (clicked->getTopLevelComponent()) == nullptr)
            return;
        browser.leaveSearchBox();
    }

private:
    BrowserComponent& browser;
};

// --- the tree (and, flat, the search results) ---------------------------------------------------------

class BrowserComponent::FileTree final : public juce::Component,
                                         public ui::HintSource,
                                         public juce::FileDragAndDropTarget
{
public:
    static constexpr int rowHeight = 22;

    FileTree (BrowserComponent& b, bool isFlat) : browser (b), flat (isFlat)
    {
        setWantsKeyboardFocus (false);
        setMouseClickGrabsKeyboardFocus (false);
    }

    enum class Kind
    {
        fsFolder, // a folder on disk (top-level ones carry their own name and colour)
        file,     // a file on disk
        project,  // "Current project"
        category, // one of its sections
        item,     // something of the project (pattern, sample, generator, effect, automation clip, undo step)
        plugins,  // the Plugin database and its folders (round 142): built-in, FL engines, installed plugins
        plugin    // one of its plugins (double-click adds it)
    };

    struct Node
    {
        Kind kind = Kind::file;
        juce::File file;
        bool folder = false, expanded = false, loaded = false;
        int depth = 0;
        Node* parent = nullptr;
        std::vector<std::unique_ptr<Node>> children;

        juce::String label, detail; // display name (empty: the file's) and the dimmed detail after it
        juce::Colour colour;        // transparent: the default
        Icon icon = Icon::none;     // none: by file type
        Category category = Category::samples;
        projectcontents::Item item; // kind == item (history: item.index = the step)
        bool history = false, missing = false, current = false;
        bool initial = false; // an initialized control (item.index = into Song::initialValues)
        std::function<void()> activate; // kind == plugin: adds it
        // kind == plugin: what it adds (one of them), also when it is dragged (round 162)
        std::function<std::optional<ChannelDef>()> generator;
        std::function<std::optional<EffectSlot>()> effect;
        std::string pluginKey;  // kind == plugin: what a user name is stored under (ui/QuickPick.h, issue #11)
        juce::String original;  // kind == plugin: the name before the user renamed it
    };

    // tree mode: the given top-level nodes
    void setTopLevel (std::vector<std::unique_ptr<Node>> nodes)
    {
        root = std::make_unique<Node>();
        root->kind = Kind::fsFolder;
        root->folder = root->expanded = root->loaded = true;
        root->depth = -1;
        for (auto& n : nodes)
        {
            n->parent = root.get();
            n->depth = 0;
            if (n->expanded)
                load (*n);
            root->children.push_back (std::move (n));
        }
        selected = hovered = -1;
        rebuild();
    }

    static std::unique_ptr<Node> makeFolder (const juce::File& folder, const juce::String& label, juce::Colour colour)
    {
        auto n = std::make_unique<Node>();
        n->kind = Kind::fsFolder;
        n->file = folder;
        n->folder = true;
        n->label = label;
        n->colour = colour;
        return n;
    }

    // FL's "Plugin database", interpreted (round 142, issue #5): Generators and Effects, each with LYNX's built-in ones,
    // FL's engines (those the user's FL installation provides) and the installed VST3 / CLAP plugins
    static std::unique_ptr<Node> makePluginDatabase (BrowserComponent& browser, const std::set<juce::String>& open)
    {
        auto folder = [&open] (Node* parent, const juce::String& label, Icon icon) {
            auto n = std::make_unique<Node>();
            n->kind = Kind::plugins;
            n->folder = n->loaded = true;
            n->label = label;
            n->icon = icon;
            n->parent = parent;
            n->depth = parent != nullptr ? parent->depth + 1 : 0;
            n->expanded = open.count ((parent != nullptr && parent->kind == Kind::plugins ? parent->label + "/" : juce::String()) + label) > 0;
            return n;
        };
        auto node = [] (Node& parent, const juce::String& label, const juce::String& detail, Icon icon) -> Node& {
            auto n = std::make_unique<Node>();
            n->kind = Kind::plugin;
            n->label = label;
            n->detail = detail;
            n->icon = icon;
            n->parent = &parent;
            n->depth = parent.depth + 1;
            parent.children.push_back (std::move (n));
            return *parent.children.back();
        };
        auto root = folder (nullptr, tr ("Plugin database"), Icon::plugin);
        root->colour = theme::colour::info;
        auto safe = juce::Component::SafePointer<BrowserComponent> (&browser);
        // a generator / an effect: double-click adds it (to the rack / the selected insert), or drag it there
        auto generatorEntry = [&node, safe] (Node& parent, const juce::String& label, const juce::String& detail, Icon icon,
                                             std::function<std::optional<ChannelDef>()> make) {
            auto& n = node (parent, label, detail, icon);
            n.generator = make;
            n.activate = [safe, make] {
                if (auto c = make(); c && safe != nullptr && safe->onAddInstrument)
                    safe->onAddInstrument (std::move (*c));
            };
        };
        auto effectEntry = [&node, safe] (Node& parent, const juce::String& label, const juce::String& detail, Icon icon,
                                          std::function<std::optional<EffectSlot>()> make) {
            auto& n = node (parent, label, detail, icon);
            n.effect = make;
            n.activate = [safe, make] {
                if (auto fx = make(); fx && safe != nullptr && safe->onAddEffect)
                    safe->onAddEffect (std::move (*fx));
            };
        };
        // the entries (ui/QuickPick.h), each under the name the user gave it (issue #11: FL's renamed plugins, "1_EQ"),
        // sorted by that name in each folder
        auto& engine = browser.engine;
        auto named = [&engine] (const std::string& key, const juce::String& label) { return quickpick::shownName (engine, key, label); };
        auto byLabel = [] (Node& folderNode) {
            std::stable_sort (folderNode.children.begin(), folderNode.children.end(),
                              [] (const auto& a, const auto& b) { return a->label.compareNatural (b->label) < 0; });
        };

        // Generators
        {
            auto gens = folder (root.get(), tr ("Generators"), Icon::keyboard);
            auto lynxGens = folder (gens.get(), "LYNX", Icon::folder);
            auto flGens = folder (gens.get(), tr ("FL engines"), Icon::folder);
            // FL's VFX note processors (round 164b): channels with no sound, they play other channels / Patcher nodes
            auto vfxGens = folder (gens.get(), tr ("VFX (note effects)"), Icon::folder);
            auto installed = folder (gens.get(), tr ("Installed"), Icon::folder);
            Node* folders[] = { lynxGens.get(), flGens.get(), vfxGens.get(), installed.get() };
            for (auto& e : quickpick::generatorEntries (engine))
            {
                generatorEntry (*folders[juce::jlimit (0, 3, e.folder)], named (e.key, e.label), e.detail, e.folder == 3 ? Icon::plugin : Icon::keyboard, e.make);
                folders[juce::jlimit (0, 3, e.folder)]->children.back()->pluginKey = e.key;
                folders[juce::jlimit (0, 3, e.folder)]->children.back()->original = e.label;
            }
            for (auto* f : { &lynxGens, &flGens, &vfxGens, &installed })
                if (! (*f)->children.empty())
                {
                    byLabel (**f);
                    gens->children.push_back (std::move (*f));
                }
            root->children.push_back (std::move (gens));
        }
        // Effects
        {
            auto fxs = folder (root.get(), tr ("Effects"), Icon::mixer);
            auto lynxFx = folder (fxs.get(), "LYNX", Icon::folder);
            auto flFx = folder (fxs.get(), tr ("FL engines"), Icon::folder);
            auto installed = folder (fxs.get(), tr ("Installed"), Icon::folder);
            Node* folders[] = { lynxFx.get(), flFx.get(), installed.get(), installed.get() };
            for (auto& e : quickpick::effectEntries (engine))
            {
                auto* target = folders[juce::jlimit (0, 3, e.folder)];
                effectEntry (*target, named (e.key, e.label), e.detail, e.folder == 3 ? Icon::plugin : Icon::mixer, e.make);
                target->children.back()->pluginKey = e.key;
                target->children.back()->original = e.label;
            }
            for (auto* f : { &lynxFx, &flFx, &installed })
                if (! (*f)->children.empty())
                {
                    byLabel (**f);
                    fxs->children.push_back (std::move (*f));
                }
            root->children.push_back (std::move (fxs));
        }
        // (the counts, dimmed after each folder)
        std::function<int (Node&)> count = [&count] (Node& n) {
            int total = 0;
            for (auto& c : n.children)
                total += c->kind == Kind::plugin ? 1 : count (*c);
            if (n.kind == Kind::plugins)
                n.detail = juce::String (total);
            return total;
        };
        count (*root);
        return root;
    }

    static std::unique_ptr<Node> makeProject (bool open)
    {
        auto n = std::make_unique<Node>();
        n->kind = Kind::project;
        n->folder = true;
        n->expanded = open;
        n->label = tr ("Current project");
        n->icon = Icon::project;
        n->colour = theme::colour::accent;
        return n;
    }

    // flat mode: a list of files with their path relative to the folder searched that holds them
    void setFiles (const juce::Array<juce::File>& files, const juce::Array<juce::File>& newBases, const juce::String& newStatus)
    {
        bases = newBases;
        status = newStatus;
        flatNodes.clear();
        for (const auto& f : files)
        {
            auto n = std::make_unique<Node>();
            n->file = f;
            flatNodes.push_back (std::move (n));
        }
        selected = hovered = -1;
        rebuild();
    }

    // flat mode: project items (the PROJECT tab's search)
    void setNodes (std::vector<std::unique_ptr<Node>> nodes, const juce::String& newStatus)
    {
        bases.clear();
        status = newStatus;
        flatNodes = std::move (nodes);
        selected = hovered = -1;
        rebuild();
    }

    void setStatus (const juce::String& text)
    {
        status = text;
        repaint();
    }

    // Removes the folders on disk `drop` takes (round 164: the content filter found them empty), keeping what is open
    // and selected. False when nothing went.
    bool pruneFolders (const std::function<bool (const juce::File&)>& drop)
    {
        if (flat || root == nullptr)
            return false;
        auto* keep = juce::isPositiveAndBelow (selected, (int) rows.size()) ? rows[(std::size_t) selected] : nullptr;
        bool removed = false, lostSelection = false;
        std::function<void (Node&)> walk = [&] (Node& n) {
            for (auto it = n.children.begin(); it != n.children.end();)
            {
                auto& c = **it;
                if (c.kind == Kind::fsFolder && drop (c.file))
                {
                    for (auto* s = keep; s != nullptr; s = s->parent)
                        lostSelection = lostSelection || s == &c;
                    it = n.children.erase (it);
                    removed = true;
                    continue;
                }
                if (c.kind == Kind::fsFolder && c.loaded)
                    walk (c);
                ++it;
            }
        };
        walk (*root);
        if (! removed)
            return false;
        rebuild();
        selected = -1;
        if (! lostSelection)
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(std::size_t) i] == keep)
                    selected = i;
        hovered = -1;
        return true;
    }

    juce::File selectedFile() const
    {
        return juce::isPositiveAndBelow (selected, (int) rows.size()) ? rows[(std::size_t) selected]->file : juce::File();
    }

    // the top-level folder on disk that holds the selection (none: the selection is not a file)
    juce::File selectedRoot() const
    {
        if (! juce::isPositiveAndBelow (selected, (int) rows.size()))
            return {};
        for (auto* n = rows[(std::size_t) selected]; n != nullptr; n = n->parent)
            if (n->kind == Kind::fsFolder && n->depth == 0)
                return n->file;
        return {};
    }

    bool selectedIsFolder() const { return juce::isPositiveAndBelow (selected, (int) rows.size()) && rows[(std::size_t) selected]->folder; }
    int numRows() const { return (int) rows.size(); }
    int getSelectedRow() const { return selected; }

    void select (int row, bool preview)
    {
        row = rows.empty() ? -1 : juce::jlimit (0, (int) rows.size() - 1, row);
        if (row != selected)
        {
            repaintRow (selected);
            selected = row;
            repaintRow (selected);
        }
        scrollToRow (row);
        if (row >= 0)
            browser.fileSelected (rows[(std::size_t) row]->file, preview && ! rows[(std::size_t) row]->folder);
    }

    // Left / Right: close the selected folder (or go to its parent) / open it
    void openSelected (bool open)
    {
        if (! juce::isPositiveAndBelow (selected, (int) rows.size()))
            return;
        auto* n = rows[(std::size_t) selected];

        if (n->folder && n->expanded != open)
        {
            toggle (*n);
            return;
        }

        if (! open && n->parent != nullptr && n->parent != root.get())
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(std::size_t) i] == n->parent)
                {
                    select (i, false);
                    return;
                }
    }

    void collapseAll()
    {
        if (root == nullptr)
            return;
        std::function<void (Node&)> close = [&close] (Node& n) {
            for (auto& c : n.children)
            {
                c->expanded = false;
                close (*c);
            }
        };
        close (*root);
        selected = hovered = -1;
        rebuild();
    }

    void expandAll (Node& n)
    {
        n.expanded = true;
        load (n);
        if (n.kind == Kind::project || n.kind == Kind::category)
            for (auto& c : n.children)
                if (c->folder && c->kind != Kind::fsFolder)
                    expandAll (*c);
        rebuild();
    }

    // FL's folder "Expand all": the folder and its sub-folders open (on disk: down to `depth` levels, so a big
    // library is not scanned at once)
    // --snapshot: opens the top-level folder `f` and its sub-folders (round 164)
    void expandTop (const juce::File& f, int depth)
    {
        if (root != nullptr)
            for (auto& c : root->children)
                if (c->kind == Kind::fsFolder && c->file == f)
                    expandFolder (*c, depth);
    }

    void expandFolder (Node& n, int depth)
    {
        n.expanded = true;
        load (n);
        if (depth > 1)
            for (auto& c : n.children)
                if (c->folder)
                    expandFolder (*c, depth - 1);
        rebuild();
    }

    void rebuild()
    {
        rows.clear();

        if (flat)
            for (auto& n : flatNodes)
                rows.push_back (n.get());
        else if (root != nullptr)
            addRows (*root);

        updateSize();
        repaint();
    }

    void updateSize()
    {
        if (auto* parent = getParentComponent())
        {
            const auto width = parent->getWidth();
            const auto viewHeight = parent->getParentComponent() != nullptr ? parent->getParentComponent()->getHeight() : parent->getHeight();
            setSize (width, juce::jmax (viewHeight, (int) rows.size() * rowHeight));
        }
    }

    // The project changed: the open sections are listed again (only those whose items changed), keeping what is
    // open and selected.
    void refreshProject()
    {
        if (root == nullptr)
            return;

        bool changed = false;
        const auto key = selectionKey();
        std::function<void (Node&)> visit = [&] (Node& n) {
            if (n.kind == Kind::category)
            {
                Node fresh;
                fresh.kind = Kind::category;
                fresh.category = n.category;
                fresh.depth = n.depth;
                listCategory (fresh, browser.engine.getSong(), &browser.engine, ! n.loaded);
                if (n.detail != fresh.detail)
                {
                    n.detail = fresh.detail;
                    changed = true;
                }
                if (! n.loaded)
                    return;
                if (signature (fresh) != signature (n))
                {
                    n.children = std::move (fresh.children);
                    for (auto& c : n.children)
                        c->parent = &n;
                    changed = true;
                }
                else
                    for (std::size_t i = 0; i < n.children.size(); ++i) // the same items: only the "current" marks
                        n.children[i]->current = fresh.children[i]->current;
                return;
            }
            for (auto& c : n.children)
                if (c->kind == Kind::project || c->kind == Kind::category)
                    visit (*c);
        };
        visit (*root);
        pendingKey = key;

        if (changed)
        {
            rebuild();
            restoreSelection (pendingKey);
        }
        else
            repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme::colour;
        g.fillAll (bg2);
        const auto clip = g.getClipBounds();
        const auto first = juce::jmax (0, clip.getY() / rowHeight), last = juce::jmin ((int) rows.size() - 1, clip.getBottom() / rowHeight);
        const auto previewing = browser.isPreviewPlaying() ? browser.previewedFile : juce::File();

        for (int i = first; i <= last; ++i)
        {
            const auto& n = *rows[(std::size_t) i];
            auto r = juce::Rectangle<int> (0, i * rowHeight, getWidth(), rowHeight);

            // an open project section sits on a slightly raised band (FL draws its open folders on a band)
            if ((n.kind == Kind::project || n.kind == Kind::category) && n.expanded)
            {
                g.setColour (bg3);
                g.fillRect (r);
            }

            if (i == selected)
            {
                g.setColour (accentMuted);
                g.fillRect (r);
                g.setColour (accent);
                g.fillRect (r.withWidth (2));
            }
            else if (i == hovered)
            {
                g.setColour (bg4);
                g.fillRect (r);
            }

            const auto indent = flat ? 6 : 6 + n.depth * 14;
            auto x = (float) indent;

            if (n.folder) // a chevron: right when closed, down when open
            {
                juce::Path p;
                const auto c = juce::Point<float> (x + 5.0f, (float) r.getCentreY());
                if (n.expanded)
                    p.addTriangle (c.x - 3.5f, c.y - 2.0f, c.x + 3.5f, c.y - 2.0f, c.x, c.y + 2.5f);
                else
                    p.addTriangle (c.x - 2.0f, c.y - 3.5f, c.x - 2.0f, c.y + 3.5f, c.x + 2.5f, c.y);
                g.setColour (i == hovered || i == selected ? textPrimary : textSecondary);
                g.fillPath (p);
            }
            if (! flat || n.kind != Kind::item)
                x += 12.0f;

            const auto playing = ! n.folder && n.file != juce::File() && n.file == previewing;
            const auto hasColour = ! n.colour.isTransparent();
            auto iconColour = playing ? accent : hasColour ? readable (n.colour) : n.folder ? warning.withAlpha (0.85f) : textSecondary;
            if (n.missing)
                iconColour = meterClip;
            const auto icon = n.icon != Icon::none ? n.icon : iconFor (n.file, n.folder);
            icons::draw (g, icon, juce::Rectangle<float> (x, (float) r.getY() + 3.0f, 16.0f, 16.0f), iconColour);
            x += 21.0f;

            auto text = r.withLeft ((int) x).withTrimmedRight (6);

            if (flat && n.kind == Kind::file) // the folder it lives in, on the right
            {
                const auto where = n.file.getParentDirectory().getRelativePathFrom (baseOf (n.file));
                g.setFont (theme::font (theme::type::caption));
                g.setColour (textDisabled);
                const auto w = juce::jmin (text.getWidth() / 2, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), where) + 4);
                g.drawText (where, text.removeFromRight (w), juce::Justification::centredRight, true);
            }

            const auto name = n.label.isNotEmpty() ? n.label
                              : n.folder || ! isAudio (n.file) ? n.file.getFileName()
                                                               : n.file.getFileNameWithoutExtension();
            const auto bold = (n.folder && n.expanded) || n.current || n.kind == Kind::project;
            const juce::Font font (theme::font (theme::type::body, bold));
            g.setFont (font);

            auto nameColour = playing ? accentHover : n.folder || i == hovered || i == selected ? textPrimary : textPrimary.withAlpha (0.86f);
            if (n.kind == Kind::item && hasColour && n.item.kind == projectcontents::Kind::pattern)
                nameColour = readable (n.colour); // FL writes the patterns in their colours
            if (n.missing)
                nameColour = meterClip.withAlpha (0.9f);
            if (n.kind == Kind::item && n.history && ! n.current && n.item.index > browser.engine.getHistoryPosition())
                nameColour = textDisabled; // undone steps (redo)
            g.setColour (nameColour);

            const auto nameWidth = juce::GlyphArrangement::getStringWidthInt (font, name);
            g.drawText (name, text, juce::Justification::centredLeft, true);

            // the detail after the name, dimmed: "(insert)", "3x", "missing", a section's count
            if (n.detail.isNotEmpty() && nameWidth + 12 < text.getWidth())
            {
                g.setFont (theme::font (theme::type::small));
                g.setColour (n.missing ? meterClip.withAlpha (0.7f) : textDisabled);
                const auto shown = n.kind == Kind::category || n.kind == Kind::project ? n.detail : "(" + n.detail + ")";
                if (n.kind == Kind::category) // the count on the right
                    g.drawText (shown, text, juce::Justification::centredRight, true);
                else
                    g.drawText (shown, text.withTrimmedLeft (nameWidth + 6), juce::Justification::centredLeft, true);
            }
        }

        if (juce::isPositiveAndBelow (dropRow, (int) rows.size())) // files from the system would go here
        {
            g.setColour (accent);
            g.drawRect (juce::Rectangle<int> (0, dropRow * rowHeight, getWidth(), rowHeight), 2);
        }

        if (rows.empty() && status.isNotEmpty())
        {
            g.setColour (textDisabled);
            g.setFont (theme::font (theme::type::small));
            g.drawFittedText (status, getLocalBounds().reduced (10).withHeight (60), juce::Justification::centredTop, 3);
        }
    }

    int rowAt (int y) const
    {
        const auto row = y / rowHeight;
        return y >= 0 && row < (int) rows.size() ? row : -1;
    }

    // --- files dragged in from the system (issue #12): copied into the folder under the mouse ----------------------
    // The folder row a drop at y lands in: a folder on disk, or a file's own folder; -1 = nowhere to copy to.
    int dropFolderRow (int y) const
    {
        auto row = rowAt (y);
        if (row < 0)
            return -1;
        const Node* n = rows[(std::size_t) row];
        if (n->kind == Kind::file)
            n = n->parent;
        if (n == nullptr || n->kind != Kind::fsFolder || ! n->file.isDirectory())
            return -1;
        const auto it = std::find (rows.begin(), rows.end(), n);
        return it != rows.end() ? (int) (it - rows.begin()) : -1;
    }

    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        return ! flat && std::any_of (files.begin(), files.end(), [] (const juce::String& f) { return juce::File (f).existsAsFile(); });
    }

    void fileDragMove (const juce::StringArray&, int, int y) override
    {
        if (const auto row = dropFolderRow (y); row != dropRow)
        {
            dropRow = row;
            repaint();
        }
    }

    void fileDragExit (const juce::StringArray&) override
    {
        dropRow = -1;
        repaint();
    }

    void filesDropped (const juce::StringArray& files, int, int y) override
    {
        dropRow = -1;
        repaint();
        const auto row = dropFolderRow (y);
        if (row < 0)
        {
            browser.showNote (tr ("Drop the files on a folder to copy them there"));
            return;
        }
        auto* folderNode = rows[(std::size_t) row];
        const auto folder = folderNode->file;
        int copied = 0;
        juce::File first;
        for (const auto& path : files)
        {
            const juce::File source (path);
            if (! source.existsAsFile() || source.getParentDirectory() == folder)
                continue;
            // never over a file that is there: "Kick (2).wav"
            auto target = folder.getChildFile (source.getFileName());
            if (target.exists())
                target = folder.getNonexistentChildFile (source.getFileNameWithoutExtension(), source.getFileExtension(), true);
            if (source.copyFileTo (target))
            {
                ++copied;
                if (first == juce::File())
                    first = target;
            }
        }
        if (copied == 0)
        {
            browser.showNote (tr ("Nothing was copied to {0}", { folder.getFileName() }));
            return;
        }
        // the folder lists again, open, with the first copy selected
        folderNode->children.clear();
        folderNode->loaded = false;
        folderNode->expanded = true;
        load (*folderNode);
        rebuild();
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(std::size_t) i]->file == first)
            {
                select (i, false);
                break;
            }
        browser.showNote (copied == 1 ? tr ("Copied 1 file to {0}", { folder.getFileName() })
                                      : tr ("Copied {0} files to {1}", { juce::String (copied), folder.getFileName() }));
    }

    void mouseMove (const juce::MouseEvent& e) override { setHovered (rowAt (e.y)); }
    void mouseExit (const juce::MouseEvent&) override { setHovered (-1); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStarted = false;
        const auto row = rowAt (e.y);
        pressedRow = row;

        if (row < 0)
            return;

        auto& n = *rows[(std::size_t) row];

        if (e.mods.isPopupMenu())
        {
            select (row, false);
            showMenu (n);
            return;
        }

        if (n.folder)
        {
            select (row, false);
            if (e.getNumberOfClicks() == 1) // a click opens / closes it (the second click of a double click does not undo it)
                toggle (n);
            return;
        }

        if (n.kind == Kind::item)
        {
            select (row, n.item.kind == projectcontents::Kind::sample && ! n.missing);
            itemClicked (n);
            return;
        }
        if (n.kind == Kind::plugin)
        {
            select (row, false);
            return;
        }

        // a click previews, even when the file is already selected (click again: hear it again)
        select (row, true);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragStarted || e.getDistanceFromDragStart() < 5 || ! juce::isPositiveAndBelow (pressedRow, (int) rows.size()))
            return;

        const auto& n = *rows[(std::size_t) pressedRow];
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this);

        if (n.folder || container == nullptr)
            return;

        if (n.kind == Kind::plugin && (n.generator || n.effect)) // a plugin onto the Channel Rack / the Mixer (round 162)
        {
            dragStarted = true;
            draggedPlugin() = { n.generator, n.effect };
            container->startDragging (juce::String (n.generator ? generatorDragDescription : effectDragDescription) + "|" + n.label, this,
                                      juce::ScaledImage (dragImage (n)), true);
            return;
        }

        if (n.kind == Kind::item && n.item.kind == projectcontents::Kind::pattern) // a pattern onto the Playlist
        {
            dragStarted = true;
            container->startDragging (juce::String (patternDragDescription) + "|" + juce::String (n.item.index), this,
                                      juce::ScaledImage (dragImage (n)), true);
            return;
        }

        if (n.file.existsAsFile())
        {
            dragStarted = true;
            container->startDragging (juce::String (dragDescription) + "|" + n.file.getFullPathName(), this, juce::ScaledImage (dragImage (n)),
                                      true);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const auto row = rowAt (e.y);

        if (row < 0 || rows[(std::size_t) row]->folder || ! e.mods.isLeftButtonDown())
            return;

        const auto& n = *rows[(std::size_t) row];
        if (n.kind == Kind::plugin)
        {
            if (n.activate)
                n.activate();
            return;
        }
        if (n.kind != Kind::item)
        {
            browser.openFile (n.file);
            return;
        }

        const auto& item = n.item;
        switch (item.kind)
        {
            case projectcontents::Kind::pattern:
                if (browser.onOpenPattern)
                    browser.onOpenPattern (item.index);
                break;
            case projectcontents::Kind::sample:
                if (! n.missing)
                    browser.openFile (n.file);
                break;
            case projectcontents::Kind::generator:
                if (browser.onOpenChannel)
                    browser.onOpenChannel (item.index);
                break;
            case projectcontents::Kind::effect:
                if (browser.onOpenEffect)
                    browser.onOpenEffect (item.index, item.slot);
                break;
            case projectcontents::Kind::automation:
                if (! n.history && ! n.initial && browser.onOpenAutomation)
                    browser.onOpenAutomation (item.index);
                break;
        }
    }

    ui::Hint hintAt (juce::Point<int> p) override
    {
        ui::Hint h;
        const auto row = rowAt (p.y);

        if (row < 0)
        {
            h.text = flat ? tr ("Search results: files in the current folder and below that match") : tr ("Browser: [LMB] open a folder or preview a sample");
            return h;
        }

        const auto& n = *rows[(std::size_t) row];
        h.hasMenu = true;

        switch (n.kind)
        {
            case Kind::project:
                h.text = tr ("Current project: what this project uses (patterns, effects, generators, automation, audio files)  [LMB] open  [RMB] expand / collapse all");
                return h;
            case Kind::category:
                h.text = categoryHint (n.category) + "  " + (n.expanded ? tr ("[LMB] close") : tr ("[LMB] open"));
                h.value = n.detail;
                return h;
            case Kind::fsFolder:
                if (n.depth == 0 || (n.parent != nullptr && n.parent->kind == Kind::project))
                {
                    h.text = n.label + ": " + n.file.getFullPathName() + "  " + (n.expanded ? tr ("[LMB] close") : tr ("[LMB] open")) + "  " + tr ("[RMB] menu");
                    return h;
                }
                break;
            case Kind::item:
                return itemHint (n);
            case Kind::plugins:
                h.text = tr ("Plugin database: LYNX's built-in generators and effects, FL's engines and the installed plugins") + "  "
                         + (n.expanded ? tr ("[LMB] close") : tr ("[LMB] open"));
                h.value = n.detail;
                return h;
            case Kind::plugin:
                h.text = n.label + " (" + n.detail + ")  "
                         + (n.generator ? tr ("[double-click] add it as a new channel, or drag it onto the Channel Rack")
                                        : tr ("[double-click] add it to the selected insert, or drag it onto a mixer insert or an effect slot"));
                h.hasMenu = true;
                return h;
            case Kind::file:
                break;
        }

        h.text = rowHint (n.file, n.folder, n.expanded);
        h.value = n.folder ? juce::String() : n.file.getFileExtension().substring (1).toUpperCase();
        return h;
    }

    // for snapshots: the first audio file under the first folders on disk (opening them)
    int firstAudioRow()
    {
        for (int pass = 0; pass < 3; ++pass)
        {
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(std::size_t) i]->kind == Kind::file && isAudio (rows[(std::size_t) i]->file))
                    return i;

            for (auto* n : rows)
                if (n->kind == Kind::fsFolder && ! n->expanded)
                {
                    toggle (*n);
                    break;
                }
        }
        return -1;
    }

    // the first row labelled `label`, or -1
    int rowLabelled (const juce::String& label) const
    {
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(std::size_t) i]->label == label)
                return i;
        return -1;
    }

    // for snapshots: opens these project sections; returns the first row of a sample item
    int openProjectSections (std::initializer_list<Category> open)
    {
        for (auto* n : std::vector<Node*> (rows))
            if (n->kind == Kind::category && std::find (open.begin(), open.end(), n->category) != open.end() && ! n->expanded)
            {
                n->expanded = true;
                load (*n);
            }
        rebuild();
        for (int i = 0; i < (int) rows.size(); ++i)
            if (rows[(std::size_t) i]->kind == Kind::item && rows[(std::size_t) i]->item.kind == projectcontents::Kind::sample)
                return i;
        return -1;
    }

    void setHovered (int row)
    {
        if (row != hovered)
        {
            repaintRow (hovered);
            hovered = row;
            repaintRow (hovered);
        }
    }

    // flat mode, PROJECT tab search: every project item whose name matches
    static std::vector<std::unique_ptr<Node>> projectMatches (const Song& song, const juce::String& query)
    {
        const auto parsed = search::parse (query.toStdString());
        std::vector<std::unique_ptr<Node>> found;
        for (auto c : categories)
        {
            if (c == Category::history)
                continue;
            Node section;
            section.kind = Kind::category;
            section.category = c;
            listCategory (section, song, {});
            for (auto& child : section.children)
                if (search::matches (parsed, child->label.toStdString()))
                {
                    child->parent = nullptr;
                    found.push_back (std::move (child));
                }
        }
        return found;
    }

private:
    juce::File baseOf (const juce::File& f) const
    {
        for (const auto& b : bases)
            if (f.isAChildOf (b))
                return b;
        return f.getParentDirectory();
    }

    void load (Node& n)
    {
        if (n.loaded)
            return;
        n.loaded = true;

        if (n.kind == Kind::project)
        {
            for (auto c : categories)
            {
                auto child = std::make_unique<Node>();
                child->kind = Kind::category;
                child->folder = true;
                child->category = c;
                child->label = categoryName (c);
                child->icon = categoryIcon (c);
                child->depth = n.depth + 1;
                child->parent = &n;
                listCategory (*child, browser.engine.getSong(), &browser.engine, true); // the counts show while closed
                child->children.clear();
                n.children.push_back (std::move (child));
                if (c == Category::initialized && browser.projectFile.existsAsFile()) // then the project's folder (FL's "Project data")
                {
                    auto folder = makeFolder (browser.projectFile.getParentDirectory(), tr ("Project folder"), juce::Colour());
                    folder->icon = Icon::folder;
                    folder->depth = n.depth + 1;
                    folder->parent = &n;
                    n.children.push_back (std::move (folder));
                }
            }
            return;
        }

        if (n.kind == Kind::category)
        {
            loadCategory (n);
            return;
        }

        auto folders = n.file.findChildFiles (juce::File::findDirectories, false, "*", juce::File::FollowSymlinks::noCycles);
        auto files = n.file.findChildFiles (juce::File::findFiles, false, "*", juce::File::FollowSymlinks::noCycles);
        auto byName = [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; };
        std::sort (folders.begin(), folders.end(), byName);
        std::sort (files.begin(), files.end(), byName);

        for (const auto& f : folders)
        {
            if (f.isHidden() || f.getFileName().startsWithChar ('.'))
                continue;
            if (browser.filterHides (f)) // nothing in it is listed under the content filter (round 164)
                continue;
            auto c = std::make_unique<Node>();
            c->kind = Kind::fsFolder;
            c->file = f;
            c->folder = true;
            c->depth = n.depth + 1;
            c->parent = &n;
            n.children.push_back (std::move (c));
        }

        for (const auto& f : files)
            if (browser.lists (f))
            {
                auto c = std::make_unique<Node>();
                c->file = f;
                c->depth = n.depth + 1;
                c->parent = &n;
                n.children.push_back (std::move (c));
            }
    }

    void loadCategory (Node& n)
    {
        n.loaded = true;
        n.children.clear();
        listCategory (n, browser.engine.getSong(), &browser.engine);
    }

    // The items of one project section (history needs the engine; the PROJECT search passes none).
    static void listCategory (Node& n, const Song& song, AudioEngine* engine, bool countOnly = false)
    {
        auto add = [&n] (projectcontents::Item item, Icon icon, juce::Colour colour) -> Node& {
            auto c = std::make_unique<Node>();
            c->kind = Kind::item;
            c->label = juce::String::fromUTF8 (item.name.c_str());
            c->detail = juce::String::fromUTF8 (item.detail.c_str());
            c->colour = colour;
            c->icon = icon;
            c->depth = n.depth + 1;
            c->parent = &n;
            c->item = std::move (item);
            n.children.push_back (std::move (c));
            return *n.children.back();
        };

        switch (n.category)
        {
            case Category::history:
                if (engine != nullptr)
                {
                    // each label compares two whole states: only listed while the section is open, the last 30
                    const auto size = engine->getHistorySize(), at = engine->getHistoryPosition();
                    for (int i = size - 1; ! countOnly && i >= juce::jmax (0, size - 30); --i) // newest first, as FL lists it
                    {
                        const auto l = engine->getHistoryLabel (i);
                        projectcontents::Item item;
                        item.kind = projectcontents::Kind::automation; // (unused for history rows)
                        item.index = i;
                        item.name = tr (l.text, { juce::String::fromUTF8 (l.arg.c_str()) }).toStdString();
                        auto& node = add (std::move (item), Icon::undo, juce::Colour());
                        node.history = true;
                        node.current = i == at;
                    }
                    n.detail = juce::String (size);
                }
                break;
            case Category::patterns:
                for (auto& item : projectcontents::patterns (song))
                {
                    const auto colour = PlaylistComponent::patternColour (song, item.index);
                    const auto current = item.index == song.currentPattern;
                    if (item.uses == 0)
                        item.detail = tr ("not in the Playlist").toStdString();
                    add (std::move (item), Icon::pattern, colour).current = current;
                }
                break;
            case Category::effects:
                for (auto& item : projectcontents::effects (song))
                    add (item, item.plugin ? Icon::plugin : Icon::mixer, item.colour != 0 ? juce::Colour (item.colour) : juce::Colour());
                break;
            case Category::generators:
                for (auto& item : projectcontents::generators (song))
                {
                    const auto& channel = song.channels[(std::size_t) item.index];
                    const auto icon = item.plugin ? Icon::plugin
                                      : projectcontents::playsSampleFile (channel.instrument) ? Icon::audioFile
                                                                                               : Icon::keyboard;
                    const auto colour = channel.instrument == InstrumentKind::audioClip ? PlaylistComponent::audioClipColour (channel)
                                        : item.colour != 0                               ? juce::Colour (item.colour)
                                                                                         : juce::Colour();
                    const auto selected = engine != nullptr && engine->getSelectedChannel() == item.index;
                    add (std::move (item), icon, colour).current = selected;
                }
                break;
            case Category::automation:
                for (auto& item : projectcontents::automations (song))
                    add (item, Icon::automate, item.colour != 0 ? juce::Colour (item.colour) : theme::colour::automation);
                break;
            case Category::initialized:
                for (int i = 0; i < (int) song.initialValues.size(); ++i)
                {
                    const auto& v = song.initialValues[(std::size_t) i];
                    projectcontents::Item item;
                    item.kind = projectcontents::Kind::automation;
                    item.index = i;
                    item.name = (engine != nullptr ? describeAutomationTarget (*engine, v.target) : juce::String ("?")).toStdString();
                    item.detail = std::to_string ((int) std::lround (v.value * 100.0f)) + "%";
                    add (std::move (item), Icon::automate, theme::colour::automation).initial = true;
                }
                break;
            case Category::samples:
                for (auto& item : projectcontents::samples (song))
                {
                    const juce::String path = juce::String::fromUTF8 (item.path.c_str());
                    const juce::File file (juce::File::isAbsolutePath (path) ? path : juce::String());
                    auto colour = item.colour != 0 ? juce::Colour (item.colour) : theme::colour::audioClip;
                    if (item.index >= 0 && song.channels[(std::size_t) item.index].instrument == InstrumentKind::audioClip)
                        colour = PlaylistComponent::audioClipColour (song.channels[(std::size_t) item.index]);
                    auto& node = add (std::move (item), Icon::audioFile, colour);
                    node.file = file;
                    node.missing = ! countOnly && ! file.existsAsFile();
                    if (node.missing)
                        node.detail = tr ("missing");
                }
                break;
        }

        if (n.category != Category::history)
            n.detail = juce::String ((int) n.children.size());
    }

    static juce::String signature (const Node& n)
    {
        juce::String s;
        for (const auto& c : n.children)
            s << c->label << '|' << c->detail << '|' << (int) c->colour.getARGB() << '|' << (int) c->missing << '|' << c->item.index << '|'
              << c->item.slot << '\n';
        return s;
    }

    juce::String selectionKey() const
    {
        if (! juce::isPositiveAndBelow (selected, (int) rows.size()))
            return {};
        const auto& n = *rows[(std::size_t) selected];
        if (n.kind != Kind::item)
            return "f:" + n.file.getFullPathName() + n.label;
        return "i:" + juce::String ((int) n.item.kind) + ":" + juce::String (n.item.index) + ":" + juce::String (n.item.slot) + ":"
               + juce::String::fromUTF8 (n.item.path.c_str()) + (n.history ? "h" : "");
    }

    void restoreSelection (const juce::String& key)
    {
        if (key.isEmpty())
            return;
        const auto keep = selected;
        selected = -1;
        for (int i = 0; i < (int) rows.size(); ++i)
        {
            selected = i;
            if (selectionKey() == key)
                return;
        }
        selected = -1;
        juce::ignoreUnused (keep);
    }

    void itemClicked (Node& n)
    {
        auto& engine = browser.engine;
        const auto& item = n.item;

        if (n.history)
        {
            engine.jumpToHistory (item.index);
            return;
        }
        if (n.initial)
            return;

        switch (item.kind)
        {
            case projectcontents::Kind::pattern: // FL: clicking a pattern makes it the current one
                if (item.index != engine.getSong().currentPattern && item.index < (int) engine.getSong().patterns.size())
                    engine.editSong ([index = item.index] (Song& s) { s.currentPattern = index; }, false);
                break;
            case projectcontents::Kind::generator:
                if (item.index < (int) engine.getSong().channels.size())
                    engine.setSelectedChannel (item.index);
                break;
            case projectcontents::Kind::sample:
            case projectcontents::Kind::effect:
            case projectcontents::Kind::automation:
                break;
        }
    }

    ui::Hint itemHint (const Node& n) const
    {
        ui::Hint h;
        h.hasMenu = true;
        const auto& item = n.item;

        if (n.initial)
        {
            h.text = n.label + ": " + tr ("reset to this value when the song starts  [RMB] delete the initial value");
            h.value = n.detail;
            return h;
        }
        if (n.history)
        {
            h.text = tr ("Undo history: [LMB] go back to this step");
            h.value = tr ("Level {0}/{1}", { juce::String (item.index + 1), juce::String (browser.engine.getHistorySize()) });
            return h;
        }

        switch (item.kind)
        {
            case projectcontents::Kind::pattern:
                h.text = n.label + ": " + tr ("[LMB] make it the current pattern  [2xLMB] open in the Piano Roll  drag into the Playlist  [RMB] pattern menu");
                h.value = item.uses == 0 ? tr ("not in the Playlist") : tr ("{0} clips", { juce::String (item.uses) });
                break;
            case projectcontents::Kind::sample:
                h.text = juce::String::fromUTF8 (item.path.c_str()) + "  "
                         + (n.missing ? tr ("(missing: File > Missing files... finds it)")
                                      : tr ("[LMB] preview  [2xLMB] new channel  drag to the Playlist or the Channel Rack  [RMB] menu"));
                h.value = tr ("used {0}x", { juce::String (item.uses) });
                break;
            case projectcontents::Kind::generator:
                h.text = n.label + (n.detail.isNotEmpty() ? " (" + n.detail + ")" : juce::String()) + ": "
                         + tr ("[LMB] select the channel  [2xLMB] open its settings  [RMB] menu");
                break;
            case projectcontents::Kind::effect:
                h.text = n.label + " (" + n.detail + "): " + tr ("[2xLMB] open its editor  [RMB] menu");
                h.value = tr ("slot {0}", { juce::String (item.slot + 1) });
                break;
            case projectcontents::Kind::automation:
                h.text = n.label + ": " + tr ("[2xLMB] open the automation editor");
                h.value = item.uses == 0 ? tr ("not in the Playlist") : tr ("{0} clips", { juce::String (item.uses) });
                break;
        }
        return h;
    }

public:
    void renamePlugin (const std::string& key, const juce::String& original)
    {
        auto safe = juce::Component::SafePointer<FileTree> (this);
        quickpick::rename (browser.engine, key, original, [safe] {
            if (safe != nullptr)
                safe->browser.rebuildTree();
        });
    }

    bool renameSelectedPlugin()
    {
        if (! juce::isPositiveAndBelow (selected, (int) rows.size()))
            return false;
        const auto& n = *rows[(std::size_t) selected];
        if (n.kind != Kind::plugin || n.pluginKey.empty())
            return false;
        renamePlugin (n.pluginKey, n.original);
        return true;
    }

private:
    void showMenu (Node& n)
    {
        auto& engine = browser.engine;
        juce::PopupMenu menu;
        auto safe = juce::Component::SafePointer<FileTree> (this);
        auto expandItems = [&menu, safe, &n] {
            Node* target = &n;
            while (target != nullptr && target->kind == Kind::item)
                target = target->parent;
            menu.addItem (tr ("Expand all"), [safe, target] {
                if (safe != nullptr && target != nullptr)
                    safe->expandAll (*target);
            });
            menu.addItem (tr ("Collapse all"), [safe] {
                if (safe != nullptr)
                    safe->collapseAll();
            });
        };

        if (n.kind == Kind::plugin)
        {
            menu.addSectionHeader (n.label);
            menu.addItem (tr ("Add"), [act = n.activate] {
                if (act)
                    act();
            });
            if (! n.pluginKey.empty()) // FL's renamed plugins (issue #11): "1_EQ" is then picked by typing 1 on an empty slot
            {
                menu.addSeparator();
                menu.addItem (tr ("Rename...") + "\tF2", [safe, key = n.pluginKey, original = n.original] {
                    if (safe != nullptr)
                        safe->renamePlugin (key, original);
                });
                menu.addItem (tr ("Original name"), n.label != n.original, false, [safe, key = n.pluginKey] {
                    if (safe != nullptr)
                    {
                        safe->browser.engine.getPluginHost().setPluginName (key, {});
                        safe->browser.rebuildTree();
                    }
                });
            }
            menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }
        if (n.kind == Kind::project || n.kind == Kind::category || n.kind == Kind::plugins)
        {
            menu.addSectionHeader (n.label);
            expandItems();
            if ((n.kind == Kind::project || n.kind == Kind::plugins) && n.parent == nullptr && browser.tab == allTab) // round 163
            {
                menu.addSeparator();
                browser.addHideItem (menu, n.kind == Kind::project ? browserview::projectRootId : browserview::pluginsRootId, n.label);
            }
            menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        if (n.kind != Kind::item)
        {
            if (n.folder)
                browser.showFolderMenu (n.file, [safe, node = &n] {
                    if (safe != nullptr)
                        safe->expandFolder (*node, 3);
                });
            else
                browser.showFileMenu (n.file);
            return;
        }

        const auto item = n.item;
        if (n.history)
            return;
        if (n.initial)
        {
            const auto& values = engine.getSong().initialValues;
            if (! juce::isPositiveAndBelow (item.index, (int) values.size()))
                return;
            menu.addSectionHeader (n.label + ": " + n.detail);
            menu.addItem (tr ("Delete initial value"), [&engine, target = values[(std::size_t) item.index].target] {
                engine.editSong ([target] (Song& s) { s.removeInitialValue (target); });
            });
            menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
            return;
        }

        switch (item.kind)
        {
            case projectcontents::Kind::pattern: // FL: the right-click makes it the current pattern, then its menu
                itemClicked (n);
                if (browser.onPatternMenu)
                    browser.onPatternMenu (item.index);
                return;
            case projectcontents::Kind::sample:
            {
                const auto file = n.file;
                const auto ok = ! n.missing;
                ui::addItemHeader (menu, tr ("Audio file"), n.label, n.colour);
                menu.addItem (tr ("Send to the selected channel"), ok && browser.onLoadIntoChannel != nullptr, false, [safe, file] {
                    if (safe != nullptr && safe->browser.onLoadIntoChannel)
                        safe->browser.onLoadIntoChannel (file);
                });
                menu.addItem (tr ("Open in a new channel"), ok, false, [safe, file] {
                    if (safe != nullptr)
                        safe->browser.openFile (file);
                });
                menu.addItem (tr ("Send to the Playlist as an audio clip"), ok && browser.onSendToPlaylist != nullptr, false, [safe, file] {
                    if (safe != nullptr && safe->browser.onSendToPlaylist)
                        safe->browser.onSendToPlaylist (file);
                });
                menu.addSeparator();
                menu.addItem (tr ("Edit in the audio editor..."), ok, false, [&engine, file] { openAudioEditor (engine, file); });
                if (item.index >= 0)
                    menu.addItem (tr ("Show its channel"), [safe, channel = item.index] {
                        if (safe != nullptr && channel < (int) safe->browser.engine.getSong().channels.size())
                            safe->browser.engine.setSelectedChannel (channel);
                    });
                menu.addSeparator();
                menu.addItem (tr ("Show in the file manager"), ok, false, [file] { file.revealToUser(); });
                break;
            }
            case projectcontents::Kind::generator:
                ui::addItemHeader (menu, tr ("Generator"), n.label, n.colour);
                menu.addItem (tr ("Open settings"), [safe, channel = item.index] {
                    if (safe != nullptr && safe->browser.onOpenChannel)
                        safe->browser.onOpenChannel (channel);
                });
                if (engine.getSong().channels[(std::size_t) item.index].mixerTrack > 0)
                    menu.addItem (tr ("Show its mixer insert"), [safe, track = engine.getSong().channels[(std::size_t) item.index].mixerTrack] {
                        if (safe != nullptr && safe->browser.onShowInsert)
                            safe->browser.onShowInsert (track);
                    });
                break;
            case projectcontents::Kind::effect:
                ui::addItemHeader (menu, tr ("Effect") + " - " + n.detail, n.label, n.colour);
                menu.addItem (tr ("View editor"), [safe, track = item.index, slot = item.slot] {
                    if (safe != nullptr && safe->browser.onOpenEffect)
                        safe->browser.onOpenEffect (track, slot);
                });
                menu.addItem (tr ("Show its mixer insert"), [safe, track = item.index] {
                    if (safe != nullptr && safe->browser.onShowInsert)
                        safe->browser.onShowInsert (track);
                });
                break;
            case projectcontents::Kind::automation:
                ui::addItemHeader (menu, tr ("Automation clip"), n.label, n.colour);
                menu.addItem (tr ("Open the automation editor"), [safe, index = item.index] {
                    if (safe != nullptr && safe->browser.onOpenAutomation)
                        safe->browser.onOpenAutomation (index);
                });
                break;
        }

        menu.addSeparator();
        expandItems();
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    void addRows (Node& n)
    {
        for (auto& c : n.children)
        {
            rows.push_back (c.get());
            if (c->folder && c->expanded)
                addRows (*c);
        }
    }

    void toggle (Node& n)
    {
        const auto keep = selectionKey();
        n.expanded = ! n.expanded;
        if (n.kind == Kind::plugins) // (remembered: the database is built again with the tree)
        {
            const auto key = (n.parent != nullptr && n.parent->kind == Kind::plugins ? n.parent->label + "/" : juce::String()) + n.label;
            if (n.expanded)
                browser.openPluginFolders.insert (key);
            else
                browser.openPluginFolders.erase (key);
        }
        if (n.expanded)
            load (n);
        rebuild();
        restoreSelection (keep);

        if (n.expanded) // show what opened
            for (int i = 0; i < (int) rows.size(); ++i)
                if (rows[(std::size_t) i] == &n)
                    scrollToRow (juce::jmin ((int) rows.size() - 1, i + juce::jmin ((int) n.children.size(), 6)));
    }

    void scrollToRow (int row)
    {
        if (row < 0)
            return;
        if (auto* view = findParentComponentOfClass<juce::Viewport>())
        {
            const auto top = row * rowHeight, bottom = top + rowHeight;
            const auto y = view->getViewPositionY(), h = view->getViewHeight();
            if (top < y)
                view->setViewPosition (0, top);
            else if (bottom > y + h)
                view->setViewPosition (0, bottom - h);
        }
    }

    void repaintRow (int row)
    {
        if (row >= 0)
            repaint (0, row * rowHeight, getWidth(), rowHeight);
    }

    juce::Image dragImage (const Node& n) const
    {
        const auto name = n.label.isNotEmpty() ? n.label : n.file.getFileNameWithoutExtension();
        const auto font = juce::Font (theme::font (theme::type::body));
        const auto width = juce::jlimit (80, 260, juce::GlyphArrangement::getStringWidthInt (font, name) + 36);
        const auto colour = n.colour.isTransparent() ? theme::colour::accent : readable (n.colour);
        juce::Image image (juce::Image::ARGB, width, 24, true);
        juce::Graphics g (image);
        g.setColour (theme::colour::bg4.withAlpha (0.92f));
        g.fillRoundedRectangle (0.5f, 0.5f, (float) width - 1.0f, 23.0f, 5.0f);
        g.setColour (colour);
        g.drawRoundedRectangle (0.5f, 0.5f, (float) width - 1.0f, 23.0f, 5.0f, 1.0f);
        icons::draw (g, n.icon != Icon::none ? n.icon : iconFor (n.file, false), { 6.0f, 4.0f, 16.0f, 16.0f }, colour);
        g.setColour (theme::colour::textPrimary);
        g.setFont (font);
        g.drawText (name, 28, 0, width - 32, 24, juce::Justification::centredLeft, true);
        return image;
    }

    BrowserComponent& browser;
    const bool flat;
    std::unique_ptr<Node> root;
    std::vector<std::unique_ptr<Node>> flatNodes;
    std::vector<Node*> rows;
    juce::Array<juce::File> bases;
    juce::String status, pendingKey;
    int selected = -1, hovered = -1, pressedRow = -1;
    int dropRow = -1; // the folder files dragged in from the system would be copied to
    bool dragStarted = false;
};

// --- the tabs: ALL / PROJECT ------------------------------------------------------------------------

class BrowserComponent::TabBar final : public juce::Component,
                                       public ui::HintSource
{
public:
    explicit TabBar (BrowserComponent& b) : browser (b) { setRepaintsOnMouseActivity (true); }

    juce::Rectangle<int> tabBounds (int t) const
    {
        const auto w = getWidth() / tabCount;
        return { t * w, 0, t == tabCount - 1 ? getWidth() - t * w : w, getHeight() };
    }

    static juce::String name (int t) { return t == allTab ? tr ("ALL") : t == projectTab ? tr ("PROJECT") : tr ("STARRED"); }

    void paint (juce::Graphics& g) override
    {
        using namespace theme;
        const auto mouse = isMouseOver() ? getMouseXYRelative() : juce::Point<int> (-1, -1);
        for (int t = 0; t < tabCount; ++t)
        {
            const auto r = tabBounds (t);
            const auto on = browser.tab == t;
            if (on || r.contains (mouse))
            {
                g.setColour (on ? colour::bg4 : colour::bg3);
                g.fillRoundedRectangle (r.reduced (2, 2).toFloat(), radius::md);
            }
            if (on)
            {
                g.setColour (colour::accent);
                g.fillRoundedRectangle (r.reduced (10, 0).removeFromBottom (2).toFloat(), 1.0f);
            }
            g.setColour (on ? colour::textPrimary : colour::textSecondary);
            g.setFont (font (type::caption, true));
            g.drawText (name (t), r, juce::Justification::centred, false);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int t = 0; t < tabCount; ++t)
            if (tabBounds (t).contains (e.getPosition()))
                browser.setTab (t);
    }

    ui::Hint hintAt (juce::Point<int> p) override
    {
        ui::Hint h;
        const auto t = juce::jlimit (0, tabCount - 1, p.x * tabCount / juce::jmax (1, getWidth()));
        h.text = t == allTab       ? tr ("Browser: ALL - the current project and every sample folder")
                 : t == projectTab ? tr ("Browser: PROJECT - only what the current project uses (patterns, effects, generators, audio files...)")
                                   : tr ("Browser: STARRED - the files and folders you starred (right-click > Starred)");
        return h;
    }

private:
    BrowserComponent& browser;
};

// --- the preview strip: waveform of the selected sample ----------------------------------------------

class BrowserComponent::PreviewStrip final : public juce::Component,
                                             public ui::HintSource,
                                             private juce::Timer,
                                             private juce::ChangeListener
{
public:
    explicit PreviewStrip (BrowserComponent& b) : browser (b) { ui::WaveformCache::getInstance()->addChangeListener (this); }

    ~PreviewStrip() override
    {
        if (auto* cache = ui::WaveformCache::getInstanceWithoutCreating())
            cache->removeChangeListener (this);
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); } // the spectral waveform is ready

    void show (const juce::File& f, bool playing)
    {
        if (f != file)
        {
            file = f;
            peaks.clear();
            seconds = 0.0;
            info.clear();

            if (isAudio (f))
                if (const auto sample = browser.engine.getSampleLibrary().load (f.getFullPathName()); sample != nullptr && sample->frames() > 0)
                {
                    // 256 peaks over the whole file
                    const auto frames = sample->frames();
                    constexpr std::size_t count = 256;
                    peaks.assign (count, 0.0f);
                    const auto* l = sample->leftData();
                    const auto* r = sample->rightData();
                    const auto stride = juce::jmax<std::size_t> (1, frames / (count * 64)); // look at up to 64 points per peak

                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto from = frames * i / count, to = juce::jmax (from + 1, frames * (i + 1) / count);
                        float m = 0.0f;
                        for (auto k = from; k < to && k < frames; k += stride)
                            m = juce::jmax (m, std::abs (l[k]), std::abs (r[k]));
                        peaks[i] = m;
                    }

                    const auto top = *std::max_element (peaks.begin(), peaks.end());
                    if (top > 1.0e-6f) // normalised: quiet files are still readable
                        for (auto& p : peaks)
                            p /= top;

                    seconds = (double) frames / juce::jmax (1.0, sample->sampleRate);
                    info = juce::String (seconds, seconds < 10.0 ? 2 : 1) + " s  " + juce::String (juce::roundToInt (sample->sampleRate / 100.0) / 10.0, 1)
                           + " kHz  " + (sample->isStereo() ? tr ("stereo") : tr ("mono"));
                }
        }

        if (playing && seconds > 0.0)
        {
            startedAt = juce::Time::getMillisecondCounterHiRes();
            startTimerHz (30);
        }
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme::colour;
        g.fillAll (bg1);
        g.setColour (border);
        g.fillRect (0, 0, getWidth(), 1);
        auto area = getLocalBounds().reduced (6, 4).withTrimmedTop (1);

        if (file == juce::File() || ! isAudio (file))
        {
            g.setColour (textDisabled);
            g.setFont (theme::font (theme::type::small));
            g.drawFittedText (tr ("Click a sample to hear it; its waveform shows here"), area, juce::Justification::centred, 2);
            return;
        }

        auto top = area.removeFromTop (14);
        g.setFont (theme::font (theme::type::caption, true));
        g.setColour (textPrimary);
        g.drawText (file.getFileNameWithoutExtension(), top, juce::Justification::centredLeft, true);
        g.setFont (theme::font (theme::type::caption));
        g.setColour (textSecondary);
        g.drawText (info, top, juce::Justification::centredRight, true);
        area.removeFromTop (2);

        g.setColour (bg0);
        g.fillRoundedRectangle (area.toFloat(), theme::radius::sm);

        if (peaks.empty())
            return;

        const auto wave = area.toFloat().reduced (2.0f, 2.0f);
        const auto mid = wave.getCentreY();
        const auto progress = position();
        const auto playedX = wave.getX() + wave.getWidth() * (float) progress;

        // the detailed, frequency-coloured waveform (ui/WaveformView) once analysed; the quick peaks until then
        const auto path = file.getFullPathName();
        if (const auto summary = ui::WaveformCache::getInstance()->find (path, [this, path] { return browser.engine.getSampleLibrary().load (path); }))
        {
            ui::paintWaveform (g, wave, *summary, 0.0, (double) summary->frames / juce::jmax (1.0f, wave.getWidth()),
                               ui::currentWaveStyle (textSecondary, ui::normalisingGain (*summary)));
            if (progress > 0.0)
            {
                g.setColour (accent.withAlpha (0.22f)); // the part already heard
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (wave.getX(), wave.getY(), playedX, wave.getBottom()));
                g.setColour (textPrimary);
                g.fillRect (playedX, wave.getY(), 1.0f, wave.getHeight());
            }
            return;
        }

        for (int x = 0; x < (int) wave.getWidth(); ++x)
        {
            const auto p = peaks[(std::size_t) (x * (int) peaks.size() / juce::jmax (1, (int) wave.getWidth()))];
            const auto h = juce::jmax (0.5f, p * wave.getHeight() * 0.5f);
            const auto px = wave.getX() + (float) x;
            g.setColour (progress > 0.0 && px <= playedX ? accent : textSecondary.withAlpha (0.75f));
            g.fillRect (px, mid - h, 1.0f, h * 2.0f);
        }

        if (progress > 0.0)
        {
            g.setColour (textPrimary);
            g.fillRect (playedX, wave.getY(), 1.0f, wave.getHeight());
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (isAudio (file) && e.mods.isLeftButtonDown())
            browser.fileSelected (file, true); // hear it again
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! dragging && isAudio (file) && e.getDistanceFromDragStart() > 5)
            if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor (this))
            {
                dragging = true;
                container->startDragging (juce::String (dragDescription) + "|" + file.getFullPathName(), this, juce::ScaledImage(), true);
            }
    }

    void mouseUp (const juce::MouseEvent&) override { dragging = false; }

    ui::Hint hintAt (juce::Point<int>) override
    {
        ui::Hint h;
        h.text = isAudio (file) ? tr ("Sample preview: [LMB] hear it again  drag it like a file in the list") : tr ("Sample preview");
        h.value = info;
        return h;
    }

    bool isPlaying() const { return position() > 0.0 && position() < 1.0; }

    void stop()
    {
        stopTimer();
        startedAt = 0.0;
        repaint();
    }

private:
    double position() const
    {
        if (startedAt <= 0.0 || seconds <= 0.0)
            return 0.0;
        return juce::jlimit (0.0, 1.0, (juce::Time::getMillisecondCounterHiRes() - startedAt) / 1000.0 / seconds);
    }

    void timerCallback() override
    {
        if (position() >= 1.0)
        {
            stop();
            browser.tree->repaint(); // the "playing" colour goes
            browser.results->repaint();
        }
        repaint();
    }

    BrowserComponent& browser;
    juce::File file;
    std::vector<float> peaks;
    double seconds = 0.0, startedAt = 0.0;
    juce::String info;
    bool dragging = false;
};

// --- the browser -------------------------------------------------------------------------------------

bool BrowserComponent::isPreviewPlaying() const
{
    return strip != nullptr && strip->isPlaying();
}

// FL's Browser details box (REA round 134/139): what is selected — a column of labels (Date, Size, Version for a
// project FL saved, Path) and their values, the path wrapping over the last lines. Hidden while nothing is selected.
class BrowserComponent::DetailsBox final : public juce::Component,
                                           public ui::HintSource
{
public:
    static constexpr int preferredHeight = 70;

    void show (const juce::File& f)
    {
        file = f;
        rows.clear();
        if (file != juce::File())
        {
            rows.push_back ({ tr ("Date"), file.getLastModificationTime().formatted ("%Y-%m-%d %H:%M:%S") });
            if (file.existsAsFile())
            {
                rows.push_back ({ tr ("Size"), juce::File::descriptionOfSizeInBytes (file.getSize()) });
                if (file.hasFileExtension (".flp"))
                {
                    juce::MemoryBlock head;
                    if (juce::FileInputStream in (file); in.openedOk())
                        in.readIntoMemoryBlock (head, 4096);
                    const auto version = flp::readSavedVersion ({ (const std::uint8_t*) head.getData(), head.getSize() });
                    if (! version.empty())
                        rows.push_back ({ tr ("Version"), juce::String (version) });
                }
            }
            rows.push_back ({ tr ("Path"), file.getFullPathName() });
        }
        repaint();
    }

    bool hasFile() const noexcept { return file != juce::File(); }

    ui::Hint hintAt (juce::Point<int>) override
    {
        ui::Hint h;
        if (file != juce::File())
            h.text = tr ("Selected: {0}", { file.getFullPathName() });
        return h;
    }

    void paint (juce::Graphics& g) override
    {
        using namespace theme::colour;
        g.fillAll (bg1);
        g.setColour (border);
        g.fillRect (0, 0, getWidth(), 1);
        auto area = getLocalBounds().reduced (6, 3).withTrimmedTop (1);
        g.setFont (theme::font (theme::type::caption));
        constexpr int line = 13, labelWidth = 48;
        for (std::size_t i = 0; i < rows.size() && area.getHeight() >= line; ++i)
        {
            const auto last = i + 1 == rows.size();
            // the path (last row) takes the lines left and wraps, as FL's box does
            auto r = last ? area : area.removeFromTop (line);
            g.setColour (textDisabled);
            g.drawText (rows[i].first, r.removeFromLeft (labelWidth).withHeight (line), juce::Justification::centredLeft, false);
            g.setColour (textSecondary);
            if (last)
                g.drawFittedText (rows[i].second, r, juce::Justification::topLeft, juce::jmax (1, r.getHeight() / line), 1.0f);
            else
                g.drawText (rows[i].second, r, juce::Justification::centredLeft, true);
        }
    }

private:
    juce::File file;
    std::vector<std::pair<juce::String, juce::String>> rows;
};

BrowserComponent::BrowserComponent (AudioEngine& e, Role r)
    : engine (e), role (r)
{
    tabs = std::make_unique<TabBar> (*this);
    addAndMakeVisible (*tabs);

    // the search index (round 162): a background pass over every folder a few seconds after start (FL's automatic
    // indexing: only the folders that changed since the last session are listed again)
    searcher = SearchShared::get();
    searcher->statusChanged.addChangeListener (this);
    searcher->indexChanged.addChangeListener (this);
    juce::Timer::callAfterDelay (4000, [safe = juce::Component::SafePointer<BrowserComponent> (this)] {
        if (safe != nullptr && safe->role == Role::main && ! safe->allRoots.isEmpty())
            safe->searcher->service->refresh (toPaths (safe->allRoots));
    });
    focusWatcher = std::make_unique<FocusWatcher> (*this);

    searchBox.setTextToShowWhenEmpty (tr ("Search (Ctrl+F): kick -loop, snare OR clap, hat*.wav"), theme::colour::textDisabled);
    searchBox.addListener (this);
    searchBox.setEscapeAndReturnKeysConsumed (true);
    ui::setHint (searchBox, tr ("Search the selected folder (or every folder) and below; in PROJECT, the project's items: words must all match; "
                                "OR, -word (not), \"exact phrase\", * and ? wildcards  [Enter] to the results  [Esc] clear"));
    addAndMakeVisible (searchBox);

    // FL's Browser has a way to silence the preview too: a Stop button beside the preview volume (issue #9)
    stopButton.setFlat (true);
    stopButton.setWantsKeyboardFocus (false);
    stopButton.setMouseClickGrabsKeyboardFocus (false);
    stopButton.setTooltip (tr ("Stop the sample preview"));
    ui::setHint (stopButton, tr ("Stop the sample preview"), true);
    stopButton.onClick = [this] { stopPreview(); };
    addAndMakeVisible (stopButton);

    // what this Browser shows (round 163): content filter, folders shown, the Sample browser window
    viewButton.setFlat (true);
    viewButton.setWantsKeyboardFocus (false);
    viewButton.setMouseClickGrabsKeyboardFocus (false);
    viewButton.setClickingTogglesState (false);
    viewButton.onClick = [this] { showViewMenu(); };
    addAndMakeVisible (viewButton);

    // the sample preview volume (FL's Browser has one too): a small knob at the end of the search row
    previewVolume.setRange (0.0, controls::maxUtilityVolume, 0.01);
    previewVolume.setDoubleClickReturnValue (true, 1.0);
    previewVolume.textFromValueFunction = [] (double v) { return ui::utilityVolumeText (v); };
    previewVolume.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue() / 100.0; };
    previewVolume.setValue (engine.getPreviewVolume(), juce::dontSendNotification);
    previewVolume.setMouseClickGrabsKeyboardFocus (false);
    previewVolume.setWantsKeyboardFocus (false);
    previewVolume.onValueChange = [this] {
        engine.setPreviewVolume (previewVolume.getValue());
        refreshPreviewVolume();
        if (onPreviewVolumeChanged != nullptr)
            onPreviewVolumeChanged();
    };
    addAndMakeVisible (previewVolume);
    refreshPreviewVolume();

    tree = std::make_unique<FileTree> (*this, false);
    results = std::make_unique<FileTree> (*this, true);

    for (auto [view, list] : { std::pair { &treeView, tree.get() }, std::pair { &resultsView, results.get() } })
    {
        *view = std::make_unique<juce::Viewport>();
        (*view)->setViewedComponent (list, false);
        (*view)->setScrollBarsShown (true, false);
        (*view)->setScrollBarThickness (10);
        (*view)->setWantsKeyboardFocus (false);
    }

    addAndMakeVisible (*treeView);
    addChildComponent (*resultsView);

    strip = std::make_unique<PreviewStrip> (*this);
    addAndMakeVisible (*strip);
    details = std::make_unique<DetailsBox>();
    addChildComponent (*details);

    rebuildRoots();
    engine.addChangeListener (this);
}

juce::File BrowserComponent::userFoldersFile()
{
    return PluginHost::settingsFolder().getChildFile ("browser-folders.txt");
}

juce::File BrowserComponent::starredFile()
{
    return PluginHost::settingsFolder().getChildFile ("browser-starred.txt");
}

void BrowserComponent::setStarred (const juce::File& file, bool on)
{
    const auto path = file.getFullPathName();
    if (on == starred.contains (path))
        return;
    if (on)
        starred.add (path);
    else
        starred.removeString (path);
    starredFile().getParentDirectory().createDirectory();
    starredFile().replaceWithText (starred.joinIntoString ("\n") + "\n");
    if (tab == starredTab)
        rebuildTree();
}

// --- what this Browser shows (round 163) ------------------------------------------------------------

juce::File BrowserComponent::viewFile (Role r)
{
    return PluginHost::settingsFolder().getChildFile (r == Role::samples ? "sample-browser-view.txt" : "browser-view.txt");
}

bool BrowserComponent::sampleBrowserWasOpen()
{
    return browserview::View::fromText (viewFile (Role::samples).loadFileAsString().toStdString()).open;
}

juce::Rectangle<int> BrowserComponent::sampleBrowserBounds()
{
    const auto v = browserview::View::fromText (viewFile (Role::samples).loadFileAsString().toStdString());
    return { v.x, v.y, v.width, v.height };
}

void BrowserComponent::loadView()
{
    viewLoaded = true;
    const auto file = viewFile (role);
    viewFromFile = file.existsAsFile();
    if (viewFromFile)
    {
        view = browserview::View::fromText (file.loadFileAsString().toStdString());
        return;
    }
    view = {};
    if (role == Role::samples)
    {
        // the Sample browser starts with only the places samples live, listing samples and SoundFonts
        view.filter = browserview::ContentFilter::samples;
        view.setShown (browserview::projectRootId, false);
        view.setShown (browserview::pluginsRootId, false);
        for (int i = 0; i < allRoots.size(); ++i)
            if (! browserview::sampleBrowserShowsByDefault (allRootKinds[(std::size_t) i]))
                view.setShown (allRoots[i].getFullPathName().toStdString(), false);
    }
}

void BrowserComponent::saveView() const
{
    if (! viewLoaded)
        return;
    const auto file = viewFile (role);
    auto v = view;
    if (role == Role::samples) // the window's open state and bounds are the window's to write
    {
        const auto old = browserview::View::fromText (file.loadFileAsString().toStdString());
        v.open = old.open;
        v.x = old.x, v.y = old.y, v.width = old.width, v.height = old.height;
    }
    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::String::fromUTF8 (v.toText().c_str()));
}

bool BrowserComponent::lists (const juce::File& file) const
{
    return browserview::listed (file.getFileName().toStdString(), view.filter) || file.isDirectory();
}

std::shared_ptr<const search::FolderMatches> BrowserComponent::filterMatches() const
{
    if (view.filter == browserview::ContentFilter::all || searcher == nullptr)
        return nullptr;
    const auto filter = view.filter;
    return searcher->service->folderMatches ((int) filter, [filter] (const std::string& name) { return browserview::listed (name, filter); });
}

bool BrowserComponent::filterHides (const juce::File& folder) const
{
    // from the search index (cached; never waits for it): a folder it doesn't know yet stays listed
    const auto matches = filterMatches();
    return matches != nullptr && matches->of (folder.getFullPathName().toStdString()) == search::FolderMatches::Contents::none;
}

void BrowserComponent::applyFilterToTree()
{
    const auto matches = filterMatches();
    if (matches == nullptr || matches->generation == prunedGeneration)
        return;
    prunedGeneration = matches->generation;
    if (tree->pruneFolders ([this] (const juce::File& f) { return filterHides (f); }))
        treeView->setViewPosition (treeView->getViewPosition()); // (keeps the scroll inside the shorter list)
}

void BrowserComponent::setContentFilter (browserview::ContentFilter filter)
{
    if (filter == view.filter)
        return;
    view.filter = filter;
    saveView();
    refreshViewButton();
    prunedGeneration = 0;
    rebuildTree();
    refreshFoldersPanel();
    if (searchBox.getText().trim().isNotEmpty())
        runSearch();
}

void BrowserComponent::setRootShown (const juce::String& id, bool shown)
{
    if (view.shows (id.toStdString()) == shown)
        return;
    view.setShown (id.toStdString(), shown);
    saveView();
    rebuildRoots();
    if (searchBox.getText().trim().isNotEmpty())
        runSearch();
}

void BrowserComponent::showAllRoots()
{
    view.hidden.clear();
    saveView();
    rebuildRoots();
}

juce::String BrowserComponent::rootId (const juce::File& folder) const
{
    return allRoots.contains (folder) ? folder.getFullPathName() : juce::String();
}

void BrowserComponent::refreshViewButton()
{
    const auto filtered = view.filter != browserview::ContentFilter::all;
    int hidden = 0;
    for (const auto& id : view.hidden)
        hidden += id == browserview::projectRootId || id == browserview::pluginsRootId
                          || allRoots.contains (juce::File (juce::String::fromUTF8 (id.c_str())))
                      ? 1 : 0;
    viewButton.setToggleState (filtered || hidden > 0, juce::dontSendNotification);
    juce::String text = tr ("Browser options: what to list, which folders to show");
    if (filtered)
        text << "  (" << tr (browserview::contentFilterLabel (view.filter)) << ")";
    if (hidden > 0)
        text << "  (" << (hidden == 1 ? tr ("1 folder hidden") : tr ("{0} folders hidden", { juce::String (hidden) })) << ")";
    viewButton.setTooltip (text);
    ui::setHint (viewButton, text, true);
}

void BrowserComponent::addHideItem (juce::PopupMenu& menu, const juce::String& id, const juce::String& label)
{
    menu.addItem (tr ("Hide \"{0}\" from this Browser", { label }), [safe = juce::Component::SafePointer<BrowserComponent> (this), id] {
        if (safe != nullptr)
            safe->setRootShown (id, false);
    });
}

void BrowserComponent::addViewItems (juce::PopupMenu& menu)
{
    juce::Component::SafePointer<BrowserComponent> safe (this);
    // what is listed (folders are always listed)
    juce::PopupMenu filters;
    for (int i = 0; i < browserview::contentFilterCount; ++i)
    {
        const auto f = (browserview::ContentFilter) i;
        filters.addItem (tr (browserview::contentFilterLabel (f)), true, view.filter == f, [safe, f] {
            if (safe != nullptr)
                safe->setContentFilter (f);
        });
    }
    menu.addSubMenu (tr ("Show") + ": " + tr (browserview::contentFilterLabel (view.filter)), filters);

    // which top-level entries are shown and the user's own folders: a window (round 164; a checklist sub-menu before)
    menu.addItem (tr ("Show folders..."), [safe] {
        if (safe != nullptr)
            safe->showFoldersDialog();
    });
}

// --- the "Show folders" window (round 164) --------------------------------------------------------------

// Which top-level entries this Browser shows, as a checklist (ticks apply at once), the user's own folders with a
// Remove button, Add folder..., Show all. Each entry says where it is and, under a content filter, whether it holds
// anything to list (those folders are left out of the tree while the filter is on).
class BrowserComponent::FoldersPanel final : public juce::Component
{
public:
    using Entry = FolderEntry;

    explicit FoldersPanel (BrowserComponent* b) : browser (b)
    {
        title.setFont (theme::font (theme::type::body, true));
        title.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (title);
        note.setFont (theme::font (theme::type::small));
        note.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        note.setJustificationType (juce::Justification::topLeft);
        addAndMakeVisible (note);
        list.setViewedComponent (&rows, false);
        list.setScrollBarsShown (true, false);
        list.setScrollBarThickness (10);
        addAndMakeVisible (list);

        addFolder.setButtonText (tr ("Add folder..."));
        addFolder.setTooltip (tr ("Add one of your own folders to the Browsers (samples, projects, SoundFonts...)"));
        addFolder.onClick = [this] { if (browser != nullptr) browser->addUserFolder(); };
        showAll.setButtonText (tr ("Show all"));
        showAll.setTooltip (tr ("Tick every folder"));
        showAll.onClick = [this] { if (browser != nullptr) browser->showAllRoots(); };
        close.setButtonText (tr ("Close"));
        close.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        close.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        close.onClick = [this] { closeToolWindowOf (*this); };
        for (auto* button : { &addFolder, &showAll, &close })
            addAndMakeVisible (button);
        refresh();
        setSize (520, 470);
    }

    // the browser's entries again (a tick, an added / removed folder, the filter changed)
    void refresh()
    {
        if (browser == nullptr)
            return;
        setEntries (browser->folderEntries(), browser->role == Role::samples,
                    browser->view.filter == browserview::ContentFilter::all ? juce::String() : tr (browserview::contentFilterLabel (browser->view.filter)));
    }

    void setEntries (std::vector<Entry> newEntries, bool sampleBrowser, const juce::String& filterName)
    {
        entries = std::move (newEntries);
        title.setText (sampleBrowser ? tr ("Folders shown in the Sample browser") : tr ("Folders shown in the Browser"), juce::dontSendNotification);
        auto text = tr ("Untick what you don't use: it leaves this Browser and its searches. Nothing is deleted.");
        if (filterName.isNotEmpty())
            text << "\n" << tr ("Listing \"{0}\": folders with nothing of that kind are left out of the tree.", { filterName });
        note.setText (text, juce::dontSendNotification);
        int shown = 0;
        for (const auto& e : entries)
            shown += e.shown ? 1 : 0;
        showAll.setEnabled (shown < (int) entries.size());

        rows.deleteAllChildren();
        for (int i = 0; i < (int) entries.size(); ++i)
            rows.addAndMakeVisible (new Row (*this, i));
        layoutRows();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 12);
        title.setBounds (area.removeFromTop (22));
        note.setBounds (area.removeFromTop (note.getText().containsChar ('\n') ? 36 : 20));
        area.removeFromTop (6);
        auto bottom = area.removeFromBottom (28);
        close.setBounds (bottom.removeFromRight (90));
        addFolder.setBounds (bottom.removeFromLeft (120));
        bottom.removeFromLeft (8);
        showAll.setBounds (bottom.removeFromLeft (90));
        area.removeFromBottom (10);
        list.setBounds (area);
        layoutRows();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::border);
        g.drawRect (list.getBounds().expanded (1), 1);
    }

    static constexpr int rowHeight = 34;

private:
    struct Row final : public juce::Component
    {
        Row (FoldersPanel& p, int i) : panel (p), index (i)
        {
            const auto& e = panel.entries[(std::size_t) i];
            tick.setToggleState (e.shown, juce::dontSendNotification);
            tick.setTooltip (e.shown ? tr ("Shown: untick to hide it from this Browser") : tr ("Hidden: tick to show it"));
            tick.onClick = [this] { panel.toggle (index, tick.getToggleState()); };
            addAndMakeVisible (tick);
            if (e.user)
            {
                remove.setTooltip (tr ("Remove this folder from the Browsers (the folder itself stays on the disk)"));
                remove.setFlat (true);
                remove.onClick = [this] { panel.remove (index); };
                addAndMakeVisible (remove);
            }
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (6, 0);
            tick.setBounds (area.removeFromLeft (26).withSizeKeepingCentre (24, 24));
            if (remove.isVisible())
                remove.setBounds (area.removeFromRight (26).withSizeKeepingCentre (22, 22));
        }

        void paint (juce::Graphics& g) override
        {
            const auto& e = panel.entries[(std::size_t) index];
            if (index % 2 == 1)
                g.fillAll (theme::colour::bg2.brighter (0.06f));
            auto area = getLocalBounds().reduced (6, 2).withTrimmedLeft (32).withTrimmedRight (remove.isVisible() ? 30 : 0);
            auto top = area.removeFromTop (area.getHeight() / 2 + 1);
            g.setFont (theme::font (theme::type::body, e.shown));
            g.setColour (e.shown ? theme::colour::textPrimary : theme::colour::textDisabled);
            juce::String name = e.name;
            if (e.user)
                name << "  " << tr ("(your folder)");
            g.drawText (name, top, juce::Justification::bottomLeft, true);
            g.setFont (theme::font (theme::type::small));
            g.setColour (theme::colour::textDisabled);
            auto detail = e.path;
            if (e.empty)
                detail = tr ("nothing to list under the filter") + "  ·  " + detail;
            g.drawFittedText (detail, area, juce::Justification::topLeft, 1, 0.9f);
        }

        void mouseUp (const juce::MouseEvent& e) override
        {
            if (e.mouseWasClicked() && ! e.mods.isPopupMenu())
                tick.triggerClick(); // the whole row ticks
        }

        FoldersPanel& panel;
        int index;
        juce::ToggleButton tick;
        IconButton remove { "remove folder", Icon::close };
    };

    void layoutRows()
    {
        const auto width = list.getWidth() - (entries.size() * (std::size_t) rowHeight > (std::size_t) list.getHeight() ? 10 : 0);
        rows.setSize (juce::jmax (0, width), (int) entries.size() * rowHeight);
        for (int i = 0; i < rows.getNumChildComponents(); ++i)
            rows.getChildComponent (i)->setBounds (0, i * rowHeight, rows.getWidth(), rowHeight);
    }

    void toggle (int index, bool shown)
    {
        if (browser != nullptr && juce::isPositiveAndBelow (index, (int) entries.size()))
        {
            const auto id = entries[(std::size_t) index].id;
            juce::MessageManager::callAsync ([safe = browser, id, shown] { // (rebuilds the rows: not inside a row's click)
                if (safe != nullptr)
                    safe->setRootShown (id, shown);
            });
        }
    }

    void remove (int index)
    {
        if (browser != nullptr && juce::isPositiveAndBelow (index, (int) entries.size()))
        {
            const juce::File folder (entries[(std::size_t) index].path);
            juce::MessageManager::callAsync ([safe = browser, folder] {
                if (safe != nullptr)
                    safe->removeUserFolder (folder);
            });
        }
    }

    juce::Component::SafePointer<BrowserComponent> browser;
    std::vector<Entry> entries;
    juce::Label title, note;
    juce::Viewport list;
    juce::Component rows;
    juce::TextButton addFolder, showAll, close;
};

std::vector<BrowserComponent::FolderEntry> BrowserComponent::folderEntries() const
{
    std::vector<FolderEntry> out;
    out.push_back ({ browserview::projectRootId, tr ("Current project"), tr ("the project's patterns, channels, samples and history"),
                     view.shows (browserview::projectRootId), false, false });
    out.push_back ({ browserview::pluginsRootId, tr ("Plugin database"), tr ("LYNX's, FL's and your installed plugins"),
                     view.shows (browserview::pluginsRootId), false, false });
    for (int i = 0; i < allRoots.size(); ++i)
    {
        FolderEntry e;
        e.id = e.path = allRoots[i].getFullPathName();
        e.name = allRootNames[i];
        e.shown = view.shows (e.id.toStdString());
        e.user = allRootKinds[(std::size_t) i] == browserview::RootKind::user;
        e.empty = filterHides (allRoots[i]);
        out.push_back (std::move (e));
    }
    return out;
}

void BrowserComponent::refreshFoldersPanel()
{
    if (auto* panel = dynamic_cast<FoldersPanel*> (foldersPanel.getComponent()))
        panel->refresh();
}

void BrowserComponent::showFoldersDialog()
{
    auto panel = std::make_unique<FoldersPanel> (this);
    foldersPanel = panel.get();
    openToolWindow (std::move (panel), role == Role::samples ? tr ("Sample browser folders") : tr ("Browser folders"),
                    role == Role::samples ? "sample-browser-folders" : "browser-folders");
}

std::unique_ptr<juce::Component> BrowserComponent::makeFoldersPanelForSnapshot()
{
    auto panel = std::make_unique<FoldersPanel> (this);
    // a user folder (with its Remove button) and one hidden entry, whatever this machine has
    auto entries = folderEntries();
    entries.push_back ({ "D:\\Samples\\Drums", "Drums", "D:\\Samples\\Drums", true, true, false });
    if (entries.size() > 3)
        entries[2].shown = false;
    panel->setEntries (std::move (entries), false, {});
    return panel;
}

void BrowserComponent::showViewMenu()
{
    juce::PopupMenu menu;
    addViewItems (menu);
    juce::Component::SafePointer<BrowserComponent> safe (this);
    menu.addSeparator();
    menu.addItem (tr ("Refresh"), [safe] { if (safe != nullptr) safe->rebuildRoots(); });
    menu.addItem (tr ("Refresh search index"), [safe] { if (safe != nullptr) safe->refreshSearchIndex (false); });
    if (onToggleSampleBrowser != nullptr)
    {
        menu.addSeparator();
        menu.addItem (role == Role::samples ? tr ("Close the Sample browser") : tr ("Sample browser (separate window)"),
                      [safe] { if (safe != nullptr && safe->onToggleSampleBrowser != nullptr) safe->onToggleSampleBrowser(); });
    }
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&viewButton));
}

namespace
{
// The Sample browser window's content: a drag container (samples dragged out of this window reach the Channel Rack,
// the Playlist and the Mixer of the main window) holding the Browser; it keeps the window's bounds and whether the
// user closed it.
class SampleBrowserHost final : public juce::Component,
                                public juce::DragAndDropContainer
{
public:
    SampleBrowserHost (AudioEngine& engine, const std::function<void (BrowserComponent&)>& wire)
        : browser (engine, BrowserComponent::Role::samples)
    {
        if (wire != nullptr)
            wire (browser);
        addAndMakeVisible (browser);
        setSize (340, 620);
    }

    ~SampleBrowserHost() override
    {
        // a window the user closed is hidden first (ToolWindow::closeSoon); one deleted while visible goes with LYNX
        // and opens again at the next start
        auto* window = getTopLevelComponent();
        auto v = browser.getView();
        v.open = window != this && window->isVisible();
        if (window != this)
        {
            const auto b = window->getBounds();
            v.x = b.getX();
            v.y = b.getY();
            v.width = b.getWidth();
            v.height = b.getHeight();
        }
        file().getParentDirectory().createDirectory();
        file().replaceWithText (juce::String::fromUTF8 (v.toText().c_str()));
    }

    void resized() override { browser.setBounds (getLocalBounds()); }

    static juce::File file() { return PluginHost::settingsFolder().getChildFile ("sample-browser-view.txt"); }

    BrowserComponent browser;
};
} // namespace

std::unique_ptr<juce::Component> BrowserComponent::makeSampleBrowserContent (AudioEngine& engine, const std::function<void (BrowserComponent&)>& wire)
{
    auto host = std::make_unique<SampleBrowserHost> (engine, wire);
    auto v = host->browser.getView();
    v.open = true; // (written at once: a crash keeps it open next time too)
    SampleBrowserHost::file().getParentDirectory().createDirectory();
    SampleBrowserHost::file().replaceWithText (juce::String::fromUTF8 (v.toText().c_str()));
    return host;
}

void BrowserComponent::rebuildRoots()
{
    roots.clear();
    rootNames.clear();
    allRoots.clear();
    allRootNames.clear();
    allRootKinds.clear();
    using browserview::RootKind;

    // FL folders (if installed) and user folders
    const auto programs = juce::File::getSpecialLocation (juce::File::globalApplicationsDirectory);
    juce::File fl;

    for (const auto& dir : programs.getChildFile ("Image-Line").findChildFiles (juce::File::findDirectories, false, "FL Studio*"))
        if (dir.getChildFile ("Data").isDirectory() && dir.getFileName() > fl.getFileName()) // "FL Studio ASIO" is the driver
            fl = dir;

    auto add = [this] (const juce::String& name, const juce::File& folder, RootKind kind) {
        if (folder.isDirectory() && ! allRoots.contains (folder))
        {
            allRoots.add (folder);
            allRootNames.add (name);
            allRootKinds.push_back (kind);
        }
    };

    const auto data = fl.getChildFile ("Data");
    add (tr ("FL samples (Packs)"), data.getChildFile ("Patches").getChildFile ("Packs"), RootKind::flSamples);
    add (tr ("FL demo projects"), data.getChildFile ("Demo projects"), RootKind::flDemos);
    add (tr ("FL MIDI"), data.getChildFile ("Patches").getChildFile ("Scores"), RootKind::flMidi);
    add (tr ("3x Osc presets (FL)"), data.getChildFile ("Patches").getChildFile ("Channel presets").getChildFile ("3x Osc"), RootKind::flPresets);
    add (tr ("Synth presets (FL)"), data.getChildFile ("Patches").getChildFile ("Plugin presets").getChildFile ("Generators"), RootKind::flPresets);
    pluginpresetui::presetsRoot().createDirectory(); // (the plugin window saves presets there; round 162d)
    add (tr ("Plugin presets"), pluginpresetui::presetsRoot(), RootKind::pluginPresets);
    add (tr ("Documents"), juce::File::getSpecialLocation (juce::File::userDocumentsDirectory), RootKind::documents);
    add (tr ("Music"), juce::File::getSpecialLocation (juce::File::userMusicDirectory), RootKind::music);
    add (tr ("Desktop"), juce::File::getSpecialLocation (juce::File::userDesktopDirectory), RootKind::desktop);
    add (tr ("Downloads"), juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile ("Downloads"), RootKind::downloads);

    for (const auto& line : juce::StringArray::fromLines (userFoldersFile().loadFileAsString()))
        if (line.trim().isNotEmpty())
            add (juce::File (line.trim()).getFileName(), juce::File (line.trim()), RootKind::user);

    if (! viewLoaded)
        loadView();
    for (int i = 0; i < allRoots.size(); ++i)
        if (view.shows (allRoots[i].getFullPathName().toStdString()))
        {
            roots.add (allRoots[i]);
            rootNames.add (allRootNames[i]);
        }
    refreshViewButton();
    refreshFoldersPanel();

    starred.clear();
    for (const auto& line : juce::StringArray::fromLines (starredFile().loadFileAsString()))
        if (line.trim().isNotEmpty() && juce::File::isAbsolutePath (line.trim()))
            starred.addIfNotAlreadyThere (line.trim());

    rebuildTree();
}

void BrowserComponent::rebuildTree()
{
    std::vector<std::unique_ptr<FileTree::Node>> top;
    if (tab == starredTab)
    {
        // folders first (each opens like the ALL tab's), then files, both by name; missing ones are left out
        juce::Array<juce::File> folders, files;
        for (const auto& path : starred)
            if (const juce::File f (path); f.isDirectory())
                folders.add (f);
            else if (f.existsAsFile())
                files.add (f);
        const auto byName = [] (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()) < 0; };
        std::sort (folders.begin(), folders.end(), byName);
        std::sort (files.begin(), files.end(), byName);
        for (const auto& f : folders)
            top.push_back (FileTree::makeFolder (f, f.getFileName(), theme::colour::accent));
        for (const auto& f : files)
        {
            auto n = std::make_unique<FileTree::Node>();
            n->file = f;
            top.push_back (std::move (n));
        }
        tree->setTopLevel (std::move (top));
        treeView->setViewPosition (0, 0);
        return;
    }
    if (tab == projectTab || view.shows (browserview::projectRootId))
        top.push_back (FileTree::makeProject (tab == projectTab));
    if (tab == allTab && view.shows (browserview::pluginsRootId)) // FL's Plugin database, under the project (round 142)
        top.push_back (FileTree::makePluginDatabase (*this, openPluginFolders));

    if (tab == allTab)
        for (int i = 0; i < allRoots.size(); ++i)
        {
            if (! roots.contains (allRoots[i]))
                continue; // hidden from this Browser (round 163)
            if (filterHides (allRoots[i]))
                continue; // nothing in it is listed under the content filter (round 164)
            // the FL folders keep their colour by position; the user's own folders share one
            auto colour = rootColour (i < 9 ? i : 99);
            top.push_back (FileTree::makeFolder (allRoots[i], allRootNames[i], colour));
        }

    tree->setTopLevel (std::move (top));
    treeView->setViewPosition (0, 0);
}

void BrowserComponent::setTab (int newTab)
{
    newTab = juce::jlimit (0, tabCount - 1, newTab);
    if (newTab == tab)
        return;
    tab = newTab;
    rebuildTree();
    tabs->repaint();
    if (searchBox.getText().isNotEmpty())
        runSearch();
}

void BrowserComponent::setProjectFile (const juce::File& file)
{
    if (file == projectFile)
        return;
    projectFile = file;
    rebuildTree(); // the "Project folder" entry follows the project
}

void BrowserComponent::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &searcher->statusChanged)
    {
        indexStatusChanged();
        return;
    }
    if (source == &searcher->indexChanged)
    {
        applyFilterToTree();
        // the background check found changed folders: the results shown are searched again, in place
        if (resultsView->isVisible() && tab != projectTab && searchBox.getText().trim().isNotEmpty())
            runSearch (true);
        return;
    }

    // the project changed: its sections are listed again a moment later (not on every knob step)
    projectDirty = true;
    if (! isTimerRunning())
        startTimer (300);
}

void BrowserComponent::addUserFolder()
{
    chooser = std::make_unique<juce::FileChooser> (tr ("Add a folder to the Browser"), juce::File::getSpecialLocation (juce::File::userMusicDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc) {
        const auto folder = fc.getResult();
        if (! folder.isDirectory())
            return;

        auto lines = juce::StringArray::fromLines (userFoldersFile().loadFileAsString());
        lines.removeEmptyStrings();
        lines.addIfNotAlreadyThere (folder.getFullPathName());
        userFoldersFile().replaceWithText (lines.joinIntoString ("\n"));
        setTab (allTab);
        rebuildRoots();
    });
}

void BrowserComponent::removeUserFolder (const juce::File& folder)
{
    auto lines = juce::StringArray::fromLines (userFoldersFile().loadFileAsString());
    lines.removeEmptyStrings();
    for (int i = lines.size(); --i >= 0;)
        if (juce::File (lines[i].trim()) == folder)
            lines.remove (i);
    userFoldersFile().replaceWithText (lines.joinIntoString ("\n"));
    if (view.hidden.erase (folder.getFullPathName().toStdString()) > 0) // (a hidden mark of a folder no longer listed)
        saveView();
    rebuildRoots();
}

BrowserComponent::~BrowserComponent()
{
    engine.removeChangeListener (this);
    searcher->statusChanged.removeChangeListener (this);
    searcher->indexChanged.removeChangeListener (this);
    searcher->service->cancelSearch();
    focusWatcher.reset();
    engine.previewFile ({});
}

void BrowserComponent::waitForListing (int)
{
    // the tree lists folders synchronously when they open
}

void BrowserComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    tabs->setBounds (area.removeFromTop (26));
    area.removeFromTop (4);
    {
        auto row = area.removeFromTop (24);
        previewVolume.setBounds (row.removeFromRight (24));
        stopButton.setBounds (row.removeFromRight (22));
        viewButton.setBounds (row.removeFromRight (22));
        row.removeFromRight (4);
        searchBox.setBounds (row);
    }
    // indexing progress / what the search found (round 162)
    statusArea = statusLine.isNotEmpty() ? area.removeFromTop (16).withTrimmedLeft (2) : juce::Rectangle<int>();
    area.removeFromTop (4);
    strip->setBounds (getLocalBounds().removeFromBottom (juce::jmin (64, getHeight() / 4)));
    area.setBottom (strip->getY() - 2);
    // FL's details box (date, size / version, folder) just above the preview strip, while something is selected
    details->setVisible (details->hasFile() && area.getHeight() > 120);
    if (details->isVisible())
        details->setBounds (area.removeFromBottom (DetailsBox::preferredHeight).withX (0).withWidth (getWidth()));
    treeView->setBounds (area);
    resultsView->setBounds (area);

    for (auto* list : { tree.get(), results.get() })
    {
        list->setSize (area.getWidth() - 10, list->getHeight());
        list->updateSize();
    }
}

void BrowserComponent::refreshPreviewVolume()
{
    previewVolume.setValue (engine.getPreviewVolume(), juce::dontSendNotification);
    const auto text = tr ("Sample preview volume: {0}", { ui::utilityVolumeText (engine.getPreviewVolume()) });
    previewVolume.setTooltip (text);
    ui::setHint (previewVolume, text + "  " + tr ("[LMB] drag  [Wheel] step  [2xLMB] 100%  [RMB] type a value"), true);
}

void BrowserComponent::paint (juce::Graphics& g)
{
    const uitrace::Scope trace ("BrowserComponent paint", true, g.getClipBounds());
    g.fillAll (theme::colour::bg2);
    if (! statusArea.isEmpty())
    {
        g.setFont (theme::font (theme::type::caption));
        g.setColour (theme::colour::textDisabled);
        g.drawText (statusLine, statusArea, juce::Justification::centredLeft, true);
    }
}

void BrowserComponent::showPreviewForSnapshot (const juce::File& file)
{
    strip->show (file, false);
}

void BrowserComponent::fileSelected (const juce::File& file, bool preview)
{
    if (details != nullptr && file != juce::File())
    {
        const auto wasVisible = details->hasFile();
        details->show (file);
        if (! wasVisible)
            resized();
    }

    if (preview && file.hasFileExtension ("sf2;sf3") && file.existsAsFile())
    {
        int bank = 0, program = 0;
        if (firstSoundFontPreset (file.getFullPathName().toStdString(), bank, program))
            previewSoundFontPreset (engine, file.getFullPathName().toStdString(), bank, program);
        return;
    }
    if (! isAudio (file))
        return;

    if (preview && file.existsAsFile())
    {
        engine.previewFile (file);
        previewedFile = file;
    }

    strip->show (file, preview);
    tree->repaint();
    results->repaint();
}

// FL's Browser menus (REA round 143b, [EXEC] FL 2026: right-click on a sample, a folder, a plugin): the file's actions
// first, then a "System" section (open the parent folder, locate the file, delete it) and a "Tags" section (Favorite);
// a folder: Expand all / Collapse all, then System (Open). LYNX keeps its own "Add folder..." and "Refresh".
// Round 163 (user feedback: "this sort of UI has a ton of wasted space"): no "System" / "Tags" titles any more; the
// rarely used system actions sit in one "File" sub-menu, Starred stays one click away.
void BrowserComponent::addSystemItems (juce::PopupMenu& menu, const juce::File& file)
{
    menu.addSeparator();
    menu.addItem (tr ("Starred"), true, isStarred (file), [this, file] { setStarred (file, ! isStarred (file)); });
    juce::PopupMenu system;
    system.addItem (tr ("Open parent folder"), [file] { file.getParentDirectory().startAsProcess(); });
    system.addItem (tr ("Locate file"), [file] { file.revealToUser(); });
    system.addItem (tr ("Copy the file path"), [file] { juce::SystemClipboard::copyTextToClipboard (file.getFullPathName()); });
    system.addSeparator();
    system.addItem (tr ("Delete file..."), file.existsAsFile(), false, [this, file] { confirmDeleteFile (file); });
    menu.addSubMenu (tr ("File"), system);
}

void BrowserComponent::showNote (const juce::String& text)
{
    const auto relayout = text.isEmpty() != statusLine.isEmpty();
    statusLine = text;
    if (relayout)
        resized();
    repaint();
    // it goes away by itself (unless something else wrote there meanwhile)
    juce::Timer::callAfterDelay (5000, [safe = juce::Component::SafePointer<BrowserComponent> (this), text] {
        if (safe != nullptr && safe->statusLine == text)
            safe->showNote ({});
    });
}

void BrowserComponent::confirmDeleteFile (const juce::File& file)
{
    juce::Component::SafePointer<BrowserComponent> safe (this);
    juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::WarningIcon, tr ("Delete file"),
                                        tr ("Move \"{0}\" to the recycle bin?", { file.getFileName() }), tr ("Delete"), tr ("Cancel"), nullptr,
                                        juce::ModalCallbackFunction::create ([safe, file] (int result) {
                                            if (result == 0 || safe == nullptr)
                                                return;
                                            if (! file.moveToTrash())
                                                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Delete file"),
                                                                                        tr ("The file could not be deleted."));
                                            safe->rebuildRoots();
                                        }));
}

void BrowserComponent::showFileMenu (const juce::File& file)
{
    juce::PopupMenu menu;
    menu.addSectionHeader (file.getFileName());

    if (isAudio (file))
    {
        menu.addItem (tr ("Send to the selected channel"), onLoadIntoChannel != nullptr, false, [this, file] { onLoadIntoChannel (file); });
        menu.addItem (tr ("Open in a new channel"), [this, file] { openFile (file); });
        menu.addSeparator();
        menu.addItem (tr ("Send to the Playlist as an audio clip"), onSendToPlaylist != nullptr, false, [this, file] { onSendToPlaylist (file); });
        menu.addSeparator();
        menu.addItem (tr ("Edit in the audio editor..."), [this, file] { openAudioEditor (engine, file); });
    }
    else if (file.hasFileExtension ("lynx;flp"))
        menu.addItem (tr ("Open project"), onOpenProject != nullptr, false, [this, file] { onOpenProject (file); });
    else if (file.hasFileExtension ("mid;midi"))
        menu.addItem (tr ("Open MIDI file"), onOpenProject != nullptr, false, [this, file] { onOpenProject (file); });
    else if (file.hasFileExtension ("fst;gmsynth"))
        menu.addItem (tr ("Open in a new channel"), [this, file] { openFile (file); });
    else if (file.hasFileExtension ("sf2;sf3"))
    {
        const auto selected = engine.getSelectedChannel();
        const auto& song = engine.getSong();
        const auto selectedIsFont = selected >= 0 && selected < (int) song.channels.size()
                                    && song.channels[(std::size_t) selected].instrument == InstrumentKind::soundfont;
        menu.addItem (tr ("Open in a new channel"), [this, file] { openSoundFontFile (engine, file); });
        menu.addItem (tr ("Send to the selected channel"), selectedIsFont, false, [this, file, selected] { openSoundFontFile (engine, file, selected); });
    }

    addSystemItems (menu, file);
    menu.addItem (tr ("Collapse all folders"), [this] { tree->collapseAll(); });
    juce::PopupMenu browserMenu;
    addViewItems (browserMenu);
    menu.addSubMenu (tr ("Browser"), browserMenu);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

void BrowserComponent::showFolderMenu (const juce::File& folder, std::function<void()> expandAll)
{
    // compact (round 163): the folder's name as the only title, its actions, then the Browser's own options
    juce::PopupMenu menu;
    menu.addSectionHeader (folder.getFileName());
    menu.addItem (tr ("Expand all"), expandAll != nullptr, false, [expandAll] { expandAll(); });
    menu.addItem (tr ("Collapse all"), [this] { tree->collapseAll(); });
    menu.addItem (tr ("Starred"), folder.isDirectory(), isStarred (folder), [this, folder] { setStarred (folder, ! isStarred (folder)); });
    menu.addItem (tr ("Open in the file manager"), [folder] { folder.startAsProcess(); });
    menu.addItem (tr ("Copy the file path"), [folder] { juce::SystemClipboard::copyTextToClipboard (folder.getFullPathName()); });
    menu.addSeparator();
    if (const auto id = rootId (folder); id.isNotEmpty() && tab == allTab)
    {
        addHideItem (menu, id, allRootNames[allRoots.indexOf (folder)]);
        if (allRootKinds[(std::size_t) allRoots.indexOf (folder)] == browserview::RootKind::user)
            menu.addItem (tr ("Remove this folder from the Browsers"), [safe = juce::Component::SafePointer<BrowserComponent> (this), folder] {
                if (safe != nullptr)
                    safe->removeUserFolder (folder);
            });
    }
    juce::PopupMenu browserMenu;
    addViewItems (browserMenu);
    browserMenu.addSeparator();
    browserMenu.addItem (tr ("Refresh"), [this] { rebuildRoots(); });
    browserMenu.addItem (tr ("Refresh search index"), [this] { refreshSearchIndex (false); });
    menu.addSubMenu (tr ("Browser"), browserMenu);
    menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
}

BrowserComponent::FileTree& BrowserComponent::activeList() const
{
    return resultsView->isVisible() ? *results : *tree;
}

// --- search ------------------------------------------------------------------------------------------

void BrowserComponent::focusSearch()
{
    searchBox.grabKeyboardFocus();
    searchBox.selectAll();
}

juce::Array<juce::File> BrowserComponent::searchRoots() const
{
    // the top-level folder holding the selection; nothing selected there: every folder
    if (const auto root = tree->selectedRoot(); root.isDirectory())
        return { root };
    if (tab == starredTab) // the starred folders
    {
        juce::Array<juce::File> folders;
        for (const auto& path : starred)
            if (const juce::File f (path); f.isDirectory())
                folders.add (f);
        return folders;
    }
    return roots;
}

void BrowserComponent::setSearchForSnapshot (const juce::String& text)
{
    // synchronous: the snapshot is drawn before any message is dispatched
    searchBox.setText (text, juce::dontSendNotification);
    searcher->service->cancelSearch();
    resultsView->setVisible (text.isNotEmpty());
    treeView->setVisible (text.isEmpty());

    if (text.isEmpty() || roots.isEmpty())
        return;

    const juce::Array<juce::File> scope { roots[0] }; // the FL samples: the same results on every machine with FL
    auto answer = std::make_shared<std::pair<std::mutex, search::SearchResult>>();
    searcher->service->search (text.toStdString(), toPaths (scope), (std::size_t) maxResults, [answer] (const search::SearchResult& r) {
        const std::lock_guard<std::mutex> guard (answer->first);
        answer->second = r;
    });
    searcher->service->waitIdle (120000);
    juce::Array<juce::File> found;
    {
        const std::lock_guard<std::mutex> guard (answer->first);
        for (const auto& path : answer->second.paths)
            found.add (juce::File (juce::String::fromUTF8 (path.c_str())));
    }
    results->setFiles (found, scope, found.isEmpty() ? tr ("Nothing found for \"{0}\".", { text }) : juce::String());
    if (! found.isEmpty())
        results->select (0, false);
}

void BrowserComponent::selectFirstSampleForSnapshot()
{
    if (const auto row = tree->firstAudioRow(); row >= 0)
    {
        tree->select (row, false);
        strip->show (tree->selectedFile(), false);
        tree->setHovered (row + 1 < tree->numRows() ? row + 1 : -1);
    }
}

void BrowserComponent::showFilterForSnapshot (const juce::File& folder, browserview::ContentFilter filter)
{
    // a folder of the user's (not saved) listed under `filter` once the index knows it (nothing saved either)
    if (! allRoots.contains (folder))
    {
        allRoots.add (folder);
        allRootNames.add (folder.getFileName());
        allRootKinds.push_back (browserview::RootKind::user);
    }
    roots.clear();
    roots.add (folder);
    view.filter = filter;
    view.setShown (browserview::projectRootId, false);
    view.setShown (browserview::pluginsRootId, false);
    searcher->service->refresh (toPaths ({ folder }));
    searcher->service->waitIdle (60000);
    prunedGeneration = 0;
    setTab (allTab);
    rebuildTree();
    tree->expandTop (folder, 3);
    refreshViewButton();
}

void BrowserComponent::resetViewForSnapshot()
{
    viewLoaded = false; // (read again: the snapshot's changes were never saved)
    prunedGeneration = 0;
    rebuildRoots();
}

void BrowserComponent::showPluginDatabaseForSnapshot()
{
    openPluginFolders = { tr ("Plugin database"), tr ("Plugin database") + "/" + tr ("Generators"), tr ("Generators") + "/" + tr ("FL engines"),
                          tr ("Plugin database") + "/" + tr ("Effects"), tr ("Effects") + "/" + tr ("FL engines") };
    setTab (allTab);
    rebuildTree();
}

void BrowserComponent::showPluginDatabase()
{
    const auto db = tr ("Plugin database");
    openPluginFolders.insert (db);
    openPluginFolders.insert (db + "/" + tr ("Generators"));
    openPluginFolders.insert (db + "/" + tr ("Effects"));
    setTab (allTab);
    rebuildTree();
    if (const auto row = tree->rowLabelled (db); row >= 0)
    {
        tree->select (row, false);
        tree->grabKeyboardFocus();
    }
}

void BrowserComponent::showProjectForSnapshot()
{
    setTab (projectTab);
    if (const auto row = tree->openProjectSections ({ Category::patterns, Category::generators, Category::samples }); row >= 0)
    {
        tree->select (row, false);
        strip->show (tree->selectedFile(), false);
        tree->setHovered (row + 1 < tree->numRows() ? row + 1 : -1);
    }
}

void BrowserComponent::textEditorTextChanged (juce::TextEditor&)
{
    searchPending = true;
    startTimer (250); // search once typing pauses
}

void BrowserComponent::textEditorEscapeKeyPressed (juce::TextEditor&)
{
    // Esc: clears the search and gives the keyboard back (Space plays again)
    searchPending = false;
    searchBox.clear();
    runSearch();
    leaveSearchBox();
}

void BrowserComponent::textEditorReturnKeyPressed (juce::TextEditor&)
{
    // Enter: searches now and leaves the box, so Up / Down step through the results (and Space plays)
    searchPending = false;
    runSearch();
    leaveSearchBox();
}

bool BrowserComponent::isTypingInSearch() const
{
    return searchBox.hasKeyboardFocus (true);
}

void BrowserComponent::leaveSearchBox()
{
    if (! isTypingInSearch())
        return;
    // the window's first ancestor that takes keys (the main window's content: the shortcuts, the transport) gets them
    for (auto* p = getParentComponent(); p != nullptr; p = p->getParentComponent())
        if (p->getWantsKeyboardFocus() && p->isShowing())
        {
            p->grabKeyboardFocus();
            if (! isTypingInSearch())
                return;
        }
    searchBox.giveAwayKeyboardFocus();
}

void BrowserComponent::timerCallback()
{
    stopTimer();

    if (std::exchange (searchPending, false))
        runSearch();

    if (std::exchange (projectDirty, false))
    {
        tree->refreshProject();
        if (tab == projectTab && resultsView->isVisible())
            runSearch();
    }
}

void BrowserComponent::runSearch (bool quiet)
{
    searcher->service->cancelSearch();
    searchId = 0;
    const auto query = searchBox.getText().trim();
    resultsView->setVisible (query.isNotEmpty());
    treeView->setVisible (query.isEmpty());

    if (query.isEmpty())
    {
        searchPartial = false;
        indexStatusChanged();
        return;
    }

    if (tab == projectTab) // the project's own items, at once
    {
        auto found = FileTree::projectMatches (engine.getSong(), query);
        const auto none = found.empty();
        results->setNodes (std::move (found), none ? tr ("Nothing found for \"{0}\".", { query }) : juce::String());
        return;
    }

    const auto scope = searchRoots();
    if (scope.isEmpty())
        return;

    if (! quiet)
    {
        results->setFiles ({}, scope, tr ("Searching {0}...", { scope.size() == 1 ? scope[0].getFullPathName() : tr ("every folder") }));
        resultsView->setViewPosition (0, 0);
    }

    // the worker answers on its thread: the answer comes to the message thread (and is dropped if this Browser went)
    searchId = searcher->service->search (query.toStdString(), toPaths (scope), (std::size_t) maxResults,
                                          [safe = juce::Component::SafePointer<BrowserComponent> (this), query, scope] (const search::SearchResult& r) {
                                              juce::MessageManager::callAsync ([safe, query, scope, r] {
                                                  if (safe != nullptr)
                                                      safe->searchAnswered (r, query, scope);
                                              });
                                          });
}

void BrowserComponent::searchAnswered (const search::SearchResult& r, const juce::String& query, const juce::Array<juce::File>& scope)
{
    if (r.id != searchId || searchBox.getText().trim() != query)
        return;

    juce::Array<juce::File> found;
    for (const auto& path : r.paths)
        if (juce::File f (juce::String::fromUTF8 (path.c_str())); view.filter == browserview::ContentFilter::all || lists (f))
            found.add (f);

    // (partial answers and re-searches keep the selection and the scroll position)
    const auto selected = results->selectedFile();
    const auto scroll = resultsView->getViewPosition();
    results->setFiles (found, scope,
                       found.isEmpty() ? (r.partial ? tr ("Indexing the folders... nothing found for \"{0}\" yet.", { query })
                                                    : tr ("Nothing found for \"{0}\".", { query }))
                                       : juce::String());
    if (selected != juce::File())
        if (const auto row = found.indexOf (selected); row >= 0)
            results->select (row, false);
    resultsView->setViewPosition (scroll);

    searchPartial = r.partial;
    if (! r.partial && ! found.isEmpty())
        // FL's hint bar after a search: "Found 51 files" (REA round 134)
        ui::postHint ({ r.truncated ? tr ("Found more than {0} files", { juce::String (found.size()) })
                                    : found.size() == 1 ? tr ("Found 1 file") : tr ("Found {0} files", { juce::String (found.size()) }),
                        {}, false, false },
                      4000);

    // the line under the search box
    juce::String line;
    if (r.partial)
        line = tr ("Indexing... {0} found so far", { juce::String (found.size()) });
    else if (r.truncated)
        line = tr ("Showing the first {0} results.", { juce::String (maxResults) });
    else
        line = found.size() == 1 ? tr ("Found 1 file") : tr ("Found {0} files", { juce::String (found.size()) });
    const auto st = searcher->service->status();
    if (st.busy && ! r.partial)
        line << "  ·  " << tr ("checking the index...");
    if (line != statusLine)
    {
        const auto relayout = line.isEmpty() != statusLine.isEmpty();
        statusLine = line;
        if (relayout)
            resized();
        repaint (statusArea);
    }
}

void BrowserComponent::indexStatusChanged()
{
    applyFilterToTree(); // the index changed: folders found empty under the content filter go (round 164)
    const auto st = searcher->service->status();
    juce::String line;
    if (resultsView->isVisible() && tab != projectTab && searchBox.getText().trim().isNotEmpty())
    {
        // a search is shown: its line stays; while the folders are indexed, the progress is added
        line = statusLine.upToFirstOccurrenceOf ("  ·  ", false, false);
        if (st.busy)
            line << "  ·  " << (searchPartial ? tr ("{0} folders indexed", { juce::String ((int) st.listed) }) : tr ("checking the index..."));
    }
    else if (st.busy && (st.rebuilding || st.listed > 0))
        line = tr ("Indexing the Browser's folders for search: {0} folders...", { juce::String ((int) st.listed) });

    if (line != statusLine)
    {
        const auto relayout = line.isEmpty() != statusLine.isEmpty();
        statusLine = line;
        if (relayout)
            resized();
        repaint();
    }
}

void BrowserComponent::refreshSearchIndex (bool rebuild)
{
    // the index is shared by every Browser: it covers every folder, hidden ones too
    if (! allRoots.isEmpty())
        searcher->service->refresh (toPaths (allRoots), rebuild);
    ui::postHint ({ rebuild ? tr ("Rebuilding the Browser's search index in the background...")
                            : tr ("Checking the Browser's search index in the background (only changed folders are read again)..."),
                    {}, false, false },
                  4000);
}

juce::String BrowserComponent::searchIndexStatus() const
{
    const auto st = searcher->service->status();
    if (! st.loaded)
        return tr ("Search index: loading...");
    auto text = tr ("Search index: {0} files in {1} folders", { juce::String ((juce::int64) st.files), juce::String ((juce::int64) st.folders) });
    if (st.busy)
        text << " (" << tr ("updating: {0} folders read", { juce::String ((int) st.listed) }) << ")";
    else if (st.checkedAt > 0)
        text << " (" << tr ("checked at {0}", { juce::Time (st.checkedAt).formatted ("%H:%M") }) << ")";
    return text;
}

bool BrowserComponent::renameSelectedPlugin()
{
    return activeList().renameSelectedPlugin();
}

void BrowserComponent::stopPreview()
{
    engine.previewFile ({});
    if (strip != nullptr)
        strip->stop();
    tree->repaint();
    results->repaint();
}

// --- keys --------------------------------------------------------------------------------------------

juce::File BrowserComponent::getSelectedFile() const
{
    return activeList().selectedFile();
}

void BrowserComponent::step (int delta, bool load)
{
    auto& list = activeList();
    const auto count = list.numRows();

    if (count == 0)
        return;

    // the next row in that direction, as in FL: Up / Down walk every row (folders too: Right opens one, Left closes
    // it), previewing each sample; Shift+Up / Down (loading each file into the channel) skips the folders
    auto row = list.getSelectedRow();
    for (int guard = 0; guard < count; ++guard)
    {
        row = row < 0 ? (delta > 0 ? 0 : count - 1) : row + delta;
        if (row < 0 || row >= count)
            return;
        list.select (row, false);
        if (! load || ! list.selectedIsFolder())
            break;
    }

    const auto file = list.selectedFile();

    if (file.existsAsFile())
        fileSelected (file, true);

    if (load && file.existsAsFile() && onLoadIntoChannel)
        onLoadIntoChannel (file);
}

void BrowserComponent::openFolder (bool open)
{
    if (! resultsView->isVisible())
        tree->openSelected (open);
}

void BrowserComponent::loadSelected()
{
    const auto file = getSelectedFile();

    if (! file.existsAsFile())
        return;

    if (file.hasFileExtension ("sf2;sf3"))
    {
        // Enter: into the selected LYNX Soundfont channel, else a new one
        if (! openSoundFontFile (engine, file, engine.getSelectedChannel()))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Load SoundFont"), tr ("This file is not a SoundFont LYNX can read."));
        return;
    }
    if (! onLoadIntoChannel || ! onLoadIntoChannel (file))
        openFile (file);
}

bool BrowserComponent::isAudioFile (const juce::File& file)
{
    return isAudio (file);
}

bool BrowserComponent::isBrowserDrag (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto text = details.description.toString();
    return text == dragDescription || text.startsWith (juce::String (dragDescription) + "|");
}

juce::File BrowserComponent::draggedFile (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto text = details.description.toString();

    if (text.startsWith (juce::String (dragDescription) + "|"))
        return juce::File (text.fromFirstOccurrenceOf ("|", false, false));

    return {};
}

bool BrowserComponent::isGeneratorDrag (const juce::DragAndDropTarget::SourceDetails& details)
{
    return details.description.toString().startsWith (juce::String (generatorDragDescription) + "|") && draggedPlugin().generator != nullptr;
}

bool BrowserComponent::isEffectDrag (const juce::DragAndDropTarget::SourceDetails& details)
{
    return details.description.toString().startsWith (juce::String (effectDragDescription) + "|") && draggedPlugin().effect != nullptr;
}

std::optional<ChannelDef> BrowserComponent::draggedGenerator (const juce::DragAndDropTarget::SourceDetails& details)
{
    return isGeneratorDrag (details) ? draggedPlugin().generator() : std::nullopt;
}

std::optional<EffectSlot> BrowserComponent::draggedEffect (const juce::DragAndDropTarget::SourceDetails& details)
{
    return isEffectDrag (details) ? draggedPlugin().effect() : std::nullopt;
}

int BrowserComponent::draggedPattern (const juce::DragAndDropTarget::SourceDetails& details)
{
    const auto text = details.description.toString();
    if (text.startsWith (juce::String (patternDragDescription) + "|"))
        return text.fromFirstOccurrenceOf ("|", false, false).getIntValue();
    return -1;
}

void BrowserComponent::openFile (const juce::File& file)
{
    // a hosted plugin's preset (round 162d, issue #8): into the plugin it belongs to
    if (pluginpresetui::isPresetFile (file) && (! file.hasFileExtension ("lynxpreset") || pluginpresetui::readLynxPreset (file).has_value()))
    {
        juce::String message;
        if (! pluginpresetui::applyFromBrowser (engine, file, message) && message.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Plugin preset"), message);
        return;
    }
    if (file.hasFileExtension ("sf2;sf3"))
    {
        if (! openSoundFontFile (engine, file))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Load SoundFont"), tr ("This file is not a SoundFont LYNX can read."));
        return;
    }
    if (isAudio (file))
    {
        const auto path = file.getFullPathName().toStdString();
        const auto name = file.getFileNameWithoutExtension().toStdString();
        engine.editSong ([&] (Song& s) {
            ChannelDef c;
            c.name = name;
            c.instrument = InstrumentKind::sampler;
            c.samplePath = path;
            c.mixerTrack = s.claimFreeInsert();
            s.channels.push_back (c);
        });
        engine.setSelectedChannel ((int) engine.getSong().channels.size() - 1);
        return;
    }

    // preset of an FL synth that LYNX converts (3x Osc, Sytrus, GMS...): new channel with the built-in synth
    if (file.hasFileExtension ("fst;gmsynth"))
    {
        juce::MemoryBlock data;

        if (! file.loadFileAsData (data))
            return;

        const auto* bytes = static_cast<const std::uint8_t*> (data.getData());

        if (auto synth = synthFromFlPreset (std::vector<std::uint8_t> (bytes, bytes + data.getSize())))
        {
            const auto name = file.getFileNameWithoutExtension().toStdString();
            engine.editSong ([&] (Song& s) {
                ChannelDef c;
                c.name = name;
                c.instrument = InstrumentKind::synth;
                c.synth = *synth;
                c.mixerTrack = s.claimFreeInsert();
                s.channels.push_back (c);
            });
            engine.setSelectedChannel ((int) engine.getSong().channels.size() - 1);
        }
        else
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Preset"),
                                                    tr ("LYNX reads presets from 3x Osc, Sytrus, Harmor, GMS, Harmless, MiniSynth, DX10, Fruit Kick, Ogun and Sakura."));
        return;
    }

    if (file.hasFileExtension ("lynx;flp;mid;midi") && onOpenProject)
        onOpenProject (file);
}
} // namespace lynx
