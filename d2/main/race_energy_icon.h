/*
 * Custom Energy-powerup icon for Race mode, pulled from the Corrukia mission's
 * own "pwr01" custom texture (Corrukia.hog / Corrukia.pog) and swapped in for
 * the stock Energy pickup animation while Race mode is active.
 */

#ifndef _RACE_ENERGY_ICON_H
#define _RACE_ENERGY_ICON_H

#include "pstypes.h"

#define RACE_ENERGY_ICON_FRAMES 15
#define RACE_ENERGY_ICON_W      64
#define RACE_ENERGY_ICON_H      64

// Raw 8-bit palette-indexed pixel data, one row-major frame per slot.
// Defined in race_energy_icon_data.c (generated from Corrukia.pog).
extern const ubyte Race_energy_icon_pixels[RACE_ENERGY_ICON_FRAMES][RACE_ENERGY_ICON_W * RACE_ENERGY_ICON_H];

// Call after Game_mode is finalized for a fresh game (single-player or
// multiplayer host/join): installs the custom Energy icon if Game_mode has
// GM_RACE set, or restores the stock one if it doesn't. Idempotent either way.
void race_sync_energy_icon(void);

#endif
