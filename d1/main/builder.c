/*
 * SNG: first-person level builder -- session, input, hover, 3D overlay.
 *
 * The crosshair hovers a face, edge or vertex. Left click applies the current
 * tool (Build by default) -- to every tag if you click one of them, else to
 * what you hover. Right click deletes, middle click selects, Shift+middle
 * click adds or removes a tag, and the wheel insets/outsets the tags (or the
 * hovered element). Precision edits live in the Tab tool panel.
 */

#include <string.h>

#include "inferno.h"
#include "segment.h"
#include "gameseg.h"
#include "object.h"
#include "fvi.h"
#include "3d.h"
#include "gr.h"
#include "player.h"
#include "game.h"
#include "kconfig.h"
#include "key.h"
#include "mouse.h"
#include "event.h"
#include "mission.h"
#include "gameseq.h"
#include "newmenu.h"
#include "playsave.h"
#include "hudmsg.h"
#include "builder.h"
#include "builder_int.h"
#ifdef OGL
#include "ogl_init.h"
#endif

#define BUILDER_REACH		(600 * F1_0)
#define VERTEX_MARK_SIZE	(F1_0 * 3 / 2)
#define WHEEL_FAST_STEPS	5
#define MIRROR_PLANE_MARGIN	(4 * F1_0)

static int Builder_on;
static builder_element Hover;

int builder_active(void)
{
	return Builder_on;
}

const builder_element *builder_hover(void)
{
	return &Hover;
}

void builder_forget_hover(void)
{
	Hover.valid = 0;
}

/* ------------------------------------------------------------ hovering */

static fix point_to_segment_dist(vms_vector *p, vms_vector *a, vms_vector *b)
{
	vms_vector ab, ap, closest;
	fix len2, t;

	vm_vec_sub(&ab, b, a);
	vm_vec_sub(&ap, p, a);
	len2 = vm_vec_dot(&ab, &ab);
	t = len2 > 0 ? fixdiv(vm_vec_dot(&ap, &ab), len2) : 0;
	if (t < 0)
		t = 0;
	if (t > F1_0)
		t = F1_0;
	vm_vec_scale_add(&closest, a, &ab, t);
	return vm_vec_dist(p, &closest);
}

/* Nearest corner (vertex mode) or nearest edge start (edge mode). */
static int nearest_part(segment *seg, int sidenum, vms_vector *point)
{
	fix best_dist = 0;
	int best = 0, k;

	for (k = 0; k < 4; k++)
	{
		vms_vector *a = bld_side_vertex(seg, sidenum, k);
		vms_vector *b = bld_side_vertex(seg, sidenum, (k + 1) % 4);
		fix dist = Builder.sel_mode == BSEL_EDGE ? point_to_segment_dist(point, a, b) : vm_vec_dist(a, point);

		if (k == 0 || dist < best_dist)
		{
			best = k;
			best_dist = dist;
		}
	}
	return best;
}

static void update_hover(void)
{
	vms_vector end;
	fvi_query query;
	fvi_info hit;

	Hover.valid = 0;
	vm_vec_scale_add(&end, &ConsoleObject->pos, &ConsoleObject->orient.fvec, BUILDER_REACH);
	query.p0 = &ConsoleObject->pos;
	query.p1 = &end;
	query.startseg = ConsoleObject->segnum;
	query.rad = 0;
	query.thisobjnum = ConsoleObject - Objects;
	query.ignore_obj_list = NULL;
	query.flags = 0;
	find_vector_intersection(&query, &hit);

	if (hit.hit_type != HIT_WALL || hit.hit_side_seg < 0 || hit.hit_side < 0)
		return;
	Hover.valid = 1;
	Hover.mode = Builder.sel_mode;
	Hover.segnum = hit.hit_side_seg;
	Hover.sidenum = hit.hit_side;
	Hover.corner = nearest_part(&Segments[Hover.segnum], Hover.sidenum, &hit.hit_pnt);
}

/* --------------------------------------------------------------- input */

static int hover_is_tagged(void)
{
	int i;

	for (i = 0; i < builder_tag_count(); i++)
	{
		const builder_element *tag = builder_tag(i);

		if (tag->segnum == Hover.segnum && tag->sidenum == Hover.sidenum)
			return 1;
	}
	return 0;
}

static void announce_selection(void)
{
	char text[96];

	builder_describe_selection(text, sizeof(text));
	bld_message(text);
}

/* Left click: the current tool. Clicking a tag applies it to all tags. */
static void apply_tool(void)
{
	int on_tags = hover_is_tagged();

	if (!Hover.valid)
		return;
	switch (Builder.tool)
	{
		case BTOOL_SELECT:
			builder_select(&Hover);
			announce_selection();
			break;
		case BTOOL_BUILD:
			if (on_tags) builder_extrude_selection(); else builder_extrude_at(&Hover);
			break;
		case BTOOL_DELETE:
			if (on_tags) builder_delete_selection(); else builder_delete_at(&Hover);
			break;
		case BTOOL_PAINT:
			if (on_tags) builder_paint_selection(0); else builder_paint_at(&Hover);
			break;
		case BTOOL_PICK:
			builder_pick_texture_at(&Hover);
			break;
	}
}

static int shift_held(void)
{
	return keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT];
}

static void middle_click(void)
{
	if (!Hover.valid)
		return;
	if (shift_held())
		builder_toggle_tag(&Hover);
	else
		builder_select(&Hover);
	announce_selection();
}

static void right_click(void)
{
	if (!Hover.valid)
		return;
	if (hover_is_tagged())
		builder_delete_selection();
	else
		builder_delete_at(&Hover);
}

/* Wheel: inset/outset the tags, or with none, whatever the crosshair is on. */
static void wheel(int direction)
{
	int steps = shift_held() ? WHEEL_FAST_STEPS : 1;

	if (builder_tag_count())
		builder_push(direction, steps);
	else
		builder_push_element(&Hover, direction, steps);
}

static void set_tool(int tool)
{
	Builder.tool = tool;
	HUD_init_message(HM_DEFAULT, "Tool: %s", Builder_tool_names[tool]);
}

static void cycle_sel_mode(void)
{
	Builder.sel_mode = (Builder.sel_mode + 1) % BSEL_COUNT;
	HUD_init_message(HM_DEFAULT, "Selecting: %s", Builder_sel_mode_names[Builder.sel_mode]);
}

static void cycle_link_mode(void)
{
	if (Builder.link_mode == BLINK_SYNC)
		Builder.link_mode = BLINK_MIRROR;
	else if (Builder.mirror_axis < 2)
		Builder.mirror_axis++;
	else
	{
		Builder.link_mode = BLINK_SYNC;
		Builder.mirror_axis = 0;
	}
	if (Builder.link_mode == BLINK_SYNC)
		bld_message("Tags move in sync");
	else
		HUD_init_message(HM_DEFAULT, "Tags mirror across %s", Builder_axis_names[Builder.mirror_axis]);
}

static int handle_mouse(d_event *event)
{
	switch (event_mouse_get_button(event))
	{
		case MBTN_LEFT:		apply_tool(); return 1;
		case MBTN_RIGHT:	right_click(); return 1;
		case MBTN_MIDDLE:	middle_click(); return 1;
		case MBTN_Z_UP:		wheel(1); return 1;
		case MBTN_Z_DOWN:	wheel(-1); return 1;
		default:		return 0;
	}
}

static int is_builder_button(d_event *event)
{
	int button = event_mouse_get_button(event);

	return button == MBTN_LEFT || button == MBTN_RIGHT || button == MBTN_MIDDLE ||
		button == MBTN_Z_UP || button == MBTN_Z_DOWN;
}

static int handle_key(int key)
{
	switch (key)
	{
		case KEY_TAB:			builder_open_tools(); return 1;
		case KEY_1:			set_tool(BTOOL_SELECT); return 1;
		case KEY_2:			set_tool(BTOOL_BUILD); return 1;
		case KEY_3:			set_tool(BTOOL_DELETE); return 1;
		case KEY_4:			set_tool(BTOOL_PAINT); return 1;
		case KEY_5:			set_tool(BTOOL_PICK); return 1;
		case KEY_V:			cycle_sel_mode(); return 1;
		case KEY_M:			cycle_link_mode(); return 1;
		case KEY_ESC + KEY_SHIFTED:	builder_clear_tags(); announce_selection(); return 1;
		case KEY_COMMA:			builder_push(-1, 1); return 1;
		case KEY_PERIOD:		builder_push(1, 1); return 1;
		case KEY_BACKSP:
		case KEY_CTRLED + KEY_Z:	builder_undo(); return 1;
		case KEY_CTRLED + KEY_Y:	builder_redo(); return 1;
		case KEY_CTRLED + KEY_S:	builder_save(); return 1;
		default:			return 0;
	}
}

int builder_handle_event(d_event *event)
{
	if (!Builder_on)
		return 0;

	switch (event->type)
	{
		case EVENT_MOUSE_BUTTON_DOWN:
			return handle_mouse(event);
		case EVENT_MOUSE_BUTTON_UP:
			return is_builder_button(event);
		case EVENT_KEY_COMMAND:
			return handle_key(event_key_get(event));
		default:
			return 0;
	}
}

/* No weapons in the builder: the fire buttons are building tools. */
void builder_filter_controls(void)
{
	if (!Builder_on)
		return;
	Controls.fire_primary_state = 0;
	Controls.fire_primary_count = 0;
	Controls.fire_secondary_state = 0;
	Controls.fire_secondary_count = 0;
	Controls.fire_flare_count = 0;
	Controls.drop_bomb_count = 0;
	Controls.cycle_primary_count = 0;
	Controls.cycle_secondary_count = 0;
	Controls.automap_count = 0;
}

void builder_frame(void)
{
	if (!Builder_on)
		return;
	Players[Player_num].shields = MAX_SHIELDS;
	Players[Player_num].energy = MAX_ENERGY;
	builder_filter_controls();
	update_hover();
}

/* ------------------------------------------------------------- overlay */

static void draw_world_line(vms_vector *a, vms_vector *b)
{
	g3s_point pa, pb;

	g3_rotate_point(&pa, a);
	g3_rotate_point(&pb, b);
	g3_draw_line(&pa, &pb);
}

static void draw_face_outline(segment *seg, int sidenum, vms_vector *offset)
{
	vms_vector corners[4];
	int k;

	for (k = 0; k < 4; k++)
	{
		corners[k] = *bld_side_vertex(seg, sidenum, k);
		if (offset)
			vm_vec_add2(&corners[k], offset);
	}
	for (k = 0; k < 4; k++)
		draw_world_line(&corners[k], &corners[(k + 1) % 4]);
}

static void draw_vertex_mark(vms_vector *corner)
{
	static const vms_vector axes[3] = {
		{VERTEX_MARK_SIZE, 0, 0}, {0, VERTEX_MARK_SIZE, 0}, {0, 0, VERTEX_MARK_SIZE},
	};
	int i;

	for (i = 0; i < 3; i++)
	{
		vms_vector a, b;

		vm_vec_sub(&a, corner, &axes[i]);
		vm_vec_add(&b, corner, &axes[i]);
		draw_world_line(&a, &b);
	}
}

/* The cube a Build click would create, drawn as a wireframe ghost. */
static void draw_extrude_preview(segment *seg, int sidenum)
{
	vms_vector step;
	int k;

	if (seg->children[sidenum] != -1)
		return;
	bld_side_outward(seg, sidenum, &step);
	vm_vec_scale(&step, bld_units(Builder.build_depth));
	draw_face_outline(seg, sidenum, &step);
	for (k = 0; k < 4; k++)
	{
		vms_vector far_corner;

		vm_vec_add(&far_corner, bld_side_vertex(seg, sidenum, k), &step);
		draw_world_line(bld_side_vertex(seg, sidenum, k), &far_corner);
	}
}

static void draw_element(const builder_element *el, int face_color, int part_color)
{
	segment *seg = &Segments[el->segnum];

	gr_setcolor(face_color);
	draw_face_outline(seg, el->sidenum, NULL);
	gr_setcolor(part_color);
	if (el->mode == BSEL_VERTEX)
		draw_vertex_mark(bld_side_vertex(seg, el->sidenum, el->corner));
	else if (el->mode == BSEL_EDGE)
		draw_world_line(bld_side_vertex(seg, el->sidenum, el->corner),
			bld_side_vertex(seg, el->sidenum, (el->corner + 1) % 4));
}

static void grow_bounds(vms_vector *lo, vms_vector *hi, vms_vector *p)
{
	if (p->x < lo->x) lo->x = p->x;
	if (p->y < lo->y) lo->y = p->y;
	if (p->z < lo->z) lo->z = p->z;
	if (p->x > hi->x) hi->x = p->x;
	if (p->y > hi->y) hi->y = p->y;
	if (p->z > hi->z) hi->z = p->z;
}

static void tag_bounds(vms_vector *lo, vms_vector *hi)
{
	int t, i;

	*lo = *hi = *bld_side_vertex(&Segments[builder_tag(0)->segnum], builder_tag(0)->sidenum, 0);
	for (t = 0; t < builder_tag_count(); t++)
	{
		int verts[4], n = bld_element_vertices(builder_tag(t), verts);

		for (i = 0; i < n; i++)
			grow_bounds(lo, hi, &Vertices[verts[i]]);
	}
}

static fix *component(vms_vector *v, int axis)
{
	return axis == 0 ? &v->x : axis == 1 ? &v->y : &v->z;
}

/* The mirror plane, as a rectangle spanning the tags. */
static void draw_mirror_plane(void)
{
	vms_vector center, lo, hi, corners[4];
	int a = (Builder.mirror_axis + 1) % 3, b = (Builder.mirror_axis + 2) % 3, k;

	if (!builder_mirror_plane(&center))
		return;
	tag_bounds(&lo, &hi);
	for (k = 0; k < 4; k++)
	{
		corners[k] = center;
		*component(&corners[k], a) = (k == 0 || k == 3) ? *component(&lo, a) - MIRROR_PLANE_MARGIN : *component(&hi, a) + MIRROR_PLANE_MARGIN;
		*component(&corners[k], b) = (k < 2) ? *component(&lo, b) - MIRROR_PLANE_MARGIN : *component(&hi, b) + MIRROR_PLANE_MARGIN;
	}
	gr_setcolor(BM_XRGB(31, 10, 31));
	for (k = 0; k < 4; k++)
		draw_world_line(&corners[k], &corners[(k + 1) % 4]);
	draw_world_line(&corners[0], &corners[2]);
	draw_world_line(&corners[1], &corners[3]);
}

static void draw_hover(void)
{
	if (!Hover.valid || Hover.segnum >= Num_segments)
		return;
	if (Builder.tool == BTOOL_BUILD)
	{
		gr_setcolor(BM_XRGB(10, 20, 31));
		draw_extrude_preview(&Segments[Hover.segnum], Hover.sidenum);
	}
	draw_element(&Hover, BM_XRGB(31, 31, 0), BM_XRGB(31, 20, 0));
}

static void draw_tags(void)
{
	int i;

	for (i = 0; i < builder_tag_count(); i++)
		draw_element(builder_tag(i), BM_XRGB(0, 31, 8), BM_XRGB(31, 0, 31));
}

/* Called inside render_frame(), so the 3D view is still set up. */
void builder_render_overlay(void)
{
	if (!Builder_on)
		return;

#ifdef OGL
	glDisable(GL_DEPTH_TEST);
	glLineWidth(2.0);
#endif
	draw_hover();
#ifdef OGL
	glLineWidth(3.0);
#endif
	draw_tags();
#ifdef OGL
	glLineWidth(1.5);
#endif
	draw_mirror_plane();
#ifdef OGL
	glLineWidth(1.0);
	glEnable(GL_DEPTH_TEST);
#endif
}

void builder_draw_hud(void)
{
	if (Builder_on)
		builder_draw_status_bar();
}

/* ------------------------------------------------------------- session */

static void reset_settings(void)
{
	memset(&Builder, 0, sizeof(Builder));
	Builder.tool = BTOOL_BUILD;
	Builder.sel_mode = BSEL_FACE;
	Builder.link_mode = BLINK_SYNC;
	Builder.nudge_step = 1.0f;
	Builder.build_depth = 20.0f;
	Builder.axis_snap = 1;
	builder_clear_tags();
	Hover.valid = 0;
}

void builder_end(void)
{
	if (!Builder_on)
		return;
	if (bld_is_dirty())
		builder_save_level();
	bld_history_clear();
	select_cockpit(PlayerCfg.PreferredCockpitMode);
	Builder_on = 0;
	bld_set_dirty(0);
}

static int prepare_level(void)
{
	int choice = 1;

	if (builder_level_exists())
		choice = nm_messagebox("Level Builder", 3, "Continue", "New level", "Cancel",
			"Continue your builder level, or start over?\n(Starting over overwrites it.)");
	if (choice < 0 || choice == 2)
		return 0;
	if (choice == 1 && !builder_write_starter_level())
	{
		nm_messagebox(NULL, 1, "Ok", "Couldn't write the builder level.");
		return 0;
	}
	return 1;
}

void builder_start_menu(void)
{
	if (!prepare_level())
		return;
	if (!load_mission_by_name(BUILDER_MISSION))
	{
		nm_messagebox(NULL, 1, "Ok", "Couldn't load the builder mission.");
		return;
	}

	bld_history_clear();
	reset_settings();
	Builder_on = 1;
	bld_set_dirty(0);
	StartNewGame(1);
	select_cockpit(CM_FULL_SCREEN);
	bld_bake_lighting();
}
