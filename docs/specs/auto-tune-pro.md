# Antares Auto-Tune Pro (Auto mode): spec dossier

LYNX substitute: **LYNX Tune** (`src/core/LynxTune.cpp`, translation in `src/core/PluginSubstitutes.cpp`
`translateAutoTunePro`). Written for round 125 (spec125). Clean-room: this dossier uses only public vendor manuals,
public patents, papers, her own project files, and our own black-box measurements of the official trial (round 120).
No Antares binary was opened, decompiled or instrumented.

Tags: [DOC] vendor manual/page · [PATENT] · [PAPER] · [MEAS] measurement (ours = REA doc link; third-party = URL) ·
[DATA] read from her project files · [model] our inference (basis given). Inline refs `[Sn]` point to §9.
All sources were accessed on 2026-10-21.

---

## 1. Identity

| | |
|---|---|
| Vendor | Antares Audio Technologies. Auto-Tune was invented by Harold "Andy" Hildebrand; his original pitch detection and correction patent was filed by Auburn Audio Technologies and later assigned to Antares [S6][PATENT] |
| Product | Auto-Tune Pro, which has two engines: **Auto mode** (real-time, low latency) and **Graph mode** (offline-style editing, ARA). Per the manual they are "two different audio paths": you only hear the one that is selected [S1 p.35][DOC] |
| Versions in her corpus | Each state is a `VC2!` chunk holding `<ChunkForVersion ChunkVersion="10.0.0">` (Pro X-era layout, 143/143 states). 72 of those 143 also carry a `ChunkVersion="11.0.0"` section with the Pro 11 Harmony Player and Modern-scale data [DATA, tmp/tune120/at]. The trial measured in round 120 was **v11.6.0.2336, VST3** [MEAS][S17 §3] |
| Lineage | Classic Mode "simulates an early Auto-Tune algorithm" ("the Auto-Tune 5 sound"). The manual says the sound has changed subtly with each version as Formant Correction, Throat Modeling and Flex-Tune were added [S1 p.35][S2 p.19][DOC] |
| Related products in her corpus | Auto-Tune EFX+ (17 slots / 12 projects / 6 states) and Auto-Key (52 slots / 52 projects / 4 states). See §4.9 |

**Her usage** [DATA]:
- The whole-laptop inventory (`tmp/r113/inventory_all.txt`, 412 projects) counts **727 slots / 399 projects / 692
  distinct states**. That makes it her second most used third-party plugin, practically one per vocal insert.
  (key, scale) counts there: (6,1) 273 · (4,1) 167 · (11,1) 115 · (2,1) 104 · (3,1) 19 · (7,1) 12 · … · scale 0 in
  14 slots. **No scale-2 (chromatic) slot appears in that inventory.**
- The 24 main projects (`tmp/gf`, decoded in round 120 with `tmp/tune120/py/atstates.py` / `atdecode.py`) hold **143
  distinct states**: 98 minor, **43 chromatic**, 2 major. **Open: why the two corpora disagree on chromatic
  (§8).**
- The Auto-mode values in the 143 states ([S17 §5], re-checked here from `tmp/tune120/at`):

| X | Name (from the trial's own parameter list, §3) | Her values (143 states) |
|---|---|---|
| X4 | Retune Speed (ms) | 0 … 14. Mostly 5.75 – 8.2 (e.g. 7.44 ×18, 5.75 ×15, 6.04 ×13, 6.96 ×11, 0 ×9) |
| X61 | Humanize | 100 ×137 · 68 ×2 · 65 ×2 · 30 ×1 · 0 ×1 |
| X90 | Flex-Tune | 20 ×109 · 22 ×26 · 18 ×2 · 17 ×2 · 0 ×2 · 16 ×1 · 10 ×1 |
| X62 | Natural Vibrato (dB) | 0 ×143 |
| X11 | Use Classic Mode DSP | 0 ×141 · **1 ×2** |
| X10 | Input Type | 1 = Alto-Tenor ×143 |
| X9 | Tracking | 50 ×143 |
| X70 / X71 | Formant Correction / Throat Length % | On ×142 (Off ×1) / 100 ×143 |
| X74 / X3 / X92 | Transpose / Detune / Wet-Dry Mix | 0 / 0 / 100 % (all states) |
| X5 | Vibrato Shape (Create Vibrato) | 0 = No Vibrato (all states). X6 18, X7 5.5, X46/X50 500, X47 10, X48 70 and X49 20 are the factory defaults of the unused vibrato generator |
| X44 | Ignore Vibrato | Off (all states) |
| X1 / X2 | Scale / Key | Same values as `<Custom ScaleGivenScale / ScaleGivenKey>` |
| X31 | Object Retune Speed (Graph mode) | 20 (default) everywhere. Round 113 misread this as the live retune |
| Custom `LowLatency_ID` | Low-latency preference | 0 (all states) |
| X91 | Latency Removal | Off (all states) |

- Scale edits [DATA]: in the legacy `<Scales>` block of the 10.0.0 chunk, the active scale's Bypass/Remove masks are
  the factory ones in every state but one. In that one state (minor) the **root note is set to Bypass**
  (`Bypass Values="1000000"`).

## 2. Signal flow (Auto mode)

```
 in (mono, or stereo; "L/R Ref" X34 picks the detection channel)
   │
   ├──► pitch detector (periodicity test, Input Type sets the period search range,
   │      Tracking sets the periodicity threshold)
   │          │ detected pitch p(t)  (cents, re A4 = 440 Hz + Detune)
   │          ▼
   │    target selection: nearest On note of (Key, Scale, per-note Remove/Bypass, keyboard
   │      octave edits, MIDI Target Notes);  Bypass → no correction for that note
   │          │ target T
   │          ▼
   │    correction law: d = T − p  →  Flex-Tune gate (§4.3)  →  Retune (1st-order) with
   │      Humanize slow-down on sustained notes (§4.1, §4.2)  →  Natural Vibrato gain (§4.4)
   │          │ applied shift s(t) (+ Transpose, + Create-Vibrato LFO)
   │          ▼
   └──► pitch shifter  (Modern: with Formant Correction / Throat model [S1 p.36];
          Classic: "Auto-Tune 5" DSP, no formant/throat/transpose/flex [S1 p.35])
              │
              ▼
          Wet/Dry Mix (X92) ─► out, delayed by the reported latency (2670 samples @ 48 kHz measured, §4.6)
```

No saturation, noise or EQ stage is documented or measured in Auto mode. Mix < 100 gives a "chorus-like effect"
because dry and shifted signals overlap [S1 p.38][DOC].

## 3. Controls (Auto mode and globals)

Ranges, defaults and tapers come from the trial's own parameter texts, read through the VST3 host API at 101
normalised positions (`C:\Users\kauan\lynx-test\tune120\at_params.tsv`, `at_rs.tsv`; round 120 host fxmeasure)
[MEAS]. "X n" = parameter ID n = the `X<n>` entry of `ParamsData` [S17 §5]. Behaviour text is from the manuals.

| Control | Range | Unit | Default | Law / taper (normalised v) | Audible effect | State |
|---|---|---|---|---|---|---|
| Retune Speed | 0 … 400 | ms | 20 (v = 0.6856) | **ms = (50/6)·(49^(1−v) − 1)** (fits all 1001 sampled points of the integer-rounded text within 0.5 ms; v = 0 → 400, v = 1 → 0) [MEAS fit] | 0 = immediate pitch changes, vibrato fully suppressed ("Auto-Tune effect"). 10 – 50 = natural [S1 p.40][DOC] | X4 |
| Humanize | 0 … 100 | — | 0 | linear | "applies a slower Retune Speed only during the sustained portion of longer notes" [S1 p.41][DOC] | X61 |
| Flex-Tune | 0 … 100 | — | 0 | linear | 0 = always pulls; higher = smaller correction area around each note [S1 p.40][DOC] | X90 |
| Natural Vibrato | −12 … +12 | dB | 0 | linear in dB | Reduces or increases the vibrato already present. Works even when every note is Bypassed [S3 p.29][DOC] | X62 |
| Tracking | 1 … 100 | — | 50 | linear | Higher = "relaxed" (noisy or breathy input); lower = "choosier" (fewer clicks) [S2 p.21][DOC] | X9 |
| Input Type | Soprano, Alto-Tenor, Low Male, Instrument, Bass Inst. | — | Alto-Tenor | 5 steps | Algorithm "optimized for different vocal ranges" (detection range) [S1 p.30][DOC] | X10 |
| Key | C … B | — | C | 12 steps | — | X2 |
| Scale (legacy list) | 29 entries: Major, Minor, Chromatic, Ling Lun, Scholar's Lute, Greek Diatonic/Chromatic/Enharmonic, Pythagorean, Just Major, Just Minor, Meantone, Werckmeister III, Vallotti, Barnes, Indian, Slendro, Pelog, Arabic 1, Arabic 2, 19/24/31/53 Tone, Partch, Carlos α/β/γ, Harmonic [MEAS][S3 pp.30–32][DOC] | — | Chromatic | 29 steps (index 0 Major, 1 Minor, 2 Chromatic) | — | X1 |
| Modern Scale (v11) | 15: Chromatic, Major, Minor, Harmonic Minor, Jazz Melodic Minor, Dorian, Phrygian, Lydian, Mixolydian, Locrian, Maj/Min Pentatonic, Blues, Whole Tone, Diminished [S1 pp.31–32][DOC] | — | Chromatic | 15 steps | — | X162 (unused by her states) |
| Detune | −100 … +100 | cents | 0 | linear | Shifts the target reference (A = 440 Hz ± 100 c) [S2 p.20][DOC] | X3 |
| Formant Correction | Off / On | — | On | — | Corrects formants when shifting [S2 p.20][DOC] | X70 |
| Throat Length | 50 … 180 | % | 100 | sampled: 50 / 72 / 100 / 135 / 180 at v = 0 / .25 / .5 / .75 / 1 | Length of the modelled vocal tract (formant effect); only active with Formant on [S1 p.36][DOC] | X71 |
| Transpose | −12 … +12 | semitones | 0 | steps | Real-time transposition on top of the correction [S1 p.36][DOC] | X74 |
| Transpose In Scale / Scale Transpose | Ignoring/In Scale; −7 … +7 steps | — | Ignoring / 0 | — | "Conform to Scale" (Major/Minor only) [S1 p.37][DOC] | X179 / X181 |
| Wet-Dry Mix | 0 … 100 | % | 100 | linear | Dry/processed balance [S1 p.38][DOC] | X92 |
| Use Classic Mode DSP | 0 / 1 | — | 0 | — | "Auto-Tune 5 sound": slightly brighter, more pronounced attack and note transitions at fast Retune. Disables Formant, Throat, Transpose, Flex-Tune and time editing [S1 p.35][S2 p.19][DOC] | X11 |
| Ignore Vibrato | Off / On | — | Off | — | Helps target choice when a wide vibrato nears the next semitone [S1 p.52][DOC] | X44 |
| Create Vibrato: Shape, Rate, Pitch, Amplitude, Formant, Variation, Onset Delay, Onset Rate | No Vibrato/Sine/Square/Sawtooth; 0.1 – 10 Hz; 0 – 100 c; 0 – 30; 0 – 100; 0 – 100; 0 – 1500 ms; 0 – 1500 ms | | No Vibrato; 5.5 Hz; 18 c; 10; 70; 20; 500 ms; 500 ms | | Synthetic vibrato [S1 pp.47–49][DOC] | X5, X7, X6, X47, X48, X49, X46, X50 |
| Target Notes via MIDI / Learn Scale from MIDI / MIDI Octaves | Off/On; Off/On; As Played/All | | Off | | MIDI targets ("To Notes": no MIDI note held → audio passes **untuned**) [S1 p.54][DOC] | X37 / X36 / X58 |
| Use Low Latency (preference) | Off / On | — | Off | — | "minimize processing delay" for live use [S1 p.109][DOC] | `<Custom LowLatency_ID>` |
| Latency Removal | Off / On | — | Off | — | Not documented in the manuals we read. **Open (§8)** | X91 |
| Correction Mode | Auto / Graph | — | Auto | — | Selects the audio path | X0 |

Per-note scale state: `<Scales>` (legacy, 10.0.0 chunk) and `<ModernScales>` (11.0.0 chunk) store, for each scale
index, `Bypass` and `Remove` masks relative to the root (e.g. Major `Remove="010100101010"`, Minor
`"010010100101"`) and 78-note `BypassEx` / `RemoveEx` masks for keyboard edits in single octaves [DATA]. The manual
keeps edits per scale ("each scale retains its own edits") [S1 p.50][DOC]. A Removed note retargets to the next
closest scale note. A Bypassed note passes uncorrected [S1 pp.43–44][DOC].

## 4. Known curves & behaviour

All [MEAS] numbers below were measured on the official trial at 48 kHz in round 120 ([S17 §5]): chromatic scale,
held notes stepping 30 c sharp mid-note, YIN tracking at a 2 ms hop.

### 4.1 Retune Speed: a first-order pull
- A held note that goes 30 c sharp mid-note is pulled back along a first-order curve. t50 / t90 = 4/20 (R 5), 10/30
  (8), 22/64 (20), 50/162 (50), 98/318 (100), 198/638 ms (200). So **τ ≈ 1.36 R − 3 ms (t50 ≈ R)** [MEAS][S17 §5].
- Vibrato ±40 c at 5.5 Hz that survives: 3 % (R 0), 20 % (5), 35 % (8), 68 % (20), 91 % (50) [MEAS][S17 §5].
  - A one-pole correction makes output = p − LP(p − T), so the surviving vibrato is the high-pass part:
    retention = ωτ/√(1+(ωτ)²) with ω = 2π·5.5. With the measured τ this predicts 0.13 / 0.26 / 0.64 / 0.91 at R 5 /
    8 / 20 / 50, against 0.20 / 0.35 / 0.68 / 0.91 measured [model: arithmetic].
  - At fast speeds Auto-Tune keeps ~7–9 points more vibrato than the pure one-pole. Detector smoothing or lag is a
    likely cause, and it matters for her R ≈ 6–8.
  - Round 120 found LYNX within 1 point of Waves on vibrato. **The Auto-Tune vibrato retention of LYNX at R 5–8
    has not been compared yet (§8).**
- **An onset from silence is corrected in ≈ 30 ms at every speed** [MEAS][S17 §5]. Retune Speed therefore does not
  govern a note's first correction. This fits a detector that first needs a few periods and then applies the
  current target directly. The patent says detection happens "within a few cycles" [S6][PATENT].
- Patent form of the law: the raw ratio `Cycle_period / desired_Cycle_period` is smoothed by a user `Decay` ∈ [0, 1],
  with 0 = instantaneous [S6][PATENT]. The patent describes this in words only. A one-pole
  `r₁[k] = D·r₁[k−1] + (1−D)·raw[k]` updated every detector hop reproduces the measured first-order curve, with
  `D = exp(−hop/τ)` [model].

### 4.2 Humanize: slow-down on *steady* sustained notes
- A note held steady for 0.8 s, then 30 c sharp, is pulled back as if the speed were R + H ms. R 0: H 25/50/75/100 →
  t50 26/50/74/100 ms. R 5 → 30/54/80/104. R 20 → 46/70/94/118 [MEAS][S17 §5].
- After only 0.3 s of steadiness the slow-down is partial. **A note with even ±5 c of vibrato does not count as
  sustained** [MEAS][S17 §5].
- Vendor description: "a slower Retune Speed only during the sustained portion of longer notes" [S1 p.41][DOC].

### 4.3 Flex-Tune: dead-band that grows with F
- A note d cents from its target is corrected by **c(d) = min(d, (1 − F)/F · (50 − d))** cents (F 10 … 90,
  d 5 … 50 c). Examples: F 50: d 30 → 20 c, d 45 → 5 c. F 90: 4.5 c at most [MEAS][S17 §5].
- In a scale, d counts in **hundredths of the gap to the next target** on the sung side. E minor, F + 20 c (80 c
  below F#): F 50 corrects 23 c, F 80 6 c, F 20 fully [MEAS][S17 §5].
- Vendor tutorial (A2–A3 sweep, Retune 0): at Flex-Tune 10 the "correction range around each scale note is still
  quite wide". At 55 the range is narrow and the sweep passes between notes uncorrected [S3 p.28][DOC]. Classic mode
  disables Flex-Tune [S1 p.35][DOC].
- Her value F ≈ 20: c(d) = 4·(50 − d), so every note within 40 c of the target (40 % of the gap) is fully corrected
  and the region near the midpoint is left partly free.

### 4.4 Natural Vibrato
- ±12 dB "roughly doubles / removes" the vibrato that survives the correction [MEAS][S17 §5]. The law (dB of what:
  depth in cents? 20·log or 40·log?) and how vibrato is separated from note drift are **not measured**.
- Vendor: it is active "even when pitch correction is not being applied" (all notes Bypassed) [S3 p.29][DOC]. So it
  works on the pitch trajectory itself, not only on the correction.
- **All 143 of her states have 0 dB**, so this control is neutral for her catalogue.

### 4.5 Target selection
- Nearest On note; Remove → next closest note; Bypass → no correction [S1 pp.43–44][DOC].
- Ignore Vibrato prevents flipping between neighbours on wide vibrato [S1 p.52][DOC]. Off in her states, so expect
  flips when a ±50 c vibrato crosses the midpoint between two targets [model].
- MIDI "To Notes" with no note held → no tuning [S1 p.54][DOC]. Off in her states.

### 4.6 Latency, sample rate, channels
- **Reported latency 2670 samples at 48 kHz (55.6 ms)**, Auto mode, Low Latency off, offline render (`nonrealtime 1`)
  [MEAS][S17 §5]. Low-latency mode and 44.1 kHz were not measured (§8).
- Antares documents only "Use Low Latency … to minimize processing delay" [S1 p.109][DOC]. A support article notes
  that the Low Latency toggle of Pro X 10.6.0 / EFX+ may not take effect until it is toggled off and on [S16][DOC].
- Stereo: an `L/R Ref` parameter (Left/Right Channel) chooses the detection reference [MEAS param list].

### 4.7 Formant / Throat
- Formant Correction uses "throat modeling technology … passing it through a physical model of the human vocal
  tract". The Throat knob sets the modelled length: left = higher and thinner, right = deeper and fuller
  [S1 p.36][DOC].
- Her states: Formant On and Throat 100 % (neutral). The audible requirement is **timbre preservation under
  ≤ ±50 c corrections** (and up to several semitones on hard-tuned jumps).

### 4.8 Classic mode
- "Slightly brighter quality … more pronounced attack and transition between notes at faster Retune Speeds".
  Formant, Throat, Transpose, Flex-Tune and time editing are disabled [S1 p.35][S2 p.19][DOC].
- Two of her 143 states use it. Their Flex-Tune (X90) is then ignored by the plugin. LYNX should ignore it too.
- Hypothesis [model, based on the patent's method and on "no formant" in Classic]: Classic shifts by plain
  resampling with period repetition/dropping (the patent method [S6]). Formants then move with the correction ratio,
  for example +3 % for +50 c, which slightly brightens upward corrections, and note changes are abrupt period
  splices. **Not measured (§8).**

### 4.9 Auto-Tune EFX+ and Auto-Key (minor in her corpus)
- **EFX+** (17 slots / 6 states): a different state format, `<AUTOTUNEEFXPLUS><PARAM id="p…" value=…>`. It contains
  pKey, pScale, pRetuneSpeed (20 ms in 5 states, 221.6 ms in 1), pHumanize 0, pTracking 5, pFormant 1,
  pThroatLength, pPitchShift (−12 in one state), pNoteA…pNoteG (scale note switches), pInputType, pLowLatency 0, plus
  a multi-FX chain: pEffect1…4 module slots, Duet (detune/timing), Tube (drive/compression), Chorus, Echo, Reverb,
  Mutate (ring mod), Vocode, Breathy, Compress, Hi/Lo-pass, EQ, and Auto-Motion patterns (pPatternID/Play)
  [DATA, `tmp/vm124/st/Auto-Tune_EFX__*.bin`].
  - The SOS review describes the modules as Pitch & Throat, Duet, Vocode, Tube Amp, Mutate and HPF, with up to 4
    modules per patch and Auto-Motion arpeggiated pitch patterns with Hard / Soft / Legato correction [S8][MEAS —
    third-party review].
  - **LYNX gap**: the substitute table names EFX+ under `[antares-auto-tune]` with `translate = auto-tune-pro`
    ([S20]). That translator looks for `ParamsData` / `ScaleGivenKey`, which EFX+ states don't have, so EFX+ slots get
    the gentle default. A dedicated `auto-tune-efx` translator could read pKey / pScale / pRetuneSpeed / pHumanize /
    pNote* / pPitchShift. The FX modules have no LYNX equivalent in LYNX Tune. The pScale numbering is not verified.
- **Auto-Key** (52 slots / 4 states): a key/scale **analyser** that sends Key/Scale to Auto-Tune instances. It is
  meant for polyphonic material (instrument buses), not for the vocal [S5 pp.3–4][DOC]. Its state stores
  `<Parameters><MasterBypass/><KeyScale/>` plus `LatestKey`, `LatestKeyType` and `LatestPitchReference` (detected
  key and reference pitch, e.g. key 11, type 1, 439.81 Hz) [DATA, `tmp/vm124/st/Auto-Key_*.bin`]. The manual
  describes no audio processing. Treat it as pass-through [model]. **Use for LYNX:** its `LatestKey/LatestKeyType` can
  vote for the project key (`substituteContextOf`), and `LatestPitchReference` could set LYNX Tune's A4 reference
  when a project's Auto-Tune has Detune 0. Unverified idea; the type numbering (1 = minor?) needs checking.

## 5. Published science

### 5.1 Hildebrand's patent (US 5,973,252, priority 1997-10-27, expired) [S6][PATENT]
- **Detection.** For a candidate period L the patent keeps two running sums over the latest samples:
  `E_i(L) = E_{i−1}(L) + x_i² − x_{i−2L}²` (energy over 2L samples) and
  `H_i(L) = H_{i−1}(L) + x_i·x_{i−L} − x_{i−L}·x_{i−2L}` (lag-L cross product over L samples).
  L is a period when **`E_i(L) − 2H_i(L) ≤ ε·E_i(L)`**, with ε ∈ [0, 0.40] set by the user (the "Tracking" ancestor).
  - Identity [model, algebra]: `E − 2H = Σ_{j=i−L+1..i} (x_j − x_{j−L})²`. This is the squared-difference function
    of YIN (§5.2), normalised by energy instead of by YIN's cumulative mean.
  - Detection mode: anti-alias filter, 8:1 decimation, search L = 2 … 110 decimated samples (≈ 50 Hz – 2.76 kHz at
    44.1 kHz). A missing-fundamental check compares 8·L_min1 against 8·L_min2 at full rate.
  - Tracking mode: E/H at full rate only over a window of 8 lags around the current estimate, updated every 5th
    iteration. Sub-sample period by a quadratic fit through the minimum. Fall back to detection when the match
    fails, the pitch moves too fast, or the energy is too low.
- **Correction.** Target period = the closest scale note, or MIDI note + bend. Raw ratio = `Cycle_period /
  desired_Cycle_period`, smoothed by `Decay` into `Resample_Rate1`. An optional vibrato coefficient (depth, rate,
  onset delay) modulates the ratio.
- **Shifting.** An output pointer advances through a circular input buffer at the resample rate and interpolates at
  (address − 5). When it overtakes the input pointer one period is subtracted (a cycle repeats). When it falls far
  behind one period is added (a cycle is dropped). This is pitch-synchronous resampling **without formant
  preservation**.

### 5.2 YIN (de Cheveigné & Kawahara 2002) [S9][PAPER]
`d_t(τ) = Σ_{j=t+1..t+W} (x_j − x_{j+τ})²` ;
`d'_t(τ) = 1` if τ = 0, else `d_t(τ) / [(1/τ) Σ_{j=1..τ} d_t(j)]` ;
pick the first τ with `d'_t(τ)` below an absolute threshold (0.1 – 0.15 typical), then refine by parabolic
interpolation. LYNX Tune uses this on an ~11 kHz decimated copy with full-rate refinement (`VocalPitch.h`).

### 5.3 TD-PSOLA (Moulines & Charpentier 1990) [S10][PAPER]
Analysis marks t_a,k one period P(t) apart. Two-period Hann grains `x_k(n) = h(n)·x(n + t_a,k)`. Synthesis marks
spaced P(t)/β for pitch ratio β = 2^{c/1200}. Output `y(n) = Σ_k x_k(n − t_s,k)`. Grains keep their internal
spectral envelope, so **formants are preserved by construction**, and resampling the grains moves them (formant
shift). Typical issues: phasiness on breathy segments, and mark jitter that causes roughness when pitch moves fast.

### 5.4 Phase vocoder with envelope preservation [S11][S12][PAPER]
Laroche & Dolson shift pitch directly in the frequency domain by moving spectral peak regions, with phase
correction (50 % – 75 % overlap) [S11]. Formant preservation: estimate the envelope E(ω) (cepstral "true envelope",
Röbel & Rodet [S12]) and output `|Y(ω)| = |X(ω/β)| · E(ω) / E(ω/β)`. This gives better quality for large shifts but
costs a frame of latency (a 2048-sample window at 48 kHz is about 43 ms) [model, arithmetic]. That frame cost is of
the same order as Auto-Tune's measured 55.6 ms; the cause of that latency is unknown.

### 5.5 Literature on the "Auto-Tune effect"
The vendor describes the effect as "pitch quantization": Retune 0, Flex-Tune 0, the right scale, instantaneous
transitions [S3 pp.26–27][DOC]. No peer-reviewed black-box measurement of Auto-Tune's retune law was found. Gohari
et al. detect Auto-Tuned vocals from spectrograms and state that no earlier work exists [S14][PAPER]. Wager et al.
contrast note-quantising correction with data-driven continuous correction [S15][PAPER]. Our round-120 laws are
therefore the primary quantitative reference.

## 6. What LYNX does today

- **Engine** [S18]: YIN detector (threshold 0.05 + 0.25·Tracking) → nearest allowed note with 0.03 st hysteresis →
  Flex-Tune gate (the measured law, gap-normalised) → one-pole retune with time constant τ, plus Humanize
  (+H·135 ms over 0.2 – 0.6 s of steadiness, reset by a second move 30 – 150 ms after the first) → TD-PSOLA with
  formant preserve/shift → dry/wet mix with a delay-matched dry signal. A new phrase (> 60 ms unvoiced) resets the
  applied shift to 0.
  - Reported latency = ceil(2·sr/f_min) + 4, i.e. **1204 samples at 48 kHz for Alto/Tenor** (Low latency: 1.25× →
    754).
- **Translation** [S19][S20]: key and scale from `<Custom>`. `retune = 1.36·X4 − 3 ms` (LYNX's τ). `humanize = X61`,
  `flexTune = X90`. Without a readable key: the project key, else a gentle chromatic preset (50 ms, humanize 50 %).
  Not translated: Classic (X11), Natural Vibrato (X62), Formant on/off (X70), Throat (X71), Transpose (X74), Detune
  (X3), Mix (X92), Input Type (X10), Tracking (X9), per-note Bypass/Remove masks.
  (Later rounds translate most of these; round 166c: X71 → LYNX Tune's own Throat length with the law measured in
  round 165hb, X47 / X48 / X49 → the created vibrato's Amplitude / Formant / Variation, X179 / X181 → Transpose in
  scale / Scale transpose — docs/analise/REA_rodada166c.md.)
- **Residual** on her `luisa sonsa` takes (37 s, 2260 voiced 10 ms frames), Auto-Tune with her state vs LYNX Tune:
  **median 4.5 c, mean 13.0 c, 15.1 % of frames > 20 c**. The earlier 20 ms translation gave 5.7 / 14.2 / 16.8 %, and
  no correction gives 24.1 / 26.5 / 58.1 % [MEAS][S17 §5].
- Step-response fits after the refit [MEAS][S17 §5]: R 5 / H 100 → 110/334 ms vs 104/336. R 20 / H 100 → 122/384 vs
  118/384. R 0 / H 50 → 58/168 vs 50/160. Flex-Tune fractions within 0.05.

## 7. Implementation guidance for LYNX (ordered by expected gain on her catalogue)

1. **Fix the onset path (largest remaining error source, [model]).** Auto-Tune corrects a note from silence in about
   30 ms at every Retune Speed [S17 §5]. LYNX starts each phrase at shift 0 and pulls with τ, so at her R ≈ 6 – 8 ms
   (τ ≈ 5 – 8 ms) it differs only through detector start-up. Measure LYNX's first-voiced-frame delay on the same
   mksig onset and match it to ~30 ms total.
   - When `R = 0` (9 of her states, the hard effect), LYNX sets `shift = wanted` at phrase start. Keep that.
   - Pass: onset t50 within ±5 ms of Auto-Tune at R 0, 8, 50.
2. **Detector lag on moving pitch.** Glides and fast note changes are where the 15 % of > 20 c frames live [model:
   most of the residual on sung takes falls on transitions; verify with the per-frame error vs |dp/dt|].
   - Options: (a) shorten the detector window (min period ×2 instead of a fixed f_min window) for Alto/Tenor;
     (b) align the applied shift with the PSOLA mark time, as `setMarkLag` already does, and audit off-by-one hops;
     (c) estimate dp/dt and predict the pitch half a window ahead.
   - Pass: octave-glide mean bias within ±3 c of Auto-Tune's on mksig. Error frames > 20 c on `luisa sonsa` ≤ 10 %.
3. **Latency matching (only for parallel paths).** LYNX reports its own latency and is delay-compensated, so serial
   inserts need no matching. If a project splits the vocal pre-Auto-Tune into a parallel bus, FL compensates
   2670 samples while LYNX compensates 1204: both are compensated, so nothing changes audibly.
   - Do **not** emulate 55.6 ms. Keep Low latency available.
4. **Classic mode (2 states).** Implement as `X11 = 1` → formant preserve OFF (shift by grain resampling, i.e. PSOLA
   with `formant = ratio`), Flex-Tune forced 0, Transpose 0 [S1 p.35]. Optionally use a shorter crossfade on target
   changes ("more pronounced attack and transition").
   - Calibrate only after measuring (§8 item 3).
5. **Translate the remaining cheap parameters.** X3 Detune → LYNX Reference (A4 = 440·2^{X3/1200}). X74 Transpose →
   LYNX Transpose. X92 Mix → LYNX Mix. X70 = 0 → formant preserve Off. X10 Input Type → LYNX Range (Soprano 0,
   Alto/Tenor 1, Low Male 2, Instrument/Bass 3; LYNX has no Bass range, so map to Instrument or add one).
   - X9 Tracking: map 50 → LYNX's default threshold. Her states are all 50, so no calibration is needed.
   - Per-note masks: read the active scale's `Bypass/Remove` from the legacy `<Scales>` block, which covers the one
     state with a bypassed root. LYNX needs a per-note **Bypass** state (pass uncorrected), distinct from note-off
     (Remove).
6. **Natural Vibrato (0 dB in all her states → lowest priority).** Design [model]: high-pass the sung pitch
   trajectory p(t) in cents (2nd-order, ~3 Hz) to get v(t). Add (g − 1)·v(t) to the applied shift, with g a function
   of X62 to be measured (round 120 suggests ×2 at +12 dB, so maybe g = 10^{X62/40}). Apply it even to Bypassed
   notes.
   - Pass: vibrato retention ratio at ±12 dB within 10 % of Auto-Tune on mksig's 5.5 Hz ±40 c vibrato.
7. **EFX+ translator** (§4.9): parse `<AUTOTUNEEFXPLUS>` PARAMs → key/scale/retune/humanize/pitch shift. Leave the
   FX modules out and log them.
8. **Test signals and criteria** (reuse `tmp/tune120/py/mksig.py`, `mksig2.py`, `ana.py`, `ana2.py`): step response
   (±10/30/45 c), onset from silence, ±2/±5 st jumps, a 150 ms sung slide, an octave glide, a 5.5 Hz ±40 c vibrato,
   chromatic E4–E5 at +20 c, exact +50 c notes. Then her takes through her own state.
   - Pass: step t50/t90 within ±10 %. Vibrato retention within ±5 points. Onset ±5 ms. Takes: median ≤ 3 c, > 20 c
     frames ≤ 10 %, plus an A/B listen.

## 8. Open questions to measure (official trial, black-box only)

1. **Corpus discrepancy**: why does `inventory_all.txt` (727 slots) have no ScaleGivenScale = 2 while `tmp/gf` holds
   43 chromatic states? Re-run both scripts on the same root.
2. **Onset delay vs Input Type / Tracking**: from-silence onsets on Alto/Tenor vs Soprano vs Low Male. Decides the
   LYNX detector start-up target.
3. **Classic mode**: same mksig at R 0 / 8, Classic on vs off. Compare the output spectral envelope on ±50 c
   corrections (formant shift?), the transition shape on +2 st jumps, and the latency. Decides how to implement
   item 7.4.
4. **Natural Vibrato law**: retention at −12, −6, +6, +12 dB on a 5.5 Hz ±20 c vibrato, with every note Bypassed
   (pure vibrato processing) and at R 8. Also check whether slow drift (0.5 Hz) is affected. Decides g(X62) and the
   high-pass corner.
5. **Latency**: report at 44.1 / 48 / 96 kHz, Low Latency on/off, Input Type variants, realtime vs offline; and what
   X91 "Latency Removal" does (it may be a Graph-mode/ARA alignment flag).
6. **Glide bias**: octave glide at R 0 / 8 / 20 in E minor and chromatic. Measure the correction bias vs glide speed.
   Decides detector-lag compensation.
7. **Humanize on her real notes**: notes with natural micro-drift (±3 c) at R 8 / H 100. Does Auto-Tune's
   "sustained" threshold (≤ 4 c in LYNX) match?
8. **Ignore Vibrato Off flips**: a wide vibrato (±60 c) centred 20 c off a semitone. Count target flips per second
   vs LYNX's 0.03 st hysteresis.
9. **Stereo**: stereo-in behaviour (L/R Ref, is the shift applied identically to both channels?).
10. **Vibrato retention of LYNX vs Auto-Tune at R 5 / 8** (her range) on mksig's ±40 c 5.5 Hz vibrato. Auto-Tune
    keeps 20 % / 35 %, more than a pure one-pole would (13 % / 26 %). If LYNX follows the one-pole, add the missing
    detector smoothing.
11. **EFX+**: verify the pScale numbering and the effect of pTracking = 5 (its range differs from Pro).

## 9. Sources

1. Antares, *Auto-Tune Pro 11 User Guide* (PDF) — https://antares-web-frontend.sfo3.cdn.digitaloceanspaces.com/documentation/pdfs/Auto-Tune_Pro_11_User_Guide.pdf — accessed 2026-10-21 — [DOC]
2. Antares, *Auto-Tune Pro User Guide* (© 2018, updated 3/18) — https://antares-web-frontend.sfo3.cdn.digitaloceanspaces.com/documentation/pdfs/Auto-Tune_Pro_Manual.pdf — accessed 2026-10-21 — [DOC]
3. Antares / Universal Audio, *Auto-Tune Realtime Advanced Manual* (tutorials, scale appendix) — https://media.uaudio.com/support/manuals/dd/Auto-Tune%20Realtime%20Advanced%20Manual.pdf — accessed 2026-10-21 — [DOC]
4. Antares / Universal Audio, *Auto-Tune Realtime X User Guide* — https://media.uaudio.com/support/manuals/dd/Auto-Tune+Realtime+X+Manual.pdf — accessed 2026-10-21 — [DOC]
5. Antares, *Auto-Key User Guide* — https://antares-web-frontend.sfo3.cdn.digitaloceanspaces.com/documentation/pdfs/Auto-Key_User_Guide.pdf — accessed 2026-10-21 — [DOC]
6. H. A. Hildebrand, US 5,973,252 "Pitch detection and intonation correction apparatus and method" (filed 1998-10-15, priority 1997-10-27; Auburn Audio Technologies → Antares; expired) — https://patents.google.com/patent/US5973252A/en — accessed 2026-10-21 — [PATENT]
7. EP 1 027 697 (European counterpart, Hildebrand / Auburn Audio Technologies) — https://data.epo.org/publication-server/rest/v1.2/patents/EP1027697NWA1/document.html — accessed 2026-10-21 — [PATENT]
8. Sound On Sound, review "Antares Auto-Tune EFX+" — https://www.soundonsound.com/reviews/antares-auto-tune-efx — accessed 2026-10-21 — [MEAS — third-party review]
9. A. de Cheveigné, H. Kawahara, "YIN, a fundamental frequency estimator for speech and music", JASA 111(4):1917–1930, 2002 — https://cagnazzo.wp.imt.fr/files/2013/05/2002_JASA_YIN.pdf — accessed 2026-10-21 — [PAPER]
10. E. Moulines, F. Charpentier, "Pitch-synchronous waveform processing techniques for text-to-speech synthesis using diphones", Speech Communication 9(5–6):453–467, 1990 — https://doi.org/10.1016/0167-6393(90)90021-Z — accessed 2026-10-21 — [PAPER]
11. J. Laroche, M. Dolson, "New phase-vocoder techniques for pitch-shifting, harmonizing and other exotic effects", IEEE WASPAA 1999 (JAES 47(11):928–936, 1999) — https://www.ee.columbia.edu/~dpwe/papers/LaroD99-pvoc.pdf — accessed 2026-10-21 — [PAPER]
12. A. Röbel, X. Rodet, "Efficient spectral envelope estimation and its application to pitch shifting and envelope preservation", DAFx 2005 — https://dafx.de/paper-archive/details/EDSTSaO17Gd_0LV03iwDfw — accessed 2026-10-21 — [PAPER]
13. U. Zölzer (ed.), *DAFX: Digital Audio Effects*, 2nd ed., Wiley 2011 (ch. on pitch shifting / source-filter processing) — https://dafx.de/DAFX_Book_Page/chapter9.html — accessed 2026-10-21 — [PAPER]
14. M. Gohari, P. Bestagini, S. Benini, N. Adami, "Spectrogram-based detection of Auto-Tuned vocals in music recordings", arXiv:2403.05380, 2024 — https://arxiv.org/abs/2403.05380 — accessed 2026-10-21 — [PAPER]
15. S. Wager, L. Guo, A. Sivaraman, M. Kim, "A data-driven approach to smooth pitch correction for singing voice in pop music", arXiv:1805.02603, 2018 — https://arxiv.org/abs/1805.02603 — accessed 2026-10-21 — [PAPER]
16. Antares Help Center, "Low Latency Mode Not Working" — https://help.antarestech.com/hc/en-us/articles/47201781644948-Low-Latency-Mode-Not-Working — accessed 2026-10-21 — [DOC]
17. LYNX REA round 120 (Auto-Tune Pro and Waves Tune RT measured; X<n> map; laws; LYNX refit) — [../analise/REA_rodada120.md](../analise/REA_rodada120.md) §3, §5, §6; raw: `~/.claude/jobs/f8789b49/tmp/tune120/` (py/, at/), `C:\Users\kauan\lynx-test\tune120\at_params.tsv`, `at_rs.tsv` — [MEAS]/[DATA]
18. LYNX source: [../../src/core/LynxTune.cpp](../../src/core/LynxTune.cpp), `src/core/VocalPitch.h` — repo
19. LYNX REA round 113 §2 (state layout `VC2!` / `<SessionChunkData>`, key/scale numbering, pitch-class fits) — [../analise/REA_rodada113.md](../analise/REA_rodada113.md) — [DATA]/[MEAS]
20. LYNX substitute table and translator: [../PLUGIN_FALLBACK.md](../PLUGIN_FALLBACK.md), `src/core/PluginSubstitutes.cpp` (`[antares-auto-tune]`, `translateAutoTunePro`, `autoTuneProParams`) — repo
21. Her EFX+ / Auto-Key states: `~/.claude/jobs/f8789b49/tmp/vm124/st/Auto-Tune_EFX__*.bin`, `Auto-Key_*.bin`; inventory `~/.claude/jobs/f8789b49/tmp/r113/inventory_all.txt` — [DATA]
