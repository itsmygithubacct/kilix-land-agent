#include "graphics.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define STUDIO_ROOM_PATH \
    "assets/graphics/rooms/kilix/studio-apartment.png"
#define STUDIO_PLAYER_PATH "assets/graphics/casts/kilix-player.png"
#define STUDIO_ROOM_WIDTH 1280u
#define STUDIO_ROOM_HEIGHT 720u
#define STUDIO_PLAYER_WIDTH 1024u
#define STUDIO_PLAYER_HEIGHT 512u

static bool asset_path(char *path, size_t size, const char *root,
                       const char *relative)
{
    int written;
    if (!path || size == 0u || !root || !relative) return false;
    written = snprintf(path, size, "%s/%s", root, relative);
    return written >= 0 && (size_t)written < size;
}

static void set_error(char *error, size_t error_size, const char *prefix,
                      const char *detail)
{
    if (error && error_size > 0u)
        (void)snprintf(error, error_size, "%s%s",
                       prefix ? prefix : "", detail ? detail : "");
}

bool studio_graphics_init(studio_graphics *graphics, const char *asset_root,
                          char *error, size_t error_size)
{
    kilix_asset_limits limits;
    kilix_asset_status status;
    const char *root = asset_root && asset_root[0] != '\0' ? asset_root : ".";
    char room_path[4096];
    char player_path[4096];
    if (!graphics) {
        set_error(error, error_size, "", "graphics is null");
        return false;
    }
    (void)memset(graphics, 0, sizeof *graphics);
    if (!asset_path(room_path, sizeof room_path, root, STUDIO_ROOM_PATH) ||
        !asset_path(player_path, sizeof player_path, root,
                    STUDIO_PLAYER_PATH)) {
        set_error(error, error_size, "", "asset path is too long");
        return false;
    }
    kilix_asset_limits_init(&limits);
    limits.max_file_bytes = 32u * 1024u * 1024u;
    limits.max_image_bytes = 64u * 1024u * 1024u;
    limits.max_dimension = 2048u;
    status = kilix_asset_image_load_png(&graphics->room, room_path, &limits);
    if (status != KILIX_ASSET_OK) {
        set_error(error, error_size, "room: ",
                  kilix_asset_status_string(status));
        studio_graphics_shutdown(graphics);
        return false;
    }
    status = kilix_asset_image_load_png(&graphics->player, player_path,
                                        &limits);
    if (status != KILIX_ASSET_OK) {
        set_error(error, error_size, "player: ",
                  kilix_asset_status_string(status));
        studio_graphics_shutdown(graphics);
        return false;
    }
    if (graphics->room.width != STUDIO_ROOM_WIDTH ||
        graphics->room.height != STUDIO_ROOM_HEIGHT) {
        set_error(error, error_size, "", "room must be 1280x720");
        studio_graphics_shutdown(graphics);
        return false;
    }
    if (graphics->player.width != STUDIO_PLAYER_WIDTH ||
        graphics->player.height != STUDIO_PLAYER_HEIGHT ||
        !kilix_asset_atlas_init_grid(&graphics->player_atlas,
                                     &graphics->player, 16u, 8u)) {
        set_error(error, error_size, "", "player must be a 16x8 atlas");
        studio_graphics_shutdown(graphics);
        return false;
    }
    if (error && error_size > 0u) error[0] = '\0';
    return true;
}

void studio_graphics_shutdown(studio_graphics *graphics)
{
    if (!graphics) return;
    kilix_asset_image_clear(&graphics->player);
    kilix_asset_image_clear(&graphics->room);
    (void)memset(graphics, 0, sizeof *graphics);
}

bool studio_graphics_room(const studio_graphics *graphics,
                          ki_td_rgba8 *image)
{
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image ||
        !kilix_asset_image_is_valid(&graphics->room) ||
        graphics->room.width > (uint32_t)INT_MAX ||
        graphics->room.height > (uint32_t)INT_MAX)
        return false;
    *image = ki_td_rgba8_make(graphics->room.pixels,
                              (int)graphics->room.width,
                              (int)graphics->room.height);
    image->stride = graphics->room.stride;
    return ki_td_rgba8_is_valid(image);
}

bool studio_graphics_player_cell(const studio_graphics *graphics,
                                 int column, int row,
                                 ki_td_rgba8 *image)
{
    kilix_asset_region region;
    if (image) *image = (ki_td_rgba8){0};
    if (!graphics || !image || column < 0 || row < 0) return false;
    region = kilix_asset_atlas_cell(&graphics->player_atlas,
                                    (uint32_t)column, (uint32_t)row);
    if (!kilix_asset_region_is_valid(&region) ||
        region.width > (uint32_t)INT_MAX ||
        region.height > (uint32_t)INT_MAX)
        return false;
    *image = ki_td_rgba8_make(region.pixels, (int)region.width,
                              (int)region.height);
    image->stride = region.stride;
    return ki_td_rgba8_is_valid(image);
}
