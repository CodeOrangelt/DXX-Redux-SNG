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

// Strips a ship down to no weapons at all -- no laser, no concussions. Race
// starts empty and everything you fire comes out of a box.
void race_strip_loadout(int pnum);

// Seconds left before a box weapon evaporates, or 0 if the player isn't
// holding that one from a box. `wclass` is CLASS_PRIMARY/CLASS_SECONDARY.
fix64 race_get_item_remaining(int wclass, int index);
int race_item_powerup(int wclass, int index);
int race_get_item_ammo(int wclass, int index);
const char *race_item_name(int wclass, int index);

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

// World-space unit direction and distance from the local player to the
// nearest checkpoint they still owe this lap. Returns 0 (leaving *dir/*dist
// untouched) if there is nothing outstanding.
int race_get_next_checkpoint_vec(vms_vector *dir, fix *dist);

#endif
