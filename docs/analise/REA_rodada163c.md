# Round 163c — hardware recording at Pro Tools / Nuendo level (rec163)

User feedback (2026-10-26, FL user who records outboard/hardware, no cloud): "FL is really weak when it comes to
recording audio real quick … you can really stand out by having the analog process improved to match the likes of
Pro Tools/Nuendo." Goal: the fastest path from "press record" to "clip on the Playlist", with sample-accurate
placement, no drift, no lost samples, nothing lost on a crash — keeping the FL-like workflow.

Evidence markers: [CODE] read in LYNX's sources, [TEST] unit test, [RUN] measured with the app/CLI, [DOC] vendor
manuals / common knowledge of the other DAWs (no reverse engineering of third-party binaries).

## 1. Audit — what LYNX had before this round

| Area | LYNX before r163 | Where |
|---|---|---|
| Record arm per mixer insert | ✅ arm button on every insert strip + in the effects panel; arming opens the input menu when the insert has no input | [CODE] MixerComponent `Part::arm`, `mixerinput::toggleArm` (REA_rodada88) |
| Input selector (stereo pairs / mono channels) | ✅ FL's "Audio input source" list: pairs first, then each channel | [CODE] core/LiveInput.h `inputSources`, .flp 0x9A import (r157c) |
| Input meters | ✅ thin level bar on the strip header and in the input box | [CODE] `LiveInput::meters` |
| Input monitoring modes | ✅ Off / When armed / Auto (tape-style: armed and not playing back, or recording) / On | [CODE] `inputMonitored()` |
| Record-safe | ❌ | — |
| Latency compensation of takes | ⚠️ driver-reported input + output only; no measurement; plugin delay compensation (PDC) of the Master ignored; clip start rounded to a whole tick (up to ~25 samples at 120 bpm / 48 kHz); takes in Song mode wrapped at the song end and became one clip running past it | [CODE] AudioEngine::finishAudioTake (old) |
| Round-trip measurement | ⚠️ CLI only (`--devicetest … --roundtrip`, r161a), not used for recording | [CODE] Main.cpp |
| Writing takes to disk | ⚠️ JUCE ThreadedWriter: when its FIFO was full `write()` returned false and the block was **dropped** — the take got shorter and everything after it moved earlier (drift); a take whose input channel was missing in a block skipped that block too | [CODE] |
| Crash safety | ❌ the WAV header was written at close: a crash left a file whose header says 0 samples (most players show it empty); nothing told the user | [CODE] |
| Punch in/out | ⚠️ FL's "recording markers" (Start/Stop recording markers on the Playlist) cut the take after recording | [CODE] Recorder.cpp `punchRanges` (REA round 139) |
| Pre-roll / post-roll | ❌ (count-in only: metronome bars before playback starts) | [CODE] |
| Count-in | ✅ 1–4 bars | [CODE] `countInLength` |
| Loop recording into takes | ❌ a take recorded over a looping region became one long clip past the loop end | [CODE] |
| Take lanes / comping / promote | ❌ | — |
| One-key record to the selected track | ⚠️ "Arm the selected insert for audio" in the record menu; recording still needed R + Space and possibly the "What to record?" dialog | [CODE] |
| File naming | FL's `<project>_<date time>_<insert>.wav` | [CODE] safety::recordingName |
| Recording format | 24-bit WAV only, device rate | [CODE] |
| Disk-space warning | ❌ | — |
| Hardware insert (External FX) | ❌ (hardware outputs per insert exist since r156, inputs per insert since r88) | — |

## 2. Gap table — LYNX vs Pro Tools / Nuendo / FL

✅ = has it, ⚠️ = partly, ❌ = no. "LYNX now" = after this round.

| Feature | Pro Tools | Nuendo / Cubase | FL Studio | LYNX before | LYNX now |
|---|---|---|---|---|---|
| Arm per track, input selector, input meters | ✅ | ✅ | ✅ (mixer insert input) | ✅ | ✅ |
| Monitoring off / auto (tape) / always | ✅ (Auto/Input) | ✅ (Manual/Tapemachine/While record enabled) | ⚠️ | ✅ | ✅ |
| Record-safe | ✅ | ✅ (Lock record) | ❌ | ❌ | ✅ |
| Latency compensation of recorded audio from the driver | ✅ | ✅ | ✅ | ✅ | ✅ |
| Measured round-trip (loopback ping) | ⚠️ (H/W insert delay comp., manual offsets) | ✅ (Record shift / ASIO-Guard, External FX "ping") | ❌ | ⚠️ CLI | ✅ Options › Audio |
| Manual record offset | ✅ (H/W Insert Delay, Delay compensation) | ✅ (Record Shift) | ❌ | ❌ | ✅ |
| PDC included when recording through a delayed Master | ✅ | ✅ | ✅ | ❌ | ✅ |
| Sample-accurate clip placement | ✅ | ✅ | ⚠️ (ms offsets) | ❌ (whole ticks) | ✅ (sub-tick offset) |
| No drift / no dropped samples on long takes | ✅ | ✅ | ✅ | ❌ (FIFO full = drop) | ✅ (overflow → silence at its place, counted, warned) |
| Crash-safe recording (file usable after a crash) | ✅ | ✅ | ⚠️ | ❌ | ✅ (header every second + journal + recovery dialog) |
| Disk-space warning | ✅ | ✅ | ❌ | ❌ | ✅ |
| 24-bit / 32-bit float files at the device rate | ✅ | ✅ | ✅ | ⚠️ 24 only | ✅ |
| Take file naming by track + take number | ✅ | ✅ | ❌ | ❌ | ✅ (`<Insert>_Take<N>`, FL naming optional) |
| Punch in/out on a time selection | ✅ | ✅ | ⚠️ (markers) | ⚠️ (markers) | ✅ on the loop region (markers still work) |
| Pre-roll / post-roll | ✅ | ✅ | ❌ | ❌ | ✅ 1/2/4 bars |
| Count-in | ✅ | ✅ | ✅ | ✅ | ✅ |
| Loop recording into takes | ✅ | ✅ (lanes) | ⚠️ | ❌ | ✅ one clip per pass, last on top, earlier muted on folded lanes |
| Comping (select range, promote take) | ✅ | ✅ | ❌ | ❌ | ✅ promote a lane (or its loop-region part) to the comp track, with short fades |
| One key from record to clip | ✅ (3 on numpad / F12) | ✅ (*) | ⚠️ | ⚠️ | ✅ Shift+R (arms, records, Shift+R stops) |
| Song keeps playing past its end while recording | ✅ (linear timeline) | ✅ | ⚠️ (loops) | ❌ (loops back to the start) | ✅ (Song mode without a loop region) |
| Hardware insert (External FX with ping) | ✅ (H/W Insert) | ✅ (External FX) | ❌ (Fruity Send + mixer I/O by hand) | ❌ | ❌ not done (§4) |

## 3. What changed (batch 1: arm, monitoring, latency compensation with the ping)

### 3.1 Sample-accurate placement [CODE][TEST]
`core/TakeRecording.{h,cpp}`: `takes::place (firstTick, takeTicks, ticksPerSample, latency)`.
- `firstTick` = the transport position of the block the first sample came in; `takeTicks` = the sum of `n * tps` over
  the recorded blocks (never wrapped by a loop); `latency` = `takes::Latency::total()`.
- The clip starts on the next whole tick; the audio skipped at the clip start (`offset + offsetFraction`, the
  existing sub-tick field the FL importer already used) keeps the rest, so the sample recorded at song time `t`
  plays at `t`. Test `rec163_placement_is_sample_accurate`: 2000 random positions/tempos/latencies, worst error
  < 0.01 sample.
- Punch parts and loop passes keep the same sub-tick alignment (`rec163_punch_parts_keep_alignment`,
  `rec163_loop_passes`).

### 3.2 Latency compensation [CODE][TEST]
`Latency::total() = round trip + fine adjust + PDC`, where round trip = a measured ping for this exact device setup
(`setupKey`: driver type | output | input | rate | buffer) when there is one and "use measured" is on, else the
driver's input + output latency. The Master's PDC is added because the song is heard that much later than it is
scheduled (a lookahead clipper/limiter on the Master — the user's habit — delays everything he plays along to).
Options › Audio › Recording latency compensation shows the figure and its parts.

### 3.3 The ping [CODE][TEST]
`takes::LatencyPing`: 8 clicks (5-sample raised-cosine pulse, −6 dBFS) a quarter second apart on the chosen output
pair; the loudest input sample in each window is the click's return (polarity-proof: absolute value). The noise floor
is measured in the first period; a click must be > max(−54 dBFS, 8 × noise RMS). Result = median; refused when fewer
than ¾ of the clicks came back or the spread is > 4 samples (an unstable driver). While it runs only the clicks
reach the outputs (no feedback through a monitored input). Test `rec163_latency_ping_finds_the_loopback`: simulated
loopback 371 samples, gain −20 dB, inverted, noise → 371 exactly; silence, buried and jittering loops refused.

Measured in the app: see §5.

### 3.4 Take stream, no drift [CODE][TEST]
`takes::TakeStream`: lock-free SPSC ring (1 M frames ≈ 22 s at 48 kHz per take) between the audio thread and the
recording thread. If the disk stalls longer than that, the frames that don't fit are counted as a **gap at that
place**: the writer hands out that many zeros there, so the file keeps its length and every later sample its time
(the user gets "N samples became silence"). Every take gets every block (silence when its input channel is missing
in that block), so all takes of a pass stay in sync. Tests: random block/chunk sizes 400 k frames exact; forced
overflow → zeros exactly in the gap, the rest bit-exact; a 2-minute float take through the stream and the writer is
bit-exact with the exact sample count.

### 3.5 Crash-safe WAV [CODE][TEST]
`takes::WavTakeWriter`: plain RIFF/WAVE (PCM 24 or IEEE float 32), the header rewritten with the current length and
the file flushed + `fsync`/`_commit` every second → a crash loses at most ~1 s and leaves a playable file.
`takes::repairWav` sets the header from the file size (also a half-written last frame). A journal
`<take>.wav.lynxrec` (first tick, ticks per sample, rate, latency, insert, project) is written when the first sample
arrives and listed in `<settings>/recordings-in-progress.txt`; it is removed when the take is placed normally. At the
next start "Recordings recovered" offers: put them on the Playlist where they were recorded / show the folder / keep
the files only. Closing LYNX while recording also leaves the journals, so those takes are offered too.

### 3.6 Placement on the Playlist [CODE][TEST]
- Each take goes to the Playlist track linked to its insert (track mode "audio", REA round 146a) if it is free there;
  otherwise a new track after the last used one, which gets linked to the insert and named after it — the next take of
  that insert lands on the same track.
- Song mode without a loop region: while takes record, the song plays once and then silence instead of starting over
  (`liveInput.linearSong`, `scheduleSong (… wrap = false)`), so a long hardware take stays one clip.
- Song mode with a loop region: loop recording. The audio thread notes the take frame at each wrap (`takeWraps`), so
  the passes are cut at the real wraps (a pass is a whole number of samples, slightly longer than the loop in ticks;
  a fixed loop length would drift a sample per pass). The last pass plays on the take's track; earlier passes go muted
  on the tracks below, grouped and folded under it (Playlist track groups). Unmute/move a lane clip to use it.

### 3.7 UI [CODE]
- Mixer: arm button right-click › "Record safe (can't be armed)"; a slash through the arm button of a safe insert.
- Options › Audio › Recording: file format (24-bit / 32-bit float), "Name takes after the track", the compensation
  read-out, ping output/input, "Measure the round trip", "Use the measured round trip", fine adjust (samples).
- Shift+R ("Record audio now"): arms the selected insert (or opens its input menu), records audio and plays (count-in
  as set); Shift+R again stops — the clip is on the Playlist.
- Messages: low disk (< 10 min left; < 30 s refuses to start), record safe, frames lost.

### 3.8 Batch 2 — punch in/out, pre-roll, post-roll, count-in, crash safety [CODE][TEST][RUN]
- Record menu: "Audio: punch in/out on the loop region", "Audio: pre-roll" and "Audio: post-roll" (Off / 1 / 2 / 4
  bars, saved with the UI options). Count-in (1–4 bars, existing) counts in from where playback really starts.
- Punch: Play starts `pre-roll` bars before the region (or before the play position without punch); the take records
  everything (so nothing is lost if you start late) but only the region is kept on the Playlist (`takes::punch`,
  sample-accurate edges). While punching, the region is *not* looped (`LiveInput::noLoopRegion`): playback runs
  through the punch-out into the post-roll and then stops by itself (`punchStopAt`, checked on the engine's 30 Hz
  timer — the take's cut is exact anyway).
- FL's recording markers keep working as before when no punch region is active.
- Crash safety: §3.5 (header every second, journal, "Recordings recovered" at the next start).

### 3.9 Batch 3 — loop takes and comping [CODE][TEST][RUN]
- Loop recording: §3.6 (one clip per pass, cut at the real wrap frames; last pass on top, earlier passes muted on
  folded lanes named "<track> (take N)").
- Comping: right-click a lane clip › "Promote this take to the comp track" (or "… in the loop region …" when the loop
  region covers part of it — select the phrase with the time selection first). `takes::promoteTake` splits the comp
  track's clips at the range edges, removes what played there, and puts that piece of the take there unmuted with
  8-tick (~4 ms) fades at the cut edges; the lane clip stays as it was. One undo step.
- Test `rec163_take_lanes_and_comping`: lanes/grouping/naming, promote of a middle range keeps both sides of the comp
  with the right offsets (sample-exact), fades only on cut edges, out-of-range/invalid calls change nothing.

## 4. Not changed / notes
- **Hardware insert (External FX with ping) — not done.** It needs a new effect kind that sends the insert's signal
  to a chosen output pair and reads it back from an input pair inside the mixer graph, reports its measured round
  trip as plugin latency (PDC), is bypassed with a warning in offline export (hardware can't run faster than real
  time), plus its editor, saving and a ping button. That is a round of its own; the parts it would reuse are there
  (`takes::LatencyPing`, per-insert hardware outputs from r156, per-insert inputs). Today: route an insert to a
  hardware output and record the return on another insert (the take is placed with the measured compensation).
- The metronome node is not delayed by PDC (it goes straight to the output) — a performer following the click
  rather than the song hears it `pdc` samples early. Takes are compensated for the song (what one usually plays to).
- Audio clips play from `startSeconds = offset × secondsPerTick` at the song's base tempo; tempo automation during a
  take moves the grid under it (same as before; the sample position of the start is exact).
- Files > 4 GB (≈ 6 h stereo 24-bit at 48 kHz) would need RF64; the header saturates at 4 GB.

## 5. Measurements
Method: the new `LYNX --rectest report.txt [seconds] [--type T] [--device D] [--input I] [--loop|--punch] [--buffer N]`
on the Linux build against a private PipeWire 1.6.2 daemon with `pw-loopback` (sink → source, the "cable"). It pings
the round trip, plays a 5-sample click on every beat at 120 bpm from an audio clip, records it on Insert 1 through the
loopback, and checks every recorded click's position on the Playlist (clip start + sub-tick offset) against its beat.
The default song has a lookahead limiter on the Master (72 samples PDC), so the PDC part of the compensation is tested
too. Takes are written next to the report (not into the user's Recordings folder).

| Run | Block | Ping | Compensation | Take | Clicks checked | Worst placement error | Lost |
|---|---|---|---|---|---|---|---|
| 20 s linear | 512 | 512, spread 0, 8/8 | 584 = 512 + PDC 72 | 960 000 frames = 20.000 s | 40 | 0.000 samples | 0 |
| 15 s linear | 128 | 128, 8/8 | 200 | 720 128 frames | 30 | 0.000 | 0 |
| 20 s loop bars 2–4 | 256 | 256, 8/8 | 328 | 3 passes (2 muted lanes) | 36 | 0.000 | 0 |
| 600 s linear (with 3 parallel compiles running) | 64 | 64, 8/8 | 136 | 26 982 336 frames (562.1 s) | 1088 | 0.000 | 0 |
| punch bars 3–4, 1 bar pre/post-roll | 256 | see below | | | | | |

The 600 s run: the device itself missed ~38 s of cycles while three compiles loaded the machine (PipeWire's timer
driver under WSL; the transport stood still with it), yet all 1088 clicks over the 9½ minutes land on their beats
with 0.000 samples of error and no frame lost — no drift.

## Where I stopped
- Batch 1 implemented (core + engine + UI), unit tests green in the mini harness; full build-local and Windows build
  running. Next: loopback check of the placement with the PipeWire test daemon, --rtstress --record long take, land
  batch 1; then batch 2 (punch on the loop region, pre/post-roll), batch 3 (comping / promote), batch 4 (hardware
  insert).
