# Round 163b (workflow163b): Browser customization — folders shown, content filter, Sample browser window

2026-10-26. User feedback (FL user, Windows, UI zoom 150 %): "the browser always has a ton of useless folders I don't
care about, I just need my samples/audio/soundfonts … being able to customize the browser so it only shows me what I
want or even better a detachable separate browser for my samples/audio would be a giant improvement", plus a
screenshot of the Browser's folder menu ("DOCUMENTS / Expand all / Collapse all / Refresh / SYSTEM / Open / Copy the
file path / TAGS / Starred / Add folder…": "a ton of wasted space … make this more compact").
Evidence markers: [CODE] LYNX source; [TEST] tests/BrowserView163Tests.inc; [WIN] Windows build.

## What changed

| Item | Before | Now |
|---|---|---|
| Folders shown | [CODE] fixed list: Current project, Plugin database, FL Packs, FL demo projects, FL MIDI, 3x Osc presets, synth presets, Plugin presets, Documents, Music, Desktop, Downloads, user folders | right-click a top-level folder (or Current project / Plugin database) › "Hide "…" from this Browser"; the options button beside the search box (and the folder menu's "Browser" sub-menu, the panel menu) › "Show folders" is a checklist of every entry, "Show all folders" brings everything back. Hidden folders are not searched either |
| Content filter | everything LYNX can use | "Show: All files / Audio files only / Samples and SoundFonts / Presets / Projects and MIDI" — applies to the folder tree and to search results (folders always listed) |
| Sample browser | — | View › "Sample browser (separate window)" (key action `view.sampleBrowser`, also in the Browser's options menu): a second, independent, resizable Browser window with its own folders and filter. First open: only FL Packs, Music, Downloads and the user's folders, filter "Samples and SoundFonts". Drag samples from it onto the Channel Rack / Playlist / Mixer like from the main Browser (the window is its own drag container, drags cross windows); click previews, double-click makes a channel. It reopens at the next start if it was open when LYNX closed, at its last size/position |
| Menus | section titles "SYSTEM", "TAGS" (each ~39 px tall) | no section titles besides the item's name; file system actions in one "File" sub-menu; the view options in a "Browser" sub-menu (the global menu density belongs to round ui163) |

Settings files (settings folder): `browser-view.txt` (main) and `sample-browser-view.txt` (window): `filter=`, `open=`,
`bounds=x,y,w,h`, one `hidden=<path or @project/@plugins>` per hidden entry. Logic in src/core/BrowserView.{h,cpp}
(no JUCE) [TEST]. The search index stays shared and covers every folder (hidden ones too), so switching a folder back
on needs no re-index.

## Where I stopped

- See the bottom of this file (updated at each landing).

### Landing 1 (2026-10-26)
- build-local with FL Data: 1753146 checks, 0 failures (tests/BrowserView163Tests.inc: 2 tests).
- [WIN] Windows build (build-win-workflow163b), GUI checked by hand with screenshots (D:\lynx-work\workflow163b\s1…s11):
  the Sample browser reopened at start from its view file at its saved bounds with Current project / Plugin database
  hidden; options button ▸ "Show: …" / "Show folders" checklist; hiding Documents and FL demo projects (checklist and
  right-click "Hide … from this Browser") saved at once; closing the window wrote open=0; View › Sample browser
  reopened it; a search for "kick" in it listed FL Packs results; a sample dragged from the Sample browser window onto
  the main window's Playlist made an audio clip at the drop point (cross-window drag).
- Not done: a dialog-style "Show folders…" window (a checklist sub-menu instead, one click per folder); the filter does
  not hide folders that contain no matching files (folders are always listed, as in FL).
