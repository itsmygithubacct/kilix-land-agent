#ifndef KILIX_LAND_AGENT_SESSION_H
#define KILIX_LAND_AGENT_SESSION_H

#include "agent_protocol.h"
#include "studio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_SESSION_ACTION_BUDGET 64

typedef enum agent_navigation_phase {
    AGENT_NAVIGATE_X = 0,
    AGENT_NAVIGATE_Y,
    AGENT_NAVIGATE_ARRIVAL
} agent_navigation_phase;

typedef struct agent_session {
    int fd;
    bool open;
    bool bound;
    bool pending;
    char session_id[AGENT_PROTOCOL_ID_CAPACITY];
    char used_action_ids[AGENT_SESSION_ACTION_BUDGET]
                        [AGENT_PROTOCOL_ID_CAPACITY];
    size_t used_action_count;
    agent_action_request pending_request;
    const studio_target_info *pending_target;
    agent_navigation_phase navigation_phase;
    int64_t deadline_ns;
    float previous_x;
    float previous_y;
    unsigned int stuck_ticks;
} agent_session;

bool agent_session_init(agent_session *session, int fd,
                        char *error, size_t error_size);
void agent_session_close(agent_session *session);
bool agent_session_is_open(const agent_session *session);
bool agent_session_has_pending(const agent_session *session);

bool agent_session_poll(agent_session *session, studio_state *state);
void agent_session_drive(agent_session *session, studio_state *state,
                         int *move_x, int *move_y);
void agent_session_manual_override(agent_session *session,
                                   studio_state *state);
bool agent_session_selftest(void);

#endif
