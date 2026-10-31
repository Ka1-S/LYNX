# Round 166a: issue #15 (audio clips change insert) and the three bugs of issue #14

v0.1.21. Per item: reporter's words | FL 2026 evidence | LYNX before | LYNX now.

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

### FL 2026 [SHOT, black box, 2026-10-31 01:13–01:16, FL 2026 build 5654, shots r166a/f*.png]

- A clip of an unrouted audio channel moved from the unassigned Track 3 onto an audio track linked to Insert 14: the
  channel's mixer track became 14.
- The channel routed back to Insert 3 by hand (Channel settings): moving *another* clip (a kick on another track) and
  sliding the clap clip horizontally along its own audio track left it on 3. Moving it to an unassigned track: still 3.
- Moving it onto the audio track linked to Insert 10: 10.
- A Shift+drag clone of the clip moved onto the Insert 14 audio track while the original stays on the Insert 10 one: FL
  asked "Make clip unique?"; No: the channel stays on 10, the clip stays where it was dropped.
- Not black-box tested: take recording onto an audio track (arming via the Playlist was inconclusive). FL's what's new
  (13543 "recording wizard now adds an audio track", 18397, 10918) says recorded clips land on audio tracks, so LYNX
  keeps its existing take routing (`linkTakeTrack`).

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
- Resizes, trims, knife, quantize, time moves, merges, and a move that keeps a clip on its own track: never reroute
  (only the clips whose track changed count, as in FL's black-box check above).
- Tests: `issue15_only_placed_audio_clips_follow_an_audio_track`, `issue15_shared_channel_asks_to_make_the_clip_unique`,
  `rea146a_playlist_track_modes` (updated).

## 2. Issue #14 bug A: snap to scale with several notes (kubapl2004gmail, Linux)

> Snap to scale is not working properly with multiple notes selected.

### FL 2026 [SHOT, black box, r166a/f36–f40]

- The scale is chosen with a right-click on the Piano roll's "Snap to scale" button (menu: Snap PR notes to scale,
  Snap incoming MIDI, the scales; Major on C here).
- C4, E4, G4 selected and dragged up by 1 semitone: nothing moves (each note would leave the scale and snaps back).
- Dragged up by 2 semitones: C, E, G → D, F, A. Each note takes its own key + the drag and snaps to the nearest key of
  the scale (a tie goes down: E+2 = F♯ → F).
- Plain Up / Down only scroll the view in FL; Shift+Up transposes the selection one *scale step* (D, F, A → E, G, B).

### LYNX before

The drag snapped only the grabbed note's key to the scale and moved the others by the grabbed note's delta, so they
left the scale (or all moved by the grabbed one's step).

### LYNX now

- `notetools::moveKeysOnScale (pattern, indices, originals, deltaKeys, root, scale)`: each note = its original key +
  the drag, snapped to the scale (ties down), as FL. Used by the Piano roll's move drag while Snap to scale is on and
  more than one note moves.
- `notetools::stepOnScale (key, dir, root, scale)`: LYNX's nudge keys (Up / Down, LYNX's own binding; FL's is
  Shift+Up / Down) step along the scale while Snap to scale is on.
- Test: `issue14_snap_to_scale_moves_every_note_onto_the_scale`.

## 3. Issue #14 bug B: keys that light up

> The keys on the piano roll should light up when the notes are played.

### FL 2026 [SHOT, black box, r166a/f28]

During playback, the Piano roll keyboard keys of the sounding notes are lit (light orange, the whole key). The typing
keyboard test (Q held) showed no lit key in that shot (inconclusive); black keys and other channels not tested.

### LYNX before

Only the key clicked with the mouse was drawn pressed.

### LYNX now

- `RackRenderer` keeps, per channel, the keys that sounded in the last block (notes held + notes struck and released
  inside it) and publishes them (`soundingKeys (channel)`, lock-free atomics); `AudioEngine::soundingKeys`.
- The Piano roll polls them on its timer (the selected channel, or every channel with notes in "all channels" view),
  repaints the keyboard only when they change and draws those keys pressed (LYNX's accent colour, the same as a
  mouse-pressed key: LYNX's dark theme, FL's behaviour).
- Test: `issue14_renderer_publishes_the_sounding_keys`.

## 4. Issue #14 bug C: master pitch, channel pitch and slide notes

> Master pitch knob and the pitch knobs for instruments are not guaranteed to work or apply to all instruments
> especially with slide notes.

### FL 2026

- [STRINGS FLEngine_x64.dll / FL's wrapper] the plugin wrapper has "Pitch bend range (semitones)" and "Send a pitch
  bend range MIDI RPN": FL drives a hosted plugin's pitch through MIDI pitch bend scaled by that range. What's new
  16592 / 18957 (pitch bend range sent to VST3), 18423 (PoiZone pitch bend).
- [INFERENCE, FL SDK voice model + what's new 16250 "MiniSynth: piano roll slides don't work", 18004 / 17811 "Repeated
  notes don't slide correctly"] slides are a host feature: FL moves the Final pitch of a native generator's sounding
  voices, every native generator follows it. Plucked! reads Final.Pitch only when a voice starts (REA_rodada65 §1.2),
  so it can't glide in FL either.
- Not black-box measured this round: what FL sends a VST on a slide note. Loading LYNX's MIDI-logging test VST2 into FL
  needed a plugin scan that started third-party license pop-ups on the user's machine; it was cancelled, the added
  search path removed and FL closed without saving.

### LYNX before [TEST `issue14_pitch_audit_of_the_instruments`, rendered with FL's data, 7 semitones each]

| engine | main pitch | PITCH knob | slide |
|---|---|---|---|
| LYNX Synth | +701 | **+0** | +701 |
| 3x Osc, Sytrus, Harmor, Harmless, GMS, Morphine, Ogun, Toxic, PoiZone, Kepler, Transistor Bass, SimSynth, MiniSynth, DX10, Plucked!, Sawer, BeepMap, FL Keys | ≈ +700 | ≈ +700 | **+0** |
| Sakura, BooBass, Drumpad | **+0** | **+0** | **+0** |
| hosted plugins (code) | ignored | ignored | ignored |

### LYNX now

- `core/HostSlide.h`: one glide per voice (linear in cents over the slide note's length, the voice keeps its key).
  Every native FL engine above overrides `slideTo` and adds the glide to its voices' pitch only while a slide is active
  (with no slide the arithmetic is unchanged, so the bit-exact FL comparisons stay). Plucked! is left as FL.
- LYNX Synth follows the channel PITCH knob; Sakura, Drumpad and BooBass follow the knob, fine tune and main pitch.
- `Instrument::setLivePitch (cents)`: the renderer passes each channel's PITCH knob + fine tune + main pitch before
  every block. A hosted plugin (`PluginInstrument`) turns it into MIDI pitch bend scaled by the wrapper's pitch bend
  range, and a slide note into a pitch-bend glide (64-sample steps) that a new note resets.
- Tests: `issue14_pitch_audit_of_the_instruments` (now with checks: every engine within 40 cents of +700 for main
  pitch, knob and slide, except the ones whose pitch the estimate can't read), and
  `issue14_plugin_channels_get_the_pitch_knob_main_pitch_and_slides`.
- Linux: the core tests run natively on Linux (WSL); the JUCE side (PluginHost, Piano roll, Playlist) compiles in the
  Linux build.
