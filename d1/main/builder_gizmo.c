/*
 * SNG: level builder texture gizmo -- drag handles in the world, Blender style.
 *
 * With the Paint tool and something tagged, the tagged face shows a red arrow
 * (slide along U), a green arrow (slide along V), a blue ring (turn) and a
 * centre square (slide freely; hold Ctrl to resize). Put the crosshair on a
 * handle, hold the left button and move the mouse. Shift drags ten times
 * finer. A whole drag is one undo step.
 */

#include <math.h>

#include "inferno.h"
#include "segment.h"
#include "vecmat.h"
#include "3d.h"
#include "gr.h"
#include "key.h"
#include "object.h"
#include "builder.h"
#include "builder_int.h"

#define ARROW_STEPS		3
#define RING_STEPS		24
#define HIT_FRACTION		0.035f
#define DISTANCE_SCALE		0.10f
#define MIN_SIZE		(3 * F1_0)
#define MAX_SIZE		(40 * F1_0)
#define LIFT_FROM_FACE		(F1_0 * 3 / 2)
#define RING_RADIUS		0.7f
#define HEAD_LENGTH		0.25f
#define HEAD_WIDTH		0.12f
#define CENTRE_HALF		0.10f
#define DEFAULT_TILE_UNITS	20.0f
#define FINE_FACTOR		0.1f
#define ZOOM_PER_PIXEL		0.005f
#define MIN_DETERMINANT		1e-6f

#define COLOR_U		BM_XRGB(31, 6, 6)
#define COLOR_V		BM_XRGB(6, 31, 6)
#define COLOR_RING	BM_XRGB(8, 16, 31)
#define COLOR_CENTRE	BM_XRGB(31, 31, 31)
#define COLOR_HOT	BM_XRGB(31, 31, 0)

enum gizmo_part {
	GIZMO_NONE,
	GIZMO_U,
	GIZMO_V,
	GIZMO_RING,
	GIZMO_CENTRE,
	GIZMO_ZOOM
};

typedef struct gizmo_frame {
	vms_vector origin, u_dir, v_dir;
	float u_tile, v_tile;	/* world units per tile along each */
	fix size;
} gizmo_frame;

typedef struct screen_point {
	int x, y, visible;
} screen_point;

static gizmo_frame Frame;
static screen_point Origin_px, U_tip_px, V_tip_px;
static int Hover = GIZMO_NONE;
static int Dragging;
static int Grabbed;
static float Pointer_x, Pointer_y;

/* ------------------------------------------------------------------ maths */

static void to_floats(const vms_vector *v, float *out)
{
	out[0] = f2fl(v->x);
	out[1] = f2fl(v->y);
	out[2] = f2fl(v->z);
}

static float length3(const float *v)
{
	return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

static void to_unit_vector(const float *v, vms_vector *out)
{
	float length = length3(v);

	if (length <= 0)
		length = 1;
	vm_vec_make(out, fl2f(v[0] / length), fl2f(v[1] / length), fl2f(v[2] / length));
}

/* Which way the texture's U and V run across the face, in the world. */
static void uv_directions(const builder_element *face, gizmo_frame *frame)
{
	segment *seg = &Segments[face->segnum];
	side *sidep = &seg->sides[face->sidenum];
	float p0[3], p1[3], p3[3], e1[3], e3[3], wu[3], wv[3];
	float d1u, d1v, d3u, d3v, det;
	int i;

	to_floats(bld_side_vertex(seg, face->sidenum, 0), p0);
	to_floats(bld_side_vertex(seg, face->sidenum, 1), p1);
	to_floats(bld_side_vertex(seg, face->sidenum, 3), p3);
	d1u = f2fl(sidep->uvls[1].u - sidep->uvls[0].u);
	d1v = f2fl(sidep->uvls[1].v - sidep->uvls[0].v);
	d3u = f2fl(sidep->uvls[3].u - sidep->uvls[0].u);
	d3v = f2fl(sidep->uvls[3].v - sidep->uvls[0].v);
	det = d1u * d3v - d3u * d1v;
	if (fabsf(det) < MIN_DETERMINANT)
		det = MIN_DETERMINANT;
	for (i = 0; i < 3; i++)
	{
		e1[i] = p1[i] - p0[i];
		e3[i] = p3[i] - p0[i];
		wu[i] = (e1[i] * d3v - e3[i] * d1v) / det;
		wv[i] = (-e1[i] * d3u + e3[i] * d1u) / det;
	}
	frame->u_tile = length3(wu);
	frame->v_tile = length3(wv);
	if (frame->u_tile < 1.0f)
		frame->u_tile = DEFAULT_TILE_UNITS;
	if (frame->v_tile < 1.0f)
		frame->v_tile = DEFAULT_TILE_UNITS;
	to_unit_vector(wu, &frame->u_dir);
	to_unit_vector(wv, &frame->v_dir);
}

static int build_frame(gizmo_frame *frame)
{
	vms_vector centre, element_centre, inward;
	const builder_element *first;
	fix distance;
	int i, count = builder_tag_count();

	if (!count)
		return 0;
	first = builder_tag(0);
	if (first->segnum >= Num_segments)
		return 0;
	vm_vec_zero(&centre);
	for (i = 0; i < count; i++)
	{
		bld_element_center(builder_tag(i), &element_centre);
		vm_vec_add2(&centre, &element_centre);
	}
	vm_vec_scale2(&centre, 1, count);
	bld_side_normal(&Segments[first->segnum], first->sidenum, &inward);
	vm_vec_scale_add(&frame->origin, &centre, &inward, LIFT_FROM_FACE);
	uv_directions(first, frame);
	distance = vm_vec_dist(&ConsoleObject->pos, &frame->origin);
	frame->size = fixmul(distance, fl2f(DISTANCE_SCALE));
	if (frame->size < MIN_SIZE)
		frame->size = MIN_SIZE;
	if (frame->size > MAX_SIZE)
		frame->size = MAX_SIZE;
	return 1;
}

static void point_along(vms_vector *out, const gizmo_frame *frame, const vms_vector *dir, float along)
{
	*out = frame->origin;
	vm_vec_scale_add2(out, dir, fl2f(f2fl(frame->size) * along));
}

static void ring_point(vms_vector *out, const gizmo_frame *frame, int step)
{
	float angle = step * 2.0f * (float)M_PI / RING_STEPS;

	*out = frame->origin;
	vm_vec_scale_add2(out, &frame->u_dir, fl2f(f2fl(frame->size) * RING_RADIUS * cosf(angle)));
	vm_vec_scale_add2(out, &frame->v_dir, fl2f(f2fl(frame->size) * RING_RADIUS * sinf(angle)));
}

/* ------------------------------------------------------------------ screen */

static screen_point project(const vms_vector *world)
{
	screen_point out = {0, 0, 0};
	g3s_point p;

	if (g3_rotate_point(&p, world) & CC_BEHIND)
		return out;
	g3_project_point(&p);
	out.x = f2i(p.p3_sx);
	out.y = f2i(p.p3_sy);
	out.visible = 1;
	return out;
}

static float pixel_distance(screen_point a, int x, int y)
{
	return sqrtf((float)((a.x - x) * (a.x - x) + (a.y - y) * (a.y - y)));
}

static void consider(screen_point p, int part, int cx, int cy, float *best, int *winner)
{
	float dist;

	if (!p.visible)
		return;
	dist = pixel_distance(p, cx, cy);
	if (dist < *best)
	{
		*best = dist;
		*winner = part;
	}
}

static void pick_hover(const gizmo_frame *frame)
{
	int cx = grd_curcanv->cv_bitmap.bm_w / 2, cy = grd_curcanv->cv_bitmap.bm_h / 2;
	float best = grd_curcanv->cv_bitmap.bm_h * HIT_FRACTION;
	int winner = GIZMO_NONE, i, a;

	if (Dragging)
		return;
	for (a = 1; a <= ARROW_STEPS; a++)
	{
		vms_vector p;

		point_along(&p, frame, &frame->u_dir, (float)a / ARROW_STEPS);
		consider(project(&p), GIZMO_U, cx, cy, &best, &winner);
		point_along(&p, frame, &frame->v_dir, (float)a / ARROW_STEPS);
		consider(project(&p), GIZMO_V, cx, cy, &best, &winner);
	}
	for (i = 0; i < RING_STEPS; i++)
	{
		vms_vector p;

		ring_point(&p, frame, i);
		consider(project(&p), GIZMO_RING, cx, cy, &best, &winner);
	}
	consider(Origin_px, GIZMO_CENTRE, cx, cy, &best, &winner);
	Hover = winner;
}

/* ------------------------------------------------------------------ drawing */

static void line(const vms_vector *a, const vms_vector *b)
{
	g3s_point pa, pb;

	g3_rotate_point(&pa, a);
	g3_rotate_point(&pb, b);
	g3_draw_line(&pa, &pb);
}

static void draw_arrow(const gizmo_frame *frame, const vms_vector *dir, const vms_vector *across, int color, int part)
{
	vms_vector tip, back_a, back_b, base;
	float head = HEAD_LENGTH, width = HEAD_WIDTH;

	gr_setcolor(Hover == part || (Dragging && Grabbed == part) ? COLOR_HOT : color);
	point_along(&tip, frame, dir, 1.0f);
	line(&frame->origin, &tip);
	point_along(&base, frame, dir, 1.0f - head);
	back_a = base;
	back_b = base;
	vm_vec_scale_add2(&back_a, across, fl2f(f2fl(frame->size) * width));
	vm_vec_scale_add2(&back_b, across, fl2f(-f2fl(frame->size) * width));
	line(&tip, &back_a);
	line(&tip, &back_b);
	line(&back_a, &back_b);
}

static void draw_ring(const gizmo_frame *frame)
{
	vms_vector previous, current;
	int i;

	gr_setcolor(Hover == GIZMO_RING || (Dragging && Grabbed == GIZMO_RING) ? COLOR_HOT : COLOR_RING);
	ring_point(&previous, frame, 0);
	for (i = 1; i <= RING_STEPS; i++)
	{
		ring_point(&current, frame, i % RING_STEPS);
		line(&previous, &current);
		previous = current;
	}
}

static void draw_centre(const gizmo_frame *frame)
{
	vms_vector corner[4];
	float half = f2fl(frame->size) * CENTRE_HALF;
	static const int sign[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
	int k;

	gr_setcolor(Hover == GIZMO_CENTRE || (Dragging && (Grabbed == GIZMO_CENTRE || Grabbed == GIZMO_ZOOM)) ? COLOR_HOT : COLOR_CENTRE);
	for (k = 0; k < 4; k++)
	{
		corner[k] = frame->origin;
		vm_vec_scale_add2(&corner[k], &frame->u_dir, fl2f(sign[k][0] * half));
		vm_vec_scale_add2(&corner[k], &frame->v_dir, fl2f(sign[k][1] * half));
	}
	for (k = 0; k < 4; k++)
		line(&corner[k], &corner[(k + 1) % 4]);
}

int builder_gizmo_visible(void)
{
	return Builder.tool == BTOOL_PAINT && builder_tag_count() > 0;
}

/* Called inside the world render; also works out what the crosshair is on. */
void builder_gizmo_draw(void)
{
	vms_vector u_tip, v_tip;

	if (!builder_gizmo_visible() || !build_frame(&Frame))
	{
		Hover = GIZMO_NONE;
		return;
	}
	point_along(&u_tip, &Frame, &Frame.u_dir, 1.0f);
	point_along(&v_tip, &Frame, &Frame.v_dir, 1.0f);
	Origin_px = project(&Frame.origin);
	U_tip_px = project(&u_tip);
	V_tip_px = project(&v_tip);
	pick_hover(&Frame);
	draw_arrow(&Frame, &Frame.u_dir, &Frame.v_dir, COLOR_U, GIZMO_U);
	draw_arrow(&Frame, &Frame.v_dir, &Frame.u_dir, COLOR_V, GIZMO_V);
	draw_ring(&Frame);
	draw_centre(&Frame);
}

/* ------------------------------------------------------------------ dragging */

int builder_gizmo_hover(void)
{
	return Hover != GIZMO_NONE && !Dragging;
}

int builder_gizmo_dragging(void)
{
	return Dragging;
}

static int ctrl_held(void)
{
	return keyd_pressed[KEY_LCTRL] || keyd_pressed[KEY_RCTRL];
}

int builder_gizmo_begin(void)
{
	if (Hover == GIZMO_NONE || !builder_uv_drag_begin())
		return 0;
	Grabbed = Hover == GIZMO_CENTRE && ctrl_held() ? GIZMO_ZOOM : Hover;
	Dragging = 1;
	Pointer_x = (float)(grd_curcanv->cv_bitmap.bm_w / 2 - Origin_px.x);
	Pointer_y = (float)(grd_curcanv->cv_bitmap.bm_h / 2 - Origin_px.y);
	return 1;
}

/* Pixels along a handle's on-screen direction, as tiles of texture slide. */
static float slide_along(screen_point tip, int dx, int dy, float tile)
{
	float sx = (float)(tip.x - Origin_px.x), sy = (float)(tip.y - Origin_px.y);
	float length = sqrtf(sx * sx + sy * sy), along;

	if (!tip.visible || !Origin_px.visible || length < 1.0f)
		return 0;
	along = (dx * sx + dy * sy) / length;
	return -(along / length) * f2fl(Frame.size) / tile;
}

static void drag_free(int dx, int dy, float fine)
{
	float ux = (float)(U_tip_px.x - Origin_px.x), uy = (float)(U_tip_px.y - Origin_px.y);
	float vx = (float)(V_tip_px.x - Origin_px.x), vy = (float)(V_tip_px.y - Origin_px.y);
	float det = ux * vy - vx * uy, a, b;

	if (fabsf(det) < 1.0f)
		return;
	a = (dx * vy - vx * dy) / det;
	b = (ux * dy - dx * uy) / det;
	builder_uv_drag_slide(-a * f2fl(Frame.size) / Frame.u_tile * fine, -b * f2fl(Frame.size) / Frame.v_tile * fine);
}

static void drag_ring(int dx, int dy, float fine)
{
	float before = atan2f(Pointer_y, Pointer_x), after, turn;

	Pointer_x += dx;
	Pointer_y += dy;
	after = atan2f(Pointer_y, Pointer_x);
	turn = after - before;
	if (turn > (float)M_PI)
		turn -= 2.0f * (float)M_PI;
	if (turn < -(float)M_PI)
		turn += 2.0f * (float)M_PI;
	builder_uv_drag_turn(-turn * 180.0f / (float)M_PI * fine);
}

void builder_gizmo_move(int dx, int dy)
{
	float fine = (keyd_pressed[KEY_LSHIFT] || keyd_pressed[KEY_RSHIFT]) ? FINE_FACTOR : 1.0f;

	if (!Dragging)
		return;
	switch (Grabbed)
	{
		case GIZMO_U:		builder_uv_drag_slide(slide_along(U_tip_px, dx, dy, Frame.u_tile) * fine, 0); break;
		case GIZMO_V:		builder_uv_drag_slide(0, slide_along(V_tip_px, dx, dy, Frame.v_tile) * fine); break;
		case GIZMO_RING:	drag_ring(dx, dy, fine); break;
		case GIZMO_CENTRE:	drag_free(dx, dy, fine); break;
		case GIZMO_ZOOM:	builder_uv_drag_zoom(expf(dy * ZOOM_PER_PIXEL * fine)); break;
	}
}

void builder_gizmo_end(void)
{
	if (!Dragging)
		return;
	Dragging = 0;
	builder_uv_drag_end();
}
