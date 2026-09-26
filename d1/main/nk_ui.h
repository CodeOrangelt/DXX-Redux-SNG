/*
 * Nuklear-based replacement UI for the netgame "Advanced options" and
 * "Hosting setup" screens. See nk_ui.c for the rendering/input backend.
 * Only available in OpenGL builds (OGL) -- the plain SDL software-surface
 * backend has no GL context for Nuklear to draw into.
 */

#ifndef _NK_UI_H
#define _NK_UI_H

#if defined(OGL) && defined(USE_UDP)
#define USE_NK_UI
#endif

// Runs the Advanced netgame options screen. Edits the global Netgame
// struct in place, same as the legacy net_udp_more_game_options().
void nk_ui_advanced_options(void);
void nk_ui_weapon_autoselect(void);

// Browses the screenshots in SCRNS_DIR, previewing the one selected.
void nk_ui_screenshots(void);

// Draws the player's chosen menu picture full screen as plain RGB. Returns 0
// when none is set or it can't be read, so the caller draws the stock one.
int nk_ui_draw_backdrop(void);

// Draws `text` centred along the bottom edge in a fixed red, unaffected by the
// menu background's palette.
void nk_ui_draw_copyright(const char *text);

// Draws the logo just above `menu_top_px`, its left edge `left_px` from the left.
void nk_ui_draw_menu_logo(float left_px, float menu_top_px);

// Runs the netgame hosting setup screen (game name, mode, player/observer
// limits, etc). Returns 1 if the player started the game, 0 if they
// cancelled out. Mirrors the contract of the legacy net_udp_setup_game().
int nk_ui_hosting_setup(void);

// Generic newmenu renderer: any newmenu can draw as a Nuklear panel. The
// window system owns the frame; these only supply input and drawing.
#include <SDL.h>

struct newmenu_item;

void nk_ui_menu_opened(void);
void nk_ui_menu_closed(void);
int nk_ui_menus_open(void);
void nk_ui_set_fixed_sections(int fixed);

// Bracket the SDL events of one event_poll() pass (see arch/sdl/event.c).
void nk_ui_input_begin(void);
void nk_ui_feed_event(SDL_Event *evt);
void nk_ui_input_end(void);

// Draws one panel; *changed / *selected get the item index touched, or -1.
void nk_ui_newmenu_frame(const void *id, const char *title, const char *subtitle, struct newmenu_item *items, int nitems, int citem, int refocus, int reorder, unsigned int *collapsed, int has_focus, int *changed, int *selected);

// Swaps two rows of a priority list (the weapon autoselect menus).
void nk_ui_swap_items(struct newmenu_item *items, int a, int b);

void nk_ui_select_radio(struct newmenu_item *items, int nitems, int chosen);

// A long run of radio options in one group is shown as a single row saying
// what is set; picking it opens a list of its own to choose from.
int nk_ui_radio_run_start(const struct newmenu_item *items, int index);
int nk_ui_radio_run_end(const struct newmenu_item *items, int nitems, int index);
int nk_ui_item_is_folded(const struct newmenu_item *items, int nitems, int index, unsigned int collapsed);

// An all-caps text row heads a section and folds it away when picked.
// `collapsed` carries one bit per heading; the menu owns it. A menu opens
// fully folded the first time and the way it was left after that.
int nk_ui_has_sections(const struct newmenu_item *items, int nitems);
unsigned int nk_ui_recall_sections(const char *title, const struct newmenu_item *items, int nitems);
void nk_ui_remember_sections(const char *title, unsigned int mask);
int nk_ui_is_collapsible_head(const struct newmenu_item *items, int nitems, int index);
void nk_ui_toggle_section(const struct newmenu_item *items, int nitems, int index, unsigned int *collapsed);
int nk_ui_radio_chosen(const struct newmenu_item *items, int start, int end);

// The accent colour family the menus are drawn in, chosen by the player.
int nk_ui_menu_color_count(void);
const char *nk_ui_menu_color_name(int index);
void nk_ui_set_menu_color(int index);

// Presents the panels built this frame; see nk_ui.c.
void nk_ui_flush(void);

enum { NK_UI_LISTBOX_NONE, NK_UI_LISTBOX_ACCEPT, NK_UI_LISTBOX_CANCEL };

// ---- Control bindings screen -------------------------------------------
// kconfig.c owns the bindings and the capture state machine; nk_ui only
// draws the rows it hands over and says what was clicked.
#define NK_UI_BIND_MAX_SLOTS 3

struct nk_ui_bind_row
{
	const char *label;
	const char *slot[NK_UI_BIND_MAX_SLOTS];	// binding text; NULL = no such slot
	int nslots;
};

enum { NK_UI_BIND_NONE, NK_UI_BIND_PICK, NK_UI_BIND_CLOSE, NK_UI_BIND_CLEAR, NK_UI_BIND_DEFAULTS };

// `slot_names`, when given, heads each column with what its slots are
// (the weapon-key screen binds keyboard, joystick and mouse side by side).
int nk_ui_bind_frame(const void *id, const char *title, const char *hint,
	const struct nk_ui_bind_row *rows, int nrows, const char *const *slot_names,
	int current_row, int current_slot, int changing, int *picked_row, int *picked_slot);

#define NK_UI_FILTER_LEN 32

// Case-insensitive substring test, shared with the list keyboard handling.
int nk_ui_filter_matches(const char *text, const char *filter);

// Draws one scrollable list panel; see nk_ui.c for the click semantics.
int nk_ui_listbox_frame(const void *id, const char *title, char **items, int nitems, int *citem, int *last_citem, unsigned int *scroll_y, char *filter, int allow_abort, int has_focus);

#endif /* _NK_UI_H */
