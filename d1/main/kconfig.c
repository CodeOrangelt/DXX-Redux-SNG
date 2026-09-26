/*
THE COMPUTER CODE CONTAINED HEREIN IS THE SOLE PROPERTY OF PARALLAX
SOFTWARE CORPORATION ("PARALLAX").  PARALLAX, IN DISTRIBUTING THE CODE TO
END-USERS, AND SUBJECT TO ALL OF THE TERMS AND CONDITIONS HEREIN, GRANTS A
ROYALTY-FREE, PERPETUAL LICENSE TO SUCH END-USERS FOR USE BY SUCH END-USERS
IN USING, DISPLAYING,  AND CREATING DERIVATIVE WORKS THEREOF, SO LONG AS
SUCH USE, DISPLAY OR CREATION IS FOR NON-COMMERCIAL, ROYALTY OR REVENUE
FREE PURPOSES.  IN NO EVENT SHALL THE END-USER USE THE COMPUTER CODE
CONTAINED HEREIN FOR REVENUE-BEARING PURPOSES.  THE END-USER UNDERSTANDS
AND AGREES TO THE TERMS HEREIN AND ACCEPTS THE SAME BY USE OF THIS FILE.
COPYRIGHT 1993-1998 PARALLAX SOFTWARE CORPORATION.  ALL RIGHTS RESERVED.
*/

/*
 *
 * Routines to configure keyboard, joystick, etc..
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

#include "dxxerror.h"
#include "pstypes.h"
#include "gr.h"
#include "window.h"
#include "console.h"
#include "palette.h"
#include "game.h"
#include "gamefont.h"
#include "iff.h"
#include "u_mem.h"
#include "kconfig.h"
#include "gauges.h"
#include "rbaudio.h"
#include "render.h"
#include "digi.h"
#include "newmenu.h"
#include "nk_ui.h"
#include "endlevel.h"
#include "multi.h"
#include "timer.h"
#include "text.h"
#include "player.h"
#include "menu.h"
#include "automap.h"
#include "args.h"
#include "lighting.h"
#include "ai.h"
#include "cntrlcen.h"
#include "collide.h"
#include "playsave.h"

#ifdef OGL
#include "ogl_init.h"
#endif

#define TABLE_CREATION 1

static const char invert_text[2][2] = { "N", "Y" };
char *joybutton_text[JOY_MAX_BUTTONS];
char *joyaxis_text[JOY_MAX_AXES];
static const char mouseaxis_text[][8] = { "L/R", "F/B", "WHEEL" };
static const char mousebutton_text[][8] = { "LEFT", "RIGHT", "MID", "M4", "M5", "M6", "M7", "M8", "M9", "M10","M11","M12","M13","M14","M15","M16" };

static const ubyte system_keys[19] = { KEY_ESC, KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12, KEY_MINUS, KEY_EQUAL, KEY_PRINT_SCREEN, KEY_CAPSLOCK, KEY_SCROLLOCK, KEY_NUMLOCK }; // KEY_*LOCK should always be last since we wanna skip these if -nostickykeys

control_info Controls;

fix Cruise_speed=0;

// Global accumulators for SNG mouse debug display
float accum_x = 0.0f, accum_y = 0.0f;
// SNG Mouse smoothing filter for reducing jitter
static float smooth_x = 0.0f, smooth_y = 0.0f;

#define BT_KEY 			0
#define BT_MOUSE_BUTTON 	1
#define BT_MOUSE_AXIS		2
#define BT_JOY_BUTTON 		3
#define BT_JOY_AXIS		4
#define BT_INVERT		5
#define STATE_BIT1		1
#define STATE_BIT2		2
#define STATE_BIT3		4
#define STATE_BIT4		8
#define STATE_BIT5		16

#define INFO_Y (188)

typedef struct kc_item {
	const short id;				// The id of this item
	const short x, y;              // x, y pos of label
	const short w1;                // x pos of input field
	const short w2;                // length of input field
#ifndef TABLE_CREATION
	const
#endif
	short u,d,l,r;           // neighboring field ids for cursor navigation
        //short text_num1;
        const char *const text;
	const ubyte type;
	ubyte value;		// what key,button,etc
	ubyte *const ci_state_ptr;
	const int state_bit;
	ubyte *const ci_count_ptr;
} kc_item;

#define KC_MAX_SLOTS NK_UI_BIND_MAX_SLOTS
#define KC_MAX_ROWS 32
#define KC_BINDING_LEN 12

// One action and the binding slots that drive it.
typedef struct kc_row
{
	const char	*label;
	short	slot[KC_MAX_SLOTS];
	short	nslots;
} kc_row;

typedef struct kc_menu
{
	window	*wind;
	kc_item	*items;
	const char	*title;
	int	nitems;
	int	citem;
	kc_row	rows[KC_MAX_ROWS];
	int	nrows;
	int	old_jaxis[JOY_MAX_AXES];
	int	old_maxis[3];
	ubyte	changing;
	ubyte	mouse_state;
} kc_menu;

const ubyte DefaultKeySettings[3][MAX_CONTROLS] = {
{0xc8,0x48,0xd0,0x50,0xcb,0x4b,0xcd,0x4d,0x38,0xff,0xff,0x4f,0xff,0x51,0xff,0x4a,0xff,0x4e,0xff,0xff,0x10,0x47,0x12,0x49,0x1d,0x9d,0x39,0xff,0x21,0xff,0x1e,0xff,0x2c,0xff,0x30,0xff,0x13,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xf,0xff,0x33,0x0,0x34,0x0},
{0x0,0x1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x1,0x0,0x0,0x0,0xff,0x0,0xff,0x0,0xff,0x0,0xff,0x0,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x0,0x0},
{0x0,0x1,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0x1,0x0,0x0,0x0,0xff,0x0,0xff,0x0,0xff,0x0,0xff,0x0,0xff,0xff,0xff,0xff,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0,0x0},
};
const ubyte DefaultKeySettingsD1X[MAX_D1X_CONTROLS] = { 0x2,0xff,0xff,0x3,0xff,0xff,0x4,0xff,0xff,0x5,0xff,0xff,0x6,0xff,0xff,0x7,0xff,0xff,0x8,0xff,0xff,0x9,0xff,0xff,0xa,0xff,0xff,0xb,0xff,0xff };

//	  id,  x,  y, w1, w2,  u,  d,   l, r,     text,   type, value
kc_item kc_keyboard[NUM_KEY_CONTROLS] = {
	{  0, 15, 49, 71, 26, 43,  2, 49,  1,"Pitch forward", BT_KEY, 255, &Controls.key_pitch_forward_state, STATE_BIT1, NULL },
	{  1, 15, 49,100, 26, 48,  3,  0, 24,"Pitch forward", BT_KEY, 255, &Controls.key_pitch_forward_state, STATE_BIT2, NULL },
	{  2, 15, 57, 71, 26,  0,  4, 25,  3,"Pitch backward", BT_KEY, 255, &Controls.key_pitch_backward_state, STATE_BIT1, NULL },
	{  3, 15, 57,100, 26,  1,  5,  2, 26,"Pitch backward", BT_KEY, 255, &Controls.key_pitch_backward_state, STATE_BIT2, NULL },
	{  4, 15, 65, 71, 26,  2,  6, 27,  5,"Turn left", BT_KEY, 255, &Controls.key_heading_left_state, STATE_BIT1, NULL },
	{  5, 15, 65,100, 26,  3,  7,  4, 28,"Turn left", BT_KEY, 255, &Controls.key_heading_left_state, STATE_BIT2, NULL },
	{  6, 15, 73, 71, 26,  4,  8, 29,  7,"Turn right", BT_KEY, 255, &Controls.key_heading_right_state, STATE_BIT1, NULL },
	{  7, 15, 73,100, 26,  5,  9,  6, 34,"Turn right", BT_KEY, 255, &Controls.key_heading_right_state, STATE_BIT2, NULL },
	{  8, 15, 85, 71, 26,  6, 10, 35,  9,"Slide on", BT_KEY, 255, &Controls.slide_on_state, STATE_BIT1, NULL },
	{  9, 15, 85,100, 26,  7, 11,  8, 36,"Slide on", BT_KEY, 255, &Controls.slide_on_state, STATE_BIT2, NULL },
	{ 10, 15, 93, 71, 26,  8, 12, 37, 11,"Slide left", BT_KEY, 255, &Controls.key_slide_left_state, STATE_BIT1, NULL },
	{ 11, 15, 93,100, 26,  9, 13, 10, 44,"Slide left", BT_KEY, 255, &Controls.key_slide_left_state, STATE_BIT2, NULL },
	{ 12, 15,101, 71, 26, 10, 14, 45, 13,"Slide right", BT_KEY, 255, &Controls.key_slide_right_state, STATE_BIT1, NULL },
	{ 13, 15,101,100, 26, 11, 15, 12, 30,"Slide right", BT_KEY, 255, &Controls.key_slide_right_state, STATE_BIT2, NULL },
	{ 14, 15,109, 71, 26, 12, 16, 31, 15,"Slide up", BT_KEY, 255, &Controls.key_slide_up_state, STATE_BIT1, NULL },
	{ 15, 15,109,100, 26, 13, 17, 14, 32,"Slide up", BT_KEY, 255, &Controls.key_slide_up_state, STATE_BIT2, NULL },
	{ 16, 15,117, 71, 26, 14, 18, 33, 17,"Slide down", BT_KEY, 255, &Controls.key_slide_down_state, STATE_BIT1, NULL },
	{ 17, 15,117,100, 26, 15, 19, 16, 38,"Slide down", BT_KEY, 255, &Controls.key_slide_down_state, STATE_BIT2, NULL },
	{ 18, 15,129, 71, 26, 16, 20, 39, 19,"Bank on", BT_KEY, 255, &Controls.bank_on_state, STATE_BIT1, NULL },
	{ 19, 15,129,100, 26, 17, 21, 18, 40,"Bank on", BT_KEY, 255, &Controls.bank_on_state, STATE_BIT2, NULL },
	{ 20, 15,137, 71, 26, 18, 22, 41, 21,"Bank left", BT_KEY, 255, &Controls.key_bank_left_state, STATE_BIT1, NULL },
	{ 21, 15,137,100, 26, 19, 23, 20, 42,"Bank left", BT_KEY, 255, &Controls.key_bank_left_state, STATE_BIT2, NULL },
	{ 22, 15,145, 71, 26, 20, 46, 43, 23,"Bank right", BT_KEY, 255, &Controls.key_bank_right_state, STATE_BIT1, NULL },
	{ 23, 15,145,100, 26, 21, 47, 22, 46,"Bank right", BT_KEY, 255, &Controls.key_bank_right_state, STATE_BIT2, NULL },
	{ 24,158, 49, 83, 26, 49, 26,  1, 25,"Fire primary", BT_KEY, 255, &Controls.fire_primary_state, STATE_BIT1, &Controls.fire_primary_count },
	{ 25,158, 49,112, 26, 42, 27, 24,  2,"Fire primary", BT_KEY, 255, &Controls.fire_primary_state, STATE_BIT2, &Controls.fire_primary_count },
	{ 26,158, 57, 83, 26, 24, 28,  3, 27,"Fire secondary", BT_KEY, 255, &Controls.fire_secondary_state, STATE_BIT1, &Controls.fire_secondary_count },
	{ 27,158, 57,112, 26, 25, 29, 26,  4,"Fire secondary", BT_KEY, 255, &Controls.fire_secondary_state, STATE_BIT2, &Controls.fire_secondary_count },
	{ 28,158, 65, 83, 26, 26, 34,  5, 29,"Fire flare", BT_KEY, 255, NULL, 0, &Controls.fire_flare_count },
	{ 29,158, 65,112, 26, 27, 35, 28,  6,"Fire flare", BT_KEY, 255, NULL, 0, &Controls.fire_flare_count },
	{ 30,158,105, 83, 26, 44, 32, 13, 31,"Accelerate", BT_KEY, 255, &Controls.accelerate_state, STATE_BIT1, NULL },
	{ 31,158,105,112, 26, 45, 33, 30, 14,"Accelerate", BT_KEY, 255, &Controls.accelerate_state, STATE_BIT2, NULL },
	{ 32,158,113, 83, 26, 30, 38, 15, 33,"Reverse", BT_KEY, 255, &Controls.reverse_state, STATE_BIT1, NULL },
	{ 33,158,113,112, 26, 31, 39, 32, 16,"Reverse", BT_KEY, 255, &Controls.reverse_state, STATE_BIT2, NULL },
	{ 34,158, 73, 83, 26, 28, 36,  7, 35,"Drop bomb", BT_KEY, 255, NULL, 0, &Controls.drop_bomb_count },
	{ 35,158, 73,112, 26, 29, 37, 34,  8,"Drop bomb", BT_KEY, 255, NULL, 0, &Controls.drop_bomb_count },
	{ 36,158, 85, 83, 26, 34, 44,  9, 37,"Rear view", BT_KEY, 255, &Controls.rear_view_state, STATE_BIT1, &Controls.rear_view_count },
	{ 37,158, 85,112, 26, 35, 45, 36, 10,"Rear view", BT_KEY, 255, &Controls.rear_view_state, STATE_BIT2, &Controls.rear_view_count },
	{ 38,158,125, 83, 26, 32, 40, 17, 39,"Cruise faster", BT_KEY, 255, &Controls.cruise_plus_state, STATE_BIT1, NULL },
	{ 39,158,125,112, 26, 33, 41, 38, 18,"Cruise faster", BT_KEY, 255, &Controls.cruise_plus_state, STATE_BIT2, NULL },
	{ 40,158,133, 83, 26, 38, 42, 19, 41,"Cruise slower", BT_KEY, 255, &Controls.cruise_minus_state, STATE_BIT1, NULL },
	{ 41,158,133,112, 26, 39, 43, 40, 20,"Cruise slower", BT_KEY, 255, &Controls.cruise_minus_state, STATE_BIT2, NULL },
	{ 42,158,141, 83, 26, 40, 25, 21, 43,"Cruise off", BT_KEY, 255, NULL, 0, &Controls.cruise_off_count },
	{ 43,158,141,112, 26, 41,  0, 42, 22,"Cruise off", BT_KEY, 255, NULL, 0, &Controls.cruise_off_count },
	{ 44,158, 93, 83, 26, 36, 30, 11, 45,"Automap", BT_KEY, 255, &Controls.automap_state, STATE_BIT1, &Controls.automap_count },
	{ 45,158, 93,112, 26, 37, 31, 44, 12,"Automap", BT_KEY, 255, &Controls.automap_state, STATE_BIT2, &Controls.automap_count },
	{ 46, 15,157, 71, 26, 22, 48, 23, 47,"Cycle Primary", BT_KEY, 255, NULL, 0, &Controls.cycle_primary_count },
	{ 47, 15,157,100, 26, 23, 49, 46, 48,"Cycle Primary", BT_KEY, 255, NULL, 0, &Controls.cycle_primary_count },
	{ 48, 15,165, 71, 26, 46,  1, 47, 49,"Cycle Second.", BT_KEY, 255, NULL, 0, &Controls.cycle_secondary_count },
	{ 49, 15,165,100, 26, 47, 24, 48,  0,"Cycle Second.", BT_KEY, 255, NULL, 0, &Controls.cycle_secondary_count },
};
kc_item kc_joystick[NUM_JOYSTICK_CONTROLS] = {
	{  0, 22, 46, 82, 26, 15,  1, 24, 29,"Fire primary", BT_JOY_BUTTON, 255, &Controls.fire_primary_state, STATE_BIT3, &Controls.fire_primary_count },
	{  1, 22, 54, 82, 26,  0,  4, 34, 30,"Fire secondary", BT_JOY_BUTTON, 255, &Controls.fire_secondary_state, STATE_BIT3, &Controls.fire_secondary_count },
	{  2, 22, 78, 82, 26, 26,  3, 37, 31,"Accelerate", BT_JOY_BUTTON, 255, &Controls.accelerate_state, STATE_BIT3, NULL },
	{  3, 22, 86, 82, 26,  2, 25, 38, 32,"Reverse", BT_JOY_BUTTON, 255, &Controls.reverse_state, STATE_BIT3, NULL },
	{  4, 22, 62, 82, 26,  1, 26, 35, 33,"Fire flare", BT_JOY_BUTTON, 255, NULL, 0, &Controls.fire_flare_count },
	{  5,174, 46, 74, 26, 23,  6, 29, 34,"Slide on", BT_JOY_BUTTON, 255, &Controls.slide_on_state, STATE_BIT3, NULL },
	{  6,174, 54, 74, 26,  5,  7, 30, 35,"Slide left", BT_JOY_BUTTON, 255, &Controls.btn_slide_left_state, STATE_BIT3, NULL },
	{  7,174, 62, 74, 26,  6,  8, 33, 36,"Slide right", BT_JOY_BUTTON, 255, &Controls.btn_slide_right_state, STATE_BIT3, NULL },
	{  8,174, 70, 74, 26,  7,  9, 43, 37,"Slide up", BT_JOY_BUTTON, 255, &Controls.btn_slide_up_state, STATE_BIT3, NULL },
	{  9,174, 78, 74, 26,  8, 10, 31, 38,"Slide down", BT_JOY_BUTTON, 255, &Controls.btn_slide_down_state, STATE_BIT3, NULL },
	{ 10,174, 86, 74, 26,  9, 11, 32, 39,"Bank on", BT_JOY_BUTTON, 255, &Controls.bank_on_state, STATE_BIT3, NULL },
	{ 11,174, 94, 74, 26, 10, 12, 42, 40,"Bank left", BT_JOY_BUTTON, 255, &Controls.btn_bank_left_state, STATE_BIT3, NULL },
	{ 12,174,102, 74, 26, 11, 44, 28, 41,"Bank right", BT_JOY_BUTTON, 255, &Controls.btn_bank_right_state, STATE_BIT3, NULL },
	{ 13, 22,154, 51, 26, 47, 15, 47, 14,"Pitch U/D", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 14, 22,154, 99,  8, 27, 16, 13, 17,"Pitch U/D", BT_INVERT, 255, NULL, 0, NULL },
	{ 15, 22,162, 51, 26, 13,  0, 18, 16,"Turn L/R", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 16, 22,162, 99,  8, 14, 29, 15, 19,"Turn L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 17,164,154, 58, 26, 28, 19, 14, 18,"Slide L/R", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 18,164,154,106,  8, 45, 20, 17, 15,"Slide L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 19,164,162, 58, 26, 17, 21, 16, 20,"Slide U/D", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 20,164,162,106,  8, 18, 22, 19, 21,"Slide U/D", BT_INVERT, 255, NULL, 0, NULL },
	{ 21,164,170, 58, 26, 19, 23, 20, 22,"Bank L/R", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 22,164,170,106,  8, 20, 24, 21, 23,"Bank L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 23,164,178, 58, 26, 21,  5, 22, 24,"Throttle", BT_JOY_AXIS, 255, NULL, 0, NULL },
	{ 24,164,178,106,  8, 22, 34, 23,  0,"Throttle", BT_INVERT, 255, NULL, 0, NULL },
	{ 25, 22, 94, 82, 26,  3, 27, 39, 42,"Rear view", BT_JOY_BUTTON, 255, &Controls.rear_view_state, STATE_BIT3, &Controls.rear_view_count },
	{ 26, 22, 70, 82, 26,  4,  2, 36, 43,"Drop bomb", BT_JOY_BUTTON, 255, NULL, 0, &Controls.drop_bomb_count },
	{ 27, 22,102, 82, 26, 25, 14, 40, 28,"Automap", BT_JOY_BUTTON, 255, &Controls.automap_state, STATE_BIT3, &Controls.automap_count },
	{ 28, 22,102,111, 26, 42, 17, 27, 12,"Automap", BT_JOY_BUTTON, 255, &Controls.automap_state, STATE_BIT4, &Controls.automap_count },
	{ 29, 22, 46,111, 26, 16, 30,  0,  5,"Fire primary", BT_JOY_BUTTON, 255, &Controls.fire_primary_state, STATE_BIT4, &Controls.fire_primary_count },
	{ 30, 22, 54,111, 26, 29, 33,  1,  6,"Fire secondary", BT_JOY_BUTTON, 255, &Controls.fire_secondary_state, STATE_BIT4, &Controls.fire_secondary_count },
	{ 31, 22, 78,111, 26, 43, 32,  2,  9,"Accelerate", BT_JOY_BUTTON, 255, &Controls.accelerate_state, STATE_BIT4, NULL },
	{ 32, 22, 86,111, 26, 31, 42,  3, 10,"Reverse", BT_JOY_BUTTON, 255, &Controls.reverse_state, STATE_BIT4, NULL },
	{ 33, 22, 62,111, 26, 30, 43,  4,  7,"Fire flare", BT_JOY_BUTTON, 255, NULL, 0, &Controls.fire_flare_count },
	{ 34,174, 46,104, 26, 24, 35,  5,  1,"Slide on", BT_JOY_BUTTON, 255, &Controls.slide_on_state, STATE_BIT4, NULL },
	{ 35,174, 54,104, 26, 34, 36,  6,  4,"Slide left", BT_JOY_BUTTON, 255, &Controls.btn_slide_left_state, STATE_BIT4, NULL },
	{ 36,174, 62,104, 26, 35, 37,  7, 26,"Slide right", BT_JOY_BUTTON, 255, &Controls.btn_slide_right_state, STATE_BIT4, NULL },
	{ 37,174, 70,104, 26, 36, 38,  8,  2,"Slide up", BT_JOY_BUTTON, 255, &Controls.btn_slide_up_state, STATE_BIT4, NULL },
	{ 38,174, 78,104, 26, 37, 39,  9,  3,"Slide down", BT_JOY_BUTTON, 255, &Controls.btn_slide_down_state, STATE_BIT4, NULL },
	{ 39,174, 86,104, 26, 38, 40, 10, 25,"Bank on", BT_JOY_BUTTON, 255, &Controls.bank_on_state, STATE_BIT4, NULL },
	{ 40,174, 94,104, 26, 39, 41, 11, 27,"Bank left", BT_JOY_BUTTON, 255, &Controls.btn_bank_left_state, STATE_BIT4, NULL },
	{ 41,174,102,104, 26, 40, 46, 12, 44,"Bank right", BT_JOY_BUTTON, 255, &Controls.btn_bank_right_state, STATE_BIT4, NULL },
	{ 42, 22, 94,111, 26, 32, 28, 25, 11,"Rear view", BT_JOY_BUTTON, 255, &Controls.rear_view_state, STATE_BIT4, &Controls.rear_view_count },
	{ 43, 22, 70,111, 26, 33, 31, 26,  8,"Drop bomb", BT_JOY_BUTTON, 255, NULL, 0, &Controls.drop_bomb_count },
	{ 44,174,110, 74, 26, 12, 45, 41, 46,"Cycle Primary", BT_JOY_BUTTON, 255, NULL, 0, &Controls.cycle_primary_count },
	{ 45,174,118, 74, 26, 44, 18, 46, 47,"Cycle Secondary", BT_JOY_BUTTON, 255, NULL, 0, &Controls.cycle_secondary_count },
	{ 46,174,110,104, 26, 41, 47, 44, 45,"Cycle Primary", BT_JOY_BUTTON, 255, NULL, 0, &Controls.cycle_primary_count },
	{ 47,174,118,104, 26, 46, 13, 45, 13,"Cycle Secondary", BT_JOY_BUTTON, 255, NULL, 0, &Controls.cycle_secondary_count },
};
kc_item kc_mouse[NUM_MOUSE_CONTROLS] = {
	{  0, 25, 46, 85, 26, 19,  1, 20,  5,"Fire primary", BT_MOUSE_BUTTON, 255, &Controls.fire_primary_state, STATE_BIT5, &Controls.fire_primary_count },
	{  1, 25, 54, 85, 26,  0,  4,  5,  6,"Fire secondary", BT_MOUSE_BUTTON, 255, &Controls.fire_secondary_state, STATE_BIT5, &Controls.fire_secondary_count },
	{  2, 25, 78, 85, 26, 26,  3,  8,  9,"Accelerate", BT_MOUSE_BUTTON, 255, &Controls.accelerate_state, STATE_BIT5, NULL },
	{  3, 25, 86, 85, 26,  2, 25,  9, 10,"Reverse", BT_MOUSE_BUTTON, 255, &Controls.reverse_state, STATE_BIT5, NULL },
	{  4, 25, 62, 85, 26,  1, 26,  6,  7,"Fire flare", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.fire_flare_count },
	{  5,180, 46, 59, 26, 23,  6,  0,  1,"Slide on", BT_MOUSE_BUTTON, 255, &Controls.slide_on_state, STATE_BIT5, NULL },
	{  6,180, 54, 59, 26,  5,  7,  1,  4,"Slide left", BT_MOUSE_BUTTON, 255, &Controls.btn_slide_left_state, STATE_BIT5, NULL },
	{  7,180, 62, 59, 26,  6,  8,  4, 26,"Slide right", BT_MOUSE_BUTTON, 255, &Controls.btn_slide_right_state, STATE_BIT5, NULL },
	{  8,180, 70, 59, 26,  7,  9, 26,  2,"Slide up", BT_MOUSE_BUTTON, 255, &Controls.btn_slide_up_state, STATE_BIT5, NULL },
	{  9,180, 78, 59, 26,  8, 10,  2,  3,"Slide down", BT_MOUSE_BUTTON, 255, &Controls.btn_slide_down_state, STATE_BIT5, NULL },
	{ 10,180, 86, 59, 26,  9, 11,  3, 25,"Bank on", BT_MOUSE_BUTTON, 255, &Controls.bank_on_state, STATE_BIT5, NULL },
	{ 11,180, 94, 59, 26, 10, 12, 25, 27,"Bank left", BT_MOUSE_BUTTON, 255, &Controls.btn_bank_left_state, STATE_BIT5, NULL },
	{ 12,180,102, 59, 26, 11, 22, 27, 28,"Bank right", BT_MOUSE_BUTTON, 255, &Controls.btn_bank_right_state, STATE_BIT5, NULL },
	{ 13, 25,154, 58, 26, 24, 15, 28, 14,"Pitch U/D", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 14, 25,154,106,  8, 28, 16, 13, 21,"Pitch U/D", BT_INVERT, 255, NULL, 0, NULL },
	{ 15, 25,162, 58, 26, 13, 17, 22, 16,"Turn L/R", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 16, 25,162,106,  8, 14, 18, 15, 23,"Turn L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 17, 25,170, 58, 26, 15, 19, 24, 18,"Slide L/R", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 18, 25,170,106,  8, 16, 20, 17, 19,"Slide L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 19, 25,178, 58, 26, 17,  0, 18, 20,"Slide U/D", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 20, 25,178,106,  8, 18, 21, 19,  0,"Slide U/D", BT_INVERT, 255, NULL, 0, NULL },
	{ 21,180,154, 58, 26, 20, 23, 14, 22,"Bank L/R", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 22,180,154,106,  8, 12, 24, 21, 15,"Bank L/R", BT_INVERT, 255, NULL, 0, NULL },
	{ 23,180,162, 58, 26, 21,  5, 16, 24,"Throttle", BT_MOUSE_AXIS, 255, NULL, 0, NULL },
	{ 24,180,162,106,  8, 22, 13, 23, 17,"Throttle", BT_INVERT, 255, NULL, 0, NULL },
	{ 25, 25, 94, 85, 26,  3, 27, 10, 11,"Rear view", BT_MOUSE_BUTTON, 255, &Controls.rear_view_state, STATE_BIT5, &Controls.rear_view_count },
	{ 26, 25, 70, 85, 26,  4,  2,  7,  8,"Drop bomb", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.drop_bomb_count },
	{ 27, 25,102, 85, 26, 25, 28, 11, 12,"Cycle Primary", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.cycle_primary_count },
	{ 28, 25,110, 85, 26, 27, 14, 12, 13,"Cycle Secondary", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.cycle_secondary_count },
};
kc_item kc_d1x[NUM_D1X_CONTROLS] = {
	{  0, 15, 69,142, 26, 29,  3, 29,  1,"LASER CANNON", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{  1, 15, 69,200, 26, 27,  4,  0,  2,"LASER CANNON", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  2, 15, 69,258, 26, 28,  5,  1,  3,"LASER CANNON", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  3, 15, 77,142, 26,  0,  6,  2,  4,"VULCAN CANNON", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{  4, 15, 77,200, 26,  1,  7,  3,  5,"VULCAN CANNON", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  5, 15, 77,258, 26,  2,  8,  4,  6,"VULCAN CANNON", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  6, 15, 85,142, 26,  3,  9,  5,  7,"SPREADFIRE CANNON", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{  7, 15, 85,200, 26,  4, 10,  6,  8,"SPREADFIRE CANNON", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  8, 15, 85,258, 26,  5, 11,  7,  9,"SPREADFIRE CANNON", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{  9, 15, 93,142, 26,  6, 12,  8, 10,"PLASMA CANNON", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 10, 15, 93,200, 26,  7, 13,  9, 11,"PLASMA CANNON", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 11, 15, 93,258, 26,  8, 14, 10, 12,"PLASMA CANNON", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 12, 15,101,142, 26,  9, 15, 11, 13,"FUSION CANNON", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 13, 15,101,200, 26, 10, 16, 12, 14,"FUSION CANNON", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 14, 15,101,258, 26, 11, 17, 13, 15,"FUSION CANNON", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 15, 15,109,142, 26, 12, 18, 14, 16,"CONCUSSION MISSILE", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 16, 15,109,200, 26, 13, 19, 15, 17,"CONCUSSION MISSILE", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 17, 15,109,258, 26, 14, 20, 16, 18,"CONCUSSION MISSILE", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 18, 15,117,142, 26, 15, 21, 17, 19,"HOMING MISSILE", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 19, 15,117,200, 26, 16, 22, 18, 20,"HOMING MISSILE", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 20, 15,117,258, 26, 17, 23, 19, 21,"HOMING MISSILE", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 21, 15,125,142, 26, 18, 24, 20, 22,"PROXIMITY BOMB", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 22, 15,125,200, 26, 19, 25, 21, 23,"PROXIMITY BOMB", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 23, 15,125,258, 26, 20, 26, 22, 24,"PROXIMITY BOMB", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 24, 15,133,142, 26, 21, 27, 23, 25,"SMART MISSILE", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 25, 15,133,200, 26, 22, 28, 24, 26,"SMART MISSILE", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 26, 15,133,258, 26, 23, 29, 25, 27,"SMART MISSILE", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 27, 15,141,142, 26, 24,  1, 26, 28,"MEGA MISSILE", BT_KEY, 255, NULL, 0, &Controls.select_weapon_count },
	{ 28, 15,141,200, 26, 25,  2, 27, 29,"MEGA MISSILE", BT_JOY_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
	{ 29, 15,141,258, 26, 26,  0, 28,  0,"MEGA MISSILE", BT_MOUSE_BUTTON, 255, NULL, 0, &Controls.select_weapon_count },
};

void kc_drawitem( kc_item *item, int is_current );
void kc_change_key( kc_menu *menu, d_event *event, kc_item * item );
void kc_change_joybutton( kc_menu *menu, d_event *event, kc_item * item );
void kc_change_mousebutton( kc_menu *menu, d_event *event, kc_item * item );
void kc_change_joyaxis( kc_menu *menu, d_event *event, kc_item * item );
void kc_change_mouseaxis( kc_menu *menu, d_event *event, kc_item * item );
void kc_change_invert( kc_menu *menu, kc_item * item );

#ifdef TABLE_CREATION
int find_item_at( kc_item * items, int nitems, int x, int y )
{
	int i;
	
	for (i=0; i<nitems; i++ )	{
		if ( ((items[i].x+items[i].w1)==x) && (items[i].y==y))
			return i;
	}
	return -1;
}

int find_next_item_up( kc_item * items, int nitems, int citem )
{
	int x, y, i;

	y = items[citem].y;
	x = items[citem].x+items[citem].w1;
	
	do {	
		y--;
		if ( y < 0 ) {
			y = grd_curcanv->cv_bitmap.bm_h-1;
			x--;
			if ( x < 0 ) {
				x = grd_curcanv->cv_bitmap.bm_w-1;
			}
		}
		i = find_item_at( items, nitems, x, y );
	} while ( i < 0 );
	
	return i;
}

int find_next_item_down( kc_item * items, int nitems, int citem )
{
	int x, y, i;

	y = items[citem].y;
	x = items[citem].x+items[citem].w1;
	
	do {	
		y++;
		if ( y > grd_curcanv->cv_bitmap.bm_h-1 ) {
			y = 0;
			x++;
			if ( x > grd_curcanv->cv_bitmap.bm_w-1 ) {
				x = 0;
			}
		}
		i = find_item_at( items, nitems, x, y );
	} while ( i < 0 );
	
	return i;
}

int find_next_item_right( kc_item * items, int nitems, int citem )
{
	int x, y, i;

	y = items[citem].y;
	x = items[citem].x+items[citem].w1;
	
	do {	
		x++;
		if ( x > grd_curcanv->cv_bitmap.bm_w-1 ) {
			x = 0;
			y++;
			if ( y > grd_curcanv->cv_bitmap.bm_h-1 ) {
				y = 0;
			}
		}
		i = find_item_at( items, nitems, x, y );
	} while ( i < 0 );
	
	return i;
}

int find_next_item_left( kc_item * items, int nitems, int citem )
{
	int x, y, i;

	y = items[citem].y;
	x = items[citem].x+items[citem].w1;
	
	do {	
		x--;
		if ( x < 0 ) {
			x = grd_curcanv->cv_bitmap.bm_w-1;
			y--;
			if ( y < 0 ) {
				y = grd_curcanv->cv_bitmap.bm_h-1;
			}
		}
		i = find_item_at( items, nitems, x, y );
	} while ( i < 0 );
	
	return i;
}
#endif

void kconfig_start_changing(kc_menu *menu);

// The binding text a slot shows: the key, button, axis or invert flag it
// holds, or nothing when it is unbound.
static void kc_binding_text(const kc_item *item, char *out, size_t out_size)
{
	out[0] = '\0';
	if (item->value == 255)
		return;
	switch (item->type)
	{
		case BT_KEY:
			snprintf(out, out_size, "%s", key_properties[item->value].key_text);
			break;
		case BT_MOUSE_BUTTON:
			snprintf(out, out_size, "%s", mousebutton_text[item->value]);
			break;
		case BT_MOUSE_AXIS:
			snprintf(out, out_size, "%s", mouseaxis_text[item->value]);
			break;
		case BT_JOY_BUTTON:
			if (joybutton_text[item->value])
				snprintf(out, out_size, "%s", joybutton_text[item->value]);
			else
				snprintf(out, out_size, "BTN%d", item->value + 1);
			break;
		case BT_JOY_AXIS:
			if (joyaxis_text[item->value])
				snprintf(out, out_size, "%s", joyaxis_text[item->value]);
			else
				snprintf(out, out_size, "AXIS%d", item->value + 1);
			break;
		case BT_INVERT:
			snprintf(out, out_size, "%s", invert_text[item->value]);
			break;
	}
}

// One row per action. The tables list a second (and for the weapon keys a
// third) binding for the same action far from the first, so rows are
// gathered by label rather than by adjacency.
static void kconfig_build_rows(kc_menu *menu)
{
	int i, r;

	menu->nrows = 0;
	for (i = 0; i < menu->nitems; i++)
	{
		for (r = 0; r < menu->nrows; r++)
			if (!strcmp(menu->rows[r].label, menu->items[i].text))
				break;
		if (r == menu->nrows)
		{
			if (r == KC_MAX_ROWS)
				return;
			menu->rows[r].label = menu->items[i].text;
			menu->rows[r].nslots = 0;
			menu->nrows++;
		}
		if (menu->rows[r].nslots < KC_MAX_SLOTS)
			menu->rows[r].slot[menu->rows[r].nslots++] = i;
	}
}

static void kconfig_find_pos(kc_menu *menu, int *row, int *slot)
{
	int r, s;

	for (r = 0; r < menu->nrows; r++)
		for (s = 0; s < menu->rows[r].nslots; s++)
			if (menu->rows[r].slot[s] == menu->citem)
			{
				*row = r;
				*slot = s;
				return;
			}
	*row = 0;
	*slot = 0;
}

static void kconfig_move(kc_menu *menu, int drow, int dslot)
{
	int row, slot;

	if (!menu->nrows)
		return;
	kconfig_find_pos(menu, &row, &slot);
	if (drow)
		row = (row + drow + menu->nrows) % menu->nrows;
	slot += dslot;
	if (slot < 0)
		slot = 0;
	if (slot >= menu->rows[row].nslots)
		slot = menu->rows[row].nslots - 1;
	menu->citem = menu->rows[row].slot[slot];
}

static void kconfig_restore_defaults(kc_menu *menu)
{
	int i;

	if (menu->items == kc_keyboard)
		for (i = 0; i < NUM_KEY_CONTROLS; i++)
			menu->items[i].value = DefaultKeySettings[0][i];
	if (menu->items == kc_joystick)
		for (i = 0; i < NUM_JOYSTICK_CONTROLS; i++)
			menu->items[i].value = DefaultKeySettings[1][i];
	if (menu->items == kc_mouse)
		for (i = 0; i < NUM_MOUSE_CONTROLS; i++)
			menu->items[i].value = DefaultKeySettings[2][i];
	if (menu->items == kc_d1x)
		for (i = 0; i < NUM_D1X_CONTROLS; i++)
			menu->items[i].value = DefaultKeySettingsD1X[i];
}

static const char *kconfig_hint(kc_menu *menu)
{
	if (!menu->changing)
		return "Pick a binding to change it  -  Ctrl-D clears  -  Ctrl-R restores defaults  -  Esc exits";
	switch (menu->items[menu->citem].type)
	{
		case BT_KEY:		return TXT_PRESS_NEW_KEY;
		case BT_MOUSE_BUTTON:	return TXT_PRESS_NEW_MBUTTON;
		case BT_MOUSE_AXIS:	return TXT_MOVE_NEW_MSE_AXIS;
		case BT_JOY_BUTTON:	return TXT_PRESS_NEW_JBUTTON;
		case BT_JOY_AXIS:	return TXT_MOVE_NEW_JOY_AXIS;
	}
	return "";
}

// The weapon-key screen binds the same action on all three devices, so its
// slots are worth naming; everywhere else they are just alternatives.
static const char *const kc_d1x_slot_names[KC_MAX_SLOTS] = { "KEYBOARD", "JOYSTICK", "MOUSE" };

void kconfig_draw(kc_menu *menu)
{
	struct nk_ui_bind_row rows[KC_MAX_ROWS];
	char text[KC_MAX_ROWS][KC_MAX_SLOTS][KC_BINDING_LEN];
	int current_row, current_slot, picked_row, picked_slot;
	int r, s, action;

	for (r = 0; r < menu->nrows; r++)
	{
		rows[r].label = menu->rows[r].label;
		rows[r].nslots = menu->rows[r].nslots;
		for (s = 0; s < KC_MAX_SLOTS; s++)
		{
			if (s >= menu->rows[r].nslots)
			{
				rows[r].slot[s] = NULL;
				continue;
			}
			kc_binding_text(&menu->items[menu->rows[r].slot[s]], text[r][s], KC_BINDING_LEN);
			rows[r].slot[s] = text[r][s];
		}
	}

	kconfig_find_pos(menu, &current_row, &current_slot);
	action = nk_ui_bind_frame(menu, menu->title, kconfig_hint(menu), rows, menu->nrows,
		menu->items == kc_d1x ? kc_d1x_slot_names : NULL,
		current_row, current_slot, menu->changing, &picked_row, &picked_slot);

	switch (action)
	{
		case NK_UI_BIND_PICK:
			menu->citem = menu->rows[picked_row].slot[picked_slot];
			kconfig_start_changing(menu);
			break;
		case NK_UI_BIND_CLEAR:
			menu->items[menu->citem].value = 255;
			break;
		case NK_UI_BIND_DEFAULTS:
			kconfig_restore_defaults(menu);
			break;
		case NK_UI_BIND_CLOSE:
			window_close(menu->wind);
			break;
	}
}

void kconfig_start_changing(kc_menu *menu)
{
	if (menu->items[menu->citem].type == BT_INVERT)
	{
		kc_change_invert(menu, &menu->items[menu->citem]);
		return;
	}

	menu->changing = 1;
}

int kconfig_key_command(window *wind, d_event *event, kc_menu *menu)
{
	int i,k;

	k = event_key_get(event);

	// when changing, process no keys instead of ESC
	if (menu->changing && (k != -2 && k != KEY_ESC))
		return 0;

	switch (k)
	{
		case KEY_CTRLED+KEY_D:
			menu->items[menu->citem].value = 255;
			return 1;
		case KEY_CTRLED+KEY_R:
			kconfig_restore_defaults(menu);
			return 1;
		case KEY_DELETE:
			menu->items[menu->citem].value=255;
			return 1;
		case KEY_UP:
		case KEY_PAD8:
			kconfig_move(menu, -1, 0);
			return 1;
		case KEY_DOWN:
		case KEY_PAD2:
			kconfig_move(menu, 1, 0);
			return 1;
		case KEY_LEFT:
		case KEY_PAD4:
			kconfig_move(menu, 0, -1);
			return 1;
		case KEY_RIGHT:
		case KEY_PAD6:
			kconfig_move(menu, 0, 1);
			return 1;
		case KEY_ENTER:
		case KEY_PADENTER:
			kconfig_start_changing(menu);
			return 1;
		case -2:	
		case KEY_ESC:
			if (menu->changing)
				menu->changing = 0;
			else
				window_close(wind);
			return 1;
#ifdef TABLE_CREATION
		case KEY_F12:	{
			static const char *const btype_text[] = { "BT_KEY", "BT_MOUSE_BUTTON", "BT_MOUSE_AXIS", "BT_JOY_BUTTON", "BT_JOY_AXIS", "BT_INVERT" };
				PHYSFS_file * fp;
				for (i=0; i<NUM_KEY_CONTROLS; i++ )	{
					kc_keyboard[i].u = find_next_item_up( kc_keyboard,NUM_KEY_CONTROLS, i);
					kc_keyboard[i].d = find_next_item_down( kc_keyboard,NUM_KEY_CONTROLS, i);
					kc_keyboard[i].l = find_next_item_left( kc_keyboard,NUM_KEY_CONTROLS, i);
					kc_keyboard[i].r = find_next_item_right( kc_keyboard,NUM_KEY_CONTROLS, i);
				}
				for (i=0; i<NUM_JOYSTICK_CONTROLS; i++ )	{
					kc_joystick[i].u = find_next_item_up( kc_joystick,NUM_JOYSTICK_CONTROLS, i);
					kc_joystick[i].d = find_next_item_down( kc_joystick,NUM_JOYSTICK_CONTROLS, i);
					kc_joystick[i].l = find_next_item_left( kc_joystick,NUM_JOYSTICK_CONTROLS, i);
					kc_joystick[i].r = find_next_item_right( kc_joystick,NUM_JOYSTICK_CONTROLS, i);
				}
				for (i=0; i<NUM_MOUSE_CONTROLS; i++ )	{
					kc_mouse[i].u = find_next_item_up( kc_mouse,NUM_MOUSE_CONTROLS, i);
					kc_mouse[i].d = find_next_item_down( kc_mouse,NUM_MOUSE_CONTROLS, i);
					kc_mouse[i].l = find_next_item_left( kc_mouse,NUM_MOUSE_CONTROLS, i);
					kc_mouse[i].r = find_next_item_right( kc_mouse,NUM_MOUSE_CONTROLS, i);
				}
				for (i=0; i<NUM_D1X_CONTROLS; i++ )	{
					kc_d1x[i].u = find_next_item_up( kc_d1x,NUM_D1X_CONTROLS, i);
					kc_d1x[i].d = find_next_item_down( kc_d1x,NUM_D1X_CONTROLS, i);
					kc_d1x[i].l = find_next_item_left( kc_d1x,NUM_D1X_CONTROLS, i);
					kc_d1x[i].r = find_next_item_right( kc_d1x,NUM_D1X_CONTROLS, i);
				}
				fp = PHYSFSX_openWriteBuffered( "kconfig.cod" );
				
				PHYSFSX_printf( fp, "ubyte DefaultKeySettings[3][MAX_CONTROLS] = {\n" );
				for (i=0; i<3; i++ )	{
					int j;
					PHYSFSX_printf( fp, "{0x%2x", PlayerCfg.KeySettings[i][0] );
					for (j=1; j<MAX_CONTROLS; j++ )
						PHYSFSX_printf( fp, ",0x%2x", PlayerCfg.KeySettings[i][j] );
					PHYSFSX_printf( fp, "},\n" );
				}
				PHYSFSX_printf( fp, "};\n" );
				
				PHYSFSX_printf( fp, "\nkc_item kc_keyboard[NUM_KEY_CONTROLS] = {\n" );
				for (i=0; i<NUM_KEY_CONTROLS; i++ )	{
					PHYSFSX_printf( fp, "\t{ %2d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%c%s%c, %s, 255 },\n", 
							kc_keyboard[i].id, kc_keyboard[i].x, kc_keyboard[i].y, kc_keyboard[i].w1, kc_keyboard[i].w2,
							kc_keyboard[i].u, kc_keyboard[i].d, kc_keyboard[i].l, kc_keyboard[i].r,
							34, kc_keyboard[i].text, 34, btype_text[kc_keyboard[i].type] );
				}
				PHYSFSX_printf( fp, "};" );
				
				PHYSFSX_printf( fp, "\nkc_item kc_joystick[NUM_JOYSTICK_CONTROLS] = {\n" );
				for (i=0; i<NUM_JOYSTICK_CONTROLS; i++ )	{
					PHYSFSX_printf( fp, "\t{ %2d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%c%s%c, %s, 255 },\n", 
							kc_joystick[i].id, kc_joystick[i].x, kc_joystick[i].y, kc_joystick[i].w1, kc_joystick[i].w2,
							kc_joystick[i].u, kc_joystick[i].d, kc_joystick[i].l, kc_joystick[i].r,
							34, kc_joystick[i].text, 34, btype_text[kc_joystick[i].type] );
				}
				PHYSFSX_printf( fp, "};" );
				
				PHYSFSX_printf( fp, "\nkc_item kc_mouse[NUM_MOUSE_CONTROLS] = {\n" );
				for (i=0; i<NUM_MOUSE_CONTROLS; i++ )	{
					PHYSFSX_printf( fp, "\t{ %2d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%c%s%c, %s, 255 },\n", 
							kc_mouse[i].id, kc_mouse[i].x, kc_mouse[i].y, kc_mouse[i].w1, kc_mouse[i].w2,
							kc_mouse[i].u, kc_mouse[i].d, kc_mouse[i].l, kc_mouse[i].r,
							34, kc_mouse[i].text, 34, btype_text[kc_mouse[i].type] );
				}
				PHYSFSX_printf( fp, "};" );
				
				PHYSFSX_printf( fp, "\nkc_item kc_d1x[NUM_D1X_CONTROLS] = {\n" );
				for (i=0; i<NUM_D1X_CONTROLS; i++ )	{
					PHYSFSX_printf( fp, "\t{ %2d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%3d,%c%s%c, %s, 255 },\n", 
							kc_d1x[i].id, kc_d1x[i].x, kc_d1x[i].y, kc_d1x[i].w1, kc_d1x[i].w2,
							kc_d1x[i].u, kc_d1x[i].d, kc_d1x[i].l, kc_d1x[i].r,
							34, kc_d1x[i].text, 34, btype_text[kc_d1x[i].type] );
				}
				PHYSFSX_printf( fp, "};" );
				
				PHYSFS_close(fp);
				
			}
			return 1;
#endif
		case 0:		// some other event
			break;
			
		default:
			break;
	}
	
	return 0;
}

int kconfig_handler(window *wind, d_event *event, kc_menu *menu)
{
	int i;
	
	switch (event->type)
	{
		case EVENT_WINDOW_ACTIVATED:
			game_flush_inputs();
			break;
			
		case EVENT_WINDOW_DEACTIVATED:
			menu->mouse_state = 0;
			break;
			
		case EVENT_MOUSE_BUTTON_DOWN:
		case EVENT_MOUSE_BUTTON_UP:
			if (menu->changing && (menu->items[menu->citem].type == BT_MOUSE_BUTTON) && (event->type == EVENT_MOUSE_BUTTON_DOWN))
			{
				kc_change_mousebutton( menu, event, &menu->items[menu->citem] );
				menu->mouse_state = 1;
				return 1;
			}

			if (event_mouse_get_button(event) == MBTN_RIGHT)
			{
				if (!menu->changing)
					window_close(wind);
				return 1;
			}
			else if (event_mouse_get_button(event) != MBTN_LEFT)
				return 0;

			menu->mouse_state = (event->type == EVENT_MOUSE_BUTTON_DOWN);
			return 1;

		case EVENT_MOUSE_MOVED:
			if (menu->changing && menu->items[menu->citem].type == BT_MOUSE_AXIS) kc_change_mouseaxis(menu, event, &menu->items[menu->citem]);
			else
				event_mouse_get_delta( event, &menu->old_maxis[0], &menu->old_maxis[1], &menu->old_maxis[2]);
			break;

		case EVENT_JOYSTICK_BUTTON_DOWN:
			if (menu->changing && menu->items[menu->citem].type == BT_JOY_BUTTON) kc_change_joybutton(menu, event, &menu->items[menu->citem]);
			break;

		case EVENT_JOYSTICK_MOVED:
			if (menu->changing && menu->items[menu->citem].type == BT_JOY_AXIS) kc_change_joyaxis(menu, event, &menu->items[menu->citem]);
			else
			{
				int axis, value;
				event_joystick_get_axis( event, &axis, &value );
				menu->old_jaxis[axis] = value;
			}
			break;

		case EVENT_KEY_COMMAND:
		{
			int rval = kconfig_key_command(wind, event, menu);
			if (rval)
				return rval;
			if (menu->changing && menu->items[menu->citem].type == BT_KEY) kc_change_key(menu, event, &menu->items[menu->citem]);
			return 0;
		}

		case EVENT_WINDOW_DRAW:
			if (menu->changing)
				timer_delay(f0_1/10);
			else
				timer_delay2(50);
			kconfig_draw(menu);
			break;
			
		case EVENT_WINDOW_CLOSE:
			nk_ui_menu_closed();
			d_free(menu);
			
			// Update save values...
			
			for (i=0; i<NUM_KEY_CONTROLS; i++ ) 
				PlayerCfg.KeySettings[0][i] = kc_keyboard[i].value;
			
			for (i=0; i<NUM_JOYSTICK_CONTROLS; i++ ) 
				PlayerCfg.KeySettings[1][i] = kc_joystick[i].value;

			for (i=0; i<NUM_MOUSE_CONTROLS; i++ ) 
				PlayerCfg.KeySettings[2][i] = kc_mouse[i].value;
			
			for (i=0; i<NUM_D1X_CONTROLS; i++)
				PlayerCfg.KeySettingsD1X[i] = kc_d1x[i].value;
			return 0;	// continue closing
			break;
			
		default:
			return 0;
			break;
	}
	
	return 1;
}

void kconfig_sub(kc_item * items,int nitems, char *title)
{
	kc_menu *menu;

	MALLOC(menu, kc_menu, 1);
	
	if (!menu)
		return;

	memset(menu, 0, sizeof(kc_menu));
	menu->items = items;
	menu->nitems = nitems;
	menu->title = title;
	menu->citem = 0;
	menu->changing = 0;
	menu->mouse_state = 0;
	kconfig_build_rows(menu);

	if (!(menu->wind = window_create(&grd_curscreen->sc_canvas, 0, 0, SWIDTH, SHEIGHT,
					   (int (*)(window *, d_event *, void *))kconfig_handler, menu)))
	{
		d_free(menu);
		return;
	}
	nk_ui_menu_opened();
}


void kc_change_key( kc_menu *menu, d_event *event, kc_item * item )
{
	int i,n;
	ubyte keycode = 255;

	Assert(event->type == EVENT_KEY_COMMAND);
	keycode = event_key_get_raw(event);

	if (!(key_properties[keycode].key_text))
		return;

	for (n=0; n<(GameArg.CtlNoStickyKeys?sizeof(system_keys)-3:sizeof(system_keys)); n++ )
		if ( system_keys[n] == keycode )
			return;

	for (i=0; i<menu->nitems; i++ )
	{
		n = item - menu->items;
		if ( (i!=n) && (menu->items[i].type==BT_KEY) && (menu->items[i].value==keycode) )
		{
			menu->items[i].value = 255;
		}
	}
	item->value = keycode;
	menu->changing = 0;
}

void kc_change_joybutton( kc_menu *menu, d_event *event, kc_item * item )
{
	int n,i,button = 255;

	Assert(event->type == EVENT_JOYSTICK_BUTTON_DOWN);
	button = event_joystick_get_button(event);

	for (i=0; i<menu->nitems; i++ )
	{
		n = item - menu->items;
		if ( (i!=n) && (menu->items[i].type==BT_JOY_BUTTON) && (menu->items[i].value==button) )
			menu->items[i].value = 255;
	}
	item->value = button;
	menu->changing = 0;
}

void kc_change_mousebutton( kc_menu *menu, d_event *event, kc_item * item )
{
	int n,i,button;

	Assert(event->type == EVENT_MOUSE_BUTTON_DOWN || event->type == EVENT_MOUSE_BUTTON_UP);
	button = event_mouse_get_button(event);

	for (i=0; i<menu->nitems; i++)
	{
		n = item - menu->items;
		if ( (i!=n) && (menu->items[i].type==BT_MOUSE_BUTTON) && (menu->items[i].value==button) )
			menu->items[i].value = 255;
	}
	item->value = button;
	menu->changing = 0;
}

void kc_change_joyaxis( kc_menu *menu, d_event *event, kc_item * item )
{
	int i, n, axis, value;

	Assert(event->type == EVENT_JOYSTICK_MOVED);
	event_joystick_get_axis( event, &axis, &value );

	if ( abs(value-menu->old_jaxis[axis])<32 )
		return;
	con_printf(CON_DEBUG, "Axis Movement detected: Axis %i\n", axis);

	for (i=0; i<menu->nitems; i++ )
	{
		n = item - menu->items;
		if ( (i!=n) && (menu->items[i].type==BT_JOY_AXIS) && (menu->items[i].value==axis) )
			menu->items[i].value = 255;
	}
	item->value = axis;
	menu->changing = 0;
}

void kc_change_mouseaxis( kc_menu *menu, d_event *event, kc_item * item )
{
	int i, n, dx, dy, dz;
	ubyte code = 255;

	Assert(event->type == EVENT_MOUSE_MOVED);
	event_mouse_get_delta( event, &dx, &dy, &dz );
	if ( abs(dx)>5 ) code = 0;
	if ( abs(dy)>5 ) code = 1;
	if ( abs(dz)>5 ) code = 2;

	if (code!=255)
	{
		for (i=0; i<menu->nitems; i++ )
		{
			n = item - menu->items;
			if ( (i!=n) && (menu->items[i].type==BT_MOUSE_AXIS) && (menu->items[i].value==code) )
				menu->items[i].value = 255;
		}
		item->value = code;
		menu->changing = 0;
	}
}

void kc_change_invert( kc_menu *menu, kc_item * item )
{
	if (item->value)
		item->value = 0;
	else 
		item->value = 1;

	menu->changing = 0;		// in case we were changing something else
}

#include "screens.h"

int undercalibrate_scale(int raw_undercalibrate) {
	return raw_undercalibrate + 1; 
}

void kconfig(int n, char * title)
{
	set_screen_mode( SCREEN_MENU );
	kc_set_controls();

	switch(n)
    	{
		case 0:kconfig_sub( kc_keyboard,NUM_KEY_CONTROLS,  title); break;
		case 1:kconfig_sub( kc_joystick,NUM_JOYSTICK_CONTROLS,title); break;
		case 2:kconfig_sub( kc_mouse,   NUM_MOUSE_CONTROLS,    title); break;
		case 3:kconfig_sub( kc_d1x, NUM_D1X_CONTROLS, title ); break;
		default:
			Int3();
			return;
	}
}

int is_key_rotate_event(d_event *event) {
	switch(event->type) {
		case EVENT_KEY_COMMAND:
			for (int i = 0; i < NUM_KEY_CONTROLS; i++)
			{
				if (kc_keyboard[i].value < 255 && kc_keyboard[i].value == event_key_get_raw(event))
				{
					if (kc_keyboard[i].ci_state_ptr != NULL)
					{
						if(kc_keyboard[i].ci_state_ptr == &Controls.key_bank_left_state) {
							return 1; 
						} else if(kc_keyboard[i].ci_state_ptr == &Controls.key_bank_right_state) {
							return 1; 
						} else if(kc_keyboard[i].ci_state_ptr == &Controls.key_pitch_forward_state) {
							return 1; 
						} else if(kc_keyboard[i].ci_state_ptr == &Controls.key_pitch_backward_state) {
							return 1; 
						} else if(kc_keyboard[i].ci_state_ptr == &Controls.key_heading_left_state) {
							return 1; 
						} else if(kc_keyboard[i].ci_state_ptr == &Controls.key_heading_right_state) {
							return 1; 
						}
					}
				}
			}

			return 0; 

		case EVENT_JOYSTICK_BUTTON_DOWN:
			if (!(PlayerCfg.ControlType & CONTROL_USING_JOYSTICK))
				break;
			for (int i = 0; i < NUM_JOYSTICK_CONTROLS; i++)
			{
				if (kc_joystick[i].value < 255 && kc_joystick[i].type == BT_JOY_BUTTON && kc_joystick[i].value == event_joystick_get_button(event))
				{
					if (kc_joystick[i].ci_state_ptr != NULL)
					{
						if(kc_joystick[i].ci_state_ptr == &Controls.key_bank_left_state) {
							return 1; 
						} else if(kc_joystick[i].ci_state_ptr == &Controls.key_bank_right_state) {
							return 1; 
						} else if(kc_joystick[i].ci_state_ptr == &Controls.key_pitch_forward_state) {
							return 1; 
						} else if(kc_joystick[i].ci_state_ptr == &Controls.key_pitch_backward_state) {
							return 1; 
						} else if(kc_joystick[i].ci_state_ptr == &Controls.key_heading_left_state) {
							return 1; 
						} else if(kc_joystick[i].ci_state_ptr == &Controls.key_heading_right_state) {
							return 1; 
						}
					}
				}
			}

			return 0; 

		case EVENT_MOUSE_BUTTON_DOWN:
			if (!(PlayerCfg.ControlType & CONTROL_USING_MOUSE))
				break;
			for (int i = 0; i < NUM_MOUSE_CONTROLS; i++)
			{
				if (kc_mouse[i].value < 255 && kc_mouse[i].type == BT_MOUSE_BUTTON && kc_mouse[i].value == event_mouse_get_button(event))
				{
					if (kc_mouse[i].ci_state_ptr != NULL)
					{
						if(kc_mouse[i].ci_state_ptr == &Controls.key_bank_left_state) {
							return 1; 
						} else if(kc_mouse[i].ci_state_ptr == &Controls.key_bank_right_state) {
							return 1; 
						} else if(kc_mouse[i].ci_state_ptr == &Controls.key_pitch_forward_state) {
							return 1; 
						} else if(kc_mouse[i].ci_state_ptr == &Controls.key_pitch_backward_state) {
							return 1; 
						} else if(kc_mouse[i].ci_state_ptr == &Controls.key_heading_left_state) {
							return 1; 
						} else if(kc_mouse[i].ci_state_ptr == &Controls.key_heading_right_state) {
							return 1; 
						}						
					}
				}
			}

			return 0;

		default: return 0; 
	}

	return 0; 
}

void kconfig_read_controls(d_event *event, int automap_flag)
{
	// Don't read from the controls if we are locked into observing a specific player.
	if (is_observer() && is_observing_player()) {
		return;
	}

	int i = 0, j = 0;
	int speed_factor = (cheats.turbo || (is_observer() && PlayerCfg.ObsTurbo[get_observer_game_mode()])) ? 2 : 1;
	static fix64 mouse_delta_time = 0;
    int overruns = 0;

#ifndef NDEBUG
	// --- Don't do anything if in debug mode ---
	if ( keyd_pressed[KEY_DELETE] )
	{
		memset( &Controls, 0, sizeof(control_info) );
		return;
	}
#endif

	Controls.pitch_time = Controls.vertical_thrust_time = Controls.heading_time = Controls.sideways_thrust_time = Controls.bank_time = Controls.forward_thrust_time = 0;

	switch (event->type)
	{
		case EVENT_KEY_COMMAND:
		case EVENT_KEY_RELEASE:
			for (i = 0; i < NUM_KEY_CONTROLS; i++)
			{
				if (kc_keyboard[i].value < 255 && kc_keyboard[i].value == event_key_get_raw(event))
				{
					if (kc_keyboard[i].ci_state_ptr != NULL)
					{
						if (event->type==EVENT_KEY_COMMAND)
							*kc_keyboard[i].ci_state_ptr |= kc_keyboard[i].state_bit;
						else
							*kc_keyboard[i].ci_state_ptr &= ~kc_keyboard[i].state_bit;
					}
					if (kc_keyboard[i].ci_count_ptr != NULL && event->type==EVENT_KEY_COMMAND)
						*kc_keyboard[i].ci_count_ptr += 1;
				}
			}
		
			if (!automap_flag && event->type == EVENT_KEY_COMMAND)
				for (i = 0, j = 0; i < 28; i += 3, j++)
					if (kc_d1x[i].value < 255 && kc_d1x[i].value == event_key_get_raw(event))
					{
						Controls.select_weapon_count = j+1;
						break;
					}
			break;
		case EVENT_JOYSTICK_BUTTON_DOWN:
		case EVENT_JOYSTICK_BUTTON_UP:
			if (!(PlayerCfg.ControlType & CONTROL_USING_JOYSTICK))
				break;
			for (i = 0; i < NUM_JOYSTICK_CONTROLS; i++)
			{
				if (kc_joystick[i].value < 255 && kc_joystick[i].type == BT_JOY_BUTTON && kc_joystick[i].value == event_joystick_get_button(event))
				{
					if (kc_joystick[i].ci_state_ptr != NULL)
					{
						if (event->type==EVENT_JOYSTICK_BUTTON_DOWN)
							*kc_joystick[i].ci_state_ptr |= kc_joystick[i].state_bit;
						else
							*kc_joystick[i].ci_state_ptr &= ~kc_joystick[i].state_bit;
					}
					if (kc_joystick[i].ci_count_ptr != NULL && event->type==EVENT_JOYSTICK_BUTTON_DOWN)
						*kc_joystick[i].ci_count_ptr += 1;
				}
			}
			if (!automap_flag && event->type == EVENT_JOYSTICK_BUTTON_DOWN)
				for (i = 1, j = 0; i < 29; i += 3, j++)
					if (kc_d1x[i].value < 255 && kc_d1x[i].value == event_joystick_get_button(event))
					{
						Controls.select_weapon_count = j+1;
						break;
					}
			break;
		case EVENT_MOUSE_BUTTON_DOWN:
		case EVENT_MOUSE_BUTTON_UP:
			if (!(PlayerCfg.ControlType & CONTROL_USING_MOUSE))
				break;
			for (i = 0; i < NUM_MOUSE_CONTROLS; i++)
			{
				if (kc_mouse[i].value < 255 && kc_mouse[i].type == BT_MOUSE_BUTTON && kc_mouse[i].value == event_mouse_get_button(event))
				{
					if (kc_mouse[i].ci_state_ptr != NULL)
					{
						if (event->type==EVENT_MOUSE_BUTTON_DOWN)
							*kc_mouse[i].ci_state_ptr |= kc_mouse[i].state_bit;
						else
							*kc_mouse[i].ci_state_ptr &= ~kc_mouse[i].state_bit;
					}
					if (kc_mouse[i].ci_count_ptr != NULL && event->type==EVENT_MOUSE_BUTTON_DOWN)
						*kc_mouse[i].ci_count_ptr += 1;
				}
			}
			if (!automap_flag && event->type == EVENT_MOUSE_BUTTON_DOWN)
				for (i = 2, j = 0; i < 30; i += 3, j++)
					if (kc_d1x[i].value < 255 && kc_d1x[i].value == event_mouse_get_button(event))
					{
						Controls.select_weapon_count = j+1;
						break;
					}
			break;
		case EVENT_JOYSTICK_MOVED:
		{
			int axis = 0, value = 0, joy_null_value = 0;
			if (!(PlayerCfg.ControlType & CONTROL_USING_JOYSTICK))
				break;
			event_joystick_get_axis(event, &axis, &value);

			Controls.raw_joy_axis[axis] = value;

			if (axis == kc_joystick[13].value) // Pitch U/D Deadzone
				joy_null_value = PlayerCfg.JoystickDead[1]*8;
			if (axis == kc_joystick[15].value) // Turn L/R Deadzone
				joy_null_value = PlayerCfg.JoystickDead[0]*8;
			if (axis == kc_joystick[17].value) // Slide L/R Deadzone
				joy_null_value = PlayerCfg.JoystickDead[2]*8;
			if (axis == kc_joystick[19].value) // Slide U/D Deadzone
				joy_null_value = PlayerCfg.JoystickDead[3]*8;
			if (axis == kc_joystick[21].value) // Bank Deadzone
				joy_null_value = PlayerCfg.JoystickDead[4]*8;
			if (axis == kc_joystick[23].value) // Throttle - default deadzone
				joy_null_value = PlayerCfg.JoystickDead[5]*3;

			Controls.raw_joy_axis[axis] = joy_apply_deadzone(Controls.raw_joy_axis[axis], joy_null_value);
			Controls.joy_axis[axis] = (Controls.raw_joy_axis[axis]*FrameTime)/128;
			break;
		}
		case EVENT_MOUSE_MOVED:
		{
			if (!(PlayerCfg.ControlType & CONTROL_USING_MOUSE))
				break;
			if (PlayerCfg.MouseControlStyle == MOUSE_CONTROL_FLIGHT_SIM) /* Old School Mouse */
			{
				int ax[3];
				event_mouse_get_delta( event, &ax[0], &ax[1], &ax[2] );
				for (i = 0; i <= 2; i++)
				{
					int mouse_null_value = (i==2?16:PlayerCfg.MouseFSDead*8);
					Controls.raw_mouse_axis[i] += ax[i];
					if (Controls.raw_mouse_axis[i] < -MOUSEFS_DELTA_RANGE)
						Controls.raw_mouse_axis[i] = -MOUSEFS_DELTA_RANGE;
					if (Controls.raw_mouse_axis[i] > MOUSEFS_DELTA_RANGE)
						Controls.raw_mouse_axis[i] = MOUSEFS_DELTA_RANGE;
					if (Controls.raw_mouse_axis[i] > mouse_null_value) 
						Controls.mouse_axis[i] = (((Controls.raw_mouse_axis[i]-mouse_null_value)*MOUSEFS_DELTA_RANGE)/(MOUSEFS_DELTA_RANGE-mouse_null_value)*FrameTime)/MOUSEFS_DELTA_RANGE;
					else if (Controls.raw_mouse_axis[i] < -mouse_null_value)
						Controls.mouse_axis[i] = (((Controls.raw_mouse_axis[i]+mouse_null_value)*MOUSEFS_DELTA_RANGE)/(MOUSEFS_DELTA_RANGE-mouse_null_value)*FrameTime)/MOUSEFS_DELTA_RANGE;
					else
						Controls.mouse_axis[i] = 0;
				}
			}
			else if(PlayerCfg.MouseControlStyle == MOUSE_CONTROL_REBIRTH)  /* Old School Mouse */
			{
				event_mouse_get_delta( event, &Controls.raw_mouse_axis[0], &Controls.raw_mouse_axis[1], &Controls.raw_mouse_axis[2] );
				Controls.mouse_axis[0] = (Controls.raw_mouse_axis[0]*FrameTime)/8;
				Controls.mouse_axis[1] = (Controls.raw_mouse_axis[1]*FrameTime)/8;
				Controls.mouse_axis[2] = (Controls.raw_mouse_axis[2]*FrameTime);
				mouse_delta_time = timer_query() + (F1_0/30);
			}

			/* Old School Mouse */
			else if(PlayerCfg.MouseControlStyle == MOUSE_CONTROL_OLDSCHOOL)
			{
				/* Emulate mouse sampling between FrameTime and 20 FPS */ 
				int impulse_factor = ((F1_0/20 / (FrameTime) - 1 ) * PlayerCfg.MouseImpulse) / 15 + 1; 
				//int impulse_factor = 1; 

				event_mouse_get_delta( event, &Controls.raw_mouse_axis[0], &Controls.raw_mouse_axis[1], &Controls.raw_mouse_axis[2] );
				Controls.mouse_axis[0] = (Controls.raw_mouse_axis[0]*FrameTime)/8 * impulse_factor; //;// * PlayerCfg.MouseImpulse / 2;
				Controls.mouse_axis[1] = (Controls.raw_mouse_axis[1]*FrameTime)/8 * impulse_factor;  // ;// * PlayerCfg.MouseImpulse / 2;
				Controls.mouse_axis[2] = (Controls.raw_mouse_axis[2]*FrameTime);
				mouse_delta_time = timer_query() + (F1_0/30);
			}
			/* SNG Mouse - smoothing and sub-pixel precision */
			else if(PlayerCfg.MouseControlStyle == MOUSE_CONTROL_SNG)
			{
				int dx, dy, dz;
				event_mouse_get_delta(event, &dx, &dy, &dz);
				
				float sensitivity = PlayerCfg.MouseSens[0] / 8.0f;
				float raw_x = dx * sensitivity;
				float raw_y = dy * sensitivity;
				
				/* exponential smoothing filter to reduce jitter */
				const float smoothing = 0.7f;
				smooth_x = smooth_x * (1.0f - smoothing) + raw_x * smoothing;
				smooth_y = smooth_y * (1.0f - smoothing) + raw_y * smoothing;
				
				/* Accumulate smoothed values for sub-pixel precision */
				accum_x += smooth_x;
				accum_y += smooth_y;
				
				/* Extract integer values while preserving fractional remainder */
				Controls.mouse_axis[0] = (int)accum_x;
				Controls.mouse_axis[1] = (int)accum_y;
				Controls.mouse_axis[2] = dz;
				
				/* Keep fractional part for next frame */
				accum_x -= (float)Controls.mouse_axis[0];
				accum_y -= (float)Controls.mouse_axis[1];
				
				/* Apply frame-time scaling for consistent response - using /8 like Rebirth for better feel */
				Controls.mouse_axis[0] = (Controls.mouse_axis[0] * FrameTime) / 8;
				Controls.mouse_axis[1] = (Controls.mouse_axis[1] * FrameTime) / 8;
				Controls.mouse_axis[2] = (Controls.mouse_axis[2] * FrameTime);
			}
			break;
		}
		case EVENT_IDLE:
		default:
			if (!(PlayerCfg.MouseControlStyle == MOUSE_CONTROL_FLIGHT_SIM) && mouse_delta_time < timer_query())
			{
				Controls.mouse_axis[0] = Controls.mouse_axis[1] = Controls.mouse_axis[2] = 0;
				mouse_delta_time = timer_query() + (F1_0/30);
			}
			break;
	}

	//------------ Read pitch_time -----------
	if ( !Controls.slide_on_state )
	{
		// From keyboard...
		if ( Controls.key_pitch_forward_state ) 
		{
			if ( Controls.key_pitch_forward_down_time < F1_0 )
				Controls.key_pitch_forward_down_time += (!Controls.key_pitch_forward_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[1]/16)+1:FrameTime/4;
			Controls.pitch_time += speed_factor*FrameTime/2*(Controls.key_pitch_forward_down_time/F1_0);
		}
		else
			Controls.key_pitch_forward_down_time = 0;
		if ( Controls.key_pitch_backward_state )
		{
			if ( Controls.key_pitch_backward_down_time < F1_0 )
				Controls.key_pitch_backward_down_time += (!Controls.key_pitch_backward_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[1]/16)+1:FrameTime/4;
			Controls.pitch_time -= speed_factor*FrameTime/2*(Controls.key_pitch_backward_down_time/F1_0);
		}
		else
			Controls.key_pitch_backward_down_time = 0;
		// From joystick...
		if ( !kc_joystick[14].value ) // If not inverted...
			Controls.pitch_time -= (Controls.joy_axis[kc_joystick[13].value]*PlayerCfg.JoystickSens[1]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[1]))/8;
		else
			Controls.pitch_time += (Controls.joy_axis[kc_joystick[13].value]*PlayerCfg.JoystickSens[1]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[1]))/8;
		// From mouse...
		if ( kc_mouse[13].value != 255 ) {
			if ( !kc_mouse[14].value ) // If not inverted...
				Controls.pitch_time -= (Controls.mouse_axis[kc_mouse[13].value]*PlayerCfg.MouseSens[1])/8;
			else
				Controls.pitch_time += (Controls.mouse_axis[kc_mouse[13].value]*PlayerCfg.MouseSens[1])/8;
		}
	}
	else Controls.pitch_time = 0;

	//----------- Read vertical_thrust_time -----------------
	if ( Controls.slide_on_state )
	{
		// From keyboard...
		if ( Controls.key_pitch_forward_state ) 
		{
			if (Controls.key_pitch_forward_down_time < F1_0)
				Controls.key_pitch_forward_down_time += (!Controls.key_pitch_forward_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[3]/16)+1:FrameTime/4;
			Controls.vertical_thrust_time += speed_factor*FrameTime*(Controls.key_pitch_forward_down_time/F1_0);
		}
		else
			Controls.key_pitch_forward_down_time = 0;
		if ( Controls.key_pitch_backward_state )
		{
			if ( Controls.key_pitch_backward_down_time < F1_0 )
				Controls.key_pitch_backward_down_time += (!Controls.key_pitch_backward_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[3]/16)+1:FrameTime/4;
			Controls.vertical_thrust_time -= speed_factor*FrameTime*(Controls.key_pitch_backward_down_time/F1_0);
		}
		else
			Controls.key_pitch_backward_down_time = 0;
		// From joystick...
		if ( !kc_joystick[20].value /*!kc_joystick[14].value*/ )		// If not inverted... NOTE: Use Slide U/D invert setting
			Controls.vertical_thrust_time += (Controls.joy_axis[kc_joystick[13].value]*PlayerCfg.JoystickSens[3]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[3]))/8;
		else
			Controls.vertical_thrust_time -= (Controls.joy_axis[kc_joystick[13].value]*PlayerCfg.JoystickSens[3]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[3]))/8;
		// From mouse...
		if ( kc_mouse[13].value != 255 ) {
			if ( !kc_mouse[20].value /*!kc_mouse[14].value*/ )		// If not inverted... NOTE: Use Slide U/D invert setting
				Controls.vertical_thrust_time -= (Controls.mouse_axis[kc_mouse[13].value]*PlayerCfg.MouseSens[3])/8;
			else
				Controls.vertical_thrust_time += (Controls.mouse_axis[kc_mouse[13].value]*PlayerCfg.MouseSens[3])/8;
		}
	}
	// From keyboard...
	if ( Controls.key_slide_up_state ) 
	{
		if (Controls.key_slide_up_down_time < F1_0)
			Controls.key_slide_up_down_time += (!Controls.key_slide_up_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[3]/16)+1:FrameTime/4;
		Controls.vertical_thrust_time += speed_factor*FrameTime*(Controls.key_slide_up_down_time/F1_0);
	}
	else
		Controls.key_slide_up_down_time = 0;
	if ( Controls.key_slide_down_state )
	{
		if ( Controls.key_slide_down_down_time < F1_0 )
			Controls.key_slide_down_down_time += (!Controls.key_slide_down_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[3]/16)+1:FrameTime/4;
		Controls.vertical_thrust_time -= speed_factor*FrameTime*(Controls.key_slide_down_down_time/F1_0);
	}
	else
		Controls.key_slide_down_down_time = 0;
	// From buttons...
	if ( Controls.btn_slide_up_state ) Controls.vertical_thrust_time += speed_factor*FrameTime;
	if ( Controls.btn_slide_down_state ) Controls.vertical_thrust_time -= speed_factor*FrameTime;
	// From joystick...
	if ( !kc_joystick[20].value )		// If not inverted...
		Controls.vertical_thrust_time += (Controls.joy_axis[kc_joystick[19].value]*PlayerCfg.JoystickSens[3]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[3]))/8;
	else
		Controls.vertical_thrust_time -= (Controls.joy_axis[kc_joystick[19].value]*PlayerCfg.JoystickSens[3]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[3]))/8;
	// From mouse...
	if ( kc_mouse[19].value != 255 ) {
		if ( !kc_mouse[20].value )		// If not inverted...
			Controls.vertical_thrust_time += (Controls.mouse_axis[kc_mouse[19].value]*PlayerCfg.MouseSens[3])/8;
		else
			Controls.vertical_thrust_time -= (Controls.mouse_axis[kc_mouse[19].value]*PlayerCfg.MouseSens[3])/8;
	}

	//---------- Read heading_time -----------
	if (!Controls.slide_on_state && !Controls.bank_on_state)
	{
		// From keyboard...
		if ( Controls.key_heading_right_state ) 
		{
			if (Controls.key_heading_right_down_time < F1_0)
				Controls.key_heading_right_down_time += (!Controls.key_heading_right_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[0]/16)+1:FrameTime/4;
			Controls.heading_time += speed_factor*FrameTime*(Controls.key_heading_right_down_time/F1_0);
		}
		else
			Controls.key_heading_right_down_time = 0;
		if ( Controls.key_heading_left_state )
		{
			if ( Controls.key_heading_left_down_time < F1_0 )
				Controls.key_heading_left_down_time += (!Controls.key_heading_left_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[0]/16)+1:FrameTime/4;
			Controls.heading_time -= speed_factor*FrameTime*(Controls.key_heading_left_down_time/F1_0);
		}
		else
			Controls.key_heading_left_down_time = 0;
		// From joystick...
		if ( !kc_joystick[16].value )		// If not inverted...
			Controls.heading_time += (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[0]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[0]))/8;
		else
			Controls.heading_time -= (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[0]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[0]))/8;
		// From mouse...
		if ( kc_mouse[15].value != 255 ) {
			if ( !kc_mouse[16].value )		// If not inverted...
				Controls.heading_time += (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[0])/8;
			else
				Controls.heading_time -= (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[0])/8;
		}
	}
	else Controls.heading_time = 0;

	//----------- Read sideways_thrust_time -----------------
	if ( Controls.slide_on_state )
	{
		// From keyboard...
		if ( Controls.key_heading_right_state ) 
		{
			if (Controls.key_heading_right_down_time < F1_0)
				Controls.key_heading_right_down_time += (!Controls.key_heading_right_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[2]/16)+1:FrameTime/4;
			Controls.sideways_thrust_time += speed_factor*FrameTime*(Controls.key_heading_right_down_time/F1_0);
		}
		else
			Controls.key_heading_right_down_time = 0;
		if ( Controls.key_heading_left_state )
		{
			if ( Controls.key_heading_left_down_time < F1_0 )
				Controls.key_heading_left_down_time += (!Controls.key_heading_left_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[2]/16)+1:FrameTime/4;
			Controls.sideways_thrust_time -= speed_factor*FrameTime*(Controls.key_heading_left_down_time/F1_0);
		}
		else
			Controls.key_heading_left_down_time = 0;
		// From joystick...
		if ( !kc_joystick[18].value /*!kc_joystick[16].value*/ )		// If not inverted... NOTE: Use Slide L/R invert setting
			Controls.sideways_thrust_time += (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[2]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[2]))/8;
		else
			Controls.sideways_thrust_time -= (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[2]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[2]))/8;
		// From mouse...
		if ( kc_mouse[15].value != 255 ) {
			if ( !kc_mouse[18].value /*!kc_mouse[16].value*/ )		// If not inverted... NOTE: Use Slide L/R invert setting
				Controls.sideways_thrust_time += (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[2])/8;
			else
				Controls.sideways_thrust_time -= (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[2])/8;
		}
	}
	// From keyboard...
	if ( Controls.key_slide_right_state ) 
	{
		if (Controls.key_slide_right_down_time < F1_0)
			Controls.key_slide_right_down_time += (!Controls.key_slide_right_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[2]/16)+1:FrameTime/4;
		Controls.sideways_thrust_time += speed_factor*FrameTime*(Controls.key_slide_right_down_time/F1_0);
	}
	else
		Controls.key_slide_right_down_time = 0;
	if ( Controls.key_slide_left_state )
	{
		if ( Controls.key_slide_left_down_time < F1_0 )
			Controls.key_slide_left_down_time += (!Controls.key_slide_left_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[2]/16)+1:FrameTime/4;
		Controls.sideways_thrust_time -= speed_factor*FrameTime*(Controls.key_slide_left_down_time/F1_0);
	}
	else
		Controls.key_slide_left_down_time = 0;
	// From buttons...
	if ( Controls.btn_slide_left_state ) Controls.sideways_thrust_time -= speed_factor*FrameTime;
	if ( Controls.btn_slide_right_state ) Controls.sideways_thrust_time += speed_factor*FrameTime;
	// From joystick...
	if ( !kc_joystick[18].value )		// If not inverted...
		Controls.sideways_thrust_time += (Controls.joy_axis[kc_joystick[17].value]*PlayerCfg.JoystickSens[2]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[2]))/8;
	else
		Controls.sideways_thrust_time -= (Controls.joy_axis[kc_joystick[17].value]*PlayerCfg.JoystickSens[2]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[2]))/8;
	// From mouse...
	if ( kc_mouse[17].value != 255 ) {
		if ( !kc_mouse[18].value )		// If not inverted...
			Controls.sideways_thrust_time += (Controls.mouse_axis[kc_mouse[17].value]*PlayerCfg.MouseSens[2])/8;
		else
			Controls.sideways_thrust_time -= (Controls.mouse_axis[kc_mouse[17].value]*PlayerCfg.MouseSens[2])/8;
	}

	//----------- Read bank_time -----------------
	if ( Controls.bank_on_state )
	{
		// From keyboard...
		if ( Controls.key_heading_left_state )
		{
			if ( Controls.key_heading_left_down_time < F1_0 )
				Controls.key_heading_left_down_time += (!Controls.key_heading_left_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[4]/16)+1:FrameTime/4;
			Controls.bank_time += speed_factor*FrameTime*(Controls.key_heading_left_down_time/F1_0);
		}
		else
			Controls.key_bank_left_down_time = 0;
		if ( Controls.key_heading_right_state ) 
		{
			if (Controls.key_heading_right_down_time < F1_0)
				Controls.key_heading_right_down_time += (!Controls.key_heading_right_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[4]/16)+1:FrameTime/4;
			Controls.bank_time -= speed_factor*FrameTime*(Controls.key_heading_right_down_time/F1_0);
		}
		else
			Controls.key_heading_right_down_time = 0;
		// From joystick...
		if ( !kc_joystick[22].value /*!kc_joystick[16].value*/ )		// If not inverted... NOTE: Use Bank L/R invert setting
			Controls.bank_time -= (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[4]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[4]))/8;
		else
			Controls.bank_time += (Controls.joy_axis[kc_joystick[15].value]*PlayerCfg.JoystickSens[4]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[4]))/8;
		// From mouse...
		if ( kc_mouse[15].value != 255 ) {
			if ( !kc_mouse[22].value /*!kc_mouse[16].value*/ )		// If not inverted... NOTE: Use Bank L/R invert setting
				Controls.bank_time += (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[4])/8;
			else
				Controls.bank_time -= (Controls.mouse_axis[kc_mouse[15].value]*PlayerCfg.MouseSens[4])/8;
		}
	}
	// From keyboard...
	if ( Controls.key_bank_left_state )
	{
		if ( Controls.key_bank_left_down_time < F1_0 )
			Controls.key_bank_left_down_time += (!Controls.key_bank_left_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[4]/16)+1:FrameTime/4;
		Controls.bank_time += speed_factor*FrameTime*(Controls.key_bank_left_down_time/F1_0);
	}
	else
		Controls.key_bank_left_down_time = 0;
	if ( Controls.key_bank_right_state ) 
	{
		if (Controls.key_bank_right_down_time < F1_0)
			Controls.key_bank_right_down_time += (!Controls.key_bank_right_down_time)?F1_0*((float)PlayerCfg.KeyboardSens[4]/16)+1:FrameTime/4;
		Controls.bank_time -= speed_factor*FrameTime*(Controls.key_bank_right_down_time/F1_0);
	}
	else
		Controls.key_bank_right_down_time = 0;
	// From buttons...
	if ( Controls.btn_bank_left_state ) Controls.bank_time += speed_factor*FrameTime;
	if ( Controls.btn_bank_right_state ) Controls.bank_time -= speed_factor*FrameTime;
	// From joystick...
	if ( !kc_joystick[22].value )		// If not inverted...
		Controls.bank_time -= (Controls.joy_axis[kc_joystick[21].value]*PlayerCfg.JoystickSens[4]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[4]))/8;
	else
		Controls.bank_time += (Controls.joy_axis[kc_joystick[21].value]*PlayerCfg.JoystickSens[4]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[4]))/8;
	// From mouse...
	if ( kc_mouse[21].value != 255 ) {
		if ( !kc_mouse[22].value )		// If not inverted...
			Controls.bank_time += (Controls.mouse_axis[kc_mouse[21].value]*PlayerCfg.MouseSens[4])/8;
		else
			Controls.bank_time -= (Controls.mouse_axis[kc_mouse[21].value]*PlayerCfg.MouseSens[4])/8;
	}

	//----------- Read forward_thrust_time -------------
	// From keyboard/buttons...
	if ( Controls.accelerate_state ) Controls.forward_thrust_time += speed_factor*FrameTime;
	if ( Controls.reverse_state ) Controls.forward_thrust_time -= speed_factor*FrameTime;
	// From joystick...
	if ( !kc_joystick[24].value )		// If not inverted...
		Controls.forward_thrust_time -= (Controls.joy_axis[kc_joystick[23].value]*PlayerCfg.JoystickSens[5]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[5]))/8;
	else
		Controls.forward_thrust_time += (Controls.joy_axis[kc_joystick[23].value]*PlayerCfg.JoystickSens[5]*undercalibrate_scale(PlayerCfg.JoystickUndercalibrate[5]))/8;
	// From mouse...
	if ( kc_mouse[23].value != 255 ) {
		if ( !kc_mouse[24].value )		// If not inverted...
			Controls.forward_thrust_time -= (Controls.mouse_axis[kc_mouse[23].value]*PlayerCfg.MouseSens[5])/8;
		else
			Controls.forward_thrust_time += (Controls.mouse_axis[kc_mouse[23].value]*PlayerCfg.MouseSens[5])/8;
	}

	//----------- Read cruise-control-type of throttle.
	// For LoNi -- have cruise go to 100% instantly.  Will add option if anyone cares about making it slow.
	if ( Controls.cruise_plus_state ) Cruise_speed = i2f(100); // Cruise_speed += speed_factor*FrameTime*80;
	if ( Controls.cruise_minus_state ) Cruise_speed -= speed_factor*FrameTime*80;
	if ( Controls.cruise_off_count > 0 ) Controls.cruise_off_count = Cruise_speed = 0;
	if (Cruise_speed > i2f(100) ) Cruise_speed = i2f(100);
	if (Cruise_speed < 0 ) Cruise_speed = 0;
	if (Controls.forward_thrust_time==0) Controls.forward_thrust_time = fixmul(Cruise_speed,FrameTime)/100;

	//----------- Add overrun values
    Controls.pitch_time += Controls.pitch_time_overrun;
    Controls.heading_time += Controls.heading_time_overrun;
    Controls.vertical_thrust_time += Controls.vertical_thrust_time_overrun;
    Controls.sideways_thrust_time += Controls.sideways_thrust_time_overrun;
    Controls.bank_time += Controls.bank_time_overrun;
    Controls.forward_thrust_time += Controls.forward_thrust_time_overrun;
    
	//----------- Set a flag to indicate whether there was an overrun for each of the values
    overruns |= (Controls.pitch_time_overrun ? 1 : 0);
    overruns |= (Controls.heading_time_overrun ? 2 : 0);
    overruns |= (Controls.vertical_thrust_time_overrun ? 4 : 0);
    overruns |= (Controls.sideways_thrust_time_overrun ? 8 : 0);
    overruns |= (Controls.bank_time_overrun ? 16 : 0);
    overruns |= (Controls.forward_thrust_time_overrun ? 32 : 0);
    
	//----------- Clear overrun values
    Controls.pitch_time_overrun = 0;
    Controls.heading_time_overrun = 0;
    Controls.vertical_thrust_time_overrun = 0;
    Controls.sideways_thrust_time_overrun = 0;
    Controls.bank_time_overrun = 0;
    Controls.forward_thrust_time_overrun = 0;

	//----------- Clamp values between -FrameTime and FrameTime
	if (Controls.pitch_time > FrameTime/2 ) {
        if (overruns & 1 || (!Controls.slide_on_state && Controls.mouse_axis[kc_mouse[13].value])) {
            Controls.pitch_time_overrun += (Controls.pitch_time - FrameTime/2);
            if (Controls.pitch_time_overrun > F1_0 * PlayerCfg.MouseOverrun[1] / 32) {
                Controls.pitch_time_overrun = F1_0 * PlayerCfg.MouseOverrun[1] / 32;
            }
        }
        Controls.pitch_time = FrameTime/2;
    }
	if (Controls.heading_time > FrameTime ) {
        if (overruns & 2 || (!Controls.slide_on_state && !Controls.bank_on_state && Controls.mouse_axis[kc_mouse[15].value])) {
            Controls.heading_time_overrun += (Controls.heading_time - FrameTime);
            if (Controls.heading_time_overrun > F1_0 * PlayerCfg.MouseOverrun[0] / 16) {
                Controls.heading_time_overrun = F1_0 * PlayerCfg.MouseOverrun[0] / 16;
            }
        }
        Controls.heading_time = FrameTime;
    }
	if (Controls.pitch_time < -FrameTime/2 ) {
        if (overruns & 1 || (!Controls.slide_on_state && Controls.mouse_axis[kc_mouse[13].value])) {
            Controls.pitch_time_overrun += (Controls.pitch_time + FrameTime/2);
            if (Controls.pitch_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[1] / 32) {
                Controls.pitch_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[1] / 32;
            }
        }
        Controls.pitch_time = -FrameTime/2;
    }
	if (Controls.heading_time < -FrameTime ) {
        if (overruns & 2 || (!Controls.slide_on_state && !Controls.bank_on_state && Controls.mouse_axis[kc_mouse[15].value])) {
            Controls.heading_time_overrun += (Controls.heading_time + FrameTime);
            if (Controls.heading_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[0] / 16) {
                Controls.heading_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[0] / 16;
            }
        }
        Controls.heading_time = -FrameTime;
    }
	if (Controls.vertical_thrust_time > speed_factor * FrameTime ) {
        if (overruns & 4 || (Controls.mouse_axis[kc_mouse[19].value] || (Controls.slide_on_state && Controls.mouse_axis[kc_mouse[13].value]))) {
            Controls.vertical_thrust_time_overrun += (Controls.vertical_thrust_time - speed_factor * FrameTime);
            if (Controls.vertical_thrust_time_overrun > F1_0 * PlayerCfg.MouseOverrun[3] / 16) {
                Controls.vertical_thrust_time_overrun = F1_0 * PlayerCfg.MouseOverrun[3] / 16;
            }
        }
        Controls.vertical_thrust_time = speed_factor * FrameTime;
    }
	if (Controls.sideways_thrust_time > speed_factor * FrameTime ) {
        if (overruns & 8 || (Controls.mouse_axis[kc_mouse[17].value] || (Controls.slide_on_state && Controls.mouse_axis[kc_mouse[15].value]))) {
            Controls.sideways_thrust_time_overrun += (Controls.sideways_thrust_time - speed_factor * FrameTime);
            if (Controls.sideways_thrust_time_overrun > F1_0 * PlayerCfg.MouseOverrun[2] / 16) {
                Controls.sideways_thrust_time_overrun = F1_0 * PlayerCfg.MouseOverrun[2] / 16;
            }
        }
        Controls.sideways_thrust_time = speed_factor * FrameTime;
    }
	if (Controls.bank_time > FrameTime ) {
        if (overruns & 16 || (Controls.mouse_axis[kc_mouse[21].value] || (Controls.bank_on_state && Controls.mouse_axis[kc_mouse[15].value]))) {
            Controls.bank_time_overrun += (Controls.bank_time - FrameTime);
            if (Controls.bank_time_overrun > F1_0 * PlayerCfg.MouseOverrun[4] / 16) {
                Controls.bank_time_overrun = F1_0 * PlayerCfg.MouseOverrun[4] / 16;
            }
        }
        Controls.bank_time = FrameTime;
    }
	if (Controls.forward_thrust_time > speed_factor * FrameTime ) {
        if (overruns & 32 || (Controls.mouse_axis[kc_mouse[23].value])) {
            Controls.forward_thrust_time_overrun += (Controls.forward_thrust_time - speed_factor * FrameTime);
            if (Controls.forward_thrust_time_overrun > F1_0 * PlayerCfg.MouseOverrun[5] / 16) {
                Controls.forward_thrust_time_overrun = F1_0 * PlayerCfg.MouseOverrun[5] / 16;
            }
        }
        Controls.forward_thrust_time = speed_factor * FrameTime;
    }
	if (Controls.vertical_thrust_time < -speed_factor * FrameTime ) {
        if (overruns & 4 || (Controls.mouse_axis[kc_mouse[19].value] || (Controls.slide_on_state && Controls.mouse_axis[kc_mouse[13].value]))) {
            Controls.vertical_thrust_time_overrun += (Controls.vertical_thrust_time + speed_factor * FrameTime);
            if (Controls.vertical_thrust_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[3] / 16) {
                Controls.vertical_thrust_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[3] / 16;
            }
        }
        Controls.vertical_thrust_time = -speed_factor * FrameTime;
    }
	if (Controls.sideways_thrust_time < -speed_factor * FrameTime ) {
        if (overruns & 8 || (Controls.mouse_axis[kc_mouse[17].value] || (Controls.slide_on_state && Controls.mouse_axis[kc_mouse[15].value]))) {
            Controls.sideways_thrust_time_overrun += (Controls.sideways_thrust_time + speed_factor * FrameTime);
            if (Controls.sideways_thrust_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[2] / 16) {
                Controls.sideways_thrust_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[2] / 16;
            }
        }
        Controls.sideways_thrust_time = -speed_factor * FrameTime;
    }
	if (Controls.bank_time < -FrameTime ) {
        if (overruns & 16 || (Controls.mouse_axis[kc_mouse[21].value] || (Controls.bank_on_state && Controls.mouse_axis[kc_mouse[15].value]))) {
            Controls.bank_time_overrun += (Controls.bank_time + FrameTime);
            if (Controls.bank_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[4] / 16) {
                Controls.bank_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[4] / 16;
            }
        }
        Controls.bank_time = -FrameTime;
    }
	if (Controls.forward_thrust_time < -speed_factor * FrameTime ) {
        if (overruns & 32 || (Controls.mouse_axis[kc_mouse[23].value])) {
            Controls.forward_thrust_time_overrun += (Controls.forward_thrust_time + speed_factor * FrameTime);
            if (Controls.forward_thrust_time_overrun < F1_0 * -PlayerCfg.MouseOverrun[5] / 16) {
                Controls.forward_thrust_time_overrun = F1_0 * -PlayerCfg.MouseOverrun[5] / 16;
            }
        }
        Controls.forward_thrust_time = -speed_factor * FrameTime;
    }
}

void reset_cruise(void)
{
	Cruise_speed=0;
}


void kc_set_controls()
{
	int i;

	for (i=0; i<NUM_KEY_CONTROLS; i++ )
		kc_keyboard[i].value = PlayerCfg.KeySettings[0][i];

	for (i=0; i<NUM_JOYSTICK_CONTROLS; i++ )
	{
		kc_joystick[i].value = PlayerCfg.KeySettings[1][i];
		if (kc_joystick[i].type == BT_INVERT )
		{
			if (kc_joystick[i].value!=1)
				kc_joystick[i].value = 0;
			PlayerCfg.KeySettings[1][i] = kc_joystick[i].value;
		}
	}

	for (i=0; i<NUM_MOUSE_CONTROLS; i++ )
	{
		kc_mouse[i].value = PlayerCfg.KeySettings[2][i];
		if (kc_mouse[i].type == BT_INVERT )
		{
			if (kc_mouse[i].value!=1)
				kc_mouse[i].value = 0;
			PlayerCfg.KeySettings[2][i] = kc_mouse[i].value;
		}
	}

	for (i=0; i<NUM_D1X_CONTROLS; i++ )
		kc_d1x[i].value = PlayerCfg.KeySettingsD1X[i];
}
