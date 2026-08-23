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
 * Header file for multiplayer robot support.
 *
 */

#ifdef NETWORK

#ifndef _MULTIBOT_H
#define _MULTIBOT_H

#ifndef SHAREWARE

// Capacity of the robot-control tables (robot_controlled[] and friends).
// This is only the array size -- the number of slots a machine will actually
// hand out is multi_max_robots_controlled() below, which stays at the stock
// value of 3 for every mode except Survival.
#define MAX_ROBOTS_CONTROLLED 32

// Stock Descent value, and still what every non-Survival mode uses. In
// multiplayer, a robot is only simulated by whichever machine currently
// "controls" it (see multi_can_move_robot()); an uncontrolled robot fails
// every ai_multiplayer_awareness() check in do_ai_frame() and so does not
// move or fire at all. With three slots per machine, a Survival wave of a
// dozen robots left most of them standing perfectly still until something
// (usually being shot, which spikes their agitation past the currently
// controlled ones) evicted a slot for them.
#define STOCK_ROBOTS_CONTROLLED 3

#define ROBOT_FIRE_AGITATION 94

// How many robots this machine may control concurrently right now. Survival
// gets the full table because its whole premise is a large simultaneous
// horde; everything else keeps stock behaviour and stock bandwidth.
int multi_max_robots_controlled(void);

extern int robot_controlled[MAX_ROBOTS_CONTROLLED];
extern int robot_agitation[MAX_ROBOTS_CONTROLLED];
extern int robot_fired[MAX_ROBOTS_CONTROLLED];

int multi_can_move_robot(int objnum, int agitation);
void multi_send_robot_position(int objnum, int fired);
void multi_send_robot_fire(int objnum, int gun_num, vms_vector *fire);
void multi_send_claim_robot(int objnum);
void multi_send_robot_explode(int objnum, int killer, char unused);
void multi_send_create_robot(int robotcen, int objnum, int type);
void multi_send_boss_actions(int bossobjnum, int action, int secondary, int objnum);
int multi_send_robot_frame(int sent);

void multi_do_robot_explode(const ubyte *buf);
void multi_do_robot_position(const ubyte *buf);
void multi_do_claim_robot(const ubyte *buf);
void multi_do_release_robot(const ubyte *buf);
void multi_do_robot_fire(const ubyte *buf);
void multi_do_create_robot(const ubyte *buf);
void multi_do_boss_actions(const ubyte *buf);
void multi_do_create_robot_powerups(const ubyte *buf);

int multi_explode_robot_sub(int botnum, int killer, char unused);

void multi_drop_robot_powerups(int objnum);
// Broadcasts the powerups a just-killed robot dropped, using del_obj's
// contains_* fields plus the Net_create_objnums[] the preceding
// object_create_egg() filled in. Exposed for survival.c, which rolls its own
// drop table and emits one of these per drop.
void multi_send_create_robot_powerups(struct object *del_obj);
void multi_dump_robots(void);

void multi_strip_robots(int playernum);
void multi_check_robot_timeout(void);

void multi_robot_request_change(object *robot, int playernum);


void reset_respawnable_bots();
void kill_respawnable_robot(object *bot);
void check_robot_respawns();
void multi_send_respawn_robot(short objnum);
void multi_do_respawn_robot(const ubyte *buf);

short respawn_robot(ubyte id, short segnum, vms_vector pos, vms_matrix orient, fix size, ubyte behavior);

#endif
#endif
#endif
 
