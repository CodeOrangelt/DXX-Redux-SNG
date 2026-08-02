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
 * Powerup header file.
 *
 */

#ifndef _POWERUP_H
#define _POWERUP_H

#include "vclip.h"
#include "player.h"

enum powerup_type_t
{
	POW_EXTRA_LIFE = 0,
	POW_ENERGY = 1,
	POW_SHIELD_BOOST = 2,
	POW_LASER = 3,

	POW_KEY_BLUE = 4,
	POW_KEY_RED = 5,
	POW_KEY_GOLD = 6,

	POW_RADAR_ROBOTS = 7,
	POW_RADAR_POWERUPS = 8,
	POW_FULL_MAP = 9,

	POW_MISSILE_1 = 10,
	POW_MISSILE_4 = 11,      // 4-pack MUST follow single missile

	POW_QUAD_FIRE = 12,

	POW_VULCAN_WEAPON = 13,
	POW_SPREADFIRE_WEAPON = 14,
	POW_PLASMA_WEAPON = 15,
	POW_FUSION_WEAPON = 16,
	POW_PROXIMITY_WEAPON = 17,
	POW_HOMING_AMMO_1 = 18,
	POW_HOMING_AMMO_4 = 19,
	POW_SMARTBOMB_WEAPON = 20,
	POW_MEGA_WEAPON = 21,
	POW_VULCAN_AMMO = 22,
	POW_CLOAK = 23,
	POW_TURBO = 24,
	POW_INVULNERABILITY = 25,
	POW_HEADLIGHT = 26,
	POW_MEGAWOW = 27,
};

#define VULCAN_AMMO_MAX             (392*2)
#define VULCAN_WEAPON_AMMO_AMOUNT   196
#define VULCAN_AMMO_AMOUNT          (49*2)

// What I picked up        What it said I picked up
// ----------------        ------------------------
// vulcan ammo             4 homing missiles
// mega missile            1 homing missile
// smart missile           vulcan ammo
// 4 homing missiles       mega missile
// 1 homing missile        smart missile
// 
// The rest were correct.  I can help you with this whenever you're free.

#define MAX_POWERUP_TYPES			29

#define POWERUP_NAME_LENGTH 16      // Length of a robot or powerup name.
extern char Powerup_names[MAX_POWERUP_TYPES][POWERUP_NAME_LENGTH];

typedef struct powerup_type_info {
	int vclip_num;
	int hit_sound;
	fix size;       // 3d size of longest dimension
	fix light;      // amount of light cast by this powerup, set in bitmaps.tbl
} __pack__ powerup_type_info;

extern int N_powerup_types;
extern powerup_type_info Powerup_info[MAX_POWERUP_TYPES];

void draw_powerup(object *obj);

//returns true if powerup consumed
int do_powerup(object *obj);

//process (animate) a powerup for one frame
void do_powerup_frame(object *obj);

// Diminish shields and energy towards max in case they exceeded it.
extern void diminish_towards_max(void);

extern void do_megawow_powerup(int quantity);

extern void powerup_basic(int redadd, int greenadd, int blueadd, int score, char *format, ...);

// may this powerup be added to the level?
// returns number of powerups left if true, otherwise 0.
extern int may_create_powerup(int powerup);

/*
 * reads n powerup_type_info structs from a PHYSFS_file
 */
extern int powerup_type_info_read_n(powerup_type_info *pti, int n, PHYSFS_file *fp);

extern vms_vector blue_key_pos;
extern int blue_key_seg;
extern vms_vector red_key_pos;
extern int red_key_seg;

// SNG: Static Powerups - resets the local player's per-life "already collected" tracker
extern void reset_static_powerups_collected(void);

// SNG: Arcade mode "super powers".
//
// A super power is an ordinary powerup object that has been tagged as special.
// The tag lives in ctype.powerup_info.count, which is the only spare per-powerup
// field the network code already replicates: multi_object_to_object_rw() and
// multi_object_rw_to_object() both copy it, so a super power stays a super power
// for clients that join after it was spawned, and the tag survives object sync.
// count is otherwise only meaningful for the vulcan powerups, which are not used
// by any super power.
#define ARCADE_SP_HOMING        0   // 6 homing missiles
#define ARCADE_SP_MEGA          1   // 1 mega missile
#define ARCADE_SP_SMART         2   // 3 smart missiles
#define ARCADE_SP_PROXY         3   // 8 proximity bombs
#define ARCADE_SP_CLOAK         4   // cloak
#define ARCADE_SP_INVULN        5   // invulnerability
#define ARCADE_SP_ENERGY        6   // temporary infinite energy
#define NUM_ARCADE_SUPERPOWERS  7

// Tag base. Real powerups carry a count of 1 (or a vulcan ammo count), so this
// is far outside any legitimate value.
#define ARCADE_MARK_BASE        1000

// Ceiling applied when a super power pushes a secondary past its normal maximum.
#define ARCADE_SECONDARY_HARD_MAX 200

// How long the "incoming" banner stays up after the countdown reaches zero.
#define ARCADE_ANNOUNCE_LINGER (F1_0 * 3 / 2)

// Which powerup object type represents this super power.
extern int arcade_superpower_powerup_type(int spid);

// Name of a super power, including the configured amount, for on-screen text.
extern const char *arcade_superpower_name(int spid);

// Stable label for the options menu (no amount baked in).
extern const char *arcade_superpower_menu_name(int spid);

// How much of the relevant secondary this super power grants.
extern int arcade_superpower_amount(int spid);

// Is this super power allowed to spawn in the current game?
extern int arcade_superpower_enabled(int spid);

// Populates the Netgame.Arcade* settings with their defaults.
extern void arcade_set_default_netgame_options(void);

// Tag an already-created powerup object as a super power.
extern void arcade_mark_superpower(int objnum, int spid);

// Super power carried by this object, or -1 if it is an ordinary powerup.
extern int arcade_object_superpower(object *obj);

// Apply a super power to the local player. Returns 1 if the powerup is consumed.
extern int arcade_pick_up_superpower(object *obj, int spid);

// Per-frame upkeep for the temporary infinite energy super power.
extern void arcade_do_infinite_energy_frame(void);

// Cleared on level start and on respawn.
extern void arcade_reset_player_state(void);

// GameTime64 at which infinite energy runs out; 0 when inactive.
extern fix64 Arcade_infinite_energy_end;

// Centre-screen "incoming super power" banner.
extern void arcade_start_announcement(int spid, int seconds);
extern void arcade_clear_announcement(void);
extern void arcade_draw_announcement(void);
extern void arcade_draw_superpower_labels(void);
extern void arcade_do_announcement_frame(void);
extern int arcade_announce_seconds_left(void);
extern int Arcade_announce_spid;		// -1 when nothing is announced
extern fix64 Arcade_announce_drop_time;	// GameTime64 the countdown reaches zero

#endif /* _POWERUP_H */

