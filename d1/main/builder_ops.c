/*
 * SNG: level builder -- tags and the user-facing edits.
 *
 * Tagged elements are edited together, as one undo step. Pushing in or out
 * moves each tag along its own face; an axis nudge either moves every tag
 * the same way (Sync) or flips the movement for tags on the far side of a
 * mirror plane through the middle of the tags (Mirror), so symmetric pairs
 * stay symmetric.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "inferno.h"
#include "segment.h"
#include "gameseg.h"
#include "textures.h"
#include "hudmsg.h"
#include "builder.h"
#include "builder_int.h"

#define UV_CORNERS		4
#define MAX_MOVES		(BUILDER_MAX_TAGS * 4)
#define MIRROR_PLANE_SLACK	(F1_0 / 8)

typedef struct vertex_move {
	int vertnum;
	vms_vector delta;
} vertex_move;

typedef void (*delta_fn)(const builder_element *el, int vertnum, vms_vector *delta, void *arg);

builder_settings Builder;

const char *const Builder_tool_names[BTOOL_COUNT] = {"Select", "Build", "Delete", "Paint", "Pick"};
const char *const Builder_sel_mode_names[BSEL_COUNT] = {"Face", "Edge", "Vertex", "Cube", "Wall"};
const char *const Builder_link_names[BLINK_COUNT] = {"Sync", "Mirror"};
const char *const Builder_axis_names[3] = {"X", "Y", "Z"};
const char *const Builder_layer_names[BLAYER_COUNT] = {"Base", "Overlay"};
const char *const Builder_paint_names[BPAINT_COUNT] = {"Brush", "Fill"};
const char *const Builder_fill_names[BFILL_COUNT] = {"Wall", "Cube", "Room", "Same"};
static const char *const Side_names[MAX_SIDES_PER_SEGMENT] = {"left", "top", "right", "bottom", "back", "front"};

static builder_element Tags[BUILDER_MAX_TAGS];
static int Tag_count;

/* ---------------------------------------------------------------- tags */

int builder_tag_count(void)
{
	return Tag_count;
}

const builder_element *builder_tag(int index)
{
	return &Tags[index];
}

static int same_element(const builder_element *a, const builder_element *b)
{
	if (a->segnum != b->segnum || a->mode != b->mode)
		return 0;
	if (a->mode == BSEL_CUBE)
		return 1;
	if (a->sidenum != b->sidenum)
		return 0;
	return a->mode == BSEL_FACE || a->mode == BSEL_WALL || a->corner == b->corner;
}

static int find_tag(const builder_element *el)
{
	int i;

	for (i = 0; i < Tag_count; i++)
		if (same_element(&Tags[i], el))
			return i;
	return -1;
}

static void remove_tag(int index)
{
	memmove(&Tags[index], &Tags[index + 1], sizeof(Tags[0]) * (Tag_count - index - 1));
	Tag_count--;
}

void builder_clear_tags(void)
{
	Tag_count = 0;
}

static int find_in(const builder_element *set, int count, const builder_element *el)
{
	int i;

	for (i = 0; i < count; i++)
		if (set[i].segnum == el->segnum && set[i].sidenum == el->sidenum)
			return i;
	return -1;
}

/* ---- wall tagging: a hovered Wall is every face joined to it in one plane */

#define MAX_WALL_CANDIDATES	512
#define WALL_FACING_DOT		(F1_0 * 99 / 100)
#define WALL_PLANE_SLACK	F1_0
#define WALL_SHARED_VERTS	2

static int shared_vertex_count(const builder_element *a, const builder_element *b)
{
	int i, j, shared = 0;

	for (i = 0; i < 4; i++)
		for (j = 0; j < 4; j++)
			if (bld_side_vertnum(&Segments[a->segnum], a->sidenum, i) ==
				bld_side_vertnum(&Segments[b->segnum], b->sidenum, j))
				shared++;
	return shared;
}

static int in_wall_plane(const builder_element *ref, int segnum, int sidenum)
{
	segment *seg = &Segments[segnum];
	vms_vector ref_normal, normal, origin;
	int k;

	bld_side_normal(&Segments[ref->segnum], ref->sidenum, &ref_normal);
	bld_side_normal(seg, sidenum, &normal);
	if (vm_vec_dot(&normal, &ref_normal) < WALL_FACING_DOT)
		return 0;
	origin = *bld_side_vertex(&Segments[ref->segnum], ref->sidenum, 0);
	for (k = 0; k < 4; k++)
	{
		vms_vector offset;

		vm_vec_sub(&offset, bld_side_vertex(seg, sidenum, k), &origin);
		if (labs(vm_vec_dot(&offset, &ref_normal)) > WALL_PLANE_SLACK)
			return 0;
	}
	return 1;
}

static int touches_any(const builder_element *face, const builder_element *set, int count)
{
	int i;

	for (i = 0; i < count; i++)
		if (shared_vertex_count(face, &set[i]) >= WALL_SHARED_VERTS)
			return 1;
	return 0;
}

static int gather_wall_candidates(const builder_element *ref, builder_element *out)
{
	int segnum, sidenum, count = 0;

	for (segnum = 0; segnum < Num_segments && count < MAX_WALL_CANDIDATES; segnum++)
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT && count < MAX_WALL_CANDIDATES; sidenum++)
		{
			if (!bld_side_is_solid(&Segments[segnum], sidenum) || !in_wall_plane(ref, segnum, sidenum))
				continue;
			out[count] = *ref;
			out[count].mode = BSEL_FACE;
			out[count].segnum = segnum;
			out[count].sidenum = sidenum;
			count++;
		}
	return count;
}

/* Grow outward from the hovered face through faces that share an edge. */
static int expand_wall(const builder_element *ref, builder_element *out, int max)
{
	builder_element candidates[MAX_WALL_CANDIDATES];
	int taken[MAX_WALL_CANDIDATES] = {0};
	int n_candidates = gather_wall_candidates(ref, candidates), count = 1, grew = 1, i;

	out[0] = *ref;
	out[0].mode = BSEL_FACE;
	while (grew)
	{
		grew = 0;
		for (i = 0; i < n_candidates && count < max; i++)
		{
			if (taken[i] || !touches_any(&candidates[i], out, count))
				continue;
			taken[i] = 1;
			if (find_in(out, count, &candidates[i]) >= 0)
				continue;
			out[count++] = candidates[i];
			grew = 1;
		}
	}
	return count;
}

static void remove_wall(const builder_element *el)
{
	builder_element wall[BUILDER_MAX_TAGS];
	int count = expand_wall(el, wall, BUILDER_MAX_TAGS), i, index;

	for (i = 0; i < count; i++)
	{
		index = find_tag(&wall[i]);
		if (index >= 0)
			remove_tag(index);
	}
}

static void add_one(const builder_element *el)
{
	if (find_tag(el) >= 0)
		return;
	if (Tag_count < BUILDER_MAX_TAGS)
		Tags[Tag_count++] = *el;
	else
		bld_message("Tag limit reached");
}

static void add_element(const builder_element *el)
{
	builder_element wall[BUILDER_MAX_TAGS];
	int count, i;

	if (el->mode != BSEL_WALL)
	{
		add_one(el);
		return;
	}
	count = expand_wall(el, wall, BUILDER_MAX_TAGS);
	for (i = 0; i < count; i++)
		add_one(&wall[i]);
}

void builder_select(const builder_element *el)
{
	Tag_count = 0;
	if (el->valid)
		add_element(el);
}

void builder_add_tag(const builder_element *el)
{
	if (el->valid)
		add_element(el);
}

void builder_toggle_tag(const builder_element *el)
{
	builder_element probe = *el;
	int index;

	if (!el->valid)
		return;
	if (probe.mode == BSEL_WALL)
		probe.mode = BSEL_FACE;
	index = find_tag(&probe);
	if (index < 0)
		add_element(el);
	else if (el->mode != BSEL_WALL)
		remove_tag(index);
	else
		remove_wall(el);
}

void builder_forget_stale_tags(void)
{
	int i = 0;

	while (i < Tag_count)
	{
		if (Tags[i].segnum >= Num_segments)
			remove_tag(i);
		else
			i++;
	}
}

static int need_tags(void)
{
	builder_forget_stale_tags();
	if (Tag_count)
		return 1;
	return bld_refuse("Tag something first (middle click)");
}

static int mirroring(void)
{
	return Builder.link_mode == BLINK_MIRROR && Tag_count >= 2;
}

/* The mirror plane passes through the middle of all tags. */
int builder_mirror_plane(vms_vector *center)
{
	vms_vector element_center;
	int i;

	if (!mirroring())
		return 0;
	vm_vec_zero(center);
	for (i = 0; i < Tag_count; i++)
	{
		bld_element_center(&Tags[i], &element_center);
		vm_vec_add2(center, &element_center);
	}
	vm_vec_scale2(center, F1_0, i2f(Tag_count));
	return 1;
}

static void describe_one(const builder_element *el, char *out, int size)
{
	if (el->mode == BSEL_CUBE)
		snprintf(out, size, "cube %d, whole", el->segnum);
	else if (el->mode == BSEL_FACE)
		snprintf(out, size, "cube %d, %s face", el->segnum, Side_names[el->sidenum]);
	else
		snprintf(out, size, "cube %d, %s face, %s %d", el->segnum, Side_names[el->sidenum],
			el->mode == BSEL_EDGE ? "edge" : "corner", el->corner + 1);
}

void builder_describe_selection(char *out, int size)
{
	if (Tag_count == 0)
		snprintf(out, size, "nothing tagged");
	else if (Tag_count == 1)
		describe_one(&Tags[0], out, size);
	else if (mirroring())
		snprintf(out, size, "%d tagged, mirrored on %s", Tag_count, Builder_axis_names[Builder.mirror_axis]);
	else
		snprintf(out, size, "%d tagged, in sync", Tag_count);
}

/* ------------------------------------------------------------- history */

void builder_undo(void)
{
	bld_message(bld_undo() ? "Undone" : "Nothing to undo");
}

void builder_redo(void)
{
	bld_message(bld_redo() ? "Redone" : "Nothing to redo");
}

void builder_save(void)
{
	if (!builder_save_level())
	{
		bld_message("Save FAILED");
		return;
	}
	bld_set_dirty(0);
	bld_message("Level saved -- play it as mission \"SNG Builder\"");
}

/* -------------------------------------------------------- vertex moves */

/* First delta wins when tags share a vertex, so a corner shared by two
 * tagged faces isn't moved twice. */
static int add_move(vertex_move *moves, int count, int vertnum, vms_vector *delta)
{
	int i;

	for (i = 0; i < count; i++)
		if (moves[i].vertnum == vertnum)
			return count;
	if (count == MAX_MOVES)
		return count;
	moves[count].vertnum = vertnum;
	moves[count].delta = *delta;
	return count + 1;
}

static int apply_moves(vertex_move *moves, int count)
{
	int vertnums[MAX_MOVES], i;

	for (i = 0; i < count; i++)
	{
		vm_vec_add2(&Vertices[moves[i].vertnum], &moves[i].delta);
		vertnums[i] = moves[i].vertnum;
	}
	return bld_refresh_after_move(vertnums, count);
}

static void move_elements(const builder_element *els, int n_els, delta_fn fn, void *arg, const char *failure)
{
	vertex_move moves[MAX_MOVES];
	int n_moves = 0, e, i;

	if (!bld_begin_edit())
		return;
	for (e = 0; e < n_els; e++)
	{
		int verts[BUILDER_ELEMENT_VERTS], count = bld_element_vertices(&els[e], verts);

		for (i = 0; i < count; i++)
		{
			vms_vector delta;

			fn(&els[e], verts[i], &delta, arg);
			n_moves = add_move(moves, n_moves, verts[i], &delta);
		}
	}
	bld_finish_edit(apply_moves(moves, n_moves), failure);
}

static void move_tags(delta_fn fn, void *arg, const char *failure)
{
	if (need_tags())
		move_elements(Tags, Tag_count, fn, arg, failure);
}

/* ------------------------------------------------------ geometry edits */

typedef struct axis_nudge {
	int axis;
	fix step;
	int mirrored;
	vms_vector plane;
} axis_nudge;

static fix *axis_component(vms_vector *v, int axis)
{
	return axis == 0 ? &v->x : axis == 1 ? &v->y : &v->z;
}

/* Tags on the mirror plane can't move across it without breaking symmetry. */
static void axis_delta(const builder_element *el, int vertnum, vms_vector *delta, void *arg)
{
	axis_nudge *nudge = arg;
	fix step = nudge->step;

	(void)vertnum;
	vm_vec_zero(delta);
	if (nudge->mirrored)
	{
		vms_vector center;
		fix offset;

		bld_element_center(el, &center);
		offset = *axis_component(&center, nudge->axis) - *axis_component(&nudge->plane, nudge->axis);
		if (labs(offset) < MIRROR_PLANE_SLACK)
			step = 0;
		else if (offset < 0)
			step = -step;
	}
	*axis_component(delta, nudge->axis) = step;
}

void builder_nudge_axis(int axis, int direction)
{
	axis_nudge nudge;

	nudge.axis = axis;
	nudge.step = bld_units(Builder.nudge_step) * direction;
	nudge.mirrored = axis == Builder.mirror_axis && builder_mirror_plane(&nudge.plane);
	move_tags(axis_delta, &nudge, "That would turn a cube inside out");
}

static void push_delta(const builder_element *el, int vertnum, vms_vector *delta, void *arg)
{
	(void)vertnum;
	bld_side_outward(&Segments[el->segnum], el->sidenum, delta);
	vm_vec_scale(delta, *(fix *)arg);
}

void builder_push_element(const builder_element *el, int direction, int steps)
{
	fix distance = bld_units(Builder.nudge_step) * direction * steps;

	if (el->valid)
		move_elements(el, 1, push_delta, &distance, "That would turn a cube inside out");
}

/* Inset (-1) or outset (+1) every tag along its own face. */
void builder_push(int direction, int steps)
{
	fix distance = bld_units(Builder.nudge_step) * direction * steps;

	move_tags(push_delta, &distance, "That would turn a cube inside out");
}

static fix snap_value(fix value, fix grid)
{
	return fixmul(fl2f(floor(f2fl(fixdiv(value, grid)) + 0.5)), grid);
}

static void snap_delta(const builder_element *el, int vertnum, vms_vector *delta, void *arg)
{
	fix grid = *(fix *)arg;
	vms_vector *v = &Vertices[vertnum];

	(void)el;
	delta->x = snap_value(v->x, grid) - v->x;
	delta->y = snap_value(v->y, grid) - v->y;
	delta->z = snap_value(v->z, grid) - v->z;
}

void builder_snap_selection(void)
{
	fix grid = bld_units(Builder.nudge_step);

	if (grid > 0)
		move_tags(snap_delta, &grid, "Snapping would turn a cube inside out");
}

/* Plane of the element's face, through the corners NOT in the element (all
 * four for a face), so a stray vertex or edge is pulled back in line. */
static void element_plane(const builder_element *el, vms_vector *point, vms_vector *normal)
{
	segment *seg = &Segments[el->segnum];
	int selected[BUILDER_ELEMENT_VERTS], n_selected = bld_element_vertices(el, selected);
	int k, i, used = 0;

	vm_vec_zero(point);
	for (k = 0; k < 4; k++)
	{
		int vertnum = bld_side_vertnum(seg, el->sidenum, k);
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
	bld_side_normal(seg, el->sidenum, normal);
}

static void flatten_delta(const builder_element *el, int vertnum, vms_vector *delta, void *arg)
{
	vms_vector point, normal, offset;

	(void)arg;
	element_plane(el, &point, &normal);
	vm_vec_sub(&offset, &Vertices[vertnum], &point);
	vm_vec_copy_scale(delta, &normal, -vm_vec_dot(&offset, &normal));
}

void builder_flatten_selection(void)
{
	move_tags(flatten_delta, NULL, "Flattening would turn a cube inside out");
}

static int tagged_vertices(int *out)
{
	int count = 0, t, i, j;

	for (t = 0; t < Tag_count; t++)
	{
		int verts[BUILDER_ELEMENT_VERTS], n = bld_element_vertices(&Tags[t], verts);

		for (i = 0; i < n; i++)
		{
			int seen = 0;

			for (j = 0; j < count; j++)
				if (out[j] == verts[i])
					seen = 1;
			if (!seen && count < MAX_MOVES)
				out[count++] = verts[i];
		}
	}
	return count;
}

/* Merge each tagged vertex into the nearest untagged vertex within one
 * step, then join faces that now coincide. Closes hairline gaps. */
void builder_weld_selection(void)
{
	int verts[MAX_MOVES], count, i, welded = 0;

	if (!need_tags() || !bld_begin_edit())
		return;
	count = tagged_vertices(verts);
	for (i = 0; i < count; i++)
	{
		int target = bld_nearest_vertex(verts[i], verts, count, bld_units(Builder.nudge_step));

		if (target < 0)
			continue;
		bld_replace_vertex(verts[i], target);
		welded++;
	}
	if (!welded)
	{
		bld_cancel_edit();
		bld_refuse("No vertex within one step to weld to");
		return;
	}
	if (bld_finish_edit(bld_revalidate_all(), "Welding would collapse a cube"))
		HUD_init_message(HM_DEFAULT, "Welded %d vertex%s", welded, welded == 1 ? "" : "es");
}

/* -------------------------------------------------------------- cubes */

static int face_seen(const builder_element *els, int upto, const builder_element *el)
{
	int i;

	for (i = 0; i < upto; i++)
		if (els[i].segnum == el->segnum && els[i].sidenum == el->sidenum)
			return 1;
	return 0;
}

/* The tags ride out onto the new faces, so repeated builds keep extending. */
static int extrude_elements(builder_element *els, int n_els)
{
	const char *failure = NULL;
	int i;

	if (!bld_begin_edit())
		return 0;
	for (i = 0; i < n_els && !failure; i++)
	{
		int newnum = Num_segments;

		if (face_seen(els, i, &els[i]))
			continue;
		failure = bld_extrude(els[i].segnum, els[i].sidenum);
		if (!failure)
			els[i].segnum = newnum;
	}
	return bld_finish_edit(!failure, failure);
}

void builder_extrude_selection(void)
{
	if (need_tags())
		extrude_elements(Tags, Tag_count);
}

void builder_extrude_at(const builder_element *el)
{
	builder_element copy = *el;

	if (el->valid && extrude_elements(&copy, 1))
		builder_select(&copy);
}

static int collect_segments(const builder_element *els, int n_els, int *out)
{
	int count = 0, i, j;

	for (i = 0; i < n_els; i++)
	{
		int seen = 0;

		for (j = 0; j < count; j++)
			if (out[j] == els[i].segnum)
				seen = 1;
		if (!seen)
			out[count++] = els[i].segnum;
	}
	return count;
}

/* Highest first: deleting fills the hole with the last segment, which must
 * never be one still waiting to be deleted. */
static void sort_descending(int *values, int count)
{
	int i, j;

	for (i = 1; i < count; i++)
		for (j = i; j > 0 && values[j] > values[j - 1]; j--)
		{
			int swap = values[j];

			values[j] = values[j - 1];
			values[j - 1] = swap;
		}
}

static void delete_elements(const builder_element *els, int n_els)
{
	int segs[BUILDER_MAX_TAGS], count, i;

	count = collect_segments(els, n_els, segs);
	for (i = 0; i < count; i++)
	{
		const char *blocker = bld_delete_blocker(segs[i]);

		if (blocker)
		{
			bld_refuse(blocker);
			return;
		}
	}
	if (count >= Num_segments)
	{
		bld_refuse("Can't delete every cube");
		return;
	}
	if (!bld_begin_edit())
		return;
	sort_descending(segs, count);
	for (i = 0; i < count; i++)
		bld_delete(segs[i]);
	builder_clear_tags();
	bld_finish_edit(1, NULL);
}

void builder_delete_selection(void)
{
	if (need_tags())
		delete_elements(Tags, Tag_count);
}

void builder_delete_at(const builder_element *el)
{
	if (el->valid)
		delete_elements(el, 1);
}

/* ------------------------------------------------------------ textures */

void builder_cycle_texture(int delta)
{
	Builder.texture = (Builder.texture + delta) % NumTextures;
	if (Builder.texture < 0)
		Builder.texture += NumTextures;
}

static side *face_side(const builder_element *face);
static int unfold_onto(const builder_element *a, const builder_element *b);
static int link_faces(const builder_element *faces, int count);

/* ---- recent textures: the last few painted or picked, newest first */

#define RECENT_MAX	10

static int Recent[RECENT_MAX];
static int Recent_count;

static void remember_texture(int texnum)
{
	int i = 0;

	while (i < Recent_count && Recent[i] != texnum)
		i++;
	if (i == Recent_count && Recent_count < RECENT_MAX)
		Recent_count++;
	if (i >= RECENT_MAX)
		i = RECENT_MAX - 1;
	memmove(&Recent[1], &Recent[0], sizeof(Recent[0]) * i);
	Recent[0] = texnum;
}

int builder_recent_count(void)
{
	return Recent_count;
}

int builder_recent(int index)
{
	return Recent[index];
}

static void paint_layer(segment *seg, int sidenum)
{
	if (Builder.layer == BLAYER_OVERLAY)
	{
		bld_paint_overlay(seg, sidenum, Builder.texture2, 0);
		return;
	}
	bld_paint_side(seg, sidenum);
	remember_texture(Builder.texture);
}

static void paint_elements(const builder_element *els, int n_els, int whole_segment)
{
	int i, sidenum;

	if (!bld_begin_edit())
		return;
	for (i = 0; i < n_els; i++)
	{
		segment *seg = &Segments[els[i].segnum];

		if (!whole_segment && els[i].mode != BSEL_CUBE)
		{
			paint_layer(seg, els[i].sidenum);
			continue;
		}
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			if (bld_side_is_solid(seg, sidenum))
				paint_layer(seg, sidenum);
	}
	bld_finish_edit(1, NULL);
}

void builder_paint_selection(int whole_segment)
{
	if (need_tags())
		paint_elements(Tags, Tag_count, whole_segment);
}

void builder_paint_at(const builder_element *el)
{
	if (el->valid)
		paint_elements(el, 1, 0);
}

/* A brush stroke: with Seamless on, the face carries on the previous face's mapping. */
void builder_paint_continuing(const builder_element *el, const builder_element *previous)
{
	if (!el->valid || !bld_begin_edit())
		return;
	paint_layer(&Segments[el->segnum], el->sidenum);
	if (Builder.seamless && Builder.layer == BLAYER_BASE && previous && previous->valid &&
		previous->segnum < Num_segments)
		unfold_onto(previous, el);
	bld_finish_edit(1, NULL);
}

/* ---- fill: one click paints a region */

#define ROOM_CUBES	64

static int add_face(builder_element *out, int count, int segnum, int sidenum)
{
	out[count].valid = 1;
	out[count].mode = BSEL_FACE;
	out[count].segnum = segnum;
	out[count].sidenum = sidenum;
	out[count].corner = 0;
	return count + 1;
}

static int gather_cube(builder_element *out, int count, int segnum)
{
	int sidenum;

	for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		if (bld_side_is_solid(&Segments[segnum], sidenum))
			count = add_face(out, count, segnum, sidenum);
	return count;
}

static int gather_room(builder_element *out, int segnum)
{
	int cubes[ROOM_CUBES], n_cubes = 1, count = 0, i, sidenum, j;

	cubes[0] = segnum;
	for (i = 0; i < n_cubes; i++)
	{
		count = gather_cube(out, count, cubes[i]);
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
		{
			int child = Segments[cubes[i]].children[sidenum], seen = 0;

			if (child < 0 || n_cubes >= ROOM_CUBES)
				continue;
			for (j = 0; j < n_cubes; j++)
				seen |= cubes[j] == child;
			if (!seen)
				cubes[n_cubes++] = child;
		}
	}
	return count;
}

static int gather_same_texture(builder_element *out, int texnum)
{
	int segnum, sidenum, count = 0;

	for (segnum = 0; segnum < Num_segments; segnum++)
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			if (bld_side_is_solid(&Segments[segnum], sidenum) &&
				Segments[segnum].sides[sidenum].tmap_num == texnum)
				count = add_face(out, count, segnum, sidenum);
	return count;
}

static int gather_region(const builder_element *el, builder_element *out)
{
	switch (Builder.fill_scope)
	{
		case BFILL_CUBE:	return gather_cube(out, 0, el->segnum);
		case BFILL_ROOM:	return gather_room(out, el->segnum);
		case BFILL_SAME:	return gather_same_texture(out, Segments[el->segnum].sides[el->sidenum].tmap_num);
		default:		return expand_wall(el, out, BUILDER_MAX_TAGS);
	}
}

void builder_fill_at(const builder_element *el)
{
	builder_element *faces;
	char text[40];
	int count, i;

	if (!el->valid)
		return;
	MALLOC(faces, builder_element, Num_segments * MAX_SIDES_PER_SEGMENT + BUILDER_MAX_TAGS);
	if (!faces)
		return;
	count = gather_region(el, faces);
	if (count && bld_begin_edit())
	{
		for (i = 0; i < count; i++)
			paint_layer(&Segments[faces[i].segnum], faces[i].sidenum);
		if (Builder.seamless && Builder.layer == BLAYER_BASE && Builder.fill_scope != BFILL_SAME)
			link_faces(faces, count);
		bld_finish_edit(1, NULL);
		snprintf(text, sizeof(text), "Filled %d face%s", count, count == 1 ? "" : "s");
		bld_message(text);
	}
	d_free(faces);
}

/* Right click with Paint: take the face's texture, and its overlay if it has one. */
void builder_eyedrop(const builder_element *el)
{
	side *sidep;
	char text[48];

	if (!el->valid)
		return;
	sidep = face_side(el);
	Builder.texture = sidep->tmap_num;
	remember_texture(Builder.texture);
	if (sidep->tmap_num2 & 0x3fff)
		Builder.texture2 = sidep->tmap_num2 & 0x3fff;
	snprintf(text, sizeof(text), "Picked #%d %s", Builder.texture, TmapInfo[Builder.texture].filename);
	bld_message(text);
}

int builder_active_texture(void)
{
	return Builder.layer == BLAYER_OVERLAY ? Builder.texture2 : Builder.texture;
}

void builder_set_active_texture(int texnum)
{
	if (Builder.layer == BLAYER_BASE)
	{
		Builder.texture = texnum;
		remember_texture(texnum);
	}
	else if (texnum > 0)
		Builder.texture2 = texnum;
}

void builder_pick_texture_at(const builder_element *el)
{
	side *sidep;

	if (!el->valid)
		return;
	sidep = &Segments[el->segnum].sides[el->sidenum];
	if (Builder.layer == BLAYER_BASE)
		Builder.texture = sidep->tmap_num;
	else if (sidep->tmap_num2 & 0x3fff)
		Builder.texture2 = sidep->tmap_num2 & 0x3fff;
	else
		bld_message("That face has no overlay");
}

void builder_clear_overlay(void)
{
	int i;

	if (!need_tags() || !bld_begin_edit())
		return;
	for (i = 0; i < Tag_count; i++)
		Segments[Tags[i].segnum].sides[Tags[i].sidenum].tmap_num2 = 0;
	bld_finish_edit(1, NULL);
}

void builder_pick_texture(void)
{
	if (need_tags())
		builder_pick_texture_at(&Tags[0]);
}

static void apply_tagged_faces(void (*change)(side *sidep, void *arg), void *arg)
{
	int i;

	for (i = 0; i < Tag_count; i++)
	{
		if (face_seen(Tags, i, &Tags[i]))
			continue;
		change(&Segments[Tags[i].segnum].sides[Tags[i].sidenum], arg);
	}
}

static void edit_tagged_faces(void (*change)(side *sidep, void *arg), void *arg)
{
	if (!need_tags() || !bld_begin_edit())
		return;
	apply_tagged_faces(change, arg);
	bld_finish_edit(1, NULL);
}

typedef struct corner_edit {
	void (*change)(uvl *corner, void *arg);
	void *arg;
} corner_edit;

static void change_each_corner(side *sidep, void *arg)
{
	corner_edit *edit = arg;
	int k;

	for (k = 0; k < UV_CORNERS; k++)
		edit->change(&sidep->uvls[k], edit->arg);
}

static void edit_tagged_uvs(void (*change)(uvl *corner, void *arg), void *arg)
{
	corner_edit edit = {change, arg};

	edit_tagged_faces(change_each_corner, &edit);
}

/* Turning and scaling pivot on the middle of every tagged face's UVs, so
 * faces linked into one mapping stay linked. */
typedef struct pivot_edit {
	float amount, u, v;
} pivot_edit;

static void tagged_uv_centre(float *u, float *v)
{
	int i, k, faces = 0;

	*u = *v = 0;
	for (i = 0; i < Tag_count; i++)
	{
		side *sidep;

		if (face_seen(Tags, i, &Tags[i]))
			continue;
		sidep = &Segments[Tags[i].segnum].sides[Tags[i].sidenum];
		for (k = 0; k < UV_CORNERS; k++)
		{
			*u += f2fl(sidep->uvls[k].u);
			*v += f2fl(sidep->uvls[k].v);
		}
		faces++;
	}
	if (faces)
	{
		*u /= faces * UV_CORNERS;
		*v /= faces * UV_CORNERS;
	}
}

static void uv_turn_face(side *sidep, void *arg)
{
	const pivot_edit *turn = arg;
	float radians = turn->amount * (float)M_PI / 180.0f;
	int k;

	for (k = 0; k < UV_CORNERS; k++)
	{
		float u = f2fl(sidep->uvls[k].u) - turn->u, v = f2fl(sidep->uvls[k].v) - turn->v;

		sidep->uvls[k].u = fl2f(turn->u + u * cosf(radians) - v * sinf(radians));
		sidep->uvls[k].v = fl2f(turn->v + u * sinf(radians) + v * cosf(radians));
	}
}

static void uv_zoom_face(side *sidep, void *arg)
{
	const pivot_edit *zoom = arg;
	int k;

	for (k = 0; k < UV_CORNERS; k++)
	{
		sidep->uvls[k].u = fl2f(zoom->u + (f2fl(sidep->uvls[k].u) - zoom->u) * zoom->amount);
		sidep->uvls[k].v = fl2f(zoom->v + (f2fl(sidep->uvls[k].v) - zoom->v) * zoom->amount);
	}
}

static void uv_shift_face(side *sidep, void *arg)
{
	const float *delta = arg;
	int k;

	for (k = 0; k < UV_CORNERS; k++)
	{
		sidep->uvls[k].u += fl2f(delta[0]);
		sidep->uvls[k].v += fl2f(delta[1]);
	}
}

void builder_uv_turn(float degrees)
{
	pivot_edit turn = {degrees, 0, 0};

	if (!need_tags())
		return;
	tagged_uv_centre(&turn.u, &turn.v);
	edit_tagged_faces(uv_turn_face, &turn);
}

/* Factor above 1 repeats the texture more (smaller); below 1 enlarges it. */
void builder_uv_zoom(float factor)
{
	pivot_edit zoom = {factor, 0, 0};

	if (!need_tags())
		return;
	tagged_uv_centre(&zoom.u, &zoom.v);
	edit_tagged_faces(uv_zoom_face, &zoom);
}

/* A drag is one edit: opened on grab, applied live, closed on release. */
static int Drag_open;

int builder_uv_drag_begin(void)
{
	if (Drag_open || !need_tags() || !bld_begin_edit())
		return 0;
	Drag_open = 1;
	return 1;
}

void builder_uv_drag_slide(float du, float dv)
{
	float delta[2] = {du, dv};

	if (Drag_open)
		apply_tagged_faces(uv_shift_face, delta);
}

void builder_uv_drag_turn(float degrees)
{
	pivot_edit turn = {degrees, 0, 0};

	if (!Drag_open)
		return;
	tagged_uv_centre(&turn.u, &turn.v);
	apply_tagged_faces(uv_turn_face, &turn);
}

void builder_uv_drag_zoom(float factor)
{
	pivot_edit zoom = {factor, 0, 0};

	if (!Drag_open)
		return;
	tagged_uv_centre(&zoom.u, &zoom.v);
	apply_tagged_faces(uv_zoom_face, &zoom);
}

void builder_uv_drag_end(void)
{
	if (!Drag_open)
		return;
	Drag_open = 0;
	bld_finish_edit(1, NULL);
}

/* ---- link: unfold faces into one continuous texture sheet ---- */

#define MIN_EDGE_LENGTH		0.01f
#define OVERLAY_ORIENT_MASK	0xc000

static void float_vector(const vms_vector *v, float *out)
{
	out[0] = f2fl(v->x);
	out[1] = f2fl(v->y);
	out[2] = f2fl(v->z);
}

static float dot3(const float *a, const float *b)
{
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static side *face_side(const builder_element *face)
{
	return &Segments[face->segnum].sides[face->sidenum];
}

static int face_vert(const builder_element *face, int corner)
{
	return bld_side_vertnum(&Segments[face->segnum], face->sidenum, corner);
}

/* The two corners of face b that face a also has; false unless it's an edge. */
static int shared_edge(const builder_element *a, const builder_element *b, int *a_corner, int *b_corner)
{
	int i, j, found = 0;

	for (i = 0; i < UV_CORNERS; i++)
		for (j = 0; j < UV_CORNERS; j++)
			if (face_vert(a, i) == face_vert(b, j) && found < 2)
			{
				a_corner[found] = i;
				b_corner[found] = j;
				found++;
			}
	return found == 2;
}

static void corner_position(const builder_element *face, int corner, float *out)
{
	float_vector(&Vertices[face_vert(face, corner)], out);
}

typedef struct edge_frame {
	float origin[3], along[3], scale;
	float u0, v0, edge_u, edge_v, perp_u, perp_v;
} edge_frame;

/* The shared edge, and which way the new face continues in UV space. */
static int edge_frame_of(const builder_element *a, const builder_element *b, edge_frame *f)
{
	int ac[2], bc[2], k;
	float p1[3], edge[3], length, uv_length, centre_u = 0, centre_v = 0;

	if (!shared_edge(a, b, ac, bc))
		return 0;
	corner_position(b, bc[0], f->origin);
	corner_position(b, bc[1], p1);
	for (k = 0; k < 3; k++)
		edge[k] = p1[k] - f->origin[k];
	length = sqrtf(dot3(edge, edge));
	if (length < MIN_EDGE_LENGTH)
		return 0;
	for (k = 0; k < 3; k++)
		f->along[k] = edge[k] / length;
	f->u0 = f2fl(face_side(a)->uvls[ac[0]].u);
	f->v0 = f2fl(face_side(a)->uvls[ac[0]].v);
	f->edge_u = f2fl(face_side(a)->uvls[ac[1]].u) - f->u0;
	f->edge_v = f2fl(face_side(a)->uvls[ac[1]].v) - f->v0;
	uv_length = sqrtf(f->edge_u * f->edge_u + f->edge_v * f->edge_v);
	if (uv_length < MIN_EDGE_LENGTH / 100.0f)
		return 0;
	f->scale = uv_length / length;
	f->edge_u /= uv_length;
	f->edge_v /= uv_length;
	for (k = 0; k < UV_CORNERS; k++)
	{
		centre_u += f2fl(face_side(a)->uvls[k].u) / UV_CORNERS;
		centre_v += f2fl(face_side(a)->uvls[k].v) / UV_CORNERS;
	}
	f->perp_u = -f->edge_v;
	f->perp_v = f->edge_u;
	if (f->perp_u * (centre_u - f->u0) + f->perp_v * (centre_v - f->v0) > 0)
	{
		f->perp_u = -f->perp_u;
		f->perp_v = -f->perp_v;
	}
	return 1;
}

/* Lay face b out beside face a, across their shared edge, continuing a's UVs. */
static int unfold_onto(const builder_element *a, const builder_element *b)
{
	edge_frame f;
	int k;

	if (!edge_frame_of(a, b, &f))
		return 0;
	for (k = 0; k < UV_CORNERS; k++)
	{
		float p[3], d[3], off[3], x, y;
		int i;

		corner_position(b, k, p);
		for (i = 0; i < 3; i++)
			d[i] = p[i] - f.origin[i];
		x = dot3(d, f.along);
		for (i = 0; i < 3; i++)
			off[i] = d[i] - f.along[i] * x;
		y = sqrtf(dot3(off, off));
		face_side(b)->uvls[k].u = fl2f(f.u0 + f.edge_u * x * f.scale + f.perp_u * y * f.scale);
		face_side(b)->uvls[k].v = fl2f(f.v0 + f.edge_v * x * f.scale + f.perp_v * y * f.scale);
	}
	bld_rebase_side_uvs(face_side(b));
	if (face_side(b)->tmap_num2)
		face_side(b)->tmap_num2 = (face_side(b)->tmap_num2 & ~OVERLAY_ORIENT_MASK) | (face_side(a)->tmap_num2 & OVERLAY_ORIENT_MASK);
	return 1;
}

/* From the first face outward: each touching face is unfolded in turn. */
static int link_faces(const builder_element *faces, int count)
{
	int done[BUILDER_MAX_TAGS] = {0};
	int linked = 1, progress = 1, a, b;

	if (count > BUILDER_MAX_TAGS)
		count = BUILDER_MAX_TAGS;
	done[0] = 1;
	while (progress)
	{
		progress = 0;
		for (b = 1; b < count; b++)
		{
			if (done[b])
				continue;
			for (a = 0; a < count && !done[b]; a++)
				if (done[a] && unfold_onto(&faces[a], &faces[b]))
				{
					done[b] = 1;
					linked++;
					progress = 1;
				}
		}
	}
	return linked;
}

void builder_uv_link(void)
{
	char text[64];
	int linked;

	if (!need_tags())
		return;
	if (Tag_count < 2)
	{
		bld_message("Tag two or more touching faces to link them");
		return;
	}
	if (!bld_begin_edit())
		return;
	linked = link_faces(Tags, Tag_count);
	bld_finish_edit(1, NULL);
	snprintf(text, sizeof(text), "Linked %d of %d faces (untouched ones keep their mapping)", linked, Tag_count);
	bld_message(text);
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

	edit_tagged_uvs(uv_shift, delta);
}

void builder_uv_rotate(void)
{
	edit_tagged_uvs(uv_rotate, NULL);
}

void builder_uv_flip(int flip_v)
{
	edit_tagged_uvs(uv_flip, &flip_v);
}

void builder_uv_reset(void)
{
	int i;

	if (!need_tags() || !bld_begin_edit())
		return;
	for (i = 0; i < Tag_count; i++)
		bld_map_side_uvs(&Segments[Tags[i].segnum], Tags[i].sidenum);
	bld_finish_edit(1, NULL);
}

/* ------------------------------------------------------------ lighting */

void builder_set_fullbright(int on)
{
	Builder.fullbright = on;
	bld_bake_lighting();
}

void builder_rebake(void)
{
	bld_bake_lighting();
	bld_message("Lighting rebuilt");
}
