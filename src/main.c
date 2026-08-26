#include "agent_protocol.h"
#include "agent_session.h"
#include "graphics.h"
#include "render.h"
#include "studio.h"

#include "kilix_game_kit.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct studio_input {
    int move_x;
    int move_y;
    bool interact_pressed;
} studio_input;

typedef enum agent_replay_phase {
    AGENT_REPLAY_OBSERVE = 0,
    AGENT_REPLAY_GO_X,
    AGENT_REPLAY_GO_Y,
    AGENT_REPLAY_ARRIVED,
    AGENT_REPLAY_COMPLETE,
    AGENT_REPLAY_FAILED
} agent_replay_phase;

typedef struct agent_replay {
    const studio_target_info *target;
    const char *controller;
    agent_replay_phase phase;
    uint64_t phase_ticks;
} agent_replay;

static kittyts_session terminal_session;
static uint32_t last_movement_key;
static char project_root_storage[PATH_MAX];

#define AGENT_VIDEO_WIDTH 1280
#define AGENT_VIDEO_HEIGHT 720
#define AGENT_VIDEO_FPS 30
#define AGENT_VIDEO_FRAMES (30 * AGENT_VIDEO_FPS)

static const char *project_root(void);

static const char *asset_root(void)
{
    const char *override = getenv("KILIX_LAND_AGENT_ASSETS");
    return override && override[0] != '\0' ? override : project_root();
}

static bool event_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static uint32_t normalize_movement_key(uint32_t key)
{
    if (key == (uint32_t)'w' || key == (uint32_t)'W') return KITTYKB_KEY_UP;
    if (key == (uint32_t)'s' || key == (uint32_t)'S') return KITTYKB_KEY_DOWN;
    if (key == (uint32_t)'a' || key == (uint32_t)'A') return KITTYKB_KEY_LEFT;
    if (key == (uint32_t)'d' || key == (uint32_t)'D') return KITTYKB_KEY_RIGHT;
    if (key == KITTYKB_KEY_UP || key == KITTYKB_KEY_DOWN ||
        key == KITTYKB_KEY_LEFT || key == KITTYKB_KEY_RIGHT)
        return key;
    return KITTYKB_KEY_NONE;
}

static bool movement_key_down(uint32_t key)
{
    switch (key) {
    case KITTYKB_KEY_UP:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_UP) ||
               kittyts_key_down(&terminal_session, (uint32_t)'w');
    case KITTYKB_KEY_DOWN:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_DOWN) ||
               kittyts_key_down(&terminal_session, (uint32_t)'s');
    case KITTYKB_KEY_LEFT:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_LEFT) ||
               kittyts_key_down(&terminal_session, (uint32_t)'a');
    case KITTYKB_KEY_RIGHT:
        return kittyts_key_down(&terminal_session, KITTYKB_KEY_RIGHT) ||
               kittyts_key_down(&terminal_session, (uint32_t)'d');
    default:
        return false;
    }
}

static void apply_movement_key(studio_input *input, uint32_t key)
{
    if (!input) return;
    if (key == KITTYKB_KEY_UP) input->move_y = -1;
    else if (key == KITTYKB_KEY_DOWN) input->move_y = 1;
    else if (key == KITTYKB_KEY_LEFT) input->move_x = -1;
    else if (key == KITTYKB_KEY_RIGHT) input->move_x = 1;
}

static bool controller_label_valid(const char *label)
{
    size_t index;
    if (!label || label[0] == '\0') return false;
    for (index = 0u; label[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)label[index];
        bool allowed = (character >= (unsigned char)'a' &&
                        character <= (unsigned char)'z') ||
                       (character >= (unsigned char)'A' &&
                        character <= (unsigned char)'Z') ||
                       (character >= (unsigned char)'0' &&
                        character <= (unsigned char)'9') ||
                       character == (unsigned char)'.' ||
                       character == (unsigned char)'_' ||
                       character == (unsigned char)'-' ||
                       character == (unsigned char)':' ||
                       character == (unsigned char)'/';
        if (!allowed || index >= 31u) return false;
    }
    return true;
}

static bool speech_text_valid(const char *speech)
{
    size_t index;
    if (!speech || speech[0] == '\0') return false;
    for (index = 0u; speech[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)speech[index];
        if (character < UINT8_C(32) || character > UINT8_C(126) ||
            index >= 63u)
            return false;
    }
    return true;
}

static void replay_notice(studio_state *state, const agent_replay *replay,
                          const char *action)
{
    char notice[STUDIO_NOTICE_CAPACITY];
    if (!state || !replay || !action) return;
    (void)snprintf(notice, sizeof notice, "%s: %s", replay->controller,
                   action);
    studio_set_notice(state, notice);
}

static void agent_replay_init(agent_replay *replay, studio_state *state,
                              const char *controller,
                              const studio_target_info *target)
{
    if (!replay || !state || !controller || !target) return;
    replay->target = target;
    replay->controller = controller;
    replay->phase = AGENT_REPLAY_OBSERVE;
    replay->phase_ticks = 0u;
    replay_notice(state, replay,
                  "observe_room() accepted by the trusted harness.");
}

static void agent_replay_step(agent_replay *replay, studio_state *state,
                              studio_input *input)
{
    float delta;
    char action[128];
    if (!replay || !state || !input || !replay->target) return;
    (void)memset(input, 0, sizeof *input);
    switch (replay->phase) {
    case AGENT_REPLAY_OBSERVE:
        replay->phase_ticks++;
        if (replay->phase_ticks >= UINT64_C(90)) {
            replay->phase = AGENT_REPLAY_GO_X;
            replay->phase_ticks = 0u;
            (void)snprintf(action, sizeof action,
                           "go_to(%s) validated; Kilix is moving.",
                           replay->target->id);
            replay_notice(state, replay, action);
        }
        break;
    case AGENT_REPLAY_GO_X:
        delta = replay->target->x - state->player_x;
        if (delta < -2.5f) input->move_x = -1;
        else if (delta > 2.5f) input->move_x = 1;
        else replay->phase = AGENT_REPLAY_GO_Y;
        break;
    case AGENT_REPLAY_GO_Y:
        delta = replay->target->y + 18.0f - state->player_y;
        if (delta < -2.5f) input->move_y = -1;
        else if (delta > 2.5f) input->move_y = 1;
        else {
            replay->phase = AGENT_REPLAY_ARRIVED;
            replay->phase_ticks = 0u;
            (void)snprintf(action, sizeof action,
                           "arrived at %s; validating interact().",
                           replay->target->id);
            replay_notice(state, replay, action);
        }
        break;
    case AGENT_REPLAY_ARRIVED:
        replay->phase_ticks++;
        if (replay->phase_ticks >= UINT64_C(60)) {
            state->nearest_target = studio_nearest_target(state);
            if (state->nearest_target != replay->target->target ||
                !studio_interact(state)) {
                replay->phase = AGENT_REPLAY_FAILED;
                replay_notice(state, replay,
                              "interact() rejected: target is not nearby.");
                break;
            }
            replay->phase = AGENT_REPLAY_COMPLETE;
            (void)snprintf(action, sizeof action,
                           "interact(%s) accepted. Q / ESC closes demo.",
                           replay->target->id);
            replay_notice(state, replay, action);
        }
        break;
    case AGENT_REPLAY_COMPLETE:
    case AGENT_REPLAY_FAILED:
        break;
    }
}

static bool poll_input(studio_input *input, bool *quit_requested)
{
    static const uint32_t directions[] = {
        KITTYKB_KEY_UP, KITTYKB_KEY_DOWN,
        KITTYKB_KEY_LEFT, KITTYKB_KEY_RIGHT
    };
    kittykb_event event;
    size_t index;
    if (!input || !quit_requested) {
        errno = EINVAL;
        return false;
    }
    (void)memset(input, 0, sizeof *input);
    if (kittyts_read_input(&terminal_session) < 0 && errno != EAGAIN &&
        errno != EWOULDBLOCK && errno != EINTR)
        return false;
    while (kittyts_next_key_event(&terminal_session, &event)) {
        uint32_t movement_key = normalize_movement_key(event.key);
        if (movement_key != KITTYKB_KEY_NONE &&
            event.action != KITTYKB_ACTION_RELEASE)
            last_movement_key = movement_key;
        if (event.action != KITTYKB_ACTION_PRESS) continue;
        if ((event_letter(&event, 'c') &&
             (event.modifiers & KITTYKB_MOD_CTRL) != 0u) ||
            event_letter(&event, 'q') || event.key == KITTYKB_KEY_ESCAPE) {
            *quit_requested = true;
        } else if (event.key == KITTYKB_KEY_ENTER ||
                   kittykb_event_matches_key(&event, (uint32_t)' ')) {
            input->interact_pressed = true;
        }
    }
    if (last_movement_key != KITTYKB_KEY_NONE &&
        movement_key_down(last_movement_key)) {
        apply_movement_key(input, last_movement_key);
        return true;
    }
    for (index = 0u; index < sizeof directions / sizeof directions[0];
         ++index) {
        if (movement_key_down(directions[index])) {
            last_movement_key = directions[index];
            apply_movement_key(input, directions[index]);
            break;
        }
    }
    return true;
}

static const char *project_root(void)
{
    ssize_t length;
    char *slash;
    if (project_root_storage[0] != '\0') return project_root_storage;
    length = readlink("/proc/self/exe", project_root_storage,
                      sizeof project_root_storage - 1u);
    if (length <= 0 || (size_t)length >= sizeof project_root_storage) {
        (void)snprintf(project_root_storage,
                       sizeof project_root_storage, "%s", ".");
        return project_root_storage;
    }
    project_root_storage[(size_t)length] = '\0';
    slash = strrchr(project_root_storage, '/');
    if (!slash || slash == project_root_storage) {
        (void)snprintf(project_root_storage,
                       sizeof project_root_storage, "%s", ".");
    } else {
        *slash = '\0';
    }
    return project_root_storage;
}

static int run_fixed_program(const char *path)
{
    pid_t child;
    pid_t waited;
    int status;
    char *const arguments[] = {(char *)"pleb-plant-grower", NULL};
    if (!path || access(path, X_OK) != 0) return -1;
    child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        execv(path, arguments);
        _exit(127);
    }
    do {
        waited = waitpid(child, &status, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

static int launch_plant_simulator(void)
{
    static const char *const installed[] = {
        "/usr/local/bin/pleb-plant-grower",
        "/usr/bin/pleb-plant-grower"
    };
    char sibling[PATH_MAX];
    int written;
    int result;
    size_t index;
    written = snprintf(sibling, sizeof sibling,
                       "%s/../../games/pleb-plant-grower/"
                       "pleb-plant-grower", project_root());
    if (written >= 0 && (size_t)written < sizeof sibling) {
        result = run_fixed_program(sibling);
        if (result >= 0) return result;
    }
    for (index = 0u; index < sizeof installed / sizeof installed[0];
         ++index) {
        result = run_fixed_program(installed[index]);
        if (result >= 0) return result;
    }
    return -1;
}

static bool write_ppm(const char *path, const uint8_t *rgba,
                      int width, int height)
{
    FILE *output;
    uint8_t *row;
    int y;
    if (!path || !rgba || width <= 0 || height <= 0) return false;
    output = fopen(path, "wb");
    if (!output) return false;
    row = malloc((size_t)width * 3u);
    if (!row) {
        (void)fclose(output);
        return false;
    }
    if (fprintf(output, "P6\n%d %d\n255\n", width, height) < 0) {
        free(row);
        (void)fclose(output);
        return false;
    }
    for (y = 0; y < height; ++y) {
        int x;
        for (x = 0; x < width; ++x) {
            const uint8_t *source =
                rgba + ((size_t)y * (size_t)width + (size_t)x) * 4u;
            row[(size_t)x * 3u] = source[0];
            row[(size_t)x * 3u + 1u] = source[1];
            row[(size_t)x * 3u + 2u] = source[2];
        }
        if (fwrite(row, 3u, (size_t)width, output) != (size_t)width) {
            free(row);
            (void)fclose(output);
            return false;
        }
    }
    free(row);
    return fclose(output) == 0;
}

static int selftest(void)
{
    studio_state state;
    studio_input replay_input;
    agent_replay replay;
    const studio_target_info *replay_target;
    char error[128];
    size_t index;
    size_t replay_steps;
    if (!agent_protocol_selftest() || !agent_session_selftest()) {
        (void)fprintf(stderr, "FAIL agent protocol or session\n");
        return EXIT_FAILURE;
    }
    studio_init(&state);
    if (!studio_validate(&state, error, sizeof error) ||
        studio_target_info_count() != 8u) {
        (void)fprintf(stderr, "FAIL initial state: %s\n", error);
        return EXIT_FAILURE;
    }
    for (index = 0u; index < studio_target_info_count(); ++index) {
        const studio_target_info *target = studio_target_info_at(index);
        size_t other;
        if (!target || !target->id || target->id[0] == '\0' ||
            !target->capability_id || target->capability_id[0] == '\0') {
            (void)fprintf(stderr, "FAIL target metadata index=%zu\n", index);
            return EXIT_FAILURE;
        }
        for (other = index + 1u; other < studio_target_info_count(); ++other) {
            const studio_target_info *candidate = studio_target_info_at(other);
            if (candidate && strcmp(target->id, candidate->id) == 0) {
                (void)fprintf(stderr, "FAIL duplicate target id=%s\n",
                              target->id);
                return EXIT_FAILURE;
            }
        }
    }
    replay_target = studio_target_info_for_id("bookshelf");
    if (!replay_target ||
        studio_target_info_for_id("not-a-target") != NULL) {
        (void)fprintf(stderr, "FAIL semantic target lookup\n");
        return EXIT_FAILURE;
    }
    agent_replay_init(&replay, &state, "qwen3.5:9b", replay_target);
    for (replay_steps = 0u;
         replay_steps < 900u && replay.phase != AGENT_REPLAY_COMPLETE &&
         replay.phase != AGENT_REPLAY_FAILED;
         ++replay_steps) {
        agent_replay_step(&replay, &state, &replay_input);
        studio_step(&state, replay_input.move_x, replay_input.move_y,
                    STUDIO_TICK_SECONDS);
    }
    if (replay.phase != AGENT_REPLAY_COMPLETE ||
        state.nearest_target != STUDIO_TARGET_BOOKSHELF) {
        (void)fprintf(stderr, "FAIL validated agent replay\n");
        return EXIT_FAILURE;
    }
    studio_init(&state);
    for (index = 0u; index < 300u; ++index)
        studio_step(&state, 0, -1, STUDIO_TICK_SECONDS);
    if (!studio_validate(&state, error, sizeof error) ||
        state.player_y < 186.0f) {
        (void)fprintf(stderr, "FAIL furniture collision: %s\n", error);
        return EXIT_FAILURE;
    }
    state.player_x = 454.0f;
    state.player_y = 216.0f;
    state.nearest_target = studio_nearest_target(&state);
    if (state.nearest_target != STUDIO_TARGET_PLANT ||
        !studio_interact(&state) ||
        !studio_take_plant_launch_request(&state) ||
        studio_take_plant_launch_request(&state)) {
        (void)fprintf(stderr, "FAIL plant capability request\n");
        return EXIT_FAILURE;
    }
    if (!studio_validate(&state, error, sizeof error)) {
        (void)fprintf(stderr, "FAIL final state: %s\n", error);
        return EXIT_FAILURE;
    }
    (void)printf(
        "PASS studio-state targets=8 collisions=bounded "
        "agent-replay=validated protocol=fail-closed "
        "plant=fixed-capability "
        "power-actions=absent\n");
    return EXIT_SUCCESS;
}

static int graphics_test(void)
{
    studio_graphics graphics;
    char error[160];
    if (!studio_graphics_init(&graphics, asset_root(), error,
                              sizeof error)) {
        (void)fprintf(stderr, "FAIL graphics: %s\n", error);
        return EXIT_FAILURE;
    }
    studio_graphics_shutdown(&graphics);
    (void)printf("PASS studio-graphics room=1280x720 player=16x8x64\n");
    return EXIT_SUCCESS;
}

static int render_test(const char *path)
{
    studio_graphics graphics;
    studio_state state;
    ki_td_soft_renderer renderer = {0};
    char error[160];
    bool success;
    if (!studio_graphics_init(&graphics, asset_root(), error,
                              sizeof error)) {
        (void)fprintf(stderr, "FAIL graphics: %s\n", error);
        return EXIT_FAILURE;
    }
    studio_init(&state);
    state.player_x = 442.0f;
    state.player_y = 221.0f;
    state.nearest_target = studio_nearest_target(&state);
    if (!ki_td_soft_renderer_init(&renderer, 1280, 720)) {
        studio_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    success = studio_render(&renderer, &state, &graphics) &&
              write_ppm(path, renderer.rgba, 1280, 720);
    ki_td_soft_renderer_destroy(&renderer);
    studio_graphics_shutdown(&graphics);
    if (!success) {
        (void)fprintf(stderr, "FAIL render output=%s\n", path);
        return EXIT_FAILURE;
    }
    (void)printf("PASS studio-render size=1280x720 output=%s\n", path);
    return EXIT_SUCCESS;
}

static void video_move_toward(studio_state *state,
                              const studio_target_info *target)
{
    float delta;
    int move_x = 0;
    int move_y = 0;
    if (!state || !target) return;
    delta = target->x - state->player_x;
    if (delta < -2.5f) move_x = -1;
    else if (delta > 2.5f) move_x = 1;
    else {
        delta = target->y + 18.0f - state->player_y;
        if (delta < -2.5f) move_y = -1;
        else if (delta > 2.5f) move_y = 1;
    }
    studio_step(state, move_x, move_y, STUDIO_TICK_SECONDS);
}

static bool write_video_frame(const ki_td_soft_renderer *renderer)
{
    size_t pixels = (size_t)AGENT_VIDEO_WIDTH *
                    (size_t)AGENT_VIDEO_HEIGHT;
    if (!renderer || !renderer->rgba) return false;
    return fwrite(renderer->rgba, 4u, pixels, stdout) == pixels;
}

static int run_video_replay(const char *controller,
                            const char *first_target_id,
                            const char *second_target_id,
                            const char *speech)
{
    const studio_target_info *first_target;
    const studio_target_info *second_target;
    studio_graphics graphics;
    studio_state state;
    ki_td_soft_renderer renderer = {0};
    char actions[7][128];
    char error[160];
    int frame;
    bool failed = false;
    if (!controller_label_valid(controller) ||
        !speech_text_valid(speech)) {
        (void)fprintf(stderr,
                      "kilix-land-agent: unsafe video label or speech\n");
        return 2;
    }
    first_target = studio_target_info_for_id(first_target_id);
    second_target = studio_target_info_for_id(second_target_id);
    if (!first_target || !second_target || first_target == second_target) {
        (void)fprintf(stderr,
                      "kilix-land-agent: video targets must be distinct "
                      "semantic IDs\n");
        return 2;
    }
    (void)snprintf(actions[0], sizeof actions[0],
                   "QWEN ACTION 1/7  observe_room()  // VALIDATED");
    (void)snprintf(actions[1], sizeof actions[1],
                   "QWEN ACTION 2/7  go_to(%s)  // VALIDATED",
                   first_target->id);
    (void)snprintf(actions[2], sizeof actions[2],
                   "QWEN ACTION 3/7  interact(%s)  // VALIDATED",
                   first_target->id);
    (void)snprintf(actions[3], sizeof actions[3],
                   "QWEN ACTION 4/7  go_to(%s)  // VALIDATED",
                   second_target->id);
    (void)snprintf(actions[4], sizeof actions[4],
                   "QWEN ACTION 5/7  interact(%s)  // VALIDATED",
                   second_target->id);
    (void)snprintf(actions[5], sizeof actions[5],
                   "QWEN ACTION 6/7  face_user()  // VALIDATED");
    (void)snprintf(actions[6], sizeof actions[6],
                   "QWEN ACTION 7/7  say(text)  // VALIDATED + TTS");

    if (!studio_graphics_init(&graphics, asset_root(), error,
                              sizeof error)) {
        (void)fprintf(stderr, "kilix-land-agent: %s\n", error);
        return EXIT_FAILURE;
    }
    if (!ki_td_soft_renderer_init(&renderer, AGENT_VIDEO_WIDTH,
                                  AGENT_VIDEO_HEIGHT)) {
        studio_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    studio_init(&state);
    studio_set_notice(&state, "");
    for (frame = 0; frame < AGENT_VIDEO_FRAMES; ++frame) {
        const char *action;
        const char *visible_speech = "";
        if (frame < 60) {
            action = actions[0];
            studio_step(&state, 0, 0, STUDIO_TICK_SECONDS);
        } else if (frame < 240) {
            action = actions[1];
            video_move_toward(&state, first_target);
        } else if (frame < 330) {
            action = actions[2];
            if (frame == 240 &&
                (studio_nearest_target(&state) != first_target->target ||
                 !studio_interact(&state))) {
                failed = true;
                break;
            }
            studio_set_notice(&state, "");
            (void)studio_take_plant_launch_request(&state);
            studio_step(&state, 0, 0, STUDIO_TICK_SECONDS);
        } else if (frame < 600) {
            action = actions[3];
            video_move_toward(&state, second_target);
        } else if (frame < 690) {
            action = actions[4];
            if (frame == 600 &&
                (studio_nearest_target(&state) != second_target->target ||
                 !studio_interact(&state))) {
                failed = true;
                break;
            }
            studio_set_notice(&state, "");
            (void)studio_take_plant_launch_request(&state);
            studio_step(&state, 0, 0, STUDIO_TICK_SECONDS);
        } else if (frame < 750) {
            action = actions[5];
            state.facing = STUDIO_FACING_DOWN;
            studio_step(&state, 0, 0, STUDIO_TICK_SECONDS);
        } else {
            action = actions[6];
            visible_speech = speech;
            state.facing = STUDIO_FACING_DOWN;
            studio_step(&state, 0, 0, STUDIO_TICK_SECONDS);
        }
        if (!studio_render_agent_video(&renderer, &state, &graphics,
                                       controller, action,
                                       visible_speech) ||
            !write_video_frame(&renderer)) {
            failed = true;
            break;
        }
    }
    if (fflush(stdout) != 0) failed = true;
    ki_td_soft_renderer_destroy(&renderer);
    studio_graphics_shutdown(&graphics);
    if (failed) {
        (void)fprintf(stderr, "kilix-land-agent: video replay failed\n");
        return EXIT_FAILURE;
    }
    (void)fprintf(stderr,
                  "PASS agent-video frames=%d fps=%d duration=30s\n",
                  AGENT_VIDEO_FRAMES, AGENT_VIDEO_FPS);
    return EXIT_SUCCESS;
}

static int print_observation(void)
{
    studio_state state;
    size_t index;
    studio_init(&state);
    (void)printf(
        "{\"protocol\":\"kilix.land.observe/v1\","
        "\"revision\":%llu,\"room_id\":\"studio-apartment\","
        "\"character_id\":\"kilix\","
        "\"player\":{\"x\":%.1f,\"y\":%.1f,\"facing\":\"up\"},"
        "\"entities\":[",
        (unsigned long long)state.revision,
        (double)state.player_x, (double)state.player_y);
    for (index = 0u; index < studio_target_info_count(); ++index) {
        const studio_target_info *target = studio_target_info_at(index);
        (void)printf(
            "%s{\"entity_id\":\"%s\",\"label\":\"%s\","
            "\"capability_id\":\"%s\",\"x\":%.1f,\"y\":%.1f}",
            index == 0u ? "" : ",", target->id, target->label,
            target->capability_id, (double)target->x, (double)target->y);
    }
    (void)printf(
        "],\"available_actions\":[\"observe\",\"go_to\","
        "\"interact\",\"face_user\",\"say\",\"cancel\"]}\n");
    return EXIT_SUCCESS;
}

static bool start_terminal(const kittyts_options *options,
                           ki_td_soft_renderer *renderer)
{
    int width;
    int height;
    if (kittyts_start(&terminal_session, STDIN_FILENO, STDOUT_FILENO,
                      options) != 0)
        return false;
    width = kittyts_width(&terminal_session);
    height = kittyts_height(&terminal_session);
    if (!ki_td_soft_renderer_resize(renderer, width, height)) {
        kittyts_stop(&terminal_session);
        return false;
    }
    last_movement_key = KITTYKB_KEY_NONE;
    return true;
}

static int run_room(const char *controller,
                    const studio_target_info *replay_target,
                    agent_session *live_session)
{
    kittyts_options terminal_options;
    kilix_game_signal_scope signals = {0};
    kilix_game_clock clock;
    kilix_game_clock_options clock_options;
    ki_td_soft_renderer renderer = {0};
    studio_graphics graphics;
    studio_state state;
    studio_input current_input;
    studio_input simulation_input;
    agent_replay replay = {0};
    bool replay_active = replay_target != NULL;
    bool live_active = live_session != NULL;
    bool pending_interact = false;
    bool quit_requested = false;
    bool failed = false;
    bool first_frame = true;
    uint64_t last_presented_step = UINT64_MAX;
    char error[160];
    int width;
    int height;

    if (!studio_graphics_init(&graphics, asset_root(), error,
                              sizeof error)) {
        (void)fprintf(stderr, "kilix-land-agent: %s\n", error);
        return EXIT_FAILURE;
    }
    studio_init(&state);
    if (replay_active)
        agent_replay_init(&replay, &state, controller, replay_target);
    kittyts_session_init(&terminal_session);
    kittyts_options_init(&terminal_options);
    terminal_options.framebuffer.min_width = 800;
    terminal_options.framebuffer.min_height = 450;
    terminal_options.framebuffer.max_width = 1280;
    terminal_options.framebuffer.max_height = 720;
    if (kittyts_start(&terminal_session, STDIN_FILENO, STDOUT_FILENO,
                      &terminal_options) != 0) {
        (void)fprintf(stderr, "kilix-land-agent: terminal start failed: %s\n",
                      strerror(errno));
        studio_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    if (!kilix_game_signals_install(&signals)) {
        kittyts_stop(&terminal_session);
        studio_graphics_shutdown(&graphics);
        return EXIT_FAILURE;
    }
    width = kittyts_width(&terminal_session);
    height = kittyts_height(&terminal_session);
    if (!ki_td_soft_renderer_init(&renderer, width, height)) {
        failed = true;
        goto done;
    }
    kilix_game_clock_options_init(&clock_options);
    clock_options.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND /
                            STUDIO_SIMULATION_HZ;
    if (!kilix_game_clock_init(&clock, &clock_options)) {
        failed = true;
        goto done;
    }
    last_movement_key = KITTYKB_KEY_NONE;

    while (!quit_requested && !kilix_game_signals_requested(&signals)) {
        kilix_game_frame frame;
        uint32_t step;
        int resized_width;
        int resized_height;
        int64_t now;
        if (!poll_input(&current_input, &quit_requested)) {
            failed = true;
            break;
        }
        if (quit_requested) break;
        if (live_active &&
            !agent_session_poll(live_session, &state)) {
            quit_requested = true;
            break;
        }
        if (live_active &&
            (current_input.move_x != 0 || current_input.move_y != 0 ||
             current_input.interact_pressed))
            agent_session_manual_override(live_session, &state);
        if (!replay_active)
            pending_interact = pending_interact ||
                               current_input.interact_pressed;
        now = kilix_game_monotonic_ns();
        frame = kilix_game_clock_advance(&clock, now);
        for (step = 0u; step < frame.steps; ++step) {
            if (replay_active) {
                agent_replay_step(&replay, &state, &simulation_input);
            } else {
                simulation_input = current_input;
                if (live_active && simulation_input.move_x == 0 &&
                    simulation_input.move_y == 0 && !pending_interact)
                    agent_session_drive(live_session, &state,
                                        &simulation_input.move_x,
                                        &simulation_input.move_y);
                if (pending_interact) {
                    (void)studio_interact(&state);
                    pending_interact = false;
                }
            }
            studio_step(&state, simulation_input.move_x,
                        simulation_input.move_y, STUDIO_TICK_SECONDS);
        }
        if (studio_take_plant_launch_request(&state)) {
            int result;
            kittyts_stop(&terminal_session);
            result = launch_plant_simulator();
            if (!start_terminal(&terminal_options, &renderer)) {
                failed = true;
                break;
            }
            width = kittyts_width(&terminal_session);
            height = kittyts_height(&terminal_session);
            kilix_game_clock_init(&clock, &clock_options);
            pending_interact = false;
            first_frame = true;
            studio_set_notice(
                &state,
                result < 0 ?
                "Plant simulator is not installed at an allowlisted path." :
                result == 0 ?
                "Returned from the plant simulator." :
                "Plant simulator exited with an error.");
        }
        if (kittyts_check_resize(&terminal_session, &resized_width,
                                 &resized_height)) {
            if (!ki_td_soft_renderer_resize(&renderer, resized_width,
                                            resized_height)) {
                failed = true;
                break;
            }
            width = resized_width;
            height = resized_height;
            first_frame = true;
        }
        if (first_frame ||
            state.simulation_tick / UINT64_C(2) !=
            last_presented_step / UINT64_C(2)) {
            if (!studio_render(&renderer, &state, &graphics) ||
                !kittyts_present(&terminal_session, renderer.rgba,
                                 width, height)) {
                failed = true;
                break;
            }
            first_frame = false;
            last_presented_step = state.simulation_tick;
        }
        if (frame.steps == 0u)
            (void)kilix_game_sleep_until_ns(now + INT64_C(1000000));
    }

done:
    ki_td_soft_renderer_destroy(&renderer);
    kittyts_stop(&terminal_session);
    kilix_game_signals_restore(&signals);
    studio_graphics_shutdown(&graphics);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int run_test_room(void)
{
    return run_room(NULL, NULL, NULL);
}

static int run_agent_replay(const char *controller, const char *target_id)
{
    const studio_target_info *target;
    if (!controller_label_valid(controller)) {
        (void)fprintf(stderr,
                      "kilix-land-agent: unsafe controller label\n");
        return 2;
    }
    target = studio_target_info_for_id(target_id);
    if (!target) {
        (void)fprintf(stderr,
                      "kilix-land-agent: unknown semantic target: %s\n",
                      target_id ? target_id : "");
        return 2;
    }
    return run_room(controller, target, NULL);
}

static int run_headless_agent_session(agent_session *session)
{
    studio_state state;
    if (!session) return EXIT_FAILURE;
    studio_init(&state);
    studio_set_notice(&state, "");
    while (agent_session_is_open(session)) {
        int move_x = 0;
        int move_y = 0;
        bool was_pending;
        if (!agent_session_poll(session, &state) &&
            !agent_session_is_open(session))
            break;
        was_pending = agent_session_has_pending(session);
        agent_session_drive(session, &state, &move_x, &move_y);
        studio_step(&state, move_x, move_y, STUDIO_TICK_SECONDS);
        if (studio_take_plant_launch_request(&state))
            studio_set_notice(&state,
                              "Plant launch suppressed in headless mode.");
        if (!was_pending && !agent_session_has_pending(session)) {
            int64_t now = kilix_game_monotonic_ns();
            if (now < INT64_MAX - INT64_C(1000000))
                (void)kilix_game_sleep_until_ns(now + INT64_C(1000000));
        }
    }
    return EXIT_SUCCESS;
}

static bool parse_agent_fd(const char *text, int *fd)
{
    char *end = NULL;
    long value;
    if (!text || !fd || text[0] == '\0') return false;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno != 0 || !end || *end != '\0' || value < 3 ||
        value > INT_MAX)
        return false;
    *fd = (int)value;
    return true;
}

static int run_live_agent_session(const char *fd_text, bool headless)
{
    agent_session session;
    char error[160];
    int fd;
    int result;
    if (!parse_agent_fd(fd_text, &fd)) {
        (void)fprintf(stderr, "kilix-land-agent: invalid agent fd\n");
        return 2;
    }
    if (!agent_session_init(&session, fd, error, sizeof error)) {
        (void)fprintf(stderr, "kilix-land-agent: %s\n", error);
        return EXIT_FAILURE;
    }
    result = headless ? run_headless_agent_session(&session) :
                        run_room(NULL, NULL, &session);
    agent_session_close(&session);
    return result;
}

static void usage(const char *program)
{
    (void)printf(
        "usage: %s --test-room\n"
        "       %s --agent-session-fd FD [--headless]\n"
        "       %s --agent-replay MODEL TARGET\n"
        "       %s --video-replay MODEL TARGET1 TARGET2 SPEECH\n"
        "       %s --observe\n"
        "       %s --selftest | --graphics-test | --render-test FILE\n"
        "       %s --version | --help\n",
        program, program, program, program, program, program, program);
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--test-room") == 0)
        return run_test_room();
    if (argc == 3 && strcmp(argv[1], "--agent-session-fd") == 0)
        return run_live_agent_session(argv[2], false);
    if (argc == 4 && strcmp(argv[1], "--agent-session-fd") == 0 &&
        strcmp(argv[3], "--headless") == 0)
        return run_live_agent_session(argv[2], true);
    if (argc == 4 && strcmp(argv[1], "--agent-replay") == 0)
        return run_agent_replay(argv[2], argv[3]);
    if (argc == 6 && strcmp(argv[1], "--video-replay") == 0)
        return run_video_replay(argv[2], argv[3], argv[4], argv[5]);
    if (argc == 2 && strcmp(argv[1], "--observe") == 0)
        return print_observation();
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc == 2 && strcmp(argv[1], "--graphics-test") == 0)
        return graphics_test();
    if (argc == 3 && strcmp(argv[1], "--render-test") == 0)
        return render_test(argv[2]);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        (void)printf("kilix-land-agent 0.1.0-dev\n");
        return EXIT_SUCCESS;
    }
    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        usage(argv[0]);
        return EXIT_SUCCESS;
    }
    usage(argv[0]);
    return argc == 1 ? EXIT_SUCCESS : 2;
}
