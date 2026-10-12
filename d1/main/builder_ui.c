/*
 * SNG: level builder tool panel (Tab). A Nuklear panel docked to the right
 * of the live scene; the game is paused while it is open.
 */

#include <stdio.h>
#include <string.h>

/* must match the configuration nk_ui.c compiles Nuklear with */
#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#include "nuklear/nuklear.h"

#include "inferno.h"
#include "segment.h"
#include "textures.h"
#include "bm.h"
#include "piggy.h"
#include "game.h"
#include "event.h"
#include "mouse.h"
#include "nk_ui.h"
#include "config.h"
#include "builder_int.h"
#ifdef OGL
#include "ogl_init.h"
#endif

#define TOOLS_WIDTH_FRAC	0.22f
#define TEXTURES_WIDTH_FRAC	0.24f
#define ROW_SCALE		0.9f
#define SECTION_GAP		0.5f
#define PANEL_GAP		5.0f
#define TEXTURE_COLUMNS		5
#define TEXTURE_BROWSER_ROWS	14
#define FILTER_LEN		32

typedef struct builder_panel {
	int running;
	char filter[FILTER_LEN];
} builder_panel;

static const float Step_presets[] = {0.25f, 0.5f, 1.0f, 2.0f, 5.0f};
#define STEP_PRESET_COUNT ((int)(sizeof(Step_presets) / sizeof(Step_presets[0])))

/* ------------------------------------------------------------ textures */

#ifdef OGL
static ogl_texture *texture_gl(int texnum)
{
	bitmap_index index = Textures[texnum];
	grs_bitmap *bm = &GameBitmaps[index.index];

	PIGGY_PAGE_IN(index);
	if (!bm->gltexture || !bm->gltexture->handle)
		ogl_loadbmtexture_f(bm, GameCfg.TexFilt);
	return bm->gltexture;
}
#endif

static int texture_image(int texnum, struct nk_image *out)
{
#ifdef OGL
	ogl_texture *tex = texture_gl(texnum);

	if (!tex)
		return 0;
	*out = nk_subimage_id(tex->handle, tex->tw, tex->th, nk_rect(0, 0, tex->w, tex->h));
	return 1;
#else
	(void)texnum; (void)out;
	return 0;
#endif
}

void builder_draw_texture_thumb(int texnum, int x, int y, int size)
{
#ifdef OGL
	bitmap_index index = Textures[texnum];

	PIGGY_PAGE_IN(index);
	ogl_ubitmapm_cs(x, y, size, size, &GameBitmaps[index.index], -1, F1_0);
#else
	(void)texnum; (void)x; (void)y; (void)size;
#endif
}

/* ------------------------------------------------------------- widgets */

static int button(struct nk_context *ctx, const char *label)
{
	return nk_button_label(ctx, label);
}

/* Like choice_row() but the caller has already laid the row out. */
static int choice_row_inline(struct nk_context *ctx, const char *const *names, int count, int current)
{
	int i;

	for (i = 0; i < count; i++)
	{
		int on = current == i;

		if (nk_selectable_label(ctx, names[i], NK_TEXT_CENTERED, &on) && on)
			current = i;
	}
	return current;
}

/* One of N exclusive choices laid out in a row; returns the new value. */
static int choice_row(struct nk_context *ctx, float row, const char *const *names, int count, int current)
{
	int i;

	nk_layout_row_dynamic(ctx, row, count);
	for (i = 0; i < count; i++)
	{
		int on = current == i;

		if (nk_selectable_label(ctx, names[i], NK_TEXT_CENTERED, &on) && on)
			current = i;
	}
	return current;
}

static void caption(struct nk_context *ctx, float row, const char *text)
{
	nk_layout_row_dynamic(ctx, row, 1);
	nk_label_colored(ctx, text, NK_TEXT_LEFT, nk_rgb(255, 220, 90));
}

static void compact_style(struct nk_context *ctx)
{
	nk_style_push_vec2(ctx, &ctx->style.window.spacing, nk_vec2(PANEL_GAP, PANEL_GAP));
	nk_style_push_vec2(ctx, &ctx->style.window.padding, nk_vec2(PANEL_GAP * 2, PANEL_GAP));
	nk_style_push_vec2(ctx, &ctx->style.button.padding, nk_vec2(1, 1));
}

static void restore_style(struct nk_context *ctx)
{
	nk_style_pop_vec2(ctx);
	nk_style_pop_vec2(ctx);
	nk_style_pop_vec2(ctx);
}

/* ---------------------------------------------------------------- sections */

static void section_gap(struct nk_context *ctx, float row)
{
	nk_layout_row_dynamic(ctx, row * SECTION_GAP, 1);
	nk_spacing(ctx, 1);
}

static void slider_row(struct nk_context *ctx, float row, const char *name, float *value, float lo, float hi, float step)
{
	char label[24];

	snprintf(label, sizeof(label), "%s %g", name, *value);
	nk_layout_row_dynamic(ctx, row, 2);
	nk_label(ctx, label, NK_TEXT_LEFT);
	nk_slider_float(ctx, lo, value, hi, step);
}

static void section_cubes(struct nk_context *ctx, float row)
{
	caption(ctx, row, "Cubes");
	slider_row(ctx, row, "Depth", &Builder.build_depth, 1.0f, 200.0f, 1.0f);
	nk_layout_row_dynamic(ctx, row, 1);
	nk_checkbox_label(ctx, "Build straight", &Builder.axis_snap);
	nk_checkbox_label(ctx, "Cuts reach connected cubes", &Builder.cut_neighbours);
	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, "Build"))
		builder_extrude_selection();
	if (button(ctx, "Delete"))
		builder_delete_selection();
	if (button(ctx, "Untag"))
		builder_clear_tags();
}

static void step_presets(struct nk_context *ctx, float row)
{
	char label[16];
	int i;

	nk_layout_row_dynamic(ctx, row, STEP_PRESET_COUNT);
	for (i = 0; i < STEP_PRESET_COUNT; i++)
	{
		snprintf(label, sizeof(label), "%g", Step_presets[i]);
		if (button(ctx, label))
			Builder.nudge_step = Step_presets[i];
	}
}

static void axis_buttons(struct nk_context *ctx, float row)
{
	static const char *const labels[6] = {"X-", "X+", "Y-", "Y+", "Z-", "Z+"};
	int i;

	nk_layout_row_dynamic(ctx, row, 6);
	for (i = 0; i < 6; i++)
		if (button(ctx, labels[i]))
			builder_nudge_axis(i / 2, i % 2 ? 1 : -1);
}

static void section_move(struct nk_context *ctx, float row)
{
	caption(ctx, row, "Move");
	slider_row(ctx, row, "Step", &Builder.nudge_step, 0.05f, 10.0f, 0.05f);
	step_presets(ctx, row);
	nk_layout_row_dynamic(ctx, row, 2);
	if (button(ctx, "Push in"))
		builder_push(-1, 1);
	if (button(ctx, "Push out"))
		builder_push(1, 1);
	axis_buttons(ctx, row);
	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, "Snap"))
		builder_snap_selection();
	if (button(ctx, "Flat"))
		builder_flatten_selection();
	if (button(ctx, "Weld"))
		builder_weld_selection();
}

static void section_level(struct nk_context *ctx, float row, builder_panel *panel)
{
	int fullbright = Builder.fullbright;

	caption(ctx, row, "Level");
	nk_layout_row_dynamic(ctx, row, 1);
	nk_checkbox_label(ctx, "Fullbright", &fullbright);
	if (fullbright != Builder.fullbright)
		builder_set_fullbright(fullbright);
	nk_layout_row_dynamic(ctx, row, 2);
	if (button(ctx, "Relight"))
		builder_rebake();
	if (button(ctx, "Close"))
		panel->running = 0;
}

/* Always at the top. */
static void action_row(struct nk_context *ctx, float row)
{
	char undo_label[24], redo_label[24];

	snprintf(undo_label, sizeof(undo_label), "Undo %d", bld_undo_depth());
	snprintf(redo_label, sizeof(redo_label), "Redo %d", bld_redo_depth());
	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, undo_label))
		builder_undo();
	if (button(ctx, redo_label))
		builder_redo();
	if (button(ctx, "Save"))
		builder_save();
}

/* Tool and select mode live on the wheel, the status bar and keys 1-5 / V. */
static void build_panel(struct nk_context *ctx, void *userdata)
{
	builder_panel *panel = userdata;
	float row = nk_ui_row_height() * ROW_SCALE;

	compact_style(ctx);
	action_row(ctx, row);
	section_gap(ctx, row);
	section_cubes(ctx, row);
	section_gap(ctx, row);
	section_move(ctx, row);
	section_gap(ctx, row);
	section_level(ctx, row, panel);
	restore_style(ctx);
}

/* ---------------------------------------------------------- Textures panel */

static void current_texture_row(struct nk_context *ctx, float row)
{
	struct nk_image image;
	char label[48];
	float thumb = row * 1.6f;

	nk_layout_row_begin(ctx, NK_STATIC, thumb, 2);
	nk_layout_row_push(ctx, thumb);
	if (texture_image(builder_active_texture(), &image))
		nk_image(ctx, image);
	else
		nk_spacing(ctx, 1);
	nk_layout_row_push(ctx, nk_window_get_content_region(ctx).w - thumb - ctx->style.window.spacing.x);
	snprintf(label, sizeof(label), "#%d %s", builder_active_texture(), TmapInfo[builder_active_texture()].filename);
	nk_label(ctx, label, NK_TEXT_LEFT);
	nk_layout_row_end(ctx);
}

static void texture_alignment(struct nk_context *ctx, float row)
{
	if (!nk_tree_push(ctx, NK_TREE_NODE, "Tagged faces", NK_MAXIMIZED))
		return;
	nk_layout_row_dynamic(ctx, row, 1);
	nk_label(ctx, "Paint tool: drag the gizmo in the world", NK_TEXT_LEFT);
	nk_label(ctx, "red/green slide, blue turns, square free", NK_TEXT_LEFT);
	nk_label(ctx, "Shift fine, Ctrl+square resizes", NK_TEXT_LEFT);
	if (button(ctx, "Link mapping"))
		builder_uv_link();
	nk_layout_row_dynamic(ctx, row, 4);
	if (button(ctx, "Rot 90"))
		builder_uv_rotate();
	if (button(ctx, "Flip U"))
		builder_uv_flip(0);
	if (button(ctx, "Flip V"))
		builder_uv_flip(1);
	if (button(ctx, "Reset"))
		builder_uv_reset();
	nk_tree_pop(ctx);
}

static void texture_thumb_button(struct nk_context *ctx, int texnum)
{
	struct nk_image image;
	int selected = texnum == builder_active_texture();

	if (!texture_image(texnum, &image))
	{
		nk_spacing(ctx, 1);
		return;
	}
	if (selected)
	{
		nk_style_push_color(ctx, &ctx->style.button.border_color, nk_rgb(255, 220, 90));
		nk_style_push_float(ctx, &ctx->style.button.border, 3.0f);
	}
	if (nk_button_image(ctx, image))
		builder_set_active_texture(texnum);
	if (selected)
	{
		nk_style_pop_float(ctx);
		nk_style_pop_color(ctx);
	}
}

static void texture_browser(struct nk_context *ctx, float row, builder_panel *panel)
{
	float thumb = (nk_window_get_content_region(ctx).w - PANEL_GAP * 4) / TEXTURE_COLUMNS - PANEL_GAP * 2;
	int texnum, in_row = 0;

	nk_layout_row_dynamic(ctx, row, 1);
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->filter, sizeof(panel->filter), nk_filter_default);

	nk_layout_row_dynamic(ctx, (thumb + PANEL_GAP * 3) * TEXTURE_BROWSER_ROWS, 1);
	if (!nk_group_begin(ctx, "Textures", NK_WINDOW_BORDER))
		return;
	for (texnum = 0; texnum < NumTextures; texnum++)
	{
		if (panel->filter[0] && !nk_ui_filter_matches(TmapInfo[texnum].filename, panel->filter))
			continue;
		if (in_row == 0)
			nk_layout_row_dynamic(ctx, thumb, TEXTURE_COLUMNS);
		texture_thumb_button(ctx, texnum);
		in_row = (in_row + 1) % TEXTURE_COLUMNS;
	}
	nk_group_end(ctx);
}

/* Brush or Fill, what Fill covers, and Seamless: the paint tool's behaviour. */
static void section_paint(struct nk_context *ctx, float row)
{
	caption(ctx, row, "Paint tool (P)");
	Builder.paint_mode = choice_row(ctx, row, Builder_paint_names, BPAINT_COUNT, Builder.paint_mode);
	if (Builder.paint_mode == BPAINT_FILL)
		Builder.fill_scope = choice_row(ctx, row, Builder_fill_names, BFILL_COUNT, Builder.fill_scope);
	nk_layout_row_dynamic(ctx, row, 1);
	nk_checkbox_label(ctx, "Seamless (continue neighbour's mapping)", &Builder.seamless);
}

static void section_recent(struct nk_context *ctx, float row)
{
	float thumb = (nk_window_get_content_region(ctx).w - PANEL_GAP * 4) / TEXTURE_COLUMNS - PANEL_GAP * 2;
	int i;

	if (!builder_recent_count())
		return;
	caption(ctx, row, "Recent");
	for (i = 0; i < builder_recent_count(); i++)
	{
		if (i % TEXTURE_COLUMNS == 0)
			nk_layout_row_dynamic(ctx, thumb, TEXTURE_COLUMNS);
		texture_thumb_button(ctx, builder_recent(i));
	}
}

static void build_texture_panel(struct nk_context *ctx, void *userdata)
{
	builder_panel *panel = userdata;
	float row = nk_ui_row_height() * ROW_SCALE;

	compact_style(ctx);
	section_paint(ctx, row);
	nk_layout_row_dynamic(ctx, row, BLAYER_COUNT + 1);
	Builder.layer = choice_row_inline(ctx, Builder_layer_names, BLAYER_COUNT, Builder.layer);
	if (button(ctx, "No overlay"))
		builder_clear_overlay();
	if (Builder.layer == BLAYER_OVERLAY)
		current_texture_row(ctx, row);
	section_recent(ctx, row);

	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, "Face"))
		builder_paint_selection(0);
	if (button(ctx, "Cube"))
		builder_paint_selection(1);
	if (button(ctx, "Pick"))
		builder_pick_texture();
	texture_alignment(ctx, row);
	texture_browser(ctx, row, panel);
	restore_style(ctx);
}

static void run_panel(const char *title, int left, void (*build)(struct nk_context *, void *), builder_panel *panel)
{
	float width = left ? TEXTURES_WIDTH_FRAC : TOOLS_WIDTH_FRAC;

	panel->running = 1;
	stop_time();
	mouse_toggle_relative(0);
	mouse_touch_cursor_time();
	if (left)
		nk_ui_run_docked_panel_left(title, width, build, &panel->running, panel);
	else
		nk_ui_run_docked_panel(title, width, build, &panel->running, panel);
	mouse_toggle_relative(1);
	game_flush_inputs();
	start_time();
}

void builder_open_tools(void)
{
	static builder_panel panel;

	run_panel("Builder", 0, build_panel, &panel);
}

void builder_open_textures(void)
{
	static builder_panel panel;

	run_panel("Textures", 1, build_texture_panel, &panel);
}
