/*
 * Race game mode -- checkpoints, laps, mystery boxes, boost pads and race
 * presentation. See race.h for the design rationale.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "race.h"
#include "racebot.h"
#include "game.h"
#include "player.h"
#include "object.h"
#include "powerup.h"
#include "fuelcen.h"
#include "gameseg.h"
#include "multi.h"
#include "hudmsg.h"
#include "timer.h"
#include "dxxerror.h"
#include "digi.h"
#include "sounds.h"
#include "vclip.h"
#include "render.h"
#include "kmatrix.h"
#include "screens.h"
#include "window.h"
#include "weapon.h"
#include "maths.h"
#include "laser.h"
#include "ai.h"
#include "aistruct.h"
#include "u_mem.h"
#include "console.h"
#include "key.h"
#include "gauges.h"

race_player_info Race_player[MAX_PLAYERS];
int Race_num_checkpoints = 0;
int Race_finish_checkpoint = -1;	// matcen acting as the line, or -1 if a goal segment is
int Race_finish_segnum = -1;
int Race_finish_marked = 0;			// level marks its line with repair centers
static int Race_respawn_segnum = -1;	// segment of the last checkpoint the local player took
int Race_num_boxes = 0;
int Race_laps_to_win = RACE_DEFAULT_LAPS;
int Race_powerup_chance = 100;
int Race_allowed_items = RACE_ALLOWED_ITEMS_ALL;

static fix Race_countdown_timer = 0;	// seconds remaining, fix; only meaningful while Race_counting_down
static int Race_counting_down = 0;
static int Race_go_announced = 0;
static int Race_countdown_voice = -1;	// last whole second the start voice called out
static ubyte Race_next_place = 1;		// host-side: place to hand out to the next finisher
static fix64 Race_wrongway_next_warn[MAX_PLAYERS];

// Mystery boxes: one pickup orb per CTF-goal segment, plus any energy powerup
// the level itself placed (see race_adopt_box).
typedef struct race_box {
	int		segnum;
	vms_vector pos;
	int		objnum;			// -1 while the orb is taken/respawning
	int		signature;		// signature of Objects[objnum], to catch object slot reuse
	fix64	respawn_at;		// GameTime64 at which to put the orb back; 0 when the orb is up
} race_box;

static race_box Race_box[RACE_MAX_BOXES];

static fix64 Race_boost_until = 0;
static fix64 Race_boost_started = 0;
static int Race_last_boost_seg = -1;	// segment race_check_boost_pad() last looked at
static int Race_last_seg_checked = -1;	// segment race_check_checkpoint() last looked at
// A crossing just processed the finish line -- ignore it again until this
// passes, so a ship that lingers there doesn't read its own recent crossing
// as a fresh one that skipped every checkpoint. One second is comfortably
// more than it takes to clear the line's segment(s) and nowhere near a real
// lap time, even on a tiny track.
#define RACE_FINISH_GRACE_TIME (F1_0*1)
static fix64 Race_finish_grace_until = 0;
static fix64 Race_next_state_send = 0;

// Local player's race clock. Everything is measured off GameTime64, which is
// zeroed at level start.
static fix64 Race_start_time = 0;		// GameTime64 when GO fired
static fix64 Race_lap_start = 0;		// GameTime64 when the current lap began
static fix64 Race_total_time = 0;		// frozen elapsed time once finished
static fix64 Race_splits[RACE_MAX_SPLITS];
static fix64 Race_best_lap = 0;
static int Race_num_splits = 0;
static int Race_summary_pending = 0;

// Which checkpoints the local player has crossed on the current lap. Order
// doesn't matter -- the lap closes when the set is complete and they cross the
// line -- so this is a set, not a sequence.
static unsigned Race_cp_mask = 0;
static fix64 Race_incomplete_next_warn = 0;

#define RACE_BANNER_SECONDS 2

static char Race_banner_text[32] = "";
static fix64 Race_banner_until = 0;
static int Race_banner_style = RACE_BANNER_NORMAL;

// Progress metric shared by ranking and by the host state broadcast. Strictly
// increases as a player advances, so it is safe to use to reject stale state.
// Defined with the rest of the class code below; the mystery box loot above
// needs it to know whose fuse it is lighting.
static const race_class_info *race_my_class(void);

// Defined with the loot table below, where the powerup-to-weapon mapping lives.
static int race_weapon_of_powerup(int powerup_id, int *wclass, int *index);
static void race_init_items(void);

static int race_progress_of(const race_player_info *rp)
{
	// Stride is one more than the set size: checkpoints_hit reaches the full
	// count on the lap's last checkpoint, and a lap boundary must still step
	// progress forward rather than land on the same number.
	int stride = Race_num_checkpoints + 1;

	return rp->laps_completed * stride + rp->checkpoints_hit;
}

static int race_is_multi(void)
{
	return (Game_mode & GM_MULTI) != 0;
}

static void race_show_banner(const char *text, int style)
{
	strncpy(Race_banner_text, text, sizeof(Race_banner_text) - 1);
	Race_banner_text[sizeof(Race_banner_text) - 1] = 0;
	Race_banner_until = GameTime64 + i2f(RACE_BANNER_SECONDS);
	Race_banner_style = style;
}

//	-------------------------------------------------------------------------
//	Mystery boxes
//	-------------------------------------------------------------------------

static void race_spawn_box(int box)
{
	race_box *rb = &Race_box[box];
	object *obj;
	int objnum, i;

	rb->objnum = -1;
	rb->signature = 0;
	rb->respawn_at = GameTime64 + RACE_BOX_RESPAWN_TIME;	// back off and retry if we bail out below

	if (rb->segnum < 0 || rb->segnum > Highest_segment_index)
		return;

	// Clear out any powerup already sitting in the box segment. Normally
	// there is none, but a client that just synced with the host will have
	// received the host's copy of this orb as a plain object, and we want
	// exactly one orb per box on every machine. Another box's orb is left
	// alone, or two boxes sharing a segment would keep killing each other.
	for (i = 0; i <= Highest_object_index; i++)
	{
		if (Objects[i].type != OBJ_POWERUP || Objects[i].segnum != rb->segnum)
			continue;
		if (race_box_index_from_object(&Objects[i]) >= 0)
			continue;

		Objects[i].flags |= OF_SHOULD_BE_DEAD;
	}

	objnum = obj_create(OBJ_POWERUP, POW_ENERGY, rb->segnum, &rb->pos, &vmd_identity_matrix,
						Powerup_info[POW_ENERGY].size, CT_POWERUP, MT_NONE, RT_POWERUP);
	if (objnum < 0)
		return;

	obj = &Objects[objnum];
	obj->rtype.vclip_info.vclip_num = Powerup_info[POW_ENERGY].vclip_num;
	obj->rtype.vclip_info.frametime = Vclip[obj->rtype.vclip_info.vclip_num].frame_time;
	obj->rtype.vclip_info.framenum = 0;
	obj->lifeleft = IMMORTAL_TIME;

	rb->objnum = objnum;
	rb->signature = obj->signature;
	rb->respawn_at = 0;
}

// Adopt an energy orb the level itself placed as a mystery box. A mapper can
// then mark boxes either with a repair-center segment or by simply dropping an
// energy powerup, which is far easier in an editor -- and it stops a stray
// energy pickup being un-collectable in a mode where energy is already pinned
// to full.
static int race_adopt_box(int objnum)
{
	race_box *rb;

	if (Race_num_boxes >= RACE_MAX_BOXES)
		return 0;

	rb = &Race_box[Race_num_boxes];
	rb->segnum = Objects[objnum].segnum;
	rb->pos = Objects[objnum].pos;
	rb->objnum = objnum;
	rb->signature = Objects[objnum].signature;
	rb->respawn_at = 0;

	// Keep it around: level powerups have a finite life, box orbs don't.
	Objects[objnum].lifeleft = IMMORTAL_TIME;

	Race_num_boxes++;

	return 1;
}

static void race_init_boxes(void)
{
	int i;

	Race_num_boxes = 0;

	for (i = 0; i <= Highest_segment_index && Race_num_boxes < RACE_MAX_BOXES; i++)
	{
		if (Segment2s[i].special != SEGMENT_IS_GOAL_BLUE &&
			Segment2s[i].special != SEGMENT_IS_GOAL_RED)
			continue;

		Race_box[Race_num_boxes].segnum = i;
		compute_segment_center(&Race_box[Race_num_boxes].pos, &Segments[i]);
		Race_box[Race_num_boxes].objnum = -1;
		Race_box[Race_num_boxes].signature = 0;
		// Spawn on the first frame rather than here: race_init_level() runs
		// during multi_prep_level(), before a joining client has synced the
		// object array with the host, so anything created now would be
		// thrown away (or duplicated) by that sync.
		Race_box[Race_num_boxes].respawn_at = 1;
		Race_num_boxes++;
	}
}

int race_box_index_from_object(const object *powerup)
{
	int i, objnum;

	if (!powerup || Race_num_boxes <= 0)
		return -1;

	objnum = powerup - Objects;

	for (i = 0; i < Race_num_boxes; i++)
		if (Race_box[i].objnum == objnum && Race_box[i].signature == powerup->signature)
			return i;

	return -1;
}

void race_box_taken(int box, int pnum, int broadcast)
{
	race_box *rb;

	if (box < 0 || box >= Race_num_boxes)
		return;

	rb = &Race_box[box];

	if (rb->objnum >= 0)
	{
		object *obj = &Objects[rb->objnum];

		// Only kill the object if it is still the orb we spawned; the slot
		// may have been recycled if something else already removed it.
		if (obj->signature == rb->signature)
			obj->flags |= OF_SHOULD_BE_DEAD;

		rb->objnum = -1;
		rb->signature = 0;
		rb->respawn_at = GameTime64 + RACE_BOX_RESPAWN_TIME;
	}
	else if (!rb->respawn_at)
	{
		// Already gone but with no respawn pending (shouldn't happen); make
		// sure it comes back rather than staying missing forever.
		rb->respawn_at = GameTime64 + RACE_BOX_RESPAWN_TIME;
	}

	if (broadcast && race_is_multi() && !is_observer())
	{
		multibuf[0] = MULTI_RACE_BOX;
		multibuf[1] = (ubyte)pnum;
		multibuf[2] = (ubyte)box;
		multi_send_data(multibuf, 3, 2);
	}
}

// Handles the local player flying into `powerup` if it is a mystery box.
// Returns 1 if it was consumed as one.
//
// Registration is not required up front: an energy orb sitting in a race level
// IS a mystery box, whether we have adopted it yet or not. Adopting it here
// removes every dependency on when the harvest sweep last ran, on object slots
// being recycled, and on a client's object array being replaced by a host
// sync -- all of which could otherwise leave an orb that just bounced the
// player off with nothing to show for it.
int race_box_hit(object *powerup)
{
	int box;

	if (!powerup || !(Game_mode & GM_RACE) || powerup->type != OBJ_POWERUP)
		return 0;

	box = race_box_index_from_object(powerup);

	if (box < 0 && powerup->id == POW_ENERGY && race_adopt_box(powerup - Objects))
		box = Race_num_boxes - 1;

	if (box < 0)
	{
		// Box table is full: still consume it rather than leaving an orb the
		// player cannot pick up.
		if (powerup->id != POW_ENERGY)
			return 0;

		race_box_roll();
		powerup->flags |= OF_SHOULD_BE_DEAD;

		return 1;
	}

	race_box_roll();
	race_box_taken(box, Player_num, 1);

	return 1;
}

void multi_do_race_box(const ubyte *buf)
{
	if (!(Game_mode & GM_RACE))
		return;

	// pnum (buf[1]) is informational for now; the roll happens on the picker's
	// machine. Applying is idempotent, so a duplicate from two players
	// grabbing the same box in the same frame is harmless.
	race_box_taken(buf[2], buf[1], 0);
}

//	-------------------------------------------------------------------------
//	Mystery box loot
//	-------------------------------------------------------------------------

// What a box can hand out. The standing missile table below is always in it;
// anything the level itself has lying around gets absorbed too, so a track
// built with vulcans and plasma on the floor still offers them -- just out of
// a box on a fuse instead of as a permanent pickup, and only to the class
// that races with that gun.
typedef struct race_item {
	ubyte		wclass;		// CLASS_PRIMARY / CLASS_SECONDARY
	ubyte		index;
	// Relative odds, at the front of the field and at the back of it. Every
	// draw interpolates between the two by the roller's place, so the leader
	// gets the light stuff and whoever is last gets the heavy stuff -- the
	// catch-up rubber band every kart racer runs on.
	ubyte		weight_front;
	ubyte		weight_back;
	const char	*name;
} race_item;

// The standing missile table is what a race is meant to feel like, so it stays
// common no matter how much hardware a level happens to have lying around.
// Without this, absorbing a level's guns quietly made megas and shakers rarer
// every time the pool grew.
#define RACE_WEIGHT_HARVESTED   1

#define RACE_MAX_ITEMS (MAX_PRIMARY_WEAPONS + MAX_SECONDARY_WEAPONS + RACE_NUM_POWERS)

static const char *const Race_primary_names[MAX_PRIMARY_WEAPONS] = {
	"LASER", "VULCAN", "SPREADFIRE", "PLASMA", "FUSION",
	"SUPER LASER", "GAUSS", "HELIX", "PHOENIX", "OMEGA"
};

static const char *const Race_secondary_names[MAX_SECONDARY_WEAPONS] = {
	"CONCUSSION", "HOMING MISSILE", "PROXIMITY BOMB", "SMART MISSILE", "MEGA MISSILE",
	"FLASH MISSILE", "GUIDED MISSILE", "SMART MINE", "MERCURY MISSILE", "EARTHSHAKER"
};

static const char *const Race_power_names[RACE_NUM_POWERS] = {
	"EMP", "TRACTOR BEAM"
};

static race_item Race_item_pool[RACE_MAX_ITEMS];
static int Race_num_items = 0;

// The powers currently on the local player, as the GameTime64 they end at.
// Both are set by somebody else's roll -- the roller is the one player they
// never touch.
static fix64 Race_emp_started = 0;
static fix64 Race_emp_until = 0;
static fix64 Race_tractor_started = 0;
static fix64 Race_tractor_until = 0;
static int   Race_tractor_by = -1;		// who is pulling us, for the on-screen callout

// GameTime64 at which each held weapon evaporates; 0 = not held from a box.
static fix64 Race_expire_pri[MAX_PRIMARY_WEAPONS];
static fix64 Race_expire_sec[MAX_SECONDARY_WEAPONS];

const char *race_item_name(int wclass, int index)
{
	if (wclass == CLASS_POWER)
		return (index >= 0 && index < RACE_NUM_POWERS) ? Race_power_names[index] : "POWER";

	if (wclass == CLASS_PRIMARY)
		return (index >= 0 && index < MAX_PRIMARY_WEAPONS) ? Race_primary_names[index] : "WEAPON";

	return (index >= 0 && index < MAX_SECONDARY_WEAPONS) ? Race_secondary_names[index] : "WEAPON";
}

static void race_add_item(int wclass, int index, int weight_front, int weight_back)
{
	int i;

	for (i = 0; i < Race_num_items; i++)
		if (Race_item_pool[i].wclass == wclass && Race_item_pool[i].index == index)
			return;		// already offered

	if (Race_num_items >= RACE_MAX_ITEMS)
		return;

	Race_item_pool[Race_num_items].wclass = (ubyte)wclass;
	Race_item_pool[Race_num_items].index = (ubyte)index;
	Race_item_pool[Race_num_items].weight_front = (ubyte)weight_front;
	Race_item_pool[Race_num_items].weight_back = (ubyte)weight_back;
	Race_item_pool[Race_num_items].name = race_item_name(wclass, index);
	Race_num_items++;
}

// How many racers are actually on the grid: the bot field's size outside a
// netgame (a bot field is a field -- the rubber band and the shaker's "is
// anyone ahead" both care about the grid, not about whether this happens to
// be a netgame), otherwise the connected player count minus an observing
// host. Shared by race_catchup_factor_for() and race_has_someone_ahead(),
// which both used to compute this the same way independently.
static int race_field_size(void)
{
	if (!(Game_mode & GM_MULTI))
		return race_bot_field_size();

	return N_players - (Netgame.host_is_obs ? 1 : 0);
}

// Where the local player is in the field, as 0 (leading) to F1_0 (last).
// Everything that rubber-bands reads this. A one-player race is always 0:
// there is nobody to catch.
static fix race_catchup_factor_for(int pnum)
{
	int rank, field = race_field_size();

	if (field < 2)
		return 0;

	rank = race_get_rank(pnum);

	if (rank < 1)
		return 0;

	if (rank > field)
		rank = field;

	return fixdiv(i2f(rank - 1), i2f(field - 1));
}

static fix race_catchup_factor(void)
{
	return race_catchup_factor_for(Player_num);
}

// This entry's odds for whoever is rolling, interpolated between its
// front-runner and back-marker weights.
static int race_item_weight(const race_item *item, fix catchup)
{
	int front = item->weight_front;
	int back = item->weight_back;

	return front + f2i(fixmul(i2f(back - front), catchup));
}

// Weighted draw from the loot table.
// True if a loose powerup is something a racer may pick up at all. Only ever
// says no to another class's primary -- see race_item_allowed(). Loose weapon
// powerups are rare in a race (the level's own are harvested into the box
// table at load, and a wreck drops nothing), so this is the backstop for the
// ones that get spawned some other way.
int race_powerup_allowed(int powerup_id)
{
	const race_class_info *ci = race_my_class();
	int wclass, index;

	if (!(Game_mode & GM_RACE) || !ci)
		return 1;

	if (!race_weapon_of_powerup(powerup_id, &wclass, &index))
		return 1;			// not a weapon: energy, shields, a key, ...

	return wclass != CLASS_PRIMARY || index == ci->primary;
}

// What a racer is allowed to take out of a box. A class is defined by the gun
// it races with, so the only primary a box will ever hand you is your own --
// rolling it tops up its ammo instead of handing you somebody else's kit.
// Secondaries stay open to everyone: missiles are the race's item game.
// True if there is anybody ahead of `pnum` for a shaker to fly at. A shaker is
// the catch-up weapon: it exists to go after whoever is running away with the
// race. Handed to the leader it has nowhere to go but back down the field, or
// at nobody at all, so it simply is not offered to them.
static int race_has_someone_ahead(int pnum)
{
	if (race_field_size() < 2)
		return 0;

	return race_get_rank(pnum) > 1;
}

static int race_item_allowed_for(const race_item *item, const race_class_info *ci, int pnum)
{
	if (item->wclass == CLASS_SECONDARY && item->index == SMISSILE5_INDEX)
		if (!race_has_someone_ahead(pnum))
			return 0;

	if (!ci || item->wclass != CLASS_PRIMARY)
		return 1;

	return item->index == ci->primary;
}

static const race_item *race_pick_item_for(int pnum)
{
	const race_class_info *ci = race_get_class_info(race_get_class(pnum));
	const race_item *last = NULL;
	fix catchup = race_catchup_factor_for(pnum);
	int total = 0, roll, i;

	// The "powerup chance" advanced option: below 100%, a roll can come up
	// empty entirely -- one gate here covers both race_box_roll() (the local
	// player) and race_roll_box_item() (the bot field), since both route
	// through this function.
	if (Race_powerup_chance < 100 && (d_rand() % 100) >= (unsigned)Race_powerup_chance)
		return NULL;

	// Weighted over what this racer can actually carry, not over the whole
	// table -- rolling and then rejecting would quietly bias the draw towards
	// whatever came first.
	for (i = 0; i < Race_num_items; i++)
		if (race_item_allowed_for(&Race_item_pool[i], ci, pnum))
			total += race_item_weight(&Race_item_pool[i], catchup);

	if (total <= 0)
		return NULL;

	roll = d_rand() % total;

	for (i = 0; i < Race_num_items; i++)
	{
		if (!race_item_allowed_for(&Race_item_pool[i], ci, pnum))
			continue;

		last = &Race_item_pool[i];
		roll -= race_item_weight(&Race_item_pool[i], catchup);

		if (roll < 0)
			return last;
	}

	return last;
}

static const race_item *race_pick_item(void)
{
	return race_pick_item_for(Player_num);
}

// One draw off the same loot table the local player rolls from, for a racer
// who has no weapon rack to put it in. Returns 0 if the table has nothing to
// offer them.
int race_roll_box_item(int pnum, int *wclass, int *index)
{
	const race_item *item;

	if (!Race_num_items)
		race_init_items();

	item = race_pick_item_for(pnum);

	if (!item)
		return 0;

	*wclass = item->wclass;
	*index = item->index;

	return 1;
}

// The eight missiles every race offers regardless of what the level holds.
static void race_init_items(void)
{
	// The standing missile table, front-runner weight then back-marker weight.
	// Concussions, proximity bombs and smart mines are deliberately absent:
	// the first is filler, and the two mines belong to the Trapper's kit --
	// handing them to everyone out of a box makes that class's whole identity
	// common property.
	//
	// The weights are what makes the rubber band: out front you mostly draw
	// homing missiles, at the back you mostly draw shakers and megas.
	// The earthshaker homes in on the leader from anywhere on the track and
	// blanks half the mine when it's the EMP drawn instead of tractor, so
	// both are kept well down the odds -- the shaker rarest of all, since it
	// is the one item that can end a good lap from off-screen.
	static const struct { ubyte index; ubyte front, back; ubyte slot; } base[] = {
		{ HOMING_INDEX,    12,  4, RACE_ITEM_HOMING },
		{ SMART_INDEX,      8,  7, RACE_ITEM_SMART },
		{ SMISSILE4_INDEX,  5,  9, RACE_ITEM_MERCURY },	// mercury
		{ MEGA_INDEX,       3, 11, RACE_ITEM_MEGA },
		{ SMISSILE5_INDEX,  1,  4, RACE_ITEM_EARTHSHAKER },	// earthshaker -- the rarest draw in the box
	};
	int i;

	Race_num_items = 0;
	memset(Race_expire_pri, 0, sizeof(Race_expire_pri));
	memset(Race_expire_sec, 0, sizeof(Race_expire_sec));

	for (i = 0; i < (int)(sizeof(base)/sizeof(base[0])); i++)
		if (Race_allowed_items & (1 << base[i].slot))
			race_add_item(CLASS_SECONDARY, base[i].index, base[i].front, base[i].back);

	// The two powers ride the same rubber band as the missiles: rare out
	// front, more common at the back. The tractor is unavailable to front-
	// runners (weight 0) and scales up with lap count so longer races see
	// more of it -- a 3-lap race keeps it very rare, a 10-lap race hands it
	// out more freely at the back.
	if (Race_allowed_items & (1 << RACE_ITEM_EMP))
		race_add_item(CLASS_POWER, RACE_POWER_EMP, 2, 6);
	if (Race_allowed_items & (1 << RACE_ITEM_TRACTOR))
		race_add_item(CLASS_POWER, RACE_POWER_TRACTOR, 0, max(1, Race_laps_to_win / 2));
}

// Maps a powerup type to the weapon it stands for. Returns 0 if that powerup
// isn't a weapon (shields, keys, cloak, ...), which race mode leaves alone.
static int race_weapon_of_powerup(int powerup_id, int *wclass, int *index)
{
	switch (powerup_id)
	{
		case POW_LASER:				*wclass = CLASS_PRIMARY;   *index = LASER_INDEX;       return 1;
		case POW_SUPER_LASER:		*wclass = CLASS_PRIMARY;   *index = SUPER_LASER_INDEX; return 1;
		case POW_VULCAN_WEAPON:
		case POW_VULCAN_AMMO:		*wclass = CLASS_PRIMARY;   *index = VULCAN_INDEX;      return 1;
		case POW_SPREADFIRE_WEAPON:	*wclass = CLASS_PRIMARY;   *index = SPREADFIRE_INDEX;  return 1;
		case POW_PLASMA_WEAPON:		*wclass = CLASS_PRIMARY;   *index = PLASMA_INDEX;      return 1;
		case POW_FUSION_WEAPON:		*wclass = CLASS_PRIMARY;   *index = FUSION_INDEX;      return 1;
		case POW_GAUSS_WEAPON:		*wclass = CLASS_PRIMARY;   *index = GAUSS_INDEX;       return 1;
		case POW_HELIX_WEAPON:		*wclass = CLASS_PRIMARY;   *index = HELIX_INDEX;       return 1;
		case POW_PHOENIX_WEAPON:	*wclass = CLASS_PRIMARY;   *index = PHOENIX_INDEX;     return 1;
		case POW_OMEGA_WEAPON:		*wclass = CLASS_PRIMARY;   *index = OMEGA_INDEX;       return 1;

		case POW_MISSILE_1:
		case POW_MISSILE_4:			*wclass = CLASS_SECONDARY; *index = CONCUSSION_INDEX;  return 1;
		case POW_HOMING_AMMO_1:
		case POW_HOMING_AMMO_4:		*wclass = CLASS_SECONDARY; *index = HOMING_INDEX;      return 1;
		case POW_PROXIMITY_WEAPON:	*wclass = CLASS_SECONDARY; *index = PROXIMITY_INDEX;   return 1;
		case POW_SMARTBOMB_WEAPON:	*wclass = CLASS_SECONDARY; *index = SMART_INDEX;       return 1;
		case POW_MEGA_WEAPON:		*wclass = CLASS_SECONDARY; *index = MEGA_INDEX;        return 1;
		case POW_SMISSILE1_1:
		case POW_SMISSILE1_4:		*wclass = CLASS_SECONDARY; *index = SMISSILE1_INDEX;   return 1;
		case POW_GUIDED_MISSILE_1:
		case POW_GUIDED_MISSILE_4:	*wclass = CLASS_SECONDARY; *index = GUIDED_INDEX;      return 1;
		case POW_SMART_MINE:		*wclass = CLASS_SECONDARY; *index = SMART_MINE_INDEX;  return 1;
		case POW_MERCURY_MISSILE_1:
		case POW_MERCURY_MISSILE_4:	*wclass = CLASS_SECONDARY; *index = SMISSILE4_INDEX;   return 1;
		case POW_EARTHSHAKER_MISSILE:*wclass = CLASS_SECONDARY; *index = SMISSILE5_INDEX;   return 1;

		default:
			return 0;
	}
}

// Sweeps weapon powerups out of the mine and folds their types into the loot
// table. Run every frame rather than once at load: on a client the object
// array is replaced when it syncs with the host, and powerups can be created
// mid-game, so a one-shot pass at level start would miss them.
static void race_harvest_level_weapons(void)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		int wclass, index;

		if (Objects[i].type != OBJ_POWERUP)
			continue;
		if (Objects[i].flags & OF_SHOULD_BE_DEAD)
			continue;
		if (race_box_index_from_object(&Objects[i]) >= 0)
			continue;		// one of our own mystery box orbs

		// An energy orb the level placed becomes a mystery box in its own
		// right rather than a pickup nobody can use.
		if (Objects[i].id == POW_ENERGY)
		{
			race_adopt_box(i);
			continue;
		}

		if (!race_weapon_of_powerup(Objects[i].id, &wclass, &index))
			continue;

		race_add_item(wclass, index, RACE_WEIGHT_HARVESTED, RACE_WEIGHT_HARVESTED);
		Objects[i].flags |= OF_SHOULD_BE_DEAD;
	}
}

// Which powerup bitmap stands for this weapon on the HUD.
int race_item_powerup(int wclass, int index)
{
	if (wclass == CLASS_POWER)
		return (index == RACE_POWER_TRACTOR) ? POW_SHIELD_BOOST : POW_MISSILE_1;

	if (wclass == CLASS_SECONDARY)
		return (index >= 0 && index < MAX_SECONDARY_WEAPONS)
			? Secondary_weapon_to_powerup[index] : POW_ENERGY;

	if (index == SUPER_LASER_INDEX)
		return POW_SUPER_LASER;		// the table maps this back to a plain laser

	return (index >= 0 && index < MAX_PRIMARY_WEAPONS)
		? Primary_weapon_to_powerup[index] : POW_ENERGY;
}

fix64 race_get_item_remaining(int wclass, int index)
{
	fix64 expire;

	if (wclass == CLASS_POWER)
		return 0;			// fires on the roll; never sits in the rack

	if (wclass == CLASS_PRIMARY)
	{
		if (index < 0 || index >= MAX_PRIMARY_WEAPONS)
			return 0;
		expire = Race_expire_pri[index];
	}
	else
	{
		if (index < 0 || index >= MAX_SECONDARY_WEAPONS)
			return 0;
		expire = Race_expire_sec[index];
	}

	if (!expire || GameTime64 >= expire)
		return 0;

	return expire - GameTime64;
}

// How much ammo the player is carrying for a held weapon, for the HUD.
int race_get_item_ammo(int wclass, int index)
{
	if (wclass == CLASS_POWER)
		return 1;

	if (wclass == CLASS_SECONDARY)
		return Players[Player_num].secondary_ammo[index];

	// Vulcan/gauss rounds are stored scaled, the same way the cockpit gauge
	// reads them out.
	if (index == VULCAN_INDEX || index == GAUSS_INDEX)
		return f2i((unsigned)VULCAN_AMMO_SCALE * (unsigned)Players[Player_num].primary_ammo[VULCAN_INDEX]);

	return 1;	// energy weapons have no ammo count to show
}

//	-------------------------------------------------------------------------
//	Box powers
//	-------------------------------------------------------------------------
//
// A power is the one thing a box hands out that never enters the rack: it
// goes off the moment it is rolled, and it goes off on everybody else. The
// roller is immune, which is the whole point of picking one up -- an EMP
// that jammed its own thrower would just be a mine you stood on.
//
// Both are applied on each machine to its own player. The roller broadcasts
// one packet; every other machine starts its own timer off it. Nothing about
// them is scored, so a dropped packet costs one player five seconds of
// inconvenience rather than desyncing the race.

// Shared shape for both powers: fades in over the first fifth of the effect
// and back out over the last third, so an EMP blooms and clears instead of
// snapping into the glitch and snapping off. Both windows are a fraction of
// `duration` -- the effect's OWN length -- rather than a single shared
// constant: the EMP and the tractor no longer run the same length (see
// RACE_POWER_EMP_TIME), and a fixed fade window sized for the shorter one
// was a barely-there flicker at the end of the longer one.
static fix race_power_strength(fix64 started, fix64 until, fix64 duration)
{
	fix64 remaining, elapsed;
	fix strength = F1_0;

	if (!until || GameTime64 >= until)
		return 0;

	remaining = until - GameTime64;
	elapsed = GameTime64 - started;

	if (remaining < duration/3)
		strength = fixdiv((fix)remaining, (fix)(duration/3));
	if (elapsed < duration/5)
	{
		fix ramp = fixdiv((fix)elapsed, (fix)(duration/5));

		if (ramp < strength)
			strength = ramp;
	}

	return strength;
}

fix race_emp_strength(void)
{
	if (!(Game_mode & GM_RACE))
		return 0;

	return race_power_strength(Race_emp_started, Race_emp_until, RACE_POWER_EMP_TIME);
}

// Above this fraction of full strength, the EMP is strong enough to blank
// the gauges. race_emp_strength() ramps smoothly (bloom in, hold, fade
// out -- see race_power_strength()), so thresholding it gives exactly ONE
// transition each way per EMP: the gauges drop out once as strength climbs
// through this, and come back once as it falls back through it on the way
// out. Deliberately not a repeating flicker -- rapid on/off strobing is a
// real photosensitive-seizure risk (WCAG flags anything flashing more than
// ~3 times a second), not just a matter of taste, so this never toggles
// more than twice in an EMP's whole ~9-second run.
#define RACE_EMP_GAUGE_HIDE_THRESHOLD (F1_0*3/4)

int race_emp_gauge_hidden(void)
{
	return race_emp_strength() > RACE_EMP_GAUGE_HIDE_THRESHOLD;
}

fix race_tractor_strength(void)
{
	if (!(Game_mode & GM_RACE))
		return 0;

	return race_power_strength(Race_tractor_started, Race_tractor_until, RACE_POWER_TIME);
}

// Thrust multiplier from the tractor beam. Full strength for the whole effect
// rather than ramped with the tint: the fade is there so the screen doesn't
// snap, but a slow that eased itself off would be far harder to read than one
// that simply ends.
fix race_power_speed_scale(void)
{
	if (!(Game_mode & GM_RACE) || !Race_tractor_until || GameTime64 >= Race_tractor_until)
		return F1_0;

	return RACE_TRACTOR_SCALE;
}

// The callsign of whoever is pulling us right now, or NULL if nobody is. For
// the center-screen callout -- race_tractor_strength() says how strong the
// effect is, this says who to blame for it.
const char *race_tractor_puller(void)
{
	if (!(Game_mode & GM_RACE) || !Race_tractor_until || GameTime64 >= Race_tractor_until)
		return NULL;

	if (Race_tractor_by < 0 || Race_tractor_by >= MAX_PLAYERS)
		return NULL;

	return Players[Race_tractor_by].callsign;
}

// Starts or extends a power effect timed as (started, until) and read
// through race_power_strength(). The bug this exists to make impossible: a
// second hit landing while the first is still running used to overwrite
// `started` unconditionally, which reset race_power_strength()'s elapsed-
// since-started ramp-IN -- so on a track with several bots each able to roll
// an EMP or a tractor, a second hit read as the glitch visibly settling down
// for a moment (the ramp restarting) before flaring back up to full
// strength, not as "still jammed." With seven bots cycling through mystery
// boxes, a 9-second EMP had plenty of time to take a second hit before it
// ran out, which is what made it feel like it kept cutting out instead of
// just running.
//
// `started` only moves when the effect wasn't already running; a hit that
// lands mid-effect just pushes `until` back out to a fresh full window,
// leaving the in-progress strength exactly where it was.
static void race_power_apply(fix64 *started, fix64 *until, fix64 duration)
{
	if (!*until || GameTime64 >= *until)
		*started = GameTime64;

	*until = GameTime64 + duration;
}

// Releases a power effect early -- the roller's own pickup lifting whatever
// was already on them (see race_power_trigger() below). Eases out over
// RACE_POWER_RELEASE_TIME through race_power_strength()'s own fade-out math
// rather than zeroing `until` outright, which would skip the fade and cut
// the effect instead of clearing it. A no-op if the effect isn't actually
// running, so it can't flash an already-expired timer back to life.
static void race_power_release(fix64 *until)
{
	if (*until && GameTime64 < *until)
		*until = GameTime64 + RACE_POWER_RELEASE_TIME;
}

void race_power_trigger(int power, int pnum, int broadcast)
{
	if (!(Game_mode & GM_RACE) || power < 0 || power >= RACE_NUM_POWERS)
		return;

	// The roller is the one racer a power never lands on. Said once, here,
	// rather than left to fall out of the branches below: a power that caught
	// its own thrower would be a box that punishes you for opening it. The
	// clear is deliberate -- if one was already on us when we rolled ours, our
	// own roll lifts it.
	if (pnum == Player_num)
	{
		race_power_release(&Race_emp_until);
		race_power_release(&Race_tractor_until);
		Race_tractor_by = -1;

		if (!is_observer())
			HUD_init_message(HM_DEFAULT, "%s -- FIELD %s", race_item_name(CLASS_POWER, power),
							 (power == RACE_POWER_EMP) ? "JAMMED" : "SLOWED");
	}
	else if (!is_observer())
	{
		const char *who = (pnum >= 0 && pnum < MAX_PLAYERS) ? Players[pnum].callsign : "SOMEONE";

		if (power == RACE_POWER_EMP)
		{
			race_power_apply(&Race_emp_started, &Race_emp_until, RACE_POWER_EMP_TIME);
			HUD_init_message(HM_DEFAULT, "%s SET OFF AN EMP", who);
			digi_play_sample(SOUND_BADASS_EXPLOSION, F1_0);
		}
		else
		{
			race_power_apply(&Race_tractor_started, &Race_tractor_until, RACE_POWER_TIME);
			Race_tractor_by = pnum;
			// Kept under 38 characters even with a full 8-char callsign:
			// the race HUD (lap counter, minimap, timer, everything
			// race_draw_hud() draws) bails out whenever the top HUD
			// message is longer than that (HUD_render_message_frame(),
			// hud.c) -- a stock guard meant to keep a long message clear
			// of the cockpit's own weapon icons, not something race mode
			// asked for. The old, longer wording tripped it on every
			// tractor hit, which is what "the whole HUD blinks out" was:
			// not a race mode bug, a message a few characters too long.
			HUD_init_message(HM_DEFAULT, "%s TRACTOR BEAM -- 50%% SPEED", who);
			digi_play_sample(SOUND_CLOAK_OFF, F1_0);
		}
	}

	// The bot field takes it too -- everyone but whoever rolled it.
	race_bots_take_power(power, pnum);

	if (broadcast && race_is_multi() && !is_observer())
	{
		multibuf[0] = MULTI_RACE_POWER;
		multibuf[1] = (ubyte)pnum;
		multibuf[2] = (ubyte)power;
		multi_send_data(multibuf, 3, 2);
	}
}

void multi_do_race_power(const ubyte *buf)
{
	if (!(Game_mode & GM_RACE))
		return;

	race_power_trigger(buf[2], buf[1], 0);
}

// Drop a weapon out of the rack, and move the player off it if it was the one
// they had selected.
static void race_drop_item(int wclass, int index)
{
	player *p = &Players[Player_num];

	if (wclass == CLASS_SECONDARY)
	{
		Race_expire_sec[index] = 0;

		// The kit is not loot: a Trapper's mines stay when a box copy of the
		// same rack slot expires.
		if (race_is_class_weapon(CLASS_SECONDARY, index))
			return;

		p->secondary_ammo[index] = 0;
		p->secondary_weapon_flags &= ~HAS_FLAG(index);

		if (p->secondary_weapon == index)
			auto_select_weapon(1);

		return;
	}

	Race_expire_pri[index] = 0;

	// The class kit is not loot: a Glass Cannon who rolls a Vulcan out of a
	// box keeps their Vulcan when the box copy's fuse runs out.
	if (race_is_class_weapon(CLASS_PRIMARY, index))
		return;

	p->primary_weapon_flags &= ~HAS_FLAG(index);

	if (index == LASER_INDEX || index == SUPER_LASER_INDEX)
		p->laser_level = 0;
	if (index == VULCAN_INDEX || index == GAUSS_INDEX)
		p->primary_ammo[VULCAN_INDEX] = 0;

	if (p->primary_weapon == index)
		auto_select_weapon(0);
}

static void race_grant_item(int wclass, int index)
{
	player *p = &Players[Player_num];
	const race_class_info *ci = race_my_class();

	// A power never reaches the rack: rolling it is firing it.
	if (wclass == CLASS_POWER)
	{
		race_power_trigger(index, Player_num, 1);
		return;
	}

	// The engine's own super split in both racks: primaries from the super
	// laser up, secondaries from the flash missile up.
	fix64 life = (index >= (wclass == CLASS_PRIMARY ? SUPER_LASER_INDEX : SUPER_WEAPON))
		? RACE_ITEM_SUPER_TIME : RACE_ITEM_NORMAL_TIME;

	// A racer carries their class's gun and no other, whatever a box or a
	// stray powerup tries to hand them.
	if (ci && wclass == CLASS_PRIMARY && index != ci->primary)
		return;

	// A class can run a longer fuse on everything it picks up.
	if (ci && ci->box_item_time && ci->box_item_time != F1_0)
		life = fixmul64((fix)life, ci->box_item_time);

	if (wclass == CLASS_SECONDARY)
	{
		if (p->secondary_ammo[index] < Secondary_ammo_max[index])
			p->secondary_ammo[index]++;

		p->secondary_weapon_flags |= HAS_FLAG(index);

		// A fresh pickup restarts the fuse rather than stacking onto it, so
		// camping boxes can't bank an indefinite arsenal -- unless this is
		// the player's own kit, which never had a fuse: a box copy of it just
		// tops the rack up.
		if (!race_is_class_weapon(CLASS_SECONDARY, index))
			Race_expire_sec[index] = GameTime64 + life;

		return;
	}

	p->primary_weapon_flags |= HAS_FLAG(index);

	switch (index)
	{
		case LASER_INDEX:
			if (p->laser_level < MAX_LASER_LEVEL)
				p->laser_level++;
			break;
		case SUPER_LASER_INDEX:
			// Super laser is the laser at levels 4-5, so it needs both flags.
			p->primary_weapon_flags |= HAS_LASER_FLAG;
			p->laser_level = MAX_SUPER_LASER_LEVEL;
			break;
		case VULCAN_INDEX:
		case GAUSS_INDEX:
			p->primary_ammo[VULCAN_INDEX] += VULCAN_AMMO_AMOUNT;
			if (p->primary_ammo[VULCAN_INDEX] > Primary_ammo_max[VULCAN_INDEX])
				p->primary_ammo[VULCAN_INDEX] = Primary_ammo_max[VULCAN_INDEX];
			break;
		default:
			break;
	}

	if (!race_is_class_weapon(CLASS_PRIMARY, index))
		Race_expire_pri[index] = GameTime64 + life;
}

void race_box_roll(void)
{
	int rolls = 1;
	int roll = d_rand() % 100;
	int first_class = -1, first_index = -1;
	int had_pri = (Players[Player_num].primary_weapon_flags != 0);
	int had_sec = (Players[Player_num].secondary_weapon_flags != 0);
	int i;

	// Should never be empty -- race_init_level() seeds the standing missile
	// table -- but a roll that silently hands out nothing is invisible to the
	// player and maddening to diagnose, so rebuild rather than return.
	if (!Race_num_items)
		race_init_items();

	// One item is the common case out front; at the back of the field two and
	// three become as likely as one. Same rubber band as the loot weights --
	// see race_catchup_factor().
	{
		fix catchup = race_catchup_factor();
		int two = 60 - f2i(fixmul(i2f(25), catchup));	// 60% -> 35% chance of just one
		int three = 90 - f2i(fixmul(i2f(25), catchup));	// 10% -> 35% chance of three

		if (roll >= two)
			rolls = (roll >= three) ? 3 : 2;
	}

	{
		const race_class_info *ci = race_my_class();

		if (ci)
			rolls += ci->box_extra_rolls;
	}

	for (i = 0; i < rolls; i++)
	{
		const race_item *item = race_pick_item();
		fix64 life;

		if (!item)
			break;

		race_grant_item(item->wclass, item->index);
		life = race_get_item_remaining(item->wclass, item->index);

		// A power has already announced itself (and has no rack slot to arm).
		if (item->wclass == CLASS_POWER)
			continue;

		if (first_index < 0)
		{
			first_class = item->wclass;
			first_index = item->index;
		}

		if (life)
			HUD_init_message(HM_DEFAULT, "%s (%d sec)", item->name, f2i((fix)life));
		else
			HUD_init_message(HM_DEFAULT, "%s", item->name);
	}

	digi_play_sample(SOUND_GOOD_SELECTION_SECONDARY, F1_0);

	// Arm the first thing out of the box if that rack was empty, so a pickup
	// is immediately usable instead of needing a weapon-cycle. Never yanks a
	// selection the player already made.
	if (first_index >= 0 && !(first_class == CLASS_PRIMARY ? had_pri : had_sec))
		select_weapon(first_index, first_class, 0, 0);
}

static void race_items_frame(void)
{
	int i;

	if (is_observer())
		return;

	for (i = 0; i < MAX_SECONDARY_WEAPONS; i++)
	{
		if (!Race_expire_sec[i])
			continue;

		// Firing the last one ends it early; otherwise the fuse does.
		if (!Players[Player_num].secondary_ammo[i])
		{
			race_drop_item(CLASS_SECONDARY, i);
			continue;
		}

		if (GameTime64 >= Race_expire_sec[i])
		{
			HUD_init_message(HM_DEFAULT, "%s expired", race_item_name(CLASS_SECONDARY, i));
			race_drop_item(CLASS_SECONDARY, i);
		}
	}

	for (i = 0; i < MAX_PRIMARY_WEAPONS; i++)
	{
		if (!Race_expire_pri[i])
			continue;

		if (GameTime64 >= Race_expire_pri[i])
		{
			HUD_init_message(HM_DEFAULT, "%s expired", race_item_name(CLASS_PRIMARY, i));
			race_drop_item(CLASS_PRIMARY, i);
		}
	}
}

void race_strip_loadout(int pnum)
{
	int i;

	Players[pnum].laser_level = 0;
	Players[pnum].primary_weapon_flags = 0;
	Players[pnum].secondary_weapon_flags = 0;

	for (i = 0; i < MAX_PRIMARY_WEAPONS; i++)
		Players[pnum].primary_ammo[i] = 0;
	for (i = 0; i < MAX_SECONDARY_WEAPONS; i++)
		Players[pnum].secondary_ammo[i] = 0;

	if (pnum == Player_num)
	{
		memset(Race_expire_pri, 0, sizeof(Race_expire_pri));
		memset(Race_expire_sec, 0, sizeof(Race_expire_sec));
	}
}

static void race_boxes_frame(void)
{
	int i;

	for (i = 0; i < Race_num_boxes; i++)
	{
		race_box *rb = &Race_box[i];

		// The orb can disappear behind our back (object array sync with the
		// host, level reset, ...), so re-check it rather than trusting the
		// stored objnum forever.
		if (rb->objnum >= 0)
		{
			const object *obj = &Objects[rb->objnum];

			if (obj->type != OBJ_POWERUP || obj->signature != rb->signature ||
				(obj->flags & OF_SHOULD_BE_DEAD))
			{
				rb->objnum = -1;
				rb->signature = 0;
				rb->respawn_at = GameTime64 + RACE_BOX_RESPAWN_TIME;
			}
			continue;
		}

		if (rb->respawn_at && GameTime64 >= rb->respawn_at)
			race_spawn_box(i);
	}
}

//	-------------------------------------------------------------------------
//	Boost pads
//	-------------------------------------------------------------------------

// Boost pads fire once per segment entry, never continuously: sitting inside a
// pad must not hold the boost open forever, and neither must a pad you are
// slowly drifting through. Entering a *different* pad does refresh the timer,
// but the boost still only ever runs RACE_BOOST_TIME from that moment.
void race_check_boost_pad(segment *segp)
{
	int segnum;

	if (!segp || Race_counting_down)
		return;

	segnum = segp - Segments;

	if (segnum == Race_last_boost_seg)
		return;		// still in (or back in) the pad we already triggered

	Race_last_boost_seg = segnum;

	if (Segment2s[segnum].special != SEGMENT_IS_FUELCEN)
		return;

	if (GameTime64 >= Race_boost_until)
		Race_boost_started = GameTime64;	// fresh boost, so ramp it in

	{
		const race_class_info *ci = race_my_class();
		fix64 time = RACE_BOOST_TIME;

		// A class can hold a pad's push for longer -- the one thing in the
		// mode that rewards knowing where the pads are.
		if (ci && ci->boost_time && ci->boost_time != F1_0)
			time = fixmul64((fix)time, ci->boost_time);

		Race_boost_until = GameTime64 + time;
	}
	digi_play_sample(SOUND_AFTERBURNER_IGNITE, F1_0);
}

// 0 when not boosting, F1_0 at full boost, ramping in over the first eighth of
// a second and out over the last half second so the speed and FOV changes
// don't snap.
static fix race_boost_strength(void)
{
	fix64 remaining;
	fix64 elapsed;
	fix strength = F1_0;

	if (GameTime64 >= Race_boost_until)
		return 0;

	remaining = Race_boost_until - GameTime64;
	elapsed = GameTime64 - Race_boost_started;

	if (remaining < F1_0/2)
		strength = (fix)(remaining * 2);
	if (elapsed < F1_0/8 && (fix)(elapsed * 8) < strength)
		strength = (fix)(elapsed * 8);

	return strength;
}

fix race_get_boost_scale(void)
{
	const race_class_info *ci = race_my_class();
	// Up to 2.25x forward thrust, a bit stronger than the afterburner's 2x.
	fix push = F1_0 + F1_0/4;

	if (ci && ci->boost_power && ci->boost_power != F1_0)
		push = fixmul(push, ci->boost_power);

	return F1_0 + fixmul(race_boost_strength(), push);
}

void race_cancel_boost(void)
{
	if (GameTime64 < Race_boost_until)
	{
		Race_boost_until = 0;
		Race_boost_started = 0;
	}
}

// Local player's trichord state, refreshed once a frame by
// race_note_trichord() (controls.c). Eased rather than snapped straight to
// the input: a real diagonal push clips in and out of the floor for a frame
// at a time even when it's genuinely being held, and the charge meter below
// stays exactly that responsive on purpose (it's what pays the speed bonus)
// -- it's the FOV chasing every one of those blips that read as a flicker
// instead of a held effect.
static fix Race_trichord_strength = 0;

// Charge bar fill (0..F1_0) -- resets to 0 when it fires a burst.
static fix Race_trichord_charge = 0;

// GameTime64 until the local player's active trichord burst expires.
static fix64 Race_trichord_boost_until = 0;

// How many HUD blobs were lit last frame, for the per-threshold ding.
static int Race_trichord_blobs_lit = 0;

// 0..F1_0 build target from `ratio`: zero below the floor, ramping to F1_0.
static fix race_trichord_target(fix ratio)
{
	fix target;

	if (ratio <= RACE_TRICHORD_FLOOR)
		return 0;

	target = fixdiv(ratio - RACE_TRICHORD_FLOOR, F1_0 - RACE_TRICHORD_FLOOR);

	if (target > F1_0)
		target = F1_0;

	return target;
}

// Advances one entity's own charge meter by a frame, off this frame's
// trichord ratio -- fills toward F1_0 at a rate scaled by how good the
// diagonal is (so a marginal one crawls and a perfect one takes
// RACE_TRICHORD_CHARGE_TIME), and empties at a flat, faster rate the instant
// it drops off, rather than easing back down to meet a lower target, so
// letting go costs the built-up speed right away instead of bleeding off
// slowly. Shared by the local player (race_note_trichord() below, off real
// stick balance) and the bot field (racebot.c, each bot keeping its own
// charge in race_bot::trichord_charge, off how hard it's leaning into its
// current crab) -- one mechanic, fed two different ratios, since a bot never
// adds the vertical axis a human stick can (see the crab comments in
// racebot.c for why).
fix race_trichord_advance_charge(fix charge, fix ratio, fix64 *boost_until)
{
	fix target;

	// No charging during an active boost -- prevents re-firing before the burst expires.
	if (GameTime64 < *boost_until)
		return 0;

	target = race_trichord_target(ratio);

	if (target > 0)
	{
		charge += fixmul(FrameTime, fixdiv(target, RACE_TRICHORD_CHARGE_TIME));
		if (charge >= F1_0)
		{
			*boost_until = GameTime64 + RACE_TRICHORD_BOOST_TIME;
			return 0;	// bar resets after firing
		}
	}
	else
	{
		return 0;	// angle lost -- wipe charge instantly
	}

	return charge;
}

// Thrust multiplier from the boost timer.
fix race_trichord_scale_from_boost(fix64 boost_until)
{
	if (GameTime64 < boost_until)
		return F1_0 + RACE_TRICHORD_CHARGE_BONUS;
	return F1_0;
}

void race_note_trichord(fix ratio)
{
	fix target = race_trichord_target(ratio);
	fix step = fixmul(FrameTime, RACE_TRICHORD_FOV_EASE);
	fix64 old_boost = Race_trichord_boost_until;
	int lit;

	if (target > Race_trichord_strength)
	{
		Race_trichord_strength += step;
		if (Race_trichord_strength > target)
			Race_trichord_strength = target;
	}
	else if (target < Race_trichord_strength)
	{
		Race_trichord_strength -= step;
		if (Race_trichord_strength < target)
			Race_trichord_strength = target;
	}

	Race_trichord_charge = race_trichord_advance_charge(Race_trichord_charge, ratio, &Race_trichord_boost_until);

	if (Race_trichord_boost_until != old_boost)
	{
		digi_play_sample(SOUND_AFTERBURNER_IGNITE, F1_0);
		Race_trichord_blobs_lit = 0;
		return;
	}

	lit = f2i(fixmul(Race_trichord_charge, i2f(RACE_TRICHORD_BLOBS)));
	if (lit > RACE_TRICHORD_BLOBS)
		lit = RACE_TRICHORD_BLOBS;

	if (lit > Race_trichord_blobs_lit)
	{
		// Rising pitch per blob: unison, maj2, maj3, p4, p5, octave
		static const fix blob_speed[RACE_TRICHORD_BLOBS] = {
			F1_0, F1_0*9/8, F1_0*5/4, F1_0*4/3, F1_0*3/2, F1_0*2
		};
		int idx = lit - 1;
		if (idx < 0) idx = 0;
		if (idx >= RACE_TRICHORD_BLOBS) idx = RACE_TRICHORD_BLOBS - 1;
		digi_play_sample_pitched(118, F1_0, blob_speed[idx]);
	}

	Race_trichord_blobs_lit = lit;
}

fix race_trichord_strength(void)
{
	return Race_trichord_strength;
}

fix race_trichord_charge(void)
{
	if (GameTime64 < Race_trichord_boost_until)
	{
		// Bar counts down during the burst so it's visually distinct from charging.
		fix64 rem = Race_trichord_boost_until - GameTime64;
		if (rem >= (fix64)RACE_TRICHORD_BOOST_TIME)
			return F1_0;
		return fixdiv((fix)rem, RACE_TRICHORD_BOOST_TIME);
	}
	return Race_trichord_charge;
}

fix race_trichord_charge_scale(void)
{
	return race_trichord_scale_from_boost(Race_trichord_boost_until);
}

fix race_get_fov_bonus(void)
{
	fix bonus;

	// Render_zoom is 0x9000 by default and 0x11000 at the "wide" end of the
	// FOV slider, so 0x2000 is a noticeable but not disorienting widening.
	bonus = fixmul(race_boost_strength(), 0x2000);

	// Getting pulled narrows it instead -- the same shape as the widening,
	// just the other way, so the screen closing in reads as a squeeze rather
	// than a snap, and opens back up the moment the beam lets go.
	bonus -= fixmul(race_tractor_strength(), 0x1800);

	// A sustained trichord widens it; the active boost holds it fully wide
	// and adds a second kick that counts down with the bar.
	{
		fix tf = Race_trichord_strength;
		if (GameTime64 < Race_trichord_boost_until && tf < F1_0)
			tf = F1_0;
		bonus += fixmul(tf, RACE_TRICHORD_FOV);
		if (GameTime64 < Race_trichord_boost_until)
			bonus += fixmul(race_trichord_charge(), RACE_TRICHORD_FOV);
	}

	return bonus;
}

//	-------------------------------------------------------------------------
//	Floating track labels
//	-------------------------------------------------------------------------

static race_label Race_label[RACE_MAX_LABELS];
static int Race_num_labels = 0;
// Which label (if any) owns each segment, so the per-frame visibility check is
// one pass over the renderer's segment list rather than a search per label.
static sbyte Race_label_of_seg[MAX_SEGMENTS];

static int race_label_kind_of_segment(int segnum)
{
	if (race_segment_is_finish(segnum))
		return RACE_LABEL_FINISH;

	switch (Segment2s[segnum].special)
	{
		case SEGMENT_IS_ROBOTMAKER:
			return RACE_LABEL_CHECKPOINT;
		case SEGMENT_IS_FUELCEN:
			return RACE_LABEL_BOOST;
		default:
			return -1;
	}
}

static void race_init_labels(void)
{
	int i;

	Race_num_labels = 0;
	memset(Race_label_of_seg, -1, sizeof(Race_label_of_seg));

	for (i = 0; i <= Highest_segment_index; i++)
	{
		int queue[RACE_MAX_LABELS * 4];
		int head = 0, tail = 0, kind, number, n;
		fix64 sx = 0, sy = 0, sz = 0;	// 64-bit: summing a group of world-space centres overflows fix
		race_label *rl;

		kind = race_label_kind_of_segment(i);
		if (kind < 0 || Race_label_of_seg[i] >= 0)
			continue;

		if (Race_num_labels >= RACE_MAX_LABELS)
			break;

		rl = &Race_label[Race_num_labels];
		// Same grouping rule as the checkpoint table, so a CHECKPOINT label
		// always corresponds to exactly one checkpoint.
		number = (kind == RACE_LABEL_CHECKPOINT) ? race_checkpoint_of_segment(i) : 0;

		// Flood fill through directly connected segments of the same kind so
		// a checkpoint or pad built out of several cubes reads as one marker,
		// labelled at the centre of the whole group.
		n = 0;
		queue[tail++] = i;
		Race_label_of_seg[i] = (sbyte)Race_num_labels;

		while (head < tail)
		{
			int seg = queue[head++];
			vms_vector center;
			int side;

			compute_segment_center(&center, &Segments[seg]);
			sx += center.x;
			sy += center.y;
			sz += center.z;
			n++;

			for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
			{
				int child = Segments[seg].children[side];

				if (child < 0 || child > Highest_segment_index)
					continue;
				if (Race_label_of_seg[child] >= 0)
					continue;
				if (race_label_kind_of_segment(child) != kind)
					continue;
				if (tail >= (int)(sizeof(queue)/sizeof(queue[0])))
					continue;

				Race_label_of_seg[child] = (sbyte)Race_num_labels;
				queue[tail++] = child;
			}
		}

		rl->kind = kind;
		rl->number = number;
		rl->pos.x = (fix)(sx / n);
		rl->pos.y = (fix)(sy / n);
		rl->pos.z = (fix)(sz / n);

		Race_num_labels++;
	}
}

int race_get_labels(const race_label **labels)
{
	if (labels)
		*labels = Race_label;

	return Race_num_labels;
}

int race_label_visible(int i)
{
	int n;

	if (i < 0 || i >= Race_num_labels)
		return 0;

	// Render_list holds exactly the segments the renderer walked this frame,
	// which is a good enough occlusion test and costs nothing to reuse.
	for (n = 0; n < N_render_segs; n++)
	{
		int seg = Render_list[n];

		if (seg >= 0 && seg <= Highest_segment_index && Race_label_of_seg[seg] == i)
			return 1;
	}

	return 0;
}

//	-------------------------------------------------------------------------
//	Minimap projection
//	-------------------------------------------------------------------------

// The HUD minimap is a flat top-down slice of the track, so it needs a plane
// to flatten onto. Mines are not laid out on any particular axis, so instead
// of assuming world XZ the level's own bounding box picks it: the two axes the
// track spreads out over most become the map's x and y, and the third (the one
// a mostly-flat track barely uses) is thrown away. One shared scale keeps the
// map square, so a track never comes out stretched.
static int Race_map_ax = 0, Race_map_ay = 2;	// world axes drawn as map x/y
static fix Race_map_cx = 0, Race_map_cy = 0;	// centre of the track on those axes
static fix Race_map_half = 0;					// half-extent; ±this covers the track
static int Race_map_ok = 0;

static fix race_vec_axis(const vms_vector *v, int axis)
{
	switch (axis)
	{
		case 0:  return v->x;
		case 1:  return v->y;
		default: return v->z;
	}
}

static void race_init_map(void)
{
	fix mn[3], mx[3], ext[3];
	int i, a, wide = 0, tall = -1;

	Race_map_ok = 0;

	if (Num_vertices <= 0)
		return;

	for (a = 0; a < 3; a++)
	{
		mn[a] = race_vec_axis(&Vertices[0], a);
		mx[a] = mn[a];
	}

	for (i = 1; i < Num_vertices; i++)
		for (a = 0; a < 3; a++)
		{
			fix v = race_vec_axis(&Vertices[i], a);

			if (v < mn[a])
				mn[a] = v;
			if (v > mx[a])
				mx[a] = v;
		}

	for (a = 0; a < 3; a++)
		ext[a] = mx[a] - mn[a];

	// Keep the two widest axes: the widest is the map's x, the runner-up its y.
	for (a = 1; a < 3; a++)
		if (ext[a] > ext[wide])
			wide = a;

	for (a = 0; a < 3; a++)
		if (a != wide && (tall < 0 || ext[a] > ext[tall]))
			tall = a;

	Race_map_ax = wide;
	Race_map_ay = tall;
	Race_map_cx = mn[wide] + ext[wide]/2;
	Race_map_cy = mn[tall] + ext[tall]/2;
	Race_map_half = ext[wide]/2;

	if (Race_map_half < F1_0)		// degenerate level; nothing to show
		return;

	Race_map_half = Race_map_half + Race_map_half/16;	// a little air around the edges
	Race_map_ok = 1;
}

int race_map_project(const vms_vector *pos, fix *mx, fix *my)
{
	if (!Race_map_ok || !pos)
		return 0;

	if (mx)
		*mx = fixdiv(race_vec_axis(pos, Race_map_ax) - Race_map_cx, Race_map_half);
	if (my)
		*my = fixdiv(race_vec_axis(pos, Race_map_ay) - Race_map_cy, Race_map_half);

	return 1;
}

// Top-down outline of the mine, built once as the level loads and kept in map
// space so the HUD only has to scale it into its frame. This follows what the
// automap does: the lines that define a mine are the edges of its solid walls
// (a side with no segment behind it), minus the seams where two nearly
// coplanar walls meet -- without that cull every segment boundary down a
// straight corridor draws as a rung and the shape disappears in the noise.
//
// Flattened onto the map plane, a track's floors and ceilings collapse onto
// the same lines as the walls between them, which is exactly the silhouette
// wanted here.
#define RACE_MAP_MAX_LINES  8192
#define RACE_MAP_HASH_SIZE  (1<<16)		// edges are hashed to find the two faces sharing one
#define RACE_MAP_MIN_LINE   (F1_0/200)	// shorter than a pixel on any sane map size: drop it

static race_map_line Race_map_line[RACE_MAP_MAX_LINES];
static int Race_num_map_lines = 0;

typedef struct race_map_edge {
	int64_t		key;		// vertex pair, low vertex first; -1 for an empty slot
	int			va, vb;
	vms_vector	normal;		// normal of the first face found on this edge
	ubyte		flat;		// a second, nearly coplanar face shares it -- a seam, not an outline
} race_map_edge;

static void race_map_side_normal(int segnum, int sidenum, vms_vector *n)
{
#ifdef COMPACT_SEGS
	get_side_normal(&Segments[segnum], sidenum, 0, n);
#else
	*n = Segments[segnum].sides[sidenum].normals[0];
#endif
}

// Records one wall edge, or folds it into the entry already there. Returns the
// slot, or NULL if the table is full (which just costs us some outline).
static race_map_edge *race_map_add_edge(race_map_edge *table, int va, int vb, const vms_vector *n)
{
	int64_t key;
	int lo = va < vb ? va : vb, hi = va < vb ? vb : va;
	unsigned h;
	int probe;

	if (lo == hi)
		return NULL;

	key = (int64_t)lo * MAX_VERTICES + hi;
	h = ((unsigned)key * 2654435761u) & (RACE_MAP_HASH_SIZE - 1);

	for (probe = 0; probe < RACE_MAP_HASH_SIZE; probe++)
	{
		race_map_edge *e = &table[(h + probe) & (RACE_MAP_HASH_SIZE - 1)];

		if (e->key == key)
		{
			// Same threshold the automap uses to decide an edge does not
			// define anything.
			if (vm_vec_dot(&e->normal, n) > (F1_0 - F1_0/10))
				e->flat = 1;

			return e;
		}

		if (e->key < 0)
		{
			e->key = key;
			e->va = lo;
			e->vb = hi;
			e->normal = *n;
			e->flat = 0;
			return e;
		}
	}

	return NULL;
}

static void race_build_map_outline(void)
{
	race_map_edge *table;
	int i, s, sn;

	Race_num_map_lines = 0;

	if (!Race_map_ok)
		return;

	table = d_malloc(RACE_MAP_HASH_SIZE * sizeof(*table));

	if (!table)
		return;

	for (i = 0; i < RACE_MAP_HASH_SIZE; i++)
		table[i].key = -1;

	for (s = 0; s <= Highest_segment_index; s++)
		for (sn = 0; sn < MAX_SIDES_PER_SEGMENT; sn++)
		{
			int v[4];
			vms_vector n;

			if (Segments[s].children[sn] != -1)		// open into another segment
				continue;

			race_map_side_normal(s, sn, &n);
			get_side_verts(v, s, sn);

			for (i = 0; i < 4; i++)
				race_map_add_edge(table, v[i], v[(i+1) & 3], &n);
		}

	for (i = 0; i < RACE_MAP_HASH_SIZE && Race_num_map_lines < RACE_MAP_MAX_LINES; i++)
	{
		race_map_edge *e = &table[i];
		race_map_line *l = &Race_map_line[Race_num_map_lines];
		fix dx, dy;

		if (e->key < 0 || e->flat)
			continue;

		if (!race_map_project(&Vertices[e->va], &l->x0, &l->y0) ||
			!race_map_project(&Vertices[e->vb], &l->x1, &l->y1))
			continue;

		// An edge running along the axis the map throws away flattens to a
		// point, and a very short one cannot land on more than one pixel.
		dx = abs(l->x1 - l->x0);
		dy = abs(l->y1 - l->y0);

		if (dx < RACE_MAP_MIN_LINE && dy < RACE_MAP_MIN_LINE)
			continue;

		Race_num_map_lines++;
	}

	d_free(table);
}

int race_get_map_outline(const race_map_line **lines)
{
	if (lines)
		*lines = Race_map_line;

	return Race_num_map_lines;
}

//	-------------------------------------------------------------------------
//	Classes and the pre-race lobby
//	-------------------------------------------------------------------------

// Every race opens with the ships held on the grid while each player picks a
// class and readies up; the start countdown does not begin until everyone is
// in (or the lobby's hard cap runs out, so one AFK player cannot hold a whole
// lobby hostage). This is the same shape as Survival's shop suspend in D1:
// input is diverted whole while it is up, readiness is broadcast, and every
// machine reaches the same "everyone is in" answer off the same synced state.
//
// A class is picked once, at the line, and lasts the race. Their numbers are
// applied in three places outside this file: thrust and afterburner drain in
// read_flying_controls() (controls.c), and damage in apply_damage_to_player()
// (collide.c), which runs on the machine taking the hit -- so the shooter's
// class has to be known there too, which is why classes are synced rather
// than kept local.
static const race_class_info Race_class_table[RACE_NUM_CLASSES] = {
	{
		// The tank. Slow enough to feel heavy, but nowhere near slow enough
		// to be unwinnable -- speed is the currency in a race, and the old
		// -25% could not be bought back with any amount of armour.
		.name = "HEAVY HITTER",
		.weapon = "FUSION CANNON",
		.perks = "+75% SHIELDS   -8% SPEED",
		.primary = FUSION_INDEX,
		.powerup = POW_FUSION_WEAPON,
		.secondary = {{ -1 }, { -1 }},
		.shield_pct = 175,
		.speed = F1_0 - (F1_0*8)/100,
		.damage_taken = F1_0,
		.damage_dealt = F1_0,
		.afterburner_drain = F1_0,
		.box_item_time = F1_0,
		.boost_time = F1_0,
		.boost_power = F1_0,
	},
	{
		// Genuinely glass now: the quickest ship and the hardest hitter, on
		// three quarters of a shield bar and taking a quarter more from
		// everything that lands.
		.name = "GLASS CANNON",
		.weapon = "VULCAN CANNON",
		.perks = "+15% DMG DEALT   +8% SPEED   -40% SHIELDS",
		.primary = VULCAN_INDEX,
		.powerup = POW_VULCAN_WEAPON,
		.secondary = {{ -1 }, { -1 }},
		.shield_pct = 60,
		.speed = F1_0 + (F1_0*8)/100,
		.damage_taken = F1_0,
		.damage_dealt = F1_0 + (F1_0*15)/100,
		.afterburner_drain = F1_0,
		.box_item_time = F1_0,
		.boost_time = F1_0,
		.boost_power = F1_0,
	},
	{
		// The all-rounder: stock numbers everywhere, and the only class whose
		// edge is in the throttle rather than the guns.
		.name = "HYBRID PYRO",
		.weapon = "QUAD LASERS LVL 4",
		.perks = "+25% AFTERBURNER   +10% BOOST PADS",
		.primary = LASER_INDEX,
		.powerup = POW_QUAD_FIRE,
		.secondary = {{ -1 }, { -1 }},
		.speed = F1_0,
		.damage_taken = F1_0,
		.damage_dealt = F1_0,
		// Drain scales by the reciprocal of the bonus, so a tank that empties
		// 20% slower lasts 25% longer.
		.afterburner_drain = (F1_0*4)/5,
		.box_item_time = F1_0,
		.boost_time = F1_0 + F1_0/10,
		.boost_power = F1_0,
	},
	{
		// Mines are the point, so they are the icon, and the caps are low: a
		// Trapper who could bank ten of them would be laying a minefield
		// rather than picking their corner.
		.name = "TRAPPER",
		.weapon = "PLASMA CANNON + MINES",
		.perks = "MINES RESTOCK   -15% DMG DEALT   -5% SPEED",
		.primary = PLASMA_INDEX,
		.powerup = POW_PROXIMITY_WEAPON,
		.secondary = {
			{ PROXIMITY_INDEX,  RACE_TRAPPER_PROX_CAP,  RACE_TRAPPER_PROX_TIME },
			{ SMART_MINE_INDEX, RACE_TRAPPER_SMART_CAP, RACE_TRAPPER_SMART_TIME },
		},
		.speed = F1_0 - F1_0/20,
		.damage_taken = F1_0,
		.damage_dealt = F1_0 - (F1_0*15)/100,
		.afterburner_drain = F1_0,
		.box_item_time = F1_0,
		.boost_time = F1_0,
		.boost_power = F1_0,
	},
	{
		// Pays for the best box economy in the field with a thinner hull.
		// Used to also carry a speed penalty on top of that, which stacked
		// two costs against a payoff that is RNG rather than pace -- a race
		// is a straight line to the finish, and being both fragile and slow
		// while you wait for the dice to pay off never felt worth it. The
		// hull cost alone is the class now; the throttle is stock.
		.name = "SCAVENGER",
		.weapon = "SPREADFIRE CANNON",
		.perks = "2 ITEMS/BOX   +50% LOOT   -15% SHIELDS",
		.primary = SPREADFIRE_INDEX,
		.powerup = POW_SPREADFIRE_WEAPON,
		.secondary = {{ -1 }, { -1 }},
		.shield_pct = 85,
		.speed = F1_0,
		.damage_taken = F1_0,
		.damage_dealt = F1_0,
		.afterburner_drain = F1_0,
		.box_extra_rolls = 1,
		.box_item_time = F1_0 + F1_0/2,
		.boost_time = F1_0,
		.boost_power = F1_0,
	},
	{
		// The route specialist: pads are scenery to everyone else and the
		// whole build to this one. Used to dock both speed AND turn to pay
		// for that, which on a track with only a couple of pads left it
		// strictly worse than the stock ship for the whole rest of the lap --
		// there was no track on which the trade could win. The penalty is
		// halved to fix that. First pass at the payoff (doubled duration plus
		// a power bonus on top) overcorrected into the opposite problem --
		// unbeatable on anything with a real pad chain -- so it's down to
		// just a longer hold, no added push.
		.name = "CHARGING BULL",
		.weapon = "HELIX CANNON",
		.perks = "+60% BOOST PADS   -5% SPEED   -8% TURN",
		.primary = HELIX_INDEX,
		.powerup = POW_HELIX_WEAPON,
		.secondary = {{ -1 }, { -1 }},
		.speed = F1_0 - (F1_0*5)/100,
		.turn = F1_0 - (F1_0*8)/100,
		.damage_taken = F1_0,
		.damage_dealt = F1_0,
		.afterburner_drain = F1_0,
		.box_item_time = F1_0,
		.boost_time = F1_0 + (F1_0*6)/10,
		.boost_power = F1_0,
	},
	{
		// The phoenix shell bounces, so this one does not need a clean line
		// to hurt somebody -- it shoots corners. Built to live where that
		// pays: the quickest thing on the track through the tight stuff. The
		// shield cost used to run deeper than any other class's, which meant
		// one bad hit -- a wall, a bot, a stray shot -- cost this class a
		// respawn that the speed and turn on offer couldn't buy back. Eased
		// off to line up with what Glass Cannon and Scavenger pay for a
		// similar edge.
		.name = "FIREBIRD",
		.weapon = "PHOENIX CANNON",
		.perks = "+15% TURN   +5% SPEED   -20% SHIELDS",
		.primary = PHOENIX_INDEX,
		.powerup = POW_PHOENIX_WEAPON,
		.secondary = {{ -1 }, { -1 }},
		.shield_pct = 80,
		.speed = F1_0 + (F1_0*5)/100,
		.turn = F1_0 + (F1_0*15)/100,
		.damage_taken = F1_0,
		.damage_dealt = F1_0,
		.afterburner_drain = F1_0,
		.box_item_time = F1_0,
		.boost_time = F1_0,
		.boost_power = F1_0,
	},
};

static sbyte Race_class[MAX_PLAYERS];		// class per player, RACE_CLASS_NONE until they pick
// When the local player's kit secondary tops itself up next. Armed by
// race_grant_class_loadout(), so it restarts with every spawn.
static fix64 Race_secondary_refill_at[RACE_MAX_KIT_SECONDARIES];
static ubyte Race_ready[MAX_PLAYERS];		// who has locked in
static int Race_lobby_open = 0;
static int Race_lobby_cursor = 0;			// class the local player is looking at
static fix64 Race_lobby_deadline = 0;		// hard cap on the whole lobby
// The lobby ignores the keyboard for a moment after it opens. The keypress
// that started the race -- ENTER on the menu, or on a briefing screen -- is
// often still being repeated when the level finishes loading, and it would
// otherwise lock the player into the class the cursor happened to start on
// before the panel had even been drawn once.
#define RACE_LOBBY_INPUT_GRACE  (F1_0*3/4)
static fix64 Race_lobby_input_at = 0;
static fix64 Race_lobby_next_resend = 0;	// our own ready is re-broadcast until the race starts

const race_class_info *race_get_class_info(int cls)
{
	if (cls < 0 || cls >= RACE_NUM_CLASSES)
		return NULL;

	return &Race_class_table[cls];
}

void race_set_player_class(int pnum, int cls)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS || cls < 0 || cls >= RACE_NUM_CLASSES)
		return;

	Race_class[pnum] = (sbyte)cls;
	race_grant_class_loadout(pnum);
}

int race_get_class(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return RACE_CLASS_NONE;

	return Race_class[pnum];
}

// The local player's own numbers, for the physics and damage call sites. A
// player who somehow has no class (joined mid-race, packet lost) races on the
// stock numbers rather than being penalised for it.
static const race_class_info *race_my_class(void)
{
	if (!(Game_mode & GM_RACE))
		return NULL;

	return race_get_class_info(Race_class[Player_num]);
}

fix race_class_speed_scale(void)
{
	const race_class_info *ci = race_my_class();

	return ci ? ci->speed : F1_0;
}

fix race_class_turn_scale(void)
{
	const race_class_info *ci = race_my_class();

	// 0 in the table means "leave it alone", so a class only has to name this
	// if it actually trades handling for something.
	return (ci && ci->turn) ? ci->turn : F1_0;
}

fix race_class_afterburner_drain_scale(void)
{
	const race_class_info *ci = race_my_class();

	return ci ? ci->afterburner_drain : F1_0;
}

fix race_scale_damage_for(int pnum, const object *killer, fix damage)
{
	const race_class_info *mine = race_get_class_info(race_get_class(pnum));

	if (!(Game_mode & GM_RACE))
		return damage;

	// The shooter's half. This runs on the machine being hit, which knows
	// every player's class because they are synced -- a Glass Cannon hits 5%
	// harder no matter whose screen the hit lands on.
	if (killer && killer->type == OBJ_PLAYER)
	{
		const race_class_info *theirs = race_get_class_info(race_get_class(killer->id));

		if (theirs)
			damage = fixmul(damage, theirs->damage_dealt);
	}

	// And the target's half, which covers every source: weapons, blast doors,
	// lava, flying into a wall.
	if (mine)
		damage = fixmul(damage, mine->damage_taken);

	return damage;
}

fix race_scale_damage(const object *killer, fix damage)
{
	return race_scale_damage_for(Player_num, killer, damage);
}

// The class weapon. Handed out fresh on every spawn (see the call in
// init_player_stats_new_ship(), right after race_strip_loadout()), so dying
// costs a racer their box loot but never the kit their class is built around.
void race_grant_class_loadout(int pnum)
{
	const race_class_info *ci;
	player *p;
	int i;

	if (!(Game_mode & GM_RACE) || pnum < 0 || pnum >= MAX_PLAYERS)
		return;

	ci = race_get_class_info(Race_class[pnum]);

	if (!ci)
		return;

	// From empty, always. Whatever else put a gun in the rack -- a netgame
	// spawn-weapon toggle that ran before GM_RACE was set, a stock loadout
	// from before the class was picked -- a racer carries their class kit and
	// nothing else.
	race_strip_loadout(pnum);

	p = &Players[pnum];
	p->primary_weapon_flags |= HAS_FLAG(ci->primary);
	p->flags |= ci->player_flags;

	// Every racer flies with a burner, full off the line. A race is about
	// speed, and the one thing a racing game must not do is hand half the grid
	// a way to go faster and not the rest -- so the kits trade on how long it
	// lasts (afterburner_drain), not on whether you have one at all.
	p->flags |= PLAYER_FLAGS_AFTERBURNER;
	p->afterburner_charge = F1_0;

	for (i = 0; i < RACE_MAX_KIT_SECONDARIES; i++)
	{
		const race_kit_secondary *ks = &ci->secondary[i];
		int start;

		if (ks->index < 0)
			continue;

		start = ks->cap / 2;					// half a load off the line
		p->secondary_weapon_flags |= HAS_FLAG(ks->index);

		if (p->secondary_ammo[ks->index] < start)
			p->secondary_ammo[ks->index] = start;

		if (pnum == Player_num)
		{
			Race_secondary_refill_at[i] = GameTime64 + ks->refill;
			Race_expire_sec[ks->index] = 0;		// the kit has no fuse
		}
	}

	// Defined in gameseq.c, where the level-start loadout is set.
	extern fix StartingShields;

	// A class can come to the line with a different shield bar. Scaled off
	// StartingShields rather than set to an absolute, so a host who has tuned
	// that still gets what they asked for; and set rather than added, so a
	// respawn is the same deal as the start.
	// A bot's shields live in its player slot the same way, and race_bot_place()
	// hands it StartingShields before calling this -- so the class bar has to
	// be applied for the field too, or every CPU racer laps on stock shields
	// no matter what kit it is in.
	if (ci->shield_pct > 0 && (pnum == Player_num || race_player_is_bot(pnum)))
		p->shields = (fix)((fix64)StartingShields * ci->shield_pct / 100);

	switch (ci->primary)
	{
		case LASER_INDEX:
			p->laser_level = MAX_LASER_LEVEL;		// level 4
			p->flags |= PLAYER_FLAGS_QUAD_LASERS;
			break;
		case VULCAN_INDEX:
			p->primary_ammo[VULCAN_INDEX] = Primary_ammo_max[VULCAN_INDEX];
			break;
		default:
			break;
	}

	if (pnum == Player_num)
	{
		p->primary_weapon = ci->primary;
		// The class weapon has no fuse -- only box loot expires.
		Race_expire_pri[ci->primary] = 0;
	}
}

// True if `index` is the local player's class weapon, which box loot must
// never expire out from under them (a Glass Cannon who rolls a Vulcan out of
// a box keeps their Vulcan when the box copy runs out).
int race_is_class_weapon(int wclass, int index)
{
	const race_class_info *ci = race_my_class();

	if (!ci)
		return 0;

	if (wclass == CLASS_PRIMARY)
		return index == ci->primary;

	{
		int i;

		for (i = 0; i < RACE_MAX_KIT_SECONDARIES; i++)
			if (ci->secondary[i].index >= 0 && index == ci->secondary[i].index)
				return 1;
	}

	return 0;
}

// The Trapper's mine trickle. Local player only: everyone else's ammo is
// their own machine's business, and nothing else needs to agree on it.
static void race_class_frame(void)
{
	const race_class_info *ci = race_my_class();
	player *p = &Players[Player_num];
	int i;

	if (!ci || is_observer())
		return;

	for (i = 0; i < RACE_MAX_KIT_SECONDARIES; i++)
	{
		const race_kit_secondary *ks = &ci->secondary[i];
		int cap;

		if (ks->index < 0 || !ks->refill)
			continue;

		cap = min(ks->cap, (int)Secondary_ammo_max[ks->index]);

		if (p->secondary_ammo[ks->index] >= cap)
		{
			// Full: hold the clock an interval out, so spending one is
			// followed by a whole wait rather than an instant refund of the
			// time spent sitting at the cap.
			Race_secondary_refill_at[i] = GameTime64 + ks->refill;
			continue;
		}

		// A deadline further out than one interval means the clock was reset
		// under us (a new level); re-arm rather than stall until GameTime64
		// catches up again.
		if (Race_secondary_refill_at[i] > GameTime64 + ks->refill)
			Race_secondary_refill_at[i] = GameTime64 + ks->refill;

		if (GameTime64 < Race_secondary_refill_at[i])
			continue;

		p->secondary_weapon_flags |= HAS_FLAG(ks->index);
		p->secondary_ammo[ks->index]++;
		Race_secondary_refill_at[i] = GameTime64 + ks->refill;

		digi_play_sample(SOUND_GOOD_SELECTION_SECONDARY, F1_0/2);
	}
}

static void race_send_ready(int pnum)
{
	if (!race_is_multi())
		return;

	multibuf[0] = MULTI_RACE_READY;
	multibuf[1] = (ubyte)pnum;
	multibuf[2] = (ubyte)Race_class[pnum];
	multi_send_data(multibuf, 3, 2);
}

void multi_do_race_ready(const ubyte *buf)
{
	int pnum = buf[1];
	int cls = (sbyte)buf[2];

	if (!(Game_mode & GM_RACE) || pnum < 0 || pnum >= MAX_PLAYERS)
		return;

	if (cls >= 0 && cls < RACE_NUM_CLASSES)
		Race_class[pnum] = (sbyte)cls;

	Race_ready[pnum] = 1;
}

// Who the lobby is waiting on: everyone still connected, observers aside --
// they are watching the race, not in it.
static int race_lobby_counts(int *ready_out, int *total_out)
{
	int i, ready = 0, total = 0;

	if (!race_is_multi())
	{
		if (ready_out)
			*ready_out = Race_ready[Player_num] ? 1 : 0;
		if (total_out)
			*total_out = 1;

		return 1;
	}

	for (i = 0; i < N_players; i++)
	{
		if (Players[i].connected == CONNECT_DISCONNECTED)
			continue;
#ifdef NETWORK
		if (Netgame.host_is_obs && i == 0)
			continue;
#endif
		total++;

		if (Race_ready[i])
			ready++;
	}

	if (ready_out)
		*ready_out = ready;
	if (total_out)
		*total_out = total;

	return total;
}

int race_lobby_ready_counts(int *ready_out, int *total_out)
{
	return race_lobby_counts(ready_out, total_out);
}

int race_player_is_ready(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return 0;

	return Race_ready[pnum];
}

int race_lobby_is_open(void)
{
	return (Game_mode & GM_RACE) && Race_lobby_open;
}

// The lobby owns the keyboard and the stick whole while it is up, the way
// chat entry and Survival's shop do -- see the ReadControls() gate.
int race_lobby_blocks_input(void)
{
	return race_lobby_is_open() && !is_observer();
}

int race_lobby_cursor_class(void)
{
	return Race_lobby_cursor;
}

fix64 race_lobby_time_left(void)
{
	fix64 left;

	if (!Race_lobby_open)
		return 0;

	left = Race_lobby_deadline - GameTime64;

	return left > 0 ? left : 0;
}

// Locks the local player in on `cls`: kits them out and tells everyone else,
// which is the only thing the rest of the lobby is waiting on.
static void race_lobby_lock_in(int cls)
{
	if (cls < 0 || cls >= RACE_NUM_CLASSES)
		cls = 0;

	Race_lobby_cursor = cls;
	Race_class[Player_num] = (sbyte)cls;
	Race_ready[Player_num] = 1;

	race_grant_class_loadout(Player_num);
	race_send_ready(Player_num);

	HUD_init_message(HM_DEFAULT, "LOCKED IN: %s", Race_class_table[cls].name);
}

int race_lobby_handle_key(int key)
{
	// Only the keys the panel actually owns are claimed. Everything else --
	// the menu on ESC, chat, a screenshot -- still works while the grid
	// fills up; flight and firing are held by their own gates, not by
	// swallowing the keyboard whole.
	if (!race_lobby_blocks_input() || Race_ready[Player_num])
		return 0;

	if (GameTime64 < Race_lobby_input_at)
		return 0;

	// KEY_1..KEY_5 are consecutive, so the number row maps straight onto the
	// class list however long that grows.
	if (key >= KEY_1 && key < KEY_1 + RACE_NUM_CLASSES)
	{
		Race_lobby_cursor = key - KEY_1;
		return 1;
	}

	switch (key)
	{
		case KEY_UP:
		case KEY_PAD8:
		case KEY_LEFT:
			Race_lobby_cursor = (Race_lobby_cursor + RACE_NUM_CLASSES - 1) % RACE_NUM_CLASSES;
			break;

		case KEY_DOWN:
		case KEY_PAD2:
		case KEY_RIGHT:
			Race_lobby_cursor = (Race_lobby_cursor + 1) % RACE_NUM_CLASSES;
			break;

		case KEY_ENTER:
		case KEY_PADENTER:
		case KEY_SPACEBAR:
			race_lobby_lock_in(Race_lobby_cursor);
			break;

		default:
			return 0;
	}

	return 1;
}

// Closes the lobby and rolls straight into the start countdown. Anyone who
// never locked in is dropped onto the class they were looking at, so a race
// always starts with everybody kitted.
static void race_lobby_close(void)
{
	if (!Race_lobby_open)
		return;

	if (!Race_ready[Player_num] && !is_observer())
		race_lobby_lock_in(Race_lobby_cursor);

	Race_lobby_open = 0;

	Race_countdown_timer = i2f(RACE_COUNTDOWN_SECONDS);
	Race_counting_down = 1;
	Race_go_announced = 0;
	Race_countdown_voice = -1;
}

// Runs on every machine off state everybody has (the ready flags), so the
// clients reach the same answer the host does; the host's periodic state
// broadcast is what settles any disagreement about the exact moment.
static void race_lobby_frame(void)
{
	int ready = 0, total = 0;

	if (!Race_lobby_open)
		return;

	race_lobby_counts(&ready, &total);

	// Keep telling the others we are in until the race actually starts: the
	// ready packet is the one piece of lobby state nobody else can derive,
	// and a lobby that lost it would sit there forever.
	if (Race_ready[Player_num] && race_is_multi() && GameTime64 >= Race_lobby_next_resend)
	{
		race_send_ready(Player_num);
		Race_lobby_next_resend = GameTime64 + F1_0;
	}

	// Clients wait to be told (see multi_do_race_state) so that everyone
	// starts the countdown on the same frame as the host.
	if (race_is_multi() && !multi_i_am_master())
	{
		// ...but not forever. If the host never releases the grid -- it
		// dropped, or every state packet since has been lost -- come out on
		// our own a little past the cap rather than sitting in the panel for
		// the rest of the session.
		if (GameTime64 >= Race_lobby_deadline + RACE_LOBBY_HOST_GRACE)
			race_lobby_close();

		return;
	}

	// An empty roster means there is nobody to wait on -- an all-observer
	// game -- but only once we are out of the way ourselves: a roster that
	// simply hasn't filled in yet must not release the grid before the
	// player at the keyboard has even picked.
	{
		int nobody_to_wait_for = (total <= 0) && (Race_ready[Player_num] || is_observer());

		if ((total > 0 && ready >= total) || nobody_to_wait_for ||
			GameTime64 >= Race_lobby_deadline)
			race_lobby_close();
	}
}

//	-------------------------------------------------------------------------
//	Level setup and per-frame update
//	-------------------------------------------------------------------------

//	-------------------------------------------------------------------------
//	Checkpoint table
//	-------------------------------------------------------------------------

// Checkpoints are derived from the segments themselves, not from the level's
// matcen bookkeeping. Num_robot_centers is read straight out of the file's
// matcen table (gamesave.c) while each segment carries its own matcen_num, and
// nothing guarantees the two agree -- a robot-maker segment whose matcen_num
// lands past Num_robot_centers is a checkpoint the player can see and fly
// through that would never register.
//
// Connected robot-maker segments are grouped into one checkpoint, matching how
// the floating labels merge, so one CHECKPOINT label is always exactly one
// checkpoint.
static sbyte Race_cp_of_seg[MAX_SEGMENTS];			// checkpoint owning each segment, -1 for none
static int Race_cp_segnum[RACE_MAX_CHECKPOINTS];	// a representative segment per checkpoint
static vms_vector Race_cp_center[RACE_MAX_CHECKPOINTS];

static void race_init_checkpoints(void)
{
	int i;

	Race_num_checkpoints = 0;
	memset(Race_cp_of_seg, -1, sizeof(Race_cp_of_seg));

	for (i = 0; i <= Highest_segment_index; i++)
	{
		int queue[RACE_MAX_CHECKPOINTS * 8];
		int head = 0, tail = 0, n = 0;
		fix64 sx = 0, sy = 0, sz = 0;

		if (Segment2s[i].special != SEGMENT_IS_ROBOTMAKER || Race_cp_of_seg[i] >= 0)
			continue;

		if (Race_num_checkpoints >= RACE_MAX_CHECKPOINTS)
			break;

		queue[tail++] = i;
		Race_cp_of_seg[i] = (sbyte)Race_num_checkpoints;

		while (head < tail)
		{
			int seg = queue[head++];
			vms_vector center;
			int side;

			compute_segment_center(&center, &Segments[seg]);
			sx += center.x;
			sy += center.y;
			sz += center.z;
			n++;

			for (side = 0; side < MAX_SIDES_PER_SEGMENT; side++)
			{
				int child = Segments[seg].children[side];

				if (child < 0 || child > Highest_segment_index)
					continue;
				if (Race_cp_of_seg[child] >= 0)
					continue;
				if (Segment2s[child].special != SEGMENT_IS_ROBOTMAKER)
					continue;
				if (tail >= (int)(sizeof(queue)/sizeof(queue[0])))
					continue;

				Race_cp_of_seg[child] = (sbyte)Race_num_checkpoints;
				queue[tail++] = child;
			}
		}

		Race_cp_segnum[Race_num_checkpoints] = i;
		Race_cp_center[Race_num_checkpoints].x = (fix)(sx / n);
		Race_cp_center[Race_num_checkpoints].y = (fix)(sy / n);
		Race_cp_center[Race_num_checkpoints].z = (fix)(sz / n);
		Race_num_checkpoints++;
	}
}

int race_checkpoint_segment(int cp)
{
	if (cp < 0 || cp >= Race_num_checkpoints || cp >= RACE_MAX_CHECKPOINTS)
		return -1;

	return Race_cp_segnum[cp];
}

int race_checkpoint_count(void)
{
	return Race_num_checkpoints;
}

int race_checkpoint_of_segment(int segnum)
{
	if (segnum < 0 || segnum > Highest_segment_index)
		return -1;

	return Race_cp_of_seg[segnum];
}

// Picks the start/finish line, and reports its segment.
//
// A mapper marks it explicitly with a repair center. Repair centers do nothing
// in D2 (they were dropped after D1) and race mode suppresses what little they
// did (see object_move_one), but they still draw with their own distinctive
// texture -- so the line reads as a real feature of the track rather than an
// invisible trigger. Unlike a matcen it is also individually selectable in an
// editor, which is the whole point: matcen_num is handed out as the mine
// loads, in segment index order (see create_matcen), so it bears no relation
// to the order a mapper placed them and there is no way to aim for a number.
//
// Every repair center on the level is part of the line, so a wide finish can
// be built out of several segments.
//
// With no repair center on the level, fall back to whichever checkpoint sits
// closest to the starting grid: the grid is at the line by definition, so
// that is right far more often than trusting an arbitrary index.
static void race_pick_finish(void)
{
	vms_vector grid;
	fix best = 0;
	int i, n = 0;

	Race_finish_checkpoint = -1;
	Race_finish_segnum = -1;
	Race_finish_marked = 0;

	for (i = 0; i <= Highest_segment_index; i++)
	{
		if (Segment2s[i].special == SEGMENT_IS_REPAIRCEN)
		{
			// Explicitly marked. Every matcen on the level is then an
			// ordinary checkpoint.
			Race_finish_segnum = i;
			Race_finish_marked = 1;
			return;
		}
	}

	if (Race_num_checkpoints <= 0)
		return;

	vm_vec_zero(&grid);

	for (i = 0; i < NumNetPlayerPositions; i++)
	{
		vm_vec_add2(&grid, &Player_init[i].pos);
		n++;
	}

	if (n)
	{
		grid.x /= n;
		grid.y /= n;
		grid.z /= n;
	}

	for (i = 0; i < Race_num_checkpoints; i++)
	{
		fix dist;

		if (!n)
		{
			// No spawn points to reason from; take the first checkpoint.
			Race_finish_checkpoint = i;
			Race_finish_segnum = Race_cp_segnum[i];
			return;
		}

		dist = vm_vec_dist_quick(&Race_cp_center[i], &grid);

		if (Race_finish_checkpoint < 0 || dist < best)
		{
			best = dist;
			Race_finish_checkpoint = i;
			Race_finish_segnum = Race_cp_segnum[i];
		}
	}
}

void race_init_level(void)
{
	int i;

	race_init_checkpoints();
	race_pick_finish();

	if (Game_mode & GM_MULTI)
	{
		if (Netgame.LapsToWin > 0)
			Race_laps_to_win = Netgame.LapsToWin;

		Race_powerup_chance = Netgame.RacePowerupChance;
		Race_allowed_items = Netgame.RaceAllowedItems;
	}

	// race_init_items() reads Race_laps_to_win/Race_allowed_items, so rebuild
	// the loot table for whatever this level's settings just became.
	race_init_items();

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		Race_player[i].checkpoints_hit = 0;
		Race_player[i].laps_completed = 0;
		Race_player[i].finished = 0;
		Race_player[i].finish_place = 0;
		Race_player[i].last_checkpoint = 0;
		Race_player[i].has_checkpoint = 0;
		Race_player[i].finish_time = 0;
		Race_wrongway_next_warn[i] = 0;
	}

	Race_cp_mask = 0;
	Race_incomplete_next_warn = 0;
	Race_respawn_segnum = -1;
	Race_emp_started = Race_emp_until = 0;
	Race_tractor_started = Race_tractor_until = 0;
	Race_tractor_by = -1;

	Race_next_place = 1;
	Race_banner_until = 0;
	Race_boost_until = 0;
	Race_boost_started = 0;
	Race_last_boost_seg = -1;
	Race_last_seg_checked = -1;
	Race_finish_grace_until = 0;
	Race_next_state_send = 0;	// GameTime64 restarts each level; don't stall the broadcast
	Race_start_time = 0;
	Race_lap_start = 0;
	Race_total_time = 0;
	Race_best_lap = 0;
	Race_num_splits = 0;
	Race_summary_pending = 0;
	Race_trichord_strength = 0;
	Race_trichord_charge = 0;
	Race_trichord_boost_until = 0;
	Race_trichord_blobs_lit = 0;

	// The grid picks classes first; the countdown starts when the lobby
	// closes (race_lobby_close()). It runs on every race level, including
	// ones with no checkpoints placed -- it is the start of the race, not a
	// property of the track layout.
	Race_countdown_timer = 0;
	Race_counting_down = 0;
	Race_go_announced = 0;
	Race_countdown_voice = -1;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		Race_class[i] = RACE_CLASS_NONE;
		Race_ready[i] = 0;
	}

	// Everyone comes to the line empty-handed: the class picked in the lobby
	// is the entire loadout, and this runs after whatever the level start
	// handed out.
	for (i = 0; i < MAX_PLAYERS; i++)
		race_strip_loadout(i);

	memset(Race_secondary_refill_at, 0, sizeof(Race_secondary_refill_at));

	Race_lobby_open = 1;
	Race_lobby_deadline = GameTime64 + RACE_LOBBY_TIMEOUT;
	Race_lobby_input_at = GameTime64 + RACE_LOBBY_INPUT_GRACE;
	Race_lobby_next_resend = 0;

	race_init_boxes();
	race_init_labels();
	race_init_items();
	race_init_map();
	race_build_map_outline();
	race_bots_init();
}

// The static/interference cue for a live EMP: SOUND_BRIEFING_PRINTING
// looped for as long as race_emp_strength() is nonzero, started the instant
// it goes off and cut the instant it clears. Edge-triggered off strength
// crossing zero rather than restarted every frame, so a nine-second EMP
// doesn't retrigger its own loop every tick while it's running. Purely
// local -- race_emp_strength() only reads nonzero for the player it
// actually landed on, so nobody else hears somebody else's jam.
static void race_update_emp_sound(void)
{
	static int playing = 0;
	int active = race_emp_strength() > 0;

	if (active && !playing)
		digi_play_sample_looping(SOUND_BRIEFING_PRINTING, F1_0, -1, -1);
	else if (!active && playing)
		digi_stop_looping_sound();

	playing = active;
}

void race_frame(void)
{
	race_update_emp_sound();

	if (Race_lobby_open)
	{
		race_lobby_frame();

		// Boxes and boost pads stay frozen behind the panel: nobody can move
		// or shoot yet, and a box respawn timer running down here would just
		// be time off the first lap.
		return;
	}

	race_class_frame();

	if (Race_counting_down)
	{
		int secs;

		Race_countdown_timer -= FrameTime;
		if (Race_countdown_timer <= 0)
		{
			Race_countdown_timer = 0;
			Race_counting_down = 0;
		}

		// The start light is the reactor countdown's own voice, counting the
		// grid down the same way it counts a mine out: SOUND_COUNTDOWN_0_SECS
		// is the base of a run of clips, one per second (cntrlcen.c uses the
		// same trick). Driven off the displayed second rather than off the
		// timer, so the voice can never disagree with the number on screen --
		// including on a client, whose clock is set by the host's state
		// packet rather than ticked locally.
		secs = race_countdown_seconds_left();

		if (secs != Race_countdown_voice)
		{
			Race_countdown_voice = secs;

			if (secs > 0)
				digi_play_sample(SOUND_COUNTDOWN_0_SECS + secs, F3_0);
		}
	}

	if (!Race_counting_down && !Race_go_announced)
	{
		Race_go_announced = 1;
		Race_start_time = GameTime64;
		Race_lap_start = GameTime64;
		HUD_init_message_literal(HM_MULTI, "GO!");

		// ...and the zero of that same run is the green light.
		digi_play_sample(SOUND_COUNTDOWN_0_SECS, F3_0);

		// The grid comes off the line stacked nose to tail (see
		// race_bot_place()'s comment on BOT_GRID_SPACING), so the first
		// corner is the one moment of the race everyone is guaranteed to be
		// touching. A few seconds where nobody can be wrecked, plus the same
		// push a boost pad gives, means that scrum costs a place rather than
		// a respawn.
		if (!is_observer() && ConsoleObject)
		{
			Players[Player_num].flags |= PLAYER_FLAGS_INVULNERABLE;
			Players[Player_num].invulnerable_time = GameTime64 - INVULNERABLE_TIME_MAX + RACE_START_INVULN_TIME;

			Race_boost_started = GameTime64;
			Race_boost_until = GameTime64 + RACE_START_BOOST_TIME;
		}

		race_bots_start_race();
	}

	if (!is_observer() && !Player_is_dead && ConsoleObject && ConsoleObject->segnum >= 0 &&
		ConsoleObject->segnum <= Highest_segment_index)
	{
		race_check_boost_pad(&Segments[ConsoleObject->segnum]);

		// Racing is about driving, not about energy management, so nobody
		// ever runs dry. Kept local: energy is per-player state that the
		// other clients don't score off, so this needs no packets.
		if (Players[Player_num].energy < MAX_ENERGY)
			Players[Player_num].energy = MAX_ENERGY;
	}

	race_boxes_frame();
	// Runs for observers too, so a spectated race looks the same as a driven
	// one -- no stray weapon pickups lying about.
	race_harvest_level_weapons();
	race_items_frame();
	race_bots_frame();
}

void race_format_time(char *buf, int bufsz, fix64 t)
{
	int secs, cs;

	if (t <= 0)
	{
		snprintf(buf, bufsz, "--:--.--");
		return;
	}

	secs = (int)(t >> 16);
	cs = (int)(((t & 0xffff) * 100) >> 16);

	snprintf(buf, bufsz, "%d:%02d.%02d", secs/60, secs%60, cs);
}

fix64 race_get_total_time(void)
{
	if (Race_player[Player_num].finished)
		return Race_total_time;

	if (Race_counting_down || !Race_go_announced)
		return 0;

	return GameTime64 - Race_start_time;
}

fix64 race_get_lap_time(void)
{
	if (Race_player[Player_num].finished)
		return 0;

	if (Race_counting_down || !Race_go_announced)
		return 0;

	return GameTime64 - Race_lap_start;
}

int race_get_splits(const fix64 **splits, fix64 *best)
{
	if (splits)
		*splits = Race_splits;
	if (best)
		*best = Race_best_lap;

	return Race_num_splits;
}

// Closes out the lap that just ended and starts the next one.
static void race_record_lap(void)
{
	fix64 split = GameTime64 - Race_lap_start;

	if (Race_num_splits < RACE_MAX_SPLITS)
		Race_splits[Race_num_splits++] = split;

	if (!Race_best_lap || split < Race_best_lap)
		Race_best_lap = split;

	Race_lap_start = GameTime64;
}

int race_take_summary_pending(void)
{
	int pending = Race_summary_pending;

	Race_summary_pending = 0;

	return pending;
}

// Drop the finisher into the standings. Deliberately does NOT touch
// Players[].connected: the player has finished the race, not left the level,
// and telling everyone else they went to the end menu would start the level's
// exit sequence for the players still driving.
void race_show_summary(void)
{
	static int in_summary = 0;

	if (in_summary)
		return;		// the results screen pumps events, which can re-enter us

	in_summary = 1;

	con_printf(CON_NORMAL, "RACE: showing summary (Game_wind=%p visible=%d)\n",
			   (void *)Game_wind, Game_wind ? window_is_visible(Game_wind) : -1);

	// Hide the game window for the duration: the standings run their own
	// event loop, and leaving the game drawing underneath both fights over
	// the screen mode and re-enters the frame we were called from.
	// kmatrix_handler() pumps the network itself, so nothing stops arriving.
	if (Game_wind && window_is_visible(Game_wind))
	{
		window_set_visible(Game_wind, 0);
		kmatrix_view(race_is_multi() && (Game_mode & GM_NETWORK));
		set_screen_mode(SCREEN_GAME);
		if (Game_wind)
			window_set_visible(Game_wind, 1);
	}
	else
	{
		kmatrix_view(race_is_multi() && (Game_mode & GM_NETWORK));
		set_screen_mode(SCREEN_GAME);
	}

	con_printf(CON_NORMAL, "RACE: summary screen closed\n");

	in_summary = 0;
}

// Elapsed race time for any player, measured on our clock. Remote finish
// times are stamped when their packet lands, so this is an estimate for
// everyone but us -- close enough for a results table, and consistent.
fix64 race_get_finish_elapsed(int pnum)
{
	fix64 t;

	if (pnum < 0 || pnum >= MAX_PLAYERS || !Race_player[pnum].finished)
		return 0;

	if (pnum == Player_num)
		return Race_total_time;

	t = Race_player[pnum].finish_time - Race_start_time;

	return (t > 0) ? t : 0;
}

int race_countdown_active(void)
{
	return Race_counting_down;
}

int race_countdown_seconds_left(void)
{
	if (!Race_counting_down)
		return 0;

	return f2i(Race_countdown_timer + F1_0*7/8);
}

//	-------------------------------------------------------------------------
//	Networking
//	-------------------------------------------------------------------------

static void race_send_update(const race_player_info *rp)
{
	if (!race_is_multi() || is_observer())
		return;

	multibuf[0] = MULTI_RACE_UPDATE;
	multibuf[1] = (ubyte)Player_num;
	multibuf[2] = rp->checkpoints_hit;
	multibuf[3] = rp->laps_completed;
	multibuf[4] = rp->finished;

	multi_send_data(multibuf, 5, 2);
}

// Host only: hand out the next finishing place if this player just finished.
static void race_assign_place(int pnum)
{
	race_player_info *rp = &Race_player[pnum];

	if (!rp->finished || rp->finish_place)
		return;

	if (!race_is_multi() || multi_i_am_master())
	{
		rp->finish_place = Race_next_place;
		if (Race_next_place < MAX_PLAYERS)
			Race_next_place++;
	}
}

static void race_announce_finish(int pnum)
{
	if (pnum == Player_num)
		HUD_init_message_literal(HM_MULTI, "You have finished the race!");
	else
		HUD_init_message(HM_MULTI, "%s has finished the race!", Players[pnum].callsign);
}

void race_finish_player(int pnum)
{
	race_player_info *rp;

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;

	rp = &Race_player[pnum];

	if (rp->finished)
		return;

	rp->finished = 1;
	rp->finish_time = GameTime64;
	race_assign_place(pnum);
	race_announce_finish(pnum);
}

void multi_do_race_update(const ubyte *buf)
{
	ubyte pnum = buf[1];
	race_player_info *rp;
	race_player_info incoming;

	if (!(Game_mode & GM_RACE) || pnum >= MAX_PLAYERS)
		return;

	rp = &Race_player[pnum];

	incoming = *rp;
	incoming.checkpoints_hit = buf[2];
	incoming.laps_completed = buf[3];

	// See the identical guard in multi_do_race_state(): an out-of-range
	// laps_completed off the wire otherwise reads as always-ahead progress.
	if (incoming.checkpoints_hit > Race_num_checkpoints ||
		incoming.laps_completed > Race_laps_to_win)
		return;		// malformed / stale packet for this level

	// Progress only ever moves forward, so a reordered or duplicated packet
	// can never rewind a player's standing.
	if (race_progress_of(&incoming) >= race_progress_of(rp))
	{
		rp->checkpoints_hit = incoming.checkpoints_hit;
		rp->laps_completed = incoming.laps_completed;
	}

	if (buf[4] && !rp->finished)
	{
		rp->finished = 1;
		rp->finish_time = GameTime64;
		race_assign_place(pnum);
		race_announce_finish(pnum);
	}
}

// Host -> everyone, authoritative snapshot of the whole race. Sent often
// during the countdown (so every ship gets GO at the same moment) and every
// couple of seconds afterwards, which is what heals dropped MULTI_RACE_UPDATE
// packets and brings late joiners up to date.
#define RACE_STATE_LEN	(3 + 4*MAX_PLAYERS)

static void race_send_state(void)
{
	int i, count;
	int decis;

	decis = Race_counting_down ? f2i(Race_countdown_timer * 10) : 0;
	if (decis < 0)
		decis = 0;
	if (decis > 255)
		decis = 255;

	multibuf[0] = MULTI_RACE_STATE;
	multibuf[1] = (ubyte)decis;
	multibuf[2] = (ubyte)(Race_lobby_open ? 1 : 0);
	count = 3;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		multibuf[count++] = Race_player[i].checkpoints_hit;
		multibuf[count++] = Race_player[i].laps_completed;
		multibuf[count++] = Race_player[i].finish_place;
		// Classes ride along so a late joiner, or anyone who lost a ready
		// packet, still scores everybody's damage the same way.
		multibuf[count++] = (ubyte)Race_class[i];
	}

	multi_send_data(multibuf, RACE_STATE_LEN, Race_counting_down ? 2 : 0);
}

void race_multi_frame(void)
{
	if (!(Game_mode & GM_RACE) || !race_is_multi() || !multi_i_am_master())
		return;

	if (GameTime64 < Race_next_state_send)
		return;

	race_send_state();

	// Tight cadence while the grid is still forming or counting down (the
	// countdown is only 3 seconds, and the lobby's release is what every
	// client is sitting there waiting for), lazy once the race is running.
	Race_next_state_send = GameTime64 + ((Race_counting_down || Race_lobby_open) ? F1_0/4 : F1_0*2);
}

void multi_do_race_state(const ubyte *buf)
{
	int i, count;
	fix host_countdown;

	if (!(Game_mode & GM_RACE) || multi_i_am_master())
		return;

	host_countdown = i2f(buf[1]) / 10;

	// The host owns the moment the grid is released, so that every ship gets
	// GO on the same frame rather than each client deciding for itself that
	// the last ready packet has landed.
	if (!buf[2] && Race_lobby_open)
		race_lobby_close();

	if (host_countdown > 0)
	{
		Race_counting_down = 1;
		Race_countdown_timer = host_countdown;
		Race_go_announced = 0;
	}
	else if (Race_counting_down)
	{
		Race_counting_down = 0;
		Race_countdown_timer = 0;
	}

	count = 3;
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		race_player_info *rp = &Race_player[i];
		race_player_info incoming = *rp;
		ubyte place;
		int cls;

		incoming.checkpoints_hit = buf[count++];
		incoming.laps_completed = buf[count++];
		place = buf[count++];
		cls = (sbyte)buf[count++];

		// Our own class is ours to know: we locked it in and told everyone,
		// and the host's snapshot may predate that.
		if (i != Player_num && cls >= 0 && cls < RACE_NUM_CLASSES)
			Race_class[i] = (sbyte)cls;

		// A snapshot with either field out of range can't be a real racer's
		// progress -- checkpoints_hit is bounded by the track, and nobody
		// can be more laps in than the race is long. Without this a bad or
		// malicious sender (an untrusted ubyte off the wire) sets
		// laps_completed to whatever it wants and race_progress_of() below
		// takes it as always ahead, corrupting this racer's standing (and
		// HUD lap count) for every other client for the rest of the race.
		if (incoming.checkpoints_hit > Race_num_checkpoints ||
			incoming.laps_completed > Race_laps_to_win)
			continue;

		// Never rewind: our own progress (and anything we already heard
		// directly) may legitimately be ahead of this snapshot.
		if (race_progress_of(&incoming) > race_progress_of(rp))
		{
			rp->checkpoints_hit = incoming.checkpoints_hit;
			rp->laps_completed = incoming.laps_completed;
		}

		if (place && !rp->finished)
		{
			rp->finished = 1;
			rp->finish_time = GameTime64;
			rp->finish_place = place;
			race_announce_finish(i);
		}
		else if (place)
			rp->finish_place = place;
	}
}

//	-------------------------------------------------------------------------
//	Checkpoints
//	-------------------------------------------------------------------------

// How many checkpoints make up a lap. Every matcen counts, unless one of them
// is standing in as the start/finish line because the level marked none --
// then that one is the line, not a checkpoint.
int race_checkpoint_total(void)
{
	int total = Race_num_checkpoints;

	if (Race_finish_checkpoint >= 0)
		total--;

	return (total > 0) ? total : 0;
}

// Checkpoints still outstanding on the current lap. The start/finish line
// itself never counts towards the set.
static int race_checkpoints_owed(void)
{
	int i, owed = 0;

	for (i = 0; i < Race_num_checkpoints && i < 32; i++)
		if (i != Race_finish_checkpoint && !(Race_cp_mask & (1u << i)))
			owed++;


	return owed;
}

int race_checkpoint_is_target(int cp)
{
	if (cp < 0 || cp >= Race_num_checkpoints || cp >= 32)
		return 0;

	// The line only becomes a target once the set is complete; every other
	// checkpoint is a target until it has been crossed this lap.
	if (cp == Race_finish_checkpoint)
		return race_checkpoints_owed() == 0;

	return !(Race_cp_mask & (1u << cp));
}

int race_segment_is_finish(int segnum)
{
	if (segnum < 0 || segnum > Highest_segment_index)
		return 0;

	// A marked line can span several segments, so match on the marker rather
	// than on the one representative segment we recorded.
	if (Race_finish_marked)
		return Segment2s[segnum].special == SEGMENT_IS_REPAIRCEN;

	return segnum == Race_finish_segnum;
}

int race_finish_is_target(void)
{
	return race_checkpoints_owed() == 0;
}

void race_check_checkpoint(segment *segp)
{
	segment2 *seg2p;
	race_player_info *rp;
	int hit, segnum;

	if (!segp || Race_num_checkpoints <= 0 || Race_counting_down || is_observer())
		return;

	segnum = segp - Segments;

	// One crossing per entry. We are handed every segment physics moved
	// through this frame and the final one is normally repeated, so without
	// this the capture would immediately be re-read as a U-turn back over the
	// checkpoint we just took.
	if (segnum == Race_last_seg_checked)
		return;

	// A wide finish line spans several segments (race.h), and at race speed
	// phys_seglist can walk two of them in one frame. Without this, the
	// second segment re-enters the finish branch a moment after the first
	// one already completed the lap and zeroed the checkpoint mask for the
	// next one -- so it reads that fresh, empty mask as every checkpoint on
	// the *new* lap having been skipped, and reports them all "missed"
	// immediately after a clean crossing. Checkpoints don't need the same
	// guard: Race_cp_mask already makes a repeat hit on one a no-op.
	if (race_segment_is_finish(segnum) && race_segment_is_finish(Race_last_seg_checked))
	{
		Race_last_seg_checked = segnum;
		return;
	}

	Race_last_seg_checked = segnum;

	seg2p = &Segment2s[segnum];

	rp = &Race_player[Player_num];
	if (rp->finished)
		return;

	// The line is matched by segment, since it may be a repair center rather
	// than a matcen. Everything else has to be a matcen to count.
	if (!race_segment_is_finish(segnum))
	{
		hit = race_checkpoint_of_segment(segnum);
		if (hit < 0)
			return;
	}
	else
		hit = Race_finish_checkpoint;

	if (race_segment_is_finish(segnum))
	{
		char buf[48];
		char split[16];
		int owed = race_checkpoints_owed();

		// A crossing just went through, and Race_cp_mask is reset the
		// instant it does -- so a ship that lingers on the line (slowing to
		// turn, wobbling at the wall, or just drifting back through the
		// segment for a frame while peeling off onto the route) reads as a
		// second, fresh crossing with none of the new lap's checkpoints hit
		// yet, and gets told it skipped all of them. The dedup two lines up
		// only catches this within a single frame's phys_seglist walk; a
		// wobble spread over a couple of frames still slips through it,
		// which is what this grace window is for.
		if (GameTime64 < Race_finish_grace_until)
			return;

		// Driving off the grid crosses the line before a lap has been run, so
		// that first crossing only starts the clock. Once the field is
		// moving, the line closes a lap -- but only when every other
		// checkpoint on the track has been crossed.
		if (!rp->has_checkpoint && !rp->laps_completed &&
			GameTime64 - Race_start_time < i2f(5))
		{
			rp->has_checkpoint = 1;
			Race_respawn_segnum = segnum;
			Race_lap_start = GameTime64;
			Race_finish_grace_until = GameTime64 + RACE_FINISH_GRACE_TIME;
			return;
		}

		if (owed)
		{
			// Cutting the track: tell them what they skipped rather than
			// silently refusing to count the lap.
			if (GameTime64 > Race_incomplete_next_warn)
			{
				PALETTE_FLASH_ADD(30, 0, 0);
				digi_play_sample_once(SOUND_BAD_SELECTION, F1_0);
				snprintf(buf, sizeof(buf), "%d CHECKPOINT%s MISSED", owed, (owed == 1) ? "" : "S");
				race_show_banner(buf, RACE_BANNER_WARNING);
				Race_incomplete_next_warn = GameTime64 + i2f(3);
			}
			return;
		}

		PALETTE_FLASH_ADD(0, 15, 0);
		digi_play_sample_once(SOUND_HOSTAGE_RESCUED, F1_0);

		rp->has_checkpoint = 1;
		Race_respawn_segnum = segnum;
		rp->laps_completed++;
		rp->checkpoints_hit = 0;
		Race_cp_mask = 0;
		Race_finish_grace_until = GameTime64 + RACE_FINISH_GRACE_TIME;

		race_format_time(split, sizeof(split), GameTime64 - Race_lap_start);
		race_record_lap();

		if (rp->laps_completed >= Race_laps_to_win)
		{
			rp->finished = 1;
			rp->finish_time = GameTime64;
			Race_total_time = GameTime64 - Race_start_time;
			race_assign_place(Player_num);
			race_show_banner("FINISH!", RACE_BANNER_FINISH);
			Race_summary_pending = 1;
			con_printf(CON_NORMAL, "RACE: local player finished, summary pending\n");
		}
		else
		{
			snprintf(buf, sizeof(buf), "LAP %d/%d   %s", rp->laps_completed + 1, Race_laps_to_win, split);
			race_show_banner(buf, RACE_BANNER_FINISH);
		}
	}
	else
	{
		char buf[32];
		int total = race_checkpoint_total();

		if (Race_cp_mask & (1u << hit))
			return;		// already collected this lap; crossing it again is free

		PALETTE_FLASH_ADD(0, 15, 0);
		digi_play_sample_once(SOUND_HOSTAGE_RESCUED, F1_0);

		Race_cp_mask |= (1u << hit);
		rp->checkpoints_hit++;
		rp->has_checkpoint = 1;
		Race_respawn_segnum = segnum;

		snprintf(buf, sizeof(buf), "CHECKPOINT %d/%d", rp->checkpoints_hit, total);
		race_show_banner(buf, RACE_BANNER_NORMAL);
	}

	race_send_update(rp);
}

//	-------------------------------------------------------------------------
//	Respawning
//	-------------------------------------------------------------------------

// Segment of the checkpoint still owed this lap that is nearest to `from`,
// or the start/finish line once the set is complete. -1 if there is nothing
// meaningful to point at.
static int race_nearest_owed_checkpoint_seg(const vms_vector *from)
{
	int i, best_seg = -1;
	fix best = 0;

	if (Race_num_checkpoints <= 0)
		return -1;

	for (i = 0; i < Race_num_checkpoints; i++)
	{
		fix dist;

		if (!race_checkpoint_is_target(i))
			continue;		// already collected, or the line before the set is done

		dist = vm_vec_dist_quick(&Race_cp_center[i], from);

		if (best_seg < 0 || dist < best)
		{
			best = dist;
			best_seg = Race_cp_segnum[i];
		}
	}

	// Set complete: the line is what's left to aim at.
	if (best_seg < 0 && Race_finish_segnum >= 0)
		best_seg = Race_finish_segnum;

	return best_seg;
}

// Every player slot sits on its own fixed point around a ring centred on
// the respawn point, so two racers who both last touched the same
// checkpoint don't land inside each other -- see race_get_respawn() and
// race_bot_place(). Riding *orient's own rvec/uvec rather than a world axis
// keeps the spread crosswise to the tube (the way BOT_LANE_SPACING already
// keeps the starting grid crosswise) instead of along it, whichever way the
// track happens to be facing here. Not a collision query against the
// geometry or against who else is actually there -- just enough of a fixed
// deal-out that two ships sharing a checkpoint stop being a coin flip on
// whether they wreck each other on arrival. pnum 0 stays dead centre, since
// there is nothing to spread a field of one away from.
//
// *segnum is trusted by the caller for an unchecked obj_relink() (see
// gameseq.c), so the offset is only kept if find_point_seg() can confirm it
// landed somewhere real -- a checkpoint too narrow for the full spread would
// otherwise put a slot's position on one side of a wall and its segment on
// the other. Falls back to the unmoved centre rather than risk that.
#define RACE_RESPAWN_SPREAD i2f(7)

void race_spread_respawn(vms_vector *pos, const vms_matrix *orient, int pnum, int *segnum)
{
	fix angle_16, s, c;
	vms_vector spread;
	int seg;

	if (pnum <= 0 || pnum >= MAX_PLAYERS)
		return;

	angle_16 = (fix)(((long)pnum * 0x10000L) / MAX_PLAYERS) & 0xffff;
	fix_fastsincos(angle_16, &s, &c);

	spread = *pos;
	vm_vec_scale_add2(&spread, &orient->rvec, fixmul(RACE_RESPAWN_SPREAD, c));
	vm_vec_scale_add2(&spread, &orient->uvec, fixmul(RACE_RESPAWN_SPREAD, s));

	seg = find_point_seg(&spread, *segnum);

	if (seg < 0)
		return;			// too tight a fit here -- stay put rather than end up outside the world

	*pos = spread;
	*segnum = seg;
}

int race_get_respawn(vms_vector *pos, vms_matrix *orient, int *segnum)
{
	race_player_info *rp = &Race_player[Player_num];
	int seg = Race_respawn_segnum;
	int next_seg;
	vms_vector center;

	if (!rp->has_checkpoint || seg < 0 || seg > Highest_segment_index)
		return 0;

	compute_segment_center(&center, &Segments[seg]);

	*pos = center;
	*segnum = seg;
	*orient = vmd_identity_matrix;

	// Point them down the road. The lap route knows which way the track
	// actually runs here, so ask it first: aiming at the next checkpoint is a
	// straight line to a segment that may well be behind a wall, or reached by
	// going the other way round, which is how a respawn ends up facing
	// backwards on a track that doubles back on itself.
	{
		vms_vector fvec;

		if (race_route_direction(&center, seg, &fvec))
		{
			vm_vector_2_matrix(orient, &fvec, NULL, NULL);
			race_spread_respawn(pos, orient, Player_num, segnum);
			return 1;
		}
	}

	// No route on this track: fall back to aiming at whatever checkpoint they
	// still owe. Rough, but better than dropping them in facing a wall.
	next_seg = race_nearest_owed_checkpoint_seg(&center);

	if (!rp->finished && next_seg >= 0 && next_seg <= Highest_segment_index)
	{
		vms_vector next_center, fvec;

		compute_segment_center(&next_center, &Segments[next_seg]);
		vm_vec_sub(&fvec, &next_center, &center);

		if (fvec.x || fvec.y || fvec.z)
			vm_vector_2_matrix(orient, &fvec, NULL, NULL);
	}

	race_spread_respawn(pos, orient, Player_num, segnum);
	return 1;
}

//	-------------------------------------------------------------------------
//	Standings and HUD helpers
//	-------------------------------------------------------------------------

// Distance from player pnum's ship to the center of checkpoint segment
// `checkpoint`, or 0 if that checkpoint index isn't a valid matcen.
// Straight-line distance from player pnum to whatever they still owe. Only a
// tiebreak between players on equal progress, so an approximation is fine --
// and for remote players we only know their count, not their set, so we
// measure against our own outstanding list.
static fix race_dist_to_owed(int pnum)
{
	int objnum = Players[pnum].objnum;
	int seg;
	vms_vector center;
	fix bot_dist;

	// A bot keeps its own checkpoint set, so it can answer this exactly. For
	// everyone else the outstanding set below is the local player's, which
	// makes this an estimate -- fine, it is only a tiebreak between two
	// racers on the same checkpoint count.
	bot_dist = race_bot_dist_to_next(pnum);

	if (bot_dist >= 0)
		return bot_dist;

	if (objnum < 0 || objnum > Highest_object_index)
		return 0;

	seg = race_nearest_owed_checkpoint_seg(&Objects[objnum].pos);
	if (seg < 0)
		return 0;

	compute_segment_center(&center, &Segments[seg]);

	return vm_vec_dist_quick(&center, &Objects[objnum].pos);
}			

typedef struct race_rank_entry {
	int		pnum;
	int		finished;
	int		place;			// host-assigned finishing order; 0 if unknown
	fix64	finish_time;
	int		progress;		// laps*checkpoints + checkpoints_hit; higher = further along
	int		route;			// how far round the lap route; higher = further along, -1 = unknown
	fix		dist_to_next;	// last-resort tiebreak, when there is no route to read
} race_rank_entry;

static int race_rank_cmp(const void *a, const void *b)
{
	const race_rank_entry *ra = (const race_rank_entry *)a;
	const race_rank_entry *rb = (const race_rank_entry *)b;

	if (ra->finished != rb->finished)
		return rb->finished - ra->finished;

	if (ra->finished)
	{
		// Host-assigned places are the authority; fall back to local receipt
		// time only until the host's snapshot has told us the order.
		if (ra->place && rb->place)
			return ra->place - rb->place;

		if (ra->finish_time != rb->finish_time)
			return (ra->finish_time < rb->finish_time) ? -1 : 1;
		return 0;
	}

	if (ra->progress != rb->progress)
		return rb->progress - ra->progress;

	// Same checkpoint count, so split them on where they actually are round
	// the lap. Checkpoints are a set and can be taken in any order, which
	// means two racers on the same count can be most of a lap apart -- and
	// "whose next checkpoint is nearer" then reads them in whatever order
	// their outstanding sets happen to fall, which is the wobble in the
	// standings. The route index is one ordered walk of the track, so it
	// answers the question properly whenever there is a route to read.
	if (ra->route >= 0 && rb->route >= 0 && ra->route != rb->route)
		return rb->route - ra->route;

	if (ra->dist_to_next != rb->dist_to_next)
		return (ra->dist_to_next < rb->dist_to_next) ? -1 : 1;

	return 0;
}

int race_get_positions(int *sorted)
{
	race_rank_entry entries[MAX_PLAYERS];
	int i, n = N_players;

	if (n > MAX_PLAYERS)
		n = MAX_PLAYERS;

	for (i = 0; i < n; i++)
	{
		race_player_info *rp = &Race_player[i];

		entries[i].pnum = i;
		entries[i].finished = rp->finished;
		entries[i].place = rp->finish_place;
		entries[i].finish_time = rp->finish_time;
		entries[i].progress = race_progress_of(rp);
		entries[i].route = rp->finished ? -1 : race_route_progress(i);
		entries[i].dist_to_next = rp->finished ? 0 : race_dist_to_owed(i);
	}

	qsort(entries, n, sizeof(race_rank_entry), race_rank_cmp);

	for (i = 0; i < n; i++)
		sorted[i] = entries[i].pnum;

	return n;
}

int race_get_rank(int pnum)
{
	int sorted[MAX_PLAYERS];
	int n = race_get_positions(sorted);
	int i;

	for (i = 0; i < n; i++)
		if (sorted[i] == pnum)
			return i + 1;

	return 0;
}

//	-------------------------------------------------------------------------
//	Earthshaker targeting
//	-------------------------------------------------------------------------

// A shaker is the heaviest thing in the loot table and it is weighted at the
// back of the field, so in a race it is the catch-up weapon. Homing on the
// nearest ship made it a weapon against whoever happened to be alongside --
// usually another back-marker. Here it always flies at whoever is leading.
//
// If the leader is the one who fired it, it takes the next player down the
// order instead: a missile that turned round on its own launcher would just
// be a way to lose the race.
int race_homing_target(const object *tracker)
{
	// One shaker bursts into a cloud of blobs and every one of them asks this
	// question on every frame, so the standings are worked out once a frame
	// and shared -- ranking walks every player's outstanding checkpoints.
	static fix64 ranked_at = 0;
	static int sorted[MAX_PLAYERS];
	static int ranked_n = 0;
	int n, i, parent = -1;

	if (!(Game_mode & GM_RACE) || !tracker || tracker->type != OBJ_WEAPON)
		return -1;

	// The missile itself and the mega blobs it bursts into, so the whole
	// spread converges on the leader rather than scattering to the nearest.
	if (tracker->id != EARTHSHAKER_ID && tracker->id != EARTHSHAKER_MEGA_ID)
		return -1;

	if (tracker->ctype.laser_info.parent_type == OBJ_PLAYER)
		parent = tracker->ctype.laser_info.parent_num;

	// Whoever fired it is out of the running whatever the standings say, and
	// so is anything else the engine considers related to it (create_smart_
	// children hands the burst the missile's own parent, but a chain that has
	// been through a recycled object slot can still come back wrong). A
	// missile that turns round on its own launcher is not a catch-up weapon,
	// it is a way to lose the race.
	if (parent >= 0 && parent <= Highest_object_index &&
		Objects[parent].signature != tracker->ctype.laser_info.parent_signature)
		parent = -1;

	if (!ranked_at || ranked_at != GameTime64)
	{
		ranked_n = race_get_positions(sorted);
		ranked_at = GameTime64;
	}

	n = ranked_n;

	for (i = 0; i < n; i++)
	{
		int pnum = sorted[i];
		int objnum;

		if (pnum < 0 || pnum >= MAX_PLAYERS)
			continue;

		if ((Game_mode & GM_MULTI) && Players[pnum].connected != CONNECT_PLAYING)
			continue;

		objnum = Players[pnum].objnum;

		if (objnum < 0 || objnum > Highest_object_index)
			continue;
		if (objnum == parent)
			continue;			// the leader fired it; take the next one down
		if (laser_are_related(tracker - Objects, objnum))
			continue;			// ...and never anything else it came out of
		if (Objects[objnum].type != OBJ_PLAYER)
			continue;
		if (Objects[objnum].flags & OF_SHOULD_BE_DEAD)
			continue;
		if (object_is_observer(&Objects[objnum]))
			continue;

		return objnum;
	}

	return -1;
}

// -------------------------------------------------------------------------
// Earthshaker corridor steering
// -------------------------------------------------------------------------
//
// race_homing_target() above picks *who* to aim at; this decides *where in
// space* to point the missile so it actually gets there. Steering straight
// at the leader's position made a shaker that turned a corner just fly into
// the wall at the corner -- and detonate on whoever else was standing near
// it, not on the leader it was chasing. Routed through the same BFS
// segment-graph pathfinder the racebots steer their own laps with
// (aipath.c's create_path_points -- see racebot.c's race_route_path() for
// the pattern this follows).
//
// Unlike racebot.c, which builds each leg's path once at level load and
// keeps it for the whole race, a shaker's path has to be built live: the
// leader it's chasing keeps moving, and there's no way to know in advance
// which segment a missile will be fired from. create_path_points does an
// O(segment count) BFS over a several-thousand-segment level and stack-
// allocates megabyte-scale visited/queue arrays to do it, so it is not
// something to call every frame for every blob in an earthshaker burst --
// it's cached per missile here and only rebuilt when it goes stale.
#define RACE_SHAKER_PATH_SLOTS   16
#define RACE_SHAKER_PATH_REFRESH (F1_0 * 2)   // how long a cached path is trusted
#define RACE_SHAKER_ARRIVE_DIST  (F1_0 * 20)  // close enough to a waypoint to advance past it

// create_path_points() can return max_depth + 1 points (the segment at the
// depth cap plus the start segment) -- a buffer sized exactly
// MAX_SEGMENTS_PER_PATH is one short. Learned the hard way: sized exactly
// to MAX_SEGMENTS_PER_PATH with safety_flag set (see race_shaker_aim_point())
// wrote past the end of this array and corrupted the stack.
#define RACE_SHAKER_PATH_CAPACITY (MAX_SEGMENTS_PER_PATH + 1)

typedef struct {
	int         signature;      // 0 == slot free
	fix64       computed_at;
	short       target_seg;
	short       n;
	short       idx;
	point_seg   pts[RACE_SHAKER_PATH_CAPACITY];
} race_shaker_path;

static race_shaker_path Race_shaker_paths[RACE_SHAKER_PATH_SLOTS];

// The slot for missile `signature` -- its own if it already has one, an
// empty one, or (the burst is bigger than RACE_SHAKER_PATH_SLOTS) whichever
// slot was computed longest ago. Never fails: worst case a fast-moving
// burst thrashes slots and some blobs re-path more often than
// RACE_SHAKER_PATH_REFRESH would otherwise call for.
static race_shaker_path *race_shaker_path_slot(int signature)
{
	int i, oldest = 0;
	fix64 oldest_time = Race_shaker_paths[0].computed_at;

	for (i = 0; i < RACE_SHAKER_PATH_SLOTS; i++)
	{
		if (Race_shaker_paths[i].signature == signature || !Race_shaker_paths[i].signature)
			return &Race_shaker_paths[i];

		if (Race_shaker_paths[i].computed_at < oldest_time)
		{
			oldest_time = Race_shaker_paths[i].computed_at;
			oldest = i;
		}
	}

	return &Race_shaker_paths[oldest];
}

// Fills *aim with the next waypoint the cached (or freshly built) path says
// to steer at, advancing past any waypoint already reached. Returns 0 and
// leaves *aim alone when no path exists -- an unreachable target, or the
// pathfinder came back empty -- so the caller can fall back to aiming
// straight at the target, same as before this existed.
static int race_shaker_aim_point(object *tracker, int target_objnum, vms_vector *aim)
{
	race_shaker_path *p;
	object *target = &Objects[target_objnum];
	point_seg local_pts[RACE_SHAKER_PATH_CAPACITY];
	short n;

	if (tracker->segnum < 0 || target->segnum < 0)
		return 0;

	p = race_shaker_path_slot(tracker->signature);

	// Rebuild when this slot belongs to a different missile (evicted or
	// never claimed), the cached path has gone stale, or the leader has
	// moved into a different segment than the path was built for -- a path
	// to where they used to be just walks the missile into the wall they
	// turned behind.
	if (p->signature != tracker->signature ||
		!p->computed_at || GameTime64 - p->computed_at > RACE_SHAKER_PATH_REFRESH ||
		p->target_seg != target->segnum)
	{
		// safety_flag off: it tells create_path_points() to splice extra
		// midpoints in after the fact (insert_center_points()), and that
		// function's own overflow guard only checks room in the engine's
		// shared Point_segs pool -- not a caller-supplied buffer like this
		// one, which is what let it write past the end of a fixed-size
		// array here. Not needed anyway: that's for a robot physically
		// steering along the path, not for picking a waypoint to aim a
		// missile at.
		if (create_path_points(tracker, tracker->segnum, target->segnum,
								local_pts, &n, MAX_SEGMENTS_PER_PATH, 0, 0, -1) == -1 ||
			n < 1)
		{
			p->signature = 0;		// give the slot back; nothing to cache
			return 0;
		}

		// Belt and suspenders against the buffer this actually filled: even
		// with safety_flag off, don't trust n past what local_pts can hold.
		if (n > RACE_SHAKER_PATH_CAPACITY)
			n = RACE_SHAKER_PATH_CAPACITY;

		p->signature = tracker->signature;
		p->computed_at = GameTime64;
		p->target_seg = target->segnum;
		p->n = n;
		p->idx = 0;
		memcpy(p->pts, local_pts, n * sizeof(point_seg));
	}

	while (p->idx < p->n - 1 &&
		   vm_vec_dist_quick(&tracker->pos, &p->pts[p->idx].point) < RACE_SHAKER_ARRIVE_DIST)
		p->idx++;

	*aim = p->pts[p->idx].point;
	return 1;
}

void race_homing_aim_point(object *tracker, int target_objnum, vms_vector *aim)
{
	*aim = Objects[target_objnum].pos;

	if (target_objnum < 0 || target_objnum > Highest_object_index)
		return;

	race_shaker_aim_point(tracker, target_objnum, aim);
}

// How fast a weapon is allowed to fly. Stock everywhere except a homing
// missile in a race: a homer's whole job is to run down a ship that is already
// flat out, and at stock speed it spends most of its short life trailing the
// racer it was fired at and expiring behind them. The one weapon class that
// exists to catch somebody gets the speed to actually do it.
//
// Applied at the call sites in laser.c rather than by editing Weapon_info,
// which is shared game data and would leak the change into every other mode.
fix race_weapon_speed(int weapon_id)
{
	fix speed;

	if (weapon_id < 0 || weapon_id >= MAX_WEAPON_TYPES)
		return 0;

	speed = Weapon_info[weapon_id].speed[Difficulty_level];

	if (!(Game_mode & GM_RACE))
		return speed;

	// The shaker is included even though the HAM never marked it as a homer,
	// because race mode flies it as one -- see race_force_homing().
	if (!Weapon_info[weapon_id].homing_flag && weapon_id != EARTHSHAKER_ID)
		return speed;

	return fixmul(speed, RACE_HOMING_SPEED);
}

// The HAM does not mark the shaker itself as a homer -- only the blobs it
// bursts into -- so in race mode the missile is flown by the homing code
// anyway. Everywhere else it flies exactly as it always has.
int race_force_homing(const object *obj)
{
	return (Game_mode & GM_RACE) && obj && obj->type == OBJ_WEAPON &&
		obj->id == EARTHSHAKER_ID;
}

int race_get_banner(char *buf, int bufsz, int *style)
{
	if (GameTime64 > Race_banner_until)
		return 0;

	strncpy(buf, Race_banner_text, bufsz - 1);
	buf[bufsz - 1] = 0;

	if (style)
		*style = Race_banner_style;

	return 1;
}

int race_get_next_checkpoint_vec(vms_vector *dir, fix *dist)
{
	race_player_info *rp = &Race_player[Player_num];
	int segnum;
	vms_vector center;

	if (Race_num_checkpoints <= 0 || rp->finished)
		return 0;

	segnum = race_nearest_owed_checkpoint_seg(&ConsoleObject->pos);
	if (segnum < 0)
		return 0;

	compute_segment_center(&center, &Segments[segnum]);
	*dist = vm_vec_normalized_dir(dir, &center, &ConsoleObject->pos);

	return 1;
}
