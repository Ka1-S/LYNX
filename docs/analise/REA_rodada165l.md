# REA round 165l (edison165): LYNX's audio editor gets FL Edison's tools

Goal: every Edison tool form (`TMEnvEditor` + `TME*ToolForm`, FL Studio 2026) in LYNX's audio editor
(`src/ui/AudioEditor.cpp`), studied with REA (clean-room: FL's forms and decompiled behaviour are read to learn what
each tool does and with which parameters; the DSP, dialogs and drawing are LYNX's own), FL-like dialogs in LYNX style,
undoable, previewable, computed off the audio thread; nulls vs FL where the tool is deterministic.

## Where I stopped

- 2026-10-27 16:40: worktree `../lynx-wt-edison165` on `feature/edison165` from main 5ed718d1; inventory of the
  tool forms from r165f's DFM dump (`tmp/rea165/dfm/TME*.dfm.txt`) started.

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
