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
#include "../../command.h"
#include "../../configuration.h"
#include "../../content.h"
#include "../../core_info.h"
#include "../../defaults.h"
#include "../../gfx/gfx_display.h"
#include "../../gfx/font_driver.h"
#include "../../gfx/video_driver.h"
#include "../../paths.h"
#include "../../playlist.h"
#include "../../runloop.h"
#include "../../tasks/task_content.h"
#include "../../verbosity.h"

#include "../../downplay/downplay_cores.h"

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

/* One row in the recents view.  pl_idx is the entry's index in
 * g_defaults.content_history at build time — kept around because we
 * skip rows whose entries have neither a label nor a usable path, so
 * the array index would otherwise drift from the playlist index. */
typedef struct
{
   char  *display_name;
   size_t pl_idx;
} downplay_recent_t;

enum downplay_view
{
   DOWNPLAY_VIEW_TOP = 0,    /* recents header + system list */
   DOWNPLAY_VIEW_SYSTEM,     /* drilled into one system; showing its ROMs */
   DOWNPLAY_VIEW_RECENTS,    /* drilled into recents history */
   DOWNPLAY_VIEW_INGAME      /* core running; show Continue/Quit overlay */
};

typedef struct
{
   /* All values in pixels, derived from scale.  Recomputed when the
    * window size or user scale factor changes.
    *
    * Single-row-height invariant: every horizontal element on screen —
    * title pill, status pill, list rows, slot picker rows, settings
    * rows, the footer button-hint pill — is exactly `row_h` tall.  The
    * footer is a dark pill that visually disappears against the
    * launcher background but reads correctly when the menu overlays a
    * running game.
    *
    * Future enhancement (deferred): an auto-fit row-count algorithm
    * that picks `row_h` so an integer number of rows fills the
    * available vertical space — eliminates the trailing gap on small
    * screens.  Sketch: target_h = 56*scale, available = vid_h - 2*
    * margin_y - 2*row_h (title strip + footer); n = round(available /
    * target_h); row_h = available / n, clamped to [40,72]*scale.  Not
    * shipped yet because target devices have plenty of vertical room. */
   float scale;
   unsigned vid_w;
   unsigned vid_h;

   int margin_x;            /* outer horizontal padding */
   int margin_y;            /* outer vertical padding (top + bottom) */
   int row_h;               /* unified row height — see invariant above */
   int row_text_indent;     /* x-inset of text inside a row */
   int chrome_pad_x;        /* horizontal padding inside chrome pills */
   int chrome_gap;          /* gap between badge pill and label text */

   float font_size;         /* main list font px */
   float chrome_font_size;  /* status / button-hint label font px */
} downplay_layout_t;

typedef struct
{
   font_data_t        *font;
   font_data_t        *chrome_font;
   /* Per-font baseline-from-line-centre offsets, measured once at font
    * load via font_driver_get_line_centre_offset.  Used by
    * downplay_baseline_y to vertically centre text in a row exactly,
    * rather than the old (font_size * 0.35f) heuristic. */
   int                 font_centre_offset;
   int                 chrome_font_centre_offset;
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
   /* Cached display rows for the RECENTS view.  Populated on drill-in
    * and freed on close so we don't re-format every frame.  Each entry
    * carries its source playlist index (see downplay_recent_t). */
   downplay_recent_t  *recents;
   size_t              recent_row_count;
   /* Saved state for INGAME entry: the view we were on when the core
    * started running.  We restore exactly this on core unload — no
    * heap state copied, since SYSTEM/RECENTS resources stay live in
    * place while the game runs. */
   enum downplay_view  prior_view;
   size_t              prior_selection;
   /* Cached: rows in the current view (TOP: recents header + systems;
    * SYSTEM: rom_count). */
   size_t              total_rows;
   /* Lazy-install handoff (PLAN.md M3 Flow B): set when the user picks a
    * ROM whose core isn't installed.  The cores module takes over the
    * frame (splash), and downplay_drive_pending_launch finishes the
    * launch once it returns to DONE.  Empty core[0] means none pending.
    * Sized to PATH_MAX_LENGTH for symmetry with the rom buffer; idents
    * are short, but a single bound keeps the asymmetry from being a
    * future footgun. */
   char                pending_launch_core[PATH_MAX_LENGTH];
   char                pending_launch_rom[PATH_MAX_LENGTH];
} downplay_handle_t;

/* Solid colors expressed as 4×RGBA (one vertex each, flat shaded).  Kept
 * non-const because gfx_display_draw_quad signature is non-const float*
 * and some helpers (gfx_display_set_alpha) write into the alpha slots. */
static float DP_COLOR_BG[16] = {
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f
};
/* INGAME view dims the running game instead of fully hiding it. */
static float DP_COLOR_BG_INGAME[16] = {
   0.0f, 0.0f, 0.0f, 0.7f,   0.0f, 0.0f, 0.0f, 0.7f,
   0.0f, 0.0f, 0.0f, 0.7f,   0.0f, 0.0f, 0.0f, 0.7f
};
static float DP_COLOR_PILL_LIGHT[16] = {
   1.0f, 1.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f,
   1.0f, 1.0f, 1.0f, 1.0f,   1.0f, 1.0f, 1.0f, 1.0f
};
static float DP_COLOR_PILL_DARK[16] = {
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f,
   0.0f, 0.0f, 0.0f, 1.0f,   0.0f, 0.0f, 0.0f, 1.0f
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

   if (dp->view == DOWNPLAY_VIEW_INGAME)
   {
      switch (row)
      {
         case 0: return "Continue";
         case 1: return "Quit";
         default: return "";
      }
   }

   if (dp->view == DOWNPLAY_VIEW_SYSTEM)
   {
      if (dp->roms && row < dp->rom_count)
         return dp->roms[row].display_name;
      return "";
   }

   if (dp->view == DOWNPLAY_VIEW_RECENTS)
   {
      if (dp->recents && row < dp->recent_row_count)
         return dp->recents[row].display_name;
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
   if (dp->view == DOWNPLAY_VIEW_INGAME)
      dp->total_rows = 2;
   else if (dp->view == DOWNPLAY_VIEW_SYSTEM)
      dp->total_rows = dp->rom_count;
   else if (dp->view == DOWNPLAY_VIEW_RECENTS)
      dp->total_rows = dp->recent_row_count;
   else
      dp->total_rows = (downplay_has_recents_row(dp) ? 1 : 0)
                     + dp->system_count;

   if (dp->total_rows == 0)
      dp->selection = 0;
   else if (dp->selection >= dp->total_rows)
      dp->selection = dp->total_rows - 1;
}

static void downplay_recents_free(downplay_recent_t *rows, size_t count)
{
   size_t i;
   if (!rows)
      return;
   for (i = 0; i < count; i++)
      free(rows[i].display_name);
   free(rows);
}

/* Build display rows for the recents view.  Prefer the entry's own
 * label; fall back to the ROM filename minus extension.  Skips entries
 * with neither (corrupt history line, contentless-core stub) so they
 * can't push a blank row — pl_idx preserves the original index for
 * launch lookup. */
static downplay_recent_t *downplay_build_recents(size_t *out_count)
{
   playlist_t        *pl    = g_defaults.content_history;
   downplay_recent_t *out   = NULL;
   char               buf[NAME_MAX_LENGTH];
   size_t             total;
   size_t             count = 0;
   size_t             i;

   *out_count = 0;
   if (!pl)
      return NULL;
   total = playlist_size(pl);
   if (total == 0)
      return NULL;
   if (!(out = (downplay_recent_t*)calloc(total, sizeof(*out))))
      return NULL;

   for (i = 0; i < total; i++)
   {
      const struct playlist_entry *entry = NULL;
      const char                  *src;
      char                        *dup;

      playlist_get_index(pl, i, &entry);
      if (!entry)
         continue;
      if (entry->label && *entry->label)
         src = entry->label;
      else if (entry->path && *entry->path)
      {
         fill_pathname_base(buf, entry->path, sizeof(buf));
         path_remove_extension(buf);
         if (!*buf)
            continue;
         src = buf;
      }
      else
         continue;

      if (!(dup = strdup(src)))
         continue;
      out[count].display_name = dup;
      out[count].pl_idx       = i;
      count++;
   }

   if (count == 0)
   {
      free(out);
      return NULL;
   }
   *out_count = count;
   return out;
}

static void downplay_open_recents(downplay_handle_t *dp)
{
   downplay_recents_free(dp->recents, dp->recent_row_count);
   dp->recents          = NULL;
   dp->recent_row_count = 0;
   dp->recents          = downplay_build_recents(&dp->recent_row_count);

   dp->top_selection = dp->selection;
   dp->view          = DOWNPLAY_VIEW_RECENTS;
   dp->selection     = 0;
   downplay_recompute_total_rows(dp);
}

static void downplay_close_recents(downplay_handle_t *dp)
{
   downplay_recents_free(dp->recents, dp->recent_row_count);
   dp->recents          = NULL;
   dp->recent_row_count = 0;
   dp->view             = DOWNPLAY_VIEW_TOP;
   dp->selection        = dp->top_selection;
   downplay_recompute_total_rows(dp);
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

/* Push the actual content-load task.  Caller must have already resolved
 * core_ident to an installed core; we re-resolve here defensively. */
static void downplay_do_launch_rom(const char *core_ident, const char *rom_path)
{
   char               core_path[PATH_MAX_LENGTH];
   content_ctx_info_t content_info;

   if (!downplay_resolve_core_path(core_ident, core_path, sizeof(core_path)))
   {
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

static void downplay_clear_pending_launch(downplay_handle_t *dp)
{
   dp->pending_launch_core[0] = '\0';
   dp->pending_launch_rom[0]  = '\0';
}

/* ROM-pick entry point.  If the core is already installed, launch now.
 * Otherwise stash the pick and kick a single-ident install through the
 * cores module — the existing splash takes over rendering and
 * downplay_drive_pending_launch finishes the launch when it dismisses. */
static void downplay_launch_rom(downplay_handle_t *dp,
      const char *core_ident, const char *rom_path)
{
   const char *idents[1];

   if (downplay_cores_is_installed(core_ident))
   {
      downplay_do_launch_rom(core_ident, rom_path);
      return;
   }

   /* Defensive: input is swallowed while render mode != LIST, so the
    * user can't actually queue a second pick mid-install — but if that
    * invariant ever slips, drop the new pick rather than clobbering
    * the in-flight one. */
   if (*dp->pending_launch_core)
   {
      RARCH_WARN("[Downplay] launch already pending; ignoring second pick\n");
      return;
   }

   RARCH_LOG("[Downplay] core %s not installed; lazy install before launch\n",
         core_ident);
   strlcpy(dp->pending_launch_core, core_ident,
         sizeof(dp->pending_launch_core));
   strlcpy(dp->pending_launch_rom,  rom_path,
         sizeof(dp->pending_launch_rom));
   idents[0] = core_ident;
   downplay_cores_begin_boot_setup(idents, 1);
}


/* Launch from the recents playlist.  The entry already carries the core
 * path it last ran with, so we don't go through core_info_find — if the
 * core has since been uninstalled, the playlist task will surface that
 * itself. */
static void downplay_launch_recent(size_t index)
{
   playlist_t                  *pl    = g_defaults.content_history;
   const struct playlist_entry *entry = NULL;
   content_ctx_info_t           content_info;

   if (!pl)
      return;
   playlist_get_index(pl, index, &entry);
   if (!entry || !entry->path || !*entry->path
         || !entry->core_path || !*entry->core_path)
   {
      RARCH_WARN("[Downplay] recents entry %u missing path/core\n",
            (unsigned)index);
      return;
   }

   content_info.argc        = 0;
   content_info.argv        = NULL;
   content_info.args        = NULL;
   content_info.environ_get = NULL;

   if (!task_push_load_content_from_playlist_from_menu(
            entry->core_path, entry->path, entry->label,
            &content_info, NULL, NULL))
      RARCH_ERR("[Downplay] recents launch failed: %s\n", entry->path);
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
   else if (dp->view == DOWNPLAY_VIEW_RECENTS)
   {
      downplay_recents_free(dp->recents, dp->recent_row_count);
      dp->recents          = NULL;
      dp->recent_row_count = 0;
      dp->view             = DOWNPLAY_VIEW_TOP;
      dp->selection        = 0;
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
   L->row_h            = (int)(48.0f  * scale);
   L->row_text_indent  = (int)(16.0f  * scale);
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
   /* Snapshot the per-font centre offsets while the fonts are fresh.
    * Re-runs on every reload (i.e. when the rendered font size changes
    * with the layout). */
   dp->font_centre_offset        = dp->font
         ? font_driver_get_line_centre_offset(dp->font, 1.0f) : 0;
   dp->chrome_font_centre_offset = dp->chrome_font
         ? font_driver_get_line_centre_offset(dp->chrome_font, 1.0f) : 0;
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

/* Vertical text baseline inside a row of `height` pixels.  The font
 * driver places the baseline (not the cap line) at the y coordinate
 * it's given; `centre_offset` is the offset from the line's vertical
 * centre to the baseline, sampled from font_driver_get_line_centre_
 * offset at font load time and cached on the handle (font_centre_
 * offset / chrome_font_centre_offset).  This is exact rather than
 * heuristic — the previous (font_size * 0.35f) approximation drifted
 * by a pixel or two depending on the font. */
static int downplay_baseline_y(int top, int height, int centre_offset)
{
   return top + (height / 2) + centre_offset;
}

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

enum downplay_anchor
{
   DOWNPLAY_ANCHOR_LEFT = 0,
   DOWNPLAY_ANCHOR_RIGHT
};

/* Footer hint = outer dark pill (row_h tall, auto-sized to content)
 * containing a single light glyph badge and its white label.  The
 * inner badge is inset vertically so the outer pill reads as a
 * footer surface and the badge as a key cap sitting on it.
 *
 * `anchor_x` is the left edge for ANCHOR_LEFT, or the right edge for
 * ANCHOR_RIGHT — the function measures the content and positions the
 * outer pill accordingly.  This keeps callers from having to predict
 * the auto-sized width. */
static void downplay_draw_footer_hint(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int anchor_x, int y, enum downplay_anchor anchor,
      const char *glyph, const char *label)
{
   int badge_inset_y = (int)(6.0f * L->scale);
   /* Inner padding sits between the tight badge_inset_y (felt cramped)
    * and the loose chrome_pad_x (felt floaty) — splits the difference
    * for the "key cap on a footer surface" look. */
   int inner_pad_x   = (int)(9.0f * L->scale);
   int badge_h       = L->row_h - 2 * badge_inset_y;
   int badge_x;
   int badge_w;
   int label_w;
   int pill_w;
   int pill_x;
   int label_x;
   int badge_text_y;
   int label_y;
   int glyph_w;
   int min_badge_w;

   if (badge_h < 1)
   {
      badge_h       = L->row_h;
      badge_inset_y = 0;
   }

   /* Replicate downplay_badge_width but against badge_h (the inset
    * inner height), not row_h, so the badge stays proportional to its
    * own height even when the outer pill is taller. */
   glyph_w     = font_driver_get_message_width(font, glyph,
         (unsigned)strlen(glyph), 1.0f);
   min_badge_w = badge_h;
   badge_w     = (glyph_w > 0) ? glyph_w + 2 * L->chrome_pad_x : min_badge_w;
   if (badge_w < min_badge_w)
      badge_w = min_badge_w;

   label_w = font_driver_get_message_width(font, label,
         (unsigned)strlen(label), 1.0f);
   if (label_w < 0)
      label_w = 0;

   /* Outer pill: padding-left + badge + gap + label + padding-right. */
   pill_w = inner_pad_x + badge_w + L->chrome_gap + label_w
          + inner_pad_x;
   if (pill_w < L->row_h)
      pill_w = L->row_h;

   pill_x = (anchor == DOWNPLAY_ANCHOR_RIGHT)
          ? anchor_x - pill_w
          : anchor_x;

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         pill_x, y, pill_w, L->row_h, DP_COLOR_PILL_DARK);

   badge_x      = pill_x + inner_pad_x;
   badge_text_y = downplay_baseline_y(y + badge_inset_y, badge_h,
         centre_offset);
   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         badge_x, y + badge_inset_y, badge_w, badge_h,
         DP_COLOR_PILL_LIGHT);
   downplay_draw_text(font, glyph,
         (float)(badge_x + badge_w / 2), (float)badge_text_y,
         L, DP_TEXT_DARK, TEXT_ALIGN_CENTER);

   label_x = badge_x + badge_w + L->chrome_gap;
   label_y = downplay_baseline_y(y, L->row_h, centre_offset);
   downplay_draw_text(font, label,
         (float)label_x, (float)label_y,
         L, DP_TEXT_LIGHT, TEXT_ALIGN_LEFT);
}

/* Trim `buf` in-place so its rendered width through `font` fits in
 * max_w pixels, appending "..." when truncation occurs.  No-op when
 * the original already fits.  buf_size is the full capacity of the
 * caller's buffer (must hold the trimmed prefix + 3 ellipsis bytes +
 * NUL); we bail without mutation if it can't. */
static void downplay_truncate_to_width(font_data_t *font, char *buf,
      size_t buf_size, int max_w)
{
   const char *ell    = "...";
   size_t      ell_n  = strlen(ell);
   size_t      len;
   int         text_w;
   int         ell_w;

   if (!buf || buf_size <= ell_n + 1 || max_w <= 0)
      return;
   len = strlen(buf);
   if (len == 0)
      return;
   text_w = font_driver_get_message_width(font, buf, (unsigned)len, 1.0f);
   if (text_w >= 0 && text_w <= max_w)
      return;

   ell_w = font_driver_get_message_width(font, ell, (unsigned)ell_n, 1.0f);
   if (ell_w < 0)
      ell_w = 0;

   /* Shrink from the back until prefix + ellipsis fits.  Linear is
    * fine — title strings are short and this runs once per frame at
    * most.  After each decrement, walk past UTF-8 continuation bytes
    * (0x80-0xBF) so we never slice mid-codepoint. */
   while (len > 0)
   {
      buf[len] = '\0';
      text_w   = font_driver_get_message_width(font, buf,
            (unsigned)len, 1.0f);
      if (text_w >= 0 && text_w + ell_w <= max_w)
         break;
      len--;
      while (len > 0 && (((unsigned char)buf[len]) & 0xC0) == 0x80)
         len--;
   }
   while (len > 0 && buf[len - 1] == ' ')
   {
      len--;
      buf[len] = '\0';
   }
   /* buf_size > ell_n + 1 (checked above) and len <= buf_size - 1 by
    * construction, so len + ell_n + 1 always fits. */
   memcpy(buf + len, ell, ell_n + 1);
}

/* Draw a left-anchored dark pill with the contents of `text_buf`
 * centered inside.  Pill grows with the text up to max_w; the buffer
 * is mutated (ellipsis-truncated) when the text doesn't fit, so it
 * must be writable and ≥ NAME_MAX_LENGTH-ish.  `pad_x` is the
 * horizontal inset from each pill cap to the text — pass
 * `row_text_indent` for big pills (title, list rows) so they match
 * the visible left-margin spacing, or `chrome_pad_x` for tight chrome
 * pills (status).  Returns the pixel width drawn so callers can
 * layout further chrome to the right. */
static int downplay_draw_text_pill(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset, int height,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int x, int y, int max_w, int pad_x,
      char *text_buf, size_t text_buf_size)
{
   int text_max_w = max_w - 2 * pad_x;
   int text_w;
   int pill_w;
   int text_y = downplay_baseline_y(y, height, centre_offset);

   if (text_max_w < 0)
      text_max_w = 0;
   downplay_truncate_to_width(font, text_buf, text_buf_size, text_max_w);

   text_w = font_driver_get_message_width(font, text_buf,
         (unsigned)strlen(text_buf), 1.0f);
   if (text_w < 0)
      text_w = 0;
   pill_w = text_w + 2 * pad_x;
   if (pill_w < height)
      pill_w = height;
   if (pill_w > max_w)
      pill_w = max_w;

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         x, y, pill_w, height, DP_COLOR_PILL_DARK);
   downplay_draw_text(font, text_buf,
         (float)(x + pill_w / 2), (float)text_y,
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
   return pill_w;
}

/* INGAME-only: dark pill at the top-left holding the loaded content's
 * display name (basename minus extension).  Sized to the row font so
 * the title visually anchors the menu the way "Continue/Quit" do. */
static void downplay_draw_title_pill(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset, int height,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int right_limit)
{
   const char *content = path_get(RARCH_PATH_CONTENT);
   char        title[NAME_MAX_LENGTH];
   int         max_w;

   if (!content || !*content)
      return;
   fill_pathname_base(title, content, sizeof(title));
   path_remove_extension(title);
   if (!*title)
      return;

   /* Title spans from the left margin to right_limit (the caller
    * subtracts the status pill + a margin gap), so on long content
    * names it grows almost the full width of the screen instead of
    * truncating at the half mark. */
   max_w = right_limit - L->margin_x;
   if (max_w < height)
      max_w = height;

   downplay_draw_text_pill(p_disp, userdata, font, centre_offset, height,
         L, cap_tex, L->margin_x, L->margin_y, max_w, L->row_text_indent,
         title, sizeof(title));
}

/* Placeholder battery indicator — real telemetry wires up later. */
static const char DOWNPLAY_STATUS_TEXT[] = "100%";

/* Width of the rendered status pill — useful to callers (e.g. the
 * title pill) that need to reserve space for it without coupling to
 * its draw site. */
static int downplay_status_pill_width(font_data_t *font,
      const downplay_layout_t *L)
{
   int text_w = font_driver_get_message_width(font, DOWNPLAY_STATUS_TEXT,
         (unsigned)strlen(DOWNPLAY_STATUS_TEXT), 1.0f);
   int pill_w;
   if (text_w < 0)
      text_w = 0;
   pill_w = text_w + 2 * L->chrome_pad_x;
   if (pill_w < L->row_h)
      pill_w = L->row_h;
   return pill_w;
}

static void downplay_draw_status_pill(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset,
      const downplay_layout_t *L, uintptr_t cap_tex)
{
   /* Right-anchored: pill sized to text + padding (with a row-h floor
    * so single-glyph values still look pill-shaped), positioned so its
    * right edge sits on the right margin. */
   int pill_w = downplay_status_pill_width(font, L);
   int x      = (int)L->vid_w - L->margin_x - pill_w;
   int text_y = downplay_baseline_y(L->margin_y, L->row_h, centre_offset);

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         x, L->margin_y, pill_w, L->row_h, DP_COLOR_PILL_DARK);
   downplay_draw_text(font, DOWNPLAY_STATUS_TEXT,
         (float)(x + pill_w / 2), (float)text_y,
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
}

/* ---------- main list ---------- */

/* Draw one list row.  The selection pill auto-sizes to the label
 * width (capped at row_max_w) — same shape as a title pill that's
 * stretched to enclose its label, with row_text_indent worth of
 * padding either side so the visible text-to-pill-edge spacing
 * matches the unselected-row left margin. */
static void downplay_draw_list_row(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int list_x, int row_y, int row_max_w,
      const char *label, bool selected)
{
   char     buf[NAME_MAX_LENGTH];
   int      text_x   = list_x + L->row_text_indent;
   int      text_y   = downplay_baseline_y(row_y, L->row_h, centre_offset);
   int      max_text_w;
   int      text_w;
   int      pill_w;
   uint32_t txt_color = selected ? DP_TEXT_DARK : DP_TEXT_LIGHT;

   if (!label || !*label)
      return;

   /* Copy to local buffer so we can truncate without mutating the
    * caller's source string. */
   strlcpy(buf, label, sizeof(buf));

   /* Available text width: full row minus the matched padding on
    * either side (row_text_indent both sides keeps text equidistant
    * from the visible pill edge when selected, and from the left
    * margin when not). */
   max_text_w = row_max_w - 2 * L->row_text_indent;
   if (max_text_w < 0)
      max_text_w = 0;
   downplay_truncate_to_width(font, buf, sizeof(buf), max_text_w);

   text_w = font_driver_get_message_width(font, buf,
         (unsigned)strlen(buf), 1.0f);
   if (text_w < 0)
      text_w = 0;

   if (selected)
   {
      pill_w = text_w + 2 * L->row_text_indent;
      if (pill_w < L->row_h)
         pill_w = L->row_h;
      if (pill_w > row_max_w)
         pill_w = row_max_w;
      downplay_draw_pill(p_disp, userdata, L, cap_tex,
            list_x, row_y, pill_w, L->row_h, DP_COLOR_PILL_LIGHT);
   }

   downplay_draw_text(font, buf,
         (float)text_x, (float)text_y, L, txt_color, TEXT_ALIGN_LEFT);
}

static void downplay_draw_list(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, uintptr_t cap_tex,
      const downplay_handle_t *dp)
{
   size_t   i;
   /* Push the list below the status pill row so they don't collide. */
   int      list_top   = L->margin_y + L->row_h + (int)(16.0f * L->scale);
   int      list_x     = L->margin_x;
   int      row_max_w  = (int)L->vid_w - (2 * L->margin_x);
   int      row_y;

   for (i = 0; i < dp->total_rows; i++)
   {
      row_y = list_top + (int)(i * (size_t)L->row_h);
      /* Skip rows that fall below the bottom hint strip — saves draws on
       * long lists and avoids text colliding with the chrome. */
      if (row_y + L->row_h > (int)L->vid_h - L->margin_y - L->row_h)
         break;
      downplay_draw_list_row(p_disp, userdata, dp->font,
            dp->font_centre_offset, L, cap_tex,
            list_x, row_y, row_max_w,
            downplay_row_label(dp, i), i == dp->selection);
   }
}

/* ---------- frame ---------- */

/* Drive the INGAME view from the actual core-running state.  Upstream
 * menu drivers don't detect this themselves — task_content sets
 * MENU_ST_FLAG_PENDING_QUICK_MENU on content load and runloop pushes
 * ACTION_OK_DL_CONTENT_SETTINGS for them.  We render our own view
 * stack instead, so we read the underlying condition (CORE_RUNNING)
 * and consume the upstream flag so it can't queue a displaylist push
 * that we'd ignore. */
static void downplay_sync_ingame(downplay_handle_t *dp)
{
   struct menu_state *menu_st = menu_state_get_ptr();
   bool               running = (runloop_get_flags()
         & RUNLOOP_FLAG_CORE_RUNNING) != 0;

   /* Unconditional: upstream may set this flag at content load even
    * before our frame fires, so we eat it every tick rather than only
    * on the running→ingame transition. */
   if (menu_st)
      menu_st->flags &= ~MENU_ST_FLAG_PENDING_QUICK_MENU;

   if (running && dp->view != DOWNPLAY_VIEW_INGAME)
   {
      dp->prior_view      = dp->view;
      dp->prior_selection = dp->selection;
      dp->view            = DOWNPLAY_VIEW_INGAME;
      dp->selection       = 0;
      downplay_recompute_total_rows(dp);
   }
   else if (!running && dp->view == DOWNPLAY_VIEW_INGAME)
   {
      dp->view      = dp->prior_view;
      dp->selection = dp->prior_selection;
      downplay_recompute_total_rows(dp);
   }
}

/* What the frame should render.  AWAITING_LIST is intentionally rendered
 * BLANK rather than LIST: showing the system list briefly and then
 * snapping into the splash once the first download starts looks like a
 * glitch.  Better to stay blank until we know whether we're going to
 * splash at all. */
enum downplay_render_mode
{
   DOWNPLAY_RENDER_LIST = 0,   /* normal menu */
   DOWNPLAY_RENDER_BLANK,      /* awaiting buildbot list — chrome only */
   DOWNPLAY_RENDER_SPLASH      /* download in flight */
};

static enum downplay_render_mode downplay_get_render_mode(void)
{
   switch (downplay_cores_get_state())
   {
      case DOWNPLAY_CORES_INSTALLING:
         return DOWNPLAY_RENDER_SPLASH;
      case DOWNPLAY_CORES_AWAITING_LIST:
         return DOWNPLAY_RENDER_BLANK;
      default:
         return DOWNPLAY_RENDER_LIST;
   }
}

/* Called every frame after downplay_cores_pump.  When the cores module
 * settles back to DONE/INACTIVE (rendering as LIST again) and we have a
 * pending pick, finish the launch.  If install failed (core still not
 * installed), do_launch logs and bails — we still clear the slot so the
 * user gets the menu back. */
static void downplay_drive_pending_launch(downplay_handle_t *dp)
{
   char core[sizeof(dp->pending_launch_core)];
   char rom[sizeof(dp->pending_launch_rom)];

   if (!*dp->pending_launch_core)
      return;
   if (downplay_get_render_mode() != DOWNPLAY_RENDER_LIST)
      return;

   strlcpy(core, dp->pending_launch_core, sizeof(core));
   strlcpy(rom,  dp->pending_launch_rom,  sizeof(rom));
   downplay_clear_pending_launch(dp);
   downplay_do_launch_rom(core, rom);
}

/* Only called in DOWNPLAY_RENDER_SPLASH (i.e. cores state == INSTALLING),
 * so we always have a current ident and progress to draw. */
static void downplay_draw_setup_splash(gfx_display_t *p_disp, void *userdata,
      const downplay_handle_t *dp)
{
   const downplay_layout_t *L     = &dp->layout;
   const char              *ident;
   size_t                   done  = 0;
   size_t                   total = 0;
   char                     subline[160];
   float                    cy    = (float)L->vid_h * 0.5f;

   ident      = downplay_cores_get_progress(&done, &total);
   subline[0] = '\0';
   if (ident)
      snprintf(subline, sizeof(subline), "%s   (%u of %u)",
            ident, (unsigned)(done + 1), (unsigned)total);

   downplay_draw_text(dp->font, "Downloading core…",
         (float)L->vid_w * 0.5f,
         cy + (L->font_size * 0.35f),
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
   if (*subline)
      downplay_draw_text(dp->chrome_font, subline,
            (float)L->vid_w * 0.5f,
            cy + L->font_size + (L->chrome_font_size * 0.35f) + (8.0f * L->scale),
            L, DP_TEXT_MUTED, TEXT_ALIGN_CENTER);
}

static void downplay_menu_frame(void *data, video_frame_info_t *video_info)
{
   downplay_handle_t *dp = (downplay_handle_t*)data;
   gfx_display_t     *p_disp;
   void              *userdata;
   settings_t        *settings;
   float              user_scale;
   int                          bottom_y;
   enum downplay_render_mode    mode;

   if (!dp)
      return;

   p_disp     = disp_get_ptr();
   userdata   = video_info->userdata;
   settings   = config_get_ptr();
   user_scale = (settings && settings->floats.menu_scale_factor > 0.0f)
                ? settings->floats.menu_scale_factor : 1.0f;

   if (!p_disp)
      return;

   /* Pump the cores state machine on every frame.  Cheap; only does work
    * when the buildbot list has just landed. */
   downplay_cores_pump();
   /* If the pump just transitioned the cores state to DONE, this fires
    * on the same frame and the splash's "complete" state is never drawn.
    * That's intentional: content-load is async, so the splash dismissing
    * straight into the loading screen is the desired UX. */
   downplay_drive_pending_launch(dp);
   downplay_sync_ingame(dp);
   mode = downplay_get_render_mode();

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

   /* Background — opaque normally, dim-overlay over the running game so
    * INGAME reads as a HUD instead of hiding the frame underneath. */
   downplay_draw_rect(p_disp, userdata, &dp->layout,
         0, 0, (int)dp->layout.vid_w, (int)dp->layout.vid_h,
         dp->view == DOWNPLAY_VIEW_INGAME ? DP_COLOR_BG_INGAME : DP_COLOR_BG);

   /* Top-right status pill */
   downplay_draw_status_pill(p_disp, userdata, dp->chrome_font,
         dp->chrome_font_centre_offset, &dp->layout, dp->pill_cap_tex);

   if (dp->view == DOWNPLAY_VIEW_INGAME)
   {
      /* Title may grow up to the left edge of the status pill, with
       * one margin's worth of gap between them so they read as
       * separate elements.  status_pill_width must be called with
       * the same font that draw_status_pill above renders with —
       * mismatch would silently misalign the gap. */
      int status_w = downplay_status_pill_width(dp->chrome_font,
            &dp->layout);
      int title_right_limit = (int)dp->layout.vid_w - dp->layout.margin_x
            - status_w - dp->layout.margin_x;
      downplay_draw_title_pill(p_disp, userdata, dp->font,
            dp->font_centre_offset, dp->layout.row_h,
            &dp->layout, dp->pill_cap_tex, title_right_limit);
   }

   switch (mode)
   {
      case DOWNPLAY_RENDER_SPLASH:
         downplay_draw_setup_splash(p_disp, userdata, dp);
         break;
      case DOWNPLAY_RENDER_LIST:
         downplay_draw_list(p_disp, userdata,
               &dp->layout, dp->pill_cap_tex, dp);
         break;
      case DOWNPLAY_RENDER_BLANK:
         /* Nothing — chrome below still draws, but no list / no splash. */
         break;
   }

   /* Bottom hints — each side is its own auto-sized footer pill: an
    * outer dark pill (row_h tall) that hugs the chrome inside.  The
    * inner glyph badge is inset vertically so it reads as a key cap
    * sitting on the footer surface, with the label in white next to
    * it.  Against the launcher's dark background the outer pills
    * visually disappear; against a running game (INGAME mode) they
    * give the chrome a coherent base. */
   bottom_y = (int)dp->layout.vid_h - dp->layout.margin_y - dp->layout.row_h;
   downplay_draw_footer_hint(p_disp, userdata, dp->chrome_font,
         dp->chrome_font_centre_offset, &dp->layout,
         dp->pill_cap_tex, dp->layout.margin_x, bottom_y,
         DOWNPLAY_ANCHOR_LEFT, "POWER", "SLEEP");

   /* Right-aligned hint depends on mode.  Hidden in BLANK since there's
    * no action to advertise yet. */
   if (mode != DOWNPLAY_RENDER_BLANK)
   {
      const char *glyph = (mode == DOWNPLAY_RENDER_SPLASH) ? "B" : "A";
      const char *label = (mode == DOWNPLAY_RENDER_SPLASH) ? "CANCEL" : "OPEN";
      int x = (int)dp->layout.vid_w - dp->layout.margin_x;
      downplay_draw_footer_hint(p_disp, userdata, dp->chrome_font,
            dp->chrome_font_centre_offset, &dp->layout,
            dp->pill_cap_tex, x, bottom_y,
            DOWNPLAY_ANCHOR_RIGHT, glyph, label);
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

   /* Boot pass in progress (awaiting list or downloading): swallow nav.
    * CANCEL aborts the pass — during AWAITING_LIST that drops straight
    * to DONE; during INSTALLING the in-flight task runs to completion
    * but its result is discarded.  The screen leaves splash/blank state
    * naturally as the cores state advances. */
   if (downplay_get_render_mode() != DOWNPLAY_RENDER_LIST)
   {
      if (action == MENU_ACTION_CANCEL)
      {
         /* Drop any pending lazy launch so a cancelled lazy install
          * returns to the menu instead of auto-launching when the
          * in-flight task completes. */
         downplay_clear_pending_launch(dp);
         downplay_cores_cancel();
      }
      return 0;
   }

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
         if (dp->view == DOWNPLAY_VIEW_INGAME)
         {
            if (dp->selection == 0)
               command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            else
               command_event(CMD_EVENT_UNLOAD_CORE, NULL);
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_SYSTEM)
         {
            const downplay_system_t *sys = &dp->systems[dp->active_system];
            const downplay_rom_t    *rom = &dp->roms[dp->selection];
            downplay_launch_rom(dp, sys->core_ident, rom->full_path);
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_RECENTS)
         {
            if (dp->recents && dp->selection < dp->recent_row_count)
               downplay_launch_recent(dp->recents[dp->selection].pl_idx);
            return 0;
         }
         /* TOP view */
         if (downplay_has_recents_row(dp) && dp->selection == 0)
         {
            downplay_open_recents(dp);
            return 0;
         }
         {
            size_t sys_idx = downplay_has_recents_row(dp)
               ? dp->selection - 1 : dp->selection;
            downplay_open_system(dp, sys_idx);
         }
         return 0;
      case MENU_ACTION_CANCEL:
         if (dp->view == DOWNPLAY_VIEW_INGAME)
         {
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_SYSTEM)
         {
            downplay_close_system(dp);
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_RECENTS)
         {
            downplay_close_recents(dp);
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

   dp->selection  = 0;
   dp->view       = DOWNPLAY_VIEW_TOP;
   dp->prior_view = DOWNPLAY_VIEW_TOP;
   /* Upstream only fires HISTORY_INIT lazily on first content load
    * (tasks/task_content.c), so on a fresh boot g_defaults.content_history
    * is NULL and our "Recently Played" row never appears.  Guard so menu
    * re-init (driver swap, GPU reset) doesn't re-read the file. */
   if (!g_defaults.content_history)
      command_event(CMD_EVENT_HISTORY_INIT, NULL);
   downplay_rebuild_lists(dp);

   /* Eager-on-boot core install (PLAN.md M3).  Collect every core_ident
    * referenced by a non-empty system folder, hand them to the cores
    * module — it dedupes, drops already-installed ones, and kicks the
    * buildbot list fetch.  Splash UI takes over while this is running. */
   if (dp->system_count > 0)
   {
      const char **idents = (const char**)malloc(
            dp->system_count * sizeof(*idents));
      if (idents)
      {
         size_t i;
         for (i = 0; i < dp->system_count; i++)
            idents[i] = dp->systems[i].core_ident;
         downplay_cores_begin_boot_setup(idents, dp->system_count);
         free(idents);
      }
   }

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
   downplay_recents_free(dp->recents, dp->recent_row_count);
   /* Don't free(dp) — menu_driver_ctl(RARCH_MENU_CTL_DEINIT) frees
    * menu_st->userdata (which is dp) itself right after this returns. */
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
