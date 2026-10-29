# lynx-share.com

The project website: a single static page (`index.html`) plus `assets/`.
The download buttons read the GitHub releases at runtime, so a new release needs no redeploy.

- **Download section (Windows / macOS / Linux):** the script detects the visitor's OS (`navigator.userAgentData.platform`,
  then `navigator.platform` / `userAgent`; a touch "Mac" is treated as iPadOS) and shows "Download for <OS>" plus that
  OS's second format, with the other platforms as links below. Phones, tablets, ChromeOS and unknown systems get all
  three platforms with no "for your OS" claim. `?os=win|mac|linux|any` forces a platform (testing, screenshots).
- **Asset matching** (`DL` in the script) runs over `/releases?per_page=10`; for each OS and format the newest
  non-draft release with a matching file wins, so platforms missing from the latest release fall back to older ones
  and releases up to 0.1.7 (Windows only) still work:
  Windows `/setup.*\.exe$/i` (fallback `/\.exe$/i`) and `/windows.*\.zip$/i`; macOS `/macos.*\.dmg$/i` (fallback
  `/\.dmg$/i`); Linux `/\.AppImage$/i` and `/linux.*\.tar\.gz$/i`. Anything unresolved, and every button without JS
  or when the API fails, links to `releases/latest`. The nav/hero `.dl-link` buttons go to the detected OS's main
  file (or to `#download` when no OS was detected).
- The multi-platform download section needs **one deploy** once v0.1.8-alpha (the first release with macOS and Linux
  files) is published; after that, new releases again need no redeploy.
- Screenshots come from LYNX SHARE's own showcase project (`LYNX.exe --showcase Demo.lynx`, then
  `LYNX.exe --snapshot <prefix> Demo.lynx` with `LYNX_SNAPSHOT_SIZE=1920x1080`), never from Image-Line demo projects.
- `assets/media/demo.mp3` is `LYNX.exe --export Demo.lynx demo.wav`; `lynx-tour.mp4` is a slideshow of the
  screenshots over that audio (ffmpeg zoompan + xfade); `peaks.json` is its waveform (600 peaks).
- `?shot` in the URL disables the entrance animations (for headless screenshots).
- Publish with `scripts/deploy-site.sh` (Hostinger API token in `~/.config/hostinger/token`, never committed).
- The plugin section is data-driven: `FEATURED` (highlight cards, Exact/Near-exact only, `by` = the original's maker,
  so engines inspired by third-party plugins can be added) and `PLUGINS` (full grid), kept in sync with `docs/PLUGINS.md`.
- The "Inspired by the classics" section (`#classics`) is generated: `scripts/gen-inspired.py` reads the substitute
  table in `src/core/PluginSubstitutes.cpp` plus `docs/inspired-by.json` (descriptions EN/pt-BR, short names, work in
  progress) and rewrites the `INSPIRED` data block here and the matching sections of `README.md` and `docs/PLUGINS.md`.
  Rerun it whenever an engine lands (release step); `--check` fails when a block is stale. A new LYNX SHARE engine without an
  entry in `inspired-by.json` is printed as a warning: add its description there.
