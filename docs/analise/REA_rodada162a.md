# Round 162a (browser162): the Browser items of issue #9

2026-10-26. Issue #9 (BURNER1PA, Windows 10, LYNX 0.1.17) lists six Browser items: a crash some 20–40 s after
searching every folder, a search index, the search box keeping the keyboard (Space types instead of playing), a Stop
button for the preview, arrow keys through the Browser, and dragging plugins from the Browser onto the Channel Rack.

Evidence markers: [CODE] read in the source; [MEAS] measured here; [TEST] core test (build-local); [EXEC] run of a
LYNX or test build.

## 1. The crash: background search threads killed by force

**Root cause [CODE].** `BrowserComponent::runSearch` (0.1.17) began with `job.reset()`: the previous search's
`juce::Thread` was destroyed with `stopThread (3000)`. Each search thread first refreshed the search index
(`refreshIndex`: a walk of every folder of the scope, checking `threadShouldExit()` only between two folders) while
holding the index's mutex, and the start-up `IndexJob` held the same mutex for its whole first walk. When the old
thread did not finish within 3 s — it was waiting for the mutex behind the start-up walk, or it was listing one big
folder on a slow / sleeping drive — JUCE's `stopThread` time-out calls `killThread()`, i.e. `TerminateThread` on
Windows ("!! killing thread by force !!"). So every letter typed while "every folder" was being walked froze the UI for
3 s and then killed a thread that could be anywhere inside the C++ runtime (allocating `juce::String`s / vectors,
inside `FindFirstFile`, or half-way through changing the index's `std::map`).

**Repro of the mechanism [EXEC].** `tmp/…/scratchpad/repro/repro.cpp` (built with the same Zig 0.14.1 /
x86_64-windows-gnu toolchain as LYNX, run on this PC, results kept in `D:\lynx-work\browser162`): an "index" thread
holds a `std::mutex` for a long walk; six "keystroke" threads are killed with `TerminateThread` after their wait time
runs out.
- killed while blocked on the mutex: the process survives (SRW lock wait blocks stay on the leaked stacks);
- killed while allocating (as a thread walking the disk does): the process **hangs for good** at the next allocation
  of another thread (exit 124 = `timeout` had to end it) — the heap lock dies with the killed thread. In the GUI this
  shows as LYNX freezing and then being closed by Windows / crashing a little later, as reported.

**Fix [CODE][TEST].** `src/core/SearchService.{h,cpp}` (JUCE-free): one worker thread per process owns the index.
- `search()` replaces the waiting search; the worker serves the newest search first; walks stop between two folders
  when a newer search arrives or the service closes; nothing is ever killed (the destructor joins after the current
  folder).
- `Index::refresh (…, std::mutex* guard)` locks the index only while it is read or changed, never while the disk is
  read, so a search of what is indexed is answered while a long walk runs.
- `BrowserComponent` keeps a `shared_ptr` to the service (`SearchShared`, weak singleton); answers come back with
  `MessageManager::callAsync` + `SafePointer` and a search id, status/index changes through two `ChangeBroadcaster`s.
- Tests `browser162_*` (tests/Browser162Tests.inc) on a slow synthetic disk: typing five queries 60 ms apart during
  a ~1 s first index (asking never waits > 50 ms, final answer = last query, complete; every folder listed once),
  closing mid-walk (< 500 ms, no kill), index persisted and reused (no listing on the next session), a changed folder
  picked up after a search, rebuild, cancel, and a search answered in ~4 ms during a rebuild of 2,551 slow folders.
  Clean under ThreadSanitizer and AddressSanitizer (g++ 15) [MEAS].

## 2. Search index (feature)

LYNX already had an index since round 134 (`core/SearchIndex`, FL's "dirty folders" idea: only folders whose
modification time changed are listed again; file `browser-index.txt` in the settings folder). Round 162 makes it the
search's only source and makes it visible:
- searches read the index (instant once it exists); a folder tree never indexed is indexed first with **partial
  results every 0.5 s**; "complete" trees are remembered in the index file (`R` lines; older files still load);
- after a search, the searched folders are checked again in the background (at most every 30 s); when that lists
  changed folders, the Browser searches again in place (keeping the selection and scroll position);
- a background check of every Browser folder 4 s after start (as before), and the index is saved every 60 s during a
  long first walk so it survives a crash or quitting;
- status: a line under the search box (indexing progress, "Found N files", "checking the index..."); the Browser
  panel's menu shows "Search index: N files in M folders (checked at HH:MM)" with **Refresh search index** and
  **Rebuild search index**; a folder's right-click menu has "Refresh search index" too.

## 3. Search box focus and Space

[CODE] The Browser's lists, the panels and the toolbar do not take the keyboard when clicked (most LYNX components
use `setMouseClickGrabsKeyboardFocus (false)` so the main window's shortcuts work), so once the search box had the
keyboard it kept it until something that wants keys was clicked — Space kept typing into the box, and Up/Down moved
its caret instead of stepping through the Browser. FL ends typing in its search field when you click elsewhere.
Fix: a global mouse listener (`BrowserComponent::FocusWatcher`): any click outside the box (other panels, toolbar,
the Browser's own list, other LYNX windows; not the box's own right-click menu) gives the keyboard back to the main
window; Enter (search now) and Esc (clear) leave the box too; switching panels by keys (F5…) also does
(`Workspace::onFocusChanged`). Space types only while the box is actively being typed in.

## 4. Stop button

A Stop button (Icon::stop) between the search box and the preview volume knob: stops the sample / SoundFont preview
(`engine.previewFile ({})`) and the preview strip's cursor.

## 5. Arrow keys

Already there since earlier rounds (KeyMap `browser.previous/next/openFolder/closeFolder`, Browser context): they did
not seem to work because of item 3 (the search box kept the keys). Changed to FL's behaviour: Up/Down now walk every
row, folders included (previewing each sample), so Right can open the folder reached and Left close it / go to the
parent; Shift+Up/Down (load each file into the selected channel) still skips folders.

## 6. Drag plugins from the Browser

The Plugin database rows (round 142) now carry what they add (`Node::generator` / `Node::effect`) and can be dragged:
- a generator (LYNX built-ins, FL engines, installed VST/VST3/CLAP/LV2 instruments) onto the Channel Rack: a new
  channel with it, its window open (as double-click);
- an effect onto the Mixer's effect slots (on a slot: replaces it, elsewhere: the first free slot — like .fst presets)
  or onto an insert strip (goes into that insert, which is selected).
Drag descriptions `lynx-plugin-generator|<name>` / `lynx-plugin-effect|<name>`; the payload stays in the Browser
(`draggedGenerator / draggedEffect`).

## 7. GUI verification [EXEC]

Linux build of this branch under Xvfb (fresh profile, Browser folders = a synthetic tree of 1,641 folders / 8,000
samples + /usr), driven with xdotool; screenshots in `D:\lynx-work\browser162\linux-gui\` (s1…s14):
- start: the status line "Indexing the Browser's folders for search: …" under the search box, the Stop button (s1);
- typing five queries quickly into "every folder" while indexing, then "kick 3": results at once, "Showing the first
  500 results." line, LYNX responsive (s2);
- click on the Playlist title, Space: transport plays, the box still reads "kick 3" (s3); click a result, Down Down:
  the selection moves two rows (s4); the Browser menu shows "Search index: 8000 files in 7412 folders (checked at
  16:03)" + Refresh / Rebuild (s5); Ctrl+F, Esc, Space: plays (s6);
- arrows through the Plugin database folders (Down/Right) (s9); drag "Kick" onto the Channel Rack: channel 6 added
  (s10); drag Delay onto the Insert 3 strip and Reverb onto its slot 2 (s14).

Windows: the branch's LYNX.exe and 0.1.17 were each driven 3 min with typed searches over every folder + D:\ (the
user's HDD) via SendKeys; both survived, but the runs were not conclusive (a firewall prompt and the "LYNX closed"
dialog of the forced stop took the keys), so the crash was not reproduced in the GUI; the mechanism is reproduced by
repro.cpp (section 1). The user's LYNX settings were restored afterwards (browser-index.txt from a backup, no
browser-folders.txt, stale session markers removed).

## Where I stopped

- Done: items 1–6, tests, pt-BR, notes and the suggested reply (tmp/coord). Landing: build-local with FL Data,
  Windows build, then `land.sh browser162`.
- Not done / ideas: a file-system watcher (ReadDirectoryChangesW / inotify) instead of the per-search check; dropping
  a generator onto an existing channel to replace it (FL does) — today it always adds a new channel.
