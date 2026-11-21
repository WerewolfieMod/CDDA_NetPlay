# DDA NetPlay Launcher

A small Windows launcher for **DDA NetPlay** builds. It downloads the game from
GitHub Releases, installs it, updates it in place and starts it — including the
co-op buttons for hosting a world and joining a friend's server.

The launcher does not compile anything. Each update is the full Windows ZIP
from a `ddamp-v*` release; saves, configuration and mods are preserved, and the
previous build is kept for a one-click rollback.

## Features

- **Install / update** from the GitHub Releases of
  [`WerewolfieMod/CDDA_NetPlay`](https://github.com/WerewolfieMod/CDDA_NetPlay).
- **Play** starts `cataclysm-tiles.exe` in the install folder.
- **Host co-op** starts the game and shows the in-game hosting steps plus your
  LAN address.
- **Join server** starts the game with `--mpclient host[:port]`, remembers the
  last address and adds the default port when it is omitted.
- **Restore previous build** rolls back to the folder kept from the last update.
- Saves (`save/`), configuration (`config/`) and user mods (`mods/`) survive an
  update.
- Automatic update check on launch, manual check button, SHA256 verification
  when the release asset publishes a digest.
- English and Russian UI, dark theme.

## Running from source

```bat
cd tools\netplay_launcher
py -m venv .venv
.venv\Scripts\python.exe -m pip install -r requirements.txt
.venv\Scripts\python.exe main.py
```

Requires Python 3.10+ and PySide6. The project is developed and tested with
Python 3.14 / PySide6 6.12.

## Building the exe

```powershell
powershell -ExecutionPolicy Bypass -File tools\netplay_launcher\build.ps1
```

The result is `tools\netplay_launcher\dist\DDA-NetPlay-Launcher.exe`
(PyInstaller one-file build). The launcher's own settings and log live in
`%APPDATA%\DDAMP Launcher\`, not next to the game.

## How updates work

1. The launcher reads `GET /repos/WerewolfieMod/CDDA_NetPlay/releases`
   (cached for 10 minutes) and keeps releases whose tag matches
   `ddamp-v<major>.<minor><letter>-<build>` and that have a Windows ZIP asset
   (`*win64*.zip` or `*windows*.zip`).
2. The newest build is compared with `version.json` inside the install folder.
3. On update the ZIP is downloaded to a temporary file, checked against the
   release digest if present, extracted next to the install folder and swapped
   in. The previous install becomes `<folder>.previous`.
4. `save/`, `config/` and `mods/` are copied into the new build. The previous
   folder is kept whole for rollback and removed only when
   `keep_previous` is off.
5. The game process is checked before every update; a running game blocks the
   operation instead of failing on locked files.

The install folder is protected: a non-empty folder that contains neither
`cataclysm-tiles.exe` nor `version.json` is refused, so pointing the launcher at
an unrelated directory cannot wipe it.

## Release requirements

For a build to appear in the launcher:

- tag: `ddamp-v4.0a-224` (same scheme as the existing tags);
- a non-draft GitHub release;
- an attached asset with `.zip`, `win64` (or `windows`) in the name;
- the ZIP must contain `cataclysm-tiles.exe` at its root.

The current build layout is produced by packing the deployed game folder:
`cataclysm-tiles.exe`, DLLs, `data/`, `gfx/`, `sound/`, `font/`, `doc/` and
`lang/`.

## Development notes

- `launcher/updater.py` — GitHub API, download, verification, install/rollback.
- `launcher/game.py` — process detection, launch arguments, LAN address.
- `launcher/ui.py` — PySide6 window and worker threads.
- `launcher/i18n.py` — English/Russian strings.
- Tests:

  ```bat
  cd tools\netplay_launcher
  .venv\Scripts\python.exe -m unittest discover -s tests -t .
  ```

- For local testing against a mock API, set `DDAMP_LAUNCHER_API` to a base URL
  that serves `GET /releases` (GitHub-shaped JSON).

## Limitations

- Windows only for now (process check and `os.startfile` paths).
- Full-archive updates only, no delta patches.
- The launcher does not update itself yet; download a new launcher build
  manually.
- The launcher trusts the GitHub release channel; it does not implement a
  separate signature scheme beyond HTTPS and the optional asset digest.
