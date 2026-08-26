#include "agent_protocol.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct json_cursor {
    const char *data;
    size_t length;
    size_t position;
} json_cursor;

typedef struct json_output {
    char *data;
    size_t capacity;
    size_t length;
    bool valid;
} json_output;

enum request_field {
    FIELD_PROTOCOL = UINT16_C(1) << 0,
    FIELD_SESSION_ID = UINT16_C(1) << 1,
    FIELD_ACTION_ID = UINT16_C(1) << 2,
    FIELD_ACTION = UINT16_C(1) << 3,
    FIELD_EXPECTED_REVISION = UINT16_C(1) << 4,
    FIELD_TIMEOUT_MS = UINT16_C(1) << 5,
    FIELD_TARGET_ID = UINT16_C(1) << 6,
    FIELD_TEXT = UINT16_C(1) << 7,
    FIELD_CANCEL_ACTION_ID = UINT16_C(1) << 8
};

#define COMMON_FIELDS (FIELD_PROTOCOL | FIELD_SESSION_ID | FIELD_ACTION_ID | \
                       FIELD_ACTION | FIELD_EXPECTED_REVISION | \
                       FIELD_TIMEOUT_MS)

static void set_error(char *error, size_t error_size, const char *code)
{
    if (error && error_size > 0u)
        (void)snprintf(error, error_size, "%s", code ? code : "invalid_request");
}

static void skip_space(json_cursor *cursor)
{
    while (cursor && cursor->position < cursor->length &&
           (cursor->data[cursor->position] == ' ' ||
            cursor->data[cursor->position] == '\t' ||
            cursor->data[cursor->position] == '\r' ||
            cursor->data[cursor->position] == '\n'))
        ++cursor->position;
}

static bool take_character(json_cursor *cursor, char expected)
{
    skip_space(cursor);
    if (!cursor || cursor->position >= cursor->length ||
        cursor->data[cursor->position] != expected)
        return false;
    cursor->position++;
    return true;
}

static bool parse_string(json_cursor *cursor, char *output,
                         size_t output_size)
{
    size_t written = 0u;
    if (!cursor || !output || output_size == 0u ||
        !take_character(cursor, '"'))
        return false;
    while (cursor->position < cursor->length) {
        unsigned char character =
            (unsigned char)cursor->data[cursor->position++];
        if (character == (unsigned char)'"') {
            output[written] = '\0';
            return true;
        }
        if (character == (unsigned char)'\\') {
            if (cursor->position >= cursor->length) return false;
            character = (unsigned char)cursor->data[cursor->position++];
            if (character != (unsigned char)'"' &&
                character != (unsigned char)'\\' &&
                character != (unsigned char)'/')
                return false;
        }
        if (character < UINT8_C(32) || character > UINT8_C(126) ||
            written + 1u >= output_size)
            return false;
        output[written++] = (char)character;
    }
    return false;
}

static bool parse_uint64(json_cursor *cursor, uint64_t *value)
{
    uint64_t parsed = 0u;
    size_t start;
    if (!cursor || !value) return false;
    skip_space(cursor);
    start = cursor->position;
    if (start >= cursor->length ||
        cursor->data[start] < '0' || cursor->data[start] > '9')
        return false;
    if (cursor->data[start] == '0' && start + 1u < cursor->length &&
        cursor->data[start + 1u] >= '0' &&
        cursor->data[start + 1u] <= '9')
        return false;
    while (cursor->position < cursor->length) {
        unsigned int digit;
        char character = cursor->data[cursor->position];
        if (character < '0' || character > '9') break;
        digit = (unsigned int)(character - '0');
        if (parsed > (UINT64_MAX - (uint64_t)digit) / UINT64_C(10))
            return false;
        parsed = parsed * UINT64_C(10) + (uint64_t)digit;
        cursor->position++;
    }
    *value = parsed;
    return cursor->position > start;
}

static bool safe_token(const char *value)
{
    size_t index;
    if (!value || value[0] == '\0') return false;
    for (index = 0u; value[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)value[index];
        bool allowed = (character >= (unsigned char)'a' &&
                        character <= (unsigned char)'z') ||
                       (character >= (unsigned char)'A' &&
                        character <= (unsigned char)'Z') ||
                       (character >= (unsigned char)'0' &&
                        character <= (unsigned char)'9') ||
                       character == (unsigned char)'.' ||
                       character == (unsigned char)'_' ||
                       character == (unsigned char)':' ||
                       character == (unsigned char)'-';
        if (!allowed) return false;
    }
    return true;
}

static bool speech_valid(const char *text)
{
    size_t index;
    if (!text || text[0] == '\0') return false;
    for (index = 0u; text[index] != '\0'; ++index) {
        unsigned char character = (unsigned char)text[index];
        if (character < UINT8_C(32) || character > UINT8_C(126))
            return false;
    }
    return true;
}

static agent_action_kind action_for_name(const char *name)
{
    if (!name) return AGENT_ACTION_INVALID;
    if (strcmp(name, "observe") == 0) return AGENT_ACTION_OBSERVE;
    if (strcmp(name, "go_to") == 0) return AGENT_ACTION_GO_TO;
    if (strcmp(name, "interact") == 0) return AGENT_ACTION_INTERACT;
    if (strcmp(name, "face_user") == 0) return AGENT_ACTION_FACE_USER;
    if (strcmp(name, "say") == 0) return AGENT_ACTION_SAY;
    if (strcmp(name, "cancel") == 0) return AGENT_ACTION_CANCEL;
    return AGENT_ACTION_INVALID;
}

static const char *action_name(agent_action_kind action)
{
    switch (action) {
    case AGENT_ACTION_OBSERVE: return "observe";
    case AGENT_ACTION_GO_TO: return "go_to";
    case AGENT_ACTION_INTERACT: return "interact";
    case AGENT_ACTION_FACE_USER: return "face_user";
    case AGENT_ACTION_SAY: return "say";
    case AGENT_ACTION_CANCEL: return "cancel";
    case AGENT_ACTION_INVALID: break;
    }
    return "invalid";
}

static bool parse_field(json_cursor *cursor, const char *key,
                        agent_action_request *request, char *protocol,
                        size_t protocol_size, char *action,
                        size_t action_size, uint16_t *seen,
                        char *error, size_t error_size)
{
    uint16_t field = 0u;
    char *string_target = NULL;
    size_t string_size = 0u;
    uint64_t number = 0u;
    bool numeric = false;
    if (strcmp(key, "protocol") == 0) {
        field = FIELD_PROTOCOL;
        string_target = protocol;
        string_size = protocol_size;
    } else if (strcmp(key, "session_id") == 0) {
        field = FIELD_SESSION_ID;
        string_target = request->session_id;
        string_size = sizeof request->session_id;
    } else if (strcmp(key, "action_id") == 0) {
        field = FIELD_ACTION_ID;
        string_target = request->action_id;
        string_size = sizeof request->action_id;
    } else if (strcmp(key, "action") == 0) {
        field = FIELD_ACTION;
        string_target = action;
        string_size = action_size;
    } else if (strcmp(key, "expected_revision") == 0) {
        field = FIELD_EXPECTED_REVISION;
        numeric = true;
    } else if (strcmp(key, "timeout_ms") == 0) {
        field = FIELD_TIMEOUT_MS;
        numeric = true;
    } else if (strcmp(key, "target_id") == 0) {
        field = FIELD_TARGET_ID;
        string_target = request->target_id;
        string_size = sizeof request->target_id;
    } else if (strcmp(key, "text") == 0) {
        field = FIELD_TEXT;
        string_target = request->text;
        string_size = sizeof request->text;
    } else if (strcmp(key, "cancel_action_id") == 0) {
        field = FIELD_CANCEL_ACTION_ID;
        string_target = request->cancel_action_id;
        string_size = sizeof request->cancel_action_id;
    } else {
        set_error(error, error_size, "unknown_field");
        return false;
    }
    if ((*seen & field) != 0u) {
        set_error(error, error_size, "duplicate_field");
        return false;
    }
    *seen = (uint16_t)(*seen | field);
    if (numeric) {
        if (!parse_uint64(cursor, &number)) {
            set_error(error, error_size, "invalid_json");
            return false;
        }
        if (field == FIELD_EXPECTED_REVISION) {
            request->expected_revision = number;
        } else {
            if (number > (uint64_t)UINT_MAX) {
                set_error(error, error_size, "invalid_timeout");
                return false;
            }
            request->timeout_ms = (unsigned int)number;
        }
        return true;
    }
    if (!parse_string(cursor, string_target, string_size)) {
        set_error(error, error_size, "invalid_string");
        return false;
    }
    return true;
}

bool agent_protocol_parse_request(const char *message, size_t length,
                                  agent_action_request *request,
                                  char *error_code, size_t error_code_size)
{
    json_cursor cursor;
    uint16_t seen = 0u;
    uint16_t allowed;
    char protocol[40] = {0};
    char action[24] = {0};
    if (!message || !request || length == 0u ||
        length >= AGENT_PROTOCOL_MESSAGE_CAPACITY) {
        set_error(error_code, error_code_size, "message_too_large");
        return false;
    }
    (void)memset(request, 0, sizeof *request);
    cursor.data = message;
    cursor.length = length;
    cursor.position = 0u;
    if (!take_character(&cursor, '{')) {
        set_error(error_code, error_code_size, "invalid_json");
        return false;
    }
    skip_space(&cursor);
    if (cursor.position < cursor.length &&
        cursor.data[cursor.position] == '}') {
        set_error(error_code, error_code_size, "missing_field");
        return false;
    }
    for (;;) {
        char key[32];
        if (!parse_string(&cursor, key, sizeof key) ||
            !take_character(&cursor, ':')) {
            set_error(error_code, error_code_size, "invalid_json");
            return false;
        }
        if (!parse_field(&cursor, key, request, protocol, sizeof protocol,
                         action, sizeof action, &seen,
                         error_code, error_code_size))
            return false;
        skip_space(&cursor);
        if (cursor.position >= cursor.length) {
            set_error(error_code, error_code_size, "invalid_json");
            return false;
        }
        if (cursor.data[cursor.position] == '}') {
            cursor.position++;
            break;
        }
        if (cursor.data[cursor.position] != ',') {
            set_error(error_code, error_code_size, "invalid_json");
            return false;
        }
        cursor.position++;
    }
    skip_space(&cursor);
    if (cursor.position != cursor.length) {
        set_error(error_code, error_code_size, "trailing_data");
        return false;
    }
    if ((seen & COMMON_FIELDS) != COMMON_FIELDS) {
        set_error(error_code, error_code_size, "missing_field");
        return false;
    }
    if (strcmp(protocol, "kilix.land.action/v1") != 0) {
        set_error(error_code, error_code_size, "unsupported_protocol");
        return false;
    }
    if (!safe_token(request->session_id) ||
        !safe_token(request->action_id)) {
        set_error(error_code, error_code_size, "invalid_id");
        return false;
    }
    if (request->timeout_ms == 0u ||
        request->timeout_ms > AGENT_PROTOCOL_TIMEOUT_MAX_MS) {
        set_error(error_code, error_code_size, "invalid_timeout");
        return false;
    }
    request->action = action_for_name(action);
    if (request->action == AGENT_ACTION_INVALID) {
        set_error(error_code, error_code_size, "unknown_action");
        return false;
    }
    allowed = COMMON_FIELDS;
    if (request->action == AGENT_ACTION_GO_TO ||
        request->action == AGENT_ACTION_INTERACT) {
        allowed = (uint16_t)(allowed | FIELD_TARGET_ID);
        if ((seen & FIELD_TARGET_ID) == 0u ||
            !safe_token(request->target_id)) {
            set_error(error_code, error_code_size, "invalid_target");
            return false;
        }
    } else if (request->action == AGENT_ACTION_SAY) {
        allowed = (uint16_t)(allowed | FIELD_TEXT);
        if ((seen & FIELD_TEXT) == 0u || !speech_valid(request->text)) {
            set_error(error_code, error_code_size, "invalid_text");
            return false;
        }
    } else if (request->action == AGENT_ACTION_CANCEL) {
        allowed = (uint16_t)(allowed | FIELD_CANCEL_ACTION_ID);
        if ((seen & FIELD_CANCEL_ACTION_ID) == 0u ||
            !safe_token(request->cancel_action_id)) {
            set_error(error_code, error_code_size, "invalid_cancel_target");
            return false;
        }
    }
    if ((seen & (uint16_t)~allowed) != 0u) {
        set_error(error_code, error_code_size, "unexpected_field");
        return false;
    }
    set_error(error_code, error_code_size, "");
    return true;
}

static void output_append(json_output *output, const char *format, ...)
{
    int written;
    va_list arguments;
    if (!output || !output->valid || output->length >= output->capacity)
        return;
    va_start(arguments, format);
    written = vsnprintf(output->data + output->length,
                        output->capacity - output->length,
                        format, arguments);
    va_end(arguments);
    if (written < 0 || (size_t)written >= output->capacity - output->length) {
        output->valid = false;
        return;
    }
    output->length += (size_t)written;
}

static void output_string(json_output *output, const char *value)
{
    size_t index;
    output_append(output, "\"");
    if (!value) value = "";
    for (index = 0u; value[index] != '\0' && output->valid; ++index) {
        unsigned char character = (unsigned char)value[index];
        if (character == (unsigned char)'"' ||
            character == (unsigned char)'\\')
            output_append(output, "\\%c", (char)character);
        else if (character >= UINT8_C(32) && character <= UINT8_C(126))
            output_append(output, "%c", (char)character);
        else
            output->valid = false;
    }
    output_append(output, "\"");
}

static json_output output_begin(char *data, size_t capacity)
{
    json_output output;
    output.data = data;
    output.capacity = capacity;
    output.length = 0u;
    output.valid = data != NULL && capacity > 0u;
    if (output.valid) data[0] = '\0';
    return output;
}

static bool output_finish(json_output *output)
{
    if (!output || !output->valid || output->length >= output->capacity)
        return false;
    output->data[output->length] = '\0';
    return true;
}

static const char *facing_name(studio_facing facing)
{
    switch (facing) {
    case STUDIO_FACING_DOWN: return "down";
    case STUDIO_FACING_LEFT: return "left";
    case STUDIO_FACING_RIGHT: return "right";
    case STUDIO_FACING_UP: return "up";
    }
    return "invalid";
}

bool agent_protocol_observation(char *output, size_t output_size,
                                const agent_action_request *request,
                                const studio_state *state)
{
    json_output document = output_begin(output, output_size);
    size_t index;
    if (!request || !state) return false;
    output_append(&document, "{\"protocol\":\"kilix.land.observe/v1\","
                  "\"session_id\":");
    output_string(&document, request->session_id);
    output_append(&document, ",\"action_id\":");
    output_string(&document, request->action_id);
    output_append(&document,
                  ",\"revision\":%llu,\"room_id\":\"studio-apartment\","
                  "\"character_id\":\"kilix\",\"player\":{"
                  "\"x\":%.1f,\"y\":%.1f,\"facing\":",
                  (unsigned long long)state->revision,
                  (double)state->player_x, (double)state->player_y);
    output_string(&document, facing_name(state->facing));
    output_append(&document, "},\"entities\":[");
    for (index = 0u; index < studio_target_info_count(); ++index) {
        const studio_target_info *target = studio_target_info_at(index);
        if (!target) return false;
        output_append(&document, "%s{\"entity_id\":",
                      index == 0u ? "" : ",");
        output_string(&document, target->id);
        output_append(&document, ",\"label\":");
        output_string(&document, target->label);
        output_append(&document, ",\"capability_id\":");
        output_string(&document, target->capability_id);
        output_append(&document, ",\"x\":%.1f,\"y\":%.1f}",
                      (double)target->x, (double)target->y);
    }
    output_append(&document,
                  "],\"available_actions\":[\"observe\",\"go_to\","
                  "\"interact\",\"face_user\",\"say\",\"cancel\"],"
                  "\"constraints\":{"
                  "\"one_action_per_turn\":true,"
                  "\"expected_revision_required\":true}}\n");
    return output_finish(&document);
}

bool agent_protocol_result(char *output, size_t output_size,
                           const agent_action_request *request,
                           const studio_state *state, const char *status,
                           const char *code, const char *result,
                           const char *target_id)
{
    json_output document = output_begin(output, output_size);
    if (!request || !state || !status) return false;
    output_append(&document,
                  "{\"protocol\":\"kilix.land.action-result/v1\","
                  "\"session_id\":");
    output_string(&document, request->session_id);
    output_append(&document, ",\"action_id\":");
    output_string(&document, request->action_id);
    output_append(&document, ",\"action\":");
    output_string(&document, action_name(request->action));
    output_append(&document, ",\"status\":");
    output_string(&document, status);
    output_append(&document, ",\"revision\":%llu",
                  (unsigned long long)state->revision);
    if (code && code[0] != '\0') {
        output_append(&document, ",\"code\":");
        output_string(&document, code);
    }
    if (result && result[0] != '\0') {
        output_append(&document, ",\"result\":");
        output_string(&document, result);
    }
    if (target_id && target_id[0] != '\0') {
        output_append(&document, ",\"target_id\":");
        output_string(&document, target_id);
    }
    output_append(&document, "}\n");
    return output_finish(&document);
}

bool agent_protocol_generic_error(char *output, size_t output_size,
                                  const char *code, uint64_t revision)
{
    json_output document = output_begin(output, output_size);
    output_append(&document,
                  "{\"protocol\":\"kilix.land.action-result/v1\","
                  "\"session_id\":\"\",\"action_id\":\"\","
                  "\"action\":\"invalid\",\"status\":\"rejected\","
                  "\"revision\":%llu,\"code\":",
                  (unsigned long long)revision);
    output_string(&document, code ? code : "invalid_request");
    output_append(&document, "}\n");
    return output_finish(&document);
}

bool agent_protocol_selftest(void)
{
    static const char valid[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"session-1\",\"action_id\":\"action-1\","
        "\"action\":\"go_to\",\"expected_revision\":7,"
        "\"timeout_ms\":5000,\"target_id\":\"bookshelf\"}";
    static const char duplicate[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"s\",\"action_id\":\"a\","
        "\"action_id\":\"b\",\"action\":\"observe\","
        "\"expected_revision\":0,\"timeout_ms\":1000}";
    static const char extra[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"s\",\"action_id\":\"a\","
        "\"action\":\"face_user\",\"expected_revision\":0,"
        "\"timeout_ms\":1000,\"target_id\":\"plant\"}";
    agent_action_request request;
    char error[40];
    if (!agent_protocol_parse_request(valid, sizeof valid - 1u, &request,
                                      error, sizeof error) ||
        request.action != AGENT_ACTION_GO_TO ||
        request.expected_revision != UINT64_C(7) ||
        strcmp(request.target_id, "bookshelf") != 0)
        return false;
    if (agent_protocol_parse_request(duplicate, sizeof duplicate - 1u,
                                     &request, error, sizeof error) ||
        strcmp(error, "duplicate_field") != 0)
        return false;
    if (agent_protocol_parse_request(extra, sizeof extra - 1u, &request,
                                     error, sizeof error) ||
        strcmp(error, "unexpected_field") != 0)
        return false;
    return true;
}
