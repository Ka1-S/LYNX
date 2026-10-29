# REA round 165g — the FL plugins LYNX still lacked, and engines out of "(beta)"

Round r165g "flplugins165" (parent). Evidence markers: [MANUAL] FL's online manual, [DECOMP addr] Ghidra on the
installation's DLL (clean-room: behaviour only), [FL render] FL64.exe /R of a crafted project, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-28 17:20: Kepler landed b06e2c59, Granulizer engine landed 471b4f23 (both out of beta). Video Player:
  build-local 1 781 828 checks, 0 failures; Windows --videoshot with the user's ffmpeg 8.1 [SHOT]: frames at song
  0 / 1.5 / 3.25 s (delay 0.5 s) = ffmpeg's own frames at video 0 (held before the delay) / 1.0 / 2.75 s; landing.
  Harmor's presets run as sub-round flbeta165h. Left: Transmitter port (§6), Kepler Exo, Newtime /
  Newtone. (Scratcher: landed by sub-round flscratch165, §6.)
- 2026-10-28 12:50 (resume after the 2nd BSOD): batch 2 for 0.1.20, one sub-round at a time. (1) Kepler
  (feature/flbeta165k, out of beta, verified in REA_rodada165g-flbeta165k.md) merged with main, build-local running;
  (2) Granulizer engine (feature/flgran165, out of beta, REA_rodada165g-flgran165.md) merged with main (pt-BR conflict:
  both kept), build-local queued; (5) Fruity Video Player written in feature/flvideo165 (section 7): core + editor +
  import + tests, not built yet. Next: land Kepler and Granulizer, build/test/shot the Video Player, then Harmor's
  presets, Transmitter / Scratcher.
- Earlier: Ghidra projects of every missing plugin's DLL are on D:\lynx-work\flplugins165\ghidra (BigClock, Mute2,
  Scratcher(+engine), Tuner(+engine), Transmitter, VideoPlayer, HtmlNoteBook). Batch 1 landed 0162e93a (Mute 2, Big
  Clock, Tuner, HTML NoteBook, inert imports); Harmor engine 1e2ecf26 and Granulizer's auto slicer ea87cd3c landed
  (still beta).

## Plugins (FL 2026, Plugins\Fruity)
| FL plugin | kind | LYNX | evidence |
|---|---|---|---|
| Fruity Mute 2 | effect | exact (§1) | [DECOMP], [MANUAL] |
| Fruity Big Clock | effect (no sound) | §2 | [MANUAL] |
| Tuner | effect (no sound) | §3 | [MANUAL] |
| Fruity HTML NoteBook | effect (no sound) | §4 | [MANUAL] |
| Fruity Scratcher | effect (turntable) | FL's engine, state, sample, pattern events, editor (REA_rodada165g-flscratch165.md); DVS timecode not reproduced | [DECOMP], [FL render] |
| Transmitter | effect (transient / sustain split) | (todo) | [MANUAL], presets |
| Fruity Video Player | generator | §7 (video via the user's ffmpeg, sound track, FL's state) | [DECOMP] |
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
FL Studio Mobile channels keep LYNX's synth substitute with a note saying why; ReWired channels (no sound
of their own) are imported muted, with the note (Fruity Video Player: §7 since batch 2).

## 6. Left for a next round (groundwork)
- Transmitter, round 165g batch 2 [DECOMP Transmitter_x64.dll]: CreatePlugInstance 0x7145c0 builds the class at VMT
  0x714360 (slot 3 SaveRestoreState 0x7157e0, slot 7 Eff_Render 0x717600). The render hands the block to a separate
  engine object (+0x154): its vtable +0x30 processes (n, in, out, the transient / sustain envelope buffers +0x134 /
  +0x13c), +0x48 returns the two chains' buffers; the sidechain input (+0x230) and the two envelope sends (+0x234 /
  +0x238) are mixer tracks read / written through the host (+0x120 / +0x128). The editor's 64 controls (DFM hints):
  Sensitivity, Lookahead, Stereo link, Hold / Release times, Detection mode and sidechain track, Transient / Sustain
  level, amplitude, tilt, and per section Saturation (drive, warmth, mix), Diffuser (delay, depth, mix), Bounce (delay
  curve, amp curve, mix) and Delay (time ×2 with tempo-based time, feedback, mix). A faithful port of this engine is a
  round of its own; not started.
- Transmitter [DATA 34 presets, tmp scratch tr.py]: state = i32 version 3 + 47 ints. Ints 1–33 are controls 0…10000
  (Default: 7475, 1968, 1968, 8743, 8743, 5000, 5000, 10000, 9112, 0, 5000, 5051, …), 34–42 flags/selectors (34–38
  0/1 enables: "Transient only" clears 38, "Sustain only" clears 37; 39–40 selectors 0…5; 41–42 0/1), 46 = 49…90,
  47 = 257 / 65793 (bytes). The DSP (transient / sustain split, Saturation / Bounce / Diffusion / Delay per section) needs
  a decompile of Transmitter.dll before LYNX can reproduce it; until then an imported Transmitter is reported as unsupported.
- Fruity Scratcher: **done** in sub-round flscratch165 (REA_rodada165g-flscratch165.md): FL's engine (motor / hold,
  smoothing, rotations, DC blocker, gain / pan glides), FL's 16-bit copy of the sample, state + sample path + pattern
  events imported, editor (scratch with the mouse, Browser / desktop drops). LYNX's renders vs FL's: motor within 1 LSB
  (96.5…99 % of the samples equal), hold 99.5…99.95 % equal. Left: the DVS timecode input (engine_x64.dll) and RIAA,
  FL's stop / reset of the platter at the song's end and its render pre-roll, FL's chunk grid for pattern events
  (2…4 ms), the resampling of non-44.1 kHz samples [HYP].
- Fruity Video Player: done in batch 2 (§7).

## 7. Fruity Video Player [DECOMP Fruity Video Player_x64.dll][DATA Default.fst]
What FL's plugin is (DFM forms + handlers): a generator that shows a video and plays its sound track, slaved to the
song position by default. Controls: the video position slider (left/right keys = previous / next frame), Load, the
options menu, Start / End time labels (click = go to, right click = set), Mute and the Volume wheel (0…127, default
100), set start / end from the current position, Loop, Play / pause, Stop, "Slave playback to host", the current time
(right click = type it). Options menu: Enable audio, Dock control panel, Ignore tempo changes, Keep aspect ratio, Time
display (H:M:S:Ms and 14 SMPTE rates, menu tags = the saved index), Send audio to playlist, Delay playback (Set… /
"Delay playback until that time"). The video window can be detached, full screen, stay on top.
- State (SaveRestoreState 0x72d9b0, VMT slot 3 of the class CreatePlugInstance 0x72c8a0 builds): int version 3;
  int flags — bit 0 Loop (+0x14c, LoopBtnClick 0x7298f0), bit 1 Keep aspect ratio (+0x204, mnuKeepAspectRatioClick
  0x727d90), bits 2–3 the player's own transport (+0x14d), bit 4 Enable audio (+0x14e, mnuEnableAudioClick 0x727d30;
  bits 4|5 when read), bit 6 Ignore tempo changes (+0x209, 0x727dc0), bit 7 Slave playback to host (+0x20a,
  SyncBtnClick 0x729360; on for states before version 3), bit 8 Dock control panel (form +0x77a, 0x727cd0); int time
  display (+0x1cc; 0 H:M:S:Ms, 1 23.976, 2 24, 3 25, 4 29.97 drop, 5 30, 6 Auto, 7 29.97, 8 48, 9 50, 10 59.94 drop,
  11 59.94, 12 60, 13 72, 14 120 — the doubles at 0x72c4c0…); int count 3 + the parameters Volume, Mute, Play
  (+0x1f8); the path (FUN_0071b530: a byte length, 0xFF + int when ≥ 255, UTF-16; through the host's path converter,
  host calls 0x1c / 0xe); two ints (+0x124 / +0x128, the window size); with a path the start and end times (int64,
  100 ns: 10⁷ per second at 0x72c4c0). Default.fst: flags 0x92, Volume 100, size 375 × 0. The delay (form +0x770) is
  not in FL's state.
- The time display (0x72bb50): "H:MM:SS:mmm", or the SMPTE frame count at the mode's rate.
- LYNX (core/VideoPlayer, ui/VideoPlayerView): FL's state as is (+ "LYNXvp1" and the delay after FL's fields, where
  FL's reader stops); the instrument plays the sound track (decoded once by the user's ffmpeg to 48 kHz stereo, in the
  background; while exporting, before the render) at the video's time; the editor shows the frames ffmpeg decodes at
  the shown size (one process per stretch of playback, one frame when stopped), FL's controls and menu, a detached
  window (double-click: full screen), file drop, "Send audio to playlist" (the track as a WAV audio clip at the
  delay). Clock [HYP, FL's exact rule not traced]: slaved and following the tempo, video time = the song's beats at the
  song tempo; with Ignore tempo changes, the song's real time. .flp import: the channel becomes a Video Player (no
  longer a muted stand-in). Patcher: Add plugin › Visual.
- Verified [TEST] video165_* (FL's Default.fst bytes round-trip, SMPTE drop frame, clock, the instrument's track at
  the delay / volume / mute); [SHOT] `LYNX.exe --videoshot prefix clip.mp4` on Windows with ffmpeg 8.1 (fresh
  profile): the editor's frames equal ffmpeg's frames at the same video times.

## 8. Drops on the plugin windows (audit 165)
- Bug: a sample dropped on the Fruity Granulizer's window fell through to the Playlist under it and made an audio clip
  (switching to Song mode). FL's Granulizer takes the drop and plays the sample. Fix: the plugin page (ui/ChannelSettings
  ModelPanel) is a Browser and OS drop target when projectcontents::pluginWindowTakesSample (the Granulizer), for the
  files SampleLibrary::isSampleFile accepts (core/SampleFiles.h, .wv included); the drop sets the channel's sample,
  keeps the knobs and the channel name (one undo step), with an accent frame while hovering. The Patcher node's Sample...
  button takes the same file types now.
- [TEST] grandrop165_granulizer_window_takes_a_dropped_sample: the drop's edit rebuilds the channel's instrument, which
  then renders the new sample exactly (and not the old one).
- [SHOT] Windows, fresh profile, real input (FL's "Render tests/Fruity Granulizer.flp"): Browser › Packs › Loops ›
  DL Breaker dragged onto the Granulizer window → no new channel, no clip, Pattern mode kept, Current project › Audio
  files = DL Breaker (808 Kick gone), History "Channel: 808 Kick". Explorer drag of DropLoop.wav onto the same window →
  Audio files = DropLoop. (Before the fix, the same Browser drag made a "DL Breaker" audio clip channel and switched
  to Song.)
- Video Player: takes video files dropped from the system (the Browser lists no videos). Fruity Scratcher: its sub-round
  (flscratch165) was asked to give its window the same drop target.
