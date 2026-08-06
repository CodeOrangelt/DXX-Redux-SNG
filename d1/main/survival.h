/*
 * Survival game mode: endless waves of robots, single cooperative team,
 * boss wave every 10th wave, periodic ammo drops, permanent elimination on
 * death. See survival.c for the full design notes, especially the caveat
 * on how "observer mode" is actually implemented (not the engine's real
 * multi-observer system -- see the big comment above survival_player_died).
 */

#ifndef _SURVIVAL_H
#define _SURVIVAL_H

#include "pstypes.h"
#include "object.h"

// Robot death drop chances for Survival mode. These are INDEPENDENT rolls,
// not slices of one 0..100 range: a robot can drop a weapon, a sustain item,
// both, or nothing. Keeping them separate means weapon luck and shield luck
// never compete with each other.
#define SURVIVAL_WEAPON_DROP_PCT 28   // primaries + secondaries (see Survival_weapon_types)
#define SURVIVAL_SUPPLY_DROP_PCT 12   // shields / energy / vulcan ammo

// Extra life drop, rolled in tenths of a percent rather than whole percent
// because it needs to be genuinely rare -- it's a free revive, not a pickup.
#define SURVIVAL_EXTRA_LIFE_DROP_PERMILLE 8   // 0.8% per robot killed

// Picks one of Survival mode's sustain (shield/energy/ammo) powerup ids at
// random. Also used for the periodic scheduled ammo drops.
int survival_random_ammo_type(void);

// Picks one of Survival mode's weapon powerup ids at random, spanning every
// primary and secondary -- in Survival these drops are the only source of
// weapons in the entire match.
int survival_random_weapon_type(void);

// Rolls every drop table for a just-killed robot and creates/syncs whatever
// comes up. Called from multi_drop_robot_powerups() (multibot.c) in place of
// the stock contains_prob path. No-op outside Survival mode.
void survival_robot_drops(object *del_obj);

// Deletes every powerup the level author placed. Survival supplies the mine
// exclusively through robot drops, so the map itself starts barren. Call
// from multi_prep_level() after the level loads. No-op outside Survival.
void survival_strip_level_powerups(void);

// Call once when a Survival match starts (net_udp_start_game path), on
// every machine.
void survival_start(void);

// Call once per multiplayer frame (from multi_do_frame()) on every machine.
// Only the spawner-authority machine actually spawns anything; everyone
// else just ages down local announcement/HUD state.
void survival_do_frame(void);

// Call from DoPlayerDead() before it would normally respawn the player, when
// Netgame.gamemode == NETGAME_SURVIVAL. Marks pnum as down, syncs that to
// everyone, and checks whether the match is now over (which only happens if
// *every* player is down at once). Returns 1 if the caller should skip the
// normal respawn flow entirely (match ended, player is being sent to the
// post-game stats screen), 0 if the caller should continue with its own
// respawn handling (survival_player_died() itself does not respawn or ghost
// anyone -- that's the caller's job, since it already owns the correct
// sequencing).
//
// Being down is NOT permanent: as long as at least one teammate survives the
// wave, everyone who went down during it is revived when that wave is
// cleared (see survival_revive_all() in survival.c). A match therefore only
// ends on a full team wipe within a single wave.
int survival_player_died(int pnum);

// True if pnum is currently down (spectating, awaiting revive at wave clear).
int survival_is_eliminated(int pnum);

// Banked self-revives the local player is holding. 0 outside Survival mode.
// Drawn alongside the score readout as "EL: n" -- see hud_show_score().
int survival_extra_lives(void);

// Call when the local player picks up a POW_EXTRA_LIFE (powerup.c). Banks one
// self-revive: the next death spends it instead of putting the player out for
// the wave. No-op outside Survival mode.
void survival_add_extra_life(void);

// Call wherever the local player is credited with a robot kill (the
// add_points_to_score() sites in collide.c and multibot.c), with the same
// score value that was awarded. Starts a floating "+points" at the robot's
// position. No-op outside Survival mode.
void survival_note_robot_kill(object *robot, int points);

// Draws the floating "+points" popups and the nearest-robot direction
// arrow, both in the configured reticle color. Uses 3D projection like
// show_survival_boss_bars(), so it belongs in the same post-render pass in
// gamerend.c, not in draw_hud(). No-op outside Survival mode.
void survival_draw_kill_feedback(void);

// Draws the "Wave N" HUD text (top-left). No-op outside Survival mode.
void survival_draw_hud(void);

// Draws an HP bar under each currently-alive boss robot. Uses screen-space
// 3D projection (like arcade_draw_superpower_labels()), so it must be
// called from the same post-3D-render pass, not from inside draw_hud()'s
// flat 2D overlay -- see its call site next to arcade_draw_superpower_labels()
// in gamerend.c. No-op outside Survival mode.
void show_survival_boss_bars(void);

// Fills out_objnums/out_fracs (parallel arrays, up to max_out entries) with
// the objnums and remaining-shields fraction (0..F1_0) of currently-alive
// boss robots, pruning any that have died since the last call. Returns the
// number of entries written. Used by gauges.c to draw an HP bar under each
// one; safe to call every HUD frame.
int survival_get_active_bosses(short *out_objnums, fix *out_fracs, int max_out);

// Network receive handlers, dispatched from multi.c's packet switch.
void multi_do_survival_wave_state(const ubyte *buf);
void multi_do_survival_spawn_robot(const ubyte *buf);
void multi_do_survival_eliminated(const ubyte *buf);
void multi_do_survival_shields(const ubyte *buf);

#endif /* _SURVIVAL_H */
