/* Unit tests for downplay/downplay_nav.{c,h}.
 *
 * Links against the real production source — there is no copy of
 * the nav helpers here.  See run.sh for the build line; the source
 * file detects DOWNPLAY_NAV_TEST_BUILD and routes RARCH_WARN
 * through dp_nav_test_warn (defined below) so we don't have to
 * drag RA's full verbosity / logger infrastructure into the link.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../downplay_nav.h"

/* ---------- warning capture for the production source ---------- */

static char g_last_warn[256];
static int  g_warn_count;

void dp_nav_test_warn(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(g_last_warn, sizeof(g_last_warn), fmt, ap);
   va_end(ap);
   g_warn_count++;
}

/* ---------- test framework ---------- */

static int g_pass;
static int g_fail;
static int g_test_pass_at_start;
static int g_test_fail_at_start;

#define ASSERT_TRUE(cond) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s\n", \
            __FILE__, __LINE__, #cond); } \
} while (0)

#define ASSERT_EQ(a, b) do { \
   long _va = (long)(a); \
   long _vb = (long)(b); \
   if (_va == _vb) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s == %s  (%ld != %ld)\n", \
            __FILE__, __LINE__, #a, #b, _va, _vb); } \
} while (0)

#define ASSERT_PTR_EQ(a, b) do { \
   const void *_va = (const void*)(a); \
   const void *_vb = (const void*)(b); \
   if (_va == _vb) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s == %s  (%p != %p)\n", \
            __FILE__, __LINE__, #a, #b, _va, _vb); } \
} while (0)

/* Per-test scratch state — captured into the after_change /
 * dispose hooks so tests can assert on what the nav helpers did. */
typedef struct
{
   int                  recompute_calls;
   enum downplay_view   view_at_recompute;
   int                  dispose_calls;
   void                *last_disposed_side;
   enum downplay_view   view_at_dispose;
   /* dp_nav_state_t snapshot at dispose-fire time, so tests can
    * assert on internal ordering (e.g. selection clamp visibility
    * before/after dispose). */
   const dp_nav_state_t *state_ref;
} test_user_t;

static void test_after_change(void *user)
{
   test_user_t *u = (test_user_t*)user;
   u->recompute_calls++;
   if (u->state_ref)
      u->view_at_recompute = u->state_ref->view;
}

static void recording_dispose(void *user, void *side)
{
   test_user_t *u = (test_user_t*)user;
   u->dispose_calls++;
   u->last_disposed_side = side;
   if (u->state_ref)
      u->view_at_dispose = u->state_ref->view;
}

#define RUN_TEST(fn) do { \
   dp_nav_state_t state; \
   test_user_t    user; \
   memset(&user, 0, sizeof(user)); \
   user.state_ref = &state; \
   dp_nav_init(&state, DOWNPLAY_VIEW_TOP, test_after_change, &user); \
   g_warn_count = 0; \
   g_last_warn[0] = '\0'; \
   g_test_pass_at_start = g_pass; \
   g_test_fail_at_start = g_fail; \
   fn(&state, &user); \
   printf("  %-52s  %s  (+%d -%d)\n", #fn, \
         (g_fail == g_test_fail_at_start) ? "ok  " : "FAIL", \
         g_pass - g_test_pass_at_start, \
         g_fail - g_test_fail_at_start); \
} while (0)

/* ============================================================
 * Tests — every test takes (state, user).
 * ============================================================ */

/* ---- init ---- */

static void test_init_seed(dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   ASSERT_EQ(s->nav_depth, 1);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->selection, 0);
   ASSERT_PTR_EQ(dp_nav_top(s), &s->nav[0]);
}

static void test_init_skips_after_change(dp_nav_state_t *s, test_user_t *u)
{
   /* dp_nav_init must NOT fire after_change — at init time the
    * caller may not have wired up the state that recompute reads
    * (row counts etc.).  The first call should come from a real
    * push or pop, not the seed. */
   (void)s;
   ASSERT_EQ(u->recompute_calls, 0);
}

/* ---- push ---- */

static void test_push_increments_depth_and_caches_view(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   ASSERT_EQ(s->nav_depth, 2);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_SYSTEM);
   ASSERT_EQ(dp_nav_top(s)->view, DOWNPLAY_VIEW_SYSTEM);
}

static void test_push_resets_selection_to_zero(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_set_selection(s, 5);
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   ASSERT_EQ(s->selection, 0);
   ASSERT_EQ(dp_nav_top(s)->selection, 0);
}

static void test_push_records_side_and_dispose(
      dp_nav_state_t *s, test_user_t *u)
{
   int marker = 42;
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, &marker, recording_dispose);
   ASSERT_PTR_EQ(dp_nav_top(s)->side, &marker);
   ASSERT_PTR_EQ(dp_nav_top(s)->dispose, recording_dispose);
}

static void test_push_calls_after_change(
      dp_nav_state_t *s, test_user_t *u)
{
   int before = u->recompute_calls;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   ASSERT_EQ(u->recompute_calls, before + 1);
   ASSERT_EQ(u->view_at_recompute, DOWNPLAY_VIEW_SYSTEM);
}

static void test_push_overflow_drops_and_disposes_orphan(
      dp_nav_state_t *s, test_user_t *u)
{
   int marker = 7;
   size_t i;
   /* Fill the stack: depth starts at 1 (TOP); push 7 more = 8. */
   for (i = 0; i < DP_NAV_STACK_MAX - 1; i++)
      dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   ASSERT_EQ(s->nav_depth, DP_NAV_STACK_MAX);

   /* One more push should be dropped. */
   dp_nav_push(s, DOWNPLAY_VIEW_CONFIRM, &marker, recording_dispose);
   ASSERT_EQ(s->nav_depth, DP_NAV_STACK_MAX); /* unchanged */
   ASSERT_EQ(u->dispose_calls, 1);            /* orphan disposed */
   ASSERT_PTR_EQ(u->last_disposed_side, &marker);
   ASSERT_TRUE(g_warn_count >= 1);
}

/* ---- pop ---- */

static void test_pop_at_ground_is_noop(dp_nav_state_t *s, test_user_t *u)
{
   /* Stack starts with just TOP. */
   dp_nav_pop(s);
   ASSERT_EQ(s->nav_depth, 1);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(u->dispose_calls, 0);
}

static void test_pop_basic_round_trip(dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_pop(s);
   ASSERT_EQ(s->nav_depth, 1);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->selection, 0);
}

static void test_pop_fires_dispose_with_correct_args(
      dp_nav_state_t *s, test_user_t *u)
{
   int marker = 99;
   dp_nav_push(s, DOWNPLAY_VIEW_SAVE_PICKER, &marker, recording_dispose);
   dp_nav_pop(s);
   ASSERT_EQ(u->dispose_calls, 1);
   ASSERT_PTR_EQ(u->last_disposed_side, &marker);
}

/* The bug we just fixed: dispose used to run *after* sync, so its
 * s->view read still saw the popped view.  refresh_ingame_actions's
 * `if (view == INGAME) clamp` guard would silently no-op,
 * leaving a stale selection that only the next recompute caught.
 * The fix: sync s->view + s->selection from the new top BEFORE
 * dispose runs.  This test pins that ordering. */
static void test_pop_dispose_sees_new_top_view(
      dp_nav_state_t *s, test_user_t *u)
{
   /* Stack: TOP → INGAME → SAVE_PICKER */
   dp_nav_push(s, DOWNPLAY_VIEW_INGAME, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_SAVE_PICKER, NULL, recording_dispose);

   /* Pop the picker.  Inside dispose, s->view should already be
    * INGAME (the new top), not SAVE_PICKER (the popped one). */
   dp_nav_pop(s);
   ASSERT_EQ(u->view_at_dispose, DOWNPLAY_VIEW_INGAME);
}

/* after_change fires AFTER dispose so any state mutations done by
 * dispose are reflected in the row recompute.  We pin this with a
 * dispose hook that snapshots the after_change call count at the
 * moment dispose fires — the count should still be the pre-pop
 * value. */
static int g_after_change_calls_seen_in_dispose;
static void snapshot_dispose(void *user, void *side)
{
   test_user_t *u = (test_user_t*)user;
   (void)side;
   g_after_change_calls_seen_in_dispose = u->recompute_calls;
}

static void test_pop_after_change_runs_after_dispose(
      dp_nav_state_t *s, test_user_t *u)
{
   int recompute_before;
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, snapshot_dispose);
   recompute_before = u->recompute_calls;
   g_after_change_calls_seen_in_dispose = -1;

   dp_nav_pop(s);

   ASSERT_EQ(g_after_change_calls_seen_in_dispose, recompute_before);
   ASSERT_EQ(u->recompute_calls, recompute_before + 1);
   ASSERT_EQ(u->view_at_recompute, DOWNPLAY_VIEW_TOP);
}

static void test_pop_clears_frame_pointers(
      dp_nav_state_t *s, test_user_t *u)
{
   int marker = 1;
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, &marker, recording_dispose);
   dp_nav_pop(s);
   ASSERT_PTR_EQ(s->nav[1].side, NULL);
   ASSERT_PTR_EQ(s->nav[1].dispose, NULL);
}

static void test_pop_with_null_dispose_does_not_crash(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_pop(s);
   ASSERT_EQ(s->nav_depth, 1);
}

/* ---- pop_to ---- */

static void test_pop_to_existing_target(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_INGAME, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_CONFIRM, NULL, NULL);
   dp_nav_pop_to(s, DOWNPLAY_VIEW_INGAME);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_INGAME);
   ASSERT_EQ(s->nav_depth, 2);
   ASSERT_EQ(g_warn_count, 0);
}

static void test_pop_to_target_already_top_is_noop(
      dp_nav_state_t *s, test_user_t *u)
{
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_pop_to(s, DOWNPLAY_VIEW_SYSTEM);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_SYSTEM);
   ASSERT_EQ(s->nav_depth, 2);
   ASSERT_EQ(u->dispose_calls, 0);
}

static void test_pop_to_top_collapses_full_stack(
      dp_nav_state_t *s, test_user_t *u)
{
   dp_nav_push(s, DOWNPLAY_VIEW_INGAME, NULL, recording_dispose);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, recording_dispose);
   dp_nav_push(s, DOWNPLAY_VIEW_CONFIRM, NULL, recording_dispose);
   dp_nav_pop_to(s, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->nav_depth, 1);
   ASSERT_EQ(u->dispose_calls, 3);
}

static void test_pop_to_absent_target_flattens_and_warns(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   /* INGAME isn't anywhere on the stack. */
   dp_nav_pop_to(s, DOWNPLAY_VIEW_INGAME);
   ASSERT_EQ(s->nav_depth, 1);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(g_warn_count, 1);
}

/* ---- root_view ---- */

static void test_root_view_only_ground_returns_top(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   ASSERT_EQ(dp_nav_root_view(s), DOWNPLAY_VIEW_TOP);
}

static void test_root_view_with_one_frame(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_INGAME, NULL, NULL);
   ASSERT_EQ(dp_nav_root_view(s), DOWNPLAY_VIEW_INGAME);
}

/* root_view answers "what's the user actually doing here?" — it's
 * the deepest non-ground frame.  Used by sync_ingame's teardown
 * predicate to decide whether a CONFIRM-over-SETTINGS-over-INGAME
 * stack is rooted in INGAME (vs. launcher Settings rooted in TOP). */
static void test_root_view_deep_stack(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_INGAME, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   dp_nav_push(s, DOWNPLAY_VIEW_CONFIRM, NULL, NULL);
   ASSERT_EQ(dp_nav_root_view(s), DOWNPLAY_VIEW_INGAME);
}

/* ---- set_selection ---- */

static void test_set_selection_updates_cache_and_frame(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_set_selection(s, 7);
   ASSERT_EQ(s->selection, 7);
   ASSERT_EQ(dp_nav_top(s)->selection, 7);
}

/* Pin that set_selection does NOT fire after_change.  Production
 * relies on this — the after_change callback (recompute_total_rows)
 * itself calls set_selection to clamp the cursor when the row count
 * shrinks; if set_selection fired after_change, that would re-enter
 * the clamp infinitely. */
static void test_set_selection_does_not_fire_after_change(
      dp_nav_state_t *s, test_user_t *u)
{
   int before;
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   before = u->recompute_calls;
   dp_nav_set_selection(s, 3);
   ASSERT_EQ(u->recompute_calls, before);
}

/* ---- selection preservation across push/pop ---- */

/* The headline win of the unified stack: a parent frame's selection
 * is intrinsically preserved while a child is on top, with no
 * "*_pre_view" / "top_selection" slot to manage. */
static void test_pop_restores_parent_selection(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_set_selection(s, 4);
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_set_selection(s, 9);
   dp_nav_pop(s);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->selection, 4);
}

static void test_nested_push_pop_preserves_each_parent(
      dp_nav_state_t *s, test_user_t *u)
{
   (void)u;
   dp_nav_set_selection(s, 2);
   dp_nav_push(s, DOWNPLAY_VIEW_SYSTEM, NULL, NULL);
   dp_nav_set_selection(s, 5);
   dp_nav_push(s, DOWNPLAY_VIEW_SETTINGS, NULL, NULL);
   dp_nav_set_selection(s, 1);

   dp_nav_pop(s);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_SYSTEM);
   ASSERT_EQ(s->selection, 5);

   dp_nav_pop(s);
   ASSERT_EQ(s->view, DOWNPLAY_VIEW_TOP);
   ASSERT_EQ(s->selection, 2);
}

/* ---- top accessor ---- */

static void test_top_returns_correct_frame(
      dp_nav_state_t *s, test_user_t *u)
{
   dp_nav_frame_t *t;
   (void)u;
   dp_nav_push(s, DOWNPLAY_VIEW_RECENTS, NULL, NULL);
   t = dp_nav_top(s);
   ASSERT_TRUE(t != NULL);
   ASSERT_EQ(t->view, DOWNPLAY_VIEW_RECENTS);
   ASSERT_PTR_EQ(t, &s->nav[1]);
}

/* ============================================================
 * Runner
 * ============================================================ */

int main(void)
{
   printf("=== downplay nav stack tests ===\n");

   RUN_TEST(test_init_seed);
   RUN_TEST(test_init_skips_after_change);

   RUN_TEST(test_push_increments_depth_and_caches_view);
   RUN_TEST(test_push_resets_selection_to_zero);
   RUN_TEST(test_push_records_side_and_dispose);
   RUN_TEST(test_push_calls_after_change);
   RUN_TEST(test_push_overflow_drops_and_disposes_orphan);

   RUN_TEST(test_pop_at_ground_is_noop);
   RUN_TEST(test_pop_basic_round_trip);
   RUN_TEST(test_pop_fires_dispose_with_correct_args);
   RUN_TEST(test_pop_dispose_sees_new_top_view);
   RUN_TEST(test_pop_after_change_runs_after_dispose);
   RUN_TEST(test_pop_clears_frame_pointers);
   RUN_TEST(test_pop_with_null_dispose_does_not_crash);

   RUN_TEST(test_pop_to_existing_target);
   RUN_TEST(test_pop_to_target_already_top_is_noop);
   RUN_TEST(test_pop_to_top_collapses_full_stack);
   RUN_TEST(test_pop_to_absent_target_flattens_and_warns);

   RUN_TEST(test_root_view_only_ground_returns_top);
   RUN_TEST(test_root_view_with_one_frame);
   RUN_TEST(test_root_view_deep_stack);

   RUN_TEST(test_set_selection_updates_cache_and_frame);
   RUN_TEST(test_set_selection_does_not_fire_after_change);

   RUN_TEST(test_pop_restores_parent_selection);
   RUN_TEST(test_nested_push_pop_preserves_each_parent);

   RUN_TEST(test_top_returns_correct_frame);

   printf("\n%d passed, %d failed\n", g_pass, g_fail);
   return g_fail > 0 ? 1 : 0;
}
