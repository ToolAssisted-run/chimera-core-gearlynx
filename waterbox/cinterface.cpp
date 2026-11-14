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
 * With the Machine setting at link2, link3 or link4 it holds that many
 * Lynxes joined by the ComLynx cable (link.cpp, after upstream's own link):
 * one machine to Chimera, with every player's keys, one picture of the
 * screens side by side and one sound.
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
#include "link.h"

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

/* ---- the Lynxes ------------------------------------------------------------------
 * One, or two to four joined by the cable. Each keeps its own picture as
 * Gearlynx draws it (160x102, or 102x160 turned) and its own sound. */
static const int kMaxSide = 160;
struct Lynx
{
	GearlynxCore *core;
	char gameName[256];
	uint32_t raw[kMaxSide * kMaxSide];
	int16_t audio[GLYNX_AUDIO_BUFFER_SIZE + 16];
	int width, height;
	bool powerWasHeld;
	bool dirOld[4];        /* the directions given last frame, for the filter */
};
static Lynx g_lynx[LINK_MAX];
static LinkUnit g_units[LINK_MAX];
static int g_count = 1;
static bool g_link;
static bool g_connected = true, g_connWasHeld;
static bool g_allowOpposite;

/* ---- the controls --------------------------------------------------------------
 * One Lynx: its nine keys, then Power. Linked: each player's nine keys, each
 * Lynx's Power, and Toggle Link Connection (waterbox.config's "buttons"). */
enum { kUp, kDown, kLeft, kRight, kA, kB, kOption1, kOption2, kPause, kKeys };
static const char *const kKeyNames[kKeys] = {
	"Up", "Down", "Left", "Right", "A", "B", "Option 1", "Option 2", "Pause",
};
static const GLYNX_Keys kGlynxKeys[kKeys] = {
	GLYNX_KEY_UP, GLYNX_KEY_DOWN, GLYNX_KEY_LEFT, GLYNX_KEY_RIGHT, GLYNX_KEY_A,
	GLYNX_KEY_B, GLYNX_KEY_OPTION1, GLYNX_KEY_OPTION2, GLYNX_KEY_PAUSE,
};
enum
{
	kSinglePower = kKeys, kSingleButtons,
	kLinkPower = LINK_MAX * kKeys, kLinkConnection = kLinkPower + LINK_MAX, kLinkButtons,
};
static const int kMaxButtons = kLinkButtons;
static char g_buttonNames[kMaxButtons][32];
static int g_buttonCount;
/* Two channels reach a core: a controller of 64 buttons or fewer arrives as
 * a packed mask in FrameAdvance, and the test runners use SetButton. A frame
 * is their union, each keeping its own state. */
static bool g_set[kMaxButtons];
static bool g_held[kMaxButtons];

/* lag: did any Lynx's program read JOYSTICK or SWITCHES this frame (patches/0001) */
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
 * Chimera takes BGRA; Gearlynx draws RGBA bytes. Linked Lynxes' screens are
 * side by side, each in a cell as large as the largest. */
static uint32_t g_picture[LINK_MAX * kMaxSide * kMaxSide];
static int g_width = GLYNX_SCREEN_WIDTH, g_height = GLYNX_SCREEN_HEIGHT;
/* turbo: what the frontend wants now, not part of the machine */
ECL_INVISIBLE static bool g_render = true;

/* ---- the sound -------------------------------------------------------------------
 * Gearlynx mixes its four channels at 44,100 samples a second, interleaved
 * left and right, at most GLYNX_AUDIO_BUFFER_SIZE values a frame. Linked
 * Lynxes' sounds are added together. */
static int16_t g_audio[GLYNX_AUDIO_BUFFER_SIZE + 16];
static int g_audioFrames;

/* ---- the frame rate --------------------------------------------------------------
 * A Lynx program sets its own refresh: timer 0 counts the lines and timer 2
 * the frames, so the frame lasts (backup0 + 1) x tick x (backup2 + 1)
 * microseconds. The boot ROM's values, 158 and 104 at 1 us, give 16,695 us
 * (59.9 frames a second); Lemmings runs at 50, Batman Returns at 75. Asked
 * after every frame. Linked, P1's frames are the machine's. */
static int32_t g_vsyncDen = 16695;
static void update_rate(void)
{
	Mikey::Mikey_State *m = g_lynx[0].core->GetMikey()->GetState();
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

/* "game" for P1, "game2" for P2... */
static void slot_id(char *out, size_t size, const char *base, int i)
{
	if (i == 0)
		snprintf(out, size, "%s", base);
	else
		snprintf(out, size, "%s%d", base, i + 1);
}

/* makes Lynx i from its game, the boot ROM and its save data */
static bool load_lynx(int i)
{
	Lynx &l = g_lynx[i];
	char slot[16];
	slot_id(slot, sizeof slot, "game", i);
	snprintf(l.gameName, sizeof l.gameName, "%s", i == 0 ? "rom" : "");
	if (wbx_slot_count(slot) > 0)
		wbx_slot_name(slot, 0, l.gameName, sizeof l.gameName);
	long size = 0;
	uint8_t *game = l.gameName[0] ? read_file(l.gameName, &size) : nullptr;
	if (!game)
	{
		if (i == 0)
			snprintf(g_loadError, sizeof g_loadError, "error.no_file");
		else
			snprintf(g_loadError, sizeof g_loadError, "error.no_file_player" TEXT_ARG "%d", i + 1);
		return false;
	}
	if (size <= 10)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.too_short" TEXT_ARG "%s" TEXT_ARG "%ld", l.gameName, size);
		return false;
	}

	l.core = new GearlynxCore();
	l.core->Init(GLYNX_PIXEL_RGBA8888);
	/* The machine starts from a seed of the project's, never the clock's:
	 * Gearlynx fills the memory and the processor's registers at power-on
	 * with it, as a real Lynx comes up with whatever was there. Linked
	 * Lynxes each get their own, as two real ones would differ. */
	l.core->SetRandomSeed((u32)wbx_setting_long("randomSeed", 0) + (u32)i * 0x9E3779B9u);

	char v[32];
	setting("model", v, sizeof v, "auto");
	l.core->GetMedia()->ForceConsoleType(model_named(v));
	setting("rotation", v, sizeof v, "auto");
	l.core->GetMedia()->ForceRotation(rotation_named(v));
	setting("eeprom", v, sizeof v, "auto");
	GLYNX_EEPROM chip;
	if (eeprom_named(v, &chip))
		l.core->GetMedia()->ForceEEPROM(chip);
	else
		l.core->GetMedia()->AutoDetectEEPROM();
	l.core->GetSuzy()->SetFastSpriteRendering(wbx_setting_bool("fastSprites", 0) != 0);

	/* the boot ROM, mounted under its id: one for every Lynx */
	long bootSize = 0;
	uint8_t *boot = read_file("lynxboot.img", &bootSize);
	if (boot && bootSize != GLYNX_BIOS_SIZE)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.boot_rom_size" TEXT_ARG "%ld", bootSize);
		return false;
	}
	const bool hasBoot = boot != nullptr;
	l.core->LoadBiosFromBuffer(hasBoot ? boot : kNoBootRom, GLYNX_BIOS_SIZE);
	free(boot);

	if (!l.core->LoadROMFromBuffer(game, (int)size, l.gameName))
	{
		snprintf(g_loadError, sizeof g_loadError, "error.not_loaded" TEXT_ARG "%s", l.gameName);
		return false;
	}
	free(game);
	if (!hasBoot && l.core->GetMedia()->GetType() != Media::MEDIA_HOMEBREW)
	{
		snprintf(g_loadError, sizeof g_loadError, "error.boot_rom" TEXT_ARG "%s", "lynxboot.img");
		return false;
	}

	/* the cartridge's EEPROM at the start: empty, or the project's save data */
	slot_id(slot, sizeof slot, "savedata", i);
	if (wbx_slot_count(slot) > 0)
	{
		char name[256];
		const s32 want = l.core->GetMedia()->GetSaveMemorySize();
		u8 *mem = l.core->GetMedia()->GetSaveMemoryPointer();
		long n = 0;
		uint8_t *saved = want > 0 && mem && wbx_slot_name(slot, 0, name, sizeof name) ? read_file(name, &n) : nullptr;
		if (saved)
		{
			/* a file of another size fills what it has */
			memcpy(mem, saved, (size_t)(n < want ? n : want));
			free(saved);
		}
	}
	l.width = GLYNX_SCREEN_WIDTH;
	l.height = GLYNX_SCREEN_HEIGHT;
	return true;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = '\0';
	char v[16];
	setting("machine", v, sizeof v, "lynx");
	g_count = !strcmp(v, "link2") ? 2 : !strcmp(v, "link3") ? 3 : !strcmp(v, "link4") ? 4 : 1;
	g_link = g_count > 1;
	g_allowOpposite = wbx_setting_bool("allowOpposite", 0) != 0;
	for (int i = 0; i < g_count; i++)
		if (!load_lynx(i))
			return 0;

	if (g_link)
	{
		for (int p = 0; p < LINK_MAX; p++)
		{
			for (int k = 0; k < kKeys; k++)
				snprintf(g_buttonNames[p * kKeys + k], sizeof g_buttonNames[0], "P%d %s", p + 1, kKeyNames[k]);
			snprintf(g_buttonNames[kLinkPower + p], sizeof g_buttonNames[0], "P%d Power", p + 1);
		}
		snprintf(g_buttonNames[kLinkConnection], sizeof g_buttonNames[0], "Toggle Link Connection");
		g_buttonCount = kLinkButtons;
		for (int i = 0; i < g_count; i++)
		{
			g_units[i].core = g_lynx[i].core;
			g_units[i].frame = (u8 *)g_lynx[i].raw;
			g_units[i].audio = g_lynx[i].audio;
			g_units[i].samples = 0;
		}
		link_init(g_units, (unsigned)g_count);
		g_connected = true;
	}
	else
	{
		for (int k = 0; k < kKeys; k++)
			snprintf(g_buttonNames[k], sizeof g_buttonNames[0], "P1 %s", kKeyNames[k]);
		snprintf(g_buttonNames[kSinglePower], sizeof g_buttonNames[0], "Power");
		g_buttonCount = kSingleButtons;
	}

	memset(g_picture, 0, sizeof g_picture);
	update_rate();
	return 1;
}

ECL_EXPORT int GetButtonCount(void) { return g_buttonCount; }
ECL_EXPORT const char *GetButtonName(int32_t i) { return i >= 0 && i < g_buttonCount ? g_buttonNames[i] : ""; }
ECL_EXPORT int IsButtonActive(int32_t i)
{
	if (i < 0 || i >= g_buttonCount)
		return 0;
	if (!g_link)
		return 1;
	if (i < kLinkPower)
		return i / kKeys < g_count;
	if (i < kLinkConnection)
		return i - kLinkPower < g_count;
	return 1;
}
ECL_EXPORT void SetButton(int32_t i, int32_t state)
{
	if (i >= 0 && i < kMaxButtons)
		g_set[i] = state != 0;
}
ECL_EXPORT int GetAxisCount(void) { return 0; }
ECL_EXPORT int IsAxisActive(int32_t) { return 0; }
ECL_EXPORT void SetAxis(int32_t, int32_t) {}

/* pressed: down this frame and not the last */
static bool pressed(int button, bool &wasHeld)
{
	const bool now = g_held[button];
	const bool edge = now && !wasHeld;
	wasHeld = now;
	return edge;
}

/* Lynx i's keys, as levels: every key let go, then the held ones pressed.
 * Gearlynx turns the directions with the picture when it presses them (and
 * for a left-handed player), so releasing all four first leaves nothing
 * pressed under an old turn. With Allow Opposite Directions off (upstream's
 * default), Up with Down and Left with Right are not passed on together:
 * the one held last frame wins, else Up and Left, as upstream's libretro
 * core filters them. */
static void set_keys(int i, const bool *held)
{
	Lynx &l = g_lynx[i];
	bool k[kKeys];
	for (int j = 0; j < kKeys; j++)
		k[j] = held[j];
	if (!g_allowOpposite)
	{
		if (k[kUp] && k[kDown])
		{
			const bool up = l.dirOld[kUp] || !l.dirOld[kDown];
			k[kUp] = up;
			k[kDown] = !up;
		}
		if (k[kLeft] && k[kRight])
		{
			const bool left = l.dirOld[kLeft] || !l.dirOld[kRight];
			k[kLeft] = left;
			k[kRight] = !left;
		}
	}
	for (int j = 0; j < 4; j++)
		l.dirOld[j] = k[j];
	for (int j = 0; j < kKeys; j++)
		l.core->KeyReleased(kGlynxKeys[j]);
	for (int j = 0; j < kKeys; j++)
		if (k[j])
			l.core->KeyPressed(kGlynxKeys[j]);
}

static void take_picture(bool *poweredOn)
{
	int cellW = 0, cellH = 0;
	for (int i = 0; i < g_count; i++)
	{
		Lynx &l = g_lynx[i];
		GLYNX_Runtime_Info info;
		l.core->GetRuntimeInfo(info);
		/* a console switched off shows upstream's picture of one, unturned */
		l.width = poweredOn[i] ? info.screen_width : GLYNX_SCREEN_WIDTH;
		l.height = poweredOn[i] ? info.screen_height : GLYNX_SCREEN_HEIGHT;
		cellW = l.width > cellW ? l.width : cellW;
		cellH = l.height > cellH ? l.height : cellH;
	}
	g_width = cellW * g_count;
	g_height = cellH;
	if (g_link)
		memset(g_picture, 0, sizeof(uint32_t) * (size_t)g_width * (size_t)g_height);
	for (int i = 0; i < g_count; i++)
	{
		const Lynx &l = g_lynx[i];
		const int x0 = i * cellW + (cellW - l.width) / 2, y0 = (cellH - l.height) / 2;
		for (int y = 0; y < l.height; y++)
			for (int x = 0; x < l.width; x++)
			{
				const uint32_t p = l.raw[y * l.width + x];
				g_picture[(y0 + y) * g_width + x0 + x] = (p & 0xFF00FF00u) | ((p & 0xFFu) << 16) | ((p >> 16) & 0xFFu);
			}
	}
}

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	for (int i = 0; i < kMaxButtons; i++)
		g_held[i] = g_set[i] || (i < 64 && ((packed >> i) & 1));

	/* Power: pressed, that Lynx starts again from power-on; its cartridge's
	 * EEPROM keeps what it holds */
	for (int i = 0; i < g_count; i++)
		if (pressed(g_link ? kLinkPower + i : (int)kSinglePower, g_lynx[i].powerWasHeld))
			g_lynx[i].core->ResetROM(true);
	if (g_link && pressed(kLinkConnection, g_connWasHeld))
	{
		g_connected = !g_connected;
		link_set_connected(g_connected);
	}
	for (int i = 0; i < g_count; i++)
		set_keys(i, g_held + i * kKeys);

	g_inputRead = false;
	bool poweredOn[LINK_MAX];
	for (int i = 0; i < g_count; i++)
		poweredOn[i] = g_lynx[i].core->GetMikey()->IsPoweredOn();
	int counts[LINK_MAX] = {0};
	if (g_link)
	{
		link_run_frame(g_render);
		for (int i = 0; i < g_count; i++)
			counts[i] = g_units[i].samples;
	}
	else
		g_lynx[0].core->RunToVBlank((u8 *)g_lynx[0].raw, g_lynx[0].audio, &counts[0], nullptr, g_render);

	/* the sound: one Lynx's, or every linked one's added together */
	int most = 0;
	for (int i = 0; i < g_count; i++)
		most = counts[i] > most ? counts[i] : most;
	if (most > GLYNX_AUDIO_BUFFER_SIZE)
		most = GLYNX_AUDIO_BUFFER_SIZE;
	for (int s = 0; s < most; s++)
	{
		int32_t sum = 0;
		for (int i = 0; i < g_count; i++)
			if (s < counts[i])
				sum += g_lynx[i].audio[s];
		g_audio[s] = (int16_t)(sum > 32767 ? 32767 : sum < -32768 ? -32768 : sum);
	}
	g_audioFrames = most / 2;
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
 * the cartridge as loaded. EEPROM: a cartridge's save memory, when it has one.
 * Linked, each Lynx's, named "P2 RAM" and so on. */
enum { kRam, kCart, kEeprom, kDomainsEach };
static const char *const kDomainNames[kDomainsEach] = { "RAM", "Cart ROM", "EEPROM" };
static char g_domainName[32];
static bool domain(int lynx, int kind, uint8_t **data, int64_t *size)
{
	if (lynx < 0 || lynx >= g_count || !g_lynx[lynx].core)
		return false;
	GearlynxCore *core = g_lynx[lynx].core;
	Media *media = core->GetMedia();
	switch (kind)
	{
	case kRam: *data = core->GetMemory()->GetRAM(); *size = 0x10000; return true;
	case kCart: *data = media->GetROM(); *size = media->GetROMSize(); return *data && *size > 0;
	case kEeprom: *data = media->GetSaveMemoryPointer(); *size = media->GetSaveMemorySize(); return *data && *size > 0;
	}
	return false;
}
/* the n-th domain this machine has: which Lynx, which kind */
static bool domain_at(int n, int *lynx, int *kind)
{
	for (int l = 0; l < g_count; l++)
		for (int k = 0; k < kDomainsEach; k++)
		{
			uint8_t *d; int64_t s;
			if (domain(l, k, &d, &s) && n-- == 0)
			{
				*lynx = l;
				*kind = k;
				return true;
			}
		}
	return false;
}
ECL_EXPORT int GetMemoryDomainCount(void)
{
	int n = 0;
	for (int l = 0; l < g_count; l++)
		for (int k = 0; k < kDomainsEach; k++)
		{
			uint8_t *d; int64_t s;
			n += domain(l, k, &d, &s);
		}
	return n;
}
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	int l, k;
	if (!domain_at(i, &l, &k))
		return "";
	if (!g_link)
		return kDomainNames[k];
	snprintf(g_domainName, sizeof g_domainName, "P%d %s", l + 1, kDomainNames[k]);
	return g_domainName;
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	int l, k; uint8_t *d; int64_t s;
	return domain_at(i, &l, &k) && domain(l, k, &d, &s) ? d : nullptr;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	int l, k; uint8_t *d; int64_t s;
	return domain_at(i, &l, &k) && domain(l, k, &d, &s) ? s : 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	int l, k;
	return domain_at(i, &l, &k) && k != kCart;
}

/* The system bus: the 64 KB the processor sees, with Suzy, Mikey and the
 * boot ROM where MAPCTL puts them, read as upstream's debugger reads it
 * (Memory::Read<true>, which changes nothing in the machine). One for each
 * Lynx. */
static const int64_t kBusRanges[] = { 0, 0x10000, 0, 0 };
ECL_INVISIBLE static uint8_t g_busBuf[0x10000];
static char g_busName[32];
static bool bus_ok(int32_t bus) { return bus >= 0 && bus < g_count && g_lynx[bus].core; }
ECL_EXPORT int32_t GetBusCount(void) { return g_lynx[0].core ? g_count : 0; }
ECL_EXPORT const char *GetBusName(int32_t bus)
{
	if (!bus_ok(bus))
		return "";
	if (!g_link)
		return "System Bus";
	snprintf(g_busName, sizeof g_busName, "P%d System Bus", bus + 1);
	return g_busName;
}
ECL_EXPORT int64_t GetBusSize(int32_t bus) { return bus_ok(bus) ? 0x10000 : 0; }
ECL_EXPORT int32_t GetBusWritable(int32_t bus) { return bus_ok(bus); }
ECL_EXPORT int32_t PeekBus(int32_t bus, int32_t addr)
{
	if (!bus_ok(bus) || addr < 0 || addr >= 0x10000)
		return 0;
	return g_lynx[bus].core->GetMemory()->Read<true>((u16)addr);
}
ECL_EXPORT const int64_t *GetBusRanges(int32_t bus) { return bus_ok(bus) ? kBusRanges : nullptr; }
ECL_EXPORT const uint8_t *ReadBus(int32_t bus, int64_t addr, int32_t len)
{
	if (!bus_ok(bus) || addr < 0 || len < 0 || addr + len > 0x10000)
		return nullptr;
	for (int32_t i = 0; i < len; i++)
		g_busBuf[i] = g_lynx[bus].core->GetMemory()->Read<true>((u16)(addr + i));
	return g_busBuf;
}
ECL_EXPORT void PokeBus(int32_t bus, int32_t addr, int32_t value)
{
	if (bus_ok(bus) && addr >= 0 && addr < 0x10000)
		g_lynx[bus].core->GetMemory()->Write<true>((u16)addr, (u8)value);
}

/* ---- the processors' registers ---------------------------------------------------
 * P1's on one Lynx; linked, each Lynx's, named "P2 PC" and so on. */
static const char *const kRegisterNames[] = { "PC", "A", "X", "Y", "S", "P" };
static char g_registerName[16];
ECL_EXPORT int32_t GetRegisterCount(void) { return 6 * g_count; }
ECL_EXPORT const char *GetRegisterName(int32_t i)
{
	if (i < 0 || i >= 6 * g_count)
		return "";
	if (!g_link)
		return kRegisterNames[i];
	snprintf(g_registerName, sizeof g_registerName, "P%d %s", i / 6 + 1, kRegisterNames[i % 6]);
	return g_registerName;
}
ECL_EXPORT int32_t GetRegisterBits(int32_t i) { return i % 6 == 0 ? 16 : 8; }
ECL_EXPORT int64_t GetRegisterValue(int32_t i)
{
	if (i < 0 || i >= 6 * g_count || !g_lynx[i / 6].core)
		return 0;
	M6502::M6502_State *s = g_lynx[i / 6].core->GetM6502()->GetState();
	switch (i % 6)
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
ECL_EXPORT const char *GetRegisterRoles(void)
{
	return g_link ? "pc P1 PC\nsp P1 S\nflags P1 P\n" : "pc PC\nsp S\nflags P\n";
}

/* ---- Export Save Data ----------------------------------------------------------------
 * A cartridge's EEPROM, as the game's name with .sav: the file Gearlynx
 * itself writes beside a game. Linked, "P2 <game>.sav" for P2's. */
static char g_saveName[256 + 16];
static bool has_save(int i)
{
	return g_lynx[i].core && g_lynx[i].core->GetMedia()->GetSaveMemorySize() > 0 && g_lynx[i].core->GetMedia()->GetSaveMemoryPointer();
}
static int save_owner(int32_t n)
{
	for (int i = 0; i < g_count; i++)
		if (has_save(i) && n-- == 0)
			return i;
	return -1;
}
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	int n = 0;
	for (int i = 0; i < g_count; i++)
		n += has_save(i);
	return n;
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t n)
{
	const int i = save_owner(n);
	if (i < 0)
		return "";
	char base[256];
	snprintf(base, sizeof base, "%s", g_lynx[i].gameName);
	char *dot = strrchr(base, '.');
	if (dot)
		*dot = '\0';
	if (g_link)
		snprintf(g_saveName, sizeof g_saveName, "P%d %s.sav", i + 1, base);
	else
		snprintf(g_saveName, sizeof g_saveName, "%s.sav", base);
	return g_saveName;
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t n)
{
	const int i = save_owner(n);
	return i < 0 ? 0 : g_lynx[i].core->GetMedia()->GetSaveMemorySize();
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t n)
{
	const int i = save_owner(n);
	return i < 0 ? nullptr : g_lynx[i].core->GetMedia()->GetSaveMemoryPointer();
}

} // extern "C"
