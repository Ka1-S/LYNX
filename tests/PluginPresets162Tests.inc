// Round 162d (issue #8): presets of hosted plugins — LYNX's own preset file and VST2's .fxp / .fxb.
#include "core/PluginPresets.h"

TEST (plugin_preset_text_round_trip_round162)
{
    namespace pp = lynx::pluginpresets;
    pp::Preset p;
    p.name = "Warm pad\nsecond line";
    p.pluginName = "Surge XT";
    p.pluginId = "VST3-Surge XT-1a2b3c4d-5e6f7a8b";
    p.format = "VST3";
    for (int i = 0; i < 300; ++i)
        p.state.push_back ((std::uint8_t) (i * 7));
    const auto text = pp::toText (p);
    CHECK (text.rfind ("LYNX plugin preset 1\n", 0) == 0);
    const auto back = pp::fromText (text);
    CHECK (back.has_value());
    CHECK (back->name == "Warm pad second line"); // one line per value
    CHECK (back->pluginName == p.pluginName && back->pluginId == p.pluginId && back->format == "VST3");
    CHECK (back->state == p.state);
    // a preset of one of LYNX's own engines is not a plugin preset (same extension, other first line)
    CHECK (! pp::fromText ("LYNX preset 1\nengine = Facet EQ\nname = x\n").has_value());
    CHECK (! pp::fromText ("LYNX plugin preset 1\nname = no state\n").has_value());
}

TEST (plugin_preset_folder_names_round162)
{
    namespace pp = lynx::pluginpresets;
    CHECK (pp::folderNameFor ("Surge XT") == "Surge XT");
    CHECK (pp::folderNameFor ("A/B: \"x\"?") == "A_B_ _x__");
    CHECK (pp::folderNameFor ("  ...  ") == "Plugin");
    CHECK (pp::folderNameFor ("name.") == "name");
    CHECK (pp::folderNameFor (std::string (100, 'x')).size() == 64);
}

TEST (vst2_fxp_fxb_round_trip_round162)
{
    namespace pp = lynx::pluginpresets;
    using K = pp::FxFile::Kind;
    // a parameter program, byte for byte as the VST 2.4 layout says
    pp::FxFile prog;
    prog.kind = K::paramProgram;
    prog.pluginId = pp::fourCC ("Abcd");
    prog.pluginVersion = 3;
    prog.programs.push_back ({ "Init", { 0.0f, 0.25f, 1.0f } });
    const auto bytes = pp::writeFx (prog);
    CHECK (bytes.size() == 56 + 3 * 4);
    CHECK (bytes[0] == 'C' && bytes[1] == 'c' && bytes[2] == 'n' && bytes[3] == 'K');
    CHECK (bytes[7] == 56 + 12 - 8); // byteSize: everything after it
    CHECK (bytes[8] == 'F' && bytes[9] == 'x' && bytes[10] == 'C' && bytes[11] == 'k');
    CHECK (bytes[16] == 'A' && bytes[19] == 'd');
    const auto back = pp::parseFx (bytes);
    CHECK (back.has_value() && back->kind == K::paramProgram && back->pluginId == prog.pluginId && back->pluginVersion == 3);
    CHECK (back->programs.size() == 1 && back->programs[0].name == "Init" && back->programs[0].params == prog.programs[0].params);

    // an opaque chunk program
    pp::FxFile chunk;
    chunk.kind = K::chunkProgram;
    chunk.pluginId = 42;
    chunk.programName = "A very long program name that is cut";
    chunk.chunk = { 1, 2, 3, 4, 5 };
    const auto c = pp::parseFx (pp::writeFx (chunk));
    CHECK (c.has_value() && c->kind == K::chunkProgram && c->chunk == chunk.chunk && c->programName.size() == 27);

    // banks: parameters and chunk
    pp::FxFile bank;
    bank.kind = K::paramBank;
    bank.pluginId = 7;
    bank.currentProgram = 1;
    bank.programs = { { "One", { 0.1f } }, { "Two", { 0.9f } } };
    const auto b = pp::parseFx (pp::writeFx (bank));
    CHECK (b.has_value() && b->isBank() && ! b->isChunk() && b->programs.size() == 2 && b->programs[1].name == "Two");
    CHECK (b->currentProgram == 1 && b->pluginId == 7 && b->programs[1].params[0] == 0.9f);
    pp::FxFile cb;
    cb.kind = K::chunkBank;
    cb.bankPrograms = 128;
    cb.chunk.assign (1000, 9);
    const auto cbb = pp::parseFx (pp::writeFx (cb));
    CHECK (cbb.has_value() && cbb->kind == K::chunkBank && cbb->chunk.size() == 1000 && cbb->bankPrograms == 128);

    // damaged files are refused, never read past their end
    auto cut = pp::writeFx (bank);
    cut.resize (cut.size() - 3);
    CHECK (! pp::parseFx (cut).has_value());
    CHECK (! pp::parseFx ({ 'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E' }).has_value());
    CHECK (! pp::parseFx ({}).has_value());
}
