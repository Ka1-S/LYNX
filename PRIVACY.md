# Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the
user or the person installing or operating it.

## What LYNX SHARE does and does not do

- **Two network connections, both disclosed and switchable.** LYNX SHARE contains no telemetry, analytics,
  advertising, online account or licence check. It connects to the network for exactly two things:
  1. **The update check** (below): a version query to GitHub, on by default, off in *Settings › General › Updates*.
  2. **Crash reports**: sending a crash / freeze report to `https://lynx-share.com/api/crash.php`, only with your
     consent (below).
  The source code is public, so this can be verified: the update check in `src/ui/Updater.cpp` and the report upload
  in `src/ui/CrashReporter.cpp` are the only uses of a network API (JUCE's `URL` / `WebInputStream`; on Linux the
  `curl` program), and JUCE's web browser component and its curl support are disabled at build time
  (`JUCE_WEB_BROWSER=0`, `JUCE_USE_CURL=0`).
- **No personal data is collected.** The update check sends nothing about you or your computer; a crash report
  leaves your computer only when you choose to send it.
- **Local files only.** LYNX SHARE writes only to files you choose (projects, exports) and to its own settings folder
  `%APPDATA%\LYNX` (language, plugin list, autosaves and similar preferences). Delete that folder to remove them.
  It also registers the `.lynx` file type for the current user (`HKEY_CURRENT_USER\Software\Classes`) so project
  files open in LYNX SHARE.
  The Microsoft Store version keeps that folder in its own app-data folder instead
  (`%LOCALAPPDATA%\Packages\<LYNX package>\LocalCache\Roaming\LYNX`, removed when the app is uninstalled) and
  registers no file types itself (Windows does, from the package).
- **Plugin sandbox.** When a plugin is run in its own process, LYNX SHARE talks to that process through a local
  inter-process channel on the same computer; nothing is sent over a network.
- **Crash logs** are written to local files (`autosave\crash *.txt`, `hang *.txt`, `crash-history.log`).

## Update check (on by default, switchable)

A few seconds after it starts, LYNX SHARE asks GitHub's public API for the list of LYNX SHARE releases
(`https://api.github.com/repos/Ka1-S/LYNX-SHARE/releases`) and compares the newest version number with its own. If a
newer version exists, a bar under the toolbar says so (*What's new*, *Update*, and ✕ to skip that version). The list
is kept for three hours (`update-cache.json` in the settings folder), so LYNX SHARE asks at most once every few hours;
*Help › Check for updates…* asks at any time.

- **What is sent:** an ordinary HTTPS request for that public list, with the user agent `LYNX-SHARE/<version>`. No
  identifier, no settings, no project, plugin or system information. GitHub sees your IP address, as for any visit to
  a web page; GitHub's privacy statement applies to it. LYNX SHARE's own server is not involved.
- **Turning it off:** *Settings › General › Updates › Check for updates automatically*. Off, LYNX SHARE never connects
  for updates unless you choose *Help › Check for updates…*. The choice is kept in `updates.txt` in the settings folder
  (with the version you skipped, if any).
- **Updating** happens only when you click *Update*: the new release file comes from GitHub and is checked against the
  release's `SHA256SUMS.txt` before it is used. The Windows installer version then installs it over itself (after
  asking to save your project) and starts again; the portable and macOS versions save the file to your Downloads
  folder; the Linux AppImage updates itself through AppImageUpdate when it is installed; the Linux .tar.gz opens the
  release page.
- **The Microsoft Store version never checks:** the Store updates it, so it has no update check and no *Update*
  button.

## Crash reports (opt-in)

After LYNX SHARE closed unexpectedly (a crash or a freeze), the next start asks **"Send the report to help fix it?"**:
*Send*, *Show what will be sent* (the exact text), *Don't send*, and *Report on GitHub instead* (opens GitHub's issue
form prefilled; you attach the files yourself). **Help › Report a problem…** sends one by hand, with an optional
description and an optional e-mail address (only if you want an answer). **Settings › General › Crash reports**:
*Ask* (default), *Always send*, *Never send*.

A report contains: a random report id, the LYNX SHARE version / build / commit, the operating system and its version, the
CPU, the graphics cards and their driver versions, the screen size and scaling, the audio driver / device / sample
rate / buffer size, the crash or freeze file (where in the program it stopped), the session's last 60 steps, the
names, vendors, formats and versions of the third-party plugins loaded in that session, and the "GPU rendering" and
plugin sandbox settings.

It does **not** contain: audio, projects or samples, your computer's name, your user name (every path shows `<user>`
instead), file and folder names of projects and samples (only their extension, e.g. `<file>.flp`, unless you tick
"include file names"), or your IP address. The server keeps no IP address: to limit abuse it counts reports per day
under a salted hash of the address that is deleted the next day. Reports are stored on lynx-share.com's server
outside the website (not reachable from the web), read only by the LYNX SHARE maintainer, and used only to fix bugs.

A report that could not be sent (offline) is tried once more at the next start, then deleted. The ids of sent reports
are listed in `crash-reports\sent.log` in LYNX SHARE's settings folder. To have a report deleted, open an issue with its id
at <https://github.com/Ka1-S/LYNX-SHARE/issues>.

## Third-party plugins

VST3 and CLAP plugins that **you** load are separate programs from other vendors. They run inside LYNX SHARE (or its
sandbox process) with your permissions and may do things LYNX SHARE does not, including network access. Their behaviour is
governed by their own privacy policies.

## Website

The website [lynx-share.com](https://lynx-share.com) is a static page (plus the crash-report endpoint above). It asks the GitHub API for the latest release
so that its download buttons point to it, and remembers your language choice in your browser's local storage. It sets
no cookies and runs no analytics.

## Contact

Questions: open an issue at <https://github.com/Ka1-S/LYNX-SHARE/issues>.
