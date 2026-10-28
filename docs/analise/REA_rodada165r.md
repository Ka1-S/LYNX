# Round 165r crash13: GitHub issue #13 (caiorobertcr7, 0.1.17, Windows 11, ASIO4ALL 64 samples)

## Where I stopped
- 2026-10-28: worktree ../lynx-wt-crash13 on feature/crash13 (from origin/main 3d88f2b2). Crash symbolized (below).
  Crash already fixed on main by r165p (c475852e); next: Windows repro on 0.1.17 vs main with FL Studio ASIO, then the
  2-8 s freezes.

## Crash (LYNX.exe 0.1.17 + LYNX.pdb from /mnt/d/lynx-work/crash-reports/symbols/0.1.17, `LYNX.exe --symbolize`)
```
exception 0xc0000005 at LYNX.exe+0x1071f65  lambda at src\ui\SettingsPages.cpp:394:31 (AudioPage panelButton.onClick) +0x45
1: juce::Button::sendClickMessage + 0x391
2: juce::Button::internalClickCallback + 0x86
3: juce::Button::mouseUp + 0x11f
4: juce::Component::internalMouseUp + 0x232
5: juce::detail::MouseInputSourceImpl::setButtons + 0x11d
6: juce::detail::MouseInputSourceImpl::handleEvent + 0x135
7: juce::ComponentPeer::handleMouseEvent + 0xc5
8: juce::HWNDComponentPeer::doMouseUp + 0x19c
9: juce::HWNDComponentPeer::peerWindowProc + 0x130
10: juce::HWNDComponentPeer::windowProc + 0x6d
13: juce::InternalMessageQueue::dispatchNextMessage + 0xf9
14: juce::JUCEApplicationBase::main + 0xe4
15: main + 0xd9
```
Root cause: Settings › Audio "Control panel" button (0.1.17): `devices.closeAudioDevice()` (deletes the device) and
then `device->showControlPanel()` on the deleted object. Same bug as issue #12's report d1f26cca (0.1.18, LYNX ASIO);
fixed on main in c475852e (panel first, reopen only when the driver says it changed — JUCE's order).
