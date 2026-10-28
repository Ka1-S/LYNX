# Parity audit: LYNX's "inspired by" plugins against the originals

Rounds 128–129 (2026-10-21). The user's question: "people tell me details are missing in the plugins — is it
configuration or design?" Round 114 scored only the layout. This audit checks **everything**, item by item:

- **Configuration**: every control, mode, menu option, switch, range / unit / default, per-band option, context
  menu, mouse gesture, side chain, oversampling / quality, latency, A/B, undo / redo, presets, MIDI learn, analyser
  options, meters, window size, help.
- **Design**: what is shown where (display contents, readouts, labels, sections, proportions), drawn in LYNX's own
  style (skin122 / skin126 own the look; this audit owns the *what* and *where*).

Sources: each original's official trial installed on this PC, hosted black-box in our own host (parameter lists read
through the public VST3 API — every control, range and option text — plus screenshots of each view as a user sees
it), the official manuals, and `docs/specs/*.md`. Nothing was decompiled; no graphics, presets or names were copied.
Reference screenshots stay outside git (`D:\lynx-work\parity128\ref`); the side-by-side pages are on the Desktop
(`LYNX-paridade`).

Legend: ✅ same · ⚠️ partial (note says what differs) · ❌ missing · — not applicable / deliberately left out.
Scores are 0–10 (10 = a user of the original finds every control and every piece of information where they expect).

Round 128 covers the FabFilter / Slate / Antares roles (below). Round 129 covers the Waves / SSL / Valhalla roles
(its own section).

## Current scores (config / design) — one table, the latest round per pair

Kept current by the parity rounds (round 165h onwards). "Since" = the round that last changed the pair; "165" = this round's
before → after (blank = not touched in round 165). Lowest first.

| LYNX engine | Original | Config | Design | Since | Round 165 |
|---|---|---|---|---|---|
| Ridge Strip | SSL E-Channel (Waves) | 9 | 8 | 165 | 8 → 9 / 7 → 8 |
| Ember Saturator | Decapitator | 8 | 7 | 135 | |
| LYNX Tune | Auto-Tune Pro | 8 | 7 | 135 | 165j: sound only (highs kept, measured; scores unchanged) |
| Ridge Strip (Vocal strip) | SSL Native Vocalstrip 2 | 9 | 7 | 145 | |
| Hush De-esser | Pro-DS | 8 | 8 | 135 | |
| Halo | ValhallaShimmer | 8 | 9 | 135 | |
| Horizon | ValhallaFutureVerb | 8 | 9 | 135 | |
| Orbit | ValhallaSpaceModulator | 8 | 9 | 135 | |
| Facet EQ | Pro-Q 4 | 9 | 8 | 152c | |
| Crest Compressor | Pro-C 3 | 9 | 8 | 137 | |
| Sentry Gate | Pro-G | 9 | 8 | 135 | |
| Ember Saturator | Saturn 2 | 9 | 8 | 152b | |
| Aether | Fresh Air | 9 | 8 | 128 | |
| Glow Leveler | CLA-2A (Waves) | 9 | 8 | 152a | |
| Hush De-esser | DeEsser (Waves) | 9 | 8 | 147 | |
| Gemini Doubler | Doubler2 / Doubler4 (Waves) | 9 | 8 | 137 | |
| Echoform | H-Delay (Waves) | 9 | 8 | 137 | |
| Echoform | ValhallaDelay | 9 | 8 | 137 | |
| Atrium | ValhallaRoom | 9 | 8 | 135 | |
| Auto-pan (stand-in) | PanMan | 9 | 8 | 152 | |
| Foil Plate | Little Plate | 9 | 8 | 152 | |
| Apex Limiter | Pro-L 2 | 9 | 9 | 135 | |
| Epoch Verb | ValhallaVintageVerb | 9 | 9 | 135 | |
| Nebula | Valhalla Supermassive | 9 | 9 | 135 | |
| Spiral Echo | ValhallaFreqEcho | 9 | 9 | 129 | |
| Lattice Mod | ValhallaUberMod | 9 | 9 | 144 | |
| Quell Denoiser | NS1 (Waves) | 9 | 9 | 137 | |
| Foil Plate | ValhallaPlate | 9 | 10 | 135 | |
| Compressor (stand-in) | Renaissance Compressor (Waves) | 10 | 8 | 152 | |
| LYNX Tune | Waves Tune Real-Time | 10 | 8 | 152b | 165j: sound only (measured; scores unchanged) |
| Echoform | EchoBoy / PrimalTap | 10 | 8 | 150 | |
| Voice Shift | Little AlterBoy | 10 | 8 | 137 | |
| Prism Multiband | Pro-MB | 10 | 10 | 152 | |

## Round 165j: LYNX Tune sound quality, before → after (measured)

Round 165j (`docs/analise/REA_rodada165j.md`): LYNX Tune against the official Auto-Tune Pro 11.6 and Waves Tune
Real-Time trials (hosted black-box) on ten synthetic takes with a known f0 and on her 24 takes with her own Auto-Tune
states (translated as LYNX loads them). Config / design scores unchanged (no controls were added); this table is the
sound.

| Measure (her 24 takes unless noted) | Auto-Tune Pro | LYNX 0.1.19 | LYNX 0.1.20 |
|---|---|---|---|
| pitch: median / p90 cents from the note | 5.79 / 26.2 | 4.39 / 20.9 | 4.42 / 21.0 |
| same note as Auto-Tune (% voiced frames) | 100 | 95.8 | 95.7 |
| HF balance vs the dry take (2.5–6.3 kHz over 0.3–1 kHz, dB) | −0.07 | −0.52 | −0.13 |
| formant envelope distance to the dry take (dB) | 0.49 | 0.80 | 0.78 |
| short bursts / min not in the dry take | 31.2 | 40.9 | 40.1 |
| Classic / Low latency: bursts / min, HF balance | 27.9, −0.02 | 48.8, −0.51 | 45.4, −0.13 |
| synthetic: latency ms, accuracy med / p90 c, retune ms, vibrato kept | 61.6, 2.8 / 10.1, 42, 1.01 | 25.4, 1.7 / 4.4, 38, 0.97 | 25.4, 1.7 / 4.4, 38, 0.97 |
| synthetic: HF balance, formant distance (dB) | —, 0.18 | −0.40, 0.28 | −0.04, 0.36 |
| CPU per instance (block 128, 44.1 kHz, median) | — | ~64 µs (2.3 % RT) | ~65 µs (+1 %) |

## Round 145: SSL Native Vocalstrip 2 (Ridge Strip's Vocal strip voicing)

Round 145 (`docs/analise/REA_rodada145.md`) measured the official Vocalstrip 2 trial (Complete Access) black-box and
gave Ridge Strip a **Vocal strip voicing** with every control of the reference: config **2 → 9** (no FFT
analyser), design **1 → 7** (the reference's modules and reading order in
LYNX's style; the transfer-curve and GR-history displays are bars). Her Vocalstrip 2 state now plays the measured
model (−60 … −67 dB null on her takes). The other Complete Access plugins (Slate, SSL, LiquidSonics, sonible,
Harrison) map to LYNX effects by role only.

## Round 152: what changed (config / design, after 150 → after 152)

Round 152 (`docs/analise/REA_rodada152.md`, sub-rounds `REA_rodada152a.md` / `152b` / `152c`).

| LYNX engine | Original | Config 150 → 152 | Design 150 → 152 | Round-152 changes |
|---|---|---|---|---|
| Prism Multiband | Pro-MB | 10 → 10 | 10 → 10 | the attack in the gain stage (slow attacks now ≤ 0.4 dB off), Dynamic phase's own narrow-band / cut shapes (measured table, ≤ 0.05 dB), 0.8-octave minimum band; editor menus at the mouse, read-out above the band panel (checked by hand) |
| Compressor (stand-in) | Renaissance Compressor | 9 → 10 | 7 → 8 | bipolar gain-change meter, measured static curve at every ratio, RComp-style factory start (152a) |
| Glow Leveler | CLA-2A | 7 → 9 | 7 → 8 | the reference's fresh instance, ANALOG 50 / 60 Hz, measured Limit and AUTO MAKEUP; translated (152a) |
| Auto-pan (stand-in) | PanMan | 8 → 9 | 7 → 8 | trigger sources Ping-pong / Random step (threshold, divider, detector filter, manual trigger), Random steps |
| Foil Plate | Little Plate | 9 → 9 | 6 → 8 | the one-knob Plate view (the substitute opens in it) |
| Facet EQ | Pro-Q 4 | 9 → 9 | 8 → 8 | spectral dynamics (Spectral / Density / Tilt per band, measured per-bin law, 5120-sample latency) and EQ match (capture reference + track, Match 25 … 100 %) (152c) |
| Crest Compressor / Echoform | Pro-C 3 / EchoBoy | — | — | stale ❌ rows re-checked: Host sync / MIDI trigger and audition (round 137), EchoBoy state recognition (round 136), EchoBoy ping-pong start side (round 135w); no ❌ row is left |

### Sub-round 152b

Sub-round 152b (`docs/analise/REA_rodada152b.md`), measured black-box on the official Saturn 2 trial.

| LYNX engine | Original | Config 150 → 152 | Design 150 → 152 | Round-152b changes |
|---|---|---|---|---|
| Ember Saturator | Saturn 2 | 8 → 9 | 8 → 8 | each crossover's own slope (6 … 48 dB/oct, exact), linear phase (zero-phase bands, 3072-sample latency), modulation (2 LFOs, envelope follower, 4 slots, the reference's laws), Breakdown's pitch drop on Collapse; translated |
| LYNX Tune | Waves Tune Real-Time | 9 → 10 | 8 → 8 | Group Octaves off (per-key statuses), MIDI reference tone |

## Round 150: what changed (config / design, after 147 → after 150)

Round 150 (`docs/analise/REA_rodada150.md`, sub-rounds `REA_rodada150a.md` / `REA_rodada150b.md`).

| LYNX engine | Original | Config 147 → 150 | Design 147 → 150 | Round-150 changes |
|---|---|---|---|---|
| Prism Multiband | Pro-MB | 10 → 10 | 9 → 10 | free layout with gaps, slopes 6 … 48 dB/oct (measured per processing mode, the 6 dB/oct designs), side-chain input per band, narrow free side-chain filter, measured detector (soft range, release after the curve); translated |
| Compressor (stand-in) | Renaissance Compressor | 8 → 9 | 7 → 7 | the reference's own ranges: ratio 0.5 … 50 with the measured upward expansion, gain ±30, attack 0.5 … 500, release 5 … 5000 ms; translated |
| Echoform | EchoBoy / PrimalTap | 10 → 10 | 8 → 8 | voicings in Dual / Ping-Pong (measured), L/R offset, wobble shapes / depths from delay tracks (no style has several voices on the trial) |
| Ember Saturator | Saturn 2 | 7 → 8 | 7 → 8 | six bands, global input / output pan, 28 measured characters (LYNX names); translated |
| LYNX Tune | Waves Tune Real-Time | 9 → 9 | 7 → 8 | Send / Receive of the scale between instances |
| Auto-pan (stand-in) | PanMan | 7 → 8 | 6 → 7 | Tweak page (rate / offset / width modulation by the measured envelope law, Feel / Groove, beyond the speakers) |
| Foil Plate | Little Plate | 8 → 9 | 6 → 6 | measured Little Plate voicing (band decays, 0.7 ms onset, ∞ ≈ 80 s) |

## Round 147: what changed (config / design, after 144 → after 147)

Round 147 (`docs/analise/REA_rodada147.md`, sub-rounds `REA_rodada147a.md` / `REA_rodada147b.md`).

| LYNX engine | Original | Config 144 → 147 | Design 144 → 147 | Round-147 changes |
|---|---|---|---|---|
| Prism Multiband | Pro-MB | 9 → 10 | 8 → 9 | Dynamic phase measured and rebuilt (minimum-phase bands in cascade; older projects keep the classic engine), expert panel: side-chain filtering band / free, stereo link + mid / side-only, each crossover its own slope; translated |
| Echoform | EchoBoy / PrimalTap | 9 → 10 | 8 → 8 | 31 echo voicings measured from EchoBoy's styles (LYNX names), saturation type menu, PrimalTap's Multiply 1x–8x |
| Ember Saturator | Saturn 2 | 6 → 7 | 7 → 7 | channel mode L/R \| M/S, each band's drive pan and pan (measured), translated |
| LYNX Tune | Waves Tune Real-Time | 8 → 9 | 6 → 7 | keyboard strip: Bypass / Remove modes, note status with direction, range bar and presets, octaves, scroll, latch |
| Ridge Strip | SSL E-Channel | 7 → 8 | 7 → 7 | ANALOG (measured noise floor), auto make-up for Ridge's own compressor, translated |
| Hush De-esser | DeEsser | 9 → 9 | 7 → 8 | the energy-detector bar beside the threshold arrow |

## Round 144: what changed (config / design, after 137 → after 144)

Round 144 (`docs/analise/REA_rodada144.md`, sub-rounds `REA_rodada144a.md` / `REA_rodada144b.md`).

| LYNX engine | Original | Config 137 → 144 | Design 137 → 144 | Round-144 changes |
|---|---|---|---|---|
| Echoform | EchoBoy / PrimalTap | 8 → 9 | 7 → 8 | rhythm editor (Even / Free / Grid taps, Shape, 12 LYNX patterns), Style edit row (input, loop EQ, FB balance, diffusion loop / post, wobble shape / sync) |
| Lattice Mod | ValhallaUberMod | 8 → 9 | 9 → 9 | DelaySync measured (18 steps, 2750 ms cap) and translated |
| Facet EQ | Pro-Q 4 | 9 → 9 | 8 → 8 | linear-phase resolution Low … Maximum (the trial's latencies), translated |
| Hush De-esser | DeEsser | 9 → 9 | 6 → 7 | the GR meter's held peak (click to reset) |
| Prism Multiband | Pro-MB | 7 → 9 | 7 → 8 | Processing (Dynamic / Linear / Minimum phase, measured latencies), oversampling 2x / 4x, mid / side band pan, analyser resolution / tilt, translated; Dynamic phase still the old engine |

## Round 137: what changed (config / design, after 135 → after 137)

Round 137 (`docs/analise/REA_rodada137.md`) took the biggest remaining ❌ / ⚠️ rows and re-measured rows marked "not
measured" on the trials (EchoBoy, Pro-C 3 and Pro-Q 4 trials; Valhalla demos — black-box, short runs between the
demo's fades).

| LYNX engine | Original | Config 135 → 137 | Design 135 → 137 | Round-137 changes |
|---|---|---|---|---|
| Echoform | EchoBoy / PrimalTap | 7 → 8 | 6 → 7 | Rhythm echo mode (measured law: repeats, decay, 2-beat pattern, feedback replay), a bar per tap |
| Echoform | ValhallaDelay | 8 → 9 | 7 → 8 | Era (measured bandwidth per mode), ducking law measured, quad taps / Repeat-Swell / Era translated; stale rows re-checked |
| Echoform | H-Delay | 9 → 9 | 7 → 8 | LoFi switch independent of the mode, the note / BPM read-out on the picture |
| Lattice Mod | ValhallaUberMod | 7 → 8 | 9 → 9 | Input pan (measured routes), translated |
| Crest Compressor | Pro-C 3 | 8 → 9 | 8 → 8 | side-chain trigger: Host sync (note, rate, length — measured) and MIDI |
| Voice Shift | Little AlterBoy | 9 → 10 | 8 → 8 | MIDI control of the pitch / the robot note |
| Gemini Doubler | Doubler2 / Doubler4 | 9 / 8 → 9 / 8 | 7 → 8 | the three graphs editable (drag, double-click), axis labels |
| Quell Denoiser | NS1 | 9 → 9 | 8 → 9 | the fader's 0–100 scale, held reduction read-out |
| Horizon / Facet EQ | FutureVerb / Pro-Q 4 | — | — | measured only: Horizon's note list confirmed; Pro-Q 4's linear-phase latencies per resolution recorded |

## Round 135: summary (all pairs, config / design)

Round 135 (`docs/analise/REA_rodada135{f,s,v,w}.md`) implemented the ❌ / ⚠️ items, biggest user-visible gaps first
(149 ❌ rows before, 36 after). The Soundtoys trials were activated by the user and are now measured black-box (their
tables are no longer "manual only"); Auto-Tune Pro was re-activated and hosted black-box. **Waves has no licence on
this PC: every Waves plugin runs in demo mode** (the UI is complete, the audio mutes periodically), so Waves rows were
re-checked for controls and layout only, never for sound. "Before" = the scores after rounds 128 / 129.

| LYNX engine | Original | Config before → 135 | Design before → 135 | Main round-135 changes |
|---|---|---|---|---|
| Facet EQ | Pro-Q 4 | 8 → 9 | 8 → 8 | Flat Tilt / All Pass shapes, Character menu |
| Apex Limiter | Pro-L 2 | 7 → 9 | 8 → 9 | loudness meter (M / S / I / LRA / true peak), peak-type switch, display modes, oversampling, side-chain trigger |
| Crest Compressor | Pro-C 3 | 8 → 8 | 8 → 8 | wet / dry pan |
| Prism Multiband | Pro-MB | 5 → 7 | 6 → 7 | up to six bands (double-click splits), band pan, external side chain |
| Sentry Gate | Pro-G | 8 → 9 | 8 → 8 | oversampling |
| Hush De-esser | Pro-DS | 7 → 8 | 8 → 8 | oversampling |
| LYNX Tune | Auto-Tune Pro | 7 → 8 | 7 → 7 | Learn scale (MIDI), Classic (low-latency path, same retune — measured) |
| Compressor | Renaissance Compressor | 5 → 8 | 5 → 7 | ARC / Manual, Electro / Opto, Warm / Smooth, 0 dBFS limiter + pill |
| Hush De-esser | DeEsser | 8 → 9 | 5 → 6 | output meter + clip lamp, "Classic split" presets |
| Gemini Doubler | Doubler2 / Doubler4 | 8 / 5 → 9 / 8 | 7 → 7 | voices 3–4, modulation reset, clip lamp |
| Echoform | H-Delay | 7 → 9 | 5 → 7 | own BPM, ping-pong start side, Link EQ, output meter + clip lamp |
| LYNX Tune | Waves Tune Real-Time | 5 → 8 | 5 → 6 | Real-time page: Speed + Link, Tolerance cents / time, Vibrato, Correction, meters |
| Echoform | ValhallaDelay | 7 → 8 | 7 → 7 | Quad taps A–D, Repeat / Swell, description line |
| Echoform | EchoBoy / PrimalTap | 5 → 7 | 5 → 6 | Groove / Feel (measured), Accent, Prime numbers, cross feedback / balance, Freeze |
| Epoch Verb | ValhallaVintageVerb | 9 → 9 | 8 → 9 | description line, MIX-label lock, original preset names |
| Atrium | ValhallaRoom | 9 → 9 | 8 → 8 | (same) |
| Foil Plate | ValhallaPlate | 9 → 9 | 9 → 10 | (same) |
| Nebula | Supermassive | 9 → 9 | 9 → 9 | (same) |
| Halo | ValhallaShimmer | 7 → 8 | 8 → 9 | the four reference modes (Mono / small / medium / big stereo) |
| Horizon | ValhallaFutureVerb | 7 → 8 | 7 → 9 | mode-dependent controls |
| Orbit | ValhallaSpaceModulator | 8 → 8 | 7 → 9 | description line |
| Lattice Mod | ValhallaUberMod | 7 → 7 | 7 → 9 | the nine named modes |
| Ember Saturator | Decapitator | 5 → 8 | 4 → 7 | Punish, Thump, Steep, five one-click voicings, wide cuts, 0–10 drive, IN / OUT meters, her states translated |
| Auto-pan | PanMan | 3 → 7 | 2 → 6 | free LFO 0.01–10 Hz, rhythm steps, direction, smoothing, in / out, analog stage, pan-position strip, her states |
| Foil Plate | Little Plate | 6 → 8 | 5 → 6 | Infinite, low cut 20 Hz–1 kHz |
| Voice Shift (new) | Little AlterBoy | 0 → 9 | 0 → 8 | new engine: Pitch / Formant ±12 st, Link, Transpose / Quantize / Robot, Drive, Mix; her 52 states translated |

Shared by every LYNX engine editor (round 135): the per-control description line at the bottom of the window
(Valhalla's strip; ~330 sentences, pt-BR), the mix lock by clicking the MIX caption, and the name of the original's
preset (Valhalla / Waves states) instead of "(no preset)".

---

# FabFilter / Slate / Antares roles (round 128)

| LYNX engine | Original (trial here) | Config before | Design before | Config after | Design after | Config 135 | Design 135 |
|---|---|---|---|---|---|---|---|
| Facet EQ | Pro-Q 4 (v4.1.3) | 6 | 8 | 8 | 8 | 9 | 8 |
| Apex Limiter | Pro-L 2 (v2.2.6) | 4 | 7 | 7 | 8 | 9 | 9 |
| Crest Compressor | Pro-C 3 (v3.0.2) | 6 | 7 | 8 | 8 | 8 | 8 |
| Prism Multiband | Pro-MB (v1.3.3) | 4 | 6 | 5 | 6 | 7 | 7 |
| Sentry Gate | Pro-G (v1.4.2) | 6 | 7 | 8 | 8 | 9 | 8 |
| Hush De-esser | Pro-DS (v1.3.2) | 4 | 6 | 7 | 8 | 8 | 8 |
| Ember Saturator | Saturn 2 (v2.1.3); Decapitator role (not installed) | 3 | 6 | 6 | 7 | 6 | 7 |
| Aether | Fresh Air (v1.1.7) | 8 | 8 | 9 | 8 | 9 | 8 |
| LYNX Tune | Auto-Tune Pro (v11.6, Auto mode) | 5 | 6 | 7 | 7 | 8 | 7 |

Columns "135" = round 135 (parity135f, `docs/analise/REA_rodada135f.md`): Apex's loudness meter, true / sample peak
metering, display modes, oversampling and side-chain triggering; oversampling in Sentry and Hush; Prism up to six
bands (double-click splits a band), band pan, external side chain; Crest's wet / dry pan; Facet's Flat Tilt / All
Pass shapes and Character; LYNX Tune's Learn (scale from MIDI) and Classic mode.

After = round 128 (per-plugin tables below and `docs/analise/REA_rodada128.md`). The shared header (undo / redo, A/B,
presets) is round 129's PresetBar, reused here (no second system); the scores include it.

## Shared by every editor (the frame around the controls)

All five FabFilter originals share one header and one bottom bar; Fresh Air and Auto-Tune have their own versions of
the same things. This is where most of the "missing details" were: things a user touches before any knob.

| Original feature | Where in the originals | LYNX before | Note |
|---|---|---|---|
| Undo / Redo buttons | header (FabFilter, Fresh Air, Auto-Tune) | ✅ | round 129's shared preset bar (per-plugin undo / redo), wired into every editor incl. Facet (round 128) |
| A/B compare + Copy (A→B) | header (FabFilter: A/B + Copy; Fresh Air: A > B) | ✅ | round 129's preset bar, saved with the project |
| Preset browser: name, previous / next, menu, save | header (all three) | ✅ | round 129's preset bar (factory + user presets, save / delete); round 128 added the factory presets of Facet, Apex, Crest, Sentry, Ember, Aether |
| Help | header (FabFilter "Help", Auto-Tune ⚙ / manual) | ⚠️ | the header shows a one-line gesture hint; no help menu |
| Resizable / full-screen window | FabFilter ⤢ corner + full-screen button, Fresh Air corner | ⚠️ | round 128: every display-first editor window resizes (70–200 %, scaled); Facet resizes with its own layout; no full-screen button |
| MIDI Learn | FabFilter bottom bar | ⚠️ | per control: right-click → "Link to controller" (LYNX's automation menu); no global learn mode |
| Right-click any control: type value / reset / automate | FabFilter: right-click menu | ✅ | right-click → Type value, Reset, automation / controller link |
| Double-click / Alt-click = reset, Ctrl-drag = fine, wheel | FabFilter | ✅ | same contract |
| Hover read-out of a value | FabFilter | ✅ | value tip while dragging / wheel; hint bar |
| Input / output level and pan (bottom bar "0.0 dB 0.0 dB") | FabFilter bottom bar | ⚠️ | in / out levels on some engines (see each table); no pan |
| Global bypass | FabFilter bottom bar (⏻), Fresh Air POWER, Auto-Tune ⏻ | ✅ | the slot's power LED in the header (Aether: the POWER button) |
| Oversampling menu | bottom bar | ⚠️ | per engine (see tables) |

## Facet EQ ← Pro-Q 4

Parameters of the original: 24 bands × (used, enabled, frequency 10 Hz–30 kHz, gain ±30 dB, Q 0.025–40, shape
(Bell, Low Shelf, Low Cut, High Shelf, High Cut, Notch, Band Pass, Tilt Shelf, Flat Tilt, All Pass), slope 0–96
dB/oct + Brickwall, stereo placement (Left, Right, Stereo, Mid, Side), dynamic range, dynamics on / auto, threshold,
attack, release, external side chain, side-chain filtering, spectral mode, density, tilt, solo) + processing mode,
resolution, character, gain scale, output level / pan / pan mode, phase invert, auto gain, analyser options, display
range.

| Original feature | LYNX | Note |
|---|---|---|
| Full-window spectrum + EQ curve, nodes on the curve | ✅ | |
| Double-click empty space = new band; drag = frequency / gain; wheel = Q | ✅ | |
| Bands | ⚠️ | 8 bands (original: 24) |
| Shapes | ✅ | Bell, Low/High Shelf, Low/High Cut, Notch, Band Pass, Tilt Shelf; round 135: Flat Tilt (a constant slope over ±5 octaves) and All Pass (phase only), translated from Pro-Q 4 states (were a tilt shelf / dropped) |
| Slopes | ⚠️ | 6–96 dB/oct; no 0 dB/oct, no Brickwall |
| Floating band panel: shape, slope, FREQ, GAIN, Q, band ◀ n ▶, close | ✅ | |
| Band panel: power button (bypass one band) | ✅ | round 128: the band's coloured power button (also in the node tip and the band menu); a band switched off keeps its settings and shows a hollow node |
| Band panel: stereo placement (L / R / Stereo / M / S) | ✅ | round 128: placement button in the panel and the band menu, an L / R / M / S tag beside the node; in the natural / linear phase modes a placed band runs minimum phase after the FIR |
| Dynamic EQ: range, threshold, attack, release | ⚠️ | attack / release are global (original: per band); no "auto" threshold |
| Dynamic EQ: external side chain per band, side-chain filtering | ⚠️ | one detector switch (input / side chain) for all bands |
| Spectral dynamics (density, tilt) | ✅ | **round 152c** [EXEC] (Pro-Q 4 trial, black-box: Spectral / Density / Tilt host parameters, partials and pink noise through a dynamic bell): **Spectral** in the band panel (beside Dynamic; right-click: density 0 … 100 %) and **Tilt** — an STFT stage (4096 points, latency 5120 samples = the trial's): each bin's level weighted by the band's shape, a threshold 30.5 dB under the knob and never lower than the band's loudest bin − 11.6 dB, 1.07 dB per dB up to the range × shape^2.18 (0.33 dB rms over 22 measured cases); density blurs the gains (100 %: a pure tone −11.2 dB, as the trial), tilt slopes the threshold 4.5 dB/oct [model] |
| Solo band (audition) | ✅ | |
| Hover tooltip next to a node (freq, gain, Q, power, solo) | ✅ | read-out box at the node with power (round 128), solo and remove |
| Right-click a node: shape, slope, dynamic range, solo, reset, remove | ✅ | |
| Processing: Zero Latency / Natural Phase / Linear Phase | ✅ | |
| Linear-phase resolution | ✅ | round 144: Low / Medium (default) / High / Very High / Maximum in the Processing menu (with each latency) and in the bottom bar; the FIR is 2 × 2048 … 65536 taps in blocks of 1024, so the latencies are the trial's exactly; the buffers are sized per instance (choosing another resolution builds a new instance); translated from Pro-Q 4 states (value 553, and the character at 554). Was: fixed (LYNX: 4608 samples at 48 kHz). Round 137 [EXEC] the trial's latencies at 48 kHz: natural phase 320 (LYNX 320 ✅); linear phase Low 3072 / Medium 5120 (default) / High 9216 / Very High 17408 / Maximum 66560 = 1024 + 2^11 … 2^16 (no 2^15) — an FIR of 2× that half-length; needs per-instance buffers sized by the resolution |
| Character (Clean / Subtle / Warm) | ✅ | round 135: in the Processing menu (shown after the mode in the bottom bar); LYNX's own soft-knee curves on the output, unity below about −20 dBFS (the reference's law is not measured) |
| Gain scale | ✅ | |
| Output level | ✅ | |
| Output pan / pan mode, phase invert | ✅ | round 128: PAN in the bottom bar (left / right or mid / side), polarity in OUT's right-click menu |
| Auto gain | ✅ | round 128: bottom-bar button; takes back the mean level change of the static curve |
| Analyser: off / pre / post / pre+post | ✅ | |
| Analyser range, resolution, speed, tilt, freeze, collisions, spectrum grab | ⚠️ | round 128: range 60 / 90 / 120 dB, speed (5 steps), tilt 0 – 6 dB/oct, freeze in the analyser menu (saved with the slot); no resolution, collisions or spectrum grab |
| Display range 3 / 6 / 12 / 30 dB | ✅ | saved with the slot since round 128 |
| EQ match (external spectrum) | ⚠️ | **round 152c**: **EQ match** in the bottom bar — capture a reference (played through the slot) and this track from the analyser, then Match 25 … 100 % with up to 2 … 8 bands and 1/12 … 1 octave smoothing: LYNX's own fit (bells, shelves at the ends, in the unused bands; one undo step) [model]; Pro-Q 4's match is not reachable through host parameters, so its rules are not measured; no live side-chain / other-instance reference |
| Piano display (note names on the frequency axis) | ✅ | piano strip toggle at the bottom left; note name of the mouse position |
| Output meter with peak hold | ✅ | |
| Window size | ✅ | resizable |

**After round 144**: configuration 9/10, design 8/10 (the linear-phase resolution row closed; left: 24 bands, spectral dynamics, EQ match, per-band attack / release).

**After round 152c**: configuration 9/10, design 8/10 — spectral dynamics (Spectral / Density / Tilt, measured per-bin law and latency) and LYNX's EQ match close the two ❌ rows; left: 24 bands, per-band attack / release and side chain, brickwall slopes, a live reference for the match.

## Apex Limiter ← Pro-L 2

Parameters of the original: Gain 0–30 dB, Style (Transparent, Punchy, Dynamic, Allround, Aggressive, Modern, Bus,
Safe), Lookahead 0–5 ms, Attack 0–10 s, Release 0–10 s, Channel Link Transients / Release 0–100 %, Oversampling
Off–32x, True Peak Limiting, Dithering Off / 16–24 bits, Noise Shaping (None, Basic, Optimized, Weighted), Filter DC
Offset, Side Chain Triggering, Unity Gain, Audition Limiting, Output Level −30…0 dBTP, meter scale (−16/−32/−48 dB,
K-12/14/20, Loudness), loudness meters (momentary / short term / integrated, target, recording).

| Original feature | LYNX | Note |
|---|---|---|
| Display = input / output level / gain reduction history, biggest reductions tagged | ✅ | |
| GAIN slider on the display's left edge | ✅ | |
| Output (ceiling) line, draggable | ✅ | |
| Style button / menu | ✅ | 8 styles (round 128: Clean, Glue, Punch, Dynamic, Dense, Loud, Bus, Safe — LYNX's own names and release laws) |
| LOOKAHEAD knob | ✅ | 0–5 ms |
| ATTACK knob | ✅ | round 128: 0–10 s (how slowly the slow release stage engages; 0 = at once, the old behaviour) |
| RELEASE knob | ✅ | |
| CHANNEL LINKING: TRANSIENTS and RELEASE knobs | ✅ | round 128: two knobs; the release is linked at least as much as the transients |
| Knob panel folds away («) | ✅ | |
| ADVANCED panel (»): channel link details, DC offset filter, side-chain triggering | ✅ | round 128: » fold-out with DC offset filter, unity gain, audition limiting; round 135: side-chain triggering |
| True Peak Limiting (bottom bar) | ✅ | TP button + bar menu |
| Oversampling (bottom bar) | ⚠️ | round 135: Off / 2x / 4x / 8x in the bottom bar (the limiter runs at the high rate; latency + the oversampler's); the original goes to 32x |
| Dither + noise shaping (bottom bar) | ✅ | round 128: Off / 16–24 bits, TPDF; None / Basic / Optimized / Weighted error-feedback shaping |
| Output level read-out (bottom bar "Out: 0.0 dBTP") | ✅ | |
| Unity Gain (monitor at the same loudness) | ✅ | round 128 (Advanced) |
| Audition Limiting (hear what is removed) | ✅ | round 128 (Advanced) |
| Meters: output peak bars with holds + gain-reduction meter | ✅ | |
| Meter scale menu (−16/−32/−48 dB, K-12/14/20) | ✅ | round 128: next to TP; rescales the display and the meters |
| Loudness meter (LUFS momentary / short term / integrated, range, true peak) | ✅ | round 135: LOUDNESS button on the display → a panel at the right: short-term and momentary bars with the target line (−23 … −9 LUFS), integrated, short term, momentary, range (LU) and peak read-outs, Reset; ITU-R BS.1770-4 / EBU R 128 (K-weighting, 400 ms / 3 s, gated integration, EBU Tech 3342 range), checked on the EBU test cases |
| True-peak / sample-peak metering switch | ✅ | round 135: in the loudness panel; the output meter and its hold follow it (4x true-peak interpolation) |
| Display mode (Slow Down / Fast / Slow / Infinite / Off) | ✅ | round 135: menu beside the meter scale — Fast (10 ms a pixel), Slow, Slow down (recent fast, the past compressed), Infinite (0.5 s a pixel, ~11 minutes), Off (meters only) |
| External side chain | ✅ | round 135: "Side-chain triggering" (Advanced): the slot's side chain triggers the limiting, the ceiling still holds the programme |

## Crest Compressor ← Pro-C 3

Parameters of the original: Style (Clean, Versatile, Smooth, Punch, Upward, TTM, Op-El, Vari-Mu, Classic, Opto,
Vocal, Mastering, Bus, Pumping), Threshold −60…0 dB + Auto Threshold (+ lock), Ratio 1–100:1, Knee 0–72 dB,
Range 0–60 dB, Attack 0.005–250 ms, Release 10 ms–2.5 s + Auto, Lookahead, Hold, Character (Off, Tube, Diode,
Bright) pre / post + drive, Wet / Dry gain and pan, Auto Gain, side chain (Internal, External, Host Sync, MIDI; level;
host trigger sync / offset / length), stereo link 0–100 % + mode (Mid, Side, M→S, S→M), side-chain EQ (6 bands, 10
shapes), audition side chain / triggering, Mix 0–200 %, input / output level and pan, oversampling Off–32x, maximum
lookahead, meter scale, knee display, input meter.

| Original feature | LYNX | Note |
|---|---|---|
| Full-window level display (input, output, gain-reduction line, threshold) | ✅ | |
| Knee / transfer panel sliding out from the left | ✅ | |
| THRESHOLD, RATIO, style button, ATTACK, RELEASE knobs floating on the display | ✅ | |
| AUTO threshold | ✅ | round 128: the threshold follows the programme (3 s RMS of the detector input; the knob's −18 dB = at the average level) |
| AUTO release | ✅ | |
| KNEE, RANGE, LOOKAHEAD, HOLD small knobs | ✅ | |
| Styles | ⚠️ | 11 (no Smooth, TTM, Op-El, Mastering under those names; the translator maps them onto the nearest fitted family — new names would need their own measured families) |
| Character Off / Tube / Diode / Bright, pre / post, drive | ✅ | |
| AUTO GAIN | ✅ | |
| DRY knob (dry level, parallel compression) | ✅ | round 128: DRY under AUTO GAIN (−∞ … +36 dB, the original's law); MIX moved to the bottom bar (0–100 %, the original goes to 200 %) |
| Wet / dry pan (mid / side balance) | ✅ | round 135: WET PAN / DRY PAN in the side-chain panel (left / right balance of each path) |
| SIDE CHAIN panel: listen, level, stereo link, detector EQ with nodes | ✅ | 3 EQ bands (original: 6) |
| Side chain Internal / External | ✅ | |
| Side chain Host Sync / MIDI triggering | ✅ | round 137: side-chain trigger Host sync (note, rate, length — measured on the trial) and MIDI (row re-checked in round 152) |
| Stereo link mode (Mid / Side / M→S / S→M) | ✅ | round 128: "Link mode" in the side-chain panel (Stereo, Mid, Side, Mid > side, Side > mid); translated from Pro-C 3's mid-only link range + mode |
| Audition triggering | ✅ | round 137 / re-checked round 152: SC LISTEN plays the trigger key while a Host sync / MIDI trigger drives the detector (the detector's signal is the trigger) |
| Oversampling (bottom bar) | ⚠️ | up to 8x (original 32x) |
| Lookahead maximum (bottom bar) | ✅ | |
| In / out levels (bottom bar) | ✅ | no pan |
| Meter scale (9/18/36/72/90 dB) | ✅ | round 128: the dB box at the display's bottom right |
| Input level meter switch | ✅ | round 128: click IN above the meters |
| Output + gain-reduction meters with holds | ✅ | |

## Prism Multiband ← Pro-MB

Parameters of the original: up to 6 freely placed bands × (state, low / high crossover + slope 6–48 dB/oct, dynamics
mode compression / expansion, threshold −90…0, range ±30 dB, ratio, attack %, release %, knee 0–48 dB, lookahead
0–20 ms, level ±30 dB, pan, side-chain filtering band / free + low / high, side-chain input, stereo link + mode,
solo / mute) + mix 0–200 %, input / output, processing mode (Linear / Dynamic / Minimum phase), oversampling,
lookahead on, expert mode, display range, analyser options.

| Original feature | LYNX | Note |
|---|---|---|
| Spectrum window with band regions and nodes | ✅ | |
| Click / drag in empty space = create a band anywhere; bands with gaps | ✅ | round 135: double-click the empty display = a new band there (the band under the mouse splits at that frequency, the new one starts as its copy); right-click → Split / Remove the band. Round 150: **Free layout** (Split \| Free beside the display): each band its own low / high edge and slopes, the room between bands passes untouched (measured: Minimum phase = the crossover tree with the gaps at unity, exact); double-click or right-click in a gap = a new band there; drag an edge or the node; Pro-MB states translate to it |
| Up to 6 bands | ✅ | round 135: 1–6 bands (the band count; 4 = the old engine sample for sample); Pro-MB states translate up to six |
| Node: drag up / down = range, left / right = move band; edges = crossovers | ✅ | |
| Wheel = threshold | ✅ | |
| S / M buttons per band | ✅ | |
| Floating band panel: power, THRESHOLD, RANGE, ATTACK, RELEASE, OUTPUT, mode, RATIO, KNEE | ✅ | |
| LOOKAHEAD per band | ✅ | round 128: LOOKAHEAD in the band panel (0–20 ms, capped by the global look-ahead, which sets the latency); translated from Pro-MB |
| EXPERT (»): side-chain filtering, external side chain, stereo link per band | ✅ | round 135: external side chain (Int / Ext: the detectors hear the side chain split by the same crossovers); round 147: **Expert >>** in the band panel — side-chain filtering Band / Free with Low / High (30 Hz – 30 kHz; the free filter measured: 4th-order sides, −24 dB an octave out), **stereo link** 0–100 % and 100 % + mid- / side-only with its Mid / Side mode (measured laws, within 0.25 dB of the trial); translated from Pro-MB states (only when its Expert mode is on, as the reference does). Round 150: **a side-chain input per band** (Global / Plug-in / External, measured: −10.3 dB on the external band, the other untouched), the free filter's **narrow-band boost** (+13.7 dB at the centre of 2 – 2.5 kHz: a centre bell fitted per width, within 0.1 – 0.3 dB) |
| Band output pan (mid / side) | ✅ | round 135: PAN in the band panel (left / right balance of the band); round 144: L/R \| M/S switch in the band panel (mid / side = the reference's law, measured: the far side's gain 1 − \|pan\|), translated from Pro-MB states |
| Crossover slope per band 6–48 dB/oct | ✅ | round 144: the global slope in the band panel; round 147: **each crossover its own slope** (Global / 12 / 24 / 48: a band's Low / High slope in its expert part); round 150: in the free layout **every slope 6 … 48 dB/oct** with Pro-MB's laws [EXEC]: Linear phase continuous (1 / (1 + x^m), m = slope / 6), Dynamic / Minimum phase quantised as the reference (6, 12, 24, 36, 48), 6 dB/oct as its first-order designs (edge shelf, the bell of a 6 / 6 band, the measured 6 / n table) — within 0.01 – 0.08 dB of the trial; translated |
| Processing mode Linear / Dynamic / Minimum phase | ✅ | round 144: Phase menu in the bottom bar; linear = FIR crossovers with the same magnitudes, 3072 samples at 48 kHz (measured); minimum = all-pass compensated sum; translated. Round 147: **Dynamic phase measured and rebuilt** — each working band is a minimum-phase filter with the linear-phase magnitude, the bands in cascade (Pro-MB's −4.1 dB / +53° at the crossover, −8.15 dB for two adjacent −12 dB bands: matched within 0.05 dB / 1°); projects saved before round 147 keep the classic engine ("Classic dynamic" in the bottom bar updates them) |
| Oversampling | ✅ | round 144: Off / 2x / 4x in the bottom bar: the gains applied at the high rate; latency + the oversampler's; translated |
| Display range 3 / 6 / 12 / 30 dB | ✅ | round 128: 3 dB added |
| Analyser off / pre / post / pre+post | ✅ | |
| Analyser resolution / speed / tilt / freeze | ✅ | round 128: the analyser button opens a menu (off / pre / post / pre+post, speed, freeze); round 144: Resolution (Low … Maximum) and Tilt (0 … 6 dB/oct) |
| Mix, In, Out (bottom bar) | ✅ | |
| Output meter | ✅ | |

**After round 144** (REA_rodada144b): configuration 7 → **9**/10, design 7 → **8**/10 — processing modes, oversampling, mid / side band pan, analyser resolution / tilt, the reference's bottom-bar order. Left: per-band slopes, side-chain filtering, per-band stereo link, the dynamic mode's crossover shape, free band placement.

**After round 147** (REA_rodada147 §1–2): configuration 9 → **10**/10, design 8 → **9**/10 — the measured Dynamic phase (the reference's sound when bands work), the expert panel (side-chain filtering, stereo link, the band's own slopes). Left: free band placement with gaps (a band switched off is the gap), a side-chain input per band, slopes only 12 / 24 / 48 (no 6 or in-between values), the free side-chain filter's extra boost on bands narrower than an octave.

**After round 150** (REA_rodada150): configuration **10**/10, design 9 → **10**/10 — the free layout with gaps (bands placed anywhere, as the reference), slopes 6 … 48 dB/oct with the reference's laws per processing mode, a side-chain input per band, the narrow free side-chain filter, and the **measured detector** (the range's 12 dB soft knee, the release after the curve: a range-limited band recovers at once; release trajectories within 0.1 – 0.7 dB of the trial where the old detector was 1 – 4 dB off; projects saved before keep the old one). Left: the slow attacks' tail (50 – 100 %: 0.5 – 1.3 dB rms), narrow even-order bands in Dynamic phase (0.3 dB at their edges), Linear phase on LYNX's prewarped axis (≤ 0.35 dB at high crossovers).

**After round 152** (REA_rodada152 §1–2): configuration **10**/10, design **10**/10 (sound closer) — the attack in the gain stage (slow attacks 50 – 100 % now within 0.4 dB of the reference over the first 400 ms; round 150 was up to 3 dB off), Dynamic phase's own band shapes for narrow bands and cuts (edges moved out, gain moved — the measured table, ≤ 0.05 dB), the 0.8-octave minimum band width in every mode. Left: Linear phase on LYNX's prewarped axis (≤ 0.35 dB at a 3 kHz edge, 6 dB/oct), mixed 6 / n sides beyond the measured table.

## Sentry Gate ← Pro-G

Parameters of the original: Threshold, Ratio, Range, Style (Classic, Clean, Vocal, Guitar, Upward; Ducking as an
expert style), Attack 0–1 s, Release 0–5 s, Hold 0–250 ms, Knee 0–30 dB, Lookahead 0–10 ms + on, upward threshold /
ratio, side chain (normal / external, per-channel gain and L/R mix, high / low pass, audition), wet / dry level +
pan, oversampling Off / 2x / 4x, expert mode, channel mode L/R or M/S, input / output.

| Original feature | LYNX | Note |
|---|---|---|
| THRESHOLD big knob with RATIO / RANGE under it | ✅ | |
| Display: level history behind the transfer curve, threshold dashed | ✅ | |
| Style button (5 styles) | ✅ | |
| Ducking style | ✅ | round 128: DUCKING in the expert strip (the gate upside down: an open gate cuts by the range); shown under the style button when on |
| ATTACK, RELEASE, HOLD, KNEE, LOOKAHEAD (+ on) | ✅ | |
| EXPERT strip: side chain In / Ext, high / low-pass range slider, Audition | ✅ | |
| EXPERT: side-chain GAIN + L/R MIX (two channels) | ✅ | round 128: GAIN / MIX per detector channel (L / R, or M / S); translated from Pro-G |
| EXPERT: WET + PAN, DRY + PAN | ✅ | round 128: WET PAN / DRY PAN next to DRY / OUTPUT; translated |
| Channel mode Left/Right or Mid/Side (bottom bar) | ✅ | round 128: bottom bar; the detector channels become mid / side; translated |
| Oversampling (bottom bar) | ✅ | round 135: Off / 2x / 4x / 8x (the gain is applied at the high rate; latency + the oversampler's); the look-ahead switch stays under its knob |
| In / Out levels (bottom bar) | ✅ | round 128: In added (−24 … +24 dB); translated |
| Level meter | ✅ | |

## Hush De-esser ← Pro-DS

Parameters of the original: Mode (Single Vocal, Allround), Threshold −∞…0 dB, Range 0–24 dB, Band Processing (Wide
/ Split), Stereo Link 0–100 % + Mid / Side, Lookahead 0–15 ms + on, Audition Triggering, side chain normal /
external, High-pass frequency 2–20 kHz, Low-pass frequency 2–20 kHz, Audition side chain, oversampling, input /
output level and pan.

| Original feature | LYNX | Note |
|---|---|---|
| Scrolling waveform with the removed part, threshold lines | ✅ | |
| THRESHOLD and RANGE big knobs, listen (headphones) button | ✅ | |
| Frequency slider with TWO handles (high-pass and low-pass of the detection band) | ✅ | round 128: left = where the band starts, right = where it ends (far right = off), 2–20 kHz |
| Audition (hear the detection band) | ✅ | |
| MODE: SINGLE VOCAL / ALLROUND | ✅ | round 128 (Allround: the sibilance must also stand out from the full-band level); LYNX's high-pass / band detection stays in the bottom bar |
| WIDE BAND / SPLIT BAND | ✅ | |
| SIDE CHAIN In / Ext | ✅ | round 128: Ext picks the source track (menu) |
| STEREO LINK knob with MID switch | ✅ | round 128 |
| LOOKAHEAD knob 0–15 ms with power button | ✅ | round 128 (the look-ahead is the latency; the mixer re-plans its delay compensation) |
| DISPLAY button (hide / show the waveform) | ✅ | round 128: the window shrinks without it |
| Oversampling (bottom bar) | ✅ | round 135: Off / 2x / 4x / 8x for Hush's own voicing (the band and its gain at the high rate; the measured classic voicing has none) |
| In / Out levels (bottom bar) | ✅ | round 128: In ±12 dB, Out ±12 dB (no pan) |
| Release | — | Pro-DS has no release control; LYNX keeps RELEASE as an extra (bottom bar) |
| Input meter with threshold arrow, reduction meter | ✅ | |

## Ember Saturator ← Saturn 2 (and Decapitator's role)

Parameters of Saturn 2: up to 6 bands × (style — 28: Subtle / Clean / Warm / Broken Tube, Subtle / Clean / Warm /
Old Tape, 9 amps, Subtle / Gentle / Heavy Saturation, 3 transformers, Smudge, Breakdown, Foldback, Rectify, Destroy —,
drive + drive pan, feedback amount + frequency, dynamics −1…+1, tone Bass / Mid / Treble / Presence ±24 dB, mix,
level, pan, enabled, solo / mute, crossover + slope 6–48 dB/oct) + global input / output gain and pan, mix, channel
mode L/R or M/S, processing mode (minimum / linear phase), high-quality mode, unrestricted feedback, and the
modulation system (6 XY controllers, 6 XLFOs with 16 steps, 6 envelope generators, 4 envelope followers, 10 MIDI
sources, 50 modulation slots).

| Original feature | LYNX | Note |
|---|---|---|
| Spectrum with bands side by side, split by clicking / double-clicking | ✅ | |
| Drag a band's handle = drive, drag a crossover | ✅ | |
| Up to 6 bands | ✅ | round 150b: six (the Bands button in the bottom bar: 1 … 6; double-click splits up to six; crossovers 4 / 5 appended); translated from Saturn 2 states with 5 / 6 bands |
| Floating band panel: power, style button, MIX, FEEDBACK + FREQ, DYNAMICS, big DRIVE, TONE faders, LEVEL | ✅ | round 128: the same order — power / solo / mute, style, MIX, FEEDBACK, FREQ, DYNAMICS, DRIVE, BASS / MID / TREBLE / PRESENCE faders, LEVEL |
| 28 styles in 6 families | ✅ | round 152b: Breakdown measured black-box — a pitch drop of 24 semitones × drive at every level and frequency, its level curve refitted (0.02 – 0.37 dB rms over seven levels): LYNX's "Collapse" now pitches down the same way (projects saved before keep the static stand-in); the amps' level "memory" re-measured at 2 ms resolution: it settles within 20 ms (≤ 1.5 dB after 10 ms, none from 50 ms), so nothing to model; Smudge (a smeared, delayed, modulated copy) and Destroy (a level-dependent quantiser) stay static stand-ins. Round 150b: a per-band **Character** menu with 28 LYNX-named voicings measured black-box on the trial's 28 styles (static curve, its drive law over 0–100 %, level, and the tone: 0–1.2 dB rms on the level curves and 0–1.4 dB rms on the tone for the 23 saturation / amp / transformer styles); the 5 effect styles: Foldback ≈ a sine fold (fit ≤ 1.3 dB to 60 % drive), Smudge / Destroy level-only, Breakdown / Rectify not static in the reference (LYNX: a gentle saturation / a full-wave rectifier); translated per band from Saturn 2 states. The 4-family Style menu stays (Character Off) |
| Per-band tone: Bass / Mid / Treble / Presence | ✅ | round 128: ±18 dB (Saturn ±24) on the band's saturated signal — LYNX's own curves (shelf 120 Hz, bell 800 Hz, shelf 3.5 kHz, bell 6 kHz); the global tilt / cuts stay as LYNX extras in the bottom bar |
| Per-band power, solo, mute | ✅ | round 128 (panel buttons, tags on the display) |
| Drive pan (L / R), band pan | ✅ | round 147b: per band DRIVE PAN (beside the drive) and PAN (under the level) [EXEC trial: the far channel's drive × (1 − \|pan\|) on the 0–100 % scale; the band pan's far side 1 − \|pan\|]; translated from Saturn 2 states |
| Feedback amount / frequency | ✅ | round 128: a band-passed loop around the shaper (10 Hz – 1 kHz, loop gain up to 0.9) |
| Dynamics (compress / expand the band) | ✅ | round 128: −1 … +1, the band's level follows its input level |
| Modulation (+ Add Modulation: XLFO, EG, EF, MIDI, XY, slots) | ⚠️ | round 152b: a **Mod** page in the band panel — two LFOs (rate 0.02 – 500 Hz with the reference's knob law; Sine / Triangle / Square / Ease in / Ease out = its two-step XLFO with the four glide functions or none), an envelope follower (attack 1 ms – 1 s, release 50 ms – 5 s, the reference's laws; mean-square follower fitted to 0.018 source units rms) and four slots source → target × amount (27 targets: in / out gain, mix, each band's drive / level / mix, the five crossovers; the reference's level law and target ranges) [EXEC trial]; translated (XLFO 1 / 2, EF 1, the first four usable slots). Not: the 16-step editor, EGs, XY, MIDI sources, 50 slots |
| Crossover slope (bottom bar) | ✅ | global; round 152b: each crossover its own slope (6 / 12 / 24 / 36 / 48 dB/oct or Global) in the band panel [EXEC trial: minimum phase = the first-order pair, LR2, LR4, LR6, LR8 — within 0.05 dB]; translated per band |
| Linear phase / minimum phase | ✅ | round 152b: "Linear phase" in the band panel [EXEC trial: zero-phase bands 1 / (1 + (f/fc)^(slope/6)) on the analog axis, the bands sum to the delayed input; latency 3072 samples at 48 kHz] — LYNX's oversampler adds 55 samples; translated (state slot 696) |
| High Quality mode (oversampling) | ✅ | "Oversampling 2x / 4x / 8x" |
| Channel mode M/S | ✅ | round 147b: "Channel L/R \| M/S" in the bottom bar [EXEC trial: M = (L + R) / 2, S = (R − L) / 2 through the bands' two channels; drive pan and band pan then balance mid / side]; translated (state slot 695) |
| In / Out gain + mix | ✅ | round 128: In (±24 dB) in the bottom bar; Out; Dry / wet |
| Input / output pan | ✅ | round 150b: In pan / Out pan in the strip [EXEC trial: the far side × (1 − \|pan\|); input pan before the drive, output pan after the mix; with M/S they act on mid / side]; translated (state slots 1 / 3) |
| Auto gain | — | LYNX extra (Saturn has none; Decapitator has Auto) |
| Decapitator: Drive, 5 styles (A, E, N, T, P), Low Cut, Tone, High Cut, Thump, Steep, Mix, Output, Auto, Punish | ⚠️ | trial not installed; Ember covers drive / tone / cuts / mix / auto gain, no Thump / Steep / Punish |

**Round 147b:** configuration 6 → 7 (channel mode M/S, drive pan, band pan, measured on the trial and translated), design 7 → 7. Remaining: 6 bands, 28 styles, modulation, linear phase.

**Round 150b (after 150):** configuration 7 → 8 (six bands, the 28 measured characters, input / output pan, all translated), design 7 → 8 (Character menu in the band panel, Bands 1 … 6, In / Out pan in the strip). Remaining: modulation, linear phase, per-band crossover slopes, the effect styles' time-varying behaviour.

**Round 152b (after 152):** configuration 8 → 9 (each crossover's slope, linear phase, a modulation subset with the reference's laws, Breakdown's pitch drop; all translated), design 8 → 8 (the Mod page and the slope / linear-phase controls in the band panel; Saturn's modulation is drawn as draggable sources and slot cables — LYNX lists them). Remaining: the step-LFO editor (16 steps), EGs / XY / MIDI sources, Smudge / Destroy, the High Quality mode's latencies (Saturn: 0 / 8 / 9 samples; LYNX always oversamples).

## Aether ← Fresh Air

Parameters of the original: Mid Air 0–100 %, High Air 0–100 %, Bypass, Trim −12…0 dB.

| Original feature | LYNX | Note |
|---|---|---|
| Arc output meter on top | ✅ | |
| MID AIR and HIGH AIR big knobs | ✅ | |
| Link button between them | ✅ | |
| POWER (bypass) bottom left | ✅ | |
| TRIM small knob bottom right | ✅ | |
| Preset menu, save, undo / redo, A > B | ✅ | the shared preset bar (round 129) + factory presets (round 128) |
| Extra settings (exciter / shelf frequency, character, mix) | — | LYNX extras in the bottom bar |

## LYNX Tune ← Auto-Tune Pro (Auto mode)

Auto-mode parameters of the original: Correction mode, Scale (29 legacy + 15 modern), Key, Detune ±100 cents, Retune
Speed 0–400 ms, Humanize, Flex-Tune, Natural Vibrato ±12 dB, Tracking, Input Type (Soprano, Alto-Tenor, Low Male,
Instrument, Bass Inst.), Classic mode, Formant Correction + Throat Length 50–180 %, Transpose ±12 (+ in scale),
Wet-Dry Mix, Create Vibrato (shape, rate, pitch, amplitude, formant, variation, onset delay / rate), Ignore Vibrato,
Learn Scale from MIDI, Target Notes via MIDI, MIDI octaves, Latency Removal, per-note Bypass / Remove, keyboard
edits (Remove / Bypass, Latched / Momentary), Hold, Auto Key.

| Original feature | LYNX | Note |
|---|---|---|
| Top strip: Tracking, Input Type, Key, Scale, Formant, Transpose, Detune, Mix | ✅ | Detune shown as the A4 frequency (440 Hz ± 100 c) |
| Retune Speed, Flex-Tune, Humanize big knobs | ✅ | |
| Natural Vibrato knob (±12 dB) | ✅ | round 128: right of the ring, as in the original |
| Centre: correction ring with the note name and cents | ✅ | |
| Hold (freeze the read-out) | ✅ | round 128: click the ring (HOLD lights) |
| Scales | ⚠️ | 16: the modern list (chromatic, major, minor, harmonic / melodic minor, modes, pentatonics, blues, whole tone, diminished) + custom; no historical tunings (Pythagorean, just, 19/24/31/53-tone …) |
| Auto Key (detect the key) | ✅ | round 128: button under Scale; key + major / minor from what was sung while the editor is open (pitch-class time, Krumhansl–Kessler profiles) |
| Learn (scale from MIDI) | ✅ | round 135 (the original's "Learn Scale from MIDI" switch, read from its parameter list): LEARN SCALE (correction section, top right): while on, the notes played on the MIDI channel build the custom scale (switching it on starts a new one) and are not used as targets |
| Keyboard at the bottom: click a note = remove / bypass, latch | ✅ | round 128: four-octave keyboard with BYPASS / REMOVE modes and LATCH; a key edits its note name in every octave (no single-octave edits) |
| Per-note Bypass (pass uncorrected) vs Remove | ✅ | round 128 (translated from the state's legacy scale edits) |
| Classic mode | ✅ | round 135: CLASSIC (correction section, top left): the short-latency path with the same retune. [EXEC] Auto-Tune Pro hosted black-box: "Use Classic Mode DSP" leaves the retune trajectory unchanged (452 Hz at 150 ms: the same pitch at every point to 0.1 Hz) and drops the latency from 2670 to 37 samples; LYNX's classic = its low-latency mode |
| Formant Correction on / off | ✅ | |
| Throat Length | ⚠️ | "Formant shift" in semitones instead of a throat length % |
| Transpose ±12 | ✅ | |
| Mix | ✅ | |
| ADVANCED: Create Vibrato (shape, rate, depth, onset), Ignore Vibrato, scale detune | ⚠️ | round 128: ADVANCED page with Create Vibrato (shape, rate, depth, onset delay / rate) and Ignore Vibrato, translated; no amplitude / formant / variation of the created vibrato, no per-note scale detune |
| Target notes via MIDI | ✅ | "MIDI channel" |
| Low latency | ✅ | |
| Sound: pitch accuracy, timbre (highs, formants), bursts | ⚠️ | round 165j [RUN] on her takes: tighter pitch than Auto-Tune (median 4.4 vs 5.8 c), highs now within 0.1 dB of Auto-Tune (normalised grain overlap-add + 3 kHz shelf; was 0.45 dB darker), formant distance 0.78 vs 0.49 dB, short bursts 40 vs 31 / min (note changes / onsets) |
| Pitch graph | ✅ | LYNX extra in Auto mode (input vs corrected) |
| Graph mode (offline editing) | — | out of scope (a separate editor) |
| Harmony Player | — | out of scope |
| Preset browser, undo / redo | ✅ | the shared preset bar (round 129) with LYNX Tune's factory presets |

---

# Waves / SSL / Valhalla / Soundtoys roles (round 129)

Trials: Waves (WaveShell 17.1, demo mode) and Valhalla DSP (demos) hosted here; the reference screenshots are from
rounds 100 / 101 / 113 / 119 and this round (outside git: `D:\lynx-work\parity129\ref`, `D:\lynx-test-archive`).
The Soundtoys 5 trials were installed during this round but every one of them stops on its iLok activation window
until the user starts the trial, so their tables were from the official manuals ("manual only"). Round 135: the user
activated them; the Decapitator, PanMan, Little Plate and Little AlterBoy tables are now from the trials (black-box).
Late in the round (after the user activated the Waves trials) the Waves shell loaded again and RComp, DeEsser,
Doubler4, Waves Tune Real-Time, SSL E-Channel, H-Delay and Doubler2 were captured; the tables above agree with those
captures (RComp: Thresh / Gain faders on the input / output meters, ARC · Electro · Warm, Mix, Trim — ARC / Electro /
Warm still missing in LYNX's Compressor; Waves Tune RT: Speed / Note Transition / Tolerance / Vibrato / Correction and the
range buttons — see its table). Our host still shows their demo banner.

| LYNX engine | Original (trial here) | Config before | Design before | Config after | Design after |
|---|---|---|---|---|---|
| Glow Leveler | CLA-2A (Waves) | 5 | 6 | 7 | 7 |
| Ridge Strip | SSL E-Channel (Waves) | 6 | 6 | 7 | 7 |
| Compressor (stand-in) | Renaissance Compressor (Waves) | 2 | 1 | 5 | 5 |
| Hush De-esser | DeEsser (Waves) | 7 | 5 | 8 | 5 |
| Quell Denoiser | NS1 (Waves) | 8 | 8 | 9 | 8 |
| Gemini Doubler | Doubler2 / Doubler4 (Waves) | 7 / 4 | 6 | 8 / 5 | 7 |
| Echoform | H-Delay (Waves) | 5 | 4 | 7 | 5 |
| LYNX Tune | Waves Tune Real-Time | 4 | 5 | 5 | 5 |
| Epoch Verb | ValhallaVintageVerb | 7 | 8 | 9 | 8 → 9 (r135) |
| Atrium | ValhallaRoom | 7 | 6 | 9 | 8 |
| Foil Plate | ValhallaPlate | 8 | 9 | 9 | 9 → 10 (r135) |
| Nebula | Valhalla Supermassive | 7 | 8 | 9 | 9 |
| Halo | ValhallaShimmer | 6 | 8 | 7 → 8 (r135) | 8 → 9 (r135) |
| Horizon | ValhallaFutureVerb | 6 | 7 | 7 → 8 (r135) | 7 → 9 (r135) |
| Echoform | ValhallaDelay | 6 | 6 | 7 | 7 |
| Spiral Echo | ValhallaFreqEcho | 9 | 9 | 9 | 9 |
| Orbit | ValhallaSpaceModulator | 7 | 7 | 8 | 7 → 9 (r135) |
| Lattice Mod | ValhallaUberMod | 6 | 7 | 7 | 7 → 9 (r135) |
| Ember Saturator | Decapitator (Soundtoys; trial measured in round 135) | 4 | 4 | 5 → **8** (r135) | 4 → **7** (r135) |
| Fruity Delay 3 (stand-in) | EchoBoy / PrimalTap (Soundtoys; trial measured in round 135) | 2 | 2 | 5 → **7** (Echoform, r135) | 5 → **6** (Echoform, r135) |
| Auto-pan (stand-in) | PanMan (Soundtoys; trial measured in round 135) | 2 | 2 | 3 → **7** (r135) | 2 → **6** (r135) |
| Reeverb 2 (stand-in) | Little Plate (Soundtoys; trial measured in round 135) | 4 | 3 | 6 → **8** (Foil Plate, r135) | 5 → **6** (Foil Plate, r135) |
| Voice Shift (round 135) | Little AlterBoy (Soundtoys; trial measured in round 135) | 0 | 0 | 0 → **9** (r135) | 0 → **8** (r135) |

Scores "after" = with round 129's changes (below). The biggest single gap was the frame every original has around its
controls — undo / redo, A / B, presets — which none of LYNX's engines had; it is now in every LYNX engine editor.

**Round 135w (Waves roles + Echoform; `docs/analise/REA_rodada135w.md`)** — config / design after round 135w:

| LYNX engine | Original | Config 129 → 135w | Design 129 → 135w | What changed |
|---|---|---|---|---|
| Compressor (stand-in) | Renaissance Compressor | 5 → 8 | 5 → 7 | ARC / Manual, Electro / Opto, Warm / Smooth, output limiter + pill, all translated |
| Hush De-esser | DeEsser | 8 → 9 | 5 → 6 | output meter with clip lamp / peak hold, the reference's five frequencies as LYNX presets |
| Gemini Doubler | Doubler2 / Doubler4 | 8 / 5 → 9 / 8 | 7 → 7 | voices 3–4 (page), modulation RESET, clip lamp |
| Echoform | H-Delay | 7 → 9 | 5 → 7 | own BPM sync, ping-pong start side, filter link, output meter + clip lamp |
| LYNX Tune | Waves Tune Real-Time | 5 → 8 | 5 → 6 | "Real-time" page: Speed + Link, tolerance cents / time, vibrato on / depth, correction %, IN / OUT meters |
| Echoform | ValhallaDelay | 7 → 8 | 7 → 7 | Quad taps A–D, Repeat / Swell |
| Echoform | EchoBoy / PrimalTap | 5 → 7 | 5 → 6 | Groove / Feel (measured on the trial), Accent, Prime numbers, X-feed, Balance, Freeze, own BPM |

## Round 129: what changed

- **Every LYNX engine editor** (the generic panels and the display-first editors) has the originals' header: undo / redo
  of that plugin's own edits, A / B with copy to the other setting, the preset name with ◀ ▶ and a menu of LYNX's
  factory presets (90, own names and settings, in folders), the user's presets (Save preset…, Delete, Show the presets
  folder) and "Keep the Mix when loading presets" (Valhalla's mix lock). A / B and the preset name are saved with the
  project. LYNX Tune's presets leave its key, scale and note switches alone.
- **Glow Leveler** (CLA-2A): the VU DISPLAY selector (input / GR / output), TRIM ±18 dB, AUTO MAKEUP, the bottom strip
  of the reference (HF emphasis, Mix, Trim).
- **Ridge Strip** (SSL E-Channel): the GAIN block's output fader beside an input / output meter with peak read-outs;
  dynamics before the EQ by default, as on the reference.
- **Compressor** (the Renaissance Compressor's stand-in): its own panel — threshold fader by the input / output
  meters, the gain-reduction meter with the ratio under it, the gain fader, attack / release, MIX and TRIM (it had a bare row
  of knobs and no meters).
- **Quell Denoiser** (NS1): the reduction meter now moves (the engine's meter was never published to the editor).
- **Gemini Doubler** (Doubler): output level meters beside the output fader.
- **Echoform** (H-Delay / ValhallaDelay): Phase L / Phase R (translated from H-Delay states), a tap-tempo pad, the
  modulation section named after the mode (Mod / Freq / Pitch) and "Delay L" in dual / ping-pong.
- **Atrium** (ValhallaRoom): the Early / Late controls on two tabs like the reference; "Depth" for the early / late
  balance. **Nebula** (Supermassive): the CLEAR button. **Foil Plate**: EQ defaults 700 Hz / 10 kHz. **Halo**: the low
  cut shows what the engine applies. **Lattice Mod**: starts with two taps like UberMod.
- **Substitutes**: Soundtoys EchoBoy / PrimalTap → Echoform (was Fruity Delay 3 at FL's default), Little Plate → Foil
  Plate (was Reeverb 2), PanMan's Auto-pan really sweeps (its depth was 0 %).

Still missing (ranked): the per-control description line of the Valhalla panels; Doubler4's voices 3–4; H-Delay's
own-BPM sync, LoFi switch and Analog 0–4; Ridge's Split / Dyn S.C. routing and Analog; RComp's ARC / Opto / Warm (a
re-route to Crest needs the vocal-chain refit); Waves Tune Real-Time's Speed / Tolerance / Vibrato (LYNX Tune is being
extended by round 128c); Decapitator's Punish / Thump / Steep and five styles; EchoBoy's Rhythm mode; PanMan's
rhythm / trigger sources; a Little AlterBoy role. Soundtoys and the newer Waves captures need the user (iLok
activation; the Waves shell hangs in our host since this round).

## The frame shared by every original (Waves bar, Valhalla preset row, Soundtoys control strip)

| Original feature | Where in the originals | LYNX before | Note |
|---|---|---|---|
| Undo / Redo of the plugin's own edits | Waves bar (every Waves plugin) | ⚠️ → ✅ | round 129: ↶ ↷ in every LYNX engine's header |
| A/B compare (Setup A / B) + copy A→B | Waves bar; Soundtoys strip | ❌ → ✅ | round 129: A, B and A→B / B→A, saved with the project |
| Preset name, ◀ ▶, menu of factory presets | Waves bar; Valhalla "PRESET: Default ◀ ▶" row; Soundtoys strip | ❌ → ✅ | round 129: LYNX factory presets in folders (FL presets stay listed for FL effects) |
| Save / load the user's own presets | Waves Save / Load; Valhalla preset menu (save, copy); Soundtoys | ❌ → ✅ | round 129: .lynxpreset files per engine |
| Name of the preset a project was saved with | Waves / Valhalla show it | ❌ → ✅ | LYNX presets' names are saved; round 135: a translated original's preset name is read too (Valhalla `presetName`, Waves `<Preset Name>`) and shown in the header |
| Per-control description line inside the window | Valhalla (bottom strip) | ⚠️ → ✅ | round 135: a strip at the bottom of every LYNX engine editor (generic panels and display-first editors): one sentence about the control under the mouse in LYNX's own words (core/ParamHelp, every control of every LYNX engine, pt-BR), or the current mode / type and what it is while nothing is hovered |
| Mix lock (Mix stays while presets change) | Valhalla (click the MIX label) | ❌ → ✅ | round 129: in the preset menu, per engine; round 135: also a click on the MIX caption (boxed in the accent colour while locked) |
| Typed values, reset, fine drag, wheel | all | ✅ | right-click value entry, double-click / Alt-click reset, Ctrl fine, wheel |
| MIDI learn | Waves (right-click) | ✅ | right-click → automation / controller link |
| Resizable window | Waves ≡ menu, Valhalla corner | ✅ | the generic panels resize at a fixed aspect ratio |

## Glow Leveler ← CLA-2A (Waves)
Sources:
- `docs/specs/cla-2a.md` (§1, §3, §6);
- trial screenshot `/mnt/c/Users/kauan/lynx-test/w119/ref/CLA-2A_Stereo.png`;
- LYNX screenshot `lynx-before/Glow Leveler.png`;
- `glowLeveler()` in PluginLayout.cpp, `glow::Leveler` and `paramText` in MixSuite.cpp, the `vumeter` display in
  PluginEditors.cpp.

Trial on this PC: yes (the new parity129 capture is missing; the w119 capture was used).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Header: Undo / Redo | WaveSystem bar, top left | song-level Ctrl+Z only | ✅ | round 129's preset bar (per-plugin undo / redo) |
| Header: Setup A/B + A→B copy | bar ("A→B", "Setup A") | none | ✅ | round 129's preset bar: A / B and copy, saved with the project |
| Header: preset name + Load menu | bar ("A: Start Me Up"), factory presets by CLA | presets box lists FL presets only, "(no preset)" | ✅ | no factory or user presets for Glow; round 129: LYNX factory presets for Glow + user presets |
| Header: Save | bar | none | ✅ | round 129: Save preset… (.lynxpreset) |
| Header: ≡ menu (help, resize) | bar, right | options menu (Presets, Reset, Bypass); window resizable | ⚠️ | |
| Bypass | VST param | slot LED / options menu | ✅ | |
| GAIN knob | 0–100 scale, default **40** (≈ +8 dB below threshold), left of the VU; nonlinear law (20 → −18.5 dB, 40 → +8, 80 → +31.8) | "Gain" 0…+30 dB linear, default **+0.0 dB**, big knob on the left | ✅ | round 123: the Opto classic voicing reads 0–100 with the measured law; round 152: a new Glow starts there at **40** (the trial's fresh instance: +8.45 dB at −40 dBFS) |
| PEAK REDUCTION knob | 0–100, default **46**, right of the VU, nonlinear | "Peak reduction" 0–100 (side chain −20…+40 dB linear), default **0** = off, big knob on the right | ✅ | round 123: the classic voicing's measured taper (one to one); round 152: default **46** |
| COMPRESS / LIMIT switch | toggle, left side under the logo; default Compress | "Compress | Limit" tabs under the VU | ✅ | different place (centre, not left) but easy to find; Limit γ = 9 (~10:1) vs about 100:1 on the hardware |
| VU meter | analog VU in the centre; −20…+3 scale, red above 0 | analog VU in the centre, same −20…+3 scale | ✅ | good match |
| VU DISPLAY selector | rotary knob INPUT / **GR** (default) / OUTPUT, top right | hidden: clicking the meter switches GR ↔ Output; no Input | ✅ | no visible selector, and Input is missing; add a 3-position switch beside the VU; round 129: INPUT / GR / OUTPUT selector beside the VU |
| VU readout text | none (needle only) | "Gain reduction −x.x dB" or "Output x VU" inside the face | ➕ | |
| VU calibration | 0 VU = −18 dBFS (spec, unverified) | output mode 0 VU = −18 dBFS | ✅ | |
| HI FREQ (side-chain HF emphasis) | knob 0–100, default **50** (≈ +5–6 dB detector sensitivity at 6 kHz), with a "FLAT" mark; bottom strip | "Emphasis" 0–100 % (+0…+10 dB shelf at 2.5 kHz in the detector), default **0 %**, small knob under the switch | ✅ | round 123: the classic voicing's measured side-chain emphasis and Hi Freq law, translated (RW 153); round 152: default **50** |
| ANALOG | 3 buttons 50Hz / 60Hz / OFF, bottom left; default **Off** (the trial's fresh instance) | none | ✅ | round 152 [EXEC]: **Analog Off \| 50 Hz \| 60 Hz** bottom left — the measured hum (mains harmonics, the 2nd strongest at −98.7 dBFS, both channels alike) and noise (−89.5 dBFS RMS, flat per third octave, L / R uncorrelated) in the wet path after the gain (Trim moves it, Mix 0 % removes it); translated (RW 140) |
| MIX | knob 0–100 %, default 100, bottom strip | "Mix" 0–100 %, default 100 %, small knob | ✅ | round 152: translated (RW 204) |
| TRIM | knob −18…+18 dB, default 0, bottom right (after the mix) | none (Gain is pre-mix and 0…+30 only) | ✅ | new parameter (output trim ±18 dB); round 129: Trim ±18 dB after the mix |
| AUTO MAKEUP | toggle under the VU-display knob, right; default Off | none | ✅ | new parameter: makeup tied to peak reduction; round 129: Auto makeup toggle; round 152 [EXEC]: the classic voicing's measured law (a make-up that follows the Peak reduction dial: −1.5 dB at 0, 0 at 46, +6.8 at 62.6, +11.4 at 80, +16.7 at 100 — not a level follower), translated (RW 207) |
| Mono / stereo components | separate Mono and Stereo plugins | stereo-linked engine | ⚠️ | no mono variant needed in a DAW slot |
| Attack / release | fixed (opto cell, program dependent: about 66 ms near the knee to 4 ms deep; two-stage release 60 ms then 0.5–15 s) | fixed: fast cell 10 ms / 50 ms + slow cell 400 ms / 0.4–5 s with memory | ✅ | round 123: the classic voicing's measured log-domain attack (faster as the reduction grows), hyperbolic two-stage release and memory; round 152 check: the recovery after a loud passage is ≈ 1 dB slower than the trial's on 0.5 s stairs |
| Static curve | γ ≈ 4.54 (≈ 4.2:1 local, 5.5:1 asymptotic) | γ = 2 (≈ 3:1) | ✅ | round 123: the Opto classic voicing's cell law fitted on the trial (γ 2.15 on the side-chain level above its threshold, feedback detection); round 152 [EXEC]: **Limit** measured (a steeper cell, 1.5 % of the input in the side chain — the trial over-compresses at 62.6 — and the cell's deepest reduction, 30.4 dB): 0.34 / 0.52 / 0.50 dB rms at peak reduction 46 / 62.6 / 80 (round 123's Limit 0.37 / 1.00 / 1.32); older projects keep the old Limit (Opto law Old \| Measured) |
| HF tilt under GR / 2nd harmonic | yes (+1…+6 dB above 6 kHz under GR; ≈ −52 dBc H2) | none | ✅ | round 123: the classic voicing's measured tilt (up to +7 dB at 20 kHz under reduction) and 2nd / 3rd harmonics |
| Section layout | Gain left, VU centre, Peak Reduction right; Comp/Limit left; VU selector + Auto Makeup right; bottom strip Analog · Hi Freq · Mix · Trim | Gain left, VU centre, Peak reduction right; Comp/Limit under the VU; Emphasis + Mix small at the bottom | ⚠️ | the main row matches; the bottom strip has 2 of its 4 items, and the right column (selector, auto makeup) is missing |
| Proportions | 659×296 window, big knobs, VU in the middle third | 540×270, same composition | ✅ | |
| Knob scales | 0–100 printed around Gain and Peak Reduction | arc only; the value appears under the knob | ⚠️ | a CLA user reads Gain as 0–100; LYNX shows dB |
| Value entry / fine / reset | Waves value handling | right-click entry, Ctrl fine, double-click reset | ✅ | |
| Saved-state translation | — | Gain (via the measured table), Peak Reduction, Comp/Limit translated; Hi Freq, Analog, Mix, Trim, Auto Makeup and VU ignored | ✅ | PLUGIN_FALLBACK row 140 / round 113 §3c; round 123: Hi Freq, Trim; round 152: Mix (RW 204), Auto Makeup (207), Analog (140); the VU selector is the editor's view only |

**After round 152** (REA_rodada152a §2): configuration 7 → **9**/10, design 7 → **8**/10 — the reference's fresh instance (Opto classic, 40 / 46 / 50), ANALOG 50 / 60 Hz (measured hum and noise), the measured Limit and AUTO MAKEUP, Mix / Auto Makeup / Analog translated. Left: the release tail ≈ 1 dB slower than the trial's after loud passages, Auto Makeup's reference point (LYNX: the dial's default 46; the trial: the dial where it was switched on).

**Scores:** configuration 5/10, design 6/10. The main row is correct. A CLA-2A user misses Trim, Auto Makeup,
Analog, the 3-way VU selector, the 0–100 Gain scale and CLA's defaults (Gain 40 / PR 46 / Hi Freq 50).

**Top gaps:**
1. **Defaults and Gain scale.** Default Gain should give ≈ +8 dB, Peak reduction ≈ 46, Emphasis ≈ the Hi Freq 50
   equivalent. Show Gain as a 0–100 knob value (the dB value can go in the tooltip), following the measured table
   already used in `translateWavesCla2a`.
   Where: `glowInfo()` (MixSuite.cpp), `paramText`. Engine: only the Gain mapping law. Effort S.
2. **VU DISPLAY selector Input / GR / Output** as a visible 3-way switch beside the VU (today a hidden click with
   no Input).
   Where: `glowLeveler()` + the `vumeter` display (PluginEditors.cpp; it needs input-peak live output 2, which the
   engine already sets). UI only; it could be a non-audio parameter so it is saved. Effort S.
3. **TRIM ±18 dB** (post-mix) and **AUTO MAKEUP** toggle.
   Where: new parameters in `glowInfo()`, `glow::Leveler::process`, the layout's right column / bottom strip, and
   the translator (RW 206 / 207). Engine + UI. Effort S.
4. **Rename/relabel Emphasis as "HF emphasis (side chain)"** with a flat point, and translate Hi Freq (RW 153)
   once its law is measured (spec §8 Q4). Engine law + translator. Effort S–M.
5. **ANALOG Off / 50 / 60 Hz** (hum + noise, default Off). Engine (noise generator). Effort M; low priority.
6. **Header: A/B + copy, user presets (Load/Save), per-plugin undo.** These are shared generic-editor work
   (PluginEditors.cpp header) and benefit every engine. UI + state storage. Effort L. Also add a few factory
   presets for Glow.
7. **Engine fidelity** (spec §7: γ ≈ 4.5, level-dependent attack, HF tilt, H2). DSP. Effort M–L. A user hears this
   as "details missing" even when every control is there.

---

## Ridge Strip ← SSL E-Channel (Waves); also SSL Native Vocalstrip 2 (round 145: the Vocal strip voicing)
Sources:
- `docs/specs/ssl-e-channel.md`, `docs/specs/ssl-native-vocalstrip-2.md`;
- Waves "SSL E-Channel & G-Channel User Manual" (assets.wavescdn.com/pdf/plugins/ssl-e-channel.pdf);
- trial screenshot `look114/ref/sslch_main.png` and parameter list `look114/sslch_params.txt`;
- LYNX: `ridgeStrip()` (PluginLayout.cpp), `ridge::` (MixSuite.cpp), `lynx-before/Ridge Strip.png`.

Trial on this PC: yes (older capture used).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Header: Undo/Redo, A/B, A→B, preset Load/Save, name | WaveSystem bar | generic header, "(no preset)" | ✅ | round 129's preset bar |
| Overall layout | left = Filters + EQ (HF top → LF bottom); right = Dynamics (Compress, Expand, GR LEDs), EQ TO / DYN TO routing, Analog, Gain block with meters + Out fader | left = Filters, HF, HMF, LMF, LF, EQ curve; right = Compressor, Expander/gate, Dynamics (GR bars, routing), Gain | ✅ | same reading order and columns |
| HPF | 16–350 Hz, 18 dB/oct, fully left = Out (default Out) | "High-pass" 16–350 Hz 3rd order, 0 = off | ✅ | |
| LPF | 3–22 kHz, 12 dB/oct, fully left = Out | "Low-pass" 22 k→3 kHz 12 dB/oct, 0 = off | ✅ | |
| SPLIT (filters before dynamics) | button under the LPF, default off | "Split" under the low-pass (E console voicing, round 123) | ✅ | round 165: shown only in the E console voicing, where it acts (Ridge's own engine keeps its filters before the dynamics) |
| HF band: gain / freq / BELL | ±16.5 shelf / ±18 bell, 1.5–16 kHz, default 8 kHz; BELL button | gain ±15 (brown) / ±18 (black), 1.5–16 kHz, default 7.87 kHz; Bell toggle | ✅ | round 165: the read-out shows only the gain of the curve in use (was "+6.0 dB / +7.2", both curves at once) |
| HMF: gain / freq / Q | 600 Hz–7 kHz (default 3.5 k), Q 0.1–3.5 (default 1.5), gain ±15…±18 depending on Q | 600–7 kHz (default 2.0 k), Q 0.5–2.5 (default 1.0), gain ±15/±18 | ⚠️ | E console voicing (round 123): the console's Q 3.5 → 0.1 in its direction (measured); Ridge's own engine keeps 0.5–2.5; different defaults |
| LMF: gain / freq / Q | 200 Hz–2.5 kHz (default 500 Hz), Q 0.1–3.5 (default 2.5) | 200–2.5 k (default 593 Hz), Q 0.5–2.5 (default 1.0) | ⚠️ | as HMF |
| LF band: gain / freq / BELL | ±16.5 shelf / ±18 bell, 30–450 Hz, default 60 Hz | ±15/±18, 30–450 Hz, default 89 Hz; Bell | ✅ | default differs |
| EQ curve family | E = "Black" 242 only (G-Channel = 383) | Brown \| Black tabs, default **Brown** | ➕ | E-Channel users expect Black; the translator uses Brown because it was fitted |
| EQ TO: BYP | button, EQ section bypass ("hardware-flat") | "EQ in" toggle | ✅ | |
| EQ TO: DYN S.C. (filters **and EQ** into the side chain) | button | "EQ to side chain" (Dynamics section) | ✅ | **round 165** [EXEC trial]: the filters (unless Split) and the EQ leave the audio and feed only the dynamics' detectors (the compressor's feedback detector and the expander's); both voicings but the 4K; translated (RealWorld 26). The old filters-only "To sidechain" switch shows only in projects that use it |
| DYN TO: BYP | button | "Dyn in" toggle | ✅ | |
| DYN TO: CH OUT (dynamics post-EQ) | button, default **pre-EQ** | "EQ>Dyn \| Dyn>EQ" tabs, default **EQ>Dyn** | ✅ | the default order is the opposite of SSL's (the translator sets Dyn>EQ); round 129: Dyn>EQ is the default, as on the reference |
| Comp RATIO | 1…∞, default 1 (off) | 1 + 19v² … ∞, default 1 (off) | ✅ | |
| Comp THRESHOLD | +10…−20 dBu (= −8…−38 dBFS), default 0 dBu (−18 dBFS); clockwise = lower | −40…0 dBFS, default 0 dBFS; clockwise = higher | ⚠️ | E console voicing: the console's +10 … −20 dB scale (round 123), but clockwise = higher threshold (SSL: lower); Ridge's own engine: dBFS |
| Comp RELEASE | 0.1–4 s, default 0.4 | 0.1–4 s, default 0.29 | ✅ | |
| Comp F.ATK | fast 1 ms; off = auto, program dependent | "Fast attack" 1 ms; off = fixed 10 ms | ⚠️ | no program-dependent attack |
| Auto make-up from ratio + threshold | always on (the manual) | none (the translator adds +0.9 dB output) | ✅ | round 123: the E voicing has the console's measured make-up; round 147b: "Auto make-up" (Compressor section) gives Ridge's own engine the same law (threshold dBFS + 18 = the console's scale, ratio) |
| Comp Mix | — | "Mix" 0–100 % | ➕ | |
| Comp curve display | — | static transfer curve | ➕ | |
| Expander THRESHOLD | −30…+10 dBu (−48…−8 dBFS), default −30 | −80…0 dBFS, default −60 | ⚠️ | E console voicing: −30 … +10 dB, the console's (round 123) |
| Expander RANGE | 0–40 dB, default 0 | 0–60 dB, default 0 (off) | ⚠️ | E console voicing: 0 … 40 dB (round 123) |
| Expander RELEASE | 0.1–4 s, default 0.2 | 0.1–4 s, default 0.29 | ✅ | |
| Expander F.ATK | 1 ms / auto | "Fast" 0.1 ms / 1.5 ms | ⚠️ | |
| GATE switch (Exp default) | button + LED; expander is the default | "Gate \| Expand" tabs, default **Gate** | ⚠️ | default is the opposite |
| Threshold hysteresis | variable (program dependent) | fixed 4 dB (gate) | ⚠️ | |
| GR indicators | two LED ladders (comp, exp) 3/6/10/14/20 dB + gate LED | two LED ladders "Comp" / "Exp" 3 / 6 / 10 / 14 / 20 dB | ✅ | round 165: the reference's ladders (were two bars); no separate gate-open LED |
| ANALOG on/off | switch, centre right; default (her state) Off | none | ✅ | round 147b: "Analog" (Dynamics section) [EXEC trial: Analog on adds white noise of −110.7 dBFS RMS per channel at the output, uncorrelated L / R, whatever the input / output / compressor settings; harmonics unchanged]; both voicings; translated (RealWorld 31) |
| INPUT trim | ±18 dB, knob in the Gain block | "Input" ±20 dB | ✅ | |
| Phase Ø | button + LED | "Phase" toggle | ✅ | |
| OUTPUT fader | fader −24…+12 dB beside the meter | "Output" knob ±20 dB | ⚠️ | knob, not a fader; different range |
| Level meter | stereo bar meter −50…0 dBFS with an INPUT / OUTPUT selector and a numeric peak readout ("−7.2") | none | ✅ | UI (the engine already exports in/out peaks via meters.set); round 129: the GAIN block's input / output meter with peak read-outs |
| EQ response display | — | "Equalizer" curve | ➕ | |
| Latency | 1 sample | 0 | ✅ | irrelevant |
| Vocalstrip 2 (SSL Native) role | de-esser, de-ploser, 3-band EQ, compander with knee/makeup/drive, process order | round 145: Ridge's **Vocal strip voicing** — every control of the reference (34 parameters, its own laws), its own panel (level, compander, de-esser / de-ploser, equaliser, order menu, GR meters), measured on the official trial; her state translated (−60 … −67 dB null on her takes) | ✅ | REA_rodada145 §2; the broadband de-esser / de-ploser on bursts is the least exact part (−13 … −25 dB) |
| Saved-state translation | — | every control (E console voicing, round 123; Analog 147b; Dyn S.C. round 165) | ✅ | round 113 §3c, 123, 165 |

**Round 147b:** configuration 7 → 8 (auto make-up, ANALOG measured and translated), design 7 → 7.

**Round 165** (REA_rodada165h): configuration 8 → **9**, design 7 → **8** — EQ TO DYN S.C. (the filters and the EQ only in the dynamics' side chain, measured: −25 … −27 dB null on a de-essing setting), the console's ∞ : 1 (state value 100, was read as 20 : 1) and its law refitted on the trial's stairs (∞ settings −0.6 … −2 dB null → −21 … −39 dB; 10 : 1 −17 … −22 → −23 … −51 dB), the two GR LED ladders, the gain read-out of the curve in use, Split shown where it acts. Left: the E voicing's compressor threshold turns the opposite way (clockwise = higher), the output fader's −24 … +12 range, a gate-open LED, Expand as the default.

**Scores:** configuration 6/10, design 6/10. An SSL user finds every knob in the expected column. What is missing
or wrong: Split / Dyn S.C. semantics, Analog, auto make-up, the In/Out meter with the output fader, and
threshold units and directions.

**Top gaps:**
1. **In/Out level meter with an INPUT/OUTPUT selector and peak readout** in the Gain block.
   Where: `ridgeStrip()` (a new display id or reuse of a level-meter display) and PluginEditors.cpp. UI only;
   `meters.set` already exports in/out peaks. Effort S.
2. **Defaults to SSL's:** order Dyn>EQ, Expand (not Gate), Black curve for new instances, Q defaults 2.5 / 1.5,
   LMF 500 Hz, HMF 3.5 kHz, LF 60 Hz, comp release 0.4 s.
   Where: `ridgeInfo()`. Caveat: a default change affects existing projects that rely on unsaved defaults; check the
   state loader. Effort S.
3. **Routing semantics:** add a **Split** switch (filters pre-dynamics vs with the EQ). Turn "To sidechain" into
   SSL's **Dyn S.C.** (filters + EQ feed the side chain, audio unchanged).
   Where: `ridge::Strip::process` (new parameter "Split"; change `filtersToSidechain`). Engine. Effort M. Watch
   compatibility: today's "Filters to SC" behaviour must be kept or migrated.
4. **Auto make-up** M(threshold, ratio) (spec §7.4) and **program-dependent attack** when F.ATK is off.
   Engine. Effort M.
5. **Threshold display in dBu (0 dBu = −18 dBFS), SSL direction, ranges +10…−20 (comp) and −30…+10 (exp), range
   0–40.** `paramText` + laws. This is a range change, so the translator must be updated as well. Effort M.
6. **Q knob 0.1–3.5, reversed direction, gain range depending on Q**; show only the active curve's gain in the
   readout. Engine law + `paramText`. Effort S–M.
7. **Analog on/off** (console colour). DSP. Effort M.
8. **Header A/B, presets, undo**: shared generic work (see Glow #6).
9. **Vocalstrip 2 translator** (XML) to Compressor/Crest (spec ssl-native-vocalstrip-2 §7). Effort S.

---

## Compressor (generic stand-in) ← Renaissance Compressor / RComp (Waves); also CLA-76
Sources:
- `docs/specs/renaissance-compressor.md`, `docs/specs/cla-76.md`;
- Waves "Renaissance Compressor User Guide" (assets.wavescdn.com/pdf/plugins/renaissance-compressor.pdf);
- **new trial capture `/mnt/c/Users/kauan/lynx-test/parity129/ref/rcomp.png`** (371×525, latency 64 reported by
  the host in ref1.log);
- LYNX `effectInfo "Compressor"` + `class Compressor` + `formatEffectParam` (Effects.cpp). The panel is the
  generated generic one: a plain knob row Threshold · Ratio · Attack · Release · Makeup · Auto gain, with no meters
  and no display.

Trial on this PC: yes.

What the trial shows (rcomp.png):
- left: **Thresh** slider over a stereo **Input** meter, with numeric peak readouts;
- centre: a large **gain-change meter** (+6…−12 dB, bipolar for expansion) with the **Ratio** value box overlaid,
  and the GR readout below;
- right: **Gain** fader over a stereo **Output** meter, with a **limiter-activity** pill under the value;
- bottom: **Attack** and **Release** horizontal sliders with value boxes; **ARC**, **Electro**, **Warm** toggle
  buttons; **Mix** knob (0–100) and **Trim** knob (±18).
- There is **no transfer-curve graph**.

| Original feature | Original (range / default / where) | LYNX Compressor now | Status | Note |
|---|---|---|---|---|
| Header: Skins menu | Light / Dark / Legacy | — | ➕/n.a. | not needed |
| Header: Undo/Redo, A/B, A→B, Load/Save, preset name | WaveSystem bar ("A: Default Preset") | generic header, "(no preset)" | ✅ | round 129's preset bar |
| Threshold | −60…0 dBFS, default **0**, vertical slider integrated in the input meter | knob −60…0 dB, default **−18** | ✅ | range ✅; round 152: the trial's fresh instance read back (threshold 0.0 dB, ratio 1.00, attack 16 ms, release 160 ms, gain 0) is the factory preset **RComp-style start** (vintage voicing, RComp ranges, limiter on); a new Compressor keeps LYNX's −18 dB so it compresses at once (it is also FL's generic compressor) |
| Ratio | 0.50…50 (below 1 = expansion), default **1.0**, value box inside the GR meter | knob 1…20, default **4:1** | ✅ | round 150: **Ranges = RComp** (LYNX \| RComp tabs): the reference's own ratio control 50 … 1.00 … 0.50 : 1 (its value law, read from the trial), below 1 : 1 the measured upward expansion above the threshold (0.50 / 0.79 / 0.90 : 1 within 0.4 dB); translated states use it |
| Gain (output) | −30…+30 dB, default 0, vertical fader on the output meter | "Makeup" 0…+24 dB | ✅ | round 150: with the RComp ranges the Gain fader runs −30 … +30 dB |
| Attack | 0.5…500 ms, default 16 ms, horizontal slider | 0.1…100 ms, default 10.2 ms | ✅ | round 150: RComp ranges 0.5 … 500 ms (default 16 ms) |
| Release | 5…5000 ms, default 160 ms, horizontal slider | 10…1000 ms, default 120 ms | ✅ | round 150: RComp ranges 5 … 5000 ms (default 160 ms) |
| ARC / Manual | toggle, bottom | none (the translator fakes ARC with a fixed 300 ms) | ✅ | engine; round 135w: ARC / Manual switch (vintage voicing; ARC = the fitted program-dependent release, Manual = one release time [model]); translated (RW 17) |
| Electro / Opto | toggle (GR-dependent release around 3 dB) | none | ✅ | engine; round 135w: Electro / Opto switch (Opto inverts the slow release curve [model]); translated (RW 22) |
| Warm / Smooth | toggle | none | ✅ | engine; round 135w: Warm / Smooth switch (Smooth = ratio as shown, without Warm's extra reduction [model]); translated (RW 16) |
| Mix | 0–100 %, default 100, knob | none (only the DAW slot dry/wet) | ✅ | round 129: Mix knob |
| Trim | −18…+18 dB, post-limiter, knob | none | ✅ | round 129: Trim ±18 dB |
| Built-in 0 dBFS output limiter + activity indicator | always on; pill under Gain | none | ✅ | round 135w: Limiter switch (on for translated RComp states) + the activity pill under it |
| Knee | soft, starts ≈ 6 dB below the threshold (fixed) | hard knee | ✅ | engine; round 123: the vintage voicing (substitutes start there) has the fitted soft knee; Classic stays hard |
| Static curve per ratio | 1.05 … 50 : 1 (20 and 50 alike), knee from ≈ 12 dB under the threshold, the same at every threshold; Warm = Smooth, ARC = Manual, Electro = Opto statically | round 123's fit at her 2.95 : 1 (0.6 … 1.8 dB rms off elsewhere, up to 3.5 dB at 1.5 : 1, 30 dB over) | ✅ | round 152 [EXEC]: **Vintage curve = Measured** (new instances and translated states; older projects keep the round-150 curve, the Old \| Measured tabs above the meter): the trial's stairs at 11 ratios as a correction table — every ratio within 0.07 dB (holdouts 1.2 / 3.3 / 8 / 14 : 1 within 0.06); her state unchanged (≤ 0.01 dB up to −3 dBFS) |
| Input meter (stereo) + peak readouts | left | none | ✅ | round 129: input / output meters with peak read-outs |
| Gain-change meter (bipolar, +6…−12) + readout | centre, the main display | none (no live output) | ✅ | round 129: the reduction meter with its read-out (0…−30, not bipolar); round 152: the **bipolar gain-change meter** — +6 … 0 … −12 dB on the reference's scale (deeper reductions compressed to −24), reduction downwards and the expansion's boost upwards from the 0 dB line, the held largest change under it (click = reset); both voicings |
| Output meter (stereo) + peak readouts | right | none | ✅ | round 129 |
| Latency / look-ahead | 64 samples @ 48 kHz | 0 | ✅ | round 123: the vintage voicing has the measured 64-sample delay |
| Auto gain (power-matched) | — | "Auto gain" toggle | ➕ | |
| External side chain | — | yes (DAW side-chain routing) | ➕ | |
| Gestures | drag, value windows accept typed values | generic contract | ✅ | |
| Saved-state translation | — | threshold (−5 dB), ratio, attack, release (ARC → 300 ms), gain; Warm/Smooth, Electro/Opto, mix, trim not translated | ✅ | round 113 §3c; 0.53 dB RMS on the stairs; round 123 / 135w: threshold, ratio, attack, release, gain, mix, trim, ARC, Electro / Opto, Warm / Smooth, limiter |
| CLA-76 (same stand-in) | Input/Output −∞…0, Attack/Release 1–7, Ratio 4/8/12/20/All, Meter In/GR/Out, Analog, Bluey/Blacky, Mix | Compressor at a fixed preset, no translation | ✅ | round 127: CLA-76 → Crest Compressor (Bus style) fitted on her state, RealWorld list translated (input / output, attack / release 1–7, ratio buttons, mix) |

**Scores:** configuration 2/10, design 1/10. A plain row of 6 knobs with no meters cannot be read as RComp.

**After round 152** (REA_rodada152a §1): configuration 9 → **10**/10, design 7 → **8**/10 — the bipolar gain-change meter, the measured static curve at every ratio, the reference's fresh instance as a factory start. Left: Warm / Smooth's and Opto's transient differences (statically identical on the trial), the meter's ratio box (the ratio is a knob under it).

**After round 150** (REA_rodada150 §6): configuration 8 → **9**/10, design 7/10 — the reference's own control ranges (ratio 0.5 … 50 with expansion, gain ±30, attack 0.5 … 500, release 5 … 5000 ms), translated. Left: threshold default 0 dB, the bipolar gain-change meter, the vintage voicing's compression away from her 2.95 : 1 (1.2 – 1.7 dB deeper at 10.46 : 1).

**Answer: does RComp need its own LYNX engine/editor?** **No new engine.** **Crest Compressor**
(`src/core/DynamicsSuite.cpp` `crest::`, editor `CrestEditor` in `src/ui/ProEditors.cpp`) already has almost
everything an RComp user looks for:
- threshold −60…0 and ratio 1…100;
- **soft knee 0–72 dB**;
- attack 0.005–250 ms and release 10–2500 ms;
- **Auto release** (≈ ARC), and an **opto** and a **classic** style (release-shape families);
- **Character Warm** (≈ Warm);
- Mix, Input/Output ±36 dB, Auto gain, look-ahead 0–20 ms (it can reproduce the 64-sample delay), oversampling;
- **IN / OUT / GR meters**, a GR history display and a knee/curve side panel.

Glow is the wrong host: it is a fixed-time opto leveler with no threshold, ratio or attack.

Recommendation:
1. **Re-route `[waves-rcomp]` (PluginSubstitutes.cpp) from Compressor to Crest Compressor.** Write a
   `translateWavesRComp` for Crest along the lines of spec §7:
   - threshold RW1 − 4, ratio RW9, **knee 18 dB**, attack RW8, release RW0;
   - auto release = ARC (RW17 = 0);
   - Character Warm when RW16 = 0;
   - Mix (RW26), Output = gain RW2, look-ahead 1.33 ms.
   - Re-fit on the round-113 stairs and take (target ≤ 0.15 dB RMS). Effort M.
2. **Add the two things Crest lacks**, both in `crest::`:
   - an **"Electro / Opto" release-shaping option**: GR-dependent τ around 3 dB (spec §5.3), as a "release
     behaviour" parameter that is active with Auto release;
   - a **post-output 0 dBFS safety limiter + trim**.
   - Expansion ratios 0.5–0.99 can be left out at first: her state never uses them, and Sentry Gate covers
     downward expansion.
   - Effort M.
3. **Keep "Compressor"** as FL's generic effect. Give its generated panel at least a GR meter: add
   `controlOutput` to `class Compressor` and a layout with a `grmeter` (S). Point CLA-3A / C1 / SSLComp /
   API-2500 at Crest as well with style presets (classic for C1/SSLComp, opto for CLA-3A). CLA-76 → Crest
   "classic" style with µs attack (Crest goes to 5 µs) and a ratio set 4/8/12/20 (translator after spec §8
   measurements).
4. **Editor (design):** Crest's display-first layout is a different design from RComp's three vertical meters,
   but every RComp control has an obvious home: Thresh = threshold line + knob, Ratio knob, Gain knob + Out meter,
   Attack/Release knobs, Auto (= ARC), Style menu (Electro/Opto would sit here), Character (Warm), Mix. An RComp
   user should rate it 7/10 for findability. Optionally add a compact "classic" view toggle (input meter + threshold,
   GR meter + ratio, output meter + gain): UI only, L. It is not required.

**Top gaps** (if Compressor stays the substitute):
1. GR / In / Out meters (S).
2. Soft knee 6–18 dB (S).
3. Signed gain −30…+30 + trim + 0 dBFS limiter (M).
4. ARC + Electro/Opto release (M).
5. Warm/Smooth (M).
6. Ranges: ratio 0.5–50, attack 0.5–500, release 5–5000 (S).
7. Mix (S).
8. Header A/B / presets / undo (L, shared).

The re-route to Crest gets 1, 2, 5, 7 and most of 3/6 for free.

---

## Hush De-esser ← DeEsser (Waves)
Sources:
- `docs/specs/deesser.md`;
- Waves "DeEsser Plug-in Manual" (assets.wavescdn.com/pdf/plugins/deesser.pdf): chapters 2 (Quick Start),
  4 (Controls and Displays), 5 (Internal design) and 7 (Factory Presets);
- `HushEditor` (ProEditors.cpp), `hush::` + `paramText` (MixSuite.cpp), `lynx-before/Hush De-esser.png`.
- **Trial screenshot not available**: `deesser.png` was not produced in this run, so the original's layout is
  taken from the manual's text.

Trial on this PC: yes (no screenshot this round).

| Original feature | Original (range / default / where) | LYNX Hush now | Status | Note |
|---|---|---|---|---|
| Header: Undo/Redo, A/B, A→B, Load/Save, resize | WaveSystem bar | ProEditor header: title, slot, gesture hint only | ✅ | round 129's preset bar in the ProEditor header; round 128: resizable |
| Factory presets | Default (5506 Hz HP Split), Male Ess 4500, Male Shh 3385, Female Ess 6779, Female Shh 5077 (all BandPass + Split) | none | ✅ | 4–5 generic-named presets (frequency + detector) would cover it; round 135w: "Classic split (band detector)" folder: Classic default 5.5 kHz, Male S 4.5, Male SH 3.4, Female S 6.8, Female SH 5.1 kHz (LYNX names; classic voicing, band detector, split) |
| Thresh | slider −80…0 dB, default **0**, aligned with the Energy Detector meter | "Threshold" big knob −60…0 dB, default **−30 dB**; also a draggable line on the waveform and an arrow on the meter | ⚠️ | 3 ways to set it ➕; range is 20 dB shorter; default differs |
| Freq | value window 2000–16000 Hz (log), default **5506 Hz** | horizontal slider 2–16 kHz with end labels, same log law; default **6.02 kHz** | ✅ | same law; set the default to 5506 (v = 0.487) |
| SideChain HighPass / BandPass | button, default HighPass | "Detection: High-pass \| Band" tabs (+ bottom-bar menu), default High-pass | ✅ | Hush BP Q 1.4; Waves "very sharp", Q unknown |
| Audio Split / Wideband | button, default **Split** | "Processing: Wide band \| Split band" tabs, default Split | ✅ | Hush's split is 2nd order at the frequency; Waves' is 1st order at ≈ Freq/π (engine) |
| Monitor Output / SideChain (+ yellow light) | button | "Audition" toggle under the frequency slider (plays the detection band) | ✅ | same function, different name |
| Energy Detector meter (side-chain energy, dB below 0 dBFS, blue) aligned with the threshold | main meter | "IN" meter = input peak with a threshold arrow, and **inside it a blue bar = the detector's energy** (what the threshold is compared with, in the threshold's scale; both engines) | ✅ | round 147 |
| Attn meter (red, instantaneous GR) + **infinite peak hold, click to reset** | meter | "GR" bar with a held-peak line and number, click to reset | ✅ | round 144 |
| Output meter −30…0 dBFS + peak readout (click to reset) | meter | none (bottom bar shows the Output **setting**) | ✅ | UI; round 135w: OUT meter −30…0 beside IN / GR, clip lamp and highest-peak read-out, click to reset |
| Frequency-response display | **none in the classic DeEsser** (the manual lists no graph; Renaissance DeEsser has one) | small detector-band graph only in the generic `hushDeEsser()` layout; the display-first editor shows the scrolling waveform with the reduction in red | ➕ | nothing to add for parity |
| Range | none (the floor is the split's leakage) | "Range" 0–24 dB, default 12 | ➕ | translator fixes it at 12 dB |
| Release | none (hidden, unknown) | 10–250 ms, default 61 ms | ➕ | |
| Lookahead | none, 0 latency | 2 ms toggle, **on** by default (adds 2 ms of latency) | ➕/⚠️ | the default should be Off for Waves parity (spec §7.3) |
| Output gain | none | ±12 dB | ➕ | |
| Knee / ratio | hard knee, effective ratio ≈ 7, RMS energy | 3 dB soft knee, ratio ∞, peak | ⚠️ | engine (spec §7.2) |
| Gestures | slider drag, value-window typing | drag, Ctrl fine, wheel, reset, right-click entry, line/arrow drags | ✅ | |
| Saved-state translation | — | frequency (×0.81), threshold (+5 dB), Split/Wide, HP/BP, Monitor translated; range fixed at 12 dB | ✅ | round 113 §3c |

**Scores:** configuration 7/10 (every Waves control exists, plus extras), design 5/10. **After round 144**: configuration 9/10, design 7/10 (round 135w: 9 / 6; round 144: the GR meter's held peak). **After round 147**: design 7 → **8**/10 (the energy-detector bar beside the threshold arrow). The scrolling-waveform
editor is Pro-DS-like. A DeEsser user finds Threshold, Freq, HP/BP, Split/Wide and Monitor (as "Audition")
quickly. What they miss: the side-chain energy meter beside the threshold, the Attn peak hold, the output meter,
presets, and the Waves defaults.

**Top gaps:**
1. **Defaults:**
   - Frequency 5506 Hz (v 0.487);
   - Lookahead Off;
   - a threshold default that does not de-ess until it is lowered: Waves starts at 0 dB, Hush at −30 dB.
   Where: `hushInfo()`. Effort S.
2. **Meter semantics:** make the IN meter show the **detector (side-chain) energy** the threshold is compared with
   (or add it as a second bar). Add **GR peak hold with click-reset** and an **OUT meter**.
   Where: `HushEditor` (ProEditors.cpp) plus a live output for the detector level in `hush::DeEsser`
   (`meters.set`). Engine export + UI. Effort S–M.
3. **Factory presets**: Default / Male S / Male SH / Female S / Female SH frequencies, with BP + Split.
   Generic names, no Waves trade names needed. This needs the shared preset mechanism (see Glow #6) or a Hush-only
   menu. Effort S once presets exist.
4. **Threshold range −80…0.** Law + translator. Effort S.
5. **Engine** (spec §7): 1st-order split at Freq/π, RMS detector, ratio ≈ 7 hard knee, no range clamp for
   translated states, 0 latency. DSP. Effort M.
6. **Header A/B, undo, presets** in the ProEditor header (shared with Crest, Apex and the others). Effort L.

---

## Saved-state translation summary (docs/PLUGIN_FALLBACK.md, round 113 §3c)

| Original | LYNX | Translated | Not translated |
|---|---|---|---|
| RComp | Compressor | threshold (−5 dB), ratio, attack, release (ARC → 300 ms), gain | Warm/Smooth, Electro/Opto, Mix, Trim (auto gain forced off) |
| CLA-2A | Glow Leveler | Gain (measured table + 1.5 dB), Peak Reduction (0.40 + 0.021·(k − 40)), Comp/Limit | Hi Freq (emphasis forced 0), Analog, Mix, Trim, Auto Makeup, VU |
| SSL E-Channel | Ridge Strip | HP, 4 bands (fitted laws), EQ/dyn bypass, comp threshold (−15.7)/ratio/release/fast, output (+0.9 make-up) | Split, Dyn S.C., Ch Out (forced Dyn>EQ), expander, Analog, Input, Phase, LP (forced off) |
| DeEsser | Hush | frequency (×0.81), threshold (+5), Split/Wide, HP/BP, Monitor | range fixed at 12 dB; look-ahead/release stay at defaults |
| CLA-76, CLA-3A, C1, SSLComp, API-2500 | Compressor | nothing (fixed preset: −15 dBFS, 2:1, auto gain) | everything |
| SSL Native Vocalstrip 2 / Channel Strip | Ridge Strip | nothing (neutral) | everything |

## Quell Denoiser ← NS1 Noise Suppressor (Waves)
Sources: docs/specs/ns1.md; REA_rodada119 §3; trial screenshot w119/ref/NS1_Stereo.png (default preset); LYNX
src/core/VoiceTools.cpp (`quellInfo`, `paramText`), PluginLayout.cpp `quellDenoiser()`, lynx-before/Quell Denoiser.png;
third-party control reference josephnilo.com/blog/waves-ns1-explained. Trial on this PC: yes.

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| NS1 fader (the only control) | 0–100 %, tall vertical fader on the right | "Amount" tall vertical slider on the right, 0–100 % | ✅ | same place, same unit |
| Fader default | 0.0 (default preset reads 0.0) | 30 % | ⚠️ | LYNX opens already suppressing; her states are 23–70 % (translated, so only new inserts are affected) |
| Fader scale | 0…100 printed both sides every 10 | round 137: 0 … 100 printed beside the fader every 10 | ✅ | round 137 (one side) |
| Fader read-out | numeric box under the fader ("0.0"), editable | "30.0%" value under the slider, right-click entry | ✅ | |
| Attenuation meter | left, vertical, 0 … −48 dB, ticks every 6 dB both sides | left, vertical, 0 … −30 dB, ticks every 6 dB | ⚠️ | NS1 can show 48 dB; Quell clips its scale at 30 (Quell max reduction ~25 dB so fine for the engine, but the scale differs) |
| Meter read-out | numeric box under the meter | "0.0 dB" under the meter | ✅ | |
| Meter meaning | "overall energy removed from the input" | broadband reduction | ✅ | |
| Meter peak hold / reset | numeric read-out box (Waves convention: click to reset) | round 137: the read-out under the meter holds the deepest reduction; click the meter = reset | ✅ | round 137 |
| Listen (hear the removed noise) | — | toggle bottom-left | ➕ | |
| Output trim | — | −12…+12 dB knob bottom-right | ➕ | |
| Mono / Stereo components | NS1 Mono, NS1 Stereo | one stereo engine, linked | ✅ | LYNX is per-slot stereo; fine |
| Adaptive noise-floor behaviour | "analyzes and adapts in real time"; steady noise drifts by a few dB over seconds | fixed 6-band expander (fitted), no adaptation | ⚠️ | engine, not UI (spec §4.3–4.4; 63 Hz octave +5…7 dB after SSL HPF) |
| Latency | not published; not measured (the first plugin our host loads from the shell is mis-instantiated, so its reported latency is not trusted) | 0 | ? | measure on the trial if it matters |
| Undo / Redo | toolbar | song-level Ctrl+Z only | ⚠️ | |
| Presets (load/save, factory) | toolbar | "(no preset)", no save | ✅ | round 129's preset bar: 4 LYNX factory presets + user presets |
| A/B, Setup A/B, copy A→B | toolbar | — | ✅ | round 129's preset bar |
| ◀ ▶ preset step | toolbar | ◀ ▶ | ✅ | empty list |
| Help / options menu | toolbar ≡ | options menu (reset, bypass) | ✅ | |
| MIDI learn | right-click | right-click MIDI link | ✅ | |
| Default / fine / wheel gestures | Alt-click, Ctrl-drag, wheel | double/Alt-click, Ctrl-drag, wheel | ✅ | |
| Bypass | host / Waves toolbar | LED in header + options | ✅ | |
| Window size | fixed 345×525-ish | resizable, fixed aspect, 300×420 | ✅ | |
| Logo / branding area | top-left logo, product name bottom-left | section caption "Noise suppressor" | ✅ | (trade dress not copied — correct) |

**Scores** — configuration: 8/10, design: 8/10.
**Top gaps**
1. Meter scale 0 … −48 dB (ticks 6 dB) instead of 0 … −30 — `quellDenoiser()` meter item / the grmeter range in
   src/ui/PluginEditors.cpp (`grmeter` scale per item; add a scale field) — UI only, S.
2. Amount default 0 % (or keep 30 % but say so) — `quellInfo()` default; translation unaffected — S (decide; a
   one-knob denoiser that does nothing on insert matches NS1, but LYNX users may prefer an audible default).
3. Fader scale 0…100 with ticks every 10 beside the slider — PluginEditors.cpp vslider drawing (a `scale` style
   flag) — UI, S.
4. Peak-hold on the meter read-out (click to reset) — PluginEditors.cpp grmeter — UI, S.
5. Adaptive noise-floor tracker (spec §7) — VoiceTools.cpp quell — engine, L.
6. (Global) A/B + user preset save — generic editor — M (shared with every engine).

## Gemini Doubler ← Doubler2 / Doubler4 (Waves)
Sources: docs/specs/doubler.md; REA_rodada119 §4; Waves Doubler user guide (assets.wavescdn.com/pdf/plugins/doubler.pdf,
text extracted); trial screenshot w119/ref/Doubler2_Stereo.png; LYNX VoiceTools.cpp (`geminiInfo`, laws),
PluginLayout.cpp `geminiDoubler()`, PluginEditors.cpp `geminipan` / `geminimap` / `geminieq`; lynx-before/Gemini Doubler.png.
Trial on this PC: yes (Doubler2 / Doubler4).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Pan / gain graph (half circle, voices + D marker) | top-left; angle = pan, radius = gain; **"Gain, Pan, Delay and Detune can be controlled using the graphs"** (drag) | `geminipan`, same place, D + 1 + 2 (+ 3 + 4) markers; round 137: drag a marker = its pan (angle) and gain (radius) | ✅ | round 137 |
| Delay / detune graph | top-centre; x 0…100 ms (labels 0 20 40 60 80 100), y +100…−100 c with "100 / 0 / 100" labels; draggable | `geminimap`, x labels 0…100; round 137: +100 / 0 / −100 cent labels, drag a voice = its delay and detune | ✅ | round 137 |
| EQ graph | top-right; L and H markers on the curve, x labels 62 / 250 / 1K / 4K, 0 dB tick; drag markers, **double-click a marker = filter on/off** | `geminieq`; round 137: L and H markers on the curve (drag = frequency / gain, double-click = on / off), labels 62 / 250 / 1K / 4K and the 0 dB tick | ✅ | round 137 |
| Direct on/off | row 2, left; On | toggle "Direct", On | ✅ | |
| Direct gain | 0…−48 dB (manual default −12; trial default 0) | 0…−48 dB, default 0 dB | ✅ | |
| Direct pan / stereo rotation | ±45°, centre | ±45°, rotation law measured | ✅ | |
| Align (direct) | NO/YES button; default Off | toggle, default **On** | ⚠️ | default differs |
| FB high-pass | 16…12000 Hz, default 16; numeric box with a HP icon | "FB low cut" knob 16 Hz…21.4 kHz, default 16 | ⚠️ | range top 12 kHz in the original |
| Range | 80 Hz / 20 Hz, manual default 20 Hz (trial screenshot shows 80HZ) | tabs 80 Hz / 20 Hz, default 80 | ✅ | |
| Modulation RESET | button, automatable, restarts every LFO in sync | — | ✅ | needs a trigger param + engine reset; round 135w: RESET button (momentary): every voice's LFO restarts in sync |
| Low shelf on / gain / freq | L marker button + gain knob + freq box; gain +12…−24 dB, below −24 "the floor drops to make a cut filter"; default On | L toggle, gain −30…+12, freq 16 Hz–21 kHz, default **off** | ⚠️ | no cut mode below −24; default off vs On |
| High shelf on / gain / freq | H marker + gain + freq box | H toggle, gain, freq, default off | ⚠️ | same remarks |
| EQ acts on voices only | yes | yes | ✅ | |
| Voice on/off | coloured numbered buttons 1 / 2 | toggles "1" / "2" | ✅ | |
| Voice gain | 0…−48 dB, default −12 | 0…−48 dB, default −6 | ✅ | default differs slightly |
| Voice pan | ±45°, defaults V1 22.5 / V2 −22.5 (V3 45, V4 −45) | ±45°, defaults −45 / +45 | ⚠️ | defaults |
| Voice delay | 0…100 ms, default 8 ms; min 8 / 14 ms when not aligned | 0…100 ms, defaults 9.4 / 23.7 | ✅ | the 8/14 ms minimum not enforced (measured: not enforced by the trial either, spec §4.2) |
| Voice feedback (FDBK) | 0…100 %, default 0, warning > 25 % | 0…100 %, 0 | ✅ | |
| Octaver | ▼ arrow button, octave down | "Oct" toggle | ✅ | |
| Detune | ±100 c | ±100 c | ✅ | |
| Mod depth | ±200 c, positive starts upward | ±200 c | ✅ | |
| Mod rate | 0.1…200 (trial up to 400), default 1.0 | 0.1…400, default 1.0 | ✅ | |
| Value editing in the voice strip | numeric boxes (drag / type); gain & pan as small knobs | knobs everywhere with values under them | ⚠️ | design only; drag/type works on both |
| Per-voice column order | Voice · Gain · Pan · Delay · Fdbk · Oct · Detune · Depth · Rate | 1 · Gain · Pan · Delay · Feedback · Oct · Detune · Depth · Rate · Source | ✅ | same order |
| Source per voice (In 1 / In 2) | bottom-left drop-downs "In 1 L▾", "In 2 R▾" (L / L+R / L−R) | tabs at the end of each voice row | ⚠️ | different place, same options |
| Master output gain | right column, vertical fader 0 … −∞ (scale 0 / 24) | vertical fader −24…0 dB, no scale | ⚠️ | no −∞, no scale marks |
| Output meters | stereo bar meters beside the fader, scale 0…−24, peak counters under them (click to reset) | — | ✅ | round 129: output meters with peak read-outs beside the fader |
| Clip light | red, click to reset | — | ✅ | round 135w: the meter's OUT label turns red when the output clipped; click the meter to reset |
| Doubler4: voices 3 and 4 | 4 rows, each with its source | 2 voices only | ✅ | engine + params + translation (spec §7 item 6); round 135w: voices 3 / 4 (page "Voices 3-4" of the voice rows; off by default), drawn on the graphs; a Doubler4 state's blocks 46 / 56 are translated [model: same layout as voices 1 / 2; sources 3 / 4 not probed] |
| Components Mono / Mono→Stereo / Stereo | 3 | 1 stereo | ⚠️ | M→S = stereo on a mono-fed track; fine |
| Undo / Redo, presets, A/B, Setup A/B, Save | toolbar | see header note | ✅ | round 129's preset bar |
| MIDI learn, gestures | right-click; Alt-click etc. | yes | ✅ | |
| Window | fixed 800×580 | resizable 800×460 | ✅ | |

**Scores** — configuration: 7/10 for Doubler2 users, 4/10 for Doubler4 users; design: 6/10.
**Top gaps**
1. Draggable markers in the three graphs (pan/gain, delay/detune, EQ L/H; double-click a shelf marker toggles it) —
   PluginEditors.cpp `geminipan` / `geminimap` / `geminieq` + a display-drag hook in mouseDown/mouseDrag (today only
   `vumeter` handles clicks) — UI only, M.
2. Doubler4 voices 3–4 — VoiceTools.cpp gemini (append V3/V4 params so old projects load unchanged), `geminiDoubler()`
   rows 3–4 (layout grows ~150 px or a 2/4 voice switch), translator `waves-doubler` blocks 46/56 — engine, M.
3. Output meters + clip light beside the output fader — `geminiDoubler()` add `meter(...)`/vumeter items in the Output
   section — UI, S.
4. Modulation Reset button — new trigger param ("Mod reset") + LFO phase reset in gemini::Gemini — engine, S.
5. Graph labels: cents scale on the delay/detune graph, frequency labels + 0 dB tick + L/H markers on the EQ graph —
   PluginEditors.cpp — UI, S.
6. Shelf "cut" below −24 dB and defaults (shelves On, Align Off, pans ±22.5, gain −12) — VoiceTools.cpp — engine +
   defaults, S (defaults must not change translated states: translation sets every value).
7. Output fader to −∞ with a scale — VoiceTools.cpp `outputDb` law + vslider scale — S.

## Echoform ← H-Delay Hybrid Delay (Waves)
Sources: docs/specs/h-delay.md; REA_rodada119 §2; josephnilo.com/blog/waves-h-delay-explained (citing the H-Delay
guide); trial screenshot w119/ref/H-Delay_Stereo.png; LYNX EchoSuite.cpp (`echoformInfo`, laws, `paramText`),
PluginLayout.cpp `echoformLayout()` (ValhallaDelay-style section row), lynx-before/Echoform.png. Trial on this PC: yes.

How an H-Delay user finds things on Echoform today: Delay → "DELAY" big knob (✓, same idea); BPM/HOST/MS → a small
menu "ms|Note|Dotted|Triplet" under the knob (no own-BPM mode); note buttons → note knob + the dotted/triplet menu;
Feedback → small knob in its own section (H-Delay: the second big knob); Mix → top-left "MIX" (H-Delay: top-right
"DRY/WET"); LoFi / Analog → hidden inside the 12-entry MODE menu at the bottom; Ping-pong / Dual → the STYLE knob
(5 styles) + "Pairs"; Phase L/R → nowhere; HiPass / LoPass → "EQ: High / Low" (stacked, reversed order); Mod
depth/rate → "MOD" section, which shows Wow/Flutter in the default Tape mode; output → "OUTPUT: Level"; tap → nowhere;
delay display → an echo-train picture (not the note/BPM read-out).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Delay knob | big, left; 1–3500 ms or note 1/64T … 2 Bar; default 1/8 dotted (Host) | big "Delay" knob in DELAY section; 0–20000 ms (piecewise law) or note 1/64…8/1; default 300 ms | ⚠️ | same role; default ms not a note |
| Note values on one knob | one knob steps triplet / straight / dotted for each length (1/64T … 2 Bar) | note knob (10 lengths) + separate kind menu (Note / Dotted / Triplet) | ⚠️ | two controls for one; reachable |
| Sync HOST | button under the display; default | menu entry "Note" (follows project tempo) | ⚠️ | named differently |
| Sync BPM (own tempo) + BPM value | button + BPM number in the display | — | ✅ | translator folds BPM into ms; round 135w: "Project / Own BPM" + BPM (30–300) on the second row; H-Delay's own-BPM states keep their note and tempo (was folded into ms) |
| Sync MS | button | menu entry "ms" | ✅ | |
| Central display | note value (e.g. "1/8 D") and tempo "120 BPM" (or ms); click-drag / type to edit | value text under the knob + echo-train picture (echoes on a time axis); round 137: the picture's top right reads the note and its tempo ("1/8 D  120.0 BPM") or the ms | ✅ | echotrain ➕; edited with the knob / sync menu, not by dragging the read-out |
| Tap pad | big pad far left; last two taps; Host: snaps to a note, BPM: sets BPM, MS: sets ms | — | ✅ | round 129: TAP pad (sets the left time) |
| Ping Pong | button above the display; Off; start-side value | STYLE = "Ping-pong" (5th of 5) + "Pairs" toggle (alternate / H-Delay pairs) | ⚠️ | no separate button; start side ✅ (next row, round 135w; stale ❌ re-checked round 152) |
| Ping-pong start side (L→R / R→L) | value box | — | ✅ | translator ignores RealWorld 45; round 135w: "Start left / Start right" (ping-pong); translated from RealWorld 45 (0 left, 1 right [EXEC] round 119 probe) |
| Dual (independent L/R times) | button; drag L or R in display | STYLE = "Dual" + Delay R / Sync R | ✅ | |
| Phase L / Phase R (ØL ØR) | two buttons beside Ping Pong / Dual; Off | — | ✅ | new params; round 129 |
| Feedback | big knob right of display, 0–200 %, default 60 | small knob, 0–200 %, default 70 | ⚠️ | size / place / default |
| Dry/Wet | top-right knob 0–100, default 100, equal power | "Mix" top-left, 0–100 %, default 50 %, equal power | ⚠️ | place + default |
| Output | knob −18…+18 dB, default 0 | "Level" −18…+18 dB, default 0 (round 119) | ✅ | |
| Output meter | vertical, 0 … −48 dB, beside Dry/Wet | — | ✅ | round 135w: OUT meter (0 … −48 dB) at the right edge |
| Peak / clip LED | red lamp above the meter | — | ✅ | round 135w: clip lamp above the meter + highest-peak read-out; click to reset |
| LoFi | switch under Tap; Off; steep LP (≈ 5.8 kHz, 8th order+) inside the loop | MODE "Lo-Fi" (a menu entry that also sets the whole character); round 137: LOFI switch (second row), independent of the mode | ✅ | round 137: the in-loop 8th-order low-pass at 5.8 kHz of docs/specs/h-delay.md §4.2 (−22 dB at 8 kHz); the steeper stop band above 8 kHz is not modelled; translated (RealWorld 21) |
| Analog | knob Off / 1 / 2 / 3 / 4, default 2 | MODE menu (Analog, Bucket brigade, Tape …) + Drive / Age | ⚠️ | no 0–4 positions, no hiss / loop compression of Mod2 |
| Mod Depth | 0–100, default 0; bottom-left MODULATION group | "Depth" in MOD section, only in non-tape modes; default 50 % | ⚠️ | default Tape mode shows Wow/Flutter instead |
| Mod Rate | 0.1 … 6000 Hz (BPM sync in concept), default 0.1 | 0.01 … 10 Hz, default ≈ 0.5 Hz | ⚠️ | range / default |
| HiPass | Off, 20 Hz–20 kHz, default Off; bottom FILTERS group (left) | "Low" (EQ section, lower knob) 10 Hz–2 kHz linear | ⚠️ | no Off label, range to 2 kHz, linear taper |
| LoPass | 20 Hz–20 kHz, Off, default Off (right) | "High" (upper knob) 200 Hz–20 kHz | ⚠️ | range, order reversed |
| Filter LINK | button between HP and LP; moves both | — | ✅ | round 135w: "Link EQ": moving the low or the high cut moves the other by the same ratio |
| Components Mono / Stereo / M>S | 3 | one stereo engine + Width | ⚠️ | Width ➕ |
| Delay changes glide (tape-like) | yes | depends on mode | ⚠️ | |
| Loop limiter (fb > 100 % stays bounded; −3 dB loud passes) | engine | Drive/Age shaper in some modes; no loop limiter in Clean | ⚠️ | engine (spec §7 item 1) |
| Style Single / Ratio / Quad | — | STYLE | ➕ | |
| Diffusion amount / size | — | DIFFUSION section | ➕ | |
| Ducking | — | OUTPUT section | ➕ | |
| Width | — | FEEDBACK section | ➕ | |
| 12 modes (Tape, Shifter, Pitch, Reverse, …) | — | MODE menu | ➕ | |
| MIDI learn (all but meter) | right-click | right-click MIDI link | ✅ | |
| Gestures (Alt-click reset, Ctrl fine, wheel, type) | yes | yes | ✅ | |
| Undo / Redo, presets, A/B, Setup A/B, Save | toolbar | see header note | ✅ | round 129's preset bar |
| Window | fixed ~750×290 | resizable 780×300 | ✅ | |

**Scores** — configuration: 5/10, design: 4/10 (the controls exist for ~60 % of H-Delay, but in ValhallaDelay's
places and names; Phase, Tap, own BPM, Link, meter are missing).
**Top gaps**
1. Tap pad — a `tap` button item in `echoformLayout()` (DELAY section) + editor handler (PluginEditors.cpp) that
   averages the last two taps and writes Time L (ms mode) or the nearest Note L/kind (note mode) — UI only, S/M.
2. Phase L / Phase R toggles — append params "Phase L", "Phase R" (polarity of each delay line) in EchoSuite.cpp,
   two toggles in the FEEDBACK or DELAY section; translator RealWorld 8 / 9 — engine (trivial DSP) + UI, S.
3. Own BPM sync — append "Sync source" (host / own BPM) + "BPM" params; ms/note text shows tempo; translator keeps
   the BPM instead of folding it to ms — engine + UI, M.
4. Ping-pong as a button + start side — keep STYLE but add a visible "Ping-pong" toggle that sets Style, append
   "Start side" param; translator RealWorld 45 — engine (swap L/R at the input) + UI, S.
5. Output meter + clip lamp in the OUTPUT section — `echoformLayout()` meter item (output level) — UI, S.
6. LoFi as an independent switch (in-loop ≈ 5.8 kHz 8th-order LP, spec §4.2) and an "Analog 0–4" selector bound
   to the loop limiter / colour / hiss of spec §7 — EchoSuite.cpp — engine, M/L.
7. Filters: HP and LP over 20 Hz–20 kHz (log taper) with "Off" at the ends, HP left / LP right, + LINK — append new
   params (keep the old ones for old projects) — engine + UI, M.
8. A note/tempo read-out block in place of (or above) the echo-train picture: "1/8 D · 120 BPM" / "375 ms" — UI, S.
9. Defaults when inserted fresh (Mix 100 on an FX bus is H-Delay's; LYNX's 50 % suits inserts) — decide; S.

**Waves delays mapped to Fruity Delay 3 (one table)**

| Original | Maps to now | Better LYNX engine | Recommendation |
|---|---|---|---|
| Waves SuperTap (2-Taps / 6-Taps: per tap delay, gain, pan, feedback, filter, pitch mod; tap pad, BPM/ms) | Fruity Delay 3 at FL's default (no translation) | **Fruity Delay Bank** (FL's multi-tap bank, exact engine) for the per-tap layout; **Lattice Mod** (multi-tap with spread/slope/tap gain) as a creative alternative | Move SuperTap → Fruity Delay Bank (taps = SuperTap taps; times/gains/pans translatable once the RealWorld list is read on the trial); keep FD3 only as fallback |
| (H-Delay already → Echoform, round 119) | Echoform | — | keep |
| Other Waves delays seen in the substitute table | none (only SuperTap and H-Delay are listed under `waves =`) | Echoform for Manny Marroquin Delay / Abbey Road tape-style delays if they appear | add rows only when found in her projects |

## LYNX Tune ← Waves Tune Real-Time (Waves)
Sources: docs/specs/waves-tune-real-time.md (v16 user guide [S1], tapers read from the trial in round 120);
REA_rodada120.md; josephnilo.com/blog/waves-tune-real-time-explained (control reference citing the guide); LYNX
LynxTune.cpp (`info`, `paramText`, laws), PluginLayout.cpp `lynxTune()` (Auto-Tune-Pro "auto" workflow, round 120;
re-skinned round 122: `pitchring`, `pitchtrace`), lynx-before/LYNX Tune.png. Trial on this PC: yes — but the
round-129 screenshot (ref/wtune.png) had **not been produced** when this was written (the capture run stopped
after rcomp.png), so the "where" column for Waves Tune RT comes from the guide, not from a picture. Re-check the
positions against wtune.png once it exists.

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Speed | 0.1–800 ms, default 15; piecewise taper (½ travel = 50 ms); in-note drift / vibrato tightness | — (one "RETUNE SPEED" knob only) | ✅ | measured effect small (only < 5 ms), but a WTRT user looks for two time knobs; round 135w: SPEED (0.1–800 ms, the trial's taper, default 15) on the "Real-time" page; it acts with Link off (flattens part of the in-note deviation below ~5 ms, as measured in round 120); translated (RW 20) |
| Note Transition | 0.1–800 ms (0.1 ms steps), default 120 | "RETUNE SPEED" 0–400 ms (quadratic), default 20 ms; translator maps NT → retune (τ = NT, matched ±10 %) | ⚠️ | name, range (800), default; behaviour matched |
| Link (Speed ↔ NT) | Off | — | ✅ | follows from Speed; round 135w: "Link" (on = the retune speed handles the deviations — the old behaviour; translated states keep it on unless Speed < 5 ms) |
| Tolerance Cents | Off–40 c (added to the 50 c switch threshold) | — | ✅ | LYNX has fixed hysteresis; round 135w: TOL. CENTS Off–40 c (added to the half-way point) [model] |
| Tolerance Time | Off–300 ms | — | ✅ | round 135w: TOL. TIME Off–300 ms (a new note must last that long) [model] |
| Vibrato On/Off | On | — | ✅ | (Flex-Tune / Humanize are Auto-Tune concepts, not equivalents); round 135w: "Vibrato" switch (off = flattened) |
| Vibrato Depth | 0–200 %, default 100 | — | ✅ | round 135w: VIB. DEPTH 0–200 % (multiplies Natural vibrato) |
| Correction On/Off | On | slot bypass / header LED | ⚠️ | no in-panel switch |
| Correction % | 1–100 %, default 100 | MIX 0–100 % (dry/wet) | ✅ | not the same: Mix blends dry + shifted (phasey); Correction scales the pitch shift; round 135w: CORRECTION 0–100 % scales the shift (not a dry mix); RW 22 translated [model] |
| Formant | Corrected / Non-Corrected, default Corrected | "FORMANT" toggle, On | ✅ | |
| Formant shift | — | ±7 st | ➕ | |
| Reference pitch | ±100 c (415.3–466.16 Hz), 440 | "A4" knob 430–450 Hz | ⚠️ | range ±40 c only |
| Root | 17 names (sharps and flats) | KEY menu, 12 names (sharps) | ✅ | |
| Scale type | 44 (User, Chromatic, Major, Natural/Harmonic/Melodic minors, Justonic, modes, Blues, Whole tone, Diminished, Bebop, pentatonics, Pythagorean, Maqamat, Indian thaats …) | SCALE menu, 13 + Custom | ⚠️ | her states use Major / Natural Minor only (covered) |
| Range | keyboard span with draggable boundary handles + presets Bass / Baritone / Tenor / Alto / Mezzo / Soprano / Generic; notes outside → **bypassed** | "Input type" menu (Soprano / Alto-Tenor / Low male / Instrument) = detector range | ✅ | round 147b: the range bar over the keys (drag a boundary), seven presets in the keyboard strip (LYNX's spans [model]: classical voice ranges ± a third; they also pick the detector's input type), notes sung outside pass uncorrected |
| Keyboard | multi-octave piano at the bottom, per-note status area | one octave of on/off key toggles at the bottom ("TARGET NOTES") | ✅ | round 128: four octaves; round 147b: 2–7 octaves shown, scrollable (see navigation) |
| Note status per key | Legal / Illegal / Illegal-Pitch-Up / Illegal-Pitch-Down / Bypass (click cycles) | on / off | ✅ | round 128: Bypass mode; round 147b: REMOVE mode cycles legal → illegal → illegal up → illegal down (arrows on the key; an illegal note's direction picks the next legal note up / down) |
| Group Octaves | switch (per-note edits apply to all octaves) | always grouped | ✅ | round 152b: "Group" (octaves) in the keyboard strip (on by default = as before); off = a click edits that key alone (REMOVE cycles its status, BYPASS toggles it; an outline marks a key with its own status), the correction picks targets per key (MIDI 24 … 119, saved in a packed key map); Reset clears it; Send / Receive carry it [MANUAL S1 p.17] |
| Reset Scale / Send Scale to All / Receive (key) | buttons | — (key comes from translation / project key) | ✅ | round 147b: RESET SCALE in the keyboard strip; round 150b: SEND (this key, scale, note statuses / bypasses / directions and range to every other LYNX Tune in the project whose RECEIVE is on; one undo step) and RECEIVE (a saved switch, on by default; switching it on takes the scale sent last in this LYNX session) |
| Keyboard view / navigation buttons | yes | — | ✅ | round 147b: − / + (more / fewer octaves, 2–7) and ◀ / ▶ (scroll by an octave) in the keyboard strip |
| Central display | target note name + correction amount in cents | `pitchring`: note name, ±100 c ring, sung offset + octave | ✅ | |
| Scrolling pitch graph | not in WTRT (guide lists none) | `pitchtrace` (input vs corrected, 8 s) | ➕ | Auto-Tune-style; useful |
| Input / output meters | −28 … 0 dBFS | — | ✅ | round 135w: IN / OUT meters on the "Real-time" page |
| MIDI Target Pitch | MIDI note forces the target (Speed/NT timing) | "MIDI channel" (held pitch classes become the targets) | ⚠️ | pitch class only, no octave |
| MIDI Reference Tone + level | tone generator ±18 dB | — | ✅ | round 152b: "Ref. tone" + level (±18 dB) on the Real-time page — the notes held on the MIDI channel play as sines at the reference pitch (0 dB = −12 dBFS peak per note [model]) [MANUAL S1 p.21] |
| Latency | reports 0; real 0–4 ms dynamic | ~25 ms reported + PDC; "Low latency" toggle | ⚠️ | ➕ toggle; different by design |
| Sound (measured, round 165j, synthetic takes) | latency 5.4 ms, accuracy 2.3 / 12.2 c, retune 29 ms, bursts 18.5 / min, formant 0.26 dB | 25.4 ms (Low latency 6.4), 1.7 / 4.4 c, 38 ms, 17.6 / min, 0.36 dB; highs within 0.1 dB of the dry take | ⚠️ | tighter pitch; slower to land by ~9 ms at the same transition setting; slightly larger envelope change |
| Transpose | — | ±12 st stepper | ➕ | |
| Tracking | — | knob | ➕ | |
| Humanize / Flex-Tune | — | knobs | ➕ | Auto-Tune Pro role |
| Mono / Stereo components (detect on (L+R)/2) | 2 | one stereo slot | ✅ | |
| Undo / Redo, presets (factory/user/favourites, search), A/B, Setup A/B, Save | toolbar | see header note | ✅ | round 129's preset bar (no search / favourites) |
| MIDI learn, gestures | right-click; Alt-click etc. | yes | ✅ | |
| Window | fixed | resizable 800×548 | ✅ | |

**Round 147b:** configuration 8 → 9, design 6 → 7 (the keyboard: note statuses with directions, the range bar and presets, Reset scale, the view's navigation). Remaining: Group Octaves off, Send / Receive, MIDI reference tone.

**Round 150b (after 150):** configuration 9 → 9, design 7 → 8 (Send / Receive in the keyboard strip). Remaining: Group Octaves off, MIDI reference tone.

**Round 152b (after 152):** configuration 9 → 10 (Group Octaves off with per-key statuses, the MIDI reference tone), design 8 → 8. Remaining: the 44 scale types (13 + Custom here), Reference pitch ±100 c, MIDI Target Pitch with octaves.

**Scores** — configuration: 4/10 (for a Waves Tune RT user: key/scale/formant/time are there, but Speed, Tolerance,
Vibrato, Correction %, note statuses and range bypass are not), design: 5/10 (note display centre + keyboard at the
bottom match; the knob row is Auto-Tune's).
**Top gaps**
1. Vibrato On + Depth 0–200 % — LynxTune.cpp (spec §7.6 model: split slow note component / residual FM, scale the
   residual) + two controls in the correction row — engine, M. Needed before Tolerance because a WTRT user's
   quick-start is "Vibrato Off for the hard effect".
2. Correction % (1–100, scales the shift, not a dry mix) — LynxTune.cpp multiply `wanted`; knob next to MIX — engine,
   S (spec §7.7).
3. Tolerance Cents / Time — LynxTune.cpp target hysteresis (50 c + cents, hold time) + two knobs — engine, S/M.
4. Note statuses Up / Down / Bypass on the keyboard (click cycles states) and a multi-octave keyboard with range
   handles that bypass notes outside — params (per-key 0…4 instead of on/off; append, keep old on/off meaning) +
   `lynxTune()` keyboard + PluginEditors.cpp — engine + UI, M/L.
5. Retune range to 800 ms and a second "Speed" knob (in-note, acts only on fast deviations; spec §8.1 to measure
   first) — LynxTune.cpp `retuneMs` (append a new param to keep old states) — engine, M.
6. Reference ±100 c (415–466 Hz) — `referenceHz` law (append a param or remap with translation) — S.
7. Input / output meters (−28…0 dBFS) — `lynxTune()` meter items — UI, S.
8. More scale types (Natural minor named as such, Justonic, Bebop, Whole tone, Diminished, Maqamat …) —
   VocalPitch scale table — engine, S/M; low priority (her states: Major / Natural Minor).

## Other Waves stand-ins (short tables)

**Waves L1 / L2 / L3 / L4 → "Limiter"** (Effects.cpp: Ceiling / Gain / Release, fixed 1.5 ms look-ahead, no panel
of its own — FL "Fruity Limiter" is a different engine)

| Original control | LYNX "Limiter" | Apex Limiter (FabFilter Pro-L role) |
|---|---|---|
| Threshold (L1/L2: 0…−30 dB, drives gain) | Gain 0…+24 dB (inverse of threshold) | Gain |
| Out Ceiling (0…−30 dB) | Ceiling −12…0 dB (default −0.3) | Ceiling (true-peak option) |
| Release (0.01…1000 ms) | Release 10…1000 ms | Release |
| ARC auto release (L2/L3) | — | Style (Clean / Punch / Loud / Safe) ≈ adaptive release |
| Look-ahead (fixed 1.5 ms in L2) | fixed 1.5 ms | Lookahead knob |
| Quantize / Dither type / Noise shaping (L1/L2) | — | — (render-time dither belongs to export) |
| Domain analog/digital (L2), Separation / Profile (L3 multiband) | — | Link (stereo) only |
| Attenuation meter + I/O meters | none | GR trace + meters (display-first editor) |

Recommendation: **map L1 / L2 / L3 / L3-16 / L4 to Apex Limiter**, Threshold → Gain (−threshold), Ceiling →
Ceiling, Release → Release, ARC on → Style "Clean"/"Safe" (adaptive), Lookahead 1.5 ms. L3-LL Multi's 5 bands have
no LYNX multiband limiter (Maximus via FL is the closest; keep Apex as a full-band stand-in). Keep "Limiter" only as
the no-state fallback. Effort S (substitute table row + small translator once the RealWorld lists are read on the
trials).

**Waves RVerb / H-Reverb / TrueVerb → Reeverb 2** (FL's Reeverb 2 clone, `preset Dry=0; Wet=0.8`, slot mix 25 %)

| Original | Key controls | Best LYNX engine | Why |
|---|---|---|---|
| RVerb | Type (Hall / Room / Plate…), Pre-delay, Time, Size, Diffusion, Decay shape, Early ref level, Reverb level, Damping (5-band), EQ (5-band), Dry/Wet | **Atrium** (type / space, early/late, early size, late size, bass/high mult + xovers, predelay, decay, mix) | early/late split, room / hall / plate types and damping multipliers all exist; Reeverb 2 has no type or ER level control beyond "Early ref." |
| H-Reverb | FIR-based hall, Pre-delay, Time, Size, Build-up, Early/late, Damping & EQ, Dynamics (ducking), Modulation, Drive, Output | **Epoch Verb** (predelay, decay, size, attack ≈ build-up, bass mult/xover, high shelf, early/late diffusion, mod rate/depth, colour/mode) | the "Attack"/build-up and colour modes have H-Reverb counterparts; ducking missing everywhere |
| TrueVerb | Room size, Distance (direct/early/late balance), Early reflections, Reverb (time, density, damping), EQ | **Atrium** (room space, early send/size, early/late) | distance = early/late + direct level |

Recommendation: RVerb → Atrium, H-Reverb → Epoch Verb, TrueVerb → Atrium; keep Reeverb 2 only as fallback when no
state is readable. Without a translator, pick a neutral hall preset with Mix = 100 % wet and LYNX slot mix 25 % as
today. Effort S (table) / M (translators after reading the RealWorld lists on the trials).

**Waves Q10 / Renaissance EQ / SSLEQ → Parametric EQ** (FL's Parametric EQ 2 clone, 7 bands)

| Original | Bands / types | Best LYNX engine | Why |
|---|---|---|---|
| Q10 | 1–10 bands (Q1, Q2, Q4, Q6, Q8, Q10 components), each Bell / Shelf / Cut / Notch, gain ±18 dB, Q | **Facet EQ** (8 bands, shape / freq / gain / Q / slope, display-first) — or Parametric EQ when ≤ 7 bands | per-band shapes and slopes; display with the curve; 10-band presets need 2 bands merged or dropped |
| Renaissance EQ (REQ 2 / 4 / 6) | 2/4/6 bands, Bell / Shelf (resonant "Renaissance" shelves) / HP / LP, Gain, Q | **Facet EQ** | 6 bands fit in 8; shelf / cut shapes present |
| SSLEQ (SSL G/E EQ) | HPF, LPF, LF (shelf/bell), LMF (Q), HMF (Q), HF (shelf/bell), E/G curve | **Ridge Strip** (EQ on alone: HPF, LPF, LF/LMF/HMF/HF with bell switches and Q, EQ curve E/G) | identical band structure — the SSL E-Channel already maps there with measured laws (round 113) |

Recommendation: SSLEQ → Ridge Strip with dynamics off (reuse `translateWavesSslChannel`'s EQ part; S), Q10 and REQ
→ Facet EQ (translator per band: type, freq, gain, Q; M). Parametric EQ remains the fallback.

## Epoch Verb ← ValhallaVintageVerb (Valhalla DSP)
Sources: docs/specs/valhalla-vintageverb.md; REA_rodada100.md §1/§7b; product page (valhalladsp.com/shop/reverb/valhalla-vintage-verb/, read from the web.archive.org 2025 snapshot because the live site returns 403); ref ui_ValhallaVintageVerb.png. LYNX: `epochVerbLayout()`, `verbs::info/paramText` (src/core/LynxVerbs.cpp:1258, 1300+), lynx-before/Epoch Verb.png. Trial on this PC: yes (4.0.5 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| MIX | 0–100 %, 100 %, equal-power; top left | same law/default, top left | ✅ | |
| PREDELAY | 0–500 ms skew 2.32, 20 ms; under Mix | same | ✅ | |
| DECAY (big knob) | 0.2–70 s skew 3.36, 4.00 s | same, big knob | ✅ | |
| DAMPING HighFreq | 100–20000 Hz, 6000 Hz | same (reads "6.00 kHz") | ✅ | |
| DAMPING HighShelf | −24…0 dB, −24 dB | same | ✅ | |
| DAMPING BassFreq | 100–10000 Hz skew 4.04, 300 Hz | same, caption "Bass freq" | ✅ | |
| DAMPING BassMult | 0.25–4 X, 1.50 X | same | ✅ | |
| SHAPE Size / Attack | 0–100 %, 100 % / 50 % | same | ✅ | Slow-attack build-up shape is not reproduced (round 100). |
| DIFF Early / Late | 0–100 %, 100 % | same, box named "Diffusion" | ✅ | |
| MOD Rate / Depth | 0.1–10 Hz 2.53 Hz / 0–100 % 38 % | same | ✅ | |
| EQ HighCut / LowCut | 100–20000 Hz 8 kHz / 10–1500 Hz 10 Hz, 6 dB/oct | same | ✅ | |
| MODE menu | 22 algorithms, Concert Hall; bottom left | 22 entries in the same order, own names (Nave, Grit…, Drift…, Gated, Grand Hall, Vintage Chamber/Hall); ◀ ▶ on the menu | ⚠️ | Positions match, so translation is exact. Users won't recognise the names (renaming is deliberate). Gated envelope and per-mode early patterns are approximate. |
| COLOR menu | 1970s / 1980s / Now, 1970s | same names/default | ⚠️ | DSP is only a band limit: no reduced internal rate, no "dark and noisy" quantised modulation (spec §7.4, §7.8). |
| PRESETS row | "PRESETS: Default ◀ ▶" bottom right; click name = menu (factory folders, select/copy/save) | header preset bar: LYNX factory presets in folders, ◀ ▶, user presets (save / delete) | ✅ | Rounds 129 / 135. Her corpus is 89 % two factory presets (spec §1). |
| Preset name of the loaded state | shown in the preset field | the original's `presetName` in the header ("Detuned & Big") | ✅ | round 135 |
| Mix lock | click the MIX label to lock Mix while browsing presets (state `mixLock`) | click the MIX caption (or the preset menu entry); per engine | ✅ | round 135; the saved `mixLock` is not read (LYNX's lock is per engine, not per slot). |
| Per-control description line | one sentence at the bottom of the window for the hovered control (the "manual") | bottom strip: the hovered control's sentence, else "Mode: Plate — a plate: dense from the first moment…" | ✅ | round 135 |
| Header info | version / format / demo note top right | title + insert name | ➕/n/a | |
| Resizable UI | yes (state stores uiWidth/uiHeight) | resizable, fixed aspect | ✅ | |
| Value text entry / reset / fine drag | not documented on the product page | right-click entry, double-/Alt-click reset, Ctrl fine, wheel | ➕ | |
| A/B, undo | none in the original | song-level undo only | ✅ | Nothing missing. |
| Tempo sync | none | none | ✅ | |
| Meters / display | none | none | ✅ | |
| Bypass parameter | Bypass (VST param) | slot on/off + options menu Bypass | ✅ | |
| Value readout under each knob, caption above | yes | yes (round 114 `captionsAbove`) | ✅ | |
| Box grouping and order | Mix/Pre · Decay · DAMPING 2×2 · SHAPE · DIFF · MOD · EQ | identical order and grouping | ✅ | |
| Translation of saved state | — | every parameter (class ID + names, `steps` for Mode/Color) and the preset name | ✅ | mixLock and ui size are dropped. |

**Scores** — configuration: 7/10, design: 8/10. **After round 135**: configuration 9/10, design 9/10 (left: the 1970s / 1980s colour character).
**Top gaps** (1–3 and 5 done in rounds 129 / 135)
1. **Factory/user preset browser for LYNX engines.** LYNX would need its own named starting points plus save/copy, and the existing ◀ ▶ should step through them. Code: src/ui/PluginEditors.cpp (`presetFiles()`, header items 2–4), presets stored per EffectKind. UI + data only; shared by all six engines. **M–L**
2. **Show the translated state's preset name.** Keep `presetName` from the XML in the substitute and show it in the header box. Code: PluginSubstitutes.cpp + PluginEditors.cpp `presetName`. UI/data only. **S**
3. **Per-control description line.** Add a `help` text to EffectParamInfo (LynxVerbs.cpp `info`) and show it in the hint bar, or in a footer line inside the panel the way the reference does. Code: PluginEditors.cpp `hintAt`. UI only. **M** (writing about 100 texts across the six engines)
4. **Colour character for 1970s/1980s.** Quantised, low-rate modulation and a half-rate tank for 1970s. Code: LynxVerbs.cpp EpochFx `configure` plus the engine's mod read. Engine/DSP. **M**
5. **Mix lock.** A toggle on the Mix caption, stored in the effect state and read from `mixLock`. Code: PluginEditors.cpp + PluginSubstitutes. UI only, once presets exist. **S**

## Atrium ← ValhallaRoom (Valhalla DSP)
Sources: REA_rodada100.md §2/§7b; product page (valhalladsp.com/shop/reverb/valhalla-room/, archive snapshot); ref ui_ValhallaRoom.png. LYNX: `atriumLayout()`, lynx-before/Atrium.png. Trial on this PC: yes (2.0.5 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| MIX fader | 0–100 %, 100 % | vertical fader, 100 % | ✅ | |
| PREDELAY fader | 0–500 ms, 10 ms; fader sits at ~30 % height at 10 ms | 0–500 ms linear, 10 ms sits near the bottom | ⚠️ | The reference fader is drawn with a skewed taper. In LYNX, small predelays are hard to set by drag. |
| DECAY fader | 0.1–100 s (param linear), 2.00 s at ~35 % height | LYNX's own law, 2 s at the centre | ⚠️ | Translation uses a `curve`, but automation recorded on the original won't line up 1:1. |
| LO CUT fader | 0–1000 Hz, 0 Hz | same | ✅ | |
| HI CUT fader | 100–15000 Hz, 8000 Hz, 2nd order | same | ✅ | |
| DEPTH fader (early/late) | 0–100 %, 50 %; caption "DEPTH" | same value, caption "Depth" | ✅ | round 129 |
| Fader captions | above the faders | below the faders (values under them) | ⚠️ | Minor. |
| EARLY / LATE tabs | two tabs switch the knob grid (state `earlyLateTabState`) | Early / Late page tabs | ✅ | round 129; `earlyLateTabState` is not read |
| Early Size | 1–1000 ms, 30.0 ms | same | ✅ | |
| Early Cross | 0–1, 0.10 | same | ✅ | |
| Early Mod Rate / Depth | 0.05–5 Hz 0.50 Hz / 0–1 0.00 | same | ✅ | |
| Early Send | 0–1, 0.00 | same | ✅ | |
| Diffusion | 0–1, 1.00 | same | ✅ | |
| Space (2.0) | 0–100 %, 0 %; feedback around predelay + early | same, in Early | ✅ | |
| Late Size / Cross | 0–1, 0.5 / 1.0 | same | ✅ | |
| Late Mod Rate / Depth | 0.05–5 Hz / 0–1 | same | ✅ | |
| Bass Mult / Xover | 0.5–2 X @ 100–10000 Hz, 1 X @ 1 kHz | same | ✅ | |
| High Mult / Xover | 0.1–1 X @ 100–15000 Hz, 0.5 X @ 8 kHz | same | ✅ | |
| REVERB MODE menu | 12 types, Large Room | 12 in the same order; last four renamed (Deep/Mirror/Vast/Distant Space) | ⚠️ | Dark types are band-limited only to −20 dB at 16 kHz (reference −60) and there is a 250 Hz dip (round 100 residuals). |
| PRESET row | "PRESET: Default ◀ ▶" (factory folders incl. "Space" folder, save/copy) | header preset bar (LYNX factory + user presets, ◀ ▶) and the original's preset name | ✅ | rounds 129 / 135 |
| GUI Mode (colour schemes Electric Blue / Old School) | top-right selector | LYNX theme only | n/a | Trade dress; do not copy. |
| Description line | bottom of the window | bottom strip (hovered control, else the type) | ✅ | round 135 |
| Mix lock | state `mixLock` (MIX label is highlighted) | click the MIX caption: boxed while locked | ✅ | round 135 |
| Resizable | "Resizable GUI" | yes, fixed aspect | ✅ | |
| Gestures / text entry | not documented | LYNX's full set | ➕ | |
| True-stereo routing | stereo in/out | stereo | ✅ | |
| Translation | — | every parameter; decay through `curve`; the preset name (round 135) | ✅ | earlyLateTabState and mixLock are dropped. |

**Scores** — configuration: 7/10, design: 6/10. **After round 135**: configuration 9/10, design 8/10 (left: fader tapers, the dark types' band limit).
**Top gaps** (1, 2, 4, 5 done in rounds 129 / 135)
1. **Early/Late page tabs.** Put the two groups on `page` 0/1 with `pageTabs`, and optionally restore the page from `earlyLateTabState`. Code: PluginLayout.cpp `atriumLayout()`. UI only. **S**
2. **Preset browser and translated preset name.** Same work as Epoch gaps 1–2. **M–L / S**
3. **Display taper on the Predelay/Decay faders**, so the default and small values sit where they do on the original. Use a slider skew in the layout item while keeping the parameter law. Code: PluginLayout.cpp / PluginEditors.cpp slider mapping. UI only. **S**
4. **Fader captions above and "Depth" wording.** Code: `atriumLayout()`. UI only. **S**
5. **Description line.** Same as Epoch gap 3. **M**
6. **Dark-type band limit and the 250 Hz dip.** Code: `atriumTypes` table in LynxVerbs.cpp. Engine tuning. **S–M**

## Foil Plate ← ValhallaPlate (Valhalla DSP)
Sources: REA_rodada100.md §3/§7b; product page (valhalladsp.com/shop/reverb/valhalla-plate/, archive snapshot); ref ui_ValhallaPlate.png. LYNX: `foilPlateLayout()`, lynx-before/Foil Plate.png. Trial on this PC: yes (1.6.3 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| MIX | 0–100 %, 100 % | same | ✅ | |
| PREDELAY | 0–500 ms skew, 0.0 ms | same | ✅ | |
| DECAY (very large knob) | 0.5–30 s piecewise, 3.0 s | same law, big knob | ✅ | |
| SIZE | 0–200 %, 100 % | same | ✅ | |
| WIDTH | 0–200 % (0 = mono, 200 = anti-correlated), 100 % | same | ✅ | |
| EQ LowFreq | 20–2000 Hz, **700 Hz** | 700 Hz | ✅ | round 129 |
| EQ HighFreq | 200–20000 Hz, **10000 Hz** | 10 kHz | ✅ | round 129 |
| EQ LowGain / HighGain | ±12 dB, 0 dB | same | ✅ | |
| MOD Rate | 0.05–5 Hz skew 2.38, 0.50 Hz | same | ✅ | |
| MOD Depth | 0–100 %, 0 % | same | ✅ | |
| MODE menu | 12 plate types, Chrome | 12 in the same order, own metal names; ◀ ▶ | ⚠️ | Positions match. Per-type onsets were fitted. Mono-in types (Adamantium/Titanium/Osmium) are not modelled as mono-in. |
| PRESETS row | "PRESETS: Default ◀ ▶" | header preset bar + the original's preset name | ✅ | rounds 129 / 135 |
| Description line | bottom | bottom strip (hovered control, else the plate type) | ✅ | round 135 |
| Mix lock | state `mixLock` | click the MIX caption | ✅ | round 135 |
| Resizable | yes (uiWidth/uiHeight in state) | yes | ✅ | |
| Layout order | Mix/Pre · Decay · Size/Width · EQ 2×2 · MOD · Mode | identical | ✅ | |
| Value readouts / captions | above/below | same | ✅ | |
| Gestures | not documented | full set | ➕ | |
| Translation | — | every parameter (Type `steps`) | ✅ | |

**Scores** — configuration: 8/10, design: 9/10. **After round 135**: configuration 9/10, design 10/10 (left: the mono-in types).
**Top gaps** (1–3 done in rounds 129 / 135)
1. **Preset browser and preset name.** Same work as Epoch. **M–L / S**
2. **Fix the EQ frequency defaults** to 700 Hz and 10 kHz (0.343434 / 0.494949). Code: LynxVerbs.cpp `foilInfo`. Data only. **S**
3. **Description line.** **M**
4. **Mono-in behaviour for the three mono-in types.** Code: FoilFx `configure` (sum the input). Engine. **S**

## Nebula ← Valhalla Supermassive (Valhalla DSP)
Sources: REA_rodada100.md §4/§7b; product page with its control tooltips (valhalladsp.com/shop/reverb/valhalla-supermassive/, archive snapshot); ref ui_ValhallaSupermassive.png. LYNX: `nebulaLayout()`, lynx-before/Nebula.png. Trial on this PC: yes (5.0.0, free).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| MIX | 0–100 %, 50 % | same | ✅ | |
| MIX lock | click the boxed MIX label = lock/unlock (documented) | click the MIX caption: boxed in the accent colour while locked | ✅ | round 135 |
| WIDTH | −100…+100 % (negative swaps L/R), 100 % | same | ✅ | |
| DELAY (big) | 0–2000 ms piecewise, 300.0 ms | same | ✅ | |
| Delay sync combo | Msec / Note / Dotted / Triplet as a combo under Delay; note values 1/64…4/4 (7) | 4 tabs + a separate always-visible note menu (1/64…1/1) | ⚠️ | Same options. In the original the sync setting changes what the Delay knob sets; LYNX shows both controls at once. |
| WARP (big) | 0–100 %, 0 % | same | ✅ | |
| CLEAR button | under Warp; flushes the delay network | CLEAR push button under Warp | ✅ | round 129; not translated (it is a button). |
| FEEDBACK | 0–100 %, 50 % (gain per pass = √fb) | same | ✅ | |
| DENSITY | 0–100 %, 0 % | same | ✅ | |
| MOD Rate / Depth | 0.01–10 Hz skew 2.33, 0.50 Hz / 50 % | same | ✅ | |
| EQ High / Low | 200–20000 Hz 20 kHz / 10–2000 Hz 10 Hz, 6 dB/oct output | same | ✅ | |
| MODE menu + ◀ ▶ (5.0) | 22 modes, Gemini | 22 in the same order, own names; ◀ ▶ | ⚠️ | Density-50 % decay and ×8 shapes at long delays are approximate. |
| PRESET row | "PRESET: Default ◀ ▶"; click = select/copy/save | header preset bar + the original's preset name | ✅ | rounds 129 / 135 |
| Description line | bottom left | bottom strip (hovered control, else the mode) | ✅ | round 135 |
| Resizable | yes (uiWidth/uiHeight) | yes | ✅ | |
| Layout | Mix/Width · Delay+Warp box with sync/Clear · Fb/Density · MOD · EQ | same order | ✅ | Clear is missing. |
| Gestures | not documented | full set | ➕ | |
| Translation | — | every parameter except Clear; Sync/Note/Mode `steps` | ✅ | |

**Scores** — configuration: 7/10, design: 8/10. **After round 135**: configuration 9/10, design 9/10 (left: sync as one combo).
**Top gaps** (1–3 and 5 done in rounds 129 / 135)
1. **Clear button.** A momentary button in `nebulaLayout()` that calls the effect's buffer flush (`VerbEffect` already has reset/clear). Code: PluginLayout.cpp + PluginEditors.cpp action + LynxVerbs.cpp. Small engine hook. **S**
2. **Mix lock on the MIX caption.** **S**
3. **Preset browser and preset name.** **M–L / S**
4. **Sync as a single combo**, with the Delay knob showing and setting note values when not in Msec. The Note menu would then be hidden or merged. Code: `nebulaLayout()` + paramText. UI only (no parameter-conditional visibility exists in Layout yet). **S–M**
5. **Description line.** **M**

## Halo ← ValhallaShimmer (Valhalla DSP)
Sources: REA_rodada100.md §5/§7b; product page with mode descriptions (valhalladsp.com/shop/reverb/valhalla-shimmer/, archive snapshot); ref ui_ValhallaShimmer.png. LYNX: `haloLayout()`, HaloFx (LynxVerbs.cpp:997–1040), lynx-before/Halo.png. Trial on this PC: yes (1.2.2 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Row of 9 knobs | mix, shift, feedback, diffusion, size, low cut, high cut, mod rate, mod depth | same order, one row | ✅ | |
| mix | shown as 0.50 (raw 0–1) | 50 % | ➕ | LYNX shows units. |
| shift | ±12 st, shown 12.00 | +12.0 st | ✅ | |
| feedback | 0–1, 0.000 | 0 % | ✅ | |
| diffusion | 0–1, 0.910 | 92 % (0.919) | ⚠️ | Tiny default difference. |
| size | 0–1, 0.50 (sets decay, about 1.5 + 12.5·size s) | 50 % | ✅ | No seconds readout in either. |
| low cut | starts at 10 Hz, default 10 | the display shows what the engine applies (0–250 Hz) | ⚠️ | round 129 fixed the mismatch; the range still differs from the reference's |
| high cut | 100–15000 Hz, 8000 | same | ✅ | |
| mod rate | default shown 0.59 | 0.55 Hz (law not confirmed) | ⚠️ | |
| mod depth | 0.30 | 30 % | ✅ | |
| reverb mode | **4: Mono / Small Stereo / Medium Stereo / Big Stereo**, default mono (mono-in for Mono) | round 135: Mono (mono in), Small / Medium / Big stereo (two separate sides) first, LYNX's Wide / Dense / Bright after them; menu with ◀ ▶ | ✅ | [EXEC] the trial with a left-only impulse: below 0.25 the input is summed and the tail leaves both sides, from 0.25 up the left input stays left; steps at 0.25 / 0.5 / 0.75; translated 1:1. The default stays Wide (older projects unchanged). |
| pitch mode | **Single (up or down by Shift) / Dual / SingleReverse / DualReverse / Bypass**, default single | round 135: caption "Pitch mode", Single / Dual / Single, bright / Dual, bright / No shift | ⚠️ | The reverse-grain modes are still approximated by a +4 dB shelf (engine). |
| color mode | Bright / Dark (Dark = steep loss above ~10 kHz) | Bright / Dark (5 kHz loop band limit) | ✅/⚠️ | Dark is darker than described. |
| Programs | 8 host programs (Program param, `currentProgram`) | LYNX's own factory presets in the preset bar | — | round 137: deliberately left out — the programs are the vendor's presets (the third-party rule: never copy their presets); Halo's own factory presets play that role |
| Description line | none visible in 1.2.2's UI (the page says tooltips) | hint bar | ✅ | |
| Resizable | not in 1.2.2 (fixed small UI) | resizable | ➕ | |
| Gestures | not documented | full set | ➕ | |
| Translation | — | parameter0–11; modes through `steps` (Reverb mode 4, Shift mode 5, Color 2) | ✅ | round 135: the four reverb modes measured and kept apart. Program (`currentProgram`) is not read. |

**Scores** — configuration: 6/10, design: 8/10. **After round 135**: configuration 8/10, design 9/10 (left: reverse-grain pitch modes, programs).
**Top gaps** (1, 3, 4 done in rounds 129 / 135)
1. **Four reverb modes in the original's order** (mono-in, then three stereo sizes; LYNX's own names, e.g. "Mono / Small / Medium / Large"). Re-measure the breakpoints (likely k/4) and update the `steps` map. Code: LynxVerbs.cpp `haloModes`, `haloLayout()`, PluginSubstitutes.cpp `[valhalla-shimmer]`. Engine + data. **M**
2. **Pitch modes that match the original's behaviour.** Single (sign of Shift), Dual, reversed-grain Single/Dual (reverse each grain before shifting in the two-head shifter), Bypass. Code: HaloFx `configure` + shifter. Engine/DSP. **M**
3. **Fix the low-cut display/DSP mismatch** so both use the same law. Code: HaloFx `configure` line `s.outLowCut = 250.0 * p[5]` vs `paramText` case 5. **S**
4. **Mode menus with ◀ ▶** (the Reverb mode button is flat), and caption "Pitch mode". Code: `haloLayout()`. UI only. **S**
5. **Programs / presets.** Same as the shared preset work. **M–L**

## Horizon ← ValhallaFutureVerb (Valhalla DSP)
Sources: REA_rodada100.md §6/§7b; product page and mode descriptions (valhalladsp.com/shop/reverb/valhallafutureverb/, archive snapshot); user-manual PDF found but not readable here (equipboard copy, no PDF tool installed); ref ui_ValhallaFutureVerb.png. LYNX: `horizonLayout()`, HorizonFx (LynxVerbs.cpp:1053–1230), lynx-before/Horizon.png. Trial on this PC: yes (1.0.2 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| MIX / WIDTH | 0–100 % 50 % / width 100 % | same | ✅ | |
| Mix lock | boxed MIX label (lock) | click the MIX caption: boxed while locked | ✅ | round 135 |
| ECHO Delay (big) | 0–2000 ms piecewise, 30.0 ms | same | ✅ | |
| Echo sync | "Msec ⇕" combo under Delay (ms / note / dotted / triplet) | tabs + note menu (1/64…1/2, 6 values) | ✅ | round 137 [EXEC] the demo's EchoNote texts: 1/64, 1/32, 1/16, 1/8, 1/4, 1/2 in sixths of the range — the same list and steps as LYNX's menu / translator |
| Feedback / Level | 0–100 %, 0 % / 0 % | same | ✅ | |
| Drive | 0–24 dB, 3.0 dB | same | ✅ | |
| Spread | 0–100 %, 0 % | same | ✅ | |
| Detune | ±30 ct; **shown only in echo modes that use it** | round 135: in Drive's place in the pitch modes (Detune … Swarm), as on the reference | ✅ | [EXEC] the trial's editor in every echo mode: Drive in Modern / Tape / Digital / Analog (/ LoFi), Detune in Detune, Reverse, the three ReverseOct modes, Sparkle and Swarm |
| Mode-dependent visibility | "Only the parameters that are active in a given ECHO MODE will be visible" | round 135: Drive ↔ Detune by echo mode, Early / late ↔ Attack by reverb mode | ✅ | `LayoutItem::whenParam` conditions |
| REVERB Decay (big) | 0.2–70 s piecewise, 3.00 s | same | ✅ | |
| Echo→Rev routing | toggle button under Decay | 2 tabs under Decay | ✅ | |
| Size / Level | 50 % / 100 % | same | ✅ | |
| Early/Late / Density | 50 % / 100 % | same | ✅ | |
| Attack | exists; not visible in Chamber (mode-dependent) | round 135: in Early / late's place in Space and Freeze, as on the reference | ✅ | [EXEC] the trial's editor in every reverb mode (Nonlin not captured; it keeps Early / late) |
| MOD Rate / Depth | 0.01–10 Hz 0.10 Hz / 40 % | same | ✅ | |
| EQ HighCut / LowCut | 1000–20000 Hz 4500 Hz / 20–1000 Hz 100 Hz; they also set the high/low **decay** (loop) | same ranges/defaults; output filters only | ⚠️ | Missing the in-loop RT part (measured in round 100; the vendor says Low Freq EQ controls the bass decay of Chamber/Plate/Cathedral). |
| ECHO MODE | 12: Modern, Tape, Digital, Analog, Detune, Reverse, ReverseOctUp/Down/UpDown, Sparkle, Swarm, LoFi; default Tape | 12 in the same order (Clean…Lo-fi), default Tape, ◀ ▶ | ⚠️ | Behaviour is LYNX's interpretation (not measured): Digital 12-bit companding, Reverse 2–8 grains by Spread, Sparkle long grains. |
| REVERB MODE | 8: Room, Chamber, Plate, Hall, Cathedral, Space, Frozen, Nonlin; default Chamber | 8 in the same order (Freeze, Gated), default Chamber | ✅ | |
| COLOR | Bright / Neutral (HF RT capped 1–1.5 s) / Dark (steep 8 kHz) / Studio (steep 10 kHz + steep 600 Hz low cut); default Neutral | Bright / Neutral (16 kHz cut) / Dark (11 kHz) / "Vintage" (9 kHz + 250 Hz, gentle) | ⚠️ | The cut frequencies and slopes differ from the documented ones. |
| Preset bar | top centre "◀ Default Preset ▶" | header preset bar + the original's preset name | ✅ | rounds 129 / 135 |
| Description line | bottom | bottom strip (hovered control, else the reverb mode; [EXEC] the reference's strip reads e.g. "Modern: a clean predelay") | ✅ | round 135 |
| Resizable | yes (uiWidth/uiHeight) | yes | ✅ | |
| Gestures | not documented | full set | ➕ | |
| Layout | Mix/Width · ECHO box · REVERB box · MOD · EQ · mode row | same order | ✅ | |
| Translation | — | every parameter (modes `steps`, routing) | ✅ | |

**Scores** — configuration: 6/10, design: 7/10. **After round 135**: configuration 8/10, design 9/10 (left: colour definitions, EQ in the loop).
**Top gaps** (1, 4, 5 done in rounds 129 / 135)
1. **Preset bar.** Same as the shared preset work. **M–L**
2. **Colour modes to the documented values**: Dark = steep 8 kHz; the 4th colour = steep 10 kHz plus steep 600 Hz high-pass; Neutral = an HF RT cap (≈ 1–1.5 s) instead of an output cut. Code: LynxVerbs.cpp `horizonColourCut` / HorizonFx `configure`. Engine. **S**
3. **EQ in the loop.** Low/High EQ should also scale the low- and high-band RT. Code: HorizonFx `configure` (the absorption bands already exist). Engine. **S–M**
4. **Echo-mode / reverb-mode dependent visibility** (Detune, Attack, Drive…). Needs a `visibleWhen param/value` field in Layout items. Code: PluginLayout.h/.cpp + PluginEditors.cpp. UI framework change. **M**
5. **Mix lock, description line.** **S / M**

---
**Cross-cutting findings (all six):**
- **Shared gaps.** No preset browser or preset name, no mix lock, and no per-control description line. These are the first things a Valhalla user touches. A/B, undo, meters and oversampling are absent in the originals too, so they are not gaps.
- **Measured-value checks.** Every measured parameter law and almost every default is identical. The only defaults that are off are Foil Plate's EQ frequencies (711 Hz vs 700 Hz, 10.1 kHz vs 10 kHz).
- **The real configuration differences:**
  - Halo's mode lists (Shimmer has 4 reverb modes and reverse-grain pitch modes).
  - Halo's low-cut display/DSP bug.
  - Horizon's colour definitions and loop EQ.
  - Nebula's missing Clear button.
- **Translation.** Every saved value maps to the substitute, but `mixLock`, `presetName`, ui size, Room's tab state and Shimmer's program are dropped. Shimmer's 4 reverb modes collapse into 3.
- **Web access.** The valhalladsp.com pages return a Cloudflare 403, so they were read from web.archive.org snapshots via curl.

Sources: [VintageVerb](https://valhalladsp.com/shop/reverb/valhalla-vintage-verb/), [Supermassive](https://valhalladsp.com/shop/reverb/valhalla-supermassive/), [Room](https://valhalladsp.com/shop/reverb/valhalla-room/), [Plate](https://valhalladsp.com/shop/reverb/valhalla-plate/), [Shimmer](https://valhalladsp.com/shop/reverb/valhalla-shimmer/), [FutureVerb](https://valhalladsp.com/shop/reverb/valhallafutureverb/), [FutureVerb Reverb Modes](https://valhalladsp.com/2025/10/03/valhallafutureverb-the-reverb-modes/), [FutureVerb manual PDF](https://images.equipboard.com/uploads/item/manual/159855/valhalla-futureverb-manual.pdf) (downloaded, not parsed).

## Echoform ← ValhallaDelay (Valhalla DSP)
**Sources:** docs/analise/REA_rodada101.md §1–3 and §8 (our black-box measurements); [ValhallaDelay: The Controls](https://valhalladsp.com/2019/04/16/valhalladelay-the-controls/), [The STYLE Control](https://valhalladsp.com/2019/04/16/valhalladelay-the-style-control/), [1.5.2 update: three new modes and ducking](https://valhalladsp.com/2019/11/26/valhalladelay-update-three-new-modes-and-ducking/) (the pages were read through archive.org, because the live site returns 403); reference screenshots `valhd101/ref/delay*.png` (Single, Quad, Ratio, Dual, Note, Digital, Ghost, Pitch); LYNX `lynx-before/Echoform.png`; `echoformLayout()` and `echosuite::echoformInfo()` / `paramText()`. **Trial on this PC:** yes (3.0.5 demo).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Mix | 0–100 %, 50 %, equal-power law; top of the left box | Mix 0–100 %, 50 %, equal-power, same place | ✅ | |
| Mix lock | Click the "MIX" label to lock Mix while browsing presets | — | ✅ | Only useful once presets exist; round 129: "Keep the Mix when loading presets" in the preset menu |
| Style | Single / Dual / Ratio / PingPong / Quad knob under Mix, default Single | Style knob with 5 steps, same place; order Single / Ratio / Quad / Dual / Ping-pong; the substitute reorders | ✅ | The knob order is different, so turning it gives a different sequence than the original |
| Style changes which controls are shown | Spread / Ratio / Spacing / Delay R swap in place | Same swaps (`when` items) | ✅ | |
| Delay time | 0–20 000 ms, piecewise law, default 300 ms, big knob | Same law and default | ✅ | |
| Delay label L / R in Dual and PingPong | Captions read "DELAY L" and "DELAY R" | Captions read "Delay L" and "Delay R" in Dual / Ping-pong | ✅ | round 129 (row re-checked in round 137) |
| Sync combo under the time knob | Msec / Note / Dotted / Triplet; "DELAY Sync" becomes L and R in Dual / PingPong | Same 4 choices, same place, R menu only in Dual / Ping-pong | ✅ | |
| Note value | 1/64 … 8/1 (10 values); the knob turns into a note knob | Same 10 values; note knob replaces the time knob | ✅ | |
| Spread (Single) | ±20 ms, 0 ms | Same, ±¼ applied per side as measured | ✅ | |
| Ratio (Ratio) | 1–100 %, 61.8 % | Same | ✅ | |
| Spacing (Quad) | ±100 %, 0 % | Same | ✅ | |
| REPEAT / SWELL button (Quad) | Feedback from TapD (Repeat) or from all active taps (Swell) | — (always takes feedback from the last tap) | ✅ | Needs an engine change; round 135w: Repeat / Swell (Quad): feedback from the last tap or the mean of the taps heard [model] |
| TapA / B / C / D on/off (Quad) | 4 buttons under the time knob, greyed out in the other styles | — (all 4 taps always on) | ✅ | Needs an engine change; round 135w: Taps A B C D (Quad, second row) |
| Feedback | 0–200 %, 70 % | Same | ✅ | |
| Width | ±100 %, 100 % | Same | ✅ | |
| Color: Drive | 0–24 dB, 0 dB | Same | ✅ | |
| Color: Age | 0–100 %, 50 %; meaning depends on mode (asperity, splice, BBD noise, bit depth) | Age 50 %, per-character meaning | ⚠️ | Not every mode-specific Age effect is modelled (e.g. tape splices, bit-depth Age in Pitch) |
| Diff Amount | 0–100 %, 0 = OFF | Same, shows "off" | ✅ | |
| Diff Size | 0–100 %, 100 % | Same | ✅ | |
| Mod: Wow / Flutter (tape modes) | 0–100 %, 50 / 50 | Same, shown in Tape only | ✅ | Original also shows them in DuckTape and ChromeTape; LYNX maps both to Tape |
| Mod: Rate / Depth (HiFi / BBD / Digital) | 0.01–10 Hz, 0.50 Hz; 0–100 %, 50 % | Same values, shown for Lo-Fi … Phaser | ✅ | |
| Section title changes to FREQ (Ghost) / PITCH (Pitch) | The section header is renamed | Mod / Freq / Pitch by mode | ✅ | round 129 (row re-checked in round 137) |
| Freq Shift / Detune (Ghost) | ±1000 Hz (\|x\|^5), 0 Hz; ±10 Hz, 0.10 Hz | Same laws and defaults (Shifter mode) | ✅ | |
| Pitch Shift / Detune (Pitch / RevPitch) | ±24 st, 0; ±100 c, 0 | Same (Pitch, Reverse) | ✅ | |
| EQ High / Low | 200–20 000 Hz, 20 kHz; 10–2000 Hz, 10 Hz; in the feedback path | Same, High above Low | ✅ | |
| Ducking | 0–100 % one-knob ducking (feedback + output); comes with the duck modes | Ducking knob in an "Output" section, active in every mode; round 137: the original's curve | ✅ | round 137 [EXEC] measured on the demo (1 kHz tone at −29 / −15 / −9 dBFS rms, ducking 25–100 %): a gain reduction on the echoes keyed by the input = input rms − T, T = −51 dB × ducking^1.28, released at ≈ 85 dB/s; LYNX uses that law (REA_rodada137 §5) |
| Mode list | 17 modes: Tape, HiFi, BBD, Digital, Ghost, Pitch, RevPitch, BBDuck, Clarity, DuckTape, PitchDuck, LoFi, Quartz, PhaserDDL, RichPitch, ChromeTape, Analog | 12 characters | ⚠️ | BBDuck → bucket (loses brightness that follows the delay time), Clarity / Quartz → Clean (loses Clarity's limiter and Era waveforms), DuckTape / ChromeTape → Tape, PitchDuck → Pitch; built-in ducking lost |
| Mode selector with ◀ ▶ | Bottom left, "MODE: Tape ◀▶" | Bottom left menu with ◀ ▶ | ✅ | |
| Era | Past / Present / Future, ◀ ▶, bottom middle; changes filters, saturation, diffusion and Clarity's waveforms | round 137: Past / Present / Future tabs, bottom right; default Past (the old sound) | ✅ | [EXEC] the demo's default is Past (value 1/3; Present from 2/3, Future at 1); the first repeat's top end opens with the era (Tape 8 kHz −10.1 / −7.1 / −6.5 dB, Digital 12 kHz −45 / −6.6 / −6.3, LoFi 8 kHz −10 / −5.3 / −1.8, BBD 4 kHz −27 / −17 / −8; HiFi and Clarity unchanged): LYNX scales each character's loop low-pass by the fitted factor; the era's saturation / diffusion differences are not modelled |
| Colour theme follows the mode | The whole panel recolours per mode family (green tape, blue digital, grey Ghost, navy Pitch) | Fixed theme | ⚠️ | Design cue only; a LYNX accent per mode family would do, without copying the original's colours |
| Preset browser | "PRESET: Default ◀▶" bottom right, large factory library | Header box shows "(no preset)" | ✅ | Applies to all LYNX engines; round 129's preset bar (LYNX factory presets for Echoform) |
| Description line | Bottom strip describes the current Style / Mode, and the hovered control | bottom strip (hovered control, else the mode) | ✅ | round 135 (row re-checked in round 137) |
| Text entry / reset gestures | Not confirmed in our refs | Right-click value entry, double-click / Alt-click default, Ctrl fine drag, wheel | ✅ | |
| Resizable window | Resize corner; uiWidth / uiHeight saved in the state | Resizable, fixed aspect ratio | ✅ | uiWidth / uiHeight not translated (harmless) |
| Output level | — | Level ±18 dB | ➕ | |
| Ping-pong "Pairs" toggle | — | Pairs / alternate | ➕ | |
| Echo-train display | — | Echoes on a time axis | ➕ | |
| Saved-state translation | — | Translates everything (round 137: + Era, TapA–D, RepeatSwell); 17 modes → 12 characters; duck variants lose built-in ducking | ⚠️ | `PluginSubstitutes.cpp [valhalla-delay]` |

**Scores:** configuration 6/10, design 6/10.

**Top gaps:**
1. **Era selector** (bottom row, between Mode and Preset). Needs a new `Era` parameter in `echoformInfo()`, per-character voicing in `Echoform::configure()` and a map line for `xml:Era`. Engine change, L.
2. **Quad TapA–D and Repeat/Swell buttons** under the time knob, shown only in Quad. Needs 5 new parameters, a tap mask in the quad read loop and Swell feedback (sum of active taps) in `EchoSuite.cpp` around lines 565 and 618. Also map TapA–D and RepeatSwell. Engine change, M.
3. **Description strip** under the bottom row: a one-line text for the current style and mode, plus the hovered control's hint. Add it in `PluginEditors.cpp`; it could become a generic "info line" layout item. UI only, S–M.
4. **Missing modes**: give BBDuck, Clarity (limiter), DuckTape / PitchDuck (built-in ducking) and Quartz their own characters, and update the map's `steps`. Engine change, M–L.
5. **Ducking**: show it only in the duck characters, or keep it global but measure the original's curve. Engine plus layout, M.
6. **Section title follows the mode** ("Freq" / "Pitch"), and the left delay caption reads "Delay L" in Dual / Ping-pong. Add a `when` on section labels in `echoformLayout()`. UI only, S.
7. **Preset browser plus Mix lock**: needs a LYNX factory / user preset system (affects all engines). L.

---

## Spiral Echo ← ValhallaFreqEcho (Valhalla DSP)
**Sources:** REA_rodada101 §2 and §4; [FreqEcho product page](https://valhalladsp.com/shop/delay/valhalla-freq-echo/) (archive.org); `ref/FreqEcho.png`; `lynx-before/Spiral Echo.png`; `spiralLayout()` and `spiralInfo()`. **Trial on this PC:** yes (1.2.0).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Mix | 0–100 %, 50 %, linear law; first knob | Same | ✅ | |
| Delay | 0.01–1000 ms (skew 3.322); screenshot shows **0.01 ms** (knob at minimum) | Same law; default **100 ms** (v = 0.5) | ⚠️ | Default differs |
| Shift | ±1000 Hz (\|x\|^5), 0 Hz; big centre knob | Same, big centre knob | ✅ | |
| Feedback | 0–100 % (loop gain 1.5×), 50 % | Same | ✅ | |
| Low Cut | 20–3000 Hz, 200 Hz | Same | ✅ | |
| High Cut | 50–15 000 Hz, 5000 Hz | Same | ✅ | |
| Delay Sync | Free plus 23 notes (straight / dotted / triplet), under Mix / Delay | Same 24 entries, same place | ✅ | |
| Output Mode | Mono / Stereo, Stereo default, bottom right | Same | ✅ | |
| Per-pass shift, opposite shift in stereo | — | Matches the measurement | ✅ | |
| Presets | None | None | ✅ | |
| Tooltip / help line | Product page says tooltips show bottom left; none visible in the screenshot | Main-window hint bar | ⚠️ | |
| Resizable | Fixed size | Resizable | ➕ | |
| Gestures | Not confirmed | Full LYNX set | ✅ | |
| Saved-state translation | — | All 8 settings | ✅ | |

**Scores:** configuration 9/10, design 9/10.

**Top gaps:**
1. **Delay default**: set `{ "Delay", 0.0f }` in `spiralInfo()` if the 0.01 ms default is confirmed on a fresh instance (the screenshot suggests it). Parameter default only, S.
2. Optional: a small spiral / spectrum display. Not needed for parity.

---

## Orbit ← ValhallaSpaceModulator (Valhalla DSP)
**Sources:** REA_rodada101 §2 and §5; [SpaceModulator product page](https://valhalladsp.com/shop/modulation/valhalla-space-modulator/) (archive.org); `ref/SpaceModulator.png`; `lynx-before/Orbit.png`; `orbitLayout()` and `orbitInfo()`. **Trial on this PC:** yes (1.1.6).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Mix | 0–100 %, 50 %, equal power | Same | ✅ | |
| Rate | ±10 Hz (\|x\|^3.333), 0.10 Hz | Same (+0.10 Hz) | ✅ | |
| Depth | 0–50 ms (skew 2.322), 10 ms | Same | ✅ | |
| Feedback | ±100 %, 0 % | Same | ✅ | |
| Manual | ±50 ms (\|x\|^4), 0 ms | Same | ✅ | |
| Five knobs in a row, each its own colour | Mix, Rate, Depth, Feedback, Manual | Same order, each knob its own LYNX colour | ✅ | |
| Mode | 11 modes (Up, Up/Down, TZFlange±, TriFlange, Ocho, Doubler, VariUp, VariUpDown, Ensemble360, Symphonic), clickable text "Mode: Up" bottom left | 11 modes renamed (Rise …), menu with ◀ ▶ bottom left | ✅ | |
| Mode voices | Measured tap ranges per mode | Own designs within the measured ranges | ⚠️ | Round 101 marks finer captures as still to do |
| Preset browser | "PRESETS: Default ◀▶" bottom middle / right | header preset bar (LYNX factory + user presets, ◀ ▶) | ✅ | rounds 129 / 135 |
| Description line | Bottom strip, e.g. a one-line summary of the current mode | bottom strip: the hovered control, else "Mode: Through-zero + — a through-zero flanger…" | ✅ | round 135 |
| Resizable | Fixed size | Resizable | ➕ | |
| Gestures / text entry | Not confirmed | Full LYNX set | ✅ | |
| Saved-state translation | — | All 6 settings | ✅ | |

**Scores:** configuration 7/10, design 7/10. **After round 135**: configuration 8/10, design 9/10 (left: the per-mode voices).

**Top gaps** (1–2 done in rounds 129 / 135):
1. **Mode description line** under the mode row, one sentence per mode. Same info-line item as Echoform. UI only, S.
2. **Factory presets** with ◀ ▶ next to the mode. Needs the shared preset system, L.
3. **Voice shapes per mode** checked against finer captures (the round 101 follow-up), in `orbit::Orbit::process`. Engine change, M.

---

## Lattice Mod ← ValhallaUberMod (Valhalla DSP)
**Sources:** REA_rodada101 §2 and §6; [UberMod: The Modes](https://valhalladsp.com/2012/01/06/valhallaubermod-the-modes/), [The TAPS Parameters](https://valhalladsp.com/2012/01/26/valhallaubermod-the-taps-parameters/), [The DIFF Parameters](https://valhalladsp.com/2012/01/27/valhallaubermod-the-diff-parameters/) (archive.org); `ref/UberMod.png` and `uber_*.png`; `lynx-before/Lattice Mod*.png`; `latticeLayout()`, `latticeInfo()` and `lattice::tapCount()`. **Trial on this PC:** yes (1.1.6).

| Original feature | Original (range / default / where) | LYNX now | Status | Note |
|---|---|---|---|---|
| Faders Mix / Depth / Width / Delay / Feedback, values under them | 50 % / 50 % / 100 % (0–200) / 10 ms (0–1000) / 0 % | Same faders, values and defaults | ✅ | |
| **MODE** menu (under the faders) | Named algorithms: 2TapChorus (default), 4TapEnsemble, SuperSix, 6TapRandom, DualEnsemble, 8Tap, 16Phase, 16Tap, 32Tap; each has its own LFO design (antiphase triangle, 3-phase, randomised triangle, 16-phase) | round 135: a "Mode" menu with the nine algorithms under LYNX's own names (2-tap chorus … 32-tap) on the reference's type index, default 2-tap chorus (round 129), ◀ ▶ | ⚠️ | The three 6-tap modes and the two 16-tap modes still play alike (one random-detune plus one vibrato LFO for every mode — vox136 / engine) |
| Sync | Free plus 17 note values, next to the mode | round 144: the reference's list on its own value law (Free, 1/16 … 2/1, 1/16. … 1/1., 1/16T … 1/1T), next to the mode; translated | ✅ | round 144 [EXEC] (REA_rodada144a §1): index = floor (17 v) (boundaries at k / 17, checked ± 0.002); 0 and 1 both free; 2–7 straight 1/16 … 2/1, 8–12 dotted 1/16. … 1/1., 13–17 triplet 1/16T … 1/1T; the synced delay is capped at 2750 ms (2/1 and 1/1. at 120 BPM; uncapped at 240 BPM). The older by-length list stays for LYNX projects that use it (shown only while it is set) |
| Page tabs | MOD / TAPS / DIFF / EQ / DRIVE / WARP, top right | Same 6 tabs, same order | ✅ | |
| MOD page | SlowRate 0.50 Hz, SlowDepth 50 %, VibRate 6.00 Hz, VibDepth 0 %, OverMod 1.0× | Same 5 knobs and defaults | ✅ | |
| TAPS page | Spread 50 %, Slope 0, Skew 0, Random 0, TapGain 0 dB | Same | ✅ | Smoothing of tap-time changes (Warp Smoothing) should be checked |
| DIFF page | On/Off (OFF), Diffusion 50 %, Size 20 ms, ModRate 0.50 Hz, ModDepth 50 % | Same | ✅ | |
| EQ page | Mode Bright / Dark, Low Cut 10 Hz, High Cut 20 kHz, SpatialEQ 300 Hz | Same | ✅ | |
| DRIVE page | On/Off, InGain 9 dB, OutGain −6 dB, NoiseGain −90 dB, Pre/Post (POST) | Same | ✅ | |
| WARP page: Speed, fbMix, Smoothing | 1X / ½X, 50 %, 100 ms | Same | ✅ | |
| WARP page: InputPan | 0–8 steps, shows "Stereo" | round 137: Input pan menu on the WARP page (Stereo / Swapped / Mono / Mono left / Mono right) | ✅ | [EXEC] left- and right-only impulses through the demo: index 0–1 stereo (default 1), 2 swapped, 3 (L + R) / 2 to both, 4 to the left only, 5 to the right only, 6–8 like stereo; translated one to one |
| 16-tap alternating polarity / type ≥ 10 | Measured | Not modelled | ⚠️ | |
| Preset browser | "PRESET: Default ◀▶" next to the page area | header preset bar + the original's preset name | ✅ | rounds 129 / 135 |
| Description / tooltip line | Bottom strip shows hover help (e.g. the OverMod warning) | bottom strip: every control described ("Over-mod — pushes the modulation past its normal range (can sound unstable)") | ✅ | round 135 |
| Tap display | — | Tap graph under the pages | ➕ | |
| Resizable | Fixed size | Resizable | ➕ | |
| Gestures / text entry | Not confirmed | Full LYNX set | ✅ | |
| Saved-state translation | — | Everything (round 137: InputPan; round 144: DelaySync) | ✅ | `PluginSubstitutes.cpp [valhalla-ubermod]` |

**Scores:** configuration 6/10, design 7/10. **After round 135**: configuration 7/10, design 9/10 (left: per-mode LFO designs, InputPan, Sync translation). **After round 144**: configuration 9/10, design 9/10 (left: per-mode LFO designs, 16-tap polarity).

**Top gaps** (1 and 5 done in round 135, except the per-mode sound):
1. **Named Mode menu with the correct default**: replace "Taps" with 9 LYNX-named algorithms using the measured `type` index (2TapChorus = index 1), and set the default to index 1 (1.5/24) instead of 6.5/24. Changes `latticeLayout()`, `latticeInfo()` and `tapCount()`. UI plus default, S.
2. **LFO structure per mode** (antiphase triangle, 3-phase, randomised triangle, 16-phase) in `LatticeMod::process`, so SuperSix, 6TapRandom, DualEnsemble and 16Phase sound different from each other. Engine change, M–L.
3. **Sync translation**: measure the 18 sync steps, confirm the list, then add `map = xml:DelaySync -> Sync`. Engine plus map, S.
4. **InputPan** on the WARP page: new parameter, input routing and map line. Engine change, M (meaning to be measured first).
5. **Description line plus presets**: the shared items described in the other plugins' gaps. S / L.

---

**Common to all four:**
- **No presets or Mix lock.** The original Delay, SpaceModulator and UberMod all ship factory presets; FreqEcho has none.
- **No in-panel help line.** Valhalla's plugins "self-document" through tooltips at the bottom of the panel; LYNX only shows hints in the main window's hint bar.

A single "info line" layout item in `PluginEditors.cpp` would close the help-line gap for all four in one change.

## Ember Saturator ← Decapitator (Soundtoys)
Sources: the official trial (Decapitator 5.5.5, hosted black-box in our host: its 12 host parameters with every value
text, screenshots, and measurements of sines / noise through it — docs/analise/REA_rodada135s.md §2); docs/specs/decapitator.md;
`mixsuite::ember` in src/core/MixSuite.{h,cpp}; `EmberEditor` in src/ui/ProEditors.cpp. Round 135 re-checked every row on the trial
(it was "manual only" before). The Soundtoys control strip (◀ ▶ preset name, save, revert, settings, menu, BYPASS) has
**no A/B**: LYNX's header (undo / redo, A/B, presets) is a superset.

| Original feature | Original (range / default / where) [EXEC trial] | LYNX now | Status | Note |
|---|---|---|---|---|
| Drive | 0.0…10.0, big knob, default 0; ≈ 3.2 dB per step at −18 dBFS | Drive 0…+36 dB; read-out "+x dB (n.n)" on the 0–10 scale (3.6 dB / step) | ✅ | round 135: the 0–10 read-out |
| Punish | on / off, top left; ≈ +20 dB before the drive | **Punish** in the strip on top of the display | ✅ | round 135: +20 dB pre-drive (single- and multiband) |
| Style A / E / N / T / P | 5 one-click buttons, bottom row | **one-click voicing row** (Tape pre, Console, Grit, Triode, Pentode; "Style menu" = the old 4 styles) | ✅ | round 135: five voicings (a curve each — a new "console" curve for E — and their own broad EQ, from the measured responses); own names |
| Per-style tone / harmonics | E brightest top (+3 dB > 8 kHz), N darker top, T thinner bass, P bass bump [EXEC noise, Drive 5] | each voicing's pre / post shelves follow these | ⚠️ | voicing curves not fitted to the measured harmonics (round 136 tunes the models) |
| Low Cut | 20 Hz…1 kHz, 12 dB/oct, before the drive | **Wide cuts**: 20 Hz…1 kHz (old range 20–500 Hz kept for older projects) | ✅ | round 135 |
| Thump | switch beside Low Cut: a resonant bump at the cutoff (+3.5 dB) | **Thump** (low cut Q 1.6) | ✅ | round 135 |
| Tone | Dark…Bright ±12 (a pre-drive tilt: +12 = −18 dB at 30 Hz / +3 dB at 16 kHz) | Tilt ±6 dB around 1 kHz, after the curve | ⚠️ | position (pre / post) left to the DSP round |
| High Cut | 1…20 kHz, gentle ≈ 6 dB/oct | **Wide cuts**: 1…20 kHz, 6 dB/oct (old: 2–20 kHz, 12 dB/oct) | ✅ | round 135 |
| Steep | switch beside High Cut: ≈ 30 dB/oct | **Steep** (5th-order Butterworth, 30 dB/oct) | ✅ | round 135 |
| Auto | on (default); links the Output knob to Drive in the GUI — the audio is identical on / off [EXEC] | Auto gain (RMS make-up) | ⚠️ | different law; her states are translated with LYNX's auto gain off and her trim |
| Output | −24…0 dB, main panel | **Output on the band panel** (±18 dB), beside Dry / wet | ✅ | round 135: moved from the bottom bar to the panel |
| Mix | Dry…Wet, default 100 % | Dry / wet | ✅ | |
| VU meter ("attitude") | analog VU on top | **IN / OUT meters** at the right of the display with the output read-out | ✅ | round 135 |
| Latency | 62 samples at 48 kHz [EXEC] | the oversampler's latency (reported) | ✅ | |
| Oversampling | not exposed | 2× / 4× / 8× (bottom bar) | ➕ | |
| Multiband (Saturn role) | — | up to 4 bands | ➕ | |
| Preset strip ◀ ▶ / save / revert | Soundtoys strip | LYNX preset bar (◀ ▶, factory incl. a "Voicings" folder, user presets) | ✅ | |
| A/B | — (none in the strip) | LYNX's A/B | ➕ | |
| Translation of her saved state | encoded | **her 7 distinct states translated** (read in the trial: style A, Drive 1.9–2.0, Tone +1.7 / +4.2, High Cut 8.1 kHz / off, Mix 88 / 100 %, Trim −3.0 / −3.2 dB) | ✅ | round 135: payload hash → settings (core/SoundtoysStates.inc); other states: the light default |
| One-screen layout | VU top, Drive left, filters row, styles row | display + band panel; the reference's switches in one strip on top | ⚠️ | layout differs (spectrum-first editor) |

**Scores**: configuration 4/10 → **8/10**, design 4/10 → **7/10** (round 135).

**Left**: Tone before the curve; the voicings' harmonic profiles fitted to the trial (round 136); Auto as an Output-follows-
Drive link instead of a make-up gain.

---

## Fruity Delay 3 (stand-in) / Echoform ← EchoBoy (Soundtoys), with PrimalTap
Sources: docs/specs/echoboy.md (Pro Tools-era guide pp. 22–54, store page); `[soundtoys-echoboy]` / `[soundtoys-primaltap]` in src/core/PluginSubstitutes.cpp (→ Fruity Delay 3, no preset, no translation); `fruityDelay3()` and `echoformLayout()` in src/core/PluginLayout.cpp; `echoform::*` in src/core/EchoSuite.cpp; screenshots lynx-before/Fruity Delay 3.png and Echoform.png. PrimalTap facts are from search snippets of the manual (gear4music PDF copy), not a full read. Trial on this PC: no. **Manual only (trial not installed).** Round 135: the EchoBoy / PrimalTap trials are now active; their parameter lists, a screenshot and her 22 EchoBoy states read black-box in the trial (Ping-Pong 1/4 note, feedback 0.52, saturation 3.72, "Edited Style" in most slots) are in docs/analise/REA_rodada135s.md §4.

| Original feature | Original (range / default / where) | Fruity Delay 3 now | Echoform now | Status (best) | Note |
|---|---|---|---|---|---|
| Echo Mode: Single | per-channel echo | Delay model Mono/Stereo | Style "Single" (+ Spread) | ✅ (Echoform) | |
| Echo Mode: Dual | two independent times, FB mix / balance | Offset only | Style "Dual" (Time L/R, sync L/R) | ✅ | no FB cross-mix or FB balance; round 135w X-feed, round 144 FB balance; **round 150** [EXEC]: every style in Dual = its Single sound on each side (first-echo tone, per-pass tone, wobble within 0.1 dB / the same track), each side with its own time and the L/R offset law; no cross-feed (a left-only input leaves the right silent) — the voicings do the same |
| Echo Mode: Ping-Pong | mono in, L then R | Ping pong | Style "Ping-pong" (+ Pairs) | ✅ | **round 150** [EXEC] with every style: the input (L + R) / √2 (+3 dB against Single for L = R), left at Time 1, right at Time 1 + Time 2 with exactly the left echo (same level and tone, no own wobble), each left-right pair the feedback gain and a style-dependent pair tone (e.g. the telephone style band-passes every pair); with a voicing and "Stereo: Measured" Echoform does the same (the pair tone fitted per voicing: ≤ 2 dB rms, median 0.4 dB; round 147's law: median 4.7 dB) |
| Echo Mode: Rhythm (≤16 taps, 1–8 beats, grid ½…1/64, Shape) | Rhythm editor | — | round 137: RHYTHM switch (third row): evenly spaced taps (ms / note / dotted / triplet), Repeats 1–16, Decay 0–40 dB, Length 1–8 beats; the echo train shows the pattern | ✅ (Echoform) | [EXEC] measured on the trial (impulses at 60 / 120 BPM; its host parameters RhythmMode / Note / Time / Repeats / Decay): Repeats = slots incl. the input, so repeats − 1 taps are heard, each 1 / √slots (equal power, with the decay spread from the first slot to the last); the pattern spans 2 beats (taps past it are dropped) and the feedback replays it every 2 beats — LYNX's Length defaults to 2 beats. Left: the Pattern presets and the Custom (drawn) taps of the rhythm editor (not host parameters, not measured); **round 144**: the rhythm editor (a row under the panel): the taps on the pattern's time axis, drag = level / time, double-click = add / remove a tap; Even / Free / Grid edits (Grid ½ … 1/64 — EchoBoy's Custom Time / Custom Beats roles), Shape (Decay, Reverse, Swell, Fade, Nonlin — LYNX's own laws, [model]), and a Patterns menu of 12 LYNX-authored patterns (gallop, tresillo, clave, stutter, triplet roll …; none copied). [EXEC] round 144: on the trial the Pattern / Custom Time / Custom Beats modes give no echo when only its host parameters are set (their taps live in its editor), so those are not measured |
| Echo Style menu (~30 voicings) | main panel menu | — | **round 147**: Echo voicing menu (Style edit row): 31 voicings with LYNX names, one per factory style, each measured black-box on the trial (first-echo tone and per-pass loop tone from noise bursts, steady-sine gain / harmonics at Saturation 0 … 24 dB, pitch wobble, impulse spread) and modelled (fitted biquad tones ≤ 0.3 dB rms for most, a knee shaper or a compressor, sine / random wobble, diffusion) | ✅ (Echoform) | [EXEC] REA_rodada147a §2; EchoBoy's style names are not shown; the reference's L/R offset (−3.8 / +4.2 ms around the time) stays Echoform's Spread; **round 150**: the offset is measured and built (Tweak: L/R Offset row); the wobbles re-measured as delay tracks (impulse trains, 24 s): no style has several voices (a steady tone shows no beating: AM < 0.05 dB in all 31; the "fat" / ambient styles are a fixed tap pattern that moves as one), the CE-1-like style's swing is a triangle (1.63 Hz, ±1.0 ms), the others sines or smooth random swings — the voicings' depths now within 11 % median of the reference's delay swing (round 147: 40 %) |
| Echo Time ms | ms, pitch-bends when moved | Time + Keep pitch | Time L/R 0–20 s, default 300 ms | ✅ | |
| Note / Dotted / Triplet | Note/Dot/Trip buttons | Tempo sync | Sync menu ms/Note/Dotted/Triplet, 1/64…8/1 | ✅ | |
| Tempo source | host, tap, MIDI, 30–300 BPM | host | host only | ✅ | no tap tempo, no free BPM; round 129 tap pad + round 135w own BPM (30–300) |
| Mix | Dry…Wet, 12 o'clock = 50/50 | Wet/Dry faders | Mix 0–100 % (default 50) | ✅ | |
| Feedback | Min…Max, can run away | Level | 0–200 %, default 70 % | ✅ | |
| Saturation | Less…More, style-dependent, centre default | Feedback distortion Sat/Limit | Drive 0–24 dB (Colour) | ✅ | **round 147**: with a voicing, Drive = EchoBoy's Saturation 0 … 24 dB with each style's own measured law (drive / make-up per 6 dB step; e.g. studio tape: −3.5 dB at low level up to 12 dB, then +2.3 dB at 24; the radio / limited styles compress) |
| HighCut | style-dependent darkening | FB cutoff/res, LP/HP/BP | High cut 200 Hz–20 kHz + Low cut 10 Hz–2 kHz | ✅ | |
| Prime Numbers | detunes repeat times | — | — | ✅ | trivial: snap times to primes in samples; round 135w (Echoform): "Primes" — the delay times snap to a prime number of samples |
| Groove (shuffle…swing) | alternate repeats | — | — | ✅ | round 135w (Echoform): Groove ±25 % = odd repeats late / early by that share of the echo time ([EXEC] EchoBoy trial: +0.25 → +113…125 ms at 500 ms) |
| Feel (rushin'…draggin') | global pre-delay ± | Offset (partial) | Spread ±20 ms (partial) | ✅ | round 135w (Echoform): Feel ±25 % moves the whole train by that share of the echo time ([EXEC]: +0.125 → +56…62 ms at 500 ms) |
| Input level (echo path only) | −24…+24 dB | Input Wet | — | ✅ | **round 144**: Echoform "Input level" −24 … +24 dB on the echo path only (Style edit row) |
| Output level (echo only) | −24…+24 dB | Wet | Level ±18 dB | ✅ | |
| Tweak: Accent | bipolar | — | — | ✅ | round 135w (Echoform): Accent ±100 % (odd vs even repeats) [MANUAL: not a host parameter on the trial, law not measured] |
| Tweak: Width | min…max, past 3 o'clock out-of-phase | Stereo | Width ±100 % | ✅ | |
| Tweak: L/R Offset | 0–25 ms, default 8 | Offset | Spread ±20 ms | ✅ | **round 150** [EXEC]: at the default 8 ms the left side's odd repeats come 4 ms early and its even ones 6 ms late, the right side the other way round (+4 / −6 ms), for every style and time, in Single and Dual (not in Ping-Pong); Echoform: "L/R offset" 0–25 ms (default 8) with a voicing and Stereo "Measured" (two loops per side, every pass coloured); other offsets scale the measured law [model] |
| Tweak Dual: FB Mix / Balance / FB Balance | knobs | — | — | ✅ | round 135w (Echoform): X-feed (each side's feedback from the other) and Balance; no FB Balance; **round 144**: FB balance (±100 %, each side's feedback) |
| Style Edit: 3-band EQ gain + per-repeat Decay, Mid Res | ±18 dB etc. | FB filter only | Low/High cut only | ⚠️ | the "EQ in the loop vs first repeat" idea is missing; **round 144**: loop EQ (bass 120 Hz shelf, mid 1.2 kHz bell, treble 5 kHz shelf, ±18 dB) inside the loop — every repeat is shaped again; no per-repeat decay / Mid Res |
| Style Edit: saturation type (Clean, Tape, Warm, Pump, Dirt, Limit×4) | menu | Limit/Sat + knee/symmetry | Saturation type menu | ✅ | **round 147**: Auto / Clean / Tape / Tube / Squash / Grit / Ceiling — the measured curves of six representative styles (digital, studio tape, valve tape, FM radio's compressor, distorted, limited); works with or without a voicing. [EXEC] the reference's own type menu lives in its editor (not a host parameter), so its types were not measured one by one [model] |
| Style Edit: Diffusion + Size, Loop/Post | knobs + switch | Diffusion level/spread | Diffusion amount/size | ✅ | no Loop/Post choice; **round 144**: Diffusion Loop / Post |
| Style Edit: Wobble, Rate, Shape (5), Sync | knobs/menu | Mod rate/time | Wow, Flutter (Tape) / Rate, Depth | ✅ | no shape menu, no LFO sync; **round 144**: Wobble shape (sine, triangle, square, saw, random) and LFO sync (free, 1/16 … 2 bars) for the LFO modes |
| Ducking | — (not in EchoBoy) | — | Ducking | ➕ | |
| Pitch / shifter / reverse modes | — | — | yes | ➕ | |
| Echo display | Tweak "visualiser" of delay tails | — | "echotrain" display of repeats | ✅ (Echoform) | |
| Freeze (PrimalTap) | holds the loop; ≤2048 ms at 8× | — | — | ✅ | round 135w (Echoform): Freeze holds the loop (no input, unity feedback, no colour) |
| Multiply 2×/4×/8× (PrimalTap; halves the rate per step) | knob | Smp rate / Bits (lo-fi) | Multiply 1x / 2x / 4x / 8x (Style edit row) | ✅ | **round 147** [EXEC] PrimalTap 5.5.5: the delay × 2 / 4 / 8 exactly, a brick-wall band of ≈ 6.2 / 3.1 / 1.55 kHz (−43 … −49 dB a third of an octave above), up to 8 s (16 s gave silence); LYNX: the same factor on the times (up to 20 s) and two 8th-order low-passes at 12.4 kHz / factor inside the loop |
| VCO Rate/Depth per tap, phase flip (PrimalTap) | knobs | Mod rate | Rate/Depth (one LFO) | ⚠️ | |
| Preset menu / user presets / A/B | Soundtoys strip | FL factory list only; no user save, no A/B | "(no preset)" | ✅ | round 129's preset bar (Echoform) |
| Gestures, value entry, automation | standard | generic editor: full set | full set | ✅ | |
| Translation of the saved state | encoded | none, FL defaults (ms delay, FB, etc. unrelated) | her states recognised (round 136) | ⚠️ | round 136 / re-checked round 152: the encoded preset is not decoded; her EchoBoy states are recognised by their hash and play Echoform's measured Tape voicing, any other state the Studio reel voicing at 1/4 note, 25 % feedback (`[soundtoys-echoboy]`) |

**Scores**: Fruity Delay 3 as the stand-in: configuration 2/10, design 2/10. Echoform: configuration 5/10, design 5/10. **After round 144** (Echoform): configuration 9/10, design 8/10 (round 137: 8 / 7) — the rhythm editor (patterns, free / grid taps, shapes) and the Style Edit role (input, loop EQ, FB balance, diffusion place, wobble shape / sync); left: the saturation types, the ~30 style voicings, PrimalTap's Multiply. **After round 147a**: configuration 10/10, design 8/10 — the 31 measured voicings, the style-dependent Saturation, six saturation types, PrimalTap's Multiply; left: the reference's per-voicing Style Edit details (per-repeat decay, Mid Res), the chorus styles' multiple modulated voices (one wobble per side here), PrimalTap's per-tap VCO. **After round 150a**: configuration 10/10, design 8/10 (sound closer) — the voicings in Dual / Ping-Pong as measured (ping-pong copy, summed input, pair tone), EchoBoy's L/R offset (a control, measured law), the wobbles' shapes and depths from delay tracks; the "several voices" of the chorus styles do not exist on the trial (one modulated read per side, measured), so nothing is missing there; left: the per-voicing Style Edit details, PrimalTap's per-tap VCO, EchoBoy's offset law at offsets other than 8 ms (not a host parameter).

**Verdict:** **Echoform is clearly the better EchoBoy (and PrimalTap) substitute.** It has the four-mode-like Style switch (Single/Dual/Ping-pong + Ratio/Quad), a character Mode menu standing in for EchoBoy's Style, Drive ≈ Saturation, Wow/Flutter ≈ Wobble, Diffusion, note/dotted/triplet sync and an echo display. Fruity Delay 3 is a clean FL delay with none of the "style" concept, played at FL defaults. The spec's own recommendation (§7.1) agrees. H-Delay already moved to Echoform in round 119.

**Top gaps** (ordered by what a user touches first):
1. **Point `[soundtoys-echoboy]` and `[soundtoys-primaltap]` at Echoform**, with a conservative default: Tape mode, low Age, 1/4 note at host tempo, ~25 % FB, mix ~25–30 %. File: src/core/PluginSubstitutes.cpp. Table only. Effort S.
2. **Rhythm mode** (≤16 taps on a 1–8-beat grid, Shape presets) as a 6th Echoform Style. New params + DSP in `echoform::Echoform`, a grid display in PluginEditors.cpp. Effort L.
3. **Groove / Feel / Accent / Prime Numbers.** Four new Echoform params (timing offsets per repeat, a prime-snap switch). Engine change, small DSP. Effort S–M.
4. **Saturation centre default and style-dependent saturation/HC** (Drive interacting with Mode). Engine change. Effort M.
5. **Dual-mode FB Mix / FB Balance; EQ-in-loop "Decay" per band.** Engine change. Effort M.
6. **Freeze switch** (PrimalTap). Engine change. Effort S.
7. **Tap tempo** in the Delay section. UI + param. Effort S.
8. **Preset box / A/B / user presets** for LYNX engines (generic editor, src/ui/PluginEditors.cpp). Effort M.

---

## Auto-pan (stand-in) ← PanMan (Soundtoys)
Sources: the official trial (PanMan 5.5.5, black-box: 16 host parameters, screenshots of its sources, measurements —
docs/analise/REA_rodada135s.md §3); `AutoPan` / `autopan::` in src/core/Effects.{h,cpp}; `autoPanLayout()` in PluginLayout.cpp. Round 135
gave Auto-pan the rhythmic-panner sources while FL's own LFO (the Love Philter conversion it was made for) stays its
default source.

| Original feature | Original [EXEC trial] | LYNX now | Status | Note |
|---|---|---|---|---|
| Offset | −105…+105°, default 0 | Offset (the Pan control) ±100 % | ✅ | ±90° = a hard side |
| Width | 0…210°, default 180 | Width (Depth) ±100 % (180° = 100 %) | ✅ | no "beyond the speakers" above 180°; round 136's Rhythm voicing: 0–210°; **round 150** [EXEC]: past ±90° the angle folds back with the far side inverted (offset 45° + 90° of modulation → left at tan 7.5° of the right, polarity inverted) — the Rhythm voicing's "Beyond" switch (new instances on; projects saved before keep the clipped angle) |
| Smoothing | Hard…Soft, default Soft | **Smoothing** (LFO: square → sine; steps: jump → glide over a third of a step) | ✅ | round 135 |
| Pan law | equal power: centre −3 dB per side, the far side silent at the extremes | **equal-power balance** for the new sources (FL's laws for the FL source) | ✅ | round 135, measured |
| Rate (free) | 100 s … 10 Hz (0.01 · 1000^v Hz), default 0.25 Hz | **Rate** 0.01–10 Hz, same law and default | ✅ | round 135 |
| Sources: LFO / Rhythm Step / Rhythm Shape / Random / PingPong / Random Step | LFO button; rhythm note + steps (2…8); the two audio-triggered sources | **Source**: FL LFO · LFO · Rhythm steps (2–8 steps on 1/1…1/32 notes); **round 152** (the Rhythm voicing's Trigger row): **Ping-pong** and **Random step** moved by the input (threshold, divider 1–12, the detector's HP / LP filter, *Trigger now*), **Random steps** (the clock's steps at random places, reproducible) | ⚠️ | round 135; round 152 [MANUAL docs/specs/panman.md; laws model — the sources are not host parameters]; no free rhythm-shape editor (her "shape" presets play as 2 steps) |
| Direction ◀ / ◀▶ / ▶ | three buttons | **Direction** (right to left, back and forth, left to right) | ✅ | round 135 |
| Input / Output | −24…+24 dB, LED meters | **Input / Output** ±24 dB + IN / OUT meters | ✅ | round 135 |
| Analog / Digital | switch (default Analog): asymmetric saturation, H2 −36 dBc at −18 dBFS | **Digital / Analog** (fitted on the measured harmonics) | ✅ | round 135 |
| Pan position display | L … C … R LED strip | **pan-position strip** (live) | ✅ | round 135 |
| Feel / Groove | knobs (rhythm modes) | **round 150**: Feel / Groove ±0.25 of a step in the Rhythm voicing (the whole pattern / the odd steps late or early) | ⚠️ | [EXEC] on the trial they do nothing in LFO mode (all host-parameter runs identical); the rhythm modes are not reachable through host parameters, so the law is LYNX's [model] (EchoBoy's measured Groove / Feel shape) |
| Tweak: envelope / trigger (Rate / Width mod, threshold, divider, manual trigger) | Tweak page | **round 150**: the Rhythm voicing's Tweak row — Rate mod ±3 oct, Offset mod / Width mod ±180°, Threshold, Range | ✅ | [EXEC] (OffsetMod +90°, Width 0, 1 kHz stairs −60 … 0 dBFS and steps): the follower hears the input before Input / Output gain; no modulation below −20 dBFS (a sine's peak), linear in amplitude to full at −9 dBFS (0 / 28 / 54 / 90° at −30 / −15 / −12 / −6 dBFS); attack ≈ 21 ms, release ≈ 290 ms (one-pole fit, 2 % rms); Rate mod = octaves × amount, Offset / Width mod = degrees × amount (offset clamped to ±105°). Left: the editor's threshold / sensitivity values other than the default (not host parameters: LYNX's Threshold / Range follow the measured default, other settings scale it [model]), the divider and ManualTrigger (each `process` restarts the trial's LFO from the same phase, so a trigger is not observable). Round 152: the divider (1–12) and the manual trigger, with the trigger sources (Trigger row) |
| Tap tempo / MIDI | in rhythm modes | follows the project tempo | ⚠️ | |
| Presets | Soundtoys strip | FL presets (Auto-pan is an FL effect) | ⚠️ | |
| Translation of her saved state | encoded | **her 7 settings translated** (LFO 3.56 Hz / 150° / soft — 52 % of her slots; 5-step and 2-step rhythm presets; analog) | ✅ | round 135: the hidden source / steps / direction read from the trial's window per setting |

**Scores**: configuration 3/10 → **7/10**, design 2/10 → **6/10** (round 135). **After round 150a**: configuration **8/10**, design **7/10** — the Tweak row (envelope → rate / offset / width, measured follower and default threshold), Feel / Groove, the measured "beyond the speakers" fold; left: the divider / manual trigger, the free rhythm-shape editor, tap tempo / MIDI. **After round 152**: configuration 8 → **9/10**, design 7 → **8/10** — the trigger sources (Ping-pong / Random step by the input's threshold, divider, detector filter, manual trigger) and the Random source in a Trigger row; left: the free rhythm-shape editor, tap tempo / MIDI, the trigger laws measured (not reachable through host parameters).

---

## Foil Plate ← Little Plate (Soundtoys)
Sources: the official trial (Little Plate 5.5.5, black-box: 5 host parameters — Mix, Decay 0.5 s…∞, Low Cut 20 Hz…1 kHz,
Mod on / off; screenshot); `foilInfo` / `FoilFx` in src/core/LynxVerbs.cpp. The substitute is Foil Plate since round 129.

| Original feature | Original [EXEC trial] | Foil Plate | Status | Note |
|---|---|---|---|---|
| Decay (one big knob) | 0.5 s … 32 s … ∞, default 3 s | Decay 0.5–30 s (big knob) + **Infinite** | ✅ | round 135: ∞ holds the plate |
| Low Cut | 20 Hz…1 kHz, default 20 | **Low cut** 20 Hz–1 kHz (0 = off), 12 dB/oct | ✅ | round 135 |
| Mix | 0–100 %, default 100 % | Mix (the substitute: 100 % wet at the slot's 25 %) | ✅ | |
| Mod | on / off | Mod rate / depth | ✅ | [EXEC] round 150: Mod on leaves the decay per band unchanged (≤ 0.1 s at 3 s); its tail tone is narrower, not wider (spectral spread 5.4 → 2.4 Hz on a 1 kHz tail) — not modelled |
| Plate character | one EMT-style plate | 12 plate types | ✅ | **round 150**: **Measured plate** switch [EXEC] (impulses at Decay 0.5 / 1 / 2 / 3 / 5 / 10 / 20 s and ∞): the plate speaks after 0.7 ms (Foil's types after 1 … 81 ms), the decay per octave against the label (20 s: 35 s at 125 Hz, 14 s at 1 kHz, 5.5 s at 8 kHz; short labels ring a little longer in the presence band; ∞ ≈ 80 s), fitted through Foil's engine: mid / bass / high RT within 5 % at every label up to 20 s (∞: the highs die sooner than on the trial), the tail's energy within ±0.5 dB; the Little Plate substitute plays it |
| Extras (predelay, size, width, EQ) | none on the panel | yes | ➕ | a fuller panel than the 3-knob original; round 152: the **Plate view** shows only the reference's controls (Full \| Plate tabs) |
| Presets | Soundtoys strip | LYNX preset bar | ✅ | |

**Scores**: configuration 6/10 → **8/10**, design 5/10 → **6/10** (round 135). **After round 150a**: configuration **9/10**, design **6/10** — the measured plate voicing (onset, decay per band, level); left: Mod's narrowing of the tail, the single-knob panel (Foil keeps its fuller layout). **After round 152**: configuration **9/10**, design 6 → **8/10** — the **Plate view** (View: Full \| Plate; the Little Plate substitute opens in it): Low Cut · the big Decay with its ∞ · Mod · Mix, the reference's one-knob panel; left: Mod's narrowing of the tail.

---

## Voice Shift ← Little AlterBoy (Soundtoys)
Sources: the official trial (Little AlterBoy 5.5.5, black-box: 7 host parameters with their value texts, screenshot,
measurements of harmonic test tones — docs/analise/REA_rodada135s.md §1); docs/specs/little-alterboy.md;
src/core/VoiceShift.{h,cpp}; `voiceShiftLayout()`. Round 135 created **Voice Shift**, LYNX's own voice shifter (YIN +
TD-PSOLA from core/VocalPitch.h), for this role: 696 slots in 399 of her projects had no LYNX module before.

| Original feature | Original [EXEC trial] | Voice Shift | Status | Note |
|---|---|---|---|---|
| PITCH | −12.0…+12.0 st, 0.1 st read-out, default 0 | Pitch ±12 st, read-out box (0.1 st), big knob | ✅ | transpose +2.5 st: 231 Hz on both |
| FORMANT | −12.0…+12.0 st, default 0 | Formant ±12 st, read-out box | ✅ | |
| LINK | on / off between the knobs | Link (formants follow the pitch; Formant offsets them) | ✅ | |
| MODE Transpose / Quantize / Robot | three buttons | Mode radio (same three) | ✅ | Quantize: nearest semitone at once (210 → 208 Hz on both); Robot: C4 at Pitch 0 (262 Hz on both), Pitch moves it |
| DRIVE | 0.0…10.0 | Drive 0–10 (tube stage on the shifted voice, ≈ 1.75 dB / step, make-down above 6) | ✅ | curve fitted roughly on the trial's level / harmonics (round 136 may refine) |
| MIX | 0–100 %, default 100, linear | Mix (linear, dry delayed by the latency) | ✅ | measured linear on the trial |
| Latency | 2417 samples at 48 kHz (reported) | two periods of 70 Hz (≈ 1376 at 48 kHz), reported | ⚠️ | different (both compensated by PDC) |
| MIDI → pitch | host MIDI | round 137: MIDI channel stepper under the modes: the highest note held on that Channel Rack channel replaces Pitch (relative to C4) in Transpose / Quantize and is the note Robot sings | ✅ | [MANUAL] the original's MIDI mode; the trial's MIDI response was not measured (our measurement host sends no notes) |
| One-screen panel | Pitch / Formant · Mode · Drive / Mix | the same three sections | ✅ | |
| Presets | Soundtoys strip | LYNX preset bar with 7 factory presets | ✅ | |
| Substitute entry | — | `[soundtoys-littlealterboy]` (VST2 `StLa`, names) | ✅ | round 135 |
| Translation of her saved state | encoded | **her 4 distinct settings translated** (Quantize, formant +1.2 st, drive 4.6, mix 100 % = 84 % of her slots; an octave-down Transpose; two 44 % mixes); others: her usual setting | ✅ | round 135 |

**Scores**: configuration 0/10 → **9/10**, design 0/10 → **8/10** (round 135).

Sources (web): [Decapitator guide (B&H copy)](https://static.bhphoto.com/lit_files/89645.pdf), [SOS Soundtoys 5 review](https://www.soundonsound.com/reviews/soundtoys-5); the rest from the official trials (round 135).
