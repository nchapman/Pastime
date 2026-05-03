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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

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
#include "../../gfx/gfx_thumbnail.h"
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
   DOWNPLAY_VIEW_INGAME,     /* core running; show Continue/Save/Load/Quit overlay */
   DOWNPLAY_VIEW_SAVE_PICKER,/* drilled into save-state list (M7) */
   DOWNPLAY_VIEW_SETTINGS    /* M8 — settings-style list (Options → core opts) */
};

/* INGAME row composition is conditional: Save hidden when the core
 * doesn't support savestates; Load hidden when no manual saves exist
 * yet.  We build this small action array on view enter so row labels
 * and OK dispatch share one source of truth (no parallel arrays
 * drifting). */
enum downplay_ingame_action
{
   DP_INGAME_CONTINUE = 0,
   DP_INGAME_SAVE,
   DP_INGAME_LOAD,
   DP_INGAME_OPTIONS,
   DP_INGAME_QUIT
};

/* Save-state UX (M7).  RetroArch supports slots 0..999 but 10 manual
 * slots is the MinUI cap — comfortably fits a list of thumbnails on
 * one screen and keeps flash wear bounded for handhelds. */
#define DOWNPLAY_MAX_MANUAL_SLOTS  10
#define DOWNPLAY_MAX_SAVE_ENTRIES (DOWNPLAY_MAX_MANUAL_SLOTS + 1) /* +1 for .auto */

typedef struct
{
   /* slot in {-1, 0..9}.  -1 means the .auto autosave. */
   int        slot;
   /* Filesystem mtime in seconds (0 if stat failed; entries with mtime
    * == 0 are dropped during enumeration so we never present them). */
   int64_t    mtime;
   /* GPU texture for the per-state thumbnail PNG.  0 when the file
    * doesn't exist or upload failed; renderer falls back to a flat
    * placeholder rect in that case. */
   uintptr_t  thumb_tex;
   unsigned   thumb_w;
   unsigned   thumb_h;
   /* Reserved for the future Lock feature (PLAN.md M7 follow-up).
    * pick_next_save_slot already filters on this so a future on-disk
    * lock-sidecar reader can flip it without further plumbing. */
   bool       locked;
   /* Pre-formatted relative-time label, e.g. "5 minutes ago" or
    * "Auto".  Built once on enumeration; the picker just draws it. */
   char       label[40];
} downplay_save_entry_t;

/* Settings-style list (M8).  A second list aesthetic — denser rows,
 * smaller font, optional value-cycler on the right, optional
 * description band at the bottom.  Powers the in-game Options menu and
 * its core-options child.
 *
 * One row covers all three "kinds" we need: an options row (title +
 * cyclable value + on_change), a navigation row (title + on_confirm
 * that pushes a child list), or an action row (title + on_confirm that
 * does something).  The visual differs only in whether `values` is
 * non-NULL (selected option rows get a full-width gray background pill
 * showing what's being adjusted; nav/action rows just show a title
 * pill).  Confirm fires `on_confirm` regardless. */
typedef struct
{
   const char         *title;        /* required; not owned */
   const char         *desc;         /* nullable; not owned */
   const char *const  *values;       /* nullable; not owned */
   size_t              values_count;
   size_t              idx_value;    /* current value index */
   /* on_change is called *after* idx_value is updated by the input
    * handler.  delta is +1 or -1.  Allowed to mutate other rows in
    * the list (e.g. visibility) — the handler refreshes after. */
   void              (*on_change)(int delta, void *userdata);
   void              (*on_confirm)(void *userdata);
   void               *userdata;
} downplay_settings_row_t;

/* One settings list.  Stack-allocated when small / static (root
 * Options); heap-allocated when row count is dynamic (core options). */
typedef struct
{
   const char              *title;       /* reserved for future header */
   downplay_settings_row_t *rows;        /* owned: free()d on list dispose */
   size_t                   row_count;
   /* Optional userdata pool that backs row->userdata pointers.  Owned;
    * free()d alongside rows.  NULL when no row needs per-row allocated
    * userdata (e.g. root Options list — its on_confirm handlers take
    * dp itself). */
   void                    *userdata_pool;
   unsigned                 width_pct;   /* % of vid_w */
   size_t                   sel;
   size_t                   scroll;      /* topmost visible row index */
} downplay_settings_list_t;

/* Cap of 4 is plenty for the current flow (INGAME → Options →
 * Emulator → maybe a category submenu later).  Bumps cheap if needed. */
#define DOWNPLAY_SETTINGS_STACK_MAX 4

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

   /* Settings-list (M8) sizing.  Smaller rows + font than the launcher
    * list so a settings menu can show more options at once.  Description
    * font reuses chrome_font (close enough size) — these are only the
    * row geometry / row font. */
   int   settings_row_h;
   float settings_font_size;
   int   settings_value_gap;   /* horizontal gap between title pill and value */
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

   /* INGAME view row composition.  Built on the running→ingame
    * transition (and refreshed after a Save action), so row_label and
    * OK dispatch read the same array.  ingame_action_count is in
    * [3,5]: Continue + Options + Quit are unconditional, Save and
    * Load are conditional on core_info_current_supports_savestate()
    * and whether manual saves exist on disk. */
   enum downplay_ingame_action ingame_actions[5];
   size_t              ingame_action_count;

   /* Settings stack (M8).  Push when a nav row enters a child list;
    * pop on Cancel.  Empty stack + view==SETTINGS is a transient
    * inconsistency we never let happen (view flips to INGAME on the
    * pop that empties the stack). */
   downplay_settings_list_t *settings_stack[DOWNPLAY_SETTINGS_STACK_MAX];
   size_t                    settings_depth;
   /* Dedicated font for settings-list rows (smaller than dp->font).
    * Description band reuses dp->chrome_font — its size already lives
    * close to the desc target (~18*scale). */
   font_data_t              *settings_font;
   int                       settings_font_centre_offset;
   /* Set by core-option on_change when the core flipped visibility
    * via update_display; consumed at the top of the next frame to
    * rebuild the active core-options list in place.  Rebuild can't
    * happen inside on_change because the rebuild frees the row
    * userdata that on_change was just called on. */
   bool                      settings_rebuild_pending;

   /* Save-state picker (DOWNPLAY_VIEW_SAVE_PICKER).  Cached on view
    * enter, freed (textures unloaded) on view exit.  Sorted desc by
    * mtime; entry 0 is always the most recent.  Includes .auto if it
    * exists alongside manual slots. */
   downplay_save_entry_t save_picker[DOWNPLAY_MAX_SAVE_ENTRIES];
   size_t              save_picker_count;

   /* Cached "Resume <Game>" availability for the launcher's TOP view.
    * Refreshed on rebuild_lists; if true, a Resume row is prepended
    * above Recents.  Stores the most-recent recents playlist index so
    * selection dispatches to downplay_launch_recent without re-
    * searching. */
   bool                resume_available;
   size_t              resume_pl_idx;
   /* Pre-formatted "Resume Foo" label, mutated only on rebuild. */
   char                resume_label[NAME_MAX_LENGTH];
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
/* Mid-gray background for the selected option row in the settings list
 * — visible against pure black, subtle enough not to fight with the
 * white title pill drawn on top. */
static float DP_COLOR_PILL_BG_GRAY[16] = {
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

/* TOP-view only: row 0 is "Recently Played" iff there are recent entries.
 * If a Resume row is present (M7), it sits *above* the Recents row, so
 * the Recents-row index in TOP becomes (resume_available ? 1 : 0). */
static bool downplay_has_recents_row(const downplay_handle_t *dp)
{
   return dp->view == DOWNPLAY_VIEW_TOP && dp->recent_count > 0;
}

static bool downplay_has_resume_row(const downplay_handle_t *dp)
{
   return dp->view == DOWNPLAY_VIEW_TOP && dp->resume_available;
}

static const char *downplay_ingame_label(enum downplay_ingame_action a)
{
   switch (a)
   {
      case DP_INGAME_CONTINUE: return "Continue";
      case DP_INGAME_SAVE:     return "Save";
      case DP_INGAME_LOAD:     return "Load";
      case DP_INGAME_OPTIONS:  return "Options";
      case DP_INGAME_QUIT:     return "Quit";
   }
   return "";
}

static const char *downplay_row_label(const downplay_handle_t *dp, size_t row)
{
   size_t sys_idx;

   if (dp->view == DOWNPLAY_VIEW_INGAME)
   {
      if (row < dp->ingame_action_count)
         return downplay_ingame_label(dp->ingame_actions[row]);
      return "";
   }

   if (dp->view == DOWNPLAY_VIEW_SAVE_PICKER)
   {
      if (row < dp->save_picker_count)
         return dp->save_picker[row].label;
      return "";
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

   /* TOP view: Resume row (if present) sits above Recents (if present)
    * sits above Systems.  Order matters here — both flags are
    * independent, so we walk them in sequence rather than using a
    * single conditional offset. */
   if (downplay_has_resume_row(dp))
   {
      if (row == 0)
         return dp->resume_label;
      row--;
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
      dp->total_rows = dp->ingame_action_count;
   else if (dp->view == DOWNPLAY_VIEW_SAVE_PICKER)
      dp->total_rows = dp->save_picker_count;
   else if (dp->view == DOWNPLAY_VIEW_SYSTEM)
      dp->total_rows = dp->rom_count;
   else if (dp->view == DOWNPLAY_VIEW_RECENTS)
      dp->total_rows = dp->recent_row_count;
   else if (dp->view == DOWNPLAY_VIEW_SETTINGS)
   {
      /* Settings view manages its own selection/scroll on the active
       * stack frame; the main `selection` cursor isn't meaningful
       * here.  total_rows kept at 0 keeps any stray default-list
       * input handler a no-op. */
      dp->total_rows = 0;
      dp->selection  = 0;
      return;
   }
   else
      dp->total_rows = (downplay_has_resume_row(dp)  ? 1 : 0)
                     + (downplay_has_recents_row(dp) ? 1 : 0)
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

/* ---------- save state UX (M7) ---------- */

/* Filesystem mtime in seconds since epoch.  Returns 0 on stat failure
 * — callers treat 0 as "absent" and skip the entry, so we never sort
 * stat-failed files into the picker.  POSIX stat is fine on macOS,
 * Linux, and Android (Downplay's target platforms).  Windows ports
 * would need a `_stat` / `_stati64` shim around `<sys/stat.h>` —
 * downplay.c is HAVE_DOWNPLAY-gated and the fork doesn't ship for
 * Windows yet, so the unconditional POSIX call is fine for now. */
static int64_t downplay_file_mtime(const char *path)
{
   struct stat st;
   if (!path || !*path)
      return 0;
   if (stat(path, &st) != 0)
      return 0;
   return (int64_t)st.st_mtime;
}

/* Format a relative-time label like "5 minutes ago".  Caps at days
 * since for our use case (10 manual slots that rotate) entries older
 * than a few weeks are a corner case, not the norm.  Returns the
 * string unconditionally (always NUL-terminated). */
static void downplay_format_relative_time(int64_t mtime,
      char *out, size_t out_len)
{
   int64_t now;
   int64_t delta;
   int     n;

   if (out_len == 0)
      return;
   out[0] = '\0';
   if (mtime <= 0)
   {
      strlcpy(out, "Unknown", out_len);
      return;
   }

   /* time_t is 32-bit on some old Android ABIs (armeabi-v7a, older
    * NDKs); st.st_mtime there is also 32-bit.  The (int64_t) casts on
    * both sides truncate identically so the delta is safe — we're not
    * doing absolute-epoch comparisons that would care about wrap. */
   now   = (int64_t)time(NULL);
   delta = now - mtime;
   if (delta < 0)
      delta = 0;

   if (delta < 60)
      strlcpy(out, "Just now", out_len);
   else if (delta < 60 * 60)
   {
      n = (int)(delta / 60);
      snprintf(out, out_len, "%d minute%s ago", n, n == 1 ? "" : "s");
   }
   else if (delta < 24 * 60 * 60)
   {
      n = (int)(delta / (60 * 60));
      snprintf(out, out_len, "%d hour%s ago", n, n == 1 ? "" : "s");
   }
   else if (delta < 2 * 24 * 60 * 60)
      strlcpy(out, "Yesterday", out_len);
   else
   {
      n = (int)(delta / (24 * 60 * 60));
      snprintf(out, out_len, "%d days ago", n);
   }
}

/* Free GPU textures held by the picker entries and zero the array.
 * Safe to call repeatedly (entries with thumb_tex == 0 are skipped). */
static void downplay_save_picker_free(downplay_handle_t *dp)
{
   size_t i;
   for (i = 0; i < dp->save_picker_count; i++)
   {
      if (dp->save_picker[i].thumb_tex)
         video_driver_texture_unload(&dp->save_picker[i].thumb_tex);
   }
   memset(dp->save_picker, 0, sizeof(dp->save_picker));
   dp->save_picker_count = 0;
}

/* Synchronously decode a PNG file into a GPU texture.  Mirrors the
 * pill_cap_tex pattern at the bottom of this file (image_texture_load
 * → video_driver_texture_load).  Returns 0 if the file is missing or
 * the decode/upload failed; caller treats 0 as "no thumbnail" and
 * draws a placeholder rect.
 *
 * `filter` picks the GPU sampler.  NEAREST is right for retro
 * framebuffer screenshots (savestate thumbs) — bilinear blurs every
 * non-integer scale step, including the final compositor upscale to
 * the display, and even at exact integer multiples the result reads
 * blurry because the menu framebuffer itself is rescaled.  LINEAR is
 * right for hand-drawn / photographic assets where smoothing helps. */
static uintptr_t downplay_load_png_texture(const char *path,
      enum texture_filter_type filter,
      unsigned *out_w, unsigned *out_h)
{
   struct texture_image ti;
   uintptr_t            tex_id = 0;

   if (out_w) *out_w = 0;
   if (out_h) *out_h = 0;
   if (!path || !*path || !path_is_valid(path))
      return 0;

   memset(&ti, 0, sizeof(ti));
   /* All RA video drivers used on Downplay's targets accept RGBA;
    * mirror what build_cap_texture below does. */
   ti.supports_rgba = true;

   if (!image_texture_load(&ti, path))
      return 0;

   video_driver_texture_load(&ti, filter, &tex_id);
   if (out_w) *out_w = ti.width;
   if (out_h) *out_h = ti.height;
   image_texture_free(&ti);
   return tex_id;
}

/* mtime-desc qsort comparator. */
static int downplay_save_entry_cmp(const void *a, const void *b)
{
   const downplay_save_entry_t *ea = (const downplay_save_entry_t*)a;
   const downplay_save_entry_t *eb = (const downplay_save_entry_t*)b;
   if (ea->mtime > eb->mtime) return -1;
   if (ea->mtime < eb->mtime) return  1;
   return 0;
}

/* Walk slots {-1, 0..9}, populate `out` with entries that exist on
 * disk, sorted desc by mtime.  Caller must have freed any previous
 * picker state (textures) before calling.  load_thumbs == true loads
 * the PNG thumbnails synchronously — the picker view passes true; the
 * INGAME row-count refresh passes false (it just needs the count). */
static void downplay_savestate_enumerate(downplay_save_entry_t *out,
      size_t *out_count, bool load_thumbs)
{
   size_t           count = 0;
   int              slot;
   int64_t          mtime;
   char             state_path[PATH_MAX_LENGTH];
   char             thumb_path[PATH_MAX_LENGTH];
   char             rel[40];
   runloop_state_t *runloop_st = runloop_state_get_ptr();
   const char      *base_savestate;

   *out_count = 0;
   if (!runloop_st)
      return;
   base_savestate = runloop_st->name.savestate;
   if (!base_savestate || !*base_savestate)
      return;

   for (slot = -1; slot < DOWNPLAY_MAX_MANUAL_SLOTS; slot++)
   {
      if (!runloop_get_savestate_path(state_path, sizeof(state_path), slot))
         continue;
      mtime = downplay_file_mtime(state_path);
      if (mtime == 0)
         continue;

      out[count].slot   = slot;
      out[count].mtime  = mtime;
      out[count].locked = false;
      downplay_format_relative_time(mtime,
            out[count].label, sizeof(out[count].label));
      /* Tag the autosave so the user can tell it apart from manual
       * saves (esp. when "Just now" matches a recent quit). */
      if (slot < 0)
      {
         strlcpy(rel, out[count].label, sizeof(rel));
         snprintf(out[count].label, sizeof(out[count].label),
               "Auto - %s", rel);
      }

      if (load_thumbs)
      {
         gfx_savestate_thumbnail_get_path(thumb_path, sizeof(thumb_path),
               base_savestate, slot);
         out[count].thumb_tex = downplay_load_png_texture(thumb_path,
               TEXTURE_FILTER_NEAREST,
               &out[count].thumb_w, &out[count].thumb_h);
      }
      count++;
   }

   if (count > 1)
      qsort(out, count, sizeof(*out), downplay_save_entry_cmp);
   *out_count = count;
}

/* For the in-game Save action: pick the slot we should overwrite next.
 * Strategy: first empty manual slot 0..9, else oldest (lowest mtime)
 * unlocked manual slot.  Ignores the .auto slot — that's reserved for
 * RA's autosave-on-quit and is never our target. */
static int downplay_pick_next_save_slot(void)
{
   int              slot;
   int64_t          mt;
   int              oldest_slot = 0;
   int64_t          oldest_mt   = INT64_MAX;
   char             state_path[PATH_MAX_LENGTH];
   runloop_state_t *runloop_st  = runloop_state_get_ptr();

   if (!runloop_st || !*runloop_st->name.savestate)
      return 0;

   for (slot = 0; slot < DOWNPLAY_MAX_MANUAL_SLOTS; slot++)
   {
      if (!runloop_get_savestate_path(state_path, sizeof(state_path), slot))
         continue;
      mt = downplay_file_mtime(state_path);
      if (mt == 0)
         return slot;       /* first empty wins */
      if (mt < oldest_mt)
      {
         oldest_mt   = mt;
         oldest_slot = slot;
      }
   }
   return oldest_slot;
}

/* Save / load to a specific slot WITHOUT permanently disturbing the
 * user's state_slot cursor (which OSD widgets, hotkeys, and Settings
 * UI all read from).  Stash → set → fire command → restore. */
static void downplay_save_to_slot(int slot)
{
   settings_t *settings = config_get_ptr();
   int         saved_slot;

   if (!settings)
      return;
   saved_slot                      = settings->ints.state_slot;
   configuration_set_int(settings, settings->ints.state_slot, slot);
   command_event(CMD_EVENT_SAVE_STATE, NULL);
   configuration_set_int(settings, settings->ints.state_slot, saved_slot);
}

static void downplay_load_from_slot(int slot)
{
   settings_t *settings = config_get_ptr();
   int         saved_slot;

   if (!settings)
      return;
   saved_slot                      = settings->ints.state_slot;
   configuration_set_int(settings, settings->ints.state_slot, slot);
   command_event(CMD_EVENT_LOAD_STATE, NULL);
   configuration_set_int(settings, settings->ints.state_slot, saved_slot);
}

/* Recompute INGAME row composition based on core capability + on-disk
 * save count.  Save row hidden when the core can't savestate; Load
 * row hidden when zero manual saves exist (auto-only doesn't count —
 * that's loaded via Resume from the launcher, not the in-game menu).
 * Selection is clamped to the new bounds. */
static void downplay_refresh_ingame_actions(downplay_handle_t *dp)
{
   downplay_save_entry_t scratch[DOWNPLAY_MAX_SAVE_ENTRIES];
   size_t                scratch_count = 0;
   bool                  supports;
   size_t                manual_count  = 0;
   size_t                i;
   size_t                n             = 0;

   supports = core_info_current_supports_savestate();

   if (supports)
   {
      downplay_savestate_enumerate(scratch, &scratch_count, false);
      for (i = 0; i < scratch_count; i++)
         if (scratch[i].slot >= 0)
            manual_count++;
   }

   dp->ingame_actions[n++] = DP_INGAME_CONTINUE;
   if (supports)
      dp->ingame_actions[n++] = DP_INGAME_SAVE;
   if (supports && manual_count > 0)
      dp->ingame_actions[n++] = DP_INGAME_LOAD;
   dp->ingame_actions[n++] = DP_INGAME_OPTIONS;
   dp->ingame_actions[n++] = DP_INGAME_QUIT;
   dp->ingame_action_count = n;

   if (dp->view == DOWNPLAY_VIEW_INGAME && dp->selection >= n)
      dp->selection = n - 1;
}

/* Build "Resume <Game>" availability from the head of recents.  No
 * runtime state is loaded (we're on the launcher home), so we
 * reproduce RA's path-construction for `<savestate_dir>/<content
 * basename minus extension>.state.auto`.  This matches the branch in
 * runloop_path_set_redirect (runloop.c:8408) that fires when
 * directory_savestate is set — Downplay always sets it via the
 * defaults overlay.  Crucially, RA strips the content extension
 * before appending `.state` (see runloop_path_set_basename at
 * runloop.c:8174), so we must too — otherwise `foo.nes` would map
 * to `foo.nes.state.auto` and the Resume row would never appear. */
static void downplay_refresh_resume(downplay_handle_t *dp)
{
   playlist_t                  *pl    = g_defaults.content_history;
   const struct playlist_entry *entry = NULL;
   const char                  *savestate_dir;
   char                         basename[NAME_MAX_LENGTH];
   char                         auto_path[PATH_MAX_LENGTH];
   const char                  *label;

   dp->resume_available = false;
   dp->resume_label[0]  = '\0';

   if (!pl || playlist_size(pl) == 0)
      return;
   playlist_get_index(pl, 0, &entry);
   if (!entry || !entry->path || !*entry->path)
      return;

   savestate_dir = dir_get_ptr(RARCH_DIR_SAVESTATE);
   if (!savestate_dir || !*savestate_dir)
      return;

   fill_pathname_base(basename, entry->path, sizeof(basename));
   path_remove_extension(basename);
   if (!*basename)
      return;

   fill_pathname_join_special(auto_path, savestate_dir, basename,
         sizeof(auto_path));
   strlcat(auto_path, ".state.auto", sizeof(auto_path));
   if (!path_is_valid(auto_path))
      return;

   if (entry->label && *entry->label)
      label = entry->label;
   else
      label = basename;

   snprintf(dp->resume_label, sizeof(dp->resume_label), "Resume %s", label);
   dp->resume_available = true;
   dp->resume_pl_idx    = 0;
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

   /* M7: launcher's "Resume <Game>" row appears when the most-recent
    * recents entry has a `.state.auto` on disk.  Refreshed here so a
    * fresh boot picks up an autosave from a previous process. */
   downplay_refresh_resume(dp);

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

   /* Settings-list dimensions.  Row height + font picked so ~10 rows
    * fit on a 480px-tall reference design with room for top/bottom
    * margins, the status pill, the description band, and a hint
    * footer.  Tweak after the first build on a real device. */
   L->settings_row_h     = (int)(36.0f * scale);
   L->settings_font_size = 22.0f * scale;
   L->settings_value_gap = (int)(24.0f * scale);
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

/* Forward decls for settings-list lifecycle (defined further down). */
static void downplay_settings_pop_all(downplay_handle_t *dp);

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
   if (dp->settings_font)
   {
      font_driver_free(dp->settings_font);
      dp->settings_font = NULL;
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
   dp->settings_font = downplay_load_font(p_disp,
         dp->layout.settings_font_size, is_threaded);
   dp->settings_font_centre_offset = dp->settings_font
         ? font_driver_get_line_centre_offset(dp->settings_font, 1.0f) : 0;
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

/* ---------- settings list (M8) ---------- */

static void downplay_settings_list_free(downplay_settings_list_t *L)
{
   if (!L)
      return;
   free(L->rows);
   free(L->userdata_pool);
   free(L);
}

/* Heap-allocate an empty list with capacity for `row_count` rows.
 * `userdata_pool_size` reserves a contiguous backing buffer for
 * per-row userdata structs (caller carves it up).  Pass 0 if no row
 * needs allocated userdata. */
static downplay_settings_list_t *downplay_settings_list_new(
      const char *title, size_t row_count,
      size_t userdata_pool_size, unsigned width_pct)
{
   downplay_settings_list_t *L;
   if (!(L = (downplay_settings_list_t*)calloc(1, sizeof(*L))))
      return NULL;
   if (row_count > 0)
   {
      if (!(L->rows = (downplay_settings_row_t*)calloc(
                  row_count, sizeof(*L->rows))))
      {
         free(L);
         return NULL;
      }
   }
   if (userdata_pool_size > 0)
   {
      if (!(L->userdata_pool = calloc(1, userdata_pool_size)))
      {
         free(L->rows);
         free(L);
         return NULL;
      }
   }
   L->title     = title;
   L->row_count = row_count;
   L->width_pct = width_pct;
   return L;
}

/* Push a fully-built list onto the stack and switch to SETTINGS view.
 * Takes ownership: a pop will free the list.  Drops the push if the
 * stack is full or the list is NULL (logs and frees the orphan). */
static void downplay_settings_push(downplay_handle_t *dp,
      downplay_settings_list_t *L)
{
   if (!L)
      return;
   if (dp->settings_depth >= DOWNPLAY_SETTINGS_STACK_MAX)
   {
      RARCH_WARN("[Downplay] settings stack full; dropping push\n");
      downplay_settings_list_free(L);
      return;
   }
   /* First push from a non-SETTINGS view captures where we came from
    * so Cancel-from-root returns there. */
   if (dp->settings_depth == 0)
   {
      dp->prior_view      = dp->view;
      dp->prior_selection = dp->selection;
      dp->view            = DOWNPLAY_VIEW_SETTINGS;
      dp->selection       = 0;
   }
   dp->settings_stack[dp->settings_depth++] = L;
   downplay_recompute_total_rows(dp);
}

/* Pop the top list.  When the stack empties, restore the prior view. */
static void downplay_settings_pop(downplay_handle_t *dp)
{
   if (dp->settings_depth == 0)
      return;
   dp->settings_depth--;
   downplay_settings_list_free(dp->settings_stack[dp->settings_depth]);
   dp->settings_stack[dp->settings_depth] = NULL;
   if (dp->settings_depth == 0)
   {
      dp->view      = dp->prior_view;
      dp->selection = dp->prior_selection;
      downplay_refresh_ingame_actions(dp);
      downplay_recompute_total_rows(dp);
   }
}

static void downplay_settings_pop_all(downplay_handle_t *dp)
{
   while (dp->settings_depth > 0)
      downplay_settings_pop(dp);
}

/* Free all stack entries without the view-restore side effects of
 * pop_all.  Used at teardown when there's nowhere to restore to. */
static void downplay_settings_clear(downplay_handle_t *dp)
{
   size_t i;
   for (i = 0; i < dp->settings_depth; i++)
   {
      downplay_settings_list_free(dp->settings_stack[i]);
      dp->settings_stack[i] = NULL;
   }
   dp->settings_depth = 0;
}

/* Top of the stack; NULL if the stack is empty. */
static downplay_settings_list_t *downplay_settings_top(
      const downplay_handle_t *dp)
{
   if (dp->settings_depth == 0)
      return NULL;
   return dp->settings_stack[dp->settings_depth - 1];
}

/* Fixed vertical clearance around the row block to host the up /
 * down scroll chevrons.  Reserved unconditionally (not only when a
 * chevron is visible) so visible_rows stays stable as scroll
 * advances — otherwise paging would shift the row block as the
 * top/bottom chevrons appeared and disappeared. */
#define DOWNPLAY_SETTINGS_CHEVRON_ZONE_PX(L) ((int)(20.0f * (L)->scale))

/* How many rows fit in the visible area for the active list.
 * Geometry mirrors downplay_draw_settings_view: top = below the
 * status row (with the same gap the launcher list uses); bottom =
 * just above the description band, which sits at the very bottom
 * margin (the SETTINGS view hides the footer hint pills so the row
 * block can extend that far).  Chevron zones are reserved at both
 * ends. */
static size_t downplay_settings_visible_rows(const downplay_handle_t *dp,
      const downplay_settings_list_t *L)
{
   const downplay_layout_t *Ly = &dp->layout;
   int chev_z   = DOWNPLAY_SETTINGS_CHEVRON_ZONE_PX(Ly);
   int top_y    = Ly->margin_y + Ly->row_h + (int)(16.0f * Ly->scale)
                + chev_z;
   int desc_h   = (int)(Ly->chrome_font_size * 2.4f);
   int bottom_y = (int)Ly->vid_h - Ly->margin_y - desc_h - chev_z;
   int avail    = bottom_y - top_y;
   int n;
   if (avail <= 0 || Ly->settings_row_h <= 0)
      return 1;
   n = avail / Ly->settings_row_h;
   if (n < 1)
      n = 1;
   if ((size_t)n > L->row_count)
      n = (int)L->row_count;
   return (size_t)n;
}

/* Keep `scroll` such that `sel` is in the visible window.  Symmetric
 * snap on both ends so a single Down past the edge advances scroll
 * by 1 (no jumpy auto-centring). */
static void downplay_settings_snap_scroll(const downplay_handle_t *dp,
      downplay_settings_list_t *L)
{
   size_t visible = downplay_settings_visible_rows(dp, L);
   if (L->row_count == 0)
   {
      L->scroll = 0;
      return;
   }
   if (L->sel < L->scroll)
      L->scroll = L->sel;
   else if (L->sel >= L->scroll + visible)
      L->scroll = L->sel + 1 - visible;
   if (L->scroll + visible > L->row_count)
      L->scroll = (L->row_count > visible) ? L->row_count - visible : 0;
}

/* Draw a downward-pointing chevron centered on (cx, cy_top).  Built
 * from filled rects so we don't depend on the bundled font carrying
 * U+25BC.  `dir` is +1 down, -1 up. */
static void downplay_draw_chevron(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, int cx, int cy_top, int dir)
{
   /* Drawn as a triangle approximated by stacked horizontal bars.
    * `bars` is the height in source rows; width tapers to zero.
    * Tested values keep the chevron crisp at both 1× and 2× scale. */
   int   bars   = (int)(8.0f * L->scale);
   int   half_w = (int)(8.0f * L->scale);
   int   i;
   float color[16];
   /* Mid-gray, matches DP_TEXT_MUTED tone; gfx_display_draw_quad takes
    * a per-vertex RGBA float array. */
   for (i = 0; i < 16; i += 4)
   {
      color[i]     = 0.5f;
      color[i + 1] = 0.5f;
      color[i + 2] = 0.5f;
      color[i + 3] = 1.0f;
   }
   if (bars < 2)
      bars = 2;
   for (i = 0; i < bars; i++)
   {
      int row     = (dir > 0) ? i : (bars - 1 - i);
      int w       = (half_w * (bars - row)) / bars * 2;
      int x       = cx - w / 2;
      int y       = cy_top + i;
      if (w < 1)
         continue;
      downplay_draw_rect(p_disp, userdata, L, x, y, w, 1, color);
   }
}

/* Render the active settings list.  Geometry: width = width_pct%,
 * centered horizontally; vertically centered in the available area
 * (between top/bottom margins, footer hint, and description band).
 * Description band is fixed at the bottom — height = ~2 lines of
 * chrome font, separated from the row block by one row's worth of
 * gap. */
static void downplay_draw_settings_view(gfx_display_t *p_disp, void *userdata,
      const downplay_handle_t *dp)
{
   const downplay_layout_t        *L = &dp->layout;
   const downplay_settings_list_t *S = downplay_settings_top(dp);
   const downplay_settings_row_t  *sel_row;
   font_data_t                    *row_font;
   int                             row_centre;
   int                             desc_centre;
   unsigned                        width_pct;
   int                             block_w;
   int                             block_x;
   int                             desc_band_h;
   int                             avail_top;
   int                             avail_bottom;
   int                             avail_h;
   size_t                          visible;
   int                             block_h;
   int                             block_top;
   size_t                          i;

   if (!S || S->row_count == 0)
      return;
   row_font    = dp->settings_font ? dp->settings_font : dp->font;
   row_centre  = dp->settings_font ? dp->settings_font_centre_offset
                                   : dp->font_centre_offset;
   desc_centre = dp->chrome_font_centre_offset;

   /* width_pct is a fraction of the *content area* (vid_w minus the
    * outer left/right margins), not of the raw screen width.  At
    * 100% the block fills from margin_x to vid_w-margin_x — i.e.
    * full width with the same margins the launcher uses.  Smaller
    * values let narrower lists like Options stay visually centred. */
   {
      int avail_w;
      width_pct  = S->width_pct ? S->width_pct : 60;
      if (width_pct > 100)
         width_pct = 100;
      avail_w   = (int)L->vid_w - 2 * L->margin_x;
      if (avail_w < 0)
         avail_w = 0;
      block_w   = avail_w * (int)width_pct / 100;
      block_x   = ((int)L->vid_w - block_w) / 2;
   }

   desc_band_h  = (int)(L->chrome_font_size * 2.4f);
   /* Top edge sits just below the status pill row, with the same
    * 16*scale gap the launcher list uses — keeps the two views
    * visually consistent.  Bottom extends to vid_h - margin_y minus
    * the description band; SETTINGS view hides the footer hint pills
    * so that bottom margin is purely outer padding.  Chevron zones
    * are reserved at both ends so the up/down chevrons never collide
    * with the status row or description band. */
   {
      int chev_z = DOWNPLAY_SETTINGS_CHEVRON_ZONE_PX(L);
      avail_top    = L->margin_y + L->row_h + (int)(16.0f * L->scale)
                   + chev_z;
      avail_bottom = (int)L->vid_h - L->margin_y - desc_band_h - chev_z;
      avail_h      = avail_bottom - avail_top;
   }
   if (avail_h < L->settings_row_h)
      return;

   visible      = downplay_settings_visible_rows(dp, S);
   block_h      = (int)visible * L->settings_row_h;
   /* Top-align the row block.  Earlier centering looked good for
    * short lists but pushed long ones (with a chevron) into the
    * middle of the screen with a confusing gap above.  Empty space
    * — when the list is short — falls between the bottom of the
    * rows and the description band, where the eye doesn't notice
    * it. */
   block_top    = avail_top;
   (void)avail_h;

   /* Selected row data once, used for both background pill and desc
    * band below. */
   sel_row = (S->sel < S->row_count) ? &S->rows[S->sel] : NULL;

   for (i = 0; i < visible; i++)
   {
      size_t                         row_idx = S->scroll + i;
      const downplay_settings_row_t *r;
      int                            row_y   = block_top + (int)i * L->settings_row_h;
      bool                           selected;
      int                            text_y;
      char                           title_buf[NAME_MAX_LENGTH];
      char                           value_buf[NAME_MAX_LENGTH];
      bool                           has_value;
      int                            avail;
      int                            title_natural;
      int                            value_natural;
      int                            title_budget;
      int                            value_budget;
      int                            text_w;
      int                            value_w   = 0;
      uint32_t                       title_color;

      if (row_idx >= S->row_count)
         break;
      r         = &S->rows[row_idx];
      selected  = (row_idx == S->sel);
      has_value = (r->values && r->values_count > 0
                && r->idx_value < r->values_count
                && r->values[r->idx_value] != NULL);

      /* Selected option-row: full-width gray background pill so it's
       * visually obvious which row left/right will adjust.  Nav /
       * action rows skip this — same row layout, but no value to
       * adjust means no full-width chrome. */
      if (selected && has_value)
         downplay_draw_pill(p_disp, userdata, L, dp->pill_cap_tex,
               block_x, row_y, block_w, L->settings_row_h,
               DP_COLOR_PILL_BG_GRAY);

      /* Buffers + budgets.  The text-area budget is the row width
       * minus left + right indents.  When a value is present, we
       * also subtract one settings_value_gap so the two columns
       * don't touch.  Each side has its own buffer because both may
       * need ellipsis-truncation. */
      strlcpy(title_buf, r->title ? r->title : "", sizeof(title_buf));
      value_buf[0] = '\0';
      if (has_value)
         strlcpy(value_buf, r->values[r->idx_value], sizeof(value_buf));

      avail = block_w - 2 * L->row_text_indent;
      if (has_value)
         avail -= L->settings_value_gap;
      if (avail < L->settings_row_h)
         avail = L->settings_row_h;

      title_natural = font_driver_get_message_width(row_font, title_buf,
            (unsigned)strlen(title_buf), 1.0f);
      if (title_natural < 0)
         title_natural = 0;

      if (has_value)
      {
         value_natural = font_driver_get_message_width(row_font, value_buf,
               (unsigned)strlen(value_buf), 1.0f);
         if (value_natural < 0)
            value_natural = 0;
      }
      else
         value_natural = 0;

      /* Fair-share allocator: both natural widths fit → render
       * verbatim.  Otherwise each side gets at least half of avail;
       * if one side is shorter than half, the other claims the
       * leftover.  Keeps the value column from steamrolling a long
       * title (and vice versa). */
      if (!has_value)
      {
         title_budget = avail;
         value_budget = 0;
      }
      else if (title_natural + value_natural <= avail)
      {
         title_budget = title_natural;
         value_budget = value_natural;
      }
      else
      {
         int half = avail / 2;
         if (title_natural <= half)
         {
            title_budget = title_natural;
            value_budget = avail - title_natural;
         }
         else if (value_natural <= half)
         {
            value_budget = value_natural;
            title_budget = avail - value_natural;
         }
         else
         {
            title_budget = half;
            value_budget = avail - half;
         }
      }

      /* Truncate each to its budget, then measure the actual width
       * for placement.  truncate_to_width is a no-op when the text
       * already fits. */
      downplay_truncate_to_width(row_font, title_buf,
            sizeof(title_buf), title_budget);
      if (has_value)
         downplay_truncate_to_width(row_font, value_buf,
               sizeof(value_buf), value_budget);

      text_y = downplay_baseline_y(row_y, L->settings_row_h, row_centre);

      /* Right-aligned value text first — measured here too so we
       * know value_w for any callers below (currently unused, but
       * cheap to keep for future row chrome). */
      if (has_value)
      {
         int value_x = block_x + block_w - L->row_text_indent;
         value_w = font_driver_get_message_width(row_font, value_buf,
               (unsigned)strlen(value_buf), 1.0f);
         if (value_w < 0)
            value_w = 0;
         downplay_draw_text(row_font, value_buf,
               (float)value_x, (float)text_y,
               L, DP_TEXT_LIGHT, TEXT_ALIGN_RIGHT);
      }
      (void)value_w;

      /* Title — selected gets a white pill behind it, sized to the
       * truncated text (with row_text_indent padding on each side).
       * Pill's left cap aligns with the gray background's left cap
       * so the two read as concentric. */
      title_color = selected ? DP_TEXT_DARK : DP_TEXT_LIGHT;
      text_w = font_driver_get_message_width(row_font, title_buf,
            (unsigned)strlen(title_buf), 1.0f);
      if (text_w < 0)
         text_w = 0;
      if (selected)
      {
         int pill_w  = text_w + 2 * L->row_text_indent;
         if (pill_w < L->settings_row_h)
            pill_w = L->settings_row_h;
         downplay_draw_pill(p_disp, userdata, L, dp->pill_cap_tex,
               block_x, row_y, pill_w, L->settings_row_h,
               DP_COLOR_PILL_LIGHT);
      }
      downplay_draw_text(row_font, title_buf,
            (float)(block_x + L->row_text_indent), (float)text_y,
            L, title_color, TEXT_ALIGN_LEFT);
   }

   /* Scroll chevrons — only when there's content above / below the
    * visible window.  Drawn just outside the row block in muted gray. */
   if (S->scroll > 0)
      downplay_draw_chevron(p_disp, userdata, L,
            block_x + block_w / 2,
            block_top - (int)(12.0f * L->scale), -1);
   if (S->scroll + visible < S->row_count)
      downplay_draw_chevron(p_disp, userdata, L,
            block_x + block_w / 2,
            block_top + block_h + (int)(4.0f * L->scale), +1);

   /* Description band: fixed at the bottom, above the footer hint
    * row.  Two-line max — we naive-wrap on the first whitespace past
    * the midpoint that fits, ellipsing line two if it overflows. */
   if (sel_row && sel_row->desc && *sel_row->desc)
   {
      char        line1[256];
      char        line2[256];
      int         line_w     = (int)L->vid_w - 2 * L->margin_x;
      int         desc_y_top = (int)L->vid_h - L->margin_y - desc_band_h;
      int         line1_y;
      int         line2_y;
      const char *src        = sel_row->desc;
      size_t      src_len    = strlen(src);
      int         full_w     = font_driver_get_message_width(dp->chrome_font,
            src, (unsigned)src_len, 1.0f);

      line1[0] = line2[0] = '\0';
      if (full_w >= 0 && full_w <= line_w)
         strlcpy(line1, src, sizeof(line1));
      else
      {
         /* Find the longest prefix ending on a space that fits.
          * Linear scan is fine — descriptions are at most a couple
          * hundred chars and this runs once per frame. */
         size_t cut = 0;
         size_t i2;
         for (i2 = 1; i2 <= src_len; i2++)
         {
            int w;
            char ch = src[i2];
            if (ch != '\0' && ch != ' ')
               continue;
            {
               char tmp[256];
               size_t take = i2;
               if (take >= sizeof(tmp))
                  break;
               memcpy(tmp, src, take);
               tmp[take] = '\0';
               w = font_driver_get_message_width(dp->chrome_font,
                     tmp, (unsigned)take, 1.0f);
            }
            if (w >= 0 && w <= line_w)
               cut = i2;
            else
               break;
         }
         if (cut == 0)
         {
            /* No fit-on-space — fall back to char-truncated single
             * line + ellipsis. */
            strlcpy(line1, src, sizeof(line1));
            downplay_truncate_to_width(dp->chrome_font, line1,
                  sizeof(line1), line_w);
         }
         else
         {
            size_t take = cut < sizeof(line1) ? cut : sizeof(line1) - 1;
            memcpy(line1, src, take);
            line1[take] = '\0';
            /* Skip the trailing space we cut on, then second line. */
            while (src[cut] == ' ')
               cut++;
            strlcpy(line2, src + cut, sizeof(line2));
            downplay_truncate_to_width(dp->chrome_font, line2,
                  sizeof(line2), line_w);
         }
      }

      line1_y = downplay_baseline_y(desc_y_top,
            (int)L->chrome_font_size + (int)(4.0f * L->scale), desc_centre);
      downplay_draw_text(dp->chrome_font, line1,
            (float)L->vid_w / 2.0f, (float)line1_y,
            L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
      if (*line2)
      {
         line2_y = line1_y + (int)(L->chrome_font_size * 1.2f);
         downplay_draw_text(dp->chrome_font, line2,
               (float)L->vid_w / 2.0f, (float)line2_y,
               L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
      }
   }
}

/* ---------- settings: builders ---------- */

#include "../../core_option_manager.h"

/* Per-row userdata for a core-option row.  The handle and option
 * index together let on_change call core_option_manager_adjust_val
 * without re-fetching the manager every time.  `dp` is along for
 * the ride so on_change can flip the rebuild-pending flag when
 * the core's update_display callback flipped option visibility. */
typedef struct
{
   core_option_manager_t *opt;
   size_t                 idx;
   downplay_handle_t     *dp;
} downplay_core_opt_row_ud_t;

/* Per-list userdata payload holding both the core-options manager
 * pointer (shared by all rows) and the synthesized values arrays
 * (one const char** per row).  Allocated as one trailing block so
 * downplay_settings_list_free's `free(userdata_pool)` reclaims
 * everything in a single call.
 *
 * Layout in `userdata_pool`:
 *   [N × downplay_core_opt_row_ud_t][N × const char* arrays...]
 * Where N = number of visible options.  rows[i].userdata points at
 * &per_row[i]; rows[i].values points into the values block. */

static void downplay_core_opt_on_change(int delta, void *userdata)
{
   downplay_core_opt_row_ud_t *ud = (downplay_core_opt_row_ud_t*)userdata;
   if (!ud || !ud->opt)
      return;
   /* Direct call rather than RARCH_CTL_CORE_OPTION_NEXT/PREV:
    * adjust_val takes the index we already have, vs the ctl
    * variants which look up by the global state_slot-style cursor
    * we don't use.  refresh_menu=true so any visibility-change
    * callback in the core fires. */
   core_option_manager_adjust_val(ud->opt, ud->idx, delta, true);
   /* If that callback toggled visibility on other options, rebuild
    * the list at the next safe point.  Doing it here would free the
    * row userdata we're standing on.  The handler clears `updated`
    * after consuming it; otherwise every adjust would trigger a
    * rebuild when the core hasn't actually moved any visibility. */
   if (ud->opt->updated && ud->dp)
      ud->dp->settings_rebuild_pending = true;
}

static downplay_settings_list_t *downplay_build_core_options_list(
      downplay_handle_t *dp);

/* Push the core-options list — invoked by the Emulator nav row's
 * on_confirm. */
static void downplay_action_open_core_options(void *userdata)
{
   downplay_handle_t        *dp = (downplay_handle_t*)userdata;
   downplay_settings_list_t *L  = downplay_build_core_options_list(dp);
   if (L)
      downplay_settings_push(dp, L);
}

/* Stub list — placeholder used by Frontend / Controls / Shortcuts
 * until they have real implementations.  One nav row that just
 * cancels back. */
static downplay_settings_list_t *downplay_build_stub_list(const char *title)
{
   downplay_settings_list_t *L = downplay_settings_list_new(title, 1, 0, 50);
   if (!L)
      return NULL;
   L->rows[0].title      = "Coming soon";
   L->rows[0].desc       = "Not yet implemented in this build.";
   return L;
}

static void downplay_action_open_frontend(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_settings_push(dp, downplay_build_stub_list("Frontend"));
}

static void downplay_action_open_controls(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_settings_push(dp, downplay_build_stub_list("Controls"));
}

static void downplay_action_open_shortcuts(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_settings_push(dp, downplay_build_stub_list("Shortcuts"));
}

/* Save Changes: flush the core-options manager to its config file
 * and pop the entire settings stack so the user lands back in the
 * in-game menu. */
static void downplay_action_save_changes(void *userdata)
{
   downplay_handle_t     *dp   = (downplay_handle_t*)userdata;
   core_option_manager_t *opts = NULL;
   retroarch_ctl(RARCH_CTL_CORE_OPTIONS_LIST_GET, &opts);
   if (opts && opts->conf)
      core_option_manager_flush(opts, opts->conf);
   downplay_settings_pop_all(dp);
}

/* Build the root Options list.  Static row labels live in .rodata,
 * so no per-row userdata pool needed (each on_confirm takes `dp`
 * directly). */
static downplay_settings_list_t *downplay_build_root_options_list(
      downplay_handle_t *dp)
{
   downplay_settings_list_t *L =
      downplay_settings_list_new("Options", 5, 0, 60);
   if (!L)
      return NULL;
   L->rows[0].title      = "Frontend";
   L->rows[0].on_confirm = downplay_action_open_frontend;
   L->rows[0].userdata   = dp;
   L->rows[1].title      = "Emulator";
   L->rows[1].desc       = "Adjust the running core's options.";
   L->rows[1].on_confirm = downplay_action_open_core_options;
   L->rows[1].userdata   = dp;
   L->rows[2].title      = "Controls";
   L->rows[2].on_confirm = downplay_action_open_controls;
   L->rows[2].userdata   = dp;
   L->rows[3].title      = "Shortcuts";
   L->rows[3].on_confirm = downplay_action_open_shortcuts;
   L->rows[3].userdata   = dp;
   L->rows[4].title      = "Save Changes";
   L->rows[4].desc       = "Persist core option changes to disk.";
   L->rows[4].on_confirm = downplay_action_save_changes;
   L->rows[4].userdata   = dp;
   return L;
}

/* Build a settings list mirroring the running core's option set.
 * Categories (v2) ignored on the first cut — flat list.  Hidden
 * options are skipped on enumeration; the Emulator nav row will
 * already have decided to push us by then, so an all-hidden manager
 * legitimately renders an empty list (gets the "No options" fallback
 * via downplay_build_no_options_list below). */
static downplay_settings_list_t *downplay_build_no_options_list(void)
{
   downplay_settings_list_t *L =
      downplay_settings_list_new("Emulator", 1, 0, 50);
   if (!L)
      return NULL;
   L->rows[0].title = "No options";
   L->rows[0].desc  = "This core exposes no configurable options.";
   return L;
}

static downplay_settings_list_t *downplay_build_core_options_list(
      downplay_handle_t *dp)
{
   core_option_manager_t        *opts = NULL;
   downplay_settings_list_t     *L;
   downplay_core_opt_row_ud_t   *row_uds;
   const char                  **values_pool;
   size_t                        visible_count = 0;
   size_t                        total_values  = 0;
   size_t                        i;
   size_t                        out_row       = 0;
   size_t                        values_offset = 0;
   size_t                        ud_bytes;

   retroarch_ctl(RARCH_CTL_CORE_OPTIONS_LIST_GET, &opts);
   if (!opts || opts->size == 0)
      return downplay_build_no_options_list();

   /* Two-pass: count visible rows + total value strings to size the
    * userdata pool exactly.  Keeps the values array contiguous in
    * one allocation. */
   for (i = 0; i < opts->size; i++)
   {
      if (!core_option_manager_get_visible(opts, i))
         continue;
      visible_count++;
      if (opts->opts[i].vals)
         total_values += opts->opts[i].vals->size;
   }
   if (visible_count == 0)
      return downplay_build_no_options_list();

   ud_bytes  = visible_count * sizeof(downplay_core_opt_row_ud_t)
             + total_values  * sizeof(const char*);
   /* Core options use the full content width — these strings tend to
    * be long (especially "On / Off / Lenient / Strict"-style enums)
    * and the launcher's outer margins are still respected. */
   L = downplay_settings_list_new("Emulator", visible_count, ud_bytes, 100);
   if (!L)
      return NULL;
   row_uds     = (downplay_core_opt_row_ud_t*)L->userdata_pool;
   values_pool = (const char**)((char*)L->userdata_pool
         + visible_count * sizeof(downplay_core_opt_row_ud_t));

   for (i = 0; i < opts->size && out_row < visible_count; i++)
   {
      struct core_option        *o = &opts->opts[i];
      struct string_list        *src;
      size_t                     k;
      downplay_settings_row_t   *r;

      if (!core_option_manager_get_visible(opts, i))
         continue;
      r              = &L->rows[out_row];
      /* Prefer desc_categorized when the core publishes v2 options
       * with category prefixes — disambiguates "Resolution" across
       * Video / Audio etc.  Falls back to plain desc / key. */
      r->title       = core_option_manager_get_desc(opts, i, true);
      if (!r->title || !*r->title)
         r->title    = o->key ? o->key : "";
      r->desc        = o->info;
      /* Prefer val_labels (human-friendly) over vals (raw value
       * strings).  Either may be NULL — render an empty value
       * column in that case.
       *
       * Visibility can shift between the count pass and this build
       * pass (the core's update_display callback runs against the
       * live manager, which other code may have called between
       * the two loops).  Guard `values_offset` against overrunning
       * the pool we sized in pass 1 — a newly-visible option here
       * would otherwise walk past the buffer end. */
      src            = o->val_labels ? o->val_labels : o->vals;
      if (src && src->size > 0
            && values_offset + src->size <= total_values)
      {
         for (k = 0; k < src->size; k++)
            values_pool[values_offset + k] = src->elems[k].data;
         r->values       = values_pool + values_offset;
         r->values_count = src->size;
         values_offset  += src->size;
      }
      /* Clamp against a stale o->index that might be ≥ values_count
       * (rare — shouldn't happen with a well-behaved core, but
       * cycling code does unguarded modulo and would misbehave
       * when r->values_count is 0). */
      r->idx_value    = (r->values_count > 0 && o->index < r->values_count)
                        ? o->index : 0;
      r->on_change    = downplay_core_opt_on_change;
      row_uds[out_row].opt = opts;
      row_uds[out_row].idx = i;
      row_uds[out_row].dp  = dp;
      r->userdata     = &row_uds[out_row];
      out_row++;
   }
   /* Defensive: if the visibility flags shifted between the count
    * pass and the build pass (multithreaded core?), trim the row
    * count to what we actually populated.  Excess values pool space
    * is harmless. */
   L->row_count = out_row;
   return L;
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

/* Right-pane preview: draw `tex` (texture_w × texture_h) centred in
 * the right half of the screen with a light placeholder when the
 * texture is 0.  Reusable for any future view that wants a one-image
 * preview of the selected list entry.
 *
 * Pane geometry: the right half minus the outer right margin
 * horizontally; vertically matches the list area (below the status
 * row, above the footer hint).
 *
 * Scaling policy is a parameter so different content types can
 * request the right behaviour — savestate thumbs are typically the
 * core's native framebuffer (e.g. 256×240 NES) and want crisp
 * integer scaling; cover art / hand-drawn assets want smooth aspect-
 * fit; tiny icons might want to render at 1:1. */
enum downplay_preview_scale
{
   /* Aspect-preserving fit: scale up or down so one axis touches the
    * pane edge.  Bilinear-smoothed.  Use for non-pixel art. */
   DOWNPLAY_PREVIEW_SCALE_FIT = 0,
   /* Native pixels when the texture fits; aspect-fit shrink when it
    * overflows.  Never scales up. */
   DOWNPLAY_PREVIEW_SCALE_NATIVE,
   /* Largest integer multiplier (1×, 2×, 3×…) that fits in the pane;
    * preserves aspect implicitly because both axes scale by the same
    * factor.  Falls back to NATIVE / aspect-fit when even 1× doesn't
    * fit.  Right choice for retro framebuffers. */
   DOWNPLAY_PREVIEW_SCALE_INTEGER
};

static float DP_COLOR_PREVIEW_PLACEHOLDER[16] = {
   1.0f, 1.0f, 1.0f, 0.06f,   1.0f, 1.0f, 1.0f, 0.06f,
   1.0f, 1.0f, 1.0f, 0.06f,   1.0f, 1.0f, 1.0f, 0.06f
};

static void downplay_draw_right_preview(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L,
      uintptr_t tex, unsigned texture_w, unsigned texture_h,
      enum downplay_preview_scale scale)
{
   int pane_left   = (int)L->vid_w / 2;
   int pane_right  = (int)L->vid_w - L->margin_x;
   int pane_top    = L->margin_y + L->row_h + (int)(16.0f * L->scale);
   int pane_bottom = (int)L->vid_h - L->margin_y - L->row_h;
   int pane_w      = pane_right  - pane_left;
   int pane_h      = pane_bottom - pane_top;
   int img_w       = 0;
   int img_h       = 0;
   int img_x;
   int img_y;
   bool fits_natively;

   if (pane_w < L->row_h || pane_h < L->row_h)
      return;

   if (tex && texture_w && texture_h)
   {
      fits_natively = (texture_w <= (unsigned)pane_w
                   && texture_h <= (unsigned)pane_h);

      if (scale == DOWNPLAY_PREVIEW_SCALE_INTEGER && fits_natively)
      {
         /* Largest integer multiplier where both axes still fit. */
         int mx = pane_w / (int)texture_w;
         int my = pane_h / (int)texture_h;
         int m  = mx < my ? mx : my;
         if (m < 1)
            m = 1;
         img_w = (int)texture_w * m;
         img_h = (int)texture_h * m;
      }
      else if (scale == DOWNPLAY_PREVIEW_SCALE_NATIVE && fits_natively)
      {
         img_w = (int)texture_w;
         img_h = (int)texture_h;
      }
      else
      {
         /* Aspect-fit (FIT, or NATIVE/INTEGER fallback when too big).
          * Cross-product comparison avoids float math + zero-div. */
         if ((unsigned)pane_w * texture_h <= (unsigned)pane_h * texture_w)
         {
            img_w = pane_w;
            img_h = (int)((unsigned)pane_w * texture_h / texture_w);
         }
         else
         {
            img_h = pane_h;
            img_w = (int)((unsigned)pane_h * texture_w / texture_h);
         }
      }
   }
   else
   {
      /* Placeholder: square at min(pane_w, pane_h), gives a hint of
       * "preview goes here" without claiming a particular aspect. */
      img_w = img_h = (pane_w < pane_h) ? pane_w : pane_h;
   }
   if (img_w < 1 || img_h < 1)
      return;

   img_x = pane_left + (pane_w - img_w) / 2;
   img_y = pane_top  + (pane_h - img_h) / 2;

   if (tex)
   {
      /* DP_COLOR_PILL_LIGHT is pure white (1,1,1,1) — gfx_display_draw_quad
       * multiplies the texture sample by this colour, so passing white
       * is the no-tint identity (texture renders verbatim).  Local
       * uintptr_t copy because the function signature is non-const. */
      uintptr_t tex_local = tex;
      gfx_display_draw_quad(p_disp, userdata,
            L->vid_w, L->vid_h,
            img_x, img_y, (unsigned)img_w, (unsigned)img_h,
            L->vid_w, L->vid_h,
            DP_COLOR_PILL_LIGHT, &tex_local);
   }
   else
      downplay_draw_rect(p_disp, userdata, L,
            img_x, img_y, img_w, img_h, DP_COLOR_PREVIEW_PLACEHOLDER);
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
   int      list_bot   = (int)L->vid_h - L->margin_y - L->row_h;
   int      avail_h    = list_bot - list_top;
   /* Capacity of the visible window in whole rows.  Clamped to >=1 so a
    * window too short for one row still shows the selected row rather
    * than nothing. */
   size_t   visible    = (avail_h > L->row_h)
                       ? (size_t)(avail_h / L->row_h) : 1;
   /* Scroll offset derived from selection — keep the selected row in
    * view by sliding the window forward once selection passes the last
    * visible index.  Source of truth is dp->selection; deriving scroll
    * here means input handlers don't need to track it. */
   size_t   scroll     = 0;
   size_t   row_idx;
   int      row_y;

   if (dp->total_rows > visible && dp->selection >= visible)
   {
      scroll = dp->selection + 1 - visible;
      if (scroll + visible > dp->total_rows)
         scroll = dp->total_rows - visible;
   }

   for (i = 0; i < visible && scroll + i < dp->total_rows; i++)
   {
      row_idx = scroll + i;
      row_y   = list_top + (int)(i * (size_t)L->row_h);
      downplay_draw_list_row(p_disp, userdata, dp->font,
            dp->font_centre_offset, L, cap_tex,
            list_x, row_y, row_max_w,
            downplay_row_label(dp, row_idx),
            row_idx == dp->selection);
   }

   /* Right-pane preview: SAVE_PICKER shows the selected save's
    * thumbnail.  Easy to extend to other views later — just plumb a
    * texture handle from whatever the selected entry refers to. */
   if (dp->view == DOWNPLAY_VIEW_SAVE_PICKER
         && dp->selection < dp->save_picker_count)
   {
      const downplay_save_entry_t *e = &dp->save_picker[dp->selection];
      downplay_draw_right_preview(p_disp, userdata, L,
            e->thumb_tex, e->thumb_w, e->thumb_h,
            DOWNPLAY_PREVIEW_SCALE_INTEGER);
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

   /* SAVE_PICKER is a sub-view of INGAME (only reachable from there),
    * so don't bounce it back — that snap was the bug where opening
    * Load did nothing.  The picker tears itself down on Cancel /
    * post-load, returning to INGAME via prior_view. */
   if (running
         && dp->view != DOWNPLAY_VIEW_INGAME
         && dp->view != DOWNPLAY_VIEW_SAVE_PICKER
         && dp->view != DOWNPLAY_VIEW_SETTINGS)
   {
      dp->prior_view      = dp->view;
      dp->prior_selection = dp->selection;
      dp->view            = DOWNPLAY_VIEW_INGAME;
      dp->selection       = 0;
      downplay_refresh_ingame_actions(dp);
      downplay_recompute_total_rows(dp);
   }
   else if (!running && dp->view == DOWNPLAY_VIEW_INGAME)
   {
      dp->view      = dp->prior_view;
      dp->selection = dp->prior_selection;
      /* Refresh launcher state that would have changed during play —
       * a new autosave (set by RA on unload) should make the Resume
       * row appear next frame. */
      downplay_refresh_resume(dp);
      downplay_recompute_total_rows(dp);
   }
   else if (!running && dp->view == DOWNPLAY_VIEW_SAVE_PICKER)
   {
      /* Edge case: core unloaded while we were drilled into the
       * picker (crashed core, external Quit).  Pop back to TOP
       * cleanly.  Note this restores to TOP directly rather than via
       * prior_view (which would be INGAME — invalid now that the core
       * is gone).  prior_view is single-slot; if a future view ever
       * stacks on top of SAVE_PICKER, that slot will need to become a
       * small stack. */
      downplay_save_picker_free(dp);
      dp->view      = DOWNPLAY_VIEW_TOP;
      dp->selection = 0;
      downplay_refresh_resume(dp);
      downplay_recompute_total_rows(dp);
   }
   else if (!running && dp->view == DOWNPLAY_VIEW_SETTINGS)
   {
      /* Same edge case as SAVE_PICKER: core died while in Options.
       * Tear down the settings stack and snap to TOP. */
      downplay_settings_pop_all(dp);
      dp->view      = DOWNPLAY_VIEW_TOP;
      dp->selection = 0;
      downplay_refresh_resume(dp);
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

   /* Core flipped option visibility on the previous frame's cycle.
    * Rebuild the active core-options list in place — same stack
    * position, preserve selection by index (best-effort: if the
    * row count shrinks past sel, snap_scroll clamps it). */
   if (dp->settings_rebuild_pending)
   {
      core_option_manager_t *opts = NULL;
      retroarch_ctl(RARCH_CTL_CORE_OPTIONS_LIST_GET, &opts);
      if (opts)
         opts->updated = false;
      if (dp->view == DOWNPLAY_VIEW_SETTINGS && dp->settings_depth > 0)
      {
         downplay_settings_list_t *cur = downplay_settings_top(dp);
         downplay_settings_list_t *neu =
               downplay_build_core_options_list(dp);
         if (neu)
         {
            size_t saved_sel    = cur ? cur->sel    : 0;
            size_t saved_scroll = cur ? cur->scroll : 0;
            downplay_settings_list_free(cur);
            if (saved_sel >= neu->row_count && neu->row_count > 0)
               saved_sel = neu->row_count - 1;
            neu->sel    = saved_sel;
            neu->scroll = saved_scroll;
            dp->settings_stack[dp->settings_depth - 1] = neu;
            downplay_settings_snap_scroll(dp, neu);
         }
      }
      dp->settings_rebuild_pending = false;
   }
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
         if (dp->view == DOWNPLAY_VIEW_SETTINGS)
            downplay_draw_settings_view(p_disp, userdata, dp);
         else
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
    * give the chrome a coherent base.
    *
    * SETTINGS view hides the footer entirely so the row block + the
    * fixed description band can extend to the bottom margin. */
   if (dp->view == DOWNPLAY_VIEW_SETTINGS)
      return;
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

   /* SETTINGS view drives its own selection cursor on the active
    * stack frame, not dp->selection — handled before the generic
    * up/down code below to avoid two cursors fighting. */
   if (dp->view == DOWNPLAY_VIEW_SETTINGS)
   {
      downplay_settings_list_t *S = downplay_settings_top(dp);
      if (!S || S->row_count == 0)
      {
         if (action == MENU_ACTION_CANCEL)
            downplay_settings_pop(dp);
         return 0;
      }
      switch (action)
      {
         case MENU_ACTION_UP:
            S->sel = (S->sel + S->row_count - 1) % S->row_count;
            downplay_settings_snap_scroll(dp, S);
            return 0;
         case MENU_ACTION_DOWN:
            S->sel = (S->sel + 1) % S->row_count;
            downplay_settings_snap_scroll(dp, S);
            return 0;
         case MENU_ACTION_LEFT:
         case MENU_ACTION_RIGHT:
         {
            downplay_settings_row_t *r = &S->rows[S->sel];
            int    delta = (action == MENU_ACTION_LEFT) ? -1 : +1;
            size_t inc;
            if (!r->values || r->values_count == 0)
               return 0;
            inc = (delta < 0) ? (r->values_count - 1) : 1;
            r->idx_value = (r->idx_value + inc) % r->values_count;
            if (r->on_change)
               r->on_change(delta, r->userdata);
            return 0;
         }
         case MENU_ACTION_OK:
         case MENU_ACTION_SELECT:
         {
            downplay_settings_row_t *r = &S->rows[S->sel];
            if (r->on_confirm)
               r->on_confirm(r->userdata);
            return 0;
         }
         case MENU_ACTION_CANCEL:
            downplay_settings_pop(dp);
            return 0;
         default:
            return 0;
      }
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
            enum downplay_ingame_action act;
            if (dp->selection >= dp->ingame_action_count)
               return 0;
            act = dp->ingame_actions[dp->selection];
            switch (act)
            {
               case DP_INGAME_CONTINUE:
                  command_event(CMD_EVENT_MENU_TOGGLE, NULL);
                  break;
               case DP_INGAME_SAVE:
                  downplay_save_to_slot(downplay_pick_next_save_slot());
                  /* Refresh row composition: zero→one save means Load
                   * appears next time the menu opens. */
                  downplay_refresh_ingame_actions(dp);
                  downplay_recompute_total_rows(dp);
                  /* Hide the menu after Save so the user can resume
                   * play without an extra Continue press. */
                  command_event(CMD_EVENT_MENU_TOGGLE, NULL);
                  break;
               case DP_INGAME_LOAD:
               {
                  downplay_save_entry_t scratch[DOWNPLAY_MAX_SAVE_ENTRIES];
                  size_t                scratch_count = 0;
                  size_t                manual_count  = 0;
                  size_t                i;
                  int                   only_slot     = 0;

                  /* Re-enumerate (without thumbs) to count manual
                   * saves — could have changed since view enter
                   * (background autosave, manual external delete). */
                  downplay_savestate_enumerate(scratch, &scratch_count, false);
                  for (i = 0; i < scratch_count; i++)
                  {
                     if (scratch[i].slot < 0)
                        continue;
                     if (manual_count == 0)
                        only_slot = scratch[i].slot;
                     manual_count++;
                  }
                  if (manual_count == 0)
                     break;       /* race: row hidden next frame */
                  if (manual_count == 1)
                  {
                     downplay_load_from_slot(only_slot);
                     command_event(CMD_EVENT_MENU_TOGGLE, NULL);
                     break;
                  }
                  /* >1: open the picker.  Loads thumbnails synchronously
                   * (~10-50 ms one-shot hitch on view enter is fine for
                   * a deliberate user action). */
                  downplay_save_picker_free(dp);
                  downplay_savestate_enumerate(dp->save_picker,
                        &dp->save_picker_count, true);
                  dp->prior_view      = dp->view;
                  dp->prior_selection = dp->selection;
                  dp->view            = DOWNPLAY_VIEW_SAVE_PICKER;
                  dp->selection       = 0;
                  downplay_recompute_total_rows(dp);
                  break;
               }
               case DP_INGAME_OPTIONS:
                  downplay_settings_push(dp,
                        downplay_build_root_options_list(dp));
                  break;
               case DP_INGAME_QUIT:
                  command_event(CMD_EVENT_UNLOAD_CORE, NULL);
                  break;
            }
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_SAVE_PICKER)
         {
            if (dp->selection < dp->save_picker_count)
            {
               int slot = dp->save_picker[dp->selection].slot;
               downplay_load_from_slot(slot);
               /* Pop back to INGAME so re-opening the menu doesn't
                * re-enter the picker against textures we already
                * freed.  CMD_EVENT_LOAD_STATE doesn't unload the
                * core, so the user resumes play directly. */
               downplay_save_picker_free(dp);
               dp->view      = dp->prior_view;
               dp->selection = dp->prior_selection;
               downplay_refresh_ingame_actions(dp);
               downplay_recompute_total_rows(dp);
               command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            }
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
         /* TOP view: Resume → recents-launch the head; Recents → open;
          * else → drill into a system.  Order mirrors row layout in
          * downplay_row_label. */
         {
            size_t sel = dp->selection;
            if (downplay_has_resume_row(dp))
            {
               if (sel == 0)
               {
                  downplay_launch_recent(dp->resume_pl_idx);
                  return 0;
               }
               sel--;
            }
            if (downplay_has_recents_row(dp) && sel == 0)
            {
               downplay_open_recents(dp);
               return 0;
            }
            {
               size_t sys_idx = downplay_has_recents_row(dp) ? sel - 1 : sel;
               downplay_open_system(dp, sys_idx);
            }
         }
         return 0;
      case MENU_ACTION_CANCEL:
         if (dp->view == DOWNPLAY_VIEW_INGAME)
         {
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            return 0;
         }
         if (dp->view == DOWNPLAY_VIEW_SAVE_PICKER)
         {
            /* Back to INGAME: free thumbnails, restore prior cursor. */
            downplay_save_picker_free(dp);
            dp->view      = dp->prior_view;
            dp->selection = dp->prior_selection;
            downplay_refresh_ingame_actions(dp);
            downplay_recompute_total_rows(dp);
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
   /* Picker thumbnails are GPU resources; drop them on context loss
    * so they're not dangling handles after a renderer reset. */
   downplay_save_picker_free(dp);
   /* Settings stack is CPU-only state, but its row data may include
    * pointers into the core options manager — which is owned by RA
    * and may be torn down before us.  Easier to clear here than to
    * track its lifetime. */
   downplay_settings_clear(dp);
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
