// REA round 151: the plugin picker's catalog (categories like FL's Plugin picker, search, favourites)

TEST (rea151_plugin_catalog_categories)
{
    using namespace lynx::plugincatalog;
    const auto fx = [] (const char* name, const char* declared = "") { return Entry { name, name, "Vendor", "VST3", declared, false }; };
    const auto gen = [] (const char* name, const char* declared = "") { return Entry { name, name, "Vendor", "VST3", declared, true }; };
    // the category the plugin declares first (VST3 sub-categories)
    CHECK (categoryOf (fx ("Mystery", "Fx|Reverb")) == "Delay & reverb");
    CHECK (categoryOf (fx ("Mystery", "Fx|Dynamics")) == "Dynamics");
    CHECK (categoryOf (fx ("Mystery", "Fx|EQ")) == "EQ & filter");
    CHECK (categoryOf (fx ("Mystery", "Fx|Analyzer")) == "Analyzer");
    CHECK (categoryOf (gen ("Mystery", "Instrument|Drum")) == "Drum");
    CHECK (categoryOf (gen ("Mystery", "Instrument|Sampler")) == "Sampler");
    // VST2's bare "Effect" / "Synth": the name decides
    CHECK (categoryOf (fx ("ValhallaVintageVerb", "Effect")) == "Delay & reverb");
    CHECK (categoryOf (fx ("CLA-2A Stereo", "Effect")) == "Dynamics");
    CHECK (categoryOf (fx ("Decapitator", "Effect")) == "Distortion");
    CHECK (categoryOf (fx ("Little AlterBoy", "Effect")) == "Pitch & vocal");
    CHECK (categoryOf (fx ("Auto-Tune Pro", "Fx")) == "Pitch & vocal");
    CHECK (categoryOf (fx ("Pro-Q 3")) == "EQ & filter");
    CHECK (categoryOf (fx ("Kirchhoff-EQ")) == "EQ & filter");
    CHECK (categoryOf (fx ("Frequency Shifter")) != "EQ & filter"); // "eq" inside a word is not an EQ
    CHECK (categoryOf (fx ("Doubler2 Stereo")) == "Modulation");
    CHECK (categoryOf (fx ("PanMan")) == "Modulation");
    CHECK (categoryOf (fx ("Unknown thing")) == "Other");
    CHECK (categoryOf (gen ("Serum", "Synth")) == "Other");
    CHECK (categoryOf (gen ("Analog Lab Synth", "Synth")) == "Synth");
    CHECK (categoryOf (gen ("Battery 4 Drum")) == "Drum");
    // every category is one of the picker's tabs
    for (const auto* e : { "Delay & reverb", "Distortion", "Dynamics", "EQ & filter", "Modulation", "Pitch & vocal", "Analyzer", "Utility", "Other" })
        CHECK (std::find (categories (false).begin(), categories (false).end(), e) != categories (false).end());
    CHECK (categories (true).back() == "Other");
}

TEST (rea151_plugin_catalog_search_and_favourites)
{
    using namespace lynx::plugincatalog;
    std::vector<Entry> all {
        { "a", "Zeta Reverb", "Acme", "VST3", "Fx|Reverb", false },
        { "b", "alpha Comp", "Acme", "VST3", "Fx|Dynamics", false },
        { "c", "Beta Delay", "Other Co", "CLAP", "", false },
        { "d", "Gamma Synth", "Acme", "VST3", "Instrument|Synth", true },
        { "e", "alpha Comp", "Acme", "VST", "", false },
    };
    const std::set<std::string> favourites { "a" };
    Filter f;
    // effects only, favourites first, then names A-Z (any case), the same name by format
    auto shown = select (all, favourites, f);
    CHECK (shown.size() == 4u);
    CHECK (shown[0] == 0); // the favourite
    CHECK (shown[1] == 4 && shown[2] == 1 && shown[3] == 2); // "VST" before "VST3"
    f.favouritesFirst = false;
    CHECK (select (all, favourites, f)[0] == 4);
    // every word must match (name, vendor, format or category)
    f.query = "acme comp";
    CHECK (select (all, favourites, f).size() == 2u);
    f.query = "clap";
    CHECK (select (all, favourites, f).size() == 1u && select (all, favourites, f)[0] == 2);
    f.query = "reverb";
    CHECK (select (all, favourites, f).size() == 2u); // Zeta Reverb, and Beta Delay by its category "Delay & reverb"
    f.query.clear();
    f.category = "Dynamics";
    CHECK (select (all, favourites, f).size() == 2u);
    f.category.clear();
    f.favouritesOnly = true;
    CHECK (select (all, favourites, f).size() == 1u);
    f.favouritesOnly = false;
    f.vendor = "Other Co";
    CHECK (select (all, favourites, f).size() == 1u);
    f = {};
    f.instruments = true;
    CHECK (select (all, favourites, f).size() == 1u && select (all, favourites, f)[0] == 3);
    CHECK (vendors (all, false) == std::vector<std::string> ({ "Acme", "Other Co" }));
    CHECK (countIn (all, false, "") == 4 && countIn (all, false, "Dynamics") == 2 && countIn (all, true, "Synth") == 1);
}

TEST (r165c_plugin_catalog_renamed_plugins)
{
    // a plugin renamed in the Plugin database ("1_EQ", FL's quick-pick prefix): the picker sorts and finds it by its new
    // name and still by its original one; its category still comes from the original name
    using namespace lynx::plugincatalog;
    std::vector<Entry> all { { "a", "Zeta EQ", "Acme", "VST3", "Fx|EQ", false, "1_EQ" }, { "b", "Alpha Comp", "Acme", "VST3", "Fx|Dynamics", false } };
    CHECK (all[0].shownName() == "1_EQ");
    CHECK (all[1].shownName() == "Alpha Comp");
    CHECK (categoryOf (all[0]) == "EQ & filter");
    Filter f;
    f.instruments = false;
    auto shown = select (all, {}, f);
    CHECK (shown.size() == 2 && shown[0] == 0); // "1_EQ" before "Alpha Comp"
    f.query = "1_eq";
    CHECK (select (all, {}, f).size() == 1);
    f.query = "zeta";
    CHECK (select (all, {}, f).size() == 1);
}
