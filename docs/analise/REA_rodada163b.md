# Round 163b (workflow163): clean mixer, insert count, new-project Master chain (+ Browser and pitching sub-forks)

2026-10-26. User feedback by chat (FL user, hardware/recording oriented, Windows, UI zoom 150 %): "a clean/passive mode
where the mixer doesn't have EQs at all? I like to only start my projects with a clipper on the masterbus", "let the
user set the mixer channels, rather than 16 being the default", "customize the browser … a detachable separate
browser for my samples", "pitching didn't work for me and getting to the sample pitching". Evidence markers: [CODE]
read in LYNX's source; [TEST] tests/Workflow163Tests.inc (build-local); [WIN] Windows build.

Sub-forks: Browser customization → REA_rodada163b-browser.md (feature/workflow163b); sample pitching →
REA_rodada163c-pitch.md (feature/workflow163c).

## 1. Clean / passive mixer

| Item | Before [CODE] | Now |
|---|---|---|
| Per-track EQ | FL's 3-band EQ after the slots of every insert; already skipped while all three gains are 0 dB | `Song::cleanMixer` (saved as "cleanMixer" in .lynx): the renderer never runs the EQ (filters reset), the EQ section of the effects panel and "Track EQ..." disappear; the settings are kept, so switching it off restores them. Bit-identical to a mixer without EQ [TEST] |
| Master chain of a new project | LYNX's template puts a limiter (transparent up to −0.3 dB) on the Master | Options › Mixer › "Master chain in new projects": Empty / Soft clipper only / LYNX default (limiter) |
| Switch | — | Options › Mixer › "Clean mixer: no built-in EQ on the inserts (this project)" (also in every mixer track's menu, section "Mixer"); "Clean mixer in new projects" |

Cost: with the switch on, `applyTrackEq` returns before touching the buffers (no biquads, no coefficient updates).

## 2. Insert count

| Item | Now |
|---|---|
| Inserts in new projects | Options › Mixer › "Inserts in new projects (N)": 0…125 (Other... up to 127), default 16. The template's channels routed past the last insert go to the Master. Applies to File › New, New project..., New from template (LYNX's own templates; a user's template stays as saved) and the start-up project |
| More when needed | Already: "+" at the end of the mixer, and since round 162c a channel routed to any insert number (or a new channel when all are used — `Song::claimFreeInsert`) adds the inserts up to it |
| Show only used inserts | Mixer track menu › Mixer / Options › Mixer: inserts nothing uses (`Song::insertInUse`: channel, sends, effects, input, Playlist link, custom name, fader/pan/mute/colour/EQ/output moved, side-chain source, automation) take no room; "+" first brings back the first unused insert, then adds new ones. Persisted (ui-options.txt "mixerUsedOnly") |
| Remove unused inserts at the end | trims the trailing unused inserts of the open project (undoable) |
| Playlist tracks | Options › Mixer › "Playlist tracks shown (N)": the minimum number of Playlist tracks (default 16 as before; always a few free tracks under the last clip) |

.flp import is unchanged (it creates the inserts the project uses, `Song::defaultMixer (highest)`).

## Where I stopped

- Items 1 + 2 done; tests in tests/Workflow163Tests.inc (clean mixer bit-exact vs no EQ, new-project defaults incl.
  0 / 4 / 125 inserts and the three Master chains, ui-options round trip, insertInUse / trimming). build-local with FL
  Data: 1753260 checks, 0 failures. Windows build OK; `--snapshot` of the Mixer with and without the clean mixer
  (D:\lynx-work\workflow163\s2-*-mixer.png): the EQ section (ui163's foldable "EQ" bar included) disappears and the
  slots keep the room. Merged ui163's foldable EQ: a clean mixer has no fold bar at all.
- Items 3 (Browser) and 4 (pitch) landed from the sub-forks (19ab4278, 18e442fa).
- Not done: a Settings-window page for these options (they live in Options › Mixer menus); the clipper choice uses
  FL's Fruity Soft Clipper engine (LYNX's exact reimplementation, available without FL).
