/*
 * SNG: first-person level builder.
 *
 * Every edit works on the face under the crosshair. Geometry operations take
 * an undo snapshot first and roll themselves back if they would turn any
 * segment inside out or strand the player outside the mine, so there is no
 * way to build an invalid level by accident.
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
#include "text.h"
#include "playsave.h"
#include "builder.h"
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
#define TEXTURE_PAGE_STEP	10
#define CORNER_MARK_SIZE	(F1_0 * 3 / 2)

static const fix Grid_sizes[] = {5 * F1_0, 10 * F1_0, 20 * F1_0, 40 * F1_0, 80 * F1_0};
#define GRID_COUNT	((int)(sizeof(Grid_sizes) / sizeof(Grid_sizes[0])))
#define GRID_DEFAULT	2

typedef struct builder_target {
	int valid;
	int segnum;
	int sidenum;
	int corner;		/* 0..3, index into Side_to_verts[sidenum] */
	vms_vector point;
} builder_target;

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

static int Builder_on;
static int Builder_dirty;
static int Builder_show_help = 1;
static int Builder_fullbright;
static int Builder_grid = GRID_DEFAULT;
static int Builder_texture;
static builder_target Target;
static builder_snapshot *Undo[MAX_UNDO];
static int Undo_count;

int builder_active(void)
{
	return Builder_on;
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

static vms_vector *side_vertex(segment *seg, int sidenum, int k)
{
	return &Vertices[seg->verts[Side_to_verts[sidenum][k]]];
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

static int push_undo(void)
{
	builder_snapshot *snap = take_snapshot();

	if (!snap)
	{
		builder_message("Builder: out of memory for undo");
		return 0;
	}
	if (Undo_count == MAX_UNDO)
	{
		free_snapshot(Undo[0]);
		memmove(Undo, Undo + 1, sizeof(Undo[0]) * (MAX_UNDO - 1));
		Undo_count--;
	}
	Undo[Undo_count++] = snap;
	return 1;
}

static void clear_undo(void)
{
	while (Undo_count > 0)
		free_snapshot(Undo[--Undo_count]);
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
}

/* Pop and apply the newest snapshot. Used by undo and by failed edits. */
static int pop_undo(void)
{
	builder_snapshot *snap;

	if (Undo_count == 0)
		return 0;
	snap = Undo[--Undo_count];
	restore_snapshot(snap);
	free_snapshot(snap);
	Builder_dirty = 1;
	return 1;
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
	seg->sides[sidenum].tmap_num = Builder_texture;
	seg->sides[sidenum].tmap_num2 = 0;
	map_side_uvs(seg, sidenum);
}

static void rotate_side_uvs(side *sidep)
{
	int k;

	for (k = 0; k < 4; k++)
	{
		fix u = sidep->uvls[k].u;

		sidep->uvls[k].u = sidep->uvls[k].v;
		sidep->uvls[k].v = -u;
	}
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
			fix l = Builder_fullbright ? F1_0 : light_at(side_vertex(seg, sidenum, k), &normal, lights, n_lights);

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

/* ----------------------------------------------------------- validity */

static int segment_is_sane(segment *seg)
{
	vms_vector center;
	int sidenum, n;

	compute_segment_center(&center, seg);
	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		for (n = 0; n < 2; n++)
		{
			vms_vector *on_plane = side_vertex(seg, sidenum, n * 2);
			vms_vector to_center;

			vm_vec_sub(&to_center, &center, on_plane);
			if (vm_vec_dot(&to_center, &seg->sides[sidenum].normals[n]) < MIN_SIDE_DEPTH)
				return 0;
		}
	return 1;
}

static int segment_uses_vertex(segment *seg, int vertnum)
{
	int v;

	for (v = 0; v < MAX_VERTICES_PER_SEGMENT; v++)
		if (seg->verts[v] == vertnum)
			return 1;
	return 0;
}

static int segment_uses_any(segment *seg, int *vertnums, int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (segment_uses_vertex(seg, vertnums[i]))
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

/* Every geometry edit ends here: keep it, or roll it back with a reason. */
static int commit_edit(int ok, const char *failure)
{
	Target.valid = 0;
	if (!ok || !player_still_inside())
	{
		pop_undo();
		builder_message(ok ? "Can't do that with you in the way" : failure);
		return 0;
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
	snap_to_axis(step);
	vm_vec_scale(step, distance);
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

/* A new cube whose face lands exactly on an existing open face joins it,
 * which is how loops and multi-cube rooms close up. */
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

static int op_extrude(int segnum, int sidenum)
{
	segment *seg = &Segments[segnum];
	segment *fresh;
	vms_vector step;
	int newnum = Num_segments, k;

	if (seg->children[sidenum] != -1 || seg->sides[sidenum].wall_num != -1)
		return builder_refuse("That face is open");
	if (Num_segments >= MAX_SEGMENTS)
		return builder_refuse("Segment limit reached");
	if (!push_undo())
		return 0;

	outward_step(seg, sidenum, Grid_sizes[Builder_grid], &step);
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
			return commit_edit(0, "Vertex limit reached");
	}

	Num_segments++;
	Highest_segment_index = newnum;
	seg->children[sidenum] = newnum;
	fresh->children[(int)Side_opposite[sidenum]] = segnum;
	validate_segment(fresh);
	join_coincident_sides(newnum);
	map_segment_uvs(newnum);
	return commit_edit(segment_is_sane(fresh), "No room to build there");
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

static int op_delete(int segnum)
{
	const char *blocker = delete_blocker(segnum);
	int last = Num_segments - 1;

	if (blocker)
		return builder_refuse(blocker);
	if (!push_undo())
		return 0;

	seal_neighbours(segnum);
	delete_segment_objects(segnum);
	if (segnum != last)
		move_segment(last, segnum);
	Num_segments--;
	Highest_segment_index = Num_segments - 1;
	return commit_edit(1, NULL);
}

static int op_push_face(int segnum, int sidenum, int direction)
{
	segment *seg = &Segments[segnum];
	vms_vector step;
	int moved[4], k;

	if (!push_undo())
		return 0;
	outward_step(seg, sidenum, Grid_sizes[Builder_grid] * direction, &step);
	for (k = 0; k < 4; k++)
	{
		moved[k] = seg->verts[Side_to_verts[sidenum][k]];
		vm_vec_add2(&Vertices[moved[k]], &step);
	}
	return commit_edit(refresh_after_move(moved, 4), "That would turn a cube inside out");
}

static int op_push_corner(int segnum, int sidenum, int corner, int direction)
{
	segment *seg = &Segments[segnum];
	vms_vector step;
	int moved;

	if (!push_undo())
		return 0;
	outward_step(seg, sidenum, Grid_sizes[Builder_grid] * direction, &step);
	moved = seg->verts[Side_to_verts[sidenum][corner]];
	vm_vec_add2(&Vertices[moved], &step);
	return commit_edit(refresh_after_move(&moved, 1), "That would turn a cube inside out");
}

/* ----------------------------------------------------------- targeting */

static int nearest_corner(segment *seg, int sidenum, vms_vector *point)
{
	fix best_dist = 0;
	int best = 0, k;

	for (k = 0; k < 4; k++)
	{
		fix dist = vm_vec_dist(side_vertex(seg, sidenum, k), point);

		if (k == 0 || dist < best_dist)
		{
			best = k;
			best_dist = dist;
		}
	}
	return best;
}

static void update_target(void)
{
	vms_vector end;
	fvi_query query;
	fvi_info hit;

	Target.valid = 0;
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
	Target.valid = 1;
	Target.segnum = hit.hit_side_seg;
	Target.sidenum = hit.hit_side;
	Target.point = hit.hit_pnt;
	Target.corner = nearest_corner(&Segments[Target.segnum], Target.sidenum, &hit.hit_pnt);
}

/* --------------------------------------------------------------- input */

static void cycle_texture(int delta)
{
	Builder_texture = (Builder_texture + delta) % NumTextures;
	if (Builder_texture < 0)
		Builder_texture += NumTextures;
}

static void pick_texture(void)
{
	if (!Target.valid)
		return;
	Builder_texture = Segments[Target.segnum].sides[Target.sidenum].tmap_num;
}

static void paint_target(int whole_segment)
{
	segment *seg;
	int sidenum;

	if (!Target.valid || !push_undo())
		return;
	seg = &Segments[Target.segnum];
	if (!whole_segment)
		paint_side(seg, Target.sidenum);
	else
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			if (side_is_solid(seg, sidenum))
				paint_side(seg, sidenum);
	commit_edit(1, NULL);
}

static void rotate_target_texture(void)
{
	if (!Target.valid || !push_undo())
		return;
	rotate_side_uvs(&Segments[Target.segnum].sides[Target.sidenum]);
	commit_edit(1, NULL);
}

static void push_target(int direction, int corner_only)
{
	if (!Target.valid)
		return;
	if (corner_only)
		op_push_corner(Target.segnum, Target.sidenum, Target.corner, direction);
	else
		op_push_face(Target.segnum, Target.sidenum, direction);
}

static void save_now(void)
{
	if (builder_save_level())
	{
		Builder_dirty = 0;
		builder_message("Level saved -- play it as mission \"SNG Builder\"");
	}
	else
		builder_message("Save FAILED");
}

static void undo_now(void)
{
	if (pop_undo())
		builder_message("Undone");
	else
		builder_message("Nothing to undo");
}

static void toggle_fullbright(void)
{
	Builder_fullbright = !Builder_fullbright;
	bake_lighting();
	builder_message(Builder_fullbright ? "Fullbright on" : "Baked lighting on");
}

static int handle_mouse(d_event *event)
{
	int shift = keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT];

	switch (event_mouse_get_button(event))
	{
		case MBTN_LEFT:
			if (Target.valid)
				op_extrude(Target.segnum, Target.sidenum);
			return 1;
		case MBTN_RIGHT:
			if (Target.valid)
				op_delete(Target.segnum);
			return 1;
		case MBTN_MIDDLE:
			pick_texture();
			return 1;
		case MBTN_Z_UP:
			push_target(1, shift);
			return 1;
		case MBTN_Z_DOWN:
			push_target(-1, shift);
			return 1;
		default:
			return 0;
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
		case KEY_INSERT:		if (Target.valid) op_extrude(Target.segnum, Target.sidenum); return 1;
		case KEY_DELETE:		if (Target.valid) op_delete(Target.segnum); return 1;
		case KEY_PAGEUP:		push_target(1, 0); return 1;
		case KEY_PAGEDOWN:		push_target(-1, 0); return 1;
		case KEY_SHIFTED + KEY_PAGEUP:	push_target(1, 1); return 1;
		case KEY_SHIFTED + KEY_PAGEDOWN:	push_target(-1, 1); return 1;
		case KEY_T:			paint_target(0); return 1;
		case KEY_SHIFTED + KEY_T:	paint_target(1); return 1;
		case KEY_G:			pick_texture(); return 1;
		case KEY_R:			rotate_target_texture(); return 1;
		case KEY_LBRACKET:		cycle_texture(-1); return 1;
		case KEY_RBRACKET:		cycle_texture(1); return 1;
		case KEY_SHIFTED + KEY_LBRACKET:	cycle_texture(-TEXTURE_PAGE_STEP); return 1;
		case KEY_SHIFTED + KEY_RBRACKET:	cycle_texture(TEXTURE_PAGE_STEP); return 1;
		case KEY_MINUS:			if (Builder_grid > 0) Builder_grid--; return 1;
		case KEY_EQUAL:			if (Builder_grid < GRID_COUNT - 1) Builder_grid++; return 1;
		case KEY_L:			toggle_fullbright(); return 1;
		case KEY_H:			Builder_show_help = !Builder_show_help; return 1;
		case KEY_BACKSP:
		case KEY_CTRLED + KEY_Z:	undo_now(); return 1;
		case KEY_CTRLED + KEY_S:	save_now(); return 1;
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
}

void builder_frame(void)
{
	if (!Builder_on)
		return;
	Players[Player_num].shields = MAX_SHIELDS;
	Players[Player_num].energy = MAX_ENERGY;
	builder_filter_controls();
	update_target();
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

/* The cube a left click would create, drawn as a wireframe ghost. */
static void draw_extrude_preview(segment *seg, int sidenum)
{
	vms_vector step;
	int k;

	if (seg->children[sidenum] != -1)
		return;
	outward_step(seg, sidenum, Grid_sizes[Builder_grid], &step);
	draw_face_outline(seg, sidenum, &step);
	for (k = 0; k < 4; k++)
	{
		vms_vector far_corner;

		vm_vec_add(&far_corner, side_vertex(seg, sidenum, k), &step);
		draw_world_line(side_vertex(seg, sidenum, k), &far_corner);
	}
}

static void draw_corner_mark(vms_vector *corner)
{
	static const vms_vector axes[3] = {
		{CORNER_MARK_SIZE, 0, 0}, {0, CORNER_MARK_SIZE, 0}, {0, 0, CORNER_MARK_SIZE},
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

/* Called inside render_frame(), so the 3D view is still set up. */
void builder_render_overlay(void)
{
	segment *seg;

	if (!Builder_on || !Target.valid)
		return;
	seg = &Segments[Target.segnum];

#ifdef OGL
	glDisable(GL_DEPTH_TEST);
	glLineWidth(2.0);
#endif
	gr_setcolor(BM_XRGB(10, 20, 31));
	draw_extrude_preview(seg, Target.sidenum);
	gr_setcolor(BM_XRGB(31, 31, 0));
	draw_face_outline(seg, Target.sidenum, NULL);
	gr_setcolor(BM_XRGB(31, 8, 8));
	draw_corner_mark(side_vertex(seg, Target.sidenum, Target.corner));
#ifdef OGL
	glLineWidth(1.0);
	glEnable(GL_DEPTH_TEST);
#endif
}

static const char *const Help_lines[] = {
	"LMB / Ins     build cube on face",
	"RMB / Del     remove cube",
	"Wheel / PgUp/PgDn   push / pull face",
	"  + Shift     push / pull nearest corner",
	"MMB / G       pick texture from face",
	"[ ]           texture (Shift: x10)",
	"T / Shift+T   paint face / whole cube",
	"R             rotate texture",
	"- =           grid size",
	"L             fullbright / baked light",
	"Backspace     undo     Ctrl+S  save",
	"H             hide this help",
};

static void draw_texture_preview(int x, int y, int size)
{
#ifdef OGL
	bitmap_index index = Textures[Builder_texture];

	PIGGY_PAGE_IN(index);
	ogl_ubitmapm_cs(x, y, size, size, &GameBitmaps[index.index], -1, F1_0);
#else
	(void)x; (void)y; (void)size;
#endif
}

static void draw_status(int line_height)
{
	int y = FSPACY(4) + line_height * 2;

	gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
	gr_printf(FSPACX(4), y, "BUILDER%s   grid %d   cubes %d   tex %d %s",
		Builder_dirty ? "*" : "",
		f2i(Grid_sizes[Builder_grid]), Num_segments,
		Builder_texture, TmapInfo[Builder_texture].filename);
	if (!Target.valid)
	{
		gr_set_fontcolor(BM_XRGB(20, 20, 20), -1);
		gr_printf(FSPACX(4), y + line_height, "aim at a face");
	}
}

static void draw_help(int line_height)
{
	int i, y = FSPACY(4) + line_height * 4;

	if (!Builder_show_help)
		return;
	gr_set_fontcolor(BM_XRGB(24, 24, 24), -1);
	for (i = 0; i < (int)(sizeof(Help_lines) / sizeof(Help_lines[0])); i++, y += line_height)
		gr_printf(FSPACX(4), y, "%s", Help_lines[i]);
}

void builder_draw_hud(void)
{
	int line_height, preview_size;

	if (!Builder_on)
		return;
	gr_set_curfont(GAME_FONT);
	line_height = LINE_SPACING;
	draw_status(line_height);
	draw_help(line_height);

	preview_size = grd_curcanv->cv_bitmap.bm_h / 8;
	draw_texture_preview(grd_curcanv->cv_bitmap.bm_w - preview_size - FSPACX(4),
		grd_curcanv->cv_bitmap.bm_h - preview_size - FSPACY(4), preview_size);
}

/* -------------------------------------------------------------- session */

void builder_end(void)
{
	if (!Builder_on)
		return;
	if (Builder_dirty)
		builder_save_level();
	clear_undo();
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
		nm_messagebox(NULL, 1, TXT_OK, "Couldn't write the builder level.");
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
		nm_messagebox(NULL, 1, TXT_OK, "Couldn't load the builder mission.");
		return;
	}

	clear_undo();
	Builder_on = 1;
	Builder_dirty = 0;
	Builder_texture = 0;
	StartNewGame(1);
	select_cockpit(CM_FULL_SCREEN);
	bake_lighting();
}
