# REA round 165f-mixer (rea165-mixer): FL's mixer latency, render-tracks and submix touches

2026-10-28. Sub-round of r165f (docs/analise/REA_rodada165f.md §1.1, §1.3, §1.6, §2 gaps #7 and #8). Clean-room:
behaviour and names from FL's forms ([DFM], tmp/rea165/dfm/TFXForm.dfm.txt) and a headless Ghidra decompilation of
`FLEngine_x64.dll` ([DECOMP], own copy of the analysed project in `D:\lynx-work\rea165-mixer`); no Image-Line code,
art or data copied.

## 1. FL's per-track latency [DFM] [DECOMP]

### 1.1 The menu (`DelayMenu`, the "Track latency compensation" label of the track properties panel)
Reset · Set in ms · Set in samples · Set in beats · Set from › (other tracks) · Ping... · Compensate for master
latency · Bypass track latency compensation: Never / Always / When recording or not playing. Mixer menu › Plugin delay
compensation › Reset manual latency on all tracks (a warning "This will reset manual latency values on all mixer tracks
/ This can not be undone.").

### 1.2 The data (per mixer track, FL's track record of 0x1478 bytes)
| field | meaning | default | .flp |
|---|---|---|---|
| +0x1CC float | manual track latency in ms, clamped to −5000…+5000 (`FUN_012a7230`) | 0 | 0xEC payload bytes 0–3 |
| +0x1C8 float | manual input latency (the input button's own menu: "Latency", "Adjust position of recorded audio") | 0 | 0xEC bytes 8–11 |
| +0x1D1 byte | Compensate for master latency (only with a hardware output) | 1 (`FUN_0127b780`) | 0xEC flags bit 0x4000 = **off** |
| +0x1D0 byte | Adjust position of recorded audio (input) | 1 | 0xEC flags bit 0x8000 = off |
| +0x1472 byte | Bypass track latency compensation 0 Never / 1 Always / 2 When recording or not playing | 0 | event 0x31 (byte) after 0xA5/0xA6 |

FL's writer is `FUN_00f3d620` (the same 0xEC LYNX already reads the insert flags from), the reader `FUN_00f46490`.
Survey of the 1332 .flp files on this PC: 10 files carry a manual latency (FL's demo "SeamlessR - Bass Antics
Performance" insert 7: −3.49 ms; the girlfriend's "twin swag" insert 18: −20 ms, plus its autosaves), none an input
latency, none a bypass mode other than Never, none with master compensation off.

### 1.3 The planner (`FUN_010ff3b0` latencies, `FUN_010ffca0` delay lines)
Per track T, in samples (ms × rate / 1000, rounded):
- own input = max(latency of the generators on T, the input latency);
- inAlign[T] = max(own input, outLat of every track routed into T);
- outLat[T] = inAlign[T] + Σ effect latencies + **manual latency** (so the manual value is "latency this track
  has": positive = the other paths wait for it — the track plays earlier; negative = the track is delayed);
- each route T → D gets a delay of inAlign[D] − outLat[T] when > 0 (the route to the Master: total − outLat[T]);
- `FUN_012c19f0` (bypass now?): Never → no; Always → yes; When recording or not playing → yes while the transport
  is stopped or recording. A bypassed track's routes get **no delay line** — but its outLat still counts in the
  destination's inAlign (the others still wait for its plugins);
- the hardware output delay (total − outLat) needs: not bypassed, a hardware output, Compensate for master latency on.
- Ping... (`FUN_012ae830`) is an empty function in FL 2026 — not built.
- Set from › lists the other tracks whose latency (ms) ≠ 0 (hint "|<x>ms"); picking one sets this track's manual
  latency so its total equals the other's (`FUN_012acd30`: manual += lat(other) − lat(this); a negative result warns
  "The delay introduced by the current mixer track is higher than the delay introduced by the source mixer track").
- The label (`FUN_0128b850`): "(none)" or "<x>ms / <n> smp"; hint "^Track latency" with "<x>ms (auto)" and
  "<x>ms (manual)".

## 2. Routing: selected tracks ⇄ this track [DFM] [DECOMP]
`RouteSelToThis1MenuClick` (`FUN_012ac520`, tag): for every selected track but this one — tag 1/3 ("only") and 4
clear all its routes first; route level unity (tags 0, 1, 4) or 0 (sidechain: tags 2, 3); the route is enabled.
Tag 4 **Create submix...**: then the rename dialog of *this* track; if it was named, it becomes "<name> master"; the
selected tracks take this track's colour and the unnamed ones its name; when the selected tracks are contiguous in one
dock, separators make them (plus this track when adjacent) a group. `RouteThisToSel1MenuClick` (`FUN_012ac950`): this
track → every selected one (tags 0–3 the same way).

## 3. Disk recording › Render armed / selected tracks to wave file(s) [DFM] [DECOMP]
`FUN_012a7a80` opens the render window in its mixer-tracks mode; `FUN_012a7d80` (Render selected) arms exactly the
selected tracks for the render and restores every track's arm state afterwards.

## 4. Record source point [DFM] [DECOMP]
`FUN_012aada0`: track byte +0x2F = 0 External input only, 1 External and mixer input, 2 Post effects, 3 Post EQ, 4 Post
level and panning, 5 Post track (.flp 0xA5); "External input only" without an input warns "won't record anything unless
you select an input!".

## Where I stopped
- REA done for §1–§4. Building §1 (Song fields, PDC planner, renderer bypass, .flp import, .lynx, menu).
