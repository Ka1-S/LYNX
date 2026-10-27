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

## Where I stopped
- 16:10 — resumed after the BSOD (the first agent had written nothing). DFM inventory extracted
  (`tmp/rea165/menus.txt`, `forms.txt`); first comparison against LYNX's code done (§1). Next: rank (§2), REA the
  marker actions and MIDI sync, start the builds.
