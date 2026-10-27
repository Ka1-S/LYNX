#include "ui/PatcherEditor.h"
#include "ui/VisualEditors.h"
#include "core/ControlSurface.h"
#include "core/VoltageController.h"
#include "ui/VoltageEditor.h"
#include "core/XyController.h"
#include "ui/Controllers164.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include "core/Effects.h"
#include "core/FlSynths.h"
#include "core/MidiControls.h"
#include "core/NativeEngines.h"
#include "core/SoundFont.h"
#include "core/Patcher.h"
#include "core/Vfx.h"
#include "engine/PluginHost.h"
#include "ui/I18n.h"
#include "ui/MixerComponent.h"
#include "ui/ChannelRackComponent.h"
#include "ui/ControllerEditors.h"
#include "ui/PluginPicker.h"
#include "ui/PluginWindow.h"
#include "ui/Theme.h"
#include "ui/ToolWindow.h"
#include "ui/VfxEditors.h"

namespace lynx
{
namespace
{
namespace col = theme::colour;

// Add plugin › Browse plugin database: the Patcher editor waiting for the Browser's next pick (round 162)
std::function<bool (const ChannelDef*, const EffectSlot*)> pendingBrowserPick;

constexpr float nodeWidth = 150.0f;
constexpr float headerHeight = 24.0f;
constexpr float pinSpacing = 17.0f;
constexpr float pinRadius = 5.0f;

// Where the graph lives in the song: a mixer slot or a channel
struct Target
{
    bool channel = false;
    int index = 0; // track or channel
    int slot = 0;

    const PatcherDoc* doc (const Song& song) const
    {
        if (channel)
            return index >= 0 && index < (int) song.channels.size() && song.channels[(std::size_t) index].instrument == InstrumentKind::patcher
                       ? &song.channels[(std::size_t) index].patcher
                       : nullptr;
        if (index < 0 || index >= (int) song.mixer.size() || slot < 0 || slot >= (int) song.mixer[(std::size_t) index].effects.size())
            return nullptr;
        const auto& fx = song.mixer[(std::size_t) index].effects[(std::size_t) slot];
        return fx.kind == EffectKind::patcher ? &fx.patcher : nullptr;
    }
    PatcherDoc* doc (Song& song) const { return const_cast<PatcherDoc*> (doc (static_cast<const Song&> (song))); }

    juce::String key() const { return channel ? "patcher-ch-" + juce::String (index) : "patcher-fx-" + juce::String (index) + "-" + juce::String (slot); }
};

struct Pin
{
    int node = patchFromHost; // node index, patchFromHost / patchToHost
    bool output = false;      // carries a signal out of its node
    PatchWire::Kind kind = PatchWire::Kind::audio;
    int port = 0;
    juce::Point<float> at; // world position
    juce::String label;
    bool adds = false;     // "To LYNX": the pin that makes a new extra output
};

juce::Colour wireColour (PatchWire::Kind kind)
{
    switch (kind)
    {
        case PatchWire::Kind::audio: return col::knobArc;
        case PatchWire::Kind::midi: return col::info;
        case PatchWire::Kind::control: return col::automation;
    }
    return col::knobArc;
}

class PatcherEditor final : public juce::Component, private juce::Timer
{
public:
    PatcherEditor (AudioEngine& e, Target t) : engine (e), target (t)
    {
        // FL's Map / Surface tabs (round 151); on the Surface: Edit (move / rename / delete the controls) and "+"
        for (auto* b : { &mapTab, &surfaceTab, &editButton, &addButton })
        {
            b->setWantsKeyboardFocus (false);
            addAndMakeVisible (b);
        }
        mapTab.setClickingTogglesState (false);
        surfaceTab.setClickingTogglesState (false);
        mapTab.onClick = [this] { showSurface (false); };
        surfaceTab.onClick = [this] { showSurface (true); };
        editButton.setClickingTogglesState (true);
        editButton.setTooltip (tr ("Edit the Surface: drag to move a control, right-click to rename or delete it"));
        editButton.onClick = [this] { repaint(); };
        addButton.setTooltip (tr ("Add a control to the Surface (it becomes an output of the Map's Surface node)"));
        addButton.onClick = [this] { showAddControlMenu ({ 60.0f, 60.0f }); };
        showSurface (false);
        setSize (900, 560);
        setWantsKeyboardFocus (true);
        startTimerHz (10);
        fit();
    }

    void resized() override
    {
        mapTab.setBounds (6, 4, 64, 22);
        surfaceTab.setBounds (72, 4, 74, 22);
        editButton.setBounds (156, 4, 56, 22);
        addButton.setBounds (216, 4, 28, 22);
    }

    void paint (juce::Graphics& g) override
    {
        if (surfaceView)
        {
            paintSurface (g);
            return;
        }
        g.fillAll (col::bg0);
        const auto* doc = currentDoc();
        if (doc == nullptr)
        {
            g.setColour (col::textSecondary);
            g.drawText (tr ("This Patcher is gone."), getLocalBounds(), juce::Justification::centred);
            return;
        }

        // grid
        g.setColour (col::gridStep);
        const auto step = 40.0f * zoom;
        for (auto x = std::fmod (-offset.x * zoom, step); x < (float) getWidth(); x += step)
            g.drawVerticalLine ((int) x, 0.0f, (float) getHeight());
        for (auto y = std::fmod (-offset.y * zoom, step); y < (float) getHeight(); y += step)
            g.drawHorizontalLine ((int) y, 0.0f, (float) getWidth());

        const auto pins = allPins (*doc);

        // cables
        for (std::size_t w = 0; w < doc->wires.size(); ++w)
            drawWire (g, *doc, pins, doc->wires[w], (int) w == selectedWire);

        // nodes
        drawHost (g, *doc, true);
        drawHost (g, *doc, false);
        drawBox (g, nodeBounds (*doc, patchSurface), tr ("Surface"), selectedNode == patchSurface, true, doc->surface.empty());
        for (int n = 0; n < (int) doc->nodes.size(); ++n)
            drawNode (g, *doc, n);
        for (const auto& p : pins)
            drawPin (g, p);

        // the cable being drawn
        if (dragging == Drag::wire)
        {
            g.setColour (wireColour (dragFrom.kind).withAlpha (0.8f));
            drawCurve (g, toScreen (dragFrom.at), dragPoint, 2.0f);
        }

        paintMinimap (g, *doc);

        g.setColour (col::textSecondary);
        g.setFont (theme::type::small);
        g.drawText (tr ("Right-click: add a plugin. Drag from an output to an input to connect. Double-click a node: its editor."),
                    getLocalBounds().removeFromBottom (20).reduced (8, 0), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (surfaceView)
        {
            surfaceDown (e);
            return;
        }
        grabKeyboardFocus();
        const auto* doc = currentDoc();
        if (doc == nullptr)
            return;
        if (! e.mods.isPopupMenu() && minimapArea().contains (e.position) && ! minimapWorld (*doc).isEmpty())
        {
            dragging = Drag::minimap;
            centreViewOn (minimapToWorld (*doc, e.position));
            return;
        }
        const auto world = toWorld (e.position);
        if (e.mods.isPopupMenu())
        {
            if (const auto n = nodeAt (*doc, world); n != noNode)
                showNodeMenu (n);
            else if (const auto w = wireAt (*doc, e.position); w >= 0)
                showWireMenu (w);
            else
                showAddMenu (world);
            return;
        }
        if (const auto pin = pinAt (*doc, world); pin && pin->output)
        {
            dragging = Drag::wire;
            dragFrom = *pin;
            dragPoint = e.position;
            return;
        }
        if (const auto n = nodeAt (*doc, world); n != noNode)
        {
            selectedNode = n;
            selectedWire = -1;
            dragging = Drag::node;
            dragStart = world;
            dragOrigin = nodePosition (*doc, n);
            repaint();
            return;
        }
        selectedWire = wireAt (*doc, e.position);
        selectedNode = noNode;
        if (selectedWire < 0)
        {
            dragging = Drag::view;
            dragStart = e.position;
            dragOrigin = offset;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (surfaceView)
        {
            surfaceDrag (e);
            return;
        }
        if (dragging == Drag::minimap)
        {
            if (const auto* doc = currentDoc())
                centreViewOn (minimapToWorld (*doc, e.position));
            return;
        }
        if (dragging == Drag::wire)
            dragPoint = e.position;
        else if (dragging == Drag::node)
            moved = dragOrigin + (toWorld (e.position) - dragStart);
        else if (dragging == Drag::view)
            offset = dragOrigin - (e.position - dragStart) / zoom;
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (surfaceView)
        {
            surfaceUp();
            return;
        }
        const auto mode = std::exchange (dragging, Drag::none);
        const auto* doc = currentDoc();
        if (doc == nullptr)
            return;
        if (mode == Drag::node && moved)
        {
            const auto n = selectedNode;
            const auto p = *moved;
            moved.reset();
            edit ([n, p] (PatcherDoc& d) { setNodePosition (d, n, p); });
        }
        else if (mode == Drag::wire)
            finishWire (*doc, toWorld (e.position));
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (surfaceView)
        {
            if (const auto c = controlAt (e.position); c >= 0)
                renameControl (c);
            return;
        }
        if (const auto* doc = currentDoc())
            if (const auto n = nodeAt (*doc, toWorld (e.position)); n >= 0)
                openNodeEditor (n);
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        if (surfaceView)
            return;
        const auto before = toWorld (e.position);
        zoom = std::clamp (zoom * (wheel.deltaY > 0 ? 1.1f : 1.0f / 1.1f), 0.25f, 3.0f);
        offset = before - e.position / zoom;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::deleteKey || key == juce::KeyPress::backspaceKey)
        {
            if (selectedNode >= 0)
            {
                const auto n = selectedNode;
                selectedNode = noNode;
                edit ([n] (PatcherDoc& d) { patcher::removeNode (d, n); });
                return true;
            }
            if (selectedWire >= 0)
            {
                const auto w = selectedWire;
                selectedWire = -1;
                edit ([w] (PatcherDoc& d) {
                    if (w < (int) d.wires.size())
                        d.wires.erase (d.wires.begin() + w);
                });
                return true;
            }
        }
        return false;
    }

private:
    // --- the Surface tab (FL's Patcher › Surface, round 151) ---------------------------------------------------
    juce::TextButton mapTab { tr ("Map") }, surfaceTab { tr ("Surface") }, editButton { tr ("Edit") }, addButton { "+" };
    bool surfaceView = false;
    int surfaceDrag_ = -1;          // the control being turned / moved
    bool surfaceMoving = false;
    juce::Point<float> surfaceGrab; // where it was taken (move: offset from its corner)
    float grabValue = 0.0f, grabValueY = 0.0f;

    void showSurface (bool on)
    {
        surfaceView = on;
        mapTab.setToggleState (! on, juce::dontSendNotification);
        surfaceTab.setToggleState (on, juce::dontSendNotification);
        editButton.setVisible (on);
        addButton.setVisible (on);
        repaint();
    }

    static juce::Rectangle<float> controlBounds (const SurfaceControl& c)
    {
        switch (c.kind)
        {
            case SurfaceControl::Kind::knob: return { c.x, c.y, 56.0f, 70.0f };
            case SurfaceControl::Kind::slider: return { c.x, c.y, 40.0f, 140.0f };
            case SurfaceControl::Kind::button: return { c.x, c.y, 80.0f, 30.0f };
            case SurfaceControl::Kind::checkBox: return { c.x, c.y, 110.0f, 24.0f };
            case SurfaceControl::Kind::xy: return { c.x, c.y, 140.0f, 158.0f };
        }
        return { c.x, c.y, 56.0f, 70.0f };
    }

    int controlAt (juce::Point<float> p) const
    {
        if (const auto* d = currentDoc())
            for (int i = (int) d->surface.size() - 1; i >= 0; --i)
                if (controlBounds (d->surface[(std::size_t) i]).contains (p))
                    return i;
        return -1;
    }

    void paintSurface (juce::Graphics& g)
    {
        g.fillAll (col::bg1);
        const auto* d = currentDoc();
        g.setColour (col::gridStep);
        for (int x = 20; x < getWidth(); x += 20)
            for (int y = 40; y < getHeight(); y += 20)
                g.fillRect ((float) x, (float) y, 1.0f, 1.0f);
        if (d == nullptr)
            return;
        if (d->surface.empty())
        {
            g.setColour (col::textSecondary);
            g.setFont (theme::type::body);
            g.drawText (tr ("The Surface is empty. \"+\" adds a knob, a slider, a button, a check box or an XY pad; each one becomes an output "
                            "of the Map's Surface node, to cable to any parameter."),
                        getLocalBounds().reduced (40), juce::Justification::centred, true);
        }
        const auto editing = editButton.getToggleState();
        for (int i = 0; i < (int) d->surface.size(); ++i)
        {
            const auto& c = d->surface[(std::size_t) i];
            const auto r = controlBounds (c);
            const auto name = juce::String::fromUTF8 (c.name.c_str());
            const auto active = surfaceDrag_ == i;
            g.setFont (theme::type::caption);
            switch (c.kind)
            {
                case SurfaceControl::Kind::knob:
                    theme::drawKnob (g, r.withHeight (56.0f).reduced (4.0f), c.value, false, active ? col::accent : col::knobArc, active);
                    g.setColour (col::textSecondary);
                    g.drawText (name, r.withTrimmedTop (56.0f), juce::Justification::centred, true);
                    break;
                case SurfaceControl::Kind::slider:
                {
                    const auto track = r.withTrimmedBottom (16.0f).withSizeKeepingCentre (6.0f, r.getHeight() - 26.0f);
                    g.setColour (col::bg0);
                    g.fillRoundedRectangle (track, 3.0f);
                    const auto y = track.getBottom() - c.value * track.getHeight();
                    g.setColour (active ? col::accent : col::knobArc);
                    g.fillRoundedRectangle (track.withTop (y), 3.0f);
                    g.setColour (col::textPrimary);
                    g.fillRoundedRectangle (juce::Rectangle<float> (24.0f, 10.0f).withCentre ({ track.getCentreX(), y }), 3.0f);
                    g.setColour (col::textSecondary);
                    g.drawText (name, r.withTop (r.getBottom() - 16.0f), juce::Justification::centred, true);
                    break;
                }
                case SurfaceControl::Kind::button:
                    g.setColour (c.value > 0.5f ? col::accent : col::bg4);
                    g.fillRoundedRectangle (r, 4.0f);
                    g.setColour (c.value > 0.5f ? col::onAccent : col::textPrimary);
                    g.drawText (name, r, juce::Justification::centred, true);
                    break;
                case SurfaceControl::Kind::checkBox:
                {
                    const auto box = r.withWidth (20.0f).reduced (2.0f);
                    g.setColour (col::bg0);
                    g.fillRoundedRectangle (box, 3.0f);
                    if (c.value > 0.5f)
                    {
                        g.setColour (col::accent);
                        g.fillRoundedRectangle (box.reduced (3.0f), 2.0f);
                    }
                    g.setColour (col::textPrimary);
                    g.drawText (name, r.withTrimmedLeft (26.0f), juce::Justification::centredLeft, true);
                    break;
                }
                case SurfaceControl::Kind::xy:
                {
                    const auto pad = r.withHeight (140.0f);
                    g.setColour (col::bg0);
                    g.fillRoundedRectangle (pad, 4.0f);
                    g.setColour (col::border);
                    g.drawHorizontalLine ((int) pad.getCentreY(), pad.getX(), pad.getRight());
                    g.drawVerticalLine ((int) pad.getCentreX(), pad.getY(), pad.getBottom());
                    const auto dot = juce::Point<float> (pad.getX() + c.value * pad.getWidth(), pad.getBottom() - c.valueY * pad.getHeight());
                    g.setColour (active ? col::accent : col::knobArc);
                    g.fillEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (dot));
                    g.setColour (col::textSecondary);
                    g.drawText (name, r.withTop (pad.getBottom()), juce::Justification::centred, true);
                    break;
                }
            }
            if (editing)
            {
                g.setColour (col::accent.withAlpha (0.6f));
                g.drawRect (r.expanded (2.0f), 1.0f);
            }
        }
    }

    void setControlValue (int i, float v, float vy)
    {
        edit ([i, v, vy] (PatcherDoc& d) {
            if (i >= 0 && i < (int) d.surface.size())
            {
                d.surface[(std::size_t) i].value = std::clamp (v, 0.0f, 1.0f);
                d.surface[(std::size_t) i].valueY = std::clamp (vy, 0.0f, 1.0f);
            }
        });
    }

    void surfaceDown (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();
        const auto* d = currentDoc();
        if (d == nullptr)
            return;
        const auto i = controlAt (e.position);
        if (e.mods.isPopupMenu())
        {
            if (i >= 0)
                showControlMenu (i);
            else
                showAddControlMenu (e.position);
            return;
        }
        if (i < 0)
            return;
        surfaceDrag_ = i;
        const auto& c = d->surface[(std::size_t) i];
        grabValue = c.value;
        grabValueY = c.valueY;
        surfaceMoving = editButton.getToggleState();
        surfaceGrab = surfaceMoving ? e.position - juce::Point<float> (c.x, c.y) : e.position;
        if (surfaceMoving)
            return;
        switch (c.kind)
        {
            case SurfaceControl::Kind::button: setControlValue (i, 1.0f, c.valueY); break; // held while pressed
            case SurfaceControl::Kind::checkBox: setControlValue (i, c.value > 0.5f ? 0.0f : 1.0f, c.valueY); break;
            case SurfaceControl::Kind::xy: surfaceDrag (e); break;
            default: break;
        }
        repaint();
    }

    void surfaceDrag (const juce::MouseEvent& e)
    {
        const auto* d = currentDoc();
        if (d == nullptr || surfaceDrag_ < 0 || surfaceDrag_ >= (int) d->surface.size())
            return;
        const auto i = surfaceDrag_;
        const auto& c = d->surface[(std::size_t) i];
        if (surfaceMoving)
        {
            const auto at = e.position - surfaceGrab;
            const auto x = std::max (0.0f, std::round (at.x / 10.0f) * 10.0f), y = std::max (30.0f, std::round (at.y / 10.0f) * 10.0f);
            edit ([i, x, y] (PatcherDoc& doc) {
                if (i < (int) doc.surface.size())
                    doc.surface[(std::size_t) i].x = x, doc.surface[(std::size_t) i].y = y;
            });
            return;
        }
        const auto r = controlBounds (c);
        switch (c.kind)
        {
            case SurfaceControl::Kind::knob:
            case SurfaceControl::Kind::slider:
            {
                // up = more; 200 px for the whole range (fine with Shift)
                const auto range = e.mods.isShiftDown() ? 1000.0f : 200.0f;
                setControlValue (i, grabValue + (surfaceGrab.y - e.position.y) / range, c.valueY);
                break;
            }
            case SurfaceControl::Kind::xy:
            {
                const auto pad = r.withHeight (140.0f);
                setControlValue (i, (e.position.x - pad.getX()) / pad.getWidth(), (pad.getBottom() - e.position.y) / pad.getHeight());
                break;
            }
            default: break;
        }
        repaint();
    }

    void surfaceUp()
    {
        const auto* d = currentDoc();
        if (d != nullptr && surfaceDrag_ >= 0 && surfaceDrag_ < (int) d->surface.size() && ! surfaceMoving
            && d->surface[(std::size_t) surfaceDrag_].kind == SurfaceControl::Kind::button)
            setControlValue (surfaceDrag_, 0.0f, d->surface[(std::size_t) surfaceDrag_].valueY); // released
        surfaceDrag_ = -1;
        surfaceMoving = false;
        repaint();
    }

    void showAddControlMenu (juce::Point<float> at)
    {
        juce::PopupMenu menu;
        menu.addSectionHeader (tr ("Add control"));
        const std::pair<SurfaceControl::Kind, const char*> kinds[] = { { SurfaceControl::Kind::knob, TR_NOOP ("Knob") },
                                                                       { SurfaceControl::Kind::slider, TR_NOOP ("Slider") },
                                                                       { SurfaceControl::Kind::button, TR_NOOP ("Button") },
                                                                       { SurfaceControl::Kind::checkBox, TR_NOOP ("Check box") },
                                                                       { SurfaceControl::Kind::xy, TR_NOOP ("XY pad") } };
        for (const auto& [kind, name] : kinds)
            menu.addItem (tr (name), [this, kind = kind, english = juce::String (name), at] {
                edit ([kind, english, at] (PatcherDoc& d) {
                    int same = 1;
                    for (const auto& c : d.surface)
                        same += c.kind == kind ? 1 : 0;
                    SurfaceControl c;
                    c.kind = kind;
                    c.name = (english + " " + juce::String (same)).toStdString(); // FL: "Knob 1"
                    c.value = kind == SurfaceControl::Kind::knob || kind == SurfaceControl::Kind::slider ? 0.5f : 0.0f;
                    c.valueY = 0.5f;
                    c.x = std::round (at.x / 10.0f) * 10.0f;
                    c.y = std::max (40.0f, std::round (at.y / 10.0f) * 10.0f);
                    patcher::addSurfaceControl (d, c);
                });
            });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void showControlMenu (int i)
    {
        juce::PopupMenu menu;
        menu.addItem (tr ("Rename..."), [this, i] { renameControl (i); });
        menu.addItem (tr ("Delete"), [this, i] { edit ([i] (PatcherDoc& d) { patcher::removeSurfaceControl (d, i); }); });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void renameControl (int i)
    {
        const auto* d = currentDoc();
        if (d == nullptr || i < 0 || i >= (int) d->surface.size())
            return;
        auto* window = new juce::AlertWindow (tr ("Rename"), {}, juce::MessageBoxIconType::NoIcon);
        window->addTextEditor ("name", juce::String::fromUTF8 (d->surface[(std::size_t) i].name.c_str()));
        window->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        window->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        juce::Component::SafePointer<PatcherEditor> safe (this);
        window->enterModalState (true, juce::ModalCallbackFunction::create ([safe, window, i] (int result) {
            if (result != 1 || safe == nullptr)
                return;
            const auto name = window->getTextEditorContents ("name").trim().toStdString();
            if (! name.empty())
                safe->edit ([i, name] (PatcherDoc& doc) {
                    if (i < (int) doc.surface.size())
                        doc.surface[(std::size_t) i].name = name;
                });
        }), true);
    }

    enum class Drag { none, wire, node, view, minimap };
    static constexpr int noNode = -100;

    AudioEngine& engine;
    Target target;
    float zoom = 1.0f;
    juce::Point<float> offset;
    int selectedNode = noNode, selectedWire = -1;
    Drag dragging = Drag::none;
    Pin dragFrom;
    juce::Point<float> dragPoint, dragStart, dragOrigin;
    std::optional<juce::Point<float>> moved; // the node being dragged (committed on release)
    PatcherDoc shown;                        // what was last painted (to repaint on changes from elsewhere)
    std::unique_ptr<juce::FileChooser> sampleChooser; // Add plugin › Sampler (choose file)...

    const PatcherDoc* currentDoc() const { return target.doc (engine.getSong()); }

    void timerCallback() override
    {
        const auto* doc = currentDoc();
        if (doc == nullptr ? ! shown.empty() : ! (*doc == shown))
        {
            shown = doc != nullptr ? *doc : PatcherDoc {};
            repaint();
        }
    }

    void edit (std::function<void (PatcherDoc&)> fn)
    {
        engine.editSong ([t = target, fn] (Song& song) {
            if (auto* d = t.doc (song))
                fn (*d);
        });
        repaint();
    }

    // --- minimap (FL's Patcher map overview, round 151) ----------------------------------------------------------
    // A small overview of the whole map in the bottom-right corner: every node as a block, the part on screen as a
    // frame; click or drag in it to move the view there.
    juce::Rectangle<float> minimapArea() const
    {
        return { (float) getWidth() - 190.0f, (float) getHeight() - 24.0f - 124.0f, 180.0f, 118.0f };
    }

    // the world the minimap shows: every node and the view, with a margin (empty: nothing to show)
    juce::Rectangle<float> minimapWorld (const PatcherDoc& d) const
    {
        auto box = nodeBounds (d, patchFromHost).getUnion (nodeBounds (d, patchToHost)).getUnion (nodeBounds (d, patchSurface));
        for (int n = 0; n < (int) d.nodes.size(); ++n)
            box = box.getUnion (nodeBounds (d, n));
        const auto view = juce::Rectangle<float> (toWorld ({ 0.0f, 0.0f }), toWorld ({ (float) getWidth(), (float) getHeight() - 24.0f }));
        return box.getUnion (view).expanded (20.0f);
    }

    // world → minimap: the world box fitted into the area, centred
    juce::AffineTransform minimapTransform (const PatcherDoc& d) const
    {
        const auto world = minimapWorld (d);
        const auto area = minimapArea().reduced (4.0f);
        const auto scale = std::min (area.getWidth() / world.getWidth(), area.getHeight() / world.getHeight());
        const auto used = juce::Point<float> (world.getWidth(), world.getHeight()) * scale;
        const auto origin = area.getCentre() - used / 2.0f;
        return juce::AffineTransform::translation (-world.getX(), -world.getY()).scaled (scale).translated (origin.x, origin.y);
    }

    juce::Point<float> minimapToWorld (const PatcherDoc& d, juce::Point<float> screen) const
    {
        return screen.transformedBy (minimapTransform (d).inverted());
    }

    void centreViewOn (juce::Point<float> world)
    {
        offset = world - juce::Point<float> ((float) getWidth(), (float) getHeight() - 24.0f) / (2.0f * zoom);
        repaint();
    }

    void paintMinimap (juce::Graphics& g, const PatcherDoc& d) const
    {
        const auto area = minimapArea();
        if (area.getX() < 200.0f || area.getY() < 60.0f) // a small window: the map itself is the overview
            return;
        g.setColour (col::bg1.withAlpha (0.92f));
        g.fillRoundedRectangle (area, 4.0f);
        g.setColour (col::border);
        g.drawRoundedRectangle (area.reduced (0.5f), 4.0f, 1.0f);
        const auto t = minimapTransform (d);
        juce::Graphics::ScopedSaveState save (g);
        g.reduceClipRegion (area.reduced (2.0f).toNearestInt());
        auto block = [&] (int n, juce::Colour c) {
            g.setColour (c);
            g.fillRect (nodeBounds (d, n).transformedBy (t));
        };
        block (patchFromHost, col::bg5);
        block (patchToHost, col::bg5);
        block (patchSurface, col::bg5);
        for (int n = 0; n < (int) d.nodes.size(); ++n)
            block (n, n == selectedNode ? col::accent : (d.nodes[(std::size_t) n].generator ? col::info : col::knobArc).withAlpha (0.8f));
        const auto view = juce::Rectangle<float> (toWorld ({ 0.0f, 0.0f }), toWorld ({ (float) getWidth(), (float) getHeight() - 24.0f })).transformedBy (t);
        g.setColour (col::textPrimary.withAlpha (0.75f));
        g.drawRect (view, 1.0f);
    }

    // --- geometry ----------------------------------------------------------------------------------------------
    juce::Point<float> toScreen (juce::Point<float> world) const { return (world - offset) * zoom; }
    juce::Point<float> toWorld (juce::Point<float> screen) const { return screen / zoom + offset; }

    static juce::Point<float> rawPosition (const PatcherDoc& d, int n)
    {
        if (n == patchFromHost)
            return { d.inputX, d.inputY };
        if (n == patchToHost)
            return { d.outputX, d.outputY };
        if (n == patchSurface)
            return { d.surfaceX, d.surfaceY };
        return { d.nodes[(std::size_t) n].x, d.nodes[(std::size_t) n].y };
    }
    juce::Point<float> nodePosition (const PatcherDoc& d, int n) const
    {
        if (moved && n == selectedNode && dragging == Drag::node)
            return *moved;
        return rawPosition (d, n);
    }
    static void setNodePosition (PatcherDoc& d, int n, juce::Point<float> p)
    {
        if (n == patchFromHost)
            d.inputX = p.x, d.inputY = p.y;
        else if (n == patchToHost)
            d.outputX = p.x, d.outputY = p.y;
        else if (n == patchSurface)
            d.surfaceX = p.x, d.surfaceY = p.y;
        else if (n >= 0 && n < (int) d.nodes.size())
            d.nodes[(std::size_t) n].x = p.x, d.nodes[(std::size_t) n].y = p.y;
    }

    int extraOutputs (const PatcherDoc& d) const
    {
        int k = (int) d.outputTracks.size();
        for (const auto& w : d.wires)
            if (w.kind == PatchWire::Kind::audio && w.to == patchToHost)
                k = std::max (k, w.toPort);
        return k;
    }

    std::vector<Pin> hostPins (const PatcherDoc& d, bool input) const
    {
        std::vector<Pin> pins;
        const auto at = nodePosition (d, input ? patchFromHost : patchToHost);
        auto y = at.y + headerHeight + pinSpacing * 0.5f;
        auto add = [&] (PatchWire::Kind kind, int port, const juce::String& label, bool adds = false) {
            Pin p;
            p.node = input ? patchFromHost : patchToHost;
            p.output = input;
            p.kind = kind;
            p.port = port;
            p.at = { input ? at.x + nodeWidth : at.x, y };
            p.label = label;
            p.adds = adds;
            pins.push_back (p);
            y += pinSpacing;
        };
        if (input)
        {
            if (! target.channel)
            {
                add (PatchWire::Kind::audio, 0, tr ("Audio input"));
                add (PatchWire::Kind::audio, 1, tr ("Side-chain"));
            }
            if (patcher::fromNotesShown (d, target.channel)) // (a mixer slot has no notes of its own)
                add (PatchWire::Kind::midi, 0, tr ("Notes"));
            // what LYNX sends on MIDI port N (a MIDI Out on that port; FL's "MIDI port N" pins, round 162)
            for (const auto port : patcher::shownFromMidiPorts (d))
                add (PatchWire::Kind::midi, patchMidiPortPin (port), tr ("MIDI port {0}", { juce::String (port) }));
        }
        else
        {
            add (PatchWire::Kind::audio, 0, tr ("Audio output"));
            for (int k = 1; k <= extraOutputs (d); ++k)
            {
                const auto track = k <= (int) d.outputTracks.size() ? d.outputTracks[(std::size_t) k - 1] : -1;
                add (PatchWire::Kind::audio, k, track > 0 ? tr ("Insert {0}", { juce::String (track) }) : tr ("Output {0}", { juce::String (k + 1) }));
            }
            add (PatchWire::Kind::audio, extraOutputs (d) + 1, tr ("+ output"), true);
            // the notes out to the plugins on a MIDI port (FL's "MIDI port N")
            const auto ports = patcher::shownToMidiPorts (d);
            for (const auto port : ports)
                add (PatchWire::Kind::midi, port, tr ("MIDI port {0}", { juce::String (port) }));
            if (ports.empty() || ports.back() + 1 < patchMidiPorts)
                add (PatchWire::Kind::midi, ports.empty() ? 1 : ports.back() + 1, tr ("+ MIDI port"), true);
        }
        return pins;
    }

    std::vector<Pin> nodePins (const PatcherDoc& d, int n) const
    {
        std::vector<Pin> pins;
        const auto& node = d.nodes[(std::size_t) n];
        const auto ports = patcher::nodePorts (node);
        const auto at = nodePosition (d, n);
        auto left = at.y + headerHeight + pinSpacing * 0.5f, right = left;
        auto add = [&] (bool output, PatchWire::Kind kind, int port, const juce::String& label) {
            Pin p;
            p.node = n;
            p.output = output;
            p.kind = kind;
            p.port = port;
            auto& y = output ? right : left;
            p.at = { output ? at.x + nodeWidth : at.x, y };
            y += pinSpacing;
            p.label = label;
            pins.push_back (p);
        };
        if (ports.midiIn)
            add (false, PatchWire::Kind::midi, 0, tr ("Notes"));
        if (ports.midiOut) // a MIDI Out node's notes (round 162); a VFX node's voice outputs (round 164b)
        {
            const auto kind = vfx::kindOf (node.channel);
            const auto shown = kind ? patcher::shownMidiOutputs (d, n) : 1;
            std::optional<vfx::KeyboardSplitterSettings> zones; // (the Keyboard Splitter's outputs carry the zones' names)
            if (kind == vfx::Kind::keyboardSplitter)
                if (const auto st = vfx::readState (node.channel.pluginState))
                    zones = vfx::parseKeyboardSplitter (st->fl);
            for (int o = 0; o < shown; ++o)
                add (true, PatchWire::Kind::midi, o,
                     zones && o < 16 && ! zones->zones[(std::size_t) o].name.empty() ? juce::String::fromUTF8 (zones->zones[(std::size_t) o].name.c_str())
                     : kind ? juce::String (vfx::outputName (*kind, o))
                            : tr ("MIDI out"));
        }
        for (int i = 0; i < ports.audioIns; ++i)
            add (false, PatchWire::Kind::audio, i, i == 0 ? tr ("In") : tr ("Side-chain"));
        const auto splitter = ! node.generator && node.effect.kind == EffectKind::frequencySplitter && node.busHeads.empty();
        for (int o = 0; o < ports.audioOuts; ++o)
            add (true, PatchWire::Kind::audio, o,
                 o == 0                    ? tr ("Out")
                 : splitter && o == 1      ? tr ("Low")
                 : splitter && o == 2      ? tr ("Mid")
                 : splitter && o == 3      ? tr ("High")
                                           : tr ("Out {0}", { juce::String (o + 1) }));
        for (int c = 0; c < ports.controlOuts; ++c)
            add (true, PatchWire::Kind::control, c, tr ("Control {0}", { juce::String (c + 1) }));
        return pins;
    }

    // the Surface node (round 151): one control output per Surface control (an XY pad two)
    std::vector<Pin> surfacePins (const PatcherDoc& d) const
    {
        std::vector<Pin> pins;
        const auto at = nodePosition (d, patchSurface);
        auto y = at.y + headerHeight + pinSpacing * 0.5f;
        int port = 0;
        for (const auto& c : d.surface)
            for (int k = 0; k < c.outputs(); ++k)
            {
                Pin p;
                p.node = patchSurface;
                p.output = true;
                p.kind = PatchWire::Kind::control;
                p.port = port++;
                p.at = { at.x + nodeWidth, y };
                p.label = juce::String::fromUTF8 (c.name.c_str()) + (c.outputs() > 1 ? (k == 0 ? " X" : " Y") : "");
                pins.push_back (p);
                y += pinSpacing;
            }
        return pins;
    }

    std::vector<Pin> allPins (const PatcherDoc& d) const
    {
        auto pins = hostPins (d, true);
        for (auto& p : surfacePins (d))
            pins.push_back (p);
        for (auto& p : hostPins (d, false))
            pins.push_back (p);
        for (int n = 0; n < (int) d.nodes.size(); ++n)
            for (auto& p : nodePins (d, n))
                pins.push_back (p);
        return pins;
    }

    juce::Rectangle<float> nodeBounds (const PatcherDoc& d, int n) const
    {
        const auto at = nodePosition (d, n);
        int rows = 1;
        if (n == patchFromHost || n == patchToHost)
            rows = std::max (1, (int) hostPins (d, n == patchFromHost).size());
        else if (n == patchSurface)
            rows = std::max (1, d.surfaceOutputs());
        else
        {
            const auto ports = patcher::nodePorts (d.nodes[(std::size_t) n]);
            const auto midiOuts = ! ports.midiOut ? 0 : vfx::kindOf (d.nodes[(std::size_t) n].channel) ? patcher::shownMidiOutputs (d, n) : 1;
            rows = std::max ({ 1, ports.audioIns + (ports.midiIn ? 1 : 0), ports.audioOuts + ports.controlOuts + midiOuts });
        }
        return { at.x, at.y, nodeWidth, headerHeight + (float) rows * pinSpacing + 4.0f };
    }

    int nodeAt (const PatcherDoc& d, juce::Point<float> world) const
    {
        for (int n = (int) d.nodes.size() - 1; n >= 0; --n)
            if (nodeBounds (d, n).contains (world))
                return n;
        for (const auto host : { patchFromHost, patchToHost, patchSurface })
            if (nodeBounds (d, host).contains (world))
                return host;
        return noNode;
    }

    std::optional<Pin> pinAt (const PatcherDoc& d, juce::Point<float> world) const
    {
        for (const auto& p : allPins (d))
            if (p.at.getDistanceFrom (world) <= pinRadius + 4.0f / zoom)
                return p;
        return std::nullopt;
    }

    const Pin* findPin (const std::vector<Pin>& pins, int node, bool output, PatchWire::Kind kind, int port) const
    {
        for (const auto& p : pins)
            if (p.node == node && p.output == output && p.kind == kind && p.port == port && ! p.adds)
                return &p;
        return nullptr;
    }

    // a control cable ends on the top of its target node
    juce::Point<float> controlTarget (const PatcherDoc& d, int node) const
    {
        const auto b = nodeBounds (d, node);
        return { b.getCentreX(), b.getY() };
    }

    bool wireEnds (const PatcherDoc& d, const std::vector<Pin>& pins, const PatchWire& w, juce::Point<float>& a, juce::Point<float>& b) const
    {
        const auto* from = findPin (pins, w.from, true, w.kind, w.fromPort);
        if (from == nullptr)
            return false;
        a = from->at;
        if (w.kind == PatchWire::Kind::control)
        {
            if (w.to < 0 || w.to >= (int) d.nodes.size())
                return false;
            b = controlTarget (d, w.to);
            return true;
        }
        const auto* to = findPin (pins, w.to, false, w.kind, w.toPort);
        if (to == nullptr)
            return false;
        b = to->at;
        return true;
    }

    int wireAt (const PatcherDoc& d, juce::Point<float> screen) const
    {
        const auto pins = allPins (d);
        for (int w = (int) d.wires.size() - 1; w >= 0; --w)
        {
            juce::Point<float> a, b;
            if (! wireEnds (d, pins, d.wires[(std::size_t) w], a, b))
                continue;
            const auto path = curve (toScreen (a), toScreen (b));
            juce::Point<float> nearest;
            path.getNearestPoint (screen, nearest);
            if (nearest.getDistanceFrom (screen) < 5.0f)
                return w;
        }
        return -1;
    }

    static juce::Path curve (juce::Point<float> a, juce::Point<float> b)
    {
        juce::Path p;
        const auto dx = std::max (40.0f, std::abs (b.x - a.x) * 0.5f);
        p.startNewSubPath (a);
        p.cubicTo (a.x + dx, a.y, b.x - dx, b.y, b.x, b.y);
        return p;
    }
    static void drawCurve (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, float thickness)
    {
        g.strokePath (curve (a, b), juce::PathStrokeType (thickness));
    }

    // --- drawing -----------------------------------------------------------------------------------------------
    void drawWire (juce::Graphics& g, const PatcherDoc& d, const std::vector<Pin>& pins, const PatchWire& w, bool selected) const
    {
        juce::Point<float> a, b;
        if (! wireEnds (d, pins, w, a, b))
            return;
        auto colour = selected ? col::accent : wireColour (w.kind);
        if (! w.enabled)
            colour = colour.withAlpha (0.3f);
        g.setColour (colour);
        const auto sa = toScreen (a), sb = toScreen (b);
        drawCurve (g, sa, sb, selected ? 2.5f : 1.6f);
        juce::String text;
        if (w.kind == PatchWire::Kind::audio && std::abs (w.gain - 1.0f) > 1e-4f)
            text = juce::String (w.gain, 2) + "x";
        if (w.kind == PatchWire::Kind::control && w.to >= 0 && w.to < (int) d.nodes.size())
            text = paramName (d.nodes[(std::size_t) w.to], w.toPort);
        if (text.isNotEmpty())
        {
            g.setFont (theme::type::caption);
            const auto mid = (sa + sb) * 0.5f;
            g.drawText (text, juce::Rectangle<float> (mid.x - 60.0f, mid.y - 16.0f, 120.0f, 14.0f), juce::Justification::centred);
        }
    }

    void drawBox (juce::Graphics& g, juce::Rectangle<float> world, const juce::String& title, bool selected, bool host, bool off) const
    {
        const auto r = juce::Rectangle<float> (toScreen (world.getTopLeft()), toScreen (world.getBottomRight()));
        g.setColour (host ? col::bg3 : col::bg2);
        g.fillRoundedRectangle (r, 5.0f);
        g.setColour (host ? col::accentMuted : col::bg4);
        g.fillRoundedRectangle (r.withHeight (headerHeight * zoom), 5.0f);
        g.setColour (selected ? col::accent : col::borderStrong);
        g.drawRoundedRectangle (r, 5.0f, selected ? 2.0f : 1.0f);
        g.setColour (off ? col::textDisabled : col::textPrimary);
        g.setFont (theme::type::body * std::clamp (zoom, 0.7f, 1.3f));
        g.drawText (title, r.withHeight (headerHeight * zoom).reduced (6.0f, 0.0f), juce::Justification::centredLeft, true);
    }

    void drawHost (juce::Graphics& g, const PatcherDoc& d, bool input) const
    {
        const auto n = input ? patchFromHost : patchToHost;
        drawBox (g, nodeBounds (d, n), input ? tr ("From LYNX") : tr ("To LYNX"), selectedNode == n, true, false);
    }

    void drawNode (juce::Graphics& g, const PatcherDoc& d, int n) const
    {
        const auto& node = d.nodes[(std::size_t) n];
        const auto off = ! node.generator && ! node.effect.enabled;
        drawBox (g, nodeBounds (d, n), juce::String::fromUTF8 (patcher::nodeName (node).c_str()), selectedNode == n, false, off);
        if (node.generator)
        {
            const auto r = nodeBounds (d, n);
            g.setColour (col::info);
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (toScreen ({ r.getRight() - 10.0f, r.getY() + headerHeight * 0.5f })));
        }
    }

    void drawPin (juce::Graphics& g, const Pin& p) const
    {
        const auto s = toScreen (p.at);
        const auto r = pinRadius * std::clamp (zoom, 0.6f, 1.5f);
        g.setColour (p.adds ? col::textDisabled : wireColour (p.kind));
        if (p.kind == PatchWire::Kind::midi)
            g.fillRect (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (s));
        else
            g.fillEllipse (juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (s));
        if (zoom >= 0.6f)
        {
            g.setColour (col::textSecondary);
            g.setFont (theme::type::caption);
            const auto w = 64.0f;
            const auto box = p.output ? juce::Rectangle<float> (s.x - r - 4.0f - w, s.y - 7.0f, w, 14.0f)
                                      : juce::Rectangle<float> (s.x + r + 4.0f, s.y - 7.0f, w, 14.0f);
            g.drawText (p.label, box, p.output ? juce::Justification::centredRight : juce::Justification::centredLeft, true);
        }
    }

    // --- editing -----------------------------------------------------------------------------------------------
    static juce::String paramName (const PatchNode& node, int param)
    {
        if (! node.generator && node.effect.kind != EffectKind::plugin)
        {
            const auto& info = effectInfo (node.effect.kind);
            if (param >= 0 && param < (int) info.params.size())
                return juce::String::fromUTF8 (info.params[(std::size_t) param].name);
        }
        return tr ("Parameter {0}", { juce::String (param + 1) });
    }

    void finishWire (const PatcherDoc& d, juce::Point<float> world)
    {
        const auto from = dragFrom;
        if (from.kind == PatchWire::Kind::control)
        {
            const auto n = nodeAt (d, world);
            if (n < 0 || n == from.node)
                return;
            chooseParameter (d.nodes[(std::size_t) n], [this, from, n] (int param) {
                PatchWire w;
                w.kind = PatchWire::Kind::control;
                w.from = from.node;
                w.fromPort = from.port;
                w.to = n;
                w.toPort = param;
                edit ([w] (PatcherDoc& doc) { patcher::connect (doc, w); });
            });
            return;
        }
        const auto pin = pinAt (d, world);
        if (! pin || pin->output || pin->kind != from.kind)
            return;
        PatchWire w;
        w.kind = from.kind;
        w.from = from.node;
        w.fromPort = from.port;
        w.to = pin->node;
        w.toPort = pin->port;
        edit ([w] (PatcherDoc& doc) {
            if (w.kind == PatchWire::Kind::audio && w.to == patchToHost && w.toPort > (int) doc.outputTracks.size())
                doc.outputTracks.resize ((std::size_t) w.toPort, -1);
            patcher::connect (doc, w);
        });
    }

    void chooseParameter (const PatchNode& node, std::function<void (int)> done)
    {
        juce::PopupMenu menu;
        if (! node.generator && node.effect.kind != EffectKind::plugin)
        {
            const auto& info = effectInfo (node.effect.kind);
            for (int p = 0; p < (int) info.params.size(); ++p)
                menu.addItem (juce::String::fromUTF8 (info.params[(std::size_t) p].name), [done, p] { done (p); });
        }
        else
        {
            // a plugin: its parameter list, from the running instance
            juce::AudioPluginInstance* instance = nullptr;
            if (const auto* graph = runningGraph())
            {
                const auto index = selectedOrDropped (node);
                if (auto* fx = dynamic_cast<PluginEffect*> (graph->effectAt (index).get()))
                    instance = &fx->getInstance();
                else if (auto* in = dynamic_cast<PluginInstrument*> (graph->instrumentAt (index).get()))
                    instance = &in->getInstance();
            }
            if (instance != nullptr)
            {
                const auto& params = instance->getParameters();
                for (int p = 0; p < params.size() && p < 512; ++p)
                    menu.addItem (params[p]->getName (40), [done, p] { done (p); });
            }
            else
                for (int p = 0; p < 16; ++p)
                    menu.addItem (tr ("Parameter {0}", { juce::String (p + 1) }), [done, p] { done (p); });
        }
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    int selectedOrDropped (const PatchNode& node) const
    {
        if (const auto* d = currentDoc())
            for (int n = 0; n < (int) d->nodes.size(); ++n)
                if (&d->nodes[(std::size_t) n] == &node)
                    return n;
        return -1;
    }

    patcher::Graph* runningGraph() const
    {
        if (target.channel)
            return patcher::graphOf (engine.getChannelInstrument (target.index).get());
        return patcher::graphOf (engine.getEffect (target.index, target.slot).get());
    }

    void addNode (PatchNode node, juce::Point<float> at)
    {
        node.x = at.x;
        node.y = at.y;
        const auto channel = target.channel;
        edit ([node, channel] (PatcherDoc& d) {
            const auto n = patcher::addNode (d, node);
            // a new instrument gets the notes (FL cables them on the generator Patcher's first instrument too)
            if (node.generator && channel)
            {
                PatchWire notes;
                notes.kind = PatchWire::Kind::midi;
                notes.to = n;
                patcher::connect (d, notes);
                if (patcher::nodePorts (node).audioOuts > 0
                    && std::none_of (d.wires.begin(), d.wires.end(), [] (const PatchWire& w) { return w.kind == PatchWire::Kind::audio && w.to == patchToHost; }))
                {
                    PatchWire out;
                    out.from = n;
                    patcher::connect (d, out);
                }
            }
        });
    }

    // FL's Patcher › Add plugin (round 162, issue #7): every generator and effect LYNX has, by FL's categories, in
    // columns; "Open plugin picker..." and "More..." (the installed VST / VST3 / CLAP plugins) on each side.
    void showAddMenu (juce::Point<float> at)
    {
        juce::PopupMenu menu;
        menu.addItem (tr ("Open plugin picker..."), [this, at] { pickPlugin (target.channel, at); });
        menu.addSubMenu (tr ("Add plugin"), addPluginMenu (at));
        menu.addSeparator();
        menu.addItem (tr ("Fit to window"), [this] {
            fit();
            repaint();
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

public:
    juce::PopupMenu addPluginMenu (juce::Point<float> at)
    {
        struct Item
        {
            juce::String name;
            bool enabled = true;
            std::function<void()> add;
        };
        std::map<juce::String, std::vector<Item>> generators, effects;
        auto addChannel = [this, at] (ChannelDef c) {
            PatchNode n;
            n.generator = true;
            n.channel = std::move (c);
            addNode (n, at);
        };
        auto builtIn = [&] (const char* category, InstrumentKind kind, const juce::String& name, std::function<void (ChannelDef&)> setup = nullptr) {
            generators[tr (category)].push_back ({ name, true, [addChannel, kind, name, setup] {
                                                      ChannelDef c;
                                                      c.instrument = kind;
                                                      c.name = name.toStdString();
                                                      if (setup)
                                                          setup (c);
                                                      addChannel (c);
                                                  } });
        };
        // generators
        builtIn ("Controller", InstrumentKind::controller, tr ("Envelope Controller"),
                 [] (ChannelDef& c) { c.controllerState = writeEnvelopeController (defaultEnvelopeController()); });
        builtIn ("Controller", InstrumentKind::controller, tr ("Keyboard Controller"), [] (ChannelDef& c) { c.controllerState = defaultKeyboardController(); });
        builtIn ("Controller", InstrumentKind::dashboard, "Dashboard", [] (ChannelDef& c) { c.patcher.surface = surface::starterControls(); });
        builtIn ("Controller", InstrumentKind::voltage, "Voltage Controller", [] (ChannelDef& c) { c.pluginState = voltage::write (voltage::Settings {}); });
        for (const auto kind : { InstrumentKind::kick, InstrumentKind::snare, InstrumentKind::hat, InstrumentKind::clap })
            builtIn ("Drum", kind, juce::String (effectNameOf (kind)));
        builtIn ("MIDI", InstrumentKind::midiOut, "MIDI Out", [] (ChannelDef& c) { midictl::writeMidiOut (c.pluginState, midictl::MidiOutSettings {}); });
        builtIn ("Misc", InstrumentKind::soundfont, "LYNX Soundfont", [] (ChannelDef& c) { c = sf::makeChannel(); });
        builtIn ("Patcher", InstrumentKind::patcher, "Patcher");
        builtIn ("Synth classic", InstrumentKind::synth, tr ("Synth"));
        generators[tr ("Sampler")].push_back ({ tr ("Sampler (choose file)..."), true, [this, at] { chooseSampleNode (at); } });
        for (const auto& e : natives::instrumentEngines())
            generators[tr (engineCategory (e.kind))].push_back ({ flEngineName (e), natives::available (e, flStudioAccess()), [kind = e.kind, addChannel] {
                                                                    if (auto c = natives::makeChannel (kind, flStudioAccess()))
                                                                        addChannel (*c);
                                                                } });
        // effects
        for (const auto kind : addableEffects())
            effects[tr (effectCategory (kind))].push_back ({ addableEffectName (kind), addableEffectAvailable (kind), [this, kind, at] {
                                                               const auto fx = newAddableEffect (kind, engine.getSong().bpm);
                                                               if (! fx)
                                                                   return;
                                                               PatchNode n;
                                                               n.effect = *fx;
                                                               addNode (n, at);
                                                           } });

        // FL's VFX note processors (round 164b): FL files them under Effects / Patcher
        for (const auto kind : vfx::allKinds)
            effects[tr ("Patcher")].push_back ({ vfx::flName (kind), true, [addChannel, kind] {
                                                   ChannelDef c;
                                                   vfx::makeChannel (c, kind);
                                                   addChannel (c);
                                               } });

        juce::PopupMenu pluginEffects, pluginInstruments;
        addPluginItems (pluginEffects, engine, false, [this, at] (PluginRef ref) {
            PatchNode n;
            n.effect.kind = EffectKind::plugin;
            n.effect.plugin = ref;
            addNode (n, at);
        }, this);
        addPluginItems (pluginInstruments, engine, true, [this, at] (PluginRef ref) {
            PatchNode n;
            n.generator = true;
            n.channel.instrument = InstrumentKind::plugin;
            n.channel.name = ref.name;
            n.channel.plugin = ref;
            addNode (n, at);
        }, this, addChannel);

        juce::PopupMenu add;
        float rows = 0.0f; // (a section header is a bit taller than an item)
        constexpr float columnRows = 33.0f;
        auto row = [&] (float h = 1.0f) {
            if ((rows += h) > columnRows)
            {
                add.addColumnBreak();
                rows = h;
            }
        };
        auto side = [&] (const juce::String& title, bool instruments, std::map<juce::String, std::vector<Item>>& groups,
                         const std::vector<const char*>& order, juce::PopupMenu& more) {
            add.addSectionHeader (title);
            row (1.2f);
            add.addItem (tr ("Open plugin picker..."), [this, at, instruments] { pickPlugin (instruments, at); });
            row();
            add.addItem (tr ("Browse plugin database"), PatcherWindows::onBrowsePluginDatabase != nullptr, false,
                         [this, at, instruments] { browsePluginDatabase (instruments, at); });
            row();
            add.addSubMenu (tr ("More..."), more);
            row();
            for (const auto* category : order)
            {
                auto& items = groups[tr (category)];
                if (items.empty())
                    continue;
                std::sort (items.begin(), items.end(), [] (const Item& x, const Item& y) { return x.name.compareNatural (y.name) < 0; });
                if (rows + 1.2f + std::min (4.0f, (float) items.size()) > columnRows) // (a header never ends a column)
                {
                    add.addColumnBreak();
                    rows = 0.0f;
                }
                add.addSectionHeader (tr (category));
                row (1.2f);
                for (const auto& item : items)
                {
                    add.addItem (item.name, item.enabled, false, item.add);
                    row();
                }
            }
        };
        side (tr ("Generators"), true, generators, { "Controller", "Drum", "MIDI", "Misc", "Patcher", "Sampler", "Synth classic", "Synth special" },
              pluginInstruments);
        add.addColumnBreak();
        rows = 0.0f;
        side (tr ("Effects"), false, effects, { "Controller", "Delay reverb", "Distortion", "Dynamics", "Filter", "Flanger", "Misc", "Patcher", "Visual" },
              pluginEffects);
        return add;
    }
private:

    // FL's Browse plugin database: the Browser opens at its Plugin database; what is picked there next comes here
    void browsePluginDatabase (bool instruments, juce::Point<float> at)
    {
        juce::Component::SafePointer<PatcherEditor> safe (this);
        pendingBrowserPick = [safe, instruments, at] (const ChannelDef* c, const EffectSlot* fx) {
            if (safe == nullptr || (c != nullptr) != instruments)
                return false;
            PatchNode n;
            n.generator = instruments;
            if (c != nullptr)
                n.channel = *c;
            else
                n.effect = *fx;
            safe->addNode (n, at);
            if (auto* w = safe->getTopLevelComponent())
                w->toFront (true);
            return true;
        };
        PatcherWindows::onBrowsePluginDatabase (instruments);
    }

    void pickPlugin (bool instruments, juce::Point<float> at)
    {
        juce::Component::SafePointer<PatcherEditor> safe (this);
        auto builtIn = [safe, at] (ChannelDef c) {
            if (safe == nullptr)
                return;
            PatchNode n;
            n.generator = true;
            n.channel = std::move (c);
            safe->addNode (n, at);
        };
        showPluginPicker (engine, instruments, [safe, instruments, at] (PluginRef ref) {
            if (safe == nullptr)
                return;
            PatchNode n;
            n.generator = instruments;
            if (instruments)
            {
                n.channel.instrument = InstrumentKind::plugin;
                n.channel.name = ref.name;
                n.channel.plugin = ref;
            }
            else
            {
                n.effect.kind = EffectKind::plugin;
                n.effect.plugin = ref;
            }
            safe->addNode (n, at);
        }, builtIn);
    }

    void chooseSampleNode (juce::Point<float> at)
    {
        sampleChooser = std::make_unique<juce::FileChooser> (tr ("Choose sample"), juce::File(), engine.getSampleLibrary().wildcard());
        juce::Component::SafePointer<PatcherEditor> safe (this);
        sampleChooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [safe, at] (const juce::FileChooser& chooser) {
            const auto file = chooser.getResult();
            if (safe == nullptr || ! file.existsAsFile())
                return;
            if (safe->engine.getSampleLibrary().load (file.getFullPathName()) == nullptr)
            {
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, tr ("Sample"), tr ("Could not read {0}", { file.getFileName() }));
                return;
            }
            PatchNode n;
            n.generator = true;
            n.channel.instrument = InstrumentKind::sampler;
            n.channel.samplePath = file.getFullPathName().toStdString();
            n.channel.name = file.getFileNameWithoutExtension().toStdString();
            safe->addNode (n, at);
        });
    }

    // FL's Fruity Keyboard Controller state (REA_rodada16 §1.4, version 3): each key's value rising from 0 to 1 over the
    // keyboard, note-off value 0 with auto release (LYNX's own default)
    static std::vector<std::uint8_t> defaultKeyboardController()
    {
        std::vector<std::uint8_t> s (553, 0);
        const std::uint32_t version = 3;
        std::memcpy (s.data(), &version, 4);
        for (int k = 0; k < 132; ++k)
        {
            const auto v = (float) std::min (1.0, k / 127.0);
            std::memcpy (s.data() + 12 + k * 4, &v, 4);
        }
        s[552] = 1;
        return s;
    }

    // The FL Plugin picker's categories (FL Studio 2026, issue #7 screenshot): generators
    static const char* engineCategory (InstrumentKind kind)
    {
        switch (kind)
        {
            case InstrumentKind::bassdrum: case InstrumentKind::drumaxx: case InstrumentKind::drumpad: case InstrumentKind::fruitkick:
            case InstrumentKind::drumsynth:
                return "Drum";
            case InstrumentKind::boobass: case InstrumentKind::flkeys: case InstrumentKind::soundfont:
                return "Misc";
            case InstrumentKind::granulizer: case InstrumentKind::directwave: case InstrumentKind::sampler:
                return "Sampler";
            case InstrumentKind::beepmap: case InstrumentKind::harmor: case InstrumentKind::morphine: case InstrumentKind::ogun:
            case InstrumentKind::autogun: case InstrumentKind::plucked: case InstrumentKind::sakura: case InstrumentKind::slayer:
                return "Synth special";
            default:
                return "Synth classic";
        }
    }

    // ...and effects
    static const char* effectCategory (EffectKind kind)
    {
        switch (kind)
        {
            case EffectKind::formulaController: case EffectKind::peakController: case EffectKind::xyController: case EffectKind::xyzController:
            case EffectKind::controlSurface:
                return "Controller";
            case EffectKind::delay: case EffectKind::reverb: case EffectKind::epochVerb: case EffectKind::atrium: case EffectKind::foilPlate:
            case EffectKind::nebula: case EffectKind::halo: case EffectKind::horizon: case EffectKind::echoform: case EffectKind::spiralEcho:
            case EffectKind::reeverb2: case EffectKind::reeverb1: case EffectKind::fruityDelay2: case EffectKind::fruityDelay3:
            case EffectKind::fruityDelayBank: case EffectKind::luxeVerb: case EffectKind::multibandDelay: case EffectKind::convolver:
                return "Delay reverb";
            case EffectKind::distortion: case EffectKind::lofi: case EffectKind::fruityWaveShaper: case EffectKind::fruitySoftClipper:
            case EffectKind::fruityBloodOverdrive: case EffectKind::fruityFastDist: case EffectKind::hardcore: case EffectKind::distructor:
            case EffectKind::ember:
                return "Distortion";
            case EffectKind::compressor: case EffectKind::limiter: case EffectKind::apexLimiter: case EffectKind::volumeGate:
            case EffectKind::crestCompressor: case EffectKind::prismMultiband: case EffectKind::sentryGate: case EffectKind::fruityLimiter:
            case EffectKind::fruityCompressor: case EffectKind::transientProcessor: case EffectKind::fruityMultiband: case EffectKind::maximus:
            case EffectKind::glowLeveler: case EffectKind::hushDeEsser: case EffectKind::emphasizer: case EffectKind::squeeze:
                return "Dynamics";
            case EffectKind::eq: case EffectKind::parametricEq: case EffectKind::facetEq: case EffectKind::filter: case EffectKind::fruityFastLP:
            case EffectKind::fruityFreeFilter: case EffectKind::fruityFilter: case EffectKind::equo: case EffectKind::lovePhilter:
            case EffectKind::vocoder: case EffectKind::vocodex: case EffectKind::aether: case EffectKind::flLowLifter:
                return "Filter";
            case EffectKind::chorus: case EffectKind::fruityChorus: case EffectKind::hyperChorus: case EffectKind::vintageChorus:
            case EffectKind::fruityFlangus: case EffectKind::fruityFlanger: case EffectKind::fruityPhaser: case EffectKind::vintagePhaser:
            case EffectKind::orbit: case EffectKind::latticeMod:
                return "Flanger";
            case EffectKind::patcher:
                return "Patcher";
            case EffectKind::waveCandy: case EffectKind::visualizer:
                return "Visual";
            default:
                return "Misc";
        }
    }

    static std::string effectNameOf (InstrumentKind kind)
    {
        switch (kind)
        {
            case InstrumentKind::kick: return "Kick";
            case InstrumentKind::snare: return "Snare";
            case InstrumentKind::hat: return "Hat";
            case InstrumentKind::clap: return "Clap";
            default: return "Synth";
        }
    }

    void showNodeMenu (int n)
    {
        selectedNode = n;
        repaint();
        if (n == patchFromHost || n == patchToHost)
        {
            showHostMenu (n == patchFromHost);
            return;
        }
        if (n < 0)
            return;
        const auto* d = currentDoc();
        const auto& node = d->nodes[(std::size_t) n];
        juce::PopupMenu menu;
        menu.addItem (tr ("Open editor"), [this, n] { openNodeEditor (n); });
        menu.addItem (tr ("Rename..."), [this, n] { renameNode (n); });
        if (! node.generator)
            menu.addItem (tr ("Bypass"), true, ! node.effect.enabled, [this, n] {
                edit ([n] (PatcherDoc& doc) {
                    if (n < (int) doc.nodes.size())
                        doc.nodes[(std::size_t) n].effect.enabled = ! doc.nodes[(std::size_t) n].effect.enabled;
                });
            });
        menu.addSeparator();
        menu.addItem (tr ("Delete"), [this, n] {
            selectedNode = noNode;
            edit ([n] (PatcherDoc& doc) { patcher::removeNode (doc, n); });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    // FL's I/O node menu (round 162, Patcher_x64 FUN_00839ab0 / FUN_0083ce50): Disconnect all, and per kind of pin
    // (Events, Audio) Activate all / Deactivate all / Deactivate unused and every pin as a check item. A pin with a
    // cable is always active; deactivating one removes its cables.
    void showHostMenu (bool input)
    {
        if (currentDoc() != nullptr)
            hostMenu (input).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

public:
    juce::PopupMenu hostMenu (bool input, juce::PopupMenu* eventsOut = nullptr)
    {
        const auto* d = currentDoc();
        if (d == nullptr)
            return {};
        const auto host = input ? patchFromHost : patchToHost;
        auto touches = [host] (const PatchWire& w) { return w.from == host || w.to == host; };
        juce::PopupMenu menu;
        menu.addSectionHeader (input ? tr ("From LYNX") : tr ("To LYNX"));
        menu.addItem (tr ("Disconnect all"), std::any_of (d->wires.begin(), d->wires.end(), touches), false, [this, touches] {
            edit ([touches] (PatcherDoc& doc) { std::erase_if (doc.wires, touches); });
        });

        // Events: Notes (From LYNX, a generator Patcher), MIDI port 0…255
        juce::PopupMenu events;
        const auto shown = input ? patcher::shownFromMidiPorts (*d) : patcher::shownToMidiPorts (*d);
        auto isMidiPin = [input] (const PatchWire& w, int pin) {
            return w.kind == PatchWire::Kind::midi && (input ? w.from == patchFromHost && w.fromPort == pin : w.to == patchToHost && w.toPort == pin);
        };
        const auto channel = target.channel;
        events.addItem (tr ("Activate all"), [this, input] {
            edit ([input] (PatcherDoc& doc) {
                auto& list = input ? doc.fromMidiPorts : doc.toMidiPorts;
                list.resize (patchMidiPorts);
                for (int p = 0; p < patchMidiPorts; ++p)
                    list[(std::size_t) p] = p;
                if (input)
                    doc.fromNotesHidden = false;
            });
        });
        // (FL greys Deactivate all / unused while no pin is active)
        const auto anyActive = ! shown.empty() || (input && channel && patcher::fromNotesShown (*d, true));
        events.addItem (tr ("Deactivate all"), anyActive, false, [this, input] {
            edit ([input] (PatcherDoc& doc) {
                (input ? doc.fromMidiPorts : doc.toMidiPorts).clear();
                std::erase_if (doc.wires, [&] (const PatchWire& w) {
                    return w.kind == PatchWire::Kind::midi && (input ? w.from == patchFromHost : w.to == patchToHost);
                });
                if (input)
                    doc.fromNotesHidden = true;
            });
        });
        events.addItem (tr ("Deactivate unused"), anyActive, false, [this, input] { edit ([input] (PatcherDoc& doc) { patcher::deactivateUnusedMidiPins (doc, input); }); });
        events.addSeparator();
        int rows = 4;
        if (input && channel)
        {
            const auto notes = patcher::fromNotesShown (*d, true);
            events.addItem (tr ("Notes"), true, notes, [this, notes, isMidiPin] {
                edit ([notes, isMidiPin] (PatcherDoc& doc) {
                    doc.fromNotesHidden = notes;
                    if (notes)
                        std::erase_if (doc.wires, [&] (const PatchWire& w) { return isMidiPin (w, 0); });
                });
            });
            ++rows;
        }
        else if (input) // a mixer slot's Patcher: no channel notes reach it, only its MIDI ports (FL: the generator's Notes)
        {
            events.addItem (tr ("Notes"), false, false, nullptr);
            ++rows;
        }
        for (int port = 0; port < patchMidiPorts; ++port)
        {
            if (rows++ % 36 == 35) // FL's long list in columns
                events.addColumnBreak();
            const auto on = std::find (shown.begin(), shown.end(), port) != shown.end();
            events.addItem (tr ("MIDI port {0}", { juce::String (port) }), true, on, [this, port, on, input, isMidiPin] {
                edit ([port, on, input, isMidiPin] (PatcherDoc& doc) {
                    auto& list = input ? doc.fromMidiPorts : doc.toMidiPorts;
                    if (on)
                    {
                        std::erase (list, port);
                        const auto pin = input ? patchMidiPortPin (port) : port;
                        std::erase_if (doc.wires, [&] (const PatchWire& w) { return isMidiPin (w, pin); });
                    }
                    else if (std::find (list.begin(), list.end(), port) == list.end())
                        list.insert (std::upper_bound (list.begin(), list.end(), port), port);
                });
            });
        }
        if (eventsOut != nullptr)
            *eventsOut = events;
        menu.addSubMenu (tr ("Events"), events);

        // Audio: the I/O node's audio pins ("To LYNX": the main output and the extra outputs to other inserts)
        juce::PopupMenu audio;
        if (input)
        {
            if (! channel)
            {
                audio.addItem (tr ("Audio input"), false, true, nullptr);
                audio.addItem (tr ("Side-chain"), false, true, nullptr);
            }
        }
        else
        {
            const auto extras = extraOutputs (*d);
            audio.addItem (tr ("Add one"), [this, extras] {
                edit ([extras] (PatcherDoc& doc) { doc.outputTracks.resize ((std::size_t) extras + 1, -1); });
            });
            audio.addItem (tr ("Deactivate unused"), [this] {
                edit ([] (PatcherDoc& doc) {
                    // the extra outputs past the last one with a cable
                    int last = 0;
                    for (const auto& w : doc.wires)
                        if (w.kind == PatchWire::Kind::audio && w.to == patchToHost)
                            last = std::max (last, w.toPort);
                    if ((int) doc.outputTracks.size() > last)
                        doc.outputTracks.resize ((std::size_t) last);
                });
            });
            audio.addSeparator();
            audio.addItem (tr ("Audio output"), false, true, nullptr);
            for (int k = 1; k <= extras; ++k)
            {
                const auto track = k <= (int) d->outputTracks.size() ? d->outputTracks[(std::size_t) k - 1] : -1;
                audio.addItem (track > 0 ? tr ("Insert {0}", { juce::String (track) }) : tr ("Output {0}", { juce::String (k + 1) }), false, true, nullptr);
            }
        }
        if (audio.getNumItems() > 0)
            menu.addSubMenu (tr ("Audio"), audio);
        return menu;
    }
private:

    void showWireMenu (int w)
    {
        selectedWire = w;
        repaint();
        const auto* d = currentDoc();
        const auto wire = d->wires[(std::size_t) w];
        juce::PopupMenu menu;
        if (wire.kind == PatchWire::Kind::audio)
        {
            juce::PopupMenu gains;
            for (const auto g : { 0.25f, 0.5f, 0.707f, 1.0f, 1.41f, 2.0f })
                gains.addItem (juce::String (g, 2) + "x", true, std::abs (wire.gain - g) < 1e-3f, [this, w, g] {
                    edit ([w, g] (PatcherDoc& doc) {
                        if (w < (int) doc.wires.size())
                            doc.wires[(std::size_t) w].gain = g;
                    });
                });
            gains.addItem (tr ("Other..."), [this, w, wire] { askGain (w, wire.gain); });
            menu.addSubMenu (tr ("Cable gain"), gains);
        }
        menu.addItem (tr ("Enabled"), true, wire.enabled, [this, w] {
            edit ([w] (PatcherDoc& doc) {
                if (w < (int) doc.wires.size())
                    doc.wires[(std::size_t) w].enabled = ! doc.wires[(std::size_t) w].enabled;
            });
        });
        menu.addItem (tr ("Delete"), [this, w] {
            selectedWire = -1;
            edit ([w] (PatcherDoc& doc) {
                if (w < (int) doc.wires.size())
                    doc.wires.erase (doc.wires.begin() + w);
            });
        });
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition());
    }

    void askGain (int w, float gain)
    {
        auto* box = new juce::AlertWindow (tr ("Cable gain"), tr ("Linear gain of this cable (1 = unchanged, 0.5 = -6 dB):"), juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("gain", juce::String (gain, 3));
        box->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([this, safe = juce::Component::SafePointer<PatcherEditor> (this), box, w] (int r) {
                                  if (r == 1 && safe != nullptr)
                                  {
                                      const auto g = std::clamp (box->getTextEditorContents ("gain").getFloatValue(), 0.0f, 16.0f);
                                      edit ([w, g] (PatcherDoc& doc) {
                                          if (w < (int) doc.wires.size())
                                              doc.wires[(std::size_t) w].gain = g;
                                      });
                                  }
                              }),
                              true);
    }

    void renameNode (int n)
    {
        const auto* d = currentDoc();
        auto* box = new juce::AlertWindow (tr ("Rename"), {}, juce::MessageBoxIconType::NoIcon);
        box->addTextEditor ("name", juce::String::fromUTF8 (patcher::nodeName (d->nodes[(std::size_t) n]).c_str()));
        box->addButton (tr ("OK"), 1, juce::KeyPress (juce::KeyPress::returnKey));
        box->addButton (tr ("Cancel"), 0, juce::KeyPress (juce::KeyPress::escapeKey));
        box->enterModalState (true, juce::ModalCallbackFunction::create ([this, safe = juce::Component::SafePointer<PatcherEditor> (this), box, n] (int r) {
                                  if (r == 1 && safe != nullptr)
                                  {
                                      const auto name = box->getTextEditorContents ("name").toStdString();
                                      edit ([n, name] (PatcherDoc& doc) {
                                          if (n < (int) doc.nodes.size())
                                              doc.nodes[(std::size_t) n].label = name;
                                      });
                                  }
                              }),
                              true);
    }

    void openNodeEditor (int n)
    {
        const auto* d = currentDoc();
        auto* graph = runningGraph();
        if (d == nullptr || n < 0 || n >= (int) d->nodes.size())
            return;
        const auto& node = d->nodes[(std::size_t) n];
        const auto title = juce::String::fromUTF8 (patcher::nodeName (node).c_str());
        if (graph != nullptr)
        {
            auto effect = graph->effectAt (n);
            auto instrument = graph->instrumentAt (n);
            if (auto* fx = dynamic_cast<PluginEffect*> (effect.get()); fx != nullptr && ! fx->hasCrashed())
            {
                PluginWindows::open (effect, fx->getInstance(), title);
                return;
            }
            if (auto* in = dynamic_cast<PluginInstrument*> (instrument.get()); in != nullptr && ! in->hasCrashed())
            {
                PluginWindows::open (instrument, in->getInstance(), title);
                return;
            }
        }
        // the X-Y / X-Y-Z pad and the Control Surface's panel (round 164) on the node's slot
        if (! node.generator && (xy::isXyController (node.effect.kind) || node.effect.kind == EffectKind::controlSurface))
        {
            SlotLocator slot = [t = target, n] (Song& song) -> EffectSlot* {
                auto* doc = t.doc (song);
                return doc != nullptr && n < (int) doc->nodes.size() && ! doc->nodes[(std::size_t) n].generator ? &doc->nodes[(std::size_t) n].effect : nullptr;
            };
            auto editor = node.effect.kind == EffectKind::controlSurface
                              ? makeControlSurfaceEditor (engine, slot, std::nullopt)
                              : makeXyControllerEditor (engine, slot,
                                                        [effect = graph != nullptr ? graph->effectAt (n) : nullptr] (int k) {
                                                            return effect != nullptr ? effect->controlOutput (k) : 0.0f;
                                                        },
                                                        std::nullopt);
            openToolWindow (std::move (editor), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (! node.generator && VisualWindows::handles (node.effect.kind)) // Wave Candy / Visualizer in a Patcher (round 164c)
        {
            VisualTarget vt;
            vt.engine = &engine;
            vt.locate = [t = target, n] (Song& song) -> EffectSlot* {
                auto* doc = t.doc (song);
                return doc != nullptr && n < (int) doc->nodes.size() && ! doc->nodes[(std::size_t) n].generator
                               && VisualWindows::handles (doc->nodes[(std::size_t) n].effect.kind)
                           ? &doc->nodes[(std::size_t) n].effect
                           : nullptr;
            };
            auto graphIn = [t = target] (AudioEngine& e) -> patcher::Graph* {
                return t.channel ? patcher::graphOf (e.getChannelInstrument (t.index).get()) : patcher::graphOf (e.getEffect (t.index, t.slot).get());
            };
            vt.runningIn = [graphIn, n] (AudioEngine& e) -> std::shared_ptr<Effect> {
                auto* g = graphIn (e);
                return g != nullptr ? g->effectAt (n) : nullptr;
            };
            vt.running = [runningIn = vt.runningIn, &e = engine] { return runningIn (e); };
            VisualWindows::openNode (vt, title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (! node.generator && node.effect.kind != EffectKind::plugin && node.effect.kind != EffectKind::patcher)
        {
            openToolWindow (std::make_unique<NodeParams> (engine, target, n), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        // a MIDI Out / Envelope Controller node: its own editor on the node's settings (round 162)
        auto locate = [t = target, n] (Song& song) -> ChannelDef* {
            auto* doc = t.doc (song);
            return doc != nullptr && n < (int) doc->nodes.size() && doc->nodes[(std::size_t) n].generator ? &doc->nodes[(std::size_t) n].channel : nullptr;
        };
        if (node.generator && node.channel.instrument == InstrumentKind::dashboard) // round 164
        {
            openToolWindow (makeDashboardEditor (engine, locate), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (node.generator && node.channel.instrument == InstrumentKind::voltage) // round 164
        {
            openToolWindow (makeVoltageControllerEditor (engine, locate), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (node.generator && node.channel.instrument == InstrumentKind::midiOut)
        {
            openToolWindow (makeMidiOutEditor (engine, locate), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (node.generator && node.channel.instrument == InstrumentKind::vfx)
        {
            openToolWindow (makeVfxEditor (engine, locate, -1), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        if (node.generator && node.channel.instrument == InstrumentKind::controller && ! parseKeyboardController (node.channel.controllerState))
        {
            openToolWindow (makeEnvelopeControllerEditor (engine, locate), title + " - " + tr ("Patcher"), target.key() + "-node-" + juce::String (n));
            return;
        }
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, tr ("Patcher"),
                                                node.effect.kind == EffectKind::plugin || node.channel.instrument == InstrumentKind::plugin
                                                    ? tr ("This plugin could not be loaded (it is not installed or has not been scanned).")
                                                    : tr ("This node has no editor of its own here."));
    }

    void fit()
    {
        const auto* d = currentDoc();
        if (d == nullptr)
            return;
        auto box = nodeBounds (*d, patchFromHost).getUnion (nodeBounds (*d, patchToHost));
        for (int n = 0; n < (int) d->nodes.size(); ++n)
            box = box.getUnion (nodeBounds (*d, n));
        box = box.expanded (40.0f);
        zoom = std::clamp (std::min ((float) getWidth() / box.getWidth(), (float) (getHeight() - 24) / box.getHeight()), 0.25f, 1.25f);
        offset = box.getCentre() - juce::Point<float> ((float) getWidth(), (float) getHeight() - 24.0f) / (2.0f * zoom);
    }

    // The parameters of a built-in effect node, as sliders
    class NodeParams final : public juce::Component, private juce::Timer
    {
    public:
        NodeParams (AudioEngine& e, Target t, int n) : engine (e), target (t), node (n)
        {
            if (const auto* slot = slotOf (engine.getSong()))
            {
                const auto& info = effectInfo (slot->kind);
                for (int p = 0; p < (int) info.params.size(); ++p)
                {
                    auto row = std::make_unique<Row>();
                    row->label.setText (juce::String::fromUTF8 (info.params[(std::size_t) p].name), juce::dontSendNotification);
                    row->slider.setSliderStyle (juce::Slider::LinearHorizontal);
                    row->slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 80, 18);
                    row->slider.setRange (0.0, 1.0);
                    row->slider.setValue (slot->params[(std::size_t) p], juce::dontSendNotification);
                    row->slider.textFromValueFunction = [kind = slot->kind, p, &song = engine.getSong()] (double v) {
                        return juce::String::fromUTF8 (formatEffectParam (kind, p, (float) v, song.bpm).c_str());
                    };
                    row->slider.updateText();
                    row->slider.onValueChange = [this, p, s = &row->slider] { set (p, (float) s->getValue(), false); };
                    row->slider.onDragEnd = [this, p, s = &row->slider] { set (p, (float) s->getValue(), true); };
                    addAndMakeVisible (row->label);
                    addAndMakeVisible (row->slider);
                    rows.push_back (std::move (row));
                }
            }
            setSize (420, std::max (60, (int) rows.size() * 26 + 12));
            startTimerHz (8);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced (6);
            for (auto& row : rows)
            {
                auto line = r.removeFromTop (26);
                row->label.setBounds (line.removeFromLeft (140));
                row->slider.setBounds (line);
            }
        }

        void paint (juce::Graphics& g) override { g.fillAll (col::bg2); }

    private:
        struct Row
        {
            juce::Label label;
            juce::Slider slider;
        };
        AudioEngine& engine;
        Target target;
        int node;
        std::vector<std::unique_ptr<Row>> rows;

        const EffectSlot* slotOf (const Song& song) const
        {
            const auto* d = target.doc (song);
            return d != nullptr && node < (int) d->nodes.size() && ! d->nodes[(std::size_t) node].generator ? &d->nodes[(std::size_t) node].effect : nullptr;
        }

        void set (int p, float v, bool undoable)
        {
            engine.editSong (
                [t = target, n = node, p, v] (Song& song) {
                    if (auto* d = t.doc (song); d != nullptr && n < (int) d->nodes.size())
                        d->nodes[(std::size_t) n].effect.params[(std::size_t) p] = v;
                },
                undoable);
        }

        void timerCallback() override // (control cables and undo move the values too)
        {
            if (const auto* slot = slotOf (engine.getSong()))
                for (std::size_t p = 0; p < rows.size(); ++p)
                    if (! rows[p]->slider.isMouseButtonDown())
                        rows[p]->slider.setValue (slot->params[p], juce::dontSendNotification);
        }
    };
};

void open (AudioEngine& engine, Target target, const juce::String& title)
{
    auto* window = openToolWindow (std::make_unique<PatcherEditor> (engine, target), title, target.key());
    if (window != nullptr)
    {
        window->setResizable (true, false);
        window->setResizeLimits (480, 320, 2400, 1600);
    }
}
} // namespace

namespace PatcherWindows
{
std::function<void (bool generators)> onBrowsePluginDatabase;

bool takeInstrumentFromBrowser (const ChannelDef& c)
{
    if (pendingBrowserPick == nullptr || ! pendingBrowserPick (&c, nullptr))
        return false;
    pendingBrowserPick = nullptr;
    return true;
}

bool takeEffectFromBrowser (const EffectSlot& fx)
{
    if (pendingBrowserPick == nullptr || ! pendingBrowserPick (nullptr, &fx))
        return false;
    pendingBrowserPick = nullptr;
    return true;
}

void openSlot (AudioEngine& engine, int track, int slot)
{
    const auto& song = engine.getSong();
    const auto name = track >= 0 && track < (int) song.mixer.size() ? juce::String::fromUTF8 (song.mixer[(std::size_t) track].name.c_str()) : juce::String();
    open (engine, { false, track, slot }, tr ("Patcher") + (name.isNotEmpty() ? " - " + name : juce::String()));
}

void openChannel (AudioEngine& engine, int channel)
{
    const auto& song = engine.getSong();
    const auto name = channel >= 0 && channel < (int) song.channels.size() ? juce::String::fromUTF8 (song.channels[(std::size_t) channel].name.c_str())
                                                                            : juce::String();
    open (engine, { true, channel, 0 }, tr ("Patcher") + (name.isNotEmpty() ? " - " + name : juce::String()));
}

bool isChannelOpen (int channel) { return findToolWindow (Target { true, channel, 0 }.key()) != nullptr; }

void closeChannel (int channel)
{
    if (auto* w = findToolWindow (Target { true, channel, 0 }.key()))
        delete w;
}

void shootMenus (AudioEngine& engine, bool channel, int index, int slot, std::function<void (const juce::Image&, const juce::String&)> save)
{
    PatcherEditor editor (engine, Target { channel, index, slot });
    juce::PopupMenu events;
    std::vector<std::pair<juce::String, juce::PopupMenu>> menus;
    menus.emplace_back ("add-plugin", editor.addPluginMenu ({ 100.0f, 100.0f }));
    menus.emplace_back ("from-node", editor.hostMenu (true, &events));
    menus.emplace_back ("from-events", events);
    menus.emplace_back ("to-node", editor.hostMenu (false, &events));
    menus.emplace_back ("to-events", events);
    for (auto& [name, menu] : menus)
    {
        auto& desktop = juce::Desktop::getInstance();
        std::vector<juce::Component*> before;
        for (int k = 0; k < desktop.getNumComponents(); ++k)
            before.push_back (desktop.getComponent (k));
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea ({ 20, 20, 1, 1 }));
        for (int k = 0; k < desktop.getNumComponents(); ++k)
            if (auto* c = desktop.getComponent (k); std::find (before.begin(), before.end(), c) == before.end() && c->getWidth() > 0)
                save (c->createComponentSnapshot (c->getLocalBounds()), name);
        juce::PopupMenu::dismissAllActiveMenus();
    }
}
} // namespace PatcherWindows
} // namespace lynx
