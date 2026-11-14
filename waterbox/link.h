/* link.h - Lynxes joined by the ComLynx cable, inside one core (link.cpp).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef CHIMERA_GEARLYNX_LINK_H
#define CHIMERA_GEARLYNX_LINK_H

#include "gearlynx.h"

#define LINK_MAX 4

struct LinkUnit
{
	GearlynxCore *core;
	u8 *frame;      /* the picture, as Gearlynx draws it */
	s16 *audio;     /* the frame's sound, left and right */
	int samples;    /* values in audio this frame */
};

/* Joins the units' Lynxes, from where each one is now, with the cable in. */
void link_init(LinkUnit *units, unsigned count);
/* The cable in or out (every Lynx at once). */
void link_set_connected(bool connected);
/* One frame: every Lynx runs, a cycle at a time in the order of a shared
 * timeline, until P1's picture is finished. */
void link_run_frame(bool render);

#endif
