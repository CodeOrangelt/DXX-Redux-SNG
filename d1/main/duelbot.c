/*
 * Duel: one human against one CPU ship. See duelbot.h.
 *
 * Techniques borrowed (not code) from the Descent 3 robot AI and the
 * Quake III bot: physics-model lead, committed dodges, wall repulsion,
 * circle-distance hysteresis, item choice by value over travel, and an
 * aggression score that decides between fighting, chasing and restocking.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <time.h>

#include "duelbot.h"
#include "inferno.h"
#include "game.h"
#include "gameseq.h"
#include "player.h"
#include "object.h"
#include "segment.h"
#include "gameseg.h"
#include "wall.h"
#include "fvi.h"
#include "ai.h"
#include "laser.h"
#include "weapon.h"
#include "powerup.h"
#include "fireball.h"
#include "cntrlcen.h"
#include "physics.h"
#include "polyobj.h"
#include "multi.h"
#include "hudmsg.h"
#include "digi.h"
#include "sounds.h"
#include "timer.h"
#include "console.h"
#include "mission.h"
#include "dxma.h"
#include "physfsx.h"
#include "args.h"
#include "net_udp.h"
#include "playsave.h"

#define DUEL_RESPAWN_TIME	(F1_0*2)
#define DUEL_MAX_BOTS		MAX_PLAYERS

// senses and reflexes
#define DUEL_LEVEL_RATE		i2f(8)		// how fast it rolls back upright
#define DUEL_SLOP_HOLD		(F1_0*35/100)	// aim wander drifts to a new point this often
#define DUEL_REACQUIRE		(F1_0/2)	// sight lost shorter than this is a flicker
#define DUEL_SIGHT_RANGE	i2f(600)
#define DUEL_CLOAK_SIGHT	i2f(30)
#define DUEL_MEMORY		(F1_0*15)
#define DUEL_HEAR_RANGE		i2f(400)
#define DUEL_HEAR_WINDOW	(F1_0/4)
#define DUEL_SELF_NOISE		i2f(25)		// sounds this close to itself are its own doing
#define DUEL_PROBE_DELAY	(F1_0*3/10)	// how long after a shot their dodge shows
#define DUEL_FOE_SMOOTH		8		// sightings an old habit takes to fade
#define DUEL_ESCAPE_PROJECT	(F1_0*3/2)	// how far ahead to cut off a pilot who broke sight
#define DUEL_CUTOFF_MIN		i2f(30)		// a prediction this close to the last sighting is just a chase
#define DUEL_HURT_SHIELDS	i2f(50)		// "hurt", for reading what they do about it
#define DUEL_HURT_MOVING	i2f(10)		// closing speed that counts as a choice, not drift
#define DUEL_HURT_SMOOTH	120		// frames: a habit, not a single dodge
#define DUEL_HURT_CONFIDENT	90
#define DUEL_HURT_LEAN		(F1_0/3)
#define DUEL_HAUNTS		8
#define DUEL_HAUNT_WINDOW	(F1_0*15)	// after a death: where they go first is a habit
#define DUEL_HAUNT_SURE		2
#define DUEL_DODGE_READ		(F1_0/2)	// share of a habitual jink worth leading
#define DUEL_RANGE_DENIAL	(F1_0*3/10)	// how hard to pull away from their favourite range
#define DUEL_AIMED_AT_DOT	fl2f(0.995)	// the enemy's nose is on it

// dodging
#define DUEL_DODGE_RANGE	i2f(200)
#define DUEL_DODGE_TIME		(F1_0*3/4)
#define DUEL_DODGE_MARGIN	i2f(4)
#define DUEL_WATCHED		8
#define DUEL_SLIDE_SPEED	i2f(10)	// sideways speed worth keeping over a fresh juke
#define DUEL_MISSILE_ALERT	(F1_0*8)
#define DUEL_NEAR_MISS		3
#define DUEL_HOMER_CUT		(F1_0*4/10)	// share of the dodge spent closing on the homer
#define DUEL_SIDESTEPS		8
#define DUEL_SIDESTEP_ROOM	i2f(60)
#define DUEL_MISSILE_TIME	(F1_0*5/4)	// missiles are big and slow: seen early, dodged early
#define DUEL_DODGE_HOLD		(F1_0*2/5)
#define DUEL_NEAR_THREAT	i2f(25)

// fighting movement
#define DUEL_STRAFE_MIN		(F1_0*2)
#define DUEL_STRAFE_SPAN	(F1_0*2)
#define DUEL_JINK_MIN		(F1_0*3/10)
#define DUEL_RANGE_BAND		(F1_0/6)
#define DUEL_TOO_CLOSE		i2f(20)
#define DUEL_UNDERGUN_FLEE	i2f(90)		// nearer, turning tail is the bigger risk
#define DUEL_WALL_PROBE		i2f(18)
#define DUEL_WALL_MARGIN	i2f(8)

// flying
#define DUEL_TOP_SPEED		i2f(90)		// above reachable: steering only ever corrects drift
#define DUEL_SPIN_DAMP		i2f(8)
#define DUEL_KEY_SHARE		3		// axes within 1/3 of the strongest get a full key
#define DUEL_TRICHORD_DIST	i2f(30)
#define DUEL_TRAVEL_SMOOTH	(F1_0*4)	// per second: how fast the trichord heading follows the route
#define DUEL_BRAKE_SHARE	4		// of full thrust counted on for braking (the rest is steering)
#define DUEL_MIN_CORNER_SPEED	i2f(45)
#define DUEL_WALL_HIT_COST	(F1_0/20)	// flight reward lost per wall hit: speed that ends in a wall isn't speed
#define DUEL_WALL_HIT_SPEED	i2f(15)

// routes
#define DUEL_PATH_MAX		128
#define DUEL_PATH_REFRESH	(F1_0/2)
#define DUEL_LOOKAHEAD		4
#define DUEL_ARRIVE_DIST	i2f(10)
#define DUEL_PICKUP_APPROACH	i2f(40)
#define DUEL_STUCK_DIST		i2f(4)
#define DUEL_STUCK_TIME		(F1_0*3/2)
#define DUEL_UNSTICK_TIME	(F1_0/2)
#define DUEL_GRAB_DEPTH		4		// a pickup this many segments off the way is worth the detour
#define DUEL_GRAB_RANGE		i2f(60)		// ...and one this close is grabbed mid-fight
#define DUEL_GRAB_PULL		(F1_0*3/2)
#define DUEL_TRAVEL_BIAS	4		// segments added to every trip when weighing pickups
#define DUEL_TOPPED_UP		i2f(100)	// past this, shield/energy pickups aren't worth a detour
#define DUEL_PATROL_MIN_DIST	i2f(100)
#define DUEL_AVOID_SLOTS	8
#define DUEL_AVOID_TIME		(F1_0*30)	// a goal it got stuck reaching is off the list this long

// strategy
#define DUEL_RETREAT_AGGRESSION	(F1_0*45/100)
#define DUEL_CHASE_AGGRESSION	(F1_0*60/100)
#define DUEL_LOW_ENERGY		i2f(25)
#define DUEL_TOP_UP_ENERGY	i2f(60)		// a fuel cen on the way is worth it below this
#define DUEL_FULL_TANK		(INITIAL_ENERGY - i2f(5))
#define DUEL_CORNER_BERTH	i2f(6)		// route lines keep this off edges: wide turns, no clipped corners
#define DUEL_SAVE_ENERGY	i2f(40)
#define DUEL_FUSION_COST	(F1_0*2)
#define DUEL_REFUEL_RATE	i2f(25)

// weapons
#define DUEL_MISSILE_GAP	(F1_0*3/4)
#define DUEL_TRIGGER_HOLD	(F1_0*3/20)
#define DUEL_HOLD_DOT		fl2f(0.985)
#define DUEL_MISSILE_GUN	4
#define DUEL_BOMB_GUN		7
#define DUEL_PREDICT_STEP	(F1_0/64)	// the physics model's own tick
#define DUEL_PREDICT_MAX	96		// 1.5s of ticks
#define DUEL_ACCEL_SMOOTH	(F1_0/8)
#define DUEL_TWO_PI		fl2f(6.2832)

// learning
#define DUEL_LEAD_ARMS		4
#define DUEL_RANGE_ARMS		3
#define DUEL_FLIGHT_ARMS	4		// plain, trichord, trichord on held keys, early trichord
#define DUEL_FLIGHT_EPOCH	(F1_0*2)	// of travelling time per trial
#define DUEL_EARLY_TRICHORD	i2f(15)
#define DUEL_LEAD_EPOCH		(F1_0*3/2)
#define DUEL_RANGE_EPOCH	(F1_0*4)
#define DUEL_EXPLORE		(32768/10)
#define DUEL_LEARN_WINDOW	100		// the brain persists across sessions: average long
#define DUEL_HEAT_CAP		60000
#define DUEL_HEAT_DECAY		(F1_0*20)

// Descent 101 tactics
#define DUEL_BLAST_CLEARANCE	i2f(90)		// a dying ship blows 50 damage over 40 units
#define DUEL_DEATH_TIME		(F1_0*2)	// same tumble-then-blow a human ship gets
#define DUEL_MINE_CLEARANCE	i2f(35)
#define DUEL_MINE_LOOK		i2f(70)		// mines this far up the route get planned around
#define DUEL_MINE_GAP_SPARE	i2f(3)		// hull-to-mine room it wants when slipping past
#define DUEL_MINE_SHOOT_MAX	i2f(120)
#define DUEL_TURN_EASE		i2f(10)		// per second: big turns start fast, finish soft
#define DUEL_SLOP_DRIFT		i2f(4)		// per second: aim wander glides, never snaps
#define DUEL_SPLASH_MARGIN	i2f(15)
#define DUEL_MINE_SHOT_RANGE	i2f(80)
#define DUEL_MINE_SHOT_CONE	(F1_0*9/10)
#define DUEL_FOV_DOT		(F1_0/2)	// ~60 degrees either side of the nose
#define DUEL_PERIPHERAL		i2f(40)
#define DUEL_TRACK_GRACE	(F1_0/2)	// a target just off-screen is still being tracked
#define DUEL_REAR_PERIOD	(F1_0*5/2)
#define DUEL_REAR_GLANCE	(F1_0*3/10)
#define DUEL_REAR_DOT		(-F1_0*6/10)
#define DUEL_OPEN_PROBE		i2f(200)
#define DUEL_OPEN_REFRESH	(F1_0/2)
#define DUEL_TIGHT		i2f(45)		// hallway
#define DUEL_OPEN		i2f(120)	// big room
#define DUEL_ENGAGE_MIN		(F1_0*3)	// Always Be Escaping: fight this long...
#define DUEL_ENGAGE_SPAN	(F1_0*2)
#define DUEL_BREAK_TIME		(F1_0*3/2)	// ...then break line of sight this long
#define DUEL_COVER_DEPTH	4
#define DUEL_BULL_SHIELDS	i2f(40)		// Charging Bull when the enemy is this weak
#define DUEL_CHARGING_SPEED	i2f(25)
#define DUEL_PREFIRE_WINDOW	(F1_0*3/2)
#define DUEL_PREFIRE_RANGE	i2f(120)
#define DUEL_PREFIRE_ENERGY	i2f(60)
#define DUEL_TRAP_MIN		(F1_0/2)	// too fresh: they may still be right there
#define DUEL_TRAP_MAX		(F1_0*4)
#define DUEL_TRAP_RANGE		i2f(160)
#define DUEL_TRAP_SLACK		i2f(30)	// blobs home in: landing this close is enough
#define DUEL_FLARE_RANGE	i2f(90)
#define DUEL_FLARE_GAP		(F1_0*3/4)
#define DUEL_SIDE_PICK		(F1_0/2)
#define DUEL_SIDE_BETTER	(F1_0*5/4)	// the other side must be this much more open
#define DUEL_SIDE_SEEN		(F1_0*8)	// their last sighting still steers the nose
#define DUEL_SHOT_CHECK		i2f(250)
#define DUEL_CAMP_AFTER		(F1_0*8)	// no idea where they are: camp until you do
#define DUEL_CAMP_TIME		(F1_0*6)
#define DUEL_CAMP_COOLDOWN	(F1_0*10)
#define DUEL_SHIELD_SURPRISE	i2f(30)		// my count hit zero but they're alive
#define DUEL_SHIELD_REGEN	(F1_0*3/2)		// per second out of sight: they're picking up shields
#define DUEL_HOLD_ANGLE		(F1_0*3/2)	// lost sight: keep aiming where they'll reappear
#define DUEL_MINE_REACH		i2f(25)		// close enough that the proxy covers the pickup
#define DUEL_MINE_KEEP		2			// leave some for bombing a chaser
#define DUEL_MINE_WORTH		9
#define DUEL_FUSION_SWAP_MIN	i2f(70)
#define DUEL_MINE_QUIET		(F1_0*10)	// only detour to mine when the duel has gone cold
#define DUEL_MINE_GAP		(F1_0*20)
#define DUEL_MINE_DEPTH		12		// segments it will detour to set one
#define DUEL_GUARD_TIME		(F1_0*4)	// weapon guarding after a kill
#define DUEL_GUARD_SHIELDS	i2f(50)		// too hurt to sit on a spew
#define DUEL_REVISIT		(F1_0*20)	// a hot spot checked this recently can wait
#define DUEL_SWEEP_STALE	(F1_0*25)	// unchecked this long: worth a look
#define DUEL_SWEEP_MIN_DEPTH	3		// next door is already in view
#define DUEL_TRICHORD_QUIET	(F1_0*3)	// no contact this long: safe to fly sideways
#define DUEL_HARD_STOP		(F1_0/4)
#define DUEL_HARD_STOP_ODDS	(32768*35/100)
#define DUEL_FUSION_RELEASE	(F1_0*6/5)
#define DUEL_FUSION_FULL	(F1_0*2)	// past this, overcharge hurts
#define DUEL_JUKE		(F1_0/2)	// share of sideways motion the lead can't predict
#define DUEL_WEAPON_TRIALS	30
#define DUEL_WEAPON_HOLD	(F1_0*5)	// commit to a gun for the fight
#define DUEL_SWITCH_MARGIN	(F1_0*3/2)	// a new gun has to be this much better between fights
#define DUEL_SWAP_MARGIN	(F1_0*2)	// ...and this much better to swap mid-fight
#define DUEL_PRESELECT_HOLD	(F1_0*10)
#define DUEL_NEW_FIGHT		(F1_0*3)	// out of sight this long: next sighting is a new fight
#define DUEL_NOMINAL_LATERAL	i2f(30)		// how fast a typical target slides, for picking ahead

// bench
#define DUEL_BENCH_FRAME	(F1_0/30)
#define DUEL_BENCH_REPORT	(F1_0*60)
#define DUEL_IDLE_GUN		(F1_0/2)
#define DUEL_LOG_INTERVAL	F1_0

typedef struct duel_arm
{
	fix		score;
	int		trials;
} duel_arm;

typedef struct duel_brain
{
	duel_arm	lead[DUEL_LEAD_ARMS];
	duel_arm	range[DUEL_RANGE_ARMS];
	duel_arm	flight[DUEL_FLIGHT_ARMS];	// rewarded with the speed it actually made
	int		weapon_shots[MAX_PRIMARY_WEAPONS], weapon_hits[MAX_PRIMARY_WEAPONS];
	unsigned short	heat[MAX_SEGMENTS];
	fix64		heat_decay_at;
} duel_brain;

typedef struct duel_epoch
{
	int		arm;
	fix64	ends_at;
	int		shots, hits;
	fix		dealt, taken;
} duel_epoch;

typedef struct duel_stats
{
	int		shots, missiles, hits, kills, deaths, stuck_events;
	fix		dealt, taken;
	fix64	alive, visible, idle_gun, no_gun, upside, stuck, fight, retreat, hunt;
	fix64	idle_reacting, idle_off_aim, idle_cooldown;
	int		plans[4];	// supplies, last seen, supplies (idle), patrol
	fix64	mode_time[8];
	int		weapon_shots[MAX_PRIMARY_WEAPONS];
	int		switches;
	fix		blast_taken;
	fix64	frozen;		// barely moving outside camp/pre-fire: should be ~0
	int		wall_hits;	// slammed into level geometry at speed
	int		missiles_dodged, missiles_taken;
} duel_stats;

typedef struct duel_bot
{
	int		pnum;
	int		active;
	fix64	respawn_at;

	int		visible;
	fix64	sighted_at, seen_at, heard_at;
	vms_vector	last_seen_pos;
	int		last_seen_seg;
	vms_vector	enemy_vel, enemy_accel;
	int		enemy_objnum;

	fix64	next_primary, next_secondary, trigger_until, last_shot_at, next_flare;
	fix64	side_pick_at;
	int	aiming_at_mine;
	vms_vector	trichord_nose;	// which side of the route the nose rides
	int		spread_toggle, missile_gun;

	int		strafe_sign, climb_sign;
	fix64	strafe_flip_at;
	struct { int sig; fix closest, last, reach; char seen; } incoming[DUEL_WATCHED];	// enemy missiles, until they're gone
	fix64	next_mine;		// honey pots are spaced out, not a minefield
	fix64	cover_until;		// retreating: heading round a corner first
	fix64	missile_alert_until;	// missiles in play: keep momentum
	vms_vector	dodge_dir;
	vms_vector	aim_slop, aim_slop_goal;
	int		supplies_known;		// the last supply search found something
	fix64	aim_slop_until;
	fix		dodge_strength;
	fix64	dodge_until;

	short	path[DUEL_PATH_MAX];
	int		path_len, path_pos;
	vms_vector	goal_pos;
	int		patrol_seg;
	fix64	next_path;

	vms_vector	stuck_pos;
	fix64	stuck_since, unstick_until;
	vms_vector	unstick_dir;
	int		goal_seg;
	short	avoid_seg[DUEL_AVOID_SLOTS];
	fix64	avoid_until[DUEL_AVOID_SLOTS];

	duel_epoch	lead, range, flight;
	duel_stats	stats;
	fix64	next_log;

	fix64	dying_until;
	int		mode;
	fix		openness;
	fix64	next_openness;
	fix64	rear_phase;
	int		target;			// player number being hunted or fought, -1 none
	fix		est_shields[MAX_PLAYERS];	// counted from damage dealt, the way a pilot counts
	int		enemy_weapon;		// last primary seen coming from it
	vms_vector	travel_dir;		// smoothed, so waypoint hand-offs don't jerk the nose
	int		probe_target;		// whose reaction to our last shot is being timed
	fix64	probe_at;
	fix		probe_r, probe_u;	// their sideways speed when we fired
	fix64	enemy_weapon_at;
	fix64	engage_until, break_until;
	vms_vector	cover_pos;
	fix64	camp_until, next_camp;
	int		camp_seg;
	vms_vector	camp_look;
	fix64	hard_stop_until;
	fix		fusion_charge;
	int		last_primary;
	fix64	last_primary_at, last_missile_at;
	fix64	blast_at;
	int		held_weapon;
	fix64	held_since;
	vms_vector	escape_vel;	// how they were moving when they broke sight
} duel_bot;

enum { MODE_HUNT, MODE_FIGHT, MODE_BULL, MODE_BREAK, MODE_RETREAT, MODE_CAMP, MODE_PREFIRE, MODE_BLAST };
static const char *const Mode_names[] = { "HUNT", "FIGHT", "BULL", "BREAK", "RETREAT", "CAMP", "PREFIRE", "BLAST" };

static const fix Lead_scale[DUEL_LEAD_ARMS] = { F1_0*6/10, F1_0*8/10, F1_0, F1_0*12/10 };
static const fix Range_scale[DUEL_RANGE_ARMS] = { F1_0*6/10, F1_0, F1_0*15/10 };

int Duel_pending = 0;

static duel_brain Brain;
static duel_bot Bots[DUEL_MAX_BOTS];
static int Bot_count;
static fix64 Dying_since[MAX_PLAYERS];	// non-CPU ships counted dead but not yet blown

static int Bench_seconds;
static int Bench_level = 1;
static int Bench_close;		// respawn near the enemy: measure fighting, not searching
static int Bench_net;		// a real netgame: real time, no camera
static duel_bot Pilot;		// bench autopilot flying the local ship through the human code
static int Pilot_on;
static int Hit_weapon_id;		// 1 + weapon id of the hit being applied, 0 = unknown
static int Hit_is_blast;
static int Bench_kit;		// spawn with every gun: measure weapon choice
static fix64 Bench_next_report;
static fix64 Bench_started_at;
static int Idle_reason_aim;

// what each opponent has shown this match: the bot plays against their habits
typedef struct duel_foe
{
	int		samples;
	fix		fight_dist;		// the distance they like to fight at
	fix		dodge_r, dodge_u;	// how they jink after a shot, in the shooter's frame
	int		gun_use[MAX_PRIMARY_WEAPONS];
	fix		hurt_push;		// -1 runs for shields when hurt, +1 turns and fights
	int		hurt_samples, hurt_told;
	fix64	died_at;
	int		haunt_open;		// respawned and not yet sighted
	short	haunt_seg[DUEL_HAUNTS];	// where they were first seen after respawning
	ubyte	haunt_hits[DUEL_HAUNTS];
} duel_foe;

static duel_foe Foes[MAX_PLAYERS];
static int match_tense(void);
static int Match_points;		// set with the dedicated server below
static int Bench_skill = NDL - 1;
static int Dedicated;

// the dedicated server's tiers, by CPU skill: the CPU is named after its tier
static const char *const Tier_names[DUEL_TIERS] = { "Bronze", "Bronze", "Silver", "Gold", "Emerald", "Impossible" };

static int tier_skill(const char *tier)
{
	int i;

	for (i = DUEL_TIERS - 1; i > 0; i--)
		if (!d_stricmp(tier, Tier_names[i]))
			return i;
	return -1;
}

static const char *Server_bot_name;	// -dedicatedname: the server's place, used for its CPU and lobby

static const char *dedicated_cpu_name(void)
{
	return Server_bot_name ? Server_bot_name : Tier_names[min(Netgame.CpuSkill, DUEL_TIERS - 1)];
}

static void name_cpu(int pnum)
{
	if (Dedicated && Netgame.CpuCount)
		snprintf(Players[pnum].callsign, sizeof(Players[pnum].callsign), "%.6s%d", dedicated_cpu_name(), pnum + 1);
	else if (Dedicated)
		snprintf(Players[pnum].callsign, sizeof(Players[pnum].callsign), "%s", dedicated_cpu_name());
	else
		snprintf(Players[pnum].callsign, sizeof(Players[pnum].callsign), "CPU%d", pnum % 10);
}

// what separates a trainee from an insane pilot: how fast it sees, turns,
// leads, holds aim and reads incoming fire
typedef struct duel_skill
{
	fix	reaction;
	fix	turn_rate;		// rad/s
	fix	lead_share;		// of the predicted lead it actually takes
	fix	aim_slop;		// aim wander, in target hull widths
	int	dodge_odds;		// of 32768 incoming shots it reacts to
} duel_skill;

static const duel_skill Skills[DUEL_TIERS] = {
	{ F1_0*50/100, fl2f(2.0), F1_0*45/100, fl2f(2.8), 32768*20/100 },	// trainee
	{ F1_0*36/100, fl2f(2.5), F1_0*60/100, fl2f(1.9), 32768*40/100 },	// rookie
	{ F1_0*26/100, fl2f(2.9), F1_0*72/100, fl2f(1.2), 32768*55/100 },	// hotshot
	{ F1_0*19/100, fl2f(3.3), F1_0*82/100, fl2f(0.7), 32768*72/100 },	// ace
	{ F1_0*14/100, fl2f(3.6), F1_0*90/100, fl2f(0.35), 32768*85/100 },	// insane
	{ F1_0*10/100, fl2f(4.0), F1_0,        0,         32768 },		// impossible: no human-like flaws
};

static fix64 Visited[MAX_PLAYERS][MAX_SEGMENTS];	// when each pilot last passed through
static short Bfs_prev[MAX_SEGMENTS];
static short Bfs_depth[MAX_SEGMENTS];
static short Bfs_queue[MAX_SEGMENTS];

extern fix StartingShields;
extern int Player_exploded;
extern int Death_sequence_aborted;
void init_player_stats_new_ship(ubyte pnum);

static void start_probe(duel_bot *b, const object *bot);
static fix deny_range(int foe, fix range);
static void init_pilot(void);
static int percent(fix64 part, fix64 whole);
static int denial_value(int id);
static int mined_already(const object *bot, const object *pow);
static int quiet(const duel_bot *b);
static int journal_foe_score(void);
static void start_camp(duel_bot *b, int segnum, const vms_vector *look, fix length);
static fix free_distance(const object *bot, const vms_vector *dir);
extern fix Fusion_charge;
void drop_player_eggs(object *playerobj);

//	-------------------------------------------------------------------------
//	Match journal: what the server's CPU did and why, for the humans to read
//	-------------------------------------------------------------------------

#define JOURNAL_DIR		"matchlogs"
#define JOURNAL_MODE_SETTLE	(F1_0*2/5)	// a stance held this long is a decision, not flicker
#define JOURNAL_HEAR_GAP	(F1_0*4)

static const char *const Gun_names[MAX_PRIMARY_WEAPONS] = { "Laser", "Vulcan", "Spreadfire", "Plasma", "Fusion" };
static const char *const Missile_names[MAX_SECONDARY_WEAPONS] = { "Concussion", "Homer", "Proxy", "Smart missile", "Mega" };

static PHYSFS_file *Journal;
static fix64 Journal_start, Journal_heard_at;
static int Journal_mode = -1, Journal_pending_mode = -1, Journal_plan = -1;
static fix64 Journal_pending_since;

static int journaling(const duel_bot *b)
{
	return Journal && b == &Pilot;
}

static void journal(const char *action, const char *decision, const char *fmt, ...)
{
	char why[160];
	fix64 t = (GameTime64 - Journal_start) / F1_0;
	va_list ap;

	if (!Journal)
		return;
	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	PHYSFSX_printf(Journal, "[%02d:%02d] %-8s %-28s %s\n", (int)(t / 60), (int)(t % 60), action, decision, why);
	PHYSFS_flush(Journal);
}

static void journal_open(const char *opponent)
{
	char name[64];
	time_t now = time(NULL);
	struct tm *tm = localtime(&now);

	if (Journal)
		return;
	PHYSFS_mkdir(JOURNAL_DIR);
	snprintf(name, sizeof(name), JOURNAL_DIR "/%04d%02d%02d-%02d%02d%02d.txt", tm->tm_year + 1900, tm->tm_mon + 1,
			 tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
	Journal = PHYSFSX_openWriteBuffered(name);
	if (!Journal)
		return;
	Journal_start = GameTime64;
	PHYSFSX_printf(Journal, "%s vs %s on %s level %d\n\n", Players[Player_num].callsign, opponent,
				   Current_mission_longname, Current_level_num);
	journal("START", "match begins", "first to the point goal, win by 2");
}

static void journal_close(void)
{
	if (!Journal)
		return;
	PHYSFS_close(Journal);
	Journal = NULL;
}

//	-------------------------------------------------------------------------
//	Lookups
//	-------------------------------------------------------------------------

static player *bot_player(const duel_bot *b)
{
	return &Players[b->pnum];
}

static duel_bot *bot_for_pnum(int pnum)
{
	int i;

	for (i = 0; i < Bot_count; i++)
		if (Bots[i].active && Bots[i].pnum == pnum)
			return &Bots[i];

	return NULL;
}

// this machine drives the CPUs: the bench, or the host of a netgame
static int duelbot_running(void)
{
	if (!Bot_count)
		return 0;
	return (Game_mode & GM_DUEL) || ((Game_mode & GM_MULTI) && multi_i_am_master());
}

// the host's own autopilot counts too: on the server it is the only CPU
static int cpu_thinking(void)
{
	return duelbot_running() || Pilot_on;
}

static duel_bot *cpu_for_pnum(int pnum)
{
	if (Pilot_on && pnum == Player_num)
		return &Pilot;
	return bot_for_pnum(pnum);
}

static int net_game(void)
{
	return (Game_mode & GM_MULTI) != 0;
}

// the bench autopilot stands in for a top human and always flies at full skill
static const duel_skill *skill(const duel_bot *b)
{
	int level = Bench_skill;

	if (b == &Pilot && !Dedicated)
		level = NDL - 1;		// the bench's stand-in for a top human
	else if (net_game() && !Bench_seconds)
		level = Netgame.CpuSkill;
	return &Skills[min(level, DUEL_TIERS - 1)];
}

int duelbot_is_bot(const object *obj)
{
	return duelbot_running() && obj->type == OBJ_PLAYER && bot_for_pnum(obj->id);
}

static object *live_player_object(int pnum)
{
	int objnum = Players[pnum].objnum;
	object *obj;

	if (objnum < 0 || objnum > Highest_object_index)
		return NULL;

	obj = &Objects[objnum];
	if (obj->type != OBJ_PLAYER || obj->id != pnum || (obj->flags & OF_SHOULD_BE_DEAD))
		return NULL;

	return obj;
}

static object *bot_object(const duel_bot *b)
{
	if (!b->active || b->respawn_at || b->dying_until)
		return NULL;

	return live_player_object(b->pnum);
}

static int teammates(int a, int b)
{
	return (Game_mode & GM_TEAM) && get_team(a) == get_team(b);
}

static int ship_dying(int pnum)
{
	duel_bot *owner = bot_for_pnum(pnum);

	if (owner)
		return owner->dying_until != 0;
	return Dying_since[pnum] && GameTime64 - Dying_since[pnum] < DUEL_DEATH_TIME + F1_0/2;
}

static int targetable(const duel_bot *b, int pnum)
{
	if (pnum == b->pnum || !Players[pnum].connected || teammates(pnum, b->pnum) || ship_dying(pnum))
		return 0;
	if (Bench_seconds && !Bench_net && pnum == Player_num)
		return 0;		// the offline bench's camera
	if (net_game() && Netgame.host_is_obs && pnum == 0)
		return 0;		// a dedicated server's own seat only watches
	return live_player_object(pnum) != NULL;
}

static int can_see(const duel_bot *b, const object *bot, const object *enemy);

// keep fighting whoever is in sight; else switch to the nearest one that is;
// else keep hunting the one it remembers; else go looking for the nearest
static object *choose_target(duel_bot *b, const object *bot)
{
	int p, seen = -1, nearest = -1;
	fix seen_dist = 0, near_dist = 0;

	if (b->target >= 0 && targetable(b, b->target) && can_see(b, bot, live_player_object(b->target)))
		return live_player_object(b->target);

	for (p = 0; p < MAX_PLAYERS; p++)
	{
		object *ship;
		fix dist;

		if (!targetable(b, p))
			continue;
		ship = live_player_object(p);
		dist = vm_vec_dist((vms_vector *)&bot->pos, &ship->pos);
		if (nearest < 0 || dist < near_dist)
			nearest = p, near_dist = dist;
		if (can_see(b, bot, ship) && (seen < 0 || dist < seen_dist))
			seen = p, seen_dist = dist;
	}

	if (seen >= 0)
		b->target = seen;
	else if (b->target < 0 || !targetable(b, b->target))
		b->target = nearest;

	return b->target >= 0 ? live_player_object(b->target) : NULL;
}

static fix target_shields(const duel_bot *b)
{
	return b->target >= 0 ? b->est_shields[b->target] : StartingShields;
}

// runners get chased down early; fighters are left to come onto the guns
static fix bull_shields(const duel_bot *b)
{
	const duel_foe *f = b->target >= 0 ? &Foes[b->target] : NULL;

	if (!f || f->hurt_samples < DUEL_HURT_CONFIDENT)
		return DUEL_BULL_SHIELDS;
	if (f->hurt_push < -DUEL_HURT_LEAN)
		return DUEL_BULL_SHIELDS * 3 / 2;
	if (f->hurt_push > DUEL_HURT_LEAN)
		return DUEL_BULL_SHIELDS * 2 / 3;
	return DUEL_BULL_SHIELDS;
}

static int random_sign(void)
{
	return (d_rand() & 1) ? 1 : -1;
}

static fix random_fix(fix span)
{
	return fixmul(span, d_rand() * 2);
}

static fix clamp_unit(fix value)
{
	if (value > F1_0)
		return F1_0;
	if (value < -F1_0)
		return -F1_0;
	return value;
}

//	-------------------------------------------------------------------------
//	Learning: epsilon-greedy bandits over lead and range, plus a heat map
//	of where the enemy likes to be
//	-------------------------------------------------------------------------

static int pick_arm(const duel_arm *arms, int count)
{
	int i, best = 0;

	if (d_rand() < DUEL_EXPLORE)
		return d_rand() % count;

	for (i = 0; i < count; i++)
	{
		if (!arms[i].trials)
			return i;
		if (arms[i].score > arms[best].score)
			best = i;
	}

	return best;
}

static void reward_arm(duel_arm *arm, fix reward)
{
	int window = min(arm->trials + 1, DUEL_LEARN_WINDOW);

	arm->trials++;
	arm->score += (reward - arm->score) / window;
}

static void start_epoch(duel_epoch *epoch, const duel_arm *arms, int count, fix length)
{
	memset(epoch, 0, sizeof(*epoch));
	epoch->arm = pick_arm(arms, count);
	epoch->ends_at = GameTime64 + length;
}

// lifetime hit rate, so a lead is judged against the gun it was used with
static fix weapon_hit_rate(int weapon)
{
	int shots = Brain.weapon_shots[weapon];

	if (shots < DUEL_WEAPON_TRIALS)
		return 0;
	return fixdiv(i2f(min(Brain.weapon_hits[weapon], shots)), i2f(shots));
}

static void finish_lead_epoch(duel_bot *b)
{
	duel_epoch *e = &b->lead;

	if (GameTime64 < e->ends_at)
		return;

	if (e->shots)
		reward_arm(&Brain.lead[e->arm], fixdiv(i2f(min(e->hits, e->shots)), i2f(e->shots)) - weapon_hit_rate(b->last_primary));

	start_epoch(e, Brain.lead, DUEL_LEAD_ARMS, DUEL_LEAD_EPOCH);
}

// the speedometer: travelling time only, rewarded by speed made good (per 100 units/s)
static void track_flight(duel_bot *b, const object *bot, int travel)
{
	duel_epoch *e = &b->flight;

	if (!travel)
	{
		e->ends_at += FrameTime;
		return;
	}
	e->dealt += fixmul(vm_vec_mag((vms_vector *)&bot->mtype.phys_info.velocity), FrameTime);
	e->taken += FrameTime;
	if (GameTime64 < e->ends_at)
		return;

	if (e->taken > 0)
		reward_arm(&Brain.flight[e->arm], fixdiv(fixdiv(e->dealt, e->taken), i2f(100)) - e->hits * DUEL_WALL_HIT_COST);
	start_epoch(e, Brain.flight, DUEL_FLIGHT_ARMS, DUEL_FLIGHT_EPOCH);
}

// only fighting time counts toward a range trial
static void finish_range_epoch(duel_bot *b, int fighting)
{
	duel_epoch *e = &b->range;

	if (!fighting)
	{
		e->ends_at += FrameTime;
		return;
	}

	if (GameTime64 < e->ends_at)
		return;

	reward_arm(&Brain.range[e->arm], e->dealt - e->taken);
	start_epoch(e, Brain.range, DUEL_RANGE_ARMS, DUEL_RANGE_EPOCH);
}

static void note_heat(int segnum)
{
	if (Brain.heat[segnum] < DUEL_HEAT_CAP)
		Brain.heat[segnum]++;
}

static void decay_heat(void)
{
	int i;

	if (GameTime64 < Brain.heat_decay_at)
		return;

	Brain.heat_decay_at = GameTime64 + DUEL_HEAT_DECAY;
	for (i = 0; i <= Highest_segment_index; i++)
		Brain.heat[i] -= Brain.heat[i] / 4;
}

static int hottest_segment(int exclude)
{
	int i, best = -1;

	for (i = 0; i <= Highest_segment_index; i++)
		if (i != exclude && Brain.heat[i] && (best < 0 || Brain.heat[i] > Brain.heat[best]))
			best = i;

	return best;
}

static void credit_hit(const object *attacker, const object *victim, fix damage)
{
	duel_bot *b;

	if (!attacker || attacker->type != OBJ_PLAYER || attacker == victim)
		return;

	b = cpu_for_pnum(attacker->id);
	if (!b)
		return;

	b->lead.hits++;
	b->range.dealt += damage;
	b->stats.hits++;
	b->stats.dealt += damage;
	if (victim->type == OBJ_PLAYER && victim->id < MAX_PLAYERS)
	{
		b->est_shields[victim->id] -= damage;
		if (b->est_shields[victim->id] <= 0)
			b->est_shields[victim->id] = DUEL_SHIELD_SURPRISE;	// still alive: they had more than I counted
	}

	// a hit soon after a gun shot, with no missile in the air, belongs to that gun
	if (GameTime64 - b->last_primary_at < F1_0 && GameTime64 - b->last_missile_at > F1_0*2)
		Brain.weapon_hits[b->last_primary]++;
}

// any ship struck by a CPU's gun, on any machine's behalf: the host sees them all
void duelbot_weapon_hit(const object *attacker, const object *victim, fix damage, const object *weapon)
{
	Hit_weapon_id = weapon && weapon->type == OBJ_WEAPON ? weapon->id + 1 : 0;	// read by the damage that follows
	Hit_is_blast = 0;
	if (cpu_thinking())
		credit_hit(attacker, victim, damage);
}

//	-------------------------------------------------------------------------
//	Spawning
//	-------------------------------------------------------------------------

// a ship already parked here would wedge the new one inside it
static int start_occupied(int idx)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_PLAYER &&
			vm_vec_dist(&Objects[i].pos, &Player_init[idx].pos) < DUEL_TOO_CLOSE)
			return 1;

	return 0;
}

static int usable_start(int idx)
{
	return Player_init[idx].segnum >= 0 && Player_init[idx].segnum <= Highest_segment_index &&
		!start_occupied(idx);
}

// nearest free start that isn't on top of `from`
static int spawn_index_near(const object *from)
{
	int i, best = 0, count = min(NumNetPlayerPositions, MAX_PLAYERS);
	fix best_dist = F1_0 * 30000;

	for (i = 0; i < count; i++)
	{
		fix dist;

		if (!usable_start(i))
			continue;

		dist = vm_vec_dist(&Player_init[i].pos, (vms_vector *)&from->pos);
		if (dist > DUEL_TOO_CLOSE * 2 && dist < best_dist)
		{
			best_dist = dist;
			best = i;
		}
	}

	return best;
}

static fix nearest_enemy_dist(const duel_bot *b, const vms_vector *pos, object **nearest)
{
	fix best = F1_0 * 30000;
	int p;

	*nearest = NULL;
	for (p = 0; p < MAX_PLAYERS; p++)
	{
		object *ship;
		fix dist;

		if (!targetable(b, p))
			continue;
		ship = live_player_object(p);
		dist = vm_vec_dist((vms_vector *)pos, &ship->pos);
		if (dist < best)
			best = dist, *nearest = ship;
	}

	return best;
}

// the free start furthest from the nearest enemy, as a netgame respawn picks
static int spawn_index(const duel_bot *b)
{
	int i, best = 0, count = min(NumNetPlayerPositions, MAX_PLAYERS);
	fix best_dist = -1;
	object *nearest;

	if (Bench_close)
	{
		nearest_enemy_dist(b, &Player_init[0].pos, &nearest);
		if (nearest)
			return spawn_index_near(nearest);
	}

	for (i = 0; i < count; i++)
	{
		fix dist;

		if (!usable_start(i))
			continue;

		dist = nearest_enemy_dist(b, &Player_init[i].pos, &nearest);
		if (!nearest)
			dist = d_rand();
		if (dist > best_dist)
			best_dist = dist, best = i;
	}

	return best;
}

static void reset_loadout(duel_bot *b)
{
	player *p = bot_player(b);

	init_player_stats_new_ship(b->pnum);	// same new ship a human gets, netgame rules included
	if (!net_game())
		p->shields = StartingShields;		// netgame toggles are stale outside one

	if (Bench_kit)
	{
		p->primary_weapon_flags = 0x1f;
		p->primary_ammo[VULCAN_INDEX] = VULCAN_AMMO_MAX;
		p->laser_level = MAX_LASER_LEVEL;
		p->flags |= PLAYER_FLAGS_QUAD_LASERS;
		p->energy = MAX_ENERGY;
	}
	if (Bench_kit == 2)
		p->secondary_ammo[HOMING_INDEX] = Secondary_ammo_max[HOMING_INDEX];
	if (Bench_kit == 3)
		p->secondary_ammo[SMART_INDEX] = Secondary_ammo_max[SMART_INDEX];
}

static void reset_navigation(duel_bot *b)
{
	b->path_len = 0;
	b->supplies_known = 1;	// assume there is a gun to find until a search says no
	b->path_pos = 0;
	b->next_path = 0;
	b->patrol_seg = -1;
	b->visible = 0;
	b->seen_at = 0;
	b->stuck_since = 0;
	b->unstick_until = 0;
	b->dodge_until = 0;
	b->goal_seg = -1;
	b->held_weapon = LASER_INDEX;
	b->held_since = 0;
	b->strafe_sign = random_sign();
	b->climb_sign = random_sign();
}

// the slot's ship, created once for the bench; a netgame level already has one
static object *slot_ship(duel_bot *b)
{
	int objnum = Players[b->pnum].objnum;

	if (objnum >= 0 && objnum <= Highest_object_index && Objects[objnum].id == b->pnum &&
		(Objects[objnum].type == OBJ_PLAYER || Objects[objnum].type == OBJ_GHOST))
		return &Objects[objnum];

	objnum = obj_create(OBJ_GHOST, b->pnum, Player_init[0].segnum, &Player_init[0].pos, &Player_init[0].orient,
						Polygon_models[Player_ship->model_num].rad, CT_NONE, MT_NONE, RT_NONE);
	if (objnum < 0)
		return NULL;

	Players[b->pnum].objnum = objnum;
	return &Objects[objnum];
}

static void spawn_bot(duel_bot *b)
{
	object *obj = slot_ship(b);
	int idx = spawn_index(b);

	if (!obj)
	{
		b->respawn_at = GameTime64 + DUEL_RESPAWN_TIME;
		return;
	}

	obj->pos = Player_init[idx].pos;
	obj->orient = Player_init[idx].orient;
	obj_relink(obj - Objects, Player_init[idx].segnum);

	if (obj->type == OBJ_GHOST)
		multi_make_ghost_player(b->pnum);
	multi_reset_player_object(obj);
	obj->flags &= ~(OF_EXPLODING | OF_SHOULD_BE_DEAD);
	// remote-ship setup omits thrust; the bot flies itself and levels in face()
	obj->mtype.phys_info.flags |= PF_USES_THRUST;

	reset_loadout(b);
	obj->shields = bot_player(b)->shields;
	reset_navigation(b);
	b->stuck_pos = obj->pos;
	b->target = -1;

	if (net_game())
		multi_send_reappear_for(b->pnum);
	create_player_appearance_effect(obj);
}

static void strip_level(void)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_ROBOT || Objects[i].type == OBJ_CNTRLCEN)
			obj_delete(i);

	Control_center_present = 0;
}

static void add_bot(int pnum)
{
	duel_bot *b = &Bots[Bot_count++];
	int p;

	memset(b, 0, sizeof(*b));
	b->pnum = pnum;
	b->target = -1;
	for (p = 0; p < MAX_PLAYERS; p++)
		b->est_shields[p] = StartingShields;
	start_epoch(&b->lead, Brain.lead, DUEL_LEAD_ARMS, DUEL_LEAD_EPOCH);
	start_epoch(&b->range, Brain.range, DUEL_RANGE_ARMS, DUEL_RANGE_EPOCH);
	start_epoch(&b->flight, Brain.flight, DUEL_FLIGHT_ARMS, DUEL_FLIGHT_EPOCH);
}

// team anarchy: each CPU joins whichever side is short
static void balance_team(int pnum)
{
	int i, red = 0, blue = 0;

	if (!(Game_mode & GM_TEAM))
		return;

	for (i = 0; i < pnum; i++)
		if (Players[i].connected)
			(Netgame.team_vector & (1 << i)) ? red++ : blue++;

	if (red < blue)
		Netgame.team_vector |= (1 << pnum);
	else
		Netgame.team_vector &= ~(1 << pnum);
}

static void seat_cpu(int pnum)
{
	name_cpu(pnum);
	Players[pnum].connected = CONNECT_PLAYING;
	Players[pnum].net_kills_total = 0;
	Players[pnum].net_killed_total = 0;
	Players[pnum].score = 0;

	// no address: every send path checks multi_cpu_slot() instead
	memset(&Netgame.players[pnum], 0, sizeof(Netgame.players[pnum]));
	memcpy(Netgame.players[pnum].callsign, Players[pnum].callsign, CALLSIGN_LEN + 1);
	Netgame.players[pnum].connected = CONNECT_PLAYING;
	Netgame.players[pnum].color = pnum % MAX_PLAYERS;
	Netgame.players[pnum].missilecolor = Netgame.players[pnum].color;
	Netgame.players[pnum].LastPacketTime = timer_query();
	Netgame.CpuSlots |= (1 << pnum);
	balance_team(pnum);
}

// host, as the game starts: CPUs take the seats after the humans already in
void duelbot_claim_net_slots(void)
{
	int want = min(Netgame.CpuCount, Netgame.max_numplayers - N_players);

	memset(Bots, 0, sizeof(Bots));
	memset(Dying_since, 0, sizeof(Dying_since));
	Bot_count = 0;
	Netgame.CpuSlots = 0;

	if (Game_mode & GM_MULTI_COOP)
		return;

	while (want-- > 0 && N_players < MAX_PLAYERS)
	{
		seat_cpu(N_players);
		add_bot(N_players);
		N_players++;
	}

	Netgame.numplayers = N_players;
}

// host, a human wants a seat in a full game: the last CPU gives it up
int duelbot_drop_one(void)
{
	int i;

	for (i = Bot_count - 1; i >= 0; i--)
		if (Bots[i].active)
		{
			int pnum = Bots[i].pnum;

			Bots[i].active = 0;
			Netgame.CpuSlots &= ~(1 << pnum);
			return pnum;
		}

	return -1;
}

void duelbot_release_slot(int pnum)
{
	duel_bot *b = bot_for_pnum(pnum);

	if (b)
		b->active = 0;
}

void duelbot_new_match(void)
{
	int i;

	memset(Bots, 0, sizeof(Bots));
	memset(Brain.heat, 0, sizeof(Brain.heat));		// maps change; habits don't
	memset(Dying_since, 0, sizeof(Dying_since));
	Bot_count = 0;

	if (!Bench_seconds)
		return;

	for (i = 0; i < 2; i++)
		add_bot(DUEL_BOT_PNUM + i);
}

static void claim_slot(duel_bot *b)
{
	if (!net_game())
	{
		name_cpu(b->pnum);
		Players[b->pnum].connected = CONNECT_PLAYING;
	}

	b->active = 1;
	b->respawn_at = 0;
	b->dying_until = 0;
	spawn_bot(b);
}

void duelbot_level_start(void)
{
	int i;

	if (Pilot_on)
		init_pilot();
	memset(Foes, 0, sizeof(Foes));		// a new match, a fresh read on everyone
	memset(Visited, 0, sizeof(Visited));
	Bench_started_at = GameTime64;
	Bench_next_report = GameTime64 + DUEL_BENCH_REPORT;

	if (!duelbot_running())
		return;

	if (Game_mode & GM_DUEL)
		strip_level();		// the bench: an empty arena
	memset(Dying_since, 0, sizeof(Dying_since));

	for (i = 0; i < Bot_count; i++)
		if (!net_game() || multi_cpu_slot(Bots[i].pnum))
			claim_slot(&Bots[i]);
}

//	-------------------------------------------------------------------------
//	Score
//	-------------------------------------------------------------------------

// the kill counts now; the blast comes DUEL_DEATH_TIME later, as for a human ship
static void start_dying(duel_bot *b, object *obj, const object *killer)
{
	int killer_objnum = (killer && (killer->type == OBJ_PLAYER || killer->type == OBJ_GHOST)) ? killer - Objects : obj - Objects;

	b->dying_until = GameTime64 + DUEL_DEATH_TIME;
	b->fusion_charge = 0;
	Players[b->pnum].killer_objnum = killer_objnum;

	// a netgame scores it through the kill message, which also reaches duelbot_note_kill()
	if (net_game())
		multi_send_kill_for(b->pnum, killer_objnum);
	else
		duelbot_note_kill(Objects[killer_objnum].id, b->pnum);

	digi_play_sample(SOUND_HUD_KILL, F3_0);
	vm_vec_zero(&obj->mtype.phys_info.thrust);
	vm_vec_zero(&obj->mtype.phys_info.rotthrust);
}

// the spin dead_player_frame() gives a human wreck: fast at first, winding down to the blast
static void tumble(const duel_bot *b, object *obj)
{
	fix left = max(0, (fix)(b->dying_until - GameTime64));

	obj->mtype.phys_info.rotvel.x = left / 4;
	obj->mtype.phys_info.rotvel.y = left / 2;
	obj->mtype.phys_info.rotvel.z = left / 3;
}

static void finish_dying(duel_bot *b, object *obj)
{
	b->dying_until = 0;
	b->respawn_at = GameTime64 + DUEL_RESPAWN_TIME;

	b->blast_at = GameTime64;

	// the same order a human death runs in: spew, tell everyone, blow, ghost
	drop_player_eggs(obj);
	if (net_game())
		multi_send_player_explode_for(b->pnum, MULTI_PLAYER_EXPLODE);
	explode_badass_player(obj);
	explode_object(obj, 0);		// fireball and flying hull pieces
	obj->flags &= ~OF_SHOULD_BE_DEAD;	// the slot's ship lives on as a ghost
	multi_make_player_ghost(b->pnum);
}

#define TAUNT_ODDS		(32768 / 5)
#define TAUNT_PER_KILL_LEAD	(32768 / 20)	// +5% per kill ahead
#define TAUNT_MAX_ODDS		(32768 * 3 / 5)

static const char *const Taunts[] = {
	"Try harder.",
	"Was that your best attempt?",
	"You flew right into that.",
	"Nice ship... or was a nice ship.",
	"Too slow",
	"Try to dodge next time.",
	"Lol!",
	"Check your six.",
	"How many times will you try that?",
	"I saw that coming.",
	"That's one more for me.",
	"Awkward...",
	"Lower my difficulty next match",
	"Boring.",
	"Take notes.",
};

// a fifth of the time, more when it's running away with the match
static int taunt_odds(const duel_bot *k)
{
	int i, lead = 99;

	for (i = 0; i < N_players; i++)
		if (i != k->pnum && Players[i].connected)
			lead = min(lead, Players[k->pnum].KillGoalCount - Players[i].KillGoalCount);
	if (lead == 99 || lead <= 0)
		return TAUNT_ODDS;
	return min(TAUNT_ODDS + lead * TAUNT_PER_KILL_LEAD, TAUNT_MAX_ODDS);
}

// a fifth of the time, rub it in
static void maybe_taunt(const duel_bot *k)
{
	const char *line;

	if (d_rand() >= taunt_odds(k))
		return;
	line = Taunts[d_rand() % (sizeof(Taunts) / sizeof(Taunts[0]))];
	if (journaling(k))
		journal("TAUNT", "", "\"%s\"", line);
	if (net_game())
		multi_send_message_for(k->pnum, line);
	else
		HUD_init_message(HM_MULTI, "%s: %s", Players[k->pnum].callsign, line);
}

static void forget_victim(duel_bot *b, int killed)
{
	b->est_shields[killed] = StartingShields;	// a fresh ship next time
	if (b->target != killed)
		return;
	b->target = -1;
	b->seen_at = 0;
}

// every kill, wherever it was decided: on the host it arrives via multi_compute_kill()
// the read it has on you so far, as a human would put it
static void journal_read(int foe)
{
	const duel_foe *f = &Foes[foe];
	int w, fav = 0;

	if (!f->samples)
		return;
	for (w = 1; w < MAX_PRIMARY_WEAPONS; w++)
		if (f->gun_use[w] > f->gun_use[fav])
			fav = w;
	journal("READ", "on you", "you fight at ~%d units, favour %s; after my shots you jink %s and %s",
			f2i(f->fight_dist), Gun_names[fav], f->dodge_r >= 0 ? "right" : "left", f->dodge_u >= 0 ? "up" : "down");
}

// a suicide takes a point back: at the end of a match that matters
static void note_suicide(int pnum)
{
	if (!Dedicated || pnum == Player_num)
		return;
	journal("SCORE", "your suicide", "costs you a point: now %d-%d", Players[Player_num].KillGoalCount, Players[pnum].KillGoalCount);
	if (match_tense() || Players[pnum].KillGoalCount >= Match_points - 2)
		multi_send_message_for(Pilot.pnum, "Suicide costs a point. Not over yet.");
}

static void journal_kill(int killer, int killed)
{
	if (!Journal || !Pilot_on)
		return;
	if (killer == killed)
		note_suicide(killed);
	if (killer == Player_num && killed != Player_num)
		journal("KILL", Players[killed].callsign, "my shields %d, score %d-%d", f2i(Players[Player_num].shields),
				Players[Player_num].KillGoalCount + 1, Players[killed].KillGoalCount);
	else if (killed == Player_num)
		journal("DIED", killer >= 0 && killer != Player_num ? Players[killer].callsign : "myself",
				"was in %s, score %d-%d", Mode_names[Pilot.mode], Players[Player_num].KillGoalCount,
				journal_foe_score() + (killer >= 0 && killer != Player_num));
	else
		return;
	journal_read(killer == Player_num ? killed : killer);
}

// a spew with real guns in it is worth sitting on; they have to come back for it
static int spew_worth_guarding(int killed)
{
	const player *v = &Players[killed];
	int i, missiles = 0;

	for (i = 0; i < MAX_SECONDARY_WEAPONS; i++)
		missiles += v->secondary_ammo[i];
	return (v->primary_weapon_flags & ~HAS_LASER_FLAG) || missiles >= 4;
}

static void after_kill(duel_bot *k, int killed)
{
	const object *victim = &Objects[Players[killed].objnum];

	if (Players[k->pnum].shields < DUEL_GUARD_SHIELDS || !spew_worth_guarding(killed))
		return;
	start_camp(k, victim->segnum, &victim->pos, DUEL_GUARD_TIME);
	if (journaling(k))
		journal("GUARD", "weapon guarding", "your spew has guns in it: you'll have to come back for it");
}

void duelbot_note_kill(int killer, int killed)
{
	duel_bot *k = killer >= 0 ? bot_for_pnum(killer) : NULL;
	duel_bot *v = bot_for_pnum(killed);
	int i;

	if (!cpu_thinking() || killed < 0 || killed >= MAX_PLAYERS)
		return;

	journal_kill(killer, killed);
	if (v)
		v->stats.deaths++;
	else
		Dying_since[killed] = GameTime64;	// its own machine blows it up shortly

	if (!k && Pilot_on && killer == Player_num)
		k = &Pilot;		// the host's own ship, flown as the CPU
	if (k && k != v && killed != k->pnum)
	{
		k->stats.kills++;
		maybe_taunt(k);
		after_kill(k, killed);
	}

	Foes[killed].died_at = GameTime64;
	Foes[killed].haunt_open = 1;
	for (i = 0; i < Bot_count; i++)
		forget_victim(&Bots[i], killed);
	if (Pilot_on)
		forget_victim(&Pilot, killed);
}

// damage from a ship that blew up this very frame is its death blast
static int dying_ship(const object *ship)
{
	duel_bot *owner = bot_for_pnum(ship->id);

	return owner && owner->blast_at == GameTime64;
}

// splash from a missile or a dying ship: the tracker names the source
void duelbot_blast_hit(const object *source)
{
	Hit_weapon_id = source && source->type == OBJ_WEAPON ? source->id + 1 : 0;
	Hit_is_blast = 1;
}

// the tracker credits kills from these, as it does for human ships
static void report_damage(const object *obj, const object *killer, fix damage)
{
	fix left = max(Players[obj->id].shields - damage, 0);
	int weapon = Hit_weapon_id, blast = Hit_is_blast || !weapon;

	Hit_weapon_id = 0;
	Hit_is_blast = 0;
	if (!(Game_mode & GM_MULTI) || !killer)
		return;
	udp_tracker_send_damage(obj->id, damage, left, killer->type, killer->type == OBJ_PLAYER ? killer->id : 0,
							blast ? DAMAGE_BLAST : DAMAGE_WEAPON, weapon ? weapon - 1 : 0);
}

int duelbot_take_damage(object *obj, object *killer, fix damage)
{
	duel_bot *b;
	player *p;

	if (!duelbot_is_bot(obj))
		return 0;

	b = bot_for_pnum(obj->id);
	p = bot_player(b);

	if (b->respawn_at || b->dying_until || (obj->flags & OF_SHOULD_BE_DEAD))
		return 1;

	if (p->flags & PLAYER_FLAGS_INVULNERABLE)
		return 1;

	// a hit tells a pilot roughly where it came from
	if (killer && killer->type == OBJ_PLAYER && killer != obj && !b->visible)
	{
		b->last_seen_pos = killer->pos;
		b->last_seen_seg = killer->segnum;
		b->seen_at = GameTime64;
		b->next_path = 0;
	}

	if (killer && killer->type == OBJ_PLAYER && killer != obj && dying_ship(killer))
		b->stats.blast_taken += damage;
	report_damage(obj, killer, damage);

	p->shields -= damage;
	obj->shields = p->shields;
	b->range.taken += damage;
	b->stats.taken += damage;
	b->strafe_flip_at = 0;		// hit: break the pattern
	b->camp_until = 0;		// found out: stop sitting still

	if (p->shields > 0)
	{
		digi_link_sound_to_pos(SOUND_PLAYER_GOT_HIT, obj->segnum, 0, &obj->pos, 0, F1_0);
		return 1;
	}

	start_dying(b, obj, killer);
	return 1;
}

//	-------------------------------------------------------------------------
//	Powerups
//	-------------------------------------------------------------------------

static int add_secondary(duel_bot *b, int index, int count)
{
	player *p = bot_player(b);

	if (p->secondary_ammo[index] >= Secondary_ammo_max[index])
		return 0;

	p->secondary_ammo[index] = min(p->secondary_ammo[index] + count, Secondary_ammo_max[index]);
	p->secondary_weapon_flags |= HAS_FLAG(index);
	return 1;
}

static int add_vulcan_ammo(duel_bot *b, int amount)
{
	player *p = bot_player(b);

	if (p->primary_ammo[VULCAN_INDEX] >= VULCAN_AMMO_MAX)
		return 0;

	p->primary_ammo[VULCAN_INDEX] = min(p->primary_ammo[VULCAN_INDEX] + amount, VULCAN_AMMO_MAX);
	return 1;
}

static int add_primary(duel_bot *b, int index)
{
	player *p = bot_player(b);

	if (p->primary_weapon_flags & HAS_FLAG(index))
		return index == VULCAN_INDEX ? add_vulcan_ammo(b, VULCAN_WEAPON_AMMO_AMOUNT) : 0;

	p->primary_weapon_flags |= HAS_FLAG(index);
	if (index == VULCAN_INDEX)
		add_vulcan_ammo(b, VULCAN_WEAPON_AMMO_AMOUNT);
	return 1;
}

static int add_capped(fix *value, fix amount, fix cap)
{
	if (*value >= cap)
		return 0;

	*value = min(*value + amount, cap);
	return 1;
}

static int has_primary(const duel_bot *b, int index)
{
	return (Players[b->pnum].primary_weapon_flags & HAS_FLAG(index)) != 0;
}

// worth of a pickup to this bot right now; 0 = not wanted
static int powerup_value(const duel_bot *b, int id)
{
	player *p = &Players[b->pnum];

	switch (id)
	{
		case POW_SHIELD_BOOST:		return p->shields < DUEL_TOPPED_UP ? 2 + f2i(DUEL_TOPPED_UP - p->shields) / 10 : 0;
		case POW_ENERGY:			return p->energy < DUEL_TOPPED_UP ? (p->energy < DUEL_SAVE_ENERGY ? 8 : 4) : 0;
		case POW_LASER:				return p->laser_level < MAX_LASER_LEVEL ? 8 - 2 * p->laser_level : 0;
		case POW_QUAD_FIRE:			return (p->flags & PLAYER_FLAGS_QUAD_LASERS) ? 0 : 8;
		case POW_VULCAN_WEAPON:		return has_primary(b, VULCAN_INDEX) ? 0 : 8;	// owned: the game won't let it be picked up
		case POW_SPREADFIRE_WEAPON:	return has_primary(b, SPREADFIRE_INDEX) ? 0 : 5;
		case POW_PLASMA_WEAPON:		return has_primary(b, PLASMA_INDEX) ? 0 : 12;
		case POW_FUSION_WEAPON:		return has_primary(b, FUSION_INDEX) ? 0 : 10;
		case POW_VULCAN_AMMO:		return has_primary(b, VULCAN_INDEX) && p->primary_ammo[VULCAN_INDEX] < VULCAN_AMMO_MAX ? 4 : 0;
		case POW_MISSILE_1:
		case POW_MISSILE_4:			return p->secondary_ammo[CONCUSSION_INDEX] < Secondary_ammo_max[CONCUSSION_INDEX] ? 3 : 0;
		case POW_HOMING_AMMO_1:
		case POW_HOMING_AMMO_4:		return p->secondary_ammo[HOMING_INDEX] < Secondary_ammo_max[HOMING_INDEX] ? 9 : 0;
		case POW_PROXIMITY_WEAPON:	return p->secondary_ammo[PROXIMITY_INDEX] < Secondary_ammo_max[PROXIMITY_INDEX] ? 4 : 0;
		case POW_SMARTBOMB_WEAPON:	return p->secondary_ammo[SMART_INDEX] < Secondary_ammo_max[SMART_INDEX] ? 16 : 0;	// the duel-decider
		case POW_MEGA_WEAPON:		return p->secondary_ammo[MEGA_INDEX] < Secondary_ammo_max[MEGA_INDEX] ? 10 : 0;
		case POW_CLOAK:				return (p->flags & PLAYER_FLAGS_CLOAKED) ? 0 : 5;
		case POW_INVULNERABILITY:	return (p->flags & PLAYER_FLAGS_INVULNERABLE) ? 0 : 9;
		default:					return 0;
	}
}

static int grant_flag(duel_bot *b, int flag, fix64 *since)
{
	player *p = bot_player(b);

	if (p->flags & flag)
		return 0;

	p->flags |= flag;
	if (since)
		*since = GameTime64;
	return 1;
}

static int apply_powerup(duel_bot *b, int id)
{
	player *p = bot_player(b);
	fix boost = 3*F1_0 + 3*F1_0*(NDL - Difficulty_level);

	switch (id)
	{
		case POW_SHIELD_BOOST:		return add_capped(&p->shields, boost, MAX_SHIELDS);
		case POW_ENERGY:			return add_capped(&p->energy, boost, MAX_ENERGY);
		case POW_LASER:
			if (p->laser_level >= MAX_LASER_LEVEL)
				return 0;
			p->laser_level++;
			return 1;
		case POW_QUAD_FIRE:			return grant_flag(b, PLAYER_FLAGS_QUAD_LASERS, NULL);
		case POW_VULCAN_WEAPON:		return add_primary(b, VULCAN_INDEX);
		case POW_SPREADFIRE_WEAPON:	return add_primary(b, SPREADFIRE_INDEX);
		case POW_PLASMA_WEAPON:		return add_primary(b, PLASMA_INDEX);
		case POW_FUSION_WEAPON:		return add_primary(b, FUSION_INDEX);
		case POW_VULCAN_AMMO:		return add_vulcan_ammo(b, VULCAN_AMMO_AMOUNT);
		case POW_MISSILE_1:			return add_secondary(b, CONCUSSION_INDEX, 1);
		case POW_MISSILE_4:			return add_secondary(b, CONCUSSION_INDEX, 4);
		case POW_HOMING_AMMO_1:		return add_secondary(b, HOMING_INDEX, 1);
		case POW_HOMING_AMMO_4:		return add_secondary(b, HOMING_INDEX, 4);
		case POW_PROXIMITY_WEAPON:	return add_secondary(b, PROXIMITY_INDEX, 4);
		case POW_SMARTBOMB_WEAPON:	return add_secondary(b, SMART_INDEX, 1);
		case POW_MEGA_WEAPON:		return add_secondary(b, MEGA_INDEX, 1);
		case POW_CLOAK:
			if (!grant_flag(b, PLAYER_FLAGS_CLOAKED, &p->cloak_time))
				return 0;
			if (net_game())
				multi_send_cloak_for(b->pnum);
			return 1;
		case POW_INVULNERABILITY:	return grant_flag(b, PLAYER_FLAGS_INVULNERABLE, &p->invulnerable_time);
		default:					return 0;
	}
}

int duelbot_pickup(object *obj, object *powerup)
{
	duel_bot *b;

	if (!duelbot_is_bot(obj))
		return 0;

	b = bot_for_pnum(obj->id);
	if (b->respawn_at || !apply_powerup(b, powerup->id))
		return 0;

	digi_link_sound_to_pos(Powerup_info[powerup->id].hit_sound, obj->segnum, 0, &obj->pos, 0, F1_0);
	b->next_path = 0;
	return 1;
}

static void expire_powers(duel_bot *b)
{
	player *p = bot_player(b);

	if ((p->flags & PLAYER_FLAGS_CLOAKED) && GameTime64 > p->cloak_time + CLOAK_TIME_MAX)
	{
		p->flags &= ~PLAYER_FLAGS_CLOAKED;
		if (net_game())
			multi_send_decloak_for(b->pnum);
	}

	if ((p->flags & PLAYER_FLAGS_INVULNERABLE) && GameTime64 > p->invulnerable_time + INVULNERABLE_TIME_MAX)
		p->flags &= ~PLAYER_FLAGS_INVULNERABLE;
}

static void refuel(duel_bot *b, const object *obj)
{
	player *p = bot_player(b);

	if (Segments[obj->segnum].special != SEGMENT_IS_FUELCEN || p->energy >= INITIAL_ENERGY)
		return;

	add_capped(&p->energy, fixmul(FrameTime, DUEL_REFUEL_RATE), INITIAL_ENERGY);
}

//	-------------------------------------------------------------------------
//	Perception
//	-------------------------------------------------------------------------

static int clear_line(const object *from, const vms_vector *to, fix rad)
{
	fvi_query fq;
	fvi_info hit;

	fq.p0 = (vms_vector *)&from->pos;
	fq.p1 = (vms_vector *)to;
	fq.startseg = from->segnum;
	fq.rad = rad;
	fq.thisobjnum = from - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = rad ? 0 : FQ_TRANSWALL;	// eyes see through grates, ships don't

	return find_vector_intersection(&fq, &hit) == HIT_NONE;
}

// the rear view, used in the middle of hallways while hunting
static int rear_glancing(const duel_bot *b)
{
	return b->mode == MODE_HUNT && (GameTime64 + b->rear_phase) % DUEL_REAR_PERIOD < DUEL_REAR_GLANCE;
}

// a pilot sees what's on screen, plus what's close enough to feel
static int in_view(const duel_bot *b, const object *bot, const object *enemy, fix dist)
{
	vms_vector to_enemy;
	fix dot;

	if (dist < DUEL_PERIPHERAL || GameTime64 - b->seen_at < DUEL_TRACK_GRACE)
		return 1;

	vm_vec_sub(&to_enemy, (vms_vector *)&enemy->pos, (vms_vector *)&bot->pos);
	vm_vec_normalize(&to_enemy);
	dot = vm_vec_dot(&to_enemy, (vms_vector *)&bot->orient.fvec);

	return dot > DUEL_FOV_DOT || (rear_glancing(b) && dot < DUEL_REAR_DOT);
}

static int can_see(const duel_bot *b, const object *bot, const object *enemy)
{
	fix dist = vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&enemy->pos);

	if (dist > DUEL_SIGHT_RANGE || !in_view(b, bot, enemy, dist))
		return 0;

	if ((Players[enemy->id].flags & PLAYER_FLAGS_CLOAKED) && dist > DUEL_CLOAK_SIGHT)
		return 0;

	return clear_line(bot, &enemy->pos, 0);
}

static void remember(duel_bot *b, const vms_vector *pos, int segnum)
{
	b->last_seen_pos = *pos;
	b->last_seen_seg = segnum;
	b->seen_at = GameTime64;
}

// full keys on all three axes: nothing a ship does accelerates harder
static fix max_ship_accel(void)
{
	fix per_tick = fixdiv(Player_ship->max_thrust, Player_ship->mass);

	return fixmul(per_tick, i2f(64)) * 2;
}

static void cap_magnitude(vms_vector *v, fix cap)
{
	fix mag = vm_vec_mag(v);

	if (mag > cap)
		vm_vec_scale(v, fixdiv(cap, mag));
}

// smoothed acceleration, observed frame to frame; bumps are capped out
static void track_accel(duel_bot *b, const object *enemy)
{
	vms_vector accel;

	if (b->enemy_objnum != enemy - Objects || b->sighted_at == GameTime64)
	{
		b->enemy_objnum = enemy - Objects;
		b->enemy_vel = enemy->mtype.phys_info.velocity;
		vm_vec_zero(&b->enemy_accel);
		return;
	}

	vm_vec_sub(&accel, (vms_vector *)&enemy->mtype.phys_info.velocity, &b->enemy_vel);
	vm_vec_scale(&accel, fixdiv(F1_0, FrameTime));
	b->enemy_vel = enemy->mtype.phys_info.velocity;

	cap_magnitude(&accel, max_ship_accel());
	vm_vec_scale(&b->enemy_accel, F1_0 - DUEL_ACCEL_SMOOTH);
	vm_vec_scale_add2(&b->enemy_accel, &accel, DUEL_ACCEL_SMOOTH);
}

static void note_haunt(duel_foe *f, int segnum)
{
	int i, slot = 0;

	f->haunt_open = 0;
	for (i = 0; i < DUEL_HAUNTS; i++)
	{
		if (f->haunt_hits[i] && f->haunt_seg[i] == segnum)
		{
			f->haunt_hits[i] = min(f->haunt_hits[i] + 1, 255);
			return;
		}
		if (f->haunt_hits[i] < f->haunt_hits[slot])
			slot = i;
	}
	f->haunt_seg[slot] = segnum;
	f->haunt_hits[slot] = 1;
}

static int favourite_haunt(const duel_foe *f)
{
	int i, best = -1;

	for (i = 0; i < DUEL_HAUNTS; i++)
		if (f->haunt_hits[i] >= DUEL_HAUNT_SURE && (best < 0 || f->haunt_hits[i] > f->haunt_hits[best]))
			best = i;
	return best < 0 ? -1 : f->haunt_seg[best];
}

static void update_sight(duel_bot *b, const object *bot, const object *enemy)
{
	int visible = enemy && can_see(b, bot, enemy);

	if (visible && !b->visible && GameTime64 - b->seen_at > DUEL_REACQUIRE)
		b->sighted_at = GameTime64;
	if (visible && !b->visible && GameTime64 - b->seen_at > DUEL_NEW_FIGHT)
		b->held_since = 0;		// new fight: free to re-pick the gun

	if (!visible && b->target >= 0)	// out of sight they find shields
		b->est_shields[b->target] = min(StartingShields, b->est_shields[b->target] + fixmul(FrameTime, DUEL_SHIELD_REGEN));
	if (!visible && b->visible)
		b->escape_vel = b->enemy_vel;
	if (visible && Foes[enemy->id].haunt_open && GameTime64 - Foes[enemy->id].died_at < DUEL_HAUNT_WINDOW)
		note_haunt(&Foes[enemy->id], enemy->segnum);
	if (visible)
	{
		remember(b, &enemy->pos, enemy->segnum);
		note_heat(enemy->segnum);
		track_accel(b, enemy);
		b->camp_until = 0;
	}

	b->visible = visible;
}

static int threat_noticed(const object *bot, const object *weapon, const vms_vector *rel)
{
	vms_vector to_weapon = *rel;

	if (Weapon_info[weapon->id].homing_flag)
		return 1;		// the human gets a lock warning too

	if (vm_vec_normalize(&to_weapon) < DUEL_NEAR_THREAT)
		return 1;

	vm_vec_negate(&to_weapon);
	return vm_vec_dot(&to_weapon, (vms_vector *)&bot->orient.fvec) > -F1_0/4;
}

// already sliding across its line: keep going, a ship can't reverse in time
static int keep_sliding(const object *bot, const vms_vector *line, fix room, vms_vector *away)
{
	vms_vector slide = bot->mtype.phys_info.velocity;

	vm_vec_scale_add2(&slide, (vms_vector *)line, -vm_vec_dot(&slide, (vms_vector *)line));
	if (vm_vec_normalize(&slide) < DUEL_SLIDE_SPEED || free_distance(bot, &slide) < room)
		return 0;
	*away = slide;
	return 1;
}

// the side step with the most room: a juke into a wall is a juke into the blast
static void best_sidestep(const object *bot, const vms_vector *line, vms_vector *best)
{
	vms_vector p1, p2, cand;
	fix best_score = -1;
	int i;

	vm_vec_cross(&p1, (vms_vector *)line, (vms_vector *)&bot->orient.uvec);
	if (vm_vec_normalize(&p1) < F1_0/16)
	{
		vm_vec_cross(&p1, (vms_vector *)line, (vms_vector *)&bot->orient.rvec);
		vm_vec_normalize(&p1);
	}
	vm_vec_cross(&p2, (vms_vector *)line, &p1);
	vm_vec_normalize(&p2);

	for (i = 0; i < DUEL_SIDESTEPS; i++)
	{
		fix a = i * (F1_0 / DUEL_SIDESTEPS), sn, cs, score;

		fix_sincos(a, &sn, &cs);
		vm_vec_copy_scale(&cand, &p1, cs);
		vm_vec_scale_add2(&cand, &p2, sn);
		score = min(free_distance(bot, &cand), DUEL_SIDESTEP_ROOM) +
				max(0, vm_vec_dot(&cand, (vms_vector *)&bot->mtype.phys_info.velocity));	// momentum: speed already had
		if (score > best_score)
		{
			best_score = score;
			*best = cand;
		}
	}
}

// a homer only holds lock inside a narrow cone off its nose. Running away
// keeps you dead centre; cutting in toward it at an angle grows the angle
// fastest (sideways speed over a shrinking distance), so the lock dies.
static fix evade_homer(const object *bot, const object *weapon, const vms_vector *rel, vms_vector *away)
{
	vms_vector dir = weapon->mtype.phys_info.velocity, side;
	fix speed = vm_vec_normalize(&dir);
	fix dist = vm_vec_mag((vms_vector *)rel);

	if (speed < F1_0 || dist > DUEL_DODGE_RANGE || vm_vec_dot((vms_vector *)rel, &dir) <= 0)
		return 0;
	if (fixdiv(dist, speed) > DUEL_MISSILE_TIME)
		return 0;
	if (!keep_sliding(bot, &dir, DUEL_SIDESTEP_ROOM / 2, &side))
		best_sidestep(bot, &dir, &side);
	vm_vec_copy_scale(away, &side, F1_0);
	vm_vec_scale_add2(away, &dir, -DUEL_HOMER_CUT);	// into it, at an angle
	vm_vec_normalize(away);
	return F1_0*4;
}


// a missile flying past still explodes on the wall beside you: get clear of where it lands
static fix blast_near(const object *bot, const object *weapon, fix blast, vms_vector *away)
{
	fvi_query fq;
	fvi_info hit;
	vms_vector end;
	fix dist;

	vm_vec_scale_add(&end, (vms_vector *)&weapon->pos, (vms_vector *)&weapon->mtype.phys_info.velocity, DUEL_MISSILE_TIME);
	fq.p0 = (vms_vector *)&weapon->pos;
	fq.p1 = &end;
	fq.startseg = weapon->segnum;
	fq.rad = weapon->size;
	fq.thisobjnum = weapon - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;
	if (find_vector_intersection(&fq, &hit) != HIT_WALL)
		return 0;

	vm_vec_sub(away, (vms_vector *)&bot->pos, &hit.hit_pnt);
	dist = vm_vec_normalize(away);
	if (dist > blast + bot->size + DUEL_DODGE_MARGIN)
		return 0;
	if (dist < F1_0/16)
		*away = bot->orient.uvec;
	return F1_0*3;
}

// how hard (0 = not at all) and which way to step off this shot's line
static fix threat_dodge(const object *bot, const object *weapon, vms_vector *away)
{
	vms_vector rel, dir;
	fix speed, along, miss, t, margin, strength;
	fix blast = Weapon_info[weapon->id].damage_radius;

	vm_vec_sub(&rel, (vms_vector *)&bot->pos, (vms_vector *)&weapon->pos);
	if (vm_vec_mag(&rel) > DUEL_DODGE_RANGE)
		return 0;
	if (Weapon_info[weapon->id].homing_flag)
		return evade_homer(bot, weapon, &rel, away);
	if (blast && (strength = blast_near(bot, weapon, blast, away)))
		return strength;

	dir = weapon->mtype.phys_info.velocity;
	speed = vm_vec_normalize(&dir);
	if (speed < F1_0)
		return 0;

	along = vm_vec_dot(&rel, &dir);
	if (along <= 0)
		return 0;

	t = fixdiv(along, speed);
	if (t > (blast ? DUEL_MISSILE_TIME : DUEL_DODGE_TIME))
		return 0;

	vm_vec_scale_add(away, &rel, &dir, -along);
	margin = bot->size + weapon->size + max(DUEL_DODGE_MARGIN, blast);
	miss = vm_vec_normalize(away);
	if (miss >= margin || !threat_noticed(bot, weapon, &rel))
		return 0;

	if (blast && keep_sliding(bot, &dir, blast, away))
		;
	else if (miss < F1_0/16 || (blast && free_distance(bot, away) < blast + bot->size))
		best_sidestep(bot, &dir, away);

	return F1_0 + 2 * (F1_0 - fixdiv(t, DUEL_DODGE_TIME));
}

// commit to a dodge for a moment; only a bigger threat changes it
// missiles, homers and smart blobs: anything a pilot sees coming and must dodge
static int seeks_or_blasts(const object *weapon)
{
	return Weapon_info[weapon->id].damage_radius || Weapon_info[weapon->id].homing_flag;
}

static void journal_dodge(const object *weapon, fix strength)
{
	if (Weapon_info[weapon->id].homing_flag)
		journal("DODGE", "cut in at homer", "angling in at it: its lock cone can't keep up");
	else if (Weapon_info[weapon->id].damage_radius)
		journal("DODGE", "sidestep missile", "to the side with room, clear of the blast and the wall it hits");
}

static void consider_dodge(duel_bot *b, const object *bot, const object *weapon)
{
	vms_vector away;
	fix strength = threat_dodge(bot, weapon, &away);

	if (!strength)
		return;
	if (!seeks_or_blasts(weapon) &&
		((weapon->signature * 2654435761u) >> 17) >= (unsigned)skill(b)->dodge_odds)
		return;		// any pilot sees a missile coming; lasers are a coin flip

	// a swarm of blobs: one escape line that clears them all, not the first one's
	if (Weapon_info[weapon->id].homing_flag && GameTime64 < b->dodge_until && strength == b->dodge_strength)
	{
		vm_vec_add2(&b->dodge_dir, &away);
		if (vm_vec_normalize(&b->dodge_dir) < F1_0/16)
			b->dodge_dir = away;
		return;
	}
	if (GameTime64 < b->dodge_until && strength <= b->dodge_strength)
		return;

	if (journaling(b) && GameTime64 >= b->dodge_until + DUEL_DODGE_HOLD)
		journal_dodge(weapon, strength);		// once per threat, not every re-plan
	b->dodge_dir = away;
	b->dodge_strength = strength;
	b->dodge_until = GameTime64 + DUEL_DODGE_HOLD;
}

// its own shots landing: the impact sound has no owner, the shot still does
static int own_noise(const object *bot, const vms_vector *pos)
{
	int i, bot_objnum = bot - Objects;

	for (i = 0; i <= Highest_object_index; i++)
	{
		const object *obj = &Objects[i];

		if (obj->type == OBJ_WEAPON && obj->ctype.laser_info.parent_num == bot_objnum &&
			vm_vec_dist((vms_vector *)&obj->pos, (vms_vector *)pos) < DUEL_SELF_NOISE)
			return 1;
	}
	return 0;
}

// a sound the bot heard: a lead on where somebody is (source -1: unknown)
static void hear(duel_bot *b, const object *bot, int source, const vms_vector *pos, int segnum, fix reach)
{
	fix dist = vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)pos);

	if (b->visible || source == b->pnum || dist > min(reach, DUEL_HEAR_RANGE))
		return;
	if (source < 0 && (dist < DUEL_SELF_NOISE || own_noise(bot, pos)))
		return;			// a door it opened, a pickup it took, its own shot landing
	if (source >= 0 && !targetable(b, source))
		return;
	if (source >= 0)
		b->target = source;
	b->heard_at = GameTime64;
	if (journaling(b) && GameTime64 - Journal_heard_at > JOURNAL_HEAR_GAP)
	{
		Journal_heard_at = GameTime64;
		journal("LISTEN", "investigate", "heard %s %d units away", source >= 0 ? "you" : "a shot or door", f2i(dist));
	}
	remember(b, pos, segnum);
}

static void hear_all(int source, const vms_vector *pos, int segnum, fix reach)
{
	int i;

	for (i = 0; i < Bot_count; i++)
		if (Bots[i].active && !Bots[i].respawn_at && !Bots[i].dying_until && bot_object(&Bots[i]))
			hear(&Bots[i], bot_object(&Bots[i]), source, pos, segnum, reach);
	if (Pilot_on && ConsoleObject->type == OBJ_PLAYER)
		hear(&Pilot, ConsoleObject, source, pos, segnum, reach);
}

// every sound tied to an object: a ship's own noises say who made them
void duelbot_hear_object(int objnum, fix reach)
{
	const object *obj;
	int source = -1;

	if (!cpu_thinking() || objnum < 0 || objnum > Highest_object_index)
		return;
	obj = &Objects[objnum];
	if (obj->type == OBJ_PLAYER)
		source = obj->id;
	else if (obj->type == OBJ_WEAPON && obj->ctype.laser_info.parent_type == OBJ_PLAYER &&
			 obj->ctype.laser_info.parent_num >= 0 && obj->ctype.laser_info.parent_num <= Highest_object_index)
		source = Objects[obj->ctype.laser_info.parent_num].id;
	hear_all(source, &obj->pos, obj->segnum, reach);
}

// every sound at a spot (doors, pickups, blasts): where, not who
void duelbot_hear_pos(int segnum, const vms_vector *pos, fix reach)
{
	if (cpu_thinking() && segnum >= 0 && segnum <= Highest_segment_index)
		hear_all(-1, pos, segnum, reach);
}

// a shot heard with nobody in sight: that's who to go after
static void hear_weapon(duel_bot *b, const object *bot, const object *weapon)
{
	int shooter = weapon->ctype.laser_info.parent_num;

	if (b->visible || GameTime64 - weapon->ctype.laser_info.creation_time > DUEL_HEAR_WINDOW)
		return;

	if (vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&weapon->pos) > DUEL_HEAR_RANGE)
		return;

	if (shooter >= 0 && shooter <= Highest_object_index && Objects[shooter].type == OBJ_PLAYER &&
		targetable(b, Objects[shooter].id))
		b->target = Objects[shooter].id;
	remember(b, &weapon->pos, weapon->segnum);
}

static int enemy_shot(const duel_bot *b, const object *weapon)
{
	int shooter = weapon->ctype.laser_info.parent_num;

	if (shooter < 0 || shooter > Highest_object_index || weapon->ctype.laser_info.parent_type != OBJ_PLAYER)
		return 0;
	return Objects[shooter].id != b->pnum && !teammates(Objects[shooter].id, b->pnum);
}

static int primary_for_weapon_id(int id)
{
	int w;

	if (id >= LASER_ID_L1 && id <= LASER_ID_L4)
		return LASER_INDEX;

	for (w = VULCAN_INDEX; w < MAX_PRIMARY_WEAPONS; w++)
		if (Primary_weapon_to_weapon_info[w] == id)
			return w;

	return -1;
}

// a pilot knows what gun is shooting at them from the shots
static void note_enemy_weapon(duel_bot *b, const object *weapon)
{
	int w;

	if (GameTime64 - weapon->ctype.laser_info.creation_time > DUEL_HEAR_WINDOW)
		return;

	w = primary_for_weapon_id(weapon->id);
	if (w < 0)
		return;

	b->enemy_weapon = w;
	b->enemy_weapon_at = GameTime64;
	if (weapon->ctype.laser_info.parent_num >= 0 && Objects[weapon->ctype.laser_info.parent_num].type == OBJ_PLAYER)
		Foes[Objects[weapon->ctype.laser_info.parent_num].id].gun_use[w]++;
}

// one pass over the world: robots out, enemy fire heard and dodged, mines avoided
// follow each missile aimed my way to its end: did it get me?
static void watch_missile(duel_bot *b, const object *bot, const object *weapon)
{
	fix dist = vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&weapon->pos);
	int i, free_slot = -1;

	if (!seeks_or_blasts(weapon) || weapon->id == PROXIMITY_ID || dist > DUEL_DODGE_RANGE)
		return;
	for (i = 0; i < DUEL_WATCHED; i++)
	{
		if (b->incoming[i].sig == weapon->signature)
		{
			b->incoming[i].closest = min(b->incoming[i].closest, dist);
			b->incoming[i].last = dist;
			b->incoming[i].seen = 1;
			return;
		}
		if (!b->incoming[i].sig && free_slot < 0)
			free_slot = i;
	}
	if (free_slot < 0)
		return;
	b->missile_alert_until = GameTime64 + DUEL_MISSILE_ALERT;
	b->incoming[free_slot].sig = weapon->signature;
	b->incoming[free_slot].closest = dist;
	b->incoming[free_slot].last = dist;
	b->incoming[free_slot].reach = Weapon_info[weapon->id].damage_radius + bot->size + weapon->size + DUEL_DODGE_MARGIN;
	b->incoming[free_slot].seen = 1;
}

static void settle_missiles(duel_bot *b)
{
	int i;

	for (i = 0; i < DUEL_WATCHED; i++)
	{
		if (!b->incoming[i].sig)
			continue;
		if (b->incoming[i].seen)
		{
			b->incoming[i].seen = 0;
			continue;
		}
		if (b->incoming[i].last < b->incoming[i].reach)
			b->stats.missiles_taken++;	// where it went off, not how close it passed
		else if (b->incoming[i].closest < b->incoming[i].reach * DUEL_NEAR_MISS)
			b->stats.missiles_dodged++;	// one that was never close wasn't a dodge
		b->incoming[i].sig = 0;
	}
}

static void scan_objects(duel_bot *b, const object *bot, const object *enemy)
{
	int i, bot_objnum = bot - Objects;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *obj = &Objects[i];

		if (obj->type == OBJ_ROBOT)
		{
			obj_delete(i);
			continue;
		}

		if (obj->type != OBJ_WEAPON)
			continue;
		if (obj->ctype.laser_info.parent_num == bot_objnum)
		{
			continue;
		}

		if (!enemy_shot(b, obj))
			continue;		// teammates' fire: neither threat nor news

		hear_weapon(b, bot, obj);
		if (enemy && obj->ctype.laser_info.parent_num == enemy - Objects)
			note_enemy_weapon(b, obj);

		watch_missile(b, bot, obj);
		consider_dodge(b, bot, obj);
	}
	settle_missiles(b);
}

//	-------------------------------------------------------------------------
//	Navigation
//	-------------------------------------------------------------------------

static int side_passable(segment *seg, int side)
{
	int wall_num;

	if (WALL_IS_DOORWAY(seg, side) & WID_FLY_FLAG)
		return 1;

	if (!IS_CHILD(seg->children[side]))
		return 0;

	wall_num = seg->sides[side].wall_num;
	if (wall_num < 0 || Walls[wall_num].type != WALL_DOOR)
		return 0;

	return Walls[wall_num].keys == KEY_NONE && !(Walls[wall_num].flags & WALL_DOOR_LOCKED);
}

static object *best_powerup_in(const duel_bot *b, int segnum, int *value)
{
	int objnum;
	object *best = NULL;

	*value = 0;
	for (objnum = Segments[segnum].objects; objnum != -1; objnum = Objects[objnum].next)
	{
		int v;

		if (Objects[objnum].type != OBJ_POWERUP)
			continue;

		v = powerup_value(b, Objects[objnum].id);
		if (v > *value)
		{
			*value = v;
			best = &Objects[objnum];
		}
	}

	return best;
}

static int needs_fuel(const duel_bot *b)
{
	return Players[b->pnum].energy < DUEL_LOW_ENERGY;
}

static void store_path(duel_bot *b, int start, int found)
{
	int s, len = Bfs_depth[found];

	b->path_len = min(len, DUEL_PATH_MAX);
	b->path_pos = 0;

	for (s = found; s != start; s = Bfs_prev[s])
	{
		len--;
		if (len < b->path_len)
			b->path[len] = s;
	}
}

static void bfs(int start)
{
	int head = 0, tail = 0, side;

	memset(Bfs_prev, 0xff, sizeof(Bfs_prev));
	Bfs_prev[start] = start;
	Bfs_depth[start] = 0;
	Bfs_queue[tail++] = start;

	while (head < tail)
	{
		int s = Bfs_queue[head++];

		for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
		{
			int child = Segments[s].children[side];

			if (!IS_CHILD(child) || Bfs_prev[child] != -1 || !side_passable(&Segments[s], side))
				continue;

			Bfs_prev[child] = s;
			Bfs_depth[child] = Bfs_depth[s] + 1;
			Bfs_queue[tail++] = child;
		}
	}
}

static int reachable(int segnum)
{
	return segnum >= 0 && segnum <= Highest_segment_index && Bfs_prev[segnum] != -1;
}

// a cube and its open neighbours are in view from inside it
static void look_around(int pnum, int segnum)
{
	segment *seg = &Segments[segnum];
	int side;

	Visited[pnum][segnum] = GameTime64;
	for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
		if (WALL_IS_DOORWAY(seg, side) == WID_NO_WALL)	// a fan or door hides what's behind it
			Visited[pnum][seg->children[side]] = GameTime64;
}

// the nearest part of the map nobody has checked lately, hidden rooms included
static int sweep_target(const object *bot)
{
	fix64 *seen = Visited[bot->id];
	int s, best = -1;

	bfs(bot->segnum);
	for (s = 0; s <= Highest_segment_index; s++)
	{
		if (!reachable(s) || Bfs_depth[s] < DUEL_SWEEP_MIN_DEPTH || GameTime64 - seen[s] < DUEL_SWEEP_STALE)
			continue;
		if (best < 0 || Bfs_depth[s] < Bfs_depth[best] ||
			(Bfs_depth[s] == Bfs_depth[best] && seen[s] < seen[best]))
			best = s;
	}
	return best;
}

static int path_to_segment(duel_bot *b, const object *bot, int goal_seg)
{
	bfs(bot->segnum);
	if (!reachable(goal_seg))
		return 0;

	store_path(b, bot->segnum, goal_seg);
	return 1;
}

static int avoided(const duel_bot *b, int segnum)
{
	int i;

	for (i = 0; i < DUEL_AVOID_SLOTS; i++)
		if (b->avoid_seg[i] == segnum && GameTime64 < b->avoid_until[i])
			return 1;

	return 0;
}

static void avoid_goal(duel_bot *b)
{
	int i, oldest = 0;

	if (b->goal_seg < 0)
		return;

	for (i = 1; i < DUEL_AVOID_SLOTS; i++)
		if (b->avoid_until[i] < b->avoid_until[oldest])
			oldest = i;

	b->avoid_seg[oldest] = b->goal_seg;
	b->avoid_until[oldest] = GameTime64 + DUEL_AVOID_TIME;
}

// best pickup (or fuel) by value over travel distance
static int path_to_supplies(duel_bot *b, const object *bot, int max_depth)
{
	int s, best_seg = -1;
	fix best_score = 0;
	object *best_pow = NULL;

	bfs(bot->segnum);

	for (s = 0; s <= Highest_segment_index; s++)
	{
		int value;
		object *pow;
		fix score;

		if (!reachable(s) || avoided(b, s) || Bfs_depth[s] > max_depth)
			continue;

		pow = best_powerup_in(b, s, &value);
		if (!pow && Segments[s].special == SEGMENT_IS_FUELCEN)
			value = needs_fuel(b) ? 6 : Players[b->pnum].energy < DUEL_TOP_UP_ENERGY ? 3 : 0;
		if (!value)
			continue;

		score = fixdiv(i2f(value), i2f(Bfs_depth[s] + DUEL_TRAVEL_BIAS));
		if (score > best_score)
		{
			best_score = score;
			best_seg = s;
			best_pow = pow;
		}
	}

	if (max_depth == MAX_SEGMENTS)
		b->supplies_known = best_seg >= 0;
	if (best_seg < 0)
		return 0;

	store_path(b, bot->segnum, best_seg);
	b->goal_seg = best_seg;
	if (best_pow)
		b->goal_pos = best_pow->pos;
	else
		compute_segment_center(&b->goal_pos, &Segments[best_seg]);
	return 1;
}

// where they'll be by now if they kept going the way they broke sight
static int predicted_segment(const duel_bot *b, vms_vector *pos)
{
	fvi_query fq;
	fvi_info hit;
	vms_vector end;
	fix ahead = DUEL_ESCAPE_PROJECT;	// by the time the bot gets there

	*pos = b->last_seen_pos;
	vm_vec_scale_add(&end, &b->last_seen_pos, &b->escape_vel, ahead);
	fq.p0 = &b->last_seen_pos;
	fq.p1 = &end;
	fq.startseg = b->last_seen_seg;
	fq.rad = F1_0 * 4;
	fq.thisobjnum = -1;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;
	find_vector_intersection(&fq, &hit);
	if (hit.hit_seg < 0)
		return b->last_seen_seg;
	*pos = hit.hit_pnt;
	return hit.hit_seg;
}

static int path_to_last_seen(duel_bot *b, const object *bot)
{
	vms_vector goal;
	int segnum;

	if (!b->seen_at || GameTime64 - b->seen_at > DUEL_MEMORY)
		return 0;

	segnum = predicted_segment(b, &goal);
	if (vm_vec_dist((vms_vector *)&bot->pos, &goal) < DUEL_ARRIVE_DIST)
	{
		b->seen_at = 0;		// got there, nobody home
		return 0;
	}

	if (!path_to_segment(b, bot, segnum))
		return 0;

	b->goal_seg = -1;		// the enemy moves; never blacklist where it was

	b->goal_pos = goal;
	return 1;
}

static int cutting_off(const duel_bot *b)
{
	return vm_vec_dist((vms_vector *)&b->goal_pos, (vms_vector *)&b->last_seen_pos) > DUEL_CUTOFF_MIN;
}

// just respawned and not seen yet: wait where they always go first
static int path_to_haunt(duel_bot *b, const object *bot)
{
	const duel_foe *f;
	int segnum;

	if (b->target < 0)
		return 0;
	f = &Foes[b->target];
	if (!f->haunt_open || GameTime64 - f->died_at > DUEL_HAUNT_WINDOW)
		return 0;
	segnum = favourite_haunt(f);
	if (segnum < 0 || segnum == bot->segnum || !path_to_segment(b, bot, segnum))
		return 0;
	compute_segment_center(&b->goal_pos, &Segments[segnum]);
	return 1;
}

// a spawn point away from here: where an enemy is likely to be coming from
static int far_start_segment(const object *bot)
{
	int count = max(min(NumNetPlayerPositions, MAX_PLAYERS), 1);
	int tries, idx = 0;

	for (tries = 0; tries < count * 2; tries++)
	{
		idx = d_rand() % count;
		if (vm_vec_dist(&Player_init[idx].pos, (vms_vector *)&bot->pos) > DUEL_PATROL_MIN_DIST)
			break;
	}

	return Player_init[idx].segnum;
}

// where the enemy has been seen most; a start position before it knows
// sweep: the spawn point it has looked at least recently, hot spots first
static int patrol_target(const object *bot)
{
	int count = max(min(NumNetPlayerPositions, MAX_PLAYERS), 1);
	int hot = hottest_segment(bot->segnum);
	fix64 *seen = Visited[bot->id];
	int idx, best = -1;

	if (hot >= 0 && GameTime64 - seen[hot] > DUEL_REVISIT)
	{
		Brain.heat[hot] /= 2;
		return hot;
	}
	best = sweep_target(bot);
	if (best >= 0)
		return best;

	for (idx = 0; idx < count; idx++)
	{
		int seg = Player_init[idx].segnum;

		if (seg == bot->segnum || vm_vec_dist(&Player_init[idx].pos, (vms_vector *)&bot->pos) < DUEL_PATROL_MIN_DIST)
			continue;
		if (best < 0 || seen[seg] < seen[best])
			best = seg;
	}
	return best >= 0 ? best : far_start_segment(bot);
}

// when the usual patrol spots are off limits: anywhere far that isn't
static int farthest_open_segment(const duel_bot *b, const object *bot)
{
	int s, best = -1;

	bfs(bot->segnum);
	for (s = 0; s <= Highest_segment_index; s++)
		if (reachable(s) && !avoided(b, s) && (best < 0 || Bfs_depth[s] > Bfs_depth[best]))
			best = s;
	return best == bot->segnum ? -1 : best;
}

static int stale_haunt(const duel_bot *b, const object *bot)
{
	int segnum;

	if (b->target < 0)
		return -1;
	segnum = favourite_haunt(&Foes[b->target]);
	if (segnum < 0 || segnum == bot->segnum || GameTime64 - Visited[bot->id][segnum] < DUEL_REVISIT)
		return -1;
	return segnum;
}

static void path_to_patrol(duel_bot *b, const object *bot)
{
	int haunt = stale_haunt(b, bot);

	if (haunt >= 0)
		b->patrol_seg = haunt;
	if (b->patrol_seg < 0 || b->patrol_seg == bot->segnum)
		b->patrol_seg = patrol_target(bot);

	if (avoided(b, b->patrol_seg) || !path_to_segment(b, bot, b->patrol_seg))
		b->patrol_seg = farthest_open_segment(b, bot);
	if (b->patrol_seg < 0 || !path_to_segment(b, bot, b->patrol_seg))
	{
		b->patrol_seg = -1;
		return;
	}

	b->goal_seg = b->patrol_seg;

	compute_segment_center(&b->goal_pos, &Segments[b->patrol_seg]);
}

//	-------------------------------------------------------------------------
//	Strategy (Q3-style aggression)
//	-------------------------------------------------------------------------

static int weapon_tier(const duel_bot *b)
{
	player *p = &Players[b->pnum];

	if (has_primary(b, PLASMA_INDEX) || has_primary(b, FUSION_INDEX))
		return 4;
	if (has_primary(b, VULCAN_INDEX) && p->primary_ammo[VULCAN_INDEX])
		return 3;
	if (has_primary(b, SPREADFIRE_INDEX) || (p->flags & PLAYER_FLAGS_QUAD_LASERS) || p->laser_level >= 2)
		return 2;
	return 1;
}

// only the spawn laser: go arm up before picking a fight
static int undergunned(const duel_bot *b)
{
	return weapon_tier(b) == 1 && b->supplies_known;
}

// 0..1ish: how well-placed this bot is to take a fight
static fix aggression(const duel_bot *b)
{
	player *p = &Players[b->pnum];
	fix health = fixdiv(p->shields, INITIAL_SHIELDS);
	fix arms = fixdiv(i2f(weapon_tier(b)), i2f(4));
	fix missiles = (p->secondary_ammo[HOMING_INDEX] || p->secondary_ammo[MEGA_INDEX]) ? F1_0/10 : 0;

	if (needs_fuel(b) && !(has_primary(b, VULCAN_INDEX) && p->primary_ammo[VULCAN_INDEX]))
		arms /= 2;

	return fixmul(health, F1_0*6/10) + fixmul(arms, F1_0*4/10) + missiles;
}

static void journal_plan(const duel_bot *b, int plan, const char *decision, const char *why)
{
	if (!journaling(b) || plan == Journal_plan)
		return;
	Journal_plan = plan;
	journal("PLAN", decision, "%s", why);
}

// smart missiles win duels: without any, go get them before hunting
static int no_smarts(const duel_bot *b)
{
	return !Players[b->pnum].secondary_ammo[SMART_INDEX] && b->supplies_known && quiet(b);	// never break off a hunt for them
}

// a stash it doesn't need but you will: go put a proxy on it
static int path_to_honey_pot(duel_bot *b, const object *bot)
{
	int s, objnum;

	if (Players[b->pnum].secondary_ammo[PROXIMITY_INDEX] < DUEL_MINE_KEEP ||
		GameTime64 - max(b->seen_at, b->heard_at) < DUEL_MINE_QUIET ||
		GameTime64 < b->next_mine || undergunned(b) || match_tense())
		return 0;
	bfs(bot->segnum);
	for (s = 0; s <= Highest_segment_index; s++)
	{
		if (!reachable(s) || avoided(b, s) || Bfs_depth[s] > DUEL_MINE_DEPTH)
			continue;
		for (objnum = Segments[s].objects; objnum != -1; objnum = Objects[objnum].next)
		{
			const object *pow = &Objects[objnum];

			if (pow->type != OBJ_POWERUP || denial_value(pow->id) < DUEL_MINE_WORTH ||
				powerup_value(b, pow->id) || mined_already(bot, pow))
				continue;
			store_path(b, bot->segnum, s);
			b->goal_seg = s;
			b->goal_pos = pow->pos;
			return 1;
		}
	}
	return 0;
}

// sit in the fuel cen until full, unless there's a fight to take
static int topping_up(duel_bot *b, const object *bot)
{
	if (b->visible || Segments[bot->segnum].special != SEGMENT_IS_FUELCEN ||
		Players[b->pnum].energy >= DUEL_FULL_TANK)
		return 0;
	b->path_len = 0;
	b->path_pos = 0;
	b->goal_seg = bot->segnum;
	compute_segment_center(&b->goal_pos, &Segments[bot->segnum]);
	return 1;
}

static void plan_route(duel_bot *b, const object *bot, int retreating)
{
	if (GameTime64 < b->next_path)
		return;

	b->next_path = GameTime64 + DUEL_PATH_REFRESH;

	if (topping_up(b, bot))
	{
		journal_plan(b, 7, "refuel", "energy center: filling the tank before moving on");
		return;
	}
	if ((retreating || undergunned(b) || no_smarts(b) || aggression(b) < DUEL_CHASE_AGGRESSION) &&
		path_to_supplies(b, bot, MAX_SEGMENTS))
	{
		b->stats.plans[0]++;
		journal_plan(b, 0, "restock", retreating ? "retreating: heading for supplies" :
					 undergunned(b) ? "undergunned: arming up before I look for you" :
					 no_smarts(b) ? "no smart missiles: they decide duels, getting some first" : "not confident enough to hunt yet");
		return;
	}
	if (path_to_supplies(b, bot, DUEL_GRAB_DEPTH))
	{
		b->stats.plans[2]++;
		journal_plan(b, 2, "grab nearby pickup", "something useful is a few rooms away");
		return;
	}
	if (path_to_honey_pot(b, bot))
	{
		journal_plan(b, 4, "set a honey pot", "a stash you'll want and I don't: mining it");
		return;
	}
	if (path_to_last_seen(b, bot))
	{
		b->stats.plans[1]++;
		if (cutting_off(b))
			journal_plan(b, 5, "cut you off", "you broke sight moving fast: heading where you'll be, not where you were");
		else
			journal_plan(b, 1, "chase", "going to where I last saw or heard you");
		return;
	}
	if (path_to_haunt(b, bot))
	{
		journal_plan(b, 6, "ambush your route", "after respawning you head the same way: waiting there");
		return;
	}
	if (path_to_supplies(b, bot, MAX_SEGMENTS))
	{
		b->stats.plans[2]++;
		return;
	}
	b->stats.plans[3]++;
	journal_plan(b, 3, "patrol", "no leads: sweeping spawns and busy rooms");
	path_to_patrol(b, bot);
}

static void advance_path(duel_bot *b, const object *bot)
{
	int k, last = min(b->path_pos + DUEL_LOOKAHEAD, b->path_len);

	for (k = b->path_pos; k < last; k++)
		if (b->path[k] == bot->segnum)
			b->path_pos = k + 1;
}

static int aimed_near(const object *bot, const vms_vector *aim);
static int reactor_in_line(const object *bot);

static int closed_door_side(int from, int to)
{
	segment *seg = &Segments[from];
	int side = find_connect_side(&Segments[to], seg);
	int wall_num;

	if (side < 0)
		return -1;
	wall_num = seg->sides[side].wall_num;
	if (wall_num < 0 || Walls[wall_num].type != WALL_DOOR || Walls[wall_num].state != WALL_DOOR_CLOSED)
		return -1;
	return side;
}

static int door_in_sight(const object *bot, int segnum, int side, const vms_vector *door)
{
	fvi_query fq;
	fvi_info hit;

	fq.p0 = (vms_vector *)&bot->pos;
	fq.p1 = (vms_vector *)door;
	fq.startseg = bot->segnum;
	fq.rad = 0;
	fq.thisobjnum = bot - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;
	if (find_vector_intersection(&fq, &hit) == HIT_NONE)
		return 1;
	return hit.hit_side_seg == segnum && hit.hit_side == side;
}

static void fire_flare(duel_bot *b, object *bot)
{
	player *p = bot_player(b);

	if (reactor_in_line(bot))
		return;
	p->energy = max(p->energy - Weapon_info[FLARE_ID].energy_usage, 0);
	Laser_player_fire(bot, FLARE_ID, 6, 1, 0, bot->orient.fvec);
	if (net_game())
		multi_send_fire_for(b->pnum, FLARE_ID + MISSILE_ADJUST, 0, 0, 1, -1);
	b->next_flare = GameTime64 + DUEL_FLARE_GAP;
}

static int door_ahead(const duel_bot *b, const object *bot, vms_vector *door, int *segnum, int *side)
{
	int k, last = min(b->path_pos + DUEL_LOOKAHEAD, b->path_len);
	int from = bot->segnum;

	for (k = b->path_pos; k < last; from = b->path[k++])
	{
		*side = closed_door_side(from, b->path[k]);
		if (*side < 0)
			continue;
		*segnum = from;
		compute_center_point_on_side(door, &Segments[from], *side);
		return vm_vec_dist(door, (vms_vector *)&bot->pos) < DUEL_FLARE_RANGE;
	}
	return 0;
}

// a door shot from range is open on arrival; bumping it costs a stop
static void flare_doors(duel_bot *b, object *bot)
{
	vms_vector door;
	int segnum, side;

	if (GameTime64 < b->next_flare || bot_player(b)->energy < F1_0)
		return;
	if (door_ahead(b, bot, &door, &segnum, &side) &&
		aimed_near(bot, &door) && door_in_sight(bot, segnum, side, &door))
		fire_flare(b, bot);
}

// trichording through a door or a tight turn points the nose at the wall
static int tight_ahead(const duel_bot *b, const object *bot, const vms_vector *look)
{
	vms_vector door;
	int segnum, side;

	return b->aiming_at_mine || vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)look) < DUEL_TRICHORD_DIST ||
		   door_ahead(b, bot, &door, &segnum, &side);
}

static void route_point(duel_bot *b, const object *bot, vms_vector *point)
{
	int k;

	advance_path(b, bot);

	if (b->path_pos >= b->path_len || clear_line(bot, &b->goal_pos, bot->size + DUEL_CORNER_BERTH))
	{
		*point = b->goal_pos;
		return;
	}

	for (k = min(b->path_pos + DUEL_LOOKAHEAD, b->path_len) - 1; k > b->path_pos; k--)
	{
		compute_segment_center(point, &Segments[b->path[k]]);
		if (clear_line(bot, point, bot->size + DUEL_CORNER_BERTH))
			return;
	}

	compute_segment_center(point, &Segments[b->path[b->path_pos]]);
}

//	-------------------------------------------------------------------------
//	Aiming (D3-style: run the target through the physics model)
//	-------------------------------------------------------------------------

// per-tick input the target must be holding to produce the observed acceleration
static void inferred_input(const duel_bot *b, const object *target, vms_vector *input)
{
	fix drag = target->mtype.phys_info.drag;

	vm_vec_copy_scale(input, (vms_vector *)&b->enemy_accel, DUEL_PREDICT_STEP);
	vm_vec_scale_add2(input, (vms_vector *)&target->mtype.phys_info.velocity, drag);
}

static void predict_position(const duel_bot *b, const object *target, fix time, vms_vector *pos)
{
	vms_vector vel = target->mtype.phys_info.velocity, input;
	fix drag = target->mtype.phys_info.drag;
	int ticks = min(time / DUEL_PREDICT_STEP, DUEL_PREDICT_MAX);

	inferred_input(b, target, &input);
	*pos = target->pos;

	while (ticks-- > 0)
	{
		vm_vec_add2(&vel, &input);
		vm_vec_scale(&vel, F1_0 - drag);
		vm_vec_scale_add2(pos, &vel, DUEL_PREDICT_STEP);
	}
}

// solves time of flight against the predicted path, so closing speed counts
static void lead_point(const duel_bot *b, const object *bot, const object *enemy, int weapon_id, fix bias, vms_vector *aim)
{
	fix speed = Weapon_info[weapon_id].speed[Difficulty_level];
	fix lead = fixmul(fixmul(Lead_scale[b->lead.arm], bias), skill(b)->lead_share);
	int pass;

	*aim = enemy->pos;
	if (speed <= 0)
		return;

	for (pass = 0; pass < 3; pass++)
	{
		fix dist = vm_vec_dist((vms_vector *)&bot->pos, aim);
		fix t = fixmul(fixdiv(dist, speed), lead);

		predict_position(b, enemy, t, aim);
	}
}

// a lesser pilot's aim wanders around the target instead of sitting on it
static void glide_toward(vms_vector *v, const vms_vector *goal, fix step)
{
	vms_vector delta;

	vm_vec_sub(&delta, (vms_vector *)goal, v);
	if (vm_vec_mag(&delta) <= step)
	{
		*v = *goal;
		return;
	}
	vm_vec_normalize(&delta);
	vm_vec_scale_add2(v, &delta, step);
}

static void add_aim_slop(duel_bot *b, const object *enemy, vms_vector *aim)
{
	fix slop = fixmul(skill(b)->aim_slop, enemy->size * 2);

	if (!slop)
		return;

	if (GameTime64 >= b->aim_slop_until)
	{
		b->aim_slop_goal.x = random_fix(F1_0) * 2 - F1_0;
		b->aim_slop_goal.y = random_fix(F1_0) * 2 - F1_0;
		b->aim_slop_goal.z = random_fix(F1_0) * 2 - F1_0;
		b->aim_slop_until = GameTime64 + DUEL_SLOP_HOLD;
	}
	glide_toward(&b->aim_slop, &b->aim_slop_goal, fixmul(FrameTime, DUEL_SLOP_DRIFT));
	vm_vec_scale_add2(aim, &b->aim_slop, slop);
}

// would a shot down the nose pass within the target's hull?
static int on_target(const object *bot, const object *enemy, const vms_vector *aim)
{
	vms_vector dir, err;
	fix dist;

	vm_vec_sub(&dir, (vms_vector *)aim, (vms_vector *)&bot->pos);
	dist = vm_vec_normalize(&dir);

	if (vm_vec_dot(&dir, (vms_vector *)&bot->orient.fvec) <= 0)
		return 0;

	vm_vec_cross(&err, &dir, (vms_vector *)&bot->orient.fvec);
	return fixmul(vm_vec_mag(&err), dist) < enemy->size;
}

static int aimed_near(const object *bot, const vms_vector *aim)
{
	vms_vector dir;

	vm_vec_sub(&dir, (vms_vector *)aim, (vms_vector *)&bot->pos);
	vm_vec_normalize(&dir);
	return vm_vec_dot(&dir, (vms_vector *)&bot->orient.fvec) > DUEL_HOLD_DOT;
}

//	-------------------------------------------------------------------------
//	Weapons
//	-------------------------------------------------------------------------

static fix primary_range(int weapon)
{
	static const int range[MAX_PRIMARY_WEAPONS] = { 45, 70, 35, 55, 35 };

	return i2f(range[weapon]);
}

// weapon id actually fired: lasers go by level
static int fired_weapon_id(const duel_bot *b, int weapon)
{
	if (weapon == LASER_INDEX)
		return LASER_ID_L1 + Players[b->pnum].laser_level;

	return Primary_weapon_to_weapon_info[weapon];
}

static int bolts_per_shot(const duel_bot *b, int weapon)
{
	static const int bolts[MAX_PRIMARY_WEAPONS] = { 2, 1, 3, 2, 2 };

	if (weapon == LASER_INDEX && (Players[b->pnum].flags & PLAYER_FLAGS_QUAD_LASERS))
		return 4;

	return bolts[weapon];
}

static fix enemy_lateral_speed(const object *bot, const object *enemy, fix *closing)
{
	vms_vector to_enemy, rel, lateral;

	vm_vec_sub(&to_enemy, (vms_vector *)&enemy->pos, (vms_vector *)&bot->pos);
	vm_vec_normalize(&to_enemy);
	vm_vec_sub(&rel, (vms_vector *)&enemy->mtype.phys_info.velocity, (vms_vector *)&bot->mtype.phys_info.velocity);

	*closing = -vm_vec_dot(&rel, &to_enemy);
	vm_vec_scale_add(&lateral, &rel, &to_enemy, *closing);
	return vm_vec_mag(&lateral);
}

// share of bolts that land: slow shots at a sideways-moving target miss
static fix modelled_hit_chance(const duel_bot *b, fix target_size, int weapon, fix dist, fix lateral)
{
	fix speed = Weapon_info[fired_weapon_id(b, weapon)].speed[Difficulty_level];
	fix drift, radius = target_size;

	if (speed <= 0)
		return 0;

	drift = min(fixmul(fixmul(lateral, fixdiv(dist, speed)), DUEL_JUKE), i2f(150));
	if (weapon == SPREADFIRE_INDEX)
		radius += dist / 8;		// the fan covers ground, not every bolt lands

	return fixdiv(fixmul(radius, radius), fixmul(radius, radius) + fixmul(drift, drift));
}

// what this pilot has actually been landing with it, once there's enough to judge
static fix hit_chance(const duel_bot *b, fix target_size, int weapon, fix dist, fix lateral)
{
	fix model = modelled_hit_chance(b, target_size, weapon, dist, lateral);
	int shots = Brain.weapon_shots[weapon];

	if (shots < DUEL_WEAPON_TRIALS)
		return model;

	return (model + fixdiv(i2f(min(Brain.weapon_hits[weapon], shots)), i2f(shots))) / 2;
}

// Descent 101 weapon theory as multipliers on raw damage per second
static fix context_weight(const duel_bot *b, int weapon, fix dist, fix closing)
{
	fix w = F1_0;
	int tight = b->openness < DUEL_TIGHT, open = b->openness > DUEL_OPEN;
	int theirs = (GameTime64 - b->enemy_weapon_at < F1_0*5) ? b->enemy_weapon : -1;

	switch (weapon)
	{
		case LASER_INDEX:		// quads: pre-fire weapon, single-cube hallways, not dogfights
			if (open && dist < i2f(80))
				w = F1_0*6/10;
			if (tight && dist > i2f(20) && dist < i2f(60))
				w = F1_0*13/10;
			break;
		case PLASMA_INDEX:		// dogfights and tight areas; not big rooms or distance
			if (open || dist > i2f(120))
				w = F1_0*6/10;
			if (tight)
				w = F1_0*12/10;
			if (theirs == FUSION_INDEX)
				w = fixmul(w, F1_0*13/10);
			break;
		case SPREADFIRE_INDEX:	// hallways and chase-downs
			if (tight)
				w = F1_0*12/10;
			if (closing < -i2f(10))
				w = fixmul(w, F1_0*13/10);
			if (theirs == PLASMA_INDEX)
				w = fixmul(w, F1_0*12/10);
			break;
		case FUSION_INDEX:		// close and small; never into vulcan or plasma
			if (dist > i2f(60))
				w = F1_0*4/10;
			else if (tight)
				w = F1_0*13/10;
			if (theirs == VULCAN_INDEX || theirs == PLASMA_INDEX)
				w = fixmul(w, F1_0*6/10);
			break;
		case VULCAN_INDEX:		// finish the weak, punish the charging bull; not point blank
			if (target_shields(b) < bull_shields(b))
				w = F1_0*14/10;
			if (closing > DUEL_CHARGING_SPEED)
				w = fixmul(w, F1_0*13/10);
			if (dist < i2f(25))
				w = fixmul(w, F1_0*6/10);
			if (theirs == PLASMA_INDEX || theirs == SPREADFIRE_INDEX)
				w = fixmul(w, F1_0*12/10);
			break;
	}

	return w;
}

static fix energy_weight(const duel_bot *b, int weapon)
{
	fix energy = Players[b->pnum].energy;

	if (weapon == VULCAN_INDEX || energy >= DUEL_SAVE_ENERGY)
		return F1_0;

	return fixdiv(energy, DUEL_SAVE_ENERGY);
}

static fix score_in_context(const duel_bot *b, int weapon, fix dist, fix lateral, fix closing, fix target_size)
{
	int id = fired_weapon_id(b, weapon);
	fix damage = Weapon_info[id].strength[Difficulty_level] * bolts_per_shot(b, weapon);
	fix wait = max(Weapon_info[id].fire_wait, F1_0/64);
	fix dps = fixdiv(damage, wait);

	if (weapon == FUSION_INDEX)
		dps *= 2;		// it charges

	dps = fixmul(dps, hit_chance(b, target_size, weapon, dist, lateral));
	dps = fixmul(dps, context_weight(b, weapon, dist, closing));
	return fixmul(dps, energy_weight(b, weapon));
}

static fix weapon_score(const duel_bot *b, const object *bot, const object *enemy, int weapon, fix dist)
{
	fix closing, lateral = enemy_lateral_speed(bot, enemy, &closing);

	return score_in_context(b, weapon, dist, lateral, closing, enemy->size);
}

static int pick_primary(const duel_bot *b, const object *bot, const object *enemy, fix dist)
{
	int w, best = -1;
	fix best_score = 0;

	for (w = 0; w < MAX_PRIMARY_WEAPONS; w++)
	{
		fix score;

		if (player_has_weapon(b->pnum, w, 0) != HAS_ALL)
			continue;

		score = weapon_score(b, bot, enemy, w, dist);
		if (best < 0 || score > best_score)
		{
			best = w;
			best_score = score;
		}
	}

	return best;
}

// same rearm a human pays on selecting a primary
static int switch_to(duel_bot *b, int weapon)
{
	if (weapon == b->held_weapon)
		return weapon;

	if (journaling(b) && weapon >= 0)
		journal("SWITCH", Gun_names[weapon], "scores best at this range with my ammo and energy");
	b->held_weapon = weapon;
	b->held_since = GameTime64;
	b->fusion_charge = 0;
	b->next_primary = max(b->next_primary, GameTime64 + REARM_TIME);
	b->stats.switches++;
	return weapon;
}

static int holding_usable(const duel_bot *b)
{
	return b->held_weapon >= 0 && player_has_weapon(b->pnum, b->held_weapon, 0) == HAS_ALL;
}

// between fights: have the gun this space calls for in hand before contact
static void preselect_weapon(duel_bot *b)
{
	fix size = Polygon_models[Player_ship->model_num].rad;
	fix dist = b->openness < DUEL_TIGHT ? i2f(40) : b->openness > DUEL_OPEN ? i2f(100) : i2f(60);
	int w, best = -1;
	fix best_score = 0;

	if (GameTime64 - b->held_since < DUEL_PRESELECT_HOLD && holding_usable(b))
		return;

	for (w = 0; w < MAX_PRIMARY_WEAPONS; w++)
	{
		fix score;

		if (player_has_weapon(b->pnum, w, 0) != HAS_ALL)
			continue;
		if (w == FUSION_INDEX)
			continue;		// searching: carry a gun that answers instantly
		score = score_in_context(b, w, dist, DUEL_NOMINAL_LATERAL, 0, size);
		if (best < 0 || score > best_score)
		{
			best = w;
			best_score = score;
		}
	}

	if (best < 0 || best == b->held_weapon)
		return;
	if (holding_usable(b) && best_score < fixmul(score_in_context(b, b->held_weapon, dist, DUEL_NOMINAL_LATERAL, 0, size), DUEL_SWITCH_MARGIN))
		return;
	switch_to(b, best);
}

// keep the gun for the fight unless something clearly better turns up
static int commit_weapon(duel_bot *b, const object *bot, const object *enemy, fix dist)
{
	int best = pick_primary(b, bot, enemy, dist);
	fix held_score;

	if (best < 0 || best == b->held_weapon)
		return best;
	if (best == FUSION_INDEX && dist < DUEL_FUSION_SWAP_MIN && holding_usable(b))
		return b->held_weapon;		// no time to charge with them this close

	if (!holding_usable(b))
		return switch_to(b, best);	// dry: anything that shoots

	if (b->held_since && GameTime64 - b->held_since < DUEL_WEAPON_HOLD)
		return b->held_weapon;

	held_score = weapon_score(b, bot, enemy, b->held_weapon, dist);
	if (weapon_score(b, bot, enemy, best, dist) < fixmul(held_score, DUEL_SWAP_MARGIN))
		return b->held_weapon;

	return switch_to(b, best);
}

// guide: overshoot plasma to saturate, slight lead on spread, high lead on quads
static fix weapon_lead_bias(int weapon)
{
	switch (weapon)
	{
		case PLASMA_INDEX:		return F1_0*125/100;
		case SPREADFIRE_INDEX:	return F1_0*85/100;
		case LASER_INDEX:		return F1_0*11/10;
		default:				return F1_0;
	}
}

static fix energy_cost(int weapon)
{
	fix cost = Weapon_info[Primary_weapon_to_weapon_info[weapon]].energy_usage;

	if (weapon == FUSION_INDEX)
		return DUEL_FUSION_COST;
	if (Difficulty_level < 2)
		cost = fixmul(cost, i2f(Difficulty_level + 2) / 4);
	return cost;
}

static void pay_for_shot(duel_bot *b, int weapon)
{
	player *p = bot_player(b);
	int wi = Primary_weapon_to_weapon_info[weapon];

	if (weapon == VULCAN_INDEX)
	{
		p->primary_ammo[VULCAN_INDEX] = max(p->primary_ammo[VULCAN_INDEX] - Weapon_info[wi].ammo_usage, 0);
		return;
	}

	p->energy = max(p->energy - energy_cost(weapon), 0);
}

// the reactor shoots back, and nothing in a duel is gained by hitting it
static int reactor_in_line(const object *bot)
{
	fvi_query fq;
	fvi_info hit;
	vms_vector end;

	vm_vec_scale_add(&end, (vms_vector *)&bot->pos, (vms_vector *)&bot->orient.fvec, DUEL_SHOT_CHECK);
	fq.p0 = (vms_vector *)&bot->pos;
	fq.p1 = &end;
	fq.startseg = bot->segnum;
	fq.rad = 0;
	fq.thisobjnum = bot - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = FQ_CHECK_OBJS;
	return find_vector_intersection(&fq, &hit) == HIT_OBJECT &&
		   Objects[hit.hit_object].type == OBJ_CNTRLCEN;
}

static void fire_primary(duel_bot *b, object *bot, int weapon)
{
	player *p = bot_player(b);
	fix saved_charge = Fusion_charge;
	int flags = 0;

	if (reactor_in_line(bot))
		return;

	if (weapon == LASER_INDEX && (p->flags & PLAYER_FLAGS_QUAD_LASERS))
		flags |= LASER_QUAD;

	if (weapon == SPREADFIRE_INDEX)
	{
		if (b->spread_toggle)
			flags |= LASER_SPREADFIRE_TOGGLED;
		b->spread_toggle = !b->spread_toggle;
	}

	// fusion reads and clears the global charge meter, which is the human's
	Fusion_charge = b->fusion_charge;
	do_laser_firing(bot - Objects, weapon, p->laser_level, flags, 1, bot->orient.fvec);
	Fusion_charge = saved_charge;

	if (net_game())
		multi_send_fire_for(b->pnum, weapon, p->laser_level,
							weapon == FUSION_INDEX ? (sbyte)(b->fusion_charge >> 12) : flags, 1, -1);
	b->fusion_charge = 0;

	if (weapon != FUSION_INDEX)		// fusion paid while charging
		pay_for_shot(b, weapon);
	b->last_primary = weapon;
	b->last_primary_at = GameTime64;
	b->stats.weapon_shots[weapon]++;
	Brain.weapon_shots[weapon]++;
	// from the scheduled time, not now, so frame jitter doesn't slow the gun
	b->next_primary = max(b->next_primary, GameTime64 - FrameTime) + Weapon_info[Primary_weapon_to_weapon_info[weapon]].fire_wait;
	start_probe(b, bot);
	b->last_shot_at = GameTime64;
	b->lead.shots++;
	b->stats.shots++;
}

static void fire_secondary(duel_bot *b, object *bot, int index)
{
	int gun = DUEL_BOMB_GUN;

	if (index != PROXIMITY_INDEX && reactor_in_line(bot))
		return;
	if (index == CONCUSSION_INDEX || index == HOMING_INDEX)
		gun = DUEL_MISSILE_GUN + (b->missile_gun++ & 1);

	Laser_player_fire(bot, Secondary_weapon_to_weapon_info[index], gun, 1, 0, bot->orient.fvec);
	bot_player(b)->secondary_ammo[index]--;
	if (net_game())
		multi_send_fire_for(b->pnum, index + MISSILE_ADJUST, 0, (gun == DUEL_BOMB_GUN) ? 0 : gun - DUEL_MISSILE_GUN, 1, -1);

	if (index == MEGA_INDEX)
	{
		vms_vector recoil;

		vm_vec_copy_scale(&recoil, &bot->orient.fvec, -(F1_0 << 7));
		phys_apply_force(bot, &recoil);
	}

	if (journaling(b))
		journal("FIRE", Missile_names[index], index == PROXIMITY_INDEX ? "dropped on your chase line" :
				"lined up and clear of my own blast");
	b->next_secondary = GameTime64 + max(Weapon_info[Secondary_weapon_to_weapon_info[index]].fire_wait, DUEL_MISSILE_GAP);
	b->last_missile_at = GameTime64;
	b->stats.missiles++;
}

static int missile_hits_wall_near(const object *bot, fix reach, fix blast)
{
	fvi_query fq;
	fvi_info hit;
	vms_vector end;

	vm_vec_scale_add(&end, (vms_vector *)&bot->pos, (vms_vector *)&bot->orient.fvec, reach);
	fq.p0 = (vms_vector *)&bot->pos;
	fq.p1 = &end;
	fq.startseg = bot->segnum;
	fq.rad = F1_0;
	fq.thisobjnum = bot - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;	// grates stop missiles too

	if (find_vector_intersection(&fq, &hit) != HIT_WALL)
		return 0;
	return vm_vec_dist(&hit.hit_pnt, (vms_vector *)&bot->pos) < blast;
}

// never close enough to eat its own blast, from the enemy or from a wall in between
// one kill from the end, a suicide is a point given away: leave more room
static fix splash_margin(void)
{
	return match_tense() ? DUEL_SPLASH_MARGIN * 2 : DUEL_SPLASH_MARGIN;
}

static int splash_safe_at(const object *bot, const object *enemy, int index, fix dist)
{
	fix blast = Weapon_info[Secondary_weapon_to_weapon_info[index]].damage_radius + splash_margin();

	return dist > blast && !missile_hits_wall_near(bot, vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&enemy->pos), blast * 2);
}

static int splash_safe_toward(const object *bot, int index, fix dist)
{
	fix blast = Weapon_info[Secondary_weapon_to_weapon_info[index]].damage_radius + splash_margin();

	return dist > blast && !missile_hits_wall_near(bot, dist, blast * 2);
}

// its own proxy in the line of fire: shooting it blows up in its face
static int own_mine_ahead(const object *bot)
{
	int i, bot_objnum = bot - Objects;

	for (i = 0; i <= Highest_object_index; i++)
	{
		const object *obj = &Objects[i];
		vms_vector to;
		fix dist;

		if (obj->type != OBJ_WEAPON || obj->id != PROXIMITY_ID || obj->ctype.laser_info.parent_num != bot_objnum)
			continue;
		vm_vec_sub(&to, (vms_vector *)&obj->pos, (vms_vector *)&bot->pos);
		dist = vm_vec_normalize(&to);
		if (dist < DUEL_MINE_SHOT_RANGE && vm_vec_dot(&to, (vms_vector *)&bot->orient.fvec) > DUEL_MINE_SHOT_CONE)
			return 1;
	}
	return 0;
}

static int moving_away_from(const object *bot, const vms_vector *pos)
{
	vms_vector to;

	vm_vec_sub(&to, (vms_vector *)pos, (vms_vector *)&bot->pos);
	return vm_vec_dot(&to, (vms_vector *)&bot->mtype.phys_info.velocity) < 0;
}

static int pick_secondary(const duel_bot *b, const object *bot, const object *enemy, fix dist)
{
	player *p = &Players[b->pnum];
	vms_vector to_enemy, aim;
	fix facing;

	vm_vec_sub(&to_enemy, (vms_vector *)&enemy->pos, (vms_vector *)&bot->pos);
	vm_vec_normalize(&to_enemy);
	facing = vm_vec_dot(&to_enemy, (vms_vector *)&bot->orient.fvec);

	if (GameTime64 < b->dodge_until)
		return -1;	// a sidestep can carry it into its own blast

	// proxy bombing: drop them on the line a chaser has to fly
	if (p->secondary_ammo[PROXIMITY_INDEX] && dist < i2f(100) &&
		(b->mode == MODE_BREAK || b->mode == MODE_RETREAT) && moving_away_from(bot, &enemy->pos))
		return PROXIMITY_INDEX;

	if (facing < F1_0*9/10)
		return -1;

	if (p->secondary_ammo[HOMING_INDEX] && dist < i2f(250) && splash_safe_at(bot, enemy, HOMING_INDEX, dist))
		return HOMING_INDEX;
	if (p->secondary_ammo[MEGA_INDEX] && dist > i2f(80) && splash_safe_at(bot, enemy, MEGA_INDEX, dist))
		return MEGA_INDEX;
	if (p->secondary_ammo[SMART_INDEX] && dist < i2f(250) && splash_safe_at(bot, enemy, SMART_INDEX, dist))
		return SMART_INDEX;

	lead_point(b, bot, enemy, Secondary_weapon_to_weapon_info[CONCUSSION_INDEX], F1_0, &aim);
	if (p->secondary_ammo[CONCUSSION_INDEX] && dist < i2f(160) && splash_safe_at(bot, enemy, CONCUSSION_INDEX, dist) &&
		on_target(bot, enemy, &aim))
		return CONCUSSION_INDEX;

	return -1;
}

// start or keep charging; 1 when it's time to let go
static int charge_fusion(duel_bot *b, int on_aim)
{
	player *p = bot_player(b);

	if (!b->fusion_charge)
	{
		if (p->energy < DUEL_FUSION_COST + F1_0)
			return 0;
		p->energy -= DUEL_FUSION_COST;
	}

	b->fusion_charge += FrameTime;
	p->energy = max(p->energy - FrameTime, 0);

	if (b->fusion_charge >= DUEL_FUSION_FULL || !p->energy)
		return 1;
	return on_aim && b->fusion_charge >= DUEL_FUSION_RELEASE;
}

static void shoot(duel_bot *b, object *bot, const object *enemy, int weapon, const vms_vector *aim, fix dist)
{
	int missile;

	if (!b->visible || GameTime64 - b->sighted_at < skill(b)->reaction)
		return;

	if (on_target(bot, enemy, aim))
		b->trigger_until = GameTime64 + DUEL_TRIGGER_HOLD;

	Idle_reason_aim = !(GameTime64 < b->trigger_until && aimed_near(bot, aim));

	if (weapon == FUSION_INDEX)
	{
		if (GameTime64 >= b->next_primary && charge_fusion(b, GameTime64 < b->trigger_until && aimed_near(bot, aim)))
			fire_primary(b, bot, weapon);
	}
	else if (weapon >= 0 && GameTime64 < b->trigger_until && GameTime64 >= b->next_primary && aimed_near(bot, aim))
		fire_primary(b, bot, weapon);

	if (GameTime64 < b->next_secondary)
		return;

	missile = pick_secondary(b, bot, enemy, dist);
	if (missile >= 0)
		fire_secondary(b, bot, missile);
}

//	-------------------------------------------------------------------------
//	Flying
//	-------------------------------------------------------------------------

// keys are digital: an axis is either held fully or not at all
static fix key_press(fix component, fix peak)
{
	if (abs(component) * DUEL_KEY_SHARE < peak)
		return 0;

	return component > 0 ? Player_ship->max_thrust : -Player_ship->max_thrust;
}

// full keys on every axis it wants: exactly what a human trichord can do
static void apply_thrust(object *bot, const vms_vector *move)
{
	fix r = vm_vec_dot((vms_vector *)move, &bot->orient.rvec);
	fix u = vm_vec_dot((vms_vector *)move, &bot->orient.uvec);
	fix f = vm_vec_dot((vms_vector *)move, &bot->orient.fvec);
	fix peak = max(abs(r), max(abs(u), abs(f)));
	vms_vector *thrust = &bot->mtype.phys_info.thrust;

	if (peak < F1_0/64)
	{
		vm_vec_zero(thrust);
		return;
	}

	vm_vec_copy_scale(thrust, &bot->orient.rvec, key_press(r, peak));
	vm_vec_scale_add2(thrust, &bot->orient.uvec, key_press(u, peak));
	vm_vec_scale_add2(thrust, &bot->orient.fvec, key_press(f, peak));
}

// the segment's floor is "down", the way a human keeps their bearings
static void level_up(const object *bot, vms_vector *up)
{
	*up = Segments[bot->segnum].sides[WBOTTOM].normals[0];
}

static int upside_down(const object *bot)
{
	vms_vector up;

	level_up(bot, &up);
	return vm_vec_dot(&up, (vms_vector *)&bot->orient.uvec) < 0;
}

// target behind: yaw round through the side rather than looping over the top
static void swing_dir(const object *bot, vms_vector *dir)
{
	if (vm_vec_dot(dir, (vms_vector *)&bot->orient.fvec) >= 0)
		return;

	vm_vec_scale_add2(dir, (vms_vector *)&bot->orient.uvec, -vm_vec_dot(dir, (vms_vector *)&bot->orient.uvec));
	vm_vec_scale_add2(dir, (vms_vector *)&bot->orient.fvec, -vm_vec_dot(dir, (vms_vector *)&bot->orient.fvec));

	if (vm_vec_normalize(dir) < F1_0/16)
		*dir = bot->orient.rvec;
}

// rotate the nose a fixed angle along the great circle; snaps when within one step
static void turn_toward(const object *bot, const vms_vector *dir, fix step, int ease, vms_vector *fvec)
{
	fix dot = vm_vec_dot((vms_vector *)dir, (vms_vector *)&bot->orient.fvec);
	fix error = fix_acos(dot);
	fix step_ang = fixdiv(step, DUEL_TWO_PI);
	vms_vector perp = *dir;
	fix sin_step, cos_step;

	if (ease)	// a hand on a mouse slows into the turn; aiming doesn't
		step_ang = min(step_ang, fixmul(error, min(F1_0, fixmul(FrameTime, DUEL_TURN_EASE))));
	if (error <= step_ang)
	{
		*fvec = *dir;
		return;
	}

	vm_vec_scale_add2(&perp, (vms_vector *)&bot->orient.fvec, -dot);
	if (vm_vec_normalize(&perp) < F1_0/64)
		perp = bot->orient.rvec;

	fix_sincos(step_ang, &sin_step, &cos_step);
	vm_vec_copy_scale(fvec, (vms_vector *)&bot->orient.fvec, cos_step);
	vm_vec_scale_add2(fvec, &perp, sin_step);
	vm_vec_normalize(fvec);
}

// turn no faster than a human can, and roll back upright while doing it
static void face(const duel_bot *b, object *bot, const vms_vector *point)
{
	vms_vector dir, fvec, up, floor_up;
	fix step = fixmul(FrameTime, skill(b)->turn_rate);

	vm_vec_sub(&dir, (vms_vector *)point, &bot->pos);
	if (vm_vec_normalize(&dir) < F1_0/16)
		dir = bot->orient.fvec;

	swing_dir(bot, &dir);
	turn_toward(bot, &dir, step, !b->visible, &fvec);

	level_up(bot, &floor_up);
	up = bot->orient.uvec;
	vm_vec_scale_add2(&up, &floor_up, fixmul(FrameTime, DUEL_LEVEL_RATE));

	vm_vector_2_matrix(&bot->orient, &fvec, &up, NULL);
}

// nose stays level, yawed ~55 degrees off the route and rolled 45, so the
// belly and one wing face the wind: forward + slide down + slide sideways
// all push along the route. Keeps the side the nose is already on.
static void trichord_frame(const object *bot, const vms_vector *travel, const vms_vector *nose, vms_vector *fvec, vms_vector *uvec)
{
	const fix third = fl2f(0.57735), two_thirds = fl2f(0.81650), half_root2 = fl2f(0.70711);
	vms_vector floor_up, lift, side, wind;

	level_up(bot, &floor_up);
	lift = floor_up;
	vm_vec_scale_add2(&lift, travel, -vm_vec_dot(&floor_up, (vms_vector *)travel));
	if (vm_vec_normalize(&lift) < F1_0/16)
		lift = bot->orient.uvec;
	vm_vec_cross(&side, (vms_vector *)travel, &lift);
	vm_vec_normalize(&side);
	if (vm_vec_dot(&side, (vms_vector *)nose) < 0)
		vm_vec_negate(&side);

	vm_vec_copy_scale(fvec, travel, third);
	vm_vec_scale_add2(fvec, &side, two_thirds);
	vm_vec_copy_scale(&wind, travel, two_thirds);
	vm_vec_scale_add2(&wind, &side, -third);
	vm_vec_copy_scale(uvec, &lift, half_root2);
	vm_vec_scale_add2(uvec, &wind, -half_root2);
}

static void face_frame(const duel_bot *b, object *bot, const vms_vector *want_f, const vms_vector *want_u)
{
	vms_vector fvec, up;
	fix step = fixmul(FrameTime, skill(b)->turn_rate);

	turn_toward(bot, want_f, step, 1, &fvec);
	up = bot->orient.uvec;
	vm_vec_scale_add2(&up, (vms_vector *)want_u, fixmul(FrameTime, DUEL_LEVEL_RATE));
	vm_vector_2_matrix(&bot->orient, &fvec, &up, NULL);
}

static fix side_view(const duel_bot *b, const object *bot, const vms_vector *nose, vms_vector *fvec)
{
	vms_vector uvec, to_seen;
	fix view;

	trichord_frame(bot, &b->travel_dir, nose, fvec, &uvec);
	view = free_distance(bot, fvec);
	if (b->seen_at && GameTime64 - b->seen_at < DUEL_SIDE_SEEN)
	{
		vm_vec_sub(&to_seen, (vms_vector *)&b->last_seen_pos, (vms_vector *)&bot->pos);
		vm_vec_normalize(&to_seen);
		view += fixmul(vm_vec_dot(&to_seen, fvec), DUEL_OPEN_PROBE);
	}
	return view;
}

// nose toward the open room or where they were, never the wall
static void pick_trichord_side(duel_bot *b, const object *bot)
{
	vms_vector here, there, flipped = bot->orient.fvec;
	fix keep;

	if (GameTime64 < b->side_pick_at)
		return;
	b->side_pick_at = GameTime64 + DUEL_SIDE_PICK;
	keep = side_view(b, bot, &bot->orient.fvec, &here);
	vm_vec_negate(&flipped);
	b->trichord_nose = side_view(b, bot, &flipped, &there) > fixmul(keep, DUEL_SIDE_BETTER) ? there : here;
}

static void trichord(duel_bot *b, object *bot, const vms_vector *move)
{
	vms_vector want_f, want_u;
	fix follow = min(F1_0, fixmul(FrameTime, DUEL_TRAVEL_SMOOTH));

	vm_vec_scale_add2(&b->travel_dir, (vms_vector *)move, follow);
	if (vm_vec_normalize(&b->travel_dir) < F1_0/16)
		b->travel_dir = *move;
	pick_trichord_side(b, bot);
	trichord_frame(bot, &b->travel_dir, &b->trichord_nose, &want_f, &want_u);
	face_frame(b, bot, &want_f, &want_u);
}

// doors are bumped open on purpose; anything else at speed was a mistake
void duelbot_hit_wall(const object *ship, fix speed, int segnum, int side)
{
	duel_bot *b = cpu_thinking() ? cpu_for_pnum(ship->id) : NULL;
	int wall_num = Segments[segnum].sides[side].wall_num;

	if (wall_num >= 0 && Walls[wall_num].type == WALL_DOOR)
		return;

	if (!b && Pilot_on && ship == ConsoleObject)
		b = &Pilot;
	if (b && speed > DUEL_WALL_HIT_SPEED)
	{
		b->stats.wall_hits++;
		b->flight.hits++;
	}
}

// thrust toward the velocity it wants, so sideways drift gets braked out
// fast enough to still slow to cornering speed by the end of the clear run
static fix corner_speed(const duel_bot *b, const object *bot, const vms_vector *look)
{
	fix room = vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)look);

	// the end of the trip is a corner too: arrive, don't crash
	room = min(room, vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&b->goal_pos));
	fix brake = max_ship_accel() / DUEL_BRAKE_SHARE;
	fix64 v2 = (fix64)DUEL_MIN_CORNER_SPEED * DUEL_MIN_CORNER_SPEED / F1_0 + 2 * (fix64)brake * room / F1_0;

	return min(DUEL_TOP_SPEED, fix_sqrt((fix)min(v2, (fix64)0x7fffffff)));
}

static int over_corner_speed(const duel_bot *b, const object *bot, const vms_vector *look)
{
	return vm_vec_mag((vms_vector *)&bot->mtype.phys_info.velocity) > corner_speed(b, bot, look);
}

// brakes for the turn: never faster than the clear run ahead allows
static void steer(const duel_bot *b, const object *bot, const vms_vector *move, const vms_vector *look, vms_vector *thrust_dir)
{
	vm_vec_copy_scale(thrust_dir, (vms_vector *)move, corner_speed(b, bot, look));
	vm_vec_sub2(thrust_dir, (vms_vector *)&bot->mtype.phys_info.velocity);
}

static void stabilize(object *bot)
{
	fix keep = F1_0 - min(F1_0, fixmul(FrameTime, DUEL_SPIN_DAMP));

	vm_vec_scale(&bot->mtype.phys_info.rotvel, keep);
}

static fix side_distance(const object *bot, int side, const vms_vector *normal)
{
	segment *seg = &Segments[bot->segnum];
	vms_vector *corner = &Vertices[seg->verts[Side_to_verts[side][0]]];
	vms_vector rel;

	vm_vec_sub(&rel, (vms_vector *)&bot->pos, corner);
	return vm_vec_dot(&rel, (vms_vector *)normal);
}

// D3-style: solid sides of this cube push back harder the closer they are
static void wall_repulsion(const object *bot, vms_vector *push)
{
	segment *seg = &Segments[bot->segnum];
	fix reach = bot->size + DUEL_WALL_MARGIN;
	int side;

	vm_vec_zero(push);

	for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
	{
		vms_vector *normal = &seg->sides[side].normals[0];
		fix dist;

		if (side_passable(seg, side))
			continue;

		dist = side_distance(bot, side, normal);
		if (dist >= reach)
			continue;

		vm_vec_scale_add2(push, normal, fixdiv(reach - max(dist, 0), DUEL_WALL_MARGIN));
	}
}

static int wall_ahead(const object *bot, const vms_vector *dir)
{
	vms_vector probe;

	vm_vec_scale_add(&probe, (vms_vector *)&bot->pos, (vms_vector *)dir, DUEL_WALL_PROBE);
	return !clear_line(bot, &probe, bot->size);
}

// the enemy's nose is lined up on it
static int under_aim(const object *bot, const object *enemy)
{
	vms_vector to_bot;

	vm_vec_sub(&to_bot, (vms_vector *)&bot->pos, (vms_vector *)&enemy->pos);
	vm_vec_normalize(&to_bot);
	return vm_vec_dot(&to_bot, (vms_vector *)&enemy->orient.fvec) > DUEL_AIMED_AT_DOT;
}

// smooth push in or out toward the held range, dead in the band
static fix range_push(fix dist, fix range)
{
	fix error;

	if (dist < DUEL_TOO_CLOSE)
		return -F1_0;

	error = fixdiv(dist - range, range);
	if (abs(error) < DUEL_RANGE_BAND)
		return 0;

	return clamp_unit(error * 2);
}

static void update_strafe(duel_bot *b, const object *bot, const object *enemy)
{
	if (GameTime64 >= b->strafe_flip_at)
	{
		b->strafe_sign = -b->strafe_sign;
		b->climb_sign = random_sign();
		b->strafe_flip_at = GameTime64 + DUEL_STRAFE_MIN + random_fix(DUEL_STRAFE_SPAN);
	}

	// missiles in play: a reversal or a stop is a sitting duck for the blast
	if (GameTime64 < b->missile_alert_until)
		return;

	// lined up in the enemy's sights: break rhythm fast, sometimes with a hard stop
	if (under_aim(bot, enemy) && b->strafe_flip_at > GameTime64 + DUEL_JINK_MIN)
	{
		b->strafe_flip_at = GameTime64 + DUEL_JINK_MIN + random_fix(DUEL_JINK_MIN);
		if (d_rand() < DUEL_HARD_STOP_ODDS)
			b->hard_stop_until = GameTime64 + DUEL_HARD_STOP;
	}
}

static void hard_stop_move(const object *bot, vms_vector *move)
{
	vm_vec_copy_scale(move, (vms_vector *)&bot->mtype.phys_info.velocity, -F1_0);
}

// drift onto a worthwhile pickup in plain reach while still fighting
static void add_grab_pull(const duel_bot *b, const object *bot, vms_vector *move)
{
	const object *best = NULL;
	fix best_score = 0;
	vms_vector dir;
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		const object *pow = &Objects[i];
		fix dist, score;
		int value;

		if (pow->type != OBJ_POWERUP || !(value = powerup_value(b, pow->id)))
			continue;
		dist = vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&pow->pos);
		if (dist > DUEL_GRAB_RANGE || !clear_line(bot, &pow->pos, bot->size))
			continue;
		score = fixdiv(i2f(value), dist + i2f(10));
		if (score > best_score)
			best_score = score, best = pow;
	}

	if (!best)
		return;
	vm_vec_sub(&dir, (vms_vector *)&best->pos, (vms_vector *)&bot->pos);
	vm_vec_normalize(&dir);
	vm_vec_scale_add2(move, &dir, DUEL_GRAB_PULL);
}

static void combat_move(duel_bot *b, const object *bot, const object *enemy, int weapon, vms_vector *move)
{
	vms_vector to_enemy, strafe;
	fix dist, range;

	vm_vec_sub(&to_enemy, (vms_vector *)&enemy->pos, (vms_vector *)&bot->pos);
	dist = vm_vec_normalize(&to_enemy);
	range = fixmul(primary_range(weapon >= 0 ? weapon : LASER_INDEX), Range_scale[b->range.arm]);
	range = deny_range(b->target, range);
	if (b->mode == MODE_BULL)
		range = range * 6 / 10;		// apply pressure

	update_strafe(b, bot, enemy);
	if (GameTime64 < b->hard_stop_until)
	{
		hard_stop_move(bot, move);
		return;
	}

	vm_vec_cross(&strafe, &to_enemy, (vms_vector *)&bot->orient.uvec);
	if (vm_vec_normalize(&strafe) < F1_0/16)
		strafe = bot->orient.rvec;
	vm_vec_scale(&strafe, i2f(b->strafe_sign));

	if (wall_ahead(bot, &strafe))
	{
		b->strafe_sign = -b->strafe_sign;
		vm_vec_negate(&strafe);
	}

	vm_vec_copy_scale(move, &to_enemy, range_push(dist, range));
	vm_vec_add2(move, &strafe);
	vm_vec_scale_add2(move, (vms_vector *)&bot->orient.uvec, i2f(b->climb_sign));
	add_grab_pull(b, bot, move);
}

static void route_move(const object *bot, const vms_vector *point, vms_vector *move)
{
	vm_vec_sub(move, (vms_vector *)point, (vms_vector *)&bot->pos);
	vm_vec_normalize(move);
}

static int check_stuck(duel_bot *b, const object *bot, vms_vector *move)
{
	// holding still on purpose is not being stuck
	if (b->mode == MODE_CAMP || b->mode == MODE_PREFIRE)
	{
		b->stuck_pos = bot->pos;
		b->stuck_since = GameTime64;
		return 0;
	}

	if (vm_vec_dist((vms_vector *)&bot->pos, &b->stuck_pos) > DUEL_STUCK_DIST)
	{
		b->stuck_pos = bot->pos;
		b->stuck_since = GameTime64;
		return 0;
	}

	if (GameTime64 < b->unstick_until)
	{
		*move = b->unstick_dir;
		return 1;
	}

	if (GameTime64 - b->stuck_since < DUEL_STUCK_TIME)
		return 0;

	avoid_goal(b);
	make_random_vector(&b->unstick_dir);
	b->unstick_until = GameTime64 + DUEL_UNSTICK_TIME;
	b->stuck_since = GameTime64;
	b->next_path = 0;
	b->patrol_seg = -1;
	b->stats.stuck_events++;
	*move = b->unstick_dir;
	return 1;
}

//	-------------------------------------------------------------------------
//	Tactics (Descent 101)
//	-------------------------------------------------------------------------

static int clear_between(const vms_vector *from, int from_seg, const vms_vector *to)
{
	fvi_query fq;
	fvi_info hit;

	fq.p0 = (vms_vector *)from;
	fq.p1 = (vms_vector *)to;
	fq.startseg = from_seg;
	fq.rad = 0;
	fq.thisobjnum = -1;
	fq.ignore_obj_list = NULL;
	fq.flags = FQ_TRANSWALL;

	return find_vector_intersection(&fq, &hit) == HIT_NONE;
}

static fix free_distance(const object *bot, const vms_vector *dir)
{
	fvi_query fq;
	fvi_info hit;
	vms_vector end;

	vm_vec_scale_add(&end, (vms_vector *)&bot->pos, (vms_vector *)dir, DUEL_OPEN_PROBE);
	fq.p0 = (vms_vector *)&bot->pos;
	fq.p1 = &end;
	fq.startseg = bot->segnum;
	fq.rad = 0;
	fq.thisobjnum = bot - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;

	if (find_vector_intersection(&fq, &hit) == HIT_NONE)
		return DUEL_OPEN_PROBE;
	return vm_vec_dist(&hit.hit_pnt, (vms_vector *)&bot->pos);
}

// hallway or big room: mean free distance along the ship's six axes
static void update_openness(duel_bot *b, const object *bot)
{
	const vms_vector *axes[3] = { &bot->orient.rvec, &bot->orient.uvec, &bot->orient.fvec };
	fix total = 0;
	int i;

	if (GameTime64 < b->next_openness)
		return;
	b->next_openness = GameTime64 + DUEL_OPEN_REFRESH;

	for (i = 0; i < 3; i++)
	{
		vms_vector back = *axes[i];

		vm_vec_negate(&back);
		total += free_distance(bot, axes[i]) + free_distance(bot, &back);
	}

	b->openness = total / 6;
}

// a wreck keeps its momentum: it blows where it drifts to, not where it died
static void wreck_drift(const object *wreck, fix remaining, vms_vector *pos)
{
	vm_vec_scale_add(pos, (vms_vector *)&wreck->pos, (vms_vector *)&wreck->mtype.phys_info.velocity,
					 min(max(remaining, 0), DUEL_DEATH_TIME) / 2);
}

// when this ship blows, if it's dying; 0 if not
static fix64 blast_time(int pnum)
{
	duel_bot *owner = bot_for_pnum(pnum);

	if (owner)
		return owner->dying_until;
	if (!Dying_since[pnum] || !ship_dying(pnum))
		return 0;
	return Dying_since[pnum] + DUEL_DEATH_TIME;
}

// the nearest place a dying ship is about to blow
static int blast_point(const duel_bot *b, const object *bot, vms_vector *pos)
{
	int p, found = 0;
	fix best = 0;

	for (p = 0; p < MAX_PLAYERS; p++)
	{
		fix64 when = (p == b->pnum) ? 0 : blast_time(p);
		object *wreck;
		vms_vector at;
		fix dist;

		if (!when)
			continue;
		wreck = &Objects[Players[p].objnum];
		if (wreck->type != OBJ_PLAYER)
			continue;

		wreck_drift(wreck, when - GameTime64, &at);
		dist = vm_vec_dist(&at, (vms_vector *)&bot->pos);
		if (!found || dist < best)
			found = 1, best = dist, *pos = at;
	}

	return found;
}

static int in_blast_danger(const duel_bot *b, const object *bot, vms_vector *pos)
{
	return blast_point(b, bot, pos) && vm_vec_dist(pos, (vms_vector *)&bot->pos) < DUEL_BLAST_CLEARANCE;
}

// nearest spot a few cubes away that the enemy can't see
static int find_cover(duel_bot *b, const object *bot, const object *enemy)
{
	int s, best = -1;

	bfs(bot->segnum);

	for (s = 0; s <= Highest_segment_index; s++)
	{
		vms_vector center;

		if (!reachable(s) || Bfs_depth[s] == 0 || Bfs_depth[s] > DUEL_COVER_DEPTH)
			continue;
		if (best >= 0 && Bfs_depth[s] >= Bfs_depth[best])
			continue;

		compute_segment_center(&center, &Segments[s]);
		if (clear_between(&center, s, &enemy->pos))
			continue;
		best = s;
	}

	if (best < 0)
		return 0;

	store_path(b, bot->segnum, best);
	compute_segment_center(&b->cover_pos, &Segments[best]);
	b->goal_pos = b->cover_pos;
	b->goal_seg = best;
	return 1;
}

// the busiest way into a cube: where to point while sitting in it
static void camp_look_point(int segnum, vms_vector *look)
{
	int side, best_child = -1;

	for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
	{
		int child = Segments[segnum].children[side];

		if (!IS_CHILD(child) || !side_passable(&Segments[segnum], side))
			continue;
		if (best_child < 0 || Brain.heat[child] > Brain.heat[best_child])
			best_child = child;
	}

	if (best_child < 0)
		best_child = segnum;
	compute_segment_center(look, &Segments[best_child]);
}

static void start_camp(duel_bot *b, int segnum, const vms_vector *look, fix length)
{
	b->camp_seg = segnum;
	b->camp_until = GameTime64 + length;
	b->next_camp = b->camp_until + DUEL_CAMP_COOLDOWN;
	b->next_path = 0;

	if (look)
		b->camp_look = *look;
	else
		camp_look_point(segnum, &b->camp_look);
}

// weapon guarding: sit on the spew so the respawned enemy can't re-arm from it
// the enemy is dead: nothing to pre-fire at or chase, so watch the ways in
// honey pot when there's a stash worth fighting over, else the busiest cube
static void maybe_start_camp(duel_bot *b, const object *bot)
{
	int seg;

	if (b->camp_until || GameTime64 < b->next_camp)
		return;
	if (GameTime64 - b->seen_at < DUEL_CAMP_AFTER || aggression(b) < DUEL_CHASE_AGGRESSION)
		return;

	seg = hottest_segment(-1);
	if (seg < 0)
		seg = far_start_segment(bot);
	start_camp(b, seg, NULL, DUEL_CAMP_TIME);
}

static int path_to_camp(duel_bot *b, const object *bot)
{
	if (!path_to_segment(b, bot, b->camp_seg))
	{
		b->camp_until = 0;
		return 0;
	}

	compute_segment_center(&b->goal_pos, &Segments[b->camp_seg]);
	b->goal_seg = b->camp_seg;
	return 1;
}

// pre-fire: where an enemy that just broke sight should be by now
static int prefire_point(const duel_bot *b, const object *bot, vms_vector *point)
{
	fix since = GameTime64 - b->seen_at;

	if (b->visible || !b->seen_at || since > DUEL_PREFIRE_WINDOW)
		return 0;

	vm_vec_scale_add(point, (vms_vector *)&b->last_seen_pos, (vms_vector *)&b->enemy_vel, min(since, F1_0));
	return vm_vec_dist(point, (vms_vector *)&bot->pos) < DUEL_PREFIRE_RANGE &&
		clear_line(bot, point, 0);
}

// a smart sent where they should be by now, not where they were
static int smart_trap_point(const duel_bot *b, const object *bot, vms_vector *point)
{
	fix64 since = GameTime64 - b->seen_at;
	fvi_query fq;
	fvi_info hit;

	if (!bot_player(b)->secondary_ammo[SMART_INDEX] || GameTime64 < b->next_secondary || match_tense())
		return 0;
	if (b->visible || !b->seen_at || since < DUEL_TRAP_MIN || since > DUEL_TRAP_MAX)
		return 0;
	predicted_segment(b, point);
	if (vm_vec_dist(point, (vms_vector *)&bot->pos) > DUEL_TRAP_RANGE)
		return 0;

	fq.p0 = (vms_vector *)&bot->pos;
	fq.p1 = point;
	fq.startseg = bot->segnum;
	fq.rad = 0;
	fq.thisobjnum = bot - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = 0;
	if (find_vector_intersection(&fq, &hit) != HIT_NONE &&
		vm_vec_dist(&hit.hit_pnt, point) > DUEL_TRAP_SLACK)
		return 0;
	return splash_safe_toward(bot, SMART_INDEX, vm_vec_dist(&hit.hit_pnt, (vms_vector *)&bot->pos));
}

// pre-fire is a plasma/quads job, and only with the gun already in hand
static int prefire_weapon(const duel_bot *b)
{
	if (Players[b->pnum].energy < DUEL_PREFIRE_ENERGY || !holding_usable(b))
		return -1;
	if (b->held_weapon == FUSION_INDEX)
		return -1;		// a charge can't be sprayed at a corner
	return b->held_weapon;
}

static void prefire(duel_bot *b, object *bot, const vms_vector *point)
{
	int weapon = prefire_weapon(b);
	player *p = bot_player(b);

	if (!aimed_near(bot, point) || own_mine_ahead(bot))
		return;
	if (weapon >= 0 && GameTime64 >= b->next_primary)
		fire_primary(b, bot, weapon);

	// smart trap: seed the corner they're about to come round
	if (p->secondary_ammo[SMART_INDEX] && GameTime64 >= b->next_secondary && !match_tense() &&
		splash_safe_toward(bot, SMART_INDEX, vm_vec_dist((vms_vector *)point, &bot->pos)))
		fire_secondary(b, bot, SMART_INDEX);
}

// Always Be Escaping, unless the enemy is weak enough to run down
static int choose_fight_mode(duel_bot *b, const object *bot, const object *enemy, int weapon)
{
	if (weapon < 0 || aggression(b) < DUEL_RETREAT_AGGRESSION)
		return MODE_RETREAT;
	if (undergunned(b) && vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&enemy->pos) > DUEL_UNDERGUN_FLEE)
		return MODE_RETREAT;	// shoot while running for a gun

	if (target_shields(b) < bull_shields(b) && aggression(b) > DUEL_CHASE_AGGRESSION)
	{
		b->engage_until = 0;
		return MODE_BULL;
	}

	if (GameTime64 < b->break_until)
		return MODE_BREAK;

	if (!b->engage_until)
		b->engage_until = GameTime64 + DUEL_ENGAGE_MIN + random_fix(DUEL_ENGAGE_SPAN);

	if (GameTime64 < b->engage_until || !find_cover(b, bot, enemy))
		return MODE_FIGHT;

	b->engage_until = 0;
	b->break_until = GameTime64 + DUEL_BREAK_TIME;
	return MODE_BREAK;
}

// spawn control: saturate the nearest respawn point it can see with plasma
static int choose_mode(duel_bot *b, const object *bot, const object *enemy, int weapon, vms_vector *point)
{
	if (in_blast_danger(b, bot, point))
		return MODE_BLAST;

	if (b->visible && enemy)
		return choose_fight_mode(b, bot, enemy, weapon);

	if (GameTime64 - b->seen_at > DUEL_NEW_FIGHT)
		b->engage_until = 0;	// a flicker out of view is the same fight

	if (smart_trap_point(b, bot, point))
		return MODE_PREFIRE;
	if (prefire_point(b, bot, point) &&
		(prefire_weapon(b) >= 0 || GameTime64 - b->seen_at < DUEL_HOLD_ANGLE))
		return MODE_PREFIRE;	// hold the angle they left by

	maybe_start_camp(b, bot);
	if (b->camp_until && GameTime64 < b->camp_until)
		return MODE_CAMP;

	b->camp_until = 0;
	return MODE_HUNT;
}

static void blast_move(const object *bot, const vms_vector *blast, vms_vector *move)
{
	vm_vec_sub(move, (vms_vector *)&bot->pos, (vms_vector *)blast);
	if (vm_vec_normalize(move) < F1_0/16)
		*move = bot->orient.rvec;
}

// sit still in the camp cube: stillness gives nothing away
static void camp_move(duel_bot *b, const object *bot, vms_vector *waypoint, vms_vector *move)
{
	if (GameTime64 >= b->next_path)
	{
		b->next_path = GameTime64 + DUEL_PATH_REFRESH;
		path_to_camp(b, bot);
	}

	route_point(b, bot, waypoint);
	if (bot->segnum == b->camp_seg)
	{
		vm_vec_zero(move);		// steer() then brakes to a hover
		*waypoint = b->camp_look;
		return;
	}
	route_move(bot, waypoint, move);
}

//	-------------------------------------------------------------------------
//	Think
//	-------------------------------------------------------------------------

static void log_state(duel_bot *b, int fighting, int retreating, int weapon, fix dist)
{
	player *p = bot_player(b);

	if (GameTime64 < b->next_log)
		return;

	b->next_log = GameTime64 + DUEL_LOG_INTERVAL;
	con_printf(CON_DEBUG, "duelbot%d: %s tgt=%d wpn=%d dist=%d sh=%d en=%d seg=%d goal=%d path=%d/%d speed=%d goaldist=%d\n",
			   b->pnum, Mode_names[b->mode], b->target, weapon,
			   b->target >= 0 && live_player_object(b->target) ? f2i(vm_vec_dist(&Objects[p->objnum].pos, &live_player_object(b->target)->pos)) : -1,
			   f2i(p->shields), f2i(p->energy), Objects[p->objnum].segnum,
			   b->path_len ? b->path[b->path_len - 1] : -1, b->path_pos, b->path_len,
			   f2i(vm_vec_mag(&Objects[p->objnum].mtype.phys_info.velocity)),
			   f2i(vm_vec_dist(&Objects[p->objnum].pos, &b->goal_pos)));
}

// why a visible enemy is not being shot at
static void tally_idle(const duel_bot *b, duel_stats *s)
{
	if (GameTime64 - b->sighted_at <= skill(b)->reaction)
		s->idle_reacting += FrameTime;
	else if (GameTime64 < b->next_primary)
		s->idle_cooldown += FrameTime;
	else if (Idle_reason_aim)
		s->idle_off_aim += FrameTime;
	s->idle_gun += FrameTime;
}

static void tally(duel_bot *b, const object *bot, int fighting, int retreating, int weapon, int stuck)
{
	duel_stats *s = &b->stats;

	s->alive += FrameTime;
	s->mode_time[b->mode] += FrameTime;
	if (b->mode != MODE_CAMP && b->mode != MODE_PREFIRE &&
		vm_vec_mag((vms_vector *)&bot->mtype.phys_info.velocity) < i2f(3))
		s->frozen += FrameTime;
	if (b->visible)
		s->visible += FrameTime;
	if (b->visible && weapon < 0)
		s->no_gun += FrameTime;
	if (b->visible && weapon >= 0 && GameTime64 - b->last_shot_at > DUEL_IDLE_GUN)
		tally_idle(b, s);
	if (upside_down(bot))
		s->upside += FrameTime;
	if (stuck)
		s->stuck += FrameTime;
	if (retreating)
		s->retreat += FrameTime;
	else if (fighting)
		s->fight += FrameTime;
	else
		s->hunt += FrameTime;
}

// the last straight run into a pickup: closets and alcoves are all walls,
// and shying off them would bounce the ship out of the doorway forever
static int closing_on_pickup(const duel_bot *b, const object *bot)
{
	return b->mode == MODE_HUNT && b->goal_seg >= 0 &&
		   vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&b->goal_pos) < DUEL_PICKUP_APPROACH &&
		   clear_line(bot, &b->goal_pos, bot->size);
}

static void push_away(const object *bot, const object *mine, fix dist, vms_vector *move)
{
	vms_vector away;

	if (dist >= DUEL_MINE_CLEARANCE)
		return;
	vm_vec_sub(&away, (vms_vector *)&bot->pos, (vms_vector *)&mine->pos);
	vm_vec_normalize(&away);
	vm_vec_scale_add2(move, &away, fixdiv(DUEL_MINE_CLEARANCE - dist, DUEL_MINE_CLEARANCE) * 3);
}

// the roomier of the four sideways directions, for a mine dead ahead
static void roomiest_side(const object *bot, vms_vector *dir)
{
	const vms_vector *axes[2] = { &bot->orient.rvec, &bot->orient.uvec };
	fix best = -1;
	int a, sign;

	for (a = 0; a < 2; a++)
		for (sign = -1; sign <= 1; sign += 2)
		{
			vms_vector cand;
			fix room;

			vm_vec_copy_scale(&cand, (vms_vector *)axes[a], sign * F1_0);
			room = free_distance(bot, &cand);
			if (room > best)
			{
				best = room;
				*dir = cand;
			}
		}
}

// slide past at an angle: the mine's hull is small, the route only needs a gap
static void slip_past(const object *bot, const object *mine, vms_vector *move)
{
	vms_vector rel, lateral, out;
	fix ahead, offset, needed = bot->size + mine->size + DUEL_MINE_GAP_SPARE;

	vm_vec_sub(&rel, (vms_vector *)&mine->pos, (vms_vector *)&bot->pos);
	ahead = vm_vec_dot(&rel, move);
	lateral = rel;
	vm_vec_scale_add2(&lateral, move, -ahead);
	offset = vm_vec_mag(&lateral);
	if (offset >= needed)
		return;

	if (offset < F1_0/2)
		roomiest_side(bot, &out);
	else
		vm_vec_copy_scale(&out, &lateral, -fixdiv(F1_0, offset));
	vm_vec_scale_add2(move, &out, fixdiv(needed - offset, needed) * 2);
}

static void avoid_mines(const object *bot, vms_vector *move)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *mine = &Objects[i];
		vms_vector rel;
		fix dist;

		if (mine->type != OBJ_WEAPON || mine->id != PROXIMITY_ID)
			continue;
		vm_vec_sub(&rel, &mine->pos, (vms_vector *)&bot->pos);
		dist = vm_vec_mag(&rel);
		if (dist > DUEL_MINE_LOOK)
			continue;
		if (vm_vec_dot(&rel, move) > 0)
			slip_past(bot, mine, move);
		else
			push_away(bot, mine, dist, move);
	}
}

// a mine on the route is cheaper shot than squeezed past, from outside its blast
static object *mine_to_shoot(const object *bot, const vms_vector *heading)
{
	fix safe = Weapon_info[PROXIMITY_ID].damage_radius + splash_margin();
	object *best = NULL;
	fix best_dist = DUEL_MINE_SHOOT_MAX;
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *mine = &Objects[i];
		vms_vector rel;
		fix dist;

		if (mine->type != OBJ_WEAPON || mine->id != PROXIMITY_ID)
			continue;
		vm_vec_sub(&rel, &mine->pos, (vms_vector *)&bot->pos);
		dist = vm_vec_normalize(&rel);
		if (dist <= safe || dist >= best_dist || vm_vec_dot(&rel, (vms_vector *)heading) < F1_0/2)
			continue;
		if (!clear_line(bot, &mine->pos, 0))
			continue;
		best = mine;
		best_dist = dist;
	}
	return best;
}

static void shoot_mine(duel_bot *b, object *bot, const object *mine)
{
	int weapon = holding_usable(b) && b->held_weapon != FUSION_INDEX ? b->held_weapon : LASER_INDEX;

	if (GameTime64 >= b->next_primary && aimed_near(bot, &mine->pos))
		fire_primary(b, bot, weapon);
}

static void add_avoidance(duel_bot *b, const object *bot, vms_vector *move)
{
	vms_vector push;

	if (GameTime64 < b->dodge_until)
		vm_vec_scale_add2(move, &b->dodge_dir, b->dodge_strength * 2);

	if (!closing_on_pickup(b, bot))
	{
		wall_repulsion(bot, &push);
		vm_vec_add2(move, &push);
	}
	avoid_mines(bot, move);
}

static int mode_fights(int mode)
{
	return mode == MODE_FIGHT || mode == MODE_BULL || mode == MODE_BREAK || mode == MODE_RETREAT;
}

// where to go and where to look, per mode
// run for a corner first: a straight line to the pickup is a straight line for their plasma
static int retreat_to_cover(duel_bot *b, const object *bot, const object *enemy)
{
	if (GameTime64 < b->cover_until)
		return 1;
	if (!enemy || !b->visible || GameTime64 < b->next_path)
		return 0;
	b->next_path = GameTime64 + DUEL_PATH_REFRESH;
	if (!find_cover(b, bot, enemy))
		return 0;
	b->cover_until = GameTime64 + DUEL_BREAK_TIME;
	return 1;
}

// holding an angle still means moving: a hovering ship is the easiest kill there is
static void hold_angle_move(duel_bot *b, const object *bot, const vms_vector *point, vms_vector *move)
{
	vms_vector to_point, strafe;

	vm_vec_sub(&to_point, (vms_vector *)point, (vms_vector *)&bot->pos);
	if (vm_vec_normalize(&to_point) < F1_0/16)
		to_point = bot->orient.fvec;
	vm_vec_cross(&strafe, &to_point, (vms_vector *)&bot->orient.uvec);
	if (vm_vec_normalize(&strafe) < F1_0/16)
		strafe = bot->orient.rvec;
	if (GameTime64 >= b->strafe_flip_at)
	{
		b->strafe_sign = -b->strafe_sign;
		b->strafe_flip_at = GameTime64 + DUEL_STRAFE_MIN + random_fix(DUEL_STRAFE_SPAN);
	}
	vm_vec_scale(&strafe, i2f(b->strafe_sign));
	if (wall_ahead(bot, &strafe))
	{
		b->strafe_sign = -b->strafe_sign;
		vm_vec_negate(&strafe);
	}
	*move = strafe;
	vm_vec_scale_add2(move, (vms_vector *)&bot->orient.uvec, i2f(b->climb_sign) / 2);
}

static void plan_motion(duel_bot *b, const object *bot, const object *enemy, int weapon,
						const vms_vector *point, vms_vector *move, vms_vector *look)
{
	switch (b->mode)
	{
		case MODE_BLAST:
			blast_move(bot, point, move);
			*look = enemy ? enemy->pos : bot->pos;
			return;
		case MODE_FIGHT:
		case MODE_BULL:
			combat_move(b, bot, enemy, weapon, move);
			return;
		case MODE_BREAK:
			route_point(b, bot, look);
			route_move(bot, look, move);
			return;
		case MODE_PREFIRE:
			hold_angle_move(b, bot, point, move);
			*look = *point;
			return;
		case MODE_CAMP:
			camp_move(b, bot, look, move);
			return;
		case MODE_RETREAT:
			if (retreat_to_cover(b, bot, enemy))
			{
				route_point(b, bot, look);
				route_move(bot, look, move);
				return;
			}
			/* fall through */
		default:	// HUNT, RETREAT
			plan_route(b, bot, b->mode == MODE_RETREAT);
			route_point(b, bot, look);
			route_move(bot, look, move);
	}
}

static int faces_enemy(int mode)
{
	return mode_fights(mode) || mode == MODE_BLAST;
}

// any trip with room left to run gets all three keys
static int travelling(const duel_bot *b, const object *bot, const vms_vector *move)
{
	fix room = b->flight.arm == 3 ? DUEL_EARLY_TRICHORD : DUEL_TRICHORD_DIST;

	return vm_vec_mag((vms_vector *)move) > F1_0/2 &&
		   vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&b->goal_pos) > room;
}

// trichording points the nose off the route: only when nobody's about
static int quiet(const duel_bot *b)
{
	fix64 contact = max(b->seen_at, b->heard_at);

	return b->mode != MODE_PREFIRE && GameTime64 - contact > DUEL_TRICHORD_QUIET;
}

static fix momentum_sign(const object *bot, const vms_vector *axis)
{
	return vm_vec_dot((vms_vector *)&bot->mtype.phys_info.velocity, (vms_vector *)axis) < 0 ? -F1_0 : F1_0;
}

// eyes on the enemy, but still three keys down: a juke is a trichord too
static void key_idle_axes(const object *bot, vms_vector *move)
{
	fix r = vm_vec_dot(move, (vms_vector *)&bot->orient.rvec);
	fix u = vm_vec_dot(move, (vms_vector *)&bot->orient.uvec);
	fix f = vm_vec_dot(move, (vms_vector *)&bot->orient.fvec);
	fix peak = max(abs(r), max(abs(u), abs(f)));

	if (peak < F1_0/64)
		return;
	if (abs(r) * DUEL_KEY_SHARE < peak)
		vm_vec_scale_add2(move, (vms_vector *)&bot->orient.rvec, fixmul(peak, momentum_sign(bot, &bot->orient.rvec)));
	if (abs(u) * DUEL_KEY_SHARE < peak)
		vm_vec_scale_add2(move, (vms_vector *)&bot->orient.uvec, fixmul(peak, momentum_sign(bot, &bot->orient.uvec)));
}

static void fly(duel_bot *b, object *bot, const vms_vector *move, vms_vector *look, const vms_vector *aim)
{
	vms_vector thrust_dir;

	if (faces_enemy(b->mode) && b->visible)
	{
		face(b, bot, aim);		// eyes stay on the enemy, kiting or not
		thrust_dir = *move;
		key_idle_axes(bot, &thrust_dir);
	}
	else
	{
		int travel = travelling(b, bot, move);
		int arm = b->flight.arm;

		vms_vector heading = *move;
		vm_vec_normalize(&heading);
		if (travel && !b->visible && b->mode != MODE_PREFIRE && !tight_ahead(b, bot, look))
			trichord(b, bot, &heading);
		else
		{
			b->travel_dir = heading;
			face(b, bot, look);
		}
		if (travel && arm == 2 && !over_corner_speed(b, bot, look))
			thrust_dir = *move;		// all three keys down until a turn needs braking
		else
			steer(b, bot, move, look, &thrust_dir);
		track_flight(b, bot, travel);
	}

	apply_thrust(bot, &thrust_dir);
	stabilize(bot);
}

//	-------------------------------------------------------------------------
//	Opponent profile: their range, their jink, their guns -- per match
//	-------------------------------------------------------------------------

static void sideways(const object *bot, const object *enemy, fix *r, fix *u)
{
	*r = vm_vec_dot((vms_vector *)&enemy->mtype.phys_info.velocity, (vms_vector *)&bot->orient.rvec);
	*u = vm_vec_dot((vms_vector *)&enemy->mtype.phys_info.velocity, (vms_vector *)&bot->orient.uvec);
}

static void start_probe(duel_bot *b, const object *bot)
{
	object *enemy = b->target >= 0 ? live_player_object(b->target) : NULL;

	if (!enemy || !b->visible || b->probe_at)
		return;
	b->probe_target = b->target;
	b->probe_at = GameTime64 + DUEL_PROBE_DELAY;
	sideways(bot, enemy, &b->probe_r, &b->probe_u);
}

static void smooth(fix *avg, fix sample, int samples)
{
	*avg += (sample - *avg) / min(samples, DUEL_FOE_SMOOTH);
}

// hurt pilots either run for shields or turn and fight: learn which
static void study_hurt(const duel_bot *b, duel_foe *f, const object *bot, const object *enemy)
{
	vms_vector to_bot;
	fix closing;

	if (b->est_shields[enemy->id] >= DUEL_HURT_SHIELDS)
		return;
	vm_vec_sub(&to_bot, (vms_vector *)&bot->pos, (vms_vector *)&enemy->pos);
	vm_vec_normalize(&to_bot);
	closing = vm_vec_dot(&b->enemy_vel, &to_bot);
	if (abs(closing) < DUEL_HURT_MOVING)
		return;
	f->hurt_samples++;
	f->hurt_push += ((closing > 0 ? F1_0 : -F1_0) - f->hurt_push) / min(f->hurt_samples, DUEL_HURT_SMOOTH);
	if (f->hurt_told || f->hurt_samples < DUEL_HURT_CONFIDENT || abs(f->hurt_push) < DUEL_HURT_LEAN || !journaling(b))
		return;
	f->hurt_told = 1;
	if (f->hurt_push < 0)
		journal("PREDICT", "when hurt", "you run for shields: I'll press in sooner to finish");
	else
		journal("PREDICT", "when hurt", "you turn and fight: I'll hold range and let you come onto my guns");
}

static void study_foe(duel_bot *b, const object *bot, const object *enemy, fix dist)
{
	duel_foe *f = &Foes[enemy->id];
	fix r, u;

	f->samples++;
	smooth(&f->fight_dist, dist, f->samples);
	study_hurt(b, f, bot, enemy);

	if (!b->probe_at || GameTime64 < b->probe_at)
		return;
	b->probe_at = 0;
	if (b->probe_target != enemy->id)
		return;
	sideways(bot, enemy, &r, &u);
	smooth(&f->dodge_r, r - b->probe_r, f->samples);
	smooth(&f->dodge_u, u - b->probe_u, f->samples);
}

// lead toward the side they always jink to
static void read_dodge(const duel_bot *b, const object *bot, const object *enemy, int weapon_id, vms_vector *aim)
{
	const duel_foe *f = &Foes[enemy->id];
	fix speed = Weapon_info[weapon_id].speed[Difficulty_level];
	fix flight, lead;

	if (f->samples < DUEL_FOE_SMOOTH || speed <= 0)
		return;
	flight = fixdiv(vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&enemy->pos), speed);
	lead = fixmul(fixmul(flight, DUEL_DODGE_READ), skill(b)->lead_share);
	vm_vec_scale_add2(aim, (vms_vector *)&bot->orient.rvec, fixmul(f->dodge_r, lead));
	vm_vec_scale_add2(aim, (vms_vector *)&bot->orient.uvec, fixmul(f->dodge_u, lead));
}

// fight where they are least comfortable
static fix deny_range(int foe, fix range)
{
	const duel_foe *f = foe >= 0 ? &Foes[foe] : NULL;
	fix denied;

	if (!f || f->samples < DUEL_FOE_SMOOTH)
		return range;
	denied = range + fixmul(range - f->fight_dist, DUEL_RANGE_DENIAL);
	return max(range * 6 / 10, min(denied, range * 3 / 2));
}

static void mode_reason(const duel_bot *b, const object *bot, const object *enemy, int weapon, char *why, int len)
{
	int dist = enemy ? f2i(vm_vec_dist((vms_vector *)&bot->pos, (vms_vector *)&enemy->pos)) : 0;

	switch (b->mode)
	{
	case MODE_BLAST:
		snprintf(why, len, "a blast is about to go off near me: getting clear");
		break;
	case MODE_RETREAT:
		if (weapon < 0)
			snprintf(why, len, "nothing worth shooting with: running for supplies");
		else if (undergunned(b))
			snprintf(why, len, "you outgun me: shooting while I run for a better gun");
		else
			snprintf(why, len, "too hurt to trade (shields %d, aggression %.2f)", f2i(bot_player(b)->shields), f2fl(aggression(b)));
		break;
	case MODE_BULL:
		snprintf(why, len, "your shields look low (~%d): pressing in to finish", f2i(target_shields(b)));
		break;
	case MODE_BREAK:
		snprintf(why, len, "traded long enough: breaking line of sight to reset");
		break;
	case MODE_FIGHT:
		snprintf(why, len, "you're in view at %d units: strafing with %s", dist, weapon >= 0 ? Gun_names[weapon] : "nothing");
		break;
	case MODE_PREFIRE:
		snprintf(why, len, "you're probably around that corner: shooting first");
		break;
	case MODE_CAMP:
		if (b->seen_at)
			snprintf(why, len, "lost you for %ds: waiting where you'll pass", (int)((GameTime64 - b->seen_at) / F1_0));
		else
			snprintf(why, len, "no idea where you are: waiting where you'll pass");
		break;
	default:
		snprintf(why, len, "no contact: searching");
	}
}

static void journal_mode(duel_bot *b, const object *bot, const object *enemy, int weapon)
{
	char why[120];

	if (b->mode != Journal_pending_mode)
	{
		Journal_pending_mode = b->mode;
		Journal_pending_since = GameTime64;
		return;
	}
	if (b->mode == Journal_mode || GameTime64 - Journal_pending_since < JOURNAL_MODE_SETTLE)
		return;
	Journal_mode = b->mode;
	mode_reason(b, bot, enemy, weapon, why, sizeof(why));
	journal("STANCE", Mode_names[b->mode], "%s", why);
}

// what a pickup is worth to someone arriving with nothing: what we deny them
static int denial_value(int id)
{
	switch (id)
	{
		case POW_SMARTBOMB_WEAPON:	return 16;
		case POW_PLASMA_WEAPON:		return 12;
		case POW_FUSION_WEAPON:
		case POW_MEGA_WEAPON:		return 10;
		case POW_HOMING_AMMO_1:
		case POW_HOMING_AMMO_4:
		case POW_INVULNERABILITY:	return 9;
		default:					return 0;
	}
}

static int mined_already(const object *bot, const object *pow)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_WEAPON && Objects[i].id == PROXIMITY_ID &&
			Objects[i].ctype.laser_info.parent_num == bot - Objects &&
			vm_vec_dist(&Objects[i].pos, (vms_vector *)&pow->pos) < DUEL_MINE_REACH)
			return 1;
	return 0;
}

// honey pot: a stash it doesn't need gets a proxy on it, so taking it costs them
static void mine_stash(duel_bot *b, object *bot)
{
	int i;

	if (b->visible || Players[b->pnum].secondary_ammo[PROXIMITY_INDEX] < DUEL_MINE_KEEP ||
		GameTime64 < b->next_secondary || GameTime64 < b->next_mine || match_tense())
		return;

	for (i = 0; i <= Highest_object_index; i++)
	{
		const object *pow = &Objects[i];

		if (pow->type != OBJ_POWERUP || denial_value(pow->id) < DUEL_MINE_WORTH || powerup_value(b, pow->id))
			continue;	// stuff it still wants would be mined against itself
		if (vm_vec_dist((vms_vector *)&pow->pos, &bot->pos) > DUEL_MINE_REACH || mined_already(bot, pow))
			continue;
		fire_secondary(b, bot, PROXIMITY_INDEX);
		b->next_mine = GameTime64 + DUEL_MINE_GAP;
		if (journaling(b))
			journal("MINE", "honey pot", "proxy on a stash you'll want: taking it will cost you");
		return;
	}
}

static void think(duel_bot *b, object *bot)
{
	object *enemy = choose_target(b, bot);
	vms_vector move, aim, look, point;
	object *mine;
	fix dist = 0;
	int weapon = -1, fighting, stuck;

	look_around(b->pnum, bot->segnum);
	update_openness(b, bot);
	update_sight(b, bot, enemy);
	scan_objects(b, bot, enemy);

	if (b->visible && enemy)
	{
		dist = vm_vec_dist(&bot->pos, &enemy->pos);
		weapon = commit_weapon(b, bot, enemy, dist);
		lead_point(b, bot, enemy, fired_weapon_id(b, weapon >= 0 ? weapon : LASER_INDEX),
				   weapon_lead_bias(weapon), &aim);
		read_dodge(b, bot, enemy, fired_weapon_id(b, weapon >= 0 ? weapon : LASER_INDEX), &aim);
		add_aim_slop(b, enemy, &aim);
		study_foe(b, bot, enemy, dist);
	}

	b->mode = choose_mode(b, bot, enemy, weapon, &point);
	if (journaling(b))
		journal_mode(b, bot, enemy, weapon);
	if (b->mode == MODE_HUNT || b->mode == MODE_CAMP)
	{
		preselect_weapon(b);
		mine_stash(b, bot);
	}
	fighting = mode_fights(b->mode) && b->visible;
	if (b->mode == MODE_BREAK && GameTime64 >= b->next_path)
	{
		b->next_path = GameTime64 + DUEL_PATH_REFRESH;
		find_cover(b, bot, enemy);
	}

	look = bot->pos;
	plan_motion(b, bot, enemy, weapon, &point, &move, &look);

	vm_vec_normalize(&move);
	add_avoidance(b, bot, &move);
	stuck = check_stuck(b, bot, &move);
	vm_vec_normalize(&move);

	mine = b->visible ? NULL : mine_to_shoot(bot, &move);
	b->aiming_at_mine = mine != NULL;
	if (mine)
		look = mine->pos;
	fly(b, bot, &move, &look, &aim);
	if (mine)
		shoot_mine(b, bot, mine);

	if (fighting)
		shoot(b, bot, enemy, weapon, &aim, dist);
	else if (b->mode == MODE_PREFIRE)
		prefire(b, bot, &point);
	else if (b->mode == MODE_HUNT)
		flare_doors(b, bot);

	finish_lead_epoch(b);
	finish_range_epoch(b, fighting);
	tally(b, bot, fighting, b->mode == MODE_RETREAT || b->mode == MODE_BREAK, weapon, stuck);
	log_state(b, fighting, b->mode == MODE_RETREAT, weapon, dist);
}

//	-------------------------------------------------------------------------
//	Frame
//	-------------------------------------------------------------------------

static void run_bot(duel_bot *b)
{
	object *bot;

	if (!b->active)
		return;

	if (b->dying_until)
	{
		object *wreck = live_player_object(b->pnum);

		if (wreck && GameTime64 >= b->dying_until)
			finish_dying(b, wreck);
		else if (wreck)
			tumble(b, wreck);
		else
			b->dying_until = 0, b->respawn_at = GameTime64 + DUEL_RESPAWN_TIME;
		return;
	}

	if (b->respawn_at)
	{
		if (GameTime64 < b->respawn_at)
			return;
		b->respawn_at = 0;
		spawn_bot(b);
		return;
	}

	bot = bot_object(b);
	if (!bot)
	{
		b->respawn_at = GameTime64 + DUEL_RESPAWN_TIME;
		return;
	}

	expire_powers(b);
	refuel(b, bot);
	think(b, bot);
}

static void bench_frame(void);

int duelbot_piloting(const object *obj)
{
	return Pilot_on && Pilot.active && obj == ConsoleObject;
}

static void init_pilot(void)
{
	int p;

	memset(&Pilot, 0, sizeof(Pilot));
	Pilot.pnum = Player_num;
	Pilot.active = 1;
	Pilot.target = -1;
	for (p = 0; p < MAX_PLAYERS; p++)
		Pilot.est_shields[p] = StartingShields;
	start_epoch(&Pilot.lead, Brain.lead, DUEL_LEAD_ARMS, DUEL_LEAD_EPOCH);
	start_epoch(&Pilot.range, Brain.range, DUEL_RANGE_ARMS, DUEL_RANGE_EPOCH);
	start_epoch(&Pilot.flight, Brain.flight, DUEL_FLIGHT_ARMS, DUEL_FLIGHT_EPOCH);
	reset_navigation(&Pilot);
}

// the local ship: dying and respawning are the human code's job; flying is ours
static void run_pilot(void)
{
	if (Player_is_dead)
	{
		if (Player_exploded)
			Death_sequence_aborted = 1;		// "press a key to continue"
		return;
	}

	if (ConsoleObject->type == OBJ_PLAYER)
		think(&Pilot, ConsoleObject);
}

//	-------------------------------------------------------------------------
//	Dedicated 1v1 server: RDL solo rules, first to 20 by 2
//	-------------------------------------------------------------------------

#define MATCH_POINTS		20
#define MATCH_MARGIN		2
#define DEDICATED_IDLE_RESTART	(F1_0*60*2)	// back to the forming lobby
#define MATCH_ABANDON_DELAY	(F1_0*3)
#define MATCH_RESTART_DELAY	(F1_0*10)	// time to read the result before the restart

static int Match_points = MATCH_POINTS;
static int Match_margin = MATCH_MARGIN;	// free-for-all: first to the goal wins
static fix64 Match_restart_at;
static fix64 Idle_since;
static int Dedicated_humans;
#define DEDICATED_FPS		60
#define DEDICATED_IDLE_FPS	10	// nobody watching: just keep the lobby answering, and the box cool

static void announce(const char *text)
{
	snprintf(Network_message, sizeof(Network_message), "%s", text);
	Network_message_reciever = 100;
	multi_send_message();
	con_printf(CON_NORMAL, "match: %s\n", text);
}

// the two best scores among the pilots (a watching host's slot never counts)
static void top_two(int *leader, int *best, int *second)
{
	int i;

	*leader = -1;
	*best = *second = -1000;
	for (i = 0; i < N_players; i++)
	{
		int points = Players[i].KillGoalCount;

		if ((i == Player_num && Netgame.host_is_obs) || !Players[i].connected)
			continue;
		if (points > *best)
		{
			*second = *best;
			*best = points;
			*leader = i;
		}
		else if (points > *second)
			*second = points;
	}
}

static int journal_foe_score(void)
{
	int i, best = 0;

	for (i = 0; i < N_players; i++)
		if (i != Player_num)
			best = max(best, Players[i].KillGoalCount);
	return best;
}

static void journal_summary(const char *result)
{
	const duel_stats *st = &Pilot.stats;
	char line[160];
	int m, len = 0;

	if (!Journal)
		return;
	journal("RESULT", "", "%s", result);
	for (m = 0; m < (int)(sizeof(Mode_names) / sizeof(Mode_names[0])); m++)
		len += snprintf(line + len, sizeof(line) - len, "%s %d%%  ", Mode_names[m], percent(st->mode_time[m], st->alive));
	journal("SUMMARY", "time per stance", "%s", line);
	journal("SUMMARY", "shooting", "%d hits from %d shots, %d missiles, %d wall bumps",
			st->hits, st->shots, st->missiles, st->wall_hits);
	journal("SUMMARY", "your missiles", "dodged %d, took %d", st->missiles_dodged, st->missiles_taken);
}

// either side one kill from the end
static int match_tense(void)
{
	int leader, best, second;

	if (!Dedicated)
		return 0;
	top_two(&leader, &best, &second);
	return leader >= 0 && best >= Match_points - 1 && best - second >= Match_margin - 1;
}

// 20 isn't a win until it's 2 clear; say so, once per score
static void explain_win_by_two(int leader, int best, int second)
{
	static int told_best = -1, told_second = -1;
	char text[MAX_MESSAGE_LEN];

	if (Match_margin < 2 || best < Match_points || best - second >= Match_margin)
		return;
	if (best == told_best && second == told_second)
		return;
	told_best = best;
	told_second = second;
	snprintf(text, sizeof(text), "%d-%d: %s needs to win by 2", best, max(second, 0), Players[leader].callsign);
	announce(text);
	journal("SCORE", "win by 2", "%s", text);
}

static void check_match_point(void)
{
	char text[MAX_MESSAGE_LEN];
	int leader, best, second;

	top_two(&leader, &best, &second);
	if (leader >= 0)
		explain_win_by_two(leader, best, second);
	if (leader < 0 || best < Match_points || best - second < Match_margin)
		return;

	snprintf(text, sizeof(text), "GAME OVER: %s wins %d-%d",
			 Players[leader].callsign, best, max(second, 0));
	announce(text);
	journal_summary(text);
	journal_close();
	snprintf(text, sizeof(text), "New match in %d seconds", f2i(MATCH_RESTART_DELAY));
	announce(text);
	Match_restart_at = GameTime64 + MATCH_RESTART_DELAY;
}

// the service manager starts a fresh lobby when this one exits
// for the Discord bot: is anyone playing right now?
#define STATUS_FILE		"dedicated-status.txt"
#define STATUS_INTERVAL		(F1_0*5)

static void write_status(void)
{
	static fix64 next_at;
	PHYSFS_file *f;
	int i, humans = 0;

	if (GameTime64 < next_at)
		return;
	next_at = GameTime64 + STATUS_INTERVAL;

	for (i = 0; i < N_players; i++)
		if (i != Player_num && Players[i].connected && !multi_cpu_slot(i))
		{
			humans++;
			if (Pilot_on)
				journal_open(Players[i].callsign);
		}
	if (Journal && !humans)
	{
		char text[64];

		snprintf(text, sizeof(text), "opponent left at %d-%d", Players[Player_num].KillGoalCount, journal_foe_score());
		journal_summary(text);
		journal_close();
		if (!Match_restart_at)		// abandoned: the next pilot gets a fresh match, the tracker archives this one
			Match_restart_at = GameTime64 + MATCH_ABANDON_DELAY;
	}
	Dedicated_humans = humans;
	f = PHYSFSX_openWriteBuffered(STATUS_FILE);
	if (!f)
		return;
	PHYSFSX_printf(f, "humans=%d\nmap=%s\nlevel=%d\ntier=%s\n", humans, Current_mission_longname,
				   Current_level_num, Tier_names[min(Netgame.CpuSkill, DUEL_TIERS - 1)]);
	PHYSFS_close(f);
}

static void dedicated_frame(void)
{
	if (!(Game_mode & GM_MULTI))
		exit(0);

	write_status();
	PlayerCfg.maxFps = Dedicated_humans || Netgame.numobservers ? DEDICATED_FPS : DEDICATED_IDLE_FPS;
	if (Dedicated_humans)
		Idle_since = GameTime64;
	else if (GameTime64 - Idle_since > DEDICATED_IDLE_RESTART)
	{
		con_printf(CON_URGENT, "DEDICATED: idle, starting a fresh lobby\n");
		multi_leave_game();
		exit(0);		// fresh level time, fresh tracker session
	}

	if (!Match_restart_at)
	{
		check_match_point();
		return;
	}
	if (GameTime64 < Match_restart_at)
		return;

	journal_close();
	multi_leave_game();
	exit(0);
}

int duelbot_dedicated(void)
{
	return Dedicated;
}

//	The server's map: Logic unless something else was chosen with
//	-dedicatedmap (the RDL bot's !cpu command); it sticks across restarts.

#define NEXT_MAP_FILE		"dedicated-next.txt"
#define DEFAULT_MAP		"logic"
#define ROTATION_FILE		"maprotation.txt"	// one DXMA title per line, optional "|level"
#define ROTATION_POS_FILE	"maprotation.pos"
#define ROTATION_MAX_LINE	128

static int clamp_skill(int skill)
{
	return max(1, min(skill, DUEL_TIERS - 1));
}

static void save_next_map(const char *mission, int level, int skill)
{
	PHYSFS_file *f = PHYSFSX_openWriteBuffered(NEXT_MAP_FILE);

	if (!f)
		return;
	PHYSFSX_printf(f, "%s\n%d\n%d\n", mission, level, skill);
	PHYSFS_close(f);
}

static void load_next_map(char *mission, int len, int *level, int *skill)
{
	PHYSFS_file *f = PHYSFSX_openReadBuffered(NEXT_MAP_FILE);
	char line[64];

	if (!f)
		return;
	if (PHYSFSX_fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		snprintf(mission, len, "%s", line);
	}
	if (PHYSFSX_fgets(line, sizeof(line), f))
		*level = max(1, atoi(line));
	if (PHYSFSX_fgets(line, sizeof(line), f))
		*skill = clamp_skill(atoi(line));
	PHYSFS_close(f);
}

static int contains_nocase(const char *hay, const char *needle)
{
	size_t n = strlen(needle);

	for (; *hay; hay++)
		if (!d_strnicmp(hay, needle, n))
			return 1;
	return 0;
}

// every word of the query in the title; anarchy-labelled missions first
static int dxma_search(const char *query)
{
	int i, best = -1, best_score = 0;

	for (i = 0; i < dxma_count(); i++)
	{
		const dxma_mission *m = dxma_get(i);
		char words[64], *word, *save;
		int score = 1;

		snprintf(words, sizeof(words), "%s", query);
		for (word = strtok_r(words, " ", &save); word && score; word = strtok_r(NULL, " ", &save))
			if (!contains_nocase(m->title, word))
				score = 0;
		if (!score)
			continue;
		if (contains_nocase(m->mode, "anarchy"))
			score += 2;
		if (!d_stricmp(m->title, query))
			score += 4;
		if (score > best_score)
			best_score = score, best = i;
	}
	return best;
}

// a DXMA mission on disk, downloading it first if needed; its filename in mission
static int fetch_mission(int index, char *mission, int len)
{
	char stem[64];

	if (!dxma_download_stem(index, stem, sizeof(stem)))
		return 0;
	if (find_mission_by_stem(stem, mission, len))
		return 1;
	return dxma_download_mission_ex(index, 0) && find_mission_by_stem(stem, mission, len);
}

static const char *arg_string(const char *name)
{
	int i;

	for (i = 1; i < Num_args - 1; i++)
		if (!d_stricmp(Args[i], name))
			return Args[i + 1];
	return NULL;
}

// look a map up on DXMA and download it; 0 and a reason if that fails
static int find_map(const char *query, char *mission, int len, const char **why)
{
	int index;

	dxma_load();
	index = dxma_search(query);
	if (index < 0)
		return *why = "no DXMA mission matches", 0;
	if (!fetch_mission(index, mission, len))
		return *why = "download failed", 0;
	*why = dxma_get(index)->title;
	return 1;
}

// -dedicatedset [-dedicatedmap <name>] [-dedicatedtier <tier>]: change what
// the server hosts next, report, and exit; whatever isn't given stays
static void apply_settings_and_exit(void)
{
	char mission[16] = DEFAULT_MAP, title[64] = DEFAULT_MAP;
	const char *query = arg_string("-dedicatedmap"), *tier = arg_string("-dedicatedtier");
	const char *result;
	int level = 1, skill = NDL - 1;

	load_next_map(mission, sizeof(mission), &level, &skill);
	if (tier && tier_skill(tier) < 0)
	{
		printf("SET: unknown tier %s\n", tier);
		exit(1);
	}
	if (tier)
		skill = tier_skill(tier);
	if (query && !find_map(query, mission, sizeof(mission), &result))
	{
		printf("SET: %s\n", result);
		exit(1);
	}
	if (query)
		snprintf(title, sizeof(title), "%s", result), level = 1;
	else
		snprintf(title, sizeof(title), "%s", mission);

	save_next_map(mission, level, skill);
	printf("SET: %s|%s|%s\n", title, mission, Tier_names[skill]);
	exit(0);
}

static int read_int_file(const char *name)
{
	PHYSFS_file *f = PHYSFSX_openReadBuffered(name);
	char line[16];
	int value = 0;

	if (!f)
		return 0;
	if (PHYSFSX_fgets(line, sizeof(line), f))
		value = atoi(line);
	PHYSFS_close(f);
	return max(value, 0);
}

static void write_int_file(const char *name, int value)
{
	PHYSFS_file *f = PHYSFSX_openWriteBuffered(name);

	if (!f)
		return;
	PHYSFSX_printf(f, "%d\n", value);
	PHYSFS_close(f);
}

// the index-th map line, or the count of them when index is -1
static int rotation_line(int index, char *entry, int len)
{
	PHYSFS_file *f = PHYSFSX_openReadBuffered(ROTATION_FILE);
	char line[ROTATION_MAX_LINE];
	int count = 0;

	if (!f)
		return 0;
	while (PHYSFSX_fgets(line, sizeof(line), f))
	{
		line[strcspn(line, "\r\n")] = 0;
		if (!line[0] || line[0] == '#')
			continue;
		if (count++ == index)
		{
			snprintf(entry, len, "%s", line);
			break;
		}
	}
	PHYSFS_close(f);
	return count;
}

// the next map in the rotation file that downloads and loads
static int load_rotation_map(int *level)
{
	char entry[ROTATION_MAX_LINE], mission[16];
	int count = rotation_line(-1, NULL, 0);
	int pos = read_int_file(ROTATION_POS_FILE);
	int tries;
	const char *why;

	for (tries = 0; tries < count; tries++, pos++)
	{
		char *bar;

		rotation_line(pos % count, entry, sizeof(entry));
		bar = strchr(entry, '|');
		*level = bar ? max(1, atoi(bar + 1)) : 1;
		if (bar)
			*bar = 0;
		if (!find_map(entry, mission, sizeof(mission), &why) || !load_mission_by_name(mission))
		{
			con_printf(CON_URGENT, "DEDICATED: rotation skips %s (%s)\n", entry, why);
			continue;
		}
		con_printf(CON_URGENT, "DEDICATED: rotation map %d/%d: %s\n", pos % count + 1, count, why);
		write_int_file(ROTATION_POS_FILE, pos % count + 1);	// advance now: a map that dies loading can't wedge the server
		return 1;
	}
	return 0;
}

// the rotation, else the saved map, else Logic, else the built-in mission
static int load_server_map(int *level, int *skill)
{
	char mission[16] = DEFAULT_MAP;
	const char *why = "";

	load_next_map(mission, sizeof(mission), level, skill);
	if (load_rotation_map(level))
		return 1;
	if (load_mission_by_name(mission))
		return 1;
	*level = 1;
	if (find_map(DEFAULT_MAP, mission, sizeof(mission), &why) && load_mission_by_name(mission))
		return 1;
	con_printf(CON_URGENT, "DEDICATED: no %s (%s), using the built-in mission\n", DEFAULT_MAP, why);
	return load_mission_by_name(D1_MISSION_FILENAME);
}

void duelbot_frame(void)
{
	int i;

	if (Bench_seconds)
		bench_frame();
	if (Dedicated)
		dedicated_frame();
	if (duelbot_idle_server())
		return;		// nobody here: CPUs hold still instead of farming suicides
	if (duelbot_piloting(ConsoleObject))
		run_pilot();

	if (!cpu_thinking())
		return;

	decay_heat();

	for (i = 0; i < Bot_count; i++)
		run_bot(&Bots[i]);
}

//	-------------------------------------------------------------------------
//	Bench: CPU vs CPU, headless, fixed timestep
//	-------------------------------------------------------------------------

static int arg_value(const char *name, int fallback)
{
	int i;

	for (i = 1; i < Num_args - 1; i++)
		if (!d_stricmp(Args[i], name))
			return atoi(Args[i + 1]);

	return fallback;
}

const char *duelbot_tier_name(int skill)
{
	static const char *const names[DUEL_TIERS] = { "Trainee", "Rookie", "Hotshot", "Ace", "Insane", "Impossible" };

	return names[max(0, min(skill, DUEL_TIERS - 1))];
}

int duelbot_idle_server(void)
{
	return Dedicated && PlayerCfg.maxFps == DEDICATED_IDLE_FPS;
}

int duelbot_no_lan(void)
{
	return arg_value("-nolan", 0);
}

int duelbot_benching(void)
{
	return Bench_seconds > 0;
}

int duelbot_bench_frame_time(void)
{
	if (!Bench_seconds || Bench_net)
		return 0;

	FrameTime = DUEL_BENCH_FRAME;
	return 1;
}

static int percent(fix64 part, fix64 whole)
{
	return whole > 0 ? (int)(part * 100 / whole) : 0;
}

static void print_arms(const char *name, const duel_arm *arms, int count)
{
	int i;

	printf("  %s:", name);
	for (i = 0; i < count; i++)
		printf(" [%d] %.2f/%d", i, f2fl(arms[i].score), arms[i].trials);
	printf("\n");
}

static void print_ship(const char *name, const duel_bot *b)
{
	const duel_stats *s = &b->stats;
	int minutes10 = max((int)(s->alive * 10 / (F1_0 * 60)), 1);
	int m;

	printf("  %s%d K/D %d/%d  acc %d%% (%d hits / %d shots+%d missiles)  dmg/min %d  taken/min %d\n",
		   name, b->pnum, s->kills, s->deaths,
		   (s->shots + s->missiles) ? s->hits * 100 / (s->shots + s->missiles) : 0,
		   s->hits, s->shots, s->missiles,
		   f2i(s->dealt) * 10 / minutes10, f2i(s->taken) * 10 / minutes10);
	printf("        plans: restock %d  chase %d  shop %d  patrol %d  aggression %.2f\n",
		   s->plans[0], s->plans[1], s->plans[2], s->plans[3], f2fl(aggression(b)));
	printf("        modes:");
	for (m = 0; m < 8; m++)
		printf(" %s %d%%", Mode_names[m], percent(s->mode_time[m], s->alive));
	printf("\n        guns: laser %d vulcan %d spread %d plasma %d fusion %d   switches/min %d   blast-taken %d\n",
		   s->weapon_shots[0], s->weapon_shots[1], s->weapon_shots[2], s->weapon_shots[3], s->weapon_shots[4],
		   s->switches * 10 / minutes10, f2i(s->blast_taken));
	printf("        frozen %d%%  wall hits/min %d  missiles dodged %d taken %d\n", percent(s->frozen, s->alive),
		   s->alive ? (int)((fix64)s->wall_hits * F1_0 * 60 / s->alive) : 0, s->missiles_dodged, s->missiles_taken);
	printf("        idle: reacting %d%%  off-aim %d%%  cooldown %d%%\n",
		   percent(s->idle_reacting, s->idle_gun), percent(s->idle_off_aim, s->idle_gun),
		   percent(s->idle_cooldown, s->idle_gun));
	printf("        visible %d%%  idle-gun %d%%  no-gun %d%%  upside %d%%  stuck %d%% (%d)  fight/retreat/hunt %d/%d/%d\n",
		   percent(s->visible, s->alive), percent(s->idle_gun, s->visible), percent(s->no_gun, s->visible),
		   percent(s->upside, s->alive), percent(s->stuck, s->alive), s->stuck_events,
		   percent(s->fight, s->alive), percent(s->retreat, s->alive), percent(s->hunt, s->alive));
}

// the netgame's own scoreboard, as this machine sees it: compare across machines
static void print_net_tally(void)
{
	int i;

	printf("  netgame tally (I am player %d, %s):\n", Player_num, multi_i_am_master() ? "host" : "client");
	for (i = 0; i < MAX_PLAYERS; i++)
		if (Players[i].connected)
			printf("    %d %-8s %s kills %d deaths %d\n", i, Players[i].callsign,
				   multi_cpu_slot(i) ? "CPU  " : "human", Players[i].net_kills_total, Players[i].net_killed_total);
}

static void print_foes(void)
{
	int p, w, fav;

	for (p = 0; p < MAX_PLAYERS; p++)
	{
		const duel_foe *f = &Foes[p];

		if (!f->samples)
			continue;
		for (fav = 0, w = 1; w < MAX_PRIMARY_WEAPONS; w++)
			if (f->gun_use[w] > f->gun_use[fav])
				fav = w;
		printf("  foe %d: fights at %d, jinks r%+d u%+d, favourite gun %d (%d sightings)\n", p,
			   f2i(f->fight_dist), f2i(f->dodge_r), f2i(f->dodge_u), fav, f->samples);
	}
}

static void bench_report(const char *label)
{
	int i;

	printf("DUELBENCH %s t=%ds level=%d\n", label, (int)((GameTime64 - Bench_started_at) / F1_0), Bench_level);

	for (i = 0; i < Bot_count; i++)
		if (Bots[i].active)
			print_ship("cpu", &Bots[i]);
	if (Pilot_on)
		print_ship("pilot", &Pilot);
	if (net_game())
		print_net_tally();

	print_arms("lead", Brain.lead, DUEL_LEAD_ARMS);
	print_arms("range", Brain.range, DUEL_RANGE_ARMS);
	print_arms("flight", Brain.flight, DUEL_FLIGHT_ARMS);
	print_foes();
	fflush(stdout);
}

// the console ship is a camera: untouchable, untrackable, out of the fight
static void hold_spectator(void)
{
	player *p = &Players[Player_num];

	p->flags |= PLAYER_FLAGS_INVULNERABLE | PLAYER_FLAGS_CLOAKED;
	p->invulnerable_time = GameTime64;
	p->cloak_time = GameTime64;
	p->shields = MAX_SHIELDS;
}

#define BRAIN_MAGIC	0x44425231	// "DBR1"; bump when duel_brain's habits change shape

static size_t brain_habits_size(void)
{
	return offsetof(duel_brain, heat);	// heat is per-map, never saved
}

static void brain_load(void)
{
	const char *path = arg_string("-duelbrain");
	static duel_brain saved;	// heat makes it too big for the stack
	unsigned int magic = 0;
	FILE *f;

	if (!path)
		return;
	f = fopen(path, "rb");
	if (!f)
		return;
	if (fread(&magic, sizeof(magic), 1, f) == 1 && magic == BRAIN_MAGIC &&
		fread(&saved, brain_habits_size(), 1, f) == 1)
		memcpy(&Brain, &saved, brain_habits_size());
	fclose(f);
}

static void brain_save(void)
{
	const char *path = arg_string("-duelbrain");
	unsigned int magic = BRAIN_MAGIC;
	FILE *f;

	if (!path)
		return;
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(&magic, sizeof(magic), 1, f);
	fwrite(&Brain, brain_habits_size(), 1, f);
	fclose(f);
}

static void bench_frame(void)
{
	if (!Bench_net)
		hold_spectator();

	if (GameTime64 >= Bench_next_report)
	{
		bench_report("progress");
		Bench_next_report = GameTime64 + DUEL_BENCH_REPORT;
	}

	if (GameTime64 - Bench_started_at < (fix64)Bench_seconds * F1_0)
		return;

	bench_report("final");
	brain_save();
	exit(0);
}

static void dedicated_start(void)
{
	char name[NETGAME_NAME_LEN + 1];
	int cpus, ffa, level, skill = NDL - 1;

	Dedicated = 1;
	Match_points = arg_value("-dedicatedpoints", MATCH_POINTS);
	PlayerCfg.maxFps = DEDICATED_FPS;
	PlayerCfg.AutoLeveling = 0;	// fights the trichord roll
	level = arg_value("-duellevel", 1);
	if (!load_server_map(&level, &skill))
	{
		printf("DEDICATED: could not load any mission\n");
		exit(1);
	}
	level = min(level, Last_level);
	con_printf(CON_URGENT, "DEDICATED: hosting %s level %d\n", Current_mission_filename, level);

	skill = clamp_skill(arg_value("-dedicatedskill", skill));
	cpus = arg_value("-dedicatedcpus", 1);
	ffa = cpus > 1;
	if (ffa)
		Match_margin = 1;
	Pilot_on = cpus;		// the host's own ship is the CPU
	Server_bot_name = arg_string("-dedicatedname");
	if (cpus)
		snprintf(Players[Player_num].callsign, sizeof(Players[Player_num].callsign), "%s",
				 Server_bot_name ? Server_bot_name : Tier_names[skill]);
	if (ffa && Server_bot_name)
		snprintf(name, sizeof(name), "%s - FFA CPU", Server_bot_name);
	else if (cpus && Server_bot_name)
		snprintf(name, sizeof(name), "%s - 1v1 CPU", Server_bot_name);
	else if (cpus)
		snprintf(name, sizeof(name), "1v1 vs %s", Tier_names[skill]);
	else
		snprintf(name, sizeof(name), "1v1 - RDL rules");
	if (!net_udp_dedicated_host(level, arg_value("-duelport", UDP_PORT_DEFAULT), name, arg_value("-dedicatedlisted", 1), cpus > 0, skill, max(cpus - 1, 0)))
	{
		printf("DEDICATED: could not host\n");
		exit(1);
	}
}

static int load_bench_mission(void)
{
	const char *mission = arg_string("-duelmission");

	return load_mission_by_name(mission ? (char *)mission : D1_MISSION_FILENAME);
}

int duelbot_headless(void)
{
	return Bench_seconds > 0 || Dedicated;
}

void duelbot_bench_start(void)
{
	if (arg_value("-dedicatedset", 0))
	{
		Dedicated = 1;		// nobody to click its popups either
		apply_settings_and_exit();
	}
	if (arg_value("-dedicated", 0))
	{
		dedicated_start();
		return;
	}

	Bench_seconds = arg_value("-duelbench", 0);
	if (!Bench_seconds)
		return;

	Bench_level = arg_value("-duellevel", 1);
	Bench_close = arg_value("-duelclose", 0);
	Bench_kit = arg_value("-duelkit", 0);
	Pilot_on = arg_value("-duelpilot", 0);
	brain_load();

	if (!load_bench_mission())
	{
		printf("DUELBENCH: could not load the mission\n");
		exit(1);
	}

	Difficulty_level = arg_value("-dueldiff", NDL - 1);
	Bench_skill = arg_value("-duelskill", NDL - 1);

	if (arg_value("-duelnethost", -1) >= 0)
	{
		Bench_net = 1;
		if (!net_udp_auto_host(arg_value("-duelnethost", 0), Bench_level, arg_value("-duelport", 42700)))
		{
			printf("DUELBENCH: could not host\n");
			exit(1);
		}
		return;
	}

	if (arg_value("-duelnetjoin", 0))
	{
		const char *host = arg_string("-duelnetaddr");

		Bench_net = 1;
		if (!net_udp_auto_join(host ? host : "127.0.0.1", arg_value("-duelnetjoin", 0), arg_value("-duelport", 42701)))
		{
			printf("DUELBENCH: could not join\n");
			exit(1);
		}
		return;
	}

	Duel_pending = 1;
	StartNewGame(Bench_level);
}
