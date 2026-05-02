/*  Downplay - a fork of RetroArch.
 *  Copyright (C) 2026 - Downplay contributors.
 *
 *  Downplay is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Downplay is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Downplay. If not, see <http://www.gnu.org/licenses/>.
 */

/* M0 skeleton: minimal menu_ctx_driver_t plumbing — register, get selected
 * via config, paint a placeholder background with the Downplay wordmark.
 * Real list rendering lands in M1. */

#include <stdlib.h>
#include <string.h>

#include <file/file_path.h>

#include "../menu_driver.h"
#include "../../configuration.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/font_driver.h"

#define DOWNPLAY_FONT_FILE "InterTight-Bold.ttf"
#define DOWNPLAY_FONT_SIZE 32.0f

typedef struct
{
   font_data_t *font;
} downplay_handle_t;

static void *downplay_menu_init(void **userdata, bool video_is_threaded)
{
   menu_handle_t     *menu = NULL;
   downplay_handle_t *dp   = NULL;

   if (!(menu = (menu_handle_t*)calloc(1, sizeof(*menu))))
      return NULL;
   if (!(dp = (downplay_handle_t*)calloc(1, sizeof(*dp))))
   {
      free(menu);
      return NULL;
   }

   *userdata = dp;
   return menu;
}

static void downplay_menu_context_destroy(void *data)
{
   downplay_handle_t *dp = (downplay_handle_t*)data;
   if (!dp)
      return;
   if (dp->font)
   {
      font_driver_free(dp->font);
      dp->font = NULL;
   }
}

static void downplay_menu_free(void *data)
{
   if (!data)
      return;
   downplay_menu_context_destroy(data);
   free(data);
}

static void downplay_menu_context_reset(void *data, bool video_is_threaded)
{
   char fontpath[PATH_MAX_LENGTH];
   char fontdir[PATH_MAX_LENGTH];
   downplay_handle_t *dp    = (downplay_handle_t*)data;
   settings_t        *settings;
   gfx_display_t     *p_disp;

   if (!dp)
      return;

   if (dp->font)
   {
      font_driver_free(dp->font);
      dp->font = NULL;
   }

   settings = config_get_ptr();
   p_disp   = disp_get_ptr();

   /* Look for our bundled font under <assets_dir>/downplay/. Fall back to
    * the renderer's built-in font if the asset is missing - lets the menu
    * still come up on installs where assets weren't deployed yet. */
   fontpath[0] = '\0';
   if (settings && *settings->paths.directory_assets)
   {
      fill_pathname_join_special(fontdir,
            settings->paths.directory_assets, "downplay", sizeof(fontdir));
      fill_pathname_join_special(fontpath,
            fontdir, DOWNPLAY_FONT_FILE, sizeof(fontpath));
   }

   if (*fontpath)
      dp->font = gfx_display_font_file(p_disp, fontpath,
            DOWNPLAY_FONT_SIZE, video_is_threaded);
   if (!dp->font)
      dp->font = gfx_display_font_file(p_disp, NULL,
            DOWNPLAY_FONT_SIZE, video_is_threaded);
}

static void downplay_menu_frame(void *data, video_frame_info_t *video_info)
{
   static float bg[16] = {
      0.07f, 0.07f, 0.09f, 1.0f,
      0.07f, 0.07f, 0.09f, 1.0f,
      0.07f, 0.07f, 0.09f, 1.0f,
      0.07f, 0.07f, 0.09f, 1.0f,
   };
   downplay_handle_t *dp = (downplay_handle_t*)data;
   gfx_display_t     *p_disp;
   void              *userdata;
   unsigned           video_width;
   unsigned           video_height;

   if (!dp)
      return;

   p_disp       = disp_get_ptr();
   userdata     = video_info->userdata;
   video_width  = video_info->width;
   video_height = video_info->height;

   if (!p_disp)
      return;

   gfx_display_draw_quad(p_disp, userdata,
         video_width, video_height,
         0, 0, video_width, video_height,
         video_width, video_height,
         bg, NULL);

   if (dp->font)
      gfx_display_draw_text(dp->font, "Downplay",
            (float)video_width  * 0.5f,
            (float)video_height * 0.5f,
            (int)video_width, (int)video_height,
            0xFFFFFFFF, TEXT_ALIGN_CENTER,
            1.0f, false, 0.0f, false);
}

menu_ctx_driver_t menu_ctx_downplay = {
   NULL,                              /* set_texture */
   NULL,                              /* render_messagebox */
   NULL,                              /* render */
   downplay_menu_frame,
   downplay_menu_init,
   downplay_menu_free,
   downplay_menu_context_reset,
   downplay_menu_context_destroy,
   NULL,                              /* populate_entries */
   NULL,                              /* toggle */
   NULL,                              /* navigation_clear */
   NULL,                              /* navigation_decrement */
   NULL,                              /* navigation_increment */
   NULL,                              /* navigation_set */
   NULL,                              /* navigation_set_last */
   NULL,                              /* navigation_descend_alphabet */
   NULL,                              /* navigation_ascend_alphabet */
   NULL,                              /* lists_init */
   NULL,                              /* list_insert */
   NULL,                              /* list_prepend */
   NULL,                              /* list_free */
   NULL,                              /* list_clear */
   NULL,                              /* list_cache */
   NULL,                              /* list_push */
   NULL,                              /* list_get_selection */
   NULL,                              /* list_get_size */
   NULL,                              /* list_get_entry */
   NULL,                              /* list_set_selection */
   NULL,                              /* bind_init */
   NULL,                              /* load_image */
   "downplay",
   NULL,                              /* environ_cb */
   NULL,                              /* update_thumbnail_path */
   NULL,                              /* update_thumbnail_image */
   NULL,                              /* refresh_thumbnail_image */
   NULL,                              /* set_thumbnail_content */
   NULL,                              /* osk_ptr_at_pos */
   NULL,                              /* update_savestate_thumbnail_path */
   NULL,                              /* update_savestate_thumbnail_image */
   NULL,                              /* pointer_down */
   NULL,                              /* pointer_up */
   NULL                               /* entry_action */
};
