/*
 * Installs the custom "pwr01" Energy-powerup icon (see race_energy_icon.h)
 * into the live bitmap table and points the Energy vclip's frames at it
 * while Race mode is running, restoring the stock frames otherwise.
 */

#include "race_energy_icon.h"
#include "game.h"
#include "gr.h"
#include "powerup.h"
#include "vclip.h"
#include "piggy.h"
#include "console.h"

static bitmap_index race_energy_icon_bi[RACE_ENERGY_ICON_FRAMES];
static bitmap_index race_energy_icon_saved[VCLIP_MAX_FRAMES];
static int race_energy_icon_saved_count = 0;
static int race_energy_icon_registered = 0;
static int race_energy_icon_installed = 0;

// One-time registration of the 15 custom frames as new named bitmaps, using
// the same convention piggy_init() uses for its "bogus" placeholder texture:
// initialize the next free GameBitmaps slot in place, then register it.
static void race_energy_icon_register(void)
{
	int i;

	if (race_energy_icon_registered)
		return;

	for (i = 0; i < RACE_ENERGY_ICON_FRAMES; i++)
	{
		grs_bitmap *bm = &GameBitmaps[Num_bitmap_files];

		gr_init_bitmap(bm, 0, 0, 0,
					   RACE_ENERGY_ICON_W, RACE_ENERGY_ICON_H, RACE_ENERGY_ICON_W,
					   (unsigned char *)Race_energy_icon_pixels[i]);
		gr_set_bitmap_flags(bm, BM_FLAG_TRANSPARENT);
		bm->avg_color = 1;

		race_energy_icon_bi[i] = piggy_register_bitmap(bm, "pwr01boost", 1);
	}

	race_energy_icon_registered = 1;
}

void race_sync_energy_icon(void)
{
	int vc = Powerup_info[POW_ENERGY].vclip_num;
	int i, n;

	if (vc < 0 || vc >= VCLIP_MAXNUM)
		return;

	n = Vclip[vc].num_frames;
	if (n <= 0)
		return;
	if (n > RACE_ENERGY_ICON_FRAMES)
		n = RACE_ENERGY_ICON_FRAMES;

	if (Game_mode & GM_RACE)
	{
		if (race_energy_icon_installed)
			return;

		race_energy_icon_register();

		for (i = 0; i < n; i++)
		{
			race_energy_icon_saved[i] = Vclip[vc].frames[i];
			Vclip[vc].frames[i] = race_energy_icon_bi[i];
		}
		race_energy_icon_saved_count = n;
		race_energy_icon_installed = 1;
		con_printf(CON_NORMAL, "race_energy_icon: installed custom Energy icon (%d frames)\n", n);
	}
	else if (race_energy_icon_installed)
	{
		for (i = 0; i < race_energy_icon_saved_count; i++)
			Vclip[vc].frames[i] = race_energy_icon_saved[i];

		race_energy_icon_installed = 0;
		con_printf(CON_NORMAL, "race_energy_icon: restored stock Energy icon\n");
	}
}
