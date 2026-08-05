/*
 * Survival game mode: endless waves of robots, everyone on one cooperative
 * team (no friendly fire, same as Coop), boss wave every 10th wave, and a
 * scavenging economy -- the level is stripped of every author-placed powerup
 * (survival_strip_level_powerups()) and robot drops are the only source of
 * weapons, shields and ammo for the whole match.
 *
 * Dying puts you out for the remainder of the current wave only: you spectate
 * (invisible and untouchable, see survival_revive_all()) and come back when
 * the surviving team clears that wave. The match ends only on a full team
 * wipe inside a single wave.
 *
 * Robots behave like a zombie horde -- they see the player through walls, via
 * a single narrow override in player_is_visible_from_object() (ai.c). That is
 * deliberately the ONLY AI change this mode makes; see the warning above
 * survival_limit_robot_speeds() before touching robot behaviour again.
 *
 * DESIGN NOTES
 *
 * Dynamic robot spawning does not exist anywhere else in this engine --
 * matcens (fuelcen.c) only replay whatever a level's author placed, and
 * nothing picks a robot type at runtime. This module is modeled closely on
 * "Arcade mode"'s periodic superpower dropper (multi_arcade_do_frame() and
 * friends in multi.c): one spawner-authority machine (normally the host)
 * owns all spawn decisions and tells everyone else via network packets;
 * every other machine just reacts to those packets. See multi_send_data()
 * callers below for the exact wire format of each new packet type
 * (MULTI_SURVIVAL_WAVE_STATE / _SPAWN_ROBOT / _ELIMINATED, multi.h).
 *
 * Spectating while down is NOT the engine's real Observer role. That role
 * (GM_OBSERVER / OBSERVER_PLAYER_ID) is a single shared global slot with
 * its own separate player roster (Netgame.observers[]) -- it cannot
 * represent several simultaneously-dead Survival players who each still
 * need their own identity, kills, and stats. Instead, a downed player gets
 * a normal respawn (safe, reuses tested code, keeps their object valid for
 * the rest of the engine's many `Objects[Players[pnum].objnum]` assumptions)
 * and is then flagged invulnerable and blocked from firing (see
 * survival_is_eliminated() call sites in game.c), while every *other*
 * machine turns them into an OBJ_GHOST so nobody can see them. They fly
 * around and watch, unable to fight, be hurt, or be seen -- the same
 * experience as spectating, built out of existing, safe primitives rather
 * than the incompatible built-in Observer system.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "pstypes.h"
#include "multi.h"
#include "net_udp.h"
#include "game.h"
#include "gr.h"
#include "gamefont.h"
#include "hudmsg.h"
#include "player.h"
#include "object.h"
#include "robot.h"
#include "multibot.h"
#include "powerup.h"
#include "fuelcen.h"
#include "fireball.h"
#include "endlevel.h"
#include "cntrlcen.h"
#include "morph.h"
#include "gameseg.h"
#include "vclip.h"
#include "timer.h"
#include "dxxerror.h"
#include "byteswap.h"
#include "digi.h"
#include "sounds.h"
#include "newdemo.h"
#include "survival.h"

extern int choose_drop_segment(void);
extern void init_player_stats_new_ship(ubyte pnum);

#define SURVIVAL_WAVE_ROBOTS_BASE     3
#define SURVIVAL_WAVE_ROBOTS_MAX      14
#define SURVIVAL_BOSS_WAVE_INTERVAL   10
#define SURVIVAL_SPAWN_TICK           (F1_0 * 3 / 2)   // 1.5s between individual robot spawns within a wave
#define SURVIVAL_INTER_WAVE_DELAY     (F1_0 * 6)        // rest period once a wave's robots are all cleared
#define SURVIVAL_FIRST_WAVE_DELAY     (F1_0 * 10)       // grace period before wave 1 (covers the countdown below)
#define SURVIVAL_COUNTDOWN_FROM        5                // "5..4..3..2..1..GO" before the first wave
#define SURVIVAL_AMMO_INTERVAL        (F1_0 * 12)
#define SURVIVAL_MAX_ACTIVE_ROBOTS    24
#define SURVIVAL_BANNER_DURATION      (F1_0 * 3)
#define SURVIVAL_SPAWN_SEG_HISTORY    6
#define SURVIVAL_ROBOT_SPEED_SCALE    (F1_0 * 3 / 5)   // damp AI-driven robot velocity each frame -- robots hunt slowly, players do the seeking

static int Survival_wave = 0;
static int Survival_wave_is_boss = 0;
static int Survival_wave_in_progress = 0;
static int Survival_robots_to_spawn = 0;
static fix64 Survival_next_spawn_at = 0;
static fix64 Survival_next_wave_at = 0;
static fix64 Survival_next_ammo_at = 0;
static int Survival_game_over = 0;

static ubyte Survival_eliminated[MAX_PLAYERS];

// Tracks currently-alive boss robots for the HP bar gauges.c draws under
// them (survival_get_active_bosses() below). Populated on every machine --
// spawner and receivers alike -- from the same is_boss byte on the
// MULTI_SURVIVAL_SPAWN_ROBOT wire packet, so the bar shows consistently for
// everyone, not just whoever's hosting.
#define SURVIVAL_MAX_TRACKED_BOSSES 8
typedef struct {
	short objnum; // -1 = empty slot
	fix max_shields;
} survival_boss_track;
static survival_boss_track Survival_bosses[SURVIVAL_MAX_TRACKED_BOSSES];

static short Survival_recent_spawn_segs[SURVIVAL_SPAWN_SEG_HISTORY];
static int Survival_recent_spawn_seg_idx = 0;

static char Survival_banner_text[48];
static fix64 Survival_banner_until = 0;
static int Survival_hud_prev_wave = -1;
static int Survival_hud_prev_in_progress = -1;

// Separate from the HUD's transition tracker above: that one lives in the
// render path, and reviving downed players is game state, not drawing. Both
// key off Survival_wave_in_progress, which is synced to every machine (see
// multi_do_survival_wave_state), so each machine independently reaches the
// same revive decision on the same wave -- no extra packet needed.
static int Survival_revive_prev_in_progress = -1;

// Last countdown tick we voiced before wave 1, so each number is spoken once.
// -1 = nothing said yet. Purely local: every machine sets its own
// Survival_next_wave_at in survival_start() when it enters the level, so all
// of them tick through the same numbers without needing a countdown packet.
static int Survival_countdown_last_spoken = -1;

// All candidates sorted ascending by base strength (shields), built lazily
// once per match from whatever robot set the loaded mission has -- there's
// no existing "difficulty tier" metadata per robot type to draw on, so this
// is the closest cheap proxy: early waves only draw from the weakest few,
// the eligible pool widens as the wave number climbs, and "boss" waves
// (see SURVIVAL_ELITE_POOL_FRACTION below) draw from the toughest slice of
// this same list instead of needing a dedicated boss robot type.
static int Survival_candidates[MAX_ROBOT_TYPES];
static int Survival_num_candidates = 0;
static int Survival_tables_built = 0;

// Subset of Survival_candidates whose Robot_names[] contains "hulk" --
// preferred pool for boss waves, see survival_pick_robot_type().
static int Survival_hulk_candidates[MAX_ROBOT_TYPES];
static int Survival_num_hulk_candidates = 0;

// Sustain drops: the stuff that keeps you alive rather than arming you.
// Rolled completely independently of the weapon table below, so a dry spell
// on weapons never also starves you of shields/energy.
static const int Survival_ammo_types[] = { POW_ENERGY, POW_SHIELD_BOOST, POW_SHIELD_BOOST, POW_VULCAN_AMMO };
#define SURVIVAL_NUM_AMMO_TYPES (sizeof(Survival_ammo_types) / sizeof(Survival_ammo_types[0]))

// Everything a robot can arm you with. Since Survival strips the level of
// its own powerups (see survival_strip_level_powerups()), this table is the
// *only* way to obtain weapons all match -- so it has to span the full set,
// primaries and secondaries alike. Weighted by repetition: the workhorse
// pickups appear more than once, the match-winners appear once.
static const int Survival_weapon_types[] = {
	// Primaries
	POW_LASER, POW_LASER,
	POW_VULCAN_WEAPON,
	POW_SPREADFIRE_WEAPON,
	POW_PLASMA_WEAPON,
	POW_FUSION_WEAPON,
	POW_QUAD_FIRE,
	// Secondaries
	POW_MISSILE_1, POW_MISSILE_1,
	POW_MISSILE_4,
	POW_HOMING_AMMO_1,
	POW_HOMING_AMMO_4,
	POW_PROXIMITY_WEAPON,
	POW_SMARTBOMB_WEAPON,
	POW_MEGA_WEAPON,
	// Situational
	POW_CLOAK,
	POW_INVULNERABILITY,
};
#define SURVIVAL_NUM_WEAPON_TYPES (sizeof(Survival_weapon_types) / sizeof(Survival_weapon_types[0]))

// Same election as multi_arcade_spawner_pnum() (multi.c): normally the
// host, falling back to the lowest connected player if the host itself is
// an observer (hosts-as-observer has no player object to anchor spawns to).
static int survival_spawner_pnum(void)
{
	int i;

	if (!Netgame.host_is_obs && Players[multi_who_is_master()].connected == CONNECT_PLAYING)
		return multi_who_is_master();

	for (i = 0; i < N_players; i++)
		if (i != multi_who_is_master() && Players[i].connected == CONNECT_PLAYING)
			return i;

	return -1;
}

static int survival_count_active_robots(void)
{
	int i, count = 0;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_ROBOT && !(Objects[i].flags & OF_SHOULD_BE_DEAD))
			count++;

	return count;
}

// Case-insensitive substring test -- no strcasestr() dependency (not
// portable to the Windows build).
static int survival_name_contains_ci(const char *haystack, const char *needle)
{
	int hlen = (int)strlen(haystack);
	int nlen = (int)strlen(needle);
	int i, j;

	for (i = 0; i + nlen <= hlen; i++)
	{
		for (j = 0; j < nlen; j++)
			if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
				break;
		if (j == nlen)
			return 1;
	}

	return 0;
}

static void survival_build_robot_tables(void)
{
	int i, j, key;
	fix key_strength;

	if (Survival_tables_built)
		return;

	Survival_num_candidates = 0;

	for (i = 0; i < N_robot_types; i++)
	{
		// Exclude the level's real end-of-mine guardian(s): boss_flag robots
		// are scripted set-pieces (self-destruct sequence, oversized/
		// multi-part models built for their own dedicated boss arena, not
		// regular mine corridors) -- spawning one as a random mob is exactly
		// what made earlier "boss wave = pick a boss_flag robot" attempts
		// end up too big to move anywhere. Never eligible, boss wave or not.
		if (Robot_info[i].boss_flag)
			continue;

		Survival_candidates[Survival_num_candidates++] = i;
	}

	// Insertion sort by strength, ascending. N is tiny (<=30) so this
	// doesn't need to be anything cleverer.
	for (i = 1; i < Survival_num_candidates; i++)
	{
		key = Survival_candidates[i];
		key_strength = Robot_info[key].strength;
		j = i - 1;
		while (j >= 0 && Robot_info[Survival_candidates[j]].strength > key_strength)
		{
			Survival_candidates[j + 1] = Survival_candidates[j];
			j--;
		}
		Survival_candidates[j + 1] = key;
	}

	// Boss waves prefer this named-"hulk" subset if the loaded robot set has
	// one (regular corridor-sized heavies, not the mine's real end boss --
	// see the boss_flag exclusion above). Falls back to the general pool
	// below when empty, e.g. non-stock robot sets without "hulk" in any name.
	Survival_num_hulk_candidates = 0;
	for (i = 0; i < Survival_num_candidates; i++)
		if (survival_name_contains_ci(Robot_names[Survival_candidates[i]], "hulk"))
			Survival_hulk_candidates[Survival_num_hulk_candidates++] = Survival_candidates[i];

	Survival_tables_built = 1;
}

// Boss waves draw from Survival_hulk_candidates when the loaded robot set
// has any (regular waves never do -- hulks are a strict subset of the
// general pool by strength/toughness already, so reserving them for bosses
// keeps a boss actually feeling distinct). Otherwise falls back to the
// toughest slice of the general (already boss_flag-free) pool, same as
// before "hulk" identification existed. Still a completely normal robot
// object either way: normal model, normal size, normal AI.
#define SURVIVAL_ELITE_POOL_FRACTION 4   // top 1/SURVIVAL_ELITE_POOL_FRACTION of candidates, by strength

static int survival_pick_robot_type(int wave, int is_boss)
{
	int pool_size, index;

	survival_build_robot_tables();

	if (Survival_num_candidates == 0)
		return -1;

	if (is_boss)
	{
		if (Survival_num_hulk_candidates > 0)
			// Favor the toughest hulks: half the time just take the single
			// strongest one (list is unsorted by strength, so scan for it),
			// otherwise pick any hulk at random for some variety.
			if (d_rand() < 16384)
			{
				int strongest = Survival_hulk_candidates[0];
				int k;
				for (k = 1; k < Survival_num_hulk_candidates; k++)
					if (Robot_info[Survival_hulk_candidates[k]].strength > Robot_info[strongest].strength)
						strongest = Survival_hulk_candidates[k];
				return strongest;
			}
			else
				return Survival_hulk_candidates[(d_rand() * Survival_num_hulk_candidates) >> 15];

		{
			int elite_count = Survival_num_candidates / SURVIVAL_ELITE_POOL_FRACTION;
			if (elite_count < 1)
				elite_count = 1;
			index = Survival_num_candidates - 1 - ((d_rand() * elite_count) >> 15);
			return Survival_candidates[index];
		}
	}

	pool_size = 2 + wave / 2;
	if (pool_size > Survival_num_candidates)
		pool_size = Survival_num_candidates;

	index = (d_rand() * pool_size) >> 15;
	return Survival_candidates[index];
}

static int survival_robots_for_wave(int wave)
{
	int n = SURVIVAL_WAVE_ROBOTS_BASE + wave;
	return n > SURVIVAL_WAVE_ROBOTS_MAX ? SURVIVAL_WAVE_ROBOTS_MAX : n;
}

static void survival_track_boss(int objnum, fix max_shields)
{
	int i;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		if (Survival_bosses[i].objnum < 0)
		{
			Survival_bosses[i].objnum = (short)objnum;
			Survival_bosses[i].max_shields = max_shields;
			return;
		}
	// Table full -- more concurrent bosses than we ever actually spawn at
	// once. Not fatal: this one just won't get an HP bar.
}

// Prunes dead/gone entries and hands back objnums + remaining-shields
// fraction (0..F1_0) for whatever bosses are still alive, for gauges.c to
// draw a bar under. Safe to call every HUD frame.
int survival_get_active_bosses(short *out_objnums, fix *out_fracs, int max_out)
{
	int i, count = 0;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
	{
		int objnum = Survival_bosses[i].objnum;

		if (objnum < 0)
			continue;

		if (objnum > Highest_object_index || Objects[objnum].type != OBJ_ROBOT || (Objects[objnum].flags & OF_SHOULD_BE_DEAD))
		{
			Survival_bosses[i].objnum = -1;
			continue;
		}

		if (count < max_out)
		{
			out_objnums[count] = (short)objnum;
			out_fracs[count] = (Survival_bosses[i].max_shields > 0) ? fixdiv(Objects[objnum].shields, Survival_bosses[i].max_shields) : 0;
			count++;
		}
	}

	return count;
}

// Spawner only. Wire format: [type:1][pnum:1][objnum:2][segnum:2][robot_type:1][shields:4][pos:12][is_boss:1] = 24 bytes.
static void survival_send_spawn_robot(int objnum, int segnum, int robot_type, fix shields, vms_vector *pos, int is_boss)
{
#ifdef WORDS_BIGENDIAN
	vms_vector swapped_vec;
#endif
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_SPAWN_ROBOT;		count += 1;
	multibuf[count] = Player_num;				count += 1;
	PUT_INTEL_SHORT(multibuf + count, objnum);		count += 2;
	PUT_INTEL_SHORT(multibuf + count, segnum);		count += 2;
	multibuf[count] = (ubyte)robot_type;			count += 1;
	PUT_INTEL_INT(multibuf + count, shields);		count += 4;
#ifndef WORDS_BIGENDIAN
	memcpy(multibuf + count, pos, sizeof(vms_vector));	count += sizeof(vms_vector);
#else
	swapped_vec.x = (fix)INTEL_INT((int)pos->x);
	swapped_vec.y = (fix)INTEL_INT((int)pos->y);
	swapped_vec.z = (fix)INTEL_INT((int)pos->z);
	memcpy(multibuf + count, &swapped_vec, 12);		count += 12;
#endif
	multibuf[count] = (ubyte)(is_boss ? 1 : 0);		count += 1;

	multi_send_data(multibuf, count, 2);

	if (Network_send_objects && multi_objnum_is_past(objnum))
		Network_send_objnum = -1;
}

void multi_do_survival_spawn_robot(const ubyte *buf)
{
	int count = 1;
	int pnum, segnum, robot_type, is_boss;
	short objnum;
	fix shields;
	vms_vector pos;
	object *obj;

	if (Endlevel_sequence || Control_center_destroyed)
		return;

	pnum = buf[count++];
	objnum = GET_INTEL_SHORT(buf + count); count += 2;
	segnum = GET_INTEL_SHORT(buf + count); count += 2;
	robot_type = buf[count++];
	shields = (fix)GET_INTEL_INT(buf + count); count += 4;
	pos = *(vms_vector *)(buf + count); count += sizeof(vms_vector);
#ifdef WORDS_BIGENDIAN
	pos.x = (fix)SWAPINT((int)pos.x);
	pos.y = (fix)SWAPINT((int)pos.y);
	pos.z = (fix)SWAPINT((int)pos.z);
#endif
	is_boss = buf[count++];

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;
	if (segnum < 0 || segnum > Highest_segment_index)
		return;
	if (robot_type < 0 || robot_type >= N_robot_types)
		return;

	obj = create_morph_robot(&Segments[segnum], &pos, robot_type);
	if (!obj)
		return;

	obj->shields = shields;
	morph_start(obj);
	map_objnum_local_to_remote(obj - Objects, objnum, pnum);

	if (is_boss)
		survival_track_boss(obj - Objects, shields);
}

static int survival_segment_has_player(int segnum)
{
	int i;

	for (i = 0; i < N_players; i++)
		if (Players[i].connected != CONNECT_DISCONNECTED && Objects[Players[i].objnum].segnum == segnum)
			return 1;

	return 0;
}

static int survival_segment_recently_used(int segnum)
{
	int i;

	for (i = 0; i < SURVIVAL_SPAWN_SEG_HISTORY; i++)
		if (Survival_recent_spawn_segs[i] == segnum)
			return 1;

	return 0;
}

static void survival_remember_spawn_seg(int segnum)
{
	Survival_recent_spawn_segs[Survival_recent_spawn_seg_idx] = (short)segnum;
	Survival_recent_spawn_seg_idx = (Survival_recent_spawn_seg_idx + 1) % SURVIVAL_SPAWN_SEG_HISTORY;
}

// choose_drop_segment() reseeds d_rand() from timer_query() at the top of
// every call, so calling it again inside a tight retry loop (same frame,
// same millisecond) reseeds to the same value and returns the identical
// segment every time -- that made the original retry-loop version of this
// function a no-op, which is why robots kept landing in the same segment.
// Call it once for a starting point, then decorrelate by walking to a
// random connected neighbor (real level topology, so it's always a valid
// segment) using d_rand() directly -- it isn't reseeded again after
// choose_drop_segment()'s one call, so successive draws actually differ.
static int survival_choose_spawn_segment(void)
{
	int segnum;
	int attempt;

	segnum = choose_drop_segment();
	if (segnum < 0 || segnum > Highest_segment_index)
		return segnum;

	for (attempt = 0; attempt < 12 && (survival_segment_has_player(segnum) || survival_segment_recently_used(segnum)); attempt++)
	{
		int side = (d_rand() * MAX_SIDES_PER_SEGMENT) >> 15;
		int child = Segments[segnum].children[side];

		if (IS_CHILD(child))
			segnum = child;
	}

	return segnum;
}

// Spawner only. Picks a segment, a type, creates the robot locally, and
// tells everyone else. Difficulty scaling here is entirely on the spawned
// instance's shields (+10% per 5 waves, bosses +100% flat) -- there's no
// per-type toughness tier to scale off of, only quantity/pool-width (see
// survival_pick_robot_type) and this per-instance shield multiplier.
static void survival_spawn_one_robot(int wave, int is_boss)
{
	int segnum, type;
	vms_vector pos;
	object *obj;
	fix mult;

	segnum = survival_choose_spawn_segment();
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	type = survival_pick_robot_type(wave, is_boss);
	if (type < 0)
		return;

	pick_random_point_in_seg(&pos, segnum);

	obj = create_morph_robot(&Segments[segnum], &pos, type);
	if (!obj)
		return;

	mult = F1_0 + (wave / 5) * (F1_0 / 10);
	if (is_boss)
		mult += F1_0;
	obj->shields = fixmul(obj->shields, mult);

	morph_start(obj);

	survival_remember_spawn_seg(segnum);
	survival_send_spawn_robot(obj - Objects, segnum, type, obj->shields, &pos, is_boss);

	if (is_boss)
		survival_track_boss(obj - Objects, obj->shields);
}

// Runs on every machine, every frame: AI recomputes each robot's velocity
// towards its target and clamps it against Robot_info[].max_speed (see
// move_towards_vector() in ai.c) -- there's no per-object speed field to
// override instead, so we just damp the result afterward. This makes
// Survival's robots amble towards players rather than beeline at full
// robot-type speed, without touching the shared Robot_info tables that
// every other game mode also uses.
// A flat per-frame multiply here (the original version of this function)
// compounds: physics drag already erodes velocity every frame, and stacking
// a further unconditional 0.6x on top of that every frame crushed robots'
// velocity toward zero within a few frames instead of settling at a slower
// steady speed -- that's what made them appear completely frozen. A real
// cap (only trims speed that's already over the target, same clamp shape
// ai.c's own move_towards_vector() uses against Robot_info[].max_speed)
// is stable and doesn't fight the AI's own acceleration.
static void survival_limit_robot_speeds(void)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *obj = &Objects[i];
		fix cap, speed;

		if (obj->type != OBJ_ROBOT || (obj->flags & OF_SHOULD_BE_DEAD))
			continue;

		cap = fixmul(Robot_info[obj->id].max_speed[Difficulty_level], SURVIVAL_ROBOT_SPEED_SCALE);
		speed = vm_vec_mag_quick(&obj->mtype.phys_info.velocity);
		if (speed > cap && speed > 0)
			vm_vec_scale(&obj->mtype.phys_info.velocity, fixdiv(cap, speed));
	}
}

// NOTE FOR FUTURE WORK -- do not "improve" robot aggression by writing to
// Ai_local_info[] fields from here. A version of this file did exactly that
// (pinning player_awareness_type at PA_WEAPON_ROBOT_COLLISION every frame to
// stop robots losing interest) and it froze them completely: they neither
// moved nor fired.
//
// Why: ai.c's do_ai_frame() reacts to that awareness level by forcing
// ailp->mode = AIM_CHASE_OBJECT on *every* frame (see the "Make sure that if
// this guy got hit or bumped, then he's chasing player" block). That
// permanently clobbers AIM_FOLLOW_PATH, which is the mode create_path_to_player()
// puts a robot into to route around geometry -- so the stuck-recovery pathing
// that keeps Survival's wall-seeing robots from wedging never survives to the
// next frame. It also disables ai.c's own time-slicing and, in multiplayer,
// changes how ai_multiplayer_awareness()/multi_can_move_robot() arbitrate who
// may move each robot.
//
// The wall-piercing pursuit this mode wants is already handled entirely in
// player_is_visible_from_object() (ai.c), which reports the player as visible
// and in-view for Survival. ai.c then raises awareness by itself through its
// normal paths. That single, narrow read-only-style override is the *only*
// AI change Survival makes, and it should stay that way.

static void survival_send_wave_state(void)
{
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_WAVE_STATE;		count += 1;
	PUT_INTEL_SHORT(multibuf + count, Survival_wave);	count += 2;
	multibuf[count] = (ubyte)Survival_wave_is_boss;	count += 1;
	multibuf[count] = (ubyte)Survival_wave_in_progress;	count += 1;

	multi_send_data(multibuf, count, 2);
}

void multi_do_survival_wave_state(const ubyte *buf)
{
	int count = 1;

	Survival_wave = GET_INTEL_SHORT(buf + count); count += 2;
	Survival_wave_is_boss = buf[count]; count += 1;
	Survival_wave_in_progress = buf[count];
}

static void survival_start_wave(void)
{
	Survival_wave++;
	Survival_wave_is_boss = (Survival_wave % SURVIVAL_BOSS_WAVE_INTERVAL == 0);

	if (Survival_wave_is_boss)
		// One extra boss every 3 boss-waves: 1 at wave 10, 2 at wave 40, 3 at wave 70, ...
		Survival_robots_to_spawn = 1 + (Survival_wave / SURVIVAL_BOSS_WAVE_INTERVAL - 1) / 3;
	else
		Survival_robots_to_spawn = survival_robots_for_wave(Survival_wave);

	Survival_wave_in_progress = 1;
	Survival_next_spawn_at = GameTime64;

	survival_send_wave_state();
}

static void survival_spawn_ammo(void)
{
	int segnum, objnum, ammo_type;
	vms_vector pos;

	segnum = choose_drop_segment();
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	ammo_type = Survival_ammo_types[(d_rand() * SURVIVAL_NUM_AMMO_TYPES) >> 15];

	Net_create_loc = 0;
	objnum = call_object_create_egg(&Objects[Players[Player_num].objnum], 1, OBJ_POWERUP, ammo_type);
	if (objnum < 0)
		return;

	pick_random_point_in_seg(&pos, segnum);
	Objects[objnum].pos = pos;
	vm_vec_zero(&Objects[objnum].mtype.phys_info.velocity);
	obj_relink(objnum, segnum);

	multi_send_create_powerup(ammo_type, segnum, objnum, &pos);
}

int survival_random_ammo_type(void)
{
	return Survival_ammo_types[(d_rand() * SURVIVAL_NUM_AMMO_TYPES) >> 15];
}

int survival_random_weapon_type(void)
{
	return Survival_weapon_types[(d_rand() * SURVIVAL_NUM_WEAPON_TYPES) >> 15];
}

// Wipes every powerup the level author placed. Survival is meant to start
// you with nothing and make the mine itself barren -- everything you get
// comes off a robot (survival_robot_drops() below). Called from
// multi_prep_level(), i.e. after the level is loaded but before anyone is
// playing it, and it runs identically on every machine, so no syncing is
// needed: each one deletes the same set of objects from the same freshly
// loaded level.
void survival_strip_level_powerups(void)
{
	int i;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_POWERUP)
			obj_delete(i);
}

// Robot death drops, called from multi_drop_robot_powerups() in place of the
// stock contains_prob path. Two *independent* rolls, deliberately: a weapon
// roll and a sustain (shield/energy/ammo) roll. They don't share a budget,
// so a robot can drop both, either, or nothing, and a run of weapon drops
// never means you go without shields.
//
// Each drop is emitted as its own object_create_egg() + network send: the
// MULTI_CREATE_ROBOT_POWERUPS packet carries a single contains_type/id for
// however many objnums it lists, so batching two *different* powerup types
// into one packet would make every remote machine spawn two of whichever
// type happened to be set last.
void survival_robot_drops(object *del_obj)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	if ((d_rand() * 100) >> 15 < SURVIVAL_WEAPON_DROP_PCT)
	{
		Net_create_loc = 0;
		del_obj->contains_type = OBJ_POWERUP;
		del_obj->contains_id = survival_random_weapon_type();
		del_obj->contains_count = 1;
		d_srand(1245L);
		if (object_create_egg(del_obj) >= 0 && Net_create_loc > 0)
			multi_send_create_robot_powerups(del_obj);
	}

	if ((d_rand() * 100) >> 15 < SURVIVAL_SUPPLY_DROP_PCT)
	{
		Net_create_loc = 0;
		del_obj->contains_type = OBJ_POWERUP;
		del_obj->contains_id = survival_random_ammo_type();
		del_obj->contains_count = 1;
		d_srand(1245L);
		if (object_create_egg(del_obj) >= 0 && Net_create_loc > 0)
			multi_send_create_robot_powerups(del_obj);
	}
}

static void survival_send_eliminated(int pnum)
{
	multibuf[0] = MULTI_SURVIVAL_ELIMINATED;
	multibuf[1] = (ubyte)pnum;
	multi_send_data(multibuf, 2, 2);
}

// Every machine independently reaches the same conclusion once elimination
// state has propagated (same pattern as spawner-authority election above --
// deterministic function of synced state, not a separate "game over"
// broadcast), so each one shows the stats screen and leaves on its own.
static void survival_check_game_over(void)
{
	int i, any_alive = 0;

	if (Survival_game_over)
		return;

	for (i = 0; i < N_players; i++)
		if (Players[i].connected != CONNECT_DISCONNECTED && !Survival_eliminated[i])
			any_alive = 1;

	if (any_alive)
		return;

	Survival_game_over = 1;

	multi_endlevel_score();
#ifdef USE_UDP
	// Survival is only reachable through the UDP hosting menu -- this can
	// never actually run without USE_UDP, but guard it anyway so this file
	// still links cleanly in a build configured without UDP support.
	net_udp_leave_game();
#endif
	if (Game_wind)
		window_close(Game_wind);
}

void multi_do_survival_eliminated(const ubyte *buf)
{
	int pnum = buf[1];

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;
	if (Survival_eliminated[pnum])
		return;

	Survival_eliminated[pnum] = 1;

	// Hide the downed player from everyone else for as long as they're out:
	// OBJ_GHOST is the engine's existing "player object that still exists but
	// is invisible and collides with nothing" state (used for players who
	// haven't finished joining), which is exactly the spectator presentation
	// we want and costs nothing to maintain. multi_make_player_ghost() Int3()s
	// if handed the local player, hence the guard -- the local side of being
	// downed is handled in DoPlayerDead()/survival_is_eliminated() instead.
	if (pnum != Player_num)
		multi_make_player_ghost(pnum);

	survival_check_game_over();
}

// Everyone who went down during the wave that just ended comes back, provided
// the team actually cleared it. Runs independently on every machine off the
// synced wave state, so no revive packet is needed -- see
// Survival_revive_prev_in_progress.
static void survival_revive_all(void)
{
	int i;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		if (!Survival_eliminated[i])
			continue;

		Survival_eliminated[i] = 0;

		if (i == Player_num)
		{
			// We were flying around as an untouchable non-combatant (see
			// DoPlayerDead()); becoming a real player again is just undoing
			// that -- the ship object has been alive the whole time, so
			// there's no respawn/level-restart to redo here.
			Players[i].flags &= ~PLAYER_FLAGS_INVULNERABLE;
			init_player_stats_new_ship(i);
			HUD_init_message(HM_DEFAULT, "Wave cleared -- you're back in!");
		}
		else if (Players[i].connected != CONNECT_DISCONNECTED)
		{
			multi_make_ghost_player(i);
		}
	}
}

void survival_start(void)
{
	int i;

	memset(Survival_eliminated, 0, sizeof(Survival_eliminated));
	Survival_wave = 0;
	Survival_wave_is_boss = 0;
	Survival_wave_in_progress = 0;
	Survival_robots_to_spawn = 0;
	Survival_game_over = 0;
	Survival_next_wave_at = GameTime64 + SURVIVAL_FIRST_WAVE_DELAY;
	Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;
	Survival_tables_built = 0; // rebuild from whatever robot set this mission loaded

	for (i = 0; i < SURVIVAL_SPAWN_SEG_HISTORY; i++)
		Survival_recent_spawn_segs[i] = -1;
	Survival_recent_spawn_seg_idx = 0;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		Survival_bosses[i].objnum = -1;

	Survival_banner_until = 0;
	Survival_hud_prev_wave = -1;
	Survival_hud_prev_in_progress = -1;
	Survival_revive_prev_in_progress = -1;
	Survival_countdown_last_spoken = -1;
}

int survival_player_died(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return Survival_game_over;

	if (!Survival_eliminated[pnum])
	{
		Survival_eliminated[pnum] = 1;
		survival_send_eliminated(pnum);
		survival_check_game_over();
	}

	return Survival_game_over;
}

int survival_is_eliminated(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return 0;
	return Survival_eliminated[pnum];
}

// "5..4..3..2..1" spoken over the grace period before wave 1, using the
// stock reactor self-destruct countdown voice clips (SOUND_COUNTDOWN_0_SECS
// is the base of a contiguous 0..14 run, so "N seconds" is base + N). Runs
// on every machine off its own Survival_next_wave_at, which is set when that
// machine enters the level -- close enough to simultaneous that a shared
// countdown packet isn't worth the sync surface.
static void survival_do_countdown(void)
{
	fix64 remaining;
	int secs;

	if (Survival_wave != 0 || Survival_wave_in_progress)
		return; // only before the very first wave

	remaining = Survival_next_wave_at - GameTime64;
	if (remaining < 0)
		remaining = 0;

	secs = f2i(remaining) + 1;
	if (secs > SURVIVAL_COUNTDOWN_FROM)
		return; // still in the quiet part of the grace period

	if (secs == Survival_countdown_last_spoken)
		return;

	Survival_countdown_last_spoken = secs;

	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;

	if (secs > 0)
	{
		digi_play_sample(SOUND_COUNTDOWN_0_SECS + secs, F1_0);
		sprintf(Survival_banner_text, "%d", secs);
		Survival_banner_until = GameTime64 + F1_0;
	}
	else
	{
		digi_play_sample(SOUND_COUNTDOWN_0_SECS, F1_0);
		sprintf(Survival_banner_text, "SURVIVE!");
		Survival_banner_until = GameTime64 + SURVIVAL_BANNER_DURATION;
	}
}

void survival_do_frame(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;
	if (Survival_game_over)
		return;
	if (Endlevel_sequence || Control_center_destroyed)
		return;

	// Speed damping applies to every locally-simulated robot object on
	// every machine, not just the spawner -- unlike wave/spawn decisions
	// this isn't spawner-authority state, it just shapes local movement.
	// Deliberately NOT touching AI mode/awareness/state here anymore --
	// create_morph_robot() (used to spawn these) already calls
	// init_ai_object() with AIB_NORMAL and an initial path, i.e. the exact
	// same setup regular matcen-spawned robots get in stock multiplayer
	// (see fuelcen.c). An earlier version of this file also force-drove
	// the AI's mode/state fields every frame to stop robots idling; that
	// fought the AI's own state machine and made them stop moving and
	// firing entirely. Trust the stock AI; only the speed is adjusted.
	survival_limit_robot_speeds();
	survival_do_countdown();

	// Wave just ended and somebody survived it -- bring the downed players
	// back. Above the spawner-only return below on purpose: this has to run
	// on every machine, since each one owns its own player's revive.
	if (Survival_revive_prev_in_progress == 1 && !Survival_wave_in_progress)
		survival_revive_all();
	Survival_revive_prev_in_progress = Survival_wave_in_progress;

	if (is_observer() || Player_num != survival_spawner_pnum())
		return;

	if (GameTime64 >= Survival_next_ammo_at)
	{
		survival_spawn_ammo();
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;
	}

	if (!Survival_wave_in_progress)
	{
		if (GameTime64 >= Survival_next_wave_at)
			survival_start_wave();
		return;
	}

	if (Survival_robots_to_spawn > 0)
	{
		if (GameTime64 >= Survival_next_spawn_at)
		{
			if (survival_count_active_robots() < SURVIVAL_MAX_ACTIVE_ROBOTS)
			{
				survival_spawn_one_robot(Survival_wave, Survival_wave_is_boss);
				Survival_robots_to_spawn--;
			}
			Survival_next_spawn_at = GameTime64 + SURVIVAL_SPAWN_TICK;
		}
		return;
	}

	// This wave's robots have all been queued -- once the mine's clear,
	// start the rest period before the next wave.
	if (survival_count_active_robots() == 0)
	{
		Survival_wave_in_progress = 0;
		Survival_next_wave_at = GameTime64 + SURVIVAL_INTER_WAVE_DELAY;
		survival_send_wave_state();
	}
}

// Detects wave-start/wave-clear transitions purely from state that's now
// synced to every machine (Survival_wave / Survival_wave_is_boss /
// Survival_wave_in_progress -- see multi_do_survival_wave_state()), so the
// centered banner and the clear-sound both fire identically on every
// client's own render frame without needing a dedicated broadcast.
static void survival_update_hud_transitions(void)
{
	if (Survival_wave == Survival_hud_prev_wave && Survival_wave_in_progress == Survival_hud_prev_in_progress)
		return;

	if (Survival_wave_in_progress && Survival_wave != Survival_hud_prev_wave)
	{
		sprintf(Survival_banner_text, "WAVE %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS!" : "");
		Survival_banner_until = GameTime64 + SURVIVAL_BANNER_DURATION;
	}
	else if (!Survival_wave_in_progress && Survival_hud_prev_in_progress == 1)
	{
		int snd;

		if (Newdemo_state != ND_STATE_PLAYBACK && (snd = Powerup_info[POW_EXTRA_LIFE].hit_sound) > -1)
			digi_play_sample(snd, F1_0);
	}

	Survival_hud_prev_wave = Survival_wave;
	Survival_hud_prev_in_progress = Survival_wave_in_progress;
}

void survival_draw_hud(void)
{
	char buf[48];

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	survival_update_hud_transitions();

	gr_set_curfont(GAME_FONT);
	gr_set_fontcolor(BM_XRGB(0, 28, 31), -1);

	if (Survival_wave_in_progress)
		sprintf(buf, "Wave %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS" : "");
	else if (Survival_wave == 0)
		sprintf(buf, "Get ready...");
	else
		sprintf(buf, "Wave %d cleared", Survival_wave);

	gr_string(FSPACX(2), LINE_SPACING + FSPACY(1), buf);

	if (survival_is_eliminated(Player_num))
	{
		gr_set_fontcolor(BM_XRGB(31, 0, 0), -1);
		gr_string(FSPACX(2), LINE_SPACING * 2 + FSPACY(1),
			Survival_wave_in_progress ? "DOWN - Spectating (back next wave)" : "DOWN - Spectating");
	}

	if (GameTime64 < Survival_banner_until)
	{
		int w, h, aw;

		gr_set_curfont(MEDIUM1_FONT);
		gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
		gr_get_string_size(Survival_banner_text, &w, &h, &aw);
		gr_string((GWIDTH - w) / 2, GHEIGHT / 4, Survival_banner_text);
	}
}
