# AGENTS.md - Gearlynx core for Chimera

This repository builds Gearlynx (https://github.com/drhelius/Gearlynx), Ignacio
Sanchez's Atari Lynx emulator, as a core for Chimera
(https://github.com/ToolAssisted-run/chimera), a frontend for tool-assisted
speedruns. It produces one file, `gearlynx.chimeraCore`: Gearlynx compiled to
run inside Chimera's sandbox (miniBox), a small program around it, and the
declarations Chimera reads (buttons, settings, file types, firmware). Its
machine is the Atari Lynx, as a Lynx I or a Lynx II, and link machines of
two to four Lynxes joined by the ComLynx cable.

Read [docs/BUILDING.md](docs/BUILDING.md) before building and
[docs/PLAN.md](docs/PLAN.md) before changing how the core works: it is the
design log, with what was decided, why, and what was measured.

## Layout

- `extern/Gearlynx` - Gearlynx at its release tag `1.2.38`, a submodule.
  Only the files its libretro core builds are compiled. Never edited in
  place: a change is a numbered patch in `patches/`.
- `patches/` - `0001` tells the core when the program reads the controls;
  `0002` reads the cartridge decryption key from the user's boot ROM
  instead of carrying it.
- `meson.build` - the build of both flavors.
- `waterbox/cinterface.cpp` - the program around Gearlynx: it loads the
  game, the boot ROM and the save data, sets the keys, runs the machine to
  the end of a picture, and is what Chimera's engine calls.
  `waterbox/link.cpp` - linked Lynxes, adapted from upstream's libretro
  link cable (GPL-3.0-or-later, like it).
  `waterbox/sd_card_filesystem_none.cpp` - no SD card.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`,
  `package-licenses.json`, `languages/` (26 tables) - what the package
  declares. `pictures/` holds the console's picture.
- `waterbox/run-native.c`, `waterbox/run-wbx.c` - the native runner and the
  sandbox runner; `waterbox/gate-harness.h` is the part they share.
- `waterbox/*.sh` - `apply-patches.sh`, `setup-guest.sh` (configures the
  sandbox build), `build-package.sh`, `run-gate.sh` (the test script).
- `tests/` - `make-testprog.py` (the test program) and `results.py` (reads
  what it left), `run-frontend.sh` (the window test, with its helpers),
  `make-project.py` (writes a project), `check-key-names.py`,
  `bgra-to-png.py`. `roms-local/` and `games-local/` hold the boot ROM and
  games of yours and are ignored by git.
- `build/` - every build output, ignored by git.

## Build and test

```sh
git submodule update --init
meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
sh waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest
sh waterbox/build-package.sh -r <chimera checkout>
CHIMERA_ROOT=<chimera checkout> bash waterbox/run-gate.sh
bash tests/run-frontend.sh --chimera-root <chimera checkout>
<chimera checkout>/build/meson-linux/source/ui/test-packages <folder holding only the package>
```

Run the test scripts before every commit that touches what they cover, and
say in the commit what ran, with the numbers. A new test is broken on
purpose once and seen to fail.

## Rules of the repository

- **Gearlynx's machine, as upstream has it.** The patches tell the core
  things and move a key out of the binary; they change nothing in the
  emulation.
- **The machine is a function of the project.** Gearlynx's random numbers
  are seeded from the Power-on Seed setting, never the clock.
- **The machine's state is its memory.** A savestate is the sandbox's copy
  of the core's memory. Only what the frontend wants now (turbo, the input
  poll callback) lives in memory a state does not carry.
- **The controls are in two places that must agree**: the tables in
  `cinterface.cpp` and the `input` in `waterbox.config`. The test
  `package:controls` compares them; `package:key-names` checks the default
  bindings name keys Chimera knows.
- **Every text a person reads** is a key of `waterbox/languages/*.json`, in
  all 26 languages, in the same commit (Chimera's
  `tools/check-core-texts.py <this checkout>` checks the keys).
- **No boot ROM or game is committed**, uploaded to CI or attached to an
  issue. The boot ROM is the user's; no part of it is carried.
- **Nothing from BizHawk**: no movie, save or other file of BizHawk's, not
  even for a test.
- **Say what was not tried.** A game that was not run has not been shown to
  work.
- **Write in plain language**: commit messages, documents, comments and
  setting descriptions are read by people who have never seen this
  repository. `.md` files are ASCII.
- **Commits**: `type(scope): what changed`; a short body with what changed,
  why, and how it was tested, with the numbers. Stage explicit paths. No
  amending, no force push.

## What is the owner's to decide

Publishing, the roster row in Chimera, moving to another Gearlynx version,
and anything a licence does not plainly allow.
