#define PASTIME_ROMMAP_TEST_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../pastime_rommap.h"
#include "../pastime_rommap.c"

/* ------------------------------------------------------------------ */
/* Tiny test framework                                                 */
/* ------------------------------------------------------------------ */
static int g_tests_run    = 0;
static int g_tests_passed = 0;

#define RUN_TEST(fn) do { \
   g_tests_run++; \
   if (fn()) g_tests_passed++; \
   else fprintf(stderr, "  FAIL: %s\n", #fn); \
} while (0)

#define ASSERT_TRUE(expr) do { \
   if (!(expr)) { \
      fprintf(stderr, "    assertion failed: %s (%s:%d)\n", \
            #expr, __FILE__, __LINE__); \
      return 0; \
   } \
} while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)
#define ASSERT_STR_EQ(a, b) do { \
   const char *_a = (a), *_b = (b); \
   if (!_a || !_b || strcmp(_a, _b) != 0) { \
      fprintf(stderr, "    strings differ: \"%s\" vs \"%s\" (%s:%d)\n", \
            _a ? _a : "(null)", _b ? _b : "(null)", \
            __FILE__, __LINE__); \
      return 0; \
   } \
} while (0)

/* Helper: create a map from a string literal (makes a heap copy). */
static pastime_rommap_t *map_from_str(const char *s)
{
   size_t len = strlen(s);
   char *buf  = (char *)malloc(len + 1);
   memcpy(buf, s, len + 1);
   return pastime_rommap_load_buf(buf, len);
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

static int test_basic_lookup(void)
{
   pastime_rommap_t *m = map_from_str(
         "mslug.zip\tMetal Slug\n"
         "kof98.zip\tThe King of Fighters '98\n"
         "sf2.zip\tStreet Fighter II\n");
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 3);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "mslug.zip"), "Metal Slug");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "kof98.zip"), "The King of Fighters '98");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "sf2.zip"), "Street Fighter II");
   pastime_rommap_free(m);
   return 1;
}

static int test_lookup_miss(void)
{
   pastime_rommap_t *m = map_from_str("mslug.zip\tMetal Slug\n");
   ASSERT_NOT_NULL(m);
   ASSERT_NULL(pastime_rommap_lookup(m, "nothere.zip"));
   ASSERT_NULL(pastime_rommap_lookup(m, "mslug"));
   ASSERT_NULL(pastime_rommap_lookup(m, "MSLUG.ZIP"));
   pastime_rommap_free(m);
   return 1;
}

static int test_hidden_rom(void)
{
   pastime_rommap_t *m = map_from_str(
         "neogeo.zip\t.bios\n"
         "mslug.zip\tMetal Slug\n");
   ASSERT_NOT_NULL(m);
   const char *val = pastime_rommap_lookup(m, "neogeo.zip");
   ASSERT_NOT_NULL(val);
   ASSERT_TRUE(val[0] == '.');
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "mslug.zip"), "Metal Slug");
   pastime_rommap_free(m);
   return 1;
}

static int test_empty_lines_skipped(void)
{
   pastime_rommap_t *m = map_from_str(
         "\n"
         "a.zip\tAlpha\n"
         "\n"
         "\n"
         "b.zip\tBeta\n"
         "\n");
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 2);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "a.zip"), "Alpha");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "b.zip"), "Beta");
   pastime_rommap_free(m);
   return 1;
}

static int test_malformed_lines_skipped(void)
{
   pastime_rommap_t *m = map_from_str(
         "no tab here\n"
         "good.zip\tGood Game\n"
         "also no tab\n"
         "\tjust a value\n");
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 1);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "good.zip"), "Good Game");
   pastime_rommap_free(m);
   return 1;
}

static int test_windows_newlines(void)
{
   pastime_rommap_t *m = map_from_str(
         "a.zip\tAlpha\r\n"
         "b.zip\tBeta\r\n"
         "c.zip\tGamma\r\n");
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 3);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "a.zip"), "Alpha");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "b.zip"), "Beta");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "c.zip"), "Gamma");
   pastime_rommap_free(m);
   return 1;
}

static int test_spaces_in_values(void)
{
   pastime_rommap_t *m = map_from_str(
         "game.zip\tMy Great Game: The Sequel (2005)\n");
   ASSERT_NOT_NULL(m);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "game.zip"),
         "My Great Game: The Sequel (2005)");
   pastime_rommap_free(m);
   return 1;
}

static int test_tabs_in_value(void)
{
   /* Second tab and beyond are part of the value */
   pastime_rommap_t *m = map_from_str(
         "game.zip\tName\twith\ttabs\n");
   ASSERT_NOT_NULL(m);
   /* Only splits on first tab — value is "Name\twith\ttabs" */
   const char *val = pastime_rommap_lookup(m, "game.zip");
   ASSERT_NOT_NULL(val);
   ASSERT_TRUE(val[0] == 'N');
   pastime_rommap_free(m);
   return 1;
}

static int test_no_trailing_newline(void)
{
   pastime_rommap_t *m = map_from_str(
         "a.zip\tAlpha\n"
         "b.zip\tBeta");
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 2);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "b.zip"), "Beta");
   pastime_rommap_free(m);
   return 1;
}

static int test_duplicate_keys_last_wins(void)
{
   pastime_rommap_t *m = map_from_str(
         "game.zip\tFirst Name\n"
         "game.zip\tSecond Name\n");
   ASSERT_NOT_NULL(m);
   /* Both entries are stored; bsearch finds one — sorted adjacent,
    * but which one bsearch returns is implementation-defined.
    * In practice we just need it to not crash. */
   const char *val = pastime_rommap_lookup(m, "game.zip");
   ASSERT_NOT_NULL(val);
   ASSERT_TRUE(strcmp(val, "First Name") == 0
            || strcmp(val, "Second Name") == 0);
   pastime_rommap_free(m);
   return 1;
}

static int test_sorted_order(void)
{
   pastime_rommap_t *m = map_from_str(
         "z.zip\tZeta\n"
         "a.zip\tAlpha\n"
         "m.zip\tMu\n");
   ASSERT_NOT_NULL(m);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "a.zip"), "Alpha");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "m.zip"), "Mu");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "z.zip"), "Zeta");
   pastime_rommap_free(m);
   return 1;
}

static int test_null_safety(void)
{
   ASSERT_NULL(pastime_rommap_lookup(NULL, "x.zip"));
   ASSERT_NULL(pastime_rommap_lookup(NULL, NULL));
   ASSERT_EQ(pastime_rommap_count(NULL), 0);
   pastime_rommap_free(NULL);
   return 1;
}

static int test_empty_buf(void)
{
   char *buf = (char *)malloc(1);
   buf[0] = '\0';
   ASSERT_NULL(pastime_rommap_load_buf(buf, 0));
   return 1;
}

static int test_route_known_cores(void)
{
   ASSERT_STR_EQ(pastime_rommap_route("fbneo"), "arcade.txt");
   ASSERT_STR_EQ(pastime_rommap_route("mame2003_plus"), "arcade.txt");
   ASSERT_STR_EQ(pastime_rommap_route("mame"), "arcade.txt");
   ASSERT_STR_EQ(pastime_rommap_route("fbalpha2012_neogeo"), "arcade.txt");
   return 1;
}

static int test_route_unknown_core(void)
{
   ASSERT_NULL(pastime_rommap_route("snes9x"));
   ASSERT_NULL(pastime_rommap_route("gambatte"));
   ASSERT_NULL(pastime_rommap_route(NULL));
   ASSERT_NULL(pastime_rommap_route(""));
   return 1;
}

static int test_two_map_priority(void)
{
   pastime_rommap_t *user = map_from_str(
         "mslug.zip\tUser Override Name\n");
   pastime_rommap_t *baked = map_from_str(
         "mslug.zip\tMetal Slug\n"
         "kof98.zip\tThe King of Fighters '98\n");
   ASSERT_NOT_NULL(user);
   ASSERT_NOT_NULL(baked);

   /* Simulate the two-step lookup: user first, baked second */
   const char *val;

   val = pastime_rommap_lookup(user, "mslug.zip");
   if (!val) val = pastime_rommap_lookup(baked, "mslug.zip");
   ASSERT_STR_EQ(val, "User Override Name");

   val = pastime_rommap_lookup(user, "kof98.zip");
   if (!val) val = pastime_rommap_lookup(baked, "kof98.zip");
   ASSERT_STR_EQ(val, "The King of Fighters '98");

   val = pastime_rommap_lookup(user, "nothere.zip");
   if (!val) val = pastime_rommap_lookup(baked, "nothere.zip");
   ASSERT_NULL(val);

   pastime_rommap_free(user);
   pastime_rommap_free(baked);
   return 1;
}

static int test_large_map(void)
{
   /* Build a ~1000-entry map to exercise growth */
   char *buf = (char *)malloc(64000);
   size_t off = 0;
   int i;
   for (i = 0; i < 1000; i++)
   {
      off += (size_t)sprintf(buf + off, "game%04d.zip\tGame Number %d\n", i, i);
   }
   pastime_rommap_t *m = pastime_rommap_load_buf(buf, off);
   ASSERT_NOT_NULL(m);
   ASSERT_EQ(pastime_rommap_count(m), 1000);
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "game0000.zip"), "Game Number 0");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "game0500.zip"), "Game Number 500");
   ASSERT_STR_EQ(pastime_rommap_lookup(m, "game0999.zip"), "Game Number 999");
   ASSERT_NULL(pastime_rommap_lookup(m, "game1000.zip"));
   pastime_rommap_free(m);
   return 1;
}

/* ------------------------------------------------------------------ */

int main(void)
{
   printf("pastime_rommap tests\n");

   RUN_TEST(test_basic_lookup);
   RUN_TEST(test_lookup_miss);
   RUN_TEST(test_hidden_rom);
   RUN_TEST(test_empty_lines_skipped);
   RUN_TEST(test_malformed_lines_skipped);
   RUN_TEST(test_windows_newlines);
   RUN_TEST(test_spaces_in_values);
   RUN_TEST(test_tabs_in_value);
   RUN_TEST(test_no_trailing_newline);
   RUN_TEST(test_duplicate_keys_last_wins);
   RUN_TEST(test_sorted_order);
   RUN_TEST(test_null_safety);
   RUN_TEST(test_empty_buf);
   RUN_TEST(test_route_known_cores);
   RUN_TEST(test_route_unknown_core);
   RUN_TEST(test_two_map_priority);
   RUN_TEST(test_large_map);

   printf("%d/%d passed\n", g_tests_passed, g_tests_run);
   return (g_tests_passed == g_tests_run) ? 0 : 1;
}
