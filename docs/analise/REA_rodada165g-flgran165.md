# REA round 165g — flgran165: Fruity Granulizer's sound engine (slices, transient hold, sync, key modes 2/3)

Sub-round of r165g flplugins165, continuing docs/analise/REA_rodada165g-flbeta165p.md (FL's auto slicer, WAV
markers / ACID beats). Goal: finish the engine and drop "(beta)" only if LYNX matches FL.

Evidence markers: [DECOMP addr], [DATA], [FL render], [EXEC FL] (the installation's DLL run unmodified in our harness,
tmp/r165g/gx.exe), [TEST], [SHOT], [RUN], [HYP].

## Where I stopped

- 2026-10-28 05:10 — started. Decompiled Voice_Render 0x753ea0, update 0x752290, sync 0x7524e0/0x752490/0x752520,
  Dispatcher 0x13 (0x7517f8), TriggerVoice 0x753580, slice set-up 0x7548c0 (Ghidra project ~/ghproj/r67_gr, DLL MD5
  083484d2…, same as the installation's). FL renders g11…g16 queued (tmp/flgran165/py/mk2.py).
