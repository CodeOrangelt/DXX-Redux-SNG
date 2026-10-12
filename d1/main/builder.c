/*
 * SNG: first-person level builder -- session, input, hover, 3D overlay.
 *
 * The crosshair hovers a face, edge or vertex. Left click applies the current
 * tool (Build by default) -- to every tag if you click one of them, else to
 * what you hover. Right click deletes, middle click selects, Shift+middle
 * click adds or removes a tag, and the wheel insets/outsets the tags (or the
 * hovered element). Precision edits live in the Tab tool panel.
 */

#include <math.h>
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
#include "timer.h"
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
#define DOUBLE_CLICK_TIME	(F1_0 / 3)
#define MIRROR_PLANE_MARGIN	(4 * F1_0)

static int Builder_on;
static int Bar_mode;
static int Painting;
static int Tagging;
static builder_element Last_middle;
static fix Last_middle_time;
static builder_element Last_tagged;
static int Wheel_open;
static int Wheel_dx, Wheel_dy;
static builder_element Last_painted;
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

static int shift_held(void)
{
	return keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT];
}

static void announce_selection(void)
{
	char text[96];

	builder_describe_selection(text, sizeof(text));
	bld_message(text);
}

/* ---------------------------------------------------------- mode wheel */

int builder_wheel_entries(int kind)
{
	if (kind == BWHEEL_CUT)
		return BUILDER_CUT_COUNT;
	if (kind >= BWHEEL_CARVE)
		return builder_carve_count(kind - BWHEEL_CARVE);
	if (kind >= BWHEEL_SHAPES)
		return builder_shape_count(kind - BWHEEL_SHAPES);
	return kind == BWHEEL_SELECT ? BSEL_COUNT : BTOOL_COUNT;
}

/* The number key held down (0-8), or -1. */
static int held_page(void)
{
	int page;

	for (page = 0; page < BUILDER_SHAPE_PAGES; page++)
		if (keyd_pressed[KEY_1 + page])
			return page;
	return -1;
}

/* ~ opens the tool wheel, V the select wheel, 1-9 a page of shapes, C carve, X cut. */
static int wheel_key_kind(void)
{
	int page = held_page();

	if (keyd_pressed[KEY_LAPOSTRO])
		return BWHEEL_MODES;
	if (keyd_pressed[KEY_V])
		return BWHEEL_SELECT;
	if (keyd_pressed[KEY_C])
		return BWHEEL_CARVE;
	if (keyd_pressed[KEY_X])
		return BWHEEL_CUT;
	return page < 0 ? BWHEEL_NONE : BWHEEL_SHAPES + page;
}

/* Entry under the mouse offset, clockwise from the top; -1 in the dead zone. */
static int wheel_choice(void)
{
	int entries = builder_wheel_entries(Wheel_open);
	double degrees;
	int sector;

	if (Wheel_dx * Wheel_dx + Wheel_dy * Wheel_dy < BUILDER_WHEEL_DEADZONE * BUILDER_WHEEL_DEADZONE)
		return -1;
	degrees = atan2(Wheel_dy, Wheel_dx) * 180.0 / M_PI + 90.0 + 180.0 / entries;
	degrees = fmod(degrees + 720.0, 360.0);
	sector = (int)(degrees / (360.0 / entries));
	return sector % entries;
}

int builder_wheel_active(int *kind, int *choice, int *dx, int *dy)
{
	if (!Wheel_open)
		return 0;
	*kind = Wheel_open;
	*choice = wheel_choice();
	*dx = Wheel_dx;
	*dy = Wheel_dy;
	return 1;
}

static void wheel_move(int dx, int dy)
{
	long length2;

	Wheel_dx += dx;
	Wheel_dy += dy;
	length2 = (long)Wheel_dx * Wheel_dx + (long)Wheel_dy * Wheel_dy;
	if (length2 > (long)BUILDER_WHEEL_REACH * BUILDER_WHEEL_REACH)
	{
		double scale = BUILDER_WHEEL_REACH / sqrt((double)length2);

		Wheel_dx = (int)(Wheel_dx * scale);
		Wheel_dy = (int)(Wheel_dy * scale);
	}
}

/* Shapes and carves act on the tags if there are any, else on the crosshair. */
static int has_target(void)
{
	if (builder_tag_count() || Hover.valid)
		return 1;
	bld_message("Aim at a wall, or tag a cube first");
	return 0;
}

static void commit_carve(int page, int choice)
{
	if (!has_target())
		return;
	if (builder_tag_count())
		builder_carve_selection(page, choice);
	else
		builder_carve_at(page, choice, &Hover);
}

static void commit_shape(int page, int choice)
{
	if (!has_target())
		return;
	if (builder_tag_count())
		builder_shape_selection(page, choice);
	else
		builder_shape_at(page, choice, &Hover);
}

static void wheel_commit(void)
{
	int choice = wheel_choice();

	if (Wheel_open == BWHEEL_MODES && choice < 0)
		return;
	if (Wheel_open == BWHEEL_SELECT)
	{
		builder_set_sel_mode(choice < 0 ? (Builder.sel_mode + 1) % BSEL_COUNT : choice);
		return;
	}
	if (choice < 0)
	{
		bld_message("Move the mouse toward an icon, then let go");
		return;
	}
	if (Wheel_open == BWHEEL_CUT)
		builder_cut_selection(choice);
	else if (Wheel_open >= BWHEEL_CARVE)
		commit_carve(Wheel_open - BWHEEL_CARVE, choice);
	else if (Wheel_open >= BWHEEL_SHAPES)
		commit_shape(Wheel_open - BWHEEL_SHAPES, choice);
	else
		builder_set_tool(choice);
}

/* Holding ~, V, 1-9, C or X opens a wheel; letting go picks what you pointed at. */
static void update_wheel(void)
{
	int kind = wheel_key_kind();

	if (kind && !Bar_mode)
	{
		if (Wheel_open != kind)
		{
			Wheel_open = kind;
			Wheel_dx = Wheel_dy = 0;
			Painting = 0;
			Tagging = 0;
		}
		return;
	}
	if (!Wheel_open)
		return;
	wheel_commit();
	Wheel_open = 0;
}

/* With tag drag on, holding the tag button and sweeping tags what you cross. */
static void drag_tag(void)
{
	if (Wheel_open || !Tagging || !Hover.valid)
		return;
	if (Hover.segnum == Last_tagged.segnum && Hover.sidenum == Last_tagged.sidenum &&
		Hover.corner == Last_tagged.corner)
		return;
	Last_tagged = Hover;
	builder_add_tag(&Hover);
}

static void toggle_tag_drag(void)
{
	Builder.tag_drag = !Builder.tag_drag;
	bld_message(Builder.tag_drag ? "Tag drag on" : "Tag drag off");
}

/* Plain selects, Shift adds or removes a tag: same in every tool. */
static void select_hover(void)
{
	if (!Hover.valid)
		return;
	if (shift_held())
		builder_toggle_tag(&Hover);
	else
		builder_select(&Hover);
	Tagging = Builder.tag_drag;
	Last_tagged = Hover;
	announce_selection();
}

/* Holding left in Paint sweeps over faces, painting each once. */
static void drag_paint(void)
{
	builder_element previous;

	if (Wheel_open || !Painting || Builder.tool != BTOOL_PAINT || !Hover.valid)
		return;
	if (Hover.segnum == Last_painted.segnum && Hover.sidenum == Last_painted.sidenum)
		return;
	previous = Last_painted;
	Last_painted = Hover;
	builder_paint_continuing(&Hover, &previous);
}

static void paint_click(int on_tags)
{
	if (Builder.paint_mode == BPAINT_FILL)
		builder_fill_at(&Hover);
	else if (on_tags)
		builder_paint_selection(0);
	else
	{
		Painting = 1;
		Last_painted = Hover;
		builder_paint_continuing(&Hover, NULL);
	}
}

/* P cycles Brush/Fill; Shift+P cycles what Fill covers. */
static void cycle_paint_mode(void)
{
	char text[48];

	if (shift_held())
		Builder.fill_scope = (Builder.fill_scope + 1) % BFILL_COUNT;
	else
		Builder.paint_mode = (Builder.paint_mode + 1) % BPAINT_COUNT;
	snprintf(text, sizeof(text), "Paint: %s%s%s", Builder_paint_names[Builder.paint_mode],
		Builder.paint_mode == BPAINT_FILL ? " - " : "",
		Builder.paint_mode == BPAINT_FILL ? Builder_fill_names[Builder.fill_scope] : "");
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
			select_hover();
			break;
		case BTOOL_BUILD:
			if (on_tags) builder_extrude_selection(); else builder_extrude_at(&Hover);
			break;
		case BTOOL_DELETE:
			if (on_tags) builder_delete_selection(); else builder_delete_at(&Hover);
			break;
		case BTOOL_PAINT:
			paint_click(on_tags);
			break;
		case BTOOL_PICK:
			builder_pick_texture_at(&Hover);
			break;
	}
}

/* A second middle click on the same face tags its whole wall. */
static int is_double_click(void)
{
	fix now = timer_query();
	int again = Last_middle.valid && now - Last_middle_time < DOUBLE_CLICK_TIME &&
		Hover.segnum == Last_middle.segnum && Hover.sidenum == Last_middle.sidenum;

	Last_middle = Hover;
	Last_middle_time = again ? 0 : now;
	return again;
}

static void tag_whole_wall(void)
{
	builder_element wall = Hover;

	wall.mode = BSEL_WALL;
	if (shift_held())
		builder_add_tag(&wall);
	else
		builder_select(&wall);
	Tagging = 0;
	announce_selection();
}

static void middle_click(void)
{
	if (!Hover.valid)
		return;
	if (is_double_click())
		tag_whole_wall();
	else
		select_hover();
}

/* Delete only where deleting is the tool's job; elsewhere it's a stray click. */
static void right_click(void)
{
	if (!Hover.valid)
		return;
	if (Builder.tool == BTOOL_PAINT)
	{
		builder_eyedrop(&Hover);
		return;
	}
	if (Builder.tool != BTOOL_BUILD && Builder.tool != BTOOL_DELETE)
	{
		bld_message("Right click deletes in Build and Delete tools");
		return;
	}
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

void builder_set_tool(int tool)
{
	Builder.tool = tool;
	HUD_init_message(HM_DEFAULT, "Tool: %s", Builder_tool_names[tool]);
}

void builder_set_sel_mode(int mode)
{
	Builder.sel_mode = mode;
	HUD_init_message(HM_DEFAULT, "Selecting: %s", Builder_sel_mode_names[Builder.sel_mode]);
}

void builder_cycle_link_mode(void)
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

static int handle_bar_mouse(d_event *event)
{
	int x, y, z;

	if (event_mouse_get_button(event) == MBTN_LEFT)
	{
		mouse_get_pos(&x, &y, &z);
		builder_bar_click(x, y);
	}
	return 1;
}

static int handle_mouse(d_event *event)
{
	if (Wheel_open)
		return 1;
	if (Bar_mode)
		return handle_bar_mouse(event);
	if (event_mouse_get_button(event) == MBTN_LEFT && builder_gizmo_begin())
		return 1;
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
	int base = key & 0xff;

	if ((base >= KEY_1 && base <= KEY_9) || base == KEY_C || base == KEY_X)
		return 1;	/* held for a wheel; keep weapon select out */
	switch (key)
	{
		case KEY_TAB:			builder_open_tools(); return 1;
		case KEY_T:			builder_open_textures(); return 1;
		case KEY_G:			toggle_tag_drag(); return 1;
		case KEY_L:			builder_uv_link(); return 1;
		case KEY_P:			cycle_paint_mode(); return 1;
		case KEY_V:			return 1;	/* held for the select wheel */
		case KEY_M:			builder_cycle_link_mode(); return 1;
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
		case EVENT_MOUSE_MOVED:
			if (builder_gizmo_dragging())
			{
				int dx, dy, dz;

				event_mouse_get_delta(event, &dx, &dy, &dz);
				builder_gizmo_move(dx, dy);
				return 1;
			}
			if (!Wheel_open)
				return 0;
			{
				int dx, dy, dz;

				event_mouse_get_delta(event, &dx, &dy, &dz);
				wheel_move(dx, dy);
			}
			return 1;
		case EVENT_MOUSE_BUTTON_UP:
			if (event_mouse_get_button(event) == MBTN_LEFT)
			{
				Painting = 0;
				builder_gizmo_end();
			}
			if (event_mouse_get_button(event) == MBTN_LEFT || event_mouse_get_button(event) == MBTN_MIDDLE)
				Tagging = 0;
			return is_builder_button(event);
		case EVENT_KEY_COMMAND:
			return handle_key(event_key_get(event));
		default:
			return 0;
	}
}

/* Hold Alt to free the mouse for the status bar. */
static void update_bar_mode(void)
{
	int want = keyd_pressed[KEY_LALT];

	if (want == Bar_mode)
		return;
	Bar_mode = want;
	mouse_toggle_relative(!Bar_mode);
	game_flush_inputs();
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
	if (Bar_mode)
	{
		Controls.pitch_time = 0;
		Controls.heading_time = 0;
		Controls.bank_time = 0;
	}
}

void builder_frame(void)
{
	if (!Builder_on)
		return;
	Players[Player_num].shields = MAX_SHIELDS;
	Players[Player_num].energy = MAX_ENERGY;
	update_bar_mode();
	update_wheel();
	if (Bar_mode)
		mouse_touch_cursor_time();
	builder_filter_controls();
	update_hover();
	drag_paint();
	drag_tag();
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
	int sidenum;

	gr_setcolor(face_color);
	if (el->mode == BSEL_CUBE)
	{
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			draw_face_outline(seg, sidenum, NULL);
		return;
	}
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
		int verts[BUILDER_ELEMENT_VERTS], n = bld_element_vertices(builder_tag(t), verts);

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

/* While the cut wheel points at a cut, show where it would land. */
static void draw_cut_preview(void)
{
	int choice;

	if (Wheel_open != BWHEEL_CUT)
		return;
	choice = wheel_choice();
	if (choice >= 0)
		builder_cut_preview(choice);
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
	draw_cut_preview();
	if (!Wheel_open)
		builder_gizmo_draw();
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
	int kind, choice, dx, dy;

	if (!Builder_on)
		return;
	builder_draw_status_bar();
	if (builder_wheel_active(&kind, &choice, &dx, &dy))
		builder_draw_wheel(kind, choice, dx, dy);
}

/* ------------------------------------------------------------- session */

static void reset_settings(void)
{
	memset(&Builder, 0, sizeof(Builder));
	Builder.tool = BTOOL_BUILD;
	Builder.sel_mode = BSEL_FACE;
	Builder.texture2 = 1;
	Builder.link_mode = BLINK_SYNC;
	Builder.nudge_step = 1.0f;
	Builder.build_depth = BUILDER_CUBE_UNITS;
	Builder.cut_neighbours = 0;
	Builder.axis_snap = 1;
	builder_clear_tags();
	Hover.valid = 0;
}

void builder_end(void)
{
	if (!Builder_on)
		return;
	Wheel_open = 0;
	if (Bar_mode)
	{
		Bar_mode = 0;
		mouse_toggle_relative(1);
	}
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
