#include "ui/PluginMonitorWindow.h"

#include "core/PluginMonitor.h"
#include "core/RackRenderer.h"
#include "engine/AudioEngine.h"
#include "ui/I18n.h"
#include "ui/SettingsPages.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
namespace
{
using namespace settingsui;

juce::String percent (double v)
{
    return v < 0.05 ? juce::String ("-") : v < 10.0 ? juce::String (v, 1) + "%" : juce::String (juce::roundToInt (v)) + "%";
}

juce::Colour loadColour (double v)
{
    return v < 25.0 ? theme::colour::meterLow : v < 60.0 ? theme::colour::meterMid : theme::colour::meterClip;
}

class MonitorTable final : public juce::Component
{
public:
    std::vector<perfmon::Row> rows;
    perfmon::SortBy sort = perfmon::SortBy::current;
    std::function<void()> onSort;
    std::function<juce::String (const perfmon::Source&)> describe;
    static constexpr int headerHeight = 24, rowHeight = 22;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg0);
        const auto cols = columns();
        // header
        g.setColour (theme::colour::bg3);
        g.fillRect (0, 0, getWidth(), headerHeight);
        g.setFont (theme::font (theme::type::small, true));
        const juce::String names[] = { tr ("Plugin"), tr ("Where"), tr ("Now"), tr ("Average"), tr ("Peak") };
        const perfmon::SortBy keys[] = { perfmon::SortBy::name, perfmon::SortBy::name, perfmon::SortBy::current, perfmon::SortBy::average, perfmon::SortBy::peak };
        for (int c = 0; c < 5; ++c)
        {
            const auto on = keys[c] == sort && c != 1;
            g.setColour (on ? theme::colour::textPrimary : theme::colour::textSecondary);
            g.drawText (names[c].toUpperCase() + (on ? juce::String::fromUTF8 (c == 0 ? " \xe2\x96\xb4" : " \xe2\x96\xbe") : juce::String()),
                        cols[c].withTrimmedLeft (8).withTrimmedRight (8).withHeight (headerHeight), c >= 2 ? juce::Justification::centredRight : juce::Justification::centredLeft);
        }
        g.setColour (theme::colour::border);
        g.fillRect (0, headerHeight - 1, getWidth(), 1);

        if (rows.empty())
        {
            g.setColour (theme::colour::textDisabled);
            g.setFont (theme::font (theme::type::body));
            g.drawText (tr ("Nothing to show"), getLocalBounds().withTrimmedTop (headerHeight), juce::Justification::centred);
            return;
        }

        for (int i = 0; i < (int) rows.size(); ++i)
        {
            const auto y = headerHeight + i * rowHeight;
            if (y > getHeight())
                break;
            const auto& r = rows[(std::size_t) i];
            if (i % 2 == 1)
            {
                g.setColour (theme::colour::bg1);
                g.fillRect (0, y, getWidth(), rowHeight);
            }
            g.setFont (theme::font (theme::type::body, r.source.plugin));
            g.setColour (theme::colour::textPrimary);
            g.drawText (juce::String::fromUTF8 (r.source.name.c_str()), cols[0].withY (y).withHeight (rowHeight).reduced (8, 0), juce::Justification::centredLeft, true);
            g.setFont (theme::font (theme::type::small));
            g.setColour (theme::colour::textSecondary);
            g.drawText (describe ? describe (r.source) : juce::String::fromUTF8 (r.source.where.c_str()), cols[1].withY (y).withHeight (rowHeight).reduced (8, 0),
                        juce::Justification::centredLeft, true);

            // "now": a bar under the number (full width = 100% of one core)
            auto now = cols[2].withY (y).withHeight (rowHeight).reduced (8, 5);
            g.setColour (theme::colour::bg4);
            g.fillRoundedRectangle (now.toFloat(), 2.0f);
            g.setColour (loadColour (r.current).withAlpha (0.55f));
            g.fillRoundedRectangle (now.toFloat().withWidth ((float) now.getWidth() * (float) juce::jlimit (0.0, 1.0, r.current / 100.0)), 2.0f);
            g.setColour (theme::colour::textPrimary);
            g.setFont (theme::monoFont (theme::type::small));
            g.drawText (percent (r.current), now.reduced (4, 0), juce::Justification::centredRight);
            g.setColour (theme::colour::textSecondary);
            g.drawText (percent (r.average), cols[3].withY (y).withHeight (rowHeight).reduced (8, 0), juce::Justification::centredRight);
            g.setColour (r.peak >= 60.0 ? theme::colour::meterClip : theme::colour::textSecondary);
            g.drawText (percent (r.peak), cols[4].withY (y).withHeight (rowHeight).reduced (8, 0), juce::Justification::centredRight);
        }
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (e.y >= headerHeight)
            return;
        const auto cols = columns();
        const perfmon::SortBy keys[] = { perfmon::SortBy::name, perfmon::SortBy::name, perfmon::SortBy::current, perfmon::SortBy::average, perfmon::SortBy::peak };
        for (int c = 0; c < 5; ++c)
            if (cols[c].contains (e.x, cols[c].getY()))
            {
                sort = keys[c];
                if (onSort)
                    onSort();
            }
    }

    int idealHeight() const { return headerHeight + juce::jmax (1, (int) rows.size()) * rowHeight; }

private:
    std::array<juce::Rectangle<int>, 5> columns() const
    {
        const auto w = getWidth();
        const auto num = 92;
        const auto name = juce::jmax (120, (w - 3 * num) * 45 / 100);
        std::array<juce::Rectangle<int>, 5> c;
        c[0] = { 0, 0, name, getHeight() };
        c[1] = { name, 0, w - name - 3 * num, getHeight() };
        c[2] = { w - 3 * num, 0, num + 20, getHeight() };
        c[3] = { w - 2 * num + 20, 0, num - 20, getHeight() };
        c[4] = { w - num, 0, num, getHeight() };
        return c;
    }
};

class PluginMonitorPanel final : public juce::Component,
                                 private juce::Timer
{
public:
    PluginMonitorPanel (AudioEngine& e, bool live) : engine (e), measuring (live)
    {
        styleLabel (findLabel, tr ("Find"));
        addAndMakeVisible (findLabel);
        styleEditor (find);
        find.setTextToShowWhenEmpty (tr ("plugin, channel or insert"), theme::colour::textDisabled);
        find.onTextChange = [this] { refreshRows(); };
        addAndMakeVisible (find);
        pluginsOnly.setButtonText (tr ("Plugins only"));
        pluginsOnly.setTooltip (tr ("Only third-party plugins (VST3 / CLAP), not LYNX SHARE's own instruments and effects"));
        pluginsOnly.onClick = [this] { refreshRows(); };
        addAndMakeVisible (pluginsOnly);
        freeze.setButtonText (tr ("Freeze"));
        freeze.setTooltip (tr ("Stops updating the numbers"));
        addAndMakeVisible (freeze);
        resetOnPlay.setButtonText (tr ("Reset on play"));
        resetOnPlay.setTooltip (tr ("Averages and peaks start again each time playback starts"));
        resetOnPlay.setToggleState (true, juce::dontSendNotification);
        addAndMakeVisible (resetOnPlay);
        reset.onClick = [this] {
            monitor.reset();
            refreshRows();
        };
        addAndMakeVisible (reset);

        total.setFont (theme::font (theme::type::body, true));
        total.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (total);
        note.setFont (theme::font (theme::type::small));
        note.setColour (juce::Label::textColourId, theme::colour::textDisabled);
        note.setText (tr ("100% = one processor core busy for the whole time the audio lasts"), juce::dontSendNotification);
        addAndMakeVisible (note);

        table.describe = [this] (const perfmon::Source& s) { return describe (s); };
        table.onSort = [this] { refreshRows(); };
        viewport.setViewedComponent (&table, false);
        viewport.setScrollBarsShown (true, false);
        addAndMakeVisible (viewport);

        if (measuring)
        {
            if (auto* times = engine.getProcessTimes())
                times->enabled.store (true);
            startTimerHz (4);
        }
        timerCallback();
        setSize (720, 380);
    }

    ~PluginMonitorPanel() override
    {
        if (measuring)
            if (auto* times = engine.getProcessTimes())
                times->enabled.store (false);
    }

    // --snapshot: plausible numbers for the demo project
    void fillWithSamples()
    {
        const auto list = perfmon::sources (engine.getSong());
        std::vector<std::uint64_t> busy (perfmon::keyCount, 0);
        monitor.update (list, busy, 0, 0);
        std::uint64_t audio = 0, wall = 0;
        for (int step = 1; step <= 6; ++step)
        {
            audio += 250'000'000;
            std::uint64_t sum = 0;
            for (const auto& s : list)
            {
                const auto weight = (double) ((s.key * 2654435761u) % 97) / 97.0;
                const auto load = (s.plugin ? 0.09 : 0.012) + weight * weight * (s.track >= 0 ? 0.05 : 0.035) * (1.0 + 0.3 * std::sin (step + s.key));
                const auto ns = (std::uint64_t) (load * 250'000'000.0);
                busy[(std::size_t) s.key] += ns;
                sum += ns;
            }
            wall += sum + 4'000'000;
            monitor.update (list, busy, audio, wall);
        }
        refreshRows();
    }

    void paint (juce::Graphics& g) override { g.fillAll (theme::colour::bg2); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (14, 12);
        auto top = area.removeFromTop (28);
        findLabel.setBounds (top.removeFromLeft (44));
        find.setBounds (top.removeFromLeft (200));
        top.removeFromLeft (12);
        pluginsOnly.setBounds (top.removeFromLeft (120));
        reset.setBounds (top.removeFromRight (80));
        top.removeFromRight (8);
        resetOnPlay.setBounds (top.removeFromRight (124));
        top.removeFromRight (6);
        freeze.setBounds (top.removeFromRight (84));
        area.removeFromTop (10);
        auto bottom = area.removeFromBottom (20);
        total.setBounds (bottom.removeFromLeft (bottom.getWidth() / 2));
        note.setBounds (bottom);
        area.removeFromBottom (6);
        viewport.setBounds (area);
        table.setSize (area.getWidth() - (table.idealHeight() > area.getHeight() ? viewport.getScrollBarThickness() : 0), juce::jmax (area.getHeight(), table.idealHeight()));
    }

private:
    juce::String describe (const perfmon::Source& s) const
    {
        const auto& song = engine.getSong();
        if (s.channel >= 0)
        {
            const auto name = s.channel < (int) song.channels.size() ? juce::String::fromUTF8 (song.channels[(std::size_t) s.channel].name.c_str()) : juce::String();
            return tr ("Channel {0}", { juce::String (s.channel + 1) }) + (name.isNotEmpty() && name != juce::String::fromUTF8 (s.name.c_str()) ? " - " + name : juce::String());
        }
        const auto track = s.track == 0 ? tr ("Master") : tr ("Insert {0}", { juce::String (s.track) });
        return track + "  ·  " + tr ("slot {0}", { juce::String (s.slot + 1) });
    }

    void timerCallback() override
    {
        if (! measuring)
            return;
        const auto playing = engine.getTransport().playing.load();
        if (playing && ! wasPlaying && resetOnPlay.getToggleState())
            monitor.reset();
        wasPlaying = playing;
        if (freeze.getToggleState())
            return;

        auto* times = engine.getProcessTimes();
        if (times == nullptr)
            return;
        std::vector<std::uint64_t> busy (perfmon::keyCount, 0);
        for (int c = 0; c < perfmon::channelKeys && c < ProcessTimes::channels; ++c)
            busy[(std::size_t) c] = times->channelNs[(std::size_t) c].load (std::memory_order_relaxed);
        for (std::size_t k = 0; k < times->effectNs.size(); ++k)
            busy[(std::size_t) perfmon::channelKeys + k] = times->effectNs[k].load (std::memory_order_relaxed);
        monitor.update (perfmon::sources (engine.getSong()), busy, times->audioNs.load(), times->wallNs.load());
        refreshRows();
    }

    void refreshRows()
    {
        auto rows = monitor.rows (find.getText().toStdString(), table.sort);
        if (pluginsOnly.getToggleState())
            rows.erase (std::remove_if (rows.begin(), rows.end(), [] (const perfmon::Row& r) { return ! r.source.plugin; }), rows.end());
        table.rows = std::move (rows);
        total.setText (tr ("Whole rack: {0} (peak {1})", { percent (monitor.rackCurrent()), percent (monitor.rackPeak()) }), juce::dontSendNotification);
        resized();
        table.repaint();
    }

    AudioEngine& engine;
    bool measuring = true, wasPlaying = false;
    perfmon::Monitor monitor;
    juce::Label findLabel, total, note;
    juce::TextEditor find;
    juce::ToggleButton pluginsOnly, freeze, resetOnPlay;
    juce::TextButton reset { tr ("Reset") };
    juce::Viewport viewport;
    MonitorTable table;
};
} // namespace

void showPluginMonitor (AudioEngine& engine)
{
    openToolWindow (std::make_unique<PluginMonitorPanel> (engine, true), tr ("Plugin performance monitor"), "plugin-monitor");
}

std::unique_ptr<juce::Component> makePluginMonitorPanel (AudioEngine& engine)
{
    auto panel = std::make_unique<PluginMonitorPanel> (engine, false);
    panel->fillWithSamples();
    return panel;
}
} // namespace lynx
