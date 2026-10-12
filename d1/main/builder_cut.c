/*
 * SNG: level builder cuts -- the cube wheel (hold X).
 *
 * Descent faces connect one cube to one cube, so a wall can't have a hole
 * punched in it directly. Cutting gets round that: it slices the cube behind
 * the crosshair into slabs (and anything sharing the cut's faces with it),
 * after which the middle slab's wall is a small face of its own that the
 * shape wheels can grow a doorway from. Door does the whole job.
 */

#include <math.h>
#include <stdio.h>

#include "inferno.h"
#include "segment.h"
#include "gameseg.h"
#include "3d.h"
#include "gr.h"
#include "vecmat.h"
#include "builder.h"
#include "builder_int.h"

#define THIRD		(1.0f / 3.0f)
#define HALF		0.5f
#define PIECES		3
#define DOOR_CUBES	2
#define DOOR_PROBE_DEPTH	(2 * F1_0)
#define STEEP_NORMAL	(F1_0 * 7 / 10)

enum cut_kind {
	CUT_HALVE_ACROSS,
	CUT_HALVE_UP,
	CUT_HALVE_DEPTH,
	CUT_THIRDS_ACROSS,
	CUT_THIRDS_UP,
	CUT_THIRDS_DEPTH,
	CUT_GRID,
	CUT_DOOR
};

static const char *const Cut_names[BUILDER_CUT_COUNT] = {
	"Halve across", "Halve up", "Halve depth",
	"Thirds across", "Thirds up", "Thirds depth",
	"Grid 3x3", "Door",
};

const char *builder_cut_name(int index)
{
	return Cut_names[index];
}

/* A pair of opposite sides is named by its lower side number. */
static int pair_rep(int sidenum)
{
	return sidenum < Side_opposite[sidenum] ? sidenum : Side_opposite[sidenum];
}

static fix up_component(segment *seg, int sidenum, int rep, const vms_vector *world_up)
{
	vms_vector from, to, axis;

	compute_center_point_on_side(&from, seg, rep);
	compute_center_point_on_side(&to, seg, Side_opposite[rep]);
	vm_vec_sub(&axis, &to, &from);
	vm_vec_normalize(&axis);
	(void)sidenum;
	return labs(vm_vec_dot(&axis, (vms_vector *)world_up));
}

/* Of the two pairs of sides that run across the wall, which one is vertical. */
static void wall_pairs(segment *seg, int sidenum, int *across, int *up)
{
	vms_vector normal, world_up;
	int depth = pair_rep(sidenum), others[2], n = 0, rep;

	for (rep = 0; rep < MAX_SIDES_PER_SEGMENT; rep++)
		if (rep == pair_rep(rep) && rep != depth)
			others[n++] = rep;
	bld_side_normal(seg, sidenum, &normal);
	vm_vec_make(&world_up, 0, F1_0, 0);
	if (labs(normal.y) > STEEP_NORMAL)
		vm_vec_make(&world_up, 0, 0, F1_0);
	if (up_component(seg, sidenum, others[0], &world_up) >= up_component(seg, sidenum, others[1], &world_up))
	{
		*up = others[0];
		*across = others[1];
	}
	else
	{
		*up = others[1];
		*across = others[0];
	}
}

static const char *halve(int segnum, int rep)
{
	int fresh;

	return bld_split(segnum, rep, HALF, &fresh);
}

/* pieces[0] keeps the rep side; pieces[2] is farthest from it. */
static const char *thirds(int segnum, int rep, int *pieces)
{
	const char *failure;

	pieces[0] = segnum;
	failure = bld_split(segnum, rep, THIRD, &pieces[1]);
	if (!failure)
		failure = bld_split(pieces[1], rep, HALF, &pieces[2]);
	return failure;
}

/* Cutting one column along "up" also cuts its neighbours at the same
 * plane, so the second cut is made once, not once per column. */
static const char *grid(int segnum, int across, int up)
{
	int columns[PIECES], rows[PIECES];
	const char *failure = thirds(segnum, across, columns);

	return failure ? failure : thirds(columns[1], up, rows);
}

/* The cube that now owns the middle of the original wall. */
static int cell_at_wall(const builder_element *face, const vms_vector *probe)
{
	return find_point_seg((vms_vector *)probe, face->segnum);
}

static const char *door(const builder_element *face, int across, int up)
{
	segment *seg = &Segments[face->segnum];
	vms_vector probe, inward;
	const char *failure;
	int cell, i;

	compute_center_point_on_side(&probe, seg, face->sidenum);
	bld_side_normal(seg, face->sidenum, &inward);
	vm_vec_scale_add2(&probe, &inward, DOOR_PROBE_DEPTH);
	failure = grid(face->segnum, across, up);
	if (failure)
		return failure;
	cell = cell_at_wall(face, &probe);
	if (cell < 0)
		return "Couldn't find the middle of the wall";
	for (i = 0; i < DOOR_CUBES && !failure; i++)
	{
		failure = bld_extrude(cell, face->sidenum);
		cell = Num_segments - 1;
	}
	return failure;
}

static const char *run_cut(int kind, const builder_element *face)
{
	segment *seg = &Segments[face->segnum];
	int pieces[PIECES], across, up, depth = pair_rep(face->sidenum);

	wall_pairs(seg, face->sidenum, &across, &up);
	switch (kind)
	{
		case CUT_HALVE_ACROSS:	return halve(face->segnum, across);
		case CUT_HALVE_UP:	return halve(face->segnum, up);
		case CUT_HALVE_DEPTH:	return halve(face->segnum, depth);
		case CUT_THIRDS_ACROSS:	return thirds(face->segnum, across, pieces);
		case CUT_THIRDS_UP:	return thirds(face->segnum, up, pieces);
		case CUT_THIRDS_DEPTH:	return thirds(face->segnum, depth, pieces);
		case CUT_GRID:		return grid(face->segnum, across, up);
		case CUT_DOOR:		return door(face, across, up);
	}
	return "Unknown cut";
}

static int cube_listed(const int *segs, int count, int segnum)
{
	int i;

	for (i = 0; i < count; i++)
		if (segs[i] == segnum)
			return 1;
	return 0;
}

static void cut_faces(int kind, const builder_element *faces, int count)
{
	int segs[BUILDER_MAX_TAGS], n_segs = 0, i, before = Num_segments;
	const char *failure = NULL;
	char report[80];

	for (i = 0; i < count; i++)
		if (!cube_listed(segs, n_segs, faces[i].segnum))
			segs[n_segs++] = faces[i].segnum;
	if (!bld_begin_edit())
		return;
	if (!Builder.cut_neighbours)
		bld_split_limit(segs, n_segs);
	bld_split_touched_reset();
	for (i = 0; i < count && !failure; i++)
		if (!bld_split_was_touched(faces[i].segnum))
			failure = run_cut(kind, &faces[i]);
	bld_split_unlimit();
	builder_clear_tags();
	if (!bld_finish_edit(!failure, failure))
		return;
	if (bld_split_sealed())
		snprintf(report, sizeof(report), "%s: %d new cubes, %d openings sealed", Cut_names[kind],
			Num_segments - before, bld_split_sealed());
	else
		snprintf(report, sizeof(report), "%s: %d new cubes", Cut_names[kind], Num_segments - before);
	bld_message(report);
}

/* Cuts only ever touch tagged cubes; nothing is cut by pointing alone. */
void builder_cut_selection(int kind)
{
	builder_element faces[BUILDER_MAX_TAGS];
	int count = builder_tag_count(), i;

	if (!count)
	{
		bld_refuse("Tag a cube first (middle click), then cut");
		return;
	}
	for (i = 0; i < count; i++)
		faces[i] = *builder_tag(i);
	cut_faces(kind, faces, count);
}

/* -------------------------------------------------------------- preview */

#define PREVIEW_CUT_COLOR	BM_XRGB(31, 22, 0)
#define PREVIEW_NEIGHBOUR_COLOR	BM_XRGB(31, 6, 4)
#define PREVIEW_MAX_CUTS	64
#define PREVIEW_DOOR_COLOR	BM_XRGB(0, 31, 24)
#define CELL_NEAR		THIRD
#define CELL_FAR		(1.0f - THIRD)

static void preview_line(const vms_vector *a, const vms_vector *b)
{
	g3s_point pa, pb;

	g3_rotate_point(&pa, (vms_vector *)a);
	g3_rotate_point(&pb, (vms_vector *)b);
	g3_draw_line(&pa, &pb);
}

static void preview_loop(const vms_vector *corners)
{
	int k;

	for (k = 0; k < 4; k++)
		preview_line(&corners[k], &corners[(k + 1) % 4]);
}

static void lerp_point(vms_vector *out, const vms_vector *from, const vms_vector *to, float t)
{
	vms_vector along;

	vm_vec_sub(&along, to, from);
	vm_vec_scale(&along, fl2f(t));
	vm_vec_add(out, from, &along);
}

/* The ring a cut at fraction t of the way from side a would leave. */
static void preview_cut_ring(segment *seg, int a, float t)
{
	vms_vector ring[4];
	int k;

	for (k = 0; k < 4; k++)
		lerp_point(&ring[k], bld_side_vertex(seg, a, k), &Vertices[bld_side_partner(seg, a, k)], t);
	preview_loop(ring);
}

/* The cut on the tagged cube in yellow; cubes it has to run through in red. */
static void preview_plan(int segnum, int rep, float t)
{
	split_cut plan[PREVIEW_MAX_CUTS];
	int n = bld_split_plan(segnum, rep, t, plan, PREVIEW_MAX_CUTS), i;

	for (i = 0; i < n; i++)
	{
		gr_setcolor(i == 0 ? PREVIEW_CUT_COLOR : PREVIEW_NEIGHBOUR_COLOR);
		preview_cut_ring(&Segments[plan[i].segnum], plan[i].a, plan[i].t);
	}
}

static void preview_rings(int segnum, int rep, int cuts)
{
	int i;

	for (i = 1; i <= cuts; i++)
		preview_plan(segnum, rep, (float)i / (cuts + 1));
}

static int corner_on_side(segment *seg, int sidenum, int rep, int k)
{
	int cidx = Side_to_verts[sidenum][k], j;

	for (j = 0; j < 4; j++)
		if (Side_to_verts[rep][j] == cidx)
			return 1;
	return 0;
}

/* The wall's middle cell, where a Door opens, and the tunnel it leads into. */
static void preview_door(segment *seg, int sidenum, int across, int up)
{
	static const float at[4][2] = {{CELL_NEAR, CELL_NEAR}, {CELL_FAR, CELL_NEAR}, {CELL_FAR, CELL_FAR}, {CELL_NEAR, CELL_FAR}};
	vms_vector opening[4], tunnel[4], reach;
	int n, k;

	bld_side_outward(seg, sidenum, &reach);
	vm_vec_scale(&reach, bld_units(Builder.build_depth) * DOOR_CUBES);
	for (n = 0; n < 4; n++)
	{
		vm_vec_zero(&opening[n]);
		for (k = 0; k < 4; k++)
		{
			float wa = corner_on_side(seg, sidenum, across, k) ? 1.0f - at[n][0] : at[n][0];
			float wu = corner_on_side(seg, sidenum, up, k) ? 1.0f - at[n][1] : at[n][1];
			vms_vector part = *bld_side_vertex(seg, sidenum, k);

			vm_vec_scale(&part, fl2f(wa * wu));
			vm_vec_add2(&opening[n], &part);
		}
		vm_vec_add(&tunnel[n], &opening[n], &reach);
	}
	gr_setcolor(PREVIEW_DOOR_COLOR);
	preview_loop(opening);
	preview_loop(tunnel);
	for (n = 0; n < 4; n++)
		preview_line(&opening[n], &tunnel[n]);
}

static void preview_cube(int kind, const builder_element *face)
{
	segment *seg = &Segments[face->segnum];
	int across, up;

	wall_pairs(seg, face->sidenum, &across, &up);
	gr_setcolor(PREVIEW_CUT_COLOR);
	switch (kind)
	{
		case CUT_HALVE_ACROSS:	preview_rings(face->segnum, across, 1); break;
		case CUT_HALVE_UP:	preview_rings(face->segnum, up, 1); break;
		case CUT_HALVE_DEPTH:	preview_rings(face->segnum, pair_rep(face->sidenum), 1); break;
		case CUT_THIRDS_ACROSS:	preview_rings(face->segnum, across, 2); break;
		case CUT_THIRDS_UP:	preview_rings(face->segnum, up, 2); break;
		case CUT_THIRDS_DEPTH:	preview_rings(face->segnum, pair_rep(face->sidenum), 2); break;
		case CUT_GRID:
		case CUT_DOOR:
			preview_rings(face->segnum, across, 2);
			preview_rings(face->segnum, up, 2);
			break;
	}
	if (kind == CUT_DOOR)
		preview_door(seg, face->sidenum, across, up);
}

/* Draws where the chosen cut would fall, in the world, on every tagged cube. */
void builder_cut_preview(int kind)
{
	int segs[BUILDER_MAX_TAGS], n_segs = 0, i;

	for (i = 0; i < builder_tag_count(); i++)
		if (builder_tag(i)->segnum < Num_segments && !cube_listed(segs, n_segs, builder_tag(i)->segnum))
			segs[n_segs++] = builder_tag(i)->segnum;
	if (!Builder.cut_neighbours)
		bld_split_limit(segs, n_segs);
	for (i = 0; i < builder_tag_count(); i++)
	{
		const builder_element *face = builder_tag(i);

		if (face->segnum < Num_segments)
			preview_cube(kind, face);
	}
	bld_split_unlimit();
}
