/*
 * This is an alternate backend for the sound effect system.
 * It uses SDL_mixer to provide a more reliable playback,
 * and allow processing of multiple audio formats.
 *
 * This file is based on the original D1X arch/sdl/digi.c
 *
 *  -- MD2211 (2006-10-12)
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>
#include <SDL_audio.h>
#include <SDL_mixer.h>

#include "pstypes.h"
#include "dxxerror.h"
#include "sounds.h"
#include "digi.h"
#include "digi_mixer.h"
#include "digi_mixer_music.h"
#include "console.h"
#include "config.h"
#include "args.h"

#include "fix.h"
#include "gr.h" // needed for piggy.h
#include "piggy.h"

#define MIX_DIGI_DEBUG 0
#define MIX_OUTPUT_FORMAT	AUDIO_S16
#define MIX_OUTPUT_CHANNELS	2

#define MAX_SOUND_SLOTS 64
#if !((defined(__APPLE__) && defined(__MACH__)) || defined(macintosh))
#define SOUND_BUFFER_SIZE 2048
#else
#define SOUND_BUFFER_SIZE 1024
#endif
#define MIN_VOLUME 10

static int digi_initialised = 0;
static int digi_max_channels = MAX_SOUND_SLOTS;
static inline int fix2byte(fix f) { return f < 0 ? 0 : f >= 65536 ? 255 : f / 256; }
Mix_Chunk SoundChunks[MAX_SOUNDS];
ubyte channels[MAX_SOUND_SLOTS];

// Temp chunks created for pitched playback; freed when the channel finishes.
static Mix_Chunk *pitched_chunks[MAX_SOUND_SLOTS];

#ifdef __linux__
static int digi_mixer_check_soundfont(const char *path, void *data)
{
	FILE *file = fopen(path, "r");
	if (!file)
		return 0;
	fclose(file);
	return 1;
}
#endif

/* Initialise audio */
int digi_mixer_init()
{
	if (MIX_DIGI_DEBUG) con_printf(CON_DEBUG,"digi_init %d (SDL_Mixer)\n", MAX_SOUNDS);
	if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) Error("SDL audio initialisation failed: %s.", SDL_GetError());

	#ifdef __linux__
	// Use the soundfont in the AppImage if no other sound font specified
	Mix_Init(0); // hack to set soundfont_paths on Debian patched SDL-mixer
	if (!Mix_EachSoundFont(digi_mixer_check_soundfont, NULL) && getenv("APPDIR"))
	{
		char soundfonts[PATH_MAX];
		snprintf(soundfonts, sizeof(soundfonts),
			"%s/usr/share/sounds/sf3/default-GM.sf3", getenv("APPDIR"));
		Mix_SetSoundFonts(soundfonts);
	}
	#endif

	if (Mix_OpenAudio(SAMPLE_RATE_44K, MIX_OUTPUT_FORMAT, MIX_OUTPUT_CHANNELS, SOUND_BUFFER_SIZE))
	{
		//edited on 10/05/98 by Matt Mueller - should keep running, just with no sound.
		con_printf(CON_URGENT,"\nError: Couldn't open audio: %s\n", SDL_GetError());
		GameArg.SndNoSound = 1;
		return 1;
	}

	digi_max_channels = Mix_AllocateChannels(digi_max_channels);
	memset(channels, 0, MAX_SOUND_SLOTS);
	Mix_Pause(0);

	digi_initialised = 1;

	digi_mixer_set_digi_volume( (GameCfg.DigiVolume*32768)/8 );

	return 0;
}

/* Shut down audio */
void digi_mixer_close() {
	if (MIX_DIGI_DEBUG) con_printf(CON_DEBUG,"digi_close (SDL_Mixer)\n");
	if (!digi_initialised) return;
	digi_initialised = 0;
	Mix_CloseAudio();
}

/* channel management */
int digi_mixer_find_channel()
{
	int i;
	for (i = 0; i < digi_max_channels; i++)
		if (channels[i] == 0)
			return i;
	return -1;
}

void digi_mixer_free_channel(int channel_num)
{
	if (pitched_chunks[channel_num])
	{
		Mix_FreeChunk(pitched_chunks[channel_num]);
		pitched_chunks[channel_num] = NULL;
	}
	channels[channel_num] = 0;
}

/*
 * Play-time conversion. Performs output conversion only once per sound effect used.
 * Once the sound sample has been converted, it is cached in SoundChunks[]
 */
void mixdigi_convert_sound(int i)
{
	SDL_AudioCVT cvt;
	Uint8 *data = GameSounds[i].data;
	Uint32 dlen = GameSounds[i].length;
	int out_freq;
	Uint16 out_format;
	int out_channels;

	Mix_QuerySpec(&out_freq, &out_format, &out_channels); // get current output settings

	if (SoundChunks[i].abuf) return; //proceed only if not converted yet

	if (data)
	{
		if (MIX_DIGI_DEBUG) con_printf(CON_DEBUG,"converting %d (%d)\n", i, dlen);
		SDL_BuildAudioCVT(&cvt, AUDIO_U8, 1, GameArg.SndDigiSampleRate, out_format, out_channels, out_freq);

		cvt.buf = malloc(dlen * cvt.len_mult);
		cvt.len = dlen;
		memcpy(cvt.buf, data, dlen);
		if (SDL_ConvertAudio(&cvt)) con_printf(CON_DEBUG,"conversion of %d failed\n", i);

		SoundChunks[i].abuf = cvt.buf;
		SoundChunks[i].alen = cvt.len_cvt;
		SoundChunks[i].allocated = 1;
		SoundChunks[i].volume = 128; // Max volume = 128
	}
}

// Volume 0-F1_0
int digi_mixer_start_sound(short soundnum, fix volume, int pan, int looping, int loop_start, int loop_end, int soundobj)
{
	int mix_vol = fix2byte(fixmul(digi_volume, volume));
	int mix_pan = fix2byte(pan);
	int mix_loop = looping * -1;
	int channel;

	if (!digi_initialised) return -1;
	Assert(GameSounds[soundnum].data != (void *)-1);

	mixdigi_convert_sound(soundnum);

	if (MIX_DIGI_DEBUG) con_printf(CON_DEBUG,"digi_start_sound %d, volume %d, pan %d (start=%d, end=%d)\n", soundnum, mix_vol, mix_pan, loop_start, loop_end);

	channel = digi_mixer_find_channel();
	if (channel == -1)
		return -1;

	Mix_PlayChannel(channel, &(SoundChunks[soundnum]), mix_loop);
	Mix_SetPanning(channel, 255-mix_pan, mix_pan);
	if (volume > F1_0)
		Mix_SetDistance(channel, 0);
	else
		Mix_SetDistance(channel, 255-mix_vol);
	channels[channel] = 1;
	Mix_ChannelFinished(digi_mixer_free_channel);

	return channel;
}

void digi_mixer_set_channel_volume(int channel, int volume)
{
	int mix_vol = fix2byte(volume);
	if (!digi_initialised) return;
	Mix_SetDistance(channel, 255-mix_vol);
}

void digi_mixer_set_channel_pan(int channel, int pan)
{
	int mix_pan = fix2byte(pan);
	Mix_SetPanning(channel, 255-mix_pan, mix_pan);
}

void digi_mixer_stop_sound(int channel) {
	if (!digi_initialised) return;
	if (MIX_DIGI_DEBUG) con_printf(CON_DEBUG,"digi_stop_sound %d\n", channel);
	Mix_HaltChannel(channel);
	channels[channel] = 0;
}

void digi_mixer_end_sound(int channel)
{
	digi_mixer_stop_sound(channel);
	channels[channel] = 0;
}

void digi_mixer_set_digi_volume( int dvolume )
{
	digi_volume = dvolume;
	if (!digi_initialised) return;
	Mix_Volume(-1, fix2byte(dvolume));
}

int digi_mixer_is_sound_playing(int soundno) { return 0; }
int digi_mixer_is_channel_playing(int channel) { return 0; }

// Plays `soundnum` with playback speed scaled by `speed` (F1_0 = normal pitch).
// A higher speed value raises the pitch; lower lowers it.
int digi_mixer_start_sound_pitched(short soundnum, fix volume, int pan, fix speed)
{
	SDL_AudioCVT cvt;
	Uint8 *data = GameSounds[soundnum].data;
	Uint32 dlen = GameSounds[soundnum].length;
	int out_freq;
	Uint16 out_format;
	int out_channels;
	int src_rate;
	Mix_Chunk *chunk;
	int channel;
	int mix_vol = fix2byte(fixmul(digi_volume, volume));
	int mix_pan = fix2byte(pan);

	if (!digi_initialised) return -1;
	if (!data || data == (void *)-1) return -1;

	Mix_QuerySpec(&out_freq, &out_format, &out_channels);

	// Treating the source as recorded at a higher rate makes SDL downsample
	// to output rate, which plays back faster (higher pitch).
	src_rate = (int)(((long long)GameArg.SndDigiSampleRate * speed) >> 16);
	if (src_rate < 100) src_rate = 100;

	if (SDL_BuildAudioCVT(&cvt, AUDIO_U8, 1, src_rate, out_format, out_channels, out_freq) < 0)
		return -1;

	// SDL_malloc/SDL_free rather than the C library's: Mix_FreeChunk() (used
	// both below on failure and by digi_mixer_free_channel() once this
	// finishes playing) frees a chunk's abuf and the chunk itself with
	// SDL_free() internally. On a build where SDL's allocator has been
	// overridden (SDL_SetMemoryFunctions(), some packaging setups) that
	// heap need not be the same as malloc()'s, and freeing one allocator's
	// pointer through the other is undefined behaviour.
	chunk = (Mix_Chunk *)SDL_malloc(sizeof(Mix_Chunk));
	if (!chunk) return -1;

	cvt.buf = (Uint8 *)SDL_malloc(dlen * cvt.len_mult);
	if (!cvt.buf) { SDL_free(chunk); return -1; }

	cvt.len = dlen;
	memcpy(cvt.buf, data, dlen);
	if (SDL_ConvertAudio(&cvt)) { SDL_free(cvt.buf); SDL_free(chunk); return -1; }

	chunk->abuf = cvt.buf;
	chunk->alen = cvt.len_cvt;
	chunk->allocated = 1;
	chunk->volume = 128;

	channel = digi_mixer_find_channel();
	if (channel == -1) { Mix_FreeChunk(chunk); return -1; }

	pitched_chunks[channel] = chunk;

	// If this fails, nothing will ever call digi_mixer_free_channel() for
	// this channel (Mix_ChannelFinished() only fires for a channel that
	// actually finished playing) -- free the chunk and give the slot back
	// here instead of leaking it and losing the channel for the rest of
	// the session.
	if (Mix_PlayChannel(channel, chunk, 0) == -1)
	{
		pitched_chunks[channel] = NULL;
		Mix_FreeChunk(chunk);
		return -1;
	}

	Mix_SetPanning(channel, 255 - mix_pan, mix_pan);
	if (volume > F1_0)
		Mix_SetDistance(channel, 0);
	else
		Mix_SetDistance(channel, 255 - mix_vol);
	channels[channel] = 1;
	Mix_ChannelFinished(digi_mixer_free_channel);

	return channel;
}

void digi_mixer_reset() {}
void digi_mixer_stop_all_channels()
{
	int i;

	Mix_HaltChannel(-1);

	// Free any outstanding pitched chunks before clearing the table --
	// digi_mixer_free_channel() (the normal owner of this) is only
	// guaranteed to run for a channel that finishes playing on its own;
	// zeroing the table out from under a halted channel without freeing
	// its chunk first leaks it.
	for (i = 0; i < MAX_SOUND_SLOTS; i++)
		if (pitched_chunks[i])
		{
			Mix_FreeChunk(pitched_chunks[i]);
			pitched_chunks[i] = NULL;
		}

	memset(channels, 0, MAX_SOUND_SLOTS);
}

extern void digi_end_soundobj(int channel);

 //added on 980905 by adb to make sound channel setting work
void digi_mixer_set_max_channels(int n) { }
int digi_mixer_get_max_channels() { return digi_max_channels; }
// end edit by adb

#ifndef NDEBUG
void digi_mixer_debug() {}
#endif
