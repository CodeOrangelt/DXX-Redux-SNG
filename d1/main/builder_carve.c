/*
 * SNG: level builder carving -- the carve wheel (hold C).
 *
 * Where shapes grow new cubes, a carve reshapes the cube you're looking at:
 * it moves that cube's eight corners, and any neighbour sharing a corner
 * follows. Corners are named from the player's view of the wall: FACE is the
 * wall under the crosshair, BACK the cube's far side, and "out" runs along
 * the wall's normal, away from you. Moves are fractions of the cube's
 * thickness, so a carve scales with the cube.
 */

#include <math.h>

#include "inferno.h"
#include "segment.h"
#include "vecmat.h"
#include "builder.h"
#include "builder_int.h"

#define MAX_CARVE_OPS	4
#define BUILDER_CARVE_PAGES	1
#define CUBE_CORNERS	8
#define FACE_CORNERS	4
#define MIN_THICKNESS	0.01f

enum { LAYER_FACE = 1, LAYER_BACK = 2, LAYER_BOTH = 3 };
enum { REG_ALL, REG_TOP, REG_BOTTOM, REG_LEFT, REG_RIGHT, REG_TL, REG_TR, REG_BL, REG_BR };
enum { CK_END, CK_MOVE, CK_SCALE, CK_TWIST, CK_STRAIGHT };

typedef struct carve_op {
	int kind, layer, region;
	float a, b, c;
} carve_op;

typedef struct carve_recipe {
	const char *name;
	carve_op ops[MAX_CARVE_OPS + 1];
} carve_recipe;

typedef struct carve_page {
	const char *name;
	int count;
	const carve_recipe *recipes;
} carve_page;

#define MOVE(layer, region, out, right, up)	{CK_MOVE, layer, region, out, right, up}
#define SQUEEZE(layer, region, fx, fy)		{CK_SCALE, layer, region, fx, fy, 0}
#define STRAIGHTEN				{CK_STRAIGHT, LAYER_BOTH, REG_ALL, 0, 0, 0}
#define STOP					{CK_END, 0, 0, 0, 0, 0}

static const carve_recipe Carves[] = {
	{"Dent",	{MOVE(LAYER_FACE, REG_ALL, 0.5f, 0, 0), STOP}},
	{"Bulge",	{MOVE(LAYER_FACE, REG_ALL, -0.5f, 0, 0), STOP}},
	{"Dish",	{SQUEEZE(LAYER_FACE, REG_ALL, 0.3f, 0.3f), MOVE(LAYER_FACE, REG_ALL, 0.3f, 0, 0), STOP}},
	{"Boss",	{SQUEEZE(LAYER_FACE, REG_ALL, 0.4f, 0.4f), MOVE(LAYER_FACE, REG_ALL, -0.5f, 0, 0), STOP}},
	{"Chamfer",	{SQUEEZE(LAYER_FACE, REG_ALL, 0.3f, 0.3f), STOP}},
	{"Cut corners",	{MOVE(LAYER_FACE, REG_TL, 0, 0.4f, -0.4f), MOVE(LAYER_FACE, REG_TR, 0, -0.4f, -0.4f),
			 MOVE(LAYER_FACE, REG_BL, 0, 0.4f, 0.4f), MOVE(LAYER_FACE, REG_BR, 0, -0.4f, 0.4f), STOP}},
	{"Slant top",	{MOVE(LAYER_FACE, REG_TOP, 0.5f, 0, 0), STOP}},
	{"Straighten",	{STRAIGHTEN, STOP}},
};

static const carve_page Pages[BUILDER_CARVE_PAGES] = {
	{"Carve", (int)(sizeof(Carves) / sizeof(Carves[0])), Carves},
};

/* ---------------------------------------------------------------- queries */

const char *builder_carve_page_name(int page)
{
	return Pages[page].name;
}

int builder_carve_count(int page)
{
	return Pages[page].count;
}

const char *builder_carve_name(int page, int index)
{
	return Pages[page].recipes[index].name;
}

/* ------------------------------------------------------------------ maths */

/* Corner i (0-3 the face, 4-7 the back) sits in the quadrant its face
 * corner started in, however far earlier ops have moved it. */
typedef struct quadrant {
	int right, up;
} quadrant;

static int in_region(int region, quadrant q)
{
	switch (region)
	{
		case REG_TOP:		return q.up > 0;
		case REG_BOTTOM:	return q.up < 0;
		case REG_LEFT:		return q.right < 0;
		case REG_RIGHT:		return q.right > 0;
		case REG_TL:		return q.right < 0 && q.up > 0;
		case REG_TR:		return q.right > 0 && q.up > 0;
		case REG_BL:		return q.right < 0 && q.up < 0;
		case REG_BR:		return q.right > 0 && q.up < 0;
	}
	return 1;
}

static int selected(const carve_op *op, int corner, const quadrant *quads)
{
	int layer = corner < FACE_CORNERS ? LAYER_FACE : LAYER_BACK;

	return (op->layer & layer) && in_region(op->region, quads[corner % FACE_CORNERS]);
}

static void centroid(const carve_op *op, const carve_point *pts, const quadrant *quads, carve_point *center)
{
	int i, n = 0;

	center->x = center->y = center->z = 0;
	for (i = 0; i < CUBE_CORNERS; i++)
	{
		if (!selected(op, i, quads))
			continue;
		center->x += pts[i].x;
		center->y += pts[i].y;
		center->z += pts[i].z;
		n++;
	}
	if (!n)
		return;
	center->x /= n;
	center->y /= n;
	center->z /= n;
}

static void apply_op(const carve_op *op, carve_point *pts, const quadrant *quads)
{
	carve_point center;
	float radians = op->a * (float)M_PI / 180.0f;
	int i;

	centroid(op, pts, quads, &center);
	for (i = 0; i < CUBE_CORNERS; i++)
	{
		carve_point *p = &pts[i];
		float x = p->x - center.x, y = p->y - center.y;

		if (op->kind == CK_STRAIGHT)
		{
			if (i >= FACE_CORNERS)
				*p = (carve_point){pts[i - FACE_CORNERS].x, pts[i - FACE_CORNERS].y, pts[i - FACE_CORNERS].z - 1.0f};
			continue;
		}
		if (!selected(op, i, quads))
			continue;
		if (op->kind == CK_MOVE)
		{
			p->z += op->a;
			p->x += op->b;
			p->y += op->c;
		}
		else if (op->kind == CK_SCALE)
		{
			p->x = center.x + x * (1 - op->a);
			p->y = center.y + y * (1 - op->b);
		}
		else if (op->kind == CK_TWIST)
		{
			p->x = center.x + x * cosf(radians) - y * sinf(radians);
			p->y = center.y + x * sinf(radians) + y * cosf(radians);
		}
	}
}

/* pts[0..3] are the wall's corners, pts[4..7] the cube's far side. */
static void run_recipe(const carve_recipe *recipe, carve_point *pts)
{
	quadrant quads[FACE_CORNERS];
	const carve_op *op;
	int k;

	for (k = 0; k < FACE_CORNERS; k++)
	{
		quads[k].right = pts[k].x < 0 ? -1 : 1;
		quads[k].up = pts[k].y < 0 ? -1 : 1;
	}
	for (op = recipe->ops; op->kind != CK_END; op++)
		apply_op(op, pts, quads);
}

/* The recipe on an ideal unit cube, for the wheel icon. */
void builder_carve_points(int page, int index, carve_point *out)
{
	static const float corner[FACE_CORNERS][2] = {{-0.5f, -0.5f}, {0.5f, -0.5f}, {0.5f, 0.5f}, {-0.5f, 0.5f}};
	int k;

	for (k = 0; k < FACE_CORNERS; k++)
	{
		out[k] = (carve_point){corner[k][0], corner[k][1], 0.5f};
		out[k + FACE_CORNERS] = (carve_point){corner[k][0], corner[k][1], -0.5f};
	}
	run_recipe(&Pages[page].recipes[index], out);
}

/* --------------------------------------------------------- on the real cube */

typedef struct cube_frame {
	vms_vector center, right, up, normal;
	float thickness;
	int verts[CUBE_CORNERS];
} cube_frame;

static void frame_basis(segment *seg, int sidenum, cube_frame *frame)
{
	vms_vector world_up;

	bld_side_outward(seg, sidenum, &frame->normal);
	vm_vec_make(&world_up, 0, F1_0, 0);
	if (labs(frame->normal.y) > F1_0 * 7 / 10)
		vm_vec_make(&world_up, 0, 0, F1_0);
	vm_vec_cross(&frame->right, &world_up, &frame->normal);
	vm_vec_normalize(&frame->right);
	vm_vec_cross(&frame->up, &frame->normal, &frame->right);
	vm_vec_normalize(&frame->up);
}

static void frame_of(const builder_element *face, cube_frame *frame)
{
	segment *seg = &Segments[face->segnum];
	fix thickness = 0;
	int k;

	for (k = 0; k < FACE_CORNERS; k++)
	{
		frame->verts[k] = bld_side_vertnum(seg, face->sidenum, k);
		frame->verts[k + FACE_CORNERS] = bld_side_partner(seg, face->sidenum, k);
	}
	frame_basis(seg, face->sidenum, frame);
	vm_vec_zero(&frame->center);
	for (k = 0; k < CUBE_CORNERS; k++)
		vm_vec_add2(&frame->center, &Vertices[frame->verts[k]]);
	vm_vec_scale2(&frame->center, 1, CUBE_CORNERS);
	for (k = 0; k < FACE_CORNERS; k++)
	{
		vms_vector across;

		vm_vec_sub(&across, &Vertices[frame->verts[k]], &Vertices[frame->verts[k + FACE_CORNERS]]);
		thickness += vm_vec_dot(&across, &frame->normal);
	}
	frame->thickness = f2fl(thickness) / FACE_CORNERS;
	if (frame->thickness < MIN_THICKNESS)
		frame->thickness = f2fl(bld_units(Builder.build_depth));
}

static void load_points(const cube_frame *frame, carve_point *pts)
{
	int i;

	for (i = 0; i < CUBE_CORNERS; i++)
	{
		vms_vector offset;

		vm_vec_sub(&offset, &Vertices[frame->verts[i]], &frame->center);
		pts[i].x = f2fl(vm_vec_dot(&offset, &frame->right)) / frame->thickness;
		pts[i].y = f2fl(vm_vec_dot(&offset, &frame->up)) / frame->thickness;
		pts[i].z = f2fl(vm_vec_dot(&offset, &frame->normal)) / frame->thickness;
	}
}

static void store_points(const cube_frame *frame, const carve_point *pts)
{
	int i;

	for (i = 0; i < CUBE_CORNERS; i++)
	{
		vms_vector *corner = &Vertices[frame->verts[i]];

		*corner = frame->center;
		vm_vec_scale_add2(corner, &frame->right, fl2f(pts[i].x * frame->thickness));
		vm_vec_scale_add2(corner, &frame->up, fl2f(pts[i].y * frame->thickness));
		vm_vec_scale_add2(corner, &frame->normal, fl2f(pts[i].z * frame->thickness));
	}
}

static const char *carve_cube(const carve_recipe *recipe, const builder_element *face)
{
	cube_frame frame;
	carve_point pts[CUBE_CORNERS];

	frame_of(face, &frame);
	load_points(&frame, pts);
	run_recipe(recipe, pts);
	store_points(&frame, pts);
	return bld_refresh_after_move(frame.verts, CUBE_CORNERS) ? NULL : "That cut would turn a cube inside out";
}

static int cube_seen(const builder_element *els, int upto, const builder_element *el)
{
	int i;

	for (i = 0; i < upto; i++)
		if (els[i].segnum == el->segnum)
			return 1;
	return 0;
}

static void carve_cubes(const carve_recipe *recipe, const builder_element *els, int count)
{
	const char *failure = NULL;
	int i;

	if (!bld_begin_edit())
		return;
	for (i = 0; i < count && !failure; i++)
		if (!cube_seen(els, i, &els[i]))
			failure = carve_cube(recipe, &els[i]);
	if (bld_finish_edit(!failure, failure))
		bld_message(recipe->name);
}

void builder_carve_at(int page, int index, const builder_element *el)
{
	if (el->valid)
		carve_cubes(&Pages[page].recipes[index], el, 1);
}

void builder_carve_selection(int page, int index)
{
	builder_element faces[BUILDER_MAX_TAGS];
	int count = builder_tag_count(), i;

	if (!count)
	{
		bld_refuse("Tag something first (middle click)");
		return;
	}
	for (i = 0; i < count; i++)
		faces[i] = *builder_tag(i);
	carve_cubes(&Pages[page].recipes[index], faces, count);
}
