# Missile CMD

A missile-defense arcade game for Flipper Zero.

[![Build, release and Pages](https://github.com/LTDev-LLC/Missile-CMD/actions/workflows/release.yml/badge.svg?branch=main)](https://github.com/LTDev-LLC/Missile-CMD/actions/workflows/release.yml?query=branch%3Amain)

| Firmware | Locked SDK |
| --- | --- |
| Official | [1.4.3](https://github.com/flipperdevices/flipperzero-firmware/releases/tag/1.4.3) |
| Unleashed | [unlshd-092](https://github.com/DarkFlippers/unleashed-firmware/releases/tag/unlshd-092) |
| Momentum | [mntm-012](https://github.com/Next-Flip/Momentum-Firmware/releases/tag/mntm-012) |

## Gameplay

- Defend six cities and three batteries against standard, fast, splitting, and evasive missiles.
- Chain explosions and spend credits on repairs, ammunition, and upgrades between waves.
- Face announced attack patterns from wave 10 onward.
- Save and resume runs, or retry a failed wave without affecting records or saves.
- Track high scores and personal records, earn 15 medals, and pin a goal.

## Modes and challenges

| Mode | Rules |
| :--- | :--- |
| Classic | Six-city defense with a fresh seed. |
| Daily | The day's shared seed at Command difficulty, using your device clock. |
| Seeded | Share a challenge with an eight-digit hexadecimal seed. |
| Limited ammo | Five shots per surviving battery each wave. |
| One city | Defend one city; the others cannot be rebuilt. |
| Barrage | Shorter intervals between enemy launches. |
| Perfect defense | Losing any city or battery ends the run. |
| Endless Crisis | Start at wave 10 on Crisis difficulty. |
| Training | Four interactive lessons covering interception, chains, battery selection, and supplies. |
| Practice | Choose the starting wave, enemies, ammo, speed, and aiming aids. No records or saves. |
| Campaign | Five missions with different attack patterns. |
| Score attack | Score as much as possible in three minutes of play; pauses and repairs don't count. |
| Puzzles | Clear nine formations with two shots each and no ground impacts. Optional hints. |
| Two players | Pass the device for matching five-mission attempts. Tied scores draw; no records or saves. |
| Evasive | Marked missiles change their target once during flight. |

## Controls

| Input | Action |
| --- | --- |
| D-pad | Move the cursor or navigate menus. |
| OK | Fire or confirm. |
| Hold OK | Tap Left, Up, or Right to select a battery; Down selects Auto. Release without firing. |
| Back | Pause during a wave; return or continue elsewhere. |
| Long Back | Save-and-return confirmation, or end a Practice/Two players session. |

## Gameplay readouts

Example: `W10 012450 H25000 A09` above the playfield, `T24 L10 C9 R9` below.

| Label | Meaning |
| --- | --- |
| `W10` | Wave 10. |
| `012450` | Current score: 12,450. |
| `H25000` | Mode best, or difficulty best for Classic/Seeded. |
| `A09` | Auto-selected battery with 9 shots. Auto picks the nearest usable battery horizontally. |
| `L09`, `C09`, `R09` | Manually selected Left, Center, or Right battery and its ammo. |
| `A--` | No battery can fire. |
| `T24` | Threats flying or waiting to spawn; splits can increase the count. |
| `L10 C9 R9` | Ammo remaining in each battery; empty or destroyed batteries show zero. |
| `LOW` | Three or fewer shots remain in total. |

## Settings

- Sound, vibration, and vibration intensity.
- Cursor speed, acceleration, and bold crosshair.
- **Controls → Resume timer:** three-second countdown before starting or resuming (default: on).
- Adaptive rendering up to 30 FPS, or Battery Saver at 15 FPS. Both keep simulation at 30 Hz.
- **Display → Invert colors**, reduced flash, missile tails, and control-card preferences.

## LED notifications

| Event | Alert |
| --- | --- |
| Detonation, chain reaction, or rubble impact | One yellow flash |
| City or battery destroyed | Two red flashes |
| Game over | Three red flashes |
| Wave cleared | Two green flashes |

## Screenshots

| Title menu | Run setup | Active gameplay |
| :---: | :---: | :---: |
| ![Title menu](screenshots/title.png) | ![Run setup](screenshots/setup.png) | ![Active gameplay](screenshots/gameplay.png) |
| **Late-wave gameplay** | **Practice aiming** | **Simple HUD** |
| ![Late-wave gameplay](screenshots/gameplay_late.png) | ![Practice aiming](screenshots/practice_aiming.png) | ![Simple HUD](screenshots/simple_hud.png) |
| **Wave complete** | **Repairs** | **Training** |
| ![Wave complete](screenshots/wave_result.png) | ![Repairs](screenshots/workshop.png) | ![Training](screenshots/training.png) |
| **Game over** | **High scores** | **Medals** |
| ![Game over](screenshots/game_over.png) | ![High scores](screenshots/high_scores.png) | ![Medals](screenshots/medals.png) |

## Install Missile CMD

Requires a Flipper Zero with a microSD card and a build matching its firmware.

### USB web installer

1. Open the [web installer](https://LTDev-LLC.github.io/Missile-CMD/) in desktop Chrome or Edge.
2. Connect your Flipper with a USB data cable. Close qFlipper and the game.
3. Select a release and firmware, review compatibility, and choose **Install Missile CMD**.
4. Open **Apps → Games → Missile CMD** on the device.

The installer checks the app and matching help file before finishing. Older app-only
releases remain available; check compatibility yourself if their firmware version is
unrecorded. Detected firmware-family or API-major mismatches block installation.

If an interrupted install leaves `.web-backup` files beside the app or help file,
keep a local copy, then use qFlipper to restore each backup to its original name and retry.

### Manual installation

Download the ZIP for your firmware and copy its `apps` and `apps_data` folders to the
SD-card root. Open **Apps → Games → Missile CMD**.

For standalone downloads, put the FAP in `apps/Games/` and matching `help.bin` in
`apps_data/missile_cmd/<VERSION>/`. The ZIP and installers include help automatically.
The release asset `missile_cmd.fap` is the Official build.

### Version data

Each release stores settings, saves, records, and help in `apps_data/missile_cmd/<VERSION>/`.
When older data exists, choose:

- **Keep:** use this release's data or defaults and leave older folders untouched.
- **Migrate:** carry forward the newest older data, preserve saved values and progress,
  add defaults for new settings, create missing configuration files, and update supported formats.
  Current-release data takes priority; originals are kept.
- **Clean:** review older folders, then confirm **Delete** to remove them. **Cancel** is
  selected by default; current, equivalent, and newer versions are protected.

Keep and successful migrations are remembered. Failed migrations offer **Retry** or
**Keep** and remain retryable at the next launch. Revisit these choices through
**Settings → Data → Version data** from the title menu.

## Development

### Build and install

```sh
python3 -m venv .venv
source .venv/bin/activate
python -m pip install --require-hashes -r requirements-build.txt
make build
```

Official is the default. Choose the firmware installed on your Flipper:

| Firmware | Build |
| --- | --- |
| Official | `make build` |
| Unleashed | `make build FIRMWARE=unleashed` |
| Momentum | `make build FIRMWARE=momentum` |

Builds use the SDKs and checksums in [`build-lock.json`](build-lock.json), uFBT 0.2.6,
and toolchain 39. Downloads and cached SDK files are verified; modified SDKs are restored.
Build commands support macOS and Linux.

| Command | Purpose |
| --- | --- |
| `make build-all` | Build all three firmware targets. |
| `make sdk-status FIRMWARE=unleashed` | Show the installed SDK version and API. |
| `make sdk-update FIRMWARE=unleashed` | Reinstall the locked SDK. |
| `make repro-check FIRMWARE=official` | Compare two clean builds and record their hashes. |

Override the SDK with `SDK_VERSION=<release>` or `SDK_VERSION=latest`; repeat the override
on subsequent commands. Commands without it return to the lock.

SDK caches live in `~/.cache/missile-cmd/ufbt/<firmware>` (`SDK_ROOT` overrides the parent).
The shared toolchain uses `~/.ufbt` or `FBT_TOOLCHAIN_PATH`; project `.env` files must not
override these paths.

Builds produce firmware-specific FAPs and ZIPs in `dist/`; locally, `dist/missile_cmd.fap`
is the most recent build. SDK, checksum, and size reports are in `build_metadata/<firmware>/`.
`VERSION` controls the app and data-folder version; help is generated at `dist/<VERSION>/help.bin`.

Connect a Flipper Zero over USB to build, upload, and launch the matching packed app:

```sh
make install                      # Official
make install FIRMWARE=unleashed
make install FIRMWARE=momentum
```

### Checks

```sh
make test
make test TEST_SANITIZERS='-g -O1 -fno-omit-frame-pointer -fsanitize=address,undefined'
make screenshots-check
make lint
make format
```

## Releases

[`VERSION`](VERSION) sets the SemVer release number. Pushes to `main` run the **Release**
pipeline: tests, builds and lint for all three firmwares, a release when the version
changes, then the Pages installer. Failures block Pages. Branches, pull requests, and
scheduled SDK checks use **Build and test**.

Releases contain firmware-specific FAPs and ZIPs, matching help when available, an Official
`missile_cmd.fap` alias, and `install-manifest.json` with verified versions, paths, and hashes.
The installer uses each release's manifest to install its matching files.

### Publishing the web installer

Set repository **Settings → Pages → Source** to **GitHub Actions**. Pages publishes from
the same checked commit at the end of **Release**. Main pushes and release changes trigger
this pipeline. To refresh existing assets manually, run **Release** on `main` with
**Publish VERSION as a release** unchecked; check it only for an unused version.

Release assets are mirrored into Pages, with the newest stable release selected by default.
No external server or browser GitHub token is needed. **Public Pages makes mirrored binaries
public even if the repository is private.** Private-repository Pages requires a supporting
GitHub plan; source and release-note links still require repository access.

### Local installer development

With Node.js 22+ and an authenticated GitHub CLI:

```sh
make web-test
python3 tools/build_pages.py
python3 -m http.server 8000 --directory build/pages --bind 127.0.0.1
```

Open `http://localhost:8000`. Rebuilding requires an empty `build/pages` directory or a fresh
`--output` path. The site uses plain HTML/CSS/JavaScript. Tests cover streaming, uploads, and
disconnections; browser/firmware compatibility still needs a real USB installation.

## License

Copyright (c) 2026 LTDev LLC. [MIT License](LICENSE). Bundled host-renderer fonts have
[separate licenses](tests/FONT_LICENSES.md).
