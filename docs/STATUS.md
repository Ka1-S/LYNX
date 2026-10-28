# Status

Last updated: 2026-10-27 (release 0.1.19 alpha). Everything below is verified by the core tests (more than a million checks), the
app's self-test (`LYNX.exe --selftest`), off-screen UI snapshots (`--snapshot`) and a regression run that
exports the 30 demo projects shipped with FL Studio 2026.

## Platforms (round 117)

Windows x64, macOS 11+ (universal: Apple Silicon + Intel) and Linux x86_64 build from the same source; CI builds and
self-tests the app on all three (`docs/PLATFORMS.md`). Windows-only: ASIO and the LYNX ASIO driver. VST2 hosting on
Windows and Linux; the out-of-process plugin sandbox on Windows and Linux (round 161: POSIX shared memory, crash noticed
at once through a pidfd, LV2/VST3 and yabridge-bridged Windows plugins). macOS adds Audio Units, Linux LV2; drivers
Core Audio / ALSA + JACK (PipeWire).

## What works

### Editing
- **Channel Rack:** step sequencer, per-channel volume/pan/mute/solo, swing (FL formula, per-channel mix),
  multiple patterns (rename, colour, length, clone, remove), cut groups, mono channels, arpeggiator,
  PC keyboard as a piano (FL layout), MIDI input, note recording (one undo step per take). Step graph lanes
  (Ctrl+K: velocity, pan, release, fine pitch, Mod X/Y; drag, right-drag ramp, Alt+click reset), fill every
  2/4/8, Euclidean fill, rotate, sort channels, select unused.
- **Recording options:** count-in (Ctrl+P), wait for the first note (Ctrl+I), overdub/replace, record quantise
  following the global grid, a pattern clip placed in Song mode; switches next to the metronome in the main
  toolbar and the full set in the "what to record" menu and Options → Recording.
- **Quick-add palette (F8 / plugin-picker icon):** searchable instruments and effects (new channel or selected
  insert). **Undo history** (Ctrl+H, Edit menu): named steps, click to jump.
- **Piano Roll:** selection (Ctrl+drag, Shift+click, Ctrl+A), group move/resize, Ctrl+C/X/V, Ctrl+B duplicate,
  Ctrl+Q quantize, arrow keys (Shift = octave/bar), snap selector, chords and scale highlighting, per-note
  velocity/pan/fine pitch/mute, slide (808 glide) and porta notes, `.mid` import/export. Note tools
  (Arpeggiate, Strum, Flip, Randomise, Scale levels, Limit; Alt+A/S/Y/R/X/K), snap to scale (toolbar switch next
  to the scale box, also for incoming MIDI), zoom presets (PgUp/PgDn, Shift+1..5/0), vertical zoom.
- **Playlist:** pattern, audio and automation clips; multiple arrangements; loop region; multi-selection,
  copy/paste/duplicate, split, "make unique"; named/muted tracks; markers and time-signature changes on the
  ruler; normalize audio clips; pattern picker. Ripple editing (Shift+S) and ripple delete, zoom presets,
  inline automation-point editing (axis locks, typed values, re-range), track and clip menus.
- **Automation:** right-click any knob/fader (channel, mixer, effects, plugin parameters) → a Playlist clip with
  a point/curve editor (Alt+drag for curves); tempo automation; MIDI learn on any parameter.
- **Mixer:** up to 127 inserts, sends with levels, "route to this track only", solo along the signal path, FL
  fader curve, gain ramps, stereo separation, phase invert, L/R swap, colours, built-in 3-band track EQ,
  10 effect slots with dry/wet, smart disable, per-edge PDC, **sidechain** (right-click a compressor slot →
  "Sidechain from"), render insert to audio. Multi-select inserts (faders/pans move together), Alt+Left/Right
  reorder, link selected channels (Ctrl+L / Ctrl+Shift+L / Alt+L), "sidechain only" sends, per-insert record
  arm, effect slots dragged to reorder, all-effects switch, A/B compare per slot, effect presets dropped from the
  Browser.
- **Spectral waveforms:** audio clips, the Sampler settings and the Browser preview draw a detailed waveform
  (RMS body + peak outline, smooth at any zoom) coloured by frequency content: warm = bass, lime = mids, cool =
  treble. Analysed in the background and cached on disk; Options → Waveforms switches to the classic one-colour
  look and to stereo lanes.
- **Browser:** tabs ALL / PROJECT; "Current project" lists the history, patterns, effects, generators, automation,
  the project folder and the audio files the project uses, each in the colour of what it belongs to, with details
  (insert, "2x", "missing"), hints, menus and drag into the Playlist; every top-level folder has its own colour;
  search (Ctrl+F: wildcards, AND/OR/NOT), Up/Down audition, Shift+Up/Down and Enter load into the
  selected channel, user folders.
- **Toolbar editing:** right-click the toolbar > Edit toolbar: drag panels to move them, into the tray to remove them,
  from the tray to add them; presets, separators, Ctrl+F11 hides the panels. Context menus of mixer tracks, channels,
  Playlist tracks/clips and patterns open with a title band in the item's colour; undo says what it undid.
- **Window menus:** each window's title-bar menu carries its own items (Playlist ripple/zoom, Channel Rack graph
  lanes/rotate/sort, Piano Roll tools/zoom/scale snap, Mixer move/link, Browser search/add folder).
- **"Inspired by" editors (round 114):** the LYNX engines that stand in for third-party plugins work like the
  originals — display-first editors for Apex Limiter, Crest Compressor, Prism Multiband, Sentry Gate, Hush De-esser
  and Ember Saturator (scrolling level / gain-reduction or spectrum displays edited by dragging: threshold lines,
  gain slider, ceiling, band nodes, crossovers; knobs floating over the display; settings in a bottom bar), a VU
  needle on Glow Leveler, the reverb / delay panels with captions above the knobs and stepping mode menus
  (docs/analise/REA_rodada114.md).
- **Project safety:** "*" in the title, "save changes?", atomic saves, autosave every 2 minutes as "Song (autosaved on
  2026-10-20 at 14h19)" and the replaced file kept as "Song (overwritten on …)" (rotation by time), an emergency save on
  crash and a "did not close properly — open the newest backup?" prompt at the next start, File › Save new version
  (Song_2, Song_3…), Revert to last backup, Export project with samples (.zip, opened back by Open), Collect project
  samples into a folder; samples are found next to a moved project (REA round 118; `%APPDATA%\LYNX\autosave`).
- **Crash / freeze reports (opt-in, round 159):** after LYNX closed unexpectedly the next start asks "Send the report
  to help fix it?" (Send / Show what will be sent / Don't send, "Always send without asking", "Report on GitHub
  instead" with the bug form prefilled, "Open the folder with the report"); Help › Report a problem… for manual
  reports; Settings › General › Crash reports: Ask / Always send / Never. Anonymised JSON (no user or machine name,
  no file names unless ticked, no audio / projects) gzip-POSTed to lynx-share.com/api/crash.php in the background,
  retried once at the next start when offline (docs/analise/REA_rodada159.md, PRIVACY.md).
- **Opening FL projects** behind a progress panel (round 112): samples decoded in parallel off the UI thread, hosted
  plugins created one by one with their names shown ("Loading Auto-Tune Pro... 3/12"), no repeated plugin searches,
  missing drives probed once; phase times in `%APPDATA%\LYNX\load.log` and in the `--export` report.

### Sound
- **Built-in synth:** 3 oscillators (sine, triangle, saw, square, noise and a band-limited **harmonics**
  oscillator built from an amplitude/phase table by IFFT), octave/semitone/fine/pan/level per oscillator,
  LP/BP/HP/BS filter with keytracking, cutoff and pitch envelopes, 4 LFOs, AM, FM with decaying index, ring
  modulation, kick pitch sweep, 3-band tone EQ, mono and portamento.
- **Sampler:** multi-sample zones (key/velocity), slices, reverse, cut, time-stretch (resample or WSOLA), root
  note, start offset, channel filter and envelopes; FL's own interpolators (24-point sinc while playing, 32-point when
  exporting — FL's defaults; linear/6-point/sinc kernels bit-identical to FLEngine's, REA round 69); long audio
  streamed from a memory-mapped disk cache.
- **Built-in drums:** kick, snare, hat, clap.
- **Effects:** EQ, 7-band parametric EQ, compressor (with sidechain), limiter (lookahead, PDC), delay, reverb,
  chorus, filter, distortion, lo-fi, Effector (12 algorithms), vocoder, pitch correction, volume gate, rhythmic
  envelope, auto-pan, Linkwitz-Riley crossover, pitch shift, frequency shifter, utility and a nestable
  parallel container.
- **Plugins:** VST3 and CLAP (own CLAP host on top of JUCE); scanning always runs in separate processes with a
  blacklist; optionally every plugin runs in a separate process (*Options → Run plugins in a separate process*). A hosted plugin
  that crashes inside an audio / parameter / state call is caught, switched off (bypassed / silent) and reported, and
  LYNX keeps running; scan children end without unloading plugins whose teardown crashes (REA round 111).
- **Missing plugins:** a project effect whose third-party plugin isn't installed plays a LYNX substitute (reverb →
  Reeverb 2/LuxeVerb, delay → Delay 3, limiter, compressor, EQ…) from a data-driven table, translating settings when
  the saved state is readable; the original reference is kept byte for byte and comes back once the plugin is
  installed (docs/PLUGIN_FALLBACK.md).
- **Export:** WAV 16/24/32f, FLAC, OGG; whole song or pattern; tail until silence; TPDF dither + noise shaping
  for 16-bit.
- **Performance:** lock-free engine, channels and mixer layers on worker threads, idle channels skipped, FTZ/DAZ.

### FL Studio import (`.flp`)
- Structure: channels, real samples (including Ogg Vorbis WAVs and WavPack), patterns, notes with
  slide/porta/pan/pitch, every arrangement, markers and time signatures, automation clips (1100+ in the demos),
  controller links (Peak Controller, Formula Controller, Envelope/Keyboard Controller, X-Y/Control Surface as constants),
  automation of a wrapped plugin's MIDI CCs / channel aftertouch (wrapper indices 0x1000…0x108F), MIDI Out channels
  routed live to the plugins on their MIDI input port (round 133).
- Mixer: routing and levels, sidechain links (level-0 routes), track EQ, colours, names, and native effects with
  their saved settings: Parametric EQ 2 and Parametric EQ (exact engines), Limiter (incl. its compressor and sidechain), Compressor, Reeverb 2 (exact), LuxeVerb (exact), Convolver (exact),
  Delay 2/3, Delay Bank, Multiband Compressor, Maximus, Soundgoodizer, Fast Dist, Fruity Filter / Fast LP / Free Filter (exact), Newtone / Newtime (state kept, pass-through, source sample in slave mode; Newtone edits rendered by LYNX's engine), Flangus, Flanger, Soft Clipper, WaveShaper, Gross Beat, Effector,
  Love Philter (exact), Frequency Splitter, Stereo Shaper, Stereo Enhancer, Transient Processor,
  Spreader, Balance/PanOMatic (exact), Blood Overdrive (exact), Pitch Shifter and EQUO (exact engines), Hardcore, Distructor and Multiband Delay (exact), Frequency Shifter (exact),
  Emphasis, Low Lifter (exact), Emphasizer (exact), Send, Transporter, Squeeze, Pitcher, Fruity Vocoder (exact); Patcher graphs of any shape run as
  the real graph (round 141: nodes, cables with gains, notes, control cables, delay compensation, editor).
- Generators: 3x Osc on its engine (REA round 47: the three oscillators on FL's host wave tables, stereo detune and
  phase, random start phases, AM, the custom wave from the channel's sample read from the user's installation, the HQ
  band-limited oscillators, and the plugin voice processor 3x Osc keeps, with its release path; bit-identical to FL's
  DLL through our reference model on all 239 distinct non-HQ states of the installation at 44.1/48/96 kHz and on 43
  processor cases; HQ within −137 dB), FL Keys on its engine (REA round 69 §8: FL's wavetable loader and voice — 16.16
  linear reads, envelope, overdrive, muffle filter, treble, tremolo, stereo delay, MaxPoly 32 stealing; bit-identical to the
  installation's DLL on all 11 demo states and on dense 150-note stress runs), DirectWave on its engine (REA round 72: zone
  choice and alternation groups, the 16-slot modulation matrix, envelopes and LFOs, 32.32 linear reads and loops, filter
  types 1–7, zone effects, limiter, pan law, and the program's delay, chorus, plate reverb and shapers, with the
  plugin's CRT math; bit-identical to the installation's DLL on all 57 demo programs with its own decoded samples),
  FPC, Slicex/Fruity Slicer, Wave Traveller; DrumSynth Live on its
  engine (REA round 30: all 120 patches, one per key, morph, voice modes; recorded waves read from the user's own FL
  installation; within −138 dB of the reference model verified against FL's DLLs); Harmless on its additive engine
  (REA round 32: 528-partial bank, FL's full voice; within −151 dB of the reference model, which is within −60 dB of
  FL's DLL on 135 of 139 demo channels) and its effects rack (REA round 36: chorus, delay, reverb and the Maximus
  compressor with Harmless's own presets read from the user's FL installation; the model with the rack is within −60 dB
  of the DLL on 135 of 139 demo channels); Sytrus on its engine (REA round 31: the whole instance from the raw plugin
  state — operators, FM/RM matrix, articulators, unison, portamento, filters, effects chain; FL's render settings when
  exporting, its live ones when playing); Harmor on its additive engine (REA round 34: both parts, 520-partial bank,
  module chain, articulators, and its effects rack from REA round 38; image synthesis, arpeggio envelopes, noise
  resonance and the other round-34 gaps from REA round 75; 97 of 99 demo channels within −60 dB of FL's DLL with the
  rack off, the other two limited by FL's 11-bit IPP `exp`); Ogun on its
  inverse-FFT engine (REA round 37: the whole voice, articulators and plugin effects from the raw state; within float
  rounding of the reference model — which is within −60 dB of FL's DLL on 20 of 20 demo channels — on all 20 demo
  channels and 55 synthetic cases); GMS and Morphine on their engines (REA round 39: bit-identical to the reference
  models on every demo channel, voice and full chain, at 44.1 and 48 kHz; the models are within −60 dB of FL's DLLs on
  9/9 GMS and 7/7 Morphine demo channels; GMS's waves and Morphine's noise samples read from the user's FL
  installation); MiniSynth on its engine (REA round 41: wrapper, 99 voices, the ten oscillator kinds on the 1024
  tables built from the user's `oscdata.dat`, the exact matrix filter, envelopes, LFO and its four effects; bit-identical
  to FL's DLL on 11 of the 12 states of FL's projects and 47 of its 91 presets, the rest within −107…−147 dB); Fruity
  DX10 on its engine with the channel processor it keeps (REA round 42: bit-identical to the reference model, which is
  bit-identical to FL's DLL on all 64 distinct installation states and on the 12 project channels with their channel
  envelopes/filter); Kepler on its engine (REA round 46: the whole Kepler-1 engine with the DLL's rounding mode, LFO
  shape and noise read from the user's own engine DLL; the default patch bit-exact against FL's DLL, the 4 demo channels
  within −124 dB, 273 of 283 preset/demo states within −99 dB); PoiZone on its engine (REA round 44: voices, gate, arpeggiator, chorus and tape delay from the raw state;
  bit-identical to FL's DLL on 384 of 385 presets with the delay off and on the render test, the delay's low/high-cut
  coefficients differ by the CRT's low-accuracy sincosf: median −79 dB, worst −48 dB); Fruit Kick and BassDrum on their engines (REA round 43: Fruit Kick bit-exact with FL's
  DLL, release fades included (REA round 74: IPP's cosine restated, 441-entry fade at every rate), with FL's host waveshaper (its x87 live / file-render
  routines since REA round 55, checked against FLEngine's own) and DX10's shared voice processor, all 8 filter types checked; BassDrum bit-exact on 228/228 cases at 44.1 and 48 kHz with the engine's own
  CRT powf/sincosf, its table and noise samples read from the user's installation); Toxic Biohazard on its engine (REA
  round 45: bit-identical to FL's DLL on the 3 demo channels, voice and full effects chain, at 44.1/48/96 kHz, the
  built-in waves read from the user's FL installation); Autogun on Ogun's engine in its Autogun variant (REA round 57:
  master × 0.25 and the Magic Dust stage = FL's Soundgoodizer engine with `Autogun\Data\SG.dat` read from the user's
  installation; within −127 dB of FL's DLL on the 4 fixed-spectrum template states, 9/9 states usable); Transistor Bass
  on its engine (REA round 57: V1 legacy and both V2 cutoff ranges, 303 Pulse with the recorded cycles read from the
  user's engine DLL, distortion, delay, reverb and the 16-step sequencer; 55/128 factory programs bit-exact, worst
  −108.6 dB vs FL's DLL); SimSynth on its engine (REA round 64: the C++ engine behind the Delphi front-end, 12 voices
  with 3 table oscillators/noise, ring modulation, LFO, both envelopes, the 2-stage filter and the chorus, FL's old
  integer voice record; bit-identical to FL's DLL on all 76 installation states in every tested scenario); Wasp and
  Wasp XT as FL 2026 plays them (REA round 64: the 64-bit DLL is a placeholder — silent, state kept); Plucked! on its Karplus-Strong engine (REA round 65 §1: bit-identical to FL's DLL for the
  same random seed); BeepMap on its image-synthesis engine with 3x Osc's voice processor (REA round 65 §2: the bitmap
  read from the plugin state; the 9 preset/render-test states bit-identical, synthetic batteries identical outside the
  processor's end fades); Sawer on its engine with the oscillator cycle `data.saw` read from the user's installation
  (REA round 65 §3: PoiZone's framework, 6× oversampled filter, phaser and reverb; all 423 preset states bit-identical to
  FL's DLL with the effects off, as saved bit-identical except the reverb's −137…−155 dB residual); Fruity Granulizer on its engine (REA round 67: grains on the channel's sample with the library's 24-point windowed-sinc reader, grain
  envelope, stereo separation, randomness, wobble, freeze, loop, sample start, key modes, and the plugin voice processor;
  within −128 dB of FL's DLL on 100 random states/scenarios and on every installation state without slices); Drumaxx on
  its physical drum engine (REA round 67 §6–§9: 16 pads at 38 kHz — mallet and noise, 128 four-lane delay-line membrane,
  SOF, LOW, HIGH and enveloped MID sections, hold/release, lo-fi —, cut groups, velocity modulation, master EQ and
  limiter, with the engine's own C-runtime powf/sinf/cosf ported; all 133 distinct installation states bit-identical to
  FL's DLLs on random notes); FL Slayer on its engine (REA round 68: the 32-bit
  reFX VST engine FL runs in its bridge — six waveguide strings at 44.1 kHz with the recorded picks, chord/glide
  tables read from the user's `Slayer_Engine.dll`, the six play modes, 15 effects, four amps, EQ and cabinet, under the
  engine's x87 truncating rounding; bit-identical to FL's DLL on all 64 presets in every tested scenario and on 340
  random parameter sets at 22.05–96 kHz); Fruity Dance as a silent channel (REA round 68: a visual plugin); Sakura and Drumpad on physical models rebuilt from their
  engines (REA round 28), BooBass on its engine with the multisample read from the user's own FL installation
  (REA round 29; within −138 dB of the reference renders verified against FL's DLLs).
- Third-party plugins in the Fruity Wrapper are matched to installed VST3/CLAP plugins (VST3 class ID, CLAP id
  or name, preferring the format FL used) and their state is restored (VST3 and CLAP); automations of a VST3
  follow FL's persistent parameter slots (record 4 → ParamID; skipped with the plugin sandbox on).
- FL presets: 169 3x Osc channel presets, ~2800 `.fst` synth presets and the effect presets of the supported
  effects.
- An import report lists what was converted, approximated or left out.

## Confirmed vs approximated (FL import)

Every conversion rule comes from the behavioural analysis in `docs/analise/REA_rodada*.md` (decompilation,
the official manual and project data). Rounds 17–24 verified the earlier hypotheses and the code was fixed
accordingly. Highlights of what is **confirmed and applied**:

- **Sample players (REA round 69):** native sampler channels and audio clips read samples through FL's `TResampler`
  rules — index→kernel table, the 2-point linear and 6-point kernels (x87 extended precision) and the windowed sinc
  (IPP-seeded sine/cosine recurrences, SSE `rcpps`+Newton reciprocal), bit-identical to FLEngine's own kernels on
  synthetic signals (except positions within ~10⁻⁹ of a frame with the cutoff lowered, where FL's result depends on
  IPP's last-bit sine); FL's defaults (24-point sinc live, 32-point in the render dialog), speed formula
  `exp(cents·ln2/1200)·ratio`, width/cutoff widening above speed 1, the copy path at speed 1.0, edge frames repeated,
  loop wrapping and the end rule. The volume envelope of native sampler channels runs FL's articulator (the host's is
  the plugins' `flvp` one: delay, hold, tensions, tick steps, linear ramp per tick). FPC: velocity index
  `round(v·128)` and the "cycle" choice among overlapping layers (every demo pad that overlaps uses it). Slicex: master
  volume through FL's L11 law. FL Keys: FL's own engine (wavetable layout, raw repacking and guard samples, the
  mda-style voice and the global treble/tremolo/stereo stage), bit-identical to the plugin; Grand Piano's Ogg samples
  through LYNX's FL-exact Ogg decoder (round 77) match FL's 16-bit buffer on all 20 081 840 values. Fruity Slicer: state
  fields and the pre-stretch of its slices decoded (§7).
- **Native sampler voice (REA round 76):** imported native sampler channels run FL's whole voice — the velocity law
  (`(11^(v/100) − 1)/10`), the channel's volume and pan inside the voice through FL's sin/cos pan law (centre −3 dB),
  the five channel articulators stepped on the song's tick grid with FL's gain ramps, the channel filter (all 8 types,
  `flvp`), the 10 ms end fade after the filter, note-off releasing only with the volume envelope on; note events land on
  FL's rounded tick samples. Checked against FL's own command-line renders (`FL64.exe /R /Ewav`) of 17 crafted projects:
  every sample within one 16-bit code. WAV loop points of native sampler channels are not read yet.

- **Effector:** its own effect (`core/Effector.cpp`) with all 12 algorithms and FL's 12 raw parameters; LFO
  locked to the song, Mod Rate/Tempo Reduce, input/output gain; exact Distort cascade, Lo-fi, Delay (Mod Rate
  time), Stereo, Trancer, Stutter, Vowel and Ring; Filter, Flanger (tape delay) and Phaser exact — impulse
  responses within 6·10⁻⁸ of FL's own engine (REA round 27); Reverb exact (FL's own diffusion network, size and
  damping law; −100 dB or better against the DLL at 44.1/48/96 kHz, REA round 33).
- **Harmor:** its own instrument (`core/Harmor.cpp`, REA round 34; the channel keeps the plugin's raw state): per
  enabled part × unison voice a record with a 520-partial sine bank (self-oscillation, Sub 1/3/4, 516 harmonics,
  linear or logarithmic spacing, per-partial amplitude glide, partials entering after pitch drops), targets rebuilt
  every 2 ms control block from the two timbres through the part's user-ordered module chain — local EQ, prism
  (both modes, from level), harmonizer, filters 1/2 (shapes 0–7, 9, 14, custom 17/18, octave/Hz scale, key tracking,
  knob smoothing, envelope/adaptive envelope, serial/parallel mix) with resonance (100–108, custom 110/111, masks),
  phaser (200–210, incl. the frequency phaser), pluck, time and harmonic blur with mask, protection, clip modes 0–4,
  volume order — then subs below, self-oscillation, top fade, global EQ (with mix), auto-gain; the 43 articulators per
  part with FL's point curves (modes 0–12), envelopes (ADSR scales, sustain-level offset), LFOs (global phase,
  tension/skew/pulse width), key/velocity/X/Y/Z/random/unison maps and time smoothing; unison types 0–4 (random and Hz
  included) with the per-partial pitch curve; vibrato, stereo tremolo, legato/portamento/mono, master volume ramp;
  table reads nearest or interpolated by the precision setting. FL's level law (part gain·(Final.Vol + offset)·unison
  gain through √2·cos/sin, channel volume applied once — checked against the DLL at FL's default channel volume:
  0.000 dB). Bit-for-bit to float32 output rounding against the reference model on 88 of the 89 demo channels it
  runs (the 89th, −79 dB: numpy's float32 `exp`, which is not correctly rounded, in the per-partial unison curve) and
  on all ~31 synthetic feature cases at 44.1 and 48 kHz (the frequency phaser at precision 0: −96 dB, the same
  `exp`). The old spectral-oscillator conversion remains for unreadable states and for the synth's "FL presets" button.
  CPU: AVX bank kernel (8 partials per pass), running sine pairs across frequency changes (steps by complex rotation
  along the harmonic series), fast per-partial exp/log, cached k^m, idle records/channels skipped; "Square a Saw —
  Oxygen" (23 Harmor channels, 260 s) exports in 50 s, "JuiceBass — Menagerie" (8, 474 s) in 78 s.
  **REA round 165g (flbeta165h):** the note start delay (part parameter 2), the part attack fade, and precision
  "Average" with IPP's 11-bit Exp/Ln as FL's DLL computes them: 298 of the 325 factory presets null within −60 dB of
  the DLL, 19 more match their spectrum (STFT < −40 dB), 8 still audible (was 262 / 44 / 19) — see
  REA_rodada165g-flbeta165h.md §4; Harmor stays beta.
  **REA round 165g (flbeta165s):** the 88 old (version 11…15) factory presets, Default.fst included, are upgraded
  the way FL's loader does (byte-identical to the DLL's own re-save; a new Harmor starts from Default.fst), filter
  shapes 8, 10…13, 15 and 16, images of any size (Labyrinth's 16 × 12), portamento time following the velocity.
  **REA round 75:** image synthesis (the part's gain and frequency planes from the project: time position with speed,
  offset and smoothing, one-shot / loop / ping-pong / crossfade, the seven interpolation curves, gain pixel scale and
  mix, frequency modes Oct / Hz / Wide Hz / Unison with the local mask, formant shift with FL's per-column pitch track
  and the formant-mutation curve, records ending with a one-shot image), arpeggio envelope markers (chord order, the
  note playing each segment, the release waiting for the next marker), the noise resonance (type 109), "pluck
  replaces blur decay", phaser and harmonizer masks, timbre blend modes 1–5, the global envelope scales, the
  harmonic-randomness module, the per-block X/Y/Z smoothing. 97 of 99 demo channels within −60 dB of the DLL (88 within
  −70, median −80.1 dB; round 34: 84); the 8 image channels −77.7…−99.6 dB (74: −57.4, see below); the new features
  within −150 dB of the reference model and −65.9…−106.7 dB of the DLL on ~60 synthetic cases.
  **Effects rack** (REA round 38, `hlfx::HarmorRack` in `core/HarmlessFx.cpp`, plugged into `harmor::FxHook`): the six
  units in the state's order — distortion (types 1–10 and the quantiser, 2×/4×/8× half-band oversampling by precision,
  bias/drive/dry/wet ramps, 4th-order post low-pass, DC stage), the shared library's chorus, delay (modes, feedback ramp
  from 0.5, stereo offset, pan law, in-path or in-loop high/low cut) and reverb (the shared Reverb_x64 DSP with Harmor's
  low/high cut, predelay, room, diffusion, decay, damping, colour, wet ramp), the records' FX send joining at unit 4,
  and the Maximus compressor with Harmor's own preset read from the user's installation (globals 49–52 → mix and the
  three pre gains; the RMS detector of preset 2 decoded); the activity counter (30/530 ms + the chorus ring). Against
  the DLL with its rack on: per unit median −78 dB (distortion −77.6, chorus −78.2, delay −79.9, reverb −79.6,
  compressor −78.3), level 0.00 dB on every channel whose voice is modelled.
- **Sytrus:** its own engine (`core/SytrusEngine.cpp`, REA round 31) driven by the plugin's saved state: 16 384-point
  operator tables with linear read-out and the 4-lane block reader, noise (shared LCG), plucked mode (IPP's uniform
  generator), band limiting (4 096 live / 16 384 export), CENTER/DECLICK, GLOBAL phase; the FM/RM matrix in FL's
  per-sample order (PM `4·g·out·MOD`, feedback `g·35/128`); every articulator (envelopes with all curve shapes 0–9,
  LFOs, key/velocity/unison/Mod X/Y/random maps, TEMPO, GLOBAL LFOs, global ADSR, velocity options) at FL's control
  tick; volume/MOD/pan ramps, output pan law and the voice-end ramp; unison (4 modes, sub layer, OSC grouping),
  portamento, GLOBAL PITCH, MONO KEY; the 3 filter units (13 types, ALT, HQ 2×, FLAT, waveshaper) and the post-voice
  chain (decimators, chorus, 3 delays, the shared reverb, 3-band EQ, DC stage). LYNX uses the state's *render*
  oversampling/tick settings while exporting (as FL does) and the live ones while playing. Against FL's own DLL on
  the 268 demo channels (one note, 0.5 s): median −116.9 dB live / −122.7 dB with the export settings; 265 / 264
  channels within −60 dB (268 / 267 when fed the DLL's own sine tables — the rest are chaotic FM patches); four
  overlapping notes with a key re-trigger (voice management, MONO KEY): 262/268 (REA round 31 §10).
  CPU: idle effects bypass themselves (exact), voices on the same tick grid are rendered in lockstep and the block
  reader runs in SSE2 — all bit-identical; "Overcome – Sytrus" (72 instances, several at 8× even live) plays in
  0.46× real time on one thread, 0.17× on four. The channel settings panel shows the plugin's "Main settings" page
  (REA round 74 §3): master level/LFO/pitch, the global volume and filter A/D/S/R, unison, Mod X/Y, PORTA and the
  live/export oversampling and envelope accuracy, written into the saved state (FL's DLL reads the edited state).
- **Harmless:** its own instrument (`core/Harmless.cpp`, REA round 32): the additive engine with up to 528 sine
  partials per voice record (self-oscillation, Sub 1/3/4, 524 harmonics; partial count and band limit per quality,
  10 ms amplitude glide), targets rebuilt every control block (88/44/8 samples at 44.1 kHz) in FL's order — colour ×
  6-band EQ, harmonic mask (+LFO), harmonizer (4 positions), per-partial filter (17 shapes, octave/Hz scale, key
  tracking, knob smoothing, tension-curve filter envelope), resonance (8 shapes, adaptive width, track motion,
  self-oscillation partial, Noise gates), phaser (10 tables, 3 scales, Freq type), pluck, low-harmonics protection;
  volume envelope with FL's smoothers and the 30 ms end/steal fade, all LFO shapes (global/retriggered, attack),
  6 Hz vibrato, stereo tremolo, unison (detune tables, pan, phase, attack, LFO offsets), portamento/mono/legato,
  grittiness, both phase-randomness modes; FL's level law (`0.75·(3^x − 1)/2`·Final.Vol·unison gain through the
  cos/sin pan law, channel volume applied once). Its effects rack (`core/HarmlessFx.cpp`, REA round 36) runs on the
  voice mix in FL's order: chorus (8 records of FL's chorus library, ⌊D⌋ taps), delay (normal / ping-pong with stereo
  offsets, tempo-synced time, feedback ramp), reverb (FL's shared plate reverb with 6 records) and the compressor =
  FL's Maximus engine (low cut, ×2 oversampling, Linkwitz–Riley crossovers, peak-window detector, √-scaled attack /
  release / release-2 stages, drawn-curve gain tables with tensions, lookahead, saturation, band mix) loaded with
  `Harmless/Data/Compression/<n>.fst` from the user's installation. The old synth approximation remains for unreadable
  states and for the synth's "FL presets" button.
- **Ogun:** its own instrument (`core/Ogun.cpp`, REA round 37; the channel keeps the plugin's raw state): per voice
  up to 32 767 partials (Richness) with seeded IPP-style weights, fullness shaping, morph, Forced (B·(1 − F) + F),
  Impulse/Decay/Release per-partial maps (sequential curve sampling, fixed patterns, 1/√h, logarithmic axis, EQ shift),
  pre-decay, RMS normalisation, band limit and update span; per 50/25 ms frame the spectrum follows the hold, ALT DECAY
  or release law and becomes an N-point table by an inverse real FFT (N = 2^(P52+q)); the two latest tables are read
  with the quality's kernel (linear, 6-point 5th-order Hermite, 32-point windowed sinc), crossfaded with the unison
  sub-voices' pan gains and phase spread (read start at the table's smallest sample), then declick, the 4-type ×
  1–3-stage filter (2× zero-order hold below 88.2 kHz), FL's volume/pan matrix with gain ramps, silence end, same-key
  cut and max polyphony; the 13 articulators (point curves modes 0–12, envelopes with ADSR knob scales, tempo sync,
  velocity options and portamento, LFOs with tension/skew/pulse width, key/velocity/X/Y maps); the plugin's 3-band EQ,
  chorus, delay (normal/inverted/ping-pong, stereo offset, tempo sync) and reverb (FL's shared plate reverb,
  `core/PlateReverb.h`, shared with Harmless's rack), and the 2:1 polyphase decimator of qualities 2–4. Level law
  `(3^(P0/128) − 1)/2`·Final.Vol through Ogun's own pan matrix (channel volume/pan applied once). LYNX plays the
  state's quality and exports at quality 4, as FL does when it renders. Against the round-37 reference model (own
  read start, same Seed-2 counter): bit-identical or within −125 dB (float rounding) on all 20 demo channels (44.1 and
  48 kHz, every effect on, and at the export quality 4) and on 55 synthetic cases (qualities 0–4, richness, unison,
  filter types, ALT DECAY, 1/√h, patterns, velocity options, portamento, host pan/volume/pitch/Mod X-Y, 48/96 kHz); the
  model itself is within −60 dB of FL's DLL on 20 of 20 demo channels. The core tests check the engine against model
  fingerprints: the 20 demo channels at the state's quality and at quality 4 (`ogun_demo_channels`) and 17 feature
  variants of a state written from scratch (`ogun_engine`). The old spectral-oscillator conversion remains for
  unreadable states and the synth's "FL presets" button. Autogun (REA round 57) uses the same engine on its state variant
  (a 4-byte seed after the comment from version 19 on; the comment length is one byte or 0xFF + i32, which also fixes
  Ogun states with comments of 128+ characters), master × 0.25 and the Magic Dust stage (`autogun_template_channels`).
  CPU: one inverse FFT of N per voice and frame (split re/im radix-2 passes, cache-blocked, AVX when available), the
  quality-4 sinc reader vectorised with per-sub-voice tables, finished voices removed and idle channels skipped;
  "Electric Loop – The Ogun Dream" (14 Ogun channels, 96 s) exports at FL's quality 4 in ~15–22 s (the old
  approximation: 4.9 s), the other demos with Ogun channels within ±6 s of before.
- **MiniSynth:** its own instrument (`core/MiniSynth.cpp`, REA round 41): FL's wrapper (velocity from Init.Vol,
  per-note FinalLevels tables, engine calls of ≤ 512 samples in 32-sample blocks, voice kill), POLY/MONO/LEAD voice
  allocation with glide from the last note, the ten oscillator kinds as phased/detuned table saws with dithered reads on
  1024 band-limited tables built at run time from the user's `Minisynth\oscdata.dat`, noise, drift, the 4-pole filter
  integrated exactly per sample (4096 sub-steps by 12 matrix squarings) with its saturating first stage and DC blocker,
  both envelopes, the engine LFO (song-locked while the transport plays) and the effects (distortion + decimator, tempo
  delay, 6-stage phaser, chorus) — bit-identical to `rea-work/r41/py/msmodel.py` on all 12 installation states and 91
  presets, and bit-identical to FL's DLL on 11/12 and 47/91 (the others within −107…−147 dB: last-bit differences of
  the C runtime's sin/pow). Not reproducible by design: the random generator is process-global in FL (LYNX: one per
  instance, started like a fresh plugin). FL's max-polyphony value at channel creation is not confirmed (LYNX: 0 → 99
  voices). Without `oscdata.dat` the importer keeps the internal-synth approximation.
- **Toxic Biohazard:** its own instrument (`core/Toxic.cpp` + `core/ToxicFx.cpp`, REA round 45): FL's wrapper
  (velocity from Init.Vol, per-note level/pitch tables, engine chunks of 64), poly/mono voices with the 4 glide modes and
  stealing, six operators × up to 8 unison lanes on band-limited tables built from the 128-harmonic spectra (wave 0 from the
  state, waves 2…32 read from the user's `Toxic Biohazard_x64.dll`, SHA-256 checked), the FM matrix (operator i → j, one-sample
  delay), operator envelopes and Catmull-Rom level ramps, LFOs, the modulation slots (velocity/LFO sources), FL's ladder with
  its filter envelope, the master envelope, the output rack (distortion + DC blocker, two slots of delay/chorus/reverb/
  flanger/phaser/lo-fi, 8-band EQ, sleep logic, the DLL's CRT `powf/sinf/cosf/expf/tanf`) and the master gain —
  bit-identical to `r45/py/toxmodel.py`, which is bit-identical to FL's DLL on the 3 demo channels (voice and full chain,
  44.1/48/96 kHz) and 35 variants (features, rates, block sizes). The 2-operator conversion stays for states that need the DLL when it is not found.
- **Kepler:** its own instrument (`core/Kepler.cpp`, REA round 46; the channel keeps the raw state and the path of the
  user's `engine_x64.dll`, whose LFO-shape and noise tables are read at run time, SHA-256 checked): FL's wrapper
  (velocity from Init.Vol, same-key release, pan/pitch/X/Y expressions from the Final levels), 8 always-running voices
  with the band-limited DCO at 2× (saw, PWM pulse, sub, noise), the saturating 4-stage ladder, envelope or gate VCA,
  glide, key follow, the voice allocator, Hold and the internal arpeggiator (up, up/down, down; 1/2/4 octaves; free or
  synced), LFO1 (shape table, delay ramp, trig mode, BPM sync from the song position), the half-band decimator, the
  high-pass, the Juno chorus with its mode cross-fade and the main volume/pan — every float operation in the engine's
  order and rounding mode (toward zero in the render). Checked against FL's DLL (`kepler_engine`: the default patch
  bit-exact; `kepler_demo_channels`: the 4 demo channels). The synth conversion (`synthFromKepler`) stays the fallback
  (Kepler Exo / "Kepler-2" states, gate/noise/×4 bytes, DLL missing).
  **BassDrum** (fallback when the engine can't run): Peak `1 + 100x³·s⁴`.
  **Sakura:** ADSR, Tune, Mix, Hi-Cut. **BooBass:** 3-band EQ. **Drumpad:** Pitch and Mallet Amplitude.
- **GMS:** its own instrument (`core/Gms.cpp`, REA round 39 §1): FL's wrapper (velocity from Init.Vol, per-note
  FinalLevels tables, chunks of ≤ 512 samples, voice kill), the 99-voice engine with the MIDI matrix, 512
  band-limited tables per wave built from `GMS\Synth Waves\<name>.wav` of the user's installation, dithered
  oscillators with sync/FM/AM, unison, noise, slide, both LFOs (song-locked when synced and the transport plays), the
  4-iteration ladder with its 65 536-entry table, the ten effect slots (distortion, crusher, flanger, phaser, LP, HP,
  tape delay, plate reverb, panner, trancer) + 5-band output EQ and the master level/pan — bit-identical to
  `rea-work/r39/py/gmsmodel.py` on the 10 demo/render-test channels (voice and full chain, 44.1/48 kHz) and 33
  synthetic cases. Not reproducible by design: GMS's random generator is process-global in FL (shared by every
  instance), so noise, dither and the random start phases of fresh voices differ from any given FL render (LYNX starts
  one generator per instance like a freshly created plugin). FL's max-polyphony value at channel creation is not
  confirmed (LYNX uses 0 → 99 voices). Without the wave files the importer keeps the 3-oscillator approximation.
- **Fruity DX10:** its own instrument (`core/Dx10.cpp`, REA round 42): the voice (two parallel sine modulators on
  the host's sine table with Init → Sustain → release level glides, the Waveform shaper, Mod Thru, the 1.2/peak
  normalised amplitude envelope, the plugin-wide vibrato, voice stealing, re-tuning on Final.Pitch changes) and the
  plugin voice processor DX10 keeps: the channel's five INS articulators stepped per sequencer tick (never released,
  as in FL), the eight filter types on the x87 path, FL's x87 pan law with its gain ramps and the 441-sample end fade —
  bit-identical to our round-42 reference model (`r42/py/dxmodel.py` + `dxproc.py`), which matches the DLL bit for
  bit on 64/64 states, 12/12 project channels with their settings and the synthetic batteries. Version-1 states take
  the channel's 0xDA/0xDB records as their settings (FL's host hands them to the plugin: inference); the project's
  PPQ sets the tick rate.
- **PoiZone:** its own instrument (`core/PoiZone.cpp`, REA round 44; the channel keeps the plugin's raw state): FL's
  wrapper (velocity from Init.Vol, per-note FinalLevels tables, chunks of 64 samples), the 1 024-voice list (poly with
  stealing, mono legato with return to the nearest held key, 4 glide modes, unison 1–16 with detune/pan, octaver, analog
  drift), per-chunk envelopes/LFO/control values, the 8×-oversampled voices (saw/pulse phases, sync, ring, noise, soft
  clip, 4-stage filter LP/BP/HP, DC blocker; one voice or four SSE lanes with their own operation order and the reversed
  write-back of the oscillator levels), the decimator, the 16-step gate, the arpeggiator (4 modes, 1–4 octaves), the
  12 kHz shelves, the 3-line chorus and the stereo tape delay — bit-identical to `rea-work/r44/py/pzmodel.py`, which is
  bit-identical to FL's DLL on the render test, 384 of 385 presets with the delay off (one `powf` near-tie: −68 dB) and 10
  synthetic cases at 44.1/48 kHz. With the delay on, its low/high-cut coefficients use correctly rounded sin/cos where
  FL's CRT uses a low-accuracy vector sincosf (318 presets: median −79 dB, worst −48 dB; demo channel −72 dB). Not
  reproducible by design: PoiZone's three random generators are process-global in FL (LYNX starts one set per
  instance), so noise, drift, start phases, random LFO and random arpeggios differ from any given FL render; FL's
  max-polyphony value and its state/rate order at creation are not confirmed (LYNX: 64 voices, 1 when the channel is
  mono; state then rate).
- **3x Osc:** its own instrument (`core/ThreeOsc.cpp`, REA round 47) on the plugin voice processor shared with DX10
  (`core/FlVoiceProcessor.h`): the host's six wave tables recomputed from FLEngine's formulas (sine, triangle, square,
  saw, the exponential "rounded" shape, MT19937 noise), 32-bit phases with the DLL's pitch law, the right channel's
  detune and phase offset, Delphi-Random start phases and noise steps, the mix/invert/AM levels split by FL's host pan
  law, the DLL's x87 mixers (64-bit precision), the custom wave (the channel's sample, resampled to 16384 points), the
  HQ oscillators of `engine_x64.dll` (44 band-limited levels from an FFT, sinf sine, LCG noise), and the processor's
  release path (envelopes released from their value, the 10 ms end fade when the volume envelope is off), its rate
  ratio and ramp limit — bit-identical to our round-47 reference model (`r47/py/osc3model.py`) on 650 jobs (every state,
  4 rates/block layouts, random notes), which matches the DLL bit for bit on 239/239 non-HQ states at 44.1/48/96 kHz,
  43/43 processor cases and the custom wave. Every state version is normalised to version 14 with the channel's
  0xDA/0xDB records as its settings.
- **Morphine:** its own instrument (`core/Morphine.cpp`, REA round 39 §2): the whole Mor0 state tree, 32 voices
  (poly/stealing, mono legato with return to the nearest held key, 4 glide modes), control blocks of int(0.0029·fs),
  Eg1–4 with loops and the 12-slot matrix (incl. the Eg-amount destinations), master ADSR, 4 generators (keyboard
  zones, frame player, PWM) through the morph line into the 128-partial coupled-form bank with its Catmull-Rom
  envelope, the noise sample player (loop modes, filter) and the effects overdrive → chorus → delay → reverb → EQ —
  bit-identical to `rea-work/r39/py/mormodel.py` on the 7 demo channels (voice and full, 44.1/48 kHz) and 22
  synthetic cases. Not modelled (unused by FL's data): release loops, unison (hidden parameters). LYNX creates the
  instance at its playing rate, so the rate-change quirks of REA round 39 §2.13 do not apply; FL's max-polyphony value
  at creation is not confirmed (LYNX uses 0 → 32 voices). A missing noise sample leaves the noise silent (reported).
- **Love Philter:** FL's exact engine as a mixer effect (`core/LovePhilter`, REA round 66): the whole state (8 units,
  their 8 articulators with the selected pattern envelope, LFO and X/Y/follower maps, waveshaper curves), FL's
  oversampling (×1…×16, doubled when exporting) through the half-band cascade, the control tick with its block ramps,
  the twelve filter kernels (Chocolate SVF, Lime, the eight Vanilla biquads, Mango with drive) in their precision,
  NEXT chaining, solo, the waveshaper, the input/unit-1 envelope follower, Mod X/Y smoothing, FL's pan law and CENTER;
  pattern envelopes and LFOs are positioned at the song position when the transport starts or jumps. FL's controls
  stay editable and automatable (48 of them: globals, cutoff/resonance/envelope amount/pan/volume per unit).
  Bit-identical to the installation's DLL on all 86 states of the installation (39 demo slots + 47 presets) in live
  and export modes, on Linux and Windows builds, with automation, transport positions and 22–96 kHz. The DLL's own
  Delphi math runtime (exp, ln, sin, cos, power, tanh) is re-created (`core/DelphiMath`, shared with the Fruity Vocoder since round 78). The old parametric-EQ
  conversion remains only for states the engine cannot read.
- **Patcher:** the real graph (REA_rodada141): every node, cable, gain, note and control cable, positions, side-chain
  pin, extra outputs; FL's "Patcher (effect)" / "Patcher (generator)" render tests match FL (0.01 / 0.04 dB).
- **Sidechain:** level-0 routes are sidechain links; Fruity Limiter (compression section) and Fruity
  Compressor with a side input listen to the source track (their exact engines below).
- **Fruity Limiter and Fruity Compressor:** FL's exact engines as mixer effects (`core/FruityDynamics`, REA round 48),
  editable with FL's own controls. Limiter: gain, RMS compressor section (ratio/knee laws, curve stages, side input),
  peak window + look-ahead limiter with release/attack curves, noise gate, soft saturation or clip, silence bypass,
  x87-precision stages; look-ahead reported to PDC. Compressor: DC blocker, peak follower, the eight knee types incl.
  Vintage and "/R" dynamic release, makeup and the fixed 64-frame look-ahead brick wall. Compressor bit-identical to
  its DLL on all 70 unique installation states; Limiter bit-identical on 101 of 272 and within −129.5 dB on all (median
  −151 dB; the residual is IPP's vector log/exp/sqrt in the compressor section).
- **Fruity Delay Bank:** FL's exact engine as a mixer effect (`core/FruityDelayBank`, REA round 56) built from FL's
  saved state, which the slot keeps (the four global controls — dry, output, input, feedback — are its editable
  parameters): eight banks with input/output pan matrices, sends between banks, cookbook biquad cascades before/after
  the delay and in the feedback loop (low/high-pass, band-pass, notch, shelves, peaking; up to three sections),
  tempo-synced or free times with a stereo offset, feedback pan/level/cross with normal / swapped / ping-pong routing,
  the stepped raised-cosine gain pattern, solo, FL's gain ramps, and the optional 2× half-band oversampling.
  Bit-identical to the DLL on all 52 unique installation states at two tempos/rates/block sizes, on control and
  tempo changes during playback, and on 96 random states.
- **Fruity Multiband Compressor:** FL's exact engine (its `comp_eng_x64.dll`) as a mixer effect (`core/FruityMultiband`,
  REA round 56), editable with FL's 28 controls: three bands split from the input on their own by 4th-order IIR
  crossovers or, in linear-phase mode, 105-tap Kaiser-windowed FIR filters (52 frames of latency reported to PDC), a
  peak compressor with knee and release curve and a gain per band (compress / off / bypass), output gain and the soft
  output limiter, the engine's silence rule, and its runtime's `sinf`/`cosf`/`powf` results. Bit-identical to the DLL
  on all 55 unique installation states (two rates/block sizes, silence gaps), 60 random states and control changes.
- **Pitcher:** FL's DSP (REA round 30, bit-exact against the reference model): integer-lag AMDF detector,
  nearest-allowed-note correction with FL's laws (speed, Fine, A reference, the note mask stored in the state, Key
  Bypass, range, Pitch Reference L/independent/R), plain 4-head shifter (9-sample latency) and Formant/Gender
  PSOLA path (257-sample latency), Harmonize and MIDI masks; latency reported to PDC.
- **Fruity Vocoder:** FL's exact engine (`core/FruityVocoder`, REA round 70): band edges and RBJ band-pass banks
  (formant, invert, order 1–3), the x87 envelope followers with FL's silence logic, per-band levels kept in the slot's
  FL state and drawn in the mixer's band-level strip, mix levels, swap, hold; mono output. Bit-identical to the DLL on
  every installation state, 84 rate/block cases and 500 random states/automation runs, and (round 78) on states whose
  raw controls lie outside the knob ranges (FL uses them unclamped; the slot keeps them while the knob is untouched).
  The set-up uses `core/DelphiMath`, the one exact replica of the Delphi RTL's math (exp / expm1 / ln / SinCos / power /
  sinh / tanh; rounds 66, 70 and 78: 0 differences against the DLLs' own routines).
- **Frequency Splitter:** FL's own engine (`core/FrequencySplitter`, REA round 62) from the saved state, editable as
  LYNX parameters: IIR crossovers for every slope (6–96 dB/oct, incl. the 18 dB/oct all-pass structure), the 3-band
  phase compensation, gain smoothing, mutes/solos and the FFT linear-phase mode (3N/4 latency to PDC); mixer slots use
  the summed output, Patcher buses the bands sent to them. IIR mode bit-identical to the DLL on all 25 IIR
  installation states (summed output and every band send, 44.1/48 kHz, automation); FFT mode within −113…−155 dB on
  its 6 states (the engine's single-precision IPP FFT is not replicated).
- **Maximus and Soundgoodizer:** FL's exact Maximus engine as a mixer effect (`core/MaximusFx`, REA round 40) —
  RMS detector, saturation A/B, state versions 3–15, solo, legacy linear-phase FIR split, x87-precision smoothers;
  Soundgoodizer runs the older engine copy of its own DLL with the preset `a…d.dat` read from the user's installation
  at import and Amount as the band mix. Within −139 dB of Maximus_x64.dll on 96 of the 100 demo states (53
  bit-identical) and within −126 dB of Soundgoodizer_x64.dll on every preset/amount; latency reported to PDC.
- **Fruity Reeverb 2:** FL's exact engine (`core/Reeverb2`, REA round 49) — the "full" process of FL's shared plate
  reverb (input filters and diffusers, early-reflection line, predelay, figure-of-eight tank with a sine-modulated
  delay in each half, bass multiplier/crossover shelf, damping), FL's level laws, separation matrix, Mid/Side input,
  tempo-synced predelay and per-block gain ramps; FL's 15 controls stay editable. Bit-identical to the installation's
  DLL on all 293 distinct installation states (44.1/48/96/22.05 kHz, blocks 64–4096) and on synthetic states and
  automation of every control; the Freeverb mapping remains only as a fallback for unreadable states. Fruity Reeverb
  (the first version, its own copy of the plate) likewise exact (`EffectKind::reeverb1`), bit-identical on its 7 demo
  states and synthetic/automation cases.
- **LuxeVerb:** FL's exact engine (`core/LuxeVerb`, REA round 60) — the shimmer-reverb engine of FL's shared DSP
  library: envelope follower (→ wet or → decay, sidechain), input filters, gliding predelay and all-pass diffuser,
  16-line modulated delay network with rotation matrix and per-line damping, freeze/sustain, granular pitch shifters
  in the feedback path or as two feed-forward voices, limiter, output peak, width and dry/wet, with the library's own
  float routines and rounding. FL's 49 controls and 4 sync switches stay editable. Bit-identical to the installation's
  DLLs on all 23 installation states (44.1/48 kHz), on 60 random states over 22.05–192 kHz and with automation of
  every control; the generic reverb mapping remains only as a fallback for unreadable states.
- **Fruity Convolver:** FL's impulse-response pipeline and convolution (`core/ConvolverFx`, REA round 60) — the
  embedded WAV or the installation's impulse file (WAV/WavPack, read at import and kept in the imported project),
  normalisation, volume envelope, blend (self-convolution), speed (sinc resampling), flat EQ level, trimming, predelay,
  zero-latency partitioned convolution, FL's dry/wet/separation matrices, gain ramps and silence gate; the 4 controls
  stay editable. Within −127…−152 dB of the DLL on its 13 decodable installation states at the impulse response's own
  rate (−95…−117 dB where a 48 kHz file is resampled to 44.1 kHz); states with a shaped EQ curve keep the approximation.
- **Fruity Parametric EQ 2 and Fruity Parametric EQ:** FL's exact engines inside LYNX's parametric EQ
  (`core/ParametricEqFl`, `core/ParametricEqLinear`, REA round 50), so the EQ editor keeps working with FL's own values
  (width, Steep/Gentle orders, enable mask, HQ options): band design with the gentle/steep stage tables, double-precision
  DF-I biquads with FL's state clean-up, ×2 polyphase half-band HQ, the Frequency Splitter resampler HQ (zero-stuffing +
  176-tap FIR, 44-sample latency), the "alternative" magnitude-matched design, linear phase (FFT FIR from the sampled
  response, cross-fades, 3N/4 latency), main-level glide and silence gate; v1's own laws (one biquad per band). On all
  1 138 + 9 installation states: the IIR modes and v1 bit-identical on 912 of 1 139 and within −166 dB on all,
  linear phase within −127 dB (IPP FFT rounding).
- **Hardcore:** FL's exact engine (`core/HardcoreFx`, REA round 71) — five pedal boxes (11 pedals: distortion Model A,
  chorus, flanger, phaser, reverb, delay, noise gate, equalizer, compressor, modulator, tube drive), cabinet convolution
  with the installation's impulses (carried in the slot), 8-band EQ, levels; bit-identical to `Hardcore_x64.dll` on both
  installation states, random chains at four rates, automation and pedal switching. Shares Toxic Biohazard's building
  blocks (`core/IlFxKit.h`).
- **Distructor:** FL's exact engine (`core/DistructorFx`, REA round 71) — the chain of up to four modules
  (Module_x64: 18 distortion algorithms with ×2…×16 oversampling and DC blocker, 22 filter types, the chorus on LYNX's
  Fruity Delay 3 engine, amp/cabinet convolution with the installation's impulses carried in the slot) and the final soft
  clipper; bit-identical to the DLL on every module type except the Harmor shapes (IPP vector math: last-bit
  differences, which the bit-jam quantizer can turn into step flips, −55 dB on one preset) and the cabinet (float FFT
  in FL, float accuracy in LYNX); the 4 installation slots −∞ or below −127 dB.
- **Multiband Delay:** FL's exact engine (`core/MultibandDelayFx`, REA round 71) — 16 bands × 8 banks of delay /
  volume / pan, morph, feedback, keep pitch, gentle / steep / linear-phase crossovers (1536-sample latency), saturation;
  bit-identical to the DLL with the IIR crossovers, linear phase within about −120 dB (IPP FFT rounding).
- **EQUO:** FL's exact engine (`core/EquoFx`, REA round 61) — 31 third-octave band passes with FL's design, the eight
  banks plus master bank, band shift / mix / morph laws, per-band volume, pan and add mode with FL's float gain ramps,
  the doubled top bands, ×2^n half-band oversampling, FL's render-mode quirk (bands designed for 2× the rate), silence
  gate, output volume and the per-tick motion smoother; knobs and options stay editable. Bit-identical to the DLL on all
  10 installation states (44.1/48 kHz, render flag) and on synthetic states, other rates, NewTick and knob changes. The
  send output to another track is not routed.
- **Pitch Shifter:** FL's exact engines (`core/PitchShifterFl`, REA round 61) — both live in `Shared/ildsp_x64.dll`: the
  "FFT" mode is a pitch-synchronous overlap-add shifter with an FFT pitch detector (formant shift/correction, absolute
  pitch modes, latency to PDC), plus the granular shifter, the delay modes and the run-time tanhf/expf. Bit-identical to
  the DLL on all 15 installation states and 70 random states (rates, blocks, signals, every option) and on automation;
  FL's 13 knobs stay editable. The granular engine's random sequence is shared by all instances in FL (one global) —
  LYNX gives each instance its own.
- **Fruity WaveShaper and Fruity Soft Clipper:** FL's exact engines as mixer effects (`core/FruityShapers`, REA round
  51). WaveShaper: the curve from FL's saved map (load-time normalisation, all 13 segment modes, FL's incremental
  evaluator, 16-bit table), pre/mix/post laws, nearest and interpolated x87 kernels (the latter rounding toward zero),
  polyphase IIR half-band oversampling ×2…×16, per-sample smoothing, CENTER DC removal; exports use FL's rendering
  behaviour (interpolated table, ×16 when oversampling is on). Soft Clipper: FL's exponential knee in x87 precision
  and its post law. Controls are editable LYNX parameters. Bit-identical to both DLLs on every installation state
  (57 WaveShaper states live and rendering, at 44.1 and 48 kHz; 12 Soft Clipper states plus edge cases).
- **Fruity PanOMatic, Fruity Balance and Fruity Blood Overdrive:** FL's exact engines as mixer effects
  (`core/FruityBasicFx`, REA round 58). PanOMatic: pan/volume laws, its 2×2 pan matrix, the LFO on FL's host wave
  tables (sine/triangle/square, free running in samples — tempo is never read) stepping once per render call, FL's gain
  ramps; Balance: its sine/cosine balance law with the cross terms, FL's ramps; Blood Overdrive: the per-sample state-
  variable filter, integrators, 65536-point sine shaper, smoothers and post filter, settled at load as FL does. Controls
  are editable LYNX parameters and take FL's automation. Bit-identical to the three DLLs on every installation state
  (54 PanOMatic, 10 Balance, 19 Blood Overdrive) plus synthetic states, at 22.05/44.1/48/96 kHz, several block sizes,
  four signals and control changes during playback.
- **Fruity Fast LP, Fruity Free Filter and Fruity Filter:** FL's exact engines as mixer effects
  (`core/FruityFilters`, REA round 63) from the saved controls, editable/automatable as LYNX parameters. Fast LP: its
  two-state resonant low-pass in x87 precision with the per-call cutoff glide; Free Filter: the seven RBJ-style types
  designed at a fixed 44.1 kHz, x87 direct form I, CENTER DC remover; Fruity Filter: the Chamberlin SVF (×2 cascade),
  per-call smoothers, CENTER. Bit-identical to the three DLLs on every installation state (7 Fast LP, 2 + 9 Free
  Filter, 19 + 4 Fruity Filter demo/preset states) plus synthetic ones, at 22.05/44.1/48/96 kHz with automation, for
  render calls of up to 512 frames. (Low Lifter was ported exactly in round 154; Emphasis is still approximate.)
- **Emphasizer, Frequency Shifter and Low Lifter:** FL's exact engines (`core/EmphasizerFx`, `core/FreqShifterFl`,
  `core/LowLifterFl`, REA round 154) imported from the saved state, editable/automatable, with FL's latency (2 ms
  Emphasizer lookahead, 10 ms Low Lifter lookahead when enabled) reported for delay compensation.
- **Newtone and Newtime:** imported as mixer effects that keep FL's state (`core/NewtoneFx`, REA round 73). Like FL,
  they pass the insert's input through unchanged (bit-identical to the DLLs); with FL's "slave to host playback"
  switch on (an editable LYNX parameter) they add their source sample from the song time while the song plays —
  identical to Newtone's output for an unedited sample played from the song start at the sample's rate (−138 dB).
  The source sample comes from the path FL stored (or the engine's working copy), or is chosen in the slot menu.
  Newtone's note edits are rendered by LYNX's own engine (REA round 95: pitch offsets with FL's note transitions, drift,
  vibrato, formant factor, volume / fade-in, timing and ownership, disabled notes; YIN analysis + TD-PSOLA; close, per
  note within ~0.01 st of FL's render). Not reproduced: Newtime's time warping, Newtone's tremolo / ramps and the
  whole-sample re-alignment FL gives later notes after an edit, its pitch-mark alignment when playback starts mid-song,
  and its behaviour at other project rates (LYNX resamples linearly).
- **LYNX Tune** (own design, REA round 95): real-time vocal pitch corrector on any insert — key / scale / note switches,
  retune, humanize, formant preserve / shift, transpose, A4 reference, voice range, low-latency mode, MIDI target notes
  from a Channel Rack channel (`Effect::setHeldNotes`); YIN + TD-PSOLA from the literature only (`core/VocalPitch`,
  `core/LynxTune`).
- **Fruity Delay 2 and Fruity Delay 3:** FL's exact engines as mixer effects (`core/FruityDelay`, REA round 52),
  FL's raw controls as editable parameters. Delay 2: time in 1/192 beat, input pan/volume ramps, stereo offset,
  normal/swapped/ping-pong loop with its one-pole cutoff in x87 precision. Delay 3 (the hosted third-party engine):
  synced (1/192 beat) and unsynced time laws (cubic 1 ms…1 s), time smoothing glides or keep-pitch crossfades, LFO
  modulation (the engine's FMA sine), diffusion all-passes, 10 Hz feedback high-pass, state-variable filter with
  cutoff LFO, limit/saturate distortion, rate/bit reduction, colour, stereo spread, the three delay models and "off".
  Bit-identical to the installation's DLLs on every installation state (60 + 85) at two tempos, at 48 kHz (one
  Delay 3 state at −151 dB) and on 110 random Delay 3 states incl. tempo changes; LYNX starts them settled.
- **Stereo Shaper, Stereo Enhancer, Transient Processor, Spreader:** FL's exact engines (`core/StereoFx`, REA round
  54) — Shaper: (3^x − 1)/2 matrix law, delay, all-pass dephasing, pre/post order, the In/Out difference for Patcher
  buses; Enhancer: pan/volume/separation matrix, phase-offset delay, inversion; Transient Processor: its split,
  detector, smoothing every 8 samples, saturator and 1.5 s silence bypass; Spreader: the Faust-built engine of
  `Shared/ildsp_x64.dll` (TPT split, crossfading width delay, 15 kHz filters, mono bass). Bit-identical to the DLLs
  on every distinct installation state (104 / 30 / 57 / 25) at 44.1, 48, 96 and 22.05 kHz.
- **Fast Dist, Flangus, Flanger:** FL's exact engines as mixer effects (`core/FlangusFastDist`, REA round 55), with
  FL's controls as editable parameters. Fast Dist runs FL's host waveshaper (the 20 FLEngine curves rebuilt from their
  formula; FL's live routine for playback, its file-render routine for exports); Flangus its x87 voice set-up and
  parabolic-LFO voices with FL's gain ramps; the Flanger its fixed-point LFO and damped-feedback lines. Bit-identical
  to the DLLs on every demo state (31 / 18 / 3), synthetic edge states and automation, Linux and Windows builds.
- **Vocodex:** its own engine (`core/Vocodex.cpp`, REA round 35) built from FL's raw state, which the `.lynx`
  project keeps: band layout and the 13 per-band maps, 64-bit band-pass banks (orders 1–4, flatness, 2×
  oversampling above 11.05 kHz), PEAK/RMS followers, gate, saturation, double-precision smoother, unison, IPP-style
  carrier noise, consonant/carrier contour, mod/car pass-through filters and FL's gain ramps; ModIn/CarIn routing and
  L-R encoding. Bit-exact against the reference model (which is within −137 dB of FL's DLL) on all 77 comparison
  cases, the 10 demo instances included. Draft follows the stored button for playback and export alike (LYNX has
  no "HQ for all plugins" option): octave levels and 16-frame re-blocking, its 16 samples of latency reported to PDC.
- **Gross Beat:** FL's own engine as a mixer effect (`core/GrossBeat`, REA round 53) built from the saved state, which
  the `.lynx` project keeps: the 36 time-travel and 36 volume slots (FL's envelope player, every curve type but the
  spline 10), slot selection with trigger/position sync, link and one-shot, the 2-bar time buffer read by FL's
  8/16-tap windowed sinc (speed-dependent cutoff in HQ, jump declick, tempo-change crossfade, bar repeat/hold),
  volume tension, attack/release followers with attack compensation, mixes and the DC option; the dry path keeps FL's
  15-sample latency. The eight parameters stay editable/automatable in LYNX. Against Gross Beat_x64.dll: every one of
  the installation's 41 states (30 demo slots, 2 template slots, 9 presets) bit-identical at 95 BPM/44.1 kHz and
  174 BPM/48 kHz, 34 at 120 BPM (the rest −141…−153 dB, IPP's 50-bit sine at near-whole-sample read positions); slot
  automation, tempo changes, hold, stop/start, song jumps, PPQ 96/960 and odd block sizes bit-identical.
- **Fruity Chorus, Hyper Chorus, Vintage Chorus:** FL's own engines (`core/ChorusFx.cpp`, REA round 59) from the saved
  controls, editable as LYNX parameters: Fruity Chorus's three table LFOs, crossover and 12-bit fixed-point taps;
  Hyper Chorus's two 4-voice cores (movement panning, cubic taps, ArachnaChor), filters, high-pass cross-fade and
  parameter smoother in the engine's round-toward-zero mode, with its math library's sine and powf re-created;
  Vintage Chorus's BBD-style chain (filters, 32768-sample lines, hiss with LFO-moved filters, gate, mix/gain smoothing,
  Chorus 1/2 presets, LFO sync) with its library's FMA `sinf`. Bit-identical to the DLLs on every installation state
  (11 + 16 + 11) at 44.1/48/96 kHz, with automation, later instances and synthetic states, Linux and Windows builds.
  FL's random start phases (Fruity Chorus: clock-seeded; Hyper Chorus: `rand()` per instance) — LYNX uses 0 and the
  instance's place in the project respectively.
- **Fruity Squeeze** and **Vintage Phaser:** FL's algorithms (REA round 29) — Squeeze within 5·10⁻⁹ of the reference model
  (which is bit-exact with FL's DLL), Vintage Phaser (V1/V2, ×4/×8 oversampling, noise, gate, delay) within −121 dB.

**Still approximated or left out** (no confirmation yet):
- Gross Beat (REA round 53 §8): MIDI control of the slots (keys, key-held, MIDI trigger option) — LYNX effects get no
  MIDI; the hidden parameter 8 (time offset, 65536 in every state) is ignored; curve type 10 (spline) runs as a line
  (in no selected slot); FL's "playing" and position-jump messages are inferred from LYNX's transport.
- Harmor (`harmorFallbacks()` lists what a state uses): resynthesis-mode images (a sample analysed at load time) and
  images with a per-column gain table are ignored (the part plays its timbres; none in the demos), unknown
  resonance/phaser types switch that stage off, clip modes > 4 switch the clip off, blend modes > 5 play as mode 0;
  the part's note start delay (parameter 2: Rotochip, Resynthesis/Harmony) and the part filter set by its misc
  field +4 (Morning) are not modelled (REA_rodada165g-flbeta165s §3). Against the DLL on all 325 installation
  presets: 264 null below −60 dB, 304 with the same spectrum (STFT magnitude below −40 dB); the frequency phaser
  (type 209) at a high mix drifts in phase at precision "Average" for the IPP reason below. At precision "Average" FL computes `exp`/`expm1`/division with IPP's 11-bit routines
  (`*_A11`); LYNX computes them exactly — demos 74 and 78 (per-partial unison curve) stay at −57/−55 dB for that reason
  (−77.5/−84.5 dB at precision 1, REA_rodada75 §5). In the
  rack, the distortion's shapers are exact functions where FL uses IPP's 11-bit `Tanh/Exp/Ln/Sqrt/Cbrt` at precision 0
  (−55…−72 dB against the DLL there, −82…−123 dB at precision 2; a full-drive type 6 also amplifies the voice
  residual by ~10 dB), and the compressor stays off when its preset file is not found (import report). FL seeds its Mersenne Twister from the clock, LYNX with a fixed value per
  instance: random unison, free/spectral phases and random maps are deterministic and differ from any FL render by
  design. Tempo-synced envelope/LFO rates, the phaser rate and the portamento time follow the song tempo (the
  reference was measured at 120 BPM). The polyphony limit is LYNX's (a pool of six notes' worth of records, 16–96;
  the oldest are stolen).
- Sakura: mod wheel, pitch bend and channel pressure are 0 as matrix sources (LYNX sends no CCs to it); the LFO steps
  once per LYNX render block; with the resonators on the output is within −72 dB of the reference (their float32
  feedback amplifies rounding); strings longer than 9 600 samples (notes below ~20 Hz) are shortened.
- Drumpad: no key tracking (FL's DSP has none; the host's per-note pitch value is not confirmed).
- DrumSynth Live: the recorded waves (8 of the 24 default sounds use one) and the exact noise seed need the exact
  drumsynth_eng_x64.dll build (SHA-256 checked; otherwise those waves are silent and the noise starts from another
  seed); sends to other tracks (p35/p36), Mod X/Y and FL's link-velocity default are not modelled (Final.Vol = channel
  volume); a cut voice fades over 256 samples (FL's host ramp is not confirmed).
- BooBass: needs the exact BooBass_Eng_x64.dll build (SHA-256 checked; otherwise the saw approximation); keys below
  36 are clamped (FL reads outside its table there).
- Toxic Biohazard (REA round 45 §7): the step sequencer is ignored (off in every demo state); MIDI CC/aftertouch/bend
  sources of the modulation slots are 0 (LYNX sends none); the free-running operators' phase generator is process-global
  in FL (shared by every instance), LYNX starts one per instance like a fresh FL process, so random phases differ from a
  given FL render by design; FL's max-polyphony event value and its rate-before-state order at channel creation are taken
  from the GMS findings (inference).
- Fruity WaveShaper (REA round 51 §5): every LYNX export is treated as FL's rendering mode (the process-mode value FL
  sends when it exports was not observed); the editor-only toggle stored by layout ≥ 2 is not identified.
- Fruity Fast LP / Free Filter / Filter (REA round 63 §7): FL's render-call size for mixer effects is not confirmed
  (LYNX uses calls of at most 512 frames; the glides and end-of-call guards are per call). Emphasis keeps
  its approximation (limiter) — not ported yet (survey in REA round 154 §3).
- Emphasizer, Frequency Shifter, Low Lifter (REA round 154): bit-identical to FL's DLLs in the harness; FL renders
  match 98–99.98 % sample-for-sample once FL's warm-up on silence before rendering is reproduced. That warm-up length
  varies between FL runs (1536–2304 samples observed), so the Frequency Shifter's shifter mode only nulls against a
  given render with that run's warm-up; LYNX uses 2304 (Emphasizer) and 1536 (Frequency Shifter).
- Fruity PanOMatic / Balance (REA round 58 §1.6, §7): FL's host order at project load/export and the size of its render
  calls to mixer effects were not observed; LYNX starts the gains settled (FL's fresh instance fades in over 23 ms /
  2.3 ms) with PanOMatic's LFO at phase 0, and steps the LFO and ramps once per 512-frame call. Blood Overdrive's
  editor-only state field is not identified.
- Kepler (REA round 46 §6–§7): single-ulp differences of the DLL library's pow inside the render remain (−125…−135 dB
  on the signal; self-oscillating patches — Resonance 1 — amplify them to audible phase differences of the
  self-oscillation, 10 of 277 presets, no demo); LYNX applies the channel volume as an output gain (how FL applies it
  to this plugin is not confirmed: the wrapper ignores Final.Vol and gets no MIDI-volume event); the free-running
  oscillators/LFO pause while LYNX skips an idle channel (FL's run from instance creation); old (version < 4) synced
  states get the converted LFO1 rate at the rate set-up after loading (whether FL re-sends it is not confirmed); the
  synth fallback keeps its earlier approximations (chorus, LFO1).
- Harmless: the rack compressor needs the user's FL installation (its four presets are read from it; otherwise it
  stays off and the import report says so); the chorus/delay use the x87 kernels FL picks on the test machine (another
  CPU may make FL round the chorus delay instead of truncating it — not confirmed); a fresh delay starts its feedback
  ramp at the stored value (FL's host call order not observed); Maximus's Master Mid mode, its v14+ linear-phase split and curve
  segment types other than 0/7 are not decoded (REA round 40); the editor's quality
  (0/1) is used for playback and export alike (FL renders with quality 2; LYNX's engine does not tell instruments
  when it renders offline); FL seeds its random draws from the clock, LYNX from a fixed seed (spectral-random
  phases, Noise resonance and the LFO "voice random" are deterministic and differ from any FL render by design);
  the Freq phaser at quality 0/1 follows FL's law but not the error pattern of its 11-bit exponential; host
  answers for the release-time factor (p30) and release velocity (p46) are 1 and 0 (FL's not observed); Mod X/Y
  (Final.FCut/FRes) are 0; FL's per-channel max polyphony is not imported (LYNX's limit is its 96-record pool);
  the LFO/phaser phases follow song-position jumps but not the start of playback.
- Ogun (REA round 37 §1.20 and open questions): Seed 2 "Random" uses a per-instance counter starting at 0 (FL's starts
  from its clock-seeded RTL random seed), so Random spectra are deterministic and differ from any given FL render; the
  read start j₀ (argmin of the first table) occasionally picks a neighbouring sample vs FL (a time shift of that
  note's table); IPP's 11-bit `exp` in the fullness shaping is evaluated exactly (up to −79 dB on the demos, −52 dB on
  the worst synthetic case vs the DLL); global LFOs/envelopes run per voice (none in FL's data); host answers 2/6 are 0,
  release velocity 0.5, Mod X/Y 0; parameter changes rebuild the instance (sounding notes stop); a voice pool grows on
  demand up to 64 voices per channel (the oldest is cut beyond that); FL's per-channel max polyphony is not imported
  (LYNX's mono setting = 1). Autogun (REA round 57 §1): the same limits; FL's Seed-2 counter differs per run; the
  Magic Dust stage is skipped when `SG.dat` is not found; automation of Magic Dust (FL updates only the stage's on/off,
  not the hosted parameter) and a possible LFO-knob refresh difference on articulators 5–12 (no data uses it) are not
  modelled. Transistor Bass (REA round 57 §2): the CRT's pow/powf last-bit differences (−108 dB worst); parameter
  automation smoothing, MIDI CC/pitch-bend, the "MIDI channel 13" sequence transpose (no note channel in LYNX) and the
  order of FL's rate call vs state load (V2 decay quirk) are not modelled / not confirmed; the channel volume is applied
  as an output gain (the plugin does not read Final.Vol). SimSynth (REA round 64 §5.2): the process-wide random index
  and chorus counter are per instance in LYNX (FL: shared by every SimSynth and every sample-rate change); FL's load
  order and its rounding when it fills the old voice record are not confirmed; automation not wired.
- Plucked! (REA round 65 §1): FL's random seed (process-global, from the clock) differs per run — LYNX starts every
  instrument at Delphi's default seed; FL's host gain-ramp law on level changes is not decoded (linear in LYNX); a
  channel sample as the string's exciter is not modelled. BeepMap (REA round 65 §2): bitmaps taller than "Max height"
  are scaled with LYNX's nearest-row pick (FL's image scaler not decoded; no installation state needs it); only
  uncompressed 24/32-bit BMP data are read; the processor's end-fade table uses the exact cosine (FL: IPP's, ≤ −95 dB).
  Sawer (REA round 65 §3): the DLL's random generator is process-global (LYNX: one per instrument, started like a
  freshly loaded DLL); aftertouch, pitch-bend and controller modulation sources are not fed (0); FL's host gain-ramp law
  is not decoded; the reverb's coefficient rounding (CRT float functions) leaves −137…−155 dB; without the user's
  `data.saw` the channel stays unsupported.
- Fruity Granulizer
  (REA round 67 §5): the slice modes (the sample's regions, "auto slicing": 2 presets, no project) and the slice key
  mode play as "no slices"; tempo sync (FL's beat count of the sample) is off; FL's interpolation quality message is not
  confirmed (the plugin's default, 24-point sinc, is used); the DLL's rcpps reciprocal and IPP sines are replaced by
  exact ones (−128 dB; isolated samples to about −87 dB when a grain reads within 10⁻¹² of a sample point at a rational
  speed); the sample's own loop points are not read (whole sample); one random seed
  per instrument.
- Drumaxx (REA round 67 §9): the pattern sequencer and swing are not played (a note on a pattern's trigger key — 100
  in every installation state — plays only the pads on that key); every pad plays on the channel (the plugin's
  separate mixer outputs are not routed; no installation state uses them); MIDI controllers are not wired; the CRT's
  double sinh/pow are the C library's (last-bit double differences that changed no float sample in the tests); CPUs
  without FMA take other CRT paths in FL (not checked).
- FL Slayer (REA rounds 68 §9, 79): the channel's pan/volume/pitch now act as FL sends them (MIDI events: the
  front-end's gains from twice the channel gain, the pitch as a pitch bend) and parameter automation / settings edits
  reach the running engine (EXEC: 120/120 random scenarios bit-identical). Not reproducible: the phase of the engine's
  5 ms control grid and the position of its noise sequence, which in FL depend on everything the engine processed since
  the project was loaded (round 79 §4); the master pitch reaches the engine through the notes, not the bend.
- 3x Osc (REA round 47 §5): HQ oscillators within −137…−168 dB of the DLL (LYNX builds the band-limited tables with a
  double FFT, FL with its own single-precision one; MSVC's sinf); the custom wave is resampled linearly (FL's resampler
  mode 6 not decoded); FL's host "HQ render" oversampling is not modelled; the random seed (start phases, noise steps)
  is per instrument, FL's is drawn per session and shared by all 3x Osc channels;
  channel records win over a stale state snapshot (inference).
- Fruity DX10 (REA round 42 §4–5): FL's host restoring Final.Pitch before each tick (pitch articulator) and its exact sample placement of
  sequencer ticks are inferences (LYNX ticks on the song's PPQ grid while playing, free-running otherwise); global
  LFOs ignore song-position jumps; notes have no Mod X/Y; the filters are bit-exact only where long double is the x87
  format.
- Sytrus: chaotic FM patches (two-way FM loops, strong self-feedback) cannot be sample-exact — FL builds sine tables
  with IPP's tone generator (≈1 ulp from an exact sine) and the chaos amplifies it (statistically equivalent);
  random maps use a fixed per-instance seed (FL's is clock-seeded, so they are not reproducible in FL either); the
  noise generator is per instance (FL shares one across all Sytrus instances in the process); the mixer send of the
  FX bus (main int 0x2F4) is not routed; the GLOBAL envelope hand-over is exact only when the held voice rests on
  its sustain point (inside a segment FL's hand-over is not decoded: 3 demo channels); curve modes 10–12, the Mod X/Y
  SMOOTH coefficient and FL's polyphony setting (FPE_MaxPoly; LYNX: mono channels → 1, otherwise unlimited) are not
  reproduced; the FX tail length is inferred (100 samples + the chorus ring incl. one block); with the export
  settings a few demo patches still differ at −61…−80 dB (unison + full-noise operators into the FX bus, noise-heavy
  resonant Vanilla HP); the polyphony limit was not exercised against the DLL. States older than version 17 still use
  the old 3-oscillator conversion. Export with the render settings is CPU-heavy: most demo patches run their voices
  8× oversampled when FL exports, and LYNX does the same.
- Love Philter: what FL sends when an export starts (a flush, which skips the first tick's ramps) is not observed; LYNX
  starts like a freshly loaded plugin.
- Pitcher: MIDI mode and Harmonize are implemented but LYNX effects receive no MIDI notes, so they behave as FL
  with nothing on the Pitcher's MIDI port (MIDI: no correction; Harmonize: dry voice / dry mix). The reported
  latency (9 / 257 samples) is the DSP's delay; FL's own PDC report is not confirmed.
- Vocodex: the Soundgoodizer stage (FL's engine with `Data/SG.dat`) and the internal Sytrus carrier (Ookay,
  NewStuff) need the user's FL installation at import (otherwise the old approximation / modulator pass-through and
  the import report says so); with the internal carrier the match is −112 dB (LYNX's Sytrus on that preset), the
  others −136…−145 dB; map segments with tension ≠ 0 are read as straight lines (no demo uses them).
- Maximus: 4 of the 100 demo states use the v15 linear-phase band split (Frequency Splitter's engine) or Master Mid
  mode is set — those keep the approximation (REA round 40 §8).
- Stereo Enhancer: a delay set before a sample-rate change keeps its old length in FL; LYNX uses the current rate (FL's
  load order not confirmed; identical at 44.1 kHz). Stereo Shaper / Enhancer ramp their gains per host call; LYNX
  splits blocks at 512 frames (FL's block size for them not confirmed). Spreader: FL's engine diverges to NaN at
  rates where its 15 kHz section passes Nyquist (22.05 kHz); LYNX outputs 0 there (REA round 54 §6–7).
- Fast Dist / Flangus / Flanger (REA round 55 §6): that FL's live/file-render switch of the host waveshaper follows
  its `.wav` render and that FL's start-up installs the x87 host routines are inferences; whether FL flushes Flangus
  or sends the Flanger's "enabled" notification before playback is not observed (LYNX starts both that way).
- FLEX: its packs are encrypted; LYNX never opens them (the channel gets a synth patch by preset name).

## `.flp` format findings (besides `docs/ANALISE_FL_STUDIO.md`)
Confirmed empirically on the demo projects:
- Note record: 24 bytes in every version, including 25.x.
- Playlist record: 32 bytes (< 20.99.1), 60 bytes (20.99.1 to 24.1), 80 bytes (≥ 24.2.99).
- Playlist: 199 tracks before FL 12.9; 500 from then on (track index inverted).
- Playlist item: index > base (20480) = pattern (index − base); otherwise a channel clip.
- A channel's mixer track: event 0x68 (u16); 0x16 (u8) only in old files.
- A mixer insert's name (0xCC) comes before the 0xEC that opens that insert.
- Channel name: 0xCB; files ≤ 11 use 0xC0. Internal plugin name: 0xC9.
- The version string (0xC7) is always ASCII; other strings are UTF-16LE from 11.5.4 on.
- In pattern clips the 8 bytes at +24 are two int32 tick values (start and end of the pattern slice), not
  floats; −1 (NaN when read as float) = no slice. In channel clips (audio/automation) they are floats, in beats
  or milliseconds depending on the project. FL does not stretch audio clips.
- Many factory samples are WAV files with format tag 0x674F/0x6750 and an Ogg Vorbis stream in the `data`
  chunk; others are plain Ogg files with a `.wav` extension. LYNX decodes them as FL's `oggio_x64.dll` does
  (libvorbis' packet loop, FL's stream-end rule, FL's MDCT table; `src/core/OggVorbis.*`, REA round 77): all 3 137
  installation files bit-identical to FL's decode (JUCE's vorbisfile reader dropped blocks in 295 of them). Paths use `%FLStudioFactoryData%` (install folder),
  `%FLStudioData%` (install\Data) and `%FLStudioUserData%` (Documents\Image-Line\FL Studio).

## Audio drivers
- Windows Audio (shared, exclusive and low-latency), DirectSound and **ASIO** (Steinberg ASIO SDK 2.3.4 headers,
  GPLv3 option). `LYNX.exe --devicetest "ASIO" "<driver>" report.txt` opens a driver, runs 1 s of silent callbacks
  and reports rate, block size and latency: "FL Studio ASIO" works (44.1 kHz, 512, 86 callbacks/s). On the test
  machine "Realtek ASIO" ends the process while loading (exit code 116) — Steinberg's own SDK host sample does the
  same, so it is the driver. In the Zig/MinGW build a driver that crashes while opening/closing is not contained
  (JUCE's `__try` guard needs MSVC; the CMake/MSVC build keeps it).
- **LYNX ASIO** (own universal ASIO driver over WASAPI, REA rounds 97 and 106): shared or exclusive mode, playback
  and recording with drift-compensated full duplex (adaptive sinc resampler on the input), lost capture frames
  replaced in time, control panel with device in/out, buffer, mode, margin and a latency readout; 64-bit
  `lynx-asio.dll` and 32-bit `lynx-asio32.dll` ("Register LYNX ASIO.bat" registers both for other programs).
  Without registration LYNX lists it as "LYNX ASIO (built in)". Measured on a USB codec: exclusive 144 samples,
  output 6 ms / input 19 ms, real project + recording without glitches (`--rtstress ... --record take.wav`).

## Plugin hosting
- Optional separate-process plugins: each plugin runs in a child `LYNX.exe --lynx-plugin-host`. Audio
  (up to 8 channels × 4096), MIDI (up to 1024 events) and parameter changes (up to 512 per block, both ways) go
  through shared memory with a pair of events per block; state and editor through messages. If the child does
  not answer within 0.5 s or exits, the plugin goes silent, a notice appears and LYNX keeps running. Verified with
  `LYNX.exe --sandboxtest plugin.vst3 report.txt`; `LYNX_SANDBOX_LOG=file` logs every step.
- The VST3 SDK bundled with JUCE builds with Zig/MinGW; plugins are prepared at a fixed rate and re-created
  when the device rate changes; plugin latency feeds the PDC.
- Plugin scanning runs in the background (progress in the hint bar), one child `LYNX.exe --scan-one` per file
  (a crash or 30 s hang blacklists the file), incremental (unchanged listed files are skipped), cancellable, and
  again a few seconds after each start for new plugins. Opening an `.flp` resolves its plugins from the list or FL
  Studio's plugin database (the same files FL uses) and scans only the missing ones' files first
  (`docs/FL_INTEGRATION.md`). First-run setup wizard: language, audio device, plugin folders (standard VST3/CLAP
  plus FL Studio's), scan. Per-user installer (`scripts/make-installer.sh`, `docs/RELEASE.md`).

## Performance (export CPU, 2026-10-15)
Where the exact-engine rounds made the export slower (per-effect timing and `perf` on `build-local/renderwav`):
LollieVox: Fruity WaveShaper (render mode forces 16× oversampling; SMOOTH ran the x87 kernel one sample at a time
with two control-word changes per sample and channel) was ~25 % of the render, Fruity Limiter (x87 one-pole chains)
~8 %, Reeverb 2 ~4 %; Exoplanet: WaveShaper, Fruity Limiter, Reeverb/Reeverb 2 and MiniSynth's `idle()` (a 99-voice
scan on every block); Future House: Reeverb 2. Chorus (r59) and 3x Osc (r47) cost ≤ 1 % on these demos.

Bit-identical changes (`feature/perf`): WaveShaper half-bands on two-lane double vectors, one control-word change per
block, a constant-gain run once the SMOOTH glide reaches its fixed point, direct x87 loads/stores and `frndint` in the
kernel (≈ 2.5× faster); Fruity Limiter curve stages run sample-interleaved and a resting gain chain is skipped
(≈ 1.15×); Reeverb 2 loop on local state and no modulation sine at zero depth; MiniSynth `idle()` cached.

Checks: the 30 demos rendered by old and new `build-local/renderwav` (`LYNX_RAW=` float dump and the WAV) are
byte-identical; `build-local.sh` 0 failures. Note: the per-file object build and `build-local.sh`'s one-command build
give different (each self-consistent) bits, so compare like with like.

User CPU seconds of a whole-song `renderwav` (old main `6f03348` and this branch run side by side on a loaded
12-thread machine; wall-clock `render_ms` is noisier than CPU time):

| Demo | old CPU s | new CPU s | old render_ms | new render_ms |
|---|---|---|---|---|
| LollieVox & Lezina J - I'll Be Your Madonna | 105.7 | 89.6 (−15 %) | 136008 | 122837 |
| Eric Kauffmann & NrgMind - Exoplanet | 230.0 | 222.5 (−3 %) | 479690 | 469316 |
| Asher Postman - Future House | 9.7 | 9.8 | 75488 | 77389 |
| DEDZII - NIGHT TERROR | 83.1 | 82.1 | 246018 | 220134 |
| Atthar … - Right Night feeling | 122.9 | 122.8 | 572121 | 567457 |
| Electric Loop - The Ogun Dream | 110.8 | 110.8 | 110819 | 110528 |
| Astes - Bien Duro | 67.3 | 66.8 | 405254 | 401997 |
| All 30 demos | 1118.0 | 1087.9 (−2.7 %) | | |

Against `2d12823` (before the exact-engine rounds), same run: LollieVox 74.3 → 105.1 → 92.2 s, Exoplanet
172.0 → 220.4 → 214.8 s, Future House 9.8 → 10.3 → 10.7 s (2d12823 → 5464b71 → this branch). What remains is the
exact engines' own arithmetic (x87 extended-precision kernels, 16× oversampling, per-sample `exp`/`log10`/`sin`),
which cannot change without changing output bits.

## Not yet verified by a human
- Audio input recording (depends on the user's hardware; code reviewed, not tested).
- CLAP plugin editor windows (audio hosting tested; windows not opened to avoid touching the user's screen).
- The setup wizard's interaction (checked by an off-screen snapshot) and FL Studio's plugin-database file layout:
  this PC's FL has no third-party plugins scanned, so `.nfo`/`.fst` parsing is tested with synthetic fixtures only.
- Mouse/keyboard interactions are checked by code review and off-screen snapshots only.

## Open items

- Sample players (REA rounds 69 §9 and 72 §9): DirectWave's HQ interpolation, time-stretch modes (§8); the host
  sampler voice's velocity/pan laws and channel filter (decoded in part, round 72 §9, not applied); FL's song-aligned tick phase for the sampler's volume envelope
  (LYNX counts ticks from the note start, ≤ 1 tick apart); FPC's random layer modes and simultaneous layers (unused in
  the demos); Fruity Slicer's time-stretched slices (5 demo channels pre-stretch their loop by 0.6–5.5 % through a
  licensed stretch engine — LYNX plays the slices unstretched); FL Keys' resampling of "resample" wavetables (Roto
  Organ) when the host rate is not 44.1 kHz.
- Maximus's Master Mid mode and linear-phase
  split; Harmor's resynthesis-mode images.
- Workflow P2 leftovers (`docs/WORKFLOW_FL.md`): per-segment automation curve shapes, clip colour/name and track
  colour/height (need model fields), F2/F3 search stepping, instrument drops on mixer inserts, the automation
  record filter, and the remaining note tools (chop, quantise, articulate, flam, period slicing, LFO).
