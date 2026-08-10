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
 * Code for controlling player movement
 *
 */


#include <stdlib.h>
#include "key.h"
#include "joy.h"
#include "timer.h"
#include "dxxerror.h"
#include "inferno.h"
#include "game.h"
#include "object.h"
#include "player.h"
#include "controls.h"
#include "render.h"
#include "args.h"
#include "palette.h"
#include "mouse.h"
#include "kconfig.h"
#include "playsave.h" // CED -- OSM 2.0
#include "survival.h"

//look at keyboard, mouse, joystick, CyberMan, whatever, and set 
//physics vars rotvel, velocity

void read_flying_controls( object * obj )
{
	Assert(FrameTime > 0); 		//Get MATT if hit this!

#ifdef NETWORK
	// Menu takes priority: while the shop blocks input, kconfig_read_
	// controls() never runs (see the should_read_controls gate in
	// gamecntl.c's ReadControls()), so Controls.* below would otherwise
	// just keep holding whatever value they had the instant the shop
	// opened -- if the player was mid-thrust or mid-turn right then, the
	// ship would keep drifting/spinning on that stale input for the whole
	// time the menu is up. Zero and bail before any of it gets read.
	if ((Game_mode & GM_MULTI) && survival_shop_blocks_input())
	{
		vm_vec_zero(&obj->mtype.phys_info.thrust);
		vm_vec_zero(&obj->mtype.phys_info.rotthrust);
		return;
	}
#endif

	int no_thrust = 0;
#ifdef NETWORK
	if((Game_mode & GM_NETWORK) && (Netgame.SpawnStyle == SPAWN_STYLE_PREVIEW) && Player_is_dead && Player_exploded) {
		no_thrust = 1; 
	}
#endif

	//	Couldn't the "50" in the next three lines be changed to "64" with no ill effect?
	obj->mtype.phys_info.rotthrust.x = Controls.pitch_time;
	obj->mtype.phys_info.rotthrust.y = Controls.heading_time;
	obj->mtype.phys_info.rotthrust.z = Controls.bank_time;

	if(! no_thrust) {

		// Set object's thrust vector for forward/backward
		vm_vec_copy_scale(&obj->mtype.phys_info.thrust,&obj->orient.fvec, Controls.forward_thrust_time );
		
		// slide left/right
		vm_vec_scale_add2(&obj->mtype.phys_info.thrust,&obj->orient.rvec, Controls.sideways_thrust_time );

		// slide up/down
		vm_vec_scale_add2(&obj->mtype.phys_info.thrust,&obj->orient.uvec, Controls.vertical_thrust_time );
	}

	
	if (!is_observer() && obj->mtype.phys_info.flags & PF_WIGGLE) {
		fix swiggle;
		fix_fastsincos(((fix)GameTime64), &swiggle, NULL);
		if (FrameTime < F1_0) // Only scale wiggle if getting at least 1 FPS, to avoid causing the opposite problem.
			swiggle = fixmul(swiggle*30, FrameTime); //make wiggle fps-independant (based on pre-scaled amount of wiggle at 30 FPS)
		vm_vec_scale_add2(&obj->mtype.phys_info.velocity,&obj->orient.uvec,fixmul(swiggle,Player_ship->wiggle));
	}

	// As of now, obj->mtype.phys_info.thrust & obj->mtype.phys_info.rotthrust are 
	// in units of time... In other words, if thrust==FrameTime, that
	// means that the user was holding down the Max_thrust key for the
	// whole frame.  So we just scale them up by the max, and divide by
	// FrameTime to make them independant of framerate

	//	Prevent divide overflows on high frame rates.
	//	In a signed divide, you get an overflow if num >= div<<15
	{
		fix	ft = FrameTime;

		//	Note, you must check for ft < F1_0/2, else you can get an overflow  on the << 15.
		if ((ft < F1_0/2) && (ft << 15 <= Player_ship->max_thrust)) {
			ft = (Player_ship->max_thrust >> 15) + 1;
		}

		vm_vec_scale( &obj->mtype.phys_info.thrust, fixdiv(Player_ship->max_thrust,ft) );

		// Survival shop: a purchased speed tier scales thrust only, not
		// rotation -- a faster ship, not a twitchier one.
		vm_vec_scale( &obj->mtype.phys_info.thrust, survival_speed_multiplier() );

		if ((ft < F1_0/2) && (ft << 15 <= Player_ship->max_rotthrust)) {
			ft = (Player_ship->max_thrust >> 15) + 1;
		}

		vm_vec_scale( &obj->mtype.phys_info.rotthrust, fixdiv(Player_ship->max_rotthrust,ft) );
	}

	//moved here by WraithX
	if (Player_is_dead) {
		//vm_vec_zero(&obj->mtype.phys_info.rotthrust); //let dead players rotate, changed by WraithX
		vm_vec_zero(&obj->mtype.phys_info.thrust);  //don't let dead players move, changed by WraithX
		return;
	}//end if


}
