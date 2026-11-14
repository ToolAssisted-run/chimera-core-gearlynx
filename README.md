# chimera-core-gearlynx

A core for [Chimera](https://github.com/ToolAssisted-run/chimera), the
frontend for tool-assisted speedruns, for the **Atari Lynx**. It is built on
[Gearlynx](https://github.com/drhelius/Gearlynx), Ignacio Sanchez's Lynx
emulator, and produces one file, `gearlynx.chimeraCore`, that goes in
Chimera's `cores` folder.

## Status

New, and not published yet.

- Both consoles: the Lynx and the Lynx II.
- Games played on their side (Gauntlet, Klax) are shown turned, 102x160,
  with the joystick turned to match.
- Tried: Batman Returns, Blue Lightning, California Games, Chip's
  Challenge, Gauntlet - The Third Encounter, Klax and Lemmings, each for
  2,000 frames from the boot ROM, in the sandbox and natively. No game was
  played through.
- Not there: the link cable (ComLynx), and the SD card of the GameDrive and
  El Cheapo SD homebrew cartridges.

## What you bring

- A game: a cartridge as a `.lnx` file (with its header) or a `.lyx` file
  (without), or a BS93 program (`.o`), the format homebrew is sent to a Lynx
  in.
- The Lynx's boot ROM, `lynxboot.img` (512 bytes, Atari's), for every
  cartridge. A BS93 program needs none.

## Buttons and settings

- **Buttons**: Up, Down, Left, Right, A, B, Option 1, Option 2 and Pause.
  **Power** starts the console again.
- **Console**: automatic (what the game's header asks for, else the Lynx
  II), Lynx I or Lynx II.
- **Rotation**: automatic (the game's header or Gearlynx's list of games),
  none, left, right or upside down.
- **Save Chip**: the EEPROM a cartridge keeps its saved games in;
  automatic, none, or a chip.
- **Fast Sprites**: Gearlynx's older, faster way of drawing sprites.
- **Power-on Seed**: what the console's memory and processor start with.

The frame rate is the game's own: most run at 59.9 frames a second, some at
75 or 50.

Emulator > Export Save Data writes the cartridge's EEPROM as `<game>.sav`;
a project can start from one.

RAM Watch and RAM Search read the Lynx's 64 KB of RAM, the cartridge, the
EEPROM, and the 64 KB the processor sees (the system bus).

## Building and testing

[docs/BUILDING.md](docs/BUILDING.md) has every step. [docs/PLAN.md](docs/PLAN.md)
is the design log: what was decided, why, and what was measured.

## Licence

This repository's own code is MIT. Gearlynx is under the GNU GPL 3 or later,
and the package, which holds it, is under the GPL 3 or later. See
[LICENSE](LICENSE).
