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
#include <lists/dir_list.h>
#include <lists/string_list.h>
#include <retro_dirent.h>

#include "../menu_driver.h"
#include "../../configuration.h"
#include "../../content.h"
#include "../../core_info.h"
#include "../../defaults.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/video_driver.h"
#include "../../playlist.h"
#include "../../tasks/task_content.h"
#include "../../verbosity.h"

/* Reference design height in pixels — the mockup was drawn at this size,
 * and every dimension below is a fraction of it.  Scaling to any actual
 * screen height = video_height / DOWNPLAY_REF_HEIGHT. */
#define DOWNPLAY_REF_HEIGHT 480.0f

/* Base font size at scale = 1.0.  Re-loaded whenever scale changes so
 * glyphs stay crisp instead of being scaled at draw time. */
#define DOWNPLAY_FONT_BASE_SIZE 28.0f

#define DOWNPLAY_FONT_FILE "InterTight-Bold.ttf"

#define DOWNPLAY_RECENTS_LABEL "Recently Played"

/* One Roms/<folder> that conformed to the "Display Name (core_ident)"
 * convention.  Folders without that suffix are dropped during scan, so
 * by the time a downplay_system_t exists, both fields are non-empty.
 * full_path is the absolute folder path (used to scan its contents on
 * drill-in). */
typedef struct
{
   char *display_name;
   char *core_ident;
   char *full_path;
} downplay_system_t;

/* One ROM file inside a system folder, expanded for the system view. */
typedef struct
{
   char *display_name;   /* basename minus extension */
   char *full_path;
} downplay_rom_t;

enum downplay_view
{
   DOWNPLAY_VIEW_TOP = 0,    /* recents header + system list */
   DOWNPLAY_VIEW_SYSTEM      /* drilled into one system; showing its ROMs */
};

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
   font_data_t        *font;
   font_data_t        *chrome_font;
   /* Sorted (by display_name) array of conforming Roms/ subfolders.
    * NULL when system_count == 0. */
   downplay_system_t  *systems;
   size_t              system_count;
   /* Populated only while view == DOWNPLAY_VIEW_SYSTEM. */
   downplay_rom_t     *roms;
   size_t              rom_count;
   size_t              active_system;     /* index into systems */
   size_t              top_selection;     /* saved cursor for return-to-top */
   uintptr_t           pill_cap_tex;      /* RGBA circle, rounded pill ends */
   downplay_layout_t   layout;
   /* Cursor for whichever view is active. */
   size_t              selection;
   enum downplay_view  view;
   /* Number of entries in g_defaults.content_history at last rebuild.
    * 0 hides the "Recently Played" row entirely. */
   size_t              recent_count;
   /* Cached: rows in the current view (TOP: recents header + systems;
    * SYSTEM: rom_count). */
   size_t              total_rows;
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

/* ---------- list (systems + recents) ---------- */

/* TOP-view only: row 0 is "Recently Played" iff there are recent entries. */
static bool downplay_has_recents_row(const downplay_handle_t *dp)
{
   return dp->view == DOWNPLAY_VIEW_TOP && dp->recent_count > 0;
}

static const char *downplay_row_label(const downplay_handle_t *dp, size_t row)
{
   size_t sys_idx;

   if (dp->view == DOWNPLAY_VIEW_SYSTEM)
   {
      if (dp->roms && row < dp->rom_count)
         return dp->roms[row].display_name;
      return "";
   }

   if (downplay_has_recents_row(dp))
   {
      if (row == 0)
         return DOWNPLAY_RECENTS_LABEL;
      sys_idx = row - 1;
   }
   else
      sys_idx = row;

   if (dp->systems && sys_idx < dp->system_count)
      return dp->systems[sys_idx].display_name;
   return "";
}

/* Parse a Roms/ subfolder name per the convention from PLAN.md:
 *   "Display Name (core_ident)"
 *
 * core_ident must be non-empty and contain only [a-z0-9_].  On match,
 * heap-allocates display_name and core_ident (caller frees) and returns
 * true.  Folders that don't match the pattern are silently rejected —
 * strict convention is the feature; there is no fallback. */
static bool downplay_parse_system_folder(const char *folder,
      char **display_out, char **ident_out)
{
   const char *open;
   const char *ident_start;
   size_t      folder_len;
   size_t      display_len;
   size_t      ident_len;
   size_t      i;
   char       *display;
   char       *ident;

   if (!folder)
      return false;
   folder_len = strlen(folder);
   if (folder_len < 4 || folder[folder_len - 1] != ')')
      return false;

   /* Match the LAST " (" so display names with their own parens still
    * work, e.g. "Game Boy Advance (hacks) (mgba)".  folder_len >= 4 above
    * guarantees no size_t wraparound on (folder_len - 1). */
   open = NULL;
   for (i = folder_len - 1; i > 0; i--)
   {
      if (folder[i] == '(' && folder[i - 1] == ' ')
      {
         open = folder + i;
         break;
      }
   }
   if (!open)
      return false;

   ident_start = open + 1;
   ident_len   = (folder + folder_len - 1) - ident_start;
   if (ident_len == 0)
      return false;
   for (i = 0; i < ident_len; i++)
   {
      char c = ident_start[i];
      if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
         return false;
   }

   /* Display name is everything before the " (".  open points at '(', so
    * the trailing space sits at open - 1; strip it. */
   display_len = (size_t)(open - 1 - folder);
   if (display_len == 0)
      return false;

   if (!(display = (char*)malloc(display_len + 1)))
      return false;
   if (!(ident = (char*)malloc(ident_len + 1)))
   {
      free(display);
      return false;
   }
   memcpy(display, folder, display_len);
   display[display_len] = '\0';
   memcpy(ident, ident_start, ident_len);
   ident[ident_len] = '\0';

   *display_out = display;
   *ident_out   = ident;
   return true;
}

static int downplay_system_cmp(const void *a, const void *b)
{
   const downplay_system_t *sa = (const downplay_system_t*)a;
   const downplay_system_t *sb = (const downplay_system_t*)b;
   return strcasecmp(sa->display_name, sb->display_name);
}

static void downplay_systems_free(downplay_system_t *systems, size_t count)
{
   size_t i;
   if (!systems)
      return;
   for (i = 0; i < count; i++)
   {
      free(systems[i].display_name);
      free(systems[i].core_ident);
      free(systems[i].full_path);
   }
   free(systems);
}

static void downplay_roms_free(downplay_rom_t *roms, size_t count)
{
   size_t i;
   if (!roms)
      return;
   for (i = 0; i < count; i++)
   {
      free(roms[i].display_name);
      free(roms[i].full_path);
   }
   free(roms);
}

/* True if the folder contains at least one regular file anywhere in its
 * tree.  Hides empty system folders from the launcher.  Walks via
 * retro_opendir so we can bail at the first hit without allocating a
 * full file listing — a 5000-ROM library would otherwise allocate
 * megabytes per system on every rebuild. */
static bool downplay_folder_has_content(const char *full_path)
{
   RDIR *dir;
   bool  found = false;

   if (!(dir = retro_opendir(full_path)))
      return false;

   while (retro_readdir(dir))
   {
      const char *name = retro_dirent_get_name(dir);
      if (!name || name[0] == '.')
         continue;
      if (retro_dirent_is_dir(dir, NULL))
      {
         char child[PATH_MAX_LENGTH];
         fill_pathname_join_special(child, full_path, name, sizeof(child));
         if (downplay_folder_has_content(child))
         {
            found = true;
            break;
         }
         continue;
      }
      found = true;
      break;
   }
   retro_closedir(dir);
   return found;
}

/* Scan Roms/ for conforming subfolders.  Drops files, drops folders that
 * don't match "Display Name (core_ident)", and drops folders with no
 * content inside.  Returns NULL + count==0 when nothing qualifies. */
static downplay_system_t *downplay_scan_systems(const char *content_root,
      size_t *out_count)
{
   struct string_list *raw;
   downplay_system_t  *systems = NULL;
   size_t              cap     = 0;
   size_t              count   = 0;
   size_t              i;

   *out_count = 0;
   if (!content_root || !*content_root)
      return NULL;

   raw = dir_list_new(content_root, NULL,
         true /* include_dirs */, false, false, false /* not recursive */);
   if (!raw)
      return NULL;

   for (i = 0; i < raw->size; i++)
   {
      char  base[NAME_MAX_LENGTH];
      char *display = NULL;
      char *ident   = NULL;

      if (raw->elems[i].attr.i != RARCH_DIRECTORY)
         continue;
      fill_pathname_base(base, raw->elems[i].data, sizeof(base));
      if (!downplay_parse_system_folder(base, &display, &ident))
         continue;
      if (!downplay_folder_has_content(raw->elems[i].data))
      {
         free(display);
         free(ident);
         continue;
      }

      if (count == cap)
      {
         size_t             new_cap = cap ? cap * 2 : 8;
         downplay_system_t *grown   = (downplay_system_t*)realloc(systems,
               new_cap * sizeof(*systems));
         if (!grown)
         {
            free(display);
            free(ident);
            break;
         }
         systems = grown;
         cap     = new_cap;
      }
      {
         char *full = strdup(raw->elems[i].data);
         if (!full)
         {
            free(display);
            free(ident);
            break;
         }
         systems[count].display_name = display;
         systems[count].core_ident   = ident;
         systems[count].full_path    = full;
         count++;
      }
   }
   string_list_free(raw);

   if (count > 1)
      qsort(systems, count, sizeof(*systems), downplay_system_cmp);

   *out_count = count;
   return systems;
}

static int downplay_rom_cmp(const void *a, const void *b)
{
   const downplay_rom_t *ra = (const downplay_rom_t*)a;
   const downplay_rom_t *rb = (const downplay_rom_t*)b;
   return strcasecmp(ra->display_name, rb->display_name);
}

/* Scan one system folder for ROMs.  Top-level files only (subfolders
 * deferred); .zip and other archives included since libretro cores
 * read them directly via the VFS layer. */
static downplay_rom_t *downplay_scan_roms(const char *system_path,
      size_t *out_count)
{
   struct string_list *raw;
   downplay_rom_t     *roms  = NULL;
   size_t              cap   = 0;
   size_t              count = 0;
   size_t              i;

   *out_count = 0;
   if (!system_path || !*system_path)
      return NULL;

   raw = dir_list_new(system_path, NULL,
         false /* include_dirs */, false, false, false /* not recursive */);
   if (!raw)
      return NULL;

   for (i = 0; i < raw->size; i++)
   {
      const char *full = raw->elems[i].data;
      char        base[NAME_MAX_LENGTH];
      char       *display;

      if (!full || !*full)
         continue;
      fill_pathname_base(base, full, sizeof(base));
      if (!*base || base[0] == '.')
         continue;
      path_remove_extension(base);
      if (!*base)
         continue;

      if (count == cap)
      {
         size_t          new_cap = cap ? cap * 2 : 16;
         downplay_rom_t *grown   = (downplay_rom_t*)realloc(roms,
               new_cap * sizeof(*roms));
         if (!grown)
            break;
         roms = grown;
         cap  = new_cap;
      }
      {
         char *path_dup;
         if (!(display = strdup(base)))
            break;
         if (!(path_dup = strdup(full)))
         {
            free(display);
            break;
         }
         roms[count].display_name = display;
         roms[count].full_path    = path_dup;
         count++;
      }
   }
   string_list_free(raw);

   if (count > 1)
      qsort(roms, count, sizeof(*roms), downplay_rom_cmp);

   *out_count = count;
   return roms;
}

static void downplay_recompute_total_rows(downplay_handle_t *dp)
{
   if (dp->view == DOWNPLAY_VIEW_SYSTEM)
      dp->total_rows = dp->rom_count;
   else
      dp->total_rows = (downplay_has_recents_row(dp) ? 1 : 0)
                     + dp->system_count;

   if (dp->total_rows == 0)
      dp->selection = 0;
   else if (dp->selection >= dp->total_rows)
      dp->selection = dp->total_rows - 1;
}

/* Drill into a system's ROM list.  Saves the top-level cursor so we can
 * restore it on return.  No-op when sys_idx is out of range. */
static void downplay_open_system(downplay_handle_t *dp, size_t sys_idx)
{
   if (!dp->systems || sys_idx >= dp->system_count)
      return;

   downplay_roms_free(dp->roms, dp->rom_count);
   dp->roms      = NULL;
   dp->rom_count = 0;
   dp->roms      = downplay_scan_roms(dp->systems[sys_idx].full_path,
         &dp->rom_count);

   dp->top_selection = dp->selection;
   dp->active_system = sys_idx;
   dp->view          = DOWNPLAY_VIEW_SYSTEM;
   dp->selection     = 0;
   downplay_recompute_total_rows(dp);
}

static void downplay_close_system(downplay_handle_t *dp)
{
   downplay_roms_free(dp->roms, dp->rom_count);
   dp->roms      = NULL;
   dp->rom_count = 0;
   dp->view      = DOWNPLAY_VIEW_TOP;
   dp->selection = dp->top_selection;
   downplay_recompute_total_rows(dp);
}

/* Resolve "<core_ident>_libretro" via core_info_find into a caller-owned
 * buffer.  The match keys on the filename's "core file id"; an
 * extensionless stem hits the same path.  Returns false (and leaves
 * out_path empty) if no installed core matches.  Lazy-download for the
 * missing case is plan M3.
 *
 * Copying out the path matters: core_info_t->path is owned by the global
 * core_info list, which the load-content path may reload mid-call. */
static bool downplay_resolve_core_path(const char *core_ident,
      char *out_path, size_t out_len)
{
   char         lookup[NAME_MAX_LENGTH];
   core_info_t *info = NULL;

   if (out_path && out_len)
      out_path[0] = '\0';
   if (!core_ident || !*core_ident || !out_path || !out_len)
      return false;
   snprintf(lookup, sizeof(lookup), "%s_libretro", core_ident);
   if (!core_info_find(lookup, &info) || !info || !info->path)
      return false;
   strlcpy(out_path, info->path, out_len);
   return true;
}

static void downplay_launch_rom(const char *core_ident, const char *rom_path)
{
   char               core_path[PATH_MAX_LENGTH];
   content_ctx_info_t content_info;

   if (!downplay_resolve_core_path(core_ident, core_path, sizeof(core_path)))
   {
      /* Lazy-download lands in plan M3.  For now, surface the miss to
       * the log so it's diagnosable. */
      RARCH_WARN("[Downplay] core not installed: %s\n", core_ident);
      return;
   }

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

   /* Note: PLAN.md mentions calling menu_driver_set_last_start_content for
    * state consistency, but that helper is static in menu_cbs_ok.c.  Skip
    * for now; revisit if the omission shows up as a visible bug. */
   if (!task_push_load_content_with_new_core_from_menu(
            core_path, rom_path, &content_info,
            CORE_TYPE_PLAIN, NULL, NULL))
      RARCH_ERR("[Downplay] task_push_load_content failed for '%s'\n",
            rom_path);
}

static void downplay_rebuild_lists(downplay_handle_t *dp)
{
   char        expanded[PATH_MAX_LENGTH];
   settings_t *settings   = config_get_ptr();
   const char *root       = settings ? settings->paths.directory_menu_content : NULL;

   /* If a future caller triggers rebuild while drilled in, drop ROM state
    * and snap back to TOP.  active_system would otherwise index into a
    * stale systems array. */
   if (dp->view == DOWNPLAY_VIEW_SYSTEM)
   {
      downplay_roms_free(dp->roms, dp->rom_count);
      dp->roms      = NULL;
      dp->rom_count = 0;
      dp->view      = DOWNPLAY_VIEW_TOP;
      dp->selection = 0;
   }

   /* directory_menu_content is stored verbatim — RA doesn't expand "~" or
    * ":/" prefixes for this setting (see configuration.c).  Expand here so
    * dir_list_new (POSIX opendir under the hood) can actually open it. */
   if (root && *root)
   {
      fill_pathname_expand_special(expanded, root, sizeof(expanded));
      root = expanded;
   }

   downplay_systems_free(dp->systems, dp->system_count);
   dp->systems      = NULL;
   dp->system_count = 0;
   dp->systems      = downplay_scan_systems(root, &dp->system_count);

   /* g_defaults.content_history is initialized once at boot in retroarch.c
    * and lives for the process lifetime, so reading it without locking is
    * fine — but only from the main thread.  playlist_size reads
    * playlist->size unlocked, so don't call rebuild from a task callback. */
   dp->recent_count = g_defaults.content_history
                      ? playlist_size(g_defaults.content_history) : 0;

   downplay_recompute_total_rows(dp);
}

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
      const downplay_handle_t *dp)
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

   for (i = 0; i < dp->total_rows; i++)
   {
      row_y = list_top + (int)(i * (size_t)L->row_height);
      /* Skip rows that fall below the bottom hint strip — saves draws on
       * long lists and avoids text colliding with the chrome. */
      if (row_y + L->row_height > (int)L->vid_h - L->margin_y - L->chrome_h)
         break;
      selected  = (i == dp->selection);
      txt_color = selected ? DP_TEXT_DARK : DP_TEXT_LIGHT;

      if (selected)
         downplay_draw_pill(p_disp, userdata, L, cap_tex,
               list_x, row_y, pill_w, L->row_height,
               DP_COLOR_PILL_LIGHT);

      downplay_draw_text(font, downplay_row_label(dp, i),
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
         &dp->layout, dp->pill_cap_tex, dp);

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

   /* Empty list (no recents and no system folders): swallow nav silently
    * but still allow CANCEL to log, so input plumbing is observably alive. */
   if (dp->total_rows == 0)
   {
      if (action == MENU_ACTION_CANCEL)
         RARCH_LOG("[Downplay] cancel (empty list)\n");
      return 0;
   }

   switch (action)
   {
      case MENU_ACTION_UP:
         dp->selection = (dp->selection + dp->total_rows - 1)
                       % dp->total_rows;
         return 0;
      case MENU_ACTION_DOWN:
         dp->selection = (dp->selection + 1) % dp->total_rows;
         return 0;
      case MENU_ACTION_OK:
      case MENU_ACTION_SELECT:
         if (dp->view == DOWNPLAY_VIEW_SYSTEM)
         {
            const downplay_system_t *sys = &dp->systems[dp->active_system];
            const downplay_rom_t    *rom = &dp->roms[dp->selection];
            downplay_launch_rom(sys->core_ident, rom->full_path);
            return 0;
         }
         /* TOP view */
         if (downplay_has_recents_row(dp) && dp->selection == 0)
         {
            /* Recents drill-in is plan M4. */
            RARCH_LOG("[Downplay] recents (TODO)\n");
            return 0;
         }
         {
            size_t sys_idx = downplay_has_recents_row(dp)
               ? dp->selection - 1 : dp->selection;
            downplay_open_system(dp, sys_idx);
         }
         return 0;
      case MENU_ACTION_CANCEL:
         if (dp->view == DOWNPLAY_VIEW_SYSTEM)
         {
            downplay_close_system(dp);
            return 0;
         }
         /* TOP view: nothing to back out to.  Stay put. */
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
   downplay_rebuild_lists(dp);
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
   downplay_handle_t *dp = (downplay_handle_t*)data;
   if (!dp)
      return;
   downplay_menu_context_destroy(dp);
   /* systems / roms are CPU-only state, freed here rather than in
    * context_destroy so they survive GPU context loss/reset cycles. */
   downplay_systems_free(dp->systems, dp->system_count);
   downplay_roms_free(dp->roms, dp->rom_count);
   free(dp);
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
