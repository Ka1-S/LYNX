#include "ui/PluginHeader.h"

#include <map>

#include "core/EffectPresets.h"
#include "core/Effects.h"
#include "core/FlEffects.h"
#include "core/ParamHelp.h"
#include "engine/AudioEngine.h"
#include "engine/Text.h"
#include "platform/Paths.h"
#include "ui/I18n.h"
#include "ui/PluginSkin.h"
#include "ui/Theme.h"
#include "ui/ValueControl.h"

namespace lynx::pluginheader
{
namespace
{
struct Snapshot
{
    EffectParams params {};
    std::vector<std::uint8_t> state;
    std::string presetName;

    bool operator== (const Snapshot& o) const { return params == o.params && state == o.state && presetName == o.presetName; }
};

struct SlotHistory
{
    EffectKind kind = EffectKind::eq;
    std::vector<Snapshot> undo, redo;
};

constexpr std::size_t maxSteps = 100;

std::map<std::pair<int, int>, SlotHistory>& histories()
{
    static std::map<std::pair<int, int>, SlotHistory> h;
    return h;
}

SlotHistory& historyOf (int track, int slot, EffectKind kind)
{
    auto& h = histories()[{ track, slot }];
    if (h.kind != kind) // another effect now sits in the slot
    {
        h = {};
        h.kind = kind;
    }
    return h;
}

Snapshot snapshotOf (const EffectSlot& fx) { return { fx.params, fx.state, fx.presetName }; }

void drawArrow (juce::Graphics& g, juce::Rectangle<float> r, bool left, juce::Colour c)
{
    juce::Path p;
    p.addTriangle (-3.0f, -5.0f, -3.0f, 5.0f, 4.0f, 0.0f);
    p.applyTransform (juce::AffineTransform::rotation (left ? juce::MathConstants<float>::pi : 0.0f).translated (r.getCentreX(), r.getCentreY()));
    g.setColour (c);
    g.fillPath (p);
}

// a curved arrow (undo: pointing left, redo: right)
void drawTurn (juce::Graphics& g, juce::Rectangle<float> r, bool redo, juce::Colour c)
{
    const auto cx = r.getCentreX(), cy = r.getCentreY() + 1.5f;
    juce::Path arc;
    arc.addCentredArc (cx, cy, 5.0f, 5.0f, 0.0f, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
    if (redo)
        arc.applyTransform (juce::AffineTransform::scale (-1.0f, 1.0f, cx, cy));
    g.setColour (c);
    g.strokePath (arc, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    // the arrow head at the arc's upper end
    const auto tip = juce::Point<float> (redo ? cx + 0.5f : cx - 0.5f, cy - 5.0f);
    juce::Path head;
    const auto d = redo ? 1.0f : -1.0f;
    head.addTriangle (tip.x + d * 3.5f, tip.y, tip.x - d * 1.0f, tip.y - 3.5f, tip.x - d * 1.0f, tip.y + 3.5f);
    g.fillPath (head);
}

bool isLynxEngine (EffectKind kind)
{
    return flPluginsForEffect (kind).empty();
}

// "Keep the Mix when loading presets" (the reference reverbs' mix lock): per engine, a marker file in its presets folder
juce::File mixLockFile (EffectKind kind) { return userFolder (kind).getChildFile (".mixlock"); }
bool mixLocked (EffectKind kind) { return mixLockFile (kind).existsAsFile(); }
void setMixLocked (EffectKind kind, bool on)
{
    if (on)
    {
        userFolder (kind).createDirectory();
        mixLockFile (kind).create();
    }
    else
        mixLockFile (kind).deleteFile();
}

// the preset's parameters with the slot's Mix kept when the mix lock is on
EffectParams withMixLock (EffectKind kind, EffectParams params, const EffectParams& current)
{
    if (mixLocked (kind))
        if (const auto p = presets::paramByName (kind, "Mix"); p >= 0)
            params[(std::size_t) p] = current[(std::size_t) p];
    return params;
}
} // namespace

juce::File userFolder (EffectKind kind)
{
    return platform::settingsFile ("Presets").getChildFile (juce::File::createLegalFileName (utf8 (effectInfo (kind).name)));
}

juce::Array<juce::File> userPresets (EffectKind kind)
{
    auto files = userFolder (kind).findChildFiles (juce::File::findFiles, false, "*.lynxpreset");
    std::sort (files.begin(), files.end(), [] (const juce::File& a, const juce::File& b) {
        return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()) < 0;
    });
    return files;
}

void clearHistory() { histories().clear(); }

juce::String describe (EffectKind kind, const char* param)
{
    // round 135: the texts moved to core/ParamHelp (tested; every LYNX engine has its own wording)
    const auto* text = paramhelp::describe (kind, param);
    return text != nullptr ? tr (text) : juce::String();
}

juce::String modeLine (EffectKind kind, const EffectParams& params, double bpm)
{
    const auto s = paramhelp::summary (kind, params);
    if (s.param < 0)
        return {};
    const auto& info = effectInfo (kind);
    auto line = tr (info.params[(std::size_t) s.param].name) + ": " + tr (juce::String::fromUTF8 (formatEffectParam (kind, s.param, params[(std::size_t) s.param], bpm, params).c_str()));
    if (s.text != nullptr)
        line << juce::String::fromUTF8 (" \xe2\x80\x94 ") << tr (s.text);
    if (s.detail != nullptr)
        line << ", " << tr (s.detail);
    return line;
}

bool isMixLocked (EffectKind kind) { return mixLocked (kind); }
void toggleMixLock (EffectKind kind) { setMixLocked (kind, ! mixLocked (kind)); }

PresetBar::PresetBar (AudioEngine& e, int t, int s) : engine (e), track (t), slot (s)
{
    const auto& mixer = engine.getSong().mixer;
    if (track >= 0 && track < (int) mixer.size() && slot >= 0 && slot < (int) mixer[(std::size_t) track].effects.size())
        kind = mixer[(std::size_t) track].effects[(std::size_t) slot].kind; // (current() checks the kind: read it directly)
}

const EffectSlot* PresetBar::current() const
{
    const auto& mixer = engine.getSong().mixer;
    if (track < 0 || track >= (int) mixer.size() || slot < 0 || slot >= (int) mixer[(std::size_t) track].effects.size())
        return nullptr;
    const auto& fx = mixer[(std::size_t) track].effects[(std::size_t) slot];
    return fx.kind == kind ? &fx : nullptr;
}

bool PresetBar::hasLynxPresets() const
{
    return ! presets::factory (kind).empty() || isLynxEngine (kind);
}

juce::Rectangle<float> PresetBar::place (juce::Rectangle<float> area, bool withPresets)
{
    presetsShown = withPresets;
    const auto h = juce::jmin (20.0f, area.getHeight() - 4.0f);
    const auto y = area.getCentreY() - h * 0.5f;
    auto x = area.getRight() - 4.0f;
    auto take = [&] (float w) {
        x -= w;
        return juce::Rectangle<float> (x, y, w, h);
    };
    if (withPresets)
    {
        rNext = take (18.0f);
        rPrev = take (18.0f);
        x -= 2.0f;
        rPreset = take (juce::jlimit (70.0f, 180.0f, area.getWidth() * 0.26f));
        x -= 8.0f;
    }
    else
        rNext = rPrev = rPreset = {};
    if (area.getWidth() >= 420.0f) // narrow windows (Quell) keep the title readable: A / B without the copy button
    {
        rCopy = take (34.0f);
        x -= 3.0f;
    }
    else
        rCopy = {};
    rB = take (18.0f);
    rA = take (18.0f);
    x -= 8.0f;
    rRedo = take (20.0f);
    rUndo = take (20.0f);
    x -= 6.0f;
    return area.withRight (juce::jmax (area.getX(), x));
}

void PresetBar::paint (juce::Graphics& g, juce::Colour accent) const
{
    using namespace theme;
    const auto* fx = current();
    if (fx == nullptr)
        return;
    const auto& h = histories();
    const auto it = h.find ({ track, slot });
    const auto canUndo = it != h.end() && it->second.kind == kind && ! it->second.undo.empty();
    const auto canRedo = it != h.end() && it->second.kind == kind && ! it->second.redo.empty();
    auto tone = [&] (Part p, bool enabled) {
        return ! enabled ? colour::textDisabled : hovered == p ? accent : colour::textSecondary;
    };

    drawTurn (g, rUndo, false, tone (Part::undo, canUndo));
    drawTurn (g, rRedo, true, tone (Part::redo, canRedo));

    // A / B: the live setting lit; the copy button names the direction
    const auto showingB = fx->abShowingB;
    for (const auto side : { 0, 1 })
    {
        const auto r = side == 0 ? rA : rB;
        const auto lit = (side == 1) == showingB;
        skin::button (g, r, lit, hovered == (side == 0 ? Part::a : Part::b), accent, radius::sm);
        g.setColour (lit ? colour::textPrimary : colour::textSecondary);
        g.setFont (font (type::small, true));
        g.drawText (side == 0 ? "A" : "B", r, juce::Justification::centred, false);
    }
    if (! rCopy.isEmpty())
    {
        skin::button (g, rCopy, false, hovered == Part::copy, accent, radius::sm);
        g.setColour (hovered == Part::copy ? colour::textPrimary : colour::textSecondary);
        g.setFont (font (type::caption, true));
        g.drawText (showingB ? juce::String::fromUTF8 ("B\xe2\x86\x92" "A") : juce::String::fromUTF8 ("A\xe2\x86\x92" "B"), rCopy,
                    juce::Justification::centred, false);
    }

    if (! presetsShown)
        return;
    skin::valuePlate (g, rPreset, radius::md);
    if (hovered == Part::preset)
    {
        g.setColour (accent.withAlpha (0.5f));
        g.drawRoundedRectangle (rPreset.reduced (0.5f), radius::md, 1.0f);
    }
    const auto label = presetLabel();
    const auto named = fx->presetName.size() > 0;
    g.setColour (named ? colour::textPrimary : colour::textSecondary);
    ui::drawFittingText (g, label, rPreset.reduced (6.0f, 0.0f), juce::Justification::centred, font (type::small, named), 7.5f, 0.0f, 0.7f); // (round 165)
    drawArrow (g, rPrev, true, hovered == Part::prev ? accent : colour::textSecondary);
    drawArrow (g, rNext, false, hovered == Part::next ? accent : colour::textSecondary);
}

juce::String PresetBar::presetLabel (const FlPresets* flPresets) const
{
    if (flPresets != nullptr && ! hasLynxPresets() && flPresets->name)
    {
        const auto n = flPresets->name();
        return n.isNotEmpty() ? n : tr ("(no preset)");
    }
    const auto* fx = current();
    if (fx != nullptr && ! fx->presetName.empty())
        return utf8 (fx->presetName.c_str());
    return tr ("(no preset)");
}

Part PresetBar::partAt (juce::Point<float> p) const
{
    if (rUndo.contains (p)) return Part::undo;
    if (rRedo.contains (p)) return Part::redo;
    if (rA.contains (p)) return Part::a;
    if (rB.contains (p)) return Part::b;
    if (rCopy.contains (p)) return Part::copy;
    if (presetsShown)
    {
        if (rPreset.contains (p)) return Part::preset;
        if (rPrev.contains (p)) return Part::prev;
        if (rNext.contains (p)) return Part::next;
    }
    return Part::none;
}

bool PresetBar::hover (juce::Point<float> p)
{
    const auto part = partAt (p);
    if (part == hovered)
        return false;
    hovered = part;
    return true;
}

juce::String PresetBar::hintFor (Part part) const
{
    switch (part)
    {
        case Part::undo: return tr ("Undo this plugin's last change");
        case Part::redo: return tr ("Redo this plugin's change");
        case Part::a: return tr ("Setting A: compare two settings (A / B)");
        case Part::b: return tr ("Setting B: compare two settings (A / B)");
        case Part::copy: return tr ("Copy the setting you hear to the other one (A / B)");
        case Part::preset: return tr ("Presets: factory presets, your presets, save");
        case Part::prev: return tr ("Previous preset");
        case Part::next: return tr ("Next preset");
        case Part::none: break;
    }
    return {};
}

void PresetBar::willEdit()
{
    const auto* fx = current();
    if (fx == nullptr)
        return;
    const auto now = juce::Time::getMillisecondCounter();
    if (gesture || now - lastEdit > 600)
    {
        auto& h = historyOf (track, slot, kind);
        const auto snap = snapshotOf (*fx);
        if (h.undo.empty() || ! (h.undo.back() == snap))
        {
            h.undo.push_back (snap);
            if (h.undo.size() > maxSteps)
                h.undo.erase (h.undo.begin());
        }
        h.redo.clear();
    }
    gesture = false;
    lastEdit = now;
}

void PresetBar::restore (const EffectParams& params, const std::vector<std::uint8_t>& state, const std::string& presetName, bool undoable)
{
    engine.editSong ([t = track, s = slot, k = kind, params, state, presetName] (Song& song) {
        if (t >= (int) song.mixer.size() || s >= (int) song.mixer[(std::size_t) t].effects.size())
            return;
        auto& fx = song.mixer[(std::size_t) t].effects[(std::size_t) s];
        if (fx.kind != k)
            return;
        fx.params = params;
        fx.state = state;
        fx.presetName = presetName;
    }, undoable);
}

void PresetBar::click (Part part, juce::Component& owner, const FlPresets* flPresets)
{
    const auto* fx = current();
    if (fx == nullptr)
        return;
    auto& h = historyOf (track, slot, kind);
    switch (part)
    {
        case Part::undo:
        case Part::redo:
        {
            auto& from = part == Part::undo ? h.undo : h.redo;
            auto& to = part == Part::undo ? h.redo : h.undo;
            if (from.empty())
                return;
            to.push_back (snapshotOf (*fx));
            const auto snap = from.back();
            from.pop_back();
            restore (snap.params, snap.state, snap.presetName);
            gesture = true;
            break;
        }
        case Part::a:
        case Part::b:
        {
            const auto wantB = part == Part::b;
            if (wantB == fx->abShowingB)
                return;
            engine.editSong ([t = track, s = slot] (Song& song) {
                auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) s];
                std::vector<float> live (target.params.begin(), target.params.end());
                if (target.abOther.empty()) // B used for the first time: it starts as a copy of A
                    target.abOther = live;
                for (std::size_t i = 0; i < target.params.size() && i < target.abOther.size(); ++i)
                    target.params[i] = target.abOther[i];
                target.abOther = live;
                target.abShowingB = ! target.abShowingB;
            });
            gesture = true;
            break;
        }
        case Part::copy:
            engine.editSong ([t = track, s = slot] (Song& song) {
                auto& target = song.mixer[(std::size_t) t].effects[(std::size_t) s];
                target.abOther.assign (target.params.begin(), target.params.end());
            });
            break;
        case Part::preset: showPresetMenu (owner, flPresets); break;
        case Part::prev: stepPreset (-1, flPresets); break;
        case Part::next: stepPreset (1, flPresets); break;
        case Part::none: break;
    }
    owner.repaint();
}

void PresetBar::loadFactory (int index)
{
    const auto& list = presets::factory (kind);
    const auto* fx = current();
    if (fx == nullptr || index < 0 || index >= (int) list.size())
        return;
    gesture = true;
    willEdit();
    restore (withMixLock (kind, presets::apply (kind, list[(std::size_t) index], engine.getSong().bpm, nullptr, &fx->params), fx->params), fx->state,
             list[(std::size_t) index].name);
    gesture = true;
}

void PresetBar::loadUser (const juce::File& file)
{
    const auto* fx = current();
    if (fx == nullptr)
        return;
    const auto loaded = presets::fromText (kind, file.loadFileAsString().toStdString());
    if (! loaded)
        return;
    gesture = true;
    willEdit();
    restore (withMixLock (kind, loaded->params, fx->params), fx->state, loaded->name.empty() ? file.getFileNameWithoutExtension().toStdString() : loaded->name);
    gesture = true;
}

void PresetBar::stepPreset (int delta, const FlPresets* flPresets)
{
    if (! hasLynxPresets())
    {
        if (flPresets != nullptr && flPresets->step)
            flPresets->step (delta);
        return;
    }
    const auto* fx = current();
    if (fx == nullptr)
        return;
    const auto& factory = presets::factory (kind);
    const auto users = userPresets (kind);
    const auto count = (int) factory.size() + users.size();
    if (count == 0)
        return;
    int at = -1;
    for (int i = 0; i < (int) factory.size() && at < 0; ++i)
        if (factory[(std::size_t) i].name == fx->presetName)
            at = i;
    for (int i = 0; i < users.size() && at < 0; ++i)
        if (users[i].getFileNameWithoutExtension().toStdString() == presets::fileNameFor (fx->presetName))
            at = (int) factory.size() + i;
    at = at < 0 ? (delta > 0 ? 0 : count - 1) : (at + delta + count) % count;
    if (at < (int) factory.size())
        loadFactory (at);
    else
        loadUser (users[at - (int) factory.size()]);
}

void PresetBar::showPresetMenu (juce::Component& owner, const FlPresets* flPresets)
{
    const auto* fx = current();
    if (fx == nullptr)
        return;
    juce::PopupMenu menu;
    const auto& factory = presets::factory (kind);
    if (! factory.empty())
    {
        std::map<std::string, juce::PopupMenu> folders;
        std::vector<std::string> order;
        for (int i = 0; i < (int) factory.size(); ++i)
        {
            const auto& p = factory[(std::size_t) i];
            if (folders.find (p.folder) == folders.end())
                order.push_back (p.folder);
            folders[p.folder].addItem (utf8 (p.name.c_str()), true, p.name == fx->presetName, [this, i] { loadFactory (i); });
        }
        menu.addSectionHeader (tr ("LYNX presets"));
        for (const auto& f : order)
            if (f.empty())
                for (juce::PopupMenu::MenuItemIterator it (folders[f]); it.next();)
                    menu.addItem (it.getItem());
            else
                menu.addSubMenu (tr (f.c_str()), folders[f]);
    }
    const auto users = userPresets (kind);
    if (! users.isEmpty())
    {
        menu.addSectionHeader (tr ("Your presets"));
        for (const auto& f : users)
            menu.addItem (f.getFileNameWithoutExtension(), true, f.getFileNameWithoutExtension().toStdString() == presets::fileNameFor (fx->presetName),
                          [this, f] { loadUser (f); });
    }
    if (flPresets != nullptr && flPresets->addTo)
    {
        juce::PopupMenu fl;
        flPresets->addTo (fl);
        if (fl.getNumItems() > 0)
        {
            if (menu.getNumItems() == 0)
                menu = fl;
            else
                menu.addSubMenu (tr ("FL Studio presets"), fl);
        }
    }
    menu.addSeparator();
    juce::Component::SafePointer<juce::Component> safe (&owner);
    menu.addItem (tr ("Copy the setting you hear to the other one (A / B)"), [this, safe] {
        if (safe != nullptr)
            click (Part::copy, *safe);
    });
    menu.addItem (tr ("Save preset..."), [this, safe] {
        if (safe != nullptr)
            saveAs (*safe);
    });
    for (const auto& f : users)
        if (f.getFileNameWithoutExtension().toStdString() == presets::fileNameFor (fx->presetName) && ! fx->presetName.empty())
        {
            menu.addItem (tr ("Delete preset \"{0}\"", { f.getFileNameWithoutExtension() }), [this, f, safe] {
                f.deleteFile();
                engine.editSong ([t = track, s = slot] (Song& song) { song.mixer[(std::size_t) t].effects[(std::size_t) s].presetName.clear(); }, false);
                if (safe != nullptr)
                    safe->repaint();
            });
            break;
        }
    if (presets::paramByName (kind, "Mix") >= 0)
        menu.addItem (tr ("Keep the Mix when loading presets"), true, mixLocked (kind), [this] { setMixLocked (kind, ! mixLocked (kind)); });
    menu.addItem (tr ("Show the presets folder"), [this] {
        const auto folder = userFolder (kind);
        folder.createDirectory();
        folder.revealToUser();
    });
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&owner).withTargetScreenArea (owner.localAreaToGlobal (rPreset.toNearestInt())));
}

void PresetBar::saveAs (juce::Component& owner)
{
    const auto* fx = current();
    if (fx == nullptr)
        return;
    auto* window = new juce::AlertWindow (tr ("Save preset"), tr ("Name of the preset:"), juce::MessageBoxIconType::NoIcon, &owner);
    window->addTextEditor ("name", fx->presetName.empty() ? tr ("My preset") : utf8 (fx->presetName.c_str()));
    window->addButton (tr ("Save"), 1, juce::KeyPress (juce::KeyPress::returnKey));
    window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
    juce::Component::SafePointer<juce::Component> safe (&owner);
    window->enterModalState (true, juce::ModalCallbackFunction::create ([this, window, safe] (int result) {
        const auto name = window->getTextEditorContents ("name").trim();
        if (result != 1 || name.isEmpty() || safe == nullptr)
            return;
        const auto* slotNow = current();
        if (slotNow == nullptr)
            return;
        const auto folder = userFolder (kind);
        folder.createDirectory();
        const auto file = folder.getChildFile (utf8 (presets::fileNameFor (name.toStdString()).c_str()) + ".lynxpreset");
        file.replaceWithText (utf8 (presets::toText (kind, name.toStdString(), slotNow->params).c_str()));
        engine.editSong ([t = track, s = slot, n = name.toStdString()] (Song& song) { song.mixer[(std::size_t) t].effects[(std::size_t) s].presetName = n; }, false);
        safe->repaint();
    }), true);
}
} // namespace lynx::pluginheader
