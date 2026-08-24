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
#define SURVIVAL_WEAPON_DROP_PCT 17   // primaries + secondaries (see Survival_weapon_types)
#define SURVIVAL_SUPPLY_DROP_PCT 7    // shields / energy / vulcan ammo

// Extra life drop, rolled in tenths of a percent rather than whole percent
// because it needs to be genuinely rare -- it's a free revive, not a pickup.
#define SURVIVAL_EXTRA_LIFE_DROP_PERMILLE 6   // 0.6% per robot killed

// Cloak/invulnerability drop off a killed robot. Split out of Survival_weapon_
// types[] into its own rare independent roll (same treatment as extra life
// above) rather than being one slot among many in that table -- being lumped
// in at 1-in-16 made them show up about as often as any other single weapon,
// which is too common for something that grants temporary immunity/stealth.
// This is the floor/robot-drop odds only; the shop's paid "Random Supply"
// roll (Survival_shop_supply_types[], survival.c) is unaffected since that's
// a player's own spending choice, not a random gift.
#define SURVIVAL_SITUATIONAL_DROP_PERMILLE 7   // 0.7% per robot killed, split 50/50 cloak vs invuln

// D2-exclusive "super" weapons (Gauss/Helix/Phoenix/Omega, and the newer
// D2 secondaries) are split out of Survival_weapon_types[] into their own
// rarer independent roll -- D1's original weapon set (SURVIVAL_WEAPON_DROP_PCT
// above) is deliberately more common than these, both in aggregate (35
// permille vs 170) and per-item (spread across ~11 entries vs ~15).
#define SURVIVAL_SUPER_WEAPON_DROP_PERMILLE 35   // 3.5% per robot killed

// The Earthshaker Missile is rarer still, and gets its own count roll rather
// than always granting a fixed amount -- a match should see maybe one or two
// of these over its whole run, not a steady trickle.
#define SURVIVAL_EARTHSHAKER_DROP_PERMILLE 2   // 0.2% per robot killed

// Picks one of Survival mode's sustain (shield/energy/ammo) powerup ids at
// random. Also used for the periodic scheduled ammo drops.
int survival_random_ammo_type(void);

// Picks one of Survival mode's original (D1-parity) weapon powerup ids at
// random, spanning every D1 primary and secondary -- in Survival these
// drops are the primary source of weapons in the entire match. D2's extra
// "super" weapons and the Earthshaker Missile are rolled separately and
// more rarely -- see SURVIVAL_SUPER_WEAPON_DROP_PERMILLE / SURVIVAL_
// EARTHSHAKER_DROP_PERMILLE above.
int survival_random_weapon_type(void);

// Rolls every drop table for a just-killed robot and creates/syncs whatever
// comes up. Called from multi_drop_robot_powerups() (multibot.c) in place of
// the stock contains_prob path. No-op outside Survival mode.
void survival_robot_drops(object *del_obj);

// Deletes every powerup the level author placed. Survival supplies the mine
// exclusively through robot drops, so the map itself starts barren. Call
// from multi_prep_level() after the level loads. No-op outside Survival.
void survival_strip_level_powerups(void);

// Deletes every robot the level author placed, including the mine's own scripted end-of-level
// guardian(s) -- every Survival robot comes from a wave spawn instead. Call from multi_prep_level()
// alongside survival_strip_level_powerups() above. No-op outside Survival.
void survival_strip_level_robots(void);

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

// Call right after a Survival respawn's init_player_stats_new_ship()/
// StartLevel(1) (gameseq.c), whether or not this particular respawn was an
// extra-life save -- no-op unless survival_player_died() actually spent one
// on this death. Grants a few seconds of invulnerability so a robot sitting
// on the spawn point can't erase the revive on the very next frame; see
// SURVIVAL_REVIVE_INVULN_DURATION in survival.c.
void survival_maybe_grant_revive_invulnerability(int pnum);

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

// "Elite" robots -- an occasional spawn, drawn with a colored 3D outline and a name readout under
// the model, that plays differently from a normal robot of its type for as long as it's alive:
//   BOUNTY   worth a large score bonus on top of its normal kill value -- a priority target, not a
//            mechanical twist on how it fights.
//   BRUTE    extra shields on top of the normal wave/difficulty scaling.
//   SWARMER  splits into fast little fragments on death. The fragments are plain, non-elite copies
//            of the swarmer's own type, so a fragment can never itself be a swarmer -- no chain
//            reaction is possible by construction, not by having to mark them as some other kind.
// Every kind also gets a harder death blast than a normal robot. Which robots are elite, and which
// kind, is decided once by the spawner and shipped in the spawn packet, so every machine agrees;
// nothing here re-rolls locally.
//
// A RUNNER kind (outran the speed cap) used to be here instead of BOUNTY -- pulled after a match
// made clear that, with every Survival robot already hunting at the mode's shared max speed
// (SURVIVAL_ROBOT_SPEED_SCALE, survival.c), "2x that" wasn't a distinct enough read in practice to be
// worth a whole kind's outline slot. BOUNTY reuses its old id (1) and its yellow/gold outline color.
#define SURVIVAL_ELITE_NONE            0
#define SURVIVAL_ELITE_BOUNTY          1
#define SURVIVAL_ELITE_BRUTE           2
#define SURVIVAL_ELITE_SWARMER         3

// survival_robot_is_elite() takes an objnum (not an object *) because the renderer works in objnums,
// and is safe to call on anything -- it returns SURVIVAL_ELITE_NONE outside Survival and for
// non-robots. No-op cost either way.
int survival_robot_is_elite(int objnum);

// Outline/label color for a kind returned by survival_robot_is_elite() above. -1 for
// SURVIVAL_ELITE_NONE, matching g3d_interp_outline_color's own "leave the model's own colour" value.
int survival_robot_elite_color(int kind);

// True while objnum is one of this wave's tracked boss robots (see show_survival_boss_bars()'s
// Survival_bosses[] tracking). Used by ai.c's boss-only AI hooks -- faster fire rate, quicker turning
// -- so a Survival boss plays noticeably differently from a normal robot of the same type, not just
// tougher. Safe to call on anything: 0 outside Survival.
int survival_robot_is_boss(int objnum);

// 1 for the first boss wave (wave 10), 2 for the second (wave 20), and so on -- 0 outside a boss
// wave or outside Survival. Feeds ai.c's escalating boss-only AI hooks (survival_boss_scale() there),
// so each successive boss fights harder than the last rather than every boss playing identically.
int survival_boss_tier(void);

// Label text ("BOUNTY"/"BRUTE"/"SWARMER") for a kind returned by survival_robot_is_elite() above.
// "" for SURVIVAL_ELITE_NONE.
const char *survival_robot_elite_name(int kind);

// Lays an area-damage blast over a dying elite's normal explosion (every kind); for a SWARMER,
// scatters its fragments too; for a BOUNTY, pays out a score bonus. Call from wherever a robot is
// actually destroyed, on every machine (multi_explode_robot_sub(), multibot.c) -- it is driven off
// the synced kind rather than a packet of its own; the fragment spawn re-uses spawner authority
// internally, so this needs no such gate at the call site. `killer` is that robot's killer objnum
// (same value multi_explode_robot_sub() receives) -- needed so the BOUNTY bonus lands on the one
// player who actually got the kill rather than on every machine that processes the death. No-op for
// non-elites.
void survival_robot_death_blast(object *robot, int killer);

// Call from collide_player_and_powerup() (collide.c) right after do_powerup() returns 0 for a
// pickup attempt that was otherwise legitimate: the player already has/holds whatever it offers, so
// stock behaviour leaves it sitting on the floor uncollected. Converts it into a small amount of
// score and reports 1 (as if it had been used) so the caller destroys and syncs it exactly like a
// normal pickup. Returns 0 (does nothing) outside Survival, for an observer, or for any powerup kind
// Survival's own tables don't produce. No-op cost outside Survival.
int survival_convert_wasted_pickup(object *powerup);

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

// Draws the shield-count readout under each elite robot's model -- same projection, same
// post-3D-render pass, called right alongside show_survival_boss_bars() in gamerend.c.
// No-op outside Survival mode.
void survival_draw_elite_labels(void);

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
void multi_do_survival_shop_ready(const ubyte *buf);

// Shop: opens after every 5th wave clears. Waits for every currently-
// connected player to hit ESC (readiness is synced via MULTI_SURVIVAL_SHOP_
// READY), up to a SURVIVAL_SHOP_DURATION hard cap (survival.c), then runs a
// 5s voice countdown into the next wave.

// True while the local player's buy list should be interactive and its
// keys/mouse diverted away from normal ship controls -- narrower than
// survival_shop_blocks_input() below, which also covers the readied-and-
// waiting and countdown stretches where there's nothing left to buy but
// flight control still shouldn't return early.
int survival_shop_is_open(void);

// True for the whole shop experience: buying, readied-but-waiting-on-others,
// and the trailing voice countdown. This is the one to gate flight control,
// weapon firing, other key handlers, and mouse capture on -- see the
// should_read_controls gate in gamecntl.c's ReadControls() and
// survival_shop_do_mouse() (survival.c).
int survival_shop_blocks_input(void);

// Routes a key event to the shop (buy an item, or ESC to ready up). Call
// site (ReadControls(), gamecntl.c) should call this whenever survival_shop_
// blocks_input() is true and swallow the key unconditionally either way --
// this has nothing left to do once the local player has readied up, but the
// key still shouldn't fall through to any other handler.
void survival_shop_handle_key(int key);

// Draws the shop panel (buy list or, once readied/waiting, a compact status
// panel) and its countdown banner. No-op outside survival_shop_blocks_
// input(). Call from the same post-3D HUD pass as survival_draw_hud().
void survival_shop_draw(void);

// Thrust/damage multipliers from purchased physics upgrades, applied to the
// local player only. F1_0 = no change (outside Survival, or before any
// purchase). See read_flying_controls() (controls.c) and
// apply_damage_to_player() (collide.c) for the call sites.
fix survival_speed_multiplier(void);
fix survival_damage_multiplier(void);

// Scales a kamikaze robot's badass death-explosion damage/radius/force. F1_0 (no change) outside
// Survival or for a non-kamikaze robot_id. See survival.c for why kamikazes specifically need this.
fix survival_kamikaze_badass_scale(int robot_id);

#endif /* _SURVIVAL_H */
