#define _GNU_SOURCE

#include "agent_session.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define AGENT_SESSION_POLL_BUDGET 16u
#define AGENT_SESSION_STUCK_TICKS 120u

static void chat_set_status(agent_session *session, const char *status)
{
    size_t length;
    if (!session || !session->chat_enabled) return;
    if (!status) status = "";
    length = strlen(status);
    if (length >= sizeof session->chat_status)
        length = sizeof session->chat_status - 1u;
    (void)memcpy(session->chat_status, status, length);
    session->chat_status[length] = '\0';
}

static void set_error(char *error, size_t error_size, const char *message)
{
    if (error && error_size > 0u)
        (void)snprintf(error, error_size, "%s", message ? message : "");
}

static int64_t monotonic_ns(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return INT64_MAX;
    if (now.tv_sec > INT64_MAX / INT64_C(1000000000)) return INT64_MAX;
    return (int64_t)now.tv_sec * INT64_C(1000000000) +
           (int64_t)now.tv_nsec;
}

static bool send_document(agent_session *session, const char *document)
{
    size_t length;
    ssize_t sent;
    if (!session || !session->open || !document) return false;
    length = strlen(document);
    do {
        sent = send(session->fd, document, length, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0 || (size_t)sent != length) {
        session->open = false;
        return false;
    }
    return true;
}

static bool send_generic_error(agent_session *session,
                               const studio_state *state,
                               const char *code)
{
    char document[AGENT_PROTOCOL_MESSAGE_CAPACITY];
    uint64_t revision = state ? state->revision : 0u;
    if (!agent_protocol_generic_error(document, sizeof document,
                                      code, revision))
        return false;
    return send_document(session, document);
}

static bool send_result(agent_session *session,
                        const agent_action_request *request,
                        const studio_state *state, const char *status,
                        const char *code, const char *result,
                        const char *target_id)
{
    char document[AGENT_PROTOCOL_MESSAGE_CAPACITY];
    if (!agent_protocol_result(document, sizeof document, request, state,
                               status, code, result, target_id)) {
        session->open = false;
        return false;
    }
    return send_document(session, document);
}

static bool send_observation(agent_session *session,
                             const agent_action_request *request,
                             const studio_state *state)
{
    char document[AGENT_PROTOCOL_MESSAGE_CAPACITY];
    if (!agent_protocol_observation(document, sizeof document,
                                    request, state)) {
        session->open = false;
        return false;
    }
    return send_document(session, document);
}

static bool action_id_used(const agent_session *session, const char *action_id)
{
    size_t index;
    if (!session || !action_id) return false;
    for (index = 0u; index < session->used_action_count; ++index)
        if (strcmp(session->used_action_ids[index], action_id) == 0)
            return true;
    return false;
}

static bool remember_action_id(agent_session *session, const char *action_id)
{
    if (!session || !action_id ||
        session->used_action_count >= AGENT_SESSION_ACTION_BUDGET)
        return false;
    (void)snprintf(session->used_action_ids[session->used_action_count],
                   sizeof session->used_action_ids[0], "%s", action_id);
    session->used_action_count++;
    return true;
}

static void clear_pending(agent_session *session)
{
    if (!session) return;
    session->pending = false;
    session->pending_target = NULL;
    session->deadline_ns = 0;
    session->stuck_ticks = 0u;
    (void)memset(&session->pending_request, 0,
                 sizeof session->pending_request);
}

static void finish_pending(agent_session *session, studio_state *state,
                           const char *status, const char *code,
                           const char *result)
{
    const char *target_id = session && session->pending_target ?
                            session->pending_target->id : NULL;
    if (!session || !state || !session->pending) return;
    (void)send_result(session, &session->pending_request, state, status,
                      code, result, target_id);
    clear_pending(session);
    if (session->chat_busy)
        chat_set_status(session, "QWEN: REVIEWING THE ACTION RESULT...");
}

bool agent_session_init(agent_session *session, int fd,
                        char *error, size_t error_size)
{
    int descriptor_flags;
    int socket_type = 0;
    socklen_t socket_type_size = (socklen_t)sizeof socket_type;
    struct ucred credentials;
    socklen_t credentials_size = (socklen_t)sizeof credentials;
    if (!session || fd < 3) {
        set_error(error, error_size, "agent fd must be at least 3");
        return false;
    }
    (void)memset(session, 0, sizeof *session);
    session->fd = -1;
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &socket_type,
                   &socket_type_size) != 0 ||
        socket_type != SOCK_SEQPACKET) {
        set_error(error, error_size, "agent fd is not a SOCK_SEQPACKET socket");
        return false;
    }
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials,
                   &credentials_size) != 0 || credentials.uid != getuid()) {
        set_error(error, error_size, "agent peer is not the current user");
        return false;
    }
    descriptor_flags = fcntl(fd, F_GETFD);
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        set_error(error, error_size, "could not protect agent fd");
        return false;
    }
    session->fd = fd;
    session->open = true;
    set_error(error, error_size, "");
    return true;
}

void agent_session_close(agent_session *session)
{
    if (!session) return;
    if (session->fd >= 0) (void)close(session->fd);
    session->fd = -1;
    session->open = false;
    clear_pending(session);
}

bool agent_session_is_open(const agent_session *session)
{
    return session && session->open;
}

bool agent_session_has_pending(const agent_session *session)
{
    return session && session->pending;
}

void agent_session_chat_enable(agent_session *session)
{
    if (!session) return;
    session->chat_enabled = true;
    session->chat_busy = false;
    session->chat_manual_mode = false;
    session->chat_serial = 0u;
    session->chat_input_length = 0u;
    session->chat_input[0] = '\0';
    session->chat_user[0] = '\0';
    session->chat_reply[0] = '\0';
    chat_set_status(session, "QWEN: CONNECTING TO THE PRIVATE ROOM...");
}

bool agent_session_chat_enabled(const agent_session *session)
{
    return session && session->chat_enabled;
}

bool agent_session_chat_manual_mode(const agent_session *session)
{
    return session && session->chat_enabled && session->chat_manual_mode;
}

void agent_session_chat_toggle_mode(agent_session *session)
{
    if (!session || !session->chat_enabled) return;
    session->chat_manual_mode = !session->chat_manual_mode;
    if (session->chat_manual_mode)
        chat_set_status(session,
                        "MANUAL MODE: WASD / ARROWS MOVE, TAB RETURNS TO CHAT");
    else if (session->chat_busy)
        chat_set_status(session, "QWEN: WORKING ON YOUR REQUEST...");
    else
        chat_set_status(session, "QWEN READY: TYPE A MESSAGE AND PRESS ENTER");
}

bool agent_session_chat_append(agent_session *session,
                               unsigned char character)
{
    if (!session || !session->chat_enabled || session->chat_busy ||
        session->chat_manual_mode || character < UINT8_C(32) ||
        character > UINT8_C(126) ||
        session->chat_input_length + 1u >= sizeof session->chat_input)
        return false;
    session->chat_input[session->chat_input_length++] = (char)character;
    session->chat_input[session->chat_input_length] = '\0';
    return true;
}

void agent_session_chat_backspace(agent_session *session)
{
    if (!session || !session->chat_enabled || session->chat_busy ||
        session->chat_manual_mode || session->chat_input_length == 0u)
        return;
    session->chat_input_length--;
    session->chat_input[session->chat_input_length] = '\0';
}

void agent_session_chat_clear(agent_session *session)
{
    if (!session || !session->chat_enabled || session->chat_busy ||
        session->chat_manual_mode)
        return;
    session->chat_input_length = 0u;
    session->chat_input[0] = '\0';
}

bool agent_session_chat_submit(agent_session *session)
{
    char document[AGENT_PROTOCOL_MESSAGE_CAPACITY];
    char message_id[AGENT_PROTOCOL_ID_CAPACITY];
    size_t start = 0u;
    size_t end;
    if (!session || !session->chat_enabled || session->chat_busy ||
        session->chat_manual_mode)
        return false;
    if (!session->bound) {
        chat_set_status(session, "QWEN: STILL CONNECTING; PLEASE WAIT...");
        return false;
    }
    end = session->chat_input_length;
    while (start < end && session->chat_input[start] == ' ') start++;
    while (end > start && session->chat_input[end - 1u] == ' ') end--;
    if (start == end) {
        agent_session_chat_clear(session);
        return false;
    }
    if (start > 0u)
        (void)memmove(session->chat_input, session->chat_input + start,
                      end - start);
    session->chat_input_length = end - start;
    session->chat_input[session->chat_input_length] = '\0';
    if (session->chat_serial == UINT_MAX) {
        chat_set_status(session, "CHAT MESSAGE BUDGET EXHAUSTED; RESTART ROOM");
        return false;
    }
    session->chat_serial++;
    (void)snprintf(message_id, sizeof message_id, "chat-%08u",
                   session->chat_serial);
    if (!agent_protocol_chat_input(document, sizeof document,
                                   session->session_id, message_id,
                                   session->chat_input) ||
        !send_document(session, document))
        return false;
    (void)snprintf(session->chat_user, sizeof session->chat_user, "%s",
                   session->chat_input);
    session->chat_input_length = 0u;
    session->chat_input[0] = '\0';
    session->chat_busy = true;
    chat_set_status(session, "QWEN: THINKING...");
    return true;
}

void agent_session_chat_snapshot(const agent_session *session,
                                 agent_chat_view *view)
{
    if (!view) return;
    (void)memset(view, 0, sizeof *view);
    if (!session || !session->chat_enabled) return;
    view->enabled = true;
    view->busy = session->chat_busy;
    view->manual_mode = session->chat_manual_mode;
    (void)snprintf(view->input, sizeof view->input, "%s",
                   session->chat_input);
    (void)snprintf(view->user, sizeof view->user, "%s",
                   session->chat_user);
    (void)snprintf(view->reply, sizeof view->reply, "%s",
                   session->chat_reply);
    (void)snprintf(view->status, sizeof view->status, "%s",
                   session->chat_status);
}

static bool bind_request(agent_session *session,
                         const agent_action_request *request,
                         const studio_state *state)
{
    if (!session->bound) {
        (void)snprintf(session->session_id, sizeof session->session_id,
                       "%s", request->session_id);
        session->bound = true;
        if (session->chat_enabled && !session->chat_busy)
            chat_set_status(session,
                            "QWEN READY: TYPE A MESSAGE AND PRESS ENTER");
    } else if (strcmp(session->session_id, request->session_id) != 0) {
        (void)send_result(session, request, state, "rejected",
                          "session_mismatch", NULL, NULL);
        return false;
    }
    if (action_id_used(session, request->action_id)) {
        (void)send_result(session, request, state, "rejected",
                          "duplicate_action_id", NULL, NULL);
        return false;
    }
    if (!remember_action_id(session, request->action_id)) {
        (void)send_result(session, request, state, "rejected",
                          "action_budget_exhausted", NULL, NULL);
        return false;
    }
    return true;
}

static void handle_cancel(agent_session *session,
                          const agent_action_request *request,
                          studio_state *state)
{
    if (!session->pending ||
        strcmp(request->cancel_action_id,
               session->pending_request.action_id) != 0) {
        (void)send_result(session, request, state, "rejected",
                          "no_matching_action", NULL, NULL);
        return;
    }
    finish_pending(session, state, "canceled", "user_cancelled", NULL);
    (void)send_result(session, request, state, "ok", NULL,
                      "canceled", NULL);
    studio_set_notice(state, "Agent action canceled.");
}

static void start_navigation(agent_session *session,
                             const agent_action_request *request,
                             const studio_target_info *target,
                             studio_state *state)
{
    int64_t now = monotonic_ns();
    session->pending = true;
    session->pending_request = *request;
    session->pending_target = target;
    session->navigation_phase = AGENT_NAVIGATE_X;
    session->previous_x = state->player_x;
    session->previous_y = state->player_y;
    session->stuck_ticks = 0u;
    if (session->chat_busy) {
        char status[AGENT_CHAT_STATUS_CAPACITY];
        (void)snprintf(status, sizeof status, "QWEN ACTION: go_to(%s)",
                       target->id);
        chat_set_status(session, status);
    }
    if (now == INT64_MAX ||
        now > INT64_MAX -
              (int64_t)request->timeout_ms * INT64_C(1000000))
        session->deadline_ns = INT64_MAX;
    else
        session->deadline_ns =
            now + (int64_t)request->timeout_ms * INT64_C(1000000);
    {
        char notice[STUDIO_NOTICE_CAPACITY];
        (void)snprintf(notice, sizeof notice, "Agent: walking to %s.",
                       target->label);
        studio_set_notice(state, notice);
    }
}

static void handle_request(agent_session *session,
                           const agent_action_request *request,
                           studio_state *state)
{
    const studio_target_info *target;
    if (!bind_request(session, request, state)) return;
    if (request->action == AGENT_ACTION_CANCEL) {
        handle_cancel(session, request, state);
        return;
    }
    if (session->pending) {
        (void)send_result(session, request, state, "rejected", "busy",
                          NULL, NULL);
        return;
    }
    if (request->action != AGENT_ACTION_OBSERVE &&
        request->expected_revision != state->revision) {
        (void)send_result(session, request, state, "rejected",
                          "stale_revision", NULL, NULL);
        return;
    }
    switch (request->action) {
    case AGENT_ACTION_OBSERVE:
        if (session->chat_busy)
            chat_set_status(session, "QWEN ACTION: observe_room()");
        (void)send_observation(session, request, state);
        break;
    case AGENT_ACTION_GO_TO:
        target = studio_target_info_for_id(request->target_id);
        if (!target) {
            (void)send_result(session, request, state, "rejected",
                              "unknown_target", NULL, request->target_id);
            break;
        }
        start_navigation(session, request, target, state);
        break;
    case AGENT_ACTION_INTERACT:
        if (session->chat_busy) {
            char status[AGENT_CHAT_STATUS_CAPACITY];
            (void)snprintf(status, sizeof status,
                           "QWEN ACTION: interact(%s)", request->target_id);
            chat_set_status(session, status);
        }
        target = studio_target_info_for_id(request->target_id);
        if (!target) {
            (void)send_result(session, request, state, "rejected",
                              "unknown_target", NULL, request->target_id);
        } else if (studio_nearest_target(state) != target->target) {
            (void)send_result(session, request, state, "rejected",
                              "target_not_nearby", NULL, target->id);
        } else if (!studio_interact(state)) {
            (void)send_result(session, request, state, "rejected",
                              "interaction_failed", NULL, target->id);
        } else {
            (void)send_result(session, request, state, "ok", NULL,
                              target->target == STUDIO_TARGET_PLANT ?
                              "launch_requested" : "interacted",
                              target->id);
        }
        break;
    case AGENT_ACTION_FACE_USER:
        if (session->chat_busy)
            chat_set_status(session, "QWEN ACTION: face_user()");
        state->facing = STUDIO_FACING_DOWN;
        state->revision++;
        studio_set_notice(state, "Kilix faces you.");
        (void)send_result(session, request, state, "ok", NULL,
                          "facing_user", NULL);
        break;
    case AGENT_ACTION_SAY:
        {
            char notice[STUDIO_NOTICE_CAPACITY];
            state->facing = STUDIO_FACING_DOWN;
            state->revision++;
            (void)snprintf(notice, sizeof notice, "Kilix: %.150s",
                           request->text);
            studio_set_notice(state, notice);
            (void)send_result(session, request, state, "ok", NULL,
                              "displayed", NULL);
            if (session->chat_enabled) {
                (void)snprintf(session->chat_reply,
                               sizeof session->chat_reply, "%s",
                               request->text);
                session->chat_busy = false;
                chat_set_status(
                    session,
                    session->chat_manual_mode ?
                    "MANUAL MODE: TAB RETURNS TO CHAT" :
                    "QWEN READY: TYPE A MESSAGE AND PRESS ENTER");
            }
        }
        break;
    case AGENT_ACTION_STATUS:
        if (session->chat_enabled)
            chat_set_status(session, request->text);
        (void)send_result(session, request, state, "ok", NULL,
                          "displayed", NULL);
        break;
    case AGENT_ACTION_CANCEL:
    case AGENT_ACTION_INVALID:
        (void)send_result(session, request, state, "rejected",
                          "unknown_action", NULL, NULL);
        break;
    }
}

bool agent_session_poll(agent_session *session, studio_state *state)
{
    unsigned int count;
    if (!session || !state || !session->open) return false;
    for (count = 0u; count < AGENT_SESSION_POLL_BUDGET; ++count) {
        char message[AGENT_PROTOCOL_MESSAGE_CAPACITY];
        char error[48];
        agent_action_request request;
        ssize_t received;
        do {
            received = recv(session->fd, message, sizeof message,
                            MSG_DONTWAIT | MSG_TRUNC);
        } while (received < 0 && errno == EINTR);
        if (received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
            session->open = false;
            return false;
        }
        if (received == 0) {
            session->open = false;
            return false;
        }
        if ((size_t)received >= sizeof message) {
            if (!send_generic_error(session, state, "message_too_large"))
                return false;
            continue;
        }
        if (!agent_protocol_parse_request(message, (size_t)received,
                                          &request, error, sizeof error)) {
            if (!send_generic_error(session, state, error)) return false;
            continue;
        }
        handle_request(session, &request, state);
        if (!session->open) return false;
    }
    return true;
}

void agent_session_drive(agent_session *session, studio_state *state,
                         int *move_x, int *move_y)
{
    float delta;
    bool requested_movement = false;
    if (move_x) *move_x = 0;
    if (move_y) *move_y = 0;
    if (!session || !state || !move_x || !move_y || !session->pending ||
        !session->pending_target)
        return;
    if (monotonic_ns() >= session->deadline_ns) {
        finish_pending(session, state, "timeout", "deadline_exceeded", NULL);
        studio_set_notice(state, "Agent action timed out.");
        return;
    }
    if (state->player_x == session->previous_x &&
        state->player_y == session->previous_y)
        session->stuck_ticks++;
    else
        session->stuck_ticks = 0u;
    session->previous_x = state->player_x;
    session->previous_y = state->player_y;

    if (session->navigation_phase == AGENT_NAVIGATE_X) {
        delta = session->pending_target->x - state->player_x;
        if (delta < -2.5f) {
            *move_x = -1;
            requested_movement = true;
        } else if (delta > 2.5f) {
            *move_x = 1;
            requested_movement = true;
        } else {
            session->navigation_phase = AGENT_NAVIGATE_Y;
            session->stuck_ticks = 0u;
        }
    }
    if (session->navigation_phase == AGENT_NAVIGATE_Y) {
        delta = session->pending_target->y + 18.0f - state->player_y;
        if (delta < -2.5f) {
            *move_y = -1;
            requested_movement = true;
        } else if (delta > 2.5f) {
            *move_y = 1;
            requested_movement = true;
        } else {
            session->navigation_phase = AGENT_NAVIGATE_ARRIVAL;
        }
    }
    if (session->navigation_phase == AGENT_NAVIGATE_ARRIVAL) {
        state->nearest_target = studio_nearest_target(state);
        if (state->nearest_target == session->pending_target->target) {
            char notice[STUDIO_NOTICE_CAPACITY];
            (void)snprintf(notice, sizeof notice, "Agent: arrived at %s.",
                           session->pending_target->label);
            studio_set_notice(state, notice);
            finish_pending(session, state, "ok", NULL, "arrived");
        } else {
            finish_pending(session, state, "rejected",
                           "navigation_failed", NULL);
            studio_set_notice(state, "Agent could not reach that object.");
        }
        return;
    }
    if (requested_movement &&
        session->stuck_ticks > AGENT_SESSION_STUCK_TICKS) {
        finish_pending(session, state, "rejected", "navigation_blocked", NULL);
        studio_set_notice(state, "Agent navigation was blocked.");
    }
}

void agent_session_manual_override(agent_session *session,
                                   studio_state *state)
{
    if (!session || !state || !session->pending) return;
    finish_pending(session, state, "canceled", "manual_override", NULL);
    studio_set_notice(state, "Manual control interrupted the agent.");
}

bool agent_session_selftest(void)
{
    static const char request[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"selftest-session\","
        "\"action_id\":\"selftest-walk\",\"action\":\"go_to\","
        "\"expected_revision\":0,\"timeout_ms\":5000,"
        "\"target_id\":\"bookshelf\"}";
    static const char timeout_request[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"selftest-session\","
        "\"action_id\":\"selftest-timeout\",\"action\":\"go_to\","
        "\"expected_revision\":0,\"timeout_ms\":1,"
        "\"target_id\":\"plant\"}";
    static const char face_request[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"selftest-session\","
        "\"action_id\":\"selftest-face\",\"action\":\"face_user\","
        "\"expected_revision\":0,\"timeout_ms\":1000}";
    static const char say_request[] =
        "{\"protocol\":\"kilix.land.action/v1\","
        "\"session_id\":\"selftest-session\","
        "\"action_id\":\"selftest-say\",\"action\":\"say\","
        "\"expected_revision\":1,\"timeout_ms\":1000,"
        "\"text\":\"Hello from the chat selftest.\"}";
    int sockets[2] = {-1, -1};
    agent_session session;
    studio_state state;
    char error[128];
    char response[AGENT_PROTOCOL_MESSAGE_CAPACITY];
    ssize_t received;
    struct timespec delay = {0, 2000000L};
    agent_chat_view chat;
    bool passed = false;
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0) return false;
    if (!agent_session_init(&session, sockets[0], error, sizeof error))
        goto done;
    agent_session_chat_enable(&session);
    sockets[0] = -1;
    studio_init(&state);
    if (send(sockets[1], request, sizeof request - 1u, MSG_NOSIGNAL) !=
        (ssize_t)(sizeof request - 1u))
        goto done_session;
    if (!agent_session_poll(&session, &state) ||
        !agent_session_has_pending(&session))
        goto done_session;
    agent_session_manual_override(&session, &state);
    received = recv(sockets[1], response, sizeof response - 1u, 0);
    if (received <= 0) goto done_session;
    response[(size_t)received] = '\0';
    if (strstr(response, "\"status\":\"canceled\"") == NULL ||
        strstr(response, "\"code\":\"manual_override\"") == NULL)
        goto done_session;
    if (!agent_session_chat_append(&session, (unsigned char)'H') ||
        !agent_session_chat_append(&session, (unsigned char)'i') ||
        !agent_session_chat_submit(&session))
        goto done_session;
    received = recv(sockets[1], response, sizeof response - 1u, 0);
    if (received <= 0) goto done_session;
    response[(size_t)received] = '\0';
    if (strstr(response, "\"protocol\":\"kilix.land.chat-input/v1\"") == NULL ||
        strstr(response, "\"text\":\"Hi\"") == NULL)
        goto done_session;
    agent_session_chat_snapshot(&session, &chat);
    if (!chat.enabled || !chat.busy || strcmp(chat.user, "Hi") != 0)
        goto done_session;
    if (send(sockets[1], timeout_request, sizeof timeout_request - 1u,
             MSG_NOSIGNAL) != (ssize_t)(sizeof timeout_request - 1u))
        goto done_session;
    if (!agent_session_poll(&session, &state) ||
        !agent_session_has_pending(&session))
        goto done_session;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {}
    {
        int move_x = 0;
        int move_y = 0;
        agent_session_drive(&session, &state, &move_x, &move_y);
    }
    received = recv(sockets[1], response, sizeof response - 1u, 0);
    if (received <= 0) goto done_session;
    response[(size_t)received] = '\0';
    if (strstr(response, "\"status\":\"timeout\"") == NULL ||
        strstr(response, "\"code\":\"deadline_exceeded\"") == NULL)
        goto done_session;
    if (send(sockets[1], face_request, sizeof face_request - 1u,
             MSG_NOSIGNAL) != (ssize_t)(sizeof face_request - 1u) ||
        !agent_session_poll(&session, &state))
        goto done_session;
    received = recv(sockets[1], response, sizeof response - 1u, 0);
    if (received <= 0) goto done_session;
    if (send(sockets[1], say_request, sizeof say_request - 1u,
             MSG_NOSIGNAL) != (ssize_t)(sizeof say_request - 1u) ||
        !agent_session_poll(&session, &state))
        goto done_session;
    received = recv(sockets[1], response, sizeof response - 1u, 0);
    if (received <= 0) goto done_session;
    agent_session_chat_snapshot(&session, &chat);
    if (chat.busy ||
        strcmp(chat.reply, "Hello from the chat selftest.") != 0)
        goto done_session;
    passed = true;

done_session:
    agent_session_close(&session);
done:
    if (sockets[0] >= 0) (void)close(sockets[0]);
    if (sockets[1] >= 0) (void)close(sockets[1]);
    return passed;
}
