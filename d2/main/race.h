/*
 * Race game mode -- checkpoints, laps, and race presentation.
 *
 * Checkpoints reuse existing SEGMENT_IS_ROBOTMAKER segments: they are
 * already invisible/non-rendered and already carry a small sequential ID
 * (matcen_num, 0-based in level-editor placement order), so no new segment
 * type, level format change, or editor support is needed. Robot maker
 * segments only spawn robots when explicitly triggered by a level trigger
 * (trigger_matcen()), which race levels simply never wire up, so reusing
 * them here does not risk spawning robots.
 *
 * matcen_num 0 is always the start/finish line. Any level with matcens
 * placed (and no matcen triggers wired to them) works as a race track with
 * zero additional editor work; levels with no matcens simply don't lock
 * players at the start and never award laps, so GM_RACE degrades to a
 * harmless free-for-all rather than soft-locking a mismatched map.
 */

#ifndef _RACE_H
#define _RACE_H

#include "player.h"
#include "segment.h"
#include "vecmat.h"

#define RACE_CHECKPOINT_START   0
#define RACE_DEFAULT_LAPS       3
#define RACE_COUNTDOWN_SECONDS  3	// "3, 2, 1, GO"

typedef struct race_player_info {
	ubyte	next_checkpoint;	// matcen_num this player must reach next
	ubyte	laps_completed;
	ubyte	finished;
	fix64	finish_time;
} race_player_info;

extern race_player_info Race_player[MAX_PLAYERS];
extern int Race_num_checkpoints;	// total checkpoints on this level (0 = none placed)
extern int Race_laps_to_win;

// Resets all players' race progress, (re)counts checkpoints for the level
// that was just loaded, and (if any checkpoints exist) starts the
// start-line countdown. Call once per level start when GM_RACE.
void race_init_level(void);

// Call every frame for the local player while GM_RACE is active (in
// addition to race_check_checkpoint on segment change). Advances the
// start-line countdown.
void race_frame(void);

// Call whenever the local player's ship enters a new segment while
// GM_RACE is active. Advances the local player's checkpoint/lap state
// and networks the update if segp is the next checkpoint they need; warns
// (rate-limited) if it's a checkpoint but the wrong one.
void race_check_checkpoint(segment *segp);

// Applies a received MULTI_RACE_UPDATE packet (also used locally to apply
// our own outgoing update, same as the capture-the-flag/bounty packets do).
void multi_do_race_update(const ubyte *buf);

// True while players should be held at the start line (no forward thrust).
int race_countdown_active(void);

// Whole seconds left in the countdown for display ("3","2","1"); 0 once GO
// has fired. Only meaningful while race_countdown_active() is true.
int race_countdown_seconds_left(void);

// Fills sorted[] (must hold at least N_players ints) with player indices
// ranked 1st..last: finishers first (by finish time), then by progress
// (laps/checkpoints reached, tie-broken by distance to their next
// checkpoint). Returns the number of entries written (N_players).
int race_get_positions(int *sorted);

// 1-based placement of a single player among all players. Recomputes the
// full ranking, so prefer race_get_positions() when showing more than one
// player's rank (e.g. a standings list).
int race_get_rank(int pnum);

// Fills buf (up to bufsz-1 chars, nul-terminated) with the currently active
// checkpoint/lap/finish banner text ("CHECKPOINT", "LAP 2/3", "FINISH!") and
// returns 1 if one should be shown right now (briefly, after crossing);
// returns 0 and leaves buf untouched otherwise.
int race_get_banner(char *buf, int bufsz);

// World-space unit direction and distance from the local player to their
// next checkpoint. Returns 0 (leaving *dir/*dist untouched) if there is no
// meaningful next checkpoint (no checkpoints on this level, or the local
// player has already finished).
int race_get_next_checkpoint_vec(vms_vector *dir, fix *dist);

#endif
