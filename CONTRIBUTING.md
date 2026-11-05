# Contributing

Thank you for helping. Testing on hardware that has not been tried is as useful as code: see [Contributing](README.md#contributing) in the README. This page is for changes to the source.

## Ground rules

- **Nothing from the game goes in.** No game files, art, sound, text, dumps or anything derived from them, and nothing
  from other Gameloft games. `.gitignore` keeps out the usual places (`GangstarMV.app`, `analysis`, `screenshots`,
  `extracted_audio`, `build`), but check your commit too.
- **The Mac game is the reference.** The port should play as the Mac did unless a setting says otherwise:
  - a fix for a problem that running on a PC causes is on, also under `ORIGINAL_MAC_EXPERIENCE`;
  - a fix for a bug the original game has too is on by default and off under `ORIGINAL_MAC_EXPERIENCE`;
  - an enhancement gets its own key, is on in a new `GangstarMV.ini`, and is off under `ORIGINAL_MAC_EXPERIENCE`.
    A key left blank or missing keeps the Mac's behaviour.
- **Native UI only.** Settings go on the game's own GAMEPATCH tab, not in an overlay.
- **No network code.** The port sends nothing anywhere.

## Where code goes

| Folder | What belongs there |
|---|---|
| `host/core/` | The loader. It knows no game, and reaches one only through `core/game.h` |
| `host/shims/` | The macOS libraries the game imports. They know no game either |
| `host/games/gangstar/` | Everything about this game: its layout, fixes, enhancements and add-ons |
| `host/games/template/` | A game with no patches. It must keep building with `core/` and `shims/` alone |

[`docs/NOTES.md`](docs/NOTES.md) explains how the parts work, and has notes on the ABI and on adding another game.

## Building and checking

- Build as the [README](README.md#build-instructions-for-developers) says, on Windows, Linux or both.
- **No warnings.** The source builds with `-Wall -Wextra` and none, and the CI check fails on any, for Windows and
  Linux, release and dev builds, `gangstar` and `template`. From Windows, `tools/linux_check.ps1` compiles and links the
  Linux build without a Linux machine.
- **Release builds stay clean.** Diagnostics, traces and test aids go behind `GMV_DEV` (see `core/log.h`), so a release
  build has none of them. A release program must hold no path or name from the machine that built it, and the log must
  not name the player's folders or songs.
- **Windows and Linux both.** System calls go through `core/platform.h` or `shims/compat.h`, not straight to one
  system.

## Testing

`tools/playtest/playtest.ps1` (Windows) plays scripted tests with your copy of the game and reports PASS or FAIL for
each. It types and moves the mouse for real, so leave the keyboard and mouse alone while it runs.

```powershell
tools\playtest\playtest.ps1 -List                  # the scenarios
tools\playtest\playtest.ps1 boot,world             # some of them
tools\playtest\playtest.ps1 boot -Exe <path>       # another build, such as a release
```

Each run gets its own folder, so your own save and settings are never touched. Results, logs and screenshots go to
`build/playtest/results/<time>/`. Look at the screenshots too, not only at PASS. Scenario files are in
`tools/playtest/scenarios/`, and the first lines of `playtest.ps1` list every step a scenario can use.

## Documenting a change

Each fix or enhancement is written up in four places:

- [`docs/NOTES.md`](docs/NOTES.md): a numbered item that says **why** the change was made, **whose fault** the
  problem was (the original game's or the port's), **what it does under `ORIGINAL_MAC_EXPERIENCE`**, **the date**, and
  **how it was found** and tested. A new key also gets a row in the table of every key's history.
- [`FIXES.txt`](FIXES.txt): the key in plain words, for players.
- [`CHANGELOG.md`](CHANGELOG.md): one entry, under the part of the port it belongs to.
- [`PATCHNOTES.md`](PATCHNOTES.md): one line, in its category.

## Pull requests

Say what the change does, why, and how you tested it: which system, which build, which scenarios, and what you looked
at in game. Keep a pull request to one change where you can.
