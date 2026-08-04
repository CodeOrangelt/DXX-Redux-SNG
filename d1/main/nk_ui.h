/*
 * Nuklear-based replacement UI for the netgame "Advanced options" and
 * "Hosting setup" screens. See nk_ui.c for the rendering/input backend.
 * Only available in OpenGL builds (OGL) -- the plain SDL software-surface
 * backend has no GL context for Nuklear to draw into.
 */

#ifndef _NK_UI_H
#define _NK_UI_H

// Runs the Advanced netgame options screen. Edits the global Netgame
// struct in place, same as the legacy net_udp_more_game_options().
void nk_ui_advanced_options(void);

// Runs the netgame hosting setup screen (game name, mode, player/observer
// limits, etc). Returns 1 if the player started the game, 0 if they
// cancelled out. Mirrors the contract of the legacy net_udp_setup_game().
int nk_ui_hosting_setup(void);

#endif /* _NK_UI_H */
