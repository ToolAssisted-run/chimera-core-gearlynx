# Building

This core is built on Linux. The result, `core.wbx`, is one file that runs in
Chimera on Linux, Windows and macOS of its CPU.

## What is needed

- GCC 13 or newer (C and C++), meson, ninja, git, python3.
- A checkout of Chimera with miniBox built, with its C++ guest toolchain
  (Chimera's `AGENTS.md`, "Build"). Below, `<miniBox>` is its
  `extern/chimera-common-minibox` folder.

## Gearlynx's source

Gearlynx is a submodule, `extern/Gearlynx`, at its release tag `1.2.38`:

```sh
git submodule update --init
```

The core compiles the files upstream's libretro core builds
(`platforms/libretro/Makefile.common`): the emulator in `src/` and miniz.
Not the libretro program itself (`libretro.cpp`, its link cable and its file
layer), and nothing of the desktop program. `waterbox/cinterface.cpp` is the
core's, and `waterbox/sd_card_filesystem_none.cpp` says there is no SD card.

The changes to Gearlynx are the numbered files in `patches/`;
`waterbox/apply-patches.sh` lays them over the submodule (the build runs it,
and it does nothing to a tree that has them). To change one: edit the files
in `extern/Gearlynx`, build and test, and write the patch from a copy of the
files taken before the edit (`diff -u`), never from `git diff` of a file two
patches touch.

## The two flavors

- `native`: for the host. `run-native` is the reference the tests compare
  the sandbox with.
- `guest`: for miniBox's sandbox. This is `core.wbx`, and `run-wbx` runs it.

```sh
meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
sh waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest
```

Both runners take a work folder that holds what the sandbox would see: the
game under its name, the boot ROM as `lynxboot.img`, a file `slots`
(`{"game":["game.lnx"]}`, with `"savedata":["game.sav"]` for save data) and
a file `settings` (`{}`, or say `{"rotation":"left","model":"lynx1"}`). A
link machine takes `{"machine":"link2"}` (or `link3`, `link4`) and the other
Lynxes' games and save data in the slots `game2` to `game4` and `savedata2`
to `savedata4`.
`waterbox/gate-harness.h` lists the options they share (`--frames`,
`--report`, `--press`, `--inputs`, `--watch`, `--vid-out`, `--av-stream`,
`--dump-domain` (up to 8), `--savedata-out`, `--list-panel`); after a run
they print the frame rate the core reports. `run-wbx` also takes
`--rerecord`, `--session` and `--rewind`. `tests/bgra-to-png.py` turns a
`--vid-out` picture into a PNG.

## The package

```sh
sh waterbox/build-package.sh -r <chimera checkout>
```

builds the sandbox core and writes `build/Cores/gearlynx.chimeraCore` in
that checkout. `tests/make-project.py` writes a project for a game, which
Chimera's `chimera-run --project` then plays.

## The tests

```sh
CHIMERA_ROOT=<chimera checkout> bash waterbox/run-gate.sh
bash tests/run-frontend.sh --chimera-root <chimera checkout>
```

The first runs every test and says, for each, what was compared. Most need
no file of anyone's: `tests/make-testprog.py` writes a test program of this
repository's own, a BS93 program of 65C02 machine code assembled by the
script. It reads the keys, counts frames, draws a line a frame, plays a tone
while A is held and keeps what the memory map register reads. With
`link=master`, `link=slave` or `link=listen` it also talks through the
ComLynx port, for the link tests.
`tests/results.py` reads what it left in the RAM.

The boot ROM test needs `tests/roms-local/lynxboot.img`, and the game tests
any `.lnx` or `.lyx` files in `tests/games-local/`. Both folders are ignored
by git; the tests say SKIP without them.

The second script runs the package in Chimera's window, run headless
(no display is needed); it needs Chimera built with its window
(`tools/ci-chimera.sh`). Its game tests need a `.lnx` game in
`tests/games-local/` and the boot ROM in `tests/roms-local/`.

Chimera's own checks of a package:

```sh
<chimera>/build/meson-linux/source/ui/test-packages <folder holding only the package>
```

## What CI does

`.github/workflows/chimera.yml` builds Chimera and miniBox, the two
flavors and the package, runs both test scripts and Chimera's checks, and
publishes the package as this repository's release. It does all of that
twice, once for each CPU, on a runner of that CPU (x86_64 on ubuntu-24.04,
aarch64 on ubuntu-24.04-arm): a package is machine code for one CPU, and the
guest is built for the CPU it is built on. A runner has no boot ROM and no
game: those tests are skipped there, and the workflow asks for the others by
name.

## Troubleshooting

- `the series does not apply to the submodule's HEAD`: the submodule was
  moved to another commit without the patches being made again for it.
- `extern/Gearlynx is partly patched`: a file of the submodule was changed
  or reverted by hand. The script says how to start again from the
  submodule's commit.
- A package build that stops in a fraction of a second with nothing on the
  screen failed on its standard output: run it again without sending that
  output away.
