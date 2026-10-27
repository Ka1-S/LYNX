# REA round 165f (rea165): FL Studio 2026's technologies and touches → what LYNX still lacks

2026-10-27. Brief: walk FL Studio 2026 like a power user, use REA/Ghidra on `FLEngine_x64.dll` where behaviour is
not visible, compare with LYNX, rank the gaps (user value, effort, evidence) and build the top ones that rounds
165a–e/g/h/i do not cover. User: "tem que ter todas as tecnologias do FL" — so §1 is a full inventory of FL's
technologies (engine, sync, recording, editing, file, UI), each marked against LYNX.

Evidence tags: **[DFM]** FL's own form/menu resources read from `FLEngine_x64.dll` (our `dfm.py` reader of Delphi
TPF0 resources; 105 forms, 1 397 menu items in the main form alone — no GUI needed), **[DECOMP]** Ghidra
decompilation of `FLEngine_x64.dll` (headless, the analysed project copied to `D:\lynx-work\rea165`), **[SHOT]**
screenshots of FL and LYNX, **[MANUAL]** FL's manual / WhatsNew, **[CODE]** LYNX source.
Clean-room: behaviour and names only, no Image-Line code, art or data copied.

## Where I stopped
- 16:10 — resumed after the BSOD (the first agent had written nothing). DFM inventory extracted
  (`tmp/rea165/menus.txt`, `forms.txt`); first comparison against LYNX's code done (§1). Next: rank (§2), REA the
  marker actions and MIDI sync, start the builds.
