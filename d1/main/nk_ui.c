/*
 * Nuklear-based replacement UI for the netgame "Advanced options" and
 * "Hosting setup" screens, and everything reachable from them (Objects
 * Allowed, Start Mission With Weapons, Static Weapons) -- see nk_ui.h. This
 * is a new, separate UI shell that coexists with the legacy newmenu system
 * (still used for non-OGL builds, see the #ifdef OGL split in
 * net_udp_setup_game()) rather than replacing it outright.
 *
 * OpenGL-only: draws into the same GL context the OGL video backend
 * (d1/arch/ogl/gr.c) already owns.
 *
 * This project targets SDL 1.2 (see d1/vcpkg.json), not SDL2 -- despite
 * some SDL2-shaped #if SDL_VERSION_ATLEAST(2,0,0) branches elsewhere in the
 * OGL backend, those never compile in this build. Nuklear's own upstream
 * reference backend (demo/sdl_opengl2) is SDL2-only (SDL_Window, unified
 * SDL_TEXTINPUT/SDL_MOUSEWHEEL events, clipboard API), so rather than vendor
 * that, this file has its own small SDL1.2 + fixed-function-GL2 backend:
 * the GL2 draw-command rendering and font atlas upload are copied from
 * Nuklear's reference implementation (that part has no SDL dependency at
 * all), and the input/window plumbing is written against SDL1.2's actual
 * API (SDL_GetVideoSurface, button-event mouse wheel, keysym.unicode text
 * input, no clipboard support).
 */

#include <SDL.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// GLEW must be the first GL header included in the translation unit, or its
// own include guard rejects a plain <GL/gl.h> included ahead of it.
#include <GL/glew.h>
#if defined(__APPLE__) && defined(__MACH__)
#include <OpenGL/gl.h>
#else
#ifdef OGLES
#include <GLES/gl.h>
#else
#include <GL/gl.h>
#endif
#endif

#define NK_INCLUDE_FIXED_TYPES
#define NK_INCLUDE_STANDARD_IO
#define NK_INCLUDE_STANDARD_VARARGS
#define NK_INCLUDE_DEFAULT_ALLOCATOR
#define NK_INCLUDE_VERTEX_BUFFER_OUTPUT
#define NK_INCLUDE_FONT_BAKING
#define NK_INCLUDE_DEFAULT_FONT
#define NK_IMPLEMENTATION
#include "nuklear/nuklear.h"

// Panels sized by fixed fractions clipped long labels. These wrappers record
// how wide each label wants to be as it is drawn; the next frame's panel is
// sized from that. See nk_ui_note().
static void nk_ui_note(const char *text, float extra_rows);
#define NK_UI_TOGGLE_EXTRA_ROWS 2.0f
#define NK_UI_LABEL_EXTRA_ROWS 0.5f
#define NK_UI_PROPERTY_EXTRA_ROWS 6.0f
#define nk_checkbox_label(ctx, label, active) (nk_ui_note((label), NK_UI_TOGGLE_EXTRA_ROWS), nk_checkbox_label((ctx), (label), (active)))
#define nk_option_label(ctx, label, active) (nk_ui_note((label), NK_UI_TOGGLE_EXTRA_ROWS), nk_option_label((ctx), (label), (active)))
#define nk_button_label(ctx, label) (nk_ui_note((label), NK_UI_TOGGLE_EXTRA_ROWS), nk_button_label((ctx), (label)))
#define nk_label(ctx, text, align) (nk_ui_note((text), NK_UI_LABEL_EXTRA_ROWS), nk_label((ctx), (text), (align)))
#define nk_property_int(ctx, name, min, val, max, step, inc) (nk_ui_note((name), NK_UI_PROPERTY_EXTRA_ROWS), nk_property_int((ctx), (name), (min), (val), (max), (step), (inc)))

#include "pstypes.h"
#include "game.h"
#include "multi.h"
#include "net_udp.h"
#include "player.h"
#include "gameseq.h"
#include "mission.h"
#include "text.h"
#include "playsave.h"
#include "weapon.h"
#include "laser.h"
#include "config.h"
#include "gr.h"
#include "ogl_init.h"
#include "mouse.h"
#include "event.h"
#include "window.h"
#include "newmenu.h"
#include "gamefont.h"
#include "nk_ui.h"
#include "shotimg.h"
#include "vers_id.h"
#include "physfsx.h"

// Declared in mouse.h. Without touching this, the cursor-autohide timer in
// mouse_cursor_autohide() (driven by event_poll(), which we bypass with our
// own SDL_PollEvent loop) goes stale while a Nuklear modal is open -- as
// soon as control returns to the legacy menu system afterwards, its next
// event_poll() sees a long-stale timestamp and immediately hides the cursor
// again. We deliberately do NOT call the game's full mouse_button_handler /
// mouse_motion_handler here instead: those call event_send(), which would
// dispatch synthetic input to whatever legacy window is still on the stack
// underneath this modal.

extern void mouse_cursor_autohide(void);
extern void net_udp_init(void);
extern void net_udp_close(void);
extern int net_udp_start_game(void);

#define NK_UI_WINDOW_ALPHA 255
#define NK_UI_HEADER_ALPHA 255
// Metrics follow the baked Descent font and screen size -- see nk_ui_sync_fonts().
static int s_row_h = 26;
static int s_chrome_h = 70;
#define NK_UI_ROW_HEIGHT s_row_h
#define NK_UI_MENU_CHROME_HEIGHT s_chrome_h
#define NK_UI_MENU_MAX_HEIGHT_FRAC 0.9f
#define NK_UI_MENU_WIDTH_FRAC 0.42f
#define NK_UI_TABLE_WIDTH_FRAC 0.8f
#define NK_UI_MENU_MIN_WIDTH_ROWS 14
#define NK_UI_DIM_ALPHA 0.6f

// One accent family per entry: the base accent, a lit version for hover and
// headings, a sunk version for borders and pressed widgets, and the panel
// tint that the window, header and shadowed widgets are built from. Picked
// by the player; see nk_ui_set_menu_color().
static const struct
{
	const char *name;
	struct nk_color accent, bright, dim, tint;
} s_menu_colors[] = {
	{ "Blue",   { 64, 140, 232, 255}, {120, 186, 255, 255}, { 34,  86, 160, 255}, { 12,  15,  22, 255} },
	{ "Teal",   { 40, 176, 176, 255}, { 96, 226, 226, 255}, { 20, 108, 110, 255}, {  9,  20,  22, 255} },
	{ "Green",  { 72, 184,  88, 255}, {130, 232, 142, 255}, { 36, 112,  48, 255}, { 11,  20,  13, 255} },
	{ "Amber",  {226, 150,  40, 255}, {255, 196,  96, 255}, {150,  92,  18, 255}, { 22,  17,  10, 255} },
	{ "Red",    {214,  70,  62, 255}, {255, 126, 112, 255}, {140,  36,  32, 255} ,{ 22,  11,  11, 255} },
	{ "Purple", {150, 104, 232, 255}, {192, 158, 255, 255}, { 88,  56, 154, 255}, { 16,  13,  24, 255} },
	{ "Pink",   {222,  92, 168, 255}, {255, 146, 206, 255}, {146,  44, 108, 255}, { 22,  11,  18, 255} },
	{ "Steel",  {150, 166, 190, 255}, {206, 218, 236, 255}, { 84,  96, 116, 255}, { 15,  17,  20, 255} }
};

#define NK_UI_MENU_COLOR_COUNT ((int)(sizeof(s_menu_colors) / sizeof(s_menu_colors[0])))

static int s_menu_color = 0;

#define NK_UI_ACCENT (s_menu_colors[s_menu_color].accent)
#define NK_UI_ACCENT_BRIGHT (s_menu_colors[s_menu_color].bright)
#define NK_UI_ACCENT_DIM (s_menu_colors[s_menu_color].dim)
#define NK_UI_TITLE_TEXT (s_menu_colors[s_menu_color].bright)

// Mixes the accent tint into a neutral grey so every panel surface shifts
// with the chosen colour instead of staying blue.
// Blends `a` toward `b`; 0 is all `a`, 256 is all `b`.
static struct nk_color nk_ui_mix(struct nk_color a, struct nk_color b, int t)
{
	struct nk_color out;

	out.r = (nk_byte)((a.r * (256 - t) + b.r * t) >> 8);
	out.g = (nk_byte)((a.g * (256 - t) + b.g * t) >> 8);
	out.b = (nk_byte)((a.b * (256 - t) + b.b * t) >> 8);
	out.a = 255;
	return out;
}

static struct nk_color nk_ui_shade(int level)
{
	struct nk_color tint = s_menu_colors[s_menu_color].tint;

	return nk_rgba(min(255, tint.r + level), min(255, tint.g + level), min(255, tint.b + level), 255);
}

static struct nk_context s_ctx_storage;
static struct nk_context *s_ctx = NULL;
static int s_initialized = 0;

#define NK_UI_MAX_PANELS 16

// The panels built into the frame that has not been presented yet.
static const void *s_frame_panels[NK_UI_MAX_PANELS];
static int s_frame_panel_count;

// Presents the pending frame when `id` is already in it. A handler can run
// an event loop of its own (a message box, the netgame setup), which redraws
// panels before the frame they were built into was presented, and Nuklear
// aborts when one is built into the same frame twice.
static void nk_ui_claim_panel(const void *id)
{
	int i;

	for (i = 0; i < s_frame_panel_count; i++)
		if (s_frame_panels[i] == id)
		{
			nk_ui_flush();
			break;
		}
	if (s_frame_panel_count < NK_UI_MAX_PANELS)
		s_frame_panels[s_frame_panel_count++] = id;
}

struct nk_ui_vertex
{
	float position[2];
	float uv[2];
	nk_byte col[4];
};

static struct
{
	struct nk_buffer cmds;
	struct nk_draw_null_texture tex_null;
	GLuint font_tex;
} s_device;

static struct nk_font_atlas s_atlas;

// Redraws whatever the game window stack shows (menu backdrop, in-game view)
// so the translucent Nuklear panels have something live to blend over.
// Nuklear frees every window that was not begun since the last nk_clear(),
// and nk_clear() runs at the end of each render. Rendering once per panel
// therefore destroyed and rebuilt every other panel each frame, losing
// scroll positions and collapsed-section state. So panels stacked in one
// frame are all built first and only the front one renders, giving the
// whole stack a single nk_clear().
static int s_drawing_underneath = 0;

// Set once any panel has been built but not yet presented. Panels built in
// one pass must all reach the same nk_clear(), so presenting is a separate
// step: the window system flushes through nk_ui_flush() after its draw loop,
// and a modal screen flushes its own frame.
static int s_panels_pending = 0;

static void nk_ui_draw_underlying_windows(void)
{
	d_event draw_event;
	window *wind;

	draw_event.type = EVENT_WINDOW_DRAW;
	s_drawing_underneath = 1;
	for (wind = window_get_first(); wind; wind = window_get_next(wind))
	{
		if (window_is_visible(wind))
			window_send_event(wind, &draw_event);
	}
	s_drawing_underneath = 0;
}

static void nk_ui_upload_atlas(const void *image, int width, int height)
{
	glGenTextures(1, &s_device.font_tex);
	glBindTexture(GL_TEXTURE_2D, s_device.font_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0,
		GL_RGBA, GL_UNSIGNED_BYTE, image);
}

// Draws the accumulated Nuklear draw commands via fixed-function GL2
// (vertex arrays, no shaders) -- ported from Nuklear's own reference
// SDL2+GL2 backend (demo/sdl_opengl2/nuklear_sdl_gl2.h), which has no SDL
// dependency in this half.
static int s_dim_next = 0;

// Darkens everything drawn so far -- the game, or a parent panel -- so the
// panel about to render reads as the active one.
static void nk_ui_draw_dim(int width, int height)
{
	glDisable(GL_TEXTURE_2D);
	glColor4f(0.0f, 0.0f, 0.0f, NK_UI_DIM_ALPHA);
	glBegin(GL_QUADS);
		glVertex2f(0.0f, 0.0f);
		glVertex2f((float)width, 0.0f);
		glVertex2f((float)width, (float)height);
		glVertex2f(0.0f, (float)height);
	glEnd();
	glEnable(GL_TEXTURE_2D);
}

static void nk_ui_render(enum nk_anti_aliasing aa, int width, int height)
{
	mouse_touch_cursor_time();
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT | GL_VIEWPORT_BIT);
	glPushClientAttrib(GL_CLIENT_VERTEX_ARRAY_BIT);
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_SCISSOR_TEST);
	glEnable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glViewport(0, 0, (GLsizei)width, (GLsizei)height);
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	if (s_dim_next)
		nk_ui_draw_dim(width, height);
	s_dim_next = 0;

	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	{
		GLsizei vs = sizeof(struct nk_ui_vertex);
		size_t vp = offsetof(struct nk_ui_vertex, position);
		size_t vt = offsetof(struct nk_ui_vertex, uv);
		size_t vc = offsetof(struct nk_ui_vertex, col);

		const struct nk_draw_command *cmd;
		const nk_draw_index *offset = NULL;
		struct nk_buffer vbuf, ebuf;

		struct nk_convert_config config;
		static const struct nk_draw_vertex_layout_element vertex_layout[] = {
			{NK_VERTEX_POSITION, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_ui_vertex, position)},
			{NK_VERTEX_TEXCOORD, NK_FORMAT_FLOAT, NK_OFFSETOF(struct nk_ui_vertex, uv)},
			{NK_VERTEX_COLOR, NK_FORMAT_R8G8B8A8, NK_OFFSETOF(struct nk_ui_vertex, col)},
			{NK_VERTEX_LAYOUT_END}
		};
		memset(&config, 0, sizeof(config));
		config.vertex_layout = vertex_layout;
		config.vertex_size = sizeof(struct nk_ui_vertex);
		config.vertex_alignment = NK_ALIGNOF(struct nk_ui_vertex);
		config.tex_null = s_device.tex_null;
		config.circle_segment_count = 22;
		config.curve_segment_count = 22;
		config.arc_segment_count = 22;
		config.global_alpha = 1.0f;
		config.shape_AA = aa;
		config.line_AA = aa;

		nk_buffer_init_default(&vbuf);
		nk_buffer_init_default(&ebuf);
		nk_convert(s_ctx, &s_device.cmds, &vbuf, &ebuf, &config);

		{
			const void *vertices = nk_buffer_memory_const(&vbuf);
			glVertexPointer(2, GL_FLOAT, vs, (const void *)((const nk_byte *)vertices + vp));
			glTexCoordPointer(2, GL_FLOAT, vs, (const void *)((const nk_byte *)vertices + vt));
			glColorPointer(4, GL_UNSIGNED_BYTE, vs, (const void *)((const nk_byte *)vertices + vc));
		}

		offset = (const nk_draw_index *)nk_buffer_memory_const(&ebuf);
		nk_draw_foreach(cmd, s_ctx, &s_device.cmds)
		{
			if (!cmd->elem_count) continue;
			glBindTexture(GL_TEXTURE_2D, (GLuint)cmd->texture.id);
			glScissor(
				(GLint)cmd->clip_rect.x,
				(GLint)(height - (GLint)(cmd->clip_rect.y + cmd->clip_rect.h)),
				(GLint)cmd->clip_rect.w,
				(GLint)cmd->clip_rect.h);
			glDrawElements(GL_TRIANGLES, (GLsizei)cmd->elem_count, GL_UNSIGNED_SHORT, offset);
			offset += cmd->elem_count;
		}
		nk_clear(s_ctx);
		nk_buffer_clear(&s_device.cmds);
		nk_buffer_free(&vbuf);
		nk_buffer_free(&ebuf);
	}

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopClientAttrib();
	glPopAttrib();
}

// Set on an Escape keydown, consumed and cleared by nk_ui_frame() after
// input handling each frame: Nuklear's own NK_KEY_TEXT_RESET_MODE binding
// for Escape only affects an active text-edit widget, so it doesn't give us
// a general "close this screen" behavior on its own -- nk_ui_frame() treats
// this the same as clicking Back/Cancel.
static int s_escape_pressed = 0;

// Same idea for Enter (accepts the modal screen) and for scrolling, which the
// modal frame applies itself: Nuklear's own wheel handling didn't scroll
// these tall panels reliably. Wheel steps only apply if Nuklear didn't
// already scroll; arrow keys always do.
static int s_enter_pressed = 0;
static int s_wheel_steps = 0;
static int s_arrow_steps = 0;

// Claims this frame's arrow steps for a list, leaving none for the scroller.
static int nk_ui_take_arrow_steps(void)
{
	int steps = s_arrow_steps;

	s_arrow_steps = 0;
	return steps;
}

// Moves `selected` by the arrow steps and keeps it inside [0, count).
static void nk_ui_step_selection(int *selected, int count)
{
	if (count < 1)
		return;
	*selected += nk_ui_take_arrow_steps();
	if (*selected < 0)
		*selected = 0;
	if (*selected >= count)
		*selected = count - 1;
}
#define NK_UI_SCROLL_ROWS_PER_STEP 3

// SDL1.2 input bridge: keyboard (with keysym.unicode for text entry, already
// enabled globally via SDL_EnableUNICODE(1) in arch/sdl/key.c), mouse
// motion/buttons, and wheel-as-buttons (SDL1.2 has no separate wheel event).
// No clipboard support -- SDL1.2 has no clipboard text API to bridge.
static void nk_ui_handle_event(SDL_Event *evt)
{
	struct nk_context *ctx = s_ctx;
	int ctrl_down = SDL_GetModState() & KMOD_CTRL;

	switch (evt->type)
	{
		case SDL_KEYUP:
		case SDL_KEYDOWN:
		{
			int down = evt->type == SDL_KEYDOWN;
			switch (evt->key.keysym.sym)
			{
				case SDLK_LALT: case SDLK_RALT: nk_input_key(ctx, NK_KEY_ALT, down); break;
				case SDLK_LSHIFT: case SDLK_RSHIFT: nk_input_key(ctx, NK_KEY_SHIFT, down); break;
				case SDLK_DELETE: nk_input_key(ctx, NK_KEY_DEL, down); break;
				case SDLK_KP_ENTER: case SDLK_RETURN:
						nk_input_key(ctx, NK_KEY_ENTER, down);
						if (down)
							s_enter_pressed = 1;
						break;
				case SDLK_TAB: nk_input_key(ctx, NK_KEY_TAB, down); break;
				case SDLK_BACKSPACE: nk_input_key(ctx, NK_KEY_BACKSPACE, down); break;
				case SDLK_HOME: nk_input_key(ctx, NK_KEY_TEXT_START, down); nk_input_key(ctx, NK_KEY_SCROLL_START, down); break;
				case SDLK_END: nk_input_key(ctx, NK_KEY_TEXT_END, down); nk_input_key(ctx, NK_KEY_SCROLL_END, down); break;
				case SDLK_PAGEDOWN: nk_input_key(ctx, NK_KEY_SCROLL_DOWN, down); break;
				case SDLK_PAGEUP: nk_input_key(ctx, NK_KEY_SCROLL_UP, down); break;
				case SDLK_z: nk_input_key(ctx, NK_KEY_TEXT_UNDO, down && ctrl_down); break;
				case SDLK_r: nk_input_key(ctx, NK_KEY_TEXT_REDO, down && ctrl_down); break;
				case SDLK_c: nk_input_key(ctx, NK_KEY_COPY, down && ctrl_down); break;
				case SDLK_v: nk_input_key(ctx, NK_KEY_PASTE, down && ctrl_down); break;
				case SDLK_x: nk_input_key(ctx, NK_KEY_CUT, down && ctrl_down); break;
				case SDLK_b: nk_input_key(ctx, NK_KEY_TEXT_LINE_START, down && ctrl_down); break;
				case SDLK_e: nk_input_key(ctx, NK_KEY_TEXT_LINE_END, down && ctrl_down); break;
				case SDLK_UP:
						nk_input_key(ctx, NK_KEY_UP, down);
						if (down)
							s_arrow_steps--;
						break;
				case SDLK_DOWN:
						nk_input_key(ctx, NK_KEY_DOWN, down);
						if (down)
							s_arrow_steps++;
						break;
				case SDLK_ESCAPE:
					nk_input_key(ctx, NK_KEY_TEXT_RESET_MODE, down);
					if (down)
						s_escape_pressed = 1;
					break;
				case SDLK_a: if (ctrl_down) nk_input_key(ctx, NK_KEY_TEXT_SELECT_ALL, down); break;
				case SDLK_LEFT:
					if (ctrl_down) nk_input_key(ctx, NK_KEY_TEXT_WORD_LEFT, down);
					else nk_input_key(ctx, NK_KEY_LEFT, down);
					break;
				case SDLK_RIGHT:
					if (ctrl_down) nk_input_key(ctx, NK_KEY_TEXT_WORD_RIGHT, down);
					else nk_input_key(ctx, NK_KEY_RIGHT, down);
					break;
				default: break;
			}
			// Printable text entry -- SDL1.2 populates keysym.unicode when
			// SDL_EnableUNICODE(1) is active (already set globally elsewhere).
			if (down && evt->key.keysym.unicode >= 32 && evt->key.keysym.unicode < 0xF000)
			{
				nk_glyph glyph;
				memset(glyph, 0, sizeof(glyph));
				glyph[0] = (char)(evt->key.keysym.unicode & 0xFF);
				nk_input_glyph(ctx, glyph);
			}
			break;
		}

		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEBUTTONDOWN:
		{
			int down = evt->type == SDL_MOUSEBUTTONDOWN;
			const int x = evt->button.x, y = evt->button.y;
			switch (evt->button.button)
			{
				case SDL_BUTTON_LEFT: nk_input_button(ctx, NK_BUTTON_LEFT, x, y, down); break;
				case SDL_BUTTON_MIDDLE: nk_input_button(ctx, NK_BUTTON_MIDDLE, x, y, down); break;
				case SDL_BUTTON_RIGHT: nk_input_button(ctx, NK_BUTTON_RIGHT, x, y, down); break;
				case SDL_BUTTON_WHEELUP:
						if (down)
						{
							nk_input_scroll(ctx, nk_vec2(0, 1));
							s_wheel_steps--;
						}
						break;
				case SDL_BUTTON_WHEELDOWN:
						if (down)
						{
							nk_input_scroll(ctx, nk_vec2(0, -1));
							s_wheel_steps++;
						}
						break;
				default: break;
			}
			mouse_touch_cursor_time();
			break;
		}

		case SDL_MOUSEMOTION:
			nk_input_motion(ctx, evt->motion.x, evt->motion.y);
			mouse_touch_cursor_time();
			break;

		default:
			break;
	}
}

// nk_bool is `int` (or C99 `bool`) depending on toolchain -- never the same
// size as the `ubyte`/`short` fields on Netgame. Binding nk_checkbox_label
// straight to &Netgame.SomeField via a cast would let it write past the
// field into whatever's next in the struct. Always go through a same-sized
// nk_bool temporary and copy the result back explicitly instead.
static void nk_ui_checkbox_ubyte(struct nk_context *ctx, const char *label, ubyte *field)
{
	nk_bool val = *field ? nk_true : nk_false;
	nk_checkbox_label(ctx, label, &val);
	*field = val ? 1 : 0;
}

static void nk_ui_checkbox_short(struct nk_context *ctx, const char *label, short *field)
{
	nk_bool val = *field ? nk_true : nk_false;
	nk_checkbox_label(ctx, label, &val);
	*field = val ? 1 : 0;
}

// ==============================
// Descent fonts: the game's own bitmap fonts, re-baked into an RGBA atlas
// and exposed to Nuklear as user fonts, so panels read as Descent menus.
// ==============================

#define NK_UI_FONT_ATLAS_WIDTH 1024
#define NK_UI_FONT_GAP 1
#define NK_UI_PALETTE_BYTES 768
#define NK_UI_FONT_CHARS 256
#define NK_UI_HEAD_PX_DIVISOR 36
#define NK_UI_BODY_TO_HEAD 0.6f
// Headings are drawn above their natural scale so they read as headings and
// not as another row of options.
#define NK_UI_HEAD_BOOST 1.35f
#define NK_UI_BODY_BOOST 1.85f
#define NK_UI_TITLE_TO_HEAD 1.5f
// A heading is one row; the gaps around it are sized in pixels, see
// nk_ui_column_gap_px() and NK_UI_HEAD_GAP_DIVISOR.
#define NK_UI_HEAD_ROW_ROWS 1
// Gap above a heading, as a fraction of a row.
#define NK_UI_HEAD_LEAD_DIVISOR 4
#define NK_UI_FALLBACK_GLYPH '?'
#define NK_UI_PALETTE_TO_BYTE 4
#define NK_UI_ROW_PAD_DIVISOR 4.0
#define NK_UI_HEAD_ROW_PAD_DIVISOR 4.0

struct nk_ui_glyph
{
	short x, y, w;
};

struct nk_ui_font
{
	struct nk_user_font handle;
	struct nk_ui_glyph glyph[NK_UI_FONT_CHARS];
	int first, last, src_h;
	float scale;
	int atlas_w, atlas_h;
	GLuint tex;
	int ready;
};

// Panels are set in the small Descent font the tiny menus use. Section
// headings keep the larger menu font so they still stand off the page, and
// the panel title keeps the big one.
static struct nk_ui_font s_body_font, s_head_font, s_title_font;
static grs_font *s_baked_body_src = NULL;
static int s_baked_screen_h = 0;
static int s_title_scale_max = 1;
static float nk_ui_text_width(const struct nk_ui_font *font, const char *text);
static void nk_ui_message_lines(struct nk_context *ctx, const char *text);
static void nk_ui_title_lines(struct nk_context *ctx, const char *text);

static int nk_ui_font_glyph_index(const struct nk_ui_font *font, nk_rune codepoint)
{
	if (codepoint < (nk_rune)font->first || codepoint > (nk_rune)font->last || !font->glyph[codepoint].w)
		codepoint = NK_UI_FALLBACK_GLYPH;
	if (codepoint < (nk_rune)font->first || codepoint > (nk_rune)font->last)
		return -1;
	return font->glyph[codepoint].w ? (int)codepoint : -1;
}

static float nk_ui_font_text_width(nk_handle handle, float height, const char *text, int len)
{
	const struct nk_ui_font *font = (const struct nk_ui_font *)handle.ptr;
	float width = 0.0f;
	int i, index;

	(void)height;
	for (i = 0; i < len; i++)
	{
		index = nk_ui_font_glyph_index(font, (unsigned char)text[i]);
		if (index >= 0)
			width += font->glyph[index].w * font->scale;
	}
	return width;
}

static void nk_ui_font_query_glyph(nk_handle handle, float height, struct nk_user_font_glyph *out, nk_rune codepoint, nk_rune next)
{
	const struct nk_ui_font *font = (const struct nk_ui_font *)handle.ptr;
	int index = nk_ui_font_glyph_index(font, codepoint);

	(void)height;
	(void)next;
	memset(out, 0, sizeof(*out));
	if (index < 0)
		return;

	out->width = (float)(font->glyph[index].w * font->scale);
	out->height = (float)(font->src_h * font->scale);
	out->xadvance = out->width;
	out->uv[0].x = (float)font->glyph[index].x / font->atlas_w;
	out->uv[0].y = (float)font->glyph[index].y / font->atlas_h;
	out->uv[1].x = (float)(font->glyph[index].x + font->glyph[index].w) / font->atlas_w;
	out->uv[1].y = (float)(font->glyph[index].y + font->src_h) / font->atlas_h;
}

static int nk_ui_src_glyph_width(const grs_font *src, int index)
{
	int width = (src->ft_flags & FT_PROPORTIONAL) ? src->ft_widths[index] : src->ft_w;

	return (width < 1 || width > NK_UI_FONT_ATLAS_WIDTH) ? 0 : width;
}

static const ubyte *nk_ui_src_glyph_data(const grs_font *src, int index, int width)
{
	if (src->ft_flags & FT_PROPORTIONAL)
		return src->ft_chars[index];
	if (src->ft_flags & FT_COLOR)
		return src->ft_data + index * width * src->ft_h;
	return src->ft_data + index * ((width + 7) >> 3) * src->ft_h;
}

// The game's own palette, read once. Baking from gr_palette would tint the
// menu font with whatever a custom menu background loaded there.
static const ubyte *nk_ui_font_palette(void)
{
	static ubyte palette[NK_UI_PALETTE_BYTES];
	static int loaded;

	if (!loaded)
	{
		PHYSFS_file *fp = PHYSFSX_openReadBuffered("palette.256");

		loaded = 1;
		memcpy(palette, gr_palette, sizeof(palette));
		if (fp)
		{
			PHYSFS_read(fp, palette, sizeof(palette), 1);
			PHYSFS_close(fp);
		}
	}
	return palette;
}

static void nk_ui_put_pixel(ubyte *rgba, int on, int palette_index, int colored)
{
	const ubyte *palette = nk_ui_font_palette();

	if (!on || (colored && palette_index == TRANSPARENCY_COLOR))
	{
		memset(rgba, 0, 4);
		return;
	}
	if (colored)
	{
		// Keep the font's light/dark shading but drop its yellow-orange tint:
		// Nuklear multiplies text colour by this, so a neutral glyph lets the
		// theme (and hover state) pick the colour.
		int brightest = max(palette[palette_index * 3], max(palette[palette_index * 3 + 1], palette[palette_index * 3 + 2]));
		memset(rgba, min(255, brightest * NK_UI_PALETTE_TO_BYTE), 3);
	}
	else
		memset(rgba, 255, 3);
	rgba[3] = 255;
}

static void nk_ui_blit_glyph(ubyte *atlas, int atlas_w, const struct nk_ui_glyph *slot, const grs_font *src, const ubyte *data, int colored)
{
	int row, col, bits = 0, mask = 0;

	for (row = 0; row < src->ft_h; row++)
	{
		mask = 0;
		for (col = 0; col < slot->w; col++)
		{
			ubyte *pixel = atlas + ((slot->y + row) * atlas_w + slot->x + col) * 4;

			if (colored)
				nk_ui_put_pixel(pixel, 1, *data++, 1);
			else
			{
				if (!mask)
				{
					bits = *data++;
					mask = 0x80;
				}
				nk_ui_put_pixel(pixel, bits & mask, 0, 0);
				mask >>= 1;
			}
		}
	}
}

// Lays glyphs out row by row; returns the atlas height needed.
static int nk_ui_layout_glyphs(struct nk_ui_font *font, const grs_font *src)
{
	int x = 0, y = 0, i, width;

	for (i = src->ft_minchar; i <= src->ft_maxchar; i++)
	{
		width = nk_ui_src_glyph_width(src, i - src->ft_minchar);
		if (!width)
			continue;
		if (x + width + NK_UI_FONT_GAP > NK_UI_FONT_ATLAS_WIDTH)
		{
			x = 0;
			y += src->ft_h + NK_UI_FONT_GAP;
		}
		font->glyph[i].x = (short)x;
		font->glyph[i].y = (short)y;
		font->glyph[i].w = (short)width;
		x += width + NK_UI_FONT_GAP;
	}
	return y + src->ft_h + NK_UI_FONT_GAP;
}

static int nk_ui_bake_font(struct nk_ui_font *font, const grs_font *src, int scale)
{
	ubyte *atlas;
	int i, colored = (src->ft_flags & FT_COLOR) != 0;

	memset(font->glyph, 0, sizeof(font->glyph));
	font->first = src->ft_minchar;
	font->last = src->ft_maxchar;
	font->src_h = src->ft_h;
	font->scale = scale;
	font->atlas_w = NK_UI_FONT_ATLAS_WIDTH;
	font->atlas_h = nk_ui_layout_glyphs(font, src);

	atlas = (ubyte *)d_calloc(font->atlas_w * font->atlas_h, 4);
	if (!atlas)
		return 0;

	for (i = src->ft_minchar; i <= src->ft_maxchar; i++)
	{
		int width = font->glyph[i].w;
		if (width)
			nk_ui_blit_glyph(atlas, font->atlas_w, &font->glyph[i], src, nk_ui_src_glyph_data(src, i - src->ft_minchar, width), colored);
	}

	if (font->tex)
		glDeleteTextures(1, &font->tex);
	glGenTextures(1, &font->tex);
	glBindTexture(GL_TEXTURE_2D, font->tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, font->atlas_w, font->atlas_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas);
	glBindTexture(GL_TEXTURE_2D, 0);
	d_free(atlas);

	font->handle.userdata = nk_handle_ptr(font);
	font->handle.height = (float)(src->ft_h * scale);
	font->handle.width = nk_ui_font_text_width;
	font->handle.query = nk_ui_font_query_glyph;
	font->handle.texture = nk_handle_id((int)font->tex);
	font->ready = 1;
	return 1;
}

#define NK_UI_MAX_SCREEN_FRAC 0.95f
#define NK_UI_MODAL_MIN_WIDTH_ROWS 24
#define NK_UI_NUMBER_EXTRA_ROWS 5
#define NK_UI_INPUT_MIN_CHARS 24
#define NK_UI_SCROLLBAR_ROWS 1

// Pixel width of the widest line in `text` (a table row counts each tab cell
// separately) when drawn in `font`.
static float nk_ui_text_width(const struct nk_ui_font *font, const char *text)
{
	float widest = 0.0f, line = 0.0f;
	const char *c;

	for (c = text; ; c++)
	{
		if (*c == '\n' || *c == '\t' || !*c)
		{
			widest = line > widest ? line : widest;
			line = 0.0f;
			if (!*c)
				break;
			continue;
		}
		line += nk_ui_font_text_width(nk_handle_ptr((void *)font), 0.0f, c, 1);
	}
	return widest;
}

static float s_widest_label = 0.0f;

static void nk_ui_note(const char *text, float extra_rows)
{
	float wanted;

	if (!text || !s_body_font.ready)
		return;
	wanted = nk_ui_text_width(&s_body_font, text) + extra_rows * s_row_h;
	if (wanted > s_widest_label)
		s_widest_label = wanted;
}

// Widest a single item wants its panel to be, widget chrome included.
static int nk_ui_is_section_head(const newmenu_item *item);

// The font this panel's items are drawn in: the heading font on a menu that
// is only a list of places to go, the small one everywhere else. Measuring
// and drawing have to agree, so both read it here.
static const struct nk_ui_font *s_item_font = &s_body_font;

static float nk_ui_item_width(const newmenu_item *item)
{
	const struct nk_ui_font *font = (s_head_font.ready && nk_ui_is_section_head(item)) ? &s_head_font : s_item_font;
	float text = item->text ? nk_ui_text_width(font, item->text) : 0.0f;
	float box = (float)s_row_h;

	switch (item->type)
	{
		case NM_TYPE_CHECK:
		case NM_TYPE_RADIO:
			return text + box * 2;
		case NM_TYPE_NUMBER:
			return text + box * NK_UI_NUMBER_EXTRA_ROWS;
		case NM_TYPE_INPUT:
		case NM_TYPE_INPUT_MENU:
			return nk_ui_font_text_width(nk_handle_ptr(&s_body_font), 0.0f, "0", 1) * min(item->text_len, NK_UI_INPUT_MIN_CHARS) + box;
		case NM_TYPE_MENU:
			if (item->text && strchr(item->text, '\t'))
				return 0.0f;
			return text + box;
		default:
			if (item->text && strchr(item->text, '\t'))
				return 0.0f;
			return text;
	}
}

#define NK_UI_WHITE_TEX_SIZE 2
static GLuint s_white_tex = 0;

// Untextured shapes (panels, buttons) sample this; unlike the stock atlas it
// is rebuilt with the fonts, so it survives the GL context a mode switch makes.
static void nk_ui_bake_white_texture(void)
{
	ubyte white[NK_UI_WHITE_TEX_SIZE * NK_UI_WHITE_TEX_SIZE * 4];

	memset(white, 255, sizeof(white));
	if (s_white_tex)
		glDeleteTextures(1, &s_white_tex);
	glGenTextures(1, &s_white_tex);
	glBindTexture(GL_TEXTURE_2D, s_white_tex);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, NK_UI_WHITE_TEX_SIZE, NK_UI_WHITE_TEX_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
	glBindTexture(GL_TEXTURE_2D, 0);
	s_device.tex_null.texture = nk_handle_id((int)s_white_tex);
	s_device.tex_null.uv = nk_vec2(0.5f, 0.5f);
}

static void nk_ui_apply_theme(struct nk_context *ctx)
{
	// Deep steel-blue panels with cool blue trim; one accent family so the
	// whole scheme retunes from the four ACCENT values.
	struct nk_color table[NK_COLOR_COUNT];
	struct nk_color accent = NK_UI_ACCENT;
	struct nk_color bright = NK_UI_ACCENT_BRIGHT;
	struct nk_color title = NK_UI_TITLE_TEXT;
	int pad = max(6, s_row_h / 3);

	table[NK_COLOR_TEXT] = nk_rgba(222, 228, 238, 255);
	table[NK_COLOR_WINDOW] = nk_ui_shade(0);
	table[NK_COLOR_HEADER] = nk_ui_shade(14);
	table[NK_COLOR_BORDER] = NK_UI_ACCENT_DIM;
	table[NK_COLOR_BUTTON] = nk_ui_shade(20);
	table[NK_COLOR_BUTTON_HOVER] = nk_ui_shade(42);
	table[NK_COLOR_BUTTON_ACTIVE] = NK_UI_ACCENT_DIM;
	table[NK_COLOR_TOGGLE] = nk_ui_shade(10);
	table[NK_COLOR_TOGGLE_HOVER] = nk_ui_shade(30);
	table[NK_COLOR_TOGGLE_CURSOR] = accent;
	table[NK_COLOR_SELECT] = nk_ui_shade(12);
	table[NK_COLOR_SELECT_ACTIVE] = NK_UI_ACCENT_DIM;
	table[NK_COLOR_SLIDER] = nk_ui_shade(12);
	table[NK_COLOR_SLIDER_CURSOR] = accent;
	table[NK_COLOR_SLIDER_CURSOR_HOVER] = bright;
	table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = bright;
	table[NK_COLOR_PROPERTY] = nk_ui_shade(16);
	table[NK_COLOR_EDIT] = nk_rgba(8, 10, 15, 255);
	table[NK_COLOR_EDIT_CURSOR] = bright;
	table[NK_COLOR_COMBO] = nk_ui_shade(16);
	table[NK_COLOR_CHART] = nk_ui_shade(12);
	table[NK_COLOR_CHART_COLOR] = accent;
	table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 60, 40, 255);
	table[NK_COLOR_SCROLLBAR] = nk_ui_shade(6);
	table[NK_COLOR_SCROLLBAR_CURSOR] = nk_ui_shade(56);
	table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_ui_shade(84);
	table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = accent;
	table[NK_COLOR_TAB_HEADER] = nk_ui_shade(14);
	nk_style_from_table(ctx, table);

	ctx->style.window.border = 2.0f;
	ctx->style.window.rounding = 0.0f;
	ctx->style.window.padding = nk_vec2((float)pad, (float)pad);
	ctx->style.window.spacing = nk_vec2((float)pad, (float)(pad / 3));
	ctx->style.window.header.padding = nk_vec2((float)pad, (float)(pad / 2));
	ctx->style.window.header.label_padding = nk_vec2((float)pad, (float)(pad / 2));
	ctx->style.window.header.label_normal = title;
	ctx->style.window.header.label_hover = title;
	ctx->style.window.header.label_active = title;
	ctx->style.button.rounding = 0.0f;
	ctx->style.button.border = 1.0f;
	ctx->style.button.border_color = nk_ui_shade(58);
	ctx->style.button.padding = nk_vec2((float)pad, (float)(pad / 2));
	ctx->style.button.text_hover = bright;
	ctx->style.button.text_active = nk_rgba(255, 255, 255, 255);
	ctx->style.slider.rounding = 0.0f;
	ctx->style.slider.bar_height = (float)max(4, s_row_h / 4);
	ctx->style.slider.cursor_size = nk_vec2((float)max(10, s_row_h / 2), (float)s_row_h * 0.8f);
	// Nuklear centres the knob on the bar end, so half of it hangs outside.
	ctx->style.slider.padding = nk_vec2(ctx->style.slider.cursor_size.x * 0.5f + 1.0f, 2.0f);
	ctx->style.edit.rounding = 0.0f;
	ctx->style.edit.border = 1.0f;
	ctx->style.edit.border_color = nk_ui_shade(58);
	// Nuklear highlights selected text in the plain text colour, which over
	// a dark field reads as a white bar covering the value.
	ctx->style.edit.selected_normal = NK_UI_ACCENT_DIM;
	ctx->style.edit.selected_hover = NK_UI_ACCENT_DIM;
	ctx->style.edit.selected_text_normal = table[NK_COLOR_TEXT];
	ctx->style.edit.selected_text_hover = table[NK_COLOR_TEXT];
	ctx->style.edit.cursor_size = 2.0f;
	ctx->style.property.rounding = 0.0f;
	ctx->style.scrollv.rounding = 0.0f;
	ctx->style.scrollv.rounding_cursor = 0.0f;
	ctx->style.selectable.rounding = 0.0f;
	ctx->style.selectable.text_hover = bright;
	ctx->style.selectable.text_normal_active = nk_rgba(255, 255, 255, 255);
	ctx->style.combo.rounding = 0.0f;
	ctx->style.combo.border = 1.0f;
	ctx->style.combo.border_color = nk_ui_shade(58);
	ctx->style.combo.label_hover = bright;
	ctx->style.contextual_button.rounding = 0.0f;
	ctx->style.contextual_button.text_hover = bright;
	// Tree tabs are the section headings of the immediate-mode screens, so
	// they are drawn the way newmenu draws its headings.
	ctx->style.tab.background = nk_style_item_color(nk_ui_shade(8));
	ctx->style.tab.border_color = NK_UI_ACCENT_DIM;
	ctx->style.tab.text = bright;
	ctx->style.tab.border = 0.0f;
	ctx->style.tab.rounding = 0.0f;
	ctx->style.tab.sym_minimize = NK_SYMBOL_PLUS;
	ctx->style.tab.sym_maximize = NK_SYMBOL_MINUS;
	ctx->style.tab.tab_minimize_button.text_normal = bright;
	ctx->style.tab.tab_minimize_button.text_hover = bright;
	ctx->style.tab.tab_minimize_button.text_active = bright;
	ctx->style.tab.tab_maximize_button = ctx->style.tab.tab_minimize_button;
}

int nk_ui_menu_color_count(void)
{
	return NK_UI_MENU_COLOR_COUNT;
}

const char *nk_ui_menu_color_name(int index)
{
	if (index < 0 || index >= NK_UI_MENU_COLOR_COUNT)
		index = 0;
	return s_menu_colors[index].name;
}

void nk_ui_set_menu_color(int index)
{
	if (index < 0 || index >= NK_UI_MENU_COLOR_COUNT)
		index = 0;
	if (index == s_menu_color)
		return;
	s_menu_color = index;
	if (s_initialized)
		nk_ui_apply_theme(s_ctx);
}

// Everything a panel spends outside its rows: title bar, window padding and
// border, plus a row of slack so the last row is never clipped.
static int nk_ui_panel_chrome_height(void)
{
	const struct nk_style_window *win = &s_ctx->style.window;
	float title = s_title_font.ready ? s_title_font.handle.height : (float)s_row_h;
	float header = title + (win->header.padding.y + win->header.label_padding.y) * 2;

	return (int)(header + win->padding.y * 2 + win->border * 2 + s_row_h + win->spacing.y);
}

// Re-bakes the Descent fonts when the game swapped them, when the screen
// size changed, or when the GL context was thrown away underneath us, and
// rederives every metric from the result. Call before building a frame.
static void nk_ui_forget_textures(void);

// The GL context is rebuilt by video mode changes, and the game then hands
// our old texture ids to its own textures -- so glIsTexture() stays true
// while the id draws someone else's picture. Compare generations instead,
// and drop the ids without deleting them: they are no longer ours.
static void nk_ui_sync_context(void)
{
	static int seen = -1;

	if (seen == ogl_context_generation)
		return;
	if (seen >= 0)
		nk_ui_forget_textures();
	seen = ogl_context_generation;
}

static void nk_ui_sync_fonts(void)
{
	grs_font *body = GAME_FONT, *head = MEDIUM1_FONT, *title = HUGE_FONT;
	int screen_h = grd_curscreen->sc_h;
	int head_px, head_scale, base_h, head_h, body_h, body_scale, title_scale;
	int textures_lost;

	if (!s_initialized || !body)
		return;

	// SDL 1.2 builds a new GL context for every video mode change, which
	// silently invalidates every texture id we hold. Sampling one of those
	// draws solid white, so the text becomes blocks; ask GL rather than try
	// to guess which game events rebuild the context.
	nk_ui_sync_context();
	textures_lost = s_body_font.ready && !glIsTexture(s_body_font.tex);

	if (s_body_font.ready && !textures_lost && body == s_baked_body_src && screen_h == s_baked_screen_h)
		return;

	// Re-baking frees the font textures, and a panel already built into the
	// pending frame still points at them -- its text would then be drawn
	// with whatever GL hands out next, as solid blocks. Present that frame
	// before replacing anything it refers to.
	nk_ui_flush();

	// Sizes hang off the heading font, which is what the panels used to be
	// set in: the body is a fraction of it and the title a multiple, so the
	// whole scheme keeps its proportions at any resolution.
	head_px = max(head->ft_h, screen_h / NK_UI_HEAD_PX_DIVISOR);
	head_scale = max(1, (head_px + head->ft_h / 2) / head->ft_h);
	base_h = head->ft_h * head_scale;
	// The boost is fractional, so the atlas stays at 1x and only the draw
	// size grows; the body still sizes off the unboosted height.
	head_h = (int)(base_h * NK_UI_HEAD_BOOST + 0.5f);

	body_scale = max(1, (int)(base_h * NK_UI_BODY_TO_HEAD / body->ft_h + 0.5f));
	// A body no smaller than the heading over it is not a body.
	while (body_scale > 1 && body->ft_h * body_scale >= base_h)
		body_scale--;
	if (!nk_ui_bake_font(&s_body_font, body, 1))
		return;
	body_h = (int)(body->ft_h * body_scale * NK_UI_BODY_BOOST + 0.5f);
	s_body_font.scale = (float)body_h / body->ft_h;
	s_body_font.handle.height = (float)body_h;
	nk_ui_bake_white_texture();

	if (head && nk_ui_bake_font(&s_head_font, head, 1))
	{
		s_head_font.scale = (float)head_h / head->ft_h;
		s_head_font.handle.height = (float)head_h;
	}

	if (title && nk_ui_bake_font(&s_title_font, title, 1))
	{
		title_scale = max(1, (int)(head_h * NK_UI_TITLE_TO_HEAD / title->ft_h + 0.5f));
		s_title_scale_max = title_scale;
		s_title_font.scale = title_scale;
		s_title_font.handle.height = (float)(title->ft_h * title_scale);
	}

	s_baked_body_src = body;
	s_baked_screen_h = screen_h;
	// Rows must clear the heading font too: headings and navigation menus
	// are drawn in it, at the same row height as everything else.
	s_row_h = body_h + body_h / NK_UI_ROW_PAD_DIVISOR;
	s_row_h = max(s_row_h, head_h + head_h / NK_UI_HEAD_ROW_PAD_DIVISOR);
	nk_style_set_font(s_ctx, &s_body_font.handle);
	nk_ui_apply_theme(s_ctx);
	s_chrome_h = nk_ui_panel_chrome_height();
}

static void nk_ui_init_once(void)
{
	const void *image;
	int w, h;

	if (s_initialized)
		return;

	s_ctx = &s_ctx_storage;
	nk_init_default(s_ctx, 0);
	nk_buffer_init_default(&s_device.cmds);

	nk_font_atlas_init_default(&s_atlas);
	nk_font_atlas_begin(&s_atlas);
	image = nk_font_atlas_bake(&s_atlas, &w, &h, NK_FONT_ATLAS_RGBA32);
	nk_ui_upload_atlas(image, w, h);
	nk_font_atlas_end(&s_atlas, nk_handle_id((int)s_device.font_tex), &s_device.tex_null);
	if (s_atlas.default_font)
		nk_style_set_font(s_ctx, &s_atlas.default_font->handle);

	s_menu_color = (GameCfg.MenuColor >= 0 && GameCfg.MenuColor < NK_UI_MENU_COLOR_COUNT) ? GameCfg.MenuColor : 0;
	nk_ui_apply_theme(s_ctx);

	s_initialized = 1;
	nk_ui_sync_fonts();
}

// One iteration of a modal Nuklear screen: pump input, let the caller build
// widgets into a floating window sized to the current draw area, then
// render+present. Returns 0 if the player closed the window/quit, 1
// otherwise. `size_frac` is the window's (width, height) as a fraction of
// the screen -- smaller screens (like the object/weapon toggle lists) should
// pass a smaller fraction so they read as a floating panel, not a takeover.
// The title bar is drawn during nk_begin, so swap in the big Descent title
// font just for that call.
static void nk_ui_set_title_scale(int scale)
{
	s_title_font.scale = scale;
	s_title_font.handle.height = (float)(s_title_font.src_h * scale);
}

// Largest title scale (down to 1) at which `title` fits `avail` pixels, or 0.
static int nk_ui_fit_title_scale(const char *title, float avail)
{
	int scale;

	for (scale = s_title_scale_max; scale >= 1; scale--)
	{
		nk_ui_set_title_scale(scale);
		if (nk_ui_text_width(&s_title_font, title) <= avail)
			return scale;
	}
	return 0;
}

// The big Descent font is very wide, so a long title steps down in size and
// finally falls back to the body font rather than running off the panel.
static int nk_ui_begin_panel(const char *name, const char *title, struct nk_rect bounds, nk_flags flags)
{
	float avail = bounds.w - (s_ctx->style.window.header.padding.x
		+ s_ctx->style.window.header.label_padding.x) * 2 - s_ctx->style.window.border * 2;
	int fit = s_title_font.ready ? nk_ui_fit_title_scale(title, avail) : 0;
	// Too long even for the smallest big-font step: fall back to the heading
	// font, never to the small font the panel body is set in.
	const struct nk_ui_font *font = fit ? &s_title_font : (s_head_font.ready ? &s_head_font : NULL);
	int opened;

	if (font)
		nk_style_push_font(s_ctx, &font->handle);
	opened = nk_begin_titled(s_ctx, name, title, bounds, flags);
	if (font)
		nk_style_pop_font(s_ctx);
	if (s_title_font.ready)
		nk_ui_set_title_scale(s_title_scale_max);
	return opened;
}

// Nested screens can't run inside a build callback: their render ends in
// nk_clear(), which frees the calling window mid-layout. Callbacks queue
// the screen here and nk_ui_frame() runs it once its own frame is done.
static void (*s_pending_screen)(void) = NULL;

static void nk_ui_defer(void (*screen)(void))
{
	s_pending_screen = screen;
}

static void nk_ui_run_pending_screen(void)
{
	void (*screen)(void) = s_pending_screen;

	s_pending_screen = NULL;
	if (screen)
		screen();
}

// Keys and wheel steps gathered this frame, in whole rows. `before` is the
// scroll offset when the panel opened, used to tell whether Nuklear moved it.
static void nk_ui_apply_manual_scroll(nk_uint scroll_x, nk_uint before, int use_arrows)
{
	nk_uint after = before;
	int delta = use_arrows ? s_arrow_steps * NK_UI_ROW_HEIGHT : 0;

	nk_window_get_scroll(s_ctx, &scroll_x, &after);
	if (after == before)
		delta += s_wheel_steps * NK_UI_SCROLL_ROWS_PER_STEP * NK_UI_ROW_HEIGHT;
	s_wheel_steps = 0;
	if (use_arrows)
		s_arrow_steps = 0;
	if (!delta)
		return;

	delta += (int)after;
	nk_window_set_scroll(s_ctx, scroll_x, (nk_uint)(delta < 0 ? 0 : delta));
}

// True once per Enter press; screens treat it as their default action.
static int nk_ui_take_enter(void)
{
	int pressed = s_enter_pressed;

	s_enter_pressed = 0;
	return pressed;
}

static float s_modal_need = 0.0f;

static int nk_ui_frame(const char *title, struct nk_vec2 size_frac, void (*build)(struct nk_context *ctx, void *userdata), void *userdata)
{
	SDL_Event evt;
	int width = grd_curscreen->sc_w, height = grd_curscreen->sc_h;
	struct nk_rect bounds;
	float panel_w;
	int title_wraps;
	nk_flags win_flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE;

	nk_ui_sync_fonts();
	mouse_cursor_autohide();

	// Only keys pressed while this screen is up may act on it: the Enter
	// that chose "HOST GAME" in the menu underneath is still flagged when
	// we get here, and would otherwise start the game immediately.
	s_enter_pressed = 0;
	s_escape_pressed = 0;

	nk_input_begin(s_ctx);
	while (SDL_PollEvent(&evt))
	{
		if (evt.type == SDL_QUIT)
		{
			nk_input_end(s_ctx);
			return 0;
		}
		nk_ui_handle_event(&evt);
	}
	nk_input_end(s_ctx);

	if (s_escape_pressed)
	{
		s_escape_pressed = 0;
		return 0;
	}

	nk_ui_draw_underlying_windows();

	panel_w = max(width * size_frac.x, (float)(s_row_h * NK_UI_MODAL_MIN_WIDTH_ROWS));
	panel_w = max(panel_w, s_modal_need + s_ctx->style.window.padding.x * 2 + s_ctx->style.window.border * 2 + s_row_h * NK_UI_SCROLLBAR_ROWS);
	if (s_title_font.ready)
		panel_w = max(panel_w, nk_ui_text_width(&s_title_font, title) / s_title_font.scale + s_ctx->style.window.padding.x * 4);
	panel_w = min(panel_w, width * NK_UI_MAX_SCREEN_FRAC);
	title_wraps = nk_ui_text_width(s_head_font.ready ? &s_head_font : &s_body_font, title)
		> panel_w - s_ctx->style.window.padding.x * 2 - s_row_h * NK_UI_SCROLLBAR_ROWS;
	if (title_wraps)
		win_flags &= ~NK_WINDOW_TITLE;
	bounds = nk_rect((width - panel_w) * 0.5f, height * (1.0f - size_frac.y) * 0.5f,
		panel_w, height * size_frac.y);
	nk_ui_claim_panel(title);
	if (nk_ui_begin_panel(title, title_wraps ? "" : title, bounds, win_flags))
	{
		nk_uint scroll_x = 0, scroll_y_before = 0;

		nk_window_get_scroll(s_ctx, &scroll_x, &scroll_y_before);
		if (title_wraps)
		{
			nk_layout_row_dynamic(s_ctx, (float)NK_UI_ROW_HEIGHT, 1);
			nk_ui_title_lines(s_ctx, title);
		}
		s_widest_label = 0.0f;
		build(s_ctx, userdata);
		s_modal_need = s_widest_label;
		nk_ui_apply_manual_scroll(scroll_x, scroll_y_before, 1);
	}
	nk_end(s_ctx);
	nk_window_set_focus(s_ctx, title);
	s_dim_next = 1;
	s_wheel_steps = 0;
	s_arrow_steps = 0;
	s_enter_pressed = 0;

	s_panels_pending = 1;
	nk_ui_flush();
	ogl_swap_buffers_internal();
	nk_ui_run_pending_screen();

	return 1;
}

// ==============================
// Objects allowed / spawn-with / static-weapons -- same design as the rest
// of the Nuklear UI, replacing the old newmenu-based popups these used to
// delegate to (net_udp_set_power / net_udp_spawn_with_weapons_menu /
// net_udp_staticpowerupsmenu, still used by the legacy non-OGL fallback).
// ==============================

static void nk_ui_build_objects_allowed(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;
	int i;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_ui_take_enter())
		*running = 0;
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	for (i = 0; i < MULTI_ALLOW_POWERUP_MAX; i++)
	{
		ubyte bit = (Netgame.AllowedItems >> i) & 1;
		nk_bool val = bit ? nk_true : nk_false;
		nk_checkbox_label(ctx, multi_allow_powerup_text[i], &val);
		if (val)
			Netgame.AllowedItems |= (1u << i);
		else
			Netgame.AllowedItems &= ~(1u << i);
	}
}

static void nk_ui_objects_allowed(void)
{
	int running = 1;

	nk_ui_init_once();

	while (running)
	{
		if (!nk_ui_frame("Objects Allowed", nk_vec2(0.4f, 0.6f), nk_ui_build_objects_allowed, &running))
			break;
	}
}

static void nk_ui_build_spawn_with_weapons(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;
	int v;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_ui_take_enter())
		*running = 0;
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	nk_ui_checkbox_ubyte(ctx, "Fusion", &Netgame.FusionSpawn);
	nk_ui_checkbox_ubyte(ctx, "Vulcan", &Netgame.VulcanSpawn);
	nk_ui_checkbox_ubyte(ctx, "Plasma", &Netgame.PlasmaSpawn);
	nk_ui_checkbox_ubyte(ctx, "Spread", &Netgame.SpreadSpawn);

	v = Netgame.LasersSpawn;
	nk_labelf(ctx, NK_TEXT_LEFT, "Quad Laser Level: %d", Netgame.LasersSpawn);
	nk_slider_int(ctx, 0, &v, 4, 1);
	Netgame.LasersSpawn = (ubyte)v;

	nk_ui_checkbox_ubyte(ctx, "Homers", &Netgame.HomersSpawn);
	nk_ui_checkbox_ubyte(ctx, "Smarts", &Netgame.SmartsSpawn);
	nk_ui_checkbox_ubyte(ctx, "Megas", &Netgame.MegasSpawn);
	nk_ui_checkbox_ubyte(ctx, "Bombs", &Netgame.BombsSpawn);
}

static void nk_ui_spawn_with_weapons(void)
{
	int running = 1;

	nk_ui_init_once();

	while (running)
	{
		if (!nk_ui_frame("Spawn Weapons", nk_vec2(0.35f, 0.5f), nk_ui_build_spawn_with_weapons, &running))
			break;
	}
}

static void nk_ui_build_static_weapons(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_ui_take_enter())
		*running = 0;
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	nk_ui_checkbox_ubyte(ctx, "All Weapons", &Netgame.StaticPowerups);
	nk_ui_checkbox_ubyte(ctx, "Fusion", &Netgame.StaticFusion);
	nk_ui_checkbox_ubyte(ctx, "Spreadfire", &Netgame.StaticSpread);
	nk_ui_checkbox_ubyte(ctx, "Vulcan", &Netgame.StaticVulcan);
	nk_ui_checkbox_ubyte(ctx, "Plasma", &Netgame.StaticPlasma);
	nk_ui_checkbox_ubyte(ctx, "Lasers", &Netgame.StaticLasers);
	nk_ui_checkbox_ubyte(ctx, "Bombs", &Netgame.StaticBombs);
	nk_ui_checkbox_ubyte(ctx, "Missiles", &Netgame.StaticMissiles);
}

static void nk_ui_static_weapons(void)
{
	int running = 1;

	nk_ui_init_once();

	while (running)
	{
		if (!nk_ui_frame("Static Weapons", nk_vec2(0.35f, 0.55f), nk_ui_build_static_weapons, &running))
			break;
	}
}

// ==============================
// Weapon autoselect -- both priority lists side by side. Each order array
// holds weapon indices plus one 255 sentinel; weapons below the sentinel are
// never picked automatically. Moving the sentinel is how you draw that line.
// ==============================

#define NK_UI_AUTOSELECT_NEVER 255
#define NK_UI_AUTOSELECT_ARROW_ROWS 1.2f
#define NK_UI_AUTOSELECT_RANK_ROWS 1.4f

extern void InitWeaponOrdering(void);

static void nk_ui_swap_bytes(ubyte *a, ubyte *b)
{
	ubyte t = *a;

	*a = *b;
	*b = t;
}

// Lasers rank twice: once for the plain gun and once for the same gun with
// quads fitted, because which one you have decides which entry applies.
static const char *nk_ui_autoselect_name(ubyte weapon, int secondary)
{
	if (weapon == NK_UI_AUTOSELECT_NEVER)
		return NULL;
	if (secondary)
		return SECONDARY_WEAPON_NAMES(weapon);
	if (weapon == QUAD_LASER_INDEX)
		return "Lasers (quad fitted)";
	if (weapon == LASER_INDEX)
		return "Lasers (no quad)";
	return PRIMARY_WEAPON_NAMES(weapon);
}

static void nk_ui_autoselect_list(struct nk_context *ctx, const char *title, ubyte *order, int nentries, int secondary)
{
	float arrow_w = (float)s_row_h * NK_UI_AUTOSELECT_ARROW_ROWS;
	float rank_w = (float)s_row_h * NK_UI_AUTOSELECT_RANK_ROWS;
	float name_w = nk_window_get_content_region(ctx).w - arrow_w * 2 - rank_w - ctx->style.window.spacing.x * 4;
	int rank = 0;
	int i;

	if (name_w < rank_w)
		name_w = rank_w;
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_ACCENT_BRIGHT);
	nk_label(ctx, title, NK_TEXT_CENTERED);
	nk_style_pop_color(ctx);

	for (i = 0; i < nentries; i++)
	{
		const char *name = nk_ui_autoselect_name(order[i], secondary);
		char cell[NM_MAX_TEXT_LEN + 1];

		nk_layout_row_begin(ctx, NK_STATIC, (float)s_row_h, 4);
		nk_layout_row_push(ctx, arrow_w);
		if (i > 0)
		{
			if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_UP))
				nk_ui_swap_bytes(&order[i], &order[i - 1]);
		}
		else
			nk_spacing(ctx, 1);
		nk_layout_row_push(ctx, arrow_w);
		if (i + 1 < nentries)
		{
			if (nk_button_symbol(ctx, NK_SYMBOL_TRIANGLE_DOWN))
				nk_ui_swap_bytes(&order[i], &order[i + 1]);
		}
		else
			nk_spacing(ctx, 1);

		nk_layout_row_push(ctx, rank_w);
		if (!name || rank < 0)
			nk_spacing(ctx, 1);
		else
		{
			snprintf(cell, sizeof(cell), "%d.", ++rank);
			nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_ACCENT_BRIGHT);
			nk_label(ctx, cell, NK_TEXT_RIGHT);
			nk_style_pop_color(ctx);
		}

		nk_layout_row_push(ctx, name_w);
		if (!name)
		{
			nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_ACCENT_DIM);
			nk_label(ctx, "-- never autoselect below --", NK_TEXT_LEFT);
			nk_style_pop_color(ctx);
			rank = -1;
		}
		else
			nk_label(ctx, name, NK_TEXT_LEFT);
		nk_layout_row_end(ctx);
	}
}

static void nk_ui_build_weapon_autoselect(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;
	int min_level;
	float half;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 2);
	if (nk_button_label(ctx, "Back") || nk_ui_take_enter())
		*running = 0;
	if (nk_button_label(ctx, "Restore Defaults"))
	{
		InitWeaponOrdering();
		PlayerCfg.LaserAutoselectMinLevel = 1;
	}

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 2);
	nk_ui_checkbox_ubyte(ctx, "No autoselect while firing", &PlayerCfg.NoFireAutoselect);
	nk_ui_checkbox_ubyte(ctx, "Only cycle autoselect weapons", &PlayerCfg.CycleAutoselectOnly);
	nk_ui_checkbox_ubyte(ctx, "Autoselect after firing", &PlayerCfg.SelectAfterFire);
	nk_ui_checkbox_ubyte(ctx, "Classic no-ammo autoselect", &PlayerCfg.ClassicAutoselectWeapon);
	// Selecting after a burst is meaningless unless firing suppresses it.
	if (PlayerCfg.SelectAfterFire)
		PlayerCfg.NoFireAutoselect = 1;

	// The property draws its name inside the widget, so it has to be short
	// enough for half a panel; the sentence above carries the meaning.
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	nk_label(ctx, "Top of a list is picked first. Lasers weaker than this are skipped:", NK_TEXT_LEFT);

	min_level = PlayerCfg.LaserAutoselectMinLevel < 1 ? 1 : PlayerCfg.LaserAutoselectMinLevel;
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 2);
	nk_property_int(ctx, "Laser level", 1, &min_level, MAX_LASER_LEVEL + 1, 1, 1);
	nk_spacing(ctx, 1);
	PlayerCfg.LaserAutoselectMinLevel = (ubyte)min_level;

	half = (nk_window_get_content_region(ctx).w - ctx->style.window.spacing.x) * 0.5f;
	nk_layout_row_begin(ctx, NK_STATIC, (s_row_h + ctx->style.window.spacing.y) * (MAX_PRIMARY_WEAPONS + 3), 2);
	nk_layout_row_push(ctx, half);
	if (nk_group_begin(ctx, "primary", NK_WINDOW_NO_SCROLLBAR))
	{
		nk_ui_autoselect_list(ctx, "PRIMARY", PlayerCfg.PrimaryOrder, MAX_PRIMARY_WEAPONS + 2, 0);
		nk_group_end(ctx);
	}
	nk_layout_row_push(ctx, half);
	if (nk_group_begin(ctx, "secondary", NK_WINDOW_NO_SCROLLBAR))
	{
		nk_ui_autoselect_list(ctx, "SECONDARY", PlayerCfg.SecondaryOrder, MAX_SECONDARY_WEAPONS + 1, 1);
		nk_group_end(ctx);
	}
	nk_layout_row_end(ctx);
}

void nk_ui_weapon_autoselect(void)
{
	int running = 1;

	nk_ui_init_once();

	while (running)
	{
		if (!nk_ui_frame("Weapon Autoselect", nk_vec2(0.62f, 0.66f), nk_ui_build_weapon_autoselect, &running))
			break;
	}
}

// ==============================
// Advanced options
// ==============================

static void nk_ui_build_advanced_options(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;
	char port_text[6];

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_ui_take_enter())
		*running = 0;
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	if (nk_tree_push(ctx, NK_TREE_TAB, "Game Rules", NK_MINIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);

		v = Netgame.difficulty;
		nk_property_int(ctx, "Difficulty", 0, &v, NDL - 1, 1, 1);
		Netgame.difficulty = (ubyte)v;

		v = Netgame.control_invul_time / 5 / F1_0 / 60;
		nk_labelf(ctx, NK_TEXT_LEFT, "Reactor Life: %d min", Netgame.control_invul_time / F1_0 / 60);
		nk_slider_int(ctx, 0, &v, 10, 1);
		Netgame.control_invul_time = v * 5 * F1_0 * 60;

		v = Netgame.PlayTimeAllowed;
		nk_labelf(ctx, NK_TEXT_LEFT, "Max time: %d min", Netgame.PlayTimeAllowed * 5);
		nk_slider_int(ctx, 0, &v, 10, 1);
		Netgame.PlayTimeAllowed = v;

		v = Netgame.KillGoal;
		nk_labelf(ctx, NK_TEXT_LEFT, "Kill Goal: %d kills", Netgame.KillGoal * 5);
		nk_slider_int(ctx, 0, &v, 10, 1);
		Netgame.KillGoal = v;

		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Powerups & Weapons", NK_MINIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);

		v = Netgame.PrimaryDupFactor - 1;
		nk_labelf(ctx, NK_TEXT_LEFT, "Extra Primaries: %s", Netgame.PrimaryDupFactor < 2 ? "None" : "xN");
		nk_slider_int(ctx, 0, &v, 7, 1);
		Netgame.PrimaryDupFactor = (ubyte)(v + 1);

		v = Netgame.SecondaryDupFactor - 1;
		nk_labelf(ctx, NK_TEXT_LEFT, "Extra Secondaries: %s", Netgame.SecondaryDupFactor < 2 ? "None" : "xN");
		nk_slider_int(ctx, 0, &v, 7, 1);
		Netgame.SecondaryDupFactor = (ubyte)(v + 1);

		v = Netgame.SecondaryCapFactor;
		nk_labelf(ctx, NK_TEXT_LEFT, "Cap Secondaries: %s", Netgame.SecondaryCapFactor == 0 ? "Uncapped" : (Netgame.SecondaryCapFactor == 1 ? "Max Six" : "Max Two"));
		nk_slider_int(ctx, 0, &v, 2, 1);
		Netgame.SecondaryCapFactor = (ubyte)v;

		nk_ui_checkbox_ubyte(ctx, "Low Vulcan Ammo (333)", &Netgame.LowVulcan);

		nk_label(ctx, "Gauss Ammo Style", NK_TEXT_LEFT);
		if (nk_option_label(ctx, "Duplicating (D2)", Netgame.GaussAmmoStyle == GAUSS_STYLE_DUPLICATING)) Netgame.GaussAmmoStyle = GAUSS_STYLE_DUPLICATING;
		if (nk_option_label(ctx, "Original (Depleting)", Netgame.GaussAmmoStyle == GAUSS_STYLE_DEPLETING)) Netgame.GaussAmmoStyle = GAUSS_STYLE_DEPLETING;
		if (nk_option_label(ctx, "Dropping Picked Up", Netgame.GaussAmmoStyle == GAUSS_STYLE_STEADY_RECHARGING)) Netgame.GaussAmmoStyle = GAUSS_STYLE_STEADY_RECHARGING;
		if (nk_option_label(ctx, "Respawning", Netgame.GaussAmmoStyle == GAUSS_STYLE_STEADY_RESPAWNING)) Netgame.GaussAmmoStyle = GAUSS_STYLE_STEADY_RESPAWNING;

		if (nk_button_label(ctx, "Set Objects Allowed"))
			nk_ui_defer(nk_ui_objects_allowed);
		if (nk_button_label(ctx, "Start Mission With"))
			nk_ui_defer(nk_ui_spawn_with_weapons);
		if (nk_button_label(ctx, "Select Static Weapons"))
			nk_ui_defer(nk_ui_static_weapons);

		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Spawn Settings", NK_MINIMIZED))
	{
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
		nk_label(ctx, "Spawn Style", NK_TEXT_LEFT);
		if (nk_option_label(ctx, "No Invuln", Netgame.SpawnStyle == SPAWN_STYLE_NO_INVUL)) Netgame.SpawnStyle = SPAWN_STYLE_NO_INVUL;
		if (nk_option_label(ctx, "Half Second Invuln", Netgame.SpawnStyle == SPAWN_STYLE_SHORT_INVUL)) Netgame.SpawnStyle = SPAWN_STYLE_SHORT_INVUL;
		if (nk_option_label(ctx, "Two Second Invuln", Netgame.SpawnStyle == SPAWN_STYLE_LONG_INVUL)) Netgame.SpawnStyle = SPAWN_STYLE_LONG_INVUL;
		if (nk_option_label(ctx, "Preview", Netgame.SpawnStyle == SPAWN_STYLE_PREVIEW)) Netgame.SpawnStyle = SPAWN_STYLE_PREVIEW;

		nk_label(ctx, "Spawn Logic", NK_TEXT_LEFT);
		nk_ui_checkbox_ubyte(ctx, "Redux: New Spawn Location Algorithm", &Netgame.NewSpawnAlgorithm);
		nk_ui_checkbox_ubyte(ctx, "Smaller Map Spawning", &Netgame.SmallerSpawn);
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Misc Game Modes", NK_MINIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
		nk_ui_checkbox_ubyte(ctx, "King of the Hill", &Netgame.PointCapture);
		v = Netgame.ScoreGoal;
		if (Netgame.PointCapture)
			nk_labelf(ctx, NK_TEXT_LEFT, "Score Goal: %s", Netgame.ScoreGoal == 0 ? "Unlimited" : "");
		else
			nk_label(ctx, "Score Goal: (King of the Hill only)", NK_TEXT_LEFT);
		nk_slider_int(ctx, 0, &v, 10, 1);
		Netgame.ScoreGoal = v;
		nk_ui_checkbox_ubyte(ctx, "Last Man Standing", &Netgame.Deathmatch);
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "SNG Toggles", NK_MINIMIZED))
	{
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
		nk_ui_checkbox_ubyte(ctx, "No Weapon Stun", &Netgame.WeaponStun);
		nk_ui_checkbox_ubyte(ctx, "No Fusion Flash", &Netgame.PurpleFlash);
		nk_ui_checkbox_ubyte(ctx, "Vulcan Overheat", &Netgame.VulcanShake);
		nk_ui_checkbox_ubyte(ctx, "No Fusion Shake", &Netgame.FusionShake);
		nk_ui_checkbox_ubyte(ctx, "Fast Doors", &Netgame.FastDoor);
		nk_ui_checkbox_ubyte(ctx, "Dark Smart Blobs", &Netgame.DarkSmartBlobs);
		nk_ui_checkbox_ubyte(ctx, "Quiet Fan", &Netgame.QuietFan);
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Visual & HUD", NK_MINIMIZED))
	{
		int show_on_map;
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);

		nk_ui_checkbox_ubyte(ctx, "Respawn Concussions", &Netgame.RespawnConcs);
		nk_ui_checkbox_ubyte(ctx, "All Players Blue", &Netgame.FairColors);
		nk_ui_checkbox_ubyte(ctx, "Allow Colored Dynamic Lighting", &Netgame.AllowColoredLighting);
		nk_ui_checkbox_ubyte(ctx, "Allow Preferred Colors", &Netgame.AllowPreferredColors);

		{
			int v = max(0, Netgame.HomingUpdateRate - 20);
			nk_labelf(ctx, NK_TEXT_LEFT, "Homing Update Rate: %d", Netgame.HomingUpdateRate);
			nk_slider_int(ctx, 0, &v, 10, 1);
			Netgame.HomingUpdateRate = (ubyte)(v + 20);
		}

		nk_ui_checkbox_ubyte(ctx, "Only Show Confirmed Hit Sparks", &Netgame.RemoteHitSpark);
		nk_ui_checkbox_ubyte(ctx, "Allow Custom Models/Textures", &Netgame.AllowCustomModelsTextures);
		nk_ui_checkbox_ubyte(ctx, "Reduced Flash Effects", &Netgame.ReducedFlash);
		nk_ui_checkbox_ubyte(ctx, "Lock FOV to Vanilla", &Netgame.DisableFOVChange);
		nk_ui_checkbox_short(ctx, "Bright Player Ships", &Netgame.BrightPlayers);
		nk_ui_checkbox_short(ctx, "Show Enemy Names on HUD", &Netgame.ShowEnemyNames);
		nk_ui_checkbox_ubyte(ctx, "Alternate Colors (Ships 6 & 7)", &Netgame.BlackAndWhitePyros);

		show_on_map = (Netgame.game_flags & NETGAME_FLAG_SHOW_MAP) ? 1 : 0;
		{
			nk_bool val = show_on_map ? nk_true : nk_false;
			nk_checkbox_label(ctx, TXT_SHOW_ON_MAP, &val);
			show_on_map = val;
		}
		if (show_on_map)
			Netgame.game_flags |= NETGAME_FLAG_SHOW_MAP;
		else
			Netgame.game_flags &= ~NETGAME_FLAG_SHOW_MAP;

		nk_ui_checkbox_ubyte(ctx, "No Friendly Fire (Team/Coop)", &Netgame.NoFriendlyFire);
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Network Settings", NK_MINIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);

		v = Netgame.PacketsPerSec;
		nk_property_int(ctx, "Packets per second", 2, &v, 40, 1, 1);
		Netgame.PacketsPerSec = (short)v;

		strncpy(port_text, net_udp_get_my_port_buf(), sizeof(port_text) - 1);
		port_text[sizeof(port_text) - 1] = '\0';
		nk_label(ctx, "Network port", NK_TEXT_LEFT);
		nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, port_text, sizeof(port_text), nk_filter_decimal);
		strncpy(net_udp_get_my_port_buf(), port_text, 5);
		net_udp_get_my_port_buf()[5] = '\0';

#ifdef USE_TRACKER
		nk_ui_checkbox_ubyte(ctx, "Track this game", &Netgame.Tracker);
#endif
		nk_ui_checkbox_ubyte(ctx, "Retro Protocol (p2p, etc.)", &Netgame.RetroProtocol);

		nk_tree_pop(ctx);
	}
}

void nk_ui_advanced_options(void)
{
	int running = 1;

	nk_ui_init_once();

	while (running)
	{
		if (!nk_ui_frame("Advanced Options", nk_vec2(0.62f, 0.82f), nk_ui_build_advanced_options, &running))
			break;
	}
}

// ==============================
// Hosting setup
// ==============================

struct hosting_ui_state
{
	int running;
	int started;
	int start_requested;
	char slevel[12];
};

static void nk_ui_build_hosting(struct nk_context *ctx, void *userdata)
{
	struct hosting_ui_state *st = (struct hosting_ui_state *)userdata;
	int numplayers_limit;
	int v;

	numplayers_limit = Netgame.gamemode == NETGAME_COOPERATIVE ? 4 : Netgame.max_numobservers ? 7 : 8;
	if (Netgame.max_numplayers > numplayers_limit)
		Netgame.max_numplayers = numplayers_limit;
	if (Netgame.max_numplayers < 2)
		Netgame.max_numplayers = 2;

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 2);
	if (nk_button_label(ctx, "Cancel"))
		st->running = 0;
	if (nk_button_label(ctx, "Start Game") || nk_ui_take_enter())
	{
		if ((Netgame.levelnum < Last_secret_level) || (Netgame.levelnum > Last_level) || (Netgame.levelnum == 0))
		{
			// Level out of range -- old menu popped a blocking messagebox here;
			// just reset silently and let the player notice/re-enter it.
			Netgame.levelnum = 1;
			strcpy(st->slevel, "1");
		}
		else
			st->start_requested = 1;
	}

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	nk_label(ctx, TXT_DESCRIPTION, NK_TEXT_LEFT);
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, Netgame.game_name, NETGAME_NAME_LEN + 1, nk_filter_default);

	{
		char level_label[48];
		snprintf(level_label, sizeof(level_label), "%s (1-%d)", TXT_LEVEL_, Last_level);
		nk_label(ctx, level_label, NK_TEXT_LEFT);
	}
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, st->slevel, sizeof(st->slevel), nk_filter_decimal);
	Netgame.levelnum = atoi(st->slevel);

	if (nk_tree_push(ctx, NK_TREE_TAB, "Game Mode", NK_MINIMIZED))
	{
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_option_label(ctx, TXT_ANARCHY, Netgame.gamemode == NETGAME_ANARCHY || Netgame.gamemode == 0)) { Netgame.gamemode = NETGAME_ANARCHY; }
	if (nk_option_label(ctx, TXT_TEAM_ANARCHY, Netgame.gamemode == NETGAME_TEAM_ANARCHY && !Netgame.CTF)) { Netgame.gamemode = NETGAME_TEAM_ANARCHY; Netgame.CTF = 0; }
	if (nk_option_label(ctx, "Capture the Flag", Netgame.gamemode == NETGAME_TEAM_ANARCHY && Netgame.CTF)) { Netgame.gamemode = NETGAME_TEAM_ANARCHY; Netgame.CTF = 1; }
	if (nk_option_label(ctx, TXT_ANARCHY_W_ROBOTS, Netgame.gamemode == NETGAME_ROBOT_ANARCHY)) { Netgame.gamemode = NETGAME_ROBOT_ANARCHY; }
	if (nk_option_label(ctx, TXT_COOPERATIVE, Netgame.gamemode == NETGAME_COOPERATIVE)) { Netgame.gamemode = NETGAME_COOPERATIVE; }
	if (nk_option_label(ctx, "Bounty", Netgame.gamemode == NETGAME_BOUNTY)) { Netgame.gamemode = NETGAME_BOUNTY; }
	if (nk_option_label(ctx, "Turkey Shoot", Netgame.gamemode == NETGAME_TURKEY_SHOOT)) { Netgame.gamemode = NETGAME_TURKEY_SHOOT; }
	if (nk_option_label(ctx, "Arcade", Netgame.gamemode == NETGAME_ARCADE)) { Netgame.gamemode = NETGAME_ARCADE; Netgame.CTF = 0; }
	if (nk_option_label(ctx, "Survival", Netgame.gamemode == NETGAME_SURVIVAL)) { Netgame.gamemode = NETGAME_SURVIVAL; Netgame.CTF = 0; }
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Access", NK_MINIMIZED))
	{
		int closed = (Netgame.game_flags & NETGAME_FLAG_CLOSED) ? 1 : 0;
		int restricted = Netgame.RefusePlayers ? 1 : 0;
		int open = (!closed && !restricted) ? 1 : 0;

		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
		if (nk_option_label(ctx, "Open game", open)) { Netgame.game_flags &= ~NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 0; }
		if (nk_option_label(ctx, TXT_CLOSED_GAME, closed)) { Netgame.game_flags |= NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 0; }
		if (nk_option_label(ctx, "Restricted Game", restricted)) { Netgame.game_flags &= ~NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 1; }
		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Players & Observers", NK_MINIMIZED))
	{
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	v = Netgame.max_numplayers;
	nk_labelf(ctx, NK_TEXT_LEFT, "Maximum players: %d", Netgame.max_numplayers);
	nk_slider_int(ctx, 2, &v, numplayers_limit, 1);
	Netgame.max_numplayers = (ubyte)v;

	v = Netgame.max_numobservers;
	nk_labelf(ctx, NK_TEXT_LEFT, "Maximum observers: %d", Netgame.max_numobservers);
	nk_slider_int(ctx, 0, &v, MAX_OBSERVERS, 2);
	Netgame.max_numobservers = (ubyte)v;

	nk_ui_checkbox_ubyte(ctx, "Broadcast delay", &Netgame.obs_delay);
	nk_ui_checkbox_ubyte(ctx, "Minimal Observer Info", &Netgame.obs_min);
		nk_tree_pop(ctx);
	}

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
	if (nk_button_label(ctx, "Advanced options"))
		nk_ui_defer(nk_ui_advanced_options);
}

int nk_ui_hosting_setup(void)
{
	struct hosting_ui_state st;

	nk_ui_init_once();

	// Release input grab and show the OS cursor -- same call the legacy
	// newmenu system makes when a menu window opens (see newmenu.c), needed
	// here too since we bypass that system entirely. This is the sole
	// top-level entry point into the Nuklear UI (Advanced Options and the
	// weapon/object toggle screens are always reached from here), so this
	// is the only place that needs to call it.
	event_toggle_focus(0);

	memset(&st, 0, sizeof(st));
	st.running = 1;
	snprintf(st.slevel, sizeof(st.slevel), "%d", Netgame.levelnum > 0 ? Netgame.levelnum : 1);

	while (st.running)
	{
		if (!nk_ui_frame("Host Game", nk_vec2(0.6f, 0.8f), nk_ui_build_hosting, &st))
			break;
		if (st.start_requested)
		{
			st.start_requested = 0;
			if (net_udp_start_game())
			{
				st.started = 1;
				st.running = 0;
			}
		}
	}

	return st.started;
}

// ==============================
// Generic newmenu renderer -- draws any newmenu_item array as a Nuklear
// panel, so every legacy menu can opt in without rewriting its item setup
// or its EVENT_NEWMENU_* handler. See newmenu_draw_nk() in newmenu.c.
// ==============================

static int s_menus_open = 0;

// The click that opened a menu is still in Nuklear's input state when the new
// menu first draws, so it lands on whatever the new menu has under the cursor.
// Buttons are hidden from the new menu until that click has fully finished.
static int s_swallow_click = 0;

void nk_ui_menu_opened(void)
{
	s_menus_open++;
	s_swallow_click = 1;
}

static int nk_ui_click_in_progress(void)
{
	int b;

	for (b = 0; b < NK_BUTTON_MAX; b++)
		if (s_ctx->input.mouse.buttons[b].down || s_ctx->input.mouse.buttons[b].clicked)
			return 1;
	return 0;
}

// Hides mouse buttons from the frame being built; returns 1 when it did, and
// the caller then puts `saved` back into the context.
static int nk_ui_swallow_click(struct nk_input *saved)
{
	if (!s_swallow_click)
		return 0;
	if (!nk_ui_click_in_progress())
	{
		s_swallow_click = 0;
		return 0;
	}
	*saved = s_ctx->input;
	memset(s_ctx->input.mouse.buttons, 0, sizeof(s_ctx->input.mouse.buttons));
	return 1;
}

void nk_ui_menu_closed(void)
{
	if (s_menus_open > 0)
		s_menus_open--;
}

int nk_ui_menus_open(void)
{
	return s_menus_open > 0 && s_initialized;
}

void nk_ui_input_begin(void)
{
	nk_input_begin(s_ctx);
	s_wheel_steps = 0;
	s_arrow_steps = 0;
}

void nk_ui_input_end(void)
{
	nk_input_end(s_ctx);
	s_escape_pressed = 0;
	s_enter_pressed = 0;
}

void nk_ui_feed_event(SDL_Event *evt)
{
	nk_ui_handle_event(evt);
}

void nk_ui_select_radio(newmenu_item *items, int nitems, int chosen)
{
	int i;

	for (i = 0; i < nitems; i++)
	{
		if (items[i].type == NM_TYPE_RADIO && items[i].group == items[chosen].group)
			items[i].value = (i == chosen);
	}
}

#define NK_UI_OFFSCREEN -10000.0f

// NK_WINDOW_NO_INPUT doesn't stop widgets reacting, so a panel that must be
// inert gets a blank input state for its frame. Without this a click that
// opened a nested screen re-fired the button underneath it on the nested
// screen's first redraw, recursing until the stack ran out.
// Only the front menu is shown. A panel underneath must still be built every
// frame -- nk_clear() frees any window that was not -- so it keeps its scroll
// and folded sections by being built off the edge of the screen instead.
static struct nk_rect nk_ui_hide_if_muted(struct nk_rect bounds, int muted)
{
	if (muted)
		bounds.y = NK_UI_OFFSCREEN;
	return bounds;
}

static struct nk_input nk_ui_mute_input(void)
{
	struct nk_input saved = s_ctx->input;

	memset(&s_ctx->input, 0, sizeof(s_ctx->input));
	s_ctx->input.mouse.pos = nk_vec2(NK_UI_OFFSCREEN, NK_UI_OFFSCREEN);
	s_ctx->input.mouse.prev = s_ctx->input.mouse.pos;
	return saved;
}

#define NK_UI_HEADING_MAX_LEN 40

// A subtitle short enough for one line doubles as the title bar text when the
// menu has no title (e.g. "Options"); longer text is a message body.
static int nk_ui_subtitle_is_heading(const char *title, const char *subtitle)
{
	return !title && subtitle && !strchr(subtitle, '\n') && strlen(subtitle) <= NK_UI_HEADING_MAX_LEN;
}

// Copies into `out` the longest prefix of `text` that fits `avail` pixels,
// breaking at a space where one is available, and returns where the next
// line starts (NULL once the text is exhausted).
static const char *nk_ui_wrap_next(const struct nk_ui_font *font, const char *text, float avail, char *out, size_t out_size)
{
	const char *newline = strchr(text, '\n');
	const char *limit = newline ? newline : text + strlen(text);
	const char *fit = text, *space = NULL, *c;
	float width = 0.0f;

	for (c = text; c < limit; c++)
	{
		width += nk_ui_font_text_width(nk_handle_ptr((void *)font), 0.0f, c, 1);
		if (width > avail && fit > text)
			break;
		if (*c == ' ')
			space = c;
		fit = c + 1;
	}
	if (c < limit && space)
		fit = space;
	snprintf(out, out_size, "%.*s", (int)(fit - text), text);

	while (*fit == ' ')
		fit++;
	if (fit < limit)
		return fit;
	return newline ? newline + 1 : NULL;
}

static int nk_ui_count_wrapped(const struct nk_ui_font *font, const char *text, float avail)
{
	char line[NM_MAX_TEXT_LEN + 1];
	int lines = 0;

	while (text && *text)
	{
		text = nk_ui_wrap_next(font, text, avail, line, sizeof(line));
		lines++;
	}
	return lines ? lines : 1;
}

static void nk_ui_wrapped_label(struct nk_context *ctx, const char *text, float avail, nk_flags align)
{
	char line[NM_MAX_TEXT_LEN + 1];

	while (text && *text)
	{
		text = nk_ui_wrap_next(&s_body_font, text, avail, line, sizeof(line));
		nk_label(ctx, line, align);
	}
}

static void nk_ui_message_lines(struct nk_context *ctx, const char *text)
{
	nk_ui_wrapped_label(ctx, text, nk_window_get_content_region(ctx).w, NK_TEXT_CENTERED);
}

// A panel title too wide for the title bar is wrapped into the panel
// instead, and keeps the heading font and colour while it is there.
static void nk_ui_title_lines(struct nk_context *ctx, const char *text)
{
	const struct nk_ui_font *font = s_head_font.ready ? &s_head_font : &s_body_font;
	float avail = nk_window_get_content_region(ctx).w;
	char line[NM_MAX_TEXT_LEN + 1];

	if (s_head_font.ready)
		nk_style_push_font(ctx, &s_head_font.handle);
	nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_TITLE_TEXT);
	while (text && *text)
	{
		text = nk_ui_wrap_next(font, text, avail, line, sizeof(line));
		nk_label(ctx, line, NK_TEXT_CENTERED);
	}
	nk_style_pop_color(ctx);
	if (s_head_font.ready)
		nk_style_pop_font(ctx);
}

#define NK_UI_MAX_COLUMNS 12
#define NK_UI_CELL_GAP_ROWS 0.6f
// How far a reference row's description leans toward the accent, of 256.
#define NK_UI_REFERENCE_TINT 90

// Column layout for the table screens (netgame browser, mission browser),
// whose rows are tab separated. Measured from every row each frame so the
// columns line up and each is only as wide as its widest cell needs.
static float s_col_w[NK_UI_MAX_COLUMNS];
static int s_col_count;

// Copies one tab-separated cell into `out`, dropping the colour-escape
// control bytes the browsers embed, and returns the next cell (or NULL).
static const char *nk_ui_cell(const char *text, char *out, size_t out_size)
{
	size_t len = 0;

	for (; *text && *text != '\t'; text++)
		if ((unsigned char)*text >= ' ' && len + 1 < out_size)
			out[len++] = *text;
	out[len] = '\0';
	return *text == '\t' ? text + 1 : NULL;
}

// Natural width of each column across every table row; returns their total.
static float nk_ui_measure_table(const newmenu_item *items, int nitems)
{
	char cell[NM_MAX_TEXT_LEN + 1];
	float gap = s_row_h * NK_UI_CELL_GAP_ROWS;
	float total = 0.0f;
	int i, col;

	memset(s_col_w, 0, sizeof(s_col_w));
	s_col_count = 0;
	for (i = 0; i < nitems; i++)
	{
		const char *rest = items[i].text;

		if (!rest || !strchr(rest, '\t'))
			continue;
		for (col = 0; rest && col < NK_UI_MAX_COLUMNS; col++)
		{
			float width;

			rest = nk_ui_cell(rest, cell, sizeof(cell));
			width = nk_ui_text_width(&s_body_font, cell) + gap;
			if (width > s_col_w[col])
				s_col_w[col] = width;
			if (col >= s_col_count)
				s_col_count = col + 1;
		}
	}
	for (col = 0; col < s_col_count; col++)
		total += s_col_w[col];
	return total;
}

// Shrinks the columns to fit `avail`, or gives the slack to the widest one.
static void nk_ui_fit_table(float avail)
{
	float total = 0.0f;
	int col, widest = 0;

	if (!s_col_count)
		return;
	for (col = 0; col < s_col_count; col++)
	{
		total += s_col_w[col];
		if (s_col_w[col] > s_col_w[widest])
			widest = col;
	}
	if (total > avail && total > 0.0f)
	{
		for (col = 0; col < s_col_count; col++)
			s_col_w[col] *= avail / total;
	}
	else
		s_col_w[widest] += avail - total;
}

// One table row: a single full-width hit target with its cells drawn at the
// shared column positions, so rows align no matter how wide a field is.
static int nk_ui_table_row(struct nk_context *ctx, const char *text, int is_button)
{
	struct nk_rect bounds = nk_widget_bounds(ctx);
	struct nk_command_buffer *canvas = nk_window_get_canvas(ctx);
	struct nk_rect clip = nk_window_get_content_region(ctx);
	struct nk_color color = ctx->style.text.color;
	struct nk_color first_col = color;
	char cell[NM_MAX_TEXT_LEN + 1];
	const char *rest = text;
	float pad = s_row_h * NK_UI_CELL_GAP_ROWS * 0.5f;
	float x = bounds.x;
	// A row with no tab is not a table row: it gets the whole width rather
	// than column 0's, which the header may well have measured as empty.
	int plain = !strchr(text, '\t');
	int clicked = 0, col;

	if (is_button)
	{
		clicked = nk_button_label(ctx, "");
		if (nk_input_is_mouse_hovering_rect(&ctx->input, bounds))
			color = NK_UI_ACCENT_BRIGHT;
	}
	else
	{
		// A reference row reads as "key -> what it does". The key takes the
		// base accent, a step below the brighter heading above it, and the
		// description is washed toward the accent rather than left plain
		// grey, so the three levels stay apart without shouting.
		nk_label(ctx, "", NK_TEXT_LEFT);
		first_col = NK_UI_ACCENT;
		color = nk_ui_mix(color, NK_UI_ACCENT, NK_UI_REFERENCE_TINT);
	}

	for (col = 0; rest && col < s_col_count; col++)
	{
		struct nk_rect cell_rect;

		rest = nk_ui_cell(rest, cell, sizeof(cell));
		cell_rect = nk_rect(x + pad, bounds.y + (bounds.h - s_body_font.handle.height) * 0.5f,
			(plain ? bounds.w : s_col_w[col]) - pad, s_body_font.handle.height);
		x += s_col_w[col];
		if (!cell[0])
			continue;
		// Drawing straight to the canvas bypasses the panel's own clipping, so
		// a row scrolled past the edge would still paint over the border.
		nk_unify(&cell_rect, &clip, cell_rect.x, cell_rect.y,
			cell_rect.x + cell_rect.w, cell_rect.y + cell_rect.h);
		if (cell_rect.w <= 0.0f || cell_rect.h <= 0.0f)
			continue;
		nk_push_scissor(canvas, cell_rect);
		nk_draw_text(canvas, cell_rect, cell, (int)strlen(cell), &s_body_font.handle, nk_rgba(0, 0, 0, 0),
			col ? color : first_col);
	}
	nk_push_scissor(canvas, clip);
	return clicked;
}

#define NK_UI_COMBO_MIN_OPTIONS 3

// One bit per section heading: set means that heading's items are hidden.
// Owned by the menu the same way.
static unsigned int *s_collapsed;
// Panel and item the view was last scrolled to, so arrow steps stay followed.
static const void *s_reveal_id;
static int s_reveal_citem = -1;

#define NK_UI_MAX_SECTIONS 32

static unsigned int nk_ui_collapsed_mask(void)
{
	return s_collapsed ? *s_collapsed : 0u;
}

static int nk_ui_is_heading(const char *text);

// A heading row is an all-caps text row with no tab in it. The tab matters:
// a table's own header row ("NAME\tMISSION\t...") is also all caps, and
// treating that as a heading would fold the whole list away.
static int nk_ui_is_section_head(const newmenu_item *item)
{
	return item->type == NM_TYPE_TEXT && item->text && item->text[0]
		&& !strchr(item->text, '\t') && nk_ui_is_heading(item->text);
}

// Set by newmenu.c around the menus whose headings are plain labels.
static int s_fixed_sections = 0;

void nk_ui_set_fixed_sections(int fixed)
{
	s_fixed_sections = fixed;
}

// A heading with nothing under it has nothing to collapse.
int nk_ui_is_collapsible_head(const newmenu_item *items, int nitems, int index)
{
	if (s_fixed_sections)
		return 0;
	return nk_ui_is_section_head(&items[index])
		&& index + 1 < nitems && !nk_ui_is_section_head(&items[index + 1]);
}

// Which heading owns this row, counting from 0, or -1 above the first one.
static int nk_ui_section_of(const newmenu_item *items, int nitems, int index)
{
	int section = -1, i;

	for (i = 0; i <= index && i < nitems; i++)
		if (nk_ui_is_section_head(&items[i]))
			section++;
	return section < NK_UI_MAX_SECTIONS ? section : -1;
}

void nk_ui_toggle_section(const newmenu_item *items, int nitems, int index, unsigned int *collapsed)
{
	int section = nk_ui_section_of(items, nitems, index);

	if (section >= 0 && collapsed)
		*collapsed ^= 1u << section;
}

// Every section folded: what a menu looks like before the player has
// touched it, so it opens as a short list of headings.
static unsigned int nk_ui_all_sections_collapsed(const newmenu_item *items, int nitems)
{
	unsigned int mask = 0;
	int section = 0, i;

	for (i = 0; i < nitems && section < NK_UI_MAX_SECTIONS; i++)
	{
		if (!nk_ui_is_section_head(&items[i]))
			continue;
		if (nk_ui_is_collapsible_head(items, nitems, i))
			mask |= 1u << section;
		section++;
	}
	return mask;
}

// Which sections each menu was left folded in, kept by title for the run of
// the game. The table is small and wraps when full; losing the oldest entry
// only costs one menu its remembered state.
#define NK_UI_SECTION_MEMORY 24
#define NK_UI_SECTION_KEY_LEN 48

static struct
{
	char title[NK_UI_SECTION_KEY_LEN];
	unsigned int mask;
} s_section_memory[NK_UI_SECTION_MEMORY];
static int s_section_memory_used;

int nk_ui_has_sections(const newmenu_item *items, int nitems)
{
	int i;

	for (i = 0; i < nitems; i++)
		if (nk_ui_is_collapsible_head(items, nitems, i))
			return 1;
	return 0;
}

unsigned int nk_ui_recall_sections(const char *title, const newmenu_item *items, int nitems)
{
	int i;

	if (title && title[0])
		for (i = 0; i < s_section_memory_used; i++)
			if (!strcmp(s_section_memory[i].title, title))
				return s_section_memory[i].mask;
	return nk_ui_all_sections_collapsed(items, nitems);
}

void nk_ui_remember_sections(const char *title, unsigned int mask)
{
	int i;

	if (!title || !title[0])
		return;
	for (i = 0; i < s_section_memory_used; i++)
		if (!strcmp(s_section_memory[i].title, title))
		{
			s_section_memory[i].mask = mask;
			return;
		}
	if (s_section_memory_used == NK_UI_SECTION_MEMORY)
		s_section_memory_used = 0;
	snprintf(s_section_memory[s_section_memory_used].title, NK_UI_SECTION_KEY_LEN, "%s", title);
	s_section_memory[s_section_memory_used++].mask = mask;
}

static int nk_ui_section_is_collapsed(const newmenu_item *items, int nitems, int index, unsigned int collapsed)
{
	int section;

	if (nk_ui_is_section_head(&items[index]))
		return 0;	// the heading itself stays put
	section = nk_ui_section_of(items, nitems, index);
	return section >= 0 && (collapsed >> section) & 1u;
}

// A run of radio buttons is one choice, so it is drawn as a single drop-down
// row rather than one row per option: a 50-entry resolution list would
// otherwise bury everything else in the menu.
int nk_ui_radio_run_start(const newmenu_item *items, int index)
{
	while (index > 0 && items[index - 1].type == NM_TYPE_RADIO && items[index - 1].group == items[index].group)
		index--;
	return index;
}

int nk_ui_radio_run_end(const newmenu_item *items, int nitems, int index)
{
	int end = index + 1;

	while (end < nitems && items[end].type == NM_TYPE_RADIO && items[end].group == items[index].group)
		end++;
	return end;
}

// True while a row is not drawn at all: inside a folded section, or one of
// the options a long run hides behind its single summary row.
int nk_ui_item_is_folded(const newmenu_item *items, int nitems, int index, unsigned int collapsed)
{
	int start;

	if (nk_ui_section_is_collapsed(items, nitems, index, collapsed))
		return 1;
	if (items[index].type != NM_TYPE_RADIO)
		return 0;
	start = nk_ui_radio_run_start(items, index);
	if (index == start)
		return 0;
	return nk_ui_radio_run_end(items, nitems, start) - start >= NK_UI_COMBO_MIN_OPTIONS;
}

// The option currently set in a run, or -1 when the choice sits elsewhere in
// the same group (the resolution menu's "use custom values").
int nk_ui_radio_chosen(const newmenu_item *items, int start, int end)
{
	int i;

	for (i = start; i < end; i++)
		if (items[i].value)
			return i;
	return -1;
}

void nk_ui_swap_items(newmenu_item *items, int a, int b)
{
	char *text = items[a].text;
	int value = items[a].value;

	items[a].text = items[b].text;
	items[a].value = items[b].value;
	items[b].text = text;
	items[b].value = value;
}

#define NK_UI_REORDER_BUTTON_ROWS 2

// A priority list (the weapon autoselect menus): each row carries the
// buttons that move it, so the order can be changed with the mouse as well
// as with the arrow keys.
static int nk_ui_reorder_row(struct nk_context *ctx, newmenu_item *items, int nitems, int index)
{
	float button_w = (float)(s_row_h * NK_UI_REORDER_BUTTON_ROWS);
	float label_w = nk_window_get_content_region(ctx).w - button_w * 2 - ctx->style.window.spacing.x * 3;
	int moved = 0;

	nk_layout_row_begin(ctx, NK_STATIC, (float)s_row_h, 3);
	nk_layout_row_push(ctx, button_w);
	if (nk_button_label(ctx, "UP") && index > 0)
	{
		nk_ui_swap_items(items, index, index - 1);
		moved = 1;
	}
	nk_layout_row_push(ctx, button_w);
	if (nk_button_label(ctx, "DOWN") && index + 1 < nitems)
	{
		nk_ui_swap_items(items, index, index + 1);
		moved = 1;
	}
	nk_layout_row_push(ctx, label_w > 0.0f ? label_w : button_w);
	nk_label(ctx, items[index].text, NK_TEXT_LEFT);
	nk_layout_row_end(ctx);
	nk_layout_row_dynamic(ctx, (float)s_row_h, 1);
	return moved;
}

// Set by the drop-down to the option it picked, so the menu reports the
// option's own index to its handler rather than the row's.
static int s_radio_pick = -1;

// The one row a long option run gets: it shows what is set and opens a list
// of its own to change it, so the menu around it never grows.
static int nk_ui_option_run_row(struct nk_context *ctx, const newmenu_item *items, int start, int end)
{
	int chosen = nk_ui_radio_chosen(items, start, end);
	char label[NM_MAX_TEXT_LEN + 4];

	snprintf(label, sizeof(label), "%s  >", chosen < 0 ? "---" : items[chosen].text);
	return nk_button_label(ctx, label);
}

// A text row in capitals is a section heading; the accent colour makes the
// groups easy to pick apart. A digit rules one out: a value readout like
// "(200 FPS)" is all caps too, and folding the rows under it hid them.
static int nk_ui_is_heading(const char *text)
{
	int capitals = 0;
	const char *c;

	for (c = text; *c; c++)
	{
		if (*c >= 'a' && *c <= 'z')
			return 0;
		if (*c >= '0' && *c <= '9')
			return 0;
		if (*c >= 'A' && *c <= 'Z')
			capitals++;
	}
	return capitals > 0;
}

// A heading that owns rows is a flat accent-coloured button: clicking it
// folds its section away, so a long menu can be cut down to the part in hand.
static int nk_ui_section_head_row(struct nk_context *ctx, const char *text, int collapsed)
{
	struct nk_style_button saved = ctx->style.button;
	char label[NM_MAX_TEXT_LEN + 8];
	int clicked;

	snprintf(label, sizeof(label), "%s  %s", collapsed ? "+" : "-", text);
	if (s_head_font.ready)
		nk_style_push_font(ctx, &s_head_font.handle);
	ctx->style.button.normal = nk_style_item_color(nk_rgba(0, 0, 0, 0));
	ctx->style.button.hover = nk_style_item_color(nk_ui_shade(24));
	ctx->style.button.active = nk_style_item_color(nk_ui_shade(36));
	ctx->style.button.border = 0.0f;
	ctx->style.button.text_normal = NK_UI_ACCENT_BRIGHT;
	ctx->style.button.text_hover = NK_UI_ACCENT_BRIGHT;
	ctx->style.button.text_active = NK_UI_ACCENT_BRIGHT;
	ctx->style.button.text_alignment = NK_TEXT_LEFT;
	clicked = nk_button_label(ctx, label);
	ctx->style.button = saved;
	if (s_head_font.ready)
		nk_style_pop_font(ctx);
	return clicked;
}

// Returns 1 if the player changed this item's value this frame.
static int nk_ui_menu_item_widget(struct nk_context *ctx, newmenu_item *items, int nitems, int index, int *selected, int focus_input);

#define NK_UI_CURRENT_ITEM_THICKNESS 2.0f

// Wraps the widget so the keyboard-selected item gets an outline.
// Extra space to pull into view above a row: the heading that labels it is
// a row of its own, and stopping at the row itself leaves it clipped.
static float nk_ui_reveal_lead(const newmenu_item *items, int nitems, int index)
{
	unsigned int collapsed = nk_ui_collapsed_mask();
	int i;

	for (i = index - 1; i >= 0; i--)
	{
		if (nk_ui_item_is_folded(items, nitems, i, collapsed))
			continue;
		if (!nk_ui_is_section_head(&items[i]))
			return 0.0f;
		return s_row_h + s_ctx->style.window.spacing.y + (float)(s_row_h / NK_UI_HEAD_LEAD_DIVISOR);
	}
	return 0.0f;
}

static void nk_ui_reveal_rect(struct nk_context *ctx, struct nk_rect item, float lead)
{
	struct nk_rect view = nk_window_get_content_region(ctx);
	nk_uint scroll_x, scroll_y;
	float shift = 0.0f;

	if (item.y - lead < view.y)
		shift = item.y - lead - view.y;
	else if (item.y + item.h > view.y + view.h)
		shift = (item.y + item.h) - (view.y + view.h);
	if (shift == 0.0f)
		return;

	nk_window_get_scroll(ctx, &scroll_x, &scroll_y);
	shift += (float)scroll_y;
	nk_window_set_scroll(ctx, scroll_x, (nk_uint)(shift < 0.0f ? 0.0f : shift));
}

static int nk_ui_menu_item(struct nk_context *ctx, newmenu_item *items, int nitems, int index, int *selected, int focus_input, int is_current, int reveal)
{
	struct nk_rect bounds = nk_widget_bounds(ctx);
	int changed = nk_ui_menu_item_widget(ctx, items, nitems, index, selected, focus_input);

	if (is_current && reveal)
		nk_ui_reveal_rect(ctx, bounds, nk_ui_reveal_lead(items, nitems, index));

	if (is_current && (items[index].type != NM_TYPE_TEXT || nk_ui_is_collapsible_head(items, nitems, index)))
		nk_stroke_rect(nk_window_get_canvas(ctx), bounds, 0, NK_UI_CURRENT_ITEM_THICKNESS, NK_UI_ACCENT_BRIGHT);
	return changed;
}

static int nk_ui_menu_item_widget(struct nk_context *ctx, newmenu_item *items, int nitems, int index, int *selected, int focus_input)
{
	newmenu_item *item = &items[index];
	int value = item->value;
	nk_bool checked;

	switch (item->type)
	{
		case NM_TYPE_TEXT:
			if (!item->text[0])
				nk_spacing(ctx, 1);
			else if (strchr(item->text, '\n'))
				nk_ui_message_lines(ctx, item->text);
			else if (nk_ui_is_collapsible_head(items, nitems, index))
			{
				if (nk_ui_section_head_row(ctx, item->text, nk_ui_section_is_collapsed(items, nitems, index + 1, nk_ui_collapsed_mask())))
					nk_ui_toggle_section(items, nitems, index, s_collapsed);
			}
			else if (s_col_count)
				nk_ui_table_row(ctx, item->text, 0);
			else if (nk_ui_is_heading(item->text))
			{
				if (s_head_font.ready)
					nk_style_push_font(ctx, &s_head_font.handle);
				nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_ACCENT_BRIGHT);
				nk_label(ctx, item->text, NK_TEXT_LEFT);
				nk_style_pop_color(ctx);
				if (s_head_font.ready)
					nk_style_pop_font(ctx);
			}
			else
				nk_label(ctx, item->text, NK_TEXT_LEFT);
			return 0;

		case NM_TYPE_MENU:
			// Every row of a table screen goes through the column layout,
			// tabs or not: a row that is only its number (an empty netgame
			// slot) would otherwise centre that number as a button label.
			if (s_col_count)
			{
				if (nk_ui_table_row(ctx, item->text, 1))
					*selected = index;
			}
			else if (nk_button_label(ctx, item->text))
				*selected = index;
			return 0;

		case NM_TYPE_CHECK:
			checked = value ? nk_true : nk_false;
			nk_checkbox_label(ctx, item->text, &checked);
			item->value = checked ? 1 : 0;
			return item->value != value;

		case NM_TYPE_RADIO:
		{
			int start = nk_ui_radio_run_start(items, index);
			int end = nk_ui_radio_run_end(items, nitems, start);

			if (end - start >= NK_UI_COMBO_MIN_OPTIONS)
			{
				if (nk_ui_option_run_row(ctx, items, start, end))
					*selected = index;
				return 0;
			}
			if (nk_option_label(ctx, item->text, value == 1) && value != 1)
			{
				nk_ui_select_radio(items, nitems, index);
				return 1;
			}
			return 0;
		}

		case NM_TYPE_SLIDER:
			nk_label(ctx, item->text, NK_TEXT_LEFT);
			nk_slider_int(ctx, item->min_value, &item->value, item->max_value, 1);
			return item->value != value;

		case NM_TYPE_NUMBER:
			nk_property_int(ctx, item->text, item->min_value, &item->value, item->max_value, 1, 1);
			return item->value != value;

		case NM_TYPE_INPUT:
		case NM_TYPE_INPUT_MENU:
		{
			char before[NM_MAX_TEXT_LEN + 1];

			snprintf(before, sizeof(before), "%s", item->text);
			if (focus_input)
				nk_edit_focus(ctx, NK_EDIT_FIELD);
			nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, item->text, item->text_len + 1, nk_filter_default);
			return strcmp(before, item->text) != 0;
		}
	}
	return 0;
}

// True for a menu that only takes you somewhere else -- the main menu, the
// multiplayer menu, a message box's buttons. Those read as navigation and
// keep the larger font; anything with a setting in it is body text.
static int nk_ui_is_navigation(const newmenu_item *items, int nitems)
{
	int destinations = 0, i;

	for (i = 0; i < nitems; i++)
	{
		if (items[i].text && strchr(items[i].text, '\t'))
			return 0;
		if (items[i].type == NM_TYPE_TEXT)
			continue;
		if (items[i].type != NM_TYPE_MENU)
			return 0;
		destinations++;
	}
	return destinations > 0;
}

static float nk_ui_menu_width(int screen_width, int is_table)
{
	float wanted = screen_width * (is_table ? NK_UI_TABLE_WIDTH_FRAC : NK_UI_MENU_WIDTH_FRAC);
	float least = (float)(NK_UI_ROW_HEIGHT * NK_UI_MENU_MIN_WIDTH_ROWS);
	float limit = screen_width * NK_UI_MENU_MAX_HEIGHT_FRAC;

	if (wanted < least)
		wanted = least;
	return wanted < limit ? wanted : limit;
}

// Grows `base` until the heading, message body and every item fit inside the
// window padding and scroll bar, up to most of the screen.
static float nk_ui_fit_width(float base, int screen_width, const char *heading, const char *body, const newmenu_item *items, int nitems)
{
	float chrome = s_ctx->style.window.padding.x * 2 + s_ctx->style.window.border * 2 + s_row_h * NK_UI_SCROLLBAR_ROWS;
	float need = 0.0f, item_need;
	int i;

	if (heading && s_title_font.ready)
		need = nk_ui_text_width(&s_title_font, heading) / s_title_font.scale + s_ctx->style.window.padding.x * 4;
	if (body)
		need = max(need, nk_ui_text_width(&s_body_font, body) + chrome);
	for (i = 0; i < nitems; i++)
	{
		item_need = nk_ui_item_width(&items[i]) + chrome;
		need = max(need, item_need);
	}
	return min(max(base, need), screen_width * NK_UI_MAX_SCREEN_FRAC);
}

static float nk_ui_menu_height_px(int nrows, float extra_px, int screen_height)
{
	float row_stride = NK_UI_ROW_HEIGHT + s_ctx->style.window.spacing.y;
	float wanted = nrows * row_stride + extra_px + NK_UI_MENU_CHROME_HEIGHT;
	float limit = screen_height * NK_UI_MENU_MAX_HEIGHT_FRAC;

	return wanted < limit ? wanted : limit;
}

static float nk_ui_menu_height(int nrows, int screen_height)
{
	return nk_ui_menu_height_px(nrows, 0.0f, screen_height);
}

#define NK_UI_MAX_PANEL_COLUMNS 1
// Below this a column is too stubby to be worth splitting into.
#define NK_UI_MIN_COLUMN_ROWS 8

// Rows one item draws into. A slider is a label row plus a bar row, and a
// text row wraps; getting this wrong clips the bottom of a column.
// `content_w` of 0 measures without wrapping, for callers sizing the panel.
static int nk_ui_item_rows(const newmenu_item *item, float content_w)
{
	if (item->type == NM_TYPE_SLIDER)
		return 2;
	if (nk_ui_is_section_head(item))
		return NK_UI_HEAD_ROW_ROWS;
	// A tabbed row is drawn as one table row, never wrapped.
	if (item->type == NM_TYPE_TEXT && item->text[0] && content_w > 0.0f
		&& !strchr(item->text, '\t'))
		return nk_ui_count_wrapped(&s_body_font, item->text, content_w);
	return 1;
}

// A short gap under a heading whose section is open, sized in pixels so it
// stays a fraction of a row rather than a whole one.
#define NK_UI_HEAD_GAP_DIVISOR 16

static int nk_ui_head_has_gap(const newmenu_item *items, int nitems, int index)
{
	if (!nk_ui_is_section_head(&items[index]) || index + 1 >= nitems)
		return 0;
	return !nk_ui_item_is_folded(items, nitems, index + 1, nk_ui_collapsed_mask());
}

// A menu taller than the screen is laid out side by side instead of made to
// scroll: wide beats tall. Breaks fall on section headings so a group never
// straddles two columns. Fills col_start[0..ncols] with item indices.
static int nk_ui_plan_columns(const newmenu_item *items, int nitems, int rows_per_col, int *col_start)
{
	unsigned int collapsed = nk_ui_collapsed_mask();
		int nrows = 0, ncols, target, used = 0, col = 1, i;

	for (i = 0; i < nitems; i++)
		if (!nk_ui_item_is_folded(items, nitems, i, collapsed))
			nrows += nk_ui_item_rows(&items[i], 0.0f);

	col_start[0] = 0;
	col_start[1] = nitems;
	if (rows_per_col < NK_UI_MIN_COLUMN_ROWS || nrows <= rows_per_col)
		return 1;

	ncols = (nrows + rows_per_col - 1) / rows_per_col;
	if (ncols > NK_UI_MAX_PANEL_COLUMNS)
		ncols = NK_UI_MAX_PANEL_COLUMNS;
	// A split panel has no scrollbar, so only split when every row still
	// fits; otherwise stay one column and let it scroll.
	if (nrows > ncols * rows_per_col)
		return 1;
	target = (nrows + ncols - 1) / ncols;

	for (i = 0; i < nitems && col < ncols; i++)
	{
		if (nk_ui_item_is_folded(items, nitems, i, collapsed))
			continue;
		if (used && (nk_ui_is_section_head(&items[i]) ? used >= target : used >= rows_per_col))
		{
			col_start[col++] = i;
			used = 0;
		}
		used += nk_ui_item_rows(&items[i], 0.0f);
	}
	while (col <= ncols)
		col_start[col++] = nitems;
	return ncols;
}

// Rows a column spends on `items[first..last)`.
static int nk_ui_column_rows(const newmenu_item *items, int nitems, int first, int last, float content_w)
{
	unsigned int collapsed = nk_ui_collapsed_mask();
		int rows = 0, i;

	for (i = first; i < last; i++)
		if (!nk_ui_item_is_folded(items, nitems, i, collapsed))
			rows += nk_ui_item_rows(&items[i], content_w);
	return rows;
}

// Pixels a column spends on the gaps under its open headings.
static float nk_ui_column_gap_px(const newmenu_item *items, int nitems, int first, int last)
{
	unsigned int collapsed = nk_ui_collapsed_mask();
	float spacing = s_ctx->style.window.spacing.y;
	float lead = s_row_h / NK_UI_HEAD_LEAD_DIVISOR + spacing;
	float trail = s_row_h / NK_UI_HEAD_GAP_DIVISOR + spacing;
	float total = 0.0f;
	int i;

	for (i = first; i < last; i++)
	{
		if (nk_ui_item_is_folded(items, nitems, i, collapsed) || !nk_ui_is_section_head(&items[i]))
			continue;
		if (i > first)
			total += lead;
		if (nk_ui_head_has_gap(items, nitems, i))
			total += trail;
	}
	return total;
}

struct nk_ui_menu_draw
{
	int reveal;	// scroll the view to the current item this frame
	newmenu_item *items;
	int nitems, citem, refocus, reorder, muted, focus_first_input, first_input;
	int *changed, *selected;
};

// A blank row of `height` pixels; Nuklear resumes at the normal row height.
static void nk_ui_gap_row(struct nk_context *ctx, int height)
{
	nk_layout_row_dynamic(ctx, (float)height, 1);
	nk_spacing(ctx, 1);
	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
}

// Draws items[first..last) as ordinary rows; stops early once one is chosen.
static void nk_ui_draw_items(struct nk_context *ctx, struct nk_ui_menu_draw *d, int first, int last)
{
	int i;

	for (i = first; i < last; i++)
	{
		if (d->reorder)
		{
			struct nk_rect row = nk_widget_bounds(ctx);

			if (nk_ui_reorder_row(ctx, d->items, d->nitems, i) && *d->changed < 0)
				*d->changed = i;
			if (i == d->citem && !d->muted)
				nk_stroke_rect(nk_window_get_canvas(ctx), row, 0, NK_UI_CURRENT_ITEM_THICKNESS, NK_UI_ACCENT_BRIGHT);
			continue;
		}
		if (nk_ui_item_is_folded(d->items, d->nitems, i, nk_ui_collapsed_mask()))
			continue;
		// Not on the first row: the layout call would skip a whole unused row.
		if (i > first && nk_ui_is_section_head(&d->items[i]))
			nk_ui_gap_row(ctx, s_row_h / NK_UI_HEAD_LEAD_DIVISOR);
		s_radio_pick = -1;
		// Focus is claimed only on the frame the keyboard lands on a field.
		// Claiming it every frame stole every keystroke for the first one.
		if (nk_ui_menu_item(ctx, d->items, d->nitems, i, d->selected,
				d->refocus && (i == d->citem || (d->focus_first_input && i == d->first_input)),
				i == d->citem && !d->muted, d->reveal) && *d->changed < 0)
			*d->changed = s_radio_pick >= 0 ? s_radio_pick : i;
		if (*d->selected >= 0 && *d->selected == i)
			return;
		// A heading with its section open gets air under it; a folded one
		// would only stack its gap onto the next heading's leading blank.
		if (nk_ui_head_has_gap(d->items, d->nitems, i))
			nk_ui_gap_row(ctx, s_row_h / NK_UI_HEAD_GAP_DIVISOR);
	}
}

// One frame of a newmenu panel, drawn over whatever the game already drew.
// Reports at most one interaction per frame through *changed / *selected
// (item index, or -1); the caller turns those into EVENT_NEWMENU_* events.
// Only the front window takes input, so a panel stacked under another one
// can't be clicked through. `id` keeps each menu's Nuklear window distinct.
void nk_ui_newmenu_frame(const void *id, const char *title, const char *subtitle, newmenu_item *items, int nitems, int citem, int refocus, int reorder, unsigned int *collapsed, int has_focus, int *changed, int *selected)
{
	int width = grd_curscreen->sc_w, height = grd_curscreen->sc_h;
	int heading_is_subtitle = nk_ui_subtitle_is_heading(title, subtitle);
	const char *body = heading_is_subtitle ? NULL : subtitle;
	const char *heading = title ? title : (heading_is_subtitle ? subtitle : NULL);
	float row_stride;
	int head_rows = 0, tall_rows, rows_per_col;
	float tall_gap_px;
	int heading_wraps = 0;
	float content_w, table_w, header_w;
	float panel_w, panel_h, column_h;
	int col_start[NK_UI_MAX_PANEL_COLUMNS + 1];
	int ncols = 1;
	int has_table = 0, table_picks = 0, wide_table = 0;
	nk_flags flags = NK_WINDOW_BORDER;
	char name[24];
	struct nk_input saved_input;
	int swallowed = 0;
	struct nk_ui_menu_draw draw;
	int muted;
	int i, c;

	*changed = -1;
	*selected = -1;
	s_collapsed = collapsed;
	nk_ui_init_once();
	nk_ui_sync_fonts();
	nk_ui_claim_panel(id);

	draw.items = items;
	draw.nitems = nitems;
	draw.citem = citem;
	draw.refocus = refocus;
	draw.reorder = reorder;
	draw.changed = changed;
	draw.selected = selected;
	draw.first_input = -1;
	draw.reveal = 0;
	draw.focus_first_input = has_focus && !s_drawing_underneath;

	for (i = 0; i < nitems; i++)
	{
		if (items[i].text && strchr(items[i].text, '\t'))
		{
			has_table = 1;
			if (items[i].type != NM_TYPE_TEXT)
				table_picks = 1;
		}
		if (items[i].type == NM_TYPE_MENU)
			draw.focus_first_input = 0;
		else if (items[i].type == NM_TYPE_INPUT && draw.first_input < 0)
			draw.first_input = i;
	}

	row_stride = NK_UI_ROW_HEIGHT + s_ctx->style.window.spacing.y;
	s_item_font = (!reorder && s_head_font.ready && nk_ui_is_navigation(items, nitems))
		? &s_head_font : &s_body_font;
	table_w = nk_ui_measure_table(items, nitems);
	panel_w = nk_ui_menu_width(width, has_table);
	panel_w = nk_ui_fit_width(panel_w, width, heading, subtitle, items, nitems);
	if (table_w > 0.0f)
		panel_w = min(max(panel_w, table_w + s_ctx->style.window.padding.x * 2 + s_row_h * NK_UI_SCROLLBAR_ROWS), width * NK_UI_MAX_SCREEN_FRAC);
	header_w = panel_w - s_ctx->style.window.padding.x * 2 - s_row_h * NK_UI_SCROLLBAR_ROWS;

	// A heading too long for the title bar even in the small font becomes
	// wrapped rows inside the panel instead of running off its edge.
	heading_wraps = heading && heading[0]
		&& nk_ui_text_width(s_head_font.ready ? &s_head_font : &s_body_font, heading) > header_w;
	if (heading_wraps)
		head_rows += nk_ui_count_wrapped(s_head_font.ready ? &s_head_font : &s_body_font, heading, header_w);
	if (body)
		head_rows += nk_ui_count_wrapped(&s_body_font, body, header_w);

	rows_per_col = (int)((height * NK_UI_MENU_MAX_HEIGHT_FRAC - NK_UI_MENU_CHROME_HEIGHT) / row_stride) - head_rows;
	// A reference table (the key lists) reads fine side by side. A table you
	// pick rows out of -- the mission browser, the netgame list -- must stay
	// one column: splitting it squeezes the cells until they clip.
	if (has_table && (table_picks
			|| table_w * 2.0f + s_ctx->style.window.padding.x * 3 > width * NK_UI_MAX_SCREEN_FRAC))
		wide_table = 1;
	if (!reorder && !wide_table)
		ncols = nk_ui_plan_columns(items, nitems, rows_per_col, col_start);
	else
	{
		col_start[0] = 0;
		col_start[1] = nitems;
	}
	if (ncols > 1)
		panel_w = min(panel_w * ncols, width * NK_UI_MAX_SCREEN_FRAC);
	content_w = panel_w - s_ctx->style.window.padding.x * 2 - s_row_h * NK_UI_SCROLLBAR_ROWS;
	if (ncols > 1)
		content_w = (content_w - s_ctx->style.window.spacing.x * (ncols - 1)) / ncols;
	nk_ui_fit_table(content_w);

	tall_rows = 0;
	tall_gap_px = 0.0f;
	for (c = 0; c < ncols; c++)
	{
		int rows = nk_ui_column_rows(items, nitems, col_start[c], col_start[c + 1], content_w);

		if (rows > tall_rows)
		{
			tall_rows = rows;
			tall_gap_px = nk_ui_column_gap_px(items, nitems, col_start[c], col_start[c + 1]);
		}
	}
	// Every panel is as tall as what it holds, capped at most of the screen.
	panel_h = nk_ui_menu_height_px(head_rows + tall_rows, tall_gap_px, height);
	column_h = panel_h - NK_UI_MENU_CHROME_HEIGHT - head_rows * row_stride;
	if (column_h < row_stride)
		column_h = row_stride;

	if (heading && heading[0] && !heading_wraps)
		flags |= NK_WINDOW_TITLE;
	if (!has_focus || s_drawing_underneath)
		flags |= NK_WINDOW_NO_INPUT;
	if (ncols > 1)
		flags |= NK_WINDOW_NO_SCROLLBAR;
	snprintf(name, sizeof(name), "nm%p", id);
	muted = !has_focus || s_drawing_underneath;
	draw.muted = muted;
	// Follow the selection only when it actually moves, so the wheel stays
	// free to scroll away from it.
	if (!muted)
	{
		draw.reveal = refocus || id != s_reveal_id || citem != s_reveal_citem;
		s_reveal_id = id;
		s_reveal_citem = citem;
	}
	if (muted)
		saved_input = nk_ui_mute_input();
	else
		swallowed = nk_ui_swallow_click(&saved_input);

	if (nk_ui_begin_panel(name, heading && !heading_wraps ? heading : "",
			nk_ui_hide_if_muted(nk_rect((width - panel_w) * 0.5f, (height - panel_h) * 0.5f, panel_w, panel_h), muted), flags))
	{
		nk_uint scroll_x = 0, scroll_before = 0;

		nk_window_get_scroll(s_ctx, &scroll_x, &scroll_before);
		if (s_item_font != &s_body_font)
			nk_style_push_font(s_ctx, &s_item_font->handle);
		nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 1);
		if (heading_wraps)
		{
			nk_ui_title_lines(s_ctx, heading);
		}
		if (body)
			nk_ui_message_lines(s_ctx, body);

		if (ncols > 1)
		{
			nk_layout_row_dynamic(s_ctx, column_h, ncols);
			for (c = 0; c < ncols; c++)
			{
				char group[8];

				snprintf(group, sizeof(group), "col%d", c);
				if (!nk_group_begin(s_ctx, group, NK_WINDOW_NO_SCROLLBAR))
					continue;
				nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 1);
				nk_ui_draw_items(s_ctx, &draw, col_start[c], col_start[c + 1]);
				nk_group_end(s_ctx);
			}
			s_wheel_steps = 0;
		}
		else
		{
			nk_ui_draw_items(s_ctx, &draw, 0, nitems);
			if (!muted)
				nk_ui_apply_manual_scroll(scroll_x, scroll_before, 0);
		}
		if (s_item_font != &s_body_font)
			nk_style_pop_font(s_ctx);
	}
	nk_end(s_ctx);
	if (!muted)
		nk_window_set_focus(s_ctx, name);
	if (muted || swallowed)
		s_ctx->input = saved_input;
	s_dim_next = !muted;
	s_panels_pending = 1;
	s_collapsed = NULL;
}

// ==============================
// Control bindings -- the keyboard / joystick / mouse / weapon-key screens.
// One row per action, one button per binding slot, laid out in as many
// columns as the screen height needs. kconfig.c keeps the bindings and the
// capture state machine; this only draws and reports clicks.
// ==============================

#define NK_UI_BIND_SLOT_ROWS 3.2f
#define NK_UI_BIND_HEAD_ROWS 2
#define NK_UI_BIND_MIN_LABEL_ROWS 6

static float nk_ui_bind_label_width(const struct nk_ui_bind_row *rows, int nrows)
{
	float widest = (float)(s_row_h * NK_UI_BIND_MIN_LABEL_ROWS);
	int i;

	for (i = 0; i < nrows; i++)
		widest = max(widest, nk_ui_text_width(&s_body_font, rows[i].label));
	return widest;
}

static int nk_ui_bind_max_slots(const struct nk_ui_bind_row *rows, int nrows)
{
	int most = 1, i;

	for (i = 0; i < nrows; i++)
		most = max(most, rows[i].nslots);
	return most;
}

// One slot: the binding it holds, or a flashing "?" while it is being set.
static int nk_ui_bind_slot(struct nk_context *ctx, const char *text, int is_current, int changing)
{
	struct nk_style_button saved = ctx->style.button;
	struct nk_rect bounds = nk_widget_bounds(ctx);
	int clicked;

	ctx->style.button.border_color = NK_UI_ACCENT_DIM;
	ctx->style.button.normal = nk_style_item_color(nk_ui_mix(nk_ui_shade(20), NK_UI_ACCENT_DIM, 40));
	ctx->style.button.hover = nk_style_item_color(nk_ui_mix(nk_ui_shade(20), NK_UI_ACCENT_DIM, 110));
	ctx->style.button.text_normal = NK_UI_ACCENT_BRIGHT;
	if (is_current && changing)
	{
		ctx->style.button.normal = nk_style_item_color(NK_UI_ACCENT_DIM);
		text = "?";
	}
	clicked = nk_button_label(ctx, text && text[0] ? text : "--");
	ctx->style.button = saved;
	if (is_current)
		nk_stroke_rect(nk_window_get_canvas(ctx), bounds, 0, NK_UI_CURRENT_ITEM_THICKNESS, NK_UI_ACCENT_BRIGHT);
	return clicked;
}

static void nk_ui_bind_row(struct nk_context *ctx, const struct nk_ui_bind_row *row, int index,
	float label_w, float slot_w, int max_slots, int current_row, int current_slot, int changing,
	int *picked_row, int *picked_slot)
{
	int slot;

	nk_layout_row_begin(ctx, NK_STATIC, (float)s_row_h, max_slots + 1);
	nk_layout_row_push(ctx, label_w);
	nk_label(ctx, row->label, NK_TEXT_LEFT);
	for (slot = 0; slot < max_slots; slot++)
	{
		nk_layout_row_push(ctx, slot_w);
		if (slot >= row->nslots)
		{
			nk_spacing(ctx, 1);
			continue;
		}
		if (nk_ui_bind_slot(ctx, row->slot[slot], index == current_row && slot == current_slot, changing)
			&& *picked_row < 0)
		{
			*picked_row = index;
			*picked_slot = slot;
		}
	}
	nk_layout_row_end(ctx);
}

static void nk_ui_bind_head(struct nk_context *ctx, const char *const *slot_names,
	float label_w, float slot_w, int max_slots)
{
	int slot;

	nk_layout_row_begin(ctx, NK_STATIC, (float)s_row_h, max_slots + 1);
	nk_layout_row_push(ctx, label_w);
	nk_spacing(ctx, 1);
	nk_style_push_color(ctx, &ctx->style.text.color, NK_UI_ACCENT_BRIGHT);
	for (slot = 0; slot < max_slots; slot++)
	{
		nk_layout_row_push(ctx, slot_w);
		nk_label(ctx, slot_names[slot] ? slot_names[slot] : "", NK_TEXT_CENTERED);
	}
	nk_style_pop_color(ctx);
	nk_layout_row_end(ctx);
}

// grows only: the hint text changes while capturing, the panel must not
static const void *s_bind_hint_id;
static float s_bind_hint_w;

int nk_ui_bind_frame(const void *id, const char *title, const char *hint,
	const struct nk_ui_bind_row *rows, int nrows, const char *const *slot_names,
	int current_row, int current_slot, int changing, int *picked_row, int *picked_slot)
{
	int width = grd_curscreen->sc_w, height = grd_curscreen->sc_h;
	float row_stride, label_w, slot_w, col_w, panel_w, panel_h, column_h;
	int max_slots, rows_per_col, ncols, per_col;
	int result = NK_UI_BIND_NONE;
	char name[24];
	int c, i;

	*picked_row = -1;
	*picked_slot = -1;
	nk_ui_init_once();
	nk_ui_sync_fonts();
	nk_ui_claim_panel(id);

	row_stride = NK_UI_ROW_HEIGHT + s_ctx->style.window.spacing.y;
	label_w = nk_ui_bind_label_width(rows, nrows);
	slot_w = (float)s_row_h * NK_UI_BIND_SLOT_ROWS;
	max_slots = nk_ui_bind_max_slots(rows, nrows);
	col_w = label_w + max_slots * (slot_w + s_ctx->style.window.spacing.x);

	rows_per_col = (int)((height * NK_UI_MENU_MAX_HEIGHT_FRAC - NK_UI_MENU_CHROME_HEIGHT) / row_stride)
		- NK_UI_BIND_HEAD_ROWS - (slot_names ? 1 : 0);
	if (rows_per_col < 1)
		rows_per_col = 1;
	ncols = (nrows + rows_per_col - 1) / rows_per_col;
	if (ncols < 1)
		ncols = 1;
	while (ncols > 1 && ncols * col_w + s_ctx->style.window.padding.x * 2 > width * NK_UI_MAX_SCREEN_FRAC)
		ncols--;
	per_col = (nrows + ncols - 1) / ncols;

	panel_w = min(ncols * col_w + s_ctx->style.window.padding.x * 2 + s_ctx->style.window.spacing.x * (ncols - 1),
		width * NK_UI_MAX_SCREEN_FRAC);
	if (id != s_bind_hint_id)
		s_bind_hint_w = 0.0f;
	s_bind_hint_id = id;
	s_bind_hint_w = max(s_bind_hint_w, nk_ui_text_width(&s_body_font, hint));
	panel_w = max(panel_w, min(s_bind_hint_w + s_ctx->style.window.padding.x * 4,
		width * NK_UI_MAX_SCREEN_FRAC));
	panel_h = nk_ui_menu_height(NK_UI_BIND_HEAD_ROWS + per_col + (slot_names ? 1 : 0), height);
	column_h = panel_h - NK_UI_MENU_CHROME_HEIGHT - NK_UI_BIND_HEAD_ROWS * row_stride
		+ s_ctx->style.window.padding.y * 2;
	if (column_h < row_stride)
		column_h = row_stride;

	snprintf(name, sizeof(name), "kc%p", id);
	if (nk_ui_begin_panel(name, title, nk_rect((width - panel_w) * 0.5f, (height - panel_h) * 0.5f, panel_w, panel_h),
			NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR))
	{
		nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 1);
		nk_style_push_color(s_ctx, &s_ctx->style.text.color, NK_UI_ACCENT_BRIGHT);
		nk_label(s_ctx, hint, NK_TEXT_CENTERED);
		nk_style_pop_color(s_ctx);

		nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 3);
		if (nk_button_label(s_ctx, "Done"))
			result = NK_UI_BIND_CLOSE;
		if (nk_button_label(s_ctx, "Clear Binding"))
			result = NK_UI_BIND_CLEAR;
		if (nk_button_label(s_ctx, "Restore Defaults"))
			result = NK_UI_BIND_DEFAULTS;

		nk_layout_row_dynamic(s_ctx, column_h, ncols);
		for (c = 0; c < ncols; c++)
		{
			char group[8];

			snprintf(group, sizeof(group), "kcc%d", c);
			if (!nk_group_begin(s_ctx, group, NK_WINDOW_NO_SCROLLBAR))
				continue;
			if (slot_names)
				nk_ui_bind_head(s_ctx, slot_names, label_w, slot_w, max_slots);
			for (i = c * per_col; i < (c + 1) * per_col && i < nrows; i++)
				nk_ui_bind_row(s_ctx, &rows[i], i, label_w, slot_w, max_slots,
					current_row, current_slot, changing, picked_row, picked_slot);
			nk_group_end(s_ctx);
		}
	}
	nk_end(s_ctx);
	nk_window_set_focus(s_ctx, name);
	s_dim_next = 1;
	s_panels_pending = 1;
	if (result == NK_UI_BIND_NONE && *picked_row >= 0)
		result = NK_UI_BIND_PICK;
	return result;
}

// ==============================
// Listbox renderer -- scrollable single-choice list (pilots, missions,
// demos, file browsers). Keyboard handling stays in newmenu.c.
// ==============================

#define NK_UI_LISTBOX_WIDTH_FRAC 0.55f
#define NK_UI_LISTBOX_HEIGHT_FRAC 0.8f
#define NK_UI_LISTBOX_PADDING 12
// Below this a list is quicker to read than to search.
#define NK_UI_LISTBOX_FILTER_MIN 12

// Case-insensitive substring test used by the list search box.
int nk_ui_filter_matches(const char *text, const char *filter)
{
	size_t flen;
	const char *c;

	if (!filter || !filter[0])
		return 1;
	if (!text)
		return 0;
	flen = strlen(filter);
	for (c = text; *c; c++)
		if (!d_strnicmp(c, filter, flen))
			return 1;
	return 0;
}

// Returns NK_UI_LISTBOX_ACCEPT / _CANCEL when a button (or a click on the
// already-highlighted row) ends the list, else _NONE. *citem follows clicks;
// the view scrolls only when *citem moved since *last_citem (keyboard).
int nk_ui_listbox_frame(const void *id, const char *title, char **items, int nitems, int *citem, int *last_citem, unsigned int *scroll_y, char *filter, int allow_abort, int has_focus)
{
	int width = grd_curscreen->sc_w, height = grd_curscreen->sc_h;
	float panel_w = width * NK_UI_LISTBOX_WIDTH_FRAC, panel_h = height * NK_UI_LISTBOX_HEIGHT_FRAC;
	float item_need = 0.0f;
	float list_h = panel_h - NK_UI_MENU_CHROME_HEIGHT - NK_UI_ROW_HEIGHT * (nitems >= NK_UI_LISTBOX_FILTER_MIN ? 2 : 1) - NK_UI_LISTBOX_PADDING;
	int shown = 0;
	nk_flags flags = NK_WINDOW_BORDER;
	int result = NK_UI_LISTBOX_NONE;
	char name[24];
	struct nk_input saved_input;
	int muted;
	int i;

	nk_ui_init_once();
	nk_ui_sync_fonts();
	nk_ui_claim_panel(id);
	for (i = 0; i < nitems; i++)
		item_need = max(item_need, nk_ui_text_width(&s_body_font, items[i]));
	panel_w = min(max(panel_w, item_need + s_row_h * 3 + s_ctx->style.window.padding.x * 2), width * NK_UI_MAX_SCREEN_FRAC);
	if (title && title[0])
		flags |= NK_WINDOW_TITLE;
	if (!has_focus || s_drawing_underneath)
		flags |= NK_WINDOW_NO_INPUT;
	snprintf(name, sizeof(name), "lb%p", id);
	muted = !has_focus || s_drawing_underneath;
	if (muted)
		saved_input = nk_ui_mute_input();

	if (nk_ui_begin_panel(name, title ? title : "",
			nk_ui_hide_if_muted(nk_rect((width - panel_w) * 0.5f, (height - panel_h) * 0.5f, panel_w, panel_h), muted), flags))
	{
		float stride = NK_UI_ROW_HEIGHT + s_ctx->style.window.spacing.y;
		float row_top;

		for (i = 0; i < nitems && i < *citem; i++)
			if (nk_ui_filter_matches(items[i], filter))
				shown++;
		row_top = shown * stride;

		if (nitems >= NK_UI_LISTBOX_FILTER_MIN)
		{
			nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 1);
			if (has_focus && !s_drawing_underneath)
				nk_edit_focus(s_ctx, NK_EDIT_FIELD);
			nk_edit_string_zero_terminated(s_ctx, NK_EDIT_FIELD, filter, NK_UI_FILTER_LEN, nk_filter_default);
		}

		if (*citem != *last_citem && *citem >= 0)
		{
			if (row_top < *scroll_y)
				*scroll_y = (unsigned int)row_top;
			else if (row_top + NK_UI_ROW_HEIGHT > *scroll_y + list_h)
				*scroll_y = (unsigned int)(row_top + NK_UI_ROW_HEIGHT - list_h);
		}
		*last_citem = *citem;

		unsigned int scroll_before = *scroll_y;

		nk_layout_row_dynamic(s_ctx, list_h, 1);
		if (nk_group_scrolled_offset_begin(s_ctx, &(nk_uint){0}, (nk_uint *)scroll_y, "items", NK_WINDOW_BORDER))
		{
			nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, 1);
			for (i = 0; i < nitems; i++)
			{
				nk_bool was_selected;

				if (!nk_ui_filter_matches(items[i], filter))
					continue;
				was_selected = (i == *citem) ? nk_true : nk_false;
				nk_bool now_selected = nk_select_label(s_ctx, items[i], NK_TEXT_LEFT, was_selected);

				if (now_selected && !was_selected)
				{
					*citem = i;
					*last_citem = i;
				}
				else if (was_selected && !now_selected)
				{
					*citem = i;
					result = NK_UI_LISTBOX_ACCEPT;
				}
			}
			nk_group_scrolled_end(s_ctx);
		}
		if (!muted && s_wheel_steps)
		{
			float content_h = nitems * stride;
			float max_scroll = content_h > list_h ? content_h - list_h : 0.0f;
			float wanted = (float)*scroll_y + s_wheel_steps * NK_UI_SCROLL_ROWS_PER_STEP * stride;

			if (*scroll_y == scroll_before)
				*scroll_y = (unsigned int)(wanted < 0.0f ? 0.0f : (wanted > max_scroll ? max_scroll : wanted));
			s_wheel_steps = 0;
		}

		nk_layout_row_dynamic(s_ctx, NK_UI_ROW_HEIGHT, allow_abort ? 2 : 1);
		if (nk_button_label(s_ctx, "Select") && *citem >= 0)
			result = NK_UI_LISTBOX_ACCEPT;
		if (allow_abort && nk_button_label(s_ctx, "Cancel"))
			result = NK_UI_LISTBOX_CANCEL;
	}
	nk_end(s_ctx);
	if (!muted)
		nk_window_set_focus(s_ctx, name);
	if (muted)
		s_ctx->input = saved_input;
	s_dim_next = !muted;
	s_panels_pending = 1;
	return result;
}

// Presents every panel built since the last flush. Called once per frame by
// the window system, after all windows have drawn.
void nk_ui_flush(void)
{
	s_frame_panel_count = 0;
	if (!s_panels_pending)
		return;
	s_panels_pending = 0;
	nk_ui_render(NK_ANTI_ALIASING_ON, grd_curscreen->sc_w, grd_curscreen->sc_h);
}

// ==============================
// Screenshot browser -- the shots PrintScreen writes into SCRNS_DIR, as a
// list with a preview of the one in hand. Only the selected shot is decoded
// and uploaded: a full-resolution grab is several megabytes, so holding a
// whole directory of them at once is not worth the preview it buys.
// ==============================

#define NK_UI_SHOTS_MAX 512
#define NK_UI_SHOT_NAME_LEN 64
// Leaves the list its own share of the panel; the rest previews the shot.
#define NK_UI_SHOTS_LIST_FRAC 0.32f

struct nk_ui_shots
{
	char (*names)[NK_UI_SHOT_NAME_LEN];
	int count;
	int selected;
	int shown;		// which shot the texture holds, or -1
	GLuint tex;
	int tex_w, tex_h;
	int running;
	int game_art;	// listing the PCX art inside the HOG instead of SCRNS_DIR
};

#define NK_UI_GAME_ART_PREFIX "pcx:"

static int nk_ui_shot_is_image(const char *name, int game_art)
{
	const char *dot = strrchr(name, '.');

	if (game_art)
		return dot && !strcasecmp(dot, ".pcx");
	return dot && (!strcasecmp(dot, ".png") || !strcasecmp(dot, ".tga"));
}

// Newest first: the shots are numbered in the order they were taken, so
// their names sort the same way.
static int nk_ui_shot_name_cmp(const void *a, const void *b)
{
	return strcasecmp((const char *)b, (const char *)a);
}

static int nk_ui_art_name_cmp(const void *a, const void *b)
{
	return strcasecmp((const char *)a, (const char *)b);
}

static void nk_ui_shots_scan(struct nk_ui_shots *s)
{
	char **found = PHYSFS_enumerateFiles(s->game_art ? "" : SCRNS_DIR);
	char **f;

	s->count = 0;
	if (!found)
		return;
	for (f = found; *f && s->count < NK_UI_SHOTS_MAX; f++)
	{
		if (!nk_ui_shot_is_image(*f, s->game_art))
			continue;
		strncpy(s->names[s->count], *f, NK_UI_SHOT_NAME_LEN - 1);
		s->names[s->count][NK_UI_SHOT_NAME_LEN - 1] = '\0';
		s->count++;
	}
	PHYSFS_freeList(found);
	if (s->count > 1)
		qsort(s->names, s->count, NK_UI_SHOT_NAME_LEN, s->game_art ? nk_ui_art_name_cmp : nk_ui_shot_name_cmp);
}

static void nk_ui_shots_drop_texture(struct nk_ui_shots *s)
{
	if (s->tex)
		glDeleteTextures(1, &s->tex);
	s->tex = 0;
	s->shown = -1;
}

// Uploads `pixels` (w*h, RGB or RGBA, top row first) as a new texture.
static GLuint nk_ui_make_texture(const ubyte *pixels, int w, int h, int channels)
{
	GLenum format = channels == 4 ? GL_RGBA : GL_RGB;
	GLuint tex;

	glGenTextures(1, &tex);
	glBindTexture(GL_TEXTURE_2D, tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, (GLint)format, (GLsizei)w, (GLsizei)h, 0, format, GL_UNSIGNED_BYTE, pixels);
	glBindTexture(GL_TEXTURE_2D, 0);
	return tex;
}

static void nk_ui_shots_upload(struct nk_ui_shots *s, const ubyte *pixels, int w, int h, int channels)
{
	s->tex = nk_ui_make_texture(pixels, w, h, channels);
	s->tex_w = w;
	s->tex_h = h;
}

static void nk_ui_shot_load(struct nk_ui_shots *s, const char *path)
{
	int w, h, channels;
	ubyte *pixels = shotimg_decode(path, &w, &h, &channels);

	if (!pixels)
		return;
	nk_ui_shots_upload(s, pixels, w, h, channels);
	d_free(pixels);
}

static void nk_ui_shots_load(struct nk_ui_shots *s, int index)
{
	char path[sizeof(SCRNS_DIR) + NK_UI_SHOT_NAME_LEN];

	nk_ui_shots_drop_texture(s);
	s->shown = index;	// a shot that will not decode is not retried every frame
	if (index < 0 || index >= s->count)
		return;

	snprintf(path, sizeof(path), "%s%s", s->game_art ? "" : SCRNS_DIR, s->names[index]);
	nk_ui_shot_load(s, path);
}

// Centres the shot in `avail` at its own aspect, never upscaling past it.
static void nk_ui_shot_preview(struct nk_context *ctx, struct nk_ui_shots *s, struct nk_rect avail)
{
	float scale, w, h;

	if (!s->tex || s->tex_w < 1 || s->tex_h < 1)
	{
		nk_layout_row_dynamic(ctx, avail.h, 1);
		nk_label(ctx, s->count ? "Cannot read this shot" : "No screenshots yet", NK_TEXT_CENTERED);
		return;
	}

	scale = min(avail.w / s->tex_w, avail.h / s->tex_h);
	if (scale > 1.0f)
		scale = 1.0f;
	w = s->tex_w * scale;
	h = s->tex_h * scale;

	nk_layout_row_begin(ctx, NK_STATIC, h, 2);
	nk_layout_row_push(ctx, (avail.w - w) * 0.5f);
	nk_spacing(ctx, 1);
	nk_layout_row_push(ctx, w);
	nk_image(ctx, nk_image_id((int)s->tex));
	nk_layout_row_end(ctx);
}

static void nk_ui_shot_config_value(const struct nk_ui_shots *s, char *out, size_t size)
{
	snprintf(out, size, "%s%s", s->game_art ? NK_UI_GAME_ART_PREFIX : "", s->names[s->selected]);
}

static int nk_ui_shot_is_background(const struct nk_ui_shots *s)
{
	char value[MENU_BACKGROUND_LEN];

	if (!s->count)
		return 0;
	nk_ui_shot_config_value(s, value, sizeof(value));
	return !strcmp(GameCfg.MenuBackground, value);
}

// Opens on the picture already in use, so it is easy to see which one that is.
static void nk_ui_shots_select_current(struct nk_ui_shots *s)
{
	int i;

	for (i = 0; i < s->count; i++)
	{
		s->selected = i;
		if (nk_ui_shot_is_background(s))
			return;
	}
	s->selected = 0;
}

static void nk_ui_shots_switch_source(struct nk_ui_shots *s)
{
	s->game_art = !s->game_art;
	s->selected = 0;
	nk_ui_shots_drop_texture(s);
	nk_ui_shots_scan(s);
}

// Picking the current background again puts the stock one back.
static void nk_ui_shots_background_button(struct nk_context *ctx, struct nk_ui_shots *s)
{
	int is_current = nk_ui_shot_is_background(s);

	if (!s->count)
	{
		nk_spacing(ctx, 1);
		return;
	}
	if (!nk_button_label(ctx, is_current ? "Use default background" : "Set as menu background"))
		return;
	if (is_current)
		snprintf(GameCfg.MenuBackground, sizeof(GameCfg.MenuBackground), "%s", MENU_BACKGROUND_DEFAULT);
	else
		nk_ui_shot_config_value(s, GameCfg.MenuBackground, sizeof(GameCfg.MenuBackground));
}

static void nk_ui_build_shots(struct nk_context *ctx, void *userdata)
{
	struct nk_ui_shots *s = (struct nk_ui_shots *)userdata;
	struct nk_rect region = nk_window_get_content_region(ctx);

	float list_w = region.w * NK_UI_SHOTS_LIST_FRAC;
	float body_h = region.h - NK_UI_ROW_HEIGHT * 2;
	int i;

	nk_ui_step_selection(&s->selected, s->count);
	if (body_h < NK_UI_ROW_HEIGHT)
		body_h = NK_UI_ROW_HEIGHT;

	nk_layout_row_begin(ctx, NK_STATIC, body_h, 2);
	nk_layout_row_push(ctx, list_w);
	if (nk_group_begin(ctx, "shotlist", NK_WINDOW_BORDER))
	{
		nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 1);
		for (i = 0; i < s->count; i++)
		{
			nk_bool chosen = i == s->selected;

			if (nk_selectable_label(ctx, s->names[i], NK_TEXT_LEFT, &chosen) && chosen)
				s->selected = i;
		}
		if (!s->count)
			nk_label(ctx, "empty", NK_TEXT_LEFT);
		nk_group_end(ctx);
	}
	nk_layout_row_push(ctx, region.w - list_w - ctx->style.window.spacing.x);
	if (nk_group_begin(ctx, "shotview", NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR))
	{
		struct nk_rect avail = nk_window_get_content_region(ctx);

		if (s->shown != s->selected)
			nk_ui_shots_load(s, s->selected);
		nk_ui_shot_preview(ctx, s, avail);
		nk_group_end(ctx);
	}
	nk_layout_row_end(ctx);

	nk_layout_row_dynamic(ctx, NK_UI_ROW_HEIGHT, 4);
	if (s->count)
	{
		char detail[NK_UI_SHOT_NAME_LEN + 32];

		snprintf(detail, sizeof(detail), "%s  (%d of %d)", s->names[s->selected], s->selected + 1, s->count);
		nk_label(ctx, detail, NK_TEXT_LEFT);
	}
	else
		nk_label(ctx, s->game_art ? "no game art found" : SCRNS_DIR " is empty", NK_TEXT_LEFT);
	if (nk_button_label(ctx, s->game_art ? "Show screenshots" : "Show game art"))
		nk_ui_shots_switch_source(s);
	nk_ui_shots_background_button(ctx, s);
	if (nk_button_label(ctx, "Close"))
		s->running = 0;
}

// The main menu backdrop is drawn as an RGB texture rather than through the
// 8-bit palette, so a picture never changes the colours the game's text uses.
static struct
{
	GLuint tex;
	char loaded[MENU_BACKGROUND_LEN];	// config value the texture was made from
} s_backdrop;

static void nk_ui_backdrop_load(void)
{
	char path[sizeof(SCRNS_DIR) + MENU_BACKGROUND_LEN];
	const char *value = GameCfg.MenuBackground;
	int w, h, channels;
	ubyte *pixels;

	if (s_backdrop.tex)
		glDeleteTextures(1, &s_backdrop.tex);
	s_backdrop.tex = 0;
	snprintf(s_backdrop.loaded, sizeof(s_backdrop.loaded), "%s", value);
	if (!value[0])
		return;

	if (!strncmp(value, NK_UI_GAME_ART_PREFIX, strlen(NK_UI_GAME_ART_PREFIX)))
		snprintf(path, sizeof(path), "%s", value + strlen(NK_UI_GAME_ART_PREFIX));
	else
		snprintf(path, sizeof(path), "%s%s", SCRNS_DIR, value);
	pixels = shotimg_decode(path, &w, &h, &channels);
	if (!pixels)
		return;
	s_backdrop.tex = nk_ui_make_texture(pixels, w, h, channels);
	d_free(pixels);
}

// Draws `tex` over a rectangle given as fractions of the screen (y down).
static void nk_ui_draw_quad(GLuint tex, float x0, float y0, float x1, float y1, int blend)
{
	const GLfloat vertices[] = { x0, 1 - y0, x1, 1 - y0, x1, 1 - y1, x0, 1 - y1 };
	static const GLfloat texcoords[] = { 0, 0, 1, 0, 1, 1, 0, 1 };
	static const GLfloat colors[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };

	if (blend)
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	}
	else
		glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, tex);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glColorPointer(4, GL_FLOAT, 0, colors);
	glTexCoordPointer(2, GL_FLOAT, 0, texcoords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glBindTexture(GL_TEXTURE_2D, 0);
	glEnable(GL_BLEND);
}

// Returns 1 when a picture was drawn, 0 to let the stock background show.
int nk_ui_draw_backdrop(void)
{
	nk_ui_sync_context();
	// A rebuilt GL context leaves the id valid-looking but unusable, and it
	// samples solid white -- ask GL rather than guess when that happened.
	if (strcmp(s_backdrop.loaded, GameCfg.MenuBackground)
		|| (s_backdrop.tex && !glIsTexture(s_backdrop.tex)))
		nk_ui_backdrop_load();
	if (!s_backdrop.tex)
		return 0;
	nk_ui_draw_quad(s_backdrop.tex, 0, 0, 1, 1, 0);
	return 1;
}

// The logo above the main menu. Its PNG is compiled in (menu_logo_data.c).
extern const unsigned char menu_logo_png[];
extern const unsigned int menu_logo_png_len;

#define NK_UI_LOGO_WIDTH_FRAC 0.36f
// Gap between the logo and the menu text below it.
#define NK_UI_LOGO_GAP_FRAC 0.02f
#define NK_UI_LOGO_MIN_TOP_FRAC 0.02f
// Gap between the letters and the version line sitting under them.
#define NK_UI_LOGO_VERSION_GAP_FRAC 0.008f
// Where the letters end inside the logo picture, as a fraction of its
// height; the rest is transparent padding the version line tucks into.
#define NK_UI_LOGO_LETTERS_BOTTOM_FRAC 0.73f
// Where the first letter starts, as a fraction of the picture's width, so
// the version line can line up with the letters rather than the padding.
#define NK_UI_LOGO_LETTERS_LEFT_FRAC 0.012f

#define NK_UI_LAVA_FRAMES 4
#define NK_UI_LAVA_TILE 64
#define NK_UI_LAVA_SCALE 2	// logo pixels per lava pixel
#define NK_UI_LAVA_FRAME_MS 150
#define NK_UI_LAVA_SAT_FLOOR 50	// below this the pixel is shadow, not letter
#define NK_UI_LAVA_SAT_RAMP 60
#define NK_UI_SHADOW_TOP_RGB { 120, 10, 0 }
#define NK_UI_SHADOW_BOTTOM_RGB { 255, 100, 10 }
#define NK_UI_SHADOW_DARKEN 150	// of 256; keeps the bevel below the lava

extern const unsigned char menu_lava_rgb[];

static GLuint s_logo_tex[NK_UI_LAVA_FRAMES];
static float s_logo_aspect;	// width / height
static int s_logo_tried;

static void nk_ui_logo_drop(void)
{
	int f;

	for (f = 0; f < NK_UI_LAVA_FRAMES; f++)
	{
		if (s_logo_tex[f])
			glDeleteTextures(1, &s_logo_tex[f]);
		s_logo_tex[f] = 0;
	}
}

// Letter bodies are saturated orange; the bevel shadow is grey.
static int nk_ui_logo_body_weight(const ubyte *px)
{
	int hi = max(px[0], max(px[1], px[2]));
	int lo = min(px[0], min(px[1], px[2]));
	int sat = hi ? (hi - lo) * 255 / hi : 0;

	return max(0, min(256, (sat - NK_UI_LAVA_SAT_FLOOR) * 256 / NK_UI_LAVA_SAT_RAMP));
}

// The bevel shadow, recoloured: red at the top of the logo to orange at the
// bottom, still darkened by the original grey so its depth survives.
static int nk_ui_logo_shadow_channel(int top, int bottom, int y, int h, int grey)
{
	int gradient = top + (bottom - top) * y / h;

	return gradient * grey * NK_UI_SHADOW_DARKEN / (255 * 256) * 2;
}

// Replaces the letter bodies with one lava frame, keeping the logo's own
// shading so the bevel still reads.
static void nk_ui_logo_paint_lava(ubyte *out, const ubyte *logo, int w, int h, int frame)
{
	const ubyte *lava = menu_lava_rgb + frame * NK_UI_LAVA_TILE * NK_UI_LAVA_TILE * 3;
	static const int shadow_top[3] = NK_UI_SHADOW_TOP_RGB;
	static const int shadow_bottom[3] = NK_UI_SHADOW_BOTTOM_RGB;
	int x, y, c;

	for (y = 0; y < h; y++)
	{
		for (x = 0; x < w; x++)
		{
			const ubyte *src = logo + (y * w + x) * 4;
			ubyte *dst = out + (y * w + x) * 4;
			int weight = nk_ui_logo_body_weight(src);
			int luma = (src[0] * 3 + src[1] * 6 + src[2]) / 10;
			int shade = 180 + luma;
			const ubyte *tex = lava + (((y / NK_UI_LAVA_SCALE) % NK_UI_LAVA_TILE) * NK_UI_LAVA_TILE
				+ (x / NK_UI_LAVA_SCALE) % NK_UI_LAVA_TILE) * 3;

			for (c = 0; c < 3; c++)
			{
				int lit = min(255, tex[c] * shade / 256);
				int shadow = min(255, nk_ui_logo_shadow_channel(shadow_top[c], shadow_bottom[c], y, h, luma));

				dst[c] = (ubyte)((shadow * (256 - weight) + lit * weight) / 256);
			}
			dst[3] = src[3];
		}
	}
}

static void nk_ui_logo_load(void)
{
	int w, h, channels, f;
	ubyte *pixels = shotimg_decode_memory(menu_logo_png, menu_logo_png_len, &w, &h, &channels);
	ubyte *frame_px;

	s_logo_tried = 1;
	nk_ui_logo_drop();
	if (!pixels)
		return;
	frame_px = d_malloc((size_t)w * h * 4);
	if (!frame_px || channels != 4)
	{
		d_free(frame_px);
		d_free(pixels);
		return;
	}
	for (f = 0; f < NK_UI_LAVA_FRAMES; f++)
	{
		nk_ui_logo_paint_lava(frame_px, pixels, w, h, f);
		s_logo_tex[f] = nk_ui_make_texture(frame_px, w, h, 4);
	}
	s_logo_aspect = (float)w / h;
	d_free(frame_px);
	d_free(pixels);
}

// The build's own version line, centred under the logo in the plain game
// font. Fixed red rather than a palette entry: the menu background is a
// player-chosen image, and its palette would otherwise tint this.
#define NK_UI_VERSION_RED { 1.0f, 0.12f, 0.1f, 1.0f }

// What the version line needs under the logo: its own height plus the gap.
static float nk_ui_logo_version_height(void)
{
	if (!s_body_font.ready)
		return 0.0f;
	return (float)s_body_font.src_h * s_body_font.scale + (float)SHEIGHT * NK_UI_LOGO_VERSION_GAP_FRAC;
}

// How much of that the logo picture's own padding already covers.
static float nk_ui_logo_version_reserve(float logo_h)
{
	float uncovered = nk_ui_logo_version_height() - logo_h * (1.0f - NK_UI_LOGO_LETTERS_BOTTOM_FRAC);

	return uncovered > 0.0f ? uncovered : 0.0f;
}

// One glyph of the baked menu font, tinted `color`, at pixel position (x, y).
static void nk_ui_draw_glyph(const struct nk_ui_font *font, int index, float x, float y, const GLfloat *color)
{
	const struct nk_ui_glyph *g = &font->glyph[index];
	float w = g->w * font->scale, h = font->src_h * font->scale;
	float x0 = x / SWIDTH, x1 = (x + w) / SWIDTH;
	float y0 = 1 - y / SHEIGHT, y1 = 1 - (y + h) / SHEIGHT;
	float u0 = (float)g->x / font->atlas_w, u1 = (float)(g->x + g->w) / font->atlas_w;
	float v0 = (float)g->y / font->atlas_h, v1 = (float)(g->y + font->src_h) / font->atlas_h;
	const GLfloat vertices[] = { x0, y0, x1, y0, x1, y1, x0, y1 };
	const GLfloat texcoords[] = { u0, v0, u1, v0, u1, v1, u0, v1 };
	const GLfloat colors[] = { color[0], color[1], color[2], color[3], color[0], color[1], color[2], color[3],
		color[0], color[1], color[2], color[3], color[0], color[1], color[2], color[3] };

	glVertexPointer(2, GL_FLOAT, 0, vertices);
	glColorPointer(4, GL_FLOAT, 0, colors);
	glTexCoordPointer(2, GL_FLOAT, 0, texcoords);
	glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
}

// Menu-font text in a fixed red, as textured quads: the game's palette-
// indexed text would be tinted by whatever palette the chosen menu
// background loaded. Left edge at `left_px`.
static void nk_ui_draw_red_text(const char *text, float left_px, float top_px)
{
	static const GLfloat red[] = NK_UI_VERSION_RED;
	const struct nk_ui_font *font = &s_body_font;
	const char *c;
	float x;

	if (!font->ready)
		return;
	x = left_px;

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glEnable(GL_TEXTURE_2D);
	glBindTexture(GL_TEXTURE_2D, font->tex);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	for (c = text; *c; c++)
	{
		int index = nk_ui_font_glyph_index(font, (nk_rune)(unsigned char)*c);

		if (index < 0)
			continue;
		nk_ui_draw_glyph(font, index, x, top_px, red);
		x += font->glyph[index].w * font->scale;
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glBindTexture(GL_TEXTURE_2D, 0);
}

// Forgets every texture id without deleting it; see nk_ui_sync_context().
static void nk_ui_forget_textures(void)
{
	int f;

	s_body_font.tex = s_head_font.tex = s_title_font.tex = 0;
	s_body_font.ready = s_head_font.ready = s_title_font.ready = 0;
	s_white_tex = 0;
	s_backdrop.tex = 0;
	s_backdrop.loaded[0] = '\0';
	for (f = 0; f < NK_UI_LAVA_FRAMES; f++)
		s_logo_tex[f] = 0;
	s_logo_tried = 0;
}

static void nk_ui_draw_logo_version(float left_px, float top_px)
{
	nk_ui_draw_red_text(DESCENT_VERSION, left_px, top_px);
}

// The copyright line, centred along the bottom edge of the menu screen.
void nk_ui_draw_copyright(const char *text)
{
	nk_ui_init_once();
	nk_ui_sync_context();
	nk_ui_sync_fonts();
	if (!s_body_font.ready)
		return;
	nk_ui_draw_red_text(text, ((float)SWIDTH - nk_ui_text_width(&s_body_font, text)) * 0.5f,
		(float)SHEIGHT - s_body_font.src_h * s_body_font.scale * 2.0f);
}

// Draws the logo resting just above `menu_top_px`, left edge at `left_px`.
void nk_ui_draw_menu_logo(float left_px, float menu_top_px)
{
	float screen_w = (float)SWIDTH, screen_h = (float)SHEIGHT;
	float min_top = screen_h * NK_UI_LOGO_MIN_TOP_FRAC;
	float w = screen_w * NK_UI_LOGO_WIDTH_FRAC;
	float h, top, version_h;

	nk_ui_init_once();
	nk_ui_sync_context();
	nk_ui_sync_fonts();
	if (!s_logo_tried || (s_logo_tex[0] && !glIsTexture(s_logo_tex[0])))
		nk_ui_logo_load();
	if (!s_logo_tex[0])
		return;

	h = w / s_logo_aspect;
	version_h = nk_ui_logo_version_reserve(h);
	top = menu_top_px - screen_h * NK_UI_LOGO_GAP_FRAC - version_h - h;
	// Shrink rather than run off the top when the menu sits high.
	if (top < min_top)
	{
		h = menu_top_px - screen_h * NK_UI_LOGO_GAP_FRAC - version_h - min_top;
		version_h = nk_ui_logo_version_reserve(h);
		h = menu_top_px - screen_h * NK_UI_LOGO_GAP_FRAC - version_h - min_top;
		if (h <= 0)
			return;
		w = h * s_logo_aspect;
		top = min_top;
	}
	nk_ui_draw_logo_version(left_px + w * NK_UI_LOGO_LETTERS_LEFT_FRAC, top + h * NK_UI_LOGO_LETTERS_BOTTOM_FRAC + screen_h * NK_UI_LOGO_VERSION_GAP_FRAC);
	nk_ui_draw_quad(s_logo_tex[(SDL_GetTicks() / NK_UI_LAVA_FRAME_MS) % NK_UI_LAVA_FRAMES], left_px / screen_w, top / screen_h, (left_px + w) / screen_w, (top + h) / screen_h, 1);
}

void nk_ui_screenshots(void)
{
	struct nk_ui_shots s;

	nk_ui_init_once();
	memset(&s, 0, sizeof(s));
	s.shown = -1;
	s.running = 1;
	s.names = d_malloc(sizeof(*s.names) * NK_UI_SHOTS_MAX);
	if (!s.names)
		return;

	s.game_art = !strncmp(GameCfg.MenuBackground, NK_UI_GAME_ART_PREFIX, strlen(NK_UI_GAME_ART_PREFIX));
	nk_ui_shots_scan(&s);
	nk_ui_shots_select_current(&s);
	while (s.running)
	{
		if (!nk_ui_frame("Screenshots", nk_vec2(0.8f, 0.8f), nk_ui_build_shots, &s))
			break;
	}

	nk_ui_shots_drop_texture(&s);
	d_free(s.names);
}
