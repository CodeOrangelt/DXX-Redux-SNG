/*
 *
 * SDL joystick support
 *
 */

#include <string.h>   // for memset

#include "joy.h"
#include "dxxerror.h"
#include "timer.h"
#include "console.h"
#include "event.h"
#include "text.h"
#include "u_mem.h"
#include "playsave.h"

extern char *joybutton_text[]; //from kconfig.c
extern char *joyaxis_text[]; //from kconfig.c

int num_joysticks = 0;
int joy_num_axes = 0;

/* This struct is a "virtual" joystick, which includes all the axes
 * and buttons of every joystick found.
 */
static struct joyinfo {
	int n_axes;
	int n_buttons;
	int axis_value[JOY_MAX_AXES];
	ubyte button_state[JOY_MAX_BUTTONS];
	ubyte button_last_state[JOY_MAX_BUTTONS]; // for HAT movement only
} Joystick;

typedef struct d_event_joystickbutton
{
	event_type type;
	int button;
} d_event_joystickbutton;

typedef struct d_event_joystick_moved
{
	event_type	type;	// EVENT_JOYSTICK_MOVED
	int		axis;
	int 		value;
} d_event_joystick_moved;

/* This struct is an array, with one entry for each physical joystick
 * found.
 */
static struct {
	SDL_Joystick *handle;
	int n_axes;
	int n_buttons;
	int n_hats;
	int hat_map[MAX_HATS_PER_JOYSTICK];  //Note: Descent expects hats to be buttons, so these are indices into Joystick.buttons
	int axis_map[MAX_AXES_PER_JOYSTICK];
	int button_map[MAX_BUTTONS_PER_JOYSTICK];
	int axis_button_map[MAX_AXES_PER_JOYSTICK];
	ubyte axis_is_trigger[MAX_AXES_PER_JOYSTICK];	// SNG: rests at full negative
} SDL_Joysticks[MAX_JOYSTICKS];

void joy_button_handler(SDL_JoyButtonEvent *jbe)
{
	int button;
	d_event_joystickbutton event;

	button = SDL_Joysticks[jbe->which].button_map[jbe->button];

	Joystick.button_state[button] = jbe->state;

	event.type = (jbe->type == SDL_JOYBUTTONDOWN) ? EVENT_JOYSTICK_BUTTON_DOWN : EVENT_JOYSTICK_BUTTON_UP;
	event.button = button;
	con_printf(CON_DEBUG, "Sending event %s, button %d\n", (jbe->type == SDL_JOYBUTTONDOWN) ? "EVENT_JOYSTICK_BUTTON_DOWN" : "EVENT_JOYSTICK_JOYSTICK_UP", event.button);
	event_send((d_event *)&event);
}

void joy_hat_handler(SDL_JoyHatEvent *jhe)
{
	int hat = SDL_Joysticks[jhe->which].hat_map[jhe->hat];
	int hbi;
	d_event_joystickbutton event;

	//Save last state of the hat-button
	Joystick.button_last_state[hat  ] = Joystick.button_state[hat  ];
	Joystick.button_last_state[hat+1] = Joystick.button_state[hat+1];
	Joystick.button_last_state[hat+2] = Joystick.button_state[hat+2];
	Joystick.button_last_state[hat+3] = Joystick.button_state[hat+3];

	//get current state of the hat-button
	Joystick.button_state[hat  ] = ((jhe->value & SDL_HAT_UP)>0);
	Joystick.button_state[hat+1] = ((jhe->value & SDL_HAT_RIGHT)>0);
	Joystick.button_state[hat+2] = ((jhe->value & SDL_HAT_DOWN)>0);
	Joystick.button_state[hat+3] = ((jhe->value & SDL_HAT_LEFT)>0);

	//determine if a hat-button up or down event based on state and last_state
	for(hbi=0;hbi<4;hbi++)
	{
		if( !Joystick.button_last_state[hat+hbi] && Joystick.button_state[hat+hbi]) //last_state up, current state down
		{
			event.type = EVENT_JOYSTICK_BUTTON_DOWN;
			event.button = hat+hbi;
			con_printf(CON_DEBUG, "Sending event EVENT_JOYSTICK_BUTTON_DOWN, button %d\n", event.button);
			event_send((d_event *)&event);
		}
		else if(Joystick.button_last_state[hat+hbi] && !Joystick.button_state[hat+hbi])  //last_state down, current state up
		{
			event.type = EVENT_JOYSTICK_BUTTON_UP;
			event.button = hat+hbi;
			con_printf(CON_DEBUG, "Sending event EVENT_JOYSTICK_BUTTON_UP, button %d\n", event.button);
			event_send((d_event *)&event);
		}
	}
}

int joy_axis_handler(SDL_JoyAxisEvent *jae)
{
	int axis;
	d_event_joystick_moved event;

	axis = SDL_Joysticks[jae->which].axis_map[jae->axis];

	// inaccurate stick is inaccurate. SDL might send SDL_JoyAxisEvent even if the value is the same as before.
	if (Joystick.axis_value[axis] == jae->value/256)
		return 0;

	event.type = EVENT_JOYSTICK_MOVED;
	event.axis = axis;
	event.value = Joystick.axis_value[axis] = jae->value/256;
	con_printf(CON_DEBUG, "Sending event EVENT_JOYSTICK_MOVED, axis: %d, value: %d\n",event.axis, event.value);
	event_send((d_event *)&event);

	return 1;
}

int joy_apply_deadzone(int value, int deadzone)
{
	if (value > deadzone)
		return ((value - deadzone) * 128) / (128 - deadzone);
	else if (value < -deadzone)
		return ((value + deadzone) * 128) / (128 - deadzone);
	else
		return 0;
}

static int send_axis_button_event(unsigned button, event_type e)
{
	d_event_joystickbutton event;

	Joystick.button_state[button] = (e == EVENT_JOYSTICK_BUTTON_UP) ? 0 : 1;
	event.type = e;
	event.button = button;
	con_printf(CON_DEBUG, "Sending event %sEVENT_JOYSTICK_BUTTON_DOWN, button %d\n",
		(e == EVENT_JOYSTICK_BUTTON_UP ? "EVENT_JOYSTICK_BUTTON_UP" : "EVENT_JOYSTICK_BUTTON_DOWN"), button);
	event_send((d_event *)&event);
	return 1;
}

// SNG: hysteresis, so an axis held at the edge doesn't chatter
#define AXIS_BUTTON_PRESS	38
#define AXIS_BUTTON_RELEASE	25
#define TRIGGER_REST_RAW	-24000

static int axis_button_wanted(int level, int held)
{
	return level > (held ? AXIS_BUTTON_RELEASE : AXIS_BUTTON_PRESS);
}

static int axis_button_set(unsigned button, int wanted)
{
	if (Joystick.button_state[button] == wanted)
		return 0;
	return send_axis_button_event(button, wanted ? EVENT_JOYSTICK_BUTTON_DOWN : EVENT_JOYSTICK_BUTTON_UP);
}

// A trigger is one button from rest to full pull; a resting trigger would
// otherwise hold its negative half down.
int joy_axisbutton_handler(SDL_JoyAxisEvent *jae)
{
	unsigned positive = SDL_Joysticks[jae->which].axis_button_map[jae->axis];
	unsigned negative = positive + 1;
	int level = jae->value / 256;
	int sent = 0;

	if (SDL_Joysticks[jae->which].axis_is_trigger[jae->axis])
		level = (level + 128) / 2;

	sent |= axis_button_set(positive, axis_button_wanted(level, Joystick.button_state[positive]));
	sent |= axis_button_set(negative, axis_button_wanted(-level, Joystick.button_state[negative]));
	return sent;
}


// SNG: menus eat axis events; keep the values current for binding baselines
void joy_axis_track(SDL_JoyAxisEvent *jae)
{
	Joystick.axis_value[SDL_Joysticks[jae->which].axis_map[jae->axis]] = jae->value / 256;
}

int joy_axis_get(int axis)
{
	return Joystick.axis_value[axis];
}

static int axis_owner(int axis, int *joy, int *raw)
{
	for (*joy = 0; *joy < num_joysticks; (*joy)++)
		for (*raw = 0; *raw < SDL_Joysticks[*joy].n_axes; (*raw)++)
			if (SDL_Joysticks[*joy].axis_map[*raw] == axis)
				return 1;
	return 0;
}

// SNG: how far one half of an axis is pushed, 0..128; a trigger's half is its pull
int joy_axis_half_level(int axis, int positive)
{
	int joy, raw, level = Joystick.axis_value[axis];

	if (axis_owner(axis, &joy, &raw) && SDL_Joysticks[joy].axis_is_trigger[raw])
		level = (level + 128) / 2;
	return max(0, positive ? level : -level);
}

int joy_axis_half_button(int axis, int positive)
{
	int joy, raw;

	if (!axis_owner(axis, &joy, &raw))
		return -1;
	return SDL_Joysticks[joy].axis_button_map[raw] + (positive ? 0 : 1);
}

int joy_button_is_axis(int button)
{
	int joy, raw;

	for (joy = 0; joy < num_joysticks; joy++)
		for (raw = 0; raw < SDL_Joysticks[joy].n_axes; raw++)
			if (button - SDL_Joysticks[joy].axis_button_map[raw] == 0 || button - SDL_Joysticks[joy].axis_button_map[raw] == 1)
				return 1;
	return 0;
}

// SNG: a paddle that an onboard profile maps (Xbox Elite) reports itself and
// the button it copies in the same instant. A button that never arrives
// without the same lower-numbered partner is such a paddle; drop the copy.
#define MIRROR_UNSEEN	-2
#define MIRROR_NONE	-1
#define MIRROR_CONFIRM	2	// presses before play trusts the pair; a bind trusts the first
#define MIRROR_BATCH	64

static struct {
	signed char partner[MAX_BUTTONS_PER_JOYSTICK];
	ubyte together[MAX_BUTTONS_PER_JOYSTICK];
	ubyte masked[MAX_BUTTONS_PER_JOYSTICK];
	ubyte drop_down[MAX_BUTTONS_PER_JOYSTICK];
	ubyte down_now[MAX_BUTTONS_PER_JOYSTICK];
} Mirror[MAX_JOYSTICKS];

static void mirror_reset(void)
{
	int joy;

	memset(Mirror, 0, sizeof(Mirror));
	for (joy = 0; joy < MAX_JOYSTICKS; joy++)
		memset(Mirror[joy].partner, MIRROR_UNSEEN, sizeof(Mirror[joy].partner));
}

static int mirror_peek(SDL_Event *batch)
{
#if SDL_VERSION_ATLEAST(2, 0, 0)
	return SDL_PeepEvents(batch, MIRROR_BATCH, SDL_PEEKEVENT, SDL_JOYBUTTONDOWN, SDL_JOYBUTTONDOWN);
#else
	return SDL_PeepEvents(batch, MIRROR_BATCH, SDL_PEEKEVENT, SDL_JOYBUTTONDOWNMASK);
#endif
}

static int mirror_lower_down(int joy, int button)
{
	int partner = Mirror[joy].partner[button], lower;

	if (partner >= 0)
		return Mirror[joy].down_now[partner] ? partner : -1;
	for (lower = button - 1; lower >= 0; lower--)
		if (Mirror[joy].down_now[lower])
			return lower;
	return -1;
}

static void mirror_learn(int joy, int button, int binding)
{
	signed char *partner = &Mirror[joy].partner[button];
	int lower = mirror_lower_down(joy, button);

	if (*partner == MIRROR_NONE)
		return;
	if (lower < 0)
	{
		*partner = MIRROR_NONE;	// pressed alone: a real button
		return;
	}
	*partner = lower;
	if (Mirror[joy].together[button] < 255)
		Mirror[joy].together[button]++;
	if (binding || Mirror[joy].together[button] >= MIRROR_CONFIRM)
		Mirror[joy].drop_down[lower] = 1;
}

// once per poll, before the events are handled
void joy_mirror_scan(int binding)
{
	SDL_Event batch[MIRROR_BATCH];
	int count, i, joy, button;

	SDL_PumpEvents();
	count = mirror_peek(batch);

	for (joy = 0; joy < num_joysticks; joy++)
	{
		memset(Mirror[joy].down_now, 0, sizeof(Mirror[joy].down_now));
		memset(Mirror[joy].drop_down, 0, sizeof(Mirror[joy].drop_down));
	}
	for (i = 0; i < count; i++)
		if (batch[i].jbutton.which < num_joysticks && batch[i].jbutton.button < MAX_BUTTONS_PER_JOYSTICK)
			Mirror[batch[i].jbutton.which].down_now[batch[i].jbutton.button] = 1;
	for (joy = 0; joy < num_joysticks; joy++)
		for (button = 0; button < SDL_Joysticks[joy].n_buttons; button++)
			if (Mirror[joy].down_now[button])
				mirror_learn(joy, button, binding);
}

int joy_mirror_drop(const SDL_Event *event)
{
	int joy, button;

	if (event->type != SDL_JOYBUTTONDOWN && event->type != SDL_JOYBUTTONUP)
		return 0;
	joy = event->jbutton.which;
	button = event->jbutton.button;
	if (joy >= num_joysticks || button >= MAX_BUTTONS_PER_JOYSTICK)
		return 0;
	if (event->type == SDL_JOYBUTTONDOWN && Mirror[joy].drop_down[button])
	{
		Mirror[joy].masked[button] = 1;
		return 1;
	}
	if (event->type == SDL_JOYBUTTONUP && Mirror[joy].masked[button])
	{
		Mirror[joy].masked[button] = 0;
		return 1;
	}
	return 0;
}

/* ----------------------------------------------- */

void joy_init()
{
	int i,j,n;
	char temp[10];

	if (SDL_Init(SDL_INIT_JOYSTICK) < 0) {
		con_printf(CON_NORMAL, "sdl-joystick: initialisation failed: %s.",SDL_GetError());
		return;
	}

	memset(&Joystick,0,sizeof(Joystick));
	mirror_reset();
	memset(joyaxis_text, 0, JOY_MAX_AXES * sizeof(char *));
	memset(joybutton_text, 0, JOY_MAX_BUTTONS * sizeof(char *));

	n = SDL_NumJoysticks();

	con_printf(CON_NORMAL, "sdl-joystick: found %d joysticks\n", n);
	for (i = 0; i < n; i++) {
		con_printf(CON_NORMAL, "sdl-joystick %d: %s\n", i, SDL_JoystickName(i));
		SDL_Joysticks[num_joysticks].handle = SDL_JoystickOpen(i);
		if (SDL_Joysticks[num_joysticks].handle) {

			SDL_Joysticks[num_joysticks].n_axes
				= SDL_JoystickNumAxes(SDL_Joysticks[num_joysticks].handle);
			if(SDL_Joysticks[num_joysticks].n_axes > MAX_AXES_PER_JOYSTICK)
			{
				Warning("sdl-joystick: found %d axes, only %d supported.\n", SDL_Joysticks[num_joysticks].n_axes, MAX_AXES_PER_JOYSTICK);
				Warning("sdl-joystick: found %d axes, only %d supported.\n", SDL_Joysticks[num_joysticks].n_axes, MAX_AXES_PER_JOYSTICK);
				SDL_Joysticks[num_joysticks].n_axes = MAX_AXES_PER_JOYSTICK;
			}

			SDL_Joysticks[num_joysticks].n_buttons
				= SDL_JoystickNumButtons(SDL_Joysticks[num_joysticks].handle);
			if(SDL_Joysticks[num_joysticks].n_buttons > MAX_BUTTONS_PER_JOYSTICK)
			{
				Warning("sdl-joystick: found %d buttons, only %d supported.\n", SDL_Joysticks[num_joysticks].n_buttons, MAX_BUTTONS_PER_JOYSTICK);
				SDL_Joysticks[num_joysticks].n_buttons = MAX_BUTTONS_PER_JOYSTICK;
			}

			SDL_Joysticks[num_joysticks].n_hats
				= SDL_JoystickNumHats(SDL_Joysticks[num_joysticks].handle);
			if(SDL_Joysticks[num_joysticks].n_hats > MAX_HATS_PER_JOYSTICK)
			{
				Warning("sdl-joystick: found %d hats, only %d supported.\n", SDL_Joysticks[num_joysticks].n_hats, MAX_HATS_PER_JOYSTICK);
				SDL_Joysticks[num_joysticks].n_hats = MAX_HATS_PER_JOYSTICK;
			}

			con_printf(CON_NORMAL, "sdl-joystick: %d axes\n", SDL_Joysticks[num_joysticks].n_axes);
			con_printf(CON_NORMAL, "sdl-joystick: %d buttons\n", SDL_Joysticks[num_joysticks].n_buttons);
			con_printf(CON_NORMAL, "sdl-joystick: %d hats\n", SDL_Joysticks[num_joysticks].n_hats);

			for (j=0; j < SDL_Joysticks[num_joysticks].n_axes; j++)
			{
				sprintf(temp, "J%d A%d", i + 1, j + 1);
				joyaxis_text[Joystick.n_axes] = d_strdup(temp);
				SDL_Joysticks[num_joysticks].axis_map[j] = Joystick.n_axes++;
			}
			for (j=0; j < SDL_Joysticks[num_joysticks].n_buttons; j++)
			{
				sprintf(temp, "J%d B%d", i + 1, j + 1);
				joybutton_text[Joystick.n_buttons] = d_strdup(temp);
				SDL_Joysticks[num_joysticks].button_map[j] = Joystick.n_buttons++;
			}
			for (j=0; j < SDL_Joysticks[num_joysticks].n_hats; j++)
			{
				if (Joystick.n_buttons + 4 > MAX_BUTTONS_PER_JOYSTICK)
					break;
				SDL_Joysticks[num_joysticks].hat_map[j] = Joystick.n_buttons;
				//a hat counts as four buttons
				sprintf(temp, "J%d H%d%c", i + 1, j + 1, 0202);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
				sprintf(temp, "J%d H%d%c", i + 1, j + 1, 0177);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
				sprintf(temp, "J%d H%d%c", i + 1, j + 1, 0200);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
				sprintf(temp, "J%d H%d%c", i + 1, j + 1, 0201);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
			}
			SDL_JoystickUpdate();	// fresh resting values for trigger detection
			for (j=0; j < SDL_Joysticks[num_joysticks].n_axes; j++)
			{
				if (Joystick.n_buttons + 2 > MAX_BUTTONS_PER_JOYSTICK)
					break;
				SDL_Joysticks[num_joysticks].axis_button_map[j] = Joystick.n_buttons;
				SDL_Joysticks[num_joysticks].axis_is_trigger[j] =
					SDL_JoystickGetAxis(SDL_Joysticks[num_joysticks].handle, j) < TRIGGER_REST_RAW;
				//an axis count as 2 buttons: positive +, then negative -
				sprintf(temp, "J%d +A%d", i + 1, j + 1);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
				sprintf(temp, "J%d -A%d", i + 1, j + 1);
				joybutton_text[Joystick.n_buttons++] = d_strdup(temp);
			}

			num_joysticks++;
		}
		else
			con_printf(CON_NORMAL, "sdl-joystick: initialization failed!\n");

		con_printf(CON_NORMAL, "sdl-joystick: %d axes (total)\n", Joystick.n_axes);
		con_printf(CON_NORMAL, "sdl-joystick: %d buttons (total)\n", Joystick.n_buttons);
	}

	joy_num_axes = Joystick.n_axes;
}

void joy_close()
{
	SDL_JoystickClose(SDL_Joysticks[num_joysticks].handle);

	while (Joystick.n_axes--)
		d_free(joyaxis_text[Joystick.n_axes]);
	while (Joystick.n_buttons--)
		d_free(joybutton_text[Joystick.n_buttons]);
}

void event_joystick_get_axis(d_event *event, int *axis, int *value)
{
	Assert(event->type == EVENT_JOYSTICK_MOVED);

	*axis  = ((d_event_joystick_moved *)event)->axis;
	*value = ((d_event_joystick_moved *)event)->value;
}

void joy_flush()
{
	int i;

	if (!num_joysticks)
		return;

	for (i = 0; i < Joystick.n_buttons; i++)
		Joystick.button_state[i] = SDL_RELEASED;
}

int event_joystick_get_button(d_event *event)
{
	Assert((event->type == EVENT_JOYSTICK_BUTTON_DOWN) || (event->type == EVENT_JOYSTICK_BUTTON_UP));
	return ((d_event_joystickbutton *)event)->button;
}
