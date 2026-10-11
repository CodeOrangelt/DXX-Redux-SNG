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
const char *const Builder_sel_mode_names[BSEL_COUNT] = {"Face", "Edge", "Vertex"};
const char *const Builder_link_names[BLINK_COUNT] = {"Sync", "Mirror"};
const char *const Builder_axis_names[3] = {"X", "Y", "Z"};
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
	if (a->segnum != b->segnum || a->sidenum != b->sidenum || a->mode != b->mode)
		return 0;
	return a->mode == BSEL_FACE || a->corner == b->corner;
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

void builder_select(const builder_element *el)
{
	Tag_count = 0;
	if (el->valid)
		Tags[Tag_count++] = *el;
}

void builder_toggle_tag(const builder_element *el)
{
	int index = find_tag(el);

	if (!el->valid)
		return;
	if (index >= 0)
		remove_tag(index);
	else if (Tag_count < BUILDER_MAX_TAGS)
		Tags[Tag_count++] = *el;
	else
		bld_message("Tag limit reached");
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
	if (el->mode == BSEL_FACE)
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
		int verts[4], count = bld_element_vertices(&els[e], verts);

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
	int selected[4], n_selected = bld_element_vertices(el, selected);
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
		int verts[4], n = bld_element_vertices(&Tags[t], verts);

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
static void extrude_elements(builder_element *els, int n_els)
{
	const char *failure = NULL;
	int i;

	if (!bld_begin_edit())
		return;
	for (i = 0; i < n_els && !failure; i++)
	{
		int newnum = Num_segments;

		if (face_seen(els, i, &els[i]))
			continue;
		failure = bld_extrude(els[i].segnum, els[i].sidenum);
		if (!failure)
			els[i].segnum = newnum;
	}
	bld_finish_edit(!failure, failure);
}

void builder_extrude_selection(void)
{
	if (need_tags())
		extrude_elements(Tags, Tag_count);
}

void builder_extrude_at(const builder_element *el)
{
	builder_element copy = *el;

	if (el->valid)
		extrude_elements(&copy, 1);
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

static void paint_elements(const builder_element *els, int n_els, int whole_segment)
{
	int i, sidenum;

	if (!bld_begin_edit())
		return;
	for (i = 0; i < n_els; i++)
	{
		segment *seg = &Segments[els[i].segnum];

		if (!whole_segment)
		{
			bld_paint_side(seg, els[i].sidenum);
			continue;
		}
		for (sidenum = 0; sidenum < MAX_SIDES_PER_SEGMENT; sidenum++)
			if (bld_side_is_solid(seg, sidenum))
				bld_paint_side(seg, sidenum);
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

void builder_pick_texture_at(const builder_element *el)
{
	if (el->valid)
		Builder.texture = Segments[el->segnum].sides[el->sidenum].tmap_num;
}

void builder_pick_texture(void)
{
	if (need_tags())
		builder_pick_texture_at(&Tags[0]);
}

static void edit_tagged_uvs(void (*change)(uvl *corner, void *arg), void *arg)
{
	int i, k;

	if (!need_tags() || !bld_begin_edit())
		return;
	for (i = 0; i < Tag_count; i++)
	{
		side *sidep;

		if (face_seen(Tags, i, &Tags[i]))
			continue;
		sidep = &Segments[Tags[i].segnum].sides[Tags[i].sidenum];
		for (k = 0; k < UV_CORNERS; k++)
			change(&sidep->uvls[k], arg);
	}
	bld_finish_edit(1, NULL);
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
