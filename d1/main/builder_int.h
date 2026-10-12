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
	BSEL_CUBE,	/* the whole cube behind the face */
	BSEL_WALL,	/* every connected coplanar face */
	BSEL_COUNT
};

/* How an axis nudge spreads across several tags. */
#define BUILDER_SHAPE_PAGES	9

/* Wheel kinds: tools/modes, shape page n (keys 1-9), carve (key C). */
enum builder_wheel {
	BWHEEL_NONE,
	BWHEEL_MODES,	/* tools, held ~ */
	BWHEEL_SELECT,	/* what a click tags, held V */
	BWHEEL_SHAPES,
	BWHEEL_CARVE = BWHEEL_SHAPES + BUILDER_SHAPE_PAGES,
	BWHEEL_CUT
};

#define BUILDER_CUT_COUNT	8

typedef struct carve_point {
	float x, y, z;
} carve_point;

typedef struct shape_stage {
	float cx, cy, w, h, angle;
} shape_stage;

enum builder_paint_mode {
	BPAINT_BRUSH,	/* click or drag over faces */
	BPAINT_FILL,	/* click paints a whole region */
	BPAINT_COUNT
};

enum builder_fill_scope {
	BFILL_WALL,	/* connected coplanar faces */
	BFILL_CUBE,	/* all faces of one cube */
	BFILL_ROOM,	/* all faces of connected cubes */
	BFILL_SAME,	/* every face using the same texture */
	BFILL_COUNT
};

enum builder_layer {
	BLAYER_BASE,
	BLAYER_OVERLAY,
	BLAYER_COUNT
};

enum builder_link_mode {
	BLINK_SYNC,	/* every tag moves the same way */
	BLINK_MIRROR,	/* tags across the mirror plane move the opposite way */
	BLINK_COUNT
};

#define BUILDER_MAX_TAGS	64
#define BUILDER_CUBE_UNITS	40	/* the starter cube, and the default build depth */
#define BUILDER_ELEMENT_VERTS	8

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
	int cut_neighbours;	/* a cut may slice the cubes it runs through, tagged or not */
	int tag_drag;		/* sweeping with the tag button tags what it crosses */
	int texture;
	int texture2;		/* overlay, never 0 */
	int layer;		/* which texture painting and browsing act on */
	int paint_mode, fill_scope;
	int seamless;		/* painted faces continue their neighbour's mapping */
} builder_settings;

extern builder_settings Builder;
extern const char *const Builder_tool_names[BTOOL_COUNT];
extern const char *const Builder_sel_mode_names[BSEL_COUNT];
extern const char *const Builder_link_names[BLINK_COUNT];
extern const char *const Builder_axis_names[3];
extern const char *const Builder_layer_names[BLAYER_COUNT];
extern const char *const Builder_paint_names[BPAINT_COUNT];
extern const char *const Builder_fill_names[BFILL_COUNT];
void builder_set_active_texture(int texnum);
int  builder_active_texture(void);
void builder_clear_overlay(void);

/* ---- builder.c */
void builder_forget_hover(void);
int  builder_wheel_active(int *kind, int *choice, int *dx, int *dy);
int  builder_wheel_entries(int kind);
void builder_set_tool(int tool);
void builder_set_sel_mode(int mode);
void builder_cycle_link_mode(void);
const builder_element *builder_hover(void);

/* ---- builder_geom.c */
fix  bld_units(float world_units);
void bld_message(const char *text);
int  bld_refuse(const char *reason);

int  bld_side_vertnum(segment *seg, int sidenum, int k);
int  bld_side_partner(segment *seg, int sidenum, int k);
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
void bld_rebase_side_uvs(side *sidep);
void bld_paint_side(segment *seg, int sidenum);
void bld_paint_side_with(segment *seg, int sidenum, int texnum);
void bld_paint_overlay(segment *seg, int sidenum, int texnum, int orientation);
void bld_bake_lighting(void);
int  bld_refresh_after_move(int *vertnums, int count);
int  bld_revalidate_all(void);

const char *bld_extrude(int segnum, int sidenum);
const char *bld_split(int segnum, int a, float t, int *new_num);
typedef struct split_cut {
	int segnum, a;
	float t;
} split_cut;

int  bld_split_plan(int segnum, int a, float t, split_cut *out, int max);
void bld_split_touched_reset(void);
int  bld_split_sealed(void);
int  bld_split_was_touched(int segnum);
void bld_split_limit(const int *segs, int count);
void bld_split_unlimit(void);
const char *bld_delete_blocker(int segnum);
void bld_delete(int segnum);
int  bld_nearest_vertex(int vertnum, int *exclude, int n_exclude, fix radius);
void bld_replace_vertex(int from, int to);

/* ---- builder_ops.c */
int  builder_tag_count(void);
const builder_element *builder_tag(int index);
void builder_select(const builder_element *el);
void builder_toggle_tag(const builder_element *el);
void builder_add_tag(const builder_element *el);
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
void builder_paint_continuing(const builder_element *el, const builder_element *previous);
void builder_fill_at(const builder_element *el);
void builder_eyedrop(const builder_element *el);
int  builder_recent_count(void);
int  builder_recent(int index);
void builder_pick_texture(void);
void builder_pick_texture_at(const builder_element *el);
void builder_cycle_texture(int delta);
void builder_uv_shift(fix du, fix dv);
void builder_uv_rotate(void);
void builder_uv_turn(float degrees);
void builder_uv_zoom(float factor);
void builder_uv_link(void);
int  builder_uv_drag_begin(void);
void builder_uv_drag_slide(float du, float dv);
void builder_uv_drag_turn(float degrees);
void builder_uv_drag_zoom(float factor);
void builder_uv_drag_end(void);
void builder_uv_flip(int flip_v);
void builder_uv_reset(void);

void builder_set_fullbright(int on);
void builder_rebake(void);

/* ---- builder_shapes.c */
const char *builder_shape_page_name(int page);
int  builder_shape_count(int page);
const char *builder_shape_name(int page, int index);
int  builder_shape_stages(int page, int index, shape_stage *out, int max);
void builder_shape_at(int page, int index, const builder_element *el);
void builder_shape_selection(int page, int index);

/* ---- builder_cut.c */
const char *builder_cut_name(int index);
void builder_cut_selection(int kind);
void builder_cut_preview(int kind);

/* ---- builder_gizmo.c */
int  builder_gizmo_visible(void);
void builder_gizmo_draw(void);
int  builder_gizmo_hover(void);
int  builder_gizmo_dragging(void);
int  builder_gizmo_begin(void);
void builder_gizmo_move(int dx, int dy);
void builder_gizmo_end(void);

/* ---- builder_carve.c */
const char *builder_carve_page_name(int page);
int  builder_carve_count(int page);
const char *builder_carve_name(int page, int index);
void builder_carve_points(int page, int index, carve_point *out);
void builder_carve_at(int page, int index, const builder_element *el);
void builder_carve_selection(int page, int index);

/* ---- builder_hud.c */
void builder_draw_status_bar(void);
int  builder_bar_click(int x, int y);
void builder_draw_wheel(int kind, int choice, int dx, int dy);

#define BUILDER_WHEEL_REACH	120	/* mouse pixels from centre to the rim */
#define BUILDER_WHEEL_DEADZONE	35

/* ---- builder_ui.c */
void builder_open_tools(void);
void builder_open_textures(void);
void builder_draw_texture_thumb(int texnum, int x, int y, int size);

#endif
