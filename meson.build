# chimera-core-gearlynx: Gearlynx, Ignacio Sanchez's Atari Lynx emulator, in
# miniBox's sandbox, as a Chimera core.
#
# Native reference build (run-native + run-wbx under the build directory):
#     meson setup build/meson-native -Dminibox_dir=<miniBox> && ninja -C build/meson-native
# Guest (core.wbx) build via the cross file waterbox/setup-guest.sh writes
# (needs miniBox's C++ guest toolchain, build/meson-cpp):
#     sh waterbox/setup-guest.sh -m <miniBox> && ninja -C build/meson-guest core.wbx
#
# The files are the ones upstream's libretro core builds
# (platforms/libretro/Makefile.common), without the libretro program itself
# (libretro.cpp, libretro_link.cpp and its file layer): waterbox/cinterface.cpp
# is the core's, waterbox/link.cpp is upstream's link cable adapted to it, and
# waterbox/sd_card_filesystem_none.cpp says there is no SD card. Nothing of the desktop program (its window, debugger or MCP server)
# is built. Both flavours compile the SAME sources; only emulibc.h differs
# (waterbox/native-shim for the reference).
project('chimera-core-gearlynx', ['c', 'cpp'],
  default_options: ['c_std=gnu11', 'cpp_std=gnu++17', 'optimization=2', 'debug=false', 'warning_level=0'])

gl = 'extern/Gearlynx'

# the submodule pin is upstream as it is; the patches are laid over it
# (waterbox/apply-patches.sh does nothing to a tree that has them)
run_command(files('waterbox/apply-patches.sh'), check: true)

mb = get_option('minibox_dir')
if mb == ''
  error('pass -Dminibox_dir=<miniBox checkout>')
endif

# upstream's libretro build's defines: no debugger, no VGM recorder, and the
# libretro flavour of its log (which the core sends to stderr). MINIZ_NO_TIME:
# a zip's dates are never asked of a clock.
defs = ['-DNDEBUG', '-D__LIBRETRO__', '-DGLYNX_DISABLE_DISASSEMBLER',
  '-DGLYNX_DISABLE_VGMRECORDER', '-DMINIZ_NO_TIME']
# one answer from a floating-point expression on every machine (no fused
# multiply-adds: the sound's low-pass filter is floating point)
flags = ['-fwrapv', '-fno-strict-aliasing', '-ffp-contract=off']
add_project_arguments(defs + flags, language: ['c', 'cpp'])

inc = include_directories('waterbox', gl / 'src', gl / 'platforms/libretro',
  gl / 'platforms/shared/dependencies/miniz')

core_sources = files(
  'waterbox/cinterface.cpp',
  'waterbox/sd_card_filesystem_none.cpp',
  'waterbox/link.cpp',
  gl / 'platforms/shared/dependencies/miniz/miniz.c',
  gl / 'src/audio.cpp',
  gl / 'src/bus.cpp',
  gl / 'src/el_cheapo_sd.cpp',
  gl / 'src/gearlynx_core.cpp',
  gl / 'src/game_drive.cpp',
  gl / 'src/input.cpp',
  gl / 'src/m6502.cpp',
  gl / 'src/m6502_functors.cpp',
  gl / 'src/m6502_opcodes.cpp',
  gl / 'src/lcd_screen.cpp',
  gl / 'src/media.cpp',
  gl / 'src/memory.cpp',
  gl / 'src/mikey.cpp',
  gl / 'src/suzy.cpp',
  gl / 'src/eeprom.cpp',
  gl / 'src/trace_logger.cpp',
  gl / 'src/profiler.cpp',
  gl / 'src/vgm_recorder.cpp',
)

if meson.is_cross_build()
  # ---- the guest: core.wbx --------------------------------------------------
  # miniBox's musl and libstdc++ arrive through the cross file. The -u lines
  # name the pthread functions libgcc's unwinder refers to weakly: a weak
  # reference is not enough to pull them out of a static libc, and it would
  # call address zero.
  mbdir = meson.get_external_property('minibox_dir')
  mbuild = meson.get_external_property('minibox_build')
  sr = meson.get_external_property('guest_sysroot')

  executable('core', core_sources,
    name_suffix: 'wbx',
    include_directories: inc,
    link_args: [
      '-static', '-no-pie',
      '-Wl,--eh-frame-hdr', '-Wl,-O2', '-Wl,--no-relax', '-Wl,-z,stack-size=8388608',
      '-T', mbdir / 'source/guest/linkscript.T',
      '-Wl,-u,pthread_once', '-Wl,-u,pthread_cond_wait',
      '-Wl,-u,pthread_cond_broadcast', '-Wl,-u,pthread_key_create',
      '-Wl,-u,pthread_mutexattr_init', '-Wl,-u,pthread_mutexattr_settype',
      '-Wl,-u,pthread_mutexattr_destroy',
      mbuild / 'source/guest/cxxglue.c.o',
      mbuild / 'source/guest/emulibc.c.o',
      '-L' + sr / 'lib', '-lstdc++', '-lgcc', '-lgcc_eh', '-lc'])
else
  # ---- the native reference -------------------------------------------------
  m = meson.get_compiler('c').find_library('m', required: false)
  executable('run-native', core_sources + files('waterbox/run-native.c'),
    include_directories: [include_directories('waterbox/native-shim'), inc],
    c_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    cpp_args: ['-I' + mb / 'source/guest/include', '-I' + mb / 'extern/jsmn'],
    dependencies: [m])

  mbhost = mb / 'build/meson-linux/source/host'
  executable('run-wbx', files('waterbox/run-wbx.c'),
    c_args: ['-I' + mb / 'source/host'],
    link_args: [mbhost / 'libminiboxhost.so'],
    build_rpath: mbhost)
endif
