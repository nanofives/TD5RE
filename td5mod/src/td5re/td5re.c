/**
 * td5re.c -- Master module table, global state, init/shutdown
 */

#include "td5re.h"
#include "td5_platform.h"
#include "td5_game.h"
#include "td5_physics.h"
#include "td5_track.h"
#include "td5_ai.h"
#include "td5_render.h"
#include "td5_frontend.h"
#include "td5_hud.h"
#include "td5_sound.h"
#include "td5_input.h"
#include "td5_asset.h"
#include "td5_save.h"
#include "td5_net.h"
#include "td5_camera.h"
#include "td5_vfx.h"
#include "td5_fmv.h"

#include <string.h>

/* ========================================================================
 * Global State
 * ======================================================================== */

TD5_GlobalState g_td5;

/* ========================================================================
 * Module Table (initialization order matters)
 * ======================================================================== */

const TD5_Module g_td5re_modules[] = {
    { "asset",    td5_asset_init,    td5_asset_shutdown    },
    { "save",     td5_save_init,     td5_save_shutdown     },
    { "input",    td5_input_init,    td5_input_shutdown    },
    { "sound",    td5_sound_init,    td5_sound_shutdown    },
    { "render",   td5_render_init,   td5_render_shutdown   },
    { "track",    td5_track_init,    td5_track_shutdown    },
    { "physics",  td5_physics_init,  td5_physics_shutdown  },
    { "ai",       td5_ai_init,       td5_ai_shutdown       },
    { "camera",   td5_camera_init,   td5_camera_shutdown   },
    { "vfx",      td5_vfx_init,      td5_vfx_shutdown      },
    { "hud",      td5_hud_init,      td5_hud_shutdown      },
    { "frontend", td5_frontend_init, td5_frontend_shutdown },
    { "net",      td5_net_init,      td5_net_shutdown      },
    { "fmv",      td5_fmv_init,      td5_fmv_shutdown      },
    { "game",     td5_game_init,     td5_game_shutdown     },
};

const int g_td5re_module_count = sizeof(g_td5re_modules) / sizeof(g_td5re_modules[0]);

/* ========================================================================
 * CRC-32 Implementation (standard polynomial)
 * ======================================================================== */

static uint32_t s_crc32_table[256];
static int s_crc32_table_built = 0;

static void td5_build_crc32_table(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int j = 0; j < 8; j++) {
            if (c & 1) c = 0xEDB88320u ^ (c >> 1);
            else       c >>= 1;
        }
        s_crc32_table[i] = c;
    }
    s_crc32_table_built = 1;
}

uint32_t td5_crc32(const uint8_t *data, size_t len) {
    if (!s_crc32_table_built) td5_build_crc32_table();
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc = s_crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ========================================================================
 * Master Entry Points
 * ======================================================================== */

int td5re_init(void) {
    /* Preserve render dimensions set by main.c before memset */
    int saved_w = g_td5.render_width;
    int saved_h = g_td5.render_height;
    memset(&g_td5, 0, sizeof(g_td5));

    /* Defaults */
    g_td5.render_width = (saved_w > 0) ? saved_w : 640;
    g_td5.render_height = (saved_h > 0) ? saved_h : 480;
    g_td5.gravity_constant = TD5_GRAVITY_NORMAL;
    g_td5.difficulty = TD5_DIFFICULTY_NORMAL;
    g_td5.viewport_count = 1;
    g_td5.intro_movie_pending = 0;      /* skip intro for now */
    g_td5.frontend_init_pending = 1;    /* MUST be 1 so MENU state inits resources */
    g_td5.game_state = TD5_GAMESTATE_INTRO;

    /* Initialize all modules in order */
    for (int i = 0; i < g_td5re_module_count; i++) {
        TD5_LOG_I("td5re", "Initializing module: %s", g_td5re_modules[i].name);
        if (!g_td5re_modules[i].init()) {
            TD5_LOG_E("td5re", "Failed to initialize module: %s", g_td5re_modules[i].name);
            /* Shutdown already-initialized modules in reverse */
            for (int j = i - 1; j >= 0; j--) {
                g_td5re_modules[j].shutdown();
            }
            return 0;
        }
    }

    TD5_LOG_I("td5re", "All %d modules initialized", g_td5re_module_count);
    return 1;
}

void td5re_shutdown(void) {
    /* Shutdown in reverse order */
    for (int i = g_td5re_module_count - 1; i >= 0; i--) {
        TD5_LOG_I("td5re", "Shutting down module: %s", g_td5re_modules[i].name);
        g_td5re_modules[i].shutdown();
    }
}

int td5re_frame(void) {
    return td5_game_tick();
}
