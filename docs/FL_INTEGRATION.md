# FL Studio integration (onboarding)

LYNX SHARE reads — never writes — what an FL Studio installation on the same PC already knows, so `.flp` projects open
with their plugins and samples found without a scan. Everything is in `src/engine/FlStudio.*` (Windows side) and
`src/core/FlPluginDb.*` (format parsing and matching, JUCE-free, unit-tested with synthetic fixtures in
`tests/tests.cpp`: `fl_plugin_db_*`, `fl_sample_relocation`). Diagnostics: `LYNX.exe --fldb report.txt [project.flp]`.

## Where FL keeps things [DATA, this PC]

- `HKCU\Software\Image-Line\Shared\Paths`: `Install path` (e.g. `C:\Program Files\Image-Line\FL Studio 2026`),
  `Shared data` (the user data root, e.g. `...\OneDrive\Documentos\Image-Line\` — may live under OneDrive, so
  LYNX SHARE takes it from here before falling back to Documents). The user data folder is `<Shared data>\FL Studio`.
- VST folders FL was configured with: `HKLM|HKCU\Software\Image-Line\Shared\Paths\VST plugins`,
  `HKCU\Software\Image-Line\Shared\Plugins\FL Studio\VstDir64` (both seen holding `C:\Program Files\Common Files\VST2`).
  Further plugin-manager search folders: not confirmed (none found on this PC).
- Plugin database: `<user data>\Presets\Plugin database\Installed\...` — written by FL's plugin manager after a
  scan ("Installed" holds every plugin found; `VerifiedIDs.nfo` is not a plugin). On this PC the folder does not
  exist yet (no third-party plugins scanned), so the layout below comes from documentation and public tools.

## Database entries

- `.nfo`: text, one `key=value` per line. Keys used: `ps_name` (plugin name), `ps_file_vendorname_N`,
  `ps_file_type_N` (1 effect, 2 generator) — [DOC: the public FL Studio Plugin Organizer tool reads exactly these].
  The binary path key is not confirmed: LYNX SHARE accepts any `ps_file_*name*_N` / `ps_file_path_N` whose value is an
  absolute `.dll` / `.vst3` / `.clap` path, and `ps_file_guid_N` (GUID text) / `ps_file_magic_N` (4-char or
  decimal VST2 id) when present. Without a type, the role comes from the `Effects` / `Generators` folder.
- `.fst`: an FL preset (FLhd/FLdt events) whose `0xD5` event holds the Fruity Wrapper record — the same record a
  project stores (REA_rodada5): plugin type, VST3 class ID (`0x34`), CLAP id (`0x3A`), VST2 id (`0x33`), name
  (`0x36`), path (`0x37`). LYNX SHARE reads only those identity records; plugin state is not kept.
- Entries for the same file (`.nfo` + `.fst`) are merged.

## How a project's plugin is found (PluginHost::findForeign)

1. Among the scanned plugins, the same VST3 class ID / CLAP id.
2. The file FL's database lists for it (matched by class ID / CLAP id / VST2 id, then the saved path, then the name
   with architecture suffixes ignored — `Serum_x64` = `Serum`), if that file is a scanned plugin; or the very file
   the project saved.
3. The same name (preferring the format the project used).

When none matches, opening the `.flp` runs a targeted scan of only the candidate files (the database's file, the
project's file, and same-named `.vst3`/`.clap` files in the plugin folders), in child processes like any scan,
then imports. The import report lists every plugin found as `name -> file (how)`.

## Samples (locateFlSample)

The path with `%FLStudioFactoryData%` / `%FLStudioUserData%` / `%FLStudioData%` resolved; if it doesn't exist, the
same file re-rooted from another PC's FL user data (`...\Image-Line\FL Studio\...`), FL install
(`...\Image-Line\FL Studio <version>\...`) or user profile (`X:\Users\<name>\...`) to this PC's; then next to the
project (the file name, then with its last folders: `Samples\kick.wav`). The report counts relocated samples.

## Rules

Read-only: no FL file or registry key is written. Encrypted content (FLEX packs, licensed data) is never opened;
the plugin database holds no licensed content. No Image-Line file is committed (tests use synthetic fixtures).
Test hook: `LYNX_FL_USER_DATA=<folder>` makes LYNX SHARE read a synthetic user data folder instead of FL's (used to check the database path end to end).
