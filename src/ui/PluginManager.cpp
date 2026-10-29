#include "ui/PluginManager.h"

#include "core/Preferences.h"
#include "engine/AudioEngine.h"
#include "engine/PluginHost.h"
#include "platform/Paths.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
juce::String u8 (const std::string& s) { return juce::String::fromUTF8 (s.c_str()); }

enum Column
{
    favouriteColumn = 1,
    nameColumn,
    vendorColumn,
    typeColumn,
    formatColumn,
    statusColumn,
    fileColumn
};

juce::String statusText (prefs::PluginStatus s)
{
    switch (s)
    {
        case prefs::PluginStatus::failed: return tr ("Failed to load");
        case prefs::PluginStatus::missingFile: return tr ("File not found");
        case prefs::PluginStatus::ok: break;
    }
    return trc ("@plugin status: OK");
}

// sample rows for --snapshot (free plugins; one failed, one moved away)
std::vector<prefs::PluginRow> sampleRows()
{
    using prefs::PluginStatus;
    return {
        { "Vital", "Vital Audio", "VST3", "Synth", "C:\\Program Files\\Common Files\\VST3\\Vital.vst3", "VST3-Vital", true, PluginStatus::ok, true },
        { "Surge XT", "Surge Synth Team", "CLAP", "Synth", "C:\\Program Files\\Common Files\\CLAP\\Surge XT.clap", "CLAP-Surge", true, PluginStatus::ok, true },
        { "Dexed", "Digital Suburban", "VST3", "Synth", "C:\\Program Files\\Common Files\\VST3\\Dexed.vst3", "VST3-Dexed", true, PluginStatus::ok, false },
        { "TDR Nova", "Tokyo Dawn Labs", "VST3", "EQ", "C:\\Program Files\\Common Files\\VST3\\TDR Nova.vst3", "VST3-Nova", false, PluginStatus::ok, true },
        { "Valhalla Supermassive", "Valhalla DSP", "VST3", "Reverb", "C:\\Program Files\\Common Files\\VST3\\ValhallaSupermassive.vst3", "VST3-VS", false, PluginStatus::ok, false },
        { "OTT", "Xfer Records", "VST3", "Dynamics", "C:\\Program Files\\Common Files\\VST3\\OTT.vst3", "VST3-OTT", false, PluginStatus::ok, false },
        { "Surge XT Effects", "Surge Synth Team", "CLAP", "Fx", "C:\\Program Files\\Common Files\\CLAP\\Surge XT Effects.clap", "CLAP-SurgeFx", false, PluginStatus::ok, false },
        { "Spectral Gate", "", "VST3", "", "D:\\Old plugins\\SpectralGate.vst3", "", false, PluginStatus::failed, false },
        { "Analog Strings", "Free Instruments", "VST3", "Synth", "E:\\VST3\\Analog Strings.vst3", "VST3-AS", true, PluginStatus::missingFile, false },
    };
}

class PluginManagerPanel final : public juce::Component,
                                 private juce::TableListBoxModel,
                                 private juce::Timer
{
public:
    PluginManagerPanel (AudioEngine& e, bool keep, std::function<void()> folders)
        : host (e.getPluginHost()), persistent (keep), onFolders (std::move (folders))
    {
        search.setTextToShowWhenEmpty (tr ("Search name, vendor, format or file"), theme::colour::textDisabled);
        search.setFont (theme::font (theme::type::body));
        search.setIndents (8, 6);
        search.onTextChange = [this] { refilter(); };
        addAndMakeVisible (search);

        const char* filterNames[] = { TR_NOOP ("All"), TR_NOOP ("Instruments"), TR_NOOP ("Effects"), TR_NOOP ("Favourites"), TR_NOOP ("Failed") };
        for (int i = 0; i < 5; ++i)
        {
            auto& b = filterButtons[(std::size_t) i];
            b.setButtonText (tr (filterNames[i]));
            b.setClickingTogglesState (true);
            b.setRadioGroupId (8201);
            b.setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0) | (i < 4 ? juce::Button::ConnectedOnRight : 0));
            b.onClick = [this, i] {
                if (filterButtons[(std::size_t) i].getToggleState())
                {
                    filter = (prefs::PluginFilter) i;
                    refilter();
                }
            };
            addAndMakeVisible (b);
        }
        filterButtons[0].setToggleState (true, juce::dontSendNotification);

        count.setFont (theme::font (theme::type::small));
        count.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        count.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (count);

        auto& header = table.getHeader();
        const auto flags = juce::TableHeaderComponent::visible | juce::TableHeaderComponent::resizable | juce::TableHeaderComponent::sortable;
        header.addColumn (juce::String::fromUTF8 ("\xe2\x98\x85"), favouriteColumn, 30, 30, 30, juce::TableHeaderComponent::visible);
        header.addColumn (tr ("Name"), nameColumn, 190, 80, 400, flags);
        header.addColumn (tr ("Vendor"), vendorColumn, 140, 60, 300, flags);
        header.addColumn (tr ("Type"), typeColumn, 90, 60, 160, flags);
        header.addColumn (tr ("Format"), formatColumn, 64, 50, 100, flags);
        header.addColumn (tr ("Status"), statusColumn, 110, 70, 200, flags);
        header.addColumn (tr ("File"), fileColumn, 300, 80, 2000, flags);
        header.setSortColumnId (nameColumn, true);
        header.setStretchToFitActive (true);
        header.setColour (juce::TableHeaderComponent::backgroundColourId, theme::colour::bg3);
        header.setColour (juce::TableHeaderComponent::textColourId, theme::colour::textSecondary);
        header.setColour (juce::TableHeaderComponent::outlineColourId, theme::colour::border);
        table.setModel (this);
        table.setRowHeight (26);
        table.setHeaderHeight (26);
        table.setMultipleSelectionEnabled (false);
        table.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        table.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        table.setOutlineThickness (1);
        addAndMakeVisible (table);
        // an empty list is not "no plugins": LYNX's own instruments and effects are never listed here (GitHub issue #2)
        emptyHint.setText (tr ("No third-party plugins found yet ({0}). Press Find new plugins, or add folders with Plugin folders...", { platform::pluginFormatNames() })
                               + "\n\n"
                               + tr ("LYNX's own instruments and effects are not listed here: they are always available from the Channel Rack's + button, "
                                     "a mixer insert slot or the Quick-add palette (F8)."),
                           juce::dontSendNotification);
        emptyHint.setFont (theme::font (theme::type::body));
        emptyHint.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        emptyHint.setJustificationType (juce::Justification::centred);
        emptyHint.setInterceptsMouseClicks (false, false);
        addChildComponent (emptyHint);

        progress.setColour (juce::ProgressBar::foregroundColourId, theme::colour::accent);
        progress.setPercentageDisplay (false);
        addChildComponent (progress);
        scanStatus.setFont (theme::font (theme::type::small));
        scanStatus.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        scanStatus.setJustificationType (juce::Justification::centredLeft);
        addAndMakeVisible (scanStatus);

        scanNew.onClick = [this] { startScan (false); };
        rescanAll.onClick = [this] { confirmRescanAll(); };
        stop.onClick = [this] { host.cancelBackgroundScan(); };
        foldersButton.onClick = [this] {
            if (onFolders)
                onFolders();
        };
        addFile.onClick = [this] { chooseFile(); };
        for (auto* b : { &scanNew, &rescanAll, &stop, &foldersButton, &addFile })
            addAndMakeVisible (b);
        scanNew.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        scanNew.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);

        lastFinished = host.backgroundScan().finished;
        reload();
        updateScan();
        if (persistent)
            startTimerHz (4);
        setSize (980, 600);
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 12);
        auto top = area.removeFromTop (30);
        search.setBounds (top.removeFromLeft (juce::jmin (300, top.getWidth() / 3)));
        top.removeFromLeft (12);
        for (auto& b : filterButtons)
            b.setBounds (top.removeFromLeft (96));
        count.setBounds (top);
        area.removeFromTop (10);

        auto bottom = area.removeFromBottom (30);
        addFile.setBounds (bottom.removeFromRight (150));
        bottom.removeFromRight (8);
        foldersButton.setBounds (bottom.removeFromRight (140));
        bottom.removeFromRight (16);
        scanNew.setBounds (bottom.removeFromLeft (170));
        bottom.removeFromLeft (8);
        rescanAll.setBounds (bottom.removeFromLeft (120));
        bottom.removeFromLeft (8);
        stop.setBounds (bottom.removeFromLeft (80));
        area.removeFromBottom (8);
        auto status = area.removeFromBottom (22);
        if (progress.isVisible())
        {
            progress.setBounds (status.removeFromLeft (200).reduced (0, 5));
            status.removeFromLeft (10);
        }
        scanStatus.setBounds (status);
        area.removeFromBottom (6);
        table.setBounds (area);
        emptyHint.setBounds (area.withTrimmedTop (table.getHeaderHeight()).reduced (40, 20));
    }

private:
    // --- data --------------------------------------------------------------------------------------------------
    void reload()
    {
        if (! persistent)
        {
            rows = sampleRows();
            refilter();
            return;
        }

        rows.clear();
        const auto& favourites = host.favourites();
        for (const auto& t : host.getKnownPlugins().getTypes())
        {
            prefs::PluginRow r;
            r.name = t.name.toStdString();
            r.vendor = t.manufacturerName.toStdString();
            r.format = t.pluginFormatName.toStdString();
            r.category = t.category.toStdString();
            r.file = t.fileOrIdentifier.toStdString();
            r.id = t.createIdentifierString().toStdString();
            r.instrument = t.isInstrument;
            r.favourite = favourites.count (r.id) > 0;
            if (juce::File::isAbsolutePath (t.fileOrIdentifier) && ! juce::File (t.fileOrIdentifier).exists())
                r.status = prefs::PluginStatus::missingFile;
            rows.push_back (r);
        }
        for (const auto& f : host.getKnownPlugins().getBlacklistedFiles())
        {
            prefs::PluginRow r;
            r.name = juce::File::createFileWithoutCheckingPath (f).getFileNameWithoutExtension().toStdString();
            r.format = juce::File::createFileWithoutCheckingPath (f).hasFileExtension ("clap") ? "CLAP" : "VST3";
            r.file = f.toStdString();
            r.status = prefs::PluginStatus::failed;
            rows.push_back (r);
        }
        refilter();
    }

    void refilter()
    {
        auto sort = prefs::PluginSort::name;
        switch (table.getHeader().getSortColumnId())
        {
            case vendorColumn: sort = prefs::PluginSort::vendor; break;
            case typeColumn: sort = prefs::PluginSort::type; break;
            case formatColumn: sort = prefs::PluginSort::format; break;
            case statusColumn: sort = prefs::PluginSort::status; break;
            default: break;
        }
        visible = prefs::visiblePlugins (rows, search.getText().toStdString(), filter, sort, ! table.getHeader().isSortedForwards());
        table.updateContent();
        table.repaint();
        emptyHint.setVisible (persistent && rows.empty());

        int instruments = 0, effects = 0, failed = 0;
        for (const auto& r : rows)
            (r.status != prefs::PluginStatus::ok ? failed : r.instrument ? instruments : effects)++;
        count.setText (tr ("{0} instruments, {1} effects, {2} with problems", { juce::String (instruments), juce::String (effects), juce::String (failed) }),
                       juce::dontSendNotification);
    }

    const prefs::PluginRow* rowAt (int row) const
    {
        return juce::isPositiveAndBelow (row, (int) visible.size()) ? &rows[(std::size_t) visible[(std::size_t) row]] : nullptr;
    }

    // --- table -------------------------------------------------------------------------------------------------
    int getNumRows() override { return (int) visible.size(); }

    void paintRowBackground (juce::Graphics& g, int row, int, int, bool selected) override
    {
        if (selected)
            g.fillAll (theme::colour::accentMuted);
        else if (row % 2 == 1)
            g.fillAll (theme::colour::bg1.withAlpha (0.5f));
    }

    void paintCell (juce::Graphics& g, int row, int column, int width, int height, bool) override
    {
        const auto* r = rowAt (row);
        if (r == nullptr)
            return;
        const auto ok = r->status == prefs::PluginStatus::ok;
        g.setFont (theme::font (theme::type::body));
        auto text = [&] (const juce::String& s, juce::Colour c) {
            g.setColour (c);
            g.drawText (s, 8, 0, width - 12, height, juce::Justification::centredLeft, true);
        };
        switch (column)
        {
            case favouriteColumn:
                if (r->id.empty())
                    break;
                g.setColour (r->favourite ? theme::colour::warning : theme::colour::textDisabled.withAlpha (0.6f));
                g.setFont (theme::font (theme::type::title));
                g.drawText (juce::String::fromUTF8 (r->favourite ? "\xe2\x98\x85" : "\xe2\x98\x86"), 0, 0, width, height, juce::Justification::centred);
                break;
            case nameColumn: text (u8 (r->name), ok ? theme::colour::textPrimary : theme::colour::textSecondary); break;
            case vendorColumn: text (u8 (r->vendor), theme::colour::textSecondary); break;
            case typeColumn:
                text (r->status == prefs::PluginStatus::failed ? juce::String ("-") : r->instrument ? tr ("Instrument") : tr ("Effect"),
                      theme::colour::textSecondary);
                break;
            case formatColumn: text (u8 (r->format), theme::colour::textSecondary); break;
            case statusColumn:
            {
                const auto colour = ok ? theme::colour::ledOn : r->status == prefs::PluginStatus::failed ? theme::colour::accent : theme::colour::warning;
                g.setColour (colour);
                g.fillEllipse (8.0f, height * 0.5f - 3.5f, 7.0f, 7.0f);
                g.setColour (ok ? theme::colour::textSecondary : colour);
                g.drawText (statusText (r->status), 22, 0, width - 26, height, juce::Justification::centredLeft, true);
                break;
            }
            case fileColumn:
                g.setFont (theme::font (theme::type::small));
                text (u8 (r->file), theme::colour::textDisabled);
                break;
            default: break;
        }
    }

    void sortOrderChanged (int, bool) override { refilter(); }

    void cellClicked (int row, int column, const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
        {
            table.selectRow (row);
            showRowMenu (row);
            return;
        }
        if (column == favouriteColumn)
            toggleFavourite (row);
    }

    void cellDoubleClicked (int row, int column, const juce::MouseEvent&) override
    {
        if (column != favouriteColumn)
            toggleFavourite (row);
    }

    void backgroundClicked (const juce::MouseEvent&) override { table.deselectAllRows(); }

    juce::String getCellTooltip (int row, int column) override
    {
        const auto* r = rowAt (row);
        if (r == nullptr)
            return {};
        if (column == favouriteColumn)
            return tr ("Favourite: listed first when adding a plugin");
        if (column == statusColumn && r->status == prefs::PluginStatus::failed)
            return tr ("The scan couldn't load this file (it crashed, hung or isn't a plugin). Right-click > Try again.");
        return u8 (r->file);
    }

    // --- actions -----------------------------------------------------------------------------------------------
    void toggleFavourite (int row)
    {
        const auto* r = rowAt (row);
        if (r == nullptr || r->id.empty())
            return;
        const auto on = ! r->favourite;
        if (persistent)
            host.setFavourite (r->id, on);
        rows[(std::size_t) visible[(std::size_t) row]].favourite = on;
        if (filter == prefs::PluginFilter::favourites)
            refilter();
        else
            table.repaintRow (row);
    }

    void showRowMenu (int row)
    {
        const auto* r = rowAt (row);
        if (r == nullptr)
            return;
        const auto file = u8 (r->file);
        juce::PopupMenu menu;
        if (! r->id.empty())
            menu.addItem (r->favourite ? tr ("Remove from favourites") : tr ("Add to favourites"), [this, row] { toggleFavourite (row); });
        if (r->status == prefs::PluginStatus::failed)
            menu.addItem (tr ("Try again (scan this file now)"), persistent && ! host.backgroundScan().running, false, [this, file] {
                host.forgetFile (file);
                host.startBackgroundScanOfFiles (juce::StringArray { file });
                reload();
                updateScan();
            });
        menu.addItem (tr ("Remove from the list"), persistent, false, [this, file] {
            host.forgetFile (file);
            reload();
        });
        menu.addSeparator();
        menu.addItem (tr ("Show in folder"), juce::File::isAbsolutePath (file) && juce::File (file).exists(), false,
                      [file] { juce::File (file).revealToUser(); });
        menu.addItem (tr ("Copy path"), [file] { juce::SystemClipboard::copyTextToClipboard (file); });
        menu.showMenuAsync (juce::PopupMenu::Options().withMousePosition());
    }

    void startScan (bool all)
    {
        if (! persistent || host.backgroundScan().running)
            return;
        if (all)
            host.forgetAll();
        host.startBackgroundScan();
        reload();
        updateScan();
    }

    void confirmRescanAll()
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, tr ("Rescan all plugins"),
                                            tr ("LYNX forgets every plugin it found (and the ones that failed) and examines all plugin folders "
                                                "again. This can take a few minutes; LYNX stays usable meanwhile."),
                                            tr ("Rescan"), tr ("Cancel"), this,
                                            juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<PluginManagerPanel> (this)] (int result) {
                                                if (safe != nullptr && result == 1)
                                                    safe->startScan (true);
                                            }));
    }

    void chooseFile()
    {
        chooser = std::make_unique<juce::FileChooser> (tr ("Add a plugin file"), juce::File::getSpecialLocation (juce::File::userHomeDirectory),
                                                       "*.vst3;*.clap");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [safe = juce::Component::SafePointer<PluginManagerPanel> (this)] (const juce::FileChooser& c) {
                                  if (safe == nullptr || c.getResult() == juce::File() || ! safe->persistent)
                                      return;
                                  safe->host.forgetFile (c.getResult().getFullPathName());
                                  safe->host.startBackgroundScanOfFiles (juce::StringArray { c.getResult().getFullPathName() });
                                  safe->updateScan();
                              });
    }

    void updateScan()
    {
        const auto state = persistent ? host.backgroundScan() : PluginHost::ScanState {};
        progressValue = state.progress;
        if (progress.isVisible() != state.running)
        {
            progress.setVisible (state.running);
            resized();
        }
        stop.setEnabled (state.running);
        scanNew.setEnabled (persistent && ! state.running);
        rescanAll.setEnabled (persistent && ! state.running);
        addFile.setEnabled (persistent && ! state.running);
        if (state.running)
            scanStatus.setText (tr ("Scanning: {0}", { state.file }), juce::dontSendNotification);
        else if (state.finished > 0)
            scanStatus.setText (state.found == 1 ? tr ("Last scan: 1 new plugin.") : tr ("Last scan: {0} new plugins.", { juce::String (state.found) }),
                                juce::dontSendNotification);
        else
            scanStatus.setText (tr ("Each plugin file is examined in a separate process: one that crashes is marked as failed and skipped later."),
                                juce::dontSendNotification);
    }

    void timerCallback() override
    {
        const auto state = host.backgroundScan();
        updateScan();
        if (state.finished != lastFinished)
        {
            lastFinished = state.finished;
            reload();
        }
        else if (state.running && ++ticks % 8 == 0)
            reload(); // show what was found so far
    }

    PluginHost& host;
    bool persistent;
    std::function<void()> onFolders;
    std::vector<prefs::PluginRow> rows;
    std::vector<int> visible;
    prefs::PluginFilter filter = prefs::PluginFilter::all;
    int lastFinished = 0, ticks = 0;
    double progressValue = 0.0;

    juce::TextEditor search;
    std::array<juce::TextButton, 5> filterButtons;
    juce::Label count, scanStatus, emptyHint;
    juce::TableListBox table;
    juce::ProgressBar progress { progressValue };
    juce::TextButton scanNew { tr ("Find new plugins") }, rescanAll { tr ("Rescan all...") }, stop { tr ("Stop") },
        foldersButton { tr ("Plugin folders...") }, addFile { tr ("Add plugin file...") };
    std::unique_ptr<juce::FileChooser> chooser;
};
} // namespace

void showPluginManager (AudioEngine& engine, std::function<void()> onFolders)
{
    openToolWindow (std::make_unique<PluginManagerPanel> (engine, true, std::move (onFolders)), tr ("Plugin manager"), "plugin-manager");
}

std::unique_ptr<juce::Component> makePluginManagerPanel (AudioEngine& engine, bool persistent, std::function<void()> onFolders)
{
    return std::make_unique<PluginManagerPanel> (engine, persistent, std::move (onFolders));
}
} // namespace lynx
