/*
 * CPU players: ships the host (or the headless bench) flies under a human's
 * rules -- same physics, guns, ammo, pickups and line of sight, no wallhacks.
 * In a netgame the host simulates them and speaks for them on the wire; every
 * other machine sees ordinary remote players.
 *
 * -duelbench <seconds> [-duellevel <n>] [-duelclose 1] [-duelkit 1] runs CPU
 * against CPU with no rendering and a fixed timestep, prints stats and exits.
 */

#ifndef _DUELBOT_H
#define _DUELBOT_H

#include "object.h"

#define DUEL_BOT_PNUM	1

extern int Duel_pending;

void duelbot_new_match(void);
void duelbot_level_start(void);
void duelbot_frame(void);
int duelbot_is_bot(const object *obj);
int duelbot_no_lan(void);
int duelbot_take_damage(object *obj, object *killer, fix damage);
int duelbot_pickup(object *obj, object *powerup);
void duelbot_weapon_hit(const object *attacker, const object *victim, fix damage, const object *weapon);
void duelbot_note_kill(int killer, int killed);
void duelbot_blast_hit(const object *source);
void duelbot_hit_wall(const object *ship, fix speed, int segnum, int side);
void duelbot_hear_object(int objnum, fix reach);
void duelbot_hear_pos(int segnum, const vms_vector *pos, fix reach);

void duelbot_claim_net_slots(void);
int duelbot_drop_one(void);
void duelbot_release_slot(int pnum);

int duelbot_piloting(const object *obj);

int duelbot_benching(void);
int duelbot_dedicated(void);
int duelbot_headless(void);
int duelbot_bench_frame_time(void);
int duelbot_idle_server(void);

#define DUEL_TIERS	(NDL + 1)	// the game's five, plus Impossible: no human-like flaws
const char *duelbot_tier_name(int skill);
void duelbot_bench_start(void);

#endif
