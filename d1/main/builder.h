/*
 * SNG: first-person level builder.
 *
 * Fly the ship around a level and grow it Sauerbraten-style: extrude cubes
 * off the face you aim at, carve them away, push faces and corners, paint
 * textures. The level is a normal D1 mission ("SNG Builder") on disk, so
 * whatever you build is immediately playable.
 */

#ifndef _BUILDER_H
#define _BUILDER_H

#include "event.h"

#define BUILDER_MISSION		"sngbuild"
#define BUILDER_LEVEL_FILE	"sngbuild.rdl"

int  builder_active(void);
void builder_start_menu(void);
void builder_frame(void);
void builder_filter_controls(void);
int  builder_handle_event(d_event *event);
void builder_render_overlay(void);
void builder_draw_hud(void);
void builder_end(void);

/* builder_level.c */
int  builder_level_exists(void);
int  builder_write_starter_level(void);
int  builder_save_level(void);

#endif
