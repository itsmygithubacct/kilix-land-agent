#ifndef KILIX_LAND_AGENT_PROTOCOL_H
#define KILIX_LAND_AGENT_PROTOCOL_H

#include "studio.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENT_PROTOCOL_MESSAGE_CAPACITY 4096
#define AGENT_PROTOCOL_ID_CAPACITY 33
#define AGENT_PROTOCOL_TEXT_CAPACITY 513
#define AGENT_PROTOCOL_TIMEOUT_MAX_MS 30000u

typedef enum agent_action_kind {
    AGENT_ACTION_INVALID = 0,
    AGENT_ACTION_OBSERVE,
    AGENT_ACTION_GO_TO,
    AGENT_ACTION_INTERACT,
    AGENT_ACTION_FACE_USER,
    AGENT_ACTION_SAY,
    AGENT_ACTION_CANCEL
} agent_action_kind;

typedef struct agent_action_request {
    agent_action_kind action;
    char session_id[AGENT_PROTOCOL_ID_CAPACITY];
    char action_id[AGENT_PROTOCOL_ID_CAPACITY];
    char target_id[AGENT_PROTOCOL_ID_CAPACITY];
    char cancel_action_id[AGENT_PROTOCOL_ID_CAPACITY];
    char text[AGENT_PROTOCOL_TEXT_CAPACITY];
    uint64_t expected_revision;
    unsigned int timeout_ms;
} agent_action_request;

bool agent_protocol_parse_request(const char *message, size_t length,
                                  agent_action_request *request,
                                  char *error_code, size_t error_code_size);

bool agent_protocol_observation(char *output, size_t output_size,
                                const agent_action_request *request,
                                const studio_state *state);

bool agent_protocol_result(char *output, size_t output_size,
                           const agent_action_request *request,
                           const studio_state *state, const char *status,
                           const char *code, const char *result,
                           const char *target_id);

bool agent_protocol_generic_error(char *output, size_t output_size,
                                  const char *code, uint64_t revision);

bool agent_protocol_selftest(void);

#endif
