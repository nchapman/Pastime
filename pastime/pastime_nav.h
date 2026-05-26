/*  Pastime - a fork of RetroArch.
 *  Copyright (C) 2026 - Pastime contributors.
 *
 *  Pastime is free software: you can redistribute it and/or modify it under
 *  the terms of the GNU General Public License as published by the Free
 *  Software Foundation, either version 3 of the License, or (at your option)
 *  any later version.
 *
 *  Pastime is distributed in the hope that it will be useful, but WITHOUT ANY
 *  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 *  FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 *  details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with Pastime. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef PASTIME_NAV_H
#define PASTIME_NAV_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The set of view-identity tags carried by every nav frame.  Used
 * by the renderer's per-frame dispatch (TOP / SYSTEM / RECENTS go
 * through pastime_draw_list; SETTINGS / CONFIRM have dedicated
 * draw functions) and by input handlers that branch on view. */
enum pastime_view
{
   PASTIME_VIEW_TOP = 0,    /* recents header + system list */
   PASTIME_VIEW_SYSTEM,     /* drilled into one system; showing its ROMs */
   PASTIME_VIEW_RECENTS,    /* drilled into recents history */
   PASTIME_VIEW_COLLECTIONS,/* list of collection names (drill from TOP) */
   PASTIME_VIEW_COLLECTION, /* ROM list from one collection .txt */
   PASTIME_VIEW_INGAME,     /* core running; show Continue/Save/Load/Quit overlay */
   PASTIME_VIEW_SAVE_PICKER,/* drilled into save-state list */
   PASTIME_VIEW_SETTINGS,   /* settings-style list (Options → core opts) */
   PASTIME_VIEW_CONFIRM     /* modal confirm/ack screen */
};

/* Per-frame dispose hook.  Runs on pop.  Receives the host's user
 * pointer (the one passed to dp_nav_init) and the frame's side
 * data.  Owns lifetime of `side` — typically frees it and may also
 * fire cross-view side effects (e.g. SAVE_PICKER's dispose unloads
 * thumbnail textures and refreshes the parent INGAME view's action
 * composition since manual save count may have changed). */
typedef void (*dp_nav_dispose_fn)(void *user, void *side);

/* Optional callback fired after every push or pop completes.  The
 * host's derived state (e.g. row count for the active view) typically
 * depends on view identity and needs refreshing on every transition.
 * Receives the host's user pointer. */
typedef void (*dp_nav_after_change_fn)(void *user);

typedef struct
{
   enum pastime_view view;
   size_t             selection;
   void              *side;     /* nullable; per-frame side data */
   dp_nav_dispose_fn  dispose;  /* nullable; runs on pop */
} dp_nav_frame_t;

/* Cap of 8 covers the deepest realistic stack today (TOP → INGAME →
 * SETTINGS root → settings sublist → CONFIRM-over-confirm-chain ≈
 * depth 6).  Bumps cheap if a future flow needs it. */
#define DP_NAV_STACK_MAX 8

typedef struct
{
   dp_nav_frame_t          nav[DP_NAV_STACK_MAX];
   size_t                  nav_depth;
   /* Cached mirrors of nav[nav_depth - 1].view + .selection.  Read
    * sites use these directly so callers don't need to deference
    * the top frame on every access; the helpers below are the only
    * writers, so the cache can't drift. */
   enum pastime_view      view;
   size_t                  selection;
   /* Optional after-change hook + opaque user pointer passed to it
    * and to every dispose hook. */
   dp_nav_after_change_fn  after_change;
   void                   *user;
} dp_nav_state_t;

/* Initialize an empty stack with `ground` at depth 1 (the seed
 * frame, never popped).  `user` is captured and forwarded to every
 * after_change call and every dispose hook.  Both callbacks may be
 * NULL. */
void dp_nav_init(dp_nav_state_t *s, enum pastime_view ground,
      dp_nav_after_change_fn after_change, void *user);

/* Push a frame.  Selection starts at 0; callers that want a
 * different initial selection use dp_nav_set_selection right after.
 * Drops the push (and immediately disposes the orphan `side`) if
 * the stack is full. */
void dp_nav_push(dp_nav_state_t *s, enum pastime_view view,
      void *side, dp_nav_dispose_fn dispose);

/* Pop the top frame.  No-op at depth 1 (the seed frame is the
 * ground).  Ordering: the cached view + selection are updated to
 * the new top *before* dispose runs (so dispose sees the parent
 * frame in s->view), then after_change fires *after* dispose (so
 * any state mutated by dispose is reflected in the recompute). */
void dp_nav_pop(dp_nav_state_t *s);

/* Pop frames until the top matches `view`.  If `view` is not on
 * the stack the loop strips down to the ground frame instead, and
 * warns so a caller passing an absent target doesn't silently
 * flatten the stack. */
void dp_nav_pop_to(dp_nav_state_t *s, enum pastime_view view);

/* Top frame, or NULL if the stack is empty. */
dp_nav_frame_t *dp_nav_top(dp_nav_state_t *s);

/* The deepest non-ground view — i.e. "what's the user actually
 * doing here?".  Used by sync_ingame's teardown predicate to
 * distinguish in-game-rooted views (where the user came from
 * INGAME and we want to snap to TOP if the core dies) from
 * launcher-rooted ones.  Returns the ground view if only the seed
 * frame is on the stack. */
enum pastime_view dp_nav_root_view(const dp_nav_state_t *s);

/* Update the cached selection AND the top frame's selection
 * together.  Use this for any cursor move (UP/DOWN cycling, in-place
 * clamps from row-count changes) so a future push/pop preserves the
 * intended cursor. */
void dp_nav_set_selection(dp_nav_state_t *s, size_t sel);

/* ---------- list navigation helpers (pure) ----------
 *
 * These are stateless math used by the entry-action handler to
 * implement long-list conveniences (page jumps, alphabet jumps,
 * top/bottom).  Kept here (rather than inline in the menu driver)
 * so they can be unit-tested without dragging in any RA state. */

/* Page jump: shift `cur` by `page` rows in direction `dir`
 * (+1 = down, -1 = up), clamped to [0, total-1].  Does NOT wrap —
 * fast-scrolling past either edge should stop, not jump to the
 * opposite end (matches LessUI; wrap on single-row UP/DOWN remains
 * the caller's choice). `page` of 0 is treated as 1. `total` of 0
 * returns 0. */
size_t dp_nav_page_jump(size_t cur, size_t total, size_t page, int dir);

/* Letter-bucket key for `label`.  Skips leading non-alphanumerics
 * and articles ("The ", "A ", "An "), then returns an uppercase
 * ASCII letter ('A'..'Z') if the next char is alpha, '#' if it's a
 * digit, or '\0' for an empty/all-skipped label.  NULL → '\0'.
 * Pure ASCII fold — non-ASCII bytes are passed through as-is so
 * the comparison still partitions consistently within a language. */
char dp_nav_first_letter_bucket(const char *label);

/* Label accessor for letter-jump: row index → display label.
 * Returning NULL is fine and treated as an empty label. */
typedef const char *(*dp_nav_label_fn)(void *user, size_t row);

/* Letter jump: scan rows for the next/prev bucket boundary.
 *
 *   dir > 0 → first row whose bucket differs from cur's bucket
 *             when walking forward; if there is none, returns
 *             total-1 (snap to end so the gesture isn't dead).
 *   dir < 0 → the FIRST row of the previous bucket (i.e. walk back
 *             to a different bucket, then walk back further to its
 *             first row).  If cur is not already at the first row
 *             of its own bucket, the first back-press snaps to that
 *             instead (matches the common "back to top of section,
 *             then to previous section" feel).  If no earlier
 *             bucket exists, returns 0.
 *
 * total of 0 returns 0.  label may be NULL (treated as empty). */
size_t dp_nav_letter_jump(size_t cur, size_t total, int dir,
      dp_nav_label_fn label, void *user);

/* ---------- TOP view row dispatch (pure) ---------- */

/* Identifies which logical section a TOP view row belongs to. */
enum dp_top_section
{
   DP_TOP_RECENTS = 0,
   DP_TOP_COLLECTIONS,
   DP_TOP_SYSTEM
};

typedef struct
{
   enum dp_top_section section;
   size_t              index;
} dp_top_row_t;

/* Map a TOP view row index to its logical section + within-section index.
 *
 * Row order: [Recents(0..1)] + [Collections(col_rows)] + [Systems].
 *   - has_recents: true if the Recents row is present (row 0)
 *   - collection_count: number of .txt collection files
 *   - system_count: number of system folders
 *
 * When system_count > 0, collections is one aggregate row (col_rows=1).
 * When system_count == 0, each collection gets its own row (promotion).
 *
 * Returns DP_TOP_SYSTEM with index=0 when row is out of range. */
dp_top_row_t dp_nav_top_row_dispatch(size_t row, bool has_recents,
      size_t collection_count, size_t system_count);

/* Total row count for the TOP view given the same parameters. */
size_t dp_nav_top_row_count(bool has_recents,
      size_t collection_count, size_t system_count);

RETRO_END_DECLS

#endif
