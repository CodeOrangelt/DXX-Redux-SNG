/*
 * SNG: level builder -- mine primitives, undo/redo, validity, lighting, UVs.
 *
 * Nothing here takes undo snapshots on its own except the bld_*_edit()
 * bracket: callers open one edit, apply any number of primitives, and close
 * it, so a multi-tag operation is a single undo step that rolls back as a
 * whole if any part of it would turn a segment inside out or strand the
 * player outside the mine.
 */

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
#include "textures.h"
#include "bm.h"
#include "hudmsg.h"
#include "builder_int.h"

#define WELD_DISTANCE		(F1_0 / 4)
#define MIN_SIDE_DEPTH		(F1_0 / 2)
#define TEXTURE_WORLD_SIZE	(20 * F1_0)
#define AXIS_SNAP_DOT		(F1_0 * 97 / 100)
#define MAX_UNDO		64
#define MAX_LIGHT_SOURCES	512
#define LIGHT_RANGE		(100 * F1_0)
#define LIGHT_AMBIENT		(F1_0 * 40 / 100)
#define LIGHT_SKY_SHADE		(F1_0 * 15 / 100)

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

static snapshot_stack Undo;
static snapshot_stack Redo;
static int Dirty;

fix bld_units(float world_units)
{
	return fl2f(world_units);
}

void bld_message(const char *text)
{
	HUD_init_message(HM_DEFAULT, "%s", text);
}

int bld_refuse(const char *reason)
{
	bld_message(reason);
	return 0;
}

int bld_is_dirty(void)
{
	return Dirty;
}

void bld_set_dirty(int dirty)
{
	Dirty = dirty;
}

/* ----------------------------------------------------------- topology */

int bld_side_vertnum(segment *seg, int sidenum, int k)
{
	return seg->verts[Side_to_verts[sidenum][k]];
}

vms_vector *bld_side_vertex(segment *seg, int sidenum, int k)
{
	return &Vertices[bld_side_vertnum(seg, sidenum, k)];
}

/* validate_segment() keeps normals[] current; they point into the segment. */
void bld_side_normal(segment *seg, int sidenum, vms_vector *inward)
{
	vm_vec_avg(inward, &seg->sides[sidenum].normals[0], &seg->sides[sidenum].normals[1]);
	vm_vec_normalize(inward);
}

/* Grid-aligned rooms stay grid-aligned: near-axis directions snap to the axis. */
static void snap_to_axis(vms_vector *dir)
{
	if (labs(dir->x) >= AXIS_SNAP_DOT)
		vm_vec_make(dir, dir->x > 0 ? F1_0 : -F1_0, 0, 0);
	else if (labs(dir->y) >= AXIS_SNAP_DOT)
		vm_vec_make(dir, 0, dir->y > 0 ? F1_0 : -F1_0, 0);
	else if (labs(dir->z) >= AXIS_SNAP_DOT)
		vm_vec_make(dir, 0, 0, dir->z > 0 ? F1_0 : -F1_0);
}

void bld_side_outward(segment *seg, int sidenum, vms_vector *outward)
{
	bld_side_normal(seg, sidenum, outward);
	vm_vec_scale(outward, -F1_0);
	if (Builder.axis_snap)
		snap_to_axis(outward);
}

int bld_side_is_solid(segment *seg, int sidenum)
{
	return seg->children[sidenum] == -1 || seg->sides[sidenum].wall_num != -1;
}

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

/* 4 vertex numbers for a face, 2 for an edge, 1 for a vertex. */
int bld_element_vertices(const builder_element *el, int *out)
{
	segment *seg = &Segments[el->segnum];
	int k;

	switch (el->mode)
	{
		case BSEL_VERTEX:
			out[0] = bld_side_vertnum(seg, el->sidenum, el->corner);
			return 1;
		case BSEL_EDGE:
			out[0] = bld_side_vertnum(seg, el->sidenum, el->corner);
			out[1] = bld_side_vertnum(seg, el->sidenum, (el->corner + 1) % 4);
			return 2;
		default:
			for (k = 0; k < 4; k++)
				out[k] = bld_side_vertnum(seg, el->sidenum, k);
			return 4;
	}
}

void bld_element_center(const builder_element *el, vms_vector *center)
{
	int verts[4], count, i;

	count = bld_element_vertices(el, verts);
	vm_vec_zero(center);
	for (i = 0; i < count; i++)
		vm_vec_add2(center, &Vertices[verts[i]]);
	vm_vec_scale2(center, F1_0, i2f(count));
}

/* ------------------------------------------------------------- history */

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

void bld_history_clear(void)
{
	stack_clear(&Undo);
	stack_clear(&Redo);
}

int bld_undo_depth(void)
{
	return Undo.count;
}

int bld_redo_depth(void)
{
	return Redo.count;
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
	builder_forget_hover();
	builder_forget_stale_tags();
	Dirty = 1;
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

int bld_undo(void)
{
	return step_history(&Undo, &Redo);
}

int bld_redo(void)
{
	return step_history(&Redo, &Undo);
}

/* Opens an edit. A new edit forks history, so redo is dropped. */
int bld_begin_edit(void)
{
	builder_snapshot *snap = take_snapshot();

	if (!snap)
		return bld_refuse("Builder: out of memory for undo");
	stack_push(&Undo, snap);
	stack_clear(&Redo);
	return 1;
}

/* Abandons an open edit without a trace: not in undo, not in redo. */
void bld_cancel_edit(void)
{
	builder_snapshot *snap = stack_pop(&Undo);

	if (!snap)
		return;
	restore_snapshot(snap);
	free_snapshot(snap);
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

/* Closes an edit: keep it, or roll the whole thing back with a reason. */
int bld_finish_edit(int ok, const char *failure)
{
	builder_forget_hover();
	if (!ok || !player_still_inside())
	{
		bld_cancel_edit();
		return bld_refuse(ok ? "Can't do that with you in the way" : failure);
	}
	bld_bake_lighting();
	Dirty = 1;
	return 1;
}

/* ---------------------------------------------------------- texturing */

/* World-aligned planar mapping: neighbouring faces line up seamlessly. */
void bld_map_side_uvs(segment *seg, int sidenum)
{
	vms_vector normal, forward, up, right;
	int k;

	bld_side_normal(seg, sidenum, &normal);
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
		vms_vector *p = bld_side_vertex(seg, sidenum, k);

		seg->sides[sidenum].uvls[k].u = fixdiv(vm_vec_dot(p, &right), TEXTURE_WORLD_SIZE);
		seg->sides[sidenum].uvls[k].v = -fixdiv(vm_vec_dot(p, &up), TEXTURE_WORLD_SIZE);
	}
}

static void map_segment_uvs(int segnum)
{
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		bld_map_side_uvs(&Segments[segnum], sidenum);
}

void bld_paint_side(segment *seg, int sidenum)
{
	seg->sides[sidenum].tmap_num = Builder.texture;
	seg->sides[sidenum].tmap_num2 = 0;
	bld_map_side_uvs(seg, sidenum);
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

			if (!bld_side_is_solid(seg, sidenum) || count == MAX_LIGHT_SOURCES)
				continue;
			intensity = texture_light(&seg->sides[sidenum]);
			if (intensity <= 0)
				continue;
			compute_center_point_on_side(&lights[count].pos, seg, sidenum);
			bld_side_normal(seg, sidenum, &normal);
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

		bld_side_normal(seg, sidenum, &normal);
		for (k = 0; k < 4; k++)
		{
			fix l = Builder.fullbright ? F1_0 : light_at(bld_side_vertex(seg, sidenum, k), &normal, lights, n_lights);

			seg->sides[sidenum].uvls[k].l = l;
			total += l;
		}
	}
	seg->static_light = total / (MAX_SIDES_PER_SEGMENT * 4);
}

/* Cheap stand-in for the editor's radiosity: ambient, a sky tint by facing,
 * and distance falloff from every light-emitting texture. No occlusion. */
void bld_bake_lighting(void)
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

			vm_vec_sub(&to_center, &center, bld_side_vertex(seg, sidenum, n * 2));
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
int bld_refresh_after_move(int *vertnums, int count)
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

/* After vertices were merged: everything valid, and newly touching faces joined. */
int bld_revalidate_all(void)
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

int bld_nearest_vertex(int vertnum, int *exclude, int n_exclude, fix radius)
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

void bld_replace_vertex(int from, int to)
{
	int segnum, v;

	for (segnum = 0; segnum < Num_segments; segnum++)
		for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
			if (Segments[segnum].verts[v] == from)
				Segments[segnum].verts[v] = to;
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

/* Grow a new cube off a solid face. Returns why it couldn't, or NULL. */
const char *bld_extrude(int segnum, int sidenum)
{
	segment *seg = &Segments[segnum];
	segment *fresh;
	vms_vector step;
	int newnum = Num_segments, k;

	if (seg->children[sidenum] != -1 || seg->sides[sidenum].wall_num != -1)
		return "That face is open";
	if (Num_segments >= MAX_SEGMENTS)
		return "Segment limit reached";

	bld_side_outward(seg, sidenum, &step);
	vm_vec_scale(&step, bld_units(Builder.build_depth));
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
			return "Vertex limit reached";
	}

	Num_segments++;
	Highest_segment_index = newnum;
	seg->children[sidenum] = newnum;
	fresh->children[(int)Side_opposite[sidenum]] = segnum;
	validate_segment(fresh);
	join_coincident_sides(newnum);
	map_segment_uvs(newnum);
	if (!segment_is_sane(fresh) || segment_has_duplicate_vertex(fresh))
		return "No room to build there";
	return NULL;
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

const char *bld_delete_blocker(int segnum)
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
		bld_map_side_uvs(&Segments[child], facing);
	}
}

/* Callers check bld_delete_blocker() first. Renumbers the last segment. */
void bld_delete(int segnum)
{
	int last = Num_segments - 1;

	seal_neighbours(segnum);
	while (Segments[segnum].objects != -1)
		obj_delete(Segments[segnum].objects);
	if (segnum != last)
		move_segment(last, segnum);
	Num_segments--;
	Highest_segment_index = Num_segments - 1;
}
