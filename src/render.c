#include "render.h"

#include <stdio.h>
#include <string.h>

#define COLOR_INK UINT32_C(0xf8ead0)
#define COLOR_MUTED UINT32_C(0xb8b2a6)
#define COLOR_PANEL UINT32_C(0x080d18)
#define COLOR_GOLD UINT32_C(0xf0b85a)
#define COLOR_GREEN UINT32_C(0x73e09a)
#define COLOR_BLUE UINT32_C(0x72c7ff)
#define PLAYER_RENDER_SIZE 64
#define CHAT_PANEL_Y 166.0f
#define CHAT_INPUT_Y 243.0f
#define CHAT_PANEL_ALPHA 0.42f
#define CHAT_TRANSCRIPT_ALPHA 0.72f
#define CHAT_INPUT_ALPHA 0.28f

static int text_scale(const ki_td_view *view)
{
    return view->scale >= 1.85f ? 2 : 1;
}

static void text_at(sr_canvas *canvas, const ki_td_view *view,
                    float x, float y, const char *text, uint32_t color)
{
    sr_text_shadow(canvas, (float)ki_td_screen_x(view, x),
                   (float)ki_td_screen_y(view, y), text, color, 1.0f,
                   text_scale(view));
}

static void chat_text_at(sr_canvas *canvas, const ki_td_view *view,
                         float x, float y, const char *text, uint32_t color,
                         int scale)
{
    sr_text_shadow(canvas, (float)ki_td_screen_x(view, x),
                   (float)ki_td_screen_y(view, y), text, color, 1.0f, scale);
}

static bool sprite_anchor(const ki_td_rgba8 *image, float *anchor_x,
                          float *anchor_y)
{
    uint64_t x_total = 0u;
    uint64_t pixels = 0u;
    int bottom = -1;
    int y;
    if (!ki_td_rgba8_is_valid(image) || !anchor_x || !anchor_y)
        return false;
    for (y = 0; y < image->height; ++y) {
        const uint8_t *row = image->pixels + (size_t)y * image->stride;
        int x;
        for (x = 0; x < image->width; ++x) {
            if (row[(size_t)x * 4u + 3u] < UINT8_C(8)) continue;
            if (y > bottom) bottom = y;
            x_total += (uint64_t)(unsigned int)x;
            pixels++;
        }
    }
    if (bottom < 0 || pixels == 0u) return false;
    *anchor_x = (float)((double)x_total / (double)pixels);
    *anchor_y = (float)bottom;
    return true;
}

static void draw_player(ki_td_soft_renderer *renderer,
                        const ki_td_view *view,
                        const studio_state *state,
                        const studio_graphics *graphics)
{
    ki_td_rgba8 cell;
    float anchor_x;
    float anchor_y;
    float scale_x;
    float scale_y;
    int column;
    int row = (int)state->facing * 2;
    if (state->player_moving)
        column = ((state->simulation_tick / UINT64_C(8)) & UINT64_C(1)) != 0u ?
                 4 : 0;
    else
        column = (int)((state->simulation_tick / UINT64_C(20)) %
                       UINT64_C(4));
    if (!studio_graphics_player_cell(graphics, column, row, &cell) ||
        !sprite_anchor(&cell, &anchor_x, &anchor_y))
        return;
    ki_td_soft_fill_ellipse(renderer, view, state->player_x, state->player_y,
                            17.0f, 5.0f, UINT32_C(0x02040a), 0.56f);
    scale_x = (float)PLAYER_RENDER_SIZE / (float)cell.width;
    scale_y = (float)PLAYER_RENDER_SIZE / (float)cell.height;
    ki_td_soft_rgba_resized(renderer, view,
                            state->player_x - anchor_x * scale_x,
                            state->player_y - anchor_y * scale_y,
                            &cell, PLAYER_RENDER_SIZE,
                            PLAYER_RENDER_SIZE, 1.0f);
}

static void draw_target_hint(ki_td_soft_renderer *renderer,
                             const ki_td_view *view,
                             const studio_state *state,
                             sr_canvas *canvas)
{
    const studio_target_info *target =
        studio_target_info_for(state->nearest_target);
    char prompt[128];
    float pulse;
    if (!target) return;
    pulse = 5.0f +
            (float)(state->simulation_tick % UINT64_C(24)) / 24.0f * 2.0f;
    ki_td_soft_fill_ellipse(renderer, view, target->x, target->y,
                            pulse, pulse * 0.42f,
                            target->target == STUDIO_TARGET_PLANT ?
                            COLOR_GREEN : COLOR_GOLD, 0.46f);
    ki_td_soft_fill_rect(renderer, view, 94.0f, 238.0f, 292.0f, 24.0f,
                         COLOR_PANEL, 0.88f);
    (void)snprintf(prompt, sizeof prompt, "ENTER / SPACE  USE %s",
                   target->label);
    text_at(canvas, view, 111.0f, 246.0f, prompt,
            target->target == STUDIO_TARGET_PLANT ? COLOR_GREEN : COLOR_GOLD);
}

static int chat_text_scale(const ki_td_view *view)
{
    return view->scale >= 3.5f ? 2 : 1;
}

static size_t chat_columns_between(const ki_td_view *view, float left,
                                   float right, int scale)
{
    int screen_left = ki_td_screen_x(view, left);
    int screen_right = ki_td_screen_x(view, right);
    int advance = SR_FONT_W * scale;
    size_t columns;
    if (screen_right <= screen_left || advance <= 0) return 1u;
    columns = (size_t)(screen_right - screen_left) / (size_t)advance;
    if (columns == 0u) columns = 1u;
    if (columns > AGENT_PROTOCOL_TEXT_CAPACITY + 16u)
        columns = AGENT_PROTOCOL_TEXT_CAPACITY + 16u;
    return columns;
}

static void chat_segment(char *output, size_t output_size,
                         const char *prefix, const char *text,
                         size_t offset, size_t columns, bool ellipsize)
{
    size_t prefix_length;
    size_t text_length;
    size_t available;
    size_t copied;
    bool clipped;
    if (!output || output_size == 0u || columns == 0u) return;
    if (!prefix) prefix = "";
    if (!text) text = "";
    prefix_length = strlen(prefix);
    if (prefix_length > columns) prefix_length = columns;
    if (prefix_length >= output_size) prefix_length = output_size - 1u;
    (void)memcpy(output, prefix, prefix_length);
    output[prefix_length] = '\0';
    text_length = strlen(text);
    available = columns - prefix_length;
    if (available > output_size - prefix_length - 1u)
        available = output_size - prefix_length - 1u;
    copied = offset < text_length ? text_length - offset : 0u;
    clipped = copied > available;
    if (copied > available) copied = available;
    if (copied > 0u) {
        (void)memcpy(output + prefix_length, text + offset, copied);
        output[prefix_length + copied] = '\0';
    }
    if (ellipsize && clipped && copied >= 3u) {
        output[prefix_length + copied - 3u] = '.';
        output[prefix_length + copied - 2u] = '.';
        output[prefix_length + copied - 1u] = '.';
    }
}

static size_t chat_wrapped_segment(char *output, size_t output_size,
                                   const char *prefix, const char *text,
                                   size_t offset, size_t columns,
                                   bool final_line)
{
    size_t prefix_length;
    size_t text_length;
    size_t available;
    size_t take;
    size_t scan;
    size_t copied;
    bool clipped;
    if (!output || output_size == 0u || columns == 0u) return offset;
    if (!prefix) prefix = "";
    if (!text) text = "";
    text_length = strlen(text);
    while (offset < text_length && text[offset] == ' ') offset++;
    prefix_length = strlen(prefix);
    if (prefix_length > columns) prefix_length = columns;
    if (prefix_length >= output_size) prefix_length = output_size - 1u;
    (void)memcpy(output, prefix, prefix_length);
    output[prefix_length] = '\0';
    available = columns - prefix_length;
    if (available > output_size - prefix_length - 1u)
        available = output_size - prefix_length - 1u;
    if (offset >= text_length || available == 0u) return text_length;
    take = text_length - offset;
    clipped = take > available;
    if (take > available) take = available;
    if (clipped && !final_line) {
        for (scan = take; scan > 0u; --scan) {
            if (text[offset + scan - 1u] == ' ' &&
                scan - 1u >= take / 2u) {
                take = scan - 1u;
                break;
            }
        }
    }
    while (take > 0u && text[offset + take - 1u] == ' ') take--;
    copied = take;
    if (copied > 0u)
        (void)memcpy(output + prefix_length, text + offset, copied);
    output[prefix_length + copied] = '\0';
    if (final_line && clipped && copied >= 3u) {
        output[prefix_length + copied - 3u] = '.';
        output[prefix_length + copied - 2u] = '.';
        output[prefix_length + copied - 1u] = '.';
        return text_length;
    }
    offset += take;
    while (offset < text_length && text[offset] == ' ') offset++;
    return offset;
}

static void draw_chat(ki_td_soft_renderer *renderer,
                      const ki_td_view *view,
                      const studio_state *state,
                      const agent_chat_view *chat,
                      sr_canvas *canvas)
{
    char line[AGENT_PROTOCOL_TEXT_CAPACITY + 32u];
    const char *input;
    const char *status;
    size_t columns;
    size_t composer_columns;
    size_t line_budget;
    size_t reply_line;
    size_t reply_offset = 0u;
    size_t input_length;
    size_t input_offset = 0u;
    int scale;
    float cursor_y;
    float font_height;
    float line_step;
    float last_text_y;
    if (!chat || !chat->enabled) return;
    scale = chat_text_scale(view);
    columns = chat_columns_between(view, 9.0f, 471.0f, scale);
    composer_columns = chat_columns_between(view, 13.0f, 467.0f, scale);
    font_height = (float)(SR_FONT_H * scale) / view->scale;
    line_step = (float)(SR_FONT_H * scale + 2) / view->scale;
    if (line_step < 7.0f) line_step = 7.0f;
    cursor_y = CHAT_PANEL_Y + 6.0f;
    last_text_y = CHAT_INPUT_Y - 2.0f - font_height;
    line_budget = last_text_y >= cursor_y ?
                  (size_t)((last_text_y - cursor_y) / line_step) + 1u : 0u;

    ki_td_soft_fill_rect(renderer, view, 0.0f, CHAT_PANEL_Y, 480.0f,
                         270.0f - CHAT_PANEL_Y, COLOR_PANEL,
                         CHAT_PANEL_ALPHA);
    /* Keep the scene visible around the panel while giving transcript text a
     * stable contrast surface.  Without this second, bounded layer, Kilix and
     * transient room notices remain legible through the copy and make long
     * replies look overprinted. */
    ki_td_soft_fill_rect(renderer, view, 7.0f, CHAT_PANEL_Y + 4.0f,
                         466.0f, CHAT_INPUT_Y - CHAT_PANEL_Y - 7.0f,
                         COLOR_PANEL, CHAT_TRANSCRIPT_ALPHA);
    ki_td_soft_fill_rect(renderer, view, 0.0f, CHAT_PANEL_Y, 480.0f, 1.0f,
                         COLOR_BLUE, 0.30f);
    status = chat->manual_mode && state->notice_ticks > 0 &&
             state->notice[0] != '\0' ? state->notice : chat->status;
    if (line_budget > 0u) {
        chat_segment(line, sizeof line, "", status, 0u, columns,
                     true);
        chat_text_at(canvas, view, 9.0f, cursor_y, line,
                     chat->manual_mode ? COLOR_GOLD :
                     chat->busy ? COLOR_BLUE : COLOR_GREEN, scale);
        cursor_y += line_step;
        line_budget--;
    }
    if (chat->user[0] != '\0' && line_budget > 0u) {
        chat_segment(line, sizeof line, "YOU: ", chat->user, 0u, columns,
                     true);
        chat_text_at(canvas, view, 9.0f, cursor_y, line, COLOR_MUTED, scale);
        cursor_y += line_step;
        line_budget--;
    }
    for (reply_line = 0u;
         chat->reply[0] != '\0' && reply_line < line_budget;
         ++reply_line) {
        const char *prefix = reply_line == 0u ? "KILIX: " : "       ";
        bool final_line = reply_line + 1u == line_budget;
        reply_offset = chat_wrapped_segment(
            line, sizeof line, prefix, chat->reply, reply_offset, columns,
            final_line);
        chat_text_at(canvas, view, 9.0f, cursor_y, line, COLOR_INK, scale);
        cursor_y += line_step;
        if (reply_offset >= strlen(chat->reply)) break;
    }
    ki_td_soft_fill_rect(renderer, view, 7.0f, CHAT_INPUT_Y, 466.0f, 20.0f,
                         UINT32_C(0x111b2d), CHAT_INPUT_ALPHA);
    input = chat->manual_mode ?
            "TAB TO RETURN TO CHAT" :
            chat->busy ? "QWEN IS WORKING..." : chat->input;
    input_length = strlen(input);
    if (!chat->manual_mode && !chat->busy && composer_columns > 5u &&
        input_length > composer_columns - 5u)
        input_offset = input_length - (composer_columns - 5u);
    chat_segment(line, sizeof line,
                 chat->manual_mode ? "" : "YOU> ", input,
                 input_offset, composer_columns, false);
    if (!chat->manual_mode && !chat->busy &&
        (state->simulation_tick / UINT64_C(30)) % UINT64_C(2) == 0u &&
        strlen(line) < composer_columns && strlen(line) + 1u < sizeof line)
        (void)strcat(line, "_");
    chat_text_at(canvas, view, 13.0f, CHAT_INPUT_Y + 6.0f, line,
                 chat->manual_mode ? COLOR_GOLD : COLOR_INK, scale);
}

static void draw_ui(ki_td_soft_renderer *renderer, const ki_td_view *view,
                    const studio_state *state,
                    const agent_chat_view *chat)
{
    sr_canvas *canvas = ki_td_soft_canvas(renderer);
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f, 480.0f, 29.0f,
                         COLOR_PANEL, 0.78f);
    text_at(canvas, view, 10.0f, 8.0f,
            chat && chat->enabled ?
            "KILIX // QWEN RESIDENT" : "KILIX // STUDIO TEST ROOM",
            COLOR_INK);
    text_at(canvas, view, 250.0f, 8.0f,
            chat && chat->enabled ?
            "TAB CHAT/MANUAL   CTRL-C LEAVES" :
            "WASD / ARROWS MOVE   Q / ESC LEAVE",
            COLOR_MUTED);
    if ((!chat || !chat->enabled) && state->notice_ticks > 0 &&
        state->notice[0] != '\0') {
        ki_td_soft_fill_rect(renderer, view, 52.0f, 207.0f,
                             376.0f, 25.0f, COLOR_PANEL, 0.88f);
        text_at(canvas, view, 64.0f, 215.0f, state->notice, COLOR_INK);
    }
    if (!chat || !chat->enabled)
        draw_target_hint(renderer, view, state, canvas);
    draw_chat(renderer, view, state, chat, canvas);
}

static void draw_agent_video_overlay(ki_td_soft_renderer *renderer,
                                     const ki_td_view *view,
                                     const char *controller,
                                     const char *action,
                                     const char *speech)
{
    sr_canvas *canvas = ki_td_soft_canvas(renderer);
    char title[96];
    char line[128];
    if (!controller || !action) return;
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f, 480.0f, 55.0f,
                         COLOR_PANEL, 0.92f);
    (void)snprintf(title, sizeof title, "KILIX // %s AGENT DEMO",
                   controller);
    text_at(canvas, view, 10.0f, 8.0f, title, COLOR_INK);
    text_at(canvas, view, 10.0f, 35.0f, action, COLOR_GREEN);
    if (!speech || speech[0] == '\0') return;
    ki_td_soft_fill_rect(renderer, view, 35.0f, 64.0f, 410.0f, 29.0f,
                         COLOR_PANEL, 0.93f);
    (void)snprintf(line, sizeof line, "KILIX: %s", speech);
    text_at(canvas, view, 49.0f, 73.0f, line, COLOR_GOLD);
}

static bool render_frame(ki_td_soft_renderer *renderer,
                         const studio_state *state,
                         const studio_graphics *graphics,
                         const char *controller,
                         const char *action,
                         const char *speech,
                         const agent_chat_view *chat)
{
    ki_td_fit_spec spec;
    ki_td_view view;
    ki_td_rgba8 room;
    if (!renderer || !state || !graphics ||
        !studio_graphics_room(graphics, &room) ||
        !ki_td_fit_spec_init(&spec, STUDIO_LOGICAL_WIDTH,
                             STUDIO_LOGICAL_HEIGHT,
                             ki_td_soft_width(renderer),
                             ki_td_soft_height(renderer)))
        return false;
    spec.scale_policy = KI_TD_SCALE_FRACTIONAL;
    spec.minimum_scale = 0.5f;
    if (!ki_td_view_fit(&view, &spec)) return false;
    ki_td_soft_clear(renderer, UINT32_C(0x05070d));
    ki_td_soft_rgba_backdrop(renderer, &view, &room, 1.0f);
    draw_player(renderer, &view, state, graphics);
    if (controller && action)
        draw_agent_video_overlay(renderer, &view, controller, action,
                                 speech);
    else
        draw_ui(renderer, &view, state, chat);
    return ki_td_soft_pack_rgba(renderer) != NULL;
}

bool studio_render(ki_td_soft_renderer *renderer,
                   const studio_state *state,
                   const studio_graphics *graphics)
{
    return render_frame(renderer, state, graphics, NULL, NULL, NULL, NULL);
}

bool studio_render_chat(ki_td_soft_renderer *renderer,
                        const studio_state *state,
                        const studio_graphics *graphics,
                        const agent_chat_view *chat)
{
    if (!chat || !chat->enabled) return false;
    return render_frame(renderer, state, graphics, NULL, NULL, NULL, chat);
}

bool studio_render_agent_video(ki_td_soft_renderer *renderer,
                               const studio_state *state,
                               const studio_graphics *graphics,
                               const char *controller,
                               const char *action,
                               const char *speech)
{
    if (!controller || !action) return false;
    return render_frame(renderer, state, graphics, controller, action,
                        speech, NULL);
}
