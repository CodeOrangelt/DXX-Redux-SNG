/*
 * Race mode bots: a field of CPU racers, so race mode can be played solo
 * against opponents rather than against the clock.
 *
 * They are not a new AI. The engine already knows how to find a way through a
 * mine (create_path_points), how to swing a ship onto a heading
 * (ai_turn_towards_vector), how to ask whether a straight line is flyable
 * (find_vector_intersection) and how to fire a weapon (Laser_create_new_easy),
 * and race.c already knows what a mystery box hands out. A bot is those pieces
 * pointed at a lap:
 *
 *   - At level load the engine pathfinder is walked from the start/finish line
 *     through every checkpoint and back, and the point_segs it returns are
 *     kept as the lap route.
 *
 *   - Each frame a bot advances along that route, aims at the furthest route
 *     point it still has line of sight to, turns onto it and thrusts.
 *
 *   - Boxes it drives through are rolled off race.c's own loot table, and what
 *     comes out is fired at whoever is one place ahead of it.
 *
 * Bots occupy real player slots (1..N) with real OBJ_PLAYER objects, so they
 * are ranked, drawn, collided with, shot at and listed in the standings by the
 * code that already does all of that for players. They are single-player only:
 * nothing about them is networked, and race_bots_enabled() is false in any
 * multiplayer game.
 */

#ifndef _RACEBOT_H
#define _RACEBOT_H

#include "player.h"
#include "object.h"
#include "vecmat.h"

// One human plus a full grid of bots, capped by the player slots that exist.
#define RACE_MAX_BOTS       (MAX_PLAYERS - 1)

// How hard the field races. Picked in the pre-race menu, held across levels.
#define RACE_SKILL_ROOKIE   0
#define RACE_SKILL_PRO      1
#define RACE_SKILL_ACE      2
#define RACE_NUM_SKILLS     3

// Set by the single-player race menu before StartNewGame(): how many bots to
// field and how hard they race. Both are read when the level loads.
extern int Race_bot_count;
extern int Race_bot_skill;

// Set by the single-player race menu, consumed by StartNewGame(), which turns
// it into GM_RACE. Cleared by every other way of starting a game.
extern int Race_sp_pending;

// True when this game is a single-player race with a bot field.
int race_bots_enabled(void);

// Called at the end of gameseq_init_network_players(), before the start
// position count is asserted against N_players: takes player slots 1..N for
// the bot field, synthesising extra start positions if the level has fewer
// than the field needs, and sets N_players so the whole race -- standings,
// ranking, results -- counts the bots as racers.
void race_bots_claim_slots(void);

// True if player slot `pnum` (or object `obj`) is a bot. Both are safe to
// call at any time, in any game mode.
int race_player_is_bot(int pnum);
int race_object_is_bot(const object *obj);

// Builds the lap route and resets the field. Called from race_init_level().
void race_bots_init(void);

// Drives the field. Called once per frame from race_frame(), after the
// countdown has been advanced -- bots hold station on the grid until GO.
void race_bots_frame(void);

// The starting grid's grace period: called once, the moment "GO!" fires
// (race.c), so the whole field -- not just the human -- gets a few seconds
// where nothing can wreck it and a boost pad's worth of push off the line.
void race_bots_start_race(void);

// Damage that landed on a bot. apply_damage_to_player() only ever moves the
// local player's shields, so this is where a bot's come off. Returns 1 if it
// took the damage (it is a bot and race mode is on), 0 to be ignored.
int race_bot_take_damage(object *botobj, object *killer, fix damage);

// Books one segment of track against bot `pnum`: takes the checkpoint if it
// is one, closes the lap if it is the line. Called for every segment the
// physics walked the bot through this frame, so a checkpoint cube crossed
// inside a single frame still counts.
void race_bot_check_segment(int pnum, int segnum);

// How far bot `pnum` is from the nearest checkpoint it still owes, for the
// standings tiebreak. Returns -1 if `pnum` is not a bot.
fix race_bot_dist_to_next(int pnum);

// How far round the lap `pnum` has got, as an index into the bot field's route
// -- the one ordered path through the track, so a bigger number really is
// further round. Measured the same way for the human and for every bot, which
// is what makes it safe to rank them against each other, and far sharper than
// "how near is your next checkpoint" when two racers hold the same count from
// opposite ends of the lap. -1 when there is no route to measure against.
int race_route_progress(int pnum);

// A box power went off, rolled by `roller`. Lands on the whole bot field
// except the roller, the same way it lands on the human -- without this the
// "FIELD JAMMED" callout is a promise nothing keeps, because the powers are
// otherwise held in local-player-only state.
void race_bots_take_power(int power, int roller);

// The direction the track runs at `pos`, taken off the lap route -- which is
// the one thing in a race that knows which way round the track goes. `segnum`
// (or -1 if unknown) biases the route sample onto the segment `pos` is
// actually in, so a point near two close-but-different stretches of track
// doesn't get matched to the wrong one. Returns 0 and leaves *dir alone when
// there is no route. Used to point a respawning racer down the road rather
// than at whatever happens to be in front of them.
int race_route_direction(const vms_vector *pos, int segnum, vms_vector *dir);

// How many racers are on the grid, human and bot. 1 when there is no field,
// which is what turns the catch-up rubber band off.
int race_bot_field_size(void);

#endif
