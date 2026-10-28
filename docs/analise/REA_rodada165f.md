# REA round 165f (rea165): FL Studio 2026's technologies and touches → what LYNX still lacks

2026-10-27. Brief: walk FL Studio 2026 like a power user, use REA/Ghidra on `FLEngine_x64.dll` where behaviour is
not visible, compare with LYNX, rank the gaps (user value, effort, evidence) and build the top ones that rounds
165a–e/g/h/i do not cover. User: "tem que ter todas as tecnologias do FL" — so §1 is a full inventory of FL's
technologies (engine, sync, recording, editing, file, UI), each marked against LYNX.

Evidence tags: **[DFM]** FL's own form/menu resources read from `FLEngine_x64.dll` (our `dfm.py` reader of Delphi
TPF0 resources; 105 forms, 1 397 menu items in the main form alone — no GUI needed), **[DECOMP]** Ghidra
decompilation of `FLEngine_x64.dll` (headless, the analysed project copied to `D:\lynx-work\rea165`), **[SHOT]**
screenshots of FL and LYNX, **[MANUAL]** FL's manual / WhatsNew, **[CODE]** LYNX source.
Clean-room: behaviour and names only, no Image-Line code, art or data copied.

## 1. FL Studio 2026 technology inventory vs LYNX

Source: every form of `FLEngine_x64.dll` read as text **[DFM]** (main form menus, Options window `TMIDIForm`,
mixer `TFXForm`, Channel Rack `TStepSeqForm`, Browser `TSampleListForm`, Edison `TMEnvEditor` + its 20 tool forms,
Piano Roll tool forms `TPR*Form`, render `TRenderForm`/`TWAVRenderForm`, `TShortcutsModule`), checked against LYNX's
source **[CODE]** (menu texts are `tr ("…")`, so a missing text is a strong hint; every ❌ below was confirmed by reading
the nearby code). ✅ has it · ⚠️ partial · ❌ missing · ⛔ out of scope (FL Cloud / AI service / licensed content /
proprietary protocol) · →r165x = another round of 165 owns it.

### 1.1 Audio engine and performance (Options › Audio, plugin CPU menu)
| FL technology | LYNX |
|---|---|
| Multithreaded generator / mixer processing, per-plugin "Allow threaded processing" | ✅ (WorkerPool, RackRenderer; →r165i measures/optimises) |
| Smart disable (per plugin, global) | ✅ (→r165i gaps) |
| Align tick lengths, Safe overloads, Triple buffer, Mix in buffer switch, Use hardware buffer / polling, priority | ⚠️ (driver-level knobs differ by design; →r165i) |
| Playback tracking Driver / Hybrid / Mixer + offset | ❌ (LYNX tracks the mixer position) — L |
| Resampling quality 2-point … 512-point sinc (sampler channels, separate for render) | ✅ (interpolation choice in Settings › Audio and Export) |
| Reset plugins on transport, Play truncated notes on transport / in clips | ❌ — M (to verify: does a note that started before the play position sound in LYNX?) |
| Panning law Circular / Triangular, Fast declick for cut groups, PPQ choice 24…960 | ✅ / ⚠️ / ✅ |
| Plugin delay compensation: automatic, per-track manual latency (ms / samples / beats, Ping), **Bypass track latency compensation** Never / Always / When recording or not playing, Compensate for master latency, Compensate automations | ⚠️ automatic PDC only; ❌ manual per-track latency and the bypass (= FL 24.2's per-track low-latency monitoring) — **M/H** |
| Plugin wrapper: bridging (32/64-bit, separate process), multithreaded processing, "Make editor thumbnail", per-plugin GUI scaling 100…400 % | ✅ sandbox/bridge; ❌ editor thumbnail; ❌ per-plugin scaling — L |
| Threaded project sample loading, keep long audio on disk, fast sample preview | ✅ (→r165i) |

### 1.2 Sync and MIDI (Options › MIDI)
| FL technology | LYNX |
|---|---|
| **Send master sync**: MIDI clock and MTC 24 / 25 / 29.97 / 30 fps (drop, non-drop) to an output, master sync offset (ms) | ❌ — **H** for hardware users |
| **External clock sync** (FL follows an incoming MIDI clock: tempo, start / stop / continue, song position), external sync offset | ❌ — **H** |
| Controller scripts (Python device scripts, "Update MIDI scripts", script information) | ❌ — L/M this version; after r165d's pocketpy lands (next version) |
| Pickup (takeover), Link note-on velocity to Velocity / Mod X / Mod Y + mapping curve, release velocity, foot pedal controls note off, Toggle on release | ✅ pickup; ⚠️ others |
| Dedicated MIDI channels for Generator muting, Omni preview, **Performance mode**, **Song marker jump** | ⚠️ omni preview; ❌ the rest — with §1.5 |
| Send all notes off on init/after render, gamepads as MIDI devices, FL Studio Remote (iOS/Android) | ⚠️ / ❌ L / ⛔ (proprietary app protocol) |

### 1.3 Recording (Recording filter menu, mixer Disk recording menu, metronome menu)
| FL technology | LYNX |
|---|---|
| Recording filter (Automation, Notes, Audio, Clips), Recording starts playback, Disarm on stop, recording markers, quantize note start / end, Record to step sequencer, loop recording into single / multiple patterns | ✅ (except the "Clips" filter, performance mode only) |
| **Score logger: "Dump score log to selected pattern"** (FL always remembers the last minutes of MIDI played, recording or not) | ❌ — **H** (small) |
| Audio logger (26.1) + Maximum length 1/2/5/10 min, Pre effects / Post effects pre fader, "Dump to audio editor" | ⚠️ logger to a file; ❌ the settings and the dump — M |
| Disk recording format 16 / 24 / 32-bit float, Auto-unarm, Auto-create audio clip, Latency compensation, Ignore silent takes, Group loop recorded takes, Apply track level and mute when recording | ✅ mostly (takes, latency) |
| **Render armed / selected mixer tracks to wave file(s)** (mixer menu) | ❌ (per-insert freeze only) — **M** |
| Record source point: External input only / External and mixer input / Post effects / Post EQ / Post level and panning / Post track | ❌ (external input only) — M |
| Metronome sound Hi-hat / Tick / Beep / Cowbell / Pulse, precount 1 / 2 bars, "Only when recording", "Respect recording filter" | ⚠️ one click sound, count-in on/off — **S** |
| One-click audio recording ("I want to record audio…"), Wait for input, count-in, typing keyboard to piano | ✅ |

### 1.4 Song, Playlist and markers
| FL technology | LYNX |
|---|---|
| Arrangements, merge, track modes, groups, Consolidate, Make unique, Chop, Select overlapping / stacked | ✅ |
| **Marker actions**: Start, Loop, Pattern length, Marker loop, Marker skip, Marker pause, Bar override, Time signature, Key/Scale, Start / Stop recording; **Quantize song jumps** Off / Beats / Bars / 2 / 4 / 8 bars; Time markers › Add jump to next bar / Add one every N bars / Add key marker | ⚠️ only time signature + punch in/out act; the other `.flp` actions are kept but **do nothing** — **H** (imported FL songs that loop/skip play wrong) |
| Performance mode (clip launching, track Performance settings: Retrigger, Hold & stop, Latch, March & wrap, Random…, trigger / position sync) | ❌ — L effort-wise (large); next version |
| Picker panel (patterns, audio, automations; sort, select unused) | ✅ |
| Playlist track: Lock to content, Send pattern instances here, Merge pattern clips, Mute / unmute all clips, size 33…200 %, Lock to this size | ⚠️ (merge / send / lock missing) — L |
| Audio clip: Chop into regions, auto-slicing, stutter pattern, Make unique as sample, Crossfade with, fade curves, Set pitch, Reverse | ✅ mostly; ❌ stutter pattern editor, make unique as sample — L |
| Extract stems from sample, Deverb (26.1 AI) | ⛔ (models; separate decision, see r153) |
| Automation clip curve modes (Single / Double curve 1–3, Half sine, Stairs, Smooth stairs, Pulse, Wave), Auto smoothing, Relocate events | ✅ |
| Time display Bar:Beat:Tick / Min:Sec:Centisecond / **SMPTE 23.976 … 72 fps** | ⚠️ bars + min:sec — **S** |

### 1.5 Channel Rack, patterns, Piano Roll
| FL technology | LYNX |
|---|---|
| Fill each 2/4/8 steps, Advanced fill, Rotate, Burn MIDI to pattern (current / new), Receive notes from, MIDI channel through | ✅ fill/rotate; ❌ **Burn MIDI** (arpeggiator / echo output → notes), ❌ Receive notes from — M |
| Channel Rack loop types: Loop step channels / Loop all channels / Advanced looping, per-channel **loop point** (step / beat / bar), "Burn to pattern" (FL 2025) | ❌ — M (polyrhythmic per-channel loops) |
| Loop Starter (25.x: genre, generate steps, replace samples) | ⛔ content from FL Cloud; a local "generate steps" variant possible — L |
| Create DirectWave instrument (auto-samples a plugin into a multisample) | ❌ — M |
| Patcherize / Patcherize all (channel or effect chain → Patcher) | ❌ — M (Patcher being changed by r165c → next version) |
| Spare state (store / flip) for plugins | ⚠️ mixer slots A/B only — L |
| Piano Roll tools (quantize, arp, chord, fill, flam, flip, key limit, legato, level scale, LFO, random, slice, strum, triplet, riff machine), note colours, groups, key labels, scale snap, ghost notes | ✅ (r153) |
| Piano Roll Python scripts ("Run last script again", script folder) | ❌ — after r165d's pocketpy (next version) |
| Time markers in the Piano Roll (pattern-level), Snap Main / Events / Markers | ❌ / ⚠️ — L |
| Export as score sheet (PDF notation), MIDI clipboard copy / paste | ❌ L / ❌ S |
| Note names English / Germanic / **Solfège (dó, ré, mi)**, sharps / flats | ⚠️ sharps / flats in places — **S** (Brazilian users read solfège) |

### 1.6 Mixer
| FL technology | LYNX |
|---|---|
| Routing / sidechain to selected (only), Create submix, select routed, unroute, groups, docks, track EQ, states | ✅ except **Create submix** ❌ and "Sidechain this track to selected" ⚠️ — S |
| Gradient color, Trim unused tracks, Assign to new audio track(s), view options (waveforms, routing cables, colourful mixer) | ⚠️ — L |

### 1.7 Audio editor (Edison: `TMEnvEditor` + tool forms)
| FL technology | LYNX |
|---|---|
| Amp, normalize, fades, reverse, DC, resample, regions, auto-slice, spectrogram, record | ✅ |
| Tools: EQ, Reverb, Blur, Claw, Decimate, Denoise (+ vocal denoise), Drum(loop) stretcher, Envelope filter, Smooth, Loop tool (seamless loop), Stretch, Events → automation, Scale | ❌ most — M (an "Edison tools" round of its own) |

### 1.8 Files, project, UI, misc
| FL technology | LYNX |
|---|---|
| Templates, project info (title, author, genre, comments, **working time**), data folder, Copy used samples to the project folder, backups, undo history options (undo knob tweaks, alternate undo) | ✅ / ⚠️ working time ❌ / ✅ / ✅ / ⚠️ |
| Video export, ZGameEditor visualizer | →r165e |
| Themes / theme editor, GUI / pop-up / toolbar scaling, animations, hint bar | ⚠️ (one LYNX theme by design; scaling ✅) |
| Browser: tags, plugin tags, indexing, extra folders, FL Cloud search, history, "Send next/previous to selected channel" | ⚠️ — **M** (tags; send next/previous ✅?) |
| External tools (launch an audio editor / app with the file) | ❌ — L |
| FL Cloud (sounds, mastering, backups, plugins), Gopher, AI Deverb / Declipper / Denoiser, stem separation, Speech synthesizer form | ⛔ |

### 1.9 Screens: every FL form group, LYNX's screen, and whether they were ever compared side by side
FL's 105 forms (`tmp/rea165/forms.txt`) grouped by window; LYNX's screen per `UI_windows_inventory.md` (round 81)
updated with later rounds; "Compared" = a round that put FL's and LYNX's screen next to each other ([SHOT]/[EXEC]
captures), "never" = no side-by-side so far. Non-user forms (base classes, docking, tutorial, test, licensing / shop /
cloud) are left out as in round 81.

| FL form(s) | LYNX screen | Compared side by side |
|---|---|---|
| TFruityLoopsMainForm (main window, menus, toolbar) | MainComponent, toolbar | UI_STUDY_round90, UI_toolbar_study, 115, 143 (menus) |
| TStepSeqForm (Channel Rack) | ChannelRackComponent | 104, 139, 143, 149 |
| Piano roll (main form code) + TPRNotePropForm | PianoRollComponent, note inspector | UI_STUDY_round90, 115, 143 A.1 (Tools menu), 149 (lane); note properties: never |
| Playlist (main form code) + TPLClipPropForm, TPLMergeArrangementForm | PlaylistComponent, Clip properties, Arrangements window | Playlist: 90, 104, UI_STUDY_playlist_clips, 143 A.2–A5; Clip properties / Merge arrangement: **never** |
| TFXForm (Mixer) | MixerComponent | 90, 104, 139, 143 A.6, 146, 149 |
| TBrowserForm, TSampleListForm (Browser) | BrowserComponent | 115, 139, 143 B3–B5 |
| TPluginForm (channel / plugin window, wrapper, Misc tab) | PluginWindow + PluginHeader, ChannelSettings | 139, 143 B1–B2, 149 |
| native plugin windows (plugin DLLs) | LYNX's own editors | UI_STUDY_plugins93, 114 (third-party), plugin rounds |
| TFLHintBarForm (hint bar) | HintBar | 115, 143 |
| TMIDIForm (Options: Audio, MIDI, General, File, Project, Info) | Settings pages | 143 B6–B8 (Audio, General, File); **MIDI, Project, Info: never** |
| TRenderForm, TWAVRenderForm, TMEMP3OptionsForm, TMEOggOptionsForm (export) | Export window | 139 B1 (export entries/menus only); **the export window itself: never** |
| TMIDIImportForm | Import MIDI data | 149 Part B |
| TEventEditForm, TGraphEditorForm | Event editor, automation editor | **never** |
| TPRQuantizeForm … TPRChordToolForm, TPRFillForm (15 Piano Roll tool dialogs) | NoteToolDialogs | **never** as dialogs (only the Tools menu, 143 A.1) |
| TMEnvEditor + 20 TME* tool forms (Edison) | AudioEditor (basic tools) | **never** |
| TTouchKeybForm | Touch keyboard | **never** |
| TNewProjForm, TInfoForm, TProjectRenameForm | New project dialog, Settings › Info; rename: none | **never** |
| TShortcutForm, TShortcutEditorForm | Shortcut manager | **never** |
| TPluginListForm, TPlugListForm, TMissingPluginsForm | Plugin manager / quick-add palette / plugin picker, Missing files | picker: 149; **plugin manager, missing files: never** |
| TPluginMonitorForm | Plugin performance monitor | **never** |
| TTapTempoForm, TAddMarkerSettingsForm, TPRTimeSigForm | tap tempo, marker menus | **never** (markers: rea165-markers will) |
| TParamCtrlForm, TMIDIInputForm, TMIDIInputForm_Generic, TEnvMapForm | link to controller, global link settings; mapping curve: none | **never** |
| TNameEditForm, TPaletteEditorForm, TThemeEditorForm | Rename and color; palette / theme: none (one LYNX theme) | **never** |
| TWelcomeWizard | Setup wizard | 149 / 161b (LYNX only); FL's: **never** |
| TExportScoreForm, TDWPRenderForm, TSpeechForm, TDemucsForm, TDeverbForm, TPythonForm, TScriptDialog | none (gaps §1.5 / ⛔) | — |

Never compared, by how often a user sees them: the **Export window**, **Piano Roll tool dialogs**, **Edison**,
**Settings › MIDI / Project / Info**, Event editor, Clip properties, New project, Shortcut manager, Touch keyboard,
Plugin manager, Missing files, Plugin monitor, Merge arrangement, tap tempo / marker dialogs. A design-fidelity round
should take them in that order (off-screen `--snapshot` of LYNX + one short FL session under the locks).

## 2. Ranked gaps (not owned by r165a–e/g/h/i)

Score = user value × (1 / effort). Evidence: [DFM] for all; [DECOMP] added where the behaviour was decompiled.

| # | Gap | Value | Effort | Plan |
|---|---|---|---|---|
| 1 | Song marker actions (loop, marker loop, skip, pause, start, pattern length, key) + Quantize song jumps + Time markers menu + MIDI song-marker jump | H (imported FL songs play wrong today) | M | sub-round **rea165-markers** |
| 2 | MIDI sync: send master sync (MIDI clock, MTC) + external clock sync (follow tempo / start / stop / continue / SPP), offsets | H (hardware users) | M | sub-round **rea165-midisync** |
| 3 | Score logger + "Dump score log to selected pattern" | H | S | lead |
| 4 | Metronome sounds (LYNX's own synthesized Hi-hat / Tick / Beep / Cowbell / Pulse), precount 1 / 2 bars, "Only when recording" | M | S | lead |
| 5 | Time display modes: Min:Sec:Centisecond, SMPTE fps list | M | S | lead |
| 6 | Note names: English / Germanic / Solfège, sharps / flats | M (pt-BR users) | S | lead |
| 7 | Mixer: per-track manual latency + Bypass track latency compensation (low-latency monitoring), Compensate for master latency | M/H | M | sub-round after 1/2 (**rea165-latency**) |
| 8 | Mixer: Render armed / selected tracks to wave files; Create submix | M | S/M | after 7 |
| 9 | Burn MIDI to pattern (arp / echo baked); Channel Rack per-channel loop points | M | M | next |
| 10 | Browser tags | M | M | next version |
| 11 | Edison processing tools (EQ, reverb, denoise, drum stretch, loop tool, blur, claw…) | M | L | next version (own round) |
| 12 | Patcherize, Performance mode, Piano Roll / controller Python scripts, Create DirectWave instrument, score-sheet export | M | L | next version (12: after r165c / r165d land) |
| — | Stem separation / AI tools, FL Cloud, FL Studio Remote, Gopher | ⛔ | | decision / out of scope |

### 2.1 Plan after the user's "everything in this version" (2026-10-27 16:40)
Edison tools → r165l, Performance mode + Patcherize → r165m. Everything else of §2 is r165f's for 0.1.20, two
sub-rounds at a time (LOAD LIMIT rules):
| Sub-round | Gaps |
|---|---|
| rea165-markers (running) | #1 marker actions, Quantize song jumps, time-marker menu, MIDI marker jump |
| rea165-midisync (running) | #2 MIDI clock / MTC out, external clock in, all-notes-off |
| rea165-mixer (next) | #7 per-track manual latency, Bypass track latency compensation, Compensate for master latency; #8 Render armed / selected tracks to wave files, Create submix; record source point |
| rea165-rack (next) | #9 Burn MIDI to pattern, Channel Rack per-channel loop points (Advanced looping), Receive notes from; Create DirectWave instrument (auto-sampler) |
| rea165-browser (then) | #10 Browser tags (+ plugin tags); Playlist track leftovers (Lock to content, Send pattern instances here, Merge pattern clips); MIDI clipboard; score-sheet export |
| rea165-scripts (after r165d's pocketpy lands) | Piano Roll Python scripts, MIDI controller scripts |
| lead | #3 score logger, #4 metronome sounds, #5 SMPTE time display, #6 note names — done, building |

## 3. Built by the lead

### 3.1 Score logger [DFM] [DECOMP]
FL: Tools › "Dump score log to selected pattern" › Last 2 / 5 / 10 / 20 / 30 minutes (actions
`ToolsDumpScoreLog{2,5,10,20,30}MinutesAction`, tag = minutes) and "Clear log" (`ToolsClearScoreLogAction`, a confirm
"This will clear all logged notes!"). The dump is `FUN_0124c060` (called with tag × 60 seconds):
- the log is a note list (start and length in milliseconds of wall time, the rack channel of each note); empty →
  "The score log is empty. That means you haven't input any notes yet.";
- a copy is converted with `FUN_01311fb0` = ms × tempo × PPQ / 60000 (rounded), start and end separately; notes still
  held (length 0x7FFFFFF0) are dropped; lengths below the minimum (`DAT_015d89cc` = ticks per step / 6) are raised;
- the window reaches back from the end of the last note; older notes are dropped;
- the first note moves to tick 0 and every silence longer than **two bars** (`DAT_015d8914` = ticks per bar, set
  by `FUN_013111e0` from PPQ / steps per beat / beats per bar) is shortened to two bars;
- undo step "score log dump"; the selected pattern: empty → the notes go in; otherwise "Merge" / "Replace" / "Cancel".
LYNX (`core/ScoreLog.{h,cpp}`, `AudioEngine::logPlayedNote`, Tools menu): the same rules. Notes are logged where
they are sent to the channels (MIDI inputs after the scale snap and target selection; the typing keyboard and the
touch keyboard), in a fixed 65 536-note ring under a spin lock held for a few instructions, on the MIDI / UI thread
only — never on the audio thread. One difference: FL's gap pass also walks the dropped (older / held) notes, so a
dump can start up to two bars late; LYNX applies the rule to the kept notes only (the first kept note at tick 0).

### 3.2 Metronome sounds [DFM]
FL's metronome menu: Hi-hat, Tick, Beep, Cowbell, Pulse. LYNX (metronome button right-click › Metronome sound):
Click (LYNX's previous sound, unchanged), Hi-hat (double high-passed noise), Tick (12 ms bright sine), Beep (80 ms
sine), Cowbell (two square partials a ×1.44 apart), Pulse (raised-cosine bump) — LYNX's own synthesis, the downbeat
higher (or louder for the hi-hat). Saved in the UI settings (`metronomeSound`). Cost: one switch per sample while a
click sounds, nothing otherwise. Count-in 1–4 bars already existed (Record menu).

### 3.3 SMPTE time display [DFM]
FL's time display modes: Bar : Beat/Step : Tick, Minute : Second : Centisecond, SMPTE 23.976 / 24 / 25 / 29.97 (drop,
non-drop) / 30 / 48 / 50 / 59.94 (drop, non-drop) / 60 / 72 fps. LYNX's toolbar time display: a click cycles bars →
minutes → SMPTE; the right-click menu picks the mode and the rate (`core/Timecode.h`: drop frame skips frames 0–1
(0–3 at 59.94) at every minute but each tenth, so one real hour reads 01:00:00;00; non-drop 29.97 reads 00:59:56:12).

### 3.4 Note names [DFM]
FL's Settings › General › Note names (English / Germanic / Solfege) and Black notes (sharps / flats). LYNX: Settings ›
General, the same two choices (`core/NoteNames.h`): Germanic writes H for B and B for B♭; Solfège Do Re Mi Fa Sol La
Si. Used by the Piano Roll keys and notes, its scale list and chord readout, the note tools, the touch keyboard and
the channel's root-note keyboard. Octaves stay FL's (key 60 = C5 / Do5).

## Where I stopped
- PAUSED by the coordinator (machine load 33). Audit done: §1 inventory ([DFM] of all 105 FLEngine forms, tmp/rea165/menus.txt),
  §2 ranking. Sub-rounds launched and paused: rea165-markers (gap 1), rea165-midisync (gap 2) in their own worktrees.
- Lead, gap 3 (score logger) in progress: [DECOMP] FUN_0124c060 (dump: ms→ticks at the project tempo, held notes dropped,
  window back from the last note end, first note at tick 0, silences > 2 bars shortened to 2 bars, min length = step/6,
  Merge / Replace / Cancel when the pattern has notes, menu Last 2/5/10/20/30 minutes + Clear log). Written:
  src/core/ScoreLog.{h,cpp} (in build-local + CMake), not built yet. Next: engine hook (AudioEngine MIDI push lambda +
  sendLiveNote record=true, SpinLock-guarded Log), Tools menu, tests; then gaps 4–6.
