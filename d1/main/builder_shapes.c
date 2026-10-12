/*
 * SNG: level builder shapes -- the number-key wheels (hold 1-9).
 *
 * A shape is a recipe: extrude a cube, then reshape the new far face
 * (taper, squeeze, slide, lift, twist), repeat. Recipes are grown off the
 * crosshair face, or every tagged face, as one undo step, and the tags move
 * to the far faces so shapes chain. New cubes always meet the old face
 * exactly (Descent faces connect one to one), so a hole can't be cut into
 * the middle of an existing wall -- only grown from it.
 */

#include <math.h>

#include "inferno.h"
#include "segment.h"
#include "vecmat.h"
#include "builder.h"
#include "builder_int.h"

#define MAX_OPS		20
#define FACE_CORNERS	4
#define PERSPECTIVE	0.28f

enum shape_op_kind {
	OP_END,
	OP_GROW,
	OP_TAPER,	/* toward the face centre; negative widens */
	OP_NARROW,	/* width only */
	OP_SQUAT,	/* height only */
	OP_SLIDE,	/* sideways, in cube depths; negative is left */
	OP_LIFT,	/* up, in cube depths */
	OP_TWIST	/* degrees about the face normal */
};

typedef struct shape_op {
	int kind;
	float value;
} shape_op;

typedef struct shape_recipe {
	const char *name;
	shape_op ops[MAX_OPS];
} shape_recipe;

typedef struct shape_page {
	const char *name;
	int count;
	const shape_recipe *recipes;
} shape_page;

#define GROW		{OP_GROW, 1}
#define TAPER(f)	{OP_TAPER, f}
#define NARROW(f)	{OP_NARROW, f}
#define SQUAT(f)	{OP_SQUAT, f}
#define SLIDE(f)	{OP_SLIDE, f}
#define LIFT(f)		{OP_LIFT, f}
#define TWIST(d)	{OP_TWIST, d}
#define END		{OP_END, 0}
#define TWICE(...)	__VA_ARGS__, __VA_ARGS__
#define THRICE(...)	__VA_ARGS__, __VA_ARGS__, __VA_ARGS__
#define FOURFOLD(...)	TWICE(__VA_ARGS__), TWICE(__VA_ARGS__)
#define SIXFOLD(...)	THRICE(__VA_ARGS__), THRICE(__VA_ARGS__)

static const shape_recipe Basics[] = {
	{"Cube",	{GROW, END}},
	{"Tunnel",	{THRICE(GROW), END}},
	{"Doorway",	{GROW, TAPER(0.35f), GROW, END}},
	{"Funnel",	{GROW, TAPER(0.4f), END}},
	{"Flare",	{GROW, TAPER(-0.4f), END}},
	{"Ramp up",	{TWICE(GROW, LIFT(0.5f)), END}},
	{"Ramp down",	{TWICE(GROW, LIFT(-0.5f)), END}},
};

static const shape_recipe Halls[] = {
	{"Long tunnel",	{SIXFOLD(GROW), END}},
	{"Narrow hall",	{GROW, NARROW(0.5f), THRICE(GROW), END}},
	{"Low crawl",	{GROW, SQUAT(0.5f), THRICE(GROW), END}},
	{"Wide hall",	{GROW, NARROW(-0.6f), THRICE(GROW), END}},
	{"Tall hall",	{GROW, SQUAT(-0.6f), THRICE(GROW), END}},
	{"Rising hall",	{FOURFOLD(GROW, LIFT(0.25f)), END}},
	{"Falling hall",{FOURFOLD(GROW, LIFT(-0.25f)), END}},
	{"Pinch hall",	{TWICE(GROW), TAPER(0.5f), TWICE(GROW), END}},
};

static const shape_recipe Rooms[] = {
	{"Room",	{GROW, TAPER(-0.6f), TWICE(GROW), END}},
	{"Grand room",	{GROW, TAPER(-1.0f), TWICE(GROW), END}},
	{"Wide room",	{GROW, NARROW(-1.0f), TWICE(GROW), END}},
	{"Tall room",	{GROW, SQUAT(-1.0f), TWICE(GROW), END}},
	{"Dome",	{GROW, TAPER(-0.5f), GROW, TAPER(0.5f), GROW, END}},
	{"Cellar",	{GROW, TAPER(-0.6f), GROW, TAPER(0.6f), END}},
	{"Hall room",	{GROW, NARROW(-0.8f), THRICE(GROW), NARROW(0.8f), END}},
	{"Atrium",	{GROW, TAPER(-0.8f), THRICE(GROW), TAPER(0.5f), GROW, END}},
};

static const shape_recipe Openings[] = {
	{"Doorway",	{GROW, TAPER(0.35f), GROW, END}},
	{"Wide door",	{GROW, SQUAT(0.4f), GROW, END}},
	{"Slot",	{GROW, NARROW(0.6f), GROW, END}},
	{"Window slit",	{GROW, SQUAT(0.6f), GROW, END}},
	{"Pinhole",	{GROW, TAPER(0.7f), GROW, END}},
	{"Airlock",	{GROW, TAPER(0.4f), TWICE(GROW), TAPER(-0.4f), GROW, END}},
	{"Archway",	{GROW, TAPER(0.2f), GROW, TAPER(-0.2f), END}},
	{"Gate",	{GROW, NARROW(0.4f), SQUAT(0.2f), GROW, END}},
};

static const shape_recipe Slopes[] = {
	{"Gentle up",	{THRICE(GROW, LIFT(0.25f)), END}},
	{"Steep up",	{TWICE(GROW, LIFT(1.0f)), END}},
	{"Gentle down",	{THRICE(GROW, LIFT(-0.25f)), END}},
	{"Steep down",	{TWICE(GROW, LIFT(-1.0f)), END}},
	{"Ramp, flat",	{TWICE(GROW, LIFT(0.5f)), TWICE(GROW), END}},
	{"Hill",	{TWICE(GROW, LIFT(0.5f)), TWICE(GROW, LIFT(-0.5f)), END}},
	{"Dip",		{TWICE(GROW, LIFT(-0.5f)), TWICE(GROW, LIFT(0.5f)), END}},
	{"Ledge up",	{GROW, LIFT(0.5f), TWICE(GROW), END}},
};

static const shape_recipe Bends[] = {
	{"Bend left",	{TWICE(GROW, SLIDE(-0.5f)), END}},
	{"Bend right",	{TWICE(GROW, SLIDE(0.5f)), END}},
	{"Sharp left",	{GROW, SLIDE(-1.0f), GROW, END}},
	{"Sharp right",	{GROW, SLIDE(1.0f), GROW, END}},
	{"Twist left",	{THRICE(GROW, TWIST(-20)), END}},
	{"Twist right",	{THRICE(GROW, TWIST(20)), END}},
	{"S-bend",	{TWICE(GROW, SLIDE(0.5f)), TWICE(GROW, SLIDE(-0.5f)), END}},
	{"Spiral up",	{FOURFOLD(GROW, SLIDE(0.3f), LIFT(0.3f), TWIST(15)), END}},
};

static const shape_recipe Points[] = {
	{"Pyramid",	{GROW, TAPER(0.7f), END}},
	{"Spike",	{TWICE(GROW, TAPER(0.4f)), END}},
	{"Hourglass",	{GROW, TAPER(0.5f), GROW, TAPER(-0.5f), END}},
	{"Diamond",	{GROW, TAPER(-0.4f), GROW, TAPER(0.6f), END}},
	{"Wedge",	{GROW, SQUAT(0.8f), END}},
	{"Blade",	{GROW, NARROW(0.8f), END}},
	{"Bulge",	{GROW, TAPER(-0.3f), GROW, TAPER(0.3f), GROW, END}},
	{"Stub",	{GROW, TAPER(0.2f), END}},
};

static const shape_recipe Grand[] = {
	{"Great hall",	{GROW, SQUAT(-0.5f), NARROW(-0.5f), SIXFOLD(GROW), END}},
	{"Narrow tunnel",{GROW, TAPER(0.4f), SIXFOLD(GROW), TWICE(GROW), END}},
	{"Grand ramp up",{SIXFOLD(GROW, LIFT(0.25f)), END}},
	{"Grand ramp down",{SIXFOLD(GROW, LIFT(-0.25f)), END}},
	{"Big room",	{GROW, TAPER(-1.0f), THRICE(GROW), TAPER(0.5f), END}},
	{"Long left",	{FOURFOLD(GROW, SLIDE(-0.25f), TWIST(-8)), END}},
	{"Long right",	{FOURFOLD(GROW, SLIDE(0.25f), TWIST(8)), END}},
	{"Long climb",	{FOURFOLD(GROW, LIFT(0.5f)), END}},
};

static const shape_recipe Sculpt[] = {
	{"Shear up",	{GROW, LIFT(0.5f), END}},
	{"Shear down",	{GROW, LIFT(-0.5f), END}},
	{"Slide left",	{GROW, SLIDE(-0.5f), END}},
	{"Slide right",	{GROW, SLIDE(0.5f), END}},
	{"Turn 45 left",{GROW, TWIST(-45), END}},
	{"Turn 45 right",{GROW, TWIST(45), END}},
	{"Squeeze",	{GROW, NARROW(0.3f), SQUAT(-0.3f), END}},
	{"Offset door",	{GROW, TAPER(0.35f), SLIDE(0.3f), GROW, END}},
};

#define PAGE(title, list) {title, (int)(sizeof(list) / sizeof(list[0])), list}

static const shape_page Pages[BUILDER_SHAPE_PAGES] = {
	PAGE("1 Basics", Basics),
	PAGE("2 Halls", Halls),
	PAGE("3 Rooms", Rooms),
	PAGE("4 Openings", Openings),
	PAGE("5 Slopes", Slopes),
	PAGE("6 Bends", Bends),
	PAGE("7 Points", Points),
	PAGE("8 Grand", Grand),
	PAGE("9 Sculpt", Sculpt),
};

/* ---------------------------------------------------------------- queries */

const char *builder_shape_page_name(int page)
{
	return Pages[page].name;
}

int builder_shape_count(int page)
{
	return Pages[page].count;
}

const char *builder_shape_name(int page, int index)
{
	return Pages[page].recipes[index].name;
}

/* The recipe as a stack of cross-sections, near to far, for the wheel icon. */
int builder_shape_stages(int page, int index, shape_stage *out, int max)
{
	const shape_op *op = Pages[page].recipes[index].ops;
	int count = 1;

	out[0] = (shape_stage){0, 0, 1, 1, 0};
	for (; op->kind != OP_END; op++)
	{
		shape_stage *far_face = &out[count - 1];

		switch (op->kind)
		{
			case OP_GROW:
				if (count < max)
					out[count++] = *far_face;
				break;
			case OP_TAPER:	far_face->w *= 1 - op->value; far_face->h *= 1 - op->value; break;
			case OP_NARROW:	far_face->w *= 1 - op->value; break;
			case OP_SQUAT:	far_face->h *= 1 - op->value; break;
			case OP_SLIDE:	far_face->cx += op->value; break;
			case OP_LIFT:	far_face->cy -= op->value; break;
			case OP_TWIST:	far_face->angle += op->value; break;
		}
	}
	return count;
}

/* ----------------------------------------------------------------- growing */

typedef struct face_frame {
	vms_vector center, right, up, normal;
	int verts[FACE_CORNERS];
} face_frame;

static void frame_of(const builder_element *face, face_frame *frame)
{
	segment *seg = &Segments[face->segnum];
	vms_vector world_up;
	int k;

	for (k = 0; k < FACE_CORNERS; k++)
		frame->verts[k] = bld_side_vertnum(seg, face->sidenum, k);
	vm_vec_zero(&frame->center);
	for (k = 0; k < FACE_CORNERS; k++)
		vm_vec_add2(&frame->center, &Vertices[frame->verts[k]]);
	vm_vec_scale2(&frame->center, 1, FACE_CORNERS);

	bld_side_outward(seg, face->sidenum, &frame->normal);
	vm_vec_make(&world_up, 0, F1_0, 0);
	if (labs(frame->normal.y) > F1_0 * 7 / 10)
		vm_vec_make(&world_up, 0, 0, F1_0);
	vm_vec_cross(&frame->right, &world_up, &frame->normal);
	vm_vec_normalize(&frame->right);
	vm_vec_cross(&frame->up, &frame->normal, &frame->right);
	vm_vec_normalize(&frame->up);
}

/* In-plane position (x, y) of a corner, in cube depths, and its way out. */
static void apply_op(const shape_op *op, float *x, float *y)
{
	float radians, rotated;

	switch (op->kind)
	{
		case OP_TAPER:	*x *= 1 - op->value; *y *= 1 - op->value; break;
		case OP_NARROW:	*x *= 1 - op->value; break;
		case OP_SQUAT:	*y *= 1 - op->value; break;
		case OP_SLIDE:	*x += op->value; break;
		case OP_LIFT:	*y += op->value; break;
		case OP_TWIST:
			radians = op->value * (float)M_PI / 180.0f;
			rotated = *x * cosf(radians) - *y * sinf(radians);
			*y = *x * sinf(radians) + *y * cosf(radians);
			*x = rotated;
			break;
	}
}

static const char *reshape(const builder_element *face, const shape_op *op)
{
	face_frame frame;
	float depth = f2fl(bld_units(Builder.build_depth));
	int k;

	frame_of(face, &frame);
	for (k = 0; k < FACE_CORNERS; k++)
	{
		vms_vector offset, *corner = &Vertices[frame.verts[k]];
		float x, y, out;

		vm_vec_sub(&offset, corner, &frame.center);
		x = f2fl(vm_vec_dot(&offset, &frame.right)) / depth;
		y = f2fl(vm_vec_dot(&offset, &frame.up)) / depth;
		out = f2fl(vm_vec_dot(&offset, &frame.normal));
		apply_op(op, &x, &y);
		*corner = frame.center;
		vm_vec_scale_add2(corner, &frame.right, fl2f(x * depth));
		vm_vec_scale_add2(corner, &frame.up, fl2f(y * depth));
		vm_vec_scale_add2(corner, &frame.normal, fl2f(out));
	}
	return bld_refresh_after_move(frame.verts, FACE_CORNERS) ? NULL : "That shape would turn a cube inside out";
}

static const char *grow(builder_element *face)
{
	const char *failure = bld_extrude(face->segnum, face->sidenum);

	if (!failure)
		face->segnum = Num_segments - 1;
	return failure;
}

static const char *run_recipe(const shape_recipe *recipe, builder_element *face)
{
	const shape_op *op;
	const char *failure = NULL;

	for (op = recipe->ops; op->kind != OP_END && !failure; op++)
		failure = op->kind == OP_GROW ? grow(face) : reshape(face, op);
	return failure;
}

static void build_shape(const shape_recipe *recipe, builder_element *faces, int count)
{
	const char *failure = NULL;
	int i;

	if (!bld_begin_edit())
		return;
	for (i = 0; i < count && !failure; i++)
		failure = run_recipe(recipe, &faces[i]);
	if (!bld_finish_edit(!failure, failure))
		return;
	builder_clear_tags();
	for (i = 0; i < count; i++)
		builder_toggle_tag(&faces[i]);
	bld_message(recipe->name);
}

void builder_shape_at(int page, int index, const builder_element *el)
{
	builder_element face = *el;

	if (el->valid)
		build_shape(&Pages[page].recipes[index], &face, 1);
}

void builder_shape_selection(int page, int index)
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
	build_shape(&Pages[page].recipes[index], faces, count);
}
