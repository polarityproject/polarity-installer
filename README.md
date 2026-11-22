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
| `src/installer/installer_linux.cpp`, `linux_*.cpp` | Linux installer (one static binary; zenity or kdialog windows, the terminal otherwise; files below) |
| `src/installer/linux_steam.*` | Steam's VDF files, the launch options, the game folder rules (Linux) |
| `src/installer/install_log.*` | The installer's log, shared by both |
| `src/installer/version.h` | The installers' version, and the commit the build scripts stamp in |
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

Both scripts stamp the current commit into the build (with `-dirty` when the tree has changes) and keep that build's symbols in `dist\symbols\<commit>\`: the exe with its `.pdb`, and the Linux binary before stripping. It is how a crash in a player's log turns into file and line.

clang-format sets the layout of the C++ sources (`.clang-format`). It comes with the Build Tools' "C++ Clang tools" component. `scripts\format.ps1` formats `src\` and `tests\`, and `scripts\format.ps1 -Check` lists the files that are not formatted.

## Releases

GitHub Actions (`.github/workflows/build.yml`) runs both build scripts on every push to main and every pull request. To release, raise `POLARITY_INSTALLER_VERSION` in `src/installer/version.h`, then push a `v<version>` tag. The workflow attests both installers and puts them, with their symbols, in a draft release. Publish the draft and the site's download buttons, which link to the latest release's files, offer it.

To read a crash from a released build, download that release's files into `dist\symbols\<commit>\`, with the commit from the log's first line, and run `scripts\read-crash.ps1` as usual.

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
| `--test-crash <kind>` | Crashes on purpose to test the crash report: `segv`, `throw` or `terminate` |

Windows only: `--find` prints the game folder Steam has and exits.
Linux only: `--steam-root <folder>` uses that Steam folder instead of looking for one; `--terminal` asks in the terminal even when windows are available.

## The Linux installer

| File | What it covers |
| --- | --- |
| `installer_linux.cpp` | the steps, the options and `main` |
| `linux_system.cpp` | files and processes |
| `linux_download.cpp` | the release info and files over HTTP(S), and swapping them in |
| `linux_steam_control.cpp` | Steam's folders and libraries, closing and opening Steam, the launch options |
| `linux_ui.cpp` | zenity, kdialog or terminal questions, and what they asked in the log |
| `linux_crash.cpp` | the log, and the crash report |

Downloads follow redirects. The installer uses the system's trusted roots when it finds a bundle where distributions keep one. Without one the download still goes ahead, since what it fetches only counts once the signature and the hashes check out. New files are swapped in with the old ones moved aside first, so a failure puts everything back.

The launch options are read and set only while Steam is closed: a running Steam holds them in memory, an edit in Properties reaches the file only when Steam closes, and Steam then writes over the file. Each `localconfig.vdf` it changes keeps a copy of the file as it was next to it.

The crash handler writes only with raw system calls, `fork` and `execve`, since nothing else is safe in a signal handler. It walks the stack through the frame pointers while they are in the installer's own code; libc keeps no frame pointer. Each return address less one is an address inside the call, which `addr2line` resolves to the call's line. A binary linked with `-static` is at a fixed address even when its code was built with `-fPIE`, so the handler takes off the load address only for a position-independent build. The crash window is set up at the start, because a signal handler cannot build it. Ctrl+C, a closed terminal or a kill is noted in the log before the installer ends.

## Running on Linux

Downloads lose the executable bit, so players run it once from a terminal:

```bash
chmod +x polarity-installer-linux && ./polarity-installer-linux
```

On the Steam Deck, run it in Desktop Mode.

## Logs

Each run adds to `polarity-installer.log` next to the installer, usually in the player's Downloads folder. When that folder can't be written to, the log goes to `%TEMP%` on Windows and to `~/.local/state/polarity-installer/` on Linux (`/tmp` as a last resort). A log past 1 MB starts over.

A run's log has the installer's version and commit, the system, every download with its size or error, what the installer found in Steam, each file it replaced, the launch options before and after (Linux), what each window asked and which button the player picked, and the exit code. The player's home folder appears as `~`.

When something fails, the error window points to the log. On Windows it has a "Show the installer's log" link that opens Explorer at the file; on Linux it shows the path. A crash adds the exception or signal and the call stack to the log, then opens a window with the log's path.

To read a crash, run this on the log a player sent:

```powershell
scripts\read-crash.ps1 <polarity-installer.log>
```

It finds the commit in each run's first line and turns each stack into function, file and line from `dist\symbols\<commit>\`. Windows stacks use the `llvm-symbolizer` that comes with the Visual Studio C++ build tools. Linux stacks use `addr2line` in WSL, or in a Docker container when WSL has none.
