/*
 * SNG: level builder internals.
 *
 *   builder.c        session, input, hover, 3D overlay
 *   builder_geom.c   mine primitives, undo/redo, validity, lighting, UVs
 *   builder_ops.c    tags and the user-facing edits (sync / mirror aware)
 *   builder_hud.c    icon status bar
 *   builder_ui.c     Tab tool panel
 *   builder_level.c  RDL / mission files
 */

#ifndef _BUILDER_INT_H
#define _BUILDER_INT_H

#include "maths.h"
#include "segment.h"

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

/* How an axis nudge spreads across several tags. */
enum builder_link_mode {
	BLINK_SYNC,	/* every tag moves the same way */
	BLINK_MIRROR,	/* tags across the mirror plane move the opposite way */
	BLINK_COUNT
};

#define BUILDER_MAX_TAGS	64

/* A face, plus which corner (vertex mode) or which edge, starting at that
 * corner (edge mode), of it is meant. */
typedef struct builder_element {
	int valid;
	int mode;
	int segnum;
	int sidenum;
	int corner;
} builder_element;

typedef struct builder_settings {
	int tool;
	int sel_mode;
	int link_mode;
	int mirror_axis;	/* 0 X, 1 Y, 2 Z */
	float nudge_step;	/* world units */
	float build_depth;	/* world units */
	int axis_snap;
	int fullbright;
	int texture;
} builder_settings;

extern builder_settings Builder;
extern const char *const Builder_tool_names[BTOOL_COUNT];
extern const char *const Builder_sel_mode_names[BSEL_COUNT];
extern const char *const Builder_link_names[BLINK_COUNT];
extern const char *const Builder_axis_names[3];

/* ---- builder.c */
void builder_forget_hover(void);
const builder_element *builder_hover(void);

/* ---- builder_geom.c */
fix  bld_units(float world_units);
void bld_message(const char *text);
int  bld_refuse(const char *reason);

int  bld_side_vertnum(segment *seg, int sidenum, int k);
vms_vector *bld_side_vertex(segment *seg, int sidenum, int k);
void bld_side_normal(segment *seg, int sidenum, vms_vector *inward);
void bld_side_outward(segment *seg, int sidenum, vms_vector *outward);
int  bld_side_is_solid(segment *seg, int sidenum);
int  bld_element_vertices(const builder_element *el, int *out);
void bld_element_center(const builder_element *el, vms_vector *center);

int  bld_begin_edit(void);
int  bld_finish_edit(int ok, const char *failure);
void bld_cancel_edit(void);
void bld_history_clear(void);
int  bld_undo_depth(void);
int  bld_redo_depth(void);
int  bld_undo(void);
int  bld_redo(void);
int  bld_is_dirty(void);
void bld_set_dirty(int dirty);

void bld_map_side_uvs(segment *seg, int sidenum);
void bld_paint_side(segment *seg, int sidenum);
void bld_bake_lighting(void);
int  bld_refresh_after_move(int *vertnums, int count);
int  bld_revalidate_all(void);

const char *bld_extrude(int segnum, int sidenum);
const char *bld_delete_blocker(int segnum);
void bld_delete(int segnum);
int  bld_nearest_vertex(int vertnum, int *exclude, int n_exclude, fix radius);
void bld_replace_vertex(int from, int to);

/* ---- builder_ops.c */
int  builder_tag_count(void);
const builder_element *builder_tag(int index);
void builder_select(const builder_element *el);
void builder_toggle_tag(const builder_element *el);
void builder_clear_tags(void);
void builder_forget_stale_tags(void);
int  builder_mirror_plane(vms_vector *center);
void builder_describe_selection(char *out, int size);

void builder_undo(void);
void builder_redo(void);
void builder_save(void);

void builder_nudge_axis(int axis, int direction);
void builder_push(int direction, int steps);
void builder_push_element(const builder_element *el, int direction, int steps);
void builder_snap_selection(void);
void builder_flatten_selection(void);
void builder_weld_selection(void);

void builder_extrude_selection(void);
void builder_delete_selection(void);
void builder_extrude_at(const builder_element *el);
void builder_delete_at(const builder_element *el);

void builder_paint_selection(int whole_segment);
void builder_paint_at(const builder_element *el);
void builder_pick_texture(void);
void builder_pick_texture_at(const builder_element *el);
void builder_cycle_texture(int delta);
void builder_uv_shift(fix du, fix dv);
void builder_uv_rotate(void);
void builder_uv_flip(int flip_v);
void builder_uv_reset(void);

void builder_set_fullbright(int on);
void builder_rebake(void);

/* ---- builder_hud.c */
void builder_draw_status_bar(void);

/* ---- builder_ui.c */
void builder_open_tools(void);
void builder_draw_texture_thumb(int texnum, int x, int y, int size);

#endif
