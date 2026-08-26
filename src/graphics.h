#ifndef KILIX_LAND_AGENT_GRAPHICS_H
#define KILIX_LAND_AGENT_GRAPHICS_H

#include "kilix_assets.h"
#include "kilix_top_down.h"

#include <stdbool.h>

typedef struct studio_graphics {
    kilix_asset_image room;
    kilix_asset_image player;
    kilix_asset_atlas player_atlas;
} studio_graphics;

bool studio_graphics_init(studio_graphics *graphics, const char *asset_root,
                          char *error, size_t error_size);
void studio_graphics_shutdown(studio_graphics *graphics);
bool studio_graphics_room(const studio_graphics *graphics,
                          ki_td_rgba8 *image);
bool studio_graphics_player_cell(const studio_graphics *graphics,
                                 int column, int row,
                                 ki_td_rgba8 *image);

#endif
