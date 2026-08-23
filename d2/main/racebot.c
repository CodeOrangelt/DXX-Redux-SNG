/*
 * Race mode bots. See racebot.h for what they are built out of.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "racebot.h"
#include "race.h"
#include "game.h"
#include "player.h"
#include "object.h"
#include "powerup.h"
#include "gameseg.h"
#include "segment.h"
#include "weapon.h"
#include "laser.h"
#include "fireball.h"
#include "gameseq.h"
#include "ai.h"
#include "physics.h"
#include "fvi.h"
#include "multi.h"
#include "hudmsg.h"
#include "digi.h"
#include "sounds.h"
#include "vclip.h"
#include "console.h"
#include "maths.h"
#include "polyobj.h"

extern fix Fusion_charge;

int Race_bot_count = 0;
int Race_bot_skill = RACE_SKILL_PRO;
int Race_sp_pending = 0;

extern fix StartingShields;

//	-------------------------------------------------------------------------
//	Tuning
//	-------------------------------------------------------------------------

#define BOT_ROUTE_MAX           4096	// point_segs in one lap of the route
#define BOT_PATH_DEPTH          400		// segments create_path_points may span in one leg

#define BOT_ARRIVE_DIST         i2f(12)	// route point counts as reached inside this
#define BOT_ADVANCE_MAX         4		// route points it may tick off in one frame
#define BOT_LOST_DIST           i2f(80)	// further than this off its route point = lost, resync
#define BOT_LOOKAHEAD_STEPS     10		// route points ahead the line-of-sight walk will try
#define BOT_LOOKAHEAD_DIST      i2f(60)

// How far apart the grid strings the field out along the route. Measured in
// world units, not in route points: the engine pathfinder puts its points
// wherever the geometry needs them, sometimes only a couple of units apart, so
// a grid spaced by index can stack the whole field inside one ship -- where
// they wedge each other solid and the race never starts.
#define BOT_GRID_SPACING        i2f(14)

#define BOT_LANE_SPACING        i2f(3)	// how far apart the field's lines sit across the tube
#define BOT_LANE_MAX            3		// ...and how far off the middle the outermost one gets
#define BOT_LANE_DRIFT          i2f(2)	// how far a wandering line swings either side of its own

#define BOT_RESPAWN_TIME        (F1_0*2)
#define BOT_STUCK_TIME          (F1_0*2)	// no progress for this long = wedged
#define BOT_UNSTICK_TIME        (F1_0*6)	// still wedged after this = lift it back on route
#define BOT_BOOST_TIME          (F1_0*3)	// pads, same as a player's

// Mystery boxes are taken on the way past, never chased. A box counts as on
// the way if it is close and sits within a narrow cone of the line the bot was
// already going to drive -- measured against the route, not against the ship's
// nose, so a bot that has been knocked sideways does not decide the whole
// track is worth a detour.
#define BOT_BOX_LOOK            i2f(45)	// as far as it will look for one at all
#define BOT_BOX_DOT             ((F1_0*7)/8)	// how near the racing line it has to be

// The afterburner, on the classes whose kit comes with one. A bot lights it on
// a clear road and lets the tank refill before the next one, using the same
// charge, drain and trail the player's does.
#define BOT_BURN_MIN_DOT        ((F1_0*15)/16)	// nose has to be this straight
#define BOT_BURN_MIN_REACH      i2f(40)			// ...down this much clear road
#define BOT_BURN_TIME           (F1_0*2)
#define BOT_BURN_COOLDOWN       (F1_0*5)
#define BOT_BURN_BLOB_TIME      (F1_0/15)		// same 3-a-second trail a player leaves
#define AFTERBURNER_USE_SECS    3

#define BOT_ITEM_HOLD           (F1_0)		// beat between taking a box and using what it gave
#define BOT_ITEM_TIMEOUT        (F1_0*6)	// fire it down the road rather than hoard it
#define BOT_MISSILE_GAP         (F1_0*3/2)	// floor under the rack's own rate of fire
// Shooting never steers. A bot fires at what its racing line has already put in
// front of it and at nothing else, so a fight costs it no time and it can
// never be pulled off the track by one.
#define BOT_FIRE_RANGE          i2f(300)
#define BOT_FIRE_DOT            ((F1_0*7)/10)

#define BOT_GUN_RANGE           i2f(260)
#define BOT_GUN_DOT             ((F1_0*9)/10)

// How hard the field races. Throttle is a fraction of the ship's own maximum
// thrust; turn is a rate in ai_turn_towards_vector()'s sense, where lower is
// quicker; gun_delay is the pause between shots from the class weapon.
// The trichord, and why a bot's is shallow.
//
// A pilot who slides while they thrust beats one who only holds forward: the
// axes add, so a full three-axis trichord is sqrt(3) times the straight-line
// push. Flying a full one means pointing the ship a long way off the direction
// of travel -- about 55 degrees, on every axis at once -- which on a CPU racer
// looks like a ship falling sideways down the track, not like a ship racing.
//
// So the field flies a shallow one: a slight crab left or right, nose still
// down the road, and the straight-line punch a deeper one would have earned.
// The crab is what you see; the punch is what makes them quick. Splitting the
// two is what lets the look and the pace be set independently.
//
// That crab is also what race_trichord_advance_charge() reads (see the
// throttle comments further down), scaled by the skill row's own `trichord`
// -- how much of the technique that skill tier actually extracts from the
// same visible lean, the same way a human's precision is a skill and not
// just a matter of holding the stick over. Kept off the visible crab itself
// rather than steepening it per skill: a race where the AI visibly leaned
// harder as it got better would read as the bots getting sloppier at low
// skill, not more careful.
#define BOT_CRAB_MAX            ((F1_0*7)/10)	// steepest crab on a straight, about 35 degrees --
												// enough that a bot with a good trichord skill can
												// actually clear RACE_TRICHORD_FLOOR on one, not
												// just in a corner's deeper lean
#define BOT_CRAB_LIMIT          F1_0			// ...and with a corner leaned into, about 45
#define BOT_BEND_CRAB           (F1_0 + F1_0/5)	// how hard a corner itself pulls the lean over,
												// on top of the style's own -- a bit more than a
												// straight gets, so the field visibly leans into
												// a corner rather than just holding its own crab
												// through it

static const struct {
	fix	throttle;	// fraction of the ship's own maximum thrust
	fix	turn;
	fix	gun_delay;
	fix	punch;		// what the slide is worth; F1_0 + 73% is a clean trichord
	fix	trichord;	// multiplier on the crab fed to race_trichord_advance_charge() --
					// F1_0 is "reads its own lean at face value"; below that is a driver
					// who can't quite hold the angle steady enough to cash it in fully,
					// above it is one who is getting more out of the same lean than its
					// raw angle would suggest
} Bot_skill[RACE_NUM_SKILLS] = {
	// A rookie can just about clear RACE_TRICHORD_FLOOR on a straight at its
	// own best style -- and only just, so it rarely holds the charge long
	// enough to matter outside a corner's deeper lean.
	{ (F1_0*78)/100, F1_0/4, (F1_0*5)/4, F1_0 + (F1_0*30)/100, (F1_0*9)/10 },		// RACE_SKILL_ROOKIE
	// A pro clears the floor with real room to spare and charges at a
	// meaningful rate down any straight, not just through corners.
	{ (F1_0*90)/100, F1_0/6, F1_0,       F1_0 + (F1_0*55)/100, F1_0 + (F1_0*15)/100 },	// RACE_SKILL_PRO
	// An ace is what "professional at the angle" looks like: close enough to
	// a perfect diagonal on an ordinary straight that it charges almost as
	// fast as if it were holding one outright.
	{ F1_0,          F1_0/8, (F1_0*2)/3, F1_0 + (F1_0*80)/100, F1_0 + (F1_0*2)/5 },	// RACE_SKILL_ACE
};

// How each driver goes about it. Skill says how fast the whole field is; this
// says how they differ from one another, so a race reads as seven drivers
// rather than one driver rendered seven times. Everything here is a multiplier
// on the skill row above, so raising the difficulty raises all of them.
typedef struct bot_style {
	fix	throttle;	// on the skill row's throttle
	fix	turn;		// on its turn rate; above F1_0 is lazier hands
	fix	reach;		// on how far up the road it aims, which is how hard it cuts
	int	lane;		// which line it likes, in lane steps either side of the middle
	int	drift;		// how fast that line wanders across the tube (0 = holds it)
	fix	aggression;	// how far it leaves the line for a box, how eagerly it shoots
	fix	burner;		// on how clear a road it wants before lighting the afterburner
	fix	crab;		// on how far it leans the ship into the slide, at most BOT_CRAB_MAX
} bot_style;

static const bot_style Bot_style[] = {
	// Sits on the throttle and turns in late. Fast, and the first to put it
	// in a wall.
	{ F1_0 + F1_0/20,  (F1_0*9)/10,  (F1_0*6)/5,   0,     0, (F1_0*4)/5,     (F1_0*9)/10,  F1_0 },
	// Smooth and wide. Slower into a corner, quicker out of it, and never
	// where you expect it to be on the exit.
	{ (F1_0*19)/20,    (F1_0*6)/5,   (F1_0*9)/10,  1,  1400, F1_0/2,         F1_0,         (F1_0*3)/5 },
	// Wanders. Holds no line at all, which makes it a nuisance to pass.
	{ F1_0,            F1_0,         F1_0,        -1,  2600, (F1_0*3)/5,     (F1_0*21)/20, (F1_0*4)/5 },
	// Races the other cars rather than the track: hunts boxes, shoots at
	// everything, and flies flattest of all so its guns point where it is going.
	{ (F1_0*9)/10,     (F1_0*9)/10,  (F1_0*4)/5,   2,   900, F1_0 + F1_0/4,  (F1_0*6)/5,   (F1_0*2)/5 },
	// Cuts everything. Aims a long way up the road and lives on the inside.
	{ F1_0 + F1_0/40,  (F1_0*19)/20, (F1_0*7)/5,  -2,     0, (F1_0*7)/10,    (F1_0*19)/20, F1_0 },
	// Cautious. Stays off the walls and off everyone else, and is still there
	// at the end of the third lap.
	{ (F1_0*9)/10,     (F1_0*11)/10, (F1_0*19)/20, 3,   600, (F1_0*2)/5,     (F1_0*5)/4,   (F1_0*1)/2 },
};

#define BOT_NUM_STYLES ((int)(sizeof(Bot_style)/sizeof(Bot_style[0])))

//	-------------------------------------------------------------------------
//	State
//	-------------------------------------------------------------------------

typedef struct race_bot {
	int			active;
	int			pnum;
	int			route_idx;		// route point being driven at
	int			last_segnum;
	unsigned	cp_mask;		// checkpoints taken this lap, same set as the player's
	int			respawn_seg;	// segment of the last checkpoint taken; -1 until it takes one

	int			style;			// index into Bot_style
	fix			lane_base;		// the middle of this driver's own line across the tube
	int			lane_phase;		// where it sits in that line's wander
	fix			crab;			// how far it leans into the slide, signed: + is right
	fix			trichord_charge;	// charge bar fill -- see race_trichord_advance_charge()
	fix64		trichord_boost_until;	// active burst timer
	vms_vector	heading;		// where its thrust actually goes, which is not quite its nose

	fix64		respawn_at;
	int			wreck_objnum;	// ship left behind by a death, -1 when there is none
	fix64		stuck_since;
	vms_vector	stuck_pos;
	fix64		boost_until;
	fix64		invuln_until;	// start-of-race grace only -- see race_bots_start_race()

	fix64		blind_until;	// somebody's EMP is on it until
	fix64		tractor_until;	// ...and somebody's tractor beam until

	fix64		burner_until;	// afterburner lit until
	fix64		burner_ready;	// ...and not again before this
	fix64		burner_blob;	// next puff of trail

	int			item_class;		// CLASS_SECONDARY / CLASS_POWER, -1 for empty
	int			item_index;
	fix64		item_use_at;
	fix64		item_drop_at;

	fix64		next_primary;
	fix64		next_secondary;
	fix64		kit_refill[RACE_MAX_KIT_SECONDARIES];	// when the kit tops itself back up
	int			missile_gun;	// alternates left/right, the way a player's does
} race_bot;

// The skill row the menu picked, clamped so a stale config can't index off
// the end of the table.
#define BOT_SKILL   (Bot_skill[(Race_bot_skill >= 0 && Race_bot_skill < RACE_NUM_SKILLS) ? Race_bot_skill : RACE_SKILL_PRO])

static race_bot Race_bot[MAX_PLAYERS];		// indexed by player slot; slot 0 is never a bot
static int Race_bots_active = 0;

// One lap, as a closed ring of the engine pathfinder's own point_segs.
static point_seg Bot_route[BOT_ROUTE_MAX];
static int Bot_route_len = 0;
static int Bot_route_overflow = 0;

int race_bots_enabled(void)
{
	// Single player only. Nothing about a bot is networked -- no other
	// machine would see them, and the host is not authoritative over them.
	return (Game_mode & GM_RACE) && !(Game_mode & GM_MULTI) && Race_bot_count > 0;
}

int race_player_is_bot(int pnum)
{
	if (pnum <= 0 || pnum >= MAX_PLAYERS)
		return 0;

	return Race_bot[pnum].active;
}

int race_object_is_bot(const object *obj)
{
	if (!obj || obj->type != OBJ_PLAYER)
		return 0;

	return race_player_is_bot(obj->id);
}

int race_bot_field_size(void)
{
	return 1 + Race_bots_active;
}

// A `chance`-per-second roll, scaled by the frame so it does not quietly get
// more likely the faster the game runs. d_rand() spans 0..0x7fff, which is
// half of F1_0, hence the halving.
static int race_bot_roll(fix chance)
{
	return d_rand() < (fixmul(chance, FrameTime) >> 1);
}

static const bot_style *race_bot_style(const race_bot *b)
{
	return &Bot_style[(b->style >= 0 && b->style < BOT_NUM_STYLES) ? b->style : 0];
}

// Where this driver's line sits across the tube right now. A style with drift
// wanders slowly across its own line rather than running on it like a rail,
// which is most of what stops a field looking like a train.
static fix race_bot_lane(const race_bot *b)
{
	const bot_style *st = race_bot_style(b);
	fix s;

	if (!st->drift)
		return b->lane_base;

	fix_fastsincos((fix)((int)((GameTime64 * st->drift) >> 16) + b->lane_phase) & 0xffff, &s, NULL);

	return b->lane_base + fixmul(BOT_LANE_DRIFT, s);
}

//	-------------------------------------------------------------------------
//	The route
//	-------------------------------------------------------------------------
//
// The level says where the checkpoints are but not what order they come in --
// a lap is a set, not a sequence, which is what lets a human take a track's
// crossovers in any order. A bot needs a sequence, so one is built here: start
// at the line, repeatedly hop to whichever checkpoint is nearest through the
// mine, and come back to the line. On a loop, "nearest unvisited" is the next
// one round, which is the lap.
//
// Every leg is the engine's own pathfinder. Its scratch space is the same
// Point_segs pool the robot AI uses; the leg is copied straight out of it into
// Bot_route, so nothing here holds on to the pool afterwards.

// Runs one leg and returns how many point_segs it produced at
// Point_segs_free_ptr, or -1 if there is no way through.
static int race_route_path(int from_seg, int to_seg)
{
	short n_points = 0;
	int room = MAX_POINT_SEGS - (int)(Point_segs_free_ptr - Point_segs);

	if (!ConsoleObject || room < BOT_PATH_DEPTH * 2 + 4)
		return -1;

	if (create_path_points(ConsoleObject, from_seg, to_seg, Point_segs_free_ptr,
						   &n_points, BOT_PATH_DEPTH, 0, 1, -1) == -1)
		return -1;

	// The pathfinder returns what it managed rather than failing outright when
	// the goal is unreachable, so check that it actually arrived.
	if (n_points < 1 || Point_segs_free_ptr[n_points - 1].segnum != to_seg)
		return -1;

	return n_points;
}

// Appends the leg from `from_seg` to `to_seg` to the route, dropping its first
// point because that is where the previous leg left off. Returns the segment
// the route now ends on.
static int race_route_add_leg(int from_seg, int to_seg)
{
	int n = race_route_path(from_seg, to_seg);
	int i;

	if (n < 0)
		return from_seg;

	for (i = (Bot_route_len ? 1 : 0); i < n; i++)
	{
		// A half-written leg is worse than no route: the ring would not close
		// and the field would spend the race flying at a point through a wall.
		if (Bot_route_len >= BOT_ROUTE_MAX)
		{
			Bot_route_overflow = 1;
			return to_seg;
		}

		Bot_route[Bot_route_len++] = Point_segs_free_ptr[i];
	}

	return to_seg;
}

// Where on the route a world point sits.
static int race_route_nearest(const vms_vector *pos)
{
	int i, best = 0;
	fix best_dist = 0;

	for (i = 0; i < Bot_route_len; i++)
	{
		fix d = vm_vec_dist_quick(pos, &Bot_route[i].point);

		if (i == 0 || d < best_dist)
		{
			best_dist = d;
			best = i;
		}
	}

	return best;
}

// Walks back along the route from `from` until it has covered `distance` in
// world units, and returns where it got to.
static int race_route_back(int from, fix distance)
{
	fix travelled = 0;
	int idx = from;
	int guard = 0;

	if (Bot_route_len <= 1)
		return from;

	while (travelled < distance && guard++ < Bot_route_len)
	{
		int prev = (idx - 1 + Bot_route_len) % Bot_route_len;

		travelled += vm_vec_dist_quick(&Bot_route[idx].point, &Bot_route[prev].point);
		idx = prev;
	}

	return idx;
}

// Same as race_route_back(), but forward. Used to shove an unsticking bot
// clear of whatever it is wedged against rather than dropping it right back
// where it got stuck.
static int race_route_forward(int from, fix distance)
{
	fix travelled = 0;
	int idx = from;
	int guard = 0;

	if (Bot_route_len <= 1)
		return from;

	while (travelled < distance && guard++ < Bot_route_len)
	{
		int next = (idx + 1) % Bot_route_len;

		travelled += vm_vec_dist_quick(&Bot_route[idx].point, &Bot_route[next].point);
		idx = next;
	}

	return idx;
}

// The direction the track runs where `pos` is, taken off the lap route.
// Returns 0 and leaves *dir alone when there is no route to read.
int race_route_direction(const vms_vector *pos, vms_vector *dir)
{
	int idx;
	vms_vector d;

	if (Bot_route_len < 2 || !pos || !dir)
		return 0;

	idx = race_route_nearest(pos);

	vm_vec_sub(&d, &Bot_route[(idx + 1) % Bot_route_len].point, &Bot_route[idx].point);

	if (vm_vec_mag_quick(&d) < F1_0/16)
		return 0;

	vm_vec_normalize_quick(&d);
	*dir = d;

	return 1;
}

// Which way round the ring is forwards. "Nearest unvisited checkpoint" is
// direction-blind: on a loop the checkpoint closest to the line is as likely
// to be the last one before it as the first one after, and a route built off
// that runs the lap backwards. A player start faces down the track, because
// that is what a starting position is for, so if the route disagrees with the
// grid it is the route that gets turned round.
static void race_route_orient(void)
{
	vms_vector dir;
	int idx, i;

	if (Bot_route_len < 2 || NumNetPlayerPositions < 1)
		return;

	idx = race_route_nearest(&Player_init[0].pos);

	vm_vec_sub(&dir, &Bot_route[(idx + 1) % Bot_route_len].point, &Bot_route[idx].point);

	if (vm_vec_mag_quick(&dir) < F1_0/16)
		return;

	vm_vec_normalize_quick(&dir);

	if (vm_vec_dot(&dir, &Player_init[0].orient.fvec) >= 0)
		return;

	for (i = 0; i < Bot_route_len/2; i++)
	{
		point_seg tmp = Bot_route[i];

		Bot_route[i] = Bot_route[Bot_route_len - 1 - i];
		Bot_route[Bot_route_len - 1 - i] = tmp;
	}
}

static void race_route_build(void)
{
	short checkpoints[RACE_MAX_CHECKPOINTS + 1];
	char used[RACE_MAX_CHECKPOINTS + 1];
	int n_cp = 0, i, start, cur;

	Bot_route_len = 0;
	Bot_route_overflow = 0;

	// race_init_level() runs ahead of init_ai_objects(), so the pathfinder's
	// scratch pool is still holding the last level's paths. There are no
	// robots in a race and init_ai_objects() resets it again a moment later,
	// so the whole pool is ours.
	Point_segs_free_ptr = Point_segs;

	// One entry per checkpoint, not one per segment: a checkpoint can be a
	// block of connected matcens, and routing to the same one four times over
	// would build a lap that visits it four times.
	for (i = 0; i < race_checkpoint_count() && n_cp < RACE_MAX_CHECKPOINTS; i++)
	{
		int seg = race_checkpoint_segment(i);

		if (seg < 0 || race_segment_is_finish(seg))
			continue;

		checkpoints[n_cp++] = (short)seg;
	}

	memset(used, 0, sizeof(used));

	// The line is where a lap starts and ends. A track that marks none has one
	// of its checkpoints standing in as it, so there is always a start.
	start = Race_finish_segnum;

	if (start < 0)
	{
		if (!n_cp)
			return;

		start = checkpoints[0];
		used[0] = 1;
	}

	cur = start;

	for (;;)
	{
		int best = -1, best_len = 0, j;

		for (j = 0; j < n_cp; j++)
		{
			int len;

			if (used[j])
				continue;

			len = race_route_path(cur, checkpoints[j]);

			if (len < 0)
				continue;

			if (best < 0 || len < best_len)
			{
				best = j;
				best_len = len;
			}
		}

		if (best < 0)
			break;

		used[best] = 1;
		cur = race_route_add_leg(cur, checkpoints[best]);
	}

	// ...and back to the line, closing the ring.
	race_route_add_leg(cur, start);

	// The closing leg ends where the first one began, so the ring carries the
	// start point twice. Drop the copy: a route with two points in the same
	// place has a zero-length step in it, which every "which way does the
	// track go here" question in this file divides by.
	while (Bot_route_len > 2 &&
		   vm_vec_dist_quick(&Bot_route[Bot_route_len - 1].point, &Bot_route[0].point) < F1_0)
		Bot_route_len--;

	if (Bot_route_len < 2 || Bot_route_overflow)
		Bot_route_len = 0;

	race_route_orient();
}

//	-------------------------------------------------------------------------
//	The field
//	-------------------------------------------------------------------------

static object *race_bot_live_object(const race_bot *b)
{
	int objnum = Players[b->pnum].objnum;
	object *obj;

	if (objnum < 0 || objnum > Highest_object_index)
		return NULL;

	obj = &Objects[objnum];

	if (obj->type != OBJ_PLAYER || obj->id != b->pnum || (obj->flags & OF_SHOULD_BE_DEAD))
		return NULL;

	return obj;
}

// Gives a bot a ship if it hasn't got one, at whichever start position its
// slot maps to. Where it lines up is decided afterwards, on the route, so this
// only has to land somewhere legal; a start position that doesn't exist falls
// back to the one every level is guaranteed to have.
static object *race_bot_spawn_object(race_bot *b)
{
	int start = b->pnum % (NumNetPlayerPositions > 0 ? NumNetPlayerPositions : 1);
	object *obj = race_bot_live_object(b);
	int objnum;

	if (obj)
		return obj;

	objnum = obj_create(OBJ_PLAYER, b->pnum, Player_init[start].segnum, &Player_init[start].pos,
						&Player_init[start].orient, Polygon_models[Player_ship->model_num].rad,
						CT_NONE, MT_PHYSICS, RT_POLYOBJ);

	if (objnum < 0 && start != 0)
		objnum = obj_create(OBJ_PLAYER, b->pnum, Player_init[0].segnum, &Player_init[0].pos,
							&Player_init[0].orient, Polygon_models[Player_ship->model_num].rad,
							CT_NONE, MT_PHYSICS, RT_POLYOBJ);

	if (objnum < 0)
	{
		b->active = 0;
		return NULL;
	}

	Players[b->pnum].objnum = objnum;

	// The same set-up a netgame gives every ship that isn't yours: the player
	// ship model, this slot's colour, and the player ship's mass and drag.
	multi_reset_player_object(&Objects[objnum]);

	// ...plus the two things a remote ship never needs, because a remote ship
	// is moved by position packets rather than by flying: its own engine, and
	// the levelling that rolls a flying ship upright in the tube. Without the
	// second one a bot's bank is whatever the last turn happened to leave it
	// at, and it spends the race sideways or upside down.
	Objects[objnum].mtype.phys_info.flags |= PF_USES_THRUST | PF_LEVELLING;
	Objects[objnum].shields = StartingShields;
	Players[b->pnum].shields = StartingShields;

	return &Objects[objnum];
}

// Puts a bot on the route at point `idx`, pointed the way the track goes.
// Used for the starting grid and to put a wreck back on the track, which is
// the same deal a human gets rather than a trip back to the line.
static void race_bot_place(race_bot *b, int idx)
{
	object *obj = race_bot_spawn_object(b);
	vms_vector fvec;
	int seg;

	if (!obj || !b->active)
		return;

	// Nothing to line up against: leave it where it spawned.
	if (Bot_route_len <= 0 || idx < 0 || idx >= Bot_route_len)
	{
		b->last_segnum = obj->segnum;
		b->stuck_pos = obj->pos;
		b->respawn_at = 0;
		return;
	}

	b->route_idx = idx;

	// Placed at the middle of the cube the route point is in, not on the route
	// point itself. Half of them sit on the face between two segments -- that
	// is what create_path_points()'s safety pass puts there -- and a ship
	// dropped exactly on a wall plane starts the race half inside geometry,
	// where it wedges and never moves.
	if (Bot_route[idx].segnum >= 0 && Bot_route[idx].segnum <= Highest_segment_index)
		compute_segment_center(&obj->pos, &Segments[Bot_route[idx].segnum]);
	else
		obj->pos = Bot_route[idx].point;

	vm_vec_sub(&fvec, &Bot_route[(idx + 1) % Bot_route_len].point, &Bot_route[idx].point);

	if (vm_vec_mag_quick(&fvec) > F1_0/16)
	{
		vm_vec_normalize_quick(&fvec);
		vm_vector_2_matrix(&obj->orient, &fvec, NULL, NULL);
	}

	// A nudge onto this bot's own line, so two ships on the same stretch of
	// track are beside each other rather than inside each other.
	if (b->lane_base)
		vm_vec_scale_add2(&obj->pos, &obj->orient.rvec, b->lane_base);

	// A route point can sit on the face between two segments and the nudge
	// moves it again, so ask the geometry which segment the ship ended up in.
	seg = find_point_seg(&obj->pos, Bot_route[idx].segnum);

	if (seg < 0)
	{
		compute_segment_center(&obj->pos, &Segments[Bot_route[idx].segnum]);
		seg = find_point_seg(&obj->pos, Bot_route[idx].segnum);
	}

	if (seg >= 0)
	{
		int spread_seg = seg;

		// The lane above spreads the field along one line across the tube,
		// which is what a moving grid wants -- but a wreck coming back at a
		// checkpoint isn't moving yet, and lane_base alone still lines two
		// same-styled (or human-plus-bot) racers up nose to nose on it. This
		// is the same per-slot ring race_get_respawn() gives the human, so a
		// checkpoint two racers share sends them to two different points
		// around it rather than one.
		race_spread_respawn(&obj->pos, &obj->orient, b->pnum, &spread_seg);
		seg = spread_seg;
	}

	obj_relink(obj - Objects, (seg >= 0) ? seg : Bot_route[idx].segnum);

	vm_vec_zero(&obj->mtype.phys_info.velocity);
	vm_vec_zero(&obj->mtype.phys_info.thrust);
	vm_vec_zero(&obj->mtype.phys_info.rotvel);

	b->last_segnum = obj->segnum;
	b->respawn_at = 0;
	b->stuck_since = 0;
	b->stuck_pos = obj->pos;
	b->burner_until = 0;
	b->burner_ready = 0;
	b->heading = obj->orient.fvec;

	// The teleport-in a real ship gets, from the same call multi_do_player_
	// explode() makes when somebody respawns.
	create_player_appearance_effect(obj);

	// A racer's colour is their player number (see get_color_for_player), and
	// a respawn builds a whole new object -- so the ship's textures are set
	// again here rather than trusted to survive it. A bot that came back in
	// somebody else's colours would break the one thing the standings, the
	// minimap dots and the ship in front of you all have to agree on.
	multi_reset_object_texture(obj);

	Players[b->pnum].shields = StartingShields;

	// Box loot is lost on a wreck, the class kit never is -- the same deal the
	// human gets on every spawn. This also puts the class's shield bar on,
	// which is why it runs after StartingShields rather than before.
	race_grant_class_loadout(b->pnum);

	obj->shields = Players[b->pnum].shields;
}

// Back onto the track at the last checkpoint it took, which is where
// race_get_respawn() puts the human. Falls back to where it was on the route
// if it has not reached one yet, so a bot wrecked on the run to the first
// checkpoint does not get sent back to the grid.
static void race_bot_respawn(race_bot *b)
{
	int idx = b->route_idx;

	if (b->respawn_seg >= 0 && b->respawn_seg <= Highest_segment_index && Bot_route_len > 0)
	{
		vms_vector center;

		compute_segment_center(&center, &Segments[b->respawn_seg]);
		idx = race_route_nearest(&center);
	}

	race_bot_place(b, idx);
}

void race_bots_claim_slots(void)
{
	int i, want;

	Race_bots_active = 0;
	memset(Race_bot, 0, sizeof(Race_bot));

	if (!race_bots_enabled())
		return;

	want = Race_bot_count;

	if (want > RACE_MAX_BOTS)
		want = RACE_MAX_BOTS;
	if (want > MAX_PLAYERS - 1)
		want = MAX_PLAYERS - 1;

	if (NumNetPlayerPositions < 1)
		return;			// no start positions at all; nothing to build a grid from

	// A track built for one ship has one start. Synthesise the rest off the
	// ones it does have, spread sideways so the grid isn't a stack of ships
	// inside each other.
	for (i = NumNetPlayerPositions; i <= want; i++)
	{
		int src = i % NumNetPlayerPositions;
		int rank = i / NumNetPlayerPositions;

		Player_init[i] = Player_init[src];
		vm_vec_scale_add2(&Player_init[i].pos, &Player_init[src].orient.rvec,
						  ((i & 1) ? -1 : 1) * i2f(6) * (rank + 1));
		vm_vec_scale_add2(&Player_init[i].pos, &Player_init[src].orient.fvec, -i2f(8) * rank);
	}

	if (want >= NumNetPlayerPositions)
		NumNetPlayerPositions = want + 1;

	for (i = 1; i <= want; i++)
	{
		race_bot *b = &Race_bot[i];

		b->active = 1;
		b->pnum = i;
		b->item_class = -1;
		b->item_index = -1;
		b->wreck_objnum = -1;
		b->respawn_seg = -1;

		if (!race_bot_spawn_object(b))
			continue;

		snprintf(Players[i].callsign, CALLSIGN_LEN + 1, "CPU%d", i);
		Players[i].connected = CONNECT_PLAYING;
		Players[i].net_killed_total = 0;
		Players[i].net_kills_total = 0;
		Players[i].score = 0;

		Race_bots_active++;
	}

	// Every ranking, standings list and results screen counts racers by
	// N_players, so this is the one line that makes the bots part of the race
	// rather than scenery flying round the track.
	N_players = 1 + Race_bots_active;
}

void race_bots_init(void)
{
	int i, slot = 0, base = 0;
	int style_seed = d_rand() % BOT_NUM_STYLES;
	int class_seed = d_rand() % RACE_NUM_CLASSES;

	Bot_route_len = 0;

	if (!(Game_mode & GM_RACE))
	{
		Race_bots_active = 0;
		return;
	}

	// Built for every race, not only one with a field in it: the standings
	// tiebreak and the respawn heading both read the route, and both matter
	// just as much in a netgame where there are no bots at all.
	race_route_build();

	if (!race_bots_enabled())
	{
		Race_bots_active = 0;
		return;
	}

	con_printf(CON_NORMAL, "RACE BOTS: %d in the field, route is %d points "
			   "(%d checkpoints, line at segment %d)\n",
			   Race_bots_active, Bot_route_len, race_checkpoint_count(), Race_finish_segnum);

	// No route means no bot can drive a metre, and a field of parked ships on
	// the grid is worse than no field at all: they sit on top of the human,
	// who cannot move until something explodes.
	if (Bot_route_len <= 0 && Race_bots_active)
	{
		for (i = 1; i < MAX_PLAYERS; i++)
		{
			object *obj = race_bot_live_object(&Race_bot[i]);

			if (Race_bot[i].active && obj)
				obj_delete(obj - Objects);

			Race_bot[i].active = 0;
		}

		Race_bots_active = 0;
		N_players = 1;

		HUD_init_message_literal(HM_DEFAULT, "NO ROUTE ON THIS TRACK -- RACING SOLO");
		return;
	}

	if (ConsoleObject)
		base = race_route_nearest(&ConsoleObject->pos);

	// The starting grid. The level's own start positions are authored for a
	// deathmatch -- there may be only one of them, and the synthesised ones
	// are guesses -- so the field is strung out along the route instead, in
	// single file back from wherever the human is sitting.
	for (i = 1; i < MAX_PLAYERS; i++)
	{
		race_bot *b = &Race_bot[i];

		if (!b->active)
			continue;

		slot++;

		b->route_idx = 0;
		b->cp_mask = 0;
		b->respawn_at = 0;
		b->respawn_seg = -1;
		b->wreck_objnum = -1;
		b->stuck_since = 0;
		b->boost_until = 0;
		b->invuln_until = 0;
		b->blind_until = 0;
		b->tractor_until = 0;
		b->burner_until = 0;
		b->burner_ready = 0;
		b->burner_blob = 0;
		b->item_class = -1;
		b->item_index = -1;
		b->item_use_at = 0;
		b->item_drop_at = 0;
		b->next_primary = 0;
		b->next_secondary = 0;
		b->missile_gun = 0;
		memset(b->kit_refill, 0, sizeof(b->kit_refill));

		// A driving style each, dealt round the field from a random start so a
		// grid of three is as mixed as a grid of seven.
		b->style = (style_seed + slot) % BOT_NUM_STYLES;

		// Its own line across the tube, so the field spreads out instead of
		// flying nose to tail down one rail. Kept close to the middle: a lane
		// wide enough to reach the wall is a lane that drives into it.
		{
			int step = race_bot_style(b)->lane;

			if (step > BOT_LANE_MAX)
				step = BOT_LANE_MAX;
			if (step < -BOT_LANE_MAX)
				step = -BOT_LANE_MAX;

			b->lane_base = BOT_LANE_SPACING * step;
			b->lane_phase = d_rand() & 0xffff;
		}

		// Which way it leans into the slide. Left/right only -- a bot that
		// crabbed vertically as well would spend the race flying on its side
		// or its back, which is what a full trichord looks like and is not
		// what a race should.
		b->crab = fixmul(BOT_CRAB_MAX, race_bot_style(b)->crab);

		if (!(slot & 1))
			b->crab = -b->crab;

		b->heading = ConsoleObject ? ConsoleObject->orient.fvec : Player_init[0].orient.fvec;

		// A class each, dealt round the field the same way rather than rolled
		// per bot: a full grid then fields every kit exactly once, instead of
		// three Trappers and no afterburner anywhere.
		race_set_player_class(i, (class_seed + slot) % RACE_NUM_CLASSES);

		race_bot_place(b, race_route_back(base, BOT_GRID_SPACING * slot));
	}
}

//	-------------------------------------------------------------------------
//	Progress: checkpoints and laps
//	-------------------------------------------------------------------------

// The same rules race_check_checkpoint() applies to the human, applied to a
// bot: checkpoints are a set, the line closes the lap once the set is full,
// and the first crossing off the grid only starts the clock.
static void race_bot_segment(race_bot *b, int segnum)
{
	race_player_info *rp = &Race_player[b->pnum];
	int cp;

	if (segnum == b->last_segnum || segnum < 0 || segnum > Highest_segment_index)
		return;

	b->last_segnum = segnum;

	if (rp->finished)
		return;

	if (race_segment_is_finish(segnum))
	{
		if (!rp->has_checkpoint && !rp->laps_completed)
		{
			rp->has_checkpoint = 1;		// drove off the grid over the line
			return;
		}

		if (rp->checkpoints_hit < race_checkpoint_total())
			return;						// cut the track; the lap doesn't count

		rp->laps_completed++;
		rp->checkpoints_hit = 0;
		b->cp_mask = 0;
		b->respawn_seg = segnum;		// a fresh lap respawns at the line

		if (rp->laps_completed >= Race_laps_to_win)
			race_finish_player(b->pnum);

		return;
	}

	cp = race_checkpoint_of_segment(segnum);

	if (cp < 0 || cp >= RACE_MAX_CHECKPOINTS)
		return;

	if (b->cp_mask & (1u << cp))
		return;

	b->cp_mask |= (1u << cp);
	rp->checkpoints_hit++;
	rp->has_checkpoint = 1;
	rp->last_checkpoint = (ubyte)cp;
	b->respawn_seg = segnum;			// where a wreck comes back, same as the human
}

void race_bot_check_segment(int pnum, int segnum)
{
	if (!race_player_is_bot(pnum))
		return;

	race_bot_segment(&Race_bot[pnum], segnum);
}

fix race_bot_dist_to_next(int pnum)
{
	const race_bot *b;
	object *obj;
	fix best = -1;
	int i;

	if (!race_player_is_bot(pnum))
		return -1;

	b = &Race_bot[pnum];
	obj = race_bot_live_object(b);

	if (!obj)
		return -1;

	// Whichever of the checkpoints it still owes is nearest -- or the line,
	// once it owes none. Measured the same way race.c measures everyone
	// else's, so the two are comparable as a tiebreak.
	if (Race_player[pnum].checkpoints_hit >= race_checkpoint_total())
	{
		vms_vector center;

		if (Race_finish_segnum < 0)
			return 0;

		compute_segment_center(&center, &Segments[Race_finish_segnum]);

		return vm_vec_dist_quick(&center, &obj->pos);
	}

	for (i = 0; i < race_checkpoint_count(); i++)
	{
		vms_vector center;
		int seg = race_checkpoint_segment(i);
		fix dist;

		if (seg < 0 || race_segment_is_finish(seg) || (b->cp_mask & (1u << i)))
			continue;

		compute_segment_center(&center, &Segments[seg]);
		dist = vm_vec_dist_quick(&center, &obj->pos);

		if (best < 0 || dist < best)
			best = dist;
	}

	return (best < 0) ? 0 : best;
}

void race_bots_take_power(int power, int roller)
{
	int i;

	if (!race_bots_enabled())
		return;

	for (i = 1; i < MAX_PLAYERS; i++)
	{
		race_bot *b = &Race_bot[i];

		if (!b->active || i == roller)
			continue;			// never the racer who rolled it

		if (power == RACE_POWER_EMP)
			b->blind_until = GameTime64 + RACE_POWER_EMP_TIME;
		else
			b->tractor_until = GameTime64 + RACE_POWER_TIME;
	}
}

int race_route_progress(int pnum)
{
	int objnum;

	if (Bot_route_len <= 0 || pnum < 0 || pnum >= MAX_PLAYERS)
		return -1;

	objnum = Players[pnum].objnum;

	if (objnum < 0 || objnum > Highest_object_index || Objects[objnum].type != OBJ_PLAYER)
		return -1;

	return race_route_nearest(&Objects[objnum].pos);
}

//	-------------------------------------------------------------------------
//	Items and weapons
//	-------------------------------------------------------------------------

// Defined with the driving code, which asks the same question of the track.
static int race_bot_can_see(const object *obj, const vms_vector *target);

// Boxes are not networked objects and bots do not go through the player
// collision path, so they take one the same way they take a corner: by being
// close enough to it.
static void race_bot_check_boxes(race_bot *b, object *obj)
{
	int i, tries;

	if (b->item_class >= 0)
		return;			// already carrying; a bot holds one item

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *pow = &Objects[i];
		int box, wclass, index;

		if (pow->type != OBJ_POWERUP || (pow->flags & OF_SHOULD_BE_DEAD))
			continue;

		box = race_box_index_from_object(pow);

		if (box < 0)
			continue;

		if (vm_vec_dist_quick(&pow->pos, &obj->pos) > obj->size + pow->size + i2f(3))
			continue;

		race_box_taken(box, b->pnum, 0);

		// The same draw the player gets, off race.c's own loot table. A bot
		// has no weapon rack and no fuse: it fires what it drew and it is
		// gone. A primary is the one thing it cannot use that way -- its class
		// gun is always loaded -- so that draw is spent again rather than
		// leaving the box having handed out nothing.
		for (tries = 0; tries < 4; tries++)
		{
			if (!race_roll_box_item(b->pnum, &wclass, &index))
				break;

			if (wclass == CLASS_PRIMARY)
				continue;

			b->item_class = wclass;
			b->item_index = index;
			b->item_use_at = GameTime64 + BOT_ITEM_HOLD;
			b->item_drop_at = GameTime64 + BOT_ITEM_TIMEOUT;
			break;
		}

		return;
	}
}

// The racer one place in front of this one, human or bot -- the one it is
// actually racing, and so the one it most wants out of the way.
static int race_bot_rival(const race_bot *b)
{
	int sorted[MAX_PLAYERS];
	int n = race_get_positions(sorted);
	int i;

	for (i = 1; i < n; i++)
		if (sorted[i] == b->pnum)
			return sorted[i - 1];

	return -1;
}

// Whoever this bot can actually shoot: the nearest racer it has a clear line
// to and is roughly pointed at. Its rival counts double, so it stays the
// preferred target while anything else in the way is still fair game -- bots
// shoot each other, not just the human.
static object *race_bot_pick_target(const race_bot *b, const object *obj, fix range, fix mindot)
{
	int rival = race_bot_rival(b);
	object *best = NULL;
	fix best_score = 0;
	int i;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		int objnum = Players[i].objnum;
		object *other;
		vms_vector dir;
		fix dist, score;

		if (i == b->pnum || objnum < 0 || objnum > Highest_object_index)
			continue;

		other = &Objects[objnum];

		if (other->type != OBJ_PLAYER || other->id != i || (other->flags & OF_SHOULD_BE_DEAD))
			continue;
		if (Race_player[i].finished)
			continue;			// already home; shooting them settles nothing

		vm_vec_sub(&dir, &other->pos, &obj->pos);
		dist = vm_vec_normalize_quick(&dir);

		// Against where the ship is going, not where its nose is pointing: a
		// bot flying a crab has its nose well off the track, and gating on
		// that would mean it only ever shot at the walls.
		if (dist > range || vm_vec_dot(&dir, &b->heading) < mindot)
			continue;

		score = (i == rival) ? dist/2 : dist;

		if (best && score >= best_score)
			continue;

		if (!race_bot_can_see(obj, &other->pos))
			continue;

		best = other;
		best_score = score;
	}

	return best;
}

// Can this ship fly straight from where it is to `target` without hitting the
// track? Asked with the hull radius, so "clear" means clear for the ship and
// not just for a hairline.
static int race_bot_can_see(const object *obj, const vms_vector *target)
{
	fvi_query fq;
	fvi_info hit;

	fq.p0 = (vms_vector *)&obj->pos;
	fq.p1 = (vms_vector *)target;
	fq.startseg = obj->segnum;
	fq.rad = obj->size;
	fq.thisobjnum = obj - Objects;
	fq.ignore_obj_list = NULL;
	fq.flags = FQ_TRANSWALL;		// walls only; other ships are not an obstruction

	return find_vector_intersection(&fq, &hit) == HIT_NONE;
}

// Fires a missile the way a player fires one: out of the ship's own missile
// gun, alternating sides where the rack does, with the launch sound and the
// recoil the heavy stuff kicks back.
static void race_bot_fire_secondary(race_bot *b, object *obj, int index, const vms_vector *dir)
{
	int weapon_index = Secondary_weapon_to_weapon_info[index];
	int gun = Secondary_weapon_to_gun_num[index];

	if (gun == 4)					// alternate left/right, same as do_missile_firing()
		gun += (b->missile_gun++ & 1);

	Laser_player_fire(obj, weapon_index, gun, 1, 0, *dir);

	if (index == MEGA_INDEX || index == SMISSILE5_INDEX)
	{
		vms_vector force_vec;

		force_vec.x = -(obj->orient.fvec.x << 7);
		force_vec.y = -(obj->orient.fvec.y << 7);
		force_vec.z = -(obj->orient.fvec.z << 7);
		phys_apply_force(obj, &force_vec);
	}

	// The rack's own rate of fire, but never faster than this: a bot is meant
	// to pick its moment, not empty the rack down the first straight.
	b->next_secondary = GameTime64 + max(Weapon_info[weapon_index].fire_wait, BOT_MISSILE_GAP);
}

// The class gun, fired the way a player fires it: through do_laser_firing(),
// so it comes out of the ship's own gun points with the right sound, spread,
// quad bolts and helix roll rather than as a bare projectile spawned on the
// nose.
static void race_bot_fire_primary(race_bot *b, object *obj)
{
	const race_class_info *ci = race_get_class_info(race_get_class(b->pnum));
	const bot_style *st = race_bot_style(b);
	object *target;
	vms_vector dir;
	fix saved_charge;
	int flags = 0;

	if (!ci || GameTime64 < b->next_primary)
		return;

	target = race_bot_pick_target(b, obj, BOT_GUN_RANGE, BOT_GUN_DOT);

	if (!target)
		return;

	vm_vec_sub(&dir, &target->pos, &obj->pos);
	vm_vec_normalize_quick(&dir);

	if (Players[b->pnum].flags & PLAYER_FLAGS_QUAD_LASERS)
		flags |= LASER_QUAD;

	// Fusion reads its damage off the global charge meter and clears it, which
	// is the local player's. A bot takes a snap shot and gives it straight
	// back rather than emptying the human's capacitor from across the track.
	saved_charge = Fusion_charge;
	Fusion_charge = 0;

	do_laser_firing(obj - Objects, ci->primary, Players[b->pnum].laser_level, flags, 1, dir);

	Fusion_charge = saved_charge;

	// The gun's own rate of fire, off the weapon table, slowed by how hard the
	// field is racing and quickened by how much this driver likes shooting.
	b->next_primary = GameTime64 +
		fixmul(Weapon_info[Primary_weapon_to_weapon_info[ci->primary]].fire_wait +
			   BOT_SKILL.gun_delay,
			   st->aggression ? fixdiv(F1_0, st->aggression) : F1_0);
}

// The class kit restocks itself on a timer. race_grant_class_loadout() only
// starts that clock for the local player, so a bot keeps its own -- otherwise
// it fires the half-load it spawned with and races the rest of the way empty.
static void race_bot_refill_kit(race_bot *b)
{
	const race_class_info *ci = race_get_class_info(race_get_class(b->pnum));
	int i;

	if (!ci)
		return;

	for (i = 0; i < RACE_MAX_KIT_SECONDARIES; i++)
	{
		const race_kit_secondary *ks = &ci->secondary[i];

		if (ks->index < 0 || !ks->refill)
			continue;

		if (!b->kit_refill[i])
		{
			b->kit_refill[i] = GameTime64 + ks->refill;
			continue;
		}

		if (GameTime64 < b->kit_refill[i])
			continue;

		b->kit_refill[i] = GameTime64 + ks->refill;

		if (Players[b->pnum].secondary_ammo[ks->index] < ks->cap)
			Players[b->pnum].secondary_ammo[ks->index]++;
	}
}

// The class kit's own missiles. Box loot is rare; this is what makes a bot on
// your tail something to watch rather than scenery.
static void race_bot_fire_kit_missile(race_bot *b, object *obj)
{
	const race_class_info *ci = race_get_class_info(race_get_class(b->pnum));
	object *target;
	vms_vector dir;
	int i;

	if (!ci || GameTime64 < b->next_secondary)
		return;

	for (i = 0; i < RACE_MAX_KIT_SECONDARIES; i++)
	{
		int index = ci->secondary[i].index;

		if (index < 0 || Players[b->pnum].secondary_ammo[index] <= 0)
			continue;

		// Mines are laid, not aimed: a Trapper drops one behind it wherever it
		// happens to be, which is the whole point of the kit.
		if (index == PROXIMITY_INDEX || index == SMART_MINE_INDEX)
		{
			vms_vector back;

			// Once every few seconds, so a lap ends up with a mine on a corner
			// rather than a wall of them down every straight.
			if (!race_bot_roll(fixmul(F1_0/4, race_bot_style(b)->aggression)))
				continue;

			vm_vec_copy_scale(&back, &b->heading, -F1_0);
			Players[b->pnum].secondary_ammo[index]--;
			race_bot_fire_secondary(b, obj, index, &back);
			return;
		}

		target = race_bot_pick_target(b, obj, BOT_FIRE_RANGE, BOT_FIRE_DOT);

		if (!target)
			return;

		vm_vec_sub(&dir, &target->pos, &obj->pos);
		vm_vec_normalize_quick(&dir);

		Players[b->pnum].secondary_ammo[index]--;
		race_bot_fire_secondary(b, obj, index, &dir);
		return;
	}
}

static void race_bot_use_item(race_bot *b, object *obj)
{
	object *target;
	vms_vector dir;

	if (b->item_class < 0 || GameTime64 < b->item_use_at || GameTime64 < b->next_secondary)
		return;

	// A power hits the whole field the moment it goes off, so there is nothing
	// to aim and nothing to wait for.
	if (b->item_class == CLASS_POWER)
	{
		race_power_trigger(b->item_index, b->pnum, 0);
		b->item_class = -1;
		return;
	}

	// The shaker flies at whoever is leading no matter which way it is pointed
	// (see race_homing_target), so a bot that is not itself leading just
	// launches it down the road.
	if (b->item_index == SMISSILE5_INDEX)
	{
		if (race_get_rank(b->pnum) <= 1)
			return;

		race_bot_fire_secondary(b, obj, b->item_index, &b->heading);
		b->item_class = -1;
		return;
	}

	target = race_bot_pick_target(b, obj, BOT_FIRE_RANGE, BOT_FIRE_DOT);

	if (target)
	{
		vm_vec_sub(&dir, &target->pos, &obj->pos);
		vm_vec_normalize_quick(&dir);

		race_bot_fire_secondary(b, obj, b->item_index, &dir);
		b->item_class = -1;
		return;
	}

	// Never found a shot. Rather than hoard it for the whole race, put it down
	// the road once the hold runs long.
	if (GameTime64 > b->item_drop_at)
	{
		race_bot_fire_secondary(b, obj, b->item_index, &b->heading);
		b->item_class = -1;
	}
}

//	-------------------------------------------------------------------------
//	Driving
//	-------------------------------------------------------------------------

// Advance the route point the bot is working on. A point is done when the bot
// is inside it or has driven past its plane -- past matters, because a fast
// ship can overshoot a portal by more than the arrival radius in one frame.
static void race_bot_advance(race_bot *b, const object *obj)
{
	int guard = 0;

	// Lost the thread of the route -- shoved off the track in a fight, knocked
	// down a side passage, or ticked past a knot of close-together points
	// while standing still. Pick the route back up where it actually is. Every
	// way a bot ends up flying at a point behind itself goes through here.
	if (vm_vec_dist_quick(&Bot_route[b->route_idx].point, &obj->pos) > BOT_LOST_DIST)
		b->route_idx = race_route_nearest(&obj->pos);

	// Capped, so a bot sitting still among close-together points cannot tick
	// its way a lap up the route and then turn round to fly back to it.
	while (guard++ < BOT_ADVANCE_MAX)
	{
		vms_vector to_pt, ahead;
		fix dist;

		vm_vec_sub(&to_pt, &Bot_route[b->route_idx].point, &obj->pos);
		dist = vm_vec_mag_quick(&to_pt);

		if (dist >= BOT_ARRIVE_DIST)
		{
			vm_vec_sub(&ahead, &Bot_route[(b->route_idx + 1) % Bot_route_len].point,
					   &Bot_route[b->route_idx].point);

			if (vm_vec_dot(&to_pt, &ahead) >= 0 || dist >= BOT_ARRIVE_DIST*3)
				break;
		}

		b->route_idx = (b->route_idx + 1) % Bot_route_len;
	}
}

// The furthest route point up the road the bot can still fly straight to. This
// is the racing line: aiming past the point it is on is what cuts a corner,
// and letting the geometry decide how far past means the cut is as deep as the
// track actually allows.
static int race_bot_aim_index(const race_bot *b, const object *obj, fix *reach)
{
	fix limit = fixmul(BOT_LOOKAHEAD_DIST, race_bot_style(b)->reach);
	int best = b->route_idx;
	int steps;

	*reach = 0;

	for (steps = 1; steps <= BOT_LOOKAHEAD_STEPS; steps++)
	{
		int cand = (b->route_idx + steps) % Bot_route_len;
		fix dist = vm_vec_dist_quick(&obj->pos, &Bot_route[cand].point);

		if (dist > limit)
			break;

		if (!race_bot_can_see(obj, &Bot_route[cand].point))
			break;

		best = cand;
		*reach = dist;
	}

	return best;
}

// A mystery box worth going for: up the road, reachable in a straight line,
// and no further off the line than this driver is willing to stray. How far
// that is comes off its style, so the ones that race the other cars go and get
// items and the ones that race the track drive past them.
static const object *race_bot_find_box(const race_bot *b, const object *obj,
									   const vms_vector *to_goal)
{
	const bot_style *st = race_bot_style(b);
	const object *best = NULL;
	fix best_dist = 0;
	fix reach = fixmul(BOT_BOX_LOOK, st->aggression);
	fix mindot = F1_0 - fixmul(F1_0 - BOT_BOX_DOT, st->aggression);
	int i;

	if (b->item_class >= 0)
		return NULL;			// already carrying; a bot holds one item

	for (i = 0; i <= Highest_object_index; i++)
	{
		const object *pow = &Objects[i];
		vms_vector to_box;
		fix dist;

		if (pow->type != OBJ_POWERUP || (pow->flags & OF_SHOULD_BE_DEAD))
			continue;
		if (race_box_index_from_object(pow) < 0)
			continue;

		vm_vec_sub(&to_box, &pow->pos, &obj->pos);
		dist = vm_vec_mag_quick(&to_box);

		if (dist > reach || dist < F1_0)
			continue;

		vm_vec_normalize_quick(&to_box);

		// Against the way the track goes, not the way the ship happens to be
		// pointing. This is the whole "take it if it is on the way" rule: a
		// box more than a few degrees off the line is next lap's problem.
		if (vm_vec_dot(&to_box, to_goal) < mindot)
			continue;

		if (best && dist >= best_dist)
			continue;

		if (!race_bot_can_see(obj, &pow->pos))
			continue;

		best = pow;
		best_dist = dist;
	}

	return best;
}

// How hard a nearby ship dead ahead gets steered around, rather than driven
// into. Nothing else in this file keeps the field off each other -- the
// racing line does not know another ship is sitting on it -- so without this
// a bot behind a slower one on a straight just holds course into its tail,
// and a starting grid is a wall of ships nobody is looking at.
#define BOT_AVOID_DIST          i2f(24)	// how far ahead it starts caring
#define BOT_AVOID_DOT           ((F1_0*17)/20)	// how nearly dead ahead counts as "in the way"
#define BOT_AVOID_PUSH           i2f(10)	// how far off the line the goal point gets shoved

// A sideways nudge to the racing line, away from another ship this one would
// otherwise drive straight into. Zero when the road ahead is clear.
static vms_vector race_bot_avoid(const race_bot *b, const object *obj, const vms_vector *to_goal)
{
	vms_vector push = { 0, 0, 0 };
	fix closest = 0;
	int i;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		int objnum;
		object *other;
		vms_vector to_other, side;
		fix dist, dot;

		if (i == b->pnum)
			continue;

		objnum = Players[i].objnum;

		if (objnum < 0 || objnum > Highest_object_index)
			continue;

		other = &Objects[objnum];

		if (other->type != OBJ_PLAYER || other->id != i || (other->flags & OF_SHOULD_BE_DEAD))
			continue;

		vm_vec_sub(&to_other, &other->pos, &obj->pos);
		dist = vm_vec_mag_quick(&to_other);

		if (dist > BOT_AVOID_DIST || dist < F1_0)
			continue;

		vm_vec_normalize_quick(&to_other);
		dot = vm_vec_dot(&to_other, to_goal);

		if (dot < BOT_AVOID_DOT)
			continue;			// not in the way of where it's actually headed

		if (closest && dist >= closest)
			continue;

		closest = dist;

		// Sideways, off whichever flank the other ship is already leaning to
		// -- the cross of the road direction and world up gives a consistent
		// left/right without caring which way the tube itself is rolled.
		vm_vec_cross(&side, to_goal, &obj->orient.uvec);

		if (vm_vec_normalize_quick(&side) < F1_0/16)
			continue;

		if (vm_vec_dot(&side, &to_other) > 0)
			vm_vec_negate(&side);

		vm_vec_copy_scale(&push, &side, fixdiv(BOT_AVOID_PUSH, F1_0/4 + fixdiv(dist, BOT_AVOID_DIST)));
	}

	return push;
}

// The afterburner, for the classes whose kit comes with one. Lit on a straight
// the bot can see all the way down, and only once the tank has refilled --
// which is the same tank, the same drain and the same trail the player's key
// gives, so a bot on the burner looks and sounds like a player on the burner.
// Returns the extra thrust multiplier, F1_0 when it is not burning.
static fix race_bot_afterburner(race_bot *b, object *obj, fix dot, fix reach)
{
	player *p = &Players[b->pnum];
	const race_class_info *ci = race_get_class_info(race_get_class(b->pnum));
	fix charge_up, cur_energy;

	if (!(p->flags & PLAYER_FLAGS_AFTERBURNER))
		return F1_0;

	if (GameTime64 >= b->burner_until)
	{
		fix want_dot = fixmul(BOT_BURN_MIN_DOT, race_bot_style(b)->burner);

		// Never all the way to 1: a dot of exactly straight does not happen,
		// and a patient driver that waits for it is a driver that never burns.
		if (want_dot > (F1_0*63)/64)
			want_dot = (F1_0*63)/64;

		if (GameTime64 >= b->burner_ready && dot >= want_dot && reach >= BOT_BURN_MIN_REACH &&
			p->afterburner_charge > F1_0/2)
		{
			b->burner_until = GameTime64 + BOT_BURN_TIME;
			b->burner_blob = 0;
			// One-shot rather than the looping version the local player's key
			// starts: nothing stops a bot's loop if it is wrecked mid-burn.
			digi_link_sound_to_object2(SOUND_AFTERBURNER_IGNITE, obj - Objects, 0, F1_0, i2f(256));
		}
	}

	if (GameTime64 < b->burner_until && p->afterburner_charge > 0)
	{
		fix scale = f1_0 + min(f1_0/2, p->afterburner_charge) * 2;

		p->afterburner_charge -= fixmul(FrameTime/AFTERBURNER_USE_SECS,
										(ci && ci->afterburner_drain) ? ci->afterburner_drain : F1_0);

		if (p->afterburner_charge <= 0)
		{
			p->afterburner_charge = 0;
			b->burner_until = 0;
			b->burner_ready = GameTime64 + BOT_BURN_COOLDOWN;
		}

		if (GameTime64 >= b->burner_blob)
		{
			b->burner_blob = GameTime64 + BOT_BURN_BLOB_TIME;
			drop_afterburner_blobs(obj, 2, i2f(5)/2, -1);
		}

		return scale;
	}

	if (GameTime64 >= b->burner_until && b->burner_until)
	{
		b->burner_until = 0;
		b->burner_ready = GameTime64 + BOT_BURN_COOLDOWN;
	}

	// Refills exactly as the player's does, off the same 8-second clock.
	charge_up = min(FrameTime/8, f1_0 - p->afterburner_charge);
	cur_energy = max(p->energy - i2f(10), 0);
	charge_up = min(charge_up, cur_energy/10);

	if (charge_up > 0)
	{
		p->afterburner_charge += charge_up;
		p->energy -= charge_up * 100 / 10;
	}

	return F1_0;
}

// Where a bot's thrust actually goes: down its nose, leaned slightly to one
// side by its crab. Sideways only -- the ship's own right vector, so the lean
// follows the ship as the tube rolls it -- and never up or down, which is what
// keeps it flying down the track rather than along it on its side.
static void race_bot_push_dir(const object *obj, fix crab, vms_vector *push)
{
	*push = obj->orient.fvec;
	vm_vec_scale_add2(push, &obj->orient.rvec, crab);
	vm_vec_normalize_quick(push);
}

// Which way the track bends between where the bot is on the route and where it
// is aiming, as a signed lateral in the ship's own frame. Zero on a straight,
// because it is the *change* in the route's direction and not the direction
// itself -- so a bot holds its own lean down a straight and rolls over into a
// corner when one arrives, the way somebody flying the ship would.
static fix race_bot_bend(const race_bot *b, const object *obj, int aim)
{
	vms_vector d0, d1;

	vm_vec_sub(&d0, &Bot_route[(b->route_idx + 1) % Bot_route_len].point,
			   &Bot_route[b->route_idx].point);
	vm_vec_sub(&d1, &Bot_route[(aim + 1) % Bot_route_len].point, &Bot_route[aim].point);

	if (vm_vec_mag_quick(&d0) < F1_0/16 || vm_vec_mag_quick(&d1) < F1_0/16)
		return 0;

	vm_vec_normalize_quick(&d0);
	vm_vec_normalize_quick(&d1);
	vm_vec_sub2(&d1, &d0);

	return vm_vec_dot(&d1, &obj->orient.rvec);
}

static void race_bot_drive(race_bot *b, object *obj)
{
	const race_class_info *ci = race_get_class_info(race_get_class(b->pnum));
	const bot_style *st = race_bot_style(b);
	const object *box;
	vms_vector goal, to_goal, along, lateral, push, aim_vec;
	fix throttle, turn, thrust, dist, dot, reach, lane, burner, crab;
	int aim;

	if (Bot_route_len <= 0)
		return;

	race_bot_advance(b, obj);

	aim = race_bot_aim_index(b, obj, &reach);
	goal = Bot_route[aim].point;

	// Onto this driver's own line across the tube. Taken across the ship's own
	// up, because a world up collapses to nothing in a vertical shaft and race
	// tracks are full of them.
	lane = race_bot_lane(b);

	vm_vec_sub(&along, &Bot_route[(aim + 1) % Bot_route_len].point, &goal);

	if (lane && vm_vec_mag_quick(&along) > F1_0/16)
	{
		vm_vec_normalize_quick(&along);
		vm_vec_cross(&lateral, &along, &obj->orient.uvec);

		if (vm_vec_mag_quick(&lateral) > F1_0/16)
		{
			vm_vec_normalize_quick(&lateral);
			vm_vec_scale_add2(&goal, &lateral, lane);
		}
	}

	// The racing line, settled before anything else gets a say in it. If the
	// goal point is (near) exactly where the ship already is -- a stationary
	// bot sitting on top of its own waypoint at the start, or one whose
	// last-known heading happens to line up perfectly -- there is no direction
	// in that vector to steer by. It must NOT be treated as "nothing to do":
	// bailing out here without ever touching thrust is a deadlock, because
	// nothing else in this function runs to set it, thrust stays at whatever
	// it last was (zero, off the grid), the ship never moves, next frame
	// recomputes the identical zero-length vector, and it sits there forever.
	// Falling back to the direction the track runs here keeps the ship moving
	// regardless, which is what breaks the loop.
	vm_vec_sub(&to_goal, &goal, &obj->pos);
	dist = vm_vec_normalize_quick(&to_goal);

	if (dist < F1_0/16)
	{
		vm_vec_sub(&to_goal, &Bot_route[(aim + 1) % Bot_route_len].point,
				   &Bot_route[aim].point);

		if (vm_vec_normalize_quick(&to_goal) < F1_0/16)
			to_goal = obj->orient.fvec;
	}

	// Only now, a box -- and only one that is already sitting on that line.
	// Racing is the job; an item is something you collect on the way to
	// doing it.
	box = race_bot_find_box(b, obj, &to_goal);

	if (box)
	{
		vms_vector box_to_goal;

		vm_vec_sub(&box_to_goal, &box->pos, &obj->pos);

		// Same rule as above: a box exactly where the ship is standing is not
		// a reason to stop steering, so keep the racing-line direction instead
		// of replacing it with nothing.
		if (vm_vec_normalize_quick(&box_to_goal) >= F1_0/16)
		{
			goal = box->pos;
			to_goal = box_to_goal;
		}
	}

	// Steer around whoever is directly in the way, box detour or no -- a
	// bot that swerved for an item straight into another ship's tail hasn't
	// gained anything.
	{
		vms_vector avoid = race_bot_avoid(b, obj, &to_goal);

		if (avoid.x || avoid.y || avoid.z)
		{
			vm_vec_add2(&goal, &avoid);
			vm_vec_sub(&to_goal, &goal, &obj->pos);

			if (vm_vec_normalize_quick(&to_goal) < F1_0/16)
				to_goal = obj->orient.fvec;
		}
	}

	turn = fixmul(BOT_SKILL.turn, st->turn);

	if (ci && ci->turn && ci->turn != F1_0)
		turn = fixdiv(turn, ci->turn);

	// Its own lean, plus a roll into whatever the track is doing up ahead. The
	// second half is what stops a bot looking like it is on rails: the lean
	// changes through every corner instead of being a fixed offset it holds
	// for three laps.
	crab = b->crab + fixmul(BOT_BEND_CRAB, race_bot_bend(b, obj, aim));

	if (crab > BOT_CRAB_LIMIT)
		crab = BOT_CRAB_LIMIT;
	if (crab < -BOT_CRAB_LIMIT)
		crab = -BOT_CRAB_LIMIT;

	race_bot_push_dir(obj, crab, &push);

	// Aim the nose off by the error between where it points and where the push
	// goes, so the push -- not the nose -- ends up on the racing line. Re-solved
	// every frame, so the ship settles into its lean as it swings round rather
	// than needing the exact rotation worked out up front. At the fixed point
	// the correction is zero.
	aim_vec = to_goal;
	vm_vec_add2(&aim_vec, &obj->orient.fvec);
	vm_vec_sub2(&aim_vec, &push);

	if (vm_vec_normalize_quick(&aim_vec) < F1_0/16)
		aim_vec = to_goal;

	// Jammed by somebody's EMP: it cannot see the line any better than the
	// human can, so it wanders off it until the EMP clears.
	if (GameTime64 < b->blind_until)
	{
		vms_vector wobble;

		make_random_vector(&wobble);
		vm_vec_scale_add2(&aim_vec, &wobble, F1_0/5);
		vm_vec_normalize_quick(&aim_vec);
	}

	ai_turn_towards_vector(&aim_vec, obj, turn);

	// Where it is going after that turn. Everything below reads this rather
	// than the nose -- including the guns, which is why it is kept on the bot.
	race_bot_push_dir(obj, crab, &push);
	b->heading = push;

	// Throttle off the heading, the way the engine's own path follower scales
	// speed: pushing where it wants to go, stay on the power; push off the
	// line, lift for the corner.
	dot = vm_vec_dot(&to_goal, &push);

	if (dot < 0)
		dot = 0;

	// A quarter throttle is the floor: a bot pointed the wrong way should be
	// turning, not driving, but it still has to be able to crawl off a wall.
	throttle = fixmul(BOT_SKILL.throttle, F1_0/4 + fixmul((F1_0*3)/4, dot));
	throttle = fixmul(throttle, st->throttle);

	if (ci && ci->speed)
		throttle = fixmul(throttle, ci->speed);

	// The same charge-gated trichord reward a human stick earns, off the one
	// diagonal a bot actually flies: how hard it has leaned into the current
	// lean (crab), against the hardest it is ever allowed to
	// (BOT_CRAB_LIMIT), then scaled by the skill row's own `trichord` -- a
	// professional-tier bot gets noticeably more out of the same visible
	// lean than a rookie does, the way a skilled human holds an angle more
	// precisely than a beginner without necessarily looking any different
	// doing it. A bot never adds the vertical axis (see
	// race_bot_push_dir()'s comment), so this is the fair equivalent rather
	// than the identical input -- but it runs through the identical charge
	// meter (race_trichord_advance_charge(), race.h), each bot keeping its
	// own in b->trichord_charge, so a bot has to hold its lean for it to pay
	// off the same way a human holding a diagonal does.
	{
		fix trichord_ratio = fixmul(fixdiv(crab < 0 ? -crab : crab, BOT_CRAB_LIMIT), BOT_SKILL.trichord);

		if (trichord_ratio > F1_0)
			trichord_ratio = F1_0;

		b->trichord_charge = race_trichord_advance_charge(b->trichord_charge, trichord_ratio, &b->trichord_boost_until);
	}
	throttle = fixmul(throttle, race_trichord_scale_from_boost(b->trichord_boost_until));

	// Somebody's tractor beam, the same 50% it takes off the human.
	if (GameTime64 < b->tractor_until)
		throttle = fixmul(throttle, RACE_TRACTOR_SCALE);

	// The burner, on the classes that carry one, down a road it can see the
	// end of. A box detour is not a road, and neither is a road you cannot
	// see -- but the call still happens, because that is also where the tank
	// refills.
	burner = race_bot_afterburner(b, obj,
								  (box || GameTime64 < b->blind_until) ? 0 : dot,
								  (box || GameTime64 < b->blind_until) ? 0 : reach);

	if (burner != F1_0)
		throttle = fixmul(throttle, burner);

	// Boost pads push a bot exactly as hard as they push a player.
	if (GameTime64 < b->boost_until)
	{
		fix push = F1_0 + F1_0/4;

		if (ci && ci->boost_power && ci->boost_power != F1_0)
			push = fixmul(push, ci->boost_power);

		throttle = fixmul(throttle, F1_0 + push);
	}

	// The crab set the direction; the punch sets how hard. Pushed along the
	// leaned heading the aim above has already lined up with the track, so a
	// bot accelerates down the road and not across it.
	thrust = fixmul(Player_ship->max_thrust, fixmul(throttle, BOT_SKILL.punch));

	vm_vec_copy_scale(&obj->mtype.phys_info.thrust, &push, thrust);
}

// A bot that has stopped moving is wedged on geometry. Give it a moment to
// drive out on its own, then lift it back onto the route -- a race with a bot
// stuck in a corner for three laps is worse than one with a bot that blinked.
static void race_bot_check_stuck(race_bot *b, object *obj)
{
	vms_vector center, out;

	if (vm_vec_dist_quick(&obj->pos, &b->stuck_pos) > i2f(6))
	{
		b->stuck_pos = obj->pos;
		b->stuck_since = GameTime64;
		return;
	}

	if (!b->stuck_since)
	{
		b->stuck_since = GameTime64;
		return;
	}

	if (GameTime64 - b->stuck_since > BOT_UNSTICK_TIME)
	{
		// Before the first checkpoint, race_bot_respawn() falls back to
		// b->route_idx -- which is exactly where it just got stuck. On a
		// crowded starting grid that is the middle of the same jam that stuck
		// it in the first place, so it lands right back in it and the whole
		// six seconds runs again, forever, with nothing short of the human
		// shooting a gap open. Nudge it up the route first so it reappears
		// clear of the pack instead of back inside it. Once it holds a
		// checkpoint this is moot -- respawn already returns it there instead.
		if (b->respawn_seg < 0 && Bot_route_len > 0)
			b->route_idx = race_route_forward(b->route_idx, BOT_GRID_SPACING * 3);

		race_bot_respawn(b);
		return;
	}

	if (GameTime64 - b->stuck_since > BOT_STUCK_TIME)
	{
		// Back off towards the middle of the cube, the way a human would.
		compute_segment_center(&center, &Segments[obj->segnum]);
		vm_vec_sub(&out, &center, &obj->pos);

		if (vm_vec_mag_quick(&out) > F1_0)
		{
			vm_vec_normalize_quick(&out);
			vm_vec_copy_scale(&obj->mtype.phys_info.thrust, &out, Player_ship->max_thrust/2);
		}
		else
			vm_vec_copy_scale(&obj->mtype.phys_info.thrust, &obj->orient.fvec,
							  -Player_ship->max_thrust/2);
	}
}

int race_bot_take_damage(object *botobj, object *killer, fix damage)
{
	int pnum;
	race_bot *b;

	if (!(Game_mode & GM_RACE) || !race_object_is_bot(botobj))
		return 0;

	pnum = botobj->id;
	b = &Race_bot[pnum];

	if (b->respawn_at)
		return 1;			// already wrecked and waiting to come back

	if (b->invuln_until && GameTime64 < b->invuln_until)
		return 1;			// start-of-race grace -- see race_bots_start_race()

	// Both halves of the class trade: what the shooter's kit hits for and what
	// this one takes. The local player gets this in apply_damage_to_player();
	// a bot never reaches that path, so it gets it here.
	damage = race_scale_damage_for(pnum, killer, damage);

	Players[pnum].shields -= damage;
	botobj->shields = Players[pnum].shields;

	if (Players[pnum].shields > 0)
	{
		// The same hit confirmation a human victim gets in apply_damage_to_
		// player() -- a bot never reaches that function, so without this a
		// shot that doesn't wreck one is silent.
		digi_link_sound_to_pos(SOUND_PLAYER_GOT_HIT, botobj->segnum, 0, &botobj->pos, 0, F1_0);
		return 1;
	}

	// Wrecked. Worth saying out loud when the human did it -- there is no kill
	// list in a single-player race to say it for us -- with the same
	// confirmation sound a netgame kill gets, so a kill in a race reads and
	// sounds like a kill anywhere else.
	if (killer && killer->type == OBJ_WEAPON &&
		killer->ctype.laser_info.parent_num == Players[Player_num].objnum)
	{
		HUD_init_message(HM_DEFAULT, "WRECKED %s", Players[pnum].callsign);
		digi_play_sample(SOUND_HUD_KILL, F3_0);
	}

	// Booked as wrecked before the explosion, because the explosion damages
	// what is around it and that comes straight back through this function.
	b->respawn_at = GameTime64 + BOT_RESPAWN_TIME;
	b->wreck_objnum = botobj - Objects;
	b->item_class = -1;			// loses whatever it was carrying, same as a human
	b->burner_until = 0;

	// The same death a real ship gets: explode_badass_player() is exactly what
	// multi_do_player_explode() runs when somebody else's ship goes up, so a
	// bot dying looks, sounds and hits like a player dying -- the ship's own
	// explosion vclip, the badass sound, and a blast that everything nearby
	// has to survive.
	explode_badass_player(botobj);

	// The wreck is cleared by race_bots_frame(), not here.
	// obj_delete_all_that_should_be_dead() deletes everything carrying this
	// flag EXCEPT player objects -- a player is only ever killed through the
	// local death sequence or, in a netgame, by the machine that owns it. A
	// bot has neither, so the flagged ship would sit on the track for the rest
	// of the race. Deleting it here is not safe either: we are inside
	// collision handling, walking the segment list the object is linked into.
	botobj->flags |= OF_SHOULD_BE_DEAD;
	botobj->render_type = RT_NONE;			// gone this frame, not next
	botobj->movement_type = MT_NONE;
	vm_vec_zero(&botobj->mtype.phys_info.velocity);

	return 1;
}

//	-------------------------------------------------------------------------
//	Frame
//	-------------------------------------------------------------------------

// Anything wearing a bot's player slot that the slot itself no longer points
// at is a body nothing owns. obj_delete_all_that_should_be_dead() walks past
// player objects that are not the local player, so if one is ever left behind
// -- by a death this file did not book, by a respawn that raced it -- it would
// sit on the track for the rest of the race. This is the sweep that guarantees
// it does not.
static void race_bots_clear_wrecks(void)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *obj = &Objects[i];

		if (obj->type != OBJ_PLAYER || obj->id <= 0 || obj->id >= MAX_PLAYERS)
			continue;

		if (!Race_bot[obj->id].active)
			continue;

		if (Players[obj->id].objnum == i && !(obj->flags & OF_SHOULD_BE_DEAD))
			continue;

		if (Players[obj->id].objnum == i)
			Players[obj->id].objnum = -1;

		if (Race_bot[obj->id].wreck_objnum == i)
			Race_bot[obj->id].wreck_objnum = -1;

		obj_delete(i);
	}
}

void race_bots_frame(void)
{
	int i;

	if (!race_bots_enabled() || !Race_bots_active)
		return;

	// Before anything drives: no bot ever shares the track with its own
	// corpse. Here we are between frames rather than inside a collision, so
	// deleting is safe -- see the comment in race_bot_take_damage().
	race_bots_clear_wrecks();

	for (i = 1; i < MAX_PLAYERS; i++)
	{
		race_bot *b = &Race_bot[i];
		object *obj;

		if (!b->active)
			continue;

		if (b->respawn_at)
		{
			if (GameTime64 >= b->respawn_at)
				race_bot_respawn(b);

			continue;
		}

		obj = race_bot_live_object(b);

		if (!obj)
		{
			b->respawn_at = GameTime64 + BOT_RESPAWN_TIME;
			continue;
		}

		// Held on the grid with everyone else. The countdown is the start of
		// the race, so a bot that crept off the line would be cheating in the
		// most visible way possible.
		if (race_countdown_active())
		{
			vm_vec_zero(&obj->mtype.phys_info.thrust);
			vm_vec_zero(&obj->mtype.phys_info.velocity);
			continue;
		}

		race_bot_segment(b, obj->segnum);

		if (obj->segnum >= 0 && obj->segnum <= Highest_segment_index &&
			Segment2s[obj->segnum].special == SEGMENT_IS_FUELCEN &&
			GameTime64 >= b->boost_until)
		{
			const race_class_info *ci = race_get_class_info(race_get_class(i));
			fix life = BOT_BOOST_TIME;

			// A class built around the pads holds one for longer, same as the
			// human's does.
			if (ci && ci->boost_time && ci->boost_time != F1_0)
				life = fixmul(life, ci->boost_time);

			b->boost_until = GameTime64 + life;
		}

		race_bot_drive(b, obj);

		if (Race_player[i].finished)
			continue;		// still drives, so the track doesn't fill up with parked ships

		race_bot_check_stuck(b, obj);
		race_bot_check_boxes(b, obj);
		race_bot_refill_kit(b);
		race_bot_use_item(b, obj);
		race_bot_fire_kit_missile(b, obj);
		race_bot_fire_primary(b, obj);

		// Racing is about driving. Nobody runs dry, bots included.
		Players[i].energy = MAX_ENERGY;
	}
}

void race_bots_start_race(void)
{
	int i;

	if (!race_bots_enabled())
		return;

	for (i = 1; i < MAX_PLAYERS; i++)
	{
		race_bot *b = &Race_bot[i];

		if (!b->active)
			continue;

		// The same boost-pad push a human off the line gets, off the same
		// clock race_bot_drive() already reads -- see the SEGMENT_IS_FUELCEN
		// check in race_bots_frame().
		b->boost_until = GameTime64 + RACE_START_BOOST_TIME;
		b->invuln_until = GameTime64 + RACE_START_INVULN_TIME;
	}
}
