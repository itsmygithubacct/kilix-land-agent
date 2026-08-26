#include "render.h"

#include <stdio.h>

#define COLOR_INK UINT32_C(0xf8ead0)
#define COLOR_MUTED UINT32_C(0xb8b2a6)
#define COLOR_PANEL UINT32_C(0x080d18)
#define COLOR_GOLD UINT32_C(0xf0b85a)
#define COLOR_GREEN UINT32_C(0x73e09a)
#define PLAYER_RENDER_SIZE 64

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

static void draw_ui(ki_td_soft_renderer *renderer, const ki_td_view *view,
                    const studio_state *state)
{
    sr_canvas *canvas = ki_td_soft_canvas(renderer);
    ki_td_soft_fill_rect(renderer, view, 0.0f, 0.0f, 480.0f, 29.0f,
                         COLOR_PANEL, 0.78f);
    text_at(canvas, view, 10.0f, 8.0f,
            "KILIX // STUDIO TEST ROOM", COLOR_INK);
    text_at(canvas, view, 250.0f, 8.0f,
            "WASD / ARROWS MOVE   Q / ESC LEAVE", COLOR_MUTED);
    if (state->notice_ticks > 0 && state->notice[0] != '\0') {
        ki_td_soft_fill_rect(renderer, view, 52.0f, 207.0f,
                             376.0f, 25.0f, COLOR_PANEL, 0.88f);
        text_at(canvas, view, 64.0f, 215.0f, state->notice, COLOR_INK);
    }
    draw_target_hint(renderer, view, state, canvas);
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
                         const char *speech)
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
        draw_ui(renderer, &view, state);
    return ki_td_soft_pack_rgba(renderer) != NULL;
}

bool studio_render(ki_td_soft_renderer *renderer,
                   const studio_state *state,
                   const studio_graphics *graphics)
{
    return render_frame(renderer, state, graphics, NULL, NULL, NULL);
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
                        speech);
}
