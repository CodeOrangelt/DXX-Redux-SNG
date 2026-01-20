/*
 * BOTPLAY.H - Bot Player System
 * Fully independent AI bots for Robo-Anarchy mode
 */

#ifndef _BOTPLAY_H
#define _BOTPLAY_H

#include "pstypes.h"
#include "vecmat.h"

#define MAX_BOTS 7

// AI State types
typedef enum {
    BOT_STATE_SPAWN,
    BOT_STATE_HUNT,
    BOT_STATE_COMBAT,
    BOT_STATE_FLEE,
    BOT_STATE_GET_POWERUP,
    BOT_STATE_ROAM,
    BOT_STATE_DEAD
} bot_state_t;

// Bot data structure - enhanced with state machine
typedef struct bot_info {
    int active;
    int player_num;
    int skill;              // 0-4
    
    /* State Machine */
    bot_state_t state;
    bot_state_t prev_state;
    int state_timer;
    int state_duration;
    
    /* Combat */
    int target_pnum;
    vms_vector last_target_pos;
    int last_target_seg;
    int last_saw_target;
    int combat_strafe_dir;  // -1 or 1
    fix64 last_strafe_change;
    
    /* Firing */
    fix64 next_fire_time;
    fix64 burst_end_time;
    int burst_shots_left;
    int current_burst_weapon;
    
    /* Movement */
    vms_vector goal_pos;
    int goal_seg;
    int goal_valid;
    fix64 last_path_time;
    fix64 stuck_check_time;
    vms_vector last_stuck_pos;
    int stuck_count;
    
    /* Pathfinding */
    int using_ai_path;
    int path_target_seg;
    
    /* Personality */
    int aggression;         // 0-100
    int accuracy;           // 0-100
    int reaction_delay;     // frames
    int preferred_range;    // ideal combat distance
    
    /* Powerups */
    int target_powerup_obj;
    
    /* Stats */
    int kills;
    int deaths;
    
    /* Respawn */
    int awaiting_respawn;
    fix64 death_time;
    
    /* Random state */
    unsigned int rand_state;
} bot_info;

// Globals
extern bot_info Bots[MAX_BOTS];
extern int Num_bots;
extern int Bot_mode;

// Pending start state
extern int Botplay_pending_start;
extern int Botplay_pending_num_bots;
extern int Botplay_pending_skill;

// Core functions
void botplay_init(void);
void botplay_close(void);
void botplay_level_init(void);

int botplay_add(int skill);
void botplay_remove(int idx);
void botplay_remove_all(void);

void botplay_frame(void);

// Damage/death handling
void botplay_apply_damage(int bot_objnum, int attacker_objnum, fix damage);
void botplay_on_player_death(int victim_pnum, int killer_pnum);

// Queries
int botplay_is_bot(int pnum);
int botplay_get_bot_idx(int pnum);

#endif /* _BOTPLAY_H */
