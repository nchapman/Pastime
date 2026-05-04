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

#ifndef DOWNPLAY_NAV_H
#define DOWNPLAY_NAV_H

#include <stddef.h>
#include <boolean.h>
#include <retro_common_api.h>

RETRO_BEGIN_DECLS

/* The set of view-identity tags carried by every nav frame.  Used
 * by the renderer's per-frame dispatch (TOP / SYSTEM / RECENTS go
 * through downplay_draw_list; SETTINGS / CONFIRM have dedicated
 * draw functions) and by input handlers that branch on view. */
enum downplay_view
{
   DOWNPLAY_VIEW_TOP = 0,    /* recents header + system list */
   DOWNPLAY_VIEW_SYSTEM,     /* drilled into one system; showing its ROMs */
   DOWNPLAY_VIEW_RECENTS,    /* drilled into recents history */
   DOWNPLAY_VIEW_INGAME,     /* core running; show Continue/Save/Load/Quit overlay */
   DOWNPLAY_VIEW_SAVE_PICKER,/* drilled into save-state list */
   DOWNPLAY_VIEW_SETTINGS,   /* settings-style list (Options → core opts) */
   DOWNPLAY_VIEW_CONFIRM     /* modal confirm/ack screen */
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
   enum downplay_view view;
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
   enum downplay_view      view;
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
void dp_nav_init(dp_nav_state_t *s, enum downplay_view ground,
      dp_nav_after_change_fn after_change, void *user);

/* Push a frame.  Selection starts at 0; callers that want a
 * different initial selection use dp_nav_set_selection right after.
 * Drops the push (and immediately disposes the orphan `side`) if
 * the stack is full. */
void dp_nav_push(dp_nav_state_t *s, enum downplay_view view,
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
void dp_nav_pop_to(dp_nav_state_t *s, enum downplay_view view);

/* Top frame, or NULL if the stack is empty. */
dp_nav_frame_t *dp_nav_top(dp_nav_state_t *s);

/* The deepest non-ground view — i.e. "what's the user actually
 * doing here?".  Used by sync_ingame's teardown predicate to
 * distinguish in-game-rooted views (where the user came from
 * INGAME and we want to snap to TOP if the core dies) from
 * launcher-rooted ones.  Returns the ground view if only the seed
 * frame is on the stack. */
enum downplay_view dp_nav_root_view(const dp_nav_state_t *s);

/* Update the cached selection AND the top frame's selection
 * together.  Use this for any cursor move (UP/DOWN cycling, in-place
 * clamps from row-count changes) so a future push/pop preserves the
 * intended cursor. */
void dp_nav_set_selection(dp_nav_state_t *s, size_t sel);

RETRO_END_DECLS

#endif
