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

#include "pstypes.h"
#include "game.h"
#include "multi.h"
#include "net_udp.h"
#include "player.h"
#include "gameseq.h"
#include "mission.h"
#include "text.h"
#include "playsave.h"
#include "gr.h"
#include "ogl_init.h"
#include "mouse.h"
#include "event.h"
#include "nk_ui.h"

// Declared in mouse.h. Without touching this, the cursor-autohide timer in
// mouse_cursor_autohide() (driven by event_poll(), which we bypass with our
// own SDL_PollEvent loop) goes stale while a Nuklear modal is open -- as
// soon as control returns to the legacy menu system afterwards, its next
// event_poll() sees a long-stale timestamp and immediately hides the cursor
// again. We deliberately do NOT call the game's full mouse_button_handler /
// mouse_motion_handler here instead: those call event_send(), which would
// dispatch synthetic input to whatever legacy window is still on the stack
// underneath this modal.

extern void net_udp_init(void);
extern void net_udp_close(void);
extern int net_udp_start_game(void);

static struct nk_context s_ctx_storage;
static struct nk_context *s_ctx = NULL;
static int s_initialized = 0;

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

// Plain procedural backdrop: a dark top-to-bottom gradient, drawn as a
// single untextured quad sized exactly to the screen. No image loading, no
// legacy scaling/caching code, nothing that can come out blown up or
// mis-scaled at an unexpected resolution -- see nk_ui_frame() for the
// history of backdrop approaches that broke in less predictable ways.
static void nk_ui_draw_backdrop(int width, int height)
{
	glMatrixMode(GL_PROJECTION);
	glPushMatrix();
	glLoadIdentity();
	glOrtho(0.0f, width, height, 0.0f, -1.0f, 1.0f);
	glMatrixMode(GL_MODELVIEW);
	glPushMatrix();
	glLoadIdentity();

	glDisable(GL_TEXTURE_2D);
	glDisable(GL_BLEND);
	glBegin(GL_QUADS);
		glColor3f(0.09f, 0.10f, 0.13f); glVertex2f(0.0f, 0.0f);
		glColor3f(0.09f, 0.10f, 0.13f); glVertex2f((float)width, 0.0f);
		glColor3f(0.03f, 0.03f, 0.05f); glVertex2f((float)width, (float)height);
		glColor3f(0.03f, 0.03f, 0.05f); glVertex2f(0.0f, (float)height);
	glEnd();
	glColor3f(1.0f, 1.0f, 1.0f);

	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
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
static void nk_ui_render(enum nk_anti_aliasing aa, int width, int height)
{
	glPushAttrib(GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_TRANSFORM_BIT);
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

	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);

	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_TEXTURE_2D);

	glBindTexture(GL_TEXTURE_2D, 0);
	glMatrixMode(GL_MODELVIEW);
	glPopMatrix();
	glMatrixMode(GL_PROJECTION);
	glPopMatrix();
	glPopAttrib();
}

// Set on an Escape keydown, consumed and cleared by nk_ui_frame() after
// input handling each frame: Nuklear's own NK_KEY_TEXT_RESET_MODE binding
// for Escape only affects an active text-edit widget, so it doesn't give us
// a general "close this screen" behavior on its own -- nk_ui_frame() treats
// this the same as clicking Back/Cancel.
static int s_escape_pressed = 0;

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
				case SDLK_KP_ENTER: case SDLK_RETURN: nk_input_key(ctx, NK_KEY_ENTER, down); break;
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
				case SDLK_UP: nk_input_key(ctx, NK_KEY_UP, down); break;
				case SDLK_DOWN: nk_input_key(ctx, NK_KEY_DOWN, down); break;
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
				case SDL_BUTTON_WHEELUP: if (down) nk_input_scroll(ctx, nk_vec2(0, 1)); break;
				case SDL_BUTTON_WHEELDOWN: if (down) nk_input_scroll(ctx, nk_vec2(0, -1)); break;
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

static void nk_ui_apply_theme(struct nk_context *ctx)
{
	// A plain dark theme -- not trying to match the game's original bitmap
	// menu skin, just something readable that doesn't look like a raw
	// debug overlay.
	struct nk_color table[NK_COLOR_COUNT];
	table[NK_COLOR_TEXT] = nk_rgba(230, 230, 230, 255);
	table[NK_COLOR_WINDOW] = nk_rgba(22, 24, 30, 222);
	table[NK_COLOR_HEADER] = nk_rgba(38, 42, 54, 255);
	table[NK_COLOR_BORDER] = nk_rgba(60, 64, 74, 255);
	table[NK_COLOR_BUTTON] = nk_rgba(56, 61, 76, 255);
	table[NK_COLOR_BUTTON_HOVER] = nk_rgba(76, 82, 102, 255);
	table[NK_COLOR_BUTTON_ACTIVE] = nk_rgba(96, 60, 40, 255);
	table[NK_COLOR_TOGGLE] = nk_rgba(45, 49, 61, 255);
	table[NK_COLOR_TOGGLE_HOVER] = nk_rgba(66, 71, 88, 255);
	table[NK_COLOR_TOGGLE_CURSOR] = nk_rgba(200, 120, 40, 255);
	table[NK_COLOR_SELECT] = nk_rgba(45, 49, 61, 255);
	table[NK_COLOR_SELECT_ACTIVE] = nk_rgba(200, 120, 40, 255);
	table[NK_COLOR_SLIDER] = nk_rgba(45, 49, 61, 255);
	table[NK_COLOR_SLIDER_CURSOR] = nk_rgba(200, 120, 40, 255);
	table[NK_COLOR_SLIDER_CURSOR_HOVER] = nk_rgba(220, 140, 60, 255);
	table[NK_COLOR_SLIDER_CURSOR_ACTIVE] = nk_rgba(230, 155, 80, 255);
	table[NK_COLOR_PROPERTY] = nk_rgba(45, 49, 61, 255);
	table[NK_COLOR_EDIT] = nk_rgba(30, 32, 40, 255);
	table[NK_COLOR_EDIT_CURSOR] = nk_rgba(230, 230, 230, 255);
	table[NK_COLOR_COMBO] = nk_rgba(45, 49, 61, 255);
	table[NK_COLOR_CHART] = nk_rgba(30, 32, 40, 255);
	table[NK_COLOR_CHART_COLOR] = nk_rgba(200, 120, 40, 255);
	table[NK_COLOR_CHART_COLOR_HIGHLIGHT] = nk_rgba(255, 0, 0, 255);
	table[NK_COLOR_SCROLLBAR] = nk_rgba(30, 32, 40, 255);
	table[NK_COLOR_SCROLLBAR_CURSOR] = nk_rgba(66, 71, 88, 255);
	table[NK_COLOR_SCROLLBAR_CURSOR_HOVER] = nk_rgba(86, 91, 108, 255);
	table[NK_COLOR_SCROLLBAR_CURSOR_ACTIVE] = nk_rgba(96, 101, 118, 255);
	table[NK_COLOR_TAB_HEADER] = nk_rgba(38, 42, 54, 255);
	nk_style_from_table(ctx, table);
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

	nk_ui_apply_theme(s_ctx);

	s_initialized = 1;
}

// One iteration of a modal Nuklear screen: pump input, let the caller build
// widgets into a floating window sized to the current draw area, then
// render+present. Returns 0 if the player closed the window/quit, 1
// otherwise. `size_frac` is the window's (width, height) as a fraction of
// the screen -- smaller screens (like the object/weapon toggle lists) should
// pass a smaller fraction so they read as a floating panel, not a takeover.
static int nk_ui_frame(const char *title, struct nk_vec2 size_frac, void (*build)(struct nk_context *ctx, void *userdata), void *userdata)
{
	SDL_Event evt;
	int width = grd_curscreen->sc_w, height = grd_curscreen->sc_h;
	struct nk_rect bounds;
	nk_flags win_flags = NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE | NK_WINDOW_SCROLL_AUTO_HIDE;

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

	bounds = nk_rect(width * (1.0f - size_frac.x) * 0.5f, height * (1.0f - size_frac.y) * 0.5f,
		width * size_frac.x, height * size_frac.y);
	if (nk_begin(s_ctx, title, bounds, win_flags))
		build(s_ctx, userdata);
	nk_end(s_ctx);

	nk_ui_draw_backdrop(width, height);
	nk_ui_render(NK_ANTI_ALIASING_ON, width, height);
	ogl_swap_buffers_internal();

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

	nk_layout_row_dynamic(ctx, 26, 1);
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, 22, 1);
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
		if (!nk_ui_frame("Objects to allow", nk_vec2(0.4f, 0.6f), nk_ui_build_objects_allowed, &running))
			break;
	}
}

static void nk_ui_build_spawn_with_weapons(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;
	int v;

	nk_layout_row_dynamic(ctx, 26, 1);
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, 22, 1);
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
		if (!nk_ui_frame("Start with weapons", nk_vec2(0.35f, 0.5f), nk_ui_build_spawn_with_weapons, &running))
			break;
	}
}

static void nk_ui_build_static_weapons(struct nk_context *ctx, void *userdata)
{
	int *running = (int *)userdata;

	nk_layout_row_dynamic(ctx, 26, 1);
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	nk_layout_row_dynamic(ctx, 22, 1);
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
		if (!nk_ui_frame("Static weapons (won't move or drop)", nk_vec2(0.35f, 0.55f), nk_ui_build_static_weapons, &running))
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

	nk_layout_row_dynamic(ctx, 26, 1);
	if (nk_button_label(ctx, "Back"))
		*running = 0;

	if (nk_tree_push(ctx, NK_TREE_TAB, "Game Rules", NK_MAXIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, 22, 1);

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

	if (nk_tree_push(ctx, NK_TREE_TAB, "Powerups & Weapons", NK_MAXIMIZED))
	{
		int v;
		nk_layout_row_dynamic(ctx, 22, 1);

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

		if (nk_button_label(ctx, "Set Objects Allowed..."))
			nk_ui_objects_allowed();
		if (nk_button_label(ctx, "Start Mission With..."))
			nk_ui_spawn_with_weapons();
		if (nk_button_label(ctx, "Select Static Weapons..."))
			nk_ui_static_weapons();

		nk_tree_pop(ctx);
	}

	if (nk_tree_push(ctx, NK_TREE_TAB, "Spawn Settings", NK_MINIMIZED))
	{
		nk_layout_row_dynamic(ctx, 22, 1);
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
		nk_layout_row_dynamic(ctx, 22, 1);
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
		nk_layout_row_dynamic(ctx, 22, 1);
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
		nk_layout_row_dynamic(ctx, 22, 1);

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
		nk_layout_row_dynamic(ctx, 22, 1);

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
		if (!nk_ui_frame("Advanced netgame options", nk_vec2(0.62f, 0.82f), nk_ui_build_advanced_options, &running))
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

	nk_layout_row_dynamic(ctx, 26, 2);
	if (nk_button_label(ctx, "Cancel"))
		st->running = 0;
	if (nk_button_label(ctx, "Start Game"))
	{
		if ((Netgame.levelnum < Last_secret_level) || (Netgame.levelnum > Last_level) || (Netgame.levelnum == 0))
		{
			// Level out of range -- old menu popped a blocking messagebox here;
			// just reset silently and let the player notice/re-enter it.
			Netgame.levelnum = 1;
			strcpy(st->slevel, "1");
		}
		else if (net_udp_start_game())
		{
			st->started = 1;
			st->running = 0;
		}
	}

	nk_layout_row_dynamic(ctx, 22, 1);
	nk_label(ctx, TXT_DESCRIPTION, NK_TEXT_LEFT);
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, Netgame.game_name, NETGAME_NAME_LEN + 1, nk_filter_default);

	{
		char level_label[48];
		snprintf(level_label, sizeof(level_label), "%s (1-%d)", TXT_LEVEL_, Last_level);
		nk_label(ctx, level_label, NK_TEXT_LEFT);
	}
	nk_edit_string_zero_terminated(ctx, NK_EDIT_FIELD, st->slevel, sizeof(st->slevel), nk_filter_decimal);
	Netgame.levelnum = atoi(st->slevel);

	nk_label(ctx, TXT_OPTIONS, NK_TEXT_LEFT);
	if (nk_option_label(ctx, TXT_ANARCHY, Netgame.gamemode == NETGAME_ANARCHY || Netgame.gamemode == 0)) { Netgame.gamemode = NETGAME_ANARCHY; }
	if (nk_option_label(ctx, TXT_TEAM_ANARCHY, Netgame.gamemode == NETGAME_TEAM_ANARCHY && !Netgame.CTF)) { Netgame.gamemode = NETGAME_TEAM_ANARCHY; Netgame.CTF = 0; }
	if (nk_option_label(ctx, "Capture the Flag", Netgame.gamemode == NETGAME_TEAM_ANARCHY && Netgame.CTF)) { Netgame.gamemode = NETGAME_TEAM_ANARCHY; Netgame.CTF = 1; }
	if (nk_option_label(ctx, TXT_ANARCHY_W_ROBOTS, Netgame.gamemode == NETGAME_ROBOT_ANARCHY)) { Netgame.gamemode = NETGAME_ROBOT_ANARCHY; }
	if (nk_option_label(ctx, TXT_COOPERATIVE, Netgame.gamemode == NETGAME_COOPERATIVE)) { Netgame.gamemode = NETGAME_COOPERATIVE; }
	if (nk_option_label(ctx, "Bounty", Netgame.gamemode == NETGAME_BOUNTY)) { Netgame.gamemode = NETGAME_BOUNTY; }
	if (nk_option_label(ctx, "Turkey Shoot", Netgame.gamemode == NETGAME_TURKEY_SHOOT)) { Netgame.gamemode = NETGAME_TURKEY_SHOOT; }
	if (nk_option_label(ctx, "Arcade", Netgame.gamemode == NETGAME_ARCADE)) { Netgame.gamemode = NETGAME_ARCADE; Netgame.CTF = 0; }

	nk_label(ctx, "Access", NK_TEXT_LEFT);
	{
		int closed = (Netgame.game_flags & NETGAME_FLAG_CLOSED) ? 1 : 0;
		int restricted = Netgame.RefusePlayers ? 1 : 0;
		int open = (!closed && !restricted) ? 1 : 0;

		if (nk_option_label(ctx, "Open game", open)) { Netgame.game_flags &= ~NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 0; }
		if (nk_option_label(ctx, TXT_CLOSED_GAME, closed)) { Netgame.game_flags |= NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 0; }
		if (nk_option_label(ctx, "Restricted Game", restricted)) { Netgame.game_flags &= ~NETGAME_FLAG_CLOSED; Netgame.RefusePlayers = 1; }
	}

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

	nk_layout_row_dynamic(ctx, 26, 1);
	if (nk_button_label(ctx, "Advanced options..."))
		nk_ui_advanced_options();
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
		if (!nk_ui_frame("Start Multiplayer Game", nk_vec2(0.6f, 0.8f), nk_ui_build_hosting, &st))
			break;
	}

	return st.started;
}
