#!/bin/bash
# The package in Chimera's own window (run headless: it needs no display),
# which the headless runner cannot show:
#  - the test program (tests/make-testprog.py) runs in the window with no
#    boot ROM and leaves the memory the headless runner leaves at the same
#    frame, with two keys pressed, at the Lynx's picture size (160x102);
#  - the same turned left (Rotation), at 102x160, with Up read as Right;
#  - the keys the package ships become Chimera's keys for the controls;
#  - two Lynxes linked by the cable run in the window side by side and leave
#    the headless runner's memory;
#  - without its boot ROM a cartridge's project is not opened, and the file
#    is named;
#  - a game of yours (tests/games-local/, the first .lnx) runs with the boot
#    ROM from Chimera's firmware (tests/roms-local/lynxboot.img) and leaves
#    the headless runner's memory.
#
# Usage: tests/run-frontend.sh --chimera-root <chimera checkout> [--frames N] [--keep]
#   --keep   leave the work folder (logs, configurations, projects) and say where it is
# Needs: Chimera built with its window (chimera --headless), and the
# package installed in that checkout (waterbox/build-package.sh -r <chimera>).
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
wb="$root/waterbox"
frames=300
chimera_root=""
keep=""
while [ $# -gt 0 ]; do
	case "$1" in
		--keep) keep=1 ;;
		--chimera-root) chimera_root="$2"; shift ;;
		--frames) frames="$2"; shift ;;
		-*) echo "unknown option: $1" >&2; exit 2 ;;
		*) break ;;
	esac
	shift
done
[ -n "$chimera_root" ] && [ -d "$chimera_root" ] || { echo "say where Chimera is: --chimera-root <chimera checkout>" >&2; exit 2; }
chimera_root="$(cd "$chimera_root" && pwd)"

chimera_exe="$chimera_root/build/meson-linux/source/ui/chimera"
package="$chimera_root/build/Cores/gearlynx.chimeraCore"
runwbx="$root/build/meson-native/run-wbx"
core="$root/build/meson-guest/core.wbx"
runner="$chimera_root/build/meson-linux/chimera-run"
[ -x "$chimera_exe" ] || { echo "Chimera's window is not built: $chimera_exe" >&2; exit 1; }
[ -f "$package" ] || { echo "the package is not installed: $package (run waterbox/build-package.sh -r $chimera_root)" >&2; exit 1; }
[ -x "$runwbx" ] && [ -f "$core" ] || { echo "the sandbox build is not there: $runwbx, $core" >&2; exit 1; }
[ -x "$runner" ] || { echo "Chimera's headless runner is not built: $runner" >&2; exit 1; }

work="$(mktemp -d)"
cleanup() {
	if [ -n "$keep" ]; then echo "work folder kept: $work"; else rm -rf "$work"; fi
}
trap cleanup EXIT

# run-wbx finds miniBox's host library in Chimera's build/dll
export LD_LIBRARY_PATH="$chimera_root/build/dll${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# Chimera's window, run headless: the cores installed in the checkout, its
# settings, cache and recovery files the run's own, and nothing carried over
# from a Chimera installed on this machine
export ALSOFT_DRIVERS=null CHIMERA_CORES_DIR="$chimera_root/build/Cores" CHIMERA_QT_CACHE="$work/qt-cache" CHIMERA_QT_RECOVERY="$work/qt-recovery" \
	CHIMERA_QT_HASHES="$work/qt-file-hashes.txt" CHIMERA_QT_CHIMERA_SETTINGS="$work/no-chimera-config.ini"

ok=0
failed=0
skipped=0
report() {
	printf '%-16s %-5s %s\n' "$1" "$2" "$3"
	case "$2" in PASS) ok=$((ok + 1)) ;; SKIP) skipped=$((skipped + 1)) ;; *) failed=$((failed + 1)) ;; esac
}

# make_project <tag> <frames> <game> [<setting>=<value> | press=...]...
# A project with the game beside it, where Chimera looks, and a movie of
# that many frames over the controls.
make_project() {
	local tag="$1" nframes="$2" gamefile="$3"; shift 3
	local dir="$work/$tag"
	mkdir -p "$dir"
	cp "$gamefile" "$dir/"
	python3 "$here/make-project.py" "$package" "$dir/$tag.chimeraProject" "$nframes" "panel=$work/panel.out" "$@" \
		-- "game=$dir/$(basename "$gamefile")" > "$work/$tag.log" 2>&1
}
# run_frontend <tag> <configuration> <frames> <domain or "">: the project in the window
run_frontend() {
	local tag="$1" cfg="$2" nframes="$3" domain="$4" job="$work/job.$1.txt"
	{
		echo "frames=$nframes"
		echo "out=$work/$tag.ram.bin"
		echo "meta=$work/$tag.meta.txt"
		echo "shot="
		echo "domain=$domain"
	} > "$job"
	( cd "$chimera_root" && MINIHAWK_JOB="$job" timeout 900 "$chimera_exe" --headless \
		"--config=$cfg" "--project=$work/$tag/$tag.chimeraProject" "--lua=$here/frontend-ram.lua" ) >> "$work/$tag.log" 2>&1
	[ -f "$work/$tag.meta.txt" ] && grep -q "^status=OK" "$work/$tag.meta.txt"
}
value() { sed -n "s/^$2=//p" "$work/$1.meta.txt" 2>/dev/null; }

python3 "$here/settings-config.py" "$work/settings.base.json" '{}'

# ---- the test program: needs no file of anyone's
r() { python3 "$here/results.py" "$1" "$2" 2> /dev/null; }
mkdir -p "$work/progs" "$work/probe"
python3 "$here/make-testprog.py" "$work/progs/testprog.o"
cp "$work/progs/testprog.o" "$work/probe/"
printf '{"game":["testprog.o"]}' > "$work/probe/slots"; printf '{}' > "$work/probe/settings"
"$runwbx" "$core" "$work/probe" --frames 1 --list-panel > "$work/panel.out" 2> /dev/null
# the movie is longer than the run: a window whose movie ends stops there;
# P1 A is held from frame 100 (row 99) to the end, and P1 Down from 150
make_project tt $((frames + 100)) "$work/progs/testprog.o" "press=P1 A:99:$frames" "press=P1 Down:149:$frames"
"$runner" --project "$work/tt/tt.chimeraProject" "$package" --files "$work/tt" --frames "$frames" \
	--dump "RAM=$work/engine.ram.bin" > "$work/engine.log" 2>&1
seen="$(r "$work/engine.ram.bin" joySeen)"
if ! run_frontend tt "$work/settings.base.json" "$frames" "RAM"; then
	report "window:program" FAIL "Chimera did not finish the run: $(value tt detail) $(tail -2 "$work/tt.log" | tr '\n' ' ' | cut -c1-200)"
elif [ "$(value tt frames)" != "$frames" ] || [ "$(value tt width)" != 160 ] || [ "$(value tt height)" != 102 ]; then
	report "window:program" FAIL "frames $(value tt frames) of $frames, picture $(value tt width)x$(value tt height) and not 160x102"
elif [ -s "$work/engine.ram.bin" ] && cmp -s "$work/engine.ram.bin" "$work/tt.ram.bin" && [ "$seen" = 65 ]; then
	report "window:program" PASS "the test program ran $frames frames in Chimera's window, 160x102, with P1 A held from frame 100 and P1 Down from 150 (the program saw $seen: Down and A); its memory ($(wc -c < "$work/tt.ram.bin") bytes) is what the headless runner leaves"
else
	report "window:program" FAIL "the memory differs from the headless runner's (or one of the two is missing), or P1 A and Down did not reach the program ($seen)"
fi

# turned left: the picture on its side, and Up read as Right
make_project tl $((frames + 100)) "$work/progs/testprog.o" rotation=left "press=P1 Up:99:$frames"
"$runner" --project "$work/tl/tl.chimeraProject" "$package" --files "$work/tl" --frames "$frames" \
	--dump "RAM=$work/tl-engine.ram.bin" > "$work/tl-engine.log" 2>&1
python3 "$here/settings-config.py" "$work/settings.tl.json" '{"rotation": "left"}'
if ! run_frontend tl "$work/settings.tl.json" "$frames" "RAM"; then
	report "window:turned" FAIL "Chimera did not finish the run: $(value tl detail) $(tail -2 "$work/tl.log" | tr '\n' ' ' | cut -c1-200)"
elif [ "$(value tl width)x$(value tl height)" = 102x160 ] && [ -s "$work/tl-engine.ram.bin" ] && cmp -s "$work/tl-engine.ram.bin" "$work/tl.ram.bin" \
	&& [ "$(r "$work/tl.ram.bin" joySeen)" = 16 ]; then
	report "window:turned" PASS "with Rotation left the picture is $(value tl width)x$(value tl height) in the window and Up reaches the program as Right ($(r "$work/tl.ram.bin" joySeen)); the memory is what the headless runner leaves"
else
	report "window:turned" FAIL "picture $(value tl width)x$(value tl height), joystick $(r "$work/tl.ram.bin" joySeen), or the memory differs from the headless runner's"
fi

# the keys the package ships are the machine's when nobody changed them (the
# window keeps only the keys a person changed; fresh settings have none)
mkdir -p "$work/keys"; cp "$work/tt/testprog.o" "$work/keys/"; python3 -c 'import json, sys, zipfile; z = zipfile.ZipFile(sys.argv[1]); d = json.loads(z.read("project.json")); d["id"] = sys.argv[3]; o = zipfile.ZipFile(sys.argv[2], "w", zipfile.ZIP_DEFLATED); o.writestr("project.json", json.dumps(d, indent="\t")); [o.writestr(n, z.read(n)) for n in z.namelist() if n != "project.json"]; o.close()' "$work/tt/tt.chimeraProject" "$work/keys/keys.chimeraProject" gearlynx-test-keys
printf 'out=%s/keys.controls.json\nmeta=%s/keys.meta.txt\n' "$work" "$work" > "$work/job.keys.txt"
( cd "$chimera_root" && MINIHAWK_JOB="$work/job.keys.txt" timeout 900 "$chimera_exe" --headless \
	"--config=$work/settings.keys.json" "--project=$work/keys/keys.chimeraProject" "--lua=$here/dump-controls.lua" ) >> "$work/keys.log" 2>&1
if [ -f "$work/keys.meta.txt" ] && grep -q "^status=OK" "$work/keys.meta.txt"; then
	if python3 "$here/check-keybinds.py" "$work/keys.controls.json" "$wb/default_keybinds.json" "Atari Lynx Controls" "P1 A" > "$work/keys.txt" 2>&1; then
		report "window:keys" PASS "$(cat "$work/keys.txt")"
	else
		report "window:keys" FAIL "$(head -1 "$work/keys.txt")"
	fi
else
	report "window:keys" FAIL "Chimera did not finish the run: $(tail -2 "$work/keys.log" | tr '\n' ' ' | cut -c1-200)"
fi

# ---- two Lynxes linked: the link programs, P2's A held, the cable pulled
python3 "$here/make-testprog.py" "$work/progs/master.o" link=master
python3 "$here/make-testprog.py" "$work/progs/slave.o" link=slave
mkdir -p "$work/lprobe"
cp "$work/progs/master.o" "$work/progs/slave.o" "$work/lprobe/"
printf '{"game":["master.o"],"game2":["slave.o"]}' > "$work/lprobe/slots"; printf '{"machine":"link2"}' > "$work/lprobe/settings"
"$runwbx" "$core" "$work/lprobe" --frames 1 --list-panel > "$work/panel-link.out" 2> /dev/null
mkdir -p "$work/lk"
cp "$work/progs/master.o" "$work/progs/slave.o" "$work/lk/"
python3 "$here/make-project.py" "$package" "$work/lk/lk.chimeraProject" $((frames + 100)) machine=link2 \
	"press=P2 A:99:$frames" "press=Toggle Link Connection:199:1" "panel=$work/panel-link.out" \
	-- "game=$work/lk/master.o" "game2=$work/lk/slave.o" > "$work/lk.log" 2>&1
"$runner" --project "$work/lk/lk.chimeraProject" "$package" --files "$work/lk" --frames "$frames" \
	--dump "P2 RAM=$work/lk-engine.ram.bin" > "$work/lk-engine.log" 2>&1
if ! run_frontend lk "$work/settings.base.json" "$frames" "P2 RAM"; then
	report "window:link" FAIL "Chimera did not finish the run: $(value lk detail) $(tail -2 "$work/lk.log" | tr '\n' ' ' | cut -c1-200)"
elif [ "$(value lk frames)" = "$frames" ] && [ "$(value lk width)x$(value lk height)" = 320x102 ] && [ -s "$work/lk-engine.ram.bin" ] && cmp -s "$work/lk-engine.ram.bin" "$work/lk.ram.bin" \
	&& [ "$(r "$work/lk.ram.bin" joySeen)" = 1 ]; then
	report "window:link" PASS "two linked Lynxes ran $frames frames in the window, side by side at 320x102, with P2's A held and the cable pulled at frame 200 (P2 saw A, and $(r "$work/lk.ram.bin" linkCount) bytes came in for it); P2's memory is what the headless runner leaves"
else
	report "window:link" FAIL "frames $(value lk frames) of $frames, picture $(value lk width)x$(value lk height), or P2's memory differs from the headless runner's ($(r "$work/lk.ram.bin" joySeen) $(r "$work/lk.ram.bin" linkCount))"
fi

# ---- a game of yours, with the boot ROM from Chimera's firmware
game="$(ls "$root"/tests/games-local/*.lnx 2> /dev/null | head -1)"
bios="$root/tests/roms-local/lynxboot.img"
if [ -z "$game" ] || [ ! -f "$bios" ]; then
	report "window:refusal" SKIP "needs a .lnx game in tests/games-local and lynxboot.img in tests/roms-local"
	report "window:game" SKIP "needs a .lnx game in tests/games-local and lynxboot.img in tests/roms-local"
else
	# the boot ROM lives in a folder of its own, which Chimera is told of only
	# by the configuration of the run that is meant to find it
	fwdir="$(mktemp -d)"
	cp "$bios" "$fwdir/lynxboot.img"
	# the game under a plain name (Chip's Challenge has a quote in its own)
	cp "$game" "$work/game.lnx"
	make_project nobios 100 "$work/game.lnx" "firmware=lynxboot.img=$fwdir/lynxboot.img"
	if run_frontend nobios "$work/settings.base.json" 1 ""; then
		report "window:refusal" FAIL "the project opened without the boot ROM"
	elif grep -q "lynxboot.img" "$work/nobios.log"; then
		report "window:refusal" PASS "without the boot ROM the project is not opened, and the file is named: $(grep -o "[^.]*lynxboot\.img[^.]*" "$work/nobios.log" | head -1 | cut -c1-120)"
	else
		report "window:refusal" FAIL "refused, but the file is not named: $(tail -2 "$work/nobios.log" | tr '\n' ' ' | cut -c1-200)"
	fi
	python3 "$here/settings-config.py" "$work/settings.fw.json" '{}' "{\"lynxboot.img\": \"$fwdir/lynxboot.img\"}"
	gframes=1500
	make_project gm $((gframes + 100)) "$work/game.lnx" "firmware=lynxboot.img=$fwdir/lynxboot.img" "press=P1 Pause:599:5" "press=P1 A:899:5" "press=P1 Pause:1099:5"
	"$runner" --project "$work/gm/gm.chimeraProject" "$package" --files "$work/gm" --firmware "lynxboot.img=$fwdir/lynxboot.img" \
		--frames "$gframes" --dump "RAM=$work/gm-engine.ram.bin" > "$work/gm-engine.log" 2>&1
	if ! run_frontend gm "$work/settings.fw.json" "$gframes" "RAM"; then
		report "window:game" FAIL "Chimera did not finish the run: $(value gm detail) $(tail -2 "$work/gm.log" | tr '\n' ' ' | cut -c1-200)"
	elif [ "$(value gm frames)" = "$gframes" ] && [ -s "$work/gm-engine.ram.bin" ] && cmp -s "$work/gm-engine.ram.bin" "$work/gm.ram.bin"; then
		report "window:game" PASS "a project of $(basename "$game") took the boot ROM from Chimera's firmware, ran $gframes frames in the window with Pause and A pressed, $(value gm width)x$(value gm height), and left the memory the headless runner leaves"
	else
		report "window:game" FAIL "frames $(value gm frames) of $gframes, or the memory differs from the headless runner's"
	fi
	rm -rf "$fwdir"
fi

echo
echo "$ok passed, $failed failed, $skipped skipped"
[ "$failed" -eq 0 ]
