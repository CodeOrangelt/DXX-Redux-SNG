// Holds the main init and de-init functions for arch-related program parts

#include <SDL.h>
#include "songs.h"
#include "key.h"
#include "digi.h"
#include "mouse.h"
#include "joy.h"
#include "gr.h"
#include "dxxerror.h"
#include "text.h"
#include "args.h"
#include "config.h"
#include "timer.h"

void arch_close(void)
{
	songs_uninit();

	gr_close();

	if (!GameArg.CtlNoJoystick)
		joy_close();

	if (!GameArg.CtlNoMouse)
		mouse_close();

	if (!GameArg.SndNoSound)
	{
		digi_close();
	}

	key_close();

	SDL_Quit();
}

// SNG: sdl12-compat picks its monitor from these at SDL_Init; a user's own env wins
static void select_display(void)
{
	static char display_env[48];

	if (!SDL_getenv("SDL_VIDEO_FULLSCREEN_DISPLAY"))
	{
		snprintf(display_env, sizeof(display_env), "SDL_VIDEO_FULLSCREEN_DISPLAY=%d", GameCfg.Display);
		SDL_putenv(display_env);
	}
	if (!SDL_getenv("SDL_VIDEO_WINDOW_POS") && !SDL_getenv("SDL_VIDEO_CENTERED"))
		SDL_putenv("SDL_VIDEO_CENTERED=1");
}

void arch_init(void)
{
	int t;

	select_display();

	if (SDL_Init(SDL_INIT_VIDEO) < 0)
		Error("SDL library initialisation failed: %s.",SDL_GetError());

	timer_init();

	key_init();

	digi_select_system( GameArg.SndDisableSdlMixer ? SDLAUDIO_SYSTEM : SDLMIXER_SYSTEM );

	if (!GameArg.SndNoSound)
		digi_init();

	if (!GameArg.CtlNoMouse)
		mouse_init();

	if (!GameArg.CtlNoJoystick)
		joy_init();

	if ((t = gr_init(0)) != 0)
		Error(TXT_CANT_INIT_GFX,t);

	atexit(arch_close);
}

