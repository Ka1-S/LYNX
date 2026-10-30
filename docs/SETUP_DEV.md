# Development environment

How to set up a machine to build, test and analyse LYNX the way the project is developed: Windows 11 with WSL2
(Ubuntu), the Windows build cross-compiled from WSL with zig, and the tests run against the user's own FL Studio
installation (never redistributed).

## 1. Windows side

1. **FL Studio 2026** installed at `C:\Program Files\Image-Line\FL Studio 2026` — the reference build is
   **26.1.7.5654**. Several engines read files from it and check their SHA-256 (see `scripts/verify-env.sh`); a
   different build still runs but those engines fall back to approximations, and REA notes cite this build's addresses.
2. **WSL2 + Ubuntu** (`wsl --install -d Ubuntu`). Give WSL enough memory in `C:\Users\<you>\.wslconfig`:

   ```ini
   [wsl2]
   memory=48GB      # about 3/4 of the RAM; Ghidra analyses and parallel builds are memory-bound
   swap=8GB
   processors=<logical cores>
   ```

   then `wsl --shutdown` and reopen Ubuntu.
3. A test folder for headless runs: `C:\Users\<you>\lynx-test` (exports, regressions, snapshots).

## 2. WSL side (user space, no root needed except apt)

```bash
sudo apt update && sudo apt install -y build-essential python3 python3-venv python3-numpy unzip git curl xz-utils
mkdir -p ~/opt ~/projetos
```

| Tool | Version | Where | Notes |
|---|---|---|---|
| zig (Linux build of the tests) | 0.13.0 | `~/opt/zig-linux-x86_64-0.13.0/` | https://ziglang.org/download/ |
| zig (Windows cross-build) | 0.14.1 | `~/opt/zig-x86_64-linux-0.14.1/` | |
| JUCE | 8.0.4 | `~/opt/juce-8.0.4/` | `git clone --depth 1 -b 8.0.4 https://github.com/juce-framework/JUCE ~/opt/juce-8.0.4` |
| GitHub CLI | 2.62+ | `~/opt/gh_*/bin/gh` or apt | `gh auth login` |
| Node.js (for the REA MCP server) | 24.x via nvm | `~/.nvm` | `curl -o- https://raw.githubusercontent.com/nvm-sh/nvm/v0.40.1/install.sh \| bash; nvm install 24` |
| Temurin JDK | 21 | `~/opt/jdk-21.*` | https://adoptium.net |
| Ghidra | 12.1.4 | `~/opt/ghidra_12.1.4_PUBLIC/` | https://github.com/NationalSecurityAgency/ghidra/releases |
| Claude Code | latest | | `npm i -g @anthropic-ai/claude-code` |

The build scripts honour `ZIG`, `JUCE_DIR` and `OUT` if the paths differ.

## 3. The repository

```bash
cd ~/projetos && git clone https://github.com/Ka1-S/LYNX-SHARE.git lynx && cd lynx
git config user.name "<your name>" && git config user.email "<your GitHub email>"
scripts/build-local.sh "/mnt/c/Program Files/Image-Line/FL Studio 2026/Data"   # must end "N checks, 0 failures"
scripts/build-windows-zig.sh                                                   # build-win/LYNX.exe
scripts/verify-env.sh                                                          # checks everything above
```

Run only **one** `build-windows-zig.sh` at a time (two concurrent zig builds can hang). When several builds run in
parallel (e.g. git worktrees), give each its own zig cache:
`export ZIG_GLOBAL_CACHE_DIR=$HOME/.cache/zig-<name> ZIG_LOCAL_CACHE_DIR=$HOME/.cache/zig-<name>`.

## 4. REA (reverse-engineering MCP server)

Register it in `~/.claude.json` under `mcpServers` (adjust the paths):

```json
"rea": {
  "command": "/home/<you>/.nvm/versions/node/v24.18.0/bin/npx",
  "args": ["-y", "rea-agents@6.5.0", "mcp"],
  "env": {
    "GHIDRA_INSTALL_DIR": "/home/<you>/opt/ghidra_12.1.4_PUBLIC",
    "JAVA_HOME": "/home/<you>/opt/jdk-21.0.12.1+1",
    "PATH": "/home/<you>/opt/jdk-21.0.12.1+1/bin:/home/<you>/.nvm/versions/node/v24.18.0/bin:/usr/local/bin:/usr/bin:/bin",
    "GHIDRA_HEADLESS_MAXMEM": "6G",
    "REA_GHIDRA_STARTUP_TIMEOUT_MS": "3600000"
  }
}
```

With more RAM, `GHIDRA_HEADLESS_MAXMEM` can go to 12G and several analyses can run at once. The analysis notes are in
`docs/analise/REA_rodada*.md`; the reference models and DLL harnesses they cite live outside the repository (they are
working material, not part of LYNX).

## 5. Headless checks used after every change

```bash
cp build-win/LYNX.exe /mnt/c/Users/<you>/lynx-test/
cd /mnt/c/Users/<you>/lynx-test
./LYNX.exe --selftest self-report.txt self-demo.lynx          # result=OK
./LYNX.exe --export "<windows path of an .flp>" out.wav < /dev/null   # out.wav.txt: peak=, render_ms=, report
./LYNX.exe --snapshot snap self-demo.lynx                      # PNGs of every view (add --lang pt-BR)
```

Always pass `< /dev/null` to `LYNX.exe` inside shell loops (it reads stdin).
