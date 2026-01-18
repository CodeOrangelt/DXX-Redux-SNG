/*
 *
 * SDL library timer functions
 *
 */

#include <SDL.h>

#include "maths.h"
#include "timer.h"
#include "config.h"

static fix64 F64_RunTime = 0;
static int64_t usec_runtime = 0;

#ifdef WIN32
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 2
#endif

static DWORD create_timer_flags = 0;
static LARGE_INTEGER freq, start;

void timer_init(void)
{
	HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
	                                      TIMER_ALL_ACCESS);
	if (timer) {
		create_timer_flags = CREATE_WAITABLE_TIMER_HIGH_RESOLUTION;
        CloseHandle(timer);
	} else
		create_timer_flags = 0;
	if (!QueryPerformanceCounter(&start) || !QueryPerformanceFrequency(&freq))
		Error("QueryPerformanceCounter not working");
}

void timer_delay_usec(int64_t usec)
{
	HANDLE timer;
	LARGE_INTEGER timeout;

	if (usec <= 0)
		return;

	timer = CreateWaitableTimerExW(NULL, NULL, create_timer_flags, TIMER_ALL_ACCESS);
	if (!create_timer_flags && (timer = CreateWaitableTimerExW(NULL, NULL, 0, TIMER_ALL_ACCESS)) &&
		create_timer_flags) {
		create_timer_flags = 0;
		timer = CreateWaitableTimerExW(NULL, NULL, create_timer_flags, TIMER_ALL_ACCESS);
	}

	if (!timer) {
		Sleep(usec / 1000);
		return;
	}

	timeout.QuadPart = -(usec * 10); // negative for relative timeout
	if (!SetWaitableTimerEx(timer, &timeout, 0, NULL, NULL, NULL, 0)) {
		CloseHandle(timer);
		Sleep(usec / 1000);
		return;
	}

	WaitForSingleObject(timer, INFINITE);
	CloseHandle(timer);
}

int64_t timer_query_usec(void)
{
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return (now.QuadPart - start.QuadPart) * 1000000 / freq.QuadPart;
}
#else
#include <time.h>

static struct timespec start;

void timer_init(void)
{
	if (clock_gettime(CLOCK_MONOTONIC, &start))
		Error("clock_gettime failed");
}

void timer_delay_usec(int64_t usec)
{
	struct timespec ts;

	if (usec <= 0)
		return;

	ts.tv_sec = usec / 1000000;
	ts.tv_nsec = (usec % 1000000) * 1000;
	nanosleep(&ts, NULL);
}

int64_t timer_query_usec(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)(now.tv_sec - start.tv_sec) * 1000000 + (now.tv_nsec - start.tv_nsec) / 1000;
}
#endif

void timer_update(void)
{
	usec_runtime = timer_query_usec();
	F64_RunTime = usec_runtime * F1_0 / 1000000;
}

fix64 timer_query(void)
{
	return (F64_RunTime);
}

void timer_delay(fix seconds)
{
	SDL_Delay(f2i(fixmul(seconds, i2f(1000))));
}

// Replacement for timer_delay which considers calc time the program needs between frames (not reentrant)
void timer_delay2(int fps)
{
	static u_int32_t FrameStart=0;
	u_int32_t FrameLoop=0;

	while (FrameLoop < 1000/(GameCfg.VSync?MAXIMUM_FPS:fps))
	{
		u_int32_t tv_now = SDL_GetTicks();
		if (FrameStart > tv_now)
			FrameStart = tv_now;
		if (!GameCfg.VSync)
			SDL_Delay(1);
		FrameLoop=tv_now-FrameStart;
	}

	FrameStart=SDL_GetTicks();
}
