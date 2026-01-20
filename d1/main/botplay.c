/*
 * BOTPLAY.C - Enhanced Bot System v2.0
 * Improved pathfinding, combat, and human-like behavior
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "botplay.h"
#include "inferno.h"
#include "game.h"
#include "object.h"
#include "laser.h"
#include "player.h"
#include "physics.h"
#include "segment.h"
#include "gameseg.h"
#include "fvi.h"
#include "fireball.h"
#include "powerup.h"
#include "weapon.h"
#include "multi.h"
#include "newmenu.h"
#include "text.h"
#include "sounds.h"
#include "digi.h"
#include "polyobj.h"
#include "robot.h"
#include "vclip.h"
#include "gameseq.h"
#include "wall.h"
#include "collide.h"
#include "ai.h"
#include "hudmsg.h"

/* ========== GLOBALS ========== */

bot_info Bots[MAX_BOTS];
int Num_bots = 0;
int Bot_mode = 0;
static fix64 Bot_last_frame_time = 0;

/* Pending start variables */
int Botplay_pending_start = 0;
int Botplay_pending_num_bots = 3;
int Botplay_pending_skill = 1;

static const char *Bot_names[MAX_BOTS] = {
    "Reaper", "Havoc", "Venom", "Spectre",
    "Phantom", "Striker", "Nova"
};

/* Constants */
#define BOT_RESPAWN_DELAY       (F1_0 * 3)      // 3 seconds
#define BOT_THINK_INTERVAL      3               // frames between thinks
#define BOT_PATH_RETRY_TIME     (F1_0 * 2)      // retry path every 2 sec
#define BOT_STUCK_CHECK_TIME    (F1_0)          // check stuck every 1 sec
#define BOT_STUCK_THRESHOLD     (F1_0 * 3)      // less than 3 units = stuck
#define BOT_COMBAT_RANGE_MIN    (F1_0 * 30)
#define BOT_COMBAT_RANGE_MAX    (F1_0 * 80)
#define BOT_FLEE_HEALTH_PCT     25
#define BOT_STRAFE_CHANGE_TIME  (F1_0 / 2)      // change strafe dir every 0.5s

/* External declarations */
extern void drop_player_eggs(object *playerobj);

/* ========== RANDOM ========== */

static unsigned int bot_rand(int idx)
{
    Bots[idx].rand_state = Bots[idx].rand_state * 1103515245 + 12345;
    return (Bots[idx].rand_state >> 16) & 0x7FFF;
}

static int bot_rand_range(int idx, int min, int max)
{
    if (max <= min) return min;
    return min + (bot_rand(idx) % (max - min + 1));
}

/* ========== VISIBILITY ========== */

static int bot_can_see_point(object *obj, vms_vector *target_pos)
{
    fvi_query fq;
    fvi_info hit;
    
    fq.p0 = &obj->pos;
    fq.startseg = obj->segnum;
    fq.p1 = target_pos;
    fq.rad = 0;
    fq.thisobjnum = obj - Objects;
    fq.ignore_obj_list = NULL;
    fq.flags = FQ_TRANSWALL | FQ_CHECK_OBJS;
    
    int fate = find_vector_intersection(&fq, &hit);
    return (fate == HIT_NONE);
}

static int bot_can_see_object(int from_objnum, int to_objnum)
{
    if (from_objnum < 0 || to_objnum < 0) return 0;
    if (from_objnum > Highest_object_index || to_objnum > Highest_object_index) return 0;
    
    object *obj1 = &Objects[from_objnum];
    object *obj2 = &Objects[to_objnum];
    
    if (obj1->type == OBJ_NONE || obj2->type == OBJ_NONE) return 0;
    
    return bot_can_see_point(obj1, &obj2->pos);
}

/* ========== PATHFINDING - Using Engine's AI System ========== */

// Create a path to a segment using the engine's pathfinding
static int bot_create_path_to_segment(int idx, int goal_seg)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0 || objnum > Highest_object_index) return 0;
    if (goal_seg < 0 || goal_seg > Highest_segment_index) return 0;
    
    object *obj = &Objects[objnum];
    
    // Use a temporary point_seg array
    static point_seg temp_path[MAX_PATH_LENGTH];
    short num_points = 0;
    
    int result = create_path_points(obj, obj->segnum, goal_seg, temp_path, &num_points, 
                                    MAX_PATH_LENGTH, 1, 1, -1);
    
    if (result == -1 || num_points < 2) {
        bot->using_ai_path = 0;
        return 0;
    }
    
    // Store the goal
    bot->path_target_seg = goal_seg;
    bot->using_ai_path = 1;
    bot->goal_seg = goal_seg;
    bot->goal_valid = 1;
    
    // Get the next waypoint (skip current position)
    int wp_idx = (num_points > 1) ? 1 : 0;
    bot->goal_pos = temp_path[wp_idx].point;
    
    bot->last_path_time = GameTime64;
    
    return 1;
}

// Create path to a specific player
static int bot_create_path_to_player(int idx, int target_pnum)
{
    if (target_pnum < 0 || target_pnum >= MAX_PLAYERS) return 0;
    if (Players[target_pnum].connected != CONNECT_PLAYING) return 0;
    
    int target_objnum = Players[target_pnum].objnum;
    if (target_objnum < 0 || target_objnum > Highest_object_index) return 0;
    
    object *target = &Objects[target_objnum];
    return bot_create_path_to_segment(idx, target->segnum);
}

// Update path - get next waypoint if we reached current one
static void bot_update_path(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return;
    object *obj = &Objects[objnum];
    
    if (!bot->goal_valid) return;
    
    // Check if we reached the current waypoint
    fix dist = vm_vec_dist_quick(&obj->pos, &bot->goal_pos);
    
    if (dist < F1_0 * 5) {
        // Reached waypoint - are we at goal?
        if (obj->segnum == bot->goal_seg || obj->segnum == bot->path_target_seg) {
            bot->goal_valid = 0;
            bot->using_ai_path = 0;
        } else {
            // Need to continue path - recreate to goal
            if (GameTime64 - bot->last_path_time > F1_0/4) {
                bot_create_path_to_segment(idx, bot->path_target_seg);
            }
        }
    }
    
    // Check if stuck
    if (GameTime64 - bot->stuck_check_time > BOT_STUCK_CHECK_TIME) {
        fix moved = vm_vec_dist_quick(&obj->pos, &bot->last_stuck_pos);
        
        if (moved < BOT_STUCK_THRESHOLD) {
            bot->stuck_count++;
            if (bot->stuck_count > 3) {
                // Very stuck - pick random direction
                bot->goal_valid = 0;
                bot->using_ai_path = 0;
                bot->stuck_count = 0;
                
                // Try to find a random connected segment
                segment *seg = &Segments[obj->segnum];
                for (int i = 0; i < MAX_SIDES_PER_SEGMENT; i++) {
                    int child = seg->children[i];
                    if (child >= 0 && child <= Highest_segment_index) {
                        compute_segment_center(&bot->goal_pos, &Segments[child]);
                        bot->goal_valid = 1;
                        bot->goal_seg = child;
                        break;
                    }
                }
            }
        } else {
            bot->stuck_count = 0;
        }
        
        bot->last_stuck_pos = obj->pos;
        bot->stuck_check_time = GameTime64;
    }
}

/* ========== TARGET SELECTION ========== */

static int bot_find_best_target(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return -1;
    object *obj = &Objects[objnum];
    
    int best_target = -1;
    fix best_score = 0;
    
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (i == pnum) continue;
        if (Players[i].connected != CONNECT_PLAYING) continue;
        
        int target_objnum = Players[i].objnum;
        if (target_objnum < 0 || target_objnum > Highest_object_index) continue;
        
        object *target = &Objects[target_objnum];
        if (target->type != OBJ_PLAYER) continue;
        if (target->flags & OF_SHOULD_BE_DEAD) continue;
        if (Players[i].shields <= 0) continue;
        
        fix dist = vm_vec_dist_quick(&obj->pos, &target->pos);
        
        // Base score - prefer closer targets
        fix score = F1_0 * 500 - dist/2;
        if (score < 0) score = F1_0;
        
        // Big bonus for visible targets
        int can_see = bot_can_see_object(objnum, target_objnum);
        if (can_see) {
            score += F1_0 * 300;
        }
        
        // Bonus for current target (consistency)
        if (i == bot->target_pnum) {
            score += F1_0 * 200;
        }
        
        // Bonus for weakened targets
        if (Players[i].shields < 40 * F1_0) {
            score += F1_0 * 150;
        }
        
        if (score > best_score) {
            best_score = score;
            best_target = i;
        }
    }
    
    return best_target;
}

static int bot_find_powerup(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return -1;
    object *obj = &Objects[objnum];
    
    int health_pct = (Players[pnum].shields * 100) / (100 * F1_0);
    int energy_pct = (Players[pnum].energy * 100) / (100 * F1_0);
    
    int best_powerup = -1;
    fix best_score = 0;
    
    for (int i = 0; i <= Highest_object_index; i++) {
        object *pw = &Objects[i];
        if (pw->type != OBJ_POWERUP) continue;
        if (pw->flags & OF_SHOULD_BE_DEAD) continue;
        
        fix dist = vm_vec_dist_quick(&obj->pos, &pw->pos);
        if (dist > F1_0 * 200) continue;
        
        int priority = 0;
        
        switch (pw->id) {
            case POW_SHIELD_BOOST:
                priority = (100 - health_pct) * 5;
                break;
            case POW_ENERGY:
                priority = (100 - energy_pct) * 3;
                break;
            case POW_CLOAK:
            case POW_INVULNERABILITY:
                priority = 400;
                break;
            case POW_QUAD_FIRE:
                if (!(Players[pnum].flags & PLAYER_FLAGS_QUAD_LASERS))
                    priority = 350;
                break;
            case POW_LASER:
                if (Players[pnum].laser_level < MAX_LASER_LEVEL)
                    priority = 300;
                break;
            case POW_VULCAN_WEAPON:
            case POW_SPREADFIRE_WEAPON:
            case POW_PLASMA_WEAPON:
            case POW_FUSION_WEAPON:
                priority = 250;
                break;
            default:
                priority = 100;
                break;
        }
        
        if (priority > 0) {
            fix score = (priority * F1_0) - dist/4;
            if (score > best_score) {
                best_score = score;
                best_powerup = i;
            }
        }
    }
    
    return best_powerup;
}

/* ========== STATE MACHINE ========== */

static void bot_change_state(int idx, bot_state_t new_state)
{
    bot_info *bot = &Bots[idx];
    
    if (bot->state == new_state) return;
    
    bot->prev_state = bot->state;
    bot->state = new_state;
    bot->state_timer = 0;
    bot->goal_valid = 0;
    
    switch (new_state) {
        case BOT_STATE_COMBAT:
            bot->state_duration = 90 + bot->aggression;
            bot->combat_strafe_dir = (bot_rand(idx) & 1) ? 1 : -1;
            bot->last_strafe_change = GameTime64;
            break;
        case BOT_STATE_FLEE:
            bot->state_duration = 60 + bot_rand_range(idx, 0, 60);
            break;
        case BOT_STATE_HUNT:
            bot->state_duration = 120 + bot_rand_range(idx, 0, 60);
            break;
        case BOT_STATE_GET_POWERUP:
            bot->state_duration = 150;
            break;
        case BOT_STATE_ROAM:
            bot->state_duration = 90 + bot_rand_range(idx, 0, 90);
            break;
        default:
            bot->state_duration = 60;
            break;
    }
}

static void bot_think(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return;
    object *obj = &Objects[objnum];
    
    bot->state_timer++;
    
    int health_pct = (Players[pnum].shields * 100) / (100 * F1_0);
    
    // Find target
    int target = bot_find_best_target(idx);
    int can_see_target = 0;
    
    if (target >= 0) {
        int target_objnum = Players[target].objnum;
        can_see_target = bot_can_see_object(objnum, target_objnum);
        
        if (can_see_target) {
            bot->target_pnum = target;
            bot->last_target_pos = Objects[target_objnum].pos;
            bot->last_target_seg = Objects[target_objnum].segnum;
            bot->last_saw_target = 1;
        }
    }
    
    // State transitions
    
    // FLEE if low health (unless very aggressive)
    if (health_pct < BOT_FLEE_HEALTH_PCT && bot->aggression < 70) {
        if (bot->state != BOT_STATE_FLEE) {
            bot_change_state(idx, BOT_STATE_FLEE);
            return;
        }
    }
    
    // COMBAT if visible target
    if (target >= 0 && can_see_target && health_pct > 20) {
        if (bot->state != BOT_STATE_COMBAT) {
            bot->target_pnum = target;
            bot_change_state(idx, BOT_STATE_COMBAT);
            return;
        }
    }
    
    // Look for powerups if hurt or state expired
    if (health_pct < 60 || bot->state_timer > bot->state_duration) {
        int pw = bot_find_powerup(idx);
        if (pw >= 0) {
            bot->target_powerup_obj = pw;
            bot_change_state(idx, BOT_STATE_GET_POWERUP);
            return;
        }
    }
    
    // HUNT if we had a target recently
    if (target >= 0 && !can_see_target && bot->last_saw_target) {
        if (bot->state != BOT_STATE_HUNT && bot->state != BOT_STATE_COMBAT) {
            bot_change_state(idx, BOT_STATE_HUNT);
            return;
        }
    }
    
    // ROAM if nothing else
    if (bot->state_timer > bot->state_duration) {
        bot_change_state(idx, BOT_STATE_ROAM);
    }
}

/* ========== MOVEMENT ========== */

static void bot_move(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return;
    object *obj = &Objects[objnum];
    
    // Update any existing path
    bot_update_path(idx);
    
    vms_vector goal_dir;
    int has_goal = 0;
    fix desired_speed = Player_ship->max_thrust;
    
    switch (bot->state) {
        case BOT_STATE_COMBAT: {
            if (bot->target_pnum < 0) break;
            
            int target_objnum = Players[bot->target_pnum].objnum;
            if (target_objnum < 0) break;
            
            object *target = &Objects[target_objnum];
            
            vms_vector to_target;
            vm_vec_sub(&to_target, &target->pos, &obj->pos);
            fix dist = vm_vec_mag_quick(&to_target);
            vm_vec_normalize_quick(&to_target);
            
            // Combat movement: strafe while maintaining range
            fix ideal_range = bot->preferred_range;
            
            // Change strafe direction periodically
            if (GameTime64 - bot->last_strafe_change > BOT_STRAFE_CHANGE_TIME) {
                if (bot_rand(idx) % 3 == 0) {
                    bot->combat_strafe_dir = -bot->combat_strafe_dir;
                }
                bot->last_strafe_change = GameTime64;
            }
            
            // Calculate strafe vector (perpendicular to target direction)
            vms_vector strafe;
            vm_vec_cross(&strafe, &to_target, &obj->orient.uvec);
            vm_vec_normalize_quick(&strafe);
            vm_vec_scale(&strafe, bot->combat_strafe_dir * F1_0);
            
            // Combine approach/retreat with strafe
            if (dist > ideal_range + F1_0 * 20) {
                // Too far - move closer
                vm_vec_scale_add(&goal_dir, &strafe, &to_target, F1_0 * 2);
            } else if (dist < ideal_range - F1_0 * 15) {
                // Too close - back away
                vms_vector away = to_target;
                vm_vec_negate(&away);
                vm_vec_scale_add(&goal_dir, &strafe, &away, F1_0);
            } else {
                // Good range - just strafe
                goal_dir = strafe;
            }
            
            vm_vec_normalize_quick(&goal_dir);
            has_goal = 1;
            desired_speed = (desired_speed * 120) / 100;
            break;
        }
        
        case BOT_STATE_FLEE: {
            if (bot->target_pnum >= 0) {
                int target_objnum = Players[bot->target_pnum].objnum;
                if (target_objnum >= 0) {
                    object *target = &Objects[target_objnum];
                    vm_vec_sub(&goal_dir, &obj->pos, &target->pos);
                    vm_vec_normalize_quick(&goal_dir);
                    has_goal = 1;
                    desired_speed = (desired_speed * 140) / 100;
                }
            }
            
            // Also try to path away
            if (!bot->goal_valid && GameTime64 - bot->last_path_time > BOT_PATH_RETRY_TIME) {
                // Find segment away from threat
                segment *seg = &Segments[obj->segnum];
                int best_child = -1;
                fix best_dist = 0;
                
                for (int i = 0; i < MAX_SIDES_PER_SEGMENT; i++) {
                    int child = seg->children[i];
                    if (child >= 0 && child <= Highest_segment_index) {
                        vms_vector center;
                        compute_segment_center(&center, &Segments[child]);
                        
                        if (bot->target_pnum >= 0) {
                            int to = Players[bot->target_pnum].objnum;
                            if (to >= 0) {
                                fix d = vm_vec_dist_quick(&center, &Objects[to].pos);
                                if (d > best_dist) {
                                    best_dist = d;
                                    best_child = child;
                                }
                            }
                        }
                    }
                }
                
                if (best_child >= 0) {
                    compute_segment_center(&bot->goal_pos, &Segments[best_child]);
                    bot->goal_valid = 1;
                    bot->goal_seg = best_child;
                }
                bot->last_path_time = GameTime64;
            }
            break;
        }
        
        case BOT_STATE_HUNT: {
            // Move toward last known position
            if (bot->last_target_seg >= 0) {
                if (!bot->goal_valid || GameTime64 - bot->last_path_time > BOT_PATH_RETRY_TIME) {
                    bot_create_path_to_segment(idx, bot->last_target_seg);
                }
            }
            break;
        }
        
        case BOT_STATE_GET_POWERUP: {
            if (bot->target_powerup_obj >= 0 && bot->target_powerup_obj <= Highest_object_index) {
                object *pw = &Objects[bot->target_powerup_obj];
                if (pw->type == OBJ_POWERUP && !(pw->flags & OF_SHOULD_BE_DEAD)) {
                    vm_vec_sub(&goal_dir, &pw->pos, &obj->pos);
                    vm_vec_normalize_quick(&goal_dir);
                    has_goal = 1;
                } else {
                    bot->target_powerup_obj = -1;
                    bot_change_state(idx, BOT_STATE_ROAM);
                }
            } else {
                bot_change_state(idx, BOT_STATE_ROAM);
            }
            break;
        }
        
        case BOT_STATE_ROAM:
        default: {
            // Roam randomly
            if (!bot->goal_valid || GameTime64 - bot->last_path_time > BOT_PATH_RETRY_TIME * 2) {
                // Pick a random nearby segment
                segment *seg = &Segments[obj->segnum];
                int attempts = 0;
                int target_seg = obj->segnum;
                
                // Walk a few segments randomly
                for (int step = 0; step < 5 && attempts < 20; step++) {
                    int side = bot_rand(idx) % MAX_SIDES_PER_SEGMENT;
                    int child = Segments[target_seg].children[side];
                    if (child >= 0 && child <= Highest_segment_index) {
                        target_seg = child;
                    }
                    attempts++;
                }
                
                if (target_seg != obj->segnum) {
                    compute_segment_center(&bot->goal_pos, &Segments[target_seg]);
                    bot->goal_valid = 1;
                    bot->goal_seg = target_seg;
                }
                bot->last_path_time = GameTime64;
            }
            break;
        }
    }
    
    // Apply movement
    if (bot->goal_valid && !has_goal) {
        vm_vec_sub(&goal_dir, &bot->goal_pos, &obj->pos);
        vm_vec_normalize_quick(&goal_dir);
        has_goal = 1;
    }
    
    if (has_goal) {
        // Turn toward goal
        fix turn_rate = F1_0/4 + (bot->skill * F1_0/16);
        ai_turn_towards_vector(&goal_dir, obj, turn_rate);
        
        // Move forward if roughly facing goal
        fix dot = vm_vec_dot(&obj->orient.fvec, &goal_dir);
        
        if (dot > F1_0/3) {
            // Scale speed by how well we're aimed
            fix speed = fixmul(desired_speed, dot);
            vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.fvec, speed);
        }
        
        // Add some vertical movement in combat
        if (bot->state == BOT_STATE_COMBAT) {
            int vert_dir = ((GameTime64 / (F1_0/2)) + idx) & 1 ? 1 : -1;
            vm_vec_scale_add2(&obj->mtype.phys_info.thrust, &obj->orient.uvec, 
                              desired_speed / 4 * vert_dir);
        }
    }
}

/* ========== COMBAT ========== */

static void bot_fire(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    int objnum = Players[pnum].objnum;
    
    if (objnum < 0) return;
    object *obj = &Objects[objnum];
    
    // Only fire in combat
    if (bot->state != BOT_STATE_COMBAT) return;
    if (bot->target_pnum < 0) return;
    
    int target_objnum = Players[bot->target_pnum].objnum;
    if (target_objnum < 0 || target_objnum > Highest_object_index) return;
    
    object *target = &Objects[target_objnum];
    if (target->type != OBJ_PLAYER) return;
    if (Players[bot->target_pnum].shields <= 0) return;
    
    // Must see target
    if (!bot_can_see_object(objnum, target_objnum)) return;
    
    // Check timing
    if (GameTime64 < bot->next_fire_time) return;
    
    // Check aim
    vms_vector to_target;
    vm_vec_sub(&to_target, &target->pos, &obj->pos);
    fix dist = vm_vec_mag_quick(&to_target);
    vm_vec_normalize_quick(&to_target);
    
    fix dot = vm_vec_dot(&obj->orient.fvec, &to_target);
    
    // Required accuracy based on skill and personality
    fix min_dot = F1_0 * 80/100 - (bot->accuracy * F1_0/400) - (bot->skill * F1_0/40);
    
    if (dot < min_dot) {
        // Not aimed well - wait a bit
        bot->next_fire_time = GameTime64 + F1_0/8;
        return;
    }
    
    // Select weapon
    int weapon = LASER_INDEX;
    
    if (Players[pnum].energy > F1_0 * 5) {
        if ((Players[pnum].primary_weapon_flags & HAS_PLASMA_FLAG) && dist > F1_0 * 40) {
            weapon = PLASMA_INDEX;
        } else if ((Players[pnum].primary_weapon_flags & HAS_SPREADFIRE_FLAG) && dist < F1_0 * 50) {
            weapon = SPREADFIRE_INDEX;
        }
    }
    
    if ((Players[pnum].primary_weapon_flags & HAS_VULCAN_FLAG) && 
        Players[pnum].primary_ammo[VULCAN_INDEX] > 50) {
        weapon = VULCAN_INDEX;
    }
    
    Players[pnum].primary_weapon = weapon;
    
    // Determine if starting new burst
    if (bot->burst_shots_left <= 0 || GameTime64 > bot->burst_end_time) {
        // Start new burst
        bot->current_burst_weapon = weapon;
        
        if (weapon == VULCAN_INDEX) {
            bot->burst_shots_left = 3 + bot_rand_range(idx, 0, 4);
            bot->burst_end_time = GameTime64 + F1_0;
        } else {
            bot->burst_shots_left = 1 + bot_rand_range(idx, 0, 2);
            bot->burst_end_time = GameTime64 + F1_0/2;
        }
    }
    
    // Get weapon type
    int wtype;
    if (weapon == LASER_INDEX) {
        wtype = LASER_ID_L1 + Players[pnum].laser_level;
        if (wtype > LASER_ID_L4) wtype = LASER_ID_L4;
    } else {
        wtype = Primary_weapon_to_weapon_info[weapon];
    }
    
    // Check resources
    if (weapon == VULCAN_INDEX) {
        if (Players[pnum].primary_ammo[VULCAN_INDEX] < Weapon_info[wtype].ammo_usage)
            return;
    } else {
        if (Players[pnum].energy < Weapon_info[wtype].energy_usage)
            return;
    }
    
    // Fire using bot's forward direction
    Laser_player_fire(obj, wtype, 0, 1, 0, obj->orient.fvec);
    
    bot->burst_shots_left--;
    
    // Consume resources
    if (weapon == VULCAN_INDEX) {
        Players[pnum].primary_ammo[VULCAN_INDEX] -= Weapon_info[wtype].ammo_usage;
    } else {
        Players[pnum].energy -= Weapon_info[wtype].energy_usage;
        if (Players[pnum].energy < 0) Players[pnum].energy = 0;
    }
    
    // Set next fire time
    fix base_delay;
    if (bot->burst_shots_left > 0) {
        // Quick follow-up in burst
        base_delay = F1_0/15 + (4 - bot->skill) * F1_0/60;
    } else {
        // Pause between bursts (more human-like)
        base_delay = F1_0/3 + bot_rand_range(idx, 0, F1_0/4);
        base_delay += (4 - bot->skill) * F1_0/10;
        base_delay += bot->reaction_delay * F1_0/30;
    }
    
    bot->next_fire_time = GameTime64 + base_delay;
}

/* ========== SPAWNING ========== */

static void bot_spawn(int idx)
{
    bot_info *bot = &Bots[idx];
    int pnum = bot->player_num;
    
    // Find spawn point
    int spawn_idx = 0;
    if (NumNetPlayerPositions > 0) {
        spawn_idx = bot_rand_range(idx, 0, NumNetPlayerPositions - 1);
    }
    
    int segnum = Player_init[spawn_idx].segnum;
    vms_vector pos = Player_init[spawn_idx].pos;
    vms_matrix orient = Player_init[spawn_idx].orient;
    
    // Clean up old object
    if (Players[pnum].objnum >= 0 && Players[pnum].objnum <= Highest_object_index) {
        Objects[Players[pnum].objnum].flags |= OF_SHOULD_BE_DEAD;
    }
    
    // Create new player object
    int objnum = obj_create(OBJ_PLAYER, pnum, segnum, &pos, &orient,
                            Polygon_models[Player_ship->model_num].rad,
                            CT_FLYING, MT_PHYSICS, RT_POLYOBJ);
    
    if (objnum < 0) return;
    
    object *obj = &Objects[objnum];
    obj->id = pnum;
    obj->rtype.pobj_info.model_num = Player_ship->model_num;
    obj->rtype.pobj_info.subobj_flags = 0;
    obj->mtype.phys_info.mass = Player_ship->mass;
    obj->mtype.phys_info.drag = Player_ship->drag;
    obj->mtype.phys_info.flags = PF_TURNROLL | PF_LEVELLING | PF_WIGGLE | PF_USES_THRUST;
    obj->shields = INITIAL_SHIELDS;
    
    vm_vec_zero(&obj->mtype.phys_info.velocity);
    vm_vec_zero(&obj->mtype.phys_info.thrust);
    vm_vec_zero(&obj->mtype.phys_info.rotvel);
    vm_vec_zero(&obj->mtype.phys_info.rotthrust);
    
    Players[pnum].objnum = objnum;
    Players[pnum].shields = INITIAL_SHIELDS;
    Players[pnum].energy = INITIAL_ENERGY;
    Players[pnum].primary_weapon_flags = HAS_LASER_FLAG;
    Players[pnum].primary_weapon = LASER_INDEX;
    Players[pnum].secondary_weapon = CONCUSSION_INDEX;
    Players[pnum].secondary_ammo[CONCUSSION_INDEX] = 2 + Difficulty_level;
    Players[pnum].laser_level = 0;
    Players[pnum].flags = 0;
    
    for (int i = 0; i < MAX_PRIMARY_WEAPONS; i++)
        Players[pnum].primary_ammo[i] = 0;
    
    bot->awaiting_respawn = 0;
    bot->state = BOT_STATE_SPAWN;
    bot->state_timer = 0;
    bot->goal_valid = 0;
    bot->target_pnum = -1;
    bot->last_saw_target = 0;
    bot->stuck_count = 0;
    bot->burst_shots_left = 0;
    bot->next_fire_time = GameTime64 + F1_0;
    
    // Spawn effect
    object_create_explosion(segnum, &pos, F1_0 * 10, VCLIP_MORPHING_ROBOT);
    
    // Start roaming
    bot_change_state(idx, BOT_STATE_ROAM);
}

/* ========== INITIALIZATION ========== */

void botplay_init(void)
{
    memset(Bots, 0, sizeof(Bots));
    for (int i = 0; i < MAX_BOTS; i++) {
        Bots[i].active = 0;
        Bots[i].player_num = -1;
        Bots[i].target_pnum = -1;
        Bots[i].rand_state = 12345 + i * 98765;
    }
    Num_bots = 0;
    Bot_mode = 0;
}

void botplay_close(void)
{
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active) {
            int pnum = Bots[i].player_num;
            if (pnum >= 0 && pnum < MAX_PLAYERS) {
                int objnum = Players[pnum].objnum;
                if (objnum >= 0 && objnum <= Highest_object_index) {
                    Objects[objnum].flags |= OF_SHOULD_BE_DEAD;
                }
                Players[pnum].connected = CONNECT_DISCONNECTED;
            }
            Bots[i].active = 0;
        }
    }
    Num_bots = 0;
    Bot_mode = 0;
}

void botplay_level_init(void)
{
    Bot_last_frame_time = GameTime64;
    
    for (int i = 0; i < MAX_BOTS; i++) {
        if (Bots[i].active) {
            Bots[i].awaiting_respawn = 1;
            Bots[i].death_time = GameTime64 - BOT_RESPAWN_DELAY;
            Bots[i].goal_valid = 0;
            Bots[i].stuck_count = 0;
        }
    }
}

/* ========== BOT CREATION ========== */

int botplay_add(int skill)
{
    int idx = -1;
    for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active) {
            idx = i;
            break;
        }
    }
    if (idx < 0) return -1;
    
    int pnum = -1;
    for (int i = 1; i < MAX_PLAYERS; i++) {
        if (Players[i].connected == CONNECT_DISCONNECTED) {
            pnum = i;
            break;
        }
    }
    if (pnum < 0) return -1;
    
    if (skill < 0) skill = 0;
    if (skill > 4) skill = 4;
    
    memset(&Bots[idx], 0, sizeof(bot_info));
    
    Bots[idx].active = 1;
    Bots[idx].player_num = pnum;
    Bots[idx].skill = skill;
    Bots[idx].rand_state = 12345 + idx * 98765 + GameTime64;
    
    // Personality
    Bots[idx].aggression = 30 + bot_rand_range(idx, 0, 40) + skill * 8;
    if (Bots[idx].aggression > 95) Bots[idx].aggression = 95;
    
    Bots[idx].accuracy = 40 + skill * 12 + bot_rand_range(idx, 0, 15);
    if (Bots[idx].accuracy > 95) Bots[idx].accuracy = 95;
    
    Bots[idx].reaction_delay = 15 - skill * 3 + bot_rand_range(idx, 0, 5);
    if (Bots[idx].reaction_delay < 2) Bots[idx].reaction_delay = 2;
    
    Bots[idx].preferred_range = F1_0 * (40 + bot_rand_range(idx, 0, 30));
    
    Bots[idx].target_pnum = -1;
    Bots[idx].target_powerup_obj = -1;
    Bots[idx].state = BOT_STATE_SPAWN;
    Bots[idx].goal_valid = 0;
    Bots[idx].next_fire_time = GameTime64 + F1_0;
    
    // Initialize player
    Players[pnum].connected = CONNECT_PLAYING;
    strncpy(Players[pnum].callsign, Bot_names[idx % MAX_BOTS], CALLSIGN_LEN);
    Players[pnum].net_kills_total = 0;
    Players[pnum].net_killed_total = 0;
    
    // Will spawn on first frame
    Bots[idx].awaiting_respawn = 1;
    Bots[idx].death_time = GameTime64 - BOT_RESPAWN_DELAY;
    
    Num_bots++;
    Bot_mode = 1;
    
    // Update player count
    int count = 0;
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (Players[i].connected == CONNECT_PLAYING) count++;
    }
    N_players = count;
    
    return idx;
}

/* ========== MAIN FRAME ========== */

void botplay_frame(void)
{
    /* Handle pending bot game start */
    if (Botplay_pending_start) {
        Botplay_pending_start = 0;
        botplay_init();
        
        for (int i = 0; i < Botplay_pending_num_bots && i < MAX_BOTS; i++) {
            botplay_add(Botplay_pending_skill);
        }
        
        Game_mode |= GM_MULTI;
        Show_kill_list = 1;
        HUD_init_message(HM_DEFAULT, "ROBO-ANARCHY: %d BOTS - SKILL %d", 
                         Botplay_pending_num_bots, Botplay_pending_skill + 1);
    }
    
    if (!Bot_mode || Num_bots <= 0) return;
    
    
    static int frame_counter = 0;
    frame_counter++;
    
    for (int i = 0; i < MAX_BOTS; i++) {
        if (!Bots[i].active) continue;
        
        bot_info *bot = &Bots[i];
        int pnum = bot->player_num;
        
        // Handle respawn
        if (bot->awaiting_respawn) {
            if (GameTime64 - bot->death_time >= BOT_RESPAWN_DELAY) {
                bot_spawn(i);
            }
            continue;
        }
        
        // Validate player object
        int objnum = Players[pnum].objnum;
        if (objnum < 0 || objnum > Highest_object_index) continue;
        if (Objects[objnum].type != OBJ_PLAYER) continue;
        if (Players[pnum].shields <= 0) continue;
        
        // Clear physics for this frame
        vm_vec_zero(&Objects[objnum].mtype.phys_info.thrust);
        vm_vec_zero(&Objects[objnum].mtype.phys_info.rotthrust);
        
        // Run AI (think less frequently than move)
        if ((frame_counter + i) % BOT_THINK_INTERVAL == 0) {
            bot_think(i);
        }
        
        // Always run movement and combat
        bot_move(i);
        bot_fire(i);
    }
    
    Bot_last_frame_time = GameTime64;
}

/* ========== DAMAGE AND QUERIES ========== */

void botplay_apply_damage(int bot_objnum, int attacker_objnum, fix damage)
{
    if (bot_objnum < 0 || bot_objnum > Highest_object_index) return;
    
    object *bot_obj = &Objects[bot_objnum];
    if (bot_obj->type != OBJ_PLAYER) return;
    
    int pnum = bot_obj->id;
    int bot_idx = botplay_get_bot_idx(pnum);
    if (bot_idx < 0) return;
    
    // Apply damage
    Players[pnum].shields -= damage;
    bot_obj->shields = Players[pnum].shields;
    
    // React to being hit - become aware of attacker
    if (attacker_objnum >= 0 && attacker_objnum <= Highest_object_index) {
        object *attacker = &Objects[attacker_objnum];
        int attacker_pnum = -1;
        
        if (attacker->type == OBJ_PLAYER) {
            attacker_pnum = attacker->id;
        } else if (attacker->type == OBJ_WEAPON) {
            int parent = attacker->ctype.laser_info.parent_num;
            if (parent >= 0 && parent <= Highest_object_index) {
                if (Objects[parent].type == OBJ_PLAYER) {
                    attacker_pnum = Objects[parent].id;
                }
            }
        }
        
        if (attacker_pnum >= 0 && attacker_pnum != pnum) {
            Bots[bot_idx].target_pnum = attacker_pnum;
            Bots[bot_idx].last_saw_target = 1;
            
            // Switch to combat or flee
            int health_pct = (Players[pnum].shields * 100) / (100 * F1_0);
            if (health_pct < BOT_FLEE_HEALTH_PCT && Bots[bot_idx].aggression < 70) {
                bot_change_state(bot_idx, BOT_STATE_FLEE);
            } else if (Bots[bot_idx].state != BOT_STATE_COMBAT) {
                bot_change_state(bot_idx, BOT_STATE_COMBAT);
            }
        }
    }
    
    // Check for death
    if (Players[pnum].shields <= 0) {
        int killer_pnum = -1;
        
        if (attacker_objnum >= 0) {
            object *attacker = &Objects[attacker_objnum];
            if (attacker->type == OBJ_PLAYER) {
                killer_pnum = attacker->id;
            } else if (attacker->type == OBJ_WEAPON) {
                int parent = attacker->ctype.laser_info.parent_num;
                if (parent >= 0 && Objects[parent].type == OBJ_PLAYER) {
                    killer_pnum = Objects[parent].id;
                }
            }
        }
        
        // Drop items
        drop_player_eggs(bot_obj);
        
        // Explosion
        object_create_explosion(bot_obj->segnum, &bot_obj->pos, i2f(40), VCLIP_PLAYER_HIT);
        
        // Update stats
        Bots[bot_idx].deaths++;
        Players[pnum].net_killed_total++;
        
        if (killer_pnum >= 0 && killer_pnum != pnum) {
            Players[killer_pnum].net_kills_total++;
            int killer_bot = botplay_get_bot_idx(killer_pnum);
            if (killer_bot >= 0) {
                Bots[killer_bot].kills++;
            }
        }
        
        // Set to respawn
        bot_obj->type = OBJ_GHOST;
        Bots[bot_idx].awaiting_respawn = 1;
        Bots[bot_idx].death_time = GameTime64;
        Bots[bot_idx].state = BOT_STATE_DEAD;
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

void botplay_remove(int idx)
{
    if (idx < 0 || idx >= MAX_BOTS) return;
    if (!Bots[idx].active) return;
    
    int pnum = Bots[idx].player_num;
    if (pnum >= 0) {
        if (Players[pnum].objnum >= 0 && Players[pnum].objnum <= Highest_object_index) {
            Objects[Players[pnum].objnum].flags |= OF_SHOULD_BE_DEAD;
        }
        Players[pnum].connected = CONNECT_DISCONNECTED;
    }
    
    Bots[idx].active = 0;
    Num_bots--;
    
    if (Num_bots <= 0) {
        Bot_mode = 0;
        Num_bots = 0;
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

void botplay_on_player_death(int victim_pnum, int killer_pnum)
{
    // Update stats when any player dies
    if (!Bot_mode) return;
    
    // Check if victim is a bot
    int victim_bot = botplay_get_bot_idx(victim_pnum);
    if (victim_bot >= 0) {
        Bots[victim_bot].deaths++;
        Bots[victim_bot].awaiting_respawn = 1;
        Bots[victim_bot].death_time = GameTime64;
        Bots[victim_bot].state = BOT_STATE_DEAD;
    }
    
    // Check if killer is a bot
    int killer_bot = botplay_get_bot_idx(killer_pnum);
    if (killer_bot >= 0 && killer_pnum != victim_pnum) {
        Bots[killer_bot].kills++;
    }
}
