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
 * Code for powerup objects.
 *
 */



#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "3d.h"
#include "inferno.h"
#include "object.h"
#include "game.h"
#include "fireball.h"
#include "powerup.h"
#include "gauges.h"
#include "sounds.h"
#include "player.h"
#include "wall.h"
#include "text.h"
#include "weapon.h"
#include "laser.h"
#include "scores.h"
#include "multi.h"
#ifdef NETWORK
#include "survival.h"
#endif
#include "newdemo.h"
#ifdef EDITOR
#include "gr.h"	//	for powerup outline drawing
#include "editor/editor.h"
#endif
#include "playsave.h"

int N_powerup_types = 0;
powerup_type_info Powerup_info[MAX_POWERUP_TYPES];

int powerup_start_level[MAX_POWERUP_TYPES];

//process this powerup for this frame
void do_powerup_frame(object *obj)
{
	vclip_info *vci = &obj->rtype.vclip_info;
	vclip *vc = &Vclip[vci->vclip_num];

	vci->frametime -= FrameTime;
	
	while (vci->frametime < 0 ) {

		vci->frametime += vc->frame_time;
		
		vci->framenum++;
		if (vci->framenum >= vc->num_frames)
			vci->framenum=0;
	}

	if (obj->lifeleft <= 0) {
		object_create_explosion(obj->segnum, &obj->pos, fl2f(3.5), VCLIP_POWERUP_DISAPPEARANCE );

		if ( Vclip[VCLIP_POWERUP_DISAPPEARANCE].sound_num > -1 )
			digi_link_sound_to_object( Vclip[VCLIP_POWERUP_DISAPPEARANCE].sound_num, obj-Objects, 0, F1_0);
	}
}

#ifdef EDITOR
extern fix blob_vertices[];

//	blob_vertices has 3 vertices in it, 4th must be computed
void draw_blob_outline(void)
{
	fix	v3x, v3y;

	v3x = blob_vertices[4] - blob_vertices[2] + blob_vertices[0];
	v3y = blob_vertices[5] - blob_vertices[3] + blob_vertices[1];

	gr_setcolor(BM_XRGB(63, 63, 63));

	gr_line(blob_vertices[0], blob_vertices[1], blob_vertices[2], blob_vertices[3]);
	gr_line(blob_vertices[2], blob_vertices[3], blob_vertices[4], blob_vertices[5]);
	gr_line(blob_vertices[4], blob_vertices[5], v3x, v3y);

	gr_line(v3x, v3y, blob_vertices[0], blob_vertices[1]);
}
#endif

void draw_powerup(object *obj)
{
	#ifdef EDITOR
	blob_vertices[0] = 0x80000;
	#endif

	draw_object_blob(obj, Vclip[obj->rtype.vclip_info.vclip_num].frames[obj->rtype.vclip_info.framenum] );

	#ifdef EDITOR
	if (EditorWindow && (Cur_object_index == obj-Objects))
		if (blob_vertices[0] != 0x80000)
			draw_blob_outline();
	#endif

}

void powerup_basic(int redadd, int greenadd, int blueadd, int score, char *format, ...)
{
	va_list	args;

	va_start(args, format );
	HUD_init_message_va(HM_DEFAULT, format, args);
	va_end(args);

	if (!(Game_mode & GM_MULTI) || !Netgame.ReducedFlash)
		PALETTE_FLASH_ADD(redadd,greenadd,blueadd);

	add_points_to_score(score);

}

//#ifndef RELEASE
//	Give the megawow powerup!
void do_megawow_powerup(int quantity)
{
	int i;

	powerup_basic(30, 0, 30, 1, "MEGA-WOWIE-ZOWIE!");
#ifndef SHAREWARE
	Players[Player_num].primary_weapon_flags = 0xff;
	Players[Player_num].secondary_weapon_flags = 0xff;
#else
	Players[Player_num].primary_weapon_flags = 0xff ^ (HAS_PLASMA_FLAG | HAS_FUSION_FLAG);
	Players[Player_num].secondary_weapon_flags = 0xff ^ (HAS_SMART_FLAG | HAS_MEGA_FLAG);
#endif
	for (i=0; i<3; i++)
		Players[Player_num].primary_ammo[i] = 200;

	for (i=0; i<3; i++)
		Players[Player_num].secondary_ammo[i] = quantity;

#ifndef SHAREWARE
	for (i=3; i<5; i++)
		Players[Player_num].primary_ammo[i] = 200;

	for (i=3; i<5; i++)
		Players[Player_num].secondary_ammo[i] = quantity/5;
#endif

	if (Newdemo_state == ND_STATE_RECORDING)
		newdemo_record_laser_level(Players[Player_num].laser_level, MAX_LASER_LEVEL);

	Players[Player_num].energy = F1_0*200;
	Players[Player_num].shields = F1_0*200;
	Players[Player_num].flags |= PLAYER_FLAGS_QUAD_LASERS;
	Players[Player_num].laser_level = MAX_LASER_LEVEL;
	update_laser_weapon_info();

}
//#endif

int pick_up_energy(void)
{
	int	used=0;

	if (Players[Player_num].energy < MAX_ENERGY) {
		Players[Player_num].energy += 3*F1_0 + 3*F1_0*(NDL - Difficulty_level);
		if (Players[Player_num].energy > MAX_ENERGY)
			Players[Player_num].energy = MAX_ENERGY;
		powerup_basic(15,15,7, ENERGY_SCORE, "%s %s %d",TXT_ENERGY,TXT_BOOSTED_TO,f2ir(Players[Player_num].energy));
		used=1;

		if (Game_mode & GM_MULTI)
			multi_send_ship_status();
	} else
		HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, TXT_MAXED_OUT,TXT_ENERGY);

	return used;
}

int pick_up_vulcan_ammo(void)
{
	int	used=0;

//added/killed on 1/21/99 by Victor Rachels ... how is this wrong?
//-killed-        int     pwsave = Players[Player_num].primary_weapon;                // Ugh, save selected primary weapon around the picking up of the ammo.  I apologize for this code.  Matthew A. Toschlog
	if (pick_up_ammo(CLASS_PRIMARY, VULCAN_INDEX, VULCAN_AMMO_AMOUNT)) {
		VulcanAmmoBoxesOnBoard[Player_num] += 1;
		VulcanBoxAmmo[Player_num] += VULCAN_AMMO_AMOUNT;
		powerup_basic(7, 14, 21, VULCAN_AMMO_SCORE, "%s!", TXT_VULCAN_AMMO);
		// SNG: restock the mine as soon as a box is taken, rather than waiting
		// for whoever picked it up to fire it dry. Only meaningful under the
		// Steady Respawning gauss style - the other styles don't tie vulcan
		// ammo pickups to a host-managed restock at all.
		if ((Game_mode & GM_MULTI) && !(Game_mode & GM_MULTI_COOP) &&
		    Netgame.GaussAmmoStyle == GAUSS_STYLE_STEADY_RESPAWNING)
			maybe_drop_net_powerup(POW_VULCAN_AMMO);
		used = 1;
	} else {
		HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, "%s %d %s!",TXT_ALREADY_HAVE,f2i(VULCAN_AMMO_SCALE * Primary_ammo_max[VULCAN_INDEX]),TXT_VULCAN_ROUNDS);
		used = 0;
	}
//-killed-        Players[Player_num].primary_weapon = pwsave;
//end this section kill - VR

	return used;
}

vms_vector blue_key_pos;// position flag(blue) is in
int blue_key_seg;// segment flag(blue) is in
vms_vector red_key_pos;// position flag(red) is in
int red_key_seg;// segment flag(red) is in

// SNG: Static Powerups - tracks which static weapon types the local player has
// already collected this life. Static pickups are never removed from the level
// (so they're always there for anyone), but each player only benefits - and
// hears the pickup sound - once per life.
#define STATIC_COLLECTED_VULCAN  (1<<0)
#define STATIC_COLLECTED_SPREAD  (1<<1)
#define STATIC_COLLECTED_PLASMA  (1<<2)
#define STATIC_COLLECTED_FUSION  (1<<3)
#define STATIC_COLLECTED_LASER   (1<<4)
static ubyte StaticPowerupsCollected = 0;

void reset_static_powerups_collected(void)
{
	StaticPowerupsCollected = 0;
}

// SNG: Arcade mode "super powers" ------------------------------------------------

fix64 Arcade_infinite_energy_end = 0;

// Energy level the "infinite energy" super power maintains. This is the
// player's own energy at the moment of pickup, not MAX_ENERGY - the point is
// unlimited firing without paying energy cost, not a free top-up. It tracks
// upward if the player picks up real energy while it's active, but is never
// pushed up by the super power itself.
static fix Arcade_infinite_energy_floor = 0;

// Announcement currently on screen. Every machine keeps its own copy; the
// spawner broadcasts the start of one and each client counts down locally.
int Arcade_announce_spid = -1;
fix64 Arcade_announce_drop_time = 0;

int arcade_superpower_powerup_type(int spid)
{
	switch (spid) {
		case ARCADE_SP_HOMING: return POW_HOMING_AMMO_4;
		case ARCADE_SP_MEGA:   return POW_MEGA_WEAPON;
		case ARCADE_SP_SMART:  return POW_SMARTBOMB_WEAPON;
		case ARCADE_SP_PROXY:  return POW_PROXIMITY_WEAPON;
		case ARCADE_SP_CLOAK:  return POW_CLOAK;
		case ARCADE_SP_INVULN: return POW_INVULNERABILITY;
		case ARCADE_SP_ENERGY: return POW_ENERGY;
	}
	return -1;
}

void arcade_set_default_netgame_options(void)
{
	int i;

	Netgame.ArcadeTeams      = 0;
	Netgame.ArcadeInterval   = 45;
	Netgame.ArcadeMaxActive  = 14;
	Netgame.ArcadeCountdown  = 3;
	Netgame.ArcadeEnergyTime = 8;
	Netgame.ArcadeHomingCount = 6;
	Netgame.ArcadeSmartCount  = 3;
	Netgame.ArcadeProxyCount  = 8;
	Netgame.ArcadeMegaCount   = 1;

	for (i = 0; i < NUM_ARCADE_SUPERPOWERS; i++)
		Netgame.ArcadeEnabled[i] = 1;
}

int arcade_superpower_enabled(int spid)
{
	if (spid < 0 || spid >= NUM_ARCADE_SUPERPOWERS)
		return 0;

	return Netgame.ArcadeEnabled[spid] != 0;
}

// How much of the relevant secondary a super power hands out. Only meaningful
// for the missile super powers.
int arcade_superpower_amount(int spid)
{
	switch (spid) {
		case ARCADE_SP_HOMING: return Netgame.ArcadeHomingCount;
		case ARCADE_SP_MEGA:   return Netgame.ArcadeMegaCount;
		case ARCADE_SP_SMART:  return Netgame.ArcadeSmartCount;
		case ARCADE_SP_PROXY:  return Netgame.ArcadeProxyCount;
	}
	return 0;
}

// Stable label for the options menu, where the amount is a separate slider.
const char *arcade_superpower_menu_name(int spid)
{
	switch (spid) {
		case ARCADE_SP_HOMING: return "Homing missiles";
		case ARCADE_SP_MEGA:   return "Mega missiles";
		case ARCADE_SP_SMART:  return "Smart missiles";
		case ARCADE_SP_PROXY:  return "Proximity bombs";
		case ARCADE_SP_CLOAK:  return "Cloak";
		case ARCADE_SP_INVULN: return "Invulnerability";
		case ARCADE_SP_ENERGY: return "Infinite energy";
	}
	return "Super power";
}

// Names carry the configured amount, so the announcement and the pickup message
// always describe what this game actually hands out.
const char *arcade_superpower_name(int spid)
{
	static char name[40];
	int amount = arcade_superpower_amount(spid);

	switch (spid) {
		case ARCADE_SP_HOMING:
			snprintf(name, sizeof(name), "%d HOMING MISSILE%s", amount, amount == 1 ? "" : "S");
			return name;
		case ARCADE_SP_MEGA:
			snprintf(name, sizeof(name), "%d MEGA MISSILE%s", amount, amount == 1 ? "" : "S");
			return name;
		case ARCADE_SP_SMART:
			snprintf(name, sizeof(name), "%d SMART MISSILE%s", amount, amount == 1 ? "" : "S");
			return name;
		case ARCADE_SP_PROXY:
			snprintf(name, sizeof(name), "%d PROXIMITY BOMB%s", amount, amount == 1 ? "" : "S");
			return name;
		case ARCADE_SP_CLOAK:  return "CLOAK";
		case ARCADE_SP_INVULN: return "INVULNERABILITY";
		case ARCADE_SP_ENERGY: return "INFINITE ENERGY";
	}
	return "SUPER POWER";
}

void arcade_mark_superpower(int objnum, int spid)
{
	if (objnum < 0 || objnum > Highest_object_index)
		return;
	if (Objects[objnum].type != OBJ_POWERUP)
		return;
	if (spid < 0 || spid >= NUM_ARCADE_SUPERPOWERS)
		return;

	Objects[objnum].ctype.powerup_info.count = ARCADE_MARK_BASE + spid;
}

int arcade_object_superpower(object *obj)
{
	int spid;

	if (!(Game_mode & GM_ARCADE))
		return -1;
	if (obj->type != OBJ_POWERUP)
		return -1;

	spid = obj->ctype.powerup_info.count - ARCADE_MARK_BASE;
	if (spid < 0 || spid >= NUM_ARCADE_SUPERPOWERS)
		return -1;

	// The tag and the object have to agree, so a stale count can never turn an
	// unrelated powerup into a super power.
	if (arcade_superpower_powerup_type(spid) != obj->id)
		return -1;

	return spid;
}

int arcade_pick_up_superpower(object *obj, int spid)
{
	int used = 1;

	switch (spid) {
		case ARCADE_SP_HOMING:
			arcade_add_secondary(HOMING_INDEX, arcade_superpower_amount(spid));
			break;
		case ARCADE_SP_MEGA:
			arcade_add_secondary(MEGA_INDEX, arcade_superpower_amount(spid));
			break;
		case ARCADE_SP_SMART:
			arcade_add_secondary(SMART_INDEX, arcade_superpower_amount(spid));
			break;
		case ARCADE_SP_PROXY:
			arcade_add_secondary(PROXIMITY_INDEX, arcade_superpower_amount(spid));
			break;
		case ARCADE_SP_CLOAK:
			// Unlike the stock powerup this refreshes an active cloak rather
			// than refusing the pickup, so a super power is never wasted.
			Players[Player_num].cloak_time = GameTime64;
			Players[Player_num].flags |= PLAYER_FLAGS_CLOAKED;
			ai_do_cloak_stuff();
#ifdef NETWORK
			if (Game_mode & GM_MULTI)
				multi_send_cloak();
#endif
			break;
		case ARCADE_SP_INVULN:
			Players[Player_num].invulnerable_time = GameTime64;
			Players[Player_num].flags |= PLAYER_FLAGS_INVULNERABLE;
#ifdef NETWORK
			if (Game_mode & GM_MULTI)
				multi_send_ship_status();
#endif
			break;
		case ARCADE_SP_ENERGY:
			Arcade_infinite_energy_end = GameTime64 + i2f(Netgame.ArcadeEnergyTime);
			// Extending an already-active window keeps the higher floor rather
			// than resetting down to whatever energy happens to be right now.
			if (Players[Player_num].energy > Arcade_infinite_energy_floor)
				Arcade_infinite_energy_floor = Players[Player_num].energy;
			break;
		default:
			return 0;
	}

	powerup_basic(20, 20, 0, 0, "SUPER POWER: %s!", arcade_superpower_name(spid));

	// SNG: Arcade - tell everyone else who just grabbed it. Mirrors the CTF
	// "has scored!" broadcast: set Network_message and let multi_send_message()
	// pick it up next frame, which prefixes it with the picker's callsign for
	// every other client. The picker themselves already saw the message above.
	if (Game_mode & GM_MULTI) {
		snprintf(Network_message, sizeof(Network_message), "picked up %s!", arcade_superpower_name(spid));
		Network_message_reciever = 100;
	}

	if (Powerup_info[obj->id].hit_sound > -1) {
#ifdef NETWORK
		if (Game_mode & GM_MULTI)
			multi_send_play_sound(Powerup_info[obj->id].hit_sound, F1_0);
#endif
		digi_play_sample(Powerup_info[obj->id].hit_sound, F1_0);
	}

	return used;
}

void arcade_do_infinite_energy_frame(void)
{
	if (!(Game_mode & GM_ARCADE) || !Arcade_infinite_energy_end)
		return;

	if (GameTime64 >= Arcade_infinite_energy_end) {
		Arcade_infinite_energy_end = 0;
		Arcade_infinite_energy_floor = 0;
		HUD_init_message(HM_DEFAULT, "Infinite energy expired!");
		return;
	}

	// A real energy pickup during the window raises the floor to match; the
	// super power itself never grants energy, only refuses to let it drop.
	if (Players[Player_num].energy > Arcade_infinite_energy_floor)
		Arcade_infinite_energy_floor = Players[Player_num].energy;
	else if (Players[Player_num].energy < Arcade_infinite_energy_floor)
		Players[Player_num].energy = Arcade_infinite_energy_floor;
}

void arcade_reset_player_state(void)
{
	Arcade_infinite_energy_end = 0;
	Arcade_infinite_energy_floor = 0;
}

// Last whole second we played a countdown voice for, so each number is spoken
// once. -1 means nothing is counting down.
static int Arcade_last_countdown_sound = -1;

void arcade_start_announcement(int spid, int seconds)
{
	if (spid < 0 || spid >= NUM_ARCADE_SUPERPOWERS)
		return;

	Arcade_announce_spid = spid;
	Arcade_announce_drop_time = GameTime64 + i2f(seconds);
	Arcade_last_countdown_sound = -1;
}

void arcade_clear_announcement(void)
{
	Arcade_announce_spid = -1;
	Arcade_announce_drop_time = 0;
	Arcade_last_countdown_sound = -1;
}

// Seconds remaining on the banner; 0 once the super power has dropped.
int arcade_announce_seconds_left(void)
{
	fix64 remaining;

	if (Arcade_announce_spid < 0)
		return 0;

	remaining = Arcade_announce_drop_time - GameTime64;

	return (remaining > 0) ? f2i(remaining) + 1 : 0;
}

// Speaks the countdown using the same voice samples as the reactor countdown.
// Runs on every machine, not just the spawner, so the whole game hears 3-2-1.
void arcade_do_announcement_frame(void)
{
	int secs_left;

	if (!(Game_mode & GM_ARCADE) || Arcade_announce_spid < 0) {
		Arcade_last_countdown_sound = -1;
		return;
	}

	secs_left = arcade_announce_seconds_left();

	if (secs_left != Arcade_last_countdown_sound && secs_left <= 9) {
		digi_play_sample(SOUND_COUNTDOWN_0_SECS + secs_left, F3_0);
		Arcade_last_countdown_sound = secs_left;
	}
}

//	returns true if powerup consumed
int do_powerup(object *obj)
{

	int used=0;
	int vulcan_ammo_to_add_with_cannon;
	int only_sound=0;


	if ((Player_is_dead) || (ConsoleObject->type == OBJ_GHOST) || (Players[Player_num].shields < 0))
		return 0;

	//if (Game_mode & GM_MULTI)
	//{
		/*
		 * The fact: Collecting a powerup is decided Client-side and due to PING it takes time for other players to know if one collected a powerup actually. This may lead to the case two players collect the same powerup!
		 * The solution: Let us check if someone else is closer to a powerup and if so, do not collect it.
		 * NOTE: Player positions computed by 'shortpos' and PING can still cause a small margin of error.
		 */

		 // CED -- causes more problems than it solves.  
		 /*
		int i = 0;
		vms_vector tvec;
		fix mydist = vm_vec_normalized_dir(&tvec, &obj->pos, &ConsoleObject->pos);

		for (i = 0; i < MAX_PLAYERS; i++)
		{
			if (i == Player_num || Players[i].connected != CONNECT_PLAYING)
				continue;
			if (Objects[Players[i].objnum].type == OBJ_GHOST || Players[i].shields < 0)
				continue;
			if (mydist > vm_vec_normalized_dir(&tvec, &obj->pos, &Objects[Players[i].objnum].pos))
				return 0;
		}
		*/
	//}

	// SNG: Arcade mode - super powers are tagged powerups and bypass the normal
	// pickup rules entirely (including the secondary ammo caps).
	{
		int arcade_sp = arcade_object_superpower(obj);

		if (arcade_sp >= 0)
			return arcade_pick_up_superpower(obj, arcade_sp);
	}

	switch (obj->id) {
		case POW_EXTRA_LIFE:
			Players[Player_num].lives++;
#ifdef NETWORK
			// Survival drops these as rare robot loot and gives them a
			// meaning multiplayer otherwise doesn't have: one banked
			// self-revive, spent automatically on the next death instead of
			// going down for the wave. No-op in every other mode.
			survival_add_extra_life();
#endif
			powerup_basic(15, 15, 15, 0, TXT_EXTRA_LIFE);
			used=1;
			break;
		case POW_ENERGY:
			used = pick_up_energy();
			break;
		case POW_SHIELD_BOOST:
			if (Players[Player_num].shields < MAX_SHIELDS) {
				fix repair = 3*F1_0 + 3*F1_0*(NDL - Difficulty_level);
				if (Game_mode & GM_MULTI)
					multi_send_repair(repair, Players[Player_num].shields, OBJ_POWERUP);

				Players[Player_num].shields += repair;
				if (Players[Player_num].shields > MAX_SHIELDS)
					Players[Player_num].shields = MAX_SHIELDS;
				powerup_basic(0, 0, 15, SHIELD_SCORE, "%s %s %d",TXT_SHIELD,TXT_BOOSTED_TO,f2ir(Players[Player_num].shields));
				used=1;
			} else
				HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, TXT_MAXED_OUT,TXT_SHIELD);
			break;
		case POW_LASER:
			// SNG: Static Powerups - already collected this life, no-op (no sound, never destroyed)
			if ((Netgame.StaticPowerups || Netgame.StaticLasers) && (StaticPowerupsCollected & STATIC_COLLECTED_LASER)) {
				used = 0;
				break;
			}
			if (Players[Player_num].laser_level >= MAX_LASER_LEVEL)
			{
				Players[Player_num].laser_level = MAX_LASER_LEVEL;
				HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, TXT_MAXED_OUT,TXT_LASER);
			} else {
				if (Newdemo_state == ND_STATE_RECORDING)
					newdemo_record_laser_level(Players[Player_num].laser_level, Players[Player_num].laser_level + 1);
				Players[Player_num].laser_level++;
				powerup_basic(10, 0, 10, LASER_SCORE, "%s %s %d",TXT_LASER,TXT_BOOSTED_TO, Players[Player_num].laser_level+1);
				update_laser_weapon_info();
				pick_up_primary (LASER_INDEX);
					if (Netgame.CTF)
					{
						only_sound = used;
						used = 0;
					}
					else
					{
						used=1;
					}
			}

			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_energy();

			// SNG: Static Powerups - mark collected, play sound once, never destroy the object
			if ((Netgame.StaticPowerups || Netgame.StaticLasers) && used) {
				StaticPowerupsCollected |= STATIC_COLLECTED_LASER;
				only_sound = used;
				used = 0;
			}
			break;
		case POW_MISSILE_1:
			used=pick_up_secondary(CONCUSSION_INDEX,1);
			break;
		case POW_MISSILE_4:
			used=pick_up_secondary(CONCUSSION_INDEX,4);
			break;
		case POW_KEY_BLUE:
			blue_key_pos = obj->pos;
			blue_key_seg = obj->segnum;
			if (Netgame.CTF && !(Players[Player_num].flags & PLAYER_FLAGS_BLUE_KEY))
			{
				if (get_team(Player_num) == 0)
				{
					only_sound = used;
					break;
				}
				else 
				{
					only_sound = used;
					used = 1;
					PALETTE_FLASH_ADD(0, 15, 0);
					sprintf(Network_message, "has picked up the \x01\xD7\ blue \x01\x99\ Flag!");
					Network_message_reciever = 100;
					HUD_init_message(HM_MULTI, "\x01\xC0\The other team has been alerted!");
					digi_play_sample(SOUND_CONTROL_CENTER_WARNING_SIREN, F1_0);
					multi_send_play_sound(SOUND_CONTROL_CENTER_WARNING_SIREN, F1_0);
				}
			}

			if (Players[Player_num].flags & PLAYER_FLAGS_BLUE_KEY)
				break;
#ifdef NETWORK
			multi_send_play_sound(Powerup_info[obj->id].hit_sound, F1_0);
#endif
			digi_play_sample( Powerup_info[obj->id].hit_sound, F1_0 );
			Players[Player_num].flags |= PLAYER_FLAGS_BLUE_KEY;
			if ((Game_mode & GM_MULTI) && Netgame.CTF)
				multi_send_flags();
			if (!Netgame.CTF)
				powerup_basic(0, 0, 15, KEY_SCORE, "%s %s",TXT_BLUE,TXT_ACCESS_GRANTED);
			else if ((Game_mode & GM_MULTI) & !Netgame.CTF)
				used=0;
			else
				used=1;
			break;

		case POW_KEY_RED:
			red_key_pos = obj->pos;
			red_key_seg = obj->segnum;
			if (Netgame.CTF && !(Players[Player_num].flags & PLAYER_FLAGS_RED_KEY))
			{
				if (get_team(Player_num) == 1)
				{
					only_sound = used;
					break;
				}
				else
				{
					only_sound = used;
					used = 1;
					PALETTE_FLASH_ADD(0, 15, 0);
					sprintf(Network_message, "has picked up the \x01\xC0\ red \x01\x99\ Flag!");
					Network_message_reciever = 100;
					HUD_init_message(HM_MULTI, "\x01\xD7\The other team has been alerted!");
					digi_play_sample(SOUND_CONTROL_CENTER_WARNING_SIREN, F1_0);
					multi_send_play_sound(SOUND_CONTROL_CENTER_WARNING_SIREN, F1_0);
				}
			}
			if (Players[Player_num].flags & PLAYER_FLAGS_RED_KEY)
				break;

#ifdef NETWORK
			multi_send_play_sound(Powerup_info[obj->id].hit_sound, F1_0);
#endif
			digi_play_sample( Powerup_info[obj->id].hit_sound, F1_0 );
			Players[Player_num].flags |= PLAYER_FLAGS_RED_KEY;
			if ((Game_mode & GM_MULTI) && Netgame.CTF)
				multi_send_flags();
			if(!Netgame.CTF)
				powerup_basic(15, 0, 0, KEY_SCORE, "%s %s",TXT_RED,TXT_ACCESS_GRANTED);
			else if ((Game_mode & GM_MULTI) & !Netgame.CTF)
				used=0;
			else
				used=1;
			break;

		case POW_KEY_GOLD:
			if (Players[Player_num].flags & PLAYER_FLAGS_GOLD_KEY)
				break;
#ifdef NETWORK
			multi_send_play_sound(Powerup_info[obj->id].hit_sound, F1_0);
#endif
			digi_play_sample( Powerup_info[obj->id].hit_sound, F1_0 );
			Players[Player_num].flags |= PLAYER_FLAGS_GOLD_KEY;
			powerup_basic(15, 15, 7, KEY_SCORE, "%s %s",TXT_YELLOW,TXT_ACCESS_GRANTED);
			if (Game_mode & GM_MULTI)
				used=0;
			else
				used=1;

		case POW_QUAD_FIRE:
			if (!(Players[Player_num].flags & PLAYER_FLAGS_QUAD_LASERS)) {
				Players[Player_num].flags |= PLAYER_FLAGS_QUAD_LASERS;
				powerup_basic(15, 15, 7, QUAD_FIRE_SCORE, "%s!",TXT_QUAD_LASERS);
				update_laser_weapon_info();
				pick_up_quads();
				used=1;
			} else
				HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, "%s %s!",TXT_ALREADY_HAVE,TXT_QUAD_LASERS);
			if (Netgame.CTF)
			{
				only_sound = used;
				used = 0;
			}

			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_energy();
			break;
		case	POW_VULCAN_WEAPON:
			// SNG: Static Powerups - already collected this life, no-op (no sound, never destroyed)
			if ((Netgame.StaticPowerups || Netgame.StaticVulcan) && (StaticPowerupsCollected & STATIC_COLLECTED_VULCAN)) {
				used = 0;
				break;
			}
			if ((used = pick_up_primary(VULCAN_INDEX)) != 0)
			{
				vulcan_ammo_to_add_with_cannon = obj->ctype.powerup_info.count;
				if (vulcan_ammo_to_add_with_cannon < VULCAN_WEAPON_AMMO_AMOUNT) vulcan_ammo_to_add_with_cannon = VULCAN_WEAPON_AMMO_AMOUNT;
				if ( (Game_mode & GM_MULTI) &&
					 (!(Game_mode & GM_MULTI_COOP)) &&
					 Netgame.LowVulcan &&
					 vulcan_ammo_to_add_with_cannon > VULCAN_WEAPON_AMMO_AMOUNT/2) 
				{
					vulcan_ammo_to_add_with_cannon = VULCAN_WEAPON_AMMO_AMOUNT/2;
				}
				pick_up_ammo(CLASS_PRIMARY, VULCAN_INDEX, vulcan_ammo_to_add_with_cannon);
			}

			if (Netgame.CTF)
			{
				only_sound = used;
				used = 0;
			}
//added/edited 8/3/98 by Victor Rachels to fix vulcan multi bug
//check if multi, if so, pick up ammo w/o using, set ammo left. else, normal

//killed 8/27/98 by Victor Rachels to fix vulcan ammo multiplying.  new way
// is by spewing the current held ammo when dead.
//-killed                        if (!used && (Game_mode & GM_MULTI))
//-killed                        {
//-killed                         int tempcount;                          
//-killed                           tempcount=Players[Player_num].primary_ammo[VULCAN_INDEX];
//-killed                            if (pick_up_ammo(CLASS_PRIMARY, VULCAN_INDEX, obj->ctype.powerup_info.count))
//-killed                             obj->ctype.powerup_info.count -= Players[Player_num].primary_ammo[VULCAN_INDEX]-tempcount;
//-killed                        }
//end kill - Victor Rachels

			if (!used && !(Game_mode & GM_MULTI) )
//end addition/edit - Victor Rachels
				used = pick_up_vulcan_ammo();

			// SNG: Static Powerups - mark collected, play sound once, never destroy the object
			if ((Netgame.StaticPowerups || Netgame.StaticVulcan) && used) {
				StaticPowerupsCollected |= STATIC_COLLECTED_VULCAN;
				only_sound = used;
				used = 0;
			}
			break;
		case	POW_SPREADFIRE_WEAPON:
			// SNG: Static Powerups - already collected this life, no-op (no sound, never destroyed)
			if ((Netgame.StaticPowerups || Netgame.StaticSpread) && (StaticPowerupsCollected & STATIC_COLLECTED_SPREAD)) {
				used = 0;
				break;
			}
			used = pick_up_primary(SPREADFIRE_INDEX);
			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_energy();
			if (Netgame.CTF)
			{
				only_sound = used;
				used = 0;
			}
			// SNG: Static Powerups - mark collected, play sound once, never destroy the object
			if ((Netgame.StaticPowerups || Netgame.StaticSpread) && used) {
				StaticPowerupsCollected |= STATIC_COLLECTED_SPREAD;
				only_sound = used;
				used = 0;
			}
			break;
		case	POW_PLASMA_WEAPON:
			// SNG: Static Powerups - already collected this life, no-op (no sound, never destroyed)
			if ((Netgame.StaticPowerups || Netgame.StaticPlasma) && (StaticPowerupsCollected & STATIC_COLLECTED_PLASMA)) {
				used = 0;
				break;
			}
			used = pick_up_primary(PLASMA_INDEX);
			if (Netgame.CTF)
			{
				only_sound = used;
				used = 0;
			}
			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_energy();
			// SNG: Static Powerups - mark collected, play sound once, never destroy the object
			if ((Netgame.StaticPowerups || Netgame.StaticPlasma) && used) {
				StaticPowerupsCollected |= STATIC_COLLECTED_PLASMA;
				only_sound = used;
				used = 0;
			}
			break;
		case	POW_FUSION_WEAPON:
			// SNG: Static Powerups - already collected this life, no-op (no sound, never destroyed)
			if ((Netgame.StaticPowerups || Netgame.StaticFusion) && (StaticPowerupsCollected & STATIC_COLLECTED_FUSION)) {
				used = 0;
				break;
			}
			used = pick_up_primary(FUSION_INDEX);
			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_energy();
			if (Netgame.CTF)
			{
				only_sound = used;
				used = 0;
			}
			// SNG: Static Powerups - mark collected, play sound once, never destroy the object
			if ((Netgame.StaticPowerups || Netgame.StaticFusion) && used) {
				StaticPowerupsCollected |= STATIC_COLLECTED_FUSION;
				only_sound = used;
				used = 0;
			}
			break;

		case	POW_PROXIMITY_WEAPON:
			used=pick_up_secondary(PROXIMITY_INDEX,4);
			break;
		case	POW_SMARTBOMB_WEAPON:
			used=pick_up_secondary(SMART_INDEX,1);
			break;
		case	POW_MEGA_WEAPON:
			used=pick_up_secondary(MEGA_INDEX,1);
			break;
		case	POW_VULCAN_AMMO: {
			used = pick_up_vulcan_ammo();
			if (!used && !(Game_mode & GM_MULTI) )
				used = pick_up_vulcan_ammo();
			break;
		}
			break;
		case	POW_HOMING_AMMO_1:
			used=pick_up_secondary(HOMING_INDEX,1);
			break;
		case	POW_HOMING_AMMO_4:
			used=pick_up_secondary(HOMING_INDEX,4);
			break;
		case	POW_CLOAK:
			if (Players[Player_num].flags & PLAYER_FLAGS_CLOAKED) {
				HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, "%s %s!",TXT_ALREADY_ARE,TXT_CLOAKED);
				break;
			} else {
				Players[Player_num].cloak_time = GameTime64;
				Players[Player_num].flags |= PLAYER_FLAGS_CLOAKED;
				ai_do_cloak_stuff();
				#ifdef NETWORK
				if (Game_mode & GM_MULTI)
					multi_send_cloak();
				// SNG: restock the mine as soon as this one is taken, rather than
				// waiting for whoever picked it up to let it run out. Arcade mode
				// reuses this same flag/timer for its own super power and has its
				// own scheduled spawner, so it's excluded here.
				if (!(Game_mode & GM_ARCADE))
					maybe_drop_net_powerup(POW_CLOAK);
				#endif
				powerup_basic(-10,-10,-10, CLOAK_SCORE, "%s!",TXT_CLOAKING_DEVICE);
				used = 1;
				break;
			}
		case	POW_INVULNERABILITY:
			if (Players[Player_num].flags & PLAYER_FLAGS_INVULNERABLE) {
				HUD_init_message(HM_DEFAULT|HM_REDUNDANT|HM_MAYDUPL, "%s %s!",TXT_ALREADY_ARE,TXT_INVULNERABLE);
				break;
			} else {
				Players[Player_num].invulnerable_time = GameTime64;
				Players[Player_num].flags |= PLAYER_FLAGS_INVULNERABLE;
				powerup_basic(7, 14, 21, INVULNERABILITY_SCORE, "%s!",TXT_INVULNERABILITY);
				// SNG: see POW_CLOAK above - restock on pickup, not on expiry.
				#ifdef NETWORK
				if (!(Game_mode & GM_ARCADE))
					maybe_drop_net_powerup(POW_INVULNERABILITY);
				#endif
				used = 1;
				break;
			}
	#ifndef RELEASE
		case	POW_MEGAWOW:
			do_megawow_powerup(50);
			used = 1;
			break;
	#endif

		default:
			break;
	}
		



//always say used, until physics problem (getting stuck on unused powerup)
//is solved.  Note also the break statements above that are commented out
//!!	used=1;

		if ((used || only_sound) && Powerup_info[obj->id].hit_sound > -1) {
		#ifdef NETWORK
		if (Game_mode & GM_MULTI) // Added by Rob, take this out if it turns out to be not good for net games!
			multi_send_play_sound(Powerup_info[obj->id].hit_sound, F1_0);
		#endif
		digi_play_sample( Powerup_info[obj->id].hit_sound, F1_0 );
		}

	return used;


}

/*
 * reads n powerup_type_info structs from a PHYSFS_file
 */
int powerup_type_info_read_n(powerup_type_info *pti, int n, PHYSFS_file *fp)
{
	int i;

	for (i = 0; i < n; i++) {
		pti[i].vclip_num = PHYSFSX_readInt(fp);
		pti[i].hit_sound = PHYSFSX_readInt(fp);
		pti[i].size = PHYSFSX_readFix(fp);
		pti[i].light = PHYSFSX_readFix(fp);
	}
	return i;
}
