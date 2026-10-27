# Round 164d — Hardware Insert (External FX) (extfx164)

Goal (spec: REA_rodada163c §4): an effect that sends an insert's signal out to chosen hardware outputs and reads it
back from hardware inputs inside the mixer, with its round trip (measured by a ping) reported to PDC, dry/wet,
send/return gain, phase invert, a ping button, saved in the project, an export that warns, LYNX UI + pt-BR, and an
automated loopback check that the signal through the insert lines up sample-accurately with the dry path.

Evidence markers: [CODE] read in LYNX's sources, [TEST] unit test, [RUN] measured with the app / CLI, [DOC] vendor
manuals / common knowledge (Pro Tools H/W Insert, Nuendo/Cubase External FX). FL Studio has no such plugin (FL users
do it by hand with Fruity Send + mixer I/O), so there is no FL behaviour or .flp state to import.

## 1. What it does

`EffectKind::hardwareInsert` ("Hardware Insert", core/HardwareInsert.h, header-only), in every mixer slot list
(Mixer › slot › add, Browser › Effects › LYNX, Quick add palette). Parameters (all saved with the project like any
effect, automatable):

| # | Name | Meaning |
|---|---|---|
| 0 | Send | send gain ±24 dB |
| 1 | Return | return gain ±24 dB |
| 2 | Dry/wet | 0 = dry only, 1 = return only (default 1: a plain insert) |
| 3 | Invert | return phase invert |
| 4 | Output | device output channel (`(ch+1)/128`, 0 = none) |
| 5 | Output mono | 0 = stereo pair, 1 = (L+R)/2 on one output |
| 6 | Input | device input channel (same encoding) |
| 7 | Input stereo | 1 = stereo pair, 0 = one channel to both sides |
| 8 | Round trip | the delay in samples / 65536 (set by the ping, editable, 0 … 65535) |
| 9 | Ping | momentary: any change of the value starts a ping |

Nothing is routed in a new slot (no sound reaches a device output before the user picks one).

### Timing [CODE][TEST]
- In each callback piece the effect adds its (send-gain scaled) input into the device-output buses of r156
  (`HardwareOutput::bus`, at the chunk's offset in the piece) and reads its return from the device input of the same
  piece (`LiveInput::channel`). What leaves on sample t comes back on t + R (R = the round trip of that exact path).
- The effect reports R as its latency (`latencySamples()`, known from the slot before the first block, so PDC plans
  it at once) and delays its own dry signal by R in a preallocated 64 k-sample line; out = dry·(1−mix) + return·mix.
  Every other path is delayed by R by PDC, so the insert's output lines up with the rest of the mix.
- The slot's own mix knob (Mixer) scales the effect's Dry/wet instead of mixing an undelayed dry outside it
  (RackRenderer: `partial` is off for this kind), so lowering it never combs.
- Inserts with a hardware insert are pinned to the audio thread (the output buses are not shared between workers);
  smart disable never skips it (outboard tails / noise).
- Offline (an export, no device): the return is replaced by the dry signal, still delayed by R, so the export keeps
  every track aligned. Routed but a channel not open: the return is silent and the editor says which channel.

### Ping [CODE][TEST]
`takes::LatencyPing` (r163c) run inside the effect on its own send → return path: 8 clicks a quarter second apart
replace the send (the music keeps playing on every other track; the insert passes its delayed dry meanwhile), the
return is listened to. Result: median; refused when clicks are missing / no signal / spread > 4 samples. The editor
applies a good result to "Round trip" once (undoable) and shows "Round trip: N samples".

### UI [CODE]
LYNX's own panel (`plugui` own layout, 784 × 236): SEND (Output menu = the device's pairs then single channels,
"(the Master plays here)" on the Master's pair; Mono; Send knob) · RETURN (Input menu, Stereo pair, Return knob,
Invert) · LEVELS (send / return peak meters) · ROUND TRIP (delay in samples and ms, the state: pinging / failed and
why / not measured / channel not open / no device; the Ping button; the delay stepper) · MIX (Dry/wet). Choosing a
channel opens it on the device (`AudioEngine::openHardwareInsertIo`: outputs like r156's targets, the input like an
insert's input source) — also when a project loads. pt-BR strings for all new text.

Not offered inside a Patcher (its device I/O and latency live in the mixer chain).

### Export [CODE]
Export window: "Hardware inserts pass their dry signal in an export (record their return to keep it)" when the song
has an active one. (Real-time export through the hardware: not done, see Where I stopped.)

## 2. Tests
- [TEST] `hw164_*` (tests/HwInsert164Tests.inc): parameters / encodings / texts; through a simulated cable at its
  round trip the return equals the dry signal delayed by the reported latency bit for bit for block/chunk sizes
  512/512, 128/64, 480/128, 64/17 (100 % wet, 50 %, and 50 % inverted = silence); send/return gains and mono
  routing; offline = dry at the latency; the ping finds 371 / 1536 / 64 samples with music under it, and fails with
  "no signal" without a cable.
- [RUN] `LYNX --hwinserttest report.txt [seconds] [--type T] [--device D] [--input I] [--buffer N]`: see §3.

## 3. Measurements
(filled below)

## Where I stopped
(filled below)
