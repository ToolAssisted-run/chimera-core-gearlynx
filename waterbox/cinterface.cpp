/* cinterface.cpp - Gearlynx behind the Chimera guest ABI.
 *
 * Gearlynx (Ignacio Sanchez, extern/Gearlynx) is an Atari Lynx: a 65C02,
 * Mikey (timers, sound, the screen's DMA) and Suzy (the sprite engine, the
 * cartridge, the controls). This file is what upstream's libretro core
 * (platforms/libretro/libretro.cpp) is to RetroArch: it makes the machine,
 * feeds it a frame's controls, runs it to the end of a picture and hands out
 * the picture, the sound and the memory. It compiles the same for the sandbox
 * and for the native reference (native-shim/emulibc.h), which is what makes
 * the comparison of the two a proof.
 *
 * SPDX-License-Identifier: MIT
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "gearlynx.h"

/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (Chimera's docs/translations.md). */
#define TEXT_ARG "\x1f"

/* ---- upstream's log ----------------------------------------------------------
 * Built as upstream's libretro core is (__LIBRETRO__), Gearlynx logs through
 * a libretro log function. Its notes go nowhere; its errors go to stderr,
 * never to stdout, which the test runners keep for their reports. */
static void RETRO_CALLCONV gearlynx_log(enum retro_log_level level, const char *fmt, ...)
{
	(void)level;
	char line[600];
	va_list args;
	va_start(args, fmt);
	vsnprintf(line, sizeof line, fmt, args);
	va_end(args);
	if (!strncmp(line, "ERROR", 5))
		fprintf(stderr, "[gearlynx] %s", line);
}
retro_log_printf_t log_cb = gearlynx_log;

static char g_loadError[512];
static GearlynxCore *g_core;
static char g_gameName[256];

/* ---- the controls --------------------------------------------------------------
 * The order of waterbox.config's "buttons": the Lynx's nine keys, then Power. */
enum
{
	kUp, kDown, kLeft, kRight, kA, kB, kOption1, kOption2, kPause, kPower, kButtons
};
static const char *const kButtonNames[kButtons] = {
	"P1 Up", "P1 Down", "P1 Left", "P1 Right", "P1 A", "P1 B",
	"P1 Option 1", "P1 Option 2", "P1 Pause", "Power",
};
static const GLYNX_Keys kKeys[kPower] = {
	GLYNX_KEY_UP, GLYNX_KEY_DOWN, GLYNX_KEY_LEFT, GLYNX_KEY_RIGHT, GLYNX_KEY_A,
	GLYNX_KEY_B, GLYNX_KEY_OPTION1, GLYNX_KEY_OPTION2, GLYNX_KEY_PAUSE,
};
/* Two channels reach a core: a controller of 64 buttons or fewer arrives as
 * a packed mask in FrameAdvance, and the test runners use SetButton. A frame
 * is their union, each keeping its own state. */
static bool g_set[kButtons];
static bool g_held[kButtons];
static bool g_powerWasHeld;

/* lag: did the program read JOYSTICK or SWITCHES this frame (patches/0001) */
static bool g_inputRead;

/* Chimera's function to call at each read of the controls, so a Lua script's
 * event.oninputpoll runs there (chimera#344). The same callback slot in every
 * session; no savestate carries it. */
ECL_INVISIBLE static uintptr_t (*g_inputPoll)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

extern "C" void chimera_input_was_read(void)
{
	g_inputRead = true;
	if (g_inputPoll)
		g_inputPoll(0, 0, 0, 0, 0, 0);
}

/* ---- the picture ---------------------------------------------------------------
 * Gearlynx draws 160x102, or 102x160 when the picture is turned for a game
 * held on its side, as RGBA bytes; Chimera takes BGRA. */
static const int kMaxSide = 160;
static uint32_t g_raw[kMaxSide * kMaxSide];
static uint32_t g_picture[kMaxSide * kMaxSide];
static int g_width = GLYNX_SCREEN_WIDTH, g_height = GLYNX_SCREEN_HEIGHT;
/* turbo: what the frontend wants now, not part of the machine */
ECL_INVISIBLE static bool g_render = true;

/* ---- the sound -------------------------------------------------------------------
 * Gearlynx mixes its four channels at 44,100 samples a second, interleaved
 * left and right, at most GLYNX_AUDIO_BUFFER_SIZE values a frame. */
static int16_t g_audio[GLYNX_AUDIO_BUFFER_SIZE + 16];
static int g_audioFrames;

/* ---- the frame rate --------------------------------------------------------------
 * A Lynx program sets its own refresh: timer 0 counts the lines and timer 2
 * the frames, so the frame lasts (backup0 + 1) x tick x (backup2 + 1)
 * microseconds. The boot ROM's values, 158 and 104 at 1 us, give 16,695 us
 * (59.9 frames a second); Lemmings runs at 50, Batman Returns at 75. Asked
 * after every frame. */
static int32_t g_vsyncDen = 16695;
static void update_rate(void)
{
	Mikey::Mikey_State *m = g_core->GetMikey()->GetState();
	const uint32_t t0 = m->timers[0].backup, t2 = m->timers[2].backup;
	const uint32_t tick = k_mikey_timer_period_us[m->timers[0].control_a & 7];
	const uint64_t us = (uint64_t)(t0 + 1) * tick * (t2 + 1);
	if ((t0 || t2) && us > 0 && us < 1000000)
		g_vsyncDen = (int32_t)us;
}

/* ---- files and settings -------------------------------------------------------- */
static uint8_t *read_file(const char *name, long *size)
{
	FILE *f = fopen(name, "rb");
	if (!f)
		return nullptr;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *data = n > 0 ? (uint8_t *)malloc((size_t)n) : nullptr;
	if (!data || fread(data, 1, (size_t)n, f) != (size_t)n)
	{
		free(data);
		fclose(f);
		return nullptr;
	}
	fclose(f);
	*size = n;
	return data;
}

static void setting(const char *name, char *out, int size, const char *dflt)
{
	snprintf(out, (size_t)size, "%s", dflt);
	wbx_setting_str(name, out, size);
}

static GLYNX_Rotation rotation_named(const char *v)
{
	if (!strcmp(v, "left")) return GLYNX_ROTATION_LEFT;
	if (!strcmp(v, "right")) return GLYNX_ROTATION_RIGHT;
	if (!strcmp(v, "none")) return GLYNX_ROTATION_DISABLED;
	if (!strcmp(v, "180")) return GLYNX_ROTATION_180;
	return GLYNX_ROTATION_AUTO;
}

static GLYNX_Console_Type model_named(const char *v)
{
	if (!strcmp(v, "lynx1")) return GLYNX_CONSOLE_MODEL_I;
	if (!strcmp(v, "lynx2")) return GLYNX_CONSOLE_MODEL_II;
	return GLYNX_CONSOLE_AUTO;
}

/* "auto", "none", or a chip and its width: "93c46_16", "93c86_8" */
static bool eeprom_named(const char *v, GLYNX_EEPROM *out)
{
	static const struct { const char *name; GLYNX_EEPROM chip; } kChips[] = {
		{ "93c46", GLYNX_EEPROM_93C46 }, { "93c56", GLYNX_EEPROM_93C56 },
		{ "93c66", GLYNX_EEPROM_93C66 }, { "93c76", GLYNX_EEPROM_93C76 },
		{ "93c86", GLYNX_EEPROM_93C86 },
	};
	if (!strcmp(v, "none"))
	{
		*out = GLYNX_EEPROM_NONE;
		return true;
	}
	for (const auto &c : kChips)
	{
		const size_t n = strlen(c.name);
		if (!strncmp(v, c.name, n) && v[n] == '_')
		{
			if (!strcmp(v + n + 1, "16")) { *out = c.chip; return true; }
			if (!strcmp(v + n + 1, "8")) { *out = (GLYNX_EEPROM)(c.chip | GLYNX_EEPROM_8BIT); return true; }
		}
	}
	return false; /* "auto": what the cartridge's header or upstream's list of games says */
}

/* The boot ROM stands in for nothing the core can make: a cartridge is read
 * by it (it decrypts the cartridge's loader), so a cartridge needs the one
 * the user brings. A BS93 program, the format homebrew is sent to a Lynx in,
 * is put in memory by Gearlynx itself, which then hides the boot ROM from the
 * processor (MAPCTL 0x0C) and starts the program: the boot ROM never runs.
 * Gearlynx still refuses to start with none, so such a program, given no boot
 * ROM, gets this: 512 bytes of zeros, nobody's work. */
static const uint8_t kNoBootRom[GLYNX_BIOS_SIZE] = { 0 };

extern "C" {

ECL_EXPORT const char *GetLoadError(void) { return g_loadError; }
ECL_EXPORT const char *DescribeDips(void) { return ""; }

ECL_EXPORT void SetInputPollCallback(uint64_t fn)
{
	g_inputPoll = (uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(uintptr_t)fn;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';

	/* the game: the project's "game" slot, else the plain rom mount */
	snprintf(g_gameName, sizeof g_gameName, "rom");
	if (wbx_slot_count("game") > 0)
		wbx_slot_name("game", 0, g_gameName, sizeof g_gameName);
	long size = 0;
	uint8_t *game = read_file(g_gameName, &size);
	if (!game)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.no_file");
		return 0;
	}
	if (size <= 10)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.too_short" TEXT_ARG "%s" TEXT_ARG "%ld", g_gameName, size);
		return 0;
	}

	g_core = new GearlynxCore();
	g_core->Init(GLYNX_PIXEL_RGBA8888);
	/* The machine starts from a seed of the project's, never the clock's:
	 * Gearlynx fills the memory and the processor's registers at power-on
	 * with it, as a real Lynx comes up with whatever was there. */
	g_core->SetRandomSeed((u32)wbx_setting_long("randomSeed", 0));

	char v[32];
	setting("model", v, sizeof v, "auto");
	g_core->GetMedia()->ForceConsoleType(model_named(v));
	setting("rotation", v, sizeof v, "auto");
	g_core->GetMedia()->ForceRotation(rotation_named(v));
	setting("eeprom", v, sizeof v, "auto");
	GLYNX_EEPROM chip;
	if (eeprom_named(v, &chip))
		g_core->GetMedia()->ForceEEPROM(chip);
	else
		g_core->GetMedia()->AutoDetectEEPROM();
	g_core->GetSuzy()->SetFastSpriteRendering(wbx_setting_bool("fastSprites", 0) != 0);

	/* the boot ROM, mounted under its id */
	long bootSize = 0;
	uint8_t *boot = read_file("lynxboot.img", &bootSize);
	if (boot && bootSize != GLYNX_BIOS_SIZE)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.boot_rom_size" TEXT_ARG "%ld", bootSize);
		return 0;
	}
	const bool hasBoot = boot != nullptr;
	g_core->LoadBiosFromBuffer(hasBoot ? boot : kNoBootRom, GLYNX_BIOS_SIZE);
	free(boot);

	if (!g_core->LoadROMFromBuffer(game, (int)size, g_gameName))
	{
		snprintf(g_loadError, sizeof g_loadError, "error.not_loaded" TEXT_ARG "%s", g_gameName);
		return 0;
	}
	free(game);
	if (!hasBoot && g_core->GetMedia()->GetType() != Media::MEDIA_HOMEBREW)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.boot_rom" TEXT_ARG "%s", "lynxboot.img");
		return 0;
	}

	/* the cartridge's EEPROM at the start: empty, or the project's save data */
	if (wbx_slot_count("savedata") > 0)
	{
		char name[256];
		const s32 want = g_core->GetMedia()->GetSaveMemorySize();
		u8 *mem = g_core->GetMedia()->GetSaveMemoryPointer();
		long n = 0;
		uint8_t *saved = want > 0 && mem && wbx_slot_name("savedata", 0, name, sizeof name) ? read_file(name, &n) : nullptr;
		if (saved)
		{
			/* a file of another size fills what it has */
			memcpy(mem, saved, (size_t)(n < want ? n : want));
			free(saved);
		}
	}

	memset(g_picture, 0, sizeof g_picture);
	update_rate();
	return 1;
}

ECL_EXPORT int GetButtonCount(void) { return kButtons; }
ECL_EXPORT const char *GetButtonName(int32_t i) { return i >= 0 && i < kButtons ? kButtonNames[i] : ""; }
ECL_EXPORT int IsButtonActive(int32_t i) { return i >= 0 && i < kButtons; }
ECL_EXPORT void SetButton(int32_t i, int32_t state)
{
	if (i >= 0 && i < kButtons)
		g_set[i] = state != 0;
}
ECL_EXPORT int GetAxisCount(void) { return 0; }
ECL_EXPORT int IsAxisActive(int32_t) { return 0; }
ECL_EXPORT void SetAxis(int32_t, int32_t) {}

static void take_picture(bool poweredOn)
{
	GLYNX_Runtime_Info info;
	g_core->GetRuntimeInfo(info);
	/* a console switched off shows upstream's picture of one, unturned */
	g_width = poweredOn ? info.screen_width : GLYNX_SCREEN_WIDTH;
	g_height = poweredOn ? info.screen_height : GLYNX_SCREEN_HEIGHT;
	const int n = g_width * g_height;
	for (int i = 0; i < n; i++)
	{
		const uint32_t p = g_raw[i];
		g_picture[i] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
	}
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	for (int i = 0; i < kButtons; i++)
		g_held[i] = g_set[i] || ((packed >> i) & 1);

	/* Power: pressed, the console starts again from power-on; the
	 * cartridge's EEPROM keeps what it holds */
	if (g_held[kPower] && !g_powerWasHeld)
		g_core->ResetROM(true);
	g_powerWasHeld = g_held[kPower];

	/* The keys as levels: every key let go, then the held ones pressed.
	 * Gearlynx turns the directions with the picture when it presses them
	 * (and for a left-handed player), so releasing all four first leaves
	 * nothing pressed under an old turn. */
	for (int i = 0; i < kPower; i++)
		g_core->KeyReleased(kKeys[i]);
	for (int i = 0; i < kPower; i++)
		if (g_held[i])
			g_core->KeyPressed(kKeys[i]);

	g_inputRead = false;
	int count = 0;
	const bool poweredOn = g_core->GetMikey()->IsPoweredOn();
	g_core->RunToVBlank((u8 *)g_raw, g_audio, &count, nullptr, g_render);
	g_audioFrames = count / 2;
	if (g_render)
		take_picture(poweredOn);
	update_rate();
}

ECL_EXPORT int InputWasRead(void) { return g_inputRead; }
ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }

ECL_EXPORT uint32_t *GetVideoBgra(void) { return g_picture; }
ECL_EXPORT int GetVideoWidth(void) { return g_width; }
ECL_EXPORT int GetVideoHeight(void) { return g_height; }
/* the Lynx's pixels are square */
ECL_EXPORT int GetDisplayAspectX(void) { return g_width; }
ECL_EXPORT int GetDisplayAspectY(void) { return g_height; }

ECL_EXPORT int GetVsyncNumerator(void) { return 1000000; }
ECL_EXPORT int GetVsyncDenominator(void) { return g_vsyncDen; }

ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_audioFrames; }

/* ---- memory ------------------------------------------------------------------------
 * RAM: the Lynx's 64 KB, all of it (Mikey, Suzy and the boot ROM sit over its
 * top page when they are mapped in; the RAM under them is here). Cart ROM:
 * the cartridge as loaded. EEPROM: a cartridge's save memory, when it has one. */
enum { kRam, kCart, kEeprom, kDomains };
static const char *const kDomainNames[kDomains] = { "RAM", "Cart ROM", "EEPROM" };
static bool domain(int i, uint8_t **data, int64_t *size)
{
	if (!g_core)
		return false;
	Media *media = g_core->GetMedia();
	switch (i)
	{
	case kRam: *data = g_core->GetMemory()->GetRAM(); *size = 0x10000; return true;
	case kCart: *data = media->GetROM(); *size = media->GetROMSize(); return *data && *size > 0;
	case kEeprom: *data = media->GetSaveMemoryPointer(); *size = media->GetSaveMemorySize(); return *data && *size > 0;
	}
	return false;
}
/* the domains this machine has, in order */
static int domain_index(int n)
{
	for (int i = 0; i < kDomains; i++)
	{
		uint8_t *d; int64_t s;
		if (domain(i, &d, &s) && n-- == 0)
			return i;
	}
	return -1;
}
ECL_EXPORT int GetMemoryDomainCount(void)
{
	int n = 0;
	for (int i = 0; i < kDomains; i++)
	{
		uint8_t *d; int64_t s;
		n += domain(i, &d, &s);
	}
	return n;
}
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	const int k = domain_index(i);
	return k < 0 ? "" : kDomainNames[k];
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	uint8_t *d; int64_t s;
	const int k = domain_index(i);
	return k >= 0 && domain(k, &d, &s) ? d : nullptr;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	uint8_t *d; int64_t s;
	const int k = domain_index(i);
	return k >= 0 && domain(k, &d, &s) ? s : 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int i) { return domain_index(i) != kCart; }

/* The system bus: the 64 KB the processor sees, with Suzy, Mikey and the
 * boot ROM where MAPCTL puts them, read as upstream's debugger reads it
 * (Memory::Read<true>, which changes nothing in the machine). */
static const int64_t kBusRanges[] = { 0, 0x10000, 0, 0 };
ECL_INVISIBLE static uint8_t g_busBuf[0x10000];
ECL_EXPORT int32_t GetBusCount(void) { return g_core ? 1 : 0; }
ECL_EXPORT const char *GetBusName(int32_t bus) { return bus == 0 ? "System Bus" : ""; }
ECL_EXPORT int64_t GetBusSize(int32_t bus) { return bus == 0 ? 0x10000 : 0; }
ECL_EXPORT int32_t GetBusWritable(int32_t bus) { return bus == 0; }
ECL_EXPORT int32_t PeekBus(int32_t bus, int32_t addr)
{
	if (bus != 0 || !g_core || addr < 0 || addr >= 0x10000)
		return 0;
	return g_core->GetMemory()->Read<true>((u16)addr);
}
ECL_EXPORT const int64_t *GetBusRanges(int32_t bus) { return bus == 0 ? kBusRanges : nullptr; }
ECL_EXPORT const uint8_t *ReadBus(int32_t bus, int64_t addr, int32_t len)
{
	if (bus != 0 || !g_core || addr < 0 || len < 0 || addr + len > 0x10000)
		return nullptr;
	for (int32_t i = 0; i < len; i++)
		g_busBuf[i] = g_core->GetMemory()->Read<true>((u16)(addr + i));
	return g_busBuf;
}
ECL_EXPORT void PokeBus(int32_t bus, int32_t addr, int32_t value)
{
	if (bus == 0 && g_core && addr >= 0 && addr < 0x10000)
		g_core->GetMemory()->Write<true>((u16)addr, (u8)value);
}

/* ---- the processor's registers ------------------------------------------------- */
static const char *const kRegisterNames[] = { "PC", "A", "X", "Y", "S", "P" };
ECL_EXPORT int32_t GetRegisterCount(void) { return 6; }
ECL_EXPORT const char *GetRegisterName(int32_t i) { return i >= 0 && i < 6 ? kRegisterNames[i] : ""; }
ECL_EXPORT int32_t GetRegisterBits(int32_t i) { return i == 0 ? 16 : 8; }
ECL_EXPORT int64_t GetRegisterValue(int32_t i)
{
	if (!g_core)
		return 0;
	M6502::M6502_State *s = g_core->GetM6502()->GetState();
	switch (i)
	{
	case 0: return s->PC.GetValue();
	case 1: return s->A.GetValue();
	case 2: return s->X.GetValue();
	case 3: return s->Y.GetValue();
	case 4: return s->S.GetValue();
	case 5: return s->P.GetValue();
	}
	return 0;
}
ECL_EXPORT const char *GetRegisterRoles(void) { return "pc PC\nsp S\nflags P\n"; }

/* ---- Export Save Data ----------------------------------------------------------------
 * A cartridge's EEPROM, as the game's name with .sav: the file Gearlynx
 * itself writes beside a game. */
static char g_saveName[256 + 8];
static bool has_save(void)
{
	return g_core && g_core->GetMedia()->GetSaveMemorySize() > 0 && g_core->GetMedia()->GetSaveMemoryPointer();
}
ECL_EXPORT int32_t GetSaveDataFileCount(void) { return has_save() ? 1 : 0; }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	if (i != 0 || !has_save())
		return "";
	char base[256];
	snprintf(base, sizeof base, "%s", g_gameName);
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';
	snprintf(g_saveName, sizeof g_saveName, "%s.sav", base);
	return g_saveName;
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i) { return i == 0 && has_save() ? g_core->GetMedia()->GetSaveMemorySize() : 0; }
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i) { return i == 0 && has_save() ? g_core->GetMedia()->GetSaveMemoryPointer() : nullptr; }

} // extern "C"
