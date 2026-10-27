#include "ui/PluginPicker.h"

#include "core/PluginCatalog.h"
#include "core/Vfx.h"
#include "ui/I18n.h"
#include "ui/PluginManager.h"
#include "ui/TextCache.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"
#include "platform/Paths.h"

namespace lynx
{
namespace
{
// the catalog's category names, for the translation table (core/PluginCatalog.cpp names them in English)
[[maybe_unused]] const char* const categoryNames[] = { TR_NOOP ("Synth"),     TR_NOOP ("Sampler"),        TR_NOOP ("Drum"),          TR_NOOP ("Piano & keys"),
                                                       TR_NOOP ("Other"),     TR_NOOP ("Delay & reverb"), TR_NOOP ("Distortion"),    TR_NOOP ("Dynamics"),
                                                       TR_NOOP ("EQ & filter"), TR_NOOP ("Modulation"),   TR_NOOP ("Pitch & vocal"), TR_NOOP ("Analyzer"), TR_NOOP ("Visual"),
                                                       TR_NOOP ("Utility") };

const juce::String star = juce::String::fromUTF8 ("\xe2\x98\x85");

// One row of the left column: a filter.
struct Group
{
    enum class Kind { all, favourites, category, heading, vendor } kind = Kind::all;
    std::string value;
    int count = 0;
};

class PluginPicker final : public juce::Component,
                           private juce::TextEditor::Listener,
                           private juce::KeyListener
{
public:
    PluginPicker (AudioEngine& e, bool instruments, std::function<void (PluginRef)> chosen, std::function<void (ChannelDef)> builtIn)
        : engine (e), onChosen (std::move (chosen)), onBuiltIn (std::move (builtIn))
    {
        for (const auto& type : engine.getPluginHost().getKnownPlugins().getTypes())
            entries.push_back ({ type.createIdentifierString().toStdString(), type.name.toStdString(), type.manufacturerName.toStdString(),
                                 type.pluginFormatName.toStdString(), type.category.toStdString(), type.isInstrument });
        if (onBuiltIn) // FL's VFX note effects, built in (round 164b)
            for (const auto kind : vfx::allKinds)
                entries.push_back ({ builtInPrefix + std::to_string ((int) kind), vfx::flName (kind), "LYNX", "Built-in", "Note effect", true });

        for (auto* b : { &instrumentsTab, &effectsTab })
        {
            b->setClickingTogglesState (false);
            b->setColour (juce::TextButton::buttonOnColourId, theme::colour::accentMuted);
            addAndMakeVisible (*b);
        }
        instrumentsTab.onClick = [this] { setKind (true); };
        effectsTab.onClick = [this] { setKind (false); };

        search.setTextToShowWhenEmpty (tr ("Find (name, vendor, format or category)"), theme::colour::textDisabled);
        search.addListener (this);
        search.addKeyListener (this);
        addAndMakeVisible (search);

        groups.onClick = [this] (int row) {
            if (row >= 0 && row < (int) groupRows.size() && groupRows[(std::size_t) row].kind != Group::Kind::heading)
            {
                group = row;
                refresh();
            }
        };
        addAndMakeVisible (groups);
        list.picker = this;
        addAndMakeVisible (list);

        manageButton.onClick = [this] { showPluginManager (engine, {}); };
        scanButton.onClick = [this] { engine.getPluginHost().startBackgroundScan(); };
        addButton.onClick = [this] { choose (list.getSelectedRow()); };
        for (auto* b : { &manageButton, &scanButton, &addButton })
            addAndMakeVisible (*b);
        countLabel.setFont (theme::font (theme::type::small));
        countLabel.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible (countLabel);

        setKind (instruments);
        setSize (760, 520);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (10);
        auto top = area.removeFromTop (28);
        instrumentsTab.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (4);
        effectsTab.setBounds (top.removeFromLeft (110));
        top.removeFromLeft (10);
        search.setBounds (top);
        area.removeFromTop (8);
        auto bottom = area.removeFromBottom (28);
        addButton.setBounds (bottom.removeFromRight (90));
        bottom.removeFromRight (6);
        scanButton.setBounds (bottom.removeFromRight (130));
        bottom.removeFromRight (6);
        manageButton.setBounds (bottom.removeFromRight (130));
        countLabel.setBounds (bottom);
        area.removeFromBottom (8);
        groups.setBounds (area.removeFromLeft (190));
        area.removeFromLeft (8);
        list.setBounds (area);
    }

    void parentHierarchyChanged() override
    {
        if (isShowing())
            search.grabKeyboardFocus();
    }

    bool keyPressed (const juce::KeyPress& key) override { return handleKey (key); }
    bool keyPressed (const juce::KeyPress& key, juce::Component*) override { return handleKey (key); }

    // --- the plugin list -------------------------------------------------------------------------------------------
    class PluginList final : public juce::ListBox,
                             private juce::ListBoxModel
    {
    public:
        PluginList()
        {
            setModel (this);
            setRowHeight (22);
            setColour (juce::ListBox::backgroundColourId, theme::colour::bg1);
            setOutlineThickness (0);
            setWantsKeyboardFocus (false);
            setHeaderComponent (std::make_unique<Header>());
        }

        PluginPicker* picker = nullptr;

        // the columns: star, name, vendor, format, category (fractions of the width after the star)
        static constexpr int starWidth = 26;
        static juce::Rectangle<int> column (int index, int width, int height)
        {
            const float splits[] = { 0.0f, 0.44f, 0.70f, 0.80f, 1.0f };
            const auto rest = (float) (width - starWidth);
            const auto x0 = starWidth + (int) (rest * splits[index]), x1 = starWidth + (int) (rest * splits[index + 1]);
            return { x0 + 6, 0, x1 - x0 - 8, height };
        }

    private:
        struct Header final : public juce::Component
        {
            Header() { setSize (100, 22); }
            void paint (juce::Graphics& g) override
            {
                g.fillAll (theme::colour::bg3);
                g.setColour (theme::colour::textSecondary);
                g.setFont (theme::font (theme::type::caption, true));
                textcache::drawText (g, star, juce::Rectangle<int> (0, 0, starWidth, getHeight()), juce::Justification::centred);
                const juce::String titles[] = { tr ("Name"), tr ("Vendor"), tr ("Format"), tr ("Category") };
                for (int i = 0; i < 4; ++i)
                    textcache::drawText (g, titles[i], column (i, getWidth(), getHeight()), juce::Justification::centredLeft);
                g.setColour (theme::colour::border);
                g.fillRect (0, getHeight() - 1, getWidth(), 1);
            }
        };

        int getNumRows() override { return picker != nullptr ? (int) picker->shown.size() : 0; }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
        {
            if (picker == nullptr || row < 0 || row >= (int) picker->shown.size())
                return;
            const auto& e = picker->entries[(std::size_t) picker->shown[(std::size_t) row]];
            if (selected)
            {
                g.setColour (theme::colour::accentMuted);
                g.fillRect (0, 0, width, height);
            }
            else if (row % 2 == 1)
            {
                g.setColour (theme::colour::bg2.withAlpha (0.45f));
                g.fillRect (0, 0, width, height);
            }
            const auto favourite = picker->isFavourite (e.id);
            g.setColour (favourite ? theme::colour::accent : theme::colour::textDisabled.withAlpha (0.5f));
            g.setFont (theme::font (theme::type::body));
            textcache::drawText (g, star, juce::Rectangle<int> (0, 0, starWidth, height), juce::Justification::centred);
            const juce::String cells[] = { juce::String::fromUTF8 (e.name.c_str()), juce::String::fromUTF8 (e.vendor.c_str()),
                                           juce::String::fromUTF8 (e.format.c_str()), tr (juce::String (plugincatalog::categoryOf (e))) };
            for (int i = 0; i < 4; ++i)
            {
                g.setColour (i == 0 ? theme::colour::textPrimary : theme::colour::textSecondary);
                g.setFont (theme::font (i == 0 ? theme::type::body : theme::type::small));
                textcache::drawText (g, cells[i], column (i, width, height), juce::Justification::centredLeft, true);
            }
        }

        void listBoxItemClicked (int row, const juce::MouseEvent& e) override
        {
            if (picker != nullptr && e.x < starWidth) // the star: favourite on / off (the Plugin manager's favourites)
                picker->toggleFavourite (row);
        }

        void listBoxItemDoubleClicked (int row, const juce::MouseEvent& e) override
        {
            if (picker != nullptr && e.x >= starWidth)
                picker->choose (row);
        }

        void returnKeyPressed (int row) override
        {
            if (picker != nullptr)
                picker->choose (row);
        }
    };

private:
    // --- the left column -------------------------------------------------------------------------------------------
    class GroupList final : public juce::ListBox,
                            private juce::ListBoxModel
    {
    public:
        GroupList()
        {
            setModel (this);
            setRowHeight (22);
            setColour (juce::ListBox::backgroundColourId, theme::colour::bg1);
            setOutlineThickness (0);
            setWantsKeyboardFocus (false);
        }

        std::vector<Group>* rows = nullptr;
        int* current = nullptr;
        std::function<void (int)> onClick;

    private:
        int getNumRows() override { return rows != nullptr ? (int) rows->size() : 0; }

        void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool) override
        {
            if (rows == nullptr || row < 0 || row >= (int) rows->size())
                return;
            const auto& r = (*rows)[(std::size_t) row];
            if (r.kind == Group::Kind::heading)
            {
                g.setColour (theme::colour::textDisabled);
                g.setFont (theme::font (theme::type::caption, true));
                textcache::drawText (g, tr (juce::String (r.value)).toUpperCase(), juce::Rectangle<int> (8, 0, width - 12, height),
                                     juce::Justification::bottomLeft);
                return;
            }
            const auto selected = current != nullptr && *current == row;
            if (selected)
            {
                g.setColour (theme::colour::accentMuted);
                g.fillRect (0, 0, width, height);
                g.setColour (theme::colour::accent);
                g.fillRect (0, 0, 2, height);
            }
            juce::String name;
            switch (r.kind)
            {
                case Group::Kind::all: name = tr ("All"); break;
                case Group::Kind::favourites: name = star + " " + tr ("Favourites"); break;
                case Group::Kind::category: name = tr (juce::String (r.value)); break;
                case Group::Kind::vendor: name = juce::String::fromUTF8 (r.value.c_str()); break;
                case Group::Kind::heading: break;
            }
            const auto indent = r.kind == Group::Kind::vendor || r.kind == Group::Kind::category ? 16 : 8;
            g.setColour (r.count == 0 ? theme::colour::textDisabled : theme::colour::textPrimary);
            g.setFont (theme::font (theme::type::body, selected));
            textcache::drawText (g, name, juce::Rectangle<int> (indent, 0, width - indent - 36, height), juce::Justification::centredLeft, true);
            g.setColour (theme::colour::textDisabled);
            g.setFont (theme::font (theme::type::caption));
            textcache::drawText (g, juce::String (r.count), juce::Rectangle<int> (width - 40, 0, 32, height), juce::Justification::centredRight);
        }

        void listBoxItemClicked (int row, const juce::MouseEvent&) override
        {
            if (onClick)
                onClick (row);
        }
    };

    void setKind (bool instrumentsNow)
    {
        instruments = instrumentsNow;
        instrumentsTab.setToggleState (instruments, juce::dontSendNotification);
        effectsTab.setToggleState (! instruments, juce::dontSendNotification);
        groupRows.clear();
        const auto& favourites = engine.getPluginHost().favourites();
        groupRows.push_back ({ Group::Kind::all, {}, plugincatalog::countIn (entries, instruments, {}) });
        int favouriteCount = 0;
        for (const auto& e : entries)
            favouriteCount += e.instrument == instruments && favourites.count (e.id) > 0 ? 1 : 0;
        groupRows.push_back ({ Group::Kind::favourites, {}, favouriteCount });
        groupRows.push_back ({ Group::Kind::heading, TR_NOOP ("Categories"), 0 });
        for (const auto& c : plugincatalog::categories (instruments))
            groupRows.push_back ({ Group::Kind::category, c, plugincatalog::countIn (entries, instruments, c) });
        groupRows.push_back ({ Group::Kind::heading, TR_NOOP ("Vendors"), 0 });
        for (const auto& v : plugincatalog::vendors (entries, instruments))
        {
            int n = 0;
            for (const auto& e : entries)
                n += e.instrument == instruments && e.vendor == v ? 1 : 0;
            groupRows.push_back ({ Group::Kind::vendor, v, n });
        }
        groups.rows = &groupRows;
        groups.current = &group;
        group = 0;
        groups.updateContent();
        groups.repaint();
        refresh();
    }

    void refresh()
    {
        plugincatalog::Filter f;
        f.instruments = instruments;
        f.query = search.getText().toStdString();
        if (group >= 0 && group < (int) groupRows.size())
        {
            const auto& r = groupRows[(std::size_t) group];
            f.favouritesOnly = r.kind == Group::Kind::favourites;
            if (r.kind == Group::Kind::category)
                f.category = r.value;
            if (r.kind == Group::Kind::vendor)
                f.vendor = r.value;
        }
        shown = plugincatalog::select (entries, engine.getPluginHost().favourites(), f);
        list.updateContent();
        list.selectRow (shown.empty() ? -1 : 0);
        list.repaint();
        groups.repaint();
        const auto total = plugincatalog::countIn (entries, instruments, {});
        countLabel.setText (instruments ? tr ("{0} of {1} instruments", { juce::String ((int) shown.size()), juce::String (total) })
                                        : tr ("{0} of {1} effects", { juce::String ((int) shown.size()), juce::String (total) }),
                            juce::dontSendNotification);
        addButton.setEnabled (! shown.empty());
    }

    bool isFavourite (const std::string& id) const { return engine.getPluginHost().favourites().count (id) > 0; }

    void toggleFavourite (int row)
    {
        if (row < 0 || row >= (int) shown.size())
            return;
        const auto& id = entries[(std::size_t) shown[(std::size_t) row]].id;
        engine.getPluginHost().setFavourite (id, ! isFavourite (id));
        // the counts change; the list keeps its order until the next filter (the row does not jump away)
        for (auto& r : groupRows)
            if (r.kind == Group::Kind::favourites)
            {
                r.count = 0;
                for (const auto& e : entries)
                    r.count += e.instrument == instruments && isFavourite (e.id) ? 1 : 0;
            }
        list.repaintRow (row);
        groups.repaint();
    }

    void choose (int row)
    {
        if (row < 0 || row >= (int) shown.size() || ! onChosen)
            return;
        const auto& e = entries[(std::size_t) shown[(std::size_t) row]];
        if (e.id.rfind (builtInPrefix, 0) == 0)
        {
            ChannelDef c;
            vfx::makeChannel (c, (vfx::Kind) std::stoi (e.id.substr (builtInPrefix.size())));
            auto chosen = std::move (onBuiltIn);
            onChosen = nullptr;
            closeToolWindowOf (*this);
            if (chosen)
                chosen (std::move (c));
            return;
        }
        PluginRef ref;
        ref.id = e.id;
        ref.name = e.name;
        auto chosen = std::move (onChosen);
        onChosen = nullptr;
        closeToolWindowOf (*this); // (deletes this picker: nothing of it is used below)
        chosen (ref);
    }

    bool handleKey (const juce::KeyPress& key)
    {
        if (key == juce::KeyPress::returnKey)
        {
            choose (list.getSelectedRow());
            return true;
        }
        if (key == juce::KeyPress::upKey || key == juce::KeyPress::downKey)
        {
            const auto count = (int) shown.size();
            if (count > 0)
            {
                const auto row = juce::jlimit (0, count - 1, list.getSelectedRow() + (key == juce::KeyPress::upKey ? -1 : 1));
                list.selectRow (row);
                list.scrollToEnsureRowIsOnscreen (row);
            }
            return true;
        }
        if (key == juce::KeyPress::tabKey)
        {
            setKind (! instruments);
            return true;
        }
        return false;
    }

    void textEditorTextChanged (juce::TextEditor&) override { refresh(); }
    void textEditorReturnKeyPressed (juce::TextEditor&) override { choose (list.getSelectedRow()); }

    AudioEngine& engine;
    std::function<void (PluginRef)> onChosen;
    std::function<void (ChannelDef)> onBuiltIn;
    inline static const std::string builtInPrefix = "lynx:vfx:";
    std::vector<plugincatalog::Entry> entries;
    std::vector<int> shown;
    std::vector<Group> groupRows;
    int group = 0;
    bool instruments = true;
    juce::TextButton instrumentsTab { tr ("Instruments") }, effectsTab { tr ("Effects") };
    juce::TextEditor search;
    GroupList groups;
    PluginList list;
    juce::TextButton manageButton { tr ("Manage plugins...") }, scanButton { tr ("Scan for new plugins") }, addButton { tr ("Add") };
    juce::Label countLabel;
};
} // namespace

void showPluginPicker (AudioEngine& engine, bool instruments, std::function<void (PluginRef)> onChosen, std::function<void (ChannelDef)> onBuiltIn)
{
    openToolWindow (std::make_unique<PluginPicker> (engine, instruments, std::move (onChosen), instruments ? std::move (onBuiltIn) : nullptr),
                    tr ("Plugin picker"), "pluginpicker");
}
} // namespace lynx
