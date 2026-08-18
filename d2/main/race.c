/*
 * Race game mode -- checkpoints, laps, mystery boxes, boost pads and race
 * presentation. See race.h for the design rationale.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "race.h"
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

race_player_info Race_player[MAX_PLAYERS];
int Race_num_checkpoints = 0;
int Race_finish_checkpoint = -1;	// matcen acting as the line, or -1 if a goal segment is
int Race_finish_segnum = -1;
int Race_finish_marked = 0;			// level marks its line with repair centers
static int Race_respawn_segnum = -1;	// segment of the last checkpoint the local player took
int Race_num_boxes = 0;
int Race_laps_to_win = RACE_DEFAULT_LAPS;

static fix Race_countdown_timer = 0;	// seconds remaining, fix; only meaningful while Race_counting_down
static int Race_counting_down = 0;
static int Race_go_announced = 0;
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

// What a box can hand out. The eight missiles below are always in the table;
// anything the level itself has lying around gets absorbed into it too, so a
// track built with vulcans and plasma on the floor still offers them -- just
// out of a box on a fuse instead of as a permanent pickup.
typedef struct race_item {
	ubyte		wclass;		// CLASS_PRIMARY / CLASS_SECONDARY
	ubyte		index;
	ubyte		weight;		// relative odds of this entry coming up
	const char	*name;
} race_item;

// The standing missile table is what a race is meant to feel like, so it stays
// common no matter how much hardware a level happens to have lying around.
// Without this, absorbing a level's guns quietly made megas and shakers rarer
// every time the pool grew.
#define RACE_WEIGHT_BASE        6
#define RACE_WEIGHT_HARVESTED   1

#define RACE_MAX_ITEMS (MAX_PRIMARY_WEAPONS + MAX_SECONDARY_WEAPONS)

static const char *const Race_primary_names[MAX_PRIMARY_WEAPONS] = {
	"LASER", "VULCAN", "SPREADFIRE", "PLASMA", "FUSION",
	"SUPER LASER", "GAUSS", "HELIX", "PHOENIX", "OMEGA"
};

static const char *const Race_secondary_names[MAX_SECONDARY_WEAPONS] = {
	"CONCUSSION", "HOMING MISSILE", "PROXIMITY BOMB", "SMART MISSILE", "MEGA MISSILE",
	"FLASH MISSILE", "GUIDED MISSILE", "SMART MINE", "MERCURY MISSILE", "EARTHSHAKER"
};

static race_item Race_item_pool[RACE_MAX_ITEMS];
static int Race_num_items = 0;

// GameTime64 at which each held weapon evaporates; 0 = not held from a box.
static fix64 Race_expire_pri[MAX_PRIMARY_WEAPONS];
static fix64 Race_expire_sec[MAX_SECONDARY_WEAPONS];

const char *race_item_name(int wclass, int index)
{
	if (wclass == CLASS_PRIMARY)
		return (index >= 0 && index < MAX_PRIMARY_WEAPONS) ? Race_primary_names[index] : "WEAPON";

	return (index >= 0 && index < MAX_SECONDARY_WEAPONS) ? Race_secondary_names[index] : "WEAPON";
}

static void race_add_item(int wclass, int index, int weight)
{
	int i;

	for (i = 0; i < Race_num_items; i++)
		if (Race_item_pool[i].wclass == wclass && Race_item_pool[i].index == index)
			return;		// already offered

	if (Race_num_items >= RACE_MAX_ITEMS)
		return;

	Race_item_pool[Race_num_items].wclass = (ubyte)wclass;
	Race_item_pool[Race_num_items].index = (ubyte)index;
	Race_item_pool[Race_num_items].weight = (ubyte)weight;
	Race_item_pool[Race_num_items].name = race_item_name(wclass, index);
	Race_num_items++;
}

// Weighted draw from the loot table.
static const race_item *race_pick_item(void)
{
	int total = 0, roll, i;

	for (i = 0; i < Race_num_items; i++)
		total += Race_item_pool[i].weight;

	if (total <= 0)
		return NULL;

	roll = d_rand() % total;

	for (i = 0; i < Race_num_items; i++)
	{
		roll -= Race_item_pool[i].weight;
		if (roll < 0)
			return &Race_item_pool[i];
	}

	return &Race_item_pool[Race_num_items - 1];
}

// The eight missiles every race offers regardless of what the level holds.
static void race_init_items(void)
{
	static const ubyte base[] = {
		CONCUSSION_INDEX, HOMING_INDEX, PROXIMITY_INDEX, SMART_INDEX,
		MEGA_INDEX, SMART_MINE_INDEX, SMISSILE4_INDEX, SMISSILE5_INDEX
	};
	int i;

	Race_num_items = 0;
	memset(Race_expire_pri, 0, sizeof(Race_expire_pri));
	memset(Race_expire_sec, 0, sizeof(Race_expire_sec));

	for (i = 0; i < (int)(sizeof(base)/sizeof(base[0])); i++)
		race_add_item(CLASS_SECONDARY, base[i], RACE_WEIGHT_BASE);
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

		race_add_item(wclass, index, RACE_WEIGHT_HARVESTED);
		Objects[i].flags |= OF_SHOULD_BE_DEAD;
	}
}

// Which powerup bitmap stands for this weapon on the HUD.
int race_item_powerup(int wclass, int index)
{
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
	if (wclass == CLASS_SECONDARY)
		return Players[Player_num].secondary_ammo[index];

	// Vulcan/gauss rounds are stored scaled, the same way the cockpit gauge
	// reads them out.
	if (index == VULCAN_INDEX || index == GAUSS_INDEX)
		return f2i((unsigned)VULCAN_AMMO_SCALE * (unsigned)Players[Player_num].primary_ammo[VULCAN_INDEX]);

	return 1;	// energy weapons have no ammo count to show
}

// Drop a weapon out of the rack, and move the player off it if it was the one
// they had selected.
static void race_drop_item(int wclass, int index)
{
	player *p = &Players[Player_num];

	if (wclass == CLASS_SECONDARY)
	{
		Race_expire_sec[index] = 0;
		p->secondary_ammo[index] = 0;
		p->secondary_weapon_flags &= ~HAS_FLAG(index);

		if (p->secondary_weapon == index)
			auto_select_weapon(1);

		return;
	}

	Race_expire_pri[index] = 0;
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
	// The engine's own super split in both racks: primaries from the super
	// laser up, secondaries from the flash missile up.
	fix64 life = (index >= (wclass == CLASS_PRIMARY ? SUPER_LASER_INDEX : SUPER_WEAPON))
		? RACE_ITEM_SUPER_TIME : RACE_ITEM_NORMAL_TIME;

	if (wclass == CLASS_SECONDARY)
	{
		if (p->secondary_ammo[index] < Secondary_ammo_max[index])
			p->secondary_ammo[index]++;

		p->secondary_weapon_flags |= HAS_FLAG(index);

		// A fresh pickup restarts the fuse rather than stacking onto it, so
		// camping boxes can't bank an indefinite arsenal.
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

	// Should never be empty -- race_init_level() seeds the eight missiles --
	// but a roll that silently hands out nothing is invisible to the player
	// and maddening to diagnose, so rebuild rather than return.
	if (!Race_num_items)
		race_init_items();

	// One item is the common case; three is a treat.
	if (roll >= 60)
		rolls = (roll >= 90) ? 3 : 2;

	for (i = 0; i < rolls; i++)
	{
		const race_item *item = race_pick_item();
		fix64 life;

		if (!item)
			break;

		race_grant_item(item->wclass, item->index);
		life = race_get_item_remaining(item->wclass, item->index);

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

	Race_boost_until = GameTime64 + RACE_BOOST_TIME;
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
	// Up to 2.25x forward thrust, a bit stronger than the afterburner's 2x.
	return F1_0 + fixmul(race_boost_strength(), F1_0 + F1_0/4);
}

void race_cancel_boost(void)
{
	if (GameTime64 < Race_boost_until)
	{
		Race_boost_until = 0;
		Race_boost_started = 0;
	}
}

fix race_get_fov_bonus(void)
{
	// Render_zoom is 0x9000 by default and 0x11000 at the "wide" end of the
	// FOV slider, so 0x2000 is a noticeable but not disorienting widening.
	return fixmul(race_boost_strength(), 0x2000);
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

	Race_next_place = 1;
	Race_banner_until = 0;
	Race_boost_until = 0;
	Race_boost_started = 0;
	Race_last_boost_seg = -1;
	Race_last_seg_checked = -1;
	Race_next_state_send = 0;	// GameTime64 restarts each level; don't stall the broadcast
	Race_start_time = 0;
	Race_lap_start = 0;
	Race_total_time = 0;
	Race_best_lap = 0;
	Race_num_splits = 0;
	Race_summary_pending = 0;

	// The countdown runs on every race level, including ones with no
	// checkpoints placed -- it is the start of the race, not a property of
	// the track layout.
	Race_countdown_timer = i2f(RACE_COUNTDOWN_SECONDS);
	Race_counting_down = 1;
	Race_go_announced = 0;

	race_init_boxes();
	race_init_labels();
	race_init_items();
}

void race_frame(void)
{
	if (Race_counting_down)
	{
		Race_countdown_timer -= FrameTime;
		if (Race_countdown_timer <= 0)
		{
			Race_countdown_timer = 0;
			Race_counting_down = 0;
		}
	}

	if (!Race_counting_down && !Race_go_announced)
	{
		Race_go_announced = 1;
		Race_start_time = GameTime64;
		Race_lap_start = GameTime64;
		HUD_init_message_literal(HM_MULTI, "GO!");
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

	if (incoming.checkpoints_hit > Race_num_checkpoints)
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
#define RACE_STATE_LEN	(2 + 3*MAX_PLAYERS)

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
	count = 2;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		multibuf[count++] = Race_player[i].checkpoints_hit;
		multibuf[count++] = Race_player[i].laps_completed;
		multibuf[count++] = Race_player[i].finish_place;
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

	// Tight cadence while counting down (the countdown is only 3 seconds and
	// every client needs to agree on when it ends), lazy afterwards.
	Race_next_state_send = GameTime64 + (Race_counting_down ? F1_0/4 : F1_0*2);
}

void multi_do_race_state(const ubyte *buf)
{
	int i, count;
	fix host_countdown;

	if (!(Game_mode & GM_RACE) || multi_i_am_master())
		return;

	host_countdown = i2f(buf[1]) / 10;

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

	count = 2;
	for (i = 0; i < MAX_PLAYERS; i++)
	{
		race_player_info *rp = &Race_player[i];
		race_player_info incoming = *rp;
		ubyte place;

		incoming.checkpoints_hit = buf[count++];
		incoming.laps_completed = buf[count++];
		place = buf[count++];

		if (incoming.checkpoints_hit > Race_num_checkpoints)
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

	// Face the checkpoint they are heading for next, so a respawn doesn't
	// dump them into the track pointing at a wall.
	next_seg = race_nearest_owed_checkpoint_seg(&center);
	if (!rp->finished && next_seg >= 0)
	{
		if (next_seg >= 0 && next_seg <= Highest_segment_index)
		{
			vms_vector next_center, fvec;

			compute_segment_center(&next_center, &Segments[next_seg]);
			vm_vec_sub(&fvec, &next_center, &center);
			if (fvec.x || fvec.y || fvec.z)
				vm_vector_2_matrix(orient, &fvec, NULL, NULL);
		}
	}

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
	fix		dist_to_next;	// tiebreak for players between the same two checkpoints
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
