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
 * wipe inside a single wave. A rare POW_EXTRA_LIFE drop banks one instant
 * self-revive that is spent automatically instead of going down at all
 * (survival_add_extra_life() / survival_player_died()).
 *
 * Robots behave like a zombie horde -- they never lose track of the player
 * through geometry, via a single narrow override in
 * player_is_visible_from_object() (ai.c) that downgrades a blocked line of
 * sight to "visible but not lined up" instead of "can't see you". They still
 * have to get a real line of sight before they can fire. That is deliberately
 * the ONLY AI change this mode makes; see the warning above
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
#include "3d.h"
#include "maths.h"
#include "multi.h"
#include "net_udp.h"
#include "game.h"
#include "gr.h"
#include "gamefont.h"
#include "gauges.h"
#include "playsave.h"
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

// Defined below, next to the countdown that drives most of the splashes.
static void survival_banner(const char *text, fix64 duration, int sound);

#define SURVIVAL_WAVE_ROBOTS_BASE     3
#define SURVIVAL_WAVE_ROBOTS_MAX      14
#define SURVIVAL_BOSS_WAVE_INTERVAL   10
#define SURVIVAL_SPAWN_TICK           (F1_0 * 3 / 2)   // 1.5s between individual robot spawns within a wave
#define SURVIVAL_INTER_WAVE_DELAY     (F1_0 * 6)        // rest period once a wave's robots are all cleared
#define SURVIVAL_FIRST_WAVE_DELAY     (F1_0 * 10)       // grace period before wave 1 (covers the countdown below)
#define SURVIVAL_COUNTDOWN_FROM        5                // "5..4..3..2..1..GO" before the first wave
#define SURVIVAL_AMMO_INTERVAL        (F1_0 * 30)
#define SURVIVAL_MAX_ACTIVE_ROBOTS    24
#define SURVIVAL_BANNER_DURATION      (F1_0 * 3)

// Spawn placement. Descent segments run roughly 20-40 world units across, so
// ~60 units of separation is "a few rooms apart" and ~30 units from a player
// is far enough that a robot never materialises in somebody's face.
#define SURVIVAL_SPAWN_HISTORY         8
#define SURVIVAL_SPAWN_MIN_SEPARATION  (i2f(60))
#define SURVIVAL_SPAWN_MIN_PLAYER_DIST (i2f(30))
#define SURVIVAL_SPAWN_CANDIDATES      24
#define SURVIVAL_ROBOT_SPEED_SCALE    (F1_0 * 3 / 5)   // damp AI-driven robot velocity each frame -- robots hunt slowly, players do the seeking

// Kill feedback: a "+points" that floats up from where the robot died.
#define SURVIVAL_MAX_POPUPS           16
#define SURVIVAL_POPUP_LIFE           (F1_0 * 5 / 4)
#define SURVIVAL_POPUP_RISE_PIXELS    26

// Nearest-robot pointer: an arrowhead orbiting the reticle at this radius.
// Suppressed while the target is already on screen inside the radius, so it
// doesn't clutter the crosshair during a face-to-face fight.
#define SURVIVAL_ARROW_RADIUS         44
#define SURVIVAL_ARROW_LENGTH         10
#define SURVIVAL_ARROW_HALF_WIDTH     5

// Distance range the arrow's brightness maps across: at or inside NEAR it is
// at its darkest, at or beyond FAR at its lightest, linear in between.
#define SURVIVAL_ARROW_NEAR_DIST      (i2f(20))
#define SURVIVAL_ARROW_FAR_DIST       (i2f(150))
#define SURVIVAL_ARROW_DARKEST        (F1_0 * 2 / 5)

// Brightness of the darker same-hue accent that backs the arrow and the
// "+points" text, as a fraction of whatever the main color currently is.
#define SURVIVAL_ACCENT_SCALE         (F1_0 * 9 / 20)

// Every player starts the match holding one of these -- see survival_start().
#define SURVIVAL_STARTING_EXTRA_LIVES 1

// Sounds for the centered banners. The engine indexes sounds numerically and
// has no name lookup, so each of these is reached through whatever already
// refers to it in the stock sound set:
//   siren01  -- the reactor alarm, i.e. SOUND_CONTROL_CENTER_WARNING_SIREN
//   invulon  -- the invulnerability powerup's own pickup sound
//   mtrl01   -- the matcen materialize, i.e. the robot-morph vclip's sound
#define SURVIVAL_SND_SIREN     SOUND_CONTROL_CENTER_WARNING_SIREN
#define SURVIVAL_SND_GET_READY (Powerup_info[POW_INVULNERABILITY].hit_sound)
#define SURVIVAL_SND_REVIVE    (Vclip[VCLIP_MORPHING_ROBOT].sound_num)

// Pre-wave pause. Boss waves get the longer one because they also get the
// full spoken countdown (survival_do_countdown()).
#define SURVIVAL_BOSS_WAVE_DELAY      (F1_0 * 10)

// Surplus energy trickles back into the hull. Energy caps at 200 but there is
// only so much you can spend it on, whereas in a mode with no author-placed
// pickups (survival_strip_level_powerups()) shields are always scarce -- so
// anything above the halfway mark slowly becomes armour instead of sitting
// there wasted. Deliberately a trickle, not an exchange: it should reward
// hoarding energy over a wave, never act as an instant heal mid-fight.
#define SURVIVAL_ENERGY_SURPLUS_AT    (i2f(100))
#define SURVIVAL_ENERGY_DRAIN_PER_SEC (i2f(5))
#define SURVIVAL_ENERGY_SHIELD_RATIO  (F1_0 / 2)  // shields gained per unit of energy spent

// How often each machine broadcasts its own hull to everyone, for the mini
// health bars under teammates' names (show_HUD_names(), gauges.c). Stock
// multiplayer never tells ordinary clients anyone else's shields -- the
// MULTI_DAMAGE / MULTI_REPAIR packets go to the host only and are applied
// only by observers -- so a co-op mode that wants to show teammate health
// has to say so itself. 4 Hz of a 6-byte packet per player is nothing next
// to the position stream, and it self-corrects any missed update.
#define SURVIVAL_SHIELD_SYNC_INTERVAL (F1_0 / 4)

static int Survival_wave = 0;
static int Survival_wave_is_boss = 0;
static int Survival_wave_in_progress = 0;
static int Survival_robots_to_spawn = 0;
static fix64 Survival_next_spawn_at = 0;
static fix64 Survival_next_wave_at = 0;
static fix64 Survival_next_ammo_at = 0;

// The two deadlines above are absolute GameTime64 values, and GameTime64 is
// reset to 0 by StartNewLevel() -- which runs *after* survival_start(), since
// the mode is chosen (net_udp_set_game_mode) before the level is entered. So
// they cannot be computed in survival_start(): whatever GameTime64 was left
// over from a previous level would be baked into them, and after the reset
// the opening wave would be that far into the future. That is what made the
// match sometimes start on time (fresh launch, GameTime64 near 0) and
// sometimes take minutes or appear never to start at all (hosting again
// after a previous match). They're armed instead on the first frame that
// actually runs inside the level, when GameTime64 is meaningful.
static int Survival_timers_armed = 0;
static int Survival_game_over = 0;

// Banked self-revives held by the local player, from POW_EXTRA_LIFE drops.
// Purely local state -- survival_player_died() is only ever called for
// Player_num, and spending a life means this machine simply never sends the
// MULTI_SURVIVAL_ELIMINATED packet, so there is nothing for anyone else to
// track. (Players[].lives is deliberately not reused for this: nothing in
// multiplayer decrements it, and every other mode's HUD/endgame code reads
// it with its own meaning.)
static int Survival_extra_lives = 0;

// Fractional shield repair from surplus energy that hasn't been reported to
// observers yet -- see survival_convert_surplus_energy().
static fix Survival_repair_pending = 0;

static fix64 Survival_next_shield_send = 0;

// Kill feedback state, all purely local and purely cosmetic: each machine
// only ever records its own kills, so none of this is synced.
typedef struct {
	vms_vector pos;   // where the robot died
	int points;
	fix64 born;       // 0 = free slot
} survival_kill_popup;
static survival_kill_popup Survival_popups[SURVIVAL_MAX_POPUPS];
static int Survival_popup_idx = 0;

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

// Ring buffer of the last few spawn *points* (not just segments): new spawns
// are placed as far from these as the level allows, which is what keeps a
// wave from arriving as one clump. Positions rather than segnums because
// "different segment" is nowhere near far enough -- adjacent segments are
// touching.
static vms_vector Survival_recent_spawn_pos[SURVIVAL_SPAWN_HISTORY];
static int Survival_recent_spawn_used[SURVIVAL_SPAWN_HISTORY];
static int Survival_recent_spawn_idx = 0;

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

// Last countdown tick we voiced before the coming wave, so each number is
// spoken once. -1 = nothing said yet. Purely local: every machine sets its
// own Survival_next_wave_at when it sees the previous wave end (see
// survival_do_frame), so all of them tick through the same numbers without
// needing a countdown packet.
static int Survival_countdown_last_spoken = -1;

// Whether the "GET READY..." / "BOSS INCOMING" banner for the wave we're
// currently waiting on has already been shown. Cleared alongside the
// countdown at every wave transition.
static int Survival_prewave_announced = 0;

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

// True if this object is one of the boss-wave robots we're tracking. Used by
// the drop table to guarantee a boss's extra life; safe to call while the
// object is mid-explosion, since entries are only pruned by
// survival_get_active_bosses() on a later render frame.
static int survival_is_tracked_boss(int objnum)
{
	int i;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		if (Survival_bosses[i].objnum == objnum)
			return 1;

	return 0;
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

static void survival_remember_spawn_pos(vms_vector *pos)
{
	Survival_recent_spawn_pos[Survival_recent_spawn_idx] = *pos;
	Survival_recent_spawn_used[Survival_recent_spawn_idx] = 1;
	Survival_recent_spawn_idx = (Survival_recent_spawn_idx + 1) % SURVIVAL_SPAWN_HISTORY;
}

// Distance to the nearest of the last few spawn points -- the thing we're
// maximizing when placing a new one. Effectively infinite before anything
// has spawned, so the first robot of a match goes wherever it likes.
static fix survival_dist_to_recent_spawns(vms_vector *pos)
{
	int i;
	fix best = 0x7fffffff;

	for (i = 0; i < SURVIVAL_SPAWN_HISTORY; i++)
	{
		fix d;

		if (!Survival_recent_spawn_used[i])
			continue;

		d = vm_vec_dist_quick(pos, &Survival_recent_spawn_pos[i]);
		if (d < best)
			best = d;
	}

	return best;
}

static fix survival_dist_to_players(vms_vector *pos)
{
	int i;
	fix best = 0x7fffffff;

	for (i = 0; i < N_players; i++)
	{
		fix d;

		if (Players[i].connected == CONNECT_DISCONNECTED)
			continue;
		if (survival_is_eliminated(i))
			continue; // spectating, not a real presence in the mine

		d = vm_vec_dist_quick(pos, &Objects[Players[i].objnum].pos);
		if (d < best)
			best = d;
	}

	return best;
}

// choose_drop_segment() reseeds d_rand() from timer_query() at the top of
// every call, so calling it again inside a tight retry loop (same frame,
// same millisecond) reseeds to the same value and returns the identical
// segment every time -- that made the original retry-loop version of this
// function a no-op, which is why robots kept landing in the same segment.
// So it's called exactly once here, for a starting point, and every
// candidate is then generated by walking a random number of steps through
// real level topology from it with d_rand() directly (not reseeded again
// after that one call, so successive draws actually differ).
//
// Candidates are scored by how far they are from the last few spawn points
// and rejected outright if they're on top of a player; the best one wins,
// with an early exit as soon as one is comfortably separated. Rejecting only
// exact segment repeats (the previous version) wasn't nearly enough -- most
// of the segments reachable in a few steps are still within the same room,
// so waves kept arriving in a clump.
static int survival_choose_spawn_point(vms_vector *out_pos)
{
	int base, best_seg = -1, attempt;
	fix best_score = -1;
	vms_vector best_pos;

	base = choose_drop_segment();
	if (base < 0 || base > Highest_segment_index)
		return -1;

	for (attempt = 0; attempt < SURVIVAL_SPAWN_CANDIDATES; attempt++)
	{
		int seg = base;
		int steps = 3 + ((d_rand() * 10) >> 15);
		vms_vector pos;
		fix score;

		while (steps-- > 0)
		{
			int side = (d_rand() * MAX_SIDES_PER_SEGMENT) >> 15;
			int child = Segments[seg].children[side];

			if (IS_CHILD(child))
				seg = child;
		}

		if (Segments[seg].special == SEGMENT_IS_CONTROLCEN)
			continue;

		pick_random_point_in_seg(&pos, seg);

		if (survival_dist_to_players(&pos) < SURVIVAL_SPAWN_MIN_PLAYER_DIST)
			continue;

		score = survival_dist_to_recent_spawns(&pos);
		if (score > best_score)
		{
			best_score = score;
			best_seg = seg;
			best_pos = pos;
		}

		if (score >= SURVIVAL_SPAWN_MIN_SEPARATION)
			break; // far enough from the rest of the wave; stop looking
	}

	if (best_seg < 0)
	{
		// Tiny or heavily-occupied level where nothing cleared the player
		// proximity test -- spawn at the unfiltered starting point rather
		// than skipping this robot entirely.
		best_seg = base;
		pick_random_point_in_seg(&best_pos, base);
	}

	*out_pos = best_pos;
	return best_seg;
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

	segnum = survival_choose_spawn_point(&pos);
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	type = survival_pick_robot_type(wave, is_boss);
	if (type < 0)
		return;

	obj = create_morph_robot(&Segments[segnum], &pos, type);
	if (!obj)
		return;

	mult = F1_0 + (wave / 5) * (F1_0 / 10);
	if (is_boss)
		mult += F1_0;
	obj->shields = fixmul(obj->shields, mult);

	morph_start(obj);

	survival_remember_spawn_pos(&pos);
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

		// Scaled from the TOP difficulty's speed, not the netgame's own
		// Difficulty_level. Survival already sets its own pace with this
		// damping factor, and stacking it on top of the per-difficulty
		// speed table compounded: on Trainee, robots crawled slowly enough
		// that early waves could take minutes to reach anybody, which reads
		// as the mode being broken from the start. This mode scales through
		// wave size, robot pool and shields (survival_start_wave() /
		// survival_pick_robot_type()) -- movement stays difficulty-neutral.
		cap = fixmul(Robot_info[obj->id].max_speed[NDL - 1], SURVIVAL_ROBOT_SPEED_SCALE);
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
// (but never as lined-up-for-a-shot) through geometry for Survival. ai.c then
// raises awareness by itself through its normal paths. That single, narrow read-only-style override is the *only*
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

// Emits one powerup off a just-killed robot: its own object_create_egg() plus
// its own network send, because the MULTI_CREATE_ROBOT_POWERUPS packet carries
// a single contains_type/id for however many objnums it lists -- batching two
// *different* powerup types into one packet would make every remote machine
// spawn two of whichever type happened to be set last.
static void survival_drop_one(object *del_obj, int powerup_id)
{
	Net_create_loc = 0;
	del_obj->contains_type = OBJ_POWERUP;
	del_obj->contains_id = powerup_id;
	del_obj->contains_count = 1;
	d_srand(1245L);
	if (object_create_egg(del_obj) >= 0 && Net_create_loc > 0)
		multi_send_create_robot_powerups(del_obj);
}

// Robot death drops, called from multi_drop_robot_powerups() in place of the
// stock contains_prob path. Three *independent* rolls, deliberately: a weapon
// roll, a sustain (shield/energy/ammo) roll, and a rare extra-life roll. They
// don't share a budget, so a robot can drop any combination or nothing, and a
// run of weapon drops never means you go without shields.
//
// All three are rolled up front, before anything is dropped, and that ordering
// is load-bearing: dropping a powerup calls d_srand(1245L) (the engine's
// fixed seed that keeps egg drops identical on every machine) and then burns
// a known number of d_rand() calls inside drop_powerup(). Any roll made after
// a drop is therefore not random at all -- it reads a fixed point in a fixed
// sequence and comes out the same way on every kill. That is what made the
// supply drop effectively unconditional rather than the intended percentage.
void survival_robot_drops(object *del_obj)
{
	int drop_weapon, drop_supply, drop_extra_life;
	int weapon_id = -1, supply_id = -1;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	drop_weapon = ((d_rand() * 100) >> 15) < SURVIVAL_WEAPON_DROP_PCT;
	drop_supply = ((d_rand() * 100) >> 15) < SURVIVAL_SUPPLY_DROP_PCT;
	drop_extra_life = ((d_rand() * 1000) >> 15) < SURVIVAL_EXTRA_LIFE_DROP_PERMILLE;

	// A boss is the reward wave: killing one always pays out a life, on top
	// of whatever the ordinary rolls above came up with.
	if (survival_is_tracked_boss(del_obj - Objects))
		drop_extra_life = 1;

	// Which powerup, likewise chosen before any drop touches the RNG.
	if (drop_weapon)
		weapon_id = survival_random_weapon_type();
	if (drop_supply)
		supply_id = survival_random_ammo_type();

	if (drop_weapon)
		survival_drop_one(del_obj, weapon_id);

	if (drop_supply)
		survival_drop_one(del_obj, supply_id);

	// By far the rarest: a free revive. It survives the engine's usual
	// "extra lives are meaningless in multiplayer, turn them into
	// invulnerability" rewrites because those only fire for modes without
	// GM_MULTI_ROBOTS (drop_powerup(), fireball.c) or for powerups the level
	// author placed (multi_prep_level()) -- Survival is neither.
	if (drop_extra_life)
		survival_drop_one(del_obj, POW_EXTRA_LIFE);
}

void survival_add_extra_life(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	Survival_extra_lives++;

	survival_banner("EXTRA LIFE!", SURVIVAL_BANNER_DURATION, -1); // the pickup already has its own sound
}

int survival_extra_lives(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return 0;

	return Survival_extra_lives;
}

// Broadcast our own hull and energy so everyone can draw the bars under our
// name. Wire format: [type:1][pnum:1][shields:4][energy:4] = 10 bytes.
static void survival_send_shields(void)
{
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_SHIELDS;			count += 1;
	multibuf[count] = Player_num;					count += 1;
	PUT_INTEL_INT(multibuf + count, Players[Player_num].shields);	count += 4;
	PUT_INTEL_INT(multibuf + count, Players[Player_num].energy);	count += 4;

	multi_send_data(multibuf, count, 0);
}

void multi_do_survival_shields(const ubyte *buf)
{
	int pnum = buf[1];

	if (pnum < 0 || pnum >= MAX_PLAYERS || pnum == Player_num)
		return;

	// Observers deliberately excluded: they already reconstruct everyone's
	// shields from the MULTI_DAMAGE / MULTI_REPAIR stream, which also feeds
	// their damage-delta readouts. Letting this coarser 4 Hz sample
	// overwrite that would make those deltas jump.
	if (is_observer())
		return;

	Players[pnum].shields = (fix)GET_INTEL_INT(buf + 2);
	Players[pnum].energy = (fix)GET_INTEL_INT(buf + 6);
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
			survival_banner("BACK IN THE FIGHT!", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_REVIVE);
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

	// Everyone starts with a life in the bank, so a single early mistake
	// doesn't put a player out of the wave before the match has warmed up.
	Survival_extra_lives = SURVIVAL_STARTING_EXTRA_LIVES;

	memset(Survival_popups, 0, sizeof(Survival_popups));
	Survival_popup_idx = 0;
	Survival_repair_pending = 0;
	// Deliberately not computed here -- see Survival_timers_armed.
	Survival_timers_armed = 0;
	Survival_next_wave_at = 0;
	Survival_next_ammo_at = 0;
	Survival_tables_built = 0; // rebuild from whatever robot set this mission loaded

	for (i = 0; i < SURVIVAL_SPAWN_HISTORY; i++)
		Survival_recent_spawn_used[i] = 0;
	Survival_recent_spawn_idx = 0;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		Survival_bosses[i].objnum = -1;

	Survival_banner_until = 0;
	Survival_hud_prev_wave = -1;
	Survival_hud_prev_in_progress = -1;
	Survival_revive_prev_in_progress = -1;
	Survival_countdown_last_spoken = -1;
	Survival_prewave_announced = 0;
}

int survival_player_died(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return Survival_game_over;

	// Spend a banked extra life instead of going down. Nothing is sent: the
	// other machines never hear about this death at all, so from their side
	// this player simply kept playing (their respawned ship keeps streaming
	// position as usual). The caller still does the normal respawn, and
	// skips the spectator flagging because we're not eliminated.
	if (pnum == Player_num && Survival_extra_lives > 0)
	{
		Survival_extra_lives--;
		survival_banner("REVIVED!", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_REVIVE);
		HUD_init_message(HM_DEFAULT, "Extra life spent -- %d left", Survival_extra_lives);
		return Survival_game_over;
	}

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

// Single entry point for every centered splash this mode shows, so each one
// is paired with its event sound in exactly one place. sound < 0 means
// silent; demo playback never makes noise.
static void survival_banner(const char *text, fix64 duration, int sound)
{
	strncpy(Survival_banner_text, text, sizeof(Survival_banner_text) - 1);
	Survival_banner_text[sizeof(Survival_banner_text) - 1] = 0;
	Survival_banner_until = GameTime64 + duration;

	if (sound > -1 && Newdemo_state != ND_STATE_PLAYBACK)
		digi_play_sample(sound, F1_0);
}

// The wave that the current rest period is leading up to.
static int survival_next_wave_is_boss(void)
{
	return ((Survival_wave + 1) % SURVIVAL_BOSS_WAVE_INTERVAL) == 0;
}

static fix64 survival_prewave_delay(void)
{
	return survival_next_wave_is_boss() ? SURVIVAL_BOSS_WAVE_DELAY : SURVIVAL_INTER_WAVE_DELAY;
}

// "5..4..3..2..1..GO" spoken over the pause before the match's first wave and
// before every boss wave, using the stock reactor self-destruct countdown
// voice clips (SOUND_COUNTDOWN_0_SECS is the base of a contiguous 0..14 run,
// so "N seconds" is base + N). A boss countdown lays the reactor siren over
// each tick, so the alarm effectively loops for the whole approach.
//
// Runs on every machine off its own Survival_next_wave_at, which each one
// sets for itself when it sees the previous wave end -- close enough to
// simultaneous that a shared countdown packet isn't worth the sync surface.
static void survival_do_countdown(void)
{
	fix64 remaining;
	int secs, boss_next;

	if (Survival_wave_in_progress)
		return;

	boss_next = survival_next_wave_is_boss();

	// Only the opening wave and boss waves get the countdown treatment;
	// ordinary waves just roll straight on.
	if (Survival_wave != 0 && !boss_next)
		return;

	if (!Survival_prewave_announced)
	{
		Survival_prewave_announced = 1;

		if (Survival_wave == 0)
			survival_banner("GET READY...", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_GET_READY);
		else
			survival_banner("BOSS INCOMING", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_SIREN);
	}

	remaining = Survival_next_wave_at - GameTime64;
	if (remaining < 0)
		remaining = 0;

	// Ceiling, so the last tick lands on 0 ("GO") exactly as the delay runs
	// out rather than being skipped -- f2i(remaining)+1 could never reach 0.
	secs = (int)((remaining + F1_0 - 1) / F1_0);
	if (secs > SURVIVAL_COUNTDOWN_FROM)
		return; // still in the quiet part of the grace period

	if (secs == Survival_countdown_last_spoken)
		return;

	Survival_countdown_last_spoken = secs;

	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;

	if (secs > 0)
	{
		char buf[8];

		digi_play_sample(SOUND_COUNTDOWN_0_SECS + secs, F1_0);
		if (boss_next)
			digi_play_sample(SURVIVAL_SND_SIREN, F1_0);

		sprintf(buf, "%d", secs);
		survival_banner(buf, F1_0, -1);
	}
	else
	{
		// Zero: just the "GO" clip. No splash of its own -- the wave's own
		// "WAVE N" / "WAVE N - BOSS!" banner (which carries the siren for a
		// boss) lands on the very next frame and would overwrite it anyway.
		digi_play_sample(SOUND_COUNTDOWN_0_SECS, F1_0);
	}
}

// Bleeds energy above SURVIVAL_ENERGY_SURPLUS_AT into shields, a slice per
// frame. Local player only, and purely local state -- nobody else simulates
// our shields.
static void survival_convert_surplus_energy(void)
{
	fix surplus, room, drain, gain;

	if (is_observer() || survival_is_eliminated(Player_num))
		return;
	if (Player_is_dead || Players[Player_num].shields <= 0)
		return;

	surplus = Players[Player_num].energy - SURVIVAL_ENERGY_SURPLUS_AT;
	room = MAX_SHIELDS - Players[Player_num].shields;

	if (surplus <= 0 || room <= 0)
		return;

	drain = fixmul(SURVIVAL_ENERGY_DRAIN_PER_SEC, FrameTime);
	if (drain > surplus)
		drain = surplus;

	gain = fixmul(drain, SURVIVAL_ENERGY_SHIELD_RATIO);
	if (gain > room)
	{
		// Topping out this frame -- spend only the energy the last sliver of
		// hull actually costs, so the remainder stays in the tank.
		gain = room;
		drain = fixdiv(gain, SURVIVAL_ENERGY_SHIELD_RATIO);
	}

	if (drain <= 0 || gain <= 0)
		return;

	Players[Player_num].energy -= drain;
	Players[Player_num].shields += gain;

	// Observers reconstruct shields from MULTI_REPAIR events. A trickle this
	// fine would mean a packet every frame, so it's batched up and reported
	// a whole point at a time.
	Survival_repair_pending += gain;
	if (Survival_repair_pending >= F1_0)
	{
		multi_send_repair(Survival_repair_pending,
			Players[Player_num].shields - Survival_repair_pending, OBJ_POWERUP);
		Survival_repair_pending = 0;
	}
}

// Keeps a downed player cloaked for as long as they're spectating. Cloak
// expires on a timer (do_cloak_stuff() in game.c drops the flag once
// cloak_time + CLOAK_TIME_MAX is past), so holding it means pushing the
// timestamp forward every frame rather than setting the flag once.
//
// Being cloaked on top of being ghosted and invulnerable is what makes
// spectating genuinely inert: OBJ_GHOST hides us from other players, but
// robots run their AI against the real object on our own machine, and a
// cloaked player is one the AI's targeting deliberately loses track of.
static void survival_hold_spectator_cloak(void)
{
	if (is_observer())
		return;

	if (!survival_is_eliminated(Player_num))
		return;

	Players[Player_num].flags |= PLAYER_FLAGS_CLOAKED;
	Players[Player_num].cloak_time = GameTime64;
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

	// First frame actually inside the level: GameTime64 is now the level's
	// own clock (StartNewLevel() zeroed it after survival_start() ran), so
	// this is the earliest point the opening deadlines can be trusted.
	if (!Survival_timers_armed)
	{
		Survival_timers_armed = 1;
		Survival_next_wave_at = GameTime64 + SURVIVAL_FIRST_WAVE_DELAY;
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;
	}

	// Self-heal: a deadline further out than the longest pause we ever set
	// can only mean the game clock moved under us. Re-arm rather than sit
	// there waiting for a time that may never come.
	if (Survival_next_wave_at - GameTime64 > SURVIVAL_FIRST_WAVE_DELAY)
		Survival_next_wave_at = GameTime64 + survival_prewave_delay();
	if (Survival_next_ammo_at - GameTime64 > SURVIVAL_AMMO_INTERVAL)
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;

	survival_do_countdown();
	survival_convert_surplus_energy();
	survival_hold_spectator_cloak();

	if (Survival_next_shield_send - GameTime64 > SURVIVAL_SHIELD_SYNC_INTERVAL)
		Survival_next_shield_send = 0; // clock moved under us -- see Survival_timers_armed

	if (!is_observer() && GameTime64 >= Survival_next_shield_send)
	{
		survival_send_shields();
		Survival_next_shield_send = GameTime64 + SURVIVAL_SHIELD_SYNC_INTERVAL;
	}

	// Wave just ended and somebody survived it -- bring the downed players
	// back, and open the rest period. Above the spawner-only return below on
	// purpose: this has to run on every machine, since each one owns its own
	// player's revive and its own copy of the countdown. Every machine
	// derives the same delay from the same synced wave number, so the
	// spawner (which is the one that actually starts the next wave off
	// Survival_next_wave_at) stays in step with everyone's countdown without
	// a timing packet.
	if (Survival_revive_prev_in_progress == 1 && !Survival_wave_in_progress)
	{
		survival_revive_all();

		Survival_next_wave_at = GameTime64 + survival_prewave_delay();
		Survival_countdown_last_spoken = -1;
		Survival_prewave_announced = 0;
	}
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

	// This wave's robots have all been queued -- once the mine's clear, end
	// the wave. Survival_next_wave_at is deliberately NOT set here: the
	// transition block above sets it on every machine (this one included) on
	// the following frame, so there is only one place that decides how long
	// the rest period is.
	if (survival_count_active_robots() == 0)
	{
		Survival_wave_in_progress = 0;
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
		char buf[48];

		sprintf(buf, "WAVE %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS!" : "");
		survival_banner(buf, SURVIVAL_BANNER_DURATION, Survival_wave_is_boss ? SURVIVAL_SND_SIREN : -1);
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
	int row = 1; // stacked top-left, one LINE_SPACING apart

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	survival_update_hud_transitions();

	gr_set_curfont(GAME_FONT);
	gr_set_fontcolor(BM_XRGB(0, 28, 31), -1);

	// Nothing here before wave 1 -- that's the centered "GET READY..."
	// banner's job now (survival_do_countdown()).
	if (Survival_wave_in_progress)
	{
		sprintf(buf, "Wave %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS" : "");
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1), buf);
	}
	else if (Survival_wave > 0)
	{
		sprintf(buf, "Wave %d cleared", Survival_wave);
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1), buf);
	}

	// The extra-life count lives in the bottom-left gauge stack, next to
	// shields and energy -- see hud_show_survival_extra_lives() in gauges.c.

	if (survival_is_eliminated(Player_num))
	{
		gr_set_fontcolor(BM_XRGB(31, 0, 0), -1);
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1),
			Survival_wave_in_progress ? "DOWN - Spectating (back next wave)" : "DOWN - Spectating");
	}

	if (GameTime64 < Survival_banner_until)
	{
		int w, h, aw;
		

		gr_set_curfont(MEDIUM2_FONT);
		gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
		gr_get_string_size(Survival_banner_text, &w, &h, &aw);
		gr_string((GWIDTH - w) / 2, GHEIGHT / 4, Survival_banner_text);
	}
}

void survival_note_robot_kill(object *robot, int points)
{
	survival_kill_popup *p;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;
	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;

	if (points <= 0)
		return; // nothing worth floating a number for

	p = &Survival_popups[Survival_popup_idx];
	Survival_popup_idx = (Survival_popup_idx + 1) % SURVIVAL_MAX_POPUPS;

	p->pos = robot->pos;
	p->points = points;
	p->born = GameTime64;
}

// The crosshair's own configured color (show_reticle() in gauges.c reads the
// same PlayerCfg.ReticleRGBA), so every Survival overlay reads as part of the
// reticle rather than as a separate HUD element. Components are on BM_XRGB's
// 0..31 scale. A configured-black reticle falls back to the stock green, so
// the overlays can never render themselves invisible.
static void survival_reticle_rgb(int *r, int *g, int *b)
{
	int c[3], i;

	c[0] = PlayerCfg.ReticleRGBA[0];
	c[1] = PlayerCfg.ReticleRGBA[1];
	c[2] = PlayerCfg.ReticleRGBA[2];

	if (c[0] <= 0 && c[1] <= 0 && c[2] <= 0)
	{
		c[0] = RET_COLOR_DEFAULT_R;
		c[1] = RET_COLOR_DEFAULT_G;
		c[2] = RET_COLOR_DEFAULT_B;
	}

	for (i = 0; i < 3; i++)
	{
		if (c[i] < 0)
			c[i] = 0;
		if (c[i] > 31)
			c[i] = 31;
	}

	*r = c[0];
	*g = c[1];
	*b = c[2];
}

// Same hue, scaled brightness (scale is 0..F1_0). This is how both the
// darker accent and the arrow's distance shading stay on the reticle's color
// instead of introducing a second one.
static int survival_shade(int r, int g, int b, fix scale)
{
	if (scale < 0)
		scale = 0;
	if (scale > F1_0)
		scale = F1_0;

	return BM_XRGB(f2i(fixmul(i2f(r), scale)),
	               f2i(fixmul(i2f(g), scale)),
	               f2i(fixmul(i2f(b), scale)));
}

// "+points" rising from where each robot died. Anchored to the world point
// (projected every frame) rather than to the screen, so it stays over the
// kill as you keep flying -- the rise and the fade are what make it read as
// a score popup rather than a label.
static void survival_draw_kill_popups(void)
{
	int i, cr, cg, cb;

	gr_set_curfont(GAME_FONT);
	survival_reticle_rgb(&cr, &cg, &cb);

	for (i = 0; i < SURVIVAL_MAX_POPUPS; i++)
	{
		survival_kill_popup *p = &Survival_popups[i];
		g3s_point pt;
		fix age, life_frac, bright;
		char buf[16];
		int w, h, aw, rise, x, y;

		if (!p->born)
			continue;

		age = (fix)(GameTime64 - p->born);
		if (age < 0 || age >= SURVIVAL_POPUP_LIFE)
		{
			p->born = 0;
			continue;
		}

		g3_rotate_point(&pt, &p->pos);
		if (pt.p3_codes != 0) // outside the view frustum
			continue;

		g3_project_point(&pt);
		if (pt.p3_flags & PF_OVERFLOW)
			continue;

		life_frac = fixdiv(age, SURVIVAL_POPUP_LIFE); // 0..F1_0
		rise = f2i(fixmul(i2f(SURVIVAL_POPUP_RISE_PIXELS), life_frac));

		// Full reticle color at first, fading toward its dark end as the
		// popup rises. Never all the way to black, so a number that is
		// still on screen is still legible.
		bright = F1_0 - fixmul(F1_0 * 7 / 10, life_frac);

		sprintf(buf, "+%d", p->points);
		gr_get_string_size(buf, &w, &h, &aw);
		x = f2i(pt.p3_sx) - w / 2;
		y = f2i(pt.p3_sy) - rise;

		// Darker same-hue accent offset a pixel down-right, then the number
		// itself on top: keeps the text readable over bright textures
		// without introducing a color the reticle doesn't already use.
		gr_set_fontcolor(survival_shade(cr, cg, cb, fixmul(bright, SURVIVAL_ACCENT_SCALE)), -1);
		gr_string(x + 1, y + 1, buf);

		gr_set_fontcolor(survival_shade(cr, cg, cb, bright), -1);
		gr_string(x, y, buf);
	}
}

// Nearest live robot to the player, or -1. Straight-line distance, deliberately
// ignoring walls: the arrow is a "where is the horde" aid, matching the fact
// that the robots themselves see through walls in this mode.
static int survival_nearest_robot(fix *out_dist)
{
	int i, best = -1;
	fix best_dist = 0x7fffffff;
	vms_vector *from;

	if (Players[Player_num].objnum < 0)
		return -1;

	from = &Objects[Players[Player_num].objnum].pos;

	for (i = 0; i <= Highest_object_index; i++)
	{
		fix d;

		if (Objects[i].type != OBJ_ROBOT)
			continue;
		if (Objects[i].flags & (OF_SHOULD_BE_DEAD | OF_EXPLODING))
			continue;

		d = vm_vec_dist_quick(from, &Objects[i].pos);
		if (d < best_dist)
		{
			best_dist = d;
			best = i;
		}
	}

	if (best >= 0 && out_dist)
		*out_dist = best_dist;

	return best;
}

// Arrowhead orbiting the reticle, pointing the way you have to turn to face
// the nearest robot. Direction comes from the target's *view-space* position
// (g3_rotate_point's p3_x/p3_y, i.e. right/up relative to where you're
// looking) rather than its screen projection, because the projection is
// meaningless once the target is off screen or behind you -- which is
// precisely when the arrow matters.
//
// It is drawn entirely in the reticle's own color: brightness carries the
// range to the target -- dark when it's right on top of you, light when it's
// far off -- so distance reads at a glance without a second hue or a number.
static void survival_draw_robot_arrow(int cx, int cy)
{
	fix dist = 0;
	int objnum = survival_nearest_robot(&dist);
	g3s_point pt;
	fix dx, dy;
	fixang ang;
	fix s, c, range, bright;
	int cr, cg, cb;
	int ux, uy, px, py;
	int tipx, tipy, b1x, b1y, b2x, b2y;

	if (objnum < 0)
		return;

	g3_rotate_point(&pt, &Objects[objnum].pos);

	// g3_project_point() maps view x by half the canvas width and view y by
	// half its height, so the vertical component has to be scaled by the
	// aspect ratio for the arrow to point where the target actually is on a
	// wide screen.
	dx = pt.p3_x;
	dy = (cx > 0) ? fixmuldiv(pt.p3_y, i2f(cy), i2f(cx)) : pt.p3_y;

	// In view, and already inside the arrow's own radius -- you're looking
	// right at it, so don't clutter the crosshair.
	if (pt.p3_codes == 0 && pt.p3_z > 0)
	{
		g3_project_point(&pt);
		if (!(pt.p3_flags & PF_OVERFLOW))
		{
			int sx = f2i(pt.p3_sx) - cx;
			int sy = f2i(pt.p3_sy) - cy;

			if (sx * sx + sy * sy < SURVIVAL_ARROW_RADIUS * SURVIVAL_ARROW_RADIUS)
				return;
		}
	}

	// Dead ahead or dead behind: no meaningful direction to point.
	if (labs((long)dx) < F1_0 / 16 && labs((long)dy) < F1_0 / 16)
		return;

	// Screen y grows downward, view-space y grows upward.
	ang = fix_atan2(dx, dy);
	fix_sincos(ang, &s, &c);

	ux = f2i(fixmul(i2f(SURVIVAL_ARROW_RADIUS), c));
	uy = -f2i(fixmul(i2f(SURVIVAL_ARROW_RADIUS), s));

	// Unit-ish perpendicular, scaled to the arrowhead's half width.
	px = f2i(fixmul(i2f(SURVIVAL_ARROW_HALF_WIDTH), s));
	py = f2i(fixmul(i2f(SURVIVAL_ARROW_HALF_WIDTH), c));

	tipx = cx + ux + f2i(fixmul(i2f(SURVIVAL_ARROW_LENGTH), c));
	tipy = cy + uy - f2i(fixmul(i2f(SURVIVAL_ARROW_LENGTH), s));

	b1x = cx + ux + px;
	b1y = cy + uy + py;
	b2x = cx + ux - px;
	b2y = cy + uy - py;

	// Brightness by range: darkest at SURVIVAL_ARROW_NEAR_DIST or closer,
	// full reticle color at SURVIVAL_ARROW_FAR_DIST or beyond.
	if (dist <= SURVIVAL_ARROW_NEAR_DIST)
		range = 0;
	else if (dist >= SURVIVAL_ARROW_FAR_DIST)
		range = F1_0;
	else
		range = fixdiv(dist - SURVIVAL_ARROW_NEAR_DIST,
		               SURVIVAL_ARROW_FAR_DIST - SURVIVAL_ARROW_NEAR_DIST);

	bright = SURVIVAL_ARROW_DARKEST + fixmul(F1_0 - SURVIVAL_ARROW_DARKEST, range);

	// Slow pulse so the arrow reads as a live indicator rather than a static
	// piece of the cockpit. It only dips the brightness, so the range
	// signal above still comes through.
	if (GameTime64 & 0x4000)
		bright = fixmul(bright, F1_0 * 4 / 5);

	survival_reticle_rgb(&cr, &cg, &cb);

	// Darker same-hue accent underneath, offset a pixel, for the same reason
	// the score popups have one: legibility over bright level geometry.
	gr_setcolor(survival_shade(cr, cg, cb, fixmul(bright, SURVIVAL_ACCENT_SCALE)));
	gr_line(i2f(tipx + 1), i2f(tipy + 1), i2f(b1x + 1), i2f(b1y + 1));
	gr_line(i2f(tipx + 1), i2f(tipy + 1), i2f(b2x + 1), i2f(b2y + 1));
	gr_line(i2f(b1x + 1), i2f(b1y + 1), i2f(b2x + 1), i2f(b2y + 1));

	gr_setcolor(survival_shade(cr, cg, cb, bright));
	gr_line(i2f(tipx), i2f(tipy), i2f(b1x), i2f(b1y));
	gr_line(i2f(tipx), i2f(tipy), i2f(b2x), i2f(b2y));
	gr_line(i2f(b1x), i2f(b1y), i2f(b2x), i2f(b2y));
}

void survival_draw_kill_feedback(void)
{
	int cx, cy;

	if (!(Game_mode & GM_MULTI) || Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	cx = grd_curcanv->cv_bitmap.bm_w / 2;
	cy = grd_curcanv->cv_bitmap.bm_h / 2;

	survival_draw_kill_popups();

	// The arrow is an aid for a player who is actually flying; a spectator
	// has no crosshair to decorate.
	if (survival_is_eliminated(Player_num) || is_observer())
		return;

	survival_draw_robot_arrow(cx, cy);
}
