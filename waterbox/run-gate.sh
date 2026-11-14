#!/bin/bash
# The core's tests. Each line says what was compared. The script ends with a
# count of tests that passed, failed and were skipped, and with an error if
# any failed. A skipped test is not a pass.
#
#   waterbox/run-gate.sh
#
# CHIMERA_ROOT=<a Chimera checkout> adds the tests that run the package in
# Chimera's own headless runner (waterbox/build-package.sh -r <it> first),
# and the check of the default key bindings' names.
#
# Needs the native build (meson setup build/meson-native ... && ninja) and
# the sandboxed core (waterbox/setup-guest.sh && ninja -C build/meson-guest):
# docs/BUILDING.md. And Python 3.
#
# WHAT RUNS WITH NO FILES OF YOURS: everything but the boot ROM and game
# tests. tests/make-testprog.py writes a Lynx program of this repository's
# own (BS93, the format homebrew is sent to a Lynx in), which Gearlynx starts
# without the boot ROM.
#
# WHAT NEEDS YOUR FILES (skipped without them):
#   tests/roms-local/lynxboot.img  the Lynx's boot ROM
#   tests/games-local/             any .lnx or .lyx games (or GEARLYNX_GAMES=<folder>); each runs 2,000
#                                  frames from the boot ROM
#
# The ways a test passes on a broken thing (Chimera's docs/gates.md) that
# these are written against: a machine that did nothing agrees with itself,
# so a run is first checked to have got somewhere; the end of a run hides a
# run that went another way and came back, so runs are compared as a stream
# of digests; and a comparison that cannot fail proves nothing, so each
# "the same" has a run beside it that must come out different.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
native="$root/build/meson-native/run-native"
runwbx="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
chimera_root="${CHIMERA_ROOT:-}"
roms="$root/tests/roms-local"
games="${GEARLYNX_GAMES:-$root/tests/games-local}"

work="$(mktemp -d)"
# GATE_KEEP=1 keeps the work folder (the runs' output and dumps) and says where
if [ -n "${GATE_KEEP:-}" ]; then trap 'echo "work folder kept: $work"' EXIT; else trap 'rm -rf "$work"' EXIT; fi
ok=0; failed=0; skipped=0
report() { # <test> <PASS|FAIL|SKIP> <what>
	printf '%-24s %-6s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; FAIL) failed=$((failed + 1)) ;; *) skipped=$((skipped + 1)) ;; esac
}
# check <test> <what> <condition...>
check() {
	ck_test="$1"; ck_what="$2"; shift 2
	if "$@"; then report "$ck_test" PASS "$ck_what"; else report "$ck_test" FAIL "$ck_what"; fi
}
stream() { grep '^frame' "$1"; }
same() { [ -s "$1" ] && [ -s "$2" ] && cmp -s "$1" "$2"; }
differ() { [ -s "$1" ] && [ -s "$2" ] && ! cmp -s "$1" "$2"; }
# field <file> <name>: the value after a name in the file's last report line
field() { stream "$1" | tail -1 | awk -v f="$2" '{for (i = 1; i <= NF; i++) if ($i == f) { print $(i + 1); exit }}'; }
pictures() { stream "$1" | awk '{print $7}' | sort -u | wc -l; }
sounds() { stream "$1" | awk '{print $10}' | sort -u | wc -l; }
size() { stream "$1" | tail -1 | awk '{print $6}'; }
rate() { sed -n 's/^vsync //p' "$1"; }
# r <RAM dump> <name>: what the test program left in the Lynx's memory
r() { python3 "$root/tests/results.py" "$1" "$2" 2> /dev/null; }
# folder <name> <settings json> <game> [<save data>] [<firmware>...]: a work
# folder as the sandbox mounts a project - every file under its name
folder() {
	fd_name="$1"; fd_settings="$2"; fd_game="$3"; fd_save="${4:-}"
	shift 3; [ $# -gt 0 ] && shift
	rm -rf "${work:?}/$fd_name"; mkdir -p "$work/$fd_name"
	cp "$fd_game" "$work/$fd_name/$(basename "$fd_game")"
	if [ -n "$fd_save" ]; then
		cp "$fd_save" "$work/$fd_name/$(basename "$fd_save")"
		printf '{"game":["%s"],"savedata":["%s"]}' "$(basename "$fd_game")" "$(basename "$fd_save")" > "$work/$fd_name/slots"
	else
		printf '{"game":["%s"]}' "$(basename "$fd_game")" > "$work/$fd_name/slots"
	fi
	for fw in "$@"; do cp "$fw" "$work/$fd_name/"; done
	printf '%s' "$fd_settings" > "$work/$fd_name/settings"
}
nat() { nt_out="$1"; nt_dir="$2"; shift 2; "$native" "$work/$nt_dir" "$@" > "$work/$nt_out.out" 2> "$work/$nt_out.err"; }
box() { bx_out="$1"; bx_dir="$2"; shift 2; "$runwbx" "$core" "$work/$bx_dir" "$@" > "$work/$bx_out.out" 2> "$work/$bx_out.err"; }
# index <folder> <control>: the control's place in the panel
index() { "$native" "$work/$1" --frames 1 --list-panel 2> /dev/null | grep "'$2'" | head -1 | awk '{print $2}'; }
# repeats <file> <first> <second> <count> <step>: do the report lines from
# frame <second> on repeat those from frame <first> on (run-wbx --rewind)
repeats() {
	awk -v a="$2" -v b="$3" -v n="$4" -v step="$5" '
		/^frame/ { f = $2; $1 = ""; $2 = ""; sub(/ lag [0-9]+$/, ""); line[f] = $0 }
		END {
			for (i = step; i < n; i += step) {
				if (!((a + i) in line) || !((b + i) in line) || line[a + i] != line[b + i]) exit 1
				seen++
			}
			exit seen > 0 ? 0 : 1
		}' "$1"
}

if [ ! -x "$native" ] || [ ! -x "$runwbx" ] || [ ! -f "$core" ]; then
	echo "not built: this needs build/meson-native/run-native and run-wbx, and build/meson-guest/core.wbx (docs/BUILDING.md)" >&2
	exit 1
fi

# ------------------------------------------------- the source and the package
patches="$(ls "$root"/patches/*.patch 2> /dev/null | wc -l)"
check "source:pinned" "extern/Gearlynx is the pinned commit, with the $patches patches of patches/" \
	sh -c "sh '$here/apply-patches.sh' > '$work/patches.log' 2>&1 && [ \"\$(git -C '$root' ls-files -s extern/Gearlynx | awk '{print \$2}')\" = \"\$(git -C '$root/extern/Gearlynx' rev-parse HEAD)\" ]"

stale=""
for src in "$here"/cinterface.cpp "$here"/sd_card_filesystem_none.cpp "$root"/meson.build "$root"/patches/*.patch; do
	[ -e "$src" ] || continue
	[ "$core" -nt "$src" ] || stale="$stale $(basename "$src")"
	[ "$native" -nt "$src" ] || stale="$stale $(basename "$src")(native)"
done
check "package:fresh" "core.wbx and run-native are not older than their sources${stale:+ (older than:$stale)}" [ -z "$stale" ]

# ------------------------------------------------- the test program
progs="$work/progs"
mkdir -p "$progs"
python3 "$root/tests/make-testprog.py" "$progs/testprog.o"
python3 "$root/tests/make-testprog.py" "$progs/nopad.o" nopad
folder t '{}' "$progs/testprog.o"
folder q '{}' "$progs/nopad.o"

"$native" "$work/t" --frames 1 --list-panel > "$work/panel.out" 2> /dev/null
check "package:controls" "the package declares the core's own $(grep -c '^panel' "$work/panel.out") buttons, in the core's order, every one live" \
	python3 - "$here/waterbox.config" "$work/panel.out" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
lines = [l for l in open(sys.argv[2]).read().splitlines() if l.startswith("panel ")]
names = [l[l.index("'") + 1:l.rindex("'")] for l in lines]
live = all(l.endswith("active") for l in lines)
sys.exit(0 if names and live and cfg["input"]["buttons"] == names else 1)
PY

if [ -n "$chimera_root" ] && [ -f "$chimera_root/source/ui/qt/key_codes.inc" ]; then
	check "package:key-names" "every host key the package's default bindings name is a key Chimera knows: $(python3 "$root/tests/check-key-names.py" "$chimera_root" "$here/default_keybinds.json" 2>&1)" \
		sh -c "python3 '$root/tests/check-key-names.py' '$chimera_root' '$here/default_keybinds.json' > /dev/null 2>&1"
else
	report "package:key-names" SKIP "needs CHIMERA_ROOT, whose source names the keys Chimera knows"
fi

nat idle t --frames 300 --report 25 --dump-domain RAM "$work/idle.ram" --vid-out "$work/idle.vid"
check "prog:runs" "the test program runs with no boot ROM: it counted $(r "$work/idle.ram" frames) frames in 300, with $(pictures "$work/idle.out") different pictures in 12 reports, at $(size "$work/idle.out"), $(rate "$work/idle.out") frames a second (the rate the program's timers set)" \
	sh -c "[ '$(r "$work/idle.ram" frames)' -ge 295 ] && [ $(pictures "$work/idle.out") -ge 12 ] && [ '$(size "$work/idle.out")' = 160x102 ] && [ '$(rate "$work/idle.out")' = 1000000/16695 ]"

# The program's colours 3 to 15 are green n and red n with no blue (its
# palette registers): a picture whose red and blue are swapped has blue
colours="$(python3 - "$work/idle.vid" <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
head, _, px = data.partition(b"\n")
seen = {(px[i], px[i + 1], px[i + 2]) for i in range(0, len(px), 4)}
grey = sorted(g for b, g, r in seen if b == 0 and g == r and g)
blue = [c for c in seen if c[0]]
print("%d %d" % (len(grey), len(blue)))
PY
)"
check "prog:colours" "the picture has the program's own colours: $(echo "$colours" | cut -d' ' -f1) of its yellow ramp (green and red the same, no blue), and $(echo "$colours" | cut -d' ' -f2) colours with blue in them" \
	sh -c "[ '$(echo "$colours" | cut -d' ' -f1)' -ge 10 ] && [ '$(echo "$colours" | cut -d' ' -f2)' = 0 ]"

# each key held alone from frame 60; what the program then read
: > "$work/buttons.txt"
for ((b = 0; b < 9; b++)); do
	"$native" "$work/t" --frames 100 --press "$b":60:10 --dump-domain RAM "$work/eb.ram" > /dev/null 2>&1
	echo "$b $(r "$work/eb.ram" joySeen)/$(r "$work/eb.ram" switchesSeen)" >> "$work/buttons.txt"
done
# the joystick's bits: Up 128, Down 64, Left 32, Right 16, Option 1 8,
# Option 2 4, B 2, A 1; Pause is the switches' 1
check "prog:buttons" "each of the Lynx's 9 keys, held alone, is the Lynx's own bit for it and no other ($(awk '{printf "%s ", $2}' "$work/buttons.txt"))" \
	[ "$(awk '{printf "%s ", $2}' "$work/buttons.txt")" = "128/0 64/0 32/0 16/0 1/0 2/0 8/0 4/0 0/1 " ]

nat lag q --frames 300 --report 100
check "prog:lag" "a frame is a lag frame when the program did not read the controls: the program that reads them every frame has $(field "$work/idle.out" lag) in 300 (the two before it starts its loop), its twin that never does $(field "$work/lag.out" lag)" \
	[ "$(field "$work/idle.out" lag) $(field "$work/lag.out" lag)" = "2 300" ]

mkdir -p "$work/snd-av" "$work/idle-av"
nat snd t --frames 600 --report 5 --press 4:100:200 --av-stream "$work/snd-av"
nat idleav t --frames 600 --report 5 --av-stream "$work/idle-av"
total="$(( $(wc -c < "$work/snd-av/audio.raw") / 4 ))"
# 44,100 a second at 1,000,000/16,695 frames a second: 736.25 a frame, once
# the program has set its timers (the first frames are cut short)
want="$(( 600 * 44100 * 16695 / 1000000 ))"
perframe="$(grep -o 'aud [0-9]*' "$work/snd.out" | awk '{print $2}' | sort -u | tr '\n' ' ')"
loud="$(python3 -c "import sys, struct; d = open(sys.argv[1], 'rb').read(); s = struct.unpack('<%dh' % (len(d) // 2), d); print(sum(1 for v in s if v))" "$work/snd-av/audio.raw" 2> /dev/null)"
quiet="$(python3 -c "import sys, struct; d = open(sys.argv[1], 'rb').read(); s = struct.unpack('<%dh' % (len(d) // 2), d); print(sum(1 for v in s if v))" "$work/idle-av/audio.raw" 2> /dev/null)"
check "prog:sound" "$total sound samples in 600 frames: 44,100 a second at the program's frame rate is $want, and every frame reported has 736 or 737 ($perframe); the first two frames are short. With A held the program plays a tone ($loud samples not silent, against $quiet with nothing held)" \
	sh -c "[ $((want - total)) -ge 0 ] && [ $((want - total)) -le 1474 ] && [ '$perframe' = '736 737 ' ] && [ '${loud:-0}' -gt 10000 ] && [ '${quiet:-1}' = 0 ] && [ $(sounds "$work/snd.out") -ge 3 ]"

pw="$(index t Power)"
nat power t --frames 300 --press "$pw":100:1 --dump-domain RAM "$work/power.ram"
nat power2 t --frames 300 --press "$pw":100:3 --dump-domain RAM "$work/power2.ram"
check "prog:power" "Power starts the console again: the program counted $(r "$work/power.ram" frames) frames after it (of 200), and the same held for 3 frames ($(r "$work/power2.ram" frames)): a press, not a level" \
	sh -c "[ '$(r "$work/power.ram" frames)' -ge 195 ] && [ '$(r "$work/power.ram" frames)' -le 200 ] && [ '$(r "$work/power2.ram" frames)' = '$(r "$work/power.ram" frames)' ]"

folder rl '{"rotation":"left"}' "$progs/testprog.o"
folder rr '{"rotation":"right"}' "$progs/testprog.o"
nat rotl rl --frames 100 --report 100 --press 0:50:10 --dump-domain RAM "$work/rotl.ram"
nat rotr rr --frames 100 --report 100 --press 0:50:10 --dump-domain RAM "$work/rotr.ram"
check "prog:rotation" "Rotation turns the picture ($(size "$work/rotl.out") turned left, $(size "$work/rotr.out") right) and the joystick with it: Up pressed reads as Right turned left ($(r "$work/rotl.ram" joySeen)) and as Left turned right ($(r "$work/rotr.ram" joySeen))" \
	sh -c "[ '$(size "$work/rotl.out") $(size "$work/rotr.out")' = '102x160 102x160' ] && [ '$(r "$work/rotl.ram" joySeen) $(r "$work/rotr.ram" joySeen)' = '16 32' ]"

folder m1 '{"model":"lynx1"}' "$progs/testprog.o"
folder m2 '{"model":"lynx2"}' "$progs/testprog.o"
nat m1 m1 --frames 10 --dump-domain RAM "$work/m1.ram"
nat m2 m2 --frames 10 --dump-domain RAM "$work/m2.ram"
check "prog:model" "Console reaches the machine: the memory map register reads $(r "$work/m1.ram" mapctl) on a Lynx I and $(r "$work/m2.ram" mapctl) on a Lynx II (its top bits read back differently), and the default is the Lynx II ($(r "$work/idle.ram" mapctl))" \
	[ "$(r "$work/m1.ram" mapctl) $(r "$work/m2.ram" mapctl) $(r "$work/idle.ram" mapctl)" = "12 252 252" ]

nat bus t --frames 300 --report 25 --dump-domain bus:128:8 "$work/bus.bin"
head -c 136 "$work/idle.ram" | tail -c 8 > "$work/idle8.bin"
stream "$work/bus.out" > "$work/bus.stream"; stream "$work/idle.out" > "$work/idle.stream"
check "prog:bus" "the system bus at 0080 holds what the RAM does ($(od -An -tx1 -N8 "$work/bus.bin" | tr -d ' ')), and reading it changes nothing in the run" \
	sh -c "cmp -s '$work/bus.bin' '$work/idle8.bin' && cmp -s '$work/bus.stream' '$work/idle.stream'"

# the save data: a cartridge's EEPROM (here forced on: the test program has
# no header to name one), taken from a project and exported again
python3 -c "import sys; open(sys.argv[1], 'wb').write(bytes((i * 7 + 3) & 255 for i in range(128)))" "$progs/testprog.sav"
folder sv '{"eeprom":"93c46_16"}' "$progs/testprog.o" "$progs/testprog.sav"
mkdir -p "$work/sv-out" "$work/none-out"
nat sv sv --frames 60 --savedata-out "$work/sv-out" --dump-domain EEPROM "$work/sv.eeprom"
nat svnone t --frames 60 --savedata-out "$work/none-out"
check "prog:save-data" "a 93C46 EEPROM (128 bytes) starts from the project's save data and Export Save Data writes it back as testprog.sav ($(wc -c < "$work/sv-out/testprog.sav" 2> /dev/null) bytes, the same); a game with no EEPROM exports nothing ($(ls "$work/none-out" | wc -l) files)" \
	sh -c "cmp -s '$work/sv-out/testprog.sav' '$progs/testprog.sav' && cmp -s '$work/sv.eeprom' '$progs/testprog.sav' && [ \$(ls '$work/none-out' | wc -l) = 0 ]"

# ------------------------------------------------- native and sandbox, states
allrun="--frames 600 --report 10 --press 4:50:40 --press 0:80:30 --press 6:200:5 --press 8:280:40 --press 2:330:60 --press $pw:400:1"
nat all t $allrun
nat idle10 t --frames 600 --report 10
box all-box t $allrun
box all-rerecord t $allrun --rerecord
box all-session t $allrun --session
for n in all all-box all-rerecord all-session idle10; do stream "$work/$n.out" > "$work/$n.stream"; done
check "prog:sandbox" "native and sandbox print the same $(wc -l < "$work/all.stream") digests of memory, picture and sound, with keys pressed and Power" \
	same "$work/all.stream" "$work/all-box.stream"
check "prog:presses-change-the-run" "the run with presses differs from the idle one" differ "$work/all.stream" "$work/idle10.stream"
check "prog:state-every-frame" "a state saved and loaded every frame changes nothing ($(grep -o 'stateBytes=[0-9]*' "$work/all-rerecord.err" | head -1 | sed 's/stateBytes=//') bytes a state)" same "$work/all-box.stream" "$work/all-rerecord.stream"
check "prog:state-other-box" "a state moved to a new sandbox half way changes nothing" same "$work/all-box.stream" "$work/all-session.stream"
box rewind t --frames 1600 --report 5 --rewind
check "prog:state-rewind" "a state kept at frame 800 and loaded at 1200: the frames after the load repeat those after the save" \
	repeats "$work/rewind.out" 800 1200 380 5
folder rot '{"rotation":"left","model":"lynx1"}' "$progs/testprog.o"
nat s-rot rot $allrun
box s-rot-box rot $allrun
box s-rot-rr rot $allrun --rerecord
for n in s-rot s-rot-box s-rot-rr; do stream "$work/$n.out" > "$work/$n.stream"; done
check "prog:sandbox-turned" "turned left on a Lynx I too, native, sandbox and a state saved and loaded every frame print the same $(wc -l < "$work/s-rot.stream") digests" \
	sh -c "cmp -s '$work/s-rot.stream' '$work/s-rot-box.stream' && cmp -s '$work/s-rot.stream' '$work/s-rot-rr.stream' && [ -s '$work/s-rot.stream' ]"

# ------------------------------------------------- refused, and why
mkdir -p "$work/none"; printf '{}' > "$work/none/settings"
nat none none --frames 1
check "refused:no-game" "a machine with no game is refused, and says so" grep -q "Init failed.*error.no_file" "$work/none.out" "$work/none.err"
printf 'not a game' > "$work/junk.lnx"
folder junk '{}' "$work/junk.lnx"
nat junk junk --frames 1
check "refused:not-a-game" "a file too short to be a game is refused, and says so" grep -q "Init failed.*error.too_short" "$work/junk.out" "$work/junk.err"
python3 -c "import sys; open(sys.argv[1], 'wb').write(bytes((i * 13 + 1) & 255 for i in range(131072)))" "$work/cart.lyx"
folder nobios '{}' "$work/cart.lyx"
nat nobios nobios --frames 1
check "refused:no-boot-rom" "a cartridge with no boot ROM given is refused, and says which file it needs" grep -q "Init failed.*error.boot_rom.lynxboot.img" "$work/nobios.out" "$work/nobios.err"
head -c 256 "$work/cart.lyx" > "$work/lynxboot.img"
folder badbios '{}' "$work/cart.lyx" "" "$work/lynxboot.img"
nat badbios badbios --frames 1
check "refused:boot-rom-size" "a boot ROM of the wrong size is refused, and says its size" grep -q "Init failed.*error.boot_rom_size.256" "$work/badbios.out" "$work/badbios.err"

# ------------------------------------------------- with the boot ROM
if [ -f "$roms/lynxboot.img" ]; then
	folder b '{}' "$progs/testprog.o" "" "$roms/lynxboot.img"
	nat b-all b $allrun
	box b-box b $allrun
	stream "$work/b-all.out" > "$work/b-all.stream"; stream "$work/b-box.out" > "$work/b-box.stream"
	check "boot:bs93" "a BS93 program never runs the boot ROM: with tests/roms-local/lynxboot.img given ($(sha1sum < "$roms/lynxboot.img" | cut -c1-12)...) it prints the same $(wc -l < "$work/b-all.stream") digests as with none, native and sandbox" \
		sh -c "cmp -s '$work/b-all.stream' '$work/all.stream' && cmp -s '$work/b-all.stream' '$work/b-box.stream'"
else
	report "boot:bs93" SKIP "no lynxboot.img in tests/roms-local"
fi

# ------------------------------------------------- the games of yours
found=0
for g in "$games"/*.lnx "$games"/*.lyx; do
	[ -f "$g" ] || continue
	found=1
	n="$(basename "$g")"
	# the game under a name a shell quotes safely (Chip's Challenge)
	safe="$work/$(printf '%s' "$n" | tr -c 'A-Za-z0-9.\n' '_')"
	cp "$g" "$safe"
	if [ ! -f "$roms/lynxboot.img" ]; then
		report "game:$n" SKIP "no lynxboot.img in tests/roms-local"
		continue
	fi
	sn="$(basename "$safe")"
	folder "g-$sn" '{}' "$safe" "" "$roms/lynxboot.img"
	gp="--frames 2000 --report 100 --press 8:600:5 --press 4:900:5 --press 4:1100:5 --press 8:1300:5 --press 4:1500:5 --press 3:1600:200"
	nat "g-$sn" "g-$sn" $gp
	box "g-$sn-box" "g-$sn" $gp
	stream "$work/g-$sn.out" > "$work/g-$sn.stream"; stream "$work/g-$sn-box.out" > "$work/g-$sn-box.stream"
	check "game:$n" "runs 2,000 frames from the boot ROM, Pause and A pressed: $(pictures "$work/g-$sn.out") different pictures and $(sounds "$work/g-$sn.out") sounds in 20 reports, at $(size "$work/g-$sn.out") and $(rate "$work/g-$sn.out") frames a second; the sandbox gives the native build's digests" \
		sh -c "[ \$(wc -l < '$work/g-$sn.stream') = 20 ] && [ $(pictures "$work/g-$sn.out") -ge 5 ] && cmp -s '$work/g-$sn.stream' '$work/g-$sn-box.stream'"
done
[ "$found" = 1 ] || report "game:*" SKIP "no .lnx or .lyx game in tests/games-local"

# ------------------------------------------------- Chimera's engine
runner="$chimera_root/build/dll/chimera-run"
package="$chimera_root/build/Cores/gearlynx.chimeraCore"
if [ -n "$chimera_root" ] && [ -x "$runner" ] && [ -f "$package" ]; then
	mkdir -p "$work/proj"
	cp "$progs/testprog.o" "$work/proj/"
	python3 "$root/tests/make-project.py" "$package" "$work/proj/test.chimeraProject" 300 \
		"press=P1 A:199:101" "press=P1 Down:149:151" panel="$work/panel.out" -- game="$work/proj/testprog.o" > /dev/null
	"$runner" --project "$work/proj/test.chimeraProject" "$package" --files "$work/proj" --frames 300 \
		--dump "RAM=$work/engine.ram" > "$work/engine.log" 2>&1
	box eng t --frames 300 --press "$(index t "P1 A")":200:101 --press "$(index t "P1 Down")":150:151 --dump-domain RAM "$work/eng.ram"
	check "engine:project" "a project of the test program with presses plays in Chimera's headless runner to the sandbox runner's memory ($(wc -c < "$work/eng.ram" 2> /dev/null) bytes; the program read the keys as $(r "$work/engine.ram" joySeen), Down and A)" \
		sh -c "cmp -s '$work/engine.ram' '$work/eng.ram' && [ '$(r "$work/engine.ram" joySeen)' = 65 ]"
	# each read of the controls is reported to Chimera as it happens, so a Lua
	# script's event.oninputpoll runs there (chimera#344): every frame the
	# program read them has at least one, and counting them moves nothing
	"$runner" --project "$work/proj/test.chimeraProject" "$package" --files "$work/proj" --frames 300 --count-input-polls \
		--dump "RAM=$work/polls.ram" --meta "$work/polls.meta" > "$work/polls.log" 2>&1
	polls=$(sed -n 's/^inputPolls=//p' "$work/polls.meta" 2> /dev/null)
	lags=$(sed -n 's/^lagFrames=//p' "$work/polls.meta" 2> /dev/null)
	check "engine:input-polls" "the program's reads of the controls reach Chimera as they happen: ${polls:-none} in 300 frames, ${lags:-?} of them lag frames; the memory is the run's without counting" \
		sh -c "[ '${polls:-none}' != none ] && [ '${polls:-0}' -ge $(( 300 - ${lags:-300} )) ] && [ '${lags:-300}' -lt 300 ] && cmp -s '$work/polls.ram' '$work/engine.ram'"
else
	report "engine:project" SKIP "needs CHIMERA_ROOT with build/dll/chimera-run and build/Cores/gearlynx.chimeraCore"
	report "engine:input-polls" SKIP "needs CHIMERA_ROOT with build/dll/chimera-run and build/Cores/gearlynx.chimeraCore"
fi
if [ -n "$chimera_root" ] && [ -x "$runner" ] && [ -f "$package" ] && [ -f "$roms/lynxboot.img" ]; then
	g="$(ls "$games"/*.lnx 2> /dev/null | head -1)"
	if [ -n "$g" ]; then
		mkdir -p "$work/gproj"
		cp "$g" "$work/gproj/game.lnx"
		python3 "$root/tests/make-project.py" "$package" "$work/gproj/game.chimeraProject" 1500 \
			"press=P1 Pause:599:5" "press=P1 A:899:5" panel="$work/panel.out" firmware=lynxboot.img="$roms/lynxboot.img" \
			-- game="$work/gproj/game.lnx" > /dev/null
		"$runner" --project "$work/gproj/game.chimeraProject" "$package" --files "$work/gproj" --frames 1500 --firmware "lynxboot.img=$roms/lynxboot.img" \
			--dump "RAM=$work/egame.ram" > "$work/egame.log" 2>&1
		folder eg '{}' "$work/gproj/game.lnx" "" "$roms/lynxboot.img"
		box egame eg --frames 1500 --press 8:600:5 --press 4:900:5 --dump-domain RAM "$work/egame-box.ram"
		check "engine:game" "a project of $(basename "$g") with the boot ROM from Chimera's firmware plays 1,500 frames in Chimera's headless runner to the sandbox runner's memory" \
			sh -c "[ -s '$work/egame.ram' ] && cmp -s '$work/egame.ram' '$work/egame-box.ram'"
	else
		report "engine:game" SKIP "no .lnx game in tests/games-local"
	fi
else
	report "engine:game" SKIP "needs CHIMERA_ROOT with the package, and lynxboot.img in tests/roms-local"
fi

echo
echo "$ok passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
