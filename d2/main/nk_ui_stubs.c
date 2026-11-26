/*
 * Non-OpenGL builds have no Nuklear UI. These stand in so the software
 * renderer still links; it is used for headless duel-bot benchmarks,
 * where the key-binding screen is never opened.
 */

#include "newmenu.h"
#include "nk_ui.h"

#ifndef USE_NK_UI

void newmenu_set_fixed_sections(newmenu *menu)
{
	(void)menu;
}

void nk_ui_menu_opened(void)
{
}

void nk_ui_menu_closed(void)
{
}

int nk_ui_bind_rows_per_column(void)
{
	return 0;
}

int nk_ui_bind_frame(const void *id, const char *title, const char *hint,
	const struct nk_ui_bind_row *rows, int nrows, const char *const *slot_names,
	int current_row, int current_slot, int changing, int *picked_row, int *picked_slot)
{
	return NK_UI_BIND_CLOSE;
}

int nk_ui_singleplayer_setup(void)
{
	return 0;
}

void newmenu_offer_pages(const char *title, const char *const *names, int count, int active, int *next)
{
	(void)title; (void)names; (void)count; (void)active; (void)next;
}

#endif
