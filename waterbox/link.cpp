/* link.cpp - Lynxes joined by the ComLynx cable, inside one core.
 *
 * Adapted from Gearlynx's own libretro link cable
 * (extern/Gearlynx/platforms/libretro/libretro_link.cpp, Ignacio Sanchez,
 * GPL-3.0-or-later), and under the same licence. What it does is
 * upstream's: each Lynx tells the cable when its UART starts a byte (its 11
 * bits and how long each lasts) and when it holds a break; when a Lynx's
 * UART samples the line it reads 0 if any other Lynx is driving a 0 bit or
 * a break at that moment on a shared timeline. The Lynxes run one cycle at a
 * time, always the one furthest behind on that timeline, so none ever reads
 * the line before the others have reached that moment. A frame ends when
 * P1's picture is finished.
 *
 * What differs from upstream: the pictures are 32-bit (the core's own
 * format); a key pressed for a frame counts from the frame's start, as on one
 * Lynx (upstream's link takes it a frame later); and everything here lives in
 * the core's memory, which a savestate carries: the units, the cable's record
 * of bytes and breaks, the origins.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "link.h"

#include <cstring>

namespace {

struct Frame
{
	u64 start_cycle;
	u32 bit_cycles;
	u16 bits;
};

struct Break
{
	u64 cycle;
	bool asserted;
};

struct Endpoint
{
	unsigned index;
};

LinkUnit *g_units;
unsigned g_count;
Endpoint g_endpoint[LINK_MAX];
u64 g_frameCycle;
u64 g_origin[LINK_MAX];
Frame g_frames[LINK_MAX][16];
Break g_breaks[LINK_MAX][16];
u32 g_frameCount[LINK_MAX];
u32 g_breakCount[LINK_MAX];

/* Each Lynx's own cycle count on the shared timeline: P2 is switched on a
 * millisecond after P1, P3 two, as upstream staggers them. */
u64 link_cycle(u64 cycle, unsigned index)
{
	return cycle - g_origin[index] + (u64)index * (GLYNX_MASTER_CLOCK / 1000);
}

u64 cycle_of(unsigned index)
{
	return link_cycle(g_units[index].core->GetComLynxCycle(), index);
}

void publish(u64 start_cycle, u32 bit_cycles, u16 bits, void *data)
{
	const unsigned index = static_cast<Endpoint *>(data)->index;
	Frame &frame = g_frames[index][g_frameCount[index]++ % 16];
	frame.start_cycle = link_cycle(start_cycle, index);
	frame.bit_cycles = bit_cycles;
	frame.bits = bits;
}

void brk(bool asserted, u64 cycle, void *data)
{
	const unsigned index = static_cast<Endpoint *>(data)->index;
	Break &state = g_breaks[index][g_breakCount[index]++ % 16];
	state.cycle = link_cycle(cycle, index);
	state.asserted = asserted;
}

/* the line as one Lynx's UART reads it: 0 when another drives it low */
bool sample(u64 cycle, void *data)
{
	const unsigned self = static_cast<Endpoint *>(data)->index;
	cycle = link_cycle(cycle, self);
	for (unsigned i = 0; i < g_count; i++)
	{
		if (i == self || !g_units[i].core->GetMikey()->IsPoweredOn())
			continue;
		u32 count = MIN(g_breakCount[i], 16u);
		for (u32 j = 0; j < count; j++)
		{
			const Break &state = g_breaks[i][(g_breakCount[i] - 1 - j) % 16];
			if (state.cycle > cycle)
				continue;
			if (state.asserted)
				return false;
			break;
		}
		count = MIN(g_frameCount[i], 16u);
		for (u32 j = 0; j < count; j++)
		{
			const Frame &frame = g_frames[i][(g_frameCount[i] - 1 - j) % 16];
			if (frame.bit_cycles == 0 || cycle < frame.start_cycle)
				continue;
			const u64 bit = (cycle - frame.start_cycle) / frame.bit_cycles;
			if (bit < 11 && (frame.bits & (1u << bit)) == 0)
				return false;
		}
	}
	return true;
}

bool can_run(GearlynxCore *core)
{
	return core->GetMedia()->IsReady() && core->GetMedia()->IsBiosLoaded() &&
		core->GetMikey()->IsPoweredOn() && !core->IsPaused();
}

} // namespace

void link_init(LinkUnit *units, unsigned count)
{
	g_units = units;
	g_count = count;
	g_frameCycle = 0;
	memset(g_frames, 0, sizeof g_frames);
	memset(g_breaks, 0, sizeof g_breaks);
	memset(g_frameCount, 0, sizeof g_frameCount);
	memset(g_breakCount, 0, sizeof g_breakCount);
	for (unsigned i = 0; i < count; i++)
	{
		g_endpoint[i].index = i;
		units[i].core->SetComLynxCallbacks(publish, sample, brk, nullptr, &g_endpoint[i]);
		g_origin[i] = units[i].core->GetComLynxCycle();
		units[i].core->SetComLynxCableConnected(true);
	}
}

void link_set_connected(bool connected)
{
	for (unsigned i = 0; i < g_count; i++)
		g_units[i].core->SetComLynxCableConnected(connected);
}

void link_run_frame(bool render)
{
	bool running[LINK_MAX];
	for (unsigned i = 0; i < g_count; i++)
	{
		g_units[i].samples = 0;
		running[i] = can_run(g_units[i].core);
		/* keys pressed for this frame count from its start, as on one
		 * Lynx (RunToVBlank does this; upstream's link waits a frame) */
		if (running[i])
			g_units[i].core->GetInput()->BeginFrame();
		if (!running[i])
			g_units[i].core->RunToVBlank(g_units[i].frame, g_units[i].audio, &g_units[i].samples, nullptr, render);
	}

	const u64 master_start = cycle_of(0);
	u64 target = master_start + 450000;
	if (!running[0])
	{
		GLYNX_Runtime_Info info;
		g_units[0].core->GetRuntimeInfo(info);
		const u32 cycles = info.frame_time > 0.0f ?
			(u32)MIN(info.frame_time * (GLYNX_MASTER_CLOCK / 1000.0f), 450000.0f) : GLYNX_MASTER_CLOCK / 60;
		target = g_frameCycle + MAX(cycles, 1u);
	}

	for (;;)
	{
		unsigned index = g_count;
		u64 first_cycle = target;
		for (unsigned i = 0; i < g_count; i++)
		{
			if (running[i] && cycle_of(i) < first_cycle)
			{
				index = i;
				first_cycle = cycle_of(i);
			}
		}
		if (index == g_count)
			break;

		u32 clocks;
		const bool vblank = g_units[index].core->RunCycle(clocks);
		if (vblank && render)
			g_units[index].core->RenderFrameBuffer(g_units[index].frame);

		running[index] = clocks != 0 && can_run(g_units[index].core);
		if (index == 0 && (vblank || !running[0] || cycle_of(0) - master_start > 450000))
			target = cycle_of(0);
	}

	g_frameCycle = target;
	for (unsigned i = 0; i < g_count; i++)
		g_units[i].core->EndFrame(g_units[i].audio, &g_units[i].samples);
}
