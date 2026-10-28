// --- The Visualizer's GPU layers (core/VisualFx.h scene) and LYNX Dance (core/FruityDance.h) - round 165e ------------

#include "core/FruityDance.h"

TEST (visual165_shader_layers_round_trip)
{
    using namespace lynx::visualfx::scene;
    // every shader type has effects with unique ids; the 2D types none
    for (int t = 0; t < layerTypeCount; ++t)
    {
        const auto type = (LayerType) t;
        CHECK (isShaderType (type) == (effectCount (type) > 0));
        std::set<std::string> ids;
        for (int e = 0; e < effectCount (type); ++e)
        {
            CHECK (ids.insert (effectId (type, e)).second);
            CHECK (effectIndex (type, effectId (type, e)) == e);
            CHECK (std::string (effectName (type, e)).size() > 2 && std::string (effectHint (type, e)).size() > 2);
            // the Add menu's layer: that effect, saved and read back unchanged
            Scene s;
            s.layers.push_back (shaderLayer (type, e));
            CHECK (s.layers[0].type == type && s.layers[0].effect == e);
            const auto back = parse (write (s));
            CHECK (back && *back == s);
        }
        CHECK (effectIndex (type, "no such effect") == -1);
    }
    CHECK (readsBelow (LayerType::feedback) && readsBelow (LayerType::postprocess) && ! readsBelow (LayerType::canvas));
    // blend modes are saved by id; unknown values keep Normal
    Scene s;
    for (int b = 0; b < blendCount; ++b)
    {
        Layer l;
        l.type = LayerType::bars;
        l.blend = (Blend) b;
        s.layers.push_back (l);
    }
    const auto back = parse (write (s));
    CHECK (back && *back == s);
    const auto odd = parse ("LYNX visualizer 1\nlayer=postprocess\neffect=nonsense\nblend=weird\n");
    CHECK (odd && odd->layers.size() == 1 && odd->layers[0].effect == 0 && odd->layers[0].blend == Blend::normal);
    // a 0.1.19 scene (no effect / blend keys) reads as before
    const auto old = parse ("LYNX visualizer 1\nlayer=bars\ncount=32\n");
    CHECK (old && old->layers[0].blend == Blend::normal && old->layers[0].effect == 0);
    // the built-in GPU scenes: each has a shader layer and starts with a background
    int gpuScenes = 0;
    for (const auto& p : presets())
        gpuScenes += std::any_of (p.layers.begin(), p.layers.end(), [] (const Layer& l) { return isShaderType (l.type); });
    CHECK (gpuScenes >= 6);
}

TEST (visual165_zgameeditor_gpu_mapping)
{
    using namespace lynx::visualfx::scene;
    const std::string ini = "[General]\r\nLayerCount=8\r\n"
                            "[AppSet0]\r\nApp=Background\\SolidColor\r\nFParamValues=0,0.5,1,0.5\r\nEnabled=1\r\n"
                            "[AppSet1]\r\nApp=Canvas effects\\Electric\r\nEnabled=1\r\n"
                            "[AppSet2]\r\nApp=Object Arrays\\CubesGrid\r\nEnabled=1\r\n"
                            "[AppSet3]\r\nApp=Feedback\\WormHoleEclipse\r\nEnabled=1\r\n"
                            "[AppSet4]\r\nApp=Postprocess\\Youlean Bloom\r\nFParamValues=0.4,0.3,0.6,0,0.9\r\nEnabled=1\r\n"
                            "[AppSet5]\r\nApp=Postprocess\\Vignette\r\nFParamValues=0.2,0,0,0,0.7\r\nEnabled=1\r\n"
                            "[AppSet6]\r\nApp=Misc\\Unknown Thing\r\nEnabled=1\r\n"
                            "[AppSet7]\r\nApp=Terrain\\Perturbed Grid\r\nEnabled=0\r\n";
    std::vector<std::string> unmapped;
    const auto s = fromZge ({ ini.begin(), ini.end() }, &unmapped);
    CHECK (s && s->layers.size() == 6);
    if (! s || s->layers.size() != 6)
        return;
    // the solid colour from its parameters (Alpha, Hue, Saturation, Lightness: hue 0.5 = cyan, value 1 - 0.5)
    const auto bg = s->layers[0];
    CHECK (bg.type == LayerType::background && (bg.colourA & 0xffffff) == 0x008080);
    CHECK (s->layers[1].type == LayerType::canvas && s->layers[1].effect == effectIndex (LayerType::canvas, "electric"));
    CHECK (s->layers[2].type == LayerType::object3d && s->layers[2].effect == effectIndex (LayerType::object3d, "cubes"));
    CHECK (s->layers[3].type == LayerType::feedback && s->layers[3].effect == effectIndex (LayerType::feedback, "spin"));
    const auto bloom = s->layers[4];
    CHECK (bloom.type == LayerType::postprocess && bloom.effect == effectIndex (LayerType::postprocess, "bloom"));
    CHECK (std::abs (bloom.amount - 0.4f) < 1.0e-4f && std::abs (bloom.size - 0.6f) < 1.0e-4f && std::abs (bloom.opacity - 0.9f) < 1.0e-4f);
    const auto vignette = s->layers[5];
    CHECK (vignette.effect == effectIndex (LayerType::postprocess, "vignette") && std::abs (vignette.amount - 0.8f) < 1.0e-4f);
    CHECK (unmapped == std::vector<std::string> { "Misc\\Unknown Thing" });

    if (! realFlpDir.empty())
    {
        // FL's presets: how many of their layers now have a counterpart (report), and every scene still draws
        const auto dir = std::filesystem::path (realFlpDir) / "Patches" / "Plugin presets" / "Effects" / "ZGameEditor Visualizer";
        int layers = 0, missing = 0, shader = 0;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator (dir, ec); ! ec && it != std::filesystem::recursive_directory_iterator(); it.increment (ec))
            if (it->path().extension() == ".fst")
            {
                std::ifstream in (it->path(), std::ios::binary);
                const std::vector<std::uint8_t> bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
                std::vector<std::string> left;
                const auto scene = fromZge (bytes, &left);
                CHECK (scene.has_value());
                if (! scene)
                    continue;
                layers += (int) scene->layers.size();
                missing += (int) left.size();
                shader += (int) std::count_if (scene->layers.begin(), scene->layers.end(), [] (const Layer& l) { return isShaderType (l.type); });
            }
        std::printf ("  ZGE presets: %d layers mapped (%d GPU layers), %d without a counterpart\n", layers, shader, missing);
        CHECK (layers == 0 || shader > 100);
    }
}

TEST (dance165_state_keys_and_poses)
{
    using namespace lynx::dance;
    // LYNX's own state round trip
    Settings s;
    s.move = hula;
    s.mirrored = true;
    s.visible = false;
    s.scale = 1.5f;
    s.speed = 2.0f;
    s.x = 900;
    s.y = 640;
    s.colour = 0xff20c0ff;
    const auto back = fromState (toState (s));
    CHECK (back && *back == s);
    CHECK (read (toState (s)) == s);
    CHECK (! fromState ({ 1, 2, 3 }) && read ({}) == Settings {});

    // FL's state: int version, then move, mirrored, shown, scale (/64), speed, x, y (+ display options)
    std::vector<std::uint8_t> fl (40, 0);
    auto put = [&fl] (int at, std::int32_t v) { std::memcpy (fl.data() + at, &v, 4); };
    put (0, 1);
    put (4, jumping);
    put (8, 1);
    put (12, 1);
    put (16, 96);
    put (20, 0);
    put (24, 300);
    put (28, 420);
    const auto f = fromFl (fl);
    CHECK (f && f->move == jumping && f->mirrored && f->visible && std::abs (f->scale - 1.5f) < 1.0e-6f && f->speed == 1.0f && f->x == 300 && f->y == 420);
    put (20, 96);
    CHECK (fromFl (fl) && std::abs (fromFl (fl)->speed - 4.0f) < 1.0e-6f);
    put (20, -96);
    CHECK (fromFl (fl) && std::abs (fromFl (fl)->speed - 0.25f) < 1.0e-6f);
    CHECK (! fromFl (std::vector<std::uint8_t> (39, 0)));
    put (0, 0);
    CHECK (! fromFl (fl));

    // FL's key map (REA_rodada165e §3: the table its TriggerVoice reads)
    CHECK (actionForKey (59).kind == KeyAction::Kind::move && actionForKey (59).move == freestyle && ! actionForKey (59).mirrored);
    CHECK (actionForKey (61).move == stepping && ! actionForKey (61).mirrored);
    CHECK (actionForKey (48).kind == KeyAction::Kind::move && actionForKey (48).move == freestyle && actionForKey (48).mirrored);
    CHECK (actionForKey (46).move == stepping && actionForKey (46).mirrored);
    CHECK (actionForKey (hideKey).kind == KeyAction::Kind::hide && actionForKey (showKey).kind == KeyAction::Kind::show);
    CHECK (actionForKey (100).kind == KeyAction::Kind::none && actionForKey (20).kind == KeyAction::Kind::none);
    CHECK (actionForKey (firstMoveKey + moveCount - 1).move == moveCount - 1);
    CHECK (actionForKey (firstMoveKey + moveCount).kind == KeyAction::Kind::none);

    // poses: deterministic, finite, moving (except the held pose's arms), Freestyle never picks Waiting / Pose
    for (int m = 0; m < moveCount; ++m)
    {
        float travel = 0.0f;
        Pose prev = pose (m, 0.0, 7);
        for (int k = 1; k <= 64; ++k)
        {
            const auto p = pose (m, k / 16.0, 7);
            CHECK (std::isfinite (p.x) && std::isfinite (p.y) && std::isfinite (p.lean) && std::isfinite (p.shoulder[0]) && std::isfinite (p.knee[1]));
            CHECK (std::abs (p.turn) <= 1.0f + 1.0e-6f && p.squash > 0.5f && p.squash <= 1.0f + 1.0e-6f);
            travel += std::abs (p.shoulder[0] - prev.shoulder[0]) + std::abs (p.y - prev.y) + std::abs (p.hipShift - prev.hipShift) + std::abs (p.turn - prev.turn);
            prev = p;
        }
        CHECK (travel > 0.01f);
        const auto a = pose (m, 3.37, 11), b = pose (m, 3.37, 11);
        CHECK (a.shoulder == b.shoulder && a.y == b.y);
    }
    for (std::int64_t i = 0; i < 200; ++i)
    {
        const auto m = freestyleMove (i, 3);
        CHECK (m >= stepping && m <= spin);
    }
    // a mix halfway is between the two
    const auto p0 = pose (held, 0.0), p1 = pose (zombie, 0.0);
    const auto mid = Pose::mix (p0, p1, 0.5f);
    CHECK (std::abs (mid.shoulder[0] - 0.5f * (p0.shoulder[0] + p1.shoulder[0])) < 1.0e-5f);
}

TEST (dance165_silent_channel_signals)
{
    using namespace lynx;
    auto inst = dance::makeInstrument();
    auto* sig = dance::signalsOf (inst.get());
    CHECK (sig != nullptr);
    CHECK (dance::signalsOf (makeInstrument (InstrumentKind::wasp).get()) == nullptr);
    if (sig == nullptr)
        return;
    inst->prepare (48000.0);
    std::uint32_t seen = 0;
    int keys[16] {};
    CHECK (sig->pull (seen, keys, 16) == 0);
    inst->noteOn (60, 0.8f);
    inst->noteOn (53, 0.8f);
    // it makes no sound
    std::vector<float> l (512, 0.0f), r (512, 0.0f);
    inst->render (l.data(), r.data(), 512, 1.0f, 1.0f);
    CHECK (std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));
    CHECK (sig->pull (seen, keys, 16) == 2 && keys[0] == 60 && keys[1] == 53);
    CHECK (sig->pull (seen, keys, 16) == 0);
    // more than the ring holds between two reads: the newest 16
    for (int k = 0; k < 40; ++k)
        inst->noteOn (k, 1.0f);
    CHECK (sig->pull (seen, keys, 16) == 16 && keys[0] == 24 && keys[15] == 39);
    // the made channel of the kind is the dance instrument
    CHECK (dance::signalsOf (makeInstrument (InstrumentKind::fruitydance).get()) != nullptr);
}
