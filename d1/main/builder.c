/*
 * SNG: first-person level builder -- editing core.
 *
 * The crosshair hovers a face, edge or vertex; clicking applies the current
 * tool, and right-click selects. Precision edits (nudges, snapping,
 * flattening, welding, texture alignment) act on the selection and live in
 * the Tab tool panel (builder_ui.c).
 *
 * Every geometry edit takes an undo snapshot first and rolls itself back if
 * it would turn a segment inside out or strand the player outside the mine,
 * so an invalid level can't be built by accident.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "u_mem.h"
#include "inferno.h"
#include "segment.h"
#include "gameseg.h"
#include "object.h"
#include "wall.h"
#include "switch.h"
#include "fuelcen.h"
#include "cntrlcen.h"
#include "fvi.h"
#include "3d.h"
#include "gr.h"
#include "gamefont.h"
#include "textures.h"
#include "bm.h"
#include "piggy.h"
#include "player.h"
#include "game.h"
#include "gamesave.h"
#include "hudmsg.h"
#include "kconfig.h"
#include "key.h"
#include "mouse.h"
#include "event.h"
#include "mission.h"
#include "gameseq.h"
#include "menu.h"
#include "newmenu.h"
#include "playsave.h"
#include "builder.h"
#include "builder_int.h"
#ifdef OGL
#include "ogl_init.h"
#endif

#define BUILDER_REACH		(600 * F1_0)
#define WELD_DISTANCE		(F1_0 / 4)
#define MIN_SIDE_DEPTH		(F1_0 / 2)
#define TEXTURE_WORLD_SIZE	(20 * F1_0)
#define AXIS_SNAP_DOT		(F1_0 * 97 / 100)
#define MAX_UNDO		64
#define MAX_LIGHT_SOURCES	512
#define LIGHT_RANGE		(100 * F1_0)
#define LIGHT_AMBIENT		(F1_0 * 40 / 100)
#define LIGHT_SKY_SHADE		(F1_0 * 15 / 100)
#define VERTEX_MARK_SIZE	(F1_0 * 3 / 2)
#define MAX_SELECTED_VERTS	4

typedef struct builder_snapshot {
	int num_segments;
	int num_vertices;
	segment *segments;
	vms_vector *vertices;
	int num_walls;
	wall walls[MAX_WALLS];
	trigger triggers[MAX_TRIGGERS];
	control_center_triggers cc_triggers;
	FuelCenter stations[MAX_NUM_FUELCENS];
	matcen_info robot_centers[MAX_ROBOT_CENTERS];
} builder_snapshot;

typedef struct snapshot_stack {
	builder_snapshot *items[MAX_UNDO];
	int count;
} snapshot_stack;

builder_settings Builder;

const char *const Builder_tool_names[BTOOL_COUNT] = {"Select", "Build", "Delete", "Paint", "Pick"};
const char *const Builder_sel_mode_names[BSEL_COUNT] = {"Face", "Edge", "Vertex"};
static const char *const Side_names[MAX_SIDES_PER_SEGMENT] = {"left", "top", "right", "bottom", "back", "front"};

static int Builder_on;
static int Builder_dirty;
static builder_element Hover;
static builder_element Selection;
static snapshot_stack Undo;
static snapshot_stack Redo;

int builder_active(void)
{
	return Builder_on;
}

int builder_is_dirty(void)
{
	return Builder_dirty;
}

const builder_element *builder_selection(void)
{
	return &Selection;
}

static fix units(float world_units)
{
	return fl2f(world_units);
}

/* ----------------------------------------------------------- topology */

static int side_has_vertex(int sidenum, int v)
{
	int k;

	for (k = 0; k < 4; k++)
		if (Side_to_verts[sidenum][k] == v)
			return 1;
	return 0;
}

static int vertices_share_edge(int a, int b)
{
	int sidenum, k;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		for (k = 0; k < 4; k++)
		{
			int p = Side_to_verts[sidenum][k];
			int q = Side_to_verts[sidenum][(k + 1) % 4];

			if ((p == a && q == b) || (p == b && q == a))
				return 1;
		}
	return 0;
}

/* The vertex across the segment from v, along the edge leaving sidenum. */
static int partner_vertex(int sidenum, int v)
{
	int other;

	for (other = 0; other < MAX_VERTICES_PER_SEGMENT; other++)
		if (!side_has_vertex(sidenum, other) && vertices_share_edge(v, other))
			return other;
	return -1;
}

static int side_vertnum(segment *seg, int sidenum, int k)
{
	return seg->verts[Side_to_verts[sidenum][k]];
}

static vms_vector *side_vertex(segment *seg, int sidenum, int k)
{
	return &Vertices[side_vertnum(seg, sidenum, k)];
}

/* Inward-facing unit normal; validate_segment() keeps normals[] current. */
static void side_normal(segment *seg, int sidenum, vms_vector *normal)
{
	vm_vec_avg(normal, &seg->sides[sidenum].normals[0], &seg->sides[sidenum].normals[1]);
	vm_vec_normalize(normal);
}

static int side_is_solid(segment *seg, int sidenum)
{
	return seg->children[sidenum] == -1 || seg->sides[sidenum].wall_num != -1;
}

/* The vertex numbers an element covers: 4 for a face, 2 for an edge, 1 for
 * a vertex. */
static int element_vertices(const builder_element *el, int *out)
{
	segment *seg = &Segments[el->segnum];
	int k;

	switch (Builder.sel_mode)
	{
		case BSEL_VERTEX:
			out[0] = side_vertnum(seg, el->sidenum, el->corner);
			return 1;
		case BSEL_EDGE:
			out[0] = side_vertnum(seg, el->sidenum, el->corner);
			out[1] = side_vertnum(seg, el->sidenum, (el->corner + 1) % 4);
			return 2;
		default:
			for (k = 0; k < 4; k++)
				out[k] = side_vertnum(seg, el->sidenum, k);
			return 4;
	}
}

/* ----------------------------------------------------------- feedback */

static void builder_message(const char *text)
{
	HUD_init_message(HM_DEFAULT, "%s", text);
}

static int builder_refuse(const char *reason)
{
	builder_message(reason);
	return 0;
}

static int need_selection(void)
{
	if (Selection.valid && Selection.segnum < Num_segments)
		return 1;
	return builder_refuse("Select something first (right-click)");
}

void builder_describe_selection(char *out, int size)
{
	if (!Selection.valid)
	{
		snprintf(out, size, "nothing selected");
		return;
	}
	if (Builder.sel_mode == BSEL_FACE)
		snprintf(out, size, "cube %d, %s face", Selection.segnum, Side_names[Selection.sidenum]);
	else
		snprintf(out, size, "cube %d, %s face, %s %d", Selection.segnum, Side_names[Selection.sidenum],
			Builder.sel_mode == BSEL_EDGE ? "edge" : "corner", Selection.corner + 1);
}

/* ------------------------------------------------------------- undo */

static void free_snapshot(builder_snapshot *snap)
{
	d_free(snap->segments);
	d_free(snap->vertices);
	d_free(snap);
}

static builder_snapshot *take_snapshot(void)
{
	builder_snapshot *snap;

	MALLOC(snap, builder_snapshot, 1);
	if (!snap)
		return NULL;
	MALLOC(snap->segments, segment, Num_segments);
	MALLOC(snap->vertices, vms_vector, Num_vertices);
	if (!snap->segments || !snap->vertices)
	{
		free_snapshot(snap);
		return NULL;
	}

	snap->num_segments = Num_segments;
	snap->num_vertices = Num_vertices;
	memcpy(snap->segments, Segments, sizeof(segment) * Num_segments);
	memcpy(snap->vertices, Vertices, sizeof(vms_vector) * Num_vertices);
	snap->num_walls = Num_walls;
	memcpy(snap->walls, Walls, sizeof(snap->walls));
	memcpy(snap->triggers, Triggers, sizeof(snap->triggers));
	snap->cc_triggers = ControlCenterTriggers;
	memcpy(snap->stations, Station, sizeof(snap->stations));
	memcpy(snap->robot_centers, RobotCenters, sizeof(snap->robot_centers));
	return snap;
}

static void stack_push(snapshot_stack *stack, builder_snapshot *snap)
{
	if (stack->count == MAX_UNDO)
	{
		free_snapshot(stack->items[0]);
		memmove(stack->items, stack->items + 1, sizeof(stack->items[0]) * (MAX_UNDO - 1));
		stack->count--;
	}
	stack->items[stack->count++] = snap;
}

static builder_snapshot *stack_pop(snapshot_stack *stack)
{
	return stack->count ? stack->items[--stack->count] : NULL;
}

static void stack_clear(snapshot_stack *stack)
{
	while (stack->count > 0)
		free_snapshot(stack->items[--stack->count]);
}

int builder_undo_depth(void)
{
	return Undo.count;
}

int builder_redo_depth(void)
{
	return Redo.count;
}

/* Every edit starts here. A new edit forks history, so redo is dropped. */
static int begin_edit(void)
{
	builder_snapshot *snap = take_snapshot();

	if (!snap)
		return builder_refuse("Builder: out of memory for undo");
	stack_push(&Undo, snap);
	stack_clear(&Redo);
	return 1;
}

/* Segment object lists are stale after a restore; rebuild them from positions. */
static void relink_all_objects(void)
{
	int i;

	for (i = 0; i < Num_segments; i++)
		Segments[i].objects = -1;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *obj = &Objects[i];
		int segnum;

		if (obj->type == OBJ_NONE)
			continue;
		segnum = obj->segnum < Num_segments ? obj->segnum : 0;
		segnum = find_point_seg(&obj->pos, segnum);
		if (segnum == -1)
			segnum = 0;
		obj->next = obj->prev = -1;
		obj->segnum = -1;
		obj_link(i, segnum);
	}
}

static void restore_snapshot(builder_snapshot *snap)
{
	Num_segments = snap->num_segments;
	Highest_segment_index = Num_segments - 1;
	Num_vertices = snap->num_vertices;
	Highest_vertex_index = Num_vertices - 1;
	memcpy(Segments, snap->segments, sizeof(segment) * Num_segments);
	memcpy(Vertices, snap->vertices, sizeof(vms_vector) * Num_vertices);
	Num_walls = snap->num_walls;
	memcpy(Walls, snap->walls, sizeof(snap->walls));
	memcpy(Triggers, snap->triggers, sizeof(snap->triggers));
	ControlCenterTriggers = snap->cc_triggers;
	memcpy(Station, snap->stations, sizeof(snap->stations));
	memcpy(RobotCenters, snap->robot_centers, sizeof(snap->robot_centers));
	validate_segment_all();
	relink_all_objects();
	if (Selection.segnum >= Num_segments)
		Selection.valid = 0;
	Hover.valid = 0;
	Builder_dirty = 1;
}

/* Move the current state onto `save`, then restore the top of `from`. */
static int step_history(snapshot_stack *from, snapshot_stack *save)
{
	builder_snapshot *target = stack_pop(from);
	builder_snapshot *current;

	if (!target)
		return 0;
	current = take_snapshot();
	if (current)
		stack_push(save, current);
	restore_snapshot(target);
	free_snapshot(target);
	return 1;
}

void builder_undo(void)
{
	builder_message(step_history(&Undo, &Redo) ? "Undone" : "Nothing to undo");
}

void builder_redo(void)
{
	builder_message(step_history(&Redo, &Undo) ? "Redone" : "Nothing to redo");
}

/* A failed edit leaves no trace: not in undo, not in redo. */
static void rollback_edit(void)
{
	builder_snapshot *snap = stack_pop(&Undo);

	if (!snap)
		return;
	restore_snapshot(snap);
	free_snapshot(snap);
}

/* ---------------------------------------------------------- texturing */

/* World-aligned planar mapping: neighbouring faces line up seamlessly. */
static void map_side_uvs(segment *seg, int sidenum)
{
	vms_vector normal, forward, up, right;
	int k;

	side_normal(seg, sidenum, &normal);
	vm_vec_copy_scale(&forward, &normal, -F1_0);
	vm_vec_make(&up, 0, F1_0, 0);
	if (labs(normal.y) > F1_0 * 7 / 10)
		vm_vec_make(&up, 0, 0, F1_0);

	vm_vec_cross(&right, &up, &forward);
	vm_vec_normalize(&right);
	vm_vec_cross(&up, &forward, &right);
	vm_vec_normalize(&up);

	for (k = 0; k < 4; k++)
	{
		vms_vector *p = side_vertex(seg, sidenum, k);

		seg->sides[sidenum].uvls[k].u = fixdiv(vm_vec_dot(p, &right), TEXTURE_WORLD_SIZE);
		seg->sides[sidenum].uvls[k].v = -fixdiv(vm_vec_dot(p, &up), TEXTURE_WORLD_SIZE);
	}
}

static void map_segment_uvs(int segnum)
{
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		map_side_uvs(&Segments[segnum], sidenum);
}

static void paint_side(segment *seg, int sidenum)
{
	seg->sides[sidenum].tmap_num = Builder.texture;
	seg->sides[sidenum].tmap_num2 = 0;
	map_side_uvs(seg, sidenum);
}

/* ----------------------------------------------------------- lighting */

typedef struct light_source {
	vms_vector pos;
	fix intensity;
} light_source;

static fix texture_light(side *sidep)
{
	fix light = TmapInfo[sidep->tmap_num].lighting;

	if (sidep->tmap_num2)
		light += TmapInfo[sidep->tmap_num2 & 0x3fff].lighting;
	return light;
}

static int gather_lights(light_source *lights)
{
	int segnum, sidenum, count = 0;

	for (segnum = 0; segnum < Num_segments; segnum++)
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		{
			segment *seg = &Segments[segnum];
			vms_vector normal;
			fix intensity;

			if (!side_is_solid(seg, sidenum) || count == MAX_LIGHT_SOURCES)
				continue;
			intensity = texture_light(&seg->sides[sidenum]);
			if (intensity <= 0)
				continue;
			compute_center_point_on_side(&lights[count].pos, seg, sidenum);
			side_normal(seg, sidenum, &normal);
			vm_vec_scale_add2(&lights[count].pos, &normal, F1_0);
			lights[count].intensity = intensity;
			count++;
		}
	return count;
}

static fix light_at(vms_vector *point, vms_vector *normal, light_source *lights, int n_lights)
{
	fix light = LIGHT_AMBIENT + fixmul(LIGHT_SKY_SHADE, normal->y);
	int i;

	for (i = 0; i < n_lights; i++)
	{
		vms_vector *pos = &lights[i].pos;
		fix dist;

		if (labs(pos->x - point->x) > LIGHT_RANGE || labs(pos->y - point->y) > LIGHT_RANGE ||
			labs(pos->z - point->z) > LIGHT_RANGE)
			continue;
		dist = vm_vec_dist(point, pos);
		if (dist < LIGHT_RANGE)
			light += fixmul(lights[i].intensity, fixdiv(LIGHT_RANGE - dist, LIGHT_RANGE));
	}
	return light > F1_0 ? F1_0 : light;
}

static void light_segment(segment *seg, light_source *lights, int n_lights)
{
	fix total = 0;
	int sidenum, k;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		vms_vector normal;

		side_normal(seg, sidenum, &normal);
		for (k = 0; k < 4; k++)
		{
			fix l = Builder.fullbright ? F1_0 : light_at(side_vertex(seg, sidenum, k), &normal, lights, n_lights);

			seg->sides[sidenum].uvls[k].l = l;
			total += l;
		}
	}
	seg->static_light = total / (MAX_SIDES_PER_SEGMENT * 4);
}

/* Cheap stand-in for the editor's radiosity: ambient, a sky tint by facing,
 * and distance falloff from every light-emitting texture. No occlusion. */
static void bake_lighting(void)
{
	light_source *lights;
	int n_lights, segnum;

	MALLOC(lights, light_source, MAX_LIGHT_SOURCES);
	if (!lights)
		return;
	n_lights = gather_lights(lights);
	for (segnum = 0; segnum < Num_segments; segnum++)
		light_segment(&Segments[segnum], lights, n_lights);
	d_free(lights);
}

void builder_set_fullbright(int on)
{
	Builder.fullbright = on;
	bake_lighting();
}

void builder_rebake(void)
{
	bake_lighting();
	builder_message("Lighting rebuilt");
}

/* ----------------------------------------------------------- validity */

static int segment_is_sane(segment *seg)
{
	vms_vector center;
	int sidenum, n;

	compute_segment_center(&center, seg);
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		for (n = 0; n < 2; n++)
		{
			vms_vector to_center;

			vm_vec_sub(&to_center, &center, side_vertex(seg, sidenum, n * 2));
			if (vm_vec_dot(&to_center, &seg->sides[sidenum].normals[n]) < MIN_SIDE_DEPTH)
				return 0;
		}
	return 1;
}

static int segment_has_duplicate_vertex(segment *seg)
{
	int a, b;

	for (a = 0; a < MAX_VERTICES_PER_SEGMENT; a++)
		for (b = a + 1; b < MAX_VERTICES_PER_SEGMENT; b++)
			if (seg->verts[a] == seg->verts[b])
				return 1;
	return 0;
}

static int segment_uses_any(segment *seg, int *vertnums, int count)
{
	int i, v;

	for (i = 0; i < count; i++)
		for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
			if (seg->verts[v] == vertnums[i])
				return 1;
	return 0;
}

/* Revalidate and remap every segment touching the moved vertices. */
static int refresh_after_move(int *vertnums, int count)
{
	int segnum;

	for (segnum = 0; segnum < Num_segments; segnum++)
	{
		segment *seg = &Segments[segnum];

		if (!segment_uses_any(seg, vertnums, count))
			continue;
		validate_segment(seg);
		if (!segment_is_sane(seg))
			return 0;
		map_segment_uvs(segnum);
	}
	return 1;
}

static int player_still_inside(void)
{
	int segnum = find_point_seg(&ConsoleObject->pos, ConsoleObject->segnum);

	if (segnum == -1)
		return 0;
	if (segnum != ConsoleObject->segnum)
		obj_relink(ConsoleObject - Objects, segnum);
	return 1;
}

/* Every edit ends here: keep it, or roll it back with a reason. */
static int finish_edit(int ok, const char *failure)
{
	Hover.valid = 0;
	if (!ok || !player_still_inside())
	{
		rollback_edit();
		return builder_refuse(ok ? "Can't do that with you in the way" : failure);
	}
	bake_lighting();
	Builder_dirty = 1;
	return 1;
}

/* ---------------------------------------------------------- vertices */

static int find_or_add_vertex(vms_vector *pos)
{
	int v;

	for (v = 0; v < Num_vertices; v++)
		if (vm_vec_dist(&Vertices[v], pos) < WELD_DISTANCE)
			return v;
	if (Num_vertices >= MAX_VERTICES)
		return -1;
	Vertices[Num_vertices] = *pos;
	Highest_vertex_index = Num_vertices;
	return Num_vertices++;
}

/* Grid-aligned rooms stay grid-aligned: snap near-axis directions to the axis. */
static void snap_to_axis(vms_vector *dir)
{
	fix ax = labs(dir->x), ay = labs(dir->y), az = labs(dir->z);

	if (ax >= AXIS_SNAP_DOT)
		vm_vec_make(dir, dir->x > 0 ? F1_0 : -F1_0, 0, 0);
	else if (ay >= AXIS_SNAP_DOT)
		vm_vec_make(dir, 0, dir->y > 0 ? F1_0 : -F1_0, 0);
	else if (az >= AXIS_SNAP_DOT)
		vm_vec_make(dir, 0, 0, dir->z > 0 ? F1_0 : -F1_0);
}

static void outward_step(segment *seg, int sidenum, fix distance, vms_vector *step)
{
	side_normal(seg, sidenum, step);
	vm_vec_scale(step, -F1_0);
	if (Builder.axis_snap)
		snap_to_axis(step);
	vm_vec_scale(step, distance);
}

static int move_vertices(int *vertnums, int count, vms_vector *delta)
{
	int i;

	for (i = 0; i < count; i++)
		vm_vec_add2(&Vertices[vertnums[i]], delta);
	return refresh_after_move(vertnums, count);
}

/* ---------------------------------------------------------- segments */

static void init_new_segment(segment *seg, segment *style, int style_side)
{
	int sidenum;

	memset(seg, 0, sizeof(*seg));
	seg->objects = -1;
	seg->static_light = F1_0;
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		seg->children[sidenum] = -1;
		seg->sides[sidenum].wall_num = -1;
		seg->sides[sidenum].tmap_num = style->sides[style_side].tmap_num;
		seg->sides[sidenum].tmap_num2 = style->sides[style_side].tmap_num2;
	}
}

static int same_vertex_set(int *a, int *b)
{
	int i, j, found;

	for (i = 0; i < 4; i++)
	{
		found = 0;
		for (j = 0; j < 4; j++)
			if (a[i] == b[j])
				found = 1;
		if (!found)
			return 0;
	}
	return 1;
}

/* A face landing exactly on another open face joins it, which is how loops
 * and multi-cube rooms close up. */
static void join_coincident_sides(int segnum)
{
	segment *seg = &Segments[segnum];
	int sidenum, other, other_side;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		int verts[4];

		if (seg->children[sidenum] != -1)
			continue;
		get_side_verts(verts, segnum, sidenum);
		for (other = 0; other < Num_segments; other++)
		{
			if (other == segnum)
				continue;
			for (other_side = 0; other_side < MAX_SIDES_PER_SEGMENT; other_side++)
			{
				int other_verts[4];

				if (Segments[other].children[other_side] != -1)
					continue;
				get_side_verts(other_verts, other, other_side);
				if (!same_vertex_set(verts, other_verts))
					continue;
				seg->children[sidenum] = other;
				Segments[other].children[other_side] = segnum;
			}
		}
	}
}

static int extrude(int segnum, int sidenum)
{
	segment *seg = &Segments[segnum];
	segment *fresh;
	vms_vector step;
	int newnum = Num_segments, k;

	if (seg->children[sidenum] != -1 || seg->sides[sidenum].wall_num != -1)
		return builder_refuse("That face is open");
	if (Num_segments >= MAX_SEGMENTS)
		return builder_refuse("Segment limit reached");
	if (!begin_edit())
		return 0;

	outward_step(seg, sidenum, units(Builder.build_depth), &step);
	fresh = &Segments[newnum];
	init_new_segment(fresh, seg, sidenum);

	for (k = 0; k < 4; k++)
	{
		int near_v = Side_to_verts[sidenum][k];
		int far_v = partner_vertex(sidenum, near_v);
		vms_vector pos;

		vm_vec_add(&pos, &Vertices[seg->verts[near_v]], &step);
		fresh->verts[far_v] = seg->verts[near_v];
		fresh->verts[near_v] = find_or_add_vertex(&pos);
		if (fresh->verts[near_v] < 0)
			return finish_edit(0, "Vertex limit reached");
	}

	Num_segments++;
	Highest_segment_index = newnum;
	seg->children[sidenum] = newnum;
	fresh->children[(int)Side_opposite[sidenum]] = segnum;
	validate_segment(fresh);
	join_coincident_sides(newnum);
	map_segment_uvs(newnum);
	return finish_edit(segment_is_sane(fresh) && !segment_has_duplicate_vertex(fresh), "No room to build there");
}

static void retarget_segment_refs(int from, int to)
{
	int i, l;

	for (i = 0; i < Num_walls; i++)
		if (Walls[i].segnum == from)
			Walls[i].segnum = to;
	for (i = 0; i < Num_triggers; i++)
		for (l = 0; l < Triggers[i].num_links; l++)
			if (Triggers[i].seg[l] == from)
				Triggers[i].seg[l] = to;
	for (l = 0; l < ControlCenterTriggers.num_links; l++)
		if (ControlCenterTriggers.seg[l] == from)
			ControlCenterTriggers.seg[l] = to;
	for (i = 0; i < Num_fuelcenters; i++)
		if (Station[i].segnum == from)
			Station[i].segnum = to;
	for (i = 0; i < Num_robot_centers; i++)
		if (RobotCenters[i].segnum == from)
			RobotCenters[i].segnum = to;
}

/* Segments are a dense array: fill the hole with the last one. */
static void move_segment(int from, int to)
{
	segment *seg = &Segments[to];
	int sidenum, other_side, objnum;

	*seg = Segments[from];
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		int child = seg->children[sidenum];

		if (child < 0)
			continue;
		for (other_side = 0; other_side < MAX_SIDES_PER_SEGMENT; other_side++)
			if (Segments[child].children[other_side] == from)
				Segments[child].children[other_side] = to;
	}
	for (objnum = seg->objects; objnum != -1; objnum = Objects[objnum].next)
		Objects[objnum].segnum = to;
	retarget_segment_refs(from, to);
}

static const char *delete_blocker(int segnum)
{
	segment *seg = &Segments[segnum];
	int sidenum;

	if (Num_segments <= 1)
		return "Can't delete the last cube";
	if (segnum == ConsoleObject->segnum)
		return "You're inside that cube";
	if (seg->special != SEGMENT_IS_NOTHING)
		return "That cube is a special room";
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		int child = seg->children[sidenum];

		if (seg->sides[sidenum].wall_num != -1)
			return "That cube has a door or wall";
		if (child >= 0 && Segments[child].sides[find_connect_side(seg, &Segments[child])].wall_num != -1)
			return "That cube has a door or wall";
	}
	return NULL;
}

/* Close every opening into segnum, so its neighbours become solid there. */
static void seal_neighbours(int segnum)
{
	segment *seg = &Segments[segnum];
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		int child = seg->children[sidenum];
		int facing;

		if (child < 0)
			continue;
		facing = find_connect_side(seg, &Segments[child]);
		Segments[child].children[facing] = -1;
		Segments[child].sides[facing].tmap_num = seg->sides[(int)Side_opposite[sidenum]].tmap_num;
		Segments[child].sides[facing].tmap_num2 = 0;
		map_side_uvs(&Segments[child], facing);
	}
}

static void delete_segment_objects(int segnum)
{
	while (Segments[segnum].objects != -1)
		obj_delete(Segments[segnum].objects);
}

static int delete_segment(int segnum)
{
	const char *blocker = delete_blocker(segnum);
	int last = Num_segments - 1;

	if (blocker)
		return builder_refuse(blocker);
	if (!begin_edit())
		return 0;

	seal_neighbours(segnum);
	delete_segment_objects(segnum);
	if (segnum != last)
		move_segment(last, segnum);
	Num_segments--;
	Highest_segment_index = Num_segments - 1;
	Selection.valid = 0;
	return finish_edit(1, NULL);
}

/* ------------------------------------------------- selection operations */

static void nudge_selection(vms_vector *delta)
{
	int verts[MAX_SELECTED_VERTS], count;

	if (!need_selection() || !begin_edit())
		return;
	count = element_vertices(&Selection, verts);
	finish_edit(move_vertices(verts, count, delta), "That would turn a cube inside out");
}

void builder_nudge_axis(int axis, int direction)
{
	vms_vector delta = {0, 0, 0};
	fix step = units(Builder.nudge_step) * direction;

	if (axis == 0)
		delta.x = step;
	else if (axis == 1)
		delta.y = step;
	else
		delta.z = step;
	nudge_selection(&delta);
}

void builder_nudge_normal(int direction)
{
	vms_vector delta;

	if (!need_selection())
		return;
	side_normal(&Segments[Selection.segnum], Selection.sidenum, &delta);
	vm_vec_scale(&delta, -units(Builder.nudge_step) * direction);
	nudge_selection(&delta);
}

static fix snap_value(fix value, fix grid)
{
	return fixmul(fl2f(floor(f2fl(fixdiv(value, grid)) + 0.5)), grid);
}

void builder_snap_selection(void)
{
	int verts[MAX_SELECTED_VERTS], count, i;
	fix grid = units(Builder.nudge_step);

	if (grid <= 0 || !need_selection() || !begin_edit())
		return;
	count = element_vertices(&Selection, verts);
	for (i = 0; i < count; i++)
	{
		vms_vector *v = &Vertices[verts[i]];

		v->x = snap_value(v->x, grid);
		v->y = snap_value(v->y, grid);
		v->z = snap_value(v->z, grid);
	}
	finish_edit(refresh_after_move(verts, count), "Snapping would turn a cube inside out");
}

/* Plane of the selected face, through the corners that are NOT selected (or
 * all four in face mode), so a stray vertex or edge is pulled back in line. */
static void selection_plane(vms_vector *point, vms_vector *normal, int *selected, int n_selected)
{
	segment *seg = &Segments[Selection.segnum];
	int k, i, used = 0;

	vm_vec_zero(point);
	for (k = 0; k < 4; k++)
	{
		int vertnum = side_vertnum(seg, Selection.sidenum, k);
		int is_selected = 0;

		for (i = 0; i < n_selected; i++)
			if (selected[i] == vertnum)
				is_selected = 1;
		if (is_selected && n_selected < 4)
			continue;
		vm_vec_add2(point, &Vertices[vertnum]);
		used++;
	}
	vm_vec_scale2(point, F1_0, i2f(used));
	side_normal(seg, Selection.sidenum, normal);
}

void builder_flatten_selection(void)
{
	int verts[MAX_SELECTED_VERTS], count, i;
	vms_vector point, normal;

	if (!need_selection() || !begin_edit())
		return;
	count = element_vertices(&Selection, verts);
	selection_plane(&point, &normal, verts, count);
	for (i = 0; i < count; i++)
	{
		vms_vector offset;
		fix dist;

		vm_vec_sub(&offset, &Vertices[verts[i]], &point);
		dist = vm_vec_dot(&offset, &normal);
		vm_vec_scale_add2(&Vertices[verts[i]], &normal, -dist);
	}
	finish_edit(refresh_after_move(verts, count), "Flattening would turn a cube inside out");
}

static int nearest_other_vertex(int vertnum, int *exclude, int n_exclude, fix radius)
{
	fix best_dist = radius;
	int best = -1, v, i;

	for (v = 0; v < Num_vertices; v++)
	{
		fix dist;
		int skip = 0;

		for (i = 0; i < n_exclude; i++)
			if (exclude[i] == v)
				skip = 1;
		if (skip)
			continue;
		dist = vm_vec_dist(&Vertices[v], &Vertices[vertnum]);
		if (dist <= best_dist)
		{
			best = v;
			best_dist = dist;
		}
	}
	return best;
}

static void replace_vertex(int from, int to)
{
	int segnum, v;

	for (segnum = 0; segnum < Num_segments; segnum++)
		for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
			if (Segments[segnum].verts[v] == from)
				Segments[segnum].verts[v] = to;
}

static int all_segments_valid_after_weld(void)
{
	int segnum;

	for (segnum = 0; segnum < Num_segments; segnum++)
	{
		segment *seg = &Segments[segnum];

		if (segment_has_duplicate_vertex(seg))
			return 0;
		validate_segment(seg);
		if (!segment_is_sane(seg))
			return 0;
	}
	for (segnum = 0; segnum < Num_segments; segnum++)
	{
		join_coincident_sides(segnum);
		map_segment_uvs(segnum);
	}
	return 1;
}

/* Merge each selected vertex into the nearest other vertex within one nudge
 * step, then join any faces that now coincide. Closes hairline gaps. */
void builder_weld_selection(void)
{
	int verts[MAX_SELECTED_VERTS], count, i, welded = 0;

	if (!need_selection() || !begin_edit())
		return;
	count = element_vertices(&Selection, verts);
	for (i = 0; i < count; i++)
	{
		int target = nearest_other_vertex(verts[i], verts, count, units(Builder.nudge_step));

		if (target < 0)
			continue;
		replace_vertex(verts[i], target);
		welded++;
	}
	if (!welded)
	{
		rollback_edit();
		builder_refuse("No vertex within one step to weld to");
		return;
	}
	if (finish_edit(all_segments_valid_after_weld(), "Welding would collapse a cube"))
		HUD_init_message(HM_DEFAULT, "Welded %d vertex%s", welded, welded == 1 ? "" : "es");
}

void builder_extrude_selection(void)
{
	if (need_selection())
		extrude(Selection.segnum, Selection.sidenum);
}

void builder_delete_selection(void)
{
	if (need_selection())
		delete_segment(Selection.segnum);
}

/* --------------------------------------------------- texture operations */

void builder_cycle_texture(int delta)
{
	Builder.texture = (Builder.texture + delta) % NumTextures;
	if (Builder.texture < 0)
		Builder.texture += NumTextures;
}

static void paint_element(const builder_element *el, int whole_segment)
{
	segment *seg = &Segments[el->segnum];
	int sidenum;

	if (!begin_edit())
		return;
	if (!whole_segment)
		paint_side(seg, el->sidenum);
	else
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			if (side_is_solid(seg, sidenum))
				paint_side(seg, sidenum);
	finish_edit(1, NULL);
}

void builder_paint_selection(int whole_segment)
{
	if (need_selection())
		paint_element(&Selection, whole_segment);
}

static void pick_texture_from(const builder_element *el)
{
	Builder.texture = Segments[el->segnum].sides[el->sidenum].tmap_num;
}

void builder_pick_texture(void)
{
	if (need_selection())
		pick_texture_from(&Selection);
}

static side *selected_side(void)
{
	return &Segments[Selection.segnum].sides[Selection.sidenum];
}

static void edit_selected_uvs(void (*change)(uvl *corner, void *arg), void *arg)
{
	side *sidep;
	int k;

	if (!need_selection() || !begin_edit())
		return;
	sidep = selected_side();
	for (k = 0; k < 4; k++)
		change(&sidep->uvls[k], arg);
	finish_edit(1, NULL);
}

static void uv_shift(uvl *corner, void *arg)
{
	fix *delta = arg;

	corner->u += delta[0];
	corner->v += delta[1];
}

static void uv_rotate(uvl *corner, void *arg)
{
	fix u = corner->u;

	(void)arg;
	corner->u = corner->v;
	corner->v = -u;
}

static void uv_flip(uvl *corner, void *arg)
{
	if (*(int *)arg)
		corner->v = -corner->v;
	else
		corner->u = -corner->u;
}

void builder_uv_shift(fix du, fix dv)
{
	fix delta[2] = {du, dv};

	edit_selected_uvs(uv_shift, delta);
}

void builder_uv_rotate(void)
{
	edit_selected_uvs(uv_rotate, NULL);
}

void builder_uv_flip(int flip_v)
{
	edit_selected_uvs(uv_flip, &flip_v);
}

void builder_uv_reset(void)
{
	if (!need_selection() || !begin_edit())
		return;
	map_side_uvs(&Segments[Selection.segnum], Selection.sidenum);
	finish_edit(1, NULL);
}

/* ----------------------------------------------------------- targeting */

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
		fix dist = Builder.sel_mode == BSEL_EDGE
			? point_to_segment_dist(point, side_vertex(seg, sidenum, k), side_vertex(seg, sidenum, (k + 1) % 4))
			: vm_vec_dist(side_vertex(seg, sidenum, k), point);

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
	Hover.segnum = hit.hit_side_seg;
	Hover.sidenum = hit.hit_side;
	Hover.corner = nearest_part(&Segments[Hover.segnum], Hover.sidenum, &hit.hit_pnt);
}

/* --------------------------------------------------------------- input */

static void select_hovered(void)
{
	char text[96];

	if (!Hover.valid)
		return;
	Selection = Hover;
	builder_describe_selection(text, sizeof(text));
	builder_message(text);
}

/* Left click: the current tool, on whatever the crosshair is over. */
static void apply_tool(void)
{
	if (!Hover.valid)
		return;
	switch (Builder.tool)
	{
		case BTOOL_SELECT:	select_hovered(); break;
		case BTOOL_BUILD:	extrude(Hover.segnum, Hover.sidenum); break;
		case BTOOL_DELETE:	delete_segment(Hover.segnum); break;
		case BTOOL_PAINT:	paint_element(&Hover, 0); break;
		case BTOOL_PICK:	pick_texture_from(&Hover); break;
	}
}

void builder_save(void)
{
	if (builder_save_level())
	{
		Builder_dirty = 0;
		builder_message("Level saved -- play it as mission \"SNG Builder\"");
	}
	else
		builder_message("Save FAILED");
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

static int handle_mouse(d_event *event)
{
	switch (event_mouse_get_button(event))
	{
		case MBTN_LEFT:		apply_tool(); return 1;
		case MBTN_RIGHT:	select_hovered(); return 1;
		default:		return 0;
	}
}

static int is_builder_button(d_event *event)
{
	int button = event_mouse_get_button(event);

	return button == MBTN_LEFT || button == MBTN_RIGHT;
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
		case KEY_COMMA:			builder_nudge_normal(-1); return 1;
		case KEY_PERIOD:		builder_nudge_normal(1); return 1;
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

/* -------------------------------------------------------------- drawing */

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
		corners[k] = *side_vertex(seg, sidenum, k);
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
	outward_step(seg, sidenum, units(Builder.build_depth), &step);
	draw_face_outline(seg, sidenum, &step);
	for (k = 0; k < 4; k++)
	{
		vms_vector far_corner;

		vm_vec_add(&far_corner, side_vertex(seg, sidenum, k), &step);
		draw_world_line(side_vertex(seg, sidenum, k), &far_corner);
	}
}

static void draw_element(const builder_element *el, int face_color, int part_color)
{
	segment *seg = &Segments[el->segnum];

	gr_setcolor(face_color);
	draw_face_outline(seg, el->sidenum, NULL);
	gr_setcolor(part_color);
	if (Builder.sel_mode == BSEL_VERTEX)
		draw_vertex_mark(side_vertex(seg, el->sidenum, el->corner));
	else if (Builder.sel_mode == BSEL_EDGE)
		draw_world_line(side_vertex(seg, el->sidenum, el->corner),
			side_vertex(seg, el->sidenum, (el->corner + 1) % 4));
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
	if (Hover.valid && Hover.segnum < Num_segments)
	{
		if (Builder.tool == BTOOL_BUILD)
		{
			gr_setcolor(BM_XRGB(10, 20, 31));
			draw_extrude_preview(&Segments[Hover.segnum], Hover.sidenum);
		}
		draw_element(&Hover, BM_XRGB(31, 31, 0), BM_XRGB(31, 20, 0));
	}
	if (Selection.valid && Selection.segnum < Num_segments)
	{
#ifdef OGL
		glLineWidth(3.0);
#endif
		draw_element(&Selection, BM_XRGB(0, 31, 8), BM_XRGB(31, 0, 31));
	}
#ifdef OGL
	glLineWidth(1.0);
	glEnable(GL_DEPTH_TEST);
#endif
}

void builder_draw_hud(void)
{
	int line_height, preview_size, x, y;
	char selection_text[96];

	if (!Builder_on)
		return;
	gr_set_curfont(GAME_FONT);
	line_height = LINE_SPACING;
	x = FSPACX(4);
	y = grd_curcanv->cv_bitmap.bm_h - line_height * 6;

	gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
	gr_printf(x, y, "BUILDER%s   %s   select %s   step %.2f   depth %.0f",
		Builder_dirty ? "*" : "", Builder_tool_names[Builder.tool],
		Builder_sel_mode_names[Builder.sel_mode], Builder.nudge_step, Builder.build_depth);
	builder_describe_selection(selection_text, sizeof(selection_text));
	gr_set_fontcolor(BM_XRGB(0, 31, 8), -1);
	gr_printf(x, y + line_height, "%s", selection_text);
	gr_set_fontcolor(BM_XRGB(20, 20, 20), -1);
	gr_printf(x, y + line_height * 2, "Tab tools   LMB use tool   RMB select   1-5 tool   V face/edge/vertex");

	preview_size = grd_curcanv->cv_bitmap.bm_h / 8;
	builder_draw_texture_thumb(Builder.texture,
		grd_curcanv->cv_bitmap.bm_w - preview_size - FSPACX(4),
		grd_curcanv->cv_bitmap.bm_h - preview_size - FSPACY(4), preview_size);
}

/* -------------------------------------------------------------- session */

static void reset_settings(void)
{
	memset(&Builder, 0, sizeof(Builder));
	Builder.tool = BTOOL_BUILD;
	Builder.sel_mode = BSEL_FACE;
	Builder.nudge_step = 1.0f;
	Builder.build_depth = 20.0f;
	Builder.axis_snap = 1;
	Selection.valid = 0;
	Hover.valid = 0;
}

void builder_end(void)
{
	if (!Builder_on)
		return;
	if (Builder_dirty)
		builder_save_level();
	stack_clear(&Undo);
	stack_clear(&Redo);
	select_cockpit(PlayerCfg.PreferredCockpitMode);
	Builder_on = 0;
	Builder_dirty = 0;
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

	stack_clear(&Undo);
	stack_clear(&Redo);
	reset_settings();
	Builder_on = 1;
	Builder_dirty = 0;
	StartNewGame(1);
	select_cockpit(CM_FULL_SCREEN);
	bake_lighting();
}
