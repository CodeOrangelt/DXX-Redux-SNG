/*
 * SNG: level builder status bar -- a translucent strip along the bottom with
 * drawn vector icons for the tool, selection mode and tag linking, plus the
 * step, build depth, current texture and save state.
 */

#include <stdio.h>

#include "inferno.h"
#include "gr.h"
#include "gamefont.h"
#include "textures.h"
#include "bm.h"
#include "builder_int.h"
#ifdef OGL
#include "ogl_init.h"
#endif

#define BAR_FADE		12
#define CELL_FADE		16
#define BAR_HEIGHT_LINES	3.2f
#define ICON_FRACTION		0.70f
#define ICON_LINE_WIDTH		2.5f
#define HINT_FADE		20

#define COLOR_BAR		BM_XRGB(0, 0, 0)
#define COLOR_RULE		BM_XRGB(18, 14, 4)
#define COLOR_ACTIVE		BM_XRGB(31, 28, 12)
#define COLOR_IDLE		BM_XRGB(19, 19, 21)
#define COLOR_CELL		BM_XRGB(22, 17, 3)
#define COLOR_ADD		BM_XRGB(8, 31, 8)
#define COLOR_REMOVE		BM_XRGB(31, 6, 6)
#define COLOR_LABEL		BM_XRGB(31, 31, 18)
#define COLOR_VALUE		BM_XRGB(20, 20, 22)
#define COLOR_HINT		BM_XRGB(22, 22, 24)
#define COLOR_SAVED		BM_XRGB(8, 28, 8)
#define COLOR_UNSAVED		BM_XRGB(31, 18, 0)

typedef struct icon_box {
	int x, y, size;
} icon_box;

typedef struct bar_layout {
	int top, height, icon, gap, line;
	int x;		/* next free column, advancing left to right */
} bar_layout;

/* ---------------------------------------------------------- primitives */

static fix px(const icon_box *box, float unit, int origin)
{
	return fl2f(origin + unit * box->size);
}

static void icon_line(const icon_box *box, float x0, float y0, float x1, float y1)
{
	gr_uline(px(box, x0, box->x), px(box, y0, box->y), px(box, x1, box->x), px(box, y1, box->y));
}

static void icon_rect(const icon_box *box, float x0, float y0, float x1, float y1)
{
	icon_line(box, x0, y0, x1, y0);
	icon_line(box, x1, y0, x1, y1);
	icon_line(box, x1, y1, x0, y1);
	icon_line(box, x0, y1, x0, y0);
}

static void icon_fill(const icon_box *box, float x0, float y0, float x1, float y1)
{
	gr_urect(box->x + x0 * box->size, box->y + y0 * box->size, box->x + x1 * box->size, box->y + y1 * box->size);
}

static void icon_polygon(const icon_box *box, const float (*points)[2], int count)
{
	int i;

	for (i = 0; i < count; i++)
		icon_line(box, points[i][0], points[i][1], points[(i + 1) % count][0], points[(i + 1) % count][1]);
}

static void icon_arrow(const icon_box *box, float from, float to, float y)
{
	float head = to > from ? -0.12f : 0.12f;

	icon_line(box, from, y, to, y);
	icon_line(box, to + head, y - 0.11f, to, y);
	icon_line(box, to + head, y + 0.11f, to, y);
}

static void translucent_rect(int left, int top, int right, int bottom, int color, int fade)
{
	gr_settransblend(fade, GR_BLEND_NORMAL);
	gr_setcolor(color);
	gr_rect(left, top, right, bottom);
	gr_settransblend(GR_FADE_OFF, GR_BLEND_NORMAL);
}

/* --------------------------------------------------------------- icons */

static void icon_select(const icon_box *box)
{
	static const float cursor[][2] = {
		{0.30f, 0.14f}, {0.30f, 0.82f}, {0.46f, 0.66f}, {0.58f, 0.88f},
		{0.68f, 0.83f}, {0.56f, 0.61f}, {0.76f, 0.61f},
	};

	icon_polygon(box, cursor, sizeof(cursor) / sizeof(cursor[0]));
}

static void icon_cube(const icon_box *box)
{
	icon_rect(box, 0.10f, 0.40f, 0.52f, 0.82f);
	icon_rect(box, 0.26f, 0.22f, 0.68f, 0.64f);
	icon_line(box, 0.10f, 0.40f, 0.26f, 0.22f);
	icon_line(box, 0.52f, 0.40f, 0.68f, 0.22f);
	icon_line(box, 0.52f, 0.82f, 0.68f, 0.64f);
	icon_line(box, 0.10f, 0.82f, 0.26f, 0.64f);
}

static void icon_build(const icon_box *box, int active)
{
	icon_cube(box);
	if (active)
		gr_setcolor(COLOR_ADD);
	icon_line(box, 0.66f, 0.80f, 0.94f, 0.80f);
	icon_line(box, 0.80f, 0.66f, 0.80f, 0.94f);
}

static void icon_delete(const icon_box *box, int active)
{
	icon_cube(box);
	if (active)
		gr_setcolor(COLOR_REMOVE);
	icon_line(box, 0.68f, 0.68f, 0.92f, 0.92f);
	icon_line(box, 0.92f, 0.68f, 0.68f, 0.92f);
}

static void icon_paint(const icon_box *box)
{
	icon_rect(box, 0.14f, 0.16f, 0.72f, 0.40f);
	icon_line(box, 0.72f, 0.28f, 0.86f, 0.28f);
	icon_line(box, 0.86f, 0.28f, 0.86f, 0.52f);
	icon_line(box, 0.86f, 0.52f, 0.48f, 0.52f);
	icon_line(box, 0.48f, 0.52f, 0.48f, 0.66f);
	icon_fill(box, 0.42f, 0.66f, 0.54f, 0.90f);
}

static void icon_pick(const icon_box *box)
{
	icon_line(box, 0.18f, 0.86f, 0.26f, 0.78f);
	icon_line(box, 0.22f, 0.74f, 0.58f, 0.38f);
	icon_line(box, 0.30f, 0.82f, 0.66f, 0.46f);
	icon_line(box, 0.22f, 0.74f, 0.30f, 0.82f);
	icon_line(box, 0.52f, 0.32f, 0.72f, 0.52f);
	gr_disk(px(box, 0.74f, box->x), px(box, 0.28f, box->y), fl2f(box->size * 0.12f));
}

static void icon_face(const icon_box *box)
{
	icon_fill(box, 0.24f, 0.24f, 0.76f, 0.76f);
	icon_rect(box, 0.18f, 0.18f, 0.82f, 0.82f);
}

static void icon_edge(const icon_box *box, int color)
{
	gr_setcolor(COLOR_IDLE);
	icon_rect(box, 0.18f, 0.18f, 0.82f, 0.82f);
	gr_setcolor(color);
	icon_fill(box, 0.14f, 0.76f, 0.86f, 0.88f);
}

static void icon_vertex(const icon_box *box, int color)
{
	gr_setcolor(COLOR_IDLE);
	icon_rect(box, 0.18f, 0.18f, 0.82f, 0.82f);
	gr_setcolor(color);
	icon_fill(box, 0.08f, 0.70f, 0.30f, 0.92f);
}

static void icon_sync(const icon_box *box)
{
	icon_arrow(box, 0.16f, 0.84f, 0.34f);
	icon_arrow(box, 0.16f, 0.84f, 0.66f);
}

static void icon_mirror(const icon_box *box)
{
	float y;

	for (y = 0.10f; y < 0.90f; y += 0.20f)
		icon_line(box, 0.50f, y, 0.50f, y + 0.10f);
	icon_arrow(box, 0.40f, 0.10f, 0.50f);
	icon_arrow(box, 0.60f, 0.90f, 0.50f);
}

/* ---------------------------------------------------------------- cells */

static void draw_cell(bar_layout *bar, int active, void (*draw)(const icon_box *box, int color))
{
	icon_box box;
	int color = active ? COLOR_ACTIVE : COLOR_IDLE;

	box.x = bar->x;
	box.y = bar->top + (bar->height - bar->icon) / 2;
	box.size = bar->icon;
	if (active)
	{
		translucent_rect(box.x - 3, box.y - 3, box.x + box.size + 3, box.y + box.size + 3, COLOR_CELL, CELL_FADE);
		gr_setcolor(COLOR_ACTIVE);
		icon_rect(&box, -0.06f, -0.06f, 1.06f, 1.06f);
	}
	gr_setcolor(color);
	draw(&box, color);
	bar->x += bar->icon + bar->gap;
}

static void draw_tool_icon(const icon_box *box, int color, int tool)
{
	int active = color == COLOR_ACTIVE;

	switch (tool)
	{
		case BTOOL_SELECT:	icon_select(box); break;
		case BTOOL_BUILD:	icon_build(box, active); break;
		case BTOOL_DELETE:	icon_delete(box, active); break;
		case BTOOL_PAINT:	icon_paint(box); break;
		case BTOOL_PICK:	icon_pick(box); break;
	}
}

/* draw_cell() takes a single drawing callback; these bind the variant. */
static void tool_select(const icon_box *b, int c) { draw_tool_icon(b, c, BTOOL_SELECT); }
static void tool_build(const icon_box *b, int c)  { draw_tool_icon(b, c, BTOOL_BUILD); }
static void tool_delete(const icon_box *b, int c) { draw_tool_icon(b, c, BTOOL_DELETE); }
static void tool_paint(const icon_box *b, int c)  { draw_tool_icon(b, c, BTOOL_PAINT); }
static void tool_pick(const icon_box *b, int c)   { draw_tool_icon(b, c, BTOOL_PICK); }
static void mode_face(const icon_box *b, int c)   { (void)c; icon_face(b); }
static void mode_edge(const icon_box *b, int c)   { icon_edge(b, c); }
static void mode_vertex(const icon_box *b, int c) { icon_vertex(b, c); }
static void link_sync(const icon_box *b, int c)   { (void)c; icon_sync(b); }
static void link_mirror(const icon_box *b, int c) { (void)c; icon_mirror(b); }

static void (*const Tool_icons[BTOOL_COUNT])(const icon_box *, int) = {
	tool_select, tool_build, tool_delete, tool_paint, tool_pick,
};
static void (*const Mode_icons[BSEL_COUNT])(const icon_box *, int) = {
	mode_face, mode_edge, mode_vertex,
};

static void separator(bar_layout *bar)
{
	int x = bar->x + bar->gap / 2;

	gr_setcolor(COLOR_RULE);
	gr_uline(i2f(x), i2f(bar->top + bar->height / 5), i2f(x), i2f(bar->top + bar->height * 4 / 5));
	bar->x += bar->gap * 2;
}

/* Two stacked lines: a bright label and a dimmer value. */
static void text_block(bar_layout *bar, const char *label, int label_color, const char *value, int value_color)
{
	int y = bar->top + (bar->height - bar->line * 2) / 2;
	int w1, w2, h, aw;

	gr_get_string_size(label, &w1, &h, &aw);
	gr_get_string_size(value, &w2, &h, &aw);
	gr_set_fontcolor(label_color, -1);
	gr_printf(bar->x, y, "%s", label);
	gr_set_fontcolor(value_color, -1);
	gr_printf(bar->x, y + bar->line, "%s", value);
	bar->x += (w1 > w2 ? w1 : w2) + bar->gap;
}

/* --------------------------------------------------------------- groups */

static void group_tools(bar_layout *bar)
{
	int i;

	for (i = 0; i < BTOOL_COUNT; i++)
		draw_cell(bar, Builder.tool == i, Tool_icons[i]);
	text_block(bar, Builder_tool_names[Builder.tool], COLOR_LABEL, "1-5", COLOR_HINT);
}

static void group_modes(bar_layout *bar)
{
	int i;

	for (i = 0; i < BSEL_COUNT; i++)
		draw_cell(bar, Builder.sel_mode == i, Mode_icons[i]);
	text_block(bar, Builder_sel_mode_names[Builder.sel_mode], COLOR_LABEL, "V", COLOR_HINT);
}

static void group_tags(bar_layout *bar)
{
	char label[24], value[24];
	int mirror = Builder.link_mode == BLINK_MIRROR;

	draw_cell(bar, builder_tag_count() > 1, mirror ? link_mirror : link_sync);
	snprintf(label, sizeof(label), builder_tag_count() == 1 ? "1 TAG" : "%d TAGS", builder_tag_count());
	if (mirror)
		snprintf(value, sizeof(value), "MIRROR %s  M", Builder_axis_names[Builder.mirror_axis]);
	else
		snprintf(value, sizeof(value), "SYNC  M");
	text_block(bar, label, builder_tag_count() ? COLOR_ADD : COLOR_LABEL, value, COLOR_VALUE);
}

static void group_values(bar_layout *bar)
{
	char step[24], depth[24];

	snprintf(step, sizeof(step), "STEP %.2f", Builder.nudge_step);
	snprintf(depth, sizeof(depth), "DEPTH %.0f", Builder.build_depth);
	text_block(bar, step, COLOR_LABEL, depth, COLOR_VALUE);
}

/* Right-aligned: texture swatch, its name, and whether the level is saved. */
static void group_texture(bar_layout *bar, int right_edge)
{
	char label[32];
	const char *state = bld_is_dirty() ? "UNSAVED" : "SAVED";
	icon_box swatch;
	int w1, w2, h, aw, text_w, x;

	snprintf(label, sizeof(label), "#%d %s", Builder.texture, TmapInfo[Builder.texture].filename);
	gr_get_string_size(label, &w1, &h, &aw);
	gr_get_string_size(state, &w2, &h, &aw);
	text_w = w1 > w2 ? w1 : w2;
	x = right_edge - text_w - bar->gap - bar->icon;

	swatch.x = x;
	swatch.y = bar->top + (bar->height - bar->icon) / 2;
	swatch.size = bar->icon;
	builder_draw_texture_thumb(Builder.texture, swatch.x, swatch.y, swatch.size);
	gr_setcolor(COLOR_ACTIVE);
	icon_rect(&swatch, -0.04f, -0.04f, 1.04f, 1.04f);
	bar->x = x + bar->icon + bar->gap;
	text_block(bar, label, COLOR_LABEL, state, bld_is_dirty() ? COLOR_UNSAVED : COLOR_SAVED);
}

/* Controls reminder on its own lighter strip above the bar. */
static void hint_line(bar_layout *bar, int width)
{
	static const char hint[] = "TAB TOOLS   MMB SELECT   SHIFT+MMB TAG   WHEEL IN/OUT   RMB DELETE";
	int top = bar->top - bar->line - bar->gap;
	int w, h, aw;

	gr_get_string_size(hint, &w, &h, &aw);
	translucent_rect(0, top, (w + bar->gap * 4 < width ? w + bar->gap * 4 : width) - 1, bar->top - 1, COLOR_BAR, HINT_FADE);
	gr_set_fontcolor(COLOR_HINT, -1);
	gr_printf(bar->gap * 2, top + bar->gap / 2, "%s", hint);
}

void builder_draw_status_bar(void)
{
	int width = grd_curcanv->cv_bitmap.bm_w, height = grd_curcanv->cv_bitmap.bm_h;
	bar_layout bar;

	gr_set_curfont(GAME_FONT);
	bar.line = LINE_SPACING;
	bar.height = bar.line * BAR_HEIGHT_LINES;
	bar.top = height - bar.height;
	bar.icon = bar.height * ICON_FRACTION;
	bar.gap = bar.height / 6;
	bar.x = bar.gap * 2;

	translucent_rect(0, bar.top, width - 1, height - 1, COLOR_BAR, BAR_FADE);
	gr_setcolor(COLOR_RULE);
	gr_uline(i2f(0), i2f(bar.top), i2f(width - 1), i2f(bar.top));
#ifdef OGL
	glLineWidth(ICON_LINE_WIDTH);
#endif
	group_tools(&bar);
	separator(&bar);
	group_modes(&bar);
	separator(&bar);
	group_tags(&bar);
	separator(&bar);
	group_values(&bar);
	group_texture(&bar, width - bar.gap * 2);
#ifdef OGL
	glLineWidth(1.0f);
#endif
	hint_line(&bar, width);
}
