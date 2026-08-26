#ifndef KILIX_LAND_AGENT_STUDIO_H
#define KILIX_LAND_AGENT_STUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STUDIO_LOGICAL_WIDTH 480
#define STUDIO_LOGICAL_HEIGHT 270
#define STUDIO_SIMULATION_HZ 60
#define STUDIO_TICK_SECONDS (1.0f / 60.0f)
#define STUDIO_NOTICE_CAPACITY 160

typedef enum studio_facing {
    STUDIO_FACING_DOWN = 0,
    STUDIO_FACING_LEFT = 1,
    STUDIO_FACING_RIGHT = 2,
    STUDIO_FACING_UP = 3
} studio_facing;

typedef enum studio_target {
    STUDIO_TARGET_NONE = 0,
    STUDIO_TARGET_COMPUTER,
    STUDIO_TARGET_DESK,
    STUDIO_TARGET_KITCHENETTE,
    STUDIO_TARGET_TV,
    STUDIO_TARGET_RADIO,
    STUDIO_TARGET_BOOKSHELF,
    STUDIO_TARGET_BED,
    STUDIO_TARGET_PLANT,
    STUDIO_TARGET_COUNT
} studio_target;

typedef struct studio_target_info {
    studio_target target;
    const char *id;
    const char *label;
    const char *capability_id;
    float x;
    float y;
} studio_target_info;

typedef struct studio_state {
    float player_x;
    float player_y;
    uint64_t simulation_tick;
    uint64_t revision;
    studio_facing facing;
    studio_target nearest_target;
    bool player_moving;
    bool plant_launch_requested;
    int notice_ticks;
    char notice[STUDIO_NOTICE_CAPACITY];
} studio_state;

void studio_init(studio_state *state);
void studio_step(studio_state *state, int move_x, int move_y, float seconds);
bool studio_interact(studio_state *state);
bool studio_take_plant_launch_request(studio_state *state);
void studio_set_notice(studio_state *state, const char *text);
bool studio_validate(const studio_state *state, char *error,
                     size_t error_size);

const studio_target_info *studio_target_info_for(studio_target target);
const studio_target_info *studio_target_info_for_id(const char *id);
const studio_target_info *studio_target_info_at(size_t index);
size_t studio_target_info_count(void);
studio_target studio_nearest_target(const studio_state *state);

#endif
