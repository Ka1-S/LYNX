#include "ui/ShortcutManager.h"

#include "core/SearchQuery.h"
#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"

namespace lynx
{
juce::File keyMapFile()
{
    return PluginHost::settingsFolder().getChildFile ("keys.json");
}

keys::Chord chordFromKeyPress (const juce::KeyPress& key)
{
    keys::Chord c;
    const auto code = key.getKeyCode();
    const auto mods = key.getModifiers();
    c.mods = (std::uint8_t) ((mods.isCommandDown() ? keys::ctrl : 0) | (mods.isShiftDown() ? keys::shift : 0) | (mods.isAltDown() ? keys::alt : 0));

    struct Pair
    {
        int juceCode, key;
    };
    const Pair specials[] = {
        { juce::KeyPress::spaceKey, keys::space },       { juce::KeyPress::returnKey, keys::enter },     { juce::KeyPress::escapeKey, keys::escape },
        { juce::KeyPress::tabKey, keys::tab },           { juce::KeyPress::backspaceKey, keys::backspace }, { juce::KeyPress::deleteKey, keys::del },
        { juce::KeyPress::insertKey, keys::insert },     { juce::KeyPress::upKey, keys::up },             { juce::KeyPress::downKey, keys::down },
        { juce::KeyPress::leftKey, keys::left },         { juce::KeyPress::rightKey, keys::right },       { juce::KeyPress::pageUpKey, keys::pageUp },
        { juce::KeyPress::pageDownKey, keys::pageDown }, { juce::KeyPress::homeKey, keys::home },         { juce::KeyPress::endKey, keys::end },
        { juce::KeyPress::numberPadAdd, keys::numpadPlus }, { juce::KeyPress::numberPadSubtract, keys::numpadMinus },
        { juce::KeyPress::F1Key, keys::f1 },   { juce::KeyPress::F2Key, keys::f2 },   { juce::KeyPress::F3Key, keys::f3 },
        { juce::KeyPress::F4Key, keys::f4 },   { juce::KeyPress::F5Key, keys::f5 },   { juce::KeyPress::F6Key, keys::f6 },
        { juce::KeyPress::F7Key, keys::f7 },   { juce::KeyPress::F8Key, keys::f8 },   { juce::KeyPress::F9Key, keys::f9 },
        { juce::KeyPress::F10Key, keys::f10 }, { juce::KeyPress::F11Key, keys::f11 }, { juce::KeyPress::F12Key, keys::f12 },
    };

    for (const auto& p : specials)
        if (code == p.juceCode)
        {
            c.key = p.key;
            return c;
        }

    if (const auto letter = keys::letterOfKeyCode (code))
        c.key = letter;

    return c;
}

namespace
{
juce::String contextName (keys::Context c)
{
    switch (c)
    {
        case keys::Context::global: return tr ("Everywhere");
        case keys::Context::channelRack: return tr ("Channel Rack");
        case keys::Context::pianoRoll: return tr ("Piano Roll");
        case keys::Context::playlist: return tr ("Playlist");
        case keys::Context::mixer: return tr ("Mixer");
        case keys::Context::browser: return tr ("Browser");
    }
    return {};
}

juce::String chordText (const keys::Chord& c) { return juce::String (keys::toString (c)); }

// key caps: "Ctrl+Shift+F4" drawn as one rounded pill per chord; returns the width used
int drawChord (juce::Graphics& g, const keys::Chord& chord, int x, int centreY, bool strong)
{
    const auto text = chordText (chord);
    const juce::Font font (theme::font (theme::type::small, true));
    const auto w = juce::GlyphArrangement::getStringWidthInt (font, text) + 14;
    const juce::Rectangle<float> r ((float) x, (float) centreY - 10.0f, (float) w, 20.0f);
    g.setColour (strong ? theme::colour::bg5 : theme::colour::bg4);
    g.fillRoundedRectangle (r, theme::radius::md);
    g.setColour (theme::colour::borderStrong);
    g.drawRoundedRectangle (r.reduced (0.5f), theme::radius::md, 1.0f);
    g.setFont (font);
    g.setColour (theme::colour::textPrimary);
    g.drawText (text, r, juce::Justification::centred);
    return w;
}

// Waits for a key chord (Esc cancels).
class KeyCatcher final : public juce::Component
{
public:
    std::function<void (keys::Chord)> onChord;
    std::function<void()> onCancel;

    KeyCatcher() { setWantsKeyboardFocus (true); }

    void paint (juce::Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (theme::colour::bg0);
        g.fillRoundedRectangle (r, theme::radius::md);
        g.setColour (theme::colour::accent);
        g.drawRoundedRectangle (r, theme::radius::md, 1.5f);
        g.setFont (theme::font (theme::type::body));
        g.setColour (theme::colour::textPrimary);
        g.drawText (tr ("Press the new keys now (Esc cancels)"), getLocalBounds().reduced (10, 0), juce::Justification::centredLeft);
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        const auto chord = chordFromKeyPress (key);
        if (chord.key == keys::escape && chord.mods == keys::none)
        {
            if (onCancel)
                onCancel();
            return true;
        }
        if (chord.isValid() && onChord)
            onChord (chord);
        return true;
    }

    void focusLost (FocusChangeType) override
    {
        if (onCancel)
            juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<KeyCatcher> (this)] {
                if (safe != nullptr && safe->isVisible() && ! safe->hasKeyboardFocus (true) && safe->onCancel)
                    safe->onCancel();
            });
    }
};

class ShortcutPanel final : public juce::Component,
                            private juce::ListBoxModel
{
public:
    ShortcutPanel (bool keep, std::function<void()> changed) : persistent (keep), onChanged (std::move (changed))
    {
        if (persistent && keyMapFile().existsAsFile())
            map.loadJson (keyMapFile().loadFileAsString().toStdString());
        else if (! persistent)
            map.setChords ("pattern.next", { { keys::f9, keys::ctrl | keys::shift } }); // a changed one for the snapshot

        search.setTextToShowWhenEmpty (tr ("Search an action or a key (e.g. \"Ctrl+L\")"), theme::colour::textDisabled);
        search.setFont (theme::font (theme::type::body));
        search.setIndents (8, 6);
        search.onTextChange = [this] { refilter(); };
        addAndMakeVisible (search);

        where.addItem (tr ("All places"), 1);
        for (int c = 0; c <= (int) keys::Context::browser; ++c)
            where.addItem (contextName ((keys::Context) c), c + 2);
        where.setSelectedId (1, juce::dontSendNotification);
        where.onChange = [this] { refilter(); };
        where.setColour (juce::ComboBox::outlineColourId, theme::colour::borderStrong);
        addAndMakeVisible (where);

        changedOnly.setButtonText (tr ("Changed only"));
        changedOnly.onClick = [this] { refilter(); };
        addAndMakeVisible (changedOnly);

        count.setFont (theme::font (theme::type::small));
        count.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        count.setJustificationType (juce::Justification::centredRight);
        addAndMakeVisible (count);

        list.setModel (this);
        list.setRowHeight (30);
        list.setColour (juce::ListBox::backgroundColourId, theme::colour::bg0);
        list.setColour (juce::ListBox::outlineColourId, theme::colour::border);
        list.setOutlineThickness (1);
        addAndMakeVisible (list);

        actionTitle.setFont (theme::font (theme::type::title, true));
        actionTitle.setColour (juce::Label::textColourId, theme::colour::textPrimary);
        addAndMakeVisible (actionTitle);
        actionWhere.setFont (theme::font (theme::type::small));
        actionWhere.setColour (juce::Label::textColourId, theme::colour::textSecondary);
        addAndMakeVisible (actionWhere);
        message.setFont (theme::font (theme::type::small));
        message.setColour (juce::Label::textColourId, theme::colour::warning);
        addAndMakeVisible (message);

        addKey.onClick = [this] { startCatching (false); };
        replaceKey.onClick = [this] { startCatching (true); };
        clearKeys.onClick = [this] { setChords ({}); };
        resetOne.onClick = [this] {
            if (const auto* a = selectedAction())
                setChords (a->defaults);
        };
        resetAll.onClick = [this] { confirmResetAll(); };
        openFile.onClick = [this] {
            save();
            if (persistent)
                keyMapFile().startAsProcess();
        };
        assign.onClick = [this] { assignPending (true); };
        cancel.onClick = [this] { stopCatching(); };
        for (auto* b : { &addKey, &replaceKey, &clearKeys, &resetOne, &resetAll, &openFile, &assign, &cancel })
            addAndMakeVisible (b);
        addKey.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        addKey.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);
        assign.setColour (juce::TextButton::buttonColourId, theme::colour::accent);
        assign.setColour (juce::TextButton::textColourOffId, theme::colour::onAccent);

        catcher.onChord = [this] (keys::Chord c) { caught (c); };
        catcher.onCancel = [this] { stopCatching(); };
        addChildComponent (catcher);

        refilter();
        if (! persistent)
        {
            // snapshot: one action selected, its clash shown
            for (int i = 0; i < (int) visible.size(); ++i)
                if (std::string (keys::actions()[(std::size_t) visible[(std::size_t) i]].id) == "pattern.next")
                    list.selectRow (i);
        }
        updateEditor();
        setSize (860, 620);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::colour::bg2);
        g.setColour (theme::colour::bg3);
        g.fillRect (editorArea);
        g.setColour (theme::colour::border);
        g.fillRect (editorArea.getX(), editorArea.getY(), editorArea.getWidth(), 1);

        if (const auto* a = selectedAction())
        {
            // the current keys
            auto x = chordsArea.getX();
            const auto& chords = map.chordsFor (a->id);
            if (chords.empty())
            {
                g.setFont (theme::font (theme::type::body));
                g.setColour (theme::colour::textDisabled);
                g.drawText (tr ("(no shortcut)"), chordsArea, juce::Justification::centredLeft);
            }
            for (const auto& c : chords)
                x += drawChord (g, c, x, chordsArea.getCentreY(), true) + 8;
        }
    }

    void resized() override
    {
        auto area = getLocalBounds();
        editorArea = area.removeFromBottom (150);
        area.reduce (14, 12);
        auto top = area.removeFromTop (30);
        search.setBounds (top.removeFromLeft (juce::jmin (340, top.getWidth() / 2)));
        top.removeFromLeft (8);
        where.setBounds (top.removeFromLeft (150));
        top.removeFromLeft (12);
        changedOnly.setBounds (top.removeFromLeft (130));
        count.setBounds (top);
        area.removeFromTop (10);
        list.setBounds (area);

        auto e = editorArea.reduced (14, 12);
        auto titleRow = e.removeFromTop (24);
        resetAll.setBounds (titleRow.removeFromRight (110));
        titleRow.removeFromRight (8);
        openFile.setBounds (titleRow.removeFromRight (120));
        actionTitle.setBounds (titleRow);
        actionWhere.setBounds (e.removeFromTop (18));
        e.removeFromTop (6);
        chordsArea = e.removeFromTop (30);
        catcher.setBounds (chordsArea.withWidth (juce::jmin (380, chordsArea.getWidth())));
        e.removeFromTop (6);
        auto buttons = e.removeFromTop (28);
        for (auto* b : { &addKey, &replaceKey, &clearKeys, &resetOne })
        {
            b->setBounds (buttons.removeFromLeft (b == &resetOne ? 140 : 120));
            buttons.removeFromLeft (8);
        }
        e.removeFromTop (6);
        auto msg = e.removeFromTop (24);
        cancel.setBounds (msg.removeFromRight (90));
        msg.removeFromRight (8);
        assign.setBounds (msg.removeFromRight (130));
        msg.removeFromRight (8);
        message.setBounds (msg);
    }

private:
    // --- list --------------------------------------------------------------------------------------------------
    void refilter()
    {
        const auto& all = keys::actions();
        const auto previous = selectedAction();
        const auto query = search::parse (search.getText().toStdString());
        const auto context = where.getSelectedId() - 2;
        visible.clear();

        for (int i = 0; i < (int) all.size(); ++i)
        {
            const auto& a = all[(std::size_t) i];
            if (context >= 0 && (int) a.context != context)
                continue;
            if (changedOnly.getToggleState() && map.isDefault (a.id))
                continue;
            juce::String text = tr (a.description) + " " + a.description + " " + a.id + " " + contextName (a.context);
            for (const auto& c : map.chordsFor (a.id))
                text << " " << chordText (c);
            if (search::matches (query, text.toStdString()))
                visible.push_back (i);
        }

        list.updateContent();
        list.deselectAllRows();
        if (previous != nullptr)
            for (int i = 0; i < (int) visible.size(); ++i)
                if (&all[(std::size_t) visible[(std::size_t) i]] == previous)
                    list.selectRow (i);
        list.repaint();
        count.setText (tr ("{0} actions", { juce::String ((int) visible.size()) }), juce::dontSendNotification);
        updateEditor();
    }

    int getNumRows() override { return (int) visible.size(); }

    void paintListBoxItem (int row, juce::Graphics& g, int width, int height, bool selected) override
    {
        if (! juce::isPositiveAndBelow (row, (int) visible.size()))
            return;
        const auto& a = keys::actions()[(std::size_t) visible[(std::size_t) row]];
        if (selected)
            g.fillAll (theme::colour::accentMuted);
        else if (row % 2 == 1)
            g.fillAll (theme::colour::bg1.withAlpha (0.5f));

        const auto changed = ! map.isDefault (a.id);
        if (changed)
        {
            g.setColour (theme::colour::accent);
            g.fillEllipse (6.0f, height * 0.5f - 3.0f, 6.0f, 6.0f);
        }
        const auto keysWidth = juce::jmin (260, width / 3);
        const auto whereWidth = 120;
        g.setFont (theme::font (theme::type::body));
        g.setColour (theme::colour::textPrimary);
        g.drawText (tr (a.description), 18, 0, width - keysWidth - whereWidth - 30, height, juce::Justification::centredLeft, true);
        g.setFont (theme::font (theme::type::small));
        g.setColour (theme::colour::textSecondary);
        g.drawText (contextName (a.context), width - keysWidth - whereWidth - 8, 0, whereWidth, height, juce::Justification::centredLeft, true);

        auto x = width - keysWidth;
        const auto& chords = map.chordsFor (a.id);
        if (chords.empty())
        {
            g.setColour (theme::colour::textDisabled);
            g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94"), x, 0, 40, height, juce::Justification::centredLeft);
        }
        for (const auto& c : chords)
        {
            if (x > width - 40)
                break;
            x += drawChord (g, c, x, height / 2, false) + 6;
        }
    }

    void selectedRowsChanged (int) override
    {
        stopCatching();
        updateEditor();
    }

    void listBoxItemDoubleClicked (int, const juce::MouseEvent&) override { startCatching (true); }

    const keys::Action* selectedAction() const
    {
        const auto row = list.getSelectedRow();
        return juce::isPositiveAndBelow (row, (int) visible.size()) ? &keys::actions()[(std::size_t) visible[(std::size_t) row]] : nullptr;
    }

    // --- editing -----------------------------------------------------------------------------------------------
    void updateEditor()
    {
        const auto* a = selectedAction();
        actionTitle.setText (a != nullptr ? tr (a->description) : tr ("Select an action to change its shortcut"), juce::dontSendNotification);
        actionWhere.setText (a != nullptr ? tr ("Works: {0}", { contextName (a->context) })
                                                + (a->context == keys::Context::global ? juce::String()
                                                                                       : "  ·  " + tr ("wins over a shortcut of the same keys that works everywhere"))
                                          : juce::String(),
                             juce::dontSendNotification);
        const auto has = a != nullptr && ! map.chordsFor (a->id).empty();
        addKey.setEnabled (a != nullptr);
        replaceKey.setEnabled (has);
        clearKeys.setEnabled (has);
        resetOne.setEnabled (a != nullptr && ! map.isDefault (a->id));
        if (! pending.isValid())
        {
            assign.setVisible (false);
            cancel.setVisible (catcher.isVisible());
            if (! catcher.isVisible())
                message.setText ({}, juce::dontSendNotification);
        }
        repaint();
    }

    void startCatching (bool replace)
    {
        if (selectedAction() == nullptr)
            return;
        replacing = replace;
        pending = {};
        catcher.setVisible (true);
        catcher.grabKeyboardFocus();
        message.setText ({}, juce::dontSendNotification);
        updateEditor();
    }

    void stopCatching()
    {
        catcher.setVisible (false);
        pending = {};
        assign.setVisible (false);
        cancel.setVisible (false);
        message.setText ({}, juce::dontSendNotification);
        repaint();
    }

    void caught (keys::Chord chord)
    {
        const auto* a = selectedAction();
        if (a == nullptr)
            return;
        catcher.setVisible (false);
        pending = chord;
        const auto others = map.actionsWith (chord, a->context, a->id);
        if (others.empty())
        {
            assignPending (false);
            explainShadowed (chord, a->id);
            return;
        }
        juce::StringArray names;
        for (const auto& id : others)
            if (const auto* o = keys::findAction (id))
                names.add ("\"" + tr (o->description) + "\"");
        message.setText (tr ("{0} is used by {1} in the same place. Assign moves it here.", { chordText (chord), names.joinIntoString (", ") }),
                         juce::dontSendNotification);
        assign.setVisible (true);
        cancel.setVisible (true);
    }

    // the same key on an action of another place (a panel's tool vs a global action): say which one runs there
    void explainShadowed (const keys::Chord& chord, const std::string& id)
    {
        juce::StringArray names;
        for (const auto& other : map.shadowedBy (chord, id))
            if (const auto* o = keys::findAction (other))
            {
                // the key the user just chose wins over a default key of the other action; two keys of his own: the panel's
                const auto otherIsDefault = std::find (o->defaults.begin(), o->defaults.end(), chord) != o->defaults.end();
                if (otherIsDefault)
                    names.add ("\"" + tr (o->description) + "\" (" + contextName (o->context) + ")");
            }
        if (! names.isEmpty())
            message.setText (tr ("{0} was also {1}: your key wins there now.", { chordText (chord), names.joinIntoString (", ") }),
                             juce::dontSendNotification);
    }

    void assignPending (bool takeFromOthers)
    {
        const auto* a = selectedAction();
        if (a == nullptr || ! pending.isValid())
            return;
        if (takeFromOthers)
            for (const auto& id : map.actionsWith (pending, a->context, a->id))
            {
                auto chords = map.chordsFor (id);
                chords.erase (std::remove (chords.begin(), chords.end(), pending), chords.end());
                map.setChords (id, chords);
            }
        auto chords = replacing ? std::vector<keys::Chord> {} : map.chordsFor (a->id);
        if (std::find (chords.begin(), chords.end(), pending) == chords.end())
            chords.push_back (pending);
        pending = {};
        setChords (chords);
    }

    void setChords (std::vector<keys::Chord> chords)
    {
        const auto* a = selectedAction();
        if (a == nullptr)
            return;
        map.setChords (a->id, std::move (chords));
        stopCatching();
        save();
        list.repaint();
        updateEditor();
    }

    void confirmResetAll()
    {
        juce::AlertWindow::showOkCancelBox (juce::MessageBoxIconType::QuestionIcon, tr ("Reset all shortcuts"),
                                            tr ("Every shortcut goes back to LYNX SHARE's default keys."), tr ("Reset all"), tr ("Cancel"), this,
                                            juce::ModalCallbackFunction::create ([safe = juce::Component::SafePointer<ShortcutPanel> (this)] (int result) {
                                                if (safe == nullptr || result != 1)
                                                    return;
                                                safe->map.resetToDefaults();
                                                safe->save();
                                                safe->refilter();
                                            }));
    }

    void save()
    {
        if (! persistent)
            return;
        keyMapFile().replaceWithText (juce::String (map.overridesJson()));
        if (onChanged)
            onChanged();
    }

    bool persistent;
    std::function<void()> onChanged;
    keys::KeyMap map;
    std::vector<int> visible;
    keys::Chord pending;
    bool replacing = false;

    juce::TextEditor search;
    juce::ComboBox where;
    juce::ToggleButton changedOnly;
    juce::Label count, actionTitle, actionWhere, message;
    juce::ListBox list;
    juce::Rectangle<int> editorArea, chordsArea;
    KeyCatcher catcher;
    juce::TextButton addKey { tr ("Add shortcut") }, replaceKey { tr ("Change") }, clearKeys { tr ("Remove") }, resetOne { tr ("Reset to default") },
        resetAll { tr ("Reset all...") }, openFile { tr ("Open keys.json") }, assign { tr ("Assign anyway") }, cancel { tr ("Cancel") };
};
} // namespace

void showShortcutManager (std::function<void()> onChanged)
{
    openToolWindow (std::make_unique<ShortcutPanel> (true, std::move (onChanged)), tr ("Manage shortcuts"), "shortcuts");
}

std::unique_ptr<juce::Component> makeShortcutManagerPanel (bool persistent, std::function<void()> onChanged)
{
    return std::make_unique<ShortcutPanel> (persistent, std::move (onChanged));
}
} // namespace lynx
