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
#include <string/stdstring.h>

#include "../menu_driver.h"
#include "../menu_shader.h"
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
#include "../../downplay/downplay_display_name.h"
#include "../../downplay/downplay_metadata.h"
#include "../../downplay/downplay_nav.h"
#include "../../downplay/downplay_setup.h"
#include "../../downplay/downplay_thumbs.h"
#ifdef HAVE_DOWNPLAY_WEBP
#include "../../downplay/downplay_webp.h"
#endif

#include <features/features_cpu.h>

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
 * by the time a downplay_system_t exists, display_name + core_ident +
 * full_path are non-empty.  db_name is the canonical system name (e.g.
 * "Nintendo - Super Nintendo Entertainment System"), used to key the
 * Downplay thumbnail index + cache subdir.  May be NULL when
 * the user's folder name doesn't hit the disambiguation table AND no
 * matching core_info entry is loaded. */
typedef struct
{
   char *display_name;
   char *core_ident;
   char *full_path;
   char *db_name;
} downplay_system_t;

/* One ROM file inside a system folder, expanded for the system view.
 *
 * mtime / size are stat()'d once at scan time and used as the index's
 * validity stamps — drop a different ROM at the same path and the
 * cached label/match invalidate automatically.
 *
 * thumbnail is owned by this struct.  Initialized to blank in scan,
 * loaded on hover (WebP via downplay_webp_load_texture, JPG/PNG via
 * gfx_thumbnail_request_file once downplay_thumbs_request returns a
 * local path), freed by gfx_thumbnail_reset in downplay_roms_free.
 * Callers MUST have
 * called gfx_thumbnail_cancel_pending_requests() before any reset to
 * avoid a callback racing onto freed memory. */
typedef struct
{
   char            *display_name;   /* cleaned: parens/brackets stripped */
   char            *sort_key;       /* lowercased + leading-article stripped */
   char            *full_path;
   int64_t          mtime;          /* st_mtime, 0 on stat failure */
   int64_t          size;           /* st_size,  0 on stat failure */
   gfx_thumbnail_t  thumbnail;
   /* Per-row image dimensions from the thumbnail index, in pixels.
    * Both 0 means either: (a) the index isn't loaded yet (cold first
    * visit to this system; populated later when the pump flips
    * load_state to READY) or (b) this ROM has no match in the index.
    * The row drawer treats (a) and (b) identically — full-width text,
    * no art slot reserved.  Non-zero pair → row narrows the text by
    * the actual image width derived from its aspect ratio against
    * the right-pane height budget (see downplay_image_pane_width). */
   uint16_t         image_w;
   uint16_t         image_h;
} downplay_rom_t;

/* One row in the recents view.  pl_idx is the entry's index in
 * g_defaults.content_history at build time — kept around because we
 * skip rows whose entries have neither a label nor a usable path, so
 * the array index would otherwise drift from the playlist index.
 *
 * db_name + rom_basename are populated at build time when both can
 * be derived from the playlist entry (ROM path inside a Downplay-
 * convention system folder, resolvable via the disambig table).  If
 * either is NULL the row simply renders without art — recents never
 * kicks downloads, so an unresolvable system is silently artless. */
typedef struct
{
   char           *display_name;
   char           *db_name;       /* resolved canonical system name; may be NULL */
   char           *rom_basename;  /* basename of the ROM file path; may be NULL */
   gfx_thumbnail_t thumbnail;     /* loaded on hover when a cached image exists */
   size_t          pl_idx;
   /* Per-row image dimensions from the per-system thumbnail index, in
    * pixels.  0/0 means either: (a) the row's system index isn't on
    * disk yet (recents pump hasn't loaded it; populated when it does)
    * or (b) the cascade misses on this ROM.  Same semantics as
    * downplay_rom_t.image_w/h — drives the row drawer's truncation
    * gate so unselected rows narrow before the preview pane. */
   uint16_t        image_w;
   uint16_t        image_h;
} downplay_recent_t;
/* No sort_key on recents — they're presented in chronological order
 * straight from g_defaults.content_history, never alphabetized. */

/* enum downplay_view, dp_nav_frame_t, dp_nav_state_t and the
 * dp_nav_* helpers all live in downplay/downplay_nav.h (included
 * above).  The handle below embeds a dp_nav_state_t — every screen
 * the user navigates into is a frame on that stack, with the bottom
 * always being TOP.  Reads of the active view + cursor go through
 * dp->nav.view / dp->nav.selection (cached mirrors of the top
 * frame).  Mutations all go through the dp_nav_* helpers so the
 * cache can't drift. */

/* downplay_handle_t is referenced as a forward decl in nav.h dispose
 * hook signatures (typed via void* there), but defined below. */
typedef struct downplay_handle_s downplay_handle_t;

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

/* Forward decl so on_close can name the typedef. */
typedef struct downplay_settings_list downplay_settings_list_t;

/* One settings list.  Stack-allocated when small / static (root
 * Options); heap-allocated when row count is dynamic (core options). */
struct downplay_settings_list
{
   const char              *title;       /* reserved for future header */
   downplay_settings_row_t *rows;        /* owned: free()d on list dispose */
   size_t                   row_count;
   /* Optional userdata pool that backs row->userdata pointers.  Owned;
    * free()d alongside rows.  NULL when no row needs per-row allocated
    * userdata (e.g. root Options list — its on_confirm handlers take
    * dp itself). */
   void                    *userdata_pool;
   /* Optional flush callback fired by downplay_settings_pop just
    * before the list is freed.  Used when row changes are staged in
    * row state during navigation and committed once on back-out (the
    * Frontend submenu does this so scrolling Effect doesn't recompile
    * shaders on every L/R press).  on_close_userdata is
    * passed verbatim. */
   void                   (*on_close)(downplay_settings_list_t *L,
                                      void *userdata);
   void                    *on_close_userdata;
   unsigned                 width_pct;   /* % of vid_w */
   size_t                   sel;
   size_t                   scroll;      /* topmost visible row index */
};

/* Aspect-ratio breakpoints for layout decisions that should respond to
 * screen shape, not just absolute size.  Ordered so `>=` comparisons mean
 * "this aspect or wider", and exact matches target a single device class.
 * Thresholds chosen to land each common handheld in its own bucket:
 *
 *   square handhelds (Anbernic RG CubeXX 1:1) → DP_AR_SQUARE
 *   classic 4:3 / 5:4                          → DP_AR_CLASSIC
 *   3:2 / 16:10                                → DP_AR_WIDE
 *   16:9 and wider                             → DP_AR_HD
 *
 * Add new layout knobs by switching on this enum rather than re-deriving
 * thresholds inline. */
enum downplay_aspect_bp
{
   DP_AR_SQUARE = 0,
   DP_AR_CLASSIC,
   DP_AR_WIDE,
   DP_AR_HD
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
   /* Last user_scale_factor used to derive `scale`.  Tracked so the
    * change-detector can compare the integer/source inputs that *drive*
    * the layout (vid_w, vid_h, user_scale_factor) instead of comparing
    * the derived `scale` float, which is unreliable across recompute
    * paths if intermediate FPU precision differs. */
   float scale_factor;
   unsigned vid_w;
   unsigned vid_h;
   enum downplay_aspect_bp aspect_bp;
   /* Left edge of the right-pane preview area, in pixels.  Derived from
    * aspect_bp so squarer screens give the rows more horizontal room.
    * The pane spans [pane_left, vid_w - margin_x).  Used by the right-
    * pane preview drawer and by the list-row truncation gate so both
    * agree on where the image area starts. */
   int pane_left;

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

struct downplay_handle_s
{
   /* Navigation stack.  nav.nav[0] is seeded with DOWNPLAY_VIEW_TOP
    * in downplay_menu_init; nav.nav_depth >= 1 always.  Use the
    * dp_nav_* helpers (downplay_nav.h) to push / pop / change
    * selection.  Read nav.view / nav.selection directly — they are
    * cached mirrors of the top frame, kept in sync by the helpers. */
   dp_nav_state_t      nav;

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
   uintptr_t           pill_cap_tex;      /* RGBA circle, rounded pill ends */
   downplay_layout_t   layout;
   /* Number of entries in g_defaults.content_history at last rebuild.
    * 0 hides the "Recently Played" row entirely. */
   size_t              recent_count;
   /* Cached display rows for the RECENTS view.  Populated on drill-in
    * and freed on close so we don't re-format every frame.  Each entry
    * carries its source playlist index (see downplay_recent_t). */
   downplay_recent_t  *recents;
   size_t              recent_row_count;
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
   /* Tracks whether the user touched Screen Effect during this
    * session.  Save Changes only writes/removes the shader auto-
    * preset file when this is true — otherwise hitting Save just
    * to persist a core-option tweak would silently delete a
    * previously-saved per-core/per-game .slangp the user never
    * intended to touch. */
   bool                      frontend_effect_dirty;

   /* Save-state picker (DOWNPLAY_VIEW_SAVE_PICKER).  Cached on view
    * enter, freed (textures unloaded) on view exit.  Sorted desc by
    * mtime; entry 0 is always the most recent.  Includes .auto if it
    * exists alongside manual slots. */
   downplay_save_entry_t save_picker[DOWNPLAY_MAX_SAVE_ENTRIES];
   size_t              save_picker_count;

   /* Per-system thumbnail manager (pastime.gg-backed).  Owns the
    * canonical-key index, on-disk image cache, and the lookahead
    * fetch queue.  Open on system enter, close on system exit. */
   downplay_thumbs_t  *current_thumbs;

   /* Read-only recents resolver.  Lazy-loads per-system binary
    * indexes on demand.  Does NOT issue HTTP fetches; rows whose
    * system has no cached `.idx` simply render without art until
    * the user opens that system view.  Open on recents enter,
    * close on recents exit. */
   downplay_thumbs_recents_t *recents_thumbs;

   /* Index of the row whose thumbnail is currently loading or loaded.
    * On selection change we reset the previous row's thumbnail so
    * scrolling through a 200-ROM folder never accumulates more than
    * one resident texture.  SIZE_MAX is the explicit "no previous yet"
    * sentinel — 0 wouldn't work because `thumb_prev_selection != 0`
    * is a valid post-init state.  The `prev < rom_count` guard in the
    * drive function handles the sentinel naturally (SIZE_MAX exceeds
    * any rom_count).
    *
    * Per-view: SYSTEM and RECENTS index into different arrays
    * (`roms[]` vs `recents[]`), so a single shared field would alias if
    * a future code path ever switched views without firing the dispose
    * hook that resets it. */
   size_t              thumb_prev_selection_sys;
   size_t              thumb_prev_selection_rec;

   /* Set true after the SYSTEM-view dim warm-up has succeeded (at
    * least one ROM matched the thumbnail index, populating image_w/h
    * for every row in dp->roms).  Reset on system enter; checked each
    * frame in downplay_drive_system_thumbnails to know whether to
    * retry the warm-up (cold first-visit case where the index hadn't
    * landed yet at open_system time). */
   bool                roms_dims_warmed;

   /* Right-pane preview fade-in.  When the texture being drawn changes
    * (selection moved, async load completed, missing→ok retry hit), we
    * restart a 120 ms smoothstep fade so art doesn't pop in.  No cross-
    * fade — the previous texture vanishes the frame the new one starts
    * fading.  preview_last_tex is the comparison key; fade_started_us
    * is the wall-clock origin (cpu_features_get_time_usec). */
   uintptr_t           preview_last_tex;
   retro_time_t        preview_fade_started_us;

   /* Cached "Resume <Game>" availability for the launcher's TOP view.
    * Refreshed on rebuild_lists; if true, a Resume row is prepended
    * above Recents.  Stores the most-recent recents playlist index so
    * selection dispatches to downplay_launch_recent without re-
    * searching. */
   bool                resume_available;
   size_t              resume_pl_idx;
   /* Pre-formatted "Resume Foo" label, mutated only on rebuild. */
   char                resume_label[NAME_MAX_LENGTH];

   /* Top-right status pill text — rebuilt once per frame in
    * downplay_menu_frame from RA's powerstate + timedate helpers (both
    * internally throttled).  Cached because the value is consumed in
    * three places (the pill draw, the title-pill right limit, and the
    * top-row max-width budget) and we want them all in sync within a
    * single frame. */
   char                status_text[32];

   /* First-run setup progress animation.  displayed lerps toward a
    * per-segment target each frame; the asymptotic curve (target - cur)
    * gives the bar a "moving but never quite full" feel within each
    * segment, snapping when the underlying task completes.  last_us is
    * the timestamp of the last frame's update, used to compute dt.
    * was_running tracks the previous frame's setup phase so we can
    * reset displayed on a fresh pass — otherwise a second setup pass
    * (e.g. lazy install after boot setup completed) would start its
    * bar at 100% from the previous pass. */
   struct
   {
      float        displayed;
      retro_time_t last_us;
      bool         was_running;
   } setup_anim;
};

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
/* Chrome pill bg in launcher views (status pill, footer hints).  Pure
 * black would vanish against DP_COLOR_BG; a mid-gray reads as a chip.
 * INGAME keeps DP_COLOR_PILL_DARK because the dimmed-game bg already
 * provides contrast. */
static float DP_COLOR_PILL_CHROME_GRAY[16] = {
   0.125f, 0.125f, 0.125f, 1.0f,   0.125f, 0.125f, 0.125f, 1.0f,
   0.125f, 0.125f, 0.125f, 1.0f,   0.125f, 0.125f, 0.125f, 1.0f
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
/* Mid-gray (#979797) used for the key-cap glyph on footer hint badges
 * — softer than pure black against the white badge fill. */
#define DP_TEXT_BADGE   0x979797FF
#define DP_TEXT_MUTED   0x808080FF

/* ---------- list (systems + recents) ---------- */

/* TOP-view only: row 0 is "Recently Played" iff there are recent entries.
 * If a Resume row is present (M7), it sits *above* the Recents row, so
 * the Recents-row index in TOP becomes (resume_available ? 1 : 0). */
static bool downplay_has_recents_row(const downplay_handle_t *dp)
{
   return dp->nav.view == DOWNPLAY_VIEW_TOP && dp->recent_count > 0;
}

static bool downplay_has_resume_row(const downplay_handle_t *dp)
{
   return dp->nav.view == DOWNPLAY_VIEW_TOP && dp->resume_available;
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

   if (dp->nav.view == DOWNPLAY_VIEW_INGAME)
   {
      if (row < dp->ingame_action_count)
         return downplay_ingame_label(dp->ingame_actions[row]);
      return "";
   }

   if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER)
   {
      if (row < dp->save_picker_count)
         return dp->save_picker[row].label;
      return "";
   }

   if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM)
   {
      if (dp->roms && row < dp->rom_count)
         return dp->roms[row].display_name;
      return "";
   }

   if (dp->nav.view == DOWNPLAY_VIEW_RECENTS)
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
      free(systems[i].db_name);
   }
   free(systems);
}

/* Free a ROM array.  Cancels any in-flight thumbnail loads first, then
 * resets each per-ROM gfx_thumbnail_t — both are required by
 * gfx_thumbnail.h (cancel before any pending->reset sequence) to
 * avoid the upload callback writing into freed memory. */
static void downplay_roms_free(downplay_rom_t *roms, size_t count)
{
   size_t i;
   if (!roms)
      return;
   gfx_thumbnail_cancel_pending_requests();
   for (i = 0; i < count; i++)
   {
      gfx_thumbnail_reset(&roms[i].thumbnail);
      free(roms[i].display_name);
      free(roms[i].sort_key);
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
         char       *full = strdup(raw->elems[i].data);
         const char *db;
         if (!full)
         {
            free(display);
            free(ident);
            break;
         }
         systems[count].display_name = display;
         systems[count].core_ident   = ident;
         systems[count].full_path    = full;
         /* db_name resolution is best-effort: NULL means we don't have
          * a libretro-thumbnails-canonical name for this folder, so
          * matching + art look-up will be disabled for it.  The user
          * either typed an unknown display name or the core's .info
          * isn't loaded yet.  Survivable; recovers on next rebuild
          * once core_info catches up. */
         db = downplay_metadata_resolve_db_name(display, ident);
         systems[count].db_name = db ? strdup(db) : NULL;
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
   const char *ka = ra->sort_key ? ra->sort_key : ra->display_name;
   const char *kb = rb->sort_key ? rb->sort_key : rb->display_name;
   return strcmp(ka, kb);
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
         char       *path_dup;
         char       *sort_dup;
         char        clean[NAME_MAX_LENGTH];
         char        sort_buf[NAME_MAX_LENGTH];
         /* Strip No-Intro / Redump trailing tags here so the user sees
          * "Super Mario Bros. 3" not "Super Mario Bros. 3 (USA) (Rev A)".
          * sort_key folds case + drops a leading article so "The Legend
          * of Zelda" sorts under L.  See downplay_display_name.h. */
         downplay_display_name_clean(base, clean, sizeof(clean));
         if (!*clean)
            continue;
         downplay_display_name_sort_key(clean, sort_buf, sizeof(sort_buf));
         if (!(display = strdup(clean)))
            break;
         if (!(sort_dup = strdup(sort_buf)))
         {
            free(display);
            break;
         }
         if (!(path_dup = strdup(full)))
         {
            free(display);
            free(sort_dup);
            break;
         }
         memset(&roms[count], 0, sizeof(roms[count]));
         roms[count].display_name = display;
         roms[count].sort_key     = sort_dup;
         roms[count].full_path    = path_dup;
         /* mtime/size deliberately left zero — vestigial holdover from
          * when the metadata index pinned cache validity to (basename,
          * mtime, size).  That index has been deleted; the thumbnail
          * index is now the single source of layout truth.  The fields
          * are kept on the struct only to avoid touching every call
          * site that reads them in the same commit. */
         gfx_thumbnail_init_blank(&roms[count].thumbnail);
         /* image_w/image_h left zero by the memset above — populated
          * once by the dim warm-up in downplay_open_system after the
          * thumbnail manager is opened, and (in the cold-fetch case)
          * re-populated by the per-frame drive once the index lands. */
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
   if (dp->nav.view == DOWNPLAY_VIEW_INGAME)
      dp->total_rows = dp->ingame_action_count;
   else if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER)
      dp->total_rows = dp->save_picker_count;
   else if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM)
      dp->total_rows = dp->rom_count;
   else if (dp->nav.view == DOWNPLAY_VIEW_RECENTS)
      dp->total_rows = dp->recent_row_count;
   else if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
   {
      /* Settings view manages its own selection/scroll on the active
       * stack frame; the main `selection` cursor isn't meaningful
       * here.  total_rows kept at 0 keeps any stray default-list
       * input handler a no-op. */
      dp->total_rows = 0;
      dp_nav_set_selection(&dp->nav, 0);
      return;
   }
   else
      dp->total_rows = (downplay_has_resume_row(dp)  ? 1 : 0)
                     + (downplay_has_recents_row(dp) ? 1 : 0)
                     + dp->system_count;

   /* Selection clamp: keep dp->nav.selection in [0, total_rows).
    * dp_nav_set_selection writes both the cache and the top frame
    * so a future push/pop preserves the clamped value.  Safe to
    * call from inside after_change — set_selection doesn't itself
    * fire after_change. */
   if (dp->total_rows == 0)
      dp_nav_set_selection(&dp->nav, 0);
   else if (dp->nav.selection >= dp->total_rows)
      dp_nav_set_selection(&dp->nav, dp->total_rows - 1);
}

/* nav stack helpers (push/pop/pop_to/top/root_view/set_selection)
 * live in downplay/downplay_nav.c — see the header included at the
 * top of this file.  Wrapper that lets the after_change callback
 * (dp_nav_state_t-typed) reach back into the host's recompute. */
static void downplay_after_nav_change(void *user)
{
   downplay_recompute_total_rows((downplay_handle_t*)user);
}

static void downplay_recents_free(downplay_recent_t *rows, size_t count)
{
   size_t i;
   if (!rows)
      return;
   /* Cancel any in-flight gfx_thumbnail_request_file before resetting,
    * mirroring downplay_roms_free; otherwise a callback can race onto
    * freed memory.  Safe even if no requests were ever issued. */
   gfx_thumbnail_cancel_pending_requests();
   for (i = 0; i < count; i++)
   {
      free(rows[i].display_name);
      free(rows[i].db_name);
      free(rows[i].rom_basename);
      gfx_thumbnail_reset(&rows[i].thumbnail);
   }
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

      {
         char clean[NAME_MAX_LENGTH];
         downplay_display_name_clean(src, clean, sizeof(clean));
         if (!*clean)
            continue;
         if (!(dup = strdup(clean)))
            continue;
      }
      out[count].display_name = dup;
      out[count].pl_idx       = i;
      gfx_thumbnail_init_blank(&out[count].thumbnail);

      /* Best-effort thumbnail-resolution metadata.  Either piece may
       * be NULL; the recents-view drive function treats that as "no
       * art for this row" and moves on without complaining.  Recents
       * never kicks downloads, so an unresolvable system is silently
       * artless until the user opens that system view (which
       * populates the index cache via the manager). */
      if (entry->path && *entry->path)
      {
         char         parent_dir[PATH_MAX_LENGTH];
         const char  *folder;
         char        *display = NULL;
         char        *ident   = NULL;
         const char  *basename;

         basename = path_basename(entry->path);
         if (basename && *basename)
            out[count].rom_basename = strdup(basename);

         strlcpy(parent_dir, entry->path, sizeof(parent_dir));
         path_basedir_wrapper(parent_dir);
         /* Trim trailing slash so path_basename gives the folder name
          * itself ("Foo (core)"), not an empty tail. */
         {
            size_t plen = strlen(parent_dir);
            while (plen > 0 && (parent_dir[plen - 1] == '/'
                  || parent_dir[plen - 1] == '\\'))
               parent_dir[--plen] = '\0';
         }
         folder = path_basename(parent_dir);
         if (folder && *folder
               && downplay_parse_system_folder(folder, &display, &ident))
         {
            const char *db = downplay_metadata_resolve_db_name(display, ident);
            if (db && *db)
               out[count].db_name = strdup(db);
            free(display);
            free(ident);
         }
      }
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

   if (dp->nav.view == DOWNPLAY_VIEW_INGAME && dp->nav.selection >= n)
      dp_nav_set_selection(&dp->nav, n - 1);
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

/* Dispose hook for the RECENTS frame: free the cached display rows
 * and the read-only thumbnail resolver.  Selection is preserved by
 * the parent TOP frame underneath us, so we don't need to do
 * anything to restore it. */
static void downplay_recents_dispose(void *user, void *side)
{
   downplay_handle_t *dp = (downplay_handle_t*)user;
   (void)side;
   downplay_recents_free(dp->recents, dp->recent_row_count);
   dp->recents             = NULL;
   dp->recent_row_count    = 0;
   downplay_thumbs_recents_close(dp->recents_thumbs);
   dp->recents_thumbs      = NULL;
   dp->thumb_prev_selection_rec = SIZE_MAX;
   /* Reset the right-pane fade key so re-entering a two-pane view
    * starts fresh from alpha 0 even if the GPU recycled a handle. */
   dp->preview_last_tex    = 0;
}

/* Dispose hook for the INGAME frame.  The action array stays on dp
 * (next push will rebuild it via refresh_ingame_actions); we just
 * need to refresh the launcher's Resume row availability so the new
 * autosave RA wrote on unload shows up. */
static void downplay_ingame_dispose(void *user, void *side)
{
   downplay_handle_t *dp = (downplay_handle_t*)user;
   (void)side;
   downplay_refresh_resume(dp);
}

/* Dispose hook for the SAVE_PICKER frame: free thumbnail textures
 * and refresh the underlying INGAME view's action composition (the
 * Load row's visibility is gated on manual_count, which the user
 * may have just changed by loading a slot — and a freshly-loaded
 * state generates an autosave that bumps the count next time). */
static void downplay_save_picker_dispose(void *user, void *side)
{
   downplay_handle_t *dp = (downplay_handle_t*)user;
   (void)side;
   downplay_save_picker_free(dp);
   downplay_refresh_ingame_actions(dp);
   /* Drop the right-pane fade key: the next two-pane view should fade
    * its first texture in from zero, not skip the fade because the
    * GPU happened to recycle a texture handle we still had cached. */
   dp->preview_last_tex = 0;
}

static void downplay_open_recents(downplay_handle_t *dp)
{
   size_t i;
   downplay_recents_free(dp->recents, dp->recent_row_count);
   dp->recents          = NULL;
   dp->recent_row_count = 0;
   dp->recents          = downplay_build_recents(&dp->recent_row_count);
   /* Cheap allocation; no I/O until the per-frame pump fires. */
   downplay_thumbs_recents_close(dp->recents_thumbs);
   dp->recents_thumbs            = downplay_thumbs_recents_open();
   dp->thumb_prev_selection_rec  = SIZE_MAX;
   /* Seed every distinct db_name across the row set so the per-frame
    * pump can pre-warm them in seed order, one .idx read per frame.
    * Without this, the first hover on each system pays a synchronous
    * 5–30 ms read; with it, the loads happen during the natural
    * frames it takes the user to settle on a row. */
   if (dp->recents && dp->recents_thumbs)
   {
      for (i = 0; i < dp->recent_row_count; i++)
      {
         const char *db = dp->recents[i].db_name;
         if (db && *db)
            downplay_thumbs_recents_seed(dp->recents_thumbs, db);
      }
   }
   dp_nav_push(&dp->nav, DOWNPLAY_VIEW_RECENTS, NULL, downplay_recents_dispose);
}

static void downplay_close_recents(downplay_handle_t *dp)
{
   dp_nav_pop(&dp->nav);
}

/* Dispose hook for the SYSTEM frame: free the loaded ROM list and
 * close the per-system metadata index (which flushes its JSON). */
static void downplay_system_dispose(void *user, void *side)
{
   downplay_handle_t *dp = (downplay_handle_t*)user;
   (void)side;
   /* Order matters: roms_free already cancels pending thumbnail
    * requests and resets each thumbnail's texture (gfx_thumbnail.h
    * requires cancel before reset for any in-flight load).  We then
    * blank the path_data so set_system / set_content from a
    * subsequent system enter start clean. */
   downplay_roms_free(dp->roms, dp->rom_count);
   dp->roms                     = NULL;
   dp->rom_count                = 0;
   dp->thumb_prev_selection_sys = SIZE_MAX;
   dp->roms_dims_warmed         = false;
   if (dp->current_thumbs)
   {
      downplay_thumbs_close(dp->current_thumbs);
      dp->current_thumbs = NULL;
   }
   /* See save_picker_dispose: clear the right-pane fade key so the
    * next view's first texture restarts the smoothstep from alpha 0
    * even if the GPU recycles a previously-seen handle. */
   dp->preview_last_tex = 0;
}

/* Drill into a system's ROM list.  No-op when sys_idx is out of
 * range.  Cursor restoration on close happens automatically — the
 * parent TOP frame's selection is preserved underneath us.
 *
 * Lifecycle:
 *   1. Scan ROM filesystem (existing behaviour).
 *   2. Open the metadata index for this system, reconcile against the
 *      filesystem set (note_present each ROM, finish_scan to prune).
 *   3. Initialize the thumbnail path resolver lazily (first system
 *      enter only) and point it at the system's libretro-thumbnails
 *      db_name.
 *   4. Push the nav frame.
 *
 * Failures along the way are non-fatal — a NULL index just means no
 * canonical labels and no art for this session, but the basic ROM list
 * still works. */
/* Populate every row's image_w/image_h from the thumbnail index via
 * side-effect-free peek lookups.  Idempotent + cheap (~50 ns per
 * call); each peek either fills dims or leaves them at the previous
 * value (typically 0, until the index lands).
 *
 * Sets dp->roms_dims_warmed = true on the first run that produces at
 * least one hit — once an index is loaded, it stays loaded for the
 * lifetime of the manager, so subsequent hits add nothing.  Caller
 * checks the flag to decide whether to retry on later frames.
 *
 * Cold-fetch case (no on-disk index yet at open_system time): every
 * peek misses and the flag stays false; downplay_drive_system_thumbnails
 * re-runs this each frame until the pump lands the index, at which
 * point all rows reflow in one frame.  That single reflow is the
 * unavoidable consequence of not having the data — far better than
 * the previous behavior where each row's text width depended on
 * whether the user had hovered onto it. */
static void dp_warm_roms_dims(downplay_handle_t *dp)
{
   size_t      i;
   const char *base;
   uint16_t    w;
   uint16_t    h;
   bool        any_hit = false;
   if (!dp || !dp->current_thumbs || !dp->roms || dp->rom_count == 0)
      return;
   for (i = 0; i < dp->rom_count; i++)
   {
      base = path_basename(dp->roms[i].full_path);
      w    = 0;
      h    = 0;
      if (!base || !*base)
         continue;
      if (downplay_thumbs_peek(dp->current_thumbs, base,
               &w, &h, NULL, NULL))
      {
         dp->roms[i].image_w = w;
         dp->roms[i].image_h = h;
         any_hit = true;
      }
   }
   if (any_hit)
      dp->roms_dims_warmed = true;
}

static void downplay_open_system(downplay_handle_t *dp, size_t sys_idx)
{
   const downplay_system_t *sys;
   size_t                   i;

   if (!dp->systems || sys_idx >= dp->system_count)
      return;

   downplay_roms_free(dp->roms, dp->rom_count);
   dp->roms                     = NULL;
   dp->rom_count                = 0;
   dp->thumb_prev_selection_sys = SIZE_MAX;

   sys           = &dp->systems[sys_idx];
   dp->roms      = downplay_scan_roms(sys->full_path, &dp->rom_count);
   dp->active_system = sys_idx;

   /* Open the pastime.gg-backed thumbnail manager.  Closes the prior
    * one (no-op normally, since system_dispose already did).  NULL
    * db_name → manager is skipped; rows still render labels. */
   if (dp->current_thumbs)
   {
      downplay_thumbs_close(dp->current_thumbs);
      dp->current_thumbs = NULL;
   }
   if (sys->db_name && *sys->db_name)
      dp->current_thumbs = downplay_thumbs_open(sys->db_name);

   /* Reset and try the per-row dim warm-up.  See dp_warm_roms_dims
    * for the cold-vs-warm cache rationale. */
   dp->roms_dims_warmed = false;
   dp_warm_roms_dims(dp);

   /* Speculative prefetch: kick off image fetches for the first ~10
    * rows immediately on system enter, before the user hovers.  By
    * the time their selection lands on a row, its art is on disk
    * (or in flight) — eliminates the wait-for-hover latency gate.
    * Existing _prefetch dedups against in-flight; harmless if the
    * index isn't ready yet (it will skip with no harm done). */
   if (dp->current_thumbs && dp->rom_count > 0)
   {
      /* names[] borrows pointers into dp->roms[].full_path; valid
       * because _prefetch consumes them synchronously and does not
       * retain references after returning. */
      #define DP_SPECULATIVE_PREFETCH_ROWS 10
      const char *names[DP_SPECULATIVE_PREFETCH_ROWS];
      size_t      n   = 0;
      size_t      cap = dp->rom_count < DP_SPECULATIVE_PREFETCH_ROWS
            ? dp->rom_count : DP_SPECULATIVE_PREFETCH_ROWS;
      for (i = 0; i < cap; i++)
      {
         const char *b = path_basename(dp->roms[i].full_path);
         if (b && *b)
            names[n++] = b;
      }
      if (n > 0)
         downplay_thumbs_prefetch(dp->current_thumbs, names, n);
      #undef DP_SPECULATIVE_PREFETCH_ROWS
   }

   dp_nav_push(&dp->nav, DOWNPLAY_VIEW_SYSTEM, NULL, downplay_system_dispose);
}

static void downplay_close_system(downplay_handle_t *dp)
{
   dp_nav_pop(&dp->nav);
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
   downplay_setup_begin_boot(idents, 1);
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

   /* If a future caller triggers rebuild while drilled in, snap back
    * to TOP — active_system / cached recents would otherwise index
    * into stale arrays.  Each frame's dispose hook handles its own
    * cleanup (roms/recents free). */
   if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM
         || dp->nav.view == DOWNPLAY_VIEW_RECENTS)
      dp_nav_pop_to(&dp->nav, DOWNPLAY_VIEW_TOP);

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
   L->scale_factor     = user_scale_factor;
   L->vid_w            = video_width;
   L->vid_h            = video_height;

   {
      /* Bucket the screen by aspect ratio so layout knobs can target
       * "square / classic / wide / HD" instead of inventing thresholds
       * inline.  Thresholds: 4:3 = 1.333, 3:2 = 1.5, 16:9 = 1.778. */
      float ar = video_height > 0
            ? (float)video_width / (float)video_height : 1.0f;
      L->aspect_bp = ar >= 1.70f ? DP_AR_HD
                   : ar >= 1.45f ? DP_AR_WIDE
                   : ar >= 1.15f ? DP_AR_CLASSIC
                   :               DP_AR_SQUARE;
   }

   {
      /* Right-pane width as a fraction of vid_w, narrower on squarer
       * screens where the rows need the room.  One arm per breakpoint
       * even where the values currently match — the slots are here so
       * 3:2 / 16:10 can be tuned independently of 4:3 later without
       * touching the structure. */
      int pct = L->aspect_bp >= DP_AR_HD      ? 42
              : L->aspect_bp >= DP_AR_WIDE    ? 40
              : L->aspect_bp >= DP_AR_CLASSIC ? 40
              :                                 38;
      int pane_w = (int)video_width * pct / 100;
      L->pane_left = (int)video_width - pane_w;
   }

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
   /* Compare the source inputs that drive the layout, not the derived
    * `scale` float — float == on a re-derived value is bit-fragile if
    * intermediate FPU precision differs across paths.  user_scale_factor
    * is the raw user setting and is bit-stable across reads. */
   return     L->vid_w        != video_width
           || L->vid_h        != video_height
           || L->scale_factor != user_scale_factor;
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

/* One badge + label pair inside a footer hint pill. */
typedef struct downplay_hint
{
   const char *glyph;
   const char *label;
} downplay_hint_t;

/* Footer hint = outer dark pill (row_h tall, auto-sized to content)
 * containing one or more (badge, label) pairs.  The inner badges are
 * inset vertically so the outer pill reads as a footer surface and the
 * badges as key caps sitting on it.  Multiple pairs share one pill
 * with a `chrome_pad_x` separator between them.
 *
 * `anchor_x` is the left edge for ANCHOR_LEFT, or the right edge for
 * ANCHOR_RIGHT — the function measures the content and positions the
 * outer pill accordingly. */
static int downplay_draw_footer_hints(gfx_display_t *p_disp, void *userdata,
      font_data_t *font, int centre_offset,
      const downplay_layout_t *L, uintptr_t cap_tex,
      int anchor_x, int y, enum downplay_anchor anchor,
      const downplay_hint_t *hints, size_t hint_count, float *bg_color)
{
   int    badge_inset_y = (int)(6.0f * L->scale);
   /* Asymmetric inner padding so the *visible text* is equally inset
    * from each outer cap: the left side already has the badge's own
    * chrome_pad_x between cap and glyph, so the right side adds
    * chrome_pad_x on top of inner_pad_l to match. */
   int    inner_pad_l   = (int)(9.0f * L->scale);
   int    inner_pad_r   = inner_pad_l + L->chrome_pad_x;
   int    sep_gap       = 2 * L->chrome_pad_x;
   int    badge_h       = L->row_h - 2 * badge_inset_y;
   int    pill_w        = 0;
   int    pill_x;
   int    cursor_x;
   int    label_y;
   size_t i;
   int    badge_w[8];
   int    label_w[8];
   int    glyph_w;
   int    min_badge_w;

   if (hint_count == 0 || hint_count > 8)
      return 0;

   if (badge_h < 1)
   {
      badge_h       = L->row_h;
      badge_inset_y = 0;
   }

   /* Pass 1 — measure each badge and label, accumulate pill_w. */
   min_badge_w = badge_h;
   pill_w      = inner_pad_l + inner_pad_r;
   for (i = 0; i < hint_count; i++)
   {
      glyph_w    = font_driver_get_message_width(font, hints[i].glyph,
            (unsigned)strlen(hints[i].glyph), 1.0f);
      badge_w[i] = (glyph_w > 0) ? glyph_w + 2 * L->chrome_pad_x : min_badge_w;
      if (badge_w[i] < min_badge_w)
         badge_w[i] = min_badge_w;

      label_w[i] = font_driver_get_message_width(font, hints[i].label,
            (unsigned)strlen(hints[i].label), 1.0f);
      if (label_w[i] < 0)
         label_w[i] = 0;

      pill_w += badge_w[i] + L->chrome_gap + label_w[i];
      if (i + 1 < hint_count)
         pill_w += sep_gap;
   }
   if (pill_w < L->row_h)
      pill_w = L->row_h;

   pill_x = (anchor == DOWNPLAY_ANCHOR_RIGHT)
          ? anchor_x - pill_w
          : anchor_x;

   /* Pass 2 — draw outer pill, then each badge + label in order. */
   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         pill_x, y, pill_w, L->row_h, bg_color);

   label_y  = downplay_baseline_y(y, L->row_h, centre_offset);
   cursor_x = pill_x + inner_pad_l;
   for (i = 0; i < hint_count; i++)
   {
      int badge_x      = cursor_x;
      int badge_text_y = downplay_baseline_y(y + badge_inset_y, badge_h,
            centre_offset);
      int label_x      = badge_x + badge_w[i] + L->chrome_gap;

      downplay_draw_pill(p_disp, userdata, L, cap_tex,
            badge_x, y + badge_inset_y, badge_w[i], badge_h,
            DP_COLOR_PILL_LIGHT);
      downplay_draw_text(font, hints[i].glyph,
            (float)(badge_x + badge_w[i] / 2), (float)badge_text_y,
            L, DP_TEXT_BADGE, TEXT_ALIGN_CENTER);
      downplay_draw_text(font, hints[i].label,
            (float)label_x, (float)label_y,
            L, DP_TEXT_LIGHT, TEXT_ALIGN_LEFT);

      cursor_x = label_x + label_w[i] + sep_gap;
   }
   return pill_w;
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
   char        clean[NAME_MAX_LENGTH];
   int         max_w;

   if (!content || !*content)
      return;
   fill_pathname_base(title, content, sizeof(title));
   path_remove_extension(title);
   if (!*title)
      return;
   /* Same cleanup as the system list rows: strip "(USA) (Rev 1) [!]"
    * tails and rotate ", The" articles to the front. */
   downplay_display_name_clean(title, clean, sizeof(clean));
   if (!*clean)
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
         clean, sizeof(clean));
}

/* Build the status-pill text from RA's built-in helpers: "HH:MM"
 * (or "HH:MM AM/PM") + battery, where battery is "NN%" / "NN% AC"
 * (charging) / "AC" (no battery, e.g. desktop builds).  Two-space
 * gap so the two halves read as separate elements.  Time format
 * tracks settings->uints.menu_timedate_style — set by the global
 * Settings menu's Clock Format row.  Date+time styles fall back to
 * 24-hour time-only since the pill has no room for a date. */
static void downplay_build_status_text(char *out, size_t len)
{
   settings_t *settings = config_get_ptr();
   gfx_display_ctx_powerstate_t ps;
   gfx_display_ctx_datetime_t dt;
   char time_buf[16];
   /* menu_display_powerstate writes a "NN%" string we don't use (we
    * read the struct fields instead) but it requires a non-NULL
    * buffer.  One byte is enough — snprintf truncates to fit. */
   char discard[1];

   time_buf[0] = '\0';

   dt.time_mode      = (settings && settings->uints.menu_timedate_style
                          == MENU_TIMEDATE_STYLE_HM_AMPM)
                       ? MENU_TIMEDATE_STYLE_HM_AMPM
                       : MENU_TIMEDATE_STYLE_HM;
   dt.date_separator = MENU_TIMEDATE_DATE_SEPARATOR_HYPHEN;
   menu_display_timedate(&dt, time_buf, sizeof(time_buf));

   ps.battery_enabled = false;
   ps.percent         = 0;
   ps.charging        = false;
   menu_display_powerstate(&ps, discard, sizeof(discard));

   if (ps.battery_enabled)
   {
      if (ps.charging)
         snprintf(out, len, "%s  %u%% AC", time_buf, ps.percent);
      else
         snprintf(out, len, "%s  %u%%", time_buf, ps.percent);
   }
   else
      snprintf(out, len, "%s  AC", time_buf);
}

/* Width of the rendered status pill — useful to callers (e.g. the
 * title pill) that need to reserve space for it without coupling to
 * its draw site.  Text is dp->status_text, recomputed once per
 * frame in downplay_menu_frame. */
static int downplay_status_pill_width(font_data_t *font,
      const downplay_layout_t *L, const char *text)
{
   int text_w = font_driver_get_message_width(font, text,
         (unsigned)strlen(text), 1.0f);
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
      const downplay_layout_t *L, uintptr_t cap_tex, float *bg_color,
      const char *text)
{
   /* Right-anchored: pill sized to text + padding (with a row-h floor
    * so single-glyph values still look pill-shaped), positioned so its
    * right edge sits on the right margin. */
   int pill_w = downplay_status_pill_width(font, L, text);
   int x      = (int)L->vid_w - L->margin_x - pill_w;
   int text_y = downplay_baseline_y(L->margin_y, L->row_h, centre_offset);

   downplay_draw_pill(p_disp, userdata, L, cap_tex,
         x, L->margin_y, pill_w, L->row_h, bg_color);
   downplay_draw_text(font, text,
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

/* Dispose hook for a SETTINGS frame.  Fires the list's optional
 * on_close (Frontend submenu uses this to commit deferred shader
 * changes) before freeing.  on_close may still inspect L->rows[]
 * since the list isn't freed until after it returns. */
static void downplay_settings_dispose(void *user, void *side)
{
   downplay_handle_t        *dp = (downplay_handle_t*)user;
   downplay_settings_list_t *L  = (downplay_settings_list_t*)side;
   if (L)
   {
      if (L->on_close)
         L->on_close(L, L->on_close_userdata);
      downplay_settings_list_free(L);
   }
   /* When the last SETTINGS frame pops (i.e. we're returning to a
    * non-SETTINGS view such as INGAME or TOP), refresh the ingame
    * action composition.  Mirrors the pre-extraction settings_pop
    * behaviour: Save/Load row visibility tracks manual_count, which
    * could have shifted while the user was inside Options.  pop's
    * sync runs before dispose, so dp->nav.view here already
    * reflects the parent — no SETTINGS frame remains under us. */
   if (dp && dp->nav.view != DOWNPLAY_VIEW_SETTINGS)
      downplay_refresh_ingame_actions(dp);
}

/* Push a fully-built list onto the nav stack as a SETTINGS frame.
 * Takes ownership: pop will fire on_close + free.  Drops the push
 * silently when L is NULL (the build_*_list helpers can return NULL
 * on calloc failure or when there's nothing to show). */
static void downplay_settings_push(downplay_handle_t *dp,
      downplay_settings_list_t *L)
{
   if (!L)
      return;
   dp_nav_push(&dp->nav, DOWNPLAY_VIEW_SETTINGS, L, downplay_settings_dispose);
}

static void downplay_settings_pop(downplay_handle_t *dp)
{
   if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
      dp_nav_pop(&dp->nav);
}

/* Pop every SETTINGS frame off the top.  Used when Start toggles
 * the in-game Options menu closed (collapse the entire stack
 * instead of one Cancel-press per level). */
static void downplay_settings_pop_all(downplay_handle_t *dp)
{
   while (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
      dp_nav_pop(&dp->nav);
}

/* Top settings list; NULL if the top frame isn't a SETTINGS frame. */
static downplay_settings_list_t *downplay_settings_top(
      const downplay_handle_t *dp)
{
   const dp_nav_frame_t *top = dp->nav.nav_depth > 0
         ? &dp->nav.nav[dp->nav.nav_depth - 1] : NULL;
   if (!top || top->view != DOWNPLAY_VIEW_SETTINGS)
      return NULL;
   return (downplay_settings_list_t*)top->side;
}

/* CONFIRM frame side data.  Each open_confirm allocates one of
 * these, attached to the pushed frame; close_confirm pops and the
 * dispose hook frees it.  Chained confirms (callback opens another
 * confirm) genuinely stack, with each frame carrying its own payload
 * — no in-place replacement, no shared single-slot to clobber. */
typedef struct
{
   char  message[256];
   char  a_label[16];
   char  b_label[16];      /* empty = no B button */
   void (*on_confirm)(void *dp);
} dp_confirm_side_t;

static void downplay_confirm_dispose(void *user, void *side)
{
   (void)user;
   free(side);
}

/* Open the confirm modal.  `b_label` may be NULL/empty to suppress
 * the cancel button (acknowledgement-only screen).  `on_confirm` is
 * invoked when the user presses A, *before* the frame is popped —
 * callbacks may call downplay_open_confirm again to chain screens.
 * Each call pushes a new CONFIRM frame; the underlying view (TOP /
 * INGAME / SETTINGS) and any deeper SETTINGS stack are preserved. */
static void downplay_open_confirm(downplay_handle_t *dp,
      const char *message, const char *a_label, const char *b_label,
      void (*on_confirm)(void *dp))
{
   dp_confirm_side_t *side;
   if (!dp || !(side = (dp_confirm_side_t*)calloc(1, sizeof(*side))))
      return;
   strlcpy(side->message, message ? message : "", sizeof(side->message));
   strlcpy(side->a_label, (a_label && *a_label) ? a_label : "OKAY",
         sizeof(side->a_label));
   strlcpy(side->b_label, b_label ? b_label : "", sizeof(side->b_label));
   side->on_confirm = on_confirm;
   dp_nav_push(&dp->nav, DOWNPLAY_VIEW_CONFIRM, side, downplay_confirm_dispose);
}

/* Dismiss the confirm modal.  Capture the callback locally before
 * the pop (the pop frees `side`), then fire it after — at which
 * point a chained open_confirm pushes a new frame onto the now-
 * exposed parent, not on top of the dying frame. */
static void downplay_close_confirm(downplay_handle_t *dp, bool fire)
{
   dp_nav_frame_t    *top  = dp_nav_top(&dp->nav);
   dp_confirm_side_t *side = (top && top->view == DOWNPLAY_VIEW_CONFIRM)
                           ? (dp_confirm_side_t*)top->side : NULL;
   void             (*cb)(void *) = side ? side->on_confirm : NULL;
   dp_nav_pop(&dp->nav);
   if (fire && cb)
      cb(dp);
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
    * values let narrower lists like Options stay visually centred.
    *
    * Nav-only lists (no row carries a right-column value) ignore
    * width_pct: with text left-aligned and nothing on the right,
    * a fixed-width block leaves all visible text hugging its left
    * edge.  Auto-fit the block to the longest title's natural
    * width + indent padding, then centre — so the visible rows
    * read as a centred block of text on the screen. */
   {
      int    avail_w;
      bool   any_value = false;
      size_t k;
      for (k = 0; k < S->row_count; k++)
      {
         const downplay_settings_row_t *rr = &S->rows[k];
         if (rr->values && rr->values_count > 0)
         {
            any_value = true;
            break;
         }
      }
      width_pct  = S->width_pct ? S->width_pct : 60;
      if (width_pct > 100)
         width_pct = 100;
      avail_w   = (int)L->vid_w - 2 * L->margin_x;
      if (avail_w < 0)
         avail_w = 0;
      if (any_value)
         block_w = avail_w * (int)width_pct / 100;
      else
      {
         int max_title_w = 0;
         for (k = 0; k < S->row_count; k++)
         {
            const char *t = S->rows[k].title;
            int         w;
            if (!t || !*t)
               continue;
            w = font_driver_get_message_width(row_font, t,
                  (unsigned)strlen(t), 1.0f);
            if (w > max_title_w)
               max_title_w = w;
         }
         block_w = max_title_w + 2 * L->row_text_indent;
         if (block_w < L->settings_row_h)
            block_w = L->settings_row_h;
         if (block_w > avail_w)
            block_w = avail_w;
      }
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

/* Render the confirm modal: a single line of centered text mid-screen.
 * Background and chrome (status pill, footer hints) are drawn by the
 * outer frame — we only paint the message.  No word wrap yet; messages
 * are short ("Saved for console.", "Restore all options to defaults?")
 * and would otherwise need a measure-and-truncate pass like the
 * settings rows. */
static void downplay_draw_confirm_view(void *userdata,
      const downplay_handle_t *dp)
{
   const downplay_layout_t *L  = &dp->layout;
   font_data_t             *f  = dp->font ? dp->font : dp->chrome_font;
   const dp_nav_frame_t    *top = (dp->nav.nav_depth > 0)
                                ? &dp->nav.nav[dp->nav.nav_depth - 1] : NULL;
   const dp_confirm_side_t *side = (top
            && top->view == DOWNPLAY_VIEW_CONFIRM)
         ? (const dp_confirm_side_t*)top->side : NULL;
   int                      cx;
   int                      cy;
   int                      ascent;
   (void)userdata;
   if (!f || !side || !*side->message)
      return;
   cx     = (int)L->vid_w / 2;
   cy     = (int)L->vid_h / 2;
   ascent = dp->font ? dp->font_centre_offset : dp->chrome_font_centre_offset;
   downplay_draw_text(f, side->message,
         (float)cx, (float)(cy + ascent),
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
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

/* Stub list — placeholder used by Controls / Shortcuts until they
 * have real implementations.  One nav row that just cancels back. */
static downplay_settings_list_t *downplay_build_stub_list(const char *title)
{
   downplay_settings_list_t *L = downplay_settings_list_new(title, 1, 0, 50);
   if (!L)
      return NULL;
   L->rows[0].title      = "Coming soon";
   L->rows[0].desc       = "Not yet implemented in this build.";
   return L;
}

/* ---- Frontend submenu (M8): Screen Scaling / Effect ----
 *
 * Each row is a value cycler whose on_change writes into settings_t
 * (or the menu shader manager) and dispatches the appropriate
 * command_event so the change takes effect on the next frame without
 * leaving the menu.
 *
 * Scaling combines a few RA knobs — see dp_frontend_scaling_apply
 * for the mapping.  Effect drives the slang shader pipeline via
 * menu_shader_manager_set_preset; rows are filtered to presets
 * present on disk so cores without the slang shaders content bucket
 * installed get a usable (smaller) list rather than a wall of
 * "shader not found" errors. */

/* The Screen Scaling row is a 2-axis cycler over (PAR × scaling
 * discipline), plus two outliers.  Six logical modes:
 *
 *   DP_SCALING_ASPECT          CORE   PAR + fractional
 *   DP_SCALING_NATIVE          CORE   PAR + integer underscale
 *   DP_SCALING_FULLSCREEN      FULL stretch (no PAR)
 *   DP_SCALING_ASPECT_SQUARE   SQUARE PAR + fractional
 *   DP_SCALING_NATIVE_SQUARE   SQUARE PAR + integer underscale
 *   DP_SCALING_CROPPED_SQUARE  SQUARE PAR + integer overscale (clip)
 *
 * The SQUARE-PAR three only appear when the loaded core's intended
 * DAR differs from the source's natural DAR (i.e. core PAR ≠ 1:1).
 * On square-PAR cores (GB, GBA, NDS) those three rows would all
 * produce identical pictures to their CORE-PAR siblings, so we
 * suppress them for a 3-row list.  On non-square cores the SQUARE
 * variants are labeled with the source DAR (e.g. "Aspect (8:7)") so
 * the user can see exactly which ratio they're picking. */
enum dp_scaling_mode
{
   DP_SCALING_ASPECT          = 0,
   DP_SCALING_NATIVE          = 1,
   DP_SCALING_FULLSCREEN      = 2,
   DP_SCALING_ASPECT_SQUARE   = 3,
   DP_SCALING_NATIVE_SQUARE   = 4,
   DP_SCALING_CROPPED_SQUARE  = 5
};

#define DP_SCALING_MODE_MAX 6
/* "Aspect (NN:NN)" — 8 base + up to 11 ratio = ~20 chars; +slack. */
#define DP_SCALING_LABEL_MAX 28

/* Per-row userdata for the Screen Scaling cycler.  storage[] backs
 * the dynamic "(W:H)" labels; labels[] points into storage[] for the
 * SQUARE rows and at static literals for the unqualified rows.
 * mode[] maps row idx → enum dp_scaling_mode so the visible rows can
 * be dense (3 on square-PAR cores, 6 otherwise) while mode IDs stay
 * stable. */
typedef struct
{
   downplay_handle_t   *dp;
   char                 storage[DP_SCALING_MODE_MAX][DP_SCALING_LABEL_MAX];
   const char          *labels[DP_SCALING_MODE_MAX];
   uint8_t              mode[DP_SCALING_MODE_MAX];
   size_t               count;
} dp_scaling_row_ud_t;

static unsigned dp_gcd(unsigned a, unsigned b)
{
   unsigned t;
   while (b)
   {
      t = b;
      b = a % b;
      a = t;
   }
   return a;
}

/* Reduce the source's natural DAR (base_w / base_h) to lowest terms
 * and write into *out_w / *out_h.  Returns true iff the loaded core's
 * intended DAR (geometry.aspect_ratio) differs from the source's
 * natural DAR — i.e., the core's pixels are non-square.  When this
 * returns false, square pixels and core PAR produce the same picture
 * and the SQUARE-variant rows would be redundant. */
static bool dp_scaling_compute_square_dar(unsigned *out_w, unsigned *out_h)
{
   const struct retro_game_geometry *g
      = &video_state_get_ptr()->av_info.geometry;
   unsigned bw, bh, gd;
   float    core_dar, square_dar, diff;
   if (!g->base_width || !g->base_height)
      return false;
   bw = g->base_width;
   bh = g->base_height;
   gd = dp_gcd(bw, bh);
   if (out_w)
      *out_w = bw / gd;
   if (out_h)
      *out_h = bh / gd;
   square_dar = (float)bw / (float)bh;
   core_dar   = (g->aspect_ratio > 0.0f) ? g->aspect_ratio : square_dar;
   diff       = core_dar - square_dar;
   if (diff < 0.0f)
      diff = -diff;
   return diff > 0.005f;
}

static enum dp_scaling_mode dp_frontend_scaling_current_mode(void)
{
   settings_t *s = config_get_ptr();
   if (!s)
      return DP_SCALING_ASPECT;
   if (s->uints.video_aspect_ratio_idx == ASPECT_RATIO_FULL)
      return DP_SCALING_FULLSCREEN;
   if (s->bools.video_scale_integer)
   {
      bool over = (s->uints.video_scale_integer_scaling
            == VIDEO_SCALE_INTEGER_SCALING_OVERSCALE);
      if (s->uints.video_aspect_ratio_idx == ASPECT_RATIO_CORE && !over)
         return DP_SCALING_NATIVE;
      if (s->uints.video_aspect_ratio_idx == ASPECT_RATIO_SQUARE && !over)
         return DP_SCALING_NATIVE_SQUARE;
      if (s->uints.video_aspect_ratio_idx == ASPECT_RATIO_SQUARE && over)
         return DP_SCALING_CROPPED_SQUARE;
      /* Integer + something exotic (4:3, 16:9, etc. in the user's
       * config).  Fall through to Aspect rather than mis-labeling. */
   }
   if (s->uints.video_aspect_ratio_idx == ASPECT_RATIO_SQUARE)
      return DP_SCALING_ASPECT_SQUARE;
   return DP_SCALING_ASPECT;
}

static void dp_frontend_scaling_apply(enum dp_scaling_mode mode)
{
   settings_t *s = config_get_ptr();
   if (!s)
      return;
   switch (mode)
   {
      case DP_SCALING_NATIVE:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_CORE;
         s->bools.video_scale_integer        = true;
         s->uints.video_scale_integer_scaling
                                             = VIDEO_SCALE_INTEGER_SCALING_UNDERSCALE;
         break;
      case DP_SCALING_FULLSCREEN:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_FULL;
         s->bools.video_scale_integer        = false;
         break;
      case DP_SCALING_ASPECT_SQUARE:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_SQUARE;
         s->bools.video_scale_integer        = false;
         break;
      case DP_SCALING_NATIVE_SQUARE:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_SQUARE;
         s->bools.video_scale_integer        = true;
         s->uints.video_scale_integer_scaling
                                             = VIDEO_SCALE_INTEGER_SCALING_UNDERSCALE;
         break;
      case DP_SCALING_CROPPED_SQUARE:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_SQUARE;
         s->bools.video_scale_integer        = true;
         s->uints.video_scale_integer_scaling
                                             = VIDEO_SCALE_INTEGER_SCALING_OVERSCALE;
         break;
      case DP_SCALING_ASPECT:
      default:
         s->uints.video_aspect_ratio_idx     = ASPECT_RATIO_CORE;
         s->bools.video_scale_integer        = false;
         break;
   }
   /* SET_ASPECT_RATIO already raises the SHOULD_RESIZE flag on every
    * modern video driver via video_driver_set_aspect_ratio(); a
    * follow-up APPLY_STATE_CHANGES would be redundant. */
   command_event(CMD_EVENT_VIDEO_SET_ASPECT_RATIO, NULL);
}

/* Slang shader presets shown by Screen Effect.  Paths are relative to
 * <directory_video_shader>/shaders_slang and must match the layout that
 * downplay_setup.c lays down for the slang shaders bucket.
 *
 * match_pass_stem is a fallback identifier for read-back when the
 * .slang pass filenames don't share a stem with the .slangp basename.
 * Most presets have a pass that names them (e.g. crt-easymode.slangp
 * → crt-easymode.slang), so leave it NULL.  Required for:
 *   - LCD:        pass is zfast_lcd.slang  (preset has a dash)
 *   - Dot Matrix: passes are gb-pass0..gb-pass4.slang
 * Without this, dp_shader_current_idx returns 0 (Off) for these
 * presets even when they're loaded.
 *
 * param_overrides is a NULL-terminated list of #pragma parameter
 * tweaks applied after the preset loads — used to ship Downplay's
 * own defaults on top of the upstream shader (e.g. softer LCD grid,
 * subtler crt-geom curvature).  Captured by Save Changes since we
 * write to both the live and menu shader copies. */
typedef struct
{
   const char *id;
   float       value;
} dp_shader_param_override_t;

typedef struct
{
   const char                       *label;
   const char                       *path;
   const char                       *match_pass_stem;
   const dp_shader_param_override_t *param_overrides;
} dp_shader_preset_t;

/* zfast-lcd's BORDERMULT default (14.0) draws fairly dark grid lines.
 * 10.0 is gentler — the LCD structure stays visible without the lines
 * fighting the picture for attention. */
static const dp_shader_param_override_t dp_lcd_overrides[] = {
   { "BORDERMULT", 10.0f },
   { NULL,          0.0f }
};

/* CRT Monitor — broadcast/PVM look:
 *  - R=5.0:        gentler bend than crt-geom default (2.0); the
 *                  "barely there" arc of a pro monitor.
 *  - cornersize:   shrunk from default 0.03 to 0.015 — more squared-
 *                  off than a consumer set, but with enough rounding
 *                  to read as a real bezel rather than a hard
 *                  rectangle.  Stays well below TV's 0.04. */
static const dp_shader_param_override_t dp_geom_overrides[] = {
   { "R",          5.0f   },
   { "cornersize", 0.015f },
   { NULL,         0.0f   }
};

/* CRT TV — consumer-set look:
 *  - R=2.5:        more pronounced bend than geom-deluxe default
 *                  (3.5), closer to a real living-room CRT bow.
 *  - cornersize:   pushed from default 0.01 up to 0.04 — visibly
 *                  rounded corners, the way a consumer set's bezel
 *                  hugged the tube.
 *  - halation:     0.13 (default 0.1) — slightly brighter bloom on
 *                  whites; consumer sets bloomed more aggressively
 *                  than studio monitors but we keep it tasteful.
 *  - aperture_strength: 0.32 (default 0.4) — slightly softer mask
 *                  gives the halation room to read.
 *  - scanline_weight: 0.32 (default 0.3) — tiny scanline bump
 *                  reinforces the across-the-room TV feel. */
static const dp_shader_param_override_t dp_geom_deluxe_overrides[] = {
   { "R",                 2.5f  },
   { "cornersize",        0.04f },
   { "halation",          0.13f },
   { "aperture_strength", 0.32f },
   { "scanline_weight",   0.32f },
   { NULL,                0.0f  }
};

static const dp_shader_preset_t dp_shader_presets[] = {
   { "LCD",         "handheld/zfast-lcd.slangp",                "zfast_lcd", dp_lcd_overrides },
   { "Dot Matrix",  "handheld/gameboy-color-dot-matrix.slangp", "gb-pass0",  NULL },
   { "CRT",         "crt/crt-easymode.slangp",                  NULL,        NULL },
   { "CRT TV",      "crt/crt-geom-deluxe.slangp",               NULL,        dp_geom_deluxe_overrides },
   { "CRT Monitor", "crt/crt-geom.slangp",                      NULL,        dp_geom_overrides },
};
#define DP_SHADER_PRESET_COUNT \
   (sizeof(dp_shader_presets)/sizeof(dp_shader_presets[0]))

/* Per-row userdata for the Screen Effect cycler.  preset_idx[i] is
 * the index into dp_shader_presets[] for visible row i, or SIZE_MAX
 * for the "Off" sentinel at row 0.  labels mirrors dp_shader_presets[].label
 * (or "Off"); rows[].values points at this array. */
typedef struct
{
   downplay_handle_t *dp;
   size_t             preset_idx[DP_SHADER_PRESET_COUNT + 1];
   const char        *labels[DP_SHADER_PRESET_COUNT + 1];
   size_t             count;
} dp_shader_row_ud_t;

/* Build absolute path: <video_shader_dir>/shaders_slang/<rel>. */
static bool dp_shader_resolve_path(const char *rel, char *out, size_t out_len)
{
   settings_t *s = config_get_ptr();
   char        mid[PATH_MAX_LENGTH];
   if (!s || !*s->paths.directory_video_shader)
      return false;
   fill_pathname_join_special(mid, s->paths.directory_video_shader,
         "shaders_slang", sizeof(mid));
   fill_pathname_join_special(out, mid, rel, out_len);
   return true;
}

/* Set parameter `id` on `s` to `value`, no-op if not present. */
static void dp_shader_set_param(struct video_shader *s,
      const char *id, float value)
{
   unsigned i;
   if (!s)
      return;
   for (i = 0; i < s->num_parameters; i++)
   {
      if (string_is_equal(s->parameters[i].id, id))
      {
         s->parameters[i].current = value;
         return;
      }
   }
}

/* Apply Downplay's #pragma parameter overrides to both shader copies:
 *   - the video driver's live shader (so the picture changes on the
 *     next set_params call), and
 *   - menu_shader (so Save Changes serialises our values to the
 *     per-core / per-game .slangp).
 *
 * The live-shader write assumes the synchronous shader-load path
 * (DEFAULT_SHADER_DEFERRED_LOADING == false, the Android default).
 * If deferred loading is ever enabled, this races: the chain may
 * still be compiling and our writes land on a struct that's about
 * to be freed.  Re-evaluate this hook point if that flag flips. */
static void dp_shader_apply_param_overrides(size_t preset_idx)
{
   const dp_shader_param_override_t *o;
   video_shader_ctx_t                ctx;
   struct video_shader              *live;
   struct video_shader              *menu;

   if (preset_idx >= DP_SHADER_PRESET_COUNT)
      return;
   o = dp_shader_presets[preset_idx].param_overrides;
   if (!o)
      return;

   live = video_shader_driver_get_current_shader(&ctx) ? ctx.data : NULL;
   menu = menu_shader_get();
   for (; o->id; o++)
   {
      dp_shader_set_param(live, o->id, o->value);
      dp_shader_set_param(menu, o->id, o->value);
   }
}

/* Apply (or clear) the Effect shader.  Called from the Frontend
 * submenu's on_close after deferred staging — see dp_frontend_on_close
 * for why scrolling stages instead of applying live.  effect_preset_idx
 * == SIZE_MAX (or out of range) clears the running shader. */
static void dp_frontend_apply_effect(size_t effect_preset_idx)
{
   char effect_abs[PATH_MAX_LENGTH];
   bool have_effect = (effect_preset_idx < DP_SHADER_PRESET_COUNT)
        && dp_shader_resolve_path(dp_shader_presets[effect_preset_idx].path,
              effect_abs, sizeof(effect_abs))
        && path_is_valid(effect_abs);

   menu_shader_manager_set_preset(menu_shader_get(),
         RARCH_SHADER_SLANG,
         have_effect ? effect_abs : NULL, true);

   if (have_effect)
      dp_shader_apply_param_overrides(effect_preset_idx);
}

/* Does any visible row in `ud` claim to be `stem`?  A preset matches
 * if `stem` equals either its .slangp basename (extension stripped)
 * or its match_pass_stem override.  Returns the row index, or 0.
 *
 * Assumes pass filenames are unique across the preset table.  If a
 * future preset reuses a .slang from another preset (e.g. a generic
 * `linearize.slang` shared by two effects), this returns the first
 * match in row order, which may be wrong. */
static size_t dp_shader_match_stem(
      const dp_shader_row_ud_t *ud, const char *stem)
{
   size_t i;
   for (i = 1; i < ud->count; i++)
   {
      const dp_shader_preset_t *p;
      char                      preset_stem[NAME_MAX_LENGTH];
      const char               *bn;
      if (ud->preset_idx[i] >= DP_SHADER_PRESET_COUNT)
         continue;
      p = &dp_shader_presets[ud->preset_idx[i]];
      if (p->match_pass_stem && string_is_equal(p->match_pass_stem, stem))
         return i;
      bn = path_basename(p->path);
      if (!bn)
         continue;
      strlcpy(preset_stem, bn, sizeof(preset_stem));
      path_remove_extension(preset_stem);
      if (string_is_equal(preset_stem, stem))
         return i;
   }
   return 0;
}

/* Identify which preset (if any) is currently loaded.
 *
 * Two paths to a match:
 *
 * 1. shader->loaded_preset_path is the .slangp the user explicitly
 *    loaded — when that's one of our presets, basename comparison is
 *    direct.  Falls back to shader->path (the resolved root preset)
 *    when loaded_preset_path is empty, which can happen when the
 *    shader was set without going through the preset loader.
 *
 * 2. Per-pass scan over shader->pass[].source.path.  Catches Save
 *    Changes wrappers (auto-loaded per-core/per-game .slangp files),
 *    where loaded_preset_path is the wrapper but the passes are still
 *    the original preset's .slang files.  match_pass_stem in the
 *    preset table covers the cases where the .slang basename doesn't
 *    match the .slangp basename. */
static size_t dp_shader_current_idx(const dp_shader_row_ud_t *ud)
{
   struct video_shader *shader = menu_shader_get();
   const char          *src;
   unsigned             p;
   size_t               match;
   char                 cur_stem[NAME_MAX_LENGTH];

   if (!shader || shader->passes == 0)
      return 0;

   src = (shader->loaded_preset_path[0] != '\0')
       ? shader->loaded_preset_path : shader->path;
   if (src && *src)
   {
      const char *bn = path_basename(src);
      if (bn && *bn)
      {
         strlcpy(cur_stem, bn, sizeof(cur_stem));
         path_remove_extension(cur_stem);
         match = dp_shader_match_stem(ud, cur_stem);
         if (match)
            return match;
      }
   }

   for (p = 0; p < shader->passes; p++)
   {
      const char *cur = path_basename(shader->pass[p].source.path);
      if (!cur || !*cur)
         continue;
      strlcpy(cur_stem, cur, sizeof(cur_stem));
      path_remove_extension(cur_stem);
      match = dp_shader_match_stem(ud, cur_stem);
      if (match)
         return match;
   }
   return 0;
}

static void dp_frontend_scaling_on_change(int delta, void *userdata)
{
   dp_scaling_row_ud_t      *ud = (dp_scaling_row_ud_t*)userdata;
   downplay_settings_list_t *S;
   size_t                    row_idx;
   (void)delta;
   if (!ud || !ud->dp)
      return;
   S = downplay_settings_top(ud->dp);
   if (!S || S->sel >= S->row_count)
      return;
   row_idx = S->rows[S->sel].idx_value;
   if (row_idx >= ud->count)
      return;
   dp_frontend_scaling_apply((enum dp_scaling_mode)ud->mode[row_idx]);
}

static void dp_frontend_effect_on_change(int delta, void *userdata)
{
   dp_shader_row_ud_t *ud = (dp_shader_row_ud_t*)userdata;
   (void)delta;
   if (!ud || !ud->dp)
      return;
   /* Defer the shader load to on_close — recompiling on every L/R
    * cycle while scrolling caused visible jank.  Mark dirty so the
    * close hook knows to flush the final selection. */
   ud->dp->frontend_effect_dirty = true;
}

/* Flush hook fired when the user backs out of the Frontend submenu.
 * Picks up the final Effect row value and applies it.  No-op if
 * nothing was touched (saves an unnecessary shader recompile). */
static void dp_frontend_on_close(downplay_settings_list_t *L,
      void *userdata)
{
   downplay_handle_t        *dp = (downplay_handle_t*)userdata;
   const dp_shader_row_ud_t *ud;
   size_t                    i;
   size_t                    effect_preset_idx = SIZE_MAX;
   if (!dp || !L || !dp->frontend_effect_dirty)
      return;
   for (i = 0; i < L->row_count; i++)
   {
      if (L->rows[i].on_change != dp_frontend_effect_on_change)
         continue;
      ud = (const dp_shader_row_ud_t*)L->rows[i].userdata;
      if (ud && L->rows[i].idx_value < ud->count)
         effect_preset_idx = ud->preset_idx[L->rows[i].idx_value];
      break;
   }
   dp_frontend_apply_effect(effect_preset_idx);
}

static downplay_settings_list_t *downplay_build_frontend_list(
      downplay_handle_t *dp)
{
   downplay_settings_list_t *L;
   dp_scaling_row_ud_t      *scaling_ud;
   dp_shader_row_ud_t       *shader_ud;
   downplay_settings_row_t  *r;
   size_t                    i;
   size_t                    out_row   = 0;
   size_t                    pool_sz;
   unsigned                  square_w  = 0;
   unsigned                  square_h  = 0;
   bool                      show_sq;
   enum dp_scaling_mode      cur_mode;
   /* Slang shader pipeline only works on slang-capable contexts
    * (vulkan / glcore / d3d10+ / metal).  On gl1/gl/glsl drivers,
    * menu_shader_manager_set_preset silently fails — the user would
    * cycle the row but see no change.  Hide the row entirely on
    * those drivers rather than presenting a broken control.  Also
    * defends against builds compiled without a menu shader manager
    * (menu_shader_get returning NULL, which the helpers already
    * tolerate but the row itself would still be a no-op). */
   bool                      slang_ok  = video_driver_test_all_flags(
         GFX_CTX_FLAGS_SHADERS_SLANG) && menu_shader_get() != NULL;
   size_t                    row_count = slang_ok ? 2 : 1;
   char                      path[PATH_MAX_LENGTH];

   /* userdata_pool layout: scaling_ud first, then shader_ud iff slang.
    * Alignment is guaranteed because (a) calloc returns memory aligned
    * to alignof(max_align_t), and (b) sizeof(dp_scaling_row_ud_t) is
    * padded by the compiler to a multiple of its own alignment, which
    * is at least sizeof(void*) since the struct leads with a pointer
    * — so the offset places shader_ud's leading pointer member on a
    * pointer-aligned address on both LP64 and ILP32 ABIs. */
   pool_sz = sizeof(dp_scaling_row_ud_t)
           + (slang_ok ? sizeof(dp_shader_row_ud_t) : 0);
   L       = downplay_settings_list_new("Frontend", row_count, pool_sz, 60);
   if (!L)
      return NULL;

   /* Screen Scaling — build the variable-length value list and label
    * the SQUARE rows with the source's natural DAR (e.g. "Aspect
    * (8:7)") so the user can see the ratio they're picking. */
   scaling_ud      = (dp_scaling_row_ud_t*)L->userdata_pool;
   scaling_ud->dp  = dp;
   show_sq         = dp_scaling_compute_square_dar(&square_w, &square_h);

   /* Common-case rows always shown. */
   scaling_ud->labels[0] = "Aspect";
   scaling_ud->mode[0]   = DP_SCALING_ASPECT;
   scaling_ud->labels[1] = "Native";
   scaling_ud->mode[1]   = DP_SCALING_NATIVE;
   scaling_ud->labels[2] = "Fullscreen";
   scaling_ud->mode[2]   = DP_SCALING_FULLSCREEN;
   scaling_ud->count     = 3;

   /* Square-PAR variants — only when the core's pixels actually differ
    * from square (otherwise these would be visual duplicates of the
    * common rows above). */
   if (show_sq)
   {
      snprintf(scaling_ud->storage[3], DP_SCALING_LABEL_MAX,
            "Aspect (%u:%u)", square_w, square_h);
      scaling_ud->labels[3] = scaling_ud->storage[3];
      scaling_ud->mode[3]   = DP_SCALING_ASPECT_SQUARE;
      snprintf(scaling_ud->storage[4], DP_SCALING_LABEL_MAX,
            "Native (%u:%u)", square_w, square_h);
      scaling_ud->labels[4] = scaling_ud->storage[4];
      scaling_ud->mode[4]   = DP_SCALING_NATIVE_SQUARE;
      snprintf(scaling_ud->storage[5], DP_SCALING_LABEL_MAX,
            "Cropped (%u:%u)", square_w, square_h);
      scaling_ud->labels[5] = scaling_ud->storage[5];
      scaling_ud->mode[5]   = DP_SCALING_CROPPED_SQUARE;
      scaling_ud->count     = 6;
   }

   /* Map the current settings_t state back to a row idx.  When the
    * SQUARE block is suppressed (square-PAR core, or migrating from an
    * older Downplay where Native wrote SQUARE+integer+UNDERSCALE), any
    * SQUARE-flavor mode in settings_t has no row to display.  Remap
    * each to its CORE-PAR sibling for the search — visually identical
    * on a square-PAR core, and the rendered picture matches the
    * displayed label.  The first user-driven cycle re-applies the
    * canonical settings_t state for that row. */
   cur_mode = dp_frontend_scaling_current_mode();
   if (!show_sq)
   {
      if (cur_mode == DP_SCALING_ASPECT_SQUARE)
         cur_mode = DP_SCALING_ASPECT;
      else if (cur_mode == DP_SCALING_NATIVE_SQUARE
            || cur_mode == DP_SCALING_CROPPED_SQUARE)
         cur_mode = DP_SCALING_NATIVE;
   }
   {
      size_t cur_idx = 0;
      for (i = 0; i < scaling_ud->count; i++)
      {
         if (scaling_ud->mode[i] == (uint8_t)cur_mode)
         {
            cur_idx = i;
            break;
         }
      }

      r              = &L->rows[out_row++];
      r->title       = "Screen Scaling";
      r->desc        = "Aspect fits the screen; Native is integer-"
                       "scaled; Fullscreen stretches.  Parenthesized "
                       "variants use 1:1 pixels.";
      r->values      = scaling_ud->labels;
      r->values_count = scaling_ud->count;
      r->idx_value   = cur_idx;
      r->on_change   = dp_frontend_scaling_on_change;
      r->userdata    = scaling_ud;
   }

   /* Screen Effect — only when the running video driver speaks slang. */
   if (slang_ok)
   {
      shader_ud      = (dp_shader_row_ud_t*)((char*)L->userdata_pool
                              + sizeof(dp_scaling_row_ud_t));
      shader_ud->dp  = dp;
      /* Row 0 is always Off; further rows added per existing preset. */
      shader_ud->labels[0]     = "Off";
      shader_ud->preset_idx[0] = SIZE_MAX;
      shader_ud->count         = 1;
      for (i = 0; i < DP_SHADER_PRESET_COUNT; i++)
      {
         if (!dp_shader_resolve_path(dp_shader_presets[i].path,
                  path, sizeof(path)))
            continue;
         if (!path_is_valid(path))
            continue;
         shader_ud->labels[shader_ud->count]     = dp_shader_presets[i].label;
         shader_ud->preset_idx[shader_ud->count] = i;
         shader_ud->count++;
      }

      r              = &L->rows[out_row++];
      r->title       = "Screen Effect";
      r->desc        = "Apply a shader over the picture.  Install the "
                       "Slang Shaders content bucket for more options.";
      r->values      = shader_ud->labels;
      r->values_count = shader_ud->count;
      r->idx_value   = dp_shader_current_idx(shader_ud);
      r->on_change   = dp_frontend_effect_on_change;
      r->userdata    = shader_ud;

      /* Wire the close-flush so Effect scrolling stages in row state
       * and applies once on back-out — only meaningful when the
       * Effect row exists.  See dp_frontend_on_close. */
      L->on_close          = dp_frontend_on_close;
      L->on_close_userdata = dp;
   }

   return L;
}

static void downplay_action_open_frontend(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_settings_push(dp, downplay_build_frontend_list(dp));
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

/* Save Changes submenu — three scoped persist operations.  Each
 * action calls into the existing retroarch.h helpers (no new patch
 * point needed) and shows a confirm/ack via DOWNPLAY_VIEW_CONFIRM.
 *
 * Persistence model recap (M8): config_save_on_exit is forced off in
 * downplay_defaults_apply, so settings_t mutations made via the
 * Frontend submenu are session-only by default.  These actions are
 * the explicit commit:
 *   - core options:  flush / create_override (RA's per-core .opt mech)
 *   - RA settings:   CMD_EVENT_MENU_SAVE_CURRENT_CONFIG_OVERRIDE_*
 *                    (writes <config>/<core>/<core|game>.cfg as a
 *                    diff against base — RA stacks it on next launch)
 *   - shader preset: menu_shader_manager_save_auto_preset for the
 *                    matching SHADER_PRESET_CORE/GAME slot, or the
 *                    matching remove_auto_preset when the user picked
 *                    Off (so the prior preset doesn't keep auto-loading) */

static void downplay_save_shader_for_scope(downplay_handle_t *dp,
      enum auto_shader_type type)
{
   settings_t          *s      = config_get_ptr();
   struct video_shader *shader = menu_shader_get();
   if (!s || !dp)
      return;
   /* Skip the shader pipe entirely when the user didn't touch Screen
    * Effect this session.  Without this guard, a user who saves an
    * Effect preset, returns later to change a core option only, and
    * hits Save Changes would lose their saved preset (passes==0 on
    * the freshly-initialised menu_shader would route to the remove
    * branch and delete the on-disk .slangp). */
   if (!dp->frontend_effect_dirty)
      return;
   /* No shader pipeline in this build, or no slang context — nothing
    * to persist on this axis.  The Frontend submenu hides Effect in
    * those configurations, but Save Changes is reachable independently
    * (the user could have built a per-core .slangp by other means);
    * silently skip rather than removing what we didn't touch. */
   if (!shader)
      return;
   if (shader->passes > 0 && *shader->path)
      menu_shader_manager_save_auto_preset(shader, type,
            s->paths.directory_video_shader,
            s->paths.directory_menu_config, true);
   else
      menu_shader_manager_remove_auto_preset(type,
            s->paths.directory_video_shader,
            s->paths.directory_menu_config);
   /* "dirty since last save" — reset so a subsequent unrelated Save
    * (e.g. user toggled a core option later) doesn't re-touch the
    * shader file.  See the matching dirty-flag set in
    * dp_frontend_effect_on_change. */
   dp->frontend_effect_dirty = false;
}

/* Save the current state as the per-core defaults: core options to
 * <core>.opt, RA settings to <core>.cfg override, shader to
 * <core>.slangp.  Applies to every game launched with this core
 * unless a per-game override exists. */
static void downplay_action_save_for_console(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   core_options_flush();
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG_OVERRIDE_CORE, NULL);
   downplay_save_shader_for_scope(dp, SHADER_PRESET_CORE);
   downplay_open_confirm(dp, "Saved for console.", "OKAY", NULL, NULL);
}

/* Save the current state as a per-game override: <core>/<game>.opt
 * for core options, <core>/<game>.cfg for RA settings,
 * <core>/<game>.slangp for the shader.  Takes precedence over the
 * per-core files when this specific ROM is loaded next time. */
static void downplay_action_save_for_game(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   core_options_create_override(true);
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG_OVERRIDE_GAME, NULL);
   downplay_save_shader_for_scope(dp, SHADER_PRESET_GAME);
   downplay_open_confirm(dp, "Saved for game.", "OKAY", NULL, NULL);
}

/* Restore step 2: user pressed YES on the confirm prompt.  Reset
 * every option to its libretro-declared default in memory, then
 * flush so the change is durable on disk.  We don't trigger our
 * settings_rebuild_pending here — the rebuild path assumes the
 * top-of-stack frame is the core-options list, but Restore is
 * invoked from the Save Changes submenu (one level above).  When
 * the user navigates back into Emulator the list is rebuilt fresh
 * from coreopts->opts[i].index anyway. */
static void downplay_action_restore_defaults_confirmed(void *userdata)
{
   (void)userdata;
   /* Guard against the core having unloaded between the user
    * pressing YES on the prompt and this callback firing.  Both
    * helpers below dereference runloop core-options state and
    * would touch freed memory if the core is gone. */
   if (!(runloop_get_flags() & RUNLOOP_FLAG_CORE_RUNNING))
      return;
   core_options_reset(NULL);
   core_options_flush();
}

/* Restore step 1: open a YES/CANCEL prompt before doing anything
 * destructive.  YES chains to ..._confirmed above. */
static void downplay_action_restore_defaults(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_open_confirm(dp,
         "Restore all options to defaults?",
         "YES", "CANCEL",
         downplay_action_restore_defaults_confirmed);
}

static downplay_settings_list_t *downplay_build_save_changes_list(
      downplay_handle_t *dp)
{
   downplay_settings_list_t *L =
      downplay_settings_list_new("Save Changes", 3, 0, 60);
   if (!L)
      return NULL;
   L->rows[0].title      = "Save for console";
   L->rows[0].desc       = "Use these values for every game with this core.";
   L->rows[0].on_confirm = downplay_action_save_for_console;
   L->rows[0].userdata   = dp;
   L->rows[1].title      = "Save for game";
   L->rows[1].desc       = "Use these values only for the current game.";
   L->rows[1].on_confirm = downplay_action_save_for_game;
   L->rows[1].userdata   = dp;
   L->rows[2].title      = "Restore defaults";
   L->rows[2].desc       = "Reset all options to the core's defaults.";
   L->rows[2].on_confirm = downplay_action_restore_defaults;
   L->rows[2].userdata   = dp;
   return L;
}

static void downplay_action_open_save_changes(void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   downplay_settings_push(dp, downplay_build_save_changes_list(dp));
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
   L->rows[4].on_confirm = downplay_action_open_save_changes;
   L->rows[4].userdata   = dp;
   return L;
}

/* ---- Global Settings — launcher-level toggles, opened from the
 * top view via Start.  Three rows: Swap A/B, Clock Format, Menu
 * Scale.  Each persists immediately to the base retroarch.cfg via
 * CMD_EVENT_MENU_SAVE_CURRENT_CONFIG since downplay_defaults_apply
 * forces config_save_on_exit = false (so without an explicit save,
 * changes here would vanish at process quit).  Reachable only from
 * DOWNPLAY_VIEW_TOP, where there is no content loaded — so
 * OVERRIDE_NONE always lands in the global cfg, never a per-core
 * override. */

/* Percentage scale presets multiplied on top of the base
 * video_height/DOWNPLAY_REF_HEIGHT scaling.  5% steps from 75% to
 * 125% — fine-grained enough that any device should land within
 * one step of comfortable.  Single source of truth: the float
 * multiplier is pct/100.0f and the row label is "NN%". */
static const unsigned dp_global_scale_pct[] = {
   75, 80, 85, 90, 95, 100, 105, 110, 115, 120, 125
};
#define DP_GLOBAL_SCALE_COUNT \
   (sizeof(dp_global_scale_pct) / sizeof(dp_global_scale_pct[0]))

/* Lazily-built parallel arrays of label strings + pointers.  Built
 * once on first list construction so the values array passed to the
 * settings row points into stable storage. */
static char        dp_global_scale_label_buf[DP_GLOBAL_SCALE_COUNT][8];
static const char *dp_global_scale_labels[DP_GLOBAL_SCALE_COUNT];

static void dp_global_scale_labels_build(void)
{
   static bool built = false;
   size_t      i;
   if (built)
      return;
   for (i = 0; i < DP_GLOBAL_SCALE_COUNT; i++)
   {
      snprintf(dp_global_scale_label_buf[i],
            sizeof(dp_global_scale_label_buf[i]),
            "%u%%", dp_global_scale_pct[i]);
      dp_global_scale_labels[i] = dp_global_scale_label_buf[i];
   }
   built = true;
}

static void dp_global_persist(void)
{
   command_event(CMD_EVENT_MENU_SAVE_CURRENT_CONFIG, NULL);
}

/* Read the active row's idx_value from the top settings frame.
 * Used by every on_change below — the framework writes idx_value
 * before invoking us, so the new state is already there. */
static size_t dp_global_active_idx(downplay_handle_t *dp)
{
   downplay_settings_list_t *S = downplay_settings_top(dp);
   if (!S || S->sel >= S->row_count)
      return 0;
   return S->rows[S->sel].idx_value;
}

static void dp_global_swap_ab_on_change(int delta, void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   settings_t        *s  = config_get_ptr();
   (void)delta;
   if (!dp || !s)
      return;
   s->bools.input_menu_swap_ok_cancel_buttons =
         (dp_global_active_idx(dp) == 1);
   dp_global_persist();
}

/* Two-state mapping: 0 → 24-hour (HM), 1 → 12-hour (HM_AMPM).
 * Overwrites any pre-existing date+time style — Downplay's status
 * pill is time-only, and we don't expose the full upstream
 * timedate menu, so this is the only sanctioned way to set it. */
static void dp_global_clock_on_change(int delta, void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   settings_t        *s  = config_get_ptr();
   (void)delta;
   if (!dp || !s)
      return;
   s->uints.menu_timedate_style = (dp_global_active_idx(dp) == 1)
         ? MENU_TIMEDATE_STYLE_HM_AMPM
         : MENU_TIMEDATE_STYLE_HM;
   dp_global_persist();
}

static void dp_global_scale_on_change(int delta, void *userdata)
{
   downplay_handle_t *dp = (downplay_handle_t*)userdata;
   settings_t        *s  = config_get_ptr();
   size_t             idx;
   (void)delta;
   if (!dp || !s)
      return;
   idx = dp_global_active_idx(dp);
   if (idx >= DP_GLOBAL_SCALE_COUNT)
      return;
   s->floats.menu_scale_factor = (float)dp_global_scale_pct[idx] / 100.0f;
   dp_global_persist();
   /* Layout picks up the new scale on the next frame via
    * downplay_layout_needs_recompute watching menu_scale_factor —
    * no explicit recompute call needed. */
}

/* Map an arbitrary on-disk menu_scale_factor onto the closest
 * bucket so the cursor lands on something sensible even when the
 * user (or a prior install) wrote a value outside our discrete
 * set. */
static size_t dp_global_scale_current_idx(float current)
{
   size_t i, best   = 0;
   float  best_diff = 1e9f;
   for (i = 0; i < DP_GLOBAL_SCALE_COUNT; i++)
   {
      float d = current - (float)dp_global_scale_pct[i] / 100.0f;
      if (d < 0.0f)
         d = -d;
      if (d < best_diff)
      {
         best_diff = d;
         best      = i;
      }
   }
   return best;
}

static downplay_settings_list_t *downplay_build_global_settings_list(
      downplay_handle_t *dp)
{
   static const char * const off_on[]    = { "Off", "On" };
   static const char * const clock_lab[] = { "24-hour", "12-hour" };

   downplay_settings_list_t *L;
   downplay_settings_row_t  *r;
   settings_t               *s = config_get_ptr();
   if (!s)
      return NULL;
   dp_global_scale_labels_build();
   L = downplay_settings_list_new("Settings", 3, 0, 60);
   if (!L)
      return NULL;

   r               = &L->rows[0];
   r->title        = "Swap A/B";
   r->desc         = "Use B to confirm and A to cancel.";
   r->values       = off_on;
   r->values_count = 2;
   r->idx_value    = s->bools.input_menu_swap_ok_cancel_buttons ? 1 : 0;
   r->on_change    = dp_global_swap_ab_on_change;
   r->userdata     = dp;

   r               = &L->rows[1];
   r->title        = "Clock Format";
   r->desc         = "How the status clock is displayed.";
   r->values       = clock_lab;
   r->values_count = 2;
   r->idx_value    = (s->uints.menu_timedate_style
                        == MENU_TIMEDATE_STYLE_HM_AMPM) ? 1 : 0;
   r->on_change    = dp_global_clock_on_change;
   r->userdata     = dp;

   r               = &L->rows[2];
   r->title        = "Menu Scale";
   r->desc         = "Size of menu text and controls.";
   r->values       = dp_global_scale_labels;
   r->values_count = DP_GLOBAL_SCALE_COUNT;
   r->idx_value    = dp_global_scale_current_idx(
         s->floats.menu_scale_factor);
   r->on_change    = dp_global_scale_on_change;
   r->userdata     = dp;

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

/* Smoothstep fade duration for right-pane art changes (in microseconds).
 * 120 ms feels close to LessUI's 100 ms but a touch softer at our larger
 * art sizes; below ~80 ms the eye reads it as a hard pop. */
#define DP_PREVIEW_FADE_US 120000

/* Aspect-fit a (src_w × src_h) image into a (pane_w × pane_h) box;
 * return the resulting on-screen width in pixels.  Cross-product
 * comparison keeps the math integer-only and matches the body of the
 * FIT branch in downplay_draw_right_preview — same answer at the
 * preview draw and at the row-text gate, so the image's left edge
 * agrees in both places.  Returns 0 on degenerate inputs (caller
 * treats as "no art slot to reserve"). */
static int downplay_image_pane_width(int pane_w, int pane_h,
      unsigned src_w, unsigned src_h)
{
   if (pane_w <= 0 || pane_h <= 0 || src_w == 0 || src_h == 0)
      return 0;
   /* pane_w/pane_h <= src_w/src_h  →  fit by width (img_w == pane_w).
    * Otherwise fit by height (img_w shrinks below pane_w). */
   if ((unsigned)pane_w * src_h <= (unsigned)pane_h * src_w)
      return pane_w;
   return (int)((unsigned)pane_h * src_w / src_h);
}

static void downplay_draw_right_preview(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, downplay_handle_t *dp,
      uintptr_t tex, unsigned texture_w, unsigned texture_h,
      enum downplay_preview_scale scale)
{
   int pane_left   = L->pane_left;
   int pane_right  = (int)L->vid_w - L->margin_x;
   /* Symmetric 16*scale gap above the row 0 / status-pill band and below
    * the footer-hint band so the art reads as floating in the middle of
    * the chrome rather than crowding either edge. */
   int pane_gap    = (int)(16.0f * L->scale);
   int pane_top    = L->margin_y + L->row_h + pane_gap;
   int pane_bottom = (int)L->vid_h - L->margin_y - L->row_h - pane_gap;
   int pane_w      = pane_right  - pane_left;
   int pane_h      = pane_bottom - pane_top;
   int img_w       = 0;
   int img_h       = 0;
   int img_x;
   int img_y;
   bool fits_natively;
   retro_time_t now;
   float        t;
   float        alpha;
   float        color[16];
   uintptr_t    tex_local;
   int          i;

   /* No texture → nothing to show.  We deliberately don't draw a
    * placeholder: legitimately art-less rows (homebrew, MISSING after
    * a probe) and rows mid-resolve all want a clean empty pane. */
   if (!tex || !texture_w || !texture_h)
   {
      /* Reset fade state so the *next* texture starts fading from 0. */
      dp->preview_last_tex = 0;
      return;
   }
   if (pane_w < L->row_h || pane_h < L->row_h)
      return;

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
       * Helper keeps the same math used by the per-row text gate so
       * the image's left edge agrees at both sites. */
      img_w = downplay_image_pane_width(pane_w, pane_h, texture_w, texture_h);
      if (img_w == pane_w)
         img_h = (int)((unsigned)pane_w * texture_h / texture_w);
      else
         img_h = pane_h;
   }
   if (img_w < 1 || img_h < 1)
      return;

   /* Right-anchored: hug the screen-edge margin so small art reads as
    * pinned to the side instead of floating in the middle of the pane. */
   img_x = pane_right - img_w;
   img_y = pane_top  + (pane_h - img_h) / 2;

   /* Restart fade whenever the texture identity changes.  Selection
    * moves, async loads, MISSING→OK retries — all hit this branch.
    * One timestamp read per frame: reusing `now` for the fade origin
    * gives the first frame `t == 0` exactly, so the alpha < 1/255
    * guard below skips its draw cleanly. */
   now = cpu_features_get_time_usec();
   if (tex != dp->preview_last_tex)
   {
      dp->preview_last_tex        = tex;
      dp->preview_fade_started_us = now;
   }
   t   = (float)(now - dp->preview_fade_started_us)
       / (float)DP_PREVIEW_FADE_US;
   if (t < 0.0f) t = 0.0f;
   if (t > 1.0f) t = 1.0f;
   alpha = t * t * (3.0f - 2.0f * t);
   /* Drop the draw at sub-pixel alpha — saves the GPU a fully-transparent
    * quad on the very first frame after a selection change. */
   if (alpha < (1.0f / 255.0f))
      return;

   /* Local color buffer: pure white (the no-tint identity for textured
    * quads) with the alpha component scaled by the fade.  Can't mutate
    * DP_COLOR_PILL_LIGHT — it's shared with the row pill draws. */
   for (i = 0; i < 4; i++)
   {
      color[i * 4 + 0] = 1.0f;
      color[i * 4 + 1] = 1.0f;
      color[i * 4 + 2] = 1.0f;
      color[i * 4 + 3] = alpha;
   }
   tex_local = tex;
   gfx_display_draw_quad(p_disp, userdata,
         L->vid_w, L->vid_h,
         img_x, img_y, (unsigned)img_w, (unsigned)img_h,
         L->vid_w, L->vid_h,
         color, &tex_local);
}

static void downplay_draw_list(gfx_display_t *p_disp, void *userdata,
      const downplay_layout_t *L, uintptr_t cap_tex,
      downplay_handle_t *dp)
{
   size_t   i;
   /* INGAME draws a left-anchored title pill on the top row, so the
    * list has to start one row below.  Other views leave that row
    * to the (right-anchored, short) status pill, which sits beside
    * the left-anchored row 0. */
   bool     has_title  = (dp->nav.view == DOWNPLAY_VIEW_INGAME);
   int      list_top   = L->margin_y
                       + (has_title ? L->row_h : 0);
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
    * visible index.  Source of truth is dp->nav.selection; deriving scroll
    * here means input handlers don't need to track it. */
   size_t   scroll     = 0;
   size_t   row_idx;
   int      row_y;
   int      row_w;
   /* Two-pane truncation gate.  In SYSTEM and SAVE_PICKER views, an
    * unselected row whose art we *know* is on disk reserves space for
    * the preview pane.  In SYSTEM view the reservation is per-row,
    * derived from the entry's actual image dimensions in the
    * thumbnail index — portrait box-art leaves more text room than
    * square box-art, and the row never reflows when the image lands
    * (its width was set up-front from the index, not measured after
    * decode).  In SAVE_PICKER, dims aren't known until the texture
    * loads, so the gate is binary on `thumb_tex != 0` and uses the
    * full-pane fallback width.  The selected row keeps full width
    * unconditionally — its pill deliberately overlaps its image. */
   bool     art_pane_active = (dp->nav.view == DOWNPLAY_VIEW_SYSTEM
                            || dp->nav.view == DOWNPLAY_VIEW_RECENTS
                            || dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER);
   int      pane_right      = (int)L->vid_w - L->margin_x;
   int      pane_full_w     = pane_right - L->pane_left;
   /* Pane height matches the right-preview drawer (margin_y +/- a row
    * + a 16*scale gap top/bottom).  Kept in sync there; this mirror
    * is what determines the per-row reservation for SYSTEM. */
   int      pane_gap        = (int)(16.0f * L->scale);
   int      pane_h          = (int)L->vid_h - 2 * (L->margin_y + L->row_h)
                            - 2 * pane_gap;
   /* SAVE_PICKER fallback width: image fills the full pane (square-ish
    * thumbnails — INTEGER-scale path in draw_right_preview).  One
    * row_text_indent of breathing room between the truncated label and
    * the image pane.  Asymmetric vs full-width row_max_w (which
    * doesn't subtract this) by design — the image sits right next to
    * where the truncated text ends; we want a visible gap there. */
   int      row_max_w_full_pane = L->pane_left - L->margin_x
                                - L->row_text_indent;
   /* Top row shares its line with the status pill — shorten its max
    * width by the pill width plus a margin's gap so a long title
    * truncates instead of overlapping.  Skipped in INGAME because
    * has_title pushes the list past that row entirely. */
   int      top_row_max_w = has_title ? row_max_w
         : (row_max_w
            - downplay_status_pill_width(dp->chrome_font, L,
                  dp->status_text)
            - L->margin_x);
   if (top_row_max_w < 0)
      top_row_max_w = 0;
   if (row_max_w_full_pane < 0)
      row_max_w_full_pane = 0;
   if (pane_h < 1)
      pane_h = 1;

   if (dp->total_rows > visible && dp->nav.selection >= visible)
   {
      scroll = dp->nav.selection + 1 - visible;
      if (scroll + visible > dp->total_rows)
         scroll = dp->total_rows - visible;
   }

   /* Right-pane preview FIRST so it sits *under* the row pills/text.
    * The selected row's full-width pill deliberately overlaps the
    * image area (LessUI parity); for that overlap to read as the pill
    * sitting on top of the art, the art has to be the bottom layer.
    *
    * SAVE_PICKER shows the selected save's thumbnail.  SYSTEM shows
    * box art for the selected ROM (loaded asynchronously via
    * gfx_thumbnail_request_stream, see
    * downplay_drive_system_thumbnails).  Both share the same right-
    * pane geometry and the helper's smoothstep fade-in.  When the
    * thumbnail isn't loaded yet (status != AVAILABLE) we pass tex=0,
    * which the helper draws as an empty pane (no placeholder). */
   if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER
         && dp->nav.selection < dp->save_picker_count)
   {
      const downplay_save_entry_t *e = &dp->save_picker[dp->nav.selection];
      downplay_draw_right_preview(p_disp, userdata, L, dp,
            e->thumb_tex, e->thumb_w, e->thumb_h,
            DOWNPLAY_PREVIEW_SCALE_INTEGER);
   }
   else if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM
         && dp->roms && dp->nav.selection < dp->rom_count)
   {
      const gfx_thumbnail_t *t = &dp->roms[dp->nav.selection].thumbnail;
      enum gfx_thumbnail_status st = (enum gfx_thumbnail_status)
            retro_atomic_load_acquire_int((retro_atomic_int_t*)&t->status);
      uintptr_t  tex = 0;
      unsigned   w   = 0;
      unsigned   h   = 0;
      if (st == GFX_THUMBNAIL_STATUS_AVAILABLE)
      {
         tex = t->texture;
         w   = t->width;
         h   = t->height;
      }
      downplay_draw_right_preview(p_disp, userdata, L, dp, tex, w, h,
            DOWNPLAY_PREVIEW_SCALE_FIT);
   }
   else if (dp->nav.view == DOWNPLAY_VIEW_RECENTS
         && dp->recents && dp->nav.selection < dp->recent_row_count)
   {
      const gfx_thumbnail_t *t = &dp->recents[dp->nav.selection].thumbnail;
      enum gfx_thumbnail_status st = (enum gfx_thumbnail_status)
            retro_atomic_load_acquire_int((retro_atomic_int_t*)&t->status);
      uintptr_t  tex = 0;
      unsigned   w   = 0;
      unsigned   h   = 0;
      if (st == GFX_THUMBNAIL_STATUS_AVAILABLE)
      {
         tex = t->texture;
         w   = t->width;
         h   = t->height;
      }
      downplay_draw_right_preview(p_disp, userdata, L, dp, tex, w, h,
            DOWNPLAY_PREVIEW_SCALE_FIT);
   }
   else
   {
      /* Single-pane view (TOP, INGAME): clear the fade tracker so
       * re-entering a two-pane view starts fresh from alpha 0. */
      dp->preview_last_tex = 0;
   }

   /* Rows on top of the preview so the selected row's full-width pill
    * visibly covers the art it overlaps. */
   for (i = 0; i < visible && scroll + i < dp->total_rows; i++)
   {
      bool selected;
      int  row_max_w_art = 0;  /* >0 means "this row has art; clamp here" */

      row_idx = scroll + i;
      row_y   = list_top + (int)(i * (size_t)L->row_h);
      row_w   = (i == 0) ? top_row_max_w : row_max_w;
      selected = (row_idx == dp->nav.selection);

      if (art_pane_active && !selected)
      {
         if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM
               && dp->roms && row_idx < dp->rom_count
               && dp->roms[row_idx].image_w > 0
               && dp->roms[row_idx].image_h > 0)
         {
            /* Per-row width: aspect-fit the index-known dims into the
             * preview pane to find the image's left edge, then clamp
             * the row's text to leave breathing room before it. */
            int img_w = downplay_image_pane_width(pane_full_w, pane_h,
                  dp->roms[row_idx].image_w,
                  dp->roms[row_idx].image_h);
            if (img_w > 0)
               row_max_w_art = (pane_right - img_w) - L->margin_x
                             - L->row_text_indent;
         }
         else if (dp->nav.view == DOWNPLAY_VIEW_RECENTS
               && dp->recents && row_idx < dp->recent_row_count
               && dp->recents[row_idx].image_w > 0
               && dp->recents[row_idx].image_h > 0)
         {
            int img_w = downplay_image_pane_width(pane_full_w, pane_h,
                  dp->recents[row_idx].image_w,
                  dp->recents[row_idx].image_h);
            if (img_w > 0)
               row_max_w_art = (pane_right - img_w) - L->margin_x
                             - L->row_text_indent;
         }
         else if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER
               && row_idx < dp->save_picker_count
               && dp->save_picker[row_idx].thumb_tex != 0)
            row_max_w_art = row_max_w_full_pane;
      }
      if (row_max_w_art > 0 && row_w > row_max_w_art)
         row_w = row_max_w_art;

      downplay_draw_list_row(p_disp, userdata, dp->font,
            dp->font_centre_offset, L, cap_tex,
            list_x, row_y, row_w,
            downplay_row_label(dp, row_idx),
            selected);
   }
}

/* ---------- frame ---------- */

/* Drive per-frame thumbnail loading for the SYSTEM view.
 *
 * The pastime.gg-backed manager owns the lookup logic (per-system
 * index + filename-match cascade); this function is just the bridge
 * between the manager's status output and the gfx_thumbnail_t the
 * renderer reads from.
 *
 * Per-frame steps:
 *   1. Pump the thumbnail manager (drains image-fetch queue,
 *      reconciles in-flight count).
 *   2. If the index just landed (warm-up at system enter saw it
 *      mid-fetch and left every row's image_w/h at 0), iterate rows
 *      once and re-warm — the row drawer keys off image_w/h, so this
 *      is what makes the cold-fetch reflow happen.
 *   3. Reset the prior selection's gfx_thumbnail_t (one resident
 *      texture at a time).
 *   4. Look up the active row's basename via the manager:
 *        - OK      → request_file on local cache path; capture dims
 *          (handles the rare case where a row was added after the
 *          warm-up — e.g. ROM dropped into the folder mid-session).
 *        - MISSING → mark row's gfx_thumbnail status MISSING.
 *        - UNKNOWN → leave PENDING, retry next frame.
 *   5. Lookahead-prefetch the ±5 surrounding rows.
 *
 * No-op outside SYSTEM view, or when the path-data isn't ready
 * (graceful degradation: rows still show filenames). */
static void downplay_drive_system_thumbnails(downplay_handle_t *dp)
{
   const downplay_rom_t *rom;
   const char           *base;
   downplay_thumb_result_t result;
   settings_t           *settings;
   unsigned              upscale_thresh;
   size_t                sel;
   enum gfx_thumbnail_status status;

   if (dp->nav.view != DOWNPLAY_VIEW_SYSTEM)
      return;
   if (!dp->roms || dp->rom_count == 0)
      return;

   sel = dp->nav.selection;
   if (sel >= dp->rom_count)
      sel = dp->rom_count - 1;

   /* Pump the thumbnail manager (drains image queue, reconciles). */
   if (dp->current_thumbs)
      downplay_thumbs_pump(dp->current_thumbs);

   /* Cold-fetch warm-up retry.  open_system tried this once with the
    * manager freshly opened; if the on-disk index wasn't there yet,
    * peek returned false for every row and the flag is still unset.
    * The retry costs nothing once the flag flips. */
   if (!dp->roms_dims_warmed)
      dp_warm_roms_dims(dp);

   /* Selection change → reset the prior selection's thumbnail. */
   if (   dp->thumb_prev_selection_sys != sel
       && dp->thumb_prev_selection_sys < dp->rom_count)
      gfx_thumbnail_reset(&dp->roms[dp->thumb_prev_selection_sys].thumbnail);
   dp->thumb_prev_selection_sys = sel;

   if (!dp->current_thumbs)
      return; /* No db_name → no art for this system, ever. */

   rom  = &dp->roms[sel];
   base = path_basename(rom->full_path);
   if (!base || !*base)
      return;

   settings       = config_get_ptr();
   upscale_thresh = settings
         ? settings->uints.gfx_thumbnail_upscale_threshold : 0;

   downplay_thumbs_request(dp->current_thumbs, base, &result);

   status = (enum gfx_thumbnail_status)
         retro_atomic_load_acquire_int(&dp->roms[sel].thumbnail.status);

   if (result.status == DP_THUMB_OK)
   {
      /* File on disk → kick a load if we haven't already. */
      if (status == GFX_THUMBNAIL_STATUS_UNKNOWN
          || status == GFX_THUMBNAIL_STATUS_MISSING)
      {
#ifdef HAVE_DOWNPLAY_WEBP
         /* WebP path uses our vendored libwebp directly — RA's
          * `task_push_image_load` doesn't know our format-selection
          * rules, and going through it would round-trip through the
          * (broken) `formats/rwebp` shim.  Decode synchronously and
          * publish into the existing gfx_thumbnail_t slot.
          *
          * Tradeoff: this blocks the menu thread for ~5–15 ms per
          * first-load while the JPG path is async.  Acceptable today
          * because the boot-time prefetch (downplay_thumbs.c) warms
          * the on-disk cache before the user navigates, so the I/O is
          * page-cache-hot and decode is the only cost.  If real-world
          * jank shows up on cold storage, move this to a task. */
         const char *ext = strrchr(result.local_path, '.');
         if (ext && !strcmp(ext, ".webp"))
         {
            gfx_thumbnail_t *th = &dp->roms[sel].thumbnail;
            unsigned w = 0, h = 0;
            gfx_thumbnail_reset(th);
            retro_atomic_store_release_int(&th->status,
                  GFX_THUMBNAIL_STATUS_MISSING);
            if (downplay_webp_load_texture(result.local_path,
                     &th->texture, &w, &h))
            {
               th->width  = w;
               th->height = h;
               th->alpha  = 1.0f; /* skip fade for the synchronous path */
               retro_atomic_store_release_int(&th->status,
                     GFX_THUMBNAIL_STATUS_AVAILABLE);
            }
         }
         else
#endif
            gfx_thumbnail_request_file(result.local_path,
                  &dp->roms[sel].thumbnail, upscale_thresh);
      }
      /* Late-bind dims if the warm-up missed this row (e.g. ROM
       * dropped into the folder mid-session, or peek raced against
       * an index reload).  Cheap; idempotent. */
      if (   dp->roms[sel].image_w == 0
          && dp->roms[sel].image_h == 0
          && (result.image_w | result.image_h))
      {
         dp->roms[sel].image_w = result.image_w;
         dp->roms[sel].image_h = result.image_h;
      }
   }
   else if (result.status == DP_THUMB_MISSING)
   {
      /* Definitive miss — index is loaded and the title isn't in it.
       * Park the gfx status at MISSING so the renderer doesn't keep
       * the placeholder up. */
      if (status != GFX_THUMBNAIL_STATUS_MISSING)
         retro_atomic_store_release_int(&dp->roms[sel].thumbnail.status,
               GFX_THUMBNAIL_STATUS_MISSING);
   }
   /* DP_THUMB_UNKNOWN → leave status as-is; next frame will retry. */

   /* Lookahead prefetch: rows ±5 from selection.  Each is at most one
    * HTTP fetch queued per call; the manager de-dups.  The loop's
    * delta range produces at most 10 entries by construction, which
    * matches the array size. */
   {
      const char *names[10];
      size_t      n = 0;
      int         delta;
      for (delta = -5; delta <= 5; delta++)
      {
         long idx;
         if (delta == 0) continue;
         idx = (long)sel + delta;
         if (idx < 0 || idx >= (long)dp->rom_count) continue;
         names[n++] = path_basename(dp->roms[idx].full_path);
      }
      if (n > 0)
         downplay_thumbs_prefetch(dp->current_thumbs, names, n);
   }
}

/* Drive per-frame thumbnail loading for the RECENTS view.  Mirrors
 * downplay_drive_system_thumbnails but reads from the read-only
 * recents resolver — no pump, no prefetch, no fetch.  If the
 * selected row's system has no cached `.idx` or no cached image,
 * the row simply renders without art.  Self-heals on first run as
 * the user opens system views and populates the cache. */
static void downplay_drive_recents_thumbnails(downplay_handle_t *dp)
{
   const downplay_recent_t *row;
   char                     path[1024];
   settings_t              *settings;
   unsigned                 upscale_thresh;
   size_t                   sel;
   enum gfx_thumbnail_status status;

   if (dp->nav.view != DOWNPLAY_VIEW_RECENTS)
      return;
   if (!dp->recents || dp->recent_row_count == 0)
      return;
   if (!dp->recents_thumbs)
      return;

   /* Pre-warm one seeded-but-unloaded system per frame.  No-op once
    * every distinct system in the row set has been settled.  Bounds
    * blocking I/O to one .idx read per frame regardless of how the
    * user scrolls. */
   downplay_thumbs_recents_pump(dp->recents_thumbs);

   /* Populate per-row dims for the row drawer's truncation gate.
    * Side-effect-free + cheap (~50 ns/row × ~30 rows max); each call
    * succeeds the moment the row's system index has been pumped in.
    * Skip rows whose dims already settled — the index doesn't change
    * between frames so once a row hits, it stays hit. */
   {
      size_t             i;
      downplay_recent_t *re;
      uint16_t           w;
      uint16_t           h;
      for (i = 0; i < dp->recent_row_count; i++)
      {
         re = &dp->recents[i];
         w  = 0;
         h  = 0;
         if (re->image_w || re->image_h)
            continue;
         if (!re->db_name || !re->rom_basename)
            continue;
         if (downplay_thumbs_recents_peek(dp->recents_thumbs,
                  re->db_name, re->rom_basename, &w, &h))
         {
            re->image_w = w;
            re->image_h = h;
         }
      }
   }

   sel = dp->nav.selection;
   if (sel >= dp->recent_row_count)
      sel = dp->recent_row_count - 1;

   /* Selection change → reset the prior selection's thumbnail to
    * keep the resident-texture count at one.  gfx_thumbnail.h
    * requires cancel-before-reset: an in-flight load callback
    * could otherwise race onto the freed texture slot.  The cancel
    * is global (bumps RA's thumbnail list_id, dropping every
    * in-flight load), which is fine here — recents only ever has
    * one row's load in flight at a time. */
   if (   dp->thumb_prev_selection_rec != sel
       && dp->thumb_prev_selection_rec < dp->recent_row_count)
   {
      gfx_thumbnail_cancel_pending_requests();
      gfx_thumbnail_reset(&dp->recents[dp->thumb_prev_selection_rec].thumbnail);
   }
   dp->thumb_prev_selection_rec = sel;

   row = &dp->recents[sel];
   if (!row->db_name || !*row->db_name)
      return;
   if (!row->rom_basename || !*row->rom_basename)
      return;

   if (!downplay_thumbs_recents_resolve(dp->recents_thumbs,
            row->db_name, row->rom_basename, path, sizeof(path)))
      return;

   status = (enum gfx_thumbnail_status)
         retro_atomic_load_acquire_int(&dp->recents[sel].thumbnail.status);
   if (status != GFX_THUMBNAIL_STATUS_UNKNOWN
       && status != GFX_THUMBNAIL_STATUS_MISSING)
      return; /* already loaded or in-flight */

   settings       = config_get_ptr();
   upscale_thresh = settings
         ? settings->uints.gfx_thumbnail_upscale_threshold : 0;
#ifdef HAVE_DOWNPLAY_WEBP
   {
      const char *ext = strrchr(path, '.');
      if (ext && !strcmp(ext, ".webp"))
      {
         gfx_thumbnail_t *th = &dp->recents[sel].thumbnail;
         unsigned w = 0, h = 0;
         gfx_thumbnail_reset(th);
         retro_atomic_store_release_int(&th->status,
               GFX_THUMBNAIL_STATUS_MISSING);
         if (downplay_webp_load_texture(path, &th->texture, &w, &h))
         {
            th->width  = w;
            th->height = h;
            th->alpha  = 1.0f;
            retro_atomic_store_release_int(&th->status,
                  GFX_THUMBNAIL_STATUS_AVAILABLE);
         }
         return;
      }
   }
#endif
   gfx_thumbnail_request_file(path, &dp->recents[sel].thumbnail,
         upscale_thresh);
}

/* Drive the INGAME view from the actual core-running state.  Upstream
 * menu drivers don't detect this themselves — task_content sets
 * MENU_ST_FLAG_PENDING_QUICK_MENU on content load and runloop pushes
 * ACTION_OK_DL_CONTENT_SETTINGS for them.  We render our own view
 * stack instead, so we read the underlying condition (CORE_RUNNING)
 * and consume the upstream flag so it can't queue a displaylist push
 * that we'd ignore. */
static void downplay_sync_ingame(downplay_handle_t *dp)
{
   struct menu_state *menu_st     = menu_state_get_ptr();
   runloop_state_t   *runloop_st  = runloop_state_get_ptr();
   /* CMD_EVENT_UNLOAD_CORE clears RUNLOOP_FLAG_CORE_RUNNING but then
    * starts a dummy core asynchronously, which sets the flag again on
    * the next frame.  Without this, "Quit" from INGAME would leave us
    * stuck on the INGAME view forever — running stays true (dummy is
    * "running"), so the !running transition below never fires.  Treat
    * the dummy as "no game" instead. */
   bool               running     =
         (runloop_get_flags() & RUNLOOP_FLAG_CORE_RUNNING) != 0
         && runloop_st->current_core_type != CORE_TYPE_DUMMY;

   /* Unconditional: upstream may set this flag at content load even
    * before our frame fires, so we eat it every tick rather than only
    * on the running→ingame transition. */
   if (menu_st)
      menu_st->flags &= ~MENU_ST_FLAG_PENDING_QUICK_MENU;

   /* SAVE_PICKER, SETTINGS, CONFIRM are all valid in-game-rooted
    * views (already INGAME-or-deeper) — don't push another INGAME on
    * top of them.  Each tears itself down via its own pop path. */
   if (running
         && dp->nav.view != DOWNPLAY_VIEW_INGAME
         && dp->nav.view != DOWNPLAY_VIEW_SAVE_PICKER
         && dp->nav.view != DOWNPLAY_VIEW_SETTINGS
         && dp->nav.view != DOWNPLAY_VIEW_CONFIRM)
   {
      /* Refresh action composition before push so the recompute
       * inside dp_nav_sync_top reads the right count. */
      downplay_refresh_ingame_actions(dp);
      dp_nav_push(&dp->nav, DOWNPLAY_VIEW_INGAME, NULL,
            downplay_ingame_dispose);
   }
   else if (!running && dp->nav.view == DOWNPLAY_VIEW_INGAME)
   {
      /* Pop returns us to whichever view was underneath INGAME (TOP
       * if the game was launched from there, RECENTS if from a
       * Resume row, etc.) — naturally restored by the parent
       * frame.  Resume-row availability is refreshed by the
       * dispose hook so the new TOP frame's row composition is
       * up to date on the next frame. */
      dp_nav_pop(&dp->nav);
   }
   else if (!running
         && dp->nav.view != DOWNPLAY_VIEW_INGAME
         && dp_nav_root_view(&dp->nav) == DOWNPLAY_VIEW_INGAME)
   {
      /* Edge case: core died (crashed, external Quit, etc.) while
       * the user was drilled into a sub-view of INGAME — SAVE_PICKER,
       * in-game Options (SETTINGS), a CONFIRM modal over Options, or
       * a confirm-over-confirm chain.  All of these are rooted in
       * INGAME (dp_nav_root_view returns INGAME), so collapse the
       * entire stack back to TOP in one shot.  The dispose hooks on
       * each popped frame handle their own cleanup (texture unload,
       * list free, etc.).  Launcher-rooted SETTINGS / CONFIRM (where
       * dp_nav_root_view returns SETTINGS / CONFIRM, never INGAME)
       * legitimately persist across "no core loaded" frames and are
       * not affected. */
      dp_nav_pop_to(&dp->nav, DOWNPLAY_VIEW_TOP);
   }
}

/* What the frame should render.  Setup activity (cores + content
 * buckets) suppresses the normal menu — we want a single, deliberate
 * "let's get you set up" screen from the very first frame instead of a
 * flash of empty list before the splash takes over. */
enum downplay_render_mode
{
   DOWNPLAY_RENDER_LIST = 0,   /* normal menu */
   DOWNPLAY_RENDER_WELCOME,    /* PLANNED — waiting for A to start */
   DOWNPLAY_RENDER_SPLASH      /* setup pass in flight */
};

static enum downplay_render_mode downplay_get_render_mode(void)
{
   switch (downplay_setup_get_phase())
   {
      case DOWNPLAY_SETUP_PLANNED: return DOWNPLAY_RENDER_WELCOME;
      case DOWNPLAY_SETUP_RUNNING: return DOWNPLAY_RENDER_SPLASH;
      default:                     return DOWNPLAY_RENDER_LIST;
   }
}

/* Called every frame after downplay_setup_pump.  When the setup module
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

/* Asymptotic progress curve: each frame, displayed lerps toward target.
 * target is the *next* segment boundary biased by ASYMPTOTE — the bar
 * settles at ~85% of a segment then snaps when the underlying task
 * actually completes (which advances the segment cursor and bumps target
 * by 1/total).  Within a segment, the lerp's (target - displayed) factor
 * decays so the bar visibly slows down — the "Zeno" feel — without ever
 * reaching the segment edge.  k controls how snappy: 3.0 feels alive
 * without being jittery. */
#define DOWNPLAY_SETUP_BAR_K        2.0f
#define DOWNPLAY_SETUP_BAR_ASYMPTOTE 0.85f

static void downplay_setup_anim_step(downplay_handle_t *dp,
      size_t done, size_t total)
{
   retro_time_t now = cpu_features_get_time_usec();
   float        dt;
   float        target;

   if (!dp->setup_anim.was_running)
   {
      dp->setup_anim.displayed   = 0.0f;
      dp->setup_anim.last_us     = now;
      dp->setup_anim.was_running = true;
   }
   if (dp->setup_anim.last_us == 0)
      dp->setup_anim.last_us = now;
   dt = (float)((double)(now - dp->setup_anim.last_us) / 1000000.0);
   dp->setup_anim.last_us = now;
   /* Clamp dt to keep a hitched frame from yanking the bar forward. */
   if (dt < 0.0f)      dt = 0.0f;
   if (dt > 0.1f)      dt = 0.1f;

   if (total == 0)
      total = 1;

   if (downplay_setup_get_phase() == DOWNPLAY_SETUP_DONE)
      target = 1.0f;
   else
      target = ((float)done + DOWNPLAY_SETUP_BAR_ASYMPTOTE)
             / (float)total;
   if (target > 1.0f) target = 1.0f;

   /* Lerp toward target.  Never let displayed move backward — segment
    * skips (e.g. a bucket that turned out to be already populated and
    * was filtered before begin) shouldn't snap the bar back. */
   if (target > dp->setup_anim.displayed)
      dp->setup_anim.displayed +=
            (target - dp->setup_anim.displayed) * dt
            * DOWNPLAY_SETUP_BAR_K;
   if (dp->setup_anim.displayed > 1.0f)
      dp->setup_anim.displayed = 1.0f;
}

/* Only called in DOWNPLAY_RENDER_WELCOME.  A static "Let's get you set
 * up" screen that gates the actual download until the user presses A.
 * No visible progress, no decisions — just a beat so the dive-in isn't
 * jarring. */
static void downplay_draw_welcome_view(const downplay_handle_t *dp)
{
   const downplay_layout_t *L      = &dp->layout;
   size_t                   cores  = downplay_setup_planned_core_count();
   size_t                   bucks  = downplay_setup_planned_bucket_count();
   char                     subline[160];
   float                    cy     = (float)L->vid_h * 0.5f;

   /* Compose the "we'll fetch X cores and Y bundles" line.  Both counts
    * can be zero individually but we never reach this view with both
    * zero (PLANNED implies something to do). */
   if (cores > 0 && bucks > 0)
      snprintf(subline, sizeof(subline),
            "We'll download %u cores and %u content bundles.",
            (unsigned)cores, (unsigned)bucks);
   else if (cores > 0)
      snprintf(subline, sizeof(subline),
            "We'll download %u cores.", (unsigned)cores);
   else
      snprintf(subline, sizeof(subline),
            "We'll download %u content bundles.", (unsigned)bucks);

   downplay_draw_text(dp->font, "Let's get you set up",
         (float)L->vid_w * 0.5f,
         cy - (L->font_size * 0.3f),
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);
   downplay_draw_text(dp->chrome_font, subline,
         (float)L->vid_w * 0.5f,
         cy + L->font_size * 0.5f
               + (L->chrome_font_size * 0.85f) + (12.0f * L->scale),
         L, DP_TEXT_MUTED, TEXT_ALIGN_CENTER);
}

/* Only called in DOWNPLAY_RENDER_SPLASH.  Centered title + asymptotic
 * progress bar + optional sub-line (current core ident during the
 * cores phase). */
static void downplay_draw_setup_splash(gfx_display_t *p_disp, void *userdata,
      downplay_handle_t *dp)
{
   const downplay_layout_t *L      = &dp->layout;
   const char              *phase  = NULL;
   const char              *item   = NULL;
   size_t                   done   = 0;
   size_t                   total  = 0;
   int                      bar_w, bar_h, bar_x, bar_y, fill_w;
   float                    cy     = (float)L->vid_h * 0.5f;

   downplay_setup_get_progress(&total, &done, &phase, &item);
   downplay_setup_anim_step(dp, done, total);

   downplay_draw_text(dp->font,
         phase ? phase : "Setting up...",
         (float)L->vid_w * 0.5f,
         cy - (L->font_size * 0.5f) - (12.0f * L->scale),
         L, DP_TEXT_LIGHT, TEXT_ALIGN_CENTER);

   /* Bar geometry: 60% of width, ~10px scaled tall, centered.  Drawn
    * as two flat rects (bg track + fill).  Pill caps would be a nicer
    * primitive but two rects keep this self-contained. */
   bar_w  = (int)((float)L->vid_w * 0.60f);
   bar_h  = (int)(10.0f * L->scale);
   if (bar_h < 4) bar_h = 4;
   bar_x  = ((int)L->vid_w - bar_w) / 2;
   bar_y  = (int)cy + (int)(8.0f * L->scale);
   fill_w = (int)((float)bar_w * dp->setup_anim.displayed);

   downplay_draw_rect(p_disp, userdata, L,
         bar_x, bar_y, bar_w, bar_h, DP_COLOR_PILL_DARK);
   downplay_draw_rect(p_disp, userdata, L,
         bar_x, bar_y, fill_w, bar_h, DP_COLOR_PILL_LIGHT);

   if (item && *item)
      downplay_draw_text(dp->chrome_font, item,
            (float)L->vid_w * 0.5f,
            (float)(bar_y + bar_h) + (L->chrome_font_size * 0.85f)
                  + (8.0f * L->scale),
            L, DP_TEXT_MUTED, TEXT_ALIGN_CENTER);
}

static void downplay_menu_frame(void *data, video_frame_info_t *video_info)
{
   downplay_handle_t       *dp = (downplay_handle_t*)data;
   gfx_display_t           *p_disp;
   void                    *userdata;
   settings_t              *settings;
   video_driver_state_t    *video_st;
   float                    user_scale;
   int                      bottom_y;
   float                   *chrome_bg;
   enum downplay_render_mode mode;

   if (!dp)
      return;

   p_disp     = disp_get_ptr();
   userdata   = video_info->userdata;
   settings   = config_get_ptr();
   video_st   = video_state_get_ptr();
   user_scale = (settings && settings->floats.menu_scale_factor > 0.0f)
                ? settings->floats.menu_scale_factor : 1.0f;

   if (!p_disp)
      return;

   /* Force the video viewport to the full window for the duration of
    * our menu frame.  Without this, the menu inherits the running core's
    * aspect-corrected viewport — so launcher chrome ends up scaled into
    * a corner (or letterboxed) when video_aspect_ratio_idx isn't
    * full/stretched.  Restored at the end of the frame so the next core
    * frame uses the user's chosen aspect again. */
   if (video_st && video_st->current_video
         && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, video_info->width, video_info->height,
            true, false);

   /* Pump the setup state machine on every frame.  Cheap; only does
    * work when the buildbot list has just landed or a bucket task has
    * settled. */
   downplay_setup_pump();
   /* If the pump just transitioned setup to DONE, this fires on the
    * same frame and the splash's "complete" state is never drawn.
    * That's intentional: content-load is async, so the splash dismissing
    * straight into the loading screen is the desired UX. */
   downplay_drive_pending_launch(dp);
   downplay_sync_ingame(dp);
   /* Thumbnail / index pump for the SYSTEM view.  No-op elsewhere; safe
    * to call before layout because it doesn't touch geometry. */
   downplay_drive_system_thumbnails(dp);
   /* Recents view loads thumbnails from the on-disk cache only — no
    * downloads kicked.  No-op outside RECENTS. */
   downplay_drive_recents_thumbnails(dp);

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
      if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
      {
         dp_nav_frame_t           *frame = dp_nav_top(&dp->nav);
         downplay_settings_list_t *cur   = downplay_settings_top(dp);
         downplay_settings_list_t *neu   =
               downplay_build_core_options_list(dp);
         if (neu && frame)
         {
            size_t saved_sel    = cur ? cur->sel    : 0;
            size_t saved_scroll = cur ? cur->scroll : 0;
            if (saved_sel >= neu->row_count && neu->row_count > 0)
               saved_sel = neu->row_count - 1;
            neu->sel    = saved_sel;
            neu->scroll = saved_scroll;
            /* Swap the list inside the existing nav frame.  This
             * intentionally bypasses the SETTINGS dispose hook —
             * the old list is being replaced because the core
             * changed option visibility, not because the user
             * backed out, so any on_close (used by Frontend to
             * commit deferred shader changes) must not fire.  Today
             * the only list reachable here (core options) has no
             * on_close, but the explicit free guards future
             * additions. */
            frame->side = neu;
            downplay_settings_list_free(cur);
            downplay_settings_snap_scroll(dp, neu);
         }
      }
      dp->settings_rebuild_pending = false;
   }
   mode = downplay_get_render_mode();
   /* When we leave splash, drop the anim flag so the next setup pass
    * (lazy install) starts fresh at displayed=0 instead of carrying
    * the previous run's full bar. */
   if (mode != DOWNPLAY_RENDER_SPLASH)
      dp->setup_anim.was_running = false;

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

   /* Refresh status-pill text once per frame, before anything reads it
    * (draw_list's top-row width budget, the INGAME title pill's right
    * limit, and the pill draw call below).  RA throttles the underlying
    * powerstate + timedate work internally, so calling every frame is
    * cheap. */
   downplay_build_status_text(dp->status_text, sizeof(dp->status_text));

   /* Background — opaque normally, dim-overlay over the running game so
    * INGAME reads as a HUD instead of hiding the frame underneath. */
   downplay_draw_rect(p_disp, userdata, &dp->layout,
         0, 0, (int)dp->layout.vid_w, (int)dp->layout.vid_h,
         dp->nav.view == DOWNPLAY_VIEW_INGAME ? DP_COLOR_BG_INGAME : DP_COLOR_BG);

   /* Chrome bg: gray in launcher (would vanish against pure-black
    * DP_COLOR_BG), dark in INGAME (sits on dimmed game). */
   chrome_bg = (dp->nav.view == DOWNPLAY_VIEW_INGAME)
             ? DP_COLOR_PILL_DARK : DP_COLOR_PILL_CHROME_GRAY;

   /* Top-right status pill */
   downplay_draw_status_pill(p_disp, userdata, dp->chrome_font,
         dp->chrome_font_centre_offset, &dp->layout, dp->pill_cap_tex,
         chrome_bg, dp->status_text);

   if (dp->nav.view == DOWNPLAY_VIEW_INGAME)
   {
      /* Title may grow up to the left edge of the status pill, with
       * one margin's worth of gap between them so they read as
       * separate elements.  status_pill_width must be called with
       * the same font that draw_status_pill above renders with —
       * mismatch would silently misalign the gap. */
      int status_w = downplay_status_pill_width(dp->chrome_font,
            &dp->layout, dp->status_text);
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
      case DOWNPLAY_RENDER_WELCOME:
         downplay_draw_welcome_view(dp);
         break;
      case DOWNPLAY_RENDER_LIST:
         if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
            downplay_draw_settings_view(p_disp, userdata, dp);
         else if (dp->nav.view == DOWNPLAY_VIEW_CONFIRM)
            downplay_draw_confirm_view(userdata, dp);
         else
            downplay_draw_list(p_disp, userdata,
                  &dp->layout, dp->pill_cap_tex, dp);
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
   if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
      goto restore_viewport;
   bottom_y = (int)dp->layout.vid_h - dp->layout.margin_y - dp->layout.row_h;

   /* CONFIRM view: just the modal-specific buttons, no POWER hint —
    * the modal is meant to read as focused, single-decision UX. */
   if (dp->nav.view == DOWNPLAY_VIEW_CONFIRM)
   {
      const dp_nav_frame_t    *top  = (dp->nav.nav_depth > 0)
                                    ? &dp->nav.nav[dp->nav.nav_depth - 1] : NULL;
      const dp_confirm_side_t *side = (top
               && top->view == DOWNPLAY_VIEW_CONFIRM)
            ? (const dp_confirm_side_t*)top->side : NULL;
      downplay_hint_t right[2];
      size_t          n = 0;
      int             x = (int)dp->layout.vid_w - dp->layout.margin_x;
      if (!side)
         goto restore_viewport;
      if (*side->b_label)
      {
         right[n].glyph = "B";
         right[n].label = side->b_label;
         n++;
      }
      right[n].glyph = "A";
      right[n].label = side->a_label;
      n++;
      downplay_draw_footer_hints(p_disp, userdata, dp->chrome_font,
            dp->chrome_font_centre_offset, &dp->layout,
            dp->pill_cap_tex, x, bottom_y,
            DOWNPLAY_ANCHOR_RIGHT, right, n, chrome_bg);
      goto restore_viewport;
   }

   {
      downplay_hint_t left[1];
      left[0].glyph = "POWER";
      left[0].label = "SLEEP";
      downplay_draw_footer_hints(p_disp, userdata, dp->chrome_font,
            dp->chrome_font_centre_offset, &dp->layout,
            dp->pill_cap_tex, dp->layout.margin_x, bottom_y,
            DOWNPLAY_ANCHOR_LEFT, left, 1, chrome_bg);
   }

   /* Right-aligned hint depends on mode.  When the current view
    * supports going back, a B BACK pair shares the outer pill with
    * the primary hint. */
   {
      downplay_hint_t right[2];
      size_t          n          = 0;
      bool            show_back  = (mode == DOWNPLAY_RENDER_LIST
                                   && dp->nav.view != DOWNPLAY_VIEW_TOP);
      int             x          = (int)dp->layout.vid_w - dp->layout.margin_x;

      /* B BACK first so it sits left of A OPEN inside the pill. */
      if (show_back)
      {
         right[n].glyph = "B";
         right[n].label = "BACK";
         n++;
      }
      right[n].glyph = "A";
      if (mode == DOWNPLAY_RENDER_SPLASH)
      {
         right[n].glyph = "B";
         right[n].label = "CANCEL";
      }
      else if (mode == DOWNPLAY_RENDER_WELCOME)
         right[n].label = "START";
      else
         right[n].label = "OPEN";
      n++;

      downplay_draw_footer_hints(p_disp, userdata, dp->chrome_font,
            dp->chrome_font_centre_offset, &dp->layout,
            dp->pill_cap_tex, x, bottom_y,
            DOWNPLAY_ANCHOR_RIGHT, right, n, chrome_bg);
   }

restore_viewport:
   /* Restore the running core's aspect-corrected viewport so the next
    * core frame draws where the user expects (matches XMB / Ozone).
    * Reached via fall-through or via the SETTINGS / CONFIRM gotos
    * above — every path that took the force_full set_viewport at the
    * top of the frame must come through here. */
   if (video_st && video_st->current_video
         && video_st->current_video->set_viewport)
      video_st->current_video->set_viewport(
            video_st->data, video_info->width, video_info->height,
            false, true);
}

/* ---------- input ---------- */

/* M2: own the navigation entirely.  We don't have a backing menu list, so
 * generic_menu_entry_action() (which assumes file_list_t-driven entries)
 * isn't useful — we mutate dp->nav.selection directly and consume the action.
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
   {
      enum downplay_render_mode m = downplay_get_render_mode();
      if (m == DOWNPLAY_RENDER_WELCOME)
      {
         if (action == MENU_ACTION_OK || action == MENU_ACTION_SELECT)
            downplay_setup_start();
         /* No CANCEL path — once we've decided setup is needed, the
          * user shouldn't be able to dismiss it.  POWER still works. */
         return 0;
      }
      if (m == DOWNPLAY_RENDER_SPLASH)
      {
         if (action == MENU_ACTION_CANCEL)
         {
            /* Drop any pending lazy launch so a cancelled lazy install
             * returns to the menu instead of auto-launching when the
             * in-flight task completes. */
            downplay_clear_pending_launch(dp);
            downplay_setup_cancel();
         }
         return 0;
      }
   }

   /* CONFIRM modal: A fires the optional callback and returns to
    * prior_view; B always dismisses without firing.  For
    * acknowledgement-only screens the B button isn't drawn, but we
    * still honour Cancel as a way out — pressing it on an ack screen
    * just dismisses, which is what the user expects. */
   if (dp->nav.view == DOWNPLAY_VIEW_CONFIRM)
   {
      if (action == MENU_ACTION_OK || action == MENU_ACTION_SELECT)
         downplay_close_confirm(dp, true);
      else if (action == MENU_ACTION_CANCEL)
         downplay_close_confirm(dp, false);
      return 0;
   }

   /* SETTINGS view drives its own selection cursor on the active
    * stack frame, not dp->nav.selection — handled before the generic
    * up/down code below to avoid two cursors fighting. */
   if (dp->nav.view == DOWNPLAY_VIEW_SETTINGS)
   {
      downplay_settings_list_t *S = downplay_settings_top(dp);
      if (!S || S->row_count == 0)
      {
         if (action == MENU_ACTION_CANCEL)
            downplay_settings_pop(dp);
         else if (action == MENU_ACTION_START)
            downplay_settings_pop_all(dp);
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
         case MENU_ACTION_START:
            /* Toggle close: mirrors Start-to-open from the launcher,
             * and from deeper stacks (in-game Options) collapses the
             * whole stack so the user exits Settings entirely instead
             * of having to back out one level at a time. */
            downplay_settings_pop_all(dp);
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
         dp_nav_set_selection(&dp->nav,
               (dp->nav.selection + dp->total_rows - 1) % dp->total_rows);
         return 0;
      case MENU_ACTION_DOWN:
         dp_nav_set_selection(&dp->nav,
               (dp->nav.selection + 1) % dp->total_rows);
         return 0;
      case MENU_ACTION_OK:
      case MENU_ACTION_SELECT:
         if (dp->nav.view == DOWNPLAY_VIEW_INGAME)
         {
            enum downplay_ingame_action act;
            if (dp->nav.selection >= dp->ingame_action_count)
               return 0;
            act = dp->ingame_actions[dp->nav.selection];
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
                  /* Pre-populate the picker buffer before push so
                   * the recompute inside dp_nav_sync_top reads the
                   * right row count.  save_picker[] stays on dp —
                   * only one picker exists at a time. */
                  downplay_save_picker_free(dp);
                  downplay_savestate_enumerate(dp->save_picker,
                        &dp->save_picker_count, true);
                  dp_nav_push(&dp->nav, DOWNPLAY_VIEW_SAVE_PICKER, NULL,
                        downplay_save_picker_dispose);
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
         if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER)
         {
            if (dp->nav.selection < dp->save_picker_count)
            {
               int slot = dp->save_picker[dp->nav.selection].slot;
               downplay_load_from_slot(slot);
               /* Pop back to INGAME — dispose hook frees thumbnails
                * and refreshes the action composition.
                * CMD_EVENT_LOAD_STATE doesn't unload the core, so
                * the user resumes play directly. */
               dp_nav_pop(&dp->nav);
               command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            }
            return 0;
         }
         if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM)
         {
            const downplay_system_t *sys = &dp->systems[dp->active_system];
            const downplay_rom_t    *rom = &dp->roms[dp->nav.selection];
            downplay_launch_rom(dp, sys->core_ident, rom->full_path);
            return 0;
         }
         if (dp->nav.view == DOWNPLAY_VIEW_RECENTS)
         {
            if (dp->recents && dp->nav.selection < dp->recent_row_count)
               downplay_launch_recent(dp->recents[dp->nav.selection].pl_idx);
            return 0;
         }
         /* TOP view: Resume → recents-launch the head; Recents → open;
          * else → drill into a system.  Order mirrors row layout in
          * downplay_row_label. */
         {
            size_t sel = dp->nav.selection;
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
         if (dp->nav.view == DOWNPLAY_VIEW_INGAME)
         {
            command_event(CMD_EVENT_MENU_TOGGLE, NULL);
            return 0;
         }
         if (dp->nav.view == DOWNPLAY_VIEW_SAVE_PICKER)
         {
            /* Back to INGAME — dispose hook frees thumbnails and
             * refreshes action composition; parent INGAME frame's
             * selection is preserved underneath. */
            dp_nav_pop(&dp->nav);
            return 0;
         }
         if (dp->nav.view == DOWNPLAY_VIEW_SYSTEM)
         {
            downplay_close_system(dp);
            return 0;
         }
         if (dp->nav.view == DOWNPLAY_VIEW_RECENTS)
         {
            downplay_close_recents(dp);
            return 0;
         }
         /* TOP view: nothing to back out to.  Stay put. */
         return 0;
      case MENU_ACTION_START:
         /* Launcher-only shortcut into the global Settings list.
          * Other views ignore Start (RA's default for it is "reset
          * to default" on whichever entry has focus, which we don't
          * want bleeding into our list-driven views). */
         if (dp->nav.view == DOWNPLAY_VIEW_TOP)
            downplay_settings_push(dp,
                  downplay_build_global_settings_list(dp));
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

   /* Seed the nav stack with TOP at the ground and wire the
    * after-change callback so every push/pop refreshes total_rows
    * for the new view.  All writes go through nav helpers from here
    * on so the cached dp->nav.view + dp->nav.selection can't drift. */
   dp_nav_init(&dp->nav, DOWNPLAY_VIEW_TOP,
         downplay_after_nav_change, dp);
   dp->thumb_prev_selection_sys = SIZE_MAX;
   dp->thumb_prev_selection_rec = SIZE_MAX;
   dp->roms_dims_warmed         = false;
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
         downplay_setup_plan_boot(idents, dp->system_count);
         free(idents);
      }
   }

   /* Boot-time thumbnail-index prefetch (PLAN — first-image-load
    * optimization).  Fan out async fetches for every installed
    * system's index.json so that by the time the user navigates
    * into a system view, downplay_thumbs_open finds a fresh
    * on-disk index and skips its own HTTP fetch. */
   if (dp->system_count > 0)
   {
      const char **systems = (const char**)malloc(
            dp->system_count * sizeof(*systems));
      if (systems)
      {
         size_t i, n = 0;
         for (i = 0; i < dp->system_count; i++)
         {
            const char *db = dp->systems[i].db_name;
            if (db && *db)
               systems[n++] = db;
         }
         if (n > 0)
            downplay_thumbs_prefetch_indexes(systems, n);
         free(systems);
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
   /* Right-pane fade key holds a uintptr_t the GPU may recycle on the
    * upcoming context_reset.  Clearing it here guarantees the next
    * texture restarts the smoothstep from alpha 0 — the dispose hooks
    * cover the in-stack cases, but context_destroy fires regardless of
    * whether SYSTEM/SAVE_PICKER are currently on the nav stack. */
   dp->preview_last_tex = 0;
   /* Pop everything down to the ground TOP frame.  Settings frames
    * hold lists whose row data may include pointers into the core
    * options manager (RA-owned, possibly torn down before us); a
    * CONFIRM frame on top would leak its heap-allocated side payload
    * if pop_all stopped at it.  pop_to(TOP) catches everything. */
   dp_nav_pop_to(&dp->nav, DOWNPLAY_VIEW_TOP);
   gfx_display_deinit_white_texture();
}

static void downplay_menu_free(void *data)
{
   downplay_handle_t *dp = (downplay_handle_t*)data;
   if (!dp)
      return;
   downplay_menu_context_destroy(dp);
   /* Thumbs are normally closed by system_dispose / recents_dispose on
    * pop_to_TOP, which context_destroy triggered above; these branches
    * only fire if the destroy path skipped them (defensive). */
   if (dp->current_thumbs)
   {
      downplay_thumbs_close(dp->current_thumbs);
      dp->current_thumbs = NULL;
   }
   if (dp->recents_thumbs)
   {
      downplay_thumbs_recents_close(dp->recents_thumbs);
      dp->recents_thumbs = NULL;
   }
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
