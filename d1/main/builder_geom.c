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

static int partner_vertex(int sidenum, int v);

int bld_side_vertnum(segment *seg, int sidenum, int k)
{
	return seg->verts[Side_to_verts[sidenum][k]];
}

/* The cube corner straight across from face corner k. */
int bld_side_partner(segment *seg, int sidenum, int k)
{
	return seg->verts[partner_vertex(sidenum, Side_to_verts[sidenum][k])];
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

/* 8 vertex numbers for a cube, 4 for a face, 2 for an edge, 1 for a vertex. */
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
		case BSEL_CUBE:
			for (k = 0; k < 4; k++)
			{
				out[k] = bld_side_vertnum(seg, el->sidenum, k);
				out[k + 4] = bld_side_partner(seg, el->sidenum, k);
			}
			return BUILDER_ELEMENT_VERTS;
		default:
			for (k = 0; k < 4; k++)
				out[k] = bld_side_vertnum(seg, el->sidenum, k);
			return 4;
	}
}

void bld_element_center(const builder_element *el, vms_vector *center)
{
	int verts[BUILDER_ELEMENT_VERTS], count, i;

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

/* Whole tiles are invisible, but the level file keeps UVs in a short
 * (+-16 tiles) and clamps the rest, which smears far-from-origin faces. */
static void rebase_uvs(side *sidep)
{
	fix whole_u = sidep->uvls[0].u & ~(F1_0 - 1);
	fix whole_v = sidep->uvls[0].v & ~(F1_0 - 1);
	int k;

	for (k = 0; k < 4; k++)
	{
		sidep->uvls[k].u -= whole_u;
		sidep->uvls[k].v -= whole_v;
	}
}

void bld_rebase_side_uvs(side *sidep)
{
	rebase_uvs(sidep);
}

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
	rebase_uvs(&seg->sides[sidenum]);
}

static void map_segment_uvs(int segnum)
{
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		bld_map_side_uvs(&Segments[segnum], sidenum);
}

void bld_paint_side_with(segment *seg, int sidenum, int texnum)
{
	seg->sides[sidenum].tmap_num = texnum;
	seg->sides[sidenum].tmap_num2 = 0;
	bld_map_side_uvs(seg, sidenum);
}

void bld_paint_overlay(segment *seg, int sidenum, int texnum, int orientation)
{
	seg->sides[sidenum].tmap_num2 = texnum | (orientation << 14);
}

void bld_paint_side(segment *seg, int sidenum)
{
	bld_paint_side_with(seg, sidenum, Builder.texture);
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

/* ---------------------------------------------------------- splitting */

/* Slicing a cube in two moves the cut through every cube it touches
 * sideways, so each shared face stays one-to-one with its neighbour. */
#define MAX_SPLIT_CUBES		128
#define MAX_CUT_EDGES		(MAX_SPLIT_CUBES * 4)
#define MAX_TOUCHED		(MAX_SPLIT_CUBES * 8)
#define MAX_SPLIT_LIMIT		256

typedef struct cut_edge {
	int near_vert, far_vert, mid;
	vms_vector pos;
} cut_edge;

typedef struct split_job {
	int segnum, a;
} split_job;

typedef struct split_state {
	split_job jobs[MAX_SPLIT_CUBES];
	int made[MAX_SPLIT_CUBES];
	float fraction[MAX_SPLIT_CUBES];
	int job_count;
	cut_edge edges[MAX_CUT_EDGES];
	int edge_count;
	float t;
} split_state;

static split_state Split;
static int Dry;	/* planning only: no vertices, no segments changed */
static int Touched[MAX_TOUCHED];
static int Touched_count;
static int Sealed;	/* openings closed because the cut stopped at the selection */
static int Limit_active;
static int Limit_segs[MAX_SPLIT_LIMIT];
static int Limit_count;

/* Cuts may only reach these cubes (and the halves they produce). */
void bld_split_limit(const int *segs, int count)
{
	Limit_count = count < MAX_SPLIT_LIMIT ? count : MAX_SPLIT_LIMIT;
	memcpy(Limit_segs, segs, sizeof(Limit_segs[0]) * Limit_count);
	Limit_active = 1;
}

void bld_split_unlimit(void)
{
	Limit_active = 0;
}

static int limit_allows(int segnum)
{
	int i;

	if (!Limit_active)
		return 1;
	for (i = 0; i < Limit_count; i++)
		if (Limit_segs[i] == segnum)
			return 1;
	return 0;
}

static void limit_add(int segnum)
{
	if (Limit_active && Limit_count < MAX_SPLIT_LIMIT)
		Limit_segs[Limit_count++] = segnum;
}

static cut_edge *find_cut_edge(int v0, int v1)
{
	int i;

	for (i = 0; i < Split.edge_count; i++)
	{
		cut_edge *e = &Split.edges[i];

		if ((e->near_vert == v0 && e->far_vert == v1) || (e->near_vert == v1 && e->far_vert == v0))
			return e;
	}
	return NULL;
}

/* Fraction of the way from near to far at which an existing cut falls. */
static float cut_fraction(const cut_edge *e, int near_vert, int far_vert)
{
	fix whole = vm_vec_dist(&Vertices[near_vert], &Vertices[far_vert]);

	if (whole <= 0)
		return 0.5f;
	return f2fl(vm_vec_dist(&Vertices[near_vert], &e->pos)) / f2fl(whole);
}

static int cut_vertex(int near_vert, int far_vert, float t)
{
	cut_edge *e = find_cut_edge(near_vert, far_vert);
	vms_vector along;

	if (e)
		return e->mid;
	if (Split.edge_count >= MAX_CUT_EDGES || (!Dry && Num_vertices >= MAX_VERTICES))
		return -1;
	vm_vec_sub(&along, &Vertices[far_vert], &Vertices[near_vert]);
	vm_vec_scale(&along, fl2f(t));
	Split.edges[Split.edge_count].near_vert = near_vert;
	Split.edges[Split.edge_count].far_vert = far_vert;
	vm_vec_add(&Split.edges[Split.edge_count].pos, &Vertices[near_vert], &along);
	Split.edges[Split.edge_count].mid = 0;
	if (!Dry)
	{
		Vertices[Num_vertices] = Split.edges[Split.edge_count].pos;
		Highest_vertex_index = Num_vertices;
		Split.edges[Split.edge_count].mid = Num_vertices++;
	}
	return Split.edges[Split.edge_count++].mid;
}

static const char *split_blocker(segment *seg, int a)
{
	int sidenum;

	if (seg->special != SEGMENT_IS_NOTHING)
		return "That cube is a special room";
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (sidenum != a && seg->sides[sidenum].wall_num != -1)
			return "That cube has a door or wall";
	return NULL;
}

static int job_queued(int segnum)
{
	int i;

	for (i = 0; i < Split.job_count; i++)
		if (Split.jobs[i].segnum == segnum)
			return 1;
	return 0;
}

static int side_holds_both(segment *seg, int sidenum, int v0, int v1)
{
	int k, found0 = 0, found1 = 0;

	for (k = 0; k < 4; k++)
	{
		int v = bld_side_vertnum(seg, sidenum, k);

		found0 |= v == v0;
		found1 |= v == v1;
	}
	return found0 && found1;
}

/* The neighbour is cut parallel to the edge our own side-a runs along it. */
static const char *queue_neighbour(segment *seg, int a, int lateral, int child)
{
	int shared[2], n_shared = 0, k, q;

	if (job_queued(child))
		return NULL;
	if (!limit_allows(child))
		return "That cut would also slice a cube you haven't tagged";
	if (Split.job_count >= MAX_SPLIT_CUBES)
		return "Too many connected cubes to cut through";
	for (k = 0; k < 4 && n_shared < 2; k++)
	{
		int cidx = Side_to_verts[a][k];

		if (side_has_vertex(lateral, cidx))
			shared[n_shared++] = seg->verts[cidx];
	}
	if (n_shared < 2)
		return "Can't cut through that neighbour";
	for (q = 0; q < MAX_SIDES_PER_SEGMENT; q++)
	{
		if (Segments[child].children[q] == (seg - Segments))
			continue;
		if (side_holds_both(&Segments[child], q, shared[0], shared[1]))
		{
			Split.jobs[Split.job_count].segnum = child;
			Split.jobs[Split.job_count].a = q;
			Split.job_count++;
			return NULL;
		}
	}
	return "Can't cut through that neighbour";
}

/* The cut stops at the selection: the opening to an unselected cube is closed. */
static void seal_opening(segment *seg, int sidenum, int child)
{
	int facing = find_connect_side(seg, &Segments[child]);

	Segments[child].children[facing] = -1;
	Segments[child].sides[facing].tmap_num = seg->sides[sidenum].tmap_num;
	Segments[child].sides[facing].tmap_num2 = 0;
	bld_map_side_uvs(&Segments[child], facing);
	Sealed++;
}

static float job_fraction(segment *seg, int a, int *cidx_a, int *cidx_b)
{
	int k;

	for (k = 0; k < 4; k++)
	{
		cut_edge *e = find_cut_edge(seg->verts[cidx_a[k]], seg->verts[cidx_b[k]]);

		if (e)
			return cut_fraction(e, seg->verts[cidx_a[k]], seg->verts[cidx_b[k]]);
	}
	(void)a;
	return Split.t;
}

static void relink_child(int child, int from, int to)
{
	int x;

	if (child < 0)
		return;
	for (x = 0; x < MAX_SIDES_PER_SEGMENT; x++)
		if (Segments[child].children[x] == from)
			Segments[child].children[x] = to;
}

/* Cuts one cube between side a and its opposite; the far half is new. */
static const char *split_one(int job)
{
	segment *seg = &Segments[Split.jobs[job].segnum];
	int segnum = Split.jobs[job].segnum, a = Split.jobs[job].a;
	int b = Side_opposite[a], newnum = Num_segments;
	int cidx_a[4], cidx_b[4], mid[4], k, sidenum;
	const char *failure = split_blocker(seg, a);
	segment *fresh;
	float t;

	if (failure)
		return failure;
	if (Num_segments >= MAX_SEGMENTS)
		return "Segment limit reached";
	for (k = 0; k < 4; k++)
	{
		cidx_a[k] = Side_to_verts[a][k];
		cidx_b[k] = partner_vertex(a, cidx_a[k]);
		if (cidx_b[k] < 0)
			return "That cube is too odd to cut";
	}
	t = job_fraction(seg, a, cidx_a, cidx_b);
	for (k = 0; k < 4; k++)
	{
		mid[k] = cut_vertex(seg->verts[cidx_a[k]], seg->verts[cidx_b[k]], t);
		if (mid[k] < 0)
			return "Vertex limit reached";
	}
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		int child = seg->children[sidenum];

		if (sidenum == a || sidenum == b)
			continue;
		if (child >= 0 && Limit_active && !limit_allows(child))
		{
			if (!Dry)
				seal_opening(seg, sidenum, child);
		}
		else if (child >= 0 && (failure = queue_neighbour(seg, a, sidenum, child)))
			return failure;
		if (!Dry)
			seg->children[sidenum] = -1;
	}
	Split.fraction[job] = t;
	if (Dry)
		return NULL;

	fresh = &Segments[newnum];
	*fresh = *seg;
	fresh->objects = -1;
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
	{
		fresh->sides[sidenum].wall_num = -1;
		if (sidenum != b)
			fresh->children[sidenum] = -1;
	}
	for (k = 0; k < 4; k++)
	{
		seg->verts[cidx_b[k]] = mid[k];
		fresh->verts[cidx_a[k]] = mid[k];
	}
	relink_child(fresh->children[b], segnum, newnum);
	seg->children[b] = newnum;
	fresh->children[a] = segnum;
	Num_segments++;
	Highest_segment_index = newnum;
	Split.made[job] = newnum;
	return NULL;
}

static const char *finish_split(void)
{
	int i;

	for (i = 0; i < Split.job_count; i++)
	{
		int pair[2] = {Split.jobs[i].segnum, Split.made[i]}, p;

		for (p = 0; p < 2; p++)
		{
			segment *seg = &Segments[pair[p]];

			if (segment_has_duplicate_vertex(seg))
				return "That cut would collapse a cube";
			validate_segment(seg);
			if (!segment_is_sane(seg))
				return "That cut would leave a cube too thin";
			map_segment_uvs(pair[p]);
		}
	}
	for (i = 0; i < Split.job_count; i++)
	{
		join_coincident_sides(Split.jobs[i].segnum);
		join_coincident_sides(Split.made[i]);
	}
	relink_all_objects();
	return NULL;
}

/* Slice a cube between side a and its opposite, t of the way from a.
 * Cubes sharing the cut's side faces are sliced too. Returns why not, or
 * NULL and the new (far-half) cube number. */
static void start_split(int segnum, int a, float t)
{
	Split.job_count = 0;
	Split.edge_count = 0;
	Split.t = t;
	Split.jobs[Split.job_count].segnum = segnum;
	Split.jobs[Split.job_count].a = a;
	Split.job_count++;
}

/* Which cubes a cut would slice, and where, without touching the level. */
int bld_split_plan(int segnum, int a, float t, split_cut *out, int max)
{
	const char *failure = NULL;
	int i, n = 0;

	start_split(segnum, a, t);
	Dry = 1;
	for (i = 0; i < Split.job_count && !failure; i++)
		failure = split_one(i);
	Dry = 0;
	for (i = 0; i < Split.job_count && n < max; i++)
	{
		out[n].segnum = Split.jobs[i].segnum;
		out[n].a = Split.jobs[i].a;
		out[n].t = Split.fraction[i];
		n++;
	}
	return failure ? 0 : n;
}

void bld_split_touched_reset(void)
{
	Touched_count = 0;
	Sealed = 0;
}

int bld_split_sealed(void)
{
	return Sealed;
}

int bld_split_was_touched(int segnum)
{
	int i;

	for (i = 0; i < Touched_count; i++)
		if (Touched[i] == segnum)
			return 1;
	return 0;
}

static void note_touched(int segnum)
{
	if (Touched_count < MAX_TOUCHED)
		Touched[Touched_count++] = segnum;
}

const char *bld_split(int segnum, int a, float t, int *new_num)
{
	const char *failure = NULL;
	int i;

	start_split(segnum, a, t);
	for (i = 0; i < Split.job_count && !failure; i++)
		failure = split_one(i);
	if (!failure)
		failure = finish_split();
	if (failure)
		return failure;
	for (i = 0; i < Split.job_count; i++)
	{
		limit_add(Split.made[i]);
		note_touched(Split.jobs[i].segnum);
		note_touched(Split.made[i]);
	}
	*new_num = Split.made[0];
	return NULL;
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
