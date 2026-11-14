# Design log

Dated entries: what was decided, why, and what was measured.

## 2026-11-14: Gearlynx as a core for the Atari Lynx (issue 581, user-decided)

The owner approved building Gearlynx (https://github.com/drhelius/Gearlynx)
as a Chimera core on 2026-11-14, with every system the emulator supports.
Gearlynx emulates one system, the Atari Lynx, in both of its models (the
Lynx and the Lynx II). It is one of six emulators of the same author (the
"Gear" family: Gearboy, Gearsystem, Gearcoleco, Geargrafx, Gearlynx,
Geartowns) built as cores at the same time; each is its own repository.

### Before any code: what the request has to answer (Chimera's docs/new-core.md, section 1)

- **A new core?** Yes. Chimera's Mednafen core also runs the Lynx (Handy's
  emulation, inside Mednafen); Gearlynx is another emulator of it, more
  recent and more exact on Mikey's timing, asked for by the owner. It is not
  a machine an existing core can be taught.
- **Upstream version**: the release tag `1.2.38` (commit `1aa6da6`,
  2026-11-04), the newest release.
- **Licence**: the GNU GPL version 3 or later (`LICENSE` at that commit, and
  the header of every file in `src/`). It allows a modified binary to be
  passed on with its source. The package is under the GPL 3 or later. miniz
  (zip files), compiled in, is MIT.
- **Firmware compiled in**: none whole. `src/no_bios.h` and `src/no_power.h`
  are pictures Gearlynx draws ("no BIOS", "no power"), upstream's own work.
  `src/media.cpp` carries a 51-byte table, the public key that decrypts a
  cartridge's loader; it is the boot ROM's own bytes, found in it at offset
  0x19A. So it is not carried: patch 0002 reads it from the boot ROM the
  user brings (below). The CPU timing tables are tables of the 65C02, not
  ROM. Nothing else in `src/` is a byte table of any size.
- **One thread, one clock?** Yes. Gearlynx runs on one thread and steps the
  machine by cycles. The only clock read is `time(NULL)` in
  `GearlynxCore::Init`, which seeds its random numbers (power-on RAM, the
  processor's registers, where Mikey's timers start); the core seeds them
  again from a setting before the machine is made (`SetRandomSeed`), so
  that read changes nothing. The libretro core's link cable (two to four
  Lynxes) is a feature of its own program and is not built.
- **The picture**: the machine's own. Gearlynx draws the 160x102 screen
  into a buffer, as 32-bit RGBA; the core swaps it to BGRA. When a game is
  played with the Lynx on its side, Gearlynx itself turns the picture
  (102x160) and the joystick with it; Chimera has no declaration for
  turning a picture, and needs none: the core reports the live size with
  `GetVideoWidth/Height`, as Mednafen's core does for the Lynx.
- **No recompiler.**
- **What the user brings**: the Lynx's boot ROM, `lynxboot.img`, 512 bytes,
  for every cartridge (`.lnx`, `.lyx`). A BS93 program (`.o`, the format
  homebrew is sent to a Lynx in) needs none.
- **What a public runner can test**: everything but the boot ROM and
  games. Gearlynx starts a BS93 program itself, without running the boot
  ROM, so a test program of this repository's own (`tests/make-testprog.py`,
  65C02 machine code assembled by the script, MIT) runs on CI. No free
  replacement boot ROM was found, and none is needed for that. Mednafen's
  core tests the Lynx only with the owner's boot ROM and games, outside CI.
- **How big**: 64 KB of RAM, cartridges of up to 2 MB. The memory layout is
  32/4/4/16/64 MB (sbrk, sealed, invisible, plain, mmap) and a state is
  about 870 KB.

No blocker. The route: the files upstream's libretro core builds, a program
of this repository's in place of `libretro.cpp`, and two patches.

### What is built

Upstream's `platforms/libretro/Makefile.common` names the files: the
emulator in `src/` and miniz. Of those, `libretro.cpp`, `libretro_link.cpp`
(the link cable) and `sd_card_filesystem_libretro.cpp` are the libretro
program's and are left out; `waterbox/cinterface.cpp` is the core's, and
`waterbox/sd_card_filesystem_none.cpp` stands in for the SD card that two
kinds of homebrew flash cartridge read (the GameDrive and the El Cheapo SD):
there is none, so they start as with no card inserted. The defines are the
libretro build's (`__LIBRETRO__`, no debugger, no VGM recorder). Nothing of
the desktop program (its window, ImGui, its debugger or its MCP server) is
built, and nothing in the core touches a network.

### The patches

- **0001, the controls read.** Suzy's JOYSTICK and SWITCHES registers call
  `chimera_input_was_read()` (a weak function: a build without it calls
  nothing) when the processor reads them, not when a debugger peeks. That
  is how a lag frame is known, and where a Lua script's input-poll event
  runs.
- **0002, the decryption key from the boot ROM.** See "Firmware compiled
  in". The key is used only for a cartridge with no loader header of its
  own (Gearlynx decrypts it itself), and such a cartridge always has the
  user's boot ROM loaded: the core refuses one without it.

### The program around Gearlynx (`waterbox/cinterface.cpp`)

- **A frame** is one `RunToVBlank`: it ends when timer 2 (the frame timer)
  borrows. A Lynx program sets its own refresh with timers 0 and 2, so the
  frame rate is the program's: the core reports 1,000,000 over the frame's
  length in microseconds after every frame (Chimera asks again after each
  one). Measured: the test program and California Games run at
  1000000/16695 (59.9 a second), Batman Returns and Klax at 1000000/13335
  (75), Lemmings at 1000000/19950 (50.1).
- **The sound**: Gearlynx mixes its four channels to 44,100 samples a second;
  736 or 737 a frame at 59.9.
- **The keys** are levels: each frame every key is let go and the held ones
  pressed again. Gearlynx turns the directions with the picture when a key is
  pressed (and for a left-handed player), so a key pressed under one turn is
  never left held under another.
- **Power** (pressed, not held) starts the console again, as upstream's
  reset does; the cartridge's EEPROM keeps what it holds.
- **The boot ROM**: mounted as `lynxboot.img` and given to Gearlynx before
  the game. With none, a BS93 program gets 512 zero bytes in its place
  (Gearlynx refuses to start with no boot ROM at all, though it never runs
  one for such a program: it copies the program to memory, hides the boot
  ROM from the processor and starts the program); a cartridge is refused,
  naming the file. The test `boot:bs93` shows the stand-in changes nothing:
  the test program gives the same 60 digests with the owner's boot ROM.
- **Save data**: a cartridge's EEPROM, when its header, upstream's list of
  games or the Save Chip setting names one. Export Save Data writes it as
  `<game>.sav`, and a project's `savedata` slot starts the EEPROM from one.
- **Memory**: RAM (64 KB, the whole of it), Cart ROM, and EEPROM when there
  is one; the system bus (the 64 KB the processor sees, read as upstream's
  debugger reads it, changing nothing); the processor's registers.

### Settings

All shape the machine, and a movie needs the same values:

- `model` (Console): automatic (the header's choice, else the Lynx II), Lynx
  I, Lynx II.
- `rotation`: automatic (the header or upstream's list of games), none,
  left, right, upside down.
- `eeprom` (Save Chip): automatic, none, or one of the five chips in 8 or 16
  bits.
- `fastSprites`: upstream's older, faster sprite drawing.
- `randomSeed` (Power-on Seed): what the random numbers start from.

Not offered: upstream's low-pass filter and per-channel volumes (sound
only). Allow Opposite Directions came later (below).

### The boot ROM the owner has

The owner's `lynxboot.img` has SHA-1 DE60F2263851BBE10E5801EF8F6C357A4BC077E6.
The known dump (Mednafen's core and Gearlynx's own CRC check name it) has
E4ED47FAE31693E016B081C6BDA48DA5B70D7CCB. They differ in one byte, at 0x1F9:
the memory map register's address, which a dump made through the bus reads
from the register and not from the ROM. Gearlynx writes its own values over
0x1F8 and 0x1F9 when it loads a boot ROM, so both run the same machine. The
package pins E4ED47...; Chimera shows the owner's as not recognised and uses
it.

### Tests

`waterbox/run-gate.sh` (see the script for what each compares):

- `source:pinned`, `package:fresh`, `package:controls`, `package:key-names`.
- The test program (BS93, no boot ROM): `prog:runs` (298 of 300 frames
  counted, 12 pictures in 12 reports, 160x102, 1000000/16695),
  `prog:colours` (the program's yellow ramp, and no blue: red and blue are
  where Chimera wants them), `prog:buttons` (each key its own bit), `prog:lag` (2 lag frames in 300
  while the program starts, 300 for its twin that never reads),
  `prog:sound`, `prog:power`, `prog:rotation` (102x160, Up read as Right
  turned left and as Left turned right), `prog:model` (the memory map
  register reads 12 on a Lynx I and 252 on a Lynx II), `prog:bus`,
  `prog:save-data`.
- Native against sandbox: `prog:sandbox` (60 digests of memory, picture and
  sound, with keys and Power), `prog:presses-change-the-run`,
  `prog:state-every-frame` (a state saved and loaded before every frame),
  `prog:state-other-box` (the machine moved to a new sandbox half way),
  `prog:state-rewind`, `prog:sandbox-turned`.
- `refused:no-game`, `refused:not-a-game`, `refused:no-boot-rom`,
  `refused:boot-rom-size`.
- With the boot ROM (`tests/roms-local`): `boot:bs93`. With games
  (`tests/games-local`): `game:<file>`, 2,000 frames from the boot ROM with
  Pause and A pressed, native and sandbox the same.
- With Chimera: `engine:project` (a project of the test program with
  presses in Chimera's headless runner, to the sandbox runner's memory),
  `engine:input-polls`, `engine:game`.

`tests/run-frontend.sh`, 5 tests in Chimera's window run headless: the
test program's memory against the headless runner's (`window:program`), the
same turned left (`window:turned`, 102x160, Up read as Right), the
package's keys (`window:keys`), a cartridge refused without its boot ROM
(`window:refusal`), and Batman Returns with the boot ROM from Chimera's
firmware, 1,500 frames, to the headless runner's memory (`window:game`).

Chimera's own checks of the package (`test-packages`): 25 passed.

### Broken on purpose

Each test of the core was seen to fail on a core broken for it (a build of
the native runner with the break, the test script run against it):

- A and B swapped: `prog:buttons` (and `prog:sound`, whose tone is on A).
- Every frame said to read the controls: `prog:lag`.
- Power doing nothing: `prog:power`; Power acting while held, not once:
  `prog:power`.
- The Rotation setting ignored: `prog:rotation`, `prog:sandbox-turned`.
- The Console setting ignored: `prog:model`.
- The save data not loaded: `prog:save-data`.
- The bus reading zeros: `prog:bus`.
- The frame rate reported as 60: `prog:runs`.
- One sample a frame lost: `prog:sound`.
- Red and blue not swapped: `prog:colours`.
- A cartridge let through with no boot ROM: `refused:no-boot-rom`.

Not broken on purpose: the savestate tests (`prog:state-every-frame`,
`prog:state-other-box`, `prog:state-rewind`). Everything of the machine is
in Gearlynx's own memory in the sandbox, and no break that a real mistake
would make was found to try; they are the runners' tests every core here
has.

What the test program does not stand in for (Chimera's docs/gates.md, E):
it never runs the boot ROM, never reads the cartridge through Suzy's
cartridge port, never draws a sprite and never talks to an EEPROM. Those are
shown only by the games, on a machine that has them.

### Games tried

From the owner's collection, each with the owner's boot ROM, 2,000 frames
with Pause and A pressed, native and sandbox the same (`game:` tests).
None was played through. SHA-1 of the files as tried (headered `.lnx`):

| Game | SHA-1 | Picture | Rate |
|---|---|---|---|
| Batman Returns (1992) | 45df55597bae86b9184861d64d1a1d770f1e8c40 | 160x102 | 75 |
| Blue Lightning (1989) | fd4dfa7b8822229c0e0c5b0f6cde608f8f94bb65 | 160x102 | 59.9 |
| California Games (1991) | ae369a7a3f076de1449097d3b926e3ab4dae242c | 160x102 | 59.9 |
| Chip's Challenge (1989) | 01098825a2f9aa6b02ba7b049c8fa2c3136ee288 | 160x102 | 59.9 |
| Gauntlet - The Third Encounter (1990) | 7cf7d08b52784e13d8278d249dcd8975d3714da4 | 102x160 | 59.9 |
| Klax (1990) | 66a04e14e07367a59fb979b25915008ac1602233 | 102x160 | 75 |
| Lemmings (1993) | e61c173116eacecf90d0296c03d8c216be57400c | 160x102 | 50.1 |

The pictures of California Games and Gauntlet at frame 2,000 were looked at:
the right colours, and Gauntlet's turned on its side. The other 100-odd
games of the collection were not tried.

### Not done, or not shown

- The link cable (ComLynx): done later (below).
- The GameDrive and El Cheapo SD cartridges have no SD card here.
- Windows, macOS and aarch64 were not run; CI would build and test aarch64.
- No movie was recorded in the window and replayed in a new process.

## 2026-11-14: Allow Opposite Directions, and the link cable (user-decided)

The owner's decisions on the first round's questions: the decryption key
stays read from the user's boot ROM; the firmware entry keeps one pinned
SHA-1 (Chimera takes one per entry, so the owner's DE60F2... dump is chosen
by hand); the sound-only options stay out. And two things to add.

### Allow Opposite Directions

Upstream's libretro core's "allow up and down" option, off by default as
upstream has it. Off, Up with Down (and Left with Right) never reach the
game together: the direction given last frame wins, else Up (or Left),
upstream's own rule. It changes what the game reads, so it is part of the
machine. Measured (`prog:opposite`): all four held, the test program reads
Up and Left (160) off and all four (240) on; Down held and then Up too,
off, it stays Down (64).

### The link cable (ComLynx), inside the core

The owner: "No need to change this at a chimera level. Each core should be
able to do the replication at the core level and expose this as a single
machine to Chimera." Gearlynx links Lynxes itself: its libretro program
(`platforms/libretro/libretro_link.cpp`) runs two to four `GearlynxCore`s
through callbacks each core's UART calls (a byte started, a break, a sample
of the line). `waterbox/link.cpp` is that code adapted to this core, under
upstream's GPL 3 or later: the Lynxes run one cycle at a time, the one
furthest behind on a shared timeline first, so no UART reads the line
before the others reach that moment; a frame ends with P1's picture. Two
changes: the pictures are 32-bit, and a key pressed for a frame counts from
the frame's start, as on one Lynx (upstream's link takes it a frame later).
Everything lives in the core's memory, which a savestate carries.

The shape is the Gambatte core's Game Boy Link:

- Setting **Machine**: `lynx`, `link2`, `link3`, `link4`. The package has two
  systems, LYNX and LYNXLINK ("Atari Lynx Link"), and a machine for each
  value.
- Slots `game2` to `game4` and `savedata2` to `savedata4`, shown for the
  machines that have that Lynx. One boot ROM serves every Lynx; it is needed
  when any Lynx's game is a cartridge.
- Controls: each Lynx's nine keys ("P2 A"), each one's Power ("P2 Power"),
  and Toggle Link Connection, which pulls the cable out and puts it back. A
  frame is a lag frame when no Lynx read its controls.
- One picture, the screens side by side (320x102 for two, 640x102 for four;
  a turned game 204x160); one sound, the Lynxes' added together. P1's frame
  rate is the machine's.
- Memory, the bus and the registers per Lynx ("P2 RAM", "P2 System Bus",
  "P2 PC"); Export Save Data writes "P2 <game>.sav".
- The other settings (Console, Rotation, Save Chip, Fast Sprites, Allow
  Opposite Directions) are every Lynx's. The Power-on Seed is P1's; P2's is
  that number plus 0x9E3779B9, P3's plus twice that, so two Lynxes do not
  start as one.

### The link test programs

`tests/make-testprog.py link=master|slave|listen` adds Mikey's UART (timer 4
at 1 us reload 1: 62,500 bits a second; SERCTL 0x1D: parity on and even,
open collector). A master sends its frame count's low 7 bits once a frame; a
slave answers each with its own count, top bit set; a listener only counts
what the master sends. Each keeps the last byte that came in for it at 0x8A
and counts them at 0x8B. A Lynx hears its own bytes on the shared line; the
programs leave those out by the top bit.

### Tests added

`waterbox/run-gate.sh`: `prog:opposite`, `refused:link`, `link:controls`
(41 buttons, the same in the three link machines), `link:exchange` (297
bytes each way in 300 frames, 41 counted mod 256), `link:unplugged` (the
cable out at frame 100: 96 bytes and no more; back in at 200: 197),
`link:more` (three and four Lynxes: every one hears the master, 640x102),
`link:players` (each Lynx's keys and Power its own), `link:lag`,
`link:save-data`, `link:sandbox` (two and four Lynxes with keys, the cable
pulled and put back and a Power: native, sandbox, a state saved and loaded
every frame, and a new sandbox half way give the same 60 digests; 1.55 MB a
state with four), `link:state-rewind`, `engine:link` (a project of two linked
Lynxes in Chimera's headless runner, to the sandbox runner's memory).
`tests/run-frontend.sh`: `window:link`, two linked Lynxes in the window.

Broken on purpose, each seen to fail: the direction filter always off
(`prog:opposite`); Toggle Link Connection doing nothing (`link:unplugged`);
every Lynx reading P1's keys (`link:players`); P2's save data from P1's
slot (`link:save-data`); the cable carrying nothing (`link:exchange`,
`link:more`).

### Commercial games through the cable

Each of the owner's 13 games was run as two Lynxes with the same game, the
owner's boot ROM, Pause and A pressed on both (P2 23 frames after P1),
3,000 frames. Natively and in the sandbox the digests are the same for all
13. Counting the bytes each Lynx's UART started (a native build with a
count, not kept): Battle Wheels (P1 7,871, P2 6,969), California Games
(1,645 and 1,832), Checkered Flag (4,579 and 4,608), Gauntlet - The Third
Encounter (44,955 and 11,151), Robo-Squash (10,837 and 10,750), Todd's
Adventure in Slime World (4,505 and 4,482), Warbirds (5,260 and 6,965) and
Xenophobe (5,695 and 3,943) talk through the cable both ways; Batman
Returns, Blue Lightning, Chip's Challenge, Klax and Lemmings send nothing,
as they have no link play. That is how far it was tried: no link game was
played into a two-player match and looked at, and three and four Lynxes
were tried only with the test programs.

SHA-1 of the games added for this (headered `.lnx`): Battle Wheels (1993)
42f8a53fdba0..., Checkered Flag (1991) 848694715135..., Robo-Squash (1990)
a5a320450820..., Todd's Adventure in Slime World (1990) a1914219cc57...,
Warbirds (1990) b9f84b44fd2b..., Xenophobe (1990) 3c9af585a1a8....

## 2026-11-14: the owner's firmware, and games played

The owner's firmware folder has four Lynx boot images. `Atari_LYNX_boot.img`
and `LYNX_boot.img` have the pinned SHA-1, E4ED47FAE31693E016B081C6BDA48DA5B70D7CCB.
`Lynx Boot Image.bin` and `lynxboot.bin` are the DE60F2... dump (one byte
differs, at 0x1F9; see above). The tests now use `Atari_LYNX_boot.img`, copied
to `tests/roms-local/lynxboot.img`.

### Games played past the title

With that boot ROM, 4,000 frames of scripted input (A tapped every 90 frames
from frame 300, then Right and Up in turns with A and B tapped), pictures
looked at every 800 frames, native and sandbox the same 40 digests each:

- California Games (horizontal): into the BMX event; the rider rides the
  course with the clock counting down, and crashes.
- Gauntlet - The Third Encounter (on its side, 102x160): in the first
  dungeon, the hero moving, his life counting down.
- Klax (on its side): in play, tiles caught, score 5 then 10.
- Todd's Adventure in Slime World: in the caves, Todd moving through them.
- Chip's Challenge: stays on the story screen after the title; the script
  does not press what it asks for. Not played.

### Allow Opposite Directions in a game

In Slime World, Left and Right held together from frame 2,000 to 2,600:
with the setting off the run is the same as one holding Left alone (the
filter gives Left), with it on the run differs from that one; on, native and
sandbox are the same.

### Two-player games through the cable

Link machine of two, the same game in each, the same script for P2 23 frames
behind P1, 4,000 frames, native and sandbox the same 40 digests:

- Gauntlet - The Third Encounter: a two-player game. Both heroes are in the
  same dungeon on both screens, with different characters' numbers (speed
  28 and 26).
- Todd's Adventure in Slime World: both Todds are in the same cave on both
  screens.
- Checkered Flag: a two-car race; P1 is in position 2, P2 in position 1.
- California Games: each Lynx rides the BMX course; P2's screen keeps a
  score of its own (50). Whether the two are in one contest was not made
  out.
- Robo-Squash and Xenophobe ran the same natively and in the sandbox; their
  pictures were not looked at.

No match was played to its end, and three or four Lynxes were not tried
with a game.
