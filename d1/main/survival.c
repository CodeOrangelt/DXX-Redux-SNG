/*
 * Survival game mode: endless waves of robots, everyone on one cooperative
 * team (no friendly fire, same as Coop), boss wave every 10th wave, and a
 * scavenging economy -- the level is stripped of every author-placed powerup
 * (survival_strip_level_powerups()) and robot drops are the only source of
 * weapons, shields and ammo for the whole match.
 *
 * Dying puts you out for the remainder of the current wave only: you spectate
 * (invisible and untouchable, see survival_revive_all()) and come back when
 * the surviving team clears that wave. The match ends only on a full team
 * wipe inside a single wave. A rare POW_EXTRA_LIFE drop banks one instant
 * self-revive that is spent automatically instead of going down at all
 * (survival_add_extra_life() / survival_player_died()).
 *
 * Robots behave like a zombie horde -- they never lose track of the player
 * through geometry, via a single narrow override in
 * player_is_visible_from_object() (ai.c) that downgrades a blocked line of
 * sight to "visible but not lined up" instead of "can't see you". They still
 * have to get a real line of sight before they can fire. That is deliberately
 * the ONLY AI change this mode makes; see the warning above
 * survival_limit_robot_speeds() before touching robot behaviour again.
 *
 * DESIGN NOTES
 *
 * Dynamic robot spawning does not exist anywhere else in this engine --
 * matcens (fuelcen.c) only replay whatever a level's author placed, and
 * nothing picks a robot type at runtime. This module is modeled closely on
 * "Arcade mode"'s periodic superpower dropper (multi_arcade_do_frame() and
 * friends in multi.c): one spawner-authority machine (normally the host)
 * owns all spawn decisions and tells everyone else via network packets;
 * every other machine just reacts to those packets. See multi_send_data()
 * callers below for the exact wire format of each new packet type
 * (MULTI_SURVIVAL_WAVE_STATE / _SPAWN_ROBOT / _ELIMINATED, multi.h).
 *
 * Spectating while down is NOT the engine's real Observer role. That role
 * (GM_OBSERVER / OBSERVER_PLAYER_ID) is a single shared global slot with
 * its own separate player roster (Netgame.observers[]) -- it cannot
 * represent several simultaneously-dead Survival players who each still
 * need their own identity, kills, and stats. Instead, a downed player gets
 * a normal respawn (safe, reuses tested code, keeps their object valid for
 * the rest of the engine's many `Objects[Players[pnum].objnum]` assumptions)
 * and is then flagged invulnerable and blocked from firing (see
 * survival_is_eliminated() call sites in game.c), while every *other*
 * machine turns them into an OBJ_GHOST so nobody can see them. They fly
 * around and watch, unable to fight, be hurt, or be seen -- the same
 * experience as spectating, built out of existing, safe primitives rather
 * than the incompatible built-in Observer system.
 */

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "pstypes.h"
#include "3d.h"
#include "maths.h"
#include "key.h"
#include "multi.h"
#include "net_udp.h"
#include "game.h"
#include "gr.h"
#include "gamefont.h"
#include "gauges.h"
#include "playsave.h"
#include "hudmsg.h"
#include "player.h"
#include "object.h"
#include "robot.h"
#include "multibot.h"
#include "powerup.h"
#include "fuelcen.h"
#include "fireball.h"
#include "endlevel.h"
#include "cntrlcen.h"
#include "morph.h"
#include "gameseg.h"
#include "vclip.h"
#include "timer.h"
#include "dxxerror.h"
#include "byteswap.h"
#include "digi.h"
#include "sounds.h"
#include "newdemo.h"
#include "mouse.h"
#include "bm.h"        // GameBitmaps, for the powerup sprites used as shop icons
#include "piggy.h"     // PIGGY_PAGE_IN
#include "ogl_init.h"  // ogl_ubitmapm_cs
#include "newmenu.h"   // nm_draw_background, the stock Descent menu frame
#include "survival.h"

extern int choose_drop_segment(void);
extern void init_player_stats_new_ship(ubyte pnum);

// Defined below, next to the countdown that drives most of the splashes.
static void survival_banner(const char *text, fix64 duration, int sound);

// Defined below with the rest of the shop's mouse handling; needed up here
// by survival_start() and survival_do_frame()'s early-outs.
static void survival_shop_set_mouse_released(int released);

#define SURVIVAL_WAVE_ROBOTS_BASE     3
#define SURVIVAL_WAVE_ROBOTS_MAX      14
#define SURVIVAL_BOSS_WAVE_INTERVAL   10

// Boss shields, on top of the normal wave/difficulty scaling every robot gets (see the mult
// computation in survival_spawn_one_robot()). Multiplicative rather than the old flat "+100%" add:
// applied after the wave/difficulty term, a multiply keeps scaling proportionally as that term grows
// with wave number, where a flat add becomes relatively smaller and smaller the longer the match runs
// -- so late bosses were barely tankier than the mobs around them. 3x here, stacked with the
// per-player term below, means the very first boss (wave 10, solo) comes in at roughly 3x a same-wave
// regular mob's shields; a full 8-player lobby stacks the per-player multiplier on top of that too.
#define SURVIVAL_BOSS_SHIELD_MULT     (F1_0 * 3)

// Player-count scaling: solo is the baseline (x1.0), and every player beyond the first multiplies
// boss shields by another 1.5x -- 2 players: x1.5, 3: x2.25, 4: x3.375, and so on, compounding rather
// than adding, so a full lobby's boss is dramatically tankier than a solo one rather than just
// linearly so. Applied in survival_boss_player_scale() below, which counts everyone still on the
// connected roster (survival_shop_all_ready()'s definition of "in the match"), not just players
// currently alive -- a downed-but-not-eliminated player still represents firepower that will be back
// next wave, and the boss should be sized for the whole team, not just whoever's currently standing.
#define SURVIVAL_BOSS_PLAYER_SCALE_PER_PLAYER  (F1_0 * 3 / 2)

// Hard ceiling on any spawned robot's shields, boss or not. `fix` is 16.16 fixed point, so it cannot
// represent more than 32767.99 -- exceed that and the value wraps negative, which reads in play as a
// boss that dies to a single shot. 24000 leaves comfortable headroom under that limit while being far
// more health than any realistic fight needs. See the clamp in survival_spawn_one_robot().
#define SURVIVAL_MAX_ROBOT_SHIELDS    (i2f(24000))

// Netgame difficulty (Difficulty_level, Trainee=0..Insane=4, see game.h)
// scaling. Applied only to wave size, robot pool width and per-instance
// shields, per the design note above survival_limit_robot_speeds(): AI
// pacing stays difficulty-neutral, this mode paces itself through numbers
// and toughness instead. Each factor below is "per level above Trainee",
// so Trainee (0) reproduces the original difficulty-blind numbers exactly.
#define SURVIVAL_DIFFICULTY_ROBOTS_PER_LEVEL     1   // extra base/cap robots per wave, per level
#define SURVIVAL_DIFFICULTY_POOL_PER_LEVEL       1   // extra candidate-pool width, per level
#define SURVIVAL_DIFFICULTY_SHIELD_PCT_PER_LEVEL 5   // extra flat shield %, per level
#define SURVIVAL_SPAWN_TICK           (F1_0 * 3 / 2)   // 1.5s between individual robot spawns within a wave
#define SURVIVAL_INTER_WAVE_DELAY     (F1_0 * 6)        // rest period once a wave's robots are all cleared
#define SURVIVAL_FIRST_WAVE_DELAY     (F1_0 * 10)       // grace period before wave 1 (covers the countdown below)
#define SURVIVAL_COUNTDOWN_FROM        5                // "5..4..3..2..1..GO" before the first wave
#define SURVIVAL_AMMO_INTERVAL        (F1_0 * 30)
#define SURVIVAL_MAX_ACTIVE_ROBOTS    24
#define SURVIVAL_BANNER_DURATION      (F1_0 * 3)

// Spawn placement. Descent segments run roughly 20-40 world units across, so
// ~60 units of separation is "a few rooms apart" and ~30 units from a player
// is far enough that a robot never materialises in somebody's face.
#define SURVIVAL_SPAWN_HISTORY         8
#define SURVIVAL_SPAWN_MIN_SEPARATION  (i2f(60))
#define SURVIVAL_SPAWN_MIN_PLAYER_DIST (i2f(30))
#define SURVIVAL_SPAWN_CANDIDATES      24
// Speed cap for hunting robots, as a fraction of the robot type's Insane-difficulty max_speed (see
// survival_limit_robot_speeds()). This was 3/5 back when the design was "robots amble, players do
// the seeking" -- that premise is gone now that the horde actually hunts you down, and at 3/5 they
// simply took too long to arrive. F1_0 leaves them at the fastest the stock tables ever run a robot.
#define SURVIVAL_ROBOT_SPEED_SCALE    (F1_0)

// Same idea, for Survival's tracked boss robots specifically: buffing shields alone (see
// SURVIVAL_BOSS_SHIELD_MULT) made a boss take longer to kill, but a slow, stationary-feeling fight
// isn't "harder", it's just longer. A visibly faster boss is what makes the fight read as dangerous
// rather than as a bigger damage sponge parked in one spot. 1.5x the already-maxed pack speed
// (SURVIVAL_ROBOT_SPEED_SCALE above), same clamp mechanism, so it's still bounded and can't clip
// through geometry at high FrameTime the way an unbounded speed could.
#define SURVIVAL_BOSS_SPEED_SCALE     (F1_0 * 3 / 2)

// "Elite" robots: visually marked with a colored 3D outline plus a name readout under the model (see
// survival_robot_is_elite() / survival_robot_elite_color()/survival_robot_elite_name(), and their draw-side callers
// do_render_object() in render.c and survival_draw_elite_labels() in gauges.c), and each kind adds a
// distinct twist on top of the type it spawned as. Purely a variation mechanic -- an elite is not
// simply "a tougher version of its type", it plays differently for the seconds it's alive.
//
// The kind is rolled per spawn on the spawner only and shipped in the spawn packet, never re-rolled
// locally, so every machine agrees on which robots are which. Bosses are never elite: they already
// have their own death sequence (start_boss_death_sequence()) which the elite death blast would fight.
//
// The SURVIVAL_ELITE_* kind constants themselves live in survival.h -- survival_robot_is_elite()'s
// return value is part of this mode's public interface (gauges.c and render.c both switch on it), so
// callers outside this file need the same constants rather than a bare int and a guess.
#define SURVIVAL_ELITE_KIND_COUNT      3   // count of the non-NONE kinds in survival.h, for the random pick

#define SURVIVAL_ELITE_CHANCE          (D_RAND_MAX / 7)   // ~1 in 7 of the non-boss spawns
#define SURVIVAL_ELITE_BLAST_SIZE_MULT  3                 // visual radius, vs the robot's own size
#define SURVIVAL_ELITE_BLAST_DAMAGE    (F1_0 * 30)
#define SURVIVAL_ELITE_BLAST_RADIUS    (F1_0 * 45)
#define SURVIVAL_ELITE_BLAST_FORCE     (F1_0 * 200)

// Brute: same per-instance shields multiply survival_spawn_one_robot() already applies for wave and
// difficulty, stacked with one more factor. +100% flat, applied after the normal scaling so it stays
// proportionate at every wave rather than being some fixed bonus that matters less as waves scale up.
#define SURVIVAL_ELITE_BRUTE_SHIELD_MULT   (F1_0 * 2)

// Bounty's own SURVIVAL_BOUNTY_SCORE_BONUS lives next to survival_robot_death_blast() below, where
// the payout actually happens -- there's no spawn-time behaviour to configure here the way Brute's
// shield multiply or Swarmer's fragment counts need.

// Swarmer: splits into this many fragments on death, each a plain (non-elite, so it can't split
// again -- a fragment is never itself a SWARMER) copy of the swarmer's own robot type at reduced
// shields. Capped against SURVIVAL_MAX_ACTIVE_ROBOTS at spawn time the same way a normal wave is.
#define SURVIVAL_SWARM_CHILD_COUNT_MIN     2
#define SURVIVAL_SWARM_CHILD_COUNT_MAX     3
#define SURVIVAL_SWARM_CHILD_SHIELD_FRAC   (F1_0 / 4)

// Kill feedback: a "+points" that floats up from where the robot died.
#define SURVIVAL_MAX_POPUPS           16
#define SURVIVAL_POPUP_LIFE           (F1_0 * 5 / 4)
#define SURVIVAL_POPUP_RISE_PIXELS    26

// Nearest-robot pointer: an arrowhead orbiting the reticle at this radius.
// Suppressed while the target is already on screen inside the radius, so it
// doesn't clutter the crosshair during a face-to-face fight.
#define SURVIVAL_ARROW_RADIUS         44
#define SURVIVAL_ARROW_LENGTH         10
#define SURVIVAL_ARROW_HALF_WIDTH     5

// Distance range the arrow's brightness maps across: at or inside NEAR it is
// at its darkest, at or beyond FAR at its lightest, linear in between.
#define SURVIVAL_ARROW_NEAR_DIST      (i2f(20))
#define SURVIVAL_ARROW_FAR_DIST       (i2f(150))
#define SURVIVAL_ARROW_DARKEST        (F1_0 * 2 / 5)

// Brightness of the darker same-hue accent that backs the arrow and the
// "+points" text, as a fraction of whatever the main color currently is.
#define SURVIVAL_ACCENT_SCALE         (F1_0 * 9 / 20)

// Every player starts the match holding one of these -- see survival_start().
#define SURVIVAL_STARTING_EXTRA_LIVES 1

// Grace window after coming back into the fight -- whether from the whole
// team clearing the wave (survival_revive_all()) or spending a banked extra
// life mid-wave (survival_player_died()) -- so a robot that happens to be
// sitting on the spawn point doesn't erase the revive on the very next
// frame. Implemented by backdating invulnerable_time the same way the
// engine's own quick-invuln cheats do (see do_invulnerable_stuff(),
// game.c), not a new mechanic.
#define SURVIVAL_REVIVE_INVULN_DURATION (F1_0 * 5)

// Sounds for the centered banners. The engine indexes sounds numerically and
// has no name lookup, so each of these is reached through whatever already
// refers to it in the stock sound set:
//   siren01  -- the reactor alarm, i.e. SOUND_CONTROL_CENTER_WARNING_SIREN
//   invulon  -- the invulnerability powerup's own pickup sound
//   mtrl01   -- the matcen materialize, i.e. the robot-morph vclip's sound
#define SURVIVAL_SND_SIREN     SOUND_CONTROL_CENTER_WARNING_SIREN
#define SURVIVAL_SND_GET_READY (Powerup_info[POW_INVULNERABILITY].hit_sound)
#define SURVIVAL_SND_REVIVE    (Vclip[VCLIP_MORPHING_ROBOT].sound_num)

// Pre-wave pause. Boss waves get the longer one because they also get the
// full spoken countdown (survival_do_countdown()).
#define SURVIVAL_BOSS_WAVE_DELAY      (F1_0 * 10)

// Surplus energy trickles back into the hull. Energy caps at 200 but there is
// only so much you can spend it on, whereas in a mode with no author-placed
// pickups (survival_strip_level_powerups()) shields are always scarce -- so
// anything above the halfway mark slowly becomes armour instead of sitting
// there wasted. Deliberately a trickle, not an exchange: it should reward
// hoarding energy over a wave, never act as an instant heal mid-fight.
#define SURVIVAL_ENERGY_SURPLUS_AT    (i2f(100))
#define SURVIVAL_ENERGY_DRAIN_PER_SEC (i2f(5))
#define SURVIVAL_ENERGY_SHIELD_RATIO  (F1_0 / 2)  // shields gained per unit of energy spent

// How often each machine broadcasts its own hull to everyone, for the mini
// health bars under teammates' names (show_HUD_names(), gauges.c). Stock
// multiplayer never tells ordinary clients anyone else's shields -- the
// MULTI_DAMAGE / MULTI_REPAIR packets go to the host only and are applied
// only by observers -- so a co-op mode that wants to show teammate health
// has to say so itself. 4 Hz of a 6-byte packet per player is nothing next
// to the position stream, and it self-corrects any missed update.
#define SURVIVAL_SHIELD_SYNC_INTERVAL (F1_0 / 4)

// Shop: after every SURVIVAL_SHOP_WAVE_INTERVAL-th wave clears, each player
// gets a personal 15s window to spend their own score before a 5s countdown
// runs the match into the next wave. Prices are deliberately steep relative
// to a single robot's score_value (collide.c) -- this is meant to cost
// several waves' worth of kills, not one.
#define SURVIVAL_SHOP_WAVE_INTERVAL       5
#define SURVIVAL_SHOP_DURATION            (F1_0 * 60)   // hard cap; ends sooner once everyone hits ESC -- see survival_shop_all_ready()
#define SURVIVAL_SHOP_COUNTDOWN_DURATION  (F1_0 * 5)

// Base prices, before the per-visit escalation below. Raised well past their original 100/800/1000:
// those were sized against a match where score was mostly cosmetic, and a Survival run now realistically
// banks tens of thousands of points by the time the shop's been visited a handful of times (robot
// kills are the only source, and a wave's worth of them adds up fast -- see survival_robots_for_wave()
// and each robot's own Robot_info[].score_value). Flat prices that size stopped being a choice; these
// are meant to actually compete against each other and against banking points for a later, pricier
// visit.
//
// I don't have this project's actual robot score-value table (it's data-driven, loaded from the HAM/
// HXM file, not something in this source tree) to calibrate against exactly, so these numbers are a
// first-pass estimate sized to "a few thousand points should feel like a real decision, not pocket
// change, by the third or fourth shop visit" -- expect to retune SURVIVAL_SHOP_PRICE_* and
// SURVIVAL_SHOP_VISIT_ESCALATION_PERMILLE below after playing a few matches through.
//
// Weapon and Supply raised a second pass beyond that first estimate -- being the two repeatable,
// no-cap buys (Shield Restore is repeatable too but gated by SURVIVAL_SHOP_SHIELD_RESTORE_CAP, so it
// self-limits how often it's worth buying; these two don't), they're the ones score actually pools up
// for, so they're the two that most needed to keep costing something.
#define SURVIVAL_SHOP_PRICE_WEAPON        1400
#define SURVIVAL_SHOP_PRICE_SUPPLY        6000
#define SURVIVAL_SHOP_PRICE_SHIELD_FULL   4000

// Deliberately below the engine's real MAX_SHIELDS (200, player.h) -- the
// shop sells a partial patch-up, not a second full tank.
#define SURVIVAL_SHOP_SHIELD_RESTORE_CAP  i2f(100)

// How much every priced item (all five below) marks up per shop visit -- see Survival_shop_visit_
// count and survival_shop_scale_price(). 250 = +25% a visit: by the 4th visit (wave 20, at the
// default SURVIVAL_SHOP_WAVE_INTERVAL of 5) prices are at 1.75x their base, so a player who spends
// freely every visit keeps feeling the pinch instead of the shop going stale once score outgrows it.
#define SURVIVAL_SHOP_VISIT_ESCALATION_PERMILLE  250

// Physics upgrades are capped so score can't buy an unkillable, unstoppable ship outright. Unlike the
// three repeatable buys above, these are also priced per-tier (PRICE_STEP), independent of the visit
// escalation, since a player very often maxes both out within their first shop or two -- the visit
// multiplier alone wouldn't have climbed far enough yet to make the 2nd and 3rd tiers cost more than
// the 1st.
#define SURVIVAL_SHOP_SPEED_MAX_TIER      3
#define SURVIVAL_SHOP_SPEED_BASE_PRICE    9000
#define SURVIVAL_SHOP_SPEED_PRICE_STEP    2500
// +3.5% thrust per tier. Stored in tenths of a percent (35 = 3.5%) since
// SURVIVAL_SHOP_SPEED_PCT_PER_TIER as a whole percent can't express the
// ".5" -- survival_speed_multiplier() divides by 1000, not 100.
#define SURVIVAL_SHOP_SPEED_PERMILLE_PER_TIER  35

#define SURVIVAL_SHOP_ARMOR_MAX_TIER      3
#define SURVIVAL_SHOP_ARMOR_BASE_PRICE    11000
#define SURVIVAL_SHOP_ARMOR_PRICE_STEP    3000
#define SURVIVAL_SHOP_ARMOR_PCT_PER_TIER  10   // -10% incoming damage per tier

static int Survival_wave = 0;
static int Survival_wave_is_boss = 0;
static int Survival_wave_in_progress = 0;
static int Survival_robots_to_spawn = 0;
static fix64 Survival_next_spawn_at = 0;
static fix64 Survival_next_wave_at = 0;
static fix64 Survival_next_ammo_at = 0;

// The two deadlines above are absolute GameTime64 values, and GameTime64 is
// reset to 0 by StartNewLevel() -- which runs *after* survival_start(), since
// the mode is chosen (net_udp_set_game_mode) before the level is entered. So
// they cannot be computed in survival_start(): whatever GameTime64 was left
// over from a previous level would be baked into them, and after the reset
// the opening wave would be that far into the future. That is what made the
// match sometimes start on time (fresh launch, GameTime64 near 0) and
// sometimes take minutes or appear never to start at all (hosting again
// after a previous match). They're armed instead on the first frame that
// actually runs inside the level, when GameTime64 is meaningful.
static int Survival_timers_armed = 0;
static int Survival_game_over = 0;

// Banked self-revives held by the local player, from POW_EXTRA_LIFE drops.
// Purely local state -- survival_player_died() is only ever called for
// Player_num, and spending a life means this machine simply never sends the
// MULTI_SURVIVAL_ELIMINATED packet, so there is nothing for anyone else to
// track. (Players[].lives is deliberately not reused for this: nothing in
// multiplayer decrements it, and every other mode's HUD/endgame code reads
// it with its own meaning.)
static int Survival_extra_lives = 0;

// Set by survival_player_died() when an extra life is spent instead of going
// down: that path's respawn (init_player_stats_new_ship() + StartLevel(1))
// runs in gameseq.c, *after* survival_player_died() returns, and clears
// PLAYER_FLAGS_INVULNERABLE as part of the normal fresh-ship reset -- so the
// revive-invulnerability grant has to happen after that call too, or it's
// wiped the instant it's set. This flag is how gameseq.c's call back into
// survival_maybe_grant_revive_invulnerability() knows a grant is owed.
static int Survival_pending_revive_invuln = 0;

// Fractional shield repair from surplus energy that hasn't been reported to
// observers yet -- see survival_convert_surplus_energy().
static fix Survival_repair_pending = 0;

static fix64 Survival_next_shield_send = 0;

// Shop phase state. The phase itself opens off the same synced wave-clear
// transition every machine already reaches independently (see the
// wave-clear block in survival_do_frame()), so no dedicated packet is
// needed for that part, same reasoning as the wave countdown clock.
// Purchases only ever touch the local player's own score/shields/tiers,
// which are already covered by existing sync (MULTI_SCORE, MULTI_REPAIR) or
// don't need syncing at all (tiers only affect how this client computes its
// own thrust/damage). Readiness is the one piece that genuinely needs a
// packet (MULTI_SURVIVAL_SHOP_READY): "has everyone hit ESC" is a fact about
// every player, not something each machine can derive from its own state.
typedef enum {
	SURVIVAL_SHOP_PHASE_NONE = 0,
	SURVIVAL_SHOP_PHASE_OPEN,       // buying window; keys/mouse diverted to the shop
	SURVIVAL_SHOP_PHASE_COUNTDOWN   // everyone's ready (or the cap ran out); waiting out the last few seconds
} survival_shop_phase_t;

static survival_shop_phase_t Survival_shop_phase = SURVIVAL_SHOP_PHASE_NONE;
static fix64 Survival_shop_open_until = 0;   // hard cap: buying window closes no later than this regardless of readiness
static int Survival_shop_last_wave = -1;     // which wave-clear already opened a shop, so the transition block can't retrigger it every frame
static int Survival_speed_tier = 0;
static int Survival_armor_tier = 0;

// How many times the shop has opened this match. Incremented once per SURVIVAL_SHOP_PHASE_OPEN
// transition (the wave-clear block in survival_do_frame() below), which runs identically on every
// machine off the same synced wave-clear condition every other shop-open decision already relies on
// -- see survival_wave_opens_shop() -- so no dedicated sync packet is needed for this either. Feeds
// survival_shop_scale_price().
static int Survival_shop_visit_count = 0;

// Who has hit ESC to leave the shop. Broadcast via MULTI_SURVIVAL_SHOP_READY
// so every machine -- not just the spawner -- can independently tell when
// every currently-connected player is ready and end the wait early instead
// of always sitting out the full SURVIVAL_SHOP_DURATION cap. Reset whenever
// a new shop opens (see the wave-clear block in survival_do_frame()).
static ubyte Survival_shop_ready[MAX_PLAYERS];

// Mouse hit-testing for the shop panel. Rects are stamped by
// survival_shop_draw() every frame the shop is open (screen-space,
// canvas-offset already folded in -- see survival_shop_draw_row()) and read
// back by survival_shop_do_mouse(); Survival_shop_mouse_was_down debounces
// the left button so a held click buys once instead of every frame.
#define SURVIVAL_SHOP_NUM_ITEMS 5
typedef struct { int x1, y1, x2, y2; } survival_shop_rect_t;
static survival_shop_rect_t Survival_shop_row_rect[SURVIVAL_SHOP_NUM_ITEMS];
static int Survival_shop_mouse_was_down = 0;

// Kill feedback state, all purely local and purely cosmetic: each machine
// only ever records its own kills, so none of this is synced.
typedef struct {
	vms_vector pos;   // where the robot died
	int points;
	fix64 born;       // 0 = free slot
} survival_kill_popup;
static survival_kill_popup Survival_popups[SURVIVAL_MAX_POPUPS];
static int Survival_popup_idx = 0;

static ubyte Survival_eliminated[MAX_PLAYERS];

// Tracks currently-alive boss robots for the HP bar gauges.c draws under
// them (survival_get_active_bosses() below). Populated on every machine --
// spawner and receivers alike -- from the same is_boss byte on the
// MULTI_SURVIVAL_SPAWN_ROBOT wire packet, so the bar shows consistently for
// everyone, not just whoever's hosting.
#define SURVIVAL_MAX_TRACKED_BOSSES 8
typedef struct {
	short objnum; // -1 = empty slot
	fix max_shields;
} survival_boss_track;
static survival_boss_track Survival_bosses[SURVIVAL_MAX_TRACKED_BOSSES];

// Ring buffer of the last few spawn *points* (not just segments): new spawns
// are placed as far from these as the level allows, which is what keeps a
// wave from arriving as one clump. Positions rather than segnums because
// "different segment" is nowhere near far enough -- adjacent segments are
// touching.
static vms_vector Survival_recent_spawn_pos[SURVIVAL_SPAWN_HISTORY];
static int Survival_recent_spawn_used[SURVIVAL_SPAWN_HISTORY];
static int Survival_recent_spawn_idx = 0;

static char Survival_banner_text[48];
static fix64 Survival_banner_until = 0;
static int Survival_hud_prev_wave = -1;
static int Survival_hud_prev_in_progress = -1;

// Separate from the HUD's transition tracker above: that one lives in the
// render path, and reviving downed players is game state, not drawing. Both
// key off Survival_wave_in_progress, which is synced to every machine (see
// multi_do_survival_wave_state), so each machine independently reaches the
// same revive decision on the same wave -- no extra packet needed.
static int Survival_revive_prev_in_progress = -1;

// Last countdown tick we voiced before the coming wave, so each number is
// spoken once. -1 = nothing said yet. Purely local: every machine sets its
// own Survival_next_wave_at when it sees the previous wave end (see
// survival_do_frame), so all of them tick through the same numbers without
// needing a countdown packet.
static int Survival_countdown_last_spoken = -1;

// Whether the "GET READY..." / "BOSS INCOMING" banner for the wave we're
// currently waiting on has already been shown. Cleared alongside the
// countdown at every wave transition.
static int Survival_prewave_announced = 0;

// All candidates sorted ascending by base strength (shields), built lazily
// once per match from whatever robot set the loaded mission has -- there's
// no existing "difficulty tier" metadata per robot type to draw on, so this
// is the closest cheap proxy: early waves only draw from the weakest few,
// the eligible pool widens as the wave number climbs, and "boss" waves
// (see survival_pick_robot_type() below) draw from a hulk-named or
// otherwise boss-worthy slice of this same list instead of needing a
// dedicated boss robot type.
static int Survival_candidates[MAX_ROBOT_TYPES];
static int Survival_num_candidates = 0;
static int Survival_tables_built = 0;

// Subset of Survival_candidates whose Robot_names[] contains "hulk" --
// preferred pool for boss waves, see survival_pick_robot_type(). Further
// filtered to survival_robot_is_boss_worthy() below: a name match alone
// isn't enough, since a "hulk" that can't actually hurt the player or
// folds like the rest of the wave still gets randomly drawn from this list
// and shows up feeling like a non-event instead of a boss.
static int Survival_hulk_candidates[MAX_ROBOT_TYPES];
static int Survival_num_hulk_candidates = 0;

// Every survival_robot_is_boss_worthy() candidate, hulk-named or not -- the fallback pool for boss
// waves when the loaded robot set has no "hulk" match at all (see survival_pick_robot_type()).
// Replaces the old "top 1/4 of candidates by strength alone" heuristic, which had the same harmless-
// robot problem the hulk list did: strength (shields) says nothing about whether a robot can fight
// back.
static int Survival_boss_worthy_candidates[MAX_ROBOT_TYPES];
static int Survival_num_boss_worthy_candidates = 0;

// A robot qualifies as boss material on two independent axes, both checked
// against data already loaded for the type -- nothing here is guessed or
// hardcoded to a specific robot set:
//
//   Can it actually hurt you? n_guns == 0 with attack_type != 1 (charge/
//   contact, e.g. green guy) means the robot has no ranged attack and no
//   melee attack either -- it is, mechanically, unable to damage the
//   player at all. That robot being tanky is irrelevant if the fight is
//   just flying around a stationary target.
//
//   Is it actually tanky? Compared against the *median* strength of the
//   whole boss_flag-free candidate pool (Survival_candidates[], already
//   sorted ascending by strength when this runs) rather than a fixed
//   number, so this self-calibrates to whatever robot set is loaded
//   instead of a constant that would be right for one HAM and wrong for
//   another.
static int survival_robot_is_boss_worthy(int id)
{
	robot_info *ri = &Robot_info[id];
	fix median_strength;

	if (ri->n_guns == 0 && ri->attack_type != 1)
		return 0;

	if (Survival_num_candidates == 0)
		return 1; // nothing to compare against yet -- let the caller sort it out

	median_strength = Robot_info[Survival_candidates[Survival_num_candidates / 2]].strength;
	return ri->strength >= median_strength;
}

// Sustain drops: the stuff that keeps you alive rather than arming you.
// Rolled completely independently of the weapon table below, so a dry spell
// on weapons never also starves you of shields/energy.
static const int Survival_ammo_types[] = { POW_ENERGY, POW_SHIELD_BOOST, POW_SHIELD_BOOST, POW_VULCAN_AMMO };
#define SURVIVAL_NUM_AMMO_TYPES (sizeof(Survival_ammo_types) / sizeof(Survival_ammo_types[0]))

// What the shop's "Random Supply" can roll. Deliberately a separate table
// from Survival_ammo_types[] above rather than an extension of it: that one
// also feeds robot death drops and the timed floor drops, so folding
// invulnerability and cloak into it would start scattering them all over
// the mine for free. Here they're something you can only ever pay for.
// Weighted by repetition, same as the weapon table.
static const int Survival_shop_supply_types[] = {
	POW_ENERGY, POW_ENERGY,
	POW_SHIELD_BOOST, POW_SHIELD_BOOST,
	POW_VULCAN_AMMO, POW_VULCAN_AMMO,
	POW_INVULNERABILITY,
	POW_CLOAK,
};
#define SURVIVAL_NUM_SHOP_SUPPLY_TYPES (sizeof(Survival_shop_supply_types) / sizeof(Survival_shop_supply_types[0]))

// Everything a robot can arm you with. Since Survival strips the level of
// its own powerups (see survival_strip_level_powerups()), this table is the
// *only* way to obtain weapons all match -- so it has to span the full set,
// primaries and secondaries alike. Weighted by repetition: the workhorse
// pickups appear more than once, the match-winners appear once.
static const int Survival_weapon_types[] = {
	// Primaries
	POW_LASER, POW_LASER,
	POW_VULCAN_WEAPON,
	POW_SPREADFIRE_WEAPON,
	POW_PLASMA_WEAPON,
	POW_FUSION_WEAPON,
	POW_QUAD_FIRE,
	// Secondaries
	POW_MISSILE_1, POW_MISSILE_1,
	POW_MISSILE_4,
	POW_HOMING_AMMO_1,
	POW_HOMING_AMMO_4,
	POW_PROXIMITY_WEAPON,
	POW_SMARTBOMB_WEAPON,
	POW_MEGA_WEAPON,
	// Situational (POW_CLOAK, POW_INVULNERABILITY) deliberately not here --
	// see SURVIVAL_SITUATIONAL_DROP_PERMILLE, survival.h.
};
#define SURVIVAL_NUM_WEAPON_TYPES (sizeof(Survival_weapon_types) / sizeof(Survival_weapon_types[0]))

// Same election as multi_arcade_spawner_pnum() (multi.c): normally the
// host, falling back to the lowest connected player if the host itself is
// an observer (hosts-as-observer has no player object to anchor spawns to).
static int survival_spawner_pnum(void)
{
	int i;

	if (!Netgame.host_is_obs && Players[multi_who_is_master()].connected == CONNECT_PLAYING)
		return multi_who_is_master();

	for (i = 0; i < N_players; i++)
		if (i != multi_who_is_master() && Players[i].connected == CONNECT_PLAYING)
			return i;

	return -1;
}

static int survival_count_active_robots(void)
{
	int i, count = 0;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_ROBOT && !(Objects[i].flags & OF_SHOULD_BE_DEAD))
			count++;

	return count;
}

// Case-insensitive substring test -- no strcasestr() dependency (not
// portable to the Windows build).
static int survival_name_contains_ci(const char *haystack, const char *needle)
{
	int hlen = (int)strlen(haystack);
	int nlen = (int)strlen(needle);
	int i, j;

	for (i = 0; i + nlen <= hlen; i++)
	{
		for (j = 0; j < nlen; j++)
			if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
				break;
		if (j == nlen)
			return 1;
	}

	return 0;
}

static void survival_build_robot_tables(void)
{
	int i, j, key;
	fix key_strength;

	if (Survival_tables_built)
		return;

	Survival_num_candidates = 0;

	for (i = 0; i < N_robot_types; i++)
	{
		// Exclude the level's real end-of-mine guardian(s): boss_flag robots
		// are scripted set-pieces (self-destruct sequence, oversized/
		// multi-part models built for their own dedicated boss arena, not
		// regular mine corridors) -- spawning one as a random mob is exactly
		// what made earlier "boss wave = pick a boss_flag robot" attempts
		// end up too big to move anywhere. Never eligible, boss wave or not.
		if (Robot_info[i].boss_flag)
			continue;

		Survival_candidates[Survival_num_candidates++] = i;
	}

	// Insertion sort by strength, ascending. N is tiny (<=30) so this
	// doesn't need to be anything cleverer.
	for (i = 1; i < Survival_num_candidates; i++)
	{
		key = Survival_candidates[i];
		key_strength = Robot_info[key].strength;
		j = i - 1;
		while (j >= 0 && Robot_info[Survival_candidates[j]].strength > key_strength)
		{
			Survival_candidates[j + 1] = Survival_candidates[j];
			j--;
		}
		Survival_candidates[j + 1] = key;
	}

	// Boss-worthy general pool first (median-strength-and-can-fight-back filter -- see
	// survival_robot_is_boss_worthy() above; run after the sort immediately above since that
	// function compares against Survival_candidates' median strength), then the "hulk"-named subset
	// of *that* rather than of the unfiltered list: a name match no longer overrides the worthiness
	// check, it narrows an already-qualified pool. survival_pick_robot_type() prefers the hulk list
	// when it's non-empty and falls back to the general boss-worthy pool otherwise.
	Survival_num_boss_worthy_candidates = 0;
	for (i = 0; i < Survival_num_candidates; i++)
		if (survival_robot_is_boss_worthy(Survival_candidates[i]))
			Survival_boss_worthy_candidates[Survival_num_boss_worthy_candidates++] = Survival_candidates[i];

	Survival_num_hulk_candidates = 0;
	for (i = 0; i < Survival_num_boss_worthy_candidates; i++)
		if (survival_name_contains_ci(Robot_names[Survival_boss_worthy_candidates[i]], "hulk"))
			Survival_hulk_candidates[Survival_num_hulk_candidates++] = Survival_boss_worthy_candidates[i];

	Survival_tables_built = 1;
}

// Picks the strongest entry of a candidate list, or (half the time) any entry at random for some
// variety. Shared by the hulk and general-boss-worthy pools below -- same selection shape, different
// list.
static int survival_pick_strongest_or_random(const int *list, int count)
{
	if (d_rand() < 16384)
	{
		int strongest = list[0];
		int k;
		for (k = 1; k < count; k++)
			if (Robot_info[list[k]].strength > Robot_info[strongest].strength)
				strongest = list[k];
		return strongest;
	}

	return list[(d_rand() * count) >> 15];
}

// Boss waves draw from Survival_hulk_candidates when the loaded robot set has any (regular waves
// never do -- hulks are a strict subset of the general pool by strength/toughness already, so
// reserving them for bosses keeps a boss actually feeling distinct), falling back to the wider
// Survival_boss_worthy_candidates when there's no hulk-named robot in the set at all. Both lists are
// already filtered to survival_robot_is_boss_worthy() -- durable *and* actually able to fight back --
// so unlike before, there's no path left that can hand a boss wave something harmless to spawn. Still
// a completely normal robot object either way: normal model, normal size, normal AI (see the harder-
// fire-rate/turn-speed hooks in ai.c for what does make it fight differently as a boss).
static int survival_pick_robot_type(int wave, int is_boss)
{
	int pool_size, index;

	survival_build_robot_tables();

	if (Survival_num_candidates == 0)
		return -1;

	if (is_boss)
	{
		if (Survival_num_hulk_candidates > 0)
			return survival_pick_strongest_or_random(Survival_hulk_candidates, Survival_num_hulk_candidates);

		if (Survival_num_boss_worthy_candidates > 0)
			return survival_pick_strongest_or_random(Survival_boss_worthy_candidates, Survival_num_boss_worthy_candidates);

		// Nothing in the whole robot set clears survival_robot_is_boss_worthy() -- every type is
		// either unarmed or below-median strength (a tiny or unusual robot set). Falling all the way
		// through to "spawn nothing" would mean boss waves just silently stop happening, which is a
		// worse failure than an undersized boss, so take the toughest single candidate regardless.
		return Survival_candidates[Survival_num_candidates - 1];
	}

	pool_size = 2 + wave / 2 + Difficulty_level * SURVIVAL_DIFFICULTY_POOL_PER_LEVEL;
	if (pool_size > Survival_num_candidates)
		pool_size = Survival_num_candidates;

	index = (d_rand() * pool_size) >> 15;
	return Survival_candidates[index];
}

static int survival_robots_for_wave(int wave)
{
	int bonus = Difficulty_level * SURVIVAL_DIFFICULTY_ROBOTS_PER_LEVEL;
	int base = SURVIVAL_WAVE_ROBOTS_BASE + bonus;
	int max = SURVIVAL_WAVE_ROBOTS_MAX + bonus;
	int n = base + wave;
	return n > max ? max : n;
}

static void survival_track_boss(int objnum, fix max_shields)
{
	int i;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		if (Survival_bosses[i].objnum < 0)
		{
			Survival_bosses[i].objnum = (short)objnum;
			Survival_bosses[i].max_shields = max_shields;
			return;
		}
	// Table full -- more concurrent bosses than we ever actually spawn at
	// once. Not fatal: this one just won't get an HP bar.
}

// True if this object is one of the boss-wave robots we're tracking. Used by
// the drop table to guarantee a boss's extra life; safe to call while the
// object is mid-explosion, since entries are only pruned by
// survival_get_active_bosses() on a later render frame.
static int survival_is_tracked_boss(int objnum)
{
	int i;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		if (Survival_bosses[i].objnum == objnum)
			return 1;

	return 0;
}

// Public wrapper for ai.c's boss-only AI hooks (faster fire rate, quicker turning -- see
// set_next_fire_time() and survival_boss_turn_time() there). Safe to call on anything: 0 outside
// Survival, same as survival_robot_is_elite().
int survival_robot_is_boss(int objnum)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return 0;

	return survival_is_tracked_boss(objnum);
}

// How many boss waves have happened, counting the current one: 1 at wave 10, 2 at wave 20, 3 at wave
// 30, and so on (see SURVIVAL_BOSS_WAVE_INTERVAL above). Every boss lives entirely inside its own
// wave -- Survival_wave doesn't advance again until the previous wave's robots are all gone, which
// for a boss wave means the boss itself -- so reading the global Survival_wave from inside ai.c's
// per-frame AI hooks (rather than needing this stamped on the boss at spawn time) always gives the
// tier of whichever boss is currently alive. Feeds the escalating fire-rate/turn-speed hooks in ai.c
// (survival_boss_scale() there): each successive boss is meant to fight harder than the last, not
// just have more shields.
int survival_boss_tier(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return 0;

	return Survival_wave / SURVIVAL_BOSS_WAVE_INTERVAL;
}

// Prunes dead/gone entries and hands back objnums + remaining-shields
// fraction (0..F1_0) for whatever bosses are still alive, for gauges.c to
// draw a bar under. Safe to call every HUD frame.
int survival_get_active_bosses(short *out_objnums, fix *out_fracs, int max_out)
{
	int i, count = 0;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
	{
		int objnum = Survival_bosses[i].objnum;

		if (objnum < 0)
			continue;

		if (objnum > Highest_object_index || Objects[objnum].type != OBJ_ROBOT || (Objects[objnum].flags & OF_SHOULD_BE_DEAD))
		{
			Survival_bosses[i].objnum = -1;
			continue;
		}

		if (count < max_out)
		{
			out_objnums[count] = (short)objnum;
			out_fracs[count] = (Survival_bosses[i].max_shields > 0) ? fixdiv(Objects[objnum].shields, Survival_bosses[i].max_shields) : 0;
			count++;
		}
	}

	return count;
}

// Which robots are elite, by objnum, and which kind (SURVIVAL_ELITE_NONE if not). Deliberately not
// derived from anything -- it is set explicitly on every spawn path (spawner and receiver alike) so
// a recycled objnum can never inherit the previous occupant's kind, and survival_robot_is_elite()
// re-checks the object is still a robot.
static ubyte Survival_robot_elite_kind[MAX_OBJECTS];

// Returns the elite kind (SURVIVAL_ELITE_BOUNTY etc, or SURVIVAL_ELITE_NONE) for objnum. Safe to
// call on anything -- 0 outside Survival, for a stale/out-of-range objnum, or for a non-robot -- so
// callers never need their own guard in front of it.
int survival_robot_is_elite(int objnum)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return SURVIVAL_ELITE_NONE;
	if (objnum < 0 || objnum > Highest_object_index)
		return SURVIVAL_ELITE_NONE;
	if (Objects[objnum].type != OBJ_ROBOT)
		return SURVIVAL_ELITE_NONE;

	return Survival_robot_elite_kind[objnum];
}

// Outline/label color for a given kind. -1 for SURVIVAL_ELITE_NONE, matching g3d_interp_outline_
// color's own "don't touch the model's colour" sentinel, so a caller that forgot to check for NONE
// first still does nothing harmful.
int survival_robot_elite_color(int kind)
{
	switch (kind)
	{
		case SURVIVAL_ELITE_BOUNTY:  return BM_XRGB(31, 31, 0);   // yellow/gold
		case SURVIVAL_ELITE_BRUTE:   return BM_XRGB(31, 16, 0);   // orange
		case SURVIVAL_ELITE_SWARMER: return BM_XRGB(24, 0, 31);   // purple
		default:                     return -1;
	}
}

// Label text for a given kind, for the readout under the model (gauges.c). "" for
// SURVIVAL_ELITE_NONE, so a caller that forgot to check first draws nothing rather than garbage.
const char *survival_robot_elite_name(int kind)
{
	switch (kind)
	{
		case SURVIVAL_ELITE_BOUNTY:  return "BOUNTY";
		case SURVIVAL_ELITE_BRUTE:   return "BRUTE";
		case SURVIVAL_ELITE_SWARMER: return "SWARMER";
		default:                     return "";
	}
}

static void survival_swarm_split(object *robot);
static void survival_spawn_score_popup(vms_vector *pos, int points);

// Flat score bonus for downing a BOUNTY, on top of its normal Robot_info[].score_value kill award --
// what makes it worth being a priority target rather than just a differently-colored version of
// whatever it spawned as. Comparable to a base-price shop weapon buy (SURVIVAL_SHOP_PRICE_WEAPON) on
// purpose: one bounty kill should feel like it meaningfully funded the next shop visit.
#define SURVIVAL_BOUNTY_SCORE_BONUS  1000

// The elite's payoff. Runs on every machine off the synced kind rather than being broadcast on
// death, so it needs no packet of its own -- multi_explode_robot_sub() (multibot.c) already runs
// everywhere for the same robot, and passes killer through from there for exactly this: the BOUNTY
// bonus below has to land on one player's score, not every machine's, and the objnum-vs-Players[
// Player_num].objnum check is the same one the ordinary kill-score award next to survival_note_
// robot_kill() (multibot.c/collide.c) already uses to decide "was *I* the one who got this kill".
//
// The blast is *added* to the normal death explosion rather than replacing it: stock's
// explode_object() still does the debris and the death bookkeeping, and this lays a badass blast
// over the top. Parent is the robot itself, so the blast hurts whoever is standing next to it --
// every kind gets this, not just the ones with their own extra twist, because "elites go out harder"
// is the baseline the individual kinds build on. A swarmer additionally scatters fragments; a bounty
// additionally pays out; see survival_swarm_split() and the bonus block below respectively.
void survival_robot_death_blast(object *robot, int killer)
{
	int kind = survival_robot_is_elite(robot - Objects);
	object *expl;

	if (kind == SURVIVAL_ELITE_NONE)
		return;

	expl = object_create_badass_explosion(robot, robot->segnum, &robot->pos,
			robot->size * SURVIVAL_ELITE_BLAST_SIZE_MULT,
			get_explosion_vclip(robot, 0),
			SURVIVAL_ELITE_BLAST_DAMAGE, SURVIVAL_ELITE_BLAST_RADIUS, SURVIVAL_ELITE_BLAST_FORCE,
			robot - Objects);

	if (expl)
		digi_link_sound_to_object(SOUND_BADASS_EXPLOSION, expl - Objects, 0, F1_0);

	if (kind == SURVIVAL_ELITE_SWARMER)
		survival_swarm_split(robot);

	if (kind == SURVIVAL_ELITE_BOUNTY && !is_observer() && killer == Players[Player_num].objnum)
	{
		add_points_to_score(SURVIVAL_BOUNTY_SCORE_BONUS);
		survival_spawn_score_popup(&robot->pos, SURVIVAL_BOUNTY_SCORE_BONUS);
	}

	Survival_robot_elite_kind[robot - Objects] = SURVIVAL_ELITE_NONE;
}

// Spawner only. Wire format: [type:1][pnum:1][objnum:2][segnum:2][robot_type:1][shields:4][pos:12][flags:1] = 24 bytes.
// The trailing byte was a bare is_boss 0/1 and is now a bitfield -- bit 0 boss, bits 1-2 the elite
// kind (0-3, SURVIVAL_ELITE_* above) -- which keeps the packet the same size rather than growing it.
#define SURVIVAL_SPAWN_FLAG_BOSS       1
#define SURVIVAL_SPAWN_ELITE_SHIFT     1
#define SURVIVAL_SPAWN_ELITE_MASK      (3 << SURVIVAL_SPAWN_ELITE_SHIFT)

static void survival_send_spawn_robot(int objnum, int segnum, int robot_type, fix shields, vms_vector *pos, int is_boss, int elite_kind)
{
#ifdef WORDS_BIGENDIAN
	vms_vector swapped_vec;
#endif
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_SPAWN_ROBOT;		count += 1;
	multibuf[count] = Player_num;				count += 1;
	PUT_INTEL_SHORT(multibuf + count, objnum);		count += 2;
	PUT_INTEL_SHORT(multibuf + count, segnum);		count += 2;
	multibuf[count] = (ubyte)robot_type;			count += 1;
	PUT_INTEL_INT(multibuf + count, shields);		count += 4;
#ifndef WORDS_BIGENDIAN
	memcpy(multibuf + count, pos, sizeof(vms_vector));	count += sizeof(vms_vector);
#else
	swapped_vec.x = (fix)INTEL_INT((int)pos->x);
	swapped_vec.y = (fix)INTEL_INT((int)pos->y);
	swapped_vec.z = (fix)INTEL_INT((int)pos->z);
	memcpy(multibuf + count, &swapped_vec, 12);		count += 12;
#endif
	multibuf[count] = (ubyte)((is_boss ? SURVIVAL_SPAWN_FLAG_BOSS : 0) |
				  ((elite_kind << SURVIVAL_SPAWN_ELITE_SHIFT) & SURVIVAL_SPAWN_ELITE_MASK));	count += 1;

	multi_send_data(multibuf, count, 2);

	if (Network_send_objects && multi_objnum_is_past(objnum))
		Network_send_objnum = -1;
}

void multi_do_survival_spawn_robot(const ubyte *buf)
{
	int count = 1;
	int pnum, segnum, robot_type, spawn_flags;
	short objnum;
	fix shields;
	vms_vector pos;
	object *obj;

	if (Endlevel_sequence || Control_center_destroyed)
		return;

	pnum = buf[count++];
	objnum = GET_INTEL_SHORT(buf + count); count += 2;
	segnum = GET_INTEL_SHORT(buf + count); count += 2;
	robot_type = buf[count++];
	shields = (fix)GET_INTEL_INT(buf + count); count += 4;

	// memcpy, not `pos = *(vms_vector *)(buf + count)`. pos sits at offset 11 in this packet, which is
	// not 4-byte aligned, and vms_vector is three 32-bit fixes -- casting a misaligned pointer and
	// dereferencing it is undefined behaviour. x86 happens to permit unaligned loads so it works on
	// the desktop builds, but on a strict-alignment target (ARM) it is a SIGBUS or a silently wrong
	// read. memcpy is the portable spelling and compiles to the same thing where it is safe anyway.
	memcpy(&pos, buf + count, sizeof(vms_vector)); count += sizeof(vms_vector);
#ifdef WORDS_BIGENDIAN
	pos.x = (fix)SWAPINT((int)pos.x);
	pos.y = (fix)SWAPINT((int)pos.y);
	pos.z = (fix)SWAPINT((int)pos.z);
#endif
	spawn_flags = buf[count++];

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;
	if (segnum < 0 || segnum > Highest_segment_index)
		return;
	if (robot_type < 0 || robot_type >= N_robot_types)
		return;

	// objnum comes straight off the wire and is handed to map_objnum_local_to_remote(), which indexes
	// remote_to_local[owner][remote_objnum] with it. That function only guards the index with an
	// Assert(), and Assert() compiles out under NDEBUG -- so in a release build a corrupt or hostile
	// packet carrying an out-of-range objnum is an out-of-bounds *write* into that table. Validate it
	// here, where we still can. (It is declared short, so a value above 32767 arrives negative; the
	// low check catches that case too.)
	if (objnum < 0 || objnum >= MAX_OBJECTS)
		return;

	obj = create_morph_robot(&Segments[segnum], &pos, robot_type);
	if (!obj)
		return;

	// Also unvalidated wire data. Negative shields make the robot die to the first thing that touches
	// it; absurdly large ones make it unkillable and, past `fix`'s 32767 ceiling, wrap negative anyway.
	// Same clamp the spawner applies in survival_spawn_one_robot().
	if (shields < 1)
		shields = 1;
	if (shields > SURVIVAL_MAX_ROBOT_SHIELDS)
		shields = SURVIVAL_MAX_ROBOT_SHIELDS;

	obj->shields = shields;
	morph_start(obj);
	map_objnum_local_to_remote(obj - Objects, objnum, pnum);

	//	Assigned unconditionally, not just when the bits are nonzero, so this objnum can't inherit an
	//	elite kind left behind by whatever robot occupied the slot before it.
	Survival_robot_elite_kind[obj - Objects] =
		(spawn_flags & SURVIVAL_SPAWN_ELITE_MASK) >> SURVIVAL_SPAWN_ELITE_SHIFT;

	if (spawn_flags & SURVIVAL_SPAWN_FLAG_BOSS)
		survival_track_boss(obj - Objects, shields);
}

static void survival_remember_spawn_pos(vms_vector *pos)
{
	Survival_recent_spawn_pos[Survival_recent_spawn_idx] = *pos;
	Survival_recent_spawn_used[Survival_recent_spawn_idx] = 1;
	Survival_recent_spawn_idx = (Survival_recent_spawn_idx + 1) % SURVIVAL_SPAWN_HISTORY;
}

// Distance to the nearest of the last few spawn points -- the thing we're
// maximizing when placing a new one. Effectively infinite before anything
// has spawned, so the first robot of a match goes wherever it likes.
static fix survival_dist_to_recent_spawns(vms_vector *pos)
{
	int i;
	fix best = 0x7fffffff;

	for (i = 0; i < SURVIVAL_SPAWN_HISTORY; i++)
	{
		fix d;

		if (!Survival_recent_spawn_used[i])
			continue;

		d = vm_vec_dist_quick(pos, &Survival_recent_spawn_pos[i]);
		if (d < best)
			best = d;
	}

	return best;
}

static fix survival_dist_to_players(vms_vector *pos)
{
	int i;
	fix best = 0x7fffffff;

	for (i = 0; i < N_players; i++)
	{
		fix d;

		if (Players[i].connected == CONNECT_DISCONNECTED)
			continue;
		if (survival_is_eliminated(i))
			continue; // spectating, not a real presence in the mine

		d = vm_vec_dist_quick(pos, &Objects[Players[i].objnum].pos);
		if (d < best)
			best = d;
	}

	return best;
}

// choose_drop_segment() reseeds d_rand() from timer_query() at the top of
// every call, so calling it again inside a tight retry loop (same frame,
// same millisecond) reseeds to the same value and returns the identical
// segment every time -- that made the original retry-loop version of this
// function a no-op, which is why robots kept landing in the same segment.
// So it's called exactly once here, for a starting point, and every
// candidate is then generated by walking a random number of steps through
// real level topology from it with d_rand() directly (not reseeded again
// after that one call, so successive draws actually differ).
//
// Candidates are scored by how far they are from the last few spawn points
// and rejected outright if they're on top of a player; the best one wins,
// with an early exit as soon as one is comfortably separated. Rejecting only
// exact segment repeats (the previous version) wasn't nearly enough -- most
// of the segments reachable in a few steps are still within the same room,
// so waves kept arriving in a clump.
static int survival_choose_spawn_point(vms_vector *out_pos)
{
	int base, best_seg = -1, attempt;
	fix best_score = -1;
	vms_vector best_pos;

	base = choose_drop_segment();
	if (base < 0 || base > Highest_segment_index)
		return -1;

	for (attempt = 0; attempt < SURVIVAL_SPAWN_CANDIDATES; attempt++)
	{
		int seg = base;
		int steps = 3 + ((d_rand() * 10) >> 15);
		vms_vector pos;
		fix score;

		while (steps-- > 0)
		{
			int side = (d_rand() * MAX_SIDES_PER_SEGMENT) >> 15;
			int child = Segments[seg].children[side];

			if (IS_CHILD(child))
				seg = child;
		}

		if (Segments[seg].special == SEGMENT_IS_CONTROLCEN)
			continue;

		pick_random_point_in_seg(&pos, seg);

		if (survival_dist_to_players(&pos) < SURVIVAL_SPAWN_MIN_PLAYER_DIST)
			continue;

		score = survival_dist_to_recent_spawns(&pos);
		if (score > best_score)
		{
			best_score = score;
			best_seg = seg;
			best_pos = pos;
		}

		if (score >= SURVIVAL_SPAWN_MIN_SEPARATION)
			break; // far enough from the rest of the wave; stop looking
	}

	if (best_seg < 0)
	{
		// Tiny or heavily-occupied level where nothing cleared the player
		// proximity test -- spawn at the unfiltered starting point rather
		// than skipping this robot entirely.
		best_seg = base;
		pick_random_point_in_seg(&best_pos, base);
	}

	*out_pos = best_pos;
	return best_seg;
}

// SURVIVAL_BOSS_PLAYER_SCALE_PER_PLAYER (1.5x) compounded once per player beyond the first. Roster
// size, not "how many are alive right now": a downed-but-not-eliminated player still represents
// firepower coming back next wave (see the constant's own comment above), and this only runs at spawn
// time anyway -- there's no live-recompute of an already-spawned boss's shields as players join,
// leave, or go down mid-wave.
static fix survival_boss_player_scale(void)
{
	int i, count = 0;
	fix scale = F1_0;

	for (i = 0; i < N_players; i++)
		if (Players[i].connected != CONNECT_DISCONNECTED)
			count++;

	for (i = 1; i < count; i++)
		scale = fixmul(scale, SURVIVAL_BOSS_PLAYER_SCALE_PER_PLAYER);

	return scale;
}

// Spawner only. Picks a segment, a type, creates the robot locally, and
// tells everyone else. Per-instance shields scale with wave (+10% per 5
// waves), netgame difficulty (+SURVIVAL_DIFFICULTY_SHIELD_PCT_PER_LEVEL%
// per level above Trainee), and, for bosses, SURVIVAL_BOSS_SHIELD_MULT and
// the per-player scale above -- there's no per-type toughness tier to scale
// off of, only quantity/pool-width (see survival_pick_robot_type) and this
// per-instance shield multiplier.
static void survival_spawn_one_robot(int wave, int is_boss)
{
	int segnum, type, elite_kind;
	vms_vector pos;
	object *obj;
	fix mult;

	segnum = survival_choose_spawn_point(&pos);
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	type = survival_pick_robot_type(wave, is_boss);
	if (type < 0)
		return;

	obj = create_morph_robot(&Segments[segnum], &pos, type);
	if (!obj)
		return;

	//	Rolled here, on the spawner, and shipped -- never re-rolled per machine, or each client would
	//	pick a different set of robots to outline. Bosses are never elite: start_boss_death_sequence()
	//	is its own death handling, and the elite death blast would fight it.
	elite_kind = SURVIVAL_ELITE_NONE;
	if (!is_boss && (d_rand() < SURVIVAL_ELITE_CHANCE))
		elite_kind = 1 + (d_rand() % SURVIVAL_ELITE_KIND_COUNT);

	mult = F1_0 + (wave / 5) * (F1_0 / 10) + Difficulty_level * (F1_0 * SURVIVAL_DIFFICULTY_SHIELD_PCT_PER_LEVEL / 100);
	if (is_boss)
		mult = fixmul(fixmul(mult, SURVIVAL_BOSS_SHIELD_MULT), survival_boss_player_scale());
	if (elite_kind == SURVIVAL_ELITE_BRUTE)
		mult = fixmul(mult, SURVIVAL_ELITE_BRUTE_SHIELD_MULT);

	// fixmul64 + clamp, not a plain fixmul, and this matters: fixmul() computes in 64 bits but
	// truncates the result back to 32-bit fix, which saturates at 32767.99 -- and boss multipliers now
	// compound hard enough to blow past that. A tough robot (strength ~250) at a late wave with a full
	// 8-player lobby reaches roughly 123x here, and 250 * 123 overflows, wrapping the boss's shields
	// NEGATIVE so it dies to the first shot that touches it. That is the exact opposite of the intent,
	// and it would have looked like a random "boss instantly dies" bug rather than an arithmetic one.
	{
		fix64 scaled = fixmul64(obj->shields, mult);

		if (scaled > SURVIVAL_MAX_ROBOT_SHIELDS)
			scaled = SURVIVAL_MAX_ROBOT_SHIELDS;

		obj->shields = (fix)scaled;
	}

	morph_start(obj);

	Survival_robot_elite_kind[obj - Objects] = elite_kind;

	survival_remember_spawn_pos(&pos);
	survival_send_spawn_robot(obj - Objects, segnum, type, obj->shields, &pos, is_boss, elite_kind);

	if (is_boss)
		survival_track_boss(obj - Objects, obj->shields);
}

// Swarmer's death payoff: 2-3 fast, fragile, plain (non-elite) copies of its own robot type. Plain
// on purpose -- SURVIVAL_ELITE_NONE can never itself be a SWARMER, so there's no chain-reaction path
// from a fragment splitting again; that's a property of what they *aren't* marked as, not something
// enforced by checking for it.
//
// Spawner-authority only, same as survival_spawn_one_robot() -- creating an object and broadcasting
// it is spawner's job everywhere else in this file, and robot death (multi_explode_robot_sub(),
// multibot.c, which calls this via survival_robot_death_blast()) runs on every machine, including
// the spawner's, for every robot regardless of who killed it or who owns it. So this only needs to
// gate itself, not be called differently depending on who's running it.
static void survival_swarm_split(object *robot)
{
	int count, i, budget;
	int segnum = robot->segnum;
	int type = robot->id;
	fix child_shields;

	if (is_observer() || Player_num != survival_spawner_pnum())
		return;
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	budget = SURVIVAL_MAX_ACTIVE_ROBOTS - survival_count_active_robots();
	if (budget <= 0)
		return;

	count = SURVIVAL_SWARM_CHILD_COUNT_MIN +
		(d_rand() % (SURVIVAL_SWARM_CHILD_COUNT_MAX - SURVIVAL_SWARM_CHILD_COUNT_MIN + 1));
	if (count > budget)
		count = budget;

	// Off the swarmer's own (already wave/difficulty/brute-scaled) shields rather than recomputing
	// from scratch, so a swarmer that spawned late in a hard wave still leaves dangerous fragments
	// instead of ones scaled as if the match had just started.
	child_shields = fixmul(robot->shields, SURVIVAL_SWARM_CHILD_SHIELD_FRAC);
	if (child_shields < 1)
		child_shields = 1;

	for (i = 0; i < count; i++)
	{
		vms_vector pos;
		object *child;

		pick_random_point_in_seg(&pos, segnum);

		child = create_morph_robot(&Segments[segnum], &pos, type);
		if (!child)
			break;

		child->shields = child_shields;
		morph_start(child);

		Survival_robot_elite_kind[child - Objects] = SURVIVAL_ELITE_NONE;

		survival_send_spawn_robot(child - Objects, segnum, type, child->shields, &pos,
					   0, SURVIVAL_ELITE_NONE);
	}
}

// Runs on every machine, every frame: AI recomputes each robot's velocity
// towards its target and clamps it against Robot_info[].max_speed (see
// move_towards_vector() in ai.c) -- there's no per-object speed field to
// override instead, so we just damp the result afterward. This makes
// Survival's robots amble towards players rather than beeline at full
// robot-type speed, without touching the shared Robot_info tables that
// every other game mode also uses.
// A flat per-frame multiply here (the original version of this function)
// compounds: physics drag already erodes velocity every frame, and stacking
// a further unconditional 0.6x on top of that every frame crushed robots'
// velocity toward zero within a few frames instead of settling at a slower
// steady speed -- that's what made them appear completely frozen. A real
// cap (only trims speed that's already over the target, same clamp shape
// ai.c's own move_towards_vector() uses against Robot_info[].max_speed)
// is stable and doesn't fight the AI's own acceleration.
static void survival_limit_robot_speeds(void)
{
	int i;

	for (i = 0; i <= Highest_object_index; i++)
	{
		object *obj = &Objects[i];
		fix cap, speed;

		if (obj->type != OBJ_ROBOT || (obj->flags & OF_SHOULD_BE_DEAD))
			continue;

		// Scaled from the TOP difficulty's speed, not the netgame's own
		// Difficulty_level. Survival already sets its own pace with this
		// damping factor, and stacking it on top of the per-difficulty
		// speed table compounded: on Trainee, robots crawled slowly enough
		// that early waves could take minutes to reach anybody, which reads
		// as the mode being broken from the start. This mode scales through
		// wave size, robot pool and shields (survival_start_wave() /
		// survival_pick_robot_type()) -- movement stays difficulty-neutral.
		//
		// A tracked boss gets a taller cap here instead of skipping the cap outright, so it's still a
		// multiple of what the robot type could ever do rather than an unbounded speed that could
		// clip through geometry at high FrameTime. Existing to make a boss visibly reposition rather
		// than read as parked while it circles/fires at the same spot -- see SURVIVAL_BOSS_SPEED_
		// SCALE's own comment for why "buff its health" alone wasn't enough to make it feel alive.
		cap = fixmul(Robot_info[obj->id].max_speed[NDL - 1],
			survival_robot_is_boss(i) ? SURVIVAL_BOSS_SPEED_SCALE : SURVIVAL_ROBOT_SPEED_SCALE);
		speed = vm_vec_mag_quick(&obj->mtype.phys_info.velocity);
		if (speed > cap && speed > 0)
			vm_vec_scale(&obj->mtype.phys_info.velocity, fixdiv(cap, speed));
	}
}

// NOTE FOR FUTURE WORK -- robot behaviour for this mode lives in ai.c/aipath.c,
// not here, and it is not driven by writing to Ai_local_info[] from this file.
//
// Do not "improve" robot aggression by pinning Ai_local_info[].player_awareness_
// type high. A version of this file did exactly that (PA_WEAPON_ROBOT_COLLISION
// every frame, to stop robots losing interest) and it froze them completely:
// they neither moved nor fired. do_ai_frame() reacts to that awareness level by
// forcing ailp->mode = AIM_CHASE_OBJECT on *every* frame (see the "Make sure
// that if this guy got hit or bumped, then he's chasing player" block), which
// permanently clobbers AIM_FOLLOW_PATH -- the mode all of the pathing depends
// on -- so a robot can never route around geometry. It also disables ai.c's own
// time-slicing and changes how ai_multiplayer_awareness()/multi_can_move_robot()
// arbitrate who may move each robot.
//
// What Survival actually changes, all of it gated on survival_horde_hunts()
// (ai.c) or the equivalent NETGAME_SURVIVAL test, and all of it additive:
//
//   * player_is_visible_from_object() (ai.c) reports the player as visible
//     through geometry -- but never as lined-up-for-a-shot, so robots still
//     can't fire through walls, they have to come and find you.
//   * robots navigate by a shared flow field (survival_flow_goal(), ai.c)
//     instead of stock's per-robot path search: one breadth-first sweep out
//     from every live player four times a second labels the whole level with
//     its distance to the nearest player, and each robot just steers at the
//     neighbouring segment one hop closer. That is what makes them find you
//     from anywhere, and it costs less than the stock pathing it replaces.
//   * do_ai_frame()'s distance time-slicing is off, so far-away robots actually
//     travel instead of getting one AI frame every two seconds.
//   * stuck-recovery pathing (the retry_count block) is on, which stock leaves
//     off in multiplayer; wall-seeing robots wedge without it.
//
// Robots only navigate this way while travelling. Inside SURVIVAL_HUNT_CLOSE_
// DIST they are handed back to stock AIM_CHASE_OBJECT, which is what actually
// fights -- circling, firing, flinching. Pursuit brings them to you; stock D1
// robot behaviour is what happens once they arrive.

static void survival_send_wave_state(void)
{
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_WAVE_STATE;		count += 1;
	PUT_INTEL_SHORT(multibuf + count, Survival_wave);	count += 2;
	multibuf[count] = (ubyte)Survival_wave_is_boss;	count += 1;
	multibuf[count] = (ubyte)Survival_wave_in_progress;	count += 1;

	multi_send_data(multibuf, count, 2);
}

void multi_do_survival_wave_state(const ubyte *buf)
{
	int count = 1;

	Survival_wave = GET_INTEL_SHORT(buf + count); count += 2;
	Survival_wave_is_boss = buf[count]; count += 1;
	Survival_wave_in_progress = buf[count];
}

static void survival_start_wave(void)
{
	Survival_wave++;
	Survival_wave_is_boss = (Survival_wave % SURVIVAL_BOSS_WAVE_INTERVAL == 0);

	if (Survival_wave_is_boss)
		// One extra boss every 3 boss-waves: 1 at wave 10, 2 at wave 40, 3 at wave 70, ...
		Survival_robots_to_spawn = 1 + (Survival_wave / SURVIVAL_BOSS_WAVE_INTERVAL - 1) / 3;
	else
		Survival_robots_to_spawn = survival_robots_for_wave(Survival_wave);

	Survival_wave_in_progress = 1;
	Survival_next_spawn_at = GameTime64;
	Survival_shop_phase = SURVIVAL_SHOP_PHASE_NONE;

	survival_send_wave_state();
}

static void survival_spawn_ammo(void)
{
	int segnum, objnum, ammo_type;
	vms_vector pos;

	segnum = choose_drop_segment();
	if (segnum < 0 || segnum > Highest_segment_index)
		return;

	ammo_type = Survival_ammo_types[(d_rand() * SURVIVAL_NUM_AMMO_TYPES) >> 15];

	Net_create_loc = 0;
	objnum = call_object_create_egg(&Objects[Players[Player_num].objnum], 1, OBJ_POWERUP, ammo_type);
	if (objnum < 0)
		return;

	pick_random_point_in_seg(&pos, segnum);
	Objects[objnum].pos = pos;
	vm_vec_zero(&Objects[objnum].mtype.phys_info.velocity);
	obj_relink(objnum, segnum);

	multi_send_create_powerup(ammo_type, segnum, objnum, &pos);
}

int survival_random_ammo_type(void)
{
	return Survival_ammo_types[(d_rand() * SURVIVAL_NUM_AMMO_TYPES) >> 15];
}

int survival_random_weapon_type(void)
{
	return Survival_weapon_types[(d_rand() * SURVIVAL_NUM_WEAPON_TYPES) >> 15];
}

// Wipes every powerup the level author placed. Survival is meant to start
// you with nothing and make the mine itself barren -- everything you get
// comes off a robot (survival_robot_drops() below). Called from
// multi_prep_level(), i.e. after the level is loaded but before anyone is
// playing it, and it runs identically on every machine, so no syncing is
// needed: each one deletes the same set of objects from the same freshly
// loaded level.
void survival_strip_level_powerups(void)
{
	int i;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	for (i = 0; i <= Highest_object_index; i++)
		if (Objects[i].type == OBJ_POWERUP)
			obj_delete(i);
}

// Emits one powerup off a just-killed robot: its own object_create_egg() plus
// its own network send, because the MULTI_CREATE_ROBOT_POWERUPS packet carries
// a single contains_type/id for however many objnums it lists -- batching two
// *different* powerup types into one packet would make every remote machine
// spawn two of whichever type happened to be set last.
static void survival_drop_one(object *del_obj, int powerup_id)
{
	Net_create_loc = 0;
	del_obj->contains_type = OBJ_POWERUP;
	del_obj->contains_id = powerup_id;
	del_obj->contains_count = 1;
	d_srand(1245L);
	if (object_create_egg(del_obj) >= 0 && Net_create_loc > 0)
		multi_send_create_robot_powerups(del_obj);
}

// Robot death drops, called from multi_drop_robot_powerups() in place of the
// stock contains_prob path. Four *independent* rolls, deliberately: a weapon
// roll, a sustain (shield/energy/ammo) roll, a rare situational (cloak/invuln)
// roll, and a rare extra-life roll. They don't share a budget, so a robot can
// drop any combination or nothing, and a run of weapon drops never means you
// go without shields.
//
// All four are rolled up front, before anything is dropped, and that ordering
// is load-bearing: dropping a powerup calls d_srand(1245L) (the engine's
// fixed seed that keeps egg drops identical on every machine) and then burns
// a known number of d_rand() calls inside drop_powerup(). Any roll made after
// a drop is therefore not random at all -- it reads a fixed point in a fixed
// sequence and comes out the same way on every kill. That is what made the
// supply drop effectively unconditional rather than the intended percentage.
void survival_robot_drops(object *del_obj)
{
	int drop_weapon, drop_supply, drop_situational, drop_extra_life;
	int weapon_id = -1, supply_id = -1, situational_id = -1;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	drop_weapon = ((d_rand() * 100) >> 15) < SURVIVAL_WEAPON_DROP_PCT;
	drop_supply = ((d_rand() * 100) >> 15) < SURVIVAL_SUPPLY_DROP_PCT;
	drop_situational = ((d_rand() * 1000) >> 15) < SURVIVAL_SITUATIONAL_DROP_PERMILLE;
	drop_extra_life = ((d_rand() * 1000) >> 15) < SURVIVAL_EXTRA_LIFE_DROP_PERMILLE;

	// A boss is the reward wave: killing one always pays out a life, on top
	// of whatever the ordinary rolls above came up with.
	if (survival_is_tracked_boss(del_obj - Objects))
		drop_extra_life = 1;

	// Which powerup, likewise chosen before any drop touches the RNG.
	if (drop_weapon)
		weapon_id = survival_random_weapon_type();
	if (drop_supply)
		supply_id = survival_random_ammo_type();
	if (drop_situational)
		situational_id = (d_rand() & 1) ? POW_CLOAK : POW_INVULNERABILITY;

	if (drop_weapon)
		survival_drop_one(del_obj, weapon_id);

	if (drop_supply)
		survival_drop_one(del_obj, supply_id);

	if (drop_situational)
		survival_drop_one(del_obj, situational_id);

	// By far the rarest: a free revive. It survives the engine's usual
	// "extra lives are meaningless in multiplayer, turn them into
	// invulnerability" rewrites because those only fire for modes without
	// GM_MULTI_ROBOTS (drop_powerup(), fireball.c) or for powerups the level
	// author placed (multi_prep_level()) -- Survival is neither.
	if (drop_extra_life)
		survival_drop_one(del_obj, POW_EXTRA_LIFE);
}

void survival_add_extra_life(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	Survival_extra_lives++;

	survival_banner("EXTRA LIFE!", SURVIVAL_BANNER_DURATION, -1); // the pickup already has its own sound
}

int survival_extra_lives(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return 0;

	return Survival_extra_lives;
}

// Broadcast our own hull and energy so everyone can draw the bars under our
// name. Wire format: [type:1][pnum:1][shields:4][energy:4] = 10 bytes.
static void survival_send_shields(void)
{
	int count = 0;

	if (is_observer())
		return;

	multibuf[count] = MULTI_SURVIVAL_SHIELDS;			count += 1;
	multibuf[count] = Player_num;					count += 1;
	PUT_INTEL_INT(multibuf + count, Players[Player_num].shields);	count += 4;
	PUT_INTEL_INT(multibuf + count, Players[Player_num].energy);	count += 4;

	multi_send_data(multibuf, count, 0);
}

void multi_do_survival_shields(const ubyte *buf)
{
	int pnum = buf[1];

	if (pnum < 0 || pnum >= MAX_PLAYERS || pnum == Player_num)
		return;

	// Observers deliberately excluded: they already reconstruct everyone's
	// shields from the MULTI_DAMAGE / MULTI_REPAIR stream, which also feeds
	// their damage-delta readouts. Letting this coarser 4 Hz sample
	// overwrite that would make those deltas jump.
	if (is_observer())
		return;

	Players[pnum].shields = (fix)GET_INTEL_INT(buf + 2);
	Players[pnum].energy = (fix)GET_INTEL_INT(buf + 6);
}

static void survival_send_eliminated(int pnum)
{
	multibuf[0] = MULTI_SURVIVAL_ELIMINATED;
	multibuf[1] = (ubyte)pnum;
	multi_send_data(multibuf, 2, 2);
}

// Every machine independently reaches the same conclusion once elimination
// state has propagated (same pattern as spawner-authority election above --
// deterministic function of synced state, not a separate "game over"
// broadcast), so each one shows the stats screen and leaves on its own.
static void survival_check_game_over(void)
{
	int i, any_alive = 0;

	if (Survival_game_over)
		return;

	for (i = 0; i < N_players; i++)
		if (Players[i].connected != CONNECT_DISCONNECTED && !Survival_eliminated[i])
			any_alive = 1;

	if (any_alive)
		return;

	Survival_game_over = 1;

	multi_endlevel_score();
#ifdef USE_UDP
	// Survival is only reachable through the UDP hosting menu -- this can
	// never actually run without USE_UDP, but guard it anyway so this file
	// still links cleanly in a build configured without UDP support.
	net_udp_leave_game();
#endif
	if (Game_wind)
		window_close(Game_wind);
}

void multi_do_survival_eliminated(const ubyte *buf)
{
	int pnum = buf[1];

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;
	if (Survival_eliminated[pnum])
		return;

	Survival_eliminated[pnum] = 1;

	// Hide the downed player from everyone else for as long as they're out:
	// OBJ_GHOST is the engine's existing "player object that still exists but
	// is invisible and collides with nothing" state (used for players who
	// haven't finished joining), which is exactly the spectator presentation
	// we want and costs nothing to maintain. multi_make_player_ghost() Int3()s
	// if handed the local player, hence the guard -- the local side of being
	// downed is handled in DoPlayerDead()/survival_is_eliminated() instead.
	if (pnum != Player_num)
		multi_make_player_ghost(pnum);

	survival_check_game_over();
}

// Broadcast when the local player hits ESC in the shop -- see
// survival_shop_handle_key(). Every machine (not just the spawner) uses
// these to independently decide when every currently-connected player is
// ready, same "deterministic function of synced state, no central referee"
// approach survival_check_game_over() above already uses for game-over.
static void survival_send_shop_ready(int pnum)
{
	multibuf[0] = MULTI_SURVIVAL_SHOP_READY;
	multibuf[1] = (ubyte)pnum;
	multi_send_data(multibuf, 2, 2);
}

void multi_do_survival_shop_ready(const ubyte *buf)
{
	int pnum = buf[1];

	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return;

	Survival_shop_ready[pnum] = 1;
}

// Grants SURVIVAL_REVIVE_INVULN_DURATION of invulnerability to a
// just-revived local player, backdating invulnerable_time the same way the
// engine's own short-invuln cheats do (game.c) so the *existing* expiry
// check in do_invulnerable_stuff() (game.c) clears the flag on its own --
// no new timer/state needed.
static void survival_grant_revive_invulnerability(int pnum)
{
	Players[pnum].flags |= PLAYER_FLAGS_INVULNERABLE;
	Players[pnum].invulnerable_time = GameTime64 - INVULNERABLE_TIME_MAX + SURVIVAL_REVIVE_INVULN_DURATION;
}

// Called from gameseq.c right after a Survival respawn's init_player_stats_
// new_ship()/StartLevel(1) -- see the comment on Survival_pending_revive_
// invuln for why it can't just be granted from survival_player_died()
// itself. No-op unless an extra life was actually just spent.
void survival_maybe_grant_revive_invulnerability(int pnum)
{
	if (!Survival_pending_revive_invuln)
		return;

	Survival_pending_revive_invuln = 0;
	survival_grant_revive_invulnerability(pnum);
}

// Everyone who went down during the wave that just ended comes back, provided
// the team actually cleared it. Runs independently on every machine off the
// synced wave state, so no revive packet is needed -- see
// Survival_revive_prev_in_progress.
static void survival_revive_all(void)
{
	int i;

	for (i = 0; i < MAX_PLAYERS; i++)
	{
		if (!Survival_eliminated[i])
			continue;

		Survival_eliminated[i] = 0;

		if (i == Player_num)
		{
			// We were flying around as an untouchable non-combatant (see
			// DoPlayerDead()); becoming a real player again is just undoing
			// that -- the ship object has been alive the whole time, so
			// there's no respawn/level-restart to redo here. init_player_
			// stats_new_ship() below clears PLAYER_FLAGS_INVULNERABLE as
			// part of its normal reset, so the revive grace grant has to
			// come after it, not before.
			init_player_stats_new_ship(i);
			survival_grant_revive_invulnerability(i);
			survival_banner("BACK IN THE FIGHT!", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_REVIVE);
			HUD_init_message(HM_DEFAULT, "Wave cleared -- you're back in!");
		}
		else if (Players[i].connected != CONNECT_DISCONNECTED)
		{
			multi_make_ghost_player(i);
		}
	}
}

void survival_start(void)
{
	int i;

	memset(Survival_eliminated, 0, sizeof(Survival_eliminated));
	Survival_wave = 0;
	Survival_wave_is_boss = 0;
	Survival_wave_in_progress = 0;
	Survival_robots_to_spawn = 0;
	Survival_game_over = 0;

	// Everyone starts with a life in the bank, so a single early mistake
	// doesn't put a player out of the wave before the match has warmed up.
	Survival_extra_lives = SURVIVAL_STARTING_EXTRA_LIVES;
	Survival_pending_revive_invuln = 0;

	memset(Survival_popups, 0, sizeof(Survival_popups));
	memset(Survival_robot_elite_kind, 0, sizeof(Survival_robot_elite_kind));
	Survival_popup_idx = 0;
	Survival_repair_pending = 0;
	// Deliberately not computed here -- see Survival_timers_armed.
	Survival_timers_armed = 0;
	Survival_next_wave_at = 0;
	Survival_next_ammo_at = 0;
	Survival_tables_built = 0; // rebuild from whatever robot set this mission loaded

	for (i = 0; i < SURVIVAL_SPAWN_HISTORY; i++)
		Survival_recent_spawn_used[i] = 0;
	Survival_recent_spawn_idx = 0;

	for (i = 0; i < SURVIVAL_MAX_TRACKED_BOSSES; i++)
		Survival_bosses[i].objnum = -1;

	Survival_banner_until = 0;
	Survival_hud_prev_wave = -1;
	Survival_hud_prev_in_progress = -1;
	Survival_revive_prev_in_progress = -1;
	Survival_countdown_last_spoken = -1;
	Survival_prewave_announced = 0;

	Survival_shop_phase = SURVIVAL_SHOP_PHASE_NONE;
	Survival_shop_open_until = 0;
	Survival_shop_last_wave = -1;
	Survival_speed_tier = 0;
	Survival_armor_tier = 0;
	Survival_shop_visit_count = 0;
	memset(Survival_shop_ready, 0, sizeof(Survival_shop_ready));

	// Clears any capture a previous match left behind before this one
	// starts flying.
	survival_shop_set_mouse_released(0);
}

int survival_player_died(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return Survival_game_over;

	// Spend a banked extra life instead of going down. Nothing is sent: the
	// other machines never hear about this death at all, so from their side
	// this player simply kept playing (their respawned ship keeps streaming
	// position as usual). The caller still does the normal respawn, and
	// skips the spectator flagging because we're not eliminated.
	if (pnum == Player_num && Survival_extra_lives > 0)
	{
		Survival_extra_lives--;
		Survival_pending_revive_invuln = 1;
		survival_banner("REVIVED!", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_REVIVE);
		HUD_init_message(HM_DEFAULT, "Extra life spent -- %d left", Survival_extra_lives);
		return Survival_game_over;
	}

	if (!Survival_eliminated[pnum])
	{
		Survival_eliminated[pnum] = 1;
		survival_send_eliminated(pnum);
		survival_check_game_over();
	}

	return Survival_game_over;
}

int survival_is_eliminated(int pnum)
{
	if (pnum < 0 || pnum >= MAX_PLAYERS)
		return 0;
	return Survival_eliminated[pnum];
}

// Single entry point for every centered splash this mode shows, so each one
// is paired with its event sound in exactly one place. sound < 0 means
// silent; demo playback never makes noise.
static void survival_banner(const char *text, fix64 duration, int sound)
{
	strncpy(Survival_banner_text, text, sizeof(Survival_banner_text) - 1);
	Survival_banner_text[sizeof(Survival_banner_text) - 1] = 0;
	Survival_banner_until = GameTime64 + duration;

	if (sound > -1 && Newdemo_state != ND_STATE_PLAYBACK)
		digi_play_sample(sound, F1_0);
}

// The wave that the current rest period is leading up to.
static int survival_next_wave_is_boss(void)
{
	return ((Survival_wave + 1) % SURVIVAL_BOSS_WAVE_INTERVAL) == 0;
}

static fix64 survival_prewave_delay(void)
{
	return survival_next_wave_is_boss() ? SURVIVAL_BOSS_WAVE_DELAY : SURVIVAL_INTER_WAVE_DELAY;
}

// Longest gap this mode ever legitimately parks Survival_next_wave_at at,
// used as the sanity bound by the clock self-heal in survival_do_frame().
// Phase-aware on purpose: only a shop schedules the wave a full window +
// countdown out, so widening the bound unconditionally would let a
// genuinely corrupt deadline sit unnoticed for a minute on ordinary waves.
static fix64 survival_max_wave_delay(void)
{
	if (Survival_shop_phase != SURVIVAL_SHOP_PHASE_NONE)
		return SURVIVAL_SHOP_DURATION + SURVIVAL_SHOP_COUNTDOWN_DURATION;

	return SURVIVAL_FIRST_WAVE_DELAY;
}

// True if the wave that just cleared (Survival_wave, not yet incremented --
// survival_start_wave() does that) is one that opens the shop before the
// next wave starts.
static int survival_wave_opens_shop(void)
{
	return Survival_wave > 0 && (Survival_wave % SURVIVAL_SHOP_WAVE_INTERVAL) == 0;
}

// True once every currently-connected player has hit ESC (Survival_shop_
// ready[], kept in sync by MULTI_SURVIVAL_SHOP_READY). Same connected-check
// survival_check_game_over() uses -- this engine doesn't track a distinct
// "observer" connection state, so "connected" is the roster this mode
// already treats as who's actually in the match. Runs on every machine
// (see the OPEN->COUNTDOWN transition in survival_do_frame()), so whichever
// one happens to be spawner authority reaches the same answer as everyone
// else without needing to be told.
static int survival_shop_all_ready(void)
{
	int i;

	for (i = 0; i < N_players; i++)
		if (Players[i].connected != CONNECT_DISCONNECTED && !Survival_shop_ready[i])
			return 0;

	return 1;
}

// Counts for the "waiting for players" sub-panel (survival_shop_draw()).
static void survival_shop_ready_counts(int *ready_out, int *total_out)
{
	int i, ready = 0, total = 0;

	for (i = 0; i < N_players; i++)
	{
		if (Players[i].connected == CONNECT_DISCONNECTED)
			continue;
		total++;
		if (Survival_shop_ready[i])
			ready++;
	}

	*ready_out = ready;
	*total_out = total;
}

// Applies the per-visit markup (SURVIVAL_SHOP_VISIT_ESCALATION_PERMILLE) to a base price. Every
// priced item in the shop routes through this, so there is exactly one place that defines what "a
// visit" is worth and every price the panel shows is guaranteed to match what buying it actually
// charges.
//
// Survival_shop_visit_count is incremented before the shop the player is standing in opens (see the
// wave-clear transition in survival_do_frame()), so it is already 1 during the very first visit --
// hence -1 here, so that first visit prices at the unscaled base rather than one step up.
static int survival_shop_scale_price(int base_price)
{
	int steps = Survival_shop_visit_count > 0 ? Survival_shop_visit_count - 1 : 0;

	return base_price * (1000 + steps * SURVIVAL_SHOP_VISIT_ESCALATION_PERMILLE) / 1000;
}

static int survival_shop_weapon_price(void)
{
	return survival_shop_scale_price(SURVIVAL_SHOP_PRICE_WEAPON);
}

static int survival_shop_supply_price(void)
{
	return survival_shop_scale_price(SURVIVAL_SHOP_PRICE_SUPPLY);
}

static int survival_shop_shield_price(void)
{
	return survival_shop_scale_price(SURVIVAL_SHOP_PRICE_SHIELD_FULL);
}

static int survival_shop_speed_price(void)
{
	return survival_shop_scale_price(SURVIVAL_SHOP_SPEED_BASE_PRICE + Survival_speed_tier * SURVIVAL_SHOP_SPEED_PRICE_STEP);
}

static int survival_shop_armor_price(void)
{
	return survival_shop_scale_price(SURVIVAL_SHOP_ARMOR_BASE_PRICE + Survival_armor_tier * SURVIVAL_SHOP_ARMOR_PRICE_STEP);
}

// Thrust/damage multipliers from purchased physics upgrades. Local-player-
// only concepts: thrust is only ever computed for the ship you're flying
// (read_flying_controls(), controls.c), and apply_damage_to_player()
// (collide.c) only mutates Players[] for player->id == Player_num on each
// machine -- so there is nothing to sync here, same reasoning survival.c
// already applies to shields/energy being locally authoritative.
fix survival_speed_multiplier(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return F1_0;
	return F1_0 + Survival_speed_tier * (F1_0 * SURVIVAL_SHOP_SPEED_PERMILLE_PER_TIER / 1000);
}

fix survival_damage_multiplier(void)
{
	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return F1_0;
	return F1_0 - Survival_armor_tier * (F1_0 * SURVIVAL_SHOP_ARMOR_PCT_PER_TIER / 100);
}

// Grants a powerup's effect straight to the local player, bypassing the
// normal fly-through-a-physical-object pickup -- a shop purchase should be
// instant, not something you can walk away from and lose. do_powerup()
// (powerup.c) only ever reads obj->id and, for vulcan ammo, obj->ctype.
// powerup_info.count (0 there is treated as "use the default amount") for
// every id Survival's weapon/ammo tables can hand it; it only touches obj->
// pos/segnum for the key powerups, which Survival never grants. A zeroed
// stand-in object is therefore safe here.
static void survival_shop_grant_powerup(int id)
{
	object tmp;

	memset(&tmp, 0, sizeof(tmp));
	tmp.id = id;
	do_powerup(&tmp);
}

static void survival_shop_buy_weapon(void)
{
	int price = survival_shop_weapon_price();

	if (Players[Player_num].score < price)
	{
		survival_banner("NOT ENOUGH POINTS", F1_0, -1);
		return;
	}

	Players[Player_num].score -= price;
	survival_shop_grant_powerup(survival_random_weapon_type());
}

static void survival_shop_buy_supply(void)
{
	int price = survival_shop_supply_price();

	if (Players[Player_num].score < price)
	{
		survival_banner("NOT ENOUGH POINTS", F1_0, -1);
		return;
	}

	Players[Player_num].score -= price;
	survival_shop_grant_powerup(
		Survival_shop_supply_types[(d_rand() * SURVIVAL_NUM_SHOP_SUPPLY_TYPES) >> 15]);
}

static void survival_shop_buy_shield_full(void)
{
	fix repair;
	int price = survival_shop_shield_price();

	if (Players[Player_num].score < price)
	{
		survival_banner("NOT ENOUGH POINTS", F1_0, -1);
		return;
	}
	if (Players[Player_num].shields >= SURVIVAL_SHOP_SHIELD_RESTORE_CAP)
	{
		survival_banner("SHIELDS ALREADY AT CAP", F1_0, -1);
		return;
	}

	Players[Player_num].score -= price;
	repair = SURVIVAL_SHOP_SHIELD_RESTORE_CAP - Players[Player_num].shields;
	Players[Player_num].shields = SURVIVAL_SHOP_SHIELD_RESTORE_CAP;
	multi_send_repair(repair, Players[Player_num].shields, OBJ_POWERUP);
}

static void survival_shop_buy_speed(void)
{
	int price;

	if (Survival_speed_tier >= SURVIVAL_SHOP_SPEED_MAX_TIER)
	{
		survival_banner("SPEED MAXED OUT", F1_0, -1);
		return;
	}

	price = survival_shop_speed_price();
	if (Players[Player_num].score < price)
	{
		survival_banner("NOT ENOUGH POINTS", F1_0, -1);
		return;
	}

	Players[Player_num].score -= price;
	Survival_speed_tier++;
	survival_banner("SPEED BOOST PURCHASED!", F1_0, -1);
}

static void survival_shop_buy_armor(void)
{
	int price;

	if (Survival_armor_tier >= SURVIVAL_SHOP_ARMOR_MAX_TIER)
	{
		survival_banner("ARMOR MAXED OUT", F1_0, -1);
		return;
	}

	price = survival_shop_armor_price();
	if (Players[Player_num].score < price)
	{
		survival_banner("NOT ENOUGH POINTS", F1_0, -1);
		return;
	}

	Players[Player_num].score -= price;
	Survival_armor_tier++;
	survival_banner("ARMOR PLATING PURCHASED!", F1_0, -1);
}

// Shared by both the keyboard (survival_shop_handle_key()) and mouse
// (survival_shop_do_mouse()) purchase paths, so there's exactly one place
// that maps a slot number to what it does.
static void survival_shop_buy_slot(int slot)
{
	switch (slot)
	{
		case 1: survival_shop_buy_weapon(); break;
		case 2: survival_shop_buy_supply(); break;
		case 3: survival_shop_buy_shield_full(); break;
		case 4: survival_shop_buy_speed(); break;
		case 5: survival_shop_buy_armor(); break;
		default: break;
	}
}

// True while the buy list should be interactive: still in the buying
// window, and the local player hasn't already hit ESC to lock in and wait
// on everyone else. Ready-but-waiting is handled as its own sub-view in
// survival_shop_draw() rather than here going back to false meaning
// something different than the caller expects.
int survival_shop_is_open(void)
{
	return Netgame.gamemode == NETGAME_SURVIVAL && Survival_shop_phase == SURVIVAL_SHOP_PHASE_OPEN
		&& !Survival_shop_ready[Player_num] && !is_observer();
}

// True for the *entire* shop experience -- buying, readied-but-waiting-on-
// others, and the trailing voice countdown alike -- as opposed to
// survival_shop_is_open(), which narrows to just the interactive buy list.
// This is the one that should gate anything meant to have "menu takes
// priority" semantics: flight controls, weapon firing, other key handlers,
// and the mouse capture handoff (see survival_shop_do_mouse() and the
// should_read_controls gate in gamecntl.c's ReadControls()).
int survival_shop_blocks_input(void)
{
	return Netgame.gamemode == NETGAME_SURVIVAL && Survival_shop_phase != SURVIVAL_SHOP_PHASE_NONE && !is_observer();
}

// Whether the mouse is currently handed to the shop UI rather than to
// flight control. Reconciled toward the desired state every frame instead
// of being flipped once on each transition: SDL can refuse to re-enter
// relative mode (it needs the window to hold input focus), and a one-shot
// edge-triggered restore that lost that race left the cursor stranded on
// screen with nothing left to retry it. Keeping the flag unset until
// mouse_toggle_relative() reports success means the next frame tries again.
static int Survival_shop_mouse_released = 0;

static void survival_shop_set_mouse_released(int released)
{
	if (released == Survival_shop_mouse_released)
		return;

	if (!mouse_toggle_relative(!released))
		return; // didn't take -- leave the flag so we retry next frame

	Survival_shop_mouse_released = released;
}

// Polls the left mouse button against whatever row rectangles the shop
// panel last drew (Survival_shop_row_rect[], stamped by survival_shop_draw()
// every frame the shop is open -- at most one render frame stale, which
// never shows since the panel's layout doesn't move frame to frame). Same
// mouse_get_pos()/MBTN_LEFT reading newmenu_mouse() (newmenu.c) uses for
// real menus. Edge-triggered on Survival_shop_mouse_was_down so a held
// button buys once, not every frame.
//
// Also owns handing the mouse back and forth between flight control and
// point-and-click: while flying, GameCfg.Grabinput keeps SDL in relative-
// motion capture mode with the OS cursor hidden (see event_toggle_focus(),
// event.c) -- mouse_get_pos() in that mode returns an accumulating relative
// offset with nothing visible on screen to aim it by, which is why clicking
// silently did nothing before this. mouse_toggle_relative(0)/(1) (mouse.c)
// switch that capture off for the whole shop experience (survival_shop_
// blocks_input(), not just the interactive buy list) and back on only once
// it's fully done -- toggling it back on at the "readied, waiting on
// others" sub-state would just mean re-releasing it a moment later, which
// read as the cursor never actually going away until a second ESC press.
static void survival_shop_do_mouse(void)
{
	int mx, my, mz, i;

	survival_shop_set_mouse_released(survival_shop_blocks_input());

	if (!survival_shop_is_open())
	{
		Survival_shop_mouse_was_down = 0;
		return;
	}

	if (!(mouse_get_btns() & MOUSE_LBTN))
	{
		Survival_shop_mouse_was_down = 0;
		return;
	}

	if (Survival_shop_mouse_was_down)
		return;
	Survival_shop_mouse_was_down = 1;

	mouse_get_pos(&mx, &my, &mz);
	for (i = 0; i < SURVIVAL_SHOP_NUM_ITEMS; i++)
	{
		if (mx >= Survival_shop_row_rect[i].x1 && mx < Survival_shop_row_rect[i].x2 &&
			my >= Survival_shop_row_rect[i].y1 && my < Survival_shop_row_rect[i].y2)
		{
			survival_shop_buy_slot(i + 1);
			break;
		}
	}
}

// Invulnerability for as long as the shop is up (buying window and its
// trailing countdown alike): browsing a menu shouldn't be able to get you
// killed. Refreshed every frame the same way survival_hold_spectator_cloak()
// holds the downed-player cloak open, and for the same reason the flag can't
// just be left to expire on its own -- do_invulnerable_stuff() (game.c)
// only turns PLAYER_FLAGS_INVULNERABLE off once GameTime64 passes
// invulnerable_time+INVULNERABLE_TIME_MAX (30s), so the instant this stops
// refreshing that timestamp, the edge below clears the flag itself rather
// than coasting on whatever's left of that 30s window into the next wave.
static void survival_hold_shop_invulnerability(void)
{
	static int was_active = 0;
	int active;

	if (is_observer())
		return;

	active = (Survival_shop_phase != SURVIVAL_SHOP_PHASE_NONE) && !Survival_wave_in_progress;

	if (active)
	{
		Players[Player_num].flags |= PLAYER_FLAGS_INVULNERABLE;
		Players[Player_num].invulnerable_time = GameTime64;
	}
	else if (was_active)
		Players[Player_num].flags &= ~PLAYER_FLAGS_INVULNERABLE;

	was_active = active;
}

// Routes a key event to the shop. The call site (ReadControls(), gamecntl.c)
// only reaches this while survival_shop_blocks_input() is true and swallows
// the key unconditionally either way -- see that function's comment for why
// "menu takes priority" means every key gets eaten for the whole shop
// experience, not just while the buy list itself is interactive. This
// function only has real work to do while survival_shop_is_open(); once the
// local player has readied up (or the buy list's own window has ended) it's
// just soaking up keypresses that have nothing left to act on.
void survival_shop_handle_key(int key)
{
	if (!survival_shop_is_open())
		return;

	switch (key)
	{
		case KEY_1: survival_shop_buy_slot(1); break;
		case KEY_2: survival_shop_buy_slot(2); break;
		case KEY_3: survival_shop_buy_slot(3); break;
		case KEY_4: survival_shop_buy_slot(4); break;
		case KEY_5: survival_shop_buy_slot(5); break;
		case KEY_ESC:
			// Lock in: no more buying for the local player, and tell
			// everyone else. The actual OPEN->COUNTDOWN transition (and
			// shortening Survival_next_wave_at to match) happens once
			// survival_shop_all_ready() sees every connected player has
			// done this, or the hard cap runs out -- see survival_do_frame().
			Survival_shop_ready[Player_num] = 1;
			survival_send_shop_ready(Player_num);
			break;
		default:
			break;
	}
}

// "5..4..3..2..1..GO" spoken over the pause before the match's first wave and
// before every boss wave, using the stock reactor self-destruct countdown
// voice clips (SOUND_COUNTDOWN_0_SECS is the base of a contiguous 0..14 run,
// so "N seconds" is base + N). A boss countdown lays the reactor siren over
// each tick, so the alarm effectively loops for the whole approach.
//
// Runs on every machine off its own Survival_next_wave_at, which each one
// sets for itself when it sees the previous wave end -- close enough to
// simultaneous that a shared countdown packet isn't worth the sync surface.
static void survival_do_countdown(void)
{
	fix64 remaining;
	int secs, boss_next;

	if (Survival_wave_in_progress)
		return;

	boss_next = survival_next_wave_is_boss();

	// The opening wave, boss waves, and shop waves (whose trailing 5s get
	// the same spoken countdown, once the shop window itself has closed)
	// get the countdown treatment; ordinary waves just roll straight on.
	if (Survival_wave != 0 && !boss_next && Survival_shop_last_wave != Survival_wave)
		return;

	if (!Survival_prewave_announced)
	{
		Survival_prewave_announced = 1;

		if (Survival_wave == 0)
			survival_banner("GET READY...", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_GET_READY);
		else if (boss_next)
			survival_banner("BOSS INCOMING", SURVIVAL_BANNER_DURATION, SURVIVAL_SND_SIREN);
		// Shop waves: no banner here -- survival_shop_draw() owns the
		// messaging for the whole window, and the last-5s voice tick below
		// speaks for itself once the shop closes.
	}

	remaining = Survival_next_wave_at - GameTime64;
	if (remaining < 0)
		remaining = 0;

	// Ceiling, so the last tick lands on 0 ("GO") exactly as the delay runs
	// out rather than being skipped -- f2i(remaining)+1 could never reach 0.
	secs = (int)((remaining + F1_0 - 1) / F1_0);
	if (secs > SURVIVAL_COUNTDOWN_FROM)
		return; // still in the quiet part of the grace period

	if (secs == Survival_countdown_last_spoken)
		return;

	Survival_countdown_last_spoken = secs;

	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;

	if (secs > 0)
	{
		char buf[8];

		digi_play_sample(SOUND_COUNTDOWN_0_SECS + secs, F1_0);
		if (boss_next)
			digi_play_sample(SURVIVAL_SND_SIREN, F1_0);

		sprintf(buf, "%d", secs);
		survival_banner(buf, F1_0, -1);
	}
	else
	{
		// Zero: just the "GO" clip. No splash of its own -- the wave's own
		// "WAVE N" / "WAVE N - BOSS!" banner (which carries the siren for a
		// boss) lands on the very next frame and would overwrite it anyway.
		digi_play_sample(SOUND_COUNTDOWN_0_SECS, F1_0);
	}
}

// Bleeds energy above SURVIVAL_ENERGY_SURPLUS_AT into shields, a slice per
// frame. Local player only, and purely local state -- nobody else simulates
// our shields.
static void survival_convert_surplus_energy(void)
{
	fix surplus, room, drain, gain;

	if (is_observer() || survival_is_eliminated(Player_num))
		return;
	if (Player_is_dead || Players[Player_num].shields <= 0)
		return;

	surplus = Players[Player_num].energy - SURVIVAL_ENERGY_SURPLUS_AT;
	room = MAX_SHIELDS - Players[Player_num].shields;

	if (surplus <= 0 || room <= 0)
		return;

	drain = fixmul(SURVIVAL_ENERGY_DRAIN_PER_SEC, FrameTime);
	if (drain > surplus)
		drain = surplus;

	gain = fixmul(drain, SURVIVAL_ENERGY_SHIELD_RATIO);
	if (gain > room)
	{
		// Topping out this frame -- spend only the energy the last sliver of
		// hull actually costs, so the remainder stays in the tank.
		gain = room;
		drain = fixdiv(gain, SURVIVAL_ENERGY_SHIELD_RATIO);
	}

	if (drain <= 0 || gain <= 0)
		return;

	Players[Player_num].energy -= drain;
	Players[Player_num].shields += gain;

	// Observers reconstruct shields from MULTI_REPAIR events. A trickle this
	// fine would mean a packet every frame, so it's batched up and reported
	// a whole point at a time.
	Survival_repair_pending += gain;
	if (Survival_repair_pending >= F1_0)
	{
		multi_send_repair(Survival_repair_pending,
			Players[Player_num].shields - Survival_repair_pending, OBJ_POWERUP);
		Survival_repair_pending = 0;
	}
}

// Keeps a downed player cloaked for as long as they're spectating. Cloak
// expires on a timer (do_cloak_stuff() in game.c drops the flag once
// cloak_time + CLOAK_TIME_MAX is past), so holding it means pushing the
// timestamp forward every frame rather than setting the flag once.
//
// Being cloaked on top of being ghosted and invulnerable is what makes
// spectating genuinely inert: OBJ_GHOST hides us from other players, but
// robots run their AI against the real object on our own machine, and a
// cloaked player is one the AI's targeting deliberately loses track of.
static void survival_hold_spectator_cloak(void)
{
	if (is_observer())
		return;

	if (!survival_is_eliminated(Player_num))
		return;

	Players[Player_num].flags |= PLAYER_FLAGS_CLOAKED;
	Players[Player_num].cloak_time = GameTime64;
}

// The body of survival_do_frame(); wrapped below so the shop's falling-edge
// input flush runs no matter which of the many early-outs in here we left
// through, and still lands in the same frame as the phase change that caused
// it. See survival_shop_release_stale_input().
static void survival_do_frame_inner(void)
{
	// Ahead of every early-out below on purpose. If the match ends, the
	// level ends, or the mode changes while a shop is up, the code that
	// would normally hand the mouse back stops running -- and the cursor
	// is left loose with nothing able to reclaim it. Reconciling here
	// first means any of those exits still releases it.
	if (Netgame.gamemode != NETGAME_SURVIVAL || Survival_game_over ||
		Endlevel_sequence || Control_center_destroyed)
		survival_shop_set_mouse_released(0);

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;
	if (Survival_game_over)
		return;
	if (Endlevel_sequence || Control_center_destroyed)
		return;

	// Speed damping applies to every locally-simulated robot object on
	// every machine, not just the spawner -- unlike wave/spawn decisions
	// this isn't spawner-authority state, it just shapes local movement.
	// Deliberately NOT touching AI mode/awareness/state here anymore --
	// create_morph_robot() (used to spawn these) already calls
	// init_ai_object() with AIB_NORMAL and an initial path, i.e. the exact
	// same setup regular matcen-spawned robots get in stock multiplayer
	// (see fuelcen.c). An earlier version of this file also force-drove
	// the AI's mode/state fields every frame to stop robots idling; that
	// fought the AI's own state machine and made them stop moving and
	// firing entirely. Trust the stock AI; only the speed is adjusted.
	survival_limit_robot_speeds();

	// First frame actually inside the level: GameTime64 is now the level's
	// own clock (StartNewLevel() zeroed it after survival_start() ran), so
	// this is the earliest point the opening deadlines can be trusted.
	if (!Survival_timers_armed)
	{
		Survival_timers_armed = 1;
		Survival_next_wave_at = GameTime64 + SURVIVAL_FIRST_WAVE_DELAY;
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;
	}

	// Self-heal: a deadline further out than the longest pause we ever set
	// can only mean the game clock moved under us. Re-arm rather than sit
	// there waiting for a time that may never come.
	//
	// The bound has to account for the shop, which legitimately parks
	// Survival_next_wave_at a full SURVIVAL_SHOP_DURATION + countdown out --
	// far beyond SURVIVAL_FIRST_WAVE_DELAY. Checking against the shorter
	// bound alone (which is what this did before the shop existed) made
	// this fire on the very first frame of every shop and yank the wave
	// back to survival_prewave_delay(), i.e. the shop slammed shut a few
	// seconds in no matter what its own timer said.
	if (Survival_next_wave_at - GameTime64 > survival_max_wave_delay())
		Survival_next_wave_at = GameTime64 + survival_prewave_delay();
	if (Survival_next_ammo_at - GameTime64 > SURVIVAL_AMMO_INTERVAL)
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;

	survival_do_countdown();
	survival_convert_surplus_energy();
	survival_hold_spectator_cloak();
	survival_hold_shop_invulnerability();
	survival_shop_do_mouse();

	if (Survival_next_shield_send - GameTime64 > SURVIVAL_SHIELD_SYNC_INTERVAL)
		Survival_next_shield_send = 0; // clock moved under us -- see Survival_timers_armed

	if (!is_observer() && GameTime64 >= Survival_next_shield_send)
	{
		survival_send_shields();
		Survival_next_shield_send = GameTime64 + SURVIVAL_SHIELD_SYNC_INTERVAL;
	}

	// Wave just ended and somebody survived it -- bring the downed players
	// back, and open the rest period. Above the spawner-only return below on
	// purpose: this has to run on every machine, since each one owns its own
	// player's revive and its own copy of the countdown. Every machine
	// derives the same delay from the same synced wave number, so the
	// spawner (which is the one that actually starts the next wave off
	// Survival_next_wave_at) stays in step with everyone's countdown without
	// a timing packet.
	if (Survival_revive_prev_in_progress == 1 && !Survival_wave_in_progress)
	{
		survival_revive_all();

		if (survival_wave_opens_shop())
		{
			Survival_shop_phase = SURVIVAL_SHOP_PHASE_OPEN;
			Survival_shop_last_wave = Survival_wave;
			Survival_shop_visit_count++;
			Survival_shop_open_until = GameTime64 + SURVIVAL_SHOP_DURATION;
			Survival_next_wave_at = Survival_shop_open_until + SURVIVAL_SHOP_COUNTDOWN_DURATION;
			memset(Survival_shop_ready, 0, sizeof(Survival_shop_ready));
		}
		else
		{
			Survival_shop_phase = SURVIVAL_SHOP_PHASE_NONE;
			Survival_next_wave_at = GameTime64 + survival_prewave_delay();
		}
		Survival_countdown_last_spoken = -1;
		Survival_prewave_announced = 0;
	}
	Survival_revive_prev_in_progress = Survival_wave_in_progress;

	// A wave running and a shop being up are mutually exclusive, and this
	// is the only thing that enforces it on machines that aren't the
	// spawner. survival_start_wave() clears the phase too, but it only ever
	// runs on the spawner (it's below the authority gate further down), so
	// without this every client would stay stuck in _PHASE_COUNTDOWN after
	// a shop wave -- panel still up, controls still blocked by
	// survival_shop_blocks_input(), forever. Deriving it from the synced
	// Survival_wave_in_progress instead means every machine leaves the shop
	// on the same wave-start packet that starts the wave.
	if (Survival_wave_in_progress)
		Survival_shop_phase = SURVIVAL_SHOP_PHASE_NONE;

	// Shop window ends into the trailing voice countdown either once every
	// connected player has hit ESC (survival_shop_all_ready()) or, failing
	// that, once the hard SURVIVAL_SHOP_DURATION cap runs out regardless of
	// who's still shopping. Runs on every machine off the same synced ready
	// flags, so whichever one is spawner authority reaches this the same
	// way everyone else's local shop UI does. The min() keeps an early
	// all-ready from *lengthening* the wait if this happens to run after
	// the cap's own deadline has already passed.
	if (Survival_shop_phase == SURVIVAL_SHOP_PHASE_OPEN &&
		(GameTime64 >= Survival_shop_open_until || survival_shop_all_ready()))
	{
		Survival_shop_phase = SURVIVAL_SHOP_PHASE_COUNTDOWN;
		if (GameTime64 + SURVIVAL_SHOP_COUNTDOWN_DURATION < Survival_next_wave_at)
			Survival_next_wave_at = GameTime64 + SURVIVAL_SHOP_COUNTDOWN_DURATION;
	}

	if (is_observer() || Player_num != survival_spawner_pnum())
		return;

	if (GameTime64 >= Survival_next_ammo_at)
	{
		survival_spawn_ammo();
		Survival_next_ammo_at = GameTime64 + SURVIVAL_AMMO_INTERVAL;
	}

	if (!Survival_wave_in_progress)
	{
		if (GameTime64 >= Survival_next_wave_at)
			survival_start_wave();
		return;
	}

	if (Survival_robots_to_spawn > 0)
	{
		if (GameTime64 >= Survival_next_spawn_at)
		{
			if (survival_count_active_robots() < SURVIVAL_MAX_ACTIVE_ROBOTS)
			{
				survival_spawn_one_robot(Survival_wave, Survival_wave_is_boss);
				Survival_robots_to_spawn--;
			}
			Survival_next_spawn_at = GameTime64 + SURVIVAL_SPAWN_TICK;
		}
		return;
	}

	// This wave's robots have all been queued -- once the mine's clear, end
	// the wave. Survival_next_wave_at is deliberately NOT set here: the
	// transition block above sets it on every machine (this one included) on
	// the following frame, so there is only one place that decides how long
	// the rest period is.
	if (survival_count_active_robots() == 0)
	{
		Survival_wave_in_progress = 0;
		survival_send_wave_state();
	}
}

// Hands input back cleanly when the shop lets go of it.
//
// Everything that means "this control is being held" lives in Controls and is
// edge-driven: kconfig_read_controls() ORs a state bit in on the key/button
// down event and ANDs it back out on the matching up event. While the shop
// owns input neither event reaches it -- presses are swallowed by the gate in
// ReadControls() (gamecntl.c) and releases die with should_read_controls == 0
// -- so whatever the player was holding the instant the shop opened stays
// latched for the whole shop, and the release that should have cleared it is
// simply dropped. The moment the shop closed, kconfig_read_controls() picked
// those stale bits straight back up and the ship thrust/turned on its own,
// with nothing left that could ever clear them; it came back at *full*
// deflection too, since the keyboard ramps (Controls.key_*_down_time) had long
// since saturated at F1_0. Only pressing and releasing that control again --
// or opening the ESC menu, which cleared it purely as a side effect of
// game_flush_inputs() on the way out -- ended it.
//
// So run exactly that flush ourselves, on the shop's own falling edge. It also
// covers the mouse: kconfig's accumulated axis and Controls.*_time_overrun are
// both inside control_info, and the queued SDL motion from the cursor being
// free (including the warp SDL does re-entering relative mode) goes with the
// event_flush()/mouse_get_delta() in there.
//
// Placement matters twice over. It has to be after survival_do_frame_inner(),
// because that's what clears Survival_shop_phase, and before object_move_all()
// in GameProcessFrame() (game.c) reaches read_flying_controls() -- multi_do_
// frame() runs earlier in that same function, so this lands in the right frame
// and no stale input ever gets applied at all.
static void survival_shop_release_stale_input(void)
{
	static int was_blocking = 0;
	int blocking = survival_shop_blocks_input();

	if (was_blocking && !blocking)
	{
		// Reclaim the mouse first, then flush: doing it the other way round
		// leaves the relative-mode warp's delta sitting in the queue with
		// nothing to drop it until the next frame. Harmless if the frame's
		// earlier survival_shop_do_mouse() already got there -- the flag
		// check inside makes it a no-op.
		survival_shop_set_mouse_released(0);
		game_flush_inputs();
	}

	was_blocking = blocking;
}

void survival_do_frame(void)
{
	survival_do_frame_inner();
	survival_shop_release_stale_input();
}

// Detects wave-start/wave-clear transitions purely from state that's now
// synced to every machine (Survival_wave / Survival_wave_is_boss /
// Survival_wave_in_progress -- see multi_do_survival_wave_state()), so the
// centered banner and the clear-sound both fire identically on every
// client's own render frame without needing a dedicated broadcast.
static void survival_update_hud_transitions(void)
{
	if (Survival_wave == Survival_hud_prev_wave && Survival_wave_in_progress == Survival_hud_prev_in_progress)
		return;

	if (Survival_wave_in_progress && Survival_wave != Survival_hud_prev_wave)
	{
		char buf[48];

		sprintf(buf, "WAVE %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS!" : "");
		survival_banner(buf, SURVIVAL_BANNER_DURATION, Survival_wave_is_boss ? SURVIVAL_SND_SIREN : -1);
	}
	else if (!Survival_wave_in_progress && Survival_hud_prev_in_progress == 1)
	{
		int snd;

		if (Newdemo_state != ND_STATE_PLAYBACK && (snd = Powerup_info[POW_EXTRA_LIFE].hit_sound) > -1)
			digi_play_sample(snd, F1_0);
	}

	Survival_hud_prev_wave = Survival_wave;
	Survival_hud_prev_in_progress = Survival_wave_in_progress;
}

void survival_draw_hud(void)
{
	char buf[48];
	int row = 1; // stacked top-left, one LINE_SPACING apart

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	survival_update_hud_transitions();

	gr_set_curfont(GAME_FONT);
	gr_set_fontcolor(BM_XRGB(0, 28, 31), -1);

	// Nothing here before wave 1 -- that's the centered "GET READY..."
	// banner's job now (survival_do_countdown()).
	if (Survival_wave_in_progress)
	{
		sprintf(buf, "Wave %d%s", Survival_wave, Survival_wave_is_boss ? " - BOSS" : "");
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1), buf);
	}
	else if (Survival_wave > 0)
	{
		sprintf(buf, "Wave %d cleared", Survival_wave);
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1), buf);
	}

	// The extra-life count lives in the bottom-left gauge stack, next to
	// shields and energy -- see hud_show_survival_extra_lives() in gauges.c.

	if (survival_is_eliminated(Player_num))
	{
		gr_set_fontcolor(BM_XRGB(31, 0, 0), -1);
		gr_string(FSPACX(2), LINE_SPACING * row++ + FSPACY(1),
			Survival_wave_in_progress ? "DOWN - Spectating (back next wave)" : "DOWN - Spectating");
	}

	if (GameTime64 < Survival_banner_until)
	{
		int w, h, aw;
		

		gr_set_curfont(MEDIUM2_FONT);
		gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
		gr_get_string_size(Survival_banner_text, &w, &h, &aw);
		gr_string((GWIDTH - w) / 2, GHEIGHT / 4, Survival_banner_text);
	}
}

// Shop icons are the game's own powerup sprites -- Vclip[Powerup_info[id].
// vclip_num].frames[0], the exact bitmap draw_powerup() (powerup.c) blits
// for that powerup in the world, so a Vulcan ammo icon here is literally
// the Vulcan ammo pickup.
//
// An earlier pass at this squashed each bitmap into a fixed NxN square,
// which stretched every non-square sprite into something unrecognizable --
// that, not the choice of asset, is what made the icons look wrong. Fit
// preserves the source's aspect ratio inside the box instead.
static void survival_shop_blit_icon(int cx, int cy, int box, bitmap_index bmi)
{
	grs_bitmap *bm;
	int w, h;

	PIGGY_PAGE_IN(bmi);
	bm = &GameBitmaps[bmi.index];
	if (!bm || bm->bm_w <= 0 || bm->bm_h <= 0)
		return;

	// Longest side fills the box; the other is scaled to match, so the
	// sprite keeps its proportions.
	if (bm->bm_w >= bm->bm_h)
	{
		w = box;
		h = box * bm->bm_h / bm->bm_w;
	}
	else
	{
		h = box;
		w = box * bm->bm_w / bm->bm_h;
	}
	if (w <= 0) w = 1;
	if (h <= 0) h = 1;

#ifdef OGL
	ogl_ubitmapm_cs(cx - w / 2, cy - h / 2, w, h, bm, -1, F1_0);
#else
	// The software blitter can't scale; it draws at native size. Same
	// limitation gauges.c's own hud_bitblt() has in non-OGL builds.
	gr_ubitmapm(cx - w / 2, cy - h / 2, bm);
#endif
}

// Resolves a powerup id to its world sprite, guarding every step -- a
// mission with a trimmed vclip table could leave any of these empty, and a
// bad bitmap_index would otherwise be blitted as garbage. Returns 0 if
// there's no usable sprite, so callers can skip that glyph entirely.
static int survival_shop_powerup_icon(int powerup_id, bitmap_index *out)
{
	int vc;

	if (powerup_id < 0 || powerup_id >= MAX_POWERUP_TYPES)
		return 0;

	vc = Powerup_info[powerup_id].vclip_num;
	if (vc < 0 || vc >= VCLIP_MAXNUM)
		return 0;
	if (Vclip[vc].num_frames <= 0)
		return 0;

	*out = Vclip[vc].frames[0];
	return 1;
}

// Draws a powerup sprite if one resolves; silently draws nothing if not.
static void survival_shop_draw_powerup_icon(int cx, int cy, int box, int powerup_id)
{
	bitmap_index bmi;

	if (survival_shop_powerup_icon(powerup_id, &bmi))
		survival_shop_blit_icon(cx, cy, box, bmi);
}

// The "?" at the center of both randomized rows, in the yellow the panel
// already uses for its title -- deliberately NOT the row's affordability
// color, since the point is that the contents are unknown, and the flanking
// sprites carry the "what kind of thing" signal on their own.
static void survival_shop_draw_qmark(int cx, int cy)
{
	int w, h, aw;

	gr_set_curfont(MEDIUM2_FONT);
	gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
	gr_get_string_size("?", &w, &h, &aw);
	gr_string(cx - w / 2, cy - h / 2, "?");
	gr_set_curfont(GAME_FONT);
}

// The two physics upgrades have no powerup to borrow from (D1 has no speed
// pickup -- POW_TURBO is in the enum but has no D1 bitmap -- and armor
// isn't a pickup at all), and an invented glyph for either one only ever
// said "some kind of upgrade". The per-tier number says exactly what the
// purchase does, which is the thing worth knowing, so it takes the icon
// column instead. Centered in that column so the labels still line up with
// the sprite rows above.
static void survival_shop_draw_icon_text(int cx, int cy, const char *text, int color)
{
	int w, h, aw;

	gr_set_fontcolor(color, -1);
	gr_get_string_size(text, &w, &h, &aw);
	gr_string(cx - w / 2, cy - h / 2, text);
}

// Icon column: wide enough for a sprite / "?" / sprite trio on the two
// randomized rows. Single-icon rows just center in the same column, so
// every label still starts at the same x.
#define SURVIVAL_SHOP_ROW_ICON_SIZE FSPACX(15)
#define SURVIVAL_SHOP_ROW_ICON_W    FSPACX(42)
#define SURVIVAL_SHOP_ROW_WING_SIZE FSPACX(12)
#define SURVIVAL_SHOP_ROW_WING_DX   FSPACX(13)
#define SURVIVAL_SHOP_ROW_KEY_W     FSPACX(20)
#define SURVIVAL_SHOP_ROW_ICON_GAP  FSPACX(8)

// icon_mode for survival_shop_draw_row(): which of the glyphs above (or
// none) this row gets.
#define SURVIVAL_SHOP_ICON_NONE    0
#define SURVIVAL_SHOP_ICON_WEAPON  1   // "?" flanked by the spreadfire + fusion pickups
#define SURVIVAL_SHOP_ICON_SUPPLY  2   // "?" flanked by the shield + vulcan ammo pickups
#define SURVIVAL_SHOP_ICON_SHIELD  3   // the shield pickup
#define SURVIVAL_SHOP_ICON_SPEED   4   // chevrons (no D1 speed pickup to borrow)
#define SURVIVAL_SHOP_ICON_ARMOR   5   // the shield pickup

// One shop row: bracketed key, an icon, a label, and a right-aligned price
// or status word. Also the mouse hit-testing for this row: stamps
// Survival_shop_row_rect[idx] every call (screen-space, canvas offset
// folded in so survival_shop_do_mouse() can compare it directly against
// mouse_get_pos()) and, while hovered, brightens the row's background --
// the same affordability color-coding (cyan/gray/gold) makes clicking an
// unaffordable or maxed row visibly pointless before you try.
static void survival_shop_draw_row(int idx, int panel_x, int panel_right, int row_y, int row_h,
	int key, int icon_mode, const char *label, int price, int maxed)
{
	char keybuf[4], pricebuf[16];
	int w, h, aw, mx, my, mz, hovered, icon_x, icon_cx, icon_cy, label_x, color;

	Survival_shop_row_rect[idx].x1 = grd_curcanv->cv_bitmap.bm_x + panel_x;
	Survival_shop_row_rect[idx].y1 = grd_curcanv->cv_bitmap.bm_y + row_y;
	Survival_shop_row_rect[idx].x2 = grd_curcanv->cv_bitmap.bm_x + panel_right;
	Survival_shop_row_rect[idx].y2 = grd_curcanv->cv_bitmap.bm_y + row_y + row_h;

	mouse_get_pos(&mx, &my, &mz);
	hovered = (mx >= Survival_shop_row_rect[idx].x1) && (mx < Survival_shop_row_rect[idx].x2) &&
	          (my >= Survival_shop_row_rect[idx].y1) && (my < Survival_shop_row_rect[idx].y2);

	if (hovered && !maxed)
	{
		// Translucent, so the menu texture underneath still reads -- a
		// solid bar would punch a flat hole in nm_draw_background()'s
		// panel. Same blend that function uses for its own beveled edges.
		gr_settransblend(14, GR_BLEND_NORMAL);
		gr_setcolor(BM_XRGB(0, 20, 24));
		gr_rect(panel_x - FSPACX(2), row_y, panel_right + FSPACX(2), row_y + row_h);
		gr_settransblend(GR_FADE_OFF, GR_BLEND_NORMAL);
	}

	if (maxed)
		color = BM_XRGB(27, 24, 0);
	else if (Players[Player_num].score >= price)
		color = BM_XRGB(0, 28, 31);
	else
		// Lighter than it needs to be on a black panel -- these now sit on
		// the menu's textured background, where a near-black grey would
		// disappear into it entirely.
		color = BM_XRGB(20, 20, 20);

	icon_x = panel_x + SURVIVAL_SHOP_ROW_KEY_W;
	icon_cx = icon_x + SURVIVAL_SHOP_ROW_ICON_W / 2;
	icon_cy = row_y + row_h / 2;
	label_x = icon_x + SURVIVAL_SHOP_ROW_ICON_W + SURVIVAL_SHOP_ROW_ICON_GAP;

	switch (icon_mode)
	{
		case SURVIVAL_SHOP_ICON_WEAPON:
			// Two real weapon pickups poking out from behind the "?" --
			// drawn first so the mark overlaps them rather than the other
			// way round, which is what sells them as being *behind* it.
			survival_shop_draw_powerup_icon(icon_cx - SURVIVAL_SHOP_ROW_WING_DX, icon_cy,
				SURVIVAL_SHOP_ROW_WING_SIZE, POW_SPREADFIRE_WEAPON);
			survival_shop_draw_powerup_icon(icon_cx + SURVIVAL_SHOP_ROW_WING_DX, icon_cy,
				SURVIVAL_SHOP_ROW_WING_SIZE, POW_FUSION_WEAPON);
			survival_shop_draw_qmark(icon_cx, icon_cy);
			break;
		case SURVIVAL_SHOP_ICON_SUPPLY:
			survival_shop_draw_powerup_icon(icon_cx - SURVIVAL_SHOP_ROW_WING_DX, icon_cy,
				SURVIVAL_SHOP_ROW_WING_SIZE, POW_SHIELD_BOOST);
			survival_shop_draw_powerup_icon(icon_cx + SURVIVAL_SHOP_ROW_WING_DX, icon_cy,
				SURVIVAL_SHOP_ROW_WING_SIZE, POW_VULCAN_AMMO);
			survival_shop_draw_qmark(icon_cx, icon_cy);
			break;
		case SURVIVAL_SHOP_ICON_SHIELD:
			survival_shop_draw_powerup_icon(icon_cx, icon_cy, SURVIVAL_SHOP_ROW_ICON_SIZE, POW_SHIELD_BOOST);
			break;
		case SURVIVAL_SHOP_ICON_SPEED:
		{
			// Built from the same constant the effect uses, so the number
			// on screen can never drift from what a purchase actually does.
			char pct[12];
			sprintf(pct, "+%d.%d%%", SURVIVAL_SHOP_SPEED_PERMILLE_PER_TIER / 10,
				SURVIVAL_SHOP_SPEED_PERMILLE_PER_TIER % 10);
			survival_shop_draw_icon_text(icon_cx, icon_cy, pct, color);
			break;
		}
		case SURVIVAL_SHOP_ICON_ARMOR:
		{
			char pct[12];
			sprintf(pct, "-%d%%", SURVIVAL_SHOP_ARMOR_PCT_PER_TIER);
			survival_shop_draw_icon_text(icon_cx, icon_cy, pct, color);
			break;
		}
		default:
			break;
	}

	gr_set_fontcolor(color, -1);
	sprintf(keybuf, "[%d]", key);
	gr_string(panel_x, row_y + FSPACY(2), keybuf);
	gr_string(label_x, row_y + FSPACY(2), label);

	if (maxed)
		strcpy(pricebuf, "MAXED");
	else
		sprintf(pricebuf, "%d", price);
	gr_get_string_size(pricebuf, &w, &h, &aw);
	gr_string(panel_right - w, row_y + FSPACY(2), pricebuf);
}

// Clamps a panel's top-left so it can never run off the bottom (or top) of
// the screen -- true centering ((GHEIGHT-panel_h)/2) still isn't enough on
// its own once a panel is tall, since GHEIGHT/2 plus panel_h can still
// exceed GHEIGHT if panel_h is more than half the screen.
static int survival_shop_panel_y(int panel_h)
{
	int y = (GHEIGHT - panel_h) / 2;

	if (y + panel_h > GHEIGHT - FSPACY(8))
		y = GHEIGHT - panel_h - FSPACY(8);
	if (y < FSPACY(8))
		y = FSPACY(8);
	return y;
}

// The interactive buy list -- only shown while survival_shop_is_open().
static void survival_shop_draw_buy_panel(void)
{
	char buf[64];
	int w, h, aw;
	int panel_w, panel_h, x, y, inner_x, inner_right, row_y, row_h, row_gap;
	fix64 remaining;
	int secs;

	gr_set_curfont(GAME_FONT);

	// Spaced out well beyond the text's own line height so each row reads
	// as a distinct clickable button rather than a packed list.
	row_h = LINE_SPACING + FSPACY(10);
	row_gap = FSPACY(4);
	panel_w = FSPACX(240);

	// Summed from exactly the pieces laid out below, in the same order, so
	// the height and the content can't drift apart the way they did when
	// this was a hand-tuned constant -- the footer ended up hanging off the
	// bottom edge. The closing pad is deliberately generous: nm_draw_
	// background() draws a beveled shadow along its bottom edge, so
	// anything flush to panel_h renders underneath it.
	panel_h = FSPACY(6)                                       // top pad
	        + LINE_SPACING + FSPACY(4)                        // title
	        + FSPACY(4)                                       // below title divider
	        + (row_h + row_gap) * SURVIVAL_SHOP_NUM_ITEMS     // the rows
	        + FSPACY(4)                                       // above footer divider
	        + LINE_SPACING * 2                                // score line + hint line
	        + FSPACY(12);                                     // bottom pad, clears the bevel

	x = (GWIDTH - panel_w) / 2;
	y = survival_shop_panel_y(panel_h);

	// The engine's own menu frame -- the same textured panel and beveled
	// edge every stock Descent menu sits on (newmenu.c). Calling it
	// directly rather than going through newmenu_do() is the whole trick:
	// nm_draw_background() is just a draw call, so we get the authentic
	// menu chrome without newmenu's blocking event loop, which would stall
	// this machine's own packet processing (see survival_shop_handle_key()).
	nm_draw_background(x, y, x + panel_w, y + panel_h);

	inner_x = x + FSPACX(10);
	inner_right = x + panel_w - FSPACX(10);
	row_y = y + FSPACY(6);

	// Title, with the shop's own countdown timer instead of a raw seconds
	// counter now that the window runs up to a minute -- M:SS reads a lot
	// better than "53 SEC LEFT" once it's not single digits.
	gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
	remaining = Survival_shop_open_until - GameTime64;
	if (remaining < 0)
		remaining = 0;
	secs = (int)((remaining + F1_0 - 1) / F1_0);
	sprintf(buf, "SURVIVAL SHOP -- %d:%02d", secs / 60, secs % 60);
	gr_get_string_size(buf, &w, &h, &aw);
	gr_string(x + (panel_w - w) / 2, row_y, buf);
	row_y += LINE_SPACING + FSPACY(4);

	// Thin divider under the title, same accent color as the border.
	gr_setcolor(BM_XRGB(0, 12, 14));
	gr_rect(inner_x, row_y - FSPACY(3), inner_right, row_y - FSPACY(3) + 1);
	row_y += FSPACY(4);

	survival_shop_draw_row(0, inner_x, inner_right, row_y, row_h, 1, SURVIVAL_SHOP_ICON_WEAPON, "Random Weapon", survival_shop_weapon_price(), 0);
	row_y += row_h + row_gap;
	survival_shop_draw_row(1, inner_x, inner_right, row_y, row_h, 2, SURVIVAL_SHOP_ICON_SUPPLY, "Random Supply", survival_shop_supply_price(), 0);
	row_y += row_h + row_gap;
	survival_shop_draw_row(2, inner_x, inner_right, row_y, row_h, 3, SURVIVAL_SHOP_ICON_SHIELD, "Shield Restore (cap 100)", survival_shop_shield_price(), 0);
	row_y += row_h + row_gap;

	sprintf(buf, "Speed Boost (%d/%d)", Survival_speed_tier, SURVIVAL_SHOP_SPEED_MAX_TIER);
	survival_shop_draw_row(3, inner_x, inner_right, row_y, row_h, 4, SURVIVAL_SHOP_ICON_SPEED, buf, survival_shop_speed_price(),
		Survival_speed_tier >= SURVIVAL_SHOP_SPEED_MAX_TIER);
	row_y += row_h + row_gap;

	sprintf(buf, "Armor Plating (%d/%d)", Survival_armor_tier, SURVIVAL_SHOP_ARMOR_MAX_TIER);
	survival_shop_draw_row(4, inner_x, inner_right, row_y, row_h, 5, SURVIVAL_SHOP_ICON_ARMOR, buf, survival_shop_armor_price(),
		Survival_armor_tier >= SURVIVAL_SHOP_ARMOR_MAX_TIER);
	row_y += row_h + FSPACY(4);

	// Divider above the footer, same as the one under the title.
	gr_setcolor(BM_XRGB(0, 12, 14));
	gr_rect(inner_x, row_y - FSPACY(3), inner_right, row_y - FSPACY(3) + 1);
	row_y += FSPACY(4);

	// Footer is two stacked lines, not two strings sharing one. Side by
	// side, a six-digit score and the full key hint together are wider than
	// the panel, so they overlapped into unreadable mush.
	{
		int ready, total;

		gr_set_fontcolor(BM_XRGB(31, 31, 31), -1);
		sprintf(buf, "Your Score: %d", Players[Player_num].score);
		gr_string(inner_x, row_y, buf);

		// Right of the score: how many players still have the shop open.
		// Short enough that it can't collide with even a huge score, and
		// it's the one thing worth knowing here -- whether everyone else
		// is already waiting on you.
		survival_shop_ready_counts(&ready, &total);
		sprintf(buf, "%d/%d ready", ready, total);
		gr_get_string_size(buf, &w, &h, &aw);
		gr_string(inner_right - w, row_y, buf);
		row_y += LINE_SPACING;

		gr_set_fontcolor(BM_XRGB(20, 20, 20), -1);
		strcpy(buf, "[ESC] Ready Up   -   [1]-[5] or Click to Buy");
		gr_get_string_size(buf, &w, &h, &aw);
		gr_string(x + (panel_w - w) / 2, row_y, buf);
	}
}

// Shown once the local player has readied up but the group hasn't started
// the wave yet (still SURVIVAL_SHOP_PHASE_OPEN with everyone else still
// shopping, or SURVIVAL_SHOP_PHASE_COUNTDOWN once the wait has actually
// ended) -- a small status panel instead of the buy list, since there's
// nothing left to interact with here.
static void survival_shop_draw_waiting_panel(void)
{
	char buf[64];
	int w, h, aw, panel_w, panel_h, x, y, row_y;
	int ready, total;
	fix64 remaining;
	int secs;

	gr_set_curfont(GAME_FONT);

	// Wide enough for its longest line, and tall enough to clear
	// nm_draw_background()'s bottom bevel -- same sizing mistake the buy
	// panel had, where the closing line rendered under the frame's edge.
	panel_w = FSPACX(210);
	panel_h = FSPACY(6)                       // top pad
	        + LINE_SPACING + FSPACY(6)        // title
	        + LINE_SPACING + FSPACY(4)        // ready count
	        + LINE_SPACING                    // countdown line
	        + FSPACY(12);                     // bottom pad, clears the bevel

	x = (GWIDTH - panel_w) / 2;
	y = survival_shop_panel_y(panel_h);

	// Same engine menu frame as the buy panel -- see survival_shop_draw_buy_panel().
	nm_draw_background(x, y, x + panel_w, y + panel_h);

	row_y = y + FSPACY(6);

	gr_set_fontcolor(BM_XRGB(31, 31, 0), -1);
	strcpy(buf, Survival_shop_phase == SURVIVAL_SHOP_PHASE_COUNTDOWN ? "WAVE INCOMING" : "READY");
	gr_get_string_size(buf, &w, &h, &aw);
	gr_string(x + (panel_w - w) / 2, row_y, buf);
	row_y += LINE_SPACING + FSPACY(6);

	survival_shop_ready_counts(&ready, &total);
	gr_set_fontcolor(BM_XRGB(0, 28, 31), -1);
	sprintf(buf, "%d / %d players ready", ready, total);
	gr_get_string_size(buf, &w, &h, &aw);
	gr_string(x + (panel_w - w) / 2, row_y, buf);
	row_y += LINE_SPACING + FSPACY(4);

	remaining = Survival_next_wave_at - GameTime64;
	if (remaining < 0)
		remaining = 0;
	secs = (int)((remaining + F1_0 - 1) / F1_0);
	gr_set_fontcolor(BM_XRGB(20, 20, 20), -1);
	sprintf(buf, "Wave starts in %d:%02d", secs / 60, secs % 60);
	gr_get_string_size(buf, &w, &h, &aw);
	gr_string(x + (panel_w - w) / 2, row_y, buf);
}

// Shop overlay: up for the whole shop experience -- buying, readied-but-
// waiting, and the trailing voice countdown alike (survival_shop_blocks_
// input()). A flat 2D panel drawn every frame, not a real newmenu_do() menu
// -- see survival_shop_handle_key() for why nothing in this mode can block
// on a modal without stalling the local machine's own network processing.
void survival_shop_draw(void)
{
	if (!survival_shop_blocks_input())
		return;

	if (survival_shop_is_open())
		survival_shop_draw_buy_panel();
	else
		survival_shop_draw_waiting_panel();
}

// Common body of survival_note_robot_kill() and survival_convert_wasted_pickup() below -- both are
// "float a +points number at this world position", the only difference being what position and what
// triggered it.
static void survival_spawn_score_popup(vms_vector *pos, int points)
{
	survival_kill_popup *p;

	if (Netgame.gamemode != NETGAME_SURVIVAL)
		return;
	if (Newdemo_state == ND_STATE_PLAYBACK)
		return;

	if (points <= 0)
		return; // nothing worth floating a number for

	p = &Survival_popups[Survival_popup_idx];
	Survival_popup_idx = (Survival_popup_idx + 1) % SURVIVAL_MAX_POPUPS;

	p->pos = *pos;
	p->points = points;
	p->born = GameTime64;
}

void survival_note_robot_kill(object *robot, int points)
{
	survival_spawn_score_popup(&robot->pos, points);
}

// Scrap value for a powerup do_powerup() (powerup.c) left on the floor because the player already
// has or is holding whatever it offers -- ammo/shields already full, a primary already owned, cloak
// or invulnerability already active, laser already at MAX_LASER_LEVEL. 0 for anything not in this
// list, which is deliberate: only the specific set of pickups Survival's own tables can ever produce
// (Survival_ammo_types[] / Survival_weapon_types[] / Survival_shop_supply_types[] above) are covered
// -- keys, flags, hostages and anything else stay exactly as untouched as they were before this
// existed.
//
// Values are a flat, independent scale, not a fraction of the shop's own prices: those went up
// sharply (see SURVIVAL_SHOP_PRICE_WEAPON and friends) to matter against a match-long score, and
// pricing scrap off them would mean a floor full of duplicate ammo drops paying out shop-purchase
// money by itself. This is meant to read as "not wasted", not as "as good as buying it".
#define SURVIVAL_SCRAP_AMMO             40   // POW_ENERGY, POW_VULCAN_AMMO
#define SURVIVAL_SCRAP_SHIELD           60   // POW_SHIELD_BOOST
#define SURVIVAL_SCRAP_SECONDARY_1      50   // POW_MISSILE_1, POW_HOMING_AMMO_1
#define SURVIVAL_SCRAP_SECONDARY_4     150   // POW_MISSILE_4, POW_HOMING_AMMO_4
#define SURVIVAL_SCRAP_SITUATIONAL     120   // POW_CLOAK, POW_INVULNERABILITY
#define SURVIVAL_SCRAP_PRIMARY_COMMON  200   // LASER/VULCAN/SPREADFIRE/PLASMA/QUAD_FIRE
#define SURVIVAL_SCRAP_PRIMARY_RARE    350   // FUSION
#define SURVIVAL_SCRAP_SECONDARY_RARE  350   // PROXIMITY, SMARTBOMB
#define SURVIVAL_SCRAP_MEGA            500   // MEGA_WEAPON
static int survival_pickup_scrap_value(int id)
{
	switch (id)
	{
		case POW_ENERGY:
		case POW_VULCAN_AMMO:
			return SURVIVAL_SCRAP_AMMO;
		case POW_SHIELD_BOOST:
			return SURVIVAL_SCRAP_SHIELD;
		case POW_MISSILE_1:
		case POW_HOMING_AMMO_1:
			return SURVIVAL_SCRAP_SECONDARY_1;
		case POW_MISSILE_4:
		case POW_HOMING_AMMO_4:
			return SURVIVAL_SCRAP_SECONDARY_4;
		case POW_CLOAK:
		case POW_INVULNERABILITY:
			return SURVIVAL_SCRAP_SITUATIONAL;
		case POW_LASER:
		case POW_VULCAN_WEAPON:
		case POW_SPREADFIRE_WEAPON:
		case POW_PLASMA_WEAPON:
		case POW_QUAD_FIRE:
			return SURVIVAL_SCRAP_PRIMARY_COMMON;
		case POW_FUSION_WEAPON:
			return SURVIVAL_SCRAP_PRIMARY_RARE;
		case POW_PROXIMITY_WEAPON:
		case POW_SMARTBOMB_WEAPON:
			return SURVIVAL_SCRAP_SECONDARY_RARE;
		case POW_MEGA_WEAPON:
			return SURVIVAL_SCRAP_MEGA;
		default:
			return 0;
	}
}

// Call from collide_player_and_powerup() (collide.c) exactly when do_powerup() left the object on
// the floor: a legitimate pickup attempt (right mode, right player -- collide_player_and_powerup()
// already filtered the CTF/co-op special cases before ever calling do_powerup()) that had nothing
// left to give. Late in a Survival match, with waves of robots dropping loot and only one or two
// things left to actually want, this used to mean the floor filling up with pickups nobody could
// ever collect. Now it is scrapped for a small amount of score and removed, same as it would have
// been had it actually been used -- returns 1 to tell the caller exactly that.
//
// Deliberately not folded into do_powerup() itself: that function is shared by every game mode and
// has non-collision call sites too (gamecntl.c's debug pickup-everything key,
// survival_shop_grant_powerup()'s own do_powerup() call for a *guaranteed* grant), and this is
// Survival-only, floor-collision-only behaviour.
// True for anything scrappable except the three plain ammo/energy drops -- those are common enough
// (every ammo-tier robot drop or shop supply that's already at cap goes through here) that announcing
// every single one would drown the chat in noise. A scrapped weapon, secondary, or situational
// pickup is rare enough by comparison to be worth telling the team about.
static int survival_pickup_is_weapon_tier(int id)
{
	return id != POW_ENERGY && id != POW_SHIELD_BOOST && id != POW_VULCAN_AMMO;
}

// Minimum gap between scrap broadcasts. Without one, flying through a cluster of loot you can't use
// -- which is exactly the late-match situation this whole feature exists for -- fires one
// whole-lobby chat broadcast per powerup touched, back to back. That is both unreadable and a real
// packet-rate problem on a busy game.
#define SURVIVAL_SCRAP_ANNOUNCE_COOLDOWN  (F1_0 * 8)

// Broadcasts "so-and-so scrapped something" to the lobby. Local feedback is NOT done here (the
// caller's HUD_init_message() handles that) because multi_send_message() deliberately does not echo
// to the sender -- it only transmits and logs, so a player never sees their own broadcast.
static void survival_announce_scrap(int points)
{
	static fix64 last_announce = 0;

	// Refuses to touch Network_message while the player is composing a chat line -- it is the *same
	// buffer* multi_message_input_sub() types into (see multi_send_message_start(), multi.c), so
	// writing here mid-compose would silently eat whatever they had typed so far.
	if (multi_sending_message[Player_num] || multi_defining_message)
		return;

	// GameTime64 < last_announce catches a level change resetting the clock, which would otherwise
	// leave a deadline far in the future and suppress every announcement for the rest of the match.
	if (GameTime64 < last_announce || GameTime64 - last_announce < SURVIVAL_SCRAP_ANNOUNCE_COOLDOWN)
		return;

	last_announce = GameTime64;

	// Network_message_reciever = 100 broadcasts to the whole lobby, same as the CTF flag-pickup
	// announcement (powerup.c) this is modeled on. No callsign in the text: multi_do_message()
	// (multi.c) prepends the sender's own callsign for every recipient.
	//
	// No item name either, much as it'd read better -- Network_message is Network_message[MAX_
	// MESSAGE_LEN], and MAX_MESSAGE_LEN (multi.h) is 35. "scrapped a spare " alone is 18 of those,
	// and Powerup_names[] entries run up to 16 more (POWERUP_NAME_LENGTH, powerup.h) -- tacking the
	// name on would overflow the buffer on a long name (sprintf has no bound) or truncate mid-word
	// (snprintf). The local HUD message the caller shows does name the item; it has room to.
	snprintf(Network_message, MAX_MESSAGE_LEN, "scrapped spares: +%d pts", points);
	Network_message_reciever = 100;
}

int survival_convert_wasted_pickup(object *powerup)
{
	int points;

	if (!(Game_mode & GM_MULTI) || Netgame.gamemode != NETGAME_SURVIVAL)
		return 0;
	if (is_observer())
		return 0;

	points = survival_pickup_scrap_value(powerup->id);
	if (points <= 0)
		return 0;

	add_points_to_score(points);

	// Local feedback goes through HUD_init_message(), NOT the floating score popup the kill feedback
	// uses. The popup would be invisible here every single time: survival_draw_kill_popups() drops any
	// popup whose g3_rotate_point() comes back with p3_codes != 0, and a powerup you are colliding with
	// is by definition at your own ship's position -- behind the near clip plane, rejected on every
	// frame of the popup's life. Kill popups work only because robots die at a distance. This is the
	// same channel powerup.c itself uses to tell you a pickup was wasted ("MAXED OUT" etc), so it also
	// reads consistently with the message it replaces.
	HUD_init_message(HM_DEFAULT, "%s SCRAPPED: +%d POINTS", Powerup_names[powerup->id], points);

	// hit_sound is genuinely -1 for some powerup types -- every other caller in powerup.c guards for
	// it (see the `> -1` tests around powerup_basic()'s own playback), and passing -1 straight to
	// digi_play_sample() indexes the sound table out of bounds. Fall back to the chat/HUD blip, which
	// is always present, so a scrap always makes *some* noise.
	{
		int snd = Powerup_info[powerup->id].hit_sound;

		if (snd < 0)
			snd = SOUND_HUD_MESSAGE;

		digi_play_sample(snd, F1_0);
		multi_send_play_sound(snd, F1_0);
	}

	// The team broadcast is separate from, and much rarer than, the local feedback above -- see
	// survival_announce_scrap().
	if (survival_pickup_is_weapon_tier(powerup->id))
		survival_announce_scrap(points);

	return 1;
}

// The crosshair's own configured color (show_reticle() in gauges.c reads the
// same PlayerCfg.ReticleRGBA), so every Survival overlay reads as part of the
// reticle rather than as a separate HUD element. Components are on BM_XRGB's
// 0..31 scale. A configured-black reticle falls back to the stock green, so
// the overlays can never render themselves invisible.
static void survival_reticle_rgb(int *r, int *g, int *b)
{
	int c[3], i;

	c[0] = PlayerCfg.ReticleRGBA[0];
	c[1] = PlayerCfg.ReticleRGBA[1];
	c[2] = PlayerCfg.ReticleRGBA[2];

	if (c[0] <= 0 && c[1] <= 0 && c[2] <= 0)
	{
		c[0] = RET_COLOR_DEFAULT_R;
		c[1] = RET_COLOR_DEFAULT_G;
		c[2] = RET_COLOR_DEFAULT_B;
	}

	for (i = 0; i < 3; i++)
	{
		if (c[i] < 0)
			c[i] = 0;
		if (c[i] > 31)
			c[i] = 31;
	}

	*r = c[0];
	*g = c[1];
	*b = c[2];
}

// Same hue, scaled brightness (scale is 0..F1_0). This is how both the
// darker accent and the arrow's distance shading stay on the reticle's color
// instead of introducing a second one.
static int survival_shade(int r, int g, int b, fix scale)
{
	if (scale < 0)
		scale = 0;
	if (scale > F1_0)
		scale = F1_0;

	return BM_XRGB(f2i(fixmul(i2f(r), scale)),
	               f2i(fixmul(i2f(g), scale)),
	               f2i(fixmul(i2f(b), scale)));
}

// "+points" rising from where each robot died. Anchored to the world point
// (projected every frame) rather than to the screen, so it stays over the
// kill as you keep flying -- the rise and the fade are what make it read as
// a score popup rather than a label.
static void survival_draw_kill_popups(void)
{
	int i, cr, cg, cb;

	gr_set_curfont(GAME_FONT);
	survival_reticle_rgb(&cr, &cg, &cb);

	for (i = 0; i < SURVIVAL_MAX_POPUPS; i++)
	{
		survival_kill_popup *p = &Survival_popups[i];
		g3s_point pt;
		fix age, life_frac, bright;
		char buf[16];
		int w, h, aw, rise, x, y;

		if (!p->born)
			continue;

		age = (fix)(GameTime64 - p->born);
		if (age < 0 || age >= SURVIVAL_POPUP_LIFE)
		{
			p->born = 0;
			continue;
		}

		g3_rotate_point(&pt, &p->pos);
		if (pt.p3_codes != 0) // outside the view frustum
			continue;

		g3_project_point(&pt);
		if (pt.p3_flags & PF_OVERFLOW)
			continue;

		life_frac = fixdiv(age, SURVIVAL_POPUP_LIFE); // 0..F1_0
		rise = f2i(fixmul(i2f(SURVIVAL_POPUP_RISE_PIXELS), life_frac));

		// Full reticle color at first, fading toward its dark end as the
		// popup rises. Never all the way to black, so a number that is
		// still on screen is still legible.
		bright = F1_0 - fixmul(F1_0 * 7 / 10, life_frac);

		sprintf(buf, "+%d", p->points);
		gr_get_string_size(buf, &w, &h, &aw);
		x = f2i(pt.p3_sx) - w / 2;
		y = f2i(pt.p3_sy) - rise;

		// Darker same-hue accent offset a pixel down-right, then the number
		// itself on top: keeps the text readable over bright textures
		// without introducing a color the reticle doesn't already use.
		gr_set_fontcolor(survival_shade(cr, cg, cb, fixmul(bright, SURVIVAL_ACCENT_SCALE)), -1);
		gr_string(x + 1, y + 1, buf);

		gr_set_fontcolor(survival_shade(cr, cg, cb, bright), -1);
		gr_string(x, y, buf);
	}
}

// Nearest live robot to the player, or -1. Straight-line distance, deliberately
// ignoring walls: the arrow is a "where is the horde" aid, matching the fact
// that the robots themselves see through walls in this mode.
static int survival_nearest_robot(fix *out_dist)
{
	int i, best = -1;
	fix best_dist = 0x7fffffff;
	vms_vector *from;

	if (Players[Player_num].objnum < 0)
		return -1;

	from = &Objects[Players[Player_num].objnum].pos;

	for (i = 0; i <= Highest_object_index; i++)
	{
		fix d;

		if (Objects[i].type != OBJ_ROBOT)
			continue;
		if (Objects[i].flags & (OF_SHOULD_BE_DEAD | OF_EXPLODING))
			continue;

		d = vm_vec_dist_quick(from, &Objects[i].pos);
		if (d < best_dist)
		{
			best_dist = d;
			best = i;
		}
	}

	if (best >= 0 && out_dist)
		*out_dist = best_dist;

	return best;
}

// Arrowhead orbiting the reticle, pointing the way you have to turn to face
// the nearest robot. Direction comes from the target's *view-space* position
// (g3_rotate_point's p3_x/p3_y, i.e. right/up relative to where you're
// looking) rather than its screen projection, because the projection is
// meaningless once the target is off screen or behind you -- which is
// precisely when the arrow matters.
//
// It is drawn entirely in the reticle's own color: brightness carries the
// range to the target -- dark when it's right on top of you, light when it's
// far off -- so distance reads at a glance without a second hue or a number.
static void survival_draw_robot_arrow(int cx, int cy)
{
	fix dist = 0;
	int objnum = survival_nearest_robot(&dist);
	g3s_point pt;
	fix dx, dy;
	fixang ang;
	fix s, c, range, bright;
	int cr, cg, cb;
	int ux, uy, px, py;
	int tipx, tipy, b1x, b1y, b2x, b2y;

	if (objnum < 0)
		return;

	g3_rotate_point(&pt, &Objects[objnum].pos);

	// g3_project_point() maps view x by half the canvas width and view y by
	// half its height, so the vertical component has to be scaled by the
	// aspect ratio for the arrow to point where the target actually is on a
	// wide screen.
	dx = pt.p3_x;
	dy = (cx > 0) ? fixmuldiv(pt.p3_y, i2f(cy), i2f(cx)) : pt.p3_y;

	// In view, and already inside the arrow's own radius -- you're looking
	// right at it, so don't clutter the crosshair.
	if (pt.p3_codes == 0 && pt.p3_z > 0)
	{
		g3_project_point(&pt);
		if (!(pt.p3_flags & PF_OVERFLOW))
		{
			int sx = f2i(pt.p3_sx) - cx;
			int sy = f2i(pt.p3_sy) - cy;

			if (sx * sx + sy * sy < SURVIVAL_ARROW_RADIUS * SURVIVAL_ARROW_RADIUS)
				return;
		}
	}

	// Dead ahead or dead behind: no meaningful direction to point.
	if (labs((long)dx) < F1_0 / 16 && labs((long)dy) < F1_0 / 16)
		return;

	// Screen y grows downward, view-space y grows upward.
	ang = fix_atan2(dx, dy);
	fix_sincos(ang, &s, &c);

	ux = f2i(fixmul(i2f(SURVIVAL_ARROW_RADIUS), c));
	uy = -f2i(fixmul(i2f(SURVIVAL_ARROW_RADIUS), s));

	// Unit-ish perpendicular, scaled to the arrowhead's half width.
	px = f2i(fixmul(i2f(SURVIVAL_ARROW_HALF_WIDTH), s));
	py = f2i(fixmul(i2f(SURVIVAL_ARROW_HALF_WIDTH), c));

	tipx = cx + ux + f2i(fixmul(i2f(SURVIVAL_ARROW_LENGTH), c));
	tipy = cy + uy - f2i(fixmul(i2f(SURVIVAL_ARROW_LENGTH), s));

	b1x = cx + ux + px;
	b1y = cy + uy + py;
	b2x = cx + ux - px;
	b2y = cy + uy - py;

	// Brightness by range: darkest at SURVIVAL_ARROW_NEAR_DIST or closer,
	// full reticle color at SURVIVAL_ARROW_FAR_DIST or beyond.
	if (dist <= SURVIVAL_ARROW_NEAR_DIST)
		range = 0;
	else if (dist >= SURVIVAL_ARROW_FAR_DIST)
		range = F1_0;
	else
		range = fixdiv(dist - SURVIVAL_ARROW_NEAR_DIST,
		               SURVIVAL_ARROW_FAR_DIST - SURVIVAL_ARROW_NEAR_DIST);

	bright = SURVIVAL_ARROW_DARKEST + fixmul(F1_0 - SURVIVAL_ARROW_DARKEST, range);

	// Slow pulse so the arrow reads as a live indicator rather than a static
	// piece of the cockpit. It only dips the brightness, so the range
	// signal above still comes through.
	if (GameTime64 & 0x4000)
		bright = fixmul(bright, F1_0 * 4 / 5);

	survival_reticle_rgb(&cr, &cg, &cb);

	// Darker same-hue accent underneath, offset a pixel, for the same reason
	// the score popups have one: legibility over bright level geometry.
	gr_setcolor(survival_shade(cr, cg, cb, fixmul(bright, SURVIVAL_ACCENT_SCALE)));
	gr_line(i2f(tipx + 1), i2f(tipy + 1), i2f(b1x + 1), i2f(b1y + 1));
	gr_line(i2f(tipx + 1), i2f(tipy + 1), i2f(b2x + 1), i2f(b2y + 1));
	gr_line(i2f(b1x + 1), i2f(b1y + 1), i2f(b2x + 1), i2f(b2y + 1));

	gr_setcolor(survival_shade(cr, cg, cb, bright));
	gr_line(i2f(tipx), i2f(tipy), i2f(b1x), i2f(b1y));
	gr_line(i2f(tipx), i2f(tipy), i2f(b2x), i2f(b2y));
	gr_line(i2f(b1x), i2f(b1y), i2f(b2x), i2f(b2y));
}

void survival_draw_kill_feedback(void)
{
	int cx, cy;

	if (!(Game_mode & GM_MULTI) || Netgame.gamemode != NETGAME_SURVIVAL)
		return;

	cx = grd_curcanv->cv_bitmap.bm_w / 2;
	cy = grd_curcanv->cv_bitmap.bm_h / 2;

	survival_draw_kill_popups();

	// The arrow is an aid for a player who is actually flying; a spectator
	// has no crosshair to decorate.
	if (survival_is_eliminated(Player_num) || is_observer())
		return;

	survival_draw_robot_arrow(cx, cy);
}
