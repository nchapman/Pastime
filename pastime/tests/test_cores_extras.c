/* Unit tests for pastime/pastime_cores_extras.c.
 *
 * Tests the static curated table — lookup behavior + invariants every
 * entry must satisfy (non-empty fields, https url, distinct idents).
 * The async install path that consumes these entries lives in
 * pastime_cores.c and is verified end-to-end on Android.
 */

#include <stdio.h>
#include <string.h>

#include "../pastime_cores_extras.h"

static int g_pass;
static int g_fail;

#define ASSERT_TRUE(cond) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; \
      fprintf(stderr, "    FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define ASSERT_NULL(p) ASSERT_TRUE((p) == NULL)
#define ASSERT_NOT_NULL(p) ASSERT_TRUE((p) != NULL)

#define RUN_TEST(fn) do { \
   int p0 = g_pass, f0 = g_fail; \
   fn(); \
   fprintf(stderr, "  %-40s %d/%d\n", #fn, \
         g_pass - p0, (g_pass - p0) + (g_fail - f0)); \
} while (0)

static void test_lookup_known_ident(void)
{
   const pastime_cores_extra_t *e = pastime_cores_extras_lookup("fake08");
   ASSERT_NOT_NULL(e);
   if (!e)
      return;
   ASSERT_TRUE(strcmp(e->ident, "fake08") == 0);
   ASSERT_TRUE(e->zip_url && strncmp(e->zip_url, "https://", 8) == 0);
   ASSERT_TRUE(e->zip_so_path && *e->zip_so_path);
}

static void test_lookup_unknown_ident(void)
{
   ASSERT_NULL(pastime_cores_extras_lookup("nonexistent"));
   ASSERT_NULL(pastime_cores_extras_lookup("fake08_libretro")); /* not stripped */
   ASSERT_NULL(pastime_cores_extras_lookup("FAKE08"));          /* case-sensitive */
}

static void test_lookup_null_and_empty(void)
{
   ASSERT_NULL(pastime_cores_extras_lookup(NULL));
   ASSERT_NULL(pastime_cores_extras_lookup(""));
}

/* Every entry in the table must satisfy: ident/url/so non-empty, https
 * scheme, idents unique. */
static void test_table_invariants(void)
{
   unsigned i, j;
   unsigned n = pastime_cores_extras_count();
   ASSERT_TRUE(n >= 1); /* at minimum, fake08 ships today */
   for (i = 0; i < n; i++)
   {
      const pastime_cores_extra_t *a = pastime_cores_extras_at(i);
      ASSERT_NOT_NULL(a);
      if (!a) continue;
      ASSERT_TRUE(a->ident && *a->ident);
      ASSERT_TRUE(a->zip_url && strncmp(a->zip_url, "https://", 8) == 0);
      ASSERT_TRUE(a->zip_so_path && *a->zip_so_path);
      /* zip_info_path may be NULL — explicitly allowed. */
      for (j = i + 1; j < n; j++)
      {
         const pastime_cores_extra_t *b = pastime_cores_extras_at(j);
         if (!b || !a->ident || !b->ident) continue;
         ASSERT_TRUE(strcmp(a->ident, b->ident) != 0);
      }
   }
}

static void test_at_out_of_range(void)
{
   unsigned n = pastime_cores_extras_count();
   ASSERT_NULL(pastime_cores_extras_at(n));
   ASSERT_NULL(pastime_cores_extras_at(n + 100));
}

int main(void)
{
   fprintf(stderr, "test_cores_extras\n");
   RUN_TEST(test_lookup_known_ident);
   RUN_TEST(test_lookup_unknown_ident);
   RUN_TEST(test_lookup_null_and_empty);
   RUN_TEST(test_table_invariants);
   RUN_TEST(test_at_out_of_range);
   fprintf(stderr, "  total: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail == 0 ? 0 : 1;
}
