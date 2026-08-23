/*
 * Race game mode -- checkpoints, laps, mystery boxes, boost pads and race
 * presentation.
 *
 * Everything a race track needs is expressed with segment "special" types the
 * level format already has, so no new segment type, level format change or
 * editor support is needed. Race levels simply never wire triggers up to any
 * of them, which is what keeps their vanilla behaviour from firing:
 *
 *   SEGMENT_IS_REPAIRCEN  (repair center) -> start/finish line
 *   SEGMENT_IS_ROBOTMAKER (matcen)        -> checkpoint
 *   SEGMENT_IS_FUELCEN    (energy center) -> boost pad
 *   SEGMENT_IS_GOAL_BLUE/RED (CTF goal)   -> mystery box spawn point
 *
 * (An energy powerup dropped in the level also becomes a mystery box, which is
 * usually easier than marking a segment.)
 *
 * Checkpoints: matcen segments are already invisible/non-rendered and already
 * carry a small sequential ID (matcen_num). Robot maker segments only spawn
 * robots when explicitly triggered by a level trigger (trigger_matcen()), so
 * reusing them here does not risk spawning robots.
 *
 * Nothing depends on matcen numbering. matcen_num is handed out as the mine
 * loads, in segment index order (see create_matcen), so it matches neither the
 * order a mapper placed them nor the order they are driven, and a mapper has
 * no way to aim for a particular number. So: the start/finish line is marked
 * explicitly with repair centers, falling back to the checkpoint nearest the
 * starting grid if the level marks none. A lap is
 * complete once every checkpoint has been crossed at least once, in any
 * order, and the line is crossed after that. That makes a track work however
 * its matcens happen to be numbered, and removes "wrong way" false alarms on
 * crossovers and shortcuts.
 *
 * Boost pads: fuel centers are visually distinct in-game already. In race
 * mode their energy-refuel behaviour is suppressed (see object_move_one) and
 * flying through one gives a timed speed + FOV boost instead.
 *
 * Start/finish line: repair centers do nothing in D2 (they were dropped after
 * D1) and race mode suppresses what little they did, but they still draw with
 * their own distinctive texture, so the line reads as a real feature of the
 * track rather than an invisible trigger. Every repair center on the level is
 * part of the line, so a wide finish can span several segments.
 *
 * Mystery boxes: CTF goal segments are inert here -- nothing ever sets
 * GM_CAPTURE in race mode -- so they are a free marker. Each one holds a
 * floating pickup orb that respawns shortly after being taken.
 *
 * Levels with no matcens simply never award laps, so GM_RACE degrades to a
 * harmless free-for-all rather than soft-locking a mismatched map. The start
 * countdown runs on every level in race mode regardless.
 *
 * Netcode: each client is authoritative over its own checkpoint progress and
 * broadcasts it reliably (MULTI_RACE_UPDATE). The host is authoritative over
 * the start countdown and over finishing order, and rebroadcasts the whole
 * race state periodically (MULTI_RACE_STATE) so dropped packets and late
 * joiners heal on their own. Mystery box pickups are broadcast reliably
 * (MULTI_RACE_BOX) and applied idempotently. Boost pads are purely local.
 */

#ifndef _RACE_H
#define _RACE_H

#include "player.h"
#include "segment.h"
#include "object.h"
#include "vecmat.h"

#define RACE_DEFAULT_LAPS       3
#define RACE_COUNTDOWN_SECONDS  3	// "3, 2, 1, GO"

#define RACE_MAX_BOXES          64
#define RACE_BOX_RESPAWN_TIME   F1_0		// mystery box is back 1 second after being taken
#define RACE_BOOST_TIME         (F1_0*3)	// boost pad lasts 3 seconds

// The start of a race: everyone, human and bot, gets a few seconds where
// nothing can wreck them and a boost-pad-strength push off the line, so the
// scrum through the first corner off a stacked grid isn't an insurance
// write-off for whoever got bumped.
#define RACE_START_INVULN_TIME  (F1_0*3)
#define RACE_START_BOOST_TIME   (F1_0*3)

// Trichording: pushing forward, sideways and vertical thrust all at once is
// what let Descent's original physics fly faster than a forward-only stick
// ever could -- the axes are added as vectors, not capped as one. This pays
// out an extra speed bonus on top of that for holding a genuinely balanced,
// near-full-deflection push across all three axes at once, the same idea as
// "splitstream" tech in other mods. `ratio` is 0..F1_0: how far the smallest
// of the three axes is from the largest, so 0 is a single-axis push and F1_0
// is a perfectly even three-way split. Below RACE_TRICHORD_FLOOR nothing is
// paid out -- a graze past a diagonal shouldn't quietly become the new
// normal speed -- and it ramps to the full bonus at a true, sustained
// diagonal.
#define RACE_TRICHORD_FLOOR     ((F1_0*6)/10)
#define RACE_TRICHORD_FOV       0x2c00			// FOV widening at full strength -- wider than the boost pad's
												// own 0x2000, since holding a perfect trichord is the harder
												// trick and should read as the bigger event of the two
// How fast the FOV widening chases the trichord strength, in full swings per
// second. A perfect diagonal is easy to clip in and out of for a frame at a
// time; easing this fast keeps every little wobble reading as a flicker
// rather than the screen visibly snapping in and out.
#define RACE_TRICHORD_FOV_EASE  (F1_0*5)

// Holding the floor charges a bar (0..F1_0). No boost applies while it
// charges -- when it hits full it fires a RACE_TRICHORD_BOOST_TIME burst,
// resets to zero, and charges again. Losing the angle wipes the charge
// instantly. One mechanic for the whole field: the local player and every
// bot each keep their own charge meter and boost timer.
#define RACE_TRICHORD_CHARGE_TIME       ((F1_0*5)/2)	// ~2.5s to fill at a perfect diagonal
#define RACE_TRICHORD_CHARGE_BONUS      (F1_0/2)	// +50% forward burst
#define RACE_TRICHORD_BOOST_TIME        (F1_0*3)	// how long the burst lasts
#define RACE_TRICHORD_BLOBS             6			// HUD resolution: dings race_note_trichord() fires as charge builds

// Advances a charge meter (0..F1_0) by one frame off this frame's trichord
// ratio. When the charge reaches F1_0 it fires: `boost_until` is set to
// GameTime64 + RACE_TRICHORD_BOOST_TIME and the charge resets to 0.
// `charge` and `boost_until` belong to the caller (Race_trichord_charge /
// Race_trichord_boost_until for the local player, race_bot fields per bot).
fix race_trichord_advance_charge(fix charge, fix ratio, fix64 *boost_until);

// Thrust multiplier from the boost timer: F1_0+RACE_TRICHORD_CHARGE_BONUS
// while boost_until is in the future, F1_0 otherwise.
fix race_trichord_scale_from_boost(fix64 boost_until);

// Local player only: notes this frame's trichord ratio (see
// race_trichord_advance_charge() above) so race_get_fov_bonus() can widen
// the view to match, and so Race_trichord_charge (behind
// race_trichord_charge_scale() below) can build or drain. Call once a frame
// from read_flying_controls(); anything that isn't a decision to trichord
// (dying, the class lobby, the countdown) should note a ratio of 0.
void race_note_trichord(fix ratio);

// The same eased 0..F1_0 strength the FOV widening rides, for the HUD to
// draw a readout off of (see race_draw_trichord() in gauges.c).
fix race_trichord_strength(void);

// Local player only: 0..F1_0, the current charge fill. Returns F1_0 while a
// boost burst is active so the HUD bar stays full during the burst.
fix race_trichord_charge(void);

// Local player only: thrust multiplier from the active boost timer.
// What controls.c actually scales thrust by.
fix race_trichord_charge_scale(void);

#define RACE_MAX_CHECKPOINTS    32	// limited by the per-lap capture bitmask
#define RACE_MAX_LABELS         64	// floating in-world track labels
#define RACE_MAX_SPLITS         16	// per-lap times we keep for the local player

// Kinds of floating label, which pick the text and colour.
#define RACE_LABEL_CHECKPOINT   0
#define RACE_LABEL_FINISH       1
#define RACE_LABEL_BOOST        2

typedef struct race_label {
	vms_vector	pos;		// centre of the merged group of segments
	int			kind;		// RACE_LABEL_*
	int			number;		// checkpoint number, for RACE_LABEL_CHECKPOINT
} race_label;

// How the centre-screen banner should be tinted.
#define RACE_BANNER_NORMAL      0	// checkpoint
#define RACE_BANNER_FINISH      1	// new lap / race finished
#define RACE_BANNER_WARNING     2	// wrong way

typedef struct race_player_info {
	ubyte	checkpoints_hit;	// how many checkpoints captured on the current lap
	ubyte	laps_completed;
	ubyte	finished;
	ubyte	finish_place;		// 1-based finishing order, host-assigned; 0 until finished
	ubyte	last_checkpoint;	// matcen_num of the last checkpoint captured
	ubyte	has_checkpoint;		// 0 until this player has captured any checkpoint
	fix64	finish_time;
} race_player_info;

extern race_player_info Race_player[MAX_PLAYERS];
extern int Race_num_checkpoints;	// total checkpoints on this level (0 = none placed)
extern int Race_finish_checkpoint;	// matcen acting as the line, or -1 if the level marks one
extern int Race_finish_segnum;		// a segment of the start/finish line, -1 if the level has none
extern int Race_finish_marked;		// level marks its line with repair centers
extern int Race_num_boxes;			// total mystery box spawn points on this level
extern int Race_laps_to_win;
extern int Race_powerup_chance;	// 0-100: odds a mystery box or bot draw yields anything at all (100 = always)
extern int Race_allowed_items;		// bitmask of RACE_ITEM_* slots the loot table may offer (see below)

// Mystery-box loot table slots, for the "allowed powerups" advanced race
// option. Bit i of Race_allowed_items enables/disables that slot;
// race_init_items() (race.c) is the only reader. Order matches the box's own
// standing missile table plus the two box powers.
#define RACE_ITEM_HOMING        0
#define RACE_ITEM_SMART         1
#define RACE_ITEM_MERCURY       2
#define RACE_ITEM_MEGA          3
#define RACE_ITEM_EARTHSHAKER   4
#define RACE_ITEM_EMP           5
#define RACE_ITEM_TRACTOR       6
#define RACE_NUM_ITEM_SLOTS     7
#define RACE_ALLOWED_ITEMS_ALL  ((1 << RACE_NUM_ITEM_SLOTS) - 1)

// Resets all players' race progress, (re)counts checkpoints for the level
// that was just loaded, spawns the mystery boxes and starts the start-line
// countdown. Call once per level start when GM_RACE.
void race_init_level(void);

// Call every frame while GM_RACE is active. Advances the start-line
// countdown, the boost timer and mystery box respawns.
void race_frame(void);

// Call whenever the local player's ship enters a new segment while
// GM_RACE is active. Advances the local player's checkpoint/lap state
// and networks the update if segp is the next checkpoint they need; warns
// (rate-limited) if it's a checkpoint but the wrong one.
void race_check_checkpoint(segment *segp);

// Call once per frame from multi_do_frame() while GM_RACE is active. Sends
// the host's authoritative race state broadcast when it is due.
void race_multi_frame(void);

// Applies a received MULTI_RACE_UPDATE packet (also used locally to apply
// our own outgoing update, same as the capture-the-flag/bounty packets do).
void multi_do_race_update(const ubyte *buf);

// Applies a received MULTI_RACE_STATE packet (host -> clients).
void multi_do_race_state(const ubyte *buf);

// Applies a received MULTI_RACE_BOX packet (a player took a mystery box).
void multi_do_race_box(const ubyte *buf);

// True while players should be held at the start line (no forward thrust).
int race_countdown_active(void);

// Whole seconds left in the countdown for display ("3","2","1"); 0 once GO
// has fired. Only meaningful while race_countdown_active() is true.
int race_countdown_seconds_left(void);

// If `powerup` is one of this level's mystery box orbs, returns its 0-based
// box index; otherwise -1. Used by the collision code to route the pickup
// through race_box_taken() instead of the normal networked-powerup path.
int race_box_index_from_object(const object *powerup);

// Consumes `powerup` as a mystery box if it is one, adopting it on the spot if
// it hasn't been registered yet. Returns 1 if it was handled.
int race_box_hit(object *powerup);

// True if checkpoint `cp` is something the local player still has to reach on
// this lap -- an uncrossed checkpoint, or the start/finish line once every
// other checkpoint is done. The set resets at the top of each lap.
int race_checkpoint_is_target(int cp);

// Which checkpoint (if any) owns `segnum`; -1 for a segment that isn't one.
int race_checkpoint_of_segment(int segnum);

// A segment belonging to checkpoint `cp`, or -1. One checkpoint can be several
// connected segments; this is the one that stands for it, which is what
// anything routing through the track wants rather than all of them.
int race_checkpoint_segment(int cp);

// How many checkpoints the level has, including the one standing in as the
// start/finish line if it marks none -- so a route can walk the whole set.
int race_checkpoint_count(void);

// How many checkpoints make up one lap. Every matcen, unless the level marks
// no line and one of them is standing in as it.
int race_checkpoint_total(void);

// True if `segnum` is part of the start/finish line.
int race_segment_is_finish(int segnum);

// True once every checkpoint is done, so the line is what's left to cross.
int race_finish_is_target(void);

// Consumes mystery box `box`: hides the orb, schedules its respawn and (if
// `broadcast`) tells the other players. Safe to call twice for the same box.
void race_box_taken(int box, int pnum, int broadcast);

// How long a weapon out of a mystery box stays in the rack before it
// evaporates, using the engine's own super split in both racks: primaries from
// the super laser up, secondaries from the flash missile up.
#define RACE_ITEM_SUPER_TIME    (F1_0*30)
#define RACE_ITEM_NORMAL_TIME   (F1_0*15)

// Rolls a mystery box for the local player: 1-3 weapons off the loot table,
// with 1 by far the most likely.
void race_box_roll(void);

// One draw off that same loot table for `pnum`, weighted by their class and
// their place in the field, for a racer with no weapon rack to put it in.
// Returns 0 if the table has nothing to offer them.
int race_roll_box_item(int pnum, int *wclass, int *index);

//	--- box powers ---

// A box can also hand out a power: an effect that fires the instant it is
// rolled and hits the rest of the field, rather than a weapon that goes into
// the rack. They ride the same loot table as the weapons, in a third "rack"
// of their own, so the rubber band that hands the back-marker megas hands
// them these too.
#define CLASS_POWER             2	// alongside CLASS_PRIMARY / CLASS_SECONDARY

#define RACE_POWER_EMP          0	// EMP: scrambles everyone else's display
#define RACE_POWER_TRACTOR      1	// tractor beam: halves everyone else's speed
#define RACE_NUM_POWERS         2

// A tractor beam lasts five seconds, same as it always has -- it is a straight
// speed penalty, so a racer knows exactly what it costs the moment it lands.
// An EMP is worse to be caught in and rarer to draw (see the loot weights in
// race.c) -- it is the box's real punishment, not the tractor's smaller
// cousin, so it runs longer: enough to bloom, disorient, and clear.
#define RACE_POWER_TIME          (F1_0*5)
#define RACE_POWER_EMP_TIME    (F1_0*9)

// How long an early clear eases out over, instead of snapping to nothing.
// Well under a third of either RACE_POWER_TIME or RACE_POWER_EMP_TIME.
#define RACE_POWER_RELEASE_TIME  (F1_0/2)
#define RACE_TRACTOR_SCALE      (F1_0/2)	// -50% thrust while held

// Fires power `power`, rolled by `pnum`. Everyone but the roller (and
// observers, who are only watching) takes it. `broadcast` sends it on to the
// other machines; a received packet applies with 0.
void race_power_trigger(int power, int pnum, int broadcast);

// Applies a received MULTI_RACE_POWER packet.
void multi_do_race_power(const ubyte *buf);

// 0 when not jammed, F1_0 at full strength, fading in and back out over the
// life of the effect. Drawn as a screen glitch by race_draw_hud() (gauges.c).
fix race_emp_strength(void);

// True once the EMP is strong enough to blank the gauges, false otherwise.
// race_emp_strength() ramps smoothly, so this makes exactly one on/off
// transition per EMP -- not a repeating flicker. Gates the render_gauges()
// call site in gamerend.c. Deliberately NOT tied to any per-frame toggle:
// rapid on/off strobing is a real photosensitive-seizure risk, so nothing
// in this effect flashes more than twice in an EMP's whole run.
int race_emp_gauge_hidden(void);

// Same shape for the tractor beam's blue wash, and the thrust multiplier that
// goes with it (F1_0 when not held). The scale is applied in
// read_flying_controls() (controls.c), next to the class one.
fix race_tractor_strength(void);

// See race.c: who is pulling the local player right now, or NULL.
const char *race_tractor_puller(void);
fix race_power_speed_scale(void);

// Strips a ship down to no weapons at all -- no laser, no concussions. Race
// starts empty and everything you fire comes out of a box.
void race_strip_loadout(int pnum);

// Seconds left before a box weapon evaporates, or 0 if the player isn't
// holding that one from a box. `wclass` is CLASS_PRIMARY/CLASS_SECONDARY.
fix64 race_get_item_remaining(int wclass, int index);
int race_item_powerup(int wclass, int index);
int race_get_item_ammo(int wclass, int index);
const char *race_item_name(int wclass, int index);

//	--- classes and the pre-race lobby ---

// A race starts with everyone held on the grid picking a class; the countdown
// only begins once every connected player has locked one in (or the lobby's
// hard cap expires, so one AFK player cannot stall the grid). The class then
// lasts the whole race.
#define RACE_CLASS_NONE     (-1)
#define RACE_NUM_CLASSES    7
#define RACE_LOBBY_TIMEOUT  (F1_0*60)	// hard cap on the wait for stragglers
// How long past that cap a client will keep waiting for the host to release
// the grid before releasing itself. Only ever reached if the host has gone.
#define RACE_LOBBY_HOST_GRACE (F1_0*10)

// The Trapper's mine trickle. The caps are deliberately well under the racks'
// own maximums: the class is meant to pick its corner and lay one, not bank a
// minefield over a slow lap. Smart mines come slower and in smaller numbers,
// being the nastier of the two.
#define RACE_TRAPPER_PROX_TIME  (F1_0*15)
#define RACE_TRAPPER_PROX_CAP   4
#define RACE_TRAPPER_SMART_TIME (F1_0*25)
#define RACE_TRAPPER_SMART_CAP  2

// How many secondaries a class kit can carry.
#define RACE_MAX_KIT_SECONDARIES 2

// One secondary a class kit carries: it starts with half a load, tops itself
// back up on a timer, and never expires the way box loot does.
typedef struct race_kit_secondary {
	int		index;			// secondary weapon index, or -1 for an unused slot
	int		cap;			// most of it the kit ever holds at once
	fix64	refill;			// how often one more arrives (0 = never)
} race_kit_secondary;

typedef struct race_class_info {
	const char	*name;				// "HEAVY HITTER"
	const char	*weapon;			// the kit it races with
	const char	*perks;				// one line of what it trades
	int			primary;			// primary weapon index granted on every spawn
	int			powerup;			// powerup sprite that stands for the kit, for the lobby panel
	int			player_flags;		// extra PLAYER_FLAGS_* the kit comes with
	race_kit_secondary secondary[RACE_MAX_KIT_SECONDARIES];
	int			shield_pct;			// shields as a % of the netgame's own starting value (0 = stock)
	int			box_extra_rolls;	// extra mystery box items on every roll
	fix			box_item_time;		// multiplier on how long box loot lasts
	fix			boost_time;			// multiplier on how long a boost pad lasts
	fix			boost_power;		// multiplier on how hard it pushes
	fix			speed;				// thrust multiplier
	fix			turn;				// rotation-thrust multiplier (0 counts as no change)
	fix			damage_taken;		// incoming damage multiplier
	fix			damage_dealt;		// outgoing damage multiplier
	fix			afterburner_drain;	// afterburner burn rate multiplier (lower lasts longer)
} race_class_info;

// The table entry for a class, or NULL for RACE_CLASS_NONE / out of range.
const race_class_info *race_get_class_info(int cls);

// Which class a player is racing, or RACE_CLASS_NONE. Synced, so this is
// answerable for every player on every machine.
int race_get_class(int pnum);

// Multipliers for the local player's class, all F1_0 when they have none.
// Call sites: read_flying_controls() (controls.c) for both of the first two,
// apply_damage_to_player() (collide.c) for the damage one, which takes the
// object that dealt it so the shooter's class counts too.
fix race_class_speed_scale(void);
fix race_class_turn_scale(void);
fix race_class_afterburner_drain_scale(void);
fix race_scale_damage(const object *killer, fix damage);

// The same, for any racer -- the bot field takes its damage in racebot.c
// rather than through apply_damage_to_player()'s local-player path, and a
// class that trades shields for speed has to mean the same thing there.
fix race_scale_damage_for(int pnum, const object *killer, fix damage);

// Earthshaker targeting. A shaker in a race is the back-marker's answer to
// whoever is running away with it, so it ignores the nearest ship and flies
// at the leader instead -- and it flies at them whether or not the HAM gave
// the missile a homing flag, which is what race_force_homing() is for.
// race_homing_target() returns the objnum to fly at, or -1 to leave the
// engine's own tracking alone.
int race_homing_target(const object *tracker);
int race_force_homing(const object *obj);

// Where to steer a race-mode shaker that has already picked target_objnum
// (from race_homing_target() above) as its leader. Routes it through the
// track's own segment graph instead of straight through whatever's between
// here and there, falling back to the target's own position -- the old
// straight-line behaviour -- if no path can be built. Always fills *aim.
void race_homing_aim_point(object *tracker, int target_objnum, vms_vector *aim);

// How much quicker a homing missile flies in a race than the HAM says. A homer
// chasing a ship at racing speed needs to be able to catch it.
#define RACE_HOMING_SPEED       (F1_0 + F1_0/2)		// +50%

// Weapon `weapon_id`'s flight speed: the HAM's own number everywhere, scaled
// by the above for a homing missile in a race. Call this instead of reading
// Weapon_info[].speed[] directly anywhere a weapon's speed is set or capped.
fix race_weapon_speed(int weapon_id);

// Hands `pnum` their class weapon. Call right after race_strip_loadout() on
// every spawn: box loot is lost on death, the class kit never is.
void race_grant_class_loadout(int pnum);

// True if a loose powerup is one the local player may pick up: everything
// except another class's primary weapon. Call before do_powerup() so a racer
// can never end up carrying a gun their class doesn't race with.
int race_powerup_allowed(int powerup_id);

// Sets `pnum`'s class. The lobby uses it for the local player and the ready
// packet for everyone else; the bot field uses it to put each CPU racer in a
// kit of its own.
void race_set_player_class(int pnum, int cls);

// Closes out `pnum`'s race: marks them finished on our clock, hands them the
// next finishing place and announces it. The local player goes through
// race_check_checkpoint(); this is the door for everyone else.
void race_finish_player(int pnum);

// True if this is the local player's class weapon, which box loot expiry must
// leave alone.
int race_is_class_weapon(int wclass, int index);

// True while the grid is still picking classes. race_lobby_blocks_input() is
// the one to gate flight control, firing and key handling on -- it is the
// same for everyone but an observer, who is only watching.
int race_lobby_is_open(void);
int race_lobby_blocks_input(void);

// Routes a key to the lobby (1/2/3 or the arrows to pick, ENTER to lock in).
// Returns 1 if the lobby consumed it; anything it doesn't own falls through,
// so the menu, chat and the rest keep working while the grid fills up.
int race_lobby_handle_key(int key);

// What the lobby panel draws: the class the local player is looking at, how
// many players are in out of how many, whether one player is in, and how long
// is left on the hard cap.
int race_lobby_cursor_class(void);
int race_lobby_ready_counts(int *ready, int *total);
int race_player_is_ready(int pnum);
fix64 race_lobby_time_left(void);

// Applies a received MULTI_RACE_READY packet (a player locked in a class).
void multi_do_race_ready(const ubyte *buf);

// Floating in-world labels over checkpoints, the finish line and boost pads.
// Segments of the same kind that touch are merged into one label centred
// across the group. race_get_labels() returns the count and points *labels at
// the table; race_label_visible() reports whether label `i` is in a segment
// the renderer drew this frame (so labels don't shine through walls).
int race_get_labels(const race_label **labels);
int race_label_visible(int i);

// Starts/refreshes the boost-pad boost if segp is a boost pad. Called for the
// local player's segment every frame.
void race_check_boost_pad(segment *segp);

// Extra forward thrust multiplier while boosting (F1_0 = no boost).
fix race_get_boost_scale(void);

// Drops the boost immediately, for when the player wants to stop rather than
// ride it out (they hit reverse).
void race_cancel_boost(void);

// Extra Render_zoom to add while boosting (0 when not boosting).
fix race_get_fov_bonus(void);

// Fills *pos/*orient/*segnum with the local player's last captured checkpoint
// and returns 1; returns 0 (leaving the outputs untouched) if they have not
// captured one yet, so the caller falls back to a normal spawn point.
int race_get_respawn(vms_vector *pos, vms_matrix *orient, int *segnum);

// Nudges *pos to player slot `pnum`'s own fixed point on a ring around
// wherever it already is, riding *orient's rvec/uvec, and updates *segnum to
// match -- so two racers respawning at the same checkpoint don't land inside
// each other. Leaves both untouched if the offset doesn't land somewhere
// real (find_point_seg() fails) or pnum is 0. Shared by race_get_respawn()
// (the local player) and race_bot_place() (racebot.c, the bot field).
void race_spread_respawn(vms_vector *pos, const vms_matrix *orient, int pnum, int *segnum);

// Formats a fix-seconds race time as "M:SS.hh" (or "--:--.--" for 0).
void race_format_time(char *buf, int bufsz, fix64 t);

// Race clock for the local player, all in fix seconds. The total runs from GO
// and freezes at the finish; the lap time runs from the last completed lap.
// Splits are the completed lap times, oldest first, with *best set to the
// quickest of them (0 if none yet).
fix64 race_get_total_time(void);
fix64 race_get_lap_time(void);
int race_get_splits(const fix64 **splits, fix64 *best);

// Set the moment the local player crosses the line for the last time, so the
// game loop can drop them into the results screen at a safe point. Reading it
// clears it.
int race_take_summary_pending(void);

// Elapsed race time for player `pnum`, measured on our clock (0 if they have
// not finished). Exact for the local player, an estimate for the rest.
fix64 race_get_finish_elapsed(int pnum);

// Live race results screen: the standings the player is dropped into when
// they finish, which keeps updating as everyone else comes in.
void race_show_summary(void);

// Fills sorted[] (must hold at least N_players ints) with player indices
// ranked 1st..last: finishers first (by finishing order), then by progress
// (laps/checkpoints reached, tie-broken by distance to their next
// checkpoint). Returns the number of entries written (N_players).
int race_get_positions(int *sorted);

// 1-based placement of a single player among all players. Recomputes the
// full ranking, so prefer race_get_positions() when showing more than one
// player's rank (e.g. a standings list).
int race_get_rank(int pnum);

// Fills buf (up to bufsz-1 chars, nul-terminated) with the currently active
// banner text ("CHECKPOINT", "LAP 2/3", "FINISH!", "WRONG WAY!") and *style
// with the matching RACE_BANNER_* tint, and returns 1 if one should be shown
// right now (briefly, after crossing); returns 0 and leaves the outputs
// untouched otherwise. `style` may be NULL.
int race_get_banner(char *buf, int bufsz, int *style);

// One line of the minimap's top-down mine outline, in the normalised map
// space race_map_project() returns.
typedef struct race_map_line {
	fix x0, y0, x1, y1;
} race_map_line;

// Top-down minimap: flattens a world point onto the plane the track spreads
// out over most and normalises it to [-F1_0, F1_0] across the level's bounding
// box (a point outside the box lands outside that range). Returns 0, leaving
// the outputs untouched, if the level has no usable bounds. `mx`/`my` may be
// NULL.
int race_map_project(const vms_vector *pos, fix *mx, fix *my);

// The level's top-down outline: the wall edges that define its shape, already
// projected. Returns the line count and points *lines at the table.
int race_get_map_outline(const race_map_line **lines);

// World-space unit direction and distance from the local player to the
// nearest checkpoint they still owe this lap. Returns 0 (leaving *dir/*dist
// untouched) if there is nothing outstanding.
int race_get_next_checkpoint_vec(vms_vector *dir, fix *dist);

#endif
