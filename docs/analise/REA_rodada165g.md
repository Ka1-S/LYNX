# REA round 165g — the FL plugins LYNX still lacked, and engines out of "(beta)"

Round r165g "flplugins165" (parent). Evidence markers: [MANUAL] FL's online manual, [DECOMP addr] Ghidra on the
installation's DLL (clean-room: behaviour only), [FL render] FL64.exe /R of a crafted project, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-27 resume after the BSOD: Ghidra projects of every missing plugin's DLL are on D:\lynx-work\flplugins165\ghidra
  (BigClock, Mute2, Scratcher(+engine), Tuner(+engine), Transmitter, VideoPlayer, HtmlNoteBook). Sub-rounds relaunched:
  flbeta165p (Granulizer, Newtime, Newtone), flbeta165s (Harmor); flbeta165k (Kepler) waits for a free slot.
- Batch 1 written (Mute 2, Big Clock, Tuner, HTML NoteBook, inert imports): building + tests, FL GUI shots, then land.
  Next: Scratcher, Transmitter (34 presets, state v3 = ~44 controls 0…10000), Video Player.

## Plugins (FL 2026, Plugins\Fruity)
| FL plugin | kind | LYNX | evidence |
|---|---|---|---|
| Fruity Mute 2 | effect | exact (§1) | [DECOMP], [MANUAL] |
| Fruity Big Clock | effect (no sound) | §2 | [MANUAL] |
| Tuner | effect (no sound) | §3 | [MANUAL] |
| Fruity HTML NoteBook | effect (no sound) | §4 | [MANUAL] |
| Fruity Scratcher | effect (turntable) | (todo) | [MANUAL] |
| Transmitter | effect (transient / sustain split) | (todo) | [MANUAL], presets |
| Fruity Video Player | generator | (todo) | [MANUAL] |
| FL Studio Mobile / Mobile Rack FX | generator / effect | import note (§5) | |
| Razer Chroma, ReWired | | inert on import, with a note | |

## 1. Fruity Mute 2
[DECOMP 006ba850] two parameters: 0 "Mute" — muted = value < 0.5 (labels On / Off); 1 "Channel" — round(value / (1/3)),
clamped to 2: 0 = L, 1 = L + R, 2 = R. [DECOMP 006baaf0] process: not muted → copy; L → left × 0, right × 1;
L + R → both zeroed; R → left × 1, right × 0. No ramp. [DECOMP 006ba2a0] state: int32 version (1), then the two
controls' raw values (int32 each).
[SHOT D:\lynx-work\flplugins165\fl\e13.png] FL 2026's window: two knobs with caption boxes MUTE / CHANNEL and their
values; a new instance shows "On" and "L + R" — so "On" is the sound passing (muted = false indexes label 0 "On"), and
LYNX's default is Mute = 1, Channel = 1/3. [EXEC FL] headless renders (FL64 /R) of projects holding Mute 2 hang at
"Opening: Fruity Mute 2" (screenshot fl\stuck1.png), so the engine is checked by the decompiled logic and unit tests
only. LYNX: EffectKind::fruityMute2, core/FlExtraFx.cpp, layout "Fruity Mute 2" (PluginLayout.cpp), import of the state.

## 2. Fruity Big Clock
[MANUAL] shows the playing time: Bar:Beat:Tick, Minute:Second:1/100 and (optional) Bar:Step:Tick; right-click for
options. LYNX: EffectKind::bigClock (no sound, passes the audio), one parameter "Display"; the editor
(ui/FlExtraEditors.cpp) draws the song position in the biggest monospaced size that fits the resizable window, a click
steps to the next display, right-click picks one. Ticks are shown at FL's default 96 per quarter note. [TEST]
fl165g_big_clock_text.

## 3. Tuner
[MANUAL] Reference 400–480 Hz (440), Reactivity slow … fast (fast: F1–C10, slow: F1–E7, ignores brief fluctuations),
Bass (one octave lower, ≥ 100 ms per estimate), Visual Basic / Dial. [DATA presets Circular|Horizontal\{Default,
Average,Bass}.fst] state = i32 version 1, reference 3000, reactivity (Default 1, Average 0), bass 0/1, visual 1 =
Horizontal (Basic), 2 = Circular (Dial). [HYP] reference raw 0…6000 = 400…480 Hz (3000 = 440, the midpoint),
reactivity −1 … 1. LYNX's detection is its own YIN on the UI thread fed by the slot's tap (the audio thread only copies,
as Wave Candy): [TEST] fl165g_tuner_state_and_detection — B0 (bass) to C8, both reactivities, A4 = 432 Hz, within 2 cents.

## 4. Fruity HTML NoteBook
[MANUAL] shows an .html page chosen with the file icon; the page is not stored in the project. [DATA presets] state =
i32 version 1 + the address as a Pascal short string ("http://www.google.com/", FL news). LYNX keeps the same state,
renders local pages as formatted text (headings, paragraphs, lists, bold / italic, links, preformatted; scripts and
styles skipped) and opens web addresses and external links in the user's browser — LYNX never downloads pages itself
(privacy). Links to other local pages open in place. [TEST] fl165g_html_notebook.

## 5. Inert plugins
Razer Chroma (vendor lighting) and FL Studio Mobile Rack FX are left out of the mixer with a note in the import report;
FL Studio Mobile channels keep LYNX's synth substitute with a note saying why; ReWired and Fruity Video Player channels
(no sound of their own) are imported muted, with the note.

## 6. Left for a next round (groundwork)
- Transmitter [DATA 34 presets, tmp scratch tr.py]: state = i32 version 3 + 47 ints. Ints 1–33 are controls 0…10000
  (Default: 7475, 1968, 1968, 8743, 8743, 5000, 5000, 10000, 9112, 0, 5000, 5051, …), 34–42 flags/selectors (34–38
  0/1 enables: "Transient only" clears 38, "Sustain only" clears 37; 39–40 selectors 0…5; 41–42 0/1), 46 = 49…90,
  47 = 257 / 65793 (bytes). The DSP (transient / sustain split, Saturation / Bounce / Diffusion / Delay per section) needs
  a decompile of Transmitter.dll before LYNX can reproduce it; until then an imported Transmitter is reported as unsupported.
- Fruity Scratcher [DATA presets]: state = version 1001000 + 14 floats + the sample path. engine_x64.dll is only the
  DVS timecode decoder; the scratch DSP is in the 4 MB Delphi DLL. An FL render exists (D:\lynx-work\flplugins165\r\m_ref.wav).
- Fruity Video Player: a video window synced to the song; plan = decode with the user's ffmpeg (ui/VisualEditors.cpp
  findFfmpeg) through a reading pipe, frames shown by song time. Not started.
