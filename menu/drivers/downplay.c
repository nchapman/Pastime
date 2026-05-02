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

/* M1: render a static MinUI-style list — header row pill, several body
 * rows, top-right status pill, bottom button-hint pills. No input wiring
 * yet (selection stays at index 0); real list data and navigation land in
 * later milestones. All sizes are derived from a single scale so the same
 * layout works at any resolution / DPI. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <file/file_path.h>
#include <formats/image.h>

#include "../menu_driver.h"
#include "../../configuration.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/video_driver.h"
#include "../../verbosity.h"

/* Reference design height in pixels — the mockup was drawn at this size,
 * and every dimension below is a fraction of it.  Scaling to any actual
 * screen height = video_height / DOWNPLAY_REF_HEIGHT. */
#define DOWNPLAY_REF_HEIGHT 480.0f

/* Base font size at scale = 1.0.  Re-loaded whenever scale changes so
 * glyphs stay crisp instead of being scaled at draw time. */
#define DOWNPLAY_FONT_BASE_SIZE 28.0f

#define DOWNPLAY_FONT_FILE "InterTight-Bold.ttf"

/* Hardcoded list for M1.  Replaced by real systems/recents in M3+. */
static const char *downplay_stub_items[] = {
   "Recently Played",
   "Game Boy",
   "Game Boy Advance",
   "Game Boy Color",
   "Genesis"
};
#define DOWNPLAY_STUB_COUNT (sizeof(downplay_stub_items) / sizeof(downplay_stub_items[0]))

typedef struct
{
   /* All values in pixels, derived from scale.  Recomputed when the
    * window size or user scale factor changes. */
   float scale;
   unsigned vid_w;
   unsigned vid_h;

   int margin_x;            /* outer horizontal padding */
   int margin_y;            /* outer vertical padding (top + bottom) */
   int row_height;          /* per-list-row vertical extent */
   int row_text_indent;     /* x-inset of text inside a row */
   int chrome_h;            /* bottom hint row height & status pill height */
   int chrome_pad_x;        /* horizontal padding inside chrome pills */
   int chrome_gap;          /* gap between badge pill and label text */

   float font_size;         /* main list font px */
   float chrome_font_size;  /* status / button-hint label font px */
} downplay_layout_t;

typedef struct
{
   font_data_t       *font;
   font_data_t       *chrome_font;
   uintptr_t          pill_cap_tex;   /* RGBA circle, used for rounded ends */
   downplay_layout_t  layout;
   size_t             selection;
} downplay_handle_t;

/* Solid colors expressed as 4×RGBA (one vertex each, flat shaded).  Kept
 * non-const because gfx_display_draw_quad signature is non-const float*
 * and some helpers (gfx_display_set_alpha) write into the alpha slots. */
static float DP_COLOR_BG[16] = {
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f
};
static float DP_COLOR_PILL_LIGHT[16] = {
   1.0f, 1.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f
};
static float DP_COLOR_PILL_DARK[16] = {
   0.18f, 0.18f, 0.18f, 1.0f,   0.18f, 0.18f, 0.18f, 1.0f,
   0.18f, 0.18f, 0.18f, 1.0f,   0.18f, 0.18f, 0.18f, 1.0f
};

/* Cap texture is generated 1:1 (no scaling) at this size, then sampled
 * by the GPU when drawn into the actual pill cap area.  Larger source
 * = smoother arc + smaller relative AA fringe = cleaner join with the
 * rect.  256 keeps the malloc tiny (256 KB) while putting the AA
 * falloff well below one output pixel at any real pill height. */
#define DOWNPLAY_CAP_TEX_DIAMETER 256

#define DP_TEXT_LIGHT   0xFFFFFFFF
#define DP_TEXT_DARK    0x000000FF
#define DP_TEXT_MUTED   0x808080FF

/* ---------- layout ---------- */

static void downplay_layout_recompute(downplay_layout_t *L,
      unsigned video_width, unsigned video_height,
      float user_scale_factor)
{
   float scale = ((float)video_height / DOWNPLAY_REF_HEIGHT) * user_scale_factor;
   if (scale < 0.5f)
      scale = 0.5f;

   L->scale            = scale;
   L->vid_w            = video_width;
   L->vid_h            = video_height;

   L->margin_x         = (int)(24.0f  * scale);
   L->margin_y         = (int)(24.0f  * scale);
   L->row_height       = (int)(48.0f  * scale);
   L->row_text_indent  = (int)(16.0f  * scale);
   L->chrome_h         = (int)(36.0f  * scale);
   L->chrome_pad_x     = (int)(12.0f  * scale);
   L->chrome_gap       = (int)(8.0f   * scale);

   L->font_size        = DOWNPLAY_FONT_BASE_SIZE * scale;
   L->chrome_font_size = (DOWNPLAY_FONT_BASE_SIZE * 0.65f) * scale;
}

static bool downplay_layout_changed(const downplay_layout_t *L,
      unsigned video_width, unsigned video_height, float user_scale_factor)
{
   float want_scale =
      ((float)video_height / DOWNPLAY_REF_HEIGHT) * user_scale_factor;
   if (want_scale < 0.5f)
      want_scale = 0.5f;
   return     L->vid_w != video_width
           || L->vid_h != video_height
           || L->scale != want_scale;
}

/* ---------- font handling ---------- */

static void downplay_resolve_font_path(char *out, size_t out_len)
{
   char fontdir[PATH_MAX_LENGTH];
   settings_t *settings = config_get_ptr();

   out[0] = '\0';
   if (!settings || !*settings->paths.directory_assets)
      return;

   fill_pathname_join_special(fontdir,
         settings->paths.directory_assets, "downplay", sizeof(fontdir));
   fill_pathname_join_special(out, fontdir, DOWNPLAY_FONT_FILE, out_len);
}

static font_data_t *downplay_load_font(gfx_display_t *p_disp,
      float size, bool is_threaded)
{
   char fontpath[PATH_MAX_LENGTH];
   font_data_t *font = NULL;

   downplay_resolve_font_path(fontpath, sizeof(fontpath));
   if (*fontpath)
      font = gfx_display_font_file(p_disp, fontpath, size, is_threaded);
   /* Fall back to the renderer's built-in font when the bundled asset
    * isn't deployed (early packaging, broken install, etc.). */
   if (!font)
      font = gfx_display_font_file(p_disp, NULL, size, is_threaded);
   return font;
}

static void downplay_release_fonts(downplay_handle_t *dp)
{
   if (dp->font)
   {
      font_driver_free(dp->font);
      dp->font = NULL;
   }
   if (dp->chrome_font)
   {
      font_driver_free(dp->chrome_font);
      dp->chrome_font = NULL;
   }
}

static void downplay_reload_fonts(downplay_handle_t *dp,
      bool is_threaded)
{
   gfx_display_t *p_disp = disp_get_ptr();
   downplay_release_fonts(dp);
   dp->font        = downplay_load_font(p_disp,
         dp->layout.font_size, is_threaded);
   dp->chrome_font = downplay_load_font(p_disp,
         dp->layout.chrome_font_size, is_threaded);
}

/* ---------- pill cap texture ---------- */

/* Build a soft-edged white circle in an RGBA32 buffer.  Inside the
 * inscribed circle, alpha=255; over a 1-source-pixel band at the arc,
 * alpha falls to 0.  At our source diameter, this band is sub-pixel
 * once scaled to typical pill heights, so the visible AA is supplied
 * by the source falloff rather than the GPU's bilinear filter, while
 * the visible extent stays close enough to the inscribed circle that
 * cap-meets-rect joins look clean. */
static uintptr_t downplay_build_cap_texture(unsigned diameter)
{
   struct texture_image ti;
   uintptr_t tex_id   = 0;
   float     radius   = (float)diameter * 0.5f;
   uint32_t *pixels;
   unsigned  x, y;

   if (!(pixels = (uint32_t*)malloc(
               (size_t)diameter * diameter * sizeof(uint32_t))))
      return 0;

   for (y = 0; y < diameter; y++)
   {
      for (x = 0; x < diameter; x++)
      {
         float   dx    = ((float)x + 0.5f) - radius;
         float   dy    = ((float)y + 0.5f) - radius;
         float   dist  = sqrtf(dx * dx + dy * dy);
         float   cover = radius - dist;
         uint8_t a;
         if (cover > 1.0f)
            cover = 1.0f;
         if (cover < 0.0f)
            cover = 0.0f;
         a = (uint8_t)(cover * 255.0f);
         /* Byte order R,G,B,A; on little-endian uint32 = 0xAABBGGRR. */
         pixels[y * diameter + x] = ((uint32_t)a << 24) | 0x00FFFFFFu;
      }
   }

   ti.pixels        = pixels;
   ti.width         = diameter;
   ti.height        = diameter;
   ti.supports_rgba = true;

   video_driver_texture_load(&ti, TEXTURE_FILTER_LINEAR, &tex_id);
   free(pixels);
   return tex_id;
}

/* ---------- low-level draw helpers ---------- */

static void downplay_draw_rect(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L,
      int x, int y, int w, int h, float *color)
{
   if (w <= 0 || h <= 0)
      return;
   gfx_display_draw_quad(p_disp, userdata,
         L->vid_w, L->vid_h,
         x, y, (unsigned)w, (unsigned)h,
         L->vid_w, L->vid_h,
         color, NULL);
}

/* Stretch a square containing a soft-edged white circle into the given
 * box, color-tinted.  Used for the rounded ends of pills.  The middle
 * rect of the pill covers the inner half of each cap, so we just draw
 * the full circle on each end and let the rect occlude the parts we
 * don't want. */
static void downplay_draw_cap(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int x, int y, int w, int h, float *color)
{
   if (w <= 0 || h <= 0 || !cap_tex)
      return;
   gfx_display_draw_quad(p_disp, userdata,
         L->vid_w, L->vid_h,
         x, y, (unsigned)w, (unsigned)h,
         L->vid_w, L->vid_h,
         color, &cap_tex);
}

/* Pill = rectangle with semicircular end-caps.  Falls back to a plain
 * rect when no cap texture is available (e.g. asset / texture upload
 * failed) so the menu still works, just with square corners. */
static void downplay_draw_pill(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int x, int y, int w, int h, float *color)
{
   int cap_w;

   if (w <= 0 || h <= 0)
      return;

   if (!cap_tex || w < h)
   {
      downplay_draw_rect(p_disp, userdata, L, x, y, w, h, color);
      return;
   }

   cap_w = h;  /* square cap == full circle when scaled */
   downplay_draw_cap(p_disp, userdata, L, cap_tex,
         x, y, cap_w, h, color);
   downplay_draw_cap(p_disp, userdata, L, cap_tex,
         x + w - cap_w, y, cap_w, h, color);
   if (w > h)
      downplay_draw_rect(p_disp, userdata, L,
            x + (cap_w / 2), y, w - cap_w, h, color);
}

static void downplay_draw_text(font_data_t *font, const char *text,
      float x, float y, const downplay_layout_t *L,
      uint32_t color, enum text_alignment align)
{
   if (!font || !text)
      return;
   gfx_display_draw_text(font, text, x, y,
         (int)L->vid_w, (int)L->vid_h,
         color, align,
         1.0f, false, 0.0f, false);
}

/* ---------- chrome (status pill, button hints) ---------- */

/* Auto-size badges to their glyph width plus padding, with a square
 * minimum so single-letter badges stay round-ish and word badges
 * like "POWER" don't clip. */
static int downplay_badge_width(font_data_t *font,
      const downplay_layout_t *L, const char *glyph)
{
   int glyph_w = font_driver_get_message_width(font, glyph,
         strlen(glyph), 1.0f);
   int min_w   = L->chrome_h;
   int badge_w = (glyph_w > 0)
                 ? glyph_w + 2 * L->chrome_pad_x
                 : min_w;
   return (badge_w < min_w) ? min_w : badge_w;
}

/* Returns the pixel width consumed by the badge. */
static int downplay_draw_button_badge(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, const downplay_layout_t *L, uintptr_t cap_tex,
      int x, int y, const char *glyph, float *pill_color,
      uint32_t glyph_color)
{
   int badge_w = downplay_badge_width(font, L, glyph);
   int text_y  = y + (L->chrome_h / 2) + (int)(L->chrome_font_size * 0.35f);

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         x, y, badge_w, L->chrome_h, pill_color);
   downplay_draw_text(font, glyph,
         (float)(x + badge_w / 2), (float)text_y,
         L, glyph_color, TEXT_ALIGN_CENTER);
   return badge_w;
}

static void downplay_draw_button_hint(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, const downplay_layout_t *L, uintptr_t cap_tex,
      int x, int y, const char *glyph, const char *label,
      float *pill_color, uint32_t glyph_color, uint32_t label_color)
{
   int badge_w;
   int label_y = y + (L->chrome_h / 2) + (int)(L->chrome_font_size * 0.35f);

   badge_w = downplay_draw_button_badge(p_disp, userdata, font, L, cap_tex,
         x, y, glyph, pill_color, glyph_color);
   downplay_draw_text(font, label,
         (float)(x + badge_w + L->chrome_gap), (float)label_y,
         L, label_color, TEXT_ALIGN_LEFT);
}

static void downplay_draw_status_pill(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, const downplay_layout_t *L, uintptr_t cap_tex)
{
   /* Placeholder battery indicator - real telemetry wires up later. */
   const char *text = "100%";
   int pill_w = L->chrome_h * 2;
   int x      = (int)L->vid_w - L->margin_x - pill_w;
   int y      = L->margin_y;
   int text_y = y + (L->chrome_h / 2) + (int)(L->chrome_font_size * 0.35f);

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         x, y, pill_w, L->chrome_h, DP_COLOR_PILL_DARK);
   downplay_draw_text(font, text,
         (float)(x + pill_w / 2), (float)text_y,
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
}

/* ---------- main list ---------- */

static void downplay_draw_list(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, const downplay_layout_t *L, uintptr_t cap_tex,
      size_t selection)
{
   size_t   i;
   /* Push the list below the status pill row so they don't collide. */
   int      list_top = L->margin_y + L->chrome_h + (int)(16.0f * L->scale);
   int      list_x   = L->margin_x;
   int      pill_w   = (int)L->vid_w - (2 * L->margin_x);
   int      row_y;
   int      text_x   = list_x + L->row_text_indent;
   int      text_y_off = (L->row_height / 2) + (int)(L->font_size * 0.35f);
   bool     selected;
   uint32_t txt_color;

   for (i = 0; i < DOWNPLAY_STUB_COUNT; i++)
   {
      selected  = (i == selection);
      row_y     = list_top + (int)(i * (size_t)L->row_height);
      txt_color = selected ? DP_TEXT_DARK : DP_TEXT_LIGHT;

      if (selected)
         downplay_draw_pill(p_disp, userdata, L, cap_tex,
               list_x, row_y, pill_w, L->row_height,
               DP_COLOR_PILL_LIGHT);

      downplay_draw_text(font, downplay_stub_items[i],
            (float)text_x, (float)(row_y + text_y_off), L,
            txt_color, TEXT_ALIGN_LEFT);
   }
}

/* ---------- frame ---------- */

static void downplay_menu_frame(void *data, video_frame_info_t *video_info)
{
   downplay_handle_t *dp = (downplay_handle_t*)data;
   gfx_display_t     *p_disp;
   void              *userdata;
   settings_t        *settings;
   float              user_scale;
   int                bottom_y;

   if (!dp)
      return;

   p_disp     = disp_get_ptr();
   userdata   = video_info->userdata;
   settings   = config_get_ptr();
   user_scale = (settings && settings->floats.menu_scale_factor > 0.0f)
                ? settings->floats.menu_scale_factor : 1.0f;

   if (!p_disp)
      return;

   /* Recompute layout (and reload fonts at new size) only when the
    * window or user scale changed.  The font reload is the expensive
    * part — purely a glyph atlas rebuild — so we don't want it every
    * frame. */
   if (downplay_layout_changed(&dp->layout,
            video_info->width, video_info->height, user_scale))
   {
      downplay_layout_recompute(&dp->layout,
            video_info->width, video_info->height, user_scale);
      downplay_reload_fonts(dp, video_driver_is_threaded());
   }

   /* Background */
   downplay_draw_rect(p_disp, userdata, &dp->layout,
         0, 0, (int)dp->layout.vid_w, (int)dp->layout.vid_h,
         DP_COLOR_BG);

   /* Top-right status pill */
   downplay_draw_status_pill(p_disp, userdata, dp->chrome_font,
         &dp->layout, dp->pill_cap_tex);

   /* Main list */
   downplay_draw_list(p_disp, userdata, dp->font,
         &dp->layout, dp->pill_cap_tex, dp->selection);

   /* Bottom hints */
   bottom_y = (int)dp->layout.vid_h - dp->layout.margin_y - dp->layout.chrome_h;
   downplay_draw_button_hint(p_disp, userdata, dp->chrome_font, &dp->layout,
         dp->pill_cap_tex,
         dp->layout.margin_x, bottom_y,
         "POWER", "SLEEP",
         DP_COLOR_PILL_LIGHT, DP_TEXT_DARK, DP_TEXT_MUTED);
   {
      /* Right-aligned hint: measure label width to position the badge. */
      const char *glyph = "A";
      const char *label = "OPEN";
      int label_w = font_driver_get_message_width(dp->chrome_font,
            label, strlen(label), 1.0f);
      int badge_w = downplay_badge_width(dp->chrome_font, &dp->layout,
            glyph);
      int total_w = badge_w + dp->layout.chrome_gap
                  + (label_w > 0 ? label_w : 0);
      int x       = (int)dp->layout.vid_w - dp->layout.margin_x - total_w;
      downplay_draw_button_hint(p_disp, userdata, dp->chrome_font, &dp->layout,
            dp->pill_cap_tex,
            x, bottom_y,
            glyph, label,
            DP_COLOR_PILL_DARK, DP_TEXT_LIGHT, DP_TEXT_LIGHT);
   }
}

/* ---------- input ---------- */

/* M2: own the navigation entirely.  We don't have a backing menu list, so
 * generic_menu_entry_action() (which assumes file_list_t-driven entries)
 * isn't useful — we mutate dp->selection directly and consume the action.
 * OK/CANCEL just log for now; real navigation targets land in M3+. */
static int downplay_entry_action(void *userdata, menu_entry_t *entry,
      size_t i, enum menu_action action)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   if (!dp)
      return -1;

   switch (action)
   {
      case MENU_ACTION_UP:
         dp->selection = (dp->selection + DOWNPLAY_STUB_COUNT - 1)
                       % DOWNPLAY_STUB_COUNT;
         return 0;
      case MENU_ACTION_DOWN:
         dp->selection = (dp->selection + 1) % DOWNPLAY_STUB_COUNT;
         return 0;
      case MENU_ACTION_OK:
      case MENU_ACTION_SELECT:
         RARCH_LOG("[Downplay] open: %s\n",
               downplay_stub_items[dp->selection]);
         return 0;
      case MENU_ACTION_CANCEL:
         RARCH_LOG("[Downplay] cancel\n");
         return 0;
      default:
         break;
   }
   /* Returning 0 = action not consumed but no error; the framework
    * iterate loop treats any non-zero return as fatal and aborts. */
   return 0;
}

/* ---------- lifecycle ---------- */

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

   dp->selection = 0;
   *userdata     = dp;
   return menu;
}

static void downplay_menu_context_destroy(void *data)
{
   downplay_handle_t *dp = (downplay_handle_t*)data;
   if (!dp)
      return;
   downplay_release_fonts(dp);
   if (dp->pill_cap_tex)
   {
      video_driver_texture_unload(&dp->pill_cap_tex);
      dp->pill_cap_tex = 0;
   }
   gfx_display_deinit_white_texture();
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
   downplay_handle_t *dp = (downplay_handle_t*)data;
   settings_t        *settings;
   float              user_scale;
   unsigned           width, height;

   if (!dp)
      return;

   /* Seed the layout from the live framebuffer size; a real layout pass
    * will run on the first frame, but we need *something* sane so
    * font_size is non-zero before downplay_reload_fonts(). */
   video_driver_get_output_size(&width, &height);
   settings   = config_get_ptr();
   user_scale = (settings && settings->floats.menu_scale_factor > 0.0f)
                ? settings->floats.menu_scale_factor : 1.0f;
   downplay_layout_recompute(&dp->layout, width, height, user_scale);

   gfx_display_init_white_texture();
   if (!dp->pill_cap_tex)
      dp->pill_cap_tex = downplay_build_cap_texture(DOWNPLAY_CAP_TEX_DIAMETER);
   downplay_reload_fonts(dp, video_is_threaded);
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
   downplay_entry_action
};
