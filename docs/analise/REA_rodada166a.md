# Round 166a: issue #15 (audio clips change insert) and the three bugs of issue #14

v0.1.21. Per item: reporter's words | FL 2026 evidence | LYNX before | LYNX now.

## Where I stopped

- #15 core + UI done (only placed clips follow an audio track, "Make clip unique?" question); FL black-box checks of
  take recording pending (the user was using the PC; FL closed again at once).
- #14 B: lit keys published by the renderer, painted by the Piano roll; FL look pending.
- #14 A, C: investigation.

## 1. Issue #15 (esiowry, Windows 11, 0.1.19)

> I record multiple audio clips to mixer insert 14 … I move any one of the audio clips into a different track. The
> audio inexplicably becomes assigned to mixer insert 10 … Moving the audio clips in the playlist to another track
> consistently reassigns them to mixer insert 10 again.

### FL 2026 [DECOMP FLEngine_x64.dll build 5654, Ghidra project ~/ghproj/r165i]

- `FUN_00e66ba0(playlist, op, force)`: after a Playlist edit, walks the edit's list of *touched clip indices*
  (op+0x3e0, filled by the move / paste / drop that just happened; cleared at the end) and calls `FUN_00e66580` on each.
  Clips the edit did not place are never looked at.
- `FUN_00e66580(…, clip, out)`: when the clip's channel is an audio clip channel (type 4) and the clip's track is an
  audio track linked to mixer track M (`FUN_012e81d0(track) > 0`):
  - it scans every playlist (every arrangement) for another clip of the same channel whose track is an audio track
    linked to another mixer track (> 0 and ≠ M). Clips on unassigned / instrument tracks don't count.
  - such a clip exists → message box "Make clip unique?|The clip "%s" is also used on another audio track. Do you want
    to make this instance unique and then route it to this audio track?" (Yes/No). Yes: the channel is cloned
    (`FUN_01004230`), the clone routed to M, the touched clips of that channel get the clone, undo name "make clip
    unique". No: nothing changes for them.
  - otherwise the channel's mixer track is set to M (`FUN_01046a70(channel mixer, M, 0x11)`): every clip of the
    channel follows.
- Files dropped on an audio track (`FUN_012f2a10`): the new channels are routed to its mixer track; on an instrument
  track FL asks "Can't drop on instrument tracks | Instrument tracks are not for audio clips. Would you like to place
  clip(s) on the next empty playlist track(s)?".

### LYNX before

`trackmodes::followAudioTracks (song)` ran after EVERY Playlist edit (`PlaylistComponent::sortClips`: moves, resizes,
knife, quantize, merge, …) over EVERY audio clip of the song: any clip sitting on an audio track had its channel forced
back to that track's insert, and a channel also used on any other track (even an unassigned one) was silently copied.
So routing a clip back to insert 14 by hand while it sat on an audio track linked to insert 10 lasted only until the
next edit anywhere in the Playlist — exactly "consistently reassigns them to mixer insert 10 again".

### LYNX now

- `trackmodes::followAudioTracks (song, placed, makeUnique)`: only the clips the edit placed (moved, cloned, painted,
  pasted, duplicated, dropped); FL's test for a shared channel (another clip of it on an audio track linked elsewhere,
  any arrangement); `trackmodes::audioTrackConflicts` lists the channels that need the question.
- The Playlist asks FL's "Make clip unique?" (Yes / No) once per channel; Yes is its own undo step.
- Files dropped on an audio track: their new channel plays through its mixer track.
- Resizes, trims, knife, quantize, time moves, merges: never reroute.
- Tests: `issue15_only_placed_audio_clips_follow_an_audio_track`, `issue15_shared_channel_asks_to_make_the_clip_unique`,
  `rea146a_playlist_track_modes` (updated).
