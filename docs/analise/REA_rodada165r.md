# Round 165r crash13: GitHub issue #13 (caiorobertcr7, 0.1.17, Windows 11, ASIO4ALL 64 samples)

## Where I stopped
- 2026-10-28 19:40: fixes committed (driver panel helper + selftest "driver_panel", SerialJobs for audio-device.xml and
  the glitch log, core tests tests/Crash13Tests.inc). 20:30 Linux froze (disk full, zig caches); rebuilding from scratch (ZIG cache ~/.cache/zig-crash13*). Big data now on /mnt/e/lynx-work/crash13 (0.1.17 exe+pdb, symbolized crash).
  Next: Windows check of the build (panel buffer kept, no hang file from saveAudioDeviceState), land, notes, reply.

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

## Windows reproduction on main (land-flbeta165h build = origin/main cce0794b, LYNX.pdb from ~/projetos/lynx/build-win)
Fresh LYNX_PROFILE_DIR + LYNX_TEST_MODE=1, FL Studio ASIO (ASIO4ALL is not installed here; FL ASIO is the same kind of
WDM wrapper), smallest buffer offered (256). Hang files of that session, symbolized with --symbolize:
| stall | message-thread stack | cause |
|---|---|---|
| 2.1 s | ASIOAudioIODevice::openDevice ← createDevice ← setCurrentAudioDeviceType ← Settings › Audio driver box | driver init (inside ilwasapi2asio_x64.dll) |
| 2.1 s | WASAPIDeviceBase::tryFormat ← createDevice ← setCurrentAudioDeviceType | Windows Audio device probe |
| 2.2 s | **FileOutputStream::flush ← XmlElement::writeTo ← AudioEngine::saveAudioDeviceState ← ChangeBroadcaster** | LYNX: audio-device.xml saved on the message thread after every device change; JUCE ends every write with FlushFileBuffers |
| 2.1 s | ASIOAudioIODevice::open (Thread::sleep loop) ← setCurrentAudioDeviceType | JUCE waits up to 3 s for the driver's first callback |
So every device change in Settings › Audio freezes the UI for 2+ s: the driver open (JUCE / driver, also in FL) and,
on top of it, LYNX's own save of audio-device.xml with a disk flush. Same code in 0.1.17. The user's log (seven 2-8 s
stalls in 8 minutes, no project / plugin steps in between, then the crash on "Driver settings…") matches someone
trying ASIO4ALL settings in Settings › Audio.

Second finding while reproducing: after the driver's own panel, LYNX reopened the device with ITS previous buffer
(272), so the buffer picked in the FL ASIO panel (384) was silently ignored (shot s12). ASIO4ALL users set 64 samples
exactly there.

## Fixes
1. `AudioEngine::showDriverPanel` (Settings › Audio "Driver settings…"): panel first; when the driver says it changed,
   close (the old device is never touched again) and reopen with buffer size 0 = the new device's preferred size, i.e.
   the panel's buffer. Regression: `--selftest` line `driver_panel=OK` (fake driver whose panel changes the preferred
   size: no call on a deleted device, one device alive, buffer = the panel's, an unchanged panel keeps the device).
2. `SerialJobs` (src/core/SerialJobs.h): one background thread, ordered jobs, drained on destruction. audio-device.xml
   and audio-glitches.log (appended every 2 s from the 30 Hz timer while glitching: likely at 64 samples) are written
   there. restoreAudioDeviceState waits for the queue. Core tests: tests/Crash13Tests.inc.
Not changed: the driver open itself stays on the message thread (ASIO drivers are COM objects tied to that thread;
FL Studio blocks the same way while it opens a device).
