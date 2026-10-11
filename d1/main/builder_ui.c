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

#define PANEL_WIDTH_FRAC	0.30f
#define UV_STEP			(F1_0 / 8)
#define TEXTURE_COLUMNS		4
#define TEXTURE_BROWSER_ROWS	9
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

static void heading_label(struct nk_context *ctx, const char *text)
{
	nk_label_colored(ctx, text, NK_TEXT_LEFT, nk_rgb(255, 220, 90));
}

/* ------------------------------------------------------------ sections */

/* Multi-tag editing: how an axis nudge spreads across the tags. */
static void section_tags(struct nk_context *ctx, float row)
{
	int i;

	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Several tags move (M)");
	nk_layout_row_dynamic(ctx, row, BLINK_COUNT + 1);
	for (i = 0; i < BLINK_COUNT; i++)
		if (nk_option_label(ctx, Builder_link_names[i], Builder.link_mode == i))
			Builder.link_mode = i;
	if (button(ctx, "Clear tags"))
		builder_clear_tags();

	if (Builder.link_mode != BLINK_MIRROR)
		return;
	nk_layout_row_dynamic(ctx, row, 4);
	nk_label(ctx, "Mirror on", NK_TEXT_LEFT);
	for (i = 0; i < 3; i++)
		if (nk_option_label(ctx, Builder_axis_names[i], Builder.mirror_axis == i))
			Builder.mirror_axis = i;
}

static void section_tools(struct nk_context *ctx, float row)
{
	char selection_text[96];
	int i;

	if (!nk_tree_push(ctx, NK_TREE_TAB, "Tool & selection", NK_MAXIMIZED))
		return;
	nk_layout_row_dynamic(ctx, row, 3);
	for (i = 0; i < BTOOL_COUNT; i++)
		if (nk_option_label(ctx, Builder_tool_names[i], Builder.tool == i))
			Builder.tool = i;

	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Select by");
	nk_layout_row_dynamic(ctx, row, BSEL_COUNT);
	for (i = 0; i < BSEL_COUNT; i++)
		if (nk_option_label(ctx, Builder_sel_mode_names[i], Builder.sel_mode == i))
			Builder.sel_mode = i;

	builder_describe_selection(selection_text, sizeof(selection_text));
	nk_layout_row_dynamic(ctx, row, 1);
	nk_label_colored(ctx, selection_text, NK_TEXT_LEFT, nk_rgb(90, 255, 120));
	section_tags(ctx, row);
	nk_tree_pop(ctx);
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

static void section_geometry(struct nk_context *ctx, float row)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Geometry", NK_MAXIMIZED))
		return;
	nk_layout_row_dynamic(ctx, row, 1);
	nk_property_float(ctx, "Step (units)", 0.05f, &Builder.nudge_step, 40.0f, 0.25f, 0.05f);
	step_presets(ctx, row);

	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Nudge selection");
	nk_layout_row_dynamic(ctx, row, 2);
	if (button(ctx, "Face in"))
		builder_push(-1, 1);
	if (button(ctx, "Face out"))
		builder_push(1, 1);
	axis_buttons(ctx, row);

	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Corrections");
	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, "Snap"))
		builder_snap_selection();
	if (button(ctx, "Flatten"))
		builder_flatten_selection();
	if (button(ctx, "Weld"))
		builder_weld_selection();
	nk_tree_pop(ctx);
}

static void section_build(struct nk_context *ctx, float row)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Cubes", NK_MAXIMIZED))
		return;
	nk_layout_row_dynamic(ctx, row, 1);
	nk_property_float(ctx, "Build depth", 1.0f, &Builder.build_depth, 200.0f, 1.0f, 0.25f);
	nk_checkbox_label(ctx, "Build straight along X/Y/Z", &Builder.axis_snap);
	nk_layout_row_dynamic(ctx, row, 2);
	if (button(ctx, "Build"))
		builder_extrude_selection();
	if (button(ctx, "Delete"))
		builder_delete_selection();
	nk_tree_pop(ctx);
}

static void current_texture_row(struct nk_context *ctx, float row)
{
	struct nk_image image;
	char label[48];

	nk_layout_row_begin(ctx, NK_STATIC, row * 2, 2);
	nk_layout_row_push(ctx, row * 2);
	if (texture_image(Builder.texture, &image))
		nk_image(ctx, image);
	else
		nk_spacing(ctx, 1);
	nk_layout_row_push(ctx, nk_window_get_content_region(ctx).w - row * 2 - ctx->style.window.spacing.x);
	snprintf(label, sizeof(label), "#%d  %s", Builder.texture, TmapInfo[Builder.texture].filename);
	nk_label(ctx, label, NK_TEXT_LEFT);
	nk_layout_row_end(ctx);
}

static void texture_alignment(struct nk_context *ctx, float row)
{
	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Align on selected face");
	nk_layout_row_dynamic(ctx, row, 4);
	if (button(ctx, "U-"))
		builder_uv_shift(-UV_STEP, 0);
	if (button(ctx, "U+"))
		builder_uv_shift(UV_STEP, 0);
	if (button(ctx, "V-"))
		builder_uv_shift(0, -UV_STEP);
	if (button(ctx, "V+"))
		builder_uv_shift(0, UV_STEP);
	if (button(ctx, "Rotate"))
		builder_uv_rotate();
	if (button(ctx, "Flip U"))
		builder_uv_flip(0);
	if (button(ctx, "Flip V"))
		builder_uv_flip(1);
	if (button(ctx, "Reset"))
		builder_uv_reset();
}

static void texture_thumb_button(struct nk_context *ctx, int texnum)
{
	struct nk_image image;
	int selected = texnum == Builder.texture;

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
		Builder.texture = texnum;
	if (selected)
	{
		nk_style_pop_float(ctx);
		nk_style_pop_color(ctx);
	}
}

static void texture_browser(struct nk_context *ctx, float row, builder_panel *panel)
{
	float thumb = (nk_window_get_content_region(ctx).w - row) / TEXTURE_COLUMNS;
	int texnum, in_row = 0;

	nk_layout_row_dynamic(ctx, row, 1);
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, panel->filter, sizeof(panel->filter), nk_filter_default);

	nk_layout_row_dynamic(ctx, row * TEXTURE_BROWSER_ROWS, 1);
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

static void section_texture(struct nk_context *ctx, float row, builder_panel *panel)
{
	if (!nk_tree_push(ctx, NK_TREE_TAB, "Texture", NK_MAXIMIZED))
		return;
	current_texture_row(ctx, row);
	nk_layout_row_dynamic(ctx, row, 1);
	heading_label(ctx, "Paint selection");
	nk_layout_row_dynamic(ctx, row, 3);
	if (button(ctx, "Face"))
		builder_paint_selection(0);
	if (button(ctx, "Cube"))
		builder_paint_selection(1);
	if (button(ctx, "Pick"))
		builder_pick_texture();
	texture_alignment(ctx, row);
	texture_browser(ctx, row, panel);
	nk_tree_pop(ctx);
}

static void section_level(struct nk_context *ctx, float row, builder_panel *panel)
{
	char undo_label[24], redo_label[24];
	int fullbright = Builder.fullbright;

	if (!nk_tree_push(ctx, NK_TREE_TAB, "Level", NK_MAXIMIZED))
		return;
	nk_layout_row_dynamic(ctx, row, 2);
	nk_checkbox_label(ctx, "Fullbright", &fullbright);
	if (fullbright != Builder.fullbright)
		builder_set_fullbright(fullbright);
	if (button(ctx, "Rebuild lighting"))
		builder_rebake();

	snprintf(undo_label, sizeof(undo_label), "Undo (%d)", bld_undo_depth());
	snprintf(redo_label, sizeof(redo_label), "Redo (%d)", bld_redo_depth());
	nk_layout_row_dynamic(ctx, row, 2);
	if (button(ctx, undo_label))
		builder_undo();
	if (button(ctx, redo_label))
		builder_redo();
	if (button(ctx, bld_is_dirty() ? "Save *" : "Save"))
		builder_save();
	if (button(ctx, "Close (Tab)"))
		panel->running = 0;
	nk_tree_pop(ctx);
}

static void build_panel(struct nk_context *ctx, void *userdata)
{
	builder_panel *panel = userdata;
	float row = nk_ui_row_height();

	section_tools(ctx, row);
	section_geometry(ctx, row);
	section_build(ctx, row);
	section_texture(ctx, row, panel);
	section_level(ctx, row, panel);
}

void builder_open_tools(void)
{
	static builder_panel panel;

	panel.running = 1;
	stop_time();
	mouse_toggle_relative(0);
	mouse_touch_cursor_time();
	nk_ui_run_docked_panel("Builder Tools", PANEL_WIDTH_FRAC, build_panel, &panel.running, &panel);
	mouse_toggle_relative(1);
	game_flush_inputs();
	start_time();
}
