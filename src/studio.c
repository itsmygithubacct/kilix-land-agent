#include "studio.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define STUDIO_PLAYER_SPEED 116.0f
#define STUDIO_PLAYER_RADIUS_X 9.0f
#define STUDIO_PLAYER_RADIUS_Y 6.0f
#define STUDIO_FLOOR_LEFT 15.0f
#define STUDIO_FLOOR_RIGHT 465.0f
#define STUDIO_FLOOR_TOP 168.0f
#define STUDIO_FLOOR_BOTTOM 254.0f
#define STUDIO_INTERACTION_RADIUS 36.0f
#define STUDIO_NOTICE_TICKS 240

typedef struct studio_rect {
    float left;
    float top;
    float right;
    float bottom;
} studio_rect;

/* These conservative rectangles follow the generated plate. They describe
 * furniture footprints only; the large lower-floor corridor remains open. */
static const studio_rect FURNITURE[] = {
    {0.0f, 126.0f, 92.0f, 198.0f},
    {78.0f, 124.0f, 177.0f, 188.0f},
    {184.0f, 132.0f, 290.0f, 182.0f},
    {296.0f, 106.0f, 354.0f, 184.0f},
    {355.0f, 126.0f, 449.0f, 199.0f},
    {431.0f, 154.0f, 480.0f, 207.0f}
};

static const studio_target_info TARGETS[] = {
    {STUDIO_TARGET_COMPUTER, "computer", "Computer",
     "kilix.studio.inspect/v1", 57.0f, 193.0f},
    {STUDIO_TARGET_DESK, "desk", "Desk",
     "kilix.studio.inspect/v1", 28.0f, 200.0f},
    {STUDIO_TARGET_KITCHENETTE, "kitchenette", "Kitchenette",
     "kilix.studio.inspect/v1", 130.0f, 187.0f},
    {STUDIO_TARGET_TV, "tv", "TV",
     "kilix.studio.inspect/v1", 235.0f, 182.0f},
    {STUDIO_TARGET_RADIO, "radio", "Radio",
     "kilix.studio.inspect/v1", 281.0f, 183.0f},
    {STUDIO_TARGET_BOOKSHELF, "bookshelf", "Bookshelf",
     "kilix.studio.inspect/v1", 324.0f, 185.0f},
    {STUDIO_TARGET_BED, "bed", "Bed",
     "kilix.studio.inspect/v1", 397.0f, 200.0f},
    {STUDIO_TARGET_PLANT, "plant", "Plant",
     "kilix.app.launch.plant-simulator/v1", 454.0f, 208.0f}
};

static bool position_blocked(float x, float y)
{
    size_t index;
    if (!isfinite(x) || !isfinite(y) ||
        x - STUDIO_PLAYER_RADIUS_X < STUDIO_FLOOR_LEFT ||
        x + STUDIO_PLAYER_RADIUS_X > STUDIO_FLOOR_RIGHT ||
        y - STUDIO_PLAYER_RADIUS_Y < STUDIO_FLOOR_TOP ||
        y + STUDIO_PLAYER_RADIUS_Y > STUDIO_FLOOR_BOTTOM)
        return true;
    for (index = 0u; index < sizeof FURNITURE / sizeof FURNITURE[0];
         ++index) {
        const studio_rect *rect = &FURNITURE[index];
        if (x + STUDIO_PLAYER_RADIUS_X > rect->left &&
            x - STUDIO_PLAYER_RADIUS_X < rect->right &&
            y + STUDIO_PLAYER_RADIUS_Y > rect->top &&
            y - STUDIO_PLAYER_RADIUS_Y < rect->bottom)
            return true;
    }
    return false;
}

static void copy_notice(studio_state *state, const char *text)
{
    if (!state) return;
    if (!text) text = "";
    (void)snprintf(state->notice, sizeof state->notice, "%s", text);
    state->notice_ticks = text[0] == '\0' ? 0 : STUDIO_NOTICE_TICKS;
}

void studio_init(studio_state *state)
{
    if (!state) return;
    (void)memset(state, 0, sizeof *state);
    state->player_x = 240.0f;
    state->player_y = 235.0f;
    state->facing = STUDIO_FACING_UP;
    state->nearest_target = studio_nearest_target(state);
    copy_notice(state,
                "Studio test mode: walk to an object and press Enter.");
}

void studio_step(studio_state *state, int move_x, int move_y, float seconds)
{
    float next_x;
    float next_y;
    bool moved = false;
    if (!state || !isfinite(seconds) || seconds <= 0.0f || seconds > 0.25f)
        return;
    state->player_moving = false;
    next_x = state->player_x;
    next_y = state->player_y;
    if (move_x < 0) {
        state->facing = STUDIO_FACING_LEFT;
        next_x -= STUDIO_PLAYER_SPEED * seconds;
    } else if (move_x > 0) {
        state->facing = STUDIO_FACING_RIGHT;
        next_x += STUDIO_PLAYER_SPEED * seconds;
    } else if (move_y < 0) {
        state->facing = STUDIO_FACING_UP;
        next_y -= STUDIO_PLAYER_SPEED * seconds;
    } else if (move_y > 0) {
        state->facing = STUDIO_FACING_DOWN;
        next_y += STUDIO_PLAYER_SPEED * seconds;
    }
    if (next_x != state->player_x &&
        !position_blocked(next_x, state->player_y)) {
        state->player_x = next_x;
        moved = true;
    }
    if (next_y != state->player_y &&
        !position_blocked(state->player_x, next_y)) {
        state->player_y = next_y;
        moved = true;
    }
    state->player_moving = moved;
    if (moved) state->revision++;
    state->simulation_tick++;
    state->nearest_target = studio_nearest_target(state);
    if (state->notice_ticks > 0) {
        state->notice_ticks--;
        if (state->notice_ticks == 0) state->notice[0] = '\0';
    }
}

const studio_target_info *studio_target_info_for(studio_target target)
{
    size_t index;
    for (index = 0u; index < sizeof TARGETS / sizeof TARGETS[0]; ++index)
        if (TARGETS[index].target == target) return &TARGETS[index];
    return NULL;
}

const studio_target_info *studio_target_info_for_id(const char *id)
{
    size_t index;
    if (!id || id[0] == '\0') return NULL;
    for (index = 0u; index < sizeof TARGETS / sizeof TARGETS[0]; ++index)
        if (strcmp(TARGETS[index].id, id) == 0) return &TARGETS[index];
    return NULL;
}

const studio_target_info *studio_target_info_at(size_t index)
{
    return index < sizeof TARGETS / sizeof TARGETS[0] ?
           &TARGETS[index] : NULL;
}

size_t studio_target_info_count(void)
{
    return sizeof TARGETS / sizeof TARGETS[0];
}

studio_target studio_nearest_target(const studio_state *state)
{
    float best = STUDIO_INTERACTION_RADIUS * STUDIO_INTERACTION_RADIUS;
    studio_target selected = STUDIO_TARGET_NONE;
    size_t index;
    if (!state) return STUDIO_TARGET_NONE;
    for (index = 0u; index < sizeof TARGETS / sizeof TARGETS[0]; ++index) {
        float dx = TARGETS[index].x - state->player_x;
        float dy = (TARGETS[index].y - state->player_y) * 1.35f;
        float distance = dx * dx + dy * dy;
        if (distance <= best) {
            best = distance;
            selected = TARGETS[index].target;
        }
    }
    return selected;
}

bool studio_interact(studio_state *state)
{
    studio_target target;
    if (!state) return false;
    target = studio_nearest_target(state);
    state->nearest_target = target;
    switch (target) {
    case STUDIO_TARGET_COMPUTER:
        copy_notice(state,
                    "Computer: safe app catalog is not connected yet.");
        break;
    case STUDIO_TARGET_DESK:
        copy_notice(state, "Desk: Kilix's workspace is ready.");
        break;
    case STUDIO_TARGET_KITCHENETTE:
        copy_notice(state, "Kitchenette: no agent capability is registered.");
        break;
    case STUDIO_TARGET_TV:
        copy_notice(state, "TV: media capability is disabled in test mode.");
        break;
    case STUDIO_TARGET_RADIO:
        copy_notice(state, "Radio: audio capability is disabled in test mode.");
        break;
    case STUDIO_TARGET_BOOKSHELF:
        copy_notice(state, "Bookshelf: local reading index is not connected.");
        break;
    case STUDIO_TARGET_BED:
        copy_notice(state, "Bed: rest is cosmetic; power actions do not exist.");
        break;
    case STUDIO_TARGET_PLANT:
        copy_notice(state,
                    "Plant: opening the allowlisted plant simulator...");
        state->plant_launch_requested = true;
        break;
    case STUDIO_TARGET_NONE:
    case STUDIO_TARGET_COUNT:
        copy_notice(state, "Nothing is close enough to use.");
        return false;
    }
    state->revision++;
    return true;
}

bool studio_take_plant_launch_request(studio_state *state)
{
    bool requested;
    if (!state) return false;
    requested = state->plant_launch_requested;
    state->plant_launch_requested = false;
    return requested;
}

void studio_set_notice(studio_state *state, const char *text)
{
    copy_notice(state, text);
}

bool studio_validate(const studio_state *state, char *error,
                     size_t error_size)
{
    const char *message = NULL;
    if (!state) message = "state is null";
    else if (position_blocked(state->player_x, state->player_y))
        message = "player position is blocked";
    else if (state->facing < STUDIO_FACING_DOWN ||
             state->facing > STUDIO_FACING_UP)
        message = "facing is invalid";
    else if (state->nearest_target < STUDIO_TARGET_NONE ||
             state->nearest_target >= STUDIO_TARGET_COUNT)
        message = "nearest target is invalid";
    if (error && error_size > 0u)
        (void)snprintf(error, error_size, "%s", message ? message : "");
    return message == NULL;
}
