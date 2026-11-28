/*
 *
 * SDL Event related stuff
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include "event.h"
#include "key.h"
#include "mouse.h"
#include "window.h"
#include "timer.h"
#include "config.h"
#include "args.h"

#include "joy.h"
#include "nk_ui.h"
#include "kconfig.h"

extern void key_handler(SDL_KeyboardEvent *event);
extern void mouse_button_handler(SDL_MouseButtonEvent *mbe);
extern void mouse_motion_handler(SDL_MouseMotionEvent *mme);
extern void mouse_cursor_autohide();

static int initialised=0;

// SNG: gamepads drive menus by posing as the keyboard
#define PAD_AXIS_DEADZONE	16000
#define PAD_AXIS_RELEASE	12000	// hysteresis: a stick resting at the edge would chatter
#define PAD_DIRECTIONS		4

#define PAD_AXIS_MAX		32767
#define PAD_REPEAT_DELAY_MS	280
#define PAD_REPEAT_SLOW_MS	160
#define PAD_REPEAT_FAST_MS	35

#define PAD_SCROLL_SLOW_MS	260
#define PAD_SCROLL_FAST_MS	70
#ifdef _WIN32
#define PAD_AXIS_SCROLL		3	// winmm: right stick is R (y), then U (x)
#else
#define PAD_AXIS_SCROLL		4	// joydev: right stick x 3, y 4
#endif

static int pad_held[PAD_DIRECTIONS];
static int pad_deflect[PAD_DIRECTIONS];		// hats count as half-way
static Uint32 pad_next_repeat[PAD_DIRECTIONS];

static void pad_key_event(SDL_Event *out, int sym, int down)
{
	memset(out, 0, sizeof(*out));
	out->type = down ? SDL_KEYDOWN : SDL_KEYUP;
	out->key.state = down ? SDL_PRESSED : SDL_RELEASED;
	out->key.keysym.sym = sym;
}

#define PAD_BUTTON_ACCEPT	0
#define PAD_BUTTON_BACK		1
#define PAD_BUTTON_PREV_TAB	4
#define PAD_BUTTON_NEXT_TAB	5
#define PAD_BUTTON_SELECT	6

static int pad_button_sym(int button)
{
	if (button == GameCfg.PadPause)
		return SDLK_RETURN;	// the game-menu button accepts in menus
	switch (button)
	{
		case PAD_BUTTON_ACCEPT:		return SDLK_RETURN;
		case PAD_BUTTON_BACK:
		case PAD_BUTTON_SELECT:		return SDLK_ESCAPE;
		case PAD_BUTTON_PREV_TAB:	return SDLK_PAGEUP;
		case PAD_BUTTON_NEXT_TAB:	return SDLK_PAGEDOWN;
	}
	return 0;
}

static const int pad_direction_sym[PAD_DIRECTIONS] = { SDLK_UP, SDLK_DOWN, SDLK_LEFT, SDLK_RIGHT };

// One direction changes per event; the caller re-polls for the rest.
static int pad_direction_event(SDL_Event *out, const int wanted[PAD_DIRECTIONS])
{
	int dir;

	for (dir = 0; dir < PAD_DIRECTIONS; dir++)
	{
		if (wanted[dir] == pad_held[dir])
			continue;
		pad_held[dir] = wanted[dir];
		pad_next_repeat[dir] = SDL_GetTicks() + PAD_REPEAT_DELAY_MS;
		pad_key_event(out, pad_direction_sym[dir], wanted[dir]);
		return 1;
	}
	return 0;
}

static int pad_scroll_deflect;	// right stick, signed; 0 inside the deadzone
static Uint32 pad_next_scroll;

static void pad_scroll_axis(int value)
{
	if (abs(value) < PAD_AXIS_DEADZONE)
		value = 0;
	else if (!pad_scroll_deflect)
		pad_next_scroll = SDL_GetTicks();
	pad_scroll_deflect = value;
}

static int pad_axis_threshold(int held)
{
	return held ? PAD_AXIS_RELEASE : PAD_AXIS_DEADZONE;
}

static int pad_axis_event(SDL_Event *out, const SDL_JoyAxisEvent *axis)
{
	int wanted[PAD_DIRECTIONS], vertical;

	if (axis->axis == PAD_AXIS_SCROLL)
	{
		pad_scroll_axis(axis->value);
		return 0;
	}

	// sticks and d-pad-as-axes only; triggers rest at full negative
	if (axis->axis != 0 && axis->axis != 1 && axis->axis != 6 && axis->axis != 7)
		return 0;
	vertical = axis->axis & 1;
	pad_deflect[vertical ? 0 : 2] = pad_deflect[vertical ? 1 : 3] = abs(axis->value);
	memcpy(wanted, pad_held, sizeof(wanted));
	wanted[vertical ? 0 : 2] = axis->value < -pad_axis_threshold(pad_held[vertical ? 0 : 2]);
	wanted[vertical ? 1 : 3] = axis->value > pad_axis_threshold(pad_held[vertical ? 1 : 3]);
	return pad_direction_event(out, wanted);
}

static int pad_hat_event(SDL_Event *out, const SDL_JoyHatEvent *hat)
{
	int wanted[PAD_DIRECTIONS];

	pad_deflect[0] = pad_deflect[1] = pad_deflect[2] = pad_deflect[3] = (PAD_AXIS_DEADZONE + PAD_AXIS_MAX) / 2;
	wanted[0] = !!(hat->value & SDL_HAT_UP);
	wanted[1] = !!(hat->value & SDL_HAT_DOWN);
	wanted[2] = !!(hat->value & SDL_HAT_LEFT);
	wanted[3] = !!(hat->value & SDL_HAT_RIGHT);
	return pad_direction_event(out, wanted);
}

// Rewrites a pad event in place as a key event. Returns 0 for pad input
// with no menu meaning, which the caller drops.
static int pad_to_key(SDL_Event *event)
{
	int sym;

	switch (event->type)
	{
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
			sym = pad_button_sym(event->jbutton.button);
			if (!sym)
				return 0;
			pad_key_event(event, sym, event->type == SDL_JOYBUTTONDOWN);
			return 1;
		case SDL_JOYAXISMOTION:
			return pad_axis_event(event, &event->jaxis);
		case SDL_JOYHATMOTION:
			return pad_hat_event(event, &event->jhat);
	}
	return 0;
}

extern window *Game_wind;

static int pad_drives_menus(window *front)
{
	return front && front != Game_wind && !GameArg.CtlNoJoystick && !kconfig_binding();
}

// a release seen only by the game would leave a menu repeating forever
static void pad_forget_held(void)
{
	memset(pad_held, 0, sizeof(pad_held));
	pad_scroll_deflect = 0;
}

static int pad_event(const SDL_Event *event)
{
	return event->type == SDL_JOYBUTTONDOWN || event->type == SDL_JOYBUTTONUP ||
		event->type == SDL_JOYAXISMOTION || event->type == SDL_JOYHATMOTION;
}

// further from centre, faster: the stick scrolls menus like a drag
static Uint32 pad_interval(int deflect, Uint32 slow_ms, Uint32 fast_ms)
{
	int over = max(0, min(deflect, PAD_AXIS_MAX) - PAD_AXIS_DEADZONE);

	return slow_ms - over * (slow_ms - fast_ms) / (PAD_AXIS_MAX - PAD_AXIS_DEADZONE);
}

static Uint32 pad_repeat_interval(int deflect)
{
	return pad_interval(deflect, PAD_REPEAT_SLOW_MS, PAD_REPEAT_FAST_MS);
}

// SNG: the right stick scrolls like a wheel, faster the further it is pushed
static int pad_scroll_steps(void)
{
	Uint32 now = SDL_GetTicks();

	if (!pad_scroll_deflect || (Sint32)(now - pad_next_scroll) < 0)
		return 0;
	pad_next_scroll = now + pad_interval(abs(pad_scroll_deflect), PAD_SCROLL_SLOW_MS, PAD_SCROLL_FAST_MS);
	return pad_scroll_deflect > 0 ? 1 : -1;
}

// A held direction's next auto-repeat key press, once it is due.
int event_pad_repeat(SDL_Event *out)
{
	Uint32 now = SDL_GetTicks();
	int dir;

	for (dir = 0; dir < PAD_DIRECTIONS; dir++)
	{
		if (!pad_held[dir] || (Sint32)(now - pad_next_repeat[dir]) < 0)
			continue;
		pad_next_repeat[dir] = now + pad_repeat_interval(pad_deflect[dir]);
		pad_key_event(out, pad_direction_sym[dir], 1);
		return 1;
	}
	return 0;
}

static int pad_capture = -2;	// -2 off, -1 waiting, else the button caught

// The next pad button is reported here instead of acting, for binding.
void event_pad_capture_begin(void)
{
	pad_capture = -1;
}

int event_pad_captured(void)
{
	int button = pad_capture;

	if (button >= 0)
		pad_capture = -2;
	return button;
}

void event_pad_capture_end(void)
{
	pad_capture = -2;
}

// 0: pad input with no menu meaning, to drop
int event_pad_as_key(SDL_Event *event)
{
	if (event->type == SDL_JOYAXISMOTION && !GameArg.CtlNoJoystick)
		joy_axis_track(&event->jaxis);
	if (pad_capture != -2 && pad_event(event))
	{
		if (pad_capture == -1 && event->type == SDL_JOYBUTTONDOWN)
			pad_capture = event->jbutton.button;
		return 0;
	}
	return !pad_event(event) || pad_to_key(event);
}

// In play only the game-menu button is the menus': it acts as Esc.
static int pad_pause_in_game(window *front, SDL_Event *event)
{
	int button = GameCfg.PadPause;

	if (front != Game_wind || !front || GameArg.CtlNoJoystick || button < 0)
		return 0;
	if ((event->type != SDL_JOYBUTTONDOWN && event->type != SDL_JOYBUTTONUP) || event->jbutton.button != button)
		return 0;
	pad_key_event(event, SDLK_ESCAPE, event->type == SDL_JOYBUTTONDOWN);
	return 1;
}

void event_poll()
{
	SDL_Event event;
	int clean_uniframe=1;
	window *wind = window_get_front();
	int idle = 1;
#ifdef USE_NK_UI
	int nk_menu_input = nk_ui_menus_open();
#endif
#if SDL_VERSION_ATLEAST(2, 0, 0)
	int i = 0;
	int j = 0;
#endif
	
	// If the front window changes, exit this loop, otherwise unintended behavior can occur
	// like pressing 'Return' really fast at 'Difficulty Level' causing multiple games to be started
#ifdef USE_NK_UI
	if (nk_menu_input)
		nk_ui_input_begin();
#endif
	if (!GameArg.CtlNoJoystick)
		joy_mirror_scan(kconfig_binding());
	while ((wind == window_get_front()) && SDL_PollEvent(&event))
	{
		if (!GameArg.CtlNoJoystick && joy_mirror_drop(&event))
			continue;
		if (!pad_drives_menus(wind))
			pad_forget_held();
		else if (!event_pad_as_key(&event))
			continue;
		pad_pause_in_game(wind, &event);
#ifdef USE_NK_UI
		if (nk_menu_input)
			nk_ui_feed_event(&event);
#endif
		switch(event.type) {
			case SDL_KEYDOWN:
			case SDL_KEYUP:
				if (clean_uniframe)
					memset(unicode_frame_buffer,'\0',sizeof(unsigned char)*KEY_BUFFER_SIZE);
				clean_uniframe=0;
				key_handler((SDL_KeyboardEvent *)&event);
				idle = 0;
				break;
#if SDL_VERSION_ATLEAST(2, 0, 0)
			case SDL_TEXTINPUT:
				if (clean_uniframe)
					memset(unicode_frame_buffer,'\0',sizeof(unsigned char)*KEY_BUFFER_SIZE);
				clean_uniframe=0;
				for (i = 0; event.text.text[i] != '\0'; i++)
				{
					unsigned char ch = (unsigned char)event.text.text[i];
					if (ch > 31 && ch < 255)
					{
						for (j = 0; j < KEY_BUFFER_SIZE; j++)
						{
							if (unicode_frame_buffer[j] == '\0')
							{
								unicode_frame_buffer[j] = ch;
								break;
							}
						}
					}
				}
				idle = 0;
				break;
#endif
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if (GameArg.CtlNoMouse)
					break;
				mouse_button_handler((SDL_MouseButtonEvent *)&event);
				idle = 0;
				break;
			case SDL_MOUSEMOTION:
				if (GameArg.CtlNoMouse)
					break;
				mouse_motion_handler((SDL_MouseMotionEvent *)&event);
				idle = 0;
				break;
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				if (GameArg.CtlNoJoystick)
					break;
				joy_button_handler((SDL_JoyButtonEvent *)&event);
				idle = 0;
				break;
			case SDL_JOYAXISMOTION:
				if (GameArg.CtlNoJoystick)
					break;
				if (joy_axisbutton_handler((SDL_JoyAxisEvent *)&event))
					idle = 0;
				if (joy_axis_handler((SDL_JoyAxisEvent *)&event))
					idle = 0;
				break;
			case SDL_JOYHATMOTION:
				if (GameArg.CtlNoJoystick)
					break;
				joy_hat_handler((SDL_JoyHatEvent *)&event);
				idle = 0;
				break;
			case SDL_JOYBALLMOTION:
				break;
			case SDL_QUIT: {
				d_event qevent = { EVENT_QUIT };
				call_default_handler(&qevent);
				idle = 0;
			} break;
		}
	}

	if (wind == window_get_front() && pad_drives_menus(wind) && event_pad_repeat(&event))
	{
#ifdef USE_NK_UI
		if (nk_menu_input)
			nk_ui_feed_event(&event);
#endif
		event.type = SDL_KEYUP;		// key_handler drops repeats when key repeat is off
		event.key.state = SDL_RELEASED;
		key_handler((SDL_KeyboardEvent *)&event);
		event.type = SDL_KEYDOWN;
		event.key.state = SDL_PRESSED;
		key_handler((SDL_KeyboardEvent *)&event);
		idle = 0;
	}

#ifdef USE_NK_UI
	if (nk_menu_input && wind == window_get_front() && pad_drives_menus(wind))
		nk_ui_pad_scroll(pad_scroll_steps());
	if (nk_menu_input)
		nk_ui_input_end();
#endif

	// Send the idle event if there were no other events
	if (idle)
	{
		d_event ievent;
		
		ievent.type = EVENT_IDLE;
		event_send(&ievent);
	}
	else
		event_reset_idle_seconds();
	
	mouse_cursor_autohide();
}

void event_flush()
{
	SDL_Event event;
	
	while (SDL_PollEvent(&event));
}

int event_init()
{
	// We should now be active and responding to events.
	initialised = 1;

	return 0;
}

int (*default_handler)(d_event *event) = NULL;

void set_default_handler(int (*handler)(d_event *event))
{
	default_handler = handler;
}

int call_default_handler(d_event *event)
{
	if (default_handler)
		return (*default_handler)(event);
	
	return 0;
}

void event_send(d_event *event)
{
	window *wind;
	int handled = 0;

	for (wind = window_get_front(); wind != NULL && !handled; wind = window_get_prev(wind))
		if (window_is_visible(wind))
		{
			handled = window_send_event(wind, event);

			if (!window_exists(wind)) // break away if necessary: window_send_event() could have closed wind by now
				break;
			if (window_is_modal(wind))
				break;
		}
	
	if (!handled)
		call_default_handler(event);
}

// Process the first event in queue, sending to the appropriate handler
// This is the new object-oriented system
// Uses the old system for now, but this may change
void event_process(void)
{
	d_event event;
	window *wind = window_get_front();

	timer_update();

	event_poll();	// send input events first

	// Doing this prevents problems when a draw event can create a newmenu,
	// such as some network menus when they report a problem
	if (window_get_front() != wind)
		return;
	
	event.type = EVENT_WINDOW_DRAW;	// then draw all visible windows
	wind = window_get_first();
	while (wind != NULL)
	{
		window *prev = window_get_prev(wind);
		if (window_is_visible(wind))
			window_send_event(wind, &event);
		if (!window_exists(wind))
		{
			if (!prev) // well there isn't a previous window ...
				break; // ... just bail out - we've done everything for this frame we can.
			wind = window_get_next(prev); // the current window seemed to be closed. so take the next one from the previous which should be able to point to the one after the current closed
		}
		else
			wind = window_get_next(wind);
	}

#ifdef USE_NK_UI
	nk_ui_flush();
#endif

	gr_flip();
}

void event_toggle_focus(int activate_focus)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	SDL_Window *window = SDL_GetMouseFocus();
	if (!window)
		window = SDL_GetKeyboardFocus();
	if (window)
		SDL_SetWindowGrab(window, (activate_focus && GameCfg.Grabinput) ? SDL_TRUE : SDL_FALSE);
	SDL_SetRelativeMouseMode((activate_focus && GameCfg.Grabinput) ? SDL_TRUE : SDL_FALSE);
#else
	if (activate_focus && GameCfg.Grabinput)
		SDL_WM_GrabInput(SDL_GRAB_ON);
	else
		SDL_WM_GrabInput(SDL_GRAB_OFF);
#endif
	mouse_toggle_cursor(!activate_focus);
}

static fix64 last_event = 0;

void event_reset_idle_seconds()
{
	last_event = timer_query();
}

fix event_get_idle_seconds()
{
	return (timer_query() - last_event)/F1_0;
}

