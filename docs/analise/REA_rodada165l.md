# REA round 165l (edison165): LYNX's audio editor gets FL Edison's tools

Goal: every Edison tool form (`TMEnvEditor` + `TME*ToolForm`, FL Studio 2026) in LYNX's audio editor
(`src/ui/AudioEditor.cpp`), studied with REA (clean-room: FL's forms and decompiled behaviour are read to learn what
each tool does and with which parameters; the DSP, dialogs and drawing are LYNX's own), FL-like dialogs in LYNX style,
undoable, previewable, computed off the audio thread; nulls vs FL where the tool is deterministic.

## Where I stopped

- 2026-10-27 16:40: worktree `../lynx-wt-edison165` on `feature/edison165` from main 5ed718d1; inventory of the
  tool forms from r165f's DFM dump (`tmp/rea165/dfm/TME*.dfm.txt`) started.

## 1. Inventory: Edison tool forms vs LYNX (before this round)

(filled below)
