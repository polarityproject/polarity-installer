# Polarity Installer

Installs the latest Polarity release into Street Fighter V, on Windows and on Linux (Steam with Proton, including the Steam Deck).

What the installers do:

1. Read the release info from the Polarity site (`https://polaritymod.link/update.json` and its `.sig`) and check its signature against the release key, the same check the mod's own updater makes.
2. Find Street Fighter V through Steam's library folders, or ask for the game folder.
3. Download the release's files, check each one's size and SHA-256 against the release info, and put them next to `StreetFighterV.exe` (`StreetFighterV\Binaries\Win64`). Either every file is replaced or none is.
4. On Linux, add `WINEDLLOVERRIDES="xinput1_3=n,b"` to the game's Steam launch options; options the player already set stay. Steam keeps launch options in memory and saves them when it closes, so the installer asks to close Steam, sets them, and opens Steam again.
5. Say that Polarity will run the next time the game starts.

Neither installer creates desktop shortcuts, menu entries or Steam shortcuts. The Windows installer also writes no registry entries.

## Layout

| Path | What |
| --- | --- |
| `src/installer/installer_win.cpp` | Windows installer (`polarity-installer.exe`, Task Dialog windows) |
| `src/installer/installer_linux.cpp` | Linux installer (one static binary; zenity or kdialog windows, the terminal otherwise) |
| `src/installer/linux_steam.*` | Steam's VDF files, the launch options, the game folder rules (Linux) |
| `src/core/update_info.*`, `src/core/json.*`, `src/mod/update_key.h` | Copies of the client's update code and release key |
| `tests/linux_tests.cpp` | Tests for the Linux pieces, run by the Linux build |

## Building

Windows, with the Visual Studio 2022 C++ build tools:

```powershell
scripts\build-windows.ps1
```

This writes `dist\polarity-installer.exe`. The C runtime is linked in, so players need nothing else.

Linux, with Docker (it builds in an Alpine container against musl):

```powershell
scripts\build-linux.ps1
```

This writes `dist\polarity-installer-linux`, a static x86_64 binary with no dependencies on the player's system besides Steam. The build fetches mbedTLS 3.6.7 (hash-pinned in `CMakeLists.txt`) and runs `tests/linux_tests.cpp`.

## The release key

`src/mod/update_key.h` must hold the same key as the client's `src/mod/update_key.h`, or the installers refuse every release. After the client's key changes (`scripts/release.ps1 -NewKey` there), copy the key array here. To compare this repo's copies with the client's:

```powershell
scripts\check-shared.ps1 [-Client <client folder>]
```

It exits with an error when the keys differ.

## Options

Both installers take:

| Option | Use |
| --- | --- |
| `--site <url>` | Release info other than `https://polaritymod.link/update.json` (tests) |
| `--game <folder>` | The game folder, at any level down to `Binaries\Win64` |
| `--yes` | No questions; messages go to the terminal (on Linux this also closes and reopens Steam when needed) |

Windows only: `--find` prints the game folder Steam has and exits.
Linux only: `--steam-root <folder>` uses that Steam folder instead of looking for one; `--terminal` asks in the terminal even when windows are available.

## Running on Linux

Downloads lose the executable bit, so players run it once from a terminal:

```bash
chmod +x polarity-installer-linux && ./polarity-installer-linux
```

On the Steam Deck, run it in Desktop Mode.
