/*
 * SNG: level builder internals shared by the editing core (builder.c) and
 * the tool panel (builder_ui.c).
 */

#ifndef _BUILDER_INT_H
#define _BUILDER_INT_H

#include "maths.h"

enum builder_tool {
	BTOOL_SELECT,
	BTOOL_BUILD,
	BTOOL_DELETE,
	BTOOL_PAINT,
	BTOOL_PICK,
	BTOOL_COUNT
};

enum builder_sel_mode {
	BSEL_FACE,
	BSEL_EDGE,
	BSEL_VERTEX,
	BSEL_COUNT
};

/* A face, plus which corner (vertex mode) or which edge, starting at
 * corner, (edge mode) of it is meant. */
typedef struct builder_element {
	int valid;
	int segnum;
	int sidenum;
	int corner;
} builder_element;

typedef struct builder_settings {
	int tool;
	int sel_mode;
	float nudge_step;	/* world units */
	float build_depth;	/* world units */
	int axis_snap;
	int fullbright;
	int texture;
} builder_settings;

extern builder_settings Builder;
extern const char *const Builder_tool_names[BTOOL_COUNT];
extern const char *const Builder_sel_mode_names[BSEL_COUNT];

const builder_element *builder_selection(void);
void builder_describe_selection(char *out, int size);
int  builder_is_dirty(void);
int  builder_undo_depth(void);
int  builder_redo_depth(void);

void builder_undo(void);
void builder_redo(void);
void builder_save(void);

void builder_nudge_axis(int axis, int direction);
void builder_nudge_normal(int direction);
void builder_snap_selection(void);
void builder_flatten_selection(void);
void builder_weld_selection(void);

void builder_extrude_selection(void);
void builder_delete_selection(void);

void builder_paint_selection(int whole_segment);
void builder_pick_texture(void);
void builder_cycle_texture(int delta);
void builder_uv_shift(fix du, fix dv);
void builder_uv_rotate(void);
void builder_uv_flip(int flip_v);
void builder_uv_reset(void);

void builder_set_fullbright(int on);
void builder_rebake(void);

/* builder_ui.c */
void builder_open_tools(void);
void builder_draw_texture_thumb(int texnum, int x, int y, int size);

#endif
