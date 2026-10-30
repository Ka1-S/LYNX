# Contributing to LYNX SHARE

Thanks for wanting to help! LYNX SHARE is an alpha, so bug reports, ideas and pull requests are all very welcome.

## Reporting bugs and asking for features
- Use the [issue templates](https://github.com/Ka1-S/LYNX-SHARE/issues/new/choose): *Bug report*, *Feature request* or *Sound / plugin difference*.
- Say which LYNX SHARE version and OS you use (Help › About). For crashes, attach the crash file LYNX SHARE offers to send, or `%APPDATA%\LYNX\autosave\crash*.txt` (Windows) / `~/.config/LYNX/autosave/` (Linux).
- Screenshots and short videos help a lot, especially side by side with FL Studio.

## Pull requests
1. Build LYNX SHARE as described in [README › Building](README.md#building) and run the tests (`scripts/build-local.sh`). They must pass with 0 failures.
2. Keep each pull request focused on one change, and add a test for logic changes.
3. Code, UI texts, comments and docs are in **English**. New UI texts also need a pt-BR line in `resources/i18n/pt-BR.txt`.
4. Follow the style of the file you're editing (C++20, JUCE 8).

## Clean-room rule
LYNX SHARE recreates the FL Studio *workflow*, not its code or artwork.
- Don't copy code, graphics, presets, samples or names from FL Studio or any third-party plugin.
- Third-party plugins are studied as black boxes only (host them, feed signals, measure). Never decompile, patch or bypass their licensing.

## License
By contributing you agree that your contribution is licensed under the [GNU AGPL v3](LICENSE), like the rest of LYNX SHARE.
