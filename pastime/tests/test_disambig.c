/* Unit tests for pastime/pastime_disambig.{c,h}.
 *
 * The disambig module is pure C with no platform dependencies, so the
 * production source is linked in directly — no test-build define
 * required.  Coverage: 2-row intra-source tag-tail collapse, 3-row
 * intra with shared prefix, region-only tag differences, multi-disc
 * mid-block prefix (no false split), cross-source source-label, and
 * the mixed-run case where same-source siblings need the second-pass
 * tag-tail differential.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pastime_disambig.h"

/* ---------- test framework ---------- */

static int g_pass;
static int g_fail;

#define ASSERT_STREQ(actual, expected) do { \
   const char *_a = (actual); \
   const char *_e = (expected); \
   if (_a && _e && strcmp(_a, _e) == 0) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  '%s' == '%s'\n", \
            __FILE__, __LINE__, \
            _a ? _a : "(null)", _e ? _e : "(null)"); } \
} while (0)

#define RUN_TEST(fn) do { \
   int p0 = g_pass, f0 = g_fail; \
   fn(); \
   fprintf(stderr, "  %s  %s  (+%d/+%d)\n", \
         (g_fail == f0) ? "ok  " : "FAIL", \
         #fn, g_pass - p0, g_fail - f0); \
} while (0)

/* ---------- helpers ---------- */

/* Synthetic source list for the resolver callback.  source_idx ->
 * short label.  Tests build whatever array they need before the call. */
typedef struct
{
   const char *labels[8];
   size_t      count;
} fake_sources_t;

static const char *fake_resolve_label(uint8_t source_idx, void *user)
{
   fake_sources_t *fs = (fake_sources_t*)user;
   if (!fs || source_idx >= fs->count)
      return NULL;
   return fs->labels[source_idx];
}

/* Build a row.  display_name and tag are heap-duplicated so the
 * disambig pass can free + replace display_name without touching test
 * static storage.  Caller frees with `free_row`. */
typedef struct
{
   char       *display_name;   /* owned */
   char       *tag;             /* owned, may be NULL */
   uint8_t     source_idx;
} test_row_t;

static void make_row(test_row_t *r,
      const char *display, const char *tag, uint8_t source_idx)
{
   r->display_name = display ? strdup(display) : NULL;
   r->tag          = tag     ? strdup(tag)     : NULL;
   r->source_idx   = source_idx;
}

static void free_row(test_row_t *r)
{
   free(r->display_name);
   free(r->tag);
}

/* Inflate the test_row_t array into the disambig row-ref view. */
static void to_disambig(pastime_disambig_row_t *out, test_row_t *src,
      size_t n)
{
   size_t i;
   for (i = 0; i < n; i++)
   {
      out[i].display_name = &src[i].display_name;
      out[i].tag          = src[i].tag;
      out[i].source_idx   = src[i].source_idx;
   }
}

/* ---------- intra-source ---------- */

static void test_intra_two_rows_tag_diff(void)
{
   /* Ape Escape: bare base + (Demo 1).  The shared "(USA)" should be
    * stripped and the base row should keep its bare label. */
   test_row_t              src[2];
   pastime_disambig_row_t  rows[2];
   make_row(&src[0], "Ape Escape", "(USA)",            0);
   make_row(&src[1], "Ape Escape", "(USA) (Demo 1)",   0);
   to_disambig(rows, src, 2);

   pastime_disambig_run(rows, 2, NULL, NULL);

   ASSERT_STREQ(src[0].display_name, "Ape Escape");
   ASSERT_STREQ(src[1].display_name, "Ape Escape (Demo 1)");
   free_row(&src[0]); free_row(&src[1]);
}

static void test_intra_three_rows_demo_progression(void)
{
   /* Three-row run: base + Demo 1 + Demo 2.  Common (USA) trims off;
    * base stays bare, demos carry their differential. */
   test_row_t              src[3];
   pastime_disambig_row_t  rows[3];
   make_row(&src[0], "Ape Escape", "(USA)",            0);
   make_row(&src[1], "Ape Escape", "(USA) (Demo 1)",   0);
   make_row(&src[2], "Ape Escape", "(USA) (Demo 2)",   0);
   to_disambig(rows, src, 3);

   pastime_disambig_run(rows, 3, NULL, NULL);

   ASSERT_STREQ(src[0].display_name, "Ape Escape");
   ASSERT_STREQ(src[1].display_name, "Ape Escape (Demo 1)");
   ASSERT_STREQ(src[2].display_name, "Ape Escape (Demo 2)");
   free_row(&src[0]); free_row(&src[1]); free_row(&src[2]);
}

static void test_intra_region_only(void)
{
   /* Two regional dumps, no common bracket prefix → both rows show
    * their full tag. */
   test_row_t              src[2];
   pastime_disambig_row_t  rows[2];
   make_row(&src[0], "Ape Escape", "(Europe)", 0);
   make_row(&src[1], "Ape Escape", "(USA)",    0);
   to_disambig(rows, src, 2);

   pastime_disambig_run(rows, 2, NULL, NULL);

   ASSERT_STREQ(src[0].display_name, "Ape Escape (Europe)");
   ASSERT_STREQ(src[1].display_name, "Ape Escape (USA)");
   free_row(&src[0]); free_row(&src[1]);
}

static void test_intra_disc_mid_block_no_split(void)
{
   /* "(Disc 1)" / "(Disc 2)" share "(Disc " char-prefix only — that's
    * mid-block, no ')' to trim back to.  Common prefix length should
    * be 0, both rows show their full tag.  Critically: NO row should
    * end up with a fragment like "(Disc " or "1)". */
   test_row_t              src[3];
   pastime_disambig_row_t  rows[3];
   make_row(&src[0], "Final Fantasy VII", "(USA) (Disc 1)", 0);
   make_row(&src[1], "Final Fantasy VII", "(USA) (Disc 2)", 0);
   make_row(&src[2], "Final Fantasy VII", "(USA) (Disc 3)", 0);
   to_disambig(rows, src, 3);

   pastime_disambig_run(rows, 3, NULL, NULL);

   /* Common prefix trims to "(USA) " → differentials are full disc tags. */
   ASSERT_STREQ(src[0].display_name, "Final Fantasy VII (Disc 1)");
   ASSERT_STREQ(src[1].display_name, "Final Fantasy VII (Disc 2)");
   ASSERT_STREQ(src[2].display_name, "Final Fantasy VII (Disc 3)");
   free_row(&src[0]); free_row(&src[1]); free_row(&src[2]);
}

static void test_intra_one_row_no_tag(void)
{
   /* Run of two where one row has no tag at all.  Common prefix is
    * zero (one tag is empty); the other row shows its full tag, the
    * tagless one stays bare. */
   test_row_t              src[2];
   pastime_disambig_row_t  rows[2];
   make_row(&src[0], "Crash Bandicoot", NULL,              0);
   make_row(&src[1], "Crash Bandicoot", "(USA) (Beta)",    0);
   to_disambig(rows, src, 2);

   pastime_disambig_run(rows, 2, NULL, NULL);

   ASSERT_STREQ(src[0].display_name, "Crash Bandicoot");
   ASSERT_STREQ(src[1].display_name, "Crash Bandicoot (USA) (Beta)");
   free_row(&src[0]); free_row(&src[1]);
}

/* ---------- cross-source ---------- */

static void test_cross_source_two_rows(void)
{
   /* Same game in two different folders.  Source labels distinguish. */
   fake_sources_t          fs = { { "pcsx_rearmed", "duckstation" }, 2 };
   test_row_t              src[2];
   pastime_disambig_row_t  rows[2];
   make_row(&src[0], "The Raiden Project", "(USA)", 0);
   make_row(&src[1], "The Raiden Project", "(USA)", 1);
   to_disambig(rows, src, 2);

   pastime_disambig_run(rows, 2, fake_resolve_label, &fs);

   ASSERT_STREQ(src[0].display_name, "The Raiden Project (pcsx_rearmed)");
   ASSERT_STREQ(src[1].display_name, "The Raiden Project (duckstation)");
   free_row(&src[0]); free_row(&src[1]);
}

static void test_cross_source_resolver_returns_null(void)
{
   /* When the resolver can't resolve a row's source label, that row
    * gets no source qualifier — but other rows with resolvable labels
    * still do.  Caller's prior label survives. */
   fake_sources_t          fs = { { "pcsx_rearmed", NULL }, 2 };
   test_row_t              src[2];
   pastime_disambig_row_t  rows[2];
   make_row(&src[0], "Game", "(USA)", 0);
   make_row(&src[1], "Game", "(USA)", 1);
   to_disambig(rows, src, 2);

   pastime_disambig_run(rows, 2, fake_resolve_label, &fs);

   ASSERT_STREQ(src[0].display_name, "Game (pcsx_rearmed)");
   ASSERT_STREQ(src[1].display_name, "Game");
   free_row(&src[0]); free_row(&src[1]);
}

/* ---------- mixed-run (the SME-flagged regression) ---------- */

static void test_mixed_run_same_source_subdupes(void)
{
   /* Three rows: two from source 0 (tags differ) + one from source 1.
    * Source label disambiguates the cross-source axis, but rows 0 and
    * 1 share source 0 and would silently render identically without
    * the second-pass tag-tail differential. */
   fake_sources_t          fs = { { "pcsx_rearmed", "duckstation" }, 2 };
   test_row_t              src[3];
   pastime_disambig_row_t  rows[3];
   make_row(&src[0], "Crash", "(USA)",          0);
   make_row(&src[1], "Crash", "(USA) (Beta)",   0);
   make_row(&src[2], "Crash", "(USA)",          1);
   to_disambig(rows, src, 3);

   pastime_disambig_run(rows, 3, fake_resolve_label, &fs);

   /* Same-source pair: source label + tag-tail differential.  Bare
    * row has empty tail, so it just gets the source label. */
   ASSERT_STREQ(src[0].display_name, "Crash (pcsx_rearmed)");
   ASSERT_STREQ(src[1].display_name, "Crash (pcsx_rearmed) (Beta)");
   ASSERT_STREQ(src[2].display_name, "Crash (duckstation)");
   free_row(&src[0]); free_row(&src[1]); free_row(&src[2]);
}

/* ---------- non-collisions ---------- */

static void test_singletons_unchanged(void)
{
   test_row_t              src[3];
   pastime_disambig_row_t  rows[3];
   make_row(&src[0], "Castlevania",  "(USA)",       0);
   make_row(&src[1], "Donkey Kong",  "(USA)",       0);
   make_row(&src[2], "Earthbound",   "(USA)",       0);
   to_disambig(rows, src, 3);

   pastime_disambig_run(rows, 3, NULL, NULL);

   ASSERT_STREQ(src[0].display_name, "Castlevania");
   ASSERT_STREQ(src[1].display_name, "Donkey Kong");
   ASSERT_STREQ(src[2].display_name, "Earthbound");
   free_row(&src[0]); free_row(&src[1]); free_row(&src[2]);
}

static void test_empty_input_no_crash(void)
{
   pastime_disambig_run(NULL, 0,    NULL, NULL);
   pastime_disambig_run(NULL, 5,    NULL, NULL);  /* NULL rows + nonzero n */
   {
      pastime_disambig_row_t row;
      char *name = strdup("Solo");
      row.display_name = &name;
      row.tag          = NULL;
      row.source_idx   = 0;
      pastime_disambig_run(&row, 1, NULL, NULL);  /* singleton */
      ASSERT_STREQ(name, "Solo");
      free(name);
   }
   /* If we got here without crashing, count it. */
   g_pass++;
}

/* ---------- runner ---------- */

int main(void)
{
   RUN_TEST(test_intra_two_rows_tag_diff);
   RUN_TEST(test_intra_three_rows_demo_progression);
   RUN_TEST(test_intra_region_only);
   RUN_TEST(test_intra_disc_mid_block_no_split);
   RUN_TEST(test_intra_one_row_no_tag);
   RUN_TEST(test_cross_source_two_rows);
   RUN_TEST(test_cross_source_resolver_returns_null);
   RUN_TEST(test_mixed_run_same_source_subdupes);
   RUN_TEST(test_singletons_unchanged);
   RUN_TEST(test_empty_input_no_crash);

   fprintf(stderr, "\nPASS: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
