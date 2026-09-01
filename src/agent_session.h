#ifndef KILIX_LAND_AGENT_SESSION_H
#define KILIX_LAND_AGENT_SESSION_H

#include "agent_protocol.h"
#include "studio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_SESSION_ACTION_BUDGET 4096
#define AGENT_CHAT_INPUT_CAPACITY 257
#define AGENT_CHAT_STATUS_CAPACITY 97

typedef enum agent_navigation_phase {
    AGENT_NAVIGATE_X = 0,
    AGENT_NAVIGATE_Y,
    AGENT_NAVIGATE_ARRIVAL
} agent_navigation_phase;

typedef struct agent_chat_view {
    bool enabled;
    bool busy;
    bool manual_mode;
    char input[AGENT_CHAT_INPUT_CAPACITY];
    char user[AGENT_CHAT_INPUT_CAPACITY];
    char reply[AGENT_PROTOCOL_TEXT_CAPACITY];
    char status[AGENT_CHAT_STATUS_CAPACITY];
} agent_chat_view;

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
    bool chat_enabled;
    bool chat_busy;
    bool chat_manual_mode;
    unsigned int chat_serial;
    size_t chat_input_length;
    char chat_input[AGENT_CHAT_INPUT_CAPACITY];
    char chat_user[AGENT_CHAT_INPUT_CAPACITY];
    char chat_reply[AGENT_PROTOCOL_TEXT_CAPACITY];
    char chat_status[AGENT_CHAT_STATUS_CAPACITY];
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
void agent_session_chat_enable(agent_session *session);
bool agent_session_chat_enabled(const agent_session *session);
bool agent_session_chat_manual_mode(const agent_session *session);
void agent_session_chat_toggle_mode(agent_session *session);
bool agent_session_chat_append(agent_session *session,
                               unsigned char character);
void agent_session_chat_backspace(agent_session *session);
void agent_session_chat_clear(agent_session *session);
bool agent_session_chat_submit(agent_session *session);
void agent_session_chat_snapshot(const agent_session *session,
                                 agent_chat_view *view);
bool agent_session_selftest(void);

#endif
