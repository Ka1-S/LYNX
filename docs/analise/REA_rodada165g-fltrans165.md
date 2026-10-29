# REA round 165g, sub-round fltrans165 — Transmitter

Evidence markers: [DECOMP addr] Ghidra on the installation's `Plugins\Fruity\Effects\Transmitter\Transmitter_x64.dll`
(the Delphi front end) and `Shared\ildsp_x64.dll` (the engine; SHA-256 `b96354e1…1760`, the same file rounds 54/60/154
analysed), clean-room: behaviour only; [EXEC] a small Windows harness that loads `ildsp_x64.dll` and drives its
`Transmitter_CreateInstance` engine directly (goldens), [FL render] FL64.exe /R, [TEST], [SHOT], [HYP].

## Where I stopped
- 2026-10-29 07:15: front end and engine mapped (§1–§3); EXEC harness runs FL's engine on Windows. Next: the Faust
  modules' DSP (detector, enveloper, tilt, saturation, bounce, diffuser, delay), LYNX's port, compare with the harness.
