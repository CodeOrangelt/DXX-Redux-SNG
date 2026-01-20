/*
 * BOTPLAY.C - Bot match system for single-player multiplayer practice
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "botplay.h"
#include "game.h"
#include "object.h"
#include "laser.h"
#include "player.h"
#include "physics.h"
#include "segment.h"
#include "gameseg.h"
#include "wall.h"
#include "fvi.h"
#include "powerup.h"
#include "weapon.h"
#include "multi.h"
#include "newmenu.h"
#include "text.h"
#include "sounds.h"
#include "digi.h"
#include "gameseq.h"
#include "polyobj.h"
#include "robot.h"
#include "vclip.h"
#include "fireball.h"
#include "ai.h"
#include "hudmsg.h"
#include "collide.h"

// Globals
bot_info Bots[MAX_BOTS];
int Num_bots = 0;
int Bot_mode = 0;

// Pending start
int Botplay_pending_start = 0;
int Botplay_pending_num_bots = 3;
int Botplay_pending_skill = 1;

// Bot names
static const char *Bot_names[] = {
    "Maverick", "Viper", "Shadow", "Phoenix",
    "Raptor", "Storm", "Blaze"
};
#define NUM_BOT_NAMES 7

// Constants
#define BOT_RESPAWN_DELAY (F1_0 * 2)
#define BOT_MIN_SPAWN_DIST (F1_0 * 100)

// Forward declarations
static void bot_spawn(int idx);
static void bot_process(int idx);
static void bot_movement(int idx);
static void bot_combat(int idx);
static void bot_collect_powerups(int idx);
static void bot_death_explosion(int pnum);
static int bot_find_spawn_point(int pnum);
static int bot_can_see(int from_pnum, int to_pnum);

// External declarations
extern void drop_player_eggs(object *playerobj);  // from collide.c
extern void collide_player_and_powerup(object *player, object *powerup, vms_vector *collision_point);  // from collide.c

void botplay_init(void)
{
    memset(Bots, 0, sizeof(Bots));
    for (int i = 0; i < MAX_BOTS; i++) {
        Bots[i].active = 0;
        Bots[i].player_num = -1;
        Bots[i].target_pnum = -1;
    }
    Num_bots = 0;
    Bot_mode = 0;
}

void botplay_close(void)
{
    // Remove all bot objects and player slots
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active) {
            int pnum = Bots[i].player_num;
            if (pnum >= 0 && pnum < MAX_PLAYERS) {
                int objnum = Players[pnum].objnum;
                if (objnum >= 0 && objnum <= Highest_object_index) {
                    Objects[objnum].flags |= OF_SHOULD_BE_DEAD;
                }
                Players[pnum].connected = CONNECT_DISCONNECTED;
                Players[pnum].objnum = -1;
            }
        }
    }
    
    // Reset all state completely
    memset(Bots, 0, sizeof(Bots));
    for (int i = 0; i < MAX_BOTS; i++) {
        Bots[i].player_num = -1;
        Bots[i].target_pnum = -1;
    }
    Num_bots = 0;
    Bot_mode = 0;
    Botplay_pending_start = 0;
    Botplay_pending_num_bots = 3;
    Botplay_pending_skill = 1;
    N_players = 1;
}

void botplay_level_init(void)
{
    // Respawn all active bots
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active) {
            Bots[i].awaiting_respawn = 0;
            bot_spawn(i);
        }
    }
}

int botplay_add(int skill)
{
    // Find free bot slot
    int idx = -1;
    for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    
    // Find free player slot (1-7)
    int pnum = -1;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (Players[i].connected == CONNECT_DISCONNECTED) {
            pnum = i;
            break;
        }
    }
    if (pnum < 0) return -1;
    
    // Clamp skill
    if (skill < 0) skill = 0;
    if (skill > 4) skill = 4;
    
    // Init bot
    memset(&Bots[idx], 0, sizeof(bot_info));
    Bots[idx].active = 1;
    Bots[idx].player_num = pnum;
    Bots[idx].skill = skill;
    Bots[idx].target_pnum = -1;
    Bots[idx].awaiting_respawn = 0;
    
    // Each bot gets unique random seed for independent behavior
    Bots[idx].move_seed = (unsigned int)(d_rand() * 65536 + d_rand() + (idx * 12345));
    Bots[idx].strafe_dir = (d_rand() & 1) ? 1 : -1;
    Bots[idx].strafe_change_time = 0;
    Bots[idx].vert_change_time = 0;
    
    // Init player
    Players[pnum].connected = CONNECT_PLAYING;
    snprintf(Players[pnum].callsign, CALLSIGN_LEN, "%s", 
             Bot_names[idx % NUM_BOT_NAMES]);
    Players[pnum].net_kills_total = 0;
    Players[pnum].net_killed_total = 0;
    
    // Spawn
    bot_spawn(idx);
    
    Num_bots++;
    Bot_mode = 1;
    
    return idx;
}

static int bot_find_spawn_point(int pnum)
{
    int best_spawn = -1;
    fix best_dist = 0;
    
    if (NumNetPlayerPositions <= 0) return 0;
    
    // Find spawn point farthest from all players
    for (int s = 0; s < NumNetPlayerPositions; s++) {
        int spawn_seg = Player_init[s].segnum;
        vms_vector *spawn_pos = &Player_init[s].pos;
        
        if (spawn_seg < 0 || spawn_seg >= Num_segments) continue;
        
        fix min_dist = F1_0 * 10000;
        
        // Check distance to all active players
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (i == pnum) continue;
            if (Players[i].connected != CONNECT_PLAYING) continue;
            
            int objnum = Players[i].objnum;
            if (objnum < 0 || objnum > Highest_object_index) continue;
            
            object *obj = &Objects[objnum];
            if (obj->type != OBJ_PLAYER) continue;
            
            fix dist = vm_vec_dist(spawn_pos, &obj->pos);
            if (dist < min_dist) min_dist = dist;
        }
        
        // Use this spawn if it's farther than previous best
        if (min_dist > best_dist) {
            best_dist = min_dist;
            best_spawn = s;
        }
    }
    
    // Require minimum distance
    if (best_dist < BOT_MIN_SPAWN_DIST && best_spawn >= 0) {
        // Use random if all are too close
        best_spawn = d_rand() % NumNetPlayerPositions;
    }
    
    if (best_spawn < 0) best_spawn = 0;
    
    return best_spawn;
}

static void bot_spawn(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    
    if (pnum < 0 || pnum >= MAX_PLAYERS) return;
    
    // Find good spawn point
    int spawn_idx = bot_find_spawn_point(pnum);
    int segnum = Player_init[spawn_idx].segnum;
    vms_vector pos = Player_init[spawn_idx].pos;
    vms_matrix orient = Player_init[spawn_idx].orient;
    
    // Remove old object
    if (Players[pnum].objnum >= 0 && Players[pnum].objnum <= Highest_object_index) {
        object *old = &Objects[Players[pnum].objnum];
        if (old->type != OBJ_NONE) {
            old->flags |= OF_SHOULD_BE_DEAD;
        }
    }
    
    // Create player object
    int objnum = obj_create(OBJ_PLAYER, pnum, segnum, &pos, &orient,
                            Polygon_models[Player_ship->model_num].rad,
                            CT_FLYING, MT_PHYSICS, RT_POLYOBJ);
    
    if (objnum < 0) return;
    
    object *obj = &Objects[objnum];
    
    // Setup as player ship
    obj->id = pnum;
    obj->rtype.pobj_info.model_num = Player_ship->model_num;
    obj->rtype.pobj_info.subobj_flags = 0;
    
    // Physics
    obj->mtype.phys_info.mass = Player_ship->mass;
    obj->mtype.phys_info.drag = Player_ship->drag;
    obj->mtype.phys_info.brakes = Player_ship->brakes;
    obj->mtype.phys_info.flags = PF_TURNROLL | PF_LEVELLING | PF_WIGGLE | PF_USES_THRUST;
    
    vm_vec_zero(&obj->mtype.phys_info.velocity);
    vm_vec_zero(&obj->mtype.phys_info.thrust);
    vm_vec_zero(&obj->mtype.phys_info.rotvel);
    vm_vec_zero(&obj->mtype.phys_info.rotthrust);
    
    obj->shields = INITIAL_SHIELDS;
    
    // Link to player
    Players[pnum].objnum = objnum;
    Players[pnum].connected = CONNECT_PLAYING;
    
    // Init weapons
    Players[pnum].energy = INITIAL_ENERGY;
    Players[pnum].shields = INITIAL_SHIELDS;
    Players[pnum].primary_weapon_flags = HAS_LASER_FLAG;
    Players[pnum].secondary_weapon_flags = HAS_CONCUSSION_FLAG;
    Players[pnum].laser_level = 0;
    Players[pnum].primary_weapon = LASER_INDEX;
    Players[pnum].secondary_weapon = CONCUSSION_INDEX;
    memset(Players[pnum].secondary_ammo, 0, sizeof(Players[pnum].secondary_ammo));
    Players[pnum].secondary_ammo[CONCUSSION_INDEX] = 4;
    Players[pnum].flags = 0;
    Players[pnum].cloak_time = 0;
    Players[pnum].invulnerable_time = 0;
    Players[pnum].homing_object_dist = -F1_0;
    
    // Reset bot state with unique timing offset per bot
    bot->awaiting_respawn = 0;
    bot->target_pnum = -1;
    bot->last_fire = GameTime64 + (idx * F1_0 / 8);  // Stagger fire timing
    bot->last_think = GameTime64 + (idx * F1_0 / 16);  // Stagger think timing
    
    // Reset per-bot movement timers with unique values
    bot->move_seed = (bot->move_seed * 1103515245 + 12345) & 0x7fffffff;
    bot->strafe_change_time = GameTime64 + F1_0/4 + (bot->move_seed % F1_0);
    bot->vert_change_time = GameTime64 + F1_0/3 + ((bot->move_seed >> 8) % F1_0);
    
    // Spawn effect
    object_create_explosion(segnum, &pos, F1_0 * 10, VCLIP_MORPHING_ROBOT);
    digi_link_sound_to_pos(SOUND_PLAYER_GOT_HIT, segnum, 0, &pos, 0, F1_0);
}

void botplay_remove(int idx)
{
    if (idx < 0 || idx >= MAX_BOTS) return;
    if (!Bots[idx].active) return;
    
    int pnum = Bots[idx].player_num;
    
    if (pnum >= 0 && pnum < MAX_PLAYERS) {
        int objnum = Players[pnum].objnum;
        if (objnum >= 0 && objnum <= Highest_object_index) {
            Objects[objnum].flags |= OF_SHOULD_BE_DEAD;
        }
        Players[pnum].connected = CONNECT_DISCONNECTED;
        Players[pnum].objnum = -1;
    }
    
    Bots[idx].active = 0;
    Bots[idx].player_num = -1;
    
    Num_bots--;
    if (Num_bots <= 0) {
        Num_bots = 0;
        Bot_mode = 0;
    }
}

void botplay_remove_all(void)
{
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active) {
            botplay_remove(i);
        }
    }
}

void botplay_frame(void)
{
    // Check pending start FIRST (before Bot_mode check)
    if (Botplay_pending_start) {
        Botplay_pending_start = 0;
        
        // Init bot system
        botplay_init();
        
        // Add bots
        for (int i = 0; i < Botplay_pending_num_bots && i < MAX_BOTS; i++) {
            botplay_add(Botplay_pending_skill);
        }
        
        // Setup multiplayer scoreboard - count all connected players
        int player_count = 0;
        for (int i = 0; i < MAX_PLAYERS; i++) {
            if (Players[i].connected == CONNECT_PLAYING) {
                player_count++;
            }
        }
        N_players = player_count;
        Game_mode |= GM_MULTI;
        
        HUD_init_message(HM_MULTI, "ROBO-ANARCHY STARTED WITH %d BOTS ON %s DIFFICULTY!",
                         Botplay_pending_num_bots,
                         Botplay_pending_skill == 0 ? "EASY" :
                         Botplay_pending_skill == 1 ? "MEDIUM" :
                         Botplay_pending_skill == 2 ? "HARD" : "INSANE");
        
        // Show kill list (scoreboard)
        Show_kill_list = 1;
        Show_kill_list_timer = F1_0 * 5;
        
        return;
    }
    
    if (!Bot_mode || Num_bots <= 0) return;
    
    // Keep N_players updated for scoreboard
    int player_count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (Players[i].connected == CONNECT_PLAYING) {
            player_count++;
        }
    }
    N_players = player_count;
    
    for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active) continue;
        
        // Check for respawn
        if (Bots[i].awaiting_respawn) {
            if (GameTime64 - Bots[i].death_time > BOT_RESPAWN_DELAY) {
                bot_spawn(i);
            }
            continue;
        }
        
        bot_process(i);
    }
}

static int bot_can_see(int from_pnum, int to_pnum)
{
    if (from_pnum < 0 || to_pnum < 0) return 0;
    if (from_pnum >= MAX_PLAYERS || to_pnum >= MAX_PLAYERS) return 0;
    
    int from_obj = Players[from_pnum].objnum;
    int to_obj = Players[to_pnum].objnum;
    
    if (from_obj < 0 || to_obj < 0) return 0;
    if (from_obj > Highest_object_index || to_obj > Highest_object_index) return 0;
    
    object *obj1 = &Objects[from_obj];
    object *obj2 = &Objects[to_obj];
    
    fvi_query fq;
    fvi_info hit;
    
    fq.p0 = &obj1->pos;
    fq.startseg = obj1->segnum;
    fq.p1 = &obj2->pos;
    fq.rad = 0;
    fq.thisobjnum = from_obj;
    fq.ignore_obj_list = NULL;
    fq.flags = FQ_TRANSWALL;
    
    int fate = find_vector_intersection(&fq, &hit);
    
    return (fate == HIT_NONE || (fate == HIT_OBJECT && hit.hit_object == to_obj));
}

static void bot_process(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    
    if (pnum < 0 || Players[pnum].connected != CONNECT_PLAYING) return;
    
    int objnum = Players[pnum].objnum;
    if (objnum < 0 || objnum > Highest_object_index) return;
    
    object *obj = &Objects[objnum];
    if (obj->type != OBJ_PLAYER) return;
    
    // Check death
    if (Players[pnum].shields <= 0 || obj->shields <= 0) {
        return; // Will be handled by collision system
    }
    
    // Think occasionally
    fix think_interval = F1_0 / 4; // 4 Hz
    if (GameTime64 - bot->last_think > think_interval) {
        bot->last_think = GameTime64;
        
        // Find target - ALWAYS prioritize human player!
        int best = -1;
        fix best_dist = F1_0 * 5000;
        
        // First: Always check human player - target them if visible!
        if (Players[Player_num].connected == CONNECT_PLAYING && 
            Players[Player_num].shields > 0) {
            int human_obj = Players[Player_num].objnum;
            if (human_obj >= 0 && human_obj <= Highest_object_index) {
                object *human = &Objects[human_obj];
                if (human->type == OBJ_PLAYER && bot_can_see(pnum, Player_num)) {
                    best = Player_num;
                    best_dist = vm_vec_dist(&obj->pos, &human->pos);
                }
            }
        }
        
        // Only look for other bots if can't see human player
        if (best < 0) {
            for (int i = 0; i < MAX_PLAYERS; i++) {
                if (i == pnum || i == Player_num) continue;
                if (Players[i].connected != CONNECT_PLAYING) continue;
                
                int target_obj = Players[i].objnum;
                if (target_obj < 0 || target_obj > Highest_object_index) continue;
                
                object *tgt = &Objects[target_obj];
                if (tgt->type != OBJ_PLAYER) continue;
                if (Players[i].shields <= 0) continue;
                
                fix dist = vm_vec_dist(&obj->pos, &tgt->pos);
                
                if (bot_can_see(pnum, i) && dist < best_dist) {
                    best_dist = dist;
                    best = i;
                }
            }
        }
        
        bot->target_pnum = best;
    }
    
    bot_movement(idx);
    bot_combat(idx);
    bot_collect_powerups(idx);
}

static void bot_movement(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    object *obj = &Objects[objnum];
    
    fix turn_rate = F1_0 / 2;  // Faster turning
    fix thrust = Player_ship->max_thrust;  // Full thrust
    
    // Advance bot's random seed (Linear Congruential Generator)
    bot->move_seed = (bot->move_seed * 1103515245 + 12345) & 0x7fffffff;
    
    // Check if low on shields - retreat!
    fix shield_percent = fixdiv(Players[pnum].shields, INITIAL_SHIELDS);
    int low_shields = (shield_percent < F1_0 / 4);  // Less than 25%
    
    if (bot->target_pnum < 0) {
        // Wander with per-bot random direction
        if ((bot->move_seed & 0x1F) < 2) {  // ~6% chance per frame
            vms_vector rand_vec;
            rand_vec.x = ((bot->move_seed >> 0) & 0xFFFF) - 0x8000;
            rand_vec.y = ((bot->move_seed >> 8) & 0xFFFF) - 0x8000;
            rand_vec.z = ((bot->move_seed >> 16) & 0xFFFF) - 0x8000;
            vm_vec_normalize(&rand_vec);
            vm_vec_scale(&rand_vec, thrust / 3);
            vm_vec_add2(&obj->mtype.phys_info.thrust, &rand_vec);
        }
        return;
    }
    
    int target_obj = Players[bot->target_pnum].objnum;
    if (target_obj < 0 || target_obj > Highest_object_index) return;
    
    object *tgt = &Objects[target_obj];
    
    vms_vector to_tgt;
    vm_vec_sub(&to_tgt, &tgt->pos, &obj->pos);
    fix dist = vm_vec_normalize(&to_tgt);
    
    // Update strafe direction using per-bot timer
    if (GameTime64 > bot->strafe_change_time) {
        bot->strafe_dir = -bot->strafe_dir;  // Flip direction
        // Next change in 0.3-0.8 seconds based on bot's seed
        bot->strafe_change_time = GameTime64 + F1_0/3 + (bot->move_seed % (F1_0/2));
        bot->move_seed = (bot->move_seed * 1103515245 + 12345) & 0x7fffffff;
    }
    
    // Update vertical direction using per-bot timer (different phase)
    int vert_dir = 1;
    if (GameTime64 > bot->vert_change_time) {
        bot->vert_change_time = GameTime64 + F1_0/4 + (bot->move_seed % (F1_0/3));
        bot->move_seed = (bot->move_seed * 1103515245 + 12345) & 0x7fffffff;
    }
    vert_dir = ((bot->move_seed >> 8) & 1) ? 1 : -1;
    
    if (low_shields) {
        // RETREAT: turn away and flee
        vms_vector away = to_tgt;
        vm_vec_negate(&away);
        ai_turn_towards_vector(&away, obj, turn_rate);
        
        // Trichord retreat with per-bot directions
        vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.fvec, thrust);
        vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.rvec, thrust/2 * bot->strafe_dir);
        vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.uvec, thrust/2 * vert_dir);
    } else {
        // COMBAT: turn toward target
        ai_turn_towards_vector(&to_tgt, obj, turn_rate);
        
        fix ideal_dist = F1_0 * 60;
        
        // Forward/backward based on distance
        if (dist > ideal_dist + F1_0 * 30) {
            vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.fvec, thrust);
        } else if (dist < ideal_dist - F1_0 * 15) {
            vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.fvec, -thrust * 2/3);
        } else {
            vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.fvec, thrust / 4);
        }
        
        // Trichord strafing with per-bot directions
        vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.rvec, thrust * 3/4 * bot->strafe_dir);
        vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.uvec, thrust * 2/3 * vert_dir);
    }
    
    // Limit velocity
    fix speed = vm_vec_mag(&obj->mtype.phys_info.velocity);
    fix max_speed = Player_ship->max_thrust * 3;
    if (speed > max_speed) {
        vm_vec_scale(&obj->mtype.phys_info.velocity, fixdiv(max_speed, speed));
    }
}

static void bot_combat(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    object *obj = &Objects[objnum];
    
    if (bot->target_pnum < 0) return;
    
    // Don't fire if retreating (low shields)
    fix shield_percent = fixdiv(Players[pnum].shields, INITIAL_SHIELDS);
    if (shield_percent < F1_0 / 3) return;  // Retreat mode
    
    int target_obj = Players[bot->target_pnum].objnum;
    if (target_obj < 0 || target_obj > Highest_object_index) return;
    
    object *tgt = &Objects[target_obj];
    
    if (!bot_can_see(pnum, bot->target_pnum)) return;
    
    // Check aim
    vms_vector to_tgt;
    vm_vec_sub(&to_tgt, &tgt->pos, &obj->pos);
    vm_vec_normalize(&to_tgt);
    
    fix dot = vm_vec_dot(&obj->orient.fvec, &to_tgt);
    fix req_dot = F1_0 * 7 / 8;
    
    if (dot < req_dot) return;
    
    // Fire rate
    fix fire_delay = F1_0 / 3;
    if (GameTime64 - bot->last_fire < fire_delay) return;
    
    // Intelligent weapon selection (best available weapon)
    int weapon = LASER_INDEX;  // Default to laser
    
    // Priority: Fusion > Plasma > Spreadfire > Vulcan > Laser
    if (Players[pnum].primary_weapon_flags & HAS_FUSION_FLAG) {
        weapon = FUSION_INDEX;
    } else if (Players[pnum].primary_weapon_flags & HAS_PLASMA_FLAG) {
        weapon = PLASMA_INDEX;
    } else if (Players[pnum].primary_weapon_flags & HAS_SPREADFIRE_FLAG) {
        weapon = SPREADFIRE_INDEX;
    } else if (Players[pnum].primary_weapon_flags & HAS_VULCAN_FLAG) {
        weapon = VULCAN_INDEX;
    } else {
        weapon = LASER_INDEX;
    }
    
    int wtype = Primary_weapon_to_weapon_info[weapon];
    
    // Check if we have enough energy
    if (Players[pnum].energy >= Weapon_info[wtype].energy_usage) {
        Laser_create_new(&obj->orient.fvec, &obj->pos, obj->segnum, objnum, wtype, 1);
        Players[pnum].energy -= Weapon_info[wtype].energy_usage;
        bot->last_fire = GameTime64;
    }
}

static void bot_collect_powerups(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0 || objnum > Highest_object_index) return;
    
    object *bot_obj = &Objects[objnum];
    
    // Scan for nearby powerups
    for (int i = 0; i <= Highest_object_index; i++) {
        object *obj = &Objects[i];
        
        if (obj->type != OBJ_POWERUP) continue;
        if (obj->flags & OF_SHOULD_BE_DEAD) continue;
        
        fix dist = vm_vec_dist(&bot_obj->pos, &obj->pos);
        fix pickup_range = (bot_obj->size + obj->size) * 2;  // Generous range
        
        if (dist < pickup_range) {
            int used = 0;
            int pid = obj->id;
            
            // Handle each powerup type for this bot's player slot
            switch (pid) {
                case POW_ENERGY:
                    if (Players[pnum].energy < MAX_ENERGY) {
                        Players[pnum].energy += 3 * F1_0;
                        if (Players[pnum].energy > MAX_ENERGY)
                            Players[pnum].energy = MAX_ENERGY;
                        used = 1;
                    }
                    break;
                case POW_SHIELD_BOOST:
                    if (Players[pnum].shields < MAX_SHIELDS) {
                        Players[pnum].shields += 3 * F1_0;
                        if (Players[pnum].shields > MAX_SHIELDS)
                            Players[pnum].shields = MAX_SHIELDS;
                        Objects[objnum].shields = Players[pnum].shields;
                        used = 1;
                    }
                    break;
                case POW_LASER:
                    if (Players[pnum].laser_level < MAX_LASER_LEVEL) {
                        Players[pnum].laser_level++;
                        used = 1;
                    }
                    break;
                case POW_QUAD_FIRE:
                    if (!(Players[pnum].flags & PLAYER_FLAGS_QUAD_LASERS)) {
                        Players[pnum].flags |= PLAYER_FLAGS_QUAD_LASERS;
                        used = 1;
                    }
                    break;
                case POW_VULCAN_WEAPON:
                    if (!(Players[pnum].primary_weapon_flags & HAS_VULCAN_FLAG)) {
                        Players[pnum].primary_weapon_flags |= HAS_VULCAN_FLAG;
                        Players[pnum].primary_ammo[VULCAN_INDEX] = VULCAN_WEAPON_AMMO_AMOUNT;
                        used = 1;
                    }
                    break;
                case POW_SPREADFIRE_WEAPON:
                    if (!(Players[pnum].primary_weapon_flags & HAS_SPREADFIRE_FLAG)) {
                        Players[pnum].primary_weapon_flags |= HAS_SPREADFIRE_FLAG;
                        used = 1;
                    }
                    break;
                case POW_PLASMA_WEAPON:
                    if (!(Players[pnum].primary_weapon_flags & HAS_PLASMA_FLAG)) {
                        Players[pnum].primary_weapon_flags |= HAS_PLASMA_FLAG;
                        used = 1;
                    }
                    break;
                case POW_FUSION_WEAPON:
                    if (!(Players[pnum].primary_weapon_flags & HAS_FUSION_FLAG)) {
                        Players[pnum].primary_weapon_flags |= HAS_FUSION_FLAG;
                        used = 1;
                    }
                    break;
                case POW_MISSILE_1:
                    if (Players[pnum].secondary_ammo[CONCUSSION_INDEX] < 20) {
                        Players[pnum].secondary_ammo[CONCUSSION_INDEX]++;
                        used = 1;
                    }
                    break;
                case POW_MISSILE_4:
                    if (Players[pnum].secondary_ammo[CONCUSSION_INDEX] < 20) {
                        Players[pnum].secondary_ammo[CONCUSSION_INDEX] += 4;
                        if (Players[pnum].secondary_ammo[CONCUSSION_INDEX] > 20)
                            Players[pnum].secondary_ammo[CONCUSSION_INDEX] = 20;
                        used = 1;
                    }
                    break;
                case POW_HOMING_AMMO_1:
                    if (Players[pnum].secondary_ammo[HOMING_INDEX] < 10) {
                        Players[pnum].secondary_ammo[HOMING_INDEX]++;
                        Players[pnum].secondary_weapon_flags |= HAS_HOMING_FLAG;
                        used = 1;
                    }
                    break;
                case POW_HOMING_AMMO_4:
                    if (Players[pnum].secondary_ammo[HOMING_INDEX] < 10) {
                        Players[pnum].secondary_ammo[HOMING_INDEX] += 4;
                        if (Players[pnum].secondary_ammo[HOMING_INDEX] > 10)
                            Players[pnum].secondary_ammo[HOMING_INDEX] = 10;
                        Players[pnum].secondary_weapon_flags |= HAS_HOMING_FLAG;
                        used = 1;
                    }
                    break;
                case POW_VULCAN_AMMO:
                    if (Players[pnum].primary_ammo[VULCAN_INDEX] < VULCAN_AMMO_MAX) {
                        Players[pnum].primary_ammo[VULCAN_INDEX] += VULCAN_AMMO_AMOUNT;
                        if (Players[pnum].primary_ammo[VULCAN_INDEX] > VULCAN_AMMO_MAX)
                            Players[pnum].primary_ammo[VULCAN_INDEX] = VULCAN_AMMO_MAX;
                        used = 1;
                    }
                    break;
                case POW_CLOAK:
                    Players[pnum].flags |= PLAYER_FLAGS_CLOAKED;
                    Players[pnum].cloak_time = GameTime64;
                    used = 1;
                    break;
                case POW_INVULNERABILITY:
                    Players[pnum].flags |= PLAYER_FLAGS_INVULNERABLE;
                    Players[pnum].invulnerable_time = GameTime64;
                    used = 1;
                    break;
                default:
                    used = 1;  // Pick up anything else
                    break;
            }
            
            if (used) {
                obj->flags |= OF_SHOULD_BE_DEAD;
                digi_link_sound_to_pos(Powerup_info[pid].hit_sound, obj->segnum, 0, &obj->pos, 0, F1_0);
            }
        }
    }
}

static void bot_death_explosion(int pnum)
{
    if (pnum < 0 || pnum >= MAX_PLAYERS) return;
    
    int objnum = Players[pnum].objnum;
    if (objnum < 0 || objnum > Highest_object_index) return;
    
    object *obj = &Objects[objnum];
    
    // Create player death explosion
    object_create_explosion(obj->segnum, &obj->pos, F1_0 * 10, VCLIP_PLAYER_HIT);
    digi_link_sound_to_pos(SOUND_PLAYER_GOT_HIT, obj->segnum, 0, &obj->pos, 0, F1_0);
    
    // Drop powerup eggs (shields, energy, weapons) just like human players
    drop_player_eggs(obj);
}

void botplay_on_player_death(int victim_pnum, int killer_pnum)
{
    int victim_idx = botplay_get_bot_idx(victim_pnum);
    
    if (victim_idx >= 0) {
        // Bot died
        bot_info *bot = &Bots[victim_idx];
        bot->deaths++;
        bot->awaiting_respawn = 1;
        bot->death_time = GameTime64;
        bot->target_pnum = -1;
        
        Players[victim_pnum].net_killed_total++;
        
        // Create proper death explosion with powerup drops
        bot_death_explosion(victim_pnum);
        
        // Mark object as dead
        int objnum = Players[victim_pnum].objnum;
        if (objnum >= 0 && objnum <= Highest_object_index) {
            Objects[objnum].type = OBJ_GHOST;
            Objects[objnum].render_type = RT_NONE;
        }
    }
    
    if (killer_pnum >= 0 && killer_pnum != victim_pnum) {
        int killer_idx = botplay_get_bot_idx(killer_pnum);
        if (killer_idx >= 0) {
            Bots[killer_idx].kills++;
        }
        
        Players[killer_pnum].net_kills_total++;
        Players[killer_pnum].KillGoalCount++;
    }
}

int botplay_is_bot(int pnum)
{
    return (botplay_get_bot_idx(pnum) >= 0);
}

int botplay_get_bot_idx(int pnum)
{
    if (!Bot_mode) return -1;
    if (pnum < 0 || pnum >= MAX_PLAYERS) return -1;
    
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active && Bots[i].player_num == pnum) {
            return i;
        }
    }
    return -1;
}

void botplay_apply_damage(int player_objnum, int killer_objnum, fix damage)
{
    if (!Bot_mode) return;
    if (player_objnum < 0 || player_objnum > Highest_object_index) return;
    
    object *player_obj = &Objects[player_objnum];
    if (player_obj->type != OBJ_PLAYER) return;
    if (player_obj->id < 0 || player_obj->id >= MAX_PLAYERS) return;
    
    int pnum = player_obj->id;
    int bot_idx = botplay_get_bot_idx(pnum);
    
    if (bot_idx < 0) return;
    
    // Play hit sound and create hit effect
    digi_link_sound_to_pos(SOUND_PLAYER_GOT_HIT, player_obj->segnum, 0, &player_obj->pos, 0, F1_0);
    object_create_explosion(player_obj->segnum, &player_obj->pos, F1_0 * 5, VCLIP_PLAYER_HIT);
    
    // Apply damage
    Players[pnum].shields -= damage;
    player_obj->shields = Players[pnum].shields;
    
    // Check for death
    if (Players[pnum].shields <= 0) {
        int killer_pnum = -1;
        
        if (killer_objnum >= 0 && killer_objnum <= Highest_object_index) {
            object *killer_obj = &Objects[killer_objnum];
            if (killer_obj->type == OBJ_PLAYER && killer_obj->id >= 0 && killer_obj->id < MAX_PLAYERS) {
                killer_pnum = killer_obj->id;
            } else if (killer_obj->type == OBJ_WEAPON) {
                // Get parent of weapon
                int parent = killer_obj->ctype.laser_info.parent_num;
                if (parent >= 0 && parent <= Highest_object_index) {
                    object *parent_obj = &Objects[parent];
                    if (parent_obj->type == OBJ_PLAYER) {
                        killer_pnum = parent_obj->id;
                    }
                }
            }
        }
        
        botplay_on_player_death(pnum, killer_pnum);
    }
}
