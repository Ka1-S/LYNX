# REA round 165l (edison165): LYNX's audio editor gets FL Edison's tools

Goal: every Edison tool form (`TMEnvEditor` + `TME*ToolForm`, FL Studio 2026) in LYNX's audio editor
(`src/ui/AudioEditor.cpp`), studied with REA (clean-room: FL's forms and decompiled behaviour are read to learn what
each tool does and with which parameters; the DSP, dialogs and drawing are LYNX's own), FL-like dialogs in LYNX style,
undoable, previewable, computed off the audio thread; nulls vs FL where the tool is deterministic.

## Where I stopped

- 2026-10-28 (after the 2nd BSOD): merged origin/main (CMakeLists, build scripts, Main.cpp shots, pt-BR: duplicate
  "1/4 beat" dropped); build-local with FL Data 1 781 887 checks, 0 failures; Windows build OK; `--edisonshot` re-run on
  Windows with a fresh profile + LYNX_TEST_MODE (C:/Users/kauan/lynx-test/edison165/shots): the tool windows and the
  volume envelope look as in the earlier shots. Landing.

- 2026-10-27 21:15: everything compiles (Windows build OK, Linux syntax checks OK); the core tests pass in a mini
  harness (209 checks, 0 failures) and the full `build-local` had only 2 failures, both fixed since (vocal denoiser
  noise floor, events-to-automation simplification); a second `build-local` and Windows build are queued.
  `--edisonshot` PNGs: `D:/lynx-work/edison165/shots/ed-*.png` (layout fixed after the first look: info text height,
  check box widths, declipper on two rows, frequency labels). Merged origin/main (pt-BR conflict regenerated).
- Next: bl2 + win3 green → new shots → notes → land.sh. FL side-by-side shots of the Edison forms not taken (FL GUI
  automation of Edison's menus judged too risky for the shared desktop this round).

## 1. Inventory: Edison tool forms vs LYNX (before this round)

Sources: [DATA] the forms' DFM resources in `FLEngine_x64.dll` (r165f dump, `tmp/rea165/dfm/TME*.dfm.txt`; summary
by `tmp/edison165/dfmsum.py`), Edison's menu (`TMEnvEditor` `WAVOptionsMenu`, `tmp/rea165/menus.txt`), [DEC] the menu
handlers and tool code decompiled from the analysed FLEngine project (`/mnt/d/lynx-work/rea165`, read-only; Edison's
code is also in `Plugins/Fruity/Effects/Edison/Edison_x64.dll`, the same forms are in both).

| Edison form (caption) | Controls [DATA] (range, default) | Menu entry | LYNX before |
|---|---|---|---|
| TMEAmpToolForm "Amp" | Panning wheel −256..256 (0) + law Circular/Triangular; Volume −512..512 (256 = unity); Left / Right volume (256); Stereo sep −256..256 (0); Reset / Preview / Accept | Tools › Amp › Amp... | ⚠️ volume dB + pan only |
| TMEBlurToolForm "Blur" | Amount 0..10000 (5000), Offset −2000..2000 (0), Use data outside selection (on), Mix 0..256 (256), Impulse envelope editor | Tools › Spectral › Blur... | ❌ |
| TMEReverbToolForm "Convolution reverb" | Offset −1000..1000 (0), Add tail (on), Use data outside selection (on), Dry 0..256 (256), Wet 0..256 (64), Impulse (a wave editor: load/draw) | Tools › Spectral › Convolution reverb... | ❌ |
| TMEEQToolForm "Equalizer" | Freq scale 0..256 (256), Smooth (on), Phases (off), Mix 0..256 (256), envelope = gain curve over frequency; menu: Filter unharmonics narrow/normal/wide (based on the middle note) | Tools › Spectral › Equalize... | ❌ |
| TMEDenoiseToolForm "Clean up (denoise)" | Denoiser (on): Freq scale 0..256 (0), Threshold 0..1000 (500), Amount 0..1000 (500), Output noise only, threshold curve editor; Declipper (off): Threshold 0..1000 (900), model General/Vocal, Safe mode; Declicker (off): Sensitivity 0..1000 (250); Acquire noise profile | Tools › Spectral › Acquire noise profile, Clean up (denoise)... | ❌ |
| TMEVocalDenoiseForm "Vocal denoiser" | Mix wheel −256 (noise) .. 256 (vocal), default 256 | Tools › Spectral › Vocal denoise/isolator... | ❌ |
| (no form) | — | Tools › Spectral › Match spectrum... | ❌ |
| TMEDrumStretchToolForm "Drum(loop) stretcher" | Pitch coarse ±12, fine ±100, mul; Time mul ±20000, length (ms), tempo; Insert tails (on), Auto-slice (off); Filling: None/Echo grains/Stretch/Pogo (Echo grains), Blur (on), Random presence, Loop; Tail envelope: None/Smooth fade/Append (Smooth fade), Transient 0..1000 (700), Tail slope 0..2000 (1000), smoothing 50..2000 (250) | Tools › Time › Drum(loop) stretch..., Edit › Paste replace (drum) | ❌ |
| TMEClawToolForm "Claw machine" | Period 1/4 beat..16 beats (Beat), Trash every 1/4..4/4, 1/3..3/3, 1/2..2/2 (4/4), Snap to markers (on); Compensation stretching: Amount 0..1000 (0), Method Resample/élastique (e3 generic) | Tools › Time › Claw machine | ❌ |
| TMELoopToolForm "Loop tuner" | Crossfade length 0..5000 (2500), tension 0..1000 (250), snap 1..7 (5, "see middle note": period snap); Blur order 1..25 (5), scale −1000..1000 (0), amp 64..192 (128) | Tools › Regions › Tune loop... | ❌ |
| TMEStretchToolForm "Time stretcher / pitch shifter" | Pitch coarse ±12, fine ±100, mul ±20000; Time mul ±20000, length (ms); Method; Insert; Formant preservation: factor coarse/fine/mul, order 8..512 (128), Copy from pitch | Tools › Time › Time stretch / pitch shift..., Edit › Paste stretch | ⚠️ length %, pitch, fine, 2 methods |
| TMEScaleToolForm "Level scaling" | Center 0..8192 (0), Multiply −16384..16384 (8192 = ×1), Offset −8192..8192 (0), Tension −8192..8192 (0) | Tools › Envelope › Scale levels... | ❌ (no envelope) |
| TMEDecimateToolForm "Decimate" | Threshold 0..1000 (500) | Tools › Envelope › Decimate points... | ❌ |
| TMEEnvFilterToolForm "Filter" | Width 0..1000 (500), Offset −1000..1000 (0), Decimation 0..1000 (500), Impulse envelope | Tools › Envelope › Filter... | ❌ |
| TMESmoothToolForm "Smooth up" | Attack / Release time 0..1000 (500), slopes 0..1000 (500), Offset −1000..1000 (0), Decimation 0..1000 (500) | Tools › Envelope › Smooth up... | ❌ |
| TMEArpToolForm "Create sequence" | step / break / level pattern editors, Mode Normal/Ping-pong, Time mul 0..10 (2), Swing, Attack, Decay (500), Sustain (800), Gate 100..1000 (550), Randomize | Tools › Envelope › Create sequence... | ❌ |
| TMEEventsToAutoTool "Convert to automation" | Sensitivity 400..1000 (1000) | Event editor › Turn into automation clip | check |
| (no form) | — | Tools › Envelope: Cancel all envelopes, Add points at selection / regions, Flip, Normalize levels, Turn all points smooth; Amp › Mixdown amp envelopes; Time › Scratch (envelope) | ❌ |
| (no form) | — | Amp › Lossy normalize; Channels › Convert mono signal to mono format; Synthesis › Generate noise; Noise gating › Acquire noise threshold, Gate noise, Trim side noise, Trim all noise & slice up; Regions › Normalize / Declick in / Declick out / Perfect all regions | ❌ |
| TMEWAVPropForm "Sample properties" | title, comments, format, rate, channels, middle note, fine tune, tempo / beats, tempo detection | Format › Edit properties... | ⚠️ |

## 2. FL's default maps [DATA]

`Plugins/Fruity/Effects/Edison/Data/Maps/*.fnv` (FL's envelope files: int version (3 = time, 7 = frequency in Hz),
int ?, int point count; then per point double Δx, double y, float tension, int curve mode): Blur default = a bell centred
on the sound (0 → 1 at x 0.5 → 0), Env filter default = a triangle, EQ / Denoiser default = flat at 0.5 (0.5 = unity /
unchanged threshold; EQ "40Hz cut" = y 0 up to 40 Hz, then 0.5), Spectral mask default = flat at 1. LYNX's tool windows
start from the same shapes (the numbers, not the files).

## 3. LYNX's implementation

- Core (`core/AudioEditTools`, JUCE-free, tests `tests/AudioEditToolsTests.inc`): point envelopes (`Envelope` in
  `core/AudioEdit.h`, carried by the `Document` for the amp envelopes, so they are in the undo history), the envelope tools,
  amp envelopes mixdown, Amp (L/R volume, circular/triangular pan law, stereo separation), lossy normalize, mono-if-mono,
  noise generation, noise gating (threshold, gate, trim side noise, trim all & slice into regions), region batch tools,
  an STFT (sqrt-Hann, hop N/4) for Equalize / Match spectrum / Blur / Clean up / Vocal denoise, FFT convolution for the
  reverb, the declipper / declicker, Claw machine, Drum(loop) stretch (slices keep their attacks, fill / tail methods),
  Loop tuner (period-matched loop end + crossfade with tension and blur).
- UI (`ui/AudioEditorTools`): one window per tool, sections like FL's form, LYNX's controls; curve editors (frequency
  axis with the selection's spectrum or the noise profile behind the curve); Reset / Preview (Space) / Accept (Enter) /
  Cancel; the audio tools run on a worker thread (progress bar, Stop), Accept = one undo step; the envelope tools edit the
  envelope over the audio live and Cancel puts it back.
- The editor: "Edit: Wave / Volume / Panning / Stereo sep" (E) like Edison's edit selector; the envelopes are heard when
  playing and mixed down by Tools › Mixdown amp envelopes; Tools menu in FL's groups (Envelope, Amp, Waveform, Time,
  Channels, Synthesis, Noise gating, Spectral, Regions).

## 4. Decompiled behaviour [DEC] (FLEngine_x64.dll, read-only project of r165f)

- **Normalize menu (TEditor `NormalizeMenuClick` 0xce57c0)** dispatches on the item tag. Fade in (8) / fade out (9)
  are a volume ramp 0→1 / 1→0 with tension 0.999999 / 0.9999; "mono if mono" (10) only converts when the channels
  are identical; declick in/out (11/12) are the region declickers.
- **Lossy normalize (tag 15, 0x697920)**: only when the selection is ≤ 30 000 ms (`len * 1000 / rate`), else a plain
  normalize. It tries passes k = 1..249 with an angle `k * 0.0251327 = k * 2π/250`, measures the peak of the
  selection after each trial transform, keeps the lowest, applies it, then normalizes to 1.0 and reports
  `Gain: x dB` = 20·log10(old peak / new peak). The trial transform works on a 2^n FFT (n = log2 of a size derived
  from the sample rate, clamped 4..20) and does not change the magnitude spectrum, i.e. a constant phase rotation.
  LYNX: `lossyNormalize` = the same search with an analytic-signal (Hilbert) rotation `x cos t + H(x) sin t`, then
  normalize; returns the ratio. (Was a soft-clip guess before.)
- **Generate noise (WDelMenuClick tag 10)**: fills the selection with noise then normalizes it to 1.0.
- **Claw machine (0xcd2070 → core 0xc7b460)**: the "Trash every" value (separators not counted) gives m = 4 (0..3),
  3 (4..6), 2 (7..8) and the part n; Period = {1/4, 1/2, 1, 2, 4, 8, 16} beats (a float table). From a beat grid
  anchored at the sample's start, each period loses the n-th of its m parts; with Snap to markers the cut's end moves to
  the nearest marker within half a cut. Each join: the 15 ms before the cut are mixed (ramp) with the last 15 ms of
  the removed part, so the audio flows into what follows. Then, if Amount > 0, the whole edited range is stretched
  by `1 + (m/(m-1) - 1) * amount/1000` (clamped 0.01..100), by resampling or by the chosen élastique mode, with 25 ms
  edge fades for some modes. LYNX: `clawMachine` does the same (cuts from the last, ramp mix, one global stretch;
  WSOLA in place of élastique).
- **Paste mix (envelope) (WDelMenuClick tag 11 → 0xce28f0)**: every frame of the whole sample becomes
  `clip * e + original * (1 - e)`, e = Edison's 4th envelope (the one the editor's selector calls "scratching
  envelope", evaluated over the sample's original length; outside the clip the clip counts as silence); the sample
  grows when the clip goes past its end; the envelope is reset and the editor goes back to the waveform. LYNX: Edit ›
  Paste mix (envelope) shows that envelope ("Edit: Paste mix", flat 0.5 to start), the second click applies
  (`pasteMixEnvelope`), one undo step.
- **Paste mix (spectral) (tag 12 → 0xce2de0)**: opens the EQ form in its mask mode, processes the source (the
  clipboard) and the target (the audio under it) with it, then mixes them. FL's default mask map is flat at 1.
  LYNX: the tool window "Paste mix (spectral)" with a mask curve (y = share of the clipboard per frequency, the
  original gets 1 - y), default flat 1 (`pasteMixSpectral`).
- **Scratch (envelope) (ScratchMenu 0xce30d0 → 0xce23b0)**: the same 4th envelope is the play position: output frame
  i reads the original at `env(i) * length` through a resampler, in blocks of 16 frames, the resampler's filter set
  from the smallest step in the block. LYNX: Tools › Time › Scratch (envelope) shows a position envelope (a rising
  diagonal = unchanged), the second click renders it (`scratch`: interpolation when slower than normal speed, the
  average over the span played when faster, so fast moves do not alias).
- **EQ "Phases"** [DATA hint "Edit phases instead of magnitudes"]: LYNX keeps a second curve; y 0.5 = no change,
  0 / 1 = −/+180° per frequency.
- **Declipper "Model" General / Vocal and "Safe mode"** [DATA]: LYNX: the general model rebuilds peaks up to +12 dB
  over the clip level, the vocal one a rounder bump up to +3 dB; safe mode never goes over full scale.
- **Time stretcher / pitch shifter form** [DATA TMEStretchToolForm]: now a full tool window: pitch coarse/fine,
  time mul / length / target tempo (one setting three ways), method Stretch / Resample, Insert, Formant
  preservation (shift coarse/fine, order 8..512 = 128, Copy from pitch). Formants: a cepstral envelope (order/4
  coefficients of a 2048 frame) of the original at the same moment replaces the processed one's (`correctFormants`).

## 5. FL side by side [UI]

FL Studio 2026, Edison on the Master track with `Packs/Loops/DL Breaker.wav` (FL GUI under the wingui + FL locks, FL
closed right after): `D:/lynx-work/edison165/flshots/03-tools.png` (Edison's Tools menu: the same groups as LYNX's),
`fl-amp.png`, `fl-equalizer.png`, `fl-cleanup.png`, `fl-claw.png`, `fl-stretch.png`, `fl-drumstretch.png`. LYNX:
`D:/lynx-work/edison165/shots/ed-tool-*.png` (`LYNX.exe --edisonshot`). Same sections, controls and defaults; LYNX
draws them in its own style (sliders / knobs / check boxes in rows, one curve editor per form). FL's Claw machine
showed "Trash every 1/4" (a remembered value; the form's default is 4/4, which LYNX uses). Items of FL's menu LYNX
does not have: Deverb (AI model), Convert to score and dump to piano roll, Run script / Edit last script.
