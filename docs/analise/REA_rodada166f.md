# Round 166f: issue #16 (esiowry) — EQs and plugin folders

Items of issue #16 handled here: (1) plugin folders in the Channel Rack / Mixer plugin menus (FL parity),
(2) Parametric EQ 2 spectrum with hot/cold colours that follow the bands (FL parity), (3) Facet EQ: a press near a
bell's centre must move it, not change its range (LYNX-own plugin), (4) Facet EQ comparative EQ (LYNX-own plugin).
The other #16 items (playlist, mixer disarm, piano roll) belong to round 166e.

FL used: FL Studio 2026 (C:\Program Files\Image-Line\FL Studio 2026), black box with a scratch project; REA static on
FL's own `Fruity Parametric EQ 2_x64.dll` (sha256 871b8396…), clean-room, documentation only. Shots:
/mnt/e/lynx-work/eq166f/shots.

## Item 1 — plugin folders in the plugin menus

| | |
|---|---|
| Reporter | "I personally edit their folder structures in FL to move the stock plugins into their own folder, and 3rd party plugins into another. I also create subcategories … and FL then displays them in chunks with subfolder headings in the list." |
| FL evidence | Mixer empty-slot menu (fl-slot-menu-categories.png): "More plugins...", "Plugin database Shift+F8", then one grey section heading per top folder of `Presets\Plugin database\Effects` (Controller, Delay reverb, Distortion, Dynamics, Filter, Flanger, Gain, Misc, Patcher, Visual, user folders sorted A-Z with them), plugins A-Z inside. A subfolder (test `ZZ Test166f/Sub A`) appears as a submenu "Sub A >" at the top of its parent's section, followed by the parent's own plugins. Bottom radio group: Categories (default) / Simple (one flat A-Z list) / Tree (every folder a submenu, fl-slot-menu-tree.png). The Channel Rack "Add" menu (generators) works the same way. The test folder was created in the user's FL database and deleted again. |
| LYNX before | Slot menu = flat list / fixed LYNX categories; no user folders at all. |
| LYNX now | `PluginCatalog::Folders` (core): user folder tree for effects and generators, `/` = subfolder, saved to `plugin-folders.txt`. Browser › Plugin database: folder right-click = New folder / New subfolder / Rename / Delete / "Folders from FL Studio's Plugin database" (imports the user's FL folder layout); plugin right-click = Move to folder / New folder... / No folder. Mixer slot menu (empty slot + "Replace with"), and the Channel Rack add menu: Categories view = a heading per top folder, subfolders as submenus first, plugins A-Z (FL order); Tree view = every folder a submenu; Simple = flat A-Z; the view choice is ticked at the bottom like FL and persisted. Plugins with no user folder keep their usual LYNX list below; a plugin in a folder is listed only under its folder (FL: every plugin sits in exactly one folder). Windows check (fresh test profile with folders `My stock/EQ`, `My stock/Dynamics`): the empty-slot menu shows the MY STOCK heading with Dynamics › and EQ › submenus first, then Fruity Delay 3 (shot mixer-slot-folders.png). |
| Tests | Eq166fTests.inc: folder model (add/rename/remove/move, parse/format round-trip), menuTree order (subfolder first, A-Z), FL import from a fake FL database. |

## Item 2 — Parametric EQ 2 spectrum, hot and cold

| | |
|---|---|
| Reporter | "I look at the frequency spectrum hot and cold levels to guide me with adjusting my EQ, and FL changes their color in tandem with their adjustments. If make a section louder or quieter with a bell, it displays that section hotter or colder respectively." |
| FL evidence | Default PEQ2: grey filled spectrum ("Histogram" enabled, fl-peq2-h1.png). Options menu: Histogram (Enabled ✓, Range, Pivot slope, Frequency precision, Time smoothing, Average mode) and Heatmap (Enabled — off by default, Enhanced frequency, High precision, Heatmap position Top/Bottom/Full, default Full). With Heatmap on every frequency column is lit full height from dark violet through red to orange by its level; cutting band 4 by −5 dB at 632 Hz made that region visibly colder (fl-peq2-h4/h5.png); Top makes the heat fade from the top edge to about the middle (h6). The display is post-EQ, so a boost makes the region hotter and a cut colder. REA [DECOMP]: FUN_006f5b20 builds a THQWAVScope-derived analyser (FUN_006e6090: floor −96 dB, range 96 dB, 1024-point, 256 bins; PEQ2 sets 16.0 smoothing) and loads `Artwork\Spectrum.bmp` (512×1 gradient black→purple→red→orange) as the colour ramp. FL's artwork is not copied; LYNX uses its own ramp. |
| LYNX before | PEQ2 graph showed the curve only (no analyser). |
| LYNX now | `core/SpectrumHeat.h`: LYNX colour ramp (black-violet → red → orange → pale gold), ~1 s power average per column, the level tilted 4.5 dB/octave around 1 kHz (music's slope, like FL's Histogram › Pivot slope) and coloured over 48 dB below the loudest column, so a ±12 dB band moves its region a quarter of the ramp whatever the song's level (a first version on FL's absolute −96…0 dB range left the demo song's mids all dark violet and a +12 dB band barely changed colour: measured on the shots, fixed); position Full/Top/Bottom. PEQ2 editor: post-EQ FFT analyser (4096) drawn as a filled spectrum (on by default, like FL's Histogram) and an optional heatmap (right-click the empty graph › Display › Spectrum / "Heatmap (hot and cold levels)" / Heatmap position Full/Top/Bottom), persisted in `peq2-display.txt`. Because it reads the post-EQ signal, a boosted band turns hotter and a cut colder, exactly as the reporter describes. |
| Tests | Eq166fTests.inc: heat ramp monotonic and cold<hot; boost/cut shift heat; position strength. Shots `--eq166fshot` (one continuous render of the demo song, editor ticked 30×/s): peq2-heatmap-flat/boost/cut/top.png — pixel at 900 Hz above the curve: flat (34,13,46), +12 dB (98,19,51) hotter, −12 dB (7,6,18) colder. |

## Item 3 — Facet EQ: press near the centre moves the bell (LYNX-own)

| | |
|---|---|
| Reporter | "Trying to drag a bell to another position and accidentally clicking too close from its center causes it to adjust the range instead of moving the bell." |
| FL evidence | none needed (Facet EQ is a LYNX plugin). |
| LYNX before | `rangeHandleAt` was tested before `nodeAt` and did not require the band to be dynamic: for the selected static band the range is 0, so the range handle sat exactly on the node centre and grabbed every press there. |
| LYNX now | `FacetEq::pickGraphTarget()` (core, testable): the node always wins inside its radius (11 px); the range handle exists only for dynamic bands and has a small radius (6 px) outside the node. |
| Tests | Eq166fTests.inc: press on the centre / near the centre of a static and a dynamic band → node; press on a dynamic band's handle away from the node → range. |

## Item 4 — Facet EQ comparative EQ (LYNX-own)

| | |
|---|---|
| Reporter | "Add an option to toggle listening for source audio, and another toggle for listening to target audio for comparing EQ profiles." |
| LYNX now | New params Listen (EQ / Source / Target) and Target level match. Source = this track without the EQ, delayed by the EQ's latency so the switch stays in time; Target = the sidechain/target track (picked in the ▾ menu › Target track (sidechain)), level-matched to this track by slow 3 s power averages (±24 dB clamp, toggle). 10 ms crossfades between the three, no clicks. Graph: 3-segment EQ/Source/Target switch top-left, the target's spectrum drawn over this track's (toggle), and "Capture this track and the target together, then Match" to build a match EQ from the two. |
| Tests | Eq166fTests.inc: Listen=Source passes the dry input (latency-aligned), Listen=Target outputs the sidechain level-matched, switching is click-free (max step bound). |

## Verification

- build-local with FL Data: 1,887,415 checks, 0 failures (6 eq166f tests). Windows build OK.
- `LYNX.exe --eq166fshot` (fresh LYNX_PROFILE_DIR, LYNX_TEST_MODE=1): peq2-heatmap-*.png, facet-compare-*.png, looked at.
- Windows GUI (fresh test profile with folders, wingui lock, closed normally, desktop checked clean): empty mixer slot
  menu = MY STOCK heading › Dynamics ›, EQ ›, Fruity Delay 3; the plugins in folders are no longer repeated in the
  list below; Categories ✓ / Simple / Tree at the bottom (mixer-slot-folders.png). Not clicked through on Windows: the
  Browser's folder right-click items and the Channel Rack add menu (same code path, covered by the model tests).
- Coordinator rule (plugin scans): no FL plugin scan or search path was ever touched. Before that rule, one empty test
  folder `ZZ Test166f` (with two stock plugin entries copied in) was created in the user's FL plugin database to see FL's
  subfolder display, and deleted right after. LYNX's FL import only reads FL's database.

## Where I stopped

- Done: all four items, tests, translations (15 languages), shots, notes-0.1.21 bullets, replies-0.1.21 "## #16" draft.
  Next: land via land.sh.
