// Round 166b (update166b): in-app update check (core/UpdateCheck.h, docs/analise/REA_rodada166b.md)
#include "core/UpdateCheck.h"
#include "core/Sha256.h"

namespace
{
// a trimmed copy of GitHub's /releases answer: a draft, two prereleases, a tag that is no version, escapes
const char* updateFixture = R"json([
  {"url":"x","html_url":"https://github.com/Ka1-S/LYNX-SHARE/releases/tag/v0.1.23-alpha","tag_name":"v0.1.23-alpha",
   "name":"LYNX SHARE 0.1.23-alpha","draft":true,"prerelease":true,"body":"draft","assets":[]},
  {"html_url":"https://github.com/Ka1-S/LYNX-SHARE/releases/tag/v0.1.22-alpha","tag_name":"v0.1.22-alpha",
   "name":"LYNX SHARE 0.1.22-alpha","draft":false,"prerelease":true,"id":12345678,"author":{"login":"Ka1-S","id":1},
   "body":"## New\r\n- **Update** button — see [the docs](https://x/y)\r\n- Café 🌶\r\n\r\n## Fixes\r\n* `crash` fixed\r\n",
   "assets":[
     {"name":"LYNX-0.1.22-alpha-linux-x86_64.tar.gz","size":40000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-linux-x86_64.tar.gz"},
     {"name":"LYNX-0.1.22-alpha-macos-universal.dmg","size":50000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-macos-universal.dmg"},
     {"name":"LYNX-0.1.22-alpha-setup-x64.exe","size":30000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-setup-x64.exe"},
     {"name":"LYNX-0.1.22-alpha-store.msix","size":30000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-store.msix"},
     {"name":"LYNX-0.1.22-alpha-windows-x64.zip","size":30000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-windows-x64.zip"},
     {"name":"LYNX-0.1.22-alpha-x86_64.AppImage","size":60000000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-x86_64.AppImage"},
     {"name":"LYNX-0.1.22-alpha-x86_64.AppImage.zsync","size":200000,"browser_download_url":"https://github.com/d/LYNX-0.1.22-alpha-x86_64.AppImage.zsync"},
     {"name":"LYNX.exe","size":90000000,"browser_download_url":"https://github.com/d/LYNX.exe"},
     {"name":"SHA256SUMS.txt","size":900,"browser_download_url":"https://github.com/d/SHA256SUMS.txt"}]},
  {"html_url":"h","tag_name":"nightly","name":"not a version","draft":false,"prerelease":true,"body":"","assets":[]},
  {"html_url":"https://github.com/Ka1-S/LYNX-SHARE/releases/tag/v0.1.21-alpha","tag_name":"v0.1.21-alpha","name":"LYNX SHARE 0.1.21-alpha",
   "draft":false,"prerelease":true,"body":null,"assets":[]},
  {"html_url":"h","tag_name":"v0.1.20-alpha","draft":false,"prerelease":true,"body":"old","assets":[], "reactions":{"+1":3}, "x":[1,2.5e3,-4,true,false,null]}
])json";
} // namespace

TEST (update166b_versions_compare_like_semver)
{
    using namespace lynx::update;
    const auto v = parseVersion ("v0.1.21-alpha");
    CHECK (v.valid && v.major == 0 && v.minor == 1 && v.patch == 21 && v.pre.size() == 1 && v.pre[0] == "alpha");
    CHECK (v.text() == "0.1.21-alpha");
    CHECK (parseVersion ("0.1").valid && parseVersion ("0.1").patch == 0);
    CHECK (parseVersion (" V1.2.3-rc.1+build.5 ").valid && parseVersion ("V1.2.3-rc.1+build.5").pre.size() == 2);
    for (const char* bad : { "", "nightly", "v1", "1.2.3.4", "1.x.3", "1.2.3-", "1.2.3-a..b", "1.2.3-ä" })
        CHECK (! parseVersion (bad).valid);

    auto lt = [] (const char* a, const char* b) { return compare (parseVersion (a), parseVersion (b)) < 0 && compare (parseVersion (b), parseVersion (a)) > 0; };
    CHECK (lt ("0.1.20", "0.1.21-alpha"));   // a newer prerelease is an update for the running 0.1.20
    CHECK (lt ("0.1.20-alpha", "0.1.20"));   // the release a build's CMake version names is not older than its tag
    CHECK (lt ("0.1.9", "0.1.10"));          // numbers, not text
    CHECK (lt ("0.9.99", "1.0.0"));
    CHECK (lt ("1.0.0-alpha", "1.0.0-alpha.1"));
    CHECK (lt ("1.0.0-alpha.1", "1.0.0-alpha.beta"));
    CHECK (lt ("1.0.0-alpha.beta", "1.0.0-beta"));
    CHECK (lt ("1.0.0-beta.2", "1.0.0-beta.11"));
    CHECK (lt ("1.0.0-rc.1", "1.0.0"));
    CHECK (compare (parseVersion ("v0.1.20"), parseVersion ("0.1.20+build7")) == 0);
}

TEST (update166b_parses_the_release_list_and_picks_the_newest)
{
    using namespace lynx::update;
    std::string error;
    const auto releases = parseReleases (updateFixture, &error);
    CHECK (error.empty());
    CHECK (releases.size() == 4); // "nightly" dropped
    if (releases.size() != 4)
        return;
    CHECK (releases[0].draft && releases[0].version.text() == "0.1.23-alpha");
    const auto& r = releases[1];
    CHECK (r.tag == "v0.1.22-alpha" && r.name == "LYNX SHARE 0.1.22-alpha" && r.prerelease && ! r.draft);
    CHECK (r.page == "https://github.com/Ka1-S/LYNX-SHARE/releases/tag/v0.1.22-alpha");
    CHECK (r.assets.size() == 9 && r.assets[2].size == 30000000 && r.assets[2].url == "https://github.com/d/LYNX-0.1.22-alpha-setup-x64.exe");
    CHECK (r.body.find ("\xE2\x80\x94") != std::string::npos);     // —
    CHECK (r.body.find ("Caf\xC3\xA9 \xF0\x9F\x8C\xB6") != std::string::npos); // é and a surrogate pair
    CHECK (releases[2].body.empty()); // "body": null

    // the draft (0.1.23) is skipped: 0.1.22-alpha is the update for 0.1.20 and 0.1.21
    CHECK (newestIndex (releases, parseVersion ("0.1.20")) == 1);
    CHECK (newestIndex (releases, parseVersion ("0.1.21")) == 1);
    CHECK (newestIndex (releases, parseVersion ("0.1.22")) == -1);   // up to date (the release build of 0.1.22-alpha)
    CHECK (newestIndex (releases, parseVersion ("0.2.0")) == -1);

    // one release object, an error answer, garbage
    const auto one = parseReleases (R"({"tag_name":"v1.0.0","draft":false,"assets":[{"name":"a","browser_download_url":"u"}]})");
    CHECK (one.size() == 1 && one[0].assets.size() == 1);
    error.clear();
    CHECK (parseReleases (R"({"message":"API rate limit exceeded for 1.2.3.4."})", &error).empty() && error.find ("rate limit") != std::string::npos);
    for (const char* bad : { "", "<html>", "[{\"tag_name\":\"v1.0.0\"", "[1,]", "{\"a\" 1}", "\"unterminated" })
    {
        error.clear();
        CHECK (parseReleases (bad, &error).empty() && ! error.empty());
    }
    std::string deep (200, '[');
    CHECK (parseReleases (deep, &error).empty()); // nesting limit, no stack overflow
}

TEST (update166b_flavour_detection_and_method)
{
    using namespace lynx::update;
    Facts f;
    f.os = "windows";
    CHECK (detectFlavour (f) == Flavour::windowsPortable);
    f.uninstallerBesideExe = true;
    CHECK (detectFlavour (f) == Flavour::windowsInstaller);
    f.packaged = true; // the Store package wins (it has no Uninstall.exe anyway)
    CHECK (detectFlavour (f) == Flavour::store);
    CHECK (! checksForUpdates (Flavour::store) && methodFor (Flavour::store) == Method::none);
    f = {};
    f.os = "macos";
    CHECK (detectFlavour (f) == Flavour::macos);
    f.os = "linux";
    CHECK (detectFlavour (f) == Flavour::linuxTarball);
    f.appImagePath = "/home/u/LYNX-0.1.20-alpha-x86_64.AppImage";
    CHECK (detectFlavour (f) == Flavour::linuxAppImage);
    f.forced = "Portable";
    CHECK (detectFlavour (f) == Flavour::windowsPortable);
    f.forced = "nonsense";
    CHECK (detectFlavour (f) == Flavour::linuxAppImage);
    f = {};
    CHECK (detectFlavour (f) == Flavour::unknown && ! checksForUpdates (Flavour::unknown));

    CHECK (methodFor (Flavour::windowsInstaller) == Method::runInstaller);
    CHECK (methodFor (Flavour::windowsPortable) == Method::downloadAndReveal);
    CHECK (methodFor (Flavour::macos) == Method::downloadAndReveal);
    CHECK (methodFor (Flavour::linuxAppImage) == Method::appImageUpdate);
    CHECK (methodFor (Flavour::linuxTarball) == Method::openPage);
    for (auto fl : { Flavour::windowsInstaller, Flavour::windowsPortable, Flavour::store, Flavour::macos, Flavour::linuxAppImage, Flavour::linuxTarball })
        CHECK (flavourFromId (flavourId (fl)) == fl);
}

TEST (update166b_asset_choice_per_flavour)
{
    using namespace lynx::update;
    const auto releases = parseReleases (updateFixture);
    CHECK (releases.size() == 4);
    if (releases.size() != 4)
        return;
    const auto& assets = releases[1].assets;
    auto name = [&] (Flavour f) { const auto i = assetFor (f, assets); return i < 0 ? std::string ("-") : assets[(std::size_t) i].name; };
    CHECK (name (Flavour::windowsInstaller) == "LYNX-0.1.22-alpha-setup-x64.exe");
    CHECK (name (Flavour::windowsPortable) == "LYNX-0.1.22-alpha-windows-x64.zip");
    CHECK (name (Flavour::macos) == "LYNX-0.1.22-alpha-macos-universal.dmg");
    CHECK (name (Flavour::linuxAppImage) == "LYNX-0.1.22-alpha-x86_64.AppImage"); // not the .zsync
    CHECK (name (Flavour::linuxTarball) == "LYNX-0.1.22-alpha-linux-x86_64.tar.gz");
    CHECK (name (Flavour::store) == "-");
    CHECK (checksumsAsset (assets) == 8);
    CHECK (assetFor (Flavour::windowsInstaller, releases[2].assets) == -1 && checksumsAsset (releases[2].assets) == -1);
}

TEST (update166b_checksum_verification)
{
    using namespace lynx::update;
    const std::string data = "LYNX SHARE setup";
    const auto actual = lynx::sha256Hex (reinterpret_cast<const std::uint8_t*> (data.data()), data.size());
    std::string upper = actual;
    for (auto& c : upper)
        c = (char) std::toupper ((unsigned char) c);
    const auto sums = std::string (64, 'a') + "  LYNX-0.1.22-alpha-windows-x64.zip\n" + upper + "  LYNX-0.1.22-alpha-setup-x64.exe\r\n"
                      + std::string (64, 'b') + " *LYNX.exe\nnot a line\n" + std::string (63, 'c') + "  short.txt\n";
    const auto expected = checksumFor (sums, "LYNX-0.1.22-alpha-setup-x64.exe");
    CHECK (expected == actual); // (lower case)
    CHECK (checksumMatches (expected, actual) && checksumMatches (expected, upper));
    CHECK (checksumFor (sums, "LYNX.exe") == std::string (64, 'b')); // binary-mode '*'
    CHECK (checksumFor (sums, "short.txt").empty() && checksumFor (sums, "missing.exe").empty() && checksumFor (sums, "LYNX").empty());
    const std::string tampered = "LYNX SHARE setuP";
    CHECK (! checksumMatches (expected, lynx::sha256Hex (reinterpret_cast<const std::uint8_t*> (tampered.data()), tampered.size())));
    // the streaming hash (files) equals the one-shot one for every length around the block and padding edges
    std::string bytes;
    for (int i = 0; i < 300; ++i)
        bytes += (char) (i * 37 + 11);
    bool same = true;
    for (std::size_t n : { 0, 1, 55, 56, 63, 64, 65, 119, 120, 128, 300 })
    {
        Sha256Stream s1;
        for (std::size_t at = 0; at < n; at += 7) // in odd-sized pieces
            s1.add (bytes.data() + at, std::min<std::size_t> (7, n - at));
        same = same && s1.finish() == lynx::sha256Hex (reinterpret_cast<const std::uint8_t*> (bytes.data()), n);
    }
    CHECK (same);
    Sha256Stream abc;
    abc.add ("abc", 3);
    CHECK (abc.finish() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"); // FIPS 180-2 example
    CHECK (! checksumMatches ("", "") && ! checksumMatches (std::string (64, 'g'), std::string (64, 'g')));
}

TEST (update166b_skip_version_and_settings)
{
    using namespace lynx::update;
    const auto s = parseSettings ("automatic=0\nskipped=0.1.22-alpha\nlastCheck=1761900000000\njunk\n");
    CHECK (! s.automatic && s.skipped == "0.1.22-alpha" && s.lastCheckMs == 1761900000000LL);
    const auto again = parseSettings (formatSettings (s));
    CHECK (again.automatic == s.automatic && again.skipped == s.skipped && again.lastCheckMs == s.lastCheckMs);
    const auto d = parseSettings ("");
    CHECK (d.automatic && d.skipped.empty() && d.lastCheckMs == 0); // on by default
    CHECK (parseSettings ("skipped=nonsense\nlastCheck=-5").skipped.empty() && parseSettings ("lastCheck=x").lastCheckMs == 0);

    Release r;
    r.version = parseVersion ("v0.1.22-alpha");
    Settings skip;
    skip.skipped = "0.1.22-alpha";
    CHECK (! shouldShow (r, skip, false)); // ✕ skipped this one
    CHECK (shouldShow (r, skip, true));    // Help › Check for updates still shows it
    r.version = parseVersion ("v0.1.23-alpha");
    CHECK (shouldShow (r, skip, false));   // a newer one shows again
    skip.skipped = "0.1.24";
    CHECK (! shouldShow (r, skip, false));
    CHECK (shouldShow (r, Settings {}, false));

    CHECK (cacheFresh (1000, 1000 + cacheLifetimeMs - 1) && ! cacheFresh (1000, 1000 + cacheLifetimeMs) && ! cacheFresh (0, 5) && ! cacheFresh (5000, 10));
}

TEST (update166b_release_notes_as_text)
{
    using namespace lynx::update;
    const auto lines = releaseNotes ("# LYNX SHARE 0.1.22\r\n\r\nThe **update**\nbutton: see [the docs](https://x).\n\n\n"
                                     "## New\n- A `bar` ![shot](a.png)\n  continues here\n  - nested <b>bold</b>\n1. first\n\n"
                                     "| Item | Where |\n|---|:---:|\n| Bar | Main window |\n\n---\n```\ncode **stays**\n```\n> quote <https://lynx-share.com>\n\\*not bold\\*\n");
    using K = NoteLine::Kind;
    std::vector<std::pair<K, std::string>> want {
        { K::heading, "LYNX SHARE 0.1.22" }, { K::blank, "" }, { K::text, "The update button: see the docs." }, { K::blank, "" },
        { K::heading, "New" }, { K::bullet, "A bar continues here" }, { K::bullet, "nested bold" }, { K::bullet, "1. first" }, { K::blank, "" },
        { K::text, "Item  \xC2\xB7  Where" }, { K::blank, "" }, { K::text, "Bar  \xC2\xB7  Main window" }, { K::blank, "" },
        { K::text, "code **stays**" }, { K::text, "quote https://lynx-share.com *not bold*" } };
    CHECK (lines.size() == want.size());
    for (std::size_t i = 0; i < lines.size() && i < want.size(); ++i)
    {
        CHECK (lines[i].kind == want[i].first);
        CHECK (lines[i].text == want[i].second);
        if (lines[i].text != want[i].second)
            std::printf ("  line %d: [%s] want [%s]\n", (int) i, lines[i].text.c_str(), want[i].second.c_str());
    }
    CHECK (lines.size() > 6 && lines[4].level == 2 && lines[6].level == 1);
    const auto text = releaseNotesText (lines);
    CHECK (text.find ("\xE2\x80\xA2 A bar") != std::string::npos && text.find ("    \xE2\x80\xA2 nested") != std::string::npos);
    // single-mark emphasis goes, snake_case and a lone star stay
    const auto em = releaseNotes ("*Update* installs it, _really_; snake_case_name, 2 * 3 = 6, **bold**");
    CHECK (em.size() == 1 && em[0].text == "Update installs it, really; snake_case_name, 2 * 3 = 6, bold");
    if (em.size() == 1)
        std::printf ("  em: [%s]\n", em[0].text.c_str());
    CHECK (releaseNotes ("").empty() && releaseNotes ("\n\n\n").empty());
    // a GitHub alert marker is dropped; the quoted text stays; a bare ">" is safe
    const auto alert = releaseNotes ("Intro\n> [!IMPORTANT]\n> Still an alpha.\n>\n> More.");
    CHECK (alert.size() == 3 && alert[0].text == "Intro" && alert[1].text == "Still an alpha." && alert[2].text == "More.");
    CHECK (releaseNotes (">").empty() && releaseNotes ("> [!NOTE]").empty());
    // a line of HTML only (the logo at the top of the release notes) leaves no empty line
    const auto logo = releaseNotes ("<p align=\"center\"><img src=\"x.png\" width=\"96\"></p>\n\n**LYNX SHARE 0.1.20 alpha**: text");
    CHECK (logo.size() == 1 && logo[0].text == "LYNX SHARE 0.1.20 alpha: text");
}
