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

#include <string.h>

#include "pastime_nav.h"

/* Production builds pull in RA's full verbosity macros via
 * verbosity.h.  The standalone unit-test build at pastime/tests/
 * defines PASTIME_NAV_TEST_BUILD and supplies its own
 * dp_nav_test_warn so we don't have to drag verbosity.c and its
 * transitive deps into the unit-test link. */
#ifdef PASTIME_NAV_TEST_BUILD
extern void dp_nav_test_warn(const char *fmt, ...);
#define RARCH_WARN(...) dp_nav_test_warn(__VA_ARGS__)
#else
#include "../verbosity.h"
#endif

/* Sync the cached view + selection from the top frame and fire the
 * after_change hook.  Internal — every push/pop ends in this so the
 * cache can never drift. */
static void dp_nav_sync_top(dp_nav_state_t *s)
{
   const dp_nav_frame_t *top = &s->nav[s->nav_depth - 1];
   s->view      = top->view;
   s->selection = top->selection;
   if (s->after_change)
      s->after_change(s->user);
}

void dp_nav_init(dp_nav_state_t *s, enum pastime_view ground,
      dp_nav_after_change_fn after_change, void *user)
{
   memset(s, 0, sizeof(*s));
   s->nav[0].view      = ground;
   s->nav[0].selection = 0;
   s->nav[0].side      = NULL;
   s->nav[0].dispose   = NULL;
   s->nav_depth        = 1;
   s->view             = ground;
   s->selection        = 0;
   s->after_change     = after_change;
   s->user             = user;
   /* Don't fire after_change here — at init time the caller is
    * still wiring up state (e.g. row sources aren't ready yet);
    * recompute callbacks would dereference NULL. */
}

void dp_nav_push(dp_nav_state_t *s, enum pastime_view view,
      void *side, dp_nav_dispose_fn dispose)
{
   dp_nav_frame_t *f;
   if (s->nav_depth >= DP_NAV_STACK_MAX)
   {
      RARCH_WARN("[Pastime] nav stack full; dropping push (view=%d)\n",
            (int)view);
      if (dispose)
         dispose(s->user, side);
      return;
   }
   f            = &s->nav[s->nav_depth++];
   f->view      = view;
   f->selection = 0;
   f->side      = side;
   f->dispose   = dispose;
   dp_nav_sync_top(s);
}

void dp_nav_pop(dp_nav_state_t *s)
{
   dp_nav_frame_t *f;
   if (s->nav_depth <= 1)
      return;
   f             = &s->nav[--s->nav_depth];
   /* Critical ordering: sync the cache to the new top *before*
    * dispose runs.  Dispose hooks may call host helpers that gate
    * on s->view (e.g. the SAVE_PICKER dispose calls
    * pastime_refresh_ingame_actions, which clamps selection only
    * when view == INGAME).  If sync ran after dispose, s->view
    * would still cache the popped view and the gate would silently
    * no-op.  after_change fires *after* dispose so any state
    * mutation done by dispose (e.g. ingame_action_count change) is
    * reflected in the row recompute. */
   s->view      = s->nav[s->nav_depth - 1].view;
   s->selection = s->nav[s->nav_depth - 1].selection;
   if (f->dispose)
      f->dispose(s->user, f->side);
   f->side    = NULL;
   f->dispose = NULL;
   if (s->after_change)
      s->after_change(s->user);
}

void dp_nav_pop_to(dp_nav_state_t *s, enum pastime_view view)
{
   while (s->nav_depth > 1
         && s->nav[s->nav_depth - 1].view != view)
      dp_nav_pop(s);
   if (s->nav_depth > 0
         && s->nav[s->nav_depth - 1].view != view)
      RARCH_WARN("[Pastime] nav pop_to(%d) didn't find target; "
            "stack flattened to ground (view=%d)\n",
            (int)view, (int)s->nav[s->nav_depth - 1].view);
}

dp_nav_frame_t *dp_nav_top(dp_nav_state_t *s)
{
   return s->nav_depth > 0 ? &s->nav[s->nav_depth - 1] : NULL;
}

enum pastime_view dp_nav_root_view(const dp_nav_state_t *s)
{
   return s->nav_depth > 1 ? s->nav[1].view : s->nav[0].view;
}

void dp_nav_set_selection(dp_nav_state_t *s, size_t sel)
{
   s->selection = sel;
   if (s->nav_depth > 0)
      s->nav[s->nav_depth - 1].selection = sel;
}

/* ---------- list navigation helpers ---------- */

size_t dp_nav_page_jump(size_t cur, size_t total, size_t page, int dir)
{
   if (total == 0)
      return 0;
   if (page == 0)
      page = 1;
   if (dir < 0)
   {
      if (cur < page)   /* would underflow size_t arithmetic */
         return 0;
      return cur - page;
   }
   if (cur + page >= total - 1)
      return total - 1;
   return cur + page;
}

/* Match an article prefix at *p (case-insensitive ASCII), where the
 * article is followed by a space.  Returns the byte length to skip
 * (0 if no match).  Articles: "The ", "A ", "An ". */
static size_t dp_nav_article_skip(const char *p)
{
   if (   (p[0] == 'T' || p[0] == 't')
       && (p[1] == 'H' || p[1] == 'h')
       && (p[2] == 'E' || p[2] == 'e')
       &&  p[3] == ' ')
      return 4;
   if (   (p[0] == 'A' || p[0] == 'a')
       && (p[1] == 'N' || p[1] == 'n')
       &&  p[2] == ' ')
      return 3;
   if (   (p[0] == 'A' || p[0] == 'a')
       &&  p[1] == ' ')
      return 2;
   return 0;
}

char dp_nav_first_letter_bucket(const char *label)
{
   const char *p;
   size_t      skip;
   unsigned    c;

   if (!label)
      return '\0';
   p = label;
   /* Skip leading non-alphanumeric (whitespace, punctuation). */
   while (*p)
   {
      c = (unsigned char)*p;
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9'))
         break;
      p++;
   }
   if (!*p)
      return '\0';
   /* Strip a single English article ("The ", "A ", "An ").  One
    * pass — nested articles don't occur in practice. */
   skip = dp_nav_article_skip(p);
   if (skip)
      p += skip;
   c = (unsigned char)*p;
   if (c >= 'a' && c <= 'z')
      return (char)(c - ('a' - 'A'));
   if (c >= 'A' && c <= 'Z')
      return (char)c;
   if (c >= '0' && c <= '9')
      return '#';
   /* Non-ASCII or empty after article skip: bucket as itself (still
    * partitions sortably within a language) — empty falls through. */
   return c ? (char)c : '\0';
}

static char dp_nav_bucket_at(size_t row, dp_nav_label_fn label, void *user)
{
   return dp_nav_first_letter_bucket(label ? label(user, row) : NULL);
}

size_t dp_nav_letter_jump(size_t cur, size_t total, int dir,
      dp_nav_label_fn label, void *user)
{
   char   cur_bucket;
   size_t i;

   if (total == 0)
      return 0;
   if (cur >= total)
      cur = total - 1;
   cur_bucket = dp_nav_bucket_at(cur, label, user);

   if (dir > 0)
   {
      for (i = cur + 1; i < total; i++)
         if (dp_nav_bucket_at(i, label, user) != cur_bucket)
            return i;
      return total - 1;
   }
   /* dir < 0: first, find the first row of cur's bucket.  If cur is
    * not already at that row, snap there (one back-press = "top of
    * current section"). */
   {
      size_t bucket_top = cur;
      while (bucket_top > 0
            && dp_nav_bucket_at(bucket_top - 1, label, user) == cur_bucket)
         bucket_top--;
      if (bucket_top != cur)
         return bucket_top;
      if (bucket_top == 0)
         return 0;
      /* Already at top of cur bucket: walk to the previous bucket's
       * first row. */
      {
         char prev_bucket = dp_nav_bucket_at(bucket_top - 1, label, user);
         size_t prev_top  = bucket_top - 1;
         while (prev_top > 0
               && dp_nav_bucket_at(prev_top - 1, label, user) == prev_bucket)
            prev_top--;
         return prev_top;
      }
   }
}
