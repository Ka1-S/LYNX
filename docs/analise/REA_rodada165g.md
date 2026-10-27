# REA round 165g — the FL plugins LYNX still lacked, and engines out of "(beta)"

Round r165g "flplugins165" (parent). Evidence markers: [MANUAL] FL's online manual, [DECOMP addr] Ghidra on the
installation's DLL (clean-room: behaviour only), [FL render] FL64.exe /R of a crafted project, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-27 resume after the BSOD: Ghidra projects of every missing plugin's DLL are on D:\lynx-work\flplugins165\ghidra
  (BigClock, Mute2, Scratcher(+engine), Tuner(+engine), Transmitter, VideoPlayer, HtmlNoteBook). Sub-rounds relaunched:
  flbeta165p (Granulizer, Newtime, Newtone), flbeta165s (Harmor); flbeta165k (Kepler) waits for a free slot.
- Now: Fruity Mute 2 decoded; FL renders of crafted Mute 2 states running; next: Big Clock, Tuner, HTML NoteBook,
  inert imports (Razer Chroma, ReWired, FL Studio Mobile), then Scratcher, Transmitter, Video Player.

## Plugins (FL 2026, Plugins\Fruity)
| FL plugin | kind | LYNX | evidence |
|---|---|---|---|
| Fruity Mute 2 | effect | (in progress) | [DECOMP], [MANUAL] |
| Fruity Big Clock | effect (no sound) | (in progress) | [MANUAL] |
| Tuner | effect (no sound) | (in progress) | [MANUAL] |
| Fruity HTML NoteBook | effect (no sound) | (in progress) | [MANUAL] |
| Fruity Scratcher | effect (turntable) | (todo) | [MANUAL] |
| Transmitter | effect (transient / sustain split) | (todo) | [MANUAL], presets |
| Fruity Video Player | generator | (todo) | [MANUAL] |
| FL Studio Mobile / Mobile Rack FX | generator / effect | (todo: import note) | |
| Razer Chroma, ReWired | | inert on import, with a note | |

## 1. Fruity Mute 2
[DECOMP 006ba850] two parameters: 0 "Mute" — muted = value < 0.5 (labels On / Off); 1 "Channel" — round(value / (1/3)),
clamped to 2: 0 = L, 1 = L + R, 2 = R. [DECOMP 006baaf0] process: not muted → copy; L → left × 0, right × 1;
L + R → both zeroed; R → left × 1, right × 0. No ramp. [DECOMP 006ba2a0] state: int32 version (1), then the two
controls' raw values (int32 each).
