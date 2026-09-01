#ifndef KILIX_LAND_AGENT_RENDER_H
#define KILIX_LAND_AGENT_RENDER_H

#include "graphics.h"
#include "agent_session.h"
#include "studio.h"

#include <stdbool.h>

bool studio_render(ki_td_soft_renderer *renderer,
                   const studio_state *state,
                   const studio_graphics *graphics);
bool studio_render_chat(ki_td_soft_renderer *renderer,
                        const studio_state *state,
                        const studio_graphics *graphics,
                        const agent_chat_view *chat);
bool studio_render_agent_video(ki_td_soft_renderer *renderer,
                               const studio_state *state,
                               const studio_graphics *graphics,
                               const char *controller,
                               const char *action,
                               const char *speech);

#endif
